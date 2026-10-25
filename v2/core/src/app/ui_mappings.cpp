#include "midibn/app/ui_mappings.h"

#include <algorithm>
#include <cmath>

namespace midibn {
namespace app {

using format::double_to_int32;
using format::double_to_int64;
using format::round_away_from_zero;
using format::round_half_even;
using models::ServiceDurationMode;

namespace {

// Math.Min / Math.Max for doubles (NaN propagates).
double net_min(double a, double b)
{
    if (a < b) return a;
    if (a == b) return std::signbit(a) ? a : b;
    if (std::isnan(a)) return a;
    return b;
}

double net_max(double a, double b)
{
    if (a > b) return a;
    if (a == b) return std::signbit(a) ? b : a;
    if (std::isnan(a)) return a;
    return b;
}

int64_t clamp64(int64_t value, int64_t low, int64_t high)
{
    return std::max(low, std::min(high, value));
}

} // namespace

// ---- ProcessingTrackBar ---------------------------------------------------------------

int value_from_track_point(int x, int width, int minimum, int maximum)
{
    if (maximum <= minimum || width <= 1) return minimum;
    const int edge = 8;
    int usable = std::max(1, width - edge * 2);
    double normalized = (x - edge) / static_cast<double>(usable);
    normalized = net_max(0.0, net_min(1.0, normalized));
    return minimum + double_to_int32(static_cast<double>(round_half_even(normalized * (maximum - minimum))));
}

int64_t slider_to_microseconds(int slider)
{
    slider = std::max(0, std::min(kSliderScaleMaximum, slider));
    if (slider <= kLowRangeEnd)
        return round_half_even(slider * static_cast<double>(kLowRangeMicroseconds) / kLowRangeEnd);
    double normalized = (slider - kLowRangeEnd) / static_cast<double>(kSliderScaleMaximum - kLowRangeEnd);
    return round_half_even(static_cast<double>(kLowRangeMicroseconds) *
        std::pow(kMaximumMicroseconds / static_cast<double>(kLowRangeMicroseconds), normalized));
}

int microseconds_to_slider(int64_t microseconds)
{
    if (microseconds <= 0) return 0;
    if (microseconds <= kLowRangeMicroseconds)
        return std::max(0, std::min(kLowRangeEnd, double_to_int32(static_cast<double>(
            round_half_even(microseconds * static_cast<double>(kLowRangeEnd) / kLowRangeMicroseconds)))));
    microseconds = std::min(kMaximumMicroseconds, microseconds);
    double normalized = std::log(microseconds / static_cast<double>(kLowRangeMicroseconds)) /
        std::log(kMaximumMicroseconds / static_cast<double>(kLowRangeMicroseconds));
    return std::max(kLowRangeEnd, std::min(kSliderScaleMaximum,
        kLowRangeEnd + double_to_int32(static_cast<double>(round_half_even(normalized * (kSliderScaleMaximum - kLowRangeEnd))))));
}

// ---- MainForm ------------------------------------------------------------------------------

int bitrate_to_slider(int64_t bitrate)
{
    double normalized = (std::log10(static_cast<double>(std::max<int64_t>(100, std::min<int64_t>(100000000, bitrate)))) - 2.0) / 6.0;
    return std::max(0, std::min(kSliderScaleMaximum,
        double_to_int32(static_cast<double>(round_half_even(normalized * kSliderScaleMaximum)))));
}

int64_t slider_to_bitrate(int slider)
{
    double exponent = 2.0 + (std::max(0, std::min(kSliderScaleMaximum, slider)) / static_cast<double>(kSliderScaleMaximum)) * 6.0;
    return round_half_even(std::pow(10.0, exponent));
}

int event_rate_to_slider(int64_t rate)
{
    if (rate <= 0) return 0;
    const int lowEnd = 5000;
    if (rate <= 100)
        return 1 + double_to_int32(static_cast<double>(round_half_even(static_cast<double>((rate - 1) * (lowEnd - 1)) / 99.0)));
    rate = std::min<int64_t>(9999999, rate);
    double normalized = std::log(rate / 100.0) / std::log(9999999.0 / 100.0);
    return lowEnd + double_to_int32(static_cast<double>(round_half_even(normalized * (kSliderScaleMaximum - lowEnd))));
}

int64_t slider_to_event_rate(int slider)
{
    if (slider <= 0) return 0;
    const int lowEnd = 5000;
    slider = std::min(kSliderScaleMaximum, slider);
    if (slider <= lowEnd)
        return 1 + round_half_even((slider - 1) * 99.0 / (lowEnd - 1));
    double normalized = (slider - lowEnd) / static_cast<double>(kSliderScaleMaximum - lowEnd);
    return std::max<int64_t>(100, std::min<int64_t>(9999999,
        round_half_even(100.0 * std::pow(9999999.0 / 100.0, normalized))));
}

int map_rate_scrub_value(ServiceDurationMode mode, int startValue, int64_t pixels)
{
    pixels = clamp64(pixels, -1000000, 1000000);
    if (mode == ServiceDurationMode::MidiBitrate && startValue < 100)
        return static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(100000000, static_cast<int64_t>(startValue) + pixels)));
    int startSlider = mode == ServiceDurationMode::MidiBitrate
        ? bitrate_to_slider(startValue)
        : mode == ServiceDurationMode::EventsPerSecond
            ? event_rate_to_slider(startValue) : microseconds_to_slider(startValue);
    int targetSlider;
    if (mode == ServiceDurationMode::EventsPerSecond) {
        // Ten slider ticks per pointer pixel in the fine 1-100 region; one tick per
        // pixel in the logarithmic high range.
        double coordinate = startSlider <= 5000 ? startSlider / 10.0 : 500.0 + startSlider - 5000;
        coordinate = net_max(0.0, net_min(5500.0, coordinate + static_cast<double>(pixels)));
        targetSlider = coordinate <= 500.0
            ? double_to_int32(static_cast<double>(round_half_even(coordinate * 10.0)))
            : 5000 + double_to_int32(static_cast<double>(round_half_even(coordinate - 500.0)));
    } else {
        targetSlider = static_cast<int>(std::max<int64_t>(0,
            std::min<int64_t>(kSliderScaleMaximum, static_cast<int64_t>(startSlider) + pixels)));
    }
    int64_t startMapped = mode == ServiceDurationMode::MidiBitrate
        ? slider_to_bitrate(startSlider)
        : mode == ServiceDurationMode::EventsPerSecond
            ? slider_to_event_rate(startSlider) : slider_to_microseconds(startSlider);
    int64_t targetMapped = mode == ServiceDurationMode::MidiBitrate
        ? slider_to_bitrate(targetSlider)
        : mode == ServiceDurationMode::EventsPerSecond
            ? slider_to_event_rate(targetSlider) : slider_to_microseconds(targetSlider);
    int64_t minimum = mode == ServiceDurationMode::MidiBitrate ? 1 : 0;
    int64_t maximum = mode == ServiceDurationMode::MidiBitrate ? 100000000
        : mode == ServiceDurationMode::EventsPerSecond ? 9999999 : 1000000;
    return static_cast<int>(std::max(minimum, std::min(maximum,
        static_cast<int64_t>(startValue) + targetMapped - startMapped)));
}

int map_queue_scrub_value(int startValue, int64_t pixels)
{
    const double knee = 500.0;
    const double scale = 2000.0;
    double start = std::max(1, std::min(1000000, startValue));
    double coordinate = start <= knee ? start * 4.0 : knee * 4.0 + scale * std::log(start / knee);
    double maximum = knee * 4.0 + scale * std::log(1000000.0 / knee);
    coordinate = net_max(4.0, net_min(maximum, coordinate + static_cast<double>(clamp64(pixels, -1000000, 1000000))));
    double target = coordinate <= knee * 4.0 ? coordinate / 4.0 : knee * std::exp((coordinate - knee * 4.0) / scale);
    int64_t delta = round_away_from_zero(target - start);
    return static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(1000000, static_cast<int64_t>(startValue) + delta)));
}

int map_queue_age_scrub_value(int startValue, int64_t pixels)
{
    const double knee = 1000000.0;
    const double scale = 2400.0;
    double start = std::max(1, std::min(1800000000, startValue));
    double coordinate = start <= knee ? start / 250.0 : knee / 250.0 + scale * std::log(start / knee);
    double maximum = knee / 250.0 + scale * std::log(1800000000.0 / knee);
    coordinate = net_max(1.0 / 250.0, net_min(maximum, coordinate + static_cast<double>(clamp64(pixels, -2000000, 2000000))));
    double target = coordinate <= knee / 250.0 ? coordinate * 250.0 : knee * std::exp((coordinate - knee / 250.0) / scale);
    int64_t delta = round_away_from_zero(target - start);
    return static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(1800000000, static_cast<int64_t>(startValue) + delta)));
}

int log_scrub_value(int startValue, int64_t pixels, int minimum, int maximum)
{
    const double pixelsPerEfold = 100.0;
    const double a = pixelsPerEfold / 3.0;   // lower end: 1 unit per 3 pixels
    double start = static_cast<double>(std::max(minimum, std::min(maximum, startValue)));
    double p = static_cast<double>(clamp64(pixels, -1000000, 1000000));
    double exponent = std::min(700.0, std::max(-700.0, p / pixelsPerEfold));
    double value = minimum + (start - minimum + a) * std::exp(exponent) - a;
    value = std::floor(value + 0.5);
    if (!(value > minimum)) return minimum;
    if (value > maximum) return maximum;
    return static_cast<int>(value);
}

int64_t processing_to_event_rate(int64_t microseconds)
{
    if (microseconds <= 0) return 0;
    return std::max<int64_t>(1, std::min<int64_t>(1000000, round_away_from_zero(1000000.0 / static_cast<double>(microseconds))));
}

int64_t event_rate_to_processing(int64_t rate)
{
    if (rate <= 0) return 0;
    return std::max<int64_t>(1, std::min<int64_t>(1000000, round_away_from_zero(1000000.0 / static_cast<double>(rate))));
}

// ---- PlaybackTimelineView -----------------------------------------------------------------

int64_t timeline_position_from_x(int x, int trackLeft, int trackWidth, int64_t duration)
{
    double fraction = net_max(0.0, net_min(1.0, static_cast<double>(x - trackLeft) / trackWidth));
    return round_half_even(static_cast<double>(duration) * fraction);
}

int64_t clamp_timeline_duration(int64_t duration) { return std::max<int64_t>(0, duration); }

int64_t clamp_timeline_position(int64_t position, int64_t duration)
{
    return std::max<int64_t>(0, std::min(clamp_timeline_duration(duration), position));
}

int64_t timeline_key_target(TimelineKey key, int64_t position, int64_t duration)
{
    auto add = [](int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)); };
    switch (key) {
    case TimelineKey::Home: position = 0; break;
    case TimelineKey::End: position = duration; break;
    case TimelineKey::Left: position = add(position, -1000000); break;
    case TimelineKey::Right: position = add(position, 1000000); break;
    case TimelineKey::PageDown: position = add(position, -5000000); break;
    case TimelineKey::PageUp: position = add(position, 5000000); break;
    }
    return std::max<int64_t>(0, std::min(duration, position));
}

int timeline_fill_width(int trackWidth, int64_t position, int64_t duration)
{
    double fraction = duration <= 0 ? 0 : net_max(0.0, net_min(1.0, static_cast<double>(position) / duration));
    return double_to_int32(static_cast<double>(round_half_even(trackWidth * fraction)));
}

// ---- UiScaleFont / ScaleMetric --------------------------------------------------------------

int clamp_ui_scale_percent(int percent) { return std::max(50, std::min(200, percent)); }

UiFontChoice ui_font_choice(float canonicalPoints, int percent)
{
    percent = clamp_ui_scale_percent(percent);
    float points = canonicalPoints * static_cast<float>(percent) / 100.0f;
    if (percent == 50) points = std::max(points, 5.5f);
    else if (percent == 75) points = std::max(points, 7.0f);
    UiFontChoice choice;
    choice.small_family = percent < 100;
    choice.points = points;
    return choice;
}

const char* ui_font_family(bool monospace, bool smallFamily)
{
    if (monospace) return smallFamily ? "Lucida Console" : "Consolas";
    return smallFamily ? "Tahoma" : "Segoe UI";
}

int scale_metric(int canonical, int percent)
{
    if (canonical == 0) return 0;
    int scaled = (canonical * percent + 50) / 100;
    return canonical > 0 ? std::max(1, scaled) : std::min(-1, scaled);
}

int scale_metric_min1(int value, int percent)
{
    return std::max(1, (value * percent + 50) / 100);
}

// ---- number parsing --------------------------------------------------------------------------------

namespace {

// Decodes one UTF-8 code point at `pos` (malformed bytes decode as themselves).
uint32_t decode_utf8(const std::string& s, size_t pos, size_t& length)
{
    unsigned char b = static_cast<unsigned char>(s[pos]);
    length = 1;
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    if (extra == 0 || pos + static_cast<size_t>(extra) >= s.size()) return b;
    uint32_t cp = b & (0x3Fu >> extra);
    for (int i = 1; i <= extra; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[pos + static_cast<size_t>(i)]) & 0x3Fu);
    length = static_cast<size_t>(extra) + 1;
    return cp;
}

bool is_net_white_space(uint32_t c)
{
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
        (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool starts_with_at(const std::string& s, size_t pos, const std::string& what)
{
    return !what.empty() && s.compare(pos, what.size(), what) == 0;
}

} // namespace

std::string trim_net(const std::string& text)
{
    size_t begin = 0;
    size_t length = 0;
    while (begin < text.size()) {
        uint32_t cp = decode_utf8(text, begin, length);
        if (!is_net_white_space(cp)) break;
        begin += length;
    }
    // Trailing: find the last code point start that is not white space.
    size_t end = text.size();
    while (end > begin) {
        size_t start = end - 1;
        while (start > begin && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
        size_t l = 0;
        uint32_t cp = decode_utf8(text, start, l);
        if (!is_net_white_space(cp)) break;
        end = start;
    }
    return text.substr(begin, end - begin);
}

bool try_parse_int32(const std::string& text, bool allowThousands, int32_t& value, const format::NumberCulture& culture)
{
    value = 0;
    size_t p = 0;
    const size_t n = text.size();
    auto is_white = [](unsigned char c) { return c == 0x20 || (c >= 0x09 && c <= 0x0D); };
    // Leading white space is allowed only before a sign (the "-n" pattern forbids it after one).
    bool negative = false;
    bool sign = false;
    while (p < n && is_white(static_cast<unsigned char>(text[p]))) ++p;
    if (p < n && text[p] == '+') { sign = true; ++p; }
    else if (p < n && text[p] == '-') { sign = true; negative = true; ++p; }
    (void)sign;
    bool anyDigit = false;
    uint64_t magnitude = 0;
    int significant = 0;
    bool overflow = false;
    while (p < n) {
        char ch = text[p];
        if (ch >= '0' && ch <= '9') {
            anyDigit = true;
            if (magnitude != 0 || ch != '0') {
                ++significant;
                if (significant > 10) overflow = true;
                else magnitude = magnitude * 10 + static_cast<uint64_t>(ch - '0');
            }
            ++p;
        } else if (allowThousands && anyDigit && starts_with_at(text, p, culture.group_separator)) {
            p += culture.group_separator.size();
        } else {
            break;
        }
    }
    if (!anyDigit) return false;
    while (p < n && is_white(static_cast<unsigned char>(text[p]))) ++p;
    // Nothing but white space may follow the number (v1's .NET parser also let NUL characters follow it; DD-042).
    if (p != n) return false;
    if (overflow) return false;
    if (negative) {
        if (magnitude > 2147483648ull) return false;
        value = static_cast<int32_t>(-static_cast<int64_t>(magnitude));
    } else {
        if (magnitude > 2147483647ull) return false;
        value = static_cast<int32_t>(magnitude);
    }
    return true;
}

// ---- ScrubField ------------------------------------------------------------------------------------

int ScrubField::clamp(int value) const { return std::max(minimum_, std::min(maximum_, value)); }

int ScrubField::clamp_long(int64_t value) const
{
    if (value < minimum_) return minimum_;
    if (value > maximum_) return maximum_;
    return static_cast<int>(value);
}

bool ScrubField::configure_numeric(int value, int minimum, int maximum, int coarseUnits, int coarsePixels,
    int fineUnits, int finePixels)
{
    if (minimum < 0 || maximum < minimum) return false;
    configure(value, minimum, maximum, 0, coarseUnits, coarsePixels, fineUnits, finePixels, false, false);
    numeric_only_ = true;
    thousands_separator_ = true;
    return true;
}

void ScrubField::configure(int value, int minimum, int maximum, int displayOffset, int coarseUnits, int coarsePixels,
    int fineUnits, int finePixels, bool forced, bool historical, std::function<std::string(int)> displayFormatter)
{
    end_gesture(false);
    minimum_ = minimum;
    maximum_ = maximum;
    display_offset_ = displayOffset;
    coarse_units_ = std::max(1, coarseUnits);
    coarse_pixels_ = std::max(1, coarsePixels);
    fine_units_ = std::max(1, fineUnits);
    fine_pixels_ = std::max(1, finePixels);
    pixel_remainder_ = 0;
    remainder_fine_ = false;
    display_formatter_ = displayFormatter;
    numeric_only_ = false;
    value_ = clamp(value);
    forced_ = forced;
    historical_ = historical;
    typing_ = false;
}

void ScrubField::set_numeric_scrub_mapping(std::function<int(int, int64_t)> mapping)
{
    numeric_scrub_mapping_ = mapping;
    numeric_scrub_initialized_ = false;
}

bool ScrubField::set_value(int value)
{
    if (value < minimum_ || value > maximum_) return false;
    value_ = value;
    return true;
}

bool ScrubField::set_range(int minimum, int maximum)
{
    if (minimum < 0 || maximum < minimum) return false;
    minimum_ = minimum;
    maximum_ = maximum;
    if (value_ < minimum || value_ > maximum) value_ = clamp(value_);
    return true;
}

void ScrubField::begin_gesture() { gesture_start_value_ = value_; }

void ScrubField::start_scrubbing()
{
    pixel_remainder_ = 0;
    remainder_fine_ = false;
    numeric_scrub_initialized_ = false;
}

void ScrubField::end_gesture(bool restoreStart)
{
    if (restoreStart) value_ = gesture_start_value_;
    pixel_remainder_ = 0;
    numeric_scrub_initialized_ = false;
}

std::optional<int> ScrubField::scrub(int pixels, bool fine)
{
    if (!numeric_only_ && minimum_ == 0 && maximum_ == 1) {
        if (pixels > 0) return 1;
        if (pixels < 0) return 0;
        return std::nullopt;
    }
    if (numeric_only_ && numeric_scrub_mapping_) {
        if (!numeric_scrub_initialized_ || remainder_fine_ != fine) {
            numeric_scrub_start_value_ = value_;
            numeric_scrub_pixels_ = 0;
            numeric_scrub_initialized_ = true;
            remainder_fine_ = fine;
        }
        numeric_scrub_pixels_ = clamp64(numeric_scrub_pixels_ + pixels, -1000000, 1000000);
        int mapped = fine
            ? clamp_long(static_cast<int64_t>(numeric_scrub_start_value_) + numeric_scrub_pixels_ / fine_pixels_)
            : numeric_scrub_mapping_(numeric_scrub_start_value_, numeric_scrub_pixels_);
        return mapped;
    }
    if (remainder_fine_ != fine) {
        pixel_remainder_ = 0;
        remainder_fine_ = fine;
    }
    int divisor = fine ? fine_pixels_ : coarse_pixels_;
    int units = fine ? fine_units_ : coarse_units_;
    int accumulated = pixel_remainder_ + pixels;
    int steps = accumulated / divisor;
    pixel_remainder_ = accumulated - steps * divisor;
    if (steps == 0) return std::nullopt;
    int64_t proposed = static_cast<int64_t>(value_) + static_cast<int64_t>(steps) * units;
    return clamp_long(proposed);
}

ScrubField::Change ScrubField::request_value(int value)
{
    Change change;
    value = clamp(value);
    change.value = value;
    if (numeric_only_) {
        if (value == value_) return change;
        value_ = value;
        change.changed = true;
        return change;
    }
    if (value == value_ && forced_) return change;
    value_ = value;
    forced_ = true;
    change.changed = true;
    return change;
}

int ScrubField::step_value(bool up) const
{
    return clamp_long(static_cast<int64_t>(value_) + (up ? numeric_increment_ : -numeric_increment_));
}

std::string ScrubField::format_editable(int value, const format::NumberCulture&) const
{
    if (is_toggle()) return value == 0 ? "Off" : "On";
    int32_t shown = static_cast<int32_t>(static_cast<uint32_t>(value) + static_cast<uint32_t>(display_offset_));
    return format::format_integer(shown);
}

bool ScrubField::try_parse_editable(const std::string& text, int& value, const format::NumberCulture& c) const
{
    std::string normalized = trim_net(text);
    if (is_toggle()) {
        std::string lower;
        for (size_t i = 0; i < normalized.size(); ++i) {
            char ch = normalized[i];
            lower += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
        }
        if (lower == "on") { value = 1; return true; }
        if (lower == "off") { value = 0; return true; }
    }
    int32_t shown = 0;
    if (!try_parse_int32(normalized, numeric_only_, shown, c)) {
        value = value_;
        return false;
    }
    value = static_cast<int32_t>(static_cast<uint32_t>(shown) - static_cast<uint32_t>(display_offset_));
    return true;
}

std::string ScrubField::flat_text(const format::NumberCulture& c) const
{
    if (numeric_only_ && thousands_separator_) return format::format_n0(value_, c);
    if (display_formatter_) return display_formatter_(value_);
    return format_editable(value_, c);
}

std::string ScrubField::begin_typing(const format::NumberCulture& c)
{
    end_gesture(false);
    typing_ = true;
    typing_start_value_ = value_;
    return format_editable(value_, c);
}

ScrubField::CommitResult ScrubField::commit_typed(const std::string& text, bool explicitCommit,
    const format::NumberCulture& c)
{
    CommitResult result;
    if (!typing_) {
        result.accepted = true;
        return result;
    }
    int parsed = 0;
    if (!try_parse_editable(text, parsed, c) || parsed < minimum_ || parsed > maximum_) {
        result.error = "Enter a value from " + format_editable(minimum_, c) + " through " + format_editable(maximum_, c) + ".";
        typing_ = false;
        return result;
    }
    result.accepted = true;
    if (explicitCommit || parsed != typing_start_value_) {
        result.request = true;
        result.value = parsed;
    }
    typing_ = false;
    return result;
}

std::string ScrubField::adjust_typed_text(const std::string& text, int direction, const format::NumberCulture& c)
{
    int parsed = 0;
    if (!typing_) return text;
    if (!try_parse_editable(text, parsed, c)) parsed = value_;
    parsed = clamp_long(static_cast<int64_t>(parsed) + direction);
    return format_editable(parsed, c);
}

} // namespace app
} // namespace midibn
