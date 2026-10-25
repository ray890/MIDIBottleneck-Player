#include "midibn/app/analysis_window_model.h"

#include "midibn/app/analysis_format.h"
#include "midibn/format/display_format.h"
#include "midibn/user_messages.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>

namespace midibn {
namespace app {

using analysis::AnalysisConfiguration;
using analysis::ProjectionState;
using analysis::WorkloadAnalysis;

namespace {

std::string recalculating_text() { return std::string("Recalculating analysis") + format::kEllipsis; }

std::string join_dash(const char* prefix, const std::string& reason)
{
    return std::string(prefix) + " " + format::kEmDash + " " + reason;
}

} // namespace

// ---------------------------------------------------------------- job

struct AnalysisWindowModel::JobEvent {
    enum Kind { Preview, Completed, Failed, Cancelled } kind = Completed;
    std::shared_ptr<const WorkloadAnalysis> data;
    std::string type;
    std::string message;
};

struct AnalysisWindowModel::Job {
    int generation = 0;
    std::atomic<bool> cancelled{ false };
    std::shared_ptr<engine::PlaybackSong> song;
    std::shared_ptr<SongAnalysisCaches> caches;
    int64_t resolution = 0;
    std::shared_ptr<const AnalysisConfiguration> requested;
    bool allow_preview = false;
    std::string cache_key;
    std::function<void()> wake;

    std::mutex mutex;
    std::vector<JobEvent> events;
    bool has_progress = false;
    std::string stage;
    int overall_permille = 0;
    std::unique_ptr<platform::Thread> thread;

    void post(const JobEvent& event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        }
        if (wake) wake();
    }
};

// The worker's body (v1: Task.Factory.StartNew(analyzer(...)) and its continuation, reduced to a mailbox).
void AnalysisWindowModel::run_job(const std::shared_ptr<Job>& job)
{
    JobEvent finish;
    try {
        analysis::AnalysisSong input;
        input.events = job->song->events;
        input.duration_us = job->song->duration_us;
        analysis::AnalysisControl control;
        Job* raw = job.get();
        control.progress = [raw](const analysis::AnalysisProgress& p) {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->has_progress = true;
            raw->stage = p.stage ? p.stage : "";
            raw->overall_permille = p.overall_permille;
        };
        control.cancellation_requested = [raw] { return raw->cancelled.load(); };
        std::function<void(const WorkloadAnalysis&)> ready;
        if (job->allow_preview) {
            ready = [raw, &control](const WorkloadAnalysis& base) {
                // v1 workload.CreatePresentationAnalysis(requested, token): an independent copy of the workload.
                JobEvent preview;
                preview.kind = JobEvent::Preview;
                preview.data = std::make_shared<WorkloadAnalysis>(analysis::copy_workload(base, raw->requested, control));
                raw->post(preview);
            };
        }
        WorkloadAnalysis result = analysis::analyze(input, job->resolution, job->requested, job->caches->workloads, control, ready);
        finish.kind = JobEvent::Completed;
        finish.data = std::make_shared<WorkloadAnalysis>(std::move(result));
    }
    catch (const models::ModelException& exception) {
        finish.type = exception.type ? exception.type : "Exception";
        finish.message = exception.message;
        finish.kind = finish.type == "OperationCanceledException" ? JobEvent::Cancelled : JobEvent::Failed;
    }
    catch (const std::bad_alloc&) {
        finish.kind = JobEvent::Failed;
        finish.type = "OutOfMemoryException";
        finish.message = messages::out_of_memory();
    }
    catch (const std::exception& exception) {
        finish.kind = JobEvent::Failed;
        finish.type = "Exception";
        finish.message = exception.what();
    }
    job->post(finish);
}

// ---------------------------------------------------------------- construction

AnalysisWindowModel::AnalysisWindowModel(PlayerController& controller, const AnalysisWindowHost& host)
    : controller_(controller), host_(host), resolution_(controller.culture())
{
    graph_.set_culture(controller.culture());
    busy_view_.status_text = recalculating_text();
    inspection_ = kInspectionPlaceholder;
    summary_ = kAnalysisPendingSummary;
    title_ = std::string("MIDIBottleneck Player ") + format::kEmDash + " Analysis " + format::kEmDash + " no file";
    graph_.set_empty_message(kAnalysisCalculatingMessage);
}

AnalysisWindowModel::~AnalysisWindowModel() { close(); }

int64_t AnalysisWindowModel::clock_us() const
{
    if (host_.now_us) return host_.now_us();
    platform::Clock& clock = controller_.services()->clock();
    return format::stopwatch_ticks_to_microseconds(clock.now_ticks(), clock.frequency());
}

void AnalysisWindowModel::start()
{
    if (started_) return;
    started_ = true;
    seen_song_revision_ = controller_.song_revision();
    seen_analysis_revision_ = controller_.analysis_revision();
    std::shared_ptr<engine::PlaybackSong> song = controller_.effective_song();
    if (controller_.loading()) {
        detached_for_loading_ = true;
        detach(kAnalysisLoadingMessage);
    } else if (song) attach(song);
    else detach(kAnalysisNoFileMessage);
    update_playback_state();
}

void AnalysisWindowModel::close()
{
    if (closed_) return;
    closed_ = true;
    cancel_current_work();
    for (size_t i = 0; i < retired_.size(); ++i) {
        retired_[i]->cancelled = true;
        if (retired_[i]->thread) {
            if (!retired_[i]->thread->join(5000)) retired_[i]->thread->detach();
        }
    }
    retired_.clear();
    song_.reset();
    caches_.reset();
    local_cache_.clear();
}

// ---------------------------------------------------------------- outputs

void AnalysisWindowModel::set_summary(const std::string& text)
{
    if (summary_ == text) return;
    summary_ = text;
    ++summary_revision_;
}

void AnalysisWindowModel::set_title(const std::string& text)
{
    if (title_ == text) return;
    title_ = text;
    ++title_revision_;
}

void AnalysisWindowModel::set_busy_view(const AnalysisBusyView& view)
{
    if (busy_view_ == view) return;
    busy_view_ = view;
    ++busy_revision_;
}

void AnalysisWindowModel::resolution_changed() { ++resolution_revision_; }

void AnalysisWindowModel::update_inspection(int64_t time, bool pinned)
{
    if (!has_analysis_) return;
    std::string text = build_inspection_text(analysis_, show_event_counts_, time, pinned, controller_.culture());
    if (text == inspection_) return;
    inspection_ = text;
    ++inspection_revision_;
}

// ---------------------------------------------------------------- attach / detach

void AnalysisWindowModel::detach(const std::string& message)
{
    debounce_deadline_us_ = -1;
    cancel_current_work();
    end_busy();
    song_.reset();
    caches_.reset();
    analysis_ = AnalysisResultView();
    has_analysis_ = false;
    has_completed_analysis_ = false;
    pending_.reset();
    local_cache_.clear();
    custom_prompt_pending_ = false;
    std::string text = message.empty() ? std::string(kAnalysisNoFileMessage) : message;
    set_title(std::string("MIDIBottleneck Player ") + format::kEmDash + " Analysis " + format::kEmDash + " no file");
    set_summary(text);
    graph_.detach(text);
    handle_graph_events();
    if (inspection_ != kInspectionPlaceholder) {
        inspection_ = kInspectionPlaceholder;
        ++inspection_revision_;
    }
    ++attach_revision_;
}

void AnalysisWindowModel::attach(const std::shared_ptr<engine::PlaybackSong>& song)
{
    debounce_deadline_us_ = -1;
    cancel_current_work();
    end_busy();
    song_ = song;
    info_ = AnalysisSongInfo();
    info_.file_name = analysis_file_name(song->file_path);
    info_.duration_us = song->duration_us;
    std::shared_ptr<const Song> source = controller_.song();
    if (source) {
        info_.smf_format = source->format;
        info_.track_count = source->track_count;
        info_.ticks_per_quarter_note = source->division;
        info_.note_count = source->note_count;
        info_.file_size_bytes = source->file_size;
    }
    caches_ = song_analysis_caches(song_);
    analysis_ = AnalysisResultView();
    has_analysis_ = false;
    has_completed_analysis_ = false;
    pending_.reset();
    local_cache_.clear();
    set_title(std::string("MIDIBottleneck Player ") + format::kEmDash + " Analysis " + format::kEmDash + " " + info_.file_name);
    set_summary(kAnalysisPendingSummary);
    graph_.detach(kAnalysisCalculatingMessage);
    handle_graph_events();
    if (inspection_ != kInspectionPlaceholder) {
        inspection_ = kInspectionPlaceholder;
        ++inspection_revision_;
    }
    ++attach_revision_;
    request_from_controller();
}

// ---------------------------------------------------------------- requests

void AnalysisWindowModel::request_from_controller()
{
    std::shared_ptr<const overrides::AnalysisOverrideSnapshot> snapshot;
    if (project_forced_overrides_) snapshot = controller_.capture_override_snapshot();
    request_analysis(controller_.analysis_configuration(), snapshot);
}

void AnalysisWindowModel::request_analysis(const AnalysisConfiguration& configuration,
    std::shared_ptr<const overrides::AnalysisOverrideSnapshot> overrideProjection)
{
    if (closed_ || !song_) return;
    debounce_deadline_us_ = -1;
    pending_ = std::make_shared<AnalysisConfiguration>(configuration);
    pending_->override_projection = overrideProjection;
    override_signature_ = overrideProjection ? overrideProjection->signature() : std::string();
    int64_t resolution = selected_resolution();
    std::string key = analysis_cache_key(*pending_, resolution);
    std::shared_ptr<const WorkloadAnalysis> cached = local_cache_.find(key);
    bool fromShared = false;
    if (!cached) {
        cached = caches_->results.find(key);
        fromShared = cached != nullptr;
    }
    if (cached) {
        cancel_current_work();
        if (fromShared) local_cache_.remember(key, cached);
        update_analysis(AnalysisResultView::of(cached).with_configuration(std::make_shared<AnalysisConfiguration>(*pending_)));
        end_busy();
        return;
    }
    cancel_current_work();
    std::shared_ptr<const AnalysisConfiguration> requested = std::make_shared<AnalysisConfiguration>(*pending_);
    start_job(song_, resolution, requested, !has_completed_analysis_, key);
    begin_busy();
}

void AnalysisWindowModel::start_job(const std::shared_ptr<engine::PlaybackSong>& song, int64_t resolution,
    const std::shared_ptr<const AnalysisConfiguration>& requested, bool allowPreview, const std::string& key)
{
    std::shared_ptr<Job> job = std::make_shared<Job>();
    job->generation = generation_;
    job->song = song;
    job->caches = caches_;
    job->resolution = resolution;
    job->requested = requested;
    job->allow_preview = allowPreview;
    job->cache_key = key;
    job->wake = host_.wake;
    job_ = job;
    ++started_jobs_;
    platform::PlatformError error;
    job->thread = controller_.services()->start_thread([job] { AnalysisWindowModel::run_job(job); },
        platform::ThreadPriority::Normal, "MIDI analysis", error);
    if (!job->thread) {
        JobEvent failed;
        failed.kind = JobEvent::Failed;
        failed.type = "InvalidOperationException";
        failed.message = "Unable to start the analysis thread.";
        job->post(failed);
    }
}

int64_t AnalysisWindowModel::selected_resolution() const
{
    int64_t span = graph_.view_end() > graph_.view_start() ? graph_.view_end() - graph_.view_start() : 0;
    return resolution_.selected_microseconds(span, song_ ? song_->duration_us : 0, graph_.graph_area().width,
        has_analysis_ ? analysis_.bucket_us() : 0);
}

void AnalysisWindowModel::cancel_current_work()
{
    ++generation_;
    if (job_) {
        job_->cancelled = true;
        retired_.push_back(job_);
        job_.reset();
    }
}

void AnalysisWindowModel::cancel_calculation()
{
    debounce_deadline_us_ = -1;
    cancel_current_work();
    // Cancel retires the wanted request as well as the worker, so the graph's layout-only
    // resize when the progress controls disappear cannot recreate the cancelled configuration.
    if (has_completed_analysis_ && has_analysis_) {
        pending_ = std::make_shared<AnalysisConfiguration>(analysis_.configuration ? *analysis_.configuration : AnalysisConfiguration());
    } else {
        pending_.reset();
    }
    if (!has_completed_analysis_ && has_analysis_ && analysis_.state == ProjectionState::Pending) {
        update_analysis_display(analysis_.with_projection_state(ProjectionState::Cancelled), false);
    } else if (!has_analysis_) {
        set_summary(kAnalysisCancelledMessage);
        graph_.set_empty_message(kAnalysisCancelledMessage);
    }
    end_busy();
}

// ---------------------------------------------------------------- display

void AnalysisWindowModel::update_analysis(const AnalysisResultView& view) { update_analysis_display(view, true); }

void AnalysisWindowModel::update_analysis_display(const AnalysisResultView& view, bool completed)
{
    updating_analysis_ = true;
    analysis_ = view;
    has_analysis_ = true;
    has_completed_analysis_ = completed;
    if (!pending_) pending_ = std::make_shared<AnalysisConfiguration>(view.configuration ? *view.configuration : AnalysisConfiguration());
    set_summary(build_analysis_summary(info_, view, controller_.culture()));
    graph_.set_empty_message(std::string());
    graph_.set_analysis(view);
    if (resolution_.mode() == ResolutionMode::Auto) {
        resolution_.update_auto_label(view.bucket_us());
        resolution_changed();
    }
    if (graph_.has_inspected()) update_inspection(graph_.inspected_time(), graph_.has_pinned());
    handle_graph_events();
    updating_analysis_ = false;
}

void AnalysisWindowModel::handle_graph_events()
{
    std::vector<GraphEvent> events = graph_.take_events();
    for (size_t i = 0; i < events.size(); ++i) {
        const GraphEvent& e = events[i];
        if (e.kind == GraphEvent::ViewportChanged) schedule_resolution_refresh();
        else if (e.kind == GraphEvent::InspectionChanged) update_inspection(e.time_us, e.pinned);
        else {
            update_inspection(e.time_us, true);
            if (host_.seek) host_.seek(e.time_us);
        }
    }
}

bool AnalysisWindowModel::graph_dynamic_update()
{
    bool repaint = graph_.process_dynamic_updates(clock_us());
    handle_graph_events();
    return repaint;
}

void AnalysisWindowModel::seek_to_pin()
{
    if (graph_.has_pinned() && host_.seek) host_.seek(graph_.pinned_time());
}

void AnalysisWindowModel::set_show_event_counts(bool value)
{
    if (show_event_counts_ == value) return;
    show_event_counts_ = value;
    graph_.set_show_event_counts(value);
    if (graph_.has_inspected()) update_inspection(graph_.inspected_time(), graph_.has_pinned());
}

void AnalysisWindowModel::set_project_forced_overrides(bool value)
{
    if (project_forced_overrides_ == value) return;
    project_forced_overrides_ = value;
    override_check_us_ = -1;
    if (song_) request_from_controller();
}

// ---------------------------------------------------------------- resolution

void AnalysisWindowModel::select_resolution(int index)
{
    ResolutionSelector::Selection selection = resolution_.select(index);
    resolution_changed();
    if (selection == ResolutionSelector::Selection::PromptCustom) {
        custom_prompt_pending_ = true;
        return;
    }
    schedule_resolution_refresh();
}

bool AnalysisWindowModel::apply_custom_resolution(const std::string& milliseconds, std::string& error)
{
    CustomResolutionResult result = validate_custom_resolution(song_ != nullptr, song_ ? song_->duration_us : 0, milliseconds,
        controller_.culture());
    if (!result.ok) {
        error = result.error;
        return false;
    }
    resolution_.apply_custom(result.microseconds);
    custom_prompt_pending_ = false;
    resolution_changed();
    schedule_resolution_refresh();
    return true;
}

void AnalysisWindowModel::cancel_custom_resolution()
{
    resolution_.cancel_custom();
    custom_prompt_pending_ = false;
    resolution_changed();
}

void AnalysisWindowModel::schedule_resolution_refresh()
{
    if (!pending_ || closed_ || updating_analysis_ || !song_) return;
    int64_t proposed = selected_resolution();
    if (has_analysis_ && proposed == analysis_.bucket_us()) {
        AnalysisConfiguration shown = analysis_.configuration ? *analysis_.configuration : AnalysisConfiguration();
        if (analysis_cache_key(*pending_, proposed) == analysis_cache_key(shown, analysis_.bucket_us())) return;
    }
    begin_busy();
    debounce_deadline_us_ = clock_us() + static_cast<int64_t>(kAnalysisViewportDebounceMs) * 1000;
}

// ---------------------------------------------------------------- busy period

void AnalysisWindowModel::begin_busy()
{
    if (analysis_busy_) return;
    analysis_busy_ = true;
    busy_indicator_visible_ = false;
    displayed_progress_permille_ = 0;
    AnalysisBusyView view;
    view.status_text = recalculating_text();
    set_busy_view(view);
    int64_t now = clock_us();
    busy_delay_deadline_us_ = now + static_cast<int64_t>(kAnalysisBusyIndicatorDelayMs) * 1000;
    progress_deadline_us_ = now + static_cast<int64_t>(kAnalysisProgressIntervalMs) * 1000;
}

void AnalysisWindowModel::show_busy_indicator()
{
    if (!analysis_busy_) return;
    busy_indicator_visible_ = true;
    AnalysisBusyView view = busy_view_;
    view.status_visible = true;
    view.progress_visible = true;
    view.cancel_visible = true;
    set_busy_view(view);
    refresh_progress();
}

void AnalysisWindowModel::refresh_progress()
{
    if (!analysis_busy_ || !job_) return;
    int overall;
    std::string stage;
    {
        std::lock_guard<std::mutex> lock(job_->mutex);
        if (!job_->has_progress || job_->generation != generation_) return;
        overall = job_->overall_permille;
        stage = job_->stage;
    }
    displayed_progress_permille_ = std::max(displayed_progress_permille_, overall);
    if (busy_indicator_visible_) {
        AnalysisBusyView view = busy_view_;
        view.progress_permille = std::min(1000, displayed_progress_permille_);
        view.status_text = stage + " " + format::kEmDash + " " + format::format_n(displayed_progress_permille_ / 10.0, 1, controller_.culture()) + "%";
        set_busy_view(view);
    }
}

void AnalysisWindowModel::end_busy()
{
    analysis_busy_ = false;
    busy_indicator_visible_ = false;
    busy_delay_deadline_us_ = -1;
    progress_deadline_us_ = -1;
    AnalysisBusyView view = busy_view_;
    view.status_visible = false;
    view.progress_visible = false;
    view.cancel_visible = false;
    set_busy_view(view);
}

// ---------------------------------------------------------------- results

void AnalysisWindowModel::drain_job_events()
{
    if (!job_) return;
    std::shared_ptr<Job> job = job_;
    std::vector<JobEvent> events;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        events.swap(job->events);
    }
    for (size_t i = 0; i < events.size(); ++i) {
        if (job != job_ || closed_) break;
        apply_job_event(job, events[i]);
    }
}

void AnalysisWindowModel::apply_job_event(const std::shared_ptr<Job>& job, const JobEvent& event)
{
    if (job != job_ || job->generation != generation_) return;
    if (event.kind == JobEvent::Preview) {
        if (job->cancelled || has_completed_analysis_ || song_ != job->song) return;
        ++preview_publications_;
        update_analysis_display(AnalysisResultView::preview(event.data, job->requested), false);
        return;
    }
    // Any other event ends the job (v1 clears _analysisCancellation before looking at the task's state).
    job_.reset();
    retired_.push_back(job);
    if (event.kind == JobEvent::Cancelled) {
        if (debounce_deadline_us_ < 0) end_busy();
        return;
    }
    if (event.kind == JobEvent::Failed) {
        std::string reason = concise_analysis_failure(event.type, event.message);
        bool retainedPreview = !has_completed_analysis_ && has_analysis_ && analysis_.state == ProjectionState::Pending;
        if (retainedPreview) {
            update_analysis_display(analysis_.with_projection_failure(reason), false);
        } else if (!has_analysis_) {
            std::string text = "Analysis failed: " + reason;
            set_summary(text);
            graph_.set_empty_message(text);
        }
        end_busy();
        const AnalysisConfiguration& requested = *job->requested;
        std::string prefix;
        if (retainedPreview) {
            prefix = requested.output_rate_limit_enabled ? "Output rate projection failed"
                : requested.note_attack_limit_enabled ? "Note start rate projection failed"
                : requested.delayed_note_off_enabled ? "Delayed NoteOff projection failed"
                : requested.per_note_interval_gate_enabled ? "Per-note projection failed" : "Queue projection failed";
        } else {
            prefix = "Analysis failed";
        }
        AnalysisBusyView view = busy_view_;
        view.status_text = join_dash(prefix.c_str(), reason);
        view.status_visible = true;
        set_busy_view(view);
        return;
    }
    local_cache_.remember(job->cache_key, event.data);
    caches_->results.remember(job->cache_key, event.data);
    update_analysis(AnalysisResultView::of(event.data));
    if (debounce_deadline_us_ < 0) end_busy();
}

// ---------------------------------------------------------------- timers

void AnalysisWindowModel::poll()
{
    if (closed_) return;
    drain_job_events();
    for (size_t i = retired_.size(); i > 0; --i) {
        const std::shared_ptr<Job>& job = retired_[i - 1];
        if (!job->thread || !job->thread->is_alive()) {
            if (job->thread) job->thread->join();
            retired_.erase(retired_.begin() + static_cast<long>(i - 1));
        }
    }
    int64_t now = clock_us();
    if (debounce_deadline_us_ >= 0 && now >= debounce_deadline_us_) {
        debounce_deadline_us_ = -1;
        // v1 re-requests without the forced-override snapshot here (so zooming silently dropped the
        // projection); the pending request keeps its snapshot in v2.
        if (pending_) request_analysis(*pending_, pending_->override_projection);
    }
    if (settings_deadline_us_ >= 0 && now >= settings_deadline_us_) {
        settings_deadline_us_ = -1;
        if (song_) request_from_controller();
    }
    if (analysis_busy_ && busy_delay_deadline_us_ >= 0 && now >= busy_delay_deadline_us_) {
        busy_delay_deadline_us_ = -1;
        show_busy_indicator();
    }
    if (analysis_busy_ && progress_deadline_us_ >= 0 && now >= progress_deadline_us_) {
        progress_deadline_us_ = now + static_cast<int64_t>(kAnalysisProgressIntervalMs) * 1000;
        refresh_progress();
    }
}

int AnalysisWindowModel::next_wake_milliseconds()
{
    if (closed_) return -1;
    int64_t earliest = -1;
    auto consider = [&](int64_t deadline) {
        if (deadline >= 0 && (earliest < 0 || deadline < earliest)) earliest = deadline;
    };
    consider(debounce_deadline_us_);
    consider(settings_deadline_us_);
    if (analysis_busy_) {
        consider(busy_delay_deadline_us_);
        consider(progress_deadline_us_);
    }
    if (!retired_.empty()) consider(clock_us() + 20000);
    if (earliest < 0) return -1;
    int64_t wait = (earliest - clock_us() + 999) / 1000;
    return static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(wait, 3600000)));
}

// ---------------------------------------------------------------- following the player

void AnalysisWindowModel::update_playback_state()
{
    if (!song_) return;
    engine::PlaybackSnapshot snapshot = controller_.engine().snapshot();
    graph_.set_playback_positions(snapshot.intended_timeline_microseconds, snapshot.last_dispatched_timeline_microseconds);
    PlaybackOverlay overlay;
    overlay.active = snapshot.state == engine::PlaybackState::Playing || snapshot.state == engine::PlaybackState::Paused;
    overlay.state = engine::playback_state_name(snapshot.state);
    overlay.timeline_and_output = format::format_timeline_output(snapshot.intended_timeline_microseconds,
        snapshot.last_dispatched_timeline_microseconds, false);
    overlay.queue = format::format_analysis_queue(snapshot, controller_.culture());
    overlay.events = format::format_event_counts(snapshot, false, controller_.culture());
    overlay.events_caption = "Events sent / dropped: ";
    overlay.output_rate = controller_.output_rate_text();
    overlay.effective_speed = controller_.effective_speed_text();
    overlay.lag = format::format_analysis_lag(snapshot, controller_.culture());
    graph_.set_playback_overlay(overlay);
    handle_graph_events();
}

void AnalysisWindowModel::check_override_signature()
{
    if (!project_forced_overrides_ || !song_ || settings_deadline_us_ >= 0) return;
    int64_t now = clock_us();
    if (override_check_us_ >= 0 && now < override_check_us_) return;
    override_check_us_ = now + 250000;
    std::shared_ptr<const overrides::AnalysisOverrideSnapshot> snapshot = controller_.capture_override_snapshot();
    std::string signature = snapshot ? snapshot->signature() : std::string();
    if (signature != override_signature_)
        settings_deadline_us_ = now + static_cast<int64_t>(kAnalysisSettingsDebounceMs) * 1000;
}

void AnalysisWindowModel::on_refresh()
{
    if (closed_ || !started_) return;
    bool loading = controller_.loading();
    uint64_t songRevision = controller_.song_revision();
    std::shared_ptr<engine::PlaybackSong> current = controller_.effective_song();
    if (loading) {
        if (!detached_for_loading_) {
            detached_for_loading_ = true;
            detach(kAnalysisLoadingMessage);
        }
        seen_song_revision_ = songRevision;
        return;
    }
    if (songRevision != seen_song_revision_ || detached_for_loading_ || (current != song_)) {
        seen_song_revision_ = songRevision;
        seen_analysis_revision_ = controller_.analysis_revision();
        if (current) {
            detached_for_loading_ = false;
            if (current != song_) attach(current);
        } else if (detached_for_loading_) {
            detached_for_loading_ = false;
            detach("MIDI loading was cancelled or failed. No MIDI file is loaded.");
        } else {
            detach(kAnalysisNoFileMessage);
        }
    } else if (song_ && controller_.analysis_revision() != seen_analysis_revision_) {
        seen_analysis_revision_ = controller_.analysis_revision();
        settings_deadline_us_ = clock_us() + static_cast<int64_t>(kAnalysisSettingsDebounceMs) * 1000;
    }
    update_playback_state();
    check_override_signature();
}

} // namespace app
} // namespace midibn
