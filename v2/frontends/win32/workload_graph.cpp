#include "workload_graph.h"

#include "display_pacer.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace midibn {
namespace ui {

namespace {

using app::GraphRect;


// ---- v1's colours (WorkloadGraph.cs) ---------------------------------------------------------
struct Rgb {
    int r, g, b;
};
const Rgb kBackground = { 24, 27, 32 };
const Rgb kForeground = { 220, 220, 220 };       // Color.Gainsboro
const Rgb kSilver = { 192, 192, 192 };
const Rgb kGrid = { 65, 75, 86 };
const Rgb kEventsPlot = { 63, 190, 255 };
const Rgb kBytesPlot = { 255, 174, 66 };
const Rgb kGatePlot = { 255, 190, 92 };
const Rgb kCapacity = { 110, 220, 130 };         // ARGB (220, 110, 220, 130)
const Rgb kOverflow = { 235, 65, 65 };           // alpha 65
const Rgb kTickPen = { 155, 165, 175 };
const Rgb kCluster = { 195, 90, 220 };           // alpha 225
const Rgb kPressure[4] = { { 75, 150, 95 }, { 230, 170, 45 }, { 230, 65, 65 }, { 155, 75, 190 } };
const int kPressureAlpha[4] = { 100, 150, 190, 210 };
const Rgb kPlaybackMarker = { 205, 225, 255 };
const Rgb kOutputMarker = { 130, 230, 185 };
const Rgb kOverlayBackground = { 12, 15, 19 };   // alpha 218
const Rgb kOverlayOutline = { 190, 195, 202 };   // alpha 130
const Rgb kWhiteSmoke = { 245, 245, 245 };
const Rgb kHover = { 245, 215, 100 };            // alpha 230
const Rgb kTipBackground = { 40, 44, 50 };       // alpha 220

COLORREF colour(const Rgb& c) { return RGB(c.r, c.g, c.b); }

// A colour with alpha, composed against the plot background (for pens, which GDI cannot blend).
Rgb over_background(const Rgb& c, int alpha)
{
    return { kBackground.r + (c.r - kBackground.r) * alpha / 255, kBackground.g + (c.g - kBackground.g) * alpha / 255,
        kBackground.b + (c.b - kBackground.b) * alpha / 255 };
}

struct Dib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    int width = 0, height = 0;

    void destroy()
    {
        if (dc) {
            SelectObject(dc, old);
            DeleteDC(dc);
        }
        if (bitmap) DeleteObject(bitmap);
        dc = nullptr;
        bitmap = nullptr;
        bits = nullptr;
        width = height = 0;
    }
    bool create(int w, int h)
    {
        destroy();
        if (w <= 0 || h <= 0) return false;
        HDC screen = GetDC(nullptr);
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!bitmap || !dc) {
            destroy();
            return false;
        }
        old = SelectObject(dc, bitmap);
        bits = static_cast<uint32_t*>(pixels);
        width = w;
        height = h;
        return true;
    }
    void blend(int x, int y, int w, int h, const Rgb& c, int alpha)
    {
        int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(width, x + w), y1 = std::min(height, y + h);
        for (int row = y0; row < y1; ++row) {
            uint32_t* p = bits + static_cast<size_t>(row) * width;
            for (int col = x0; col < x1; ++col) {
                uint32_t v = p[col];
                int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
                r += (c.r - r) * alpha / 255;
                g += (c.g - g) * alpha / 255;
                b += (c.b - b) * alpha / 255;
                p[col] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            }
        }
    }
};

struct GraphState {
    UiContext* ctx = nullptr;
    app::AnalysisWindowModel* model = nullptr;
    HWND window = nullptr;
    HWND tip = nullptr;
    HFONT font = nullptr;
    HDC measure_dc = nullptr;
    HGDIOBJ measure_old = nullptr;
    Dib statics, back;
    uint64_t static_revision = 0;
    int static_width = 0, static_height = 0;
    bool timer_on = false;   // the graph's animation is running (paced by the display, see DisplayPacer)
    int pacer_token = 0;
    bool tracking = false;
    int pointer_x = -1, pointer_y = -1;
    std::wstring tip_text;
};

GraphState* state_of(HWND window) { return reinterpret_cast<GraphState*>(GetWindowLongPtrW(window, GWLP_USERDATA)); }

void make_font(GraphState* s)
{
    LOGFONTW logfont = {};
    GetObjectW(s->ctx->ui_font(), sizeof(logfont), &logfont);
    // v1: UiScaleFont.CreateUi(8F, ...): the 9 pt UI font scaled to 8 pt.
    logfont.lfHeight = -std::max(1, static_cast<int>(std::lround(std::abs(logfont.lfHeight) * 8.0 / 9.0)));
    if (s->measure_dc && s->measure_old) SelectObject(s->measure_dc, s->measure_old);
    if (s->font) DeleteObject(s->font);
    s->font = CreateFontIndirectW(&logfont);
    if (s->measure_dc) s->measure_old = SelectObject(s->measure_dc, s->font);
}

void install_model_hooks(GraphState* s)
{
    app::WorkloadGraphModel& graph = s->model->graph();
    graph.set_text_measure([s](const std::string& text, bool) {
        app::GraphSize size;
        std::wstring wide = W(text);
        SIZE extent = {};
        if (!wide.empty()) GetTextExtentPoint32W(s->measure_dc, wide.c_str(), static_cast<int>(wide.size()), &extent);
        size.width = extent.cx;
        size.height = extent.cy;
        return size;
    });
    UiContext* ctx = s->ctx;
    graph.set_scale([ctx](int canonical) { return ctx->px(canonical); });
}

void notify_parent(GraphState* s)
{
    HWND parent = GetParent(s->window);
    if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s->window), GN_MODEL_CHANGED), reinterpret_cast<LPARAM>(s->window));
}

void request_paint(GraphState* s)
{
    if (s->model->graph().take_repaint_request()) InvalidateRect(s->window, nullptr, FALSE);
}

void after_input(GraphState* s)
{
    app::WorkloadGraphModel& graph = s->model->graph();
    if (graph.needs_dynamic_update() && !s->timer_on) {
        s->pacer_token = DisplayPacer::instance().subscribe(s->window);
        s->timer_on = true;
    }
    request_paint(s);
    notify_parent(s);
}

// ---- drawing ------------------------------------------------------------------------------

void text_out(HDC dc, HFONT font, const std::string& text, const GraphRect& r, const Rgb& c, UINT flags)
{
    std::wstring wide = W(text);
    RECT rect = { r.x, r.y, r.x + r.width, r.y + r.height };
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour(c));
    DrawTextW(dc, wide.c_str(), static_cast<int>(wide.size()), &rect, flags | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

void line(HDC dc, int x0, int y0, int x1, int y1, const Rgb& c, int style = PS_SOLID, int width = 1)
{
    HPEN pen = CreatePen(style, width, colour(c));
    HGDIOBJ old = SelectObject(dc, pen);
    SetBkMode(dc, TRANSPARENT);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void polyline(HDC dc, const std::vector<int>& ys, int x0, const Rgb& c, int width)
{
    if (ys.size() < 2) return;
    std::vector<POINT> points(ys.size());
    for (size_t i = 0; i < ys.size(); ++i) {
        points[i].x = x0 + static_cast<LONG>(i);
        points[i].y = ys[i];
    }
    HPEN pen = CreatePen(PS_SOLID, width, colour(c));
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, old);
    DeleteObject(pen);
}

void draw_static(GraphState* s)
{
    const app::GraphStaticFrame& f = s->model->graph().static_frame();
    Dib& d = s->statics;
    UiContext* ctx = s->ctx;
    HDC dc = d.dc;
    {
        RECT all = { 0, 0, d.width, d.height };
        HBRUSH brush = CreateSolidBrush(colour(kBackground));
        FillRect(dc, &all, brush);
        DeleteObject(brush);
    }
    if (f.empty) {
        GraphRect r;
        r.width = d.width;
        r.height = d.height;
        text_out(dc, s->font, f.message, r, kSilver, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }
    const GraphRect& a = f.area;
    text_out(dc, s->font, f.scope.text, f.scope.bounds, kForeground, DT_LEFT | DT_VCENTER);
    // panel frame and grid
    {
        HPEN pen = CreatePen(PS_SOLID, 1, colour(kGrid));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, a.x, a.y, a.x + a.width + 1, a.y + a.height + 1);
        SelectObject(dc, oldBrush);
        for (size_t i = 0; i < f.grid_y.size(); ++i) {
            MoveToEx(dc, a.x, f.grid_y[i], nullptr);
            LineTo(dc, a.right(), f.grid_y[i]);
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    for (size_t i = 0; i < f.overflow_x.size(); ++i) d.blend(f.overflow_x[i], a.y, 1, a.height, kOverflow, 65);
    int plotWidth = std::max(1, ctx->px(3) / 2 + ctx->px(3) % 2);   // 1.5 canonical pixels, rounded up
    polyline(dc, f.source_y, a.x, f.bytes_model ? kBytesPlot : kEventsPlot, plotWidth);
    if (!f.gate_y.empty()) polyline(dc, f.gate_y, a.x, kGatePlot, plotWidth);
    if (f.capacity_y >= 0) line(dc, a.x, f.capacity_y, a.right(), f.capacity_y, over_background(kCapacity, 220), PS_DASH);
    text_out(dc, s->font, f.title.text, f.title.bounds, kForeground, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    text_out(dc, s->font, f.scale.text, f.scale.bounds, kForeground, DT_RIGHT | DT_VCENTER);
    for (size_t i = 0; i < f.ticks.size(); ++i) {
        const app::GraphAxisTick& t = f.ticks[i];
        line(dc, t.x, t.mark_top, t.x, t.mark_bottom, kTickPen);
        text_out(dc, s->font, t.label, t.label_bounds, kSilver, DT_CENTER | DT_VCENTER);
    }
    // marker lanes
    {
        Rgb cluster = over_background(kCluster, 225);
        HBRUSH brush = CreateSolidBrush(colour(cluster));
        HPEN pen = CreatePen(PS_SOLID, 1, colour(cluster));
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        int r = f.cluster_radius;
        for (size_t i = 0; i < f.cluster_x.size(); ++i) {
            int x = f.cluster_x[i];
            POINT diamond[4] = { { x, f.cluster_y - r }, { x + r, f.cluster_y }, { x, f.cluster_y + r }, { x - r, f.cluster_y } };
            Polygon(dc, diamond, 4);
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
    for (size_t i = 0; i < f.pressure.size(); ++i) {
        int level = static_cast<int>(f.pressure[i].level);
        d.blend(f.pressure[i].x, f.pressure_y, 1, f.pressure_height, kPressure[level], kPressureAlpha[level]);
    }
    for (size_t i = 0; i < f.legend.size(); ++i)
        text_out(dc, s->font, f.legend[i].text, f.legend[i].bounds, kSilver, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

void draw_dynamic(GraphState* s)
{
    app::GraphDynamicFrame f = s->model->graph().dynamic_frame();
    Dib& d = s->back;
    HDC dc = d.dc;
    const GraphRect a = s->model->graph().graph_area();
    for (size_t i = 0; i < f.markers.size(); ++i) {
        if (f.markers[i].midi_output) line(dc, f.markers[i].x, a.y, f.markers[i].x, a.bottom(), over_background(kOutputMarker, 225), PS_DASHDOT);
        else line(dc, f.markers[i].x, a.y, f.markers[i].x, a.bottom(), over_background(kPlaybackMarker, 225), PS_DOT);
    }
    if (f.overlay_visible) {
        const GraphRect& o = f.overlay_bounds;
        d.blend(o.x, o.y, o.width, o.height, kOverlayBackground, 218);
        d.blend(o.x, o.y, o.width, 1, kOverlayOutline, 130);
        d.blend(o.x, o.y + o.height - 1, o.width, 1, kOverlayOutline, 130);
        d.blend(o.x, o.y, 1, o.height, kOverlayOutline, 130);
        d.blend(o.x + o.width - 1, o.y, 1, o.height, kOverlayOutline, 130);
        for (size_t i = 0; i < f.overlay_lines.size(); ++i)
            text_out(dc, s->font, f.overlay_lines[i].text, f.overlay_lines[i].bounds, kWhiteSmoke, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (f.pin_visible) line(dc, f.pin_x, a.y, f.pin_x, a.bottom(), { 255, 255, 255 }, PS_DASH);
    if (f.hover_visible) line(dc, f.hover_x, a.y, f.hover_x, a.bottom(), over_background(kHover, 230));
    if (f.tip_visible) {
        const GraphRect& t = f.tip_bounds;
        d.blend(t.x, t.y, t.width, t.height, kTipBackground, 220);
        GraphRect text = t;
        text.x += s->ctx->px(4);
        text.width -= s->ctx->px(6);
        text_out(dc, s->font, f.tip_text, text, { 255, 255, 255 }, DT_LEFT | DT_VCENTER);
    }
}

void paint_to(GraphState* s, HDC target)
{
    RECT client;
    GetClientRect(s->window, &client);
    int w = client.right, h = client.bottom;
    if (w <= 0 || h <= 0) return;
    if (s->statics.width != w || s->statics.height != h) {
        s->statics.create(w, h);
        s->back.create(w, h);
        s->static_revision = 0;
    }
    if (!s->statics.bits || !s->back.bits) return;
    uint64_t revision = s->model->graph().static_revision();
    if (s->static_revision != revision || s->static_width != w || s->static_height != h) {
        draw_static(s);
        s->static_revision = s->model->graph().static_revision();
        s->static_width = w;
        s->static_height = h;
    }
    std::memcpy(s->back.bits, s->statics.bits, static_cast<size_t>(w) * h * sizeof(uint32_t));
    draw_dynamic(s);
    BitBlt(target, 0, 0, w, h, s->back.dc, 0, 0, SRCCOPY);
}

// ---- tooltip ------------------------------------------------------------------------------

void update_tooltip(GraphState* s, int x, int y)
{
    if (!s->tip) return;
    std::wstring text = W(s->model->graph().tooltip_text(x, y));
    if (text == s->tip_text) return;
    s->tip_text = text;
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.hwnd = s->window;
    info.uId = reinterpret_cast<UINT_PTR>(s->window);
    info.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(s->tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
}

void create_tooltip(GraphState* s)
{
    s->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, s->window, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!s->tip) return;
    SendMessageW(s->tip, TTM_SETMAXTIPWIDTH, 0, 600);
    SendMessageW(s->tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    TOOLINFOW info = {};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = s->window;
    info.uId = reinterpret_cast<UINT_PTR>(s->window);
    info.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(s->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    s->tip_text = W(s->model->graph().tooltip_text(-1, -1));
}

// ---- window procedure ---------------------------------------------------------------------

LRESULT CALLBACK graph_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    GraphState* s = state_of(window);
    if (message == DisplayPacer::message()) {
        // One display-paced animation step (a message posted just before the animation ended is ignored).
        if (s && s->timer_on && static_cast<int>(wParam) == s->pacer_token) {
            if (s->model->graph_dynamic_update()) InvalidateRect(window, nullptr, FALSE);
            if (!s->model->graph().needs_dynamic_update()) {
                DisplayPacer::instance().unsubscribe(s->pacer_token);
                s->timer_on = false;
            }
            request_paint(s);
            notify_parent(s);
        }
        DisplayPacer::instance().finished(static_cast<int>(wParam));
        return 0;
    }
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        return TRUE;
    case WM_CREATE: {
        s = state_of(window);
        s->window = window;
        HDC screen = GetDC(nullptr);
        s->measure_dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        make_font(s);
        install_model_hooks(s);
        create_tooltip(s);
        return 0;
    }
    case WM_NCDESTROY:
        if (s) {
            if (s->timer_on) DisplayPacer::instance().unsubscribe(s->pacer_token);
            s->statics.destroy();
            s->back.destroy();
            if (s->measure_dc) {
                SelectObject(s->measure_dc, s->measure_old);
                DeleteDC(s->measure_dc);
            }
            if (s->font) DeleteObject(s->font);
            // The model outlives the control and must not call back into it.
            s->model->graph().set_text_measure([](const std::string& text, bool) {
                app::GraphSize size;
                size.width = static_cast<int>(text.size()) * 6;
                size.height = 13;
                return size;
            });
            delete s;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (s) {
            s->model->graph_resized(LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(window, nullptr, FALSE);
            after_input(s);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        if (s) paint_to(s, dc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (s) paint_to(s, reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETCURSOR:
        if (s && LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, s->model->graph().cursor() == app::GraphCursor::SizeWE ? IDC_SIZEWE : IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (s) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (!s->tracking) {
                TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, window, 0 };
                TrackMouseEvent(&track);
                s->tracking = true;
            }
            s->pointer_x = x;
            s->pointer_y = y;
            s->model->graph_mouse_move(x, y);
            update_tooltip(s, x, y);
            after_input(s);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (s) {
            s->tracking = false;
            s->pointer_x = s->pointer_y = -1;
            s->model->graph_mouse_leave();
            update_tooltip(s, -1, -1);
            after_input(s);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (s) {
            SetFocus(window);
            s->model->graph_mouse_down(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (s->model->graph().mouse_captured()) SetCapture(window);
            after_input(s);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s) {
            bool had = s->model->graph().mouse_captured();
            s->model->graph_mouse_up(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (had && GetCapture() == window) ReleaseCapture();
            after_input(s);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (s && s->model->graph().mouse_captured() && reinterpret_cast<HWND>(lParam) != window) {
            // capture lost (for example a dialog opened): end the gesture without pinning
            s->model->graph_mouse_up(-100000, -100000);
            after_input(s);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (s) {
            s->model->graph_double_click(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            after_input(s);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (s) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            s->model->graph_wheel(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wParam));
            after_input(s);
        }
        return 0;
    case WM_NOTIFY:
        if (s) {
            NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == s->tip && header->code == TTN_GETDISPINFOW) {
                NMTTDISPINFOW* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                // v1 shows the live hint or the full playback values; ask for the text under the pointer now.
                POINT cursor;
                GetCursorPos(&cursor);
                ScreenToClient(window, &cursor);
                s->tip_text = W(s->model->graph().tooltip_text(cursor.x, cursor.y));
                info->lpszText = const_cast<wchar_t*>(s->tip_text.c_str());
                return 0;
            }
        }
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

void register_workload_graph_class(HINSTANCE instance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = graph_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kWorkloadGraphClass;
    RegisterClassExW(&wc);
}

HWND create_workload_graph(HWND parent, int id, UiContext* ctx, app::AnalysisWindowModel* model)
{
    GraphState* s = new GraphState();
    s->ctx = ctx;
    s->model = model;
    HWND window = CreateWindowExW(0, kWorkloadGraphClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), s);
    if (!window) delete s;
    return window;
}

void workload_graph_scale_changed(HWND graph)
{
    GraphState* s = state_of(graph);
    if (!s) return;
    make_font(s);
    install_model_hooks(s);
    s->static_revision = 0;
    InvalidateRect(graph, nullptr, FALSE);
}

void workload_graph_invalidate(HWND graph)
{
    GraphState* s = state_of(graph);
    if (!s) return;
    request_paint(s);
    if (s->model->graph().needs_dynamic_update() && !s->timer_on) {
        s->pacer_token = DisplayPacer::instance().subscribe(graph);
        s->timer_on = true;
    }
}

void workload_graph_paint_to(HWND graph, HDC dc)
{
    GraphState* s = state_of(graph);
    if (s) paint_to(s, dc);
}

} // namespace ui
} // namespace midibn
