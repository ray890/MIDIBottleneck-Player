// The state machine behind the Track routing editor (v1 TrackRoutingForm.cs),
// without any window: the three automatic-routing options, the 16 usable
// destination channels, the lane rows and the selected row, the manual
// destination of the selected lane, the plan preview text, the lossy
// confirmation, and the enabling rules of Apply and "Use original routing".
//
// The planner can take noticeable time on big songs, so the model separates
// "the settings changed" from "the plan arrived": every change that needs a
// new plan bumps generation() and clears the confirmation (as v1's
// RefreshPlan does); the frontend computes the plan for settings() -- on a
// worker thread -- and hands the result back with plan_ready(); a result for
// an older generation is ignored. While a plan is pending Apply is disabled.
// recompute_now() does the same synchronously (tests; v1 behaviour).
//
// Everything is UTF-16 text; numbers follow the UI culture (v1 formats with
// CurrentCulture). Nothing here calls the operating system.
#pragma once

#include "midibn/format/net_format.h"
#include "midibn/routing/route_plan_view.h"
#include "midibn/routing/track_routing.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace app {

// v1 GeneralMidiProgramCatalog.DescribePatch: the tooltip of a lane's patch cell.
std::u16string describe_patch(const routing::MidiPatchIdentity& patch);

// One row of the lanes grid.
struct RoutingDialogRow {
    routing::RouteLaneRow row;
    std::u16string patch_tooltip;
};

// The dialog's widget texts (v1's wording).
struct RoutingDialogTexts {
    static const char16_t* title();                 // "Track routing — MIDIBottleneck Player"
    static const char16_t* introduction();
    static const char16_t* tier_item(int index);    // 0..2
    static const char16_t* percussion_item(int index);   // 0..1
    static const char16_t* lanes_heading();
    static const char16_t* preview_heading();
    static const char16_t* lane_caption();          // "Selected MIDI track/channel output:"
    static const char16_t* correction_help();
    static const char16_t* confirm_text();
    static std::u16string destination_item(int comboIndex);   // 0 "Automatic suggestion", 1..16 "Output channel N"
};

// What closed the dialog.
struct RoutingDialogOutcome {
    enum Kind { None, Apply, UseOriginal };
    Kind kind = None;
    std::shared_ptr<const routing::CompiledTrackRoute> route;   // Apply only
    routing::TrackRoutingSettings settings;                     // Apply only
    bool explicitly_confirmed = false;
};

class RoutingDialogModel {
public:
    // `initial` is the settings of the active route (or the defaults); `hadActiveRoute` enables
    // "Use original routing". No plan exists until recompute_now() or plan_ready().
    RoutingDialogModel(const routing::RoutingSong& song, const routing::TrackRoutingSettings& initial,
        bool hadActiveRoute, const format::NumberCulture& culture = format::NumberCulture());

    // ---- user input (each change that needs a new plan bumps generation() and clears the confirmation)
    void set_tier(int index);                       // 0 Exact-safe, 1 GM-family, 2 Aggressive; others ignored
    void set_capacity(int value);                   // clamped to 1..16
    void set_percussion(int index);                 // 0 Reserve channel 10, 1 Allow any
    void set_usable(int channel, bool usable);
    void select_row(int row);                       // -1 = none; selecting does not change the plan
    // Combo index of the selected lane: 0 Automatic suggestion, 1..16 = Output channel 1..16.
    void set_selected_lane_destination(int comboIndex);
    // The "I understand..." checkbox. A frontend can only change it while it is visible; the model
    // accepts it at any time (as v1's testing hook does) and clears it with every plan change.
    void set_lossy_confirmed(bool checked);
    // v1 ApplyClicked: publishes only when Apply is enabled. True when the dialog should close.
    bool apply();
    // v1 "Use original routing": enabled only when a route was active. True when the dialog should close.
    bool use_original();

    // ---- the plan
    uint64_t generation() const { return generation_; }
    bool plan_pending() const { return pending_; }
    // The settings the plan for generation() must be computed with.
    const routing::TrackRoutingSettings& settings() const { return settings_; }
    const routing::RoutingSong& song() const { return song_; }
    // Result of compile_route for `generation`; ignored (false) when stale.
    bool plan_ready(uint64_t generation, std::shared_ptr<const routing::CompiledTrackRoute> route);
    // The planner refused the song/settings (a thrown limit error); `message` is shown, Apply stays off.
    bool plan_failed(uint64_t generation, const std::string& message);
    // Compiles synchronously for the current generation (catching the planner's exceptions).
    void recompute_now();

    // ---- what the widgets show
    int tier_index() const { return tier_index_; }
    int capacity() const { return capacity_; }
    int percussion_index() const { return percussion_index_; }
    const std::array<bool, 16>& usable() const { return settings_.usable_destination_channels; }
    // Channels the current plan routes to (v1 shows their captions in bold).
    const std::array<bool, 16>& used() const { return used_; }
    const std::vector<RoutingDialogRow>& rows() const { return rows_; }
    int selected_row() const { return selected_row_; }
    bool selected_lane(routing::TrackLaneKey& lane) const;
    bool destination_enabled() const { return selected_row_ >= 0; }
    int destination_index() const;                  // -1 when no row is selected, or when the lane's saved manual destination is outside channels 1-16 (DD-005)
    const std::u16string& preview() const { return preview_; }
    bool apply_enabled() const;
    bool lossy_confirmation_visible() const;
    bool lossy_confirmation_checked() const { return confirmed_; }
    bool original_enabled() const { return had_active_route_; }
    bool status_is_error() const;
    std::u16string status_text() const;
    const routing::CompiledTrackRoute* compiled() const { return compiled_.get(); }
    // The planner refused the song or settings (see plan_failed): the message shown, empty otherwise.
    bool plan_has_failed() const { return failed_; }
    const std::u16string& failure_message() const { return failure_message_; }

    // ---- result
    bool finished() const { return outcome_.kind != RoutingDialogOutcome::None; }
    const RoutingDialogOutcome& outcome() const { return outcome_; }

private:
    void settings_changed();
    void adopt(std::shared_ptr<const routing::CompiledTrackRoute> route);
    void rebuild_rows();
    void restore_selection(bool hadLane, const routing::TrackLaneKey& lane);

    routing::RoutingSong song_;
    format::NumberCulture culture_;
    routing::TrackRoutingSettings settings_;
    bool had_active_route_ = false;
    int tier_index_ = 0;
    int capacity_ = 16;
    int percussion_index_ = 0;

    uint64_t generation_ = 0;
    bool pending_ = true;
    bool failed_ = false;
    std::u16string failure_message_;
    std::shared_ptr<const routing::CompiledTrackRoute> compiled_;
    std::array<bool, 16> used_;
    std::vector<RoutingDialogRow> rows_;
    int selected_row_ = -1;
    std::u16string preview_;
    bool confirmed_ = false;
    RoutingDialogOutcome outcome_;
};

} // namespace app
} // namespace midibn
