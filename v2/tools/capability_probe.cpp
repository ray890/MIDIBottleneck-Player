// Capability probe implementation. See capability_probe.h.
//
// Processor requirement of THIS binary (reported at run time as well):
//   x64 build            : SSE2 is the x64 baseline.
//   x86 builds (incl. XP): v141/v143 generate SSE2 code by default (/arch:SSE2
//                          has been the default since Visual Studio 2012) and
//                          CMakeLists.txt adds no /arch option, so the x86
//                          binaries need a CPU with SSE2 (Pentium 4 / Athlon 64
//                          or newer). std::atomic<int64_t> also needs CMPXCHG8B.
// Everything newer than Windows XP SP3 is resolved with GetProcAddress.
#include "capability_probe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <mmsystem.h>
#include <intrin.h>
#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace midibn {
namespace tools {
namespace {

typedef std::pair<std::string, std::string> Field;
typedef std::vector<Field> Fields;

// ---------------------------------------------------------------- helpers

std::string ascii_clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e) c = '?';
    }
    return out;
}

std::string wide_ascii(const wchar_t* w)
{
    std::string out;
    for (; *w; ++w) out.push_back((*w >= 0x20 && *w <= 0x7e) ? static_cast<char>(*w) : '?');
    return out;
}

std::string wide_ascii(const std::wstring& w) { return wide_ascii(w.c_str()); }

std::string num(unsigned long long v) { return std::to_string(v); }
std::string snum(long long v) { return std::to_string(v); }

std::string fnum(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string hex(unsigned long long v, int width)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%0*llX", width, v);
    return buf;
}

std::string yesno(bool b) { return b ? "yes" : "no"; }

std::string win_error(const char* what, DWORD code = GetLastError())
{
    return std::string(what) + " failed (Win32 error " + std::to_string(static_cast<unsigned long>(code)) + ")";
}

double qpc_frequency()
{
    static double freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        f.QuadPart = 0;
        QueryPerformanceFrequency(&f);
        freq = static_cast<double>(f.QuadPart);
    }
    return freq;
}

double qpc_us()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e6 / qpc_frequency();
}

struct Stats {
    double min = 0, median = 0, p99 = 0, max = 0;
    size_t n = 0;
};

Stats compute_stats(std::vector<double> v)
{
    Stats s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v[v.size() / 2];
    size_t idx = static_cast<size_t>(0.99 * static_cast<double>(v.size()));
    if (idx >= v.size()) idx = v.size() - 1;
    s.p99 = v[idx];
    return s;
}

Fields stat_fields_us(const std::vector<double>& samples)
{
    Stats s = compute_stats(samples);
    return { { "median_us", fnum(s.median, 0) }, { "p99_us", fnum(s.p99, 0) }, { "max_us", fnum(s.max, 0) },
        { "min_us", fnum(s.min, 0) }, { "samples", num(s.n) } };
}

class Report {
public:
    Report(std::vector<std::string>& h, std::vector<std::string>& m) : human(h), machine(m) {}

    void heading(const std::string& title)
    {
        human.push_back("");
        human.push_back("-- " + title + " --");
    }

    void note(const std::string& text) { human.push_back("[capability] note: " + ascii_clean(text)); }

    void rec(const std::string& key, const Fields& fields)
    {
        std::string line = "[capability] " + key;
        for (const Field& f : fields) {
            std::string v = ascii_clean(f.second);
            bool quote = v.empty() || v.find(' ') != std::string::npos;
            line += " " + f.first + "=" + (quote ? "\"" + v + "\"" : v);
            machine.push_back(key + "." + f.first + "=" + v);
        }
        human.push_back(line);
    }

    void error(const std::string& key, const std::string& text) { rec(key + ".error", { { "error", text } }); }

    void append(const std::vector<std::string>& h, const std::vector<std::string>& m)
    {
        human.insert(human.end(), h.begin(), h.end());
        machine.insert(machine.end(), m.begin(), m.end());
    }

private:
    std::vector<std::string>& human;
    std::vector<std::string>& machine;
};

// Runs one probe; a failure is recorded as error=<text> and never propagates.
template <class F>
void guard(Report& r, const char* key, F&& f)
{
    try {
        f();
    }
    catch (const std::exception& e) {
        r.error(key, e.what());
    }
    catch (...) {
        r.error(key, "unknown exception");
    }
}

template <class F>
void section(Report& r, const char* title, const char* key, F&& f)
{
    r.heading(title);
    double t0 = qpc_us();
    guard(r, key, f);
    r.rec(std::string(key) + ".time", { { "elapsed_ms", fnum((qpc_us() - t0) / 1000.0, 0) } });
}

struct HandleCloser {
    HANDLE h;
    explicit HandleCloser(HANDLE handle) : h(handle) {}
    ~HandleCloser()
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
    }
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
};

// timeBeginPeriod immediately paired with timeEndPeriod when this goes out of scope.
struct TimePeriod {
    UINT period;
    bool ok;
    explicit TimePeriod(UINT p) : period(p), ok(timeBeginPeriod(p) == TIMERR_NOERROR) {}
    ~TimePeriod()
    {
        if (ok) timeEndPeriod(period);
    }
    TimePeriod(const TimePeriod&) = delete;
    TimePeriod& operator=(const TimePeriod&) = delete;
};

HMODULE kernel32() { return GetModuleHandleW(L"kernel32.dll"); }

std::wstring system_directory()
{
    wchar_t buf[MAX_PATH + 8] = {};
    UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return n ? std::wstring(buf, n) : std::wstring();
}

std::wstring windows_directory()
{
    wchar_t buf[MAX_PATH + 8] = {};
    UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
    return n ? std::wstring(buf, n) : std::wstring();
}

std::wstring full_path(const std::wstring& p)
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetFullPathNameW(p.empty() ? L"." : p.c_str(), MAX_PATH * 2, buf, nullptr);
    std::wstring out = (n && n < MAX_PATH * 2) ? std::wstring(buf, n) : p;
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

bool is_wow64()
{
    typedef BOOL(WINAPI * Fn)(HANDLE, PBOOL);
    Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(kernel32(), "IsWow64Process")));
    BOOL wow = FALSE;
    if (fn && fn(GetCurrentProcess(), &wow)) return wow != FALSE;
    return false;
}

// ---------------------------------------------------------------- 1. processor

void probe_processor(Report& r)
{
    int regs[4] = { 0, 0, 0, 0 };
    __cpuid(regs, 0);
    const int maxLeaf = regs[0];
    char vendor[13];
    std::memcpy(vendor + 0, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    vendor[12] = 0;

    std::string brand;
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        char b[49] = {};
        for (int i = 0; i < 3; ++i) {
            __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
            std::memcpy(b + 16 * i, regs, 16);
        }
        brand = b;
        size_t a = brand.find_first_not_of(' ');
        size_t z = brand.find_last_not_of(' ');
        brand = a == std::string::npos ? std::string() : brand.substr(a, z - a + 1);
    }
    r.rec("processor.cpu", { { "vendor", vendor }, { "brand", brand }, { "max_cpuid_leaf", num(static_cast<unsigned>(maxLeaf)) } });

    bool mmx = false, cmov = false, cx8 = false, sse = false, sse2 = false, sse3 = false, ssse3 = false, sse41 = false,
         sse42 = false, popcnt = false, avx = false, avx2 = false;
    if (maxLeaf >= 1) {
        __cpuid(regs, 1);
        const unsigned edx = static_cast<unsigned>(regs[3]);
        const unsigned ecx = static_cast<unsigned>(regs[2]);
        cx8 = (edx >> 8) & 1;
        cmov = (edx >> 15) & 1;
        mmx = (edx >> 23) & 1;
        sse = (edx >> 25) & 1;
        sse2 = (edx >> 26) & 1;
        sse3 = (ecx >> 0) & 1;
        ssse3 = (ecx >> 9) & 1;
        sse41 = (ecx >> 19) & 1;
        sse42 = (ecx >> 20) & 1;
        popcnt = (ecx >> 23) & 1;
        const bool osxsave = (ecx >> 27) & 1;
        const bool avxBit = (ecx >> 28) & 1;
        bool avxUsable = false;
        if (osxsave && avxBit) {
            unsigned long long xcr0 = _xgetbv(0);
            avxUsable = (xcr0 & 6) == 6; // OS saves XMM and YMM state
        }
        avx = avxUsable;
        if (avxUsable && maxLeaf >= 7) {
            __cpuidex(regs, 7, 0);
            avx2 = ((static_cast<unsigned>(regs[1]) >> 5) & 1) != 0;
        }
    }
    r.rec("processor.flags", { { "mmx", yesno(mmx) }, { "cmov", yesno(cmov) }, { "cmpxchg8b", yesno(cx8) },
        { "sse", yesno(sse) }, { "sse2", yesno(sse2) }, { "sse3", yesno(sse3) }, { "ssse3", yesno(ssse3) },
        { "sse41", yesno(sse41) }, { "sse42", yesno(sse42) }, { "popcnt", yesno(popcnt) },
        { "avx_usable_by_os", yesno(avx) }, { "avx2", yesno(avx2) } });

    r.rec("processor.os_features", { { "PF_XMMI_INSTRUCTIONS_AVAILABLE_sse", yesno(IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE) != FALSE) },
        { "PF_XMMI64_INSTRUCTIONS_AVAILABLE_sse2", yesno(IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) != FALSE) } });

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    std::string arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
        : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86"
        : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64 ? "ia64"
        : "other(" + num(si.wProcessorArchitecture) + ")";
    r.rec("processor.system", { { "native_architecture", arch }, { "logical_processors", num(si.dwNumberOfProcessors) },
        { "processor_level", num(si.wProcessorLevel) }, { "processor_revision", hex(si.wProcessorRevision, 4) },
        { "process_bits", num(sizeof(void*) * 8) }, { "running_under_wow64", yesno(is_wow64()) } });

#if defined(_M_X64)
    const char* need = "SSE2 (x64 baseline)";
    const bool meets = sse2 && cx8;
#elif defined(_M_IX86_FP) && _M_IX86_FP >= 2
    const char* need = "SSE2 (default x86 code generation of the Microsoft compiler; CMakeLists.txt adds no /arch option) and CMPXCHG8B";
    const bool meets = sse2 && cx8;
#else
    const char* need = "CMPXCHG8B only (no SSE code generation)";
    const bool meets = cx8;
#endif
    r.rec("processor.binary_requirement", { { "this_binary_requires", need }, { "this_cpu_meets_it", yesno(meets) },
        { "built_for_winnt", hex(_WIN32_WINNT, 4) } });
}

// ---------------------------------------------------------------- 2. timing

std::vector<double> sample_sleep1(int n)
{
    std::vector<double> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double t0 = qpc_us();
        Sleep(1);
        v.push_back(qpc_us() - t0);
    }
    return v;
}

std::vector<double> sample_timer(HANDLE timer, int n, int& timeouts)
{
    std::vector<double> v;
    v.reserve(static_cast<size_t>(n));
    timeouts = 0;
    for (int i = 0; i < n; ++i) {
        LARGE_INTEGER due;
        due.QuadPart = -10000; // 1 ms, relative, in 100 ns units
        double t0 = qpc_us();
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) throw std::runtime_error(win_error("SetWaitableTimer"));
        if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0) ++timeouts;
        v.push_back(qpc_us() - t0);
    }
    return v;
}

void probe_timing(Report& r)
{
    guard(r, "timing.qpc_frequency", [&] {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f)) throw std::runtime_error("QueryPerformanceFrequency not supported");
        r.rec("timing.qpc_frequency", { { "hz", snum(f.QuadPart) } });
    });

    guard(r, "timing.qpc_stability", [&] {
        const size_t N = 200000;
        std::vector<long long> d;
        d.reserve(N);
        LARGE_INTEGER prev, cur;
        QueryPerformanceCounter(&prev);
        for (size_t i = 0; i < N; ++i) {
            QueryPerformanceCounter(&cur);
            d.push_back(cur.QuadPart - prev.QuadPart);
            prev = cur;
        }
        size_t backward = 0, zero = 0;
        for (long long x : d) {
            if (x < 0) ++backward;
            else if (x == 0) ++zero;
        }
        std::sort(d.begin(), d.end());
        r.rec("timing.qpc_stability", { { "reads", num(N) }, { "backward_steps", num(backward) }, { "zero_deltas", num(zero) },
            { "min_ticks", snum(d.front()) }, { "median_ticks", snum(d[N / 2]) }, { "p99_ticks", snum(d[static_cast<size_t>(0.99 * static_cast<double>(N))]) },
            { "max_ticks", snum(d.back()) } });
    });

    guard(r, "timing.qpc_vs_tickcount", [&] {
        // Both samples are taken just after a GetTickCount change so the tick
        // resolution (about 16 ms) does not dominate the comparison.
        auto wait_edge = [] {
            DWORD t = GetTickCount();
            double deadline = qpc_us() + 100000.0;
            while (GetTickCount() == t && qpc_us() < deadline) {}
        };
        wait_edge();
        double q0 = qpc_us();
        DWORD k0 = GetTickCount();
        Sleep(250);
        wait_edge();
        double q1 = qpc_us();
        DWORD k1 = GetTickCount();
        double tickMs = static_cast<double>(k1 - k0);
        double qpcMs = (q1 - q0) / 1000.0;
        if (tickMs <= 0) throw std::runtime_error("GetTickCount did not advance");
        r.rec("timing.qpc_vs_tickcount", { { "qpc_ms", fnum(qpcMs, 3) }, { "tick_ms", fnum(tickMs, 0) },
            { "drift_ppm", fnum((qpcMs - tickMs) / tickMs * 1e6, 0) }, { "note", "approximate; limited by GetTickCount edge accuracy" } });
    });

    guard(r, "timing.timegetdevcaps", [&] {
        TIMECAPS tc;
        MMRESULT mr = timeGetDevCaps(&tc, sizeof(tc));
        if (mr != TIMERR_NOERROR) throw std::runtime_error("timeGetDevCaps returned " + num(mr));
        r.rec("timing.timegetdevcaps", { { "min_period_ms", num(tc.wPeriodMin) }, { "max_period_ms", num(tc.wPeriodMax) } });
    });

    // Current system timer resolution (read-only query; ntdll is a system DLL).
    typedef LONG(NTAPI * NtQueryTimerResolutionFn)(PULONG, PULONG, PULONG);
    NtQueryTimerResolutionFn ntq = nullptr;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) ntq = reinterpret_cast<NtQueryTimerResolutionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryTimerResolution")));
    auto report_resolution = [&](const char* key) {
        if (!ntq) {
            r.rec(key, { { "status", "not available" } });
            return;
        }
        ULONG minimum = 0, maximum = 0, current = 0;
        if (ntq(&minimum, &maximum, &current) != 0) {
            r.rec(key, { { "status", "not available" } });
            return;
        }
        r.rec(key, { { "coarsest_ms", fnum(minimum / 10000.0, 3) }, { "finest_ms", fnum(maximum / 10000.0, 3) },
            { "current_ms", fnum(current / 10000.0, 3) } });
    };
    guard(r, "timing.resolution.default", [&] {
        report_resolution("timing.resolution.default");
        r.note("the system-wide timer resolution above may be lowered by other programs; since Windows 10 version 2004 it does not apply to this process unless it calls timeBeginPeriod itself, so the Sleep and timer measurements below are what counts");
    });

    guard(r, "timing.sleep1.default", [&] {
        std::vector<double> v = sample_sleep1(200);
        r.rec("timing.sleep1.default", stat_fields_us(v));
    });
    guard(r, "timing.sleep1.period1", [&] {
        TimePeriod tp(1);
        if (!tp.ok) throw std::runtime_error("timeBeginPeriod(1) refused");
        report_resolution("timing.resolution.period1");
        std::vector<double> v = sample_sleep1(200);
        r.rec("timing.sleep1.period1", stat_fields_us(v));
    });

    guard(r, "timing.waittimer.default", [&] {
        HANDLE t = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!t) throw std::runtime_error(win_error("CreateWaitableTimerW"));
        HandleCloser closer(t);
        int timeouts = 0;
        std::vector<double> v = sample_timer(t, 200, timeouts);
        Fields f = stat_fields_us(v);
        f.push_back({ "timeouts", num(static_cast<unsigned>(timeouts)) });
        r.rec("timing.waittimer.default", f);
    });
    guard(r, "timing.waittimer.period1", [&] {
        HANDLE t = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!t) throw std::runtime_error(win_error("CreateWaitableTimerW"));
        HandleCloser closer(t);
        TimePeriod tp(1);
        if (!tp.ok) throw std::runtime_error("timeBeginPeriod(1) refused");
        int timeouts = 0;
        std::vector<double> v = sample_timer(t, 200, timeouts);
        Fields f = stat_fields_us(v);
        f.push_back({ "timeouts", num(static_cast<unsigned>(timeouts)) });
        r.rec("timing.waittimer.period1", f);
    });
    guard(r, "timing.waittimer.highres", [&] {
        typedef HANDLE(WINAPI * CreateExFn)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        CreateExFn fn = reinterpret_cast<CreateExFn>(reinterpret_cast<void*>(GetProcAddress(kernel32(), "CreateWaitableTimerExW")));
        if (!fn) {
            r.rec("timing.waittimer.highres", { { "status", "not available" }, { "reason", "CreateWaitableTimerExW missing" } });
            return;
        }
        HANDLE t = fn(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!t) {
            r.rec("timing.waittimer.highres", { { "status", "not available" },
                { "reason", win_error("CreateWaitableTimerExW(high resolution)") } });
            return;
        }
        HandleCloser closer(t);
        int timeouts = 0;
        std::vector<double> v = sample_timer(t, 200, timeouts);
        Fields f = stat_fields_us(v);
        f.push_back({ "timeouts", num(static_cast<unsigned>(timeouts)) });
        r.rec("timing.waittimer.highres", f);
    });
}

// ---------------------------------------------------------------- 3. threads

DWORD WINAPI priority_thread(LPVOID p)
{
    *static_cast<volatile LONG*>(p) = GetThreadPriority(GetCurrentThread());
    return 42;
}

// Starts n threads that wait for a common go signal, then times them to completion.
template <class F>
bool run_parallel(int n, F work, double& elapsedUs, std::string& error)
{
    std::atomic<bool> go(false);
    std::vector<std::thread> threads;
    try {
        for (int i = 0; i < n; ++i)
            threads.emplace_back([&] {
                while (!go.load()) std::this_thread::yield();
                work();
            });
    }
    catch (const std::exception& e) {
        error = e.what();
    }
    double t0 = qpc_us();
    go = true;
    for (std::thread& t : threads) t.join();
    elapsedUs = qpc_us() - t0;
    return error.empty();
}

void probe_threads(Report& r)
{
    guard(r, "threads.createthread", [&] {
        volatile LONG seen = -999;
        DWORD id = 0;
        HANDLE h = CreateThread(nullptr, 0, priority_thread, const_cast<LONG*>(&seen), CREATE_SUSPENDED, &id);
        if (!h) throw std::runtime_error(win_error("CreateThread"));
        HandleCloser closer(h);
        BOOL setOk = SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        int got = GetThreadPriority(h);
        ResumeThread(h);
        DWORD wait = WaitForSingleObject(h, 5000);
        DWORD code = 0;
        GetExitCodeThread(h, &code);
        r.rec("threads.createthread", { { "set_priority_ok", yesno(setOk != FALSE) },
            { "get_priority_after_set", snum(got) }, { "expected", snum(THREAD_PRIORITY_ABOVE_NORMAL) },
            { "round_trip_ok", yesno(setOk && got == THREAD_PRIORITY_ABOVE_NORMAL && seen == THREAD_PRIORITY_ABOVE_NORMAL) },
            { "joined", yesno(wait == WAIT_OBJECT_0 && code == 42) } });
    });

    guard(r, "threads.std_thread", [&] {
        std::atomic<bool> ran(false);
        double t0 = qpc_us();
        std::thread t([&] { ran = true; });
        t.join();
        r.rec("threads.std_thread", { { "start_join_ok", yesno(ran.load()) }, { "start_join_us", fnum(qpc_us() - t0, 0) } });
    });

    guard(r, "threads.mutex", [&] {
        std::mutex m;
        long long counter = 0;
        double us = 0;
        std::string err;
        run_parallel(4, [&] {
            for (int i = 0; i < 50000; ++i) {
                std::lock_guard<std::mutex> lock(m);
                ++counter;
            }
        }, us, err);
        if (!err.empty()) throw std::runtime_error(err);
        r.rec("threads.mutex", { { "threads", "4" }, { "increments_each", "50000" }, { "total", snum(counter) },
            { "correct", yesno(counter == 200000) }, { "elapsed_ms", fnum(us / 1000.0, 2) } });
    });

    guard(r, "threads.condvar_pingpong", [&] {
        const int N = 2000;
        std::mutex m;
        std::condition_variable cv;
        int turn = 0;
        bool abort = false;
        std::thread responder([&] {
            for (int i = 0; i < N; ++i) {
                std::unique_lock<std::mutex> lk(m);
                if (!cv.wait_for(lk, std::chrono::seconds(5), [&] { return turn == 1 || abort; }) || abort) return;
                turn = 0;
                lk.unlock();
                cv.notify_all();
            }
        });
        std::vector<double> v;
        v.reserve(N);
        int timeouts = 0;
        for (int i = 0; i < N; ++i) {
            double t0 = qpc_us();
            {
                std::lock_guard<std::mutex> lk(m);
                turn = 1;
            }
            cv.notify_all();
            std::unique_lock<std::mutex> lk(m);
            if (!cv.wait_for(lk, std::chrono::seconds(5), [&] { return turn == 0; })) {
                ++timeouts;
                abort = true;
                lk.unlock();
                cv.notify_all();
                break;
            }
            lk.unlock();
            v.push_back(qpc_us() - t0);
        }
        responder.join();
        Fields f = stat_fields_us(v);
        f.insert(f.begin(), { "round_trips", num(v.size()) });
        f.push_back({ "timeouts", num(static_cast<unsigned>(timeouts)) });
        r.rec("threads.condvar_pingpong", f);
    });

    guard(r, "threads.atomic64", [&] {
        std::atomic<int64_t> a(0);
        double us = 0;
        std::string err;
        run_parallel(4, [&] {
            for (int i = 0; i < 100000; ++i) a.fetch_add(1);
        }, us, err);
        if (!err.empty()) throw std::runtime_error(err);
        r.rec("threads.atomic64", { { "threads", "4" }, { "increments_each", "100000" }, { "total", snum(a.load()) },
            { "correct", yesno(a.load() == 400000) }, { "is_lock_free", yesno(a.is_lock_free()) },
            { "elapsed_ms", fnum(us / 1000.0, 2) } });
    });

    guard(r, "threads.interlocked_cas64", [&] {
        alignas(8) LONGLONG v = 5;
        LONGLONG old1 = _InterlockedCompareExchange64(&v, 9, 5); // matches: becomes 9
        bool first = old1 == 5 && v == 9;
        LONGLONG old2 = _InterlockedCompareExchange64(&v, 1, 5); // no match: unchanged
        bool second = old2 == 9 && v == 9;
        r.rec("threads.interlocked_cas64", { { "match_case_ok", yesno(first) }, { "mismatch_case_ok", yesno(second) },
            { "correct", yesno(first && second) } });
    });
}

// ---------------------------------------------------------------- 4. memory

unsigned long long to_mb(unsigned long long bytes) { return bytes >> 20; }

void probe_memory(Report& r)
{
    MEMORYSTATUSEX ms;
    std::memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) throw std::runtime_error(win_error("GlobalMemoryStatusEx"));
    r.rec("memory.status", { { "total_physical_mb", num(to_mb(ms.ullTotalPhys)) }, { "available_physical_mb", num(to_mb(ms.ullAvailPhys)) },
        { "total_virtual_mb", num(to_mb(ms.ullTotalVirtual)) }, { "available_virtual_mb", num(to_mb(ms.ullAvailVirtual)) },
        { "pagefile_total_mb", num(to_mb(ms.ullTotalPageFile)) }, { "pagefile_available_mb", num(to_mb(ms.ullAvailPageFile)) },
        { "memory_load_percent", num(ms.dwMemoryLoad) } });

    guard(r, "memory.largest_reservable_region", [&] {
        double t0 = qpc_us();
        // Binary search in 1 MB steps. On 32-bit, size_t limits the search to just under 4 GB.
        unsigned long long lo = 0; // known reservable
        unsigned long long hi = std::min<unsigned long long>((ms.ullTotalVirtual >> 20) + 1,
            static_cast<unsigned long long>(~static_cast<size_t>(0)) >> 20); // assumed not reservable
        while (hi - lo > 1) {
            unsigned long long mid = lo + (hi - lo) / 2;
            void* p = VirtualAlloc(nullptr, static_cast<size_t>(mid << 20), MEM_RESERVE, PAGE_NOACCESS);
            if (p) {
                VirtualFree(p, 0, MEM_RELEASE);
                lo = mid;
            }
            else hi = mid;
        }
        r.rec("memory.largest_reservable_region", { { "mb", num(lo) }, { "search_ms", fnum((qpc_us() - t0) / 1000.0, 0) },
            { "search_limit_mb", num(hi) } });
    });

    guard(r, "memory.total_reservable", [&] {
        double t0 = qpc_us();
        const size_t segment = static_cast<size_t>(64) << 20;
        const size_t maxSegments = 64; // 4 GB
        std::vector<void*> blocks;
        for (size_t i = 0; i < maxSegments; ++i) {
            void* p = VirtualAlloc(nullptr, segment, MEM_RESERVE, PAGE_NOACCESS);
            if (!p) break;
            blocks.push_back(p);
        }
        const size_t got = blocks.size();
        for (void* p : blocks) VirtualFree(p, 0, MEM_RELEASE);
        r.rec("memory.total_reservable", { { "segment_mb", "64" }, { "segments", num(got) }, { "total_mb", num(got * 64) },
            { "stopped_because", got == maxSegments ? "reached 4 GB cap" : "reservation failed" },
            { "ms", fnum((qpc_us() - t0) / 1000.0, 0) } });
    });

    guard(r, "memory.commit_speed", [&] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const size_t page = si.dwPageSize ? si.dwPageSize : 4096;
        unsigned long long target = std::min<unsigned long long>(256ull << 20, ms.ullAvailPhys / 4);
        size_t chunk = static_cast<size_t>(16) << 20;
        size_t count = static_cast<size_t>(target / chunk);
        if (count == 0) {
            chunk = static_cast<size_t>(target / 65536 * 65536);
            count = chunk >= (static_cast<size_t>(1) << 20) ? 1 : 0;
        }
        if (count == 0) {
            r.rec("memory.commit_speed", { { "status", "skipped" }, { "reason", "less than 4 MB of physical memory available" } });
            return;
        }
        std::vector<void*> blocks;
        double t0 = qpc_us();
        for (size_t i = 0; i < count; ++i) {
            void* p = VirtualAlloc(nullptr, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!p) break;
            volatile unsigned char* bytes = static_cast<volatile unsigned char*>(p);
            for (size_t off = 0; off < chunk; off += page) bytes[off] = 1;
            blocks.push_back(p);
        }
        double seconds = (qpc_us() - t0) / 1e6;
        const unsigned long long committed = static_cast<unsigned long long>(blocks.size()) * chunk;
        for (void* p : blocks) VirtualFree(p, 0, MEM_RELEASE);
        r.rec("memory.commit_speed", { { "committed_touched_mb", num(to_mb(committed)) }, { "chunk_mb", num(chunk >> 20) },
            { "mb_per_s", seconds > 0 ? fnum(static_cast<double>(to_mb(committed)) / seconds, 0) : std::string("n/a") },
            { "seconds", fnum(seconds, 3) } });
    });

    guard(r, "memory.large_address_aware", [&] {
        bool laa = false;
        bool headerOk = false;
        HMODULE self = GetModuleHandleW(nullptr);
        if (self) {
            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(self) + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    headerOk = true;
                    laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
                }
            }
        }
        if (!headerOk) throw std::runtime_error("could not read this executable's PE header");

        const bool is32 = sizeof(void*) == 4;
        const bool wow = is_wow64();
        const bool over2gb = ms.ullTotalVirtual > (2ull << 30) + (64ull << 20);
        std::string answer, reason;
        if (!is32) {
            answer = "no";
            reason = "this is a 64-bit process, its address space is not limited to 2 GB";
        }
        else if (laa) {
            answer = "no";
            reason = "this executable is already large-address-aware; its address space is " + num(to_mb(ms.ullTotalVirtual)) + " MB";
        }
        else if (wow) {
            answer = "yes";
            reason = "32-bit program on 64-bit Windows: marking it large-address-aware raises its address space from 2 GB to 4 GB";
        }
        else if (over2gb) {
            answer = "yes";
            reason = "this 32-bit process already sees more than 2 GB of address space";
        }
        else {
            answer = "no";
            reason = "32-bit Windows: the flag only helps when Windows is booted with /3GB, which a test program that is not "
                     "large-address-aware cannot detect; otherwise the limit stays 2 GB";
        }
        r.rec("memory.large_address_aware", { { "this_exe_flag_set", yesno(laa) }, { "process_bits", num(sizeof(void*) * 8) },
            { "wow64", yesno(wow) }, { "process_address_space_mb", num(to_mb(ms.ullTotalVirtual)) },
            { "large_address_aware_would_help", answer } });
        r.note("large-address-aware would help: " + answer + " - " + reason);
    });
}

// ---------------------------------------------------------------- 5. files

struct TempFileGuard {
    std::wstring path;
    explicit TempFileGuard(std::wstring p) : path(std::move(p)) {}
    ~TempFileGuard() { DeleteFileW(path.c_str()); }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

bool file_gone(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool seek_to(HANDLE h, long long offset)
{
    LARGE_INTEGER li;
    li.QuadPart = offset;
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != FALSE;
}

bool write_exact(HANDLE h, const void* data, DWORD n)
{
    DWORD done = 0;
    return WriteFile(h, data, n, &done, nullptr) && done == n;
}

bool read_exact(HANDLE h, void* data, DWORD n)
{
    DWORD done = 0;
    return ReadFile(h, data, n, &done, nullptr) && done == n;
}

bool g_quickProbe = false; // set by run_capability_probe(quick): shorter disk throughput test

struct Throughput {
    bool ran = false;
    double writeMBps = 0;
    double readUncachedMBps = 0;
    double readCachedMBps = 0;
};

Throughput probe_throughput(Report& r, const std::wstring& folder, unsigned long long freeBytes)
{
    Throughput result;
    if (freeBytes < (256ull << 20)) {
        r.rec("files.throughput", { { "status", "skipped" }, { "reason", "less than 256 MB free in this folder" } });
        return result;
    }
    const size_t chunk = static_cast<size_t>(1) << 20;
    const int chunks = g_quickProbe ? 16 : 64; // MB
    std::wstring path = folder + L"\\probe-seq.tmp";
    TempFileGuard guardFile(path);
    void* buffer = VirtualAlloc(nullptr, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer) throw std::runtime_error(win_error("VirtualAlloc(1 MB)"));
    struct BufferFree {
        void* p;
        ~BufferFree() { VirtualFree(p, 0, MEM_RELEASE); }
    } bufferFree{ buffer };
    unsigned char* bytes = static_cast<unsigned char*>(buffer);
    for (size_t i = 0; i < chunk; ++i) bytes[i] = static_cast<unsigned char>((i * 131 + 7) & 0xFF);

    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error(win_error("CreateFile(write 64 MB file)"));
        HandleCloser closer(h);
        double t0 = qpc_us();
        for (int i = 0; i < chunks; ++i)
            if (!write_exact(h, buffer, static_cast<DWORD>(chunk))) throw std::runtime_error(win_error("WriteFile(64 MB file)"));
        result.writeMBps = chunks / ((qpc_us() - t0) / 1e6);
    }
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error(win_error("CreateFile(unbuffered read)"));
        HandleCloser closer(h);
        double t0 = qpc_us();
        for (int i = 0; i < chunks; ++i)
            if (!read_exact(h, buffer, static_cast<DWORD>(chunk))) throw std::runtime_error(win_error("ReadFile(unbuffered)"));
        result.readUncachedMBps = chunks / ((qpc_us() - t0) / 1e6);
    }
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error(win_error("CreateFile(cached read)"));
        HandleCloser closer(h);
        double t0 = qpc_us();
        for (int i = 0; i < chunks; ++i)
            if (!read_exact(h, buffer, static_cast<DWORD>(chunk))) throw std::runtime_error(win_error("ReadFile(cached)"));
        result.readCachedMBps = chunks / ((qpc_us() - t0) / 1e6);
    }
    result.ran = true;
    r.rec("files.throughput", { { "file_mb", num(static_cast<unsigned>(chunks)) },{ "write_through_write_mb_per_s", fnum(result.writeMBps, 1) },
        { "uncached_read_1mb_mb_per_s", fnum(result.readUncachedMBps, 1) }, { "cached_read_mb_per_s", fnum(result.readCachedMBps, 1) } });
    DeleteFileW(path.c_str());
    r.rec("files.throughput.cleanup", { { "temp_file_deleted", yesno(file_gone(path)) } });
    return result;
}

struct LargeStep {
    long long offset;
    const char* name;
};

void probe_large_file(Report& r, const std::wstring& folder, const std::string& fsName, DWORD fsFlags,
    unsigned long long freeBytes, const Throughput& tp)
{
    const long long MB = 1024ll * 1024;
    const long long GB = 1024ll * MB;
    const bool isNtfs = fsName == "NTFS";
    const bool isFat = fsName.compare(0, 3, "FAT") == 0;
    const bool isFat32 = fsName == "FAT32";
    const bool sparseSupported = isNtfs || (fsFlags & FILE_SUPPORTS_SPARSE_FILES) != 0;

    if (isFat32) r.note("files of 4 GB or more are impossible on FAT32 (this is a limit of the file system, not a failure)");

    std::wstring path = folder + L"\\probe-large.tmp";
    TempFileGuard guardFile(path);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error(win_error("CreateFile(probe-large.tmp)"));
    {
        HandleCloser closer(h);

        bool sparse = false;
        if (sparseSupported) {
            DWORD bytes = 0;
            double t0 = qpc_us();
            BOOL ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr);
            DWORD err = GetLastError();
            sparse = ok != FALSE;
            r.rec("files.large.set_sparse", { { "result", sparse ? "ok" : "failed" },
                { "detail", sparse ? std::string() : win_error("FSCTL_SET_SPARSE", err) },
                { "ms", fnum((qpc_us() - t0) / 1000.0, 1) } });
        }
        else r.rec("files.large.set_sparse", { { "result", "not supported" }, { "detail", "file system '" + fsName + "' has no sparse files" } });

        // Decide which offsets are possible.
        std::vector<LargeStep> steps;
        steps.push_back({ 1 * MB, "offset_1MB" });
        long long size = 2 * MB;
        std::string skipped;
        const double estimateLimitSeconds = 30.0;
        auto dense_ok = [&](long long bytesToWrite, const char* what) -> bool {
            if (!tp.ran || tp.writeMBps <= 0) {
                skipped += std::string(what) + ": no write-speed estimate available; ";
                return false;
            }
            double seconds = static_cast<double>(bytesToWrite / MB) / tp.writeMBps;
            if (seconds > estimateLimitSeconds) {
                skipped += std::string(what) + ": would need to write about " + num(static_cast<unsigned long long>(bytesToWrite / MB)) +
                    " MB (about " + num(static_cast<unsigned long long>(seconds)) + " s at the measured speed); ";
                return false;
            }
            return true;
        };
        if (sparse) {
            steps.push_back({ 2 * GB + 64 * 1024, "offset_2GB_plus_64KB" });
            steps.push_back({ 4 * GB + 64 * 1024, "offset_4GB_plus_64KB" });
            size = 4 * GB + 1 * MB;
        }
        else if (isFat32) {
            if (freeBytes >= static_cast<unsigned long long>(2.2 * static_cast<double>(GB)) && dense_ok(2 * GB + 1 * MB, "2 GB offset")) {
                steps.push_back({ 2 * GB + 64 * 1024, "offset_2GB_plus_64KB" });
                size = 2 * GB + 1 * MB;
            }
            else if (freeBytes < static_cast<unsigned long long>(2.2 * static_cast<double>(GB)))
                skipped += "2 GB offset: less than 2.2 GB free; ";
        }
        else if (isFat) {
            skipped += "offsets above 2 GB: plain FAT limit; ";
        }
        else {
            if (freeBytes >= static_cast<unsigned long long>(4.6 * static_cast<double>(GB)) && dense_ok(4 * GB + 1 * MB, "4 GB size")) {
                steps.push_back({ 2 * GB + 64 * 1024, "offset_2GB_plus_64KB" });
                steps.push_back({ 4 * GB + 64 * 1024, "offset_4GB_plus_64KB" });
                size = 4 * GB + 1 * MB;
            }
            else if (freeBytes < static_cast<unsigned long long>(4.6 * static_cast<double>(GB)))
                skipped += "files above 4 GB: sparse files not available and less than 4.6 GB free; ";
        }
        if (!skipped.empty()) r.note("large-file test limited: " + skipped);

        {
            double t0 = qpc_us();
            bool ok = seek_to(h, size) && SetEndOfFile(h);
            DWORD err = GetLastError();
            r.rec("files.large.set_size", { { "bytes", snum(size) }, { "result", ok ? "ok" : "failed" },
                { "detail", ok ? std::string() : win_error("SetEndOfFile", err) }, { "ms", fnum((qpc_us() - t0) / 1000.0, 1) } });
            if (!ok) return;
        }

        std::vector<unsigned char> block(64 * 1024), back(64 * 1024);
        for (size_t s = 0; s < steps.size(); ++s) {
            for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<unsigned char>((i * 29 + s * 53 + 11) & 0xFF);
            double t0 = qpc_us();
            bool wrote = seek_to(h, steps[s].offset) && write_exact(h, block.data(), static_cast<DWORD>(block.size()));
            DWORD wErr = GetLastError();
            double t1 = qpc_us();
            std::fill(back.begin(), back.end(), static_cast<unsigned char>(0));
            bool read = wrote && seek_to(h, steps[s].offset) && read_exact(h, back.data(), static_cast<DWORD>(back.size()));
            DWORD rErr = GetLastError();
            double t2 = qpc_us();
            bool match = read && std::memcmp(block.data(), back.data(), block.size()) == 0;
            std::string detail;
            if (!wrote) detail = win_error("positioned write", wErr);
            else if (!read) detail = win_error("positioned read", rErr);
            r.rec(std::string("files.large.") + steps[s].name, { { "offset", snum(steps[s].offset) }, { "write", wrote ? "ok" : "failed" },
                { "write_ms", fnum((t1 - t0) / 1000.0, 1) }, { "read", read ? "ok" : "failed" }, { "read_ms", fnum((t2 - t1) / 1000.0, 1) },
                { "verify", match ? "ok" : "failed" }, { "detail", detail } });
        }
    } // handle closed here: DELETE_ON_CLOSE removes the file

    double t0 = qpc_us();
    DeleteFileW(path.c_str()); // belt and braces; normally already gone
    r.rec("files.large.cleanup", { { "temp_file_deleted", yesno(file_gone(path)) }, { "ms", fnum((qpc_us() - t0) / 1000.0, 1) } });
}

void probe_files(Report& r, const std::wstring& folderIn)
{
    const std::wstring folder = full_path(folderIn);
    r.rec("files.folder", { { "path", wide_ascii(folder) } });

    wchar_t root[MAX_PATH + 4] = {};
    if (!GetVolumePathNameW(folder.c_str(), root, MAX_PATH)) throw std::runtime_error(win_error("GetVolumePathNameW"));
    wchar_t fsName[MAX_PATH + 4] = {};
    DWORD serial = 0, maxComponent = 0, fsFlags = 0;
    if (!GetVolumeInformationW(root, nullptr, 0, &serial, &maxComponent, &fsFlags, fsName, MAX_PATH))
        throw std::runtime_error(win_error("GetVolumeInformationW"));
    ULARGE_INTEGER freeToCaller, total, totalFree;
    if (!GetDiskFreeSpaceExW(folder.c_str(), &freeToCaller, &total, &totalFree)) throw std::runtime_error(win_error("GetDiskFreeSpaceExW"));
    std::string fs = wide_ascii(fsName);
    for (char& c : fs) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    r.rec("files.volume", { { "root", wide_ascii(root) }, { "file_system", fs },
        { "supports_sparse_files", yesno((fsFlags & FILE_SUPPORTS_SPARSE_FILES) != 0) },
        { "free_bytes", num(freeToCaller.QuadPart) }, { "free_mb", num(to_mb(freeToCaller.QuadPart)) } });

    // Sequential throughput runs first: its write speed decides whether a
    // non-sparse large-file test is affordable. Its lines are reported last.
    std::vector<std::string> deferredHuman, deferredMachine;
    Report deferred(deferredHuman, deferredMachine);
    Throughput tp;
    guard(deferred, "files.throughput", [&] { tp = probe_throughput(deferred, folder, freeToCaller.QuadPart); });

    guard(r, "files.large", [&] { probe_large_file(r, folder, fs, fsFlags, freeToCaller.QuadPart, tp); });
    r.append(deferredHuman, deferredMachine);
}

// ---------------------------------------------------------------- 6. MIDI

std::string midi_technology(WORD t)
{
    switch (t) {
    case MOD_MIDIPORT: return "MIDI hardware port";
    case MOD_SYNTH: return "synthesizer";
    case MOD_SQSYNTH: return "square wave synthesizer";
    case MOD_FMSYNTH: return "FM synthesizer";
    case MOD_MAPPER: return "Microsoft MIDI mapper";
    case 6: return "hardware wavetable synthesizer";
    case 7: return "software synthesizer";
    default: return "unknown(" + num(t) + ")";
    }
}

std::string midi_support(DWORD s)
{
    std::string out;
    if (s & MIDICAPS_VOLUME) out += "volume ";
    if (s & MIDICAPS_LRVOLUME) out += "left-right-volume ";
    if (s & MIDICAPS_CACHE) out += "patch-caching ";
    if (s & MIDICAPS_STREAM) out += "midiStream ";
    if (!out.empty()) out.pop_back();
    return out.empty() ? "none" : out;
}

void report_midi_device(Report& r, const std::string& key, UINT_PTR id)
{
    MIDIOUTCAPSW caps;
    std::memset(&caps, 0, sizeof(caps));
    MMRESULT mr = midiOutGetDevCapsW(id, &caps, sizeof(caps)); // query only, nothing is opened
    if (mr != MMSYSERR_NOERROR) {
        r.rec(key, { { "status", "midiOutGetDevCapsW failed" }, { "mmresult", num(mr) } });
        return;
    }
    r.rec(key, { { "name", wide_ascii(caps.szPname) }, { "technology", midi_technology(caps.wTechnology) },
        { "voices", num(caps.wVoices) }, { "notes", num(caps.wNotes) }, { "channel_mask", hex(caps.wChannelMask, 4) },
        { "support", midi_support(caps.dwSupport) }, { "driver_version", num(HIBYTE(caps.vDriverVersion)) + "." + num(LOBYTE(caps.vDriverVersion)) } });
}

void inspect_dll(Report& r, const std::string& key, const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        r.rec(key, { { "path", wide_ascii(path) }, { "found", "no" } });
        return;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.rec(key, { { "path", wide_ascii(path) }, { "found", "yes" }, { "error", win_error("open for reading") } });
        return;
    }
    HandleCloser closer(h);
    LARGE_INTEGER size;
    size.QuadPart = 0;
    GetFileSizeEx(h, &size);
    std::string machineText = "unreadable";
    IMAGE_DOS_HEADER dos;
    DWORD got = 0;
    if (ReadFile(h, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew > 0 && seek_to(h, dos.e_lfanew)) {
        unsigned char pe[6] = {};
        if (ReadFile(h, pe, sizeof(pe), &got, nullptr) && got == sizeof(pe) && pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0) {
            unsigned machine = pe[4] | (static_cast<unsigned>(pe[5]) << 8);
            machineText = machine == 0x014c ? "x86 (32-bit)" : machine == 0x8664 ? "x64 (64-bit)" : machine == 0xAA64 ? "ARM64" : "other " + hex(machine, 4);
        }
        else machineText = "not a PE file";
    }
    r.rec(key, { { "path", wide_ascii(path) }, { "found", "yes" }, { "machine", machineText }, { "size_bytes", snum(size.QuadPart) } });
}

void probe_midi(Report& r, const std::wstring& folderIn)
{
    guard(r, "midi.devices", [&] {
        UINT n = midiOutGetNumDevs();
        r.rec("midi.count", { { "output_devices", num(n) }, { "note", "enumeration only; no device is opened and nothing is played" } });
        for (UINT i = 0; i < n; ++i) report_midi_device(r, "midi.device." + num(i), i);
        report_midi_device(r, "midi.mapper", static_cast<UINT_PTR>(MIDI_MAPPER));
    });

    guard(r, "midi.omnimidi", [&] {
        const std::wstring folder = full_path(folderIn);
        wchar_t exe[MAX_PATH + 4] = {};
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring exeDir(exe, n);
        size_t slash = exeDir.find_last_of(L"\\/");
        exeDir = slash == std::wstring::npos ? std::wstring(L".") : exeDir.substr(0, slash);

        inspect_dll(r, "midi.omnimidi.exe_folder", exeDir + L"\\OmniMIDI.dll");
        inspect_dll(r, "midi.omnimidi.package_folder", folder + L"\\OmniMIDI.dll");
        std::wstring sys = system_directory();
        if (!sys.empty()) inspect_dll(r, "midi.omnimidi.system_dir", sys + L"\\OmniMIDI.dll");

        typedef UINT(WINAPI * Wow64DirFn)(LPWSTR, UINT);
        Wow64DirFn wowDir = reinterpret_cast<Wow64DirFn>(reinterpret_cast<void*>(GetProcAddress(kernel32(), "GetSystemWow64DirectoryW")));
        if (wowDir) {
            wchar_t buf[MAX_PATH + 4] = {};
            UINT len = wowDir(buf, MAX_PATH);
            if (len) inspect_dll(r, "midi.omnimidi.syswow64_dir", std::wstring(buf, len) + L"\\OmniMIDI.dll");
            else r.rec("midi.omnimidi.syswow64_dir", { { "status", "no 32-bit system folder (32-bit Windows)" } });
        }
        else r.rec("midi.omnimidi.syswow64_dir", { { "status", "not available (GetSystemWow64DirectoryW missing)" } });

        if (is_wow64()) {
            std::wstring win = windows_directory();
            if (!win.empty()) inspect_dll(r, "midi.omnimidi.native_system32_via_sysnative", win + L"\\Sysnative\\OmniMIDI.dll");
        }
        r.note("OmniMIDI.dll is only looked for as a file; it is never loaded");
    });
}

// ---------------------------------------------------------------- 7. user interface

int CALLBACK font_found(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param)
{
    *reinterpret_cast<bool*>(param) = true;
    return 0; // stop at the first match
}

std::string file_version(const std::wstring& path)
{
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return "unknown";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &len) || !info || len < sizeof(VS_FIXEDFILEINFO)) return "unknown";
    return num(HIWORD(info->dwFileVersionMS)) + "." + num(LOWORD(info->dwFileVersionMS)) + "." + num(HIWORD(info->dwFileVersionLS)) + "." +
        num(LOWORD(info->dwFileVersionLS));
}

std::string sxs_matches(const std::wstring& pattern, int& count)
{
    count = 0;
    std::string names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern.c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    do {
        if (++count <= 4) names += (names.empty() ? "" : ",") + wide_ascii(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return names;
}

void probe_ui(Report& r)
{
    guard(r, "ui.fonts", [&] {
        HDC dc = GetDC(nullptr);
        if (!dc) throw std::runtime_error(win_error("GetDC"));
        Fields fields;
        const wchar_t* names[] = { L"Segoe UI", L"Tahoma", L"Lucida Console", L"Consolas" };
        for (const wchar_t* name : names) {
            LOGFONTW lf;
            std::memset(&lf, 0, sizeof(lf));
            lf.lfCharSet = DEFAULT_CHARSET;
            wcscpy_s(lf.lfFaceName, LF_FACESIZE, name);
            bool found = false;
            EnumFontFamiliesExW(dc, &lf, font_found, reinterpret_cast<LPARAM>(&found), 0);
            std::string key = wide_ascii(name);
            for (char& c : key)
                if (c == ' ') c = '_';
            fields.push_back({ key, yesno(found) });
        }
        ReleaseDC(nullptr, dc);
        r.rec("ui.fonts", fields);
    });

    guard(r, "ui.display", [&] {
        HDC dc = GetDC(nullptr);
        if (!dc) throw std::runtime_error(win_error("GetDC"));
        int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY), bpp = GetDeviceCaps(dc, BITSPIXEL);
        ReleaseDC(nullptr, dc);
        r.rec("ui.dpi", { { "x", snum(dpiX) }, { "y", snum(dpiY) }, { "scale_percent", snum(dpiX * 100 / 96) },
            { "note", "as seen by this program, which declares no DPI awareness" } });
        r.rec("ui.screen", { { "primary_width", snum(GetSystemMetrics(SM_CXSCREEN)) }, { "primary_height", snum(GetSystemMetrics(SM_CYSCREEN)) },
            { "virtual_left", snum(GetSystemMetrics(SM_XVIRTUALSCREEN)) }, { "virtual_top", snum(GetSystemMetrics(SM_YVIRTUALSCREEN)) },
            { "virtual_width", snum(GetSystemMetrics(SM_CXVIRTUALSCREEN)) }, { "virtual_height", snum(GetSystemMetrics(SM_CYVIRTUALSCREEN)) },
            { "monitors", snum(GetSystemMetrics(SM_CMONITORS)) }, { "bits_per_pixel", snum(bpp) } });
    });

    guard(r, "ui.themes", [&] {
        std::wstring sys = system_directory();
        HMODULE ux = LoadLibraryW((sys + L"\\uxtheme.dll").c_str());
        if (!ux) {
            r.rec("ui.themes", { { "status", "not available" }, { "reason", "uxtheme.dll missing" } });
            return;
        }
        typedef BOOL(WINAPI * BoolFn)();
        BoolFn active = reinterpret_cast<BoolFn>(reinterpret_cast<void*>(GetProcAddress(ux, "IsThemeActive")));
        BoolFn themed = reinterpret_cast<BoolFn>(reinterpret_cast<void*>(GetProcAddress(ux, "IsAppThemed")));
        r.rec("ui.themes", { { "IsThemeActive", active ? yesno(active() != FALSE) : std::string("not available") },
            { "IsAppThemed_for_this_program", themed ? yesno(themed() != FALSE) : std::string("not available") } });
        FreeLibrary(ux);
    });

    guard(r, "ui.comctl32", [&] {
        std::wstring win = windows_directory();
        int count32 = 0, count64 = 0;
        std::string n32 = sxs_matches(win + L"\\WinSxS\\x86_Microsoft.Windows.Common-Controls_6595b64144ccf1df_6.*", count32);
        std::string n64 = sxs_matches(win + L"\\WinSxS\\amd64_Microsoft.Windows.Common-Controls_6595b64144ccf1df_6.*", count64);
        r.rec("ui.comctl32_v6_sidebyside", { { "x86_assemblies", num(static_cast<unsigned>(count32)) }, { "x86_names", n32 },
            { "amd64_assemblies", num(static_cast<unsigned>(count64)) }, { "amd64_names", n64 },
            { "visual_styles_for_buttons_possible", yesno(count32 > 0 || count64 > 0) } });

        HMODULE cc = GetModuleHandleW(L"comctl32.dll");
        const bool alreadyLoaded = cc != nullptr;
        if (!cc) cc = LoadLibraryW((system_directory() + L"\\comctl32.dll").c_str());
        if (!cc) {
            r.rec("ui.comctl32_loaded", { { "status", "not available" } });
            return;
        }
        wchar_t path[MAX_PATH + 4] = {};
        GetModuleFileNameW(cc, path, MAX_PATH);
        r.rec("ui.comctl32_loaded", { { "was_loaded_before", yesno(alreadyLoaded) }, { "path", wide_ascii(path) }, { "version", file_version(path) } });
        if (!alreadyLoaded) FreeLibrary(cc);
    });

    guard(r, "ui.layered_window", [&] {
        typedef BOOL(WINAPI * SetLayeredFn)(HWND, COLORREF, BYTE, DWORD);
        SetLayeredFn setLayered = reinterpret_cast<SetLayeredFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetLayeredWindowAttributes")));
        if (!setLayered) {
            r.rec("ui.layered_window", { { "status", "not available" }, { "reason", "SetLayeredWindowAttributes missing" } });
            return;
        }
        const wchar_t* className = L"MidibnCapabilityProbeWindow";
        WNDCLASSEXW wc;
        std::memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        if (!RegisterClassExW(&wc)) throw std::runtime_error(win_error("RegisterClassExW"));
        HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, className, L"", WS_POPUP, 3900, 1150, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            DWORD err = GetLastError();
            UnregisterClassW(className, wc.hInstance);
            throw std::runtime_error(win_error("CreateWindowExW", err));
        }
        BOOL ok = setLayered(hwnd, 0, 128, LWA_ALPHA); // the window is never shown
        DWORD err = GetLastError();
        DestroyWindow(hwnd);
        UnregisterClassW(className, wc.hInstance);
        r.rec("ui.layered_window", { { "SetLayeredWindowAttributes", ok ? "ok" : "failed" },
            { "detail", ok ? std::string() : win_error("SetLayeredWindowAttributes", err) }, { "window_shown", "no" } });
    });
}

} // namespace

std::vector<std::string> run_capability_probe(const std::wstring& packageFolder, std::vector<std::string>& machine, bool quick)
{
    std::vector<std::string> human;
    machine.clear();
    g_quickProbe = quick;
    Report r(human, machine);
    human.push_back("CAPABILITY REPORT (measurements, not pass/fail)");
    const double started = qpc_us();
    r.rec("probe.build", { { "process_bits", num(sizeof(void*) * 8) }, { "winnt_target", hex(_WIN32_WINNT, 4) } });

    section(r, "1. Processor", "processor", [&] { probe_processor(r); });
    section(r, "2. Timing", "timing", [&] { probe_timing(r); });
    section(r, "3. Threads", "threads", [&] { probe_threads(r); });
    section(r, "4. Memory", "memory", [&] { probe_memory(r); });
    section(r, "5. Files", "files", [&] { probe_files(r, packageFolder); });
    section(r, "6. MIDI (enumeration only)", "midi", [&] { probe_midi(r, packageFolder); });
    section(r, "7. User interface", "ui", [&] { probe_ui(r); });

    human.push_back("");
    r.rec("probe.total", { { "elapsed_ms", fnum((qpc_us() - started) / 1000.0, 0) } });
    return human;
}

} // namespace tools
} // namespace midibn
