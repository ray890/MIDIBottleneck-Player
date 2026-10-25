#include "scenario_runner.h"

#include "lockstep_scheduler.h"
#include "midibn/channel_index.h"
#include "midibn/engine/playback_engine.h"
#include "midibn/event_store.h"
#include "midibn/midi_output.h"
#include "midibn/routing/track_routing.h"
#include "midibn/sha256.h"
#include "scenario_binder.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace midibn {
namespace harness {

using engine::EngineError;
using engine::PlaybackEngine;
using engine::PlaybackSnapshot;
using engine::PlaybackState;

namespace {

const int64_t kBaseTime = 10000000;      // non-zero virtual start
const int kHangMilliseconds = 20000;

void append_number(std::string& text, int64_t value)
{
    char buffer[24];
    int n = 0;
    bool negative = value < 0;
    uint64_t v = negative ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    do {
        buffer[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    if (negative) text.push_back('-');
    while (n > 0) text.push_back(buffer[--n]);
}

// The recording shared by the harness thread, the worker and the output.
struct RunLog {
    std::mutex mutex;
    std::string text;
    void append(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        text += line;
    }
};

// A message the engine is about to send, recorded by the send observer
// (immediately before the output call, on the sending thread).
struct PendingSend {
    bool valid = false;
    EventKind kind = EventKind::NoteOff;
    int32_t source = 0;
    int32_t length = 0;
};
thread_local PendingSend t_pending;

// RecordingMidiOutput of the spike: every Send/Reset/Panic is recorded with
// the virtual timestamp and the calling party (W = worker, H = harness
// thread), then each Send advances virtual time by max(1, bytes) * 320 us
// (31,250 bit/s, 10 bits per byte) and yields at the send boundary.
class HarnessOutput : public platform::MidiOutput {
public:
    static constexpr int64_t kMicrosecondsPerByte = 320;

    HarnessOutput(std::shared_ptr<RunLog> log, LockStepScheduler& scheduler, std::thread::id harness)
        : log_(std::move(log)), scheduler_(scheduler), harness_(harness) {}

    void observe(const models::EventView& e)
    {
        t_pending.valid = true;
        t_pending.kind = e.kind;
        t_pending.source = e.event_index();
        t_pending.length = e.data_length;
    }

    const char* backend_name() const override { return "Harness"; }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return std::vector<platform::OutputDeviceInfo>(); }
    platform::OutputStatus open(uint32_t) override { open_ = true; return platform::OutputStatus::success(); }
    platform::OutputStatus close() override { open_ = false; return platform::OutputStatus::success(); }
    bool is_open() const override { return open_; }
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string&) override {}

    platform::OutputStatus send_short(uint32_t packed) override
    {
        uint8_t bytes[3] = { static_cast<uint8_t>(packed & 0xFF), static_cast<uint8_t>((packed >> 8) & 0xFF),
            static_cast<uint8_t>((packed >> 16) & 0xFF) };
        PendingSend pending = t_pending;
        t_pending.valid = false;
        int32_t length = pending.valid ? pending.length : 3;
        send(pending, bytes, static_cast<size_t>(std::min<int32_t>(length, 3)), length);
        return platform::OutputStatus::success();
    }

    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override
    {
        PendingSend pending = t_pending;
        t_pending.valid = false;
        pending.valid = true;
        pending.kind = EventKind::SystemExclusive;
        pending.source = fragment.event_index;
        send(pending, fragment.data, fragment.length, static_cast<int32_t>(fragment.length));
        return platform::OutputStatus::success();
    }

    platform::OutputStatus reset() override
    {
        resets++;
        record_plain("RESET");
        return platform::OutputStatus::success();
    }

    platform::OutputStatus panic() override
    {
        panics++;
        record_plain("PANIC");
        return platform::OutputStatus::success();
    }

    int64_t sends = 0;
    int64_t resets = 0;
    int64_t panics = 0;

private:
    char who() const { return std::this_thread::get_id() == harness_ ? 'H' : 'W'; }

    void record_plain(const char* word)
    {
        std::string line;
        append_number(line, scheduler_.now());
        line += ' ';
        line.push_back(who());
        line += ' ';
        line += word;
        line.push_back('\n');
        log_->append(line);
    }

    void send(const PendingSend& pending, const uint8_t* bytes, size_t shown, int32_t length)
    {
        int64_t cost = std::max<int64_t>(1, length) * kMicrosecondsPerByte;
        std::string line;
        append_number(line, scheduler_.now());
        line += ' ';
        line.push_back(who());
        line += " SEND ";
        line += pending.valid ? event_kind_name(pending.kind) : "Unobserved";
        line += " src=";
        append_number(line, pending.valid ? pending.source : 0);
        line += ' ';
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < shown; ++i) {
            line.push_back(hex[bytes[i] >> 4]);
            line.push_back(hex[bytes[i] & 15]);
        }
        line += " cost=";
        append_number(line, cost);
        line.push_back('\n');
        log_->append(line);
        sends++;
        scheduler_.advance(cost);
        scheduler_.send_boundary();
    }

    std::shared_ptr<RunLog> log_;
    LockStepScheduler& scheduler_;
    std::thread::id harness_;
    bool open_ = true;
};

// Keeps the route and its view alive for as long as the routed song plays.
struct RouteHolder {
    std::shared_ptr<Song> song;
    std::shared_ptr<routing::CompiledTrackRoute> route;
    std::shared_ptr<routing::TrackRoutedEventStore> view;
};

int64_t next_due(const std::vector<BoundCommand>& commands, size_t next, int64_t cap)
{
    return next < commands.size() ? kBaseTime + commands[next].offset : cap;
}

void append_state_line(std::string& line, const PlaybackSnapshot& snap)
{
    line += " state=";
    line += engine::playback_state_name(snap.state);
}

// Drive of the spike: applies the scripted commands at the first scheduling
// point at or after their time.
void drive(PlaybackEngine& engine, LockStepScheduler& scheduler, const BoundScenario& scenario, RunLog& log, int64_t cap,
    BindContext& context)
{
    const std::vector<BoundCommand>& commands = scenario.commands;
    bool finalStopIssued = !scenario.stop_after_end;
    size_t next = 0;
    scheduler.engine_wake_generation = [&engine] { return engine.wake_generation(); };
    scheduler.engine_playing = [&engine] { return engine.state() == PlaybackState::Playing; };
    int64_t seenSeq = scheduler.park_seq();
    std::string pendingNote;
    bool havePendingNote = false;
    bool snapshotPending = false;
    BoundCommand synthetic;
    for (;;) {
        int64_t deadline = 0, parkSeq = 0;
        bool parked = scheduler.wait_quiescent(seenSeq, deadline, parkSeq);
        if (havePendingNote) {
            // Logged only once the worker is quiescent again, so it cannot
            // interleave nondeterministically with worker records.
            log.append(pendingNote + "\n");
            havePendingNote = false;
        }
        if (snapshotPending) {
            snapshotPending = false;
            PlaybackSnapshot snap = engine.snapshot();
            std::string line;
            append_number(line, scheduler.now());
            line += " H SNAP";
            append_state_line(line, snap);
            line += " processed="; append_number(line, snap.processed_events);
            line += " dropped="; append_number(line, snap.dropped_events);
            line += " queue="; append_number(line, snap.queue_length);
            line += " position="; append_number(line, snap.playback_microseconds);
            line += " intended="; append_number(line, snap.intended_timeline_microseconds);
            line += " worker=";
            line += parked ? (deadline == INT64_MAX ? "waiting-no-deadline" : "parked") : "none";
            line.push_back('\n');
            if (scenario.snapshot_mode == 2) {
                append_number(line, scheduler.now());
                line += " H SNAPX gateFiltered="; append_number(line, snap.gate_filtered_events);
                line += " musicalRejectedAttacks="; append_number(line, snap.musical_limit_rejected_attacks);
                line += " musicalSuppressedReleases="; append_number(line, snap.musical_limit_suppressed_releases);
                line += " rateRejectedAttacks="; append_number(line, snap.output_rate_rejected_attacks);
                line += " rateSuppressedReleases="; append_number(line, snap.output_rate_suppressed_releases);
                line += " rateRejectedOther="; append_number(line, snap.output_rate_rejected_other_messages);
                line += " rateSafetySoftExcess="; append_number(line, snap.output_rate_safety_soft_excess);
                line += " rateSafetyBypass="; append_number(line, snap.output_rate_safety_bypass_messages);
                line += " virtualQueue="; append_number(line, snap.virtual_queue_length);
                line += " maxQueue="; append_number(line, snap.maximum_queue_length);
                line += " lastDispatched="; append_number(line, snap.last_dispatched_timeline_microseconds);
                line += " frontier="; append_number(line, snap.effective_speed_frontier_microseconds);
                line += " lag="; append_number(line, snap.current_lag_microseconds);
                line += " maxLag="; append_number(line, snap.maximum_lag_microseconds);
                line.push_back('\n');
            }
            log.append(line);
        }
        const BoundCommand* command = nullptr;
        if (next < commands.size() && (!parked || kBaseTime + commands[next].offset <= deadline))
            command = &commands[next++];
        else if (next >= commands.size() && parked && deadline >= cap) {
            // A worker waiting with no deadline is paused with nothing left to
            // script: end the run at the current virtual time.
            bool noDeadline = deadline == INT64_MAX;
            synthetic = BoundCommand();
            synthetic.offset = noDeadline ? scheduler.now() - kBaseTime : deadline - kBaseTime;
            synthetic.name = noDeadline ? "Stop(paused at end)" : "Stop(CAP)";
            synthetic.starts_worker = [](PlaybackEngine&) { return false; };
            synthetic.apply = [](PlaybackEngine& e, BindContext&) { return e.stop(); };
            command = &synthetic;
        }
        else if (next >= commands.size() && !parked) {
            if (finalStopIssued) return;
            finalStopIssued = true;
            synthetic = BoundCommand();
            synthetic.offset = scheduler.now() - kBaseTime;
            synthetic.name = "Stop(after end)";
            synthetic.starts_worker = [](PlaybackEngine&) { return false; };
            synthetic.apply = [](PlaybackEngine& e, BindContext&) { return e.stop(); };
            command = &synthetic;
        }

        if (command == nullptr) {
            // Nothing due before the worker's deadline: advance and release.
            scheduler.next_command_time.store(next_due(commands, next, cap));
            scheduler.release_to_deadline(deadline);
            seenSeq = parkSeq;
            continue;
        }

        int64_t at = kBaseTime + command->offset;
        if (scheduler.now() < at) scheduler.set_time(at);
        int generation = scheduler.worker_generation();
        int64_t seqBefore = scheduler.park_seq();
        int32_t wakeGenerationBefore = engine.wake_generation();
        bool startsWorker = command->starts_worker(engine);
        scheduler.command_seen_seq.store(seqBefore);
        {
            std::string line;
            append_number(line, scheduler.now());
            line += " H CMD ";
            line += command->name;
            line.push_back('\n');
            log.append(line);
        }
        scheduler.note("CMD " + command->name + " t=" + std::to_string(scheduler.now()));
        // Published before apply: a worker released by this command may yield
        // at a send boundary once the following command is due.
        scheduler.next_command_time.store(next_due(commands, next, cap));
        scheduler.begin_command();
        EngineError error;
        try { error = command->apply(engine, context); }
        catch (const EngineError& thrown) { error = thrown; }
        scheduler.end_command();
        if (!error.ok()) {
            pendingNote = "CMD-EXCEPTION " + command->name + " " + error.type + ": " + error.message;
            havePendingNote = true;
        }
        if (startsWorker && !havePendingNote) scheduler.wait_for_worker_generation(generation + 1);
        bool released = startsWorker || !parked || scheduler.command_released_worker(parkSeq, wakeGenerationBefore);
        // seqBefore - 1: the worker is still parked at the same rendezvous. A
        // control wait inside the command (preserving Pause) may already have
        // released and seen later parks; never wait for those again.
        seenSeq = released ? std::max<int64_t>(seqBefore, scheduler.command_seen_seq.load()) : seqBefore - 1;
        snapshotPending = scenario.snapshot_mode != 0;
    }
}

std::string hex_prefix16(const std::string& bytes)
{
    Sha256 sha;
    sha.update(bytes);
    return sha.hex_digest().substr(0, 16);
}

} // namespace

const std::vector<std::string>& engine_fixture_names()
{
    static const std::vector<std::string> names = {
        "decimal-halfway.mid", "decimal-stress.mid", "empty-tracks.mid", "f0-all-message-types.mid",
        "f1-cross-track-tempo.mid", "f1-v1-test-midi.mid", "format0-two-tracks.mid", "header-extra-bytes.mid",
        "index-chase-rich.mid", "long-sysex-segment-crossing.mid", "many-events-segments.mid", "many-tracks-ties.mid",
        "metadata-edge.mid", "metadata-names.mid", "models-dense.mid", "models-notes.mid", "rmid-plain-odd.rmi",
        "rmid-wrapped.rmi", "running-status-repair.mid", "sysex-short.mid", "sysex-variants.mid",
        "tempo-changes.mid", "tempo-edge-values.mid" };
    return names;
}

std::string fixture_stem(const std::string& fileName)
{
    size_t dot = fileName.find_last_of('.');
    return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

bool run_engine_scenario(const std::shared_ptr<Song>& song, const std::string& fixtureFileName,
    const ScenarioDefinition& definition, EngineRunResult& result)
{
    result = EngineRunResult();
    const std::chrono::steady_clock::time_point runStarted = std::chrono::steady_clock::now();
    int64_t duration = std::max<int64_t>(1000, song->duration_us);
    BoundScenario scenario;
    std::string bindError;
    if (!bind_scenario(definition, duration, scenario, bindError)) {
        result.error = "bind: " + bindError;
        return false;
    }

    std::shared_ptr<LockStepScheduler> scheduler = std::make_shared<LockStepScheduler>(kHangMilliseconds);
    scheduler->set_time(kBaseTime);
    std::shared_ptr<RunLog> log = std::make_shared<RunLog>();
    std::shared_ptr<HarnessOutput> output = std::make_shared<HarnessOutput>(log, *scheduler, std::this_thread::get_id());
    BindContext context;
    context.output = output;
    std::string error;
    bool completed = false;
    int64_t cap = kBaseTime + 40 * duration + 600000000LL;
    std::unique_ptr<PlaybackEngine> engine;

    try {
        context.song = engine::make_playback_song(*song, fixtureFileName, song);
        if (scenario.has_route) {
            // MainForm.ApplyTrackRoute: compile the route over the loaded song
            // and play the derived view. An invalid plan cannot be applied
            // (the dialog's Apply stays disabled), so it is logged and the
            // unrouted song plays.
            routing::RoutingSong rs;
            rs.events = models::EventSource(&song->store);
            rs.track_count = song->track_count;
            rs.duration_us = song->duration_us;
            rs.tracks = &song->tracks;
            rs.identity = song.get();
            std::shared_ptr<RouteHolder> holder = std::make_shared<RouteHolder>();
            holder->song = song;
            holder->route = routing::compile_route(rs, scenario.route);
            const routing::CompiledTrackRoute& route = *holder->route;
            const routing::TrackRouteStatistics& stats = route.statistics;
            if (route.is_valid()) {
                holder->view = std::make_shared<routing::TrackRoutedEventStore>(rs, holder->route);
                context.song = engine::make_playback_song(*song, *holder->view, fixtureFileName, holder);
            }
            std::string line;
            append_number(line, scheduler->now());
            line += " H ROUTE valid="; append_number(line, route.is_valid() ? 1 : 0);
            line += " explicit="; append_number(line, route.requires_explicit_apply ? 1 : 0);
            line += " tier="; append_number(line, static_cast<int>(route.highest_tier));
            line += " errors="; append_number(line, static_cast<int64_t>(route.errors.size()));
            line += " lanes="; append_number(line, stats.routed_lane_count);
            line += " destinations="; append_number(line, stats.destination_count);
            line += " merged="; append_number(line, stats.merged_lane_count);
            line += " retimbred="; append_number(line, stats.retimbred_lane_count);
            line += " omitted="; append_number(line, stats.omitted_lane_count);
            line += " omittedStarts="; append_number(line, stats.omitted_note_start_count);
            line += " generated="; append_number(line, stats.generated_destination_state_count);
            line += " included="; append_number(line, stats.included_source_event_count);
            line += " output="; append_number(line, stats.routed_output_event_count);
            line += " applied="; append_number(line, route.is_valid() ? 1 : 0);
            line.push_back('\n');
            log->append(line);
        }
    }
    catch (const models::ModelException& ex) {
        error = std::string(ex.type) + ": " + ex.message;
        log->append("HARNESS-ERROR " + error + "\n");
    }
    catch (const EngineError& ex) {
        error = ex.type + ": " + ex.message;
        log->append("HARNESS-ERROR " + error + "\n");
    }

    if (error.empty()) {
        engine.reset(new PlaybackEngine(scheduler));
        std::shared_ptr<HarnessOutput> observed = output;
        engine->set_send_observer([observed](const models::EventView& e) { observed->observe(e); });
        std::shared_ptr<LockStepScheduler> timeSource = scheduler;
        std::shared_ptr<RunLog> sharedLog = log;
        engine->set_playback_ended_handler([timeSource, sharedLog] {
            std::string line;
            append_number(line, timeSource->now());
            line += " W EVENT PlaybackEnded\n";
            sharedLog->append(line);
        });
        engine->set_playback_failed_handler([timeSource, sharedLog](const EngineError& e) {
            std::string line;
            append_number(line, timeSource->now());
            line += " W EVENT PlaybackFailed " + e.type + ": " + e.message + "\n";
            sharedLog->append(line);
        });
        engine->set_channel_control_failed_handler([timeSource, sharedLog](int channel, ChannelAttribute attribute,
            const EngineError& e) {
            std::string line;
            append_number(line, timeSource->now());
            line += " W EVENT ChannelControlFailed ch=";
            append_number(line, channel);
            line += ' ';
            line += channel_attribute_name(attribute);
            line += ' ' + e.type + ": " + e.message + "\n";
            sharedLog->append(line);
        });
        try {
            for (const SetupAction& action : scenario.setup) {
                EngineError setupError = action(*engine);
                if (!setupError.ok()) throw setupError;
            }
            drive(*engine, *scheduler, scenario, *log, cap, context);
            completed = true;
        }
        catch (const EngineError& ex) {
            error = ex.type + ": " + ex.message;
            log->append("HARNESS-ERROR " + error + "\n");
        }
    }

    if (error.empty()) {
        PlaybackSnapshot snap = engine->snapshot();
        std::string line = "FINAL t=";
        append_number(line, scheduler->now());
        append_state_line(line, snap);
        line += " processed="; append_number(line, snap.processed_events);
        line += " dropped="; append_number(line, snap.dropped_events);
        line += " maxQueue="; append_number(line, snap.maximum_queue_length);
        line += " maxLagUs="; append_number(line, snap.maximum_lag_microseconds);
        line += " position="; append_number(line, snap.playback_microseconds);
        line.push_back('\n');
        log->append(line);
        EngineError disposeError = engine->dispose();
        if (!disposeError.ok()) error = "Dispose: " + disposeError.type + ": " + disposeError.message;
        line.clear();
        line = "DISPOSED t=";
        append_number(line, scheduler->now());
        line.push_back('\n');
        log->append(line);
    }
    {
        std::lock_guard<std::mutex> lock(log->mutex);
        result.out = log->text;
    }
    if (error.empty() && !scheduler->protocol_error.empty()) error = "protocol: " + scheduler->protocol_error;
    result.sched = scheduler->trace();
    result.error = error;
    result.sends = output->sends;
    result.parks = scheduler->parks;
    result.go_resumes = scheduler->go_resumes;
    result.wake_resumes = scheduler->wake_resumes;
    result.spins = scheduler->spin_count.load();
    result.yields = scheduler->send_yields;
    result.waits = scheduler->event_waits;
    result.control_waits = scheduler->control_waits;
    result.control_timeouts = scheduler->control_timeouts;
    result.virtual_end = scheduler->now() - kBaseTime;
    result.real_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - runStarted).count();

    // No worker may outlive the objects it uses: end whatever is left (only a
    // failed run has a live worker), then destroy the engine.
    if (!completed || !error.empty()) scheduler->abort_and_join();
    engine.reset();
    return error.empty();
}

std::string console_summary(const std::string& scenario, const std::string& fixtureFileName, const EngineRunResult& r)
{
    std::string line = scenario;
    if (line.size() < 30) line.append(30 - line.size(), ' ');
    line += ' ';
    std::string fixture = fixtureFileName;
    if (fixture.size() < 22) fixture.append(22 - fixture.size(), ' ');
    line += fixture;
    line += " out=" + hex_prefix16(r.out) + " sched=" + hex_prefix16(r.sched);
    line += " sends=" + std::to_string(r.sends) + " parks=" + std::to_string(r.parks) + " go=" + std::to_string(r.go_resumes) +
        " wake=" + std::to_string(r.wake_resumes) + " spins=" + std::to_string(r.spins) + " yields=" + std::to_string(r.yields) +
        " waits=" + std::to_string(r.waits) + " ctl=" + std::to_string(r.control_waits) + "/" + std::to_string(r.control_timeouts) +
        " virtEnd=" + std::to_string(r.virtual_end) + " realMs=" + std::to_string(r.real_milliseconds);
    return line;
}

} // namespace harness
} // namespace midibn
