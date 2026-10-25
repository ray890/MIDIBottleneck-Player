// The text v1's main window shows in its status area, ported as pure
// functions (MainForm.cs 3358-3374, 3485-3487, 3560-3582, 3886-3900,
// 4267-4345, 4491-4513; ProductDialogs.cs LargeMidiWarningDialog;
// EffectivePlaybackSpeed.cs; PlaybackTimelineView.FormatTime;
// StatisticsView.cs queue-pressure text; QueuePressurePalette.cs).
//
// v1 formats with CultureInfo.CurrentCulture; here the last parameter is the
// NumberCulture (default en-US). All text is UTF-8; multi-line strings use
// "\r\n" like v1's Environment.NewLine.
#pragma once

#include "midibn/engine/engine_types.h"
#include "midibn/format/net_format.h"
#include "midibn/midi_loader.h"

#include <cstdint>
#include <optional>
#include <string>

namespace midibn {
namespace format {

// ---- characters v1's text uses (UTF-8) ----------------------------------------
extern const char* const kEmDash;        // U+2014  "—"  (no value)
extern const char* const kEnDash;        // U+2013  "–"
extern const char* const kBullet;        // U+2022  "•"
extern const char* const kMicro;         // U+00B5  "µ"
extern const char* const kCompactSeparator;  // U+2009 "/" U+2009 (compact separator)
extern const char* const kEllipsis;      // U+2026  "…"

// MainForm.FormatTime / PlaybackTimelineView.FormatTime: "mm:ss.fff"
// (minutes are not limited to two digits; negative input shows as zero).
std::string format_time(int64_t microseconds);
// MainForm.FormatDuration: "<n> µs" below 1000, otherwise "<n.nnn> ms".
std::string format_duration(int64_t microseconds, const NumberCulture& c = NumberCulture());
// MainForm.FormatLagMilliseconds: "<n.nnn> ms" (negative shows as zero).
std::string format_lag_milliseconds(int64_t microseconds, const NumberCulture& c = NumberCulture());

// MainForm.TotalDroppedEvents (saturating sum of every drop counter).
int64_t total_dropped_events(const engine::PlaybackSnapshot& snapshot);
// MainForm.FormatEventCounts: "<sent> / <dropped>" (compact uses thin spaces).
std::string format_event_counts(const engine::PlaybackSnapshot& snapshot, bool compact,
    const NumberCulture& c = NumberCulture());

// MainForm.FormatObservedMaximumRate: "—" without a finite value.
std::string format_observed_maximum_rate(const std::optional<double>& rate, bool compact,
    const NumberCulture& c = NumberCulture());
// MainForm.FormatMaximumRate (the snapshot's own configuration).
std::string format_maximum_rate(const engine::PlaybackSnapshot& snapshot, const std::optional<double>& observed,
    bool compact, const NumberCulture& c = NumberCulture());
std::string format_per_note_frame_rate(int64_t intervalMicroseconds, bool compact,
    const NumberCulture& c = NumberCulture());
std::string format_delayed_note_off_duration(int64_t microseconds, bool compact,
    const NumberCulture& c = NumberCulture());
// The Note start rate gate text ("unlimited note starts" at 0).
std::string format_note_start_limit(int64_t eventsPerSecond, bool compact,
    const NumberCulture& c = NumberCulture());
// The Output rate gate text, with the required-over-target note when
// `softExcess` is not zero (shared by FormatMaximumRate and RefreshStatistics).
std::string format_output_rate_limit(int64_t eventsPerSecond, int64_t softExcess, bool compact,
    const NumberCulture& c = NumberCulture());

// The window's own selection of rate model, as RefreshStatistics' `configuredRate`
// expression reads it (the UI check boxes plus the engine's configured values).
struct ConfiguredRateInputs {
    bool per_note_interval_gate_enabled = false;
    bool delayed_note_off_enabled = false;
    bool note_attack_limit_enabled = false;
    bool output_rate_limit_enabled = false;
    int64_t engine_processing_microseconds = 0;   // _engine.ProcessingMicroseconds
    int64_t engine_events_per_second = 0;         // _engine.EventsPerSecond
};
// RefreshStatistics' `configuredRate` (the fourth gate falls back to FormatMaximumRate).
std::string format_configured_rate(const ConfiguredRateInputs& inputs, const engine::PlaybackSnapshot& snapshot,
    const std::optional<double>& observedMaximum, bool compact, const NumberCulture& c = NumberCulture());

// "Timeline / output": "<intended> / <dispatched>".
std::string format_timeline_output(int64_t intendedMicroseconds, int64_t lastDispatchedMicroseconds, bool compact);
// "Queue now / maximum": "<outstanding> / <maximum>" (+ " actual backlog"/" actual" when virtual).
std::string format_queue_now_maximum(const engine::PlaybackSnapshot& snapshot, bool compact,
    const NumberCulture& c = NumberCulture());
// The compact output-rate cell ("<n> events/s") or RollingOutputRate.Format.
std::string format_output_rate_cell(const std::optional<double>& outputRate, bool compact,
    const NumberCulture& c = NumberCulture());
// The Analysis overlay's queue text: "<outstanding> / <maximum>".
std::string format_analysis_queue(const engine::PlaybackSnapshot& snapshot, const NumberCulture& c = NumberCulture());
// The Analysis overlay's lag text: "<current> / <maximum>".
std::string format_analysis_lag(const engine::PlaybackSnapshot& snapshot, const NumberCulture& c = NumberCulture());

// The eight statistics cells in order: Timeline / output, Queue now / maximum,
// Maximum rate, Output rate, Events sent / dropped, Effective speed, Maximum
// lag, Current lag.
struct StatisticsText {
    std::string timeline_output;
    std::string queue;
    std::string maximum_rate;
    std::string output_rate;
    std::string events;
    std::string effective_speed;
    std::string maximum_lag;
    std::string current_lag;
};
StatisticsText format_statistics(const engine::PlaybackSnapshot& snapshot, const ConfiguredRateInputs& inputs,
    const std::optional<double>& observedMaximum, const std::optional<double>& outputRate,
    const std::optional<double>& effectiveSpeed, bool compact, const NumberCulture& c = NumberCulture());

// MainForm.FormatFileInformation inputs (the parts of MidiSong it reads).
struct FileSummary {
    int64_t event_count = 0;
    int64_t track_count = 0;
    int64_t note_count = 0;
    int64_t duration_us = 0;
    int64_t running_status_repairs = 0;
};
std::string format_file_information(const FileSummary& summary, bool compact, const NumberCulture& c = NumberCulture());
// The message box shown after a load with running-status repairs.
std::string format_running_status_repair_message(int64_t repairs, const NumberCulture& c = NumberCulture());

// MainForm.ComputeLoadingElapsedSeconds (Stopwatch ticks; `frequency` ticks per second).
int64_t compute_loading_elapsed_seconds(int64_t started, int64_t pauseStarted, int64_t pausedTicks, int64_t now,
    int64_t frequency);
// MainForm.FormatLoadingTelemetry: "m:ss elapsed  •  n MiB committed" (compact drops the words).
std::string format_loading_telemetry(int64_t elapsedSeconds, int64_t privateBytes, bool compact,
    const NumberCulture& c = NumberCulture());
// The file label while loading: "Loading <name>…\r\n<telemetry>".
std::string format_loading_file_label(const std::string& fileName, const std::string& telemetry);
// UpdateLoadStatus: "<stage>\r\n<overall>% overall  •  <stage>% stage" (permille inputs).
std::string format_load_status(const std::string& stage, int overallPermille, int stagePermille,
    const NumberCulture& c = NumberCulture());
// UpdateLoadStatus: the hover text for the loading panel.
std::string format_load_tooltip(const std::string& stage);
// MainForm.StopwatchTicksToMicroseconds: (long)(ticks * 1000000m / frequency), non-negative ticks.
int64_t stopwatch_ticks_to_microseconds(int64_t ticks, int64_t frequency);

// LargeMidiWarningDialog.FormatBytes: "<n.nn> GiB" at 1 GiB and above, else "<n> MiB".
std::string format_bytes(int64_t bytes, const NumberCulture& c = NumberCulture());
// LargeMidiWarningDialog.CreateMessage (`fileName` is the file's name without its folder).
std::string create_large_midi_message(const std::string& fileName, int64_t dispatchableEvents,
    const MemoryProjection& projection, const NumberCulture& c = NumberCulture());

// EffectivePlaybackSpeed.Format: "—" or "<n.n>%".
std::string format_effective_speed(const std::optional<double>& percent, const NumberCulture& c = NumberCulture());
// EffectivePlaybackSpeed.DescribeWindow: "Instantaneous", "<n> ms" or "<n> s".
std::string describe_speed_window(int64_t microseconds, const NumberCulture& c = NumberCulture());
// RollingOutputRate.Format: "—" or "<n.n> events/sec".
std::string format_output_rate(const std::optional<double>& eventsPerSecond, const NumberCulture& c = NumberCulture());

// StatisticsView queue-pressure bar text.
std::string format_compact_pressure_number(int64_t value, const NumberCulture& c = NumberCulture());
std::string format_queue_pressure_text(bool virtualQueue, bool agePressure, int64_t occupied, int64_t limit,
    bool compact, const NumberCulture& c = NumberCulture());
// The ratio the pressure bar fills to: occupied / max(1, limit).
double queue_pressure_ratio(int64_t occupied, int64_t limit);
// StatisticsView's hover text for the bar (the text, then an explanation, then the purple note).
std::string queue_pressure_tooltip(bool virtualQueue, bool agePressure, int64_t occupied, int64_t limit,
    const NumberCulture& c = NumberCulture());

// QueuePressurePalette.
struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};
constexpr Rgb kPressureLowRgb = { 65, 165, 90 };
constexpr Rgb kPressureMediumRgb = { 215, 155, 35 };
constexpr Rgb kPressureHighRgb = { 210, 70, 70 };
constexpr Rgb kPressureOverLimitRgb = { 155, 75, 190 };
constexpr Rgb kPressureBackgroundRgb = { 225, 228, 232 };
constexpr Rgb kPressureOutlineRgb = { 155, 160, 166 };

enum class PressureColor { Low, Medium, High, OverLimit };
// QueuePressurePalette.Choose.
PressureColor choose_pressure_color(double ratio, bool overflowPulse);
Rgb pressure_color_rgb(PressureColor color);

} // namespace format
} // namespace midibn
