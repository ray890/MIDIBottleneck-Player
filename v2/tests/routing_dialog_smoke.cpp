// Smoke test of the Track routing editor window (frontends/win32/routing_dialog.cpp) without a user.
//
// A PlayerController with recording outputs loads fixtures; the real modal dialog is opened
// on this thread and a second thread drives it with messages (choose a tier, type a capacity,
// select a lane and give it a destination, confirm, press Apply / Use original routing / Cancel).
// Afterwards the controller's route and settings are checked and the routed song is played on
// output None and on a recording output (no device is opened, no provider is loaded).
// The dialog is rendered to BMP files with WM_PRINT (never the screen) when --shots is given.
//
// The dialog opens over an invisible stand-in for the main window at (3900, 1150) on Internal Display, or at --owner X,Y; every opening
// checks that the dialog is on the stand-in's monitor and inside its work area (DD-098), and the placement
// arithmetic is checked for a two-monitor layout without opening anything there.
//
// Usage: midibn-routing-dialog-smoke [--fixtures DIR] [--shots DIR] [--scale PERCENT] [--large FILE] [--owner X,Y]
#include "routing_dialog.h"

#include "lane_grid.h"
#include "midibn/app/player_controller.h"
#include "midibn/null_output.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;
using namespace midibn::ui;

namespace {

std::mutex g_mutex;
int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

struct Env {
    win32::Win32FileSystem files;
    std::shared_ptr<platform::RecordingMidiOutput> winmm = std::make_shared<platform::RecordingMidiOutput>();
    std::shared_ptr<platform::RecordingMidiOutput> kdmapi = std::make_shared<platform::RecordingMidiOutput>();
    std::unique_ptr<app::PlayerController> controller;
    std::atomic<int> wakes{ 0 };

    Env()
    {
        app::PlayerEnvironment env;
        env.services = std::make_shared<win32::Win32EngineServices>();
        env.files = &files;
        env.list_devices = [] { return std::vector<platform::OutputDeviceInfo>(); };
        env.create_winmm = [this] { return std::shared_ptr<platform::MidiOutput>(winmm); };
        env.create_kdmapi = [this] { return std::shared_ptr<platform::MidiOutput>(kdmapi); };
        env.pointer_size = static_cast<int>(sizeof(void*));
        env.wake_ui = [this] { ++wakes; };
        controller.reset(new app::PlayerController(env));
    }
    bool load(const std::string& path, int ms)
    {
        controller->open_file(path);
        for (int i = 0; i < ms / 5; ++i) {
            controller->pump();
            if (controller->has_song() || !controller->loading()) return controller->has_song();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

// ------------------------------------------------------------------ driving the dialog from another thread

HWND wait_for_dialog(int ms)
{
    for (int i = 0; i < ms / 10; ++i) {
        HWND window = FindWindowW(kRoutingDialogClass, nullptr);
        if (window) return window;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}

LRESULT query(HWND dialog, UINT message, LPARAM lParam = 0)
{
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(dialog, message, 0, lParam, SMTO_ABORTIFHUNG, 5000, &result)) return -1;
    return static_cast<LRESULT>(result);
}

struct Latency {
    double max_ms = 0;
    int probes = 0;
};

// Waits until the plan is no longer pending; probes the window's responsiveness while waiting.
bool wait_plan(HWND dialog, Latency* latency, int ms)
{
    LARGE_INTEGER frequency, a, b;
    QueryPerformanceFrequency(&frequency);
    for (int i = 0; i < ms / 5; ++i) {
        QueryPerformanceCounter(&a);
        LRESULT bits = query(dialog, WM_ROUTING_STATE);
        QueryPerformanceCounter(&b);
        double elapsed = 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(frequency.QuadPart);
        if (latency) {
            latency->max_ms = std::max(latency->max_ms, elapsed);
            ++latency->probes;
        }
        if (bits >= 0 && (bits & 1) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void choose_combo(HWND dialog, int id, int index)
{
    HWND combo = GetDlgItem(dialog, id);
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
}

void select_row(HWND dialog, int row)
{
    LVITEMW item = {};
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(GetDlgItem(dialog, RdLanes), LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
}

void click(HWND dialog, int id)
{
    SendMessageW(GetDlgItem(dialog, id), BM_CLICK, 0, 0);
}

std::wstring shot_path(const std::string& dir, const char* name)
{
    return win32::utf8_to_wide(dir + "\\" + name);
}

void shot(HWND dialog, const std::string& dir, const char* name)
{
    if (dir.empty()) return;
    std::wstring path = shot_path(dir, name);
    check(query(dialog, WM_ROUTING_SCREENSHOT, reinterpret_cast<LPARAM>(path.c_str())) == 1, std::string("screenshot ") + name);
}

routing::TrackLaneKey lane_of_row(app::PlayerController& controller, int row)
{
    std::shared_ptr<routing::CompiledTrackRoute> plan = routing::compile_route(controller.routing_song(), routing::TrackRoutingSettings());
    return plan->lanes[static_cast<size_t>(row)]->lane;
}

// ------------------------------------------------------------------ scenarios

struct Options {
    std::string fixtures = "v2\\harness\\fixtures";
    std::string shots;
    std::string large;
    int scale = 100;
    int owner_x = 3900, owner_y = 1150;   // inside Internal Display (never the primary 4K monitor); --owner X,Y overrides
};

bool rect_inside(const RECT& inner, const RECT& outer)
{
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

// DD-098: the dialog opens on the main window's monitor, centred over it, wholly inside that monitor's work area.
void placement_checks(HWND dialog, HWND owner)
{
    RECT d = {}, o = {};
    GetWindowRect(dialog, &d);
    GetWindowRect(owner, &o);
    RECT work = monitor_work_area(owner);
    check(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST) == MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST),
        "the dialog opens on the main window's monitor");
    check(rect_inside(d, work), "the dialog lies inside the work area of the main window's monitor");
    POINT expected = centre_in_work_area(o, work, d.right - d.left, d.bottom - d.top);
    check(d.left == expected.x && d.top == expected.y, "the dialog is centred over the main window (kept inside the work area)");
}

// The placement arithmetic on this PC's layout and on a few edge cases (nothing is opened).
void placement_arithmetic_checks()
{
    const RECT primary = { 0, 0, 3840, 2112 };          // a 4K primary monitor with the taskbar at the bottom
    const RECT second = { 3840, 1073, 5760, 2113 };     // a 1920x1080 monitor right of it, lower, taskbar 40 high
    const RECT left = { -1920, 0, 0, 1040 };            // a monitor left of the primary (negative coordinates)
    auto at = [](const RECT& owner, const RECT& work, int w, int h) { return centre_in_work_area(owner, work, w, h); };
    POINT p = at(RECT{ 4000, 1200, 4800, 1900 }, second, 1070, 730);
    check(p.x >= second.left && p.y >= second.top && p.x + 1070 <= second.right && p.y + 730 <= second.bottom,
        "arithmetic: a dialog over a main window on the second monitor stays on the second monitor");
    check(p.x == 3865 && p.y == 1185, "arithmetic: centred over the main window, then moved inside the work area");
    p = at(RECT{ 100, 100, 900, 800 }, primary, 1070, 730);
    check(p.x == 0 && p.y == 85, "arithmetic: on the primary monitor, kept right of its left edge");
    p = at(RECT{ -1800, 200, -1000, 900 }, left, 1070, 730);
    check(p.x == -1920 && p.y == 185, "arithmetic: on a monitor with negative coordinates (it used to be moved to x 0)");
    p = at(RECT{ 5000, 1500, 5760, 2113 }, second, 1070, 730);
    check(p.x == 5760 - 1070 && p.y == 2113 - 730, "arithmetic: near the bottom-right corner, kept inside");
    p = at(RECT{ 4000, 1200, 4800, 1900 }, second, 2400, 1500);
    check(p.x == second.left && p.y == second.top, "arithmetic: larger than the work area, the caption stays visible");
    POINT q = keep_in_work_area(5500, 1000, 600, 400, second);
    check(q.x == 5160 && q.y == 1073, "arithmetic: keep_in_work_area moves a window partly outside back in");

    // DD-106: the main window opens centred on the monitor that holds the mouse
    const RECT mons[2] = { { 0, 0, 3840, 2160 }, { 3840, 1073, 5760, 2153 } };
    const RECT works[2] = { primary, second };
    RECT w = startup_window_rect(mons, works, 2, POINT{ 4500, 1500 }, 800, 600);
    check(w.left == 3840 + (1920 - 800) / 2 && w.top == 1073 + (1040 - 600) / 2 && w.right - w.left == 800 && w.bottom - w.top == 600,
        "arithmetic: mouse on the second monitor: the main window is centred in its work area");
    w = startup_window_rect(mons, works, 2, POINT{ 100, 100 }, 800, 600);
    check(w.left == (3840 - 800) / 2 && w.top == (2112 - 600) / 2, "arithmetic: mouse on the primary monitor: centred there");
    w = startup_window_rect(mons, works, 2, POINT{ 6000, 3000 }, 800, 600);
    check(w.left >= second.left && w.right <= second.right, "arithmetic: mouse outside every monitor: the nearest monitor");
    w = startup_window_rect(mons, works, 2, POINT{ 5000, 2150 }, 2400, 1500);
    check(w.left == second.left && w.top == second.top, "arithmetic: larger than the work area: kept at its top left");
    w = startup_window_rect(mons, works, 0, POINT{ 0, 0 }, 800, 600);
    check(w.left == 0 && w.top == 0, "arithmetic: no monitors: origin");
}

// Opens the dialog on this thread while `driver` runs on another.
void run_dialog(CompanionHost& host, const std::function<void(HWND)>& driver)
{
    HWND owner = host.main_window;
    std::thread thread([&driver, owner] {
        HWND dialog = wait_for_dialog(15000);
        check(dialog != nullptr, "the dialog opens");
        if (dialog && owner) placement_checks(dialog, owner);
        if (dialog) driver(dialog);
    });
    show_track_routing_dialog(host);
    thread.join();
}

void play_checks(Env& env, const std::shared_ptr<const routing::CompiledTrackRoute>& route)
{
    app::PlayerController& c = *env.controller;
    // Output None: the routed song plays to its end or at least runs.
    c.select_output(0);
    c.play_clicked();
    std::string state = c.view().state_text;
    check(state == "Playing" || state == "Completed", "routed song plays on output None (" + state + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    c.seek_to(c.view().duration_us / 2);
    c.stop_clicked();
    check(c.view().state_text == "Stopped", "stops");
    // A recording output sees only the planned destination channels.
    env.kdmapi->messages.clear();
    c.set_kdmapi(true);
    c.play_clicked();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c.stop_clicked();
    bool destination[16] = {};
    for (const routing::TrackRouteDestinationPreview& d : route->destinations) destination[d.destination_channel] = true;
    size_t channelMessages = 0, outside = 0;
    for (const platform::RecordedMessage& message : env.kdmapi->messages) {
        if (message.kind != platform::RecordedMessage::Short) continue;
        uint32_t status = message.packed & 0xFF;
        if ((status & 0xF0) == 0x80 || (status & 0xF0) == 0x90) {   // notes only: Stop sends the panic controllers on every channel
            ++channelMessages;
            if (!destination[status & 0x0F]) ++outside;
        }
    }
    check(channelMessages > 0, "the recording output received channel messages");
    check(outside == 0, "every channel message goes to a planned destination channel");
    c.set_kdmapi(false);
}

void scenario_apply(Env& env, CompanionHost& host, const Options& options)
{
    app::PlayerController& c = *env.controller;
    check(c.track_route() == nullptr, "no route before the dialog");
    routing::TrackLaneKey lane = lane_of_row(c, 4);
    run_dialog(host, [&](HWND dialog) {
        check(wait_plan(dialog, nullptr, 20000), "the first plan arrives");
        check(query(dialog, WM_ROUTING_ROWS) > 4, "lane rows are shown");
        shot(dialog, options.shots, "routing-1-defaults.bmp");
        // Aggressive tier: lossy, needs the confirmation.
        choose_combo(dialog, RdTier, 2);
        check(wait_plan(dialog, nullptr, 20000), "plan after choosing the tier");
        // Capacity typed into the field (committed by the field's timer).
        SetWindowTextW(GetDlgItem(dialog, RdCapacity), L"6");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(wait_plan(dialog, nullptr, 20000), "plan after typing the capacity");
        for (int i = 0; i < 100 && (query(dialog, WM_ROUTING_STATE) & 1) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(wait_plan(dialog, nullptr, 20000), "plan after the capacity timer");
        // A manual destination for row 4: combo index 3 = Output channel 3.
        select_row(dialog, 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        choose_combo(dialog, RdDestination, 3);
        check(wait_plan(dialog, nullptr, 20000), "plan after the manual destination");
        shot(dialog, options.shots, "routing-2-aggressive.bmp");
        LRESULT bits = query(dialog, WM_ROUTING_STATE);
        check((bits & 4) != 0, "the lossy confirmation is offered for an Aggressive plan");
        check((bits & 2) == 0, "Apply waits for the confirmation");
        click(dialog, RdConfirm);
        bits = query(dialog, WM_ROUTING_STATE);
        check((bits & 2) != 0, "Apply is enabled after the confirmation");
        shot(dialog, options.shots, "routing-3-confirmed.bmp");
        click(dialog, IDOK);
    });
    std::shared_ptr<const routing::CompiledTrackRoute> route = c.track_route();
    check(route != nullptr && route->is_valid(), "the controller holds the applied route");
    routing::TrackRoutingSettings settings = c.track_routing_settings();
    check(settings.maximum_automatic_tier == routing::TrackRoutingTier::Aggressive, "tier recorded");
    check(settings.target_capacity == 6, "capacity recorded");
    check(settings.manual_destinations.count(lane) == 1 && settings.manual_destinations.at(lane) == 2, "manual destination recorded");
    check(c.view().menu.track_routing_checked, "the menu shows an active route");
    if (route) play_checks(env, route);
}

void scenario_original_and_cancel(Env& env, CompanionHost& host, const Options& options)
{
    app::PlayerController& c = *env.controller;
    std::shared_ptr<const routing::CompiledTrackRoute> before = c.track_route();
    check(before != nullptr, "a route is active");
    // Cancel keeps the route.
    run_dialog(host, [&](HWND dialog) {
        check(wait_plan(dialog, nullptr, 20000), "plan arrives (reopened)");
        // The settings of the active route come back.
        check(static_cast<int>(SendMessageW(GetDlgItem(dialog, RdTier), CB_GETCURSEL, 0, 0)) == 2, "the dialog starts from the applied tier");
        wchar_t capacity[8] = {};
        GetWindowTextW(GetDlgItem(dialog, RdCapacity), capacity, 7);
        check(std::wstring(capacity) == L"6", "the dialog starts from the applied capacity");
        check(IsWindowEnabled(GetDlgItem(dialog, RdOriginal)) != FALSE, "Use original routing is enabled with an active route");
        shot(dialog, options.shots, "routing-4-reopened.bmp");
        choose_combo(dialog, RdPercussion, 1);
        check(wait_plan(dialog, nullptr, 20000), "plan after the percussion policy");
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, IDCANCEL)));
    });
    check(c.track_route() == before, "Cancel leaves the route unchanged");
    // Use original routing clears it.
    run_dialog(host, [&](HWND dialog) {
        check(wait_plan(dialog, nullptr, 20000), "plan arrives (third time)");
        click(dialog, RdOriginal);
    });
    check(c.track_route() == nullptr, "Use original routing restores the original routing");
    check(!c.view().menu.track_routing_checked, "the menu shows no route");
    // With no route the button is disabled and the defaults are shown.
    run_dialog(host, [&](HWND dialog) {
        check(wait_plan(dialog, nullptr, 20000), "plan arrives (fourth time)");
        check(IsWindowEnabled(GetDlgItem(dialog, RdOriginal)) == FALSE, "Use original routing is disabled without a route");
        check(static_cast<int>(SendMessageW(GetDlgItem(dialog, RdTier), CB_GETCURSEL, 0, 0)) == 0, "defaults after clearing the route");
        // Default settings give a hard error on this song (more lanes than channels): Apply is off.
        click(dialog, IDOK);
        check((query(dialog, WM_ROUTING_STATE) & 16) == 0, "Apply cannot be pressed on a plan with hard errors");
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, IDCANCEL)));
    });
    check(c.track_route() == nullptr, "still no route");
}

void scenario_failure(Env& env, CompanionHost& host, const Options& options)
{
    app::PlayerController& c = *env.controller;
    check(env.load(options.fixtures + "\\routing-patches-4097.mid", 10000), "the over-limit fixture loads");
    run_dialog(host, [&](HWND dialog) {
        check(wait_plan(dialog, nullptr, 20000), "the failing plan finishes");
        LRESULT bits = query(dialog, WM_ROUTING_STATE);
        check((bits & 8) != 0 && (bits & 2) == 0, "failure is reported and Apply stays off");
        shot(dialog, options.shots, "routing-5-failure.bmp");
        click(dialog, IDCANCEL);
    });
    check(c.track_route() == nullptr, "nothing applied after a failed plan");
}

void scenario_large(Env& env, CompanionHost& host, const Options& options)
{
    app::PlayerController& c = *env.controller;
    std::printf("large: loading %s\n", options.large.c_str());
    check(env.load(options.large, 600000), "the large fixture loads");
    LARGE_INTEGER frequency, a, b;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&a);
    std::shared_ptr<routing::CompiledTrackRoute> direct = routing::compile_route(c.routing_song(), routing::TrackRoutingSettings());
    QueryPerformanceCounter(&b);
    std::printf("large: compile_route takes %.0f ms (%d lanes, %s)\n",
        1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(frequency.QuadPart), static_cast<int>(direct->lanes.size()),
        direct->is_valid() ? "valid" : "hard errors");
    Latency latency;
    run_dialog(host, [&](HWND dialog) {
        check(wait_plan(dialog, &latency, 120000), "large: the first plan arrives");
        // A setting change while the window must stay responsive.
        choose_combo(dialog, RdTier, 2);
        check(wait_plan(dialog, &latency, 120000), "large: plan after the tier change");
        shot(dialog, options.shots, "routing-6-large.bmp");
        click(dialog, IDCANCEL);
    });
    std::printf("large: the dialog answered %d probes while planning, slowest %.1f ms\n", latency.probes, latency.max_ms);
    check(latency.max_ms < 250.0, "large: the window stays responsive while the plan is computed");
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string key = argv[i], value = argv[i + 1];
        if (key == "--fixtures") options.fixtures = value;
        else if (key == "--shots") options.shots = value;
        else if (key == "--scale") options.scale = std::atoi(value.c_str());
        else if (key == "--large") options.large = value;
        else if (key == "--owner") {
            char* rest = nullptr;
            options.owner_x = static_cast<int>(std::strtol(value.c_str(), &rest, 10));
            if (rest && *rest == ',') options.owner_y = static_cast<int>(std::strtol(rest + 1, nullptr, 10));
        }
    }
    UiContext ctx;
    ctx.set_scale(options.scale);
    placement_arithmetic_checks();

    Env env;
    HWND owner = CreateWindowExW(0, L"STATIC", L"owner", WS_POPUP, options.owner_x, options.owner_y, 400, 300, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    CompanionHost host;
    host.controller = env.controller.get();
    host.ctx = &ctx;
    host.main_window = owner;

    if (!env.load(options.fixtures + "\\routing-lanes.mid", 10000)) {
        std::printf("FAIL cannot load routing-lanes.mid\n");
        return 1;
    }
    scenario_apply(env, host, options);
    scenario_original_and_cancel(env, host, options);
    scenario_failure(env, host, options);
    if (!options.large.empty()) scenario_large(env, host, options);
    DestroyWindow(owner);
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
