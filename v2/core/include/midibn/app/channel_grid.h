// The Channels window's grid, without any drawing (v1 ChannelMonitorForm.cs):
// the 16 columns with their headings and tooltips, and the text, style and
// tooltip of each of the 16 x 16 cells for a channel snapshot, a source
// readout (chase off, or an unobserved value) and the detached state.
//
// Everything is plain data. A frontend draws `ChannelCell` text with the
// colour and font the cell names, and repaints only the cells that
// `ChannelGridChanges` reports. Numbers use the `NumberCulture` (UI_DIFFERENCES
// UD-001). Tables written by v1's own form (v2/harness/expected/channels-ui)
// are compared with these results byte for byte by midibn-channels-app-tests.
#pragma once

#include "midibn/channel_index.h"
#include "midibn/engine/channel_state.h"
#include "midibn/format/net_format.h"

#include <array>
#include <cstdint>
#include <string>

namespace midibn {
namespace app {

enum ChannelColumn {
    ColChannel = 0,    // "Ch": click to enable or disable the channel
    ColKeysDown,       // Polyphony
    ColPeakKeys,       // Peak polyphony
    ColSent,
    ColDropped,
    ColSuppressed,     // Override filtered
    ColBankMsb,
    ColBankLsb,
    ColProgram,
    ColVolume,
    ColExpression,
    ColPan,
    ColSustain,
    ColBend,           // Pitch bend
    ColAftertouch,
    ColPosition,
    kChannelColumnCount
};

struct ChannelColumnInfo {
    std::string name;       // v1 column name ("BankMsb")
    std::string caption;    // heading text ("Bank MSB")
    int canonical_width;    // v1's initial width at 100% (the window re-fits to the headings)
    std::string tooltip;    // heading tooltip
    bool is_attribute;      // an editable attribute column
    ChannelAttribute attribute;
};
const ChannelColumnInfo& channel_column_info(int column);
// The column of an attribute (v1 ColumnName).
int channel_column_for_attribute(ChannelAttribute attribute);

// Foreground of a cell: v1 SystemColors.ControlText, Color.FromArgb(20, 75, 155), Color.DimGray.
enum class CellColor { Normal, Forced, Historical };
// Font of a cell: the Application font, its bold and its italic form.
enum class CellFont { Regular, Bold, Italic };

struct ChannelCell {
    std::string text = "\xE2\x80\x94";   // U+2014: no value
    std::string tooltip;
    CellColor color = CellColor::Normal;
    CellFont font = CellFont::Regular;
    bool pending = false;               // a forced value not yet applied to the output session
    bool operator==(const ChannelCell& o) const
    {
        return text == o.text && tooltip == o.tooltip && color == o.color && font == o.font && pending == o.pending;
    }
    bool same_look(const ChannelCell& o) const { return text == o.text && color == o.color && font == o.font; }
};

// What an update changed: bit c of rows[r] is set when cell (r, c) must be repainted
// (all bits when the row's background changed).
struct ChannelGridChanges {
    std::array<uint16_t, 16> rows;
    ChannelGridChanges() { rows.fill(0); }
    bool any() const
    {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] != 0) return true;
        return false;
    }
    void mark_all() { rows.fill(0xFFFF); }
};

class ChannelGridModel {
public:
    explicit ChannelGridModel(const format::NumberCulture& culture = format::NumberCulture());

    void set_culture(const format::NumberCulture& culture) { culture_ = culture; }

    // ---- the window's text
    // "MIDIBottleneck Player — MIDI Channel Monitor — <file name>" (or "— no file" while detached).
    std::string title() const;
    static std::string title_for(const std::string& fileName);
    static const std::string& explanation();

    // ---- data in (v1 UpdateSnapshot / SetSourceReadouts / DetachForSongReplacement / AttachSong)
    // Applies a snapshot. False (nothing changed) while detached. `changes` receives the cells that changed.
    bool update_snapshot(const engine::ChannelPlaybackSnapshot& snapshot, ChannelGridChanges* changes);
    // Source values to show where the output has not yet sent a value; null clears them.
    // They take effect with the next update_snapshot (as in v1).
    void set_source_readouts(const std::array<ChannelReadout, 16>* readouts);
    bool has_source_readouts() const { return has_readouts_; }
    bool has_snapshot() const { return has_snapshot_; }
    // Rebuild every cell from the remembered snapshot (after a scale change); true if there is one.
    bool refresh_from_last(ChannelGridChanges* changes);

    void detach(const std::string& message, ChannelGridChanges* changes);   // empty message: "No MIDI file is loaded."
    void attach(const std::string& fileName);
    bool detached() const { return detached_; }
    const std::string& detached_message() const { return detached_message_; }

    // ---- cells
    const ChannelCell& cell(int channel, int column) const { return rows_[static_cast<size_t>(channel)].cells[static_cast<size_t>(column)]; }
    // Row background: window colour while the channel is enabled, button-face colour while it is off.
    bool row_enabled(int channel) const { return rows_[static_cast<size_t>(channel)].enabled; }
    const engine::MidiChannelSnapshot& channel_state(int channel) const { return channels_[static_cast<size_t>(channel)]; }
    // v1 ShowFeedback sets the cell's tooltip to the message it shows.
    void set_cell_tooltip(int channel, int column, const std::string& text)
    { rows_[static_cast<size_t>(channel)].cells[static_cast<size_t>(column)].tooltip = text; }

    // ---- attribute questions (the editor and the right-click behaviour)
    bool is_forced(int channel, ChannelAttribute attribute) const;
    bool is_historical(int channel, ChannelAttribute attribute) const;
    // v1 CurrentAttributeValue: the value an editor starts from.
    int current_attribute_value(int channel, ChannelAttribute attribute) const;
    static int default_editor_seed(ChannelAttribute attribute);
    // v1 FormatAttribute: "—" for unknown, "1 — Acoustic Grand Piano" for Program, Off/On, +n/-n/0, plain numbers.
    std::string format_attribute(int channel, ChannelAttribute attribute, int value) const;
    // v1 AttributeHelp: the editor's tooltip.
    static std::string attribute_help(ChannelAttribute attribute, int channel);

    // v1 ObservedValue / ForcedValue / IsUnknownAttribute.
    static int observed_value(const engine::MidiChannelSnapshot& state, ChannelAttribute attribute);
    static int forced_value(const engine::MidiChannelSnapshot& state, ChannelAttribute attribute);
    static int readout_value(const ChannelReadout& readout, ChannelAttribute attribute);
    static bool is_unknown_attribute(ChannelAttribute attribute, int value);
    // v1 FormatClock: mm:ss.fff, minutes not limited to two digits.
    static std::string format_clock(int64_t microseconds);

private:
    struct Row {
        bool enabled = true;
        std::array<ChannelCell, kChannelColumnCount> cells;
    };
    int displayed_attribute_value(int channel, ChannelAttribute attribute, const engine::MidiChannelSnapshot& state,
        bool& sourceDerived) const;
    void build_row(int channel, Row& row) const;
    void set_cell(Row& row, int column, const std::string& value, bool forced, bool historical, bool pending) const;
    void set_attribute_cell(int channel, Row& row, ChannelAttribute attribute,
        const engine::MidiChannelSnapshot& state) const;
    void clear_displayed_state();
    bool rebuild(ChannelGridChanges* changes);

    format::NumberCulture culture_;
    std::array<Row, 16> rows_;
    std::array<engine::MidiChannelSnapshot, 16> channels_;
    std::array<ChannelReadout, 16> readouts_;
    bool has_snapshot_ = false;
    bool has_readouts_ = false;
    bool detached_ = false;
    std::string detached_message_;
    std::string file_name_;
};

} // namespace app
} // namespace midibn
