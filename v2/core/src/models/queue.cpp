#include "midibn/models/queue.h"

namespace midibn {
namespace models {

// -------------------------------------------------------- pending queue

PendingMidiQueue::PendingMidiQueue(int32_t capacity, bool noteIndexEnabled)
{
    if (capacity < 1) throw_argument_out_of_range("capacity");
    note_index_enabled_ = noteIndexEnabled;
}

int32_t PendingMidiQueue::oldest_event_index() const
{
    if (count_ == 0) return -1;
    return note_index_enabled_ ? node(head_).entry.event_index
                               : (*simple_segments_.front())[static_cast<size_t>(simple_head_offset_)].event_index;
}

int32_t PendingMidiQueue::allocated_segment_count() const
{
    return note_index_enabled_ ? static_cast<int32_t>(segments_.size())
        : static_cast<int32_t>(simple_segments_.size() + simple_free_segments_.size());
}

void PendingMidiQueue::enqueue(int32_t eventIndex)
{
    Entry entry;
    entry.event_index = eventIndex;
    entry.paired_attack_index = -1;
    enqueue(entry);
}

void PendingMidiQueue::enqueue(const Entry& entry)
{
    if (!note_index_enabled_) { enqueue_simple(entry); return; }
    // A second attack with the same index is refused before anything is changed. v1 threw the same exception
    // only after it had already linked the node, which left a half-added entry in the queue (register entry DD-004).
    if (entry.is_note_on && pending_attack_nodes_.find(entry.event_index) != pending_attack_nodes_.end())
        throw_duplicate_key();
    int32_t id = allocate();
    // Every field is written in place (DD-095): building the node in a local and copying it in cost a 36-byte block
    // copy, which the XP release build (/arch:IA32) does with a slow `rep movs`.
    Node& fresh = node(id);
    fresh.entry = entry;
    fresh.previous = tail_;
    fresh.next = -1;
    fresh.eligible_previous = -1;
    fresh.eligible_next = -1;
    fresh.paired_release_node = -1;
    fresh.free_next = -1;
    if (tail_ >= 0) node(tail_).next = id;
    else head_ = id;
    tail_ = id;
    count_++;

    if (entry.is_note_on) {
        node(id).eligible_previous = eligible_tail_;
        if (eligible_tail_ >= 0) node(eligible_tail_).eligible_next = id;
        else eligible_head_ = id;
        eligible_tail_ = id;
        eligible_count_++;
        pending_attack_nodes_.emplace(entry.event_index, id);
    }
    else if (entry.is_note_off && entry.paired_attack_index >= 0) {
        auto found = pending_attack_nodes_.find(entry.paired_attack_index);
        if (found != pending_attack_nodes_.end()) node(found->second).paired_release_node = id;
    }
}

PendingMidiQueue::Entry PendingMidiQueue::dequeue_entry()
{
    if (!note_index_enabled_) return dequeue_simple();
    if (head_ < 0) throw_invalid_operation("The pending MIDI queue is empty.");
    int32_t id = head_;
    Entry entry = node(id).entry;
    remove(id);
    return entry;
}

bool PendingMidiQueue::try_evict_oldest_complete_note(int32_t& attackIndex, int32_t& releaseIndex)
{
    if (!note_index_enabled_) throw_invalid_operation("The pending-note index has not been enabled.");
    if (eligible_head_ < 0) {
        attackIndex = -1;
        releaseIndex = -1;
        return false;
    }
    int32_t attackNode = eligible_head_;
    attackIndex = node(attackNode).entry.event_index;
    releaseIndex = -1;
    int32_t releaseNode = node(attackNode).paired_release_node;
    if (releaseNode >= 0) {
        releaseIndex = node(releaseNode).entry.event_index;
        remove(releaseNode);
    }
    remove(attackNode);
    return true;
}

bool PendingMidiQueue::try_evict_oldest_complete_note_at_head(int32_t& attackIndex, int32_t& releaseIndex)
{
    if (!note_index_enabled_) throw_invalid_operation("The pending-note index has not been enabled.");
    if (eligible_head_ < 0 || eligible_head_ != head_) {
        attackIndex = -1;
        releaseIndex = -1;
        return false;
    }
    return try_evict_oldest_complete_note(attackIndex, releaseIndex);
}

bool PendingMidiQueue::try_evict_least_important_complete_note(const EventSource& events, int32_t& fairnessCursor,
    int32_t& attackIndex, int32_t& releaseIndex)
{
    if (!note_index_enabled_) throw_invalid_operation("The pending-note index has not been enabled.");
    if (static_cast<int32_t>(musical_candidates_.size()) < eligible_count_) {
        size_t capacity = musical_candidates_.size();
        while (capacity < static_cast<size_t>(eligible_count_)) capacity *= 2;
        musical_candidates_.resize(capacity);
        musical_attack_nodes_.resize(capacity);
        musical_release_nodes_.resize(capacity);
    }
    int32_t candidateCount = 0;
    for (int32_t nodeId = eligible_head_; nodeId >= 0; nodeId = node(nodeId).eligible_next) {
        const Node& current = node(nodeId);
        EventView attack = events.at(current.entry.event_index);
        int32_t releaseNode = current.paired_release_node;
        int64_t duration = releaseNode < 0 ? kInt64Max
            : max64(0, wrap_sub(events.at(node(releaseNode).entry.event_index).micros, attack.micros));
        // Filled in place (DD-095; a 40-byte block copy on the XP release build otherwise); every field is set.
        MusicalNotePriorityCandidate& candidate = musical_candidates_[static_cast<size_t>(candidateCount)];
        candidate.pitch = attack.data_byte(1) & 0x7F;
        candidate.velocity = attack.data_byte(2) & 0x7F;
        candidate.track = attack.track;
        candidate.channel = attack.channel();
        candidate.source_order = attack.event_index();
        candidate.intended_microseconds = attack.micros;
        candidate.duration_microseconds = duration;
        musical_attack_nodes_[static_cast<size_t>(candidateCount)] = nodeId;
        musical_release_nodes_[static_cast<size_t>(candidateCount)] = releaseNode;
        candidateCount++;
    }
    if (candidateCount == 0) {
        attackIndex = releaseIndex = -1;
        return false;
    }
    int32_t victimIndex = musical_priority_.select_least_important(musical_candidates_.data(),
        static_cast<int32_t>(musical_candidates_.size()), candidateCount, fairnessCursor);
    int32_t victimAttack = musical_attack_nodes_[static_cast<size_t>(victimIndex)];
    int32_t victimRelease = musical_release_nodes_[static_cast<size_t>(victimIndex)];
    attackIndex = node(victimAttack).entry.event_index;
    releaseIndex = victimRelease < 0 ? -1 : node(victimRelease).entry.event_index;
    if (victimRelease >= 0) remove(victimRelease);
    remove(victimAttack);
    fairnessCursor = musical_priority_.next_fairness_cursor(musical_candidates_[static_cast<size_t>(victimIndex)]);
    return true;
}

void PendingMidiQueue::set_note_index_enabled(bool enabled)
{
    if (note_index_enabled_ == enabled) return;
    if (enabled) {
        int32_t count = count_;
        std::vector<Entry> entries = simple_entries(count);
        count_ = 0;
        note_index_enabled_ = true;
        for (const Entry& entry : entries) enqueue(entry);
        clear_simple();
        if (count_ != count) throw_invalid_operation("Pending queue conversion lost an event.");
        return;
    }
    int32_t linkedCount = count_;
    note_index_enabled_ = enabled;
    count_ = 0;
    for (int32_t id = head_; id >= 0; id = node(id).next) enqueue_simple(node(id).entry);
    if (count_ != linkedCount) throw_invalid_operation("Pending queue conversion lost an event.");
    head_ = tail_ = -1;
    pending_attack_nodes_.clear();
    eligible_head_ = eligible_tail_ = -1;
    eligible_count_ = 0;
    free_ = -1;
    next_fresh_ = 0;
}

void PendingMidiQueue::clear()
{
    clear_simple();
    simple_peak_ = 0;
    head_ = tail_ = eligible_head_ = eligible_tail_ = -1;
    free_ = -1;
    next_fresh_ = 0;
    eligible_count_ = count_ = 0;
    pending_attack_nodes_.clear();
}

std::vector<int32_t> PendingMidiQueue::event_indices() const
{
    std::vector<int32_t> result;
    if (note_index_enabled_) {
        for (int32_t id = head_; id >= 0; id = node(id).next) result.push_back(node(id).entry.event_index);
    }
    else for (const Entry& entry : simple_entries(count_)) result.push_back(entry.event_index);
    return result;
}

void PendingMidiQueue::remove(int32_t id)
{
    // Read in place (DD-095): only free_next of this node changes below, and no other node written here is this one
    // (its neighbours, and for a release the node of its attack), so the copy out and back of the whole node is not needed.
    Node& removed = node(id);
    if (removed.previous >= 0) node(removed.previous).next = removed.next;
    else head_ = removed.next;
    if (removed.next >= 0) node(removed.next).previous = removed.previous;
    else tail_ = removed.previous;

    if (note_index_enabled_ && removed.entry.is_note_on) {
        if (removed.eligible_previous >= 0) node(removed.eligible_previous).eligible_next = removed.eligible_next;
        else eligible_head_ = removed.eligible_next;
        if (removed.eligible_next >= 0) node(removed.eligible_next).eligible_previous = removed.eligible_previous;
        else eligible_tail_ = removed.eligible_previous;
        eligible_count_--;
        pending_attack_nodes_.erase(removed.entry.event_index);
    }
    else if (note_index_enabled_ && removed.entry.is_note_off && removed.entry.paired_attack_index >= 0) {
        auto found = pending_attack_nodes_.find(removed.entry.paired_attack_index);
        if (found != pending_attack_nodes_.end() && node(found->second).paired_release_node == id)
            node(found->second).paired_release_node = -1;
    }

    removed.free_next = free_;
    free_ = id;
    count_--;
}

int32_t PendingMidiQueue::allocate()
{
    if (free_ >= 0) {
        int32_t id = free_;
        free_ = node(id).free_next;
        return id;
    }
    int32_t fresh = next_fresh_++;
    if ((fresh >> kSegmentShift) >= static_cast<int32_t>(segments_.size()))
        segments_.push_back(std::unique_ptr<std::vector<Node>>(new std::vector<Node>(kSegmentSize)));
    return fresh;
}

void PendingMidiQueue::enqueue_simple(const Entry& entry)
{
    if (!simple_tail_segment_ || simple_tail_offset_ == kSegmentSize) {
        if (!simple_free_segments_.empty()) {
            simple_tail_segment_ = simple_free_segments_.back();
            simple_free_segments_.pop_back();
        }
        else simple_tail_segment_ = std::make_shared<std::vector<Entry>>(kSegmentSize);
        simple_segments_.push_back(simple_tail_segment_);
        simple_tail_offset_ = 0;
    }
    (*simple_tail_segment_)[static_cast<size_t>(simple_tail_offset_++)] = entry;
    count_++;
    if (count_ > simple_peak_) simple_peak_ = count_;
}

PendingMidiQueue::Entry PendingMidiQueue::dequeue_simple()
{
    if (count_ == 0) throw_invalid_operation("The pending MIDI queue is empty.");
    SimpleSegment head = simple_segments_.front();
    Entry entry = (*head)[static_cast<size_t>(simple_head_offset_++)];
    count_--;
    if (simple_head_offset_ == kSegmentSize || count_ == 0) {
        simple_segments_.pop_front();
        simple_free_segments_.push_back(head);
        simple_head_offset_ = 0;
        if (count_ == 0) {
            simple_tail_segment_.reset();
            simple_tail_offset_ = 0;
        }
    }
    return entry;
}

std::vector<PendingMidiQueue::Entry> PendingMidiQueue::simple_entries(int32_t count) const
{
    std::vector<Entry> result;
    int32_t remaining = count;
    bool first = true;
    for (const SimpleSegment& segment : simple_segments_) {
        int32_t start = first ? simple_head_offset_ : 0;
        int32_t end = segment == simple_tail_segment_ ? simple_tail_offset_ : kSegmentSize;
        for (int32_t offset = start; offset < end && remaining > 0; offset++, remaining--)
            result.push_back((*segment)[static_cast<size_t>(offset)]);
        first = false;
    }
    return result;
}

void PendingMidiQueue::clear_simple()
{
    while (!simple_segments_.empty()) {
        simple_free_segments_.push_back(simple_segments_.front());
        simple_segments_.pop_front();
    }
    simple_tail_segment_.reset();
    simple_head_offset_ = simple_tail_offset_ = 0;
}

// ---------------------------------------------------- forward-drop queue

ForwardDropQueue::ForwardDropQueue(int32_t capacity)
{
    if (capacity < 1) throw_argument_out_of_range("capacity");
    capacity_ = capacity;
}

void ForwardDropQueue::advance(int64_t sourceMicroseconds)
{
    while (busy_ && completion_microseconds_ <= sourceMicroseconds) {
        if (pending_service_.empty()) {
            busy_ = false;
            break;
        }
        int64_t service = pending_service_.front();
        pending_service_.pop_front();
        completion_microseconds_ = checked_add(completion_microseconds_, service);
    }
}

bool ForwardDropQueue::try_admit(int64_t sourceMicroseconds, int64_t serviceMicroseconds, bool safetyAdmission)
{
    advance(sourceMicroseconds);
    if (occupancy() >= capacity_ && !safetyAdmission) return false;
    serviceMicroseconds = max64(0, serviceMicroseconds);
    if (!busy_) {
        busy_ = true;
        completion_microseconds_ = checked_add(sourceMicroseconds, serviceMicroseconds);
    }
    else pending_service_.push_back(serviceMicroseconds);
    int32_t current = occupancy();
    if (current > maximum_occupancy_) maximum_occupancy_ = current;
    return true;
}

// ------------------------------------------------- complete-note tracker

CompleteNoteEventKind CompleteNoteTracker::classify(const EventView& midiEvent)
{
    if (!midiEvent.is_valid() || midiEvent.data_length < 3) return CompleteNoteEventKind::Other;
    int command = midiEvent.status() & 0xF0;
    if (command == 0x90)
        return midiEvent.data_byte(2) == 0 ? CompleteNoteEventKind::NoteOff : CompleteNoteEventKind::NoteOn;
    return command == 0x80 ? CompleteNoteEventKind::NoteOff : CompleteNoteEventKind::Other;
}

bool CompleteNoteTracker::is_safety_control(const EventView& midiEvent)
{
    if (!midiEvent.is_valid() || midiEvent.data_length < 3 || (midiEvent.status() & 0xF0) != 0xB0) return false;
    int controller = midiEvent.data_byte(1);
    int value = midiEvent.data_byte(2);
    return (controller == 64 && value < 64) || (controller >= 120 && controller <= 127);
}

void CompleteNoteTracker::record_note_on(const EventView& midiEvent, int32_t attackIndex, bool accepted,
    bool rejectedByCompleteNotePolicy)
{
    int s = slot(midiEvent);
    if (s < 0) return;
    occurrences_[static_cast<size_t>(s)].push_back(Occurrence{ attackIndex,
        accepted ? kAccepted : rejectedByCompleteNotePolicy ? kRejectedByCompleteNotePolicy : kRejectedByOtherPolicy });
    if (accepted && attackIndex >= 0) flags_.set(attackIndex, AttackFlags::kUnmatchedAccepted);
}

void CompleteNoteTracker::record_filtered_note_on(const EventView& midiEvent, int32_t attackIndex)
{
    int s = slot(midiEvent);
    if (s < 0) return;
    occurrences_[static_cast<size_t>(s)].push_back(Occurrence{ attackIndex, kRejectedBySourceFilter });
}

CompleteNoteTracker::NoteOffMatch CompleteNoteTracker::take_note_off(const EventView& midiEvent)
{
    NoteOffMatch match;
    int s = slot(midiEvent);
    if (s < 0) return match;
    SlotFifo& queue = occurrences_[static_cast<size_t>(s)];
    if (queue.empty()) return match;
    Occurrence occurrence = queue.front();
    queue.pop_front();
    bool evicted = occurrence.attack_index >= 0 && flags_.clear(occurrence.attack_index, AttackFlags::kEvicted);
    bool filtered = occurrence.attack_index >= 0 && flags_.clear(occurrence.attack_index, AttackFlags::kFiltered);
    if (occurrence.attack_index >= 0) flags_.clear(occurrence.attack_index, AttackFlags::kUnmatchedAccepted);
    match.attack_index = occurrence.attack_index;
    match.accepted = occurrence.state == kAccepted && !evicted && !filtered;
    match.suppress = occurrence.state == kRejectedByCompleteNotePolicy || occurrence.state == kRejectedBySourceFilter ||
        evicted || filtered;
    match.filtered = occurrence.state == kRejectedBySourceFilter || filtered;
    return match;
}

void CompleteNoteTracker::mark_unsent_attack_evicted(int32_t attackIndex)
{
    if (flags_.test(attackIndex, AttackFlags::kUnmatchedAccepted)) flags_.set(attackIndex, AttackFlags::kEvicted);
}

void CompleteNoteTracker::mark_unsent_attack_filtered(int32_t attackIndex)
{
    if (flags_.test(attackIndex, AttackFlags::kUnmatchedAccepted)) {
        flags_.clear(attackIndex, AttackFlags::kEvicted);
        flags_.set(attackIndex, AttackFlags::kFiltered);
    }
}

int CompleteNoteTracker::slot(const EventView& midiEvent)
{
    if (!midiEvent.is_valid() || midiEvent.data_length < 2) return -1;
    int channel = midiEvent.status() & 0x0F;
    int key = midiEvent.data_byte(1) & 0x7F;
    return channel * 128 + key;
}

} // namespace models
} // namespace midibn
