// Port of the plan presentation logic in v1 TrackRoutingForm.cs.
#include "midibn/routing/route_plan_view.h"

#include "midibn/format/net_format.h"

namespace midibn {
namespace routing {

namespace {

// UTF-8 to UTF-16: the texts are ASCII except a culture's number separators.
std::u16string u(const std::string& utf8)
{
    std::u16string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = c;
        size_t extra = 0;
        if (c >= 0xF0) { cp = c & 0x07u; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1Fu; extra = 1; }
        if (c >= 0x80 && extra == 0) { out += static_cast<char16_t>(0xFFFD); ++i; continue; }
        if (extra != 0 && i + extra >= utf8.size()) {
            out += static_cast<char16_t>(0xFFFD);
            break;
        }
        for (size_t k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        i += extra + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        }
        else out += static_cast<char16_t>(cp);
    }
    return out;
}

const char16_t kEmDash[] = u" — ";
const char16_t kBullet[] = u"• ";
const char16_t kNewLine[] = u"\r\n";

const TrackRouteAssignment* find_assignment(const CompiledTrackRoute& compiled, const TrackLaneKey& lane)
{
    for (const auto& assignment : compiled.assignments)
        if (assignment->profile->lane == lane) return assignment.get();
    return nullptr;
}

std::u16string join_warnings(const std::vector<std::string>& warnings)
{
    std::u16string text;
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i != 0) text += u"; ";
        text += u(warnings[i]);
    }
    return text;
}

void line(std::u16string& text, const std::u16string& value)
{
    text += value;
    text += kNewLine;
}

} // namespace

std::string format_n0(int64_t value)
{
    bool negative = value < 0;
    uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    std::string text;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) text += ',';
        text += digits[i];
    }
    return negative ? "-" + text : text;
}

std::string tier_name(TrackRoutingTier tier)
{
    return tier == TrackRoutingTier::ExactSafe ? "Exact-safe" : tier == TrackRoutingTier::GmFamily ? "GM-family" : "Aggressive";
}

std::vector<RouteLaneRow> route_lane_rows(const CompiledTrackRoute& compiled, const format::NumberCulture& culture)
{
    auto n0 = [&culture](int64_t v) { return format::format_n0(v, culture); };
    std::vector<RouteLaneRow> rows;
    for (const auto& lanePtr : compiled.lanes) {
        const TrackRouteLaneProfile& lane = *lanePtr;
        const TrackRouteAssignment* assignment = find_assignment(compiled, lane.lane);
        RouteLaneRow row;
        row.lane = lane.lane;
        row.destination_text = assignment == nullptr ? u"Unresolved" : assignment->omitted ? u"Omitted"
            : u("Channel " + std::to_string(assignment->destination_channel + 1));
        if (assignment == nullptr) row.status_text = u"Hard error — no complete route";
        else if (assignment->omitted)
            row.status_text = u"Aggressive — omitted " + u(n0(assignment->profile->attack_count)) +
                (assignment->profile->attack_count == 1 ? u" note start" : u" note starts");
        else row.status_text = u(assignment->manual ? std::string("Manual") : tier_name(assignment->tier));
        if (assignment != nullptr && !assignment->warnings.empty())
            row.status_text += kEmDash + join_warnings(assignment->warnings);
        row.lane_text = lane.display_name() + u" · channel " + u(std::to_string(lane.lane.source_channel + 1));
        row.patch_text = u(lane.representative_patch.to_string());
        row.counts_text = u(n0(lane.event_count) + " / " + n0(lane.attack_count));
        if (assignment == nullptr) row.style = RouteRowStyle::Unresolved;
        else if (assignment->omitted) row.style = RouteRowStyle::Omitted;
        else if (!assignment->warnings.empty() || assignment->manual) row.style = RouteRowStyle::Warning;
        rows.push_back(row);
    }
    return rows;
}

std::u16string route_preview_text(const CompiledTrackRoute& compiled, TrackRoutingTier settingsTier,
    const format::NumberCulture& culture)
{
    auto n0 = [&culture](int64_t v) { return format::format_n0(v, culture); };
    std::u16string text;
    const TrackRouteStatistics& statistics = compiled.statistics;
    line(text, compiled.is_valid() ? u"VALID COMPLETE PLAN" : u"PLAN HAS HARD ERRORS");
    line(text, u("Automatic tier: " + tier_name(settingsTier)));
    line(text, u("Routed: " + n0(statistics.routed_lane_count) + " MIDI track/channel entries to " +
        n0(statistics.destination_count) +
        (statistics.destination_count == 1 ? " output channel" : " output channels")));
    line(text, u("Merged: " + n0(statistics.merged_lane_count) +
        (statistics.merged_lane_count == 1 ? " additional entry shares an output channel"
                                           : " additional entries share output channels")));
    line(text, u("Retimbred: " + n0(statistics.retimbred_lane_count) + " entries"));
    line(text, u("Omitted: " + n0(statistics.omitted_lane_count) + " entries / " +
        n0(statistics.omitted_note_start_count) + " note starts"));
    line(text, u("Destination Bank/Program messages generated: " + n0(statistics.generated_destination_state_count)));
    line(text, u("Routed output view: " + n0(statistics.included_source_event_count) + " source messages + " +
        n0(statistics.generated_destination_state_count) + " generated = " +
        n0(statistics.routed_output_event_count) + " total"));
    line(text, u(std::string("Apply confirmation: ") + (compiled.requires_explicit_apply ? "required" : "not required")));
    if (!compiled.errors.empty()) {
        line(text, u"");
        line(text, u"HARD ERRORS");
        for (const std::string& error : compiled.errors) line(text, kBullet + u(error));
    }
    for (const TrackRouteDestinationPreview& destination : compiled.destinations) {
        line(text, u"");
        line(text, u("OUTPUT CHANNEL " + std::to_string(destination.destination_channel + 1)));
        line(text, u("Representative: " + destination.representative_patch.to_string() + " from " +
            destination.representative_lane.to_string()));
        for (const auto& assignment : destination.assignments) {
            text += u"  ";
            text += kBullet;
            text += assignment->profile->display_name();
            text += u" · source ch " + u(std::to_string(assignment->profile->lane.source_channel + 1));
            text += kEmDash + u(assignment->manual ? std::string("manual") : tier_name(assignment->tier));
            if (assignment->representative_patch) text += u"; representative";
            text += kNewLine;
            for (const std::string& warning : assignment->warnings) line(text, u("      WARNING: " + warning));
        }
    }
    bool wroteOmissions = false;
    for (const auto& assignment : compiled.assignments) {
        if (!assignment->omitted) continue;
        if (!wroteOmissions) {
            line(text, u"");
            line(text, u"OMITTED MIDI TRACKS / CHANNELS — LOSSY CONFIRMATION REQUIRED");
            wroteOmissions = true;
        }
        text += u"  ";
        text += kBullet;
        text += assignment->profile->display_name();
        text += u" · source ch " + u(std::to_string(assignment->profile->lane.source_channel + 1));
        text += kEmDash + u(n0(assignment->profile->attack_count));
        line(text, assignment->profile->attack_count == 1 ? u" note start omitted" : u" note starts omitted");
        for (const std::string& warning : assignment->warnings) line(text, u("      WARNING: " + warning));
    }
    return text;
}

std::array<bool, 16> route_used_channels(const CompiledTrackRoute& compiled)
{
    std::array<bool, 16> used;
    used.fill(false);
    for (const TrackRouteDestinationPreview& destination : compiled.destinations) {
        int channel = destination.destination_channel;
        if (channel >= 0 && channel < 16) used[static_cast<size_t>(channel)] = true;
    }
    return used;
}

RouteApplyState route_apply_state(const CompiledTrackRoute* compiled, bool lossyConfirmationChecked)
{
    RouteApplyState state;
    state.valid = compiled != nullptr && compiled->is_valid();
    bool confirmed = !state.valid || !compiled->requires_explicit_apply || lossyConfirmationChecked;
    state.lossy_confirmation_visible = state.valid && compiled->requires_explicit_apply;
    state.apply_enabled = state.valid && confirmed;
    state.status_error = !state.valid;
    state.status_text = !state.valid
        ? u"Resolve every hard error before Apply. The current route remains unchanged."
        : compiled->requires_explicit_apply ? u"Review all highlighted warnings and confirm before Apply."
                                            : u"Exact-safe complete plan. Apply changes only this stopped session.";
    return state;
}

int32_t route_correction_index(const TrackRoutingSettings& settings, const TrackLaneKey& lane)
{
    auto found = settings.manual_destinations.find(lane);
    if (found == settings.manual_destinations.end()) return 0;   // "Auto"
    // A destination outside channels 1-16 (0-15 here) has no entry in the selector. v1 handed index 17 to the
    // selector, which threw while the dialog opened (register entry DD-005); v2 leaves the selector empty. The
    // lane keeps reporting "has an unavailable manual destination" in the plan, so the problem stays visible.
    if (found->second < 0 || found->second > 15) return -1;
    return found->second + 1;
}

bool route_apply(const CompiledTrackRoute* compiled, bool lossyConfirmationChecked, bool& explicitlyConfirmed)
{
    RouteApplyState state = route_apply_state(compiled, lossyConfirmationChecked);
    if (!state.apply_enabled || compiled == nullptr || !compiled->is_valid()) return false;
    explicitlyConfirmed = !compiled->requires_explicit_apply || lossyConfirmationChecked;
    return true;
}

} // namespace routing
} // namespace midibn
