#include "gtk_util.h"

#include "midibn/app/ui_mappings.h"

#include <cstdio>

namespace midibn {
namespace gtkui {

ScaleStyle& ScaleStyle::instance()
{
    static ScaleStyle style;
    return style;
}

ScaleStyle::ScaleStyle()
{
    provider_ = gtk_css_provider_new();
    GdkScreen* screen = gdk_screen_get_default();
    if (screen) gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider_), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    set_percent(100);
}

int ScaleStyle::px(int canonical) const { return app::scale_metric(canonical, percent_); }

void ScaleStyle::set_percent(int percent)
{
    percent_ = app::clamp_ui_scale_percent(percent);
    const app::UiFontChoice choice = app::ui_font_choice(10.0f, percent_);
    char css[2048];
    std::snprintf(css, sizeof(css),
        "* { font-size: %.2fpt; }\n"
        "button { padding: %dpx %dpx; min-height: 0; min-width: 0; }\n"
        "entry { padding: %dpx %dpx; min-height: 0; }\n"
        "combobox button.combo { padding: %dpx %dpx; }\n"
        ".mono { font-family: monospace; }\n"
        ".mono-bold { font-family: monospace; font-weight: bold; }\n"
        "frame > label { font-weight: normal; }\n"
        "frame > border { padding: %dpx; }\n"
        "scale trough { min-height: %dpx; }\n"
        "progressbar trough, progressbar progress { min-height: %dpx; }\n"
        ".help-view { padding: %dpx; }\n",
        static_cast<double>(choice.points), px(3), px(10), px(2), px(4), px(2), px(4), px(3), px(5), px(8), px(10));
    gtk_css_provider_load_from_data(provider_, css, -1, nullptr);
}

}   // namespace gtkui
}   // namespace midibn
