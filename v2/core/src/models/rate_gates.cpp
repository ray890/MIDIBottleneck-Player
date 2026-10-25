#include "midibn/models/rate_gates.h"
#include "midibn/models/per_note_gate.h"
#include "midibn/models/queue.h"

namespace midibn {
namespace models {

namespace {

int64_t checked_attack_rate(int64_t attacksPerSecond)
{
    if (attacksPerSecond < 0 || attacksPerSecond > 9999999) throw_argument_out_of_range("attacksPerSecond");
    return attacksPerSecond;
}

int64_t checked_output_rate(int64_t eventsPerSecond, int64_t clockUnitsPerSecond)
{
    if (eventsPerSecond < 0 || eventsPerSecond > 9999999) throw_argument_out_of_range("eventsPerSecond");
    if (clockUnitsPerSecond <= 0) throw_argument_out_of_range("clockUnitsPerSecond");
    return eventsPerSecond;
}

template <typename T> void ensure_capacity(std::vector<T>& values, std::vector<int32_t>& indices, size_t count)
{
    if (values.size() >= count) return;
    size_t capacity = values.size();
    while (capacity < count) capacity *= 2;
    values.assign(capacity, T());
    indices.assign(capacity, 0);
}

} // namespace

// ================================================ Note start rate gate

MusicalAttackRateLimiter::MusicalAttackRateLimiter(int64_t attacksPerSecond)
    : rate_(checked_attack_rate(attacksPerSecond)), pacer_(attacksPerSecond, 1000000)
{
    head_.fill(-1);
    tail_.fill(-1);
}

void MusicalAttackRateLimiter::begin_source_tick(int64_t)
{
    if (!tick_events_.empty()) throw_invalid_operation("The previous source tick was not completed.");
}

int32_t MusicalAttackRateLimiter::end_source_tick(std::vector<EventView>& emissions)
{
    emissions.clear();
    if (tick_events_.empty()) return 0;
    int64_t timeMicroseconds = max64(0, tick_events_[0].micros);
    build_attack_priorities();
    select_attacks(timeMicroseconds);

    size_t candidateIndex = 0;
    for (size_t index = 0; index < tick_events_.size(); index++) {
        const EventView& midiEvent = tick_events_[index];
        int32_t pitch;
        bool noteOn;
        if (!PerNoteIntervalGate::try_classify(midiEvent, pitch, noteOn)) {
            emissions.push_back(midiEvent);
            continue;
        }
        if (noteOn) {
            while (candidates_[candidateIndex].tick_index != static_cast<int32_t>(index)) candidateIndex++;
            bool accepted = candidates_[candidateIndex].accepted;
            append_occurrence(midiEvent.channel(), pitch, accepted);
            if (accepted) emissions.push_back(midiEvent);
            else rejected_attacks_++;
            candidateIndex++;
            continue;
        }
        int32_t node = remove_oldest(midiEvent.channel(), pitch);
        if (node < 0) emissions.push_back(midiEvent);
        else {
            if (get(node).accepted) emissions.push_back(midiEvent);
            else suppressed_releases_++;
            free_node(node);
        }
    }
    tick_events_.clear();
    candidates_.clear();
    return static_cast<int32_t>(emissions.size());
}

void MusicalAttackRateLimiter::complete_source()
{
    tick_events_.clear();
    candidates_.clear();
    for (size_t s = 0; s < head_.size(); s++) {
        int32_t node = head_[s];
        while (node >= 0) {
            int32_t next = get(node).next;
            free_node(node);
            node = next;
        }
        head_[s] = tail_[s] = -1;
    }
}

void MusicalAttackRateLimiter::retire_channel(int32_t channel)
{
    if (channel < 0 || channel >= kChannelCount) return;
    for (int32_t pitch = 0; pitch < kPitchCount; pitch++) {
        size_t s = static_cast<size_t>(slot(channel, pitch));
        int32_t node = head_[s];
        while (node >= 0) {
            int32_t next = get(node).next;
            free_node(node);
            node = next;
        }
        head_[s] = tail_[s] = -1;
    }
}

int64_t MusicalAttackRateLimiter::take_rejected_attacks()
{
    int64_t value = rejected_attacks_;
    rejected_attacks_ = 0;
    return value;
}

int64_t MusicalAttackRateLimiter::take_suppressed_releases()
{
    int64_t value = suppressed_releases_;
    suppressed_releases_ = 0;
    return value;
}

void MusicalAttackRateLimiter::build_attack_priorities()
{
    candidates_.clear();
    for (size_t index = 0; index < tick_events_.size(); index++) {
        const EventView& midiEvent = tick_events_[index];
        int32_t pitch;
        bool noteOn;
        if (!PerNoteIntervalGate::try_classify(midiEvent, pitch, noteOn) || !noteOn) continue;
        Candidate candidate;
        candidate.tick_index = static_cast<int32_t>(index);
        candidate.pitch = pitch;
        candidate.velocity = midiEvent.data_byte(2) & 0x7F;
        candidate.track = midiEvent.track;
        candidate.channel = midiEvent.channel();
        candidates_.push_back(candidate);
    }
}

void MusicalAttackRateLimiter::select_attacks(int64_t timeMicroseconds)
{
    int32_t candidateCount = static_cast<int32_t>(candidates_.size());
    if (candidateCount == 0) return;
    ensure_capacity(musical_candidates_, active_candidate_indices_, static_cast<size_t>(candidateCount));
    int64_t available = pacer_.available_slots(timeMicroseconds, candidateCount);
    int32_t admitted = static_cast<int32_t>(min64(candidateCount, available));
    for (int32_t index = 0; index < candidateCount; index++) {
        candidates_[static_cast<size_t>(index)].accepted = true;
        active_candidate_indices_[static_cast<size_t>(index)] = index;
    }
    int32_t activeCount = candidateCount;
    while (activeCount > admitted) {
        for (int32_t index = 0; index < activeCount; index++) {
            const Candidate& candidate = candidates_[static_cast<size_t>(active_candidate_indices_[static_cast<size_t>(index)])];
            MusicalNotePriorityCandidate m;
            m.pitch = candidate.pitch;
            m.velocity = candidate.velocity;
            m.track = candidate.track;
            m.channel = candidate.channel;
            m.source_order = candidate.tick_index;
            m.intended_microseconds = timeMicroseconds;
            // The streaming limiter has not seen the paired release yet.
            m.duration_microseconds = kInt64Max;
            musical_candidates_[static_cast<size_t>(index)] = m;
        }
        int32_t victim = musical_priority_.select_least_important(musical_candidates_.data(),
            static_cast<int32_t>(musical_candidates_.size()), activeCount, fairness_cursor_);
        fairness_cursor_ = musical_priority_.next_fairness_cursor(musical_candidates_[static_cast<size_t>(victim)]);
        int32_t candidateIndex = active_candidate_indices_[static_cast<size_t>(victim)];
        candidates_[static_cast<size_t>(candidateIndex)].accepted = false;
        activeCount--;
        active_candidate_indices_[static_cast<size_t>(victim)] = active_candidate_indices_[static_cast<size_t>(activeCount)];
    }
    if (rate_ != 0)
        for (int32_t index = 0; index < candidateCount; index++)
            if (candidates_[static_cast<size_t>(index)].accepted && !pacer_.try_consume(timeMicroseconds))
                throw_invalid_operation("The Note start pacing decision lost its due slot.");
}

void MusicalAttackRateLimiter::append_occurrence(int32_t channel, int32_t pitch, bool accepted)
{
    if (live_ >= kMaximumLiveOccurrences)
        throw_invalid_operation("The Note start rate gate has more than 1,000,000 unmatched source occurrences.");
    int32_t node;
    if (free_ >= 0) {
        node = free_;
        free_ = get(node).next_free;
    }
    else {
        node = next_unused_++;
        if (node / kChunkSize == static_cast<int32_t>(chunks_.size()))
            chunks_.push_back(std::unique_ptr<std::vector<Occurrence>>(new std::vector<Occurrence>(kChunkSize)));
    }
    size_t s = static_cast<size_t>(slot(channel, pitch));
    Occurrence fresh;
    fresh.in_use = true;
    fresh.accepted = accepted;
    fresh.channel = channel;
    fresh.pitch = pitch;
    fresh.previous = tail_[s];
    fresh.next = -1;
    fresh.next_free = -1;
    if (tail_[s] >= 0) get(tail_[s]).next = node;
    else head_[s] = node;
    tail_[s] = node;
    get(node) = fresh;
    live_++;
}

int32_t MusicalAttackRateLimiter::remove_oldest(int32_t channel, int32_t pitch)
{
    size_t s = static_cast<size_t>(slot(channel, pitch));
    int32_t node = head_[s];
    if (node < 0) return -1;
    Occurrence& current = get(node);
    head_[s] = current.next;
    if (current.next >= 0) get(current.next).previous = -1;
    else tail_[s] = -1;
    current.previous = current.next = -1;
    return node;
}

void MusicalAttackRateLimiter::free_node(int32_t node)
{
    if (!get(node).in_use) return;
    Occurrence cleared;
    cleared.next_free = free_;
    cleared.previous = -1;
    cleared.next = -1;
    get(node) = cleared;
    free_ = node;
    live_--;
}

// ===================================================== Output rate gate

OutputRateGate::OutputRateGate(int64_t eventsPerSecond, int64_t clockUnitsPerSecond)
    : rate_(checked_output_rate(eventsPerSecond, clockUnitsPerSecond)), pacer_(eventsPerSecond, clockUnitsPerSecond)
{
    head_.fill(-1);
    tail_.fill(-1);
}

void OutputRateGate::begin_source_tick(int64_t)
{
    if (!tick_events_.empty()) throw_invalid_operation("The previous output-rate source tick was not completed.");
}

int32_t OutputRateGate::end_source_tick(std::vector<EventView>& emissions, int64_t decisionTimestamp)
{
    emissions.clear();
    if (tick_events_.empty()) return 0;
    build_plans();
    select_attacks(decisionTimestamp);

    for (size_t index = 0; index < tick_events_.size(); index++) {
        const EventPlan& plan = plans_[index];
        if (plan.kind == PlanKind::RequiredRelease || (plan.kind == PlanKind::OrdinaryMessage && plan.accepted)) {
            emissions.push_back(tick_events_[index]);
            queue_dispatch(plan.kind == PlanKind::RequiredRelease);
            continue;
        }
        if (plan.kind == PlanKind::OrdinaryMessage) {
            rejected_other_messages_++;
            continue;
        }
        if (plan.kind == PlanKind::Suppressed) {
            suppressed_releases_++;
            continue;
        }
        const Candidate& candidate = candidates_[static_cast<size_t>(plan.candidate_index)];
        if (candidate.accepted) {
            emissions.push_back(tick_events_[index]);
            queue_dispatch(plan.kind == PlanKind::CandidateRelease);
        }
        else if (plan.kind == PlanKind::CandidateAttack) rejected_attacks_++;
        else suppressed_releases_++;
    }

    for (const Candidate& candidate : candidates_) {
        if (candidate.occurrence_node < 0) continue;
        Occurrence& current = get(candidate.occurrence_node);
        if (!current.in_use) continue;
        current.accepted = candidate.accepted;
        current.candidate_index = -1;
    }

    tick_events_.clear();
    plans_.clear();
    candidates_.clear();
    return static_cast<int32_t>(emissions.size());
}

bool OutputRateGate::record_dispatch(int64_t timestamp, bool requiredRelease)
{
    if (!pending_required_release_.empty()) {
        pending_required_release_.pop_front();
        pacer_.complete_reservation(timestamp);
        return false;
    }
    if (rate_ == 0) return false;
    bool consumed = pacer_.try_consume(timestamp);
    return requiredRelease && !consumed;
}

bool OutputRateGate::record_exceptional_dispatch(int64_t timestamp)
{
    if (rate_ == 0) return false;
    return !pacer_.try_consume(timestamp);
}

void OutputRateGate::complete_source()
{
    tick_events_.clear();
    plans_.clear();
    candidates_.clear();
    clear_occurrences();
}

void OutputRateGate::retire_channel(int32_t channel)
{
    if (channel < 0 || channel >= kChannelCount) return;
    for (int32_t pitch = 0; pitch < kPitchCount; pitch++) clear_slot(slot(channel, pitch));
}

int64_t OutputRateGate::take_rejected_attacks()
{
    int64_t value = rejected_attacks_;
    rejected_attacks_ = 0;
    return value;
}

int64_t OutputRateGate::take_suppressed_releases()
{
    int64_t value = suppressed_releases_;
    suppressed_releases_ = 0;
    return value;
}

int64_t OutputRateGate::take_rejected_other_messages()
{
    int64_t value = rejected_other_messages_;
    rejected_other_messages_ = 0;
    return value;
}

bool OutputRateGate::is_protected_traffic(const EventView& midiEvent)
{
    return CompleteNoteTracker::classify(midiEvent) != CompleteNoteEventKind::NoteOn;
}

void OutputRateGate::build_plans()
{
    plans_.clear();
    candidates_.clear();
    for (size_t index = 0; index < tick_events_.size(); index++) {
        const EventView& midiEvent = tick_events_[index];
        CompleteNoteEventKind kind = CompleteNoteTracker::classify(midiEvent);
        EventPlan plan;
        if (kind == CompleteNoteEventKind::Other) {
            plan.kind = PlanKind::OrdinaryMessage;
            plan.candidate_index = -1;
            plan.accepted = true;
            plans_.push_back(plan);
            continue;
        }
        int32_t pitch = midiEvent.data_byte(1) & 0x7F;
        if (kind == CompleteNoteEventKind::NoteOn) {
            int32_t candidateIndex = static_cast<int32_t>(candidates_.size());
            int32_t node = append_occurrence(midiEvent.channel(), pitch, candidateIndex);
            Candidate candidate;
            candidate.tick_index = static_cast<int32_t>(index);
            candidate.pitch = pitch;
            candidate.velocity = midiEvent.data_byte(2) & 0x7F;
            candidate.track = midiEvent.track;
            candidate.channel = midiEvent.channel();
            candidate.message_cost = 1;
            candidate.occurrence_node = node;
            candidate.accepted = true;
            candidates_.push_back(candidate);
            plan.kind = PlanKind::CandidateAttack;
            plan.candidate_index = candidateIndex;
            plans_.push_back(plan);
            continue;
        }

        Occurrence removed;
        if (!remove_oldest(midiEvent.channel(), pitch, removed)) {
            plan.kind = PlanKind::OrdinaryMessage;
            plan.candidate_index = -1;
            plan.accepted = true;
            plans_.push_back(plan);
            continue;
        }
        if (removed.candidate_index >= 0) {
            Candidate& candidate = candidates_[static_cast<size_t>(removed.candidate_index)];
            candidate.message_cost++;
            candidate.occurrence_node = -1;
            plan.kind = PlanKind::CandidateRelease;
            plan.candidate_index = removed.candidate_index;
        }
        else {
            plan.kind = removed.accepted ? PlanKind::RequiredRelease : PlanKind::Suppressed;
            plan.candidate_index = -1;
        }
        plans_.push_back(plan);
    }
}

void OutputRateGate::select_attacks(int64_t decisionTimestamp)
{
    int32_t candidateCount = static_cast<int32_t>(candidates_.size());
    int64_t requiredReleases = 0;
    int64_t ordinaryMessages = 0;
    for (const EventPlan& plan : plans_)
        if (plan.kind == PlanKind::RequiredRelease) requiredReleases++;
        else if (plan.kind == PlanKind::OrdinaryMessage) ordinaryMessages++;
    int64_t newAvailable = pacer_.available_slots(decisionTimestamp, ordinaryMessages + candidateCount * 2LL);
    if (ordinaryMessages == 0 && candidateCount != 0 && newAvailable == 1 &&
        pacer_.rolling_available_slots(decisionTimestamp, 2) >= 2)
        newAvailable = 2;
    int64_t available = requiredReleases + newAvailable;
    int64_t ordinaryAccepted = max64(0, min64(ordinaryMessages, available - requiredReleases));
    for (EventPlan& plan : plans_)
        if (plan.kind == PlanKind::OrdinaryMessage) plan.accepted = ordinaryAccepted-- > 0;
    int64_t planned = requiredReleases + min64(ordinaryMessages, max64(0, available - requiredReleases));
    if (candidateCount == 0) {
        reserve(decisionTimestamp, planned - requiredReleases);
        return;
    }
    ensure_capacity(musical_candidates_, active_candidate_indices_, static_cast<size_t>(candidateCount));
    for (int32_t index = 0; index < candidateCount; index++) {
        Candidate& candidate = candidates_[static_cast<size_t>(index)];
        candidate.accepted = true;
        active_candidate_indices_[static_cast<size_t>(index)] = index;
        planned += candidate.message_cost + (candidate.occurrence_node >= 0 ? 1 : 0);
    }
    int32_t activeCount = candidateCount;
    while (activeCount > 0 && planned > available) {
        for (int32_t index = 0; index < activeCount; index++) {
            const Candidate& candidate = candidates_[static_cast<size_t>(active_candidate_indices_[static_cast<size_t>(index)])];
            MusicalNotePriorityCandidate m;
            m.pitch = candidate.pitch;
            m.velocity = candidate.velocity;
            m.track = candidate.track;
            m.channel = candidate.channel;
            m.source_order = candidate.tick_index;
            m.intended_microseconds = tick_events_[static_cast<size_t>(candidate.tick_index)].micros;
            m.duration_microseconds = kInt64Max;
            musical_candidates_[static_cast<size_t>(index)] = m;
        }
        int32_t victim = musical_priority_.select_least_important(musical_candidates_.data(),
            static_cast<int32_t>(musical_candidates_.size()), activeCount, fairness_cursor_);
        fairness_cursor_ = musical_priority_.next_fairness_cursor(musical_candidates_[static_cast<size_t>(victim)]);
        int32_t candidateIndex = active_candidate_indices_[static_cast<size_t>(victim)];
        Candidate& rejected = candidates_[static_cast<size_t>(candidateIndex)];
        rejected.accepted = false;
        planned -= rejected.message_cost + (rejected.occurrence_node >= 0 ? 1 : 0);
        activeCount--;
        active_candidate_indices_[static_cast<size_t>(victim)] = active_candidate_indices_[static_cast<size_t>(activeCount)];
    }
    reserve(decisionTimestamp, planned - requiredReleases);
}

void OutputRateGate::reserve(int64_t decisionTimestamp, int64_t count)
{
    bool usedOccurrenceExtension = false;
    for (int64_t index = 0; index < count; index++)
        if (!pacer_.try_reserve(decisionTimestamp)) {
            if (usedOccurrenceExtension || !pacer_.try_reserve_ahead(decisionTimestamp))
                throw_invalid_operation("The Output rate pacing decision lost its reserved slot.");
            usedOccurrenceExtension = true;
        }
}

void OutputRateGate::queue_dispatch(bool requiredRelease)
{
    if (rate_ == 0) return;
    pending_required_release_.push_back(requiredRelease);
}

int32_t OutputRateGate::append_occurrence(int32_t channel, int32_t pitch, int32_t candidateIndex)
{
    if (live_ >= kMaximumLiveOccurrences)
        throw_invalid_operation("The Output rate gate has more than 1,000,000 unmatched source occurrences.");
    int32_t node;
    if (free_ >= 0) {
        node = free_;
        free_ = get(node).next_free;
    }
    else {
        node = next_unused_++;
        if (node / kChunkSize == static_cast<int32_t>(chunks_.size()))
            chunks_.push_back(std::unique_ptr<std::vector<Occurrence>>(new std::vector<Occurrence>(kChunkSize)));
    }
    size_t s = static_cast<size_t>(slot(channel, pitch));
    Occurrence fresh;
    fresh.in_use = true;
    fresh.accepted = true;
    fresh.candidate_index = candidateIndex;
    fresh.channel = channel;
    fresh.pitch = pitch;
    fresh.previous = tail_[s];
    fresh.next = -1;
    fresh.next_free = -1;
    if (tail_[s] >= 0) get(tail_[s]).next = node;
    else head_[s] = node;
    tail_[s] = node;
    get(node) = fresh;
    live_++;
    return node;
}

bool OutputRateGate::remove_oldest(int32_t channel, int32_t pitch, Occurrence& removed)
{
    size_t s = static_cast<size_t>(slot(channel, pitch));
    int32_t node = head_[s];
    if (node < 0) {
        removed = Occurrence();
        return false;
    }
    removed = get(node);
    head_[s] = removed.next;
    if (removed.next >= 0) get(removed.next).previous = -1;
    else tail_[s] = -1;
    free_node(node, false);
    return true;
}

void OutputRateGate::clear_occurrences()
{
    for (int32_t s = 0; s < kSlotCount; s++) clear_slot(s);
}

void OutputRateGate::clear_slot(int32_t slotIndex)
{
    size_t s = static_cast<size_t>(slotIndex);
    int32_t node = head_[s];
    while (node >= 0) {
        int32_t next = get(node).next;
        free_node(node, true);
        node = next;
    }
    head_[s] = tail_[s] = -1;
}

void OutputRateGate::free_node(int32_t node, bool cancelReservation)
{
    const Occurrence& current = get(node);
    if (!current.in_use) return;
    if (cancelReservation && current.accepted && current.candidate_index < 0) pacer_.cancel_reservation();
    Occurrence cleared;
    cleared.candidate_index = -1;
    cleared.previous = -1;
    cleared.next = -1;
    cleared.next_free = free_;
    get(node) = cleared;
    free_ = node;
    live_--;
}

} // namespace models
} // namespace midibn
