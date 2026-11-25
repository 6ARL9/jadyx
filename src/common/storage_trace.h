// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

/// Every read of the game's storage, for designing read-ahead offline from a real session
/// ("rom_trace=1"). Offsets, sizes and times only, never contents. Entries are kept in memory and
/// written to the log by Flush (the renderer calls it once a perf window, off the file service's
/// thread), so recording costs a lock and a vector append per read.
///
/// A log line holds up to LINE_ENTRIES entries "Kid t offset length a b c" separated by ';', the
/// kind letter followed by the id with no space:
///   K  'G' a storage read the game asked for (IStorage::Read), 'F' a file read (IFile::Read),
///      'D' a read of the game file on the host's disk (after decryption layers merged them)
///   id the storage, file or disk file it went to (numbered in order of first use)
///   t  microseconds since the trace started; offset and length in hexadecimal bytes
///   a, b, c in microseconds: G and F: waited for the file service, slept in read pacing, handled
///      (pacing included); D: took on the disk, 0, 0
namespace Common::StorageTrace {

inline std::atomic<bool> enabled{false};

struct Entry {
    u64 time_us;
    u64 offset;
    u32 length;
    u32 a;
    u32 b;
    u32 c;
    u16 id;
    char kind;
};

namespace Detail {
inline std::mutex mutex;
inline std::vector<Entry> entries;
inline std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
inline u64 recorded{};
inline u64 written{};
inline constexpr size_t LINE_ENTRIES = 128;
/// Past it the recording thread writes the entries itself, so memory stays bounded.
inline constexpr size_t MAX_PENDING = 16384;

inline void Write(const std::vector<Entry>& batch) {
    std::string line;
    for (size_t first = 0; first < batch.size(); first += LINE_ENTRIES) {
        line.clear();
        const size_t end = std::min(batch.size(), first + LINE_ENTRIES);
        for (size_t i = first; i < end; ++i) {
            const Entry& e = batch[i];
            fmt::format_to(std::back_inserter(line), "{}{}{} {} {:x} {:x} {} {} {}", i == first ? "" : ";",
                           e.kind, e.id, e.time_us, e.offset, e.length, e.a, e.b, e.c);
        }
        LOG_INFO(Common_Filesystem, "ROM trace: {}", line);
    }
}
} // namespace Detail

/// The trace's own clock, in microseconds.
[[nodiscard]] inline u64 NowUs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - Detail::start)
                                .count());
}

inline void Record(char kind, u16 id, u64 time_us, u64 offset, u64 length, u64 a, u64 b, u64 c) {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const auto clamp = [](u64 value) { return static_cast<u32>(std::min<u64>(value, ~u32{0})); };
    std::vector<Entry> full;
    {
        std::scoped_lock lock{Detail::mutex};
        Detail::entries.push_back(
            {time_us, offset, clamp(length), clamp(a), clamp(b), clamp(c), id, kind});
        ++Detail::recorded;
        if (Detail::entries.size() < Detail::MAX_PENDING) {
            return;
        }
        full.swap(Detail::entries);
        Detail::written += full.size();
    }
    Detail::Write(full);
}

/// Writes the pending entries to the log; any thread.
inline void Flush() {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    std::vector<Entry> batch;
    {
        std::scoped_lock lock{Detail::mutex};
        if (Detail::entries.empty()) {
            return;
        }
        batch.swap(Detail::entries);
        Detail::entries.reserve(batch.size());
        Detail::written += batch.size();
    }
    Detail::Write(batch);
}

/// Turns the trace on or off; the time origin starts again when it is turned on.
inline void SetEnabled(bool on) {
    if (on && !enabled.load(std::memory_order_relaxed)) {
        std::scoped_lock lock{Detail::mutex};
        Detail::start = std::chrono::steady_clock::now();
        LOG_INFO(Common_Filesystem,
                 "ROM trace on: entries Kid t offset length a b c (G/F: queued, paced, handled "
                 "us; D: disk us)");
    }
    enabled.store(on, std::memory_order_relaxed);
}

/// A small number for a storage object, in order of first use.
[[nodiscard]] inline u16 IdOf(const void* object) {
    static std::mutex ids_mutex;
    static std::vector<const void*> ids;
    std::scoped_lock lock{ids_mutex};
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == object) {
            return static_cast<u16>(i + 1);
        }
    }
    ids.push_back(object);
    return static_cast<u16>(ids.size());
}

} // namespace Common::StorageTrace
