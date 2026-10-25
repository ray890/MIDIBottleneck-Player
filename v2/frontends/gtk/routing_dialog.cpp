// The Track routing editor (v1 TrackRoutingForm) in GTK 3: a modal, resizable dialog that edits a session-only route and hands the
// result to the controller. All decisions live in app::RoutingDialogModel; this file is widgets, layout and the planner's
// worker thread (one job at a time; a result for an older generation is discarded by the model).
#include "companion.h"

#include "midibn/app/help_text.h"
#include "midibn/app/routing_dialog_model.h"
#include "midibn/app/ui_mappings.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace midibn {
namespace gtkui {

namespace {

std::string utf8(const std::u16string& text)
{
    gchar* s = g_utf16_to_utf8(reinterpret_cast<const guint16*>(text.data()), static_cast<glong>(text.size()), nullptr, nullptr, nullptr);
    std::string out = s ? s : "";
    g_free(s);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return out;
}

// The planner thread's hand-off to the window.
struct Mailbox {
    std::mutex mutex;
    bool has = false;
    bool closed = false;
    uint64_t generation = 0;
    std::shared_ptr<const routing::CompiledTrackRoute> route;
    bool failed = false;
    std::string message;
};

struct Dialog {
    CompanionHost* host = nullptr;
    std::shared_ptr<const Song> song;
    std::unique_ptr<app::RoutingDialogModel> model;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    bool running = false;
    bool syncing = false;

    GtkWidget* window = nullptr;
    GtkWidget *tier = nullptr, *capacity = nullptr, *percussion = nullptr, *usable[16] = {};
    GtkWidget *destination = nullptr, *caption = nullptr, *preview = nullptr, *status = nullptr, *confirm = nullptr, *tree = nullptr;
    GtkWidget *apply = nullptr, *original = nullptr;
    GtkListStore* store = nullptr;
};

const char* row_color(routing::RouteRowStyle style)
{
    switch (style) {
    case routing::RouteRowStyle::Unresolved: return "#FFE4E1";   // MistyRose
    case routing::RouteRowStyle::Omitted: return "#F08080";      // LightCoral
    case routing::RouteRowStyle::Warning: return "#FFFACD";      // LemonChiffon
    default: return nullptr;
    }
}

// ---- view updates -------------------------------------------------------------------------------

void refresh_state(Dialog& d)
{
    const app::RoutingDialogModel& m = *d.model;
    const bool was = d.syncing;
    d.syncing = true;
    const std::string status = utf8(m.status_text());
    if (m.status_is_error()) {
        gchar* e = g_markup_escape_text(status.c_str(), -1);
        gchar* markup = g_strdup_printf("<span foreground=\"#8B0000\">%s</span>", e);
        gtk_label_set_markup(GTK_LABEL(d.status), markup);
        g_free(e);
        g_free(markup);
    }
    else gtk_label_set_text(GTK_LABEL(d.status), status.c_str());
    const bool visible = m.lossy_confirmation_visible();
    gtk_widget_set_visible(d.confirm, visible);
    gtk_widget_set_sensitive(d.confirm, visible);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d.confirm), m.lossy_confirmation_checked());
    gtk_widget_set_sensitive(d.apply, m.apply_enabled());
    gtk_widget_set_sensitive(d.original, m.original_enabled());
    gtk_widget_set_sensitive(d.destination, m.destination_enabled());
    gtk_combo_box_set_active(GTK_COMBO_BOX(d.destination), m.destination_index());
    d.syncing = was;
}

void refresh_option_controls(Dialog& d)
{
    const app::RoutingDialogModel& m = *d.model;
    d.syncing = true;
    gtk_combo_box_set_active(GTK_COMBO_BOX(d.tier), m.tier_index());
    gtk_combo_box_set_active(GTK_COMBO_BOX(d.percussion), m.percussion_index());
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d.capacity), m.capacity());
    for (int c = 0; c < 16; ++c) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d.usable[c]), m.usable()[static_cast<size_t>(c)]);
    d.syncing = false;
}

// A new plan (or failure) arrived: rows, preview, the bold "used" channel captions.
void refresh_plan(Dialog& d)
{
    const app::RoutingDialogModel& m = *d.model;
    d.syncing = true;
    gtk_list_store_clear(d.store);
    for (const app::RoutingDialogRow& r : m.rows()) {
        GtkTreeIter it;
        gtk_list_store_append(d.store, &it);
        gtk_list_store_set(d.store, &it, 0, utf8(r.row.lane_text).c_str(), 1, utf8(r.row.patch_text).c_str(), 2, utf8(r.row.counts_text).c_str(),
            3, utf8(r.row.destination_text).c_str(), 4, utf8(r.row.status_text).c_str(), 5, row_color(r.row.style),
            6, utf8(r.patch_tooltip).c_str(), -1);
    }
    if (m.selected_row() >= 0) {
        GtkTreePath* path = gtk_tree_path_new_from_indices(m.selected_row(), -1);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(d.tree)), path);
        gtk_tree_path_free(path);
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d.preview)), utf8(m.preview()).c_str(), -1);
    for (int c = 0; c < 16; ++c) {
        GtkWidget* label = gtk_bin_get_child(GTK_BIN(d.usable[c]));
        if (!GTK_IS_LABEL(label)) continue;
        char text[24];
        std::snprintf(text, sizeof(text), m.used()[static_cast<size_t>(c)] ? "<b>%d</b>" : "%d", c + 1);
        gtk_label_set_markup(GTK_LABEL(label), text);
    }
    d.syncing = false;
    refresh_state(d);
}

// ---- planner thread -----------------------------------------------------------------------------

void handle_plan_done(Dialog& d);

void post_wake(const std::shared_ptr<Mailbox>& box, Dialog* dialog)
{
    struct Wake { std::shared_ptr<Mailbox> box; Dialog* dialog; };
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        Wake* w = static_cast<Wake*>(data);
        bool closed;
        { std::lock_guard<std::mutex> lock(w->box->mutex); closed = w->box->closed; }
        if (!closed) handle_plan_done(*w->dialog);   // the dialog sets `closed` before it goes away (UI thread)
        delete w;
        return G_SOURCE_REMOVE;
    }, new Wake{ box, dialog }, nullptr);
}

void deliver(const std::shared_ptr<Mailbox>& box, Dialog* dialog, uint64_t generation, std::shared_ptr<const routing::CompiledTrackRoute> route,
    bool failed, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(box->mutex);
        box->generation = generation;
        box->route = std::move(route);
        box->failed = failed;
        box->message = message;
        box->has = true;
        if (box->closed) return;
    }
    post_wake(box, dialog);
}

void plan_job(const std::shared_ptr<Mailbox>& box, Dialog* dialog, const routing::RoutingSong& song, const routing::TrackRoutingSettings& settings,
    uint64_t generation)
{
    try {
        deliver(box, dialog, generation, routing::compile_route(song, settings), false, std::string());
    }
    catch (const models::ModelException& exception) {
        deliver(box, dialog, generation, nullptr, true, exception.message);
    }
    catch (const std::bad_alloc&) {
        deliver(box, dialog, generation, nullptr, true, "There is not enough memory to calculate the routing plan.");
    }
    catch (...) {
        deliver(box, dialog, generation, nullptr, true, "The routing plan could not be calculated.");
    }
}

void start_job(Dialog& d)
{
    if (d.running || !d.model->plan_pending()) return;
    const uint64_t generation = d.model->generation();
    routing::RoutingSong song = d.model->song();
    routing::TrackRoutingSettings settings = d.model->settings();
    std::shared_ptr<Mailbox> box = d.mailbox;
    std::shared_ptr<const Song> keep = d.song;   // the planner reads this song's store
    Dialog* dialog = &d;
    auto body = [box, keep, song, settings, generation, dialog]() { plan_job(box, dialog, song, settings, generation); };
    platform::PlatformError error;
    std::unique_ptr<platform::Thread> thread;
    if (d.host->controller->services())
        thread = d.host->controller->services()->start_thread(body, platform::ThreadPriority::Normal, "Track routing planner", error);
    if (thread) {
        d.running = true;
        thread->detach();   // the job owns what it uses; closing the dialog never waits for it
        return;
    }
    d.running = true;   // no worker thread available: compute here
    body();
}

void handle_plan_done(Dialog& d)
{
    uint64_t generation = 0;
    std::shared_ptr<const routing::CompiledTrackRoute> route;
    bool failed = false, has = false;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(d.mailbox->mutex);
        has = d.mailbox->has;
        if (has) {
            generation = d.mailbox->generation;
            route = std::move(d.mailbox->route);
            failed = d.mailbox->failed;
            message = d.mailbox->message;
            d.mailbox->has = false;
        }
    }
    if (!has) return;
    d.running = false;
    const bool changed = failed ? d.model->plan_failed(generation, message) : d.model->plan_ready(generation, route);
    if (changed) refresh_plan(d);
    if (d.model->plan_pending()) start_job(d);   // the settings changed again while this one ran
}

// A setting changed: show the pending state and plan again.
void after_change(Dialog& d)
{
    refresh_state(d);
    start_job(d);
}

GtkWidget* label_for(const std::u16string& text, bool bold = false)
{
    GtkWidget* l = make_label(utf8(text).c_str());
    if (bold) {
        gchar* e = g_markup_escape_text(utf8(text).c_str(), -1);
        gchar* m = g_strdup_printf("<b>%s</b>", e);
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(e);
        g_free(m);
    }
    return l;
}

void build(Dialog& d)
{
    namespace T = app;
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(d.window));
    gtk_container_set_border_width(GTK_CONTAINER(area), 10);
    gtk_box_set_spacing(GTK_BOX(area), 6);

    GtkWidget* intro = label_for(T::RoutingDialogTexts::introduction());
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 160);
    gtk_box_pack_start(GTK_BOX(area), intro, FALSE, FALSE, 0);

    // options, row 1: tier, melodic destinations, percussion
    GtkWidget* row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row1), make_label("Automatic tier:"), FALSE, FALSE, 0);
    d.tier = gtk_combo_box_text_new();
    for (int i = 0; i < 3; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d.tier), utf8(T::RoutingDialogTexts::tier_item(i)).c_str());
    gtk_box_pack_start(GTK_BOX(row1), d.tier, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row1), make_label("Melodic destinations:"), FALSE, FALSE, 10);
    d.capacity = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_box_pack_start(GTK_BOX(row1), d.capacity, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row1), make_label("Percussion:"), FALSE, FALSE, 10);
    d.percussion = gtk_combo_box_text_new();
    for (int i = 0; i < 2; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d.percussion), utf8(T::RoutingDialogTexts::percussion_item(i)).c_str());
    gtk_box_pack_start(GTK_BOX(row1), d.percussion, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(area), row1, FALSE, FALSE, 0);

    // options, row 2: the 16 usable-channel boxes
    GtkWidget* row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(row2), make_label("Usable output channels:"), FALSE, FALSE, 0);
    GtkWidget* flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 16);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 0);
    for (int c = 0; c < 16; ++c) {
        char text[8];
        std::snprintf(text, sizeof(text), "%d", c + 1);
        d.usable[c] = gtk_check_button_new_with_label(text);
        gtk_flow_box_insert(GTK_FLOW_BOX(flow), d.usable[c], -1);
    }
    gtk_box_pack_start(GTK_BOX(row2), flow, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(area), row2, FALSE, FALSE, 0);

    // the split: lanes on the left, preview on the right
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(area), paned, TRUE, TRUE, 0);
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_pack_start(GTK_BOX(left), label_for(T::RoutingDialogTexts::lanes_heading(), true), FALSE, FALSE, 0);
    d.store = gtk_list_store_new(7, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    d.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d.store));
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(d.tree), GTK_TREE_VIEW_GRID_LINES_BOTH);
    gtk_widget_set_has_tooltip(d.tree, TRUE);
    static const char* const kHeadings[5] = { "MIDI track / channel", "Patch", "Events / notes", "Destination", "Status" };
    static const int kWidths[5] = { 155, 155, 88, 82, 80 };
    for (int i = 0; i < 5; ++i) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(kHeadings[i], r, "text", i, "cell-background", 5, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_min_width(col, kWidths[i]);
        if (i == 4) gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(d.tree), col);
    }
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), d.tree);
    gtk_box_pack_start(GTK_BOX(left), scroll, TRUE, TRUE, 0);
    GtkWidget* lane_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d.caption = label_for(T::RoutingDialogTexts::lane_caption());
    d.destination = gtk_combo_box_text_new();
    for (int i = 0; i <= 16; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d.destination), utf8(T::RoutingDialogTexts::destination_item(i)).c_str());
    set_tooltip(d.caption, utf8(T::RoutingDialogTexts::correction_help()));
    set_tooltip(d.destination, utf8(T::RoutingDialogTexts::correction_help()));
    gtk_box_pack_start(GTK_BOX(lane_row), d.caption, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(lane_row), d.destination, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(left), lane_row, FALSE, FALSE, 0);
    gtk_paned_pack1(GTK_PANED(paned), left, TRUE, FALSE);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_pack_start(GTK_BOX(right), label_for(T::RoutingDialogTexts::preview_heading(), true), FALSE, FALSE, 0);
    d.preview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(d.preview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(d.preview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d.preview), GTK_WRAP_NONE);
    add_class(d.preview, "mono");
    GtkWidget* pscroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(pscroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(pscroll), d.preview);
    gtk_box_pack_start(GTK_BOX(right), pscroll, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);
    const int percent = ScaleStyle::instance().percent();
    gtk_paned_set_position(GTK_PANED(paned), app::scale_metric(620, percent));

    // status, confirmation, buttons
    d.status = make_label("");
    gtk_label_set_line_wrap(GTK_LABEL(d.status), TRUE);
    gtk_box_pack_start(GTK_BOX(area), d.status, FALSE, FALSE, 0);
    d.confirm = gtk_check_button_new_with_label(utf8(T::RoutingDialogTexts::confirm_text()).c_str());
    GtkWidget* confirm_label = gtk_bin_get_child(GTK_BIN(d.confirm));
    if (GTK_IS_LABEL(confirm_label)) { gtk_label_set_line_wrap(GTK_LABEL(confirm_label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(confirm_label), 110); }
    gtk_box_pack_start(GTK_BOX(area), d.confirm, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(d.confirm, TRUE);
    d.original = gtk_dialog_add_button(GTK_DIALOG(d.window), "Use original routing", 1);
    d.apply = gtk_dialog_add_button(GTK_DIALOG(d.window), "Apply", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d.window), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d.window), GTK_RESPONSE_OK);

    // signals
    Dialog* dp = &d;
    connect(d.tier, "changed", [dp] { if (dp->syncing) return; dp->model->set_tier(gtk_combo_box_get_active(GTK_COMBO_BOX(dp->tier))); after_change(*dp); });
    connect(d.percussion, "changed", [dp] { if (dp->syncing) return; dp->model->set_percussion(gtk_combo_box_get_active(GTK_COMBO_BOX(dp->percussion))); after_change(*dp); });
    connect(d.capacity, "value-changed", [dp] {
        if (dp->syncing) return;
        const int before = dp->model->capacity();
        dp->model->set_capacity(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dp->capacity)));
        if (dp->model->capacity() != before) after_change(*dp);
    });
    for (int c = 0; c < 16; ++c)
        connect(d.usable[c], "toggled", [dp, c] {
            if (dp->syncing) return;
            dp->model->set_usable(c, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dp->usable[c])));
            after_change(*dp);
        });
    connect(d.destination, "changed", [dp] {
        if (dp->syncing) return;
        dp->model->set_selected_lane_destination(gtk_combo_box_get_active(GTK_COMBO_BOX(dp->destination)));
        after_change(*dp);
    });
    connect(d.confirm, "toggled", [dp] {
        if (dp->syncing) return;
        dp->model->set_lossy_confirmed(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dp->confirm)));
        refresh_state(*dp);
    });
    connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d.tree)), "changed", [dp] {
        if (dp->syncing) return;
        GtkTreeModel* model = nullptr;
        GtkTreeIter it;
        int row = -1;
        if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(dp->tree)), &model, &it)) {
            GtkTreePath* path = gtk_tree_model_get_path(model, &it);
            row = gtk_tree_path_get_indices(path)[0];
            gtk_tree_path_free(path);
        }
        dp->model->select_row(row);
        refresh_state(*dp);
    });
    auto tooltip_callback = +[](GtkWidget* tree, gint x, gint y, gboolean keyboard, GtkTooltip* tooltip, gpointer) -> gboolean {
        if (keyboard) return FALSE;
        gint bx = 0, by = 0;
        gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(tree), x, y, &bx, &by);
        GtkTreePath* path = nullptr;
        GtkTreeViewColumn* column = nullptr;
        if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), bx, by, &path, &column, nullptr, nullptr)) return FALSE;
        GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
        GtkTreeIter it;
        gboolean shown = FALSE;
        if (gtk_tree_model_get_iter(model, &it, path)) {
            GList* cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(tree));
            const int index = g_list_index(cols, column);
            g_list_free(cols);
            gchar* text = nullptr;
            gtk_tree_model_get(model, &it, index == 1 ? 6 : index, &text, -1);
            if (index == 1 && text && *text) { gtk_tooltip_set_text(tooltip, text); shown = TRUE; }
            g_free(text);
        }
        gtk_tree_path_free(path);
        return shown;
    };
    g_signal_connect(d.tree, "query-tooltip", G_CALLBACK(tooltip_callback), nullptr);
}

}   // namespace

void show_track_routing_dialog(CompanionHost& host, GtkWidget** window_out)
{
    app::PlayerController& controller = *host.controller;
    std::shared_ptr<const Song> song = controller.song();
    if (!song || !controller.track_routing_available()) return;

    Dialog d;
    d.host = &host;
    d.song = song;
    d.model.reset(new app::RoutingDialogModel(controller.routing_song(), controller.track_routing_settings(), controller.track_route() != nullptr,
        controller.culture()));
    d.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d.window), utf8(app::RoutingDialogTexts::title()).c_str());
    gtk_window_set_modal(GTK_WINDOW(d.window), TRUE);
    if (host.main_window) gtk_window_set_transient_for(GTK_WINDOW(d.window), GTK_WINDOW(host.main_window));
    const int percent = ScaleStyle::instance().percent();
    gtk_window_set_default_size(GTK_WINDOW(d.window), app::scale_metric(1050, percent), app::scale_metric(690, percent));
    build(d);
    refresh_option_controls(d);
    refresh_state(d);
    gtk_widget_show_all(d.window);
    gtk_widget_set_visible(d.confirm, d.model->lossy_confirmation_visible());
    if (window_out) *window_out = d.window;
    start_job(d);

    // The dialog's loop: Apply closes only when the model allows it (a plan may still be pending).
    for (;;) {
        const int response = gtk_dialog_run(GTK_DIALOG(d.window));
        if (response == GTK_RESPONSE_OK) {
            if (d.model->apply()) break;
            continue;
        }
        if (response == 1) {
            if (d.model->use_original()) break;
            continue;
        }
        break;
    }
    { std::lock_guard<std::mutex> lock(d.mailbox->mutex); d.mailbox->closed = true; }
    app::RoutingDialogOutcome outcome = d.model->outcome();
    if (window_out) *window_out = nullptr;
    gtk_widget_destroy(d.window);
    d.window = nullptr;

    // Hand the result to the controller (v1 ShowTrackRoutingDialog / ApplyTrackRoute).
    if (outcome.kind == app::RoutingDialogOutcome::None) return;
    std::string error;
    const bool ok = outcome.kind == app::RoutingDialogOutcome::UseOriginal
        ? controller.apply_track_route(nullptr, false, nullptr, error)
        : controller.apply_track_route(outcome.route, outcome.explicitly_confirmed, &outcome.settings, error);
    if (!ok) {
        GtkWidget* box = new_message_dialog(host.main_window ? GTK_WINDOW(host.main_window) : nullptr, GTK_MESSAGE_ERROR,
            app::join_error_text(app::explain_error(error, "The track routing could not be applied.")));
        gtk_window_set_title(GTK_WINDOW(box), "Track routing");
        gtk_dialog_run(GTK_DIALOG(box));
        gtk_widget_destroy(box);
    }
}

}   // namespace gtkui
}   // namespace midibn
