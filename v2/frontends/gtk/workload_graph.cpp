#include "workload_graph.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace midibn {
namespace gtkui {

namespace {

using app::GraphRect;

// v1's colours (WorkloadGraph.cs), as in the Windows window.
struct Rgb {
    int r, g, b;
};
const Rgb kBackground = { 24, 27, 32 };
const Rgb kForeground = { 220, 220, 220 };
const Rgb kSilver = { 192, 192, 192 };
const Rgb kGrid = { 65, 75, 86 };
const Rgb kEventsPlot = { 63, 190, 255 };
const Rgb kBytesPlot = { 255, 174, 66 };
const Rgb kGatePlot = { 255, 190, 92 };
const Rgb kCapacity = { 110, 220, 130 };
const Rgb kOverflow = { 235, 65, 65 };
const Rgb kTickPen = { 155, 165, 175 };
const Rgb kCluster = { 195, 90, 220 };
const Rgb kPressure[4] = { { 75, 150, 95 }, { 230, 170, 45 }, { 230, 65, 65 }, { 155, 75, 190 } };
const int kPressureAlpha[4] = { 100, 150, 190, 210 };
const Rgb kPlaybackMarker = { 205, 225, 255 };
const Rgb kOutputMarker = { 130, 230, 185 };
const Rgb kOverlayBackground = { 12, 15, 19 };
const Rgb kOverlayOutline = { 190, 195, 202 };
const Rgb kWhiteSmoke = { 245, 245, 245 };
const Rgb kHover = { 245, 215, 100 };
const Rgb kTipBackground = { 40, 44, 50 };
const Rgb kWhite = { 255, 255, 255 };

void set_source(cairo_t* cr, const Rgb& c, int alpha = 255)
{
    cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0, alpha / 255.0);
}

enum Align { Left, Center, Right };

}   // namespace

struct WorkloadGraphWidget::Impl {
    app::AnalysisWindowModel* model = nullptr;
    std::function<void()> changed;
    GtkWidget* area = nullptr;
    PangoFontDescription* font = nullptr;
    PangoLayout* measure_layout = nullptr;
    cairo_surface_t* statics = nullptr;
    int static_w = 0, static_h = 0;
    uint64_t static_revision = 0;
    guint tick_id = 0;
    bool detached = false;
    bool skip_release = false;
    guint32 last_press_time = 0;
    double last_press_x = 0, last_press_y = 0;

    void make_font()
    {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        const PangoFontDescription* base = gtk_style_context_get_font(gtk_widget_get_style_context(area), GTK_STATE_FLAG_NORMAL);
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (font) pango_font_description_free(font);
        font = pango_font_description_copy(base);
        // v1: the 9 pt UI font scaled to 8 pt.
        const int size = pango_font_description_get_size(font);
        pango_font_description_set_size(font, std::max(1, static_cast<int>(std::lround(size * 8.0 / 9.0))));
        if (measure_layout) g_object_unref(measure_layout);
        measure_layout = gtk_widget_create_pango_layout(area, "");
        pango_layout_set_font_description(measure_layout, font);
    }

    void install_hooks()
    {
        app::WorkloadGraphModel& graph = model->graph();
        graph.set_text_measure([this](const std::string& text, bool) {
            app::GraphSize size;
            if (detached || !measure_layout) { size.width = static_cast<int>(text.size()) * 6; size.height = 13; return size; }
            pango_layout_set_text(measure_layout, text.c_str(), -1);
            pango_layout_get_pixel_size(measure_layout, &size.width, &size.height);
            return size;
        });
        graph.set_scale([](int canonical) { return ScaleStyle::instance().px(canonical); });
    }

    // ---- drawing
    void text(cairo_t* cr, const std::string& s, const GraphRect& r, const Rgb& c, Align align, bool ellipsize = false, int alpha = 255)
    {
        if (s.empty() || r.width <= 0) return;
        PangoLayout* layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, font);
        pango_layout_set_text(layout, s.c_str(), -1);
        if (ellipsize) {
            pango_layout_set_width(layout, r.width * PANGO_SCALE);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
        int w = 0, h = 0;
        pango_layout_get_pixel_size(layout, &w, &h);
        int x = r.x;
        if (align == Center) x = r.x + (r.width - w) / 2;
        else if (align == Right) x = r.x + r.width - w;
        const int y = r.y + (r.height - h) / 2;
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_clip(cr);
        set_source(cr, c, alpha);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        g_object_unref(layout);
    }

    void line(cairo_t* cr, double x0, double y0, double x1, double y1, const Rgb& c, int alpha = 255, int dash = 0, double width = 1.0)
    {
        cairo_save(cr);
        set_source(cr, c, alpha);
        cairo_set_line_width(cr, width);
        if (dash == 1) { const double d[2] = { 5, 3 }; cairo_set_dash(cr, d, 2, 0); }
        else if (dash == 2) { const double d[2] = { 1, 2 }; cairo_set_dash(cr, d, 2, 0); }
        else if (dash == 3) { const double d[4] = { 5, 2, 1, 2 }; cairo_set_dash(cr, d, 4, 0); }
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    void polyline(cairo_t* cr, const std::vector<int>& ys, int x0, const Rgb& c, double width)
    {
        if (ys.size() < 2) return;
        cairo_save(cr);
        set_source(cr, c);
        cairo_set_line_width(cr, width);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        for (size_t i = 0; i < ys.size(); ++i) {
            if (i == 0) cairo_move_to(cr, x0 + 0.5, ys[i] + 0.5);
            else cairo_line_to(cr, x0 + static_cast<double>(i) + 0.5, ys[i] + 0.5);
        }
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    void fill_rect(cairo_t* cr, int x, int y, int w, int h, const Rgb& c, int alpha)
    {
        set_source(cr, c, alpha);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    }

    void draw_static(cairo_t* cr, int w, int h)
    {
        const app::GraphStaticFrame& f = model->graph().static_frame();
        set_source(cr, kBackground);
        cairo_paint(cr);
        if (f.empty) {
            GraphRect r;
            r.width = w;
            r.height = h;
            text(cr, f.message, r, kSilver, Center, true);
            return;
        }
        const GraphRect& a = f.area;
        text(cr, f.scope.text, f.scope.bounds, kForeground, Left);
        // panel frame and grid
        cairo_save(cr);
        set_source(cr, kGrid);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, a.x + 0.5, a.y + 0.5, a.width, a.height);
        cairo_stroke(cr);
        for (int gy : f.grid_y) {
            cairo_move_to(cr, a.x, gy + 0.5);
            cairo_line_to(cr, a.right(), gy + 0.5);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
        for (int ox : f.overflow_x) fill_rect(cr, ox, a.y, 1, a.height, kOverflow, 65);
        const int percent = ScaleStyle::instance().px(3);
        const double plot_width = std::max(1, percent / 2 + percent % 2);   // 1.5 canonical pixels, rounded up
        polyline(cr, f.source_y, a.x, f.bytes_model ? kBytesPlot : kEventsPlot, plot_width);
        if (!f.gate_y.empty()) polyline(cr, f.gate_y, a.x, kGatePlot, plot_width);
        if (f.capacity_y >= 0) line(cr, a.x, f.capacity_y + 0.5, a.right(), f.capacity_y + 0.5, kCapacity, 220, 1);
        text(cr, f.title.text, f.title.bounds, kForeground, Left, true);
        text(cr, f.scale.text, f.scale.bounds, kForeground, Right);
        for (const app::GraphAxisTick& t : f.ticks) {
            line(cr, t.x + 0.5, t.mark_top, t.x + 0.5, t.mark_bottom, kTickPen);
            text(cr, t.label, t.label_bounds, kSilver, Center);
        }
        // marker lanes
        for (int x : f.cluster_x) {
            const int r = f.cluster_radius;
            cairo_save(cr);
            set_source(cr, kCluster, 225);
            cairo_move_to(cr, x, f.cluster_y - r);
            cairo_line_to(cr, x + r, f.cluster_y);
            cairo_line_to(cr, x, f.cluster_y + r);
            cairo_line_to(cr, x - r, f.cluster_y);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        for (const app::GraphPressureStrip& p : f.pressure) {
            const int level = static_cast<int>(p.level);
            fill_rect(cr, p.x, f.pressure_y, 1, f.pressure_height, kPressure[level], kPressureAlpha[level]);
        }
        for (const app::GraphTextItem& item : f.legend) text(cr, item.text, item.bounds, kSilver, Left, true);
    }

    void draw_dynamic(cairo_t* cr)
    {
        app::GraphDynamicFrame f = model->graph().dynamic_frame();
        const GraphRect a = model->graph().graph_area();
        for (const app::GraphLiveMarker& m : f.markers) {
            if (m.midi_output) line(cr, m.x + 0.5, a.y, m.x + 0.5, a.bottom(), kOutputMarker, 225, 3);
            else line(cr, m.x + 0.5, a.y, m.x + 0.5, a.bottom(), kPlaybackMarker, 225, 2);
        }
        if (f.overlay_visible) {
            const GraphRect& o = f.overlay_bounds;
            fill_rect(cr, o.x, o.y, o.width, o.height, kOverlayBackground, 218);
            cairo_save(cr);
            set_source(cr, kOverlayOutline, 130);
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, o.x + 0.5, o.y + 0.5, o.width - 1, o.height - 1);
            cairo_stroke(cr);
            cairo_restore(cr);
            for (const app::GraphTextItem& item : f.overlay_lines) text(cr, item.text, item.bounds, kWhiteSmoke, Left, true);
        }
        if (f.pin_visible) line(cr, f.pin_x + 0.5, a.y, f.pin_x + 0.5, a.bottom(), kWhite, 255, 1);
        if (f.hover_visible) line(cr, f.hover_x + 0.5, a.y, f.hover_x + 0.5, a.bottom(), kHover, 230);
        if (f.tip_visible) {
            const GraphRect& t = f.tip_bounds;
            fill_rect(cr, t.x, t.y, t.width, t.height, kTipBackground, 220);
            GraphRect tx = t;
            tx.x += ScaleStyle::instance().px(4);
            tx.width -= ScaleStyle::instance().px(6);
            text(cr, f.tip_text, tx, kWhite, Left);
        }
    }

    void paint(cairo_t* cr)
    {
        const int w = gtk_widget_get_allocated_width(area), h = gtk_widget_get_allocated_height(area);
        if (w <= 0 || h <= 0) return;
        if (!statics || static_w != w || static_h != h) {
            if (statics) cairo_surface_destroy(statics);
            statics = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            static_w = w;
            static_h = h;
            static_revision = 0;
        }
        const uint64_t revision = model->graph().static_revision();
        if (static_revision != revision) {
            cairo_t* sc = cairo_create(statics);
            draw_static(sc, w, h);
            cairo_destroy(sc);
            // The static frame may have changed again while it was built (the model rebuilds it on demand).
            static_revision = model->graph().static_revision();
        }
        cairo_set_source_surface(cr, statics, 0, 0);
        cairo_paint(cr);
        draw_dynamic(cr);
    }

    // ---- input
    void after_input()
    {
        if (detached) return;
        if (model->graph().take_repaint_request()) gtk_widget_queue_draw(area);
        update_animation();
        if (changed) changed();
    }

    void update_animation()
    {
        if (model->graph().needs_dynamic_update() && tick_id == 0) {
            tick_id = gtk_widget_add_tick_callback(area, [](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
                Impl* self = static_cast<Impl*>(data);
                if (self->detached) { self->tick_id = 0; return G_SOURCE_REMOVE; }
                if (self->model->graph_dynamic_update()) gtk_widget_queue_draw(self->area);
                if (self->model->graph().take_repaint_request()) gtk_widget_queue_draw(self->area);
                if (!self->model->graph().needs_dynamic_update()) {
                    self->tick_id = 0;
                    if (self->changed) self->changed();
                    return G_SOURCE_REMOVE;
                }
                if (self->changed) self->changed();
                return G_SOURCE_CONTINUE;
            }, this, nullptr);
        }
    }

    void update_cursor()
    {
        GdkWindow* window = gtk_widget_get_window(area);
        if (!window) return;
        const char* name = model->graph().cursor() == app::GraphCursor::SizeWE ? "ew-resize" : "crosshair";
        GdkCursor* cursor = gdk_cursor_new_from_name(gtk_widget_get_display(area), name);
        gdk_window_set_cursor(window, cursor);
        if (cursor) g_object_unref(cursor);
    }
};

WorkloadGraphWidget::WorkloadGraphWidget(app::AnalysisWindowModel* model, std::function<void()> changed) : impl_(new Impl())
{
    Impl* d = impl_.get();
    d->model = model;
    d->changed = std::move(changed);
    area_ = gtk_drawing_area_new();
    d->area = area_;
    gtk_widget_set_can_focus(area_, TRUE);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_widget_set_has_tooltip(area_, TRUE);
    gtk_widget_add_events(area_, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK |
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_STRUCTURE_MASK);
    d->make_font();
    d->install_hooks();

    connect_draw(area_, [d](cairo_t* cr, GtkWidget*) { d->paint(cr); });
    g_signal_connect(area_, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle* r, gpointer data) {
        Impl* self = static_cast<Impl*>(data);
        if (self->detached) return;
        self->model->graph_resized(r->width, r->height);
        gtk_widget_queue_draw(self->area);
        // The model reacts to the size in the next main-loop turn (the window must not change controls inside an allocation).
        g_idle_add([](gpointer p) -> gboolean { Impl* s = static_cast<Impl*>(p); if (!s->detached) s->after_input(); return G_SOURCE_REMOVE; }, self);
    }), d);
    connect_event(area_, "motion-notify-event", [d](GdkEvent* e) {
        GdkEventMotion* m = reinterpret_cast<GdkEventMotion*>(e);
        d->model->graph_mouse_move(static_cast<int>(m->x), static_cast<int>(m->y));
        d->update_cursor();
        d->after_input();
        return true;
    });
    connect_event(area_, "leave-notify-event", [d](GdkEvent* e) {
        GdkEventCrossing* c = reinterpret_cast<GdkEventCrossing*>(e);
        if (c->mode == GDK_CROSSING_GRAB || c->mode == GDK_CROSSING_UNGRAB) return false;   // the implicit grab of a drag
        d->model->graph_mouse_leave();
        d->after_input();
        return false;
    });
    connect_event(area_, "button-press-event", [d](GdkEvent* e) {
        GdkEventButton* b = reinterpret_cast<GdkEventButton*>(e);
        if (b->button != 1) return false;
        gtk_widget_grab_focus(d->area);
        if (b->type == GDK_2BUTTON_PRESS) {
            d->model->graph_double_click(static_cast<int>(b->x), static_cast<int>(b->y));
            d->after_input();
            return true;
        }
        if (b->type != GDK_BUTTON_PRESS) return true;
        // The second press of a double click is the double click itself (as on Windows, where it replaces the press).
        gint dbl_time = 400, dbl_dist = 5;
        g_object_get(gtk_widget_get_settings(d->area), "gtk-double-click-time", &dbl_time, "gtk-double-click-distance", &dbl_dist, nullptr);
        const bool second = d->last_press_time != 0 && b->time - d->last_press_time <= static_cast<guint32>(dbl_time) &&
            std::fabs(b->x - d->last_press_x) <= dbl_dist && std::fabs(b->y - d->last_press_y) <= dbl_dist;
        d->last_press_time = second ? 0 : b->time;
        d->last_press_x = b->x;
        d->last_press_y = b->y;
        if (second) { d->skip_release = true; return true; }
        d->skip_release = false;
        d->model->graph_mouse_down(static_cast<int>(b->x), static_cast<int>(b->y));
        d->after_input();
        return true;
    });
    connect_event(area_, "button-release-event", [d](GdkEvent* e) {
        GdkEventButton* b = reinterpret_cast<GdkEventButton*>(e);
        if (b->button != 1) return false;
        if (d->skip_release) { d->skip_release = false; return true; }
        d->model->graph_mouse_up(static_cast<int>(b->x), static_cast<int>(b->y));
        d->after_input();
        return true;
    });
    connect_event(area_, "scroll-event", [d](GdkEvent* e) {
        GdkEventScroll* s = reinterpret_cast<GdkEventScroll*>(e);
        int delta = 0;
        if (s->direction == GDK_SCROLL_UP) delta = 120;
        else if (s->direction == GDK_SCROLL_DOWN) delta = -120;
        else if (s->direction == GDK_SCROLL_SMOOTH) delta = s->delta_y < 0 ? 120 : s->delta_y > 0 ? -120 : 0;
        if (delta == 0) return false;
        d->model->graph_wheel(static_cast<int>(s->x), static_cast<int>(s->y), delta);
        d->after_input();
        return true;
    });
    g_signal_connect(area_, "query-tooltip", G_CALLBACK(+[](GtkWidget*, gint x, gint y, gboolean, GtkTooltip* tooltip, gpointer data) -> gboolean {
        Impl* self = static_cast<Impl*>(data);
        if (self->detached) return FALSE;
        const std::string text = self->model->graph().tooltip_text(x, y);
        if (text.empty()) return FALSE;
        gtk_tooltip_set_text(tooltip, text.c_str());
        return TRUE;
    }), d);
    g_signal_connect(area_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        Impl* self = static_cast<Impl*>(data);
        self->detached = true;
        // The model outlives the control and must not call back into it.
        self->model->graph().set_text_measure([](const std::string& text, bool) {
            app::GraphSize size;
            size.width = static_cast<int>(text.size()) * 6;
            size.height = 13;
            return size;
        });
    }), d);
}

WorkloadGraphWidget::~WorkloadGraphWidget()
{
    Impl* d = impl_.get();
    d->detached = true;
    if (d->statics) cairo_surface_destroy(d->statics);
    if (d->measure_layout) g_object_unref(d->measure_layout);
    if (d->font) pango_font_description_free(d->font);
}

void WorkloadGraphWidget::scale_changed()
{
    Impl* d = impl_.get();
    if (d->detached) return;
    d->make_font();
    d->install_hooks();
    d->static_revision = 0;
    gtk_widget_queue_draw(area_);
}

void WorkloadGraphWidget::invalidate()
{
    Impl* d = impl_.get();
    if (d->detached) return;
    if (d->model->graph().take_repaint_request()) gtk_widget_queue_draw(area_);
    d->update_animation();
}

void WorkloadGraphWidget::detach() { impl_->detached = true; }

}   // namespace gtkui
}   // namespace midibn
