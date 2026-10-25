// Channel override classification and the immutable override snapshot used
// by the Analysis forced-override projection (v1 ChannelOverrides.cs:
// AnalysisOverrideSnapshot and ChannelOverrideState.TryClassify). The live
// override state used by playback comes with the engine (Milestone 5).
#pragma once

#include "midibn/models/model_base.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace overrides {

enum class ChannelAttribute : int32_t {
    BankMsb = 0,
    BankLsb = 1,
    Program = 2,
    Volume = 3,
    Expression = 4,
    Pan = 5,
    Sustain = 6,
    PitchBend = 7,
    Aftertouch = 8
};

const int kAttributeCount = 9;
const int32_t kAutoValue = (std::numeric_limits<int32_t>::min)();

// v1 ChannelOverrideState.TryClassify.
bool try_classify(const models::EventView& midiEvent, ChannelAttribute& attribute, int32_t& value);

class AnalysisOverrideSnapshot {
public:
    // `values` holds 16 channels x 9 attributes (kAutoValue = not forced);
    // any other length throws ArgumentException, as v1.
    explicit AnalysisOverrideSnapshot(const std::vector<int32_t>& values);

    const std::string& signature() const { return signature_; }
    int32_t forced_message_count() const { return forced_message_count_; }
    bool any_overrides() const { return forced_message_count_ != 0; }
    int32_t get_value(int channel, ChannelAttribute attribute) const;
    bool should_suppress(const models::EventView& midiEvent) const;

private:
    bool conflicts_with_reset(int channel, ChannelAttribute attribute, int32_t resetValue) const;

    std::vector<int32_t> values_;
    std::string signature_;
    int32_t forced_message_count_ = 0;
};

} // namespace overrides
} // namespace midibn
