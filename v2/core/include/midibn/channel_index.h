// Source-value and whole-state chase indexes (v1 ChannelSourceValueIndex.cs
// and MidiStateChaseIndex.cs). Both are built in the loader's merge pass from
// the provisional event in final-index order, as v1 does. Entries are 8 bytes
// in 4,096-entry segments.
#pragma once

#include "midibn/event_store.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace midibn {

// v1 ChannelAttribute (ChannelOverrides.cs); the order is part of the trace.
enum class ChannelAttribute : int {
    BankMsb = 0,
    BankLsb,
    Program,
    Volume,
    Expression,
    Pan,
    Sustain,
    PitchBend,
    Aftertouch
};
const int kChannelAttributeCount = 9;
const char* channel_attribute_name(ChannelAttribute attribute);

struct IndexEntry {
    int32_t event_index;
    int32_t value;
};
static_assert(sizeof(IndexEntry) == 8, "index entries stay 8 bytes");

// Entries of one series, in index order. The first 4,096 entries live in one
// array that grows by doubling (so the thousands of sparse series of a big file
// cost only what they hold); later entries go into fixed 4,096-entry segments
// (improvement DD-004: v1 and v2 Milestone 2 reserved a whole segment per series
// before trimming it).
class IndexSeries {
public:
    static constexpr int kSegmentShift = 12;
    static constexpr int kSegmentCapacity = 1 << kSegmentShift;  // 4,096

    void append(const IndexEntry& entry);
    int32_t count() const { return count_; }
    const IndexEntry& operator[](int32_t index) const
    {
        return index < kSegmentCapacity ? head_[static_cast<size_t>(index)]
            : tail_[static_cast<size_t>(index >> kSegmentShift) - 1][index & (kSegmentCapacity - 1)];
    }
    void trim_last_segment();
    // v1 TryLatest: the last entry whose event index is below `exclusive`.
    bool latest_before(int32_t exclusive, IndexEntry& entry) const;

private:
    std::vector<IndexEntry> head_;
    std::vector<std::unique_ptr<IndexEntry[]>> tail_;
    int32_t count_ = 0;
};

// What the indexes read from one event (v1 MidiEventView over a short message).
struct IndexInput {
    int channel;
    uint8_t status;
    int32_t length;
    uint8_t data1;  // byte 1 when length > 1, else 0
    uint8_t data2;  // byte 2 when length > 2, else 0
};
IndexInput index_input(const EventRecord& record);
IndexInput index_input(const PackedRecord& record);

// v1 ChannelOverrideState.TryClassify.
bool classify_channel_attribute(const IndexInput& input, ChannelAttribute& attribute, int& value);

// Called at v1's cancellation checkpoints while indexes are completed.
typedef std::function<void()> CancellationCheck;

class SourceValueIndex {
public:
    static constexpr int kSlotCount = 16 * kChannelAttributeCount;

    void add(const IndexInput& input, int32_t event_index);
    void complete(const CancellationCheck& check);
    // v1 TryGetLatest: value of the last change whose event time <= position.
    bool try_get_latest(const EventStore& store, int channel, ChannelAttribute attribute,
        int64_t position_us, int& value) const;
    const IndexSeries& slot(int index) const { return slots_[static_cast<size_t>(index)]; }
    int64_t entry_count() const;

private:
    void add(int channel, ChannelAttribute attribute, int32_t event_index, int32_t value);
    std::array<IndexSeries, kSlotCount> slots_;
};

struct ChannelReadout {
    int bank_msb, bank_lsb, program, volume, expression, pan, sustain, pitch_bend, channel_pressure;
};
// v1 ChannelPlaybackSnapshot.Unknown values.
ChannelReadout unknown_channel_readout();

struct ChaseMessage {
    EventKind kind;
    uint8_t length;
    uint8_t bytes[3];
};

// Channels disabled for output (v1 ChannelRoutingState as CreateMessages sees it).
struct ChannelRouting {
    uint16_t disabled_mask = 0;
    bool is_enabled(int channel) const { return (disabled_mask & (1u << channel)) == 0; }
};

// Forced attribute values (v1 ChannelOverrideState values; kAuto = not forced).
struct ChannelOverrideValues {
    static constexpr int32_t kAuto = INT32_MIN;
    std::array<int32_t, 16 * kChannelAttributeCount> values;
    ChannelOverrideValues() { values.fill(kAuto); }
    int32_t get(int channel, ChannelAttribute a) const
    { return values[static_cast<size_t>(channel * kChannelAttributeCount + static_cast<int>(a))]; }
    void set(int channel, ChannelAttribute a, int32_t value)
    { values[static_cast<size_t>(channel * kChannelAttributeCount + static_cast<int>(a))] = value; }
};

class ChaseIndex {
public:
    static constexpr int kControllerCount = 120;
    static constexpr int kProgramSlot = 120;
    static constexpr int kPitchBendSlot = 121;
    static constexpr int kChannelPressureSlot = 122;
    static constexpr int kSlotsPerChannel = 123;

    struct ParameterSeries {
        int32_t key;
        IndexSeries values;
    };

    ChaseIndex();
    void add(const IndexInput& input, int32_t event_index);
    void complete(const CancellationCheck& check);

    int64_t entry_count() const { return entry_count_; }
    int32_t parameter_series_count() const { return parameter_series_count_; }
    std::array<ChannelReadout, 16> attribute_readouts(int32_t event_index_exclusive) const;
    std::vector<ChaseMessage> messages(int32_t event_index_exclusive, const ChannelRouting* routing,
        const ChannelOverrideValues* overrides) const;

    const IndexSeries& state(int channel, int slot) const
    { return state_[static_cast<size_t>(channel * kSlotsPerChannel + slot)]; }
    const IndexSeries& resets(int channel) const { return resets_[static_cast<size_t>(channel)]; }
    const IndexSeries& selectors(int channel) const { return selectors_[static_cast<size_t>(channel)]; }
    const std::vector<ParameterSeries>& parameters(int channel) const { return parameters_[static_cast<size_t>(channel)]; }

private:
    void add_state(int channel, int slot, int32_t event_index, int32_t value);
    void select(int channel, bool nrpn, int32_t event_index);
    void apply_data(int channel, int controller, int data, int32_t event_index);
    int read_controller(int channel, int controller, int32_t exclusive, int32_t reset_index) const;
    int read_special(int channel, int slot, int32_t exclusive, int32_t reset_index) const;

    std::vector<IndexSeries> state_;
    std::vector<IndexSeries> resets_;
    std::vector<IndexSeries> selectors_;
    std::array<std::vector<ParameterSeries>, 16> parameters_;
    int64_t entry_count_ = 0;
    int32_t parameter_series_count_ = 0;

    // Builder state, released by complete().
    struct Builder {
        std::array<std::map<int32_t, IndexSeries>, 16> parameters;
        std::array<std::map<int32_t, int32_t>, 16> parameter_values;
        std::array<int, 16> rpn_msb, rpn_lsb, nrpn_msb, nrpn_lsb, active_selector;
    };
    std::unique_ptr<Builder> builder_;
};

} // namespace midibn
