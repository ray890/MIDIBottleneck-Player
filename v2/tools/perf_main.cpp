// midibn-perf: one timed load in a fresh process; key=value output that
// matches the v1 harness `perf-load` keys.
//
//   midibn-perf <midi>               load only
//   midibn-perf --analysis <midi>    load, then three timed Analysis runs with
//                                    fixed configurations (measurement only)
//   midibn-perf --micro <midi>       load, then time small simple workloads (event views, copies, 64-bit
//                                    division, double conversion) for build-to-build A/B comparisons
//   midibn-perf --stages <midi>      load with the time spent in each load stage
//                                    (options may be combined)
//   midibn-perf --routing <midi>    load, then time track-routing compile_route with default settings
//   midibn-perf --engine <midi>      load, then play the song to its end in Queue mode on the real engine
//                                    under the lock-step virtual clock into a recording no-device output;
//                                    prints wall time and events per second (see run_engine)
//   midibn-perf --engine-real <midi> load, then play the song on the real engine with the REAL clock sped up
//                                    1000-fold (all events are due at once, so the engine, not the song, sets
//                                    the pace) into the same counting output; prints events per second
//                                    (--null-output: into an output that does nothing, the engine's own cost;
//                                    --repeat=N: N plays after one load, one result block each;
//                                    --profile: also sample where the threads spend their time, see
//                                    sampling_profiler.h; needs a build with symbols; with --analysis it
//                                    samples the queue and gates runs instead; does nothing in XP builds)
//
// The Analysis mode runs midibn::analysis::analyze at the default resolution
// for the song, each time with a fresh cache so the base scan is included:
//   base    no configuration: the workload scan only
//   queue   queue projection: slowdown simulation, 1024-event and 5 ms limits
//   gates   gate projection: Per-note interval, Delayed NoteOff, Note start
//           rate and Output rate gates all on (all four gate projections)
// It prints each run's time and a hash of every result field (including the
// bit patterns of the doubles), so two builds can be compared bit for bit.
#include "midibn/analysis/workload_analysis.h"
#include "midibn/engine/playback_engine.h"
#include "midibn/midi_loader.h"
#include "midibn/null_output.h"
#include "midibn/routing/track_routing.h"
#include "lockstep_services.h"
#include "sampling_profiler.h"
#include "tool_support.h"
#include "win32_timing.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

using namespace midibn;

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(uint64_t v)
    {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ull; }
    }
    void add_double(double d)
    {
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        add(bits);
    }
};

std::string result_hash(const analysis::WorkloadAnalysis& a, uint64_t& doubleHash)
{
    Fnv all, doubles;
    all.add(static_cast<uint64_t>(a.total_events));
    all.add(static_cast<uint64_t>(a.total_bytes));
    all.add(static_cast<uint64_t>(a.unique_timestamps));
    all.add(static_cast<uint64_t>(a.largest_timestamp_cluster));
    all.add(static_cast<uint64_t>(a.events_in_clusters_at_least_2));
    all.add(static_cast<uint64_t>(a.events_in_clusters_at_least_10));
    all.add(static_cast<uint64_t>(a.events_in_clusters_at_least_50));
    all.add(static_cast<uint64_t>(a.events_in_clusters_at_least_100));
    all.add(static_cast<uint64_t>(a.bucket_us));
    all.add(static_cast<uint64_t>(a.duration_us));
    for (const analysis::WorkloadBucket& b : a.buckets) {
        all.add(static_cast<uint64_t>(b.event_count));
        all.add(static_cast<uint64_t>(b.largest_cluster));
        all.add(static_cast<uint64_t>(b.byte_count));
        all.add(static_cast<uint64_t>(b.service_demand_us));
        all.add(static_cast<uint64_t>(b.predicted_peak_queue_age_us));
        all.add(static_cast<uint64_t>(b.predicted_peak_occupancy));
        all.add(static_cast<uint64_t>(b.predicted_dropped_events));
        all.add(static_cast<uint64_t>(b.predicted_buffer_clears));
    }
    all.add(static_cast<uint64_t>(a.predicted_maximum_occupancy));
    all.add(static_cast<uint64_t>(a.predicted_peak_queue_age_us));
    all.add(static_cast<uint64_t>(a.predicted_dropped_events));
    all.add(static_cast<uint64_t>(a.predicted_buffer_clears));
    all.add(static_cast<uint64_t>(a.predicted_output_completion_us));
    for (int64_t v : a.gate_output_buckets) all.add(static_cast<uint64_t>(v));
    all.add(static_cast<uint64_t>(a.gate_output_events));
    all.add(static_cast<uint64_t>(a.gate_output_note_transitions));
    all.add(static_cast<uint64_t>(a.gate_output_non_note_events));
    all.add(static_cast<uint64_t>(a.gate_filtered_note_events));
    all.add(static_cast<uint64_t>(a.musical_limit_rejected_attacks));
    all.add(static_cast<uint64_t>(a.musical_limit_suppressed_releases));
    all.add(static_cast<uint64_t>(a.output_rate_rejected_attacks));
    all.add(static_cast<uint64_t>(a.output_rate_suppressed_releases));
    all.add(static_cast<uint64_t>(a.output_rate_rejected_other_messages));
    all.add(static_cast<uint64_t>(a.output_rate_safety_soft_excess));
    all.add(static_cast<uint64_t>(a.gate_output_completion_us));
    all.add(static_cast<uint64_t>(a.projection_state));
    doubles.add_double(a.average_events_per_second);
    doubles.add_double(a.peak_events_per_second);
    doubles.add_double(a.peak_bytes_per_second);
    doubles.add_double(a.event_service_capacity_per_second);
    doubles.add_double(a.byte_service_capacity_per_second);
    doubles.add_double(a.gate_peak_events_per_second);
    doubleHash = doubles.h;
    char text[40];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(all.h));
    return text;
}

void run_analysis(const char* name, const Song& song, std::shared_ptr<const analysis::AnalysisConfiguration> configuration,
    bool profile = false)
{
    analysis::AnalysisSong input;
    input.events = models::EventSource(&song.store);
    input.duration_us = song.duration_us;
    int64_t bucket = analysis::default_resolution(song.duration_us);
    analysis::WorkloadCache cache;
    analysis::AnalysisControl control;
    tools::SamplingProfiler profiler;
    // --profile with --analysis: the Analysis runs on this thread, so no thread is left out (0).
    if (profile) profiler.start(0, 100);
    int64_t start = win32::performance_counter();
    analysis::WorkloadAnalysis result = analysis::analyze(input, bucket, configuration, cache, control);
    double elapsed = win32::milliseconds_since(start);
    if (profile) {
        profiler.stop();
        std::printf("profile-analysis=%s\n", name);
        if (!profiler.report(30)) std::printf("profile-unavailable=dbghelp could not be loaded\n");
    }
    uint64_t doubleHash = 0;
    std::string hash = result_hash(result, doubleHash);
    std::printf("analysis-%s-ms=%.1f\n", name, elapsed);
    std::printf("analysis-%s-buckets=%lld\n", name, static_cast<long long>(result.buckets.size()));
    std::printf("analysis-%s-state=%d\n", name, static_cast<int>(result.projection_state));
    std::printf("analysis-%s-hash=%s\n", name, hash.c_str());
    std::printf("analysis-%s-doubles-hash=%016llx\n", name, static_cast<unsigned long long>(doubleHash));
}

// --micro: small, simple workloads timed one by one, to locate where one x86 build (for instance the XP release
// build, /arch:IA32) is slower than another (the SSE2 build). Each prints its time and a checksum (the checksums must
// be equal between builds). See XP_COST_LOG.md, 2026-10-16.
void run_micro(const Song& song)
{
    using models::EventSource;
    using models::EventView;
    EventSource events(&song.store);
    const int32_t count = events.count();
    volatile uint32_t sink = 0;
    auto timed = [&](const char* name, auto&& body) {
        int64_t start = win32::performance_counter();
        uint64_t check = body();
        double elapsed = win32::milliseconds_since(start);
        sink = sink + static_cast<uint32_t>(check);
        std::printf("micro-%s-ms=%.1f\nmicro-%s-check=%016llx\n", name, elapsed, name, static_cast<unsigned long long>(check));
    };
    // Reading every event as a decoded 48-byte view (what the Analysis projections and the gates do per event).
    timed("view-read", [&]() -> uint64_t {
        uint64_t sum = 0;
        for (int32_t i = 0; i < count; ++i) {
            EventView v = events.at(i);
            sum += static_cast<uint64_t>(v.micros) + v.data_length + v.bytes[1];
        }
        return sum;
    });
    // Reading only the time of every event (no view).
    timed("micros-read", [&]() -> uint64_t {
        uint64_t sum = 0;
        for (int32_t i = 0; i < count; ++i) sum += static_cast<uint64_t>(events.micros_at(i));
        return sum;
    });
    // Copying views between two arrays of 128 (the gates' per-pitch arrays and emission buffers).
    timed("view-copy", [&]() -> uint64_t {
        std::vector<EventView> a(128), b(128);
        for (int32_t i = 0; i < 128; ++i) a[static_cast<size_t>(i)] = events.at(i % std::max(1, count));
        uint64_t sum = 0;
        for (int round = 0; round < 40000; ++round) {
            for (size_t i = 0; i < 128; ++i) b[i] = a[(i + static_cast<size_t>(round)) & 127];
            sum += static_cast<uint64_t>(b[static_cast<size_t>(round) & 127].micros);
            std::swap(a, b);
        }
        return sum;
    });
    // The same copies with a plain 48-byte structure that keeps the compiler's own copy (what EventView did on every
    // build before DD-095; on /arch:IA32 a `rep movs`).
    timed("pod48-copy", [&]() -> uint64_t {
        struct Pod48 { int64_t a, micros; int32_t c[8]; };
        static_assert(sizeof(Pod48) == 48, "48 bytes like EventView");
        std::vector<Pod48> a(128), b(128);
        for (int32_t i = 0; i < 128; ++i) {
            a[static_cast<size_t>(i)] = Pod48();
            a[static_cast<size_t>(i)].micros = events.micros_at(i % std::max(1, count));
        }
        uint64_t sum = 0;
        for (int round = 0; round < 40000; ++round) {
            for (size_t i = 0; i < 128; ++i) b[i] = a[(i + static_cast<size_t>(round)) & 127];
            sum += static_cast<uint64_t>(b[static_cast<size_t>(round) & 127].micros);
            std::swap(a, b);
        }
        return sum;
    });
    // 64-bit division and remainder by a run-time divisor (a library call on every 32-bit build).
    timed("div64", [&]() -> uint64_t {
        int64_t divisor = 5000 + (count & 1);
        uint64_t sum = 0;
        for (int32_t i = 0; i < count; ++i) {
            int64_t m = events.micros_at(i);
            sum += static_cast<uint64_t>(m / divisor) + static_cast<uint64_t>(m % divisor);
        }
        return sum;
    });
    // Integer to double and back, and double division (x87 on /arch:IA32, SSE2 otherwise).
    timed("double-convert", [&]() -> uint64_t {
        double acc = 0;
        uint64_t sum = 0;
        for (int32_t i = 0; i < count; ++i) {
            double seconds = static_cast<double>(events.micros_at(i)) / 1000000.0;
            acc += seconds;
            sum += static_cast<uint64_t>(static_cast<int64_t>(seconds * 1000.0));
        }
        uint64_t bits;
        std::memcpy(&bits, &acc, sizeof bits);
        return sum ^ bits;
    });
}

// --stages: wall time between the first progress report of each load stage
// (the monitor adds a small, constant cost per report).
struct StageTimer : midibn::LoadMonitor {
    int64_t start = 0;
    std::vector<std::pair<std::string, int64_t>> firsts;
    void progress(const midibn::LoadProgress& p) override
    {
        if (firsts.empty() || firsts.back().first != p.stage) firsts.push_back({ p.stage, win32::performance_counter() });
    }
    bool cancellation_requested() override { return false; }
    void print(int64_t end) const
    {
        for (size_t i = 0; i < firsts.size(); ++i) {
            int64_t stop = i + 1 < firsts.size() ? firsts[i + 1].second : end;
            if (stop < firsts[i].second) continue;
            std::string key = firsts[i].first;
            for (char& c : key) if (c == ' ') c = '-';
            std::printf("stage-%s-ms=%.1f\n", key.c_str(), (stop - firsts[i].second) * 1000.0 / frequency());
        }
    }
    static double frequency()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }
};

// --engine: plays the whole song to its end in Queue mode (default settings: Simulate slowdown with a
// 0 us processing time, so every due event is sent immediately) on the real engine and its real worker
// thread, under the lock-step virtual clock (platform/virtual/lockstep_services.h), into a recording
// no-device output that counts what it receives and hashes it. Virtual time moves only when the driver
// releases the worker's timed waits, so the wall time measures the engine's own work (admission,
// dispatch, publication) plus the thread hand-off at each timed wait, never a real sleep. The hash lets
// two builds be compared for identical output.
class CountingOutput : public platform::NullMidiOutput {
public:
    uint64_t shorts = 0, longs = 0;
    Fnv hash;
    platform::OutputStatus send_short(uint32_t packed) override
    {
        ++shorts;
        hash.add(packed);
        return platform::OutputStatus::success();
    }
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override
    {
        ++longs;
        hash.add(0x100000000ull | fragment.length);
        for (size_t i = 0; i < fragment.length; ++i) hash.add(fragment.data[i]);
        return platform::OutputStatus::success();
    }
};

int run_engine(const Song& song, const std::string& path)
{
    std::shared_ptr<lockstep::LockStepEngineServices> services = std::make_shared<lockstep::LockStepEngineServices>(120000);
    std::shared_ptr<CountingOutput> output = std::make_shared<CountingOutput>();
    std::atomic<int> ended(0), failed(0);
    std::string failure;
    engine::PlaybackSnapshot snapshot;
    engine::EngineError error;
    bool idle = false;
    double elapsed = 0;
    {
        engine::PlaybackEngine playback(services);
        playback.set_playback_ended_handler([&] { ended++; });
        playback.set_playback_failed_handler([&](const engine::EngineError& e) { failure = e.message; failed++; });
        std::shared_ptr<engine::PlaybackSong> playbackSong = engine::make_playback_song(song, path, nullptr);
        int64_t start = win32::performance_counter();
        services->command([&] { error = playback.start(playbackSong, output, engine::ProcessingMode::Queue, 0, false); });
        idle = services->run_until_idle();
        elapsed = win32::milliseconds_since(start);
        snapshot = playback.snapshot();
    }
    std::printf("engine-mode=queue-lockstep-virtual-clock\n");
    std::printf("engine-start-ok=%d\n", error.ok() ? 1 : 0);
    std::printf("engine-finished=%d\n", idle && !services->hung() && ended.load() == 1 && failed.load() == 0 ? 1 : 0);
    if (failed.load() != 0) std::printf("engine-failure=%s\n", failure.c_str());
    std::printf("engine-wall-ms=%.1f\n", elapsed);
    std::printf("engine-events-processed=%lld\n", static_cast<long long>(snapshot.processed_events));
    std::printf("engine-events-per-second=%.0f\n", elapsed > 0 ? snapshot.processed_events * 1000.0 / elapsed : 0.0);
    std::printf("engine-virtual-us=%lld\n", static_cast<long long>(services->now()));
    std::printf("engine-timed-waits=%lld\n", static_cast<long long>(services->parks()));
    std::printf("engine-spins=%lld\n", static_cast<long long>(services->spins()));
    std::printf("engine-output-short=%llu\n", static_cast<unsigned long long>(output->shorts));
    std::printf("engine-output-long=%llu\n", static_cast<unsigned long long>(output->longs));
    std::printf("engine-output-hash=%016llx\n", static_cast<unsigned long long>(output->hash.h));
    return error.ok() && idle && ended.load() == 1 && failed.load() == 0 ? 0 : 1;
}

// --engine-real: the real Win32 services (QueryPerformanceCounter, kernel events, waitable timer, worker thread
// at above-normal priority) with the clock running 1000 times faster than real time, so a song of minutes is
// due within a fraction of a second and the worker is busy all the time: wall time per event is the engine's own
// per-event cost (locks, clock reads, publication, dispatch) without the lock-step hand-offs.
class FastClock : public platform::Clock {
public:
    FastClock() : origin_(base_.now_ticks()) {}
    int64_t now_ticks() const override { return origin_ + (base_.now_ticks() - origin_) * 1000; }
    int64_t frequency() const override { return base_.frequency(); }
private:
    win32::QpcClock base_;
    int64_t origin_;
};

class FastServices : public platform::EngineServices {
public:
    platform::Clock& clock() override { return clock_; }
    std::unique_ptr<platform::WakeEvent> create_wake_event(platform::WakeEventRole role, bool initiallySet,
        bool autoReset) override { return inner_.create_wake_event(role, initiallySet, autoReset); }
    std::unique_ptr<platform::Waiter> create_waiter(platform::WakeEvent& wake, platform::PlatformError& error) override
    { return inner_.create_waiter(wake, error); }
    std::unique_ptr<platform::Thread> start_thread(std::function<void()> body, platform::ThreadPriority priority,
        const char* name, platform::PlatformError& error) override
    { return inner_.start_thread(std::move(body), priority, name, error); }
private:
    win32::Win32EngineServices inner_;
    FastClock clock_;
};

int run_engine_real(const Song& song, const std::string& path, bool nullOutput = false, bool profile = false)
{
    tools::SamplingProfiler profiler;
    std::shared_ptr<FastServices> services = std::make_shared<FastServices>();
    // The counting output hashes every message (about 10 ns each); the null output does nothing, so with it the
    // figure is the engine's own cost per event (register entry DD-065).
    std::shared_ptr<CountingOutput> output = std::make_shared<CountingOutput>();
    std::shared_ptr<platform::MidiOutput> sink = output;
    if (nullOutput) sink = std::make_shared<platform::NullMidiOutput>();
    std::atomic<int> ended(0), failed(0);
    std::string failure;
    engine::PlaybackSnapshot snapshot;
    engine::EngineError error;
    double elapsed = 0;
    {
        engine::PlaybackEngine playback(services);
        playback.set_playback_ended_handler([&] { ended++; });
        playback.set_playback_failed_handler([&](const engine::EngineError& e) { failure = e.message; failed++; });
        std::shared_ptr<engine::PlaybackSong> playbackSong = engine::make_playback_song(song, path, nullptr);
        int64_t start = win32::performance_counter();
        if (profile) profiler.start(GetCurrentThreadId(), 100);
        error = playback.start(playbackSong, sink, engine::ProcessingMode::Queue, 0, false);
        while (error.ok() && ended.load() == 0 && failed.load() == 0 && win32::milliseconds_since(start) < 120000) Sleep(1);
        elapsed = win32::milliseconds_since(start);
        if (profile) profiler.stop();
        snapshot = playback.snapshot();
    }
    if (profile && !profiler.report(25)) std::printf("profile-unavailable=dbghelp could not be loaded\n");
    std::printf("engine-mode=queue-real-clock-x1000%s\n", nullOutput ? "-null-output" : "");
    std::printf("engine-start-ok=%d\n", error.ok() ? 1 : 0);
    std::printf("engine-finished=%d\n", ended.load() == 1 && failed.load() == 0 ? 1 : 0);
    if (failed.load() != 0) std::printf("engine-failure=%s\n", failure.c_str());
    std::printf("engine-wall-ms=%.1f\n", elapsed);
    std::printf("engine-events-processed=%lld\n", static_cast<long long>(snapshot.processed_events));
    std::printf("engine-events-per-second=%.0f\n", elapsed > 0 ? snapshot.processed_events * 1000.0 / elapsed : 0.0);
    std::printf("engine-ns-per-event=%.1f\n", snapshot.processed_events > 0 ? elapsed * 1e6 / snapshot.processed_events : 0.0);
    std::printf("engine-output-hash=%016llx\n", static_cast<unsigned long long>(output->hash.h));
    return error.ok() && ended.load() == 1 && failed.load() == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    using namespace midibn;
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    bool withAnalysis = false, withStages = false, withEngine = false, withRouting = false, withEngineReal = false;
    bool nullOutput = false, profile = false, withMicro = false;
    int repeat = 1;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--null-output") nullOutput = true;
        else if (args[i] == "--profile") profile = true;
        else if (args[i].compare(0, 9, "--repeat=") == 0) repeat = std::max(1, std::atoi(args[i].c_str() + 9));
        else if (args[i] == "--analysis") withAnalysis = true;
        else if (args[i] == "--micro") withMicro = true;
        else if (args[i] == "--stages") withStages = true;
        else if (args[i] == "--engine") withEngine = true;
        else if (args[i] == "--routing") withRouting = true;
        else if (args[i] == "--engine-real") withEngineReal = true;
        else { args.clear(); break; }
    }
    if (args.size() < 2) { std::fprintf(stderr, "usage: midibn-perf [--analysis] [--micro] [--stages] [--routing] [--engine] [--engine-real] <midi>\n"); return 2; }
    const std::string& path = args.back();
    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    if (!source) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    std::unique_ptr<Song> song(new Song());
    LoadError loadError;
    StageTimer stages;
    LoadOptions options;
    if (withStages) options.monitor = &stages;
    int64_t start = win32::performance_counter();
    stages.start = start;
    bool ok = load_song(*source, options, *song, loadError);
    double elapsed = win32::milliseconds_since(start);
    if (withStages) stages.print(win32::performance_counter());
    if (!ok) { std::fprintf(stderr, "%s: %s\n", loadError.type.c_str(), loadError.message.c_str()); return 1; }
    std::printf("implementation=v2-core\n");
    std::printf("architecture=%s\n", sizeof(void*) == 8 ? "x64" : "x86");
    std::printf("build=%s\n", tools::build_description().c_str());
    std::printf("events=%lld\n", static_cast<long long>(song->store.count()));
    std::printf("load-ms=%.1f\n", elapsed);
    win32::ProcessMemory memory = win32::process_memory();
    std::printf("peak-working-set-bytes=%llu\n", static_cast<unsigned long long>(memory.peak_working_set));
    std::printf("peak-private-bytes=%llu\n", static_cast<unsigned long long>(memory.peak_private));
    std::printf("private-bytes-after-load=%llu\n", static_cast<unsigned long long>(memory.current_private));
    if (withMicro) run_micro(*song);
    if (withAnalysis) {
        run_analysis("base", *song, nullptr);
        auto queue = std::make_shared<analysis::AnalysisConfiguration>();
        queue->simulate_slowdown = true;
        queue->service_duration_mode = models::ServiceDurationMode::ProcessingTime;
        queue->processing_us = 250;
        queue->midi_bitrate = 31250;
        queue->events_per_second = 1000;
        queue->queue_length_limit_enabled = true;
        queue->queue_length_limit = 1024;
        queue->queue_age_limit_enabled = true;
        queue->queue_age_limit_us = 5000;
        queue->overflow_policy = models::OverflowPolicy::DropNewest;
        run_analysis("queue", *song, queue, profile);
        auto gates = std::make_shared<analysis::AnalysisConfiguration>(*queue);
        gates->per_note_interval_gate_enabled = true;
        gates->delayed_note_off_enabled = true;
        gates->note_attack_limit_enabled = true;
        gates->output_rate_limit_enabled = true;
        run_analysis("gates", *song, gates, profile);
        win32::ProcessMemory after = win32::process_memory();
        std::printf("peak-working-set-bytes-after-analysis=%llu\n", static_cast<unsigned long long>(after.peak_working_set));
    }
    if (withRouting) {
        routing::RoutingSong rs;
        rs.events = models::EventSource(&song->store);
        rs.track_count = song->track_count;
        rs.duration_us = song->duration_us;
        rs.tracks = &song->tracks;
        rs.identity = &song->store;
        int64_t routeStart = win32::performance_counter();
        std::shared_ptr<routing::CompiledTrackRoute> route = routing::compile_route(rs, routing::TrackRoutingSettings());
        std::printf("routing-compile-ms=%.1f\n", win32::milliseconds_since(routeStart));
        std::printf("routing-valid=%d lanes=%lld output-events=%d\n", route->is_valid() ? 1 : 0, static_cast<long long>(route->lanes.size()), route->statistics.routed_output_event_count);
        win32::ProcessMemory after = win32::process_memory();
        std::printf("peak-private-bytes-after-routing=%llu\n", static_cast<unsigned long long>(after.peak_private));
        std::printf("private-bytes-after-routing=%llu\n", static_cast<unsigned long long>(after.current_private));
        if (route->is_valid()) {
            // Applying the route builds the routed event view (what playback reads).
            int64_t viewStart = win32::performance_counter();
            routing::TrackRoutedEventStore view(rs, route);
            std::printf("routed-view-ms=%.1f\n", win32::milliseconds_since(viewStart));
            std::printf("routed-view-events=%d\n", view.count());
            win32::ProcessMemory applied = win32::process_memory();
            std::printf("peak-private-bytes-after-routed-view=%llu\n", static_cast<unsigned long long>(applied.peak_private));
            std::printf("private-bytes-after-routed-view=%llu\n", static_cast<unsigned long long>(applied.current_private));
        }
    }
    if (withEngine) return run_engine(*song, path);
    if (withEngineReal) {
        int result = 0;
        for (int i = 0; i < repeat; ++i) result |= run_engine_real(*song, path, nullOutput, profile);
        return result;
    }
    return 0;
}
