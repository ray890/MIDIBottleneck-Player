// Tests for the portable UI layer (core/format and core/app). Every table in
// v2/harness/expected/ui was written by v1's own code through
// v2/harness/ui-reference; this program recomputes each value with the v2 port
// and compares. Usage: midibn-ui-tests [table-directory]
// (default v2/harness/expected/ui, relative to the current directory).
// Exit code 0 when every check passes.
#include "midibn/app/display_tier.h"
#include "midibn/app/effective_speed.h"
#include "midibn/app/help_text.h"
#include "midibn/app/statistics.h"
#include "midibn/app/ui_mappings.h"
#include "midibn/format/display_format.h"
#include "midibn/format/net_format.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace midibn;
using format::NumberCulture;
using models::ServiceDurationMode;

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

void eq_i(const std::string& section, const std::string& label, int64_t expected, int64_t actual)
{
    ++g_checks;
    if (expected != actual)
        fail(section, label + ": expected " + std::to_string(expected) + " got " + std::to_string(actual));
}

void check(const std::string& section, bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) fail(section, what);
}

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

bool read_lines(const std::string& name, std::vector<std::string>& lines)
{
    std::ifstream in((g_dir + "/" + name).c_str(), std::ios::binary);
    if (!in) {
        fail("tables", "cannot open " + g_dir + "/" + name);
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '#') continue;
        lines.push_back(line);
    }
    return true;
}

// Tab-separated rows with escaped fields.
bool read_rows(const std::string& name, std::vector<Row>& rows)
{
    std::vector<std::string> lines;
    if (!read_lines(name, lines)) return false;
    for (size_t i = 0; i < lines.size(); ++i) {
        Row row;
        size_t start = 0;
        while (true) {
            size_t tab = lines[i].find('\t', start);
            if (tab == std::string::npos) {
                row.push_back(unescape(lines[i].substr(start)));
                break;
            }
            row.push_back(unescape(lines[i].substr(start, tab - start)));
            start = tab + 1;
        }
        rows.push_back(row);
    }
    return true;
}

// One decimal integer per line (the grid tables).
bool read_values(const std::string& name, std::vector<int64_t>& values)
{
    std::vector<std::string> lines;
    if (!read_lines(name, lines)) return false;
    for (size_t i = 0; i < lines.size(); ++i) values.push_back(std::strtoll(lines[i].c_str(), nullptr, 10));
    return true;
}

int64_t to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }
bool to_b(const std::string& s) { return s == "1"; }

double bits_to_double(const std::string& hex)
{
    uint64_t bits = std::strtoull(hex.c_str(), nullptr, 16);
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

std::optional<double> opt_bits(const std::string& s)
{
    if (s == "null") return std::nullopt;
    return bits_to_double(s);
}

float bits_to_float(const std::string& hex)
{
    uint32_t bits = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

uint32_t float_to_bits(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

std::string hex32(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

NumberCulture culture_from_row(const Row& row)
{
    NumberCulture c;
    c.decimal_separator = row[2];
    c.group_separator = row[3];
    return c;
}

// ---- number formatting ----------------------------------------------------------------

void test_format_n()
{
    const char* cultures[] = { "en-US", "de-DE" };
    for (size_t k = 0; k < 2; ++k) {
        std::string section = std::string("format-n ") + cultures[k];
        std::vector<Row> rows;
        if (!read_rows(std::string("format-n-") + cultures[k] + ".txt", rows)) return;
        NumberCulture c = culture_from_row(rows[0]);
        const Row& columns = rows[1];
        int perColumn = 0;
        for (size_t i = 2; i < rows.size(); ++i) {
            double v = bits_to_double(rows[i][0]);
            ++perColumn;
            for (size_t col = 1; col < columns.size(); ++col) {
                int places = std::atoi(columns[col].c_str() + 1);
                std::string actual = columns[col][0] == 'N' ? format::format_n(v, places, c) : format::format_trimmed(v, places, c);
                eq(section, rows[i][0] + " " + columns[col], rows[i][col], actual);
            }
        }
        std::printf("  %s: %d inputs x %d formats\n", section.c_str(), perColumn, static_cast<int>(columns.size()) - 1);
    }
    std::vector<Row> rows;
    if (!read_rows("format-n0.txt", rows)) return;
    NumberCulture en;
    NumberCulture de;
    de.decimal_separator = ",";
    de.group_separator = ".";
    for (size_t i = 0; i < rows.size(); ++i) {
        int64_t v = to_i(rows[i][0]);
        eq("format-n0", rows[i][0] + " en-US", rows[i][1], format::format_n0(v, en));
        eq("format-n0", rows[i][0] + " de-DE", rows[i][2], format::format_n0(v, de));
    }
    // A few facts that do not depend on the tables.
    eq("format-n", "2.675 N2", "2.68", format::format_n(2.675, 2));
    eq("format-n", "1.005 N2", "1.01", format::format_n(1.005, 2));
    eq("format-n", "-0.0001 N2", "0.00", format::format_n(-0.0001, 2));
    eq("format-n", "0.5 N0", "1", format::format_n(0.5, 0));
    eq("format-n", "1e15 N1", "1,000,000,000,000,000.0", format::format_n(1e15, 1));
    eq("format-n", "trimmed 0.1+0.2", "0.3", format::format_trimmed(0.1 + 0.2, 3));
    eq("format-n", "integer", "-42", format::format_integer(-42));
    eq("format-n", "int64 min", "-9,223,372,036,854,775,808", format::format_n0(INT64_MIN));
    eq_i("round", "half even 0.5", 0, format::round_half_even(0.5));
    eq_i("round", "half even 1.5", 2, format::round_half_even(1.5));
    eq_i("round", "half even 2.5", 2, format::round_half_even(2.5));
    eq_i("round", "half even -0.5", 0, format::round_half_even(-0.5));
    eq_i("round", "half even -1.5", -2, format::round_half_even(-1.5));
    eq_i("round", "away 0.5", 1, format::round_away_from_zero(0.5));
    eq_i("round", "away 2.5", 3, format::round_away_from_zero(2.5));
    eq_i("round", "away -2.5", -3, format::round_away_from_zero(-2.5));
    eq_i("round", "away 0.49999999999999994", 0, format::round_away_from_zero(0.49999999999999994));
    eq_i("round", "nan", INT64_MIN, format::round_half_even(std::nan("")));
    eq_i("round", "big", INT64_MIN, format::round_half_even(1e19));
}

// ---- display strings ----------------------------------------------------------------------

engine::PlaybackSnapshot snapshot_from_csv(const std::string& csv)
{
    std::vector<int64_t> v;
    size_t start = 0;
    while (true) {
        size_t comma = csv.find(',', start);
        v.push_back(std::strtoll(csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start).c_str(), nullptr, 10));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    engine::PlaybackSnapshot s;
    size_t i = 0;
    s.state = static_cast<engine::PlaybackState>(v[i++]);
    s.processed_events = v[i++];
    s.dropped_events = v[i++];
    s.gate_filtered_events = v[i++];
    s.musical_limit_rejected_attacks = v[i++];
    s.musical_limit_suppressed_releases = v[i++];
    s.output_rate_rejected_attacks = v[i++];
    s.output_rate_suppressed_releases = v[i++];
    s.output_rate_rejected_other_messages = v[i++];
    s.output_rate_safety_soft_excess = v[i++];
    s.virtual_queue_active = v[i++] != 0;
    s.simulate_slowdown = v[i++] != 0;
    s.service_duration_mode = static_cast<ServiceDurationMode>(v[i++]);
    s.processing_microseconds = v[i++];
    s.midi_bitrate = v[i++];
    s.events_per_second = v[i++];
    s.processing_mode = static_cast<engine::ProcessingMode>(v[i++]);
    s.outstanding_events = v[i++];
    s.maximum_queue_length = v[i++];
    s.intended_timeline_microseconds = v[i++];
    s.last_dispatched_timeline_microseconds = v[i++];
    s.current_lag_microseconds = v[i++];
    s.maximum_lag_microseconds = v[i++];
    return s;
}

std::string rgb_text(format::Rgb c)
{
    return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
}

void test_display_file(const std::string& name)
{
    std::string section = "display " + name;
    std::vector<Row> rows;
    if (!read_rows(name, rows)) return;
    NumberCulture c = culture_from_row(rows[0]);
    std::map<std::string, engine::PlaybackSnapshot> snapshots;
    std::map<std::string, int> counts;
    for (size_t i = 1; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const std::string& fn = r[0];
        ++counts[fn];
        std::string label = fn + " #" + std::to_string(i);
        if (fn == "snap") {
            snapshots[r[1]] = snapshot_from_csv(r[2]);
        } else if (fn == "time") {
            eq(section, label + " " + r[1], r[2], format::format_time(to_i(r[1])));
        } else if (fn == "duration") {
            eq(section, label + " " + r[1], r[2], format::format_duration(to_i(r[1]), c));
        } else if (fn == "lag") {
            eq(section, label + " " + r[1], r[2], format::format_lag_milliseconds(to_i(r[1]), c));
        } else if (fn == "delayednoteoff") {
            eq(section, label + " " + r[1], r[3], format::format_delayed_note_off_duration(to_i(r[1]), to_b(r[2]), c));
        } else if (fn == "pernote") {
            eq(section, label + " " + r[1], r[3], format::format_per_note_frame_rate(to_i(r[1]), to_b(r[2]), c));
        } else if (fn == "window") {
            eq(section, label + " " + r[1], r[2], format::describe_speed_window(to_i(r[1]), c));
        } else if (fn == "bytes") {
            eq(section, label + " " + r[1], r[2], format::format_bytes(to_i(r[1]), c));
        } else if (fn == "pressurenumber") {
            eq(section, label + " " + r[1], r[2], format::format_compact_pressure_number(to_i(r[1]), c));
        } else if (fn == "telemetry") {
            eq(section, label + " " + r[1] + " " + r[2], r[4], format::format_loading_telemetry(to_i(r[1]), to_i(r[2]), to_b(r[3]), c));
        } else if (fn == "loadingelapsed") {
            eq_i(section, label, to_i(r[6]), format::compute_loading_elapsed_seconds(to_i(r[1]), to_i(r[2]), to_i(r[3]), to_i(r[4]), to_i(r[5])));
        } else if (fn == "loadstatus") {
            eq(section, label, r[4], format::format_load_status(r[1], static_cast<int>(to_i(r[2])), static_cast<int>(to_i(r[3])), c));
        } else if (fn == "fileinfo") {
            format::FileSummary summary;
            summary.event_count = to_i(r[1]);
            summary.track_count = to_i(r[2]);
            summary.note_count = to_i(r[3]);
            summary.duration_us = to_i(r[4]);
            summary.running_status_repairs = to_i(r[5]);
            eq(section, label, r[7], format::format_file_information(summary, to_b(r[6]), c));
        } else if (fn == "repairmessage") {
            eq(section, label, r[2], format::format_running_status_repair_message(to_i(r[1]), c));
        } else if (fn == "largemessage") {
            MemoryProjection p;
            p.retained = to_i(r[3]);
            p.peak_low = to_i(r[4]);
            p.peak_high = to_i(r[5]);
            p.retained_with_routing = to_i(r[6]);
            p.routing_peak_total = to_i(r[7]);
            p.architecture_risk = to_b(r[8]);
            eq(section, label, r[9], format::create_large_midi_message(r[1], to_i(r[2]), p, c));
        } else if (fn == "observed") {
            eq(section, label, r[3], format::format_observed_maximum_rate(opt_bits(r[1]), to_b(r[2]), c));
        } else if (fn == "speed") {
            eq(section, label, r[2], format::format_effective_speed(opt_bits(r[1]), c));
        } else if (fn == "outputrate") {
            eq(section, label, r[2], format::format_output_rate(opt_bits(r[1]), c));
        } else if (fn == "dropped") {
            eq_i(section, label, to_i(r[2]), format::total_dropped_events(snapshots[r[1]]));
        } else if (fn == "counts") {
            eq(section, label, r[3], format::format_event_counts(snapshots[r[1]], to_b(r[2]), c));
        } else if (fn == "maxrate") {
            eq(section, label, r[4], format::format_maximum_rate(snapshots[r[1]], opt_bits(r[2]), to_b(r[3]), c));
        } else if (fn == "cells") {
            const engine::PlaybackSnapshot& s = snapshots[r[1]];
            format::ConfiguredRateInputs in;
            in.per_note_interval_gate_enabled = r[2][0] == '1';
            in.delayed_note_off_enabled = r[2][1] == '1';
            in.note_attack_limit_enabled = r[2][2] == '1';
            in.output_rate_limit_enabled = r[2][3] == '1';
            in.engine_processing_microseconds = to_i(r[3]);
            in.engine_events_per_second = to_i(r[4]);
            format::StatisticsText t = format::format_statistics(s, in, opt_bits(r[7]), opt_bits(r[6]), opt_bits(r[5]), to_b(r[8]), c);
            eq(section, label + " timeline", r[9], t.timeline_output);
            eq(section, label + " queue", r[10], t.queue);
            eq(section, label + " maximum rate", r[11], t.maximum_rate);
            eq(section, label + " output rate", r[12], t.output_rate);
            eq(section, label + " events", r[13], t.events);
            eq(section, label + " speed", r[14], t.effective_speed);
            eq(section, label + " max lag", r[15], t.maximum_lag);
            eq(section, label + " lag", r[16], t.current_lag);
        } else if (fn == "analysis") {
            const engine::PlaybackSnapshot& s = snapshots[r[1]];
            eq(section, label + " queue", r[2], format::format_analysis_queue(s, c));
            eq(section, label + " lag", r[3], format::format_analysis_lag(s, c));
        } else if (fn == "pressure") {
            bool virtualQueue = to_b(r[1]);
            bool age = to_b(r[2]);
            eq(section, label, r[5], format::format_queue_pressure_text(virtualQueue, age, to_i(r[3]), to_i(r[4]), false, c));
            eq(section, label + " compact", r[6], format::format_queue_pressure_text(virtualQueue, age, to_i(r[3]), to_i(r[4]), true, c));
        } else if (fn == "color") {
            format::PressureColor color = format::choose_pressure_color(bits_to_double(r[1]), to_i(r[2]) == 1);
            const char* names[] = { "Low", "Medium", "High", "OverLimit" };
            eq(section, label, r[3], names[static_cast<int>(color)]);
            eq(section, label + " rgb", r[4], rgb_text(format::pressure_color_rgb(color)));
        } else if (fn == "palette") {
            eq(section, "palette background", r[1], rgb_text(format::kPressureBackgroundRgb));
            eq(section, "palette outline", r[2], rgb_text(format::kPressureOutlineRgb));
        } else if (fn == "stopwatchfrequency") {
            counts[fn] = 1;
        } else if (fn == "stopwatch") {
            // The reference ran with the machine's Stopwatch.Frequency (stored in the 'stopwatchfrequency' row).
            int64_t frequency = 0;
            for (size_t k = 1; k < rows.size(); ++k)
                if (rows[k][0] == "stopwatchfrequency") frequency = to_i(rows[k][1]);
            eq_i(section, label + " " + r[1], to_i(r[2]), format::stopwatch_ticks_to_microseconds(to_i(r[1]), frequency));
        } else {
            fail(section, "unknown row kind " + fn);
        }
    }
    int total = 0;
    for (std::map<std::string, int>::const_iterator it = counts.begin(); it != counts.end(); ++it) total += it->second;
    std::printf("  %s: %d rows, %d function kinds\n", section.c_str(), total, static_cast<int>(counts.size()));
}

// ---- slider, scrub and timeline mappings ----------------------------------------------------------

struct Compare {
    std::string section;
    std::vector<int64_t> expected;
    size_t index = 0;
    bool ok = true;

    explicit Compare(const std::string& name) : section(name)
    {
        ok = read_values(name + ".txt", expected);
    }
    void next(int64_t actual, const std::string& input)
    {
        if (!ok) return;
        if (index >= expected.size()) {
            fail(section, "table is shorter than the grid (input " + input + ")");
            ++index;
            return;
        }
        eq_i(section, "line " + std::to_string(index + 1) + " input " + input, expected[index], actual);
        ++index;
    }
    void finish()
    {
        if (ok && index != expected.size())
            fail(section, "grid has " + std::to_string(index) + " inputs, table has " + std::to_string(expected.size()));
        std::printf("  %s: %d values\n", section.c_str(), static_cast<int>(index));
    }
};

void test_sliders()
{
    {
        Compare t("slider-microseconds");
        for (int s = 0; s <= 10000; ++s) t.next(app::slider_to_microseconds(s), std::to_string(s));
        t.finish();
    }
    {
        Compare t("microseconds-slider");
        for (int64_t v = -3; v <= 6000; ++v) t.next(app::microseconds_to_slider(v), std::to_string(v));
        for (int s = 0; s <= 10000; ++s) {
            int64_t m = app::slider_to_microseconds(s);
            t.next(app::microseconds_to_slider(m), std::to_string(m));
            if (s > 6000 && s % 3 == 0) {
                t.next(app::microseconds_to_slider(m - 1), std::to_string(m - 1));
                t.next(app::microseconds_to_slider(m + 1), std::to_string(m + 1));
            }
        }
        for (int64_t i = 0;; ++i) {
            int64_t v = 5000 + i * i;
            if (v > 1100000) break;
            t.next(app::microseconds_to_slider(v), std::to_string(v));
        }
        t.finish();
    }
    {
        Compare t("slider-bitrate");
        for (int s = 0; s <= 10000; ++s) t.next(app::slider_to_bitrate(s), std::to_string(s));
        t.finish();
    }
    {
        Compare t("bitrate-slider");
        for (int64_t v = -5; v <= 300; ++v) t.next(app::bitrate_to_slider(v), std::to_string(v));
        for (int s = 0; s <= 10000; ++s) {
            int64_t b = app::slider_to_bitrate(s);
            t.next(app::bitrate_to_slider(b), std::to_string(b));
            if (s % 10 == 0) {
                t.next(app::bitrate_to_slider(b - 1), std::to_string(b - 1));
                t.next(app::bitrate_to_slider(b + 1), std::to_string(b + 1));
            }
        }
        for (int64_t v = 100; v <= 200000000; v += std::max<int64_t>(1, v / 200)) t.next(app::bitrate_to_slider(v), std::to_string(v));
        t.finish();
    }
    {
        Compare t("slider-eventrate");
        for (int s = 0; s <= 10000; ++s) t.next(app::slider_to_event_rate(s), std::to_string(s));
        t.finish();
    }
    {
        Compare t("eventrate-slider");
        for (int64_t v = -3; v <= 3000; ++v) t.next(app::event_rate_to_slider(v), std::to_string(v));
        for (int s = 0; s <= 10000; ++s) {
            int64_t b = app::slider_to_event_rate(s);
            t.next(app::event_rate_to_slider(b), std::to_string(b));
            if (s % 10 == 0) {
                t.next(app::event_rate_to_slider(b - 1), std::to_string(b - 1));
                t.next(app::event_rate_to_slider(b + 1), std::to_string(b + 1));
            }
        }
        for (int64_t v = 100; v <= 20000000; v += std::max<int64_t>(1, v / 200)) t.next(app::event_rate_to_slider(v), std::to_string(v));
        t.finish();
    }
    {
        Compare a("processing-eventrate");
        Compare b("eventrate-processing");
        for (int64_t v = -3; v <= 2100; ++v) {
            a.next(app::processing_to_event_rate(v), std::to_string(v));
            b.next(app::event_rate_to_processing(v), std::to_string(v));
        }
        for (int64_t v = 2100; v <= 2000000; v += std::max<int64_t>(1, v / 50)) {
            a.next(app::processing_to_event_rate(v), std::to_string(v));
            b.next(app::event_rate_to_processing(v), std::to_string(v));
        }
        a.finish();
        b.finish();
    }
    {
        Compare t("track-point");
        const int ranges[][2] = { { 0, 10000 }, { 0, 100 }, { 5, 9 }, { 0, 1 }, { 0, 0 }, { 3, 2 }, { -5, 5 } };
        const int widths[] = { -1, 0, 1, 2, 3, 10, 17, 18, 19, 40, 100, 299, 300, 801 };
        for (size_t ri = 0; ri < 7; ++ri)
            for (size_t wi = 0; wi < 14; ++wi) {
                int step = widths[wi] <= 40 ? 1 : 3;
                for (int x = -10; x <= widths[wi] + 10; x += step)
                    t.next(app::value_from_track_point(x, widths[wi], ranges[ri][0], ranges[ri][1]),
                        std::to_string(x) + "," + std::to_string(widths[wi]));
            }
        t.finish();
    }
}

std::vector<int64_t> scrub_pixels()
{
    std::vector<int64_t> list = { -2000001, -2000000, -1000001, -1000000, -999999, -100000, -10000, -5001, -5000, -1000, -501, -500, -100, -50 };
    for (int64_t p = -40; p <= 40; ++p) list.push_back(p);
    const int64_t more[] = { 50, 100, 499, 500, 501, 1000, 5000, 5001, 10000, 100000, 999999, 1000000, 1000001, 2000000, 2000001 };
    for (size_t i = 0; i < sizeof more / sizeof more[0]; ++i) list.push_back(more[i]);
    return list;
}

void scrub_table(const std::string& name, const std::vector<int>& starts, const std::vector<int>& dense,
    const std::function<int(int, int64_t)>& map)
{
    Compare t(name);
    std::vector<int64_t> pixels = scrub_pixels();
    for (size_t i = 0; i < starts.size(); ++i)
        for (size_t p = 0; p < pixels.size(); ++p)
            t.next(map(starts[i], pixels[p]), std::to_string(starts[i]) + "," + std::to_string(pixels[p]));
    for (size_t i = 0; i < dense.size(); ++i)
        for (int64_t p = -700; p <= 700; ++p) t.next(map(dense[i], p), std::to_string(dense[i]) + "," + std::to_string(p));
    t.finish();
}

void test_scrubs()
{
    scrub_table("scrub-rate-processing",
        { 0, 1, 2, 3, 10, 100, 1000, 4999, 5000, 5001, 5002, 6000, 10000, 50000, 123456, 500000, 999999, 1000000, 1000001, -1 },
        { 0, 5000 }, [](int s, int64_t p) { return app::map_rate_scrub_value(ServiceDurationMode::ProcessingTime, s, p); });
    scrub_table("scrub-rate-bitrate",
        { 0, 1, 50, 99, 100, 101, 300, 1000, 31250, 38400, 115200, 1000000, 12345678, 99999999, 100000000, 100000001 },
        { 99, 31250 }, [](int s, int64_t p) { return app::map_rate_scrub_value(ServiceDurationMode::MidiBitrate, s, p); });
    scrub_table("scrub-rate-eventrate",
        { 0, 1, 2, 50, 99, 100, 101, 500, 1000, 9999, 10000, 100000, 1234567, 9999998, 9999999, 10000000 },
        { 50, 100 }, [](int s, int64_t p) { return app::map_rate_scrub_value(ServiceDurationMode::EventsPerSecond, s, p); });
    scrub_table("scrub-queue",
        { -3, 0, 1, 2, 3, 10, 100, 250, 499, 500, 501, 502, 1000, 2000, 10000, 50000, 123456, 999999, 1000000, 1000001 },
        { 500, 1 }, app::map_queue_scrub_value);
    scrub_table("scrub-queue-age",
        { -5, 0, 1, 10, 249, 250, 251, 1000, 100000, 999999, 1000000, 1000001, 5000000, 100000000, 1799999999, 1800000000, 1800000001 },
        { 1000000, 1000 }, app::map_queue_age_scrub_value);
}

// DD-104: the logarithmic pointer feel of the scrub boxes (limits as v1's: queue 1..1,000,000, waiting time
// 1..1,800,000,000, processing time 0..1,000,000, bitrate 1..100,000,000, events/s 0..9,999,999).
void test_log_scrub()
{
    struct Range { const char* name; int lo, hi; } ranges[] = {
        { "queue", 1, 1000000 }, { "age", 1, 1800000000 }, { "processing", 0, 1000000 },
        { "bitrate", 1, 100000000 }, { "eventrate", 0, 9999999 } };
    for (size_t r = 0; r < sizeof ranges / sizeof ranges[0]; ++r) {
        const std::string sec = std::string("log-scrub-") + ranges[r].name;
        const int lo = ranges[r].lo, hi = ranges[r].hi;
        int previous = lo;
        int firstTop = -1;
        bool monotonic = true, inRange = true, steady = true;
        int lastStep = 0;
        for (int p = 0; p <= 4000; ++p) {
            int v = app::log_scrub_value(lo, p, lo, hi);
            if (v < previous) monotonic = false;
            if (v < lo || v > hi) inRange = false;
            if (firstTop < 0 && v == hi) firstTop = p;
            int step = v - previous;
            if (p > 1 && step + 1 < lastStep && v < hi) steady = false;   // steps never shrink (rounding tolerance 1)
            lastStep = step;
            previous = v;
        }
        check(sec, monotonic, "non-decreasing with pointer travel");
        check(sec, inRange, "always within the limits");
        check(sec, steady, "steps get steadily coarser");
        check(sec, firstTop > 0 && firstTop < 4000, "reaches the upper limit within 4000 pixels");
        eq_i(sec, "reaches the upper limit", hi, app::log_scrub_value(lo, 1000000, lo, hi));
        eq_i(sec, "reaches the lower limit", lo, app::log_scrub_value(hi, -1000000, lo, hi));
        eq_i(sec, "0 pixels keeps the start", lo, app::log_scrub_value(lo, 0, lo, hi));
        eq_i(sec, "0 pixels keeps a high start", hi, app::log_scrub_value(hi, 0, lo, hi));
        check(sec, app::log_scrub_value(lo, 3, lo, hi) == lo + 1, "one unit up from the lower limit within 3 pixels");
        int mid = lo + (hi - lo) / 7;
        for (int p = -300; p <= 300; p += 25)
            if (app::log_scrub_value(mid, p, lo, hi) != app::log_scrub_value(mid, p, lo, hi)) check(sec, false, "deterministic");
        // round trip: out by p pixels and back by p pixels returns to the start (the mapping is relative to the start)
        for (int p = 1; p <= 400; p += 37) {
            int out = app::log_scrub_value(mid, p, lo, hi);
            int back = app::log_scrub_value(mid, p - p, lo, hi);
            check(sec, back == mid && out >= mid, "round trip returns the start value");
        }
    }
    eq_i("log-scrub-queue", "1 to 2 in 3 pixels", 2, app::log_scrub_value(1, 3, 1, 1000000));
    check("log-scrub-queue", app::log_scrub_value(1, 2, 1, 1000000) <= 2, "1 to 2 within a few pixels");
    check("log-scrub-queue", app::log_scrub_value(2, 6, 1, 1000000) >= 3, "2 to 3 within a few more pixels");
    eq_i("log-scrub-age", "1 to 2 in 3 pixels", 2, app::log_scrub_value(1, 3, 1, 1800000000));
    eq_i("log-scrub-processing", "0 to 1 in 3 pixels", 1, app::log_scrub_value(0, 3, 0, 1000000));
    // a start of 1000 goes up a visibly coarser step per pixel than a start of 2
    check("log-scrub-queue", app::log_scrub_value(1000, 1, 1, 1000000) - 1000 > app::log_scrub_value(2, 1, 1, 1000000) - 2,
        "coarser further up");
    // going out and back along the cumulative travel is stable (same start, the same travel gives the same value)
    check("log-scrub-queue", app::log_scrub_value(5000, 100, 1, 1000000) == app::log_scrub_value(5000, 100, 1, 1000000),
        "same travel, same value");
}

void test_timeline()
{
    {
        std::vector<Row> rows;
        if (read_rows("timeline-position.txt", rows)) {
            for (size_t i = 0; i < rows.size(); ++i) {
                int x = static_cast<int>(to_i(rows[i][3]));
                int left = static_cast<int>(to_i(rows[i][4]));
                int width = static_cast<int>(to_i(rows[i][5]));
                int64_t duration = to_i(rows[i][1]);
                eq_i("timeline-position", "line " + std::to_string(i + 1), to_i(rows[i][6]),
                    app::timeline_position_from_x(x, left, width, duration));
            }
            std::printf("  timeline-position: %d rows\n", static_cast<int>(rows.size()));
        }
    }
    {
        std::vector<Row> rows;
        if (read_rows("timeline-keys.txt", rows)) {
            std::map<std::string, app::TimelineKey> keys;
            keys["Home"] = app::TimelineKey::Home;
            keys["End"] = app::TimelineKey::End;
            keys["Left"] = app::TimelineKey::Left;
            keys["Right"] = app::TimelineKey::Right;
            keys["PageUp"] = app::TimelineKey::PageUp;
            keys["PageDown"] = app::TimelineKey::PageDown;
            for (size_t i = 0; i < rows.size(); ++i)
                eq_i("timeline-keys", rows[i][0] + " " + rows[i][1] + "/" + rows[i][2], to_i(rows[i][3]),
                    app::timeline_key_target(keys[rows[i][0]], to_i(rows[i][1]), to_i(rows[i][2])));
            std::printf("  timeline-keys: %d rows\n", static_cast<int>(rows.size()));
        }
    }
    check("timeline", app::clamp_timeline_position(-5, 100) == 0 && app::clamp_timeline_position(500, 100) == 100 &&
        app::clamp_timeline_duration(-3) == 0, "timeline clamps");
    check("timeline", app::timeline_fill_width(300, 50, 100) == 150 && app::timeline_fill_width(300, 5, 0) == 0, "fill width");
}

void test_scale_and_fonts()
{
    const int canonical[] = { -50, -17, -8, -4, -3, -1, 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 64, 100, 235, 640 };
    {
        Compare t("scale-metric");
        for (int percent = 50; percent <= 200; ++percent)
            for (size_t i = 0; i < sizeof canonical / sizeof canonical[0]; ++i)
                t.next(app::scale_metric(canonical[i], percent), std::to_string(canonical[i]) + "@" + std::to_string(percent));
        t.finish();
    }
    {
        Compare t("scale-metric-min1");
        for (int percent = 50; percent <= 200; ++percent)
            for (size_t i = 0; i < sizeof canonical / sizeof canonical[0]; ++i)
                t.next(app::scale_metric_min1(canonical[i], percent), std::to_string(canonical[i]) + "@" + std::to_string(percent));
        t.finish();
    }
    std::vector<Row> rows;
    if (read_rows("ui-font.txt", rows)) {
        for (size_t i = 0; i < rows.size(); ++i) {
            float canonicalPoints = bits_to_float(rows[i][1]);
            int percent = static_cast<int>(to_i(rows[i][2]));
            app::UiFontChoice choice = app::ui_font_choice(canonicalPoints, percent);
            std::string label = rows[i][0] + " " + rows[i][1] + " @" + rows[i][2];
            eq("ui-font", label + " family", rows[i][3], app::ui_font_family(rows[i][0] == "mono", choice.small_family));
            eq("ui-font", label + " points", rows[i][4], hex32(float_to_bits(choice.points)));
        }
        std::printf("  ui-font: %d rows\n", static_cast<int>(rows.size()));
    }
}

// ---- ScrubOrTypeTextBox -------------------------------------------------------------------------------

void configure_kind(app::ScrubField& f, const std::string& kind)
{
    if (kind == "numeric") f.configure_numeric(1234, 0, 1000000, 1, 4, 1, 2);
    else if (kind == "channel") f.configure(5, 0, 127, 1, 1, 4, 1, 8, false, false);
    else if (kind == "offset0") f.configure(5, 0, 127, 0, 1, 4, 1, 8, false, false);
    else f.configure(0, 0, 1, 0, 1, 1, 1, 1, false, false);
}

void test_scrub_parse()
{
    const char* cultures[] = { "en-US", "de-DE" };
    for (size_t k = 0; k < 2; ++k) {
        std::string section = std::string("scrub-parse ") + cultures[k];
        std::vector<Row> rows;
        if (!read_rows(std::string("scrub-parse-") + cultures[k] + ".txt", rows)) return;
        NumberCulture c = culture_from_row(rows[0]);
        std::map<std::string, app::ScrubField> fields;
        const char* kinds[] = { "numeric", "channel", "toggle" };
        for (size_t i = 0; i < 3; ++i) configure_kind(fields[kinds[i]], kinds[i]);
        int parseRows = 0;
        for (size_t i = 1; i < rows.size(); ++i) {
            const Row& r = rows[i];
            app::ScrubField& f = fields[r[1]];
            if (r[0] == "parse") {
                int value = 0;
                bool ok = f.try_parse_editable(r[2], value, c);
                ++parseRows;
                std::string label = r[1] + " \"" + show(r[2]) + "\"";
                if (r[3] == "1" && !r[2].empty() && r[2][r[2].size() - 1] == '\0') {
                    // DD-042: v1's .NET parser accepted NUL characters after the number; v2 refuses them.
                    eq(section, label + " ok (DD-042: v1 accepted trailing NUL characters)", "0", ok ? "1" : "0");
                } else {
                    eq(section, label + " ok", r[3], ok ? "1" : "0");
                    eq_i(section, label + " value", to_i(r[4]), value);
                }
            } else if (r[0] == "format") {
                eq(section, r[1] + " format " + r[2], r[3], f.format_editable(static_cast<int>(to_i(r[2])), c));
            } else if (r[0] == "flat") {
                f.set_value(static_cast<int>(to_i(r[2])));
                eq(section, r[1] + " flat " + r[2], r[3], f.flat_text(c));
            }
        }
        std::printf("  %s: %d parse cases\n", section.c_str(), parseRows);
    }
    // try_parse_int32 facts (also covered by the tables)
    int32_t v = 0;
    check("parse", app::try_parse_int32("1,234", true, v) && v == 1234, "thousands parse");
    check("parse", !app::try_parse_int32("1,234", false, v), "no thousands");
    check("parse", app::try_parse_int32("-2147483648", false, v) && v == INT32_MIN, "int32 min");
    check("parse", !app::try_parse_int32("2147483648", false, v), "int32 overflow");
    check("parse", app::trim_net("\xC2\xA0 x \xE2\x80\x83") == "x", "trim");
}

struct ScriptBox {
    app::ScrubField f;
    std::string text;
    std::string error;
    bool typing = false;
    std::function<int(int, int64_t)> mapping;

    void set_flat()
    {
        if (!typing) text = f.flat_text();
    }
    // RequestValue as the control runs it (typing flag as it is at the call).
    void request(int value)
    {
        app::ScrubField::Change change = f.request_value(value);
        if (!change.changed) return;
        error.clear();
        set_flat();
    }
    void exit_typing()
    {
        typing = false;
        text = f.flat_text();
    }
};

std::function<int(int, int64_t)> mapping_by_name(const std::string& name)
{
    if (name == "queue") return app::map_queue_scrub_value;
    if (name == "age") return app::map_queue_age_scrub_value;
    if (name == "rate0") return [](int s, int64_t p) { return app::map_rate_scrub_value(ServiceDurationMode::ProcessingTime, s, p); };
    if (name == "rate1") return [](int s, int64_t p) { return app::map_rate_scrub_value(ServiceDurationMode::MidiBitrate, s, p); };
    if (name == "rate2") return [](int s, int64_t p) { return app::map_rate_scrub_value(ServiceDurationMode::EventsPerSecond, s, p); };
    return std::function<int(int, int64_t)>();
}

void test_scrub_scripts()
{
    std::vector<Row> rows;
    if (!read_rows("scrub-scripts.txt", rows)) return;
    std::unique_ptr<ScriptBox> box;
    std::string scenario;
    int ops = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r[0] == "scenario") {
            box.reset(new ScriptBox());
            scenario = r[1];
            const std::string& kind = r[2];
            int value = static_cast<int>(to_i(r[3])), minimum = static_cast<int>(to_i(r[4])), maximum = static_cast<int>(to_i(r[5]));
            int cu = static_cast<int>(to_i(r[6])), cp = static_cast<int>(to_i(r[7])), fu = static_cast<int>(to_i(r[8])), fp = static_cast<int>(to_i(r[9]));
            int offset = static_cast<int>(to_i(r[10])), increment = static_cast<int>(to_i(r[11]));
            if (kind == "numeric") {
                box->f.configure_numeric(value, minimum, maximum, cu, cp, fu, fp);
                box->f.set_numeric_scrub_mapping(mapping_by_name(r[12]));
                box->f.set_increment(increment);
                box->f.set_thousands_separator(true);
            } else {
                box->f.configure(value, minimum, maximum, offset, cu, cp, fu, fp, false, false);
            }
            box->text = box->f.flat_text();
            continue;
        }
        // op row: op, args..., value, forced, typing, error, text
        const std::string& op = r[0];
        size_t argCount = r.size() - 6;
        std::string label = scenario + " op #" + std::to_string(i) + " " + op;
        ScriptBox& b = *box;
        if (op == "scrub") {
            std::optional<int> requested = b.f.scrub(static_cast<int>(to_i(r[1])), to_b(r[2]));
            if (requested.has_value()) b.request(*requested);
        } else if (op == "begin") {
            b.f.start_scrubbing();
        } else if (op == "end") {
            b.f.end_gesture(false);
            b.set_flat();
        } else if (op == "key") {
            bool up = r[1] == "up";
            if (b.f.numeric_only() && !b.typing) b.request(b.f.step_value(up));
            else if (b.typing) {
                b.text = b.f.adjust_typed_text(b.text, up ? 1 : -1);
                b.error.clear();
            }
        } else if (op == "type") {
            b.f.end_gesture(false);
            b.set_flat();
            b.typing = true;
            b.text = b.f.begin_typing();
        } else if (op == "commit") {
            app::ScrubField::CommitResult result = b.f.commit_typed(r[1], r[2] == "1");
            if (!result.accepted) {
                b.error = result.error;
            } else if (result.request) {
                b.request(result.value);   // still typing here, as in the control
            } else {
                b.error.clear();
            }
            b.exit_typing();
        } else if (op == "arrow") {
            if (b.typing) {
                b.text = b.f.adjust_typed_text(b.text, r[1] == "up" ? 1 : -1);
                b.error.clear();
            }
        } else if (op == "escape") {
            if (b.typing) {
                b.error.clear();
                b.f.abandon_typing();
                b.exit_typing();
            }
            b.f.end_gesture(false);
            b.set_flat();
        } else if (op == "request") {
            b.request(static_cast<int>(to_i(r[1])));
        } else {
            fail("scrub-scripts", "unknown op " + op);
            continue;
        }
        ++ops;
        eq_i("scrub-scripts", label + " value", to_i(r[argCount + 1]), b.f.value());
        eq_i("scrub-scripts", label + " forced", to_i(r[argCount + 2]), b.f.forced() ? 1 : 0);
        eq_i("scrub-scripts", label + " typing", to_i(r[argCount + 3]), b.typing ? 1 : 0);
        eq("scrub-scripts", label + " error", r[argCount + 4], b.error);
        eq("scrub-scripts", label + " text", r[argCount + 5], b.text);
    }
    std::printf("  scrub-scripts: %d operations\n", ops);
}

// ---- effective speed / output rate ---------------------------------------------------------------------

std::string opt_hex(const std::optional<double>& value)
{
    if (!value.has_value()) return "null";
    uint64_t bits;
    std::memcpy(&bits, &*value, sizeof bits);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(bits));
    return buf;
}

void test_speed_windows()
{
    {
        std::vector<Row> rows;
        if (read_rows("speed-window.txt", rows)) {
            for (size_t i = 0; i < rows.size(); ++i) {
                const Row& r = rows[i];
                if (r[0] == "window") {
                    int64_t w = to_i(r[1]);
                    eq("speed-window", "valid " + r[1], r[2], app::EffectivePlaybackSpeed::is_valid_window(w) ? "1" : "0");
                    app::EffectivePlaybackSpeed speed;
                    eq("speed-window", "set " + r[1], r[3], speed.set_window(w) ? "ok" : "E");
                }
                // The "parse" rows of v1's table test its settings-file reader; v2 keeps no settings file (DD-026).
            }
        }
    }
    {
        std::vector<Row> rows;
        if (read_rows("effective-speed.txt", rows)) {
            app::EffectivePlaybackSpeed speed;
            int adds = 0;
            for (size_t i = 0; i < rows.size(); ++i) {
                const Row& r = rows[i];
                if (r[0] == "window") {
                    speed = app::EffectivePlaybackSpeed();
                    speed.set_window(to_i(r[1]));
                } else if (r[0] == "set") {
                    speed.set_window(to_i(r[1]));
                } else {
                    std::optional<double> result = speed.add(to_i(r[1]), to_i(r[2]), to_i(r[3]), to_i(r[4]), to_b(r[5]), to_i(r[6]));
                    eq("effective-speed", "row " + std::to_string(i + 1), r[7], opt_hex(result));
                    ++adds;
                }
            }
            std::printf("  effective-speed: %d samples\n", adds);
        }
    }
    {
        std::vector<Row> rows;
        if (read_rows("output-rate.txt", rows)) {
            app::RollingOutputRate rate;
            int adds = 0;
            for (size_t i = 0; i < rows.size(); ++i) {
                const Row& r = rows[i];
                if (r[0] == "run") rate = app::RollingOutputRate();
                else if (r[0] == "restart") rate.restart_window();
                else if (r[0] == "reset") rate.reset();
                else {
                    std::optional<double> result = rate.add(to_i(r[1]), to_i(r[2]));
                    eq("output-rate", "row " + std::to_string(i + 1), r[3], opt_hex(result));
                    eq("output-rate", "maximum row " + std::to_string(i + 1), r[4], opt_hex(rate.maximum_observed()));
                    ++adds;
                }
            }
            std::printf("  output-rate: %d samples\n", adds);
        }
    }
}

void test_statistics_state()
{
    app::LiveStatistics stats;
    engine::PlaybackSnapshot s;
    s.state = engine::PlaybackState::Playing;
    int64_t t = 1000000;
    app::StatisticsSample sample = stats.update(s, t);
    check("statistics", !sample.overflow_pulse && sample.synchronized, "first sample is synchronized, no pulse");
    s.dropped_events = 5;
    t += 50000;
    sample = stats.update(s, t);
    check("statistics", sample.overflow_pulse && sample.total_dropped == 5, "drop starts the pulse");
    t += 799999;
    check("statistics", stats.update(s, t).overflow_pulse, "pulse lasts 800 ms");
    t += 1;
    check("statistics", !stats.update(s, t).overflow_pulse, "pulse ends");
    s.state = engine::PlaybackState::Stopped;
    t += 1000;
    sample = stats.update(s, t);
    check("statistics", !sample.effective_speed.has_value() && !sample.output_rate.has_value(), "no rates while stopped");
    stats.reset();
    check("statistics", !stats.output_rate().maximum_observed().has_value(), "reset clears the maximum");
    s.processing_mode = engine::ProcessingMode::OutputRateLimit;
    check("statistics", !app::statistics_synchronized(s), "gates are never synchronized");
    s.processing_mode = engine::ProcessingMode::Queue;
    s.outstanding_events = 1;
    check("statistics", !app::statistics_synchronized(s), "backlog is not synchronized");
    app::QueuePressureInput in = app::queue_pressure_input(s, true, false, false, false, false, false, 100);
    check("statistics", in.limited && in.occupied == 1 && in.limit == 100 && !in.virtual_queue && !in.age_pressure, "pressure input");
    in = app::queue_pressure_input(s, true, true, false, false, false, false, 100);
    check("statistics", !in.limited, "gates hide the bar");
    s.queue_age_microseconds = -4;
    s.queue_age_limit_microseconds = 0;
    in = app::queue_pressure_input(s, true, false, false, false, false, true, 100);
    check("statistics", in.age_pressure && in.occupied == 0 && in.limit == 1, "age pressure clamps");
    check("statistics", app::speed_window_presets().size() == 5 && app::speed_window_presets()[4].microseconds == 1500000, "speed presets");
    check("statistics", app::speed_window_dialog_initial_ms(0) == 250.0 && app::speed_window_dialog_initial_ms(10000000) == 10000.0 &&
        app::speed_window_dialog_initial_ms(30000) == 30.0, "dialog start");
    check("statistics", app::speed_window_from_dialog_ms(30.0) == 30000 && app::speed_window_from_dialog_ms(1500.0) == 1500000, "dialog result");
    // queue pressure tooltip = the bar text, the explanation, the purple note
    std::string tip = format::queue_pressure_tooltip(true, false, 5, 10);
    check("statistics", tip.find("Virtual 5 / 10 \xE2\x80\x94 50%\r\nVirtual pressure uses") == 0 &&
        tip.find("\r\nPurple means protected pending work has exceeded the configured soft limit.") != std::string::npos, "pressure tooltip");
}

// v2's Help is its own text (UD-008 resolved), not v1's. The test checks what must hold for every topic and that the
// subjects v2 changed are explained, using the register's wording for the names the user sees.
void test_help()
{
    std::vector<Row> titles;
    const char* names[] = { "Overview", "FileAndOutput", "RateModel", "QueueAndOverflow", "Playback", "Analysis", "Statistics", "Channels" };
    // The topic titles and their order are v1's: the F1 mapping and the list box depend on them.
    if (read_rows("help-titles.txt", titles)) {
        for (int i = 0; i < app::kHelpTopicCount; ++i) {
            eq("help", std::string("title ") + names[i], titles[static_cast<size_t>(i)][1], app::help_topic_title(static_cast<app::HelpTopic>(i)));
            eq("help", std::string("title key ") + names[i], names[i], titles[static_cast<size_t>(i)][0]);
        }
    }
    // Phrases each topic must contain (names of menu items, choices and windows exactly as the program shows them).
    const char* const required[8][6] = {
        /* Overview */ { "Application scale", "Borderless style", "Integrated header", "Auto-hide overlay", "remembers", "settings.ini" },
        /* FileAndOutput */ { "Direct WinMM driver access", "Track routing", "Use original routing", "KDMAPI", "None", nullptr },
        /* RateModel */ { "Per-note interval gate", "Delayed NoteOff", "Note start rate gate", "Maximum output rate", "Events per second", nullptr },
        /* QueueAndOverflow */ { "Limit queue by waiting time", "Clear buffer and jump to realtime", "Drop oldest complete note", "Drop newest events", "Drop background notes", "Interval polyphony" },
        /* Playback */ { "Chase MIDI state on Play/Seek", "Preserve playback queue on Pause and Seek", "Output-paced timeline", "Direct WinMM", nullptr, nullptr },
        /* Analysis */ { "Project forced overrides", "Seek to pin", "Custom resolution", nullptr, nullptr, nullptr },
        /* Statistics */ { "Reset stats", "Effective speed", "Lag", nullptr, nullptr, nullptr },
        /* Channels */ { "With the keyboard", "forced overrides", "cancels an edit", nullptr, nullptr, nullptr },
    };
    for (int i = 0; i < app::kHelpTopicCount; ++i) {
        std::string text = app::help_text(static_cast<app::HelpTopic>(i), app::HelpPlatform::Windows);
        check("help", !text.empty(), std::string("topic ") + names[i] + " is not empty");
        // The text is markup with "\n" line breaks (a "\r" before one is ignored by the parser); it starts with the topic heading, and
        // every block parses to something non-empty.
        std::vector<app::HelpBlock> blocks = app::parse_help_markup(text);
        check("help", !blocks.empty() && blocks[0].kind == app::HelpBlockKind::Heading &&
            blocks[0].runs[0].text == app::help_topic_title(static_cast<app::HelpTopic>(i)), std::string("topic ") + names[i] + " starts with its heading");
        int longParagraphs = 0, subheadings = 0;
        for (size_t k = 0; k < blocks.size(); ++k) {
            size_t len = 0;
            for (size_t r = 0; r < blocks[k].runs.size(); ++r) len += blocks[k].runs[r].text.size();
            if (blocks[k].kind == app::HelpBlockKind::Paragraph && len > 450) ++longParagraphs;
            if (blocks[k].kind == app::HelpBlockKind::Subheading) ++subheadings;
        }
        check("help", longParagraphs == 0, std::string("topic ") + names[i] + " has no wall-of-text paragraph");
        check("help", subheadings >= 1, std::string("topic ") + names[i] + " has section headings");
        for (int k = 0; k < 6; ++k) {
            if (!required[i][k]) continue;
            check("help", text.find(required[i][k]) != std::string::npos, std::string("topic ") + names[i] + " mentions \"" + required[i][k] + "\"");
        }
        // v1's old overflow names must not come back.
        check("help", text.find("Drop newest or Drop oldest") == std::string::npos, std::string("topic ") + names[i] + " has no v1 overflow names");
        std::printf("  help %s: %d bytes\n", names[i], static_cast<int>(text.size()));
    }
    check("help", app::help_text(static_cast<app::HelpTopic>(99)) == app::help_text(app::HelpTopic::Overview), "out-of-range topic is the overview");

    // The Linux wording (DD-126): same eight topics and titles, ALSA instead of WinMM/KDMAPI, no Windows-only items; checked on every build.
    for (int i = 0; i < app::kHelpTopicCount; ++i) {
        const app::HelpTopic topic = static_cast<app::HelpTopic>(i);
        const std::string text = app::help_text(topic, app::HelpPlatform::Linux);
        const std::vector<app::HelpBlock> blocks = app::parse_help_markup(text);
        check("help-linux", !blocks.empty() && blocks[0].kind == app::HelpBlockKind::Heading &&
            blocks[0].runs[0].text == app::help_topic_title(topic), std::string("topic ") + names[i] + " starts with its heading");
        const char* const windowsOnly[] = { "WinMM", "KDMAPI", "OmniMIDI", "VirtualMIDISynth", "title-bar", "Alt+Space", "Windows", "Borderless", "borderless", "Integrated header", "Auto-hide", "Wavetable", "taskbar" };
        for (const char* word : windowsOnly)
            check("help-linux", text.find(word) == std::string::npos, std::string("topic ") + names[i] + " has no \"" + word + "\" on Linux");
        // Everything the Linux text still describes must stay (the rest of the topic is shared with Windows).
        if (i == static_cast<int>(app::HelpTopic::Overview) || i == static_cast<int>(app::HelpTopic::FileAndOutput))
            check("help-linux", text.find("ALSA") != std::string::npos, std::string("topic ") + names[i] + " names ALSA");
        if (i != static_cast<int>(app::HelpTopic::Overview) && i != static_cast<int>(app::HelpTopic::FileAndOutput) && i != static_cast<int>(app::HelpTopic::Playback) &&
            i != static_cast<int>(app::HelpTopic::QueueAndOverflow) && i != static_cast<int>(app::HelpTopic::Analysis))
            check("help-linux", text == app::help_text(topic, app::HelpPlatform::Windows), std::string("topic ") + names[i] + " is shared with Windows");
    }
    check("help-linux", app::help_text(app::HelpTopic::FileAndOutput, app::HelpPlatform::Linux).find("Use original routing") != std::string::npos, "Linux File and output keeps Track routing");
    check("help-linux", app::help_text(app::HelpTopic::Playback, app::HelpPlatform::Linux).find("Output-paced timeline") != std::string::npos, "Linux Playback keeps its options");
    check("help-linux", app::help_text(app::HelpTopic::QueueAndOverflow, app::HelpPlatform::Linux).find("Limit queue by waiting time") != std::string::npos, "Linux Queue keeps its options");
    // The Android wording: same eight topics and titles; file picker, USB MIDI devices and the built-in Sonivox synthesizer, touch, screen-off playback.
    for (int i = 0; i < app::kHelpTopicCount; ++i) {
        const app::HelpTopic topic = static_cast<app::HelpTopic>(i);
        const std::string text = app::help_text(topic, app::HelpPlatform::Android);
        const std::vector<app::HelpBlock> blocks = app::parse_help_markup(text);
        check("help-android", !blocks.empty() && blocks[0].kind == app::HelpBlockKind::Heading &&
            blocks[0].runs[0].text == app::help_topic_title(topic), std::string("topic ") + names[i] + " starts with its heading");
        const char* const notOnAndroid[] = { "WinMM", "KDMAPI", "OmniMIDI", "VirtualMIDISynth", "title-bar", "Alt+Space", "Windows", "Borderless", "borderless", "Integrated header",
            "Auto-hide", "Wavetable", "taskbar", "ALSA", "FluidSynth", "audio** group", "drag one file", "onto the main window", "Right-click", "Ctrl+", "mouse", "keyboard", "Hover", "F1", "Direct WinMM", "menu button" };
        for (const char* word : notOnAndroid)
            check("help-android", text.find(word) == std::string::npos, std::string("topic ") + names[i] + " has no \"" + word + "\" on Android");
        check("help-android", text != app::help_text(topic, app::HelpPlatform::Windows) || i == static_cast<int>(app::HelpTopic::RateModel) ||
            i == static_cast<int>(app::HelpTopic::Statistics), std::string("topic ") + names[i] + " differs from Windows where it should");
        if (i == static_cast<int>(app::HelpTopic::RateModel) || i == static_cast<int>(app::HelpTopic::Statistics))
            check("help-android", text == app::help_text(topic, app::HelpPlatform::Windows), std::string("topic ") + names[i] + " is shared with Windows");
    }
    {
        const std::string overview = app::help_text(app::HelpTopic::Overview, app::HelpPlatform::Android);
        check("help-android", overview.find("file picker") != std::string::npos, "Android Overview names the file picker");
        check("help-android", overview.find("USB MIDI") != std::string::npos, "Android Overview names USB MIDI devices");
        check("help-android", overview.find("Built-in synth (Sonivox)") != std::string::npos && overview.find("no built-in MIDI synthesizer") == std::string::npos,
            "Android Overview names the built-in synthesizer");
        check("help-android", overview.find("Sonivox") != std::string::npos && overview.find("Apache License 2.0") != std::string::npos && overview.find("licence text is in the app") != std::string::npos,
            "Android Overview credits Sonivox, its licence and the licence text in the app");
        check("help-android", overview.find("screen") != std::string::npos && overview.find("notification") != std::string::npos, "Android Overview explains screen-off playback through the notification");
        check("help-android", overview.find("Rescan") != std::string::npos, "Android Overview names Rescan");
        const std::string output = app::help_text(app::HelpTopic::FileAndOutput, app::HelpPlatform::Android);
        check("help-android", output.find("file picker") != std::string::npos && output.find("USB MIDI device") != std::string::npos && output.find("Use original routing") != std::string::npos,
            "Android File and output has the picker, USB devices and Track routing");
        check("help-android", output.find("falls back to **None**") != std::string::npos, "Android File and output says Rescan falls back to None");
        check("help-android", output.find("**Built-in synth (Sonivox)**") != std::string::npos && output.find("Apache License 2.0") != std::string::npos,
            "Android File and output describes the built-in synth and its licence");
        for (int w = 0; w < app::kHelpTopicCount; ++w)
            if (w != static_cast<int>(app::HelpTopic::Overview) && w != static_cast<int>(app::HelpTopic::FileAndOutput))
                check("help-android", app::help_text(static_cast<app::HelpTopic>(w), app::HelpPlatform::Android).find("Sonivox") == std::string::npos, "the Sonivox credit is only in Overview and File and output");
        for (int w = 0; w < app::kHelpTopicCount; ++w)
            for (app::HelpPlatform other : { app::HelpPlatform::Windows, app::HelpPlatform::Linux })
                check("help-android", app::help_text(static_cast<app::HelpTopic>(w), other).find("Sonivox") == std::string::npos, "Windows and Linux Help do not mention Sonivox");
        check("help-android", app::help_text(app::HelpTopic::Playback, app::HelpPlatform::Android).find("Output-paced timeline") != std::string::npos, "Android Playback keeps its options");
        check("help-android", app::help_text(app::HelpTopic::QueueAndOverflow, app::HelpPlatform::Android).find("In the menu, **Limit queue by waiting time**") != std::string::npos,
            "Android Queue points at the menu");
        check("help-android", overview.find("## The menu") != std::string::npos && overview.find("**Synth settings...**") != std::string::npos &&
            overview.find("**Help**") != std::string::npos, "Android Overview describes the top bar's menu with Synth settings and Help");
        check("help-android", output.find("## Synth settings") != std::string::npos && output.find("**Load DLS sound bank...**") != std::string::npos &&
            output.find("**CLIP**") != std::string::npos && output.find("1024") != std::string::npos, "Android File and output describes the Synth settings page");
        for (int w = 0; w < app::kHelpTopicCount; ++w)
            check("help-android", app::help_text(static_cast<app::HelpTopic>(w), app::HelpPlatform::Android).find("More settings") == std::string::npos,
                "the Android Help no longer names the removed More settings section");
        for (int w = 0; w < app::kHelpTopicCount; ++w)
            for (app::HelpPlatform other : { app::HelpPlatform::Windows, app::HelpPlatform::Linux })
                check("help-android", app::help_text(static_cast<app::HelpTopic>(w), other).find("Synth settings") == std::string::npos, "Windows and Linux Help have no Synth settings");
        check("help-android", app::help_text(static_cast<app::HelpTopic>(99), app::HelpPlatform::Android) == overview, "out-of-range topic is the Android overview");
    }
    check("help", app::help_text(app::HelpTopic::FileAndOutput) == app::help_text(app::HelpTopic::FileAndOutput, app::kNativeHelpPlatform), "the one-argument form is the native wording");

    // The markup parser (portable; the Win32 and a later GTK front end both draw its blocks).
    using app::HelpBlockKind;
    std::vector<app::HelpBlock> b = app::parse_help_markup("# Title\r\n\r\n## Part\nPlain **bold** and plain.\n- item **one**\n12. step twelve\n3.not a step\n**unclosed bold\n");
    check("help-markup", b.size() == 7, "seven blocks (blank lines are ignored, CR is ignored)");
    if (b.size() == 7) {
        check("help-markup", b[0].kind == HelpBlockKind::Heading && b[0].runs.size() == 1 && b[0].runs[0].text == "Title" && !b[0].runs[0].bold, "heading");
        check("help-markup", b[1].kind == HelpBlockKind::Subheading && b[1].runs[0].text == "Part", "subheading");
        check("help-markup", b[2].kind == HelpBlockKind::Paragraph && b[2].runs.size() == 3 && b[2].runs[0].text == "Plain " && !b[2].runs[0].bold &&
            b[2].runs[1].text == "bold" && b[2].runs[1].bold && b[2].runs[2].text == " and plain." && !b[2].runs[2].bold, "bold inside a paragraph");
        check("help-markup", b[3].kind == HelpBlockKind::Bullet && b[3].runs.size() == 2 && b[3].runs[0].text == "item " && b[3].runs[1].bold && b[3].runs[1].text == "one", "bullet with bold");
        check("help-markup", b[4].kind == HelpBlockKind::Step && b[4].number == 12 && b[4].runs[0].text == "step twelve", "numbered step keeps its number");
        check("help-markup", b[5].kind == HelpBlockKind::Paragraph && b[5].runs[0].text == "3.not a step", "a number without a space is plain text");
        check("help-markup", b[6].kind == HelpBlockKind::Paragraph && b[6].runs.size() == 1 && b[6].runs[0].text == "**unclosed bold" && !b[6].runs[0].bold,
            "an unclosed ** stays literal");
    }
    check("help-markup", app::parse_help_markup("").empty(), "empty text has no blocks");
    std::vector<app::HelpBlock> e = app::parse_help_markup("- \n");
    check("help-markup", e.size() == 1 && e[0].kind == HelpBlockKind::Paragraph, "a bullet marker without text is not a bullet (trailing space is trimmed)");
}
} // namespace

// ---- adaptive display rate: the tier rule (display_tier.h), pure --------------------------------------------------------

app::DisplaySample sim_sample(const app::DisplayLadder& ladder, int native, int tier, double movingUs, double statsFullUs, bool behind)
{
    app::DisplaySample s;
    const int rate = app::display_tier_rate(ladder, tier, native);
    s.window_us = 500000;
    s.updates = rate / 2;
    s.moving_us = static_cast<int64_t>(s.updates * movingUs);
    s.statistics_us = static_cast<int64_t>(s.updates * statsFullUs * (ladder.half_statistics[tier] ? 0.5 : 1.0));
    s.behind = behind;
    return s;
}

// Runs the rule in a closed loop (the cost depends on the tier it chose); returns the tier after each window.
std::vector<int> run_tiers(const app::DisplayLadder& ladder, int native, int windows, double movingUs, double statsUs, int startTier = 0,
    int behindFrom = -1, int behindTo = -1)
{
    std::vector<int> tiers;
    app::DisplayTierState st;
    st.tier = startTier;
    app::DisplayTierParams params;
    for (int w = 0; w < windows; ++w) {
        const bool behind = w >= behindFrom && w < behindTo;
        st = app::next_display_tier(ladder, native, st, sim_sample(ladder, native, st.tier, movingUs, statsUs, behind), params);
        tiers.push_back(st.tier);
    }
    return tiers;
}

void test_display_tiers()
{
    const std::string sec = "display-tiers";
    // the budget by processor count: scarcer processors, tighter budget
    check(sec, app::display_budget_for_processors(1) < app::display_budget_for_processors(2) &&
        app::display_budget_for_processors(2) < app::display_budget_for_processors(3), "budget grows with the processor count");
    eq_i(sec, "budget 12 processors = 3", 0, app::display_budget_for_processors(12) == app::display_budget_for_processors(3) ? 0 : 1);
    eq_i(sec, "budget unknown count (0) is the tightest", 0, app::display_budget_for_processors(0) == app::display_budget_for_processors(1) ? 0 : 1);
    // ladders: rungs clearly below the screen's rate only
    app::DisplayLadder l144 = app::make_display_ladder(144), l60 = app::make_display_ladder(60), l30 = app::make_display_ladder(30);
    eq_i(sec, "ladder 144 count", 6, l144.count);
    eq_i(sec, "ladder 144 rung 2", 60, l144.rate_hz[2]);
    eq_i(sec, "ladder 144 last", 10, l144.rate_hz[5]);
    eq_i(sec, "ladder 60 count (no 60 rung)", 5, l60.count);
    eq_i(sec, "ladder 60 rung 2", 30, l60.rate_hz[2]);
    eq_i(sec, "ladder 59 (59.94 Hz) like 60", 5, app::make_display_ladder(59).count);
    eq_i(sec, "ladder 30 count", 4, l30.count);
    eq_i(sec, "ladder unknown rate reads as 60", 5, app::make_display_ladder(0).count);
    check(sec, !l144.half_statistics[0] && l144.half_statistics[1] && l144.half_statistics[5], "half statistics from tier 1 on, never at tier 0");
    eq_i(sec, "rate tier 0 is native", 144, app::display_tier_rate(l144, 0, 144));
    eq_i(sec, "rate tier 1 is native", 144, app::display_tier_rate(l144, 1, 144));
    eq_i(sec, "rate tier 2", 60, app::display_tier_rate(l144, 2, 144));
    // a modern PC stays at full rate for a long time (60 Hz: 300 us per update; 144 Hz: 500 us per update, 7 %)
    {
        std::vector<int> a = run_tiers(l60, 60, 200, 100, 200), b = run_tiers(l144, 144, 200, 200, 300);
        int maxA = 0, maxB = 0;
        for (size_t i = 0; i < a.size(); ++i) maxA = std::max(maxA, a[i]);
        for (size_t i = 0; i < b.size(); ++i) maxB = std::max(maxB, b[i]);
        eq_i(sec, "modern 60 Hz PC stays at tier 0", 0, maxA);
        eq_i(sec, "modern 144 Hz PC stays at tier 0", 0, maxB);
    }
    // one window over the budget is not enough; two in a row are; far over steps at once
    {
        app::DisplayTierParams p;
        app::DisplayTierState st;
        app::DisplaySample over = sim_sample(l60, 60, 0, 2400, 0, false);   // 60 x 2.4 ms = 14 % of the thread
        check(sec, app::display_busy_share(over) > p.budget && app::display_busy_share(over) < p.budget * p.far_over, "test sample is over, not far over");
        st = app::next_display_tier(l60, 60, st, over, p);
        eq_i(sec, "one window over: still tier 0", 0, st.tier);
        st = app::next_display_tier(l60, 60, st, over, p);
        eq_i(sec, "two windows over: tier 1", 1, st.tier);
        app::DisplaySample far = sim_sample(l60, 60, 1, 6000, 0, false);
        st = app::next_display_tier(l60, 60, app::DisplayTierState(), far, p);
        eq_i(sec, "far over: tier 1 at once", 1, st.tier);
        // an over window followed by a good one resets the count
        app::DisplayTierState s2 = app::next_display_tier(l60, 60, app::DisplayTierState(), over, p);
        s2 = app::next_display_tier(l60, 60, s2, sim_sample(l60, 60, 0, 300, 0, false), p);
        s2 = app::next_display_tier(l60, 60, s2, over, p);
        eq_i(sec, "over, good, over: still tier 0", 0, s2.tier);
        // nothing measured: no change
        app::DisplaySample none;
        eq_i(sec, "empty window: unchanged", 0, app::next_display_tier(l60, 60, st, none, p).tier == st.tier ? 0 : 1);
    }
    // heavy fixed cost (4 ms per update at 60 Hz): walks down to 15 per second and stays there
    {
        std::vector<int> t = run_tiers(l60, 60, 60, 4000, 0);
        eq_i(sec, "heavy cost settles at the 15/s rung (tier 3)", 3, t.back());
        bool monotone = true;
        for (size_t i = 1; i < t.size(); ++i) if (t[i] < t[i - 1]) monotone = false;
        check(sec, monotone, "heavy cost: tiers only go down");
        int changesLate = 0;
        for (size_t i = 30; i < t.size(); ++i) if (t[i] != t[i - 1]) ++changesLate;
        eq_i(sec, "heavy cost: no change in the last 30 windows", 0, changesLate);
    }
    // statistics-heavy cost: halving the statistics is the first step and, with the 30/s rung, enough
    {
        std::vector<int> t = run_tiers(l60, 60, 40, 200, 3000);   // 60 x 3.2 ms = 19 %; half: 60 x 1.7 ms = 10 %; 30/s: 5 %
        int firstChange = -1;
        for (size_t i = 0; i < t.size() && firstChange < 0; ++i) if (t[i] != 0) firstChange = static_cast<int>(i);
        eq_i(sec, "statistics-heavy: first step is tier 1 (half statistics)", 1, t[static_cast<size_t>(firstChange)]);
        eq_i(sec, "statistics-heavy: settles at tier 2 (30/s)", 2, t.back());
    }
    // release: cost gone, one rung per 6 calm windows, never before
    {
        app::DisplayTierParams p;
        app::DisplayTierState st;
        st.tier = 3;
        int steps = 0;
        std::vector<int> when;
        for (int w = 0; w < 60; ++w) {
            int before = st.tier;
            st = app::next_display_tier(l60, 60, st, sim_sample(l60, 60, st.tier, 300, 300, false), p);
            if (st.tier != before) when.push_back(w + 1);
            if (st.tier > before) ++steps;
        }
        eq_i(sec, "release: back to tier 0", 0, st.tier);
        eq_i(sec, "release: never steps down again", 0, steps);
        eq_i(sec, "release: three rungs", 3, static_cast<int64_t>(when.size()));
        check(sec, when.size() == 3 && when[0] == p.calm_windows && when[1] == 2 * p.calm_windows && when[2] == 3 * p.calm_windows,
            "release: one rung per calm_windows windows");
    }
    // hysteresis: for any steady cost the loop settles and then never changes (no flicker), going up never re-triggers going down
    {
        int worstReversals = 0, worstLate = 0;
        for (int native = 60; native <= 144; native += 84) {
            app::DisplayLadder l = app::make_display_ladder(native);
            for (double cost = 50; cost <= 30000; cost *= 1.07) {
                for (double statsShare = 0; statsShare <= 1; statsShare += 0.5) {
                    std::vector<int> t = run_tiers(l, native, 120, cost * (1 - statsShare) + 1, cost * statsShare * 2);
                    int reversals = 0, late = 0;
                    for (size_t i = 2; i < t.size(); ++i)
                        if ((t[i] - t[i - 1]) * (t[i - 1] - t[i - 2]) < 0) ++reversals;
                    for (size_t i = 60; i < t.size(); ++i) if (t[i] != t[i - 1]) ++late;
                    worstReversals = std::max(worstReversals, reversals);
                    worstLate = std::max(worstLate, late);
                }
            }
        }
        eq_i(sec, "no flicker: direction reversals over all steady costs", 0, worstReversals);
        eq_i(sec, "no flicker: changes after settling (windows 60-120)", 0, worstLate);
    }
    // playback behind: backs off one rung per window, to the bottom and no further; stepping up needs calm afterwards
    {
        std::vector<int> t = run_tiers(l60, 60, 40, 100, 100, 0, 5, 15);   // light cost, behind during windows 5..14
        eq_i(sec, "behind: tier before", 0, t[4]);
        eq_i(sec, "behind: one rung per window", 1, t[5]);
        eq_i(sec, "behind: second window", 2, t[6]);
        eq_i(sec, "behind: bottom rung and no further", l60.count - 1, t[14]);
        eq_i(sec, "behind over: not up at once", l60.count - 1, t[15]);
        eq_i(sec, "behind over: back at full rate later", 0, t.back());
        // behind wins over a good cost: never steps up while behind
        std::vector<int> u = run_tiers(l60, 60, 30, 100, 100, 3, 0, 30);
        eq_i(sec, "behind: held at the bottom, never up", l60.count - 1, u.back());
    }
}

int main(int argc, char** argv)
{
    g_dir = argc > 1 ? argv[1] : "v2/harness/expected/ui";
    std::printf("midibn-ui-tests, tables in %s\n", g_dir.c_str());
    test_format_n();
    for (int i = 0; i < 2; ++i) test_display_file(i == 0 ? "display-en-US.txt" : "display-de-DE.txt");
    test_sliders();
    test_scrubs();
    test_log_scrub();
    test_timeline();
    test_scale_and_fonts();
    test_scrub_parse();
    test_scrub_scripts();
    test_speed_windows();
    test_statistics_state();
    test_help();
    test_display_tiers();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("UI TESTS: PASS\n");
    else std::printf("UI TESTS: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
