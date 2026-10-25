// Port of v1 ChannelOverrides.cs (AnalysisOverrideSnapshot, TryClassify).
#include "midibn/overrides/override_snapshot.h"

namespace midibn {
namespace overrides {

using models::EventView;

bool try_classify(const EventView& midiEvent, ChannelAttribute& attribute, int32_t& value)
{
    attribute = ChannelAttribute::BankMsb;
    value = 0;
    int status = midiEvent.status() & 0xF0;
    int first = midiEvent.data_length > 1 ? midiEvent.data_byte(1) : 0;
    int second = midiEvent.data_length > 2 ? midiEvent.data_byte(2) : 0;
    if (status == 0xC0) { attribute = ChannelAttribute::Program; value = first; return true; }
    if (status == 0xD0) { attribute = ChannelAttribute::Aftertouch; value = first; return true; }
    if (status == 0xE0) { attribute = ChannelAttribute::PitchBend; value = ((second << 7) | first) - 8192; return true; }
    if (status != 0xB0) return false;
    switch (first) {
    case 0: attribute = ChannelAttribute::BankMsb; value = second; return true;
    case 32: attribute = ChannelAttribute::BankLsb; value = second; return true;
    case 7: attribute = ChannelAttribute::Volume; value = second; return true;
    case 11: attribute = ChannelAttribute::Expression; value = second; return true;
    case 10: attribute = ChannelAttribute::Pan; value = second; return true;
    case 64: attribute = ChannelAttribute::Sustain; value = second >= 64 ? 1 : 0; return true;
    default: return false;
    }
}

AnalysisOverrideSnapshot::AnalysisOverrideSnapshot(const std::vector<int32_t>& values)
{
    if (values.size() != static_cast<size_t>(16 * kAttributeCount))
        models::throw_argument("values", "A complete 16-channel override snapshot is required.");
    values_ = values;
    int32_t count = 0;
    for (int channel = 0; channel < 16; ++channel) {
        for (int attribute = 0; attribute < kAttributeCount; ++attribute) {
            int32_t value = values_[static_cast<size_t>(channel * kAttributeCount + attribute)];
            if (value == kAutoValue) continue;
            if (!signature_.empty()) signature_ += ';';
            signature_ += std::to_string(channel) + '.' + std::to_string(attribute) + '=' + std::to_string(value);
            ++count;
        }
    }
    if (signature_.empty()) signature_ = "none";
    forced_message_count_ = count;
}

int32_t AnalysisOverrideSnapshot::get_value(int channel, ChannelAttribute attribute) const
{
    if (channel < 0 || channel >= 16) models::throw_argument_out_of_range("channel");
    return values_[static_cast<size_t>(channel * kAttributeCount + static_cast<int>(attribute))];
}

bool AnalysisOverrideSnapshot::should_suppress(const EventView& midiEvent) const
{
    if (!any_overrides() || !midiEvent.is_valid() || midiEvent.channel() < 0 || midiEvent.channel() >= 16)
        return false;
    if ((midiEvent.status() & 0xF0) == 0xB0 && midiEvent.data_length > 2 && midiEvent.data_byte(1) == 121) {
        int channel = midiEvent.channel();
        return conflicts_with_reset(channel, ChannelAttribute::Expression, 127) ||
            conflicts_with_reset(channel, ChannelAttribute::Pan, 64) ||
            conflicts_with_reset(channel, ChannelAttribute::Sustain, 0) ||
            conflicts_with_reset(channel, ChannelAttribute::PitchBend, 0) ||
            conflicts_with_reset(channel, ChannelAttribute::Aftertouch, 0);
    }
    ChannelAttribute attribute;
    int32_t sourceValue;
    if (!try_classify(midiEvent, attribute, sourceValue)) return false;
    int32_t forcedValue = get_value(midiEvent.channel(), attribute);
    return forcedValue != kAutoValue && forcedValue != sourceValue;
}

bool AnalysisOverrideSnapshot::conflicts_with_reset(int channel, ChannelAttribute attribute, int32_t resetValue) const
{
    int32_t value = get_value(channel, attribute);
    return value != kAutoValue && value != resetValue;
}

} // namespace overrides
} // namespace midibn
