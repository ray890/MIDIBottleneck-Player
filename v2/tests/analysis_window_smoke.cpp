// Smoke test of the Win32 Analysis window: a PlayerController on a recording output loads a committed
// fixture, an Analysis window opens on it, the test pumps messages until the calculation finishes, clicks
// and double-clicks the graph (the double-click seeks the player), presses Space, checks the system-menu
// commands, and closes the window. Screenshots are rendered with WM_PRINT (never from the screen) into the
// output directory.
//
// Usage: midibn-analysis-window-smoke [fixture-directory] [output-directory]
// (defaults v2\harness\fixtures and v2\out\analysis-smoke). Exit code 0 when every check passes.
// The window is shown on screen: run it on a hidden desktop, or set MIDIBN_SMOKE_HOST_AT=X,Y to a point on the
// monitor it may use (the window opens beside that point's monitor's stand-in main window, DD-098).
#include "analysis_window.h"
#include "ui_common.h"
#include "workload_graph.h"

#include "midibn/app/analysis_cache.h"
#include "midibn/app/player_controller.h"
#include "midibn/midi_output.h"
#include "midibn/null_output.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <commctrl.h>
#include <windowsx.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::string g_fixtures = "v2\\harness\\fixtures";
std::string g_out = "v2\\out\\analysis-smoke";

LRESULT CALLBACK host_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

struct World {
    win32::Win32FileSystem files;
    std::shared_ptr<platform::RecordingMidiOutput> winmm = std::make_shared<platform::RecordingMidiOutput>();
    std::shared_ptr<platform::RecordingMidiOutput> kdmapi = std::make_shared<platform::RecordingMidiOutput>();
    std::unique_ptr<app::PlayerController> controller;
    ui::UiContext ctx;
    HWND main_window = nullptr;
    ui::CompanionHost host;
    std::vector<int64_t> seeks;
    int channel_requests = 0;
    int transport_toggles = 0;

    World()
    {
        app::PlayerEnvironment env;
        env.services = std::make_shared<win32::Win32EngineServices>();
        env.files = &files;
        env.list_devices = [] {
            std::vector<platform::OutputDeviceInfo> devices(1);
            devices[0].device_id = 0;
            devices[0].name = "Recording device";
            return devices;
        };
        env.create_winmm = [this] { return std::shared_ptr<platform::MidiOutput>(winmm); };
        env.create_kdmapi = [this] { return std::shared_ptr<platform::MidiOutput>(kdmapi); };
        env.pointer_size = static_cast<int>(sizeof(void*));
        env.wake_ui = [] {};
        controller.reset(new app::PlayerController(env));
        controller->select_output(1);   // the recording device: no real device is opened
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = host_proc;
        wc.lpszClassName = L"AnalysisSmokeHost";
        RegisterClassExW(&wc);
        // The (invisible) stand-in main window sits at 3900,1150 (Internal Display), or at the point in MIDIBN_SMOKE_HOST_AT ("X,Y"): the
        // Analysis window opens beside it on its monitor (DD-098), so this chooses the monitor the window shows on.
        int hostX = 3900, hostY = 1150;   // inside Internal Display, never the primary 4K monitor
        char at[64] = {};
        if (GetEnvironmentVariableA("MIDIBN_SMOKE_HOST_AT", at, sizeof at) > 0) {
            char* rest = nullptr;
            hostX = static_cast<int>(std::strtol(at, &rest, 10));
            if (rest && *rest == ',') hostY = static_cast<int>(std::strtol(rest + 1, nullptr, 10));
        }
        main_window = CreateWindowExW(0, wc.lpszClassName, L"host", WS_OVERLAPPEDWINDOW, hostX, hostY, 700, 500, nullptr, nullptr, wc.hInstance,
            nullptr);
        host.controller = controller.get();
        host.ctx = &ctx;
        host.main_window = main_window;
        host.analysis_seek = [this](int64_t t) {
            seeks.push_back(t);
            controller->analysis_seek(t);
        };
        host.toggle_transport = [this] {
            ++transport_toggles;
            controller->play_clicked();
        };
        host.open_channels = [this] { ++channel_requests; };
        host.request_refresh = [] {};
    }
    ~World()
    {
        controller->shutdown();
        if (main_window) DestroyWindow(main_window);
    }
};

void pump(World& w, ui::CompanionWindow* window, int milliseconds, bool refresh = true)
{
    int64_t start = win32::performance_counter();
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        w.controller->pump();
        if (refresh) {
            w.controller->refresh();
            if (window && IsWindow(window->hwnd())) window->on_refresh();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    } while (win32::milliseconds_since(start) < milliseconds);
}

bool pump_until(World& w, ui::AnalysisCompanion* window, int timeoutMs, bool (*done)(ui::AnalysisCompanion*))
{
    int64_t start = win32::performance_counter();
    while (win32::milliseconds_since(start) < timeoutMs) {
        pump(w, window, 20);
        if (done(window)) return true;
    }
    return false;
}

bool completed(ui::AnalysisCompanion* window) { return window->model().has_completed_analysis() && !window->model().calculation_pending(); }

std::wstring out_path(const std::string& name) { return ui::W(g_out + "\\" + name); }

void shot(ui::AnalysisCompanion* window, const std::string& name)
{
    check(ui::save_window_bitmap(window->hwnd(), out_path(name)), "screenshot " + name);
}

void click(HWND graph, int x, int y, bool doubleClick = false)
{
    LPARAM position = MAKELPARAM(x, y);
    SendMessageW(graph, WM_MOUSEMOVE, 0, position);
    SendMessageW(graph, WM_LBUTTONDOWN, MK_LBUTTON, position);
    SendMessageW(graph, WM_LBUTTONUP, 0, position);
    if (doubleClick) {
        SendMessageW(graph, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
        SendMessageW(graph, WM_LBUTTONUP, 0, position);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1) g_fixtures = argv[1];
    if (argc > 2) g_out = argv[2];
    CreateDirectoryA("v2\\out", nullptr);
    CreateDirectoryA(g_out.c_str(), nullptr);
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);

    {
        World w;
        w.controller->open_file(g_fixtures + "\\models-notes.mid");
        for (int i = 0; i < 2000 && !w.controller->has_song(); ++i) {
            w.controller->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(w.controller->has_song(), "the fixture loads");

        // ---- opening: calculating, then finished
        std::unique_ptr<ui::CompanionWindow> created = ui::create_analysis_window(w.host);
        check(created != nullptr, "the Analysis window opens");
        if (!created) return 1;
        ui::AnalysisCompanion* window = static_cast<ui::AnalysisCompanion*>(created.get());
        check(IsWindow(window->hwnd()) != FALSE, "the window exists");
        {
            // DD-098: on the main window's monitor, wholly inside its work area
            RECT wr = {}, work = ui::monitor_work_area(w.main_window);
            GetWindowRect(window->hwnd(), &wr);
            check(MonitorFromWindow(window->hwnd(), MONITOR_DEFAULTTONEAREST) == MonitorFromWindow(w.main_window, MONITOR_DEFAULTTONEAREST),
                "the window opens on the main window's monitor");
            check(wr.left >= work.left && wr.top >= work.top && wr.right <= work.right && wr.bottom <= work.bottom,
                "the window lies inside the work area of the main window's monitor");
        }
        wchar_t title[256] = {};
        GetWindowTextW(window->hwnd(), title, 256);
        check(std::wstring(title) == L"MIDIBottleneck Player \x2014 Analysis \x2014 models-notes.mid", "title names the file");
        RECT client;
        GetClientRect(window->hwnd(), &client);
        check(client.right >= w.ctx.px(1000) && client.bottom >= w.ctx.px(600), "default client size is about 1080 x 660");
        check(window->using_rich_edit(), "the report is a RichEdit control");
        check(pump_until(w, window, 5000, completed), "the analysis finishes");
        check(!window->model().analysis_busy(), "no longer busy");
        wchar_t text[64] = {};
        GetWindowTextW(window->summary_window(), text, 64);
        check(std::wstring(text).compare(0, 9, L"MIDI FILE") == 0, "the report pane shows the report");
        shot(window, "finished.bmp");

        // ---- the Resolution combo and the status area
        HWND resolution = GetDlgItem(window->hwnd(), 1005);
        check(resolution != nullptr && SendMessageW(resolution, CB_GETCOUNT, 0, 0) == 9, "Resolution lists Auto, seven fixed choices and Custom...");

        // ---- a click pins, a double-click seeks
        HWND graph = window->graph_window();
        pump(w, window, 100);
        app::GraphRect area = window->model().graph().graph_area();
        click(graph, area.x + area.width / 3, area.y + area.height / 2);
        check(window->model().graph().has_pinned(), "a click pins a time");
        check(IsWindowEnabled(window->seek_button()) != FALSE, "Seek to pin becomes enabled");
        wchar_t inspection[256] = {};
        GetWindowTextW(window->inspection_window(), inspection, 256);
        check(std::wstring(inspection).compare(0, 7, L"Pinned:") == 0, "the inspection box shows the pin");
        SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1014, BN_CLICKED), reinterpret_cast<LPARAM>(window->seek_button()));
        check(w.seeks.size() == 1, "Seek to pin seeks");
        int64_t pinned = window->model().graph().pinned_time();
        check(!w.seeks.empty() && w.seeks[0] == pinned, "it seeks to the pinned time");
        click(graph, area.x + area.width * 2 / 3, area.y + area.height / 2, true);
        check(w.seeks.size() == 2, "a double-click seeks");
        pump(w, window, 100);
        check(w.controller->selected_position() > 0 || w.controller->engine().state() != engine::PlaybackState::Stopped, "the player moved to the new position");
        shot(window, "pinned.bmp");

        // ---- hover shows the tip
        SendMessageW(graph, WM_MOUSEMOVE, 0, MAKELPARAM(area.x + area.width / 2, area.y + 40));
        pump(w, window, 120);
        shot(window, "hover.bmp");

        // ---- a queue projection with a finite limit and an age limit
        w.controller->select_rate_choice(app::RateModelChoice::ProcessingTime);
        w.controller->set_processing_value(2500);
        w.controller->set_queue_limit_checked(true);
        w.controller->set_queue_value(40);
        w.controller->stop_clicked();
        pump(w, window, 100);
        check(pump_until(w, window, 5000, [](ui::AnalysisCompanion* a) {
            return a->model().has_completed_analysis() && !a->model().calculation_pending() && !a->model().analysis_busy() &&
                a->model().summary_text().find("Overflow policy") != std::string::npos;
        }), "a queue-limit setting refreshes the analysis");
        shot(window, "queue.bmp");

        // ---- zoomed
        SendMessageW(graph, WM_MOUSEWHEEL, MAKEWPARAM(0, 240), MAKELPARAM(0, 0));   // outside: nothing happens
        POINT inside = { area.x + area.width / 2, area.y + area.height / 2 };
        ClientToScreen(graph, &inside);
        SendMessageW(graph, WM_MOUSEWHEEL, MAKEWPARAM(0, 480), MAKELPARAM(inside.x, inside.y));
        pump(w, window, 450);
        check(window->model().graph().view_end() - window->model().graph().view_start() < window->model().analysis().duration_us(), "the wheel zooms");
        shot(window, "zoomed.bmp");
        SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1001, BN_CLICKED), 0);
        check(window->model().graph().view_start() == 0, "Reset zoom shows the whole file");

        // ---- Playback statistics overlay while playing
        SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1006, BN_CLICKED), 0);   // toggles nothing visible until checked
        SendMessageW(GetDlgItem(window->hwnd(), 1006), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1006, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window->hwnd(), 1006)));
        check(window->model().graph().playback_statistics_visible(), "Playback statistics can be switched on");
        w.controller->play_clicked();
        pump(w, window, 400);
        check(w.controller->engine().state() == engine::PlaybackState::Playing, "playing");
        check(window->model().graph().has_playback_positions(), "the playback markers are updated");
        shot(window, "playing.bmp");

        // ---- Space toggles the transport (v1 TryHandleAnalysisSpace)
        SetActiveWindow(window->hwnd());
        SetFocus(graph);
        PostMessageW(graph, WM_KEYDOWN, VK_SPACE, 0);
        PostMessageW(graph, WM_KEYUP, VK_SPACE, 0);
        pump(w, window, 200);
        check(w.transport_toggles == 1, "Space asks the main window to toggle the transport");
        check(w.controller->engine().state() == engine::PlaybackState::Paused, "and the player paused");
        w.controller->stop_clicked();

        // ---- Tab moves between the controls (the thread's message loop only knows the main window)
        SetFocus(GetDlgItem(window->hwnd(), 1001));
        PostMessageW(GetDlgItem(window->hwnd(), 1001), WM_KEYDOWN, VK_TAB, 0);
        PostMessageW(GetDlgItem(window->hwnd(), 1001), WM_KEYUP, VK_TAB, 0);
        pump(w, window, 100);
        check(GetFocus() == GetDlgItem(window->hwnd(), 1003), "Tab moves from Reset zoom to the Follow combo");

        // ---- system menu commands
        SendMessageW(window->hwnd(), WM_SYSCOMMAND, 0x1F20, 0);
        check(window->model().show_event_counts(), "Show event counts");
        shot(window, "event-counts.bmp");
        SendMessageW(window->hwnd(), WM_SYSCOMMAND, 0x1F20, 0);
        SendMessageW(window->hwnd(), WM_SYSCOMMAND, 0x1F30, 0);
        check(window->always_on_top(), "Always on top");
        check((GetWindowLongW(window->hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "the window is top-most");
        SendMessageW(window->hwnd(), WM_SYSCOMMAND, 0x1F30, 0);
        SendMessageW(window->hwnd(), WM_SYSCOMMAND, 0x1F40, 0);
        check(window->model().project_forced_overrides(), "Project forced overrides");
        check(pump_until(w, window, 5000, completed), "projection with overrides finishes");
        SendMessageW(window->hwnd(), WM_SYSCOMMAND, 0x1F40, 0);
        check(pump_until(w, window, 5000, completed), "and finishes again when switched off");

        // ---- a gate projection
        w.controller->select_rate_choice(app::RateModelChoice::PerNoteIntervalGate);
        check(pump_until(w, window, 5000, [](ui::AnalysisCompanion* a) {
            return a->model().has_completed_analysis() && !a->model().calculation_pending() &&
                a->model().summary_text().find("PER-NOTE PROJECTION") != std::string::npos;
        }), "a gate setting refreshes the analysis");
        shot(window, "gate.bmp");

        // ---- calculating: hold the song's base-workload cache so a new resolution cannot finish
        {
            std::shared_ptr<app::SongAnalysisCaches> caches = app::song_analysis_caches(window->model().song());
            check(caches->workloads.try_enter(1), "hold the base-workload cache");
            SendMessageW(resolution, CB_SETCURSEL, 3, 0);   // 50 ms
            SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(resolution));
            pump(w, window, 400);
            check(window->model().analysis_busy() && !window->model().busy_view().status_visible, "busy but the indicator waits one second");
            check(!IsWindowVisible(GetDlgItem(window->hwnd(), 1010)), "Cancel is hidden at first");
            pump(w, window, 1000);
            check(window->model().busy_view().status_visible && IsWindowVisible(GetDlgItem(window->hwnd(), 1008)), "after one second the status shows");
            check(IsWindowVisible(GetDlgItem(window->hwnd(), 1009)) && IsWindowVisible(GetDlgItem(window->hwnd(), 1010)), "progress bar and Cancel show");
            shot(window, "calculating.bmp");
            SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1010, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window->hwnd(), 1010)));
            check(!window->model().analysis_busy() && !IsWindowVisible(GetDlgItem(window->hwnd(), 1010)), "Cancel hides the status controls");
            caches->workloads.leave();
            pump(w, window, 300);
            SendMessageW(resolution, CB_SETCURSEL, 0, 0);   // back to Auto
            SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(resolution));
            check(pump_until(w, window, 5000, completed), "back to Auto finishes");
        }

        // ---- Application scale and the minimum window size
        for (int percent : { 150, 50, 100 }) {
            w.ctx.set_scale(percent);
            window->on_scale_changed();
            pump(w, window, 300);
            GetClientRect(window->hwnd(), &client);
            check(client.right > 0 && window->model().graph().client_width() > 0, "scale " + std::to_string(percent) + " keeps a drawable window");
            shot(window, "scale-" + std::to_string(percent) + ".bmp");
        }
        SetWindowPos(window->hwnd(), nullptr, 0, 0, 100, 100, SWP_NOMOVE | SWP_NOZORDER);
        pump(w, window, 300);
        GetWindowRect(window->hwnd(), &client);
        check(client.right - client.left >= w.ctx.px(820) && client.bottom - client.top >= w.ctx.px(520), "the window cannot get smaller than 820 x 520");
        shot(window, "minimum.bmp");
        SetWindowPos(window->hwnd(), nullptr, 0, 0, w.ctx.px(1100), w.ctx.px(700), SWP_NOMOVE | SWP_NOZORDER);
        pump(w, window, 200);

        // ---- Channels button
        SendMessageW(window->hwnd(), WM_COMMAND, MAKEWPARAM(1007, BN_CLICKED), 0);
        check(w.channel_requests == 1, "Channels... asks the main window");

        // ---- song replacement
        w.controller->open_file(g_fixtures + "\\tempo-changes.mid");
        pump(w, window, 50);
        check(!window->model().attached() || w.controller->has_song(), "the window detaches while the new file loads");
        for (int i = 0; i < 2000 && !w.controller->has_song(); ++i) {
            pump(w, window, 5);
        }
        check(pump_until(w, window, 5000, completed), "the new song is analysed");
        GetWindowTextW(window->hwnd(), title, 256);
        check(std::wstring(title).find(L"tempo-changes.mid") != std::wstring::npos, "the title follows the new file");
        shot(window, "second-song.bmp");

        // ---- a second window and clean close
        std::unique_ptr<ui::CompanionWindow> second = ui::create_analysis_window(w.host);
        check(second != nullptr, "a second Analysis window opens");
        if (second) {
            ui::AnalysisCompanion* other = static_cast<ui::AnalysisCompanion*>(second.get());
            check(pump_until(w, other, 5000, completed), "the second window finishes too");
            second->close();
            check(!IsWindow(second->hwnd()), "the second window closes");
        }
        created->close();
        check(!IsWindow(window->hwnd()), "the window closes");
        pump(w, nullptr, 100, false);
    }
    std::printf("midibn-analysis-window-smoke: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
