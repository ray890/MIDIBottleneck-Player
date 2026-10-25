// Event time from ticks and tempo changes, in exact integer arithmetic
// (improvement DD-002, v2/docs/DIFFERENCES_FROM_V1.md).
//
// The time of a tick is
//     round_half_up( sum over tempo segments of (ticks in the segment x tempo) / ppqn )
// microseconds, where the tempo is microseconds per quarter note and ppqn the file's
// ticks per quarter note. The sum is kept as whole microseconds plus a remainder in
// units of 1/ppqn microsecond, so nothing is ever rounded until the final answer, and
// the answer is the correctly rounded value of the true time.
//
// v1 accumulated the same sum in .NET `decimal`, which rounds every non-terminating
// quotient (such as 1/3 microsecond) to 28 digits. When the true time lies exactly
// halfway between two whole microseconds, that rounding noise decides which way v1
// rounds, so v1 is occasionally one microsecond low (see the decimal-halfway
// fixture). In every other case the two results are identical.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace midibn {

struct TempoRecord {
    int64_t tick;
    int32_t tempo;  // microseconds per quarter note, > 0
    int32_t track;
    int32_t order;
};

// Converts non-decreasing ticks to microseconds. `changes` must be sorted by
// (tick, track, order) and outlive the clock.
class TempoClock {
public:
    TempoClock(const std::vector<TempoRecord>& changes, int ppqn);

    // Returns false when the time does not fit an Int64 (v1: OverflowException).
    bool time_at(int64_t tick, int64_t& micros)
    {
        if (cached_ && tick == cached_tick_) {   // the usual case in a dense file: many events on one tick
            micros = cached_micros_;
            return cached_ok_;
        }
        return compute(tick, micros);
    }

private:
    bool compute(int64_t tick, int64_t& micros);
    // Moves the cursor (the exact time at cursor_tick_) forward to `tick` at the current tempo.
    void advance_cursor(int64_t tick);

    struct Wide {  // unsigned 128-bit
        uint64_t hi;
        uint64_t lo;
    };

    const std::vector<TempoRecord>& changes_;
    uint32_t ppqn_;
    size_t next_ = 0;
    int64_t previous_tick_ = 0;
    int32_t tempo_ = 500000;
    Wide whole_ = { 0, 0 };    // whole microseconds up to previous_tick_
    uint32_t remainder_ = 0;   // plus remainder_ / ppqn microseconds
    // DD-056: the exact time at the last tick asked for, so the next (usually close) tick is one small step from it
    // instead of a long multiplication and division from the start of the tempo segment. Never earlier than previous_tick_.
    int64_t cursor_tick_ = 0;
    Wide cursor_whole_ = { 0, 0 };
    uint32_t cursor_remainder_ = 0;
    bool cached_ = false;
    int64_t cached_tick_ = 0;
    int64_t cached_micros_ = 0;
    bool cached_ok_ = true;
};

} // namespace midibn
