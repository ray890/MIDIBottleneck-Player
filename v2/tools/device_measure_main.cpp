// midibn-device-measure: real-device MIDI output measurements for v2's engine
// (user prompt 007, Step 2). It plays a generated fixture through the real
// engine into a real output (WinMM or KDMAPI, both ending in OmniMIDI) and
// records when sends actually happen. NOT part of any test suite: it opens
// real devices, so it must only be run through v2/tools/device/run-device-measurements.ps1
// (which mutes the playback endpoints first and restores them afterwards).
//
// Usage:
//   midibn-device-measure gen <out.mid> <uniform|clustered> <events-per-second> <seconds>
//   midibn-device-measure analyze <samples.bin> [timeline.csv]       (prints key=value lines)
//   midibn-device-measure devices
//   midibn-device-measure run --output winmm|winmm-direct|kdmapi|null --fixture <f.mid> --out <dir> --label <name>
//                             [--repeats 3] [--instr none|count|sample|tsall] [--sample-every 128]
//                             [--device-name OmniMIDI] [--warm 1] [--settle-ms 1000] [--mode queue|drop] [--profile 1]
//                             [--monitor 1] [--override 1] [--ui-hz 60]   open windows emulated (see cmd_run; prompt 009)
//     --output paced [--paced-mps 15]: a silent output that takes at most that many million messages per second
//     Passages (user prompt P8-G): play a part of the file instead of the whole file, as the players do on
//     Play-from-position (the engine starts at that source time and first sends the state chase):
//                             [--start <s> [--seconds <duration>] [--mark <s>]]  or  [--passages 118-126:123,190-210]
//                               (start-end[:mark] in source seconds; mark = where the heavy part itself begins, after a lead-in)
//     Several outputs in one process, so a big file is loaded once:
//                             [--outputs kdmapi,winmm] [--path-tags kdmapi,winmm-system] [--path-settle-ms 3000]
//                             [--label-template "v2-{path}-user-file-1-p{pass}"] [--cap-extras 120,10]
//                               (cap-extras: seconds of wall time allowed beyond the passage length, per output; then the run is
//                                stopped, marked capped=yes and the lag reached is reported)
//   midibn-device-measure run --fixture <f.mid> --out <dir> --label <name> --load-only 1   load time and peak memory only (no device)
//   midibn-device-measure outputs                                    the output kinds this build supports
//   midibn-device-measure raw --output winmm|winmm-direct|kdmapi --layer api|native|output --seconds 2 [--repeats 3] [--device-name OmniMIDI]
//   midibn-device-measure direct-stress --fixture <f.mid> [--device-name OmniMIDI] [--cycles 200] [--flood-ms 100] [--mode stop|close|panic]
//                         [--close-style reset|notesoff|settle] [--settle-ms 250] [--keep-open 1]
//                         (--keep-open 1, DD-111: every cycle on one output that stays open across Stop, as the
//                          player does; closed once at the end.)
//                         (DD-085: open, flood, then Stop / close / panic during the flood, repeated; only non-sounding files.
//                          DD-094: a hang names its cycle, step and the driver calls in progress; a Stop that times out ends
//                          the cycles, because the device must not be opened again while a call is still inside the driver)
//   "winmm-direct" is Direct WinMM driver access (register entry DD-085): the driver's own modMessage instead of midiOutShortMsg.
//
// "run" writes, per repeat r, <out>/<label>.r<r>.run.txt (key=value lines) and, unless --instr is
// none, <out>/<label>.r<r>.samples.bin (format: device_analysis.h). The v1 driver
// (v2/harness/v1-reference/device/DeviceMeasure.cs) writes the same files.
#include "device_analysis.h"
#include "midibn/app/effective_speed.h"
#include "midibn/engine/playback_engine.h"
#include "midibn/midi_loader.h"
#include "midibn/null_output.h"
#include "kdmapi_midi_output.h"
#include "sampling_profiler.h"
#include "tool_support.h"
#include "win32_platform.h"
#include "win32_timing.h"
#include "winmm_direct_output.h"
#include "winmm_midi_output.h"

#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
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

int64_t qpc_frequency()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

void say(const std::string& text) { tools::write_stdout(text + "\n"); }

// The system timer resolution in effect now, in microseconds (NtQueryTimerResolution; 15625 = the default
// 15.6 ms tick, 1000 = something has called timeBeginPeriod(1), e.g. the OmniMIDI driver). It decides how long
// Sleep(1) really sleeps, which is what the engine's final-approach spin falls back to.
double timer_resolution_us()
{
    typedef LONG(NTAPI * Fn)(PULONG, PULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    Fn fn = ntdll ? reinterpret_cast<Fn>(GetProcAddress(ntdll, "NtQueryTimerResolution")) : nullptr;
    ULONG minimum = 0, maximum = 0, current = 0;
    if (!fn || fn(&minimum, &maximum, &current) != 0) return -1.0;
    return static_cast<double>(current) / 10.0;
}

std::string to_str(double v, int digits = 3)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}

// ---------------------------------------------------------------- arguments

struct Options {
    std::map<std::string, std::string> values;
    std::string get(const std::string& key, const std::string& fallback = std::string()) const
    {
        auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    long long num(const std::string& key, long long fallback) const
    {
        auto it = values.find(key);
        return it == values.end() ? fallback : std::atoll(it->second.c_str());
    }
};

Options parse_options(const std::vector<std::string>& args, size_t from)
{
    Options o;
    for (size_t i = from; i + 1 < args.size(); i += 2) {
        if (args[i].size() < 3 || args[i][0] != '-' || args[i][1] != '-') throw std::runtime_error("bad option " + args[i]);
        o.values[args[i].substr(2)] = args[i + 1];
    }
    return o;
}

// ---------------------------------------------------------------- fixture generation

void put_vlq(std::vector<uint8_t>& v, uint32_t x)
{
    uint8_t b[5];
    int n = 0;
    b[n++] = x & 0x7F;
    while ((x >>= 7) != 0) b[n++] = static_cast<uint8_t>((x & 0x7F) | 0x80);
    while (n > 0) v.push_back(b[--n]);
}

void put_u32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}

// Format 0, one track of short channel messages (controllers 20..31, undefined by the MIDI standard,
// so a synthesizer does almost no work for them). "uniform": a 1 microsecond tick, the events spread
// evenly over time. "clustered": 960 ticks per quarter at 120 BPM (a tick is 520.8 microseconds),
// every event of a tick at the same instant (how a dense real file is laid out).
int cmd_gen(const std::vector<std::string>& args)
{
    if (args.size() < 6) throw std::runtime_error("gen <out.mid> <uniform|clustered> <events-per-second> <seconds>");
    const std::string path = args[2];
    const bool clustered = args[3] == "clustered";
    if (!clustered && args[3] != "uniform") throw std::runtime_error("kind must be uniform or clustered");
    const double rate = std::atof(args[4].c_str());
    const double seconds = std::atof(args[5].c_str());
    const uint32_t division = clustered ? 960 : 480;
    const uint32_t tempo = clustered ? 500000 : 480;   // microseconds per quarter: 960 ticks -> 520.833 us; 480 ticks -> 1 us
    const double tickSeconds = static_cast<double>(tempo) / division / 1e6;
    const double perTick = rate * tickSeconds;
    const long long ticks = static_cast<long long>(seconds / tickSeconds);
    std::vector<uint8_t> track;
    track.reserve(static_cast<size_t>(rate * seconds * 4.0 + 1024));
    put_vlq(track, 0);
    track.push_back(0xFF); track.push_back(0x51); track.push_back(0x03);
    track.push_back(static_cast<uint8_t>(tempo >> 16)); track.push_back(static_cast<uint8_t>(tempo >> 8)); track.push_back(static_cast<uint8_t>(tempo));
    double carry = 0;
    long long events = 0;
    uint32_t pendingDelta = 1;
    for (long long t = 0; t < ticks; ++t) {
        carry += perTick;
        const long long count = static_cast<long long>(carry);
        carry -= static_cast<double>(count);
        for (long long i = 0; i < count; ++i) {
            put_vlq(track, i == 0 ? pendingDelta : 0);
            track.push_back(static_cast<uint8_t>(0xB0 | (events & 15)));
            track.push_back(static_cast<uint8_t>(20 + (events / 16) % 12));
            track.push_back(static_cast<uint8_t>((events * 7) & 127));
            ++events;
        }
        pendingDelta = (count == 0) ? pendingDelta + 1 : 1;
    }
    put_vlq(track, pendingDelta);
    track.push_back(0xFF); track.push_back(0x2F); track.push_back(0x00);
    std::vector<uint8_t> file;
    const char head[] = { 'M', 'T', 'h', 'd' };
    file.insert(file.end(), head, head + 4);
    put_u32(file, 6);
    file.push_back(0); file.push_back(0);   // format 0
    file.push_back(0); file.push_back(1);   // one track
    file.push_back(static_cast<uint8_t>(division >> 8)); file.push_back(static_cast<uint8_t>(division));
    const char trk[] = { 'M', 'T', 'r', 'k' };
    file.insert(file.end(), trk, trk + 4);
    put_u32(file, static_cast<uint32_t>(track.size()));
    file.insert(file.end(), track.begin(), track.end());
    std::string blob(reinterpret_cast<const char*>(file.data()), file.size());
    if (!tools::write_file(path, blob)) throw std::runtime_error("cannot write " + path);
    say("gen " + path + " kind=" + args[3] + " events=" + std::to_string(events) + " bytes=" + std::to_string(file.size()) +
        " intended_meps=" + to_str(static_cast<double>(events) / seconds / 1e6, 3));
    return 0;
}

int cmd_analyze(const std::vector<std::string>& args)
{
    if (args.size() < 3) throw std::runtime_error("analyze <samples.bin> [timeline.csv]");
    tools::SampleRun run;
    std::string error;
    if (!tools::read_sample_file(args[2], run, error)) throw std::runtime_error(error);
    for (const std::string& line : tools::analyze_samples(run, args.size() > 3 ? args[3] : std::string())) say(line);
    return 0;
}

// ---------------------------------------------------------------- loading

struct LoadedSong {
    std::shared_ptr<Song> song;
    std::shared_ptr<engine::PlaybackSong> playback;
};

LoadedSong load_fixture(const std::string& path)
{
    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    if (!source) throw std::runtime_error("cannot open " + path + ": " + error);
    LoadedSong loaded;
    loaded.song = std::make_shared<Song>();
    LoadError loadError;
    if (!load_song(*source, LoadOptions(), *loaded.song, loadError))
        throw std::runtime_error("cannot load " + path + ": " + loadError.type + ": " + loadError.message);
    loaded.playback = engine::make_playback_song(*loaded.song, path, loaded.song);
    return loaded;
}

// ---------------------------------------------------------------- calibration

double calibrate_qpc_ns()
{
    // Cost of one timer read: the mean over a long run of consecutive reads (the counter ticks every 100 ns
    // here, so a back-to-back pair mostly reads 0 or 1 tick and says nothing).
    const int reads = 4000000;
    int64_t a = qpc();
    volatile int64_t sink = 0;
    for (int i = 0; i < reads; ++i) sink = qpc();
    int64_t b = qpc();
    (void)sink;
    return static_cast<double>(b - a) * 1e9 / static_cast<double>(qpc_frequency()) / reads;
}

double calibrate_lock_ns()
{
    // Uncontended recursive_mutex lock + unlock (what the engine takes several times per event).
    std::recursive_mutex m;
    const int n = 5000000;
    volatile int sink = 0;
    int64_t a = qpc();
    for (int i = 0; i < n; ++i) { std::lock_guard<std::recursive_mutex> l(m); sink = sink + 1; }
    int64_t b = qpc();
    return static_cast<double>(b - a) * 1e9 / static_cast<double>(qpc_frequency()) / n;
}

// ---------------------------------------------------------------- CPU load

struct CpuSnapshot {
    uint64_t idle = 0, kernel = 0, user = 0, self = 0;
    static uint64_t u(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
    static CpuSnapshot take()
    {
        CpuSnapshot s;
        FILETIME i, k, us, c, e, sk, su;
        GetSystemTimes(&i, &k, &us);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &sk, &su);
        s.idle = u(i); s.kernel = u(k); s.user = u(us); s.self = u(sk) + u(su);
        return s;
    }
};

std::vector<std::string> cpu_lines(const CpuSnapshot& a, const CpuSnapshot& b)
{
    // GetSystemTimes: kernel time includes idle.
    const double total = static_cast<double>((b.kernel - a.kernel) + (b.user - a.user));
    const double idle = static_cast<double>(b.idle - a.idle);
    const double self = static_cast<double>(b.self - a.self);
    std::vector<std::string> v;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    v.push_back("logical_cpus=" + std::to_string(info.dwNumberOfProcessors));
    if (total > 0) {
        v.push_back("cpu_total_busy_pct=" + to_str(100.0 * (total - idle) / total, 1));
        v.push_back("cpu_this_process_pct=" + to_str(100.0 * self / total, 1));
        v.push_back("cpu_other_processes_pct=" + to_str(100.0 * std::max(0.0, total - idle - self) / total, 1));
    }
    return v;
}

// ---------------------------------------------------------------- the measuring decorator

enum class Instr { None, Count, Sample, TsAll };

// Wraps a real output. Per send: one counter increment and one compare (Count), plus two timer reads
// on every `every`-th send (Sample), or two timer reads on every send (TsAll, the naive approach, kept
// only to measure how much it costs). Runs on the engine's single sending thread; read after the
// worker has stopped.
class MeasuringOutput : public platform::MidiOutput {
public:
    struct Raw { int64_t k; int64_t before; int32_t dur; };

    // `expected` = the events of the played window (the whole file, or the passage). `chaseSkip` = how many
    // state-chase messages the engine sends first when playback starts inside the file (play-from-position);
    // they are forwarded but not counted as scheduled events (0 for a play from the beginning).
    MeasuringOutput(std::shared_ptr<platform::MidiOutput> inner, Instr mode, uint32_t every, int64_t expected, int64_t chaseSkip = 0)
        : inner_(std::move(inner)), mode_(mode), every_(every), last_(expected - 1), chase_left_(chaseSkip)
    {
        mask_ = (mode == Instr::Sample) ? static_cast<int64_t>(every - 1) : (mode == Instr::TsAll ? 127 : INT64_MAX);
        if (mode == Instr::Sample || mode == Instr::TsAll) {
            const int64_t stride = (mode == Instr::Sample) ? every : 128;
            samples_.reserve(static_cast<size_t>(expected / stride + 8));
        }
    }

    platform::OutputStatus send_short(uint32_t packed) override
    {
        if (chase_left_ > 0) { --chase_left_; ++chase_sends_; note_chase(packed); return inner_->send_short(packed); }
        const int64_t c = count_++;
        if (mode_ == Instr::TsAll) {
            const int64_t b = qpc();
            platform::OutputStatus s = inner_->send_short(packed);
            const int64_t a = qpc();
            sum_dur_ += a - b;
            if (((c & mask_) == 0 && c <= last_) || c == last_) samples_.push_back({ c, b, static_cast<int32_t>(a - b) });
            if (c == last_) finish(a);
            return s;
        }
        if (((c & mask_) == 0 && c <= last_) || c == last_) {
            const int64_t b = qpc();
            platform::OutputStatus s = inner_->send_short(packed);
            const int64_t a = qpc();
            samples_.push_back({ c, b, static_cast<int32_t>(a - b) });
            if (c == last_) finish(a);
            return s;
        }
        return inner_->send_short(packed);
    }

    // What the state chase sent, per channel (-1 = nothing sent): volume, expression, sustain pedal, program, bank.
    struct ChaseValues {
        int16_t cc7[16], cc11[16], cc64[16], program[16], bank_msb[16], bank_lsb[16];
        ChaseValues() { for (int c = 0; c < 16; ++c) cc7[c] = cc11[c] = cc64[c] = program[c] = bank_msb[c] = bank_lsb[c] = -1; }
    };
    const ChaseValues& chase_values() const { return chase_values_; }

    // True once the last event of the window has been sent (read by the waiting thread).
    bool done() const { return done_.load(std::memory_order_acquire); }
    int64_t done_stamp() const { return done_stamp_; }
    int64_t chase_sends() const { return chase_sends_; }
    int64_t window_sends() const { return std::min<int64_t>(count_, last_ + 1); }
    int64_t extra_sends() const { return std::max<int64_t>(0, count_ - (last_ + 1)); }

    const char* backend_name() const override { return inner_->backend_name(); }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return inner_->enumerate(); }
    platform::OutputStatus open(uint32_t id) override { return inner_->open(id); }
    platform::OutputStatus close() override { return inner_->close(); }
    bool is_open() const override { return inner_->is_open(); }
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& f) override { return inner_->send_long(f); }
    platform::OutputStatus reset() override { return inner_->reset(); }
    platform::OutputStatus panic() override { return inner_->panic(); }
    bool sysex_supported() const override { return inner_->sysex_supported(); }
    std::string sysex_disabled_reason() const override { return inner_->sysex_disabled_reason(); }
    void set_source_file(const std::string& p) override { inner_->set_source_file(p); }

    int64_t count() const { return count_; }
    int64_t sum_dur() const { return sum_dur_; }
    const std::vector<Raw>& samples() const { return samples_; }

private:
    std::shared_ptr<platform::MidiOutput> inner_;
    Instr mode_;
    uint32_t every_;
    int64_t last_;
    int64_t chase_left_;
    int64_t chase_sends_ = 0;
    int64_t mask_;
    int64_t count_ = 0;
    int64_t sum_dur_ = 0;
    int64_t done_stamp_ = 0;
    std::atomic<bool> done_{ false };
    std::vector<Raw> samples_;

    ChaseValues chase_values_;

    void note_chase(uint32_t packed)
    {
        const int status = static_cast<int>(packed & 0xFF);
        const int ch = status & 15;
        const int d1 = static_cast<int>((packed >> 8) & 0x7F);
        const int d2 = static_cast<int>((packed >> 16) & 0x7F);
        if ((status & 0xF0) == 0xC0) chase_values_.program[ch] = static_cast<int16_t>(d1);
        else if ((status & 0xF0) == 0xB0) {
            if (d1 == 7) chase_values_.cc7[ch] = static_cast<int16_t>(d2);
            else if (d1 == 11) chase_values_.cc11[ch] = static_cast<int16_t>(d2);
            else if (d1 == 64) chase_values_.cc64[ch] = static_cast<int16_t>(d2);
            else if (d1 == 0) chase_values_.bank_msb[ch] = static_cast<int16_t>(d2);
            else if (d1 == 32) chase_values_.bank_lsb[ch] = static_cast<int16_t>(d2);
        }
    }

    void finish(int64_t stamp)
    {
        done_stamp_ = stamp;
        done_.store(true, std::memory_order_release);
    }
};

// Counts the sends in a value another thread may read at any time (one plain store per send, no locked instruction):
// the waiting loop of `run --ui-hz` uses it to find the longest stretch in which nothing was sent (output in chunks,
// register entry DD-093). Forwards everything to the wrapped output.
class ProgressOutput : public platform::MidiOutput {
public:
    explicit ProgressOutput(std::shared_ptr<platform::MidiOutput> inner) : inner_(std::move(inner)) {}
    platform::OutputStatus send_short(uint32_t packed) override
    {
        sends_.store(sends_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return inner_->send_short(packed);
    }
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& f) override
    {
        sends_.store(sends_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return inner_->send_long(f);
    }
    int64_t sends() const { return sends_.load(std::memory_order_relaxed); }
    const char* backend_name() const override { return inner_->backend_name(); }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return inner_->enumerate(); }
    platform::OutputStatus open(uint32_t id) override { return inner_->open(id); }
    platform::OutputStatus close() override { return inner_->close(); }
    bool is_open() const override { return inner_->is_open(); }
    platform::OutputStatus reset() override { return inner_->reset(); }
    platform::OutputStatus panic() override { return inner_->panic(); }
    bool sysex_supported() const override { return inner_->sysex_supported(); }
    std::string sysex_disabled_reason() const override { return inner_->sysex_disabled_reason(); }
    void set_source_file(const std::string& p) override { inner_->set_source_file(p); }
    std::string path_description() const override { return inner_->path_description(); }

private:
    std::shared_ptr<platform::MidiOutput> inner_;
    std::atomic<int64_t> sends_{ 0 };
};

double percentile_ms(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

// ---------------------------------------------------------------- outputs

// Receives every short message, counts it and folds it into an order-dependent hash (one multiply per message):
// two engines that send the same messages in the same order give the same hash. Makes no sound, opens nothing.
class CountingHashOutput : public platform::NullMidiOutput {
public:
    platform::OutputStatus send_short(uint32_t packed) override
    {
        ++shorts;
        hash = (hash ^ packed) * 1099511628211ull;
        return platform::OutputStatus::success();
    }
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override
    {
        ++longs;
        hash = (hash ^ (0x100000000ull | fragment.length)) * 1099511628211ull;
        for (size_t i = 0; i < fragment.length; ++i) hash = (hash ^ fragment.data[i]) * 1099511628211ull;
        return platform::OutputStatus::success();
    }
    void clear() { shorts = 0; longs = 0; hash = 1469598103934665603ull; }
    uint64_t shorts = 0, longs = 0, hash = 1469598103934665603ull;
};

// A silent output that takes messages no faster than a set rate (prompt 009, bug 4.3: a device that accepts about 15
// million messages per second). Every 64th message waits, spinning, until the previous 64 have used their share of time;
// after an idle stretch it starts afresh (no catch-up burst). Makes no sound, opens nothing.
class PacedOutput : public platform::NullMidiOutput {
public:
    explicit PacedOutput(double messagesPerSecond)
        : batch_ticks_(64.0 * static_cast<double>(qpc_frequency()) / messagesPerSecond) {}
    platform::OutputStatus send_short(uint32_t) override
    {
        if ((++count_ & 63) == 0) {
            double now = static_cast<double>(qpc());
            if (busy_until_ > now) {
                while (static_cast<double>(qpc()) < busy_until_) {}
                now = busy_until_;
            }
            busy_until_ = now + batch_ticks_;
        }
        return platform::OutputStatus::success();
    }

private:
    double batch_ticks_;
    double busy_until_ = 0;
    uint64_t count_ = 0;
};

struct OpenedOutput {
    std::shared_ptr<platform::MidiOutput> output;
    std::string identity;   // for the log
    std::shared_ptr<CountingHashOutput> counting;           // set for --output count
    std::shared_ptr<platform::RecordingMidiOutput> recording;   // set for --output recording
};

OpenedOutput open_output(const std::string& kind, const std::string& deviceName, double pacedMessagesPerSecond = 0)
{
    OpenedOutput o;
    if (kind == "null") {
        o.output = std::make_shared<platform::NullMidiOutput>();
        o.identity = "null";
        return o;
    }
    // Two more outputs that make no sound: "count" (counts and hashes every message) and "recording" (keeps every
    // message in memory, as the engine tests' recording output does).
    if (kind == "count") {
        o.counting = std::make_shared<CountingHashOutput>();
        o.output = o.counting;
        o.identity = "count";
        return o;
    }
    if (kind == "paced") {
        const double rate = pacedMessagesPerSecond > 0 ? pacedMessagesPerSecond : 15e6;
        o.output = std::make_shared<PacedOutput>(rate);
        o.identity = "paced " + to_str(rate / 1e6, 2) + " million messages per second";
        return o;
    }
    if (kind == "recording") {
        o.recording = std::make_shared<platform::RecordingMidiOutput>();
        o.recording->open(0);
        o.output = o.recording;
        o.identity = "recording";
        return o;
    }
    if (kind == "winmm") {
        std::shared_ptr<win32::WinMmMidiOutput> out = std::make_shared<win32::WinMmMidiOutput>();
        int64_t id = -1;
        for (const platform::OutputDeviceInfo& d : win32::WinMmMidiOutput::enumerate_devices())
            if (d.name == deviceName) { id = d.device_id; break; }
        if (id < 0) throw std::runtime_error("WinMM device '" + deviceName + "' not found");
        platform::OutputStatus s = out->open(static_cast<uint32_t>(id));
        if (!s.ok()) throw std::runtime_error("WinMM open failed: " + s.message);
        o.output = out;
        o.identity = "winmm device=" + std::to_string(id) + " identity=" + out->output_identity();
        return o;
    }
    if (kind == "winmm-direct") {
        std::shared_ptr<win32::WinMmDirectOutput> out = std::make_shared<win32::WinMmDirectOutput>();
        int64_t id = -1;
        for (const platform::OutputDeviceInfo& d : win32::WinMmMidiOutput::enumerate_devices())
            if (d.name == deviceName) { id = d.device_id; break; }
        if (id < 0) throw std::runtime_error("WinMM device '" + deviceName + "' not found");
        platform::OutputStatus s = out->open(static_cast<uint32_t>(id));
        if (!s.ok()) throw std::runtime_error("direct WinMM open failed: " + s.message);
        o.output = out;
        o.identity = "winmm-direct device=" + std::to_string(id) + " identity=" + out->output_identity() + " driver=" + out->driver_file() +
            (out->driver_verified() ? " (tested)" : " (unverified)") + " lifecycle=[" + out->lifecycle() + "]";
        return o;
    }
    if (kind == "kdmapi") {
        std::shared_ptr<win32::KdmApiMidiOutput> out = std::make_shared<win32::KdmApiMidiOutput>();
        platform::OutputStatus s = out->open(0);
        if (!s.ok()) throw std::runtime_error("KDMAPI open failed: " + s.message);
        o.output = out;
        o.identity = "kdmapi provider=" + out->provider_path();
        return o;
    }
    throw std::runtime_error("unknown output '" + kind + "'");
}

// A device driver can hang in reset or close after a flood (system WinMM does, see DEVICE_MEASUREMENTS.md). All
// results are already on disk by then, so the cleanup runs under a 20 second watchdog that ends the process
// and says so in <label>.cleanup.txt.
template <typename Reset, typename Close>
void cleanup_with_watchdog(const std::string& logPath, Reset&& reset, Close&& close)
{
    struct Log {
        std::string path;
        std::string text;
        std::mutex m;
        void add(const std::string& line)
        {
            std::lock_guard<std::mutex> l(m);
            text += line + "\n";
            tools::write_file(path, text);
        }
    };
    std::shared_ptr<Log> log = std::make_shared<Log>();
    log->path = logPath;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::thread([log, done] {
        for (int i = 0; i < 200 && !done->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (done->load()) return;
        log->add("cleanup-timeout: reset or close did not return within 20 s; process ended by the watchdog");
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
    log->add("reset-start");
    reset();
    log->add("reset-done");
    log->add("close-start");
    close();
    log->add("close-done");
    done->store(true);
}

// ---------------------------------------------------------------- one measured playback

// A part of the file to play: the source seconds it starts and ends at, and optionally the "mark" (the second the
// heavy passage itself starts, after a lead-in), used to report what the lag was when the passage began.
struct Window {
    double start_s = 0;
    double end_s = -1;    // < 0: to the end of the file
    double mark_s = -1;   // < 0: none
};

std::vector<std::string> split_list(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == separator) { if (!current.empty()) parts.push_back(current); current.clear(); }
        else current += ch;
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

// "120-126" or "118-126:123" (window start-end, optional :mark), several separated by commas.
std::vector<Window> parse_windows(const std::string& text)
{
    std::vector<Window> windows;
    for (const std::string& part : split_list(text, ',')) {
        Window w;
        std::string range = part;
        const size_t colon = part.find(':');
        if (colon != std::string::npos) { w.mark_s = std::atof(part.c_str() + colon + 1); range = part.substr(0, colon); }
        const size_t dash = range.find('-', 1);
        if (dash == std::string::npos) throw std::runtime_error("bad passage '" + part + "' (use start-end in seconds)");
        w.start_s = std::atof(range.substr(0, dash).c_str());
        const std::string endText = range.substr(dash + 1);
        w.end_s = endText.empty() ? -1.0 : std::atof(endText.c_str());
        if (w.start_s < 0 || (w.end_s >= 0 && w.end_s <= w.start_s)) throw std::runtime_error("bad passage '" + part + "'");
        windows.push_back(w);
    }
    return windows;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
    return text;
}

double percentile_of(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double rank = p / 100.0 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(rank);
    const size_t hi = std::min(v.size() - 1, lo + 1);
    return v[lo] * (1.0 - (rank - static_cast<double>(lo))) + v[hi] * (rank - static_cast<double>(lo));
}

// One look at the engine's snapshot, taken about every 16 ms like the player's statistics timer.
struct SnapRow {
    double wall_ms = 0;
    int64_t playback_us = 0, timeline_us = 0, frontier_us = 0, processed = 0, dropped = 0, current_lag_us = 0, max_lag_us = 0;
    bool has_speed = false;
    double speed = 0;
};

// What the file itself says is sounding at an event index: notes started and not yet ended (stacked notes count once
// each), distinct (channel, note) pairs, and channels with the sustain pedal down. Sustain tails and release times are
// not modelled. A direct start at a passage misses the note starts before it, so these are the notes that stay silent
// there but sound when playback starts earlier (lead-in).
struct Held {
    int64_t voices = 0;
    int32_t distinct = 0;
    int32_t pedal_channels = 0;
    // The latest value the file set on each channel before this position (-1 = never set): what a state chase should restore.
    int16_t cc7[16], cc11[16], cc64[16], program[16], bank_msb[16], bank_lsb[16];
    uint16_t reset_channels = 0;   // channels that had a Reset All Controllers (CC121) before this position
    Held() { for (int c = 0; c < 16; ++c) cc7[c] = cc11[c] = cc64[c] = program[c] = bank_msb[c] = bank_lsb[c] = -1; }
};

std::map<int32_t, Held> scan_held(const models::EventSource& events, std::vector<int32_t> targets)
{
    std::map<int32_t, Held> result;
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.empty()) return result;
    std::vector<int32_t> counts(16 * 128, 0);
    bool pedal[16] = {};
    Held state;   // running per-channel values
    int64_t voices = 0;
    int32_t distinct = 0;
    size_t t = 0;
    const int32_t last = targets.back();
    for (int32_t i = 0; i <= last; ++i) {
        while (t < targets.size() && targets[t] == i) {
            Held h = state;
            h.voices = voices;
            h.distinct = distinct;
            for (bool b : pedal) if (b) ++h.pedal_channels;
            result[i] = h;
            ++t;
        }
        if (i == last) break;
        const models::EventView e = events.at(i);
        const int ch = e.channel() & 15;
        if (e.kind == EventKind::NoteOn && e.data_length >= 3 && e.data_byte(2) != 0) {
            int32_t& c = counts[static_cast<size_t>(ch * 128 + (e.data_byte(1) & 127))];
            if (c++ == 0) ++distinct;
            ++voices;
        }
        else if (e.kind == EventKind::NoteOff || (e.kind == EventKind::NoteOn && e.data_length >= 3)) {
            int32_t& c = counts[static_cast<size_t>(ch * 128 + (e.data_byte(1) & 127))];
            if (c > 0) { if (--c == 0) --distinct; --voices; }
        }
        else if (e.kind == EventKind::ControlChange && e.data_length >= 3) {
            const int cc = e.data_byte(1);
            const int value = e.data_byte(2);
            if (cc == 64) { pedal[ch] = value >= 64; state.cc64[ch] = static_cast<int16_t>(value); }
            else if (cc == 7) state.cc7[ch] = static_cast<int16_t>(value);
            else if (cc == 11) state.cc11[ch] = static_cast<int16_t>(value);
            else if (cc == 0) state.bank_msb[ch] = static_cast<int16_t>(value);
            else if (cc == 32) state.bank_lsb[ch] = static_cast<int16_t>(value);
            else if (cc == 121) state.reset_channels = static_cast<uint16_t>(state.reset_channels | (1u << ch));
        }
        else if (e.kind == EventKind::ProgramChange && e.data_length >= 2)
            state.program[ch] = static_cast<int16_t>(e.data_byte(1));
    }
    return result;
}

struct PlayParams {
    bool passage = false;
    bool chase = true;           // "Chase MIDI state on Play/Seek" (the players' default: on)
    bool have_held = false;
    Held held_start, held_mark;
    int64_t start_us = 0;
    int64_t end_us = 0;          // exclusive end of the window
    int64_t mark_us = -1;
    int32_t start_index = 0;     // first event of the window
    int64_t window_events = 0;
    int64_t passage_events = 0;  // events from the mark (the heavy part) to the end of the window
    int64_t chase_expected = 0;  // state-chase messages the engine sends first
    double limit_seconds = 0;    // wall-clock limit: duration + cap for a passage
    double window_seconds = 0;
};

std::string mb(uint64_t bytes) { return to_str(static_cast<double>(bytes) / 1048576.0, 1); }

int cmd_run(const Options& opt)
{
    const std::string fixture = opt.get("fixture");
    const std::string outDir = opt.get("out", ".");
    const std::string label = opt.get("label", "run");
    const int repeats = static_cast<int>(opt.num("repeats", 3));
    const std::vector<std::string> instrNames = split_list(opt.get("instr", "sample"), ',');
    const uint32_t every = static_cast<uint32_t>(opt.num("sample-every", 128));
    const long long settleMs = opt.num("settle-ms", 1000);
    const long long pathSettleMs = opt.num("path-settle-ms", 3000);
    const bool loadOnly = opt.num("load-only", 0) != 0;
    if ((every & (every - 1)) != 0 || every == 0) throw std::runtime_error("--sample-every must be a power of two");

    // --mode queue (default) | drop: the processing mode the song is played in (Drop: finite queue, no limit reached).
    const std::string modeName = opt.get("mode", "queue");
    if (modeName != "queue" && modeName != "drop") throw std::runtime_error("--mode must be queue or drop");
    const engine::ProcessingMode processingMode = modeName == "drop" ? engine::ProcessingMode::Drop : engine::ProcessingMode::Queue;

    // Open windows, emulated without a window (prompt 009, bugs 4.2 and 4.3; register entries DD-092 and DD-093):
    //   --monitor 1    channel monitoring on, as while the MIDI Channel Monitor window is open
    //   --override 1   a forced override (channel 1 Volume = 100) as set in that window; the generated fixtures hold no
    //                  Volume messages, so it suppresses nothing, but it turns the live source filters on
    //   --ui-hz N      a second thread asks for the statistics N times a second, as the player's display refresh does
    //                  (twice per refresh with --monitor 1: the main window and the Channel Monitor each ask), and reads the
    //                  monitor's channel snapshot; reported: how long those calls took (ui_call_*), how late the refreshes
    //                  came (ui_tick_gap_max_ms) and the longest stretch in which nothing was sent (send_stall_max_ms)
    const bool emulateMonitor = opt.num("monitor", 0) != 0;
    const bool emulateOverride = opt.num("override", 0) != 0;
    const double uiHz = std::atof(opt.get("ui-hz", "0").c_str());

    // Outputs: one (--output) or several (--outputs a,b,c) played in sequence in this process, so a big file is loaded once.
    std::vector<std::string> kinds = split_list(opt.get("outputs", opt.get("output")), ',');
    if (kinds.empty() && !loadOnly) throw std::runtime_error("--output or --outputs is needed");
    std::vector<std::string> tags = opt.values.count("path-tags") ? split_list(opt.get("path-tags"), ',') : kinds;
    if (tags.size() != kinds.size()) throw std::runtime_error("--path-tags must have as many entries as --outputs");
    std::vector<double> capExtras;
    for (const std::string& c : split_list(opt.get("cap-extras"), ',')) capExtras.push_back(std::atof(c.c_str()));
    while (capExtras.size() < kinds.size()) capExtras.push_back(120.0);

    // Passages: --passages s-e[:mark],...  or  --start S [--seconds D] [--mark M].
    std::vector<Window> windows;
    if (opt.values.count("passages")) windows = parse_windows(opt.get("passages"));
    else if (opt.values.count("start") || opt.values.count("seconds")) {
        Window w;
        w.start_s = std::atof(opt.get("start", "0").c_str());
        w.end_s = opt.values.count("seconds") ? w.start_s + std::atof(opt.get("seconds").c_str()) : -1.0;
        w.mark_s = opt.values.count("mark") ? std::atof(opt.get("mark").c_str()) : -1.0;
        windows.push_back(w);
    }
    const bool passageMode = !windows.empty();

    const std::string arch = sizeof(void*) == 8 ? "x64" : "x86";
    const double qpcNs = loadOnly ? 0.0 : calibrate_qpc_ns();
    const double lockNs = loadOnly ? 0.0 : calibrate_lock_ns();
    const win32::ProcessMemory memBefore = win32::process_memory();
    int64_t loadStart = qpc();
    LoadedSong loaded = load_fixture(fixture);
    const double loadSeconds = static_cast<double>(qpc() - loadStart) / static_cast<double>(qpc_frequency());
    const win32::ProcessMemory memLoad = win32::process_memory();
    const int64_t events = loaded.playback->events.count();
    const int64_t durationUs = loaded.playback->duration_us;
    std::vector<std::string> loadLines;
    loadLines.push_back("program=v2");
    loadLines.push_back("arch=" + arch);
    loadLines.push_back("fixture=" + fixture);
    loadLines.push_back("events=" + std::to_string(events));
    loadLines.push_back("duration_ms=" + to_str(static_cast<double>(durationUs) / 1000.0, 1));
    loadLines.push_back("load_s=" + to_str(loadSeconds, 2));
    loadLines.push_back("chase_prepare_s=0.00");   // v2 builds the state-chase index inside the load
    loadLines.push_back("ready_to_play_s=" + to_str(loadSeconds, 2));
    loadLines.push_back("private_mb_before_load=" + mb(memBefore.current_private));
    loadLines.push_back("private_mb_after_load=" + mb(memLoad.current_private));
    loadLines.push_back("peak_private_mb_after_load=" + mb(memLoad.peak_private));
    loadLines.push_back("peak_working_set_mb_after_load=" + mb(memLoad.peak_working_set));
    {
        std::string text;
        for (const std::string& l : loadLines) text += l + "\n";
        tools::write_file(outDir + "\\" + label + ".load.txt", text);
    }
    if (loadOnly) {
        say("device-measure v2 " + arch + " load-only events=" + std::to_string(events) + " load_s=" + to_str(loadSeconds, 2) +
            " peak_private_mb=" + mb(memLoad.peak_private) + " peak_working_set_mb=" + mb(memLoad.peak_working_set));
        return 0;
    }

    std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();

    // "Chase MIDI state on Play/Seek": the players default it to on (v1 MainForm, v2 PlayerController); --chase 0 turns it off.
    const bool chaseOn = opt.num("chase", 1) != 0;
    // Notes the file has sounding at each window start and mark (one pass over the events up to the last of them).
    std::map<int32_t, Held> heldAt;
    if (passageMode && opt.num("held-scan", 1) != 0) {
        std::vector<int32_t> targets;
        for (const Window& w : windows) {
            const int64_t s = std::min<int64_t>(durationUs, static_cast<int64_t>(w.start_s * 1e6 + 0.5));
            targets.push_back(loaded.playback->events.lower_bound_by_time(s));
            if (w.mark_s >= 0) targets.push_back(loaded.playback->events.lower_bound_by_time(std::min<int64_t>(durationUs, static_cast<int64_t>(w.mark_s * 1e6 + 0.5))));
        }
        const int64_t scanStart = qpc();
        heldAt = scan_held(loaded.playback->events, targets);
        say("held-note scan: " + std::to_string(heldAt.size()) + " positions in " + to_str(static_cast<double>(qpc() - scanStart) / static_cast<double>(qpc_frequency()), 1) + " s");
    }

    auto make_params = [&](const Window* w) {
        PlayParams p;
        p.start_us = 0;
        p.end_us = durationUs;
        p.window_events = events;
        p.window_seconds = static_cast<double>(durationUs) / 1e6;
        if (w) {
            p.passage = true;
            p.start_us = std::min<int64_t>(durationUs, static_cast<int64_t>(w->start_s * 1e6 + 0.5));
            p.end_us = w->end_s < 0 ? durationUs : std::min<int64_t>(durationUs, static_cast<int64_t>(w->end_s * 1e6 + 0.5));
            p.mark_us = w->mark_s < 0 ? -1 : static_cast<int64_t>(w->mark_s * 1e6 + 0.5);
            p.start_index = loaded.playback->events.lower_bound_by_time(p.start_us);
            // The window holds the events from its start up to (not including) its end time; a window that runs to the
            // end of the file holds every remaining event, including any after the last whole microsecond.
            const int32_t endIndex = w->end_s < 0 ? loaded.playback->events.count() : loaded.playback->events.lower_bound_by_time(p.end_us);
            p.window_events = std::max<int64_t>(0, static_cast<int64_t>(endIndex) - p.start_index);
            const int32_t markIndex = p.mark_us >= 0 ? loaded.playback->events.lower_bound_by_time(std::min<int64_t>(durationUs, p.mark_us)) : p.start_index;
            p.passage_events = std::max<int64_t>(0, static_cast<int64_t>(endIndex) - markIndex);
            p.chase = chaseOn;
            auto hs = heldAt.find(p.start_index);
            auto hm = heldAt.find(markIndex);
            if (hs != heldAt.end()) { p.have_held = true; p.held_start = hs->second; p.held_mark = hm != heldAt.end() ? hm->second : hs->second; }
            p.window_seconds = static_cast<double>(p.end_us - p.start_us) / 1e6;
            if (p.chase && p.start_index > 0 && loaded.playback->chase_index != nullptr)
                p.chase_expected = static_cast<int64_t>(loaded.playback->chase_index->messages(p.start_index, nullptr, nullptr).size());
        }
        return p;
    };

    // Plays one window (or the whole file) once. `measuring` is the decorator wrapped around `output` (may be null).
    auto play = [&](std::shared_ptr<platform::MidiOutput> output, const PlayParams& p, bool warm, std::vector<std::string>& info,
                    MeasuringOutput* measuring, int64_t& t0, double& endedMs, int64_t& epoch_rel_ticks, std::vector<SnapRow>& snaps) {
        engine::PlaybackEngine engine(services);
        if (emulateMonitor) engine.set_channel_monitoring(true);
        if (emulateOverride) {
            engine::EngineError overrideError = engine.set_channel_override(0, ChannelAttribute::Volume, 100);
            if (!overrideError.ok()) throw std::runtime_error("--override: " + overrideError.message);
        }
        std::shared_ptr<ProgressOutput> progress;
        if (uiHz > 0 && !warm) {
            progress = std::make_shared<ProgressOutput>(output);
            output = progress;
        }
        if (p.passage) {
            // The settings the players apply at start-up (the chase of controller and program state on a play from a position).
            engine.set_chase_midi_state_on_play_seek(p.chase);
            engine.set_preserve_playback_queue_on_pause_seek(false);
            engine.set_output_paced_timeline(false);
        }
        app::EffectivePlaybackSpeed speedWindow;   // the players' default 250 ms window
        std::atomic<int64_t> endedAt{ 0 };
        std::atomic<int> failed{ 0 };
        std::string failure;
        engine.set_playback_ended_handler([&] { endedAt.store(qpc()); });
        engine.set_playback_failed_handler([&](const engine::EngineError& e) { failure = e.type + ": " + e.message; failed.store(1); endedAt.store(qpc()); });
        const CpuSnapshot cpu0 = CpuSnapshot::take();
        tools::SamplingProfiler profiler;
        const bool profile = !warm && opt.num("profile", 0) != 0;   // --profile 1: where the threads spend their time
        t0 = qpc();
        if (profile) profiler.start(GetCurrentThreadId(), 100);
        engine::EngineError startError = engine.start(loaded.playback, output, processingMode, p.start_us, false);
        const int64_t startReturned = qpc();
        const int64_t freq = qpc_frequency();
        info.push_back("start_call_us=" + to_str(static_cast<double>(startReturned - t0) * 1e6 / static_cast<double>(freq), 1));
        if (!startError.ok()) { info.push_back("start_error=" + startError.type + ": " + startError.message); return; }
        // --ui-hz: the display refresh's calls on their own thread (see the option's comment above).
        std::atomic<bool> uiStop{ false };
        std::vector<double> uiCallMs;
        double uiTickGapMaxMs = 0;
        std::thread uiThread;
        if (uiHz > 0 && !warm) {
            uiCallMs.reserve(4096);
            uiThread = std::thread([&] {
                const int64_t period = std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(freq) / uiHz));
                int64_t next = qpc();
                int64_t lastTick = 0;
                while (!uiStop.load()) {
                    const int64_t a = qpc();
                    if (lastTick != 0) uiTickGapMaxMs = std::max(uiTickGapMaxMs, static_cast<double>(a - lastTick) * 1e3 / static_cast<double>(freq));
                    lastTick = a;
                    engine::PlaybackSnapshot s = engine.snapshot();
                    (void)s;
                    if (emulateMonitor) {
                        engine::PlaybackSnapshot second = engine.snapshot();   // the Channel Monitor's own position read
                        (void)second;
                        std::shared_ptr<const engine::ChannelPlaybackSnapshot> channels = engine.channel_snapshot();
                        (void)channels;
                    }
                    uiCallMs.push_back(static_cast<double>(qpc() - a) * 1e3 / static_cast<double>(freq));
                    next += period;
                    const int64_t now = qpc();
                    if (next <= now) next = now;
                    else std::this_thread::sleep_for(std::chrono::microseconds((next - now) * 1000000 / freq));
                }
            });
        }
        int64_t lastProgressValue = 0;
        int64_t lastProgressStamp = 0;
        double sendStallMaxMs = 0;
        const double limitSeconds = p.passage ? p.limit_seconds : static_cast<double>(durationUs) / 1e6 * 30.0 + 60.0;
        std::string reason = "completed";
        // The engine's own time zero is a little after t0 (start-up work inside start()). Find it from two
        // snapshots a few milliseconds in: epoch = (timer at the snapshot) - (transport position it reports,
        // counted from the start position). The snapshot that took the least time is used.
        int64_t bestSpan = INT64_MAX;
        epoch_rel_ticks = 0;
        int snapshotsTaken = 0;
        int64_t nextSample = t0 + freq * 16 / 1000;
        while (true) {
            const int64_t now = qpc();
            const double elapsedSeconds = static_cast<double>(now - t0) / static_cast<double>(freq);
            if (progress) {
                const int64_t sendsNow = progress->sends();
                if (sendsNow != lastProgressValue) {
                    if (lastProgressValue != 0)
                        sendStallMaxMs = std::max(sendStallMaxMs, static_cast<double>(now - lastProgressStamp) * 1e3 / static_cast<double>(freq));
                    lastProgressValue = sendsNow;
                    lastProgressStamp = now;
                }
            }
            if (snapshotsTaken < 2 && elapsedSeconds > (snapshotsTaken == 0 ? 0.003 : 0.006) && endedAt.load() == 0) {
                const int64_t a = qpc();
                engine::PlaybackSnapshot s = engine.snapshot();
                const int64_t b = qpc();
                ++snapshotsTaken;
                if (b - a < bestSpan) {
                    bestSpan = b - a;
                    epoch_rel_ticks = (a + b) / 2 - t0 - (s.playback_microseconds - p.start_us) * freq / 1000000;
                }
            }
            else if (p.passage && !warm && now >= nextSample && endedAt.load() == 0) {
                nextSample = now + freq * 16 / 1000;
                engine::PlaybackSnapshot s = engine.snapshot();
                const bool synchronized = s.processing_mode != engine::ProcessingMode::PerNoteIntervalGate &&
                    s.processing_mode != engine::ProcessingMode::DelayedNoteOff && s.processing_mode != engine::ProcessingMode::NoteAttackLimit &&
                    s.processing_mode != engine::ProcessingMode::OutputRateLimit && s.outstanding_events == 0 &&
                    s.current_lag_microseconds == 0 && s.last_dispatched_timeline_microseconds <= s.intended_timeline_microseconds;
                std::optional<double> speed;
                if (s.state == engine::PlaybackState::Playing)
                    speed = speedWindow.add(s.playback_microseconds, s.intended_timeline_microseconds, s.effective_speed_frontier_microseconds,
                        s.processed_events, synchronized, (now - t0) * 1000000 / freq);
                SnapRow row;
                row.wall_ms = static_cast<double>(now - t0) * 1e3 / static_cast<double>(freq);
                row.playback_us = s.playback_microseconds;
                row.timeline_us = s.intended_timeline_microseconds;
                row.frontier_us = s.effective_speed_frontier_microseconds;
                row.processed = s.processed_events;
                row.dropped = s.dropped_events + s.gate_filtered_events;
                row.current_lag_us = s.current_lag_microseconds;
                row.max_lag_us = s.maximum_lag_microseconds;
                row.has_speed = speed.has_value();
                row.speed = speed.value_or(0.0);
                snaps.push_back(row);
            }
            if (p.passage && !warm && measuring != nullptr && measuring->done()) { reason = "completed"; break; }
            if (endedAt.load() != 0 && !engine.has_live_worker()) break;
            if (warm && elapsedSeconds > 0.4) { reason = "warm-stopped"; break; }
            if (elapsedSeconds > limitSeconds) { reason = p.passage ? "CAPPED" : "TIMEOUT"; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int64_t finished = qpc();
        if (uiThread.joinable()) {
            uiStop.store(true);
            uiThread.join();
        }
        const CpuSnapshot cpu1 = CpuSnapshot::take();
        if (profile) {
            profiler.stop();
            if (!profiler.report(30)) say("profile-unavailable=dbghelp could not be loaded");
        }
        engine::PlaybackSnapshot snap = engine.snapshot();
        engine.stop();
        engine::PlaybackSnapshot after = engine.snapshot();
        engine.dispose();
        int64_t endStamp = endedAt.load() != 0 ? endedAt.load() : finished;
        if (p.passage && measuring != nullptr && measuring->done()) endStamp = measuring->done_stamp();
        endedMs = static_cast<double>(endStamp - t0) * 1e3 / static_cast<double>(freq);
        info.push_back("end_reason=" + reason);
        info.push_back("ended_ms=" + to_str(endedMs, 2));
        info.push_back("processed_events=" + std::to_string(snap.processed_events));
        info.push_back("dropped_events=" + std::to_string(snap.dropped_events));
        info.push_back("gate_filtered_events=" + std::to_string(snap.gate_filtered_events));
        info.push_back("engine_max_lag_us=" + std::to_string(snap.maximum_lag_microseconds));
        info.push_back("engine_current_lag_us=" + std::to_string(snap.current_lag_microseconds));
        info.push_back("processed_events_final=" + std::to_string(after.processed_events));
        info.push_back("playback_failed=" + (failed.load() ? failure : std::string("no")));
        info.push_back(std::string("emulated_monitor=") + (emulateMonitor ? "on" : "off"));
        info.push_back(std::string("emulated_override=") + (emulateOverride ? "channel 1 Volume 100" : "off"));
        if (uiHz > 0 && !warm) {
            info.push_back("ui_hz=" + to_str(uiHz, 1));
            info.push_back("ui_calls=" + std::to_string(uiCallMs.size()));
            info.push_back("ui_call_p50_ms=" + to_str(percentile_ms(uiCallMs, 0.50), 3));
            info.push_back("ui_call_p99_ms=" + to_str(percentile_ms(uiCallMs, 0.99), 3));
            info.push_back("ui_call_max_ms=" + to_str(percentile_ms(uiCallMs, 1.0), 3));
            info.push_back("ui_tick_gap_max_ms=" + to_str(uiTickGapMaxMs, 2));
            info.push_back("send_stall_max_ms=" + to_str(sendStallMaxMs, 2));
        }
        for (const std::string& l : cpu_lines(cpu0, cpu1)) info.push_back(l);
    };

    // The label of one run: the template with {path} and {pass}, or the plain label (plus -path and -pN when several).
    auto run_label = [&](size_t outputIndex, int passageIndex) {
        const std::string templ = opt.get("label-template");
        const std::string passText = passageIndex > 0 ? std::to_string(passageIndex) : std::string("all");
        if (!templ.empty()) return replace_all(replace_all(templ, "{path}", tags[outputIndex]), "{pass}", passText);
        std::string l = label;
        if (kinds.size() > 1) l += "-" + tags[outputIndex];
        if (passageIndex > 0) l += "-p" + passText;
        return l;
    };

    const size_t passageCount = passageMode ? windows.size() : 1;
    for (size_t oi = 0; oi < kinds.size(); ++oi) {
        const std::string& kind = kinds[oi];
        OpenedOutput opened;
        try { opened = open_output(kind, opt.get("device-name", "OmniMIDI"), std::atof(opt.get("paced-mps", "15").c_str()) * 1e6); }
        catch (const std::exception& ex) {
            say("output " + tags[oi] + " (" + kind + ") could not be opened: " + ex.what());
            tools::write_file(outDir + "\\" + run_label(oi, 0) + ".error.txt", std::string("open failed: ") + ex.what() + "\n");
            continue;
        }
        say("device-measure v2 " + arch + " output=" + kind + " path=" + tags[oi] + " fixture=" + fixture + " events=" + std::to_string(events) +
            " load_s=" + to_str(loadSeconds, 1) + " identity=[" + opened.identity + "] winmm_module=" + win32::WinMmMidiOutput::loaded_module_path());

        if (opt.num("warm", 1) != 0) {
            std::vector<std::string> ignore;
            std::vector<SnapRow> ignoreSnaps;
            int64_t t0 = 0, epoch = 0;
            double ms = 0;
            PlayParams wp = make_params(passageMode ? &windows[0] : nullptr);
            play(opened.output, wp, true, ignore, nullptr, t0, ms, epoch, ignoreSnaps);
            std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
        }

        for (size_t pi = 0; pi < passageCount; ++pi) {
            const Window* window = passageMode ? &windows[pi] : nullptr;
            PlayParams params = make_params(window);
            if (params.passage) params.limit_seconds = params.window_seconds + capExtras[oi];
            const std::string runName = run_label(oi, passageMode ? static_cast<int>(pi) + 1 : 0);
            for (const std::string& instrName : instrNames) {
            Instr mode = instrName == "none" ? Instr::None : instrName == "count" ? Instr::Count : instrName == "tsall" ? Instr::TsAll : Instr::Sample;
            if (passageMode && mode == Instr::None) mode = Instr::Count;   // the passage needs the decorator to know when it is over
            const std::string instrRunName = instrNames.size() > 1 ? runName + "-i" + instrName : runName;
            for (int r = 1; r <= repeats; ++r) {
                std::vector<std::string> info;
                info.push_back("program=v2");
                info.push_back("arch=" + arch);
                info.push_back("output=" + kind);
                info.push_back("path=" + tags[oi]);
                info.push_back("fixture=" + fixture);
                info.push_back("repeat=" + std::to_string(r));
                info.push_back("instr=" + instrName);
                info.push_back("events=" + std::to_string(events));
                info.push_back("fixture_duration_ms=" + to_str(static_cast<double>(durationUs) / 1000.0, 1));
                info.push_back("calib_qpc_ns=" + to_str(qpcNs, 2));
                info.push_back("calib_lock_ns=" + to_str(lockNs, 2));
                info.push_back("load_s=" + to_str(loadSeconds, 1));
                info.push_back("peak_private_mb_after_load=" + mb(memLoad.peak_private));
                info.push_back("peak_working_set_mb_after_load=" + mb(memLoad.peak_working_set));
                info.push_back("timer_resolution_us_before_play=" + to_str(timer_resolution_us(), 1));
                info.push_back("identity=" + opened.identity);
                info.push_back("winmm_module=" + win32::WinMmMidiOutput::loaded_module_path());
                info.push_back("mode=" + modeName);
                if (opened.counting) opened.counting->clear();
                if (opened.recording) { opened.recording->messages.clear(); opened.recording->messages.reserve(static_cast<size_t>(events) + 64); }
                if (params.passage) {
                    info.push_back("passage_index=" + std::to_string(pi + 1));
                    info.push_back("window_start_s=" + to_str(static_cast<double>(params.start_us) / 1e6, 3));
                    info.push_back("window_end_s=" + to_str(static_cast<double>(params.end_us) / 1e6, 3));
                    info.push_back("mark_s=" + (params.mark_us < 0 ? std::string("none") : to_str(static_cast<double>(params.mark_us) / 1e6, 3)));
                    info.push_back("window_events=" + std::to_string(params.window_events));
                    info.push_back("passage_events=" + std::to_string(params.passage_events));
                    info.push_back("rate_model=None");
                    info.push_back(std::string("chase_midi_state_on_play_seek=") + (params.chase ? "on" : "off"));
                    if (params.have_held) {
                        info.push_back("held_at_start_voices=" + std::to_string(params.held_start.voices));
                        info.push_back("held_at_start_distinct=" + std::to_string(params.held_start.distinct));
                        info.push_back("pedal_down_channels_at_start=" + std::to_string(params.held_start.pedal_channels));
                        info.push_back("held_at_mark_voices=" + std::to_string(params.held_mark.voices));
                        info.push_back("held_at_mark_distinct=" + std::to_string(params.held_mark.distinct));
                        info.push_back("pedal_down_channels_at_mark=" + std::to_string(params.held_mark.pedal_channels));
                    }
                    info.push_back("processing_mode=Queue (no rate model, no queue limit, simulate slowdown off)");
                    info.push_back(std::string("instrumentation=") + instrName + (instrName == "count" ? " (a counter per send; no timer reads except the first and last send)" : instrName == "sample" ? " (two timer reads on every " + std::to_string(every) + "th send)" : ""));
                    info.push_back("chase_expected=" + std::to_string(params.chase_expected));
                    info.push_back("wall_cap_s=" + to_str(params.limit_seconds, 1));
                }
                std::shared_ptr<MeasuringOutput> measuring;
                std::shared_ptr<platform::MidiOutput> output = opened.output;
                if (mode != Instr::None) {
                    measuring = std::make_shared<MeasuringOutput>(opened.output, mode, every, params.passage ? params.window_events : events,
                        params.passage ? params.chase_expected : 0);
                    output = measuring;
                }
                int64_t t0 = 0, epoch = 0;
                double endedMs = 0;
                std::vector<SnapRow> snaps;
                play(output, params, false, info, measuring.get(), t0, endedMs, epoch, snaps);
                if (opened.counting) {
                    char hashText[32];
                    std::snprintf(hashText, sizeof hashText, "%016llx", static_cast<unsigned long long>(opened.counting->hash));
                    info.push_back("output_shorts=" + std::to_string(opened.counting->shorts));
                    info.push_back("output_longs=" + std::to_string(opened.counting->longs));
                    info.push_back(std::string("output_hash=") + hashText);
                }
                if (opened.recording) info.push_back("output_messages=" + std::to_string(opened.recording->messages.size()));
                info.push_back("engine_epoch_after_t0_us=" + to_str(static_cast<double>(epoch) * 1e6 / static_cast<double>(qpc_frequency()), 1));
                const int64_t sentInWindow = measuring ? measuring->window_sends() : events;
                info.push_back("throughput_over_ended_meps=" + to_str(endedMs > 0 ? static_cast<double>(sentInWindow) / endedMs / 1000.0 : 0.0, 4));
                const std::string base = outDir + "\\" + instrRunName + ".r" + std::to_string(r);
                const win32::ProcessMemory memRun = win32::process_memory();
                info.push_back("peak_private_mb_after_run=" + mb(memRun.peak_private));
                info.push_back("peak_working_set_mb_after_run=" + mb(memRun.peak_working_set));
                if (measuring) {
                    info.push_back("sends=" + std::to_string(measuring->window_sends()));
                    if (params.passage) {
                        const std::string endReason = [&] { for (const std::string& l : info) if (l.rfind("end_reason=", 0) == 0) return l.substr(11); return std::string(); }();
                        info.push_back("capped=" + std::string(endReason == "CAPPED" ? "yes" : "no"));
                        info.push_back("window_sends=" + std::to_string(measuring->window_sends()));
                        info.push_back("window_complete=" + std::string(measuring->window_sends() >= params.window_events ? "yes" : "no"));
                        info.push_back("extra_sends_after_window=" + std::to_string(measuring->extra_sends()));
                        info.push_back("chase_sends=" + std::to_string(measuring->chase_sends()));
                        if (params.have_held && params.chase && params.start_index > 0) {
                            // Did the chase restore what the file had set before the start position? (per channel: the latest
                            // CC7 volume, CC11 expression, CC64 sustain, program and bank). A channel that had a Reset All
                            // Controllers (CC121) before the position may legitimately differ for CC11 and CC64.
                            const MeasuringOutput::ChaseValues& sent = measuring->chase_values();
                            auto check = [&](const char* name, const int16_t* want, const int16_t* got) {
                                int wanted = 0, restored = 0, resetExempt = 0;
                                for (int c = 0; c < 16; ++c) {
                                    if (want[c] < 0) continue;
                                    ++wanted;
                                    if (got[c] == want[c]) ++restored;
                                    else if ((params.held_start.reset_channels >> c) & 1) ++resetExempt;
                                }
                                info.push_back(std::string("chase_restored_") + name + "=" + std::to_string(restored) + "_of_" + std::to_string(wanted) +
                                    (resetExempt ? "_(" + std::to_string(resetExempt) + "_differ_after_a_reset_all_controllers)" : std::string()));
                            };
                            check("cc7_volume", params.held_start.cc7, sent.cc7);
                            check("cc11_expression", params.held_start.cc11, sent.cc11);
                            check("cc64_sustain", params.held_start.cc64, sent.cc64);
                            check("program", params.held_start.program, sent.program);
                            check("bank_msb", params.held_start.bank_msb, sent.bank_msb);
                            check("bank_lsb", params.held_start.bank_lsb, sent.bank_lsb);
                        }
                        // Every send the output saw is either a chase message, a window event or an event after the window;
                        // the engine counts only the scheduled ones.
                        std::string finalProcessed;
                        for (const std::string& l : info) if (l.rfind("processed_events_final=", 0) == 0) finalProcessed = l.substr(23);
                        const long long scheduled = measuring->window_sends() + measuring->extra_sends();
                        info.push_back("chase_check=" + std::string(finalProcessed == std::to_string(scheduled) ? "ok" : "processed_events_final differs from the scheduled sends counted"));
                    }
                    if (mode == Instr::TsAll)
                        info.push_back("tsall_mean_send_ns=" + to_str(static_cast<double>(measuring->sum_dur()) * 1e9 / static_cast<double>(qpc_frequency()) /
                            static_cast<double>(std::max<int64_t>(1, measuring->count())) - qpcNs, 1));
                    tools::SampleRun run;
                    std::memset(&run.header, 0, sizeof run.header);
                    std::memcpy(run.header.magic, "MBDEVS01", 8);
                    run.header.version = 1;
                    run.header.sample_every = mode == Instr::Sample ? every : (mode == Instr::TsAll ? 128u : 0u);
                    run.header.frequency = qpc_frequency();
                    run.header.total_sends = measuring->window_sends();
                    run.header.qpc_cost_ns = qpcNs;
                    run.header.expected_events = params.passage ? params.window_events : events;
                    run.header.reserved[0] = epoch;   // the engine's time zero, in ticks after t0
                    for (const MeasuringOutput::Raw& s : measuring->samples()) {
                        tools::SampleRecord rec;
                        rec.k = s.k;
                        // Intended time counted from the start position (a passage starts the engine's clock there).
                        const int64_t index = static_cast<int64_t>(params.start_index) + s.k;
                        rec.intended_us = index < events ? loaded.playback->events.at(static_cast<int32_t>(index)).micros - params.start_us : 0;
                        rec.before_rel_ticks = s.before - t0;
                        rec.duration_ticks = s.dur;
                        rec.reserved = 0;
                        run.samples.push_back(rec);
                    }
                    if (mode != Instr::Count && !tools::write_sample_file(base + ".samples.bin", run))
                        info.push_back("sample_file_error=cannot write");
                    else if (mode == Instr::Count) {
                        // Count mode keeps only the first and the last send: enough for the throughput figure.
                        if (run.samples.size() >= 2) {
                            const double spanUs = static_cast<double>(run.samples.back().before_rel_ticks - run.samples.front().before_rel_ticks) * 1e6 /
                                static_cast<double>(qpc_frequency());
                            info.push_back("throughput_first_to_last_send_meps=" + to_str(spanUs > 0 ? static_cast<double>(run.samples.back().k - run.samples.front().k) / spanUs : 0.0, 4));
                        }
                    }
                }
                if (params.passage && !snaps.empty()) {
                    // The player's own readouts, sampled every 16 ms: Maximum lag / Current lag / Effective speed.
                    // max_lag_* is the engine's statistic (largest lag of any event sent since the run started, reset at Start).
                    const SnapRow& lastRow = snaps.back();
                    info.push_back("max_lag_at_last_look_us=" + std::to_string(lastRow.max_lag_us));
                    if (params.mark_us >= 0) {
                        for (const SnapRow& row : snaps)
                            if (row.timeline_us >= params.mark_us) {
                                info.push_back("max_lag_at_mark_us=" + std::to_string(row.max_lag_us));
                                info.push_back("current_lag_at_mark_us=" + std::to_string(row.current_lag_us));
                                break;
                            }
                    }
                    auto speedStats = [&](const std::string& prefix, int64_t fromTimeline) {
                        std::vector<double> v;
                        const SnapRow* first = nullptr;
                        const SnapRow* last = nullptr;
                        for (const SnapRow& row : snaps) {
                            if (row.timeline_us < fromTimeline) continue;
                            if (!first) first = &row;
                            last = &row;
                            if (row.has_speed) v.push_back(row.speed);
                        }
                        if (!v.empty()) {
                            double sum = 0;
                            for (double x : v) sum += x;
                            info.push_back(prefix + "_min_pct=" + to_str(*std::min_element(v.begin(), v.end()), 1));
                            info.push_back(prefix + "_p5_pct=" + to_str(percentile_of(v, 5), 1));
                            info.push_back(prefix + "_p50_pct=" + to_str(percentile_of(v, 50), 1));
                            info.push_back(prefix + "_mean_pct=" + to_str(sum / static_cast<double>(v.size()), 1));
                            info.push_back(prefix + "_last_pct=" + to_str(v.back(), 1));
                        }
                        if (first && last && last->playback_us > first->playback_us)
                            info.push_back(prefix + "_overall_pct=" + to_str(100.0 * static_cast<double>(last->frontier_us - first->frontier_us) /
                                static_cast<double>(last->playback_us - first->playback_us), 1));
                    };
                    speedStats("eff_speed", 0);
                    if (params.mark_us >= 0) speedStats("eff_speed_passage", params.mark_us);
                    std::string csv = "wall_ms,playback_us,timeline_us,frontier_us,processed,dropped,current_lag_us,max_lag_us,speed_pct\n";
                    for (const SnapRow& row : snaps)
                        csv += to_str(row.wall_ms, 1) + "," + std::to_string(row.playback_us) + "," + std::to_string(row.timeline_us) + "," +
                            std::to_string(row.frontier_us) + "," + std::to_string(row.processed) + "," + std::to_string(row.dropped) + "," +
                            std::to_string(row.current_lag_us) + "," + std::to_string(row.max_lag_us) + "," + (row.has_speed ? to_str(row.speed, 1) : std::string()) + "\n";
                    tools::write_file(base + ".snapshots.csv", csv);
                }
                std::string text;
                for (const std::string& l : info) text += l + "\n";
                tools::write_file(base + ".run.txt", text);
                say("repeat " + std::to_string(r) + " " + instrRunName + ": ended_ms=" + to_str(endedMs, 1) + " throughput_over_ended_meps=" +
                    to_str(endedMs > 0 ? static_cast<double>(sentInWindow) / endedMs / 1000.0 : 0.0, 3));
                std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
            }
            }
        }
        // The driver can hang in reset or close after a flood; the watchdog then ends the process (the script runs the
        // outputs that were not reached again, in a new process).
        cleanup_with_watchdog(outDir + "\\" + run_label(oi, 0) + ".cleanup.txt", [&] { opened.output->reset(); }, [&] { opened.output->close(); });
        if (oi + 1 < kinds.size()) std::this_thread::sleep_for(std::chrono::milliseconds(pathSettleMs));
    }
    return 0;
}

int cmd_outputs()
{
    // The output kinds this build can play through (the run script asks, and skips a path whose kind is missing).
    say("outputs=null,count,recording,winmm,winmm-direct,kdmapi");   // winmm-direct: DD-085, v2 only (v1's driver does not list it, so the run script skips it for v1 with a note)
    return 0;
}

// ---------------------------------------------------------------- raw ceilings (no engine)

void pin_above_normal() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }

std::vector<uint32_t> message_table()
{
    std::vector<uint32_t> m(4096);
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = static_cast<uint32_t>(0xB0 | (i & 15)) | (static_cast<uint32_t>(20 + (i / 16) % 12) << 8) | (static_cast<uint32_t>((i * 7) & 127) << 16);
    return m;
}

// Calls `send` in a tight loop for `seconds`; returns millions of sends per second.
template <typename F>
double tight_loop(double seconds, F&& send)
{
    const std::vector<uint32_t> msgs = message_table();
    const int64_t freq = qpc_frequency();
    const int64_t start = qpc();
    const int64_t end = start + static_cast<int64_t>(seconds * static_cast<double>(freq));
    uint64_t n = 0;
    int64_t now = start;
    while (now < end) {
        for (int i = 0; i < 65536; ++i) send(msgs[static_cast<size_t>(i) & 4095]);
        n += 65536;
        now = qpc();
    }
    return static_cast<double>(n) / (static_cast<double>(now - start) / static_cast<double>(freq)) / 1e6;
}

int cmd_raw(const Options& opt)
{
    const std::string kind = opt.get("output");
    const std::string layer = opt.get("layer", "output");
    const double seconds = std::atof(opt.get("seconds", "2").c_str());
    const int repeats = static_cast<int>(opt.num("repeats", 3));
    const std::string deviceName = opt.get("device-name", "OmniMIDI");
    const std::string arch = sizeof(void*) == 8 ? "x64" : "x86";
    pin_above_normal();
    std::vector<double> rates;
    std::string identity;
    std::function<void()> cleanup_reset = [] {};
    std::function<void()> cleanup_close = [] {};
    std::shared_ptr<platform::MidiOutput> keep_alive;
    std::shared_ptr<win32::DynamicKdmApiNative> keep_native;
    if (kind == "winmm" && layer == "api") {
        // The bare winmm call: our own handle, midiOutShortMsg straight from a loop.
        int id = -1;
        for (const platform::OutputDeviceInfo& d : win32::WinMmMidiOutput::enumerate_devices())
            if (d.name == deviceName) { id = static_cast<int>(d.device_id); break; }
        if (id < 0) throw std::runtime_error("WinMM device not found");
        HMIDIOUT h = nullptr;
        if (midiOutOpen(&h, static_cast<UINT>(id), 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) throw std::runtime_error("midiOutOpen failed");
        identity = "midiOutShortMsg direct, winmm_module=" + win32::WinMmMidiOutput::loaded_module_path();
        tight_loop(0.3, [&](uint32_t m) { midiOutShortMsg(h, m); });
        for (int r = 0; r < repeats; ++r) { rates.push_back(tight_loop(seconds, [&](uint32_t m) { midiOutShortMsg(h, m); })); std::this_thread::sleep_for(std::chrono::milliseconds(1000)); }
        cleanup_reset = [h] { midiOutReset(h); };
        cleanup_close = [h] { midiOutClose(h); };
    }
    else if (kind == "kdmapi" && (layer == "api" || layer == "native")) {
        // The provider's own SendDirectData through v2's KdmApiNative (one virtual call plus the function pointer).
        platform::OutputStatus status;
        std::unique_ptr<win32::DynamicKdmApiNative> native = win32::DynamicKdmApiNative::load(L"", status);
        if (!native) throw std::runtime_error("KDMAPI load failed: " + status.message);
        if (!native->is_available() || !native->initialize_stream()) throw std::runtime_error("KDMAPI initialize failed");
        identity = "KdmApiNative::send_short, provider=" + native->provider_path();
        tight_loop(0.3, [&](uint32_t m) { native->send_short(m); });
        for (int r = 0; r < repeats; ++r) { rates.push_back(tight_loop(seconds, [&](uint32_t m) { native->send_short(m); })); std::this_thread::sleep_for(std::chrono::milliseconds(1000)); }
        win32::DynamicKdmApiNative* nat = native.get();
        keep_native.reset(native.release());
        cleanup_reset = [nat] { nat->reset_stream(); };
        cleanup_close = [nat] { nat->terminate_stream(); };
    }
    else if ((kind == "winmm" || kind == "winmm-direct" || kind == "kdmapi" || kind == "null") && layer == "output") {
        // v2's MidiOutput::send_short through the abstract interface, as the engine calls it.
        OpenedOutput o = open_output(kind, deviceName);
        platform::MidiOutput* out = o.output.get();
        identity = "MidiOutput::send_short " + o.identity;
        tight_loop(0.3, [&](uint32_t m) { out->send_short(m); });
        for (int r = 0; r < repeats; ++r) { rates.push_back(tight_loop(seconds, [&](uint32_t m) { out->send_short(m); })); std::this_thread::sleep_for(std::chrono::milliseconds(1000)); }
        cleanup_reset = [out] { out->reset(); };
        cleanup_close = [out] { out->close(); };
        keep_alive = o.output;
    }
    else throw std::runtime_error("unsupported --output/--layer combination");
    std::string line = "RAW program=v2 arch=" + arch + " output=" + kind + " layer=" + layer + " seconds=" + to_str(seconds, 1) + " identity=[" + identity + "] meps=";
    for (size_t i = 0; i < rates.size(); ++i) line += (i ? "," : "") + to_str(rates[i], 3);
    say(line);
    // Without --cleanup-log the log goes beside this program (v2\out\build-*\Release, git-ignored), never into the
    // caller's current folder (a probe once wrote raw.cleanup.txt into the repository root).
    std::string cleanupLog = opt.get("cleanup-log");
    if (cleanupLog.empty()) {
        wchar_t module[MAX_PATH * 2] = {};
        const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH * 2);
        std::wstring folder(module, length);
        const size_t slash = folder.find_last_of(L"\\/");
        folder = slash == std::wstring::npos ? std::wstring(L".") : folder.substr(0, slash);
        cleanupLog = win32::wide_to_utf8(folder) + "\\raw.cleanup.txt";
    }
    cleanup_with_watchdog(cleanupLog, cleanup_reset, cleanup_close);
    return 0;
}

// DD-085 stress: repeated open / flood / end, the end being Stop, a plain close, or a panic during the flood. Only
// non-sounding fixtures (controllers 20..31). Prints one summary line; a hung call ends the process through the watchdog.
int cmd_direct_stress(const Options& opt)
{
    const std::string fixture = opt.get("fixture");
    const std::string deviceName = opt.get("device-name", "OmniMIDI");
    const int cycles = static_cast<int>(opt.num("cycles", 200));
    const int floodMs = static_cast<int>(opt.num("flood-ms", 100));
    const std::string mode = opt.get("mode", "stop");
    // DD-094 comparison on a real driver: how the device is reset before MODM_CLOSE (and at Stop): reset = MODM_RESET
    // (the player's way), notesoff = the 48 all-sound-off / all-notes-off / sustain-off messages instead of MODM_RESET,
    // settle = MODM_RESET after waiting --settle-ms (default 250) for the driver to drain its queue.
    const std::string closeStyle = opt.get("close-style", "reset");
    if (closeStyle != "reset" && closeStyle != "notesoff" && closeStyle != "settle")
        throw std::runtime_error("--close-style must be reset, notesoff or settle");
    const int settleMs = closeStyle == "settle" ? static_cast<int>(opt.num("settle-ms", 250)) : 0;
    LoadedSong loaded = load_fixture(fixture);
    std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();
    int64_t deviceId = -1;
    for (const platform::OutputDeviceInfo& d : win32::WinMmMidiOutput::enumerate_devices())
        if (d.name == deviceName) { deviceId = d.device_id; break; }
    if (deviceId < 0) throw std::runtime_error("WinMM device not found: " + deviceName);

    // DD-094: where the cycles are, for the watchdog. A hang then says which cycle, which step and whether a call was
    // still inside the driver (a step on this thread that blocks inside the driver shows "calls in progress 0" for
    // open and close, whose driver calls are not counted, and 1 for the reset and panic of "dispose").
    enum Step { StepOpen, StepStart, StepFlood, StepStop, StepDispose, StepClose, StepBetween };
    static const char* const kStepNames[] = { "open (MODM_OPEN)", "engine start", "flood", "engine Stop",
        "engine dispose (reset and panic on this thread when the worker has ended)", "close (device reset, then MODM_CLOSE)", "between cycles" };
    struct Progress {
        std::atomic<int64_t> last{ 0 };
        std::atomic<int> cycle{ 0 };
        std::atomic<int> step{ 0 };   // a Step; set below
        std::mutex mutex;
        std::shared_ptr<win32::WinMmDirectOutput> output;   // the cycle's output, for its gate counters
    };
    std::shared_ptr<Progress> progress = std::make_shared<Progress>();
    progress->last.store(qpc());
    progress->step.store(StepBetween);
    auto mark = [&](int c, Step step) { progress->cycle.store(c); progress->step.store(step); };
    std::thread([progress, cycles] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (static_cast<double>(qpc() - progress->last.load()) / static_cast<double>(qpc_frequency()) > 30.0) {
                uint32_t inProgress = 0, control = 0, controlMs = 0;
                int32_t controlAfterData = -1;
                int64_t sinceData = -1;
                bool pending = false;
                {
                    std::lock_guard<std::mutex> lock(progress->mutex);
                    if (progress->output) {
                        inProgress = progress->output->calls_in_progress();
                        pending = progress->output->close_pending();
                        control = progress->output->control_call_in_progress();
                        controlMs = progress->output->control_call_elapsed_milliseconds();
                        controlAfterData = progress->output->control_call_after_data_milliseconds();
                        sinceData = progress->output->milliseconds_since_last_data();
                    }
                }
                // DD-094: a reset or close of the driver has its own time limit, so the call named here is one without
                // a limit (MODM_OPEN, the dispose's panic) unless "control call" names one.
                say("DIRECT-STRESS WATCHDOG: no cycle finished within 30 s; process ended (cycle " + std::to_string(progress->cycle.load() + 1) +
                    " of " + std::to_string(cycles) + ", step: " + kStepNames[progress->step.load()] + ", driver calls in progress: " +
                    std::to_string(inProgress) + ", close pending: " + (pending ? "yes" : "no") + ", control call: " +
                    (control == 0 ? std::string("none") : std::string(win32::WinMmDirectOutput::call_name(control)) + " for " +
                        std::to_string(controlMs) + " ms, started " + std::to_string(controlAfterData) + " ms after the last MODM_DATA returned") +
                    ", last MODM_DATA returned " + std::to_string(sinceData) + " ms ago)");
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    }).detach();

    int failures = 0, openFailures = 0, startFailures = 0, stopTimeouts = 0, deferredCloses = 0, cyclesRun = 0;
    double maxOpenMs = 0, maxStopMs = 0, maxCloseMs = 0, sumOpenMs = 0, sumStopMs = 0, sumCloseMs = 0;
    uint64_t busyWaits = 0;
    int64_t processed = 0;
    std::string firstProblem, endedEarly, stoppedCall;
    int32_t stoppedAfterDataMs = -1;
    // DD-111: --keep-open 1 plays every cycle on ONE output object, as the player does since Stop keeps the device
    // open: each cycle's open() finds the device open and does nothing, Stop resets it, and the device is closed once at the
    // end (modes stop and panic; mode close still closes at the end of every cycle).
    const bool keepOpen = opt.num("keep-open", 0) != 0;
    std::shared_ptr<win32::WinMmDirectOutput> keptOutput;
    for (int c = 0; c < cycles; ++c) {
        ++cyclesRun;
        std::shared_ptr<win32::WinMmDirectOutput> out = keepOpen && keptOutput ? keptOutput : std::make_shared<win32::WinMmDirectOutput>();
        if (keepOpen) keptOutput = out;
        if (!keepOpen || c == 0) {
            out->set_track_data_time(true);
            out->set_reset_style(closeStyle == "notesoff" ? win32::WinMmDirectOutput::ResetStyle::NotesOff
                                                          : win32::WinMmDirectOutput::ResetStyle::DriverReset, settleMs);
        }
        { std::lock_guard<std::mutex> lock(progress->mutex); progress->output = out; }
        mark(c, StepOpen);
        int64_t t0 = qpc();
        platform::OutputStatus s = out->open(static_cast<uint32_t>(deviceId));
        const double openMs = static_cast<double>(qpc() - t0) * 1e3 / static_cast<double>(qpc_frequency());
        sumOpenMs += openMs; maxOpenMs = std::max(maxOpenMs, openMs);
        if (!s.ok()) { ++openFailures; ++failures; if (firstProblem.empty()) firstProblem = "open: " + s.message; progress->last.store(qpc()); continue; }
        engine::PlaybackEngine eng(services);
        std::atomic<int> failed{ 0 };
        std::string failure;
        eng.set_playback_failed_handler([&](const engine::EngineError& e) { failure = e.type + ": " + e.message; failed.store(1); });
        mark(c, StepStart);
        engine::EngineError startError = eng.start(loaded.playback, out, engine::ProcessingMode::Queue, 0, false);
        if (!startError.ok()) { ++startFailures; ++failures; if (firstProblem.empty()) firstProblem = "start: " + startError.message; out->close(); progress->last.store(qpc()); continue; }
        mark(c, StepFlood);
        std::this_thread::sleep_for(std::chrono::milliseconds(floodMs));
        double stopMs = 0, closeMs = 0;
        bool timedOut = false;
        // "panic": a panic from another thread would race the single sender; the engine's own Stop does reset + panic
        // from its worker, so "panic during a flood" is the Stop of this mode, measured the same way.
        // "close": no Stop; the engine is disposed together with the close (below), right after the flood.
        if (mode == "panic" || mode == "stop") {
            mark(c, StepStop);
            int64_t a = qpc();
            engine::EngineError e = eng.stop();
            stopMs = static_cast<double>(qpc() - a) * 1e3 / static_cast<double>(qpc_frequency());
            if (!e.ok()) {
                ++failures;
                if (e.type == "PlaybackWorkerTimeoutException") { timedOut = true; ++stopTimeouts; }
                if (firstProblem.empty()) firstProblem = "stop (cycle " + std::to_string(c + 1) + "): " + e.message;
            }
        }
        processed += eng.snapshot().processed_events;
        {
            int64_t a = qpc();
            mark(c, StepDispose);
            engine::EngineError e = eng.dispose();
            if (!e.ok()) {
                if (!timedOut) ++failures;
                if (e.type == "PlaybackWorkerTimeoutException" || eng.has_live_worker()) timedOut = true;
                if (firstProblem.empty()) firstProblem = "dispose (cycle " + std::to_string(c + 1) + "): " + e.message;
            }
            mark(c, StepClose);
            // DD-094: if the worker is still inside the driver, close() waits a bounded time and then leaves the reset
            // and close to that worker instead of closing underneath its call.
            // (--keep-open: Stop leaves the device open, as in the player; the device is closed after the last cycle)
            platform::OutputStatus closed = keepOpen && mode != "close" ? platform::OutputStatus::success() : out->close();
            if (!closed.ok()) {
                if (!out->driver_stopped_answering()) ++deferredCloses;
                if (firstProblem.empty()) firstProblem = "close (cycle " + std::to_string(c + 1) + "): " + closed.message;
            }
            closeMs = static_cast<double>(qpc() - a) * 1e3 / static_cast<double>(qpc_frequency());
        }
        mark(c, StepBetween);
        if (!keepOpen) busyWaits += out->busy_waits();
        sumStopMs += stopMs; maxStopMs = std::max(maxStopMs, stopMs);
        sumCloseMs += closeMs; maxCloseMs = std::max(maxCloseMs, closeMs);
        if (failed.load()) { ++failures; if (firstProblem.empty()) firstProblem = "playback: " + failure; }
        progress->last.store(qpc());
        if (out->driver_stopped_answering()) {
            // DD-094: a reset or MODM_CLOSE did not return within its limit; the driver is not called again in this process.
            stoppedCall = win32::WinMmDirectOutput::call_name(out->stopped_answering_call());
            stoppedAfterDataMs = out->stopped_answering_after_data_milliseconds();
            endedEarly = "cycle " + std::to_string(c + 1) + " (the driver stopped answering in " + stoppedCall + ", which started " +
                std::to_string(stoppedAfterDataMs) + " ms after the last MODM_DATA returned)";
            break;
        }
        if (timedOut || out->close_pending()) {
            // A call is still inside the driver: opening the device again now would make a second client of it.
            endedEarly = "cycle " + std::to_string(c + 1) + " (a driver call did not return in time)";
            break;
        }
    }
    uint32_t keptOpens = 0;
    if (keepOpen && keptOutput) {
        keptOpens = keptOutput->kept_open_count();
        mark(cyclesRun, StepClose);
        int64_t a = qpc();
        platform::OutputStatus closed = keptOutput->close();   // the one close of the whole run
        busyWaits = keptOutput->busy_waits();   // (the shared output counts across cycles)
        sumCloseMs += static_cast<double>(qpc() - a) * 1e3 / static_cast<double>(qpc_frequency());
        if (!closed.ok()) { ++failures; if (!keptOutput->driver_stopped_answering()) ++deferredCloses; if (firstProblem.empty()) firstProblem = "final close: " + closed.message; }
        if (keptOutput->driver_stopped_answering() && stoppedCall.empty()) stoppedCall = win32::WinMmDirectOutput::call_name(keptOutput->stopped_answering_call());
        mark(cyclesRun, StepBetween);
    }
    const int n = std::max(1, cyclesRun);
    say("DIRECT-STRESS device=\"" + deviceName + "\" mode=" + mode + " close_style=" + closeStyle +
        (keepOpen ? " keep_open=1 opens_answered_by_the_open_device=" + std::to_string(keptOpens) : std::string()) +
        (settleMs > 0 ? " settle_ms=" + std::to_string(settleMs) : std::string()) + " cycles=" + std::to_string(cyclesRun) +
        (cyclesRun != cycles ? " cycles_requested=" + std::to_string(cycles) : std::string()) + " flood_ms=" + std::to_string(floodMs) +
        " failures=" + std::to_string(failures) + " open_failures=" + std::to_string(openFailures) + " start_failures=" + std::to_string(startFailures) +
        " open_ms_mean=" + to_str(sumOpenMs / n, 2) + " open_ms_max=" + to_str(maxOpenMs, 2) +
        " stop_ms_mean=" + to_str(sumStopMs / n, 2) + " stop_ms_max=" + to_str(maxStopMs, 2) +
        " close_ms_mean=" + to_str(sumCloseMs / n, 2) + " close_ms_max=" + to_str(maxCloseMs, 2) +
        " events_processed=" + std::to_string(processed) + " busy_waits=" + std::to_string(busyWaits) +
        " stop_timeouts=" + std::to_string(stopTimeouts) + " deferred_closes=" + std::to_string(deferredCloses) +
        " stopped_answering=" + (stoppedCall.empty() ? std::string("0") : "1 stopped_call=[" + stoppedCall + "] stopped_after_data_ms=" +
            std::to_string(stoppedAfterDataMs)) +
        (endedEarly.empty() ? std::string() : " ended_early_at=[" + endedEarly + "]") +
        (firstProblem.empty() ? std::string() : " first_problem=[" + firstProblem + "]"));
    // A worker still inside the driver: end without the normal exit (DLL unloading could wait on that driver).
    if (!endedEarly.empty()) TerminateProcess(GetCurrentProcess(), 1);
    return failures == 0 && deferredCloses == 0 && stoppedCall.empty() ? 0 : 1;
}

int cmd_devices()
{
    say("winmm_module=" + win32::WinMmMidiOutput::loaded_module_path());
    for (const platform::OutputDeviceInfo& d : win32::WinMmMidiOutput::enumerate_devices()) {
        say("device " + std::to_string(d.device_id) + " name=" + d.name);
        // DD-085: whether the direct path could drive it (nothing is opened)
        win32::DirectResolution r = win32::resolve_direct_device(d.device_id);
        say("  direct=" + std::string(r.ok ? "yes driver=" + r.driver_file + (r.verified ? " (tested)" : " (unverified)") + " local=" + std::to_string(r.local_index) + " " + r.lifecycle
                                          : "no reason=[" + r.reason + "]"));
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------- producer/consumer prototype (evidence only)

// `pctest`: ARCHITECTURE.md section 10 question 2 - would a separate producer thread feeding the sender through a
// lock-free ring beat the single worker? This is NOT the engine: it takes the same stored events the engine sends
// (decoded the way the engine's direct path decodes them) and moves them to a simulated output that costs a fixed
// number of nanoseconds per message, once on one thread (what the engine does now: decode and send in one loop) and
// once on two (a producer decodes and pushes to a single-producer single-consumer ring, a consumer pops and sends).
// No device is opened. Prints million messages per second and the processor time used for each form.
uint32_t g_spin_sink = 0;

double calibrate_spin_ns_per_iteration()
{
    const uint32_t iterations = 20000000;
    uint32_t x = 1;
    const int64_t a = qpc();
    for (uint32_t i = 0; i < iterations; ++i) x = x * 1664525u + 1013904223u;
    const int64_t b = qpc();
    g_spin_sink += x;
    return static_cast<double>(b - a) * 1e9 / static_cast<double>(qpc_frequency()) / iterations;
}

inline void simulated_send(uint32_t packed, uint32_t iterations)
{
    uint32_t x = packed;
    for (uint32_t i = 0; i < iterations; ++i) x = x * 1664525u + 1013904223u;
    g_spin_sink += x;
}

#pragma warning(push)
#pragma warning(disable : 4324)   // the padding is the point: head and tail live on separate cache lines
struct SpscRing {
    static const uint32_t kSize = 1u << 16;
    uint32_t slots[kSize];
    alignas(64) std::atomic<uint64_t> head{ 0 };   // written by the producer
    alignas(64) std::atomic<uint64_t> tail{ 0 };   // written by the consumer
};
#pragma warning(pop)

int cmd_pctest(const Options& opt)
{
    const std::string fixture = opt.get("fixture");
    const double sendNs = std::atof(opt.get("send-ns", "30").c_str());
    const uint32_t batch = static_cast<uint32_t>(opt.num("batch", 16));
    const int repeats = static_cast<int>(opt.num("repeats", 3));
    LoadedSong loaded = load_fixture(fixture);
    const EventStore& store = *loaded.playback->events.store;
    const int64_t count = store.count();
    const double nsPerIteration = calibrate_spin_ns_per_iteration();
    const uint32_t iterations = static_cast<uint32_t>(std::max(0.0, sendNs / nsPerIteration));
    static const uint32_t kMask[4] = { 0u, 0xFFu, 0xFFFFu, 0xFFFFFFu };
    auto decode = [&](int64_t i) -> uint32_t {
        const PackedRecord& r = store.records.packed_at(i);
        return r.packed & kMask[r.inline_length()];
    };
    auto cpu_ms = [] {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        return static_cast<double>((CpuSnapshot::u(k) + CpuSnapshot::u(u)) / 10000);
    };
    say("pctest events=" + std::to_string(count) + " send_ns=" + to_str(static_cast<double>(iterations) * nsPerIteration, 1) +
        " batch=" + std::to_string(batch) + " ns_per_iteration=" + to_str(nsPerIteration, 3));
    for (int r = 1; r <= repeats; ++r) {
        // one thread: decode and send in the same loop
        const double cpu0 = cpu_ms();
        int64_t t0 = qpc();
        for (int64_t i = 0; i < count; ++i) simulated_send(decode(i), iterations);
        const double singleSeconds = static_cast<double>(qpc() - t0) / static_cast<double>(qpc_frequency());
        const double singleCpu = cpu_ms() - cpu0;

        // two threads: producer decodes and pushes, consumer pops and sends
        std::unique_ptr<SpscRing> ring(new SpscRing());
        std::atomic<bool> go{ false };
        const double cpu1 = cpu_ms();
        std::thread consumer([&] {
            while (!go.load()) {}
            uint64_t tail = 0;
            uint64_t known = 0;
            while (tail < static_cast<uint64_t>(count)) {
                known = ring->head.load(std::memory_order_acquire);
                if (known == tail) continue;   // spin: a sleeping consumer would add its wake-up delay to every event
                uint64_t stop = std::min<uint64_t>(known, tail + 256);
                while (tail < stop) { simulated_send(ring->slots[tail & (SpscRing::kSize - 1)], iterations); ++tail; }
                ring->tail.store(tail, std::memory_order_release);
            }
        });
        int64_t t1 = qpc();
        go.store(true);
        {
            uint64_t head = 0, published = 0, cachedTail = 0;
            for (int64_t i = 0; i < count; ++i) {
                while (head - cachedTail >= SpscRing::kSize) cachedTail = ring->tail.load(std::memory_order_acquire);
                ring->slots[head & (SpscRing::kSize - 1)] = decode(i);
                ++head;
                if (head - published >= batch) { ring->head.store(head, std::memory_order_release); published = head; }
            }
            ring->head.store(head, std::memory_order_release);
        }
        consumer.join();
        const double ringSeconds = static_cast<double>(qpc() - t1) / static_cast<double>(qpc_frequency());
        const double ringCpu = cpu_ms() - cpu1;
        say("pctest repeat " + std::to_string(r) + ": single_thread_meps=" + to_str(static_cast<double>(count) / singleSeconds / 1e6, 2) +
            " single_cpu_ms=" + to_str(singleCpu, 0) + " ring_meps=" + to_str(static_cast<double>(count) / ringSeconds / 1e6, 2) +
            " ring_cpu_ms=" + to_str(ringCpu, 0));
    }
    return g_spin_sink == 0x12345678u ? 1 : 0;
}

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    try {
        if (args.size() >= 2 && args[1] == "pctest") return cmd_pctest(parse_options(args, 2));
        if (args.size() >= 2 && args[1] == "gen") return cmd_gen(args);
        if (args.size() >= 2 && args[1] == "analyze") return cmd_analyze(args);
        if (args.size() >= 2 && args[1] == "devices") return cmd_devices();
        if (args.size() >= 2 && args[1] == "outputs") return cmd_outputs();
        if (args.size() >= 2 && args[1] == "run") return cmd_run(parse_options(args, 2));
        if (args.size() >= 2 && args[1] == "raw") return cmd_raw(parse_options(args, 2));
        if (args.size() >= 2 && args[1] == "direct-stress") return cmd_direct_stress(parse_options(args, 2));
    }
    catch (const std::exception& ex) {
        tools::write_stdout(std::string("midibn-device-measure ERROR: ") + ex.what() + "\n");
        return 2;
    }
    tools::write_stdout("usage: midibn-device-measure gen|analyze|devices|run|raw ... (see the header of device_measure_main.cpp)\n");
    return 2;
}
