// --auto mode of midibn-few-cpu-measure (P12-WINPLAT): the silent few-processor measurement for the XP self-test package.
// One call runs the whole measurement and writes one readable result file (few-processor-result.txt):
//   * it makes a silent MIDI file of controller messages only (no Note On, so no sound even on a real synthesizer, and
//     none is opened: the output is None, a counter, with a per-message processor cost so that playback always falls behind);
//   * it runs this same program once per condition (child processes, so each run starts with fresh threads):
//       processors  2 logical (the two hyper-threads of a Pentium 4 core; the two lowest usable processors of any PC)
//                   and 1 logical (the lowest usable processor), set with the process affinity mask;
//       option      off, and on = "Let other programs run while playback is behind" (the playback loop yields every 2 ms
//                   while it is behind, DD-112), in interleaved rounds;
//   * it reads each child's line and writes the medians: how far playback is behind when the song ends, the largest lateness
//     of any send, messages sent per second, and how long a competing Normal-priority thread (a stand-in for the player's
//     window and every other program) waited past its wake-up time: typical, 99th percentile and longest.
// No MIDI device is opened, no driver is loaded, no sound is made. Total time is bounded (see kBudgetSeconds).
#pragma once

#include "thread_report.h"
#include "tool_support.h"
#include "win32_timing.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace few_cpu_auto {

const double kBudgetSeconds = 80.0;   // no new run starts after this much time (the whole run stays under about 2 minutes on a slow PC)

inline std::wstring widen(const std::string& text)
{
    if (text.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], n);
    return wide;
}

inline double now_seconds()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// A Standard MIDI File (format 0) of controller messages only: `rate` messages per second for `seconds` seconds.
inline std::string make_silent_file(int rate, int seconds)
{
    const int ticksPerSecond = 2000;   // division 1000 at the default tempo of 120 beats per minute
    const int perTick = std::max(1, rate / ticksPerSecond);
    std::string track;
    track.reserve(static_cast<size_t>(perTick) * static_cast<size_t>(ticksPerSecond) * static_cast<size_t>(seconds) * 4 + 16);
    uint32_t i = 0;
    for (int tick = 0; tick < ticksPerSecond * seconds; ++tick) {
        for (int k = 0; k < perTick; ++k, ++i) {
            track += static_cast<char>(k == 0 ? 1 : 0);                       // delta time: one tick after the previous group
            track += static_cast<char>(0xB0 | (i & 15));                      // control change, channel i % 16
            track += static_cast<char>(20 + (i >> 4) % 12);                   // controllers 20..31 (undefined controllers)
            track += static_cast<char>((i * 7) & 127);
        }
    }
    track += std::string("\x00\xFF\x2F\x00", 4);
    std::string file("MThd\x00\x00\x00\x06\x00\x00\x00\x01\x03\xE8", 14);
    file += "MTrk";
    const uint32_t length = static_cast<uint32_t>(track.size());
    for (int shift = 24; shift >= 0; shift -= 8) file += static_cast<char>((length >> shift) & 0xFF);
    return file + track;
}

inline double field(const std::string& line, const char* key)
{
    const std::string needle = std::string(" ") + key + "=";
    size_t at = (" " + line).find(needle);
    if (at == std::string::npos) return -1;
    return std::atof((" " + line).c_str() + at + needle.size());
}

inline double median(std::vector<double> v)
{
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[(v.size() - 1) / 2];
}

struct Condition {
    int cpus;               // 2 or 1 logical processors
    bool option;            // "Let other programs run while playback is behind"
    unsigned long long mask;
    std::vector<std::string> lines;
    std::string label() const
    {
        return std::string(cpus == 2 ? "2 logical processors" : "1 logical processor ") + (option ? ", option ON " : ", option off");
    }
};

// Runs this program as a child with the given arguments; true when it ended by itself within the limit.
inline bool run_child(const std::wstring& arguments, double limitSeconds)
{
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + arguments;
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    if (!CreateProcessW(nullptr, &buffer[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return false;
    const DWORD waited = WaitForSingleObject(info.hProcess, static_cast<DWORD>(limitSeconds * 1000.0));
    if (waited != WAIT_OBJECT_0) TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return waited == WAIT_OBJECT_0;
}

inline std::string number(double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

inline int run(const std::vector<std::string>& args)
{
    using namespace midibn;
    std::string outFile = tools::executable_directory() + "\\few-processor-result.txt";
    int rounds = 2, rate = 250000, seconds = 3, costNs = 5000;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--out") outFile = args[i + 1];
        else if (args[i] == "--rounds") rounds = std::max(1, std::atoi(args[i + 1].c_str()));
        else if (args[i] == "--rate") rate = std::max(2000, std::atoi(args[i + 1].c_str()));
        else if (args[i] == "--seconds") seconds = std::max(1, std::atoi(args[i + 1].c_str()));
        else if (args[i] == "--cost-ns") costNs = std::max(0, std::atoi(args[i + 1].c_str()));
    }
    const double started = now_seconds();
    std::string base = outFile;
    const std::string fixture = base + ".fixture.tmp.mid";
    const std::string runs = base + ".runs.tmp";
    DeleteFileW(widen(runs).c_str());
    if (!tools::write_file(fixture, make_silent_file(rate, seconds))) {
        tools::write_stdout("cannot write the test file " + fixture + "\n");
        return 2;
    }

    DWORD_PTR processMask = 0, systemMask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    unsigned long long bits[2] = { 0, 0 };   // the lowest usable processor, then the two lowest
    int found = 0;
    for (int b = 0; b < 64 && found < 2; ++b) {
        if (((static_cast<unsigned long long>(processMask) >> b) & 1ull) == 0) continue;
        if (found == 0) bits[0] = 1ull << b;
        bits[1] |= 1ull << b;
        ++found;
    }
    std::vector<Condition> conditions;
    if (found >= 2) {
        conditions.push_back(Condition{ 2, false, bits[1], {} });
        conditions.push_back(Condition{ 2, true, bits[1], {} });
    }
    conditions.push_back(Condition{ 1, false, bits[0], {} });
    conditions.push_back(Condition{ 1, true, bits[0], {} });

    int done = 0, failed = 0, skipped = 0;
    for (int round = 1; round <= rounds; ++round) {
        for (size_t c = 0; c < conditions.size(); ++c) {
            if (now_seconds() - started > kBudgetSeconds) { ++skipped; continue; }
            Condition& cond = conditions[c];
            char mask[32];
            std::snprintf(mask, sizeof(mask), "%llx", cond.mask);
            const std::string label = "cpus" + std::to_string(cond.cpus) + (cond.option ? "-on" : "-off");
            std::string arguments = "--fixture \"" + fixture + "\" --label " + label + " --affinity " + mask + " --send-cost-ns " + std::to_string(costNs) +
                " --few-cpu " + (cond.option ? "yield=2" : "off") + " --out \"" + runs + "\"";
            tools::write_stdout("round " + std::to_string(round) + ": " + cond.label() + " ...\n");
            // limit: the song plus a generous allowance for loading and for falling behind
            if (!run_child(widen(arguments), seconds * 6.0 + 40.0)) { ++failed; continue; }
            ++done;
        }
    }
    std::string text;
    tools::read_file(runs, text);
    DeleteFileW(widen(runs).c_str());
    DeleteFileW(widen(fixture).c_str());
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (!line.empty()) lines.push_back(line);
        pos = end + 1;
    }
    for (size_t c = 0; c < conditions.size(); ++c) {
        const std::string label = "cpus" + std::to_string(conditions[c].cpus) + (conditions[c].option ? "-on" : "-off");
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].compare(0, 6 + label.size(), "label=" + label) == 0 && lines[i].size() > 6 + label.size() && lines[i][6 + label.size()] == ' ') conditions[c].lines.push_back(lines[i]);
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    char when[64];
    std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::string out;
    out += "FEW-PROCESSOR MEASUREMENT (silent: output None, no MIDI device, no driver, no sound)\r\n";
    out += std::string("Date: ") + when + "\r\n";
    out += "Processors usable by this program: " + std::to_string(win32::usable_logical_processors()) + " logical, physical cores: " +
        (win32::physical_core_count() == 0 ? std::string("unknown") : std::to_string(win32::physical_core_count())) + "\r\n";
    out += "Workload: " + std::to_string(rate) + " controller messages per second for " + std::to_string(seconds) + " s, each costing " +
        number(costNs / 1000.0, 1) + " microseconds of processor time, so playback always falls behind.\r\n";
    out += "Option = \"Let other programs run while playback is behind\" (the playback loop gives other programs a turn every 2 ms while behind).\r\n";
    out += "A competing Normal-priority thread (stand-in for the player's window and other programs) asks to run 60 times a second;\r\n";
    out += "its wake-up delay is how long it waited past its time. Rounds: " + std::to_string(rounds) + " (medians shown).\r\n\r\n";
    out += "condition                           runs  behind at end (s)  worst send lateness (s)  messages/s     competing thread delay (ms): typical  99%   longest\r\n";
    double longest[2][2] = { { -1, -1 }, { -1, -1 } }, lag[2][2] = { { -1, -1 }, { -1, -1 } }, meps[2][2] = { { -1, -1 }, { -1, -1 } };
    for (size_t c = 0; c < conditions.size(); ++c) {
        const std::vector<std::string>& l = conditions[c].lines;
        std::vector<double> lagMs, worst, sent, wall, p50, p99, mx;
        for (size_t i = 0; i < l.size(); ++i) {
            lagMs.push_back(field(l[i], "lag_ms"));
            worst.push_back(field(l[i], "max_send_lag_ms"));
            const double s = field(l[i], "sent"), w = field(l[i], "wall_ms");
            sent.push_back(s);
            wall.push_back(w);
            p50.push_back(field(l[i], "ui_wake_late_ms_p50"));
            p99.push_back(field(l[i], "p99"));
            mx.push_back(field(l[i], "max"));
        }
        std::vector<double> perSecond;
        for (size_t i = 0; i < sent.size(); ++i) if (wall[i] > 0) perSecond.push_back(sent[i] / wall[i] * 1000.0);
        char row[320];
        if (l.empty()) std::snprintf(row, sizeof(row), "%-35s  0     no result (the run failed or was skipped)\r\n", conditions[c].label().c_str());
        else std::snprintf(row, sizeof(row), "%-35s  %-4u  %-17.1f  %-23.1f  %-10.0f  %26.1f  %5.0f  %7.0f\r\n", conditions[c].label().c_str(), static_cast<unsigned>(l.size()),
            median(lagMs) / 1000.0, median(worst) / 1000.0, median(perSecond), median(p50), median(p99), median(mx));
        out += row;
        const int ci = conditions[c].cpus == 1 ? 0 : 1, oi = conditions[c].option ? 1 : 0;
        longest[ci][oi] = median(mx);
        lag[ci][oi] = median(lagMs) / 1000.0;
        meps[ci][oi] = median(perSecond);
    }
    out += "\r\n";
    for (int ci = 0; ci < 2; ++ci) {
        if (longest[ci][0] < 0 || longest[ci][1] < 0) continue;
        out += std::string(ci == 0 ? "On 1 logical processor" : "On 2 logical processors") + ": the longest wait of the competing thread was " + number(longest[ci][0], 0) +
            " ms with the option off and " + number(longest[ci][1], 0) + " ms with it on; playback ended " + number(lag[ci][0], 1) + " s behind with it off and " +
            number(lag[ci][1], 1) + " s behind with it on (" + number(meps[ci][0], 0) + " and " + number(meps[ci][1], 0) + " messages per second).\r\n";
    }
    out += "\r\nruns finished: " + std::to_string(done) + ", failed: " + std::to_string(failed) + ", skipped for time: " + std::to_string(skipped) +
        ", total time " + number(now_seconds() - started, 1) + " s\r\n";
    out += "RESULT few_processor rounds=" + std::to_string(rounds) + " done=" + std::to_string(done) + " failed=" + std::to_string(failed) +
        " skipped=" + std::to_string(skipped) + " seconds=" + number(now_seconds() - started, 1) + "\r\n";
    out += "\r\nRaw lines of every run:\r\n";
    for (size_t i = 0; i < lines.size(); ++i) out += lines[i] + "\r\n";
    tools::write_file(outFile, out);
    std::string console;   // the console adds the carriage returns itself
    for (size_t i = 0; i < out.size(); ++i) if (out[i] != '\r') console += out[i];
    tools::write_stdout(console);
    return failed == 0 && done > 0 ? 0 : 1;
}

} // namespace few_cpu_auto
