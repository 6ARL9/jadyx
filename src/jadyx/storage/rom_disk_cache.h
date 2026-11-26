// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Jadyx {

/// Blocks of games on external storage, kept in one file on the console's own storage
/// ("rom_cache_mib=", 1024 by default, 0 turns it off). Metroid Prime Remastered on a USB hard
/// drive read 38% of its 128 KiB reads again within the same session, the first one included,
/// and a read that has to move the drive's head costs 5-9 ms; one from the console's storage
/// costs a fraction of a millisecond.
///
/// A read the cache cannot answer whole goes to the source as one read stretched to block
/// boundaries (Expand); its blocks are then written to the cache file by a background thread
/// (Offer), least recently used ones making room. Each block keeps a hash of what was written
/// and is checked when read back: a block that does not match is dropped and the read goes to
/// the source, so the cache can make reads faster, never different. The cache file is started
/// again on every run.
class RomDiskCache {
public:
    static constexpr u64 BLOCK = 32 * 1024;
    /// Reads longer than this skip the cache: they are rare and would need a large buffer.
    static constexpr u64 MAX_SPAN = 8ULL << 20;
    /// Blocks waiting to be written; past it new blocks are not offered.
    static constexpr u64 MAX_PENDING_BYTES = 16ULL << 20;

    /// The cache file. Reads and writes at an offset; both may run at once on different threads.
    class File {
    public:
        virtual ~File() = default;
        virtual bool Read(u64 at, u8* data, size_t size) = 0;
        virtual bool Write(u64 at, const u8* data, size_t size) = 0;
    };

    struct Stats {
        u64 hits{};
        u64 hit_bytes{};
        u64 hit_us{};
        u64 misses{};
        u64 blocks_written{};
        u64 bytes_written{};
        u64 dropped{};       ///< offered while the write queue was full
        u64 evicted{};
        u64 mismatches{};    ///< read back different from what was written
        u64 read_failures{};
        u64 write_failures{};
        u64 used_bytes{};
        u64 capacity_bytes{};
    };

    using Hasher = u64 (*)(const u8* data, size_t size);
    using Clock = u64 (*)(); ///< microseconds

    RomDiskCache(std::unique_ptr<File> file_, u64 capacity_bytes, Hasher hasher_, Clock clock_)
        : file{std::move(file_)}, hasher{hasher_}, now_us{clock_},
          slot_count{static_cast<u32>(std::max<u64>(capacity_bytes / BLOCK, 1))} {
        slots.resize(slot_count);
        for (u32 i = slot_count; i-- > 0;) {
            free_slots.push_back(i);
        }
        writer = std::thread([this] { WriteLoop(); });
    }

    ~RomDiskCache() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        pending_cv.notify_all();
        if (writer.joinable()) {
            writer.join();
        }
    }

    RomDiskCache(const RomDiskCache&) = delete;
    RomDiskCache& operator=(const RomDiskCache&) = delete;

    /// The range to read from the source on a miss: [offset, offset + length) stretched to whole
    /// blocks and clipped to the file. Zero length when the read should skip the cache.
    [[nodiscard]] static std::pair<u64, u64> Expand(u64 offset, u64 length, u64 file_size) {
        const u64 start = offset / BLOCK * BLOCK;
        const u64 end = std::min(file_size, (offset + length + BLOCK - 1) / BLOCK * BLOCK);
        if (end <= start || end - start > MAX_SPAN) {
            return {offset, 0};
        }
        return {start, end - start};
    }

    /// Copies [offset, offset + length) of `file_id` from the cache, when every block of it is
    /// there and reads back as written. False otherwise (nothing usable was copied).
    bool Read(u32 file_id, u64 offset, u8* data, size_t length, u64 file_size) {
        if (length == 0 || offset + length > file_size) {
            return false;
        }
        const u64 start_us = now_us();
        const u64 first = offset / BLOCK;
        const u64 last = (offset + length - 1) / BLOCK;
        const size_t count = static_cast<size_t>(last - first + 1);
        if (count * BLOCK > MAX_SPAN) {
            return false;
        }
        thread_local std::vector<u32> found;
        found.clear();
        {
            std::scoped_lock lock{mutex};
            for (u64 block = first; block <= last; ++block) {
                const auto it = index.find(Key(file_id, block));
                if (it == index.end()) {
                    ++stats.misses;
                    return false;
                }
                found.push_back(it->second);
            }
            for (const u32 slot : found) {
                ++slots[slot].pins;
                Touch(slot);
            }
        }
        // Read the blocks, runs of consecutive slots at once, and check them.
        thread_local std::vector<u8> buffer;
        buffer.resize(count * BLOCK);
        bool good = true;
        std::vector<u32> bad;
        for (size_t i = 0; i < count && good;) {
            size_t run = 1;
            while (i + run < count && found[i + run] == found[i] + run &&
                   slots[found[i + run - 1]].size == BLOCK) {
                ++run;
            }
            size_t bytes = 0;
            for (size_t j = 0; j < run; ++j) {
                bytes += slots[found[i + j]].size;
            }
            if (!file->Read(u64{found[i]} * BLOCK, buffer.data() + i * BLOCK, bytes)) {
                good = false;
                break;
            }
            // Only the last block of a run can be short (the end of a file), so the run lands
            // in the buffer with every block at its place.
            for (size_t j = 0; j < run; ++j) {
                const Slot& slot = slots[found[i + j]];
                if (hasher(buffer.data() + (i + j) * BLOCK, slot.size) != slot.hash) {
                    bad.push_back(found[i + j]);
                    good = false;
                }
            }
            i += run;
        }
        {
            std::scoped_lock lock{mutex};
            for (const u32 slot : found) {
                --slots[slot].pins;
            }
            if (!good) {
                if (bad.empty()) {
                    ++stats.read_failures;
                } else {
                    stats.mismatches += bad.size();
                    for (const u32 slot : bad) {
                        Drop(slot);
                    }
                }
                ++stats.misses;
                return false;
            }
            ++stats.hits;
            stats.hit_bytes += length;
            stats.hit_us += now_us() - start_us;
        }
        std::memcpy(data, buffer.data() + (offset - first * BLOCK), length);
        return true;
    }

    /// Offers the blocks of [start, start + length), just read from the source; `start` is a
    /// block boundary and the range ends on one or at the end of the file. Blocks already cached
    /// or waiting are skipped; the rest are copied and written by the background thread.
    void Offer(u32 file_id, u64 start, const u8* data, size_t length, u64 file_size) {
        if (start % BLOCK != 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (disabled) {
            return;
        }
        for (u64 at = 0; at < length; at += BLOCK) {
            const u64 size = std::min<u64>(BLOCK, length - at);
            const u64 block = (start + at) / BLOCK;
            if (size != BLOCK && start + at + size != file_size) {
                break; // a partial block not at the end of the file
            }
            const u64 key = Key(file_id, block);
            if (index.contains(key) || queued.contains(key)) {
                continue;
            }
            if (pending_bytes + size > MAX_PENDING_BYTES) {
                ++stats.dropped;
                continue;
            }
            Pending item{key, std::vector<u8>(data + at, data + at + size)};
            pending_bytes += size;
            queued.insert(key);
            pending.push_back(std::move(item));
        }
        pending_cv.notify_one();
    }

    [[nodiscard]] Stats GetStats() {
        std::scoped_lock lock{mutex};
        Stats out = stats;
        out.used_bytes = u64{slot_count - static_cast<u32>(free_slots.size())} * BLOCK;
        out.capacity_bytes = u64{slot_count} * BLOCK;
        return out;
    }

    /// Waits until every offered block is written (tests).
    void Drain() {
        std::unique_lock lock{mutex};
        drained_cv.wait(lock, [this] { return pending.empty() && !writing; });
    }

    [[nodiscard]] bool Disabled() {
        std::scoped_lock lock{mutex};
        return disabled;
    }

private:
    static constexpr u32 NONE = ~u32{0};
    /// After this many failed writes in a row the cache stops taking blocks (a full disk).
    static constexpr u32 MAX_WRITE_FAILURES = 8;

    struct Slot {
        u64 key{};
        u64 hash{};
        u32 size{};
        u32 pins{};
        u32 prev{NONE};
        u32 next{NONE};
        bool used{};
    };
    struct Pending {
        u64 key;
        std::vector<u8> bytes;
    };

    [[nodiscard]] static u64 Key(u32 file_id, u64 block) {
        return (u64{file_id} << 40) | (block & ((u64{1} << 40) - 1));
    }

    /// Least recently used list: head is the newest. Callers hold the mutex.
    void Unlink(u32 slot) {
        Slot& s = slots[slot];
        if (s.prev != NONE) {
            slots[s.prev].next = s.next;
        } else if (lru_head == slot) {
            lru_head = s.next;
        }
        if (s.next != NONE) {
            slots[s.next].prev = s.prev;
        } else if (lru_tail == slot) {
            lru_tail = s.prev;
        }
        s.prev = s.next = NONE;
    }
    void PushFront(u32 slot) {
        Slot& s = slots[slot];
        s.prev = NONE;
        s.next = lru_head;
        if (lru_head != NONE) {
            slots[lru_head].prev = slot;
        }
        lru_head = slot;
        if (lru_tail == NONE) {
            lru_tail = slot;
        }
    }
    void Touch(u32 slot) {
        if (lru_head != slot) {
            Unlink(slot);
            PushFront(slot);
        }
    }
    /// Forgets a cached block (it goes back to the free slots once nobody reads it).
    void Drop(u32 slot) {
        Slot& s = slots[slot];
        if (!s.used) {
            return;
        }
        index.erase(s.key);
        Unlink(slot);
        s.used = false;
        if (s.pins == 0) {
            free_slots.push_back(slot);
        } else {
            dropped_pinned.push_back(slot);
        }
    }

    /// A slot to write into: a free one, or the least recently used block nobody is reading.
    u32 TakeSlot() {
        for (auto it = dropped_pinned.begin(); it != dropped_pinned.end();) {
            if (slots[*it].pins == 0) {
                free_slots.push_back(*it);
                it = dropped_pinned.erase(it);
            } else {
                ++it;
            }
        }
        if (!free_slots.empty()) {
            const u32 slot = free_slots.back();
            free_slots.pop_back();
            return slot;
        }
        for (u32 slot = lru_tail; slot != NONE; slot = slots[slot].prev) {
            if (slots[slot].pins == 0) {
                index.erase(slots[slot].key);
                Unlink(slot);
                slots[slot].used = false;
                ++stats.evicted;
                return slot;
            }
        }
        return NONE;
    }

    void WriteLoop() {
        std::unique_lock lock{mutex};
        while (true) {
            pending_cv.wait(lock, [this] { return stopping || !pending.empty(); });
            if (stopping) {
                return;
            }
            Pending item = std::move(pending.front());
            pending.pop_front();
            const u32 slot = TakeSlot();
            if (slot == NONE) {
                queued.erase(item.key);
                pending_bytes -= item.bytes.size();
                ++stats.dropped;
                if (pending.empty()) {
                    drained_cv.notify_all();
                }
                continue;
            }
            writing = true;
            lock.unlock();
            const u64 hash = hasher(item.bytes.data(), item.bytes.size());
            const bool written =
                file->Write(u64{slot} * BLOCK, item.bytes.data(), item.bytes.size());
            lock.lock();
            writing = false;
            queued.erase(item.key);
            pending_bytes -= item.bytes.size();
            if (written) {
                write_failures_in_a_row = 0;
                Slot& s = slots[slot];
                s.key = item.key;
                s.hash = hash;
                s.size = static_cast<u32>(item.bytes.size());
                s.used = true;
                index[item.key] = slot;
                PushFront(slot);
                ++stats.blocks_written;
                stats.bytes_written += item.bytes.size();
            } else {
                free_slots.push_back(slot);
                ++stats.write_failures;
                if (++write_failures_in_a_row >= MAX_WRITE_FAILURES) {
                    disabled = true;
                    for (const Pending& left : pending) {
                        queued.erase(left.key);
                    }
                    pending.clear();
                    pending_bytes = 0;
                }
            }
            if (pending.empty()) {
                drained_cv.notify_all();
            }
        }
    }

    std::unique_ptr<File> file;
    Hasher hasher;
    Clock now_us;
    const u32 slot_count;

    std::mutex mutex;
    std::condition_variable pending_cv;
    std::condition_variable drained_cv;
    std::vector<Slot> slots;
    std::vector<u32> free_slots;
    std::vector<u32> dropped_pinned;
    std::unordered_map<u64, u32> index;
    std::unordered_set<u64> queued;
    std::deque<Pending> pending;
    u64 pending_bytes{};
    u32 lru_head{NONE};
    u32 lru_tail{NONE};
    u32 write_failures_in_a_row{};
    bool writing{};
    bool stopping{};
    bool disabled{};
    Stats stats;
    std::thread writer;
};

} // namespace Jadyx
