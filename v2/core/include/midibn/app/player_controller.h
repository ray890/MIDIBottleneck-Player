// The main window's behaviour, without any presentation (Milestone 6).
//
// A port of the non-UI logic of v1's MainForm.cs: load orchestration with the
// large-file decision, output selection, the Rate-model state machine, queue
// and overflow settings, play preconditions, transport, statistics, and the
// state of every control. A frontend (Win32 today) feeds user actions in and
// draws `PlayerView`. Nothing here calls the operating system: the platform
// is reached through `PlayerEnvironment`.
//
// Threading: every method runs on the frontend's UI thread. The load runs on a
// worker thread and the engine reports from its own worker thread; both only
// record a flag and call `PlayerEnvironment::wake_ui` (thread-safe), after
// which the UI thread calls `pump()`.
#pragma once

#include "midibn/app/effective_speed.h"
#include "midibn/app/error_text.h"
#include "midibn/app/rate_model.h"
#include "midibn/app/settings_store.h"
#include "midibn/analysis/workload_analysis.h"
#include "midibn/engine/playback_engine.h"
#include "midibn/overrides/override_snapshot.h"
#include "midibn/routing/track_routing.h"
#include "midibn/format/net_format.h"
#include "midibn/midi_loader.h"
#include "midibn/midi_output.h"
#include "midibn/platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace midibn {
namespace app {

// The answer of the Direct WinMM check for one output device (R1): can the notes be sent straight to its driver?
// `reason` (plain language, first letter lower case) says why not.
struct DirectProbeResult {
    bool usable = false;
    std::string reason;
};

struct PlayerEnvironment {
    std::shared_ptr<platform::EngineServices> services;
    platform::FileSystem* files = nullptr;
    // WinMM device list (item 0 of the output list is always "None").
    std::function<std::vector<platform::OutputDeviceInfo>()> list_devices;
    // DD-057: when true the controller calls list_devices() at the first moment it needs the list (the first refresh()
    // after construction, the output selection, starting playback) instead of in its constructor, and until then its
    // view shows only "None". The Windows frontend starts the listing on another thread first and builds its window
    // meanwhile. Off by default: the controller then lists the devices in its constructor, as before.
    bool defer_device_listing = false;
    std::function<std::shared_ptr<platform::MidiOutput>()> create_winmm;
    std::function<std::shared_ptr<platform::MidiOutput>()> create_kdmapi;
    // Direct WinMM driver access (DD-085): the WinMM device played through the driver's own modMessage entry point.
    std::function<std::shared_ptr<platform::MidiOutput>()> create_winmm_direct;
    // R1: decides whether Direct mode can be used for a WinMM device id without opening the device. The controller runs it
    // on its own worker thread (it may block on a driver), once per device per session, when the output selection
    // changes; the box stays enabled until the answer arrives. Empty: nothing is known before the first Play.
    std::function<DirectProbeResult(uint32_t winmm_device_id)> probe_direct;
    int pointer_size = 8;                          // CLR-style pointer size for v1's warning projection
    int64_t memory_warning_bytes = 0;              // large-MIDI memory warning threshold; 0 = built-in for the pointer size (32-bit: half the real address space, DD-114 draft)
    std::function<int64_t()> private_bytes;        // process private commit, for the loading telemetry
    std::function<void()> wake_ui;                 // thread-safe: ask the UI thread to call pump()
    format::NumberCulture culture;
    // Remembered settings (Step 4): applied inside the constructor, before the first view. The output is matched by name
    // when the device list is read (an unknown name leaves the normal default). Off: everything starts from its default.
    bool has_initial_settings = false;
    RememberedSettings initial_settings;
};

// A message the frontend shows in a message box (v1 MessageBox.Show).
struct UiMessage {
    enum Kind { Information, Warning, Error };
    Kind kind = Information;
    std::string title;
    std::string text;
};

enum class ScrubKind { None, Queue, QueueAge, Rate };

// The engine could not apply a channel control (v1 PlaybackEngine.ChannelControlFailed); the Channels window shows it.
struct ChannelControlFailure {
    int channel = -1;
    ChannelAttribute attribute = ChannelAttribute::BankMsb;
    bool is_mute = false;       // the failed request was a mute / unmute: `attribute` is then only a placeholder (DD-033)
    std::string message;
};

struct NumericFieldView {
    int64_t value = 0;
    int64_t minimum = 0;
    int64_t maximum = 0;
    int64_t increment = 1;
    bool enabled = true;
    bool visible = true;
    // Pointer-gesture arithmetic when `scrub` is None (v1 ConfigureNumeric):
    int coarse_units = 1, coarse_pixels = 1, fine_units = 1, fine_pixels = 1;
    ScrubKind scrub = ScrubKind::None;
    std::string name;      // accessible name
    std::string tooltip;
    bool operator==(const NumericFieldView& o) const
    {
        return value == o.value && minimum == o.minimum && maximum == o.maximum && increment == o.increment &&
            enabled == o.enabled && visible == o.visible && coarse_units == o.coarse_units &&
            coarse_pixels == o.coarse_pixels && fine_units == o.fine_units && fine_pixels == o.fine_pixels &&
            scrub == o.scrub && name == o.name && tooltip == o.tooltip;
    }
};

struct StatisticsText {
    // Timeline / output, Queue now / maximum, Maximum rate, Output rate,
    // Events sent / dropped, Effective speed, Maximum lag, Current lag.
    std::string values[8];
    bool queue_limited = false;
    bool virtual_queue = false;
    bool age_pressure = false;
    int64_t occupied = 0;
    int64_t limit = 1;
    bool overflow_pulse = false;
    bool per_note_gate = false;   // wording of the speed/rate tooltips
    std::string speed_window_description = "250 ms";
    bool operator==(const StatisticsText& o) const
    {
        for (int i = 0; i < 8; ++i)
            if (values[i] != o.values[i]) return false;
        return queue_limited == o.queue_limited && virtual_queue == o.virtual_queue && age_pressure == o.age_pressure &&
            occupied == o.occupied && limit == o.limit && overflow_pulse == o.overflow_pulse &&
            per_note_gate == o.per_note_gate && speed_window_description == o.speed_window_description;
    }
};

struct SystemMenuState {
    bool apply_queue_limit_checked = false, apply_queue_limit_enabled = true;
    bool queue_age_checked = false, queue_age_enabled = true;
    bool chase_checked = true, chase_enabled = true;
    bool preserve_checked = false, preserve_enabled = true;
    bool output_paced_checked = false, output_paced_enabled = true;
    bool scale_interval_checked = false, scale_interval_enabled = true;
    bool track_routing_checked = false, track_routing_enabled = false;
    bool operator==(const SystemMenuState& o) const
    {
        return apply_queue_limit_checked == o.apply_queue_limit_checked && apply_queue_limit_enabled == o.apply_queue_limit_enabled &&
            queue_age_checked == o.queue_age_checked && queue_age_enabled == o.queue_age_enabled &&
            chase_checked == o.chase_checked && chase_enabled == o.chase_enabled &&
            preserve_checked == o.preserve_checked && preserve_enabled == o.preserve_enabled &&
            output_paced_checked == o.output_paced_checked && output_paced_enabled == o.output_paced_enabled &&
            scale_interval_checked == o.scale_interval_checked && scale_interval_enabled == o.scale_interval_enabled &&
            track_routing_checked == o.track_routing_checked && track_routing_enabled == o.track_routing_enabled;
    }
};

struct PlayerView {
    // File and output
    std::string open_button_text = "Open MIDI...";
    bool loading = false;
    std::string file_text = "No file loaded";
    std::string file_tooltip;
    std::string file_info;          // "\r\n" separated lines when compact
    std::string file_info_tooltip;
    int load_overall_permille = 0;
    int load_stage_permille = 0;
    std::string load_status;        // two lines
    std::string load_tooltip;
    std::string state_text = "Stopped";
    bool kdmapi_checked = false;
    // The "Direct WinMM" checkbox (P12-CHK; it replaces the status text and the system-menu item). The tick is
    // session-only and off at the start. `direct_enabled` is false (greyed out, the tick kept) when Direct mode cannot be
    // used for the selected output: output None, KDMAPI ticked, no direct output on this system, or the selected device
    // was found unusable when playback last started. `direct_tooltip` says in plain sentences whether Direct mode is in
    // use, or why not, and what the user can do.
    bool direct_checked = false, direct_enabled = false;
    std::string direct_tooltip;
    // Unused since P12-CHK (always empty); the Linux window still reads them.
    std::string output_path_text;
    std::string output_path_tooltip;
    std::string output_path_short;
    std::vector<std::string> output_items;     // [0] = "None"
    int output_selected = 0;
    bool output_enabled = true;

    // Processing model
    bool queue_check_checked = false;
    bool queue_check_enabled = true;
    std::string queue_check_text = "Queue length limit:";
    std::string queue_check_tooltip;
    NumericFieldView queue_value;
    std::string events_label = "events";
    std::string overflow_label = "Overflow:";
    std::vector<std::string> overflow_items;
    int overflow_selected = 0;
    bool overflow_enabled = true;
    std::string overflow_tooltip;
    std::string rate_label = "Rate model:";
    RateModelChoice rate_choice = RateModelChoice::None;
    bool rate_forward_heading = false;
    std::string rate_tooltip;
    std::string service_value_label;
    NumericFieldView processing_value;
    std::string service_unit;
    bool service_unit_visible = false;
    bool din_visible = false;
    bool din_enabled = false;
    int slider_value = 0;
    bool slider_enabled = false;
    std::string slider_tooltip;

    // Playback
    std::string play_text = "Play";
    bool play_enabled = false;
    std::string play_tooltip;
    bool stop_enabled = false;
    bool seek_enabled = false;     // timeline, +-5 s, Analysis
    int64_t position_us = 0;
    int64_t duration_us = 0;

    // Statistics
    StatisticsText statistics;

    SystemMenuState menu;

    // True when everything except the changing readouts (statistics, position, state) is equal:
    // a frontend then only refreshes those readouts (display refresh while playing stays cheap).
    bool same_controls(const PlayerView& o) const
    {
        return open_button_text == o.open_button_text && loading == o.loading && file_text == o.file_text &&
            file_tooltip == o.file_tooltip && file_info == o.file_info && file_info_tooltip == o.file_info_tooltip &&
            load_overall_permille == o.load_overall_permille && load_stage_permille == o.load_stage_permille &&
            load_status == o.load_status && load_tooltip == o.load_tooltip && kdmapi_checked == o.kdmapi_checked &&
            output_path_text == o.output_path_text && output_path_tooltip == o.output_path_tooltip &&
            output_path_short == o.output_path_short && direct_checked == o.direct_checked &&
            direct_enabled == o.direct_enabled && direct_tooltip == o.direct_tooltip &&
            output_items == o.output_items && output_selected == o.output_selected && output_enabled == o.output_enabled &&
            queue_check_checked == o.queue_check_checked && queue_check_enabled == o.queue_check_enabled &&
            queue_check_text == o.queue_check_text && queue_check_tooltip == o.queue_check_tooltip &&
            queue_value == o.queue_value && events_label == o.events_label && overflow_label == o.overflow_label &&
            overflow_items == o.overflow_items && overflow_selected == o.overflow_selected &&
            overflow_enabled == o.overflow_enabled && overflow_tooltip == o.overflow_tooltip && rate_label == o.rate_label &&
            rate_choice == o.rate_choice && rate_forward_heading == o.rate_forward_heading && rate_tooltip == o.rate_tooltip &&
            service_value_label == o.service_value_label && processing_value == o.processing_value &&
            service_unit == o.service_unit && service_unit_visible == o.service_unit_visible && din_visible == o.din_visible &&
            din_enabled == o.din_enabled && slider_value == o.slider_value && slider_enabled == o.slider_enabled &&
            slider_tooltip == o.slider_tooltip && play_text == o.play_text && play_enabled == o.play_enabled &&
            play_tooltip == o.play_tooltip && stop_enabled == o.stop_enabled && seek_enabled == o.seek_enabled &&
            duration_us == o.duration_us && menu == o.menu;
    }
};

class PlayerController {
public:
    explicit PlayerController(const PlayerEnvironment& environment);
    ~PlayerController();
    PlayerController(const PlayerController&) = delete;
    PlayerController& operator=(const PlayerController&) = delete;

    // ---- layout input from the frontend
    void set_compact(bool compact);
    bool compact() const { return compact_; }
    void set_timeline_dragging(bool dragging) { timeline_dragging_ = dragging; }

    // ---- state for drawing
    const PlayerView& view();
    std::vector<UiMessage> take_messages();
    // True while the window must refresh at display rate (playing or loading).
    bool needs_periodic_refresh();

    // ---- events (UI thread)
    // Completes a finished load and handles the engine's events; call after wake_ui.
    void pump();
    // A pending large-file decision to show; the frontend then calls answer_large_file().
    bool large_file_decision_pending(std::string& title, std::string& heading, std::string& text);
    void answer_large_file(bool accepted);

    // ---- file
    void open_file(const std::string& pathUtf8);
    // Test-only (v1 ForceLargeFilePreflightForTests / ForceLargeFileWarningForTests).
    void set_force_large_file_checks_for_tests(bool preflight, bool warning)
    {
        force_preflight_for_tests_ = preflight;
        force_warning_for_tests_ = warning;
    }
    // Cancel while loading (wantsDialog stays false); otherwise asks for the file dialog.
    void open_button_clicked(bool& wantsDialog);
    void cancel_load();
    bool loading() const { return loading_; }
    bool has_song() const { return song_ != nullptr; }
    const std::string& song_path() const { return file_path_; }
    std::shared_ptr<const Song> song() const { return song_; }

    // ---- output
    void set_kdmapi(bool checked);
    // DD-085: play WinMM devices through the driver's own entry point instead of midiOutShortMsg. Off by default,
    // session-only. A device that cannot be driven that way plays through standard WinMM, and the status says so.
    void toggle_direct_winmm();
    bool direct_winmm() const { return direct_checked_; }
    // R1 (self-test and unit tests): drop the session's cached Direct mode checks and ask again for the selected output;
    // whether a check for the selected output is still running.
    void forget_direct_checks();
    bool direct_check_pending() const;
    void select_output(int index);
    // Reads the output device list again while stopped (devices come and go). The loaded file and all options stay; the selection
    // stays on the same device (by name), or becomes None with a warning when that device is gone. While playing or paused it only
    // gives an information message.
    void refresh_devices();
    int output_selection() const { ensure_devices(); return output_index_; }

    // ---- remembered settings (the controller's part; the frontend adds its own window options)
    // The values to save now. The output name is the user's last choice, kept even while that device is absent.
    RememberedSettings remembered_settings() const;
    // The one-time note about the settings file (plain sentence, details below it): shown in the file-information
    // area while no file is loaded, and in its tooltip.
    void set_settings_note(const std::string& text, const std::string& detail);

    // ---- transport
    void play_clicked();
    void stop_clicked();
    void seek_by(int64_t deltaUs);
    void seek_to(int64_t targetUs);
    // v1 PerformAnalysisSeek: seek; a paused player resumes after the seek.
    void analysis_seek(int64_t targetUs);
    void reset_statistics_clicked();
    int64_t selected_position() const { return selected_position_us_; }

    // ---- processing model
    void select_rate_choice(RateModelChoice choice);
    void set_queue_limit_checked(bool checked);
    void set_queue_value(int64_t value);
    void set_overflow_selection(int index);
    void set_processing_value(int64_t value);
    void set_slider(int sliderValue);
    void din_preset();
    // Scrub arithmetic of a mapped numeric field: start value and signed pixel travel.
    int64_t scrub_value(bool processingField, int64_t start, int64_t pixels);

    // ---- system-menu toggles
    void toggle_apply_queue_limit_without_slowdown();
    void toggle_queue_age_limit();
    void toggle_state_chase();
    void toggle_preserve_pause_seek();
    void toggle_output_paced_timeline();
    void toggle_scale_note_interval_by_traffic();

    // ---- statistics
    void set_effective_speed_window(int64_t microseconds);
    int64_t effective_speed_window() const { return effective_speed_.window_microseconds(); }
    // Samples the engine and rebuilds the statistics strings; call from the UI timer and after changes.
    void refresh();

    // ---- shutdown
    // Returns false (with a message queued) when the engine could not stop safely.
    bool shutdown();

    // ---- Track routing (v1 ApplyTrackRoute): stopped-only, session-only
    routing::RoutingSong routing_song() const;     // the loaded song as the planner reads it
    std::shared_ptr<const routing::CompiledTrackRoute> track_route() const { return track_route_; }
    routing::TrackRoutingSettings track_routing_settings() const { return track_routing_settings_; }
    bool track_routing_available();                // a file is loaded and playback is stopped
    // route == null restores the original routing. False with `error` set (v1 InvalidOperationException text).
    bool apply_track_route(std::shared_ptr<const routing::CompiledTrackRoute> route, bool explicitlyConfirmed,
        const routing::TrackRoutingSettings* settings, std::string& error);

    // ---- for the Analysis, Channels and Track routing windows
    // The loaded song as playback and Analysis see it (the routed view when a route is active).
    std::shared_ptr<engine::PlaybackSong> effective_song() const { return playback_song_; }
    // v1 CurrentAnalysisConfiguration (without the override projection).
    analysis::AnalysisConfiguration analysis_configuration();
    // v1 CaptureAnalysisOverrideSnapshot: the live forced values, 16 channels x 9 attributes.
    std::shared_ptr<const overrides::AnalysisOverrideSnapshot> capture_override_snapshot();
    // Counters the windows compare to learn that something changed (no events, no polling of the engine).
    uint64_t analysis_revision() const { return analysis_revision_; }   // a setting that Analysis shows changed
    uint64_t song_revision() const { return song_revision_; }           // loaded, unloaded or re-routed
    // v1 UpdateChannelSourceReadouts: the source value of each channel attribute at a position.
    bool chase_enabled() const { return chase_midi_state_; }
    int64_t playback_position_for_readouts();      // intended timeline while active, else the selected position
    engine::PlaybackEngine& engine() { return engine_; }
    // Platform services (threads, clock) for the windows' own worker tasks; the UI culture.
    const std::shared_ptr<platform::EngineServices>& services() const { return env_.services; }
    const format::NumberCulture& culture() const { return env_.culture; }
    std::function<void()> wake_ui() const { return env_.wake_ui; }
    // The Output rate readout in its long form ("12,345.6 events/sec"; the statistics cell shows a short
    // form in the compact layout), as the Analysis window's playback overlay shows it.
    const std::string& output_rate_text() const { return output_rate_text_; }
    const std::string& effective_speed_text() const { return effective_speed_text_; }
    // Channels window (Milestone 6): v1 MainForm.EngineChannelControlFailed queued for the window (at most 64 are
    // kept; the engine thread adds them and calls wake_ui); v1 ScheduleAnalysisRefresh after a forced channel
    // value changed; v1's text for windows that lost their song ("Loading new MIDI…", "MIDI loading cancelled.
    // No MIDI file is loaded.", "MIDI loading failed. No MIDI file is loaded." or "No MIDI file is loaded.").
    std::vector<ChannelControlFailure> take_channel_control_failures();
    void notify_overrides_changed() { schedule_analysis_refresh(); }
    std::string no_song_message() const;

private:
    void ensure_devices() const;
    struct Field {
        int64_t value = 0, minimum = 0, maximum = 0, increment = 1;
        void set_range(int64_t lo, int64_t hi)
        {
            minimum = lo;
            maximum = hi;
            if (value < lo) value = lo;
            if (value > hi) value = hi;
        }
        void set_value(int64_t v) { value = v < minimum ? minimum : v > maximum ? maximum : v; }
    };
    struct OverflowItem {
        engine::OverflowPolicy policy;
        const char* text;
    };
    struct LoadJob;
    struct Failure {
        std::string title;
        std::string message;
        bool worker_timeout = false;
    };

    // messages
    // A failure text of a lower layer: the plain sentence first, technical detail below it (error_text.h).
    void message(UiMessage::Kind kind, const std::string& title, const std::string& text);
    void message_explained(UiMessage::Kind kind, const std::string& title, const ErrorText& text);
    bool engine_failed(const engine::EngineError& error, const char* title);

    // processing model (MainForm.cs counterparts)
    engine::OverflowPolicy selected_overflow_policy() const;
    void populate_overflow_policies(bool ageMode, engine::OverflowPolicy requested);
    void overflow_policy_changed();
    void configure_queue_limit_controls();
    void set_queue_age_limit_enabled(bool enabled);
    void apply_rate_choice(RateModelChoice choice);
    void synchronize_rate_processing();
    void configure_service_controls();
    void apply_processing_microseconds(int64_t microseconds, bool fromNumeric);
    void apply_midi_bitrate(int64_t bitrate, bool fromNumeric);
    void apply_events_per_second(int64_t rate, bool fromNumeric);
    void update_processing_summary(int64_t microseconds);
    void update_bitrate_summary();
    void update_event_rate_summary(int64_t rate);
    bool is_unsupported_forward_queue_selection();
    bool engine_active();
    engine::ProcessingMode selected_processing_mode() const;
    bool restart_for_processing_mode_change(engine::PlaybackState previousState, const std::function<void()>& configureAfterStop);
    bool try_restart_for_processing_mode_change(engine::PlaybackState previousState,
        const std::function<void()>& configureAfterStop = std::function<void()>());
    void schedule_analysis_refresh();

    // output
    std::shared_ptr<platform::MidiOutput> open_selected_output(Failure& failure);
    void close_output(const std::shared_ptr<platform::MidiOutput>& output);
    void release_active_unless(const std::shared_ptr<platform::MidiOutput>& keep);
    void direct_status(bool& enabled, std::string& tooltip) const;
    // R1: the cached answer of the check for the selected output (false: unknown or pending).
    bool direct_known_unusable(std::string& reason) const;
    void start_direct_probe() const;   // starts the check for the selected output when it has not been asked yet
    void output_setting_changed();

    // song and load
    void begin_load(const std::string& path);
    void unload_current_song();
    void finish_load(std::shared_ptr<LoadJob> job);
    void update_load_status();
    void update_loading_file_telemetry(bool force);
    void reset_loading_telemetry_state();
    void begin_loading_decision_pause();
    void end_loading_decision_pause();
    void load_cleared_presentation();
    int64_t clock_microseconds();
    void reset_live_measurements();
    void restart_live_measurement_windows();
    void perform_seek(int64_t targetMicroseconds);

    PlayerEnvironment env_;
    std::shared_ptr<platform::MidiOutput> winmm_;
    std::shared_ptr<platform::MidiOutput> kdmapi_;
    std::shared_ptr<platform::MidiOutput> direct_;
    std::shared_ptr<platform::MidiOutput> null_;
    std::shared_ptr<platform::MidiOutput> active_output_;
    // The device list is filled by ensure_devices() (mutable: a const reader may be the first to need it).
    mutable std::vector<platform::OutputDeviceInfo> devices_;
    mutable std::vector<std::string> output_items_;
    mutable int output_index_ = 0;
    mutable std::string pending_output_name_;   // remembered output, matched when the device list is read
    std::string remembered_output_name_;        // the user's last output choice (saved)
    std::string settings_note_, settings_note_detail_;
    bool constructed_ = false;
    mutable bool devices_ready_ = true;   // false only while PlayerEnvironment::defer_device_listing has not been honoured yet
    bool kdmapi_checked_ = false;
    bool direct_checked_ = false;
    bool switching_output_ = false;
    // Result of the last attempt to open the selected device on the direct path (for the status line).
    enum class DirectOutcome { NotTried, InUse, Unavailable };
    DirectOutcome direct_outcome_ = DirectOutcome::NotTried;
    struct DirectProbeState;                    // R1: answers of the check per device id (shared with the worker threads)
    std::shared_ptr<DirectProbeState> probe_;
    int direct_outcome_device_ = -1;            // output list index the outcome belongs to
    std::string direct_outcome_text_;           // InUse: the output's own description; Unavailable: the reason

    std::shared_ptr<Song> song_;
    std::string file_path_;
    std::shared_ptr<engine::PlaybackSong> playback_song_;
    std::shared_ptr<engine::PlaybackSong> engine_song_;
    std::shared_ptr<const routing::CompiledTrackRoute> track_route_;
    routing::TrackRoutingSettings track_routing_settings_;
    uint64_t analysis_revision_ = 0;
    uint64_t song_revision_ = 0;
    int64_t selected_position_us_ = 0;

    EffectivePlaybackSpeed effective_speed_;
    RollingOutputRate output_rate_;
    int64_t last_dropped_events_ = 0;
    int64_t overflow_visible_until_us_ = 0;
    std::string output_error_;
    engine::PlaybackState last_statistics_state_ = engine::PlaybackState::Stopped;
    StatisticsText statistics_;
    std::string output_rate_text_ = "\xE2\x80\x94";
    std::string effective_speed_text_ = "\xE2\x80\x94";

    bool compact_ = false;
    bool timeline_dragging_ = false;

    // the state MainForm keeps in fields
    bool per_note_gate_ = false;
    bool delayed_note_off_ = false;
    bool note_attack_limit_ = false;
    bool output_rate_limit_ = false;
    bool apply_queue_limit_without_slowdown_ = false;
    bool chase_midi_state_ = true;
    bool preserve_queue_ = false;
    bool output_paced_ = false;
    bool scale_note_interval_ = false;
    bool queue_age_limit_enabled_ = false;
    bool interval_polyphony_limit_enabled_ = false;
    bool remembered_ordinary_queue_limit_enabled_ = false;
    int remembered_event_count_limit_ = engine::EngineCore::kDefaultQueueLengthLimit;
    int remembered_interval_polyphony_limit_ = engine::EngineCore::kDefaultIntervalPolyphonyLimit;
    int64_t remembered_queue_age_limit_ = engine::EngineCore::kDefaultQueueAgeLimitMicroseconds;
    engine::OverflowPolicy remembered_event_overflow_policy_ = engine::OverflowPolicy::DropNewest;
    engine::OverflowPolicy remembered_age_overflow_policy_ = engine::OverflowPolicy::ClearBufferAndCatchUp;
    RateModelChoice selected_rate_choice_ = RateModelChoice::None;
    models::ServiceDurationMode last_ordinary_service_mode_ = models::ServiceDurationMode::ProcessingTime;
    int64_t remembered_processing_us_ = 100;
    int64_t remembered_gate_us_ = 100;
    int64_t remembered_delayed_note_off_us_ = 100;
    int64_t remembered_ordinary_event_rate_ = 10000;
    int64_t remembered_attack_rate_ = 100;
    int64_t remembered_output_rate_ = 10000;

    // the controls' own values
    bool queue_check_ = false;
    Field queue_value_;
    std::vector<OverflowItem> overflow_items_;
    int overflow_selected_ = 0;
    Field processing_value_;
    int slider_value_ = 0;
    std::string queue_check_tooltip_;
    std::string queue_value_tooltip_;
    std::string queue_value_name_;
    std::string events_label_;
    std::string overflow_tooltip_;
    std::string processing_tooltip_;
    std::string slider_tooltip_;
    std::string processing_name_;

    // load
    std::shared_ptr<LoadJob> load_job_;
    bool loading_ = false;
    int load_generation_ = 0;
    std::string loading_file_name_;
    int64_t load_started_ticks_ = 0;
    int64_t load_pause_started_ticks_ = 0;
    int64_t load_paused_ticks_ = 0;
    int64_t last_telemetry_second_ = -1;
    std::string last_telemetry_text_;
    std::string last_load_status_;
    std::string last_tooltip_stage_;
    std::string loading_file_text_;
    int load_overall_permille_ = 0;
    int load_stage_permille_ = 0;
    std::string load_status_text_;
    std::string load_tooltip_text_;
    bool decision_shown_ = false;
    bool force_preflight_for_tests_ = false;
    bool force_warning_for_tests_ = false;

    std::vector<UiMessage> messages_;
    int no_song_reason_ = 0;    // 0 none, 1 loading cancelled, 2 loading failed
    std::mutex channel_failure_mutex_;
    std::vector<ChannelControlFailure> channel_failures_;
    std::atomic<int> ended_flag_{ 0 };
    std::mutex event_mutex_;
    std::vector<engine::EngineError> failures_;
    PlayerView view_;
    engine::PlaybackEngine engine_;   // last: destroyed first, so its worker cannot call a dead member
};

} // namespace app
} // namespace midibn
