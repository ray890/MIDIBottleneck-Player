// The Channels window's in-place editor, without any drawing: what v1's
// ChannelMonitorForm.ActivateEditor / EditorValueRequested / EditorAutoRequested /
// EditorChaseRequested and the channel-field part of ScrubOrTypeTextBox do
// (pointer gesture, typing, Auto and "restore source value" on right-click).
//
// The frontend owns the edit control, the pointer capture, the cursor hiding and
// warping, and the tooltips; this class owns the state and says what to do.
// Values are sent through a `ChannelRequestSink`.
#pragma once

#include "midibn/app/channel_grid.h"
#include "midibn/app/ui_mappings.h"

#include <string>

namespace midibn {
namespace app {

// Where the editor and the grid send their requests. Each returns an error message,
// empty on success (v1: the exception's Message).
class ChannelRequestSink {
public:
    virtual ~ChannelRequestSink() {}
    // value == ChannelOverrideState::kAutoValue (INT32_MIN) releases the forced value.
    virtual std::string request_override(int channel, ChannelAttribute attribute, int value) = 0;
    // One source-derived value through the ordered output path (the result also arrives later as feedback).
    virtual std::string request_historical_chase(int channel, ChannelAttribute attribute) = 0;
    virtual std::string request_enabled(int channel, bool enabled) = 0;
};

// v1 ActivateEditor's Configure call.
struct ChannelEditorSpec {
    int channel = -1;
    ChannelAttribute attribute = ChannelAttribute::BankMsb;
    int value = 0;
    int minimum = 0, maximum = 127;
    int display_offset = 0;                     // Program shows 1..128
    int coarse_units = 1, coarse_pixels = 8;    // drag: one step per eight pixels (pitch bend: 16 per pixel)
    int fine_units = 1, fine_pixels = 16;       // Shift: one per sixteen (pitch bend: one per pixel)
    bool forced = false;
    bool historical = false;
    std::string help;
};
ChannelEditorSpec channel_editor_spec(const ChannelGridModel& model, int channel, ChannelAttribute attribute);

class ChannelEditor {
public:
    ChannelEditor(const ChannelGridModel& model, ChannelRequestSink& sink, const format::NumberCulture& culture);

    // ---- show / hide (v1 ActivateEditor, HideEditor)
    void open(int channel, ChannelAttribute attribute);
    bool is_open() const { return open_; }
    int channel() const { return spec_.channel; }
    ChannelAttribute attribute() const { return spec_.attribute; }
    const ChannelEditorSpec& spec() const { return spec_; }

    // What happened to a typed value or a closing editor: a tooltip to show for 2.5 s, if any.
    struct Outcome {
        bool requested = false;     // a value request was sent
        std::string error;          // empty: none; else the tooltip text (v1 _lastError)
    };
    // Hides the editor; typed text is applied first when `commitTyped`. The outcome's error is
    // not shown by v1 (the control is hidden).
    Outcome close(bool commitTyped, const std::string& typedText = std::string());

    // ---- appearance
    // The editor's text is bold and blue: set when the editor opens on a forced value and after every
    // value request (v1 EditorValueRequested), and kept until the next open.
    bool bold() const { return bold_; }
    // The field's own forced flag (cleared by a successful release to Auto).
    bool forced() const { return field_.forced(); }
    int value() const { return field_.value(); }
    // The text of the flat (not typing) field: "1 — Acoustic Grand Piano", "+5", "On", ...
    std::string flat_text() const { return field_.flat_text(culture_); }
    // The control's hover tooltip: the attribute help, or the last error (v1 SetToolTip(_lastError)).
    const std::string& tooltip() const { return tooltip_; }
    const std::string& last_error() const { return last_error_; }
    bool typing() const { return field_.typing(); }
    bool armed() const { return armed_; }
    bool scrubbing() const { return scrubbing_; }

    // ---- pointer (screen coordinates; the frontend reads the pointer)
    // Left button went down on the field: arm a gesture.
    void pointer_down(int screenX, int screenY);
    struct PointerResult {
        bool scrub_started = false;   // hide the cursor and capture the pointer
        bool warp = false;            // move the pointer back to (warpX, warpY)
        int warp_x = 0, warp_y = 0;
        bool requested = false;       // a value request was sent
        std::string error;            // the request failed: the field's tooltip (v1 "Could not apply: ...")
        bool gesture_ended = false;   // the left button is no longer down
    };
    PointerResult pointer_move(int screenX, int screenY, bool fine, bool leftDown);
    // Left button released. True: typing started (the frontend focuses the edit control with typing_text()).
    bool pointer_up();
    // The text the edit control starts with after begin_typing() / pointer_up().
    const std::string& typing_text() const { return typing_text_; }
    // The pointer capture was lost while scrubbing (v1 OnMouseCaptureChanged) or focus left.
    void end_gesture();
    // Scrub by a number of pixels without a pointer (v1 ApplyScrubDelta; used by tests and keyboard-free hosts).
    PointerResult scrub_by(int pixels, bool fine);

    // ---- typing
    std::string begin_typing();                       // F2 / click without drag; the text to edit
    Outcome commit_typed(const std::string& text, bool explicitCommit);   // Enter (explicit) or focus loss
    void abandon_typing();                            // Escape
    std::string adjust_typed(const std::string& text, int direction);   // Up / Down while typing

    // ---- right click on the field (v1 BeginPointerGesture(Right))
    struct RightClickResult {
        bool handled = false;
        bool close = false;      // the request succeeded: hide the editor
        std::string error;       // tooltip text ("Could not return to Auto: ...")
    };
    RightClickResult right_click();

private:
    Outcome apply_request(int value);
    void finish_gesture();

    const ChannelGridModel& model_;
    ChannelRequestSink& sink_;
    format::NumberCulture culture_;
    ScrubField field_;
    ChannelEditorSpec spec_;
    bool open_ = false;
    bool armed_ = false;
    bool scrubbing_ = false;
    bool ignore_warp_ = false;
    bool bold_ = false;
    std::string typing_text_;
    int start_x_ = 0, anchor_x_ = 0, anchor_y_ = 0;
    std::string tooltip_;
    std::string last_error_;
};

} // namespace app
} // namespace midibn
