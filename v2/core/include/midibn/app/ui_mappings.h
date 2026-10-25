// Exact ports of the arithmetic v1's main window uses to turn pointer
// movement into settings and back (ProcessingTrackBar.cs; MainForm.cs
// 3197-3333; PlaybackTimelineView.cs; UiScaleFont.cs; MainForm.ScaleMetric;
// ScrubOrTypeTextBox.cs). The operation order follows the C# so the doubles
// agree bit for bit (ARCHITECTURE.md 5.4); rounding is explicit
// (round_half_even = Math.Round(x), round_away_from_zero =
// MidpointRounding.AwayFromZero). Differences from v1 are listed in
// UI_DIFFERENCES.md.
#pragma once

#include "midibn/format/net_format.h"
#include "midibn/models/rate.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace midibn {
namespace app {

// ---- ProcessingTrackBar ---------------------------------------------------------
constexpr int kSliderScaleMaximum = 10000;
constexpr int kLowRangeEnd = 6000;
constexpr int64_t kLowRangeMicroseconds = 5000;
constexpr int64_t kMaximumMicroseconds = 1000000;

// A click on the track at client x: the slider value under it (8-pixel edges).
int value_from_track_point(int x, int width, int minimum, int maximum);
int64_t slider_to_microseconds(int slider);
int microseconds_to_slider(int64_t microseconds);

// ---- MainForm slider / scrub mappings --------------------------------------------
int bitrate_to_slider(int64_t bitrate);
int64_t slider_to_bitrate(int slider);
int event_rate_to_slider(int64_t rate);
int64_t slider_to_event_rate(int slider);
// Pointer distance through the selected slider's local scale, added to the
// exact typed start value (MainForm.MapRateScrubValue).
int map_rate_scrub_value(models::ServiceDurationMode mode, int startValue, int64_t pixels);
int map_queue_scrub_value(int startValue, int64_t pixels);
int map_queue_age_scrub_value(int startValue, int64_t pixels);
// DD-104 (v2 improvement; the three functions above stay as v1's reference): the pointer feel of the scrub
// boxes without Shift. One logarithmic curve over the whole range: from the lower limit it moves about one unit per
// 3 pixels (so 1 to 2 takes 3 pixels) and from there every 100 pixels multiply the distance above the lower limit
// by e, so the steps get steadily coarser. `startValue` is the exact start of the gesture, `pixels` the signed
// pointer travel since then; the result stays within [minimum, maximum] and 0 pixels returns the start value.
int log_scrub_value(int startValue, int64_t pixels, int minimum, int maximum);
int64_t processing_to_event_rate(int64_t microseconds);
int64_t event_rate_to_processing(int64_t rate);

// ---- PlaybackTimelineView -----------------------------------------------------------
// Position (microseconds) under client x; `trackLeft` and `trackWidth` are
// the track rectangle's Left and Width. Math.Round(duration * fraction), half even.
int64_t timeline_position_from_x(int x, int trackLeft, int trackWidth, int64_t duration);
enum class TimelineKey { Home, End, Left, Right, PageUp, PageDown };
// The position a key press seeks to (clamped to 0..duration):
// Home/End; Left/Right = -/+ 1 s; PageDown/PageUp = -/+ 5 s.
int64_t timeline_key_target(TimelineKey key, int64_t position, int64_t duration);
// PlaybackTimelineView.SetTimeline clamping.
int64_t clamp_timeline_duration(int64_t duration);
int64_t clamp_timeline_position(int64_t position, int64_t duration);
// The width of the filled part of the track while painting.
int timeline_fill_width(int trackWidth, int64_t position, int64_t duration);

// ---- UiScaleFont / ScaleMetric ------------------------------------------------------
struct UiFontChoice {
    bool small_family;   // true: Tahoma / Lucida Console, false: Segoe UI / Consolas
    float points;
};
// UiScaleFont.Create: percent is clamped to 50..200; canonicalPoints * percent / 100F
// with floors of 5.5 (50%) and 7 (75%); the small family below 100%.
UiFontChoice ui_font_choice(float canonicalPoints, int percent);
const char* ui_font_family(bool monospace, bool smallFamily);
int clamp_ui_scale_percent(int percent);
// MainForm.ScaleMetric: (canonical * percent + 50) / 100 away from zero, never 0 for nonzero input.
int scale_metric(int canonical, int percent);
// PlaybackTimelineView.ScaleMetric and the other controls' Math.Max(1, (v * p + 50) / 100).
int scale_metric_min1(int value, int percent);

// ---- ScrubOrTypeTextBox ---------------------------------------------------------------
// String.Trim() (Char.IsWhiteSpace) on UTF-8 text.
std::string trim_net(const std::string& text);
// Int32.TryParse(text, NumberStyles.Integer [| AllowThousands], culture) as .NET
// Framework 4.8 reads it: optional leading "+"/"-", digits (group separators
// anywhere after the first digit when allowed), surrounding white space.
bool try_parse_int32(const std::string& text, bool allowThousands, int32_t& value,
    const format::NumberCulture& culture = format::NumberCulture());

// The pure state of one scrub-or-type number field: value, range, gesture
// arithmetic, text. The frontend owns the edit control, the pointer and the
// events; it asks this object what a pointer movement or a typed string means.
class ScrubField {
public:
    // The result of RequestValue-style changes.
    struct Change {
        bool changed = false;   // a value request was made (numeric: value changed; other: handler runs)
        int value = 0;          // the requested (clamped) value
    };
    struct CommitResult {
        bool accepted = false;      // false: invalid text or out of range (error set)
        bool request = false;       // the frontend should apply `value` (RequestValue)
        int value = 0;
        std::string error;          // empty when none
    };

    // ConfigureNumeric (a plain number box). False where v1 throws ArgumentOutOfRange.
    bool configure_numeric(int value, int minimum, int maximum, int coarseUnits, int coarsePixels, int fineUnits,
        int finePixels);
    // Configure (channel fields: optional Off/On for 0..1, display offset such as channel numbers).
    void configure(int value, int minimum, int maximum, int displayOffset, int coarseUnits, int coarsePixels,
        int fineUnits, int finePixels, bool forced, bool historical,
        std::function<std::string(int)> displayFormatter = std::function<std::string(int)>());
    void set_numeric_scrub_mapping(std::function<int(int, int64_t)> mapping);
    void set_increment(int increment) { numeric_increment_ = increment < 1 ? 1 : increment; }
    void set_thousands_separator(bool on) { thousands_separator_ = on; }
    // v1 RequestAuto: a successful release clears the control's forced flag.
    void set_forced(bool forced) { forced_ = forced; }
    // Value property setter: false where v1 throws (out of range).
    bool set_value(int value);
    // Minimum / Maximum setters: false where v1 throws.
    bool set_range(int minimum, int maximum);

    int value() const { return value_; }
    int minimum() const { return minimum_; }
    int maximum() const { return maximum_; }
    int increment() const { return numeric_increment_; }
    bool forced() const { return forced_; }
    bool numeric_only() const { return numeric_only_; }
    bool typing() const { return typing_; }

    // Pointer gesture. `begin_gesture` on left button down, `start_scrubbing` once the
    // pointer moved past the drag threshold (3 pixels), `end_gesture` on release.
    static constexpr int kDragThreshold = 3;
    void begin_gesture();
    void start_scrubbing();
    void end_gesture(bool restoreStart = false);
    // The value a horizontal movement of `pixels` (fine: Shift held) asks for, or no
    // change. Does not apply it: pass the result to `request_value`.
    std::optional<int> scrub(int pixels, bool fine);
    // RequestValue: clamps; numeric boxes take the value (changed only when different),
    // other fields report the request (unless unchanged while forced) and become forced.
    Change request_value(int value);
    // Up/Down on a focused number box (not typing): value +/- increment, clamped.
    int step_value(bool up) const;

    // Typing.
    std::string format_editable(int value, const format::NumberCulture& c = format::NumberCulture()) const;
    bool try_parse_editable(const std::string& text, int& value,
        const format::NumberCulture& c = format::NumberCulture()) const;
    // The displayed text when not typing (N0 with thousands separator, the formatter, or the editable text).
    std::string flat_text(const format::NumberCulture& c = format::NumberCulture()) const;
    // Enter typing mode; returns the text to edit.
    std::string begin_typing(const format::NumberCulture& c = format::NumberCulture());
    // Enter / focus loss with the edit box's text.
    CommitResult commit_typed(const std::string& text, bool explicitCommit,
        const format::NumberCulture& c = format::NumberCulture());
    // Escape.
    void abandon_typing() { typing_ = false; }
    // Up/Down while typing: the new edit text.
    std::string adjust_typed_text(const std::string& text, int direction,
        const format::NumberCulture& c = format::NumberCulture());

private:
    int clamp(int value) const;
    int clamp_long(int64_t value) const;
    bool is_toggle() const { return !numeric_only_ && minimum_ == 0 && maximum_ == 1 && display_offset_ == 0; }

    int minimum_ = 0;
    int maximum_ = 127;
    int value_ = 0;
    int display_offset_ = 0;
    int coarse_units_ = 1;
    int coarse_pixels_ = 1;
    int fine_units_ = 1;
    int fine_pixels_ = 1;
    int pixel_remainder_ = 0;
    bool remainder_fine_ = false;
    int gesture_start_value_ = 0;
    bool typing_ = false;
    bool forced_ = false;
    bool historical_ = false;
    bool numeric_only_ = false;
    std::function<int(int, int64_t)> numeric_scrub_mapping_;
    int numeric_scrub_start_value_ = 0;
    int64_t numeric_scrub_pixels_ = 0;
    bool numeric_scrub_initialized_ = false;
    int numeric_increment_ = 1;
    bool thousands_separator_ = false;
    int typing_start_value_ = 0;
    std::function<std::string(int)> display_formatter_;
};

} // namespace app
} // namespace midibn
