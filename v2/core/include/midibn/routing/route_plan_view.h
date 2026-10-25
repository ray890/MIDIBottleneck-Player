// Plan presentation logic of v1 TrackRoutingForm.cs, without the window:
// the lane rows, the complete plan preview text, the used-channel emphasis,
// the Apply/confirmation state, and the correction selector index. Texts are
// UTF-16 (track names are UTF-16 in v1). Numbers use v1's "N0" format with
// the invariant culture; v1 uses the current culture, which is a frontend
// formatting choice for Milestone 6.
#pragma once

#include "midibn/format/net_format.h"
#include "midibn/routing/track_routing.h"

#include <array>
#include <string>
#include <vector>

namespace midibn {
namespace routing {

// Row background (v1: none, MistyRose, LightCoral, LemonChiffon).
enum class RouteRowStyle : int32_t { Normal = 0, Unresolved = 1, Omitted = 2, Warning = 3 };

struct RouteLaneRow {
    TrackLaneKey lane;
    std::u16string lane_text;
    std::u16string patch_text;
    std::u16string counts_text;
    std::u16string destination_text;
    std::u16string status_text;
    RouteRowStyle style = RouteRowStyle::Normal;
};

struct RouteApplyState {
    bool valid = false;
    bool lossy_confirmation_visible = false;  // also its Enabled state
    bool apply_enabled = false;
    bool status_error = false;  // DarkRed status text
    std::u16string status_text;
};

std::string format_n0(int64_t value);
std::string tier_name(TrackRoutingTier tier);

std::vector<RouteLaneRow> route_lane_rows(const CompiledTrackRoute& compiled,
    const format::NumberCulture& culture = format::NumberCulture());
// Lines end with CR LF, as StringBuilder.AppendLine.
std::u16string route_preview_text(const CompiledTrackRoute& compiled, TrackRoutingTier settingsTier,
    const format::NumberCulture& culture = format::NumberCulture());
std::array<bool, 16> route_used_channels(const CompiledTrackRoute& compiled);
RouteApplyState route_apply_state(const CompiledTrackRoute* compiled, bool lossyConfirmationChecked);
// Correction selector index for a lane: manual destination + 1, or 0 (Automatic suggestion).
int32_t route_correction_index(const TrackRoutingSettings& settings, const TrackLaneKey& lane);
// v1 ApplyClicked: whether Apply publishes, and the ExplicitlyConfirmed flag it records.
bool route_apply(const CompiledTrackRoute* compiled, bool lossyConfirmationChecked, bool& explicitlyConfirmed);

} // namespace routing
} // namespace midibn
