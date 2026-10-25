// In-process smoke test of the main window (test-only: --selftest RESULT_FILE --open FIXTURE).
// It drives the real controls with ordinary window messages (button clicks, combo
// selections, system-menu commands), never opens a MIDI device (output None only),
// and writes one PASS/FAIL line per check.
#include "main_window.h"

#include "glue.h"
#include "win32_timing.h"
#include "thread_report.h"

#include "midibn/app/ui_mappings.h"

#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace midibn {
namespace ui {

namespace {

struct Report {
    std::string text;
    int failures = 0;
    int checks = 0;
    void check(bool ok, const std::string& what)
    {
        ++checks;
        if (!ok) ++failures;
        text += std::string(ok ? "PASS " : "FAIL ") + what + "\n";
    }
};

void pump(int milliseconds)
{
    DWORD end = GetTickCount() + static_cast<DWORD>(milliseconds);
    MSG message;
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { PostQuitMessage(0); return; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    } while (static_cast<int>(end - GetTickCount()) > 0);
}

bool wait_until(const std::function<bool()>& condition, int timeoutMs)
{
    DWORD end = GetTickCount() + static_cast<DWORD>(timeoutMs);
    while (!condition()) {
        pump(20);
        if (static_cast<int>(end - GetTickCount()) <= 0) return condition();
    }
    return true;
}

RECT rect_in(HWND parent, HWND child)
{
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

bool own_visible_style(HWND window) { return (GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE) != 0; }

bool overlap(const RECT& a, const RECT& b)
{
    RECT i;
    return IntersectRect(&i, &a, &b) != FALSE;
}

} // namespace

// --selftest with --settings FILE: only the remembered-settings checks (the full self-test toggles many options, which would
// rewrite the file). Writes the state the window started in ("start:" lines, in the settings file format), optionally makes
// some changes (--settings-change), then writes the state after them ("after:" lines) and the note shown, if any.
int MainWindow::run_settings_selftest()
{
    flush_accessible_names();
    std::string text;
    auto dump = [&](const char* tag) {
        const std::string body = app::serialize_settings(current_settings());
        text += std::string("[") + tag + "]\n" + body;
    };
    dump("start");
    text += "note=" + controller_->view().file_info + "\n";
    text += "output_selected=" + std::to_string(controller_->view().output_selected) + "\n";
    text += "few_cpu_offered=" + std::to_string(midibn::win32::few_cpu_yield_option_available() ? 1 : 0) + " usable=" +
        std::to_string(midibn::win32::usable_logical_processors()) + " few_cpu_on=" + std::to_string(few_cpu_yield_ ? 1 : 0) + "\n";
    if (options_.settings_change) {
        SendMessageW(window_, WM_SYSCOMMAND, CmdScale150, 0);
        SendMessageW(window_, WM_SYSCOMMAND, CmdAlwaysOnTop, 0);
        SendMessageW(window_, WM_SYSCOMMAND, CmdChase, 0);
        SendMessageW(window_, WM_SYSCOMMAND, CmdShowStatistics, 0);
        controller_->set_kdmapi(true);
        const int last = static_cast<int>(controller_->view().output_items.size()) - 1;
        if (last > 0) controller_->select_output(last);
        after_action();
        if (midibn::win32::few_cpu_yield_option_available()) SendMessageW(window_, WM_SYSCOMMAND, CmdFewCpuYield, 0);
        text += "note_after=" + controller_->view().file_info + "\n";
        dump("after");
    }
    if (!options_.result_file.empty()) {
        HANDLE file = CreateFileW(options_.result_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    return 0;
}

int MainWindow::run_selftest()
{
    if (!options_.settings_path.empty() || options_.settings_default) return run_settings_selftest();
    flush_accessible_names();   // DD-058: normally done from the first idle moment; the self-test never pumps messages first
    Report r;
    const std::wstring fixture = options_.open_file;
    // ---- controls exist
    struct Named { const char* name; HWND window; };
    Named named[] = {
        { "Open MIDI", open_ }, { "file name", file_label_ }, { "file info", info_label_ }, { "output combo", output_ },
        { "KDMAPI check", kdmapi_ }, { "queue check", queue_check_ }, { "queue value", queue_value_ }, { "overflow combo", overflow_ },
        { "rate combo", rate_ }, { "processing value", processing_value_ }, { "rate slider", slider_ }, { "timeline", timeline_ },
        { "Play", play_ }, { "Stop", stop_ }, { "-5 sec", back5_ }, { "+5 sec", forward5_ }, { "Analysis", analysis_ }, { "Reset stats", reset_ },
        { "statistics", stats_ }, { "state", state_ }
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i) r.check(named[i].window != nullptr && IsWindow(named[i].window), std::string("control exists: ") + named[i].name);

    const app::PlayerView& v0 = controller_->view();
    r.check(v0.output_items.size() >= 1 && v0.output_items[0] == "None", "output list starts with None");
    r.check(!v0.play_enabled, "Play disabled with no file");
    r.check(v0.rate_choice == app::RateModelChoice::None, "Rate model starts at None");
    r.check(v0.queue_value.value == 2000 && v0.queue_value.minimum == 1 && v0.queue_value.maximum == 1000000, "queue limit defaults 2000 in 1..1,000,000");
    r.check(v0.overflow_items.size() == 6, "six overflow choices");

    // ---- load
    controller_->open_file(U(fixture));
    after_action();
    bool loaded = wait_until([&] { return controller_->has_song(); }, 20000);
    r.check(loaded, "file loads");
    handle_wake();
    const app::PlayerView& v1 = controller_->view();
    r.check(v1.play_enabled, "Play enabled after load");
    r.check(!v1.file_info.empty(), "file information shown");
    r.check(v1.duration_us > 0, "duration known");
    r.check(IsWindowEnabled(play_) != FALSE, "Play button enabled");

    // ---- a file dropped on any control loads (WM_DROPFILES is forwarded to the main window)
    {
        size_t bytes = sizeof(DROPFILES) + (fixture.size() + 2) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GHND, bytes);
        DROPFILES* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        wchar_t* names = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES));
        for (size_t i = 0; i < fixture.size(); ++i) names[i] = fixture[i];
        GlobalUnlock(memory);
        controller_->open_file(U(fixture));   // replace with the same file first so the drop has something to replace
        wait_until([&] { return controller_->has_song(); }, 20000);
        SendMessageW(stats_, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
        r.check(controller_->loading(), "dropping a MIDI file on a control starts loading");
        r.check(wait_until([&] { return controller_->has_song(); }, 20000), "the dropped file finishes loading");
        handle_wake();
    }

    // ---- the large-file warning: Continue loads, Cancel does not
    {
        controller_->set_force_large_file_checks_for_tests(true, true);
        SetTimer(window_, 6, 100, nullptr);
        test_dialog_answer_ = 1;
        test_dialog_seen_ = false;
        controller_->open_file(U(fixture));
        r.check(wait_until([&] { return controller_->has_song(); }, 20000), "Continue at the large-file warning loads the file");
        r.check(test_dialog_seen_, "the large-file warning was shown");
        test_dialog_answer_ = 2;
        test_dialog_seen_ = false;
        controller_->open_file(U(fixture));
        wait_until([&] { return !controller_->loading(); }, 20000);
        handle_wake();
        r.check(test_dialog_seen_ && !controller_->has_song(), "Cancel at the large-file warning loads nothing");
        controller_->take_messages();
        KillTimer(window_, 6);
        test_dialog_answer_ = 0;
        controller_->set_force_large_file_checks_for_tests(false, false);
        controller_->open_file(U(fixture));
        r.check(wait_until([&] { return controller_->has_song(); }, 20000), "the file loads again without the warning");
        handle_wake();
    }

    // ---- layout sanity at several widths, scales and styles
    struct Case { int scale; int width; int borderless; };
    // P12-CHK: every frame style (-1 = the native frame) in both layouts (790 standard, 560 compact) at 100, 75 and 50 %
    // and at the other scales, so the Direct WinMM checkbox and the playback state text are checked in every layout.
    std::vector<Case> cases;
    {
        const Case extra[] = { { 100, 600, -1 }, { 100, 450, -1 }, { 150, 1100, -1 }, { 50, 500, -1 }, { 75, 420, -1 }, { 200, 1500, -1 },
            { 125, 790, -1 }, { 125, 560, -1 }, { 150, 560, 1 }, { 200, 560, -1 }, { 200, 790, 1 } };
        const int scales[] = { 100, 75, 50 };
        for (size_t si = 0; si < 3; ++si)
            for (int style = -1; style <= 2; ++style) {
                Case std_ = { scales[si], 790, style }, compact = { scales[si], 560, style };
                cases.push_back(std_);
                cases.push_back(compact);
            }
        for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) cases.push_back(extra[i]);
    }
    for (size_t c = 0; c < cases.size(); ++c) {
        const Case& k = cases[c];
        if (borderless_) set_borderless(false);
        set_scale(k.scale);
        if (k.borderless >= 0) { set_borderless_style(static_cast<BorderlessStyle>(k.borderless)); set_borderless(true); }
        // set the client width
        RECT w, cl;
        GetWindowRect(window_, &w);
        GetClientRect(window_, &cl);
        int nc = (w.right - w.left) - cl.right;
        int target = std::max(ctx_.px(416) + 1, ctx_.px(k.width) / std::max(1, ctx_.scale_percent()) * 100 / std::max(1, 100));
        (void)target;
        int clientWidth = k.width * ctx_.dpi() / 96 * k.scale / 100;
        SetWindowPos(window_, nullptr, 0, 0, clientWidth + nc, w.bottom - w.top, SWP_NOMOVE | SWP_NOZORDER);
        pump(60);
        relayout();
        GetClientRect(window_, &cl);
        std::string label = "scale " + std::to_string(k.scale) + " width " + std::to_string(k.width) + " style " + std::to_string(k.borderless);
        bool inside = true, noOverlap = true;
        std::vector<HWND> ids;
        HWND visibleControls[] = { open_, file_label_, output_label_, output_, direct_, kdmapi_, state_, queue_check_, queue_value_, overflow_, rate_, slider_,
            timeline_, play_, stop_, back5_, forward5_, analysis_, reset_, stats_ };
        std::vector<RECT> rects;
        std::string insideWho, overlapWho;
        for (size_t i = 0; i < sizeof(visibleControls) / sizeof(visibleControls[0]); ++i) {
            HWND h = visibleControls[i];
            if (!own_visible_style(h)) continue;
            RECT rr = rect_in(window_, h);
            if (rr.left < 0 || rr.top < 0 || rr.right > cl.right || rr.bottom > cl.bottom) { inside = false; insideWho += std::to_string(i) + " "; }
            rects.push_back(rr);
            ids.push_back(h);
        }
        for (size_t a = 0; a < rects.size(); ++a)
            for (size_t b = a + 1; b < rects.size(); ++b) {
                // The combo boxes' rectangles include only the closed height; labels are skipped.
                if (overlap(rects[a], rects[b])) { noOverlap = false; overlapWho += std::to_string(a) + "/" + std::to_string(b) + " "; }
            }
        {   // P12-CHK: the checkbox and the playback state text fit their rectangles (measured, never clipped) and keep their places
            RECT dr = rect_in(window_, direct_), kr = rect_in(window_, kdmapi_), orr = rect_in(window_, output_), sr = rect_in(window_, state_);
            RECT pg = rect_in(window_, group_[2]);
            int widest = 0;
            for (const wchar_t* word : { L"Stopped", L"Playing", L"Paused", L"Completed" }) widest = std::max(widest, ctx_.text_width(word));
            r.check(own_visible_style(direct_) && ctx_.text_width(L"Direct WinMM") + ctx_.check_box_space() <= dr.right - dr.left &&
                    dr.right <= kr.left && dr.left >= orr.right && dr.top < kr.bottom && dr.bottom > kr.top,
                "Direct WinMM checkbox fits, left of KDMAPI, clear of the output list: " + label);
            r.check(own_visible_style(state_) && widest <= sr.right - sr.left && sr.top >= pg.top && sr.bottom <= pg.top + ctx_.px(40) &&
                    sr.left > pg.left + ctx_.text_width(L"Playback") + ctx_.px(10) && sr.right <= pg.right,
                "playback state text fits on the Playback heading line, clear of the heading: " + label);
        }
        r.check(inside, "controls inside client: " + label + " [" + insideWho + "]");
        r.check(noOverlap, "controls do not overlap: " + label + " [" + overlapWho + "]");
        RECT w2;
        GetWindowRect(window_, &w2);
        r.check(w2.bottom - w2.top == required_outer_height_, "window height equals the required height: " + label);
        {
            // P11-DS: the file information is either fully visible (every line's measured width fits its rectangle) or
            // moved into the file name's tooltip (and hidden); it is never clipped.
            RECT ir = rect_in(window_, info_label_);
            wchar_t text[512] = L"";
            GetWindowTextW(info_label_, text, 512);
            std::wstring t = text;
            int widest = 0;
            for (size_t cut = 0; cut <= t.size();) {
                size_t next = t.find_first_of(L"\r\n", cut);
                widest = std::max(widest, ctx_.text_width(t.substr(cut, next == std::wstring::npos ? std::wstring::npos : next - cut)));
                if (next == std::wstring::npos) break;
                cut = next + 1;
            }
            bool shown = own_visible_style(info_label_);
            r.check(t.empty() || (shown && widest <= ir.right - ir.left) || (!shown && info_in_file_tooltip_),
                "file information fully visible or in the file name's tooltip: " + label);
        }
    }
    if (borderless_) set_borderless(false);
    set_scale(100);
    set_borderless_style(BorderlessStyle::ClassicStrip);

    // ---- Rate model choices
    for (int i = 0; i < app::kRateModelChoiceCount; ++i) {
        SendMessageW(rate_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
        SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IdRate, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate_));
        const app::PlayerView& v = controller_->view();
        r.check(v.rate_choice == rate_index_to_choice(i), "rate model " + std::to_string(i) + " selected");
        r.check((v.processing_value.visible) == (i != 0), "rate model " + std::to_string(i) + " value field visibility");
    }
    SendMessageW(rate_, CB_SETCURSEL, 0, 0);
    SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IdRate, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate_));

    // ---- typing into the numeric fields (click, type, Enter) and the arrow keys
    auto type_into = [&](HWND field, const wchar_t* text) {
        SendMessageW(field, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
        SendMessageW(field, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        SetWindowTextW(field, text);
        SendMessageW(field, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(field, WM_CHAR, VK_RETURN, 0);
    };
    SendMessageW(queue_check_, BM_CLICK, 0, 0);
    r.check(controller_->view().queue_value.enabled, "queue limit check enables the value field");
    type_into(queue_value_, L"500");
    r.check(controller_->view().queue_value.value == 500 && controller_->engine().queue_length_limit() == 500, "typing 500 sets the queue limit");
    type_into(queue_value_, L"99999999");
    r.check(controller_->view().queue_value.value == 500, "an out-of-range value is rejected");
    SendMessageW(queue_value_, WM_KEYDOWN, VK_UP, 0);
    r.check(controller_->view().queue_value.value == 600, "Up arrow adds 100");
    SendMessageW(queue_check_, BM_CLICK, 0, 0);
    SendMessageW(rate_, CB_SETCURSEL, 1, 0);
    SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IdRate, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate_));
    type_into(processing_value_, L"250");
    r.check(controller_->engine().processing_microseconds() == 250, "typing 250 sets the processing time");
    r.check(controller_->view().slider_value == app::microseconds_to_slider(250), "the slider follows the typed value");
    SendMessageW(slider_, TBM_SETPOS, TRUE, 6000);
    SendMessageW(window_, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 6000), reinterpret_cast<LPARAM>(slider_));
    r.check(controller_->engine().processing_microseconds() == app::slider_to_microseconds(6000), "the slider sets the processing time");
    SendMessageW(rate_, CB_SETCURSEL, 0, 0);
    SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IdRate, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate_));

    // ---- play / pause / resume / stop on output None
    SendMessageW(output_, CB_SETCURSEL, 0, 0);
    SendMessageW(window_, WM_COMMAND, MAKEWPARAM(IdOutput, CBN_SELCHANGE), reinterpret_cast<LPARAM>(output_));
    // Fail closed: never press Play unless the output reads back as None (and KDMAPI is off); a failure here
    // skips the whole play part, so a test can never make sound through a real device.
    const bool outputIsNone = controller_->output_selection() == 0 && !controller_->view().kdmapi_checked;
    r.check(outputIsNone, "output None selected (Play is skipped otherwise)");
    if (outputIsNone) {
    SendMessageW(play_, BM_CLICK, 0, 0);
    pump(250);
    app::PlayerView vp = controller_->view();
    r.check(vp.state_text == "Playing" || vp.state_text == "Completed", "Play starts playback (state " + vp.state_text + ")");
    if (vp.state_text == "Playing") {
        r.check(vp.play_text == "Pause", "Play button reads Pause while playing");
        SendMessageW(play_, BM_CLICK, 0, 0);
        pump(50);
        r.check(controller_->view().state_text == "Paused", "Pause pauses");
        SendMessageW(play_, BM_CLICK, 0, 0);
        pump(50);
        r.check(controller_->view().state_text == "Playing" || controller_->view().state_text == "Completed", "Resume resumes");
    }
    int64_t seekTarget = std::max<int64_t>(0, controller_->view().duration_us / 2);
    SendMessageW(window_, WM_MIDIBN_SEEK, IdTimeline, reinterpret_cast<LPARAM>(&seekTarget));
    pump(50);
    r.check(std::llabs(controller_->selected_position() - seekTarget) < 2000000 || controller_->view().state_text == "Completed", "seek moves the position");
    SendMessageW(stop_, BM_CLICK, 0, 0);
    pump(50);
    r.check(controller_->view().state_text == "Stopped", "Stop stops");
    r.check(controller_->selected_position() == 0, "Stop returns to the start");
    }

    // ---- system menu options
    struct Toggle { UINT command; const char* name; };
    Toggle toggles[] = { { CmdChase, "chase" }, { CmdPreserve, "preserve queue" }, { CmdOutputPaced, "output-paced timeline" },
        { CmdScaleInterval, "scale interval by traffic" }, { CmdApplyQueueLimit, "apply queue limit without slowdown" }, { CmdQueueAge, "queue age limit" } };
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); ++i) {
        app::SystemMenuState before = controller_->view().menu;
        SendMessageW(window_, WM_SYSCOMMAND, toggles[i].command, 0);
        app::SystemMenuState after = controller_->view().menu;
        r.check(!(before == after), std::string("system menu toggles: ") + toggles[i].name);
        SendMessageW(window_, WM_SYSCOMMAND, toggles[i].command, 0);
        r.check(before == controller_->view().menu, std::string("system menu toggles back: ") + toggles[i].name);
    }
    {   // DD-112 (decision): the yield option exists only on a one-physical-core PC (or with --force-one-core) and starts off.
        HMENU sys = GetSystemMenu(window_, FALSE);
        UINT st = GetMenuState(sys, CmdFewCpuYield, MF_BYCOMMAND);
        r.check((st != static_cast<UINT>(-1)) == midibn::win32::few_cpu_yield_option_available(), "few-processor yield menu item present exactly when offered");
        if (st != static_cast<UINT>(-1)) {
            // Remembered-settings rule: on at the start only with exactly one usable logical processor (nothing is read in the self-test)
            const bool expectOn = midibn::win32::usable_logical_processors() == 1;
            r.check(((st & MF_CHECKED) != 0) == expectOn, "few-processor yield option starts at its automatic default (on only with one usable logical processor)");
            SendMessageW(window_, WM_SYSCOMMAND, CmdFewCpuYield, 0);
            r.check(((GetMenuState(sys, CmdFewCpuYield, MF_BYCOMMAND) & MF_CHECKED) != 0) == !expectOn, "few-processor yield option toggles");
            SendMessageW(window_, WM_SYSCOMMAND, CmdFewCpuYield, 0);
            r.check(((GetMenuState(sys, CmdFewCpuYield, MF_BYCOMMAND) & MF_CHECKED) != 0) == expectOn, "few-processor yield option toggles back");
        }
    }
    // ---- P12-CHK: the "Direct WinMM" checkbox (it replaced the system-menu item and the status text)
    {
        HMENU sys = GetSystemMenu(window_, FALSE);
        bool oldItem = false;
        for (int i = 0; i < GetMenuItemCount(sys); ++i) {
            wchar_t text[256] = L"";
            GetMenuStringW(sys, static_cast<UINT>(i), text, 256, MF_BYPOSITION);
            if (std::wstring(text).find(L"Direct WinMM") != std::wstring::npos || std::wstring(text).find(L"ModMessage") != std::wstring::npos) oldItem = true;
        }
        r.check(!oldItem, "the system menu no longer has a Direct WinMM item");
        r.check(IsWindow(direct_) && own_visible_style(direct_), "Direct WinMM checkbox exists and is visible");
        wchar_t label[64] = L"";
        GetWindowTextW(direct_, label, 64);
        r.check(std::wstring(label) == L"Direct WinMM", "Direct WinMM checkbox text");
        r.check(SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_UNCHECKED && !controller_->direct_winmm(), "Direct WinMM starts off");
        r.check(!IsWindowEnabled(direct_) && !controller_->view().direct_enabled, "output None: Direct WinMM is greyed out");
        r.check(tooltip_for(direct_).find(L"output is None") != std::wstring::npos, "output None: the tooltip says why Direct mode is not in use");
        r.check(GetWindow(output_, GW_HWNDNEXT) == direct_ && GetWindow(direct_, GW_HWNDNEXT) == kdmapi_, "tab order: output list, Direct WinMM, KDMAPI");
        RECT d = rect_in(window_, direct_), k = rect_in(window_, kdmapi_), o = rect_in(window_, output_);
        r.check(d.right <= k.left && d.left >= o.right && d.top < k.bottom && d.bottom > k.top, "Direct WinMM sits between the output list and KDMAPI, on the same row");
        r.check(ctx_.text_width(L"Direct WinMM") + ctx_.check_box_space() <= d.right - d.left, "Direct WinMM text fits its checkbox");
        // what ticking does: the same as the old menu item (the controller flips its session-only choice)
        if (controller_->view().output_items.size() > 1) {
            // R1: greying is decided when the selection changes, with stand-ins for the check (the real one depends on this PC's drivers)
            auto standin = [&](int mode) {
                MainWindow::direct_probe_standin = mode;
                controller_->forget_direct_checks();
                for (int i = 0; i < 300 && controller_->direct_check_pending(); ++i) pump(10);
                pump(20);
                after_action();
            };
            controller_->select_output(1);   // nothing is opened by selecting
            after_action();
            standin(1);   // a stand-in "Windows' own driver" device
            r.check(!IsWindowEnabled(direct_) && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "selecting a device that is Windows' own driver: Direct WinMM greyed out at once (no Play)");
            r.check(tooltip_for(direct_).find(L"cannot be used with") != std::wstring::npos && tooltip_for(direct_).find(L"Windows' own MIDI driver") != std::wstring::npos, "that device: the tooltip says why");
            controller_->toggle_direct_winmm();
            after_action();
            r.check(!IsWindowEnabled(direct_) && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_CHECKED, "Windows' own driver with the tick set: greyed out, tick kept");
            controller_->toggle_direct_winmm();
            standin(2);   // a stand-in usable driver
            r.check(IsWindowEnabled(direct_) && controller_->view().direct_enabled, "selecting a device with a usable driver: Direct WinMM enabled");
            controller_->select_output(0);
            after_action();
            r.check(!IsWindowEnabled(direct_), "output None: greyed out");
            controller_->select_output(1);
            after_action();
            r.check(IsWindowEnabled(direct_), "back to the usable device: enabled again (cached answer)");
            r.check(IsWindowEnabled(direct_) && controller_->view().direct_enabled, "a MIDI device selected: Direct WinMM is enabled");
            r.check(tooltip_for(direct_).find(L"Direct mode is off") != std::wstring::npos, "enabled and off: the tooltip explains the option");
            SendMessageW(direct_, BM_CLICK, 0, 0);
            pump(40);
            r.check(controller_->direct_winmm() && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_CHECKED, "clicking the checkbox turns Direct WinMM on");
            r.check(tooltip_for(direct_).find(L"checks the selected device when you press Play") != std::wstring::npos, "on, not yet played: the tooltip says the device is checked at Play");
            controller_->set_kdmapi(true);
            after_action();
            r.check(!IsWindowEnabled(direct_) && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_CHECKED, "KDMAPI ticked: Direct WinMM greyed out, tick kept");
            r.check(tooltip_for(direct_).find(L"KDMAPI is ticked") != std::wstring::npos, "KDMAPI ticked: the tooltip says why");
            controller_->set_kdmapi(false);
            controller_->select_output(0);
            after_action();
            r.check(!IsWindowEnabled(direct_) && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_CHECKED, "output None: Direct WinMM greyed out, tick kept");
            controller_->select_output(1);
            after_action();
            SendMessageW(direct_, BM_CLICK, 0, 0);
            pump(40);
            r.check(!controller_->direct_winmm() && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "clicking again turns Direct WinMM off");
            MainWindow::direct_probe_standin = 0;
            controller_->forget_direct_checks();
            controller_->select_output(0);
            after_action();
        }
        else {
            controller_->toggle_direct_winmm();   // no MIDI device in this PC's list: exercise the controller path only
            after_action();
            r.check(controller_->direct_winmm() && SendMessageW(direct_, BM_GETCHECK, 0, 0) == BST_CHECKED && !IsWindowEnabled(direct_), "no MIDI device: ticked state shown, greyed out");
            controller_->toggle_direct_winmm();
            after_action();
        }
        // the playback state text sits on the Playback frame's heading line, at the right end, in the standard layout
        RECT s = rect_in(window_, state_), pg = rect_in(window_, group_[2]);
        r.check(own_visible_style(state_) && s.top >= pg.top && s.bottom <= pg.top + ctx_.px(40) && pg.right - s.right <= ctx_.px(12) &&
                s.left > pg.left + ctx_.text_width(L"Playback") + ctx_.px(10), "the playback state sits at the right end of the Playback heading line");
    }

    // ---- DD-090: a width change repaints every control whose size changed completely (a resize used to leave the
    // old pixels of right-aligned labels, the statistics columns and frame lines behind; only the uncovered strip was
    // repainted). The update region is read right after the change, before anything is painted.
    if (IsWindowVisible(window_)) {
        HWND sized[] = { file_label_, info_label_, state_, stats_, group_[0], group_[1], group_[2], group_[3] };
        const char* names[] = { "file name", "file info", "state", "statistics", "File and output frame", "Processing model frame",
            "Playback frame", "Statistics frame" };
        for (int step = 0; step < 2; ++step) {
            RedrawWindow(window_, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);   // everything painted and valid
            RECT before[8];
            for (int i = 0; i < 8; ++i) before[i] = rect_in(window_, sized[i]);
            RECT w;
            GetWindowRect(window_, &w);
            int delta = step == 0 ? -ctx_.px(70) : ctx_.px(70);   // shrink, then grow back
            SetWindowPos(window_, nullptr, 0, 0, (w.right - w.left) + delta, w.bottom - w.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (int i = 0; i < 8; ++i) {
                RECT now = rect_in(window_, sized[i]);
                if (!own_visible_style(sized[i]) || now.right - now.left == before[i].right - before[i].left) continue;
                RECT client, update;
                GetClientRect(sized[i], &client);
                GetUpdateRect(sized[i], &update, FALSE);
                r.check(EqualRect(&client, &update) != FALSE, std::string(step == 0 ? "shrinking" : "growing") + " the window repaints the whole " +
                    names[i] + " (update " + std::to_string(update.left) + "," + std::to_string(update.top) + "-" + std::to_string(update.right) + "," +
                    std::to_string(update.bottom) + " of " + std::to_string(client.right) + "x" + std::to_string(client.bottom) + ")");
            }
            pump(40);
        }
    }
    SendMessageW(window_, WM_SYSCOMMAND, CmdShowProcessing, 0);
    r.check(!show_processing_ && !IsWindowVisible(rate_), "Processing model can be hidden");
    SendMessageW(window_, WM_SYSCOMMAND, CmdShowProcessing, 0);
    r.check(show_processing_ && IsWindowVisible(rate_), "Processing model can be shown again");
    SendMessageW(window_, WM_SYSCOMMAND, CmdShowStatistics, 0);
    r.check(!show_statistics_ && !IsWindowVisible(stats_), "Statistics can be hidden");
    SendMessageW(window_, WM_SYSCOMMAND, CmdShowStatistics, 0);
    r.check(show_statistics_ && IsWindowVisible(stats_), "Statistics can be shown again");

    // ---- borderless hit testing
    for (int style = 0; style < 3; ++style) {
        set_borderless_style(static_cast<BorderlessStyle>(style));
        set_borderless(true);
        pump(40);
        RECT w;
        GetWindowRect(window_, &w);
        std::string label = " (style " + std::to_string(style) + ")";
        auto hit = [&](int x, int y) { return static_cast<int>(SendMessageW(window_, WM_NCHITTEST, 0, MAKELPARAM(x, y))); };
        r.check(hit(w.left + 1, w.top + (w.bottom - w.top) / 2) == HTLEFT, "left edge resizes" + label);
        r.check(hit(w.right - 2, w.top + (w.bottom - w.top) / 2) == HTRIGHT, "right edge resizes" + label);
        r.check(hit(w.left + 1, w.top + 1) == HTTOPLEFT, "top-left corner resizes" + label);
        r.check(hit(w.right - 2, w.bottom - 2) == HTBOTTOMRIGHT, "bottom-right corner resizes" + label);
        r.check(hit(w.left + (w.right - w.left) / 2, w.bottom - 2) == HTBOTTOM, "bottom edge resizes" + label);
        // children covering the edge must pass the hit test through
        HWND probe = stats_;
        RECT cr;
        GetWindowRect(probe, &cr);
        int edge = ctx_.px(6);
        if (cr.left - w.left < edge) {
            int childHit = static_cast<int>(SendMessageW(probe, WM_NCHITTEST, 0, MAKELPARAM(w.left + 1, cr.top + 5)));
            r.check(childHit == HTTRANSPARENT, "child covering the edge passes the hit test through" + label);
        }
        HWND buttonProbe = play_;
        GetWindowRect(buttonProbe, &cr);
        r.check(static_cast<int>(SendMessageW(buttonProbe, WM_NCHITTEST, 0, MAKELPARAM(cr.left + 3, cr.top + 3))) != HTTRANSPARENT,
            "a button away from the edge keeps its own hit test" + label);
        if (style == 0) r.check(hit(w.left + 300, w.top + ctx_.px(15)) == HTCAPTION, "strip drags the window" + label);
        if (style == 1) {
            RECT row = file_row_rect_;
            POINT p = { row.left + (row.right - row.left) / 2, row.top + 2 };
            ClientToScreen(window_, &p);
            r.check(hit(p.x, p.y) == HTCAPTION, "header row drags the window" + label);
        }
        set_borderless(false);
    }
    set_borderless_style(BorderlessStyle::ClassicStrip);

    // ---- the companion windows open from the main window and close with it
    {
        SendMessageW(analysis_, BM_CLICK, 0, 0);
        pump(300);
        r.check(analysis_windows_.size() == 1 && IsWindow(analysis_windows_[0]->hwnd()), "Analysis opens from the Analysis button");
        wait_until([&] { return IsWindow(analysis_windows_.empty() ? nullptr : analysis_windows_[0]->hwnd()) != FALSE; }, 1000);
        pump(1500);   // let the first calculation run
        SendMessageW(analysis_, BM_CLICK, 0, 0);
        pump(100);
        r.check(analysis_windows_.size() == 2, "a second Analysis window can be opened");
        open_channels();
        pump(300);
        r.check(channels_window_ && IsWindow(channels_window_->hwnd()), "Channels opens");
        open_channels();
        r.check(channels_window_ != nullptr, "opening Channels again reuses the window");
        // Track routing: the modal editor opens and is dismissed through the close box.
        SetTimer(window_, 6, 150, nullptr);
        test_dialog_answer_ = 3;
        test_dialog_seen_ = false;
        SendMessageW(window_, WM_SYSCOMMAND, CmdTrackRouting, 0);
        KillTimer(window_, 6);
        test_dialog_answer_ = 0;
        r.check(test_dialog_seen_, "Track routing editor opens from the system menu");
        r.check(!controller_->track_route(), "cancelling the Track routing editor leaves the routing unchanged");
        close_companions();
        pump(100);
        r.check(analysis_windows_.empty() && !channels_window_, "companion windows close");
    }

    // ---- effective-speed window: changes within the session (the remembered copy is checked by the unit tests and the settings round trip)
    {
        int64_t previous = controller_->effective_speed_window();
        controller_->set_effective_speed_window(500000);
        r.check(controller_->effective_speed_window() == 500000, "the effective-speed window can be changed within the session");
        controller_->set_effective_speed_window(previous);
        r.check(controller_->effective_speed_window() == previous, "the previous value is restored");
    }

    // ---- close path
    r.check(true, "reached the end of the checks");
    char summary[128];
    std::snprintf(summary, sizeof(summary), "SELFTEST: %s (%d checks, %d failures)\n", r.failures == 0 ? "PASS" : "FAIL", r.checks, r.failures);
    r.text += summary;
    if (!options_.result_file.empty()) {
        HANDLE file = CreateFileW(options_.result_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, r.text.data(), static_cast<DWORD>(r.text.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    return r.failures == 0 ? 0 : 1;
}

} // namespace ui
} // namespace midibn
