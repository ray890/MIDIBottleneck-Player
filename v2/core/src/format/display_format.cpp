#include "midibn/format/display_format.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace midibn {
namespace format {

const char* const kEmDash = "\xE2\x80\x94";
const char* const kEnDash = "\xE2\x80\x93";
const char* const kBullet = "\xE2\x80\xA2";
const char* const kMicro = "\xC2\xB5";
const char* const kCompactSeparator = "\xE2\x80\x89/\xE2\x80\x89";
const char* const kEllipsis = "\xE2\x80\xA6";

namespace {

const char* const kNewLine = "\r\n";

// "{0:00}"-style custom numeric format for an int: at least `width` digits.
std::string pad_int(int32_t value, int width)
{
    bool negative = value < 0;
    uint64_t magnitude = negative ? (~static_cast<uint64_t>(static_cast<int64_t>(value)) + 1) : static_cast<uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    while (static_cast<int>(digits.size()) < width) digits.insert(digits.begin(), '0');
    return negative ? "-" + digits : digits;
}

std::string separator(bool compact) { return compact ? kCompactSeparator : " / "; }

int64_t saturating_count_add(int64_t total, int64_t value)
{
    if (value < 0) value = 0;
    return total > std::numeric_limits<int64_t>::max() - value ? std::numeric_limits<int64_t>::max() : total + value;
}

bool is_finite_value(const std::optional<double>& value)
{
    return value.has_value() && !std::isnan(*value) && !std::isinf(*value);
}

} // namespace

std::string format_time(int64_t microseconds)
{
    if (microseconds < 0) microseconds = 0;
    // TimeSpan.FromTicks(microseconds * 10): the product wraps like unchecked C#.
    int64_t ticks = static_cast<int64_t>(static_cast<uint64_t>(microseconds) * 10u);
    int32_t totalMinutes = double_to_int32(static_cast<double>(ticks) / 600000000.0);
    int32_t seconds = static_cast<int32_t>((ticks / 10000000) % 60);
    int32_t milliseconds = static_cast<int32_t>((ticks / 10000) % 1000);
    return pad_int(totalMinutes, 2) + ":" + pad_int(seconds, 2) + "." + pad_int(milliseconds, 3);
}

std::string format_duration(int64_t microseconds, const NumberCulture& c)
{
    if (microseconds < 1000) return format_n0(microseconds, c) + " " + kMicro + "s";
    return format_n(static_cast<double>(microseconds) / 1000.0, 3, c) + " ms";
}

std::string format_lag_milliseconds(int64_t microseconds, const NumberCulture& c)
{
    if (microseconds < 0) microseconds = 0;
    return format_n(static_cast<double>(microseconds) / 1000.0, 3, c) + " ms";
}

int64_t total_dropped_events(const engine::PlaybackSnapshot& s)
{
    int64_t total = std::max<int64_t>(0, s.dropped_events);
    total = saturating_count_add(total, s.gate_filtered_events);
    total = saturating_count_add(total, s.musical_limit_rejected_attacks);
    total = saturating_count_add(total, s.musical_limit_suppressed_releases);
    total = saturating_count_add(total, s.output_rate_rejected_attacks);
    total = saturating_count_add(total, s.output_rate_suppressed_releases);
    return saturating_count_add(total, s.output_rate_rejected_other_messages);
}

std::string format_event_counts(const engine::PlaybackSnapshot& snapshot, bool compact, const NumberCulture& c)
{
    return format_n0(snapshot.processed_events, c) + separator(compact) + format_n0(total_dropped_events(snapshot), c);
}

std::string format_observed_maximum_rate(const std::optional<double>& rate, bool compact, const NumberCulture& c)
{
    if (!is_finite_value(rate)) return kEmDash;
    return format_n(*rate, compact ? 0 : 1, c) + (compact ? " events/s" : " events/sec");
}

std::string format_per_note_frame_rate(int64_t intervalMicroseconds, bool compact, const NumberCulture& c)
{
    if (intervalMicroseconds <= 0) return compact ? "source-time frames" : "Source-time boundaries";
    return format_n(1000000.0 / static_cast<double>(std::max<int64_t>(1, intervalMicroseconds)), compact ? 0 : 1, c) +
        (compact ? " frames/s" : " frames/sec");
}

std::string format_delayed_note_off_duration(int64_t microseconds, bool compact, const NumberCulture& c)
{
    if (microseconds <= 0) return compact ? "source timing" : "Source NoteOff timing";
    return format_n0(microseconds, c) + " " + kMicro + (compact ? "s min" : "s minimum NoteOn");
}

std::string format_note_start_limit(int64_t eventsPerSecond, bool compact, const NumberCulture& c)
{
    return eventsPerSecond == 0 ? std::string("unlimited note starts") :
        format_n0(eventsPerSecond, c) + (compact ? " starts/s" : " note starts/sec");
}

std::string format_output_rate_limit(int64_t eventsPerSecond, int64_t softExcess, bool compact, const NumberCulture& c)
{
    std::string text = eventsPerSecond == 0 ? std::string("unlimited output") :
        format_n0(eventsPerSecond, c) + (compact ? " events/s max" : " maximum events/sec");
    if (softExcess == 0) return text;
    if (compact) return text + " (+" + format_n0(softExcess, c) + " required over target)";
    return text + " (" + format_n0(softExcess, c) + " required messages sent over target)";
}

std::string format_maximum_rate(const engine::PlaybackSnapshot& snapshot, const std::optional<double>& observed,
    bool compact, const NumberCulture& c)
{
    using engine::ProcessingMode;
    using models::ServiceDurationMode;
    if (snapshot.processing_mode == ProcessingMode::PerNoteIntervalGate)
        return format_per_note_frame_rate(snapshot.processing_microseconds, compact, c);
    if (snapshot.processing_mode == ProcessingMode::DelayedNoteOff)
        return format_delayed_note_off_duration(snapshot.processing_microseconds, compact, c);
    if (snapshot.processing_mode == ProcessingMode::NoteAttackLimit)
        return format_note_start_limit(snapshot.events_per_second, compact, c);
    if (snapshot.processing_mode == ProcessingMode::OutputRateLimit)
        return format_output_rate_limit(snapshot.events_per_second, snapshot.output_rate_safety_soft_excess, compact, c);
    bool immediate = (!snapshot.simulate_slowdown && !snapshot.virtual_queue_active) ||
        (snapshot.service_duration_mode == ServiceDurationMode::ProcessingTime && snapshot.processing_microseconds == 0) ||
        (snapshot.service_duration_mode == ServiceDurationMode::EventsPerSecond && snapshot.events_per_second == 0);
    if (immediate) return format_observed_maximum_rate(observed, compact, c);
    if (snapshot.service_duration_mode == ServiceDurationMode::MidiBitrate)
        return format_n0(snapshot.midi_bitrate, c) + " bit/s";
    if (snapshot.service_duration_mode == ServiceDurationMode::EventsPerSecond)
        return format_n0(snapshot.events_per_second, c) + (compact ? " events/s" : " events/sec");
    return format_n(1000000.0 / static_cast<double>(std::max<int64_t>(1, snapshot.processing_microseconds)),
        compact ? 0 : 1, c) + (compact ? " events/s" : " events/sec");
}

std::string format_configured_rate(const ConfiguredRateInputs& in, const engine::PlaybackSnapshot& snapshot,
    const std::optional<double>& observedMaximum, bool compact, const NumberCulture& c)
{
    if (in.per_note_interval_gate_enabled)
        return format_per_note_frame_rate(in.engine_processing_microseconds, compact, c);
    if (in.delayed_note_off_enabled)
        return format_delayed_note_off_duration(in.engine_processing_microseconds, compact, c);
    if (in.note_attack_limit_enabled) return format_note_start_limit(in.engine_events_per_second, compact, c);
    if (in.output_rate_limit_enabled)
        return format_output_rate_limit(in.engine_events_per_second, snapshot.output_rate_safety_soft_excess, compact, c);
    return format_maximum_rate(snapshot, observedMaximum, compact, c);
}

std::string format_timeline_output(int64_t intendedMicroseconds, int64_t lastDispatchedMicroseconds, bool compact)
{
    return format_time(intendedMicroseconds) + separator(compact) + format_time(lastDispatchedMicroseconds);
}

std::string format_queue_now_maximum(const engine::PlaybackSnapshot& snapshot, bool compact, const NumberCulture& c)
{
    return format_n0(snapshot.outstanding_events, c) + separator(compact) + format_n0(snapshot.maximum_queue_length, c) +
        (snapshot.virtual_queue_active ? (compact ? " actual" : " actual backlog") : "");
}

std::string format_output_rate_cell(const std::optional<double>& outputRate, bool compact, const NumberCulture& c)
{
    if (compact && outputRate.has_value()) return format_n(*outputRate, 0, c) + " events/s";
    return format_output_rate(outputRate, c);
}

std::string format_analysis_queue(const engine::PlaybackSnapshot& snapshot, const NumberCulture& c)
{
    return format_n0(snapshot.outstanding_events, c) + " / " + format_n0(snapshot.maximum_queue_length, c);
}

std::string format_analysis_lag(const engine::PlaybackSnapshot& snapshot, const NumberCulture& c)
{
    return format_lag_milliseconds(snapshot.current_lag_microseconds, c) + " / " +
        format_lag_milliseconds(snapshot.maximum_lag_microseconds, c);
}

StatisticsText format_statistics(const engine::PlaybackSnapshot& snapshot, const ConfiguredRateInputs& inputs,
    const std::optional<double>& observedMaximum, const std::optional<double>& outputRate,
    const std::optional<double>& effectiveSpeed, bool compact, const NumberCulture& c)
{
    StatisticsText text;
    text.timeline_output = format_timeline_output(snapshot.intended_timeline_microseconds,
        snapshot.last_dispatched_timeline_microseconds, compact);
    text.queue = format_queue_now_maximum(snapshot, compact, c);
    text.maximum_rate = format_configured_rate(inputs, snapshot, observedMaximum, compact, c);
    text.output_rate = format_output_rate_cell(outputRate, compact, c);
    text.events = format_event_counts(snapshot, compact, c);
    text.effective_speed = format_effective_speed(effectiveSpeed, c);
    text.maximum_lag = format_lag_milliseconds(snapshot.maximum_lag_microseconds, c);
    text.current_lag = format_lag_milliseconds(snapshot.current_lag_microseconds, c);
    return text;
}

std::string format_file_information(const FileSummary& summary, bool compact, const NumberCulture& c)
{
    std::string first = format_n0(summary.event_count, c) + " events  " + kBullet + "  " +
        format_integer(summary.track_count) + " tracks";
    std::string second = format_n0(summary.note_count, c) + " notes  " + kBullet + "  " + format_time(summary.duration_us);
    if (summary.running_status_repairs > 0)
        second += std::string("  ") + kBullet + "  Loaded with " + format_n0(summary.running_status_repairs, c) +
            " nonstandard running-status repair" + (summary.running_status_repairs == 1 ? "" : "s");
    return compact ? first + kNewLine + second : first + "  " + kBullet + "  " + second;
}

std::string format_running_status_repair_message(int64_t repairs, const NumberCulture& c)
{
    return "This file was loaded with " + format_n0(repairs, c) + " nonstandard running-status repair" +
        (repairs == 1 ? "" : "s") +
        ". The repair was limited to an unambiguous channel message immediately after MIDI metadata.";
}

int64_t compute_loading_elapsed_seconds(int64_t started, int64_t pauseStarted, int64_t pausedTicks, int64_t now,
    int64_t frequency)
{
    if (started == 0 || frequency <= 0) return 0;
    int64_t effectiveNow = pauseStarted == 0 ? now : pauseStarted;
    uint64_t difference = static_cast<uint64_t>(effectiveNow) - static_cast<uint64_t>(started) -
        static_cast<uint64_t>(std::max<int64_t>(0, pausedTicks));
    int64_t elapsedTicks = std::max<int64_t>(0, static_cast<int64_t>(difference));
    return std::max<int64_t>(0, double_to_int64(static_cast<double>(elapsedTicks) / static_cast<double>(frequency)));
}

std::string format_loading_telemetry(int64_t elapsedSeconds, int64_t privateBytes, bool compact, const NumberCulture& c)
{
    elapsedSeconds = std::max<int64_t>(0, elapsedSeconds);
    privateBytes = std::max<int64_t>(0, privateBytes);
    std::string time;
    if (elapsedSeconds >= 3600) {
        time = std::to_string(elapsedSeconds / 3600) + ":" + pad_int(static_cast<int32_t>((elapsedSeconds / 60) % 60), 2) +
            ":" + pad_int(static_cast<int32_t>(elapsedSeconds % 60), 2);
    } else {
        time = std::to_string(elapsedSeconds / 60) + ":" + pad_int(static_cast<int32_t>(elapsedSeconds % 60), 2);
    }
    double mebibytes = static_cast<double>(privateBytes) / 1048576.0;
    std::string memory = mebibytes >= 1024 ?
        format_n(mebibytes / 1024.0, mebibytes >= 10240 ? 1 : 2, c) + " GiB" :
        format_n(mebibytes, 0, c) + " MiB";
    std::string dot = std::string("  ") + kBullet + "  ";
    return compact ? time + dot + memory : time + " elapsed" + dot + memory + " committed";
}

std::string format_loading_file_label(const std::string& fileName, const std::string& telemetry)
{
    return "Loading " + fileName + kEllipsis + kNewLine + telemetry;
}

std::string format_load_status(const std::string& stage, int overallPermille, int stagePermille, const NumberCulture& c)
{
    return stage + kNewLine + format_n(overallPermille / 10.0, 1, c) + "% overall  " + kBullet + "  " +
        format_n(stagePermille / 10.0, 1, c) + "% stage";
}

std::string format_load_tooltip(const std::string& stage)
{
    return stage + kNewLine +
        "The upper bar shows overall parser work; the lower bar shows the current stage. Click Cancel to stop loading.";
}

int64_t stopwatch_ticks_to_microseconds(int64_t ticks, int64_t frequency)
{
    if (frequency <= 0 || ticks <= 0) return 0;
    uint64_t t = static_cast<uint64_t>(ticks);
    uint64_t f = static_cast<uint64_t>(frequency);
    // floor(t * 1000000 / f) without a 128-bit product: whole seconds plus the remainder's share.
    uint64_t whole = t / f;
    uint64_t rest = t % f;
    uint64_t restMicro = rest <= std::numeric_limits<uint64_t>::max() / 1000000u ? rest * 1000000u / f : 0;
    return static_cast<int64_t>(whole * 1000000u + restMicro);
}

std::string format_bytes(int64_t bytes, const NumberCulture& c)
{
    int64_t clamped = std::max<int64_t>(0, bytes);
    double gib = static_cast<double>(clamped) / 1073741824.0;
    if (gib >= 1) return format_n(gib, gib >= 10 ? 1 : 2, c) + " GiB";
    return format_n(static_cast<double>(clamped) / 1048576.0, 0, c) + " MiB";
}

std::string create_large_midi_message(const std::string& fileName, int64_t dispatchableEvents,
    const MemoryProjection& p, const NumberCulture& c)
{
    std::string risk = p.architecture_risk ?
        std::string(kNewLine) + kNewLine +
            "32-bit warning: this MIDI may be too large for the x86 version. Use the x64 version if possible." :
        std::string();
    return "File: " + fileName + kNewLine +
        "MIDI events to process: " + format_n0(dispatchableEvents, c) + kNewLine + kNewLine +
        "Estimated memory while the MIDI is open: " + format_bytes(p.retained, c) + kNewLine +
        "Estimated highest memory use while loading: " + format_bytes(p.peak_low, c) + kEnDash +
            format_bytes(p.peak_high, c) + kNewLine +
        "Estimated memory if Track routing is applied: " + format_bytes(p.retained_with_routing, c) + kNewLine +
        "Estimated highest memory use while planning/applying a route: " + format_bytes(p.routing_peak_total, c) +
            kNewLine + kNewLine +
        "These are estimates. Loading may still fail if enough memory is not available." + risk;
}

std::string format_effective_speed(const std::optional<double>& percent, const NumberCulture& c)
{
    if (!is_finite_value(percent)) return kEmDash;
    return format_n(*percent, 1, c) + "%";
}

std::string describe_speed_window(int64_t microseconds, const NumberCulture& c)
{
    if (microseconds == 0) return "Instantaneous";
    if (microseconds >= 1000000) return format_trimmed(static_cast<double>(microseconds) / 1000000.0, 3, c) + " s";
    return format_trimmed(static_cast<double>(microseconds) / 1000.0, 3, c) + " ms";
}

std::string format_output_rate(const std::optional<double>& eventsPerSecond, const NumberCulture& c)
{
    if (!is_finite_value(eventsPerSecond)) return kEmDash;
    return format_n(*eventsPerSecond, 1, c) + " events/sec";
}

std::string format_compact_pressure_number(int64_t value, const NumberCulture& c)
{
    double magnitude = std::fabs(static_cast<double>(value));
    if (magnitude >= 1000000000.0) return format_trimmed(static_cast<double>(value) / 1000000000.0, 1, c) + "B";
    if (magnitude >= 1000000.0) return format_trimmed(static_cast<double>(value) / 1000000.0, 1, c) + "M";
    if (magnitude >= 1000.0) return format_trimmed(static_cast<double>(value) / 1000.0, 1, c) + "K";
    return format_n0(value, c);
}

double queue_pressure_ratio(int64_t occupied, int64_t limit)
{
    return static_cast<double>(occupied) / static_cast<double>(std::max<int64_t>(1, limit));
}

std::string format_queue_pressure_text(bool virtualQueue, bool agePressure, int64_t occupied, int64_t limit,
    bool compact, const NumberCulture& c)
{
    // StatisticsView.SetQueuePressure stores max(0, occupied) and max(1, limit).
    occupied = std::max<int64_t>(0, occupied);
    limit = std::max<int64_t>(1, limit);
    std::string prefix = virtualQueue ? "Virtual " : "";
    std::string unit = agePressure ? std::string(" ") + kMicro + "s" : std::string();
    std::string occupiedText = compact ? format_compact_pressure_number(occupied, c) : format_n0(occupied, c);
    std::string limitText = compact ? format_compact_pressure_number(limit, c) : format_n0(limit, c);
    std::string percent = format_n(round_half_even_double(100 * queue_pressure_ratio(occupied, limit)), 0, c);
    if (compact && agePressure)
        return prefix + occupiedText + " / " + limitText + unit + " " + kEmDash + " " + percent + "%";
    return prefix + occupiedText + unit + " / " + limitText + unit + " " + kEmDash + " " + percent + "%";
}

std::string queue_pressure_tooltip(bool virtualQueue, bool agePressure, int64_t occupied, int64_t limit,
    const NumberCulture& c)
{
    const char* explanation = virtualQueue ?
        "Virtual pressure uses the selected rate model to decide whether a newly arriving MIDI event fits. Accepted MIDI is sent immediately. This is separate from the actual unsent scheduler/output backlog shown above, and it does not predict delay inside a driver or synthesizer."
        : agePressure ?
        "Waiting-time pressure is the age of the oldest pending event divided by the configured microsecond limit. The event currently in service is not removable. Drop oldest complete note is a soft limit when protected traffic remains at the head."
        :
        "Finite-buffer occupancy includes the event currently in service. Safety-preserving overflow policies may temporarily exceed the configured soft limit so a required Note Off or non-note message is not discarded.";
    return format_queue_pressure_text(virtualQueue, agePressure, occupied, limit, false, c) + kNewLine + explanation +
        kNewLine + "Purple means protected pending work has exceeded the configured soft limit.";
}

PressureColor choose_pressure_color(double ratio, bool overflowPulse)
{
    if (ratio > 1.0) return PressureColor::OverLimit;
    if (overflowPulse || ratio >= 0.9) return PressureColor::High;
    return ratio >= 0.65 ? PressureColor::Medium : PressureColor::Low;
}

Rgb pressure_color_rgb(PressureColor color)
{
    switch (color) {
    case PressureColor::Medium: return kPressureMediumRgb;
    case PressureColor::High: return kPressureHighRgb;
    case PressureColor::OverLimit: return kPressureOverLimitRgb;
    default: return kPressureLowRgb;
    }
}

} // namespace format
} // namespace midibn
