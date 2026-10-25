// Input controls: the scrub-or-type numeric field, the slider with v1's
// click-to-position rule, and the Rate model popup.
#include "controls.h"

#include "midibn/app/ui_mappings.h"
#include "midibn/format/net_format.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace midibn {
namespace ui {

namespace {

COLORREF sys(int index) { return GetSysColor(index); }

// ------------------------------------------------------------------ scrub field palette

struct FieldPalette {
    bool initialized = false;
    COLORREF color[2] = {};   // [0] disabled, [1] enabled
    HBRUSH brush[2] = {};
};
FieldPalette g_field_palette;

// v1 TryMeasureComboBoxField: paint a blank drop-down list into a bitmap and take the dominant interior colour
// (away from the border, the arrow and any text).
bool measure_combo_field(bool enabled, COLORREF& color)
{
    const int w = 140, h = 22;
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_POPUP | CBS_DROPDOWNLIST, 0, 0, w, 200, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!combo) return false;
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    EnableWindow(combo, enabled ? TRUE : FALSE);
    RECT r;
    GetWindowRect(combo, &r);
    int cw = std::max<LONG>(w, r.right - r.left), ch = std::max<LONG>(h, r.bottom - r.top);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = cw;
    info.bmiHeader.biHeight = -ch;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap && bits) {
        HGDIOBJ old = SelectObject(dc, bitmap);
        RECT all = { 0, 0, cw, ch };
        FillRect(dc, &all, GetSysColorBrush(COLOR_BTNFACE));
        SendMessageW(combo, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        GdiFlush();
        const uint32_t* px = static_cast<const uint32_t*>(bits);
        std::vector<std::pair<uint32_t, int>> counts;
        int right = std::max(5, cw - 28), bottom = std::max(5, ch - 4);
        for (int y = 4; y < bottom; ++y)
            for (int x = 4; x < right; ++x) {
                uint32_t v = px[y * cw + x] & 0x00FFFFFFu;
                bool found = false;
                for (size_t i = 0; i < counts.size(); ++i)
                    if (counts[i].first == v) { ++counts[i].second; found = true; break; }
                if (!found && counts.size() < 64) counts.push_back(std::make_pair(v, 1));
            }
        int best = 0;
        for (size_t i = 0; i < counts.size(); ++i)
            if (counts[i].second > best) { best = counts[i].second; color = RGB((counts[i].first >> 16) & 0xFF, (counts[i].first >> 8) & 0xFF, counts[i].first & 0xFF); }
        ok = best > 0;
        SelectObject(dc, old);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    DestroyWindow(combo);
    return ok;
}

void ensure_field_palette()
{
    if (g_field_palette.initialized) return;
    for (int e = 0; e < 2; ++e) {
        COLORREF c = sys(e ? COLOR_WINDOW : COLOR_BTNFACE);
        if (!measure_combo_field(e == 1, c)) c = sys(e ? COLOR_WINDOW : COLOR_BTNFACE);
        g_field_palette.color[e] = c;
        g_field_palette.brush[e] = CreateSolidBrush(c);
    }
    g_field_palette.initialized = true;
}

// ------------------------------------------------------------------ scrub field

struct FieldData {
    UiContext* ctx = nullptr;
    const format::NumberCulture* culture = nullptr;
    HWND window = nullptr;
    std::function<int64_t(int64_t, int64_t)> mapper;
    bool use_mapper = false;

    int64_t value = 0, minimum = 0, maximum = 0, increment = 1;
    int coarse_units = 1, coarse_pixels = 1, fine_units = 1, fine_pixels = 1;

    bool typing = false, armed = false, scrubbing = false, cursor_hidden = false, ignore_warp = false;
    POINT start_screen = {}, anchor_screen = {};
    int64_t typing_start_value = 0;
    int pixel_remainder = 0;
    bool remainder_fine = false;
    int64_t scrub_start = 0, scrub_pixels = 0;
    bool scrub_init = false;
    std::wstring last_error;
};

const int kDragThreshold = 3;
const UINT_PTR kFieldSubclass = 0x5C2B;

int64_t clamp64(FieldData* d, int64_t v) { return v < d->minimum ? d->minimum : v > d->maximum ? d->maximum : v; }

std::wstring display_text(FieldData* d)
{
    return W(format::format_n0(d->value, d->culture ? *d->culture : format::NumberCulture()));
}

void set_text(FieldData* d, const std::wstring& text)
{
    int length = GetWindowTextLengthW(d->window);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(d->window, &current[0], length + 1);
    current.resize(static_cast<size_t>(length));
    if (current != text) SetWindowTextW(d->window, text.c_str());
}

void show_flat_text(FieldData* d)
{
    if (!d->typing) set_text(d, display_text(d));
}

std::wstring editable_text(int64_t value) { return std::to_wstring(value); }

bool parse_editable(FieldData* d, const std::wstring& raw, int64_t& value)
{
    std::wstring text;
    // trim
    size_t b = 0, e = raw.size();
    while (b < e && iswspace(raw[b])) ++b;
    while (e > b && iswspace(raw[e - 1])) --e;
    std::wstring group = d->culture ? W(d->culture->group_separator) : L",";
    for (size_t i = b; i < e;) {
        if (!group.empty() && raw.compare(i, group.size(), group) == 0) { i += group.size(); continue; }
        text.push_back(raw[i++]);
    }
    if (text.empty()) return false;
    size_t i = 0;
    bool negative = false;
    if (text[0] == L'-' || text[0] == L'+') { negative = text[0] == L'-'; i = 1; }
    if (i >= text.size()) return false;
    int64_t result = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < L'0' || text[i] > L'9') return false;
        result = result * 10 + (text[i] - L'0');
        if (result > 4000000000LL) return false;
    }
    value = negative ? -result : result;
    return true;
}

void notify_value(FieldData* d, int64_t value)
{
    SendMessageW(GetParent(d->window), WM_MIDIBN_FIELD, GetDlgCtrlID(d->window), reinterpret_cast<LPARAM>(&value));
}

void request_value(FieldData* d, int64_t value)
{
    value = clamp64(d, value);
    if (value == d->value) return;
    d->value = value;
    if (!d->typing) show_flat_text(d);
    notify_value(d, value);
}

void end_gesture(FieldData* d)
{
    d->armed = false;
    bool wasScrubbing = d->scrubbing;
    d->scrubbing = false;
    d->ignore_warp = false;
    d->pixel_remainder = 0;
    d->scrub_init = false;
    if (wasScrubbing && GetCapture() == d->window) ReleaseCapture();
    if (d->cursor_hidden) {
        ShowCursor(TRUE);
        d->cursor_hidden = false;
    }
    if (!d->typing) show_flat_text(d);
}

void apply_scrub_delta(FieldData* d, int pixels, bool fine)
{
    if (d->use_mapper && d->mapper) {
        if (!d->scrub_init || d->remainder_fine != fine) {
            d->scrub_start = d->value;
            d->scrub_pixels = 0;
            d->scrub_init = true;
            d->remainder_fine = fine;
        }
        d->scrub_pixels = std::max<int64_t>(-1000000, std::min<int64_t>(1000000, d->scrub_pixels + pixels));
        int64_t mapped = fine ? clamp64(d, d->scrub_start + d->scrub_pixels / std::max(1, d->fine_pixels))
                              : d->mapper(d->scrub_start, d->scrub_pixels);
        request_value(d, mapped);
        return;
    }
    if (d->remainder_fine != fine) { d->pixel_remainder = 0; d->remainder_fine = fine; }
    int divisor = std::max(1, fine ? d->fine_pixels : d->coarse_pixels);
    int units = fine ? d->fine_units : d->coarse_units;
    int accumulated = d->pixel_remainder + pixels;
    int steps = accumulated / divisor;
    d->pixel_remainder = accumulated - steps * divisor;
    if (steps == 0) return;
    request_value(d, d->value + static_cast<int64_t>(steps) * units);
}

void enter_typing(FieldData* d)
{
    end_gesture(d);
    d->typing = true;
    d->typing_start_value = d->value;
    SendMessageW(d->window, EM_SETREADONLY, FALSE, 0);
    set_text(d, editable_text(d->value));
    SetFocus(d->window);
    ShowCaret(d->window);
    SendMessageW(d->window, EM_SETSEL, 0, -1);
    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
}

void exit_typing(FieldData* d)
{
    d->typing = false;
    SendMessageW(d->window, EM_SETREADONLY, TRUE, 0);
    HideCaret(d->window);
    SendMessageW(d->window, EM_SETSEL, 0, 0);
    show_flat_text(d);
    if (!d->last_error.empty()) show_transient_tip(d->window, d->last_error, 2500);
}

bool commit_typed(FieldData* d, bool explicitCommit)
{
    if (!d->typing) return true;
    int length = GetWindowTextLengthW(d->window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(d->window, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    int64_t parsed = 0;
    d->last_error.clear();
    if (!parse_editable(d, text, parsed) || parsed < d->minimum || parsed > d->maximum) {
        d->last_error = L"Enter a value from " + editable_text(d->minimum) + L" through " + editable_text(d->maximum) + L".";
        exit_typing(d);
        return false;
    }
    if (explicitCommit || parsed != d->typing_start_value) request_value(d, parsed);
    exit_typing(d);
    return true;
}

void abandon_typing(FieldData* d)
{
    if (d->typing) {
        d->last_error.clear();
        exit_typing(d);
    }
    end_gesture(d);
}

void adjust_typed(FieldData* d, int direction)
{
    if (!d->typing) return;
    int length = GetWindowTextLengthW(d->window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(d->window, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    int64_t parsed = d->value;
    if (!parse_editable(d, text, parsed)) parsed = d->value;
    parsed = clamp64(d, parsed + direction);
    set_text(d, editable_text(parsed));
    SendMessageW(d->window, EM_SETSEL, 0, -1);
    d->last_error.clear();
}

LRESULT CALLBACK field_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR ref)
{
    FieldData* d = reinterpret_cast<FieldData*>(ref);
    switch (message) {
    case WM_GETDLGCODE:
        return d->typing ? (DLGC_WANTALLKEYS | DLGC_WANTCHARS) : DLGC_WANTARROWS;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && IsWindowEnabled(window)) {
            SetCursor(LoadCursorW(nullptr, d->typing ? IDC_IBEAM : IDC_SIZEWE));
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU:
        return 0;
    case WM_SETFOCUS: {
        LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (!d->typing) HideCaret(window);
        return result;
    }
    case WM_KILLFOCUS:
        if (d->typing) commit_typed(d, false);
        if (d->scrubbing || d->armed) end_gesture(d);
        break;
    case WM_CAPTURECHANGED:
        if (d->scrubbing && reinterpret_cast<HWND>(lParam) != window) end_gesture(d);
        break;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
        if (d->typing) break;
        if (!IsWindowEnabled(window)) return 0;
        SetFocus(window);
        d->armed = true;
        GetCursorPos(&d->start_screen);
        d->anchor_screen = d->start_screen;
        d->last_error.clear();
        return 0;
    case WM_MOUSEMOVE: {
        if (d->typing) break;
        if (!d->armed) return 0;
        if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) { end_gesture(d); return 0; }
        POINT screen;
        GetCursorPos(&screen);
        int total = screen.x - d->start_screen.x;
        if (!d->scrubbing && std::abs(total) > kDragThreshold) {
            d->scrubbing = true;
            d->anchor_screen = screen;
            d->pixel_remainder = 0;
            d->remainder_fine = false;
            d->scrub_init = false;
            SetCapture(window);
            if (!d->cursor_hidden) { ShowCursor(FALSE); d->cursor_hidden = true; }
        }
        if (!d->scrubbing) return 0;
        if (d->ignore_warp && screen.x == d->anchor_screen.x && screen.y == d->anchor_screen.y) {
            d->ignore_warp = false;
            return 0;
        }
        int delta = screen.x - d->anchor_screen.x;
        if (delta == 0) return 0;
        apply_scrub_delta(d, delta, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        d->ignore_warp = true;
        SetCursorPos(d->anchor_screen.x, d->anchor_screen.y);
        return 0;
    }
    case WM_LBUTTONUP:
        if (d->typing) break;
        if (d->armed) {
            if (d->scrubbing) end_gesture(d);
            else {
                d->armed = false;
                enter_typing(d);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (!d->typing) {
            if (wParam == VK_F2 || wParam == VK_RETURN) { enter_typing(d); return 0; }
            if (wParam == VK_UP || wParam == VK_DOWN) {
                request_value(d, clamp64(d, d->value + (wParam == VK_UP ? d->increment : -d->increment)));
                return 0;
            }
            break;
        }
        if (wParam == VK_RETURN) { commit_typed(d, true); return 0; }
        if (wParam == VK_ESCAPE) { abandon_typing(d); return 0; }
        if (wParam == VK_UP || wParam == VK_DOWN) { adjust_typed(d, wParam == VK_UP ? 1 : -1); return 0; }
        if (wParam == VK_TAB) {
            commit_typed(d, false);
            PostMessageW(GetParent(window), WM_NEXTDLGCTL, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, FALSE);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB) return 0;
        if (!d->typing) return 0;
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, field_proc, kFieldSubclass);
        delete d;
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

COLORREF scrub_field_back_color(bool enabled)
{
    ensure_field_palette();
    return g_field_palette.color[enabled ? 1 : 0];
}

HBRUSH scrub_field_back_brush(bool enabled)
{
    ensure_field_palette();
    return g_field_palette.brush[enabled ? 1 : 0];
}

HWND create_scrub_field(HWND parent, int id, UiContext* ctx, const format::NumberCulture* culture,
    std::function<int64_t(int64_t, int64_t)> mapper)
{
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | ES_CENTER | ES_READONLY | ES_AUTOHSCROLL,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    FieldData* d = new FieldData();
    d->ctx = ctx;
    d->culture = culture;
    d->window = edit;
    d->mapper = std::move(mapper);
    d->use_mapper = static_cast<bool>(d->mapper);
    SetWindowSubclass(edit, field_proc, kFieldSubclass, reinterpret_cast<DWORD_PTR>(d));
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->ui_font()), FALSE);
    return edit;
}

void scrub_field_set_mapper(HWND field, std::function<int64_t(int64_t, int64_t)> mapper)
{
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(field, field_proc, kFieldSubclass, &ref)) return;
    FieldData* d = reinterpret_cast<FieldData*>(ref);
    d->mapper = std::move(mapper);
    d->use_mapper = static_cast<bool>(d->mapper);
    d->scrub_init = false;
}

void scrub_field_apply(HWND field, const app::NumericFieldView& view)
{
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(field, field_proc, kFieldSubclass, &ref)) return;
    FieldData* d = reinterpret_cast<FieldData*>(ref);
    d->minimum = view.minimum;
    d->maximum = view.maximum;
    d->increment = view.increment;
    d->coarse_units = view.coarse_units;
    d->coarse_pixels = view.coarse_pixels;
    d->fine_units = view.fine_units;
    d->fine_pixels = view.fine_pixels;
    d->use_mapper = view.scrub != app::ScrubKind::None && static_cast<bool>(d->mapper);
    if (!d->typing) {
        if (d->value != view.value) d->scrub_init = d->scrub_init && d->scrubbing;
        d->value = clamp64(d, view.value);
        show_flat_text(d);
    }
    bool enabled = view.enabled;
    if ((IsWindowEnabled(field) != FALSE) != enabled) {
        if (!enabled && (d->typing || d->scrubbing || d->armed)) { abandon_typing(d); }
        EnableWindow(field, enabled ? TRUE : FALSE);
    }
}

bool scrub_field_active(HWND field)
{
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(field, field_proc, kFieldSubclass, &ref)) return false;
    FieldData* d = reinterpret_cast<FieldData*>(ref);
    return d->typing || d->scrubbing || d->armed;
}

void scrub_field_finish(HWND field, bool commit)
{
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(field, field_proc, kFieldSubclass, &ref)) return;
    FieldData* d = reinterpret_cast<FieldData*>(ref);
    if (d->typing && commit) commit_typed(d, false);
    else if (d->typing) abandon_typing(d);
    end_gesture(d);
}

int scrub_field_preferred_width(HWND field)
{
    DWORD_PTR ref = 0;
    if (!GetWindowSubclass(field, field_proc, kFieldSubclass, &ref)) return 40;
    FieldData* d = reinterpret_cast<FieldData*>(ref);
    int minimum = d->ctx->text_width(L"999");
    std::wstring text = d->typing ? std::wstring() : display_text(d);
    int actual = d->ctx->text_width(text);
    return std::max(minimum, actual) + d->ctx->px(6);
}

// ------------------------------------------------------------------ slider

namespace {

const UINT_PTR kSliderSubclass = 0x5C2C;

LRESULT CALLBACK slider_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_LBUTTONDOWN && IsWindowEnabled(window)) {
        RECT client;
        GetClientRect(window, &client);
        int minimum = static_cast<int>(SendMessageW(window, TBM_GETRANGEMIN, 0, 0));
        int maximum = static_cast<int>(SendMessageW(window, TBM_GETRANGEMAX, 0, 0));
        int value = app::value_from_track_point(GET_X_LPARAM(lParam), client.right, minimum, maximum);
        SendMessageW(window, TBM_SETPOS, TRUE, value);
        SendMessageW(GetParent(window), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, value), reinterpret_cast<LPARAM>(window));
    }
    else if (message == WM_NCDESTROY) RemoveWindowSubclass(window, slider_proc, kSliderSubclass);
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

HWND create_slider(HWND parent, int id, UiContext* ctx)
{
    HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TBS_HORZ | TBS_AUTOTICKS,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    (void)ctx;
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, app::kSliderScaleMaximum));
    SendMessageW(slider, TBM_SETTICFREQ, 1000, 0);
    SendMessageW(slider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(slider, TBM_SETPAGESIZE, 0, 5);
    SetWindowSubclass(slider, slider_proc, kSliderSubclass, 0);
    return slider;
}

// ------------------------------------------------------------------ Rate model popup

struct RatePopupItem {
    bool heading;
    app::RateModelChoice choice;
    std::wstring text;
};

namespace {

const UINT_PTR kRateSubclass = 0x5C2D;

struct RatePopupData {
    UiContext* ctx = nullptr;
    bool forward = false;
    bool open = false;
};

const app::RateModelChoice kIndexChoices[app::kRateModelChoiceCount] = {
    app::RateModelChoice::None, app::RateModelChoice::ProcessingTime, app::RateModelChoice::MidiBitrate,
    app::RateModelChoice::EventsPerSecond, app::RateModelChoice::PerNoteIntervalGate, app::RateModelChoice::DelayedNoteOff,
    app::RateModelChoice::NoteAttackLimit, app::RateModelChoice::OutputRateLimit
};

std::vector<RatePopupItem> g_popup_items;

void show_rate_popup(HWND combo, RatePopupData* d)
{
    if (d->open) return;
    d->open = true;
    g_popup_items.clear();
    g_popup_items.push_back({ false, app::RateModelChoice::None, L"None" });
    g_popup_items.push_back({ true, app::RateModelChoice::None, W(app::rate_slowdown_heading(d->forward)) });
    g_popup_items.push_back({ false, app::RateModelChoice::ProcessingTime, L"Processing time per event" });
    g_popup_items.push_back({ false, app::RateModelChoice::MidiBitrate, L"MIDI serial bitrate" });
    g_popup_items.push_back({ false, app::RateModelChoice::EventsPerSecond, L"Events per second" });
    g_popup_items.push_back({ true, app::RateModelChoice::None, W(app::rate_gate_heading()) });
    g_popup_items.push_back({ false, app::RateModelChoice::PerNoteIntervalGate, L"Per-note interval gate" });
    g_popup_items.push_back({ false, app::RateModelChoice::DelayedNoteOff, L"Delayed NoteOff" });
    g_popup_items.push_back({ false, app::RateModelChoice::NoteAttackLimit, L"Note start rate gate" });
    g_popup_items.push_back({ false, app::RateModelChoice::OutputRateLimit, L"Output rate gate" });
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < g_popup_items.size(); ++i) {
        UINT flags = MF_OWNERDRAW | (g_popup_items[i].heading ? MF_DISABLED : 0);
        AppendMenuW(menu, flags, 1000 + i, reinterpret_cast<LPCWSTR>(&g_popup_items[i]));
    }
    RECT rect;
    GetWindowRect(combo, &rect);
    HWND owner = GetAncestor(combo, GA_ROOT);
    SetFocus(combo);
    int command = TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, rect.left, rect.bottom,
        owner, nullptr);
    DestroyMenu(menu);
    d->open = false;
    if (command >= 1000 && command < 1000 + static_cast<int>(g_popup_items.size())) {
        app::RateModelChoice choice = g_popup_items[static_cast<size_t>(command - 1000)].choice;
        int index = rate_choice_to_index(choice);
        if (SendMessageW(combo, CB_GETCURSEL, 0, 0) != index) {
            SendMessageW(combo, CB_SETCURSEL, index, 0);
            SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
        }
    }
}

LRESULT CALLBACK rate_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR ref)
{
    RatePopupData* d = reinterpret_cast<RatePopupData*>(ref);
    switch (message) {
    case CB_SHOWDROPDOWN:
        if (wParam) show_rate_popup(window, d);
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowEnabled(window)) break;
        show_rate_popup(window, d);
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_F4 || (wParam == VK_DOWN && (GetKeyState(VK_MENU) & 0x8000)) ||
            (wParam == VK_UP && (GetKeyState(VK_MENU) & 0x8000))) {
            show_rate_popup(window, d);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, rate_proc, kRateSubclass);
        delete d;
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

int rate_choice_to_index(app::RateModelChoice choice)
{
    for (int i = 0; i < app::kRateModelChoiceCount; ++i)
        if (kIndexChoices[i] == choice) return i;
    return 0;
}

app::RateModelChoice rate_index_to_choice(int index)
{
    return index >= 0 && index < app::kRateModelChoiceCount ? kIndexChoices[index] : app::RateModelChoice::None;
}

void install_rate_popup(HWND combo, UiContext* ctx)
{
    RatePopupData* d = new RatePopupData();
    d->ctx = ctx;
    SetWindowSubclass(combo, rate_proc, kRateSubclass, reinterpret_cast<DWORD_PTR>(d));
}

void rate_popup_set_forward_only(HWND combo, bool forward)
{
    DWORD_PTR ref = 0;
    if (GetWindowSubclass(combo, rate_proc, kRateSubclass, &ref)) reinterpret_cast<RatePopupData*>(ref)->forward = forward;
}

bool rate_popup_measure(MEASUREITEMSTRUCT* item, UiContext* ctx)
{
    if (item->CtlType != ODT_MENU) return false;
    const RatePopupItem* data = reinterpret_cast<const RatePopupItem*>(item->itemData);
    if (!data) return false;
    HFONT font = data->heading ? ctx->ui_bold_font() : ctx->ui_font();
    int textWidth = ctx->text_width(font, data->text);
    item->itemWidth = textWidth + ctx->px_min1(data->heading ? 12 : 24) + ctx->px_min1(4);
    int height = std::max(ctx->px_min1(20), ctx->font_height() + ctx->px_min1(8));
    item->itemHeight = height + (data->heading ? ctx->px_min1(5) : 0);
    return true;
}

bool rate_popup_draw(DRAWITEMSTRUCT* item, UiContext* ctx)
{
    if (item->CtlType != ODT_MENU) return false;
    const RatePopupItem* data = reinterpret_cast<const RatePopupItem*>(item->itemData);
    if (!data) return false;
    HDC dc = item->hDC;
    RECT rect = item->rcItem;
    bool selected = (item->itemState & ODS_SELECTED) != 0 && !data->heading;
    HBRUSH back = CreateSolidBrush(selected ? sys(COLOR_HIGHLIGHT) : data->heading ? sys(COLOR_3DHIGHLIGHT) : sys(COLOR_MENU));
    FillRect(dc, &rect, back);
    DeleteObject(back);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? sys(COLOR_HIGHLIGHTTEXT) : sys(COLOR_MENUTEXT));
    HGDIOBJ old = SelectObject(dc, data->heading ? ctx->ui_bold_font() : ctx->ui_font());
    RECT text = rect;
    text.left += ctx->px_min1(data->heading ? 9 : 20);
    if (data->heading) text.top += ctx->px_min1(5);
    DrawTextW(dc, data->text.c_str(), static_cast<int>(data->text.size()), &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    if (data->heading) {
        HPEN pen = CreatePen(PS_SOLID, 1, sys(COLOR_3DSHADOW));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
        LineTo(dc, rect.right, rect.bottom - 1);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    return true;
}

} // namespace ui
} // namespace midibn
