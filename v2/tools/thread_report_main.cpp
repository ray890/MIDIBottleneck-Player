// midibn-thread-report: which threads does a MIDI output path create inside our process, and at what priority?
//
// Background: on a Windows XP PC with Hyper-Threading a thread inside MIDIBottleneck's process ran at "Time Critical"
// priority; v2's own threads are Above Normal, so that thread probably belongs to the synthesizer driver loaded into the
// process. This tool shows it: it lists every thread of its own process (priority, module of the start address), opens one
// MIDI output through the chosen path, sends only silent messages, waits, lists again (new threads marked NEW), closes and
// lists once more.
//
// SOUND SAFETY: the only messages sent are Control Change 121 ("reset all controllers") and 123 ("all notes off") on each
// of the 16 channels. No Note On is ever sent. Windows' own driver (Microsoft GS Wavetable Synth) and the MIDI Mapper are
// never chosen by default.
//
//   midibn-thread-report --path winmm|kdmapi|direct|none [--device TEXT] [--out FILE] [--wait-ms N] [--list]
//     --path      winmm   standard WinMM (midiOutOpen)
//                 kdmapi  OmniMIDI's KDMAPI entry points (loaded like the player does)
//                 direct  the driver's own modMessage entry (the player's "Direct WinMM driver access")
//                 none    opens nothing (checks the tool itself)
//     --device    open the WinMM output whose name contains TEXT (default: the first one that is not made by Microsoft;
//                 kdmapi ignores it)
//     --out       also write the report to this file
//     --wait-ms   how long to wait after opening and sending (default 2000)
//     --list      only list the WinMM outputs and the threads, open nothing
// Exit code: 0 report written (also when there is no non-Microsoft output: the report says so), 2 open failed, 1 usage.
#include "kdmapi_midi_output.h"
#include "midibn/midi_output.h"
#include "thread_report.h"
#include "tool_support.h"
#include "win32_platform.h"
#include "winmm_direct_output.h"
#include "winmm_midi_output.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace midibn;

namespace {

std::string g_report;

void say(const std::string& text)
{
    g_report += text + "\n";
    tools::write_stdout(text + "\n");
}

std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

bool contains(const std::string& text, const std::string& part) { return lower(text).find(lower(part)) != std::string::npos; }

void list_phase(const char* title, std::set<uint32_t>* known)
{
    std::vector<win32::ThreadInfo> threads = win32::snapshot_threads();
    say(std::string("--- ") + title);
    std::string table = win32::format_thread_table(threads, known != nullptr && !known->empty() ? known : nullptr, GetCurrentThreadId());
    g_report += table;
    tools::write_stdout(table);
    if (known != nullptr) {
        std::set<uint32_t> now = win32::thread_ids(threads);
        known->insert(now.begin(), now.end());
    }
}

// Silent messages only: CC121 and CC123 on every channel.
int send_silent(platform::MidiOutput& out)
{
    int failed = 0;
    for (uint32_t channel = 0; channel < 16; ++channel) {
        if (!out.send_short(0xB0u | channel | (121u << 8)).ok()) ++failed;
        if (!out.send_short(0xB0u | channel | (123u << 8)).ok()) ++failed;
    }
    return failed;
}

} // namespace

static int report_main(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    std::string path = "none", device, outFile;
    int waitMs = 2000;
    bool listOnly = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--path" && i + 1 < args.size()) path = args[++i];
        else if (args[i] == "--device" && i + 1 < args.size()) device = args[++i];
        else if (args[i] == "--out" && i + 1 < args.size()) outFile = args[++i];
        else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::atoi(args[++i].c_str());
        else if (args[i] == "--list") listOnly = true;
        else { tools::write_stdout("usage: midibn-thread-report --path winmm|kdmapi|direct|none [--device TEXT] [--out FILE] [--wait-ms N] [--list]\n"); return 1; }
    }
    if (path != "winmm" && path != "kdmapi" && path != "direct" && path != "none") {
        tools::write_stdout("--path must be winmm, kdmapi, direct or none\n");
        return 1;
    }
    if (waitMs < 0) waitMs = 0;

    // One copy at a time with the other device tools (two programs in one synthesizer driver at once is what made
    // drivers stall in earlier tests).
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Global\\MIDIBottleneck-direct-check");
    if (single != nullptr && path != "none" && !listOnly && WaitForSingleObject(single, 120000) == WAIT_TIMEOUT) {
        tools::write_stdout("Another device check is still running on this PC; this one stopped without touching any MIDI driver.\n");
        return 4;
    }
    say("midibn-thread-report 1  path=" + path + (sizeof(void*) == 8 ? "  x64" : "  x86"));
    std::vector<platform::OutputDeviceInfo> devices = win32::WinMmMidiOutput::enumerate_devices();
    say("WinMM outputs: " + std::to_string(devices.size()));
    for (size_t i = 0; i < devices.size(); ++i) say("  [" + std::to_string(devices[i].device_id) + "] " + devices[i].name);
    int code = 0;

    std::set<uint32_t> known;
    list_phase("1. before opening anything", &known);
    if (listOnly || path == "none") {
        if (path == "none" && !listOnly) say("path none: nothing opened.");
        goto finish;
    }
    {
        int32_t chosen = -1;
        if (path != "kdmapi") {
            for (size_t i = 0; i < devices.size() && chosen < 0; ++i) {
                const std::string& name = devices[i].name;
                if (!device.empty()) { if (contains(name, device)) chosen = static_cast<int32_t>(devices[i].device_id); }
                else if (!contains(name, "microsoft") && !contains(name, "mapper")) chosen = static_cast<int32_t>(devices[i].device_id);
            }
            if (chosen < 0) {
                say(device.empty() ? "No MIDI output that is not made by Microsoft was found: nothing opened (this is not an error)."
                                   : "No MIDI output whose name contains \"" + device + "\": nothing opened.");
                goto finish;
            }
            say("output chosen: [" + std::to_string(chosen) + "]");
        }
        std::unique_ptr<platform::MidiOutput> output;
        if (path == "winmm") output.reset(new win32::WinMmMidiOutput());
        else if (path == "direct") output.reset(new win32::WinMmDirectOutput());
        else output.reset(new win32::KdmApiMidiOutput());
        say(std::string("backend: ") + output->backend_name());
        platform::OutputStatus opened = output->open(chosen < 0 ? 0u : static_cast<uint32_t>(chosen));
        if (!opened.ok()) {
            say("open failed: " + opened.message);
            code = 2;
            goto finish;
        }
        int failed = send_silent(*output);
        say("sent 32 silent controller messages (CC121 and CC123 on 16 channels); failed sends: " + std::to_string(failed));
        Sleep(static_cast<DWORD>(waitMs));
        list_phase("2. open, silent messages sent, waited", &known);
        platform::OutputStatus closed = output->close();
        say(closed.ok() ? "closed." : "close reported: " + closed.message);
        Sleep(500);
        list_phase("3. after closing (threads still alive here were left behind by the driver)", &known);
    }
finish:
    if (!outFile.empty() && !tools::write_file(outFile, g_report)) {
        tools::write_stdout("could not write " + outFile + "\n");
        return 1;
    }
    return code;
}

// The report is complete when report_main returns. The process then ends at once without running the exit code of the
// loaded drivers (a driver thread must not be able to keep the window open once the answer is written).
int wmain(int argc, wchar_t** argv)
{
    int code = report_main(argc, argv);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
