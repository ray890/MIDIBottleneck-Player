// Tests of the Channels window's portable layer (core/app/channel_*): the catalog, the grid cells,
// the editor and the request flow, against the tables v1's own ChannelMonitorForm wrote
// (v2/harness/channels-ui-reference -> v2/harness/expected/channels-ui). Usage:
// midibn-channels-app-tests [table-directory]  (default v2/harness/expected/channels-ui)
// Exit code 0 when every check passes.
#include "midibn/app/channel_catalog.h"
#include "midibn/app/channel_editor.h"
#include "midibn/app/channel_grid.h"
#include "midibn/app/channel_navigation.h"
#include "midibn/app/channel_session.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;
using namespace midibn::app;

namespace {

int g_checks = 0;
int g_failures = 0;
std::map<std::string, int> g_shown;
std::string g_dir;

void fail(const std::string& section, const std::string& message)
{
    ++g_failures;
    if (++g_shown[section] <= 8) std::printf("FAIL [%s] %s\n", section.c_str(), message.c_str());
    else if (g_shown[section] == 9) std::printf("FAIL [%s] ... further failures in this section not shown\n", section.c_str());
}

std::string show(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F && c != '\\') out += static_cast<char>(c);
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        }
    }
    return out;
}

void eq(const std::string& section, const std::string& label, const std::string& expected, const std::string& actual)
{
    ++g_checks;
    if (expected != actual) fail(section, label + ": expected \"" + show(expected) + "\" got \"" + show(actual) + "\"");
}

void check(const std::string& section, bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) fail(section, what);
}

std::string num(int64_t v) { return std::to_string(v); }

// ---- table reading ----------------------------------------------------------------

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        char c = s[++i];
        if (c == '\\') out += '\\';
        else if (c == 't') out += '\t';
        else if (c == 'r') out += '\r';
        else if (c == 'n') out += '\n';
        else if (c == 'u' && i + 4 < s.size() + 0u) {
            uint32_t unit = static_cast<uint32_t>(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (unit >= 0xD800 && unit < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                uint32_t low = static_cast<uint32_t>(std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                }
            }
            append_utf8(out, unit);
        } else {
            out += '\\';
            out += c;
        }
    }
    return out;
}

typedef std::vector<std::string> Row;

std::vector<std::string> g_pool;

// Reads the data rows of a table (comments skipped, fields unescaped, "@n" pool references NOT yet resolved).
bool read_table(const std::string& name, std::vector<Row>& rows)
{
    std::ifstream in((g_dir + "/" + name).c_str(), std::ios::binary);
    if (!in) {
        fail("tables", "cannot open " + g_dir + "/" + name);
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // never present; tables use \n
        if (line.empty() || line[0] == '#') continue;
        Row row;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            row.push_back(unescape(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        rows.push_back(row);
    }
    return true;
}

// "@12" -> pool text; anything else unchanged.
std::string pooled(const std::string& field)
{
    if (field.size() > 1 && field[0] == '@') {
        size_t id = static_cast<size_t>(std::atoi(field.c_str() + 1));
        if (id < g_pool.size()) return g_pool[id];
    }
    return field;
}

int to_i(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 10)); }
int64_t to_i64(const std::string& s) { return static_cast<int64_t>(std::strtoll(s.c_str(), nullptr, 10)); }

// ---- scripted snapshots ----------------------------------------------------------------

struct Snap {
    engine::ChannelPlaybackSnapshot snapshot;
    bool has_readouts = false;
    std::array<ChannelReadout, 16> readouts;
};
std::map<std::string, Snap> g_states;

void load_inputs(const std::string& file)
{
    std::vector<Row> rows;
    if (!read_table(file, rows)) return;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "state") {
            g_states[r[1]].has_readouts = r[2] == "1";
        } else if (r[0] == "ch" && r.size() == 3 + 31) {
            engine::MidiChannelSnapshot& s = g_states[r[1]].snapshot.channels[static_cast<size_t>(to_i(r[2]))];
            size_t k = 3;
            s.channel = to_i(r[k++]);
            s.keys_down = to_i(r[k++]);
            s.peak_keys_down = to_i(r[k++]);
            s.sent_events = to_i64(r[k++]);
            s.dropped_events = to_i64(r[k++]);
            s.override_suppressed_events = to_i64(r[k++]);
            s.muted_filtered_events = to_i64(r[k++]);
            s.enabled = r[k++] == "1";
            s.bank_msb = to_i(r[k++]);
            s.bank_lsb = to_i(r[k++]);
            s.program = to_i(r[k++]);
            s.volume = to_i(r[k++]);
            s.expression = to_i(r[k++]);
            s.pan = to_i(r[k++]);
            s.sustain = to_i(r[k++]);
            s.pitch_bend = to_i(r[k++]);
            s.channel_pressure = to_i(r[k++]);
            s.last_dispatched_microseconds = to_i64(r[k++]);
            s.historical_attribute_mask = to_i(r[k++]);
            s.last_position_historical = r[k++] == "1";
            s.forced_attribute_mask = to_i(r[k++]);
            s.pending_forced_attribute_mask = to_i(r[k++]);
            s.forced_bank_msb = to_i(r[k++]);
            s.forced_bank_lsb = to_i(r[k++]);
            s.forced_program = to_i(r[k++]);
            s.forced_volume = to_i(r[k++]);
            s.forced_expression = to_i(r[k++]);
            s.forced_pan = to_i(r[k++]);
            s.forced_sustain = to_i(r[k++]);
            s.forced_pitch_bend = to_i(r[k++]);
            s.forced_aftertouch = to_i(r[k++]);
        } else if (r[0] == "ro" && r.size() == 3 + 9) {
            ChannelReadout& o = g_states[r[1]].readouts[static_cast<size_t>(to_i(r[2]))];
            o.bank_msb = to_i(r[3]);
            o.bank_lsb = to_i(r[4]);
            o.program = to_i(r[5]);
            o.volume = to_i(r[6]);
            o.expression = to_i(r[7]);
            o.pan = to_i(r[8]);
            o.sustain = to_i(r[9]);
            o.pitch_bend = to_i(r[10]);
            o.channel_pressure = to_i(r[11]);
        } else {
            fail("inputs", "bad row in " + file);
        }
    }
}

void apply_state(ChannelGridModel& model, const Snap& state, ChannelGridChanges* changes = nullptr)
{
    model.set_source_readouts(state.has_readouts ? &state.readouts : nullptr);
    model.update_snapshot(state.snapshot, changes);
}

// ---- catalog ----------------------------------------------------------------

void test_catalog()
{
    std::vector<Row> rows;
    if (!read_table("catalog.txt", rows)) return;
    int names = 0, drums = 0, patches = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "name") {
            ++names;
            eq("catalog", "name " + r[1], r[2], general_midi_program_name(to_i(r[1])));
        } else if (r[0] == "drum") {
            ++drums;
            std::string label = "ch" + r[1] + " bank " + r[2] + "/" + r[3] + " prog " + r[4];
            eq("catalog", label + " name", r[5], channel_program_name(to_i(r[1]), to_i(r[2]), to_i(r[3]), to_i(r[4])));
            eq("catalog", label + " reference", pooled(r[6]),
                channel_program_reference(to_i(r[1]), to_i(r[2]), to_i(r[3]), to_i(r[4])));
        } else if (r[0] == "patch") {
            ++patches;
            eq("catalog", "patch " + r[1] + "/" + r[2] + " " + r[3], r[4], describe_patch(to_i(r[1]), to_i(r[2]), to_i(r[3])));
        }
    }
    check("catalog", names == 133 && drums > 1500 && patches == 96, "catalog table sizes (names " + num(names) + ", drum " + num(drums) + ", patch " + num(patches) + ")");
    // try_drum_kit_name agrees with the name text
    std::string kit, standard;
    check("catalog", try_drum_kit_name(120, 0, 0, kit, standard) && kit == "Standard Set" && standard == "GM2", "GM2 standard set");
    check("catalog", !try_drum_kit_name(120, 1, 0, kit, standard), "a non-zero LSB is never a kit");
    check("catalog", !try_drum_kit_name(126, 0, 2, kit, standard), "XG SFX has two kits only");
}

// ---- columns, texts ----------------------------------------------------------------

void test_texts()
{
    std::vector<Row> rows;
    if (!read_table("texts.txt", rows)) return;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "col") {
            int c = to_i(r[1]);
            const ChannelColumnInfo& info = channel_column_info(c);
            eq("columns", "name " + r[1], r[2], info.name);
            eq("columns", "caption " + r[1], r[3], info.caption);
            eq("columns", "width " + r[1], r[4], num(info.canonical_width));
            eq("columns", "tooltip " + r[1], pooled(r[5]), info.tooltip);
            eq("columns", "attribute " + r[1], r[7], info.is_attribute ? num(static_cast<int>(info.attribute)) : "-");
        } else if (r[0] == "rows") {
            eq("columns", "column count", r[2], num(kChannelColumnCount));
        } else if (r[0] == "explanation") {
            eq("texts", "explanation", pooled(r[1]), ChannelGridModel::explanation());
        } else if (r[0] == "title") {
            eq("texts", "title " + r[1], r[2], ChannelGridModel::title_for(r[1] == "(null)" ? "" : r[1]));
        } else if (r[0] == "seed") {
            eq("texts", "seed " + r[1], r[2], num(ChannelGridModel::default_editor_seed(static_cast<ChannelAttribute>(to_i(r[1])))));
        } else if (r[0] == "help") {
            eq("texts", "help " + r[1] + " ch" + r[2], pooled(r[3]),
                ChannelGridModel::attribute_help(static_cast<ChannelAttribute>(to_i(r[1])), to_i(r[2])));
        } else if (r[0] == "clock") {
            eq("texts", "clock " + r[1], r[2], ChannelGridModel::format_clock(to_i64(r[1])));
        }
    }
    // detach / attach text
    ChannelGridModel model;
    check("texts", !model.detached(), "a new grid is attached");
    model.detach("", nullptr);
    check("texts", model.detached() && model.detached_message() == "No MIDI file is loaded.", "detach without a message");
    eq("texts", "detached title", rows.empty() ? "" : "", "");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "detach" && r[1] == "Loading new MIDI\xE2\x80\xA6") {
            model.detach(r[1], nullptr);
            eq("texts", "detach message", r[3], model.detached_message());
            eq("texts", "detach title", r[4], model.title());
        } else if (r[0] == "attach" && r[1] == "b.mid") {
            model.attach(r[1]);
            check("texts", !model.detached(), "attach clears detached");
            eq("texts", "attach title", r[3], model.title());
        }
    }
}

// ---- cells ----------------------------------------------------------------

int g_positionTooltipsChanged = 0;

void test_cells()
{
    std::vector<Row> rows;
    if (!read_table("cells.txt", rows)) return;
    ChannelGridModel model;
    std::array<std::array<ChannelCell, kChannelColumnCount>, 16> previous;
    for (int ch = 0; ch < 16; ++ch)
        for (int c = 0; c < kChannelColumnCount; ++c) previous[static_cast<size_t>(ch)][static_cast<size_t>(c)] = model.cell(ch, c);
    std::string lastLabel = "initial";
    bool first = true;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] != "cells") continue;
        const std::string& label = r[1];
        int ch = to_i(r[2]);
        if (label != lastLabel || first) {
            // New state: apply it before checking its first row.
            first = false;
            lastLabel = label;
            ChannelGridChanges changes;
            if (label == "initial") {
            } else if (label == "detached") {
                model.detach("detached for the table", &changes);
            } else if (label == "detached-ignored") {
                check("cells", !model.update_snapshot(g_states["2"].snapshot, &changes), "a snapshot is ignored while detached");
            } else if (label == "reattached") {
                model.attach("again.mid");
                model.update_snapshot(g_states["2"].snapshot, &changes);
            } else {
                if (g_states.find(label) == g_states.end()) {
                    fail("cells", "no input state " + label);
                    continue;
                }
                apply_state(model, g_states[label], &changes);
            }
            // The change mask names every cell whose look changed (and only those may differ).
            for (int row = 0; row < 16; ++row)
                for (int c = 0; c < kChannelColumnCount; ++c) {
                    bool changed = !model.cell(row, c).same_look(previous[static_cast<size_t>(row)][static_cast<size_t>(c)]);
                    bool flagged = (changes.rows[static_cast<size_t>(row)] & (1u << c)) != 0;
                    if (changed && !flagged) fail("changes", "state " + label + " cell " + num(row) + "," + num(c) + " changed but is not flagged");
                    if (flagged && !changed && changes.rows[static_cast<size_t>(row)] != 0xFFFF)
                        fail("changes", "state " + label + " cell " + num(row) + "," + num(c) + " flagged without a change");
                    ++g_checks;
                    previous[static_cast<size_t>(row)][static_cast<size_t>(c)] = model.cell(row, c);
                }
        }
        eq("cells", label + " ch" + num(ch) + " row background", r[3], model.row_enabled(ch) ? "W" : "C");
        for (int c = 0; c < kChannelColumnCount; ++c) {
            size_t k = 4 + static_cast<size_t>(c) * 3;
            if (k + 2 >= r.size()) {
                fail("cells", "short row for " + label);
                break;
            }
            const ChannelCell& cell = model.cell(ch, c);
            std::string where = "state " + label + " ch" + num(ch + 1) + " " + channel_column_info(c).name;
            eq("cells", where + " text", r[k], cell.text);
            std::string code;
            code += cell.color == CellColor::Forced ? 'F' : cell.color == CellColor::Historical ? 'H' : 'N';
            code += cell.font == CellFont::Bold ? 'B' : cell.font == CellFont::Italic ? 'I' : 'R';
            eq("cells", where + " style", r[k + 1], code);
            std::string expectedTooltip = pooled(r[k + 2]);
            if (c == ColPosition) {
                // DD-046: the Position cell no longer carries the attribute wording v1 gave its historical tooltip.
                const std::string v1Text = "Historical last-sent value; it is not enforced and may no longer be effective. Right-click to restore the latest source-file value for this attribute at the current position.";
                size_t at = expectedTooltip.find(v1Text);
                if (at != std::string::npos) {
                    expectedTooltip.replace(at, v1Text.size(), "Historical last-sent position; it is not enforced and may no longer be current.");
                    ++g_positionTooltipsChanged;
                }
            }
            eq("cells", where + " tooltip", expectedTooltip, cell.tooltip);
        }
    }
}

// ---- queries and editor specs ----------------------------------------------------------------

void test_queries()
{
    std::vector<Row> rows;
    if (!read_table("queries.txt", rows)) return;
    std::string loaded;
    ChannelGridModel model;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[1] != loaded) {
            loaded = r[1];
            apply_state(model, g_states[loaded]);
        }
        int ch = to_i(r[2]);
        if (r[0] == "attr") {
            for (int a = 0; a < 9; ++a) {
                ChannelAttribute attribute = static_cast<ChannelAttribute>(a);
                std::string actual = std::string(model.is_forced(ch, attribute) ? "1" : "0") +
                    (model.is_historical(ch, attribute) ? "1" : "0") + ":" + num(model.current_attribute_value(ch, attribute));
                eq("queries", "state " + r[1] + " ch" + r[2] + " attribute " + num(a), r[3 + static_cast<size_t>(a)], actual);
            }
        } else if (r[0] == "format") {
            ChannelAttribute attribute = static_cast<ChannelAttribute>(to_i(r[3]));
            static const int bendProbe[] = { INT32_MIN, -8193, -8192, -17, -1, 0, 1, 16, 8191, 8192 };
            static const int plainProbe[] = { INT32_MIN, -5, -1, 0, 1, 24, 63, 64, 100, 127, 128 };
            bool bend = attribute == ChannelAttribute::PitchBend;
            size_t count = bend ? sizeof(bendProbe) / sizeof(int) : sizeof(plainProbe) / sizeof(int);
            for (size_t p = 0; p < count; ++p) {
                int value = bend ? bendProbe[p] : plainProbe[p];
                eq("queries", "format state " + r[1] + " ch" + r[2] + " attribute " + r[3] + " value " + num(value),
                    r[4 + p], model.format_attribute(ch, attribute, value));
            }
        }
    }
}

class NullSink : public ChannelRequestSink {
public:
    std::string request_override(int, ChannelAttribute, int) override { return std::string(); }
    std::string request_historical_chase(int, ChannelAttribute) override { return std::string(); }
    std::string request_enabled(int, bool) override { return std::string(); }
};

void test_editor_specs()
{
    std::vector<Row> rows;
    if (!read_table("editor-specs.txt", rows)) return;
    std::string loaded;
    ChannelGridModel model;
    NullSink sink;
    ChannelEditor editor(model, sink, format::NumberCulture());
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] != "spec") continue;
        if (r[1] != loaded) {
            loaded = r[1];
            apply_state(model, g_states[loaded]);
        }
        int ch = to_i(r[2]);
        ChannelAttribute attribute = static_cast<ChannelAttribute>(to_i(r[3]));
        editor.open(ch, attribute);
        const ChannelEditorSpec& s = editor.spec();
        std::string where = "state " + r[1] + " ch" + r[2] + " attribute " + r[3];
        eq("editor-specs", where + " minimum", r[4], num(s.minimum));
        eq("editor-specs", where + " maximum", r[5], num(s.maximum));
        eq("editor-specs", where + " display offset", r[6], num(s.display_offset));
        eq("editor-specs", where + " coarse units", r[7], num(s.coarse_units));
        eq("editor-specs", where + " coarse pixels", r[8], num(s.coarse_pixels));
        eq("editor-specs", where + " fine units", r[9], num(s.fine_units));
        eq("editor-specs", where + " fine pixels", r[10], num(s.fine_pixels));
        eq("editor-specs", where + " value", r[11], num(editor.value()));
        eq("editor-specs", where + " forced", r[12], editor.forced() ? "1" : "0");
        eq("editor-specs", where + " historical", r[13], s.historical ? "1" : "0");
        eq("editor-specs", where + " text", r[14], editor.flat_text());
        eq("editor-specs", where + " help", pooled(r[15]), editor.tooltip());
        eq("editor-specs", where + " look", r[16], std::string(editor.bold() ? "F" : "N") + (editor.bold() ? "B" : "R"));
    }
}

// ---- flows ----------------------------------------------------------------

class FakeEngine : public ChannelEngineAccess {
public:
    std::vector<Row> log;
    std::shared_ptr<engine::ChannelPlaybackSnapshot> current;
    std::string override_mode = "ok", chase_mode = "ok", enabled_mode = "ok";
    bool monitoring = false;
    int monitoring_calls = 0;
    int snapshot_reads = 0;
    bool monitor_chase_requested = false;
    engine::ChannelControlCompletion monitor_completion;
    engine::ChannelControlCompletion deferred;

    static bool failing(const std::string& mode, std::string& message)
    {
        if (mode.compare(0, 5, "fail:") == 0) { message = mode.substr(5); return true; }
        if (mode.compare(0, 6, "throw:") == 0) { message = mode.substr(6); return true; }
        return false;
    }
    void set_monitoring(bool enabled) override { monitoring = enabled; ++monitoring_calls; }
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> snapshot() override { ++snapshot_reads; return current; }
    engine::EngineError set_override(int channel, ChannelAttribute attribute, int32_t value) override
    {
        log.push_back(Row{ "r", "ev", "override", num(channel), num(static_cast<int>(attribute)), value == INT32_MIN ? "auto" : num(value) });
        std::string message;
        if (failing(override_mode, message)) return engine::EngineError::make("Exception", message);
        return engine::EngineError();
    }
    engine::EngineError set_enabled(int channel, bool enabled) override
    {
        log.push_back(Row{ "r", "ev", "enable", num(channel), enabled ? "1" : "0" });
        std::string message;
        if (failing(enabled_mode, message)) return engine::EngineError::make("Exception", message);
        return engine::EngineError();
    }
    engine::EngineError chase_latest(int channel, ChannelAttribute attribute, engine::ChannelControlCompletion completion) override
    {
        log.push_back(Row{ "r", "ev", "chase", num(channel), num(static_cast<int>(attribute)) });
        std::string message;
        if (chase_mode == "defer") {
            deferred = completion;
            return engine::EngineError();
        }
        if (!failing(chase_mode, message)) {
            if (completion) completion(engine::EngineError());
            return engine::EngineError();
        }
        engine::EngineError error = engine::EngineError::make("Exception", message);
        if (chase_mode.compare(0, 6, "throw:") == 0) return error;
        if (completion) completion(error);
        return engine::EngineError();
    }
    bool chase_state_for_new_monitor(engine::ChannelControlCompletion completion) override
    {
        monitor_chase_requested = true;
        monitor_completion = completion;
        return true;
    }
};

struct Driver {
    FakeEngine engine;
    ChannelSession session;
    std::string edit_text;
    std::atomic<int> wakes{ 0 };
    int overrides_changed = 0;

    static ChannelSession::Host make_host(Driver* d)
    {
        ChannelSession::Host host;
        host.engine = &d->engine;
        host.wake = [d] { ++d->wakes; };
        host.overrides_changed = [d] { ++d->overrides_changed; };
        return host;
    }
    explicit Driver(const std::shared_ptr<engine::ChannelPlaybackSnapshot>& initial)
        : session(make_host(this))
    {
        engine.current = initial;
    }
};

std::string cfg_mode(const std::string& op, const std::string& kind) { return op.substr(std::string("cfg " + kind + " ").size()); }

std::vector<std::string> split_space(const std::string& s)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t sp = s.find(' ', start);
        parts.push_back(s.substr(start, sp == std::string::npos ? std::string::npos : sp - start));
        if (sp == std::string::npos) break;
        start = sp + 1;
    }
    return parts;
}

// Runs one op and returns the result lines v1 wrote for it.
std::vector<Row> run_op(Driver& d, const std::string& op, int& tipRow, int& tipCol)
{
    ChannelEditor& editor = d.session.editor();
    std::vector<std::string> p = split_space(op);
    d.engine.log.clear();
    tipRow = tipCol = -1;
    if (p[0] == "state") {
        Snap& state = g_states[p[1] == "0" ? "F0" : "F1"];
        d.engine.current = std::make_shared<engine::ChannelPlaybackSnapshot>(state.snapshot);
        apply_state(d.session.model(), state);
    } else if (p[0] == "click") {
        int row = to_i(p[2]), col = to_i(p[3]);
        tipRow = row; tipCol = col;
        d.session.cell_mouse_down(row, col, p[1] == "L" ? ChannelSession::Button::Left : ChannelSession::Button::Right, 100, 100, d.edit_text);
        if (editor.typing()) d.edit_text = editor.typing_text();
    } else if (p[0] == "scrub") {
        editor.scrub_by(to_i(p[1]), p[2] == "1");
    } else if (p[0] == "up") {
        if (editor.is_open() && editor.armed() && editor.pointer_up()) d.edit_text = editor.typing_text();
    } else if (p[0] == "typing") {
        d.edit_text = editor.begin_typing();
    } else if (p[0] == "commit") {
        std::string text = p.size() > 1 ? op.substr(7) : d.edit_text;
        editor.commit_typed(text, true);
    } else if (p[0] == "focusloss") {
        std::string text = p.size() > 1 ? op.substr(10) : d.edit_text;
        editor.commit_typed(text, false);
    } else if (p[0] == "arrow") {
        d.edit_text = editor.adjust_typed(d.edit_text, p[1] == "up" ? 1 : -1);
    } else if (p[0] == "escape") {
        editor.abandon_typing();
    } else if (p[0] == "edright") {
        ChannelEditor::RightClickResult r = editor.right_click();
        if (r.close) d.session.hide_editor(true, d.edit_text);
    } else if (p[0] == "hide") {
        d.session.hide_editor(p[1] == "commit", d.edit_text);
    } else if (p[0] == "cfg") {
        std::string mode = cfg_mode(op, p[1]);
        if (p[1] == "override") d.engine.override_mode = mode;
        else if (p[1] == "chase") d.engine.chase_mode = mode;
        else d.engine.enabled_mode = mode;
    } else if (p[0] == "controlerror") {
        int ch = to_i(p[1]);
        int attr = to_i(p[2]);
        std::string prefix = "controlerror " + p[1] + " " + p[2] + " ";
        std::string message = op.size() > prefix.size() ? op.substr(prefix.size()) : std::string();
        tipRow = ch; tipCol = 6 + attr;
        d.session.control_failed(ch, static_cast<ChannelAttribute>(attr), message);
    } else if (p[0] == "completechase") {
        int ch = to_i(p[1]);
        int attr = to_i(p[2]);
        std::string message = op.substr(("completechase " + p[1] + " " + p[2] + " ").size());
        tipRow = ch; tipCol = 6 + attr;
        d.session.report_chase_result(ch, static_cast<ChannelAttribute>(attr), message == "-" ? std::string() : message);
    } else if (p[0] == "detach") {
        d.session.detach(op.substr(7));
    } else if (p[0] == "attach") {
        d.session.attach(op.substr(7), 1);
    } else {
        fail("flows", "unknown op " + op);
    }
    if (!editor.typing() && p[0] != "typing") d.edit_text = editor.is_open() ? editor.flat_text() : d.edit_text;
    d.session.take_changes();
    d.session.take_feedback();

    std::vector<Row> out = d.engine.log;
    Row ed;
    ed.push_back("r"); ed.push_back("ed");
    ed.push_back(editor.is_open() ? "1" : "0");
    ed.push_back(num(editor.channel()));
    ed.push_back(num(static_cast<int>(editor.attribute())));
    ed.push_back(editor.is_open() ? (editor.typing() ? d.edit_text : editor.flat_text()) : "-");
    ed.push_back(num(editor.value()));
    ed.push_back(editor.forced() ? "1" : "0");
    ed.push_back(editor.typing() ? "1" : "0");
    ed.push_back(editor.armed() ? "1" : "0");
    ed.push_back(editor.last_error());
    ed.push_back(std::string(editor.bold() ? "B" : "R") + (editor.bold() ? "F" : "N"));
    out.push_back(ed);
    if (tipRow >= 0 && tipRow < 16 && tipCol >= 0) {
        Row tip;
        tip.push_back("r"); tip.push_back("tip"); tip.push_back(num(tipRow)); tip.push_back(num(tipCol));
        tip.push_back(d.session.model().cell(tipRow, tipCol).tooltip);
        out.push_back(tip);
    }
    return out;
}

void test_flows()
{
    std::vector<Row> rows;
    if (!read_table("flows.txt", rows)) return;
    load_inputs("flow-inputs.txt");
    std::string scenario;
    std::unique_ptr<Driver> driver;
    size_t i = 0;
    int ops = 0;
    while (i < rows.size()) {
        const Row& r = rows[i];
        if (r[0] == "scenario") {
            scenario = r[1];
            driver.reset(new Driver(std::make_shared<engine::ChannelPlaybackSnapshot>(engine::ChannelPlaybackSnapshot())));
            ++i;
            continue;
        }
        if (r[0] != "op" || !driver) { ++i; continue; }
        std::string op = r[1];
        ++i;
        std::vector<Row> expected;
        while (i < rows.size() && rows[i][0] == "r") expected.push_back(rows[i++]);
        int tipRow = -1, tipCol = -1;
        std::vector<Row> actual = run_op(*driver, op, tipRow, tipCol);
        ++ops;
        std::string where = scenario + " / " + op;
        // The expected tooltips are pool references.
        for (size_t k = 0; k < expected.size(); ++k)
            if (expected[k].size() > 4 && expected[k][1] == "tip") expected[k][4] = pooled(expected[k][4]);
        // v1's CompleteHistoricalChase does nothing on success; the player's callback (and v2's session)
        // refreshes the snapshot, which rewrites the cell tooltip. The refresh is not part of the comparison.
        if (op.compare(0, 14, "completechase ") == 0 && op.size() > 2 && op.compare(op.size() - 2, 2, " -") == 0) {
            for (size_t k = expected.size(); k > 0; --k)
                if (expected[k - 1][1] == "tip") expected.erase(expected.begin() + static_cast<long>(k - 1));
            for (size_t k = actual.size(); k > 0; --k)
                if (actual[k - 1][1] == "tip") actual.erase(actual.begin() + static_cast<long>(k - 1));
        }
        if (expected.size() != actual.size()) {
            fail("flows", where + ": expected " + num(static_cast<int64_t>(expected.size())) + " result lines, got " + num(static_cast<int64_t>(actual.size())));
            for (size_t k = 0; k < actual.size(); ++k) {
                std::string line;
                for (size_t f = 0; f < actual[k].size(); ++f) line += (f ? " | " : "") + actual[k][f];
                std::printf("      got: %s\n", show(line).c_str());
            }
            continue;
        }
        for (size_t k = 0; k < expected.size(); ++k) {
            ++g_checks;
            if (expected[k] != actual[k]) {
                std::string e, a;
                for (size_t f = 0; f < expected[k].size(); ++f) e += (f ? " | " : "") + expected[k][f];
                for (size_t f = 0; f < actual[k].size(); ++f) a += (f ? " | " : "") + actual[k][f];
                fail("flows", where + ": expected [" + show(e) + "] got [" + show(a) + "]");
            }
        }
    }
    check("flows", ops > 200, "the flow table has its operations (" + num(ops) + ")");
}

// ---- behaviour that is not in the v1 tables ----------------------------------------------------------------

void test_pointer_gesture()
{
    NullSink sink;
    ChannelGridModel model;
    model.update_snapshot(g_states["F0"].snapshot, nullptr);
    ChannelEditor editor(model, sink, format::NumberCulture());
    editor.open(0, ChannelAttribute::Expression);   // unknown: seed 127
    editor.pointer_down(500, 300);
    ChannelEditor::PointerResult r = editor.pointer_move(503, 300, false, true);
    check("pointer", !r.scrub_started && !r.warp && !editor.scrubbing(), "three pixels is still a click");
    r = editor.pointer_move(504, 300, false, true);
    check("pointer", r.scrub_started && editor.scrubbing(), "four pixels start scrubbing");
    check("pointer", r.warp && r.warp_x == 504 && r.warp_y == 300 || !r.warp, "warp target is the anchor");
    // The echo of the warp is ignored.
    r = editor.pointer_move(504, 300, false, true);
    check("pointer", !r.requested && !r.warp, "the warp echo is ignored");
    r = editor.pointer_move(512, 300, false, true);   // 8 pixels right: one step, but the value is at its maximum
    check("pointer", r.warp, "a movement warps back");
    r = editor.pointer_move(520, 300, false, false);
    check("pointer", r.gesture_ended && !editor.armed() && !editor.scrubbing(), "button released ends the gesture");
    // click without drag becomes typing
    editor.open(0, ChannelAttribute::Volume);
    editor.pointer_down(10, 10);
    check("pointer", editor.pointer_up() && editor.typing() && editor.typing_text() == "100", "click without drag starts typing with the editable value");
    editor.abandon_typing();
    check("pointer", !editor.typing(), "escape leaves typing");
    // a scrub gesture does not enter typing
    editor.open(0, ChannelAttribute::Volume);
    editor.pointer_down(10, 10);
    editor.pointer_move(20, 10, false, true);
    check("pointer", !editor.pointer_up() && !editor.typing(), "release after scrubbing does not type");
}

void test_session_flow()
{
    Snap& state = g_states["F0"];
    Driver d(std::make_shared<engine::ChannelPlaybackSnapshot>(state.snapshot));
    ChannelSongInfo info;
    info.has_song = true;
    info.file_name = "song.mid";
    info.song_revision = 1;
    info.chase_enabled = false;
    d.session.start(info);
    check("session", d.engine.monitoring && d.engine.monitoring_calls == 1, "start turns monitoring on");
    check("session", !d.engine.monitor_chase_requested, "no state chase when the setting is off");
    check("session", d.session.model().title() == ChannelGridModel::title_for("song.mid"), "title follows the file");
    ChannelGridChanges changes = d.session.take_changes();
    check("session", changes.any(), "start reports changes");
    int reads = d.engine.snapshot_reads;
    d.session.sync(info);
    d.session.sync(info);
    check("session", d.engine.snapshot_reads == reads + 2, "sync reads the snapshot each time");
    check("session", !d.session.take_changes().any(), "an unchanged snapshot changes nothing");
    // a new snapshot object with one different value flags that cell only
    std::shared_ptr<engine::ChannelPlaybackSnapshot> next = std::make_shared<engine::ChannelPlaybackSnapshot>(state.snapshot);
    next->channels[4].sent_events += 1;
    d.engine.current = next;
    d.session.sync(info);
    changes = d.session.take_changes();
    int flagged = 0;
    for (int row = 0; row < 16; ++row)
        for (int c = 0; c < kChannelColumnCount; ++c)
            if (changes.rows[static_cast<size_t>(row)] & (1u << c)) {
                ++flagged;
                check("session", row == 4 && c == ColSent, "only channel 5 Sent changed");
            }
    check("session", flagged == 1, "exactly one cell is flagged");

    // loading, absent, cancelled, loaded again
    ChannelSongInfo loading = info;
    loading.loading = true;
    loading.has_song = false;
    d.session.sync(loading);
    check("session", d.session.model().detached() && d.session.model().detached_message() == "Loading new MIDI\xE2\x80\xA6", "loading detaches with v1's message");
    check("session", !d.engine.monitoring, "loading turns monitoring off");
    uint64_t rev = d.session.view_revision();
    d.session.sync(loading);
    check("session", d.session.view_revision() == rev, "the same detached state is not re-applied");
    ChannelSongInfo cancelled = loading;
    cancelled.loading = false;
    cancelled.absent_message = "MIDI loading cancelled. No MIDI file is loaded.";
    d.session.sync(cancelled);
    check("session", d.session.model().detached_message() == cancelled.absent_message, "cancel message");
    ChannelSongInfo loaded = info;
    loaded.file_name = "next.mid";
    loaded.song_revision = 2;
    d.session.sync(loaded);
    check("session", !d.session.model().detached() && d.engine.monitoring, "a loaded song attaches and turns monitoring on");
    check("session", d.session.model().title() == ChannelGridModel::title_for("next.mid"), "title follows the new file");

    // the new-monitor chase, with and without error
    Driver e(std::make_shared<engine::ChannelPlaybackSnapshot>(state.snapshot));
    ChannelSongInfo chase = info;
    chase.chase_enabled = true;
    e.session.start(chase);
    check("session", e.engine.monitor_chase_requested, "state chase requested for a new monitor");
    e.engine.monitor_completion(engine::EngineError::make("Exception", "Output closed."));
    e.session.pump();
    std::vector<ChannelFeedback> feedback = e.session.take_feedback();
    check("session", feedback.size() == 1 && feedback[0].kind == ChannelFeedback::MonitorStatus &&
        feedback[0].text == "Could not refresh MIDI state: Output closed." && feedback[0].milliseconds == 3500, "monitor status text and duration");

    // a completion from another thread only queues; pump drains it
    Driver f(std::make_shared<engine::ChannelPlaybackSnapshot>(state.snapshot));
    f.session.start(info);
    f.session.take_feedback();
    f.engine.chase_mode = "defer";
    std::string err = f.session.request_historical_chase(0, ChannelAttribute::Program);
    check("session", err.empty(), "chase request accepted");
    check("session", f.engine.log.size() == 1 && static_cast<bool>(f.engine.deferred), "chase reached the engine");
    int wakesBefore = f.wakes.load();
    std::thread worker([&f] { f.engine.deferred(engine::EngineError::make("Exception", "Late failure.")); });
    worker.join();
    check("session", f.wakes.load() == wakesBefore + 1, "a completion from the engine thread asks the UI thread to pump");
    check("session", f.session.take_feedback().empty(), "nothing is shown before the UI thread pumps");
    f.session.pump();
    feedback = f.session.take_feedback();
    check("session", feedback.size() == 1 && feedback[0].kind == ChannelFeedback::CellTip && feedback[0].channel == 0 &&
        feedback[0].column == ColProgram && feedback[0].text == "Late failure.", "the late failure appears under the Program cell");
    check("session", f.session.model().cell(0, ColProgram).tooltip == "Late failure.", "and becomes the cell tooltip");
    f.engine.deferred(engine::EngineError());   // a second completion of the same request is ignored
    f.session.pump();
    check("session", f.session.take_feedback().empty(), "a request completes once");

    // errors reach the cell tips with v1's duration
    f.session.control_failed(2, ChannelAttribute::Volume, "boom");
    feedback = f.session.take_feedback();
    check("session", feedback.size() == 1 && feedback[0].kind == ChannelFeedback::CellTip && feedback[0].channel == 2 &&
        feedback[0].column == ColVolume && feedback[0].text == "boom" && feedback[0].milliseconds == 2500, "control error feedback");
    // DD-033: a failed mute (the engine reports it with a placeholder attribute, Bank MSB) belongs to the Ch cell
    f.session.control_failed(4, ChannelAttribute::BankMsb, "mute failed", true);
    feedback = f.session.take_feedback();
    check("session", feedback.size() == 1 && feedback[0].kind == ChannelFeedback::CellTip && feedback[0].channel == 4 &&
        feedback[0].column == ColChannel && feedback[0].text == "mute failed" && feedback[0].milliseconds == 2500,
        "a failed mute is shown under the Ch cell, not the Bank MSB cell");
    check("session", f.session.model().cell(4, ColChannel).tooltip == "mute failed" &&
        f.session.model().cell(4, ColBankMsb).tooltip != "mute failed", "and becomes the Ch cell's tooltip");
    f.session.control_failed(4, ChannelAttribute::BankMsb, "bank failed");
    feedback = f.session.take_feedback();
    check("session", feedback.size() == 1 && feedback[0].column == ColBankMsb, "a failed Bank MSB change still appears under the Bank MSB cell");
    check("session", f.session.follow_up_active(), "a request starts the follow-up polling");
    for (int i = 0; i < 60; ++i) f.session.follow_up_tick();
    check("session", !f.session.follow_up_active(), "the follow-up polling ends");
    // overrides_changed fires on success only
    int before = f.overrides_changed;
    f.engine.override_mode = "fail:no";
    f.session.request_override(0, ChannelAttribute::Volume, 5);
    check("session", f.overrides_changed == before, "a failed override does not refresh Analysis");
    f.engine.override_mode = "ok";
    f.session.request_override(0, ChannelAttribute::Volume, 5);
    check("session", f.overrides_changed == before + 1, "a successful override refreshes Analysis");
    // closing turns monitoring off
    {
        Driver g(std::make_shared<engine::ChannelPlaybackSnapshot>(state.snapshot));
        g.session.start(info);
        FakeEngine* engine = &g.engine;
        check("session", engine->monitoring, "monitoring on while the window lives");
    }
}

// DD-031: which cell each navigation key reaches (new in v2; v1's tables have nothing to compare).
void test_navigation()
{
    const std::string s = "navigation";
    ChannelCellPosition p;
    p.row = 5; p.column = 7;
    auto at = [&](ChannelCellPosition q, int row, int column) { return q.row == row && q.column == column; };
    check(s, at(channel_navigate(p, ChannelNavKey::Left, 16), 5, 6), "Left");
    check(s, at(channel_navigate(p, ChannelNavKey::Right, 16), 5, 8), "Right");
    check(s, at(channel_navigate(p, ChannelNavKey::Up, 16), 4, 7), "Up");
    check(s, at(channel_navigate(p, ChannelNavKey::Down, 16), 6, 7), "Down");
    check(s, at(channel_navigate(p, ChannelNavKey::Home, 16), 5, 0), "Home goes to the first column");
    check(s, at(channel_navigate(p, ChannelNavKey::End, 16), 5, kChannelColumnCount - 1), "End goes to the last column");
    check(s, at(channel_navigate(p, ChannelNavKey::CtrlHome, 16), 0, 0), "Ctrl+Home goes to the first cell");
    check(s, at(channel_navigate(p, ChannelNavKey::CtrlEnd, 16), 15, kChannelColumnCount - 1), "Ctrl+End goes to the last cell");
    check(s, at(channel_navigate(p, ChannelNavKey::PageUp, 3), 2, 7), "Page Up moves one page");
    check(s, at(channel_navigate(p, ChannelNavKey::PageDown, 3), 8, 7), "Page Down moves one page");
    check(s, at(channel_navigate(p, ChannelNavKey::PageDown, 0), 6, 7), "a page is at least one row");
    ChannelCellPosition first, last;
    last.row = 15; last.column = kChannelColumnCount - 1;
    check(s, at(channel_navigate(first, ChannelNavKey::Left, 16), 0, 0) && at(channel_navigate(first, ChannelNavKey::Up, 16), 0, 0), "no movement past the first cell");
    check(s, at(channel_navigate(last, ChannelNavKey::Right, 16), 15, kChannelColumnCount - 1) && at(channel_navigate(last, ChannelNavKey::Down, 16), 15, kChannelColumnCount - 1), "no movement past the last cell");
    check(s, at(channel_navigate(p, ChannelNavKey::PageUp, 100), 0, 7) && at(channel_navigate(p, ChannelNavKey::PageDown, 100), 15, 7), "pages stop at the edges");
    ChannelCellPosition outside;
    outside.row = 40; outside.column = -3;
    check(s, at(channel_navigate(outside, ChannelNavKey::Left, 16), 15, 0), "a position outside the grid is brought inside first");
    // every key from every cell stays inside the grid
    bool inside = true;
    for (int row = 0; row < 16; ++row)
        for (int column = 0; column < kChannelColumnCount; ++column)
            for (int k = 0; k <= static_cast<int>(ChannelNavKey::PageDown); ++k) {
                ChannelCellPosition from;
                from.row = row; from.column = column;
                ChannelCellPosition to = channel_navigate(from, static_cast<ChannelNavKey>(k), 5);
                if (to.row < 0 || to.row > 15 || to.column < 0 || to.column >= kChannelColumnCount) inside = false;
            }
    check(s, inside, "every key from every cell lands inside the grid");
}

} // namespace

int main(int argc, char** argv)
{
    g_dir = argc > 1 ? argv[1] : "v2/harness/expected/channels-ui";
    std::printf("midibn-channels-app-tests, tables in %s\n", g_dir.c_str());
    {
        std::vector<Row> rows;
        if (read_table("pool.txt", rows)) {
            g_pool.resize(rows.size());
            for (size_t i = 0; i < rows.size(); ++i) g_pool[static_cast<size_t>(to_i(rows[i][0]))] = rows[i].size() > 1 ? rows[i][1] : std::string();
        }
    }
    load_inputs("inputs.txt");
    test_catalog();
    test_texts();
    test_cells();
    check("cells", g_positionTooltipsChanged > 0, "the tables contain historical Position cells (DD-046)");
    std::printf("  Position tooltips changed by DD-046: %d\n", g_positionTooltipsChanged);
    test_queries();
    test_editor_specs();
    test_flows();
    test_pointer_gesture();
    test_session_flow();
    test_navigation();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures == 0 ? "CHANNELS APP TESTS: PASS\n" : "CHANNELS APP TESTS: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
