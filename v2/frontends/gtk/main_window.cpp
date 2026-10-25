#include "main_window.h"
#include "settings_file.h"

#include "midibn/app/help_text.h"
#include "midibn/app/rate_model.h"
#include "midibn/app/statistics.h"
#include "midibn/app/statistics_tips.h"
#include "midibn/app/ui_mappings.h"
#include "midibn/format/display_format.h"
#include "midibn/format/net_format.h"

#if defined(MIDIBN_HAVE_ALSA)
#include "alsa_midi_output.h"
#endif

#include <sched.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace midibn {
namespace gtkui {

namespace {

int64_t now_us() { return g_get_monotonic_time(); }

bool has_midi_extension(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& ch : ext) ch = static_cast<char>(g_ascii_tolower(ch));
    return ext == ".mid" || ext == ".midi" || ext == ".rmi";
}

// One physical processor core only (DD-112): count the distinct (physical id, core id) pairs of /proc/cpuinfo.
// Unknown (no file): exactly one logical processor.
bool one_physical_core()
{
    std::ifstream in("/proc/cpuinfo");
    std::set<std::string> cores;
    std::string line, physical = "0";
    int logical = 0;
    bool any = false;
    while (std::getline(in, line)) {
        if (line.compare(0, 9, "processor") == 0) ++logical;
        else if (line.compare(0, 11, "physical id") == 0) physical = line.substr(line.find(':') + 1);
        else if (line.compare(0, 7, "core id") == 0) { cores.insert(physical + "/" + line.substr(line.find(':') + 1)); any = true; }
    }
    if (any) return cores.size() == 1;
    return logical == 1;
}

int usable_processors()
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) return std::max(1, CPU_COUNT(&set));
    return 1;
}

std::string pressure_markup(const std::string& text, format::Rgb c)
{
    char head[64];
    std::snprintf(head, sizeof(head), "<span foreground=\"#%02x%02x%02x\" weight=\"bold\">", c.r, c.g, c.b);
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string out = std::string(head) + escaped + "</span>";
    g_free(escaped);
    return out;
}

const int kScales[6] = { 50, 75, 100, 125, 150, 200 };

// A tree view draws its column headings into a window of its own, which gtk_widget_draw (the picture path) skips: draw the
// heading buttons of every tree view below `widget` by hand.
void draw_tree_headers(GtkWidget* root, GtkWidget* widget, cairo_t* cr)
{
    if (GTK_IS_TREE_VIEW(widget)) {
        GList* columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(widget));
        gint tx = 0, ty = 0;
        gtk_widget_translate_coordinates(widget, root, 0, 0, &tx, &ty);
        cairo_save(cr);
        cairo_rectangle(cr, tx, ty, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
        cairo_clip(cr);
        for (GList* it = columns; it; it = it->next) {
            GtkWidget* button = gtk_tree_view_column_get_button(GTK_TREE_VIEW_COLUMN(it->data));
            gint x = 0, y = 0;
            if (button && gtk_widget_translate_coordinates(button, root, 0, 0, &x, &y)) {
                cairo_save(cr);
                cairo_translate(cr, x, y);
                gtk_widget_draw(button, cr);
                cairo_restore(cr);
            }
        }
        cairo_restore(cr);
        g_list_free(columns);
        return;
    }
    if (GTK_IS_CONTAINER(widget)) {
        gtk_container_forall(GTK_CONTAINER(widget), [](GtkWidget* child, gpointer data) {
            void** args = static_cast<void**>(data);
            draw_tree_headers(static_cast<GtkWidget*>(args[0]), child, static_cast<cairo_t*>(args[1]));
        }, std::array<void*, 2>{ { root, cr } }.data());
    }
}

}   // namespace

// ------------------------------------------------------------------ the scrub-or-type number field

namespace {

struct ScrubEntry {
    GtkWidget* entry = nullptr;
    app::NumericFieldView view;
    bool typing = false, armed = false, scrubbing = false, applying = false;
    double press_x = 0;
    int64_t start_value = 0, pixels = 0;
    format::NumberCulture culture;
    std::function<void(int64_t)> commit;
    std::function<int64_t(int64_t, int64_t)> mapper;   // pointer travel -> value for the mapped fields

    void init(int width_chars)
    {
        entry = gtk_entry_new();
        gtk_entry_set_alignment(GTK_ENTRY(entry), 1.0f);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), width_chars);
        gtk_entry_set_max_width_chars(GTK_ENTRY(entry), width_chars);
        gtk_widget_add_events(entry, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
        connect(entry, "changed", [this] { if (!applying) typing = true; });
        connect(entry, "activate", [this] { commit_text(); });
        connect_event(entry, "focus-in-event", [this](GdkEvent*) {
            applying = true;
            gtk_entry_set_text(GTK_ENTRY(entry), std::to_string(view.value).c_str());
            applying = false;
            return false;
        });
        connect_event(entry, "focus-out-event", [this](GdkEvent*) {
            if (typing) commit_text();
            typing = false;
            show_value();
            return false;
        });
        connect_event(entry, "button-press-event", [this](GdkEvent* e) {
            GdkEventButton* b = reinterpret_cast<GdkEventButton*>(e);
            if (b->button == 1 && b->type == GDK_BUTTON_PRESS && gtk_widget_get_sensitive(entry)) {
                armed = true;
                scrubbing = false;
                press_x = b->x_root;
                start_value = view.value;
                pixels = 0;
            }
            return false;
        });
        connect_event(entry, "motion-notify-event", [this](GdkEvent* e) {
            GdkEventMotion* m = reinterpret_cast<GdkEventMotion*>(e);
            if (!armed) return false;
            const double dx = m->x_root - press_x;
            if (!scrubbing && std::fabs(dx) > app::ScrubField::kDragThreshold) {
                scrubbing = true;
                typing = false;
                gtk_widget_grab_focus(gtk_widget_get_toplevel(entry));
            }
            if (!scrubbing) return false;
            const bool fine = (m->state & GDK_SHIFT_MASK) != 0;
            pixels = static_cast<int64_t>(dx);
            int64_t value;
            if (view.scrub != app::ScrubKind::None && mapper && !fine) value = mapper(start_value, pixels);
            else if (fine) value = start_value + pixels / std::max(1, view.fine_pixels) * view.fine_units;
            else value = start_value + pixels / std::max(1, view.coarse_pixels) * view.coarse_units;
            value = std::max(view.minimum, std::min(view.maximum, value));
            if (value != view.value && commit) commit(value);
            return true;
        });
        connect_event(entry, "button-release-event", [this](GdkEvent*) {
            const bool was = scrubbing;
            armed = false;
            scrubbing = false;
            return was;
        });
        connect_event(entry, "key-press-event", [this](GdkEvent* e) {
            GdkEventKey* k = reinterpret_cast<GdkEventKey*>(e);
            if (k->keyval == GDK_KEY_Escape) { typing = false; show_value(); return true; }
            if (k->keyval == GDK_KEY_Up || k->keyval == GDK_KEY_Down) {
                int64_t base = view.value;
                if (typing) {
                    int32_t parsed = 0;
                    if (app::try_parse_int32(gtk_entry_get_text(GTK_ENTRY(entry)), true, parsed, culture)) base = parsed;
                }
                int64_t value = base + (k->keyval == GDK_KEY_Up ? 1 : -1) * std::max<int64_t>(1, view.increment);
                value = std::max(view.minimum, std::min(view.maximum, value));
                typing = false;
                if (commit && value != view.value) commit(value);
                return true;
            }
            return false;
        });
    }

    void show_value()
    {
        applying = true;
        std::string text = gtk_widget_has_focus(entry) ? std::to_string(view.value) : format::format_n0(view.value, culture);
        if (text != gtk_entry_get_text(GTK_ENTRY(entry))) gtk_entry_set_text(GTK_ENTRY(entry), text.c_str());
        applying = false;
    }

    void apply(const app::NumericFieldView& v)
    {
        view = v;
        gtk_widget_set_visible(entry, v.visible);
        gtk_widget_set_sensitive(entry, v.enabled);
        set_tooltip(entry, v.tooltip);
        if (!typing) show_value();
    }

    void commit_text()
    {
        int32_t parsed = 0;
        const std::string text = gtk_entry_get_text(GTK_ENTRY(entry));
        typing = false;
        if (app::try_parse_int32(text, true, parsed, culture) && parsed >= view.minimum && parsed <= view.maximum) {
            if (commit && parsed != view.value) commit(parsed);
        }
        show_value();
    }
};

}   // namespace

// ------------------------------------------------------------------ the controls

struct MainWindow::Controls {
    GtkWidget* group[4] = {};
    GtkWidget* menu_button = nullptr;
    GtkWidget *open = nullptr, *file_label = nullptr, *info_label = nullptr, *load_status = nullptr;
    GtkWidget *progress_overall = nullptr, *progress_stage = nullptr, *load_box = nullptr;
    GtkWidget *output = nullptr, *state = nullptr, *path_status = nullptr;
    GtkWidget *queue_check = nullptr, *events_label = nullptr, *overflow_label = nullptr, *overflow = nullptr;
    GtkWidget *rate_label = nullptr, *rate = nullptr, *service_label = nullptr, *unit_label = nullptr, *din = nullptr, *slider = nullptr;
    GtkListStore* rate_store = nullptr;
    GtkWidget *timeline = nullptr, *time_label = nullptr, *play = nullptr, *stop = nullptr, *back5 = nullptr, *forward5 = nullptr;
    GtkWidget *analysis = nullptr, *reset = nullptr;
    ScrubEntry queue_value, processing_value;
    // statistics
    GtkWidget *stat_caption[8] = {}, *stat_value[8] = {}, *pressure_box = nullptr, *pressure_bar = nullptr, *pressure_label = nullptr;
    app::StatisticsText stats;
    // menu
    GtkWidget* menu = nullptr;
    GtkWidget *mi_always_on_top = nullptr, *mi_show_processing = nullptr, *mi_show_statistics = nullptr, *mi_chase = nullptr,
              *mi_preserve = nullptr, *mi_output_paced = nullptr, *mi_few_cpu = nullptr, *mi_apply_queue = nullptr, *mi_queue_age = nullptr,
              *mi_scale_interval = nullptr, *mi_routing = nullptr;
    GtkWidget* mi_scale[6] = {};
    bool timeline_dragging = false;
};

namespace {
// Rows of the Rate model list: 0 None, 1 heading, 2..4 the three slowdown models, 5 heading, 6..9 the gate models.
int rate_row_for_choice(int choice) { return choice == 0 ? 0 : choice <= 3 ? choice + 1 : choice + 2; }
int rate_choice_for_row(int row)
{
    if (row == 0) return 0;
    if (row >= 2 && row <= 4) return row - 1;
    if (row >= 6 && row <= 9) return row - 2;
    return -1;
}
}   // namespace

MainWindow::MainWindow() {}

MainWindow::~MainWindow()
{
    if (smoke_timer_) g_source_remove(smoke_timer_);
    controller_.reset();
}

void MainWindow::setup_environment()
{
    files_.reset(new posix::PosixFileSystem());
    app::PlayerEnvironment env;
    env.services = std::make_shared<posix::PosixEngineServices>();
    env.files = files_.get();
#if defined(MIDIBN_HAVE_ALSA)
    // The ALSA sequencer ports of other clients take the place of the WinMM device list; the same output slot is used.
    env.list_devices = [] { return posix::AlsaMidiOutput::enumerate_devices(); };
    env.create_winmm = [] { return std::shared_ptr<platform::MidiOutput>(new posix::AlsaMidiOutput()); };
#else
    env.list_devices = [] { return std::vector<platform::OutputDeviceInfo>(); };
#endif
    env.defer_device_listing = false;
    env.pointer_size = static_cast<int>(sizeof(void*));
    env.private_bytes = [] { return static_cast<int64_t>(posix::process_memory().current_private); };
    env.wake_ui = [this] {
        if (!wake_pending_.exchange(true)) {
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                MainWindow* self = static_cast<MainWindow*>(data);
                self->wake_pending_ = false;
                self->handle_wake();
                return G_SOURCE_REMOVE;
            }, this, nullptr);
        }
    };
    env.has_initial_settings = settings_store_ != nullptr;
    env.initial_settings = remembered_.settings;
    controller_.reset(new app::PlayerController(env));
    if (options_.force_large_warning) controller_->set_force_large_file_checks_for_tests(true, true);
    if (settings_store_ && !settings_store_->note().empty())
        controller_->set_settings_note(settings_store_->note().text, settings_store_->note().detail);
}

// The values to remember now: the controller's part plus the window options of the menu.
app::RememberedSettings MainWindow::current_settings() const
{
    app::RememberedSettings s = controller_->remembered_settings();
    s.always_on_top = c_ && c_->mi_always_on_top && gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(c_->mi_always_on_top));
    s.show_processing = show_processing_;
    s.show_statistics = show_statistics_;
    s.scale_percent = scale_percent_;
    s.few_cpu_yield = few_cpu_stored_;
    return s;
}

// Writes the settings file when a remembered option changed (cheap compare; nothing is written at start). A failure never
// shows a box: the first one is noted once in the file-information area and its tooltip.
void MainWindow::save_settings_if_changed()
{
    if (!settings_store_ || !controller_ || !c_) return;
    const bool wrote = settings_store_->save_if_changed(current_settings());
    if (wrote && settings_load_note_) {   // a fresh file replaced the damaged one
        settings_load_note_ = false;
        controller_->set_settings_note(std::string(), std::string());
        controller_->refresh();
        apply_view(false);
    }
    else if (!wrote && !settings_load_note_ && !settings_failure_noted_ && !settings_store_->note().empty() && !settings_store_->read_only()) {
        settings_failure_noted_ = true;
        controller_->set_settings_note(settings_store_->note().text, settings_store_->note().detail);
        controller_->refresh();
        apply_view(false);
    }
}

bool MainWindow::create(const Options& options)
{
    options_ = options;
    // Remembered settings: read once, before anything is built. Automated launches (capture, smoke, auto-close) never read
    // or write the user's file unless they name one with --settings; --no-settings turns them off for any launch.
    const bool automated = options.smoke || options.auto_close_ms > 0 || !options.capture_file.empty();
    if (!options.no_settings && (!options.settings_path.empty() || !automated)) {
        settings_store_.reset(new app::SettingsStore(make_settings_io(options.settings_path)));
        remembered_ = settings_store_->load();
        settings_load_note_ = !settings_store_->note().empty();
    }
    few_cpu_stored_ = remembered_.settings.few_cpu_yield;
    scale_percent_ = app::clamp_ui_scale_percent(options.scale_given || !settings_store_ ? options.scale_percent : remembered_.settings.scale_percent);
    ScaleStyle::instance().set_percent(scale_percent_);
    one_core_ = options.force_one_core || one_physical_core();
    setup_environment();

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), kProductName);
    gtk_window_set_default_size(GTK_WINDOW(window_),
        options.client_width > 0 ? options.client_width : app::scale_metric(790, scale_percent_), 1);
    if (options.has_position) gtk_window_move(GTK_WINDOW(window_), options.position_x, options.position_y);
    c_.reset(new Controls());
    build_controls();
    build_menu();
    install_events();

    host_.controller = controller_.get();
    host_.main_window = window_;
    host_.analysis_seek = [this](int64_t us) { controller_->analysis_seek(us); after_action(); };
    host_.toggle_transport = [this] { controller_->play_clicked(); after_action(); };
    host_.open_channels = [this] { open_channels(); };
    host_.request_refresh = [this] { after_action(); };
    host_.show_help = [this](int topic) { show_help(topic); };

    controller_->refresh();
    apply_view(true);
    gtk_widget_show_all(window_);
    apply_view(true);   // visibility of the conditional rows
    if (settings_store_) {   // remembered window options
        if (!remembered_.settings.show_processing) set_section_visible(true, false);
        if (!remembered_.settings.show_statistics) set_section_visible(false, false);
        if (remembered_.settings.always_on_top) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(c_->mi_always_on_top), TRUE);
    }
    // The few-processor option: offered only on a one-core PC; on by default only with exactly one usable logical processor;
    // a remembered choice wins.
    if (one_core_) {
        few_cpu_yield_ = app::few_cpu_effective(few_cpu_stored_, static_cast<uint32_t>(usable_processors()));
        controller_->engine().set_behind_yield_microseconds(few_cpu_yield_ ? 2000 : 0);
        update_menu_states();
    }
    if (options.settings_change) {   // test: make some changes so the saved file can be checked
        apply_scale(150, true);
        controller_->toggle_state_chase();
        after_action();
    }
    if (!options.open_file.empty()) {
        controller_->open_file(options.open_file);
        test_play_pending_ = options.play;
        after_action();
    }
    if (options.show == "help") show_help(options.help_topic);
    else if (options.show == "about") g_idle_add([](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->show_about(); return G_SOURCE_REMOVE; }, this);
    else if (!options.show.empty()) show_pending_ = options.show;   // the song-dependent windows open once the file is loaded
    if (options.auto_close_ms > 0)
        g_timeout_add(static_cast<guint>(options.auto_close_ms), [](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->quit_now(); return G_SOURCE_REMOVE; }, this);
    if (!options.capture_file.empty())
        g_timeout_add(static_cast<guint>(options.capture_after_ms), [](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->capture(); return G_SOURCE_REMOVE; }, this);
    if (options.smoke) {
        smoke_stage_since_us_ = now_us();
        smoke_timer_ = g_timeout_add(40, [](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->smoke_step(); return G_SOURCE_CONTINUE; }, this);
    }
    return true;
}

// ------------------------------------------------------------------ building

void MainWindow::build_controls()
{
    Controls& c = *c_;
    const char* titles[4] = { "File and output", "Processing model", "Playback", "Statistics" };
    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(root), 6);
    gtk_container_add(GTK_CONTAINER(window_), root);
    for (int i = 0; i < 4; ++i) {
        c.group[i] = gtk_frame_new(titles[i]);
        gtk_box_pack_start(GTK_BOX(root), c.group[i], FALSE, FALSE, 0);
    }
    auto grid = [](GtkWidget* frame) {
        GtkWidget* g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 5);
        gtk_grid_set_column_spacing(GTK_GRID(g), 6);
        gtk_container_set_border_width(GTK_CONTAINER(g), 6);
        gtk_container_add(GTK_CONTAINER(frame), g);
        return g;
    };

    // ---- File and output
    GtkWidget* g1 = grid(c.group[0]);
    c.open = gtk_button_new_with_label("Open MIDI...");
    c.file_label = make_label("No file loaded");
    gtk_label_set_ellipsize(GTK_LABEL(c.file_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(c.file_label, TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(c.file_label), 10);
    c.info_label = make_label("", 1.0f);
    gtk_label_set_justify(GTK_LABEL(c.info_label), GTK_JUSTIFY_RIGHT);
    c.menu_button = gtk_button_new_with_label("\xE2\x89\xA1");
    set_tooltip(c.menu_button, "Menu: About, Help, Application scale, and the playback options");
    gtk_grid_attach(GTK_GRID(g1), c.open, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), c.file_label, 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(g1), c.info_label, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), c.menu_button, 4, 0, 1, 1);
    // the loading status (visible while a file loads)
    c.load_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c.load_status = make_label("");
    gtk_label_set_ellipsize(GTK_LABEL(c.load_status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(c.load_status, TRUE);
    GtkWidget* bars = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    c.progress_overall = gtk_progress_bar_new();
    c.progress_stage = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(bars), c.progress_overall, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bars), c.progress_stage, FALSE, FALSE, 0);
    gtk_widget_set_size_request(bars, app::scale_metric(180, 100), -1);
    gtk_box_pack_start(GTK_BOX(c.load_box), c.load_status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(c.load_box), bars, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g1), c.load_box, 0, 1, 5, 1);
    GtkWidget* output_label = make_label("MIDI output:", 1.0f);
    c.output = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(c.output, TRUE);
    c.path_status = make_label("", 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(c.path_status), PANGO_ELLIPSIZE_END);
    c.state = make_label("Stopped", 1.0f);
    gtk_grid_attach(GTK_GRID(g1), output_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), c.output, 1, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(g1), c.path_status, 3, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), c.state, 4, 2, 1, 1);

    // ---- Processing model
    GtkWidget* g2 = grid(c.group[1]);
    c.queue_check = gtk_check_button_new_with_label("Queue length limit:");
    c.queue_value.init(9);
    c.events_label = make_label("events");
    c.overflow_label = make_label("Overflow:", 1.0f);
    c.overflow = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(c.overflow, TRUE);
    GtkWidget* queue_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(queue_row), c.queue_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(queue_row), c.queue_value.entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(queue_row), c.events_label, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g2), queue_row, 0, 0, 1, 1);
    GtkWidget* overflow_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(overflow_row), c.overflow_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(overflow_row), c.overflow, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(g2), overflow_row, 1, 0, 1, 1);
    gtk_widget_set_hexpand(overflow_row, TRUE);

    c.rate_label = make_label("Rate model:");
    // The Rate model list: None, then the two headed groups (the headings cannot be chosen).
    c.rate_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_BOOLEAN);
    auto add_row = [&](const char* text, bool selectable) {
        GtkTreeIter it;
        gtk_list_store_append(c.rate_store, &it);
        gtk_list_store_set(c.rate_store, &it, 0, text, 1, selectable, -1);
    };
    add_row(app::rate_choice_full_label(app::RateModelChoice::None), true);
    add_row(app::rate_slowdown_heading(false), false);
    for (int i = 1; i <= 3; ++i) add_row(app::rate_choice_full_label(static_cast<app::RateModelChoice>(i)), true);
    add_row(app::rate_gate_heading(), false);
    for (int i = 4; i <= 7; ++i) add_row(app::rate_choice_full_label(static_cast<app::RateModelChoice>(i)), true);
    c.rate = gtk_combo_box_new_with_model(GTK_TREE_MODEL(c.rate_store));
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(c.rate), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(c.rate), renderer, "text", 0, "sensitive", 1, nullptr);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c.rate), 0);
    c.service_label = make_label("");
    c.processing_value.init(10);
    c.unit_label = make_label("");
    c.din = gtk_button_new_with_label("5-pin DIN");
    GtkWidget* rate_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(rate_row), c.rate_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rate_row), c.rate, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rate_row), c.service_label, FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(rate_row), c.processing_value.entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rate_row), c.unit_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rate_row), c.din, FALSE, FALSE, 6);
    gtk_grid_attach(GTK_GRID(g2), rate_row, 0, 1, 2, 1);
    c.slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, app::kSliderScaleMaximum, 1);
    gtk_scale_set_draw_value(GTK_SCALE(c.slider), FALSE);
    for (int m = 0; m <= app::kSliderScaleMaximum; m += 1000) gtk_scale_add_mark(GTK_SCALE(c.slider), m, GTK_POS_BOTTOM, nullptr);
    gtk_widget_set_hexpand(c.slider, TRUE);
    gtk_grid_attach(GTK_GRID(g2), c.slider, 0, 2, 2, 1);

    // ---- Playback
    GtkWidget* g3 = grid(c.group[2]);
    c.timeline = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(c.timeline), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(c.timeline), 0);
    gtk_widget_set_hexpand(c.timeline, TRUE);
    c.time_label = make_label("00:00.000 / 00:00.000", 1.0f);
    add_class(c.time_label, "mono");
    GtkWidget* tl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(tl), c.timeline, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tl), c.time_label, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g3), tl, 0, 0, 1, 1);
    gtk_widget_set_hexpand(tl, TRUE);
    c.play = gtk_button_new_with_label("Play");
    c.stop = gtk_button_new_with_label("Stop");
    c.back5 = gtk_button_new_with_label("\xE2\x88\x92" "5 sec");
    c.forward5 = gtk_button_new_with_label("+5 sec");
    c.analysis = gtk_button_new_with_label("Analysis...");
    c.reset = gtk_button_new_with_label("Reset stats");
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (GtkWidget* b : { c.play, c.stop, c.back5, c.forward5, c.analysis }) gtk_box_pack_start(GTK_BOX(buttons), b, FALSE, FALSE, 0);
    GtkWidget* spacer = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(buttons), spacer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), c.reset, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g3), buttons, 0, 1, 1, 1);

    // ---- Statistics
    GtkWidget* g4 = grid(c.group[3]);
    static const char* const kCaptions[8] = { "Timeline / output:", "Queue now / maximum:", "Maximum rate:", "Output rate:",
        "Events sent / dropped:", "Effective speed:", "Maximum lag:", "Current lag:" };
    GtkWidget* halves = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_set_homogeneous(GTK_BOX(halves), TRUE);
    GtkWidget* half[2];
    for (int h = 0; h < 2; ++h) {
        half[h] = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(half[h]), 4);
        gtk_grid_set_column_spacing(GTK_GRID(half[h]), 8);
        gtk_box_pack_start(GTK_BOX(halves), half[h], TRUE, TRUE, 0);
    }
    for (int i = 0; i < 8; ++i) {
        c.stat_caption[i] = make_label(kCaptions[i]);
        c.stat_value[i] = make_label("", 1.0f);
        add_class(c.stat_value[i], "mono");
        gtk_widget_set_hexpand(c.stat_value[i], TRUE);
        gtk_label_set_width_chars(GTK_LABEL(c.stat_value[i]), i == 0 ? 25 : 12);
        GtkWidget* cap_box = gtk_event_box_new();
        GtkWidget* val_box = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(cap_box), c.stat_caption[i]);
        gtk_container_add(GTK_CONTAINER(val_box), c.stat_value[i]);
        gtk_widget_set_hexpand(val_box, TRUE);
        const int row = i / 2, col = i % 2;
        gtk_grid_attach(GTK_GRID(half[col]), cap_box, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(half[col]), val_box, 1, row, 1, 1);
        if (i == 5) {   // right-click: the measurement window of the effective speed
            for (GtkWidget* box : { cap_box, val_box }) {
                gtk_widget_add_events(box, GDK_BUTTON_PRESS_MASK);
                connect_event(box, "button-press-event", [this](GdkEvent* e) {
                    GdkEventButton* b = reinterpret_cast<GdkEventButton*>(e);
                    if (b->button == 3) { show_speed_menu(e); return true; }
                    return false;
                });
            }
        }
    }
    gtk_grid_attach(GTK_GRID(g4), halves, 0, 0, 1, 1);
    gtk_widget_set_hexpand(halves, TRUE);
    c.pressure_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    c.pressure_bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(c.pressure_bar, app::scale_metric(220, 100), app::scale_metric(10, 100));
    gtk_widget_set_valign(c.pressure_bar, GTK_ALIGN_CENTER);
    connect_draw(c.pressure_bar, [this](cairo_t* cr, GtkWidget* widget) {
        const app::StatisticsText& t = c_->stats;
        const int w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
        const double ratio = format::queue_pressure_ratio(t.occupied, t.limit);
        const format::Rgb bg = format::kPressureBackgroundRgb, ol = format::kPressureOutlineRgb;
        const format::Rgb fg = format::pressure_color_rgb(format::choose_pressure_color(ratio, t.overflow_pulse));
        cairo_set_source_rgb(cr, bg.r / 255.0, bg.g / 255.0, bg.b / 255.0);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, fg.r / 255.0, fg.g / 255.0, fg.b / 255.0);
        cairo_rectangle(cr, 0, 0, std::nearbyint(w * std::min(1.0, ratio)), h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, ol.r / 255.0, ol.g / 255.0, ol.b / 255.0);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
        cairo_stroke(cr);
    });
    c.pressure_label = make_label("");
    add_class(c.pressure_label, "mono");
    gtk_box_pack_start(GTK_BOX(c.pressure_box), c.pressure_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c.pressure_box), c.pressure_label, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g4), c.pressure_box, 0, 1, 1, 1);
    gtk_widget_set_no_show_all(c.pressure_box, TRUE);
    gtk_widget_set_no_show_all(c.load_box, TRUE);

    // ---- signals
    connect(c.open, "clicked", [this] {
        bool wants_dialog = false;
        controller_->open_button_clicked(wants_dialog);
        after_action();
        if (wants_dialog) choose_file();
    });
    connect(c.play, "clicked", [this] { controller_->play_clicked(); after_action(); });
    connect(c.stop, "clicked", [this] { controller_->stop_clicked(); after_action(); });
    connect(c.back5, "clicked", [this] { controller_->seek_by(-5000000); after_action(); });
    connect(c.forward5, "clicked", [this] { controller_->seek_by(5000000); after_action(); });
    connect(c.analysis, "clicked", [this] { open_analysis(); });
    connect(c.reset, "clicked", [this] { controller_->reset_statistics_clicked(); after_action(); });
    connect(c.din, "clicked", [this] { controller_->din_preset(); after_action(); });
    connect(c.queue_check, "toggled", [this] {
        if (applying_view_) return;
        controller_->set_queue_limit_checked(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c_->queue_check)));
        after_action();
    });
    connect(c.output, "changed", [this] {
        if (applying_view_) return;
        controller_->select_output(gtk_combo_box_get_active(GTK_COMBO_BOX(c_->output)));
        after_action();
    });
    connect(c.overflow, "changed", [this] {
        if (applying_view_) return;
        controller_->set_overflow_selection(gtk_combo_box_get_active(GTK_COMBO_BOX(c_->overflow)));
        after_action();
    });
    connect(c.rate, "changed", [this] {
        if (applying_view_) return;
        const int choice = rate_choice_for_row(gtk_combo_box_get_active(GTK_COMBO_BOX(c_->rate)));
        if (choice >= 0) controller_->select_rate_choice(static_cast<app::RateModelChoice>(choice));
        after_action();
    });
    connect(c.slider, "value-changed", [this] {
        if (applying_view_) return;
        controller_->set_slider(static_cast<int>(std::lround(gtk_range_get_value(GTK_RANGE(c_->slider)))));
        after_action();
    });
    c.queue_value.commit = [this](int64_t v) { controller_->set_queue_value(v); after_action(); };
    c.queue_value.mapper = [this](int64_t start, int64_t px) { return controller_->scrub_value(false, start, px); };
    c.processing_value.commit = [this](int64_t v) { controller_->set_processing_value(v); after_action(); };
    c.processing_value.mapper = [this](int64_t start, int64_t px) { return controller_->scrub_value(true, start, px); };

    // the timeline: a click or drag seeks when the button is released; the arrow keys, Page keys, Home and End seek at once
    gtk_widget_add_events(c.timeline, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    connect_event(c.timeline, "button-press-event", [this](GdkEvent*) {
        if (gtk_widget_get_sensitive(c_->timeline)) { c_->timeline_dragging = true; controller_->set_timeline_dragging(true); }
        return false;
    });
    g_signal_connect_after(c.timeline, "button-release-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
        MainWindow* self = static_cast<MainWindow*>(data);
        if (self->c_->timeline_dragging) {
            self->c_->timeline_dragging = false;
            self->controller_->set_timeline_dragging(false);
            self->controller_->seek_to(static_cast<int64_t>(gtk_range_get_value(GTK_RANGE(self->c_->timeline))));
            self->after_action();
        }
        return FALSE;
    }), this);
    connect(c.timeline, "value-changed", [this] {
        if (c_->timeline_dragging)
            set_text(GTK_LABEL(c_->time_label), format::format_time(static_cast<int64_t>(gtk_range_get_value(GTK_RANGE(c_->timeline)))) +
                " / " + format::format_time(last_view_.duration_us));
    });
    connect_event(c.timeline, "key-press-event", [this](GdkEvent* e) {
        GdkEventKey* k = reinterpret_cast<GdkEventKey*>(e);
        app::TimelineKey key;
        switch (k->keyval) {
        case GDK_KEY_Home: key = app::TimelineKey::Home; break;
        case GDK_KEY_End: key = app::TimelineKey::End; break;
        case GDK_KEY_Left: key = app::TimelineKey::Left; break;
        case GDK_KEY_Right: key = app::TimelineKey::Right; break;
        case GDK_KEY_Page_Up: key = app::TimelineKey::PageUp; break;
        case GDK_KEY_Page_Down: key = app::TimelineKey::PageDown; break;
        default: return false;
        }
        const app::PlayerView& v = controller_->view();
        controller_->seek_to(app::timeline_key_target(key, v.position_us, v.duration_us));
        after_action();
        return true;
    });

    // accessible names
    struct Named { GtkWidget* w; const char* name; };
    for (const Named& n : { Named{ c.output, "MIDI output" }, Named{ c.rate, "Rate model" }, Named{ c.overflow, "Overflow policy" },
             Named{ c.slider, "Rate" }, Named{ c.timeline, "Playback position" }, Named{ c.queue_value.entry, "Queue length limit" },
             Named{ c.processing_value.entry, "Processing time per event" }, Named{ c.file_label, "File name" },
             Named{ c.info_label, "File information" }, Named{ c.state, "Playback state" }, Named{ c.path_status, "Output path status" } })
        atk_object_set_name(gtk_widget_get_accessible(n.w), n.name);
}

void MainWindow::build_menu()
{
    Controls& c = *c_;
    c.menu = gtk_menu_new();
    auto add = [&](GtkWidget* item) { gtk_menu_shell_append(GTK_MENU_SHELL(c.menu), item); return item; };
    auto sep = [&] { add(gtk_separator_menu_item_new()); };
    auto plain = [&](const char* text, std::function<void()> fn) {
        GtkWidget* item = add(gtk_menu_item_new_with_mnemonic(text));
        connect(item, "activate", std::move(fn));
        return item;
    };
    auto check = [&](const char* text, std::function<void()> fn) {
        GtkWidget* item = add(gtk_check_menu_item_new_with_mnemonic(text));
        connect(item, "toggled", [this, fn] { if (!applying_view_) fn(); });
        return item;
    };
    plain("About MIDIBottleneck Player\xE2\x80\xA6", [this] { show_about(); });
    plain("_Help (F1)\xE2\x80\xA6", [this] { show_help(help_topic_for_focus()); });
    sep();
    c.mi_routing = plain("Track _routing\xE2\x80\xA6", [this] { open_routing(); });
    sep();
    c.mi_always_on_top = check("Always on top", [this] {
        gtk_window_set_keep_above(GTK_WINDOW(window_), gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(c_->mi_always_on_top)));
        save_settings_if_changed();
    });
    c.mi_show_processing = check("Show _Processing model", [this] { set_section_visible(true, !show_processing_); });
    c.mi_show_statistics = check("Show _Statistics", [this] { set_section_visible(false, !show_statistics_); });
    GtkWidget* scale_item = add(gtk_menu_item_new_with_mnemonic("Application _scale"));
    GtkWidget* scale_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(scale_item), scale_menu);
    GSList* group = nullptr;
    for (int i = 0; i < 6; ++i) {
        char text[16];
        std::snprintf(text, sizeof(text), "%d%%", kScales[i]);
        c.mi_scale[i] = gtk_radio_menu_item_new_with_label(group, text);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(c.mi_scale[i]));
        gtk_menu_shell_append(GTK_MENU_SHELL(scale_menu), c.mi_scale[i]);
        const int percent = kScales[i];
        connect(c.mi_scale[i], "toggled", [this, i, percent] {
            if (applying_view_) return;
            if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(c_->mi_scale[i]))) apply_scale(percent, true);
        });
    }
    sep();
    c.mi_chase = check("Chase MIDI state on Play/Seek", [this] { controller_->toggle_state_chase(); after_action(); });
    c.mi_preserve = check("Preserve playback queue on Pause and Seek", [this] { controller_->toggle_preserve_pause_seek(); after_action(); });
    c.mi_output_paced = check("Output-paced timeline", [this] { controller_->toggle_output_paced_timeline(); after_action(); });
    if (one_core_)   // only a PC with one physical processor core (DD-112)
        c.mi_few_cpu = check("Let other programs run while playback is behind", [this] {
            few_cpu_yield_ = !few_cpu_yield_;
            few_cpu_stored_ = few_cpu_yield_ ? 1 : 0;   // from now on the user's choice wins over the automatic default
            controller_->engine().set_behind_yield_microseconds(few_cpu_yield_ ? 2000 : 0);
            after_action();
        });
    sep();
    c.mi_apply_queue = check("Apply queue limit without slowdown", [this] { controller_->toggle_apply_queue_limit_without_slowdown(); after_action(); });
    c.mi_queue_age = check("Limit queue by waiting time (\xC2\xB5s)", [this] { controller_->toggle_queue_age_limit(); after_action(); });
    c.mi_scale_interval = check("Scale note interval by note traffic", [this] { controller_->toggle_scale_note_interval_by_traffic(); after_action(); });
    gtk_widget_show_all(c.menu);
    connect(c.menu_button, "clicked", [this] {
        update_menu_states();
        gtk_menu_popup_at_widget(GTK_MENU(c_->menu), c_->menu_button, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
    });
}

void MainWindow::install_events()
{
    connect_delete(window_, [this] { return on_delete(); });
    connect(window_, "destroy", [] { gtk_main_quit(); });
    connect_event(window_, "key-press-event", [this](GdkEvent* e) { return on_key(reinterpret_cast<GdkEventKey*>(e)); });
    // Drag and drop of a file
    static const GtkTargetEntry targets[] = { { const_cast<char*>("text/uri-list"), 0, 0 } };
    gtk_drag_dest_set(window_, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(window_, "drag-data-received", G_CALLBACK(+[](GtkWidget*, GdkDragContext* context, gint, gint, GtkSelectionData* data,
        guint, guint time, gpointer self) {
        gchar** uris = gtk_selection_data_get_uris(data);
        std::string list;
        if (uris) {
            for (int i = 0; uris[i]; ++i) list += std::string(uris[i]) + "\n";
            g_strfreev(uris);
        }
        static_cast<MainWindow*>(self)->open_dropped(list);
        gtk_drag_finish(context, TRUE, FALSE, time);
    }), this);
}

// ------------------------------------------------------------------ keyboard

bool MainWindow::on_key(GdkEventKey* event)
{
    if (event->keyval == GDK_KEY_F1) {
        show_help(help_topic_for_focus());
        return true;
    }
    if (event->keyval == GDK_KEY_space && (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) == 0) {
        GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window_));
        if (focus && (GTK_IS_ENTRY(focus) || GTK_IS_BUTTON(focus) || GTK_IS_COMBO_BOX(focus))) return false;   // the focused control keeps the key
        if (controller_->view().play_enabled) {
            controller_->play_clicked();
            after_action();
        }
        return true;
    }
    return false;
}

int MainWindow::help_topic_for_focus()
{
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window_));
    Controls& c = *c_;
    auto among = [&](std::initializer_list<GtkWidget*> list) {
        for (GtkWidget* w : list) if (w == focus) return true;
        return false;
    };
    if (focus == c.analysis) return static_cast<int>(app::HelpTopic::Analysis);
    if (among({ c.queue_check, c.queue_value.entry, c.overflow })) return static_cast<int>(app::HelpTopic::QueueAndOverflow);
    if (among({ c.open, c.output })) return static_cast<int>(app::HelpTopic::FileAndOutput);
    if (among({ c.rate, c.processing_value.entry, c.din, c.slider })) return static_cast<int>(app::HelpTopic::RateModel);
    if (among({ c.timeline, c.play, c.stop, c.back5, c.forward5, c.reset })) return static_cast<int>(app::HelpTopic::Playback);
    return static_cast<int>(app::HelpTopic::Overview);
}

// ------------------------------------------------------------------ presentation

void MainWindow::apply_view(bool force)
{
    Controls& c = *c_;
    applying_view_ = true;
    const app::PlayerView& v = controller_->view();
    const bool first = !view_valid_ || force;
    const app::PlayerView& o = last_view_;
    auto set_time = [&] {
        if (!c.timeline_dragging) {
            gtk_range_set_range(GTK_RANGE(c.timeline), 0, static_cast<double>(std::max<int64_t>(1, v.duration_us)));
            gtk_range_set_value(GTK_RANGE(c.timeline), static_cast<double>(v.position_us));
            set_text(GTK_LABEL(c.time_label), format::format_time(v.position_us) + " / " + format::format_time(v.duration_us));
        }
    };
    // Fast path (every live update while playing): only the readouts changed.
    if (!first && v.same_controls(o)) {
        set_time();
        const int64_t t0 = now_us();
        unsigned mask = 0xFF;
        if (ladder_ready_ && ladder_.half_statistics[tier_state_.tier] && tick_id_ != 0) mask = stats_phase_ ? 0xF0u : 0x0Fu;
        c_->stats = v.statistics;
        for (int i = 0; i < 8; ++i)
            if (mask & (1u << i)) set_text(GTK_LABEL(c.stat_value[i]), v.statistics.values[i]);
        meter_window_.stats_set += now_us() - t0;
        if (v.state_text != o.state_text) set_text(GTK_LABEL(c.state), v.state_text);
        gtk_widget_queue_draw(c.pressure_bar);
        last_view_.statistics = v.statistics;
        last_view_.position_us = v.position_us;
        last_view_.state_text = v.state_text;
        applying_view_ = false;
        return;
    }

    gtk_button_set_label(GTK_BUTTON(c.open), v.open_button_text.c_str());
    set_text(GTK_LABEL(c.file_label), v.file_text);
    set_tooltip(c.file_label, v.file_tooltip);
    std::string info = v.file_info;
    for (size_t p; (p = info.find('\r')) != std::string::npos;) info.erase(p, 1);
    set_text(GTK_LABEL(c.info_label), info);
    set_tooltip(c.info_label, v.file_info_tooltip);
    set_text(GTK_LABEL(c.load_status), v.load_status);
    set_tooltip(c.load_status, v.load_tooltip);
    set_text(GTK_LABEL(c.state), v.state_text);
    set_text(GTK_LABEL(c.path_status), v.output_path_short);
    set_tooltip(c.path_status, v.output_path_tooltip.empty() ? v.output_path_text : v.output_path_tooltip);
    gtk_widget_set_visible(c.load_box, v.loading);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(c.progress_overall), v.load_overall_permille / 1000.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(c.progress_stage), v.load_stage_permille / 1000.0);

    if (first || v.output_items != o.output_items) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(c.output));
        for (const std::string& item : v.output_items) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c.output), item.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c.output), v.output_selected);
    gtk_widget_set_sensitive(c.output, v.output_enabled);
    set_tooltip(c.output, "The MIDI output: None plays nothing and only measures; the other entries are the ALSA sequencer ports of other programs and devices.");

    gtk_button_set_label(GTK_BUTTON(c.queue_check), v.queue_check_text.c_str());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c.queue_check), v.queue_check_checked);
    gtk_widget_set_sensitive(c.queue_check, v.queue_check_enabled);
    set_tooltip(c.queue_check, v.queue_check_tooltip);
    c.queue_value.apply(v.queue_value);
    set_text(GTK_LABEL(c.events_label), v.events_label);
    set_text(GTK_LABEL(c.overflow_label), v.overflow_label);
    if (first || v.overflow_items != o.overflow_items) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(c.overflow));
        for (const std::string& item : v.overflow_items) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c.overflow), item.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c.overflow), v.overflow_selected);
    gtk_widget_set_sensitive(c.overflow, v.overflow_enabled);
    set_tooltip(c.overflow, v.overflow_tooltip);

    set_text(GTK_LABEL(c.rate_label), v.rate_label);
    {
        GtkTreeIter it;
        if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(c.rate_store), &it, nullptr, 1))
            gtk_list_store_set(c.rate_store, &it, 0, app::rate_slowdown_heading(v.rate_forward_heading), -1);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c.rate), rate_row_for_choice(static_cast<int>(v.rate_choice)));
    set_tooltip(c.rate, v.rate_tooltip);
    set_text(GTK_LABEL(c.service_label), v.service_value_label);
    gtk_widget_set_visible(c.service_label, !v.service_value_label.empty());
    c.processing_value.apply(v.processing_value);
    set_text(GTK_LABEL(c.unit_label), v.service_unit);
    gtk_widget_set_visible(c.unit_label, v.service_unit_visible);
    gtk_widget_set_visible(c.din, v.din_visible);
    gtk_widget_set_sensitive(c.din, v.din_enabled);
    set_tooltip(c.din, "Sets the rate to the speed of a 5-pin DIN MIDI cable (31,250 bits per second).");
    if (static_cast<int>(std::lround(gtk_range_get_value(GTK_RANGE(c.slider)))) != v.slider_value) gtk_range_set_value(GTK_RANGE(c.slider), v.slider_value);
    gtk_widget_set_sensitive(c.slider, v.slider_enabled);
    set_tooltip(c.slider, v.slider_tooltip);

    gtk_button_set_label(GTK_BUTTON(c.play), v.play_text.c_str());
    gtk_widget_set_sensitive(c.play, v.play_enabled);
    set_tooltip(c.play, v.play_tooltip);
    gtk_widget_set_sensitive(c.stop, v.stop_enabled);
    for (GtkWidget* w : { c.back5, c.forward5, c.analysis, c.timeline }) gtk_widget_set_sensitive(w, v.seek_enabled);
    set_tooltip(c.back5, "Seek back 5 seconds");
    set_tooltip(c.forward5, "Seek forward 5 seconds");
    set_time();

    c_->stats = v.statistics;
    for (int i = 0; i < 8; ++i) {
        set_text(GTK_LABEL(c.stat_value[i]), v.statistics.values[i]);
        const std::string tip = app::statistics_cell_tooltip(v.statistics, i);
        set_tooltip(c.stat_caption[i], tip);
        set_tooltip(c.stat_value[i], tip);
    }
    gtk_widget_set_visible(c.pressure_box, v.statistics.queue_limited && show_statistics_);
    if (v.statistics.queue_limited) {
        const double ratio = format::queue_pressure_ratio(v.statistics.occupied, v.statistics.limit);
        const format::Rgb color = format::pressure_color_rgb(format::choose_pressure_color(ratio, v.statistics.overflow_pulse));
        gtk_label_set_markup(GTK_LABEL(c.pressure_label), pressure_markup(format::format_queue_pressure_text(v.statistics.virtual_queue,
            v.statistics.age_pressure, v.statistics.occupied, v.statistics.limit, false), color).c_str());
        set_tooltip(c.pressure_box, format::queue_pressure_tooltip(v.statistics.virtual_queue, v.statistics.age_pressure,
            v.statistics.occupied, v.statistics.limit));
        gtk_widget_queue_draw(c.pressure_bar);
    }

    last_view_ = v;
    view_valid_ = true;
    applying_view_ = false;
    update_menu_states();
}

void MainWindow::update_menu_states()
{
    if (!c_ || !c_->menu) return;
    Controls& c = *c_;
    const app::PlayerView& v = controller_->view();
    const bool was = applying_view_;
    applying_view_ = true;
    auto check = [&](GtkWidget* item, bool on, bool enabled) {
        if (!item) return;
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), on);
        gtk_widget_set_sensitive(item, enabled);
    };
    check(c.mi_show_processing, show_processing_, true);
    check(c.mi_show_statistics, show_statistics_, true);
    check(c.mi_chase, v.menu.chase_checked, v.menu.chase_enabled);
    check(c.mi_preserve, v.menu.preserve_checked, v.menu.preserve_enabled);
    check(c.mi_output_paced, v.menu.output_paced_checked, v.menu.output_paced_enabled);
    check(c.mi_few_cpu, few_cpu_yield_, true);
    check(c.mi_apply_queue, v.menu.apply_queue_limit_checked, v.menu.apply_queue_limit_enabled);
    check(c.mi_queue_age, v.menu.queue_age_checked, v.menu.queue_age_enabled);
    check(c.mi_scale_interval, v.menu.scale_interval_checked, v.menu.scale_interval_enabled);
    gtk_widget_set_sensitive(c.mi_routing, v.menu.track_routing_enabled || controller_->track_routing_available());
    for (int i = 0; i < 6; ++i) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(c.mi_scale[i]), kScales[i] == scale_percent_);
    applying_view_ = was;
}

void MainWindow::set_section_visible(bool processing, bool visible)
{
    bool& flag = processing ? show_processing_ : show_statistics_;
    if (flag == visible) return;
    flag = visible;
    GtkWidget* group = processing ? c_->group[1] : c_->group[3];
    if (!visible) {
        GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window_));
        if (focus && gtk_widget_is_ancestor(focus, group)) gtk_widget_grab_focus(c_->open);
    }
    gtk_widget_set_visible(group, visible);
    gtk_window_resize(GTK_WINDOW(window_), gtk_widget_get_allocated_width(window_), 1);
    if (visible) after_action();
    else save_settings_if_changed();
}

void MainWindow::apply_scale(int percent, bool resize)
{
    percent = app::clamp_ui_scale_percent(percent);
    if (percent == scale_percent_ && !resize) return;
    int w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(window_), &w, &h);
    const double canonical_width = w * 100.0 / std::max(1, scale_percent_);
    scale_percent_ = percent;
    ScaleStyle::instance().set_percent(percent);
    gtk_widget_set_size_request(gtk_widget_get_parent(c_->progress_overall), app::scale_metric(180, percent), -1);
    gtk_widget_set_size_request(c_->pressure_bar, app::scale_metric(220, percent), app::scale_metric(10, percent));
    if (resize) gtk_window_resize(GTK_WINDOW(window_), std::max(1, static_cast<int>(std::lround(canonical_width * percent / 100.0))), 1);
    update_menu_states();
    if (help_window_) help_window_->on_scale_changed();
    for (auto& w2 : analysis_windows_) w2->on_scale_changed();
    if (channels_window_) channels_window_->on_scale_changed();
    after_action();
}

void MainWindow::after_action()
{
    if (closing_) return;
    controller_->refresh();
    apply_view(false);
    update_refresh();
    show_pending_messages();
    refresh_companions();
    save_settings_if_changed();
}

void MainWindow::show_pending_messages()
{
    std::vector<app::UiMessage> messages = controller_->take_messages();
    for (const app::UiMessage& m : messages)
        message_box(m.title, m.text, m.kind == app::UiMessage::Error ? 2 : m.kind == app::UiMessage::Warning ? 1 : 0);
}

int MainWindow::run_dialog(GtkWidget* dialog)
{
    GtkWidget* previous = modal_;
    modal_ = dialog;
    const int response = gtk_dialog_run(GTK_DIALOG(dialog));
    modal_ = previous;
    return response;
}

void MainWindow::message_box(const std::string& title, const std::string& text, int kind)
{
    GtkWidget* dialog = new_message_dialog(GTK_WINDOW(window_),
        kind == 2 ? GTK_MESSAGE_ERROR : kind == 1 ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, text);
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    run_dialog(dialog);
    gtk_widget_destroy(dialog);
}

void MainWindow::handle_wake()
{
    controller_->pump();
    std::string title, heading, text;
    if (!large_dialog_open_ && controller_->large_file_decision_pending(title, heading, text)) {
        apply_view(false);
        large_dialog_open_ = true;
        const bool accepted = large_file_dialog(title, heading, text);
        large_dialog_open_ = false;
        controller_->answer_large_file(accepted);
    }
    if (test_play_pending_ && controller_->has_song() && !controller_->loading()) {
        test_play_pending_ = false;
        if (options_.select_rate >= 0) controller_->select_rate_choice(static_cast<app::RateModelChoice>(options_.select_rate));
        controller_->select_output(0);   // output None: tests never open a device
        if (controller_->output_selection() != 0) std::fprintf(stderr, "midibn-gtk: --play refused: the active output is not None\n");
        else controller_->play_clicked();
    }
    if (!show_pending_.empty() && controller_->has_song() && !controller_->loading()) {
        const std::string which = show_pending_;
        show_pending_.clear();
        if (which == "analysis") open_analysis();
        else if (which == "channels") open_channels();
        else if (which == "routing")
            g_idle_add([](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->open_routing(); return G_SOURCE_REMOVE; }, this);
    }
    after_action();
}

void MainWindow::choose_file()
{
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Open Standard MIDI File", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter* midi = gtk_file_filter_new();
    gtk_file_filter_set_name(midi, "MIDI files (*.mid;*.midi;*.rmi)");
    for (const char* pattern : { "*.mid", "*.midi", "*.rmi", "*.MID", "*.MIDI", "*.RMI" }) gtk_file_filter_add_pattern(midi, pattern);
    GtkFileFilter* all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files (*)");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), midi);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);
    if (run_dialog(dialog) == GTK_RESPONSE_ACCEPT) {
        gchar* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (name) {
            controller_->open_file(name);
            g_free(name);
        }
    }
    gtk_widget_destroy(dialog);
    after_action();
}

void MainWindow::open_dropped(const std::string& uri_list)
{
    std::vector<std::string> uris;
    std::istringstream in(uri_list);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) uris.push_back(line);
    }
    if (uris.size() != 1) return;
    gchar* path = g_filename_from_uri(uris[0].c_str(), nullptr, nullptr);
    if (!path) return;
    const std::string file = path;
    g_free(path);
    if (has_midi_extension(file) && g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR)) {
        controller_->open_file(file);
        after_action();
    }
}

// ------------------------------------------------------------------ dialogs

bool MainWindow::large_file_dialog(const std::string& title, const std::string& heading, const std::string& text)
{
    GtkWidget* dialog = gtk_dialog_new_with_buttons(title.c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL, "_Continue", GTK_RESPONSE_OK,
        "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 14);
    gtk_box_set_spacing(GTK_BOX(area), 6);
    auto add_label = [&](const std::string& s, bool bold) {
        GtkWidget* l = make_label("");
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
        gtk_label_set_selectable(GTK_LABEL(l), FALSE);
        if (bold) {
            gchar* e = g_markup_escape_text(s.c_str(), -1);
            gchar* m = g_strdup_printf("<b>%s</b>", e);
            gtk_label_set_markup(GTK_LABEL(l), m);
            g_free(e);
            g_free(m);
        }
        else gtk_label_set_text(GTK_LABEL(l), s.c_str());
        gtk_box_pack_start(GTK_BOX(area), l, FALSE, FALSE, 0);
    };
    add_label(heading, true);
    // The message line by line: the estimated memory goes on its own bold line and the 32-bit warning is bold (DD-105).
    const std::string memory_label = "Estimated memory while the MIDI is open: ";
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, memory_label.size(), memory_label) == 0) {
            add_label(memory_label.substr(0, memory_label.size() - 1), false);
            add_label(line.substr(memory_label.size()), true);
        }
        else if (line.empty()) add_label(" ", false);
        else add_label(line, line.compare(0, 11, "32-bit warn") == 0);
    }
    gtk_widget_show_all(dialog);
    const bool accepted = run_dialog(dialog) == GTK_RESPONSE_OK;
    gtk_widget_destroy(dialog);
    return accepted;
}

void MainWindow::show_speed_menu(GdkEvent* event)
{
    GtkWidget* menu = gtk_menu_new();
    const int64_t current = controller_->effective_speed_window();
    GSList* group = nullptr;
    for (const app::SpeedWindowPreset& preset : app::speed_window_presets()) {
        GtkWidget* item = gtk_radio_menu_item_new_with_label(group, preset.caption);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), preset.microseconds == current);
        const int64_t us = preset.microseconds;
        connect(item, "activate", [this, us] { controller_->set_effective_speed_window(us); after_action(); });
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget* custom = gtk_menu_item_new_with_label("Custom...");
    connect(custom, "activate", [this] { custom_speed_window(); });
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), custom);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(+[](GtkMenuShell* m, gpointer) { g_idle_add([](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, m); }), nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

void MainWindow::custom_speed_window()
{
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Effective playback speed window", GTK_WINDOW(window_), GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK,
        "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* spin = gtk_spin_button_new_with_range(app::kSpeedWindowDialogMinimumMs, app::kSpeedWindowDialogMaximumMs, app::kSpeedWindowDialogIncrementMs);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), app::speed_window_dialog_initial_ms(controller_->effective_speed_window()));
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(row), make_label("Measurement window:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_label("ms"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), row, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    if (run_dialog(dialog) == GTK_RESPONSE_OK) {
        gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
        controller_->set_effective_speed_window(app::speed_window_from_dialog_ms(gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin))));
        after_action();
    }
    gtk_widget_destroy(dialog);
}

void MainWindow::show_about()
{
    GtkWidget* dialog = gtk_dialog_new_with_buttons("About MIDIBottleneck Player", GTK_WINDOW(window_), GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK, nullptr);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(area), 18);
    gtk_box_set_spacing(GTK_BOX(area), 8);
    GtkWidget* name = make_label("");
    gtk_label_set_markup(GTK_LABEL(name), "<b>MIDIBottleneck Player</b>");
    gtk_box_pack_start(GTK_BOX(area), name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), make_label("v2 preview  \xE2\x80\xA2  version 2.0.0.0"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), make_label("A MIDI workload, queue, timing, and bottleneck laboratory."), FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    run_dialog(dialog);
    gtk_widget_destroy(dialog);
}

// ------------------------------------------------------------------ Help and the companion windows

void MainWindow::show_help(int topic)
{
    if (help_window_ && help_window_->alive()) {
        help_window_select(help_window_.get(), topic);
        return;
    }
    help_window_ = create_help_window(host_, topic);
}

void MainWindow::refresh_companions()
{
    for (size_t i = analysis_windows_.size(); i > 0; --i)
        if (!analysis_windows_[i - 1]->alive()) analysis_windows_.erase(analysis_windows_.begin() + static_cast<long>(i - 1));
    if (channels_window_ && !channels_window_->alive()) channels_window_.reset();
    for (auto& w : analysis_windows_) w->on_refresh();
    if (channels_window_) channels_window_->on_refresh();
}

void MainWindow::close_companions()
{
    for (auto& w : analysis_windows_) w->close();
    if (channels_window_) channels_window_->close();
    if (help_window_) help_window_->close();
    analysis_windows_.clear();
    channels_window_.reset();
    help_window_.reset();
}

// ------------------------------------------------------------------ live updates paced by GTK's frame clock

void MainWindow::update_refresh()
{
    const bool need = controller_->needs_periodic_refresh();
    if (need && tick_id_ == 0) {
        tick_id_ = gtk_widget_add_tick_callback(window_, [](GtkWidget*, GdkFrameClock* clock, gpointer data) -> gboolean {
            return static_cast<MainWindow*>(data)->on_tick(clock) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        }, this, nullptr);
        last_update_us_ = 0;
        window_start_us_ = now_us();
        meter_window_ = Meter();
        GdkFrameClock* clock = gtk_widget_get_frame_clock(window_);
        if (clock && !paint_hooked_) {
            paint_hooked_ = true;
            g_signal_connect(clock, "layout", G_CALLBACK(+[](GdkFrameClock*, gpointer d) { static_cast<MainWindow*>(d)->layout_mark_us_ = now_us(); }), this);
            g_signal_connect(clock, "after-paint", G_CALLBACK(+[](GdkFrameClock*, gpointer d) {
                MainWindow* self = static_cast<MainWindow*>(d);
                if (self->layout_mark_us_ != 0 && self->tick_id_ != 0) self->meter_window_.paint += now_us() - self->layout_mark_us_;
                self->layout_mark_us_ = 0;
            }), this);
        }
    }
}

bool MainWindow::on_tick(GdkFrameClock* clock)
{
    if (!controller_->needs_periodic_refresh()) {
        tick_id_ = 0;
        controller_->refresh();   // final sample
        apply_view(false);
        refresh_companions();
        return false;
    }
    const int64_t frame_us = clock ? gdk_frame_clock_get_frame_time(clock) : now_us();
    ++meter_window_.frames;
    if (!ladder_ready_) {
        tier_native_hz_ = 60;
        ladder_ = app::make_display_ladder(tier_native_hz_);
        tier_params_ = app::DisplayTierParams();
        const int processors = usable_processors();
        tier_params_.budget = app::display_budget_for_processors(processors);
        tier_params_.behind_backoff = processors <= 2;
        ladder_ready_ = true;
        if (options_.display_fixed_tier >= 0) set_display_tier(std::min(options_.display_fixed_tier, ladder_.count - 1), now_us(), "fixed by test switch");
    }
    // The tier's update rate is a cap on the frame rate: skip frames that come too soon.
    const int rate = app::display_tier_rate(ladder_, tier_state_.tier, tier_native_hz_);
    const int64_t interval = 1000000 / std::max(1, rate);
    if (last_update_us_ != 0 && frame_us - last_update_us_ < interval * 85 / 100) {
        adapt_display(now_us());
        return tick_id_ != 0;
    }
    last_update_us_ = frame_us;
    refresh_tick();
    return tick_id_ != 0;
}

void MainWindow::refresh_tick()
{
    const int64_t start = now_us();
    stats_phase_ ^= 1u;
    controller_->refresh();
    apply_view(false);
    show_pending_messages();
    refresh_companions();
    const int64_t end = now_us();
    meter_window_.tick += end - start;
    ++meter_window_.updates;
    if (!controller_->needs_periodic_refresh()) {
        tick_id_ = 0;   // the next tick call has nothing to do; remove it now
        return;
    }
    adapt_display(end);
}

void MainWindow::set_display_tier(int tier, int64_t now, const char* why)
{
    if (tier == tier_state_.tier && ladder_.count > 0 && tier_changes_ > 0) return;
    char line[160];
    std::snprintf(line, sizeof(line), "tier %d->%d rate=%d half_stats=%d (%s)\n", tier_state_.tier, tier,
        app::display_tier_rate(ladder_, tier, tier_native_hz_), ladder_.half_statistics[tier] ? 1 : 0, why);
    tier_log_ += line;
    ++tier_changes_;
    tier_state_.tier = tier;
    (void)now;
}

// Once per half second of live updates: turn what the interface thread spent drawing into a tier (display_tier.h).
void MainWindow::adapt_display(int64_t now)
{
    if (now - window_start_us_ < 500000) return;
    const Meter w = meter_window_;
    meter_window_ = Meter();
    const int64_t window_us = now - window_start_us_;
    window_start_us_ = now;
    if (options_.display_fixed_tier >= 0 || !options_.adaptive_display) return;
    // The screen's rate: the frame-clock ticks counted over the window (every tick counts, also the skipped ones).
    const int native = static_cast<int>(std::lround(w.frames * 1000000.0 / std::max<int64_t>(1, window_us)));
    if (native > 20 && (native * 100 > tier_native_hz_ * 115 || native * 115 < tier_native_hz_ * 100)) {
        tier_native_hz_ = native;
        ladder_ = app::make_display_ladder(native);
        if (tier_state_.tier >= ladder_.count) tier_state_.tier = ladder_.count - 1;
    }
    app::DisplaySample sample;
    sample.window_us = window_us;
    sample.updates = static_cast<int>(w.updates);
    sample.statistics_us = w.stats_set;
    sample.moving_us = std::max<int64_t>(0, w.tick - w.stats_set) + w.paint;
    sample.behind = tier_params_.behind_backoff &&
        controller_->engine().snapshot().current_lag_microseconds >= tier_params_.behind_lag_us;
    app::DisplayTierState next = app::next_display_tier(ladder_, tier_native_hz_, tier_state_, sample, tier_params_);
    if (next.tier != tier_state_.tier) {
        char why[96];
        std::snprintf(why, sizeof(why), sample.behind ? "playback behind, drawing %.1f%%" : "drawing %.1f%% of the interface thread",
            100.0 * app::display_busy_share(sample));
        const int tier = next.tier;
        next.tier = tier_state_.tier;
        tier_state_ = next;
        set_display_tier(tier, now, why);
        tier_state_.tier = tier;
    }
    else tier_state_ = next;
}

// ------------------------------------------------------------------ closing and pictures

bool MainWindow::on_delete()
{
    if (closing_) return FALSE;
    close_companions();
    if (!controller_->shutdown()) {
        show_pending_messages();
        g_timeout_add(200, [](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->on_delete(); return G_SOURCE_REMOVE; }, this);
        return TRUE;   // the output is still stopping; try again soon
    }
    closing_ = true;
    if (tick_id_ != 0) { gtk_widget_remove_tick_callback(window_, tick_id_); tick_id_ = 0; }
    return FALSE;
}

void MainWindow::quit_now()
{
    if (modal_ || routing_window_) {   // leave the dialog first; the window goes in a later turn of the main loop
        gtk_dialog_response(GTK_DIALOG(modal_ ? modal_ : routing_window_), GTK_RESPONSE_CANCEL);
        g_idle_add([](gpointer d) -> gboolean { static_cast<MainWindow*>(d)->quit_now(); return G_SOURCE_REMOVE; }, this);
        return;
    }
    if (on_delete() == FALSE) gtk_widget_destroy(window_);
}

void MainWindow::capture()
{
    GtkWidget* target = window_;
    const std::string& want = options_.capture_window;
    if (want == "dialog" || (want == "auto" && modal_)) { if (modal_) target = modal_; }
    else if (want == "help" && help_window_) target = help_window_->widget();
    else if (want == "analysis" && !analysis_windows_.empty()) target = analysis_windows_[0]->widget();
    else if (want == "channels" && channels_window_) target = channels_window_->widget();
    else if (want == "routing" && routing_window_) target = routing_window_;
    const int w = gtk_widget_get_allocated_width(target), h = gtk_widget_get_allocated_height(target);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    gtk_widget_draw(target, cr);
    draw_tree_headers(target, target, cr);
    cairo_destroy(cr);
    GdkPixbuf* pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, w, h);
    GError* error = nullptr;
    if (!pixbuf || !gdk_pixbuf_save(pixbuf, options_.capture_file.c_str(), "png", &error, nullptr)) {
        std::fprintf(stderr, "midibn-gtk: capture failed: %s\n", error ? error->message : "no pixels");
        exit_code_ = 1;
        if (error) g_error_free(error);
    }
    else std::printf("captured %s (%dx%d)\n", options_.capture_file.c_str(), w, h);
    if (pixbuf) g_object_unref(pixbuf);
    cairo_surface_destroy(surface);
    quit_now();
}

// ------------------------------------------------------------------ the scripted check (output None only)

void MainWindow::smoke_check(bool ok, const std::string& what)
{
    std::printf("SMOKE %s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++smoke_failures_;
}

void MainWindow::smoke_step()
{
    const int64_t elapsed = now_us() - smoke_stage_since_us_;
    auto next = [&] { ++smoke_stage_; smoke_stage_since_us_ = now_us(); };
    const app::PlayerView& v = controller_->view();
    switch (smoke_stage_) {
    case 0:   // wait for the load
        if (controller_->has_song() && !controller_->loading()) {
            smoke_check(true, "file loaded: " + v.file_text);
            controller_->select_output(0);
            smoke_check(controller_->output_selection() == 0 && v.output_items.size() >= 1 && v.output_items[0] == "None", "output is None");
            smoke_events_before_ = controller_->engine().snapshot().processed_events;
            smoke_stats_before_ = v.statistics.values[4];
            if (controller_->output_selection() == 0) { controller_->play_clicked(); after_action(); next(); }
            else smoke_stage_ = 99;
        }
        else if (elapsed > 15000000) { smoke_check(false, "file did not load in 15 s"); smoke_stage_ = 99; }
        break;
    case 1:   // playing: the statistics must move
        if (elapsed > 900000) {
            controller_->refresh();
            const int64_t events = controller_->engine().snapshot().processed_events;
            smoke_check(v.state_text == "Playing" || v.state_text == "Completed", "state is " + v.state_text);
            smoke_check(events > smoke_events_before_, "events were processed while playing");
            smoke_check(controller_->view().statistics.values[4] != smoke_stats_before_, "the statistics block changed (" + controller_->view().statistics.values[4] + ")");
            smoke_check(controller_->view().position_us > 0, "the position moved");
            controller_->play_clicked();   // Pause
            after_action();
            next();
        }
        break;
    case 2:
        if (elapsed > 150000) {
            smoke_check(controller_->view().state_text == "Paused" || controller_->view().state_text == "Completed", "state after Pause is " + controller_->view().state_text);
            const int64_t before = controller_->view().position_us;
            controller_->seek_by(-5000000);
            controller_->seek_to(std::max<int64_t>(0, std::min<int64_t>(controller_->view().duration_us, 1000000)));
            after_action();
            smoke_check(controller_->view().position_us != before || before == 1000000, "Seek moved the position");
            controller_->select_rate_choice(app::RateModelChoice::ProcessingTime);
            after_action();
            smoke_check(controller_->view().rate_choice == app::RateModelChoice::ProcessingTime, "Rate model changed to Processing time per event");
            controller_->play_clicked();
            after_action();
            next();
        }
        break;
    case 3:
        if (elapsed > 500000) {
            apply_scale(150, true);
            smoke_check(scale_percent_ == 150, "scale 150%");
            apply_scale(75, true);
            apply_scale(100, true);
            smoke_check(scale_percent_ == 100, "scale back to 100%");
            show_help(2);
            smoke_check(help_window_ && help_window_->alive(), "Help window opened at topic 2");
            show_help(5);   // an open Help window is reused and jumps to the topic
            open_analysis();
            open_channels();
            smoke_check(analysis_windows_.size() == 1 && analysis_windows_[0]->alive(), "Analysis window opened");
            smoke_check(channels_window_ && channels_window_->alive(), "Channels window opened");
            controller_->stop_clicked();
            after_action();
            smoke_check(controller_->view().state_text == "Stopped", "state after Stop is " + controller_->view().state_text);
            smoke_check(controller_->view().position_us == 0, "position back at 0 after Stop");
            next();
        }
        break;
    case 4:
        if (elapsed > 900000) {
            apply_scale(150, true);   // the companion windows follow the scale
            apply_scale(100, true);
            close_companions();
            smoke_check(analysis_windows_.empty() && !channels_window_ && !help_window_, "companion windows closed");
            smoke_check(logged_problems() == 0, "no GLib/GTK warnings or criticals (" + std::to_string(logged_problems()) + ")");
            std::printf("SMOKE tier changes: %d%s%s\n", tier_changes_, tier_log_.empty() ? "" : " ", tier_log_.c_str());
            smoke_stage_ = 99;
        }
        break;
    default:
        std::printf("SMOKE %s\n", smoke_failures_ == 0 ? "PASS" : "FAIL");
        exit_code_ = smoke_failures_ == 0 ? 0 : 1;
        g_source_remove(smoke_timer_);
        smoke_timer_ = 0;
        quit_now();
        break;
    }
}

// ------------------------------------------------------------------ GLib message counter

namespace {
int g_problems = 0;
}

void install_log_counter()
{
    g_log_set_writer_func([](GLogLevelFlags level, const GLogField* fields, gsize n, gpointer) -> GLogWriterOutput {
        if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR)) ++g_problems;
        return g_log_writer_standard_streams(level, fields, n, nullptr);
    }, nullptr, nullptr);
}

int logged_problems() { return g_problems; }

}   // namespace gtkui
}   // namespace midibn
