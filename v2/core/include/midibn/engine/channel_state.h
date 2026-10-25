// Live channel state used by the playback engine (Milestone 5, Group D):
// ports of v1 ChannelStateTracker.cs (MidiChannelSnapshot,
// ChannelPlaybackSnapshot, ChannelStateTracker), the live part of
// ChannelOverrides.cs (ChannelOverrideState) and ChannelRoutingState.cs.
// v1's Volatile/Interlocked fields are std::atomic; its `lock (_changeSync)`
// is a std::mutex. v1's ChannelStateTracker has no lock (it relies on the
// engine's ordering); this port adds a private leaf mutex so concurrent
// access is defined behaviour in C++ (no observable difference).
#pragma once

#include "midibn/channel_index.h"
#include "midibn/models/model_base.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace midibn {
namespace engine {

// v1 MidiChannelSnapshot.
struct MidiChannelSnapshot {
    int32_t channel = 0;
    int32_t keys_down = 0;
    int32_t peak_keys_down = 0;
    int64_t sent_events = 0;
    int64_t dropped_events = 0;
    int64_t override_suppressed_events = 0;
    int64_t muted_filtered_events = 0;
    bool enabled = false;
    int32_t bank_msb = 0;
    int32_t bank_lsb = 0;
    int32_t program = 0;
    int32_t volume = 0;
    int32_t expression = 0;
    int32_t pan = 0;
    int32_t sustain = 0;
    int32_t pitch_bend = 0;
    int32_t channel_pressure = 0;
    int64_t last_dispatched_microseconds = 0;
    int32_t historical_attribute_mask = 0;
    bool last_position_historical = false;
    int32_t forced_attribute_mask = 0;
    int32_t pending_forced_attribute_mask = 0;
    int32_t forced_bank_msb = 0;
    int32_t forced_bank_lsb = 0;
    int32_t forced_program = 0;
    int32_t forced_volume = 0;
    int32_t forced_expression = 0;
    int32_t forced_pan = 0;
    int32_t forced_sustain = 0;
    int32_t forced_pitch_bend = 0;
    int32_t forced_aftertouch = 0;
};

// v1 ChannelPlaybackSnapshot (16 channels).
struct ChannelPlaybackSnapshot {
    std::array<MidiChannelSnapshot, 16> channels;

    static std::shared_ptr<ChannelPlaybackSnapshot> empty();
    static MidiChannelSnapshot unknown(int channel);
    std::shared_ptr<ChannelPlaybackSnapshot> with_statistics_reset() const;
};

// v1 ChannelStateTracker.
class ChannelStateTracker {
public:
    // One successfully sent message, reduced to exactly what record_successful reads from it (register entry DD-092):
    // the channel the view reports (-1: not a channel message), its status byte, data bytes 1 and 2 (0 when the
    // message is shorter) and its intended time. The engine's immediate burst collects these for a group of sends and
    // records them under one lock (record_successful_batch) instead of locking the tracker once per message.
    struct SentRecord {
        int64_t micros;
        int32_t channel;
        uint8_t status;
        uint8_t first;
        uint8_t second;
    };
    static SentRecord sent_record(const models::EventView& midiEvent)
    {
        SentRecord r;
        r.micros = midiEvent.micros;
        r.channel = midiEvent.channel();
        r.status = midiEvent.status();
        r.first = midiEvent.data_length > 1 ? midiEvent.data_byte(1) : 0;
        r.second = midiEvent.data_length > 2 ? midiEvent.data_byte(2) : 0;
        return r;
    }
    // The same fields straight from a stored short message (not long, not SysEx), as EventView::from_store builds them.
    static SentRecord sent_record(const PackedRecord& record)
    {
        SentRecord r;
        const int length = record.inline_length();
        r.micros = record.micros;
        r.channel = record.channel();
        r.status = record.status();
        r.first = length > 1 ? static_cast<uint8_t>(record.packed >> 8) : 0;
        r.second = length > 2 ? static_cast<uint8_t>(record.packed >> 16) : 0;
        return r;
    }

    ChannelStateTracker();

    void request_full_reset() { requested_full_reset_.fetch_add(1); }
    void request_statistics_reset() { requested_statistics_reset_.fetch_add(1); }
    void reset_all_direct();
    void pause_boundary_direct();
    void seek_boundary_direct();
    void stop_boundary_direct() { reset_all_direct(); }
    void panic_direct();
    void silence_channel_direct(int channel);
    void mark_attribute_historical_direct(int channel, ChannelAttribute attribute);
    void record_manual_chase_applied(int channel, ChannelAttribute attribute, int value)
    { record_override_applied(channel, attribute, value); }
    void record_monitor_state_chase_applied(const models::EventView& midiEvent);
    void record_successful(const models::EventView& midiEvent);
    // record_successful for `count` sends in order, under one lock (DD-092).
    void record_successful_batch(const SentRecord* records, int count);
    void record_override_applied(int channel, ChannelAttribute attribute, int value);
    void record_dropped(const models::EventView& midiEvent);
    std::shared_ptr<ChannelPlaybackSnapshot> create_snapshot();

private:
    void reset_all_locked();
    void apply_requests();
    void apply_successful_locked(const SentRecord& sent);
    void note_on(int channel, int key, MidiChannelSnapshot& state);
    void note_off(int channel, int key, MidiChannelSnapshot& state);
    void apply_controller(int channel, int controller, int value, MidiChannelSnapshot& state);
    void clear_channel_keys(int channel, MidiChannelSnapshot& state);
    static void clear_historical(MidiChannelSnapshot& state, ChannelAttribute attribute);
    static int known_attribute_mask(const MidiChannelSnapshot& state);

    std::mutex mutex_;
    std::array<MidiChannelSnapshot, 16> channels_;
    std::array<int32_t, 16 * 128> key_occurrences_;
    std::atomic<int32_t> requested_full_reset_;
    int32_t applied_full_reset_ = 0;
    std::atomic<int32_t> requested_statistics_reset_;
    int32_t applied_statistics_reset_ = 0;
};

// v1 ChannelOverrideState (live forced channel values).
class ChannelOverrideState {
public:
    static constexpr int kAttributeCount = 9;
    static constexpr int32_t kAutoValue = INT32_MIN;

    ChannelOverrideState();

    bool any_overrides() const { return any_overrides_.load() != 0; }
    // Which message kinds of `channel` (0..15) an override can suppress (0: none; see should_suppress).
    int32_t status_mask(int channel) const { return status_masks_[static_cast<size_t>(channel)].load(); }
    int32_t filter_generation() const { return static_cast<int32_t>(filter_generation_.load()); }
    bool has_pending_output();
    int32_t get_value(int channel, ChannelAttribute attribute) const;
    // v1 SetValue; throws models::ModelException (ArgumentOutOfRangeException) as v1.
    bool set_value(int channel, ChannelAttribute attribute, int32_t value);
    void clear();
    void reset_statistics();
    void mark_all_forced_pending();
    void mark_channel_forced_pending(int channel);
    int32_t take_pending_mask(int channel);
    void requeue(int channel, int32_t mask);
    bool should_suppress(const models::EventView& midiEvent, ChannelAttribute& attribute, int32_t& forcedValue) const;
    void record_suppressed(int channel);
    void apply_to_snapshot(ChannelPlaybackSnapshot& snapshot);
    // The forced values as the chase index reads them.
    ChannelOverrideValues values_snapshot() const;

    // v1 CreateMessage / CreateControllerMessage: a generated MidiEvent seen
    // through a MidiEventView with event index `eventIndex` (0 when v1
    // converts the MidiEvent implicitly, -1 for MidiEventView.FromEvent(e, -1)).
    static models::EventView create_message(int channel, ChannelAttribute attribute, int32_t value,
        int32_t eventIndex, int64_t micros = 0);
    static models::EventView create_controller_message(int channel, int controller, int value,
        int32_t eventIndex, int64_t micros = 0);
    static bool try_classify(const models::EventView& midiEvent, ChannelAttribute& attribute, int32_t& value);
    static int32_t minimum(ChannelAttribute attribute);
    static int32_t maximum(ChannelAttribute attribute);

private:
    static int index(int channel, ChannelAttribute attribute)
    { return channel * kAttributeCount + static_cast<int>(attribute); }
    bool conflicts_with_reset(int channel, ChannelAttribute attribute, int32_t resetValue, int32_t& forcedValue) const;
    void recalculate_any_overrides_locked();

    std::array<std::atomic<int32_t>, 16 * kAttributeCount> values_;
    std::array<int32_t, 16> pending_masks_;
    std::array<std::atomic<int32_t>, 16> status_masks_;
    std::array<std::atomic<int64_t>, 16> suppressed_;
    std::mutex change_sync_;
    std::atomic<int32_t> any_overrides_;
    std::atomic<uint32_t> filter_generation_;
};

// v1 ChannelRoutingState (disabled channels).
class ChannelRoutingState {
public:
    ChannelRoutingState();

    bool any_disabled() const { return disabled_mask_.load() != 0; }
    int32_t disabled_mask() const { return disabled_mask_.load(); }
    int32_t filter_generation() const { return static_cast<int32_t>(filter_generation_.load()); }
    // v1 IsEnabled / SetEnabled / DisableGeneration throw
    // ArgumentOutOfRangeException("channel") outside 0..15.
    bool is_enabled(int channel) const;
    bool set_enabled(int channel, bool enabled);
    bool should_filter(const models::EventView& midiEvent) const;
    int32_t disable_generation(int channel) const;
    void record_filtered(int channel);
    void apply_to_snapshot(ChannelPlaybackSnapshot& snapshot) const;
    void reset_statistics();
    void clear();
    ChannelRouting routing_snapshot() const;

private:
    std::array<std::atomic<int64_t>, 16> muted_filtered_;
    std::array<std::atomic<uint32_t>, 16> disable_generation_;
    std::atomic<int32_t> disabled_mask_;
    std::atomic<uint32_t> filter_generation_;
};

} // namespace engine
} // namespace midibn
