// Everything an Analysis window does except drawing it (v1 DiagnosticsForm.cs): request
// generations and cancellation of stale work, the worker task, the preview, progress and the
// busy indicator, the per-window and per-song caches, the Resolution choice, the debounce of
// viewport and settings changes, the report text, the inspection text and the
// follow-the-player updates. It owns the WorkloadGraphModel. A frontend creates one per
// Analysis window, forwards user actions, calls on_refresh() whenever the main window
// refreshed and poll() when wake() was called or next_wake_milliseconds() elapsed, and shows
// the outputs (each text has a revision that changes only when the text changes).
//
// Threading: every method runs on the UI thread. Analysis runs on a worker thread
// (PlayerController::services()->start_thread); the worker only fills a mailbox and calls
// the `wake` function the frontend passed (thread-safe: it must post a message).
#pragma once

#include "midibn/app/analysis_cache.h"
#include "midibn/app/analysis_graph.h"
#include "midibn/app/analysis_resolution.h"
#include "midibn/app/analysis_summary.h"
#include "midibn/app/analysis_view.h"
#include "midibn/app/player_controller.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace app {

// Timer values from v1 (milliseconds).
const int kAnalysisBusyIndicatorDelayMs = 1000;
const int kAnalysisProgressIntervalMs = 100;
const int kAnalysisViewportDebounceMs = 180;
const int kAnalysisSettingsDebounceMs = 200;    // MainForm._analysisRefreshTimer

// The calculation status shown beside the header controls.
struct AnalysisBusyView {
    bool status_visible = false;
    std::string status_text = "Recalculating analysis\xE2\x80\xA6";
    bool progress_visible = false;
    int progress_permille = 0;
    bool cancel_visible = false;
    bool operator==(const AnalysisBusyView& o) const
    {
        return status_visible == o.status_visible && status_text == o.status_text && progress_visible == o.progress_visible &&
            progress_permille == o.progress_permille && cancel_visible == o.cancel_visible;
    }
};

struct AnalysisWindowHost {
    // v1 SeekRequested -> MainForm.PerformAnalysisSeek.
    std::function<void(int64_t microseconds)> seek;
    // The worker has something for the UI thread: post a message that leads to poll().
    std::function<void()> wake;
    // Monotonic microseconds; empty = the controller's platform clock.
    std::function<int64_t()> now_us;
};

class AnalysisWindowModel {
public:
    AnalysisWindowModel(PlayerController& controller, const AnalysisWindowHost& host);
    ~AnalysisWindowModel();
    AnalysisWindowModel(const AnalysisWindowModel&) = delete;
    AnalysisWindowModel& operator=(const AnalysisWindowModel&) = delete;

    // ---- life cycle
    // Attaches to the controller's current song and requests the first analysis (ShowAnalysisClicked).
    void start();
    // Cancels all work (v1 Dispose).
    void close();

    // ---- from the main window and the clock
    // The player refreshed: song replaced or loading (detach / attach), settings changed (200 ms later a new
    // request), the playback markers and the Playback statistics overlay.
    void on_refresh();
    // Worker results and due timers. Cheap when nothing is due.
    void poll();
    // Milliseconds until poll() has something to do on its own; -1 when only wake() can start it.
    int next_wake_milliseconds();

    // ---- user actions
    void cancel_calculation();                          // the Cancel button (CancelAnalysisCalculation)
    void select_resolution(int index);                  // the Resolution combo
    // The "Custom…" prompt: `text` is the typed milliseconds (decimal text, invariant); false and `error` when invalid.
    bool apply_custom_resolution(const std::string& milliseconds, std::string& error);
    void cancel_custom_resolution();                    // the prompt was dismissed
    bool custom_prompt_pending() const { return custom_prompt_pending_; }
    void set_follow(FollowTarget target) { graph_.set_follow(target); handle_graph_events(); }
    void set_playback_statistics_visible(bool value) { graph_.set_playback_statistics_visible(value); }
    void reset_zoom() { graph_.reset_zoom(); handle_graph_events(); }
    void set_show_event_counts(bool value);             // system menu
    bool show_event_counts() const { return show_event_counts_; }
    void set_project_forced_overrides(bool value);      // system menu; requests a new analysis at once
    bool project_forced_overrides() const { return project_forced_overrides_; }
    void seek_to_pin();                                 // the "Seek to pin" button
    bool seek_to_pin_enabled() const { return graph_.has_pinned(); }
    // Pointer events of the graph (client coordinates) and the graph's resize.
    void graph_mouse_move(int x, int y) { graph_.mouse_move(x, y); handle_graph_events(); }
    void graph_mouse_leave() { graph_.mouse_leave(); handle_graph_events(); }
    void graph_mouse_down(int x, int y) { graph_.mouse_down(x, y); handle_graph_events(); }
    void graph_mouse_up(int x, int y) { graph_.mouse_up(x, y); handle_graph_events(); }
    void graph_double_click(int x, int y) { graph_.mouse_double_click(x, y); handle_graph_events(); }
    void graph_wheel(int x, int y, int delta) { graph_.mouse_wheel(x, y, delta); handle_graph_events(); }
    void graph_resized(int width, int height) { graph_.set_client_size(width, height); handle_graph_events(); }
    // Runs the graph's 16 ms hover/repaint step; true when the graph must repaint.
    bool graph_dynamic_update();

    // ---- outputs
    WorkloadGraphModel& graph() { return graph_; }
    const WorkloadGraphModel& graph() const { return graph_; }
    bool attached() const { return song_ != nullptr; }
    const std::string& title() const { return title_; }
    const std::string& summary_text() const { return summary_; }
    const std::string& inspection_text() const { return inspection_; }
    const AnalysisBusyView& busy_view() const { return busy_view_; }
    const ResolutionSelector& resolution() const { return resolution_; }
    // Each changes when (and only when) the matching output changed.
    uint64_t title_revision() const { return title_revision_; }
    uint64_t summary_revision() const { return summary_revision_; }
    uint64_t inspection_revision() const { return inspection_revision_; }
    uint64_t busy_revision() const { return busy_revision_; }
    uint64_t resolution_revision() const { return resolution_revision_; }
    uint64_t attach_revision() const { return attach_revision_; }   // attached / detached
    // The shown analysis (null view while none).
    const AnalysisResultView& analysis() const { return analysis_; }
    bool has_completed_analysis() const { return has_completed_analysis_; }
    bool calculation_pending() const { return job_ != nullptr; }
    bool analysis_busy() const { return analysis_busy_; }
    int preview_publication_count() const { return preview_publications_; }

    // For tests: the song that is attached and the request counter.
    const std::shared_ptr<engine::PlaybackSong>& song() const { return song_; }
    int started_job_count() const { return started_jobs_; }

private:
    struct Job;
    struct JobEvent;

    static void run_job(const std::shared_ptr<Job>& job);
    void start_job(const std::shared_ptr<engine::PlaybackSong>& song, int64_t resolution,
        const std::shared_ptr<const analysis::AnalysisConfiguration>& requested, bool allowPreview, const std::string& key);
    void drain_job_events();
    int64_t clock_us() const;
    void attach(const std::shared_ptr<engine::PlaybackSong>& song);
    void detach(const std::string& message);
    void request_analysis(const analysis::AnalysisConfiguration& configuration,
        std::shared_ptr<const overrides::AnalysisOverrideSnapshot> overrideProjection);
    void request_from_controller();
    void update_analysis_display(const AnalysisResultView& view, bool completed);
    void update_analysis(const AnalysisResultView& view);
    void cancel_current_work();
    void schedule_resolution_refresh();
    void begin_busy();
    void end_busy();
    void show_busy_indicator();
    void refresh_progress();
    void apply_job_event(const std::shared_ptr<Job>& job, const JobEvent& event);
    void set_summary(const std::string& text);
    void set_title(const std::string& text);
    void set_busy_view(const AnalysisBusyView& view);
    void update_inspection(int64_t time, bool pinned);
    void handle_graph_events();
    void resolution_changed();
    void update_playback_state();
    void check_override_signature();
    int64_t selected_resolution() const;

    PlayerController& controller_;
    AnalysisWindowHost host_;
    bool closed_ = false;
    bool started_ = false;

    // the attached song
    std::shared_ptr<engine::PlaybackSong> song_;
    AnalysisSongInfo info_;
    std::shared_ptr<SongAnalysisCaches> caches_;
    LocalAnalysisCache local_cache_;

    // what is shown
    AnalysisResultView analysis_;
    bool has_analysis_ = false;
    bool has_completed_analysis_ = false;
    bool updating_analysis_ = false;
    std::shared_ptr<analysis::AnalysisConfiguration> pending_;
    WorkloadGraphModel graph_;
    ResolutionSelector resolution_;
    bool custom_prompt_pending_ = false;
    bool show_event_counts_ = false;
    bool project_forced_overrides_ = false;
    std::string title_, summary_, inspection_;
    AnalysisBusyView busy_view_;
    uint64_t title_revision_ = 1, summary_revision_ = 1, inspection_revision_ = 1, busy_revision_ = 1;
    uint64_t resolution_revision_ = 1, attach_revision_ = 1;

    // work
    std::shared_ptr<Job> job_;
    int generation_ = 0;
    int started_jobs_ = 0;
    int preview_publications_ = 0;
    bool analysis_busy_ = false;
    bool busy_indicator_visible_ = false;
    int displayed_progress_permille_ = 0;
    int64_t busy_delay_deadline_us_ = -1;
    int64_t progress_deadline_us_ = -1;
    int64_t debounce_deadline_us_ = -1;          // viewport / resolution (180 ms)
    int64_t settings_deadline_us_ = -1;          // controller settings (200 ms)
    std::vector<std::shared_ptr<Job>> retired_;  // cancelled jobs whose threads have not been joined yet

    // follow the controller
    uint64_t seen_song_revision_ = 0;
    uint64_t seen_analysis_revision_ = 0;
    bool detached_for_loading_ = false;
    std::string override_signature_;
    int64_t override_check_us_ = -1;
};

} // namespace app
} // namespace midibn
