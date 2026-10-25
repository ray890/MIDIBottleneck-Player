#include "midibn/tempo_clock.h"

namespace midibn {
namespace {

// a x b as an unsigned 128-bit value (a < 2^64, b < 2^32), plus `addend`.
void multiply_add(uint64_t a, uint32_t b, uint32_t addend, uint64_t& hi, uint64_t& lo)
{
    const uint64_t low = (a & 0xFFFFFFFFull) * b;
    const uint64_t high = (a >> 32) * b;
    lo = low + (high << 32);
    hi = (high >> 32) + (lo < low ? 1 : 0);
    const uint64_t before = lo;
    lo += addend;
    if (lo < before) ++hi;
}

// (hi:lo) / divisor with the remainder; divisor < 2^32 and nonzero.
uint32_t divide_remainder(uint64_t hi, uint64_t lo, uint32_t divisor, uint64_t& quotient_hi, uint64_t& quotient_lo)
{
    if (hi == 0) {
        quotient_hi = 0;
        if ((lo >> 32) == 0) {  // the usual case: a 32-bit division
            const uint32_t small = static_cast<uint32_t>(lo);
            const uint32_t q = small / divisor;
            quotient_lo = q;
            return small - q * divisor;
        }
        quotient_lo = lo / divisor;
        return static_cast<uint32_t>(lo - quotient_lo * divisor);
    }
    quotient_hi = hi / divisor;
    uint64_t rest = hi - quotient_hi * divisor;
    uint64_t numerator = (rest << 32) | (lo >> 32);
    const uint64_t high_part = numerator / divisor;
    rest = numerator - high_part * divisor;
    numerator = (rest << 32) | (lo & 0xFFFFFFFFull);
    const uint64_t low_part = numerator / divisor;
    quotient_lo = (high_part << 32) | low_part;
    return static_cast<uint32_t>(numerator - low_part * divisor);
}

} // namespace

TempoClock::TempoClock(const std::vector<TempoRecord>& changes, int ppqn)
    : changes_(changes), ppqn_(static_cast<uint32_t>(ppqn))
{
}

void TempoClock::advance_cursor(int64_t tick)
{
    // The time of `tick` is the time at the cursor plus (ticks since the cursor x tempo + remainder) / ppqn; flooring
    // the sum in two steps gives the same whole microseconds and remainder as one step from the start of the segment.
    if (tick < cursor_tick_) {   // not a later tick: start again from the segment's start
        cursor_tick_ = previous_tick_;
        cursor_whole_ = whole_;
        cursor_remainder_ = remainder_;
    }
    uint64_t hi, lo, quotient_hi, quotient_lo;
    multiply_add(static_cast<uint64_t>(tick - cursor_tick_), static_cast<uint32_t>(tempo_), cursor_remainder_, hi, lo);
    cursor_remainder_ = divide_remainder(hi, lo, ppqn_, quotient_hi, quotient_lo);
    const uint64_t before = cursor_whole_.lo;
    cursor_whole_.lo += quotient_lo;
    cursor_whole_.hi += quotient_hi + (cursor_whole_.lo < before ? 1 : 0);
    cursor_tick_ = tick;
}

bool TempoClock::compute(int64_t tick, int64_t& micros)
{
    while (next_ < changes_.size() && changes_[next_].tick <= tick) {
        const TempoRecord& change = changes_[next_++];
        // Fold the finished segment into whole microseconds and a remainder.
        advance_cursor(change.tick);
        whole_ = cursor_whole_;
        remainder_ = cursor_remainder_;
        previous_tick_ = change.tick;
        tempo_ = change.tempo;
    }

    // Time of `tick` inside the current segment: whole + (ticks x tempo + remainder) / ppqn, rounded half up.
    advance_cursor(tick);
    uint64_t total_hi = cursor_whole_.hi;
    uint64_t total_lo = cursor_whole_.lo;
    if (static_cast<uint64_t>(cursor_remainder_) * 2 >= ppqn_) {
        if (++total_lo == 0) ++total_hi;
    }
    bool ok = total_hi == 0 && total_lo <= static_cast<uint64_t>(INT64_MAX);
    cached_ = true;
    cached_tick_ = tick;
    cached_ok_ = ok;
    cached_micros_ = ok ? static_cast<int64_t>(total_lo) : 0;
    micros = cached_micros_;
    return ok;
}

} // namespace midibn
