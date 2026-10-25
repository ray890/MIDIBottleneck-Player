#include "midibn/app/analysis_summary.h"

#include "midibn/app/analysis_format.h"
#include "midibn/format/display_format.h"

#include <algorithm>

namespace midibn {
namespace app {

using analysis::AnalysisConfiguration;
using analysis::ProjectionState;
using analysis::WorkloadAnalysis;
using format::format_n;
using format::format_n0;
using models::ServiceDurationMode;

const char* const kInspectionPlaceholder = "Hover over the graph to inspect a time. Click to pin; double-click to seek.";
const char* const kAnalysisPendingSummary = "Analysis will appear here when calculation completes.";
const char* const kAnalysisCalculatingMessage = "Calculating workload analysis\xE2\x80\xA6";
const char* const kAnalysisCancelledMessage = "Analysis calculation cancelled.";
const char* const kAnalysisNoFileMessage = "No MIDI file is attached.";
const char* const kAnalysisLoadingMessage = "Loading new MIDI\xE2\x80\xA6";

namespace {

const char* const kNl = "\r\n";

struct Text {
    std::string value;
    void line(const std::string& s) { value += s; value += kNl; }
    void blank() { value += kNl; }
};

void append_cluster(Text& text, const char* threshold, int64_t count, int64_t total, const format::NumberCulture& c)
{
    double percent = total == 0 ? 0 : 100.0 * static_cast<double>(count) / static_cast<double>(total);
    text.line(pad_right_utf16(threshold, 8) + pad_left_utf16(format_n0(count, c), 12) + "  (" + format_n(percent, 2, c) + "%)");
}

} // namespace

std::string build_analysis_summary(const AnalysisSongInfo& song, const AnalysisResultView& view,
    const format::NumberCulture& c)
{
    const WorkloadAnalysis& analysis = view.workload();
    const AnalysisConfiguration* cfg = view.configuration.get();
    const std::string micro = std::string(format::kMicro);
    Text text;
    text.line("MIDI FILE");
    text.line("Filename              " + song.file_name);
    text.line("SMF format            " + format::format_integer(song.smf_format));
    text.line("Tracks                " + format_n0(song.track_count, c));
    text.line("PPQN                  " + format_n0(song.ticks_per_quarter_note, c));
    text.line("Duration              " + analysis_format_clock(song.duration_us));
    text.line("Musical note-ons      " + format_n0(song.note_count, c));
    text.line("Dispatchable events   " + format_n0(analysis.total_events, c));
    text.line("File size             " + analysis_format_bytes(song.file_size_bytes, c));
    text.blank();
    text.line("WORKLOAD");
    text.line(std::string("Scope                 Whole file, ") + analysis_format_clock(0) + " " + format::kEnDash + " " +
        analysis_format_clock(analysis.duration_us));
    text.line("Graph resolution      " + analysis_format_resolution(analysis.bucket_us, c));
    text.line("MIDI message bytes    " + format_n0(analysis.total_bytes, c));
    text.line("Unique timestamps     " + format_n0(analysis.unique_timestamps, c));
    text.line("Average events/sec    " + format_n(analysis.average_events_per_second, 1, c));
    text.line("Peak events/sec       " + format_n(analysis.peak_events_per_second, 1, c));
    text.line("Peak bytes/sec        " + format_n(analysis.peak_bytes_per_second, 1, c));
    if (cfg != nullptr && cfg->override_projection != nullptr) {
        text.blank();
        text.line("FORCED OVERRIDE PROJECTION");
        text.line("Snapshot              This Analysis window only");
        text.line("Forced messages       " + format_n0(cfg->override_projection->forced_message_count(), c));
        if (view.state == ProjectionState::Complete) {
            text.line("Projected forced      " + format_n0(analysis.override_projected_forced_messages, c) +
                " generated MIDI messages");
            text.line("Filtered competing    " + format_n0(analysis.override_filtered_source_messages, c) +
                " source MIDI messages");
        } else {
            text.line("Status                Projection pending");
        }
        text.line("Order                 Track routing, then this forced-override snapshot");
        text.line("Playback              Unchanged by this Analysis setting");
    }
    if (!view.has_queue_projection() && !view.has_gate_projection()) {
        text.blank();
        bool gatePending = view.uses_gate();
        bool delayedPending = gatePending && cfg->delayed_note_off_enabled;
        bool attackPending = gatePending && cfg->note_attack_limit_enabled;
        bool outputPending = gatePending && cfg->output_rate_limit_enabled;
        text.line(gatePending ? (outputPending ? "OUTPUT RATE GATE PROJECTION"
                                   : attackPending ? "NOTE START RATE GATE PROJECTION"
                                   : delayedPending ? "DELAYED NOTEOFF PROJECTION" : "PER-NOTE PROJECTION")
                              : "QUEUE PROJECTION");
        if (view.state == ProjectionState::Pending) {
            text.line(std::string("Status                ") + (outputPending ? "Output rate gate"
                : attackPending ? "Note start rate gate"
                : delayedPending ? "Delayed NoteOff" : gatePending ? "Per-note gate" : "Queue") + " projection pending");
        } else if (view.state == ProjectionState::Cancelled) {
            text.line(std::string("Status                Workload only ") + format::kEmDash + " projection cancelled");
        } else {
            text.line(std::string("Status                Workload only ") + format::kEmDash + " " + (outputPending ? "Output rate gate"
                : attackPending ? "Note start rate gate"
                : delayedPending ? "Delayed NoteOff" : gatePending ? "Per-note" : "queue") + " projection failed");
            if (!view.failure_reason.empty()) text.line("Failure               " + view.failure_reason);
        }
    }
    if (view.has_gate_projection()) {
        text.blank();
        bool delayed = cfg->delayed_note_off_enabled;
        bool attackLimit = cfg->note_attack_limit_enabled;
        bool outputLimit = cfg->output_rate_limit_enabled;
        text.line(outputLimit ? "OUTPUT RATE GATE PROJECTION" : attackLimit ? "NOTE START RATE GATE PROJECTION"
            : delayed ? "DELAYED NOTEOFF PROJECTION" : "PER-NOTE PROJECTION");
        if (outputLimit)
            text.line("Maximum output rate   " + (cfg->events_per_second == 0 ? std::string("Unlimited")
                : format_n0(cfg->events_per_second, c) + " events/sec progressively paced"));
        else if (attackLimit)
            text.line("Maximum note starts   " + (cfg->events_per_second == 0 ? std::string("Unlimited")
                : format_n0(cfg->events_per_second, c) + " starts/sec progressively paced"));
        else
            text.line(std::string(delayed ? "Minimum NoteOn       " : "Gate interval         ") +
                format_n0(cfg->processing_us, c) + " " + micro + "s");
        if (delayed) {
            int64_t recovery = std::min<int64_t>(cfg->processing_us / 4, 40000);
            text.line("Recovery (derived)    " + format_n0(recovery, c) + " " + micro + "s");
        }
        text.line("Source events         " + format_n0(analysis.total_events, c));
        text.line("Projected output      " + format_n0(analysis.gate_output_events, c) + " MIDI messages");
        int64_t projectedGateDropped = outputLimit
            ? analysis.output_rate_rejected_attacks + analysis.output_rate_suppressed_releases + analysis.output_rate_rejected_other_messages
            : attackLimit ? analysis.musical_limit_rejected_attacks + analysis.musical_limit_suppressed_releases
            : analysis.gate_filtered_note_events;
        text.line("Projected dropped     " + format_n0(projectedGateDropped, c) +
            (outputLimit ? " source MIDI messages" : " source note messages"));
        text.line("Note transitions      " + format_n0(analysis.gate_output_note_transitions, c) +
            (delayed ? " (after inside-threshold coalescing)" : " (including required retrigger releases)"));
        text.line("Other MIDI messages   " + format_n0(analysis.gate_output_non_note_events, c));
        if (outputLimit) {
            text.line("Rejected attacks      " + format_n0(analysis.output_rate_rejected_attacks, c));
            text.line("Suppressed releases   " + format_n0(analysis.output_rate_suppressed_releases, c));
            text.line("Rejected other msgs   " + format_n0(analysis.output_rate_rejected_other_messages, c));
            text.line("Required over target  " + format_n0(analysis.output_rate_safety_soft_excess, c) +
                " required messages sent over target");
        } else if (attackLimit) {
            text.line("Rejected attacks      " + format_n0(analysis.musical_limit_rejected_attacks, c));
            text.line("Suppressed releases   " + format_n0(analysis.musical_limit_suppressed_releases, c));
        } else {
            text.line(std::string(delayed ? "Coalesced note msgs   " : "Gate-filtered notes   ") +
                format_n0(analysis.gate_filtered_note_events, c) + " source note messages");
        }
        text.line(std::string(delayed ? "Peak model output     " : "Peak gate output      ") +
            format_n(analysis.gate_peak_events_per_second, 1, c) + " messages/sec");
        text.line("Source end            " + analysis_format_clock(song.duration_us));
        text.line("Projected last send   " + (analysis.gate_output_events == 0 ? std::string("No MIDI messages")
            : analysis_format_clock(analysis.gate_output_completion_us)));
        text.line("Blue graph: source MIDI; amber graph: projected messages at gate emission times.");
        text.line(outputLimit
            ? "This is the deterministic ideal nonblocking result. Live decisions use actual completed dispatch timing and may differ after unpredictable provider blocking."
            : "This is a static source-file projection, not a queue-overflow or native-output prediction.");
        text.line(cfg->override_projection == nullptr
            ? "It excludes driver delays, synth tails, live channel mutes, and forced overrides."
            : "It includes this window's forced-override snapshot, but excludes driver delays, synth tails, and live channel mutes.");
    }
    if (view.has_queue_projection() && cfg != nullptr) {
        text.blank();
        text.line("PROCESSING MODEL");
        bool virtualForward = !cfg->simulate_slowdown && cfg->queue_length_limit_enabled && cfg->apply_queue_limit_without_slowdown;
        text.line(std::string("Simulate slowdown     ") + (cfg->simulate_slowdown ? "On" : "Off (accepted output is immediate)"));
        text.line(std::string("Output-paced timeline ") + (cfg->output_paced_timeline
            ? std::string("On ") + format::kEmDash + " source time resumes after deterministic simulated acceptance; native-provider blocking is not predicted"
            : std::string("Off ") + format::kEmDash + " source time follows the file independently of output service"));
        if (virtualForward)
            text.line(std::string("Forward queue limit   On ") + format::kEmDash +
                " modeled pressure can reject new MIDI, but never delays or retracts sent MIDI");
        if (cfg->service_duration_mode == ServiceDurationMode::MidiBitrate) {
            text.line("Rate model            MIDI serial bitrate at " + format_n0(cfg->midi_bitrate, c) + " bit/s");
            text.line("Maximum rate          " + format_n(analysis.byte_service_capacity_per_second, 1, c) + " bytes/sec");
        } else if (cfg->service_duration_mode == ServiceDurationMode::EventsPerSecond) {
            text.line("Rate model            Events per second, " + (cfg->events_per_second == 0 ? std::string("Unlimited / immediate")
                : format_n0(cfg->events_per_second, c) + " events/sec"));
            text.line("Maximum rate          " + (cfg->events_per_second == 0 ? std::string("Immediate (simulated)")
                : format_n0(cfg->events_per_second, c) + " events/sec"));
        } else {
            text.line("Rate model            Processing time, " + format_n0(cfg->processing_us, c) + " " + micro + "s/event");
            text.line("Maximum rate          " + ((!cfg->simulate_slowdown && !virtualForward) || cfg->processing_us == 0
                ? std::string("Immediate (simulated)")
                : format_n(analysis.event_service_capacity_per_second, 1, c) + " events/sec"));
        }
        text.blank();
        text.line("QUEUE PROJECTION");
        text.line(cfg->queue_age_limit_enabled
            ? "Queue waiting limit   " + format_n0(cfg->queue_age_limit_us, c) + " " + micro + "s"
            : "Queue length limit    " + (cfg->queue_length_limit_enabled ? format_n0(cfg->queue_length_limit, c) + " event slots" : std::string("Unlimited")));
        if (cfg->queue_length_limit_enabled) text.line("Overflow policy       " + analysis_format_overflow_policy(cfg->overflow_policy));
        text.line("Predicted peak        " + format_n0(analysis.predicted_maximum_occupancy, c) + " outstanding events");
        if (cfg->queue_age_limit_enabled)
            text.line("Peak waiting age      " + format_n0(analysis.predicted_peak_queue_age_us, c) + " " + micro + "s");
        text.line("Predicted drops       " + format_n0(analysis.predicted_dropped_events, c));
        if (analysis.predicted_buffer_clears > 0) text.line("Predicted clears      " + format_n0(analysis.predicted_buffer_clears, c));
        text.line("Source end            " + analysis_format_clock(song.duration_us));
        text.line("Predicted completion  " + analysis_format_clock(analysis.predicted_output_completion_us));
        int64_t predictedOverrun = std::max<int64_t>(0, analysis.predicted_output_completion_us - song.duration_us);
        if (predictedOverrun > 0) text.line("Predicted overrun     " + analysis_format_clock(predictedOverrun) + " after source end");
        text.line(virtualForward
            ? "Completion is the last accepted source event; virtual queue work does not delay accepted output."
            : "Completion is modeled accepted-event service, not synthesizer or audible-tail completion.");
        if (virtualForward) text.line("Virtual pressure does not predict delay inside a MIDI driver or synthesizer.");
        text.line("Projection is simulator output, not a hardware measurement.");
        text.line("Live channel mutes/overrides are not applied to this static source-file projection.");
    }
    text.blank();
    text.line("EVENT CLUSTERS");
    text.line("Largest simultaneous  " + format_n0(analysis.largest_timestamp_cluster, c));
    append_cluster(text, ">= 2", analysis.events_in_clusters_at_least_2, analysis.total_events, c);
    append_cluster(text, ">= 10", analysis.events_in_clusters_at_least_10, analysis.total_events, c);
    append_cluster(text, ">= 50", analysis.events_in_clusters_at_least_50, analysis.total_events, c);
    append_cluster(text, ">= 100", analysis.events_in_clusters_at_least_100, analysis.total_events, c);
    text.blank();
    text.line("MESSAGE TYPES");
    text.line("Type                     Events       Bytes");
    if (analysis.message_types != nullptr) {
        for (size_t i = 0; i < analysis.message_types->size(); ++i) {
            const analysis::MessageTypeWorkload& type = (*analysis.message_types)[i];
            text.value += pad_right_utf16(event_kind_name(type.kind), 24);
            text.value += pad_left_utf16(format_n0(type.event_count, c), 10);
            text.value += pad_left_utf16(format_n0(type.byte_count, c), 12);
            text.value += kNl;
        }
    }
    return text.value;
}

const std::vector<std::string>& analysis_summary_headings()
{
    static const std::vector<std::string> headings = { "MIDI FILE", "WORKLOAD", "FORCED OVERRIDE PROJECTION", "PROCESSING MODEL",
        "QUEUE PROJECTION", "PER-NOTE PROJECTION", "EVENT CLUSTERS", "MESSAGE TYPES" };
    return headings;
}

std::vector<SummaryHeadingSpan> find_summary_headings(const std::string& summary)
{
    // Only whole lines count (v1 formats the first occurrence anywhere, so a file named like a
    // heading is coloured instead of the heading; AD-006). Offsets are RichEdit positions: UTF-16
    // units with each "\r\n" counted once.
    std::vector<SummaryHeadingSpan> spans;
    const std::vector<std::string>& headings = analysis_summary_headings();
    std::vector<bool> found(headings.size(), false);
    size_t position = 0;
    size_t lineStart = 0;
    while (lineStart <= summary.size()) {
        size_t lineEnd = summary.find("\r\n", lineStart);
        size_t end = lineEnd == std::string::npos ? summary.size() : lineEnd;
        std::string line = summary.substr(lineStart, end - lineStart);
        for (size_t i = 0; i < headings.size(); ++i) {
            if (!found[i] && line == headings[i]) {
                found[i] = true;
                SummaryHeadingSpan span;
                span.start = position;
                span.length = utf16_length(line);
                spans.push_back(span);
            }
        }
        position += utf16_length(line);
        if (lineEnd == std::string::npos) break;
        position += 1;
        lineStart = lineEnd + 2;
    }
    return spans;
}

std::string build_inspection_text(const AnalysisResultView& view, bool showEventCounts, int64_t time, bool pinned,
    const format::NumberCulture& c)
{
    if (!view.valid()) return std::string();
    const WorkloadAnalysis& analysis = view.workload();
    const analysis::WorkloadBucket* bucket = analysis_bucket_at(view, time);
    int32_t bucketIndex = analysis_bucket_index_at(view, time);
    int64_t start = static_cast<int64_t>(bucketIndex) * analysis.bucket_us;
    int64_t end = std::min(analysis.duration_us, start + analysis.bucket_us);
    double seconds = static_cast<double>(analysis.bucket_us) / 1000000.0;
    const AnalysisConfiguration* cfg = view.configuration.get();
    bool gate = view.has_gate_projection();
    auto gateBucket = [&](int32_t index) -> int64_t {
        return index >= 0 && static_cast<size_t>(index) < analysis.gate_output_buckets.size()
            ? analysis.gate_output_buckets[static_cast<size_t>(index)] : 0;
    };
    Text text;
    text.line(std::string(pinned ? "Pinned: " : "Cursor: ") + analysis_format_clock(time));
    text.line(std::string(showEventCounts ? "Interval: " : "Bucket: ") + analysis_format_interval_boundary(start) +
        format::kEnDash + analysis_format_interval_boundary(end));
    if (bucket != nullptr) {
        if (showEventCounts) {
            text.value += std::string(gate ? "Source events in interval: " : "Events in interval: ") + format_n0(bucket->event_count, c);
            if (gate) text.value += "   Gate output in interval: " + format_n0(gateBucket(bucketIndex), c);
        } else {
            text.value += std::string(gate ? "Source events/sec: " : "Events/sec: ") +
                format_n(static_cast<double>(bucket->event_count) / seconds, 1, c);
            if (gate) text.value += "   Gate output/sec: " + format_n(static_cast<double>(gateBucket(bucketIndex)) / seconds, 1, c);
        }
        text.value += "   MIDI bytes/sec: " + format_n(static_cast<double>(bucket->byte_count) / seconds, 1, c) + kNl;
        text.value += "Largest simultaneous cluster: " + format_n0(bucket->largest_cluster, c);
        if (view.has_queue_projection() && cfg != nullptr && cfg->queue_length_limit_enabled) {
            if (cfg->queue_age_limit_enabled)
                text.value += "   Predicted queue age: " + format_n0(bucket->predicted_peak_queue_age_us, c) + " " + format::kMicro +
                    "s / " + format_n0(cfg->queue_age_limit_us, c) + " " + format::kMicro + "s";
            else
                text.value += "   Predicted occupancy: " + format_n0(bucket->predicted_peak_occupancy, c) + " / " +
                    format_n0(cfg->queue_length_limit, c);
            text.value += "   Predicted drops: " + format_n0(bucket->predicted_dropped_events, c);
            if (bucket->predicted_buffer_clears > 0) text.value += "   Predicted buffer clear";
        }
    }
    return text.value;
}

} // namespace app
} // namespace midibn
