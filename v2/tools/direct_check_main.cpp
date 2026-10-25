// midibn-direct-check: a silent check of "Direct WinMM driver access" (register entry DD-085) for one PC.
//
// It reads Windows' MIDI driver list (Drivers32), asks each user-mode MIDI driver for its devices, says for every
// WinMM output device whether the driver can be reached directly, opens and closes such a device a few times through
// the direct path with only non-sounding messages, and measures how many messages per second the standard WinMM path
// and the direct path accept. It changes no system setting, needs no administrator rights, installs nothing and
// writes one text file.
//
// SOUND SAFETY: the only messages ever sent are Control Change 20..31 (controllers the MIDI standard leaves
// undefined), Note On with velocity 0 (a Note Off), the "all sound off / all notes off / sustain off" controllers sent
// by a panic, and System Exclusive messages of the non-commercial manufacturer id 7D that no synthesizer reacts to.
// Windows' own driver (Microsoft GS Wavetable Synth, hardware and USB ports) and the MIDI Mapper are never opened.
//
//   midibn-direct-check [--out FILE] [--seconds S] [--cycles N] [--device TEXT] [--list] [--no-throughput]
//     --out FILE        result file (default: direct-check-result.txt beside the program)
//     --seconds S       length of each throughput measurement (default 1.0)
//     --cycles N        open / send / close repetitions per device (default 5)
//     --device TEXT     only devices whose name contains TEXT (case-insensitive)
//     --list            only the lists (no device is opened)
//     --no-throughput   skip the speed measurement
//     --fake-driver     NO DEVICE: measure the cost of one direct send through the real WinMmDirectOutput code path into an
//                       in-process no-op driver entry point (a few stores into a ring buffer, like a synthesizer queue), next
//                       to a bare call of the same function; 3 rounds, up to 7 when they are not close (--seconds S per round).
//                       No driver is loaded and no MIDI device is opened, so it is safe on any PC, including Windows XP.
#include "midibn/midi_output.h"
#include "midi_driver_discovery.h"
#include "tool_support.h"
#include "win32_platform.h"
#include "winmm_direct_output.h"
#include "winmm_midi_output.h"

#include <mmsystem.h>
#include <winsvc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;

namespace {

const char* kToolVersion = "midibn-direct-check 1 (DD-085)";

class Report {
public:
    explicit Report(const std::string& path) : path_(path) { file_ = nullptr; if (!path.empty() && _wfopen_s(&file_, win32::utf8_to_wide(path).c_str(), L"wb") != 0) file_ = nullptr; }
    ~Report() { if (file_) std::fclose(file_); }
    void line(const std::string& text = std::string())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tools::write_stdout(text + "\n");
        std::string out = text + "\r\n";
        if (file_) { std::fwrite(out.data(), 1, out.size(), file_); std::fflush(file_); }
    }
    bool file_ok() const { return path_.empty() || file_ != nullptr; }
private:
    std::string path_;
    FILE* file_ = nullptr;
    std::mutex mutex_;
};

std::string fixed(double v, int digits)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", digits, v);
    return b;
}

std::string lower_ascii(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
    return s;
}

int64_t qpc()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
double qpc_seconds(int64_t ticks)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(ticks) / static_cast<double>(f.QuadPart);
}

// A hung driver call (system WinMM can hang in close after a flood) must not leave the program stuck: after the limit
// the report says so and the process ends. Everything measured before is already in the file.
class Watchdog {
public:
    Watchdog(Report& report, const std::string& what, int seconds) : done_(std::make_shared<std::atomic<bool> >(false))
    {
        std::shared_ptr<std::atomic<bool> > done = done_;
        Report* r = &report;
        std::string label = what;
        std::thread([done, r, label, seconds] {
            for (int i = 0; i < seconds * 10 && !done->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (done->load()) return;
            r->line("WATCHDOG: " + label + " did not return within " + std::to_string(seconds) + " seconds; the program ends here.");
            r->line("RESULT hung=" + label);
            TerminateProcess(GetCurrentProcess(), 3);
        }).detach();
    }
    ~Watchdog() { done_->store(true); }
private:
    std::shared_ptr<std::atomic<bool> > done_;
};

double cpu_busy_percent(int milliseconds)
{
    FILETIME i0, k0, u0, i1, k1, u1;
    GetSystemTimes(&i0, &k0, &u0);
    Sleep(milliseconds);
    GetSystemTimes(&i1, &k1, &u1);
    auto v = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    double total = static_cast<double>((v(k1) - v(k0)) + (v(u1) - v(u0)));
    double idle = static_cast<double>(v(i1) - v(i0));
    return total > 0 ? 100.0 * (total - idle) / total : 0.0;
}

std::string registry_text(const wchar_t* subkey, const wchar_t* name, REGSAM view = 0)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return std::string();
    wchar_t buffer[512];
    DWORD size = sizeof buffer - 2, type = 0;
    std::string result;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS) {
        if (type == REG_SZ) { buffer[size / 2] = 0; result = win32::wide_to_utf8(buffer); }
        else if (type == REG_DWORD) result = std::to_string(*reinterpret_cast<DWORD*>(buffer));
    }
    RegCloseKey(key);
    return result;
}

// ---------------------------------------------------------------- sections

void environment(Report& r)
{
    r.line(std::string("== ") + kToolVersion + " ==");
    r.line(std::string("program: ") + (sizeof(void*) == 8 ? "64-bit" : "32-bit") + (win32::running_under_wow64() ? " on 64-bit Windows (WOW64)" : ""));
    r.line("windows: " + win32::describe_os());
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    r.line("product name: " + registry_text(nt, L"ProductName"));
    r.line("display version: " + registry_text(nt, L"DisplayVersion") + "   current build: " + registry_text(nt, L"CurrentBuildNumber") +
        "." + registry_text(nt, L"UBR"));
    r.line("processor: " + registry_text(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    r.line("logical processors: " + std::to_string(info.dwNumberOfProcessors));
    r.line("cpu busy before the check: " + fixed(cpu_busy_percent(500), 1) + " %");
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    r.line(std::string("time: ") + stamp);
    r.line();
}

void midi_services(Report& r)
{
    r.line("== Windows MIDI Services (read only) ==");
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) r.line("service manager: could not be opened (error " + std::to_string(GetLastError()) + ")");
    else {
        const wchar_t* names[] = { L"MidiSrv", L"midisrv" };
        SC_HANDLE service = OpenServiceW(manager, names[0], SERVICE_QUERY_STATUS);
        if (!service) {
            DWORD e = GetLastError();
            r.line(e == ERROR_SERVICE_DOES_NOT_EXIST ? "service MidiSrv: not installed" : "service MidiSrv: cannot be queried (error " + std::to_string(e) + ")");
        }
        else {
            SERVICE_STATUS status;
            if (QueryServiceStatus(service, &status)) {
                std::string state = status.dwCurrentState == SERVICE_RUNNING ? "running" : status.dwCurrentState == SERVICE_STOPPED ? "stopped" :
                    status.dwCurrentState == SERVICE_START_PENDING ? "starting" : "state " + std::to_string(status.dwCurrentState);
                r.line("service MidiSrv: installed, " + state);
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
    }
    // Files of the new MIDI stack in the system folder (names that start with "midi"), as an extra hint.
    win32::SystemFolders folders = win32::system_folders();
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((folders.directory + L"\\midi*").c_str(), &data);
    std::string names;
    if (find != INVALID_HANDLE_VALUE) {
        do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names += (names.empty() ? "" : ", ") + win32::wide_to_utf8(data.cFileName); } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    r.line("files named midi* in " + win32::wide_to_utf8(folders.directory) + ": " + (names.empty() ? "none" : names));
    r.line();
}

void drivers(Report& r, bool letWinmmStartFirst)
{
    r.line(letWinmmStartFirst ? "== Windows MIDI driver list (Drivers32), drivers started by Windows first =="
                              : "== Windows MIDI driver list (Drivers32), drivers started by this program (second start-up path) ==");
    std::vector<win32::DriverReportLine> lines = win32::describe_driver_entries(letWinmmStartFirst);
    for (size_t i = 0; i < lines.size(); ++i) {
        const win32::DriverReportLine& l = lines[i];
        r.line(l.name + " = " + l.value + "   [" + l.kind + "]");
        if (!l.path.empty()) r.line("    file: " + l.path);
        if (!l.state.empty()) r.line("    " + l.state);
    }
    r.line();
}

struct Device {
    uint32_t id;
    std::string name;
    win32::DirectResolution resolution;
};

std::vector<Device> list_devices(Report& r, const std::string& filter)
{
    std::vector<Device> devices;
    r.line("== WinMM output devices and direct access ==");
    std::vector<platform::OutputDeviceInfo> list = win32::WinMmMidiOutput::enumerate_devices();
    for (size_t i = 0; i < list.size(); ++i) {
        Device d;
        d.id = list[i].device_id;
        d.name = list[i].name;
        d.resolution = win32::resolve_direct_device(d.id);
        r.line("device " + std::to_string(d.id) + ": " + d.name + "   (manufacturer " + std::to_string(list[i].manufacturer_id) +
            ", product " + std::to_string(list[i].product_id) + ", driver version 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%X", list[i].driver_version); return std::string(b); }() + ")");
        if (d.resolution.ok)
            r.line("    direct access: possible through " + d.resolution.driver_file + " (" + (d.resolution.verified ? "tested driver" : "unverified driver") +
                "), the driver's own device number " + std::to_string(d.resolution.local_index) + "; " + d.resolution.lifecycle);
        else
            r.line("    direct access: not available - " + d.resolution.reason + "  (the standard path would be used)");
        if (!filter.empty() && lower_ascii(d.name).find(lower_ascii(filter)) == std::string::npos) continue;
        devices.push_back(d);
    }
    r.line();
    return devices;
}

struct Counts {
    bool ok = true;
    std::string first_error;
};

void note(Counts& c, const platform::OutputStatus& s, const std::string& what)
{
    if (!s.ok() && c.ok) { c.ok = false; c.first_error = what + ": " + s.message; }
}

// One open / send / close round; returns the milliseconds the open took.
bool direct_round(Report& r, win32::WinMmDirectOutput& out, uint32_t id, bool verbose, Counts& counts, double& open_ms)
{
    int64_t t0 = qpc();
    platform::OutputStatus s = out.open(id);
    open_ms = qpc_seconds(qpc() - t0) * 1000.0;
    if (!s.ok()) { note(counts, s, "open"); return false; }
    for (int k = 0; k < 12; ++k) note(counts, out.send_short(0x0000B0u | (static_cast<uint32_t>(20 + k) << 8) | (static_cast<uint32_t>(k) << 16)), "CC send");
    note(counts, out.send_short(0x003C90), "Note On velocity 0");
    // System Exclusive of the non-commercial id 7D: 3 bytes (shortest useful), 7 bytes, 600 bytes; every one should
    // complete (the driver reports MOM_DONE through the callback of this program).
    uint32_t before = out.long_completions();
    std::vector<std::vector<uint8_t> > messages;
    messages.push_back({ 0xF0, 0x7D, 0xF7 });
    messages.push_back({ 0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0xF7 });
    std::vector<uint8_t> big(600, 0x15);
    big[0] = 0xF0; big[1] = 0x7D; big[599] = 0xF7;
    messages.push_back(big);
    for (size_t m = 0; m < messages.size(); ++m) {
        output::SystemExclusiveEvent e;
        e.status = 0xF0;
        e.data = messages[m].data();
        e.length = messages[m].size();
        note(counts, out.send_long(e), "SysEx of " + std::to_string(messages[m].size()) + " bytes");
    }
    for (int w = 0; w < 200 && out.long_completions() - before < messages.size(); ++w) Sleep(5);
    uint32_t done = out.long_completions() - before;
    if (done != messages.size() && counts.ok) { counts.ok = false; counts.first_error = "SysEx: " + std::to_string(done) + " of " + std::to_string(messages.size()) + " completions reported"; }
    note(counts, out.send_short(0x000078B0), "CC120 (all sound off)");
    note(counts, out.panic(), "panic");
    note(counts, out.reset(), "reset");
    note(counts, out.close(), "close");
    if (verbose) r.line("    round ok=" + std::string(counts.ok ? "yes" : "no") + "  open " + fixed(open_ms, 2) + " ms  SysEx completions " + std::to_string(done) + "/3  outstanding after close " + std::to_string(out.outstanding_long_messages()));
    return counts.ok;
}

struct Throughput {
    long long events = 0;
    double seconds = 0;
    long long errors = 0;
    std::string first_error;
    double eps() const { return seconds > 0 ? events / seconds : 0; }
};

Throughput measure(platform::MidiOutput& out, double seconds)
{
    Throughput t;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const int64_t limit = static_cast<int64_t>(seconds * static_cast<double>(frequency.QuadPart));
    uint32_t n = 0;
    int64_t start = qpc();
    for (;;) {
        platform::SendBatch batch(&out);   // 256 sends per batch, as the engine sends a burst of due events (DD-110)
        for (int i = 0; i < 256; ++i, ++n) {
            // Control Change 20..31 on all 16 channels: undefined controllers, silent.
            uint32_t packed = 0xB0u | (n & 15u) | (static_cast<uint32_t>(20 + (n >> 4) % 12) << 8) | (static_cast<uint32_t>((n * 7) & 127) << 16);
            platform::OutputStatus s = out.send_short(packed);
            if (!s.ok()) { if (t.errors == 0) t.first_error = s.message; ++t.errors; }
        }
        t.events += 256;
        int64_t elapsed = qpc() - start;
        if (elapsed >= limit) { t.seconds = qpc_seconds(elapsed); break; }
    }
    return t;
}

void run_tests(Report& r, const std::vector<Device>& devices, double seconds, int cycles, bool throughput)
{
    r.line("== Direct open / send / close, and speed ==");
    r.line("(only silent messages: controllers 20-31, note-on velocity 0, panic controllers, SysEx id 7D)");
    int tested = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        const Device& d = devices[i];
        if (!d.resolution.ok) continue;
        ++tested;
        r.line("device " + std::to_string(d.id) + ": " + d.name + " through " + d.resolution.driver_file);
        win32::WinMmDirectOutput direct;
        int passed = 0;
        Counts counts;
        double firstOpen = 0, anyOpen = 0;
        for (int c = 0; c < cycles; ++c) {
            Counts one;
            Watchdog roundDog(r, "a direct open/send/close round", 45);   // a driver call that never returns ends the program, with what is known so far
            if (direct_round(r, direct, d.id, c == 0, one, anyOpen)) ++passed;
            else if (counts.ok) { counts = one; }
            if (c == 0) firstOpen = anyOpen;
        }
        r.line("    direct rounds passed: " + std::to_string(passed) + " of " + std::to_string(cycles) + (counts.ok ? "" : "   first problem: " + counts.first_error));
        r.line("    first open took " + fixed(firstOpen, 2) + " ms");
        std::string result = "RESULT device=\"" + d.name + "\" driver=" + d.resolution.driver_file + " rounds=" + std::to_string(passed) + "/" + std::to_string(cycles);
        if (throughput && passed > 0) {
            Throughput std_t, dir_t;
            uint64_t directWaits = 0;
            double directWaitMs = 0;
            {
                win32::WinMmMidiOutput standard;
                Watchdog openDog(r, "standard WinMM open and speed measurement", static_cast<int>(seconds) + 45);
                platform::OutputStatus s = standard.open(d.id);
                if (!s.ok()) r.line("    standard WinMM: could not open - " + s.message);
                else {
                    std_t = measure(standard, seconds);
                    Watchdog dog(r, "standard WinMM close", 20);
                    standard.close();
                }
            }
            {
                Watchdog openDog(r, "direct open and speed measurement", static_cast<int>(seconds) + 45);
                platform::OutputStatus s = direct.open(d.id);
                if (!s.ok()) r.line("    direct: could not open for speed - " + s.message);
                else {
                    uint64_t waits0 = direct.busy_waits();
                    double waitMs0 = direct.busy_wait_milliseconds();
                    dir_t = measure(direct, seconds);
                    directWaits = direct.busy_waits() - waits0;
                    directWaitMs = direct.busy_wait_milliseconds() - waitMs0;
                    Watchdog dog(r, "direct close", 20);
                    direct.close();
                }
            }
            r.line("    standard WinMM: " + fixed(std_t.eps() / 1e6, 3) + " million messages/s (" + fixed(std_t.eps() > 0 ? 1e9 / std_t.eps() : 0, 0) +
                " ns each, " + std::to_string(std_t.errors) + " errors)");
            r.line("    direct:         " + fixed(dir_t.eps() / 1e6, 3) + " million messages/s (" + fixed(dir_t.eps() > 0 ? 1e9 / dir_t.eps() : 0, 0) +
                " ns each, " + std::to_string(dir_t.errors) + " errors)");
            if (directWaits > 0) r.line("    the driver answered \"busy\" (its queue was full) " + std::to_string(directWaits) + " times; waiting for it took " + fixed(directWaitMs, 1) + " ms in all");
            if (std_t.eps() > 0) r.line("    direct is " + fixed(dir_t.eps() / std_t.eps(), 1) + " times as fast as standard WinMM here");
            if (!std_t.first_error.empty()) r.line("    standard first error: " + std_t.first_error);
            if (!dir_t.first_error.empty()) r.line("    direct first error: " + dir_t.first_error);
            result += " standard_mps=" + fixed(std_t.eps(), 0) + " direct_mps=" + fixed(dir_t.eps(), 0) + " standard_errors=" + std::to_string(std_t.errors) +
                " direct_errors=" + std::to_string(dir_t.errors) + " direct_busy_waits=" + std::to_string(directWaits);
        }
        r.line(result);
    }
    if (tested == 0) r.line("no device can be reached directly on this PC (see the list above for the reason); nothing was opened.");
    r.line();
}

// ---------------------------------------------------------------- --fake-driver (no device)

uint32_t g_fake_ring[4096];
uint32_t g_fake_position = 0;

uint32_t __stdcall fake_mod_message(uint32_t, uint32_t message, uintptr_t instance, uintptr_t param1, uintptr_t)
{
    if (message == win32::drv::kModData) {   // what a synthesizer's event queue does: store the message, advance
        g_fake_ring[g_fake_position++ & 4095u] = static_cast<uint32_t>(param1);
        return 0;
    }
    if (message == win32::drv::kModOpen) *reinterpret_cast<uintptr_t*>(instance) = 0x1234;
    return 0;
}

win32::DirectResolution fake_resolution(uint32_t)
{
    win32::DirectResolution r;
    r.ok = true;
    r.mod_message = &fake_mod_message;
    r.local_index = 0;
    r.device_name = "in-process no-op driver";
    r.driver_file = "FAKE-NOOP.DLL";
    r.verified = true;
    r.lifecycle = "none (no driver is loaded)";
    return r;
}

// batch: the 256 sends of one pass are bracketed by begin/end_send_batch, as the engine brackets a burst of due events.
double fake_sends_ns(platform::MidiOutput* out, win32::ModMessageFn raw, double seconds, bool batch, bool lean = false)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const int64_t limit = static_cast<int64_t>(seconds * static_cast<double>(frequency.QuadPart));
    long long n = 0;
    platform::OutputStatus failure;   // the engine keeps one too: a send that works builds no status
    uint32_t value = 0;
    const int64_t start = qpc();
    int64_t elapsed = 0;
    do {
        if (out && batch) out->begin_send_batch();
        for (int i = 0; i < 256; ++i, ++value) {
            const uint32_t packed = 0xB0u | (value & 15u) | (static_cast<uint32_t>(20 + (value >> 4) % 12) << 8) | (static_cast<uint32_t>((value * 7) & 127) << 16);
            if (out) { if (lean) out->try_send_short(packed, failure); else out->send_short(packed); }
            else raw(0, win32::drv::kModData, 0x1234, packed, 0);
        }
        if (out && batch) out->end_send_batch();
        n += 256;
        elapsed = qpc() - start;
    } while (elapsed < limit);
    return qpc_seconds(elapsed) * 1e9 / static_cast<double>(n);
}

// 3 rounds; up to 7 when the first 3 differ by more than 5 % (the median is reported, with the range).
void adaptive(Report& r, const std::string& label, platform::MidiOutput* out, double seconds, bool batch, std::vector<double>& rounds, bool lean = false)
{
    win32::ModMessageFn volatile raw = &fake_mod_message;
    rounds.clear();
    for (int round = 0; round < 7; ++round) {
        rounds.push_back(fake_sends_ns(out, raw, seconds, batch, lean));
        if (round == 2) {
            std::vector<double> v = rounds;
            std::sort(v.begin(), v.end());
            if ((v[2] - v[0]) / v[1] <= 0.05) break;
        }
    }
    std::string all;
    for (size_t i = 0; i < rounds.size(); ++i) all += (i ? ", " : "") + fixed(rounds[i], 1);
    std::sort(rounds.begin(), rounds.end());
    r.line("    " + label + ": median " + fixed(rounds[rounds.size() / 2], 1) + " ns per send (min " + fixed(rounds.front(), 1) + ", max " +
        fixed(rounds.back(), 1) + "; " + std::to_string(rounds.size()) + " rounds in order: " + all + ")");
}

int fake_driver_check(Report& r, double seconds)
{
    r.line("== Direct send cost into an in-process no-op driver (no device, no driver loaded) ==");
    win32::WinMmDirectOutput direct(&fake_resolution);
    platform::OutputStatus s = direct.open(0);
    if (!s.ok()) { r.line("open failed: " + s.message); r.line("RESULT fake_driver=failed"); return 1; }
    std::vector<double> bare, viaOutput, viaBatch, leanOutput, leanBatch;
    adaptive(r, "bare call of the no-op driver function", nullptr, seconds, false, bare);
    adaptive(r, "WinMmDirectOutput::send_short, one send at a time", &direct, seconds, false, viaOutput);
    adaptive(r, "WinMmDirectOutput::send_short, 256 sends per batch (an engine burst)", &direct, seconds, true, viaBatch);
    adaptive(r, "WinMmDirectOutput::try_send_short (no status on success), one send at a time", &direct, seconds, false, leanOutput, true);
    adaptive(r, "WinMmDirectOutput::try_send_short (no status on success), 256 sends per batch (what the engine now does)", &direct, seconds, true, leanBatch, true);
    direct.close();
    const double b = bare[bare.size() / 2], d = viaOutput[viaOutput.size() / 2], e = viaBatch[viaBatch.size() / 2];
    r.line("    cost of the output wrapper (call gate and the rest) per send: " + fixed(d - b, 1) + " ns one at a time, " + fixed(e - b, 1) + " ns in batches");
    r.line("RESULT fake_driver bare_ns=" + fixed(b, 1) + " direct_ns=" + fixed(d, 1) + " wrapper_ns=" + fixed(d - b, 1) +
        " batch_ns=" + fixed(e, 1) + " batch_wrapper_ns=" + fixed(e - b, 1) +
        " lean_ns=" + fixed(leanOutput[leanOutput.size() / 2], 1) + " lean_batch_ns=" + fixed(leanBatch[leanBatch.size() / 2], 1) +
        " rounds=" + std::to_string(viaOutput.size()) + "/" + std::to_string(viaBatch.size()) + " sink=" + std::to_string(g_fake_ring[0] & 1));
    r.line();
    return 0;
}

} // namespace

static int check_main(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    std::string out = tools::executable_directory() + "\\direct-check-result.txt";
    std::string filter;
    double seconds = 1.0;
    int cycles = 5;
    bool listOnly = false, throughput = true, fakeDriver = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "--seconds" && i + 1 < args.size()) seconds = std::atof(args[++i].c_str());
        else if (args[i] == "--cycles" && i + 1 < args.size()) cycles = std::atoi(args[++i].c_str());
        else if (args[i] == "--device" && i + 1 < args.size()) filter = args[++i];
        else if (args[i] == "--list") listOnly = true;
        else if (args[i] == "--no-throughput") throughput = false;
        else if (args[i] == "--fake-driver") fakeDriver = true;
        else { std::fprintf(stderr, "unknown option %s\n(see the comment at the top of tools\\direct_check_main.cpp)\n", args[i].c_str()); return 2; }
    }
    if (fakeDriver && seconds == 1.0) seconds = 0.5;
    if (seconds < 0.05) seconds = 0.05;
    if (cycles < 1) cycles = 1;
    Report report(out);
    if (!report.file_ok()) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 3; }
    // Only one copy at a time: two programs talking to the same synthesizer driver at once is what made drivers (OmniMIDI)
    // crash or stall in tests, so a second copy waits for the first (up to two minutes) instead of colliding with it.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Global\\MIDIBottleneck-direct-check");
    if (single && WaitForSingleObject(single, 120000) == WAIT_TIMEOUT) {
        report.line("Another copy of this check is still running on this PC; this one stopped without touching any MIDI driver. Run it again when the first one has finished.");
        report.line("DIRECT-CHECK: NOT RUN");
        return 4;
    }
    environment(report);
    if (fakeDriver) {   // nothing below touches a driver or a device
        const int code = fake_driver_check(report, seconds);
        report.line("result file: " + out);
        report.line("DIRECT-CHECK: DONE");
        return code;
    }
    midi_services(report);
    drivers(report, false);   // before any midiOut call: this program starts (and stops) the drivers itself
    drivers(report, true);    // then the normal case: Windows has started them
    std::vector<Device> devices = list_devices(report, filter);
    if (!listOnly) run_tests(report, devices, seconds, cycles, throughput);
    report.line("result file: " + out);
    report.line("DIRECT-CHECK: DONE");
    return 0;
}

// The result file is complete when check_main returns (its Report is closed). The process then ends at once without
// running the exit code of the loaded drivers: a driver whose worker thread is busy with another program must not be
// able to keep the window open after the answer is already written.
int wmain(int argc, wchar_t** argv)
{
    int code = check_main(argc, argv);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
