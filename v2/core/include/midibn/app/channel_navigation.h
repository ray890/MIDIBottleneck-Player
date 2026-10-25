// Keyboard navigation of the Channels window's grid, without any drawing (new in v2; v1's grid offered no
// keyboard use of its cells, see v2/docs/DIFFERENCES_FROM_V1.md DD-031). The frontend turns Windows key
// codes into NavKey values; this function says which cell becomes the current one.
#pragma once

#include "midibn/app/channel_grid.h"

namespace midibn {
namespace app {

struct ChannelCellPosition {
    int row = 0;        // 0..15 (the heading row is never the current cell)
    int column = 0;     // 0..kChannelColumnCount-1
    bool operator==(const ChannelCellPosition& other) const { return row == other.row && column == other.column; }
    bool operator!=(const ChannelCellPosition& other) const { return !(*this == other); }
};

enum class ChannelNavKey { Left, Right, Up, Down, Home, End, CtrlHome, CtrlEnd, PageUp, PageDown };

// The cell reached from `from` by `key`. The movement stops at the edges (it does not wrap). `pageRows` is the
// number of body rows visible at once (Page Up / Page Down move that many rows, at least one). Home and End go
// to the first and last column of the row, Ctrl+Home and Ctrl+End to the first and last cell of the grid.
// A position outside the grid is first brought inside it.
inline ChannelCellPosition channel_navigate(ChannelCellPosition from, ChannelNavKey key, int pageRows)
{
    const int lastRow = 15, lastColumn = kChannelColumnCount - 1;
    if (from.row < 0) from.row = 0;
    if (from.row > lastRow) from.row = lastRow;
    if (from.column < 0) from.column = 0;
    if (from.column > lastColumn) from.column = lastColumn;
    int page = pageRows < 1 ? 1 : pageRows;
    ChannelCellPosition to = from;
    switch (key) {
    case ChannelNavKey::Left: if (to.column > 0) --to.column; break;
    case ChannelNavKey::Right: if (to.column < lastColumn) ++to.column; break;
    case ChannelNavKey::Up: if (to.row > 0) --to.row; break;
    case ChannelNavKey::Down: if (to.row < lastRow) ++to.row; break;
    case ChannelNavKey::Home: to.column = 0; break;
    case ChannelNavKey::End: to.column = lastColumn; break;
    case ChannelNavKey::CtrlHome: to.row = 0; to.column = 0; break;
    case ChannelNavKey::CtrlEnd: to.row = lastRow; to.column = lastColumn; break;
    case ChannelNavKey::PageUp: to.row = to.row - page < 0 ? 0 : to.row - page; break;
    case ChannelNavKey::PageDown: to.row = to.row + page > lastRow ? lastRow : to.row + page; break;
    }
    return to;
}

} // namespace app
} // namespace midibn
