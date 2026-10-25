// The Analysis window's Resolution choice (v1 DiagnosticsForm: ChooseAutoResolution,
// ResolutionChoices, the Auto/Fixed/Custom selection, the "Custom…" prompt and its
// validation, the combo box items and labels).
#pragma once

#include "midibn/format/net_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace app {

// 10 ms, 25 ms, 50 ms, 100 ms, 250 ms, 500 ms, 1 s.
const std::vector<int64_t>& analysis_resolution_choices();
// "10 ms" ... "1 s" -> microseconds; 0 for any other text.
int64_t analysis_resolution_from_text(const std::string& text);
// DiagnosticsForm.ChooseAutoResolution: the resolution (of the fixed choices) that suits `span` across
// `plotWidth` pixels; the current one is kept while it stays within 0.72 .. 1.85 of the target.
int64_t choose_auto_resolution(int64_t span, int plotWidth, int64_t current);

struct CustomResolutionResult {
    bool ok = false;
    int64_t microseconds = 0;
    std::string error;
};
// DiagnosticsForm.TryValidateCustomResolution for `milliseconds` given as decimal text (digits with an
// optional '.' fraction and sign; the invariant culture). `hasSong` false -> "No MIDI file is available."
CustomResolutionResult validate_custom_resolution(bool hasSong, int64_t durationMicroseconds, const std::string& milliseconds,
    const format::NumberCulture& culture = format::NumberCulture());
// The same check for a value already in microseconds (the dialog's number box has three decimals).
CustomResolutionResult validate_custom_resolution_microseconds(bool hasSong, int64_t durationMicroseconds, int64_t microseconds,
    const format::NumberCulture& culture = format::NumberCulture());
// The custom-resolution prompt's fixed texts and number-box range (milliseconds, 3 decimals).
extern const char* const kCustomResolutionTitle;     // "Custom Analysis resolution"
extern const char* const kCustomResolutionLabel;     // "Aggregation interval:"
extern const char* const kCustomResolutionNote;      // "Up to 2,000,000 buckets (about 128 MiB before drawing caches)."
extern const char* const kCustomResolutionInvalidTitle;   // "Invalid Analysis resolution"
const int64_t kCustomResolutionMinimumMicroseconds = 1;            // 0.001 ms
const int64_t kCustomResolutionMaximumMicroseconds = 86400000000LL; // 86,400,000 ms
const int64_t kCustomResolutionDefaultMicroseconds = 100000;       // 100 ms when nothing was chosen before

enum class ResolutionMode { Auto, Fixed, Custom };

// The state behind the Resolution combo box. `items()` is what the combo lists; every change
// of the choice goes through a method here, so nothing needs a "suppress events" flag.
class ResolutionSelector {
public:
    explicit ResolutionSelector(const format::NumberCulture& culture = format::NumberCulture());

    ResolutionMode mode() const { return mode_; }
    const std::vector<std::string>& items() const { return items_; }
    int selected_index() const { return selected_; }
    const std::string& selected_text() const { return items_[static_cast<size_t>(selected_)]; }
    int64_t custom_microseconds() const { return custom_us_; }
    const std::string& auto_label() const { return auto_label_; }
    // Tooltip text of the combo while Auto shows an accepted resolution ("" before the first result).
    const std::string& tooltip() const { return tooltip_; }

    enum class Selection { Changed, PromptCustom };
    // The user picked item `index`. PromptCustom: show the prompt (the combo then shows "Custom…").
    Selection select(int index);
    // The prompt was accepted with `microseconds` (already validated), or cancelled.
    void apply_custom(int64_t microseconds);
    void cancel_custom();
    // The accepted result changed (Auto only).
    void update_auto_label(int64_t acceptedMicroseconds);

    // DiagnosticsForm.SelectedResolutionMicroseconds. `viewSpan` is the visible span (<= 0: the whole
    // song), `plotWidth` the graph's plot width, `currentBucket` the shown analysis' bucket (0: none).
    int64_t selected_microseconds(int64_t viewSpan, int64_t songDuration, int plotWidth, int64_t currentBucket) const;

private:
    void rebuild();
    format::NumberCulture culture_;
    ResolutionMode mode_ = ResolutionMode::Auto;
    std::string auto_label_ = "Auto";
    std::string custom_label_;
    int64_t custom_us_ = 0;
    std::string last_valid_ = "Auto";
    std::string tooltip_;
    std::vector<std::string> items_;
    int selected_ = 0;
};

} // namespace app
} // namespace midibn
