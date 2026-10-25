#include "midibn/format/net_format.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace midibn {
namespace format {
namespace {

// ---- exact binary -> decimal expansion --------------------------------------

typedef std::vector<uint32_t> Limbs;   // base 1e9, least significant first
const uint32_t kBase = 1000000000u;

void multiply_small(Limbs& v, uint32_t m)
{
    uint64_t carry = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        uint64_t cur = static_cast<uint64_t>(v[i]) * m + carry;
        v[i] = static_cast<uint32_t>(cur % kBase);
        carry = cur / kBase;
    }
    while (carry != 0) {
        v.push_back(static_cast<uint32_t>(carry % kBase));
        carry /= kBase;
    }
}

std::string limbs_to_string(const Limbs& v)
{
    std::string text;
    char piece[16];
    for (size_t i = v.size(); i-- > 0;) {
        if (text.empty()) {
            std::snprintf(piece, sizeof piece, "%u", v[i]);
        } else {
            std::snprintf(piece, sizeof piece, "%09u", v[i]);
        }
        text += piece;
    }
    return text;
}

// The digits .NET's Number carries: at most 15 significant digits, no
// trailing zeros (an empty string is zero); value = 0.DIGITS * 10^scale.
struct DecimalDigits {
    std::string digits;
    int scale = 0;
    bool negative = false;
};

DecimalDigits double_to_digits15(double value)
{
    DecimalDigits result;
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    result.negative = (bits >> 63) != 0;
    int exponent = static_cast<int>((bits >> 52) & 0x7FF);
    uint64_t mantissa = bits & 0xFFFFFFFFFFFFFull;
    if (exponent == 0) {
        exponent = 1;
    } else {
        mantissa |= 1ull << 52;
    }
    if (mantissa == 0) return result;   // zero: no digits, scale 0
    exponent -= 1075;

    Limbs n;
    while (mantissa != 0) {
        n.push_back(static_cast<uint32_t>(mantissa % kBase));
        mantissa /= kBase;
    }
    int decimalExponent = 0;
    if (exponent >= 0) {
        int remaining = exponent;
        while (remaining > 0) {
            int step = remaining > 28 ? 28 : remaining;
            multiply_small(n, 1u << step);
            remaining -= step;
        }
    } else {
        int remaining = -exponent;
        while (remaining > 0) {
            int step = remaining > 13 ? 13 : remaining;
            uint32_t power = 1;
            for (int i = 0; i < step; ++i) power *= 5;
            multiply_small(n, power);
            remaining -= step;
        }
        decimalExponent = exponent;
    }
    std::string text = limbs_to_string(n);
    int scale = static_cast<int>(text.size()) + decimalExponent;
    if (text.size() > 15) {
        bool up = text[15] >= '5';
        text.resize(15);
        if (up) {
            int i = 14;
            while (i >= 0 && text[static_cast<size_t>(i)] == '9') {
                text[static_cast<size_t>(i)] = '0';
                --i;
            }
            if (i < 0) {
                text.insert(text.begin(), '1');
                text.resize(15);
                ++scale;
            } else {
                ++text[static_cast<size_t>(i)];
            }
        }
    }
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (text.empty()) return result;
    result.digits = text;
    result.scale = scale;
    return result;
}

// Number.RoundNumber(ref number, pos) of .NET Framework 4.8.
void round_number(DecimalDigits& number, int pos)
{
    std::string& dig = number.digits;
    int length = static_cast<int>(dig.size());
    int i = 0;
    while (i < pos && i < length) ++i;
    if (i == pos && i < length && dig[static_cast<size_t>(i)] >= '5') {
        while (i > 0 && dig[static_cast<size_t>(i - 1)] == '9') --i;
        if (i > 0) {
            ++dig[static_cast<size_t>(i - 1)];
        } else {
            ++number.scale;
            dig[0] = '1';
            i = 1;
        }
    } else {
        while (i > 0 && dig[static_cast<size_t>(i - 1)] == '0') --i;
    }
    if (i == 0) {
        number.scale = 0;
        number.negative = false;
        dig.clear();
    } else {
        dig.resize(static_cast<size_t>(i));
    }
}

bool special_value(double value, const NumberCulture&, std::string& text)
{
    if (std::isnan(value)) {
        text = "NaN";
        return true;
    }
    if (std::isinf(value)) {
        // en-US and de-DE on .NET Framework 4.8 / Windows 10: U+221E.
        text = value < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E";
        return true;
    }
    return false;
}

std::string group_digits(const std::string& integerDigits, const NumberCulture& culture)
{
    std::string out;
    size_t length = integerDigits.size();
    for (size_t i = 0; i < length; ++i) {
        if (i != 0 && (length - i) % 3 == 0) out += culture.group_separator;
        out += integerDigits[i];
    }
    return out;
}

char digit_at(const DecimalDigits& number, int index)
{
    if (index >= 0 && index < static_cast<int>(number.digits.size())) return number.digits[static_cast<size_t>(index)];
    return '0';
}

std::string integer_part(const DecimalDigits& number)
{
    std::string text;
    if (number.scale > 0) {
        for (int i = 0; i < number.scale; ++i) text += digit_at(number, i);
    } else {
        text = "0";
    }
    return text;
}

} // namespace

std::string format_n(double value, int decimals, const NumberCulture& culture)
{
    std::string special;
    if (special_value(value, culture, special)) return special;
    if (decimals < 0) decimals = 0;
    DecimalDigits number = double_to_digits15(value);
    round_number(number, number.scale + decimals);
    std::string text;
    if (number.negative) text += '-';
    text += group_digits(integer_part(number), culture);
    if (decimals > 0) {
        text += culture.decimal_separator;
        for (int k = 0; k < decimals; ++k) text += digit_at(number, number.scale + k);
    }
    return text;
}

std::string format_n0(int64_t value, const NumberCulture& culture)
{
    bool negative = value < 0;
    uint64_t magnitude = negative ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    std::string text;
    if (negative) text += '-';
    text += group_digits(digits, culture);
    return text;
}

std::string format_trimmed(double value, int maxDecimals, const NumberCulture& culture)
{
    std::string special;
    if (special_value(value, culture, special)) return special;
    if (maxDecimals < 0) maxDecimals = 0;
    DecimalDigits number = double_to_digits15(value);
    round_number(number, number.scale + maxDecimals);
    std::string text;
    if (number.negative) text += '-';
    text += integer_part(number);
    int lastFraction = 0;
    for (int k = 0; k < maxDecimals; ++k) {
        if (digit_at(number, number.scale + k) != '0') lastFraction = k + 1;
    }
    if (lastFraction > 0) {
        text += culture.decimal_separator;
        for (int k = 0; k < lastFraction; ++k) text += digit_at(number, number.scale + k);
    }
    return text;
}

std::string format_integer(int64_t value)
{
    bool negative = value < 0;
    uint64_t magnitude = negative ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    std::string text;
    if (negative) text += '-';
    text += std::to_string(magnitude);
    return text;
}

double round_half_even_double(double value)
{
    if (std::isnan(value) || std::isinf(value)) return value;
    return std::nearbyint(value);
}

double round_away_from_zero_double(double value)
{
    if (std::isnan(value) || std::isinf(value)) return value;
    double whole = 0;
    double fraction = std::modf(value, &whole);
    if (std::fabs(fraction) >= 0.5) whole += fraction > 0 ? 1.0 : -1.0;
    return whole;
}

int64_t double_to_int64(double value)
{
    // 2^63 as a double is exact; values at or beyond it, and NaN, are "indefinite".
    if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) return INT64_MIN;
    return static_cast<int64_t>(value);
}

int32_t double_to_int32(double value)
{
    if (!(value > -2147483649.0 && value < 2147483648.0)) return INT32_MIN;
    return static_cast<int32_t>(value);
}

int64_t round_half_even(double value) { return double_to_int64(round_half_even_double(value)); }

int64_t round_away_from_zero(double value) { return double_to_int64(round_away_from_zero_double(value)); }

} // namespace format
} // namespace midibn
