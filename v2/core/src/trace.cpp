#include "midibn/trace.h"

#include "midibn/midi_loader.h"
#include "midibn/sha256.h"

#include <array>
#include <memory>
#include <vector>

namespace midibn {
namespace {

std::string num(int64_t value) { return std::to_string(value); }

std::string hex_byte(uint8_t value)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string s;
    s.push_back(kHex[value >> 4]);
    s.push_back(kHex[value & 0xF]);
    return s;
}

std::string quote(const std::string& ascii)
{
    std::string out = "\"";
    for (unsigned char c : ascii) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c >= 0x20 && c <= 0x7E) out.push_back(static_cast<char>(c));
        else {
            static const char kHex[] = "0123456789ABCDEF";
            out += "\\u00";
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out + "\"";
}

std::string quote(const std::u16string& text)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out = "\"";
    for (char16_t c : text) {
        if (c == u'"') out += "\\\"";
        else if (c == u'\\') out += "\\\\";
        else if (c >= 0x20 && c <= 0x7E) out.push_back(static_cast<char>(c));
        else {
            out += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(c >> shift) & 0xF]);
        }
    }
    return out + "\"";
}

std::string error_status(const LoadError& error)
{
    return "status error " + error.type + " " + quote(error.message);
}

class Body {
public:
    Body(std::string& out, bool summary) : out_(out), summary_(summary) {}
    void line(const std::string& text)
    {
        if (!summary_) { out_ += text; out_ += '\n'; return; }
        hash_.update(text);
        hash_.update("\n", 1);
    }
    void finish()
    {
        if (summary_) out_ += "digest sha256 " + hash_.hex_digest() + "\n";
    }

private:
    std::string& out_;
    bool summary_;
    Sha256 hash_;
};

std::string join_values(const int64_t* values, size_t count)
{
    std::string s;
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) s.push_back(' ');
        s += num(values[i]);
    }
    return s;
}

void write_preflight(std::string& t, platform::ByteSource& source, int64_t fileSize)
{
    t += "[preflight]\n";
    PreflightCounts c;
    LoadError error;
    if (preflight_scan(source, c, error)) {
        t += "status ok\n";
        t += "format " + num(c.format) + "\n";
        t += "tracks " + num(c.track_count) + "\n";
        t += "division " + num(c.division) + "\n";
        t += "dispatchable-events " + num(c.dispatchable_events) + "\n";
        t += "note-ons " + num(c.note_ons) + "\n";
        t += "final-long-payload-bytes " + num(c.final_long_payload_bytes) + "\n";
        t += "provisional-payload-bytes " + num(c.provisional_payload_bytes) + "\n";
        t += "tempo-changes " + num(c.tempo_changes) + "\n";
        t += "source-value-entries " + num(c.source_value_entries) + "\n";
        t += "chase-state-entries " + num(c.chase_state_entries) + "\n";
        t += "chase-parameter-value-entries " + num(c.chase_parameter_value_entries) + "\n";
        t += "running-status-repairs " + num(c.running_status_repairs) + "\n";
        t += "routing-lanes " + num(c.routing_lanes) + "\n";
        t += "routing-patch-state-events " + num(c.routing_patch_state_events) + "\n";
        t += "routing-patch-timeline-upper-bound " + num(c.routing_patch_timeline_upper_bound) + "\n";
        t += "routing-generated-state-upper-bound " + num(c.routing_generated_state_upper_bound) + "\n";
        for (int track = 0; track < c.track_count; ++track) {
            size_t i = static_cast<size_t>(track);
            t += "track " + num(track) + " events " + num(c.events_by_track[i]) + " payload-bytes " +
                num(c.payload_bytes_by_track[i]) + " tempo-changes " + num(c.tempo_changes_by_track[i]) + "\n";
        }
        t += "source-slots " + join_values(c.source_entries_by_slot.data(), c.source_entries_by_slot.size()) + "\n";
        t += "routing-note-lanes-by-channel " +
            join_values(c.routing_note_lanes_by_channel.data(), c.routing_note_lanes_by_channel.size()) + "\n";
        t += "routing-patch-state-by-channel " +
            join_values(c.routing_patch_state_events_by_channel.data(), c.routing_patch_state_events_by_channel.size()) + "\n";
        for (int ps : { 4, 8 }) {
            MemoryProjection p = preflight_estimate(c, ps);
            t += "estimate ptr" + num(ps) + " retained " + num(p.retained) + " peak-low " + num(p.peak_low) +
                " peak-high " + num(p.peak_high) + " provisional " + num(p.provisional) + " merge-workspace " +
                num(p.merge_workspace) + " names " + num(p.retained_names) + " routing-retained " +
                num(p.routing_retained) + " routing-peak " + num(p.routing_peak) + " retained-with-routing " +
                num(p.retained_with_routing) + " routing-peak-total " + num(p.routing_peak_total) +
                " warning-threshold " + num(p.warning_threshold) + " architecture-risk " +
                (p.architecture_risk ? "1" : "0") + " requires-warning " +
                (preflight_requires_warning(p) ? "1" : "0") + "\n";
        }
    }
    else t += error_status(error) + "\n";
    t += std::string("requires-preflight ptr4 ") + (preflight_required(fileSize, 4) ? "1" : "0") +
        " ptr8 " + (preflight_required(fileSize, 8) ? "1" : "0") + "\n";
}

void write_events(std::string& t, const Song& song, bool summary)
{
    const EventStore& store = song.store;
    t += "[events]\n";
    t += "count " + num(store.count()) + "\n";
    Body body(t, summary);
    std::vector<uint8_t> payload;
    std::string line;
    for (int64_t i = 0; i < store.count(); ++i) {
        const EventRecord& r = store.records[i];
        line = "e " + num(i) + " " + num(r.tick) + " " + num(r.micros) + " " + num(r.track) + " " +
            event_kind_name(r.kind()) + " " + num(r.channel()) + " " + hex_byte(r.status) + " " +
            num(r.payload_length) + " ";
        if (r.payload_length == 0) line += "-";
        else if (r.payload_length <= 3) {
            for (int b = 0; b < r.payload_length; ++b) line += hex_byte(store.data_byte(i, b));
        }
        else {
            payload.resize(static_cast<size_t>(r.payload_length));
            store.payloads.copy_to(r.payload_offset, payload.data(), payload.size());
            Sha256 hash;
            hash.update(payload.data(), payload.size());
            line += "sha256:" + hash.hex_digest();
        }
        body.line(line);
    }
    body.finish();
}

// ------------------------------------------------------------ indexes

void write_series(Body& body, const std::string& header, const IndexSeries& series)
{
    if (series.count() == 0) return;
    body.line(header + " " + num(series.count()));
    for (int32_t i = 0; i < series.count(); ++i)
        body.line("v " + num(series[i].event_index) + " " + num(series[i].value));
}

const char* const kReadoutNames[9] = { "BankMsb", "BankLsb", "Program", "Volume", "Expression", "Pan", "Sustain",
    "PitchBend", "ChannelPressure" };

std::array<int, 16 * 9> readout_values(const std::array<ChannelReadout, 16>& readouts)
{
    std::array<int, 16 * 9> values;
    for (size_t c = 0; c < 16; ++c) {
        const ChannelReadout& r = readouts[c];
        const int v[9] = { r.bank_msb, r.bank_lsb, r.program, r.volume, r.expression, r.pan, r.sustain, r.pitch_bend,
            r.channel_pressure };
        for (size_t i = 0; i < 9; ++i) values[c * 9 + i] = v[i];
    }
    return values;
}

// TRACE_FORMAT.md "filtered chase configuration".
void filtered_chase_configuration(ChannelRouting& routing, ChannelOverrideValues& overrides)
{
    for (int channel = 0; channel < 16; ++channel) {
        if (channel % 3 == 1) routing.disabled_mask = static_cast<uint16_t>(routing.disabled_mask | (1u << channel));
        switch (channel % 4) {
        case 0:
            overrides.set(channel, ChannelAttribute::Volume, 100);
            overrides.set(channel, ChannelAttribute::Program, 5);
            overrides.set(channel, ChannelAttribute::PitchBend, 0);
            break;
        case 1:
            overrides.set(channel, ChannelAttribute::BankMsb, 1);
            overrides.set(channel, ChannelAttribute::BankLsb, 2);
            overrides.set(channel, ChannelAttribute::Pan, 64);
            overrides.set(channel, ChannelAttribute::Expression, 90);
            overrides.set(channel, ChannelAttribute::Sustain, 1);
            overrides.set(channel, ChannelAttribute::Aftertouch, 10);
            break;
        case 3:
            overrides.set(channel, ChannelAttribute::PitchBend, -8192);
            overrides.set(channel, ChannelAttribute::Aftertouch, 0);
            overrides.set(channel, ChannelAttribute::Volume, 0);
            break;
        }
    }
}

std::string message_text(const std::vector<ChaseMessage>& messages)
{
    if (messages.empty()) return "-";
    std::string text;
    for (const ChaseMessage& m : messages) {
        if (!text.empty()) text.push_back(' ');
        text += event_kind_name(m.kind);
        text.push_back(':');
        for (int b = 0; b < m.length; ++b) text += hex_byte(m.bytes[b]);
    }
    return text;
}

std::string text_digest(const std::string& text)
{
    Sha256 hash;
    hash.update(text);
    return "sha256:" + hash.hex_digest();
}

void write_indexes(std::string& t, const Song& song, bool summary)
{
    const SourceValueIndex& source = song.source_index;
    t += "[source-index]\n";
    t += "count " + num(source.entry_count()) + "\n";
    {
        Body body(t, summary);
        for (int slot = 0; slot < SourceValueIndex::kSlotCount; ++slot)
            write_series(body, "slot " + num(slot / 9) + " " + channel_attribute_name(static_cast<ChannelAttribute>(slot % 9)),
                source.slot(slot));
        body.finish();
    }

    const ChaseIndex& chase = song.chase_index;
    t += "[chase-index]\n";
    t += "entries " + num(chase.entry_count()) + " parameter-series " + num(chase.parameter_series_count()) + "\n";
    {
        Body body(t, summary);
        for (int channel = 0; channel < 16; ++channel)
            for (int slot = 0; slot < ChaseIndex::kSlotsPerChannel; ++slot)
                write_series(body, "state " + num(channel) + " " + num(slot), chase.state(channel, slot));
        for (int channel = 0; channel < 16; ++channel) write_series(body, "reset " + num(channel), chase.resets(channel));
        for (int channel = 0; channel < 16; ++channel) write_series(body, "selector " + num(channel), chase.selectors(channel));
        for (int channel = 0; channel < 16; ++channel)
            for (const ChaseIndex::ParameterSeries& p : chase.parameters(channel))
                write_series(body, "parameter " + num(channel) + " " + num(p.key), p.values);
        body.finish();
    }
    if (summary) return;

    const EventStore& store = song.store;
    const int32_t count = static_cast<int32_t>(store.count());
    t += "[source-query]\n";
    std::array<int, 16 * 9> previous = {};
    std::array<bool, 16 * 9> previousKnown = {};
    for (int32_t index = 0; index < count; ++index) {
        int64_t time = store.records[index].micros;
        std::string line;
        for (int slot = 0; slot < 16 * 9; ++slot) {
            int value;
            bool known = source.try_get_latest(store, slot / 9, static_cast<ChannelAttribute>(slot % 9), time, value);
            if (known == previousKnown[static_cast<size_t>(slot)] &&
                (!known || value == previous[static_cast<size_t>(slot)])) continue;
            previousKnown[static_cast<size_t>(slot)] = known;
            previous[static_cast<size_t>(slot)] = value;
            if (line.empty()) line = "q " + num(index) + " " + num(time);
            line += " " + num(slot / 9) + "." + channel_attribute_name(static_cast<ChannelAttribute>(slot % 9)) + "=" +
                (known ? num(value) : std::string("-"));
        }
        if (!line.empty()) t += line + "\n";
    }

    t += "[chase-readouts]\n";
    std::array<ChannelReadout, 16> empty;
    empty.fill(unknown_channel_readout());
    std::array<int, 16 * 9> readout = readout_values(empty);
    for (int32_t exclusive = 0; exclusive <= count; ++exclusive) {
        std::array<int, 16 * 9> current = readout_values(chase.attribute_readouts(exclusive));
        std::string line;
        for (size_t i = 0; i < current.size(); ++i) {
            if (current[i] == readout[i]) continue;
            if (line.empty()) line = "r " + num(exclusive);
            line += " " + num(static_cast<int64_t>(i / 9)) + "." + kReadoutNames[i % 9] + "=" + num(current[i]);
        }
        if (!line.empty()) t += line + "\n";
        readout = current;
    }

    ChannelRouting routing;
    ChannelOverrideValues overrides;
    filtered_chase_configuration(routing, overrides);
    for (bool filtered : { false, true }) {
        t += filtered ? "[chase-messages-filtered]\n" : "[chase-messages]\n";
        std::string last = "-";
        for (int32_t exclusive = 0; exclusive <= count; ++exclusive) {
            std::vector<ChaseMessage> list = filtered ? chase.messages(exclusive, &routing, &overrides)
                                                      : chase.messages(exclusive, nullptr, nullptr);
            std::string messages = message_text(list);
            if (messages != last) {
                t += "c " + num(exclusive) + " " + num(static_cast<int64_t>(list.size())) + " " + text_digest(messages) + "\n";
                last = messages;
            }
        }
        t += "final " + last + "\n";
    }
}

// ------------------------------------------------------------ flow

enum class Decision { Accept, Decline, AcceptThenCancel };

struct FlowScenario {
    const char* name;
    bool force_preflight;
    bool force_warning;
    Decision decision;
};

const FlowScenario kFlowScenarios[] = {
    { "natural", false, false, Decision::Accept },
    { "preflight", true, false, Decision::Accept },
    { "warning-decline", true, true, Decision::Decline },
    { "warning-accept", true, true, Decision::Accept },
    { "warning-accept-cancel", true, true, Decision::AcceptThenCancel },
};

// cancel_at: -1 never, 0 before the call, K >= 1 inside the K-th report.
class ScriptedMonitor : public LoadMonitor {
public:
    ScriptedMonitor(int cancelAt, Body* body) : cancel_at_(cancelAt), body_(body), cancelled_(cancelAt == 0) {}
    void progress(const LoadProgress& p) override
    {
        ++reports_;
        if (body_)
            body_->line("p " + quote(p.stage) + " " + num(p.completed_tracks) + " " + num(p.total_tracks) + " " +
                num(p.overall_completed) + " " + num(p.overall_total) + " " + num(p.stage_completed) + " " +
                num(p.stage_total));
        if (reports_ == cancel_at_) cancelled_ = true;
    }
    bool cancellation_requested() override { return cancelled_; }
    void cancel() { cancelled_ = true; }
    int reports() const { return reports_; }

private:
    int cancel_at_;
    Body* body_;
    bool cancelled_;
    int reports_ = 0;
};

struct FlowRun {
    int reports = 0;
    bool decision_requested = false;
    std::string outcome;
};

FlowRun run_flow(platform::ByteSource& source, int pointerSize, const FlowScenario& scenario, int cancelAt, Body* body)
{
    FlowRun run;
    ScriptedMonitor monitor(cancelAt, body);
    LoadFlowOptions options;
    options.monitor = &monitor;
    options.force_preflight = scenario.force_preflight;
    options.force_warning = scenario.force_warning;
    options.decision = [&](const PreflightCounts&, const MemoryProjection&) {
        run.decision_requested = true;
        if (scenario.decision == Decision::AcceptThenCancel) monitor.cancel();
        return scenario.decision != Decision::Decline;
    };
    std::unique_ptr<Song> song(new Song());
    LoadError error;
    LoadFlowResult result = load_with_preflight(source, pointerSize, options, *song, error);
    run.reports = monitor.reports();
    if (result == LoadFlowResult::Loaded)
        run.outcome = "ok events " + num(song->store.count()) + " notes " + num(song->note_count) + " duration-us " +
            num(song->duration_us);
    else if (result == LoadFlowResult::Declined) run.outcome = "declined";
    else run.outcome = "error " + error.type + " " + quote(error.message);
    return run;
}

} // namespace

std::string write_flow_trace(platform::ByteSource& source, const std::string& input_name, bool summary,
    int pointer_size, const std::string& producer)
{
    std::string t;
    t += "# producer " + producer + "\n";
    t += "# input " + input_name + "\n";
    t += "trace-format 1\n";
    t += "kind flow\n";
    t += std::string("mode ") + (summary ? "summary" : "full") + "\n";
    t += "pointer-size " + num(pointer_size) + "\n";
    for (const FlowScenario& scenario : kFlowScenarios) {
        t += std::string("[scenario ") + scenario.name + "]\n";
        Body body(t, summary);
        FlowRun run = run_flow(source, pointer_size, scenario, -1, &body);
        body.finish();
        t += "reports " + num(run.reports) + " decision-requested " + (run.decision_requested ? "1" : "0") + "\n";
        t += "outcome " + run.outcome + "\n";
    }
    if (!summary) {
        for (const FlowScenario& scenario : kFlowScenarios) {
            std::string name = scenario.name;
            if (name != "natural" && name != "warning-accept") continue;
            int total = run_flow(source, pointer_size, scenario, -1, nullptr).reports;
            t += "[cancel-sweep " + name + "]\n";
            for (int k = 0; k <= total + 1; ++k) {
                FlowRun run = run_flow(source, pointer_size, scenario, k, nullptr);
                t += "k " + num(k) + " reports " + num(run.reports) + " decision-requested " +
                    (run.decision_requested ? "1" : "0") + " outcome " + run.outcome + "\n";
            }
        }
    }
    t += "[end]\n";
    return t;
}

std::string write_load_trace(platform::ByteSource& source, const std::string& input_name,
    bool summary, const std::string& producer)
{
    std::string t;
    t += "# producer " + producer + "\n";
    t += "# input " + input_name + "\n";
    t += "trace-format 1\n";
    t += std::string("mode ") + (summary ? "summary" : "full") + "\n";

    const int64_t fileSize = source.size();
    t += "[file]\n";
    t += "size " + num(fileSize) + "\n";
    bool rmid = false;
    LoadError probeError;
    t += std::string("container ") + (probe_container(source, rmid, probeError) ? (rmid ? "rmid" : "smf") : "error") + "\n";

    write_preflight(t, source, fileSize);

    t += "[song]\n";
    std::unique_ptr<Song> song(new Song());
    LoadOptions options;
    options.collect_tempo_map = true;
    LoadError error;
    if (load_song(source, options, *song, error)) {
        t += "status ok\n";
        t += "format " + num(song->format) + "\n";
        t += "tracks " + num(song->track_count) + "\n";
        t += "division " + num(song->division) + "\n";
        t += "note-count " + num(song->note_count) + "\n";
        t += "running-status-repairs " + num(song->running_status_repairs) + "\n";
        t += "duration-us " + num(song->duration_us) + "\n";
        t += "events " + num(song->store.count()) + "\n";
        t += "payload-bytes " + num(song->store.payloads.length()) + "\n";
        for (size_t i = 0; i < song->tracks.size(); ++i) {
            const TrackMetadata& m = song->tracks[i];
            t += "track " + num(static_cast<int64_t>(i)) + " name " + quote(m.name) + " name-truncated " +
                (m.name_truncated ? "1" : "0") + " instrument " + quote(m.instrument) + " instrument-truncated " +
                (m.instrument_truncated ? "1" : "0") + "\n";
        }
        t += "[tempo]\n";
        t += "count " + num(static_cast<int64_t>(song->tempo_map.size())) + "\n";
        Body body(t, summary);
        for (const TempoMapEntry& e : song->tempo_map)
            body.line("t " + num(e.tick) + " " + num(e.time_us) + " " + num(e.microseconds_per_quarter) + " " +
                num(e.track) + " " + num(e.order));
        body.finish();
        write_events(t, *song, summary);
        write_indexes(t, *song, summary);
    }
    else t += error_status(error) + "\n";
    t += "[end]\n";
    return t;
}

std::string strip_trace_comments(const std::string& trace)
{
    std::string out;
    size_t start = 0;
    while (start < trace.size()) {
        size_t end = trace.find('\n', start);
        if (end == std::string::npos) end = trace.size();
        if (trace[start] != '#') out.append(trace, start, end - start + (end < trace.size() ? 1 : 0));
        start = end + 1;
    }
    return out;
}

} // namespace midibn
