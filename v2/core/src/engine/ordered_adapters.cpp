#include "midibn/engine/ordered_adapters.h"

namespace midibn {
namespace engine {

using models::CompleteNoteEventKind;
using models::CompleteNoteTracker;
using models::OutputWorkProtection;

namespace {

std::vector<OutputWorkItem> chase_work(int32_t id, int64_t targetMicroseconds, const std::vector<EventView>& prefix)
{
    std::vector<OutputWorkItem> work;
    work.reserve(prefix.size());
    for (const EventView& e : prefix)
        work.push_back(OutputWorkItem::forced_or_chase(id, targetMicroseconds, OutputWorkProtection::SafetyTraffic, e));
    return work;
}

int32_t bound_of(int64_t requested)
{
    return requested > models::kInt32Max ? models::kInt32Max : (requested < 1 ? 1 : static_cast<int32_t>(requested));
}

// v1 per-adapter ProtectionFor / inline protection selection for emitted work.
OutputWorkProtection emitted_protection(const EventView& e)
{
    CompleteNoteEventKind kind = CompleteNoteTracker::classify(e);
    if (kind == CompleteNoteEventKind::NoteOn) return OutputWorkProtection::OrdinaryNoteAttack;
    if (kind == CompleteNoteEventKind::NoteOff) return OutputWorkProtection::ProtectedRelease;
    if (CompleteNoteTracker::is_safety_control(e)) return OutputWorkProtection::SafetyTraffic;
    return OutputWorkProtection::Ordinary;
}

} // namespace

// ---------------------------------------------------------------- unlimited Queue

OrderedUnlimitedQueueAdapter::OrderedUnlimitedQueueAdapter(models::EventSource events, int32_t maximumLiveGenerations)
    : events_(events), generations_(maximumLiveGenerations)
{
}

void OrderedUnlimitedQueueAdapter::start(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
{
    if (generations_.live_generation_count() != 0) models::throw_invalid_operation("The adapter has already started.");
    std::unique_ptr<PlaybackGeneration> generation = create_generation(++next_generation_id_, targetMicroseconds,
        sourceEventCursor, chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks);
    generations_.start(std::move(generation));
}

bool OrderedUnlimitedQueueAdapter::try_admit_next(const EventView& capturedEvent, bool sourceFiltered)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor < 0 ||
        last->source_event_cursor >= events_.count())
        return false;
    int32_t sourceIndex = last->source_event_cursor;
    if (sourceFiltered) {
        last->source_event_cursor++;
        return true;
    }
    // v1 ProtectionFor (Queue): releases and safety traffic only.
    OutputWorkProtection protection = OutputWorkProtection::Ordinary;
    if (CompleteNoteTracker::classify(capturedEvent) == CompleteNoteEventKind::NoteOff)
        protection = OutputWorkProtection::ProtectedRelease;
    else if (CompleteNoteTracker::is_safety_control(capturedEvent))
        protection = OutputWorkProtection::SafetyTraffic;
    OutputWorkItem item = OutputWorkItem::captured_source(last->id, sourceIndex, capturedEvent.micros, protection,
        sourceIndex, capturedEvent);
    if (!generations_.try_admit_to_tail(item)) return false;
    last->source_event_cursor++;
    return true;
}

bool OrderedUnlimitedQueueAdapter::try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr) return false;
    int32_t nextId = next_generation_id_ + 1;
    std::unique_ptr<PlaybackGeneration> next = create_generation(nextId, targetMicroseconds, sourceEventCursor,
        chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks);
    if (!generations_.try_append_seek(OutputWorkItem::seek_marker(last->id, targetMicroseconds), next)) return false;
    next_generation_id_ = nextId;
    return true;
}

std::unique_ptr<PlaybackGeneration> OrderedUnlimitedQueueAdapter::create_generation(int32_t id,
    int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
    int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
{
    if (sourceEventCursor < 0 || sourceEventCursor > events_.count())
        models::throw_argument_out_of_range("sourceEventCursor");
    std::vector<OutputWorkItem> work = chase_work(id, targetMicroseconds, chasePrefix);
    int32_t workBound = bound_of(static_cast<int64_t>(events_.count()) + static_cast<int64_t>(work.size()) + 1);
    return std::unique_ptr<PlaybackGeneration>(new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
        sourceClockAnchorTicks, frozenElapsedTicks, &work, nullptr, workBound));
}

// ---------------------------------------------------------------- virtual forward

OrderedVirtualForwardAdapter::OrderedVirtualForwardAdapter(models::EventSource events, int32_t maximumLiveGenerations)
    : events_(events), generations_(maximumLiveGenerations)
{
}

int32_t OrderedVirtualForwardAdapter::virtual_occupancy() const
{
    std::map<int32_t, std::unique_ptr<GenerationState>>::const_iterator found =
        states_.find(active_pressure_generation_id_);
    return found != states_.end() ? found->second->queue.occupancy() : 0;
}

void OrderedVirtualForwardAdapter::start(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks,
    int32_t capacity)
{
    if (generations_.live_generation_count() != 0)
        models::throw_invalid_operation("The virtual adapter has already started.");
    int32_t id = ++next_generation_id_;
    std::unique_ptr<PlaybackGeneration> generation = create_generation(id, targetMicroseconds, sourceEventCursor,
        chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks, capacity);
    generations_.start(std::move(generation));
    active_pressure_generation_id_ = id;
}

bool OrderedVirtualForwardAdapter::try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks,
    int32_t capacity)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr) return false;
    int32_t nextId = next_generation_id_ + 1;
    std::unique_ptr<PlaybackGeneration> next = create_generation(nextId, targetMicroseconds, sourceEventCursor,
        chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks, capacity);
    if (!generations_.try_append_seek(OutputWorkItem::seek_marker(last->id, targetMicroseconds), next)) {
        states_.erase(nextId);
        return false;
    }
    next_generation_id_ = nextId;
    return true;
}

OrderedVirtualAdmissionResult OrderedVirtualForwardAdapter::admit_next_source(const EventView& capturedEvent,
    bool sourceFiltered, int64_t serviceMicroseconds, models::OverflowPolicy policy)
{
    if (serviceMicroseconds < 0) models::throw_argument_out_of_range("serviceMicroseconds");
    if (policy != models::OverflowPolicy::DropNewest && policy != models::OverflowPolicy::DropIncomingCompleteNotes)
        models::throw_not_supported("Forward-only admission cannot retract already dispatched MIDI.");
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor >= events_.count())
        models::throw_invalid_operation("No open virtual source event is available.");
    if (!generations_.open_tail_is_ready_for_immediate_source() || active_pressure_generation_id_ != last->id)
        models::throw_invalid_operation("A marker or chase boundary must dispatch before target source MIDI.");

    int32_t sourceIndex = last->source_event_cursor;
    if (!capturedEvent.is_valid() || capturedEvent.event_index() != sourceIndex)
        models::throw_argument("capturedEvent", "Captured virtual source work does not match the tail cursor.");
    last->source_event_cursor++;
    std::map<int32_t, std::unique_ptr<GenerationState>>::iterator found = states_.find(last->id);
    if (found == states_.end()) {
        models::ModelException e{ "KeyNotFoundException", "", "The given key was not present in the dictionary." };
        throw e;
    }
    GenerationState& state = *found->second;
    CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(capturedEvent);
    CompleteNoteTracker::NoteOffMatch noteMatch;
    noteMatch.attack_index = 0;  // C# default(NoteOffMatch)
    OrderedVirtualAdmissionResult result;
    if (sourceFiltered) {
        if (noteKind == CompleteNoteEventKind::NoteOn)
            state.complete_notes.record_filtered_note_on(capturedEvent, sourceIndex);
        else if (noteKind == CompleteNoteEventKind::NoteOff)
            noteMatch = state.complete_notes.take_note_off(capturedEvent);
        filtered_events_++;
        result.disposition = OrderedVirtualAdmissionDisposition::Filtered;
        result.paired_release_suppressed = noteMatch.suppress;
        return result;
    }

    if (noteKind == CompleteNoteEventKind::NoteOff) {
        noteMatch = state.complete_notes.take_note_off(capturedEvent);
        if (noteMatch.suppress) {
            dropped_events_++;
            result.disposition = OrderedVirtualAdmissionDisposition::Rejected;
            result.paired_release_suppressed = true;
            return result;
        }
    }

    bool safetyAdmission = policy == models::OverflowPolicy::DropIncomingCompleteNotes &&
        noteKind != CompleteNoteEventKind::NoteOn;
    bool accepted = state.queue.try_admit(capturedEvent.micros, serviceMicroseconds, safetyAdmission);
    record_occupancy(state.queue.occupancy());
    if (!accepted) {
        if (noteKind == CompleteNoteEventKind::NoteOn)
            state.complete_notes.record_note_on(capturedEvent, sourceIndex, false,
                policy == models::OverflowPolicy::DropIncomingCompleteNotes);
        dropped_events_++;
        result.disposition = OrderedVirtualAdmissionDisposition::Rejected;
        return result;
    }
    if (noteKind == CompleteNoteEventKind::NoteOn)
        state.complete_notes.record_note_on(capturedEvent, sourceIndex, true, false);
    accepted_events_++;
    OutputWorkProtection protection = emitted_protection(capturedEvent);
    int64_t occurrenceId = noteKind == CompleteNoteEventKind::NoteOn ? sourceIndex
        : noteKind == CompleteNoteEventKind::NoteOff && noteMatch.attack_index >= 0 ? noteMatch.attack_index
        : sourceIndex;
    result.disposition = OrderedVirtualAdmissionDisposition::Accepted;
    result.work_item = OutputWorkItem::captured_source(last->id, sourceIndex, capturedEvent.micros, protection,
        occurrenceId, capturedEvent);
    return result;
}

void OrderedVirtualForwardAdapter::advance_virtual_time(int64_t sourceMicroseconds)
{
    if (sourceMicroseconds < 0) models::throw_argument_out_of_range("sourceMicroseconds");
    std::map<int32_t, std::unique_ptr<GenerationState>>::iterator found = states_.find(active_pressure_generation_id_);
    if (found == states_.end()) return;
    found->second->queue.advance(sourceMicroseconds);
    record_occupancy(found->second->queue.occupancy());
}

bool OrderedVirtualForwardAdapter::try_begin_boundary_dispatch(OutputWorkItem& item)
{
    if (has_boundary_in_service_) {
        item = OutputWorkItem();
        return false;
    }
    if (!generations_.try_dequeue_dispatch(item)) return false;
    boundary_in_service_ = item;
    has_boundary_in_service_ = true;
    return true;
}

void OrderedVirtualForwardAdapter::complete_boundary_dispatch()
{
    if (!has_boundary_in_service_) models::throw_invalid_operation("No virtual boundary work is in service.");
    if (boundary_in_service_.kind == models::OutputWorkItemKind::ProtectedSeekMarker) {
        int32_t completedGeneration = boundary_in_service_.generation_id;
        std::map<int32_t, std::unique_ptr<GenerationState>>::iterator found = states_.find(completedGeneration);
        if (found != states_.end()) record_occupancy(found->second->queue.maximum_occupancy());
        states_.erase(completedGeneration);
        active_pressure_generation_id_ = completedGeneration + 1;
    }
    boundary_in_service_ = OutputWorkItem();
    has_boundary_in_service_ = false;
}

std::unique_ptr<PlaybackGeneration> OrderedVirtualForwardAdapter::create_generation(int32_t id,
    int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix,
    int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks, int32_t capacity)
{
    if (capacity < 1) models::throw_argument_out_of_range("capacity");
    if (sourceEventCursor < 0 || sourceEventCursor > events_.count())
        models::throw_argument_out_of_range("sourceEventCursor");
    std::vector<OutputWorkItem> work = chase_work(id, targetMicroseconds, chasePrefix);
    std::unique_ptr<GenerationState> state(new GenerationState(capacity));
    if (states_.find(id) != states_.end()) models::throw_duplicate_key();
    states_[id] = std::move(state);   // v1 adds the state before constructing the generation
    int32_t bound = static_cast<int32_t>(work.size()) + 1;
    return std::unique_ptr<PlaybackGeneration>(new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
        sourceClockAnchorTicks, frozenElapsedTicks, &work, nullptr, bound < 1 ? 1 : bound));
}

// ---------------------------------------------------------------- musical gates

OrderedMusicalGateAdapter::GenerationState::GenerationState(ProcessingMode m, int64_t intervalMicroseconds,
    bool scaleByTraffic, bool intervalPolyphonyLimitEnabled, int32_t intervalPolyphonyLimit)
    : mode(m), adaptive(intervalMicroseconds, scaleByTraffic)
{
    // v1 constructs the gate before the adaptive clock; neither constructor
    // has side effects beyond its own state, so the order is not observable.
    if (m == ProcessingMode::PerNoteIntervalGate)
        per_note.reset(new models::PerNoteIntervalGate(intervalMicroseconds, intervalPolyphonyLimitEnabled,
            intervalPolyphonyLimit));
    else if (m == ProcessingMode::DelayedNoteOff)
        delayed.reset(new models::DelayedNoteOffGate(intervalMicroseconds, intervalPolyphonyLimitEnabled,
            intervalPolyphonyLimit));
    else models::throw_argument_out_of_range("mode");
}

int64_t OrderedMusicalGateAdapter::GenerationState::next_boundary_microseconds() const
{
    return mode == ProcessingMode::PerNoteIntervalGate ? per_note->next_boundary_microseconds()
                                                      : delayed->next_boundary_microseconds();
}

bool OrderedMusicalGateAdapter::GenerationState::has_pending_transitions() const
{
    return mode == ProcessingMode::PerNoteIntervalGate ? per_note->has_pending_transitions()
                                                      : delayed->has_pending_transitions();
}

OrderedMusicalGateAdapter::OrderedMusicalGateAdapter(models::EventSource events, int32_t maximumLiveGenerations,
    ProcessingMode mode, int64_t intervalMicroseconds, bool scaleByTraffic, bool intervalPolyphonyLimitEnabled,
    int32_t intervalPolyphonyLimit)
    : events_(events), generations_(maximumLiveGenerations), mode_(mode), interval_microseconds_(intervalMicroseconds),
      scale_by_traffic_(scaleByTraffic), interval_polyphony_limit_enabled_(intervalPolyphonyLimitEnabled),
      interval_polyphony_limit_(128)
{
    if (mode != ProcessingMode::PerNoteIntervalGate && mode != ProcessingMode::DelayedNoteOff)
        models::throw_argument_out_of_range("mode");
    if (intervalMicroseconds < 0) models::throw_argument_out_of_range("intervalMicroseconds");
    delayed_emissions_.reserve(256);
    interval_polyphony_limit_ = intervalPolyphonyLimitEnabled
        ? (intervalPolyphonyLimit < 1 ? 1 : intervalPolyphonyLimit > 128 ? 128 : intervalPolyphonyLimit)
        : 128;
}

OrderedMusicalGateAdapter::GenerationState& OrderedMusicalGateAdapter::state_of(const PlaybackGeneration& generation) const
{
    return *states_.find(generation.id)->second;
}

bool OrderedMusicalGateAdapter::tail_source_complete() const
{
    PlaybackGeneration* last = tail();
    return last != nullptr && state_of(*last).source_complete;
}

void OrderedMusicalGateAdapter::start(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
{
    if (generations_.live_generation_count() != 0)
        models::throw_invalid_operation("The musical gate adapter has already started.");
    std::unique_ptr<PlaybackGeneration> generation = create_generation(++next_generation_id_, targetMicroseconds,
        sourceEventCursor, chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks);
    generations_.start(std::move(generation));
}

int32_t OrderedMusicalGateAdapter::admit_next_source_unit(const CaptureDecision& captureDecision)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor >= events_.count())
        models::throw_invalid_operation("No open musical-gate source work is available.");
    GenerationState& state = state_of(*last);
    if (state.source_complete) models::throw_invalid_operation("The musical-gate source is already complete.");
    EventView first = events_.at(last->source_event_cursor);
    resolve_boundaries_before_source(*last, state, first);

    if (mode_ == ProcessingMode::DelayedNoteOff) {
        admit_captured_source(*last, state, first, captureDecision);
        return 1;
    }

    int64_t tick = first.tick;
    state.per_note->begin_source_tick(tick);
    int32_t admitted = 0;
    while (last->source_event_cursor < events_.count() && events_.at(last->source_event_cursor).tick == tick) {
        EventView sourceEvent = events_.at(last->source_event_cursor);
        admit_captured_source(*last, state, sourceEvent, captureDecision);
        admitted++;
    }
    state.per_note->end_source_tick();
    take_gate_filtered(state);
    return admitted;
}

bool OrderedMusicalGateAdapter::try_append_seek(int64_t closingSourceMicroseconds, int64_t targetMicroseconds,
    int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks,
    int64_t frozenElapsedTicks)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || !generations_.can_append_seek()) return false;
    complete_generation_source(*last, closingSourceMicroseconds);
    int32_t nextId = next_generation_id_ + 1;
    std::unique_ptr<PlaybackGeneration> next = create_generation(nextId, targetMicroseconds, sourceEventCursor,
        chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks);
    if (!generations_.try_append_seek(OutputWorkItem::seek_marker(last->id, targetMicroseconds), next))
        models::throw_invalid_operation("A preflighted musical-gate Seek boundary could not be appended.");
    next_generation_id_ = nextId;
    prune_retired_states();
    return true;
}

void OrderedMusicalGateAdapter::complete_tail_source(int64_t sourceEndMicroseconds)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed())
        models::throw_invalid_operation("No open musical-gate generation can complete.");
    complete_generation_source(*last, sourceEndMicroseconds);
}

bool OrderedMusicalGateAdapter::try_begin_dispatch(OutputWorkItem& item)
{
    if (has_in_service_) {
        item = OutputWorkItem();
        return false;
    }
    if (!generations_.try_dequeue_dispatch(item)) return false;
    in_service_ = item;
    has_in_service_ = true;
    return true;
}

void OrderedMusicalGateAdapter::complete_dispatch()
{
    if (!has_in_service_) models::throw_invalid_operation("No musical-gate work is in service.");
    in_service_ = OutputWorkItem();
    has_in_service_ = false;
}

void OrderedMusicalGateAdapter::admit_captured_source(PlaybackGeneration& generation, GenerationState& state,
    const EventView& sourceEvent, const CaptureDecision& captureDecision)
{
    int32_t sourceIndex = generation.source_event_cursor;
    models::OrderedFiniteCapturedDecision decision;
    if (captureDecision) decision = captureDecision(sourceEvent);
    else {
        decision.captured_event = sourceEvent;
        decision.source_filtered = false;
    }
    EventView captured = decision.captured_event;
    if (!captured.is_valid() || captured.event_index() != sourceIndex)
        models::throw_invalid_operation("A musical-gate source decision did not preserve source identity.");
    generation.source_event_cursor++;
    CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(captured);
    if (decision.source_filtered) {
        if (noteKind == CompleteNoteEventKind::NoteOn) state.source_filters.record_filtered_note_on(captured, sourceIndex);
        else if (noteKind == CompleteNoteEventKind::NoteOff) state.source_filters.take_note_off(captured);
        filtered_events_++;
        return;
    }
    if (noteKind == CompleteNoteEventKind::NoteOn) state.source_filters.record_note_on(captured, sourceIndex, true, false);
    else if (noteKind == CompleteNoteEventKind::NoteOff) {
        CompleteNoteTracker::NoteOffMatch noteMatch = state.source_filters.take_note_off(captured);
        if (noteMatch.suppress) {
            filtered_events_++;
            return;
        }
    }

    bool notNote;
    if (mode_ == ProcessingMode::PerNoteIntervalGate)
        notNote = state.per_note->admit(captured) == models::PerNoteGateAdmission::NotNote;
    else
        notNote = state.delayed->admit(captured) == models::DelayedNoteOffAdmission::NotNote;
    if (notNote) {
        // v1 ProtectionFor (musical gate) equals the emitted-work rule.
        admit_or_throw(generation, OutputWorkItem::captured_source(generation.id, sourceIndex, captured.micros,
            emitted_protection(captured), sourceIndex, captured));
    }
    take_gate_filtered(state);
}

void OrderedMusicalGateAdapter::resolve_boundaries_before_source(PlaybackGeneration& generation, GenerationState& state,
    const EventView& sourceEvent)
{
    while (state.has_pending_transitions()) {
        int64_t localBoundary = state.next_boundary_microseconds();
        state.adaptive.complete_if_closed(localBoundary, sourceEvent.micros);
        int64_t boundary = state.adaptive.effective_boundary(localBoundary);
        bool emit = mode_ == ProcessingMode::PerNoteIntervalGate
            ? boundary < sourceEvent.micros
            : boundary < sourceEvent.micros ||
                (boundary == sourceEvent.micros && !state.delayed->source_precedes_boundary(sourceEvent, boundary));
        if (!emit) break;
        emit_boundary(generation, state, boundary);
    }
}

void OrderedMusicalGateAdapter::complete_generation_source(PlaybackGeneration& generation, int64_t sourceEndMicroseconds)
{
    GenerationState& state = state_of(generation);
    if (!state.source_complete) {
        if (mode_ == ProcessingMode::PerNoteIntervalGate) state.per_note->complete_source(sourceEndMicroseconds);
        else state.delayed->complete_source(sourceEndMicroseconds);
        state.source_complete = true;
        take_gate_filtered(state);
    }
    while (state.has_pending_transitions()) {
        int64_t localBoundary = state.next_boundary_microseconds();
        state.adaptive.complete_if_closed(localBoundary, models::kInt64Max);
        int64_t boundary = state.adaptive.effective_boundary(localBoundary);
        emit_boundary(generation, state, boundary);
    }
}

void OrderedMusicalGateAdapter::emit_boundary(PlaybackGeneration& generation, GenerationState& state,
    int64_t boundaryMicroseconds)
{
    int32_t count;
    if (mode_ == ProcessingMode::PerNoteIntervalGate)
        count = state.per_note->emit_boundary(boundaryMicroseconds, emissions_.data(),
            static_cast<int32_t>(emissions_.size()));
    else count = state.delayed->emit_boundary(boundaryMicroseconds, delayed_emissions_);
    state.adaptive.record_boundary_pass(boundaryMicroseconds, count);
    take_gate_filtered(state);
    for (int32_t index = 0; index < count; ++index) {
        const EventView& emitted = mode_ == ProcessingMode::PerNoteIntervalGate
            ? emissions_[static_cast<size_t>(index)] : delayed_emissions_[static_cast<size_t>(index)];
        int32_t sourceIndex = emitted.event_index();
        int64_t occurrenceId = sourceIndex >= 0 ? sourceIndex
            : static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(generation.id)) << 32) |
                static_cast<uint32_t>(emitted.data_byte(1) & 0x7F));
        admit_or_throw(generation, OutputWorkItem::projected_gate(generation.id, sourceIndex, boundaryMicroseconds,
            emitted_protection(emitted), occurrenceId, emitted));
    }
}

void OrderedMusicalGateAdapter::take_gate_filtered(GenerationState& state)
{
    int64_t filtered = mode_ == ProcessingMode::PerNoteIntervalGate ? state.per_note->take_filtered_event_count()
                                                                    : state.delayed->take_filtered_event_count();
    filtered_events_ = filtered_events_ > models::kInt64Max - filtered ? models::kInt64Max : filtered_events_ + filtered;
}

void OrderedMusicalGateAdapter::admit_or_throw(PlaybackGeneration& generation, const OutputWorkItem& item)
{
    if (!generation.try_admit(item))
        models::throw_invalid_operation("The musical-gate generation exceeded its admitted-work bound.");
}

std::unique_ptr<PlaybackGeneration> OrderedMusicalGateAdapter::create_generation(int32_t id, int64_t targetMicroseconds,
    int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks,
    int64_t frozenElapsedTicks)
{
    if (sourceEventCursor < 0 || sourceEventCursor > events_.count())
        models::throw_argument_out_of_range("sourceEventCursor");
    std::vector<OutputWorkItem> work = chase_work(id, targetMicroseconds, chasePrefix);
    int32_t workBound = bound_of(static_cast<int64_t>(events_.count()) * 3 + static_cast<int64_t>(work.size()) + 257);
    std::unique_ptr<GenerationState> state(new GenerationState(mode_, interval_microseconds_, scale_by_traffic_,
        interval_polyphony_limit_enabled_, interval_polyphony_limit_));
    std::unique_ptr<PlaybackGeneration> generation(new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
        sourceClockAnchorTicks, frozenElapsedTicks, &work, nullptr, workBound));
    states_[id] = std::move(state);
    return generation;
}

void OrderedMusicalGateAdapter::prune_retired_states()
{
    for (std::map<int32_t, std::unique_ptr<GenerationState>>::iterator it = states_.begin(); it != states_.end();) {
        if (generations_.find_generation(it->first) == nullptr) it = states_.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------- note start rate

OrderedNoteAttackAdapter::OrderedNoteAttackAdapter(models::EventSource events, int32_t maximumLiveGenerations,
    int64_t attacksPerSecond)
    : events_(events), generations_(maximumLiveGenerations), attacks_per_second_(attacksPerSecond)
{
}

OrderedNoteAttackAdapter::GenerationState& OrderedNoteAttackAdapter::state_of(const PlaybackGeneration& generation) const
{
    return *states_.find(generation.id)->second;
}

bool OrderedNoteAttackAdapter::tail_source_complete() const
{
    PlaybackGeneration* last = tail();
    return last != nullptr && state_of(*last).source_complete;
}

void OrderedNoteAttackAdapter::start(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks)
{
    std::unique_ptr<PlaybackGeneration> generation = create_generation(++next_generation_id_, targetMicroseconds,
        sourceEventCursor, chasePrefix, sourceClockAnchorTicks);
    generations_.start(std::move(generation));
}

int32_t OrderedNoteAttackAdapter::admit_next_source_tick(const CaptureDecision& captureDecision)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor >= events_.count())
        models::throw_invalid_operation("No note-start source work is available.");
    GenerationState& state = state_of(*last);
    int64_t tick = events_.at(last->source_event_cursor).tick;
    state.limiter->begin_source_tick(tick);
    int32_t count = 0;
    while (last->source_event_cursor < events_.count() && events_.at(last->source_event_cursor).tick == tick) {
        EventView source = events_.at(last->source_event_cursor);
        models::OrderedFiniteCapturedDecision decision;
        if (captureDecision) decision = captureDecision(source);
        else {
            decision.captured_event = source;
            decision.source_filtered = false;
        }
        if (!decision.captured_event.is_valid() || decision.captured_event.event_index() != last->source_event_cursor)
            models::throw_invalid_operation("Captured note-start work lost source identity.");
        last->source_event_cursor++;
        count++;
        CompleteNoteEventKind kind = CompleteNoteTracker::classify(decision.captured_event);
        if (decision.source_filtered) {
            if (kind == CompleteNoteEventKind::NoteOn)
                state.source_filters.record_filtered_note_on(decision.captured_event, decision.captured_event.event_index());
            else if (kind == CompleteNoteEventKind::NoteOff)
                state.source_filters.take_note_off(decision.captured_event);
            continue;
        }
        if (kind == CompleteNoteEventKind::NoteOff && state.source_filters.take_note_off(decision.captured_event).suppress)
            continue;
        state.limiter->admit(decision.captured_event);
    }
    state.limiter->end_source_tick(state.emissions);
    for (size_t index = 0; index < state.emissions.size(); ++index) {
        const EventView& emitted = state.emissions[index];
        int64_t occurrence = emitted.event_index() >= 0 ? emitted.event_index() : static_cast<int64_t>(index);
        if (!last->try_admit(OutputWorkItem::captured_source(last->id, emitted.event_index(), emitted.micros,
                emitted_protection(emitted), occurrence, emitted)))
            models::throw_invalid_operation("The note-start generation exceeded its work bound.");
    }
    take_counts(state);
    return count;
}

bool OrderedNoteAttackAdapter::try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || !generations_.can_append_seek()) return false;
    complete_source(*last);
    int32_t nextId = next_generation_id_ + 1;
    std::unique_ptr<PlaybackGeneration> next = create_generation(nextId, targetMicroseconds, sourceEventCursor,
        chasePrefix, sourceClockAnchorTicks);
    if (!generations_.try_append_seek(OutputWorkItem::seek_marker(last->id, targetMicroseconds), next)) {
        states_.erase(nextId);
        return false;
    }
    next_generation_id_ = nextId;
    prune_retired_states();
    return true;
}

void OrderedNoteAttackAdapter::complete_tail_source()
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed())
        models::throw_invalid_operation("No note-start tail can complete.");
    complete_source(*last);
}

bool OrderedNoteAttackAdapter::try_begin_dispatch(OutputWorkItem& item)
{
    if (has_in_service_) {
        item = OutputWorkItem();
        return false;
    }
    if (!generations_.try_dequeue_dispatch(item)) return false;
    has_in_service_ = true;
    return true;
}

void OrderedNoteAttackAdapter::complete_dispatch()
{
    if (!has_in_service_) models::throw_invalid_operation("No note-start work is in service.");
    has_in_service_ = false;
}

void OrderedNoteAttackAdapter::complete_source(PlaybackGeneration& generation)
{
    GenerationState& state = state_of(generation);
    if (state.source_complete) return;
    state.limiter->complete_source();
    state.source_complete = true;
    take_counts(state);
}

void OrderedNoteAttackAdapter::take_counts(GenerationState& state)
{
    rejected_attacks_ += state.limiter->take_rejected_attacks();
    suppressed_releases_ += state.limiter->take_suppressed_releases();
}

std::unique_ptr<PlaybackGeneration> OrderedNoteAttackAdapter::create_generation(int32_t id, int64_t targetMicroseconds,
    int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks)
{
    std::vector<OutputWorkItem> prefix = chase_work(id, targetMicroseconds, chasePrefix);
    int32_t bound = bound_of(static_cast<int64_t>(events_.count()) + static_cast<int64_t>(prefix.size()) + 1);
    std::unique_ptr<GenerationState> state(new GenerationState(attacks_per_second_));
    std::unique_ptr<PlaybackGeneration> generation(new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
        sourceClockAnchorTicks, 0, &prefix, nullptr, bound));
    states_[id] = std::move(state);
    return generation;
}

void OrderedNoteAttackAdapter::prune_retired_states()
{
    for (std::map<int32_t, std::unique_ptr<GenerationState>>::iterator it = states_.begin(); it != states_.end();) {
        if (generations_.find_generation(it->first) == nullptr) it = states_.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------- output rate

OrderedOutputRateAdapter::OrderedOutputRateAdapter(models::EventSource events, int32_t maximumLiveGenerations,
    int64_t eventsPerSecond, int64_t clockUnitsPerSecond)
    : events_(events), generations_(maximumLiveGenerations),
      gate_(new models::OutputRateGate(eventsPerSecond, clockUnitsPerSecond))
{
}

OrderedOutputRateAdapter::GenerationState& OrderedOutputRateAdapter::state_of(const PlaybackGeneration& generation) const
{
    return *states_.find(generation.id)->second;
}

bool OrderedOutputRateAdapter::tail_source_complete() const
{
    PlaybackGeneration* last = tail();
    return last != nullptr && state_of(*last).source_complete;
}

void OrderedOutputRateAdapter::start(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks)
{
    std::unique_ptr<PlaybackGeneration> generation = create_generation(++next_generation_id_, targetMicroseconds,
        sourceEventCursor, chasePrefix, sourceClockAnchorTicks);
    generations_.start(std::move(generation));
}

int32_t OrderedOutputRateAdapter::admit_next_source_tick(int64_t decisionTimestamp, const CaptureDecision& captureDecision)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor >= events_.count())
        models::throw_invalid_operation("No output-rate source work is available.");
    GenerationState& state = state_of(*last);
    int64_t tick = events_.at(last->source_event_cursor).tick;
    gate_->begin_source_tick(tick);
    int32_t count = 0;
    while (last->source_event_cursor < events_.count() && events_.at(last->source_event_cursor).tick == tick) {
        EventView source = events_.at(last->source_event_cursor);
        models::OrderedFiniteCapturedDecision decision;
        if (captureDecision) decision = captureDecision(source);
        else {
            decision.captured_event = source;
            decision.source_filtered = false;
        }
        if (!decision.captured_event.is_valid() || decision.captured_event.event_index() != last->source_event_cursor)
            models::throw_invalid_operation("Captured output-rate work lost source identity.");
        last->source_event_cursor++;
        count++;
        CompleteNoteEventKind kind = CompleteNoteTracker::classify(decision.captured_event);
        if (decision.source_filtered) {
            if (kind == CompleteNoteEventKind::NoteOn)
                state.source_filters.record_filtered_note_on(decision.captured_event, decision.captured_event.event_index());
            else if (kind == CompleteNoteEventKind::NoteOff)
                state.source_filters.take_note_off(decision.captured_event);
            continue;
        }
        if (kind == CompleteNoteEventKind::NoteOff && state.source_filters.take_note_off(decision.captured_event).suppress)
            continue;
        gate_->admit(decision.captured_event);
    }
    gate_->end_source_tick(state.emissions, decisionTimestamp);
    for (size_t index = 0; index < state.emissions.size(); ++index) {
        const EventView& emitted = state.emissions[index];
        int64_t occurrence = emitted.event_index() >= 0 ? emitted.event_index() : static_cast<int64_t>(index);
        if (!last->try_admit(OutputWorkItem::captured_source(last->id, emitted.event_index(), emitted.micros,
                emitted_protection(emitted), occurrence, emitted)))
            models::throw_invalid_operation("The output-rate generation exceeded its work bound.");
    }
    take_counts();
    return count;
}

bool OrderedOutputRateAdapter::try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || !generations_.can_append_seek()) return false;
    complete_source(*last);
    int32_t nextId = next_generation_id_ + 1;
    std::unique_ptr<PlaybackGeneration> next = create_generation(nextId, targetMicroseconds, sourceEventCursor,
        chasePrefix, sourceClockAnchorTicks);
    if (!generations_.try_append_seek(OutputWorkItem::seek_marker(last->id, targetMicroseconds), next)) {
        states_.erase(nextId);
        return false;
    }
    next_generation_id_ = nextId;
    prune_retired_states();
    return true;
}

void OrderedOutputRateAdapter::complete_tail_source()
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed())
        models::throw_invalid_operation("No output-rate tail can complete.");
    complete_source(*last);
}

bool OrderedOutputRateAdapter::try_begin_dispatch(OutputWorkItem& item)
{
    if (has_in_service_) {
        item = OutputWorkItem();
        return false;
    }
    if (!generations_.try_dequeue_dispatch(item)) return false;
    has_in_service_ = true;
    return true;
}

void OrderedOutputRateAdapter::complete_dispatch()
{
    if (!has_in_service_) models::throw_invalid_operation("No output-rate work is in service.");
    has_in_service_ = false;
}

void OrderedOutputRateAdapter::complete_source(PlaybackGeneration& generation)
{
    GenerationState& state = state_of(generation);
    if (state.source_complete) return;
    gate_->complete_source();
    state.source_complete = true;
    take_counts();
}

void OrderedOutputRateAdapter::take_counts()
{
    rejected_attacks_ += gate_->take_rejected_attacks();
    suppressed_releases_ += gate_->take_suppressed_releases();
    rejected_other_messages_ += gate_->take_rejected_other_messages();
}

std::unique_ptr<PlaybackGeneration> OrderedOutputRateAdapter::create_generation(int32_t id, int64_t targetMicroseconds,
    int32_t sourceEventCursor, const std::vector<EventView>& chasePrefix, int64_t sourceClockAnchorTicks)
{
    std::vector<OutputWorkItem> prefix = chase_work(id, targetMicroseconds, chasePrefix);
    int32_t bound = bound_of(static_cast<int64_t>(events_.count()) + static_cast<int64_t>(prefix.size()) + 1);
    std::unique_ptr<GenerationState> state(new GenerationState());
    std::unique_ptr<PlaybackGeneration> generation(new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
        sourceClockAnchorTicks, 0, &prefix, nullptr, bound));
    states_[id] = std::move(state);
    return generation;
}

void OrderedOutputRateAdapter::prune_retired_states()
{
    for (std::map<int32_t, std::unique_ptr<GenerationState>>::iterator it = states_.begin(); it != states_.end();) {
        if (generations_.find_generation(it->first) == nullptr) it = states_.erase(it);
        else ++it;
    }
}

} // namespace engine
} // namespace midibn
