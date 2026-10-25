// midibn-few-cpu-measure: how does playback behave on a PC with one or two logical processors, and what do the
// few-processor options (win32_timing.h: set_few_cpu_policy) change? One play of one fixture per run, on the real engine
// and the real clock, output None (a counter): NO MIDI device is opened and no driver is loaded.
//
// The run can restrict the process to some logical processors (--affinity) and start, inside this process,
//   * a stand-in "audio" thread at Time Critical priority that is busy for --audio-percent of every 10 ms (the thread
//     a synthesizer driver such as OmniMIDI creates inside the player's process), and
//   * a stand-in "user interface" thread at Normal priority that wants to run every 1/--ui-hz second (the player's
//     window thread asking for the statistics) and records how late it was woken.
// It prints one line of key=value results: how late playback ended (lag), events per second, the longest lateness of
// the send of an event (maximum lag the engine reports), the user-interface wake-up lateness (p50, p99, max), and how much
// processor time the stand-in audio thread actually got.
//
//   midibn-few-cpu-measure --auto [--out FILE] [--rounds N]   (P12-WINPLAT: makes its own silent file, runs option off/on on
//                          2 and 1 logical processors, writes few-processor-result.txt; see few_cpu_auto.h)
//   midibn-few-cpu-measure --fixture FILE [--affinity HEX] [--few-cpu normal|yield=MS|normal,yield=MS]
//                          [--audio-percent N] [--send-cost-ns N] [--ui-hz N] [--label TEXT] [--out FILE]
#include "midibn/engine/playback_engine.h"
#include "midibn/midi_loader.h"
#include "midibn/null_output.h"
#include "few_cpu_auto.h"
#include "thread_report.h"
#include "tool_support.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;

namespace {

int64_t qpc()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

int64_t qpc_freq()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

double thread_cpu_ms(HANDLE thread)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(thread, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a, b;
    a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return static_cast<double>(a.QuadPart + b.QuadPart) / 10000.0;
}

// Counts sends (the "None" output with a counter).
class CountingOutput : public platform::NullMidiOutput {
public:
    // A driver costs processor time per message: every 8th send spins for 8 x cost (the counter ticks every 100 ns).
    platform::OutputStatus send_short(uint32_t) override
    {
        if ((++count & 7) == 0 && cost_ticks > 0) { const int64_t s = qpc(); while (qpc() - s < cost_ticks) {} }
        return platform::OutputStatus::success();
    }
    int64_t cost_ticks = 0;   // per 8 sends
    uint64_t count = 0;
};

double percentile(std::vector<double>& v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(p * (v.size() - 1) + 0.5);
    return v[i];
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--auto") return few_cpu_auto::run(args);   // the whole silent measurement in one call (XP self-test package)
    std::string fixture, label = "run", outFile, fewCpu;
    unsigned long long affinity = 0;
    int audioPercent = 0;
    int sendCostNs = 0;
    double uiHz = 60;
    for (size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--fixture") fixture = args[i + 1];
        else if (args[i] == "--affinity") affinity = std::strtoull(args[i + 1].c_str(), nullptr, 16);
        else if (args[i] == "--few-cpu") fewCpu = args[i + 1];
        else if (args[i] == "--audio-percent") audioPercent = std::atoi(args[i + 1].c_str());
        else if (args[i] == "--send-cost-ns") sendCostNs = std::atoi(args[i + 1].c_str());
        else if (args[i] == "--ui-hz") uiHz = std::atof(args[i + 1].c_str());
        else if (args[i] == "--label") label = args[i + 1];
        else if (args[i] == "--out") outFile = args[i + 1];
        else { tools::write_stdout("unknown option " + args[i] + "\n"); return 1; }
    }
    if (fixture.empty()) { tools::write_stdout("--fixture FILE is needed\n"); return 1; }
    timeBeginPeriod(1);   // 1 ms timer resolution, as the player holds while playing (so the stand-in sleeps are fair)
    if (affinity != 0) SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(affinity));
    int yieldMs = 0;
    size_t y = fewCpu.find("yield=");
    if (y != std::string::npos) yieldMs = std::atoi(fewCpu.c_str() + y + 6);
    size_t nap = fewCpu.find("nap=");
    if (nap != std::string::npos) { yieldMs = std::atoi(fewCpu.c_str() + nap + 4); win32::set_few_cpu_nap(true); }
    win32::set_few_cpu_policy(fewCpu.find("normal") != std::string::npos, yieldMs);

    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(fixture, error);
    if (!source) { tools::write_stdout("cannot open " + fixture + ": " + error + "\n"); return 2; }
    std::shared_ptr<Song> song = std::make_shared<Song>();
    LoadError loadError;
    if (!load_song(*source, LoadOptions(), *song, loadError)) { tools::write_stdout("cannot load: " + loadError.message + "\n"); return 2; }
    std::shared_ptr<engine::PlaybackSong> playback = engine::make_playback_song(*song, fixture, song);
    const int64_t events = playback->events.count();
    const double durationMs = static_cast<double>(playback->duration_us) / 1000.0;

    const int64_t freq = qpc_freq();
    std::atomic<bool> stop{ false };
    // Stand-in audio thread: Time Critical, busy for audioPercent of each 10 ms.
    std::atomic<uint32_t> audioThreadId{ 0 };
    std::atomic<double> audioCpuMs{ 0 };
    std::thread audio;
    if (audioPercent > 0) {
        audio = std::thread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            audioThreadId.store(GetCurrentThreadId());
            const int64_t period = freq / 100;
            while (!stop.load()) {
                const int64_t start = qpc();
                const int64_t busy = period * audioPercent / 100;
                while (qpc() - start < busy) {}
                const int64_t left = period - (qpc() - start);
                if (left > 0) Sleep(static_cast<DWORD>(std::max<int64_t>(1, left * 1000 / freq)));
            }
            audioCpuMs.store(thread_cpu_ms(GetCurrentThread()));
        });
    }
    // Stand-in user-interface thread: Normal priority, wants to run every 1/uiHz s.
    std::vector<double> uiLate;
    std::vector<double> uiAt;   // when (ms after the stand-in started) each wake happened
    std::thread ui;
    if (uiHz > 0) {
        uiLate.reserve(4096);
        ui = std::thread([&] {
            const int64_t period = static_cast<int64_t>(static_cast<double>(freq) / uiHz);
            const int64_t uiStart = qpc();
            int64_t next = uiStart + period;
            while (!stop.load()) {
                const int64_t now = qpc();
                if (next > now) Sleep(static_cast<DWORD>(std::max<int64_t>(1, (next - now) * 1000 / freq)));
                const int64_t woke = qpc();
                uiLate.push_back(static_cast<double>(woke - next) * 1000.0 / static_cast<double>(freq));
                uiAt.push_back(static_cast<double>(woke - uiStart) * 1000.0 / static_cast<double>(freq));
                next += period;
                if (next < woke) next = woke + period;   // missed ticks are not made up (a window repaints once)
            }
        });
    }

    std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();
    std::shared_ptr<CountingOutput> output = std::make_shared<CountingOutput>();
    output->cost_ticks = static_cast<int64_t>(sendCostNs) * 8 * freq / 1000000000LL;
    engine::PlaybackEngine engine(services);
    std::atomic<int64_t> endedAt{ 0 };
    engine.set_playback_ended_handler([&] { endedAt.store(qpc()); });
    engine.set_playback_failed_handler([&](const engine::EngineError&) { endedAt.store(qpc()); });
    Sleep(50);   // let the stand-in threads start
    const std::set<uint32_t> before = win32::thread_ids(win32::snapshot_threads());
    const int64_t t0 = qpc();
    engine::EngineError started = engine.start(playback, output, engine::ProcessingMode::Queue, 0, false);
    if (!started.ok()) { tools::write_stdout("start failed: " + started.message + "\n"); stop.store(true); if (audio.joinable()) audio.join(); if (ui.joinable()) ui.join(); return 3; }
    // The worker's own priority, read from this process's thread list after the start (shows the policy took effect).
    std::string workerPriority = "?";
    {
        std::vector<win32::ThreadInfo> threads = win32::snapshot_threads();
        for (size_t i = 0; i < threads.size(); ++i)
            if (before.find(threads[i].id) == before.end() && threads[i].priority_known)
                workerPriority = win32::thread_priority_name(threads[i].priority);   // the engine's worker (started after the snapshot)
    }
    bool timeout = false;
    while (endedAt.load() == 0 || engine.has_live_worker()) {
        Sleep(2);
        if (static_cast<double>(qpc() - t0) * 1000.0 / static_cast<double>(freq) > durationMs * 20.0 + 30000.0) { timeout = true; break; }
    }
    const int64_t t1 = endedAt.load() != 0 ? endedAt.load() : qpc();
    engine::PlaybackSnapshot snap = engine.snapshot();
    engine.stop();
    engine.dispose();
    stop.store(true);
    if (audio.joinable()) audio.join();
    if (ui.joinable()) ui.join();

    const double wallMs = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(freq);
    char line[1024];
    std::snprintf(line, sizeof(line),
        "label=%s cpus=%u few_cpu=\"%s\" worker=%s audio_pct=%d cost_ns=%d events=%lld sent=%llu song_ms=%.0f wall_ms=%.0f lag_ms=%.0f meps=%.2f "
        "max_send_lag_ms=%.1f ui_wake_late_ms_p50=%.2f p99=%.2f max=%.2f ui_ticks=%zu audio_cpu_ms=%.0f%s\n",
        label.c_str(), static_cast<unsigned>(win32::usable_logical_processors()), fewCpu.c_str(), workerPriority.c_str(), audioPercent, sendCostNs,
        static_cast<long long>(events), static_cast<unsigned long long>(output->count), durationMs, wallMs, wallMs - durationMs,
        wallMs > 0 ? static_cast<double>(output->count) / wallMs / 1000.0 : 0.0,
        static_cast<double>(snap.maximum_lag_microseconds) / 1000.0, percentile(uiLate, 0.5), percentile(uiLate, 0.99),
        uiLate.empty() ? 0.0 : *std::max_element(uiLate.begin(), uiLate.end()), uiLate.size(), audioCpuMs.load(), timeout ? " TIMEOUT" : "");
    {   // where the largest wake-up delay happened (ms after the stand-in thread started; playback starts at about 50 ms)
        size_t m = 0;
        for (size_t i = 1; i < uiLate.size(); ++i) if (uiLate[i] > uiLate[m]) m = i;
        char where[96];
        std::snprintf(where, sizeof(where), " ui_max_delay_at_ms=%.0f", uiAt.empty() ? 0.0 : uiAt[m]);
        std::string text(line);
        text.insert(text.size() - 1, where);
        std::snprintf(line, sizeof(line), "%s", text.c_str());
    }
    tools::write_stdout(line);
    if (!outFile.empty()) {
        FILE* f = nullptr;
        if (fopen_s(&f, outFile.c_str(), "ab") == 0 && f) { std::fputs(line, f); std::fclose(f); }
    }
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
