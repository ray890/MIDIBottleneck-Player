// Group 3 model (Milestone 3): v1 PerNoteIntervalGate.cs, the live,
// bounded-state pitch transition gate shared by Playback and Analysis.
#pragma once

#include "midibn/models/priority.h"

#include <array>
#include <memory>
#include <vector>

namespace midibn {
namespace models {

struct GateTraceAccess;

enum class PerNoteGateAdmission : int32_t { NotNote = 0, Accepted = 1, Filtered = 2 };

class PerNoteIntervalGate {
public:
    static constexpr int kPitchCount = 128;
    static constexpr int kChannelCount = 16;

    PerNoteIntervalGate(int64_t intervalMicroseconds, bool intervalPolyphonyLimitEnabled = false,
        int32_t intervalPolyphonyLimit = kPitchCount);

    int64_t interval_microseconds() const { return interval_microseconds_; }
    int64_t next_boundary_microseconds() const { return next_boundary_; }
    bool has_pending_transitions() const { return next_boundary_ != kInt64Max; }
    int32_t next_boundary_output_count() const;

    void begin_source_tick(int64_t absoluteTick);
    PerNoteGateAdmission admit(const EventView& midiEvent);
    void end_source_tick();
    // Emits at most one transition per pitch; `output` must hold 128 entries.
    int32_t emit_boundary(int64_t boundaryMicroseconds, EventView* output, int32_t outputLength);
    void complete_source(int64_t sourceEndMicroseconds);
    int32_t retire_channel(int32_t channel, int64_t nowMicroseconds);
    int64_t take_filtered_event_count();

    bool is_active(int32_t pitch) const { return pitch >= 0 && pitch < kPitchCount && output_down_[static_cast<size_t>(pitch)]; }
    int32_t owner_channel(int32_t pitch) const
    { return pitch < 0 || pitch >= kPitchCount ? -1 : output_channel_[static_cast<size_t>(pitch)]; }
    int32_t occurrence_segment_count() const { return static_cast<int32_t>(occurrence_chunks_.size()); }
    bool has_active_occurrence(int32_t track, int32_t channel, int32_t pitch) const;

    static bool is_better_candidate(const EventView& left, const EventView& right);
    static bool try_classify(const EventView& midiEvent, int32_t& pitch, bool& noteOn);

private:
    friend struct GateTraceAccess;
    static constexpr int kOccurrenceChunkSize = 4096;

    struct SourceOccurrence {
        bool in_use = false;
        bool ended = false;
        bool removed_by_routing = false;
        uint8_t disposition = 0;  // 0 undecided, 1 selected, 2 rejected
        int32_t track = 0;
        int32_t channel = 0;
        int32_t pitch = 0;
        int32_t next_active = 0;
        int32_t next_group = 0;
        int32_t next_free = 0;
        EventView note_on;
        EventView note_off;
    };
    template <typename T> using PitchArray = std::array<T, kPitchCount>;

    int32_t emit_boundary_without_polyphony_limit(int64_t boundaryMicroseconds, EventView* output);
    void sort_release_pitches(int32_t count);
    bool release_precedes(int32_t leftPitch, int32_t rightPitch) const;
    void emit_protected_release(int32_t pitch, int64_t boundaryMicroseconds, EventView* output, int32_t& count);
    void drop_pending_attack(int32_t pitch);
    int32_t find_best_selected_support(int32_t pitch);
    int32_t allocate_occurrence(const EventView& noteOn, int32_t pitch);
    SourceOccurrence& occurrence(int32_t node)
    { return (*occurrence_chunks_[static_cast<size_t>(node / kOccurrenceChunkSize)])[static_cast<size_t>(node % kOccurrenceChunkSize)]; }
    const SourceOccurrence& occurrence(int32_t node) const
    { return (*occurrence_chunks_[static_cast<size_t>(node / kOccurrenceChunkSize)])[static_cast<size_t>(node % kOccurrenceChunkSize)]; }
    int32_t match_occurrence(int32_t track, int32_t channel, int32_t pitch);
    void add_to_tick_group(int32_t pitch, int32_t node);
    void store_candidate(int32_t pitch, int32_t head, int32_t representative, int32_t members, int64_t boundary);
    void finalize_candidate(int32_t pitch);
    void reject_group(int32_t head);
    void discard_removed_group(int32_t head);
    void clear_candidate(int32_t pitch);
    int32_t find_best_group_member(int32_t head);
    bool is_better_representative(int32_t candidate, int32_t current) const;
    static bool is_later_source_event(const EventView& candidate, const EventView& current);
    void schedule_natural_release(int32_t pitch, const EventView& source, int64_t requestedMicroseconds);
    void free_occurrence(int32_t node);
    int64_t boundary_at_or_after(int64_t microseconds) const;
    int64_t add_interval(int64_t boundary) const;
    void recalculate_next_boundary();
    static int32_t key(int32_t channel, int32_t pitch) { return channel * kPitchCount + pitch; }
    static EventView create_note_off(const EventView& source, int32_t pitch, int32_t channel, int32_t velocity,
        int64_t boundaryMicroseconds);

    void arm(int32_t pitch) { armed_[pitch >> 5] |= static_cast<uint32_t>(1) << (pitch & 31); }

    // Pitch bit sets (bit n of word w = pitch 32 w + n), so the 128-pitch loops can visit only the pitches that can
    // have work (DD-075): armed_ = may hold a finite boundary (see recalculate_next_boundary); tick_touched_ = has a
    // note in the current source tick.
    uint32_t armed_[4] = { 0, 0, 0, 0 };
    uint32_t tick_touched_[4] = { 0, 0, 0, 0 };
    int64_t interval_microseconds_;
    int64_t max_boundary_quotient_ = 0;
    bool interval_polyphony_limit_enabled_;
    int32_t interval_polyphony_limit_;
    PitchArray<bool> output_down_{};
    PitchArray<int32_t> output_channel_{};
    PitchArray<int32_t> selected_support_count_{};
    PitchArray<int64_t> minimum_release_boundary_{};
    PitchArray<int64_t> next_attack_boundary_{};
    PitchArray<EventView> pending_attack_{};
    PitchArray<int64_t> attack_boundary_{};
    PitchArray<int64_t> preparatory_release_boundary_{};
    PitchArray<EventView> natural_release_{};
    PitchArray<int64_t> natural_release_boundary_{};
    PitchArray<int32_t> candidate_head_{};
    PitchArray<int32_t> candidate_representative_{};
    PitchArray<int32_t> candidate_members_{};
    PitchArray<int64_t> candidate_boundary_{};
    PitchArray<int32_t> tick_head_{};
    PitchArray<int32_t> tick_tail_{};
    PitchArray<int32_t> tick_representative_{};
    PitchArray<int32_t> tick_members_{};
    int64_t source_tick_ = kInt64Min;
    std::vector<std::unique_ptr<std::vector<SourceOccurrence>>> occurrence_chunks_;
    std::array<int32_t, kChannelCount * kPitchCount> active_head_{};
    std::array<int32_t, kChannelCount * kPitchCount> active_tail_{};
    int32_t free_occurrence_ = -1;
    int32_t next_unused_occurrence_ = 0;
    int64_t next_boundary_ = kInt64Max;
    int64_t filtered_events_ = 0;
    bool source_complete_ = false;
    PitchArray<uint8_t> boundary_kind_{};
    PitchArray<int32_t> release_pitches_{};
    PitchArray<int32_t> attack_pitches_{};
    PitchArray<int64_t> release_carry_order_{};
    int64_t next_release_carry_order_ = 1;
    MusicalNotePrioritySelector musical_priority_;
    PitchArray<MusicalNotePriorityCandidate> musical_candidates_{};
    int32_t musical_fairness_cursor_ = 0;
};

} // namespace models
} // namespace midibn
