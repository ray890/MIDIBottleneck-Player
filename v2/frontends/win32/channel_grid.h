// The Channels window's grid (plain Win32 + GDI): 16 rows below a heading row, owner-drawn cells
// (blue bold forced values, gray italic historical values), the in-place scrub-or-type editor over
// the clicked cell, per-cell tooltips, transient feedback tips, and the "detached" message.
//
// Behaviour comes from app::ChannelSession (v2/core/include/midibn/app/channel_session.h); this class
// only draws and turns pointer and keyboard input into calls on the session and its editor.
#pragma once

#include "ui_common.h"

#include "midibn/app/channel_session.h"

#include <string>
#include <vector>

namespace midibn {
namespace ui {

// Posted to the grid's parent when the column widths (or the content size) changed.
const UINT WM_MIDIBN_GRIDLAYOUT = WM_APP + 30;
// Posted to the grid's parent after a click was handled (a request may have been sent; feedback may follow).
const UINT WM_MIDIBN_GRIDACTION = WM_APP + 31;

class ChannelGrid {
public:
    ChannelGrid(UiContext* ctx, app::ChannelSession* session);
    ~ChannelGrid();
    ChannelGrid(const ChannelGrid&) = delete;
    ChannelGrid& operator=(const ChannelGrid&) = delete;

    HWND create(HWND parent);
    HWND hwnd() const { return window_; }

    // Application scale (or DPI) changed: fonts, heights, column widths.
    void on_scale_changed();
    // Repaint what the session says changed; widens columns whose text no longer fits.
    void apply_changes(const app::ChannelGridChanges& changes);
    void invalidate_all();
    void set_detached(bool detached, const std::string& message);
    bool detached() const { return detached_; }
    void show_feedback(const app::ChannelFeedback& feedback);

    // The columns take the width of their headings (v1 ApplyHeaderPreferredColumnWidths) and then only
    // grow to fit their values. Columns the user resized keep that width (scaled with the Application scale).
    void apply_header_preferred_widths();
    int content_width() const;     // all columns, in device pixels
    int content_height() const;    // heading row and the 16 rows
    int client_width() const;
    int client_height() const;
    int scrollbar_width() const { return GetSystemMetrics(SM_CXVSCROLL); }
    int scrollbar_height() const { return GetSystemMetrics(SM_CYHSCROLL); }

    // The editor.
    void hide_editor(bool commitTyped);
    bool editor_visible() const;
    HWND editor_window() const { return editor_; }

    // Keyboard (DD-031). The current cell is the one the arrow keys move; it is drawn with a focus rectangle
    // while the grid has the keyboard focus. `key_down` handles one key press (a Windows virtual-key code;
    // `ctrl` is whether Control is held) and returns true when the grid used the key:
    //   arrows, Home, End, Page Up, Page Down (Ctrl+Home / Ctrl+End: first / last cell)  move the current cell
    //   Enter, F2    start editing an attribute cell (typing), or enable/disable the channel on the Ch cell
    //   Space        enable/disable the channel of the current row (like clicking its Ch cell)
    //   Delete       like right-click: release a forced value to Auto, or restore the latest source value
    //   Escape       closes the editor when it is open (typing is cancelled by the editor itself)
    bool key_down(UINT virtualKey, bool ctrl);
    int current_row() const { return current_row_; }
    int current_column() const { return current_column_; }
    bool has_keyboard_focus() const { return has_focus_; }
    // DD-087 (user decision, prompt 009): the dotted rectangle on the current cell appears only after the first key
    // press in the grid (or its edit box); a mouse-only user never sees it. There is no filled highlight.
    bool focus_cue_visible() const { return has_focus_ && keyboard_used_; }
    // Makes (row, column) the current cell and scrolls it into view.
    void set_current_cell(int row, int column);

    // Geometry for tests: the cell's rectangle in client coordinates (row -1 is the heading row).
    RECT cell_rect(int row, int column) const;
    int column_width(int column) const { return widths_[static_cast<size_t>(column)]; }
    // Renders the client area with the window's own paint code into a 32-bit DIB (never the screen).
    bool render(HDC target, int width, int height);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editor_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR self);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    bool handle_editor_message(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

    void make_fonts();
    void measure();
    void paint(HDC dc, const RECT& clip);
    void paint_cell(HDC dc, int row, int column, const RECT& rect);
    void draw_detached(HDC dc);
    int header_preferred(int column) const;
    int text_width(HFONT font, const std::string& text) const;
    int column_left(int column) const;
    bool hit_test(int x, int y, int& row, int& column) const;
    int border_hit(int x, int y) const;                 // column whose right border is near (x, y), else -1
    void update_scrollbars();
    void scroll_to(int xOffset, int yOffset);
    void layout_tooltips();
    void notify_layout();
    double scale_factor() const;
    void invalidate_cell(int row, int column);
    void ensure_visible(int row, int column);
    void mark_current(int row, int column);
    int visible_body_rows() const;
    // The shared part of a click and of the keyboard actions on a cell: sends the request, repaints, shows
    // feedback; `keyboard` starts typing at once when an editor opened (a click without movement does the same).
    void cell_action(int row, int column, bool leftButton, int screenX, int screenY, bool keyboard);
    void return_focus_to_grid();

    // pointer gestures (editor)
    void show_editor(int row, int column);
    void apply_editor_look();
    void gesture_move(int screenX, int screenY);
    void gesture_up();
    void gesture_cancel();
    void finish_cursor();
    void start_typing(const std::string& text);
    void editor_commit(bool explicitCommit);
    void editor_error_tip(const std::string& text);
    void show_tip_at(const std::wstring& text, int screenX, int screenY, int milliseconds);
    void hide_transient_tip();
    void request_notify();

    UiContext* ctx_;
    app::ChannelSession* session_;
    HWND window_ = nullptr;
    HWND editor_ = nullptr;
    HWND tips_ = nullptr;
    HWND transient_tip_ = nullptr;
    HFONT italic_ = nullptr;
    int row_height_ = 22;
    int header_height_ = 23;
    int widths_[app::kChannelColumnCount];
    double user_canonical_[app::kChannelColumnCount];   // user-set width in canonical pixels, 0 = automatic
    int auto_widths_[app::kChannelColumnCount];
    int x_offset_ = 0, y_offset_ = 0;
    bool detached_ = false;
    std::string detached_message_;
    int hot_row_ = -2, hot_column_ = -2;
    int current_row_ = 0, current_column_ = 0;
    bool has_focus_ = false;
    bool keyboard_used_ = false;   // a key was pressed in the grid or its edit box (shows the focus rectangle)
    void note_key_press();
    bool resizing_ = false;
    int resize_column_ = -1, resize_start_x_ = 0, resize_start_width_ = 0;
    bool gesture_capture_ = false;
    bool cursor_hidden_ = false;
    bool editor_typing_ = false;
    std::wstring tip_text_;       // storage for the tooltip callbacks
    bool layout_notified_ = false;
};

} // namespace ui
} // namespace midibn
