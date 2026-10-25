// Groups 5 and 6 (Milestone 3): v1 MusicalAttackRateLimiter.cs (Note start
// rate gate) and OutputRateGate.cs (Output rate gate). Musical priority, the
// rest of group 5, is in priority.h.
#pragma once

#include "midibn/models/priority.h"
#include "midibn/models/rate.h"

#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace midibn {
namespace models {

struct GateTraceAccess;

class MusicalAttackRateLimiter {
public:
    explicit MusicalAttackRateLimiter(int64_t attacksPerSecond);

    int64_t attacks_per_second() const { return rate_; }
    int32_t live_occurrence_count() const { return live_; }
    int32_t occurrence_segment_count() const { return static_cast<int32_t>(chunks_.size()); }
    int64_t available_starts(int64_t timestamp, int64_t maximum) { return pacer_.available_slots(timestamp, maximum); }

    void begin_source_tick(int64_t absoluteTick);
    void admit(const EventView& midiEvent) { tick_events_.push_back(midiEvent); }
    int32_t end_source_tick(std::vector<EventView>& emissions);
    void complete_source();
    void retire_channel(int32_t channel);
    int64_t take_rejected_attacks();
    int64_t take_suppressed_releases();

private:
    friend struct GateTraceAccess;
    static constexpr int kChannelCount = 16;
    static constexpr int kPitchCount = 128;
    static constexpr int kSlotCount = kChannelCount * kPitchCount;
    static constexpr int kChunkSize = 4096;
    static constexpr int kMaximumLiveOccurrences = 1000000;

    struct Occurrence {
        bool in_use = false;
        bool accepted = false;
        int32_t channel = 0;
        int32_t pitch = 0;
        int32_t previous = 0;
        int32_t next = 0;
        int32_t next_free = 0;
    };
    struct Candidate {
        int32_t tick_index = 0;
        int32_t pitch = 0;
        int32_t velocity = 0;
        int32_t track = 0;
        int32_t channel = 0;
        bool accepted = false;
    };

    void build_attack_priorities();
    void select_attacks(int64_t timeMicroseconds);
    void append_occurrence(int32_t channel, int32_t pitch, bool accepted);
    int32_t remove_oldest(int32_t channel, int32_t pitch);
    void free_node(int32_t node);
    Occurrence& get(int32_t node) { return (*chunks_[static_cast<size_t>(node / kChunkSize)])[static_cast<size_t>(node % kChunkSize)]; }
    const Occurrence& get(int32_t node) const { return (*chunks_[static_cast<size_t>(node / kChunkSize)])[static_cast<size_t>(node % kChunkSize)]; }
    static int32_t slot(int32_t channel, int32_t pitch) { return channel * kPitchCount + pitch; }

    int64_t rate_;
    std::vector<std::unique_ptr<std::vector<Occurrence>>> chunks_;
    std::array<int32_t, kSlotCount> head_{};
    std::array<int32_t, kSlotCount> tail_{};
    std::vector<EventView> tick_events_;
    std::vector<Candidate> candidates_;
    ProgressiveRatePacer pacer_;
    MusicalNotePrioritySelector musical_priority_;
    std::vector<MusicalNotePriorityCandidate> musical_candidates_ = std::vector<MusicalNotePriorityCandidate>(16);
    std::vector<int32_t> active_candidate_indices_ = std::vector<int32_t>(16);
    int32_t free_ = -1;
    int32_t next_unused_ = 0;
    int32_t live_ = 0;
    int32_t fairness_cursor_ = 0;
    int64_t rejected_attacks_ = 0;
    int64_t suppressed_releases_ = 0;
};

class OutputRateGate {
public:
    OutputRateGate(int64_t eventsPerSecond, int64_t clockUnitsPerSecond);

    int64_t events_per_second() const { return rate_; }
    int32_t live_occurrence_count() const { return live_; }
    int64_t available_events(int64_t timestamp) { return pacer_.available_slots(timestamp, rate_ == 0 ? kInt64Max : rate_); }
    int64_t available_events(int64_t timestamp, int64_t maximum) { return pacer_.available_slots(timestamp, maximum); }

    void begin_source_tick(int64_t absoluteTick);
    void admit(const EventView& midiEvent) { tick_events_.push_back(midiEvent); }
    int32_t end_source_tick(std::vector<EventView>& emissions, int64_t decisionTimestamp);
    // True only for a required admitted-note release that could not consume a slot.
    bool record_dispatch(int64_t timestamp, bool requiredRelease);
    bool record_exceptional_dispatch(int64_t timestamp);
    void complete_source();
    void retire_channel(int32_t channel);
    int64_t take_rejected_attacks();
    int64_t take_suppressed_releases();
    int64_t take_rejected_other_messages();
    static bool is_protected_traffic(const EventView& midiEvent);

private:
    friend struct GateTraceAccess;
    static constexpr int kChannelCount = 16;
    static constexpr int kPitchCount = 128;
    static constexpr int kSlotCount = kChannelCount * kPitchCount;
    static constexpr int kChunkSize = 4096;
    static constexpr int kMaximumLiveOccurrences = 1000000;

    enum class PlanKind : uint8_t { RequiredRelease, OrdinaryMessage, Suppressed, CandidateAttack, CandidateRelease };

    struct Occurrence {
        bool in_use = false;
        bool accepted = false;
        int32_t candidate_index = 0;
        int32_t channel = 0;
        int32_t pitch = 0;
        int32_t previous = 0;
        int32_t next = 0;
        int32_t next_free = 0;
    };
    struct Candidate {
        int32_t tick_index = 0;
        int32_t pitch = 0;
        int32_t velocity = 0;
        int32_t track = 0;
        int32_t channel = 0;
        int32_t message_cost = 0;
        int32_t occurrence_node = 0;
        bool accepted = false;
    };
    struct EventPlan {
        PlanKind kind = PlanKind::RequiredRelease;
        int32_t candidate_index = 0;
        bool accepted = false;
    };

    void build_plans();
    void select_attacks(int64_t decisionTimestamp);
    void reserve(int64_t decisionTimestamp, int64_t count);
    void queue_dispatch(bool requiredRelease);
    int32_t append_occurrence(int32_t channel, int32_t pitch, int32_t candidateIndex);
    bool remove_oldest(int32_t channel, int32_t pitch, Occurrence& occurrence);
    void clear_occurrences();
    void clear_slot(int32_t slotIndex);
    void free_node(int32_t node, bool cancelReservation);
    Occurrence& get(int32_t node) { return (*chunks_[static_cast<size_t>(node / kChunkSize)])[static_cast<size_t>(node % kChunkSize)]; }
    const Occurrence& get(int32_t node) const { return (*chunks_[static_cast<size_t>(node / kChunkSize)])[static_cast<size_t>(node % kChunkSize)]; }
    static int32_t slot(int32_t channel, int32_t pitch) { return channel * kPitchCount + pitch; }

    int64_t rate_;
    ProgressiveRatePacer pacer_;
    std::vector<std::unique_ptr<std::vector<Occurrence>>> chunks_;
    std::array<int32_t, kSlotCount> head_{};
    std::array<int32_t, kSlotCount> tail_{};
    std::vector<EventView> tick_events_;
    std::vector<EventPlan> plans_;
    std::vector<Candidate> candidates_;
    std::deque<bool> pending_required_release_;
    MusicalNotePrioritySelector musical_priority_;
    std::vector<MusicalNotePriorityCandidate> musical_candidates_ = std::vector<MusicalNotePriorityCandidate>(16);
    std::vector<int32_t> active_candidate_indices_ = std::vector<int32_t>(16);
    int32_t free_ = -1;
    int32_t next_unused_ = 0;
    int32_t live_ = 0;
    int32_t fairness_cursor_ = 0;
    int64_t rejected_attacks_ = 0;
    int64_t suppressed_releases_ = 0;
    int64_t rejected_other_messages_ = 0;
};

} // namespace models
} // namespace midibn
