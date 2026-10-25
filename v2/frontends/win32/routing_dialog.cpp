// The Track routing editor (v1 TrackRoutingForm): a modal, resizable dialog that edits a
// session-only route and hands the result to the controller. All decisions live in
// app::RoutingDialogModel; this file is windows, layout and the planner's worker thread.
//
// The plan is computed on a worker thread (platform services) so the window stays
// responsive on big songs. One job runs at a time; when a setting changes while a job runs,
// the finished result is discarded (generation counter) and a job for the newest settings
// starts. While a plan is pending Apply is off and the status line says so.
#include "routing_dialog.h"

#include "lane_grid.h"
#include "controls.h"

#include "midibn/app/help_text.h"
#include "midibn/app/routing_dialog_model.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace midibn {
namespace ui {

namespace {

const UINT WM_PLAN_DONE = WM_APP + 30;
const UINT_PTR kCapacityTimer = 0x7411;

std::wstring w16(const std::u16string& text)
{
    return std::wstring(reinterpret_cast<const wchar_t*>(text.data()), text.size());
}

// The planner thread's hand-off to the window.
struct Mailbox {
    std::mutex mutex;
    bool has = false;
    bool closed = false;
    uint64_t generation = 0;
    std::shared_ptr<const routing::CompiledTrackRoute> route;
    bool failed = false;
    std::string message;
};

struct Dialog {
    CompanionHost* host = nullptr;
    UiContext* ctx = nullptr;
    std::shared_ptr<const Song> song;
    std::unique_ptr<app::RoutingDialogModel> model;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    bool running = false;

    HWND window = nullptr;
    HWND intro = nullptr, tier_label = nullptr, tier = nullptr, capacity_label = nullptr, capacity = nullptr, spin = nullptr;
    HWND percussion_label = nullptr, percussion = nullptr, usable_label = nullptr, usable[16] = {};
    HWND lanes_heading = nullptr, preview_heading = nullptr, caption = nullptr, destination = nullptr, preview = nullptr;
    HWND status = nullptr, confirm = nullptr, original = nullptr, apply = nullptr, cancel = nullptr;
    LaneGrid grid;
    TooltipHost tips;
    HFONT mono = nullptr;
    int combo_h = 0;

    bool syncing = false;
    bool done = false;
    bool apply_requested = false;
    double split_ratio = 0.6;
    RECT bar = {};
    int bar_width = 0;
    bool dragging = false;
    int min_width = 0, min_height = 0;
};

bool g_class_registered = false;

HFONT make_mono_font(UiContext& ctx)
{
    // v1 UiScaleFont.CreateMonospace(8.5F): Consolas (Lucida Console below 100%), 8.5 pt at the scale.
    double points = 8.5 * ctx.scale_percent() / 100.0;
    if (ctx.scale_percent() == 50) points = std::max(points, 6.0);
    else if (ctx.scale_percent() == 75) points = std::max(points, 7.0);
    const wchar_t* face = ctx.scale_percent() < 100 ? L"Lucida Console" : L"Consolas";
    if (!UiContext::font_exists(face)) face = UiContext::font_exists(L"Consolas") ? L"Consolas" : L"Courier New";
    LOGFONTW logfont = {};
    wcsncpy_s(logfont.lfFaceName, face, _TRUNCATE);
    logfont.lfHeight = -static_cast<LONG>(points * ctx.dpi() / 72.0 + 0.5);
    logfont.lfWeight = FW_NORMAL;
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&logfont);
}

int measure_wrapped(HFONT font, const std::wstring& text, int width)
{
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    RECT rect = { 0, 0, std::max(1, width), 0 };
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return rect.bottom - rect.top;
}

HWND make_child(Dialog& d, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0)
{
    HWND window = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, d.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(d.ctx->ui_font()), TRUE);
    attach_scaled_button(window, d.ctx);   // P10-UI1: check boxes draw at the Application scale (no-op for other controls)
    return window;
}

HWND make_label(Dialog& d, const std::u16string& text, bool bold = false, bool notify = false)
{
    HWND label = make_child(d, L"STATIC", w16(text).c_str(), SS_LEFT | SS_NOPREFIX | (notify ? SS_NOTIFY : 0), -1);
    if (bold) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(d.ctx->ui_bold_font()), TRUE);
    return label;
}

HWND make_combo(Dialog& d, int id)
{
    return make_child(d, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
}

// ------------------------------------------------------------------ view updates

std::vector<LaneGridRow> grid_rows(const app::RoutingDialogModel& model)
{
    std::vector<LaneGridRow> rows;
    rows.reserve(model.rows().size());
    for (const app::RoutingDialogRow& source : model.rows()) {
        LaneGridRow row;
        row.cells[0] = w16(source.row.lane_text);
        row.cells[1] = w16(source.row.patch_text);
        row.cells[2] = w16(source.row.counts_text);
        row.cells[3] = w16(source.row.destination_text);
        row.cells[4] = w16(source.row.status_text);
        row.style = static_cast<int>(source.row.style);
        row.patch_tip = w16(source.patch_tooltip);
        rows.push_back(std::move(row));
    }
    return rows;
}

// Status line, confirmation, Apply, correction selector: everything that follows from the model's
// current state and not from a new plan.
void refresh_state(Dialog& d)
{
    const app::RoutingDialogModel& model = *d.model;
    bool wasSyncing = d.syncing;
    d.syncing = true;
    SetWindowTextW(d.status, w16(model.status_text()).c_str());
    InvalidateRect(d.status, nullptr, TRUE);
    bool visible = model.lossy_confirmation_visible();
    ShowWindow(d.confirm, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(d.confirm, visible);
    SendMessageW(d.confirm, BM_SETCHECK, model.lossy_confirmation_checked() ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(d.apply, model.apply_enabled());
    EnableWindow(d.original, model.original_enabled());
    EnableWindow(d.destination, model.destination_enabled());
    SendMessageW(d.destination, CB_SETCURSEL, static_cast<WPARAM>(model.destination_index()), 0);
    d.syncing = wasSyncing;
}

void refresh_option_controls(Dialog& d)
{
    const app::RoutingDialogModel& model = *d.model;
    d.syncing = true;
    SendMessageW(d.tier, CB_SETCURSEL, static_cast<WPARAM>(model.tier_index()), 0);
    SendMessageW(d.percussion, CB_SETCURSEL, static_cast<WPARAM>(model.percussion_index()), 0);
    SendMessageW(d.spin, UDM_SETPOS32, 0, model.capacity());
    for (int channel = 0; channel < 16; ++channel)
        SendMessageW(d.usable[channel], BM_SETCHECK, model.usable()[static_cast<size_t>(channel)] ? BST_CHECKED : BST_UNCHECKED, 0);
    d.syncing = false;
}

// A new plan (or failure) arrived: rows, preview, the bold "used" channel captions.
void refresh_plan(Dialog& d)
{
    const app::RoutingDialogModel& model = *d.model;
    d.syncing = true;
    d.grid.set_rows(grid_rows(model), model.selected_row());
    SetWindowTextW(d.preview, w16(model.preview()).c_str());
    for (int channel = 0; channel < 16; ++channel) {
        HFONT font = model.used()[static_cast<size_t>(channel)] ? d.ctx->ui_bold_font() : d.ctx->ui_font();
        SendMessageW(d.usable[channel], WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    d.syncing = false;
    refresh_state(d);
}

// ------------------------------------------------------------------ planner thread

void deliver(Mailbox& box, uint64_t generation, std::shared_ptr<const routing::CompiledTrackRoute> route, bool failed,
    const std::string& message, HWND window)
{
    std::lock_guard<std::mutex> lock(box.mutex);
    box.generation = generation;
    box.route = std::move(route);
    box.failed = failed;
    box.message = message;
    box.has = true;
    if (!box.closed) PostMessageW(window, WM_PLAN_DONE, 0, 0);
}

void plan_job(Mailbox& box, const routing::RoutingSong& song, const routing::TrackRoutingSettings& settings, uint64_t generation,
    HWND window)
{
    try {
        deliver(box, generation, routing::compile_route(song, settings), false, std::string(), window);
    }
    catch (const models::ModelException& exception) {
        deliver(box, generation, nullptr, true, exception.message, window);
    }
    catch (const std::bad_alloc&) {
        deliver(box, generation, nullptr, true, "There is not enough memory to calculate the routing plan.", window);
    }
    catch (...) {
        deliver(box, generation, nullptr, true, "The routing plan could not be calculated.", window);
    }
}

void handle_plan_done(Dialog& d);

void start_job(Dialog& d)
{
    if (d.running || !d.model->plan_pending()) return;
    uint64_t generation = d.model->generation();
    routing::RoutingSong song = d.model->song();
    routing::TrackRoutingSettings settings = d.model->settings();
    std::shared_ptr<Mailbox> box = d.mailbox;
    std::shared_ptr<const Song> keep = d.song;   // the planner reads this song's store
    HWND window = d.window;
    auto body = [box, keep, song, settings, generation, window]() { plan_job(*box, song, settings, generation, window); };
    platform::PlatformError error;
    std::unique_ptr<platform::Thread> thread;
    if (d.host->controller->services())
        thread = d.host->controller->services()->start_thread(body, platform::ThreadPriority::Normal, "Track routing planner", error);
    if (thread) {
        d.running = true;
        thread->detach();   // the job owns what it uses; closing the dialog never waits for it
        return;
    }
    // No worker thread available: compute here, with the wait cursor.
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    d.running = true;
    body();
    SetCursor(old);
    handle_plan_done(d);
}

void handle_plan_done(Dialog& d)
{
    uint64_t generation = 0;
    std::shared_ptr<const routing::CompiledTrackRoute> route;
    bool failed = false, has = false;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(d.mailbox->mutex);
        has = d.mailbox->has;
        if (has) {
            generation = d.mailbox->generation;
            route = std::move(d.mailbox->route);
            failed = d.mailbox->failed;
            message = d.mailbox->message;
            d.mailbox->has = false;
        }
    }
    if (!has) return;
    d.running = false;
    bool changed = failed ? d.model->plan_failed(generation, message) : d.model->plan_ready(generation, route);
    if (changed) refresh_plan(d);
    if (d.model->plan_pending()) start_job(d);   // the settings changed again while this one ran
}

// A setting changed: show the pending state and plan again.
void after_change(Dialog& d)
{
    refresh_state(d);
    start_job(d);
}

// ------------------------------------------------------------------ capacity field

void commit_capacity(Dialog& d)
{
    KillTimer(d.window, kCapacityTimer);
    wchar_t text[16] = {};
    GetWindowTextW(d.capacity, text, 15);
    wchar_t* end = nullptr;
    long value = text[0] ? wcstol(text, &end, 10) : 0;
    bool valid = text[0] != 0 && end != nullptr && *end == 0;
    if (!valid) value = d.model->capacity();
    value = std::max(1L, std::min(16L, value));
    wchar_t shown[16];
    swprintf_s(shown, L"%ld", value);
    if (wcscmp(shown, text) != 0) {
        d.syncing = true;
        SetWindowTextW(d.capacity, shown);
        d.syncing = false;
    }
    int before = d.model->capacity();
    d.model->set_capacity(static_cast<int>(value));
    if (d.model->capacity() != before) after_change(d);
}

// ------------------------------------------------------------------ layout

void layout(Dialog& d)
{
    if (!d.window || !d.grid.hwnd()) return;
    UiContext& x = *d.ctx;
    RECT c;
    GetClientRect(d.window, &c);
    const int W = c.right, H = c.bottom;
    const int pad = x.px(10), fh = x.font_height();
    const int w = std::max(10, W - 2 * pad);
    int y = pad;

    // introduction
    int introW = std::max(x.px(100), W - x.px(40));
    std::wstring introText = w16(app::RoutingDialogTexts::introduction());
    int introH = measure_wrapped(x.ui_font(), introText, introW);
    MoveWindow(d.intro, pad, y, introW, introH, TRUE);
    y += introH + x.px(8);

    // options, row 1: tier, melodic destinations, percussion (v1 table: auto, 34%, auto, 18%, auto, 48%)
    const int comboH = d.combo_h;
    const int dropH = x.px(180);
    const int gap = x.px(3);
    auto labelWidth = [&](const wchar_t* text) { return x.text_width(x.ui_font(), text) + x.px(8); };
    int c1 = std::max(labelWidth(L"Automatic tier:"), labelWidth(L"Usable output channels:"));
    int c2 = labelWidth(L"Melodic destinations:");
    int c3 = labelWidth(L"Percussion:");
    int remain = std::max(x.px(60), w - c1 - c2 - c3);
    int w1 = remain * 34 / 100, w2 = remain * 18 / 100, w3 = remain - w1 - w2;
    int labelY = y + (comboH - fh) / 2;
    int cx = pad;
    MoveWindow(d.tier_label, cx, labelY, c1, fh + 2, TRUE);
    cx += c1;
    MoveWindow(d.tier, cx, y, std::max(10, w1 - gap), comboH + dropH, TRUE);
    cx += w1;
    MoveWindow(d.capacity_label, cx, labelY, c2, fh + 2, TRUE);
    cx += c2;
    MoveWindow(d.capacity, cx, y, std::max(10, w2 - gap), comboH, TRUE);
    SendMessageW(d.spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(d.capacity), 0);   // re-attach: the spin follows the field
    cx += w2;
    MoveWindow(d.percussion_label, cx, labelY, c3, fh + 2, TRUE);
    cx += c3;
    MoveWindow(d.percussion, cx, y, std::max(10, w3), comboH + dropH, TRUE);
    y += comboH + x.px(6);

    // options, row 2: the 16 usable-channel boxes, wrapped
    int flowX = pad + c1, flowW = std::max(x.px(60), w - c1);
    int boxW = x.text_width(x.ui_bold_font(), L"16") + x.px(18) + x.px(10);   // DD-099
    int boxH = std::max(fh + 2, x.px(18));
    MoveWindow(d.usable_label, pad, y + x.px(4) + (boxH - fh) / 2, c1, fh + 2, TRUE);
    int bx = 0, by = x.px(4), lineH = boxH + x.px(2);
    for (int channel = 0; channel < 16; ++channel) {
        if (bx + boxW > flowW && bx > 0) { bx = 0; by += lineH; }
        MoveWindow(d.usable[channel], flowX + bx, y + by, boxW, boxH, TRUE);
        bx += boxW;
    }
    y += by + boxH + x.px(8);
    const int splitTop = y;

    // bottom area: status line, then confirmation and buttons
    int bh = fh + x.button_padding(10);   // DD-099
    auto buttonWidth = [&](const wchar_t* text) { return std::max(x.px(75), x.text_width(x.ui_font(), text) + x.px(24)); };
    int originalW = buttonWidth(L"Use original routing"), applyW = buttonWidth(L"Apply"), cancelW = buttonWidth(L"Cancel");
    int buttonGap = x.px(6);
    int buttonsW = originalW + applyW + cancelW + 2 * buttonGap;
    int confirmW = std::max(x.px(120), w - buttonsW - x.px(12));
    int confirmH = measure_wrapped(x.ui_font(), w16(app::RoutingDialogTexts::confirm_text()), confirmW - x.px(20)) + x.px(4);
    int rowH = std::max(bh, confirmH);
    int statusH = fh + x.px(6);
    int bottomH = x.px(8) + statusH + x.px(4) + rowH;
    int bottomY = std::max(splitTop + x.px(60), H - pad - bottomH);
    MoveWindow(d.status, pad, bottomY + x.px(8), w, statusH, TRUE);
    int rowY = bottomY + x.px(8) + statusH + x.px(4);
    MoveWindow(d.confirm, pad, rowY, confirmW, rowH, TRUE);
    int bxr = pad + w - cancelW;
    int buttonY = rowY + (rowH - bh) / 2;
    MoveWindow(d.cancel, bxr, buttonY, cancelW, bh, TRUE);
    bxr -= buttonGap + applyW;
    MoveWindow(d.apply, bxr, buttonY, applyW, bh, TRUE);
    bxr -= buttonGap + originalW;
    MoveWindow(d.original, bxr, buttonY, originalW, bh, TRUE);

    // the split: lanes on the left, preview on the right
    int splitH = std::max(x.px(60), bottomY - splitTop);
    int barW = d.bar_width;
    int minLeft = x.px(420), minRight = x.px(220);
    int available = std::max(10, w - barW);
    int left = static_cast<int>(d.split_ratio * w + 0.5);
    left = std::min(left, available - minRight);
    left = std::max(left, std::min(minLeft, available - 10));
    left = std::max(10, std::min(left, available - 10));
    d.bar.left = pad + left;
    d.bar.right = d.bar.left + barW;
    d.bar.top = splitTop;
    d.bar.bottom = splitTop + splitH;
    int rightX = d.bar.right, rightW = pad + w - rightX;

    int headingH = fh + x.px(4);
    MoveWindow(d.lanes_heading, pad, splitTop, left, headingH, TRUE);
    int laneRowH = comboH + x.px(6);
    int gridH = std::max(x.px(40), splitH - headingH - laneRowH);
    MoveWindow(d.grid.hwnd(), pad, splitTop + headingH, left, gridH, TRUE);
    int captionW = std::min(left / 2, x.text_width(x.ui_font(), w16(app::RoutingDialogTexts::lane_caption())) + x.px(10));
    int laneY = splitTop + headingH + gridH + x.px(3);
    MoveWindow(d.caption, pad, laneY + (comboH - fh) / 2, captionW, fh + 2, TRUE);
    MoveWindow(d.destination, pad + captionW, laneY, std::max(10, left - captionW), comboH + dropH, TRUE);
    d.grid.fit_columns();

    MoveWindow(d.preview_heading, rightX, splitTop, rightW, headingH, TRUE);
    MoveWindow(d.preview, rightX, splitTop + headingH, rightW, std::max(x.px(40), splitH - headingH), TRUE);
    InvalidateRect(d.window, nullptr, TRUE);
}

// ------------------------------------------------------------------ test hooks

bool render_to_bmp(HWND window, const wchar_t* path)
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
    // Render from the window's own paint code (never the screen): the background, then every
    // visible child from the bottom of the z-order up.
    RECT whole = { 0, 0, w, h };
    FillRect(dc, &whole, GetSysColorBrush(COLOR_BTNFACE));
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
    for (HWND child = GetWindow(GetWindow(window, GW_CHILD), GW_HWNDLAST); child; child = GetWindow(child, GW_HWNDPREV)) {
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT cr;
        GetWindowRect(child, &cr);
        POINT origin = { cr.left, cr.top };
        ScreenToClient(window, &origin);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        SetViewportOrgEx(dc, 0, 0, nullptr);
    }
    BITMAPFILEHEADER header = {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        ok = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
            WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr) &&
            WriteFile(file, bits, static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4, &written, nullptr);
        CloseHandle(file);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}

// ------------------------------------------------------------------ the window

void create_controls(Dialog& d)
{
    namespace T = app;
    d.mono = make_mono_font(*d.ctx);

    d.intro = make_label(d, T::RoutingDialogTexts::introduction());
    d.tier_label = make_label(d, u"Automatic tier:");
    d.tier = make_combo(d, RdTier);
    for (int i = 0; i < 3; ++i)
        SendMessageW(d.tier, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w16(T::RoutingDialogTexts::tier_item(i)).c_str()));
    d.capacity_label = make_label(d, u"Melodic destinations:");
    d.capacity = make_child(d, L"EDIT", L"16", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, RdCapacity, WS_EX_CLIENTEDGE);
    d.spin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
        0, 0, 0, 0, d.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(RdCapacitySpin)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(d.spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(d.capacity), 0);
    SendMessageW(d.spin, UDM_SETRANGE32, 1, 16);
    SendMessageW(d.capacity, EM_LIMITTEXT, 2, 0);
    d.percussion_label = make_label(d, u"Percussion:");
    d.percussion = make_combo(d, RdPercussion);
    for (int i = 0; i < 2; ++i)
        SendMessageW(d.percussion, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w16(T::RoutingDialogTexts::percussion_item(i)).c_str()));
    d.usable_label = make_label(d, u"Usable output channels:");
    for (int channel = 0; channel < 16; ++channel) {
        wchar_t label[8];
        swprintf_s(label, L"%d", channel + 1);
        d.usable[channel] = make_child(d, L"BUTTON", label, BS_AUTOCHECKBOX | WS_TABSTOP, RdUsableFirst + channel);
    }

    d.lanes_heading = make_label(d, T::RoutingDialogTexts::lanes_heading(), true);
    d.tips.create(d.window);
    d.grid.create(d.window, RdLanes, d.ctx, d.tips.window());
    d.grid.set_selection_callback([&d](int row) {
        d.model->select_row(row);
        refresh_state(d);
    });
    d.caption = make_label(d, T::RoutingDialogTexts::lane_caption(), false, true);
    d.destination = make_combo(d, RdDestination);
    for (int i = 0; i <= 16; ++i)
        SendMessageW(d.destination, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w16(T::RoutingDialogTexts::destination_item(i)).c_str()));

    d.preview_heading = make_label(d, T::RoutingDialogTexts::preview_heading(), true);
    d.preview = make_child(d, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
        RdPreview, WS_EX_CLIENTEDGE);
    SendMessageW(d.preview, WM_SETFONT, reinterpret_cast<WPARAM>(d.mono), TRUE);
    SendMessageW(d.preview, EM_SETLIMITTEXT, 0, 0);

    d.status = make_child(d, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, RdStatus);
    d.confirm = make_child(d, L"BUTTON", w16(T::RoutingDialogTexts::confirm_text()).c_str(), BS_AUTOCHECKBOX | BS_MULTILINE | BS_VCENTER | WS_TABSTOP,
        RdConfirm);
    d.original = make_child(d, L"BUTTON", L"Use original routing", BS_PUSHBUTTON | WS_TABSTOP, RdOriginal);
    d.apply = make_child(d, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
    d.cancel = make_child(d, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

    // Tooltips: v1 gives the correction selector and its caption the help text; the grid gets per-cell tips.
    d.tips.add_control(d.caption);
    d.tips.add_control(d.destination);
    d.tips.add_control(d.grid.hwnd());

    RECT closed;
    GetWindowRect(d.tier, &closed);
    d.combo_h = closed.bottom - closed.top;
    // A native drop-down list reports its closed height once created with the font; keep it sane.
    d.combo_h = std::max(d.combo_h, d.ctx->font_height() + d.ctx->px(8));
    d.combo_h = std::min(d.combo_h, d.ctx->font_height() + d.ctx->px(16));
    d.bar_width = d.ctx->px(5);
}

LRESULT CALLBACK dialog_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Dialog* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (!d || !d->model || !d->grid.hwnd()) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) layout(*d);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = d->min_width;
        info->ptMinTrackSize.y = d->min_height;
        return 0;
    }
    case WM_PLAN_DONE:
        handle_plan_done(*d);
        return 0;
    case DM_GETDEFID:
        if (d->model->apply_enabled()) return MAKELRESULT(IDOK, DC_HASDEFID);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (d->syncing) return 0;
        if (id == RdTier && code == CBN_SELCHANGE) {
            d->model->set_tier(static_cast<int>(SendMessageW(d->tier, CB_GETCURSEL, 0, 0)));
            after_change(*d);
        }
        else if (id == RdPercussion && code == CBN_SELCHANGE) {
            d->model->set_percussion(static_cast<int>(SendMessageW(d->percussion, CB_GETCURSEL, 0, 0)));
            after_change(*d);
        }
        else if (id == RdCapacity && code == EN_CHANGE) SetTimer(window, kCapacityTimer, 400, nullptr);
        else if (id == RdCapacity && code == EN_KILLFOCUS) commit_capacity(*d);
        else if (id >= RdUsableFirst && id < RdUsableFirst + 16 && code == BN_CLICKED) {
            int channel = id - RdUsableFirst;
            d->model->set_usable(channel, SendMessageW(d->usable[channel], BM_GETCHECK, 0, 0) == BST_CHECKED);
            after_change(*d);
        }
        else if (id == RdDestination && code == CBN_SELCHANGE) {
            d->model->set_selected_lane_destination(static_cast<int>(SendMessageW(d->destination, CB_GETCURSEL, 0, 0)));
            after_change(*d);
        }
        else if (id == RdConfirm && code == BN_CLICKED) {
            d->model->set_lossy_confirmed(SendMessageW(d->confirm, BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_state(*d);
        }
        else if (id == RdOriginal && code == BN_CLICKED) {
            if (d->model->use_original()) d->done = true;
        }
        else if (id == IDOK) {
            // A capacity typed a moment ago is committed first; that starts a new plan, so Apply waits for it.
            if (GetFocus() == d->capacity) commit_capacity(*d);
            else if (d->model->apply()) { d->apply_requested = true; d->done = true; }
        }
        else if (id == IDCANCEL) d->done = true;
        return 0;
    }
    case WM_TIMER:
        if (wParam == kCapacityTimer) { commit_capacity(*d); return 0; }
        break;
    case WM_NOTIFY: {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        LRESULT result = 0;
        if (header->idFrom == RdLanes && d->grid.handle_notify(header, &result)) return result;
        if (header->idFrom == RdCapacitySpin && header->code == UDN_DELTAPOS) {
            NMUPDOWN* delta = reinterpret_cast<NMUPDOWN*>(lParam);
            int target = std::max(1, std::min(16, delta->iPos + delta->iDelta));
            int before = d->model->capacity();
            d->model->set_capacity(target);
            if (d->model->capacity() != before) after_change(*d);
            return 0;
        }
        if (d->tips.handle_notify(header, [d](HWND control) -> std::wstring {
                if (control == d->grid.hwnd()) return d->grid.hover_text();
                return w16(app::RoutingDialogTexts::correction_help());
            }))
            return 0;
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HWND control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (control == d->preview) {
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        if (control == d->status && d->model->status_is_error()) {
            SetTextColor(dc, RGB(139, 0, 0));   // DarkRed
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        if (PtInRect(&d->bar, p)) { SetCapture(window); d->dragging = true; return 0; }
        break;
    }
    case WM_MOUSEMOVE:
        if (d->dragging) {
            RECT c;
            GetClientRect(window, &c);
            int pad = d->ctx->px(10);
            int w = std::max(10, static_cast<int>(c.right) - 2 * pad);
            int x = static_cast<short>(LOWORD(lParam)) - pad - d->bar_width / 2;
            d->split_ratio = std::max(0.05, std::min(0.95, static_cast<double>(x) / w));
            layout(*d);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (d->dragging) { d->dragging = false; ReleaseCapture(); return 0; }
        break;
    case WM_CAPTURECHANGED:
        d->dragging = false;
        break;
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(window, &p);
        if (LOWORD(lParam) == HTCLIENT && (d->dragging || PtInRect(&d->bar, p))) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (LOWORD(lParam) == HTCLIENT && d->model->plan_pending()) {
            SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));   // arrow with the busy indicator: the window still works
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        d->done = true;
        return 0;
    case WM_ROUTING_STATE: {
        LRESULT bits = 0;
        if (d->model->plan_pending()) bits |= 1;
        if (d->model->apply_enabled()) bits |= 2;
        if (d->model->lossy_confirmation_visible()) bits |= 4;
        if (d->model->plan_has_failed()) bits |= 8;
        if (d->done) bits |= 16;
        return bits;
    }
    case WM_ROUTING_ROWS:
        return d->grid.row_count();
    case WM_ROUTING_SCREENSHOT:
        return render_to_bmp(window, reinterpret_cast<const wchar_t*>(lParam)) ? 1 : 0;
    case WM_DESTROY:
        KillTimer(window, kCapacityTimer);
        {
            std::lock_guard<std::mutex> lock(d->mailbox->mutex);
            d->mailbox->closed = true;
        }
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void register_class()
{
    if (g_class_registered) return;
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = dialog_proc;
    wc.lpszClassName = kRoutingDialogClass;
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    g_class_registered = true;
}

} // namespace

void show_track_routing_dialog(CompanionHost& host)
{
    app::PlayerController& controller = *host.controller;
    std::shared_ptr<const Song> song = controller.song();
    if (!song || !controller.track_routing_available()) return;
    register_class();

    Dialog d;
    d.host = &host;
    d.ctx = host.ctx;
    d.song = song;
    UiContext& ctx = *host.ctx;
    d.model.reset(new app::RoutingDialogModel(controller.routing_song(), controller.track_routing_settings(),
        controller.track_route() != nullptr, controller.culture()));

    // v1: client min(1050, max(720, work width - 80)) by min(690, max(520, work height - 80)), scaled. v1 measured the
    // PRIMARY monitor's work area; v2 measures the main window's monitor, where the dialog opens (DD-098).
    HWND owner = host.main_window;
    RECT work = monitor_work_area(owner);
    int clientW = std::min(ctx.px(1050), std::max(ctx.px(720), static_cast<int>(work.right - work.left) - ctx.px(80)));
    int clientH = std::min(ctx.px(690), std::max(ctx.px(520), static_cast<int>(work.bottom - work.top) - ctx.px(80)));
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    RECT frame = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&frame, style, FALSE, 0);
    int outerW = frame.right - frame.left, outerH = frame.bottom - frame.top;
    d.min_width = std::min(ctx.px(760), outerW);
    d.min_height = std::min(ctx.px(540), outerH);
    {
        // The splitter starts where v1's SplitContainer does: 60% of the width, at least 560, within the minimum panels.
        int splitWidth = std::max(ctx.px(720), clientW - ctx.px(20));
        int distance = std::max(ctx.px(420), std::min(splitWidth - ctx.px(220), std::max(ctx.px(560), splitWidth * 3 / 5)));
        d.split_ratio = static_cast<double>(distance) / std::max(1, clientW - 2 * ctx.px(10));
    }

    // v1 CenterParent: centred over the main window, inside the work area of the main window's monitor (DD-098; it
    // was kept inside the primary monitor's work area, which moved it onto the primary monitor).
    POINT at = centre_on_owner(owner, outerW, outerH);
    d.window = CreateWindowExW(0, kRoutingDialogClass, w16(app::RoutingDialogTexts::title()).c_str(), style, at.x, at.y, outerW, outerH, owner,
        nullptr, GetModuleHandleW(nullptr), &d);
    if (!d.window) return;
    create_controls(d);
    refresh_option_controls(d);
    refresh_state(d);
    layout(d);

    bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d.window, SW_SHOW);
    SetFocus(d.tier);
    start_job(d);
    MSG message;
    while (!d.done) {
        BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) { if (got == 0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_F1 && owner && GetAncestor(message.hwnd, GA_ROOT) == d.window) {
            // F1: Help on the topic that covers Track routing ("File and output"); the main window opens it
            PostMessageW(owner, WM_MIDIBN_HELP, static_cast<WPARAM>(app::HelpTopic::FileAndOutput), 0);
            continue;
        }
        if (!IsDialogMessageW(d.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner && ownerWasEnabled) EnableWindow(owner, TRUE);
    app::RoutingDialogOutcome outcome = d.model->outcome();
    DestroyWindow(d.window);
    d.window = nullptr;
    if (owner) SetForegroundWindow(owner);

    // Hand the result to the controller (v1 ShowTrackRoutingDialog / ApplyTrackRoute).
    if (outcome.kind == app::RoutingDialogOutcome::None) return;
    std::string error;
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = outcome.kind == app::RoutingDialogOutcome::UseOriginal
        ? controller.apply_track_route(nullptr, false, nullptr, error)
        : controller.apply_track_route(outcome.route, outcome.explicitly_confirmed, &outcome.settings, error);
    SetCursor(oldCursor);
    if (!ok) show_message(owner, "Track routing", app::join_error_text(app::explain_error(error, "The track routing could not be applied.")), 2);
}

} // namespace ui
} // namespace midibn
