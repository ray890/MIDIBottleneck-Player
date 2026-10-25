// Tests of the Track routing editor's state machine (app::RoutingDialogModel) against
// v1's own TrackRoutingForm. harness/routing-ui-reference drives v1's dialog through
// scripted sessions and records the dialog state after every action
// (harness/expected/routing-ui/<fixture>.txt, RoutingUiReference.cs documents the format);
// this program replays the same actions against the model and requires identical states.
// It also checks the plan flow (stale results are ignored, failures keep Apply off).
//
// A few states differ from v1 on purpose (register entry DD-044: a track name is cut between whole characters).
// Those are listed in harness\expected-v2-ui\routing-ui\explained-states.txt (key line, then v2's state line);
// such a state must still differ from v1's table, and must equal the listed line.
//
// Usage: midibn-routing-app-tests [fixtureDirectory [tablesDirectory [explainedStatesFile]]] [--write-explained FILE]
//        (defaults: v2\harness\fixtures, v2\harness\expected\routing-ui,
//         v2\harness\expected-v2-ui\routing-ui\explained-states.txt;
//         --write-explained writes the states that differ from v1 instead of failing on them)
#include "midibn/app/routing_dialog_model.h"
#include "midibn/midi_loader.h"
#include "win32_platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace midibn;
using namespace midibn::app;

namespace {

int g_failures = 0;
int g_checks = 0;
int g_sessions = 0;
int g_states = 0;
int g_explained = 0;
std::map<std::string, std::string> g_explainedStates;   // "table<TAB>session<TAB>what" -> v2's line
std::map<std::string, bool> g_explainedUsed;
std::map<std::string, int> g_occurrence;
std::string g_writeExplainedPath;
std::vector<std::string> g_writtenExplained;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        if (g_failures <= 40) std::printf("FAIL %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------- text helpers

std::string esc(const std::u16string& value)
{
    std::string out;
    for (char16_t c : value) {
        if (c == u'\\') out += "\\\\";
        else if (c == u'\t') out += "\\t";
        else if (c == u'\r') out += "\\r";
        else if (c == u'\n') out += "\\n";
        else if (c < 0x20 || c > 0x7E) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        }
        else out += static_cast<char>(c);
    }
    return out;
}

std::string digest(const std::u16string& text)
{
    uint64_t hash = 14695981039346656037ULL;
    for (char16_t c : text) {
        hash ^= static_cast<uint64_t>(c & 0xFF);
        hash *= 1099511628211ULL;
        hash ^= static_cast<uint64_t>(c >> 8);
        hash *= 1099511628211ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::u16string ascii16(const std::string& text) { return std::u16string(text.begin(), text.end()); }

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == separator) { parts.push_back(current); current.clear(); }
        else current += c;
    }
    parts.push_back(current);
    return parts;
}

std::string lane_token(const routing::TrackLaneKey& lane)
{
    return std::to_string(lane.track) + "." + std::to_string(lane.source_channel);
}

std::string settings_text(const routing::TrackRoutingSettings& s)
{
    int mask = 0;
    for (int channel = 0; channel < 16; ++channel)
        if (s.usable_destination_channels[static_cast<size_t>(channel)]) mask |= 1 << channel;
    std::vector<std::string> parts;
    for (const auto& pair : s.manual_destinations) parts.push_back(lane_token(pair.first) + "=" + std::to_string(pair.second));
    std::sort(parts.begin(), parts.end());
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%04X", mask);
    std::string manual = "-";
    for (size_t i = 0; i < parts.size(); ++i) manual = (i == 0 ? "" : manual + ";") + parts[i];
    return "tier=" + std::to_string(static_cast<int>(s.maximum_automatic_tier)) + ",cap=" + std::to_string(s.target_capacity) +
        ",perc=" + std::to_string(static_cast<int>(s.percussion_policy)) + ",usable=" + hex + ",manual=" + manual;
}

routing::TrackRoutingSettings parse_settings(const std::string& text)
{
    routing::TrackRoutingSettings settings;
    for (const std::string& part : split(text, ',')) {
        size_t eq = part.find('=');
        std::string key = part.substr(0, eq), value = part.substr(eq + 1);
        if (key == "tier") settings.maximum_automatic_tier = static_cast<routing::TrackRoutingTier>(std::atoi(value.c_str()));
        else if (key == "cap") settings.target_capacity = std::atoi(value.c_str());
        else if (key == "perc") settings.percussion_policy = static_cast<routing::PercussionRoutingPolicy>(std::atoi(value.c_str()));
        else if (key == "usable") {
            int mask = static_cast<int>(std::strtol(value.c_str(), nullptr, 16));
            for (int channel = 0; channel < 16; ++channel)
                settings.usable_destination_channels[static_cast<size_t>(channel)] = (mask & (1 << channel)) != 0;
        }
        else if (key == "manual" && value != "-") {
            for (const std::string& entry : split(value, ';')) {
                size_t dot = entry.find('.'), equal = entry.find('=');
                routing::TrackLaneKey lane(std::atoi(entry.substr(0, dot).c_str()), std::atoi(entry.substr(dot + 1, equal - dot - 1).c_str()));
                settings.manual_destinations[lane] = std::atoi(entry.substr(equal + 1).c_str());
            }
        }
    }
    return settings;
}

// ---------------------------------------------------------------- the song

struct LoadedSong {
    std::shared_ptr<Song> song;
    routing::RoutingSong routing;
};

bool load_fixture(const std::string& path, LoadedSong& out)
{
    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    if (!source) { std::printf("cannot open %s: %s\n", path.c_str(), error.c_str()); return false; }
    out.song = std::make_shared<Song>();
    LoadError loadError;
    if (!load_song(*source, LoadOptions(), *out.song, loadError)) {
        std::printf("cannot load %s: %s\n", path.c_str(), loadError.message.c_str());
        return false;
    }
    out.routing.events = models::EventSource(&out.song->store);
    out.routing.track_count = out.song->track_count;
    out.routing.duration_us = out.song->duration_us;
    out.routing.tracks = &out.song->tracks;
    out.routing.identity = out.song.get();
    return true;
}

// ---------------------------------------------------------------- the state line (RoutingUiReference.cs State())

std::string state_line(const RoutingDialogModel& model)
{
    std::u16string rows;
    for (const RoutingDialogRow& r : model.rows()) {
        rows += ascii16(lane_token(r.row.lane));
        rows += u"|" + r.row.lane_text + u"|" + r.row.patch_text + u"|" + r.row.counts_text + u"|" + r.row.destination_text +
            u"|" + r.row.status_text + u"|" + ascii16(std::to_string(static_cast<int>(r.row.style))) + u"|" + r.patch_tooltip + u"\n";
    }
    const routing::CompiledTrackRoute* compiled = model.compiled();
    bool valid = compiled != nullptr && compiled->is_valid();
    std::string used, usable;
    for (int channel = 0; channel < 16; ++channel) {
        used += model.used()[static_cast<size_t>(channel)] ? '1' : '0';
        usable += model.usable()[static_cast<size_t>(channel)] ? '1' : '0';
    }
    std::string line = "S";
    line += "\t" + std::to_string(model.rows().size());
    line += "\t" + digest(rows);
    line += "\t" + digest(model.preview());
    line += "\t" + used;
    line += "\t" + usable;
    line += "\t" + std::to_string(model.tier_index()) + "," + std::to_string(model.capacity()) + "," + std::to_string(model.percussion_index());
    line += "\t" + (model.selected_row() < 0 ? std::string("-") : std::to_string(model.selected_row()));
    line += std::string("\t") + (model.destination_enabled() ? "1" : "0") + "," + std::to_string(model.destination_index());
    line += std::string("\t") + (valid ? "1" : "0") + (valid && compiled->requires_explicit_apply ? "1" : "0") +
        (model.apply_enabled() ? "1" : "0") + (model.status_is_error() ? "1" : "0") + (model.lossy_confirmation_checked() ? "1" : "0") +
        (model.original_enabled() ? "1" : "0");
    line += "\t" + esc(model.status_text());
    line += "\t" + esc(RoutingDialogTexts::lane_caption());
    return line;
}

std::string outcome_line(const RoutingDialogModel& model)
{
    const RoutingDialogOutcome& outcome = model.outcome();
    std::string kind = outcome.kind == RoutingDialogOutcome::UseOriginal ? "original"
        : outcome.kind == RoutingDialogOutcome::Apply ? "apply" : "none";
    return "O\t" + kind + "\t" + (outcome.explicitly_confirmed ? "1" : "0") + "\t" +
        (outcome.kind == RoutingDialogOutcome::Apply ? settings_text(outcome.settings) : std::string("-"));
}

format::NumberCulture culture_named(const std::string& name)
{
    format::NumberCulture culture;
    if (name == "de-DE") { culture.decimal_separator = ","; culture.group_separator = "."; }
    return culture;
}

void do_action(RoutingDialogModel& model, const std::string& action)
{
    std::vector<std::string> f = split(action, ' ');
    if (f[0] == "tier") model.set_tier(std::atoi(f[1].c_str()));
    else if (f[0] == "cap") model.set_capacity(std::atoi(f[1].c_str()));
    else if (f[0] == "perc") model.set_percussion(std::atoi(f[1].c_str()));
    else if (f[0] == "usable") model.set_usable(std::atoi(f[1].c_str()), f[2] == "1");
    else if (f[0] == "select") model.select_row(std::atoi(f[1].c_str()));
    else if (f[0] == "dest") model.set_selected_lane_destination(std::atoi(f[1].c_str()));
    else if (f[0] == "confirm") model.set_lossy_confirmed(f[1] == "1");
    else if (f[0] == "apply") model.apply();
    else if (f[0] == "original") model.use_original();
    else check(false, "unknown action " + action);
    // The plan is computed synchronously here (the dialog does it on a worker thread).
    if (model.plan_pending()) model.recompute_now();
}

// ---------------------------------------------------------------- a table

struct Reader {
    std::vector<std::string> lines;
    size_t at = 0;
    bool more() const { return at < lines.size(); }
    const std::string& peek() const { return lines[at]; }
    const std::string& next() { return lines[at++]; }
};

void expect_line(const std::string& table, int session, const std::string& what, const std::string& expected, const std::string& actual)
{
    ++g_states;
    if (expected != actual) {
        // The same action text can occur more than once in a session: the key carries the occurrence number.
        const std::string base = table + "\t" + std::to_string(session) + "\t" + what;
        const std::string key = base + "\t#" + std::to_string(++g_occurrence[base]);
        if (!g_writeExplainedPath.empty()) {
            g_writtenExplained.push_back(key);
            g_writtenExplained.push_back(actual);
            ++g_checks;
            return;
        }
        std::map<std::string, std::string>::const_iterator known = g_explainedStates.find(key);
        if (known != g_explainedStates.end() && known->second == actual) {
            g_explainedUsed[key] = true;
            ++g_explained;
            ++g_checks;
            return;
        }
        check(false, table + " session " + std::to_string(session) + " " + what + "\n  expected: " + expected.substr(0, 400) + "\n  actual:   " + actual.substr(0, 400));
    }
    else ++g_checks;
}

void run_table(const std::string& fixtureDir, const std::string& tablePath, const std::string& tableName)
{
    std::ifstream file(tablePath, std::ios::binary);
    if (!file) { check(false, "cannot open " + tablePath); return; }
    Reader reader;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        reader.lines.push_back(line);
    }
    std::string fixture;
    LoadedSong loaded;
    while (reader.more()) {
        const std::string& head = reader.next();
        if (head.empty() || head[0] == '#') continue;
        std::vector<std::string> f = split(head, '\t');
        if (f[0] == "fixture") {
            fixture = f[1];
            if (!load_fixture(fixtureDir + "\\" + fixture, loaded)) { check(false, "fixture " + fixture); return; }
            continue;
        }
        if (f[0] != "session") { check(false, tableName + ": unexpected line " + head.substr(0, 80)); return; }
        int number = std::atoi(f[1].c_str());
        routing::TrackRoutingSettings initial = parse_settings(f[2].substr(5));
        bool active = f[3] == "active=1";
        std::string cultureName = f[4].substr(8);
        ++g_sessions;
        RoutingDialogModel model(loaded.routing, initial, active, culture_named(cultureName));
        model.recompute_now();
        // The first line is the dialog's creation: its state, or the exception v1 threw.
        const std::string& first = reader.next();
        if (first.compare(0, 2, "X\t") == 0) {
            std::vector<std::string> x = split(first, '\t');
            check(model.plan_has_failed(), tableName + " session " + std::to_string(number) + ": v1 could not build the plan, v2 did");
            std::string message;
            for (char16_t c : model.failure_message()) message += static_cast<char>(c);
            expect_line(tableName, number, "failure message", "X\t" + x[1] + "\t" + x[2], "X\t" + x[1] + "\t" + esc(model.failure_message()));
            check(!model.apply_enabled() && model.status_is_error() && model.rows().empty(), tableName + ": failed plan keeps Apply off");
            check(!model.apply() && !model.finished(), tableName + ": Apply refused after a failed plan");
            // consume "end"
            if (reader.more() && reader.peek() == "end") reader.next();
            continue;
        }
        expect_line(tableName, number, "initial state", first, state_line(model));
        while (reader.more()) {
            const std::string& entry = reader.next();
            if (entry == "end") break;
            if (entry.compare(0, 2, "A\t") == 0) {
                std::string action = entry.substr(2);
                do_action(model, action);
                const std::string& expectedState = reader.next();
                expect_line(tableName, number, "after '" + action + "'", expectedState, state_line(model));
            }
            else if (entry.compare(0, 2, "O\t") == 0) {
                expect_line(tableName, number, "outcome", entry, outcome_line(model));
                check(model.finished(), tableName + ": dialog closed");
            }
            else check(false, tableName + ": unexpected entry " + entry.substr(0, 60));
        }
    }
}

// ---------------------------------------------------------------- plan flow

void test_plan_flow(const std::string& fixtureDir)
{
    LoadedSong loaded;
    if (!load_fixture(fixtureDir + "\\routing-lanes.mid", loaded)) { check(false, "routing-lanes.mid"); return; }
    RoutingDialogModel model(loaded.routing, routing::TrackRoutingSettings(), true);
    check(model.plan_pending() && !model.apply_enabled() && model.rows().empty(), "nothing is shown before the first plan");
    check(model.status_text() == u"Calculating the routing plan…" && !model.status_is_error(), "pending status text");
    check(!model.apply(), "Apply refused while the first plan is pending");
    uint64_t first = model.generation();
    model.set_tier(2);
    uint64_t second = model.generation();
    check(second == first + 1 && model.plan_pending(), "a setting change starts a new generation");
    // A result for the older generation is ignored.
    std::shared_ptr<const routing::CompiledTrackRoute> stale = routing::compile_route(loaded.routing, routing::TrackRoutingSettings());
    check(!model.plan_ready(first, stale) && model.plan_pending(), "stale plan ignored");
    std::shared_ptr<const routing::CompiledTrackRoute> fresh = routing::compile_route(loaded.routing, model.settings());
    check(model.plan_ready(second, fresh) && !model.plan_pending(), "current plan accepted");
    check(model.rows().size() == fresh->lanes.size() && model.selected_row() == 0, "rows built, first row selected");
    // The confirmation is cleared by every change and enables Apply only for a lossy plan.
    check(model.lossy_confirmation_visible() == (fresh->is_valid() && fresh->requires_explicit_apply), "confirmation visible for a valid lossy plan");
    if (fresh->is_valid() && fresh->requires_explicit_apply) {
        check(!model.apply_enabled(), "lossy plan needs the confirmation");
        model.set_lossy_confirmed(true);
        check(model.apply_enabled(), "confirmed lossy plan can be applied");
        model.set_usable(0, false);
        check(!model.lossy_confirmation_checked() && model.plan_pending() && !model.apply_enabled(), "a change clears the confirmation and pends the plan");
        model.recompute_now();
    }
    // Selecting a row and a destination keeps the selection across the new plan.
    model.set_tier(0);
    model.recompute_now();
    model.select_row(3);
    routing::TrackLaneKey lane;
    check(model.selected_lane(lane), "row 3 selected");
    model.set_selected_lane_destination(5);
    check(model.plan_pending() && model.destination_index() == 5, "manual destination recorded at once");
    model.recompute_now();
    routing::TrackLaneKey after;
    check(model.selected_lane(after) && after == lane, "selection follows the lane across the new plan");
    check(model.settings().manual_destinations.count(lane) == 1 && model.settings().manual_destinations.at(lane) == 4, "output channel 5 is destination index 4");
    model.set_selected_lane_destination(0);
    check(model.settings().manual_destinations.count(lane) == 0, "Automatic suggestion removes the manual destination");
    // Use original routing is available only with an active route.
    RoutingDialogModel noRoute(loaded.routing, routing::TrackRoutingSettings(), false);
    check(!noRoute.original_enabled() && !noRoute.use_original(), "Use original routing disabled without an active route");
    check(model.original_enabled() && model.use_original() && model.outcome().kind == RoutingDialogOutcome::UseOriginal, "Use original routing closes the dialog");
}

void test_failure_flow(const std::string& fixtureDir)
{
    LoadedSong loaded;
    if (!load_fixture(fixtureDir + "\\routing-patches-4097.mid", loaded)) { check(false, "routing-patches-4097.mid"); return; }
    RoutingDialogModel model(loaded.routing, routing::TrackRoutingSettings(), false);
    model.recompute_now();
    check(model.plan_has_failed() && !model.plan_pending(), "planner limit reported");
    check(model.status_is_error() && !model.apply_enabled(), "failed plan: error status, Apply off");
    check(model.status_text().find(u"Track-routing analysis exceeded the bounded distinct-patch limit") == 0, "failure message first in the status");
    check(model.preview().find(u"THE ROUTING PLAN COULD NOT BE CALCULATED") == 0, "failure message in the preview");
    // Changing a setting starts over (the song still fails).
    model.set_capacity(3);
    check(model.plan_pending(), "a change after a failure re-plans");
}

void test_patch_descriptions()
{
    check(describe_patch(routing::MidiPatchIdentity(0, 0, 0)) == u"Program 1 — Acoustic Grand Piano (General MIDI).", "GM bank description");
    check(describe_patch(routing::MidiPatchIdentity(2, -1, 127)) == u"Program 128 — Gunshot (General MIDI reference only; bank 2/? may use a different sound).", "other bank description");
    check(describe_patch(routing::MidiPatchIdentity(0, 0, -1)) == u"No valid Program Change was found for this source lane.", "no program");
    check(describe_patch(routing::MidiPatchIdentity(0, 0, 128)) == u"No valid Program Change was found for this source lane.", "program out of range");
    check(std::u16string(RoutingDialogTexts::title()) == u"Track routing — MIDIBottleneck Player", "title");
    check(RoutingDialogTexts::destination_item(0) == u"Automatic suggestion" && RoutingDialogTexts::destination_item(16) == u"Output channel 16", "destination items");
}

// ---------------------------------------------------------------- memory bound (DD-027)

// The large-file warning counts at most one generated Bank/Program group per note start or patch-state event and at
// most three generated events per such event (the preflight counts). The compiled route must stay inside both bounds
// for every setting, including the most merging one (capacity 1, aggressive).
void test_generated_bounds(const std::string& fixtureDir)
{
    const char* names[] = { "routing-lanes", "routing-many-lanes", "routing-exact-merge", "models-notes", "index-chase-rich",
        "many-tracks-ties", "f0-all-message-types", "metadata-names", "routing-patches-4096" };
    for (const char* name : names) {
        const std::string path = fixtureDir + "\\" + name + ".mid";
        win32::Win32FileSystem files;
        std::string error;
        std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
        PreflightCounts counts;
        LoadError loadError;
        if (!source || !preflight_scan(*source, counts, loadError)) { check(false, std::string("preflight ") + name); continue; }
        LoadedSong loaded;
        if (!load_fixture(path, loaded)) { check(false, std::string("load ") + name); continue; }
        const int64_t groupBound = counts.note_ons + counts.routing_patch_state_events;
        for (int variant = 0; variant < 4; ++variant) {
            routing::TrackRoutingSettings settings;
            if (variant == 1) settings.maximum_automatic_tier = routing::TrackRoutingTier::GmFamily;
            if (variant >= 2) settings.maximum_automatic_tier = routing::TrackRoutingTier::Aggressive;
            if (variant == 3) settings.target_capacity = 1;
            std::shared_ptr<routing::CompiledTrackRoute> route = routing::compile_route(loaded.routing, settings);
            const std::string what = std::string(name) + " variant " + std::to_string(variant);
            int64_t events = 0;
            int32_t previous = -1;
            bool ordered = true;
            for (const routing::TrackRouteGeneratedPatch& group : route->generated_patches()) {
                events += group.event_count();
                ordered = ordered && group.source_event_index > previous && group.event_count() >= 1;
                previous = group.source_event_index;
            }
            check(static_cast<int64_t>(route->generated_patches().size()) <= groupBound, "generated groups within the preflight bound: " + what);
            check(events <= counts.routing_generated_state_upper_bound, "generated events within the preflight bound: " + what);
            check(ordered, "generated groups ascend by source event: " + what);
            if (route->is_valid()) {
                routing::TrackRoutedEventStore view(loaded.routing, route);
                check(view.count() == route->statistics.routed_output_event_count, "routed view size: " + what);
                bool generatedFound = false;
                for (int32_t i = 0; i < view.count() && !generatedFound; ++i) generatedFound = view.at(i).source_index < 0;
                check(route->generated_patches().empty() || generatedFound,
                    "generated events appear in the routed view: " + what);
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--write-explained" && i + 1 < argc) g_writeExplainedPath = argv[++i];
        else positional.push_back(a);
    }
    std::string fixtures = positional.size() > 0 ? positional[0] : "v2/harness/fixtures";
    std::string tables = positional.size() > 1 ? positional[1] : "v2/harness/expected/routing-ui";
    std::string explainedFile = positional.size() > 2 ? positional[2] : "v2/harness/expected-v2-ui/routing-ui/explained-states.txt";
    if (g_writeExplainedPath.empty()) {
        std::ifstream in(explainedFile, std::ios::binary);
        std::string keyLine, valueLine;
        while (in && std::getline(in, keyLine) && std::getline(in, valueLine)) {
            if (!keyLine.empty() && keyLine.back() == '\r') keyLine.pop_back();
            if (!valueLine.empty() && valueLine.back() == '\r') valueLine.pop_back();
            g_explainedStates[keyLine] = valueLine;
        }
    }
    const char* names[] = { "routing-lanes", "routing-many-lanes", "routing-exact-merge", "models-notes", "index-chase-rich",
        "many-tracks-ties", "f0-all-message-types", "metadata-names", "empty-tracks", "routing-patches-4096", "routing-patches-4097" };
    test_patch_descriptions();
    test_plan_flow(fixtures);
    test_failure_flow(fixtures);
    test_generated_bounds(fixtures);
    for (const char* name : names) run_table(fixtures, tables + "/" + name + ".txt", name);
    if (!g_writeExplainedPath.empty()) {
        std::ofstream out(g_writeExplainedPath, std::ios::binary);
        for (size_t i = 0; i < g_writtenExplained.size(); ++i) out << g_writtenExplained[i] << "\n";
        std::printf("wrote %zu explained states to %s\n", g_writtenExplained.size() / 2, g_writeExplainedPath.c_str());
    }
    else {
        for (std::map<std::string, std::string>::const_iterator it = g_explainedStates.begin(); it != g_explainedStates.end(); ++it)
            if (!g_explainedUsed[it->first]) check(false, "explained state no longer occurs: " + it->first);
    }
    std::printf("%s: %d checks, %d failures (%d sessions, %d dialog states compared with v1, %d of them explained by DD-044)\n",
        g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures, g_sessions, g_states, g_explained);
    return g_failures == 0 ? 0 : 1;
}
