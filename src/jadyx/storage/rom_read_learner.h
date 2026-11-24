// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

namespace Jadyx {

/// Learns in which order a game reads the blocks of one of its files, so a later visit to the
/// same place can have the next ones read ahead. A game on a USB hard drive waits about a seek
/// (10 to 20 ms) for each read the drive has not cached: Metroid Prime Remastered lost 190 to
/// 225 ms a minute that way. Reading ahead blindly (the next blocks of the file) read 6.75 GB to
/// use 18% of it and got in the way of the game's own reads; this reads what the game read next
/// the last time it was at the same block, in that order, and only what followed it closely.
///
/// The journal holds every block the game touched, once per DEDUPE_MS, with the time since its
/// session began; sessions are kept apart by a marker. It is saved between runs and keeps the
/// newest MAX_ENTRIES. Not thread safe: the owner serializes calls.
class RomReadLearner {
public:
    static constexpr std::uint32_t SESSION_MARK = 0xFFFFFFFFu;
    static constexpr std::size_t MAX_ENTRIES = 1u << 19; ///< 4 MiB saved
    static constexpr std::uint32_t DEDUPE_MS = 5000;
    /// Read ahead only what followed within this of the matched block in the journal: far
    /// enough to fill the disk's idle time before the game's next burst of reads.
    static constexpr std::uint32_t FOLLOW_MS = 8000;
    static constexpr std::size_t DEPTH = 160; ///< blocks (40 MiB) handed out ahead of the match
    static constexpr std::size_t MAX_BLOCKS_PER_READ = 64;
    /// The newest entries have no successors yet: they are not matched.
    static constexpr std::size_t TAIL_SKIP = 64;

    struct Entry {
        std::uint32_t block;
        std::uint32_t ms;
    };

    /// Takes a saved journal (Serialize's format); false and empty when it is not one.
    bool Load(std::span<const std::uint8_t> bytes) {
        entries.clear();
        index.clear();
        if (bytes.size() < 8 || std::memcmp(bytes.data(), MAGIC, 4) != 0) {
            StartSession();
            return false;
        }
        std::uint32_t count{};
        std::memcpy(&count, bytes.data() + 4, 4);
        if (bytes.size() < 8 + std::size_t{count} * sizeof(Entry)) {
            StartSession();
            return false;
        }
        entries.resize(count);
        std::memcpy(entries.data(), bytes.data() + 8, std::size_t{count} * sizeof(Entry));
        loaded = count;
        StartSession();
        RebuildIndex();
        return true;
    }

    /// The journal to save: the newest MAX_ENTRIES of the loaded one and this session's.
    [[nodiscard]] std::vector<std::uint8_t> Serialize() const {
        const std::size_t first = entries.size() > MAX_ENTRIES ? entries.size() - MAX_ENTRIES : 0;
        const auto count = static_cast<std::uint32_t>(entries.size() - first);
        std::vector<std::uint8_t> out(8 + std::size_t{count} * sizeof(Entry));
        std::memcpy(out.data(), MAGIC, 4);
        std::memcpy(out.data() + 4, &count, 4);
        std::memcpy(out.data() + 8, entries.data() + first, std::size_t{count} * sizeof(Entry));
        return out;
    }

    /// Records the blocks [first, last] of a read made at now_ms (since this session began) and
    /// returns the blocks to read ahead, in the order the game read them last time.
    std::vector<std::uint32_t> OnRead(std::uint32_t first, std::uint32_t last,
                                      std::uint32_t now_ms) {
        last = std::min<std::uint32_t>(last, first + MAX_BLOCKS_PER_READ - 1);
        const std::size_t before = entries.size();
        const std::size_t match = Match(first);
        for (std::uint32_t block = first; block <= last; ++block) {
            Record(block, now_ms);
        }
        if (entries.size() != before) {
            ++recorded_reads;
        }
        std::vector<std::uint32_t> ahead;
        if (match == NONE) {
            cursor = NONE;
            return ahead;
        }
        ++matched_reads;
        // A jump elsewhere in the journal starts handing out again from there.
        if (cursor == NONE || match < cursor || match > handed_until) {
            handed_until = match;
        }
        cursor = match;
        const std::uint32_t base_ms = entries[match].ms;
        const std::size_t stop = std::min(entries.size(), match + 1 + DEPTH);
        for (std::size_t at = match + 1; at < stop; ++at) {
            const Entry& entry = entries[at];
            if (entry.block == SESSION_MARK || entry.ms < base_ms ||
                entry.ms - base_ms > FOLLOW_MS) {
                break;
            }
            if (at <= handed_until) {
                continue;
            }
            handed_until = at;
            if (entry.block < first || entry.block > last) {
                ahead.push_back(entry.block);
            }
        }
        return ahead;
    }

    [[nodiscard]] bool Dirty() const {
        return entries.size() != saved_size;
    }
    void MarkSaved() {
        saved_size = entries.size();
    }

    [[nodiscard]] std::size_t LoadedEntries() const {
        return loaded;
    }
    [[nodiscard]] std::size_t SessionEntries() const {
        return entries.size() - session_begin;
    }
    [[nodiscard]] std::uint64_t MatchedReads() const {
        return matched_reads;
    }

private:
    static constexpr char MAGIC[4] = {'J', 'R', 'L', '1'};
    static constexpr std::size_t NONE = ~std::size_t{0};

    void StartSession() {
        if (!entries.empty() && entries.back().block != SESSION_MARK) {
            entries.push_back({SESSION_MARK, 0});
        }
        session_begin = entries.size();
        saved_size = entries.size();
    }

    void RebuildIndex() {
        index.clear();
        for (std::size_t at = 0; at < entries.size(); ++at) {
            if (entries[at].block != SESSION_MARK) {
                index[entries[at].block].push_back(static_cast<std::uint32_t>(at));
            }
        }
    }

    void Record(std::uint32_t block, std::uint32_t now_ms) {
        const auto [seen, fresh] = recent.try_emplace(block, now_ms);
        if (!fresh) {
            if (now_ms - seen->second < DEDUPE_MS) {
                return;
            }
            seen->second = now_ms;
        }
        index[block].push_back(static_cast<std::uint32_t>(entries.size()));
        entries.push_back({block, now_ms});
        if (entries.size() > MAX_ENTRIES + MAX_ENTRIES / 4) {
            Compact();
        }
    }

    /// The journal position to follow for a read starting at block: the next occurrence close
    /// after the last match (the game goes on where it went last time), else the newest one
    /// with successors.
    std::size_t Match(std::uint32_t block) const {
        const auto found = index.find(block);
        if (found == index.end()) {
            return NONE;
        }
        const auto& positions = found->second;
        const std::size_t limit = entries.size() > TAIL_SKIP ? entries.size() - TAIL_SKIP : 0;
        if (cursor != NONE) {
            const auto next = std::upper_bound(positions.begin(), positions.end(),
                                               static_cast<std::uint32_t>(cursor));
            if (next != positions.end() && *next <= cursor + 2 * DEPTH && *next < limit) {
                return *next;
            }
        }
        const auto newest = std::lower_bound(positions.begin(), positions.end(),
                                             static_cast<std::uint32_t>(limit));
        if (newest == positions.begin()) {
            return NONE;
        }
        return *std::prev(newest);
    }

    /// Drops the oldest entries past MAX_ENTRIES.
    void Compact() {
        const std::size_t drop = entries.size() - MAX_ENTRIES;
        entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(drop));
        loaded = loaded > drop ? loaded - drop : 0;
        session_begin = session_begin > drop ? session_begin - drop : 0;
        saved_size = saved_size > drop ? saved_size - drop : 0;
        cursor = NONE;
        handed_until = 0;
        RebuildIndex();
    }

    std::vector<Entry> entries;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> index;
    std::unordered_map<std::uint32_t, std::uint32_t> recent; ///< block -> last recorded ms
    std::size_t loaded{};
    std::size_t session_begin{};
    std::size_t saved_size{};
    std::size_t cursor{NONE};
    std::size_t handed_until{};
    std::uint64_t recorded_reads{};
    std::uint64_t matched_reads{};
};

} // namespace Jadyx
