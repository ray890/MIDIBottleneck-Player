#include "midibn/app/analysis_resolution.h"

#include "midibn/analysis/workload_analysis.h"
#include "midibn/app/analysis_format.h"
#include "midibn/format/display_format.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace midibn {
namespace app {

const char* const kCustomResolutionTitle = "Custom Analysis resolution";
const char* const kCustomResolutionLabel = "Aggregation interval:";
const char* const kCustomResolutionNote = "Up to 2,000,000 buckets (about 128 MiB before drawing caches).";
const char* const kCustomResolutionInvalidTitle = "Invalid Analysis resolution";

namespace {

const char* const kCustomItem = "Custom\xE2\x80\xA6";

std::string strip_leading_zeros(const std::string& digits)
{
    size_t i = 0;
    while (i + 1 < digits.size() && digits[i] == '0') ++i;
    return digits.substr(i);
}

// a > b for non-negative decimal digit strings without leading zeros.
bool digits_greater(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return a.size() > b.size();
    return a > b;
}

std::string bucket_limit_message(int64_t bucketCount, const format::NumberCulture& c)
{
    return "That resolution would require " + format::format_n0(bucketCount, c) + " graph buckets. The safety limit is " +
        format::format_n0(analysis::kMaximumBucketCount, c) +
        " buckets (approximately 128 MiB before drawing caches). Choose a larger interval.";
}

CustomResolutionResult fail(const std::string& error)
{
    CustomResolutionResult r;
    r.error = error;
    return r;
}

} // namespace

const std::vector<int64_t>& analysis_resolution_choices()
{
    static const std::vector<int64_t> choices = { 10000, 25000, 50000, 100000, 250000, 500000, 1000000 };
    return choices;
}

int64_t analysis_resolution_from_text(const std::string& text)
{
    if (text == "10 ms") return 10000;
    if (text == "25 ms") return 25000;
    if (text == "50 ms") return 50000;
    if (text == "100 ms") return 100000;
    if (text == "250 ms") return 250000;
    if (text == "500 ms") return 500000;
    if (text == "1 s") return 1000000;
    return 0;
}

int64_t choose_auto_resolution(int64_t span, int plotWidth, int64_t current)
{
    const std::vector<int64_t>& choices = analysis_resolution_choices();
    double target = static_cast<double>(span) / std::max(1.0, plotWidth / 1.5);
    if (current > 0 && target >= current * 0.72 && target <= current * 1.85) return current;
    int64_t best = choices[0];
    double bestDistance = (std::numeric_limits<double>::max)();
    for (size_t i = 0; i < choices.size(); ++i) {
        double distance = std::fabs(std::log(std::max(1.0, target) / static_cast<double>(choices[i])));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = choices[i];
        }
    }
    return best;
}

CustomResolutionResult validate_custom_resolution_microseconds(bool hasSong, int64_t duration, int64_t microseconds,
    const format::NumberCulture& culture)
{
    if (!hasSong) return fail("No MIDI file is available.");
    if (microseconds <= 0) return fail("Enter a positive aggregation interval in milliseconds.");
    // v1: WorkloadAnalyzer.CalculateBucketCount(duration, microseconds)
    int64_t bucketCount = duration / microseconds + 1;
    if (bucketCount > analysis::kMaximumBucketCount) return fail(bucket_limit_message(bucketCount, culture));
    CustomResolutionResult ok;
    ok.ok = true;
    ok.microseconds = microseconds;
    return ok;
}

CustomResolutionResult validate_custom_resolution(bool hasSong, int64_t duration, const std::string& milliseconds,
    const format::NumberCulture& culture)
{
    if (!hasSong) return fail("No MIDI file is available.");
    // Parse [-]digits[.digits] (anything else is not a positive interval).
    size_t i = 0;
    bool negative = false;
    if (i < milliseconds.size() && (milliseconds[i] == '-' || milliseconds[i] == '+')) negative = milliseconds[i++] == '-';
    std::string whole, fraction;
    while (i < milliseconds.size() && milliseconds[i] >= '0' && milliseconds[i] <= '9') whole += milliseconds[i++];
    if (i < milliseconds.size() && milliseconds[i] == '.') {
        ++i;
        while (i < milliseconds.size() && milliseconds[i] >= '0' && milliseconds[i] <= '9') fraction += milliseconds[i++];
    }
    if (i != milliseconds.size() || (whole.empty() && fraction.empty()))
        return fail("Enter a positive aggregation interval in milliseconds.");
    whole = strip_leading_zeros(whole.empty() ? std::string("0") : whole);
    bool zero = whole == "0" && fraction.find_first_not_of('0') == std::string::npos;
    // milliseconds <= 0 || milliseconds > Decimal.MaxValue / 1000m (79228162514264337593543950.335)
    bool tooLarge = digits_greater(whole, "79228162514264337593543950");
    if (!tooLarge && whole == "79228162514264337593543950") {
        std::string head = (fraction + "000").substr(0, 3);
        bool rest = fraction.size() > 3 && fraction.find_first_not_of('0', 3) != std::string::npos;
        tooLarge = head > "335" || (head == "335" && rest);
    }
    if (negative || zero || tooLarge) return fail("Enter a positive aggregation interval in milliseconds.");
    // Decimal.Round(milliseconds * 1000, 0, MidpointRounding.AwayFromZero)
    std::string padded = fraction + "0000";
    std::string micros = strip_leading_zeros(whole + padded.substr(0, 3));
    if (padded[3] >= '5') {
        size_t k = micros.size();
        while (k > 0) {
            if (micros[k - 1] == '9') micros[--k] = '0';
            else { ++micros[k - 1]; break; }
        }
        if (k == 0) micros.insert(micros.begin(), '1');
    }
    if (digits_greater(micros, "9223372036854775807") || micros == "0")
        return fail("The aggregation interval is outside the supported range.");
    return validate_custom_resolution_microseconds(true, duration, std::stoll(micros), culture);
}

// ---------------------------------------------------------------- ResolutionSelector

ResolutionSelector::ResolutionSelector(const format::NumberCulture& culture) : culture_(culture) { rebuild(); }

void ResolutionSelector::rebuild()
{
    items_.clear();
    items_.push_back(auto_label_);
    items_.push_back("10 ms");
    items_.push_back("25 ms");
    items_.push_back("50 ms");
    items_.push_back("100 ms");
    items_.push_back("250 ms");
    items_.push_back("500 ms");
    items_.push_back("1 s");
    if (!custom_label_.empty()) items_.push_back(custom_label_);
    items_.push_back(kCustomItem);
}

ResolutionSelector::Selection ResolutionSelector::select(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) index = 0;
    selected_ = index;
    const std::string& selected = items_[static_cast<size_t>(index)];
    if (selected == kCustomItem) return Selection::PromptCustom;
    if (selected == auto_label_) mode_ = ResolutionMode::Auto;
    else if (!custom_label_.empty() && selected == custom_label_) mode_ = ResolutionMode::Custom;
    else mode_ = ResolutionMode::Fixed;
    if (!selected.empty()) last_valid_ = selected;
    return Selection::Changed;
}

void ResolutionSelector::apply_custom(int64_t microseconds)
{
    custom_us_ = microseconds;
    std::string label = "Custom: " + analysis_format_resolution(microseconds, culture_);
    custom_label_ = label;
    rebuild();
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i] == label) selected_ = static_cast<int>(i);
    mode_ = ResolutionMode::Custom;
    last_valid_ = label;
}

void ResolutionSelector::cancel_custom()
{
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i] == last_valid_) { selected_ = static_cast<int>(i); return; }
}

void ResolutionSelector::update_auto_label(int64_t acceptedMicroseconds)
{
    std::string label = "Auto (" + analysis_format_resolution(acceptedMicroseconds, culture_) + ")";
    if (label == auto_label_) return;
    int index = 0;
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i] == auto_label_) { index = static_cast<int>(i); break; }
    auto_label_ = label;
    items_[static_cast<size_t>(index)] = label;
    selected_ = index;
    last_valid_ = label;
    tooltip_ = "Automatic graph resolution; the displayed graph currently uses " +
        analysis_format_resolution(acceptedMicroseconds, culture_) + " buckets.";
}

int64_t ResolutionSelector::selected_microseconds(int64_t viewSpan, int64_t songDuration, int plotWidth, int64_t currentBucket) const
{
    if (mode_ == ResolutionMode::Custom) return custom_us_;
    if (mode_ == ResolutionMode::Fixed) {
        // v1 reads the combo's text, which is "Custom…" (resolution 0, a failed analysis) while the prompt is open.
        int64_t fixed = analysis_resolution_from_text(selected_text() == kCustomItem ? last_valid_ : selected_text());
        return fixed > 0 ? fixed : analysis::kDefaultBucketMicroseconds;
    }
    int64_t span = viewSpan > 0 ? viewSpan : std::max<int64_t>(1, songDuration);
    return choose_auto_resolution(span, std::max(100, plotWidth), currentBucket);
}

} // namespace app
} // namespace midibn
