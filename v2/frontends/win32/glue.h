// Operating-system glue for the frontend: locale. (The remembered settings have their own file, settings_file.h; the
// program never touches the registry.)
#pragma once

#include "midibn/format/net_format.h"

#include <string>

namespace midibn {
namespace ui {

// The user's decimal and thousands separators.
format::NumberCulture system_number_culture();

} // namespace ui
} // namespace midibn
