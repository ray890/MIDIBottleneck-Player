// The Analysis window's report pane and inspection box (v1 DiagnosticsForm.BuildSummary,
// ApplySummary headings and UpdateInspection).
#pragma once

#include "midibn/app/analysis_view.h"
#include "midibn/format/net_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace app {

// The parts of v1's MidiSong the report reads.
struct AnalysisSongInfo {
    std::string file_name;        // the name without its folder
    int64_t smf_format = 1;
    int64_t track_count = 0;
    int64_t ticks_per_quarter_note = 0;
    int64_t duration_us = 0;
    int64_t note_count = 0;
    int64_t file_size_bytes = 0;
};

// DiagnosticsForm.BuildSummary. "\r\n" line ends, the same text v1 shows.
std::string build_analysis_summary(const AnalysisSongInfo& song, const AnalysisResultView& analysis,
    const format::NumberCulture& culture = format::NumberCulture());

// The section titles DiagnosticsForm.ApplySummary shows in bold blue (first occurrence of each).
const std::vector<std::string>& analysis_summary_headings();
struct SummaryHeadingSpan {
    size_t start;    // UTF-16 offset into the summary text (what a Windows edit control counts)
    size_t length;
};
// Where each heading sits in `summary` (UTF-16 offsets, in heading order; absent headings are skipped).
std::vector<SummaryHeadingSpan> find_summary_headings(const std::string& summary);

// DiagnosticsForm.UpdateInspection: the inspection box for a cursor (pinned == false) or a pin.
std::string build_inspection_text(const AnalysisResultView& analysis, bool showEventCounts, int64_t timeMicroseconds,
    bool pinned, const format::NumberCulture& culture = format::NumberCulture());

// The inspection box before anything is hovered.
extern const char* const kInspectionPlaceholder;
// Messages the report and the graph show while no analysis is available.
extern const char* const kAnalysisPendingSummary;      // "Analysis will appear here when calculation completes."
extern const char* const kAnalysisCalculatingMessage;  // "Calculating workload analysis…"
extern const char* const kAnalysisCancelledMessage;    // "Analysis calculation cancelled."
extern const char* const kAnalysisNoFileMessage;       // "No MIDI file is attached."
extern const char* const kAnalysisLoadingMessage;      // "Loading new MIDI…"

} // namespace app
} // namespace midibn
