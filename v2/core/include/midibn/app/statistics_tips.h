// The hover text of the eight statistics cells (portable: the Win32 and GTK windows show the same words).
#pragma once

#include "midibn/app/player_controller.h"

#include <string>

namespace midibn {
namespace app {

// Cells are numbered as in StatisticsText::values (0 timeline/output ... 7 current lag). Cells without a text give "".
std::string statistics_cell_tooltip(const StatisticsText& text, int cell);

} // namespace app
} // namespace midibn
