// Exact emulation of the subset of .NET System.Decimal arithmetic that v1
// uses to convert ticks to microseconds (DirectCompactMidiParser.TempoClock):
// non-negative values only, scale-0 multiply, divide of a scale-0 value by a
// positive 32-bit integer, add, and Round(value, 0, AwayFromZero).
//
// .NET decimal is a 96-bit mantissa with a power-of-ten scale 0..28. Division
// keeps the largest scale whose quotient fits 96 bits (max 28) and rounds half
// to even; addition is exact at the larger scale and, on 96-bit overflow,
// drops the fewest digits needed, rounding half to even with a sticky
// remainder. Results therefore differ from exact rational arithmetic in the
// last digit, which can change a final rounding (see ARCHITECTURE.md §5.2).
#pragma once

#include <cstdint>
#include <string>

namespace midibn {

struct NetDecimal {
    uint32_t lo = 0;
    uint32_t mid = 0;
    uint32_t hi = 0;
    int scale = 0;
};

// (decimal)a * b with a < 2^64, b < 2^32: exact, scale 0 (always fits 96 bits).
NetDecimal decimal_multiply(uint64_t a, uint32_t b);
// dividend (scale 0) / divisor (> 0), System.Decimal division semantics.
NetDecimal decimal_divide(const NetDecimal& dividend, uint32_t divisor);
// a + b (both non-negative), System.Decimal addition semantics. The checked
// form returns false where .NET throws OverflowException (sum >= 2^96 at
// scale 0; never reached by v1's tempo arithmetic).
bool decimal_add_checked(const NetDecimal& a, const NetDecimal& b, NetDecimal& result);
NetDecimal decimal_add(const NetDecimal& a, const NetDecimal& b);
// Decimal.ToInt64(Decimal.Round(v, 0, MidpointRounding.AwayFromZero)).
// Returns false when the result does not fit Int64 (v1: OverflowException).
bool decimal_round_away_to_int64(const NetDecimal& value, int64_t& result);
// Invariant text with trailing fractional zeros removed (harness vectors).
std::string decimal_to_string(const NetDecimal& value);

} // namespace midibn
