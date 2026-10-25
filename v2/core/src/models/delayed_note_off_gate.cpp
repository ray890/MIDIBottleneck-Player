#include "midibn/models/delayed_note_off_gate.h"
#include "midibn/models/per_note_gate.h"

#include <algorithm>

namespace midibn {
namespace models {

DelayedNoteOffGate::DelayedNoteOffGate(int64_t minimumActiveMicroseconds, bool intervalPolyphonyLimitEnabled,
    int32_t intervalPolyphonyLimit)
{
    if (minimumActiveMicroseconds < 0) throw_argument_out_of_range("minimumActiveMicroseconds");
    if (intervalPolyphonyLimit < 1 || intervalPolyphonyLimit > kPitchCount)
        throw_argument_out_of_range("intervalPolyphonyLimit");
    minimum_active_microseconds_ = minimumActiveMicroseconds;
    interval_polyphony_limit_enabled_ = intervalPolyphonyLimitEnabled;
    interval_polyphony_limit_ = intervalPolyphonyLimit;
    recovery_microseconds_ = min64(minimumActiveMicroseconds / 4, kMaximumRecoveryMicroseconds);
    active_head_.fill(-1);
    active_tail_.fill(-1);
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        owner_occurrence_[pitch] = -1;
        candidate_occurrence_[pitch] = -1;
        attack_due_[pitch] = release_due_[pitch] = recovery_due_[pitch] = kInt64Max;
    }
    due_transitions_.reserve(kPitchCount);
}

int32_t DelayedNoteOffGate::next_boundary_output_count() const
{
    if (next_boundary_ == kInt64Max) return 0;
    int32_t count = 0;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        if (state_[pitch] == kAttackPending && attack_due_[pitch] == next_boundary_) count++;
        else if (state_[pitch] == kHeld && release_pending_[pitch] && release_due_[pitch] == next_boundary_) count++;
        else if (state_[pitch] == kRecovery && recovery_due_[pitch] == next_boundary_ && candidate_occurrence_[pitch] >= 0)
            count++;
    }
    return interval_polyphony_limit_enabled_ ? std::min(count, interval_polyphony_limit_) : count;
}

int64_t DelayedNoteOffGate::take_filtered_event_count()
{
    int64_t result = filtered_events_;
    filtered_events_ = 0;
    return result;
}

bool DelayedNoteOffGate::source_precedes_boundary(const EventView& midiEvent, int64_t effectiveBoundaryMicroseconds) const
{
    if (next_boundary_ == kInt64Max || midiEvent.micros != effectiveBoundaryMicroseconds) return false;
    int32_t pitch;
    bool noteOn;
    if (PerNoteIntervalGate::try_classify(midiEvent, pitch, noteOn))
        return interval_polyphony_limit_enabled_ ||
            (state_[static_cast<size_t>(pitch)] == kRecovery &&
                recovery_due_[static_cast<size_t>(pitch)] <= effectiveBoundaryMicroseconds);
    if (!is_sustain_controller(midiEvent)) return false;
    int channel = midiEvent.channel();
    for (pitch = 0; pitch < kPitchCount; pitch++) {
        if (state_[static_cast<size_t>(pitch)] != kRecovery ||
            recovery_due_[static_cast<size_t>(pitch)] > effectiveBoundaryMicroseconds) continue;
        int32_t candidate = candidate_occurrence_[static_cast<size_t>(pitch)];
        if (candidate >= 0 && occurrence(candidate).channel == channel) return true;
    }
    return false;
}

DelayedNoteOffAdmission DelayedNoteOffGate::admit(const EventView& midiEvent)
{
    int32_t pitch;
    bool noteOn;
    if (!PerNoteIntervalGate::try_classify(midiEvent, pitch, noteOn)) {
        observe_sustain(midiEvent);
        recalculate_next_boundary();
        return DelayedNoteOffAdmission::NotNote;
    }
    int64_t order = source_order_++;
    if (noteOn) admit_attack(pitch, midiEvent, order);
    else admit_release(pitch, midiEvent, order);
    recalculate_next_boundary();
    return DelayedNoteOffAdmission::Accepted;
}

int32_t DelayedNoteOffGate::emit_boundary(int64_t boundaryMicroseconds, std::vector<EventView>& emissions)
{
    emissions.clear();
    collect_due_transitions(boundaryMicroseconds);
    if (!interval_polyphony_limit_enabled_) {
        std::sort(due_transitions_.begin(), due_transitions_.end(), compare_legacy_transitions);
        for (const DueTransition& transition : due_transitions_) {
            if (transition.kind == kTransitionAttack) emit_attack(transition.pitch, boundaryMicroseconds, emissions);
            else if (transition.kind == kTransitionRelease) emit_release(transition.pitch, boundaryMicroseconds, emissions);
            else emit_recovered_attack(transition.pitch, boundaryMicroseconds, emissions);
        }
        recalculate_next_boundary();
        return static_cast<int32_t>(emissions.size());
    }
    std::sort(due_transitions_.begin(), due_transitions_.end(), compare_transitions);
    int32_t emittedReleases = 0;
    int32_t attackCount = 0;
    for (const DueTransition& transition : due_transitions_) {
        size_t p = static_cast<size_t>(transition.pitch);
        selected_attack_pitch_[p] = false;
        if (transition.kind == kTransitionRelease) {
            if (emittedReleases < interval_polyphony_limit_) {
                emit_release(transition.pitch, boundaryMicroseconds, emissions);
                emittedReleases++;
            }
            else release_due_[p] = add_saturated(boundaryMicroseconds, minimum_active_microseconds_);
        }
        else if (transition.kind == kTransitionRecovery && candidate_occurrence_[p] < 0)
            emit_recovered_attack(transition.pitch, boundaryMicroseconds, emissions);
        else attack_transitions_[static_cast<size_t>(attackCount++)] = transition;
    }

    int32_t attackCapacity = interval_polyphony_limit_ - static_cast<int32_t>(emissions.size());
    while (attackCount > attackCapacity) {
        for (int32_t index = 0; index < attackCount; index++) {
            const DueTransition& transition = attack_transitions_[static_cast<size_t>(index)];
            EventView attack = attack_for_transition(transition);
            // Filled in place (DD-095): no 40-byte block copy into the array afterwards; every field is set below.
            MusicalNotePriorityCandidate& candidate = musical_candidates_[static_cast<size_t>(index)];
            candidate.pitch = transition.pitch;
            candidate.velocity = attack.data_byte(2) & 0x7F;
            candidate.track = attack.track;
            candidate.channel = attack.channel();
            candidate.source_order = attack.event_index();
            candidate.intended_microseconds = boundaryMicroseconds;
            candidate.duration_microseconds = duration_for_transition(transition);
        }
        int32_t victimIndex = musical_priority_.select_least_important(musical_candidates_.data(), kPitchCount,
            attackCount, musical_fairness_cursor_);
        musical_fairness_cursor_ = musical_priority_.next_fairness_cursor(musical_candidates_[static_cast<size_t>(victimIndex)]);
        reject_due_attack(attack_transitions_[static_cast<size_t>(victimIndex)]);
        attackCount--;
        attack_transitions_[static_cast<size_t>(victimIndex)] = attack_transitions_[static_cast<size_t>(attackCount)];
    }
    for (int32_t index = 0; index < attackCount; index++)
        selected_attack_pitch_[static_cast<size_t>(attack_transitions_[static_cast<size_t>(index)].pitch)] = true;
    for (const DueTransition& transition : due_transitions_) {
        if (!selected_attack_pitch_[static_cast<size_t>(transition.pitch)]) continue;
        if (transition.kind == kTransitionAttack) emit_attack(transition.pitch, boundaryMicroseconds, emissions);
        else if (transition.kind == kTransitionRecovery) emit_recovered_attack(transition.pitch, boundaryMicroseconds, emissions);
    }
    recalculate_next_boundary();
    return static_cast<int32_t>(emissions.size());
}

EventView DelayedNoteOffGate::attack_for_transition(const DueTransition& transition) const
{
    if (transition.kind == kTransitionAttack) return attack_[static_cast<size_t>(transition.pitch)];
    int32_t candidate = candidate_occurrence_[static_cast<size_t>(transition.pitch)];
    return candidate < 0 ? EventView() : occurrence(candidate).note_on;
}

int64_t DelayedNoteOffGate::duration_for_transition(const DueTransition& transition) const
{
    size_t p = static_cast<size_t>(transition.pitch);
    int32_t node = transition.kind == kTransitionAttack ? owner_occurrence_[p] : candidate_occurrence_[p];
    if (node < 0) return kInt64Max;
    const SourceOccurrence& current = occurrence(node);
    return current.note_off.is_valid() ? max64(0, wrap_sub(current.note_off.micros, current.note_on.micros)) : kInt64Max;
}

void DelayedNoteOffGate::reject_due_attack(const DueTransition& transition)
{
    size_t p = static_cast<size_t>(transition.pitch);
    if (transition.kind == kTransitionRecovery) {
        int32_t candidate = candidate_occurrence_[p];
        candidate_occurrence_[p] = -1;
        recovery_due_[p] = kInt64Max;
        if (candidate >= 0) reject_recovery_candidate(candidate, kRecoveryLost);
        state_[p] = kIdle;
        return;
    }

    int32_t owner = owner_occurrence_[p];
    owner_occurrence_[p] = -1;
    if (owner >= 0) {
        SourceOccurrence copy = occurrence(owner);
        copy.disposition = kRecoveryLost;
        count_filtered(kRecoveryLost, 1);
        if (copy.key_released) {
            count_filtered(kRecoveryLost, 1);
            remove_and_free(owner);
        }
        else occurrence(owner) = copy;
    }
    attack_[p] = EventView();
    attack_due_[p] = kInt64Max;
    release_pending_[p] = false;
    release_for_retrigger_[p] = false;
    release_[p] = EventView();
    release_due_[p] = kInt64Max;
    state_[p] = kIdle;
}

void DelayedNoteOffGate::complete_source(int64_t sourceEndMicroseconds)
{
    if (source_complete_) return;
    source_complete_ = true;
    sourceEndMicroseconds = max64(0, sourceEndMicroseconds);
    for (size_t slot = 0; slot < active_head_.size(); slot++) {
        int32_t node = active_head_[slot];
        while (node >= 0) {
            SourceOccurrence copy = occurrence(node);
            int32_t next = copy.next_active;
            if (copy.disposition == kOwner) {
                copy.key_released = true;
                copy.sustain_held = false;
                occurrence(node) = copy;
                schedule_owner_release(copy.pitch, EventView(), sourceEndMicroseconds, false);
            }
            else if (copy.disposition == kRecoveryCandidate) {
                if (candidate_occurrence_[static_cast<size_t>(copy.pitch)] == node)
                    candidate_occurrence_[static_cast<size_t>(copy.pitch)] = -1;
                count_filtered(kRecoveryEnded, 1);
                remove_and_free(node);
            }
            else remove_and_free(node);
            node = next;
        }
    }
    recalculate_next_boundary();
}

int32_t DelayedNoteOffGate::retire_channel(int32_t channel, int64_t)
{
    if (channel < 0 || channel >= kChannelCount) return 0;
    sustain_down_[static_cast<size_t>(channel)] = false;
    int32_t routed = 0;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        int32_t node = active_head_[static_cast<size_t>(match_slot(channel, pitch))];
        while (node >= 0) {
            const SourceOccurrence& current = occurrence(node);
            int32_t next = current.next_active;
            if (current.disposition == kRecoveryCandidate && candidate_occurrence_[pitch] == node) {
                candidate_occurrence_[pitch] = -1;
                routed++;
            }
            if (current.disposition == kOwner && owner_occurrence_[pitch] == node) {
                owner_occurrence_[pitch] = -1;
                state_[pitch] = kIdle;
                release_pending_[pitch] = false;
                attack_due_[pitch] = release_due_[pitch] = recovery_due_[pitch] = kInt64Max;
            }
            remove_and_free(node);
            node = next;
        }
        // Recovery timing is global per pitch; its deadline stays even when
        // this channel's candidate left.
    }
    recalculate_next_boundary();
    return routed;
}

bool DelayedNoteOffGate::is_note_terminating_control(const EventView& midiEvent)
{
    if (!midiEvent.is_valid() || midiEvent.channel() < 0 || midiEvent.channel() >= kChannelCount ||
        midiEvent.data_length < 2 || (midiEvent.status() & 0xF0) != 0xB0) return false;
    int controller = midiEvent.data_byte(1) & 0x7F;
    return controller == 120 || (controller >= 123 && controller <= 127);
}

void DelayedNoteOffGate::admit_attack(int32_t pitch, const EventView& midiEvent, int64_t order)
{
    size_t p = static_cast<size_t>(pitch);
    if (state_[p] == kIdle) {
        int32_t owner = allocate_occurrence(midiEvent, pitch, kOwner, order);
        owner_occurrence_[p] = owner;
        attack_[p] = midiEvent;
        attack_due_[p] = midiEvent.micros;
        attack_order_[p] = order;
        state_[p] = kAttackPending;
        return;
    }
    // Later source attacks compete for the pending attack's one owner.
    if (state_[p] == kAttackPending) {
        select_pending_attack_owner(pitch, midiEvent, order);
        return;
    }
    int64_t lastAttack = attack_emission_[p];
    if (state_[p] == kHeld && midiEvent.micros < add_saturated(lastAttack, minimum_active_microseconds_)) {
        allocate_occurrence(midiEvent, pitch, kInsideDuration, order);
        count_filtered(kInsideDuration, 1);
        return;
    }
    // A matured same-pitch attack is the batch's eligible retrigger.
    if (state_[p] == kHeld) {
        int32_t retrigger = allocate_occurrence(midiEvent, pitch, kRecoveryCandidate, order);
        select_recovery_candidate(pitch, retrigger);
        schedule_owner_release(pitch, EventView(), midiEvent.micros, true);
        return;
    }
    // Recovery owns one bounded candidate slot for the pitch.
    int32_t candidate = allocate_occurrence(midiEvent, pitch, kRecoveryCandidate, order);
    select_recovery_candidate(pitch, candidate);
}

void DelayedNoteOffGate::select_pending_attack_owner(int32_t pitch, const EventView& midiEvent, int64_t order)
{
    size_t p = static_cast<size_t>(pitch);
    int32_t current = owner_occurrence_[p];
    if (current < 0) throw_invalid_operation("A pending Delayed NoteOff attack has no owner occurrence.");
    SourceOccurrence existing = occurrence(current);
    if (!PerNoteIntervalGate::is_better_candidate(midiEvent, existing.note_on)) {
        allocate_occurrence(midiEvent, pitch, kRecoveryLost, order);
        count_filtered(kRecoveryLost, 1);
        return;
    }

    existing.disposition = kRecoveryLost;
    count_filtered(kRecoveryLost, 1);
    if (existing.key_released) {
        count_filtered(kRecoveryLost, 1);
        remove_and_free(current);
    }
    else occurrence(current) = existing;

    int32_t replacement = allocate_occurrence(midiEvent, pitch, kOwner, order);
    owner_occurrence_[p] = replacement;
    attack_[p] = midiEvent;
    attack_order_[p] = order;
    release_pending_[p] = false;
    release_for_retrigger_[p] = false;
    release_[p] = EventView();
    release_due_[p] = kInt64Max;
}

void DelayedNoteOffGate::admit_release(int32_t pitch, const EventView& midiEvent, int64_t)
{
    int32_t node = match_occurrence(midiEvent.track, midiEvent.channel(), pitch);
    if (node < 0) return;
    SourceOccurrence& current = occurrence(node);
    current.key_released = true;
    current.note_off = midiEvent;

    uint8_t disposition = current.disposition;
    if (disposition == kInsideDuration || disposition == kRecoveryLost || disposition == kRecoveryEnded) {
        count_filtered(disposition, 1);
        remove_and_free(node);
        return;
    }
    if (disposition == kDisplacedOwner) {
        remove_and_free(node);
        return;
    }
    if (sustain_down_[static_cast<size_t>(current.channel)]) {
        current.sustain_held = true;
        return;
    }
    resolve_supported_release(node, midiEvent.micros);
}

void DelayedNoteOffGate::resolve_supported_release(int32_t node, int64_t releaseTime)
{
    SourceOccurrence& current = occurrence(node);
    current.sustain_held = false;
    int32_t pitch = current.pitch;
    if (current.disposition == kRecoveryCandidate) {
        if (candidate_occurrence_[static_cast<size_t>(pitch)] == node) candidate_occurrence_[static_cast<size_t>(pitch)] = -1;
        count_filtered(kRecoveryEnded, 2);
        remove_and_free(node);
        return;
    }
    if (current.disposition == kOwner && owner_occurrence_[static_cast<size_t>(pitch)] == node) {
        EventView noteOff = current.note_off;
        schedule_owner_release(pitch, noteOff, releaseTime, false);
        return;
    }
    remove_and_free(node);
}

void DelayedNoteOffGate::select_recovery_candidate(int32_t pitch, int32_t candidate)
{
    size_t p = static_cast<size_t>(pitch);
    int32_t current = candidate_occurrence_[p];
    if (current < 0) {
        candidate_occurrence_[p] = candidate;
        return;
    }
    if (PerNoteIntervalGate::is_better_candidate(occurrence(candidate).note_on, occurrence(current).note_on)) {
        reject_recovery_candidate(current, kRecoveryLost);
        candidate_occurrence_[p] = candidate;
    }
    else reject_recovery_candidate(candidate, kRecoveryLost);
}

void DelayedNoteOffGate::reject_recovery_candidate(int32_t node, uint8_t reason)
{
    SourceOccurrence copy = occurrence(node);
    copy.disposition = reason;
    count_filtered(reason, 1);
    if (copy.key_released) {
        count_filtered(reason, 1);
        remove_and_free(node);
    }
    else occurrence(node) = copy;
}

void DelayedNoteOffGate::schedule_owner_release(int32_t pitch, const EventView& source, int64_t requested, bool retrigger)
{
    size_t p = static_cast<size_t>(pitch);
    if (state_[p] != kHeld && state_[p] != kAttackPending) return;
    int64_t due = max64(max64(0, requested),
        add_saturated(state_[p] == kAttackPending ? attack_due_[p] : attack_emission_[p], minimum_active_microseconds_));
    if (!release_pending_[p] || due < release_due_[p] || retrigger) {
        release_pending_[p] = true;
        release_for_retrigger_[p] = retrigger;
        release_[p] = source;
        release_due_[p] = retrigger ? max64(0, requested) : due;
        release_order_[p] = source_order_++;
    }
}

void DelayedNoteOffGate::emit_attack(int32_t pitch, int64_t boundary, std::vector<EventView>& emissions)
{
    size_t p = static_cast<size_t>(pitch);
    if (state_[p] != kAttackPending || attack_due_[p] > boundary) return;
    emissions.push_back(attack_[p]);
    attack_emission_[p] = boundary;
    attack_due_[p] = kInt64Max;
    state_[p] = kHeld;
    if (release_pending_[p] && !release_for_retrigger_[p])
        release_due_[p] = max64(release_due_[p], add_saturated(boundary, minimum_active_microseconds_));
}

void DelayedNoteOffGate::emit_release(int32_t pitch, int64_t boundary, std::vector<EventView>& emissions)
{
    size_t p = static_cast<size_t>(pitch);
    if (state_[p] != kHeld || !release_pending_[p] || release_due_[p] > boundary) return;
    int32_t owner = owner_occurrence_[p];
    int32_t channel = owner >= 0 ? occurrence(owner).channel : attack_[p].channel();
    emissions.push_back(create_release(release_[p], pitch, channel, boundary));
    bool retrigger = release_for_retrigger_[p];
    release_pending_[p] = false;
    release_for_retrigger_[p] = false;
    release_due_[p] = kInt64Max;
    if (owner >= 0) {
        owner_occurrence_[p] = -1;
        if (retrigger && !occurrence(owner).key_released) occurrence(owner).disposition = kDisplacedOwner;
        else remove_and_free(owner);
    }
    state_[p] = kRecovery;
    recovery_due_[p] = add_saturated(boundary, recovery_microseconds_);
    recovery_order_[p] = release_order_[p];
}

void DelayedNoteOffGate::emit_recovered_attack(int32_t pitch, int64_t boundary, std::vector<EventView>& emissions)
{
    size_t p = static_cast<size_t>(pitch);
    if (state_[p] != kRecovery || recovery_due_[p] > boundary) return;
    int32_t candidate = candidate_occurrence_[p];
    candidate_occurrence_[p] = -1;
    recovery_due_[p] = kInt64Max;
    if (candidate < 0) {
        state_[p] = kIdle;
        return;
    }
    SourceOccurrence& current = occurrence(candidate);
    if (current.key_released && !current.sustain_held) {
        count_filtered(kRecoveryEnded, 2);
        remove_and_free(candidate);
        state_[p] = kIdle;
        return;
    }
    current.disposition = kOwner;
    owner_occurrence_[p] = candidate;
    attack_[p] = current.note_on;
    attack_emission_[p] = boundary;
    emissions.push_back(current.note_on);
    state_[p] = kHeld;
    if (current.key_released) {
        EventView noteOff = current.note_off;
        schedule_owner_release(pitch, noteOff, boundary, false);
    }
}

void DelayedNoteOffGate::collect_due_transitions(int64_t boundary)
{
    due_transitions_.clear();
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        DueTransition transition;
        transition.pitch = pitch;
        if (state_[pitch] == kAttackPending && attack_due_[pitch] <= boundary) {
            transition.kind = kTransitionAttack;
            transition.order = attack_order_[pitch];
        }
        else if (state_[pitch] == kHeld && release_pending_[pitch] && release_due_[pitch] <= boundary) {
            transition.kind = kTransitionRelease;
            transition.order = release_order_[pitch];
        }
        else if (state_[pitch] == kRecovery && recovery_due_[pitch] <= boundary) {
            transition.kind = kTransitionRecovery;
            transition.order = candidate_order(pitch);
        }
        else continue;
        due_transitions_.push_back(transition);
    }
}

int64_t DelayedNoteOffGate::candidate_order(int32_t pitch) const
{
    int32_t candidate = candidate_occurrence_[static_cast<size_t>(pitch)];
    return candidate >= 0 ? occurrence(candidate).order : recovery_order_[static_cast<size_t>(pitch)];
}

void DelayedNoteOffGate::observe_sustain(const EventView& midiEvent)
{
    if (!is_sustain_controller(midiEvent)) return;
    int channel = midiEvent.channel();
    bool down = (midiEvent.data_byte(2) & 0x7F) >= 64;
    if (sustain_down_[static_cast<size_t>(channel)] == down) return;
    sustain_down_[static_cast<size_t>(channel)] = down;
    if (down) return;
    int64_t time = midiEvent.micros;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        int32_t node = active_head_[static_cast<size_t>(match_slot(channel, pitch))];
        while (node >= 0) {
            const SourceOccurrence& current = occurrence(node);
            int32_t next = current.next_active;
            if (current.key_released && current.sustain_held) resolve_supported_release(node, time);
            node = next;
        }
    }
}

bool DelayedNoteOffGate::is_sustain_controller(const EventView& midiEvent)
{
    return midiEvent.is_valid() && midiEvent.channel() >= 0 && midiEvent.channel() < kChannelCount &&
        midiEvent.data_length >= 3 && (midiEvent.status() & 0xF0) == 0xB0 && (midiEvent.data_byte(1) & 0x7F) == 64;
}

void DelayedNoteOffGate::recalculate_next_boundary()
{
    int64_t next = kInt64Max;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        if (state_[pitch] == kAttackPending) next = min64(next, attack_due_[pitch]);
        else if (state_[pitch] == kHeld && release_pending_[pitch]) next = min64(next, release_due_[pitch]);
        else if (state_[pitch] == kRecovery) next = min64(next, recovery_due_[pitch]);
    }
    next_boundary_ = next;
}

int32_t DelayedNoteOffGate::allocate_occurrence(const EventView& noteOn, int32_t pitch, uint8_t disposition, int64_t order)
{
    if (live_occurrences_ >= kMaximumLiveOccurrences)
        throw_invalid_operation("Delayed NoteOff has more than 1,000,000 simultaneous live note occurrences.");
    int32_t node;
    if (free_occurrence_ >= 0) {
        node = free_occurrence_;
        free_occurrence_ = occurrence(node).next_free;
    }
    else {
        node = next_unused_occurrence_++;
        if (node / kOccurrenceChunkSize == static_cast<int32_t>(occurrence_chunks_.size()))
            occurrence_chunks_.push_back(std::unique_ptr<std::vector<SourceOccurrence>>(
                new std::vector<SourceOccurrence>(kOccurrenceChunkSize)));
    }
    SourceOccurrence fresh;
    fresh.in_use = true;
    fresh.disposition = disposition;
    fresh.track = noteOn.track;
    fresh.channel = noteOn.channel();
    fresh.pitch = pitch;
    fresh.previous_active = -1;
    fresh.next_active = -1;
    fresh.next_free = -1;
    fresh.order = order;
    fresh.note_on = noteOn;
    occurrence(node) = fresh;
    append_active(node);
    live_occurrences_++;
    return node;
}

int32_t DelayedNoteOffGate::match_occurrence(int32_t track, int32_t channel, int32_t pitch) const
{
    if (channel < 0 || channel >= kChannelCount) return -1;
    int32_t first = -1;
    for (int32_t node = active_head_[static_cast<size_t>(match_slot(channel, pitch))]; node >= 0;
         node = occurrence(node).next_active) {
        const SourceOccurrence& current = occurrence(node);
        if (current.key_released) continue;
        if (first < 0) first = node;
        if (current.track == track) return node;
    }
    return first;
}

void DelayedNoteOffGate::append_active(int32_t node)
{
    SourceOccurrence& current = occurrence(node);
    size_t slot = static_cast<size_t>(match_slot(current.channel, current.pitch));
    current.previous_active = active_tail_[slot];
    if (active_tail_[slot] >= 0) occurrence(active_tail_[slot]).next_active = node;
    else active_head_[slot] = node;
    active_tail_[slot] = node;
}

void DelayedNoteOffGate::remove_and_free(int32_t node)
{
    SourceOccurrence current = occurrence(node);
    if (!current.in_use) return;
    size_t slot = static_cast<size_t>(match_slot(current.channel, current.pitch));
    if (current.previous_active >= 0) occurrence(current.previous_active).next_active = current.next_active;
    else active_head_[slot] = current.next_active;
    if (current.next_active >= 0) occurrence(current.next_active).previous_active = current.previous_active;
    else active_tail_[slot] = current.previous_active;
    SourceOccurrence cleared;
    cleared.next_free = free_occurrence_;
    cleared.previous_active = -1;
    cleared.next_active = -1;
    occurrence(node) = cleared;
    free_occurrence_ = node;
    live_occurrences_--;
}

void DelayedNoteOffGate::count_filtered(uint8_t reason, int64_t count)
{
    if (count <= 0) return;
    filtered_events_ += count;
    if (reason == kInsideDuration) inside_duration_filtered_ += count;
    else if (reason == kRecoveryLost) recovery_lost_filtered_ += count;
    else if (reason == kRecoveryEnded) recovery_ended_filtered_ += count;
}

bool DelayedNoteOffGate::compare_transitions(const DueTransition& left, const DueTransition& right)
{
    if (left.kind != right.kind) {
        if (left.kind == kTransitionRelease) return true;
        if (right.kind == kTransitionRelease) return false;
    }
    if (left.order != right.order) return left.order < right.order;
    return left.pitch < right.pitch;
}

bool DelayedNoteOffGate::compare_legacy_transitions(const DueTransition& left, const DueTransition& right)
{
    if (left.order != right.order) return left.order < right.order;
    if (left.kind != right.kind) {
        if (left.kind == kTransitionRelease) return true;
        if (right.kind == kTransitionRelease) return false;
    }
    return left.pitch < right.pitch;
}

int64_t DelayedNoteOffGate::add_saturated(int64_t value, int64_t increment)
{
    return increment > 0 && value > kInt64Max - increment ? kInt64Max : wrap_add(value, increment);
}

EventView DelayedNoteOffGate::create_release(const EventView& source, int32_t pitch, int32_t channel,
    int64_t boundaryMicroseconds)
{
    if (channel < 0 || channel >= kChannelCount) channel = 0;
    uint8_t command = source.is_valid() && (source.status() & 0xF0) == 0x90 ? 0x90 : 0x80;
    uint8_t status = static_cast<uint8_t>(command | channel);
    uint8_t velocity = source.is_valid() && source.data_length > 2 ? source.data_byte(2) : 0;
    return EventView::create(source.is_valid() ? source.tick : 0, source.is_valid() ? source.micros : boundaryMicroseconds,
        source.is_valid() ? source.track : -1, EventKind::NoteOff, channel, status,
        source.is_valid() ? source.event_index() : -1, static_cast<uint8_t>(pitch), velocity, 3);
}

} // namespace models
} // namespace midibn
