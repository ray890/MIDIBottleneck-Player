// Ordered-generation adapters used by the preserve-on-Pause/Seek playback
// path (Milestone 5, Group D): ports of v1 OrderedUnlimitedQueueAdapter.cs,
// OrderedVirtualForwardAdapter.cs, OrderedMusicalGateAdapter.cs,
// OrderedNoteAttackAdapter.cs and OrderedOutputRateAdapter.cs over the
// Milestone 3 generation queue (models/ordered_admission.h).
//
// v1 keeps per-generation mode state in PlaybackGeneration.ModeState
// (object); these ports keep it in a map keyed by generation id inside the
// adapter (OrderedVirtualForwardAdapter already does so in v1), and drop the
// state of retired generations. A misuse of an adapter (a call out of order) throws
// models::ModelException; these are internal consistency checks, never shown to a person.
#pragma once

#include "midibn/engine/engine_types.h"
#include "midibn/models/delayed_note_off_gate.h"
#include "midibn/models/ordered_admission.h"
#include "midibn/models/per_note_gate.h"
#include "midibn/models/queue.h"
#include "midibn/models/rate.h"
#include "midibn/models/rate_gates.h"

#include <map>
#include <memory>
#include <vector>

namespace midibn {
namespace engine {

using models::CaptureDecision;
using models::EventView;
using models::OutputWorkItem;
using models::PlaybackGeneration;

// v1 OrderedUnlimitedQueueAdapter (the parts the engine uses).
class OrderedUnlimitedQueueAdapter {
public:
    OrderedUnlimitedQueueAdapter(models::EventSource events, int32_t maximumLiveGenerations);

    int32_t pending_work_count() const { return generations_.total_admitted_work_count(); }
    PlaybackGeneration* tail() const { return generations_.tail(); }

    void start(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
        int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks);
    bool try_admit_next(const EventView& capturedEvent, bool sourceFiltered);
    bool try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
        const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks);
    bool try_dequeue(OutputWorkItem& item) { return generations_.try_dequeue_dispatch(item); }
    bool try_peek(OutputWorkItem& item) { return generations_.try_peek_dispatch(item); }
    int64_t dispatch_due_clock_ticks(const OutputWorkItem& item, const TimeConversion& time) const
    { return engine::dispatch_due_clock_ticks(generations_, item, time); }
    void shift_source_clock_anchors(int64_t deltaTicks) { generations_.shift_source_clock_anchors(deltaTicks); }

private:
    std::unique_ptr<PlaybackGeneration> create_generation(int32_t id, int64_t targetMicroseconds,
        int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks,
        int64_t frozenElapsedTicks);

    models::EventSource events_;
    models::OrderedPlaybackGenerationQueue generations_;
    int32_t next_generation_id_ = 0;
};

// v1 OrderedVirtualAdmissionResult.
enum class OrderedVirtualAdmissionDisposition : int32_t { Accepted = 0, Rejected, Filtered };
struct OrderedVirtualAdmissionResult {
    OrderedVirtualAdmissionDisposition disposition = OrderedVirtualAdmissionDisposition::Accepted;
    OutputWorkItem work_item;
    bool paired_release_suppressed = false;
    bool accepted() const { return disposition == OrderedVirtualAdmissionDisposition::Accepted; }
};

// v1 OrderedVirtualForwardAdapter.
class OrderedVirtualForwardAdapter {
public:
    OrderedVirtualForwardAdapter(models::EventSource events, int32_t maximumLiveGenerations);

    int32_t pending_boundary_count() const { return generations_.total_admitted_work_count(); }
    int64_t dropped_events() const { return dropped_events_; }
    int32_t maximum_occupancy() const { return maximum_occupancy_; }
    PlaybackGeneration* tail() const { return generations_.tail(); }
    bool can_admit_tail_source() { return generations_.open_tail_is_ready_for_immediate_source(); }
    int32_t virtual_occupancy() const;

    void start(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
        int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks, int32_t capacity);
    bool try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
        const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks,
        int32_t capacity);
    OrderedVirtualAdmissionResult admit_next_source(const EventView& capturedEvent, bool sourceFiltered,
        int64_t serviceMicroseconds, models::OverflowPolicy policy);
    void advance_virtual_time(int64_t sourceMicroseconds);
    bool try_begin_boundary_dispatch(OutputWorkItem& item);
    void complete_boundary_dispatch();
    void shift_source_clock_anchors(int64_t deltaTicks) { generations_.shift_source_clock_anchors(deltaTicks); }

private:
    struct GenerationState {
        explicit GenerationState(int32_t capacity) : queue(capacity) {}
        models::ForwardDropQueue queue;
        models::CompleteNoteTracker complete_notes;
    };
    std::unique_ptr<PlaybackGeneration> create_generation(int32_t id, int64_t targetMicroseconds,
        int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks,
        int64_t frozenElapsedTicks, int32_t capacity);
    void record_occupancy(int32_t occupancy) { if (occupancy > maximum_occupancy_) maximum_occupancy_ = occupancy; }

    models::EventSource events_;
    models::OrderedPlaybackGenerationQueue generations_;
    std::map<int32_t, std::unique_ptr<GenerationState>> states_;
    int32_t next_generation_id_ = 0;
    int32_t active_pressure_generation_id_ = 0;
    bool has_boundary_in_service_ = false;
    OutputWorkItem boundary_in_service_;
    int64_t accepted_events_ = 0;
    int64_t dropped_events_ = 0;
    int64_t filtered_events_ = 0;
    int32_t maximum_occupancy_ = 0;
};

// v1 OrderedMusicalGateAdapter (Per-note interval gate and Delayed NoteOff).
class OrderedMusicalGateAdapter {
public:
    OrderedMusicalGateAdapter(models::EventSource events, int32_t maximumLiveGenerations, ProcessingMode mode,
        int64_t intervalMicroseconds, bool scaleByTraffic, bool intervalPolyphonyLimitEnabled,
        int32_t intervalPolyphonyLimit);

    int32_t pending_count() const { return generations_.total_admitted_work_count(); }
    int64_t filtered_events() const { return filtered_events_; }
    PlaybackGeneration* tail() const { return generations_.tail(); }
    bool tail_source_complete() const;

    bool try_peek_dispatch(OutputWorkItem& item) { return generations_.try_peek_dispatch(item); }
    int64_t dispatch_due_clock_ticks(const OutputWorkItem& item, const TimeConversion& time) const
    { return engine::dispatch_due_clock_ticks(generations_, item, time); }
    void shift_source_clock_anchors(int64_t deltaTicks) { generations_.shift_source_clock_anchors(deltaTicks); }
    void start(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
        int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks);
    int32_t admit_next_source_unit(const CaptureDecision& captureDecision);
    bool try_append_seek(int64_t closingSourceMicroseconds, int64_t targetMicroseconds, int32_t sourceEventCursor,
        const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks);
    void complete_tail_source(int64_t sourceEndMicroseconds);
    bool try_begin_dispatch(OutputWorkItem& item);
    void complete_dispatch();

private:
    struct GenerationState {
        GenerationState(ProcessingMode mode, int64_t intervalMicroseconds, bool scaleByTraffic,
            bool intervalPolyphonyLimitEnabled, int32_t intervalPolyphonyLimit);
        int64_t next_boundary_microseconds() const;
        bool has_pending_transitions() const;
        ProcessingMode mode;
        std::unique_ptr<models::PerNoteIntervalGate> per_note;
        std::unique_ptr<models::DelayedNoteOffGate> delayed;
        models::AdaptiveNoteIntervalClock adaptive;
        models::CompleteNoteTracker source_filters;
        bool source_complete = false;
    };
    GenerationState& state_of(const PlaybackGeneration& generation) const;
    void admit_captured_source(PlaybackGeneration& generation, GenerationState& state, const EventView& sourceEvent,
        const CaptureDecision& captureDecision);
    void resolve_boundaries_before_source(PlaybackGeneration& generation, GenerationState& state,
        const EventView& sourceEvent);
    void complete_generation_source(PlaybackGeneration& generation, int64_t sourceEndMicroseconds);
    void emit_boundary(PlaybackGeneration& generation, GenerationState& state, int64_t boundaryMicroseconds);
    void take_gate_filtered(GenerationState& state);
    static void admit_or_throw(PlaybackGeneration& generation, const OutputWorkItem& item);
    std::unique_ptr<PlaybackGeneration> create_generation(int32_t id, int64_t targetMicroseconds,
        int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks,
        int64_t frozenElapsedTicks);
    void prune_retired_states();

    models::EventSource events_;
    models::OrderedPlaybackGenerationQueue generations_;
    ProcessingMode mode_;
    int64_t interval_microseconds_;
    bool scale_by_traffic_;
    bool interval_polyphony_limit_enabled_;
    int32_t interval_polyphony_limit_;
    std::vector<EventView> emissions_ = std::vector<EventView>(128);
    std::vector<EventView> delayed_emissions_;
    std::map<int32_t, std::unique_ptr<GenerationState>> states_;
    int32_t next_generation_id_ = 0;
    bool has_in_service_ = false;
    OutputWorkItem in_service_;
    int64_t filtered_events_ = 0;
};

// v1 OrderedNoteAttackAdapter (Note start rate gate).
class OrderedNoteAttackAdapter {
public:
    OrderedNoteAttackAdapter(models::EventSource events, int32_t maximumLiveGenerations, int64_t attacksPerSecond);

    PlaybackGeneration* tail() const { return generations_.tail(); }
    int32_t pending_count() const { return generations_.total_admitted_work_count(); }
    int64_t rejected_attacks() const { return rejected_attacks_; }
    int64_t suppressed_releases() const { return suppressed_releases_; }
    bool tail_source_complete() const;

    void start(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
        int64_t sourceClockAnchorTicks);
    int32_t admit_next_source_tick(const CaptureDecision& captureDecision);
    bool try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
        const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks);
    void complete_tail_source();
    bool try_peek_dispatch(OutputWorkItem& item) { return generations_.try_peek_dispatch(item); }
    bool try_begin_dispatch(OutputWorkItem& item);
    void complete_dispatch();
    int64_t dispatch_due_clock_ticks(const OutputWorkItem& item, const TimeConversion& time) const
    { return engine::dispatch_due_clock_ticks(generations_, item, time); }
    void shift_source_clock_anchors(int64_t deltaTicks) { generations_.shift_source_clock_anchors(deltaTicks); }

private:
    struct GenerationState {
        explicit GenerationState(int64_t attacksPerSecond) : limiter(new models::MusicalAttackRateLimiter(attacksPerSecond)) {}
        std::unique_ptr<models::MusicalAttackRateLimiter> limiter;
        models::CompleteNoteTracker source_filters;
        std::vector<EventView> emissions;
        bool source_complete = false;
    };
    GenerationState& state_of(const PlaybackGeneration& generation) const;
    void complete_source(PlaybackGeneration& generation);
    void take_counts(GenerationState& state);
    std::unique_ptr<PlaybackGeneration> create_generation(int32_t id, int64_t targetMicroseconds,
        int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks);
    void prune_retired_states();

    models::EventSource events_;
    models::OrderedPlaybackGenerationQueue generations_;
    int64_t attacks_per_second_;
    std::map<int32_t, std::unique_ptr<GenerationState>> states_;
    int32_t next_generation_id_ = 0;
    bool has_in_service_ = false;
    int64_t rejected_attacks_ = 0;
    int64_t suppressed_releases_ = 0;
};

// v1 OrderedOutputRateAdapter (Output rate gate; one shared gate).
class OrderedOutputRateAdapter {
public:
    OrderedOutputRateAdapter(models::EventSource events, int32_t maximumLiveGenerations, int64_t eventsPerSecond,
        int64_t clockUnitsPerSecond);

    models::OutputRateGate* gate() { return gate_.get(); }
    PlaybackGeneration* tail() const { return generations_.tail(); }
    int32_t pending_count() const { return generations_.total_admitted_work_count(); }
    int64_t rejected_attacks() const { return rejected_attacks_; }
    int64_t suppressed_releases() const { return suppressed_releases_; }
    int64_t rejected_other_messages() const { return rejected_other_messages_; }
    bool tail_source_complete() const;

    void start(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
        int64_t sourceClockAnchorTicks);
    int32_t admit_next_source_tick(int64_t decisionTimestamp, const CaptureDecision& captureDecision);
    bool try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
        const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks);
    void complete_tail_source();
    bool try_peek_dispatch(OutputWorkItem& item) { return generations_.try_peek_dispatch(item); }
    bool try_begin_dispatch(OutputWorkItem& item);
    void complete_dispatch();
    int64_t dispatch_due_clock_ticks(const OutputWorkItem& item, const TimeConversion& time) const
    { return engine::dispatch_due_clock_ticks(generations_, item, time); }
    void shift_source_clock_anchors(int64_t deltaTicks) { generations_.shift_source_clock_anchors(deltaTicks); }

private:
    struct GenerationState {
        models::CompleteNoteTracker source_filters;
        std::vector<EventView> emissions;
        bool source_complete = false;
    };
    GenerationState& state_of(const PlaybackGeneration& generation) const;
    void complete_source(PlaybackGeneration& generation);
    void take_counts();
    std::unique_ptr<PlaybackGeneration> create_generation(int32_t id, int64_t targetMicroseconds,
        int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks);
    void prune_retired_states();

    models::EventSource events_;
    models::OrderedPlaybackGenerationQueue generations_;
    std::unique_ptr<models::OutputRateGate> gate_;
    std::map<int32_t, std::unique_ptr<GenerationState>> states_;
    int32_t next_generation_id_ = 0;
    bool has_in_service_ = false;
    int64_t rejected_attacks_ = 0;
    int64_t suppressed_releases_ = 0;
    int64_t rejected_other_messages_ = 0;
};

} // namespace engine
} // namespace midibn
