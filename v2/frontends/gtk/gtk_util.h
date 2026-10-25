// Small helpers shared by the GTK windows: signal connection with C++ callables, the Application scale (a CSS provider),
// and a few text conversions.
#pragma once

#include <gtk/gtk.h>

#include "midibn/app/error_text.h"

#include <functional>
#include <string>

namespace midibn {
namespace gtkui {

inline const char* const kProductName = "MIDIBottleneck Player";

namespace detail {
template <class F>
struct Holder {
    F fn;
};
template <class F>
void destroy_holder(gpointer data, GClosure*)
{
    delete static_cast<Holder<F>*>(data);
}
}   // namespace detail

// connect(widget, "clicked", [=] { ... });          signals with no arguments besides the instance
template <class F>
gulong connect(gpointer instance, const char* signal, F fn)
{
    auto* holder = new detail::Holder<F>{ std::move(fn) };
    auto trampoline = +[](gpointer, gpointer data) { static_cast<detail::Holder<F>*>(data)->fn(); };
    return g_signal_connect_data(instance, signal, G_CALLBACK(trampoline), holder, detail::destroy_holder<F>, static_cast<GConnectFlags>(0));
}

// Signals with one pointer argument (for example "row-selected"): fn() is called, the argument is ignored.
template <class F>
gulong connect_arg(gpointer instance, const char* signal, F fn)
{
    auto* holder = new detail::Holder<F>{ std::move(fn) };
    auto trampoline = +[](gpointer, gpointer, gpointer data) { static_cast<detail::Holder<F>*>(data)->fn(); };
    return g_signal_connect_data(instance, signal, G_CALLBACK(trampoline), holder, detail::destroy_holder<F>, static_cast<GConnectFlags>(0));
}

// Signals whose handler takes an event and returns gboolean: fn(GdkEvent*) -> bool (true = handled).
template <class F>
gulong connect_event(gpointer instance, const char* signal, F fn)
{
    auto* holder = new detail::Holder<F>{ std::move(fn) };
    auto trampoline = +[](GtkWidget*, GdkEvent* event, gpointer data) -> gboolean {
        return static_cast<detail::Holder<F>*>(data)->fn(event) ? TRUE : FALSE;
    };
    return g_signal_connect_data(instance, signal, G_CALLBACK(trampoline), holder, detail::destroy_holder<F>, static_cast<GConnectFlags>(0));
}

// "delete-event": fn() -> bool (true = keep the window open).
template <class F>
gulong connect_delete(gpointer instance, F fn)
{
    auto* holder = new detail::Holder<F>{ std::move(fn) };
    auto trampoline = +[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
        return static_cast<detail::Holder<F>*>(data)->fn() ? TRUE : FALSE;
    };
    return g_signal_connect_data(instance, "delete-event", G_CALLBACK(trampoline), holder, detail::destroy_holder<F>, static_cast<GConnectFlags>(0));
}

// "draw": fn(cairo_t*, GtkWidget*) -> void; the widget gets the default draw handler afterwards only if it is not a
// drawing area (this helper is for drawing areas).
template <class F>
gulong connect_draw(gpointer instance, F fn)
{
    auto* holder = new detail::Holder<F>{ std::move(fn) };
    auto trampoline = +[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
        static_cast<detail::Holder<F>*>(data)->fn(cr, widget);
        return FALSE;
    };
    return g_signal_connect_data(instance, "draw", G_CALLBACK(trampoline), holder, detail::destroy_holder<F>, static_cast<GConnectFlags>(0));
}

inline void set_text(GtkLabel* label, const std::string& text)
{
    const gchar* current = gtk_label_get_text(label);
    if (text != current) gtk_label_set_text(label, text.c_str());
}

inline GtkWidget* make_label(const char* text, float xalign = 0.0f)
{
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), xalign);
    return label;
}

inline void add_class(GtkWidget* widget, const char* name) { gtk_style_context_add_class(gtk_widget_get_style_context(widget), name); }

// The Application scale: one CSS provider for the whole program (all windows). Percent 50..200.
class ScaleStyle {
public:
    static ScaleStyle& instance();
    void set_percent(int percent);
    int percent() const { return percent_; }
    // Canonical pixels (96 dpi design) scaled by the Application scale, never 0 for non-zero input.
    int px(int canonical) const;

private:
    ScaleStyle();
    int percent_ = 100;
    GtkCssProvider* provider_ = nullptr;
};

// Sets the tooltip text of a widget (an empty text removes it).
inline void set_tooltip(GtkWidget* widget, const std::string& text)
{
    if (text.empty()) gtk_widget_set_tooltip_text(widget, nullptr);
    else gtk_widget_set_tooltip_text(widget, text.c_str());
}

// A message dialog for a failure text laid out as in the Windows player (error_text.h): the plain sentence is the
// dialog's main text, the technical details (if any) are its secondary text under the heading "Details:".
inline GtkWidget* new_message_dialog(GtkWindow* parent, GtkMessageType type, const std::string& text)
{
    std::string plain, details;
    app::split_error_text(text, plain, details);
    GtkWidget* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", plain.c_str());
    if (!details.empty())
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s\n%s", app::error_details_heading(), details.c_str());
    return dialog;
}

}   // namespace gtkui
}   // namespace midibn
