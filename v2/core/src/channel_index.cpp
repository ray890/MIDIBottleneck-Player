// Port of v1.0.44 ChannelSourceValueIndex.cs, MidiStateChaseIndex.cs and
// ChannelOverrideState.TryClassify / CreateMessage / CreateControllerMessage.
#include "midibn/channel_index.h"

#include <algorithm>
#include <climits>

namespace midibn {

const char* channel_attribute_name(ChannelAttribute attribute)
{
    switch (attribute) {
    case ChannelAttribute::BankMsb: return "BankMsb";
    case ChannelAttribute::BankLsb: return "BankLsb";
    case ChannelAttribute::Program: return "Program";
    case ChannelAttribute::Volume: return "Volume";
    case ChannelAttribute::Expression: return "Expression";
    case ChannelAttribute::Pan: return "Pan";
    case ChannelAttribute::Sustain: return "Sustain";
    case ChannelAttribute::PitchBend: return "PitchBend";
    case ChannelAttribute::Aftertouch: return "Aftertouch";
    }
    return "Unknown";
}

// ------------------------------------------------------------ IndexSeries

void IndexSeries::append(const IndexEntry& entry)
{
    if (count_ < kSegmentCapacity) {
        if (head_.size() == head_.capacity())
            head_.reserve(std::min<size_t>(kSegmentCapacity, std::max<size_t>(4, head_.capacity() * 2)));
        head_.push_back(entry);
    }
    else {
        int offset = count_ & (kSegmentCapacity - 1);
        if (offset == 0) tail_.emplace_back(new IndexEntry[kSegmentCapacity]);
        tail_.back()[offset] = entry;
    }
    ++count_;
}

void IndexSeries::trim_last_segment()
{
    head_.shrink_to_fit();
    int used = count_ & (kSegmentCapacity - 1);
    if (tail_.empty() || used == 0) return;
    std::unique_ptr<IndexEntry[]> trimmed(new IndexEntry[static_cast<size_t>(used)]);
    std::copy(tail_.back().get(), tail_.back().get() + used, trimmed.get());
    tail_.back() = std::move(trimmed);
}

bool IndexSeries::latest_before(int32_t exclusive, IndexEntry& entry) const
{
    if (count_ == 0) return false;
    int32_t low = 0, high = count_;
    while (low < high) {
        int32_t middle = low + ((high - low) >> 1);
        if ((*this)[middle].event_index < exclusive) low = middle + 1;
        else high = middle;
    }
    if (low == 0) return false;
    entry = (*this)[low - 1];
    return true;
}

IndexInput index_input(const EventRecord& record)
{
    IndexInput input;
    input.channel = record.channel();
    input.status = record.status;
    input.length = record.payload_length;
    input.data1 = record.payload_length > 1 ? static_cast<uint8_t>(record.packed >> 8) : 0;
    input.data2 = record.payload_length > 2 ? static_cast<uint8_t>(record.packed >> 16) : 0;
    return input;
}

IndexInput index_input(const PackedRecord& record)
{
    // A long SysEx has channel -1, which the indexes ignore; its length is only a placeholder.
    const int length = record.is_long() ? 4 : record.inline_length();
    IndexInput input;
    input.channel = record.channel();
    input.status = record.status();
    input.length = length;
    input.data1 = length > 1 && !record.is_long() ? static_cast<uint8_t>(record.packed >> 8) : 0;
    input.data2 = length > 2 && !record.is_long() ? static_cast<uint8_t>(record.packed >> 16) : 0;
    return input;
}

bool classify_channel_attribute(const IndexInput& input, ChannelAttribute& attribute, int& value)
{
    attribute = ChannelAttribute::BankMsb;
    value = 0;
    int status = input.status & 0xF0;
    int first = input.data1;
    int second = input.data2;
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

// ------------------------------------------------------------ source values

void SourceValueIndex::add(int channel, ChannelAttribute attribute, int32_t event_index, int32_t value)
{
    slots_[static_cast<size_t>(channel * kChannelAttributeCount + static_cast<int>(attribute))]
        .append(IndexEntry{ event_index, value });
}

void SourceValueIndex::add(const IndexInput& input, int32_t event_index)
{
    if (input.channel < 0 || input.channel >= 16) return;
    int status = input.status & 0xF0;
    if (status == 0xB0 && input.length > 2 && input.data1 == 121) {
        add(input.channel, ChannelAttribute::Expression, event_index, 127);
        add(input.channel, ChannelAttribute::Pan, event_index, 64);
        add(input.channel, ChannelAttribute::Sustain, event_index, 0);
        add(input.channel, ChannelAttribute::PitchBend, event_index, 0);
        add(input.channel, ChannelAttribute::Aftertouch, event_index, 0);
        return;
    }
    ChannelAttribute attribute;
    int value;
    if (classify_channel_attribute(input, attribute, value)) add(input.channel, attribute, event_index, value);
}

void SourceValueIndex::complete(const CancellationCheck& check)
{
    for (int slot = 0; slot < kSlotCount; ++slot) {
        if ((slot & 15) == 0 && check) check();
        slots_[static_cast<size_t>(slot)].trim_last_segment();
    }
}

bool SourceValueIndex::try_get_latest(const EventStore& store, int channel, ChannelAttribute attribute,
    int64_t position_us, int& value) const
{
    value = 0;
    if (channel < 0 || channel >= 16) return false;
    const IndexSeries& series = slots_[static_cast<size_t>(channel * kChannelAttributeCount + static_cast<int>(attribute))];
    if (series.count() == 0) return false;
    int32_t low = 0, high = series.count();
    while (low < high) {
        int32_t middle = low + ((high - low) >> 1);
        if (store.records.micros_at(series[middle].event_index) <= position_us) low = middle + 1;
        else high = middle;
    }
    if (low == 0) return false;
    value = series[low - 1].value;
    return true;
}

int64_t SourceValueIndex::entry_count() const
{
    int64_t total = 0;
    for (const IndexSeries& s : slots_) total += s.count();
    return total;
}

// ------------------------------------------------------------ chase

ChannelReadout unknown_channel_readout()
{
    return ChannelReadout{ -1, -1, -1, -1, -1, -1, -1, INT32_MIN, -1 };
}

namespace {

ChaseMessage controller_message(int channel, int controller, int value)
{
    ChaseMessage m;
    m.kind = EventKind::ControlChange;
    m.length = 3;
    m.bytes[0] = static_cast<uint8_t>(0xB0 | channel);
    m.bytes[1] = static_cast<uint8_t>(controller);
    m.bytes[2] = static_cast<uint8_t>(value);
    return m;
}

// v1 ChannelOverrideState.CreateMessage for the attributes chase emits.
ChaseMessage attribute_message(int channel, ChannelAttribute attribute, int value)
{
    ChaseMessage m;
    if (attribute == ChannelAttribute::Program) {
        m.kind = EventKind::ProgramChange;
        m.length = 2;
        m.bytes[0] = static_cast<uint8_t>(0xC0 | channel);
        m.bytes[1] = static_cast<uint8_t>(value);
        m.bytes[2] = 0;
    }
    else if (attribute == ChannelAttribute::PitchBend) {
        int raw = value + 8192;
        m.kind = EventKind::PitchBend;
        m.length = 3;
        m.bytes[0] = static_cast<uint8_t>(0xE0 | channel);
        m.bytes[1] = static_cast<uint8_t>(raw & 0x7F);
        m.bytes[2] = static_cast<uint8_t>((raw >> 7) & 0x7F);
    }
    else {  // Aftertouch: channel pressure, labelled SystemMessage as in v1 (a label only; see channel_state.cpp)
        m.kind = EventKind::SystemMessage;
        m.length = 2;
        m.bytes[0] = static_cast<uint8_t>(0xD0 | channel);
        m.bytes[1] = static_cast<uint8_t>(value);
        m.bytes[2] = 0;
    }
    return m;
}

void add_selector(std::vector<ChaseMessage>& messages, int channel, int encoded)
{
    if (encoded < 0) {
        messages.push_back(controller_message(channel, 101, 127));
        messages.push_back(controller_message(channel, 100, 127));
        return;
    }
    bool nrpn = (encoded & 0x4000) != 0;
    int parameter = encoded & 0x3FFF;
    messages.push_back(controller_message(channel, nrpn ? 99 : 101, (parameter >> 7) & 0x7F));
    messages.push_back(controller_message(channel, nrpn ? 98 : 100, parameter & 0x7F));
}

bool preserved_across_reset(int controller) { return controller == 0 || controller == 7 || controller == 32; }

bool is_forced_controller(const ChannelOverrideValues* overrides, int channel, int controller)
{
    if (!overrides) return false;
    ChannelAttribute attribute;
    switch (controller) {
    case 0: attribute = ChannelAttribute::BankMsb; break;
    case 32: attribute = ChannelAttribute::BankLsb; break;
    case 7: attribute = ChannelAttribute::Volume; break;
    case 10: attribute = ChannelAttribute::Pan; break;
    case 11: attribute = ChannelAttribute::Expression; break;
    case 64: attribute = ChannelAttribute::Sustain; break;
    default: return false;
    }
    return overrides->get(channel, attribute) != ChannelOverrideValues::kAuto;
}

int32_t latest_index(const IndexSeries& series, int32_t exclusive, int32_t fallback)
{
    IndexEntry entry;
    return series.latest_before(exclusive, entry) ? entry.event_index : fallback;
}

} // namespace

ChaseIndex::ChaseIndex()
    : state_(16 * kSlotsPerChannel), resets_(16), selectors_(16), builder_(new Builder())
{
    builder_->rpn_msb.fill(127);
    builder_->rpn_lsb.fill(127);
    builder_->nrpn_msb.fill(127);
    builder_->nrpn_lsb.fill(127);
    builder_->active_selector.fill(-1);
}

void ChaseIndex::add_state(int channel, int slot, int32_t event_index, int32_t value)
{
    state_[static_cast<size_t>(channel * kSlotsPerChannel + slot)].append(IndexEntry{ event_index, value });
}

void ChaseIndex::add(const IndexInput& input, int32_t event_index)
{
    if (input.channel < 0 || input.channel >= 16) return;
    const int channel = input.channel;
    const size_t c = static_cast<size_t>(channel);
    int status = input.status & 0xF0;
    int first = input.data1;
    int second = input.data2;
    if (status == 0xC0) { add_state(channel, kProgramSlot, event_index, first); return; }
    if (status == 0xD0) { add_state(channel, kChannelPressureSlot, event_index, first); return; }
    if (status == 0xE0) { add_state(channel, kPitchBendSlot, event_index, ((second << 7) | first) - 8192); return; }
    if (status != 0xB0) return;
    Builder& b = *builder_;
    if (first == 121) {
        resets_[c].append(IndexEntry{ event_index, 0 });
        b.rpn_msb[c] = b.rpn_lsb[c] = 127;
        b.nrpn_msb[c] = b.nrpn_lsb[c] = 127;
        b.active_selector[c] = -1;
        b.parameter_values[c].clear();
        selectors_[c].append(IndexEntry{ event_index, -1 });
        return;
    }
    if (first >= 120) return;
    if (first == 101) { b.rpn_msb[c] = second; select(channel, false, event_index); return; }
    if (first == 100) { b.rpn_lsb[c] = second; select(channel, false, event_index); return; }
    if (first == 99) { b.nrpn_msb[c] = second; select(channel, true, event_index); return; }
    if (first == 98) { b.nrpn_lsb[c] = second; select(channel, true, event_index); return; }
    if (first == 6 || first == 38 || first == 96 || first == 97) { apply_data(channel, first, second, event_index); return; }
    add_state(channel, first, event_index, second);
}

void ChaseIndex::select(int channel, bool nrpn, int32_t event_index)
{
    Builder& b = *builder_;
    const size_t c = static_cast<size_t>(channel);
    int msb = nrpn ? b.nrpn_msb[c] : b.rpn_msb[c];
    int lsb = nrpn ? b.nrpn_lsb[c] : b.rpn_lsb[c];
    int key = msb == 127 && lsb == 127 ? -1 : (nrpn ? 0x4000 : 0) | (msb << 7) | lsb;
    b.active_selector[c] = key;
    selectors_[c].append(IndexEntry{ event_index, key });
}

void ChaseIndex::apply_data(int channel, int controller, int data, int32_t event_index)
{
    Builder& b = *builder_;
    const size_t c = static_cast<size_t>(channel);
    int key = b.active_selector[c];
    if (key < 0) return;
    std::map<int32_t, int32_t>& values = b.parameter_values[c];
    std::map<int32_t, int32_t>::iterator found = values.find(key);
    bool known = found != values.end();
    int value = known ? found->second : 0;
    if (controller == 6) value = (data << 7) | (known ? value & 0x7F : 0);
    else if (controller == 38) value = (known ? value & 0x3F80 : 0) | data;
    else {
        if (!known) return;
        value = std::max(0, std::min(16383, value + (controller == 96 ? 1 : -1)));
    }
    values[key] = value;
    b.parameters[c][key].append(IndexEntry{ event_index, value });
}

void ChaseIndex::complete(const CancellationCheck& check)
{
    int64_t entries = 0;
    for (size_t i = 0; i < state_.size(); ++i) {
        if ((i & 127) == 0 && check) check();
        state_[i].trim_last_segment();
        entries += state_[i].count();
    }
    for (std::vector<IndexSeries>* group : { &resets_, &selectors_ }) {
        for (size_t i = 0; i < group->size(); ++i) {
            if ((i & 127) == 0 && check) check();
            (*group)[i].trim_last_segment();
            entries += (*group)[i].count();
        }
    }
    int32_t parameterCount = 0;
    for (size_t channel = 0; channel < 16; ++channel) {
        if (check) check();
        std::map<int32_t, IndexSeries>& built = builder_->parameters[channel];
        if (built.empty()) continue;
        std::vector<ParameterSeries>& completed = parameters_[channel];
        completed.reserve(built.size());
        for (std::map<int32_t, IndexSeries>::iterator it = built.begin(); it != built.end(); ++it) {
            it->second.trim_last_segment();
            entries += it->second.count();
            completed.push_back(ParameterSeries{ it->first, std::move(it->second) });
        }
        parameterCount += static_cast<int32_t>(completed.size());
    }
    builder_.reset();
    entry_count_ = entries;
    parameter_series_count_ = parameterCount;
}

int ChaseIndex::read_controller(int channel, int controller, int32_t exclusive, int32_t reset_index) const
{
    IndexEntry change;
    if (!state(channel, controller).latest_before(exclusive, change) ||
        (change.event_index <= reset_index && !preserved_across_reset(controller))) return -1;
    return change.value;
}

int ChaseIndex::read_special(int channel, int slot, int32_t exclusive, int32_t reset_index) const
{
    IndexEntry change;
    if (!state(channel, slot).latest_before(exclusive, change) ||
        (slot != kProgramSlot && change.event_index <= reset_index))
        return slot == kPitchBendSlot ? INT32_MIN : -1;
    return change.value;
}

std::array<ChannelReadout, 16> ChaseIndex::attribute_readouts(int32_t exclusive) const
{
    std::array<ChannelReadout, 16> result;
    result.fill(unknown_channel_readout());
    if (exclusive <= 0) return result;
    for (int channel = 0; channel < 16; ++channel) {
        int32_t reset = latest_index(resets(channel), exclusive, INT32_MIN);
        ChannelReadout& r = result[static_cast<size_t>(channel)];
        r.bank_msb = read_controller(channel, 0, exclusive, reset);
        r.bank_lsb = read_controller(channel, 32, exclusive, reset);
        r.volume = read_controller(channel, 7, exclusive, reset);
        r.expression = read_controller(channel, 11, exclusive, reset);
        r.pan = read_controller(channel, 10, exclusive, reset);
        r.sustain = read_controller(channel, 64, exclusive, reset);
        r.program = read_special(channel, kProgramSlot, exclusive, reset);
        r.pitch_bend = read_special(channel, kPitchBendSlot, exclusive, reset);
        r.channel_pressure = read_special(channel, kChannelPressureSlot, exclusive, reset);
    }
    return result;
}

std::vector<ChaseMessage> ChaseIndex::messages(int32_t exclusive, const ChannelRouting* routing,
    const ChannelOverrideValues* overrides) const
{
    std::vector<ChaseMessage> out;
    if (exclusive <= 0) return out;
    for (int channel = 0; channel < 16; ++channel) {
        if (routing && !routing->is_enabled(channel)) continue;
        int32_t reset = latest_index(resets(channel), exclusive, INT32_MIN);
        auto addController = [&](int controller) {
            if (is_forced_controller(overrides, channel, controller)) return;
            IndexEntry change;
            if (!state(channel, controller).latest_before(exclusive, change)) return;
            if (change.event_index <= reset && !preserved_across_reset(controller)) return;
            out.push_back(controller_message(channel, controller, change.value));
        };
        auto addSpecial = [&](int slot) {
            ChannelAttribute attribute = slot == kProgramSlot ? ChannelAttribute::Program :
                slot == kPitchBendSlot ? ChannelAttribute::PitchBend : ChannelAttribute::Aftertouch;
            if (overrides && overrides->get(channel, attribute) != ChannelOverrideValues::kAuto) return;
            IndexEntry change;
            if (!state(channel, slot).latest_before(exclusive, change)) return;
            if (slot != kProgramSlot && change.event_index <= reset) return;
            out.push_back(attribute_message(channel, attribute, change.value));
        };
        addController(0);
        addController(32);
        addSpecial(kProgramSlot);
        for (int controller = 1; controller < kControllerCount; ++controller) {
            if (controller == 6 || controller == 32 || controller == 38 || controller == 96 || controller == 97 ||
                controller == 98 || controller == 99 || controller == 100 || controller == 101) continue;
            addController(controller);
        }
        addSpecial(kPitchBendSlot);
        addSpecial(kChannelPressureSlot);

        for (const ParameterSeries& parameter : parameters(channel)) {
            IndexEntry change;
            if (!parameter.values.latest_before(exclusive, change) || change.event_index <= reset) continue;
            add_selector(out, channel, parameter.key);
            out.push_back(controller_message(channel, 6, (change.value >> 7) & 0x7F));
            out.push_back(controller_message(channel, 38, change.value & 0x7F));
        }
        IndexEntry selector;
        if (selectors(channel).latest_before(exclusive, selector) && selector.event_index > reset)
            add_selector(out, channel, selector.value);
    }
    return out;
}

} // namespace midibn
