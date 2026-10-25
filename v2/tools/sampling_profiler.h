// A small sampling profiler for measurement builds (Milestone 7, helper P8-M7B). Not part of the player.
//
// A sampler thread suspends each thread of this process (except its own and the one that started the sampler) at a
// fixed pace, reads the instruction pointer and puts the thread straight back. Afterwards the addresses are turned
// into function names and source lines with Microsoft's DbgHelp (dbghelp.dll from Windows, loaded at run time; the
// executable needs its .pdb next to it, so build with /Zi and /DEBUG, see v2\tools\throughput-measure.ps1 -Profile).
// It exists because no profiler is installed on the build PC; it measures where the worker thread spends its time.
#pragma once

#ifdef MIDIBN_XP_BUILD
// The Windows XP toolset's SDK header does not compile under the strict build settings, and a measurement build does
// not need the profiler there: the same interface, doing nothing.
namespace midibn {
namespace tools {
class SamplingProfiler {
public:
    void start(unsigned long, int) {}
    void stop() {}
    bool report(unsigned) { return false; }
};
} // namespace tools
} // namespace midibn
#else

#include <windows.h>
#pragma warning(push)
#pragma warning(disable : 4091)   // the Windows 7.1A SDK's dbghelp.h has unnamed typedefs
#include <dbghelp.h>
#pragma warning(pop)
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace midibn {
namespace tools {

class SamplingProfiler {
public:
    // `skipThread`: the calling thread (mostly waiting) is left out. `periodMicroseconds`: pause between samples.
    void start(DWORD skipThread, int periodMicroseconds)
    {
        skip_ = skipThread;
        period_ = periodMicroseconds;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    // Prints the busiest functions and lines. Returns false if DbgHelp could not be loaded.
    bool report(size_t top)
    {
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        if (!dbg) return false;
        typedef BOOL(WINAPI * InitFn)(HANDLE, PCSTR, BOOL);
        typedef DWORD(WINAPI * OptFn)(DWORD);
        typedef BOOL(WINAPI * SymFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
        typedef BOOL(WINAPI * LineFn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
        InitFn init = reinterpret_cast<InitFn>(GetProcAddress(dbg, "SymInitialize"));
        OptFn options = reinterpret_cast<OptFn>(GetProcAddress(dbg, "SymSetOptions"));
        SymFn fromAddr = reinterpret_cast<SymFn>(GetProcAddress(dbg, "SymFromAddr"));
        LineFn lineFromAddr = reinterpret_cast<LineFn>(GetProcAddress(dbg, "SymGetLineFromAddr64"));
        typedef BOOL(WINAPI * CleanupFn)(HANDLE);
        CleanupFn cleanup = reinterpret_cast<CleanupFn>(GetProcAddress(dbg, "SymCleanup"));
        if (!init || !options || !fromAddr || !lineFromAddr || !cleanup) return false;
        HANDLE process = GetCurrentProcess();
        options(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        if (!init(process, nullptr, TRUE)) return false;

        std::map<std::string, size_t> byFunction, byLine, byThread;
        for (const Sample& s : samples_) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            std::string function = "?";
            if (fromAddr(process, s.address, &displacement, symbol)) function = symbol->Name;
            byFunction[function]++;
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            std::string where = function + " @?";
            if (lineFromAddr(process, s.address, &lineDisplacement, &line)) {
                const char* file = line.FileName;
                const char* slash = file;
                for (const char* p = file; *p; ++p) if (*p == '\\' || *p == '/') slash = p + 1;
                where = function + " @" + slash + ":" + std::to_string(line.LineNumber);
            }
            byLine[where]++;
            byThread["thread " + std::to_string(s.thread)]++;
        }
        print("by thread", byThread, 6);
        print("by function", byFunction, top);
        print("by source line", byLine, top);
        cleanup(process);   // a second report in the same process (perf --analysis --profile) initialises again
        return true;
    }

    size_t sample_count() const { return samples_.size(); }

private:
    struct Sample {
        DWORD64 address;
        DWORD thread;
    };

    static void print(const char* title, const std::map<std::string, size_t>& counts, size_t top)
    {
        std::vector<std::pair<size_t, std::string>> rows;
        size_t total = 0;
        for (const auto& kv : counts) { rows.push_back({ kv.second, kv.first }); total += kv.second; }
        std::sort(rows.begin(), rows.end(), [](const std::pair<size_t, std::string>& a, const std::pair<size_t, std::string>& b) { return a.first > b.first; });
        std::printf("profile-%s (%zu samples)\n", title, total);
        for (size_t i = 0; i < rows.size() && i < top; ++i)
            std::printf("  %5.1f%%  %s\n", 100.0 * static_cast<double>(rows[i].first) / static_cast<double>(total), rows[i].second.c_str());
    }

    void run()
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        std::vector<std::pair<DWORD, HANDLE>> threads;
        auto refresh = [&] {
            for (auto& t : threads) CloseHandle(t.second);
            threads.clear();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return;
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self || entry.th32ThreadID == skip_) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (h) threads.push_back({ entry.th32ThreadID, h });
            }
            CloseHandle(snapshot);
        };
        int rounds = 0;
        while (!stop_) {
            if (rounds++ % 200 == 0) refresh();
            for (auto& t : threads) {
                if (SuspendThread(t.second) == static_cast<DWORD>(-1)) continue;
                CONTEXT context = {};
                context.ContextFlags = CONTEXT_CONTROL;
                const bool got = GetThreadContext(t.second, &context) != FALSE;
                // Resume before storing: push_back may allocate, and the suspended thread may hold the heap lock
                // (a busy single-threaded workload such as the Analysis allocates often; storing first deadlocked).
                ResumeThread(t.second);
                if (got) {
#ifdef _M_X64
                    samples_.push_back({ context.Rip, t.first });
#else
                    samples_.push_back({ context.Eip, t.first });
#endif
                }
            }
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            const int64_t until = a.QuadPart + static_cast<int64_t>(period_) * frequency.QuadPart / 1000000;
            do { QueryPerformanceCounter(&b); } while (b.QuadPart < until && !stop_);
        }
        for (auto& t : threads) CloseHandle(t.second);
    }

    std::thread thread_;
    std::atomic<bool> stop_{ false };
    std::vector<Sample> samples_;
    DWORD skip_ = 0;
    int period_ = 200;
};

} // namespace tools
} // namespace midibn

#endif // MIDIBN_XP_BUILD
