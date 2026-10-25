// Text and arithmetic helpers of the Analysis window (v1 DiagnosticsForm.cs and
// WorkloadGraph.cs): sizes, resolutions, clock readouts, the timeline tick interval and
// the failure message. Pure functions; every number follows the NumberCulture exactly
// where v1 follows CultureInfo.CurrentCulture. Compared with v1's own output in
// v2/tests/analysis_app_tests.cpp (harness/expected/analysis-ui).
//
// All text is UTF-8; multi-line strings use "\r\n" like v1's Environment.NewLine.
#pragma once

#include "midibn/format/net_format.h"
#include "midibn/models/ordered_admission.h"

#include <cstdint>
#include <string>

namespace midibn {
namespace app {

// DiagnosticsForm.FormatBytes: "—" up to zero, "<n> bytes", "<n.n> KiB", "<n.nn> MiB".
std::string analysis_format_bytes(int64_t bytes, const format::NumberCulture& c = format::NumberCulture());
// DiagnosticsForm.FormatResolution / WorkloadGraph.FormatResolution: "<0.###> s" from one second, else "<0.###> ms".
std::string analysis_format_resolution(int64_t microseconds, const format::NumberCulture& c = format::NumberCulture());
// DiagnosticsForm.FormatOverflowPolicy. v1 has no case for "Drop background notes" and reports
// "Drop newest" for it (a v1 bug, UI difference AD-001); this function names the policy. The choices are named
// "Drop newest events" and "Drop oldest events" (DD-025; v1: "Drop newest", "Drop oldest pending event").
std::string analysis_format_overflow_policy(models::OverflowPolicy policy);
// DiagnosticsForm.FormatClock and WorkloadGraph.FormatClockDetailed: "mm:ss.fff" (negative shows as zero).
std::string analysis_format_clock(int64_t microseconds);
// WorkloadGraph.FormatClock: "mm:ss".
std::string analysis_format_clock_short(int64_t microseconds);
// FormatIntervalBoundary (both classes): "mm:ss.ffffff".
std::string analysis_format_interval_boundary(int64_t microseconds);
// WorkloadGraph.FormatAxisClock: "mm:ss" or "hh:mm:ss", with ".fff" when the interval is under one second.
std::string analysis_format_axis_clock(int64_t microseconds, int64_t intervalMicroseconds);
// WorkloadGraph.ChooseTimelineTickInterval: the "nice" tick spacing for a visible span and plot width.
int64_t choose_timeline_tick_interval(int64_t visibleSpanMicroseconds, int plotWidth);
// DiagnosticsForm.ConciseAnalysisFailure: one line, at most 240 UTF-16 units. `typeName` (the
// exception's type name) stands in for a blank message.
std::string concise_analysis_failure(const std::string& typeName, const std::string& message);
// System.IO.Path.GetFileName on the separators Windows uses ('\\', '/', ':').
std::string analysis_file_name(const std::string& path);

// WorkloadGraph.AbbreviatePlaybackCounts: numbers with group separators of 10,000 and more become 12.3k / 4.5M / 1B.
std::string abbreviate_playback_counts(const std::string& value, const format::NumberCulture& culture = format::NumberCulture());

// UTF-16 code units of a UTF-8 string (what String.Length and PadLeft count).
size_t utf16_length(const std::string& utf8);
// String.PadRight / PadLeft on UTF-16 length.
std::string pad_right_utf16(const std::string& text, size_t width);
std::string pad_left_utf16(const std::string& text, size_t width);

} // namespace app
} // namespace midibn
