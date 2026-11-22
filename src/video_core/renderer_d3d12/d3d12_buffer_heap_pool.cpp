// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/thread.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_buffer_heap_pool.h"
#include "video_core/renderer_d3d12/d3d12_heap_growth_policy.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"

namespace D3D12 {

using namespace Common::Literals;

namespace {
std::atomic_bool buffer_heaps{true};

/// Heaps are made at this size; buffers over MAX_PLACED stay committed (rare, and a heap they
/// share would be held by whichever of them lives longest).
constexpr u64 HEAP_SIZE = 32_MiB;
constexpr u64 MAX_PLACED = 4_MiB;
constexpr u64 ALIGNMENT = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
/// With less free than this left in the heaps, the next heap is made ahead on another thread.
constexpr u64 LOW_FREE = 16_MiB;
/// An empty heap is released after staying empty this many frames (one is kept).
constexpr u64 EMPTY_FRAMES = 600;
constexpr u64 TRIM_EVERY = 60;

/// Identifies the range token in a placed buffer's private data.
constexpr GUID RANGE_TOKEN_GUID = {
    0x5f3b6a21, 0x8c4e, 0x4d1a, {0x9b, 0x77, 0x2e, 0x61, 0xc0, 0x4f, 0xa8, 0x13}};

HeapGrowth::Budget AppBudget(const CacheMemorySnapshot& memory) {
    return {memory.app_used, memory.app_limit};
}

/// Growth must leave the app its exhaustion reserve and the GPU budget room for the heap.
bool CanGrow(const CacheMemorySnapshot& memory, u64 bytes) {
    if (!memory.Known()) return true;
    if (HeapGrowth::Exhausted(bytes, AppBudget(memory))) return false;
    return !memory.gpu_budget || memory.gpu_used + bytes < memory.gpu_budget;
}

ComPtr<ID3D12Heap> MakeHeap(ID3D12Device* device) {
    D3D12_HEAP_DESC desc{
        .SizeInBytes = HEAP_SIZE,
        .Properties = {.Type = D3D12_HEAP_TYPE_DEFAULT},
        .Alignment = ALIGNMENT,
        .Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | NotZeroedFlag(device),
    };
    ComPtr<ID3D12Heap> heap;
    HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&heap));
    if (RefuseNotZeroed(desc.Flags, hr)) {
        desc.Flags &= ~D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        hr = device->CreateHeap(&desc, IID_PPV_ARGS(&heap));
    }
    return SUCCEEDED(hr) ? heap : ComPtr<ID3D12Heap>{};
}
} // Anonymous namespace

void SetBufferHeaps(bool enabled) {
    buffer_heaps.store(enabled, std::memory_order_relaxed);
}

struct BufferHeapPool::State {
    struct Block {
        ComPtr<ID3D12Heap> heap;
        u64 used{};
        std::map<u64, u64> free_ranges; ///< offset -> bytes
        u64 empty_since{}; ///< frame a trim pass first saw it empty; 0 while in use
        bool spare{};      ///< made ahead and not placed into yet
    };
    struct Reservation {
        size_t block{SIZE_MAX};
        u64 offset{};
        ID3D12Heap* heap{};
    };

    mutable std::mutex mutex;
    std::vector<Block> blocks;
    u64 heap_bytes{};
    u64 live_bytes{};
    u64 peak_live_bytes{};
    u64 placed{};
    u64 too_large{};
    u64 refused{};
    u64 failed{};
    u64 grown_inline{};
    u64 spares_made{};
    u64 spares_used{};
    u64 released{};

    /// Caller holds the mutex. The fullest heap with room first, so the sparse ones can drain,
    /// and the tightest range in it.
    Reservation Reserve(u64 bytes) {
        size_t best = SIZE_MAX;
        u64 best_offset{};
        u64 best_free = UINT64_MAX;
        u64 best_waste = UINT64_MAX;
        for (size_t index = 0; index < blocks.size(); ++index) {
            const Block& block = blocks[index];
            if (!block.heap) continue;
            const u64 block_free = HEAP_SIZE - block.used;
            if (block_free < bytes || block_free > best_free) continue;
            for (const auto& [offset, range] : block.free_ranges) {
                if (range < bytes) continue;
                const u64 waste = range - bytes;
                if (block_free < best_free || waste < best_waste) {
                    best = index;
                    best_offset = offset;
                    best_free = block_free;
                    best_waste = waste;
                }
            }
        }
        if (best == SIZE_MAX) return {};
        Take(best, best_offset, bytes);
        return {best, best_offset, blocks[best].heap.Get()};
    }

    /// Caller holds the mutex.
    void Take(size_t index, u64 offset, u64 bytes) {
        Block& block = blocks[index];
        const auto range = block.free_ranges.find(offset);
        const u64 range_bytes = range->second;
        block.free_ranges.erase(range);
        if (range_bytes > bytes) block.free_ranges.emplace(offset + bytes, range_bytes - bytes);
        if (block.spare) {
            block.spare = false;
            ++spares_used;
        }
        block.used += bytes;
        block.empty_since = 0;
        live_bytes += bytes;
        peak_live_bytes = std::max(peak_live_bytes, live_bytes);
        ++placed;
    }

    /// Caller holds the mutex. Slots of released heaps are reused: no token points at them.
    size_t Insert(ComPtr<ID3D12Heap> heap, bool spare) {
        Block block{.heap = std::move(heap), .spare = spare};
        block.free_ranges.emplace(0, HEAP_SIZE);
        heap_bytes += HEAP_SIZE;
        const auto vacant = std::ranges::find_if(blocks, [](const Block& b) { return !b.heap; });
        if (vacant == blocks.end()) {
            blocks.push_back(std::move(block));
            return blocks.size() - 1;
        }
        *vacant = std::move(block);
        return static_cast<size_t>(vacant - blocks.begin());
    }

    /// Caller holds the mutex.
    [[nodiscard]] bool HasEmptyHeap() const {
        return std::ranges::any_of(blocks, [](const Block& b) { return b.heap && b.used == 0; });
    }

    /// Caller holds the mutex.
    [[nodiscard]] bool LowOnFree() const {
        return heap_bytes - live_bytes < LOW_FREE && !HasEmptyHeap();
    }

    void Free(size_t index, u64 offset, u64 bytes) {
        std::scoped_lock lock{mutex};
        if (index >= blocks.size() || !blocks[index].heap) return;
        Block& block = blocks[index];
        block.used -= bytes;
        live_bytes -= bytes;
        auto& ranges = block.free_ranges;
        u64 size = bytes;
        auto next = ranges.lower_bound(offset);
        if (next != ranges.begin()) {
            const auto prev = std::prev(next);
            if (prev->first + prev->second == offset) {
                offset = prev->first;
                size += prev->second;
                ranges.erase(prev);
            }
        }
        next = ranges.lower_bound(offset);
        if (next != ranges.end() && offset + size == next->first) {
            size += next->second;
            ranges.erase(next);
        }
        ranges.emplace(offset, size);
    }
};

namespace {
/// Kept in a placed buffer's private data: the buffer's destruction releases it, and its range
/// goes back to the pool.
class RangeToken final : public IUnknown {
public:
    RangeToken(std::shared_ptr<BufferHeapPool::State> state_, size_t block_, u64 offset_,
               u64 bytes_)
        : state{std::move(state_)}, block{block_}, offset{offset_}, bytes{bytes_} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) delete this;
        return left;
    }

private:
    ~RangeToken() {
        state->Free(block, offset, bytes);
    }

    std::shared_ptr<BufferHeapPool::State> state;
    size_t block;
    u64 offset;
    u64 bytes;
    std::atomic<ULONG> refs{1};
};
} // Anonymous namespace

struct BufferHeapPool::SpareWorker {
    std::mutex mutex;
    std::condition_variable_any wake;
    bool wanted{};
    std::jthread thread; ///< last: stopped before the members it uses

    void Request() {
        {
            std::scoped_lock lock{mutex};
            wanted = true;
        }
        wake.notify_one();
    }

    /// CreateHeap waits while the system gets the pages ready: done here, the GPU thread finds
    /// the heap ready. Only while memory is plentiful (room for two heaps past the reserve).
    static void Make(const Device& device, State& state) {
        {
            std::scoped_lock lock{state.mutex};
            if (!state.LowOnFree()) return;
        }
        if (!CanGrow(device.QueryCacheMemoryPressure(), 2 * HEAP_SIZE)) return;
        ComPtr<ID3D12Heap> heap = MakeHeap(device.Get());
        if (!heap) return;
        std::scoped_lock lock{state.mutex};
        // Releases or a growth on the GPU thread made room meanwhile: not needed.
        if (!state.LowOnFree()) return;
        state.Insert(std::move(heap), true);
        ++state.spares_made;
    }
};

BufferHeapPool::BufferHeapPool(const Device& device_)
    : device{device_}, state{std::make_shared<State>()},
      spare_worker{std::make_unique<SpareWorker>()} {
    spare_worker->thread = std::jthread{[this](std::stop_token stop) {
        Common::SetCurrentThreadName("D3D12BufferHeaps");
        auto& worker = *spare_worker;
        while (true) {
            {
                std::unique_lock lock{worker.mutex};
                if (!worker.wake.wait(lock, stop, [&] { return worker.wanted; })) return;
                worker.wanted = false;
            }
            SpareWorker::Make(device, *state);
        }
    }};
    // The first heap too: the first buffers of a game arrive in a burst.
    if (buffer_heaps.load(std::memory_order_relaxed)) spare_worker->Request();
}

BufferHeapPool::~BufferHeapPool() {
    spare_worker.reset();
}

ComPtr<ID3D12Resource> BufferHeapPool::Create(u64 size) {
    if (!buffer_heaps.load(std::memory_order_relaxed) || size == 0) return {};
    if (size > MAX_PLACED) {
        std::scoped_lock lock{state->mutex};
        ++state->too_large;
        return {};
    }
    const u64 bytes = Common::AlignUp(size, ALIGNMENT);
    State::Reservation reservation;
    bool want_spare = false;
    {
        std::scoped_lock lock{state->mutex};
        reservation = state->Reserve(bytes);
        want_spare = reservation.heap && state->LowOnFree();
    }
    if (!reservation.heap) {
        // No room: grow here, unless memory is short (a committed buffer takes only its size).
        if (!CanGrow(device.QueryCacheMemoryPressure(), HEAP_SIZE)) {
            std::scoped_lock lock{state->mutex};
            ++state->refused;
            return {};
        }
        ComPtr<ID3D12Heap> heap = MakeHeap(device.Get());
        std::scoped_lock lock{state->mutex};
        if (!heap) {
            ++state->failed;
            return {};
        }
        const size_t index = state->Insert(std::move(heap), false);
        ++state->grown_inline;
        VideoCore::Perf::Add(VideoCore::Perf::Counter::BufferHeapsGrown, 1);
        state->Take(index, 0, bytes);
        reservation = {index, 0, state->blocks[index].heap.Get()};
        want_spare = state->LowOnFree();
    }
    if (want_spare) spare_worker->Request();

    // The reserved range keeps the heap from being released while the buffer is placed in it.
    const D3D12_RESOURCE_DESC desc = BufferDesc(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = device.Get()->CreatePlacedResource(reservation.heap, reservation.offset, &desc,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&buffer));
    if (SUCCEEDED(hr)) {
        auto* const token = new RangeToken(state, reservation.block, reservation.offset, bytes);
        hr = buffer->SetPrivateDataInterface(RANGE_TOKEN_GUID, token);
        if (FAILED(hr)) buffer.Reset(); // destroyed before the token frees its range
        token->Release();
        if (SUCCEEDED(hr)) return buffer;
    } else {
        state->Free(reservation.block, reservation.offset, bytes);
    }
    std::scoped_lock lock{state->mutex};
    --state->placed;
    if (++state->failed <= 4) {
        LOG_WARNING(Render, "D3D12: a buffer of {} bytes could not be placed in a heap ({:#010x}); "
                    "committed instead", size, static_cast<u32>(hr));
    }
    return {};
}

void BufferHeapPool::TickFrame() {
    if (++frame % TRIM_EVERY != 0) return;
    const auto memory = device.QueryCacheMemoryPressure();
    // Short of memory: no empty heap is kept.
    const bool short_of_memory = !CanGrow(memory, 2 * HEAP_SIZE);
    std::scoped_lock lock{state->mutex};
    bool kept = false;
    for (auto& block : state->blocks) {
        if (!block.heap || block.used != 0) continue;
        if (!block.empty_since) block.empty_since = frame;
        if (!short_of_memory && !kept) {
            kept = true;
            continue;
        }
        if (!short_of_memory && frame - block.empty_since < EMPTY_FRAMES) continue;
        block.heap.Reset();
        block.free_ranges.clear();
        block.spare = false;
        block.empty_since = 0;
        state->heap_bytes -= HEAP_SIZE;
        ++state->released;
    }
}

std::string BufferHeapPool::Report() const {
    std::scoped_lock lock{state->mutex};
    size_t heaps = 0;
    for (const auto& block : state->blocks) heaps += block.heap != nullptr;
    return fmt::format("buffer heaps {} ({} MiB, {} MiB live, peak {} MiB live); {} buffers "
                       "placed, committed {} too large, {} short of memory, {} failed; heaps made "
                       "{} on the GPU thread, {} ahead ({} used), {} released",
                       heaps, state->heap_bytes / 1_MiB, state->live_bytes / 1_MiB,
                       state->peak_live_bytes / 1_MiB, state->placed, state->too_large,
                       state->refused, state->failed, state->grown_inline, state->spares_made,
                       state->spares_used, state->released);
}

} // namespace D3D12
