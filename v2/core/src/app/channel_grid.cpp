// Port of the grid logic of v1 ChannelMonitorForm.cs (UpdateSnapshot, SetCell,
// SetAttributeCell, DisplayedAttributeValue, FormatAttribute, ...).
#include "midibn/app/channel_grid.h"

#include "midibn/app/channel_catalog.h"

#include <algorithm>
#include <climits>

namespace midibn {
namespace app {

namespace {

const char kDash[] = "\xE2\x80\x94";            // U+2014, v1's "no value"
const char kNewLine[] = "\r\n";                  // Environment.NewLine

const char kEditHelp[] =
    " Click to type or drag horizontally. Right-click releases a forced value to historical Auto; right-click a historical value to restore the latest source value for that attribute.";

std::vector<ChannelColumnInfo> make_columns()
{
    std::vector<ChannelColumnInfo> c;
    std::string edit = kEditHelp;
    ChannelColumnInfo info;
    info.is_attribute = false;
    info.attribute = ChannelAttribute::BankMsb;
    struct Spec {
        const char* name;
        const char* caption;
        int width;
        std::string tooltip;
        int attribute;   // -1: none
    };
    const Spec specs[kChannelColumnCount] = {
        { "Channel", "Ch", 48, "Click to enable or disable this MIDI channel. Disabled source events are filtered before scheduler admission and do not consume queue/service work.", -1 },
        { "KeysDown", "Polyphony", 72, "MIDI-observed held-key polyphony from successfully dispatched Note On/Off messages. This is not necessarily the synthesizer's internal voice count.", -1 },
        { "PeakKeys", "Peak polyphony", 90, "Highest MIDI-observed held-key polyphony since Reset stats.", -1 },
        { "Sent", "Sent", 76, "Successfully dispatched source channel messages. Override injections are not source events.", -1 },
        { "Dropped", "Dropped", 68, "Channel messages rejected by the selected finite-queue overflow policy.", -1 },
        { "Suppressed", "Override filtered", 108, "Source messages filtered before scheduler admission because they conflict with a forced channel value. They consume no queue/service work and are not queue-overflow drops.", -1 },
        { "BankMsb", "Bank MSB", 72, "Latest successfully dispatched Bank Select MSB (CC0)." + edit, static_cast<int>(ChannelAttribute::BankMsb) },
        { "BankLsb", "Bank LSB", 72, "Latest successfully dispatched Bank Select LSB (CC32)." + edit, static_cast<int>(ChannelAttribute::BankLsb) },
        { "Program", "Program", 158, "Latest Program Change, displayed as 1\xE2\x80\x93" "128. Melodic channels use General MIDI reference names; Channel 10 uses recognized GM2, GS, or XG drum-kit names from the displayed Bank and otherwise shows a numbered drum-kit fallback. Device-specific sounds can differ." + edit, static_cast<int>(ChannelAttribute::Program) },
        { "Volume", "Volume", 60, "Latest Channel Volume (CC7)." + edit, static_cast<int>(ChannelAttribute::Volume) },
        { "Expression", "Expr", 54, "Latest Expression (CC11)." + edit, static_cast<int>(ChannelAttribute::Expression) },
        { "Pan", "Pan", 48, "Latest Pan (CC10)." + edit, static_cast<int>(ChannelAttribute::Pan) },
        { "Sustain", "Sustain", 68, "Latest Sustain Pedal (CC64) state. Panic safety always sends sustain off before a forced On is reapplied at the next playback boundary." + edit, static_cast<int>(ChannelAttribute::Sustain) },
        { "Bend", "Pitch bend", 76, "Latest pitch bend, centered at 0. Drag by 16 units per pixel or hold Shift for single-unit changes." + edit, static_cast<int>(ChannelAttribute::PitchBend) },
        { "Aftertouch", "Aftertouch", 72, "Latest MIDI channel pressure/channel aftertouch." + edit, static_cast<int>(ChannelAttribute::Aftertouch) },
        { "Position", "Position", 72, "Source timestamp of the most recent successfully dispatched channel message.", -1 },
    };
    for (int i = 0; i < kChannelColumnCount; ++i) {
        info.name = specs[i].name;
        info.caption = specs[i].caption;
        info.canonical_width = specs[i].width;
        info.tooltip = specs[i].tooltip;
        info.is_attribute = specs[i].attribute >= 0;
        info.attribute = specs[i].attribute >= 0 ? static_cast<ChannelAttribute>(specs[i].attribute) : ChannelAttribute::BankMsb;
        c.push_back(info);
    }
    return c;
}

std::string count_text(int64_t value, const format::NumberCulture& culture) { return format::format_n0(value, culture); }

} // namespace

const ChannelColumnInfo& channel_column_info(int column)
{
    static const std::vector<ChannelColumnInfo> columns = make_columns();
    return columns[static_cast<size_t>(column)];
}

int channel_column_for_attribute(ChannelAttribute attribute)
{
    return ColBankMsb + static_cast<int>(attribute);
}

ChannelGridModel::ChannelGridModel(const format::NumberCulture& culture)
    : culture_(culture)
{
    for (size_t i = 0; i < channels_.size(); ++i) channels_[i] = engine::ChannelPlaybackSnapshot::unknown(static_cast<int>(i));
    for (size_t i = 0; i < readouts_.size(); ++i) readouts_[i] = unknown_channel_readout();
    clear_displayed_state();
}

std::string ChannelGridModel::title_for(const std::string& fileName)
{
    return std::string("MIDIBottleneck Player \xE2\x80\x94 MIDI Channel Monitor \xE2\x80\x94 ") + fileName;
}

std::string ChannelGridModel::title() const
{
    return detached_ ? title_for("no file") : title_for(file_name_);
}

const std::string& ChannelGridModel::explanation()
{
    static const std::string text =
        "Dispatched MIDI state. Click a Ch cell to enable/disable. Attribute cells scrub or type; right-click releases force or restores one latest source value. Blue bold is forced; gray italic is historical.";
    return text;
}

// ---------------------------------------------------------------- values

int ChannelGridModel::observed_value(const engine::MidiChannelSnapshot& s, ChannelAttribute attribute)
{
    switch (attribute) {
    case ChannelAttribute::BankMsb: return s.bank_msb;
    case ChannelAttribute::BankLsb: return s.bank_lsb;
    case ChannelAttribute::Program: return s.program;
    case ChannelAttribute::Volume: return s.volume;
    case ChannelAttribute::Expression: return s.expression;
    case ChannelAttribute::Pan: return s.pan;
    case ChannelAttribute::Sustain: return s.sustain;
    case ChannelAttribute::PitchBend: return s.pitch_bend;
    default: return s.channel_pressure;
    }
}

int ChannelGridModel::forced_value(const engine::MidiChannelSnapshot& s, ChannelAttribute attribute)
{
    switch (attribute) {
    case ChannelAttribute::BankMsb: return s.forced_bank_msb;
    case ChannelAttribute::BankLsb: return s.forced_bank_lsb;
    case ChannelAttribute::Program: return s.forced_program;
    case ChannelAttribute::Volume: return s.forced_volume;
    case ChannelAttribute::Expression: return s.forced_expression;
    case ChannelAttribute::Pan: return s.forced_pan;
    case ChannelAttribute::Sustain: return s.forced_sustain;
    case ChannelAttribute::PitchBend: return s.forced_pitch_bend;
    default: return s.forced_aftertouch;
    }
}

int ChannelGridModel::readout_value(const ChannelReadout& r, ChannelAttribute attribute)
{
    switch (attribute) {
    case ChannelAttribute::BankMsb: return r.bank_msb;
    case ChannelAttribute::BankLsb: return r.bank_lsb;
    case ChannelAttribute::Program: return r.program;
    case ChannelAttribute::Volume: return r.volume;
    case ChannelAttribute::Expression: return r.expression;
    case ChannelAttribute::Pan: return r.pan;
    case ChannelAttribute::Sustain: return r.sustain;
    case ChannelAttribute::PitchBend: return r.pitch_bend;
    default: return r.channel_pressure;
    }
}

bool ChannelGridModel::is_unknown_attribute(ChannelAttribute attribute, int value)
{
    return attribute == ChannelAttribute::PitchBend ? value == INT32_MIN : value < 0;
}

int ChannelGridModel::default_editor_seed(ChannelAttribute attribute)
{
    switch (attribute) {
    case ChannelAttribute::Volume: return 100;
    case ChannelAttribute::Expression: return 127;
    case ChannelAttribute::Pan: return 64;
    default: return 0;
    }
}

int ChannelGridModel::displayed_attribute_value(int channel, ChannelAttribute attribute,
    const engine::MidiChannelSnapshot& state, bool& sourceDerived) const
{
    int bit = 1 << static_cast<int>(attribute);
    bool forced = (state.forced_attribute_mask & bit) != 0;
    int displayed = forced ? forced_value(state, attribute) : observed_value(state, attribute);
    sourceDerived = false;
    if (!forced && is_unknown_attribute(attribute, displayed) && has_readouts_) {
        int source = readout_value(readouts_[static_cast<size_t>(channel)], attribute);
        if (!is_unknown_attribute(attribute, source)) {
            displayed = source;
            sourceDerived = true;
        }
    }
    return displayed;
}

bool ChannelGridModel::is_forced(int channel, ChannelAttribute attribute) const
{
    if (!has_snapshot_ || channel < 0 || channel >= 16) return false;
    return (channels_[static_cast<size_t>(channel)].forced_attribute_mask & (1 << static_cast<int>(attribute))) != 0;
}

bool ChannelGridModel::is_historical(int channel, ChannelAttribute attribute) const
{
    if (!has_snapshot_ || channel < 0 || channel >= 16) return false;
    const engine::MidiChannelSnapshot& state = channels_[static_cast<size_t>(channel)];
    if ((state.historical_attribute_mask & (1 << static_cast<int>(attribute))) != 0) return true;
    return has_readouts_ && !is_forced(channel, attribute) && is_unknown_attribute(attribute, observed_value(state, attribute)) &&
        !is_unknown_attribute(attribute, readout_value(readouts_[static_cast<size_t>(channel)], attribute));
}

int ChannelGridModel::current_attribute_value(int channel, ChannelAttribute attribute) const
{
    if (!has_snapshot_ || channel < 0 || channel >= 16) return default_editor_seed(attribute);
    const engine::MidiChannelSnapshot& state = channels_[static_cast<size_t>(channel)];
    int bit = 1 << static_cast<int>(attribute);
    int value = (state.forced_attribute_mask & bit) != 0 ? forced_value(state, attribute) : observed_value(state, attribute);
    if (is_unknown_attribute(attribute, value) && has_readouts_)
        value = readout_value(readouts_[static_cast<size_t>(channel)], attribute);
    return value == INT32_MIN || (value < 0 && attribute != ChannelAttribute::PitchBend) ? default_editor_seed(attribute) : value;
}

std::string ChannelGridModel::format_attribute(int channel, ChannelAttribute attribute, int value) const
{
    if (value == INT32_MIN || (value < 0 && attribute != ChannelAttribute::PitchBend)) return kDash;
    if (attribute == ChannelAttribute::Program) {
        int bankMsb = -1;
        int bankLsb = -1;
        if (has_snapshot_ && channel >= 0 && channel < 16) {
            bool ignored;
            const engine::MidiChannelSnapshot& state = channels_[static_cast<size_t>(channel)];
            bankMsb = displayed_attribute_value(channel, ChannelAttribute::BankMsb, state, ignored);
            bankLsb = displayed_attribute_value(channel, ChannelAttribute::BankLsb, state, ignored);
        }
        return format::format_integer(static_cast<int64_t>(value) + 1) + " " + kDash + " " +
            channel_program_name(channel, bankMsb, bankLsb, value);
    }
    if (attribute == ChannelAttribute::Sustain) return value == 0 ? "Off" : "On";
    if (attribute == ChannelAttribute::PitchBend) {
        // "+#;-#;0": the sign of the culture's negative pattern is the ASCII minus (UD-001).
        return value > 0 ? "+" + format::format_integer(value) : value < 0 ? format::format_integer(value) : std::string("0");
    }
    return format::format_integer(value);
}

std::string ChannelGridModel::attribute_help(ChannelAttribute attribute, int channel)
{
    switch (attribute) {
    case ChannelAttribute::BankMsb:
    case ChannelAttribute::BankLsb:
        return "0–127. Bank interpretation is synthesizer-specific. Drag one step per eight pixels, or one per sixteen with Shift.";
    case ChannelAttribute::Program:
        return channel == 9
            ? "Program 1–128 with a Bank-aware GM2, GS, or XG drum-kit reference when recognized; device-specific mappings can differ. Drag one step per eight pixels, or one per sixteen with Shift."
            : "Program 1–128 with a General MIDI reference name; actual sounds may differ. Drag one step per eight pixels, or one per sixteen with Shift.";
    case ChannelAttribute::Volume:
        return "MIDI channel volume, 0–127. Drag one step per eight pixels, or one per sixteen with Shift.";
    case ChannelAttribute::Expression:
        return "MIDI expression, 0–127. Drag one step per eight pixels, or one per sixteen with Shift.";
    case ChannelAttribute::Pan:
        return "0 left, 64 center, 127 right. Drag one step per eight pixels, or one per sixteen with Shift.";
    case ChannelAttribute::Sustain:
        return "Off/On. Note-safety cleanup still sends sustain off before reapplying On.";
    case ChannelAttribute::PitchBend:
        return "−8192 through +8191. Drag by 16 units per pixel; hold Shift for one unit per pixel.";
    default:
        return "0–127 channel-wide pressure; drag one step per eight pixels or one per sixteen with Shift. The synthesizer may map or ignore it.";
    }
}

std::string ChannelGridModel::format_clock(int64_t microseconds)
{
    int64_t us = std::max<int64_t>(0, microseconds);
    if (us > INT64_MAX / 10) us = INT64_MAX / 10;
    int64_t ticks = us * 10;
    int64_t minutes = ticks / 600000000LL;
    int64_t seconds = (ticks / 10000000LL) % 60;
    int64_t millis = (ticks / 10000LL) % 1000;
    auto two = [](int64_t v) { return v < 10 ? "0" + format::format_integer(v) : format::format_integer(v); };
    std::string ms = format::format_integer(millis);
    while (ms.size() < 3) ms = "0" + ms;
    return two(minutes) + ":" + two(seconds) + "." + ms;
}

// ---------------------------------------------------------------- cells

void ChannelGridModel::set_cell(Row& row, int column, const std::string& value, bool forced, bool historical,
    bool pending) const
{
    ChannelCell& cell = row.cells[static_cast<size_t>(column)];
    cell.text = value;
    cell.color = forced ? CellColor::Forced : historical ? CellColor::Historical : CellColor::Normal;
    cell.font = forced ? CellFont::Bold : historical ? CellFont::Italic : CellFont::Regular;
    cell.pending = forced && pending;
    cell.tooltip = forced
        ? (pending
            ? "Forced override is configured but has not yet been applied successfully to the current output session."
            : "Forced override is active. Conflicting source messages are filtered at the ordered output boundary.")
        : historical ? (column == ColPosition
            // DD-046: the Position cell is not an attribute and has no restore action (v1 gave it the attribute text).
            ? "Historical last-sent position; it is not enforced and may no longer be current."
            : "Historical last-sent value; it is not enforced and may no longer be effective. Right-click to restore the latest source-file value for this attribute at the current position.")
                     : "";
    if (!value.empty() && value != kDash) cell.tooltip = cell.tooltip.empty() ? value : value + kNewLine + cell.tooltip;
}

void ChannelGridModel::set_attribute_cell(int channel, Row& row, ChannelAttribute attribute,
    const engine::MidiChannelSnapshot& state) const
{
    int column = channel_column_for_attribute(attribute);
    int bit = 1 << static_cast<int>(attribute);
    bool forced = (state.forced_attribute_mask & bit) != 0;
    bool pending = (state.pending_forced_attribute_mask & bit) != 0;
    bool historical = !forced && (state.historical_attribute_mask & bit) != 0;
    bool sourceDerived;
    int displayed = displayed_attribute_value(channel, attribute, state, sourceDerived);
    if (sourceDerived) historical = true;
    std::string value = format_attribute(channel, attribute, displayed);
    set_cell(row, column, value, forced, historical, pending);
    ChannelCell& cell = row.cells[static_cast<size_t>(column)];
    if (sourceDerived)
        cell.tooltip = value + kNewLine +
            "Source-file value immediately before this position. It has not been confirmed at MIDI output; queued or dropped events may differ.";
    if (attribute == ChannelAttribute::Program && channel == 9 && displayed >= 0) {
        bool ignored;
        int bankMsb = displayed_attribute_value(channel, ChannelAttribute::BankMsb, state, ignored);
        int bankLsb = displayed_attribute_value(channel, ChannelAttribute::BankLsb, state, ignored);
        std::string reference = channel_program_reference(channel, bankMsb, bankLsb, displayed);
        cell.tooltip = cell.tooltip.empty() ? reference : cell.tooltip + kNewLine + reference;
    }
}

void ChannelGridModel::build_row(int channel, Row& row) const
{
    const engine::MidiChannelSnapshot& state = channels_[static_cast<size_t>(channel)];
    row.enabled = state.enabled;
    std::string number = format::format_integer(channel + 1);
    ChannelCell& chCell = row.cells[ColChannel];
    chCell.text = state.enabled ? number : number + " off";
    chCell.tooltip = state.enabled
        ? "Channel enabled. Click to disable it."
        : "Channel disabled; " + count_text(state.muted_filtered_events, culture_) +
            " source channel event(s) filtered at output. Click to enable; no state chase is performed.";
    set_cell(row, ColKeysDown, count_text(state.keys_down, culture_), false, false, false);
    set_cell(row, ColPeakKeys, count_text(state.peak_keys_down, culture_), false, false, false);
    set_cell(row, ColSent, count_text(state.sent_events, culture_), false, false, false);
    set_cell(row, ColDropped, count_text(state.dropped_events, culture_), false, false, false);
    set_cell(row, ColSuppressed, count_text(state.override_suppressed_events, culture_), false, false, false);
    for (int a = 0; a < kChannelAttributeCount; ++a) set_attribute_cell(channel, row, static_cast<ChannelAttribute>(a), state);
    set_cell(row, ColPosition, state.last_dispatched_microseconds < 0 ? std::string(kDash) : format_clock(state.last_dispatched_microseconds),
        false, state.last_position_historical, false);
}

bool ChannelGridModel::rebuild(ChannelGridChanges* changes)
{
    for (int channel = 0; channel < 16; ++channel) {
        Row next;
        build_row(channel, next);
        Row& row = rows_[static_cast<size_t>(channel)];
        if (changes) {
            if (next.enabled != row.enabled) changes->rows[static_cast<size_t>(channel)] = 0xFFFF;
            for (int c = 0; c < kChannelColumnCount; ++c)
                if (!next.cells[static_cast<size_t>(c)].same_look(row.cells[static_cast<size_t>(c)]))
                    changes->rows[static_cast<size_t>(channel)] = static_cast<uint16_t>(changes->rows[static_cast<size_t>(channel)] | (1u << c));
        }
        row = next;
    }
    return true;
}

bool ChannelGridModel::update_snapshot(const engine::ChannelPlaybackSnapshot& snapshot, ChannelGridChanges* changes)
{
    if (detached_) return false;
    channels_ = snapshot.channels;
    has_snapshot_ = true;
    return rebuild(changes);
}

bool ChannelGridModel::refresh_from_last(ChannelGridChanges* changes)
{
    if (!has_snapshot_) return false;
    return rebuild(changes);
}

void ChannelGridModel::set_source_readouts(const std::array<ChannelReadout, 16>* readouts)
{
    has_readouts_ = readouts != nullptr;
    if (readouts) readouts_ = *readouts;
}

void ChannelGridModel::clear_displayed_state()
{
    for (int channel = 0; channel < 16; ++channel) {
        Row& row = rows_[static_cast<size_t>(channel)];
        row.enabled = true;
        for (int c = 0; c < kChannelColumnCount; ++c) {
            ChannelCell& cell = row.cells[static_cast<size_t>(c)];
            if (c == ColChannel) {
                cell.text = format::format_integer(channel + 1);
                cell.tooltip.clear();
                continue;
            }
            cell = ChannelCell();
        }
    }
}

void ChannelGridModel::detach(const std::string& message, ChannelGridChanges* changes)
{
    has_snapshot_ = false;
    has_readouts_ = false;
    clear_displayed_state();
    if (changes) changes->mark_all();
    detached_ = true;
    detached_message_ = message.empty() ? "No MIDI file is loaded." : message;
}

void ChannelGridModel::attach(const std::string& fileName)
{
    detached_ = false;
    file_name_ = fileName;
}

} // namespace app
} // namespace midibn
