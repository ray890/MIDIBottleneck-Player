// The pending queue the Analysis projection uses (P13-ANA). It behaves exactly like models::PendingMidiQueue in its
// note-index mode (same order, same evictions, same fairness cursor), but answers "which complete note is the least
// important" from an ordered index instead of ranking every queued note again for every incoming event. The live
// engine keeps using models::PendingMidiQueue unchanged; this class exists only for the Analysis, where a queue of a
// few thousand notes and millions of events made the old ranking take about twenty minutes.
//
// How the answer stays identical: MusicalNotePrioritySelector orders the queued notes by
//   (1) not a chord's lowest/highest pitch before one that is, (2) lower velocity, (3) shorter duration (a note
//   whose release is not queued counts as longest), (4) the lane with more queued notes, (5) the lane whose identity
//   comes first after the fairness cursor, (6) the later source order.
// Keys 1 to 3 are kept in an ordered set (updated when a note enters or leaves the queue, when its release arrives or
// leaves, and when its chord's lowest or highest pitch changes); within the first (1,2,3) group the set also groups by
// lane, with the latest source order first, so only one note per lane has to be compared on keys 4 to 6.
#pragma once

#include "midibn/models/model_base.h"
#include "midibn/models/priority.h"
#include "midibn/models/queue.h"

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace midibn {
namespace analysis {

class FastPendingQueue {
public:
    using Entry = models::PendingMidiQueue::Entry;

    // `events` must outlive the queue.
    // `noteIndexEnabled` as in PendingMidiQueue: only the complete-note policies need the note index.
    // `rankingIndexEnabled` builds the ordered index that try_evict_least_important_complete_note needs (only the
    // Drop background notes policy uses it; the other policies skip its upkeep).
    FastPendingQueue(const models::EventSource& events, bool noteIndexEnabled, bool rankingIndexEnabled)
        : events_(events), note_index_enabled_(noteIndexEnabled),
          ranking_index_enabled_(noteIndexEnabled && rankingIndexEnabled) {}

    int32_t count() const { return count_; }
    int32_t oldest_event_index() const { return count_ == 0 ? -1 : nodes_[static_cast<size_t>(head_)].entry.event_index; }
    int32_t eligible_count() const { return eligible_count_; }

    void enqueue(const Entry& entry);
    int32_t dequeue() { return dequeue_entry().event_index; }
    Entry dequeue_entry();
    bool try_evict_oldest_complete_note(int32_t& attackIndex, int32_t& releaseIndex);
    bool try_evict_oldest_complete_note_at_head(int32_t& attackIndex, int32_t& releaseIndex);
    bool try_evict_least_important_complete_note(int32_t& fairnessCursor, int32_t& attackIndex, int32_t& releaseIndex);
    void clear();
    std::vector<int32_t> event_indices() const;

private:
    struct Node {
        Entry entry;
        int32_t previous = -1;
        int32_t next = -1;
        int32_t eligible_previous = -1;
        int32_t eligible_next = -1;
        int32_t paired_release_node = -1;
        int32_t free_next = -1;
        // Attack notes only.
        int64_t micros = 0;
        int64_t duration = 0;
        int32_t track = 0;
        int32_t source_order = 0;
        int32_t cell_previous = -1;
        int32_t cell_next = -1;
        int16_t channel = 0;
        uint8_t pitch = 0;
        uint8_t velocity = 0;
        bool boundary = false;
        bool indexed = false;
    };
    struct Key {
        int32_t boundary;
        int32_t velocity;
        int64_t duration;
        int64_t lane;
        int32_t source_order;   // later source order sorts first
        int32_t node;
    };
    struct KeyLess {
        bool operator()(const Key& a, const Key& b) const
        {
            if (a.boundary != b.boundary) return a.boundary < b.boundary;
            if (a.velocity != b.velocity) return a.velocity < b.velocity;
            if (a.duration != b.duration) return a.duration < b.duration;
            if (a.lane != b.lane) return a.lane < b.lane;
            if (a.source_order != b.source_order) return a.source_order > b.source_order;
            return a.node < b.node;
        }
    };
    // The notes of one chord (same intended time) that share a pitch.
    struct Cell {
        int32_t count = 0;
        int32_t head = -1;
        bool boundary = false;
    };
    struct Group {
        std::map<int32_t, Cell> cells;
    };

    static int64_t lane_of(const Node& n) { return models::MusicalNotePrioritySelector::lane(n.track, n.channel); }
    static Key key_of(const Node& n, int32_t id)
    {
        Key k;
        k.boundary = n.boundary ? 1 : 0;
        k.velocity = n.velocity;
        k.duration = n.duration;
        k.lane = lane_of(n);
        k.source_order = n.source_order;
        k.node = id;
        return k;
    }

    int32_t allocate();
    void remove(int32_t id, bool reindexAttack);
    void add_attack(int32_t id);
    void remove_attack(int32_t id);
    void index_insert(int32_t id);
    void index_erase(int32_t id);
    void refresh_group(Group& group, int32_t oldMin, int32_t oldMax, bool hadCells);
    void refresh_cell(Group& group, int32_t pitch, int32_t minPitch, int32_t maxPitch);

    models::EventSource events_;
    bool note_index_enabled_;
    bool ranking_index_enabled_;
    std::vector<Node> nodes_;
    std::unordered_map<int32_t, int32_t> pending_attack_nodes_;
    std::set<Key, KeyLess> index_;
    std::unordered_map<int64_t, int32_t> lane_counts_;
    std::unordered_map<int64_t, Group> groups_;
    int32_t free_ = -1;
    int32_t head_ = -1;
    int32_t tail_ = -1;
    int32_t eligible_head_ = -1;
    int32_t eligible_tail_ = -1;
    int32_t eligible_count_ = 0;
    int32_t count_ = 0;
};

// ------------------------------------------------------------------ implementation

inline int32_t FastPendingQueue::allocate()
{
    if (free_ >= 0) {
        int32_t id = free_;
        free_ = nodes_[static_cast<size_t>(id)].free_next;
        return id;
    }
    nodes_.emplace_back();
    return static_cast<int32_t>(nodes_.size()) - 1;
}

inline void FastPendingQueue::index_insert(int32_t id)
{
    Node& n = nodes_[static_cast<size_t>(id)];
    index_.insert(key_of(n, id));
    n.indexed = true;
}

inline void FastPendingQueue::index_erase(int32_t id)
{
    Node& n = nodes_[static_cast<size_t>(id)];
    index_.erase(key_of(n, id));
    n.indexed = false;
}

inline void FastPendingQueue::enqueue(const Entry& entry)
{
    if (note_index_enabled_ && entry.is_note_on &&
        pending_attack_nodes_.find(entry.event_index) != pending_attack_nodes_.end())
        models::throw_duplicate_key();
    int32_t id = allocate();
    {
        Node& fresh = nodes_[static_cast<size_t>(id)];
        fresh = Node();
        fresh.entry = entry;
        fresh.previous = tail_;
        fresh.next = -1;
    }
    if (tail_ >= 0) nodes_[static_cast<size_t>(tail_)].next = id;
    else head_ = id;
    tail_ = id;
    count_++;

    if (!note_index_enabled_) return;
    if (entry.is_note_on) {
        Node& n = nodes_[static_cast<size_t>(id)];
        n.eligible_previous = eligible_tail_;
        if (eligible_tail_ >= 0) nodes_[static_cast<size_t>(eligible_tail_)].eligible_next = id;
        else eligible_head_ = id;
        eligible_tail_ = id;
        eligible_count_++;
        pending_attack_nodes_.emplace(entry.event_index, id);
        if (ranking_index_enabled_) add_attack(id);
    }
    else if (entry.is_note_off && entry.paired_attack_index >= 0) {
        auto found = pending_attack_nodes_.find(entry.paired_attack_index);
        if (found != pending_attack_nodes_.end()) {
            Node& attack = nodes_[static_cast<size_t>(found->second)];
            attack.paired_release_node = id;
            if (ranking_index_enabled_) {
            int64_t releaseMicros = events_.micros_at(entry.event_index);
            int64_t duration = models::max64(0, models::wrap_sub(releaseMicros, attack.micros));
            if (duration != attack.duration) {
                index_erase(found->second);
                attack.duration = duration;
                index_insert(found->second);
            }
            }
        }
    }
}

inline FastPendingQueue::Entry FastPendingQueue::dequeue_entry()
{
    if (head_ < 0) models::throw_invalid_operation("The pending MIDI queue is empty.");
    int32_t id = head_;
    Entry entry = nodes_[static_cast<size_t>(id)].entry;
    remove(id, true);
    return entry;
}

inline bool FastPendingQueue::try_evict_oldest_complete_note(int32_t& attackIndex, int32_t& releaseIndex)
{
    if (!note_index_enabled_) models::throw_invalid_operation("The pending-note index has not been enabled.");
    if (eligible_head_ < 0) {
        attackIndex = -1;
        releaseIndex = -1;
        return false;
    }
    int32_t attackNode = eligible_head_;
    attackIndex = nodes_[static_cast<size_t>(attackNode)].entry.event_index;
    releaseIndex = -1;
    int32_t releaseNode = nodes_[static_cast<size_t>(attackNode)].paired_release_node;
    if (releaseNode >= 0) {
        releaseIndex = nodes_[static_cast<size_t>(releaseNode)].entry.event_index;
        remove(releaseNode, false);
    }
    remove(attackNode, true);
    return true;
}

inline bool FastPendingQueue::try_evict_oldest_complete_note_at_head(int32_t& attackIndex, int32_t& releaseIndex)
{
    if (!note_index_enabled_) models::throw_invalid_operation("The pending-note index has not been enabled.");
    if (eligible_head_ < 0 || eligible_head_ != head_) {
        attackIndex = -1;
        releaseIndex = -1;
        return false;
    }
    return try_evict_oldest_complete_note(attackIndex, releaseIndex);
}

inline bool FastPendingQueue::try_evict_least_important_complete_note(int32_t& fairnessCursor, int32_t& attackIndex,
    int32_t& releaseIndex)
{
    if (!ranking_index_enabled_) models::throw_invalid_operation("The pending-note index has not been enabled.");
    if (index_.empty()) {
        attackIndex = releaseIndex = -1;
        return false;
    }
    using models::MusicalNotePrioritySelector;
    // The first group of keys 1 to 3, then one representative (the latest source order) per lane.
    const Key first = *index_.begin();
    int32_t best = -1;
    int64_t bestRepresentation = 0;
    int64_t bestDistance = 0;
    int32_t bestOrder = 0;
    auto it = index_.begin();
    while (it != index_.end() && it->boundary == first.boundary && it->velocity == first.velocity &&
        it->duration == first.duration) {
        const Key key = *it;
        const Node& n = nodes_[static_cast<size_t>(key.node)];
        int64_t representation = lane_counts_[key.lane];
        int64_t distance = MusicalNotePrioritySelector::fairness_distance(
            MusicalNotePrioritySelector::identity(n.track, n.channel), fairnessCursor);
        bool less = best < 0;
        if (!less) {
            if (representation != bestRepresentation) less = representation > bestRepresentation;
            else if (distance != bestDistance) less = distance < bestDistance;
            else less = key.source_order > bestOrder;
        }
        if (less) {
            best = key.node;
            bestRepresentation = representation;
            bestDistance = distance;
            bestOrder = key.source_order;
        }
        if (key.lane == models::kInt64Max) break;
        Key next = key;
        next.lane = key.lane + 1;
        next.source_order = models::kInt32Max;
        next.node = -1;
        it = index_.lower_bound(next);
    }
    int32_t victimAttack = best;
    int32_t victimRelease = nodes_[static_cast<size_t>(victimAttack)].paired_release_node;
    attackIndex = nodes_[static_cast<size_t>(victimAttack)].entry.event_index;
    releaseIndex = victimRelease < 0 ? -1 : nodes_[static_cast<size_t>(victimRelease)].entry.event_index;
    int32_t identity = MusicalNotePrioritySelector::identity(nodes_[static_cast<size_t>(victimAttack)].track,
        nodes_[static_cast<size_t>(victimAttack)].channel);
    if (victimRelease >= 0) remove(victimRelease, false);
    remove(victimAttack, true);
    fairnessCursor = identity == models::kInt32Max ? 0 : identity + 1;
    return true;
}

inline void FastPendingQueue::clear()
{
    nodes_.clear();
    pending_attack_nodes_.clear();
    index_.clear();
    lane_counts_.clear();
    groups_.clear();
    free_ = head_ = tail_ = eligible_head_ = eligible_tail_ = -1;
    eligible_count_ = count_ = 0;
}

inline std::vector<int32_t> FastPendingQueue::event_indices() const
{
    std::vector<int32_t> result;
    for (int32_t id = head_; id >= 0; id = nodes_[static_cast<size_t>(id)].next)
        result.push_back(nodes_[static_cast<size_t>(id)].entry.event_index);
    return result;
}

inline void FastPendingQueue::remove(int32_t id, bool reindexAttack)
{
    Node& removed = nodes_[static_cast<size_t>(id)];
    if (removed.previous >= 0) nodes_[static_cast<size_t>(removed.previous)].next = removed.next;
    else head_ = removed.next;
    if (removed.next >= 0) nodes_[static_cast<size_t>(removed.next)].previous = removed.previous;
    else tail_ = removed.previous;

    if (!note_index_enabled_) {
        // fall through to the free list below
    }
    else if (removed.entry.is_note_on) {
        if (removed.eligible_previous >= 0)
            nodes_[static_cast<size_t>(removed.eligible_previous)].eligible_next = removed.eligible_next;
        else eligible_head_ = removed.eligible_next;
        if (removed.eligible_next >= 0)
            nodes_[static_cast<size_t>(removed.eligible_next)].eligible_previous = removed.eligible_previous;
        else eligible_tail_ = removed.eligible_previous;
        eligible_count_--;
        pending_attack_nodes_.erase(removed.entry.event_index);
        if (ranking_index_enabled_) remove_attack(id);
    }
    else if (removed.entry.is_note_off && removed.entry.paired_attack_index >= 0) {
        auto found = pending_attack_nodes_.find(removed.entry.paired_attack_index);
        if (found != pending_attack_nodes_.end() &&
            nodes_[static_cast<size_t>(found->second)].paired_release_node == id) {
            Node& attack = nodes_[static_cast<size_t>(found->second)];
            attack.paired_release_node = -1;
            if (ranking_index_enabled_ && reindexAttack && attack.duration != models::kInt64Max) {
                index_erase(found->second);
                attack.duration = models::kInt64Max;
                index_insert(found->second);
            }
        }
    }

    removed.free_next = free_;
    free_ = id;
    count_--;
}

inline void FastPendingQueue::add_attack(int32_t id)
{
    models::EventView view = events_.at(nodes_[static_cast<size_t>(id)].entry.event_index);
    Node& n = nodes_[static_cast<size_t>(id)];
    n.pitch = static_cast<uint8_t>(view.data_byte(1) & 0x7F);
    n.velocity = static_cast<uint8_t>(view.data_byte(2) & 0x7F);
    n.track = view.track;
    n.channel = static_cast<int16_t>(view.channel());
    n.source_order = view.event_index();
    n.micros = view.micros;
    n.duration = models::kInt64Max;
    n.boundary = false;
    lane_counts_[lane_of(n)] += 1;
    if (n.channel == 9) {   // percussion is outside the tonal range
        index_insert(id);
        return;
    }
    Group& group = groups_[n.micros];
    bool hadCells = !group.cells.empty();
    int32_t oldMin = hadCells ? group.cells.begin()->first : 0;
    int32_t oldMax = hadCells ? group.cells.rbegin()->first : 0;
    Cell& cell = group.cells[n.pitch];
    n.boundary = cell.boundary;
    n.cell_previous = -1;
    n.cell_next = cell.head;
    if (cell.head >= 0) nodes_[static_cast<size_t>(cell.head)].cell_previous = id;
    cell.head = id;
    cell.count++;
    index_insert(id);
    refresh_group(group, oldMin, oldMax, hadCells);
}

inline void FastPendingQueue::remove_attack(int32_t id)
{
    Node& n = nodes_[static_cast<size_t>(id)];
    int64_t lane = lane_of(n);
    auto laneFound = lane_counts_.find(lane);
    if (laneFound != lane_counts_.end() && --laneFound->second <= 0) lane_counts_.erase(laneFound);
    if (n.indexed) index_erase(id);
    if (n.channel == 9) return;
    auto groupFound = groups_.find(n.micros);
    Group& group = groupFound->second;
    int32_t oldMin = group.cells.begin()->first;
    int32_t oldMax = group.cells.rbegin()->first;
    auto cellFound = group.cells.find(n.pitch);
    Cell& cell = cellFound->second;
    if (n.cell_previous >= 0) nodes_[static_cast<size_t>(n.cell_previous)].cell_next = n.cell_next;
    else cell.head = n.cell_next;
    if (n.cell_next >= 0) nodes_[static_cast<size_t>(n.cell_next)].cell_previous = n.cell_previous;
    if (--cell.count == 0) group.cells.erase(cellFound);
    if (group.cells.empty()) {
        groups_.erase(groupFound);
        return;
    }
    refresh_group(group, oldMin, oldMax, true);
}

inline void FastPendingQueue::refresh_cell(Group& group, int32_t pitch, int32_t minPitch, int32_t maxPitch)
{
    auto found = group.cells.find(pitch);
    if (found == group.cells.end()) return;
    bool desired = pitch == minPitch || pitch == maxPitch;
    Cell& cell = found->second;
    if (cell.boundary == desired) return;
    cell.boundary = desired;
    for (int32_t id = cell.head; id >= 0; id = nodes_[static_cast<size_t>(id)].cell_next) {
        index_erase(id);
        nodes_[static_cast<size_t>(id)].boundary = desired;
        index_insert(id);
    }
}

inline void FastPendingQueue::refresh_group(Group& group, int32_t oldMin, int32_t oldMax, bool hadCells)
{
    int32_t newMin = group.cells.begin()->first;
    int32_t newMax = group.cells.rbegin()->first;
    refresh_cell(group, newMin, newMin, newMax);
    if (newMax != newMin) refresh_cell(group, newMax, newMin, newMax);
    if (hadCells) {
        if (oldMin != newMin && oldMin != newMax) refresh_cell(group, oldMin, newMin, newMax);
        if (oldMax != newMin && oldMax != newMax && oldMax != oldMin) refresh_cell(group, oldMax, newMin, newMax);
    }
}

} // namespace analysis
} // namespace midibn
