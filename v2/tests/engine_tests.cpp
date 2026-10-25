// Playback engine tests (Milestone 5, Group D). Deterministic: the real
// engine runs on a real worker thread under the lock-step virtual clock
// (platform/virtual/lockstep_services.h) with a recording output; one smoke
// test runs the Win32 services in real time against the None output. No
// test opens a MIDI device or loads a provider. These are smoke and property
// tests; the exact comparison with v1's recordings is the Group E harness.
// Exit code 0 when all checks pass.
#include "midibn/engine/playback_engine.h"
#include "midibn/midi_loader.h"
#include "midibn/null_output.h"
#include "midibn/user_messages.h"
#include "lockstep_services.h"
#include "scenario_file.h"
#include "scenario_runner.h"
#include "win32_timing.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace midibn;
using engine::EngineError;
using engine::OverflowPolicy;
using engine::PlaybackEngine;
using engine::PlaybackSnapshot;
using engine::PlaybackState;
using engine::ProcessingMode;
using engine::ServiceDurationMode;

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_scenario;

void check(bool condition, const char* what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL [%s] %s\n", g_scenario.c_str(), what);
    }
}

// ---------------------------------------------------------------- songs

class MemorySource : public platform::ByteSource {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    int64_t size() const override { return static_cast<int64_t>(bytes_.size()); }
    size_t read_at(int64_t offset, uint8_t* buffer, size_t count) override
    {
        if (offset >= size()) return 0;
        size_t available = bytes_.size() - static_cast<size_t>(offset);
        size_t n = count < available ? count : available;
        std::memcpy(buffer, bytes_.data() + offset, n);
        return n;
    }

private:
    std::vector<uint8_t> bytes_;
};

struct TrackEvent {
    uint32_t tick;
    std::vector<uint8_t> bytes;  // as written in the file (after the delta time)
};

void put_vlq(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t buffer[5];
    int n = 0;
    buffer[n++] = static_cast<uint8_t>(value & 0x7F);
    while ((value >>= 7) != 0) buffer[n++] = static_cast<uint8_t>(0x80 | (value & 0x7F));
    while (n > 0) out.push_back(buffer[--n]);
}

// Format 0, 96 ticks per quarter note, 500,000 us per quarter (one tick = 5208.33 us).
std::vector<uint8_t> make_smf(const std::vector<TrackEvent>& events)
{
    std::vector<uint8_t> track;
    put_vlq(track, 0);
    const uint8_t tempo[] = { 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 };
    track.insert(track.end(), tempo, tempo + sizeof(tempo));
    uint32_t last = 0;
    for (const TrackEvent& e : events) {
        put_vlq(track, e.tick - last);
        last = e.tick;
        track.insert(track.end(), e.bytes.begin(), e.bytes.end());
    }
    put_vlq(track, 0);
    track.push_back(0xFF);
    track.push_back(0x2F);
    track.push_back(0x00);
    std::vector<uint8_t> file = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k' };
    uint32_t length = static_cast<uint32_t>(track.size());
    file.push_back(static_cast<uint8_t>(length >> 24));
    file.push_back(static_cast<uint8_t>(length >> 16));
    file.push_back(static_cast<uint8_t>(length >> 8));
    file.push_back(static_cast<uint8_t>(length));
    file.insert(file.end(), track.begin(), track.end());
    return file;
}

struct LoadedSong {
    std::shared_ptr<Song> song;
    std::shared_ptr<engine::PlaybackSong> playback;
};

LoadedSong load(const std::vector<TrackEvent>& events)
{
    LoadedSong result;
    result.song = std::make_shared<Song>();
    MemorySource source(make_smf(events));
    LoadError error;
    bool ok = load_song(source, LoadOptions(), *result.song, error);
    check(ok, "fixture song loads");
    result.playback = engine::make_playback_song(*result.song, "memory.mid", result.song);
    return result;
}

// Bank/volume/program state, four notes on channel 0 (one every 96 ticks,
// held 48 ticks), a SysEx, and one note on channel 1.
std::vector<TrackEvent> basic_events()
{
    return {
        { 0, { 0xB0, 0x07, 100 } },
        { 0, { 0xC0, 5 } },
        { 0, { 0x90, 60, 100 } },
        { 48, { 0x80, 60, 0 } },
        { 96, { 0x90, 62, 100 } },
        { 120, { 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7 } },
        { 144, { 0x80, 62, 0 } },
        { 192, { 0x90, 64, 100 } },
        { 192, { 0x91, 40, 90 } },
        { 240, { 0x80, 64, 0 } },
        { 240, { 0x81, 40, 0 } },
        { 288, { 0x90, 65, 100 } },
        { 336, { 0x80, 65, 0 } },
    };
}

// A dense burst: `count` notes on channel 0, one tick apart.
std::vector<TrackEvent> dense_events(int count)
{
    std::vector<TrackEvent> events;
    for (int i = 0; i < count; ++i) {
        uint8_t key = static_cast<uint8_t>(36 + (i % 48));
        events.push_back({ static_cast<uint32_t>(i), { 0x90, key, 100 } });
        events.push_back({ static_cast<uint32_t>(i + 2), { 0x80, key, 0 } });
    }
    std::sort(events.begin(), events.end(), [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    return events;
}

// ---------------------------------------------------------------- output

struct Sent {
    int64_t time;
    bool worker;
    platform::RecordedMessage::Kind kind;
    uint32_t packed;
    std::vector<uint8_t> bytes;
};

// The recording output plus virtual send cost, the sending party and the
// virtual time of every send; optionally fails the Nth short send.
class TestOutput : public platform::RecordingMidiOutput {
public:
    explicit TestOutput(lockstep::LockStepEngineServices* services, int64_t cost = 0)
        : services_(services), cost_(cost) { open(0); }

    platform::OutputStatus send_short(uint32_t packed) override
    {
        if (before_send) before_send(static_cast<int>(sent.size()) + 1);
        if (fail_at_ > 0 && ++short_sends_ == fail_at_)
            return platform::OutputStatus::failure(platform::OutputErrorKind::InvalidOperation, "Injected send failure.");
        platform::OutputStatus status = RecordingMidiOutput::send_short(packed);
        log(platform::RecordedMessage::Short, packed, std::vector<uint8_t>());
        if (services_ != nullptr) {
            // The first send may cost more than the others (a stall that leaves a backlog behind it).
            services_->advance(first_send_cost >= 0 && sent.size() == 1 ? first_send_cost : cost_);
        }
        return status;
    }
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override
    {
        size_t before = messages.size();
        platform::OutputStatus status = RecordingMidiOutput::send_long(fragment);
        if (messages.size() > before) log(platform::RecordedMessage::Long, 0, messages.back().bytes);
        if (services_ != nullptr) services_->advance(cost_);
        return status;
    }
    platform::OutputStatus reset() override
    {
        log(platform::RecordedMessage::Reset, 0, std::vector<uint8_t>());
        return RecordingMidiOutput::reset();
    }
    platform::OutputStatus panic() override
    {
        panics++;
        return RecordingMidiOutput::panic();
    }

    std::vector<Sent> sent;
    int panics = 0;
    int fail_at_ = 0;
    int64_t first_send_cost = -1;
    std::function<void(int)> before_send;   // called on the sending thread with the number of the send about to happen

    int count_short(uint32_t packed) const
    {
        int n = 0;
        for (const Sent& s : sent) if (s.kind == platform::RecordedMessage::Short && s.packed == packed) ++n;
        return n;
    }
    int count_status(uint8_t statusHigh) const
    {
        int n = 0;
        for (const Sent& s : sent)
            if (s.kind == platform::RecordedMessage::Short && (s.packed & 0xF0) == statusHigh) ++n;
        return n;
    }
    const Sent* first_short(uint32_t packed) const
    {
        for (const Sent& s : sent) if (s.kind == platform::RecordedMessage::Short && s.packed == packed) return &s;
        return nullptr;
    }

private:
    void log(platform::RecordedMessage::Kind kind, uint32_t packed, std::vector<uint8_t> bytes)
    {
        Sent s;
        s.time = services_ != nullptr ? services_->now() : 0;
        s.worker = services_ != nullptr && services_->on_worker_thread();
        s.kind = kind;
        s.packed = packed;
        s.bytes = std::move(bytes);
        sent.push_back(std::move(s));
    }
    lockstep::LockStepEngineServices* services_;
    int64_t cost_;
    int short_sends_ = 0;
};

uint32_t msg(uint8_t a, uint8_t b, uint8_t c = 0) { return a | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16); }

// One engine under lock-step time with event counters.
struct Rig {
    std::shared_ptr<lockstep::LockStepEngineServices> services_owner;
    lockstep::LockStepEngineServices& services;
    std::shared_ptr<TestOutput> output_owner;
    TestOutput& output;
    PlaybackEngine engine;
    std::atomic<int> ended{ 0 };
    std::atomic<int> failed{ 0 };
    EngineError failure;
    std::vector<int32_t> sent_indices;

    // `observe` false leaves the send observer unset, which lets the engine send straight from the stored events
    // (the fast path of register entry DD-065).
    explicit Rig(int64_t cost = 0, bool observe = true)
        : services_owner(std::make_shared<lockstep::LockStepEngineServices>()), services(*services_owner),
          output_owner(std::make_shared<TestOutput>(services_owner.get(), cost)), output(*output_owner),
          engine(services_owner)
    {
        engine.set_playback_ended_handler([this] { ended++; });
        engine.set_playback_failed_handler([this](const EngineError& e) { failure = e; failed++; });
        if (observe)
            engine.set_send_observer([this](const models::EventView& e) { sent_indices.push_back(e.event_index()); });
    }
    EngineError start(const LoadedSong& song, ProcessingMode mode, int64_t at = 0, bool paused = false,
        bool continuation = false)
    {
        EngineError error;
        services.command([&] { error = engine.start(song.playback, output_owner, mode, at, paused, continuation); });
        return error;
    }
    EngineError run(const std::function<EngineError()>& command)
    {
        EngineError error;
        services.command([&] { error = command(); });
        return error;
    }
    void play_to_end() { check(services.run_until_idle(), "run_until_idle finishes"); }
};

// ---------------------------------------------------------------- scenarios

void test_time_conversion()
{
    g_scenario = "time";
    engine::TimeConversion qpc(10000000);
    check(qpc.microseconds_to_ticks(1234567) == 12345670, "us->ticks at 10 MHz");
    check(qpc.ticks_to_microseconds(12345679) == 1234567, "ticks->us truncates");
    check(qpc.ticks_to_microseconds(-15) == -1, "negative ticks truncate toward zero");
    engine::TimeConversion odd(3579545);
    check(odd.microseconds_to_ticks(1000000) == 3579545 && odd.microseconds_to_ticks(999999) == 3579541,
        "split quotient/remainder formula");
    bool overflow = false;
    try { qpc.microseconds_to_ticks(models::kInt64Max); }
    catch (const models::ModelException& e) { overflow = std::string(e.type) == "OverflowException"; }
    check(overflow, "checked overflow raises OverflowException");
    engine::TimeConversion huge(10000000000000LL);  // decimal branch
    check(huge.microseconds_to_ticks(3) == 30000000, "decimal branch us->ticks");
    check(huge.ticks_to_microseconds(30000001) == 3, "decimal branch ticks->us");

    // Register entry DD-066: the due threshold is exactly the largest time whose tick count is not above `ticks`.
    bool thresholdExact = true;
    int64_t unused;
    for (int64_t frequency : { int64_t(1000000), int64_t(3579545), int64_t(10000000), int64_t(14318180), int64_t(1000000000),
                               int64_t(2500000000LL), int64_t(7), int64_t(999999), int64_t(1000001) }) {
        engine::TimeConversion conversion(frequency);
        uint64_t seed = 88172645463325252ull ^ static_cast<uint64_t>(frequency);
        for (int sample = 0; sample < 4000; ++sample) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            int64_t ticks = sample < 200 ? sample : static_cast<int64_t>(seed % (sample < 2000 ? 100000000ull : 9000000000000ull));
            int64_t latest;
            if (!conversion.latest_microseconds_due(ticks, latest)) { thresholdExact = false; break; }
            if (conversion.microseconds_to_ticks(latest) > ticks) thresholdExact = false;
            if (latest < models::kInt64Max / 1000000 && conversion.microseconds_to_ticks(latest + 1) <= ticks) thresholdExact = false;
        }
    }
    check(thresholdExact, "latest_microseconds_due is the exact inverse of microseconds_to_ticks");
    check(!qpc.latest_microseconds_due(-1, unused) && !huge.latest_microseconds_due(5, unused) &&
        !qpc.latest_microseconds_due(models::kInt64Max / 1000000, unused),
        "the threshold is not offered where 64-bit arithmetic cannot give it");
}

void test_play_to_end()
{
    g_scenario = "queue play to end";
    LoadedSong song = load(basic_events());
    Rig rig;
    check(rig.engine.state() == PlaybackState::Stopped, "starts stopped");
    EngineError error = rig.start(song, ProcessingMode::Queue);
    check(error.ok(), "start succeeds");
    check(rig.engine.state() == PlaybackState::Playing, "playing after start");
    check(rig.engine.has_live_worker(), "worker alive");
    // The first Start has no previous output to reset (v1 Stop sees _output == null).
    check(rig.output.panics == 0, "first start sends no reset");
    rig.play_to_end();
    check(rig.engine.state() == PlaybackState::Completed, "completed");
    check(rig.ended.load() == 1 && rig.failed.load() == 0, "PlaybackEnded raised once");
    check(!rig.engine.has_live_worker(), "worker exited");
    PlaybackSnapshot s = rig.engine.snapshot();
    check(s.processed_events == song.song->store.count(), "every event processed");
    check(s.dropped_events == 0, "nothing dropped");
    check(s.playback_microseconds == song.song->duration_us, "transport frozen at the end");
    const Sent* noteOn = rig.output.first_short(msg(0x90, 62, 100));
    check(noteOn != nullptr && noteOn->time == 500000 && noteOn->worker, "second note sent at 500 ms by the worker");
    const Sent* last = rig.output.first_short(msg(0x80, 65, 0));
    check(last != nullptr && last->time == 1750000, "last note-off at 1.75 s");
    bool sysex = false;
    for (const Sent& m : rig.output.sent)
        if (m.kind == platform::RecordedMessage::Long && m.bytes.size() == 6 && m.bytes[0] == 0xF0 && m.bytes[5] == 0xF7)
            sysex = true;
    check(sysex, "SysEx delivered as one F0...F7 packet");
    check(static_cast<int64_t>(rig.sent_indices.size()) == song.song->store.count(), "observer saw every send");
    check(rig.services.spins() == 0, "no spin needed under the lock-step waiter");
}

void test_stop_restart_chase()
{
    g_scenario = "stop/restart chase";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(600000);
    size_t before = rig.output.sent.size();
    EngineError error = rig.run([&] { return rig.engine.stop(); });
    check(error.ok(), "stop succeeds");
    check(rig.engine.state() == PlaybackState::Stopped, "stopped");
    check(!rig.engine.has_live_worker(), "worker joined");
    // The stopped worker resets and silences on its own thread.
    bool workerReset = false;
    for (size_t i = before; i < rig.output.sent.size(); ++i)
        if (rig.output.sent[i].kind == platform::RecordedMessage::Reset && rig.output.sent[i].worker) workerReset = true;
    check(workerReset, "worker exit resets the output");
    check(rig.ended.load() == 0, "a stop is not an end");

    rig.output.sent.clear();
    error = rig.start(song, ProcessingMode::Queue, 1000000);
    check(error.ok(), "restart at 1 s");
    rig.play_to_end();
    // Chase restores program and volume before the first note at the new position.
    size_t program = rig.output.sent.size(), volume = rig.output.sent.size(), firstNote = rig.output.sent.size();
    for (size_t i = 0; i < rig.output.sent.size(); ++i) {
        const Sent& m = rig.output.sent[i];
        if (m.kind != platform::RecordedMessage::Short || !m.worker) continue;
        if (m.packed == msg(0xC0, 5) && program == rig.output.sent.size()) program = i;
        if (m.packed == msg(0xB0, 7, 100) && volume == rig.output.sent.size()) volume = i;
        if ((m.packed & 0xF0) == 0x90 && firstNote == rig.output.sent.size()) firstNote = i;
    }
    check(program < firstNote && volume < firstNote, "chase precedes the first note after restart");
    check(rig.output.count_short(msg(0x90, 60, 100)) == 0, "notes before the position are not replayed");
    check(rig.engine.state() == PlaybackState::Completed, "restart completes");
}

void test_pause_resume_classic()
{
    g_scenario = "pause/resume classic";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(700000);
    EngineError error = rig.run([&] { return rig.engine.pause(); });
    check(error.ok(), "pause succeeds");
    check(rig.engine.state() == PlaybackState::Paused, "paused");
    check(rig.engine.has_live_worker() && rig.services.worker_parked_untimed(), "replacement worker waits while paused");
    PlaybackSnapshot paused = rig.engine.snapshot();
    check(paused.playback_microseconds == 700000, "pause freezes the transport");
    rig.services.run_until(5000000);
    check(rig.engine.snapshot().playback_microseconds == 700000, "time does not move the paused transport");
    rig.run([&] { rig.engine.resume(); return EngineError(); });
    check(rig.engine.state() == PlaybackState::Playing, "resumed");
    rig.play_to_end();
    check(rig.engine.state() == PlaybackState::Completed && rig.ended.load() == 1, "completes after resume");
    const Sent* last = rig.output.first_short(msg(0x80, 65, 0));
    check(last != nullptr && last->time == 1750000 + 4300000, "remaining events shifted by the pause");
}

void test_seek_classic()
{
    g_scenario = "seek classic";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(300000);
    EngineError error = rig.run([&] { return rig.engine.seek(1400000); });
    check(error.ok(), "seek succeeds");
    check(rig.engine.state() == PlaybackState::Playing, "still playing after seek");
    rig.play_to_end();
    check(rig.output.count_short(msg(0x90, 62, 100)) == 0 && rig.output.count_short(msg(0x90, 64, 100)) == 0,
        "events between the old and new position are skipped");
    check(rig.output.count_short(msg(0x90, 65, 100)) == 1, "events after the target play");
    check(rig.engine.state() == PlaybackState::Completed, "completes after seek");
    // Stopped seek only moves the position.
    rig.run([&] { return rig.engine.seek(250000); });
    check(rig.engine.state() == PlaybackState::Stopped && rig.engine.snapshot().playback_microseconds == 250000,
        "seek while stopped moves the position only");
}

void test_drop_mode()
{
    g_scenario = "drop newest";
    LoadedSong song = load(dense_events(200));
    Rig rig;
    rig.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 20000, 31250, 0);
    rig.engine.set_queue_length_limit(4);
    rig.engine.set_overflow_policy(OverflowPolicy::DropNewest);
    rig.start(song, ProcessingMode::Drop);
    rig.play_to_end();
    PlaybackSnapshot s = rig.engine.snapshot();
    check(rig.engine.state() == PlaybackState::Completed, "drop run completes");
    check(s.dropped_events > 0, "a finite queue drops under pressure");
    check(s.processed_events + s.dropped_events == song.song->store.count(), "every event is processed or dropped");
    check(s.maximum_queue_length >= 4, "the queue fills to its limit (releases may exceed it as soft-limit traffic)");
    check(rig.output.count_status(0x80) >= rig.output.count_status(0x90) - 1, "releases are protected");

    g_scenario = "drop oldest complete note";
    Rig rig2;
    rig2.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 20000, 31250, 0);
    rig2.engine.set_queue_length_limit(6);
    rig2.engine.set_overflow_policy(OverflowPolicy::DropOldestCompleteNote);
    rig2.start(song, ProcessingMode::Drop);
    rig2.play_to_end();
    PlaybackSnapshot s2 = rig2.engine.snapshot();
    check(rig2.engine.state() == PlaybackState::Completed && s2.dropped_events > 0, "complete-note eviction drops");
    check(s2.processed_events + s2.dropped_events == song.song->store.count(), "counts conserved");
    check(rig2.output.count_status(0x90) == rig2.output.count_status(0x80), "every sent attack has its release");

    g_scenario = "drop age limit clear";
    Rig rig3;
    rig3.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 20000, 31250, 0);
    rig3.engine.set_queue_age_limit_enabled(true);
    rig3.engine.set_queue_age_limit_microseconds(50000);
    rig3.engine.set_overflow_policy(OverflowPolicy::ClearBufferAndCatchUp);
    rig3.start(song, ProcessingMode::Drop);
    rig3.play_to_end();
    PlaybackSnapshot s3 = rig3.engine.snapshot();
    check(rig3.engine.state() == PlaybackState::Completed && s3.dropped_events > 0, "age limit clears the buffer");
    check(rig3.output.panics > 1, "clearing panics the output");

    g_scenario = "virtual forward";
    Rig rig4;
    rig4.engine.set_simulate_slowdown(false);
    rig4.engine.set_apply_queue_limit_without_slowdown(true);
    rig4.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 20000, 31250, 0);
    rig4.engine.set_queue_length_limit(3);
    rig4.engine.set_overflow_policy(OverflowPolicy::DropIncomingCompleteNotes);
    rig4.start(song, ProcessingMode::Drop);
    rig4.play_to_end();
    PlaybackSnapshot s4 = rig4.engine.snapshot();
    check(s4.virtual_queue_active && s4.dropped_events > 0, "forward-only virtual queue drops");
    check(s4.virtual_maximum_queue_length >= 3, "virtual queue fills to its limit (safety admissions may exceed it)");
    check(s4.processed_events + s4.dropped_events == song.song->store.count(), "virtual counts conserved");
}

// DD-041: the overflow-time Clear buffer (finite queue full, policy Clear buffer) filters the skipped events of a
// muted channel instead of counting them as dropped, as the waiting-time Clear buffer always did.
void test_clear_buffer_filters_muted_channel()
{
    g_scenario = "clear buffer with a muted channel (DD-041)";
    std::vector<TrackEvent> events;
    int channelZero = 0;
    for (int i = 0; i < 200; ++i) {
        uint8_t key = static_cast<uint8_t>(36 + (i % 48));
        events.push_back({ static_cast<uint32_t>(i), { 0x90, key, 100 } });
        events.push_back({ static_cast<uint32_t>(i + 2), { 0x80, key, 0 } });
        events.push_back({ static_cast<uint32_t>(i), { 0x91, key, 100 } });
        events.push_back({ static_cast<uint32_t>(i + 2), { 0x81, key, 0 } });
        channelZero += 2;
    }
    std::stable_sort(events.begin(), events.end(), [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    LoadedSong song = load(events);
    Rig rig;
    rig.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 20000, 31250, 0);
    rig.engine.set_queue_length_limit(4);
    rig.engine.set_overflow_policy(OverflowPolicy::ClearBufferAndCatchUp);
    EngineError e = rig.engine.set_channel_enabled(1, false);
    check(e.ok(), "channel 2 muted before the start");
    rig.start(song, ProcessingMode::Drop);
    rig.play_to_end();
    PlaybackSnapshot s = rig.engine.snapshot();
    check(rig.engine.state() == PlaybackState::Completed && s.dropped_events > 0 && rig.output.panics > 1,
        "the full queue was cleared at least once");
    check(rig.output.count_status(0x91) == 0 && rig.output.count_status(0x81) == 0, "nothing of the muted channel is sent");
    check(s.processed_events + s.dropped_events == channelZero,
        "every event of the audible channel is processed or dropped, and none of the muted channel is counted");
}

void test_start_errors()
{
    g_scenario = "start errors";
    LoadedSong song = load(basic_events());
    Rig rig;
    EngineError e = rig.engine.start(nullptr, rig.output_owner, ProcessingMode::Queue);
    check(e.type == "ArgumentNullException" && e.message == "No song was supplied to start playback.",
        "null song text");
    rig.engine.set_simulate_slowdown(false);
    rig.engine.set_apply_queue_limit_without_slowdown(true);
    rig.engine.set_queue_age_limit_enabled(true);
    e = rig.engine.start(song.playback, rig.output_owner, ProcessingMode::Drop);
    check(e.type == "InvalidOperationException" &&
        e.message == "Queue waiting-time limits require a retained delayed queue and cannot be combined with forward-only admission.",
        "age limit with forward-only admission");
    rig.engine.set_queue_age_limit_enabled(false);
    rig.engine.set_overflow_policy(OverflowPolicy::DropOldest);
    e = rig.engine.start(song.playback, rig.output_owner, ProcessingMode::Drop);
    check(e.message == "This overflow policy can remove MIDI that was already sent. Turn on Simulate slowdown, or choose Drop incoming complete notes or Drop newest events.",
        "forward-only policy text");
    check(rig.engine.state() == PlaybackState::Stopped, "refused start leaves the engine stopped");
}

void test_failure_event()
{
    g_scenario = "send failure";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.output.fail_at_ = 3;  // the third short message (the first Start sends no reset)
    rig.start(song, ProcessingMode::Queue);
    rig.play_to_end();
    check(rig.failed.load() == 1 && rig.ended.load() == 0, "PlaybackFailed raised once");
    check(rig.failure.type == "InvalidOperationException" && rig.failure.message == "Injected send failure.",
        "failure carries the output's exception type and text");
    check(rig.engine.state() == PlaybackState::Stopped, "failure stops playback");
    check(rig.output.panics == 1, "worker failure panics the output once");
}

void test_overrides_and_disabled_channels()
{
    g_scenario = "overrides";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.engine.set_channel_monitoring(true);
    EngineError e = rig.engine.set_channel_override(0, ChannelAttribute::Volume, 50);
    check(e.ok(), "override accepted");
    e = rig.engine.set_channel_override(0, ChannelAttribute::Volume, 200);
    check(e.type == "ArgumentOutOfRangeException", "override range checked");
    e = rig.engine.set_channel_enabled(1, false);
    check(e.ok() && !rig.engine.is_channel_enabled(1), "channel 1 disabled without a worker");
    rig.start(song, ProcessingMode::Queue);
    rig.play_to_end();
    check(rig.output.count_short(msg(0xB0, 7, 50)) == 1, "forced volume sent once");
    check(rig.output.count_short(msg(0xB0, 7, 100)) == 0, "source volume suppressed");
    check(rig.output.count_short(msg(0x91, 40, 90)) == 0, "disabled channel filtered");
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> channels = rig.engine.channel_snapshot();
    check(channels != nullptr, "channel monitoring publishes a snapshot");
    if (channels) {
        check(channels->channels[0].volume == 50 && channels->channels[0].forced_volume == 50,
            "monitor shows the forced value");
        check(channels->channels[0].override_suppressed_events == 1, "suppressed source event counted");
        check(!channels->channels[1].enabled && channels->channels[1].muted_filtered_events == 2,
            "muted channel and its filtered events");
        check(channels->channels[0].program == 5, "program tracked");
    }
    check(rig.engine.snapshot().processed_events == song.song->store.count() - 3, "filtered events are not processed");

    g_scenario = "override while stopped";
    rig.output.sent.clear();
    e = rig.engine.set_channel_override(2, ChannelAttribute::Program, 7);
    check(e.ok() && rig.output.count_short(msg(0xC2, 7)) == 1 && !rig.output.sent.back().worker,
        "with no worker the override is sent at once on the control thread");
    e = rig.engine.chase_latest_source_channel_attribute(1, ChannelAttribute::Volume);
    check(e.type == "InvalidOperationException" && e.message == "Enable this MIDI channel before restoring its source value.",
        "chase on a disabled channel");
    e = rig.engine.chase_latest_source_channel_attribute(0, ChannelAttribute::Pan);
    check(e.message == "No source MIDI value for this channel attribute exists at the current position.",
        "chase without a source value");
    e = rig.engine.set_channel_override(0, ChannelAttribute::Volume, engine::ChannelOverrideState::kAutoValue);
    check(e.ok() && rig.engine.channel_override(0, ChannelAttribute::Volume) == engine::ChannelOverrideState::kAutoValue,
        "override cleared");
    rig.output.sent.clear();
    e = rig.engine.chase_latest_source_channel_attribute(0, ChannelAttribute::Volume);
    check(e.ok() && rig.output.count_short(msg(0xB0, 7, 100)) == 1, "chase restores the source volume");
    e = rig.engine.set_channel_enabled(1, true);
    check(e.ok() && rig.engine.is_channel_enabled(1), "channel re-enabled");
}

void test_live_controls_while_playing()
{
    g_scenario = "live controls";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(100000);
    std::atomic<int> completions(0);
    EngineError controlError;
    rig.run([&] {
        return rig.engine.send_explicit_channel_attribute(3, ChannelAttribute::Pan, 10,
            [&](const EngineError& error) { controlError = error; completions++; });
    });
    rig.run([&] { return rig.engine.set_channel_enabled(0, false); });
    rig.play_to_end();
    check(completions.load() == 1 && controlError.ok(), "queued control completes at the worker boundary");
    check(rig.output.count_short(msg(0xB3, 10, 10)) == 1, "explicit attribute sent");
    check(rig.output.count_short(msg(0xB0, 64, 0)) == 1 && rig.output.count_short(msg(0xB0, 123, 0)) >= 1,
        "disabling a channel silences it");
    check(rig.output.count_short(msg(0x90, 62, 100)) == 0, "notes on the disabled channel are filtered");
}

// One pitch retriggered every tick: interval gates must merge the repeats.
std::vector<TrackEvent> repeated_pitch_events(int count)
{
    std::vector<TrackEvent> events;
    for (int i = 0; i < count; ++i) {
        events.push_back({ static_cast<uint32_t>(2 * i), { 0x90, 60, 100 } });
        events.push_back({ static_cast<uint32_t>(2 * i + 1), { 0x80, 60, 0 } });
    }
    return events;
}

void test_gates()
{
    LoadedSong song = load(dense_events(120));
    LoadedSong repeated = load(repeated_pitch_events(60));

    g_scenario = "per-note gate";
    Rig a;
    a.engine.set_processing_microseconds(40000);
    a.start(repeated, ProcessingMode::PerNoteIntervalGate);
    a.play_to_end();
    PlaybackSnapshot sa = a.engine.snapshot();
    check(a.engine.state() == PlaybackState::Completed, "per-note gate completes");
    check(sa.processed_events > 0 && sa.processed_events < repeated.song->store.count(), "per-note gate thins output");
    check(sa.gate_filtered_events > 0, "gate-filtered events counted");
    check(sa.effective_speed_frontier_microseconds > 0, "gate frontier advances");
    for (const Sent& m : a.output.sent)
        if (m.worker && m.kind == platform::RecordedMessage::Short && (m.packed & 0xF0) == 0x90)
            check(m.time % 40000 == 0, "per-note attacks land on interval boundaries");

    g_scenario = "delayed note-off";
    Rig b;
    b.engine.set_processing_microseconds(60000);
    b.start(song, ProcessingMode::DelayedNoteOff);
    b.play_to_end();
    check(b.engine.state() == PlaybackState::Completed && b.ended.load() == 1, "delayed note-off completes");
    check(b.output.count_status(0x90) == b.output.count_status(0x80), "delayed note-off releases every attack");

    g_scenario = "note start rate";
    Rig c;
    c.engine.set_events_per_second(20);
    c.start(song, ProcessingMode::NoteAttackLimit);
    c.play_to_end();
    PlaybackSnapshot sc = c.engine.snapshot();
    check(c.engine.state() == PlaybackState::Completed, "note start gate completes");
    check(sc.musical_limit_rejected_attacks > 0, "note start gate rejects attacks");
    check(c.output.count_status(0x90) == c.output.count_status(0x80), "rejected attacks lose their releases too");

    g_scenario = "output rate";
    Rig d;
    d.engine.set_events_per_second(30);
    d.start(song, ProcessingMode::OutputRateLimit);
    d.play_to_end();
    PlaybackSnapshot sd = d.engine.snapshot();
    check(d.engine.state() == PlaybackState::Completed, "output rate gate completes");
    check(sd.output_rate_rejected_attacks > 0, "output rate gate rejects attacks");
}

void test_service_models()
{
    LoadedSong song = load(dense_events(20));

    g_scenario = "events per second";
    Rig a;
    a.engine.set_service_configuration(ServiceDurationMode::EventsPerSecond, 0, 31250, 100);
    a.start(song, ProcessingMode::Queue);
    a.play_to_end();
    PlaybackSnapshot sa = a.engine.snapshot();
    check(sa.processed_events == song.song->store.count(), "rate-limited queue sends everything");
    std::vector<int64_t> times;
    for (const Sent& m : a.output.sent) if (m.worker && m.kind == platform::RecordedMessage::Short) times.push_back(m.time);
    bool spaced = times.size() > 2;
    for (size_t i = 2; i < times.size(); ++i) if (times[i] - times[i - 1] != 10000) spaced = false;
    check(spaced, "100 events/s spaces completions 10 ms apart");
    check(sa.maximum_lag_microseconds > 0, "lag measured");

    g_scenario = "MIDI bitrate";
    Rig b;
    b.engine.set_service_configuration(ServiceDurationMode::MidiBitrate, 0, 31250, 0);
    b.start(song, ProcessingMode::Queue);
    b.play_to_end();
    times.clear();
    for (const Sent& m : b.output.sent) if (m.worker && m.kind == platform::RecordedMessage::Short) times.push_back(m.time);
    check(times.size() == static_cast<size_t>(song.song->store.count()), "bitrate queue sends everything");
    check(times.size() > 3 && times[3] - times[2] >= 960, "3-byte messages need 960 us at 31,250 bit/s");

    g_scenario = "output-paced timeline";
    Rig c(500);
    c.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 3000, 31250, 0);
    c.engine.set_output_paced_timeline(true);
    c.start(song, ProcessingMode::Queue);
    c.play_to_end();
    check(c.engine.state() == PlaybackState::Completed, "output-paced timeline completes");
    PlaybackSnapshot sc = c.engine.snapshot();
    check(sc.processed_events == song.song->store.count() && sc.current_lag_microseconds == 0,
        "output-paced timeline holds the transport");
}

void ordered_scenario(const char* name, ProcessingMode mode, const std::function<void(PlaybackEngine&)>& configure)
{
    g_scenario = std::string("ordered ") + name;
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.engine.set_preserve_playback_queue_on_pause_seek(true);
    configure(rig.engine);
    rig.start(song, mode);
    rig.services.run_until(600000);
    EngineError error = rig.run([&] { return rig.engine.pause(); });
    check(error.ok(), "preserving pause acknowledged");
    check(rig.engine.state() == PlaybackState::Paused && rig.engine.has_live_worker(), "worker kept while paused");
    error = rig.run([&] { return rig.engine.seek(1400000); });
    check(error.ok(), "preserving seek queued");
    check(rig.engine.snapshot().intended_timeline_microseconds == 1400000, "intended timeline shows the target");
    rig.run([&] { rig.engine.resume(); return EngineError(); });
    rig.play_to_end();
    check(rig.engine.state() == PlaybackState::Completed && rig.ended.load() == 1, "completes after pause/seek/resume");
    check(rig.output.count_short(msg(0xB0, 123, 0)) >= 1, "seek marker sends all-notes-off");
    check(rig.output.count_short(msg(0x90, 65, 100)) == 1, "events after the target play");
    check(rig.output.count_short(msg(0x90, 64, 100)) == 0, "skipped region not played");
}

void test_ordered()
{
    ordered_scenario("queue", ProcessingMode::Queue, [](PlaybackEngine& e) { e.set_processing_microseconds(1000); });
    ordered_scenario("queue no slowdown", ProcessingMode::Queue, [](PlaybackEngine& e) { e.set_simulate_slowdown(false); });
    ordered_scenario("drop", ProcessingMode::Drop, [](PlaybackEngine& e) {
        e.set_processing_microseconds(1500);
        e.set_queue_length_limit(32);
    });
    ordered_scenario("per-note", ProcessingMode::PerNoteIntervalGate, [](PlaybackEngine& e) { e.set_processing_microseconds(20000); });
    ordered_scenario("delayed", ProcessingMode::DelayedNoteOff, [](PlaybackEngine& e) { e.set_processing_microseconds(20000); });
    ordered_scenario("note start", ProcessingMode::NoteAttackLimit, [](PlaybackEngine& e) { e.set_events_per_second(50); });
    ordered_scenario("output rate", ProcessingMode::OutputRateLimit, [](PlaybackEngine& e) { e.set_events_per_second(500); });
    ordered_scenario("virtual forward", ProcessingMode::Drop, [](PlaybackEngine& e) {
        e.set_simulate_slowdown(false);
        e.set_apply_queue_limit_without_slowdown(true);
        e.set_overflow_policy(OverflowPolicy::DropNewest);
    });

    g_scenario = "ordered output-paced";
    LoadedSong song = load(dense_events(30));
    Rig rig(400);
    rig.engine.set_preserve_playback_queue_on_pause_seek(true);
    rig.engine.set_output_paced_timeline(true);
    rig.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 2000, 31250, 0);
    rig.start(song, ProcessingMode::Queue);
    rig.play_to_end();
    check(rig.engine.state() == PlaybackState::Completed && rig.engine.snapshot().processed_events == song.song->store.count(),
        "ordered output-paced queue completes");

    g_scenario = "ordered stop while paused";
    LoadedSong basic = load(basic_events());
    Rig r2;
    r2.engine.set_preserve_playback_queue_on_pause_seek(true);
    r2.start(basic, ProcessingMode::Queue);
    r2.services.run_until(300000);
    r2.run([&] { return r2.engine.pause(); });
    EngineError error = r2.run([&] { return r2.engine.stop(); });
    check(error.ok() && r2.engine.state() == PlaybackState::Stopped && !r2.engine.has_live_worker(),
        "stop while paused retires the preserved worker");
}

void test_channel_control_failure()
{
    g_scenario = "channel-control failure";
    LoadedSong song = load(basic_events());
    Rig rig;
    std::atomic<int> failures(0);
    int failedChannel = -1;
    EngineError eventError;
    rig.engine.set_channel_control_failed_handler([&](int channel, ChannelAttribute, const EngineError& error) {
        failedChannel = channel;
        eventError = error;
        failures++;
    });
    // DD-033: the detail handler also says whether the failed request was a mute / unmute.
    std::atomic<int> detailFailures(0);
    bool detailMute = true;
    ChannelAttribute detailAttribute = ChannelAttribute::BankMsb;
    rig.engine.set_channel_control_failure_detail_handler([&](int, ChannelAttribute attribute, bool isEnableRequest, const EngineError&) {
        detailAttribute = attribute;
        detailMute = isEnableRequest;
        detailFailures++;
    });
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(100000);
    rig.run([&] { return rig.engine.set_channel_enabled(5, false); });
    EngineError completionError;
    rig.run([&] {
        return rig.engine.send_explicit_channel_attribute(5, ChannelAttribute::Volume, 1,
            [&](const EngineError& error) { completionError = error; });
    });
    rig.play_to_end();
    check(failures.load() == 1 && failedChannel == 5, "ChannelControlFailed raised for the queued control");
    check(eventError.type == "InvalidOperationException" &&
        eventError.message == "Enable this MIDI channel before restoring its source value.", "control failure text");
    check(completionError.message == eventError.message, "completion receives the same failure");
    check(detailFailures.load() == 1 && !detailMute && detailAttribute == ChannelAttribute::Volume,
        "the detail handler reports an attribute request as such");

    g_scenario = "control while paused";
    Rig r2;
    r2.engine.set_preserve_playback_queue_on_pause_seek(true);
    r2.start(song, ProcessingMode::Queue);
    r2.services.run_until(100000);
    r2.run([&] { return r2.engine.pause(); });
    EngineError pausedResult = EngineError::make("pending", "");
    r2.run([&] {
        return r2.engine.send_explicit_channel_attribute(2, ChannelAttribute::Pan, 3,
            [&](const EngineError& error) { pausedResult = error; });
    });
    check(pausedResult.ok() && r2.output.count_short(msg(0xB2, 10, 3)) == 1,
        "a control queued while paused is applied by the paused worker");
    check(r2.engine.state() == PlaybackState::Paused && r2.services.worker_parked_untimed(), "still paused afterwards");
    EngineError stopped = r2.run([&] { return r2.engine.stop(); });
    check(stopped.ok() && !r2.engine.has_live_worker(), "stop while paused");
}
// Two identical runs produce identical output (times, bytes, threads).
void test_determinism()
{
    g_scenario = "determinism";
    LoadedSong song = load(dense_events(80));
    std::vector<Sent> logs[2];
    PlaybackSnapshot snaps[2];
    for (int run = 0; run < 2; ++run) {
        Rig rig(250);
        rig.engine.set_preserve_playback_queue_on_pause_seek(true);
        rig.engine.set_service_configuration(ServiceDurationMode::EventsPerSecond, 0, 31250, 400);
        rig.start(song, ProcessingMode::Drop);
        rig.services.run_until(150000);
        rig.run([&] { return rig.engine.pause(); });
        rig.run([&] { return rig.engine.seek(250000); });
        rig.services.run_until(400000);
        rig.run([&] { rig.engine.resume(); return EngineError(); });
        rig.play_to_end();
        logs[run] = rig.output.sent;
        snaps[run] = rig.engine.snapshot();
    }
    bool same = logs[0].size() == logs[1].size();
    for (size_t i = 0; same && i < logs[0].size(); ++i)
        same = logs[0][i].time == logs[1][i].time && logs[0][i].packed == logs[1][i].packed &&
            logs[0][i].worker == logs[1][i].worker && logs[0][i].kind == logs[1][i].kind;
    check(same && !logs[0].empty(), "repeated runs are identical");
    check(snaps[0].processed_events == snaps[1].processed_events && snaps[0].dropped_events == snaps[1].dropped_events &&
        snaps[0].maximum_queue_length == snaps[1].maximum_queue_length, "repeated snapshots are identical");
}

// Defects of v1's engine that v2 fixed (register entries DD-007 and DD-009; ENGINE_PORT_NOTES.md).
// These checks state the corrected behaviour.
void test_fixed_v1_defects()
{
    g_scenario = "pause on an event time (DD-007)";
    LoadedSong song = load(basic_events());
    Rig rig;
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(500000);  // the note at 500 ms has just been sent
    check(rig.output.count_short(msg(0x90, 62, 100)) == 1, "note at 500 ms sent before the pause");
    rig.run([&] { return rig.engine.pause(); });
    rig.run([&] { rig.engine.resume(); return EngineError(); });
    rig.play_to_end();
    check(rig.output.count_short(msg(0x90, 62, 100)) == 1,
        "DD-007: Pause and Resume do not send again the note that landed exactly on the paused time (v1 did)");

    g_scenario = "continuation start (DD-007)";
    Rig again;
    again.start(song, ProcessingMode::Queue);
    again.services.run_until(500000);
    check(again.output.count_short(msg(0x90, 62, 100)) == 1, "note at 500 ms sent before the restart");
    again.run([&] { return again.engine.stop(); });
    again.start(song, ProcessingMode::Queue, 500000, false, true);
    again.play_to_end();
    check(again.output.count_short(msg(0x90, 62, 100)) == 1, "a continuation start does not send the events at the start position again");
    Rig chosen;
    chosen.start(song, ProcessingMode::Queue, 500000);
    chosen.play_to_end();
    check(chosen.output.count_short(msg(0x90, 62, 100)) == 1, "a start the person chose at 500 ms plays the event at 500 ms");

    g_scenario = "ordered gate pressure (DD-009)";
    Rig gate;
    gate.engine.set_preserve_playback_queue_on_pause_seek(true);
    gate.engine.set_processing_microseconds(20000);
    gate.start(song, ProcessingMode::PerNoteIntervalGate);
    gate.services.run_until(1300000);
    PlaybackSnapshot s = gate.engine.snapshot();
    check(s.queue_length < 8, "DD-009: ordered gate pressure counts only the work that is really waiting, not everything since event 0");
    gate.run([&] { return gate.engine.stop(); });
}

void test_statistics()
{
    g_scenario = "statistics";
    LoadedSong song = load(dense_events(60));
    Rig rig;
    rig.engine.set_service_configuration(ServiceDurationMode::ProcessingTime, 30000, 31250, 0);
    rig.start(song, ProcessingMode::Queue);
    rig.services.run_until(400000);
    PlaybackSnapshot mid = rig.engine.snapshot();
    check(mid.state == PlaybackState::Playing && mid.processing_mode == ProcessingMode::Queue, "snapshot state and mode");
    check(mid.queue_length > 0 && mid.outstanding_events == mid.queue_length + 1, "backlog visible while serving");
    check(mid.current_lag_microseconds > 0 && mid.maximum_lag_microseconds >= mid.current_lag_microseconds, "lag tracked");
    check(mid.processing_microseconds == 30000 && mid.simulate_slowdown, "settings reported");
    rig.run([&] { rig.engine.reset_statistics(); return EngineError(); });
    PlaybackSnapshot reset = rig.engine.snapshot();
    check(reset.processed_events == 0 && reset.maximum_lag_microseconds == reset.current_lag_microseconds,
        "reset statistics");
    rig.run([&] { return rig.engine.stop(); });
    check(rig.engine.snapshot().queue_length == 0 && rig.engine.snapshot().outstanding_events == 0,
        "stop clears the queue figures");
    EngineError e = rig.engine.unload();
    check(e.ok() && !rig.engine.has_attached_song() && !rig.engine.has_attached_output(), "unload detaches");
}

// Every message shape the compact store holds: one-, two- and three-byte channel messages, a very short SysEx, a
// long SysEx, and a dense run of notes; several at one instant.
std::vector<TrackEvent> mixed_shape_events()
{
    std::vector<TrackEvent> events = basic_events();
    events.push_back({ 0, { 0xD0, 40 } });
    events.push_back({ 0, { 0xE0, 0, 64 } });
    events.push_back({ 0, { 0xA0, 60, 20 } });
    events.push_back({ 0, { 0xF0, 0x01, 0xF7 } });
    events.push_back({ 48, { 0xB1, 0x0A, 33 } });
    events.push_back({ 48, { 0xC1, 9 } });
    std::vector<TrackEvent> dense = dense_events(150);
    events.insert(events.end(), dense.begin(), dense.end());
    std::stable_sort(events.begin(), events.end(), [](const TrackEvent& a, const TrackEvent& b) { return a.tick < b.tick; });
    return events;
}

// Register entry DD-092: with channel monitoring on, the immediate burst records a group of sends at once (taken
// from the stored form on the direct path). The channel monitor must end exactly where recording every sent event one
// by one (v1 RecordSuccessful) ends, on both paths.
bool same_tracked_state(const engine::MidiChannelSnapshot& a, const engine::MidiChannelSnapshot& b)
{
    return a.channel == b.channel && a.keys_down == b.keys_down && a.peak_keys_down == b.peak_keys_down &&
        a.sent_events == b.sent_events && a.bank_msb == b.bank_msb && a.bank_lsb == b.bank_lsb && a.program == b.program &&
        a.volume == b.volume && a.expression == b.expression && a.pan == b.pan && a.sustain == b.sustain &&
        a.pitch_bend == b.pitch_bend && a.channel_pressure == b.channel_pressure &&
        a.last_dispatched_microseconds == b.last_dispatched_microseconds &&
        a.historical_attribute_mask == b.historical_attribute_mask && a.last_position_historical == b.last_position_historical;
}

void check_monitor_matches_reference(const LoadedSong& song, Rig& observed, Rig& direct)
{
    engine::ChannelStateTracker reference;
    for (int32_t i = 0; i < song.playback->events.count(); ++i) reference.record_successful(song.playback->events.at(i));
    std::shared_ptr<engine::ChannelPlaybackSnapshot> expected = reference.create_snapshot();
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> a = observed.engine.channel_snapshot();
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> b = direct.engine.channel_snapshot();
    bool same = a != nullptr && b != nullptr;
    for (int channel = 0; same && channel < 16; ++channel)
        same = same_tracked_state(a->channels[static_cast<size_t>(channel)], expected->channels[static_cast<size_t>(channel)]) &&
            same_tracked_state(b->channels[static_cast<size_t>(channel)], expected->channels[static_cast<size_t>(channel)]);
    check(same, "the channel monitor records every sent event as recording them one by one does (DD-092)");

    // The two ways of reading a sent message give the same record for every stored message shape.
    bool sameRecords = true;
    const EventStore& store = song.song->store;
    for (int64_t i = 0; sameRecords && i < store.count(); ++i) {
        const PackedRecord& record = store.records.packed_at(i);
        if (record.is_long() || record.kind() == EventKind::SystemExclusive) continue;
        engine::ChannelStateTracker::SentRecord x = engine::ChannelStateTracker::sent_record(record);
        engine::ChannelStateTracker::SentRecord y = engine::ChannelStateTracker::sent_record(models::EventView::from_store(store, i));
        sameRecords = x.micros == y.micros && x.channel == y.channel && x.status == y.status && x.first == y.first && x.second == y.second;
    }
    check(sameRecords, "a stored short message gives the same monitor record as its event view");
}

// Register entry DD-093: the snapshot's eligible count after a moved start, from the per-block counts, against v1's
// algorithm (a full rescan whenever the start moves) for long random call sequences.
struct ReferenceEligibleCounter {   // the port of v1 CountSnapshotEligible before DD-093
    int32_t start = -1, end = -1, first = -1, routing = -1, override_ = -1;
    int64_t total = 0;
    template <typename Eligible>
    int64_t count(int32_t s, int32_t e, bool active, int32_t r, int32_t o, Eligible eligible)
    {
        if (s >= e) { first = -1; return 0; }
        if (!active) { first = s; return static_cast<int64_t>(e) - s; }
        int32_t scan;
        int64_t c;
        if (start == s && end >= s && end <= e && routing == r && override_ == o) { scan = end; c = total; }
        else { scan = s; c = 0; first = -1; start = s; routing = r; override_ = o; }
        for (int32_t i = scan; i < e; ++i) {
            if (!eligible(i)) continue;
            if (first < 0) first = i;
            ++c;
        }
        end = e;
        total = c;
        return c;
    }
    void reset() { start = -1; end = -1; total = 0; first = -1; }
};

void test_snapshot_eligible_counter()
{
    g_scenario = "snapshot eligible count (DD-093)";
    const int32_t space = 60000;
    int32_t generation = 0;
    // Generation-dependent filters with long runs of filtered events (whole blocks of 4,096 and more), as a forced
    // override on a busy channel gives.
    auto eligible = [&generation](int32_t index) {
        uint32_t h = static_cast<uint32_t>(index) * 2654435761u ^ static_cast<uint32_t>(generation) * 40503u;
        if (((index / 5000) + generation) % 4 == 0) return false;
        return (h >> 7) % 5 != 0;
    };
    engine::SnapshotEligibleCounter counter;
    ReferenceEligibleCounter reference;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto next = [&seed](uint32_t range) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return static_cast<int32_t>(seed % range);
    };
    int32_t s = 0, e = 0;
    bool active = true;
    bool same = true;
    int steps = 0;
    for (; steps < 4000 && same; ++steps) {
        int choice = next(100);
        if (choice < 45) s = std::min(space, s + next(choice < 15 ? 6000 : 300));          // the worker sends
        else if (choice < 85) e = std::min(space, e + next(choice < 60 ? 9000 : 200));     // events become due
        else if (choice < 88) s = next(static_cast<uint32_t>(space));                      // seek / rewind
        else if (choice < 91) e = next(static_cast<uint32_t>(space));
        else if (choice < 94) ++generation;                                                // the user changes a filter
        else if (choice < 96) active = !active;
        else if (choice < 97) { counter.reset(); reference.reset(); }                      // statistics reset
        else { s = 0; e = 0; }
        if (s >= space && e >= space) { s = next(1000); e = s + next(20000); }
        int32_t g = generation;
        int64_t a = counter.count(s, e, active, g, g * 3, eligible);
        int64_t b = reference.count(s, e, active, g, g * 3, eligible);
        same = a == b && counter.first_eligible() == reference.first;
    }
    check(same, "the same count and first eligible event as v1's rescan for every call");
    check(steps == 4000, "the random sequence ran to its end");
}

// Register entry DD-065: with no send observer (and monitoring off) the engine sends straight from the stored
// events instead of building an event view first. The messages, their order and times, and the statistics must be
// the same as on the observed path, for every message shape, with and without send cost, and with monitoring on.
void test_direct_send_equivalence()
{
    g_scenario = "direct send (DD-065)";
    LoadedSong song = load(mixed_shape_events());
    for (int64_t cost : { int64_t(0), int64_t(320) }) {
        for (int monitoring = 0; monitoring < 2; ++monitoring) {
            Rig observed(cost, true);
            Rig direct(cost, false);
            observed.engine.set_channel_monitoring(monitoring != 0);
            direct.engine.set_channel_monitoring(monitoring != 0);
            observed.start(song, ProcessingMode::Queue);
            direct.start(song, ProcessingMode::Queue);
            observed.play_to_end();
            direct.play_to_end();
            bool same = observed.output.sent.size() == direct.output.sent.size() && !direct.output.sent.empty();
            for (size_t i = 0; same && i < observed.output.sent.size(); ++i) {
                const Sent& a = observed.output.sent[i];
                const Sent& b = direct.output.sent[i];
                same = a.time == b.time && a.kind == b.kind && a.packed == b.packed && a.bytes == b.bytes;
            }
            check(same, "the direct path sends the same messages at the same times as the observed path");
            PlaybackSnapshot a = observed.engine.snapshot();
            PlaybackSnapshot b = direct.engine.snapshot();
            check(a.processed_events == b.processed_events && a.processed_events == song.song->store.count(),
                "the same number of events is processed");
            check(a.dropped_events == b.dropped_events && a.current_lag_microseconds == b.current_lag_microseconds &&
                a.maximum_lag_microseconds == b.maximum_lag_microseconds &&
                a.last_dispatched_timeline_microseconds == b.last_dispatched_timeline_microseconds,
                "the same lag and dispatch statistics");
            check(direct.ended.load() == 1 && direct.failed.load() == 0, "the direct run ends normally");
            if (monitoring != 0) check_monitor_matches_reference(song, observed, direct);
        }
    }

    // The same with the Drop mode (its unfiltered burst) and a failing output.
    Rig observedDrop(0, true);
    Rig directDrop(0, false);
    observedDrop.start(song, ProcessingMode::Drop);
    directDrop.start(song, ProcessingMode::Drop);
    observedDrop.play_to_end();
    directDrop.play_to_end();
    check(observedDrop.output.sent.size() == directDrop.output.sent.size() &&
        observedDrop.engine.snapshot().processed_events == directDrop.engine.snapshot().processed_events,
        "Drop mode: the direct path sends the same messages");

    Rig failing(0, false);
    failing.output.fail_at_ = 5;
    failing.start(song, ProcessingMode::Queue);
    failing.play_to_end();
    check(failing.failed.load() == 1 && failing.failure.message == "Injected send failure.",
        "a send failure on the direct path ends playback with the output's message");
}

double seconds_since(const win32::QpcClock& clock, int64_t start);

// Register entry DD-065: the lag statistics of a burst. One hundred events are due at the same instant and each send
// takes `cost` microseconds of virtual time, so the event sent last is `100 * cost` late: that is the maximum lag.
// The clock is read once per group of events (the group is longer the cheaper the send), and the lag of the last
// event of the burst must come out exactly the same for every group size. The same test passes on the engine as it
// was before that change (a clock read after every send).
void test_burst_lag_statistics()
{
    g_scenario = "burst lag (DD-065)";
    std::vector<TrackEvent> events;
    for (int i = 0; i < 100; ++i) events.push_back({ 0, { 0x90, static_cast<uint8_t>(30 + i % 60), 100 } });
    LoadedSong song = load(events);
    for (int64_t cost : { int64_t(0), int64_t(1), int64_t(3), int64_t(7), int64_t(320) }) {
        for (int observe = 0; observe < 2; ++observe) {
            Rig rig(cost, observe != 0);
            rig.start(song, ProcessingMode::Queue);
            rig.play_to_end();
            PlaybackSnapshot s = rig.engine.snapshot();
            check(rig.ended.load() == 1 && rig.failed.load() == 0, "the run ends normally");
            check(s.processed_events == 100, "all events are processed");
            check(s.maximum_lag_microseconds == 100 * cost, "the last event of the burst, sent 100 sends late, sets the maximum lag");        }
    }
}

// An output that blocks inside its Nth short send until released.
class BlockAtOutput : public platform::NullMidiOutput {
public:
    explicit BlockAtOutput(int blockAt) : blockAt_(blockAt) {}
    platform::OutputStatus send_short(uint32_t) override
    {
        if (++sends_ == blockAt_) {
            entered.store(true);
            release_.wait(30000);
        }
        return platform::OutputStatus::success();
    }
    void release() { release_.set(); }
    std::atomic<bool> entered{ false };

private:
    int blockAt_;
    std::atomic<int> sends_{ 0 };
    win32::Win32WakeEvent release_;
};

// Register entry DD-065: the burst publishes the event being sent without the engine lock. A reader that looks
// while the output call is blocked must see the same picture as before: the event in service, and everything
// already due behind it as the queue.
void test_burst_snapshot_while_send_blocked()
{
    g_scenario = "burst snapshot (DD-065)";
    std::vector<TrackEvent> events;
    for (int i = 0; i < 40; ++i) events.push_back({ 0, { 0x90, static_cast<uint8_t>(30 + i), 100 } });
    LoadedSong song = load(events);
    const int64_t due = song.song->store.count();
    check(due == 40, "forty events, all due at once");
    for (int blockAt : { 1, 3, 17 }) {
        std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();
        std::shared_ptr<BlockAtOutput> output = std::make_shared<BlockAtOutput>(blockAt);
        win32::QpcClock clock;
        PlaybackEngine engine(services);
        EngineError e = engine.start(song.playback, output, ProcessingMode::Queue);
        check(e.ok(), "start");
        int64_t waitStart = clock.now_ticks();
        while (!output->entered.load() && seconds_since(clock, waitStart) < 5) {
            win32::Win32WakeEvent idle;
            idle.wait(1);
        }
        check(output->entered.load(), "the worker is blocked in the output call");
        PlaybackSnapshot s = engine.snapshot();
        // 40 events are due; blockAt - 1 are gone, one is in the output call, the rest wait.
        check(s.outstanding_events == due + 1 - blockAt && s.queue_length == due - blockAt,
            "while a send is blocked the snapshot shows the event in service and the due events behind it");
        output->release();
        win32::Win32WakeEvent done;
        int64_t releaseStart = clock.now_ticks();
        while (engine.state() == PlaybackState::Playing && seconds_since(clock, releaseStart) < 5) done.wait(1);
        PlaybackSnapshot end = engine.snapshot();
        check(end.processed_events == 40 && end.outstanding_events == 0, "released: all events are processed");
    }
}

void test_win32_services_smoke()
{
    g_scenario = "win32 services";
    LoadedSong song = load({ { 0, { 0x90, 60, 100 } }, { 2, { 0x80, 60, 0 } }, { 4, { 0x90, 62, 100 } },
        { 6, { 0x80, 62, 0 } } });
    std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();
    std::shared_ptr<platform::NullMidiOutput> output = std::make_shared<platform::NullMidiOutput>();
    std::atomic<int> ended(0);
    {
        PlaybackEngine engine(services);
        engine.set_playback_ended_handler([&] { ended++; });
        EngineError e = engine.start(song.playback, output, ProcessingMode::Queue);
        check(e.ok(), "real-time start");
        win32::QpcClock clock;
        int64_t deadline = clock.now_ticks() + clock.frequency() * 5;
        while (engine.state() == PlaybackState::Playing && clock.now_ticks() < deadline) {
            win32::Win32WakeEvent idle;
            idle.wait(2);
        }
        check(engine.state() == PlaybackState::Completed && ended.load() == 1, "real-time playback completes");
        check(engine.snapshot().processed_events == 4, "real-time playback sends everything");
    }
}

// An output whose first short send blocks (like a hung driver call) until
// the test releases it.
class BlockingOutput : public platform::NullMidiOutput {
public:
    platform::OutputStatus send_short(uint32_t) override
    {
        if (!blocked_once_.exchange(true)) {
            entered.store(true);
            release_.wait(30000);
        }
        sends++;
        return platform::OutputStatus::success();
    }
    platform::OutputStatus reset() override
    {
        resets++;
        return platform::OutputStatus::success();
    }
    void release() { release_.set(); }
    std::atomic<bool> entered{ false };
    std::atomic<int> sends{ 0 };
    std::atomic<int> resets{ 0 };

private:
    std::atomic<bool> blocked_once_{ false };
    win32::Win32WakeEvent release_;
};

double seconds_since(const win32::QpcClock& clock, int64_t start)
{
    return static_cast<double>(clock.now_ticks() - start) / static_cast<double>(clock.frequency());
}

// v1 abandons a worker blocked in an output call after its bounded waits:
// Stop and Dispose return after v1's 2000 ms timeout with v1's text, and the
// released worker later finishes against memory it still owns.
void test_hung_output_abandoned()
{
    g_scenario = "hung output";
    LoadedSong song = load({ { 0, { 0x90, 60, 100 } }, { 2, { 0x80, 60, 0 } }, { 4, { 0x90, 62, 100 } },
        { 6, { 0x80, 62, 0 } } });
    std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();
    std::shared_ptr<BlockingOutput> output = std::make_shared<BlockingOutput>();
    win32::QpcClock clock;
    std::unique_ptr<PlaybackEngine> engine(new PlaybackEngine(services));
    EngineError e = engine->start(song.playback, output, ProcessingMode::Queue);
    check(e.ok(), "start");
    int64_t waitStart = clock.now_ticks();
    while (!output->entered.load() && seconds_since(clock, waitStart) < 5) {
        win32::Win32WakeEvent idle;
        idle.wait(1);
    }
    check(output->entered.load(), "worker is blocked in the output call");

    int64_t stopStart = clock.now_ticks();
    e = engine->stop();
    double stopSeconds = seconds_since(clock, stopStart);
    check(e.type == "PlaybackWorkerTimeoutException" &&
        e.message == "The MIDI output call did not return within 2000 ms while stopping playback. Playback remains "
            "stopped and no replacement scheduler was started. The output is not reset concurrently with its active "
            "call; a final reset and all-notes-off panic will run after that call returns.",
        "Stop times out with v1's text");
    check(stopSeconds >= 1.9 && stopSeconds < 4.0, "Stop returns after v1's 2000 ms wait");
    check(engine->state() == PlaybackState::Stopped && engine->has_live_worker(), "stopped, worker still blocked");

    int64_t disposeStart = clock.now_ticks();
    engine.reset();  // v1 Dispose: Unload -> Stop times out again, then the worker is abandoned
    double disposeSeconds = seconds_since(clock, disposeStart);
    check(disposeSeconds >= 1.9 && disposeSeconds < 4.0, "Dispose returns after v1's 2000 ms wait");
    check(output.use_count() > 1, "the abandoned worker still owns the output");

    output->release();
    int64_t releaseStart = clock.now_ticks();
    while (output.use_count() > 1 && seconds_since(clock, releaseStart) < 5) {
        win32::Win32WakeEvent idle;
        idle.wait(1);
    }
    check(output.use_count() == 1, "the released worker finished and freed the engine");
    check(output->resets.load() == 1, "the worker's final reset ran after the call returned");
}

// A worker thread that cannot be started (v1: Thread.Start throws).
class FailingStartServices : public lockstep::LockStepEngineServices {
public:
    std::unique_ptr<platform::Thread> start_thread(std::function<void()>, platform::ThreadPriority, const char*,
        platform::PlatformError& error) override
    {
        error.type = "OutOfMemoryException";
        error.message = messages::playback_thread_not_started();
        return nullptr;
    }
};

void test_thread_start_failure()
{
    g_scenario = "thread start failure";
    LoadedSong song = load(basic_events());
    std::shared_ptr<FailingStartServices> services = std::make_shared<FailingStartServices>();
    std::shared_ptr<TestOutput> output = std::make_shared<TestOutput>(services.get());
    PlaybackEngine engine(services);
    EngineError e = engine.start(song.playback, output, ProcessingMode::Queue);
    check(e.type == "OutOfMemoryException" && e.message == messages::playback_thread_not_started(),
        "start failure says that Windows could not start the playback thread and what to do");
    check(engine.state() == PlaybackState::Stopped && !engine.has_live_worker(),
        "DD-045: the engine is Stopped when the thread cannot be started (v1 stayed Playing with no worker)");
    e = engine.stop();
    check(e.ok(), "DD-003: Stop after the failed start stops cleanly (v1: ThreadStateException \"Thread has not been started.\")");
    check(engine.state() == PlaybackState::Stopped, "stopped");
    e = engine.stop();
    check(e.ok(), "a second Stop is harmless");
}


// A v1 defect fixed by DD-006 (ENGINE_PORT_NOTES.md; ENGINE_SCENARIOS.md section 11): in v1, on the ordered
// path, clearing the last override before the worker applied the pending one leaves the pending
// mask set. All ordered workers then stop admitting source events and loop at the top of their loop reading the
// clock, so playback freezes with the worker spinning. Repro: ordered path, Pause, override 0 volume 20,
// override 0 volume auto (both while paused: the paused worker applies channel controls but not overrides),
// Resume. Under the lock-step harness's virtual clock v1's spin ended in the clock-read livelock guard (5,000,000
// reads at one virtual time), which v1's engine reports as PlaybackFailed; the classic path was never affected.
// The same repro ran against the real v1 engine in the v1 spike (evidence-ordered-override-livelock).
// v2 cancels the pending message when the override is cleared, so the run plays to the end.
void test_ordered_override_set_clear_while_paused()
{
    g_scenario = "ordered stale override mask (DD-006)";
    const char* text =
        "format 1\n"
        "scenario repro-ordered\n"
        "  snapshots on\n"
        "  set preserve-playback-queue-on-pause-seek on\n"
        "  at 0 start queue 0\n"
        "  at 2/10 pause\n"
        "  at 3/10 override 0 volume 20\n"
        "  at 4/10 override 0 volume auto\n"
        "  at 5/10 resume\n"
        "scenario repro-classic\n"
        "  snapshots on\n"
        "  at 0 start queue 0\n"
        "  at 2/10 pause\n"
        "  at 3/10 override 0 volume 20\n"
        "  at 4/10 override 0 volume auto\n"
        "  at 5/10 resume\n";
    std::vector<harness::ScenarioDefinition> scenarios;
    std::string parseError;
    check(harness::load_scenario_file(text, "inline", scenarios, parseError) && scenarios.size() == 2, "repro scenarios parse");
    if (scenarios.size() != 2) return;
    LoadedSong song = load(basic_events());

    harness::EngineRunResult ordered;
    harness::run_engine_scenario(song.song, "basic.mid", scenarios[0], ordered);
    // DD-006: v1 left the queued message flagged, the ordered workers waited for it and froze; v2 cancels it when
    // the override is cleared, so the ordered path plays to the end like the classic path.
    check(ordered.ok() && ordered.out.find("PlaybackFailed") == std::string::npos &&
        ordered.out.find(" W EVENT PlaybackEnded\n") != std::string::npos,
        "DD-006: ordered path: Resume after set+clear while paused plays to the end");
    size_t resumeAt = ordered.out.find(" H CMD Resume\n");
    check(resumeAt != std::string::npos && ordered.out.find(" SEND ", resumeAt) != std::string::npos,
        "DD-006: ordered path: events are sent after Resume");

    harness::EngineRunResult classic;
    harness::run_engine_scenario(song.song, "basic.mid", scenarios[1], classic);
    check(classic.ok() && classic.out.find("PlaybackFailed") == std::string::npos &&
        classic.out.find(" W EVENT PlaybackEnded\n") != std::string::npos, "classic path: it plays to the end (as before)");
}
} // namespace

#define RUN(test) do { std::printf("-- %s\n", #test); test(); } while (0)

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RUN(test_time_conversion);
    RUN(test_direct_send_equivalence);
    RUN(test_snapshot_eligible_counter);
    RUN(test_burst_lag_statistics);
    RUN(test_burst_snapshot_while_send_blocked);
    RUN(test_play_to_end);
    RUN(test_stop_restart_chase);
    RUN(test_pause_resume_classic);
    RUN(test_seek_classic);
    RUN(test_drop_mode);
    RUN(test_clear_buffer_filters_muted_channel);
    RUN(test_start_errors);
    RUN(test_failure_event);
    RUN(test_overrides_and_disabled_channels);
    RUN(test_live_controls_while_playing);
    RUN(test_gates);
    RUN(test_service_models);
    RUN(test_ordered);
    RUN(test_channel_control_failure);
    RUN(test_determinism);
    RUN(test_fixed_v1_defects);
    RUN(test_statistics);
    RUN(test_win32_services_smoke);
    RUN(test_ordered_override_set_clear_while_paused);
    RUN(test_thread_start_failure);
    RUN(test_hung_output_abandoned);
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
