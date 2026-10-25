#include "ui_common.h"

#include "win32_platform.h"
#include "winmm_midi_output.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace midibn {
namespace ui {

std::wstring W(const std::string& utf8) { return win32::utf8_to_wide(utf8); }
std::string U(const std::wstring& wide) { return win32::wide_to_utf8(wide); }

namespace {

const int kMaxMarks = 48;
struct Mark { const char* name; LONGLONG counter; };
Mark g_marks[kMaxMarks];
int g_mark_count = 0;

int g_edge_band = 0;

int CALLBACK font_probe(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found)
{
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

} // namespace

void startup_mark(const char* name)
{
    if (g_mark_count >= kMaxMarks) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_marks[g_mark_count].name = name;
    g_marks[g_mark_count].counter = now.QuadPart;
    ++g_mark_count;
}

namespace {

HANDLE g_device_thread = nullptr;
std::vector<platform::OutputDeviceInfo> g_prefetched_devices;

DWORD WINAPI list_devices_thread(LPVOID)
{
    g_prefetched_devices = win32::WinMmMidiOutput::enumerate_devices();
    return 0;
}

} // namespace

void prefetch_output_devices()
{
    if (!g_device_thread) g_device_thread = CreateThread(nullptr, 0, &list_devices_thread, nullptr, 0, nullptr);
}

std::vector<platform::OutputDeviceInfo> take_output_devices()
{
    if (!g_device_thread) return win32::WinMmMidiOutput::enumerate_devices();
    WaitForSingleObject(g_device_thread, INFINITE);
    CloseHandle(g_device_thread);
    g_device_thread = nullptr;
    std::vector<platform::OutputDeviceInfo> devices;
    devices.swap(g_prefetched_devices);
    return devices;
}

std::string startup_marks_text()
{
    std::string text;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < g_mark_count; ++i) {
        char line[96];
        std::snprintf(line, sizeof line, "mark_%s_ms=%.2f\n", g_marks[i].name,
            (g_marks[i].counter - g_marks[0].counter) * 1000.0 / static_cast<double>(f.QuadPart));
        text += line;
    }
    return text;
}

bool UiContext::font_exists(const wchar_t* face)
{
    HDC dc = GetDC(nullptr);
    LOGFONTW logfont = {};
    logfont.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(logfont.lfFaceName, face, _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(dc, &logfont, font_probe, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

UiContext::UiContext()
{
    HDC dc = GetDC(nullptr);
    dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (dpi_ < 96) dpi_ = 96;
    rebuild();
}

UiContext::~UiContext() { release(); }

void UiContext::release()
{
    HFONT* fonts[] = { &ui_, &ui_bold_, &caption_, &mono_, &mono_bold_ };
    for (size_t i = 0; i < 5; ++i)
        if (*fonts[i]) { DeleteObject(*fonts[i]); *fonts[i] = nullptr; }
}

void UiContext::set_scale(int percent)
{
    percent = std::max(50, std::min(200, percent));
    if (percent == scale_percent_) return;
    scale_percent_ = percent;
    rebuild();
}

int UiContext::px(int canonical) const
{
    if (canonical == 0) return 0;
    long long scaled = (static_cast<long long>(canonical) * scale_percent_ * dpi_ + 4800) / 9600;
    int value = static_cast<int>(scaled);
    return canonical > 0 ? std::max(1, value) : std::min(-1, value);
}

int UiContext::px_min1(int canonical) const
{
    long long scaled = (static_cast<long long>(canonical) * scale_percent_ * dpi_ + 4800) / 9600;
    return static_cast<int>(std::max<long long>(1, scaled));
}

COLORREF UiContext::disabled_text_color() const
{
    COLORREF grey = GetSysColor(COLOR_GRAYTEXT);
    if (scale_percent_ >= 100) return grey;
    int percent = scale_percent_ <= 50 ? 62 : 78;   // 75 %: a little darker; 50 %: darker still
    return RGB(GetRValue(grey) * percent / 100, GetGValue(grey) * percent / 100, GetBValue(grey) * percent / 100);
}

HFONT UiContext::make(const wchar_t* canonicalFace, const wchar_t* smallFace, double points, bool bold)
{
    // v1 UiScaleFont: geometry follows the canonical scale; below 100% the small-size faces are crisper.
    double scaled = points * scale_percent_ / 100.0;
    if (scale_percent_ == 50) scaled = std::max(scaled, 6.0);
    else if (scale_percent_ == 75) scaled = std::max(scaled, 7.0);
    const wchar_t* face = scale_percent_ < 100 ? smallFace : canonicalFace;
    if (!font_exists(face)) face = font_exists(canonicalFace) ? canonicalFace : nullptr;
    LOGFONTW logfont = {};
    if (face == nullptr) {
        // Windows XP has no Segoe UI / Consolas: use the system message font face.
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        wcsncpy_s(logfont.lfFaceName, metrics.lfMessageFont.lfFaceName, _TRUNCATE);
        if (wcsstr(canonicalFace, L"Consolas") != nullptr) wcsncpy_s(logfont.lfFaceName, L"Courier New", _TRUNCATE);
    }
    else wcsncpy_s(logfont.lfFaceName, face, _TRUNCATE);
    logfont.lfHeight = -static_cast<LONG>(scaled * dpi_ / 72.0 + 0.5);
    logfont.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&logfont);
}

void UiContext::rebuild()
{
    release();
    for (int i = 0; i < 5; ++i) height_cache_font_[i] = nullptr;
    ui_ = make(L"Segoe UI", L"Tahoma", 9.0, false);
    ui_bold_ = make(L"Segoe UI", L"Tahoma", 9.0, true);
    caption_ = make(L"Segoe UI", L"Tahoma", 8.5, false);
    mono_ = make(L"Consolas", L"Lucida Console", 9.0, false);
    mono_bold_ = make(L"Consolas", L"Lucida Console", 9.0, true);
    font_height_ = font_height_of(ui_);
}

int UiContext::font_height_of(HFONT font) const
{
    for (int i = 0; i < 5; ++i)
        if (height_cache_font_[i] == font && font != nullptr) return height_cache_[i];
    int h = measure_font_height(font);
    for (int i = 0; i < 5; ++i)
        if (height_cache_font_[i] == nullptr) { height_cache_font_[i] = font; height_cache_[i] = h; break; }
    return h;
}

int UiContext::measure_font_height(HFONT font) const
{
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}

int UiContext::text_width(HFONT font, const std::wstring& text) const
{
    if (text.empty()) return 0;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return size.cx;
}

void show_message(HWND owner, const std::string& title, const std::string& text, int kind)
{
    UINT icon = kind == 2 ? MB_ICONERROR : kind == 1 ? MB_ICONWARNING : MB_ICONINFORMATION;
    MessageBoxW(owner, W(text).c_str(), W(title).c_str(), MB_OK | icon);
}

// ---------------------------------------------------------------- placement (DD-098)

RECT monitor_work_area(HWND window)
{
    RECT work = {};
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    HMONITOR monitor = window && IsWindow(window) ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                                  : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

POINT keep_in_work_area(int x, int y, int width, int height, const RECT& work)
{
    // Right and bottom first, then left and top, so a window larger than the work area keeps its caption visible.
    POINT p = { x, y };
    if (p.x + width > work.right) p.x = work.right - width;
    if (p.y + height > work.bottom) p.y = work.bottom - height;
    if (p.x < work.left) p.x = work.left;
    if (p.y < work.top) p.y = work.top;
    return p;
}

POINT centre_in_work_area(const RECT& owner, const RECT& work, int width, int height)
{
    int x = owner.left + ((owner.right - owner.left) - width) / 2;
    int y = owner.top + ((owner.bottom - owner.top) - height) / 2;
    return keep_in_work_area(x, y, width, height, work);
}

RECT startup_window_rect(const RECT* monitors, const RECT* works, int count, POINT mouse, int width, int height)
{
    RECT out = { 0, 0, width, height };
    if (count <= 0) return out;
    int best = 0;
    double bestDistance = -1.0;
    for (int i = 0; i < count; ++i) {
        const RECT& m = monitors[i];
        double dx = mouse.x < m.left ? m.left - mouse.x : mouse.x >= m.right ? mouse.x - m.right + 1 : 0;
        double dy = mouse.y < m.top ? m.top - mouse.y : mouse.y >= m.bottom ? mouse.y - m.bottom + 1 : 0;
        double distance = dx * dx + dy * dy;   // 0 when the point is inside
        if (bestDistance < 0 || distance < bestDistance) { best = i; bestDistance = distance; }
    }
    POINT p = centre_in_work_area(works[best], works[best], width, height);
    out.left = p.x;
    out.top = p.y;
    out.right = p.x + width;
    out.bottom = p.y + height;
    return out;
}

namespace {

struct MonitorList { RECT monitors[16]; RECT works[16]; int count = 0; };

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    MonitorList* list = reinterpret_cast<MonitorList*>(data);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (list->count < 16 && GetMonitorInfoW(monitor, &info)) {
        list->monitors[list->count] = info.rcMonitor;
        list->works[list->count] = info.rcWork;
        ++list->count;
    }
    return TRUE;
}

} // namespace

POINT startup_window_origin(int width, int height)
{
    MonitorList list;
    POINT mouse = {};
    GetCursorPos(&mouse);
    EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&list));
    if (list.count == 0) {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        list.monitors[0] = list.works[0] = work;
        list.count = 1;
    }
    RECT r = startup_window_rect(list.monitors, list.works, list.count, mouse, width, height);
    POINT p = { r.left, r.top };
    return p;
}

int caption_fit_client_width(const std::wstring& title, DWORD style, DWORD exStyle)
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    BOOL got = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0);
    if (!got) {   // Windows XP knows the structure without its last field (iPaddedBorderWidth)
        metrics.cbSize = sizeof(metrics) - sizeof(int);
        got = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0);
    }
    if (!got) return 0;
    HFONT font = CreateFontIndirectW(&metrics.lfCaptionFont);
    if (!font) return 0;
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    DeleteObject(font);
    // the window's whole width: the small icon and the gaps around it, the title, the close button (on Windows 10 it
    // is wider than SM_CXSIZE says: allow 1.5 times) and a margin before it
    int outer = GetSystemMetrics(SM_CXSMICON) + 16 + size.cx + GetSystemMetrics(SM_CXSIZE) * 3 / 2 + 16;
    RECT frame = { 0, 0, 100, 100 };
    AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    return outer - ((frame.right - frame.left) - 100);
}

POINT centre_on_owner(HWND owner, int width, int height)
{
    RECT work = monitor_work_area(owner);
    RECT own = work;
    if (!owner || !IsWindow(owner) || !GetWindowRect(owner, &own)) own = work;
    return centre_in_work_area(own, work, width, height);
}

// ---------------------------------------------------------------- edge pass-through

void set_edge_band(int pixels) { g_edge_band = pixels; }
int edge_band() { return g_edge_band; }

namespace {

LRESULT CALLBACK passthrough_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_NCHITTEST && g_edge_band > 0) {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr && !IsZoomed(root)) {
            POINT point = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            RECT rect;
            GetWindowRect(root, &rect);
            if (point.x < rect.left + g_edge_band || point.x >= rect.right - g_edge_band ||
                point.y < rect.top + g_edge_band || point.y >= rect.bottom - g_edge_band)
                return HTTRANSPARENT;
        }
    }
    if (message == WM_DROPFILES) {
        // Every control accepts a dropped MIDI file: hand it to the main window.
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != nullptr && root != window) return SendMessageW(root, WM_DROPFILES, wParam, lParam);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, passthrough_proc, 1);
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

bool install_edge_passthrough(HWND child, int)
{
    return SetWindowSubclass(child, passthrough_proc, 1, 0) != FALSE;
}

} // namespace ui
} // namespace midibn
