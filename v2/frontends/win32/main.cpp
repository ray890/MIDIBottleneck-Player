// MIDIBottleneck Player v2 - Windows frontend (plain Win32).
//
// Command line: [file.mid] plus test-only switches (a normal launch ignores them):
//   --auto-close-ms N                  close through a posted WM_CLOSE after N ms
//   --open FILE                        load FILE at start
//   --play                             after loading, start playback on output None (never opens a device)
//   --rate N                           with --play: Rate model index (0 None ... 7 Output rate gate)
//   --scale P  --width W               Application scale 50..200, initial client width
//   --borderless 0|1|2                 start borderless (0 classic strip, 1 integrated header, 2 auto-hide overlay)
//   --screenshot FILE [--screenshot-delay-ms N]   write the window to a BMP, then close
//   --report FILE                      write UI-thread and process CPU use at exit (and the live-update timing)
//   --thread-report FILE [--thread-report-delay-ms N]   write the per-thread table (priority, start module) N ms (default 4000) after playback starts
//   --affinity HEX                     test: run on these logical processors only (bit mask), e.g. 1 or 3
//   --force-one-core   test: the system menu offers "Let other programs run while playback is behind" as on a one-physical-core PC
//   --few-cpu normal|yield=MS|nap=MS|normal,yield=MS   test, default off: with at most 2 usable logical processors, v2 threads at Normal priority and/or the playback loop yields every MS ms while behind
//   --settings FILE                    keep the remembered settings in FILE instead of the user's own settings file
//   --no-settings                      start from the defaults and save nothing (every automated launch uses this; a launch with
//                                      --selftest, --screenshot, --popup-shot, --thread-report, --report or --auto-close-ms
//                                      does the same on its own unless --settings names a file)
//   --refresh-ms N                   use the old Windows timer at N ms for the live updates (comparison only)
//   --pace auto|display|timer          live-update pacing (default auto: monitor refresh via DwmFlush, else timer)
//   --pace-hz N / --pace-max-hz N      timer-mode rate (default 60) / cap of the display mode
//   --adaptive-display off             never lower the display cost (default: on)
//   --display-tier N                   hold adaptive tier N, no adapting (cost measurements)
//   --display-cost-us N[,S]            pretend each update costs N us (+ S us for all the statistics): busy-waits
//   --display-budget-pct P             another budget (share of the interface thread); --display-pretend-behind MS
//   --native-window-monitor-fixture / --native-process-monitor-fixture   release-monitor proofs
#include "main_window.h"
#include "win32_timing.h"
#include "settings_file.h"

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const wchar_t kFixtureClass[] = L"MIDIBottleneckV2DetectorFixture";

LRESULT CALLBACK fixture_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_TIMER) {
        KillTimer(window, 1);
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Test-only switch: shows a window with a fixed, identifiable title for the release monitor.
int run_window_fixture(HINSTANCE instance, int show, int closeMs)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = fixture_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kFixtureClass;
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, kFixtureClass, L"Deliberately identifiable detector proof window", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 520, 330, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);
    UpdateWindow(window);
    SetTimer(window, 1, static_cast<UINT>(closeMs > 0 ? closeMs : 2000), nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

// Test-only switch: starts a short-lived copy of this program and waits for it.
int run_process_monitor_fixture()
{
    wchar_t self[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 2;
    std::wstring commandLine = L"\"" + std::wstring(self) + L"\" --auto-close-ms 700";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info = {};
    if (!CreateProcessW(self, &commandLine[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return 3;
    WaitForSingleObject(info.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

double filetime_ms(const FILETIME& t)
{
    ULARGE_INTEGER v;
    v.LowPart = t.dwLowDateTime;
    v.HighPart = t.dwHighDateTime;
    return v.QuadPart / 10000.0;
}

void write_report(const std::wstring& path, DWORD startTick, const std::string& notes)
{
    FILETIME creation, exit, kernel, user, pkernel, puser;
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &pkernel, &puser);
    char head[160];
    int headLength = std::snprintf(head, sizeof(head), "wall_ms=%lu\nui_thread_cpu_ms=%.1f\nprocess_cpu_ms=%.1f\n",
        static_cast<unsigned long>(GetTickCount() - startTick), filetime_ms(kernel) + filetime_ms(user), filetime_ms(pkernel) + filetime_ms(puser));
    const std::string text = std::string(head, static_cast<size_t>(headLength)) + notes;
    const int length = static_cast<int>(text.size());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, text.data(), static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    // Start-up timing for --report (DD-055): raw performance-counter readings, comparable with a reading the
    // launching script takes just before it starts the process (the counter is system-wide).
    LARGE_INTEGER entryCounter = {}, createdCounter = {}, idleCounter = {};
    QueryPerformanceCounter(&entryCounter);
    midibn::ui::startup_mark("entry");
    midibn::ui::TestOptions options;
    bool windowFixture = false;
    std::wstring reportFile;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring argument = argv[i];
        bool hasValue = i + 1 < argc;
        if (argument == L"--auto-close-ms" && hasValue) options.auto_close_ms = _wtoi(argv[++i]);
        else if (argument == L"--open" && hasValue) options.open_file = argv[++i];
        else if (argument == L"--play") options.play = true;
        else if (argument == L"--force-large-warning") options.force_large_warning = true;
        else if (argument == L"--force-large-warning-32") { options.force_large_warning = true; options.force_large_risk = true; }
        else if (argument == L"--rate" && hasValue) options.select_rate = _wtoi(argv[++i]);
        else if (argument == L"--scale" && hasValue) options.scale_percent = _wtoi(argv[++i]);
        else if (argument == L"--width" && hasValue) options.client_width = _wtoi(argv[++i]);
        else if (argument == L"--position" && hasValue) {
            std::wstring xy = argv[++i];
            size_t comma = xy.find(L',');
            if (comma != std::wstring::npos) {
                options.has_position = true;
                options.position_x = _wtoi(xy.substr(0, comma).c_str());
                options.position_y = _wtoi(xy.substr(comma + 1).c_str());
            }
        }
        else if (argument == L"--refresh-ms" && hasValue) options.refresh_ms = _wtoi(argv[++i]);
        else if (argument == L"--pace" && hasValue) {
            std::wstring mode = argv[++i];
            options.pace_mode = mode == L"display" ? 1 : mode == L"timer" ? 2 : 0;
        }
        else if (argument == L"--pace-max-hz" && hasValue) options.pace_max_hz = _wtoi(argv[++i]);
        else if (argument == L"--pace-hz" && hasValue) options.pace_hz = _wtoi(argv[++i]);
        else if (argument == L"--adaptive-display" && hasValue) options.adaptive_display = std::wstring(argv[++i]) != L"off";
        else if (argument == L"--display-tier" && hasValue) options.display_fixed_tier = _wtoi(argv[++i]);
        else if (argument == L"--display-cost-us" && hasValue) {
            std::wstring spec = argv[++i];
            options.display_cost_us = _wtoi(spec.c_str());
            size_t comma = spec.find(L',');
            if (comma != std::wstring::npos) options.display_stats_cost_us = _wtoi(spec.c_str() + comma + 1);
        }
        else if (argument == L"--display-budget-pct" && hasValue) options.display_budget_permille = static_cast<int>(_wtof(argv[++i]) * 10.0);
        else if (argument == L"--display-pretend-behind" && hasValue) {
            std::wstring spec = argv[++i];
            options.display_behind_after_ms = _wtoi(spec.c_str());
            size_t comma = spec.find(L',');
            if (comma != std::wstring::npos) options.display_behind_until_ms = _wtoi(spec.c_str() + comma + 1);
        }
        else if (argument == L"--display-cost-for-ms" && hasValue) options.display_cost_for_ms = _wtoi(argv[++i]);
        else if (argument == L"--display-paint-all") options.display_paint_all = true;
        else if (argument == L"--popup-shot" && hasValue) options.popup_file = argv[++i];
        else if (argument == L"--borderless" && hasValue) options.borderless_style = _wtoi(argv[++i]);
        else if (argument == L"--screenshot" && hasValue) options.screenshot_file = argv[++i];
        else if (argument == L"--screenshot-delay-ms" && hasValue) options.screenshot_delay_ms = _wtoi(argv[++i]);
        else if (argument == L"--report" && hasValue) reportFile = argv[++i];
        else if (argument == L"--thread-report" && hasValue) options.thread_report_file = argv[++i];
        else if (argument == L"--thread-report-delay-ms" && hasValue) options.thread_report_delay_ms = _wtoi(argv[++i]);
        else if (argument == L"--affinity" && hasValue) {
            DWORD_PTR mask = static_cast<DWORD_PTR>(wcstoull(argv[++i], nullptr, 16));
            if (mask != 0) SetProcessAffinityMask(GetCurrentProcess(), mask);
        }
        else if (argument == L"--direct-check" && hasValue) {   // test: stand-in for the Direct WinMM check (usable | own-driver); default: the real check
            std::wstring mode = argv[++i];
            options.direct_check_standin = mode == L"own-driver" ? 1 : mode == L"usable" ? 2 : mode == L"own-driver-late" ? 3 : 0;
        }
        else if (argument == L"--force-one-core") midibn::win32::set_force_one_physical_core(true);   // test: act as a one-core PC (shows the yield menu item)
        else if (argument == L"--few-cpu" && hasValue) {
            std::wstring spec = argv[++i];
            if (spec.find(L"normal") != std::wstring::npos) options.few_cpu_normal = 1;
            size_t y = spec.find(L"yield=");
            if (y != std::wstring::npos) options.few_cpu_yield_ms = _wtoi(spec.c_str() + y + 6);
            size_t nap = spec.find(L"nap=");
            if (nap != std::wstring::npos) { options.few_cpu_yield_ms = _wtoi(spec.c_str() + nap + 4); midibn::win32::set_few_cpu_nap(true); }
        }
        else if (argument == L"--settings" && hasValue) options.settings_path = argv[++i];
        else if (argument == L"--no-settings") options.no_settings = true;
        else if (argument == L"--settings-change") options.settings_change = true;
        else if (argument == L"--settings-appdata" && hasValue) { midibn::win32::set_appdata_override_for_tests(argv[++i]); options.settings_default = true; }
        else if (argument == L"--selftest" && hasValue) { options.selftest = true; options.result_file = argv[++i]; }
        else if (argument == L"--native-window-monitor-fixture") windowFixture = true;
        else if (argument == L"--native-process-monitor-fixture") {
            LocalFree(argv);
            return run_process_monitor_fixture();
        }
        else if (options.open_file.empty() && argument.compare(0, 2, L"--") != 0) options.open_file = argument;
    }
    if (argv) LocalFree(argv);
    options.record_refresh = !reportFile.empty();
    midibn::win32::set_few_cpu_policy(options.few_cpu_normal != 0, options.few_cpu_yield_ms);   // default off

    if (windowFixture) return run_window_fixture(instance, show, options.auto_close_ms);
    midibn::ui::prefetch_output_devices();   // DD-057: the device list is built while the window is created

    DWORD startTick = GetTickCount();
    midibn::ui::MainWindow::register_class(instance);
    midibn::ui::startup_mark("classes");
    midibn::ui::MainWindow window;
    midibn::ui::startup_mark("object");
    if (options.selftest) {
        midibn::ui::TestOptions quiet = options;
        quiet.auto_close_ms = 0;
        if (!window.create(instance, SW_SHOWNOACTIVATE, quiet)) return 1;
        int code = window.run_selftest();
        DestroyWindow(window.hwnd());
        return code;
    }
    if (!window.create(instance, show, options)) return 1;
    if (!reportFile.empty()) QueryPerformanceCounter(&createdCounter);

    MSG message;
    double busy[3] = { 0, 0, 0 };
    long count[3] = { 0, 0, 0 };
    for (;;) {
        // First idle: the window is shown and painted and the message queue is empty (only with --report).
        if (!reportFile.empty() && idleCounter.QuadPart == 0 && !PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE))
            QueryPerformanceCounter(&idleCounter);
        if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
        if (window.pre_translate(&message)) continue;
        LARGE_INTEGER t0 = {}, t1;
        QueryPerformanceCounter(&t0);
        if (!window.hwnd() || !IsDialogMessageW(window.hwnd(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        QueryPerformanceCounter(&t1);
        window.note_dispatch(message, t1.QuadPart - t0.QuadPart);   // adaptive display rate: painting is metered
        if (!reportFile.empty()) {
            int slot = message.message == WM_TIMER ? 0 : message.message == WM_PAINT ? 1 : 2;
            busy[slot] += static_cast<double>(t1.QuadPart - t0.QuadPart);
            ++count[slot];
        }
    }
    if (!reportFile.empty()) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        char extra[640];
        int used = std::snprintf(extra, sizeof(extra), "timer_ms=%.1f (%ld)\npaint_ms=%.1f (%ld)\nother_ms=%.1f (%ld)\n", busy[0] * 1000.0 / f.QuadPart,
            count[0], busy[1] * 1000.0 / f.QuadPart, count[1], busy[2] * 1000.0 / f.QuadPart, count[2]);
        // Start-up (DD-055): counter readings at entry to wWinMain, after the window was created and shown, and at the
        // first idle moment, with the counter frequency; the derived milliseconds are entry -> created and entry -> idle.
        std::snprintf(extra + used, sizeof(extra) - static_cast<size_t>(used),
            "qpc_frequency=%lld\nqpc_entry=%lld\nqpc_created=%lld\nqpc_idle=%lld\nstartup_create_ms=%.2f\nstartup_idle_ms=%.2f\n",
            f.QuadPart, entryCounter.QuadPart, createdCounter.QuadPart, idleCounter.QuadPart,
            (createdCounter.QuadPart - entryCounter.QuadPart) * 1000.0 / f.QuadPart,
            idleCounter.QuadPart ? (idleCounter.QuadPart - entryCounter.QuadPart) * 1000.0 / f.QuadPart : -1.0);
        write_report(reportFile, startTick, window.run_report() + extra + midibn::ui::startup_marks_text());
    }
    return static_cast<int>(message.wParam);
}
