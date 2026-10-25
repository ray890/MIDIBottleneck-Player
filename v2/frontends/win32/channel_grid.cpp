// The Channels window's grid and in-place editor. See channel_grid.h.
#include "channel_grid.h"

#include "midibn/app/channel_navigation.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace midibn {
namespace ui {

namespace {

const wchar_t kGridClass[] = L"MidiBnChannelGrid";
const UINT_PTR kEditorSubclass = 0x5C2C;
const UINT_PTR kTipTimer = 0x7101;
const UINT_PTR kTransientToolId = 0xF00D;
const COLORREF kForcedColor = RGB(20, 75, 155);          // v1 Color.FromArgb(20, 75, 155)
const COLORREF kHistoricalColor = RGB(105, 105, 105);    // Color.DimGray
const COLORREF kGridLineColor = RGB(160, 160, 160);      // SystemColors.ControlDark
const COLORREF kHeaderSeparatorColor = RGB(229, 229, 229);

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

std::wstring window_text(HWND window)
{
    int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void set_window_text_if_changed(HWND window, const std::wstring& text)
{
    if (window_text(window) != text) SetWindowTextW(window, text.c_str());
}

} // namespace

ChannelGrid::ChannelGrid(UiContext* ctx, app::ChannelSession* session)
    : ctx_(ctx), session_(session)
{
    for (int i = 0; i < app::kChannelColumnCount; ++i) {
        widths_[i] = 40;
        user_canonical_[i] = 0;
        auto_widths_[i] = 40;
    }
}

ChannelGrid::~ChannelGrid()
{
    if (italic_) DeleteObject(italic_);
}

double ChannelGrid::scale_factor() const
{
    return (ctx_->dpi() / 96.0) * (ctx_->scale_percent() / 100.0);
}

HWND ChannelGrid::create(HWND parent)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kGridClass, &existing)) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &ChannelGrid::window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kGridClass;
        RegisterClassExW(&wc);
    }
    // The window text is never drawn; it is the grid's accessible name (screen readers read it).
    window_ = CreateWindowExW(0, kGridClass, L"MIDI channel grid", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 100, 100, parent,
        nullptr, instance, this);
    return window_;
}

// ---------------------------------------------------------------- metrics

void ChannelGrid::make_fonts()
{
    if (italic_) {
        DeleteObject(italic_);
        italic_ = nullptr;
    }
    LOGFONTW logfont = {};
    GetObjectW(ctx_->ui_font(), sizeof(logfont), &logfont);
    logfont.lfItalic = TRUE;
    italic_ = CreateFontIndirectW(&logfont);
}

int ChannelGrid::text_width(HFONT font, const std::string& text) const
{
    return ctx_->text_width(font, W(text));
}

int ChannelGrid::header_preferred(int column) const
{
    // v1: Math.Max(ScaleMetric(34), the heading's preferred width); Program is ScaleMetric(158).
    const app::ChannelColumnInfo& info = app::channel_column_info(column);
    if (column == app::ColProgram) return ctx_->px(158);
    return std::max(ctx_->px(34), text_width(ctx_->ui_font(), info.caption) + ctx_->px(13));
}

void ChannelGrid::measure()
{
    row_height_ = ctx_->px(22);
    header_height_ = ctx_->px(23);
}

void ChannelGrid::apply_header_preferred_widths()
{
    measure();
    const app::ChannelGridModel& model = session_->model();
    for (int c = 0; c < app::kChannelColumnCount; ++c) {
        int need = header_preferred(c);
        // The values fit too (v1's columns cut long counts to "123...").
        for (int r = 0; r < 16; ++r) {
            const app::ChannelCell& cell = model.cell(r, c);
            HFONT font = cell.font == app::CellFont::Bold ? ctx_->ui_bold_font() : cell.font == app::CellFont::Italic ? italic_ : ctx_->ui_font();
            need = std::max(need, text_width(font, cell.text) + ctx_->px(10));
        }
        auto_widths_[c] = need;
        widths_[c] = user_canonical_[c] > 0 ? std::max(ctx_->px(5), static_cast<int>(std::lround(user_canonical_[c] * scale_factor()))) : need;
    }
}

int ChannelGrid::content_width() const
{
    int total = 0;
    for (int c = 0; c < app::kChannelColumnCount; ++c) total += widths_[c];
    return total;
}

int ChannelGrid::content_height() const { return header_height_ + 16 * row_height_; }

int ChannelGrid::client_width() const
{
    RECT rc;
    GetClientRect(window_, &rc);
    return rc.right;
}

int ChannelGrid::client_height() const
{
    RECT rc;
    GetClientRect(window_, &rc);
    return rc.bottom;
}

int ChannelGrid::column_left(int column) const
{
    int x = -x_offset_;
    for (int c = 0; c < column; ++c) x += widths_[c];
    return x;
}

RECT ChannelGrid::cell_rect(int row, int column) const
{
    RECT r;
    r.left = column_left(column);
    r.right = r.left + widths_[column];
    if (row < 0) {
        r.top = 0;
        r.bottom = header_height_;
    }
    else {
        r.top = header_height_ + row * row_height_ - y_offset_;
        r.bottom = r.top + row_height_;
    }
    return r;
}

bool ChannelGrid::hit_test(int x, int y, int& row, int& column) const
{
    row = column = -2;
    if (y < 0 || x < 0) return false;
    int left = -x_offset_;
    for (int c = 0; c < app::kChannelColumnCount; ++c) {
        if (x >= left && x < left + widths_[c]) {
            column = c;
            break;
        }
        left += widths_[c];
    }
    if (column < 0) return false;
    if (y < header_height_) {
        row = -1;
        return true;
    }
    int r = (y - header_height_ + y_offset_) / row_height_;
    if (r < 0 || r >= 16) return false;
    row = r;
    return true;
}

int ChannelGrid::border_hit(int x, int y) const
{
    if (y < 0 || y >= header_height_) return -1;
    int slop = std::max(3, ctx_->px(3));
    int right = -x_offset_;
    for (int c = 0; c < app::kChannelColumnCount; ++c) {
        right += widths_[c];
        if (std::abs(x - right) <= slop) return c;
    }
    return -1;
}

// ---------------------------------------------------------------- scrolling

void ChannelGrid::update_scrollbars()
{
    if (!window_) return;
    RECT rc;
    GetClientRect(window_, &rc);
    LONG style = GetWindowLongW(window_, GWL_STYLE);
    int sbw = scrollbar_width(), sbh = scrollbar_height();
    int cw = rc.right + ((style & WS_VSCROLL) ? sbw : 0);
    int ch = rc.bottom + ((style & WS_HSCROLL) ? sbh : 0);
    int contentW = content_width(), contentH = content_height() + 1;
    bool needH = contentW > cw;
    bool needV = contentH > ch;
    if (needH && !needV) needV = contentH > ch - sbh;
    if (needV && !needH) needH = contentW > cw - sbw;
    int pageW = cw - (needV ? sbw : 0);
    int pageH = ch - (needH ? sbh : 0) - header_height_;
    int bodyH = contentH - header_height_;
    x_offset_ = clamp_int(x_offset_, 0, std::max(0, contentW - pageW));
    y_offset_ = clamp_int(y_offset_, 0, std::max(0, bodyH - pageH));
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, contentW - 1);
    si.nPage = static_cast<UINT>(std::max(1, pageW));
    si.nPos = x_offset_;
    if (((style & WS_HSCROLL) != 0) != needH) ShowScrollBar(window_, SB_HORZ, needH);
    SetScrollInfo(window_, SB_HORZ, &si, TRUE);
    si.nMax = std::max(0, bodyH - 1);
    si.nPage = static_cast<UINT>(std::max(1, pageH));
    si.nPos = y_offset_;
    if (((style & WS_VSCROLL) != 0) != needV) ShowScrollBar(window_, SB_VERT, needV);
    SetScrollInfo(window_, SB_VERT, &si, TRUE);
}

void ChannelGrid::scroll_to(int xOffset, int yOffset)
{
    if (xOffset == x_offset_ && yOffset == y_offset_) return;
    hide_editor(true);
    x_offset_ = xOffset;
    y_offset_ = yOffset;
    update_scrollbars();
    layout_tooltips();
    InvalidateRect(window_, nullptr, FALSE);
}

// ---------------------------------------------------------------- tooltips

void ChannelGrid::layout_tooltips()
{
    if (!tips_) return;
    RECT client;
    GetClientRect(window_, &client);
    for (int index = 0; index < 16 + 16 * app::kChannelColumnCount; ++index) {
        RECT r;
        if (index < app::kChannelColumnCount) r = cell_rect(-1, index);
        else r = cell_rect((index - app::kChannelColumnCount) / app::kChannelColumnCount, (index - app::kChannelColumnCount) % app::kChannelColumnCount);
        if (index >= app::kChannelColumnCount && r.top < header_height_) r.top = header_height_;
        RECT clipped;
        if (!IntersectRect(&clipped, &r, &client)) clipped = RECT{ 0, 0, 0, 0 };
        TOOLINFOW tool = {};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window_;
        tool.uId = static_cast<UINT_PTR>(index);
        tool.rect = clipped;
        SendMessageW(tips_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void ChannelGrid::notify_layout()
{
    HWND parent = GetParent(window_);
    if (parent) PostMessageW(parent, WM_MIDIBN_GRIDLAYOUT, 0, 0);
}

// ---------------------------------------------------------------- painting

void ChannelGrid::paint_cell(HDC dc, int row, int column, const RECT& rect)
{
    const app::ChannelGridModel& model = session_->model();
    if (row < 0) {
        const app::ChannelColumnInfo& info = app::channel_column_info(column);
        HBRUSH white = GetSysColorBrush(COLOR_WINDOW);
        RECT body = rect;
        FillRect(dc, &body, white);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SelectObject(dc, ctx_->ui_font());
        RECT text = rect;
        text.left += ctx_->px(6);
        text.right -= ctx_->px(2);
        text.bottom -= 1;
        DrawTextW(dc, W(info.caption).c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        HPEN sep = CreatePen(PS_SOLID, 1, kHeaderSeparatorColor);
        HGDIOBJ old = SelectObject(dc, sep);
        MoveToEx(dc, rect.right - 1, rect.top, nullptr);
        LineTo(dc, rect.right - 1, rect.bottom - 1);
        SelectObject(dc, old);
        DeleteObject(sep);
        HPEN line = CreatePen(PS_SOLID, 1, kGridLineColor);
        old = SelectObject(dc, line);
        MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
        LineTo(dc, rect.right, rect.bottom - 1);
        SelectObject(dc, old);
        DeleteObject(line);
        return;
    }
    const app::ChannelCell& cell = model.cell(row, column);
    bool enabled = model.row_enabled(row);
    HBRUSH background = GetSysColorBrush(enabled ? COLOR_WINDOW : COLOR_BTNFACE);
    FillRect(dc, &rect, background);
    SetBkMode(dc, TRANSPARENT);
    COLORREF color = cell.color == app::CellColor::Forced ? kForcedColor : cell.color == app::CellColor::Historical ? kHistoricalColor
                                                                                                                       : GetSysColor(COLOR_BTNTEXT);
    SetTextColor(dc, color);
    SelectObject(dc, cell.font == app::CellFont::Bold ? ctx_->ui_bold_font() : cell.font == app::CellFont::Italic ? italic_ : ctx_->ui_font());
    RECT text = rect;
    text.left += ctx_->px(4);
    text.right -= ctx_->px(5);
    text.bottom -= 1;
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | (column == app::ColChannel ? DT_CENTER : DT_RIGHT);
    DrawTextW(dc, W(cell.text).c_str(), -1, &text, flags);
    HPEN line = CreatePen(PS_SOLID, 1, kGridLineColor);
    HGDIOBJ old = SelectObject(dc, line);
    MoveToEx(dc, rect.right - 1, rect.top, nullptr);
    LineTo(dc, rect.right - 1, rect.bottom);
    MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
    LineTo(dc, rect.right, rect.bottom - 1);
    SelectObject(dc, old);
    DeleteObject(line);
    if (focus_cue_visible() && row == current_row_ && column == current_column_) {
        // The keyboard's current cell (DD-031), shown once the keyboard has been used (DD-087).
        RECT focus = rect;
        InflateRect(&focus, -1, -1);
        --focus.right;
        --focus.bottom;
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        DrawFocusRect(dc, &focus);
    }
}

void ChannelGrid::draw_detached(HDC dc)
{
    RECT rc;
    GetClientRect(window_, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kHistoricalColor);
    SelectObject(dc, ctx_->ui_font());
    DrawTextW(dc, W(detached_message_).c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void ChannelGrid::paint(HDC dc, const RECT& clip)
{
    if (detached_) {
        draw_detached(dc);
        return;
    }
    FillRect(dc, &clip, GetSysColorBrush(COLOR_WINDOW));
    RECT client;
    GetClientRect(window_, &client);
    int first = 0, left = -x_offset_;
    while (first < app::kChannelColumnCount && left + widths_[first] <= clip.left) {
        left += widths_[first];
        ++first;
    }
    // body rows
    int firstRow = std::max<int>(0, (std::max<int>(clip.top, header_height_) - header_height_ + y_offset_) / row_height_);
    int lastRow = std::min<int>(15, (clip.bottom - 1 - header_height_ + y_offset_) / row_height_);
    for (int c = first, x = left; c < app::kChannelColumnCount && x < clip.right; x += widths_[c], ++c) {
        for (int r = firstRow; r <= lastRow; ++r) {
            RECT rect = cell_rect(r, c);
            RECT hit;
            if (!IntersectRect(&hit, &rect, &clip)) continue;
            paint_cell(dc, r, c, rect);
        }
    }
    // The heading row paints last so that scrolled rows never show through it.
    if (clip.top < header_height_) {
        for (int c = first, x = left; c < app::kChannelColumnCount && x < clip.right; x += widths_[c], ++c) {
            RECT rect = cell_rect(-1, c);
            paint_cell(dc, -1, c, rect);
        }
    }
}

bool ChannelGrid::render(HDC target, int width, int height)
{
    RECT clip = { 0, 0, width, height };
    FillRect(target, &clip, GetSysColorBrush(COLOR_WINDOW));
    paint(target, clip);
    return true;
}

void ChannelGrid::invalidate_cell(int row, int column)
{
    RECT r = cell_rect(row, column);
    InvalidateRect(window_, &r, FALSE);
}

void ChannelGrid::invalidate_all() { InvalidateRect(window_, nullptr, FALSE); }

void ChannelGrid::apply_changes(const app::ChannelGridChanges& changes)
{
    if (!window_) return;
    // Columns whose changed values no longer fit grow (never shrink while the window lives).
    bool widened = false;
    const app::ChannelGridModel& model = session_->model();
    for (int r = 0; r < 16; ++r) {
        uint16_t bits = changes.rows[static_cast<size_t>(r)];
        if (bits == 0) continue;
        for (int c = 0; c < app::kChannelColumnCount; ++c) {
            if ((bits & (1u << c)) == 0) continue;
            const app::ChannelCell& cell = model.cell(r, c);
            HFONT font = cell.font == app::CellFont::Bold ? ctx_->ui_bold_font() : cell.font == app::CellFont::Italic ? italic_ : ctx_->ui_font();
            int need = text_width(font, cell.text) + ctx_->px(10);
            if (need > auto_widths_[c]) {
                auto_widths_[c] = need;
                if (user_canonical_[c] <= 0 && widths_[c] < need) {
                    widths_[c] = need;
                    widened = true;
                }
            }
        }
    }
    if (widened) {
        hide_editor(true);
        update_scrollbars();
        layout_tooltips();
        InvalidateRect(window_, nullptr, FALSE);
        notify_layout();
        return;
    }
    for (int r = 0; r < 16; ++r) {
        uint16_t bits = changes.rows[static_cast<size_t>(r)];
        if (bits == 0) continue;
        if (bits == 0xFFFF) {
            RECT row = cell_rect(r, 0);
            row.right = column_left(app::kChannelColumnCount - 1) + widths_[app::kChannelColumnCount - 1];
            InvalidateRect(window_, &row, FALSE);
            continue;
        }
        for (int c = 0; c < app::kChannelColumnCount; ++c)
            if (bits & (1u << c)) invalidate_cell(r, c);
    }
}

void ChannelGrid::set_detached(bool detached, const std::string& message)
{
    bool changed = detached != detached_ || message != detached_message_;
    detached_ = detached;
    detached_message_ = message;
    if (detached) hide_editor(false);
    if (tips_) SendMessageW(tips_, TTM_ACTIVATE, detached ? FALSE : TRUE, 0);
    if (changed) InvalidateRect(window_, nullptr, FALSE);
}

void ChannelGrid::on_scale_changed()
{
    hide_editor(false);
    make_fonts();
    apply_header_preferred_widths();
    update_scrollbars();
    layout_tooltips();
    if (tips_) SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, ctx_->px(420));
    InvalidateRect(window_, nullptr, TRUE);
    notify_layout();
}

// ---------------------------------------------------------------- editor

bool ChannelGrid::editor_visible() const { return editor_ && (GetWindowLongW(editor_, GWL_STYLE) & WS_VISIBLE) != 0; }

void ChannelGrid::apply_editor_look()
{
    app::ChannelEditor& editor = session_->editor();
    SendMessageW(editor_, WM_SETFONT, reinterpret_cast<WPARAM>(editor.bold() ? ctx_->ui_bold_font() : ctx_->ui_font()), TRUE);
    if (!editor_typing_) set_window_text_if_changed(editor_, W(editor.flat_text()));
    InvalidateRect(editor_, nullptr, TRUE);
}

void ChannelGrid::show_editor(int row, int column)
{
    RECT cell = cell_rect(row, column);
    int height = std::max(ctx_->font_height() + 3, static_cast<int>(cell.bottom - cell.top) - 4);
    int width = std::max(12, static_cast<int>(cell.right - cell.left) - 4);
    editor_typing_ = false;
    SendMessageW(editor_, EM_SETREADONLY, TRUE, 0);
    apply_editor_look();
    SendMessageW(editor_, EM_SETSEL, 0, 0);
    SetWindowPos(editor_, HWND_TOP, cell.left + 2, cell.top + 2, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(editor_, nullptr, TRUE);
}

void ChannelGrid::hide_editor(bool commitTyped)
{
    if (!editor_) return;
    bool wasOpen = session_->editor().is_open();
    if (wasOpen) {
        std::string typed = editor_typing_ ? U(window_text(editor_)) : std::string();
        // The session may request a value (typing committed), which can change the snapshot.
        session_->hide_editor(commitTyped, typed);
    }
    editor_typing_ = false;
    gesture_cancel();
    if (editor_visible()) ShowWindow(editor_, SW_HIDE);
    if (wasOpen) {
        app::ChannelGridChanges changes = session_->take_changes();
        apply_changes(changes);
    }
}

void ChannelGrid::gesture_cancel()
{
    session_->editor().end_gesture();
    if (gesture_capture_) {
        gesture_capture_ = false;
        if (GetCapture() == window_) ReleaseCapture();
    }
    finish_cursor();
}

void ChannelGrid::finish_cursor()
{
    if (cursor_hidden_) {
        ShowCursor(TRUE);
        cursor_hidden_ = false;
    }
}

void ChannelGrid::start_typing(const std::string& text)
{
    editor_typing_ = true;
    SendMessageW(editor_, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(editor_, W(text).c_str());
    SetFocus(editor_);
    ShowCaret(editor_);
    SendMessageW(editor_, EM_SETSEL, 0, -1);
}

void ChannelGrid::editor_error_tip(const std::string& text)
{
    if (text.empty()) return;
    RECT r;
    GetWindowRect(editor_, &r);
    show_tip_at(W(text), r.left, r.bottom, 2500);
}

void ChannelGrid::editor_commit(bool explicitCommit)
{
    app::ChannelEditor& editor = session_->editor();
    if (!editor.typing()) return;
    std::string text = U(window_text(editor_));
    app::ChannelEditor::Outcome outcome = editor.commit_typed(text, explicitCommit);
    editor_typing_ = false;
    SendMessageW(editor_, EM_SETREADONLY, TRUE, 0);
    HideCaret(editor_);
    SendMessageW(editor_, EM_SETSEL, 0, 0);
    apply_editor_look();
    app::ChannelGridChanges changes = session_->take_changes();
    apply_changes(changes);
    editor_error_tip(outcome.error);
}

void ChannelGrid::gesture_move(int screenX, int screenY)
{
    app::ChannelEditor& editor = session_->editor();
    if (!editor.armed()) return;
    bool leftDown = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool fine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    app::ChannelEditor::PointerResult r = editor.pointer_move(screenX, screenY, fine, leftDown);
    if (r.scrub_started) {
        if (!gesture_capture_) {
            gesture_capture_ = true;
            SetCapture(window_);
        }
        if (!cursor_hidden_) {
            ShowCursor(FALSE);
            cursor_hidden_ = true;
        }
    }
    if (r.warp) SetCursorPos(r.warp_x, r.warp_y);
    if (r.requested) {
        apply_editor_look();
        apply_changes(session_->take_changes());
    }
    if (r.gesture_ended) gesture_cancel();
}

void ChannelGrid::gesture_up()
{
    app::ChannelEditor& editor = session_->editor();
    if (!editor.armed()) {
        gesture_cancel();
        return;
    }
    bool typing = editor.pointer_up();
    gesture_capture_ = false;
    if (GetCapture() == window_) ReleaseCapture();
    finish_cursor();
    if (typing) start_typing(editor.typing_text());
}

LRESULT CALLBACK ChannelGrid::editor_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR self)
{
    ChannelGrid* grid = reinterpret_cast<ChannelGrid*>(self);
    LRESULT result = 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, editor_proc, kEditorSubclass);
    else if (grid->handle_editor_message(window, message, wParam, lParam, result)) return result;
    return DefSubclassProc(window, message, wParam, lParam);
}

bool ChannelGrid::handle_editor_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    app::ChannelEditor& editor = session_->editor();
    result = 0;
    switch (message) {
    case WM_GETDLGCODE:
        result = editor_typing_ ? (DLGC_WANTALLKEYS | DLGC_WANTCHARS) : DLGC_WANTARROWS;
        return true;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, editor_typing_ ? IDC_IBEAM : IDC_SIZEWE));
            result = TRUE;
            return true;
        }
        return false;
    case WM_CONTEXTMENU:
        return true;
    case WM_SETFOCUS: {
        result = DefSubclassProc(window, message, wParam, lParam);
        if (!editor_typing_) HideCaret(window);
        return true;
    }
    case WM_KILLFOCUS:
        if (editor_typing_) editor_commit(false);
        return false;
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
        if (editor_typing_) return false;
        {
            POINT p;
            GetCursorPos(&p);
            editor.pointer_down(p.x, p.y);
            gesture_capture_ = true;
            SetCapture(window_);
        }
        return true;
    case WM_RBUTTONDOWN:
        if (editor_typing_) return false;
        {
            app::ChannelEditor::RightClickResult r = editor.right_click();
            if (r.close) hide_editor(false);
            else editor_error_tip(r.error);
            apply_changes(session_->take_changes());
        }
        return true;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (editor_typing_) return false;
        return true;   // the grid holds the pointer capture while a gesture runs
    case WM_KEYDOWN:
        note_key_press();
        if (!editor_typing_) {
            if (wParam == VK_ESCAPE) {
                editor.abandon_typing();
                return_focus_to_grid();
                return true;
            }
            return false;
        }
        if (wParam == VK_RETURN) {
            editor_commit(true);
            return_focus_to_grid();   // the arrow keys move between cells again (DD-031)
            return true;
        }
        if (wParam == VK_ESCAPE) {
            editor.abandon_typing();
            editor_typing_ = false;
            SendMessageW(editor_, EM_SETREADONLY, TRUE, 0);
            HideCaret(editor_);
            SendMessageW(editor_, EM_SETSEL, 0, 0);
            apply_editor_look();
            return_focus_to_grid();
            return true;
        }
        if (wParam == VK_UP || wParam == VK_DOWN) {
            std::string text = editor.adjust_typed(U(window_text(editor_)), wParam == VK_UP ? 1 : -1);
            SetWindowTextW(editor_, W(text).c_str());
            SendMessageW(editor_, EM_SETSEL, 0, -1);
            return true;
        }
        return false;
    case WM_CHAR:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB) return true;
        return !editor_typing_;
    }
    return false;
}

// ---------------------------------------------------------------- feedback tips

void ChannelGrid::show_tip_at(const std::wstring& text, int screenX, int screenY, int milliseconds)
{
    if (!transient_tip_) {
        transient_tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, window_,
            nullptr, GetModuleHandleW(nullptr), nullptr);
        TOOLINFOW info = {};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        info.hwnd = window_;
        info.uId = kTransientToolId;
        info.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(transient_tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        SendMessageW(transient_tip_, TTM_SETMAXTIPWIDTH, 0, ctx_->px(420));
    }
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.hwnd = window_;
    info.uId = kTransientToolId;
    info.lpszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(transient_tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    SendMessageW(transient_tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(screenX, screenY));
    SendMessageW(transient_tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    SetTimer(window_, kTipTimer, static_cast<UINT>(milliseconds), nullptr);
}

void ChannelGrid::hide_transient_tip()
{
    KillTimer(window_, kTipTimer);
    if (!transient_tip_) return;
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.hwnd = window_;
    info.uId = kTransientToolId;
    SendMessageW(transient_tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
}

void ChannelGrid::show_feedback(const app::ChannelFeedback& feedback)
{
    if (feedback.text.empty()) return;
    if (feedback.kind == app::ChannelFeedback::CellTip) {
        if (feedback.channel < 0 || feedback.channel >= 16 || feedback.column < 0 || feedback.column >= app::kChannelColumnCount) return;
        RECT r = cell_rect(feedback.channel, feedback.column);
        POINT p = { r.left, r.bottom };
        ClientToScreen(window_, &p);
        show_tip_at(W(feedback.text), p.x, p.y, feedback.milliseconds);
    }
    else {
        RECT client;
        GetClientRect(window_, &client);
        POINT p = { 8, std::max<LONG>(0, client.bottom - 26) };
        ClientToScreen(window_, &p);
        show_tip_at(W(feedback.text), p.x, p.y, feedback.milliseconds);
    }
}

// ---------------------------------------------------------------- cell actions and keyboard

void ChannelGrid::cell_action(int row, int column, bool leftButton, int screenX, int screenY, bool keyboard)
{
    bool opened = session_->cell_mouse_down(row, column, leftButton ? app::ChannelSession::Button::Left : app::ChannelSession::Button::Right,
        screenX, screenY, editor_typing_ ? U(window_text(editor_)) : std::string());
    apply_changes(session_->take_changes());
    std::vector<app::ChannelFeedback> feedback = session_->take_feedback();
    if (opened) {
        show_editor(row, column);
        if (keyboard) {
            // The same as a click released without moving: the value can be typed at once.
            if (session_->editor().pointer_up()) start_typing(session_->editor().typing_text());
        }
        else {
            gesture_capture_ = true;
            SetCapture(window_);
        }
    }
    else if (editor_visible() && !session_->editor().is_open()) {
        editor_typing_ = false;
        ShowWindow(editor_, SW_HIDE);
    }
    for (size_t i = 0; i < feedback.size(); ++i) show_feedback(feedback[i]);
    PostMessageW(GetParent(window_), WM_MIDIBN_GRIDACTION, 0, 0);
}

void ChannelGrid::note_key_press()
{
    if (keyboard_used_) return;
    keyboard_used_ = true;
    if (window_ && !detached_) invalidate_cell(current_row_, current_column_);
}

void ChannelGrid::return_focus_to_grid()
{
    if (window_ && editor_ && GetFocus() == editor_) SetFocus(window_);
}

int ChannelGrid::visible_body_rows() const
{
    return std::max(1, (client_height() - header_height_) / std::max(1, row_height_));
}

void ChannelGrid::ensure_visible(int row, int column)
{
    RECT r = cell_rect(row, column);
    int cw = client_width(), ch = client_height();
    int dx = 0, dy = 0;
    if (r.top < header_height_) dy = r.top - header_height_;
    else if (r.bottom > ch) dy = std::min<int>(r.bottom - ch, r.top - header_height_);
    if (r.left < 0) dx = r.left;
    else if (r.right > cw) dx = std::min<int>(r.right - cw, r.left);
    if (dx != 0 || dy != 0) scroll_to(x_offset_ + dx, y_offset_ + dy);
}

void ChannelGrid::mark_current(int row, int column)
{
    row = clamp_int(row, 0, 15);
    column = clamp_int(column, 0, app::kChannelColumnCount - 1);
    if (row == current_row_ && column == current_column_) return;
    if (window_) invalidate_cell(current_row_, current_column_);
    current_row_ = row;
    current_column_ = column;
    if (window_) invalidate_cell(row, column);
}

void ChannelGrid::set_current_cell(int row, int column)
{
    mark_current(row, column);
    if (window_) ensure_visible(current_row_, current_column_);
}

bool ChannelGrid::key_down(UINT key, bool ctrl)
{
    if (detached_ || !window_) return false;
    if (key == VK_ESCAPE) {
        if (session_->editor().is_open() || editor_visible()) {
            hide_editor(false);
            return true;
        }
        return false;   // nothing to cancel: the window does what it always did with Escape
    }
    app::ChannelNavKey nav;
    switch (key) {
    case VK_LEFT: nav = app::ChannelNavKey::Left; break;
    case VK_RIGHT: nav = app::ChannelNavKey::Right; break;
    case VK_UP: nav = app::ChannelNavKey::Up; break;
    case VK_DOWN: nav = app::ChannelNavKey::Down; break;
    case VK_HOME: nav = ctrl ? app::ChannelNavKey::CtrlHome : app::ChannelNavKey::Home; break;
    case VK_END: nav = ctrl ? app::ChannelNavKey::CtrlEnd : app::ChannelNavKey::End; break;
    case VK_PRIOR: nav = app::ChannelNavKey::PageUp; break;
    case VK_NEXT: nav = app::ChannelNavKey::PageDown; break;
    case VK_RETURN:
    case VK_F2:
    case VK_SPACE:
    case VK_DELETE: {
        int row = current_row_, column = current_column_;
        RECT r = cell_rect(row, column);
        POINT screen = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        ClientToScreen(window_, &screen);
        bool isChannel = column == app::ColChannel;
        bool isAttribute = app::channel_column_info(column).is_attribute;
        if (key == VK_SPACE) {
            hide_editor(true);
            cell_action(row, app::ColChannel, true, screen.x, screen.y, true);
        }
        else if (key == VK_DELETE) {
            if (!isAttribute) return true;
            cell_action(row, column, false, screen.x, screen.y, true);
        }
        else if (isChannel || isAttribute) cell_action(row, column, true, screen.x, screen.y, true);
        return true;
    }
    default: return false;
    }
    hide_editor(true);
    app::ChannelCellPosition from;
    from.row = current_row_;
    from.column = current_column_;
    app::ChannelCellPosition to = app::channel_navigate(from, nav, visible_body_rows());
    set_current_cell(to.row, to.column);
    return true;
}

// ---------------------------------------------------------------- window procedure

LRESULT CALLBACK ChannelGrid::window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ChannelGrid* self = reinterpret_cast<ChannelGrid*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ChannelGrid*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    return self->handle(message, wParam, lParam);
}

LRESULT ChannelGrid::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        make_fonts();
        apply_header_preferred_widths();
        editor_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_RIGHT | ES_READONLY | ES_AUTOHSCROLL, 0, 0, 10, 10, window_, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(editor_, &ChannelGrid::editor_proc, kEditorSubclass, reinterpret_cast<DWORD_PTR>(this));
        tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, window_, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, ctx_->px(420));
        SendMessageW(tips_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
        SendMessageW(tips_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        for (int index = 0; index < 16 + 16 * app::kChannelColumnCount; ++index) {
            TOOLINFOW tool = {};
            tool.cbSize = sizeof(tool);
            tool.uFlags = TTF_SUBCLASS;
            tool.hwnd = window_;
            tool.uId = static_cast<UINT_PTR>(index);
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        TOOLINFOW editorTool = {};
        editorTool.cbSize = sizeof(editorTool);
        editorTool.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        editorTool.hwnd = window_;
        editorTool.uId = reinterpret_cast<UINT_PTR>(editor_);
        editorTool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&editorTool));
        return 0;
    }
    case WM_SIZE:
        if (editor_visible()) hide_editor(true);
        update_scrollbars();
        layout_tooltips();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window_, &ps);
        RECT clip = ps.rcPaint;
        int w = clip.right - clip.left, h = clip.bottom - clip.top;
        if (w > 0 && h > 0) {
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ old = SelectObject(memory, bitmap);
            SetWindowOrgEx(memory, clip.left, clip.top, nullptr);
            paint(memory, clip);
            BitBlt(dc, clip.left, clip.top, w, h, memory, clip.left, clip.top, SRCCOPY);
            SelectObject(memory, old);
            DeleteObject(bitmap);
            DeleteDC(memory);
        }
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rc;
        GetClientRect(window_, &rc);
        HDC dc = reinterpret_cast<HDC>(wParam);
        render(dc, rc.right, rc.bottom);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        bool forced = session_->editor().bold();
        SetTextColor(dc, forced ? kForcedColor : GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_NOTIFY: {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == tips_ && (header->code == TTN_GETDISPINFOW)) {
            NMTTDISPINFOW* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            tip_text_.clear();
            UINT_PTR id = header->idFrom;
            if (id == reinterpret_cast<UINT_PTR>(editor_)) {
                tip_text_ = W(session_->editor().tooltip());
            }
            else if (!detached_) {
                if (id < static_cast<UINT_PTR>(app::kChannelColumnCount)) tip_text_ = W(app::channel_column_info(static_cast<int>(id)).tooltip);
                else {
                    int index = static_cast<int>(id) - app::kChannelColumnCount;
                    if (index >= 0 && index < 16 * app::kChannelColumnCount)
                        tip_text_ = W(session_->model().cell(index / app::kChannelColumnCount, index % app::kChannelColumnCount).tooltip);
                }
            }
            info->lpszText = const_cast<wchar_t*>(tip_text_.c_str());
            return 0;
        }
        break;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lParam) != HTCLIENT) break;
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(window_, &p);
        LPCWSTR cursor = IDC_ARROW;
        int row, column;
        if (resizing_ || border_hit(p.x, p.y) >= 0) cursor = IDC_SIZEWE;
        else if (session_->editor().armed()) cursor = IDC_SIZEWE;
        else if (!detached_ && hit_test(p.x, p.y, row, column) && row >= 0) {
            if (column == app::ColChannel) cursor = IDC_HAND;
            else if (app::channel_column_info(column).is_attribute) cursor = IDC_SIZEWE;
        }
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: {
        if (detached_) return 0;
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        bool left = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK;
        // Taking the focus ends a typed edit first (the editor commits when it loses the focus).
        if (GetFocus() != window_) SetFocus(window_);
        int border = left ? border_hit(x, y) : -1;
        if (border >= 0) {
            hide_editor(true);
            resizing_ = true;
            resize_column_ = border;
            resize_start_x_ = x;
            resize_start_width_ = widths_[border];
            SetCapture(window_);
            return 0;
        }
        int row, column;
        if (!hit_test(x, y, row, column) || row < 0) {
            hide_editor(true);
            return 0;
        }
        POINT screen = { x, y };
        ClientToScreen(window_, &screen);
        mark_current(row, column);
        cell_action(row, column, left, screen.x, screen.y, false);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        note_key_press();
        if (key_down(static_cast<UINT>(wParam), (GetKeyState(VK_CONTROL) & 0x8000) != 0)) return 0;
        break;
    case WM_SETFOCUS:
        has_focus_ = true;
        if (!detached_) invalidate_cell(current_row_, current_column_);
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        if (resizing_) {
            int width = std::max(ctx_->px(5), resize_start_width_ + (x - resize_start_x_));
            if (width != widths_[resize_column_]) {
                widths_[resize_column_] = width;
                user_canonical_[resize_column_] = width / scale_factor();
                update_scrollbars();
                layout_tooltips();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        if (session_->editor().armed()) {
            POINT p;
            GetCursorPos(&p);
            gesture_move(p.x, p.y);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (resizing_) {
            resizing_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            notify_layout();
            return 0;
        }
        if (session_->editor().armed()) gesture_up();
        return 0;
    case WM_CAPTURECHANGED:
        if (resizing_ && reinterpret_cast<HWND>(lParam) != window_) resizing_ = false;
        if (gesture_capture_ && reinterpret_cast<HWND>(lParam) != window_) {
            gesture_capture_ = false;
            if (session_->editor().scrubbing()) {
                session_->editor().end_gesture();
                finish_cursor();
            }
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        scroll_to(x_offset_, y_offset_ - delta / WHEEL_DELTA * row_height_ * 3);
        return 0;
    }
    case WM_HSCROLL:
    case WM_VSCROLL: {
        bool horizontal = message == WM_HSCROLL;
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(window_, horizontal ? SB_HORZ : SB_VERT, &si);
        int pos = si.nPos;
        int step = horizontal ? ctx_->px(20) : row_height_;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: pos -= step; break;
        case SB_LINEDOWN: pos += step; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: break;
        }
        if (horizontal) scroll_to(pos, y_offset_);
        else scroll_to(x_offset_, pos);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTipTimer) {
            hide_transient_tip();
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        has_focus_ = false;
        if (!detached_) invalidate_cell(current_row_, current_column_);
        return 0;
    case WM_DESTROY:
        if (tips_) DestroyWindow(tips_);
        if (transient_tip_) DestroyWindow(transient_tip_);
        tips_ = transient_tip_ = nullptr;
        KillTimer(window_, kTipTimer);
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace ui
} // namespace midibn
