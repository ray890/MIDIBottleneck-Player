// Replay of the Analysis and track-routing families (Milestone 4). Formats
// must match v2/harness/v1-reference/V1ModelHarness.Analysis.cs and
// V1ModelHarness.Routing.cs.
#include "replay_internal.h"

#include "midibn/analysis/workload_analysis.h"
#include "midibn/routing/route_plan_view.h"
#include "midibn/routing/track_routing.h"

#include <cstdio>
#include <cstring>

namespace midibn {
namespace models {

using analysis::AnalysisConfiguration;
using analysis::AnalysisControl;
using analysis::AnalysisSong;
using analysis::WorkloadAnalysis;
using analysis::WorkloadBucket;
using analysis::WorkloadCache;

struct AnalysisReplay {
    int64_t cache_serial = -1;
    std::unique_ptr<WorkloadCache> cache;
    // The last compiled route and the song it was compiled for (kept alive,
    // so a later song never shares its identity).
    std::shared_ptr<Song> route_song;
    std::shared_ptr<routing::CompiledTrackRoute> route;
    // The routed song of the last successful routed-view, with its own cache.
    std::unique_ptr<routing::TrackRoutedEventStore> routed;
    std::shared_ptr<Song> routed_source;
    int64_t routed_duration = 0;
    std::unique_ptr<WorkloadCache> routed_cache;
    std::vector<std::string> progress;
};

void AnalysisReplayDeleter::operator()(AnalysisReplay* p) const { delete p; }

namespace {

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t end = text.find(separator, start);
        if (end == std::string::npos) { parts.push_back(text.substr(start)); return parts; }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

struct Fnv {
    uint64_t hash = 14695981039346656037ULL;
    void add(const std::string& text)
    {
        for (char c : text) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
    }
    std::string text() const
    {
        char buffer[24];
        std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(hash));
        return buffer;
    }
};

std::string hx(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(bits));
    return buffer;
}

std::string hex16(uint64_t value)
{
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string u64(uint64_t value)
{
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%llu", static_cast<unsigned long long>(value));
    return buffer;
}

std::string esc(const std::u16string& text)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size());
    for (char16_t c : text) {
        if (c < 0x20 || c > 0x7E || c == u'\\') {
            result += "\\u";
            result += digits[(c >> 12) & 15];
            result += digits[(c >> 8) & 15];
            result += digits[(c >> 4) & 15];
            result += digits[c & 15];
        } else result += static_cast<char>(c);
    }
    return result;
}

std::string esc(const std::string& ascii) { return esc(std::u16string(ascii.begin(), ascii.end())); }

AnalysisReplay& state(ReplayContext& c)
{
    if (!c.analysis) c.analysis.reset(new AnalysisReplay());
    AnalysisReplay& a = *c.analysis;
    if (a.cache_serial != c.song_serial || !a.cache) {
        a.cache_serial = c.song_serial;
        a.cache.reset(new WorkloadCache());
    }
    return a;
}

// ------------------------------------------------------------ configuration

std::shared_ptr<const overrides::AnalysisOverrideSnapshot> parse_override(const std::string& text)
{
    if (text == "-") return nullptr;
    std::vector<int32_t> values(16 * overrides::kAttributeCount, overrides::kAutoValue);
    if (text != "none")
        for (const std::string& part : split(text, ';')) {
            size_t dot = part.find('.');
            size_t eq = part.find('=');
            int channel = static_cast<int>(to_i64(part.substr(0, dot)));
            int attribute = static_cast<int>(to_i64(part.substr(dot + 1, eq - dot - 1)));
            values[static_cast<size_t>(channel * overrides::kAttributeCount + attribute)] =
                static_cast<int32_t>(to_i64(part.substr(eq + 1)));
        }
    return std::make_shared<overrides::AnalysisOverrideSnapshot>(values);
}

std::shared_ptr<const AnalysisConfiguration> parse_configuration(const std::string& text)
{
    if (text == "null") return nullptr;
    auto c = std::make_shared<AnalysisConfiguration>();
    for (const std::string& part : split(text, ',')) {
        size_t eq = part.find('=');
        std::string key = part.substr(0, eq);
        std::string value = part.substr(eq + 1);
        size_t colon = value.find(':');
        std::string first = colon == std::string::npos ? value : value.substr(0, colon);
        int64_t second = colon == std::string::npos ? 0 : to_i64(value.substr(colon + 1));
        if (key == "sim") c->simulate_slowdown = value == "1";
        else if (key == "mode") c->service_duration_mode = static_cast<ServiceDurationMode>(to_i64(value));
        else if (key == "proc") c->processing_us = to_i64(value);
        else if (key == "bit") c->midi_bitrate = to_i64(value);
        else if (key == "eps") c->events_per_second = to_i64(value);
        else if (key == "ql") { c->queue_length_limit_enabled = first == "1"; c->queue_length_limit = static_cast<int32_t>(second); }
        else if (key == "qa") { c->queue_age_limit_enabled = first == "1"; c->queue_age_limit_us = second; }
        else if (key == "pol") c->overflow_policy = static_cast<OverflowPolicy>(to_i64(value));
        else if (key == "pn") c->per_note_interval_gate_enabled = value == "1";
        else if (key == "dn") c->delayed_note_off_enabled = value == "1";
        else if (key == "scale") c->scale_note_interval_by_traffic = value == "1";
        else if (key == "ip") { c->interval_polyphony_limit_enabled = first == "1"; c->interval_polyphony_limit = static_cast<int32_t>(second); }
        else if (key == "na") c->note_attack_limit_enabled = value == "1";
        else if (key == "or") c->output_rate_limit_enabled = value == "1";
        else if (key == "paced") c->output_paced_timeline = value == "1";
        else if (key == "aq") c->apply_queue_limit_without_slowdown = value == "1";
        else if (key == "ovr") c->override_projection = parse_override(value);
    }
    return c;
}

// ------------------------------------------------------------ analysis output

std::string bucket_text(const WorkloadBucket& b)
{
    std::string text;
    append_number(text, b.event_count); text += ',';
    append_number(text, b.byte_count); text += ',';
    append_number(text, b.largest_cluster); text += ',';
    append_number(text, b.service_demand_us); text += ',';
    append_number(text, b.predicted_peak_occupancy); text += ',';
    append_number(text, b.predicted_peak_queue_age_us); text += ',';
    append_number(text, b.predicted_dropped_events); text += ',';
    append_number(text, b.predicted_buffer_clears);
    return text;
}

std::string analysis_text(const WorkloadAnalysis& a, const std::string* ready)
{
    std::vector<std::string> lines;
    lines.push_back("state " + s(static_cast<int>(a.projection_state)) + " gate " + b(a.has_gate_projection()) + " queue " +
        b(a.has_queue_projection()) + " failure " +
        (a.projection_failure_reason.empty() ? std::string("-") : esc(a.projection_failure_reason)) +
        (ready == nullptr ? std::string() : " ready " + *ready));
    lines.push_back("totals events " + s(a.total_events) + " bytes " + s(a.total_bytes) + " unique " + s(a.unique_timestamps) +
        " largest " + s(a.largest_timestamp_cluster) + " c2 " + s(a.events_in_clusters_at_least_2) + " c10 " +
        s(a.events_in_clusters_at_least_10) + " c50 " + s(a.events_in_clusters_at_least_50) + " c100 " +
        s(a.events_in_clusters_at_least_100) + " bucket " + s(a.bucket_us) + " duration " + s(a.duration_us));
    lines.push_back("rates avg " + hx(a.average_events_per_second) + " peak-ev " + hx(a.peak_events_per_second) +
        " peak-bytes " + hx(a.peak_bytes_per_second) + " cap-ev " + hx(a.event_service_capacity_per_second) +
        " cap-bytes " + hx(a.byte_service_capacity_per_second) + " gate-peak " + hx(a.gate_peak_events_per_second));
    lines.push_back("predicted occ " + s(a.predicted_maximum_occupancy) + " age " + s(a.predicted_peak_queue_age_us) +
        " dropped " + s(a.predicted_dropped_events) + " clears " + s(a.predicted_buffer_clears) + " completion " +
        s(a.predicted_output_completion_us));
    lines.push_back("gate events " + s(a.gate_output_events) + " notes " + s(a.gate_output_note_transitions) + " other " +
        s(a.gate_output_non_note_events) + " filtered " + s(a.gate_filtered_note_events) + " musical " +
        s(a.musical_limit_rejected_attacks) + " " + s(a.musical_limit_suppressed_releases) + " outrate " +
        s(a.output_rate_rejected_attacks) + " " + s(a.output_rate_suppressed_releases) + " " +
        s(a.output_rate_rejected_other_messages) + " " + s(a.output_rate_safety_soft_excess) + " override " +
        s(a.override_projected_forced_messages) + " " + s(a.override_filtered_source_messages) + " completion " +
        s(a.gate_output_completion_us));
    std::string types = "types";
    if (!a.message_types) types += " null";
    else
        for (const analysis::MessageTypeWorkload& t : *a.message_types)
            types += " " + s(static_cast<int>(t.kind)) + ":" + s(t.event_count) + ":" + s(t.byte_count);
    lines.push_back(types);
    Fnv digest;
    std::string sample;
    int shown = 0;
    for (size_t i = 0; i < a.buckets.size(); ++i) {
        std::string text = bucket_text(a.buckets[i]);
        digest.add(text);
        digest.add(";");
        if (shown < 6 && text != "0,0,0,0,0,0,0,0") {
            sample += " " + s(static_cast<int64_t>(i)) + "=" + text;
            shown++;
        }
    }
    lines.push_back("buckets " + s(static_cast<int64_t>(a.buckets.size())) + " " + digest.text() + sample);
    if (!a.has_gate_output_buckets) lines.push_back("gate-buckets null");
    else {
        Fnv gate;
        std::string gateSample;
        int gateShown = 0;
        for (size_t i = 0; i < a.gate_output_buckets.size(); ++i) {
            std::string value;
            append_number(value, a.gate_output_buckets[i]);
            gate.add(value);
            gate.add(",");
            if (gateShown < 6 && a.gate_output_buckets[i] != 0) {
                gateSample += " " + s(static_cast<int64_t>(i)) + "=" + value;
                gateShown++;
            }
        }
        lines.push_back("gate-buckets " + s(static_cast<int64_t>(a.gate_output_buckets.size())) + " " + gate.text() + gateSample);
    }
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) joined += "\n= ";
        joined += lines[i];
    }
    return joined;
}

AnalysisControl recording_control(AnalysisReplay& a)
{
    a.progress.clear();
    AnalysisControl control;
    std::vector<std::string>* progress = &a.progress;
    control.progress = [progress](const analysis::AnalysisProgress& p) {
        progress->push_back(std::string(p.stage) + "|" + s(p.overall_permille) + "|" + s(p.stage_permille));
    };
    return control;
}

std::string progress_text(const AnalysisReplay& a)
{
    Fnv digest;
    std::string runs;
    std::string last;
    bool any = false;
    int64_t run = 0;
    for (const std::string& entry : a.progress) {
        digest.add(entry);
        digest.add(";");
        std::string stage = entry.substr(0, entry.find('|'));
        if (any && stage == last) { run++; continue; }
        if (any) runs += last + "*" + s(run) + "/";
        last = stage;
        any = true;
        run = 1;
    }
    if (any) runs += last + "*" + s(run);
    return "progress " + s(static_cast<int64_t>(a.progress.size())) + " " + digest.text() + " " + runs;
}

// ------------------------------------------------------------ routing output

routing::TrackRoutingSettings parse_settings(const std::string& text)
{
    routing::TrackRoutingSettings settings;
    for (const std::string& part : split(text, ',')) {
        size_t eq = part.find('=');
        std::string key = part.substr(0, eq);
        std::string value = part.substr(eq + 1);
        if (key == "tier") settings.maximum_automatic_tier = static_cast<routing::TrackRoutingTier>(to_i64(value));
        else if (key == "cap") settings.target_capacity = static_cast<int32_t>(to_i64(value));
        else if (key == "perc") settings.percussion_policy = static_cast<routing::PercussionRoutingPolicy>(to_i64(value));
        else if (key == "usable") {
            int mask = static_cast<int>(std::strtol(value.c_str(), nullptr, 16));
            for (int channel = 0; channel < 16; ++channel)
                settings.usable_destination_channels[static_cast<size_t>(channel)] = (mask & (1 << channel)) != 0;
        } else if (key == "manual" && value != "-") {
            for (const std::string& entry : split(value, ';')) {
                size_t dot = entry.find('.');
                size_t at = entry.find('=');
                routing::TrackLaneKey lane(static_cast<int32_t>(to_i64(entry.substr(0, dot))),
                    static_cast<int32_t>(to_i64(entry.substr(dot + 1, at - dot - 1))));
                settings.manual_destinations[lane] = static_cast<int32_t>(to_i64(entry.substr(at + 1)));
            }
        }
    }
    return settings;
}

std::string lane_tok(const routing::TrackLaneKey& lane) { return s(lane.track) + "." + s(lane.source_channel); }

std::string patch_tok(const routing::MidiPatchIdentity& p) { return s(p.bank_msb) + "/" + s(p.bank_lsb) + "/" + s(p.program); }

std::string route_text(const routing::CompiledTrackRoute& route)
{
    std::vector<std::string> lines;
    lines.push_back("valid " + b(route.is_valid()) + " explicit " + b(route.requires_explicit_apply) + " highest " +
        s(static_cast<int>(route.highest_tier)) + " lanes " + s(static_cast<int64_t>(route.lanes.size())) + " assignments " +
        s(static_cast<int64_t>(route.assignments.size())) + " destinations " +
        s(static_cast<int64_t>(route.destinations.size())) + " errors " + s(static_cast<int64_t>(route.errors.size())));
    for (const std::string& error : route.errors) lines.push_back("error " + esc(error));
    for (const auto& lanePtr : route.lanes) {
        const routing::TrackRouteLaneProfile& lane = *lanePtr;
        std::string timeline;
        for (size_t i = 0; i < lane.patch_timeline.size(); ++i) {
            if (i == 8) { timeline += " ..."; break; }
            timeline += " " + s(lane.patch_timeline[i].event_index) + ":" + patch_tok(lane.patch_timeline[i].patch);
        }
        Fnv timelineDigest;
        for (const routing::TrackRoutePatchChange& change : lane.patch_timeline)
            timelineDigest.add(s(change.event_index) + ":" + patch_tok(change.patch) + ";");
        Fnv intervals;
        for (const routing::TrackRouteSoundInterval& interval : lane.sounding_intervals)
            intervals.add(s(interval.start_event_index) + ":" + s(interval.end_event_index) + ":" + s(interval.start_us) + ":" +
                s(interval.end_us) + ":" + s(interval.prefix_maximum_end_us) + ":" + s(interval.key) + ";");
        lines.push_back("lane " + lane_tok(lane.lane) + " name " + esc(lane.track_name) + " inst " + esc(lane.instrument_name) +
            " display " + esc(lane.display_name()) + " ev " + s(lane.event_count) + " at " + s(lane.attack_count) +
            " first " + s(lane.first_attack_us) + " last " + s(lane.last_release_us) + " dyn " + b(lane.has_dynamic_state) +
            " sus " + b(lane.has_sustain) + " sys " + b(lane.track_contains_system_traffic) + " sig " +
            hex16(lane.dynamic_state_signature) + " rep " + patch_tok(lane.representative_patch) + " dom " +
            u64(lane.dominance_score) + " hist " + s(static_cast<int64_t>(lane.patch_history.size())) + " intervals " +
            s(static_cast<int64_t>(lane.sounding_intervals.size())) + " " + intervals.text() + " timeline " +
            s(static_cast<int64_t>(lane.patch_timeline.size())) + " " + timelineDigest.text() + timeline);
    }
    for (const auto& a : route.assignments) {
        std::string warnings;
        for (size_t i = 0; i < a->warnings.size(); ++i) {
            if (i != 0) warnings += "; ";
            warnings += a->warnings[i];
        }
        lines.push_back("assign " + lane_tok(a->profile->lane) + " dest " + s(a->destination_channel) + " tier " +
            s(static_cast<int>(a->tier)) + " flags " + b(a->manual) + b(a->representative_patch) + b(a->state_conflict) +
            b(a->same_key_overlap) + b(a->time_handoff) + b(a->percussion_exception) + b(a->retimbred) + b(a->omitted) +
            " | " + esc(warnings));
    }
    for (const routing::TrackRouteDestinationPreview& d : route.destinations) {
        std::string members;
        for (const auto& a : d.assignments) members += " " + lane_tok(a->profile->lane);
        lines.push_back("dest " + s(d.destination_channel) + " rep " + patch_tok(d.representative_patch) + " lane " +
            lane_tok(d.representative_lane) + " members" + members);
    }
    const routing::TrackRouteStatistics& st = route.statistics;
    lines.push_back("stats routed " + s(st.routed_lane_count) + " dests " + s(st.destination_count) + " merged " +
        s(st.merged_lane_count) + " retimbred " + s(st.retimbred_lane_count) + " omitted " + s(st.omitted_lane_count) +
        " omitted-starts " + s(st.omitted_note_start_count) + " generated " + s(st.generated_destination_state_count) +
        " included " + s(st.included_source_event_count) + " output " + s(st.routed_output_event_count));
    Fnv destinationDigest;
    int64_t histogram[18] = {};
    for (int32_t i = 0; i < route.source_event_count(); ++i) {
        int32_t d = route.event_destination(i);
        destinationDigest.add(s(d) + ",");
        histogram[d + 2]++;
    }
    std::string hist;
    for (int i = 0; i < 18; ++i)
        if (histogram[i] != 0) hist += " " + s(i - 2) + ":" + s(histogram[i]);
    lines.push_back("event-destinations " + s(route.source_event_count()) + " " + destinationDigest.text() + hist);
    Fnv generatedDigest;
    std::string generatedSample;
    int64_t total = 0;
    for (const routing::TrackRouteGeneratedPatch& group : route.generated_patches()) {
        std::string entry = s(group.source_event_index) + "<";
        for (int k = 0; k < group.event_count(); ++k) {
            append_event(entry, route.generated_event(group, k));
            entry += ' ';
        }
        entry += '>';
        generatedDigest.add(entry);
        if (total < 6) generatedSample += " " + entry;
        total += group.event_count();
    }
    lines.push_back("generated " + s(static_cast<int64_t>(route.generated_patches().size())) + " " + s(total) + " " +
        generatedDigest.text() + generatedSample);
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) joined += "\n= ";
        joined += lines[i];
    }
    return joined;
}

std::string apply_state_text(const char* name, const routing::CompiledTrackRoute& route, bool checked)
{
    routing::RouteApplyState st = routing::route_apply_state(&route, checked);
    return std::string(name) + " " + b(st.valid) + " " + b(st.lossy_confirmation_visible) + " " + b(st.apply_enabled) + " " +
        b(st.status_error) + " " + esc(st.status_text);
}

std::string form_text(const routing::CompiledTrackRoute& route, const routing::TrackRoutingSettings& settings)
{
    std::vector<std::string> lines;
    for (const routing::RouteLaneRow& row : routing::route_lane_rows(route))
        lines.push_back("row " + lane_tok(row.lane) + " | " + esc(row.lane_text) + " | " + esc(row.patch_text) + " | " +
            esc(row.counts_text) + " | " + esc(row.destination_text) + " | " + esc(row.status_text) + " | " +
            s(static_cast<int>(row.style)));
    std::u16string preview = routing::route_preview_text(route, settings.maximum_automatic_tier);
    size_t start = 0;
    while (true) {
        size_t end = preview.find(u"\r\n", start);
        if (end == std::u16string::npos) {
            lines.push_back("preview " + esc(preview.substr(start)));
            break;
        }
        lines.push_back("preview " + esc(preview.substr(start, end - start)));
        start = end + 2;
    }
    std::string used = "used ";
    for (bool channel : routing::route_used_channels(route)) used += b(channel);
    lines.push_back(used);
    lines.push_back(apply_state_text("apply", route, false));
    bool checked = false;
    if (route.requires_explicit_apply) {
        checked = true;
        lines.push_back(apply_state_text("confirm", route, true));
    }
    bool explicitlyConfirmed = false;
    bool published = routing::route_apply(&route, checked, explicitlyConfirmed);
    lines.push_back("applied " + b(published) + " " + b(explicitlyConfirmed) + " " + b(published));
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) joined += "\n= ";
        joined += lines[i];
    }
    return joined;
}

routing::RoutingSong routing_song(const std::shared_ptr<Song>& song)
{
    routing::RoutingSong r;
    r.events = EventSource(&song->store);
    r.track_count = song->track_count;
    r.duration_us = song->duration_us;
    r.tracks = &song->tracks;
    r.identity = song.get();
    return r;
}

} // namespace

bool replay_analysis_op(ReplayContext& c, const Args& a, OpOutput& out)
{
    const std::string& op = a[0];
    if (op == "bucket-count") {
        out.set(s(analysis::calculate_bucket_count(to_i64(a[1]), to_i64(a[2]))));
        return true;
    }
    if (op == "default-resolution") {
        out.set(s(analysis::default_resolution(to_i64(a[1]))));
        return true;
    }
    if (op == "cache-clear") {
        state(c).cache.reset(new WorkloadCache());
        return true;
    }
    if (op == "analyze" || op == "uncached" || op == "routed-analyze") {
        AnalysisReplay& st = state(c);
        AnalysisReplay* captured = &st;
        out.state = [captured] { return progress_text(*captured); };
        st.progress.clear();
        int64_t bucket = to_i64(a[1]);
        std::shared_ptr<const AnalysisConfiguration> configuration = parse_configuration(a[2]);
        AnalysisSong song;
        WorkloadCache* cache = st.cache.get();
        if (op == "routed-analyze") {
            song.events = EventSource(st.routed.get());
            song.duration_us = st.routed_duration;
            cache = st.routed_cache.get();
        } else {
            song.events = EventSource(&c.song->store);
            song.duration_us = c.song->duration_us;
        }
        AnalysisControl control = recording_control(st);
        if (op == "uncached") {
            WorkloadAnalysis result = analysis::analyze_uncached(song, bucket, configuration, control, cache);
            out.set(analysis_text(result, nullptr));
        } else {
            std::string ready;
            bool hasReady = false;
            WorkloadAnalysis result = analysis::analyze(song, bucket, configuration, *cache, control,
                [&](const WorkloadAnalysis& w) { ready = s(w.bucket_us); hasReady = true; });
            out.set(analysis_text(result, hasReady ? &ready : nullptr));
        }
        return true;
    }
    if (op == "project") {
        AnalysisReplay& st = state(c);
        AnalysisReplay* captured = &st;
        out.state = [captured] { return progress_text(*captured); };
        st.progress.clear();
        const std::string& gate = a[1];
        int64_t bucket = to_i64(a[2]);
        std::shared_ptr<const AnalysisConfiguration> configuration = parse_configuration(a[3]);
        AnalysisSong song;
        song.events = EventSource(&c.song->store);
        song.duration_us = c.song->duration_us;
        AnalysisControl control = recording_control(st);
        AnalysisControl quiet;
        WorkloadAnalysis result = analysis::copy_workload(
            analysis::analyze_uncached(song, bucket, nullptr, quiet, st.cache.get()), configuration, quiet);
        Fnv digest;
        std::string sample;
        int64_t count = 0;
        analysis::GateObserver observer = [&](const EventView& e, int64_t time) {
            std::string text;
            append_event(text, e);
            text += "@" + s(time);
            digest.add(text);
            digest.add(";");
            if (count < 4) sample += " " + text;
            count++;
        };
        if (gate == "pernote") analysis::project_per_note_gate(song, result, *configuration, control, observer);
        else if (gate == "delayed") analysis::project_delayed_note_off(song, result, *configuration, control, observer);
        else if (gate == "attack") analysis::project_note_attack_limit(song, result, *configuration, control, observer);
        else analysis::project_output_rate_limit(song, result, *configuration, control, observer);
        out.set("emitted " + s(count) + " " + digest.text() + sample + "\n= " + analysis_text(result, nullptr));
        return true;
    }
    if (op == "route") {
        AnalysisReplay& st = state(c);
        st.route.reset();
        routing::TrackRoutingSettings settings = parse_settings(a[1]);
        std::shared_ptr<routing::CompiledTrackRoute> route = routing::compile_route(routing_song(c.song), settings);
        st.route = route;
        st.route_song = c.song;
        out.set(route_text(*route) + "\n= " + form_text(*route, settings));
        return true;
    }
    if (op == "routed-view") {
        AnalysisReplay& st = state(c);
        st.routed.reset();
        st.routed_cache.reset();
        st.routed_source.reset();
        std::unique_ptr<routing::TrackRoutedEventStore> view(
            new routing::TrackRoutedEventStore(routing_song(c.song), st.route));
        st.routed = std::move(view);
        st.routed_source = c.song;
        st.routed_duration = c.song->duration_us;
        st.routed_cache.reset(new WorkloadCache());
        EventSource events(st.routed.get());
        Fnv digest;
        std::string sample;
        for (int32_t i = 0; i < events.count(); ++i) {
            EventView e = events.at(i);
            std::string text;
            append_event(text, e);
            text += "/" + s(e.source_index);
            digest.add(text);
            digest.add(";");
            if (i < 4) sample += " " + text;
        }
        out.set("view " + s(events.count()) + " " + digest.text() + " lower " +
            s(events.lower_bound_by_time(st.routed_duration / 2)) + sample);
        return true;
    }
    return false;
}

} // namespace models
} // namespace midibn
