#include "lane_grid.h"

#include <algorithm>

namespace midibn {
namespace ui {

namespace {

const UINT kReselectMessage = WM_APP + 77;

const wchar_t* const kHeadings[5] = { L"MIDI track / channel", L"Patch", L"Events / notes", L"Destination", L"Status" };
const int kWidths[4] = { 155, 155, 88, 82 };   // v1 column widths; the Status column fills the rest
const int kStatusMinimum = 80;   // v1 fills the last column with the remaining width

COLORREF row_colour(int style)
{
    switch (style) {
    case 1: return RGB(255, 228, 225);   // MistyRose
    case 2: return RGB(240, 128, 128);   // LightCoral
    case 3: return RGB(255, 250, 205);   // LemonChiffon
    default: return CLR_NONE;
    }
}

} // namespace

LaneGrid::~LaneGrid()
{
    if (row_height_images_) ImageList_Destroy(row_height_images_);
}

bool LaneGrid::create(HWND parent, int id, UiContext* ctx, HWND tooltipWindow)
{
    ctx_ = ctx;
    tooltip_ = tooltipWindow;
    window_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"MIDI track and channel routing preview",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0, 0, 100, 100,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!window_) return false;
    ListView_SetExtendedListViewStyle(window_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    SetWindowSubclass(window_, subclass_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    rebuild_columns();
    apply_scale();
    return true;
}

void LaneGrid::rebuild_columns()
{
    while (ListView_DeleteColumn(window_, 0)) {}
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(kHeadings[i]);
        column.cx = i < 4 ? ctx_->px(kWidths[i]) : ctx_->px(kStatusMinimum);
        column.iSubItem = i;
        ListView_InsertColumn(window_, i, &column);
    }
}

void LaneGrid::apply_scale()
{
    if (!window_) return;
    SendMessageW(window_, WM_SETFONT, reinterpret_cast<WPARAM>(ctx_->ui_font()), TRUE);
    // The small-image list only sets the row height (a DataGridView row is taller than the default list row).
    int rowHeight = ctx_->font_height() + ctx_->px(7);
    if (row_height_images_) ImageList_Destroy(row_height_images_);
    row_height_images_ = ImageList_Create(1, rowHeight, ILC_COLOR, 1, 1);
    ListView_SetImageList(window_, row_height_images_, LVSIL_SMALL);
    for (int i = 0; i < 4; ++i) ListView_SetColumnWidth(window_, i, ctx_->px(kWidths[i]));
    fit_columns();
}

void LaneGrid::fit_columns()
{
    if (!window_) return;
    RECT client;
    GetClientRect(window_, &client);
    int used = 0;
    for (int i = 0; i < 4; ++i) used += ListView_GetColumnWidth(window_, i);
    // P10-UI2: the vertical scroll bar may not exist yet when this runs (the list shows it on its next paint); leave
    // room for it when the rows do not fit, or the header ends up wider than the list.
    // (computed from the window's own width, not from its client rectangle, which may still be the old one)
    RECT outer;
    GetWindowRect(window_, &outer);
    int available = (outer.right - outer.left) - 2 * GetSystemMetrics(SM_CXEDGE);
    int count = ListView_GetItemCount(window_);
    RECT first = {};
    HWND header = ListView_GetHeader(window_);
    RECT headerRect = {};
    if (header) GetWindowRect(header, &headerRect);
    if (count > 0 && ListView_GetItemRect(window_, 0, &first, LVIR_BOUNDS) && first.bottom > first.top) {
        int body = (outer.bottom - outer.top) - 2 * GetSystemMetrics(SM_CYEDGE) - (headerRect.bottom - headerRect.top);
        if (count * (first.bottom - first.top) > body) available -= GetSystemMetrics(SM_CXVSCROLL);
    }
    if (available <= 0) available = client.right;
    int width = std::max(ctx_->px(kStatusMinimum), available - used);
    ListView_SetColumnWidth(window_, 4, width);
    if (header) {   // the list keeps the header at the width it had before the scroll bar appeared: trim it to the client area
        RECT c2, h2;
        GetClientRect(window_, &c2);
        GetClientRect(header, &h2);
        if (h2.right > c2.right) SetWindowPos(header, nullptr, 0, 0, c2.right, h2.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void LaneGrid::set_rows(std::vector<LaneGridRow> rows, int selected)
{
    programmatic_ = true;
    rows_ = std::move(rows);
    ListView_SetItemState(window_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(window_, static_cast<int>(rows_.size()), LVSICF_NOSCROLL);
    InvalidateRect(window_, nullptr, TRUE);
    programmatic_ = false;
    reselect_ = -1;
    hot_item_ = hot_sub_ = -1;
    select(selected);
    fit_columns();
}

void LaneGrid::select(int row)
{
    programmatic_ = true;
    ListView_SetItemState(window_, -1, 0, LVIS_SELECTED);
    if (row >= 0 && row < static_cast<int>(rows_.size())) {
        ListView_SetItemState(window_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(window_, row, FALSE);
    }
    programmatic_ = false;
}

int LaneGrid::selected() const
{
    return window_ ? ListView_GetNextItem(window_, -1, LVNI_SELECTED) : -1;
}

const LaneGridRow* LaneGrid::row(int index) const
{
    return index >= 0 && index < static_cast<int>(rows_.size()) ? &rows_[static_cast<size_t>(index)] : nullptr;
}

bool LaneGrid::handle_notify(NMHDR* header, LRESULT* result)
{
    if (header->hwndFrom != window_) return false;
    switch (header->code) {
    case LVN_GETDISPINFOW: {
        NMLVDISPINFOW* info = reinterpret_cast<NMLVDISPINFOW*>(header);
        if (info->item.mask & LVIF_TEXT) {
            static const wchar_t empty[] = L"";
            const LaneGridRow* r = row(info->item.iItem);
            info->item.pszText = const_cast<wchar_t*>(r && info->item.iSubItem >= 0 && info->item.iSubItem < 5
                    ? r->cells[info->item.iSubItem].c_str() : empty);
        }
        if (info->item.mask & LVIF_IMAGE) info->item.iImage = I_IMAGENONE;
        *result = 0;
        return true;
    }
    case LVN_ITEMCHANGED: {
        NMLISTVIEW* change = reinterpret_cast<NMLISTVIEW*>(header);
        if (programmatic_ || !(change->uChanged & LVIF_STATE) || change->iItem < 0) { *result = 0; return true; }
        bool nowSelected = (change->uNewState & LVIS_SELECTED) != 0;
        bool wasSelected = (change->uOldState & LVIS_SELECTED) != 0;
        if (nowSelected && !wasSelected) {
            reselect_ = -1;
            if (on_select_) on_select_(change->iItem);
        }
        else if (wasSelected && !nowSelected) {
            // v1's grid always keeps a selected row: put the selection back if nothing replaces it.
            reselect_ = change->iItem;
            PostMessageW(window_, kReselectMessage, 0, 0);
        }
        *result = 0;
        return true;
    }
    case NM_CUSTOMDRAW: {
        NMLVCUSTOMDRAW* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
        switch (draw->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            *result = CDRF_NOTIFYITEMDRAW;
            return true;
        case CDDS_ITEMPREPAINT:
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            const LaneGridRow* r = row(static_cast<int>(draw->nmcd.dwItemSpec));
            COLORREF colour = r ? row_colour(r->style) : CLR_NONE;
            bool isSelected = ListView_GetItemState(window_, static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED) != 0;
            if (colour != CLR_NONE && !isSelected) {
                draw->clrTextBk = colour;
                draw->clrText = RGB(0, 0, 0);
            }
            *result = draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT ? (CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT) : CDRF_NEWFONT;
            return true;
        }
        default:
            *result = CDRF_DODEFAULT;
            return true;
        }
    }
    default:
        break;
    }
    return false;
}

bool LaneGrid::hit(POINT point, int& item, int& subItem) const
{
    LVHITTESTINFO info = {};
    info.pt = point;
    int found = ListView_SubItemHitTest(window_, &info);
    if (found < 0 || !(info.flags & LVHT_ONITEM)) return false;
    item = info.iItem;
    subItem = info.iSubItem;
    return true;
}

std::wstring LaneGrid::hover_text() const
{
    POINT point;
    GetCursorPos(&point);
    ScreenToClient(window_, &point);
    int item, sub;
    if (!hit(point, item, sub)) return std::wstring();
    const LaneGridRow* r = row(item);
    if (!r) return std::wstring();
    if (sub == 1) return r->patch_tip;
    if (sub >= 2 && sub < 5) {
        // A cell whose text does not fit shows it in full.
        const std::wstring& text = r->cells[sub];
        int needed = ListView_GetStringWidth(window_, text.c_str()) + ctx_->px(14);
        if (needed > ListView_GetColumnWidth(window_, sub)) return text;
    }
    if (sub == 0) {
        const std::wstring& text = r->cells[0];
        int needed = ListView_GetStringWidth(window_, text.c_str()) + ctx_->px(14);
        if (needed > ListView_GetColumnWidth(window_, 0)) return text;
    }
    return std::wstring();
}

LRESULT CALLBACK LaneGrid::subclass_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    LaneGrid* grid = reinterpret_cast<LaneGrid*>(data);
    switch (message) {
    case WM_MOUSEMOVE: {
        POINT point = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        int item = -1, sub = -1;
        grid->hit(point, item, sub);
        if (item != grid->hot_item_ || sub != grid->hot_sub_) {
            grid->hot_item_ = item;
            grid->hot_sub_ = sub;
            if (grid->tooltip_) SendMessageW(grid->tooltip_, TTM_POP, 0, 0);   // ask for the new cell's text
        }
        break;
    }
    case WM_SIZE:
        break;
    case kReselectMessage:
        if (grid->reselect_ >= 0 && grid->selected() < 0) grid->select(grid->reselect_);
        grid->reselect_ = -1;
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, subclass_proc, id);
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace ui
} // namespace midibn
