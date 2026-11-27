// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/textures/workers.h"

#include <cstring>
#include <vector>

#include "common/literals.h"
#include "common/permafrost.h"
#include "core/device_memory_manager.h"

namespace Tegra::Texture {

Common::ThreadWorker& GetThreadWorkers() {
    static Common::ThreadWorker workers{(std::max)(std::thread::hardware_concurrency(), 2U) / 2,
                                        "ImageTranscode"};
    return workers;
}

void RegisterMemoryWorker() {
    thread_local const Common::Permafrost::ScopedMemoryThread registration;
    (void)registration;
}

void RunCopies(std::span<const Core::DeviceCopyRun> runs) {
    using namespace Common::Literals;
    // Most textures are about 1 MiB: pieces of 256 KiB, the calling thread copying the last.
    constexpr size_t PARALLEL_MIN = 512_KiB;
    constexpr size_t TASK_BYTES = 256_KiB;
    struct Piece {
        const Core::DeviceCopyRun* run;
        size_t begin;
        size_t size;
    };
    const auto copy = [](const Core::DeviceCopyRun& run, size_t begin, size_t size) {
        if (run.source != nullptr) {
            std::memcpy(run.dest + begin, run.source + begin, size);
        } else {
            std::memset(run.dest + begin, 0, size);
        }
    };
    size_t total = 0;
    for (const Core::DeviceCopyRun& run : runs) {
        total += run.size;
    }
    if (total < PARALLEL_MIN) {
        for (const Core::DeviceCopyRun& run : runs) {
            copy(run, 0, run.size);
        }
        return;
    }
    Common::ThreadWorker& workers = GetThreadWorkers();
    std::vector<Piece> pieces;
    size_t bytes = 0;
    const auto queue = [&] {
        if (pieces.empty()) {
            return;
        }
        workers.QueueWork([copy, task = std::move(pieces)] {
            RegisterMemoryWorker();
            for (const Piece& piece : task) {
                copy(*piece.run, piece.begin, piece.size);
            }
        });
        pieces = {};
        bytes = 0;
    };
    size_t remaining = total;
    for (const Core::DeviceCopyRun& run : runs) {
        for (size_t begin = 0; begin < run.size;) {
            const size_t amount = (std::min)(run.size - begin, TASK_BYTES - bytes);
            pieces.push_back({&run, begin, amount});
            bytes += amount;
            begin += amount;
            remaining -= amount;
            if (bytes >= TASK_BYTES && remaining != 0) {
                queue();
            }
        }
    }
    // The last pieces on this thread meanwhile.
    for (const Piece& piece : pieces) {
        copy(*piece.run, piece.begin, piece.size);
    }
    workers.WaitForRequests();
}

} // namespace Tegra::Texture
