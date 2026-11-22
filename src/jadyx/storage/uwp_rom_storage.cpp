// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/storage/uwp_rom_storage.h"
#include "jadyx/storage/storage_path.h"
#include "jadyx/i18n/localization.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>
#include <objbase.h>
#include <fileapifromapp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.System.h>
#include "common/host_stalls.h"
#include "common/logging.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/nro.h"

namespace Jadyx {
namespace {
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using winrt::Windows::Storage::AccessCache::StorageApplicationPermissions;
constexpr unsigned PageSize = 64;
constexpr unsigned MaxSources = 16;
constexpr unsigned MaxEntries = 10000;
std::atomic<bool> storage_read_failure{};
constexpr unsigned ReadChunk = 1024 * 1024;

void EnsureStorageApartment() {
    // Core filesystem workers are ordinary std::threads. Initialize their MTA
    // lazily, once per thread; never block a picker/UI STA on a WinRT .get().
    struct Apartment {
        Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        ~Apartment() { winrt::uninit_apartment(); }
    };
    thread_local Apartment apartment;
}

std::string ErrorText(const winrt::hresult_error& e) {
    return I18n::ToUtf8(I18n::Format(I18n::Text::StorageUnavailable, {e.message()}));
}
void AddError(LibraryScan& scan, std::string_view error) {
    // Bound diagnostic/UI text even if every source is unavailable.
    if (scan.error.size() < 1500) {
        if (!scan.error.empty()) scan.error += " | ";
        scan.error += error;
    }
}
StorageFolder ResolveFolder(const StoragePath& path, std::string_view relative) {
    auto folder = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(
        winrt::to_hstring(path.token)).get();
    while (!relative.empty()) {
        const auto slash = relative.find('/');
        folder = folder.GetFolderAsync(winrt::to_hstring(relative.substr(0, slash))).get();
        if (slash == std::string_view::npos) break;
        relative.remove_prefix(slash + 1);
    }
    return folder;
}

std::atomic<bool> rom_cache_enabled{true};

/// Reads [offset, offset + length) of an open file handle at that position. The file pointer
/// is set and read under the caller's lock, if the handle is shared.
bool ReadAt(HANDLE handle, u8* data, size_t length, u64 offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    size_t done = 0;
    while (done < length) {
        const DWORD count = static_cast<DWORD>(std::min<size_t>(ReadChunk, length - done));
        DWORD read{};
        if (!ReadFile(handle, data + done, count, &read, nullptr) || read == 0) {
            return false;
        }
        done += read;
    }
    return true;
}

/// Games on external storage (a USB hard drive) are read through this: blocks already read
/// come from memory, and a reader going through a file in order has the next blocks read ahead
/// by a background thread. Metroid Prime Remastered on a USB hard drive: reads took 1 to 110 ms
/// each, 2.4 s of the hitches over 100 ms in 39 minutes. boot.cfg rom_cache=0 turns it off.
class RomReadCache {
public:
    static constexpr size_t BLOCK = 256 * 1024;
    static constexpr size_t CAPACITY = 192ULL << 20;
    /// While the app has less than this free, the cache keeps a quarter of its capacity.
    static constexpr u64 LOW_FREE = 384ULL << 20;
    static constexpr u64 PREFETCH_BLOCKS = 16; ///< 4 MiB ahead of an in-order reader
    static constexpr size_t MAX_QUEUED = 8;

    static RomReadCache& Instance() {
        static RomReadCache cache;
        return cache;
    }

    /// One id per file path, shared by every handle opened on it.
    u32 FileId(const std::wstring& file_path) {
        std::scoped_lock lock{mutex};
        return file_ids.try_emplace(file_path, static_cast<u32>(file_ids.size())).first->second;
    }

    /// Fills data with [offset, offset + length) of the file, from memory where it can and
    /// through read_disk (positional) for the rest. False when a disk read failed.
    bool Read(u32 file, u64 file_size, u8* data, size_t length, u64 offset,
              const std::function<bool(u8*, size_t, u64)>& read_disk) {
        requests.fetch_add(1, std::memory_order_relaxed);
        const u64 end = offset + length;
        u64 block = offset / BLOCK;
        const u64 last = (end - 1) / BLOCK;
        while (block <= last) {
            u64 run = 0;
            {
                std::unique_lock lock{mutex};
                auto it = blocks.find(Key{file, block});
                if (it != blocks.end() && !it->second.ready) {
                    // Being read ahead: wait for it rather than seek the same disk twice.
                    waits.fetch_add(1, std::memory_order_relaxed);
                    cv.wait_for(lock, std::chrono::seconds{2}, [&] {
                        it = blocks.find(Key{file, block});
                        return it == blocks.end() || it->second.ready;
                    });
                    if (it != blocks.end() && !it->second.ready) {
                        it = blocks.end(); // Still not there: read it here.
                    }
                }
                if (it != blocks.end() && it->second.ready) {
                    Block& cached = it->second;
                    CopyOut(block, cached.bytes.get(), cached.size, data, offset, end);
                    lru.splice(lru.begin(), lru, cached.lru);
                    hits.fetch_add(1, std::memory_order_relaxed);
                    if (std::exchange(cached.prefetched, false)) {
                        prefetch_hits.fetch_add(1, std::memory_order_relaxed);
                    }
                    ++block;
                    continue;
                }
                // The run of blocks from here that nobody has or is reading.
                while (block + run <= last && !blocks.contains(Key{file, block + run})) {
                    blocks.emplace(Key{file, block + run}, Block{});
                    ++run;
                }
                if (run == 0) {
                    // A block that was being read ahead and did not come: read it alone.
                    run = 1;
                }
            }
            const u64 first_byte = block * BLOCK;
            const size_t run_bytes =
                static_cast<size_t>(std::min<u64>(run * BLOCK, file_size - first_byte));
            // A run inside the request is read straight into it; one sticking out of it goes
            // through a buffer.
            const bool direct = first_byte >= offset && first_byte + run_bytes <= end;
            std::vector<u8> buffer(direct ? 0 : run_bytes);
            u8* const target = direct ? data + (first_byte - offset) : buffer.data();
            const auto disk_begin = std::chrono::steady_clock::now();
            const bool read = read_disk(target, run_bytes, first_byte);
            disk_reads.fetch_add(1, std::memory_order_relaxed);
            disk_bytes.fetch_add(run_bytes, std::memory_order_relaxed);
            disk_us.fetch_add(static_cast<u64>(
                                  std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - disk_begin)
                                      .count()),
                              std::memory_order_relaxed);
            misses.fetch_add(run, std::memory_order_relaxed);
            Insert(file, block, run, target, run_bytes, read, false);
            if (!read) {
                return false;
            }
            for (u64 i = 0; i < run && !direct; ++i) {
                const size_t in_run = static_cast<size_t>(i * BLOCK);
                CopyOut(block + i, target + in_run,
                        static_cast<u32>(std::min<size_t>(BLOCK, run_bytes - in_run)), data,
                        offset, end);
            }
            block += run;
        }
        MaybeLog();
        return true;
    }

    /// Reads [first, first + count) blocks of the file ahead, on the background thread, with a
    /// handle of its own on file_path.
    void Prefetch(u32 file, const std::wstring& file_path, u64 file_size, u64 first,
                  u64 count) {
        const u64 file_blocks = (file_size + BLOCK - 1) / BLOCK;
        if (first >= file_blocks) {
            return;
        }
        count = std::min(count, file_blocks - first);
        {
            std::scoped_lock lock{mutex};
            // Nothing to do when the first and last are there or on their way.
            if (blocks.contains(Key{file, first}) &&
                blocks.contains(Key{file, first + count - 1})) {
                return;
            }
            if (queue.size() >= MAX_QUEUED) {
                return;
            }
            queue.push_back({file, file_path, file_size, first, count});
            if (!worker.joinable()) {
                worker = std::jthread{[this](std::stop_token stop) { Work(stop); }};
            }
        }
        cv.notify_all();
    }

private:
    struct Key {
        u32 file;
        u64 block;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return std::hash<u64>{}((key.block * 0x9E3779B97F4A7C15ULL) ^ key.file);
        }
    };
    struct Block {
        std::unique_ptr<u8[]> bytes;
        u32 size{};
        bool ready{};
        bool prefetched{}; ///< read ahead and not asked for yet
        std::list<Key>::iterator lru;
    };
    struct Request {
        u32 file;
        std::wstring path;
        u64 file_size;
        u64 first;
        u64 count;
    };

    RomReadCache() = default;
    ~RomReadCache() {
        if (worker.joinable()) {
            worker.request_stop();
            cv.notify_all();
            worker.join();
        }
    }

    static void CopyOut(u64 block, const u8* bytes, u32 size, u8* data, u64 offset, u64 end) {
        const u64 block_begin = block * BLOCK;
        const u64 from = std::max(offset, block_begin);
        const u64 to = std::min(end, block_begin + size);
        if (to > from) {
            std::memcpy(data + (from - offset), bytes + (from - block_begin),
                        static_cast<size_t>(to - from));
        }
    }

    /// Fills the placeholders of [block, block + run) from buffer, or drops them if the read
    /// failed, and wakes whoever waits for them.
    void Insert(u32 file, u64 block, u64 run, const u8* bytes, size_t bytes_size, bool read,
                bool prefetched) {
        {
            std::scoped_lock lock{mutex};
            for (u64 i = 0; i < run; ++i) {
                const auto it = blocks.find(Key{file, block + i});
                if (it == blocks.end() || it->second.ready) {
                    continue;
                }
                if (!read) {
                    blocks.erase(it);
                    continue;
                }
                const size_t in_run = static_cast<size_t>(i * BLOCK);
                const u32 block_size =
                    static_cast<u32>(std::min<size_t>(BLOCK, bytes_size - in_run));
                Block& entry = it->second;
                entry.bytes = std::make_unique<u8[]>(block_size);
                std::memcpy(entry.bytes.get(), bytes + in_run, block_size);
                entry.size = block_size;
                entry.ready = true;
                entry.prefetched = prefetched;
                lru.push_front(Key{file, block + i});
                entry.lru = lru.begin();
                cached_bytes += block_size;
            }
            Evict();
        }
        cv.notify_all();
    }

    /// Drops the least recently used blocks over the capacity, a quarter of it while the app
    /// is short of memory. Under the lock.
    void Evict() {
        const auto now = std::chrono::steady_clock::now();
        if (now - memory_checked > std::chrono::seconds{2}) {
            memory_checked = now;
            try {
                EnsureStorageApartment();
                const u64 limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
                const u64 usage = winrt::Windows::System::MemoryManager::AppMemoryUsage();
                short_of_memory = limit != 0 && (usage >= limit || limit - usage < LOW_FREE);
            } catch (...) {
                short_of_memory = false;
            }
        }
        const size_t capacity = short_of_memory ? CAPACITY / 4 : CAPACITY;
        while (cached_bytes > capacity && !lru.empty()) {
            const auto it = blocks.find(lru.back());
            lru.pop_back();
            if (it != blocks.end()) {
                cached_bytes -= it->second.size;
                blocks.erase(it);
            }
        }
    }

    void Work(std::stop_token stop) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::unordered_map<std::wstring, HANDLE> handles;
        while (!stop.stop_requested()) {
            Request request;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] { return stop.stop_requested() || !queue.empty(); });
                if (stop.stop_requested()) {
                    break;
                }
                request = std::move(queue.front());
                queue.pop_front();
            }
            HANDLE& file_handle = handles[request.path];
            if (!file_handle) {
                file_handle = CreateFile2FromAppW(request.path.c_str(), GENERIC_READ,
                                                  FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            }
            if (file_handle == INVALID_HANDLE_VALUE) {
                continue;
            }
            u64 block = request.first;
            const u64 stop_block = request.first + request.count;
            while (block < stop_block && !stop.stop_requested()) {
                u64 run = 0;
                {
                    std::scoped_lock lock{mutex};
                    while (block < stop_block && blocks.contains(Key{request.file, block})) {
                        ++block;
                    }
                    while (block + run < stop_block &&
                           !blocks.contains(Key{request.file, block + run})) {
                        blocks.emplace(Key{request.file, block + run}, Block{});
                        ++run;
                    }
                }
                if (run == 0) {
                    break;
                }
                const u64 first_byte = block * BLOCK;
                const size_t run_bytes = static_cast<size_t>(
                    std::min<u64>(run * BLOCK, request.file_size - first_byte));
                std::vector<u8> buffer(run_bytes);
                const bool read = ReadAt(file_handle, buffer.data(), run_bytes, first_byte);
                prefetched_blocks.fetch_add(read ? run : 0, std::memory_order_relaxed);
                Insert(request.file, block, run, buffer.data(), run_bytes, read, true);
                block += run;
            }
        }
        for (const auto& entry : handles) {
            if (entry.second && entry.second != INVALID_HANDLE_VALUE) {
                CloseHandle(entry.second);
            }
        }
    }

    void MaybeLog() {
        const u64 now_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        u64 logged_ms = last_log_ms.load(std::memory_order_relaxed);
        if (now_ms - logged_ms < 30'000 ||
            !last_log_ms.compare_exchange_strong(logged_ms, now_ms, std::memory_order_relaxed)) {
            return;
        }
        const u64 reads = disk_reads.load(std::memory_order_relaxed);
        size_t held{};
        bool short_now{};
        {
            std::scoped_lock lock{mutex};
            held = cached_bytes;
            short_now = short_of_memory;
        }
        LOG_INFO(Frontend,
                 "ROM cache: {} reads; blocks {} from memory ({} read ahead), {} from disk; "
                 "{} disk reads, {} MiB, {:.1f} ms each; {} blocks read ahead, {} waits for "
                 "one; {} MiB held{}",
                 requests.load(std::memory_order_relaxed), hits.load(std::memory_order_relaxed),
                 prefetch_hits.load(std::memory_order_relaxed),
                 misses.load(std::memory_order_relaxed), reads,
                 disk_bytes.load(std::memory_order_relaxed) >> 20,
                 reads ? static_cast<double>(disk_us.load(std::memory_order_relaxed)) / 1000.0 /
                             static_cast<double>(reads)
                       : 0.0,
                 prefetched_blocks.load(std::memory_order_relaxed),
                 waits.load(std::memory_order_relaxed), held >> 20,
                 short_now ? " (short of memory)" : "");
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<Key, Block, KeyHash> blocks;
    std::list<Key> lru;
    size_t cached_bytes{};
    std::unordered_map<std::wstring, u32> file_ids;
    std::deque<Request> queue;
    std::chrono::steady_clock::time_point memory_checked{};
    bool short_of_memory{};
    std::atomic<u64> requests{}, hits{}, misses{}, prefetch_hits{}, prefetched_blocks{},
        disk_reads{}, disk_bytes{}, disk_us{}, waits{}, last_log_ms{};
    std::jthread worker; ///< last: stopped before the rest goes
};

class StorageDirectory;
FileSys::VirtualDir MakeDirectory(StorageFolder folder, std::string path);

class StorageGameFile final : public FileSys::VfsFile {
public:
    StorageGameFile(StorageFile file, FileSys::VirtualDir parent, std::string path,
                    bool force_stream = false)
        : item(std::move(file)), parent(std::move(parent)), path(std::move(path)),
          name(winrt::to_string(item.Name())) {
        if (!force_stream && !item.Path().empty()) {
            handle = CreateFile2FromAppW(item.Path().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        OPEN_EXISTING, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                LOG_INFO(Frontend, "ROM storage: direct open failed error={}, trying WinRT: {}",
                         GetLastError(), this->path);
            } else {
                wide_path = std::wstring{item.Path()};
                cache_id = RomReadCache::Instance().FileId(wide_path);
                LARGE_INTEGER bytes{};
                if (!GetFileSizeEx(handle, &bytes) || bytes.QuadPart < 0) {
                    CloseHandle(handle);
                    handle = INVALID_HANDLE_VALUE;
                } else size = static_cast<size_t>(bytes.QuadPart);
            }
        }
        if (handle == INVALID_HANDLE_VALUE) {
            stream = item.OpenReadAsync().get();
            size = static_cast<size_t>(stream.Size());
        }
        LOG_INFO(Frontend, "ROM storage: opened backend={} size={} path={}",
                 handle != INVALID_HANDLE_VALUE ? "FromApp" : "WinRT", size, this->path);
    }
    ~StorageGameFile() override {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (stream) { try { stream.Close(); } catch (...) {} }
    }
    std::string GetName() const override { return name; }
    std::string GetFullPath() const override { return path; }
    size_t GetSize() const override { return size; }
    FileSys::VirtualDir GetContainingDirectory() const override { return parent; }
    bool IsReadable() const override { return true; }
    bool IsDirect() const { return handle != INVALID_HANDLE_VALUE; }
    bool IsWritable() const override { return false; }
    bool Resize(size_t) override { return false; }
    bool Rename(std::string_view) override { return false; }
    size_t Write(const u8*, size_t, size_t) override { return 0; }
    size_t Read(u8* data, size_t length, size_t offset) const override {
        if (offset >= size || !length || !data) return 0;
        length = std::min(length, size - offset);
        const Common::HostStalls::FileReadTimer read_timer;
        if (handle != INVALID_HANDLE_VALUE && rom_cache_enabled.load(std::memory_order_relaxed)) {
            RomReadCache& cache = RomReadCache::Instance();
            const bool read = cache.Read(cache_id, size, data, length, offset,
                                         [this](u8* bytes, size_t count, u64 at) {
                                             // Serialize seek+read on this handle.
                                             std::scoped_lock lock{mutex};
                                             return ReadAt(handle, bytes, count, at);
                                         });
            if (!read) {
                LogReadFailure(GetLastError());
                read_timer.Done(0);
                return 0;
            }
            // In order with the previous read of this file: read the next blocks ahead.
            const u64 previous_end =
                last_end.exchange(offset + length, std::memory_order_relaxed);
            if (offset + RomReadCache::BLOCK >= previous_end &&
                offset <= previous_end + RomReadCache::BLOCK) {
                cache.Prefetch(cache_id, wide_path, size,
                               (offset + length + RomReadCache::BLOCK - 1) / RomReadCache::BLOCK,
                               RomReadCache::PREFETCH_BLOCKS);
            }
            read_timer.Done(length);
            return length;
        }
        std::scoped_lock lock{mutex}; // Serialize seek+read on this file, not the whole VFS.
        size_t done = 0;
        try {
            if (handle != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(offset);
                if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
                    LogReadFailure(GetLastError());
                    return 0;
                }
            } else {
                EnsureStorageApartment();
                stream.Seek(offset);
            }
            // At most 1MiB scratch on the brokered route, independent of dump size.
            Buffer buffer{nullptr};
            if (handle == INVALID_HANDLE_VALUE)
                buffer = Buffer{static_cast<unsigned>(std::min<size_t>(ReadChunk, length))};
            while (done < length) {
                const auto count = static_cast<unsigned>(std::min<size_t>(ReadChunk, length - done));
                unsigned received{};
                if (handle != INVALID_HANDLE_VALUE) {
                    DWORD read{};
                    if (!ReadFile(handle, data + done, count, &read, nullptr)) {
                        LogReadFailure(GetLastError());
                        break;
                    }
                    received = read;
                } else {
                    buffer.Length(0);
                    auto result = stream.ReadAsync(buffer, count, InputStreamOptions::None).get();
                    received = result.Length();
                    if (received > count) break;
                    std::memcpy(data + done, result.data(), received);
                }
                done += received;
                if (!received) break;
            }
        } catch (const winrt::hresult_error& e) {
            LogReadFailure(static_cast<unsigned>(e.code().value));
        }
        if (done != length) LogReadFailure(ERROR_READ_FAULT);
        read_timer.Done(done);
        return done;
    }
private:
    void LogReadFailure(unsigned error) const {
        storage_read_failure.store(true);
        if (!failed.exchange(true))
            LOG_ERROR(Frontend, "ROM storage: read failed error={} path={} (USB removed/permission?)",
                      error, path);
    }
    StorageFile item;
    FileSys::VirtualDir parent;
    std::string path, name;
    std::wstring wide_path; ///< for the read cache's own handle
    u32 cache_id{};
    mutable std::atomic<u64> last_end{~0ULL}; ///< end of the previous read, to spot in-order ones
    HANDLE handle{INVALID_HANDLE_VALUE};
    IRandomAccessStreamWithContentType stream{nullptr};
    size_t size{};
    mutable std::mutex mutex;
    mutable std::atomic<bool> failed{};
};

class StorageDirectory final : public FileSys::ReadOnlyVfsDirectory {
public:
    StorageDirectory(StorageFolder folder, std::string path)
        : folder(std::move(folder)), path(std::move(path)), name(winrt::to_string(this->folder.Name())) {}
    std::string GetName() const override { return name; }
    std::string GetFullPath() const override { return path; }
    FileSys::VirtualDir GetParentDirectory() const override {
        const auto parsed = ParseStoragePath(path);
        if (!parsed || parsed->relative.empty()) return {};
        const auto slash = parsed->relative.rfind('/');
        const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
        try {
            EnsureStorageApartment();
            return MakeDirectory(ResolveFolder(*parsed, parent), MakeStoragePath(parsed->token, parent));
        }
        catch (const winrt::hresult_error&) { return {}; }
    }
    FileSys::VirtualFile GetFileRelative(std::string_view relative) const override {
        if (relative.empty() || !ValidStorageRelative(relative)) return {};
        try {
            EnsureStorageApartment();
            const auto slash = relative.rfind('/');
            auto containing = folder;
            std::string parent_path = path;
            if (slash != std::string_view::npos) {
                auto directory = GetDirectoryRelative(relative.substr(0, slash));
                if (!directory) return {};
                return directory->GetFile(relative.substr(slash + 1));
            }
            auto file = containing.GetFileAsync(winrt::to_hstring(relative)).get();
            return std::make_shared<StorageGameFile>(file, MakeDirectory(containing, parent_path),
                                                     path + "/" + std::string{relative});
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "ROM storage: file unavailable {}: {}", path, winrt::to_string(e.message()));
            return {};
        }
    }
    FileSys::VirtualDir GetDirectoryRelative(std::string_view relative) const override {
        if (!ValidStorageRelative(relative)) return {};
        try {
            EnsureStorageApartment();
            auto next = folder;
            auto remaining = relative;
            while (!remaining.empty()) {
                const auto slash = remaining.find('/');
                next = next.GetFolderAsync(winrt::to_hstring(remaining.substr(0, slash))).get();
                if (slash == std::string_view::npos) break;
                remaining.remove_prefix(slash + 1);
            }
            return MakeDirectory(next, path + (relative.empty() ? "" : "/" + std::string{relative}));
        } catch (const winrt::hresult_error&) { return {}; }
    }
    std::vector<FileSys::VirtualFile> GetFiles() const override {
        std::vector<FileSys::VirtualFile> result;
        try {
            EnsureStorageApartment();
            for (unsigned start = 0; start < MaxEntries; start += PageSize) {
                auto files = folder.GetFilesAsync(Search::CommonFileQuery::DefaultQuery, start, PageSize).get();
                for (const auto& file : files)
                    result.push_back(std::make_shared<StorageGameFile>(file, MakeDirectory(folder, path),
                                      path + "/" + winrt::to_string(file.Name())));
                if (files.Size() < PageSize) break;
            }
        } catch (const winrt::hresult_error&) {}
        return result;
    }
    std::vector<FileSys::VirtualDir> GetSubdirectories() const override {
        std::vector<FileSys::VirtualDir> result;
        try {
            EnsureStorageApartment();
            for (unsigned start = 0; start < MaxEntries; start += PageSize) {
                auto folders = folder.GetFoldersAsync(Search::CommonFolderQuery::DefaultQuery, start, PageSize).get();
                for (const auto& child : folders)
                    result.push_back(MakeDirectory(child, path + "/" + winrt::to_string(child.Name())));
                if (folders.Size() < PageSize) break;
            }
        } catch (const winrt::hresult_error&) {}
        return result;
    }
private:
    StorageFolder folder;
    std::string path, name;
};
FileSys::VirtualDir MakeDirectory(StorageFolder folder, std::string path) {
    return std::make_shared<StorageDirectory>(std::move(folder), std::move(path));
}

class UwpFilesystem final : public FileSys::RealVfsFilesystem {
public:
    FileSys::VirtualFile OpenFile(std::string_view path, FileSys::OpenMode mode) override {
        if (!IsStoragePath(path)) return RealVfsFilesystem::OpenFile(path, mode);
        const auto parsed = ParseStoragePath(path);
        if (!parsed || parsed->relative.empty() || mode != FileSys::OpenMode::Read) return {};
        try {
            EnsureStorageApartment();
            if (!StorageApplicationPermissions::FutureAccessList().ContainsItem(winrt::to_hstring(parsed->token))) return {};
            const auto slash = parsed->relative.rfind('/');
            const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
            const auto name = slash == std::string::npos ? parsed->relative : parsed->relative.substr(slash + 1);
            auto folder = ResolveFolder(*parsed, parent);
            auto file = folder.GetFileAsync(winrt::to_hstring(name)).get();
            return std::make_shared<StorageGameFile>(file, MakeDirectory(folder, MakeStoragePath(parsed->token, parent)),
                                                     std::string{path});
        } catch (const winrt::hresult_error& e) {
            LOG_ERROR(Frontend, "ROM storage: open failed {}: {}", path, winrt::to_string(e.message()));
            return {};
        }
    }
    FileSys::VirtualDir OpenDirectory(std::string_view path, FileSys::OpenMode mode) override {
        if (!IsStoragePath(path)) return RealVfsFilesystem::OpenDirectory(path, mode);
        const auto parsed = ParseStoragePath(path);
        if (!parsed || mode != FileSys::OpenMode::Read) return {};
        try {
            EnsureStorageApartment();
            return MakeDirectory(ResolveFolder(*parsed, parsed->relative), std::string{path});
        }
        catch (const winrt::hresult_error&) { return {}; }
    }
    FileSys::VfsEntryType GetEntryType(std::string_view path) const override {
        if (!IsStoragePath(path)) return RealVfsFilesystem::GetEntryType(path);
        const auto parsed = ParseStoragePath(path);
        if (!parsed) return FileSys::VfsEntryType::None;
        try {
            EnsureStorageApartment();
            if (parsed->relative.empty()) {
                ResolveFolder(*parsed, "");
                return FileSys::VfsEntryType::Directory;
            }
            const auto slash = parsed->relative.rfind('/');
            const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
            const auto name = slash == std::string::npos ? parsed->relative : parsed->relative.substr(slash + 1);
            auto item = ResolveFolder(*parsed, parent).TryGetItemAsync(winrt::to_hstring(name)).get();
            if (!item) return FileSys::VfsEntryType::None;
            return item.IsOfType(StorageItemTypes::Folder) ? FileSys::VfsEntryType::Directory : FileSys::VfsEntryType::File;
        } catch (const winrt::hresult_error&) { return FileSys::VfsEntryType::None; }
    }
};

void ScanFolder(StorageFolder folder, const std::string& token, const std::wstring& source,
                std::string relative, unsigned depth, unsigned& visited,
                LibraryScan& scan, std::stop_token stop) {
    for (unsigned start = 0; !stop.stop_requested(); start += PageSize) {
        auto items = folder.GetItemsAsync(start, PageSize).get();
        for (const auto& item : items) {
            if (stop.stop_requested()) return;
            if (++visited > MaxEntries) { scan.limited = true; return; }
            const auto name = winrt::to_string(item.Name());
            const auto child = relative.empty() ? name : relative + "/" + name;
            if (!ValidStorageRelative(child)) continue;
            if (item.IsOfType(StorageItemTypes::Folder)) {
                if (depth < 4) {
                    try { ScanFolder(item.as<StorageFolder>(), token, source, child, depth + 1, visited, scan, stop); }
                    catch (const winrt::hresult_error& e) { AddError(scan, ErrorText(e)); }
                }
            } else {
                const auto p = std::filesystem::path{winrt::to_hstring(child).c_str()};
                auto ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!IsGameExtension(ext)) continue;
                LibraryEntry entry{p, p.stem().wstring()};
                entry.launch_path = MakeStoragePath(token, child);
                entry.source_name = source;
                // Enumerated items carry their dates: no extra call per file.
                try { entry.added = static_cast<std::int64_t>(winrt::clock::to_time_t(item.DateCreated())); }
                catch (const winrt::hresult_error&) {}
                scan.entries.push_back(std::move(entry));
            }
            if (scan.limited) return;
        }
        if (items.Size() < PageSize) break;
    }
}
} // namespace

void SetRomReadCache(bool enabled) {
    rom_cache_enabled.store(enabled, std::memory_order_relaxed);
}

bool ConsumeStorageReadFailure() { return storage_read_failure.exchange(false); }
std::uint64_t StorageFileStamp(std::string_view path) {
    const auto parsed = ParseStoragePath(path);
    if (!parsed) {
        std::error_code ec;
        const auto value = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
        return ec ? 0 : static_cast<std::uint64_t>(value.time_since_epoch().count());
    }
    try {
        EnsureStorageApartment();
        const auto slash = parsed->relative.rfind('/');
        const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
        const auto name = slash == std::string::npos ? parsed->relative : parsed->relative.substr(slash + 1);
        const auto file = ResolveFolder(*parsed, parent).GetFileAsync(winrt::to_hstring(name)).get();
        return static_cast<std::uint64_t>(file.GetBasicPropertiesAsync().get().DateModified().time_since_epoch().count());
    } catch (...) { return 0; }
}
std::vector<StorageBrowserEntry> BrowseStorageFolder(std::string_view path, std::stop_token stop) {
    return BrowseStorageFolder(path, stop, [](std::string_view name) {
        auto ext = std::filesystem::u8path(name).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        return ext == ".nsp" || ext == ".xci" || ext == ".nca" || ext == ".nsz" || ext == ".xcz";
    });
}

std::vector<StorageBrowserEntry> BrowseStorageFolder(std::string_view path, std::stop_token stop,
                                                     const std::function<bool(std::string_view)>& keep) {
    std::vector<StorageBrowserEntry> result;
    const auto parsed = ParseStoragePath(path);
    if (!parsed) return result;
    EnsureStorageApartment();
    const auto folder = ResolveFolder(*parsed, parsed->relative);
    for (unsigned start = 0; start < 10000 && !stop.stop_requested(); start += PageSize) {
        const auto items = folder.GetItemsAsync(start, PageSize).get();
        for (const auto& item : items) {
            const auto name = winrt::to_string(item.Name());
            const auto child = parsed->relative.empty() ? name : parsed->relative + "/" + name;
            if (!ValidStorageRelative(child)) continue;
            const bool directory = item.IsOfType(StorageItemTypes::Folder);
            if (!directory && !keep(name)) continue;
            result.push_back({MakeStoragePath(parsed->token, child), std::wstring{item.Name()}, directory});
        }
        if (items.Size() < PageSize) break;
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.folder != b.folder ? a.folder > b.folder : a.name < b.name;
    });
    return result;
}

std::string RememberFolder(const StorageFolder& folder, std::string_view prefix) {
    EnsureStorageApartment();
    auto list = StorageApplicationPermissions::FutureAccessList();
    unsigned count = 0;
    std::string token;
    const auto path = folder.Path();
    for (const auto& entry : list.Entries()) {
        const auto existing = winrt::to_string(entry.Token);
        if (!existing.starts_with(prefix)) continue;
        ++count;
        // Reauthorizing the same path updates its token rather than duplicating games.
        try {
            if (!path.empty() && list.GetFolderAsync(entry.Token).get().Path() == path) token = existing;
        } catch (const winrt::hresult_error&) {
            if (!path.empty() && entry.Metadata == path) token = existing;
        }
    }
    if (token.empty()) {
        if (count >= MaxSources) throw std::runtime_error(I18n::ToUtf8(I18n::Tr(I18n::Text::FolderLimit)));
        GUID guid{};
        winrt::check_hresult(CoCreateGuid(&guid));
        token = prefix;
        const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(guid); ++i) { token += hex[bytes[i] >> 4]; token += hex[bytes[i] & 15]; }
    }
    list.AddOrReplace(winrt::to_hstring(token), folder, path);
    return token;
}

std::string RememberGameFolder(const StorageFolder& folder) { return RememberFolder(folder, GameTokenPrefix); }
std::string RememberContentFolder(const StorageFolder& folder) { return RememberFolder(folder, ContentTokenPrefix); }
std::vector<StorageBrowserEntry> ContentFolderSources() {
    EnsureStorageApartment(); std::vector<StorageBrowserEntry> result;
    for (const auto& entry : StorageApplicationPermissions::FutureAccessList().Entries()) {
        const auto token = winrt::to_string(entry.Token);
        if (!IsSourceToken(token)) continue;
        const auto name = std::filesystem::path{std::wstring{entry.Metadata}}.filename().wstring();
        result.push_back({MakeStoragePath(token), name.empty() ? L"USB" : name, true});
    }
    return result;
}
LibraryScan ScanExternalGameFolders(std::stop_token stop) {
    EnsureStorageApartment();
    LibraryScan result;
    unsigned visited = 0, sources = 0;
    auto list = StorageApplicationPermissions::FutureAccessList();
    for (const auto& entry : list.Entries()) {
        if (stop.stop_requested()) break;
        const auto token = winrt::to_string(entry.Token);
        if (!IsGameToken(token)) continue;
        if (++sources > MaxSources) { result.limited = true; break; }
        const auto first = result.entries.size();
        try {
            auto folder = list.GetFolderAsync(entry.Token).get();
            ScanFolder(folder, token, std::wstring{folder.Name()}, "", 0, visited, result, stop);
        } catch (const winrt::hresult_error& e) { AddError(result, ErrorText(e)); }
        DropPackageParts(result.entries, first); // Per folder: relative paths repeat across them.
        if (result.limited) break;
    }
    return result;
}

FileSys::VirtualFilesystem MakeUwpFilesystem() { return std::make_shared<UwpFilesystem>(); }

std::string CheckExternalGame(std::string_view path) {
    if (!IsStoragePath(path)) return {};
    try {
        auto fs = MakeUwpFilesystem();
        auto file = fs->OpenFile(path, FileSys::OpenMode::Read);
        if (!file) return I18n::ToUtf8(I18n::Tr(I18n::Text::GameOpenFailed));
        std::array<u8, 64> bytes{};
        const auto size = file->GetSize();
        if (!size || file->Read(bytes.data(), std::min(bytes.size(), size), 0) != std::min(bytes.size(), size))
            return I18n::ToUtf8(I18n::Tr(I18n::Text::GameReadFailed));
        // Exercise 64-bit offsets before spending time constructing the guest.
        if (size > (u64{1} << 32) + bytes.size() &&
            file->Read(bytes.data(), bytes.size(), (u64{1} << 32) + 1) != bytes.size())
            return I18n::ToUtf8(I18n::Tr(I18n::Text::GameReadPast4GiB));
        const auto count = std::min(bytes.size(), size);
        if (file->Read(bytes.data(), count, size - count) != count ||
            file->Read(bytes.data(), 1, size) != 0) return I18n::ToUtf8(I18n::Tr(I18n::Text::GameReadEndFailed));
        LOG_INFO(Frontend, "ROM storage: preflight passed size={} path={}", size, path);
        return {};
    } catch (const winrt::hresult_error& e) { return ErrorText(e); }
      catch (const std::exception& e) {
        LOG_WARNING(Frontend, "ROM storage: preflight failed path={}: {}", path, e.what());
        return I18n::ToUtf8(I18n::Tr(I18n::Text::GameReadFailed));
    }
}

bool RunRomStorageGate(bool restore, const std::function<void(std::string)>& diagnostic) {
    try {
        auto require = [](bool success, const char* message) {
            if (!success) throw std::runtime_error(message);
        };
        const auto settings = ApplicationData::Current().LocalSettings().Values();
        constexpr auto setting = L"rom-storage-gate-token";
        auto folder = ApplicationData::Current().LocalFolder().GetFolderAsync(L"rom-storage-gate").get();
        std::string token;
        if (restore) {
            require(settings.HasKey(setting), "missing gate token from preceding launch");
            token = winrt::to_string(winrt::unbox_value<winrt::hstring>(settings.Lookup(setting)));
            folder = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(winrt::to_hstring(token)).get();
        } else {
            token = RememberGameFolder(folder);
            settings.Insert(setting, winrt::box_value(winrt::to_hstring(token)));
        }
        require(RememberGameFolder(folder) == token, "reauthorization duplicated the source");
        const auto path = MakeStoragePath(token, "fixture.bin");
        const auto item = folder.GetFileAsync(L"fixture.bin").get();
        const auto parent = MakeDirectory(folder, MakeStoragePath(token));
        auto direct = std::make_shared<StorageGameFile>(item, parent, path);
        auto brokered = std::make_shared<StorageGameFile>(item, parent, path, true);
        require(direct->IsDirect(), "fixture direct FromApp open failed");
        const size_t expected_size = (u64{1} << 32) + 2 * ReadChunk + 257;
        require(direct->GetSize() == expected_size && brokered->GetSize() == expected_size, "64-bit file size mismatch");
        auto verify = [&](const FileSys::VirtualFile& file, size_t offset, size_t length) {
            std::vector<u8> bytes(length, 0);
            const auto expected = offset >= expected_size ? 0 : std::min(length, expected_size - offset);
            require(file->Read(bytes.data(), length, offset) == expected, "read length/EOF mismatch");
            for (size_t i = 0; i < expected; ++i) {
                const u64 position = offset + i;
                const auto value = static_cast<u8>((position * 17) ^ (position >> 16) ^ 0x5a);
                require(bytes[i] == value, "reference bytes mismatch");
            }
        };
        for (const FileSys::VirtualFile& file : {FileSys::VirtualFile{direct}, FileSys::VirtualFile{brokered}}) {
            verify(file, 0, 2 * ReadChunk + 257);
            verify(file, 13, ReadChunk + 101); // crosses the bounded read chunk
            verify(file, (u64{1} << 32) - 97, 512);
            verify(file, (u64{1} << 32) + 1, 64);
            verify(file, expected_size - 64, 128);
            verify(file, expected_size, 64);
            verify(file, SIZE_MAX, 64);
            require(!file->Resize(0) && !file->Rename("changed.bin") &&
                    file->Write(nullptr, 1, 0) == 0, "external VFS allowed mutation");
        }
        std::atomic<bool> concurrent_ok{true};
        {
            std::array<std::jthread, 4> workers;
            for (unsigned worker = 0; worker < workers.size(); ++worker) {
                workers[worker] = std::jthread([&, worker] {
                    // Intentionally no apartment: exercise the ordinary core worker path.
                    try {
                        for (unsigned i = 0; i < 32; ++i) {
                            const size_t offset = i % 2 ? (u64{1} << 32) + 1 : 13 + worker * 29;
                            verify(direct, offset, 64);
                            verify(brokered, offset, 64);
                        }
                    } catch (...) { concurrent_ok = false; }
                });
            }
        }
        require(concurrent_ok, "concurrent offset reads mismatch");
        auto fs = MakeUwpFilesystem();
        require(fs->GetEntryType(path) == FileSys::VfsEntryType::File, "token VFS entry type mismatch");
        auto virtual_file = fs->OpenFile(path, FileSys::OpenMode::Read);
        require(bool(virtual_file), "token VFS open failed");
        verify(virtual_file, (u64{1} << 32) + 1, 64);
        require(bool(virtual_file->GetContainingDirectory()->GetFile("fixture.bin")), "sibling open failed");
        require(!fs->OpenFile(path, FileSys::OpenMode::ReadWrite), "writable external open allowed");
        require(!fs->OpenFile(MakeStoragePath(token, "../fixture.bin"), FileSys::OpenMode::Read), "traversal allowed");
        require(!fs->OpenFile(MakeStoragePath("jadyx-games-missing", "fixture.bin"), FileSys::OpenMode::Read), "missing token opened");
        require(CheckExternalGame(path).empty(), "preflight failed");
        const auto scan = ScanExternalGameFolders({});
        unsigned catalog_entries = 0;
        bool nested_a = false, nested_b = false;
        for (const auto& entry : scan.entries) {
            const auto parsed = ParseStoragePath(entry.launch_path);
            if (!parsed || parsed->token != token) continue;
            ++catalog_entries;
            nested_a |= parsed->relative == "a/same.nro";
            nested_b |= parsed->relative == "b/same.nro";
        }
        require(catalog_entries == 72 && nested_a && nested_b, "paged/nested catalog identity mismatch");
        const auto nested = fs->OpenDirectory(MakeStoragePath(token, "a"), FileSys::OpenMode::Read);
        require(bool(nested), "nested directory open failed");
        require(nested->GetParentDirectory()->GetFullPath() == MakeStoragePath(token), "directory parent escaped the source");
        auto homebrew = nested->GetFile("same.nro");
        require(homebrew && Loader::AppLoader_NRO::IdentifyType(homebrew) == Loader::FileType::NRO,
                "core loader cannot identify token-scoped homebrew");
        auto homebrew_item = folder.GetFolderAsync(L"a").get().GetFileAsync(L"same.nro").get();
        auto streamed_homebrew = std::make_shared<StorageGameFile>(homebrew_item, nested,
            MakeStoragePath(token, "a/same.nro"), true);
        require(Loader::AppLoader_NRO::IdentifyType(streamed_homebrew) == Loader::FileType::NRO,
                "core loader cannot identify brokered homebrew");
        Loader::AppLoader_NRO direct_loader{homebrew}, stream_loader{streamed_homebrew};
        std::string direct_title, stream_title;
        require(direct_loader.ReadTitle(direct_title) == stream_loader.ReadTitle(stream_title) &&
                direct_title == stream_title, "core metadata differs between backends");
        if (restore) {
            StorageApplicationPermissions::FutureAccessList().Remove(winrt::to_hstring(token));
            settings.Remove(setting);
            require(!fs->OpenFile(path, FileSys::OpenMode::Read), "revoked token opened a new file");
        }
        diagnostic(std::string("ROM storage gate PASS phase=") + (restore ? "restore" : "record") +
                   " size=" + std::to_string(expected_size) +
                   " direct+forced-WinRT reference/EOF/chunks/concurrency/token/sibling/readonly/catalog72/loader OK");
        return true;
    } catch (const winrt::hresult_error& e) {
        diagnostic("ROM storage gate FAIL: " + winrt::to_string(e.message()));
    } catch (const std::exception& e) { diagnostic(std::string("ROM storage gate FAIL: ") + e.what()); }
    return false;
}
} // namespace Jadyx
