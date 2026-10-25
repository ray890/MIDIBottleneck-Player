// Group 4 model (Milestone 3): v1 DelayedNoteOffGate.cs, the shared
// live/offline Delayed NoteOff state machine (global ownership per pitch).
#pragma once

#include "midibn/models/priority.h"

#include <array>
#include <memory>
#include <vector>

namespace midibn {
namespace models {

struct GateTraceAccess;

enum class DelayedNoteOffAdmission : int32_t { NotNote = 0, Accepted = 1 };

class DelayedNoteOffGate {
public:
    static constexpr int kChannelCount = 16;
    static constexpr int kPitchCount = 128;

    DelayedNoteOffGate(int64_t minimumActiveMicroseconds, bool intervalPolyphonyLimitEnabled = false,
        int32_t intervalPolyphonyLimit = kPitchCount);

    int64_t minimum_active_microseconds() const { return minimum_active_microseconds_; }
    int64_t recovery_microseconds() const { return recovery_microseconds_; }
    int64_t next_boundary_microseconds() const { return next_boundary_; }
    bool has_pending_transitions() const { return next_boundary_ != kInt64Max; }
    int32_t next_boundary_output_count() const;
    int32_t live_occurrence_count() const { return live_occurrences_; }
    int32_t occurrence_segment_count() const { return static_cast<int32_t>(occurrence_chunks_.size()); }

    int64_t take_filtered_event_count();
    bool source_precedes_boundary(const EventView& midiEvent) const
    { return source_precedes_boundary(midiEvent, next_boundary_); }
    bool source_precedes_boundary(const EventView& midiEvent, int64_t effectiveBoundaryMicroseconds) const;
    DelayedNoteOffAdmission admit(const EventView& midiEvent);
    int32_t emit_boundary(int64_t boundaryMicroseconds, std::vector<EventView>& emissions);
    void complete_source(int64_t sourceEndMicroseconds);
    int32_t retire_channel(int32_t channel, int64_t nowMicroseconds);
    static bool is_note_terminating_control(const EventView& midiEvent);

private:
    friend struct GateTraceAccess;
    static constexpr int kMatchSlotCount = kChannelCount * kPitchCount;
    static constexpr int kOccurrenceChunkSize = 4096;
    static constexpr int kMaximumLiveOccurrences = 1000000;
    static constexpr int64_t kMaximumRecoveryMicroseconds = 40000;

    static constexpr uint8_t kIdle = 0;
    static constexpr uint8_t kAttackPending = 1;
    static constexpr uint8_t kHeld = 2;
    static constexpr uint8_t kRecovery = 3;

    static constexpr uint8_t kOwner = 1;
    static constexpr uint8_t kInsideDuration = 2;
    static constexpr uint8_t kRecoveryCandidate = 3;
    static constexpr uint8_t kRecoveryLost = 4;
    static constexpr uint8_t kRecoveryEnded = 5;
    static constexpr uint8_t kDisplacedOwner = 6;

    static constexpr uint8_t kTransitionAttack = 1;
    static constexpr uint8_t kTransitionRelease = 2;
    static constexpr uint8_t kTransitionRecovery = 3;

    struct SourceOccurrence {
        bool in_use = false;
        bool key_released = false;
        bool sustain_held = false;
        uint8_t disposition = 0;
        int32_t track = 0;
        int32_t channel = 0;
        int32_t pitch = 0;
        int32_t previous_active = 0;
        int32_t next_active = 0;
        int32_t next_free = 0;
        int64_t order = 0;
        EventView note_on;
        EventView note_off;
    };
    struct DueTransition {
        int32_t pitch = 0;
        uint8_t kind = 0;
        int64_t order = 0;
    };
    template <typename T> using PitchArray = std::array<T, kPitchCount>;

    EventView attack_for_transition(const DueTransition& transition) const;
    int64_t duration_for_transition(const DueTransition& transition) const;
    void reject_due_attack(const DueTransition& transition);
    void admit_attack(int32_t pitch, const EventView& midiEvent, int64_t order);
    void select_pending_attack_owner(int32_t pitch, const EventView& midiEvent, int64_t order);
    void admit_release(int32_t pitch, const EventView& midiEvent, int64_t order);
    void resolve_supported_release(int32_t node, int64_t releaseTime);
    void select_recovery_candidate(int32_t pitch, int32_t candidate);
    void reject_recovery_candidate(int32_t node, uint8_t reason);
    void schedule_owner_release(int32_t pitch, const EventView& source, int64_t requested, bool retrigger);
    void emit_attack(int32_t pitch, int64_t boundary, std::vector<EventView>& emissions);
    void emit_release(int32_t pitch, int64_t boundary, std::vector<EventView>& emissions);
    void emit_recovered_attack(int32_t pitch, int64_t boundary, std::vector<EventView>& emissions);
    void collect_due_transitions(int64_t boundary);
    int64_t candidate_order(int32_t pitch) const;
    void observe_sustain(const EventView& midiEvent);
    static bool is_sustain_controller(const EventView& midiEvent);
    void recalculate_next_boundary();
    int32_t allocate_occurrence(const EventView& noteOn, int32_t pitch, uint8_t disposition, int64_t order);
    int32_t match_occurrence(int32_t track, int32_t channel, int32_t pitch) const;
    void append_active(int32_t node);
    void remove_and_free(int32_t node);
    void count_filtered(uint8_t reason, int64_t count);
    SourceOccurrence& occurrence(int32_t node)
    { return (*occurrence_chunks_[static_cast<size_t>(node / kOccurrenceChunkSize)])[static_cast<size_t>(node % kOccurrenceChunkSize)]; }
    const SourceOccurrence& occurrence(int32_t node) const
    { return (*occurrence_chunks_[static_cast<size_t>(node / kOccurrenceChunkSize)])[static_cast<size_t>(node % kOccurrenceChunkSize)]; }
    static bool compare_transitions(const DueTransition& left, const DueTransition& right);
    static bool compare_legacy_transitions(const DueTransition& left, const DueTransition& right);
    static int32_t match_slot(int32_t channel, int32_t pitch) { return channel * kPitchCount + pitch; }
    static int64_t add_saturated(int64_t value, int64_t increment);
    static EventView create_release(const EventView& source, int32_t pitch, int32_t channel, int64_t boundaryMicroseconds);

    int64_t minimum_active_microseconds_;
    int64_t recovery_microseconds_;
    bool interval_polyphony_limit_enabled_;
    int32_t interval_polyphony_limit_;
    std::vector<std::unique_ptr<std::vector<SourceOccurrence>>> occurrence_chunks_;
    std::array<int32_t, kMatchSlotCount> active_head_{};
    std::array<int32_t, kMatchSlotCount> active_tail_{};
    std::array<bool, kChannelCount> sustain_down_{};
    PitchArray<uint8_t> state_{};
    PitchArray<int32_t> owner_occurrence_{};
    PitchArray<int32_t> candidate_occurrence_{};
    PitchArray<EventView> attack_{};
    PitchArray<int64_t> attack_due_{};
    PitchArray<int64_t> attack_emission_{};
    PitchArray<int64_t> attack_order_{};
    PitchArray<bool> release_pending_{};
    PitchArray<bool> release_for_retrigger_{};
    PitchArray<EventView> release_{};
    PitchArray<int64_t> release_due_{};
    PitchArray<int64_t> release_order_{};
    PitchArray<int64_t> recovery_due_{};
    PitchArray<int64_t> recovery_order_{};
    std::vector<DueTransition> due_transitions_;
    PitchArray<DueTransition> attack_transitions_{};
    PitchArray<MusicalNotePriorityCandidate> musical_candidates_{};
    MusicalNotePrioritySelector musical_priority_;
    PitchArray<bool> selected_attack_pitch_{};
    int32_t musical_fairness_cursor_ = 0;
    int32_t free_occurrence_ = -1;
    int32_t next_unused_occurrence_ = 0;
    int32_t live_occurrences_ = 0;
    int64_t next_boundary_ = kInt64Max;
    int64_t source_order_ = 0;
    int64_t filtered_events_ = 0;
    int64_t inside_duration_filtered_ = 0;
    int64_t recovery_lost_filtered_ = 0;
    int64_t recovery_ended_filtered_ = 0;
    bool source_complete_ = false;
};

} // namespace models
} // namespace midibn
