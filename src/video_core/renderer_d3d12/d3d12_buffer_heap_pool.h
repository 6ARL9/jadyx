// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Guest buffers placed in pooled heaps instead of one committed resource each
/// ("buffer_heaps=0" turns it off). Applies to buffers created from now on.
void SetBufferHeaps(bool enabled);

/// Places the buffer cache's buffers in shared DEFAULT heaps. A committed buffer is an allocation
/// of its own from the system: Metroid Prime Remastered made 100 to 170 of them in one frame at
/// room transitions, about 0.3 ms each, and two thirds of the resources made in its hitch frames
/// were such buffers. A placed buffer only takes a range of a heap made earlier, most often on
/// another thread.
///
/// The range goes back to the pool when the resource is destroyed (an interface kept in the
/// resource's private data), which the scheduler does once the GPU is done with it, so every
/// release path of the buffer cache, recycling included, frees it without knowing about the pool.
/// Contents are not zeroed, as with a recycled buffer: the cache uploads what it binds.
class BufferHeapPool {
public:
    explicit BufferHeapPool(const Device& device);
    ~BufferHeapPool();

    BufferHeapPool(const BufferHeapPool&) = delete;
    BufferHeapPool& operator=(const BufferHeapPool&) = delete;

    /// A placed buffer of `size` bytes allowing unordered access, in the COMMON state; null when
    /// it should be committed instead (too large, the pool is off or under memory pressure, or
    /// the device refused). Any thread.
    [[nodiscard]] ComPtr<ID3D12Resource> Create(u64 size);

    /// Releases heaps that stayed empty, keeping one. Once a frame, from the recording thread.
    void TickFrame();

    /// Diagnostic summary for the log.
    [[nodiscard]] std::string Report() const;

    struct State; ///< shared with the range tokens, which can outlive the pool

private:
    struct SpareWorker;
    const Device& device;
    std::shared_ptr<State> state;
    u64 frame{};
    std::unique_ptr<SpareWorker> spare_worker; ///< last: stopped before the members it uses
};

} // namespace D3D12
