#include "glue.h"

#include "ui_common.h"

#include <windows.h>

namespace midibn {
namespace ui {

format::NumberCulture system_number_culture()
{
    format::NumberCulture culture;
    wchar_t text[16];
    if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, text, 16) > 0) culture.decimal_separator = U(text);
    if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, text, 16) > 0) culture.group_separator = U(text);
    return culture;
}

} // namespace ui
} // namespace midibn
