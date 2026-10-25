// v1 MusicalNotePriority.cs: deterministic ranking shared by queue
// overflow, the interval gates and the rate gates. Ported with group 2
// because the complete-note queues depend on it.
#pragma once

#include "midibn/models/model_base.h"

#include <unordered_map>
#include <vector>

namespace midibn {
namespace models {

struct MusicalNotePriorityCandidate {
    int32_t pitch = 0;
    int32_t velocity = 0;
    int32_t track = 0;
    int32_t channel = 0;
    int32_t source_order = 0;
    int64_t intended_microseconds = 0;
    int64_t duration_microseconds = 0;
};

class MusicalNotePrioritySelector {
public:
    // `capacity` is the length of v1's candidate array (count must not exceed it).
    int32_t select_least_important(const MusicalNotePriorityCandidate* candidates, int32_t capacity, int32_t count,
        int32_t fairnessCursor);
    int32_t next_fairness_cursor(const MusicalNotePriorityCandidate& candidate) const;

    static int64_t lane(int32_t track, int32_t channel);
    static int32_t identity(int32_t track, int32_t channel);
    static int64_t fairness_distance(int32_t identity, int32_t cursor);

private:
    struct Annotation {
        int32_t lane_representation = 0;
        int32_t identity = 0;
        bool chord_boundary = false;
    };
    struct PitchRange {
        int32_t minimum = 0;
        int32_t maximum = 0;
    };

    static bool is_less_important(const MusicalNotePriorityCandidate& candidate, const Annotation& candidateAnnotation,
        const MusicalNotePriorityCandidate& current, const Annotation& currentAnnotation, int32_t fairnessCursor);

    std::unordered_map<int64_t, int32_t> lane_counts_;
    std::unordered_map<int64_t, PitchRange> tonal_chord_ranges_;
    std::vector<Annotation> annotations_;
};

} // namespace models
} // namespace midibn
