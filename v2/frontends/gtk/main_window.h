// The main window of MIDIBottleneck Player v2 (GTK 3). Same features and behaviour as the Windows window, driven by the
// same portable PlayerController; only the presentation is GTK.
#pragma once

#include "companion.h"
#include "gtk_util.h"

#include "midibn/app/display_tier.h"
#include "midibn/app/player_controller.h"
#include "linux_platform.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace gtkui {

// Switches for tests and pictures (never needed in normal use).
struct Options {
    std::string open_file;
    int auto_close_ms = 0;
    bool play = false;                // start playback on output None once the file is loaded
    int select_rate = -1;             // Rate model index to select before playing
    int scale_percent = 100;
    bool scale_given = false;         // --scale was given (a remembered scale does not override it)
    int client_width = 0;
    bool has_position = false;
    int position_x = 0, position_y = 0;
    std::string capture_file;         // write a PNG of a window after the delay, then close
    int capture_after_ms = 1500;
    std::string capture_window = "auto";   // auto | main | dialog | help | about | analysis | channels | routing
    std::string show;                 // open one more window at start: help | about | analysis | channels | routing
    int help_topic = 0;
    bool force_large_warning = false;
    bool smoke = false;               // scripted end-to-end check on output None (prints SMOKE lines, exit code)
    bool force_one_core = false;      // test: act as a one-physical-core PC (shows the few-processor menu item)
    std::string settings_path;        // --settings FILE: remember the settings in this file (tests)
    bool no_settings = false;         // --no-settings: defaults, nothing read, nothing saved
    bool settings_change = false;     // test: change some options at start so the saved file can be checked
    bool adaptive_display = true;
    int display_fixed_tier = -1;
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool create(const Options& options);
    GtkWidget* widget() const { return window_; }
    int exit_code() const { return exit_code_; }

private:
    struct Controls;
    friend struct Controls;

    // construction
    void build_controls();
    void build_menu();
    void setup_environment();
    void install_events();

    // presentation
    void apply_view(bool force);
    void apply_scale(int percent, bool resize);
    void update_menu_states();
    void set_section_visible(bool processing, bool visible);
    void after_action();
    void show_pending_messages();
    void handle_wake();
    void choose_file();
    void open_dropped(const std::string& uri_list);
    bool on_key(GdkEventKey* event);
    int help_topic_for_focus();
    void show_speed_menu(GdkEvent* event);
    void custom_speed_window();
    bool large_file_dialog(const std::string& title, const std::string& heading, const std::string& text);
    void message_box(const std::string& title, const std::string& text, int kind);
    int run_dialog(GtkWidget* dialog);

    // live updates paced by the frame clock
    void update_refresh();
    bool on_tick(GdkFrameClock* clock);
    void refresh_tick();
    void adapt_display(int64_t now_us);
    void set_display_tier(int tier, int64_t now_us, const char* why);

    // windows
    void show_help(int topic);
    void show_about();
    void open_analysis();
    void open_channels();
    void open_routing();
    void refresh_companions();
    void close_companions();

    // closing, capture and the scripted check
    bool on_delete();
    void quit_now();
    void capture();
    void smoke_step();
    void smoke_check(bool ok, const std::string& what);

    GtkWidget* window_ = nullptr;
    Options options_;
    std::unique_ptr<posix::PosixFileSystem> files_;
    std::unique_ptr<app::PlayerController> controller_;
    std::unique_ptr<Controls> c_;
    CompanionHost host_;
    std::unique_ptr<CompanionWindow> help_window_;
    std::vector<std::unique_ptr<CompanionWindow>> analysis_windows_;
    std::unique_ptr<CompanionWindow> channels_window_;
    GtkWidget* routing_window_ = nullptr;   // the Track routing editor while it is open
    bool paint_hooked_ = false;
    app::PlayerView last_view_;
    bool view_valid_ = false;
    bool applying_view_ = false;
    int scale_percent_ = 100;
    bool show_processing_ = true, show_statistics_ = true;
    bool few_cpu_yield_ = false;
    std::unique_ptr<app::SettingsStore> settings_store_;   // null: nothing read or saved (tests, --no-settings)
    app::SettingsLoad remembered_;
    int few_cpu_stored_ = -1;          // -1 not chosen yet, 0 / 1 the user's choice
    bool settings_load_note_ = false, settings_failure_noted_ = false;
    app::RememberedSettings current_settings() const;
    void save_settings_if_changed();
    bool one_core_ = false;
    std::atomic<bool> wake_pending_{ false };
    bool large_dialog_open_ = false;
    GtkWidget* modal_ = nullptr;
    bool closing_ = false;
    int exit_code_ = 0;
    bool test_play_pending_ = false;
    std::string show_pending_;
    std::string layout_signature_;

    // live updates
    guint tick_id_ = 0;
    int64_t last_update_us_ = 0;
    unsigned stats_phase_ = 0;
    struct Meter {
        int64_t tick = 0, stats_set = 0, paint = 0, updates = 0, frames = 0;
    };
    Meter meter_window_;
    int64_t window_start_us_ = 0, layout_mark_us_ = 0;
    app::DisplayLadder ladder_;
    app::DisplayTierState tier_state_;
    app::DisplayTierParams tier_params_;
    int tier_native_hz_ = 60;
    int tier_changes_ = 0;
    bool ladder_ready_ = false;
    std::string tier_log_;

    // scripted check
    int smoke_stage_ = 0;
    int64_t smoke_stage_since_us_ = 0;
    int smoke_failures_ = 0;
    int64_t smoke_events_before_ = 0;
    std::string smoke_stats_before_;
    guint smoke_timer_ = 0;
};

// Counts GLib warnings and criticals (the scripted check fails on any); installed by the scripted check.
void install_log_counter();
int logged_problems();

}   // namespace gtkui
}   // namespace midibn
