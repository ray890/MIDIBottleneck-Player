// .NET Framework 4.8 number formatting as v1 uses it (CurrentCulture), for
// the portable UI layer (UI_DIFFERENCES.md). Everything is UTF-8 text.
//
// double.ToString("N<d>") / ("0.###"): the value is first converted to 15
// significant digits (the exact binary value, rounded half up on the 16th
// digit), then rounded half up again at the requested decimal position; a
// result that rounds to zero loses its sign. These routines reproduce that
// digit by digit, with their own exact binary-to-decimal expansion, so they
// do not depend on the C runtime's printf.
#pragma once

#include <cstdint>
#include <string>

namespace midibn {
namespace format {

// The two separators v1 reads from CultureInfo.CurrentCulture.NumberFormat
// (NumberDecimalSeparator, NumberGroupSeparator). Group size is always 3 and
// the negative pattern is "-n", as in en-US and de-DE.
struct NumberCulture {
    std::string decimal_separator = ".";
    std::string group_separator = ",";
};

// double.ToString("N<decimals>", culture) with grouping by 3.
std::string format_n(double value, int decimals, const NumberCulture& culture = NumberCulture());
// long.ToString("N0", culture).
std::string format_n0(int64_t value, const NumberCulture& culture = NumberCulture());
// double.ToString("0.###"-style custom format with maxDecimals '#' places):
// no grouping, trailing zeros dropped, at least one integer digit.
std::string format_trimmed(double value, int maxDecimals, const NumberCulture& culture = NumberCulture());
// long/int.ToString(culture): digits with a leading "-" when negative, no grouping.
std::string format_integer(int64_t value);

// Math.Round(x) (round half to even) converted with (long): NaN and values
// outside the Int64 range give Int64.MinValue, as the x64 .NET conversion does.
int64_t round_half_even(double value);
// Math.Round(x, MidpointRounding.AwayFromZero) converted with (long).
int64_t round_away_from_zero(double value);
// The same two roundings, left as doubles (Math.Round results).
double round_half_even_double(double value);
double round_away_from_zero_double(double value);
// C# (long)double / (int)double conversions (x64 behaviour: truncate toward
// zero; out of range or NaN gives the "indefinite" value Int64.MinValue /
// Int32.MinValue).
int64_t double_to_int64(double value);
int32_t double_to_int32(double value);

} // namespace format
} // namespace midibn
