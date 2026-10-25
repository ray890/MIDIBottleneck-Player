// The Analysis window (v1 DiagnosticsForm) in GTK 3 around app::AnalysisWindowModel. Layout, wording and behaviour follow
// the Windows window; the graph is workload_graph.cpp.
#include "companion.h"
#include "workload_graph.h"

#include "midibn/app/analysis_summary.h"
#include "midibn/app/help_text.h"
#include "midibn/app/analysis_window_model.h"
#include "midibn/app/ui_mappings.h"

#include <atomic>
#include <cmath>
#include <cstdio>

namespace midibn {
namespace gtkui {

namespace {

const int kDefaultClientWidth = 1080, kDefaultClientHeight = 660;
const int kSplitterDistance = 390, kInspectionHeight = 92;

// The summary text keeps v1's "\r\n"; the text view needs "\n". Offsets from find_summary_headings count UTF-16 units of
// the original text; this maps one to a character offset of the cleaned text.
std::string strip_cr(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char ch : s) if (ch != '\r') out += ch;
    return out;
}

long char_offset_for_utf16(const std::string& original, size_t utf16_offset)
{
    size_t units = 0;
    long chars = 0;
    for (size_t i = 0; i < original.size() && units < utf16_offset;) {
        const unsigned char c = static_cast<unsigned char>(original[i]);
        if (c == '\r' && i + 1 < original.size() && original[i + 1] == '\n') {   // an edit control counts "\r\n" as one character
            ++units;
            ++chars;
            i += 2;
            continue;
        }
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += len == 4 ? 2 : 1;
        ++chars;
        i += len;
    }
    return chars;
}

class AnalysisWindow : public CompanionWindow {
public:
    explicit AnalysisWindow(CompanionHost& host) : host_(host)
    {
        wake_target_ = std::make_shared<std::atomic<AnalysisWindow*>>(this);
        app::AnalysisWindowHost model_host;
        CompanionHost copy = host;
        model_host.seek = [copy](int64_t t) { if (copy.analysis_seek) copy.analysis_seek(t); };
        std::shared_ptr<std::atomic<AnalysisWindow*>> target = wake_target_;
        model_host.wake = [target] {   // worker thread: hand the poll to the UI thread
            struct Call { std::shared_ptr<std::atomic<AnalysisWindow*>> target; };
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                Call* call = static_cast<Call*>(data);
                if (AnalysisWindow* w = call->target->load()) { w->model_->poll(); w->sync(); }
                delete call;
                return G_SOURCE_REMOVE;
            }, new Call{ target }, nullptr);
        };
        model_.reset(new app::AnalysisWindowModel(*host.controller, model_host));
    }

    ~AnalysisWindow() override
    {
        wake_target_->store(nullptr);
        close();
        graph_.reset();
        model_.reset();
    }

    bool create()
    {
        const int percent = ScaleStyle::instance().percent();
        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window_), "MIDIBottleneck Player \xE2\x80\x94 Analysis");
        gtk_window_set_default_size(GTK_WINDOW(window_), app::scale_metric(kDefaultClientWidth, percent), app::scale_metric(kDefaultClientHeight, percent));
        build_controls();
        if (host_.main_window) {   // v1: beside the main window (right edge + 12)
            gint mx = 0, my = 0, mw = 0, mh = 0;
            gtk_window_get_position(GTK_WINDOW(host_.main_window), &mx, &my);
            gtk_window_get_size(GTK_WINDOW(host_.main_window), &mw, &mh);
            GdkScreen* screen = gtk_widget_get_screen(window_);
            GdkRectangle work = {};
            gdk_monitor_get_workarea(gdk_display_get_primary_monitor(gdk_screen_get_display(screen)), &work);
            const int sw = work.width;
            const int win_w = app::scale_metric(kDefaultClientWidth, percent);
            int x = mx + mw + 12;
            if (x + win_w > sw) x = std::max(0, mx + 36);
            gtk_window_move(GTK_WINDOW(window_), x, std::max(0, my));
        }
        gtk_widget_show_all(window_);
        update_busy_visibility();
        model_->start();
        sync();
        return true;
    }

    GtkWidget* widget() const override { return window_; }
    bool alive() const override { return alive_; }
    void on_refresh() override
    {
        if (!alive_) return;
        model_->on_refresh();
        sync();
    }
    void on_scale_changed() override
    {
        if (!alive_) return;
        graph_->scale_changed();
        const int percent = ScaleStyle::instance().percent();
        gtk_paned_set_position(GTK_PANED(paned_), app::scale_metric(kSplitterDistance, percent));
        gtk_widget_set_size_request(inspection_scroll_, -1, app::scale_metric(kInspectionHeight, percent));
        int w = 0, h = 0;
        gtk_window_get_size(GTK_WINDOW(window_), &w, &h);
        const double factor = percent / 100.0;
        gtk_window_resize(GTK_WINDOW(window_), static_cast<int>(std::lround(canonical_w_ * factor)), static_cast<int>(std::lround(canonical_h_ * factor)));
        apply_summary(true);
    }
    void close() override
    {
        if (!alive_) return;
        alive_ = false;
        if (timer_) { g_source_remove(timer_); timer_ = 0; }
        if (model_) model_->close();
        if (graph_) graph_->detach();
        GtkWidget* w = window_;
        window_ = nullptr;
        if (w) gtk_widget_destroy(w);
    }

    app::AnalysisWindowModel& model() { return *model_; }

private:
    // ---- construction
    void build_controls()
    {
        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(outer), 4);
        gtk_container_add(GTK_CONTAINER(window_), outer);
        GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);
        auto add = [&](GtkWidget* w) { gtk_box_pack_start(GTK_BOX(header), w, FALSE, FALSE, 0); return w; };

        reset_ = add(gtk_button_new_with_label("Reset zoom"));
        follow_label_ = add(make_label("Follow:"));
        follow_ = add(gtk_combo_box_text_new());
        for (const char* s : { "Off", "Playback timeline", "MIDI output" }) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(follow_), s);
        gtk_combo_box_set_active(GTK_COMBO_BOX(follow_), 0);
        resolution_label_ = add(make_label("Resolution:"));
        resolution_ = add(gtk_combo_box_text_new());
        stats_check_ = add(gtk_check_button_new_with_label("Playback statistics"));
        channels_ = add(gtk_button_new_with_label("Channels\xE2\x80\xA6"));
        status_ = add(make_label("Recalculating analysis\xE2\x80\xA6"));
        progress_ = add(gtk_progress_bar_new());
        gtk_widget_set_size_request(progress_, app::scale_metric(105, ScaleStyle::instance().percent()), -1);
        gtk_widget_set_valign(progress_, GTK_ALIGN_CENTER);
        cancel_ = add(gtk_button_new_with_label("Cancel"));
        GtkWidget* spacer = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);
        menu_button_ = gtk_button_new_with_label("\xE2\x89\xA1");
        set_tooltip(menu_button_, "Menu: show event counts, project forced overrides, always on top");
        gtk_box_pack_start(GTK_BOX(header), menu_button_, FALSE, FALSE, 0);
        gtk_widget_set_no_show_all(status_, TRUE);
        gtk_widget_set_no_show_all(progress_, TRUE);
        gtk_widget_set_no_show_all(cancel_, TRUE);
        if (!host_.open_channels) gtk_widget_set_no_show_all(channels_, TRUE);

        paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(outer), paned_, TRUE, TRUE, 0);
        // the report
        summary_view_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(summary_view_), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(summary_view_), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(summary_view_), GTK_WRAP_WORD_CHAR);
        add_class(summary_view_, "mono");
        summary_buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(summary_view_));
        gtk_text_buffer_create_tag(summary_buffer_, "heading", "weight", PANGO_WEIGHT_BOLD, "foreground", "#2A5C96", nullptr);
        GtkWidget* summary_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(summary_scroll), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(summary_scroll), summary_view_);
        gtk_paned_pack1(GTK_PANED(paned_), summary_scroll, FALSE, FALSE);
        gtk_widget_set_size_request(summary_scroll, app::scale_metric(330, ScaleStyle::instance().percent()), -1);
        // the graph, the inspection box and Seek to pin
        GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        graph_.reset(new WorkloadGraphWidget(model_.get(), [this] { if (alive_) sync(); }));
        gtk_box_pack_start(GTK_BOX(right), graph_->widget(), TRUE, TRUE, 0);
        GtkWidget* bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        inspection_view_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(inspection_view_), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(inspection_view_), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(inspection_view_), GTK_WRAP_WORD_CHAR);
        add_class(inspection_view_, "mono");
        inspection_buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(inspection_view_));
        inspection_scroll_ = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(inspection_scroll_), GTK_SHADOW_IN);
        gtk_container_add(GTK_CONTAINER(inspection_scroll_), inspection_view_);
        gtk_widget_set_size_request(inspection_scroll_, -1, app::scale_metric(kInspectionHeight, ScaleStyle::instance().percent()));
        gtk_box_pack_start(GTK_BOX(bottom), inspection_scroll_, TRUE, TRUE, 0);
        seek_ = gtk_button_new_with_label("Seek to pin");
        gtk_widget_set_valign(seek_, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(bottom), seek_, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(right), bottom, FALSE, FALSE, 0);
        gtk_paned_pack2(GTK_PANED(paned_), right, TRUE, FALSE);
        gtk_paned_set_position(GTK_PANED(paned_), app::scale_metric(kSplitterDistance, ScaleStyle::instance().percent()));
        gtk_text_buffer_set_text(inspection_buffer_, app::kInspectionPlaceholder, -1);
        inspection_applied_ = app::kInspectionPlaceholder;

        // menu
        menu_ = gtk_menu_new();
        mi_counts_ = gtk_check_menu_item_new_with_label("Show event counts");
        mi_overrides_ = gtk_check_menu_item_new_with_label("Project forced overrides");
        mi_top_ = gtk_check_menu_item_new_with_label("Always on top");
        for (GtkWidget* mi : { mi_counts_, mi_overrides_, mi_top_ }) gtk_menu_shell_append(GTK_MENU_SHELL(menu_), mi);
        gtk_widget_show_all(menu_);
        connect(mi_counts_, "toggled", [this] { if (!applying_) { model_->set_show_event_counts(!model_->show_event_counts()); sync(); } });
        connect(mi_overrides_, "toggled", [this] { if (!applying_) { model_->set_project_forced_overrides(!model_->project_forced_overrides()); sync(); } });
        connect(mi_top_, "toggled", [this] {
            if (applying_) return;
            topmost_ = !topmost_;
            gtk_window_set_keep_above(GTK_WINDOW(window_), topmost_);
        });
        connect(menu_button_, "clicked", [this] {
            applying_ = true;
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi_counts_), model_->show_event_counts());
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi_overrides_), model_->project_forced_overrides());
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi_top_), topmost_);
            applying_ = false;
            gtk_menu_popup_at_widget(GTK_MENU(menu_), menu_button_, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, nullptr);
        });

        // signals
        connect(reset_, "clicked", [this] { model_->reset_zoom(); graph_->invalidate(); sync(); });
        connect(follow_, "changed", [this] {
            if (applying_) return;
            model_->set_follow(static_cast<app::FollowTarget>(gtk_combo_box_get_active(GTK_COMBO_BOX(follow_))));
            graph_->invalidate();
            sync();
        });
        connect(resolution_, "changed", [this] {
            if (applying_) return;
            model_->select_resolution(gtk_combo_box_get_active(GTK_COMBO_BOX(resolution_)));
            if (model_->custom_prompt_pending()) {
                sync();
                if (!run_custom_prompt()) model_->cancel_custom_resolution();
            }
            sync();
        });
        connect(stats_check_, "toggled", [this] {
            if (applying_) return;
            model_->set_playback_statistics_visible(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(stats_check_)));
            graph_->invalidate();
            gtk_widget_queue_draw(graph_->widget());
            sync();
        });
        connect(channels_, "clicked", [this] { if (host_.open_channels) host_.open_channels(); });
        connect(cancel_, "clicked", [this] { model_->cancel_calculation(); sync(); });
        connect(seek_, "clicked", [this] { model_->seek_to_pin(); sync(); });
        connect_delete(window_, [this] { close(); return true; });
        connect_event(window_, "key-press-event", [this](GdkEvent* e) {
            GdkEventKey* k = reinterpret_cast<GdkEventKey*>(e);
            if (k->keyval == GDK_KEY_F1 && host_.show_help) { host_.show_help(static_cast<int>(app::HelpTopic::Analysis)); return true; }
            if (k->keyval == GDK_KEY_space && (k->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) == 0) {
                GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window_));
                if (focus && (GTK_IS_COMBO_BOX(focus) || GTK_IS_BUTTON(focus))) return false;
                if (host_.toggle_transport) host_.toggle_transport();
                return true;
            }
            return false;
        });
        const int percent = ScaleStyle::instance().percent();
        canonical_w_ = kDefaultClientWidth;
        canonical_h_ = kDefaultClientHeight;
        (void)percent;
    }

    // ---- the custom-resolution prompt (v1 PromptForCustomResolution)
    bool run_custom_prompt()
    {
        GtkWidget* dialog = gtk_dialog_new_with_buttons(app::kCustomResolutionTitle, GTK_WINDOW(window_), GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK,
            "Cancel", GTK_RESPONSE_CANCEL, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
        GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(area), 12);
        gtk_box_set_spacing(GTK_BOX(area), 8);
        const int64_t initial = model_->resolution().custom_microseconds() > 0 ? model_->resolution().custom_microseconds() : app::kCustomResolutionDefaultMicroseconds;
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* spin = gtk_spin_button_new_with_range(0.001, 86400000.0, 1.0);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 3);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), initial / 1000.0);
        gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
        gtk_box_pack_start(GTK_BOX(row), make_label(app::kCustomResolutionLabel), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), spin, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), make_label("ms"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(area), row, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(area), make_label(app::kCustomResolutionNote), FALSE, FALSE, 0);
        gtk_widget_show_all(dialog);
        bool accepted = false;
        while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
            gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
            char text[64];
            g_ascii_formatd(text, sizeof(text), "%.3f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin)));
            std::string error;
            if (model_->apply_custom_resolution(text, error)) { accepted = true; break; }
            GtkWidget* box = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s", error.c_str());
            gtk_window_set_title(GTK_WINDOW(box), app::kCustomResolutionInvalidTitle);
            gtk_dialog_run(GTK_DIALOG(box));
            gtk_widget_destroy(box);
        }
        gtk_widget_destroy(dialog);
        return accepted;
    }

    // ---- model -> controls
    void apply_summary(bool force)
    {
        const std::string& text = model_->summary_text();
        if (!force && text == summary_applied_) return;
        summary_applied_ = text;
        GtkAdjustment* v = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(summary_view_));
        const double scroll = gtk_adjustment_get_value(v);
        const std::string clean = strip_cr(text);
        gtk_text_buffer_set_text(summary_buffer_, clean.c_str(), -1);
        for (const app::SummaryHeadingSpan& span : app::find_summary_headings(text)) {
            GtkTextIter a, b;
            const long start = char_offset_for_utf16(text, span.start);
            const long end = char_offset_for_utf16(text, span.start + span.length);
            gtk_text_buffer_get_iter_at_offset(summary_buffer_, &a, static_cast<gint>(start));
            gtk_text_buffer_get_iter_at_offset(summary_buffer_, &b, static_cast<gint>(end));
            gtk_text_buffer_apply_tag_by_name(summary_buffer_, "heading", &a, &b);
        }
        gtk_adjustment_set_value(v, scroll);
    }

    void apply_resolution()
    {
        const app::ResolutionSelector& r = model_->resolution();
        if (r.items() != resolution_items_) {
            resolution_items_ = r.items();
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(resolution_));
            for (const std::string& s : resolution_items_) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(resolution_), s.c_str());
        }
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(resolution_)) != r.selected_index()) gtk_combo_box_set_active(GTK_COMBO_BOX(resolution_), r.selected_index());
        set_tooltip(resolution_, r.tooltip());
    }

    void update_busy_visibility()
    {
        const app::AnalysisBusyView& b = model_->busy_view();
        gtk_widget_set_visible(status_, b.status_visible);
        gtk_widget_set_visible(progress_, b.progress_visible);
        gtk_widget_set_visible(cancel_, b.cancel_visible);
    }

    void apply_busy()
    {
        const app::AnalysisBusyView& b = model_->busy_view();
        set_text(GTK_LABEL(status_), b.status_text);
        update_busy_visibility();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), b.progress_permille / 1000.0);
    }

    // Applies everything the model changed since the last call and re-arms the model timer.
    void sync()
    {
        if (!alive_) return;
        if (syncing_) { resync_ = true; return; }
        syncing_ = true;
        applying_ = true;
        do {
            resync_ = false;
            if (model_->title_revision() != title_rev_) {
                title_rev_ = model_->title_revision();
                gtk_window_set_title(GTK_WINDOW(window_), model_->title().c_str());
            }
            if (model_->attach_revision() != attach_rev_) {
                attach_rev_ = model_->attach_revision();
                const bool on = model_->attached();
                for (GtkWidget* w : { reset_, follow_label_, follow_, resolution_label_, resolution_, stats_check_, channels_, status_, cancel_, summary_view_, inspection_view_, graph_->widget() })
                    gtk_widget_set_sensitive(w, on);
                graph_->invalidate();
            }
            if (model_->summary_revision() != summary_rev_) {
                summary_rev_ = model_->summary_revision();
                apply_summary(false);
            }
            if (model_->inspection_revision() != inspection_rev_) {
                inspection_rev_ = model_->inspection_revision();
                const std::string text = strip_cr(model_->inspection_text());
                if (text != inspection_applied_) {
                    inspection_applied_ = text;
                    gtk_text_buffer_set_text(inspection_buffer_, text.c_str(), -1);
                }
            }
            gtk_widget_set_sensitive(seek_, model_->attached() && model_->seek_to_pin_enabled());
            if (model_->resolution_revision() != resolution_rev_) {
                resolution_rev_ = model_->resolution_revision();
                apply_resolution();
            }
            if (model_->busy_revision() != busy_rev_) {
                busy_rev_ = model_->busy_revision();
                apply_busy();
            }
            graph_->invalidate();
        } while (resync_);
        applying_ = false;
        syncing_ = false;
        if (timer_) { g_source_remove(timer_); timer_ = 0; }
        const int wait = model_->next_wake_milliseconds();
        if (wait >= 0)
            timer_ = g_timeout_add(static_cast<guint>(std::max(wait, 1)), [](gpointer data) -> gboolean {
                AnalysisWindow* self = static_cast<AnalysisWindow*>(data);
                self->timer_ = 0;
                if (self->alive_) { self->model_->poll(); self->sync(); }
                return G_SOURCE_REMOVE;
            }, this);
    }

    CompanionHost host_;
    std::unique_ptr<app::AnalysisWindowModel> model_;
    std::unique_ptr<WorkloadGraphWidget> graph_;
    std::shared_ptr<std::atomic<AnalysisWindow*>> wake_target_;
    GtkWidget* window_ = nullptr;
    GtkWidget *reset_ = nullptr, *follow_label_ = nullptr, *follow_ = nullptr, *resolution_label_ = nullptr, *resolution_ = nullptr;
    GtkWidget *stats_check_ = nullptr, *channels_ = nullptr, *status_ = nullptr, *progress_ = nullptr, *cancel_ = nullptr, *menu_button_ = nullptr;
    GtkWidget *paned_ = nullptr, *summary_view_ = nullptr, *inspection_view_ = nullptr, *inspection_scroll_ = nullptr, *seek_ = nullptr;
    GtkTextBuffer *summary_buffer_ = nullptr, *inspection_buffer_ = nullptr;
    GtkWidget *menu_ = nullptr, *mi_counts_ = nullptr, *mi_overrides_ = nullptr, *mi_top_ = nullptr;
    bool alive_ = true, applying_ = false, syncing_ = false, resync_ = false, topmost_ = false;
    guint timer_ = 0;
    double canonical_w_ = kDefaultClientWidth, canonical_h_ = kDefaultClientHeight;
    uint64_t title_rev_ = 0, attach_rev_ = 0, summary_rev_ = 0, inspection_rev_ = 0, resolution_rev_ = 0, busy_rev_ = 0;
    std::string summary_applied_, inspection_applied_;
    std::vector<std::string> resolution_items_;
};

}   // namespace

std::unique_ptr<CompanionWindow> create_analysis_window(CompanionHost& host)
{
    std::unique_ptr<AnalysisWindow> window(new AnalysisWindow(host));
    if (!window->create()) return nullptr;
    return std::unique_ptr<CompanionWindow>(window.release());
}

}   // namespace gtkui
}   // namespace midibn
