#include "midibn/models/per_note_gate.h"

#include <algorithm>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace midibn {
namespace models {
namespace {

// Index of the lowest set bit (bits != 0): the compiler intrinsic where there is one, a loop otherwise.
inline int lowest_set_bit(uint32_t bits)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward(&index, bits);
    return static_cast<int>(index);
#elif defined(__GNUC__)
    return __builtin_ctz(bits);
#else
    int index = 0;
    while ((bits & 1u) == 0) { bits >>= 1; ++index; }
    return index;
#endif
}

} // namespace

PerNoteIntervalGate::PerNoteIntervalGate(int64_t intervalMicroseconds, bool intervalPolyphonyLimitEnabled,
    int32_t intervalPolyphonyLimit)
{
    if (intervalMicroseconds < 0) throw_argument_out_of_range("intervalMicroseconds");
    if (intervalPolyphonyLimit < 1 || intervalPolyphonyLimit > kPitchCount)
        throw_argument_out_of_range("intervalPolyphonyLimit");
    interval_microseconds_ = intervalMicroseconds;
    max_boundary_quotient_ = intervalMicroseconds > 0 ? kInt64Max / intervalMicroseconds : 0;
    interval_polyphony_limit_enabled_ = intervalPolyphonyLimitEnabled;
    interval_polyphony_limit_ = intervalPolyphonyLimit;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        output_channel_[pitch] = -1;
        candidate_head_[pitch] = candidate_representative_[pitch] = -1;
        tick_head_[pitch] = tick_tail_[pitch] = tick_representative_[pitch] = -1;
        attack_boundary_[pitch] = kInt64Max;
        preparatory_release_boundary_[pitch] = kInt64Max;
        natural_release_boundary_[pitch] = kInt64Max;
    }
    active_head_.fill(-1);
    active_tail_.fill(-1);
}

int32_t PerNoteIntervalGate::next_boundary_output_count() const
{
    if (next_boundary_ == kInt64Max) return 0;
    int32_t count = 0;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        if ((candidate_head_[pitch] >= 0 && candidate_boundary_[pitch] == next_boundary_) ||
            preparatory_release_boundary_[pitch] == next_boundary_ ||
            (pending_attack_[pitch].is_valid() && attack_boundary_[pitch] == next_boundary_) ||
            (output_down_[pitch] && natural_release_boundary_[pitch] == next_boundary_))
            count++;
    }
    return interval_polyphony_limit_enabled_ ? std::min(count, interval_polyphony_limit_) : count;
}

void PerNoteIntervalGate::begin_source_tick(int64_t absoluteTick)
{
    if (source_tick_ != kInt64Min) end_source_tick();
    source_tick_ = absoluteTick;
}

PerNoteGateAdmission PerNoteIntervalGate::admit(const EventView& midiEvent)
{
    int32_t pitch;
    bool noteOn;
    if (!try_classify(midiEvent, pitch, noteOn)) return PerNoteGateAdmission::NotNote;
    if (source_tick_ == kInt64Min) begin_source_tick(midiEvent.tick);

    if (noteOn) {
        int32_t node = allocate_occurrence(midiEvent, pitch);
        add_to_tick_group(pitch, node);
        return PerNoteGateAdmission::Accepted;
    }

    int32_t matchedNode = match_occurrence(midiEvent.track, midiEvent.channel(), pitch);
    if (matchedNode < 0) {
        filtered_events_++;
        return PerNoteGateAdmission::Filtered;
    }

    SourceOccurrence& matched = occurrence(matchedNode);
    matched.ended = true;
    matched.note_off = midiEvent;
    uint8_t disposition = matched.disposition;
    if (disposition == 0) return PerNoteGateAdmission::Accepted;
    if (disposition == 2) {
        filtered_events_++;
        free_occurrence(matchedNode);
        return PerNoteGateAdmission::Filtered;
    }

    if (selected_support_count_[pitch] > 0) selected_support_count_[pitch]--;
    if (selected_support_count_[pitch] == 0) {
        schedule_natural_release(pitch, midiEvent, midiEvent.micros);
        free_occurrence(matchedNode);
        return PerNoteGateAdmission::Accepted;
    }

    filtered_events_++;
    free_occurrence(matchedNode);
    return PerNoteGateAdmission::Filtered;
}

void PerNoteIntervalGate::end_source_tick()
{
    if (source_tick_ == kInt64Min) return;
    for (int word = 0; word < 4; word++) {
      uint32_t touched = tick_touched_[word];
      tick_touched_[word] = 0;
      while (touched != 0) {
        const int low = lowest_set_bit(touched);
        touched &= touched - 1;
        const int pitch = word * 32 + static_cast<int>(low);
        int32_t head = tick_head_[pitch];
        if (head < 0) continue;
        int32_t representative = tick_representative_[pitch];
        int64_t boundary = boundary_at_or_after(occurrence(representative).note_on.micros);
        if (candidate_head_[pitch] < 0)
            store_candidate(pitch, head, representative, tick_members_[pitch], boundary);
        else if (candidate_boundary_[pitch] == boundary &&
            is_better_representative(representative, candidate_representative_[pitch])) {
            reject_group(candidate_head_[pitch]);
            store_candidate(pitch, head, representative, tick_members_[pitch], boundary);
        }
        else reject_group(head);

        tick_head_[pitch] = tick_tail_[pitch] = tick_representative_[pitch] = -1;
        tick_members_[pitch] = 0;
      }
    }
    source_tick_ = kInt64Min;
    recalculate_next_boundary();
}

int32_t PerNoteIntervalGate::emit_boundary(int64_t boundaryMicroseconds, EventView* output, int32_t outputLength)
{
    if (output == nullptr || outputLength < kPitchCount)
        throw_argument("output", "A 128-entry output buffer is required.");
    end_source_tick();
    if (!interval_polyphony_limit_enabled_) return emit_boundary_without_polyphony_limit(boundaryMicroseconds, output);
    int32_t releaseCount = 0;
    int32_t attackCount = 0;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        if (candidate_head_[pitch] >= 0 && candidate_boundary_[pitch] <= boundaryMicroseconds) finalize_candidate(pitch);
        boundary_kind_[pitch] = 0;
        if (output_down_[pitch] &&
            (preparatory_release_boundary_[pitch] <= boundaryMicroseconds ||
                natural_release_boundary_[pitch] <= boundaryMicroseconds)) {
            boundary_kind_[pitch] = 1;
            release_pitches_[static_cast<size_t>(releaseCount++)] = pitch;
        }
        else if (pending_attack_[pitch].is_valid() && attack_boundary_[pitch] <= boundaryMicroseconds) {
            boundary_kind_[pitch] = 2;
            attack_pitches_[static_cast<size_t>(attackCount++)] = pitch;
        }
    }

    sort_release_pitches(releaseCount);
    int32_t count = 0;
    int32_t emittedReleases = std::min(releaseCount, interval_polyphony_limit_);
    for (int32_t index = 0; index < releaseCount; index++) {
        int32_t pitch = release_pitches_[static_cast<size_t>(index)];
        if (index < emittedReleases) {
            emit_protected_release(pitch, boundaryMicroseconds, output, count);
            release_carry_order_[static_cast<size_t>(pitch)] = 0;
        }
        else {
            if (release_carry_order_[static_cast<size_t>(pitch)] == 0)
                release_carry_order_[static_cast<size_t>(pitch)] = next_release_carry_order_++;
            int64_t following = add_interval(boundaryMicroseconds);
            arm(pitch);
            if (preparatory_release_boundary_[static_cast<size_t>(pitch)] <= boundaryMicroseconds)
                preparatory_release_boundary_[static_cast<size_t>(pitch)] = following;
            if (natural_release_boundary_[static_cast<size_t>(pitch)] <= boundaryMicroseconds)
                natural_release_boundary_[static_cast<size_t>(pitch)] = following;
        }
    }

    int32_t attackCapacity = interval_polyphony_limit_ - count;
    while (attackCount > attackCapacity) {
        for (int32_t index = 0; index < attackCount; index++) {
            int32_t pitch = attack_pitches_[static_cast<size_t>(index)];
            const EventView& attack = pending_attack_[static_cast<size_t>(pitch)];
            // Filled in place (DD-095): no 40-byte block copy into the array afterwards; every field is set below.
            MusicalNotePriorityCandidate& candidate = musical_candidates_[static_cast<size_t>(index)];
            candidate.pitch = pitch;
            candidate.velocity = attack.data_byte(2) & 0x7F;
            candidate.track = attack.track;
            candidate.channel = attack.channel();
            candidate.source_order = attack.event_index();
            candidate.intended_microseconds = boundaryMicroseconds;
            candidate.duration_microseconds = natural_release_[static_cast<size_t>(pitch)].is_valid()
                ? max64(0, wrap_sub(natural_release_[static_cast<size_t>(pitch)].micros, attack.micros)) : kInt64Max;
        }
        int32_t victimIndex = musical_priority_.select_least_important(musical_candidates_.data(), kPitchCount,
            attackCount, musical_fairness_cursor_);
        musical_fairness_cursor_ = musical_priority_.next_fairness_cursor(musical_candidates_[static_cast<size_t>(victimIndex)]);
        drop_pending_attack(attack_pitches_[static_cast<size_t>(victimIndex)]);
        attackCount--;
        attack_pitches_[static_cast<size_t>(victimIndex)] = attack_pitches_[static_cast<size_t>(attackCount)];
    }
    std::sort(attack_pitches_.begin(), attack_pitches_.begin() + attackCount);
    for (int32_t index = 0; index < attackCount; index++) {
        size_t pitch = static_cast<size_t>(attack_pitches_[static_cast<size_t>(index)]);
        EventView attack = pending_attack_[pitch];
        if (!attack.is_valid()) continue;
        output[count++] = attack;
        output_down_[pitch] = true;
        output_channel_[pitch] = attack.channel();
        minimum_release_boundary_[pitch] = add_interval(boundaryMicroseconds);
        next_attack_boundary_[pitch] = add_interval(add_interval(boundaryMicroseconds));
        pending_attack_[pitch].valid = false;
        attack_boundary_[pitch] = kInt64Max;
        preparatory_release_boundary_[pitch] = kInt64Max;
        if (selected_support_count_[pitch] == 0)
            schedule_natural_release(static_cast<int32_t>(pitch), natural_release_[pitch], minimum_release_boundary_[pitch]);
    }
    recalculate_next_boundary();
    return count;
}

int32_t PerNoteIntervalGate::emit_boundary_without_polyphony_limit(int64_t boundaryMicroseconds, EventView* output)
{
    int32_t count = 0;
    // Only the armed pitches can have anything due (DD-075); they are visited in ascending pitch order, as before.
    for (int word = 0; word < 4; word++) {
      uint32_t bits = armed_[word];
      while (bits != 0) {
        const int low = lowest_set_bit(bits);
        bits &= bits - 1;
        const int pitch = word * 32 + static_cast<int>(low);
        if (candidate_head_[pitch] >= 0 && candidate_boundary_[pitch] <= boundaryMicroseconds) finalize_candidate(pitch);
        if (output_down_[pitch] &&
            (preparatory_release_boundary_[pitch] <= boundaryMicroseconds ||
                natural_release_boundary_[pitch] <= boundaryMicroseconds)) {
            emit_protected_release(pitch, boundaryMicroseconds, output, count);
            continue;
        }
        // Looked at in place; only an attack that is really emitted is copied.
        if (!pending_attack_[pitch].is_valid() || attack_boundary_[pitch] > boundaryMicroseconds) continue;
        const EventView& attack = pending_attack_[pitch];
        output[count++] = attack;
        output_down_[pitch] = true;
        output_channel_[pitch] = attack.channel();
        minimum_release_boundary_[pitch] = add_interval(boundaryMicroseconds);
        next_attack_boundary_[pitch] = add_interval(add_interval(boundaryMicroseconds));
        pending_attack_[pitch].valid = false;
        attack_boundary_[pitch] = kInt64Max;
        preparatory_release_boundary_[pitch] = kInt64Max;
        if (selected_support_count_[pitch] == 0)
            schedule_natural_release(pitch, natural_release_[pitch], minimum_release_boundary_[pitch]);
      }
    }
    recalculate_next_boundary();
    return count;
}

void PerNoteIntervalGate::sort_release_pitches(int32_t count)
{
    for (int32_t index = 1; index < count; index++) {
        int32_t pitch = release_pitches_[static_cast<size_t>(index)];
        int32_t position = index;
        while (position > 0 && release_precedes(pitch, release_pitches_[static_cast<size_t>(position - 1)])) {
            release_pitches_[static_cast<size_t>(position)] = release_pitches_[static_cast<size_t>(position - 1)];
            position--;
        }
        release_pitches_[static_cast<size_t>(position)] = pitch;
    }
}

bool PerNoteIntervalGate::release_precedes(int32_t leftPitch, int32_t rightPitch) const
{
    int64_t leftCarry = release_carry_order_[static_cast<size_t>(leftPitch)];
    int64_t rightCarry = release_carry_order_[static_cast<size_t>(rightPitch)];
    if ((leftCarry != 0) != (rightCarry != 0)) return leftCarry != 0;
    if (leftCarry != rightCarry) return leftCarry < rightCarry;
    int64_t leftDue = min64(preparatory_release_boundary_[static_cast<size_t>(leftPitch)],
        natural_release_boundary_[static_cast<size_t>(leftPitch)]);
    int64_t rightDue = min64(preparatory_release_boundary_[static_cast<size_t>(rightPitch)],
        natural_release_boundary_[static_cast<size_t>(rightPitch)]);
    if (leftDue != rightDue) return leftDue < rightDue;
    return leftPitch < rightPitch;
}

void PerNoteIntervalGate::emit_protected_release(int32_t pitch, int64_t boundaryMicroseconds, EventView* output,
    int32_t& count)
{
    size_t p = static_cast<size_t>(pitch);
    bool natural = natural_release_boundary_[p] <= boundaryMicroseconds && selected_support_count_[p] == 0;
    EventView releaseSource = natural ? natural_release_[p] : EventView();
    int32_t velocity = !releaseSource.is_valid() ? 0 : releaseSource.data_byte(2) & 0x7F;
    output[count++] = create_note_off(releaseSource, pitch, output_channel_[p], velocity, boundaryMicroseconds);
    output_down_[p] = false;
    output_channel_[p] = -1;
    preparatory_release_boundary_[p] = kInt64Max;
    if (natural) {
        natural_release_[p].valid = false;
        natural_release_boundary_[p] = kInt64Max;
    }
    if (pending_attack_[p].is_valid()) drop_pending_attack(pitch);
}

void PerNoteIntervalGate::drop_pending_attack(int32_t pitch)
{
    size_t p = static_cast<size_t>(pitch);
    if (!pending_attack_[p].is_valid()) return;
    filtered_events_++;
    for (int32_t channel = 0; channel < kChannelCount; channel++) {
        for (int32_t node = active_head_[static_cast<size_t>(key(channel, pitch))]; node >= 0; node = occurrence(node).next_active) {
            SourceOccurrence& current = occurrence(node);
            if (current.disposition != 1) continue;
            current.disposition = 2;
        }
    }
    selected_support_count_[p] = 0;
    if (natural_release_[p].is_valid()) filtered_events_++;
    pending_attack_[p].valid = false;
    attack_boundary_[p] = kInt64Max;
    preparatory_release_boundary_[p] = kInt64Max;
    natural_release_[p].valid = false;
    natural_release_boundary_[p] = kInt64Max;
}

void PerNoteIntervalGate::complete_source(int64_t sourceEndMicroseconds)
{
    if (source_complete_) return;
    end_source_tick();
    source_complete_ = true;
    sourceEndMicroseconds = max64(0, sourceEndMicroseconds);
    for (size_t slot = 0; slot < active_head_.size(); slot++) {
        int32_t node = active_head_[slot];
        active_head_[slot] = active_tail_[slot] = -1;
        while (node >= 0) {
            // Updated in place (DD-075): the whole 160-byte record used to be copied out and back.
            SourceOccurrence& current = occurrence(node);
            int32_t next = current.next_active;
            current.ended = true;
            current.next_active = -1;
            const uint8_t disposition = current.disposition;
            if (disposition == 1 && selected_support_count_[static_cast<size_t>(current.pitch)] > 0)
                selected_support_count_[static_cast<size_t>(current.pitch)]--;
            if (disposition != 0) free_occurrence(node);
            node = next;
        }
    }
    for (int pitch = 0; pitch < kPitchCount; pitch++)
        // Preserve an already paired source release; synthesize only when none is scheduled.
        if (selected_support_count_[pitch] == 0 && (output_down_[pitch] || pending_attack_[pitch].is_valid()) &&
            natural_release_boundary_[pitch] == kInt64Max)
            schedule_natural_release(pitch, EventView(), sourceEndMicroseconds);
    recalculate_next_boundary();
}

int32_t PerNoteIntervalGate::retire_channel(int32_t channel, int64_t nowMicroseconds)
{
    if (channel < 0 || channel >= kChannelCount) return 0;
    int32_t routedFiltered = 0;
    for (int pitch = 0; pitch < kPitchCount; pitch++) {
        size_t k = static_cast<size_t>(key(channel, pitch));
        int32_t node = active_head_[k];
        active_head_[k] = active_tail_[k] = -1;
        while (node >= 0) {
            SourceOccurrence copy = occurrence(node);
            int32_t next = copy.next_active;
            copy.ended = true;
            copy.removed_by_routing = true;
            copy.next_active = -1;
            occurrence(node) = copy;
            if (copy.disposition == 0) routedFiltered++;
            else if (copy.disposition == 1 && selected_support_count_[pitch] > 0) selected_support_count_[pitch]--;
            if (copy.disposition != 0) free_occurrence(node);
            node = next;
        }

        if (pending_attack_[pitch].is_valid() && pending_attack_[pitch].channel() == channel) {
            pending_attack_[pitch].valid = false;
            attack_boundary_[pitch] = kInt64Max;
            preparatory_release_boundary_[pitch] = kInt64Max;
            routedFiltered++;
        }
        if (output_down_[pitch] && output_channel_[pitch] == channel) {
            output_down_[pitch] = false;
            output_channel_[pitch] = -1;
            natural_release_[pitch].valid = false;
            natural_release_boundary_[pitch] = kInt64Max;
            preparatory_release_boundary_[pitch] = kInt64Max;
        }
        if (!output_down_[pitch] && !pending_attack_[pitch].is_valid() && selected_support_count_[pitch] > 0) {
            int32_t replacement = find_best_selected_support(pitch);
            if (replacement >= 0) {
                int64_t boundary = boundary_at_or_after(max64(0, nowMicroseconds));
                if (boundary <= nowMicroseconds) boundary = add_interval(boundary);
                pending_attack_[pitch] = occurrence(replacement).note_on;
                arm(pitch);
                attack_boundary_[pitch] = boundary;
                natural_release_[pitch].valid = false;
                natural_release_boundary_[pitch] = kInt64Max;
            }
        }
        if (selected_support_count_[pitch] == 0 && output_down_[pitch])
            schedule_natural_release(pitch, EventView(), nowMicroseconds);
        if (!output_down_[pitch] && !pending_attack_[pitch].is_valid()) {
            natural_release_[pitch].valid = false;
            natural_release_boundary_[pitch] = kInt64Max;
        }
    }
    recalculate_next_boundary();
    return routedFiltered;
}

int32_t PerNoteIntervalGate::find_best_selected_support(int32_t pitch)
{
    int32_t best = -1;
    for (int32_t channel = 0; channel < kChannelCount; channel++) {
        for (int32_t node = active_head_[static_cast<size_t>(key(channel, pitch))]; node >= 0; node = occurrence(node).next_active) {
            const SourceOccurrence& current = occurrence(node);
            if (current.ended || current.removed_by_routing || current.disposition != 1) continue;
            if (best < 0 || is_better_representative(node, best)) best = node;
        }
    }
    return best;
}

int64_t PerNoteIntervalGate::take_filtered_event_count()
{
    int64_t count = filtered_events_;
    filtered_events_ = 0;
    return count;
}

bool PerNoteIntervalGate::has_active_occurrence(int32_t track, int32_t channel, int32_t pitch) const
{
    if (channel < 0 || channel >= kChannelCount || pitch < 0 || pitch >= kPitchCount) return false;
    for (int32_t node = active_head_[static_cast<size_t>(key(channel, pitch))]; node >= 0; node = occurrence(node).next_active)
        if (occurrence(node).track == track) return true;
    return false;
}

int32_t PerNoteIntervalGate::allocate_occurrence(const EventView& noteOn, int32_t pitch)
{
    int32_t node;
    if (free_occurrence_ >= 0) {
        node = free_occurrence_;
        free_occurrence_ = occurrence(node).next_free;
    }
    else {
        node = next_unused_occurrence_;
        if (node >= static_cast<int32_t>(occurrence_chunks_.size()) * kOccurrenceChunkSize)
            occurrence_chunks_.push_back(std::unique_ptr<std::vector<SourceOccurrence>>(
                new std::vector<SourceOccurrence>(kOccurrenceChunkSize)));
        next_unused_occurrence_++;
    }
    // Written field by field into the slot (DD-075): the same values a freshly constructed record held, without
    // building a 160-byte temporary and copying it over.
    SourceOccurrence& fresh = occurrence(node);
    fresh.in_use = true;
    fresh.ended = false;
    fresh.removed_by_routing = false;
    fresh.disposition = 0;
    fresh.track = noteOn.track;
    fresh.channel = noteOn.channel();
    fresh.pitch = pitch;
    fresh.next_active = -1;
    fresh.next_group = -1;
    fresh.next_free = -1;
    fresh.note_on = noteOn;
    fresh.note_off.valid = false;

    size_t k = static_cast<size_t>(key(noteOn.channel(), pitch));
    int32_t tail = active_tail_[k];
    if (tail < 0) active_head_[k] = node;
    else occurrence(tail).next_active = node;
    active_tail_[k] = node;
    return node;
}

int32_t PerNoteIntervalGate::match_occurrence(int32_t track, int32_t channel, int32_t pitch)
{
    if (channel < 0 || channel >= kChannelCount) return -1;
    size_t k = static_cast<size_t>(key(channel, pitch));
    int32_t previous = -1;
    int32_t node = active_head_[k];
    int32_t fallback = node;
    while (node >= 0) {
        if (occurrence(node).track == track) break;
        previous = node;
        node = occurrence(node).next_active;
    }
    if (node < 0) { node = fallback; previous = -1; }
    if (node < 0) return -1;
    int32_t next = occurrence(node).next_active;
    if (previous < 0) active_head_[k] = next;
    else occurrence(previous).next_active = next;
    if (active_tail_[k] == node) active_tail_[k] = previous;
    occurrence(node).next_active = -1;
    return node;
}

void PerNoteIntervalGate::add_to_tick_group(int32_t pitch, int32_t node)
{
    size_t p = static_cast<size_t>(pitch);
    int32_t tail = tick_tail_[p];
    if (tail < 0) tick_head_[p] = node;
    else occurrence(tail).next_group = node;
    tick_tail_[p] = node;
    tick_members_[p]++;
    tick_touched_[pitch >> 5] |= static_cast<uint32_t>(1) << (pitch & 31);
    if (tick_representative_[p] < 0 || is_better_representative(node, tick_representative_[p]))
        tick_representative_[p] = node;
}

void PerNoteIntervalGate::store_candidate(int32_t pitch, int32_t head, int32_t representative, int32_t members,
    int64_t boundary)
{
    size_t p = static_cast<size_t>(pitch);
    arm(pitch);
    candidate_head_[p] = head;
    candidate_representative_[p] = representative;
    candidate_members_[p] = members;
    candidate_boundary_[p] = boundary;
}

void PerNoteIntervalGate::finalize_candidate(int32_t pitch)
{
    size_t p = static_cast<size_t>(pitch);
    int32_t head = candidate_head_[p];
    if (head < 0) return;
    int64_t firstBoundary = candidate_boundary_[p];
    int64_t assigned = max64(firstBoundary, next_attack_boundary_[p]);
    if (output_down_[p] || pending_attack_[p].is_valid()) assigned = max64(assigned, add_interval(firstBoundary));
    if (assigned > firstBoundary) {
        reject_group(head);
        clear_candidate(pitch);
        return;
    }

    int32_t representative = candidate_representative_[p];
    if (representative < 0 || occurrence(representative).removed_by_routing) representative = find_best_group_member(head);
    if (representative < 0) {
        discard_removed_group(head);
        clear_candidate(pitch);
        return;
    }
    EventView representativeEvent = occurrence(representative).note_on;

    int32_t liveSupports = 0;
    int32_t endedCount = 0;
    int32_t selectedMembers = 0;
    EventView latestOff;
    int32_t node = head;
    while (node >= 0) {
        SourceOccurrence& current = occurrence(node);   // updated in place (DD-075), same field changes as before
        int32_t next = current.next_group;
        const bool ended = current.ended;
        if (!current.removed_by_routing) {
            current.next_group = -1;
            selectedMembers++;
            current.disposition = 1;
            if (ended) {
                endedCount++;
                if (current.note_off.is_valid() && (!latestOff.is_valid() || is_later_source_event(current.note_off, latestOff)))
                    latestOff = current.note_off;
            }
            else liveSupports++;
        }
        if (ended) free_occurrence(node);
        node = next;
    }

    filtered_events_ += std::max(0, selectedMembers - 1);
    selected_support_count_[p] += liveSupports;
    if (selected_support_count_[p] == 0) {
        if (endedCount > 0) filtered_events_ += endedCount - 1;
        natural_release_[p] = latestOff;
    }
    else filtered_events_ += endedCount;

    pending_attack_[p] = representativeEvent;
    arm(pitch);
    attack_boundary_[p] = assigned;
    if (output_down_[p]) preparatory_release_boundary_[p] = wrap_sub(assigned, interval_microseconds_);
    if (selected_support_count_[p] == 0) schedule_natural_release(pitch, latestOff, add_interval(assigned));
    clear_candidate(pitch);
}

void PerNoteIntervalGate::reject_group(int32_t head)
{
    int32_t node = head;
    while (node >= 0) {
        SourceOccurrence& current = occurrence(node);
        int32_t next = current.next_group;
        const bool ended = current.ended;
        if (!current.removed_by_routing) {
            current.next_group = -1;
            current.disposition = 2;
            filtered_events_++;
            if (ended) filtered_events_++;
        }
        if (ended) free_occurrence(node);
        node = next;
    }
}

void PerNoteIntervalGate::discard_removed_group(int32_t head)
{
    int32_t node = head;
    while (node >= 0) {
        SourceOccurrence& current = occurrence(node);
        int32_t next = current.next_group;
        const bool ended = current.ended;
        current.next_group = -1;
        if (ended) free_occurrence(node);
        node = next;
    }
}

void PerNoteIntervalGate::clear_candidate(int32_t pitch)
{
    size_t p = static_cast<size_t>(pitch);
    candidate_head_[p] = candidate_representative_[p] = -1;
    candidate_members_[p] = 0;
    candidate_boundary_[p] = 0;
}

int32_t PerNoteIntervalGate::find_best_group_member(int32_t head)
{
    int32_t best = -1;
    for (int32_t node = head; node >= 0; node = occurrence(node).next_group)
        if (!occurrence(node).removed_by_routing && (best < 0 || is_better_representative(node, best))) best = node;
    return best;
}

bool PerNoteIntervalGate::is_better_representative(int32_t candidate, int32_t current) const
{
    return is_better_candidate(occurrence(candidate).note_on, occurrence(current).note_on);
}

bool PerNoteIntervalGate::is_better_candidate(const EventView& left, const EventView& right)
{
    int leftVelocity = left.data_byte(2) & 0x7F;
    int rightVelocity = right.data_byte(2) & 0x7F;
    if (leftVelocity != rightVelocity) return leftVelocity > rightVelocity;
    if (left.event_index() != right.event_index()) return left.event_index() < right.event_index();
    return left.track < right.track;
}

bool PerNoteIntervalGate::is_later_source_event(const EventView& candidate, const EventView& current)
{
    if (candidate.micros != current.micros) return candidate.micros > current.micros;
    if (candidate.tick != current.tick) return candidate.tick > current.tick;
    return candidate.event_index() > current.event_index();
}

void PerNoteIntervalGate::schedule_natural_release(int32_t pitch, const EventView& source, int64_t requestedMicroseconds)
{
    size_t p = static_cast<size_t>(pitch);
    int64_t boundary = boundary_at_or_after(max64(0, requestedMicroseconds));
    if (minimum_release_boundary_[p] != 0) boundary = max64(boundary, minimum_release_boundary_[p]);
    if (pending_attack_[p].is_valid()) boundary = max64(boundary, add_interval(attack_boundary_[p]));
    natural_release_[p] = source;
    arm(pitch);
    natural_release_boundary_[p] = boundary;
}

void PerNoteIntervalGate::free_occurrence(int32_t node)
{
    if (node < 0 || node >= next_unused_occurrence_ || !occurrence(node).in_use) return;
    // Same values as a default record with the three links set (DD-075: no 160-byte temporary). Only the two event
    // views' validity is cleared; their other fields are never read while a record is free and allocation rewrites them.
    SourceOccurrence& cleared = occurrence(node);
    cleared.in_use = false;
    cleared.ended = false;
    cleared.removed_by_routing = false;
    cleared.disposition = 0;
    cleared.track = 0;
    cleared.channel = 0;
    cleared.pitch = 0;
    cleared.next_free = free_occurrence_;
    cleared.next_active = -1;
    cleared.next_group = -1;
    cleared.note_on.valid = false;
    cleared.note_off.valid = false;
    free_occurrence_ = node;
}

int64_t PerNoteIntervalGate::boundary_at_or_after(int64_t microseconds) const
{
    if (interval_microseconds_ == 0) return max64(0, microseconds);
    if (microseconds <= 0) return 0;
    int64_t quotient = microseconds / interval_microseconds_;
    int64_t remainder = microseconds - quotient * interval_microseconds_;   // microseconds % interval, one division less
    if (remainder == 0) return quotient * interval_microseconds_;
    if (quotient >= max_boundary_quotient_) return kInt64Max;   // kInt64Max / interval_microseconds_, computed once
    return (quotient + 1) * interval_microseconds_;
}

int64_t PerNoteIntervalGate::add_interval(int64_t boundary) const
{
    if (interval_microseconds_ == 0) return boundary;
    return boundary > kInt64Max - interval_microseconds_ ? kInt64Max : boundary + interval_microseconds_;
}

void PerNoteIntervalGate::recalculate_next_boundary()
{
    int64_t next = kInt64Max;
    // Only pitches that may hold a finite boundary are visited (DD-075): armed_ has a bit for every pitch that was
    // given a candidate, an attack boundary, a preparatory release or a natural release since it was last found idle,
    // and a pitch with none of those contributes nothing. Same minimum as visiting all 128 pitches, which this loop
    // did for every source tick.
    for (int word = 0; word < 4; word++) {
        uint32_t bits = armed_[word];
        while (bits != 0) {
            const int low = lowest_set_bit(bits);
            bits &= bits - 1;
            const int pitch = word * 32 + static_cast<int>(low);
            if (candidate_head_[pitch] >= 0 && candidate_boundary_[pitch] < next) next = candidate_boundary_[pitch];
            if (preparatory_release_boundary_[pitch] < next) next = preparatory_release_boundary_[pitch];
            if (attack_boundary_[pitch] < next) next = attack_boundary_[pitch];
            if ((output_down_[pitch] || pending_attack_[pitch].is_valid()) && natural_release_boundary_[pitch] < next)
                next = natural_release_boundary_[pitch];
            if (candidate_head_[pitch] < 0 && preparatory_release_boundary_[pitch] == kInt64Max &&
                attack_boundary_[pitch] == kInt64Max && natural_release_boundary_[pitch] == kInt64Max)
                armed_[word] &= ~(static_cast<uint32_t>(1) << low);
        }
    }
    next_boundary_ = next;
}

EventView PerNoteIntervalGate::create_note_off(const EventView& source, int32_t pitch, int32_t channel, int32_t velocity,
    int64_t boundaryMicroseconds)
{
    if (channel < 0 || channel >= kChannelCount) channel = 0;
    uint8_t status = static_cast<uint8_t>(0x80 | channel);
    return EventView::create(source.is_valid() ? source.tick : 0, source.is_valid() ? source.micros : boundaryMicroseconds,
        source.is_valid() ? source.track : -1, EventKind::NoteOff, channel, status,
        source.is_valid() ? source.event_index() : -1, static_cast<uint8_t>(pitch), static_cast<uint8_t>(velocity), 3);
}

bool PerNoteIntervalGate::try_classify(const EventView& midiEvent, int32_t& pitch, bool& noteOn)
{
    pitch = -1;
    noteOn = false;
    if (!midiEvent.is_valid() || midiEvent.channel() < 0 || midiEvent.channel() >= kChannelCount || midiEvent.data_length < 3)
        return false;
    int command = midiEvent.status() & 0xF0;
    if (command != 0x80 && command != 0x90) return false;
    pitch = midiEvent.data_byte(1) & 0x7F;
    noteOn = command == 0x90 && midiEvent.data_byte(2) != 0;
    return true;
}

} // namespace models
} // namespace midibn
