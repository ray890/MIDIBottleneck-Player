// The main window's custom controls (plain Win32 + GDI): the timeline, the
// statistics grid, a flat label, caption buttons, the scrub-or-type numeric
// field, the Rate model popup, and the shared tooltip host.
#pragma once

#include "ui_common.h"

#include "midibn/app/player_controller.h"

#include <functional>
#include <string>
#include <vector>

namespace midibn {
namespace ui {

void register_control_classes(HINSTANCE instance);

// ---------------------------------------------------------------- flat label
// Single- or two-line text (a line break splits the box in half like v1's
// BufferedStatusLabel), optional end ellipsis, no flicker. A label is transparent to the mouse (clicks reach the
// window below) unless it has LabelTooltip: such a label takes the mouse wherever its parent window would treat the
// point as ordinary client area, so its tooltip can appear (a borderless drag zone or resize edge stays the parent's).
// The whole label is repainted when its size changes (the text is aligned or shortened to the new width).
const wchar_t kLabelClass[] = L"MidiBnLabel";
enum LabelStyle { LabelLeft = 0, LabelRight = 1, LabelCenter = 2, LabelEllipsis = 4, LabelDim = 8, LabelTooltip = 16 };
HWND create_label(HWND parent, int id, int style, UiContext* ctx);

// ---------------------------------------------------------------- check boxes and radio buttons
// P10-UI1: Windows draws a check box's box (and a radio button's circle) at the system size whatever the font, so at
// 125-200 % the box looked small next to the text and at 50-75 % it was too large and cut the label off. This
// subclasses a BS_AUTOCHECKBOX / BS_AUTORADIOBUTTON (or BS_CHECKBOX / BS_RADIOBUTTON) button so it draws itself at the
// Application scale (box = 13 canonical pixels), in the theme's look when a visual style is active (else the classic
// look), with the usual disabled colour, hot / pressed states and keyboard focus rectangle. The button stays a real
// BUTTON control: clicks, Space, BM_GETCHECK / BM_SETCHECK, tab order and the accessible role are Windows' own.
void attach_scaled_button(HWND button, UiContext* ctx);

// ---------------------------------------------------------------- timeline
const wchar_t kTimelineClass[] = L"MidiBnTimeline";
HWND create_timeline(HWND parent, int id, UiContext* ctx);
struct TimelineApi {
    static void set_timeline(HWND window, int64_t position, int64_t duration);
    static bool dragging(HWND window);
    static void scale_changed(HWND window);
};

// ---------------------------------------------------------------- statistics grid
const wchar_t kStatsClass[] = L"MidiBnStatistics";
HWND create_statistics(HWND parent, int id, UiContext* ctx);
struct StatisticsApi {
    // cellMask: bit i set = the value of cell i is updated now (the others keep what is shown). The adaptive display
    // rate uses it to draw half of the statistics on one update and half on the next. Everything else always updates.
    static void set_values(HWND window, const app::StatisticsText& text, unsigned cellMask = 0xFF);
    static void set_compact(HWND window, bool compact);
    static void set_paint_all(bool all);   // test: paint every cell on every paint (the behaviour before the cells were skipped)
    static int preferred_height(HWND window);
    static void scale_changed(HWND window);
};

// ---------------------------------------------------------------- caption button
const wchar_t kCaptionButtonClass[] = L"MidiBnCaptionButton";
enum CaptionGlyph { GlyphMenu, GlyphMinimize, GlyphMaximize, GlyphRestore, GlyphClose };
HWND create_caption_button(HWND parent, int id, CaptionGlyph glyph, UiContext* ctx, const wchar_t* accessibleName);
void set_caption_glyph(HWND button, CaptionGlyph glyph);

// ---------------------------------------------------------------- scrub-or-type numeric field
// A read-only EDIT that scrubs on drag and types on click (v1 ScrubOrTypeTextBox, numeric mode).
HWND create_scrub_field(HWND parent, int id, UiContext* ctx, const format::NumberCulture* culture,
    std::function<int64_t(int64_t start, int64_t pixels)> mapper);
void scrub_field_apply(HWND field, const app::NumericFieldView& view);
bool scrub_field_active(HWND field);                 // typing or scrubbing
int scrub_field_preferred_width(HWND field);
void scrub_field_set_mapper(HWND field, std::function<int64_t(int64_t start, int64_t pixels)> mapper);
void scrub_field_finish(HWND field, bool commit);
// v1 ScrubOrTypeTextBox field palette: the face of a blank themed drop-down list (enabled or disabled), measured
// once from a hidden combo box; system colours when that cannot be measured. The parent answers the field's
// WM_CTLCOLOR* with these.
COLORREF scrub_field_back_color(bool enabled);
HBRUSH scrub_field_back_brush(bool enabled);

// ---------------------------------------------------------------- slider
// Native trackbar plus v1's click-to-position behaviour (ProcessingTrackBar).
HWND create_slider(HWND parent, int id, UiContext* ctx);

// ---------------------------------------------------------------- Rate model popup
// Installs the grouped popup on a native drop-down-list combo box.
void install_rate_popup(HWND combo, UiContext* ctx);
void rate_popup_set_forward_only(HWND combo, bool forward);
int rate_choice_to_index(app::RateModelChoice choice);
app::RateModelChoice rate_index_to_choice(int index);
// WM_MEASUREITEM / WM_DRAWITEM for the popup's owner-drawn menu items; true when handled.
bool rate_popup_measure(MEASUREITEMSTRUCT* item, UiContext* ctx);
bool rate_popup_draw(DRAWITEMSTRUCT* item, UiContext* ctx);

// ---------------------------------------------------------------- tooltips
class TooltipHost {
public:
    TooltipHost() {}
    ~TooltipHost();
    void create(HWND owner);
    // Text is requested lazily through TTN_GETDISPINFO (handled by handle_notify).
    void add_control(HWND control);
    void remove_control(HWND control);
    void set_maximum_width(int pixels);
    HWND window() const { return tip_; }
    // Returns true when `header` was a tooltip request; fills the text through `provider`.
    bool handle_notify(NMHDR* header, const std::function<std::wstring(HWND)>& provider);

private:
    HWND tip_ = nullptr;
    HWND owner_ = nullptr;
    std::wstring text_;
};

// A transient error tooltip under a control (v1 ToolTip.Show(text, control, 0, Height, 2500)).
void show_transient_tip(HWND control, const std::wstring& text, int milliseconds);

} // namespace ui
} // namespace midibn
