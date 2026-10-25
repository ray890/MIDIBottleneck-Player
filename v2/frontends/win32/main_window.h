// The main window of MIDIBottleneck Player v2 (plain Win32).
#pragma once


#include "companion.h"
#include "controls.h"
#include "ui_common.h"

#include <shellapi.h>

#include "midibn/app/display_tier.h"
#include "midibn/app/player_controller.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace ui {

enum class BorderlessStyle { ClassicStrip, IntegratedHeader, AutoHideOverlay };

// Test-only switches (never needed in normal use).
struct TestOptions {
    std::wstring open_file;
    int auto_close_ms = 0;
    bool play = false;             // start playback on output None once the file is loaded
    int select_rate = -1;          // Rate model index to select before playing
    int scale_percent = 0;
    int borderless_style = -1;     // 0 classic, 1 integrated, 2 overlay (borderless on)
    std::wstring screenshot_file;  // write the window to a BMP after the settle time, then close
    int screenshot_delay_ms = 1500;
    int client_width = 0;
    bool force_large_risk = false;      // test: --force-large-warning-32 adds the 32-bit warning line to that warning (pictures only)
    bool force_large_warning = false;   // test: --force-large-warning shows the large-MIDI memory warning for any file
    bool has_position = false;     // test: --position X,Y puts the window's top-left corner there (screen pixels), so a
    int position_x = 0;            // measurement window opens directly on a chosen monitor
    int position_y = 0;
    int refresh_ms = 0;           // test: use the old Windows timer at this interval instead of display pacing
    int pace_mode = 0;             // test: 0 auto (display, else timer), 1 display, 2 timer
    int pace_max_hz = 0;           // test: cap of the display mode (0 = none)
    int pace_hz = 0;               // test: rate of the timer mode (0 = 60)
    bool record_refresh = false;   // test: time every live update for the report
    // Adaptive display rate (Step 4); on by default, the rest are test switches.
    bool adaptive_display = true;      // --adaptive-display off: never lower the display cost
    int display_fixed_tier = -1;       // --display-tier N: hold tier N and do not adapt (cost measurements)
    int display_cost_us = 0;           // --display-cost-us N[,S]: pretend every update costs N us (busy-wait), plus S us
    int display_stats_cost_us = 0;     //   when all the statistics are drawn (scaled by the share drawn)
    int display_budget_permille = 0;   // --display-budget-pct P: another budget (0 = the default)
    int display_behind_after_ms = -1;  // --display-pretend-behind MS: playback counts as behind from MS after it starts
    int display_cost_for_ms = 0;       // --display-cost-for-ms MS: the pretended cost ends MS after playback starts (shows release)
    int display_behind_until_ms = -1;  // --display-pretend-behind START,END: ... until END ms after it starts
    bool display_paint_all = false;    // --display-paint-all: the statistics block paints every cell on every paint (as before)
    std::wstring popup_file;       // test: write the opened Rate model popup to a BMP
    std::wstring thread_report_file;   // test: --thread-report FILE writes the per-thread table (priority, start module)
    int thread_report_delay_ms = 4000; // test: ... this long after playback starts (after the window opens without --play)
    int affinity_mask = 0;             // test: --affinity HEX limits this process to those logical processors (0 = unchanged)
    int few_cpu_normal = 0;            // test: --few-cpu normal  (v2 threads at Normal on a PC with at most 2 processors)
    int few_cpu_yield_ms = 0;          // test: --few-cpu yield=MS  (playback yields while behind)
    int direct_check_standin = 0;      // test: --direct-check own-driver|usable  (see MainWindow::direct_probe_standin)
    std::wstring settings_path;        // --settings FILE: remember the settings in this file (tests and portable use)
    bool no_settings = false;          // --no-settings: defaults, nothing read, nothing saved
    bool settings_change = false;      // test: with --selftest and --settings, change some options and report the saved state
    bool settings_default = false;     // test: --settings-default uses the real default location even in an automated launch (APPDATA is redirected by the test)
    bool selftest = false;
    std::wstring selftest_file;    // result lines
    std::wstring result_file;
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    static bool register_class(HINSTANCE instance);
    bool create(HINSTANCE instance, int show, const TestOptions& options);
    HWND hwnd() const { return window_; }
    // Message-loop hook: F1, Alt+Space in borderless mode. Returns true when consumed.
    bool pre_translate(MSG* message);
    // Message-loop hook: the time one dispatched message took (performance-counter ticks); painting is metered by window.
    void note_dispatch(const MSG& message, int64_t ticks);
    app::PlayerController* controller() { return controller_.get(); }

    // Test hooks
    int run_selftest();
    int run_settings_selftest();   // --selftest with --settings: the remembered-settings round trip only
    // The self-test's stand-in for the Direct WinMM check (R1): 0 = the real check; 1 = every device is "Windows' own
    // driver" (not usable); 2 = every device has a usable driver; 3 = like 1, answering 1.5 s late. Never set outside the self-test.
    static std::atomic<int> direct_probe_standin;

private:
    enum Id {
        IdGroupFile = 100, IdGroupProcessing, IdGroupPlayback, IdGroupStatistics,
        IdOpen, IdFileLabel, IdInfoLabel, IdLoadStatus, IdProgressOverall, IdProgressStage,
        IdOutputLabel, IdOutput, IdKdmapi, IdState,
        IdQueueCheck, IdQueueValue, IdEventsLabel, IdOverflowLabel, IdOverflow,
        IdRateLabel, IdRate, IdServiceLabel, IdProcessingValue, IdUnitLabel, IdDin, IdSlider,
        IdTimeline, IdPlay, IdStop, IdBack5, IdForward5, IdAnalysis, IdReset, IdStats,
        // New controls go here, after the existing ones: test tools (visual gate, player-e2e, watch-passages)
        // address controls by these numbers (IdOutput = 111 ... IdStats = 133).
        IdDirect,   // the "Direct WinMM" checkbox (134; the output-path status text, which had this number, is gone)
        IdCaptionBase = 200   // + style * 10 + {0 menu, 1 minimize, 2 maximize, 3 close}
    };
    enum SystemCommand {
        CmdAbout = 0x1F20, CmdAlwaysOnTop = 0x1F30, CmdApplyQueueLimit = 0x1F50, CmdChase = 0x1F60,
        CmdShowProcessing = 0x1F70, CmdShowStatistics = 0x1F80, CmdHelp = 0x1F90, CmdQueueAge = 0x1FA0,
        CmdScale50 = 0x1FB0, CmdScale75 = 0x1FC0, CmdScale100 = 0x1FD0, CmdScale125 = 0x1FE0,
        CmdScale150 = 0x1FF0, CmdScale200 = 0x2000, CmdPreserve = 0x2010, CmdScaleInterval = 0x2020,
        CmdTrackRouting = 0x2030, CmdBorderless = 0x2040, CmdOutputPaced = 0x2050, CmdFewCpuYield = 0x20A0,
        CmdStyleClassic = 0x2060, CmdStyleIntegrated = 0x2070, CmdStyleOverlay = 0x2080
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    // construction
    void build_controls();
    HWND make_button(int id, const wchar_t* text, DWORD style = BS_PUSHBUTTON);
    HWND make_combo(int id);
    HWND make_static_label(int id, int style);
    void set_all_fonts();
    void build_system_menu();
    void install_child_hooks();
    std::wstring tooltip_for(HWND control);

    // layout
    bool compute_compact(int clientWidth) const;
    int layout(int clientWidth, bool apply);
    void relayout();
    void apply_window_constraints();
    int non_client_height() const;
    int required_outer_height() const { return required_outer_height_; }
    int px_(int canonical) const;
    bool IsOverGroupControl(HWND focus, bool processing) const;
    void open_routing();

    // presentation
    void apply_view(bool force);
    void update_rate_combo_labels();
    void update_system_menu_states();
    void update_refresh_timer();
    void after_action();
    void show_pending_messages();
    void handle_wake();
    void choose_file();
    void open_dropped(HDROP drop);

    // window chrome
    void set_borderless(bool enabled);
    void set_borderless_style(BorderlessStyle style);
    void apply_chrome_visibility();
    void update_caption_glyphs();
    void show_system_menu_for_chrome();
    void toggle_maximize();
    LRESULT hit_test(LPARAM lParam);
    int resize_hit(POINT clientPoint) const;
    void update_overlay_visibility();
    void set_scale(int percent);
    void set_section_visible(bool processing, bool visible);
    void toggle_always_on_top();
    void show_speed_menu(POINT screenPoint);
    void show_help(int topic);
    int help_topic_for_focus();
    void show_about();
    void custom_speed_window();
    void open_analysis();
    void take_screenshot(const std::wstring& path);

    // state
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    TestOptions options_;
    UiContext ctx_;
    TooltipHost tooltips_;
    std::unique_ptr<win32::Win32FileSystem> files_;
    std::unique_ptr<app::PlayerController> controller_;

    // controls
    HWND group_[4] = {};
    HWND open_ = nullptr, file_label_ = nullptr, info_label_ = nullptr, load_status_ = nullptr;
    HWND progress_overall_ = nullptr, progress_stage_ = nullptr;
    HWND output_label_ = nullptr, output_ = nullptr, kdmapi_ = nullptr, state_ = nullptr;
    HWND direct_ = nullptr;   // the "Direct WinMM" checkbox, left of KDMAPI (P12-CHK)
    HWND queue_check_ = nullptr, queue_value_ = nullptr, events_label_ = nullptr, overflow_label_ = nullptr, overflow_ = nullptr;
    HWND rate_label_ = nullptr, rate_ = nullptr, service_label_ = nullptr, processing_value_ = nullptr, unit_label_ = nullptr;
    HWND din_ = nullptr, slider_ = nullptr;
    HWND timeline_ = nullptr, play_ = nullptr, stop_ = nullptr, back5_ = nullptr, forward5_ = nullptr, analysis_ = nullptr, reset_ = nullptr;
    HWND stats_ = nullptr;
    HWND caption_[3][4] = {};

    // presentation state
    app::PlayerView last_view_;
    bool view_valid_ = false;
    bool compact_ = false;
    bool in_layout_ = false;
    bool relayout_again_ = false;   // a size change arrived during the layout (DD-090)
    bool applying_view_ = false;
    bool info_in_file_tooltip_ = false;   // DD-103: the file information is hidden (too tight) and shown in the file name's tooltip
    int required_client_height_ = 0;
    int required_outer_height_ = 0;
    int refresh_interval_ms_ = 16;   // only for the old-timer test mode (--refresh-ms)
    int refresh_token_ = 0;          // DisplayPacer subscription while live updates run
    void refresh_tick();
    std::vector<int32_t> refresh_intervals_us_;   // test report: time between live updates
    int64_t refresh_last_qpc_ = 0, refresh_busy_qpc_ = 0, refresh_count_ = 0;

    // ---- adaptive display rate (display_tier.h) ----
    struct DisplayMeter {   // performance-counter ticks, summed
        int64_t tick = 0;        // the update itself (controller, bar, time, statistics values, companions)
        int64_t stats_set = 0;   // part of the tick spent setting the statistics values
        int64_t paint_timeline = 0, paint_stats = 0, paint_other = 0;
        int64_t updates = 0;
    };
    DisplayMeter meter_total_, meter_window_;
    app::DisplayLadder ladder_;
    app::DisplayTierState tier_state_;
    app::DisplayTierParams tier_params_;
    int tier_native_hz_ = 60;
    int64_t window_start_qpc_ = 0, tier_since_qpc_ = 0, subscribe_qpc_ = 0, qpc_frequency_ = 1;
    unsigned stats_phase_ = 0;           // alternates 0/1 in the half-statistics tier
    unsigned stats_cells_drawn_ = 0xFF;
    int64_t tier_time_qpc_[app::DisplayLadder::MaxTiers] = {};
    int tier_changes_ = 0;
    std::string tier_log_;               // test report: "t=1.5s 0->1 (over budget 12.3%)" lines
    void adapt_display(int64_t nowQpc);
    void set_display_tier(int tier, int64_t nowQpc, const char* why);
    void busy_wait_us(int us);
    bool pretend_cost_active() const;
    void set_statistics_metered(const app::StatisticsText& text);
    bool timer_running_ = false;
    bool show_processing_ = true, show_statistics_ = true;
    bool few_cpu_yield_ = false;   // system menu "Let other programs run while playback is behind" (one-core PCs only; remembered; default on only with one usable logical processor)
    // Remembered settings (app/settings_store.h): null when nothing is read or saved (tests, --no-settings).
    std::unique_ptr<app::SettingsStore> settings_store_;
    int few_cpu_stored_ = -1;              // -1 not chosen yet, 0 / 1 the user's choice (kept as read while the option is not offered)
    bool settings_failure_noted_ = false;
    bool settings_load_note_ = false;     // the note on screen came from reading the file (cleared once a save repairs it)
    app::RememberedSettings current_settings() const;
    void save_settings_if_changed();
    bool borderless_ = false;
    BorderlessStyle style_ = BorderlessStyle::ClassicStrip;
    bool overlay_visible_ = false;
    bool always_on_top_ = false;
    int scale_percent_ = 100;
    HMENU style_menu_ = nullptr, scale_menu_ = nullptr;
    RECT file_row_rect_ = {};   // header row (drag zone) in client coordinates
    RECT strip_rect_ = {};
    std::wstring last_rate_labels_[8];
    int last_rate_width_ = -1;
    HWND help_window_ = nullptr;
    HWND routing_window_ = nullptr;
    CompanionHost host_;
    std::vector<std::unique_ptr<CompanionWindow>> analysis_windows_;
    std::unique_ptr<CompanionWindow> channels_window_;
    void open_channels();
    void refresh_companions();
    void reap_companions();
    void close_companions();
    format::NumberCulture controller_culture_;
    int combo_closed_h_ = 0;
    bool place_hidden_[3][4] = {};
    std::wstring layout_signature_;
    DWORD play_started_tick_ = 0, play_completed_tick_ = 0;
    std::string report_text_;
    int test_dialog_answer_ = 0;       // self-test: 1 Continue, 2 Cancel
    bool test_dialog_seen_ = false;
    std::string compute_report();
    int64_t completed_events_ = 0;

public:
    std::string run_report() { return test_play_refusal_ + (report_text_.empty() ? compute_report() : report_text_); }

private:
    bool test_play_pending_ = false;
    std::string test_play_refusal_;   // set when the --play test switch refused to play (output not None)
    bool large_dialog_open_ = false;
};

} // namespace ui
} // namespace midibn
