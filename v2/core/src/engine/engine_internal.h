// Private helpers shared by the engine's translation units.
#pragma once

#include "midibn/engine/playback_engine.h"
#include "midibn/user_messages.h"

#include <map>
#include <new>

namespace midibn {
namespace engine {

// v1 PlaybackEngine.FilteredSourceEvents: sparse segmented bits (32,768
// events per segment) recording exact source-filter decisions.
class EngineCore::FilteredSourceEvents {
public:
    bool any() const { return !segments_.empty(); }
    bool contains(int32_t eventIndex) const
    {
        std::map<int32_t, std::vector<uint32_t>>::const_iterator found = segments_.find(eventIndex >> kSegmentShift);
        if (found == segments_.end()) return false;
        int32_t within = eventIndex & (kSegmentEventCount - 1);
        return (found->second[static_cast<size_t>(within >> 5)] & (1u << (within & 31))) != 0;
    }
    void add(int32_t eventIndex)
    {
        std::vector<uint32_t>& words = segments_[eventIndex >> kSegmentShift];
        if (words.empty()) words.assign(kWordsPerSegment, 0u);
        int32_t within = eventIndex & (kSegmentEventCount - 1);
        words[static_cast<size_t>(within >> 5)] |= 1u << (within & 31);
    }

private:
    static constexpr int32_t kSegmentShift = 15;
    static constexpr int32_t kSegmentEventCount = 1 << kSegmentShift;
    static constexpr int32_t kWordsPerSegment = kSegmentEventCount / 32;
    std::map<int32_t, std::vector<uint32_t>> segments_;
};

// Runs `body`; a v1 exception it raises (EngineError, a model's
// ModelException, or an allocation failure) is returned in `error`.
template <typename Body>
bool EngineCore::capture(Body&& body, EngineError& error)
{
    try {
        body();
        return true;
    }
    catch (const EngineError& e) {
        error = e;
    }
    catch (const models::ModelException& e) {
        error = from_model(e);
    }
    catch (const std::bad_alloc&) {
        error = EngineError::make("OutOfMemoryException", messages::out_of_memory());
    }
    return false;
}

} // namespace engine
} // namespace midibn
