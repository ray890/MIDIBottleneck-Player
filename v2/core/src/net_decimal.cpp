#include "midibn/net_decimal.h"

#include <algorithm>

namespace midibn {
namespace {

const uint32_t kPow10[10] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u,
    10000000u, 100000000u, 1000000000u };
const int kMaxScale = 28;

// 256-bit little-endian unsigned integer; enough for a 96-bit mantissa
// aligned by up to 10^28 (< 2^190).
struct Wide {
    uint32_t w[8];
};

Wide from_decimal(const NetDecimal& d)
{
    Wide r = {};
    r.w[0] = d.lo;
    r.w[1] = d.mid;
    r.w[2] = d.hi;
    return r;
}

NetDecimal to_decimal(const Wide& v, int scale)
{
    NetDecimal d;
    d.lo = v.w[0];
    d.mid = v.w[1];
    d.hi = v.w[2];
    d.scale = scale;
    return d;
}

void mul_small(Wide& a, uint32_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < 8; ++i) {
        uint64_t t = static_cast<uint64_t>(a.w[i]) * m + carry;
        a.w[i] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
}

void add_small(Wide& a, uint32_t v)
{
    uint64_t carry = v;
    for (int i = 0; i < 8 && carry != 0; ++i) {
        uint64_t t = static_cast<uint64_t>(a.w[i]) + carry;
        a.w[i] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
}

void add_wide(Wide& a, const Wide& b)
{
    uint64_t carry = 0;
    for (int i = 0; i < 8; ++i) {
        uint64_t t = static_cast<uint64_t>(a.w[i]) + b.w[i] + carry;
        a.w[i] = static_cast<uint32_t>(t);
        carry = t >> 32;
    }
}

uint32_t divmod_small(Wide& a, uint32_t d)
{
    uint64_t r = 0;
    for (int i = 7; i >= 0; --i) {
        uint64_t cur = (r << 32) | a.w[i];
        a.w[i] = static_cast<uint32_t>(cur / d);
        r = cur % d;
    }
    return static_cast<uint32_t>(r);
}

bool fits96(const Wide& a)
{
    for (int i = 3; i < 8; ++i)
        if (a.w[i] != 0) return false;
    return true;
}

void scale_up(Wide& a, int digits)
{
    while (digits > 0) {
        int p = std::min(digits, 9);
        mul_small(a, kPow10[p]);
        digits -= p;
    }
}

// A rounded-up result that reached exactly 2^96: .NET divides it by ten once
// more (remainder 6 always rounds up) and lowers the scale.
void round_up_overflow(Wide& q, int& scale)
{
    divmod_small(q, 10);
    add_small(q, 1);
    --scale;
}

// Increments a 96-bit result, unscaling if it reaches exactly 2^96.
void round_up(Wide& q, int& scale)
{
    add_small(q, 1);
    if (!fits96(q)) round_up_overflow(q, scale);
}

} // namespace

NetDecimal decimal_multiply(uint64_t a, uint32_t b)
{
    Wide r = {};
    r.w[0] = static_cast<uint32_t>(a);
    r.w[1] = static_cast<uint32_t>(a >> 32);
    mul_small(r, b);
    return to_decimal(r, 0);
}

NetDecimal decimal_divide(const NetDecimal& dividend, uint32_t divisor)
{
    Wide q = from_decimal(dividend);
    uint32_t remainder = divmod_small(q, divisor);
    int scale = dividend.scale;
    while (remainder != 0 && scale < kMaxScale) {
        bool applied = false;
        for (int p = std::min(9, kMaxScale - scale); p >= 1; --p) {
            Wide candidate = q;
            mul_small(candidate, kPow10[p]);
            uint64_t scaled = static_cast<uint64_t>(remainder) * kPow10[p];
            add_small(candidate, static_cast<uint32_t>(scaled / divisor));
            if (fits96(candidate)) {
                q = candidate;
                remainder = static_cast<uint32_t>(scaled % divisor);
                scale += p;
                applied = true;
                break;
            }
        }
        if (!applied) break;
    }
    if (remainder != 0) {
        uint64_t twice = static_cast<uint64_t>(remainder) * 2u;
        if (twice > divisor || (twice == divisor && (q.w[0] & 1u) != 0))
            round_up(q, scale);
    }
    // .NET keeps no trailing zeros beyond the dividend's scale, whether the
    // quotient is exact (1/2 is 0.5 at scale 1, not 0.500000000 at scale 9) or
    // rounded (1/999999 ends at scale 24). The value is unchanged.
    while (scale > dividend.scale) {
        Wide candidate = q;
        if (divmod_small(candidate, 10) != 0) break;
        q = candidate;
        --scale;
    }
    return to_decimal(q, scale);
}

bool decimal_add_checked(const NetDecimal& a, const NetDecimal& b, NetDecimal& result)
{
    int scale = std::max(a.scale, b.scale);
    Wide x = from_decimal(a);
    Wide y = from_decimal(b);
    scale_up(x, scale - a.scale);
    scale_up(y, scale - b.scale);
    add_wide(x, y);
    if (fits96(x)) { result = to_decimal(x, scale); return true; }

    uint32_t last = 0;
    bool sticky = false;
    while (!fits96(x)) {
        if (scale == 0) return false;  // .NET: OverflowException
        sticky = sticky || last != 0;
        last = divmod_small(x, 10);
        --scale;
    }
    if (last > 5 || (last == 5 && (sticky || (x.w[0] & 1u) != 0))) {
        add_small(x, 1);
        if (!fits96(x)) {
            if (scale == 0) return false;
            round_up_overflow(x, scale);
        }
    }
    result = to_decimal(x, scale);
    return true;
}

NetDecimal decimal_add(const NetDecimal& a, const NetDecimal& b)
{
    NetDecimal result;
    decimal_add_checked(a, b, result);
    return result;
}

bool decimal_round_away_to_int64(const NetDecimal& value, int64_t& result)
{
    Wide q = from_decimal(value);
    int remaining = value.scale;
    uint32_t last = 0;
    uint32_t lastDivisor = 1;
    while (remaining > 0) {
        int p = std::min(remaining, 9);
        lastDivisor = kPow10[p];
        last = divmod_small(q, lastDivisor);
        remaining -= p;
    }
    if (value.scale > 0 && last >= (lastDivisor >> 1)) add_small(q, 1);
    if (q.w[2] != 0 || (q.w[1] & 0x80000000u) != 0 || !fits96(q)) return false;
    result = static_cast<int64_t>((static_cast<uint64_t>(q.w[1]) << 32) | q.w[0]);
    return true;
}

std::string decimal_to_string(const NetDecimal& value)
{
    Wide v = from_decimal(value);
    std::string digits;
    bool zero = true;
    for (int i = 0; i < 3; ++i) if (v.w[i] != 0) zero = false;
    if (zero) digits = "0";
    while (!zero) {
        uint32_t chunk = divmod_small(v, 1000000000u);
        zero = v.w[0] == 0 && v.w[1] == 0 && v.w[2] == 0;
        for (int i = 0; i < 9 && (!zero || chunk != 0); ++i) {
            digits.push_back(static_cast<char>('0' + chunk % 10));
            chunk /= 10;
        }
    }
    // digits is little-endian; pad so there is at least one integer digit.
    while (static_cast<int>(digits.size()) <= value.scale) digits.push_back('0');
    std::reverse(digits.begin(), digits.end());
    if (value.scale == 0) return digits;
    std::string integer = digits.substr(0, digits.size() - value.scale);
    std::string fraction = digits.substr(digits.size() - value.scale);
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    return fraction.empty() ? integer : integer + "." + fraction;
}

} // namespace midibn
