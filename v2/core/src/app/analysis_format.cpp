#include "midibn/app/analysis_format.h"

#include "midibn/format/display_format.h"

#include <algorithm>
#include <vector>

namespace midibn {
namespace app {

namespace {

// 4-digit-or-more "00"-style integer.
std::string pad_int(int64_t value, int width)
{
    std::string digits = std::to_string(value < 0 ? 0 : value);
    while (static_cast<int>(digits.size()) < width) digits.insert(digits.begin(), '0');
    return digits;
}

// v1: TimeSpan.FromTicks(Math.Max(0, microseconds) * 10), the product wrapping like unchecked C#.
int64_t ticks_of(int64_t microseconds)
{
    if (microseconds < 0) microseconds = 0;
    return static_cast<int64_t>(static_cast<uint64_t>(microseconds) * 10u);
}

// UTF-8 text as UTF-16 code units (invalid bytes become U+FFFD).
std::vector<char16_t> to_utf16(const std::string& text)
{
    std::vector<char16_t> out;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0xFFFD;
        size_t length = 1;
        if (c < 0x80) cp = c;
        else if (c >= 0xC2 && c < 0xE0 && i + 1 < text.size() && (text[i + 1] & 0xC0) == 0x80) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            length = 2;
        } else if (c >= 0xE0 && c < 0xF0 && i + 2 < text.size() && (text[i + 1] & 0xC0) == 0x80 && (text[i + 2] & 0xC0) == 0x80) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            length = 3;
        } else if (c >= 0xF0 && c < 0xF5 && i + 3 < text.size() && (text[i + 1] & 0xC0) == 0x80 && (text[i + 2] & 0xC0) == 0x80 &&
            (text[i + 3] & 0xC0) == 0x80) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            length = 4;
        }
        i += length;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string to_utf8(const char16_t* units, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;   // a lone surrogate has no UTF-8 form
        }
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Char.IsWhiteSpace.
bool is_white_space(char16_t c)
{
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
        c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

} // namespace

std::string analysis_format_bytes(int64_t bytes, const format::NumberCulture& c)
{
    if (bytes <= 0) return format::kEmDash;
    if (bytes >= 1024LL * 1024LL) return format::format_n(static_cast<double>(bytes) / (1024.0 * 1024.0), 2, c) + " MiB";
    if (bytes >= 1024LL) return format::format_n(static_cast<double>(bytes) / 1024.0, 1, c) + " KiB";
    return format::format_n0(bytes, c) + " bytes";
}

std::string analysis_format_resolution(int64_t microseconds, const format::NumberCulture& c)
{
    return microseconds >= 1000000
        ? format::format_trimmed(static_cast<double>(microseconds) / 1000000.0, 3, c) + " s"
        : format::format_trimmed(static_cast<double>(microseconds) / 1000.0, 3, c) + " ms";
}

std::string analysis_format_overflow_policy(models::OverflowPolicy policy)
{
    using models::OverflowPolicy;
    if (policy == OverflowPolicy::DropOldest) return "Drop oldest events";
    if (policy == OverflowPolicy::DropOldestCompleteNote) return "Drop oldest complete note";
    if (policy == OverflowPolicy::ClearBufferAndCatchUp) return "Clear buffer and jump to realtime";
    if (policy == OverflowPolicy::DropIncomingCompleteNotes) return "Drop incoming complete notes";
    if (policy == OverflowPolicy::DropBackgroundNotes) return "Drop background notes";
    return "Drop newest events";
}

std::string analysis_format_clock(int64_t microseconds) { return format::format_time(microseconds); }

std::string analysis_format_clock_short(int64_t microseconds)
{
    int64_t ticks = ticks_of(microseconds);
    return pad_int(static_cast<int32_t>(ticks / 600000000), 2) + ":" + pad_int((ticks / 10000000) % 60, 2);
}

std::string analysis_format_interval_boundary(int64_t microseconds)
{
    if (microseconds < 0) microseconds = 0;
    int64_t minutes = microseconds / 60000000LL;
    int64_t remainder = microseconds % 60000000LL;
    int64_t seconds = remainder / 1000000LL;
    int64_t fraction = remainder % 1000000LL;
    return pad_int(minutes, 2) + ":" + pad_int(seconds, 2) + "." + pad_int(fraction, 6);
}

std::string analysis_format_axis_clock(int64_t microseconds, int64_t intervalMicroseconds)
{
    int64_t ticks = ticks_of(microseconds);
    int64_t totalSeconds = ticks / 10000000;
    int64_t seconds = totalSeconds % 60;
    int64_t minutes = (totalSeconds / 60) % 60;
    std::string prefix;
    if (ticks >= 36000000000LL) prefix = pad_int(totalSeconds / 3600, 2) + ":" + pad_int(minutes, 2) + ":" + pad_int(seconds, 2);
    else prefix = pad_int(totalSeconds / 60, 2) + ":" + pad_int(seconds, 2);
    if (intervalMicroseconds < 1000000) return prefix + "." + pad_int((ticks / 10000) % 1000, 3);
    return prefix;
}

int64_t choose_timeline_tick_interval(int64_t visibleSpanMicroseconds, int plotWidth)
{
    static const int64_t nice[] = { 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000,
        10000000, 15000000, 30000000, 60000000, 120000000, 300000000, 600000000, 900000000, 1800000000LL, 3600000000LL,
        7200000000LL, 18000000000LL, 36000000000LL };
    int64_t span = std::max<int64_t>(1, visibleSpanMicroseconds);
    int64_t targetTicks = std::max<int64_t>(2, plotWidth / 76);
    int64_t minimum = std::max<int64_t>(1,
        static_cast<int64_t>(static_cast<uint64_t>(span) + static_cast<uint64_t>(targetTicks) - 1u) / targetTicks);
    for (size_t i = 0; i < sizeof(nice) / sizeof(nice[0]); ++i)
        if (nice[i] >= minimum) return nice[i];
    const int64_t hours = 3600000000LL;
    int64_t multiples = (minimum + hours - 1) / hours;
    return std::max<int64_t>(1, multiples) * hours;
}

std::string concise_analysis_failure(const std::string& typeName, const std::string& message)
{
    std::vector<char16_t> units = to_utf16(message);
    bool blank = true;
    for (size_t i = 0; i < units.size(); ++i)
        if (!is_white_space(units[i])) { blank = false; break; }
    if (blank) units = to_utf16(typeName.empty() ? std::string("Exception") : typeName);
    for (size_t i = 0; i < units.size(); ++i)
        if (units[i] == u'\r' || units[i] == u'\n') units[i] = u' ';
    size_t first = 0, last = units.size();
    while (first < last && is_white_space(units[first])) ++first;
    while (last > first && is_white_space(units[last - 1])) --last;
    size_t length = last - first;
    if (length <= 240) return to_utf8(units.data() + first, length);
    return to_utf8(units.data() + first, 237) + format::kEllipsis;
}

std::string analysis_file_name(const std::string& path)
{
    size_t cut = path.find_last_of("\\/:");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string abbreviate_playback_counts(const std::string& value, const format::NumberCulture& culture)
{
    std::string result;
    size_t i = 0;
    const std::string& group = culture.group_separator;
    while (i < value.size()) {
        if (value[i] < '0' || value[i] > '9') { result += value[i++]; continue; }
        size_t start = i;
        // digits, and group separators between digits (v1 takes only ',' here; the culture's own one works in v2)
        while (i < value.size()) {
            if (value[i] >= '0' && value[i] <= '9') { ++i; continue; }
            if (!group.empty() && value.compare(i, group.size(), group) == 0 && i + group.size() < value.size() &&
                value[i + group.size()] >= '0' && value[i + group.size()] <= '9') {
                i += group.size();
                continue;
            }
            break;
        }
        std::string token = value.substr(start, i - start);
        bool hasGroup = !group.empty() && token.find(group) != std::string::npos;
        if (hasGroup) {
            // Int64.TryParse(token, AllowThousands): groups are accepted anywhere between digits.
            std::string digits;
            for (size_t k = 0; k < token.size();) {
                if (token[k] >= '0' && token[k] <= '9') digits += token[k++];
                else k += group.size();
            }
            size_t firstDigit = digits.find_first_not_of('0');
            digits = firstDigit == std::string::npos ? std::string("0") : digits.substr(firstDigit);
            // at most 19 digits and not above Int64.MaxValue
            if (digits.size() < 19 || (digits.size() == 19 && digits <= "9223372036854775807")) {
                int64_t number = std::stoll(digits);
                if (number >= 10000) {
                    double scale = number >= 1000000000 ? 1000000000.0 : number >= 1000000 ? 1000000.0 : 1000.0;
                    result += format::format_trimmed(static_cast<double>(number) / scale, 1, culture);
                    result += scale == 1000000000.0 ? 'B' : scale == 1000000.0 ? 'M' : 'k';
                    continue;
                }
            }
        }
        result += token;
    }
    return result;
}

size_t utf16_length(const std::string& utf8) { return to_utf16(utf8).size(); }

std::string pad_right_utf16(const std::string& text, size_t width)
{
    size_t length = utf16_length(text);
    return length >= width ? text : text + std::string(width - length, ' ');
}

std::string pad_left_utf16(const std::string& text, size_t width)
{
    size_t length = utf16_length(text);
    return length >= width ? text : std::string(width - length, ' ') + text;
}

} // namespace app
} // namespace midibn
