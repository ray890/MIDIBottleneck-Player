#include "midibn/engine/channel_state.h"

#include <cstring>

namespace midibn {
namespace engine {

using models::EventView;

// ---------------------------------------------------------------- snapshots

MidiChannelSnapshot ChannelPlaybackSnapshot::unknown(int channel)
{
    MidiChannelSnapshot s;
    s.channel = channel;
    s.enabled = true;
    s.bank_msb = -1;
    s.bank_lsb = -1;
    s.program = -1;
    s.volume = -1;
    s.expression = -1;
    s.pan = -1;
    s.sustain = -1;
    s.pitch_bend = INT32_MIN;
    s.channel_pressure = -1;
    s.last_dispatched_microseconds = -1;
    return s;
}

std::shared_ptr<ChannelPlaybackSnapshot> ChannelPlaybackSnapshot::empty()
{
    std::shared_ptr<ChannelPlaybackSnapshot> result = std::make_shared<ChannelPlaybackSnapshot>();
    for (int i = 0; i < 16; ++i) result->channels[static_cast<size_t>(i)] = unknown(i);
    return result;
}

std::shared_ptr<ChannelPlaybackSnapshot> ChannelPlaybackSnapshot::with_statistics_reset() const
{
    std::shared_ptr<ChannelPlaybackSnapshot> copy = std::make_shared<ChannelPlaybackSnapshot>(*this);
    for (MidiChannelSnapshot& c : copy->channels) {
        c.sent_events = 0;
        c.dropped_events = 0;
        c.peak_keys_down = c.keys_down;
    }
    return copy;
}

// ---------------------------------------------------------------- tracker

ChannelStateTracker::ChannelStateTracker() : requested_full_reset_(0), requested_statistics_reset_(0)
{
    reset_all_direct();
}

void ChannelStateTracker::reset_all_direct()
{
    std::lock_guard<std::mutex> lock(mutex_);
    reset_all_locked();
}

void ChannelStateTracker::reset_all_locked()
{
    key_occurrences_.fill(0);
    for (int channel = 0; channel < 16; ++channel)
        channels_[static_cast<size_t>(channel)] = ChannelPlaybackSnapshot::unknown(channel);
    applied_full_reset_ = requested_full_reset_.load();
    applied_statistics_reset_ = requested_statistics_reset_.load();
}

void ChannelStateTracker::pause_boundary_direct()
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    key_occurrences_.fill(0);
    for (MidiChannelSnapshot& c : channels_) {
        c.keys_down = 0;
        c.historical_attribute_mask |= known_attribute_mask(c);
        if (c.last_dispatched_microseconds >= 0) c.last_position_historical = true;
    }
}

void ChannelStateTracker::seek_boundary_direct()
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    key_occurrences_.fill(0);
    for (MidiChannelSnapshot& c : channels_) {
        c.keys_down = 0;
        c.peak_keys_down = 0;
        c.sent_events = 0;
        c.historical_attribute_mask |= known_attribute_mask(c);
        if (c.last_dispatched_microseconds >= 0) c.last_position_historical = true;
    }
}

void ChannelStateTracker::panic_direct()
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    key_occurrences_.fill(0);
    for (MidiChannelSnapshot& c : channels_) {
        c.keys_down = 0;
        c.sustain = 0;
        c.historical_attribute_mask &= ~(1 << static_cast<int>(ChannelAttribute::Sustain));
    }
}

void ChannelStateTracker::silence_channel_direct(int channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    if (channel < 0 || channel >= 16) return;
    MidiChannelSnapshot state = channels_[static_cast<size_t>(channel)];
    clear_channel_keys(channel, state);
    state.sustain = 0;
    clear_historical(state, ChannelAttribute::Sustain);
    channels_[static_cast<size_t>(channel)] = state;
}

void ChannelStateTracker::mark_attribute_historical_direct(int channel, ChannelAttribute attribute)
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    if (channel < 0 || channel >= 16) return;
    MidiChannelSnapshot state = channels_[static_cast<size_t>(channel)];
    if ((known_attribute_mask(state) & (1 << static_cast<int>(attribute))) != 0)
        state.historical_attribute_mask |= 1 << static_cast<int>(attribute);
    channels_[static_cast<size_t>(channel)] = state;
}

void ChannelStateTracker::record_monitor_state_chase_applied(const EventView& midiEvent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    int channel = midiEvent.channel();
    if (channel < 0 || channel >= 16) return;
    MidiChannelSnapshot state = channels_[static_cast<size_t>(channel)];
    int status = midiEvent.status() & 0xF0;
    int first = midiEvent.data_length > 1 ? midiEvent.data_byte(1) : 0;
    int second = midiEvent.data_length > 2 ? midiEvent.data_byte(2) : 0;
    if (status == 0xB0) apply_controller(channel, first, second, state);
    else if (status == 0xC0) { state.program = first; clear_historical(state, ChannelAttribute::Program); }
    else if (status == 0xD0) { state.channel_pressure = first; clear_historical(state, ChannelAttribute::Aftertouch); }
    else if (status == 0xE0) {
        state.pitch_bend = ((second << 7) | first) - 8192;
        clear_historical(state, ChannelAttribute::PitchBend);
    }
    channels_[static_cast<size_t>(channel)] = state;
}

void ChannelStateTracker::record_successful(const EventView& midiEvent)
{
    SentRecord sent = sent_record(midiEvent);
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    apply_successful_locked(sent);
}

void ChannelStateTracker::record_successful_batch(const SentRecord* records, int count)
{
    if (count <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    for (int i = 0; i < count; ++i) apply_successful_locked(records[i]);
}

// The channel's state is changed in place (it used to be copied out and back for every message; DD-092).
void ChannelStateTracker::apply_successful_locked(const SentRecord& sent)
{
    int channel = sent.channel;
    if (channel < 0 || channel >= 16) return;
    MidiChannelSnapshot& state = channels_[static_cast<size_t>(channel)];
    state.sent_events++;
    state.last_dispatched_microseconds = sent.micros;
    state.last_position_historical = false;
    int status = sent.status & 0xF0;
    int first = sent.first;
    int second = sent.second;
    if (status == 0x90 && second > 0) note_on(channel, first, state);
    else if (status == 0x80 || status == 0x90) note_off(channel, first, state);
    else if (status == 0xB0) apply_controller(channel, first, second, state);
    else if (status == 0xC0) { state.program = first; clear_historical(state, ChannelAttribute::Program); }
    else if (status == 0xD0) { state.channel_pressure = first; clear_historical(state, ChannelAttribute::Aftertouch); }
    else if (status == 0xE0) {
        state.pitch_bend = ((second << 7) | first) - 8192;
        clear_historical(state, ChannelAttribute::PitchBend);
    }
}

void ChannelStateTracker::record_override_applied(int channel, ChannelAttribute attribute, int value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    if (channel < 0 || channel >= 16) return;
    MidiChannelSnapshot state = channels_[static_cast<size_t>(channel)];
    switch (attribute) {
    case ChannelAttribute::BankMsb: state.bank_msb = value; break;
    case ChannelAttribute::BankLsb: state.bank_lsb = value; break;
    case ChannelAttribute::Program: state.program = value; break;
    case ChannelAttribute::Volume: state.volume = value; break;
    case ChannelAttribute::Expression: state.expression = value; break;
    case ChannelAttribute::Pan: state.pan = value; break;
    case ChannelAttribute::Sustain: state.sustain = value; break;
    case ChannelAttribute::PitchBend: state.pitch_bend = value; break;
    case ChannelAttribute::Aftertouch: state.channel_pressure = value; break;
    }
    clear_historical(state, attribute);
    channels_[static_cast<size_t>(channel)] = state;
}

void ChannelStateTracker::record_dropped(const EventView& midiEvent)
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    int channel = midiEvent.channel();
    if (channel >= 0 && channel < 16) channels_[static_cast<size_t>(channel)].dropped_events++;
}

std::shared_ptr<ChannelPlaybackSnapshot> ChannelStateTracker::create_snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    apply_requests();
    std::shared_ptr<ChannelPlaybackSnapshot> snapshot = std::make_shared<ChannelPlaybackSnapshot>();
    snapshot->channels = channels_;
    return snapshot;
}

void ChannelStateTracker::apply_requests()
{
    int32_t full = requested_full_reset_.load();
    if (full != applied_full_reset_) {
        reset_all_locked();
        applied_full_reset_ = full;
    }
    int32_t statistics = requested_statistics_reset_.load();
    if (statistics != applied_statistics_reset_) {
        for (MidiChannelSnapshot& c : channels_) {
            c.sent_events = 0;
            c.dropped_events = 0;
            c.peak_keys_down = c.keys_down;
        }
        applied_statistics_reset_ = statistics;
    }
}

void ChannelStateTracker::note_on(int channel, int key, MidiChannelSnapshot& state)
{
    if (key < 0 || key > 127) return;
    size_t index = static_cast<size_t>(channel * 128 + key);
    if (key_occurrences_[index] == 0) state.keys_down++;
    if (key_occurrences_[index] != INT32_MAX) key_occurrences_[index]++;
    if (state.keys_down > state.peak_keys_down) state.peak_keys_down = state.keys_down;
}

void ChannelStateTracker::note_off(int channel, int key, MidiChannelSnapshot& state)
{
    if (key < 0 || key > 127) return;
    size_t index = static_cast<size_t>(channel * 128 + key);
    if (key_occurrences_[index] == 0) return;
    key_occurrences_[index]--;
    if (key_occurrences_[index] == 0 && state.keys_down > 0) state.keys_down--;
}

void ChannelStateTracker::apply_controller(int channel, int controller, int value, MidiChannelSnapshot& state)
{
    switch (controller) {
    case 0: state.bank_msb = value; clear_historical(state, ChannelAttribute::BankMsb); break;
    case 7: state.volume = value; clear_historical(state, ChannelAttribute::Volume); break;
    case 10: state.pan = value; clear_historical(state, ChannelAttribute::Pan); break;
    case 11: state.expression = value; clear_historical(state, ChannelAttribute::Expression); break;
    case 32: state.bank_lsb = value; clear_historical(state, ChannelAttribute::BankLsb); break;
    case 64: state.sustain = value >= 64 ? 1 : 0; clear_historical(state, ChannelAttribute::Sustain); break;
    case 120:
    case 123:
        clear_channel_keys(channel, state);
        break;
    case 121:
        state.expression = 127;
        state.pan = 64;
        state.sustain = 0;
        state.pitch_bend = 0;
        state.channel_pressure = 0;
        state.historical_attribute_mask = 0;
        break;
    default:
        break;
    }
}

void ChannelStateTracker::clear_channel_keys(int channel, MidiChannelSnapshot& state)
{
    for (int key = 0; key < 128; ++key) key_occurrences_[static_cast<size_t>(channel * 128 + key)] = 0;
    state.keys_down = 0;
}

void ChannelStateTracker::clear_historical(MidiChannelSnapshot& state, ChannelAttribute attribute)
{
    state.historical_attribute_mask &= ~(1 << static_cast<int>(attribute));
}

int ChannelStateTracker::known_attribute_mask(const MidiChannelSnapshot& state)
{
    int mask = 0;
    if (state.bank_msb >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::BankMsb);
    if (state.bank_lsb >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::BankLsb);
    if (state.program >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::Program);
    if (state.volume >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::Volume);
    if (state.expression >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::Expression);
    if (state.pan >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::Pan);
    if (state.sustain >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::Sustain);
    if (state.pitch_bend != INT32_MIN) mask |= 1 << static_cast<int>(ChannelAttribute::PitchBend);
    if (state.channel_pressure >= 0) mask |= 1 << static_cast<int>(ChannelAttribute::Aftertouch);
    return mask;
}

// ---------------------------------------------------------------- overrides

ChannelOverrideState::ChannelOverrideState() : any_overrides_(0), filter_generation_(0)
{
    for (std::atomic<int32_t>& v : values_) v.store(kAutoValue);
    pending_masks_.fill(0);
    for (std::atomic<int32_t>& m : status_masks_) m.store(0);
    for (std::atomic<int64_t>& s : suppressed_) s.store(0);
}

bool ChannelOverrideState::has_pending_output()
{
    std::lock_guard<std::mutex> lock(change_sync_);
    for (int32_t mask : pending_masks_)
        if (mask != 0) return true;
    return false;
}

int32_t ChannelOverrideState::get_value(int channel, ChannelAttribute attribute) const
{
    int i = index(channel, attribute);
    if (i < 0 || i >= 16 * kAttributeCount) models::throw_index_out_of_range();
    return values_[static_cast<size_t>(i)].load();
}

int32_t ChannelOverrideState::minimum(ChannelAttribute attribute)
{
    return attribute == ChannelAttribute::PitchBend ? -8192 : 0;
}

int32_t ChannelOverrideState::maximum(ChannelAttribute attribute)
{
    return attribute == ChannelAttribute::PitchBend ? 8191 : attribute == ChannelAttribute::Sustain ? 1 : 127;
}

bool ChannelOverrideState::set_value(int channel, ChannelAttribute attribute, int32_t value)
{
    // v1 Validate.
    if (channel < 0 || channel >= 16) models::throw_argument_out_of_range("channel");
    if (static_cast<int>(attribute) < 0 || static_cast<int>(attribute) >= kAttributeCount)
        models::throw_argument_out_of_range("attribute");
    if (value != kAutoValue && (value < minimum(attribute) || value > maximum(attribute)))
        models::throw_argument_out_of_range("value");
    std::lock_guard<std::mutex> lock(change_sync_);
    size_t i = static_cast<size_t>(index(channel, attribute));
    if (values_[i].load() == value) return false;
    values_[i].store(value);
    // Setting a value queues its forced message. Clearing the override cancels a message that was queued but not yet
    // sent; v1 left the queued bit set, so the ordered workers waited for a message that could never be sent and
    // playback froze with the CPU busy (register entry DD-006).
    if (value != kAutoValue) pending_masks_[static_cast<size_t>(channel)] |= 1 << static_cast<int>(attribute);
    else pending_masks_[static_cast<size_t>(channel)] &= ~(1 << static_cast<int>(attribute));
    recalculate_any_overrides_locked();
    filter_generation_.fetch_add(1);
    return true;
}

void ChannelOverrideState::clear()
{
    std::lock_guard<std::mutex> lock(change_sync_);
    for (std::atomic<int32_t>& v : values_) v.store(kAutoValue);
    pending_masks_.fill(0);
    for (std::atomic<int32_t>& m : status_masks_) m.store(0);
    for (std::atomic<int64_t>& s : suppressed_) s.store(0);
    any_overrides_.store(0);
    filter_generation_.fetch_add(1);
}

void ChannelOverrideState::reset_statistics()
{
    for (std::atomic<int64_t>& s : suppressed_) s.store(0);
}

void ChannelOverrideState::mark_all_forced_pending()
{
    if (!any_overrides()) return;
    std::lock_guard<std::mutex> lock(change_sync_);
    for (int channel = 0; channel < 16; ++channel) {
        int32_t mask = 0;
        for (int attribute = 0; attribute < kAttributeCount; ++attribute)
            if (values_[static_cast<size_t>(channel * kAttributeCount + attribute)].load() != kAutoValue)
                mask |= 1 << attribute;
        pending_masks_[static_cast<size_t>(channel)] |= mask;
    }
}

void ChannelOverrideState::mark_channel_forced_pending(int channel)
{
    if (!any_overrides() || channel < 0 || channel >= 16) return;
    std::lock_guard<std::mutex> lock(change_sync_);
    int32_t mask = 0;
    for (int attribute = 0; attribute < kAttributeCount; ++attribute)
        if (values_[static_cast<size_t>(channel * kAttributeCount + attribute)].load() != kAutoValue)
            mask |= 1 << attribute;
    pending_masks_[static_cast<size_t>(channel)] |= mask;
}

int32_t ChannelOverrideState::take_pending_mask(int channel)
{
    std::lock_guard<std::mutex> lock(change_sync_);
    int32_t mask = pending_masks_[static_cast<size_t>(channel)];
    pending_masks_[static_cast<size_t>(channel)] = 0;
    return mask;
}

void ChannelOverrideState::requeue(int channel, int32_t mask)
{
    if (mask == 0) return;
    std::lock_guard<std::mutex> lock(change_sync_);
    pending_masks_[static_cast<size_t>(channel)] |= mask;
}

bool ChannelOverrideState::conflicts_with_reset(int channel, ChannelAttribute attribute, int32_t resetValue,
    int32_t& forcedValue) const
{
    forcedValue = get_value(channel, attribute);
    return forcedValue != kAutoValue && forcedValue != resetValue;
}

bool ChannelOverrideState::should_suppress(const EventView& midiEvent, ChannelAttribute& attribute,
    int32_t& forcedValue) const
{
    attribute = ChannelAttribute::BankMsb;
    forcedValue = kAutoValue;
    if (!any_overrides() || !midiEvent.is_valid() || midiEvent.channel() < 0 || midiEvent.channel() >= 16) return false;
    int channel = midiEvent.channel();
    int status = midiEvent.status() & 0xF0;
    int statusBit = status == 0xB0 ? 1 : status == 0xC0 ? 2 : status == 0xE0 ? 4 : status == 0xD0 ? 8 : 0;
    if (statusBit == 0 || (status_masks_[static_cast<size_t>(channel)].load() & statusBit) == 0) return false;
    if (status == 0xB0 && midiEvent.data_length > 2 && midiEvent.data_byte(1) == 121) {
        if (conflicts_with_reset(channel, ChannelAttribute::Expression, 127, forcedValue)) { attribute = ChannelAttribute::Expression; return true; }
        if (conflicts_with_reset(channel, ChannelAttribute::Pan, 64, forcedValue)) { attribute = ChannelAttribute::Pan; return true; }
        if (conflicts_with_reset(channel, ChannelAttribute::Sustain, 0, forcedValue)) { attribute = ChannelAttribute::Sustain; return true; }
        if (conflicts_with_reset(channel, ChannelAttribute::PitchBend, 0, forcedValue)) { attribute = ChannelAttribute::PitchBend; return true; }
        if (conflicts_with_reset(channel, ChannelAttribute::Aftertouch, 0, forcedValue)) { attribute = ChannelAttribute::Aftertouch; return true; }
        return false;
    }
    int32_t sourceValue;
    if (!try_classify(midiEvent, attribute, sourceValue)) return false;
    forcedValue = get_value(channel, attribute);
    return forcedValue != kAutoValue && sourceValue != forcedValue;
}

void ChannelOverrideState::record_suppressed(int channel)
{
    if (channel >= 0 && channel < 16) suppressed_[static_cast<size_t>(channel)].fetch_add(1);
}

void ChannelOverrideState::apply_to_snapshot(ChannelPlaybackSnapshot& snapshot)
{
    for (int channel = 0; channel < 16; ++channel) {
        MidiChannelSnapshot state = snapshot.channels[static_cast<size_t>(channel)];
        state.override_suppressed_events = suppressed_[static_cast<size_t>(channel)].load();
        int32_t mask = 0;
        for (int attribute = 0; attribute < kAttributeCount; ++attribute) {
            int32_t value = get_value(channel, static_cast<ChannelAttribute>(attribute));
            if (value == kAutoValue) continue;
            mask |= 1 << attribute;
            switch (static_cast<ChannelAttribute>(attribute)) {
            case ChannelAttribute::BankMsb: state.forced_bank_msb = value; break;
            case ChannelAttribute::BankLsb: state.forced_bank_lsb = value; break;
            case ChannelAttribute::Program: state.forced_program = value; break;
            case ChannelAttribute::Volume: state.forced_volume = value; break;
            case ChannelAttribute::Expression: state.forced_expression = value; break;
            case ChannelAttribute::Pan: state.forced_pan = value; break;
            case ChannelAttribute::Sustain: state.forced_sustain = value; break;
            case ChannelAttribute::PitchBend: state.forced_pitch_bend = value; break;
            case ChannelAttribute::Aftertouch: state.forced_aftertouch = value; break;
            }
        }
        state.forced_attribute_mask = mask;
        {
            std::lock_guard<std::mutex> lock(change_sync_);
            state.pending_forced_attribute_mask = pending_masks_[static_cast<size_t>(channel)] & mask;
        }
        snapshot.channels[static_cast<size_t>(channel)] = state;
    }
}

ChannelOverrideValues ChannelOverrideState::values_snapshot() const
{
    ChannelOverrideValues result;
    for (int channel = 0; channel < 16; ++channel)
        for (int attribute = 0; attribute < kAttributeCount; ++attribute)
            result.set(channel, static_cast<ChannelAttribute>(attribute),
                values_[static_cast<size_t>(channel * kAttributeCount + attribute)].load());
    return result;
}

void ChannelOverrideState::recalculate_any_overrides_locked()
{
    bool any = false;
    for (int channel = 0; channel < 16; ++channel) {
        int32_t mask = 0;
        for (int attribute = 0; attribute < kAttributeCount; ++attribute) {
            if (values_[static_cast<size_t>(channel * kAttributeCount + attribute)].load() == kAutoValue) continue;
            any = true;
            ChannelAttribute kind = static_cast<ChannelAttribute>(attribute);
            if (kind == ChannelAttribute::Program) mask |= 2;
            else if (kind == ChannelAttribute::PitchBend) mask |= 4;
            else if (kind == ChannelAttribute::Aftertouch) mask |= 8;
            else mask |= 1;
        }
        status_masks_[static_cast<size_t>(channel)].store(mask);
    }
    any_overrides_.store(any ? 1 : 0);
}

EventView ChannelOverrideState::create_message(int channel, ChannelAttribute attribute, int32_t value,
    int32_t eventIndex, int64_t micros)
{
    uint8_t status;
    uint8_t first;
    uint8_t second;
    switch (attribute) {
    case ChannelAttribute::BankMsb: status = static_cast<uint8_t>(0xB0 | channel); first = 0; second = static_cast<uint8_t>(value); break;
    case ChannelAttribute::BankLsb: status = static_cast<uint8_t>(0xB0 | channel); first = 32; second = static_cast<uint8_t>(value); break;
    case ChannelAttribute::Program:
        status = static_cast<uint8_t>(0xC0 | channel);
        return EventView::create(0, micros, 0, EventKind::ProgramChange, channel, status, eventIndex,
            static_cast<uint8_t>(value), 0, 2);
    case ChannelAttribute::Volume: status = static_cast<uint8_t>(0xB0 | channel); first = 7; second = static_cast<uint8_t>(value); break;
    case ChannelAttribute::Expression: status = static_cast<uint8_t>(0xB0 | channel); first = 11; second = static_cast<uint8_t>(value); break;
    case ChannelAttribute::Pan: status = static_cast<uint8_t>(0xB0 | channel); first = 10; second = static_cast<uint8_t>(value); break;
    case ChannelAttribute::Sustain: status = static_cast<uint8_t>(0xB0 | channel); first = 64; second = static_cast<uint8_t>(value == 0 ? 0 : 127); break;
    case ChannelAttribute::PitchBend: {
        status = static_cast<uint8_t>(0xE0 | channel);
        int raw = value + 8192;
        return EventView::create(0, micros, 0, EventKind::PitchBend, channel, status, eventIndex,
            static_cast<uint8_t>(raw & 0x7F), static_cast<uint8_t>((raw >> 7) & 0x7F), 3);
    }
    case ChannelAttribute::Aftertouch:
        // Channel pressure generated by an override or chase carries the label SystemMessage, as in v1. Kept
        // on purpose: nothing reads the label (it is not sent or counted), but the chase digests of the traces
        // and the engine recordings print it, so changing it would only add differences to explain.
        status = static_cast<uint8_t>(0xD0 | channel);
        return EventView::create(0, micros, 0, EventKind::SystemMessage, channel, status, eventIndex,
            static_cast<uint8_t>(value), 0, 2);
    default:
        models::throw_argument_out_of_range("attribute");
    }
    return EventView::create(0, micros, 0, EventKind::ControlChange, channel, status, eventIndex, first, second, 3);
}

EventView ChannelOverrideState::create_controller_message(int channel, int controller, int value, int32_t eventIndex,
    int64_t micros)
{
    uint8_t status = static_cast<uint8_t>(0xB0 | channel);
    return EventView::create(0, micros, 0, EventKind::ControlChange, channel, status, eventIndex,
        static_cast<uint8_t>(controller), static_cast<uint8_t>(value), 3);
}

bool ChannelOverrideState::try_classify(const EventView& midiEvent, ChannelAttribute& attribute, int32_t& value)
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

// ---------------------------------------------------------------- routing

ChannelRoutingState::ChannelRoutingState() : disabled_mask_(0), filter_generation_(0)
{
    for (std::atomic<int64_t>& m : muted_filtered_) m.store(0);
    for (std::atomic<uint32_t>& g : disable_generation_) g.store(0);
}

bool ChannelRoutingState::is_enabled(int channel) const
{
    if (channel < 0 || channel >= 16) models::throw_argument_out_of_range("channel");
    return (disabled_mask_.load() & (1 << channel)) == 0;
}

bool ChannelRoutingState::set_enabled(int channel, bool enabled)
{
    if (channel < 0 || channel >= 16) models::throw_argument_out_of_range("channel");
    while (true) {
        int32_t previous = disabled_mask_.load();
        int32_t next = enabled ? previous & ~(1 << channel) : previous | (1 << channel);
        if (next == previous) return false;
        if (disabled_mask_.compare_exchange_strong(previous, next)) {
            if (!enabled) disable_generation_[static_cast<size_t>(channel)].fetch_add(1);
            filter_generation_.fetch_add(1);
            return true;
        }
    }
}

bool ChannelRoutingState::should_filter(const EventView& midiEvent) const
{
    int channel = !midiEvent.is_valid() ? -1 : midiEvent.channel();
    return channel >= 0 && channel < 16 && (disabled_mask_.load() & (1 << channel)) != 0;
}

int32_t ChannelRoutingState::disable_generation(int channel) const
{
    if (channel < 0 || channel >= 16) models::throw_argument_out_of_range("channel");
    return static_cast<int32_t>(disable_generation_[static_cast<size_t>(channel)].load());
}

void ChannelRoutingState::record_filtered(int channel)
{
    if (channel >= 0 && channel < 16) muted_filtered_[static_cast<size_t>(channel)].fetch_add(1);
}

void ChannelRoutingState::apply_to_snapshot(ChannelPlaybackSnapshot& snapshot) const
{
    int32_t disabled = disabled_mask_.load();
    for (int channel = 0; channel < 16; ++channel) {
        MidiChannelSnapshot& state = snapshot.channels[static_cast<size_t>(channel)];
        state.enabled = (disabled & (1 << channel)) == 0;
        state.muted_filtered_events = muted_filtered_[static_cast<size_t>(channel)].load();
    }
}

void ChannelRoutingState::reset_statistics()
{
    for (std::atomic<int64_t>& m : muted_filtered_) m.store(0);
}

void ChannelRoutingState::clear()
{
    disabled_mask_.store(0);
    filter_generation_.fetch_add(1);
    reset_statistics();
}

ChannelRouting ChannelRoutingState::routing_snapshot() const
{
    ChannelRouting routing;
    routing.disabled_mask = static_cast<uint16_t>(disabled_mask_.load() & 0xFFFF);
    return routing;
}

} // namespace engine
} // namespace midibn
