#include "controls.h"

#include "midibn/app/ui_mappings.h"
#include "midibn/app/statistics_tips.h"
#include "midibn/format/display_format.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace midibn {
namespace ui {

namespace {

// ------------------------------------------------------------------ helpers

RECT client_rect(HWND window)
{
    RECT rect;
    GetClientRect(window, &rect);
    return rect;
}

COLORREF sys(int index) { return GetSysColor(index); }

void fill(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

class DoubleBuffer {
public:
    DoubleBuffer(HDC target, const RECT& area) : target_(target), area_(area)
    {
        dc_ = CreateCompatibleDC(target);
        bitmap_ = CreateCompatibleBitmap(target, std::max<LONG>(1, area.right - area.left), std::max<LONG>(1, area.bottom - area.top));
        old_ = SelectObject(dc_, bitmap_);
        SetViewportOrgEx(dc_, -area.left, -area.top, nullptr);
    }
    ~DoubleBuffer()
    {
        SetViewportOrgEx(dc_, 0, 0, nullptr);
        BitBlt(target_, area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top, dc_, 0, 0, SRCCOPY);
        SelectObject(dc_, old_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }
    HDC dc() const { return dc_; }

private:
    HDC target_;
    RECT area_;
    HDC dc_;
    HBITMAP bitmap_;
    HGDIOBJ old_;
};

template <class T>
T* self_of(HWND window)
{
    return reinterpret_cast<T*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

int text_extent(HDC dc, const std::wstring& text)
{
    if (text.empty()) return 0;
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

} // namespace

// ------------------------------------------------------------------ label

namespace {

struct LabelData {
    UiContext* ctx = nullptr;
    int style = 0;
    HWND window = nullptr;
};

LRESULT CALLBACK label_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    LabelData* data = self_of<LabelData>(window);
    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        data = new LabelData();
        data->window = window;
        data->ctx = static_cast<UiContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        break;
    }
    case WM_NCDESTROY:
        delete data;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_NCHITTEST: {
        // DD-089: a label with a tooltip must receive the mouse, or the tooltip never appears; where the parent
        // would answer something other than client area (a borderless window's drag zone), the label stays transparent.
        if (data == nullptr || !(data->style & LabelTooltip)) return HTTRANSPARENT;
        HWND parent = GetParent(window);
        LRESULT parentHit = parent ? SendMessageW(parent, WM_NCHITTEST, 0, lParam) : HTCLIENT;
        return parentHit == HTCLIENT ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_SIZE:
        // DD-090: the text is aligned (right, centre) or shortened with an ellipsis to the label's width, so a
        // new width needs a full repaint; without it a resize left the old text where it was (garbage on resize).
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETTEXT: {
        LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        InvalidateRect(window, nullptr, FALSE);
        return result;
    }
    case WM_ENABLE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETFONT:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC target = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(wParam);
        RECT rect = client_rect(window);
        {
            DoubleBuffer buffer(target, message == WM_PAINT ? ps.rcPaint : rect);
            HDC dc = buffer.dc();
            fill(dc, rect, sys(COLOR_BTNFACE));
            int length = GetWindowTextLengthW(window);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(window, &text[0], length + 1);
            text.resize(static_cast<size_t>(length));
            HFONT font = data && data->ctx ? data->ctx->ui_font() : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HGDIOBJ old = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            bool enabled = IsWindowEnabled(window) != FALSE;
            SetTextColor(dc, enabled && !(data->style & LabelDim) ? sys(COLOR_WINDOWTEXT) : (data->ctx ? data->ctx->disabled_text_color() : sys(COLOR_GRAYTEXT)));
            UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
            if (data->style & LabelEllipsis) flags |= DT_END_ELLIPSIS;
            int align = data->style & 3;
            flags |= align == LabelRight ? DT_RIGHT : align == LabelCenter ? DT_CENTER : DT_LEFT;
            size_t lineBreak = text.find_first_of(L"\r\n");
            if (lineBreak != std::wstring::npos) {
                std::wstring first = text.substr(0, lineBreak);
                size_t second = lineBreak;
                while (second < text.size() && (text[second] == L'\r' || text[second] == L'\n')) ++second;
                std::wstring last = text.substr(second);
                int half = (rect.bottom - rect.top) / 2;
                RECT upper = { rect.left, rect.top, rect.right, rect.top + half };
                RECT lower = { rect.left, rect.top + half, rect.right, rect.bottom };
                DrawTextW(dc, first.c_str(), static_cast<int>(first.size()), &upper, flags);
                DrawTextW(dc, last.c_str(), static_cast<int>(last.size()), &lower, flags);
            }
            else DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags);
            SelectObject(dc, old);
        }
        if (message == WM_PAINT) EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

HWND create_label(HWND parent, int id, int style, UiContext* ctx)
{
    HWND window = CreateWindowExW(0, kLabelClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), ctx);
    LabelData* data = self_of<LabelData>(window);
    if (data) data->style = style;
    return window;
}

// ------------------------------------------------------------------ timeline

namespace {

struct TimelineData {
    UiContext* ctx = nullptr;
    HWND window = nullptr;
    int64_t position = 0, duration = 0, drag_position = 0;
    bool dragging = false;
    bool has_focus = false;
    int last_fill = -1;
    RECT last_track = {}, last_time = {};
    size_t last_text_length = 0;
    bool layout_valid = false;
};

std::string time_text(int64_t position, int64_t duration)
{
    return format::format_time(position) + " / " + format::format_time(duration);
}

struct TimelineLayout {
    RECT track;
    RECT time;
    std::wstring text;
};

TimelineLayout timeline_layout(TimelineData* d, int64_t position, HDC dc = nullptr)
{
    TimelineLayout layout;
    layout.text = W(time_text(position, d->duration));
    RECT client = client_rect(d->window);
    int width = client.right, height = client.bottom;
    int measured = 0;
    if (dc) {
        HGDIOBJ old = SelectObject(dc, d->ctx->mono_font());
        measured = text_extent(dc, layout.text);
        SelectObject(dc, old);
    }
    else measured = d->ctx->text_width(d->ctx->mono_font(), layout.text);
    int trackLeft = d->ctx->px_min1(8), rightInset = d->ctx->px_min1(4), separation = d->ctx->px_min1(8),
        minimumTrack = d->ctx->px_min1(20);
    int maximumTimeWidth = std::max(1, width - trackLeft - minimumTrack - separation - rightInset);
    int timeWidth = std::min(measured, maximumTimeWidth);
    int timeLeft = std::max(trackLeft + minimumTrack + separation, width - rightInset - timeWidth);
    layout.time = { timeLeft, 0, timeLeft + timeWidth, height };
    int trackWidth = std::max(minimumTrack, timeLeft - separation - trackLeft);
    int trackHeight = d->ctx->px_min1(8);
    int y = (height - trackHeight) / 2;
    layout.track = { trackLeft, y, trackLeft + trackWidth, y + trackHeight };
    return layout;
}

int64_t timeline_position(TimelineData* d, int x)
{
    TimelineLayout layout = timeline_layout(d, d->dragging ? d->drag_position : d->position);
    return app::timeline_position_from_x(x, layout.track.left, layout.track.right - layout.track.left, d->duration);
}

void timeline_raise(TimelineData* d)
{
    int64_t position = d->position;
    SendMessageW(GetParent(d->window), WM_MIDIBN_SEEK, GetDlgCtrlID(d->window), reinterpret_cast<LPARAM>(&position));
}

LRESULT CALLBACK timeline_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    TimelineData* d = self_of<TimelineData>(window);
    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = new TimelineData();
        d->window = window;
        d->ctx = static_cast<UiContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        break;
    }
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SIZE:
        d->layout_valid = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        d->has_focus = message == WM_SETFOCUS;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ENABLE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (!IsWindowEnabled(window)) return 0;
        SetFocus(window);
        SetCapture(window);
        d->dragging = true;
        d->drag_position = timeline_position(d, GET_X_LPARAM(lParam));
        SendMessageW(GetParent(window), WM_MIDIBN_DRAGGING, 1, 0);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (d->dragging) {
            int64_t position = timeline_position(d, GET_X_LPARAM(lParam));
            if (position != d->drag_position) {
                d->drag_position = position;
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (d->dragging) {
            d->drag_position = timeline_position(d, GET_X_LPARAM(lParam));
            d->position = d->drag_position;
            d->dragging = false;
            ReleaseCapture();
            SendMessageW(GetParent(window), WM_MIDIBN_DRAGGING, 0, 0);
            InvalidateRect(window, nullptr, FALSE);
            timeline_raise(d);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (d->dragging && reinterpret_cast<HWND>(lParam) != window) {
            d->dragging = false;
            SendMessageW(GetParent(window), WM_MIDIBN_DRAGGING, 0, 0);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN: {
        if (!IsWindowEnabled(window)) return 0;
        int64_t position = d->position;
        switch (wParam) {
        case VK_HOME: position = 0; break;
        case VK_END: position = d->duration; break;
        case VK_LEFT: position -= 1000000; break;
        case VK_RIGHT: position += 1000000; break;
        case VK_NEXT: position -= 5000000; break;     // PageDown
        case VK_PRIOR: position += 5000000; break;    // PageUp
        default: return DefWindowProcW(window, message, wParam, lParam);
        }
        d->position = std::max<int64_t>(0, std::min<int64_t>(d->duration, position));
        d->drag_position = d->position;
        InvalidateRect(window, nullptr, FALSE);
        timeline_raise(d);
        return 0;
    }
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC target = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(wParam);
        RECT rect = client_rect(window);
        {
            DoubleBuffer buffer(target, message == WM_PAINT ? ps.rcPaint : rect);
            HDC dc = buffer.dc();
            fill(dc, rect, sys(COLOR_BTNFACE));
            bool enabled = IsWindowEnabled(window) != FALSE;
            int64_t shown = d->dragging ? d->drag_position : d->position;
            TimelineLayout layout = timeline_layout(d, shown, dc);
            d->last_track = layout.track;
            d->last_time = layout.time;
            d->last_text_length = layout.text.size();
            d->layout_valid = true;
            double fraction = d->duration <= 0 ? 0.0
                : std::max(0.0, std::min(1.0, static_cast<double>(shown) / static_cast<double>(d->duration)));
            int trackWidth = layout.track.right - layout.track.left;
            int fillWidth = static_cast<int>(std::nearbyint(trackWidth * fraction));
            d->last_fill = fillWidth;
            COLORREF backgroundColor = enabled ? RGB(205, 209, 214) : RGB(225, 225, 225);
            COLORREF fillColor = enabled ? RGB(52, 120, 214) : RGB(160, 170, 180);
            fill(dc, layout.track, backgroundColor);
            if (fillWidth > 0) {
                RECT filled = { layout.track.left, layout.track.top, layout.track.left + fillWidth, layout.track.bottom };
                fill(dc, filled, fillColor);
            }
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(125, 130, 138));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, layout.track.left, layout.track.top, layout.track.right + 1, layout.track.bottom + 1);
            int thumbWidth = d->ctx->px_min1(12), thumbHeight = d->ctx->px_min1(16);
            int thumbX = layout.track.left + fillWidth;
            HBRUSH fillBrush = CreateSolidBrush(fillColor);
            HPEN fillPen = CreatePen(PS_SOLID, 1, fillColor);
            SelectObject(dc, fillBrush);
            SelectObject(dc, fillPen);
            int thumbTop = layout.track.top + ((layout.track.bottom - layout.track.top) - thumbHeight) / 2;
            Ellipse(dc, thumbX - thumbWidth / 2, thumbTop, thumbX - thumbWidth / 2 + thumbWidth, thumbTop + thumbHeight);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            DeleteObject(fillPen);
            DeleteObject(fillBrush);
            HGDIOBJ oldFont = SelectObject(dc, d->ctx->mono_font());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? sys(COLOR_WINDOWTEXT) : d->ctx->disabled_text_color());
            RECT timeRect = layout.time;
            DrawTextW(dc, layout.text.c_str(), static_cast<int>(layout.text.size()), &timeRect,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            if (d->has_focus && (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
                RECT focus = { 1, 1, rect.right - 2, rect.bottom - 2 };
                DrawFocusRect(dc, &focus);
            }
        }
        if (message == WM_PAINT) EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

HWND create_timeline(HWND parent, int id, UiContext* ctx)
{
    return CreateWindowExW(0, kTimelineClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), ctx);
}

void TimelineApi::set_timeline(HWND window, int64_t position, int64_t duration)
{
    TimelineData* d = self_of<TimelineData>(window);
    if (!d) return;
    duration = std::max<int64_t>(0, duration);
    position = std::max<int64_t>(0, std::min(duration, position));
    if (d->position == position && d->duration == duration) return;
    bool positionOnly = d->duration == duration && !d->dragging && duration > 0 && d->layout_valid;
    d->position = position;
    d->duration = duration;
    if (!d->dragging) d->drag_position = position;
    if (positionOnly && time_text(position, duration).size() != d->last_text_length) positionOnly = false;
    if (!positionOnly) {
        d->layout_valid = false;
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    // Repaint only the clock text and the strip of the track the thumb crossed (digits are
    // fixed-width in the clock's monospaced font, so the layout of the last paint still holds).
    RECT clientRect = client_rect(window);
    InvalidateRect(window, &d->last_time, FALSE);
    int trackWidth = d->last_track.right - d->last_track.left;
    int newFill = static_cast<int>(std::nearbyint(trackWidth * std::max(0.0, std::min(1.0, static_cast<double>(position) / static_cast<double>(d->duration)))));
    int oldFill = d->last_fill >= 0 ? d->last_fill : newFill;
    int thumb = d->ctx->px_min1(16);
    RECT band = { d->last_track.left + std::min(oldFill, newFill) - thumb, 0, d->last_track.left + std::max(oldFill, newFill) + thumb,
        clientRect.bottom };
    InvalidateRect(window, &band, FALSE);
}

bool TimelineApi::dragging(HWND window)
{
    TimelineData* d = self_of<TimelineData>(window);
    return d && d->dragging;
}

void TimelineApi::scale_changed(HWND window) { InvalidateRect(window, nullptr, FALSE); }

// ------------------------------------------------------------------ statistics grid

namespace {

const wchar_t* const kCaptions[8] = {
    L"Timeline / output:", L"Queue now / maximum:", L"Maximum rate:", L"Output rate:",
    L"Events sent / dropped:", L"Effective speed:", L"Maximum lag:", L"Current lag:"
};
const wchar_t* const kCompactCaptions[8][3] = {
    { L"Timeline/output:", L"Timeline:", L"Time:" },
    { L"Queue now/max:", L"Queue:", nullptr },
    { L"Max rate:", L"Rate:", nullptr },
    { L"Output rate:", L"Output:", nullptr },
    { L"Sent/dropped:", L"Events:", nullptr },
    { L"Effective speed:", L"Speed:", nullptr },
    { L"Max lag:", nullptr, nullptr },
    { L"Current lag:", nullptr, nullptr }
};
const wchar_t kCompactSlash[] = L"\x2009/\x2009";

bool g_paint_all_cells = false;

struct StatisticsData {
    UiContext* ctx = nullptr;
    HWND window = nullptr;
    HWND tip = nullptr;
    bool compact = false;
    app::StatisticsText text;
    bool captionTruncated[8] = {};
    bool valueTruncated[8] = {};
    std::wstring tipText;
    int lastLayoutWidth = -1, lastLayoutHeight = -1, lastLayoutQueue = -1;
};

int stats_content_height(StatisticsData* d)
{
    RECT client = client_rect(d->window);
    return std::max(1, static_cast<int>(client.bottom) - (d->text.queue_limited ? d->ctx->px(18) : 0));
}

int cell_at(StatisticsData* d, POINT point)
{
    RECT client = client_rect(d->window);
    int contentHeight = client.bottom - (d->text.queue_limited ? d->ctx->px(18) : 0);
    if (point.y < 0 || point.y >= contentHeight || point.x < 0 || point.x >= client.right) return -1;
    int column = point.x < client.right / 2 ? 0 : 1;
    int row = std::min(3, static_cast<int>(point.y) * 4 / std::max(1, contentHeight));
    return row * 2 + column;
}

int measure_value(StatisticsData* d, HDC dc, const std::wstring& text)
{
    if (!d->compact || text.find(kCompactSlash) == std::wstring::npos) return text_extent(dc, text);
    int width = 0;
    size_t start = 0;
    size_t slash = text.find(kCompactSlash, start);
    while (slash != std::wstring::npos) {
        if (slash > start) width += text_extent(dc, text.substr(start, slash - start));
        width += text_extent(dc, L"/") + 4;
        start = slash + 3;
        slash = text.find(kCompactSlash, start);
    }
    if (start < text.size()) width += text_extent(dc, text.substr(start));
    return width;
}

void draw_value(StatisticsData* d, HDC dc, const std::wstring& text, RECT bounds, int measured)
{
    UINT flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    if (!d->compact || text.find(kCompactSlash) == std::wstring::npos || measured > bounds.right - bounds.left) {
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds, flags | DT_RIGHT);
        return;
    }
    int x = bounds.right - measured;
    size_t start = 0;
    size_t slash = text.find(kCompactSlash, start);
    while (slash != std::wstring::npos) {
        if (slash > start) {
            std::wstring part = text.substr(start, slash - start);
            int width = text_extent(dc, part);
            RECT r = { x, bounds.top, x + width, bounds.bottom };
            DrawTextW(dc, part.c_str(), static_cast<int>(part.size()), &r, flags | DT_LEFT);
            x += width;
        }
        x += 2;
        int slashWidth = text_extent(dc, L"/");
        RECT r = { x, bounds.top, x + slashWidth, bounds.bottom };
        DrawTextW(dc, L"/", 1, &r, flags | DT_LEFT);
        x += slashWidth + 2;
        start = slash + 3;
        slash = text.find(kCompactSlash, start);
    }
    if (start < text.size()) {
        std::wstring rest = text.substr(start);
        RECT r = { x, bounds.top, bounds.right, bounds.bottom };
        DrawTextW(dc, rest.c_str(), static_cast<int>(rest.size()), &r, flags | DT_LEFT);
    }
}

void draw_cell(StatisticsData* d, HDC dc, int left, int index, int row, int width, int rowHeight)
{
    int gap = d->ctx->px_min1(d->compact ? 2 : 6);
    std::wstring value = W(d->text.values[index]);
    HGDIOBJ oldFont = SelectObject(dc, d->ctx->mono_bold_font());
    int measuredValue = measure_value(d, dc, value);
    int innerWidth = std::max(1, width - 4);
    int valueWidth = std::min(measuredValue, innerWidth);
    int captionAllowance = std::max(0, innerWidth - valueWidth);
    SelectObject(dc, d->ctx->caption_font());
    std::wstring choices[5];
    int choiceCount = 0;
    if (!d->compact) choices[choiceCount++] = kCaptions[index];
    for (int i = 0; i < 3 && kCompactCaptions[index][i] != nullptr; ++i) choices[choiceCount++] = kCompactCaptions[index][i];
    std::wstring captionText;
    int captionWidth = 0;
    for (int i = 0; i < choiceCount; ++i) {
        int measured = text_extent(dc, choices[i]);
        if (measured + gap <= captionAllowance) {
            captionText = choices[i];
            captionWidth = measured;
            break;
        }
    }
    int realizedGap = captionText.empty() ? 0 : gap;
    RECT caption = { left + 2, row * rowHeight, left + 2 + captionWidth, row * rowHeight + rowHeight };
    RECT valueRect = { left + 2 + innerWidth - valueWidth, row * rowHeight, left + 2 + innerWidth, row * rowHeight + rowHeight };
    d->captionTruncated[index] = captionText != kCaptions[index];
    d->valueTruncated[index] = measuredValue > valueRect.right - valueRect.left;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, sys(COLOR_WINDOWTEXT));
    if (!captionText.empty()) {
        RECT r = caption;
        r.right = std::min<LONG>(caption.right, std::max<LONG>(caption.left, valueRect.left - realizedGap));
        DrawTextW(dc, captionText.c_str(), static_cast<int>(captionText.size()), &r,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, d->ctx->mono_bold_font());
    draw_value(d, dc, value, valueRect, measuredValue);
    SelectObject(dc, oldFont);
}

COLORREF pressure_color(double ratio, bool overflowPulse)
{
    if (ratio > 1.0) return RGB(155, 75, 190);
    if (overflowPulse || ratio >= 0.9) return RGB(210, 70, 70);
    return ratio >= 0.65 ? RGB(215, 155, 35) : RGB(65, 165, 90);
}

std::wstring compact_pressure_text(StatisticsData* d, bool compact)
{
    return W(format::format_queue_pressure_text(d->text.virtual_queue, d->text.age_pressure, d->text.occupied,
        d->text.limit, compact));
}

void draw_queue_pressure(StatisticsData* d, HDC dc, const RECT& client)
{
    double ratio = d->text.limit <= 0 ? 0.0 : static_cast<double>(d->text.occupied) / static_cast<double>(std::max<int64_t>(1, d->text.limit));
    COLORREF color = pressure_color(ratio, d->text.overflow_pulse);
    std::wstring text = compact_pressure_text(d, false);
    HGDIOBJ oldFont = SelectObject(dc, d->ctx->mono_bold_font());
    int available = std::max(1, static_cast<int>(client.right) - 6);
    int gap = d->ctx->px_min1(7), minimumBar = d->ctx->px_min1(24);
    int measured = text_extent(dc, text);
    if (measured + minimumBar + gap > available) {
        std::wstring compact = compact_pressure_text(d, true);
        int compactMeasured = text_extent(dc, compact);
        if (compactMeasured < measured) { text = compact; measured = compactMeasured; }
    }
    int textWidth = std::min(measured, std::max(1, available - minimumBar - gap));
    int barWidth = std::max(minimumBar, available - textWidth - gap);
    int barHeight = d->ctx->px_min1(9);
    RECT bar = { 3, client.bottom - d->ctx->px_min1(14), 3 + barWidth, client.bottom - d->ctx->px_min1(14) + barHeight };
    fill(dc, bar, RGB(225, 228, 232));
    RECT filled = { bar.left, bar.top, bar.left + static_cast<int>(std::nearbyint((bar.right - bar.left) * std::min(1.0, ratio))), bar.bottom };
    fill(dc, filled, color);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(155, 160, 166));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bar.left, bar.top, bar.right + 1, bar.bottom + 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RECT textBounds = { bar.right + gap, bar.top - d->ctx->px_min1(4), client.right - 3, bar.top - d->ctx->px_min1(4) + d->ctx->px_min1(18) };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textBounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void stats_update_tools(StatisticsData* d)
{
    if (!d->tip) return;
    RECT client = client_rect(d->window);
    int contentHeight = std::max(1, static_cast<int>(client.bottom) - (d->text.queue_limited ? d->ctx->px(18) : 0));
    if (client.right == d->lastLayoutWidth && client.bottom == d->lastLayoutHeight &&
        static_cast<int>(d->text.queue_limited) == d->lastLayoutQueue) return;
    d->lastLayoutWidth = client.right;
    d->lastLayoutHeight = client.bottom;
    d->lastLayoutQueue = d->text.queue_limited ? 1 : 0;
    for (int i = 0; i < 9; ++i) {
        TOOLINFOW info = {};
        info.cbSize = sizeof(info);
        info.hwnd = d->window;
        info.uId = static_cast<UINT_PTR>(i);
        RECT r;
        if (i < 8) {
            int column = i % 2, row = i / 2;
            int rowTop = row * contentHeight / 4, rowBottom = (row + 1) * contentHeight / 4;
            r = { column * (client.right / 2), rowTop, column == 0 ? client.right / 2 : client.right, rowBottom };
        }
        else r = { 0, contentHeight, client.right, client.bottom };
        info.rect = r;
        SendMessageW(d->tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info));
    }
}

std::wstring stats_tip_for(StatisticsData* d, int cell)
{
    const app::StatisticsText& t = d->text;
    std::string text;
    if (cell == 8) {
        std::string explanation = t.virtual_queue
            ? "Virtual pressure uses the selected rate model to decide whether a newly arriving MIDI event fits. Accepted MIDI is sent immediately. This is separate from the actual unsent scheduler/output backlog shown above, and it does not predict delay inside a driver or synthesizer."
            : t.age_pressure
                ? "Waiting-time pressure is the age of the oldest pending event divided by the configured microsecond limit. The event currently in service is not removable. Drop oldest complete note is a soft limit when protected traffic remains at the head."
                : "Finite-buffer occupancy includes the event currently in service. Safety-preserving overflow policies may temporarily exceed the configured soft limit so a required Note Off or non-note message is not discarded.";
        return W(format::format_queue_pressure_text(t.virtual_queue, t.age_pressure, t.occupied, t.limit, false)) +
            L"\r\n" + W(explanation) + L"\r\nPurple means protected pending work has exceeded the configured soft limit.";
    }
    if (cell != 8) text = app::statistics_cell_tooltip(t, cell);
    std::wstring result = W(text);
    if (cell >= 0 && cell < 8 && (d->captionTruncated[cell] || d->valueTruncated[cell])) {
        std::wstring full = std::wstring(kCaptions[cell]) + L" " + W(t.values[cell]);
        result = result.empty() ? full : full + L"\r\n" + result;
    }
    return result;
}

LRESULT CALLBACK stats_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    StatisticsData* d = self_of<StatisticsData>(window);
    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = new StatisticsData();
        d->window = window;
        d->ctx = static_cast<UiContext*>(create->lpCreateParams);
        for (int i = 0; i < 8; ++i) d->text.values[i] = "\xE2\x80\x94";
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        break;
    }
    case WM_CREATE: {
        d->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
            window, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(d->tip, TTM_SETMAXTIPWIDTH, 0, 420);
        SendMessageW(d->tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        for (int i = 0; i < 9; ++i) {
            TOOLINFOW info = {};
            info.cbSize = sizeof(info);
            info.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
            info.hwnd = window;
            info.uId = static_cast<UINT_PTR>(i);
            info.lpszText = LPSTR_TEXTCALLBACKW;
            info.rect = { 0, 0, 1, 1 };
            SendMessageW(d->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
        return 0;
    }
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_SIZE:
        stats_update_tools(d);
        // DD-090: the columns split the width in half and the values are right-aligned, so every cell moves when
        // the width changes: repaint everything (a resize used to keep the old cells beside the new ones).
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_NOTIFY: {
        NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            d->tipText = stats_tip_for(d, static_cast<int>(header->idFrom));
            info->lpszText = const_cast<wchar_t*>(d->tipText.c_str());
            return 0;
        }
        break;
    }
    case WM_RBUTTONUP: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (cell_at(d, point) == 5) {
            ClientToScreen(window, &point);
            SendMessageW(GetParent(window), WM_MIDIBN_SPEEDMENU, 0, MAKELPARAM(point.x, point.y));
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC target = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(wParam);
        RECT rect = client_rect(window);
        {
            DoubleBuffer buffer(target, message == WM_PAINT ? ps.rcPaint : rect);
            HDC dc = buffer.dc();
            fill(dc, rect, sys(COLOR_BTNFACE));
            int columnWidth = std::max(1, static_cast<int>(rect.right) / 2);
            int contentHeight = stats_content_height(d);
            int captionH = d->ctx->font_height_of(d->ctx->caption_font());
            int valueH = d->ctx->font_height_of(d->ctx->mono_bold_font());
            int rowHeight = std::max(std::max(captionH, valueH), contentHeight / 4);
            for (int index = 0; index < 8; ++index) {
                // Only the cells that need painting (live updates invalidate just the cells whose value changed).
                if (message == WM_PAINT && !g_paint_all_cells) {
                    RECT cell = { (index % 2) * columnWidth, (index / 2) * rowHeight, (index % 2) * columnWidth + columnWidth,
                        (index / 2) * rowHeight + rowHeight }, hit;
                    if (!IntersectRect(&hit, &cell, &ps.rcPaint)) continue;
                }
                draw_cell(d, dc, (index % 2) * columnWidth, index, index / 2, columnWidth, rowHeight);
            }
            if (d->text.queue_limited) draw_queue_pressure(d, dc, rect);
        }
        if (message == WM_PAINT) EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

HWND create_statistics(HWND parent, int id, UiContext* ctx)
{
    return CreateWindowExW(0, kStatsClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), ctx);
}

void StatisticsApi::set_values(HWND window, const app::StatisticsText& newText, unsigned cellMask)
{
    StatisticsData* d = self_of<StatisticsData>(window);
    if (!d) return;
    app::StatisticsText text = newText;
    if ((cellMask & 0xFFu) != 0xFFu)
        for (int i = 0; i < 8; ++i)
            if (!(cellMask & (1u << i))) text.values[i] = d->text.values[i];
    if (d->text == text) return;
    bool onlyValues = d->text.queue_limited == text.queue_limited && d->text.virtual_queue == text.virtual_queue &&
        d->text.age_pressure == text.age_pressure && d->text.occupied == text.occupied && d->text.limit == text.limit &&
        d->text.overflow_pulse == text.overflow_pulse && d->text.per_note_gate == text.per_note_gate;
    app::StatisticsText previous = d->text;
    d->text = text;
    stats_update_tools(d);
    if (!onlyValues) {
        InvalidateRect(window, nullptr, FALSE);
        return;
    }
    // Only the cells whose text changed are repainted.
    RECT client = client_rect(window);
    int contentHeight = stats_content_height(d);
    for (int i = 0; i < 8; ++i) {
        if (previous.values[i] == text.values[i]) continue;
        int column = i % 2, row = i / 2;
        RECT cell = { column * (client.right / 2), row * contentHeight / 4, column == 0 ? client.right / 2 : client.right,
            (row + 1) * contentHeight / 4 };
        InvalidateRect(window, &cell, FALSE);
    }
}

void StatisticsApi::set_paint_all(bool all) { g_paint_all_cells = all; }

void StatisticsApi::set_compact(HWND window, bool compact)
{
    StatisticsData* d = self_of<StatisticsData>(window);
    if (!d || d->compact == compact) return;
    d->compact = compact;
    InvalidateRect(window, nullptr, FALSE);
}

int StatisticsApi::preferred_height(HWND window)
{
    StatisticsData* d = self_of<StatisticsData>(window);
    if (!d) return 0;
    int line = std::max(d->ctx->font_height_of(d->ctx->caption_font()), d->ctx->font_height_of(d->ctx->mono_bold_font()));
    return std::max(d->ctx->px_min1(d->compact ? 84 : 104), 4 * (line + 1) + d->ctx->px_min1(18));
}

void StatisticsApi::scale_changed(HWND window)
{
    StatisticsData* d = self_of<StatisticsData>(window);
    if (d) { d->lastLayoutWidth = -1; InvalidateRect(window, nullptr, FALSE); }
}

// ------------------------------------------------------------------ caption button

namespace {

struct CaptionData {
    UiContext* ctx = nullptr;
    CaptionGlyph glyph = GlyphMenu;
    bool hover = false, pressed = false, tracking = false, focused = false;
    COLORREF background = 0;
    bool close_style = false;
};

LRESULT CALLBACK caption_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    CaptionData* d = self_of<CaptionData>(window);
    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = new CaptionData();
        d->ctx = static_cast<UiContext*>(create->lpCreateParams);
        d->background = GetSysColor(COLOR_BTNFACE);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        break;
    }
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_USER + 1:   // set background colour
        d->background = static_cast<COLORREF>(wParam);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_MOUSEMOVE:
        if (!d->tracking) {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            d->tracking = true;
        }
        if (!d->hover) { d->hover = true; InvalidateRect(window, nullptr, FALSE); }
        return 0;
    case WM_MOUSELEAVE:
        d->tracking = false;
        d->hover = false;
        d->pressed = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        SetCapture(window);
        d->pressed = true;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (d->pressed) {
            d->pressed = false;
            ReleaseCapture();
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT r = client_rect(window);
            InvalidateRect(window, nullptr, FALSE);
            if (PtInRect(&r, p))
                SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), BN_CLICKED), reinterpret_cast<LPARAM>(window));
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), BN_CLICKED), reinterpret_cast<LPARAM>(window));
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        d->focused = message == WM_SETFOCUS;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETOBJECT:
        break;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC target = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(wParam);
        RECT rect = client_rect(window);
        {
            DoubleBuffer buffer(target, message == WM_PAINT ? ps.rcPaint : rect);
            HDC dc = buffer.dc();
            bool closeHot = d->glyph == GlyphClose && (d->hover || d->pressed);
            COLORREF bg = d->background;
            if (closeHot) bg = d->pressed ? RGB(241, 112, 122) : RGB(232, 17, 35);
            else if (d->pressed) bg = RGB(std::max(0, GetRValue(bg) - 40), std::max(0, GetGValue(bg) - 40), std::max(0, GetBValue(bg) - 40));
            else if (d->hover) bg = RGB(std::max(0, GetRValue(bg) - 22), std::max(0, GetGValue(bg) - 22), std::max(0, GetBValue(bg) - 22));
            fill(dc, rect, bg);
            COLORREF ink = closeHot ? RGB(255, 255, 255) : sys(COLOR_WINDOWTEXT);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, d->ctx->px(1)), ink);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            int cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
            // Half extent. v1 draws these glyphs as text ("≡", "□", "×") in the window's UI font, so they are about
            // as large as a capital letter; follow the font height (clamped to the button) rather than the button size.
            int box = static_cast<int>(std::min(rect.right - rect.left, rect.bottom - rect.top));
            int s = std::max(3, std::min(box * 3 / 10, (d->ctx->font_height() * 3 + 5) / 10));
            switch (d->glyph) {
            case GlyphMenu:
                for (int i = -1; i <= 1; ++i) { MoveToEx(dc, cx - s, cy + i * s * 2 / 3, nullptr); LineTo(dc, cx + s + 1, cy + i * s * 2 / 3); }
                break;
            case GlyphMinimize:
                MoveToEx(dc, cx - s, cy + s / 2, nullptr);
                LineTo(dc, cx + s + 1, cy + s / 2);
                break;
            case GlyphMaximize:
                Rectangle(dc, cx - s, cy - s, cx + s + 1, cy + s + 1);
                break;
            case GlyphRestore: {
                int o = std::max(2, s / 3);
                Rectangle(dc, cx - s, cy - s + o, cx + s - o + 1, cy + s + 1);
                MoveToEx(dc, cx - s + o, cy - s + o, nullptr);
                LineTo(dc, cx - s + o, cy - s);
                LineTo(dc, cx + s + 1, cy - s);
                LineTo(dc, cx + s + 1, cy + s - o + 1);
                LineTo(dc, cx + s - o + 1, cy + s - o + 1);
                break;
            }
            case GlyphClose:
                MoveToEx(dc, cx - s, cy - s, nullptr);
                LineTo(dc, cx + s + 1, cy + s + 1);
                MoveToEx(dc, cx - s, cy + s, nullptr);
                LineTo(dc, cx + s + 1, cy - s - 1);
                break;
            }
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            if (d->focused && (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
                RECT focus = { rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2 };
                DrawFocusRect(dc, &focus);
            }
        }
        if (message == WM_PAINT) EndPaint(window, &ps);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

HWND create_caption_button(HWND parent, int id, CaptionGlyph glyph, UiContext* ctx, const wchar_t* accessibleName)
{
    HWND window = CreateWindowExW(0, kCaptionButtonClass, accessibleName, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), ctx);
    CaptionData* d = self_of<CaptionData>(window);
    if (d) d->glyph = glyph;
    return window;
}

void set_caption_glyph(HWND button, CaptionGlyph glyph)
{
    CaptionData* d = self_of<CaptionData>(button);
    if (d && d->glyph != glyph) { d->glyph = glyph; InvalidateRect(button, nullptr, FALSE); }
}

// ------------------------------------------------------------------ tooltips

TooltipHost::~TooltipHost()
{
    if (tip_ && IsWindow(tip_)) DestroyWindow(tip_);
}

void TooltipHost::create(HWND owner)
{
    owner_ = owner;
    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 420);
    SendMessageW(tip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
}

void TooltipHost::set_maximum_width(int pixels) { SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, pixels); }

void TooltipHost::add_control(HWND control)
{
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = owner_;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void TooltipHost::remove_control(HWND control)
{
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.hwnd = owner_;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    SendMessageW(tip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

bool TooltipHost::handle_notify(NMHDR* header, const std::function<std::wstring(HWND)>& provider)
{
    if (header->code != TTN_GETDISPINFOW || header->hwndFrom != tip_) return false;
    NMTTDISPINFOW* info = reinterpret_cast<NMTTDISPINFOW*>(header);
    HWND control = (info->uFlags & TTF_IDISHWND) ? reinterpret_cast<HWND>(header->idFrom) : nullptr;
    text_ = control ? provider(control) : std::wstring();
    info->lpszText = const_cast<wchar_t*>(text_.c_str());
    return true;
}

namespace {

HWND g_transient_tip = nullptr;
const UINT_PTR kTransientTimer = 0x7410;

void CALLBACK transient_tip_timer(HWND, UINT, UINT_PTR id, DWORD)
{
    KillTimer(nullptr, id);
    if (g_transient_tip) {
        DestroyWindow(g_transient_tip);
        g_transient_tip = nullptr;
    }
}

} // namespace

void show_transient_tip(HWND control, const std::wstring& text, int milliseconds)
{
    if (g_transient_tip) { DestroyWindow(g_transient_tip); g_transient_tip = nullptr; }
    g_transient_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
        GetAncestor(control, GA_ROOT), nullptr, GetModuleHandleW(nullptr), nullptr);
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    info.hwnd = control;
    info.uId = 1;
    info.lpszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(g_transient_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    RECT rect;
    GetWindowRect(control, &rect);
    SendMessageW(g_transient_tip, TTM_TRACKPOSITION, 0, MAKELPARAM(rect.left, rect.bottom));
    SendMessageW(g_transient_tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    SetTimer(nullptr, kTransientTimer, static_cast<UINT>(milliseconds), transient_tip_timer);
}

// ------------------------------------------------------------------ registration

void register_control_classes(HINSTANCE instance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // DD-090: every one of these controls draws from its full size (aligned text, half-width columns, a centred
    // glyph), so a size change invalidates the whole control (CS_HREDRAW | CS_VREDRAW), as the standard controls do.
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = label_proc;
    wc.lpszClassName = kLabelClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = timeline_proc;
    wc.lpszClassName = kTimelineClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = stats_proc;
    wc.lpszClassName = kStatsClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = caption_proc;
    wc.lpszClassName = kCaptionButtonClass;
    RegisterClassExW(&wc);
}

// ------------------------------------------------------------------ check boxes and radio buttons (P10-UI1)

namespace {

// uxtheme.dll is resolved at run time: Windows XP has it, but a visual style may be switched off, and the program
// must also start where the library or the functions are missing (classic look then).
struct ThemeApi {
    typedef HANDLE (WINAPI* OpenFn)(HWND, LPCWSTR);
    typedef HRESULT (WINAPI* CloseFn)(HANDLE);
    typedef HRESULT (WINAPI* DrawFn)(HANDLE, HDC, int, int, const RECT*, const RECT*);
    typedef HRESULT (WINAPI* SizeFn)(HANDLE, HDC, int, int, const RECT*, int, SIZE*);
    OpenFn open = nullptr;
    CloseFn close = nullptr;
    DrawFn draw = nullptr;
    SizeFn size = nullptr;
    ThemeApi()
    {
        HMODULE module = LoadLibraryW(L"uxtheme.dll");
        if (module == nullptr) return;
        open = reinterpret_cast<OpenFn>(GetProcAddress(module, "OpenThemeData"));
        close = reinterpret_cast<CloseFn>(GetProcAddress(module, "CloseThemeData"));
        draw = reinterpret_cast<DrawFn>(GetProcAddress(module, "DrawThemeBackground"));
        size = reinterpret_cast<SizeFn>(GetProcAddress(module, "GetThemePartSize"));
        if (!open || !close || !draw || !size) open = nullptr;
    }
    bool available() const { return open != nullptr; }
};

const ThemeApi& theme_api()
{
    static ThemeApi api;
    return api;
}

const wchar_t kScaledButtonProp[] = L"MidiBnScaledButton";
const int kThemeCheckboxPart = 3, kThemeRadioPart = 2;   // BP_CHECKBOX, BP_RADIOBUTTON
const int kThemeNativeBoxPixels = 13;                    // the canonical box: the classic and the Windows 7-10 size at 96 dpi

struct ScaledButtonData {
    UiContext* ctx = nullptr;
    WNDPROC original = nullptr;
    HANDLE theme = nullptr;
    bool theme_tried = false;
    bool hot = false;
};

void close_button_theme(ScaledButtonData* data)
{
    if (data->theme != nullptr) theme_api().close(data->theme);
    data->theme = nullptr;
    data->theme_tried = false;
}

HANDLE button_theme(HWND window, ScaledButtonData* data)
{
    if (!data->theme_tried) {
        data->theme_tried = true;
        if (theme_api().available()) data->theme = theme_api().open(window, L"BUTTON");
    }
    return data->theme;
}

// Draws the box (or circle) of a check box / radio button into `box` of `dc`: the theme's picture at its own size,
// stretched to the scaled size when that differs, or the classic frame control (which scales its own glyph).
void draw_scaled_glyph(HWND window, ScaledButtonData* data, HDC dc, const RECT& box, bool radio, bool checked, bool enabled, bool pressed, bool hot)
{
    int width = box.right - box.left, height = box.bottom - box.top;
    HANDLE theme = button_theme(window, data);
    if (theme != nullptr) {
        // CBS_/RBS_ states: unchecked normal, hot, pressed, disabled = 1..4; checked = 5..8.
        int state = (enabled ? (pressed ? 3 : hot ? 2 : 1) : 4) + (checked ? 4 : 0);
        int part = radio ? kThemeRadioPart : kThemeCheckboxPart;
        SIZE native = { kThemeNativeBoxPixels, kThemeNativeBoxPixels };
        theme_api().size(theme, dc, part, state, nullptr, 1 /* TS_DRAW */, &native);
        if (native.cx < 1 || native.cy < 1) native.cx = native.cy = kThemeNativeBoxPixels;
        if (native.cx == width && native.cy == height) {
            RECT at = box;
            theme_api().draw(theme, dc, part, state, &at, nullptr);
            return;
        }
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, native.cx, native.cy);
        HGDIOBJ old = SelectObject(memory, bitmap);
        RECT whole = { 0, 0, native.cx, native.cy };
        fill(memory, whole, sys(COLOR_BTNFACE));
        theme_api().draw(theme, memory, part, state, &whole, nullptr);
        int oldMode = SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, box.left, box.top, width, height, memory, 0, 0, native.cx, native.cy, SRCCOPY);
        SetStretchBltMode(dc, oldMode);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        return;
    }
    RECT at = box;
    UINT kind = (radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK) | (checked ? DFCS_CHECKED : 0) | (enabled ? 0 : DFCS_INACTIVE) | (pressed ? DFCS_PUSHED : 0);
    DrawFrameControl(dc, &at, DFC_BUTTON, kind);
}

void paint_scaled_button(HWND window, ScaledButtonData* data, HDC target, const RECT& area)
{
    RECT rect = client_rect(window);
    UiContext* ctx = data->ctx;
    LONG style = GetWindowLongW(window, GWL_STYLE);
    int type = style & BS_TYPEMASK;
    bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    bool enabled = IsWindowEnabled(window) != FALSE;
    LRESULT buttonState = SendMessageW(window, BM_GETSTATE, 0, 0);
    bool pressed = (buttonState & BST_PUSHED) != 0;
    bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
    bool focused = GetFocus() == window;
    bool multiline = (style & BS_MULTILINE) != 0;
    bool centreBox = !multiline || (style & BS_VCENTER) == BS_VCENTER;
    DoubleBuffer buffer(target, area);
    HDC dc = buffer.dc();
    fill(dc, rect, sys(COLOR_BTNFACE));

    int boxSize = ctx ? ctx->px_min1(kThemeNativeBoxPixels) : kThemeNativeBoxPixels;
    int gap = ctx ? std::max(3, ctx->px(4)) : 4;
    int clientHeight = rect.bottom - rect.top;
    int boxTop = centreBox ? rect.top + (clientHeight - boxSize) / 2 : rect.top + 1;
    RECT box = { rect.left, boxTop, rect.left + boxSize, boxTop + boxSize };

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    if (font == nullptr) font = ctx ? ctx->ui_font() : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? sys(COLOR_BTNTEXT) : (ctx ? ctx->disabled_text_color() : sys(COLOR_GRAYTEXT)));

    int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    RECT textBox = { box.right + gap, rect.top, rect.right, rect.bottom };
    UINT flags = DT_LEFT | DT_NOPREFIX | (multiline ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER);
    RECT used = textBox;
    if (!text.empty()) {
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &used, flags | DT_CALCRECT);
        int textHeight = used.bottom - used.top;
        if (multiline) {
            int top = (style & BS_VCENTER) == BS_VCENTER ? rect.top + (clientHeight - textHeight) / 2 : rect.top;
            OffsetRect(&used, 0, top - used.top);
        }
        else {
            used.top = rect.top + (clientHeight - textHeight) / 2;
            used.bottom = used.top + textHeight;
        }
        used.right = std::min<LONG>(used.right, textBox.right);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &used, DT_LEFT | DT_NOPREFIX | (multiline ? DT_WORDBREAK : DT_SINGLELINE) | DT_NOCLIP);
    }
    SelectObject(dc, oldFont);

    draw_scaled_glyph(window, data, dc, box, radio, checked, enabled, pressed, data->hot && !pressed);

    if (focused && enabled) {
        // Keyboard focus cue: shown unless Windows is hiding focus cues (before the first keyboard use).
        LRESULT uiState = SendMessageW(window, 0x0129 /* WM_QUERYUISTATE */, 0, 0);
        if ((uiState & 1 /* UISF_HIDEFOCUS */) == 0) {
            RECT focus = used;
            if (text.empty()) focus = box;
            InflateRect(&focus, 2, 1);
            focus.left = std::max<LONG>(focus.left, rect.left);
            focus.right = std::min<LONG>(focus.right, rect.right);
            focus.top = std::max<LONG>(focus.top, rect.top);
            focus.bottom = std::min<LONG>(focus.bottom, rect.bottom);
            SetTextColor(dc, sys(COLOR_BTNTEXT));
            SetBkColor(dc, sys(COLOR_BTNFACE));
            DrawFocusRect(dc, &focus);
        }
    }
}

LRESULT CALLBACK scaled_button_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    ScaledButtonData* data = reinterpret_cast<ScaledButtonData*>(GetPropW(window, kScaledButtonProp));
    if (data == nullptr) return DefWindowProcW(window, message, wParam, lParam);
    WNDPROC original = data->original;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps = {};
        HDC target = message == WM_PAINT ? BeginPaint(window, &ps) : reinterpret_cast<HDC>(wParam);
        RECT area = message == WM_PAINT ? ps.rcPaint : client_rect(window);
        if (area.right > area.left && area.bottom > area.top) paint_scaled_button(window, data, target, area);
        if (message == WM_PAINT) EndPaint(window, &ps);
        return 0;
    }
    case WM_NCDESTROY: {
        close_button_theme(data);
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        RemovePropW(window, kScaledButtonProp);
        delete data;
        return CallWindowProcW(original, window, message, wParam, lParam);
    }
    case WM_THEMECHANGED:
        close_button_theme(data);
        break;
    case WM_MOUSEMOVE:
        if (!data->hot) {
            data->hot = true;
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        data->hot = false;
        InvalidateRect(window, nullptr, FALSE);
        break;
    default:
        break;
    }
    LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
    switch (message) {
    case BM_SETCHECK: case BM_SETSTATE: case BM_CLICK: case WM_ENABLE: case WM_SETTEXT: case WM_SETFONT:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_KEYDOWN: case WM_KEYUP:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_CAPTURECHANGED: case WM_CANCELMODE: case WM_SHOWWINDOW:
    case WM_THEMECHANGED: case 0x0128 /* WM_UPDATEUISTATE */:
        // The system control may have drawn its own (system-size) version for the change; draw ours over it at once.
        if (IsWindowVisible(window)) {
            InvalidateRect(window, nullptr, FALSE);
            UpdateWindow(window);
        }
        break;
    default:
        break;
    }
    return result;
}

} // namespace

void attach_scaled_button(HWND button, UiContext* ctx)
{
    if (button == nullptr || GetPropW(button, kScaledButtonProp) != nullptr) return;
    wchar_t className[16] = {};
    GetClassNameW(button, className, 16);
    if (_wcsicmp(className, L"Button") != 0) return;   // the style bits mean something else in a combo box, an edit box...
    int type = GetWindowLongW(button, GWL_STYLE) & BS_TYPEMASK;
    if (type != BS_CHECKBOX && type != BS_AUTOCHECKBOX && type != BS_RADIOBUTTON && type != BS_AUTORADIOBUTTON) return;
    ScaledButtonData* data = new ScaledButtonData();
    data->ctx = ctx;
    data->original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(button, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(scaled_button_proc)));
    SetPropW(button, kScaledButtonProp, data);
    InvalidateRect(button, nullptr, TRUE);
}

} // namespace ui
} // namespace midibn
