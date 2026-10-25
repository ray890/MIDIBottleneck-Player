// The Track routing editor's lanes grid (v1: a read-only DataGridView with five columns):
// a virtual report-view list (SysListView32, owner data), so thousands of lanes cost
// nothing. Row colours (unresolved, omitted, warning) are drawn through custom draw; the
// patch cell and truncated status cells have hover tooltips.
#pragma once

#include "ui_common.h"

#include <commctrl.h>

#include <functional>
#include <string>
#include <vector>

namespace midibn {
namespace ui {

struct LaneGridRow {
    std::wstring cells[5];     // MIDI track / channel, Patch, Events / notes, Destination, Status
    int style = 0;             // 0 normal, 1 unresolved (MistyRose), 2 omitted (LightCoral), 3 warning (LemonChiffon)
    std::wstring patch_tip;    // tooltip of the Patch cell
};

class LaneGrid {
public:
    LaneGrid() {}
    ~LaneGrid();
    LaneGrid(const LaneGrid&) = delete;
    LaneGrid& operator=(const LaneGrid&) = delete;

    bool create(HWND parent, int id, UiContext* ctx, HWND tooltipWindow);
    HWND hwnd() const { return window_; }

    // Replaces all rows; `selected` (or none for -1) is selected without raising the callback.
    void set_rows(std::vector<LaneGridRow> rows, int selected);
    void select(int row);                     // programmatic: no callback
    int selected() const;                     // -1 when none
    int row_count() const { return static_cast<int>(rows_.size()); }
    const LaneGridRow* row(int index) const;
    // The user selected a row (mouse or keyboard).
    void set_selection_callback(std::function<void(int)> callback) { on_select_ = std::move(callback); }

    // Fonts, row height and column widths for the context's current scale.
    void apply_scale();
    // The last column takes the width the others leave (called after a resize).
    void fit_columns();

    // WM_NOTIFY from the parent for this grid's control id; true when handled.
    bool handle_notify(NMHDR* header, LRESULT* result);
    // The tooltip text for the cell under the mouse (empty for none).
    std::wstring hover_text() const;

    static int column_count() { return 5; }

private:
    static LRESULT CALLBACK subclass_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    bool hit(POINT clientPoint, int& item, int& subItem) const;
    void rebuild_columns();

    HWND window_ = nullptr;
    HWND tooltip_ = nullptr;
    UiContext* ctx_ = nullptr;
    std::vector<LaneGridRow> rows_;
    std::function<void(int)> on_select_;
    bool programmatic_ = false;
    int reselect_ = -1;
    int hot_item_ = -1, hot_sub_ = -1;
    HIMAGELIST row_height_images_ = nullptr;
};

} // namespace ui
} // namespace midibn
