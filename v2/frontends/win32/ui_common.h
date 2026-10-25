// Shared Win32 frontend helpers: metrics (DPI and Application scale), fonts,
// string conversion, and the custom window messages between the controls and
// the main window. Plain Win32 only; everything newer than Windows XP SP3 is
// resolved at run time.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>   // declares IUnknown before the old SDK's objbase.h (XP toolset, /permissive-)

#include "midibn/midi_output.h"

#include <algorithm>
#include <string>
#include <vector>

namespace midibn {
namespace ui {

// Custom messages (sent to a control's parent).
const UINT WM_MIDIBN_FIELD = WM_APP + 10;      // wParam = control id, lParam = new value (pointer to int64_t)
const UINT WM_MIDIBN_SEEK = WM_APP + 11;       // wParam = control id, lParam = pointer to int64_t position (us)
const UINT WM_MIDIBN_SPEEDMENU = WM_APP + 12;  // statistics view: right click on Effective speed; lParam = screen point
const UINT WM_MIDIBN_WAKE = WM_APP + 13;       // posted by the engine/loader threads
const UINT WM_MIDIBN_DRAGGING = WM_APP + 14;   // timeline: wParam = dragging (0/1)
const UINT WM_MIDIBN_CAPTION = WM_APP + 15;    // caption button clicked: wParam = id
const UINT WM_MIDIBN_HELP = WM_APP + 17;        // main window: open Help; wParam = topic index (F1 in the Track routing editor)
const UINT WM_MIDIBN_ACCESSIBLE_NAMES = WM_APP + 16;   // main window: set the accessible names collected during construction

std::wstring W(const std::string& utf8);
std::string U(const std::wstring& wide);

// Start-up timing marks (DD-055): the performance-counter reading at named points of the start-up. The first mark is
// the origin; --report prints every mark as "mark_<name>_ms=<ms since the first mark>". Cheap (a counter read).
void startup_mark(const char* name);
std::string startup_marks_text();

// DD-057: Windows takes tens of milliseconds to list the MIDI output devices (WinMM starts every installed driver),
// and the window is built meanwhile. prefetch_output_devices() starts the listing on a worker thread (call it first
// thing); take_output_devices() returns its result, waiting if needed, or lists the devices right there if no
// prefetch was started. The result is the same list either way.
void prefetch_output_devices();

// DD-058: sets the accessible names collected while the main window was built (main_window.cpp).
void flush_accessible_names();
std::vector<platform::OutputDeviceInfo> take_output_devices();

// Metrics and fonts of one window family (the main window and its companions).
// px() is the canonical-pixel -> device-pixel mapping: dpi/96 * scale/100.
class UiContext {
public:
    UiContext();
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    void set_scale(int percent);          // 50..200 (Application scale)
    int scale_percent() const { return scale_percent_; }
    int dpi() const { return dpi_; }
    int px(int canonical) const;          // v1 ScaleMetric (signed, rounds half up, at least 1 for positive)
    int px_min1(int canonical) const;     // Math.Max(1, (v*p+50)/100)

    HFONT ui_font() const { return ui_; }
    HFONT ui_bold_font() const { return ui_bold_; }
    HFONT caption_font() const { return caption_; }      // statistics captions (8.5 pt)
    HFONT mono_font() const { return mono_; }            // timeline time (Consolas 9 pt)
    HFONT mono_bold_font() const { return mono_bold_; }  // statistics values (Consolas 9 pt bold)
    int font_height() const { return font_height_; }     // tmHeight of the UI font
    int text_width(HFONT font, const std::wstring& text) const;
    int text_width(const std::wstring& text) const { return text_width(ui_, text); }
    int font_height_of(HFONT font) const;
    int measure_font_height(HFONT font) const;
    // DD-099: room that does not shrink with the Application scale. Windows draws a check box's box at the system size
    // and a push button's text inside a fixed border, while the font shrinks less than the layout below 100 %, so at
    // 50 % check-box labels ("KDMAPI", "Queue length limit:", "Playback statistics", channels 10-16) and the lower
    // part of button texts were cut off. At 100 % and above (and 75 % for buttons) the values are unchanged.
    // P10-UI1: check boxes and radio buttons are now drawn at the Application scale (controls.h
    // attach_scaled_button), so their room follows the scale like everything else.
    int check_box_space(int canonical = 22) const { return px(canonical); }
    // P10-UI1: the colour of disabled (greyed-out) text: the system's grey-text colour at 100 % and above, a little
    // darker below 100 % (small fonts are thin, so the system grey was hard to read at 75 % and 50 %).
    COLORREF disabled_text_color() const;
    int button_padding(int canonical = 8) const { return std::max(px(canonical), 8); }

    // v1 UiScaleFont families: Segoe UI / Consolas at or above 100%, Tahoma / Lucida Console below.
    static bool font_exists(const wchar_t* face);

private:
    void rebuild();
    void release();
    HFONT make(const wchar_t* canonicalFace, const wchar_t* smallFace, double points, bool bold);

    int dpi_ = 96;
    int scale_percent_ = 100;
    HFONT ui_ = nullptr, ui_bold_ = nullptr, caption_ = nullptr, mono_ = nullptr, mono_bold_ = nullptr;
    int font_height_ = 15;
    mutable HFONT height_cache_font_[5] = {};
    mutable int height_cache_[5] = {};
};

// A message box owned by `owner` with the icon of `kind` (0 info, 1 warning, 2 error).
void show_message(HWND owner, const std::string& title, const std::string& text, int kind);

// DD-098: every secondary window and dialog opens on the monitor of the window it belongs to (v1 Screen.FromControl /
// FormStartPosition.CenterParent), never on the primary monitor merely because it is the primary one, and never partly
// outside that monitor's work area.
// The work area of the monitor that holds most of `window` (the nearest one; the primary monitor's when `window` is
// null or no longer exists).
RECT monitor_work_area(HWND window);
// Moves a width x height window whose top-left corner would be (x, y) inside `work` (not resized; when it is larger
// than the work area its left and top edges stay inside). Pure arithmetic.
POINT keep_in_work_area(int x, int y, int width, int height, const RECT& work);
// v1 Form.CenterToParent: centred over `owner`, then kept inside `work`. Pure arithmetic.
POINT centre_in_work_area(const RECT& owner, const RECT& work, int width, int height);
// DD-106 (v1 FormStartPosition.CenterScreen): where the main window opens. `monitors` and `works` are the
// monitor rectangles and work areas (same order, `count` of them); the window is centred in the work area of the
// monitor that contains `mouse` (the nearest one when the point is outside all of them), then kept inside it. Pure
// arithmetic. Returns the window's rectangle; for count 0 it returns the rectangle at (0, 0).
RECT startup_window_rect(const RECT* monitors, const RECT* works, int count, POINT mouse, int width, int height);
// The same for this PC's monitors and the current mouse position.
POINT startup_window_origin(int width, int height);
// DD-099: the narrowest client width at which a fixed dialog with this style shows its whole title in the caption
// (icon, title in the system caption font, close button). The caption font does not follow the Application scale, so
// at 50 % a dialog sized by the scale cut its title off with "...".
int caption_fit_client_width(const std::wstring& title, DWORD style, DWORD exStyle);
// Where a width x height window owned by `owner` opens: centred over the owner on the owner's monitor (centred on the
// primary work area when there is no owner).
POINT centre_on_owner(HWND owner, int width, int height);

// Brings the standard resize/caption behaviour of a borderless window to a child:
// returns HTTRANSPARENT from WM_NCHITTEST when the point lies in the parent's edge band.
bool install_edge_passthrough(HWND child, int edgeBandPixels);
void set_edge_band(int pixels);   // the current band width, read by every installed subclass
int edge_band();

} // namespace ui
} // namespace midibn
