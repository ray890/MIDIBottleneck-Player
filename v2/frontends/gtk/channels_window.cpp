// The Channels window (v1 ChannelMonitorForm) in GTK 3: the 16 x 16 grid drawn with cairo, the in-place scrub-or-type editor
// (a GtkEntry over the clicked cell), per-cell tooltips, transient feedback tips and the detached message. All behaviour is
// app::ChannelSession (and its editor); this file draws and turns pointer and keyboard input into calls on it.
#include "companion.h"

#include "midibn/app/channel_navigation.h"
#include "midibn/app/channel_session.h"
#include "midibn/app/ui_mappings.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace midibn {
namespace gtkui {

namespace {

struct Rgb { double r, g, b; };
const Rgb kForced = { 20 / 255.0, 75 / 255.0, 155 / 255.0 };
const Rgb kHistorical = { 105 / 255.0, 105 / 255.0, 105 / 255.0 };
const Rgb kNormal = { 0.1, 0.1, 0.1 };
const Rgb kWindow = { 1, 1, 1 };
const Rgb kFace = { 240 / 255.0, 240 / 255.0, 240 / 255.0 };
const Rgb kGridLine = { 160 / 255.0, 160 / 255.0, 160 / 255.0 };
const Rgb kHeaderSep = { 229 / 255.0, 229 / 255.0, 229 / 255.0 };

std::string file_name_of(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void set_rgb(cairo_t* cr, const Rgb& c) { cairo_set_source_rgb(cr, c.r, c.g, c.b); }

class ChannelsWindow : public CompanionWindow {
public:
    explicit ChannelsWindow(CompanionHost& host) : host_(host)
    {
        wake_target_ = std::make_shared<std::atomic<ChannelsWindow*>>(this);
        access_.reset(new app::PlaybackEngineChannels(host.controller->engine()));
        app::ChannelSession::Host sh;
        sh.engine = access_.get();
        sh.culture = host.controller->culture();
        auto target = wake_target_;
        sh.wake = [target] {
            struct Call { std::shared_ptr<std::atomic<ChannelsWindow*>> target; };
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                Call* call = static_cast<Call*>(data);
                if (ChannelsWindow* w = call->target->load()) { w->session_->pump(); w->process_session_output(); }
                delete call;
                return G_SOURCE_REMOVE;
            }, new Call{ target }, nullptr);
        };
        app::PlayerController* controller = host.controller;
        auto request_refresh = host.request_refresh;
        sh.overrides_changed = [controller, target, request_refresh] {
            controller->notify_overrides_changed();
            struct Call { std::shared_ptr<std::atomic<ChannelsWindow*>> target; std::function<void()> refresh; };
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                Call* call = static_cast<Call*>(data);
                if (call->target->load() && call->refresh) call->refresh();
                delete call;
                return G_SOURCE_REMOVE;
            }, new Call{ target, request_refresh }, nullptr);
        };
        session_.reset(new app::ChannelSession(sh));
    }

    ~ChannelsWindow() override
    {
        wake_target_->store(nullptr);
        close();
    }

    bool create()
    {
        if (!host_.controller->has_song()) return false;
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), "MIDIBottleneck Player \xE2\x80\x94 MIDI Channel Monitor");
        build_children();
        if (host_.main_window) {   // v1: 28 pixels right of and below the main window's corner
            gint mx = 0, my = 0;
            gtk_window_get_position(GTK_WINDOW(host_.main_window), &mx, &my);
            gtk_window_move(GTK_WINDOW(window_), mx + 28, my + 28);
        }
        host_.controller->take_channel_control_failures();   // a failure from before this window existed is not its business
        session_->start(build_info());
        fonts_ready_ = false;
        measure();
        apply_widths(true);
        process_session_output();
        gtk_widget_show_all(window_);
        gtk_widget_hide(tip_label_holder_);
        fit_window();
        return true;
    }

    GtkWidget* widget() const override { return window_; }
    bool alive() const override { return alive_; }

    void on_refresh() override
    {
        if (!alive_) return;
        std::vector<app::ChannelControlFailure> failures = host_.controller->take_channel_control_failures();
        for (const app::ChannelControlFailure& f : failures) session_->control_failed(f.channel, f.attribute, f.message, f.is_mute);
        session_->sync(build_info());
        session_->pump();
        process_session_output();
    }

    void on_scale_changed() override
    {
        if (!alive_) return;
        hide_editor(false);
        fonts_ready_ = false;
        measure();
        apply_widths(true);
        session_->rebuild();
        process_session_output();
        gtk_widget_queue_resize(area_);
        fit_window();
    }

    void close() override
    {
        if (!alive_) return;
        alive_ = false;
        wake_target_->store(nullptr);
        if (follow_timer_) { g_source_remove(follow_timer_); follow_timer_ = 0; }
        if (tip_timer_) { g_source_remove(tip_timer_); tip_timer_ = 0; }
        session_->stop();   // v1 FormClosed: monitoring off
        GtkWidget* w = window_;
        window_ = nullptr;
        if (w) gtk_widget_destroy(w);
    }

private:
    // ---- the song
    app::ChannelSongInfo build_info()
    {
        app::PlayerController* c = host_.controller;
        app::ChannelSongInfo info;
        info.loading = c->loading();
        info.has_song = c->has_song();
        info.file_name = file_name_of(c->song_path());
        info.song_revision = c->song_revision();
        info.absent_message = c->no_song_message();
        info.chase_enabled = c->chase_enabled();
        info.position_us = c->playback_position_for_readouts();
        info.source = c->effective_song().get();
        return info;
    }

    void process_session_output()
    {
        if (!alive_) return;
        if (session_->view_revision() != seen_view_revision_) {
            seen_view_revision_ = session_->view_revision();
            const app::ChannelGridModel& model = session_->model();
            gtk_window_set_title(GTK_WINDOW(window_), model.title().c_str());
            detached_ = model.detached();
            detached_message_ = model.detached_message();
            gtk_widget_set_sensitive(explanation_, !detached_);
            if (detached_) hide_editor(false);
            gtk_widget_queue_draw(area_);
        }
        if (!session_->editor().is_open() && editor_visible()) hide_editor(false);
        apply_changes(session_->take_changes());
        for (const app::ChannelFeedback& f : session_->take_feedback()) show_feedback(f);
        update_timers();
    }

    void update_timers()
    {
        const bool want = session_->follow_up_active();
        if (want && !follow_timer_) {
            follow_timer_ = g_timeout_add(25, [](gpointer data) -> gboolean {
                ChannelsWindow* self = static_cast<ChannelsWindow*>(data);
                if (!self->alive_) return G_SOURCE_REMOVE;
                self->on_refresh();
                self->session_->follow_up_tick();
                if (!self->session_->follow_up_active()) { self->follow_timer_ = 0; return G_SOURCE_REMOVE; }
                return G_SOURCE_CONTINUE;
            }, this);
        }
    }

    // ---- construction
    void build_children()
    {
        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(outer), 6);
        gtk_container_add(GTK_CONTAINER(window_), outer);
        scroll_ = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll_), GTK_SHADOW_IN);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll_), TRUE);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll_), TRUE);
        gtk_box_pack_start(GTK_BOX(outer), scroll_, TRUE, TRUE, 0);
        overlay_ = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(scroll_), overlay_);
        area_ = gtk_drawing_area_new();
        gtk_widget_set_can_focus(area_, TRUE);
        gtk_widget_set_has_tooltip(area_, TRUE);
        gtk_container_add(GTK_CONTAINER(overlay_), area_);
        editor_ = gtk_entry_new();
        gtk_entry_set_alignment(GTK_ENTRY(editor_), 1.0f);
        gtk_editable_set_editable(GTK_EDITABLE(editor_), FALSE);
        gtk_widget_set_no_show_all(editor_, TRUE);
        gtk_widget_set_halign(editor_, GTK_ALIGN_START);
        gtk_widget_set_valign(editor_, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), editor_);
        g_signal_connect(overlay_, "get-child-position", G_CALLBACK(+[](GtkOverlay*, GtkWidget* child, GdkRectangle* alloc, gpointer data) -> gboolean {
            ChannelsWindow* self = static_cast<ChannelsWindow*>(data);
            if (child != self->editor_) return FALSE;
            alloc->x = self->editor_rect_.x;
            alloc->y = self->editor_rect_.y;
            alloc->width = self->editor_rect_.width;
            alloc->height = self->editor_rect_.height;
            return TRUE;
        }), this);
        gtk_widget_add_events(area_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
        gtk_widget_add_events(editor_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);

        GtkWidget* bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        explanation_ = make_label(app::ChannelGridModel::explanation().c_str());
        gtk_label_set_line_wrap(GTK_LABEL(explanation_), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(explanation_), 150);
        gtk_widget_set_hexpand(explanation_, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(explanation_), "dim-label");
        gtk_box_pack_start(GTK_BOX(bottom), explanation_, TRUE, TRUE, 0);
        GtkWidget* menu_button = gtk_button_new_with_label("\xE2\x89\xA1");
        set_tooltip(menu_button, "Menu: always on top");
        gtk_widget_set_valign(menu_button, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(bottom), menu_button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(outer), bottom, FALSE, FALSE, 0);
        tip_label_holder_ = gtk_label_new("");   // placeholder kept hidden (feedback tips use a popover)
        gtk_box_pack_start(GTK_BOX(outer), tip_label_holder_, FALSE, FALSE, 0);
        menu_ = gtk_menu_new();
        mi_top_ = gtk_check_menu_item_new_with_label("Always on top");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu_), mi_top_);
        gtk_widget_show_all(menu_);
        connect(mi_top_, "toggled", [this] { gtk_window_set_keep_above(GTK_WINDOW(window_), gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(mi_top_))); });
        connect(menu_button, "clicked", [this, menu_button] {
            gtk_menu_popup_at_widget(GTK_MENU(menu_), menu_button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, nullptr);
        });

        connect_draw(area_, [this](cairo_t* cr, GtkWidget*) { paint(cr); });
        g_signal_connect(area_, "query-tooltip", G_CALLBACK(+[](GtkWidget*, gint x, gint y, gboolean, GtkTooltip* tooltip, gpointer data) -> gboolean {
            return static_cast<ChannelsWindow*>(data)->on_query_tooltip(x, y, tooltip) ? TRUE : FALSE;
        }), this);
        connect_event(area_, "button-press-event", [this](GdkEvent* e) { return on_grid_press(reinterpret_cast<GdkEventButton*>(e)); });
        connect_event(area_, "motion-notify-event", [this](GdkEvent* e) { return on_gesture_motion(reinterpret_cast<GdkEventMotion*>(e)); });
        connect_event(area_, "button-release-event", [this](GdkEvent* e) { return on_gesture_release(reinterpret_cast<GdkEventButton*>(e)); });
        connect_event(area_, "key-press-event", [this](GdkEvent* e) { return on_grid_key(reinterpret_cast<GdkEventKey*>(e)); });
        connect_event(area_, "focus-in-event", [this](GdkEvent*) { has_focus_ = true; gtk_widget_queue_draw(area_); return false; });
        connect_event(area_, "focus-out-event", [this](GdkEvent*) { has_focus_ = false; gtk_widget_queue_draw(area_); return false; });
        // the editor entry
        connect_event(editor_, "button-press-event", [this](GdkEvent* e) { return on_editor_press(reinterpret_cast<GdkEventButton*>(e)); });
        connect_event(editor_, "motion-notify-event", [this](GdkEvent* e) { return on_gesture_motion(reinterpret_cast<GdkEventMotion*>(e)); });
        connect_event(editor_, "button-release-event", [this](GdkEvent* e) { return on_gesture_release(reinterpret_cast<GdkEventButton*>(e)); });
        connect_event(editor_, "key-press-event", [this](GdkEvent* e) { return on_editor_key(reinterpret_cast<GdkEventKey*>(e)); });
        connect_event(editor_, "focus-out-event", [this](GdkEvent*) { if (editor_typing_) editor_commit(false); return false; });
        connect_delete(window_, [this] { close(); return true; });
        connect_event(window_, "focus-out-event", [this](GdkEvent*) { hide_editor(true); return false; });   // v1 Deactivate
        gtk_widget_grab_focus(area_);
    }

    // ---- metrics
    PangoFontDescription* base_font()
    {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        return pango_font_description_copy(gtk_style_context_get_font(gtk_widget_get_style_context(area_), GTK_STATE_FLAG_NORMAL));
        G_GNUC_END_IGNORE_DEPRECATIONS
    }

    int text_width(const std::string& text, app::CellFont font)
    {
        PangoLayout* layout = gtk_widget_create_pango_layout(area_, text.c_str());
        PangoFontDescription* desc = base_font();
        if (font == app::CellFont::Bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        if (font == app::CellFont::Italic) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
        pango_layout_set_font_description(layout, desc);
        int w = 0, h = 0;
        pango_layout_get_pixel_size(layout, &w, &h);
        text_height_ = std::max(text_height_, h);
        pango_font_description_free(desc);
        g_object_unref(layout);
        return w;
    }

    void measure()
    {
        text_height_ = 0;
        text_width("Ag", app::CellFont::Regular);
        const int percent = ScaleStyle::instance().percent();
        row_height_ = std::max(app::scale_metric(22, percent), text_height_ + app::scale_metric(6, percent));
        header_height_ = row_height_ + app::scale_metric(1, percent);
        fonts_ready_ = true;
    }

    int header_preferred(int column)
    {
        const int percent = ScaleStyle::instance().percent();
        const app::ChannelColumnInfo& info = app::channel_column_info(column);
        if (column == app::ColProgram) return app::scale_metric(158, percent);
        return std::max(app::scale_metric(34, percent), text_width(info.caption, app::CellFont::Regular) + app::scale_metric(13, percent));
    }

    void apply_widths(bool reset)
    {
        const app::ChannelGridModel& model = session_->model();
        const int percent = ScaleStyle::instance().percent();
        for (int c = 0; c < app::kChannelColumnCount; ++c) {
            int need = reset ? header_preferred(c) : widths_[c];
            for (int r = 0; r < 16; ++r) {
                const app::ChannelCell& cell = model.cell(r, c);
                need = std::max(need, text_width(cell.text, cell.font) + app::scale_metric(10, percent));
            }
            widths_[c] = need;
        }
        update_size_request();
    }

    void update_size_request()
    {
        int total = 0;
        for (int c = 0; c < app::kChannelColumnCount; ++c) total += widths_[c];
        content_w_ = total;
        content_h_ = header_height_ + 16 * row_height_;
        gtk_widget_set_size_request(area_, content_w_, content_h_);
    }

    void fit_window()
    {
        if (!window_) return;
        // v1: the window fits the grid; the explanation line wraps at that width.
        const int percent = ScaleStyle::instance().percent();
        const int pads = 2 * 6 + 4;
        int width = std::max(app::scale_metric(420, percent), content_w_ + pads);
        int label_h = 0, natural = 0;
        gtk_widget_get_preferred_height_for_width(explanation_, std::max(1, width - 40), &label_h, &natural);
        GdkRectangle work = {};
        GdkMonitor* monitor = gdk_display_get_primary_monitor(gtk_widget_get_display(window_));
        if (monitor) gdk_monitor_get_workarea(monitor, &work);
        int height = content_h_ + pads + std::max(label_h, natural) + 14;
        if (work.width > 0) { width = std::min(width, work.width); height = std::min(height, work.height); }
        gtk_window_resize(GTK_WINDOW(window_), width, height);
    }

    int column_left(int column) const
    {
        int x = 0;
        for (int c = 0; c < column; ++c) x += widths_[c];
        return x;
    }
    // row -1 is the heading row
    GdkRectangle cell_rect(int row, int column) const
    {
        GdkRectangle r;
        r.x = column_left(column);
        r.width = widths_[column];
        r.y = row < 0 ? 0 : header_height_ + row * row_height_;
        r.height = row < 0 ? header_height_ : row_height_;
        return r;
    }
    bool hit_test(int x, int y, int& row, int& column) const
    {
        if (x < 0 || y < 0 || x >= content_w_) return false;
        if (y < header_height_) { row = -1; }
        else {
            row = (y - header_height_) / std::max(1, row_height_);
            if (row > 15) return false;
        }
        int left = 0;
        for (column = 0; column < app::kChannelColumnCount; ++column) {
            if (x < left + widths_[column]) return true;
            left += widths_[column];
        }
        return false;
    }

    // ---- drawing
    void text(cairo_t* cr, const std::string& s, const GdkRectangle& r, const Rgb& color, app::CellFont font, int align)
    {
        if (s.empty()) return;
        PangoLayout* layout = pango_cairo_create_layout(cr);
        PangoFontDescription* desc = base_font();
        if (font == app::CellFont::Bold) pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        if (font == app::CellFont::Italic) pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
        pango_layout_set_font_description(layout, desc);
        pango_layout_set_text(layout, s.c_str(), -1);
        pango_layout_set_width(layout, r.width * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        int w = 0, h = 0;
        pango_layout_get_pixel_size(layout, &w, &h);
        int x = r.x;
        if (align == 1) x = r.x + (r.width - w) / 2;
        else if (align == 2) x = r.x + r.width - w;
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        cairo_clip(cr);
        set_rgb(cr, color);
        cairo_move_to(cr, x, r.y + (r.height - h) / 2);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
        pango_font_description_free(desc);
        g_object_unref(layout);
    }

    void hline(cairo_t* cr, double x0, double x1, double y, const Rgb& c)
    {
        set_rgb(cr, c);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, x0, y + 0.5);
        cairo_line_to(cr, x1, y + 0.5);
        cairo_stroke(cr);
    }
    void vline(cairo_t* cr, double x, double y0, double y1, const Rgb& c)
    {
        set_rgb(cr, c);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, x + 0.5, y0);
        cairo_line_to(cr, x + 0.5, y1);
        cairo_stroke(cr);
    }

    void paint(cairo_t* cr)
    {
        if (!fonts_ready_) measure();
        const int w = gtk_widget_get_allocated_width(area_), h = gtk_widget_get_allocated_height(area_);
        set_rgb(cr, kWindow);
        cairo_paint(cr);
        const int percent = ScaleStyle::instance().percent();
        if (detached_) {
            GdkRectangle all = { 0, 0, w, h };
            text(cr, detached_message_, all, kHistorical, app::CellFont::Regular, 1);
            return;
        }
        const app::ChannelGridModel& model = session_->model();
        for (int r = 0; r < 16; ++r) {
            for (int c = 0; c < app::kChannelColumnCount; ++c) {
                const GdkRectangle rect = cell_rect(r, c);
                const app::ChannelCell& cell = model.cell(r, c);
                set_rgb(cr, model.row_enabled(r) ? kWindow : kFace);
                cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
                cairo_fill(cr);
                GdkRectangle t = rect;
                t.x += app::scale_metric(4, percent);
                t.width -= app::scale_metric(9, percent);
                text(cr, cell.text, t, cell.color == app::CellColor::Forced ? kForced : cell.color == app::CellColor::Historical ? kHistorical : kNormal,
                    cell.font, c == app::ColChannel ? 1 : 2);
                vline(cr, rect.x + rect.width - 1, rect.y, rect.y + rect.height, kGridLine);
                hline(cr, rect.x, rect.x + rect.width, rect.y + rect.height - 1, kGridLine);
                if (has_focus_ && keyboard_used_ && r == current_row_ && c == current_column_) {
                    cairo_save(cr);
                    const double dashes[2] = { 1, 1 };
                    cairo_set_dash(cr, dashes, 2, 0);
                    set_rgb(cr, { 0, 0, 0 });
                    cairo_set_line_width(cr, 1.0);
                    cairo_rectangle(cr, rect.x + 1.5, rect.y + 1.5, rect.width - 3, rect.height - 3);
                    cairo_stroke(cr);
                    cairo_restore(cr);
                }
            }
        }
        // the heading row
        for (int c = 0; c < app::kChannelColumnCount; ++c) {
            const GdkRectangle rect = cell_rect(-1, c);
            set_rgb(cr, kWindow);
            cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
            cairo_fill(cr);
            GdkRectangle t = rect;
            t.x += app::scale_metric(6, percent);
            t.width -= app::scale_metric(8, percent);
            text(cr, app::channel_column_info(c).caption, t, kNormal, app::CellFont::Regular, 0);
            vline(cr, rect.x + rect.width - 1, rect.y, rect.y + rect.height - 1, kHeaderSep);
            hline(cr, rect.x, rect.x + rect.width, rect.y + rect.height - 1, kGridLine);
        }
    }

    void apply_changes(const app::ChannelGridChanges& changes)
    {
        if (!changes.any()) return;
        // Columns whose changed values no longer fit grow (never shrink while the window lives).
        const app::ChannelGridModel& model = session_->model();
        const int percent = ScaleStyle::instance().percent();
        bool widened = false;
        for (int r = 0; r < 16; ++r) {
            const uint16_t bits = changes.rows[static_cast<size_t>(r)];
            if (bits == 0) continue;
            for (int c = 0; c < app::kChannelColumnCount; ++c) {
                if ((bits & (1u << c)) == 0) continue;
                const app::ChannelCell& cell = model.cell(r, c);
                const int need = text_width(cell.text, cell.font) + app::scale_metric(10, percent);
                if (need > widths_[c]) { widths_[c] = need; widened = true; }
            }
        }
        if (widened) {
            hide_editor(true);
            update_size_request();
            gtk_widget_queue_resize(area_);
            fit_window();
        }
        gtk_widget_queue_draw(area_);
    }

    // ---- tooltips and feedback
    bool on_query_tooltip(int x, int y, GtkTooltip* tooltip)
    {
        if (detached_) return false;
        int row = 0, column = 0;
        if (!hit_test(x, y, row, column)) return false;
        std::string text;
        if (row < 0) text = app::channel_column_info(column).tooltip;
        else text = session_->model().cell(row, column).tooltip;
        if (text.empty()) return false;
        gtk_tooltip_set_text(tooltip, text.c_str());
        return true;
    }

    void show_tip(const std::string& text, GdkRectangle where, int milliseconds)
    {
        if (text.empty() || !alive_) return;
        if (!tip_popover_) {
            tip_popover_ = gtk_popover_new(area_);
            gtk_popover_set_modal(GTK_POPOVER(tip_popover_), FALSE);
            gtk_popover_set_position(GTK_POPOVER(tip_popover_), GTK_POS_BOTTOM);
            tip_text_ = gtk_label_new("");
            gtk_label_set_line_wrap(GTK_LABEL(tip_text_), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(tip_text_), 60);
            gtk_widget_set_margin_start(tip_text_, 8);
            gtk_widget_set_margin_end(tip_text_, 8);
            gtk_widget_set_margin_top(tip_text_, 6);
            gtk_widget_set_margin_bottom(tip_text_, 6);
            gtk_container_add(GTK_CONTAINER(tip_popover_), tip_text_);
            gtk_widget_show_all(tip_text_);
        }
        gtk_label_set_text(GTK_LABEL(tip_text_), text.c_str());
        gtk_popover_set_pointing_to(GTK_POPOVER(tip_popover_), &where);
        gtk_popover_popup(GTK_POPOVER(tip_popover_));
        if (tip_timer_) g_source_remove(tip_timer_);
        tip_timer_ = g_timeout_add(static_cast<guint>(milliseconds), [](gpointer data) -> gboolean {
            ChannelsWindow* self = static_cast<ChannelsWindow*>(data);
            self->tip_timer_ = 0;
            if (self->tip_popover_) gtk_popover_popdown(GTK_POPOVER(self->tip_popover_));
            return G_SOURCE_REMOVE;
        }, this);
    }

    void show_feedback(const app::ChannelFeedback& f)
    {
        if (f.text.empty()) return;
        if (f.kind == app::ChannelFeedback::CellTip) {
            if (f.channel < 0 || f.channel >= 16 || f.column < 0 || f.column >= app::kChannelColumnCount) return;
            show_tip(f.text, cell_rect(f.channel, f.column), f.milliseconds);
        }
        else {
            GdkRectangle where = { 8, std::max(0, content_h_ - 26), 10, 10 };
            show_tip(f.text, where, f.milliseconds);
        }
    }

    // ---- the editor
    bool editor_visible() const { return editor_ && gtk_widget_get_visible(editor_); }

    void apply_editor_look()
    {
        app::ChannelEditor& editor = session_->editor();
        if (!editor_typing_) {
            const std::string flat = editor.flat_text();
            if (flat != gtk_entry_get_text(GTK_ENTRY(editor_))) gtk_entry_set_text(GTK_ENTRY(editor_), flat.c_str());
        }
        PangoAttrList* attrs = pango_attr_list_new();
        if (editor.bold()) {
            pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            pango_attr_list_insert(attrs, pango_attr_foreground_new(static_cast<guint16>(kForced.r * 65535), static_cast<guint16>(kForced.g * 65535), static_cast<guint16>(kForced.b * 65535)));
        }
        gtk_entry_set_attributes(GTK_ENTRY(editor_), attrs);
        pango_attr_list_unref(attrs);
        set_tooltip(editor_, editor.tooltip());
    }

    void show_editor(int row, int column)
    {
        const GdkRectangle cell = cell_rect(row, column);
        editor_rect_.x = cell.x + 1;
        editor_rect_.y = cell.y + 1;
        editor_rect_.width = std::max(12, cell.width - 2);
        editor_rect_.height = std::max(8, cell.height - 2);
        editor_typing_ = false;
        gtk_editable_set_editable(GTK_EDITABLE(editor_), FALSE);
        apply_editor_look();
        gtk_widget_set_no_show_all(editor_, FALSE);
        gtk_widget_show(editor_);
        gtk_widget_queue_resize(overlay_);
    }

    void hide_editor(bool commit_typed)
    {
        if (!editor_) return;
        const bool was_open = session_->editor().is_open();
        if (was_open) {
            const std::string typed = editor_typing_ ? gtk_entry_get_text(GTK_ENTRY(editor_)) : std::string();
            session_->hide_editor(commit_typed, typed);
        }
        editor_typing_ = false;
        gesture_cancel();
        if (editor_visible()) {
            gtk_widget_hide(editor_);
            gtk_widget_set_no_show_all(editor_, TRUE);
            if (gtk_window_get_focus(GTK_WINDOW(window_)) == editor_ || !gtk_window_get_focus(GTK_WINDOW(window_))) gtk_widget_grab_focus(area_);
        }
        if (was_open) apply_changes(session_->take_changes());
    }

    void gesture_cancel()
    {
        session_->editor().end_gesture();
        finish_cursor();
    }

    void finish_cursor()
    {
        if (cursor_hidden_) {
            GdkWindow* w = gtk_widget_get_window(area_);
            if (w) gdk_window_set_cursor(w, nullptr);
            GdkWindow* ew = editor_visible() ? gtk_widget_get_window(editor_) : nullptr;
            if (ew) gdk_window_set_cursor(ew, nullptr);
            cursor_hidden_ = false;
        }
    }

    void hide_cursor()
    {
        if (cursor_hidden_) return;
        GdkCursor* blank = gdk_cursor_new_from_name(gtk_widget_get_display(area_), "none");
        GdkWindow* w = gtk_widget_get_window(area_);
        if (w) gdk_window_set_cursor(w, blank);
        GdkWindow* ew = gtk_widget_get_window(editor_);
        if (ew) gdk_window_set_cursor(ew, blank);
        if (blank) g_object_unref(blank);
        cursor_hidden_ = true;
    }

    void start_typing(const std::string& text)
    {
        editor_typing_ = true;
        gtk_editable_set_editable(GTK_EDITABLE(editor_), TRUE);
        gtk_entry_set_text(GTK_ENTRY(editor_), text.c_str());
        gtk_widget_grab_focus(editor_);
        gtk_editable_select_region(GTK_EDITABLE(editor_), 0, -1);
    }

    void editor_error_tip(const std::string& text)
    {
        if (text.empty()) return;
        GdkRectangle r = editor_rect_;
        show_tip(text, r, 2500);
    }

    void editor_commit(bool explicit_commit)
    {
        app::ChannelEditor& editor = session_->editor();
        if (!editor.typing()) return;
        const std::string typed = gtk_entry_get_text(GTK_ENTRY(editor_));
        app::ChannelEditor::Outcome outcome = editor.commit_typed(typed, explicit_commit);
        editor_typing_ = false;
        gtk_editable_set_editable(GTK_EDITABLE(editor_), FALSE);
        apply_editor_look();
        apply_changes(session_->take_changes());
        editor_error_tip(outcome.error);
    }

    bool on_gesture_motion(GdkEventMotion* m)
    {
        app::ChannelEditor& editor = session_->editor();
        if (!editor.armed()) return false;
        const bool left_down = (m->state & GDK_BUTTON1_MASK) != 0;
        const bool fine = (m->state & GDK_SHIFT_MASK) != 0;
        app::ChannelEditor::PointerResult r = editor.pointer_move(static_cast<int>(m->x_root), static_cast<int>(m->y_root), fine, left_down);
        if (r.scrub_started) hide_cursor();
        if (r.warp) {
            GdkDevice* device = m->device;
            if (device) gdk_device_warp(device, gtk_widget_get_screen(area_), r.warp_x, r.warp_y);
        }
        if (r.requested) {
            apply_editor_look();
            apply_changes(session_->take_changes());
        }
        if (r.gesture_ended) gesture_cancel();
        return true;
    }

    bool on_gesture_release(GdkEventButton* b)
    {
        app::ChannelEditor& editor = session_->editor();
        if (b->button != 1 || !editor.armed()) return false;
        const bool typing = editor.pointer_up();
        finish_cursor();
        if (typing) start_typing(editor.typing_text());
        return true;
    }

    bool on_editor_press(GdkEventButton* b)
    {
        if (editor_typing_) return false;
        app::ChannelEditor& editor = session_->editor();
        if (b->button == 1) {
            if (b->type != GDK_BUTTON_PRESS) return true;
            editor.pointer_down(static_cast<int>(b->x_root), static_cast<int>(b->y_root));
            return true;
        }
        if (b->button == 3) {
            app::ChannelEditor::RightClickResult r = editor.right_click();
            if (r.close) hide_editor(false);
            else editor_error_tip(r.error);
            apply_changes(session_->take_changes());
            return true;
        }
        return true;
    }

    bool on_editor_key(GdkEventKey* k)
    {
        app::ChannelEditor& editor = session_->editor();
        note_key_press();
        if (!editor_typing_) {
            if (k->keyval == GDK_KEY_Escape) { editor.abandon_typing(); gtk_widget_grab_focus(area_); return true; }
            return false;
        }
        if (k->keyval == GDK_KEY_Return || k->keyval == GDK_KEY_KP_Enter) {
            editor_commit(true);
            gtk_widget_grab_focus(area_);   // the arrow keys move between cells again
            return true;
        }
        if (k->keyval == GDK_KEY_Escape) {
            editor.abandon_typing();
            editor_typing_ = false;
            gtk_editable_set_editable(GTK_EDITABLE(editor_), FALSE);
            apply_editor_look();
            gtk_widget_grab_focus(area_);
            return true;
        }
        if (k->keyval == GDK_KEY_Up || k->keyval == GDK_KEY_Down) {
            const std::string text = editor.adjust_typed(gtk_entry_get_text(GTK_ENTRY(editor_)), k->keyval == GDK_KEY_Up ? 1 : -1);
            gtk_entry_set_text(GTK_ENTRY(editor_), text.c_str());
            gtk_editable_select_region(GTK_EDITABLE(editor_), 0, -1);
            return true;
        }
        return false;
    }

    // ---- cell actions and keyboard
    void cell_action(int row, int column, bool left, int screen_x, int screen_y, bool keyboard)
    {
        const bool opened = session_->cell_mouse_down(row, column, left ? app::ChannelSession::Button::Left : app::ChannelSession::Button::Right,
            screen_x, screen_y, editor_typing_ ? std::string(gtk_entry_get_text(GTK_ENTRY(editor_))) : std::string());
        apply_changes(session_->take_changes());
        std::vector<app::ChannelFeedback> feedback = session_->take_feedback();
        if (opened) {
            show_editor(row, column);
            if (keyboard) {   // the same as a click released without moving: the value can be typed at once
                if (session_->editor().pointer_up()) start_typing(session_->editor().typing_text());
            }
        }
        else if (editor_visible() && !session_->editor().is_open()) {
            editor_typing_ = false;
            gtk_widget_hide(editor_);
            gtk_widget_set_no_show_all(editor_, TRUE);
        }
        for (const app::ChannelFeedback& f : feedback) show_feedback(f);
        if (host_.request_refresh) host_.request_refresh();
    }

    bool on_grid_press(GdkEventButton* b)
    {
        gtk_widget_grab_focus(area_);
        if (detached_ || (b->type != GDK_BUTTON_PRESS && b->type != GDK_2BUTTON_PRESS)) return true;
        int row = 0, column = 0;
        if (!hit_test(static_cast<int>(b->x), static_cast<int>(b->y), row, column) || row < 0) return true;
        if (b->button != 1 && b->button != 3) return true;
        set_current(row, column);
        cell_action(row, column, b->button == 1, static_cast<int>(b->x_root), static_cast<int>(b->y_root), false);
        return true;
    }

    void note_key_press()
    {
        if (keyboard_used_) return;
        keyboard_used_ = true;
        gtk_widget_queue_draw(area_);
    }

    void set_current(int row, int column)
    {
        current_row_ = std::max(0, std::min(15, row));
        current_column_ = std::max(0, std::min(app::kChannelColumnCount - 1, column));
        gtk_widget_queue_draw(area_);
        // keep the cell in view
        GdkRectangle r = cell_rect(current_row_, current_column_);
        GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll_));
        GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
        if (h) gtk_adjustment_clamp_page(h, r.x, r.x + r.width);
        if (v) gtk_adjustment_clamp_page(v, r.y, r.y + r.height);
    }

    int visible_body_rows() const
    {
        GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
        const double page = v ? gtk_adjustment_get_page_size(v) : content_h_;
        return std::max(1, static_cast<int>((page - header_height_) / std::max(1, row_height_)));
    }

    bool on_grid_key(GdkEventKey* k)
    {
        if (detached_) return false;
        note_key_press();
        const bool ctrl = (k->state & GDK_CONTROL_MASK) != 0;
        if (k->keyval == GDK_KEY_Escape) {
            if (session_->editor().is_open() || editor_visible()) { hide_editor(false); return true; }
            return false;
        }
        app::ChannelNavKey nav;
        switch (k->keyval) {
        case GDK_KEY_Left: nav = app::ChannelNavKey::Left; break;
        case GDK_KEY_Right: nav = app::ChannelNavKey::Right; break;
        case GDK_KEY_Up: nav = app::ChannelNavKey::Up; break;
        case GDK_KEY_Down: nav = app::ChannelNavKey::Down; break;
        case GDK_KEY_Home: nav = ctrl ? app::ChannelNavKey::CtrlHome : app::ChannelNavKey::Home; break;
        case GDK_KEY_End: nav = ctrl ? app::ChannelNavKey::CtrlEnd : app::ChannelNavKey::End; break;
        case GDK_KEY_Page_Up: nav = app::ChannelNavKey::PageUp; break;
        case GDK_KEY_Page_Down: nav = app::ChannelNavKey::PageDown; break;
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_F2: case GDK_KEY_space: case GDK_KEY_Delete: {
            const int row = current_row_, column = current_column_;
            GdkRectangle r = cell_rect(row, column);
            gint ox = 0, oy = 0;
            GdkWindow* w = gtk_widget_get_window(area_);
            if (w) gdk_window_get_origin(w, &ox, &oy);
            const int sx = ox + r.x + r.width / 2, sy = oy + r.y + r.height / 2;
            const bool is_channel = column == app::ColChannel;
            const bool is_attribute = app::channel_column_info(column).is_attribute;
            if (k->keyval == GDK_KEY_space) {
                hide_editor(true);
                cell_action(row, app::ColChannel, true, sx, sy, true);
            }
            else if (k->keyval == GDK_KEY_Delete) {
                if (is_attribute) cell_action(row, column, false, sx, sy, true);
            }
            else if (is_channel || is_attribute) cell_action(row, column, true, sx, sy, true);
            return true;
        }
        default: return false;
        }
        hide_editor(true);
        app::ChannelCellPosition from;
        from.row = current_row_;
        from.column = current_column_;
        const app::ChannelCellPosition to = app::channel_navigate(from, nav, visible_body_rows());
        set_current(to.row, to.column);
        return true;
    }

    CompanionHost host_;
    std::shared_ptr<std::atomic<ChannelsWindow*>> wake_target_;
    std::unique_ptr<app::PlaybackEngineChannels> access_;
    std::unique_ptr<app::ChannelSession> session_;
    GtkWidget *window_ = nullptr, *scroll_ = nullptr, *overlay_ = nullptr, *area_ = nullptr, *editor_ = nullptr, *explanation_ = nullptr;
    GtkWidget *tip_label_holder_ = nullptr, *tip_popover_ = nullptr, *tip_text_ = nullptr, *menu_ = nullptr, *mi_top_ = nullptr;
    GdkRectangle editor_rect_ = { 0, 0, 10, 10 };
    int widths_[app::kChannelColumnCount] = {};
    int row_height_ = 22, header_height_ = 23, content_w_ = 100, content_h_ = 100, text_height_ = 0;
    int current_row_ = 0, current_column_ = 0;
    bool alive_ = true, fonts_ready_ = false, detached_ = false, has_focus_ = false, keyboard_used_ = false;
    bool editor_typing_ = false, cursor_hidden_ = false;
    std::string detached_message_;
    uint64_t seen_view_revision_ = ~0ull;
    guint follow_timer_ = 0, tip_timer_ = 0;
};

}   // namespace

std::unique_ptr<CompanionWindow> create_channels_window(CompanionHost& host)
{
    std::unique_ptr<ChannelsWindow> window(new ChannelsWindow(host));
    if (!window->create()) return nullptr;
    return std::unique_ptr<CompanionWindow>(window.release());
}

}   // namespace gtkui
}   // namespace midibn
