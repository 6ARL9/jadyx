// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/perf_counters.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace VideoCore::Perf {

u64 ProcessPageFaults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        return memory.PageFaultCount;
    }
#endif
    return 0;
}

} // namespace VideoCore::Perf
