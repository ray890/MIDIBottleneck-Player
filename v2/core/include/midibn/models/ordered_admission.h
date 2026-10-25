// Group 2 models (Milestone 3): ordered finite admission. Ports of v1
// OrderedPlaybackGenerations.cs (work items, generations, generation queue)
// and the model-only part of OrderedFiniteAdmissionCore.cs.
//
// Not ported here: OrderedFiniteAdmissionCore.DispatchDueClockTicks, which
// converts through PlaybackEngine.MicrosecondsToTicks and the platform clock
// frequency; it belongs to the engine (Milestone 5).
#pragma once

#include "midibn/models/queue.h"

#include <functional>
#include <list>
#include <memory>
#include <vector>

namespace midibn {
namespace models {

struct TraceAccess;

enum class OutputWorkItemKind : int32_t { SourceMidi = 0, ProjectedGateMidi, ForcedOrChaseMidi, ProtectedSeekMarker };
enum class OutputWorkProtection : int32_t { Ordinary = 0, OrdinaryNoteAttack, ProtectedRelease, SafetyTraffic, PreservationBarrier };
enum class OverflowPolicy : int32_t {
    DropNewest = 0, DropOldest, ClearBufferAndCatchUp, DropIncomingCompleteNotes, DropOldestCompleteNote, DropBackgroundNotes
};

struct OutputWorkItem {
    int32_t generation_id = 0;
    OutputWorkItemKind kind = OutputWorkItemKind::SourceMidi;
    int32_t source_event_index = 0;
    int64_t logical_output_microseconds = 0;
    OutputWorkProtection protection = OutputWorkProtection::Ordinary;
    int64_t occurrence_id = 0;
    EventView midi_event;
    bool has_midi_event = false;

    static OutputWorkItem source(int32_t generationId, int32_t sourceEventIndex, int64_t logicalOutputMicroseconds,
        OutputWorkProtection protection, int64_t occurrenceId);
    static OutputWorkItem captured_source(int32_t generationId, int32_t sourceEventIndex,
        int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId, const EventView& midiEvent);
    static OutputWorkItem projected_gate(int32_t generationId, int32_t sourceEventIndex,
        int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId, const EventView& midiEvent);
    static OutputWorkItem forced_or_chase(int32_t generationId, int64_t logicalOutputMicroseconds,
        OutputWorkProtection protection, const EventView& midiEvent);
    static OutputWorkItem seek_marker(int32_t generationId, int64_t logicalOutputMicroseconds);

private:
    static OutputWorkItem make(int32_t generationId, OutputWorkItemKind kind, int32_t sourceEventIndex,
        int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId,
        const EventView& midiEvent, bool hasMidiEvent);
};

// Per-generation mode state of the finite admission core (v1 GenerationState).
struct FiniteGenerationState {
    CompleteNoteTracker complete_notes;
    int32_t musical_fairness_cursor = 0;
};

class PlaybackGeneration {
public:
    PlaybackGeneration(int32_t id, int64_t targetSourceMicroseconds, int32_t sourceEventCursor,
        int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks, const std::vector<OutputWorkItem>* chasePrefix,
        std::unique_ptr<FiniteGenerationState> modeState, int32_t maximumAdmittedWork);

    bool is_admission_closed() const { return admission_closed_; }
    int32_t admitted_work_count() const { return static_cast<int32_t>(admitted_.size()); }
    int32_t chase_prefix_count() const { return static_cast<int32_t>(chase_prefix_.size()); }
    const OutputWorkItem& chase_prefix_at(int32_t index) const { return chase_prefix_[static_cast<size_t>(index)]; }
    const std::list<OutputWorkItem>& admitted() const { return admitted_; }

    bool try_admit(const OutputWorkItem& item);
    bool try_evict_oldest_ordinary(OutputWorkItem& item);
    bool try_evict_ordinary_at_head(OutputWorkItem& item);
    bool try_evict_oldest_complete_note(OutputWorkItem& attack, OutputWorkItem& release, bool& hasRelease);
    bool try_evict_complete_note_at_head(OutputWorkItem& attack, OutputWorkItem& release, bool& hasRelease);
    bool try_evict_least_important_complete_note(int32_t& fairnessCursor, OutputWorkItem& attack,
        OutputWorkItem& release, bool& hasRelease);
    bool try_clear_open_admitted(std::vector<OutputWorkItem>& cleared);
    bool can_append_marker() const
    { return !admission_closed_ && static_cast<int32_t>(admitted_.size()) < maximum_admitted_work_; }
    void close_with_marker(const OutputWorkItem& marker);
    bool try_peek(OutputWorkItem& item) const;
    bool try_dequeue(OutputWorkItem& item);

    const int32_t id;
    const int64_t target_source_microseconds;
    int32_t source_event_cursor;
    int64_t source_clock_anchor_ticks;
    int64_t frozen_elapsed_ticks;
    std::unique_ptr<FiniteGenerationState> mode_state;

private:
    friend struct TraceAccess;
    std::list<OutputWorkItem> admitted_;
    std::vector<OutputWorkItem> chase_prefix_;
    int32_t maximum_admitted_work_;
    MusicalNotePrioritySelector musical_priority_;
    std::vector<MusicalNotePriorityCandidate> musical_candidates_ = std::vector<MusicalNotePriorityCandidate>(16);
    bool admission_closed_ = false;
};

class OrderedPlaybackGenerationQueue {
public:
    explicit OrderedPlaybackGenerationQueue(int32_t maximumLiveGenerations);

    int32_t live_generation_count() const { return static_cast<int32_t>(generations_.size()); }
    int32_t total_admitted_work_count() const;
    PlaybackGeneration* head() const { return generations_.empty() ? nullptr : generations_.front().get(); }
    PlaybackGeneration* tail() const { return generations_.empty() ? nullptr : generations_.back().get(); }
    const std::list<std::unique_ptr<PlaybackGeneration>>& generations() const { return generations_; }
    bool can_append_seek() const;

    void start(std::unique_ptr<PlaybackGeneration> generation);
    bool try_admit_to_tail(const OutputWorkItem& item);
    bool try_evict_oldest_ordinary_from_tail(OutputWorkItem& item);
    bool try_evict_oldest_complete_note_from_tail(OutputWorkItem& attack, OutputWorkItem& release, bool& hasRelease);
    bool try_clear_open_tail(std::vector<OutputWorkItem>& cleared);
    bool open_tail_is_dispatch_head();
    bool open_tail_is_ready_for_immediate_source();
    bool try_evict_ordinary_at_open_head(OutputWorkItem& item);
    bool try_evict_complete_note_at_open_head(OutputWorkItem& attack, OutputWorkItem& release, bool& hasRelease);
    bool try_evict_least_important_complete_note_from_tail(int32_t& fairnessCursor, OutputWorkItem& attack,
        OutputWorkItem& release, bool& hasRelease);
    bool try_append_seek(const OutputWorkItem& marker, std::unique_ptr<PlaybackGeneration>& nextGeneration);
    bool try_peek_dispatch(OutputWorkItem& item);
    bool try_dequeue_dispatch(OutputWorkItem& item);
    PlaybackGeneration* find_generation(int32_t generationId) const;
    void shift_source_clock_anchors(int64_t deltaTicks);

private:
    void retire_empty_closed_heads();

    std::list<std::unique_ptr<PlaybackGeneration>> generations_;
    int32_t maximum_live_generations_;
};

enum class OrderedFiniteAdmissionDisposition : int32_t { Accepted = 0, AcceptedSoftLimit, Rejected, Filtered };

struct OrderedFiniteAdmissionResult {
    OrderedFiniteAdmissionDisposition disposition = OrderedFiniteAdmissionDisposition::Accepted;
    std::vector<OutputWorkItem> evicted;
    bool paired_release_suppressed = false;
    bool accepted() const
    {
        return disposition == OrderedFiniteAdmissionDisposition::Accepted ||
            disposition == OrderedFiniteAdmissionDisposition::AcceptedSoftLimit;
    }
};

struct OrderedFiniteCapturedDecision {
    EventView captured_event;
    bool source_filtered = false;
};
using CaptureDecision = std::function<OrderedFiniteCapturedDecision(const EventView&)>;

struct OrderedFiniteClearResult {
    std::vector<OutputWorkItem> cleared_work;
    int32_t first_skipped_source_event_index = 0;
    int32_t source_event_cursor_after = 0;
    int32_t dropped_skipped_source_events = 0;
    int32_t filtered_skipped_source_events = 0;
    int32_t dropped_events() const { return static_cast<int32_t>(cleared_work.size()) + dropped_skipped_source_events; }
    bool requires_safety_reset() const
    { return !cleared_work.empty() || source_event_cursor_after != first_skipped_source_event_index; }
};

struct OrderedFinitePressureSnapshot {
    int32_t admitted_outstanding_count = 0;
    int32_t external_due_count = 0;
    int32_t total_due_count = 0;
    int64_t oldest_due_microseconds = 0;
    int64_t current_age_microseconds = 0;
};

struct OrderedFiniteAgeRepairResult {
    std::vector<OutputWorkItem> evicted;
    bool has_clear_result = false;
    OrderedFiniteClearResult clear_result;
    bool stalled_by_in_service_or_preservation_barrier = false;
    int64_t age_after_microseconds = 0;
};

class OrderedFiniteAdmissionCore {
public:
    OrderedFiniteAdmissionCore(EventSource events, int32_t maximumLiveGenerations);

    int32_t live_generation_count() const { return generations_.live_generation_count(); }
    int32_t pending_count() const { return generations_.total_admitted_work_count(); }
    int32_t outstanding_count() const;
    bool has_in_service() const { return has_in_service_; }
    const OutputWorkItem& in_service() const { return in_service_; }
    PlaybackGeneration* tail() const { return generations_.tail(); }
    const OrderedPlaybackGenerationQueue& generations() const { return generations_; }
    int32_t next_generation_id() const { return next_generation_id_; }

    bool try_peek_dispatch(OutputWorkItem& item) { return generations_.try_peek_dispatch(item); }
    void shift_source_clock_anchors(int64_t deltaTicks) { generations_.shift_source_clock_anchors(deltaTicks); }
    bool is_at_or_above_capacity(int32_t capacity) const;
    void start(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>* chasePrefix,
        int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks);
    OrderedFiniteAdmissionResult admit_next_source(int32_t capacity, OverflowPolicy policy);
    OrderedFiniteAdmissionResult admit_next_source(const EventView& capturedEvent, bool sourceFiltered,
        int32_t capacity, OverflowPolicy policy);
    bool try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>* chasePrefix,
        int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks);
    OrderedFiniteClearResult clear_open_tail_and_catch_up(int64_t throughMicroseconds, const CaptureDecision& captureDecision);
    OrderedFinitePressureSnapshot measure_due_pressure(int64_t logicalNowMicroseconds,
        const CaptureDecision& captureDecision);
    OrderedFiniteAgeRepairResult repair_waiting_time_pressure(int64_t logicalNowMicroseconds,
        int64_t ageLimitMicroseconds, OverflowPolicy policy, const CaptureDecision& captureDecision);
    bool try_begin_dispatch(OutputWorkItem& item);
    void complete_dispatch();

private:
    int64_t admitted_age(int64_t logicalNowMicroseconds);
    void admit_or_throw(const OutputWorkItem& item);
    std::unique_ptr<PlaybackGeneration> create_generation(int32_t id, int64_t targetMicroseconds,
        int32_t sourceEventCursor, const std::vector<EventView>* chasePrefix, int64_t sourceClockAnchorTicks,
        int64_t frozenElapsedTicks);
    static OutputWorkProtection protection_for(const EventView& midiEvent, CompleteNoteEventKind noteKind);

    EventSource events_;
    OrderedPlaybackGenerationQueue generations_;
    int32_t next_generation_id_ = 0;
    bool has_in_service_ = false;
    OutputWorkItem in_service_;
};

} // namespace models
} // namespace midibn
