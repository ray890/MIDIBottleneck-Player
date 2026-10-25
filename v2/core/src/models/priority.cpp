#include "midibn/models/priority.h"

namespace midibn {
namespace models {

int32_t MusicalNotePrioritySelector::select_least_important(const MusicalNotePriorityCandidate* candidates,
    int32_t capacity, int32_t count, int32_t fairnessCursor)
{
    if (candidates == nullptr) throw_argument_null("candidates");
    if (count <= 0 || count > capacity) throw_argument_out_of_range("count");
    if (static_cast<int32_t>(annotations_.size()) < count) annotations_.resize(static_cast<size_t>(count));
    lane_counts_.clear();
    tonal_chord_ranges_.clear();

    for (int32_t index = 0; index < count; index++) {
        const MusicalNotePriorityCandidate& candidate = candidates[index];
        lane_counts_[lane(candidate.track, candidate.channel)] += 1;
        // Zero-based channel 9 is percussion: excluded from the tonal range.
        if (candidate.channel == 9) continue;
        auto found = tonal_chord_ranges_.find(candidate.intended_microseconds);
        if (found == tonal_chord_ranges_.end())
            tonal_chord_ranges_[candidate.intended_microseconds] = PitchRange{ candidate.pitch, candidate.pitch };
        else {
            if (candidate.pitch < found->second.minimum) found->second.minimum = candidate.pitch;
            if (candidate.pitch > found->second.maximum) found->second.maximum = candidate.pitch;
        }
    }

    for (int32_t index = 0; index < count; index++) {
        const MusicalNotePriorityCandidate& candidate = candidates[index];
        bool boundary = false;
        if (candidate.channel != 9) {
            auto found = tonal_chord_ranges_.find(candidate.intended_microseconds);
            boundary = found != tonal_chord_ranges_.end() &&
                (candidate.pitch == found->second.minimum || candidate.pitch == found->second.maximum);
        }
        Annotation annotation;
        annotation.lane_representation = lane_counts_[lane(candidate.track, candidate.channel)];
        annotation.identity = identity(candidate.track, candidate.channel);
        annotation.chord_boundary = boundary;
        annotations_[static_cast<size_t>(index)] = annotation;
    }

    int32_t victim = 0;
    for (int32_t index = 1; index < count; index++)
        if (is_less_important(candidates[index], annotations_[static_cast<size_t>(index)], candidates[victim],
                annotations_[static_cast<size_t>(victim)], fairnessCursor)) victim = index;
    return victim;
}

int32_t MusicalNotePrioritySelector::next_fairness_cursor(const MusicalNotePriorityCandidate& candidate) const
{
    int32_t id = identity(candidate.track, candidate.channel);
    return id == kInt32Max ? 0 : id + 1;
}

bool MusicalNotePrioritySelector::is_less_important(const MusicalNotePriorityCandidate& candidate,
    const Annotation& candidateAnnotation, const MusicalNotePriorityCandidate& current,
    const Annotation& currentAnnotation, int32_t fairnessCursor)
{
    if (candidateAnnotation.chord_boundary != currentAnnotation.chord_boundary)
        return !candidateAnnotation.chord_boundary;
    if (candidate.velocity != current.velocity) return candidate.velocity < current.velocity;
    if (candidate.duration_microseconds != current.duration_microseconds)
        return candidate.duration_microseconds < current.duration_microseconds;
    if (candidateAnnotation.lane_representation != currentAnnotation.lane_representation)
        return candidateAnnotation.lane_representation > currentAnnotation.lane_representation;
    int64_t candidateDistance = fairness_distance(candidateAnnotation.identity, fairnessCursor);
    int64_t currentDistance = fairness_distance(currentAnnotation.identity, fairnessCursor);
    if (candidateDistance != currentDistance) return candidateDistance < currentDistance;
    return candidate.source_order > current.source_order;
}

int64_t MusicalNotePrioritySelector::lane(int32_t track, int32_t channel)
{
    // ((long)track << 8) ^ (uint)(channel + 1)
    int64_t shifted = static_cast<int64_t>(static_cast<uint64_t>(static_cast<int64_t>(track)) << 8);
    int64_t low = static_cast<int64_t>(static_cast<uint32_t>(static_cast<int32_t>(
        static_cast<uint32_t>(channel) + 1u)));
    return shifted ^ low;
}

int32_t MusicalNotePrioritySelector::identity(int32_t track, int32_t channel)
{
    // unchecked(track * 397 ^ (channel < 0 ? 0 : channel)) & Int32.MaxValue
    return (wrap_mul32(track, 397) ^ (channel < 0 ? 0 : channel)) & kInt32Max;
}

int64_t MusicalNotePrioritySelector::fairness_distance(int32_t identityValue, int32_t cursor)
{
    const int64_t range = static_cast<int64_t>(kInt32Max) + 1;
    int64_t distance = static_cast<int64_t>(identityValue) - static_cast<int64_t>(cursor);
    return distance < 0 ? distance + range : distance;
}

} // namespace models
} // namespace midibn
