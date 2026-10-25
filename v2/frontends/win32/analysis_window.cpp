// The Analysis window (v1 DiagnosticsForm): plain Win32 + GDI around app::AnalysisWindowModel.
// Layout, wording and behaviour follow v1; every difference is listed in v2/docs/UI_DIFFERENCES.md (AD-nnn).
#include "analysis_window.h"

#include "controls.h"
#include "workload_graph.h"

#include "midibn/app/analysis_summary.h"
#include "midibn/format/display_format.h"

#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace midibn {
namespace ui {

namespace {

const wchar_t kAnalysisClass[] = L"MIDIBottleneckAnalysisV2";
const wchar_t kPromptClass[] = L"MidiBnAnalysisPrompt";

enum ControlId {
    IdResetZoom = 1001, IdFollowLabel, IdFollow, IdResolutionLabel, IdResolution, IdPlaybackStats, IdChannels, IdStatus, IdProgress,
    IdCancel, IdSummary, IdGraph, IdInspection, IdSeekPin
};

const UINT WM_ANALYSIS_WAKE = WM_APP + 41;
const UINT WM_ANALYSIS_SPACE = WM_APP + 42;
const UINT_PTR kTimerModel = 1;

// v1 system-menu command ids (DiagnosticsForm).
const UINT kMenuShowEventCounts = 0x1F20;
const UINT kMenuAlwaysOnTop = 0x1F30;
const UINT kMenuProjectForcedOverrides = 0x1F40;

// Canonical metrics (v1 DiagnosticsForm at 100%).
const int kDefaultClientWidth = 1080, kDefaultClientHeight = 660;
const int kMinimumWidth = 820, kMinimumHeight = 520;
const int kSplitterDistance = 390, kSplitterWidth = 6, kPanel1Minimum = 330, kPanel2Minimum = 360;
const int kInspectionHeight = 92;

class AnalysisWindow;
std::vector<AnalysisWindow*> g_windows;
HHOOK g_hook = nullptr;

std::wstring read_text(HWND window)
{
    int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

bool visible(HWND window) { return (GetWindowLongW(window, GWL_STYLE) & WS_VISIBLE) != 0; }

void show(HWND window, bool on)
{
    if (window && visible(window) != on) ShowWindow(window, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void enable(HWND window, bool on)
{
    if (window && (IsWindowEnabled(window) != FALSE) != on) EnableWindow(window, on ? TRUE : FALSE);
}

bool class_is(HWND window, const wchar_t* name)
{
    wchar_t buffer[64] = {};
    GetClassNameW(window, buffer, 64);
    return _wcsicmp(buffer, name) == 0;
}

// ---------------------------------------------------------------- the custom-resolution prompt

struct PromptState {
    UiContext* ctx = nullptr;
    app::AnalysisWindowModel* model = nullptr;
    format::NumberCulture culture;
    HWND edit = nullptr;
    bool done = false;
    bool accepted = false;
    int64_t initial_us = 100000;
};

std::wstring micros_to_text(int64_t microseconds, const format::NumberCulture& culture)
{
    std::string text = std::to_string(microseconds / 1000) + culture.decimal_separator;
    char fraction[8];
    std::snprintf(fraction, sizeof fraction, "%03d", static_cast<int>(microseconds % 1000));
    return W(text + fraction);
}

// The edit text as invariant decimal text clamped to 0.001 .. 86,400,000 (NumericUpDown's range); false when unreadable.
bool text_to_decimal(const std::wstring& wide, const format::NumberCulture& culture, std::string& out)
{
    std::string text = U(wide);
    std::string digits;
    bool point = false;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ' || text[i] == '\t') { ++i; continue; }
        if (!culture.decimal_separator.empty() && text.compare(i, culture.decimal_separator.size(), culture.decimal_separator) == 0) {
            if (point) return false;
            point = true;
            digits += '.';
            i += culture.decimal_separator.size();
            continue;
        }
        if (!culture.group_separator.empty() && text.compare(i, culture.group_separator.size(), culture.group_separator) == 0) {
            i += culture.group_separator.size();
            continue;
        }
        if (text[i] < '0' || text[i] > '9') return false;
        digits += text[i++];
    }
    if (digits.empty() || digits == ".") return false;
    double value = std::strtod(digits.c_str(), nullptr);
    if (value < 0.001) digits = "0.001";
    else if (value > 86400000.0) digits = "86400000";
    out = digits;
    return true;
}

LRESULT CALLBACK prompt_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    PromptState* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        break;
    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wParam) == IDOK) {
            std::string decimal;
            std::string error;
            if (!text_to_decimal(read_text(state->edit), state->culture, decimal)) {
                // NumericUpDown restores its last value when the text cannot be read.
                SetWindowTextW(state->edit, micros_to_text(state->initial_us, state->culture).c_str());
                SetFocus(state->edit);
                return 0;
            }
            if (state->model->apply_custom_resolution(decimal, error)) {
                state->accepted = true;
                state->done = true;
            } else {
                MessageBoxW(window, W(error).c_str(), W(app::kCustomResolutionInvalidTitle).c_str(), MB_OK | MB_ICONWARNING);
                SetFocus(state->edit);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            state->done = true;
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (state && header->code == UDN_DELTAPOS) {
            NMUPDOWN* delta = reinterpret_cast<NMUPDOWN*>(lParam);
            std::string decimal;
            double value = 100.0;
            if (text_to_decimal(read_text(state->edit), state->culture, decimal)) value = std::strtod(decimal.c_str(), nullptr);
            value += delta->iDelta < 0 ? 1.0 : -1.0;   // the arrow up gives iDelta < 0
            value = std::max(0.001, std::min(86400000.0, value));
            int64_t micros = static_cast<int64_t>(std::llround(value * 1000.0));
            SetWindowTextW(state->edit, micros_to_text(micros, state->culture).c_str());
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        if (state) state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// v1 PromptForCustomResolution. Returns true when a value was accepted by the model.
bool run_custom_prompt(HWND owner, UiContext& ctx, app::AnalysisWindowModel& model, const format::NumberCulture& culture)
{
    PromptState state;
    state.ctx = &ctx;
    state.model = &model;
    state.culture = culture;
    state.initial_us = model.resolution().custom_microseconds() > 0 ? model.resolution().custom_microseconds() : app::kCustomResolutionDefaultMicroseconds;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    // DD-099: the label and the note are at least as wide as their texts, what follows the label moves right by the
    // difference, the dialog grows to hold them and its title, and OK / Cancel stay at the right edge (at Application
    // scale 50 % the font shrinks less than the layout, so the texts were cut off).
    std::wstring label = W(app::kCustomResolutionLabel), noteText = W(app::kCustomResolutionNote), title = W(app::kCustomResolutionTitle);
    int labelW = std::max(ctx.px(130), ctx.text_width(label) + 4);
    int shift = labelW - ctx.px(130);
    int noteW = std::max(ctx.px(366), ctx.text_width(noteText) + 4);
    int clientW = std::max(std::max(ctx.px(390) + shift, ctx.px(12) + noteW + ctx.px(12)), caption_fit_client_width(title, style, WS_EX_DLGMODALFRAME));
    int right = clientW - ctx.px(390);   // how far the buttons move right
    RECT rect = { 0, 0, clientW, ctx.px(120) };
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_DLGMODALFRAME);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kPromptClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD childStyle, int x, int y, int w, int h, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | childStyle, x, y, w, h, dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ctx.ui_font()), TRUE);
        return c;
    };
    int fh = ctx.font_height();
    child(L"STATIC", label.c_str(), SS_LEFT | SS_NOPREFIX, ctx.px(12), ctx.px(20), labelW, fh + 4, 0);
    state.edit = child(L"EDIT", micros_to_text(state.initial_us, culture).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, ctx.px(145) + shift, ctx.px(17),
        ctx.px(135), fh + ctx.px(6), 10, WS_EX_CLIENTEDGE);
    HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(11)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(state.edit), 0);
    child(L"STATIC", L"ms", SS_LEFT | SS_NOPREFIX, ctx.px(286) + shift, ctx.px(20), ctx.px(30), fh + 4, 0);
    HWND note = child(L"STATIC", noteText.c_str(), SS_LEFT | SS_NOPREFIX, ctx.px(12), ctx.px(50), noteW, fh + 4, 0);
    (void)note;
    int bh = fh + ctx.px(8);
    child(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, ctx.px(218) + right, ctx.px(120) - bh - ctx.px(14), ctx.px(75), bh, IDOK);
    child(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ctx.px(299) + right, ctx.px(120) - bh - ctx.px(14), ctx.px(75), bh, IDCANCEL);
    // v1 CenterParent: centred over the Analysis window, inside the work area of its monitor (DD-098; it used to be
    // centred without a monitor check and only kept right of and below the point 0,0).
    RECT outer;
    GetWindowRect(dialog, &outer);
    POINT at = centre_on_owner(owner, outer.right - outer.left, outer.bottom - outer.top);
    SetWindowPos(dialog, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetFocus(state.edit);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);
    bool ownerWasEnabled = IsWindowEnabled(owner) != FALSE;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG message;
    while (!state.done) {
        BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (ownerWasEnabled) EnableWindow(owner, TRUE);
    DestroyWindow(dialog);
    SetForegroundWindow(owner);
    return state.accepted;
}

// ---------------------------------------------------------------- the window

class AnalysisWindow : public AnalysisCompanion {
public:
    explicit AnalysisWindow(CompanionHost& host) : host_(host), ctx_(host.ctx)
    {
        wake_target_ = std::make_shared<std::atomic<HWND>>(static_cast<HWND>(nullptr));
        app::AnalysisWindowHost modelHost;
        CompanionHost copy = host;
        modelHost.seek = [copy](int64_t t) {
            if (copy.analysis_seek) copy.analysis_seek(t);
        };
        std::shared_ptr<std::atomic<HWND>> target = wake_target_;
        modelHost.wake = [target] {
            HWND w = target->load();
            if (w) PostMessageW(w, WM_ANALYSIS_WAKE, 0, 0);
        };
        model_.reset(new app::AnalysisWindowModel(*host.controller, modelHost));
        culture_ = host.controller->culture();
    }

    ~AnalysisWindow() override
    {
        if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
        model_.reset();
    }

    bool create()
    {
        double factor = ctx_->scale_percent() / 100.0 * ctx_->dpi() / 96.0;
        canonical_client_width_ = kDefaultClientWidth;
        canonical_client_height_ = kDefaultClientHeight;
        canonical_split_ = kSplitterDistance;
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT work = work_area();
        RECT rect = { 0, 0, std::max(1, static_cast<int>(std::lround(kDefaultClientWidth * factor))),
            std::max(1, static_cast<int>(std::lround(kDefaultClientHeight * factor))) };
        AdjustWindowRectEx(&rect, style, FALSE, 0);
        int width = std::min<int>(rect.right - rect.left, work.right - work.left);
        int height = std::min<int>(rect.bottom - rect.top, work.bottom - work.top);
        // v1: beside the main window (right edge + 12), else over its left part, inside the work area.
        RECT main = work;
        if (host_.main_window) GetWindowRect(host_.main_window, &main);
        int x = main.right + 12;
        if (x + width > work.right) x = std::max<int>(work.left, main.left + 36);
        int y = std::max<int>(work.top, std::min<int>(work.bottom - height, main.top));
        // DD-098: also never past the right edge of the work area (v1 could leave the window partly off its screen
        // when the main window was near the right edge).
        POINT at = keep_in_work_area(x, y, width, height, work);
        x = at.x;
        y = at.y;
        hwnd_ = CreateWindowExW(0, kAnalysisClass, L"MIDIBottleneck Player \x2014 Analysis", style, x, y, width, height, nullptr, nullptr,
            GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;
        g_windows.push_back(this);
        if (!g_hook) g_hook = SetWindowsHookExW(WH_GETMESSAGE, &AnalysisWindow::get_message_hook, nullptr, GetCurrentThreadId());
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        model_->start();
        sync();
        return true;
    }

    // ---- CompanionWindow
    HWND hwnd() const override { return hwnd_; }
    void on_refresh() override
    {
        if (!hwnd_ || !IsWindow(hwnd_)) return;
        model_->on_refresh();
        sync();
    }
    void on_scale_changed() override
    {
        if (!hwnd_ || !IsWindow(hwnd_)) return;
        rebuild_fonts();
        apply_summary();
        workload_graph_scale_changed(graph_);
        measure_combo_height();
        double factor = ctx_->scale_percent() / 100.0 * ctx_->dpi() / 96.0;
        RECT work = work_area();
        RECT window, client;
        GetWindowRect(hwnd_, &window);
        GetClientRect(hwnd_, &client);
        int ncW = (window.right - window.left) - client.right, ncH = (window.bottom - window.top) - client.bottom;
        int w = std::min<int>(std::max(1, static_cast<int>(std::lround(canonical_client_width_ * factor))), (work.right - work.left) - ncW);
        int h = std::min<int>(std::max(1, static_cast<int>(std::lround(canonical_client_height_ * factor))), (work.bottom - work.top) - ncH);
        int x = std::max<int>(work.left, std::min<int>(window.left, work.right - (w + ncW)));
        int y = std::max<int>(work.top, std::min<int>(window.top, work.bottom - (h + ncH)));
        applying_scale_ = true;
        SetWindowPos(hwnd_, nullptr, x, y, w + ncW, h + ncH, SWP_NOZORDER | SWP_NOACTIVATE);
        split_px_ = static_cast<int>(std::lround(canonical_split_ * factor));
        layout();
        applying_scale_ = false;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    void close() override
    {
        if (model_) model_->close();
        if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    }

    // ---- AnalysisCompanion
    app::AnalysisWindowModel& model() override { return *model_; }
    HWND graph_window() const override { return graph_; }
    HWND summary_window() const override { return summary_; }
    HWND inspection_window() const override { return inspection_; }
    HWND seek_button() const override { return seek_; }
    bool using_rich_edit() const override { return rich_; }
    bool always_on_top() const override { return topmost_; }
    void process_pending() override
    {
        model_->poll();
        sync();
    }

private:
    friend LRESULT CALLBACK analysis_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK get_message_hook(int code, WPARAM wParam, LPARAM lParam);

    // ---- helpers
    RECT work_area() const
    {
        // v1 Screen.FromControl(this): the window's own monitor once it exists (a scale change keeps it where the user
        // put it), the main window's before (DD-098).
        return monitor_work_area(hwnd_ && IsWindow(hwnd_) ? hwnd_ : host_.main_window);
    }

    int px(int canonical) const { return ctx_->px(canonical); }

    HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0)
    {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ctx_->ui_font()), TRUE);
        attach_scaled_button(c, ctx_);   // P10-UI1: check boxes draw at the Application scale (no-op for other controls)
        return c;
    }

    void rebuild_fonts()
    {
        if (inspection_font_) DeleteObject(inspection_font_);
        LOGFONTW logfont = {};
        GetObjectW(ctx_->mono_font(), sizeof(logfont), &logfont);
        logfont.lfHeight = -std::max(1, static_cast<int>(std::lround(std::abs(logfont.lfHeight) * 8.5 / 9.0)));   // Consolas 8.5 pt
        inspection_font_ = CreateFontIndirectW(&logfont);
        HWND uiControls[] = { reset_, follow_label_, follow_, resolution_label_, resolution_, stats_check_, channels_, status_, cancel_, seek_ };
        for (size_t i = 0; i < sizeof(uiControls) / sizeof(uiControls[0]); ++i)
            if (uiControls[i]) SendMessageW(uiControls[i], WM_SETFONT, reinterpret_cast<WPARAM>(ctx_->ui_font()), TRUE);
        SendMessageW(summary_, WM_SETFONT, reinterpret_cast<WPARAM>(ctx_->mono_font()), TRUE);
        SendMessageW(inspection_, WM_SETFONT, reinterpret_cast<WPARAM>(inspection_font_), TRUE);
        summary_applied_.clear();   // a new font drops the heading formats: apply_summary() draws them again
    }

    void measure_combo_height()
    {
        RECT r;
        GetWindowRect(follow_, &r);
        combo_height_ = r.bottom - r.top;
    }

    // ---- creation of the child windows
    void build_controls()
    {
        reset_ = make(L"BUTTON", L"Reset zoom", BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE, IdResetZoom);
        follow_label_ = make(L"STATIC", L"Follow:", SS_LEFT | SS_NOPREFIX | WS_VISIBLE, IdFollowLabel);
        follow_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_VISIBLE, IdFollow);
        SendMessageW(follow_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
        SendMessageW(follow_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Playback timeline"));
        SendMessageW(follow_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MIDI output"));
        SendMessageW(follow_, CB_SETCURSEL, 0, 0);
        resolution_label_ = make(L"STATIC", L"Resolution:", SS_LEFT | SS_NOPREFIX | WS_VISIBLE, IdResolutionLabel);
        resolution_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_VISIBLE, IdResolution);
        stats_check_ = make(L"BUTTON", L"Playback statistics", BS_AUTOCHECKBOX | WS_TABSTOP | WS_VISIBLE, IdPlaybackStats);
        channels_ = make(L"BUTTON", L"Channels\x2026", BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE, IdChannels);
        status_ = make(L"STATIC", L"Recalculating analysis\x2026", SS_LEFT | SS_NOPREFIX, IdStatus);
        progress_ = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_CLIPSIBLINGS | PBS_SMOOTH, 0, 0, 10, 10, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdProgress)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
        cancel_ = make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IdCancel);

        // The report: RichEdit 4.1 (Msftedit.dll) when present, a plain read-only edit control otherwise.
        HMODULE rich = LoadLibraryW(L"Msftedit.dll");
        if (rich) {
            summary_ = CreateWindowExW(0, L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_BORDER | WS_VSCROLL | WS_TABSTOP |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSummary)),
                GetModuleHandleW(nullptr), nullptr);
            rich_ = summary_ != nullptr;
        }
        if (!summary_) {
            summary_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_BORDER | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSummary)), GetModuleHandleW(nullptr), nullptr);
            rich_ = false;
        }
        if (rich_) SendMessageW(summary_, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
        graph_ = create_workload_graph(hwnd_, IdGraph, ctx_, model_.get());
        inspection_ = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_TABSTOP | WS_VISIBLE, IdInspection, WS_EX_CLIENTEDGE);
        seek_ = make(L"BUTTON", L"Seek to pin", BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE, IdSeekPin);
        rebuild_fonts();
        measure_combo_height();
        tooltips_.create(hwnd_);
        tooltips_.set_maximum_width(420);
        tooltips_.add_control(resolution_);
        SetWindowTextW(inspection_, W(app::kInspectionPlaceholder).c_str());
        inspection_applied_ = app::kInspectionPlaceholder;
        build_system_menu();
    }

    void build_system_menu()
    {
        HMENU menu = GetSystemMenu(hwnd_, FALSE);
        if (!menu) return;
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuShowEventCounts, L"Show event counts");
        AppendMenuW(menu, MF_STRING, kMenuProjectForcedOverrides, L"Project forced overrides");
        AppendMenuW(menu, MF_STRING, kMenuAlwaysOnTop, L"Always on top");
    }

    void update_menu_checks()
    {
        HMENU menu = GetSystemMenu(hwnd_, FALSE);
        if (!menu) return;
        CheckMenuItem(menu, kMenuShowEventCounts, MF_BYCOMMAND | (model_->show_event_counts() ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu, kMenuProjectForcedOverrides, MF_BYCOMMAND | (model_->project_forced_overrides() ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu, kMenuAlwaysOnTop, MF_BYCOMMAND | (topmost_ ? MF_CHECKED : MF_UNCHECKED));
        DrawMenuBar(hwnd_);
    }

    // ---- layout
    struct HeaderItem {
        HWND window;
        int width, height;
        int margin_left, margin_top, margin_right, margin_bottom;
    };

    void layout()
    {
        if (!hwnd_ || !summary_ || in_layout_) return;
        in_layout_ = true;
        RECT client;
        GetClientRect(hwnd_, &client);
        int W_ = client.right, H_ = client.bottom;
        int padL = px(2), padT = px(3), padR = px(2), padB = px(2);
        int fh = ctx_->font_height();
        int buttonH = std::max(px(23), fh + px(8));

        // header: a wrapping row of controls
        auto textW = [&](const wchar_t* t) { return ctx_->text_width(std::wstring(t)); };
        std::vector<HeaderItem> items;
        items.push_back({ reset_, textW(L"Reset zoom") + px(18), buttonH, px(2), px(3), px(6), px(3) });
        items.push_back({ follow_label_, textW(L"Follow:"), fh, px(3), px(8), px(3), px(3) });
        items.push_back({ follow_, px(150), combo_height_, 0, px(4), px(7), px(3) });
        items.push_back({ resolution_label_, textW(L"Resolution:"), fh, px(3), px(8), px(3), px(3) });
        items.push_back({ resolution_, px(145), combo_height_, 0, px(4), px(7), px(3) });
        items.push_back({ stats_check_, textW(L"Playback statistics") + ctx_->check_box_space(), fh + px(4), px(3), px(7), px(8), px(3) });
        items.push_back({ channels_, textW(L"Channels\x2026") + px(18), buttonH, px(3), px(3), px(8), px(3) });
        if (visible(status_)) items.push_back({ status_, ctx_->text_width(read_text(status_)) + px(2), fh, px(3), px(8), px(3), px(3) });
        if (visible(progress_)) items.push_back({ progress_, px(105), px(14), px(3), px(8), px(3), px(3) });
        if (visible(cancel_)) items.push_back({ cancel_, textW(L"Cancel") + px(18), buttonH, px(3), px(3), px(3), px(3) });
        int avail = W_ - padL - padR;
        int x = 0, rowTop = 0, rowHeight = 0, totalHeight = 0;
        std::vector<POINT> positions(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            const HeaderItem& it = items[i];
            int need = it.margin_left + it.width + it.margin_right;
            if (x > 0 && x + need > avail) {
                rowTop += rowHeight;
                x = 0;
                rowHeight = 0;
            }
            positions[i].x = padL + x + it.margin_left;
            positions[i].y = padT + rowTop + it.margin_top;
            x += need;
            rowHeight = std::max(rowHeight, it.margin_top + it.height + it.margin_bottom);
            totalHeight = rowTop + rowHeight;
        }
        for (size_t i = 0; i < items.size(); ++i)
            SetWindowPos(items[i].window, nullptr, positions[i].x, positions[i].y, items[i].width, items[i].height, SWP_NOZORDER | SWP_NOACTIVATE);
        header_height_ = totalHeight;

        // body: the splitter container
        int top = padT + totalHeight + px(4);
        int bottom = H_ - padB - px(4);
        int left = padL, right = W_ - padR;
        body_ = { left, top, right, std::max(top, bottom) };
        int splitW = px(kSplitterWidth);
        int avail2 = body_.right - body_.left;
        int minimum1 = px(kPanel1Minimum), minimum2 = px(kPanel2Minimum);
        int maximum = std::max(minimum1, avail2 - minimum2 - splitW);
        split_px_ = std::max(minimum1, std::min(maximum, split_px_ > 0 ? split_px_ : px(kSplitterDistance)));
        splitter_ = { body_.left + split_px_, body_.top, body_.left + split_px_ + splitW, body_.bottom };
        SetWindowPos(summary_, nullptr, body_.left, body_.top, split_px_, body_.bottom - body_.top, SWP_NOZORDER | SWP_NOACTIVATE);
        int rightLeft = splitter_.right;
        int rightRight = body_.right - px(2);
        int inspectionH = px(kInspectionHeight);
        int graphH = std::max<int>(1, (body_.bottom - body_.top) - inspectionH);
        SetWindowPos(graph_, nullptr, rightLeft, body_.top, std::max(1, rightRight - rightLeft), graphH, SWP_NOZORDER | SWP_NOACTIVATE);
        int seekW = ctx_->text_width(L"Seek to pin") + px(18);
        int seekH = buttonH;
        int seekX = rightRight - seekW;
        SetWindowPos(seek_, nullptr, seekX, body_.top + graphH + px(3), seekW, seekH, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(inspection_, nullptr, rightLeft, body_.top + graphH, std::max(1, seekX - px(3) - rightLeft), inspectionH,
            SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, &splitter_, TRUE);
        in_layout_ = false;
    }

    void paint_splitter(HDC dc)
    {
        RECT r = splitter_;
        HBRUSH brush = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));   // ControlDark
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        int height = r.bottom - r.top;
        if (height <= 0) return;
        int center = r.left + (r.right - r.left) / 2;
        int top = r.top + std::max(4, (height - 24) / 2);
        HPEN dark = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DDKSHADOW));
        HPEN light = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DHILIGHT));
        HGDIOBJ old = SelectObject(dc, dark);
        for (int y = top; y < top + 24; y += 6) {
            SelectObject(dc, dark);
            MoveToEx(dc, center - 1, y, nullptr);
            LineTo(dc, center - 1, y + 2);
            SelectObject(dc, light);
            MoveToEx(dc, center, y + 1, nullptr);
            LineTo(dc, center, y + 3);
        }
        SelectObject(dc, old);
        DeleteObject(dark);
        DeleteObject(light);
    }

    // ---- model -> controls
    void apply_summary()
    {
        const std::string& text = model_->summary_text();
        if (text == summary_applied_) return;
        summary_applied_ = text;
        std::wstring wide = W(text);
        POINT scroll = {};
        CHARRANGE selection = {};
        if (rich_) {
            SendMessageW(summary_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
            SendMessageW(summary_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        }
        SendMessageW(summary_, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(summary_, wide.c_str());
        if (rich_) {
            std::vector<app::SummaryHeadingSpan> spans = app::find_summary_headings(text);
            for (size_t i = 0; i < spans.size(); ++i) {
                CHARRANGE range = { static_cast<LONG>(spans[i].start), static_cast<LONG>(spans[i].start + spans[i].length) };
                SendMessageW(summary_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
                CHARFORMAT2W format = {};
                format.cbSize = sizeof(format);
                format.dwMask = CFM_BOLD | CFM_COLOR;
                format.dwEffects = CFE_BOLD;
                format.crTextColor = RGB(42, 92, 150);
                SendMessageW(summary_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
            }
            LONG length = static_cast<LONG>(SendMessageW(summary_, WM_GETTEXTLENGTH, 0, 0));
            CHARRANGE restore = { std::min(selection.cpMin, length), std::min(selection.cpMin, length) };
            SendMessageW(summary_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&restore));
            SendMessageW(summary_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        }
        SendMessageW(summary_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(summary_, nullptr, TRUE);
    }

    void apply_resolution()
    {
        const app::ResolutionSelector& r = model_->resolution();
        if (r.items() != resolution_items_) {
            resolution_items_ = r.items();
            SendMessageW(resolution_, CB_RESETCONTENT, 0, 0);
            for (size_t i = 0; i < resolution_items_.size(); ++i)
                SendMessageW(resolution_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(resolution_items_[i]).c_str()));
        }
        if (static_cast<int>(SendMessageW(resolution_, CB_GETCURSEL, 0, 0)) != r.selected_index())
            SendMessageW(resolution_, CB_SETCURSEL, static_cast<WPARAM>(r.selected_index()), 0);
        resolution_tip_ = W(r.tooltip());
    }

    void apply_busy()
    {
        const app::AnalysisBusyView& b = model_->busy_view();
        std::wstring text = W(b.status_text);
        if (read_text(status_) != text) SetWindowTextW(status_, text.c_str());
        bool before = visible(status_) != b.status_visible || visible(progress_) != b.progress_visible || visible(cancel_) != b.cancel_visible;
        show(status_, b.status_visible);
        show(progress_, b.progress_visible);
        show(cancel_, b.cancel_visible);
        if (static_cast<int>(SendMessageW(progress_, PBM_GETPOS, 0, 0)) != b.progress_permille)
            SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(b.progress_permille), 0);
        layout();
        (void)before;
    }

    // Applies everything the model changed since the last call and re-arms the model timer.
    void sync()
    {
        if (syncing_) {
            resync_ = true;
            return;
        }
        syncing_ = true;
        do {
            resync_ = false;
            if (model_->title_revision() != title_rev_) {
                title_rev_ = model_->title_revision();
                SetWindowTextW(hwnd_, W(model_->title()).c_str());
            }
            if (model_->attach_revision() != attach_rev_) {
                attach_rev_ = model_->attach_revision();
                bool on = model_->attached();
                HWND headers[] = { reset_, follow_label_, follow_, resolution_label_, resolution_, stats_check_, channels_, status_, cancel_ };
                for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); ++i) enable(headers[i], on);
                enable(summary_, on);
                enable(graph_, on);
                enable(inspection_, on);
                workload_graph_invalidate(graph_);
            }
            if (model_->summary_revision() != summary_rev_) {
                summary_rev_ = model_->summary_revision();
                apply_summary();
            }
            if (model_->inspection_revision() != inspection_rev_) {
                inspection_rev_ = model_->inspection_revision();
                const std::string& text = model_->inspection_text();
                if (text != inspection_applied_) {
                    inspection_applied_ = text;
                    SetWindowTextW(inspection_, W(text).c_str());
                }
            }
            enable(seek_, model_->attached() && model_->seek_to_pin_enabled());
            if (model_->resolution_revision() != resolution_rev_) {
                resolution_rev_ = model_->resolution_revision();
                apply_resolution();
            }
            if (model_->busy_revision() != busy_rev_) {
                busy_rev_ = model_->busy_revision();
                apply_busy();
            }
            workload_graph_invalidate(graph_);
        } while (resync_);
        syncing_ = false;
        int wait = model_->next_wake_milliseconds();
        if (wait >= 0) SetTimer(hwnd_, kTimerModel, static_cast<UINT>(std::max(wait, 1)), nullptr);
        else KillTimer(hwnd_, kTimerModel);
    }

    // ---- commands
    void toggle_always_on_top()
    {
        topmost_ = !topmost_;
        SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        update_menu_checks();
    }

    void command(WORD id, WORD code, HWND source)
    {
        switch (id) {
        case IdResetZoom:
            if (code == BN_CLICKED) model_->reset_zoom();
            break;
        case IdFollow:
            if (code == CBN_SELCHANGE) model_->set_follow(static_cast<app::FollowTarget>(SendMessageW(follow_, CB_GETCURSEL, 0, 0)));
            break;
        case IdResolution:
            if (code == CBN_SELCHANGE) {
                model_->select_resolution(static_cast<int>(SendMessageW(resolution_, CB_GETCURSEL, 0, 0)));
                if (model_->custom_prompt_pending()) {
                    sync();
                    if (!run_custom_prompt(hwnd_, *ctx_, *model_, culture_)) model_->cancel_custom_resolution();
                }
            }
            break;
        case IdPlaybackStats:
            if (code == BN_CLICKED) {
                model_->set_playback_statistics_visible(SendMessageW(stats_check_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                workload_graph_invalidate(graph_);
                InvalidateRect(graph_, nullptr, FALSE);
            }
            break;
        case IdChannels:
            if (code == BN_CLICKED && host_.open_channels) host_.open_channels();
            break;
        case IdCancel:
            if (code == BN_CLICKED) model_->cancel_calculation();
            break;
        case IdSeekPin:
            if (code == BN_CLICKED) model_->seek_to_pin();
            break;
        case IdGraph:
            if (code == GN_MODEL_CHANGED) {
                sync();
                return;
            }
            break;
        default:
            return;
        }
        (void)source;
        sync();
    }

    std::wstring tooltip_for(HWND control)
    {
        if (control == resolution_) return resolution_tip_;
        return std::wstring();
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_CREATE:
            wake_target_->store(hwnd_);
            build_controls();
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) layout();
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            // v1 MinimumSize is the window's outer size.
            info->ptMinTrackSize.x = px(kMinimumWidth);
            info->ptMinTrackSize.y = px(kMinimumHeight);
            return 0;
        }
        case WM_EXITSIZEMOVE:
            if (!applying_scale_) {
                RECT client;
                GetClientRect(hwnd_, &client);
                double factor = ctx_->scale_percent() / 100.0 * ctx_->dpi() / 96.0;
                canonical_client_width_ = client.right / factor;
                canonical_client_height_ = client.bottom / factor;
            }
            return 0;
        case WM_ERASEBKGND: {
            RECT r;
            GetClientRect(hwnd_, &r);
            FillRect(reinterpret_cast<HDC>(wParam), &r, GetSysColorBrush(COLOR_BTNFACE));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            paint_splitter(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT r;
            GetClientRect(hwnd_, &r);
            FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
            paint_splitter(dc);
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd_, &p);
                if (PtInRect(&splitter_, p) || dragging_splitter_) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&splitter_, p)) {
                dragging_splitter_ = true;
                drag_offset_ = p.x - splitter_.left;
                SetCapture(hwnd_);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_splitter_) {
                split_px_ = GET_X_LPARAM(lParam) - drag_offset_ - body_.left;
                layout();
                double factor = ctx_->scale_percent() / 100.0 * ctx_->dpi() / 96.0;
                canonical_split_ = split_px_ / factor;
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging_splitter_) {
                dragging_splitter_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            dragging_splitter_ = false;
            return 0;
        case WM_COMMAND:
            command(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
            return 0;
        case WM_NOTIFY: {
            NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
            if (tooltips_.handle_notify(header, [this](HWND control) { return tooltip_for(control); })) return 0;
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HWND control = reinterpret_cast<HWND>(lParam);
            HDC dc = reinterpret_cast<HDC>(wParam);
            if (control == inspection_ || (control == summary_ && !rich_)) {
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                SetTextColor(dc, IsWindowEnabled(control) ? GetSysColor(COLOR_WINDOWTEXT) : ctx_->disabled_text_color());
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            if (control == status_) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(105, 105, 105));   // Color.DimGray
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORDLG:
            break;
        case WM_TIMER:
            if (wParam == kTimerModel) {
                KillTimer(hwnd_, kTimerModel);
                model_->poll();
                sync();
                return 0;
            }
            break;
        case WM_ANALYSIS_WAKE:
            model_->poll();
            sync();
            return 0;
        case WM_ANALYSIS_SPACE:
            if (host_.toggle_transport) host_.toggle_transport();
            return 0;
        case WM_SYSCOMMAND:
            switch (wParam & 0xFFF0) {
            case kMenuShowEventCounts:
                model_->set_show_event_counts(!model_->show_event_counts());
                update_menu_checks();
                sync();
                return 0;
            case kMenuProjectForcedOverrides:
                model_->set_project_forced_overrides(!model_->project_forced_overrides());
                update_menu_checks();
                sync();
                return 0;
            case kMenuAlwaysOnTop:
                toggle_always_on_top();
                return 0;
            }
            break;
        case WM_INITMENUPOPUP:
            update_menu_checks();
            break;
        case WM_DESTROY: {
            wake_target_->store(nullptr);
            KillTimer(hwnd_, kTimerModel);
            if (model_) model_->close();
            for (size_t i = 0; i < g_windows.size(); ++i)
                if (g_windows[i] == this) { g_windows.erase(g_windows.begin() + static_cast<long>(i)); break; }
            if (g_windows.empty() && g_hook) {
                UnhookWindowsHookEx(g_hook);
                g_hook = nullptr;
            }
            break;
        }
        case WM_NCDESTROY:
            if (inspection_font_) { DeleteObject(inspection_font_); inspection_font_ = nullptr; }
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    CompanionHost host_;
    UiContext* ctx_;
    format::NumberCulture culture_;
    std::unique_ptr<app::AnalysisWindowModel> model_;
    std::shared_ptr<std::atomic<HWND>> wake_target_;
    HWND hwnd_ = nullptr;
    HWND reset_ = nullptr, follow_label_ = nullptr, follow_ = nullptr, resolution_label_ = nullptr, resolution_ = nullptr;
    HWND stats_check_ = nullptr, channels_ = nullptr, status_ = nullptr, progress_ = nullptr, cancel_ = nullptr;
    HWND summary_ = nullptr, graph_ = nullptr, inspection_ = nullptr, seek_ = nullptr;
    HFONT inspection_font_ = nullptr;
    TooltipHost tooltips_;
    bool rich_ = false;
    bool topmost_ = false;
    bool in_layout_ = false, syncing_ = false, resync_ = false, applying_scale_ = false;
    int combo_height_ = 22;
    int header_height_ = 0;
    int split_px_ = 0;
    double canonical_split_ = kSplitterDistance;
    double canonical_client_width_ = kDefaultClientWidth, canonical_client_height_ = kDefaultClientHeight;
    RECT body_ = {}, splitter_ = {};
    bool dragging_splitter_ = false;
    int drag_offset_ = 0;
    uint64_t title_rev_ = 0, attach_rev_ = 0, summary_rev_ = 0, inspection_rev_ = 0, resolution_rev_ = 0, busy_rev_ = 0;
    std::string summary_applied_, inspection_applied_;
    std::vector<std::string> resolution_items_;
    std::wstring resolution_tip_;
};

LRESULT CALLBACK analysis_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    AnalysisWindow* self = reinterpret_cast<AnalysisWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<AnalysisWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    LRESULT result = self->handle(message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

// v1 TryHandleAnalysisSpace and the dialog keys: the thread's message loop only knows the main window, so a
// GetMessage hook gives the Analysis windows Space (Play/Pause), Tab and Enter/Escape handling.
LRESULT CALLBACK AnalysisWindow::get_message_hook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && wParam == PM_REMOVE) {
        MSG* message = reinterpret_cast<MSG*>(lParam);
        if (message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST) {
            HWND root = message->hwnd ? GetAncestor(message->hwnd, GA_ROOT) : nullptr;
            AnalysisWindow* target = nullptr;
            for (size_t i = 0; i < g_windows.size() && root; ++i)
                if (g_windows[i]->hwnd_ == root) target = g_windows[i];
            if (target && IsWindowEnabled(root)) {
                if (message->message == WM_KEYDOWN && message->wParam == VK_SPACE && GetActiveWindow() == root) {
                    bool handled = true;
                    HWND focus = message->hwnd;
                    if (class_is(focus, L"Edit") || class_is(focus, L"RichEdit50W")) {
                        handled = (GetWindowLongW(focus, GWL_STYLE) & ES_READONLY) != 0;
                    } else if (class_is(focus, L"ComboBox")) {
                        handled = SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0) == 0;
                    }
                    if (handled) {
                        message->message = WM_NULL;
                        PostMessageW(root, WM_ANALYSIS_SPACE, 0, 0);
                        return CallNextHookEx(g_hook, code, wParam, lParam);
                    }
                }
                // F1 is left for the main loop (pre_translate opens Help on the Analysis topic)
                if (!(message->message == WM_KEYDOWN && message->wParam == VK_F1) && IsDialogMessageW(root, message)) message->message = WM_NULL;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

bool g_registered = false;

void register_classes()
{
    if (g_registered) return;
    g_registered = true;
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpfnWndProc = analysis_proc;
    wc.lpszClassName = kAnalysisClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = {};
    pc.cbSize = sizeof(pc);
    pc.hInstance = instance;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    pc.lpfnWndProc = prompt_proc;
    pc.lpszClassName = kPromptClass;
    pc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&pc);
    register_workload_graph_class(instance);
}

} // namespace

bool save_window_bitmap(HWND window, const std::wstring& path)
{
    RECT r;
    GetClientRect(window, &r);
    int w = r.right, h = r.bottom;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    // The window's own paint code for the background, then every visible child from the bottom of the z-order up.
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    for (HWND child = GetWindow(GetWindow(window, GW_CHILD), GW_HWNDLAST); child; child = GetWindow(child, GW_HWNDPREV)) {
        if (!visible(child)) continue;
        RECT cr;
        GetWindowRect(child, &cr);
        POINT origin = { cr.left, cr.top };
        ScreenToClient(window, &origin);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        SetViewportOrgEx(dc, 0, 0, nullptr);
    }
    BITMAPFILEHEADER header = {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + w * h * 4;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD written;
        WriteFile(file, &header, sizeof(header), &written, nullptr);
        WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
        WriteFile(file, bits, w * h * 4, &written, nullptr);
        CloseHandle(file);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}

std::unique_ptr<CompanionWindow> create_analysis_window(CompanionHost& host)
{
    register_classes();
    std::unique_ptr<AnalysisWindow> window(new AnalysisWindow(host));
    if (!window->create()) return nullptr;
    return std::unique_ptr<CompanionWindow>(window.release());
}

} // namespace ui
} // namespace midibn
