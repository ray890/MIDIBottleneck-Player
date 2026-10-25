// Track routing (Milestone 4): port of v1 TrackRouting.cs (lane profiles,
// the planner across the exact-safe, GM-family and aggressive tiers, the
// compiler with destination patch timelines and handoffs, omission, and the
// immutable routed event view shared by Playback and Analysis), plus the
// plan presentation logic of TrackRoutingForm.cs (route_plan_view.h).
//
// v1 iterates hash sets and dictionaries in a few places; every such loop
// was checked to be order-independent (results are sorted with a total
// order, or combined commutatively), so ordered containers are used here.
#pragma once

#include "midibn/midi_loader.h"
#include "midibn/models/model_base.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace midibn {
namespace routing {

using models::EventSource;
using models::EventView;

enum class TrackRoutingTier : int32_t { ExactSafe = 0, GmFamily = 1, Aggressive = 2 };
enum class PercussionRoutingPolicy : int32_t { ReserveChannel10 = 0, AllowAnyDestination = 1 };

struct TrackLaneKey {
    int32_t track = 0;
    int32_t source_channel = 0;
    TrackLaneKey() {}
    TrackLaneKey(int32_t t, int32_t c) : track(t), source_channel(c) {}
    bool operator==(const TrackLaneKey& o) const { return track == o.track && source_channel == o.source_channel; }
    bool operator!=(const TrackLaneKey& o) const { return !(*this == o); }
    int compare(const TrackLaneKey& o) const
    {
        if (track != o.track) return track < o.track ? -1 : 1;
        return source_channel < o.source_channel ? -1 : source_channel > o.source_channel ? 1 : 0;
    }
    bool operator<(const TrackLaneKey& o) const { return compare(o) < 0; }
    std::string to_string() const;  // "track N, ch M"
};

struct MidiPatchIdentity {
    int32_t bank_msb = 0;
    int32_t bank_lsb = 0;
    int32_t program = 0;
    MidiPatchIdentity() {}
    MidiPatchIdentity(int32_t msb, int32_t lsb, int32_t prog) : bank_msb(msb), bank_lsb(lsb), program(prog) {}
    int32_t gm_family() const { return program < 0 ? -1 : program / 8; }
    bool is_general_midi_bank() const { return bank_msb == 0 && bank_lsb == 0; }
    bool operator==(const MidiPatchIdentity& o) const
    { return bank_msb == o.bank_msb && bank_lsb == o.bank_lsb && program == o.program; }
    bool operator!=(const MidiPatchIdentity& o) const { return !(*this == o); }
    bool operator<(const MidiPatchIdentity& o) const;  // v1 ComparePatch order
    std::string to_string() const;  // "bank M/L, program P"
};

struct TrackRoutePatchChange {
    int32_t event_index = 0;
    MidiPatchIdentity patch;
};

struct TrackRouteSoundInterval {
    int32_t start_event_index = 0;
    int32_t end_event_index = 0;
    int64_t start_us = 0;
    int64_t end_us = 0;
    int64_t prefix_maximum_end_us = 0;
    int32_t key = 0;
};

struct TrackRouteLaneProfile {
    TrackLaneKey lane;
    std::u16string track_name;
    std::u16string instrument_name;
    int64_t event_count = 0;
    int64_t attack_count = 0;
    int64_t first_attack_us = -1;
    int64_t last_release_us = -1;
    bool has_dynamic_state = false;
    bool has_sustain = false;
    bool track_contains_system_traffic = false;
    uint64_t dynamic_state_signature = 0;
    MidiPatchIdentity representative_patch;
    uint64_t dominance_score = 0;
    std::set<MidiPatchIdentity> patch_history;
    std::vector<TrackRoutePatchChange> patch_timeline;
    std::vector<TrackRouteSoundInterval> sounding_intervals;
    // end_event_index of every interval, ascending (filled by the planner; empty means "compute when needed").
    std::vector<int32_t> sounding_end_events;

    std::u16string display_name() const;
};

struct TrackRoutingSettings {
    std::array<bool, 16> usable_destination_channels;
    int32_t target_capacity = 16;
    TrackRoutingTier maximum_automatic_tier = TrackRoutingTier::ExactSafe;
    PercussionRoutingPolicy percussion_policy = PercussionRoutingPolicy::ReserveChannel10;
    std::map<TrackLaneKey, int32_t> manual_destinations;
    TrackRoutingSettings() { usable_destination_channels.fill(true); }
};

struct TrackRouteAssignment {
    std::shared_ptr<const TrackRouteLaneProfile> profile;
    int32_t destination_channel = 0;
    TrackRoutingTier tier = TrackRoutingTier::ExactSafe;
    bool manual = false;
    bool representative_patch = false;
    bool state_conflict = false;
    bool same_key_overlap = false;
    bool time_handoff = false;
    bool percussion_exception = false;
    bool retimbred = false;
    bool omitted = false;
    std::vector<std::string> warnings;
};

struct TrackRouteDestinationPreview {
    int32_t destination_channel = 0;
    MidiPatchIdentity representative_patch;
    TrackLaneKey representative_lane;
    std::vector<std::shared_ptr<TrackRouteAssignment>> assignments;  // shared with CompiledTrackRoute::assignments
};

struct TrackRouteStatistics {
    int32_t routed_lane_count = 0;
    int32_t destination_count = 0;
    int32_t merged_lane_count = 0;
    int32_t retimbred_lane_count = 0;
    int32_t omitted_lane_count = 0;
    int64_t omitted_note_start_count = 0;
    int32_t generated_destination_state_count = 0;
    int32_t included_source_event_count = 0;
    int32_t routed_output_event_count = 0;
};

// The song as the planner reads it (v1 MidiSong): source events, track
// count, metadata, duration. `identity` stands for v1's source-store
// reference (CompiledTrackRoute.IsForSource).
struct RoutingSong {
    EventSource events;
    int32_t track_count = 0;
    int64_t duration_us = 0;
    const std::vector<TrackMetadata>* tracks = nullptr;
    const void* identity = nullptr;
};

// The Bank/Program events generated before one source event because the destination channel's patch changes there
// (v2 improvement DD-027: 12 bytes per source event instead of a map node and a vector of 64-byte event views;
// the events themselves take their tick, time and track from the source event and are built on demand).
struct TrackRouteGeneratedPatch {
    int32_t source_event_index = 0;
    uint8_t destination_channel = 0;
    uint8_t mask = 0;  // bit 0: Bank MSB (controller 0), bit 1: Bank LSB (controller 32), bit 2: Program Change
    uint8_t bank_msb = 0;
    uint8_t bank_lsb = 0;
    uint8_t program = 0;
    // One to three events, in the order Bank MSB, Bank LSB, Program Change.
    int event_count() const { return (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1); }
};

class CompiledTrackRoute {
public:
    CompiledTrackRoute(const RoutingSong& source, std::vector<std::shared_ptr<TrackRouteLaneProfile>> lanes,
        std::vector<std::shared_ptr<TrackRouteAssignment>> assignments,
        std::vector<TrackRouteDestinationPreview> destinations, std::vector<std::string> errors,
        bool requiresExplicitApply, TrackRoutingTier highestTier);

    bool is_valid() const { return errors.empty(); }
    bool is_for_source(const void* identity) const { return identity == source_identity_; }
    bool try_transform(const EventView& source, EventView& routed) const;
    bool includes(const EventView& source) const;
    // Generated Bank/Program events placed before a source event, or null.
    const TrackRouteGeneratedPatch* generated_patch_before(int32_t sourceEventIndex) const;
    int32_t event_destination(int32_t sourceEventIndex) const { return event_destinations_[static_cast<size_t>(sourceEventIndex)]; }
    int32_t source_event_count() const { return static_cast<int32_t>(event_destinations_.size()); }
    // All generated groups, ascending by source event index.
    const std::vector<TrackRouteGeneratedPatch>& generated_patches() const { return generated_; }
    // The k-th event (0 <= k < event_count()) of a generated group, built from its source event.
    EventView generated_event(const TrackRouteGeneratedPatch& group, int k) const;

    std::vector<std::shared_ptr<TrackRouteLaneProfile>> lanes;  // display order (by lane)
    std::vector<std::shared_ptr<TrackRouteAssignment>> assignments;  // by lane
    std::vector<TrackRouteDestinationPreview> destinations;  // by destination channel
    std::vector<std::string> errors;
    bool requires_explicit_apply = false;
    TrackRoutingTier highest_tier = TrackRoutingTier::ExactSafe;
    TrackRouteStatistics statistics;

private:
    void build_canonical_patch_events(const EventSource& source);
    void build_statistics(const EventSource& source);

    const void* source_identity_;
    EventSource source_events_;  // the song the route was compiled for (valid while that song is; see is_for_source)
    std::vector<int32_t> destinations_by_slot_;
    std::vector<int8_t> event_destinations_;
    std::vector<TrackRouteGeneratedPatch> generated_;
};

// v1 TrackRoutePlanner.Compile. Throws models::ModelException
// (InvalidOperationException) at v1's bounded-analysis limits.
const int32_t kMaximumActiveAttacks = 1000000;
const int32_t kMaximumDistinctPatchesPerLane = 4096;
std::shared_ptr<CompiledTrackRoute> compile_route(const RoutingSong& song, const TrackRoutingSettings& settings);

// v1 TrackRoutedEventStore: the immutable routed view. Throws
// InvalidOperationException for a route of another song or an invalid route.
class TrackRoutedEventStore : public models::EventViewStore {
public:
    TrackRoutedEventStore(const RoutingSong& source, std::shared_ptr<const CompiledTrackRoute> route);
    int32_t count() const override { return static_cast<int32_t>(entries_.size()); }
    EventView at(int32_t index) const override;

private:
    // One int32 per routed event: a source event index (>= 0), or a generated event, stored as
    // -(1 + (group ordinal * 4 + k)) for the k-th event of the route's group number `ordinal`.
    EventSource source_;
    std::shared_ptr<const CompiledTrackRoute> route_;
    std::vector<int32_t> entries_;
};

} // namespace routing
} // namespace midibn
