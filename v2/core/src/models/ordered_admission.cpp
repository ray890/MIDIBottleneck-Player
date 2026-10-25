#include "midibn/models/ordered_admission.h"

namespace midibn {
namespace models {

// ------------------------------------------------------------ work items

OutputWorkItem OutputWorkItem::make(int32_t generationId, OutputWorkItemKind kind, int32_t sourceEventIndex,
    int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId,
    const EventView& midiEvent, bool hasMidiEvent)
{
    if (generationId < 0) throw_argument_out_of_range("generationId");
    if (logicalOutputMicroseconds < 0) throw_argument_out_of_range("logicalOutputMicroseconds");
    OutputWorkItem item;
    item.generation_id = generationId;
    item.kind = kind;
    item.source_event_index = sourceEventIndex;
    item.logical_output_microseconds = logicalOutputMicroseconds;
    item.protection = protection;
    item.occurrence_id = occurrenceId;
    item.midi_event = midiEvent;
    item.has_midi_event = hasMidiEvent;
    return item;
}

OutputWorkItem OutputWorkItem::source(int32_t generationId, int32_t sourceEventIndex,
    int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId)
{
    if (sourceEventIndex < 0) throw_argument_out_of_range("sourceEventIndex");
    return make(generationId, OutputWorkItemKind::SourceMidi, sourceEventIndex, logicalOutputMicroseconds, protection,
        occurrenceId, EventView(), false);
}

OutputWorkItem OutputWorkItem::captured_source(int32_t generationId, int32_t sourceEventIndex,
    int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId, const EventView& midiEvent)
{
    if (sourceEventIndex < 0) throw_argument_out_of_range("sourceEventIndex");
    return make(generationId, OutputWorkItemKind::SourceMidi, sourceEventIndex, logicalOutputMicroseconds, protection,
        occurrenceId, midiEvent, true);
}

OutputWorkItem OutputWorkItem::projected_gate(int32_t generationId, int32_t sourceEventIndex,
    int64_t logicalOutputMicroseconds, OutputWorkProtection protection, int64_t occurrenceId, const EventView& midiEvent)
{
    return make(generationId, OutputWorkItemKind::ProjectedGateMidi, sourceEventIndex, logicalOutputMicroseconds,
        protection, occurrenceId, midiEvent, true);
}

OutputWorkItem OutputWorkItem::forced_or_chase(int32_t generationId, int64_t logicalOutputMicroseconds,
    OutputWorkProtection protection, const EventView& midiEvent)
{
    return make(generationId, OutputWorkItemKind::ForcedOrChaseMidi, -1, logicalOutputMicroseconds, protection, -1,
        midiEvent, true);
}

OutputWorkItem OutputWorkItem::seek_marker(int32_t generationId, int64_t logicalOutputMicroseconds)
{
    return make(generationId, OutputWorkItemKind::ProtectedSeekMarker, -1, logicalOutputMicroseconds,
        OutputWorkProtection::PreservationBarrier, -1, EventView(), false);
}

namespace {

bool is_ordinary(const OutputWorkItem& item)
{
    return item.protection == OutputWorkProtection::Ordinary || item.protection == OutputWorkProtection::OrdinaryNoteAttack;
}

// First later ProtectedRelease with the same occurrence id, or end().
std::list<OutputWorkItem>::iterator paired_release(std::list<OutputWorkItem>& admitted,
    std::list<OutputWorkItem>::iterator attack)
{
    std::list<OutputWorkItem>::iterator release = std::next(attack);
    while (release != admitted.end() &&
        (release->protection != OutputWorkProtection::ProtectedRelease || release->occurrence_id != attack->occurrence_id))
        ++release;
    return release;
}

} // namespace

// ------------------------------------------------------------ generation

PlaybackGeneration::PlaybackGeneration(int32_t idValue, int64_t targetSourceMicroseconds, int32_t sourceEventCursor,
    int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks, const std::vector<OutputWorkItem>* chasePrefix,
    std::unique_ptr<FiniteGenerationState> modeState, int32_t maximumAdmittedWork)
    : id(idValue), target_source_microseconds(targetSourceMicroseconds), source_event_cursor(sourceEventCursor),
      source_clock_anchor_ticks(sourceClockAnchorTicks), frozen_elapsed_ticks(frozenElapsedTicks),
      mode_state(std::move(modeState)), maximum_admitted_work_(maximumAdmittedWork)
{
    if (idValue < 0) throw_argument_out_of_range("id");
    if (targetSourceMicroseconds < 0) throw_argument_out_of_range("targetSourceMicroseconds");
    if (sourceEventCursor < 0) throw_argument_out_of_range("sourceEventCursor");
    if (maximumAdmittedWork < 1) throw_argument_out_of_range("maximumAdmittedWork");
    if (chasePrefix != nullptr) {
        for (const OutputWorkItem& item : *chasePrefix) {
            if (item.generation_id != idValue || item.kind != OutputWorkItemKind::ForcedOrChaseMidi)
                throw_argument("chasePrefix", "A chase prefix contains an invalid work item.");
            chase_prefix_.push_back(item);
        }
    }
    if (static_cast<int32_t>(chase_prefix_.size()) > maximumAdmittedWork)
        throw_argument("chasePrefix", "The chase prefix exceeds the admitted-work bound.");
    for (const OutputWorkItem& item : chase_prefix_) admitted_.push_back(item);
}

bool PlaybackGeneration::try_admit(const OutputWorkItem& item)
{
    if (admission_closed_ || item.generation_id != id || static_cast<int32_t>(admitted_.size()) >= maximum_admitted_work_)
        return false;
    if (item.kind == OutputWorkItemKind::ProtectedSeekMarker) return false;
    admitted_.push_back(item);
    return true;
}

bool PlaybackGeneration::try_evict_oldest_ordinary(OutputWorkItem& item)
{
    for (auto node = admitted_.begin(); node != admitted_.end(); ++node) {
        if (!is_ordinary(*node)) continue;
        item = *node;
        admitted_.erase(node);
        return true;
    }
    item = OutputWorkItem();
    return false;
}

bool PlaybackGeneration::try_evict_ordinary_at_head(OutputWorkItem& item)
{
    if (admitted_.empty() || !is_ordinary(admitted_.front())) {
        item = OutputWorkItem();
        return false;
    }
    item = admitted_.front();
    admitted_.pop_front();
    return true;
}

bool PlaybackGeneration::try_evict_oldest_complete_note(OutputWorkItem& attack, OutputWorkItem& release,
    bool& hasRelease)
{
    for (auto node = admitted_.begin(); node != admitted_.end(); ++node) {
        if (node->protection != OutputWorkProtection::OrdinaryNoteAttack) continue;
        auto pairedRelease = paired_release(admitted_, node);
        attack = *node;
        if (pairedRelease != admitted_.end()) {
            release = *pairedRelease;
            admitted_.erase(pairedRelease);
            hasRelease = true;
        }
        else {
            release = OutputWorkItem();
            hasRelease = false;
        }
        admitted_.erase(node);
        return true;
    }
    attack = OutputWorkItem();
    release = OutputWorkItem();
    hasRelease = false;
    return false;
}

bool PlaybackGeneration::try_evict_complete_note_at_head(OutputWorkItem& attack, OutputWorkItem& release,
    bool& hasRelease)
{
    if (admitted_.empty() || admitted_.front().protection != OutputWorkProtection::OrdinaryNoteAttack) {
        attack = OutputWorkItem();
        release = OutputWorkItem();
        hasRelease = false;
        return false;
    }
    auto head = admitted_.begin();
    auto pairedRelease = paired_release(admitted_, head);
    attack = *head;
    if (pairedRelease != admitted_.end()) {
        release = *pairedRelease;
        admitted_.erase(pairedRelease);
        hasRelease = true;
    }
    else {
        release = OutputWorkItem();
        hasRelease = false;
    }
    admitted_.pop_front();
    return true;
}

bool PlaybackGeneration::try_evict_least_important_complete_note(int32_t& fairnessCursor, OutputWorkItem& attack,
    OutputWorkItem& release, bool& hasRelease)
{
    if (musical_candidates_.size() < admitted_.size()) {
        size_t capacity = musical_candidates_.size();
        while (capacity < admitted_.size()) capacity *= 2;
        musical_candidates_.resize(capacity);
    }
    std::vector<std::list<OutputWorkItem>::iterator> attackNodes;
    std::vector<std::list<OutputWorkItem>::iterator> releaseNodes;
    int32_t candidateCount = 0;
    for (auto node = admitted_.begin(); node != admitted_.end(); ++node) {
        const OutputWorkItem& item = *node;
        if (item.protection != OutputWorkProtection::OrdinaryNoteAttack || !item.has_midi_event) continue;
        auto pairedRelease = paired_release(admitted_, node);
        int64_t duration = pairedRelease == admitted_.end() ? kInt64Max
            : max64(0, wrap_sub(pairedRelease->logical_output_microseconds, item.logical_output_microseconds));
        // Filled in place (DD-095): no 40-byte block copy into the array afterwards; every field is set below.
        MusicalNotePriorityCandidate& candidate = musical_candidates_[static_cast<size_t>(candidateCount)];
        candidate.pitch = item.midi_event.data_byte(1) & 0x7F;
        candidate.velocity = item.midi_event.data_byte(2) & 0x7F;
        candidate.track = item.midi_event.track;
        candidate.channel = item.midi_event.channel();
        candidate.source_order = item.source_event_index;
        candidate.intended_microseconds = item.logical_output_microseconds;
        candidate.duration_microseconds = duration;
        attackNodes.push_back(node);
        releaseNodes.push_back(pairedRelease);
        candidateCount++;
    }
    if (candidateCount == 0) {
        attack = OutputWorkItem();
        release = OutputWorkItem();
        hasRelease = false;
        return false;
    }
    int32_t victimIndex = musical_priority_.select_least_important(musical_candidates_.data(),
        static_cast<int32_t>(musical_candidates_.size()), candidateCount, fairnessCursor);
    auto victimAttack = attackNodes[static_cast<size_t>(victimIndex)];
    auto victimRelease = releaseNodes[static_cast<size_t>(victimIndex)];
    attack = *victimAttack;
    if (victimRelease != admitted_.end()) {
        release = *victimRelease;
        admitted_.erase(victimRelease);
        hasRelease = true;
    }
    else {
        release = OutputWorkItem();
        hasRelease = false;
    }
    admitted_.erase(victimAttack);
    fairnessCursor = musical_priority_.next_fairness_cursor(musical_candidates_[static_cast<size_t>(victimIndex)]);
    return true;
}

bool PlaybackGeneration::try_clear_open_admitted(std::vector<OutputWorkItem>& cleared)
{
    if (admission_closed_) return false;
    for (const OutputWorkItem& item : admitted_) cleared.push_back(item);
    admitted_.clear();
    return true;
}

void PlaybackGeneration::close_with_marker(const OutputWorkItem& marker)
{
    if (!can_append_marker()) throw_invalid_operation("Generation cannot accept its marker.");
    if (marker.generation_id != id || marker.kind != OutputWorkItemKind::ProtectedSeekMarker ||
        marker.protection != OutputWorkProtection::PreservationBarrier)
        throw_argument("marker", "The work item is not this generation's protected Seek marker.");
    admitted_.push_back(marker);
    admission_closed_ = true;
}

bool PlaybackGeneration::try_peek(OutputWorkItem& item) const
{
    if (admitted_.empty()) { item = OutputWorkItem(); return false; }
    item = admitted_.front();
    return true;
}

bool PlaybackGeneration::try_dequeue(OutputWorkItem& item)
{
    if (admitted_.empty()) { item = OutputWorkItem(); return false; }
    item = admitted_.front();
    admitted_.pop_front();
    return true;
}

// ------------------------------------------------------ generation queue

OrderedPlaybackGenerationQueue::OrderedPlaybackGenerationQueue(int32_t maximumLiveGenerations)
{
    if (maximumLiveGenerations < 1) throw_argument_out_of_range("maximumLiveGenerations");
    maximum_live_generations_ = maximumLiveGenerations;
}

int32_t OrderedPlaybackGenerationQueue::total_admitted_work_count() const
{
    int32_t count = 0;
    for (const auto& generation : generations_)
        count = count > kInt32Max - generation->admitted_work_count() ? kInt32Max : count + generation->admitted_work_count();
    return count;
}

bool OrderedPlaybackGenerationQueue::can_append_seek() const
{
    PlaybackGeneration* last = tail();
    return last != nullptr && live_generation_count() < maximum_live_generations_ && !last->is_admission_closed() &&
        last->can_append_marker();
}

void OrderedPlaybackGenerationQueue::start(std::unique_ptr<PlaybackGeneration> generation)
{
    if (!generation) throw_argument_null("generation");
    if (!generations_.empty()) throw_invalid_operation("The generation queue has already started.");
    generations_.push_back(std::move(generation));
}

bool OrderedPlaybackGenerationQueue::try_admit_to_tail(const OutputWorkItem& item)
{
    PlaybackGeneration* last = tail();
    return last != nullptr && last->try_admit(item);
}

bool OrderedPlaybackGenerationQueue::try_evict_oldest_ordinary_from_tail(OutputWorkItem& item)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed()) {
        item = OutputWorkItem();
        return false;
    }
    return last->try_evict_oldest_ordinary(item);
}

bool OrderedPlaybackGenerationQueue::try_evict_oldest_complete_note_from_tail(OutputWorkItem& attack,
    OutputWorkItem& release, bool& hasRelease)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed()) {
        attack = OutputWorkItem();
        release = OutputWorkItem();
        hasRelease = false;
        return false;
    }
    return last->try_evict_oldest_complete_note(attack, release, hasRelease);
}

bool OrderedPlaybackGenerationQueue::try_clear_open_tail(std::vector<OutputWorkItem>& cleared)
{
    PlaybackGeneration* last = tail();
    return last != nullptr && last->try_clear_open_admitted(cleared);
}

bool OrderedPlaybackGenerationQueue::open_tail_is_dispatch_head()
{
    retire_empty_closed_heads();
    PlaybackGeneration* first = head();
    PlaybackGeneration* last = tail();
    return first != nullptr && first == last && !last->is_admission_closed();
}

bool OrderedPlaybackGenerationQueue::open_tail_is_ready_for_immediate_source()
{
    return open_tail_is_dispatch_head() && tail()->admitted_work_count() == 0;
}

bool OrderedPlaybackGenerationQueue::try_evict_ordinary_at_open_head(OutputWorkItem& item)
{
    if (!open_tail_is_dispatch_head()) {
        item = OutputWorkItem();
        return false;
    }
    return tail()->try_evict_ordinary_at_head(item);
}

bool OrderedPlaybackGenerationQueue::try_evict_complete_note_at_open_head(OutputWorkItem& attack,
    OutputWorkItem& release, bool& hasRelease)
{
    if (!open_tail_is_dispatch_head()) {
        attack = OutputWorkItem();
        release = OutputWorkItem();
        hasRelease = false;
        return false;
    }
    return tail()->try_evict_complete_note_at_head(attack, release, hasRelease);
}

bool OrderedPlaybackGenerationQueue::try_evict_least_important_complete_note_from_tail(int32_t& fairnessCursor,
    OutputWorkItem& attack, OutputWorkItem& release, bool& hasRelease)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed()) {
        attack = OutputWorkItem();
        release = OutputWorkItem();
        hasRelease = false;
        return false;
    }
    return last->try_evict_least_important_complete_note(fairnessCursor, attack, release, hasRelease);
}

bool OrderedPlaybackGenerationQueue::try_append_seek(const OutputWorkItem& marker,
    std::unique_ptr<PlaybackGeneration>& nextGeneration)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || !nextGeneration || live_generation_count() >= maximum_live_generations_) return false;
    if (last->is_admission_closed() || !last->can_append_marker() || nextGeneration->id <= last->id ||
        marker.generation_id != last->id || marker.kind != OutputWorkItemKind::ProtectedSeekMarker ||
        marker.protection != OutputWorkProtection::PreservationBarrier)
        return false;
    last->close_with_marker(marker);
    generations_.push_back(std::move(nextGeneration));
    return true;
}

bool OrderedPlaybackGenerationQueue::try_peek_dispatch(OutputWorkItem& item)
{
    retire_empty_closed_heads();
    PlaybackGeneration* first = head();
    if (first == nullptr) { item = OutputWorkItem(); return false; }
    return first->try_peek(item);
}

bool OrderedPlaybackGenerationQueue::try_dequeue_dispatch(OutputWorkItem& item)
{
    retire_empty_closed_heads();
    PlaybackGeneration* first = head();
    if (first == nullptr) { item = OutputWorkItem(); return false; }
    if (!first->try_dequeue(item)) return false;
    retire_empty_closed_heads();
    return true;
}

PlaybackGeneration* OrderedPlaybackGenerationQueue::find_generation(int32_t generationId) const
{
    for (const auto& generation : generations_)
        if (generation->id == generationId) return generation.get();
    return nullptr;
}

void OrderedPlaybackGenerationQueue::shift_source_clock_anchors(int64_t deltaTicks)
{
    if (deltaTicks <= 0) return;
    for (const auto& generation : generations_) {
        int64_t anchor = generation->source_clock_anchor_ticks;
        generation->source_clock_anchor_ticks = anchor > kInt64Max - deltaTicks ? kInt64Max : anchor + deltaTicks;
    }
}

void OrderedPlaybackGenerationQueue::retire_empty_closed_heads()
{
    while (!generations_.empty() && generations_.front()->is_admission_closed() &&
        generations_.front()->admitted_work_count() == 0)
        generations_.pop_front();
}

// ------------------------------------------------------ admission core

OrderedFiniteAdmissionCore::OrderedFiniteAdmissionCore(EventSource events, int32_t maximumLiveGenerations)
    : events_(events), generations_(maximumLiveGenerations)
{
}

int32_t OrderedFiniteAdmissionCore::outstanding_count() const
{
    int32_t pending = pending_count();
    return has_in_service_ && pending < kInt32Max ? pending + 1 : pending;
}

bool OrderedFiniteAdmissionCore::is_at_or_above_capacity(int32_t capacity) const
{
    if (capacity < 1) throw_argument_out_of_range("capacity");
    return outstanding_count() >= capacity;
}

void OrderedFiniteAdmissionCore::start(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>* chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
{
    if (generations_.live_generation_count() != 0)
        throw_invalid_operation("The finite admission core has already started.");
    int32_t id = ++next_generation_id_;
    generations_.start(create_generation(id, targetMicroseconds, sourceEventCursor, chasePrefix,
        sourceClockAnchorTicks, frozenElapsedTicks));
}

OrderedFiniteAdmissionResult OrderedFiniteAdmissionCore::admit_next_source(int32_t capacity, OverflowPolicy policy)
{
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor >= events_.count())
        throw_invalid_operation("No tail source event is available for admission.");
    return admit_next_source(events_.at(last->source_event_cursor), false, capacity, policy);
}

OrderedFiniteAdmissionResult OrderedFiniteAdmissionCore::admit_next_source(const EventView& capturedEvent,
    bool sourceFiltered, int32_t capacity, OverflowPolicy policy)
{
    if (capacity < 1) throw_argument_out_of_range("capacity");
    if (policy != OverflowPolicy::DropNewest && policy != OverflowPolicy::DropOldest &&
        policy != OverflowPolicy::DropOldestCompleteNote && policy != OverflowPolicy::DropIncomingCompleteNotes &&
        policy != OverflowPolicy::DropBackgroundNotes && policy != OverflowPolicy::ClearBufferAndCatchUp)
        throw_not_supported("The finite admission policy is not supported.");
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed() || last->source_event_cursor >= events_.count())
        throw_invalid_operation("No tail source event is available for admission.");

    int32_t sourceIndex = last->source_event_cursor;
    const EventView& midiEvent = capturedEvent;
    if (!midiEvent.is_valid() || midiEvent.event_index() != sourceIndex)
        throw_argument("capturedEvent", "Captured source work does not match the tail cursor.");
    last->source_event_cursor++;
    FiniteGenerationState& state = *last->mode_state;
    CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(midiEvent);
    OrderedFiniteAdmissionResult result;
    if (sourceFiltered) {
        if (noteKind == CompleteNoteEventKind::NoteOn) state.complete_notes.record_filtered_note_on(midiEvent, sourceIndex);
        else if (noteKind == CompleteNoteEventKind::NoteOff) state.complete_notes.take_note_off(midiEvent);
        result.disposition = OrderedFiniteAdmissionDisposition::Filtered;
        return result;
    }
    CompleteNoteTracker::NoteOffMatch noteMatch;
    noteMatch.attack_index = 0;
    if (noteKind == CompleteNoteEventKind::NoteOff) {
        noteMatch = state.complete_notes.take_note_off(midiEvent);
        if (noteMatch.suppress) {
            result.disposition = noteMatch.filtered ? OrderedFiniteAdmissionDisposition::Filtered
                                                    : OrderedFiniteAdmissionDisposition::Rejected;
            result.paired_release_suppressed = true;
            return result;
        }
    }
    OutputWorkProtection protection = protection_for(midiEvent, noteKind);
    int64_t occurrenceId = noteKind == CompleteNoteEventKind::NoteOn ? sourceIndex
        : noteKind == CompleteNoteEventKind::NoteOff ? noteMatch.attack_index : sourceIndex;
    OutputWorkItem incoming = OutputWorkItem::captured_source(last->id, sourceIndex, midiEvent.micros, protection,
        occurrenceId, midiEvent);
    std::vector<OutputWorkItem>& evicted = result.evicted;
    bool ownAttackEvicted = false;

    if (policy == OverflowPolicy::DropOldest) {
        while (outstanding_count() >= capacity) {
            OutputWorkItem removed;
            if (!generations_.try_evict_oldest_ordinary_from_tail(removed)) break;
            evicted.push_back(removed);
        }
    }
    else if (policy == OverflowPolicy::DropOldestCompleteNote || policy == OverflowPolicy::DropBackgroundNotes) {
        while (outstanding_count() >= capacity) {
            OutputWorkItem attack, release;
            bool hasRelease;
            bool found = policy == OverflowPolicy::DropOldestCompleteNote
                ? generations_.try_evict_oldest_complete_note_from_tail(attack, release, hasRelease)
                : generations_.try_evict_least_important_complete_note_from_tail(state.musical_fairness_cursor,
                      attack, release, hasRelease);
            if (!found) break;
            evicted.push_back(attack);
            if (hasRelease) evicted.push_back(release);
            else state.complete_notes.mark_unsent_attack_evicted(attack.source_event_index);
            if (noteKind == CompleteNoteEventKind::NoteOff && noteMatch.attack_index == attack.source_event_index)
                ownAttackEvicted = true;
        }
        if (ownAttackEvicted) {
            result.disposition = OrderedFiniteAdmissionDisposition::Rejected;
            result.paired_release_suppressed = true;
            return result;
        }
    }

    if (outstanding_count() >= capacity) {
        if ((policy == OverflowPolicy::DropIncomingCompleteNotes || policy == OverflowPolicy::DropBackgroundNotes) &&
            noteKind != CompleteNoteEventKind::NoteOn) {
            admit_or_throw(incoming);
            result.disposition = OrderedFiniteAdmissionDisposition::AcceptedSoftLimit;
            return result;
        }
        if (protection == OutputWorkProtection::Ordinary || protection == OutputWorkProtection::OrdinaryNoteAttack) {
            if (noteKind == CompleteNoteEventKind::NoteOn)
                state.complete_notes.record_note_on(midiEvent, sourceIndex, false,
                    policy == OverflowPolicy::DropOldestCompleteNote || policy == OverflowPolicy::DropIncomingCompleteNotes ||
                        policy == OverflowPolicy::DropBackgroundNotes);
            result.disposition = OrderedFiniteAdmissionDisposition::Rejected;
            return result;
        }
        admit_or_throw(incoming);
        result.disposition = OrderedFiniteAdmissionDisposition::AcceptedSoftLimit;
        return result;
    }

    admit_or_throw(incoming);
    if (noteKind == CompleteNoteEventKind::NoteOn) state.complete_notes.record_note_on(midiEvent, sourceIndex, true, false);
    result.disposition = OrderedFiniteAdmissionDisposition::Accepted;
    return result;
}

bool OrderedFiniteAdmissionCore::try_append_seek(int64_t targetMicroseconds, int32_t sourceEventCursor,
    const std::vector<EventView>* chasePrefix, int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
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

OrderedFiniteClearResult OrderedFiniteAdmissionCore::clear_open_tail_and_catch_up(int64_t throughMicroseconds,
    const CaptureDecision& captureDecision)
{
    if (throughMicroseconds < 0) throw_argument_out_of_range("throughMicroseconds");
    PlaybackGeneration* last = tail();
    if (last == nullptr || last->is_admission_closed()) throw_invalid_operation("No open finite generation can catch up.");

    OrderedFiniteClearResult result;
    if (!generations_.try_clear_open_tail(result.cleared_work))
        throw_invalid_operation("The finite tail could not be cleared.");

    int32_t firstSkipped = last->source_event_cursor;
    int32_t catchUp = throughMicroseconds == kInt64Max ? events_.count() : events_.lower_bound_by_time(throughMicroseconds + 1);
    if (catchUp < firstSkipped) catchUp = firstSkipped;

    int32_t droppedSkipped = 0;
    int32_t filteredSkipped = 0;
    FiniteGenerationState& state = *last->mode_state;
    if (!captureDecision) droppedSkipped = catchUp - firstSkipped;
    else {
        for (int32_t sourceIndex = firstSkipped; sourceIndex < catchUp; sourceIndex++) {
            OrderedFiniteCapturedDecision decision = captureDecision(events_.at(sourceIndex));
            const EventView& captured = decision.captured_event;
            if (!captured.is_valid() || captured.event_index() != sourceIndex)
                throw_invalid_operation("A catch-up source decision did not preserve source identity.");
            if (!decision.source_filtered) {
                droppedSkipped++;
                continue;
            }
            filteredSkipped++;
            CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(captured);
            if (noteKind == CompleteNoteEventKind::NoteOn) state.complete_notes.record_filtered_note_on(captured, sourceIndex);
            else if (noteKind == CompleteNoteEventKind::NoteOff) state.complete_notes.take_note_off(captured);
        }
    }
    last->source_event_cursor = catchUp;
    result.first_skipped_source_event_index = firstSkipped;
    result.source_event_cursor_after = catchUp;
    result.dropped_skipped_source_events = droppedSkipped;
    result.filtered_skipped_source_events = filteredSkipped;
    return result;
}

OrderedFinitePressureSnapshot OrderedFiniteAdmissionCore::measure_due_pressure(int64_t logicalNowMicroseconds,
    const CaptureDecision& captureDecision)
{
    if (logicalNowMicroseconds < 0) throw_argument_out_of_range("logicalNowMicroseconds");
    int32_t admitted = outstanding_count();
    int64_t oldest = kInt64Max;
    if (has_in_service_) oldest = in_service_.logical_output_microseconds;
    OutputWorkItem pendingHead;
    if (generations_.try_peek_dispatch(pendingHead)) oldest = min64(oldest, pendingHead.logical_output_microseconds);

    int32_t externalDue = 0;
    PlaybackGeneration* last = tail();
    if (last != nullptr && !last->is_admission_closed()) {
        int32_t dueEnd = logicalNowMicroseconds == kInt64Max ? events_.count()
                                                             : events_.lower_bound_by_time(logicalNowMicroseconds + 1);
        int32_t first = last->source_event_cursor > 0 ? last->source_event_cursor : 0;
        for (int32_t sourceIndex = first; sourceIndex < dueEnd; sourceIndex++) {
            EventView sourceEvent = events_.at(sourceIndex);
            if (captureDecision) {
                OrderedFiniteCapturedDecision decision = captureDecision(sourceEvent);
                if (!decision.captured_event.is_valid() || decision.captured_event.event_index() != sourceIndex)
                    throw_invalid_operation("A pressure source decision did not preserve source identity.");
                if (decision.source_filtered) continue;
                sourceEvent = decision.captured_event;
            }
            if (externalDue < kInt32Max) externalDue++;
            oldest = min64(oldest, sourceEvent.micros);
        }
    }
    int64_t age = oldest == kInt64Max ? 0 : max64(0, wrap_sub(logicalNowMicroseconds, oldest));
    OrderedFinitePressureSnapshot snapshot;
    snapshot.admitted_outstanding_count = admitted;
    snapshot.external_due_count = externalDue;
    snapshot.total_due_count = admitted > kInt32Max - externalDue ? kInt32Max : admitted + externalDue;
    snapshot.oldest_due_microseconds = oldest;
    snapshot.current_age_microseconds = age;
    return snapshot;
}

OrderedFiniteAgeRepairResult OrderedFiniteAdmissionCore::repair_waiting_time_pressure(int64_t logicalNowMicroseconds,
    int64_t ageLimitMicroseconds, OverflowPolicy policy, const CaptureDecision& captureDecision)
{
    if (logicalNowMicroseconds < 0) throw_argument_out_of_range("logicalNowMicroseconds");
    if (ageLimitMicroseconds < 0) throw_argument_out_of_range("ageLimitMicroseconds");
    if (policy != OverflowPolicy::DropOldest && policy != OverflowPolicy::DropOldestCompleteNote &&
        policy != OverflowPolicy::ClearBufferAndCatchUp)
        throw_not_supported("Waiting-time pressure requires an oldest-removal or clear policy.");

    OrderedFiniteAgeRepairResult result;
    bool stalled = false;
    while (admitted_age(logicalNowMicroseconds) > ageLimitMicroseconds) {
        if (has_in_service_) {
            stalled = true;
            break;
        }
        if (policy == OverflowPolicy::ClearBufferAndCatchUp) {
            if (!generations_.open_tail_is_dispatch_head()) {
                stalled = true;
                break;
            }
            result.clear_result = clear_open_tail_and_catch_up(logicalNowMicroseconds, captureDecision);
            result.has_clear_result = true;
            break;
        }
        if (policy == OverflowPolicy::DropOldest) {
            OutputWorkItem removed;
            if (!generations_.try_evict_ordinary_at_open_head(removed)) {
                stalled = true;
                break;
            }
            result.evicted.push_back(removed);
            continue;
        }
        OutputWorkItem attack, release;
        bool hasRelease;
        if (!generations_.try_evict_complete_note_at_open_head(attack, release, hasRelease)) {
            stalled = true;
            break;
        }
        result.evicted.push_back(attack);
        if (hasRelease) result.evicted.push_back(release);
        else tail()->mode_state->complete_notes.mark_unsent_attack_evicted(attack.source_event_index);
    }
    result.stalled_by_in_service_or_preservation_barrier = stalled;
    result.age_after_microseconds = admitted_age(logicalNowMicroseconds);
    return result;
}

int64_t OrderedFiniteAdmissionCore::admitted_age(int64_t logicalNowMicroseconds)
{
    int64_t oldest = kInt64Max;
    if (has_in_service_) oldest = in_service_.logical_output_microseconds;
    OutputWorkItem pending;
    if (generations_.try_peek_dispatch(pending)) oldest = min64(oldest, pending.logical_output_microseconds);
    return oldest == kInt64Max ? 0 : max64(0, wrap_sub(logicalNowMicroseconds, oldest));
}

bool OrderedFiniteAdmissionCore::try_begin_dispatch(OutputWorkItem& item)
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

void OrderedFiniteAdmissionCore::complete_dispatch()
{
    if (!has_in_service_) throw_invalid_operation("No ordered finite work item is in service.");
    in_service_ = OutputWorkItem();
    has_in_service_ = false;
}

void OrderedFiniteAdmissionCore::admit_or_throw(const OutputWorkItem& item)
{
    if (!generations_.try_admit_to_tail(item))
        throw_invalid_operation("The ordered finite tail rejected bounded source work.");
}

std::unique_ptr<PlaybackGeneration> OrderedFiniteAdmissionCore::create_generation(int32_t id,
    int64_t targetMicroseconds, int32_t sourceEventCursor, const std::vector<EventView>* chasePrefix,
    int64_t sourceClockAnchorTicks, int64_t frozenElapsedTicks)
{
    if (sourceEventCursor < 0 || sourceEventCursor > events_.count()) throw_argument_out_of_range("sourceEventCursor");
    std::vector<OutputWorkItem> chaseWork;
    if (chasePrefix != nullptr)
        for (const EventView& chase : *chasePrefix)
            chaseWork.push_back(OutputWorkItem::forced_or_chase(id, targetMicroseconds,
                OutputWorkProtection::SafetyTraffic, chase));
    int64_t requestedBound = static_cast<int64_t>(events_.count()) + static_cast<int64_t>(chaseWork.size()) + 1;
    int32_t workBound = requestedBound > kInt32Max ? kInt32Max
        : (requestedBound < 1 ? 1 : static_cast<int32_t>(requestedBound));
    return std::unique_ptr<PlaybackGeneration>(new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
        sourceClockAnchorTicks, frozenElapsedTicks, &chaseWork, std::unique_ptr<FiniteGenerationState>(
            new FiniteGenerationState()), workBound));
}

OutputWorkProtection OrderedFiniteAdmissionCore::protection_for(const EventView& midiEvent,
    CompleteNoteEventKind noteKind)
{
    if (noteKind == CompleteNoteEventKind::NoteOn) return OutputWorkProtection::OrdinaryNoteAttack;
    if (noteKind == CompleteNoteEventKind::NoteOff) return OutputWorkProtection::ProtectedRelease;
    if (CompleteNoteTracker::is_safety_control(midiEvent)) return OutputWorkProtection::SafetyTraffic;
    return OutputWorkProtection::Ordinary;
}

} // namespace models
} // namespace midibn
