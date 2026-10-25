// Tests of the Analysis window's portable layer (core/app/analysis_*.h). Every table in
// v2/harness/expected/analysis-ui was written by v1's own DiagnosticsForm / WorkloadGraph code
// (v2/harness/analysis-ui-reference); this program recomputes each value with the v2 port and
// compares. The second half tests behaviour that has no v1 table (caches, generations, the
// busy indicator, debounce, song replacement) against a real PlayerController on the None/recording
// outputs: no device is opened.
//
// Usage: midibn-analysis-app-tests [fixture-directory] [table-directory]
// (defaults v2/harness/fixtures and v2/harness/expected/analysis-ui, relative to the current directory).
#include "midibn/app/analysis_cache.h"
#include "midibn/app/analysis_format.h"
#include "midibn/app/analysis_graph.h"
#include "midibn/app/analysis_resolution.h"
#include "midibn/app/analysis_summary.h"
#include "midibn/app/analysis_window_model.h"
#include "midibn/app/player_controller.h"
#include "midibn/format/display_format.h"
#include "midibn/midi_loader.h"
#include "midibn/null_output.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;
using namespace midibn::app;
using analysis::AnalysisConfiguration;
using analysis::ProjectionState;
using analysis::WorkloadAnalysis;
using format::NumberCulture;

namespace {

int g_checks = 0;
int g_failures = 0;
std::map<std::string, int> g_shown;
std::string g_fixtures = "v2/harness/fixtures";
std::string g_tables = "v2/harness/expected/analysis-ui";

void fail(const std::string& section, const std::string& message)
{
    ++g_failures;
    if (++g_shown[section] <= 6) std::printf("FAIL [%s] %s\n", section.c_str(), message.c_str());
    else if (g_shown[section] == 7) std::printf("FAIL [%s] ... further failures in this section not shown\n", section.c_str());
}

std::string show(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F && c != '\\') out += static_cast<char>(c);
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        }
    }
    if (out.size() > 600) out = out.substr(0, 600) + "...";
    return out;
}

void eq(const std::string& section, const std::string& label, const std::string& expected, const std::string& actual)
{
    ++g_checks;
    if (expected == actual) return;
    size_t at = 0;
    while (at < expected.size() && at < actual.size() && expected[at] == actual[at]) ++at;
    size_t from = at > 60 ? at - 60 : 0;
    fail(section, label + ": first difference at byte " + std::to_string(at) + "\n    expected \"..." + show(expected.substr(from, 160)) +
        "\"\n    got      \"..." + show(actual.substr(from, 160)) + "\"");
}

void eq_i(const std::string& section, const std::string& label, int64_t expected, int64_t actual)
{
    ++g_checks;
    if (expected != actual) fail(section, label + ": expected " + std::to_string(expected) + " got " + std::to_string(actual));
}

void check(const std::string& section, bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) fail(section, what);
}

// ---------------------------------------------------------------- tables

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        char c = s[++i];
        if (c == '\\') out += '\\';
        else if (c == 't') out += '\t';
        else if (c == 'r') out += '\r';
        else if (c == 'n') out += '\n';
        else if (c == 'u' && i + 4 < s.size() + 0u + 0u) {
            append_utf8(out, static_cast<uint32_t>(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16)));
            i += 4;
        } else out += c;
    }
    return out;
}

typedef std::vector<std::string> Row;

std::vector<Row> load_table(const std::string& name)
{
    std::vector<Row> rows;
    std::ifstream in(g_tables + "/" + name, std::ios::binary);
    if (!in) {
        fail(name, "cannot read " + g_tables + "/" + name);
        return rows;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        Row row;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            row.push_back(unescape(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        rows.push_back(row);
    }
    return rows;
}

int64_t to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

double from_hex(const std::string& s)
{
    uint64_t bits = std::strtoull(s.c_str(), nullptr, 16);
    double value;
    static_assert(sizeof value == sizeof bits, "double");
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

std::string to_hex(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(bits));
    return buf;
}

NumberCulture culture_of(const std::string& name)
{
    NumberCulture c;
    if (name == "de-DE") {
        c.decimal_separator = ",";
        c.group_separator = ".";
    }
    return c;
}

// ---------------------------------------------------------------- fixtures and configurations

struct LoadedFixture {
    Song song;
    analysis::WorkloadCache cache;
    AnalysisSongInfo info;
    analysis::AnalysisSong input;
};

std::map<std::string, std::unique_ptr<LoadedFixture>> g_loaded;

LoadedFixture* fixture(const std::string& name)
{
    auto it = g_loaded.find(name);
    if (it != g_loaded.end()) return it->second.get();
    win32::Win32FileSystem files;
    std::string error;
    std::string path = g_fixtures + "/" + name + ".mid";
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    std::unique_ptr<LoadedFixture> f(new LoadedFixture());
    LoadError loadError;
    LoadOptions options;
    if (!source || !load_song(*source, options, f->song, loadError)) {
        fail("fixtures", "cannot load " + path + " " + error + loadError.message);
        g_loaded[name] = nullptr;
        return nullptr;
    }
    f->input.events = models::EventSource(&f->song.store);
    f->input.duration_us = f->song.duration_us;
    f->info.file_name = name + ".mid";
    f->info.smf_format = f->song.format;
    f->info.track_count = f->song.track_count;
    f->info.ticks_per_quarter_note = f->song.division;
    f->info.duration_us = f->song.duration_us;
    f->info.note_count = f->song.note_count;
    f->info.file_size_bytes = f->song.file_size;
    LoadedFixture* raw = f.get();
    g_loaded[name] = std::move(f);
    return raw;
}

std::shared_ptr<const overrides::AnalysisOverrideSnapshot> parse_override(const std::string& text)
{
    if (text == "-") return nullptr;
    std::vector<int32_t> values(16 * overrides::kAttributeCount, overrides::kAutoValue);
    if (text != "none") {
        size_t start = 0;
        while (start <= text.size()) {
            size_t semi = text.find(';', start);
            std::string part = text.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            size_t dot = part.find('.'), eq = part.find('=');
            values[static_cast<size_t>(to_i(part.substr(0, dot)) * overrides::kAttributeCount + to_i(part.substr(dot + 1, eq - dot - 1)))] =
                static_cast<int32_t>(to_i(part.substr(eq + 1)));
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }
    return std::make_shared<overrides::AnalysisOverrideSnapshot>(values);
}

std::shared_ptr<AnalysisConfiguration> parse_cfg(const std::string& text)
{
    auto c = std::make_shared<AnalysisConfiguration>();
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t eq = part.find('=');
        std::string key = part.substr(0, eq), value = part.substr(eq + 1);
        size_t colon = value.find(':');
        std::string second = colon == std::string::npos ? std::string() : value.substr(colon + 1);
        std::string first = value.substr(0, colon);
        if (key == "sim") c->simulate_slowdown = value == "1";
        else if (key == "mode") c->service_duration_mode = static_cast<models::ServiceDurationMode>(to_i(value));
        else if (key == "proc") c->processing_us = to_i(value);
        else if (key == "bit") c->midi_bitrate = to_i(value);
        else if (key == "eps") c->events_per_second = to_i(value);
        else if (key == "ql") { c->queue_length_limit_enabled = first == "1"; c->queue_length_limit = static_cast<int32_t>(to_i(second)); }
        else if (key == "qa") { c->queue_age_limit_enabled = first == "1"; c->queue_age_limit_us = to_i(second); }
        else if (key == "pol") c->overflow_policy = static_cast<models::OverflowPolicy>(to_i(value));
        else if (key == "pn") c->per_note_interval_gate_enabled = value == "1";
        else if (key == "dn") c->delayed_note_off_enabled = value == "1";
        else if (key == "scale") c->scale_note_interval_by_traffic = value == "1";
        else if (key == "ip") { c->interval_polyphony_limit_enabled = first == "1"; c->interval_polyphony_limit = static_cast<int32_t>(to_i(second)); }
        else if (key == "na") c->note_attack_limit_enabled = value == "1";
        else if (key == "or") c->output_rate_limit_enabled = value == "1";
        else if (key == "paced") c->output_paced_timeline = value == "1";
        else if (key == "aq") c->apply_queue_limit_without_slowdown = value == "1";
        else if (key == "ovr") c->override_projection = parse_override(value);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return c;
}

AnalysisResultView make_view(LoadedFixture* f, int64_t bucket, const std::string& cfgText, const std::string& variant)
{
    std::shared_ptr<AnalysisConfiguration> cfg = parse_cfg(cfgText);
    analysis::AnalysisControl control;
    if (variant == "complete") {
        WorkloadAnalysis result = analysis::analyze(f->input, bucket, cfg, f->cache, control);
        return AnalysisResultView::of(std::make_shared<WorkloadAnalysis>(std::move(result)));
    }
    WorkloadAnalysis base = analysis::analyze(f->input, bucket, nullptr, f->cache, control);
    AnalysisResultView preview = AnalysisResultView::preview(
        std::make_shared<WorkloadAnalysis>(analysis::copy_workload(base, cfg, control)), cfg);
    if (variant == "pending") return preview;
    if (variant == "cancelled") return preview.with_projection_state(ProjectionState::Cancelled);
    if (variant.compare(0, 7, "failed:") == 0) return preview.with_projection_failure(variant.substr(7));
    fail("variants", "unknown variant " + variant);
    return preview;
}

// Known differences of the "Overflow policy" line of the report from v1's recorded text:
//  - v1 FormatOverflowPolicy has no case for "Drop background notes" and prints "Drop newest" (AD-001);
//  - the single-event choices are named "Drop newest events" and "Drop oldest events" (DD-025; v1: "Drop newest",
//    "Drop oldest pending event").
std::string fix_expected_policy(const std::string& cfg, std::string expected)
{
    const std::string label = "Overflow policy       ";
    const std::string newest = label + "Drop newest";
    const std::string oldest = label + "Drop oldest pending event";
    size_t at = expected.find(newest);
    if (at != std::string::npos)
        expected.replace(at, newest.size(),
            cfg.find(",pol=5,") != std::string::npos ? label + "Drop background notes" : label + "Drop newest events");
    at = expected.find(oldest);
    if (at != std::string::npos) expected.replace(at, oldest.size(), label + "Drop oldest events");
    return expected;
}

// ---------------------------------------------------------------- v1 tables

void test_summary()
{
    std::vector<Row> rows = load_table("summary.txt");
    check("summary", rows.size() > 100, "summary table rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        LoadedFixture* f = fixture(r[0]);
        if (!f) continue;
        AnalysisResultView view = make_view(f, to_i(r[1]), r[3], r[2]);
        std::string label = r[0] + " bucket " + r[1] + " " + r[2] + " " + r[3];
        eq("summary", label, fix_expected_policy(r[3], r[4]), build_analysis_summary(f->info, view));
    }
}

void test_inspection()
{
    std::vector<Row> rows = load_table("inspection.txt");
    check("inspection", rows.size() > 100, "inspection table rows");
    std::string lastKey;
    AnalysisResultView view;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        LoadedFixture* f = fixture(r[0]);
        if (!f) continue;
        std::string key = r[0] + "|" + r[1] + "|" + r[2] + "|" + r[3];
        if (key != lastKey) {
            view = make_view(f, to_i(r[1]), r[3], r[2]);
            lastKey = key;
        }
        eq("inspection", key + " counts " + r[4] + " pinned " + r[5] + " time " + r[6], r[7],
            build_inspection_text(view, r[4] == "1", to_i(r[6]), r[5] == "1"));
    }
}

void test_formats()
{
    std::vector<Row> rows = load_table("formats.txt");
    check("formats", rows.size() > 100, "format table rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const std::string& fn = r[0];
        NumberCulture culture = culture_of(r[1]);
        int64_t arg = to_i(r[2]);
        std::string actual;
        if (fn == "bytes") actual = analysis_format_bytes(arg, culture);
        else if (fn == "resolution" || fn == "graph-resolution") actual = analysis_format_resolution(arg, culture);
        else if (fn == "clock" || fn == "graph-clock-detailed") actual = analysis_format_clock(arg);
        else if (fn == "boundary" || fn == "graph-boundary") actual = analysis_format_interval_boundary(arg);
        else if (fn == "graph-clock") actual = analysis_format_clock_short(arg);
        else if (fn == "overflow") {
            actual = analysis_format_overflow_policy(static_cast<models::OverflowPolicy>(arg));
            if (arg == 5) {
                eq("formats", "overflow 5 (v1 prints Drop newest; AD-001)", "Drop newest", r[3]);
                eq("formats", "overflow 5 in v2", "Drop background notes", actual);
                continue;
            }
            // DD-025: the two single-event choices carry "events" in their names.
            std::string expected = r[3];
            if (expected == "Drop newest") expected = "Drop newest events";
            else if (expected == "Drop oldest pending event") expected = "Drop oldest events";
            eq("formats", "overflow " + r[2] + " (DD-025 names)", expected, actual);
            continue;
        } else { fail("formats", "unknown function " + fn); continue; }
        eq("formats", fn + " " + r[1] + " " + r[2], r[3], actual);
    }
}

void test_ticks()
{
    std::vector<Row> rows = load_table("ticks.txt");
    check("ticks", rows.size() > 500, "tick table rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "interval")
            eq_i("ticks", "interval span " + r[1] + " width " + r[2], to_i(r[3]), choose_timeline_tick_interval(to_i(r[1]), static_cast<int>(to_i(r[2]))));
        else if (r[0] == "axis")
            eq("ticks", "axis time " + r[1] + " interval " + r[2], r[3], analysis_format_axis_clock(to_i(r[1]), to_i(r[2])));
        else if (r[0] == "abbreviate")
            eq("ticks", "abbreviate " + r[1], r[2], abbreviate_playback_counts(r[1]));
    }
}

void test_resolution()
{
    std::vector<Row> rows = load_table("resolution.txt");
    check("resolution", rows.size() > 1000, "resolution table rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "auto") {
            eq_i("resolution", "auto span " + r[1] + " width " + r[2] + " current " + r[3], to_i(r[4]),
                choose_auto_resolution(to_i(r[1]), static_cast<int>(to_i(r[2])), to_i(r[3])));
        } else if (r[0] == "custom") {
            bool hasSong = r[1] != "nosong";
            CustomResolutionResult result = validate_custom_resolution(hasSong, hasSong ? to_i(r[1]) : 0, r[2]);
            std::string label = "custom duration " + r[1] + " ms " + r[2];
            eq_i("resolution", label + " ok", to_i(r[3]), result.ok ? 1 : 0);
            eq_i("resolution", label + " microseconds", to_i(r[4]), result.microseconds);
            eq("resolution", label + " error", r[5], result.error);
        } else if (r[0] == "default") {
            int64_t duration = to_i(r[1]);
            eq_i("resolution", "default resolution for " + r[1], to_i(r[2]), analysis::default_resolution(duration));
            std::string error;
            int64_t count = -1;
            try { count = analysis::calculate_bucket_count(duration, analysis::default_resolution(duration)); }
            catch (const models::ModelException& e) { error = std::string(e.type) + ": " + e.message; }
            eq_i("resolution", "default bucket count for " + r[1], to_i(r[3]), count);
            if (!error.empty()) eq("resolution", "default error for " + r[1], r[4], error);
        }
    }
}

void test_cache_key()
{
    std::vector<Row> rows = load_table("cache-key.txt");
    check("cache-key", rows.size() > 100, "cache key rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        std::shared_ptr<AnalysisConfiguration> cfg = r[0] == "default" ? std::make_shared<AnalysisConfiguration>() : parse_cfg(r[0]);
        eq("cache-key", r[0] + " @" + r[1], r[2], analysis_cache_key(*cfg, to_i(r[1])));
    }
}

void test_failure()
{
    std::vector<Row> rows = load_table("failure.txt");
    check("failure", rows.size() > 50, "failure rows");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "null") continue;   // v1 passes a null exception: C++ has no such value
        eq("failure", r[0] + " \"" + r[1] + "\"", r[2], concise_analysis_failure(r[0], r[1]));
    }
}

// ---------------------------------------------------------------- overlay


void test_overlay()
{
    std::vector<Row> rows = load_table("overlay.txt");
    check("overlay", rows.size() > 500, "overlay rows");
    std::map<std::string, int> widths;   // scale|text -> width
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i][0] == "measure") widths[rows[i][1] + "|" + rows[i][2]] = static_cast<int>(to_i(rows[i][3]));
    WorkloadGraphModel model;
    int scale = 100;
    std::string missing;
    model.set_text_measure([&](const std::string& text, bool padded) {
        GraphSize s;
        auto it = widths.find(std::to_string(scale) + "|" + text);
        if (padded || it == widths.end()) { missing = text; return s; }
        s.width = it->second;
        s.height = 15;
        return s;
    });
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "bounds") {
            scale = static_cast<int>(to_i(r[1]));
            model.set_scale_percent(scale);
            GraphRect area;
            area.x = 44; area.y = 42; area.width = static_cast<int>(to_i(r[2])); area.height = 300;
            GraphRect b = model.playback_statistics_bounds(area);
            eq("overlay", "bounds scale " + r[1] + " width " + r[2],
                r[4] + "," + r[5] + "," + r[6] + "," + r[7],
                std::to_string(b.x) + "," + std::to_string(b.y) + "," + std::to_string(b.width) + "," + std::to_string(b.height));
        } else if (r[0] == "fit") {
            scale = static_cast<int>(to_i(r[1]));
            model.set_scale_percent(scale);
            missing.clear();
            std::string fit = model.fit_playback_line(r[2], r[3], r[4], static_cast<int>(to_i(r[5])));
            check("overlay", missing.empty(), "no width recorded for \"" + missing + "\"");
            eq("overlay", "fit " + r[2] + r[4] + " width " + r[5] + " scale " + r[1], r[6], fit);
        }
    }
}

// ---------------------------------------------------------------- graph script

std::string opt(bool have, int64_t value) { return have ? std::to_string(value) : "-"; }

struct GraphRig {
    WorkloadGraphModel g;
    std::map<std::string, int> widths;   // d|scale|label -> w*65536 + h
    std::map<std::string, AnalysisResultView> views;
    std::string missing;
    int scale = 100;
    int64_t now_us = 0;

    GraphRig()
    {
        g.set_text_measure([this](const std::string& text, bool padded) {
            GraphSize s;
            std::string key = std::string(padded ? "d" : "n") + "|" + std::to_string(scale) + "|" + text;
            auto it = widths.find(key);
            if (it == widths.end()) {
                missing = key;
                return s;
            }
            s.width = it->second >> 16;
            s.height = it->second & 0xFFFF;
            return s;
        });
    }

    AnalysisResultView view_for(const std::string& kind, const std::string& fixtureName, const std::string& bucket, const std::string& cfg)
    {
        std::string key = kind + " " + fixtureName + " " + bucket + " " + cfg;
        auto it = views.find(key);
        if (it != views.end()) return it->second;
        LoadedFixture* f = fixture(fixtureName);
        AnalysisResultView v = f ? make_view(f, to_i(bucket), cfg, kind == "analysis" ? "complete" : "pending") : AnalysisResultView();
        views[key] = v;
        return v;
    }

    std::string exec(const std::string& op)
    {
        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t sp = op.find(' ', start);
            f.push_back(op.substr(start, sp == std::string::npos ? std::string::npos : sp - start));
            if (sp == std::string::npos) break;
            start = sp + 1;
        }
        std::string extra;
        const std::string& c = f[0];
        auto n = [&](size_t i) { return static_cast<int>(to_i(f[i])); };
        if (c == "size") g.set_client_size(n(1), n(2));
        else if (c == "scale") { scale = n(1); g.set_scale_percent(scale); }
        else if (c == "analysis" || c == "preview") g.set_analysis(view_for(c, f[1], f[2], f[3]));
        else if (c == "detach") g.detach("detached");
        else if (c == "reset") g.reset_zoom();
        else if (c == "zoom") g.zoom_at(n(1), n(2));
        else if (c == "pan") g.pan_by_pixels(n(1));
        else if (c == "follow") g.set_follow(static_cast<FollowTarget>(n(1)));
        else if (c == "pos") g.set_playback_positions(to_i(f[1]), to_i(f[2]));
        else if (c == "events") g.set_show_event_counts(f[1] == "1");
        else if (c == "move" || c == "dragmove") g.mouse_move(n(1), n(2));
        else if (c == "tick") { now_us += 100000; g.process_dynamic_updates(now_us); }
        else if (c == "leave") g.mouse_leave();
        else if (c == "down") g.mouse_down(n(1), n(2));
        else if (c == "up") g.mouse_up(n(1), n(2));
        else if (c == "dbl") g.mouse_double_click(n(1), n(2));
        else if (c == "wheel") g.mouse_wheel(n(1), n(2), n(3));
        else if (c == "timeat") extra = " timeat=" + std::to_string(g.time_at_client_x(n(1)));
        else if (c == "bucketat") {
            const analysis::WorkloadBucket* b = g.has_analysis() ? analysis_bucket_at(g.analysis(), to_i(f[1])) : nullptr;
            extra = " bucketat=" + (b == nullptr ? std::string("-") : std::to_string(b->event_count) + "," + std::to_string(b->byte_count) + "," +
                std::to_string(b->largest_cluster) + "," + std::to_string(b->predicted_peak_occupancy) + "," +
                std::to_string(b->predicted_peak_queue_age_us) + "," + std::to_string(b->predicted_dropped_events) + "," +
                std::to_string(b->predicted_buffer_clears));
        } else if (c == "range") {
            int first = 0, last = 0;
            g.bucket_range_at_pixel(n(1), g.graph_area(), first, last);
            extra = " range=" + std::to_string(first) + "," + std::to_string(last);
        } else if (c == "pin") {
            int64_t time;
            bool ok = g.try_get_pin_time(n(1), n(2), time);
            extra = " pintime=" + (ok ? std::to_string(time) : std::string("-"));
        } else if (c != "noop") return "unknown op " + op;
        return state(extra);
    }

    std::string state(const std::string& extra)
    {
        std::string s = "view=" + std::to_string(g.view_start()) + ".." + std::to_string(g.view_end());
        GraphRect a = g.graph_area();
        s += " area=" + std::to_string(a.x) + "," + std::to_string(a.y) + "," + std::to_string(a.width) + "," + std::to_string(a.height);
        s += " hover=" + opt(g.has_hover(), g.hover_time());
        s += " pin=" + opt(g.has_pinned(), g.pinned_time());
        s += " pos=" + opt(g.has_playback_positions(), g.playback_timeline()) + "," + opt(g.has_playback_positions(), g.midi_output_position());
        s += " follow=" + std::to_string(static_cast<int>(g.follow()));
        s += std::string(" bytes=") + (g.uses_byte_rate() ? "1" : "0");
        s += " title=" + (g.uses_byte_rate() ? std::string("MIDI bytes/sec") : g.event_panel_title());
        s += " peak=" + to_hex(g.event_panel_peak());
        std::vector<GraphEvent> events = g.take_events();
        std::string ev;
        for (size_t i = 0; i < events.size(); ++i) {
            if (i) ev += ",";
            if (events[i].kind == GraphEvent::ViewportChanged) ev += "V";
            else if (events[i].kind == GraphEvent::InspectionChanged) ev += "I:" + std::to_string(events[i].time_us) + ":" + (events[i].pinned ? "1" : "0");
            else ev += "S:" + std::to_string(events[i].time_us) + ":1";
        }
        s += " ev=" + ev;
        s += " ticks=";
        std::vector<GraphAxisTick> ticks = g.timeline_ticks();
        for (size_t i = 0; i < ticks.size(); ++i) {
            const GraphAxisTick& t = ticks[i];
            s += std::to_string(t.time_us) + ":" + std::to_string(t.x) + ":" + t.label + ":" + std::to_string(t.label_bounds.x) + ":" +
                std::to_string(t.label_bounds.y) + ":" + std::to_string(t.label_bounds.width) + ":" + std::to_string(t.label_bounds.height) + ";";
        }
        return s + extra;
    }
};

void test_graph_script()
{
    std::vector<Row> rows = load_table("graph-script.txt");
    check("graph", rows.size() > 400, "graph script rows");
    GraphRig rig;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "measure") rig.widths[r[1] + "|" + r[2] + "|" + r[3]] = (static_cast<int>(to_i(r[4])) << 16) | static_cast<int>(to_i(r[5]));
    }
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        if (rows[i][0] != "op") continue;
        const std::string& op = rows[i][1];
        const std::string& expected = rows[i + 1][1];
        rig.missing.clear();
        std::string actual = rig.exec(op);
        check("graph", rig.missing.empty(), "no text width recorded for " + rig.missing + " (op " + op + ")");
        eq("graph", "op " + std::to_string(i / 2) + " \"" + op + "\"", expected, actual);
    }
}

// ---------------------------------------------------------------- behaviour without a v1 table

void test_view_and_cache_logic()
{
    // WithConfiguration returns the same view for an equal configuration; override snapshots compare by signature.
    LoadedFixture* f = fixture("models-notes");
    if (!f) return;
    AnalysisResultView complete = make_view(f, 100000, "sim=1,mode=0,proc=250,bit=31250,eps=10000,ql=1:8,qa=0:1000000,pol=0,pn=0,dn=0,scale=0,ip=1:128,na=0,or=0,paced=0,aq=1,ovr=-", "complete");
    std::shared_ptr<AnalysisConfiguration> same = std::make_shared<AnalysisConfiguration>(*complete.configuration);
    AnalysisResultView again = complete.with_configuration(same);
    check("view", again.configuration == complete.configuration, "an equal configuration keeps the view's configuration");
    std::shared_ptr<AnalysisConfiguration> other = std::make_shared<AnalysisConfiguration>(*complete.configuration);
    other->queue_length_limit = 9;
    check("view", complete.with_configuration(other).configuration == other, "a different configuration replaces it");
    check("view", complete.with_projection_state(ProjectionState::Complete).data == complete.data, "state change shares the data");
    check("view", complete.has_queue_projection() && !complete.has_gate_projection(), "queue projection flags");
    check("view", !complete.with_projection_state(ProjectionState::Cancelled).has_queue_projection(), "cancelled has no projection");

    // LocalAnalysisCache: 12 entries, oldest key first out, a repeated key keeps its place.
    LocalAnalysisCache local;
    auto data = std::make_shared<WorkloadAnalysis>();
    for (int i = 0; i < 12; ++i) local.remember("k" + std::to_string(i), data);
    local.remember("k3", data);
    check("cache", local.size() == 12 && local.order().front() == "k0", "12 entries, order kept for a repeated key");
    local.remember("k12", data);
    check("cache", local.size() == 12 && !local.find("k0") && local.find("k1") && local.find("k12"), "the oldest key is evicted first");
    local.remember("k13", data);
    check("cache", !local.find("k1") && local.find("k2"), "next oldest evicted");

    // SharedAnalysisResults: weakly held.
    SharedAnalysisResults shared;
    {
        auto strong = std::make_shared<WorkloadAnalysis>();
        shared.remember("a", strong);
        check("cache", shared.find("a") == strong, "shared result found while alive");
    }
    check("cache", shared.find("a") == nullptr, "shared result gone once nothing holds it");
    check("cache", shared.size() == 0, "dead entry removed on lookup");
    std::vector<std::shared_ptr<WorkloadAnalysis>> keep;
    for (int i = 0; i < 14; ++i) {
        keep.push_back(std::make_shared<WorkloadAnalysis>());
        shared.remember("s" + std::to_string(i), keep.back());
    }
    check("cache", shared.size() <= 12, "shared cache capped at 12 entries");

    // Per-song registry: same song -> same caches; gone with the song.
    size_t before = song_analysis_cache_count();
    {
        std::shared_ptr<const void> song = std::make_shared<int>(1);
        std::shared_ptr<SongAnalysisCaches> one = song_analysis_caches(song);
        std::shared_ptr<SongAnalysisCaches> two = song_analysis_caches(song);
        check("cache", one == two && song_analysis_cache_count() == before + 1, "one cache set per song");
        std::shared_ptr<const void> anotherSong = std::make_shared<int>(2);
        check("cache", song_analysis_caches(anotherSong) != one, "another song has its own caches");
    }
    check("cache", song_analysis_cache_count() == before, "caches vanish with their song");
}

void test_workload_cache_polling()
{
    // v1: while (!Monitor.TryEnter(cache, 25)) cancellationToken.ThrowIfCancellationRequested();
    analysis::WorkloadCache cache;
    check("polling", cache.try_enter(1), "free cache enters");
    check("polling", !cache.try_enter(30), "held cache times out");
    std::atomic<bool> got(false);
    std::thread waiter([&] {
        if (cache.try_enter(2000)) { got = true; cache.leave(); }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check("polling", !got, "waiter blocks while the cache is held");
    cache.leave();
    waiter.join();
    check("polling", got, "waiter enters once released");

    // analyze() waits for a held cache and stops waiting when cancelled (polls every 25 ms).
    LoadedFixture* f = fixture("models-notes");
    if (!f) return;
    analysis::WorkloadCache held;
    check("polling", held.try_enter(1), "hold the cache");
    std::atomic<bool> cancelled(false);
    std::atomic<int> outcome(0);   // 1 = finished, 2 = cancelled
    int64_t start = win32::performance_counter();
    std::thread worker([&] {
        analysis::AnalysisControl control;
        control.cancellation_requested = [&] { return cancelled.load(); };
        try {
            analysis::analyze(f->input, 100000, nullptr, held, control);
            outcome = 1;
        } catch (const models::ModelException& e) {
            outcome = std::string(e.type) == "OperationCanceledException" ? 2 : 3;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    check("polling", outcome == 0, "analysis waits while another holds the cache");
    cancelled = true;
    worker.join();
    double elapsed = win32::milliseconds_since(start);
    check("polling", outcome == 2, "a cancelled wait ends with OperationCanceledException");
    check("polling", elapsed < 1500, "cancellation is noticed within a few polls");
    held.leave();
}

void test_resolution_selector()
{
    ResolutionSelector s;
    check("selector", s.items().size() == 9 && s.items()[0] == "Auto" && s.items()[8] == "Custom\xE2\x80\xA6", "items: Auto, 7 fixed choices, Custom...");
    check("selector", s.mode() == ResolutionMode::Auto && s.selected_index() == 0, "starts on Auto");
    s.update_auto_label(100000);
    check("selector", s.items()[0] == "Auto (100 ms)" && s.selected_index() == 0 && !s.tooltip().empty(), "auto label shows the accepted resolution");
    eq("selector", "tooltip", "Automatic graph resolution; the displayed graph currently uses 100 ms buckets.", s.tooltip());
    check("selector", s.select(5) == ResolutionSelector::Selection::Changed && s.mode() == ResolutionMode::Fixed &&
        s.selected_microseconds(0, 5000000, 800, 100000) == 250000, "a fixed choice");
    check("selector", s.select(8) == ResolutionSelector::Selection::PromptCustom, "Custom... asks for a value");
    check("selector", s.mode() == ResolutionMode::Fixed && s.selected_microseconds(0, 5000000, 800, 100000) == 250000,
        "while the prompt is open the previous fixed choice still applies (v1 reads 0 here)");
    s.cancel_custom();
    check("selector", s.selected_text() == "250 ms", "cancelling the prompt restores the previous choice");
    s.select(8);
    s.apply_custom(12500);
    check("selector", s.mode() == ResolutionMode::Custom && s.items().size() == 10 && s.items()[8] == "Custom: 12.5 ms" &&
        s.selected_text() == "Custom: 12.5 ms" && s.selected_microseconds(0, 5000000, 800, 100000) == 12500, "custom value becomes an item");
    s.select(8);
    s.apply_custom(2000000);
    check("selector", s.items().size() == 10 && s.items()[8] == "Custom: 2 s", "a second custom value replaces the first");
    s.select(0);
    check("selector", s.mode() == ResolutionMode::Auto, "back to Auto");
    check("selector", s.selected_microseconds(60000000, 60000000, 900, 0) == choose_auto_resolution(60000000, 900, 0), "auto follows the visible span");
    check("selector", s.selected_microseconds(0, 60000000, 20, 0) == choose_auto_resolution(60000000, 100, 0), "plot width has a floor of 100 and the whole song is the span when no view");
}

void test_summary_headings()
{
    std::string text = "MIDI FILE\r\nFilename              WORKLOAD.mid\r\n\r\nWORKLOAD\r\nScope\r\n\r\nQUEUE PROJECTION\r\n";
    std::vector<SummaryHeadingSpan> spans = find_summary_headings(text);
    check("headings", spans.size() == 3, "three headings found");
    if (spans.size() == 3) {
        eq_i("headings", "MIDI FILE start", 0, static_cast<int64_t>(spans[0].start));
        // RichEdit counts each CRLF as one character: line 1 (9) + 1, line 2 (36) + 1, empty (0) + 1 -> 48.
        eq_i("headings", "WORKLOAD start (not the one in the file name)", 9 + 1 + 34 + 1 + 0 + 1, static_cast<int64_t>(spans[1].start));
        eq_i("headings", "WORKLOAD length", 8, static_cast<int64_t>(spans[1].length));
    }
}

// ---------------------------------------------------------------- controller-backed model tests

struct Rig {
    win32::Win32FileSystem files;
    std::shared_ptr<platform::RecordingMidiOutput> winmm = std::make_shared<platform::RecordingMidiOutput>();
    std::shared_ptr<platform::RecordingMidiOutput> kdmapi = std::make_shared<platform::RecordingMidiOutput>();
    std::unique_ptr<PlayerController> controller;
    std::atomic<int> wakes{ 0 };
    std::atomic<int> model_wakes{ 0 };
    std::vector<int64_t> seeks;
    std::atomic<int64_t> clock_us{ 1000000 };
    std::unique_ptr<AnalysisWindowModel> model;

    Rig()
    {
        PlayerEnvironment env;
        env.services = std::make_shared<win32::Win32EngineServices>();
        env.files = &files;
        env.list_devices = [] { return std::vector<platform::OutputDeviceInfo>(); };
        env.create_winmm = [this] { return std::shared_ptr<platform::MidiOutput>(winmm); };
        env.create_kdmapi = [this] { return std::shared_ptr<platform::MidiOutput>(kdmapi); };
        env.pointer_size = static_cast<int>(sizeof(void*));
        env.wake_ui = [this] { ++wakes; };
        controller.reset(new PlayerController(env));
        controller->select_output(0);   // None
    }
    ~Rig() { model.reset(); controller->shutdown(); }

    bool load(const std::string& name)
    {
        controller->open_file(g_fixtures + "/" + name + ".mid");
        for (int i = 0; i < 2000; ++i) {
            controller->pump();
            if (controller->has_song() || !controller->loading()) return controller->has_song();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    void make_model(bool fakeClock = true)
    {
        AnalysisWindowHost host;
        host.seek = [this](int64_t t) { seeks.push_back(t); };
        host.wake = [this] { ++model_wakes; };
        if (fakeClock) host.now_us = [this] { return clock_us.load(); };
        model.reset(new AnalysisWindowModel(*controller, host));
    }

    // Polls until the shown analysis is a completed one (up to ~5 s of real time).
    bool wait_completed()
    {
        for (int i = 0; i < 2500; ++i) {
            model->poll();
            if (model->has_completed_analysis() && !model->calculation_pending()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
};

void test_model_basic()
{
    Rig r;
    if (!r.load("models-notes")) { fail("model", "fixture did not load"); return; }
    r.make_model();
    check("model", !r.model->attached() && r.model->summary_text() == kAnalysisPendingSummary, "before start nothing is attached");
    r.model->start();
    check("model", r.model->attached(), "start attaches the loaded song");
    check("model", r.model->title() == std::string("MIDIBottleneck Player \xE2\x80\x94 Analysis \xE2\x80\x94 models-notes.mid"), "title names the file");
    check("model", r.model->calculation_pending() || r.model->has_completed_analysis(), "a calculation started");
    check("model", r.wait_completed(), "the analysis finishes");
    check("model", !r.model->analysis_busy(), "busy ended with the result");
    // The report is v1's text for the controller's default configuration.
    LoadedFixture* f = fixture("models-notes");
    AnalysisResultView expected = make_view(f, r.model->analysis().bucket_us(),
        "sim=0,mode=0,proc=100,bit=31250,eps=10000,ql=0:2000,qa=0:1000000,pol=0,pn=0,dn=0,scale=0,ip=1:128,na=0,or=0,paced=0,aq=1,ovr=-", "complete");
    std::string summary = r.model->summary_text();
    check("model", summary.find("QUEUE PROJECTION") != std::string::npos && summary.find("models-notes.mid") != std::string::npos, "summary text shown");
    eq("model", "summary equals the direct computation", build_analysis_summary(f->info, expected), summary);
    check("model", r.model->graph().has_analysis() && r.model->graph().view_end() == r.model->analysis().duration_us(), "graph shows the whole file");
    check("model", r.model->resolution().items()[0].compare(0, 6, "Auto (") == 0, "Auto label shows the accepted resolution");
    check("model", r.model->preview_publication_count() <= 1, "at most one preview was published");
    int jobs = r.model->started_job_count();
    // Same settings again: the local cache answers, no worker.
    r.model->select_resolution(0);
    r.model->poll();
    check("model", r.model->started_job_count() == jobs, "no new job for an unchanged request");
    // seek to pin
    check("model", !r.model->seek_to_pin_enabled(), "no pin yet");
    r.model->graph_resized(900, 520);
    GraphRect area = r.model->graph().graph_area();
    r.model->graph_mouse_down(area.x + 100, area.y + 50);
    r.model->graph_mouse_up(area.x + 100, area.y + 50);
    check("model", r.model->seek_to_pin_enabled() && r.model->graph().has_pinned(), "a click pins");
    check("model", r.model->inspection_text().compare(0, 7, "Pinned:") == 0, "inspection shows the pin");
    r.model->seek_to_pin();
    check("model", r.seeks.size() == 1 && r.seeks[0] == r.model->graph().pinned_time(), "Seek to pin seeks to the pinned time");
    r.model->graph_double_click(area.x + 200, area.y + 50);
    check("model", r.seeks.size() == 2, "double-click seeks");
    // event counts
    uint64_t rev = r.model->inspection_revision();
    r.model->set_show_event_counts(true);
    check("model", r.model->inspection_revision() != rev && r.model->inspection_text().find("Interval: ") != std::string::npos, "event-count view changes the inspection text");
    r.model->close();
    check("model", true, "closes cleanly");
}

void test_model_stale_and_cancel()
{
    Rig r;
    if (!r.load("models-notes")) { fail("model2", "fixture did not load"); return; }
    r.make_model();
    // Hold the song's base-workload cache so the worker cannot finish: the busy indicator and Cancel can be tested.
    r.model->start();
    check("model2", r.wait_completed(), "initial analysis finishes");
    std::shared_ptr<SongAnalysisCaches> caches = song_analysis_caches(r.model->song());
    r.controller->select_rate_choice(RateModelChoice::ProcessingTime);
    r.controller->set_processing_value(777);
    r.controller->set_queue_limit_checked(true);
    r.model->on_refresh();
    int jobs = r.model->started_job_count();
    check("model2", r.model->next_wake_milliseconds() >= 0, "a settings change schedules a refresh");
    r.clock_us += 100000;
    r.model->poll();
    check("model2", r.model->started_job_count() == jobs, "nothing yet before 200 ms");
    r.clock_us += 150000;
    // The projection needs the workload of the same resolution, which is cached; to hold the worker, use a new resolution.
    r.model->select_resolution(1);   // 10 ms: needs a new base workload
    check("model2", caches->workloads.try_enter(1), "hold the base-workload cache");
    r.model->poll();                  // the settings refresh is due: request at 10 ms starts (blocked) job
    check("model2", r.model->started_job_count() == jobs + 1 || r.model->started_job_count() == jobs + 0, "request started");
    r.clock_us += 200000;
    r.model->poll();
    check("model2", r.model->calculation_pending(), "the held cache keeps the job pending");
    check("model2", r.model->analysis_busy() && !r.model->busy_view().status_visible, "busy, indicator not yet shown (under 1 s)");
    r.clock_us += 900000;
    r.model->poll();
    check("model2", r.model->busy_view().status_visible && r.model->busy_view().progress_visible && r.model->busy_view().cancel_visible,
        "indicator appears after one second");
    eq("model2", "busy text", std::string("Recalculating analysis") + format::kEllipsis, r.model->busy_view().status_text);
    // Newer request while the old one waits: only the newest result may be shown.
    r.controller->set_queue_value(5);
    r.model->on_refresh();
    r.clock_us += 250000;
    r.model->poll();
    int running = r.model->started_job_count();
    r.model->cancel_calculation();
    check("model2", !r.model->calculation_pending() && !r.model->analysis_busy() && !r.model->busy_view().status_visible, "Cancel ends the calculation and hides the indicator");
    caches->workloads.leave();
    r.clock_us += 500000;
    for (int i = 0; i < 50; ++i) { r.model->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(4)); }
    check("model2", r.model->started_job_count() == running, "cancel leaves no pending request");
    check("model2", r.model->has_completed_analysis(), "the earlier completed analysis stays shown");
    r.model->close();
}

void test_model_preview_and_failure()
{
    Rig r;
    if (!r.load("models-notes")) { fail("model3", "fixture did not load"); return; }
    r.controller->select_rate_choice(RateModelChoice::ProcessingTime);
    r.controller->set_queue_limit_checked(true);
    r.make_model();
    r.model->start();
    check("model3", r.wait_completed(), "analysis with a queue limit finishes");
    check("model3", r.model->summary_text().find("Overflow policy") != std::string::npos, "queue projection text");
    // Song replacement: loading shows the message, the new song attaches.
    r.controller->open_file(g_fixtures + "/tempo-changes.mid");
    r.model->on_refresh();
    check("model3", !r.model->attached() && r.model->summary_text() == kAnalysisLoadingMessage, "loading detaches with the Loading message");
    check("model3", r.model->title() == std::string("MIDIBottleneck Player \xE2\x80\x94 Analysis \xE2\x80\x94 no file"), "title says no file");
    for (int i = 0; i < 1000 && r.controller->loading(); ++i) { r.controller->pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    r.model->on_refresh();
    check("model3", r.model->attached() && r.model->title().find("tempo-changes.mid") != std::string::npos, "the new song attaches");
    check("model3", r.wait_completed(), "analysis of the new song finishes");
    // Project forced overrides toggles a new request with the snapshot.
    int jobs = r.model->started_job_count();
    r.model->set_project_forced_overrides(true);
    check("model3", r.model->started_job_count() == jobs + 1 || r.model->has_completed_analysis(), "projecting overrides requests a new analysis");
    check("model3", r.wait_completed(), "override projection finishes");
    check("model3", r.model->summary_text().find("FORCED OVERRIDE PROJECTION") != std::string::npos, "report has the override section");
    // A viewport change keeps the override projection (v1 dropped it, AD-005).
    r.model->select_resolution(1);   // Fixed 10 ms
    r.clock_us += 400000;
    r.model->poll();
    check("model3", r.wait_completed(), "analysis at 10 ms finishes");
    check("model3", r.model->summary_text().find("FORCED OVERRIDE PROJECTION") != std::string::npos, "override projection survives a resolution change");
    check("model3", r.model->analysis().bucket_us() == 10000, "fixed resolution used");
    r.model->close();
}

void test_custom_resolution_model()
{
    Rig r;
    if (!r.load("models-notes")) { fail("model4", "fixture did not load"); return; }
    r.make_model();
    r.model->start();
    check("model4", r.wait_completed(), "initial analysis");
    r.model->select_resolution(static_cast<int>(r.model->resolution().items().size()) - 1);
    check("model4", r.model->custom_prompt_pending(), "Custom... opens the prompt");
    std::string error;
    check("model4", !r.model->apply_custom_resolution("0", error) && error == "Enter a positive aggregation interval in milliseconds.", "zero is refused with v1's message");
    check("model4", !r.model->apply_custom_resolution("0.0001", error) && error == "The aggregation interval is outside the supported range.", "too small");
    check("model4", r.model->custom_prompt_pending(), "prompt stays open after an invalid value");
    check("model4", r.model->apply_custom_resolution("20", error), "20 ms accepted");
    check("model4", !r.model->custom_prompt_pending() && r.model->resolution().mode() == ResolutionMode::Custom, "custom mode");
    r.clock_us += 400000;
    r.model->poll();
    check("model4", r.wait_completed() && r.model->analysis().bucket_us() == 20000, "analysis at the custom resolution");
    r.model->close();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1) g_fixtures = argv[1];
    if (argc > 2) g_tables = argv[2];
    test_summary();
    test_inspection();
    test_formats();
    test_ticks();
    test_resolution();
    test_cache_key();
    test_failure();
    test_overlay();
    test_graph_script();
    test_view_and_cache_logic();
    test_workload_cache_polling();
    test_resolution_selector();
    test_summary_headings();
    test_model_basic();
    test_model_stale_and_cancel();
    test_model_preview_and_failure();
    test_custom_resolution_model();
    std::printf("midibn-analysis-app-tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
