// Group 2 models (Milestone 3): queues. Ports of v1 PendingMidiQueue.cs,
// ForwardDropQueue.cs and CompleteNoteTracker.cs.
#pragma once

#include "midibn/models/priority.h"

#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace midibn {
namespace models {

struct TraceAccess;

// v1 PendingMidiQueue: FIFO value segments for single-event policies and
// a linked, note-indexed node pool for complete-note eviction.
class PendingMidiQueue {
public:
    struct Entry {
        int32_t event_index = 0;
        int32_t paired_attack_index = 0;
        bool is_note_on = false;
        bool is_note_off = false;
    };

    explicit PendingMidiQueue(int32_t capacity, bool noteIndexEnabled = false);

    int32_t count() const { return count_; }
    int32_t oldest_event_index() const;
    int32_t peak_backing_count() const { return note_index_enabled_ ? next_fresh_ : simple_peak_; }
    int32_t eligible_count() const { return eligible_count_; }
    int32_t allocated_segment_count() const;

    void enqueue(int32_t eventIndex);
    void enqueue(const Entry& entry);
    int32_t dequeue() { return dequeue_entry().event_index; }
    Entry dequeue_entry();
    bool try_evict_oldest_complete_note(int32_t& attackIndex, int32_t& releaseIndex);
    bool try_evict_oldest_complete_note_at_head(int32_t& attackIndex, int32_t& releaseIndex);
    bool try_evict_least_important_complete_note(const EventSource& events, int32_t& fairnessCursor,
        int32_t& attackIndex, int32_t& releaseIndex);
    void set_note_index_enabled(bool enabled);
    void clear();
    // Event indices in queue order (v1 GetEnumerator).
    std::vector<int32_t> event_indices() const;

private:
    friend struct TraceAccess;
    static constexpr int kSegmentShift = 8;
    static constexpr int kSegmentSize = 1 << kSegmentShift;
    static constexpr int kSegmentMask = kSegmentSize - 1;

    struct Node {
        Entry entry;
        int32_t previous = 0;
        int32_t next = 0;
        int32_t eligible_previous = 0;
        int32_t eligible_next = 0;
        int32_t paired_release_node = 0;
        int32_t free_next = 0;
    };
    using SimpleSegment = std::shared_ptr<std::vector<Entry>>;

    void remove(int32_t id);
    int32_t allocate();
    Node& node(int32_t id) { return (*segments_[static_cast<size_t>(id >> kSegmentShift)])[static_cast<size_t>(id & kSegmentMask)]; }
    const Node& node(int32_t id) const { return (*segments_[static_cast<size_t>(id >> kSegmentShift)])[static_cast<size_t>(id & kSegmentMask)]; }
    void enqueue_simple(const Entry& entry);
    Entry dequeue_simple();
    std::vector<Entry> simple_entries(int32_t count) const;
    void clear_simple();

    std::vector<std::unique_ptr<std::vector<Node>>> segments_;
    std::unordered_map<int32_t, int32_t> pending_attack_nodes_;
    std::deque<SimpleSegment> simple_segments_;
    std::vector<SimpleSegment> simple_free_segments_;  // stack
    SimpleSegment simple_tail_segment_;
    int32_t simple_head_offset_ = 0;
    int32_t simple_tail_offset_ = 0;
    int32_t simple_peak_ = 0;
    int32_t next_fresh_ = 0;
    int32_t free_ = -1;
    int32_t head_ = -1;
    int32_t tail_ = -1;
    int32_t eligible_head_ = -1;
    int32_t eligible_tail_ = -1;
    int32_t eligible_count_ = 0;
    int32_t count_ = 0;
    bool note_index_enabled_ = false;
    MusicalNotePrioritySelector musical_priority_;
    std::vector<MusicalNotePriorityCandidate> musical_candidates_ = std::vector<MusicalNotePriorityCandidate>(16);
    std::vector<int32_t> musical_attack_nodes_ = std::vector<int32_t>(16);
    std::vector<int32_t> musical_release_nodes_ = std::vector<int32_t>(16);
};

// v1 ForwardDropQueue: forward-only admission over bounded service durations.
class ForwardDropQueue {
public:
    explicit ForwardDropQueue(int32_t capacity);
    int32_t occupancy() const { return static_cast<int32_t>(pending_service_.size()) + (busy_ ? 1 : 0); }
    int32_t maximum_occupancy() const { return maximum_occupancy_; }
    void advance(int64_t sourceMicroseconds);
    bool try_admit(int64_t sourceMicroseconds, int64_t serviceMicroseconds, bool safetyAdmission);

private:
    friend struct TraceAccess;
    int32_t capacity_;
    std::deque<int64_t> pending_service_;
    bool busy_ = false;
    int64_t completion_microseconds_ = 0;
    int32_t maximum_occupancy_ = 0;
};

enum class CompleteNoteEventKind : int32_t { Other = 0, NoteOn = 1, NoteOff = 2 };

// v1 CompleteNoteTracker: FIFO pairing of unmatched note occurrences per
// channel/key.
class CompleteNoteTracker {
public:
    struct NoteOffMatch {
        int32_t attack_index = -1;
        bool accepted = false;
        bool suppress = false;
        bool filtered = false;
    };

    static CompleteNoteEventKind classify(const EventView& midiEvent);
    static bool is_safety_control(const EventView& midiEvent);

    void record_note_on(const EventView& midiEvent, bool accepted, bool rejectedByCompleteNotePolicy)
    { record_note_on(midiEvent, -1, accepted, rejectedByCompleteNotePolicy); }
    void record_note_on(const EventView& midiEvent, int32_t attackIndex, bool accepted, bool rejectedByCompleteNotePolicy);
    void record_filtered_note_on(const EventView& midiEvent, int32_t attackIndex);
    bool should_suppress_note_off(const EventView& midiEvent) { return take_note_off(midiEvent).suppress; }
    NoteOffMatch take_note_off(const EventView& midiEvent);
    void mark_unsent_attack_evicted(int32_t attackIndex);
    void mark_unsent_attack_filtered(int32_t attackIndex);

private:
    friend struct TraceAccess;
    static constexpr uint8_t kAccepted = 1;
    static constexpr uint8_t kRejectedByCompleteNotePolicy = 2;
    static constexpr uint8_t kRejectedByOtherPolicy = 3;
    static constexpr uint8_t kRejectedBySourceFilter = 4;
    struct Occurrence {
        int32_t attack_index;
        uint8_t state;
    };
    static int slot(const EventView& midiEvent);

    // First-in first-out list of the unmatched occurrences of one channel/key. A vector with a head index replaces
    // std::deque (DD-075): same order, no block allocation per 64 entries, and the storage is reused once emptied.
    class SlotFifo {
    public:
        bool empty() const { return head_ == items_.size(); }
        const Occurrence& front() const { return items_[head_]; }
        void push_back(const Occurrence& value) { items_.push_back(value); }
        void pop_front()
        {
            if (++head_ == items_.size()) { items_.clear(); head_ = 0; }
            else if (head_ >= 1024 && head_ * 2 >= items_.size()) {
                items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(head_));
                head_ = 0;
            }
        }
        const Occurrence* begin() const { return items_.data() + head_; }
        const Occurrence* end() const { return items_.data() + items_.size(); }
    private:
        std::vector<Occurrence> items_;
        size_t head_ = 0;
    };

    // Three sets of attack indices (unmatched accepted, evicted while unmatched, filtered while unmatched) kept as
    // flag bytes in 64 KiB pages indexed by the attack index (DD-075): an index is a position in the song, so the
    // pages stay small, and no hash node is allocated per note. Negative indices are never members.
    class AttackFlags {
    public:
        static constexpr uint8_t kUnmatchedAccepted = 1;
        static constexpr uint8_t kEvicted = 2;
        static constexpr uint8_t kFiltered = 4;
        bool test(int32_t index, uint8_t flag) const
        {
            if (index < 0) return false;
            size_t page = static_cast<size_t>(index) >> kPageShift;
            return page < pages_.size() && pages_[page] && (pages_[page][index & kPageMask] & flag) != 0;
        }
        void set(int32_t index, uint8_t flag)
        {
            if (index < 0) return;
            size_t page = static_cast<size_t>(index) >> kPageShift;
            if (page >= pages_.size()) pages_.resize(page + 1);
            if (!pages_[page]) pages_[page].reset(new uint8_t[kPageSize]());
            uint8_t& cell = pages_[page][index & kPageMask];
            if ((cell & flag) == 0) { cell = static_cast<uint8_t>(cell | flag); counts_[slot(flag)]++; }
        }
        // True when the index was a member.
        bool clear(int32_t index, uint8_t flag)
        {
            if (!test(index, flag)) return false;
            uint8_t& cell = pages_[static_cast<size_t>(index) >> kPageShift][index & kPageMask];
            cell = static_cast<uint8_t>(cell & ~flag);
            counts_[slot(flag)]--;
            return true;
        }
        int64_t count(uint8_t flag) const { return counts_[slot(flag)]; }
        // Members in ascending order (for the model trace).
        std::vector<int32_t> members(uint8_t flag) const
        {
            std::vector<int32_t> result;
            for (size_t page = 0; page < pages_.size(); ++page) {
                if (!pages_[page]) continue;
                for (int32_t offset = 0; offset < kPageSize; ++offset)
                    if ((pages_[page][offset] & flag) != 0) result.push_back(static_cast<int32_t>((page << kPageShift) + offset));
            }
            return result;
        }
    private:
        static constexpr int kPageShift = 16;
        static constexpr int32_t kPageSize = 1 << kPageShift;
        static constexpr int32_t kPageMask = kPageSize - 1;
        static int slot(uint8_t flag) { return flag == kUnmatchedAccepted ? 0 : flag == kEvicted ? 1 : 2; }
        std::vector<std::unique_ptr<uint8_t[]>> pages_;
        int64_t counts_[3] = { 0, 0, 0 };
    };

    std::vector<SlotFifo> occurrences_ = std::vector<SlotFifo>(16 * 128);
    AttackFlags flags_;
};

} // namespace models
} // namespace midibn
