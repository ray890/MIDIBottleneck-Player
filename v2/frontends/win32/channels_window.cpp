// The Channels window (v1 ChannelMonitorForm). See channels_window.h.
//
// Presentation differences from v1 (all deliberate improvements; listed in the milestone report):
//  * columns also widen to fit their values (v1 sized them to their headings only and cut "123,456" to "123...");
//  * no cell-selection highlight (the editor shows which cell is being edited);
//  * right-click "Auto" failures show a tip under the cell (v1 dropped the error); a stale error is cleared
//    from the editor's tooltip after a later success;
//  * minimizing does not switch auto-fit off (only a user resize or maximize does).
#include "channels_window.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>

namespace midibn {
namespace ui {

int g_channels_window_show_command = SW_SHOW;

namespace {

const wchar_t kChannelsClass[] = L"MIDIBottleneckV2Channels";
const UINT kCmdAlwaysOnTop = 0x1F30;   // v1 SystemMenuAlwaysOnTop
const UINT_PTR kFollowUpTimer = 0x7201;

std::string file_name_of(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::unique_ptr<app::ChannelSession> make_session(CompanionHost& host, app::ChannelEngineAccess* access,
    const std::shared_ptr<std::atomic<HWND> >& target)
{
    app::ChannelSession::Host sh;
    sh.engine = access;
    sh.culture = host.controller->culture();
    sh.wake = [target] {
        HWND h = target->load();
        if (h) PostMessageW(h, WM_MIDIBN_CHANNELS_WAKE, 0, 0);
    };
    app::PlayerController* controller = host.controller;
    sh.overrides_changed = [controller, target] {
        controller->notify_overrides_changed();
        HWND h = target->load();
        if (h) PostMessageW(h, WM_MIDIBN_CHANNELS_REFRESH, 0, 0);
    };
    return std::unique_ptr<app::ChannelSession>(new app::ChannelSession(sh));
}

} // namespace

ChannelsWindow::ChannelsWindow(CompanionHost& host)
    : host_(host),
      wake_target_(std::make_shared<std::atomic<HWND> >(static_cast<HWND>(nullptr))),
      access_(new app::PlaybackEngineChannels(host.controller->engine())),
      session_(make_session(host, access_.get(), wake_target_)),
      grid_(host.ctx, session_.get())
{
}

ChannelsWindow::~ChannelsWindow()
{
    wake_target_->store(nullptr);
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
}

std::unique_ptr<ChannelsWindow> ChannelsWindow::create(CompanionHost& host)
{
    if (!host.controller || !host.ctx || !host.controller->has_song()) return nullptr;
    std::unique_ptr<ChannelsWindow> window(new ChannelsWindow(host));
    if (!window->initialize()) return nullptr;
    return window;
}

double ChannelsWindow::scale_factor() const
{
    return (host_.ctx->dpi() / 96.0) * (host_.ctx->scale_percent() / 100.0);
}

RECT ChannelsWindow::work_area() const
{
    return monitor_work_area(window_);   // v1 Screen.FromControl(this)
}

bool ChannelsWindow::initialize()
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kChannelsClass, &existing)) {
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ChannelsWindow::window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kChannelsClass;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
    }
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    UiContext* ctx = host_.ctx;
    RECT rect = { 0, 0, ctx->px(1240), ctx->px(430) };
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    // v1: 28 pixels right of and below the main window's corner, kept inside the working area of its screen
    // (Screen.FromControl). DD-097: that is the main window's monitor, not always the primary one (the window used to
    // be pulled onto the primary monitor when the main window was on another one).
    RECT work = monitor_work_area(host_.main_window);   // the primary monitor's when there is no main window
    RECT main = {};
    int x = work.left + 28, y = work.top + 28;
    if (host_.main_window && IsWindow(host_.main_window) && GetWindowRect(host_.main_window, &main)) {
        x = std::max<int>(work.left, main.left + 28);
        y = std::max<int>(work.top, main.top + 28);
    }
    x = std::min<int>(x, work.right - width);
    y = std::min<int>(y, work.bottom - height);
    CreateWindowExW(0, kChannelsClass, L"", style, std::max<int>(work.left, x), std::max<int>(work.top, y), width, height, nullptr, nullptr,
        instance, this);
    if (!window_) return false;
    wake_target_->store(window_);
    // A failure reported before this window existed is not its business.
    host_.controller->take_channel_control_failures();
    session_->start(build_info());
    grid_.apply_header_preferred_widths();
    process_session_output();
    layout();
    fit_window_to_grid(true);
    ShowWindow(window_, g_channels_window_show_command);
    UpdateWindow(window_);
    settled_ = true;
    return true;
}

app::ChannelSongInfo ChannelsWindow::build_info()
{
    app::PlayerController* controller = host_.controller;
    app::ChannelSongInfo info;
    info.loading = controller->loading();
    info.has_song = controller->has_song();
    info.file_name = file_name_of(controller->song_path());
    info.song_revision = controller->song_revision();
    info.absent_message = controller->no_song_message();
    info.chase_enabled = controller->chase_enabled();
    info.position_us = controller->playback_position_for_readouts();
    info.source = controller->effective_song().get();
    return info;
}

// ---------------------------------------------------------------- following the player

void ChannelsWindow::on_refresh()
{
    if (!window_ || closing_) return;
    std::vector<app::ChannelControlFailure> failures = host_.controller->take_channel_control_failures();
    for (size_t i = 0; i < failures.size(); ++i)
        session_->control_failed(failures[i].channel, failures[i].attribute, failures[i].message, failures[i].is_mute);
    session_->sync(build_info());
    session_->pump();
    process_session_output();
}

void ChannelsWindow::process_session_output()
{
    if (!window_) return;
    if (session_->view_revision() != seen_view_revision_) {
        seen_view_revision_ = session_->view_revision();
        const app::ChannelGridModel& model = session_->model();
        SetWindowTextW(window_, W(model.title()).c_str());
        grid_.set_detached(model.detached(), model.detached_message());
        EnableWindow(explanation_, model.detached() ? FALSE : TRUE);
    }
    if (!session_->editor().is_open() && grid_.editor_visible()) grid_.hide_editor(false);
    grid_.apply_changes(session_->take_changes());
    std::vector<app::ChannelFeedback> feedback = session_->take_feedback();
    for (size_t i = 0; i < feedback.size(); ++i) grid_.show_feedback(feedback[i]);
    update_timers();
}

void ChannelsWindow::update_timers()
{
    bool want = session_->follow_up_active();
    if (want && !follow_up_timer_) {
        SetTimer(window_, kFollowUpTimer, 25, nullptr);
        follow_up_timer_ = true;
    }
    else if (!want && follow_up_timer_) {
        KillTimer(window_, kFollowUpTimer);
        follow_up_timer_ = false;
    }
}

void ChannelsWindow::close()
{
    closing_ = true;
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
}

// ---------------------------------------------------------------- layout

int ChannelsWindow::label_height(int width)
{
    HDC dc = GetDC(window_);
    HGDIOBJ old = SelectObject(dc, host_.ctx->ui_font());
    RECT r = { 0, 0, std::max(1, width), 0 };
    DrawTextW(dc, W(app::ChannelGridModel::explanation()).c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(window_, dc);
    return r.bottom;
}

void ChannelsWindow::layout()
{
    if (!window_ || !explanation_) return;
    UiContext* ctx = host_.ctx;
    RECT client;
    GetClientRect(window_, &client);
    int pad = ctx->px(6), marginSide = ctx->px(1), marginTop = ctx->px(5), marginBottom = ctx->px(1);
    int labelWidth = std::max(1, static_cast<int>(client.right) - 2 * pad - 2 * marginSide);
    int labelH = label_height(labelWidth);
    int labelY = client.bottom - pad - marginBottom - labelH;
    int gridH = std::max(1, labelY - marginTop - pad);
    MoveWindow(grid_.hwnd(), pad, pad, std::max(1, static_cast<int>(client.right) - 2 * pad), gridH, TRUE);
    RECT before = {};
    GetClientRect(explanation_, &before);
    MoveWindow(explanation_, pad + marginSide, labelY, labelWidth, labelH, TRUE);
    // DD-099: the whole explanation is repainted after a size change. The STATIC keeps its old pixels and repaints only
    // the newly uncovered strip, so when the window grew to fit the grid the line wrapped for the earlier, narrower
    // width stayed on the screen: at 50 % its last word ("historical") was missing and only the "." showed.
    if (before.right != labelWidth || before.bottom != labelH) InvalidateRect(explanation_, nullptr, TRUE);
}

int ChannelsWindow::outer_width_for_client(int clientWidth) const
{
    RECT wr, cr;
    GetWindowRect(window_, &wr);
    GetClientRect(window_, &cr);
    return clientWidth + (wr.right - wr.left) - cr.right;
}

void ChannelsWindow::fit_window_to_grid(bool includeHeight)
{
    if (!auto_fit_ || !window_ || IsIconic(window_) || IsZoomed(window_)) return;
    UiContext* ctx = host_.ctx;
    int gridWidth = grid_.content_width() + ctx->px(3);
    int rowsHeight = grid_.content_height() + ctx->px(3);
    RECT work = work_area();
    RECT wr, cr;
    GetWindowRect(window_, &wr);
    GetClientRect(window_, &cr);
    int ncW = (wr.right - wr.left) - cr.right, ncH = (wr.bottom - wr.top) - cr.bottom;
    int maxClientW = std::max<int>(1, (work.right - work.left) - ncW);
    int maxClientH = std::max<int>(1, (work.bottom - work.top) - ncH);
    // DD-097: the margins are the ones layout() uses (two pads of px(6)), not px(12): at 75 % and
    // 125 % px(12) is one pixel less than 2 * px(6), the grid came out one pixel narrower than its columns, a
    // horizontal scroll bar appeared, and with it a vertical one (the Position column and channel 16 were cut off).
    const int pads = 2 * ctx->px(6);
    int clientW = std::min<int>(std::max(ctx->px(420), gridWidth + pads), maxClientW);
    int labelW = std::max(1, clientW - ctx->px(14) - ctx->px(2));
    int horizontalScroll = clientW < gridWidth + pads ? GetSystemMetrics(SM_CYHSCROLL) : 0;
    int desiredH = rowsHeight + horizontalScroll + label_height(labelW) + ctx->px(5) + ctx->px(1) + pads + (ctx->scale_percent() == 100 ? 0 : 4);
    int clientH = includeHeight ? std::min<int>(std::max(ctx->px(300), desiredH), maxClientH) : cr.bottom;
    int outerW = clientW + ncW, outerH = clientH + ncH;
    int x = std::max<int>(work.left, std::min<int>(wr.left, work.right - outerW));
    int y = std::max<int>(work.top, std::min<int>(wr.top, work.bottom - outerH));
    fitting_ = true;
    SetWindowPos(window_, nullptr, x, y, outerW, outerH, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    fitting_ = false;
}

void ChannelsWindow::clamp_to_work_area()
{
    if (!window_ || IsIconic(window_) || IsZoomed(window_)) return;
    RECT work = work_area(), wr;
    GetWindowRect(window_, &wr);
    int x = std::max<int>(work.left, std::min<int>(wr.left, work.right - (wr.right - wr.left)));
    int y = std::max<int>(work.top, std::min<int>(wr.top, work.bottom - (wr.bottom - wr.top)));
    if (x != wr.left || y != wr.top) SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ChannelsWindow::on_scale_changed()
{
    if (!window_) return;
    UiContext* ctx = host_.ctx;
    bool wasAutoFit = auto_fit_;
    fitting_ = true;
    SendMessageW(explanation_, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->ui_font()), TRUE);
    grid_.on_scale_changed();
    if (!wasAutoFit && !IsIconic(window_) && !IsZoomed(window_)) {
        RECT work = work_area(), wr, cr;
        GetWindowRect(window_, &wr);
        GetClientRect(window_, &cr);
        int ncW = (wr.right - wr.left) - cr.right, ncH = (wr.bottom - wr.top) - cr.bottom;
        int clientW = std::min<int>(std::max<int>(1, (work.right - work.left) - ncW),
            std::max(1, static_cast<int>(std::lround(canonical_client_width_ * scale_factor()))));
        int clientH = std::min<int>(std::max<int>(1, (work.bottom - work.top) - ncH),
            std::max(1, static_cast<int>(std::lround(canonical_client_height_ * scale_factor()))));
        SetWindowPos(window_, nullptr, 0, 0, clientW + ncW, clientH + ncH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout();
    fitting_ = false;
    if (wasAutoFit) fit_window_to_grid(true);
    session_->rebuild();
    process_session_output();
    clamp_to_work_area();
}

void ChannelsWindow::note_manual_resize()
{
    auto_fit_ = false;
    RECT cr;
    GetClientRect(window_, &cr);
    double f = scale_factor();
    canonical_client_width_ = cr.right / f;
    canonical_client_height_ = cr.bottom / f;
}

void ChannelsWindow::set_always_on_top(bool on)
{
    always_on_top_ = on;
    SetWindowPos(window_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    HMENU menu = GetSystemMenu(window_, FALSE);
    if (menu) CheckMenuItem(menu, kCmdAlwaysOnTop, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

std::wstring ChannelsWindow::explanation_text() const
{
    int length = GetWindowTextLengthW(explanation_);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(explanation_, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

// ---------------------------------------------------------------- screenshot

namespace {

// Prints `parent`'s visible children (and theirs) into `dc`, bottom of the z-order first. `root` is the window
// whose client area the DC represents.
void print_children(HWND root, HWND parent, HDC dc)
{
    for (HWND child = GetWindow(GetWindow(parent, GW_CHILD), GW_HWNDLAST); child; child = GetWindow(child, GW_HWNDPREV)) {
        if ((GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE) == 0) continue;
        RECT cr;
        GetWindowRect(child, &cr);
        POINT origin = { cr.left, cr.top };
        ScreenToClient(root, &origin);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        SetViewportOrgEx(dc, 0, 0, nullptr);
        print_children(root, child, dc);
    }
}

} // namespace

bool ChannelsWindow::write_bitmap(const std::wstring& path)
{
    RECT r;
    GetClientRect(window_, &r);
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
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    print_children(window_, window_, dc);
    BITMAPFILEHEADER header = {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD written;
        WriteFile(file, &header, sizeof(header), &written, nullptr);
        WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
        WriteFile(file, bits, static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4, &written, nullptr);
        CloseHandle(file);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}

// ---------------------------------------------------------------- window procedure

LRESULT CALLBACK ChannelsWindow::window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ChannelsWindow* self = reinterpret_cast<ChannelsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ChannelsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    LRESULT result = self->handle(message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        self->window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return result;
}

void ChannelsWindow::build_children()
{
    UiContext* ctx = host_.ctx;
    grid_.create(window_);
    explanation_ = CreateWindowExW(0, L"STATIC", W(app::ChannelGridModel::explanation()).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0,
        100, 20, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(explanation_, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->ui_font()), TRUE);
    HMENU menu = GetSystemMenu(window_, FALSE);
    if (menu) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdAlwaysOnTop, L"Always on top");
    }
}

LRESULT ChannelsWindow::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        build_children();
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        layout();
        if (settled_ && !fitting_ && wParam == SIZE_MAXIMIZED) auto_fit_ = false;
        return 0;
    case WM_ENTERSIZEMOVE: {
        in_size_move_ = true;
        RECT wr;
        GetWindowRect(window_, &wr);
        size_at_enter_.cx = wr.right - wr.left;
        size_at_enter_.cy = wr.bottom - wr.top;
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        in_size_move_ = false;
        RECT wr;
        GetWindowRect(window_, &wr);
        if (settled_ && !fitting_ && ((wr.right - wr.left) != size_at_enter_.cx || (wr.bottom - wr.top) != size_at_enter_.cy)) note_manual_resize();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT work = work_area();
        info->ptMinTrackSize.x = std::min<int>(host_.ctx->px(780), work.right - work.left);
        info->ptMinTrackSize.y = std::min<int>(host_.ctx->px(400), work.bottom - work.top);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) grid_.hide_editor(true);   // v1 Deactivate
        break;
    case WM_SETFOCUS:
        // The grid is the only control that takes the keyboard (arrow keys, Enter, F2, Space; DD-031).
        if (grid_.hwnd() && IsWindowVisible(grid_.hwnd())) SetFocus(grid_.hwnd());
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == kCmdAlwaysOnTop) {
            set_always_on_top(!always_on_top_);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        break;
    case WM_PRINTCLIENT: {
        RECT r;
        GetClientRect(window_, &r);
        FillRect(reinterpret_cast<HDC>(wParam), &r, GetSysColorBrush(COLOR_BTNFACE));
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (reinterpret_cast<HWND>(lParam) == explanation_) {
            SetTextColor(dc, IsWindowEnabled(explanation_) ? RGB(105, 105, 105) : host_.ctx->disabled_text_color());
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    }
    case WM_MIDIBN_GRIDLAYOUT:
        if (auto_fit_) fit_window_to_grid(true);
        return 0;
    case WM_MIDIBN_GRIDACTION:
        process_session_output();
        return 0;
    case WM_MIDIBN_CHANNELS_WAKE:
        session_->pump();
        process_session_output();
        return 0;
    case WM_MIDIBN_CHANNELS_REFRESH:
        if (host_.request_refresh) host_.request_refresh();
        return 0;
    case WM_TIMER:
        if (wParam == kFollowUpTimer) {
            on_refresh();
            session_->follow_up_tick();
            update_timers();
            return 0;
        }
        break;
    case WM_CLOSE:
        closing_ = true;
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        wake_target_->store(nullptr);
        if (follow_up_timer_) KillTimer(window_, kFollowUpTimer);
        follow_up_timer_ = false;
        session_->stop();   // v1 FormClosed: monitoring off
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

std::unique_ptr<CompanionWindow> create_channels_window(CompanionHost& host)
{
    return std::unique_ptr<CompanionWindow>(ChannelsWindow::create(host).release());
}

} // namespace ui
} // namespace midibn
