// The count behind the Queue mode's statistics snapshot (v1 PlaybackEngine.CountSnapshotEligible): how many events of
// [start, end) pass the live source filters (disabled channels and forced overrides), and the first one that does.
//
// v1 kept the result of the previous call and reused it only when the range kept its start (it then scanned only the
// new end). During playback the start is the next event the worker will send, so it moves on every call, and v1
// scanned the whole waiting backlog again on every display refresh, holding the engine lock. When the output falls
// behind (millions of events waiting) a refresh took seconds, the player stopped answering, and the worker, waiting for
// the same lock between its bursts, sent in chunks (register entry DD-093). This counter keeps, besides v1's cached
// range, the eligible count of each block of 4,096 events in it, so a call whose start moved forward subtracts the
// passed blocks instead of rescanning: the work per call is the events that became due plus the events sent since the
// previous call (at most one partial block is scanned to move the start), not the backlog.
//
// The results are exactly v1's for every sequence of calls: the count is exact, and the first eligible event after a
// moved start is found exactly (a full rescan in v1); calls that v1 answered from its cache (same start, same filters)
// are answered the same way here, including v1's handling of the first eligible event, which a call with an empty range
// or without filters clears or sets without touching the cache.
#pragma once

#include <cstdint>
#include <deque>

namespace midibn {
namespace engine {

class SnapshotEligibleCounter {
public:
    static constexpr int kBlockShift = 12;   // 4,096 events per block

    // v1 ResetStatistics: the cached range is forgotten (the filter generations are kept, as in v1).
    void reset()
    {
        start_ = -1;
        end_ = -1;
        total_ = 0;
        first_ = -1;
        first_block_ = 0;
        blocks_.clear();
    }

    // The first eligible event of the last counted range (-1: none), as v1's _snapshotFirstEligibleIndex.
    int32_t first_eligible() const { return first_; }

    // `eligible(index)` decides one event with the current filters; the generations identify those filters.
    template <typename Eligible>
    int64_t count(int32_t startInclusive, int32_t endExclusive, bool filtersActive, int32_t routingGeneration,
        int32_t overrideGeneration, Eligible&& eligible)
    {
        if (startInclusive >= endExclusive) {
            first_ = -1;
            return 0;
        }
        if (!filtersActive) {
            first_ = startInclusive;
            return static_cast<int64_t>(endExclusive) - startInclusive;
        }
        const bool sameFilters = routing_generation_ == routingGeneration && override_generation_ == overrideGeneration;
        if (sameFilters && start_ == startInclusive && end_ >= startInclusive && end_ <= endExclusive) {
            // v1's reuse: the same start; only the new end is scanned below.
        }
        else if (sameFilters && start_ >= 0 && start_ < startInclusive && startInclusive <= end_ && end_ <= endExclusive) {
            // The start moved forward inside the counted range: v1 rescanned everything; the same result from the blocks.
            advance(startInclusive, eligible);
            find_first(eligible);
        }
        else {
            start_ = startInclusive;
            end_ = startInclusive;
            total_ = 0;
            first_ = -1;
            first_block_ = block_of(startInclusive);
            blocks_.clear();
            routing_generation_ = routingGeneration;
            override_generation_ = overrideGeneration;
        }
        extend(endExclusive, eligible);
        return total_;
    }

private:
    static int32_t block_of(int32_t index) { return index >> kBlockShift; }
    static int64_t block_end(int32_t block) { return (static_cast<int64_t>(block) + 1) << kBlockShift; }

    // Drops [start_, newStart) from the count: whole blocks by their stored counts, the rest of a partial block by a scan.
    template <typename Eligible>
    void advance(int32_t newStart, Eligible& eligible)
    {
        while (start_ < newStart) {
            const int64_t blockEnd = block_end(block_of(start_));
            if (newStart >= blockEnd) {
                total_ -= blocks_.front();
                blocks_.pop_front();
                ++first_block_;
                start_ = static_cast<int32_t>(blockEnd);
            }
            else {
                int32_t passed = 0;
                for (int32_t index = start_; index < newStart; ++index)
                    if (eligible(index)) ++passed;
                blocks_.front() -= passed;
                total_ -= passed;
                start_ = newStart;
            }
        }
    }

    // The first eligible event of [start_, end_) (-1: none); blocks whose count is zero are skipped.
    template <typename Eligible>
    void find_first(Eligible& eligible)
    {
        first_ = -1;
        if (total_ <= 0) return;
        int32_t index = start_;
        size_t block = 0;
        while (index < end_ && block < blocks_.size()) {
            const int64_t blockEnd = block_end(block_of(index));
            const int32_t stop = static_cast<int32_t>(blockEnd < end_ ? blockEnd : end_);
            if (blocks_[block] > 0) {
                for (; index < stop; ++index)
                    if (eligible(index)) {
                        first_ = index;
                        return;
                    }
            }
            index = stop;
            ++block;
        }
    }

    // Counts [end_, newEnd) into the blocks; the first eligible event found is kept when none is known (as v1).
    template <typename Eligible>
    void extend(int32_t newEnd, Eligible& eligible)
    {
        int32_t index = end_;
        while (index < newEnd) {
            const int32_t block = block_of(index);
            if (blocks_.empty()) first_block_ = block;
            while (static_cast<int64_t>(first_block_) + static_cast<int64_t>(blocks_.size()) <= block) blocks_.push_back(0);
            const int64_t blockEnd = block_end(block);
            const int32_t stop = static_cast<int32_t>(blockEnd < newEnd ? blockEnd : newEnd);
            int32_t added = 0;
            for (; index < stop; ++index) {
                if (!eligible(index)) continue;
                if (first_ < 0) first_ = index;
                ++added;
            }
            blocks_.back() += added;
            total_ += added;
        }
        end_ = newEnd;
    }

    int32_t start_ = -1;
    int32_t end_ = -1;
    int64_t total_ = 0;
    int32_t first_ = -1;
    int32_t routing_generation_ = -1;
    int32_t override_generation_ = -1;
    int32_t first_block_ = 0;
    std::deque<int32_t> blocks_;   // eligible events of each block of [start_, end_), from block first_block_
};

} // namespace engine
} // namespace midibn
