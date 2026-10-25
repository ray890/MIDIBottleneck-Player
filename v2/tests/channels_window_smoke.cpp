// Smoke test of the Win32 Channels window: a PlayerController with a recording output (no device,
// no provider) plays a small generated file, and the real window is opened on it (hidden unless
// --show is given): cells update, a forced value reaches the output, a channel is muted, mouse
// messages open and commit the editor, a song replacement detaches the window, scale changes work,
// and the window closes cleanly. Screenshots are drawn by the window's own paint code (never taken
// from the screen) into BMP files.
//
// Usage: midibn-channels-window-smoke [output-directory] [--show]
#include "channels_window.h"

#include "midibn/app/player_controller.h"
#include "midibn/null_output.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// ---- a small Standard MIDI File (division 480, 120 bpm: 480 ticks = 0.5 s), about 2.5 s long ----

void put_vlq(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t buffer[5];
    int n = 0;
    buffer[n++] = static_cast<uint8_t>(value & 0x7F);
    while ((value >>= 7) != 0) buffer[n++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    while (n > 0) out.push_back(buffer[--n]);
}

struct TrackWriter {
    std::vector<uint8_t> data;
    uint32_t last_tick = 0;
    void event(uint32_t tick, std::initializer_list<uint8_t> bytes)
    {
        put_vlq(data, tick - last_tick);
        last_tick = tick;
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    void finish()
    {
        put_vlq(data, 0);
        data.push_back(0xFF);
        data.push_back(0x2F);
        data.push_back(0x00);
    }
};

std::vector<uint8_t> build_midi()
{
    TrackWriter t;
    t.event(0, { 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 });                 // tempo 500,000 us/quarter
    t.event(0, { 0xB0, 0x00, 0x00 });                                   // channel 1: bank MSB 0, LSB 0
    t.event(0, { 0xB0, 0x20, 0x00 });
    t.event(0, { 0xC0, 0x04 });                                         // program 5 (Electric Piano 1)
    t.event(0, { 0xB0, 0x07, 100 });                                    // volume
    t.event(0, { 0xB0, 0x0A, 64 });                                     // pan
    t.event(0, { 0xE0, 0x00, 0x50 });                                   // pitch bend +2048... (lsb 0, msb 0x50)
    t.event(0, { 0xB9, 0x00, 120 });                                    // channel 10: GM2 rhythm bank
    t.event(0, { 0xB9, 0x20, 0x00 });
    t.event(0, { 0xC9, 0x08 });                                         // room set
    t.event(0, { 0xB2, 0x07, 90 });                                     // channel 3 volume
    for (uint32_t beat = 0; beat < 5; ++beat) {
        uint32_t tick = beat * 480;
        t.event(tick + 1, { 0x90, static_cast<uint8_t>(60 + beat), 100 });
        t.event(tick + 2, { 0x99, 36, 100 });
        t.event(tick + 3, { 0x92, static_cast<uint8_t>(48 + beat), 90 });
        t.event(tick + 240, { 0x80, static_cast<uint8_t>(60 + beat), 0 });
        t.event(tick + 241, { 0x82, static_cast<uint8_t>(48 + beat), 0 });
        t.event(tick + 242, { 0x89, 36, 0 });
    }
    t.finish();
    std::vector<uint8_t> file = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k' };
    uint32_t length = static_cast<uint32_t>(t.data.size());
    file.push_back(static_cast<uint8_t>(length >> 24));
    file.push_back(static_cast<uint8_t>(length >> 16));
    file.push_back(static_cast<uint8_t>(length >> 8));
    file.push_back(static_cast<uint8_t>(length));
    file.insert(file.end(), t.data.begin(), t.data.end());
    return file;
}

std::string temp_file(const char* name)
{
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + name;
}

struct Rig {
    win32::Win32FileSystem files;
    std::shared_ptr<platform::RecordingMidiOutput> winmm = std::make_shared<platform::RecordingMidiOutput>();
    std::shared_ptr<platform::RecordingMidiOutput> kdmapi = std::make_shared<platform::RecordingMidiOutput>();
    std::unique_ptr<app::PlayerController> controller;
    ui::UiContext ctx;
    ui::CompanionHost host;
    int refreshes = 0;
    HWND wake_window = nullptr;

    Rig()
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
        host.controller = controller.get();
        host.ctx = &ctx;
        // An invisible stand-in main window on Internal Display ('MIDIBN_SMOKE_HOST_AT=X,Y' overrides): without one the window
        // would open on the primary (4K) monitor. Default 3900,1150.
        int hostX = 3900, hostY = 1150;
        char at[64] = {};
        if (GetEnvironmentVariableA("MIDIBN_SMOKE_HOST_AT", at, sizeof at) > 0) {
            char* rest = nullptr;
            hostX = static_cast<int>(std::strtol(at, &rest, 10));
            if (rest && *rest == ',') hostY = static_cast<int>(std::strtol(rest + 1, nullptr, 10));
        }
        host.main_window = CreateWindowExW(0, L"STATIC", L"owner", WS_POPUP, hostX, hostY, 700, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        host.request_refresh = [this] { ++refreshes; };
    }
};

// One slice of the application's life: messages, the controller's events, the window's refresh.
void pump(Rig& rig, ui::ChannelsWindow* window)
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    rig.controller->pump();
    rig.controller->refresh();
    if (window) window->on_refresh();
}

template <typename Condition>
bool wait_until(Rig& rig, ui::ChannelsWindow* window, int milliseconds, Condition condition)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        pump(rig, window);
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    pump(rig, window);
    return condition();
}

bool load(Rig& rig, const std::string& path)
{
    rig.controller->open_file(path);
    return wait_until(rig, nullptr, 10000, [&] { return rig.controller->has_song() || !rig.controller->loading(); }) && rig.controller->has_song();
}

const app::ChannelCell& cell(ui::ChannelsWindow* w, int row, int column) { return w->session().model().cell(row, column); }

LPARAM point_in_cell(ui::ChannelsWindow* w, int row, int column)
{
    RECT r = w->grid().cell_rect(row, column);
    return MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

} // namespace

int main(int argc, char** argv)
{
    std::string outDir = temp_file("");
    bool show = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--show") show = true;
        else outDir = a;
    }
    if (!outDir.empty() && outDir.back() != '\\' && outDir.back() != '/') outDir += "\\";
    ui::g_channels_window_show_command = show ? SW_SHOWNOACTIVATE : SW_HIDE;

    std::string path = temp_file("midibn-channels-smoke.mid");
    {
        std::vector<uint8_t> bytes = build_midi();
        std::ofstream out(path.c_str(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    Rig rig;
    check(load(rig, path), "the generated file loads");
    app::PlayerController& controller = *rig.controller;

    // A window needs a song (v1 ShowChannelMonitor does nothing without one).
    {
        Rig empty;
        check(ui::ChannelsWindow::create(empty.host) == nullptr, "no window without a loaded song");
    }

    std::unique_ptr<ui::ChannelsWindow> window = ui::ChannelsWindow::create(rig.host);
    check(window != nullptr, "the window opens on a loaded song");
    if (!window) {
        std::printf("CHANNELS WINDOW SMOKE: FAIL\n");
        return 1;
    }
    ui::ChannelsWindow* w = window.get();
    wchar_t title[256] = L"";
    GetWindowTextW(w->hwnd(), title, 256);
    check(std::wstring(title) == L"MIDIBottleneck Player \x2014 MIDI Channel Monitor \x2014 midibn-channels-smoke.mid", "the title names the file");
    check(controller.engine().channel_snapshot() != nullptr, "monitoring is on while the window is open");
    check(w->auto_fit_enabled(), "the window starts auto-fitted");
    check(!w->grid().focus_cue_visible(), "no focus rectangle before the first key press (DD-087)");
    RECT client;
    GetClientRect(w->hwnd(), &client);
    check(client.right >= rig.ctx.px(420) && client.bottom >= rig.ctx.px(300), "the fitted window is a sensible size");
    check(w->grid().content_width() > 0 && w->grid().content_height() == rig.ctx.px(23) + 16 * rig.ctx.px(22), "grid metrics at 100%");
    {
        std::string widths;
        for (int c = 0; c < app::kChannelColumnCount; ++c) widths += (c ? "," : "") + std::to_string(w->grid().column_width(c));
        RECT outer;
        GetWindowRect(w->hwnd(), &outer);
        std::printf("fitted: client %ldx%ld, window %ldx%ld, columns %s\n", client.right, client.bottom, outer.right - outer.left,
            outer.bottom - outer.top, widths.c_str());
    }
    check(w->explanation_text() == L"Dispatched MIDI state. Click a Ch cell to enable/disable. Attribute cells scrub or type; right-click releases force or restores one latest source value. Blue bold is forced; gray italic is historical.",
        "the explanation line is v1's text");
    check(cell(w, 0, app::ColProgram).text == "\xE2\x80\x94", "unknown values show a dash before playback");

    // ---- play on the recording output
    controller.select_output(1);
    controller.play_clicked();
    bool updated = wait_until(rig, w, 4000, [&] { return cell(w, 0, app::ColSent).text != "0" && cell(w, 9, app::ColSent).text != "0"; });
    check(updated, "playing updates the Sent counts");
    check(cell(w, 0, app::ColProgram).text == "5 \xE2\x80\x94 Electric Piano 1", "channel 1 shows its Program with the General MIDI name");
    check(cell(w, 9, app::ColProgram).text == "9 \xE2\x80\x94 Room Set (GM2)", "channel 10 names the GM2 drum kit");
    check(cell(w, 0, app::ColVolume).text == "100" && cell(w, 0, app::ColPan).text == "64", "channel 1 volume and pan");
    check(cell(w, 0, app::ColPosition).text != "\xE2\x80\x94", "the Position column shows a time");
    check(cell(w, 0, app::ColKeysDown).text != "", "polyphony is shown");

    // ---- force a value (the override reaches the output)
    check(w->session().request_override(0, ChannelAttribute::Volume, 80).empty(), "a forced Volume is accepted");
    wait_until(rig, w, 1500, [&] { return cell(w, 0, app::ColVolume).text == "80"; });
    check(cell(w, 0, app::ColVolume).text == "80" && cell(w, 0, app::ColVolume).color == app::CellColor::Forced &&
        cell(w, 0, app::ColVolume).font == app::CellFont::Bold, "the forced value shows blue and bold");
    check(controller.engine().channel_override(0, ChannelAttribute::Volume) == 80, "the engine holds the forced Volume");

    // ---- mute a channel
    check(w->session().request_enabled(1, false).empty(), "muting channel 2 is accepted");
    wait_until(rig, w, 1500, [&] { return !w->session().model().row_enabled(1); });
    check(cell(w, 1, app::ColChannel).text == "2 off" && !w->session().model().row_enabled(1), "the muted channel shows off");
    check(wait_until(rig, w, 1500, [&] { return !w->session().follow_up_active(); }), "the follow-up polling ends by itself");

    // ---- real mouse messages
    HWND grid = w->grid().hwnd();
    {
        // click the Expression cell of channel 3 (unknown -> seed 127), release without moving: typing starts
        LPARAM p = point_in_cell(w, 2, app::ColExpression);
        SendMessageW(grid, WM_LBUTTONDOWN, MK_LBUTTON, p);
        check(w->grid().editor_visible() && w->session().editor().is_open(), "a click on an attribute cell opens the editor");
        SendMessageW(grid, WM_LBUTTONUP, 0, p);
        check(w->session().editor().typing(), "releasing without moving starts typing");
        HWND editor = w->grid().editor_window();
        SetWindowTextW(editor, L"90");
        SendMessageW(editor, WM_KEYDOWN, VK_RETURN, 0);
        check(controller.engine().channel_override(2, ChannelAttribute::Expression) == 90, "typing 90 and pressing Enter forces Expression");
        pump(rig, w);
        check(cell(w, 2, app::ColExpression).text == "90" && cell(w, 2, app::ColExpression).color == app::CellColor::Forced, "the cell shows the forced value");
        // an invalid entry is refused with the range
        SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        SendMessageW(editor, WM_LBUTTONUP, 0, 0);
        SendMessageW(grid, WM_LBUTTONUP, 0, 0);
        check(w->session().editor().typing(), "a click on the editor starts typing again");
        SetWindowTextW(editor, L"500");
        SendMessageW(editor, WM_KEYDOWN, VK_RETURN, 0);
        check(w->session().editor().last_error() == "Enter a value from 0 through 127.", "an out-of-range entry is refused with v1's message");
        check(controller.engine().channel_override(2, ChannelAttribute::Expression) == 90, "and changes nothing");
        // right-click on the forced cell releases it to Auto
        w->grid().hide_editor(true);
        SendMessageW(grid, WM_RBUTTONDOWN, MK_RBUTTON, p);
        check(controller.engine().channel_override(2, ChannelAttribute::Expression) == engine::ChannelOverrideState::kAutoValue,
            "right-click on a forced value releases it to Auto");
        check(!w->grid().editor_visible(), "and the editor is hidden");
        // click on the Ch column toggles the channel
        LPARAM ch = point_in_cell(w, 1, app::ColChannel);
        SendMessageW(grid, WM_LBUTTONDOWN, MK_LBUTTON, ch);
        SendMessageW(grid, WM_LBUTTONUP, 0, ch);
        wait_until(rig, w, 1500, [&] { return w->session().model().row_enabled(1); });
        check(w->session().model().row_enabled(1) && cell(w, 1, app::ColChannel).text == "2", "a click on Ch unmutes the channel again");
    }

    // ---- keyboard (DD-031): the keys reach the grid as ordinary WM_KEYDOWN messages
    {
        ui::ChannelGrid& g = w->grid();
        wchar_t name[64] = L"";
        GetWindowTextW(grid, name, 64);
        check(std::wstring(name) == L"MIDI channel grid", "the grid has an accessible name");
        check(g.current_row() == 1 && g.current_column() == app::ColChannel, "the current cell follows the last click (the Ch cell of channel 2)");
        SendMessageW(grid, WM_KEYDOWN, VK_HOME, 0);
        check(g.focus_cue_visible() == g.has_keyboard_focus(), "after a key press the focus rectangle follows the keyboard focus (DD-087)");
        SendMessageW(grid, WM_KEYDOWN, VK_UP, 0);
        SendMessageW(grid, WM_KEYDOWN, VK_UP, 0);
        SendMessageW(grid, WM_KEYDOWN, VK_UP, 0);
        check(g.current_row() == 0 && g.current_column() == 0, "Home and Up reach the first cell and stop there");
        SendMessageW(grid, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(grid, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(grid, WM_KEYDOWN, VK_DOWN, 0);
        for (int i = 0; i < app::ColVolume; ++i) SendMessageW(grid, WM_KEYDOWN, VK_RIGHT, 0);
        check(g.current_row() == 3 && g.current_column() == app::ColVolume, "arrow keys move the current cell");
        check(!g.key_down(VK_TAB, false) && !g.key_down('A', false), "Tab and letters are not used by the grid");
        // Enter starts typing in the Volume cell of channel 4; Enter in the edit control commits
        SendMessageW(grid, WM_KEYDOWN, VK_RETURN, 0);
        check(g.editor_visible() && w->session().editor().is_open() && w->session().editor().typing(), "Enter starts editing the current cell");
        HWND editor = g.editor_window();
        SetWindowTextW(editor, L"70");
        SendMessageW(editor, WM_KEYDOWN, VK_RETURN, 0);
        check(controller.engine().channel_override(3, ChannelAttribute::Volume) == 70, "typing 70 and Enter forces Volume of channel 4");
        pump(rig, w);
        // moving away closes the editor
        SendMessageW(grid, WM_KEYDOWN, VK_RIGHT, 0);
        check(!g.editor_visible() && g.current_column() == app::ColExpression, "an arrow key closes the editor and moves");
        // F2 then Escape cancels the edit and changes nothing
        SendMessageW(grid, WM_KEYDOWN, VK_F2, 0);
        check(w->session().editor().typing(), "F2 starts editing");
        SetWindowTextW(editor, L"55");
        SendMessageW(editor, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!w->session().editor().typing() && controller.engine().channel_override(3, ChannelAttribute::Expression) !=55, "Escape cancels the edit");
        check(g.key_down(VK_ESCAPE, false), "Escape closes the still open editor");
        check(!g.editor_visible() && !g.key_down(VK_ESCAPE, false), "with nothing to cancel Escape is left to the window");
        // Delete releases a forced value, like right-click
        SendMessageW(grid, WM_KEYDOWN, VK_LEFT, 0);
        check(g.current_column() == app::ColVolume, "Left moves back");
        SendMessageW(grid, WM_KEYDOWN, VK_DELETE, 0);
        check(controller.engine().channel_override(3, ChannelAttribute::Volume) == engine::ChannelOverrideState::kAutoValue, "Delete releases a forced value to Auto");
        // Space mutes and unmutes the row's channel from any cell
        SendMessageW(grid, WM_KEYDOWN, VK_DOWN, 0);
        check(g.current_row() == 4, "Down");
        SendMessageW(grid, WM_KEYDOWN, VK_SPACE, 0);
        wait_until(rig, w, 1500, [&] { return !w->session().model().row_enabled(4); });
        check(!w->session().model().row_enabled(4) && cell(w, 4, app::ColChannel).text == "5 off", "Space mutes the current row's channel");
        SendMessageW(grid, WM_KEYDOWN, VK_SPACE, 0);
        wait_until(rig, w, 1500, [&] { return w->session().model().row_enabled(4); });
        check(w->session().model().row_enabled(4), "Space unmutes it again");
        // Enter on the Ch cell is a click on it
        SendMessageW(grid, WM_KEYDOWN, VK_HOME, 0);
        SendMessageW(grid, WM_KEYDOWN, VK_RETURN, 0);
        wait_until(rig, w, 1500, [&] { return !w->session().model().row_enabled(4); });
        check(!w->session().model().row_enabled(4) && !g.editor_visible(), "Enter on the Ch cell mutes the channel");
        SendMessageW(grid, WM_KEYDOWN, VK_SPACE, 0);
        wait_until(rig, w, 1500, [&] { return w->session().model().row_enabled(4); });
        check(w->session().model().row_enabled(4), "and Space unmutes it");
        // Enter on a cell that cannot be edited does nothing
        SendMessageW(grid, WM_KEYDOWN, VK_RIGHT, 0);
        SendMessageW(grid, WM_KEYDOWN, VK_RETURN, 0);
        check(!g.editor_visible(), "Enter on a read-only cell opens nothing");
        // a click makes its cell the current one
        LPARAM p = point_in_cell(w, 7, app::ColPan);
        SendMessageW(grid, WM_LBUTTONDOWN, MK_LBUTTON, p);
        SendMessageW(grid, WM_LBUTTONUP, 0, p);
        check(g.current_row() == 7 && g.current_column() == app::ColPan, "a click makes its cell current");
        w->grid().hide_editor(false);
        check(g.key_down(VK_END, true) && g.current_row() == 15 && g.current_column() == app::kChannelColumnCount - 1, "Ctrl+End goes to the last cell");
        check(g.key_down(VK_HOME, true) && g.current_row() == 0 && g.current_column() == 0, "Ctrl+Home goes to the first cell");
        check(g.key_down(VK_NEXT, false) && g.current_row() == 15 && g.key_down(VK_PRIOR, false) && g.current_row() == 0, "Page Down / Page Up");
        check(g.key_down(VK_END, false) && g.current_column() == app::kChannelColumnCount - 1 && g.key_down(VK_HOME, false) && g.current_column() == 0, "End / Home");
    }

    // ---- nothing is repainted when nothing changed (the player is stopped here; no timer runs when idle)
    controller.stop_clicked();
    wait_until(rig, w, 1500, [&] { return !w->session().follow_up_active(); });
    pump(rig, w);
    UpdateWindow(grid);
    pump(rig, w);
    pump(rig, w);
    check(GetUpdateRect(grid, nullptr, FALSE) == FALSE, "an idle refresh repaints nothing");

    // ---- a column the user resized keeps its width, and the window re-fits
    {
        int before = w->grid().column_width(app::ColVolume);
        RECT volume = w->grid().cell_rect(-1, app::ColVolume);
        int borderX = volume.right - 1;
        int oldContent = w->grid().content_width();
        SendMessageW(grid, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(borderX, 5));
        SendMessageW(grid, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(borderX + 30, 5));
        SendMessageW(grid, WM_LBUTTONUP, 0, MAKELPARAM(borderX + 30, 5));
        pump(rig, w);
        check(w->grid().column_width(app::ColVolume) == before + 30, "dragging a heading border resizes the column");
        check(w->grid().content_width() == oldContent + 30, "the content grows with it");
        RECT c2;
        GetClientRect(w->hwnd(), &c2);
        check(c2.right >= w->grid().content_width(), "the auto-fitted window follows the new width");
    }

    // ---- screenshot at 100% and at 150%
    check(w->write_bitmap(widen(outDir) + L"channels-100.bmp"), "the 100% screenshot is written");
    rig.ctx.set_scale(150);
    w->on_scale_changed();
    pump(rig, w);
    check(w->write_bitmap(widen(outDir) + L"channels-150.bmp"), "the 150% screenshot is written");
    check(w->grid().content_height() == rig.ctx.px(23) + 16 * rig.ctx.px(22), "grid metrics follow the Application scale");
    rig.ctx.set_scale(100);
    w->on_scale_changed();
    pump(rig, w);

    // ---- a manual resize ends auto-fit; a narrow window scrolls
    {
        RECT wr;
        GetWindowRect(w->hwnd(), &wr);
        SendMessageW(w->hwnd(), WM_ENTERSIZEMOVE, 0, 0);
        SetWindowPos(w->hwnd(), nullptr, wr.left, wr.top, rig.ctx.px(900), rig.ctx.px(300), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(w->hwnd(), WM_EXITSIZEMOVE, 0, 0);
        pump(rig, w);
        check(!w->auto_fit_enabled(), "a manual resize ends auto-fit");
        check((GetWindowLongW(grid, GWL_STYLE) & WS_HSCROLL) != 0, "a narrow window shows the horizontal scroll bar");
        check((GetWindowLongW(grid, GWL_STYLE) & WS_VSCROLL) != 0, "a short window shows the vertical scroll bar");
        check(w->write_bitmap(widen(outDir) + L"channels-narrow.bmp"), "the narrow screenshot is written");
        SendMessageW(grid, WM_HSCROLL, SB_BOTTOM, 0);
        SendMessageW(grid, WM_VSCROLL, SB_BOTTOM, 0);
        RECT last = w->grid().cell_rect(15, app::ColPosition);
        RECT gc;
        GetClientRect(grid, &gc);
        check(last.right <= gc.right && last.bottom <= gc.bottom, "scrolling to the end brings the last cell into view");
        check(w->write_bitmap(widen(outDir) + L"channels-scrolled.bmp"), "the scrolled screenshot is written");
        SendMessageW(grid, WM_HSCROLL, SB_TOP, 0);
        SendMessageW(grid, WM_VSCROLL, SB_TOP, 0);
        // the keyboard scrolls the current cell into view
        {
            ui::ChannelGrid& g = w->grid();
            g.key_down(VK_END, true);
            RECT lastCell = g.cell_rect(15, app::ColPosition), view;
            GetClientRect(grid, &view);
            check(lastCell.right <= view.right && lastCell.bottom <= view.bottom && lastCell.left >= 0 && lastCell.top >= 0, "Ctrl+End scrolls the last cell into view");
            g.key_down(VK_HOME, true);
            RECT firstCell = g.cell_rect(0, 0);
            check(firstCell.left >= 0 && firstCell.top >= rig.ctx.px(23), "Ctrl+Home scrolls the first cell into view");
            g.key_down(VK_DOWN, false);
            g.key_down(VK_PRIOR, false);
        }
        // restore the fitted size for the rest of the run
        w->fit_window_to_grid(true);
    }

    // ---- Always on top and manual resize
    w->set_always_on_top(true);
    check((GetWindowLongW(w->hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, "Always on top");
    w->set_always_on_top(false);
    check((GetWindowLongW(w->hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) == 0, "Always on top off");
    SendMessageW(w->hwnd(), WM_SYSCOMMAND, 0x1F30, 0);
    check(w->always_on_top(), "the system menu item toggles it");
    SendMessageW(w->hwnd(), WM_SYSCOMMAND, 0x1F30, 0);

    // ---- a new file replaces the song: the window detaches, then attaches
    controller.stop_clicked();
    controller.open_file(path);
    w->on_refresh();   // the main window refreshes its companions right after the action
    check(w->session().model().detached() && w->session().model().detached_message() == "Loading new MIDI\xE2\x80\xA6", "loading detaches with v1's message");
    check(controller.engine().channel_snapshot() == nullptr, "monitoring is off while loading");
    bool attached = wait_until(rig, w, 10000, [&] { return !w->session().model().detached(); });
    check(attached, "the window attaches to the new song");
    check(controller.engine().channel_snapshot() != nullptr, "monitoring is back on");
    controller.open_file(temp_file("midibn-no-such-file.mid"));
    w->on_refresh();
    bool failed = wait_until(rig, w, 10000, [&] { return !controller.loading() && w->session().model().detached(); });
    check(failed && w->session().model().detached_message() == "MIDI loading failed. No MIDI file is loaded.", "a failed load leaves v1's message");
    controller.take_messages();
    check(load(rig, path), "the file loads again");
    pump(rig, w);
    check(!w->session().model().detached(), "and the window attaches again");

    // ---- close
    rig.controller->stop_clicked();
    pump(rig, w);
    check(controller.engine().channel_snapshot() != nullptr, "monitoring still on before close");
    window->close();
    check(!IsWindow(w->hwnd()) || w->hwnd() == nullptr, "the window is gone");
    check(controller.engine().channel_snapshot() == nullptr, "closing turns monitoring off");
    window.reset();
    // a second window can open afterwards
    window = ui::ChannelsWindow::create(rig.host);
    check(window != nullptr && controller.engine().channel_snapshot() != nullptr, "a new window opens and monitors again");
    window.reset();
    check(controller.engine().channel_snapshot() == nullptr, "destroying the window turns monitoring off");

    // The recording output saw the forced Volume (channel 1, controller 7, value 80).
    bool sawForced = false;
    for (size_t i = 0; i < rig.winmm->messages.size(); ++i) {
        const platform::RecordedMessage& m = rig.winmm->messages[i];
        if (m.kind == platform::RecordedMessage::Short && (m.packed & 0xFFFFFF) == (0xB0u | (7u << 8) | (80u << 16))) sawForced = true;
    }
    check(sawForced, "the output received the forced Volume");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf(g_failures == 0 ? "CHANNELS WINDOW SMOKE: PASS\n" : "CHANNELS WINDOW SMOKE: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
