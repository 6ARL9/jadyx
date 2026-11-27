// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/thread_worker.h"

namespace Core {
struct DeviceCopyRun;
}

namespace Tegra::Texture {

Common::ThreadWorker& GetThreadWorkers();

/// For a worker task about to read guest memory where it is: the worker is paused like the GPU
/// thread while the guest swap fills pages (Common::Permafrost::ScopedMemoryThread), from its
/// first such task to its exit, so it never reads a page half filled.
void RegisterMemoryWorker();

/// Runs the copies of a read (MemoryManager::ReadBlockRuns), split over the workers when they add
/// up to a few MiB; returns when all are done.
void RunCopies(std::span<const Core::DeviceCopyRun> runs);

} // namespace Tegra::Texture
