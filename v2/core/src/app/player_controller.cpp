// Port of the non-presentation logic of v1 MainForm.cs (see player_controller.h).
// Section comments name the MainForm member each function corresponds to.
#include "midibn/app/player_controller.h"

#include "midibn/app/ui_mappings.h"
#include "midibn/format/display_format.h"
#include "midibn/null_output.h"

#include <algorithm>
#include <map>
#include <utility>

namespace midibn {
namespace app {

// R1: what the Direct WinMM check has answered, per WinMM device id, for this session. Shared with the worker threads
// that run the check (they may outlive the controller, so they hold their own reference).
struct PlayerController::DirectProbeState {
    std::mutex mutex;
    struct Entry { bool done = false; bool usable = true; std::string reason; };
    std::map<uint32_t, Entry> entries;
    std::atomic<int> changed{ 0 };
};

using engine::OverflowPolicy;
using engine::PlaybackSnapshot;
using engine::PlaybackState;
using engine::ProcessingMode;
using models::ServiceDurationMode;

namespace {

const char kWorkerTimeoutType[] = "PlaybackWorkerTimeoutException";

bool is_timeout(const engine::EngineError& error) { return error.type == kWorkerTimeoutType; }

std::string file_name_of(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

const char kThinSlash[] = "\xE2\x80\x89/\xE2\x80\x89";   // U+2009 / U+2009

} // namespace

// ------------------------------------------------------------------ load job

struct PlayerController::LoadJob : public LoadMonitor {
    int generation = 0;
    std::string path;
    std::mutex mutex;
    std::condition_variable condition;
    LoadProgress latest;
    bool have_progress = false;
    std::atomic<bool> cancelled{ false };
    // large-file decision handshake
    bool decision_pending = false;
    bool decision_answered = false;
    bool decision_accepted = false;
    PreflightCounts counts;
    MemoryProjection projection;
    // result
    std::atomic<bool> done{ false };
    LoadFlowResult result = LoadFlowResult::Failed;
    LoadError error;
    std::shared_ptr<Song> song;
    std::unique_ptr<platform::Thread> thread;

    void progress(const LoadProgress& value) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        latest = value;
        have_progress = true;
    }
    bool cancellation_requested() override { return cancelled.load(); }

    bool ask(const PreflightCounts& c, const MemoryProjection& p, const std::function<void()>& wake)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (cancelled.load()) return false;
        counts = c;
        projection = p;
        decision_pending = true;
        decision_answered = false;
        lock.unlock();
        if (wake) wake();
        lock.lock();
        while (!decision_answered && !cancelled.load()) condition.wait(lock);
        decision_pending = false;
        return decision_answered && decision_accepted && !cancelled.load();
    }
};

// ------------------------------------------------------------------ construction

PlayerController::PlayerController(const PlayerEnvironment& environment)
    : env_(environment), engine_(environment.services)
{
    engine_.set_simulate_slowdown(false);
    engine_.set_apply_queue_limit_without_slowdown(false);
    engine_.set_chase_midi_state_on_play_seek(true);
    engine_.set_preserve_playback_queue_on_pause_seek(false);
    engine_.set_output_paced_timeline(false);
    engine_.set_scale_note_interval_by_traffic(false);
    engine_.set_interval_polyphony_limit_enabled(false);
    engine_.set_interval_polyphony_limit(engine::EngineCore::kDefaultIntervalPolyphonyLimit);

    if (env_.has_initial_settings && !env_.initial_settings.output_name.empty()) {
        pending_output_name_ = env_.initial_settings.output_name;
        remembered_output_name_ = pending_output_name_;
    }
    null_ = std::make_shared<platform::NullMidiOutput>();
    if (env_.create_winmm) winmm_ = env_.create_winmm();
    if (env_.create_kdmapi) kdmapi_ = env_.create_kdmapi();
    if (env_.create_winmm_direct) direct_ = env_.create_winmm_direct();
    probe_ = std::make_shared<DirectProbeState>();
    output_items_.push_back(platform::NullMidiOutput::display_name());
    devices_ready_ = false;
    if (!env_.defer_device_listing) ensure_devices();

    // BuildProcessingGroup initial control values.
    queue_check_ = false;
    queue_value_.value = engine::EngineCore::kDefaultQueueLengthLimit;
    queue_value_.minimum = 1;
    queue_value_.maximum = 1000000;
    queue_value_.increment = 100;
    queue_check_tooltip_ = "Queue structure is locked while playback is active.";
    queue_value_tooltip_ =
        "Click to type; Enter applies, Escape cancels. Drag sideways: fine at small limits, gradually faster at larger limits. Shift adjusts one event per 2 pixels. Up/Down adjusts by 100. During ordinary finite-queue playback, a live value applies to the next admission decision without retracting accepted work.";
    queue_value_name_ = "Queue length limit";
    events_label_ = "events";
    populate_overflow_policies(false, OverflowPolicy::DropNewest);
    overflow_tooltip_ = "A change made during playback applies at the next overflow.";
    processing_value_.value = 0;
    processing_value_.minimum = 0;
    processing_value_.maximum = 1000000;
    processing_value_.increment = 1;
    processing_name_ = "Processing time per event";

    apply_processing_microseconds(100, false);   // SetProcessingMicroseconds(100)
    configure_service_controls();

    engine_.set_playback_ended_handler([this]() {
        ended_flag_.store(1);
        if (env_.wake_ui) env_.wake_ui();
    });
    engine_.set_playback_failed_handler([this](const engine::EngineError& error) {
        {
            std::lock_guard<std::mutex> lock(event_mutex_);
            failures_.push_back(error);
        }
        if (env_.wake_ui) env_.wake_ui();
    });
    engine_.set_channel_control_failure_detail_handler([this](int channel, ChannelAttribute attribute, bool isEnableRequest,
        const engine::EngineError& error) {
        {
            std::lock_guard<std::mutex> lock(channel_failure_mutex_);
            ChannelControlFailure failure;
            failure.channel = channel;
            failure.attribute = attribute;
            failure.is_mute = isEnableRequest;
            failure.message = error.message.empty() ? error.type : error.message;
            if (channel_failures_.size() >= 64) channel_failures_.erase(channel_failures_.begin());
            channel_failures_.push_back(failure);
        }
        if (env_.wake_ui) env_.wake_ui();
    });
    if (env_.has_initial_settings) {
        // Remembered settings (Step 4). Nothing is playing, so every toggle is allowed. The queue-by-age option is set
        // first because "apply without slowdown" switches it off again.
        const RememberedSettings& s = env_.initial_settings;
        kdmapi_checked_ = s.kdmapi;
        if (s.direct_winmm) direct_checked_ = true;
        if (chase_midi_state_ != s.chase_midi_state) toggle_state_chase();
        if (preserve_queue_ != s.preserve_queue) toggle_preserve_pause_seek();
        if (output_paced_ != s.output_paced) toggle_output_paced_timeline();
        if (scale_note_interval_ != s.scale_note_interval) toggle_scale_note_interval_by_traffic();
        if (queue_age_limit_enabled_ != s.queue_age_limit) toggle_queue_age_limit();
        if (apply_queue_limit_without_slowdown_ != s.apply_queue_limit && !s.queue_age_limit) toggle_apply_queue_limit_without_slowdown();
        effective_speed_.set_window(s.speed_window_us);
    }
    refresh();
    constructed_ = true;
}

RememberedSettings PlayerController::remembered_settings() const
{
    RememberedSettings s;
    s.output_name = remembered_output_name_;
    s.kdmapi = kdmapi_checked_;
    s.direct_winmm = direct_checked_;
    s.chase_midi_state = chase_midi_state_;
    s.preserve_queue = preserve_queue_;
    s.output_paced = output_paced_;
    s.apply_queue_limit = apply_queue_limit_without_slowdown_;
    s.queue_age_limit = queue_age_limit_enabled_;
    s.scale_note_interval = scale_note_interval_;
    s.speed_window_us = effective_speed_.window_microseconds();
    return s;
}

void PlayerController::set_settings_note(const std::string& text, const std::string& detail)
{
    settings_note_ = text;
    settings_note_detail_ = detail;
}

// Lists the output devices once (DD-057: the constructor, or the first moment the list is needed).
void PlayerController::ensure_devices() const
{
    if (devices_ready_) return;
    devices_ready_ = true;
    if (env_.list_devices) devices_ = env_.list_devices();
    output_items_.clear();
    output_items_.push_back(platform::NullMidiOutput::display_name());
    for (size_t i = 0; i < devices_.size(); ++i) output_items_.push_back(devices_[i].name);
    // v1 DefaultOutputSelectionIndex: the first real Windows device if any, else None.
    output_index_ = devices_.empty() ? 0 : 1;
    if (!pending_output_name_.empty()) {   // the remembered output, only if that device is still there
        for (size_t i = 0; i < output_items_.size(); ++i)
            if (output_items_[i] == pending_output_name_) { output_index_ = static_cast<int>(i); break; }
        pending_output_name_.clear();
    }
    start_direct_probe();   // R1: the initial selection
}

// R1: decides whether Direct mode can be used for the selected device, when the selection changes. The MIDI Mapper needs
// no driver call. Anything else is asked of the environment on a worker thread (a driver can block); until the answer
// arrives the box stays enabled. One answer per device for the session.
void PlayerController::start_direct_probe() const
{
    if (!direct_ || !probe_ || output_index_ <= 0 || static_cast<size_t>(output_index_ - 1) >= devices_.size()) return;
    const uint32_t id = devices_[static_cast<size_t>(output_index_ - 1)].device_id;
    if (id == 0xFFFFFFFFu) return;   // the Mapper is decided in direct_known_unusable
    if (!env_.probe_direct || !env_.services) return;
    {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        if (probe_->entries.find(id) != probe_->entries.end()) return;
        probe_->entries[id] = DirectProbeState::Entry();
    }
    std::shared_ptr<DirectProbeState> state = probe_;
    std::function<DirectProbeResult(uint32_t)> probe = env_.probe_direct;
    std::function<void()> wake = env_.wake_ui;
    platform::PlatformError threadError;
    std::unique_ptr<platform::Thread> thread = env_.services->start_thread([state, probe, wake, id]() {
        DirectProbeResult result = probe(id);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            DirectProbeState::Entry& entry = state->entries[id];
            entry.done = true;
            entry.usable = result.usable;
            entry.reason = result.reason;
        }
        state->changed.store(1);
        if (wake) wake();
    }, platform::ThreadPriority::Normal, "Direct WinMM check", threadError);
    if (thread) thread->detach();
    else {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        probe_->entries.erase(id);   // no answer: the first Play decides, as before
    }
}

// Lists the output devices again while stopped (a USB device was plugged in or removed). The loaded file and every option stay.
// The selection stays on the same device (found by name); when that device is gone the output becomes None and a warning says so.
// None stays None. Not while a song is playing or paused: the open output belongs to the old list.
void PlayerController::refresh_devices()
{
    if (!devices_ready_) {
        ensure_devices();   // the first listing is the fresh one; there is no earlier selection to keep
        refresh();
        return;
    }
    const PlaybackState state = engine_.state();
    if (state == PlaybackState::Playing || state == PlaybackState::Paused || engine_.has_live_worker()) {
        message(UiMessage::Information, "Stop playback first", "The list of MIDI outputs can be read again only while playback is stopped.");
        return;
    }
    const bool hadDevice = output_index_ > 0 && static_cast<size_t>(output_index_) < output_items_.size();
    const std::string previousName = hadDevice ? output_items_[static_cast<size_t>(output_index_)] : std::string();
    const int previousIndex = output_index_;
    std::vector<platform::OutputDeviceInfo> listed;
    if (env_.list_devices) listed = env_.list_devices();
    devices_ = listed;
    output_items_.clear();
    output_items_.push_back(platform::NullMidiOutput::display_name());
    for (size_t i = 0; i < devices_.size(); ++i) output_items_.push_back(devices_[i].name);
    bool vanished = false;
    if (hadDevice) {
        int found = 0;
        for (size_t i = 0; i < devices_.size() && found == 0; ++i)
            if (devices_[i].name == previousName) found = static_cast<int>(i) + 1;
        output_index_ = found;
        vanished = found == 0;
    }
    else if (output_index_ >= static_cast<int>(output_items_.size())) output_index_ = 0;
    // The device numbers may have changed: forget the old Direct mode answers and ask again for the selection.
    direct_outcome_ = DirectOutcome::NotTried;
    direct_outcome_device_ = -1;
    if (probe_) {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        probe_->entries.clear();
    }
    start_direct_probe();
    if (vanished) {
        message(UiMessage::Warning, "MIDI output removed",
            "The MIDI output \"" + previousName + "\" is no longer available. The output is now None, which plays no sound. Choose another output in the list.");
    }
    if (output_index_ != previousIndex) output_setting_changed();   // stopped: closes the output that was kept open
    refresh();
}

void PlayerController::forget_direct_checks()
{
    if (!probe_) return;
    {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        probe_->entries.clear();
    }
    start_direct_probe();
}

bool PlayerController::direct_check_pending() const
{
    if (!probe_ || output_index_ <= 0 || static_cast<size_t>(output_index_ - 1) >= devices_.size()) return false;
    std::lock_guard<std::mutex> lock(probe_->mutex);
    std::map<uint32_t, DirectProbeState::Entry>::const_iterator it = probe_->entries.find(devices_[static_cast<size_t>(output_index_ - 1)].device_id);
    return it != probe_->entries.end() && !it->second.done;
}

bool PlayerController::direct_known_unusable(std::string& reason) const
{
    if (output_index_ <= 0 || static_cast<size_t>(output_index_ - 1) >= devices_.size() || !probe_) return false;
    const uint32_t id = devices_[static_cast<size_t>(output_index_ - 1)].device_id;
    if (id == 0xFFFFFFFFu) {
        reason = "This is the MIDI Mapper, which Windows routes to another device";
        return true;
    }
    std::lock_guard<std::mutex> lock(probe_->mutex);
    std::map<uint32_t, DirectProbeState::Entry>::const_iterator it = probe_->entries.find(id);
    if (it == probe_->entries.end() || !it->second.done || it->second.usable) return false;
    reason = it->second.reason;
    return true;
}

std::vector<ChannelControlFailure> PlayerController::take_channel_control_failures()
{
    std::vector<ChannelControlFailure> out;
    std::lock_guard<std::mutex> lock(channel_failure_mutex_);
    out.swap(channel_failures_);
    return out;
}

std::string PlayerController::no_song_message() const
{
    if (loading_) return "Loading new MIDI\xE2\x80\xA6";
    if (no_song_reason_ == 1) return "MIDI loading cancelled. No MIDI file is loaded.";
    if (no_song_reason_ == 2) return "MIDI loading failed. No MIDI file is loaded.";
    return "No MIDI file is loaded.";
}

PlayerController::~PlayerController()
{
    cancel_load();
    engine_.set_playback_ended_handler(std::function<void()>());
    engine_.set_playback_failed_handler(std::function<void(const engine::EngineError&)>());
    engine_.dispose();
}

// ------------------------------------------------------------------ messages

namespace {

// The plain sentence for a failure whose lower-level text is technical: it names what the person was doing.
std::string plain_sentence_for(const std::string& title)
{
    if (title == "Unable to start playback") return "Playback could not be started.";
    if (title == "Unable to change MIDI output") return "The MIDI output could not be changed.";
    if (title == "Unable to pause playback") return "Playback could not be paused.";
    if (title == "Unable to pause MIDI output safely") return "The MIDI output could not be paused safely.";
    if (title == "Unable to stop playback") return "Playback could not be stopped.";
    if (title == "Unable to stop MIDI output safely") return "The MIDI output could not be stopped safely.";
    if (title == "Unable to seek") return "Playback could not move to that position.";
    if (title == "Unable to seek safely") return "The MIDI output could not move to that position safely.";
    if (title == "Playback error") return "Playback stopped because of an error.";
    if (title == "Unable to load MIDI file") return "The MIDI file could not be loaded.";
    return "The action could not be completed.";
}

} // namespace

void PlayerController::message(UiMessage::Kind kind, const std::string& title, const std::string& text)
{
    message_explained(kind, title, explain_error(text, plain_sentence_for(title)));
}

void PlayerController::message_explained(UiMessage::Kind kind, const std::string& title, const ErrorText& text)
{
    UiMessage m;
    m.kind = kind;
    m.title = title;
    m.text = join_error_text(text);
    messages_.push_back(m);
}

std::vector<UiMessage> PlayerController::take_messages()
{
    std::vector<UiMessage> out;
    out.swap(messages_);
    return out;
}

bool PlayerController::engine_failed(const engine::EngineError& error, const char* title)
{
    if (error.ok()) return false;
    output_error_ = error.message;
    message_explained(UiMessage::Error, title, explain_engine_error(error, plain_sentence_for(title)));
    return true;
}

int64_t PlayerController::clock_microseconds()
{
    platform::Clock& clock = env_.services->clock();
    int64_t ticks = clock.now_ticks();
    int64_t frequency = clock.frequency();
    if (frequency <= 0) return 0;
    return (ticks / frequency) * 1000000 + (ticks % frequency) * 1000000 / frequency;
}

void PlayerController::schedule_analysis_refresh() { ++analysis_revision_; }

// ------------------------------------------------------------------ small queries

bool PlayerController::engine_active()
{
    PlaybackState state = engine_.state();
    return state == PlaybackState::Playing || state == PlaybackState::Paused;
}

OverflowPolicy PlayerController::selected_overflow_policy() const
{
    if (overflow_selected_ < 0 || overflow_selected_ >= static_cast<int>(overflow_items_.size()))
        return OverflowPolicy::DropNewest;
    return overflow_items_[static_cast<size_t>(overflow_selected_)].policy;
}

ProcessingMode PlayerController::selected_processing_mode() const
{
    if (per_note_gate_) return ProcessingMode::PerNoteIntervalGate;
    if (delayed_note_off_) return ProcessingMode::DelayedNoteOff;
    if (note_attack_limit_) return ProcessingMode::NoteAttackLimit;
    if (output_rate_limit_) return ProcessingMode::OutputRateLimit;
    return queue_check_ ? ProcessingMode::Drop : ProcessingMode::Queue;
}

bool PlayerController::is_unsupported_forward_queue_selection()
{
    if (per_note_gate_ || delayed_note_off_ || note_attack_limit_ || output_rate_limit_ || !queue_check_ ||
        engine_.simulate_slowdown() || !apply_queue_limit_without_slowdown_)
        return false;
    OverflowPolicy policy = selected_overflow_policy();
    return policy == OverflowPolicy::DropOldest || policy == OverflowPolicy::DropOldestCompleteNote ||
        policy == OverflowPolicy::DropBackgroundNotes || policy == OverflowPolicy::ClearBufferAndCatchUp;
}

void PlayerController::set_compact(bool compact) { compact_ = compact; }

// ------------------------------------------------------------------ overflow choices

void PlayerController::populate_overflow_policies(bool ageMode, OverflowPolicy requested)
{
    overflow_items_.clear();
    if (!ageMode) {
        // DD-025: the complete-note choices come first; "Drop newest events" (the default policy) is no longer first.
        overflow_items_.push_back({ OverflowPolicy::DropIncomingCompleteNotes, "Drop incoming complete notes" });
        overflow_items_.push_back({ OverflowPolicy::DropOldestCompleteNote, "Drop oldest complete note" });
        overflow_items_.push_back({ OverflowPolicy::DropNewest, "Drop newest events" });
        overflow_items_.push_back({ OverflowPolicy::DropOldest, "Drop oldest events" });
        overflow_items_.push_back({ OverflowPolicy::ClearBufferAndCatchUp, "Clear buffer and jump to realtime" });
        overflow_items_.push_back({ OverflowPolicy::DropBackgroundNotes, "Drop background notes" });
    }
    else {
        // DD-086 (user decision, prompt 009): with a waiting-time limit "Clear buffer and jump to realtime" (the default
        // of this list) comes first; the other two keep their order below it. The selection is kept by policy, not index.
        overflow_items_.push_back({ OverflowPolicy::ClearBufferAndCatchUp, "Clear buffer and jump to realtime" });
        overflow_items_.push_back({ OverflowPolicy::DropOldestCompleteNote, "Drop oldest complete note (soft limit)" });
        overflow_items_.push_back({ OverflowPolicy::DropOldest, "Drop oldest events" });
        if (requested == OverflowPolicy::DropNewest || requested == OverflowPolicy::DropIncomingCompleteNotes)
            requested = OverflowPolicy::ClearBufferAndCatchUp;
    }
    int selected = 0;
    for (size_t i = 0; i < overflow_items_.size(); ++i)
        if (overflow_items_[i].policy == requested) { selected = static_cast<int>(i); break; }
    overflow_selected_ = selected;
    // v1 calls OverflowPolicyChanged here once the window is built (the playback buttons exist).
    overflow_policy_changed();
}

void PlayerController::overflow_policy_changed()
{
    OverflowPolicy policy = selected_overflow_policy();
    if (queue_age_limit_enabled_) remembered_age_overflow_policy_ = policy;
    else remembered_event_overflow_policy_ = policy;
    engine_.set_overflow_policy(policy);
    overflow_tooltip_ = policy == OverflowPolicy::DropIncomingCompleteNotes
        ? "When full, reject an incoming Note On and later suppress its paired Note Off. Required Note Off and non-note messages are retained, so the configured capacity is a soft safety limit. A live change applies at the next overflow."
        : policy == OverflowPolicy::DropOldestCompleteNote
            ? "When full, remove the oldest queued Note On that has not begun service, including its queued release. A later matching release is suppressed. Required releases may exceed the limit; if no old note is eligible, other incoming MIDI is rejected. Requires simulated slowdown."
            : policy == OverflowPolicy::DropBackgroundNotes
                ? "When full, drop one complete unsent background note. Inner chord voices are considered before bass and melody boundaries, then velocity, duration, and fair track or percussion-lane representation decide. Releases for notes already sent, state, safety traffic, and Seek markers remain protected, so the limit can be soft. Requires simulated slowdown."
                : policy == OverflowPolicy::DropOldest || policy == OverflowPolicy::ClearBufferAndCatchUp
                    ? "This policy requires simulated slowdown. A forward-only queue cannot retract MIDI that has already been sent."
                    : "A change made during playback applies at the next overflow.";
    schedule_analysis_refresh();
}

void PlayerController::set_overflow_selection(int index)
{
    if (index < 0 || index >= static_cast<int>(overflow_items_.size())) return;
    overflow_selected_ = index;
    overflow_policy_changed();
}

// ------------------------------------------------------------------ queue limit controls

void PlayerController::configure_queue_limit_controls()
{
    bool intervalGate = per_note_gate_ || delayed_note_off_;
    if (intervalGate) {
        queue_check_ = interval_polyphony_limit_enabled_;
        queue_value_.set_range(1, engine::EngineCore::kDefaultIntervalPolyphonyLimit);
        queue_value_.increment = 1;
        queue_value_.set_value(std::max<int64_t>(1, std::min<int64_t>(engine::EngineCore::kDefaultIntervalPolyphonyLimit,
            remembered_interval_polyphony_limit_)));
        queue_value_name_ = "Maximum note messages in one logical interval";
        events_label_ = compact_ ? "notes" : "note messages/interval";
        queue_check_tooltip_ =
            "Limits transmitted Note On and Note Off messages in each logical interval. This is not the synthesizer's total sounding-voice limit. The setting is structural and is locked while playback is active.";
        queue_value_tooltip_ =
            "Maximum transmitted note messages in one interval, from 1 to 128. Required releases take priority; only excess required releases can continue into the next interval.";
        overflow_tooltip_ =
            "Interval polyphony uses one fixed note-safe policy, so the ordinary queue Overflow choices do not apply.";
        engine_.set_interval_polyphony_limit_enabled(interval_polyphony_limit_enabled_);
        engine_.set_interval_polyphony_limit(remembered_interval_polyphony_limit_);
    }
    else if (queue_age_limit_enabled_) {
        queue_check_tooltip_ = "Queue structure is locked while playback is active.";
        queue_value_.set_range(1, 1800000000);
        queue_value_.increment = 1000;
        queue_value_.set_value(std::max<int64_t>(1, std::min<int64_t>(1800000000LL, remembered_queue_age_limit_)));
        queue_value_name_ = "Queue waiting-time limit in microseconds";
        events_label_ = "\xC2\xB5s";
        queue_value_tooltip_ =
            "Oldest permitted waiting time, 1 \xC2\xB5s through 30 minutes. Exactly the limit is allowed; overflow starts above it. Click to type or drag; Shift adjusts one microsecond per 2 pixels.";
        engine_.set_queue_age_limit_microseconds(remembered_queue_age_limit_);
    }
    else {
        queue_check_tooltip_ = "Queue structure is locked while playback is active.";
        queue_check_ = remembered_ordinary_queue_limit_enabled_;
        queue_value_.set_range(1, 1000000);
        queue_value_.increment = 100;
        queue_value_.set_value(remembered_event_count_limit_);
        queue_value_name_ = "Queue length limit";
        events_label_ = "events";
        queue_value_tooltip_ =
            "Click to type; Enter applies, Escape cancels. Drag sideways: fine at small limits, gradually faster at larger limits. Shift adjusts one event per 2 pixels. Up/Down adjusts by 100. During ordinary finite-queue playback, a live value applies to the next admission decision without retracting accepted work.";
        engine_.set_queue_length_limit(remembered_event_count_limit_);
    }
}

void PlayerController::set_queue_age_limit_enabled(bool enabled)
{
    if (per_note_gate_ || delayed_note_off_) return;
    if (queue_age_limit_enabled_ == enabled) return;
    if (enabled) {
        remembered_event_count_limit_ = static_cast<int>(queue_value_.value);
        remembered_event_overflow_policy_ = selected_overflow_policy();
        if (apply_queue_limit_without_slowdown_) {
            apply_queue_limit_without_slowdown_ = false;
            engine_.set_apply_queue_limit_without_slowdown(false);
        }
    }
    else {
        remembered_queue_age_limit_ = queue_value_.value;
        remembered_age_overflow_policy_ = selected_overflow_policy();
    }
    queue_age_limit_enabled_ = enabled;
    engine_.set_queue_age_limit_enabled(enabled);
    configure_queue_limit_controls();
    populate_overflow_policies(enabled, enabled ? remembered_age_overflow_policy_ : remembered_event_overflow_policy_);
}

void PlayerController::set_queue_limit_checked(bool checked)
{
    queue_check_ = checked;
    // QueueLimitChanged
    if (per_note_gate_ || delayed_note_off_) {
        interval_polyphony_limit_enabled_ = queue_check_;
        engine_.set_interval_polyphony_limit_enabled(interval_polyphony_limit_enabled_);
        refresh();
        schedule_analysis_refresh();
        return;
    }
    remembered_ordinary_queue_limit_enabled_ = queue_check_;
    if (queue_check_ && apply_queue_limit_without_slowdown_ && selected_rate_choice_ == RateModelChoice::None)
        apply_rate_choice(choice_for_ordinary_mode(last_ordinary_service_mode_));
    else
        synchronize_rate_processing();
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::set_queue_value(int64_t value)
{
    queue_value_.set_value(value);
    // The ValueChanged handler built in BuildProcessingGroup.
    if (per_note_gate_ || delayed_note_off_) {
        remembered_interval_polyphony_limit_ = static_cast<int>(queue_value_.value);
        engine_.set_interval_polyphony_limit(remembered_interval_polyphony_limit_);
    }
    else if (queue_age_limit_enabled_) {
        remembered_queue_age_limit_ = queue_value_.value;
        engine_.set_queue_age_limit_microseconds(remembered_queue_age_limit_);
    }
    else {
        remembered_event_count_limit_ = static_cast<int>(queue_value_.value);
        engine_.set_queue_length_limit(remembered_event_count_limit_);
    }
    schedule_analysis_refresh();
}

// ------------------------------------------------------------------ system-menu toggles

void PlayerController::toggle_apply_queue_limit_without_slowdown()
{
    if (engine_active()) return;
    apply_queue_limit_without_slowdown_ = !apply_queue_limit_without_slowdown_;
    if (apply_queue_limit_without_slowdown_ && queue_age_limit_enabled_) set_queue_age_limit_enabled(false);
    engine_.set_apply_queue_limit_without_slowdown(apply_queue_limit_without_slowdown_);
    if (apply_queue_limit_without_slowdown_ && queue_check_ && selected_rate_choice_ == RateModelChoice::None)
        apply_rate_choice(choice_for_ordinary_mode(last_ordinary_service_mode_));
    else
        synchronize_rate_processing();
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::toggle_queue_age_limit()
{
    if (engine_active()) return;
    set_queue_age_limit_enabled(!queue_age_limit_enabled_);
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::toggle_state_chase()
{
    if (engine_active()) return;
    chase_midi_state_ = !chase_midi_state_;
    engine_.set_chase_midi_state_on_play_seek(chase_midi_state_);
    refresh();
}

void PlayerController::toggle_preserve_pause_seek()
{
    if (engine_active()) return;
    preserve_queue_ = !preserve_queue_;
    engine_.set_preserve_playback_queue_on_pause_seek(preserve_queue_);
}

void PlayerController::toggle_output_paced_timeline()
{
    if (engine_active()) return;
    output_paced_ = !output_paced_;
    engine_.set_output_paced_timeline(output_paced_);
    schedule_analysis_refresh();
}

void PlayerController::toggle_scale_note_interval_by_traffic()
{
    if (engine_active()) return;
    scale_note_interval_ = !scale_note_interval_;
    engine_.set_scale_note_interval_by_traffic(scale_note_interval_);
    schedule_analysis_refresh();
}

// ------------------------------------------------------------------ restart on structural change

bool PlayerController::restart_for_processing_mode_change(PlaybackState previousState,
    const std::function<void()>& configureAfterStop)
{
    std::shared_ptr<engine::PlaybackSong> playbackSong = playback_song_;
    if (!playbackSong || !active_output_) {
        if (configureAfterStop) configureAfterStop();
        return true;
    }
    PlaybackSnapshot before = engine_.snapshot();
    int64_t restartPosition = before.intended_timeline_microseconds;
    engine::EngineError error = engine_.stop();
    if (!error.ok()) throw Failure{ "Unable to change processing mode", error.message, is_timeout(error) };
    selected_position_us_ = restartPosition;
    if (configureAfterStop) configureAfterStop();
    playbackSong = playback_song_;
    // A restart continues where playback just was: the events at exactly that time were already sent (DD-007).
    error = engine_.start(playbackSong, active_output_, selected_processing_mode(), restartPosition,
        previousState == PlaybackState::Paused, true);
    if (!error.ok()) throw Failure{ "Unable to change processing mode", error.message, is_timeout(error) };
    engine_song_ = playbackSong;
    reset_live_measurements();
    return true;
}

bool PlayerController::try_restart_for_processing_mode_change(PlaybackState previousState,
    const std::function<void()>& configureAfterStop)
{
    try {
        return restart_for_processing_mode_change(previousState, configureAfterStop);
    }
    catch (const Failure& failure) {
        output_error_ = failure.message;
        refresh();
        message(UiMessage::Error, failure.title, failure.message);
        return false;
    }
}

// ------------------------------------------------------------------ Rate model

void PlayerController::select_rate_choice(RateModelChoice choice) { apply_rate_choice(choice); }

void PlayerController::apply_rate_choice(RateModelChoice choice)
{
    PlaybackState previousState = engine_.state();
    RateModelChoice previousChoice = selected_rate_choice_;
    bool previousIntervalGate = is_interval_gate_choice(previousChoice);
    bool nextIntervalGate = is_interval_gate_choice(choice);
    if (previousIntervalGate) {
        interval_polyphony_limit_enabled_ = queue_check_;
        remembered_interval_polyphony_limit_ = static_cast<int>(queue_value_.value);
    }
    else remembered_ordinary_queue_limit_enabled_ = queue_check_;
    bool musicalBoundary = previousChoice != choice &&
        (is_musical_rate_choice(previousChoice) || is_musical_rate_choice(choice));
    bool structuralBoundary = musicalBoundary ||
        ((previousChoice == RateModelChoice::None) != (choice == RateModelChoice::None));
    ServiceDurationMode mode = (choice == RateModelChoice::NoteAttackLimit || choice == RateModelChoice::OutputRateLimit)
        ? ServiceDurationMode::EventsPerSecond
        : is_musical_rate_choice(choice) ? ServiceDurationMode::ProcessingTime
        : is_ordinary_rate_choice(choice) ? ordinary_mode_for_choice(choice) : last_ordinary_service_mode_;
    if (previousChoice == RateModelChoice::ProcessingTime) remembered_processing_us_ = engine_.processing_microseconds();
    else if (previousChoice == RateModelChoice::PerNoteIntervalGate) remembered_gate_us_ = engine_.processing_microseconds();
    else if (previousChoice == RateModelChoice::DelayedNoteOff) remembered_delayed_note_off_us_ = engine_.processing_microseconds();
    else if (previousChoice == RateModelChoice::EventsPerSecond) remembered_ordinary_event_rate_ = engine_.events_per_second();
    else if (previousChoice == RateModelChoice::NoteAttackLimit) remembered_attack_rate_ = engine_.events_per_second();
    else if (previousChoice == RateModelChoice::OutputRateLimit) remembered_output_rate_ = engine_.events_per_second();
    int64_t processing = choice == RateModelChoice::PerNoteIntervalGate ? remembered_gate_us_
        : choice == RateModelChoice::DelayedNoteOff ? remembered_delayed_note_off_us_
        : choice == RateModelChoice::ProcessingTime ? remembered_processing_us_ : engine_.processing_microseconds();
    int64_t eventRate = choice == RateModelChoice::NoteAttackLimit ? remembered_attack_rate_
        : choice == RateModelChoice::OutputRateLimit ? remembered_output_rate_
        : choice == RateModelChoice::EventsPerSecond ? remembered_ordinary_event_rate_ : engine_.events_per_second();
    bool applied = false;
    std::function<void()> applyConfiguration = [&]() {
        if (is_ordinary_rate_choice(choice)) last_ordinary_service_mode_ = mode;
        selected_rate_choice_ = choice;
        per_note_gate_ = choice == RateModelChoice::PerNoteIntervalGate;
        delayed_note_off_ = choice == RateModelChoice::DelayedNoteOff;
        note_attack_limit_ = choice == RateModelChoice::NoteAttackLimit;
        output_rate_limit_ = choice == RateModelChoice::OutputRateLimit;
        queue_check_ = nextIntervalGate ? interval_polyphony_limit_enabled_ : remembered_ordinary_queue_limit_enabled_;
        configure_queue_limit_controls();
        if (!nextIntervalGate) overflow_policy_changed();
        if (choice == RateModelChoice::None && apply_queue_limit_without_slowdown_ && queue_check_) {
            apply_queue_limit_without_slowdown_ = false;
            engine_.set_apply_queue_limit_without_slowdown(false);
        }
        engine_.set_service_configuration(mode, processing, engine_.midi_bitrate(), eventRate);
        synchronize_rate_processing();
        applied = true;
        configure_service_controls();
    };
    if (structuralBoundary && (previousState == PlaybackState::Playing || previousState == PlaybackState::Paused)) {
        if (!try_restart_for_processing_mode_change(previousState, applyConfiguration) && !applied)
            return;   // the Rate model view keeps the previous choice
    }
    else applyConfiguration();
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::synchronize_rate_processing()
{
    bool forward = is_ordinary_rate_choice(selected_rate_choice_) && queue_check_ && apply_queue_limit_without_slowdown_;
    engine_.set_simulate_slowdown(is_ordinary_rate_choice(selected_rate_choice_) && !forward);
}

void PlayerController::configure_service_controls()
{
    ServiceDurationMode mode = engine_.service_duration_mode();
    if (mode == ServiceDurationMode::MidiBitrate) {
        processing_name_ = "MIDI bitrate in bits per second";
        processing_value_.set_range(1, 100000000);
        processing_value_.increment = 100;
        processing_value_.set_value(std::min<int64_t>(processing_value_.maximum, engine_.midi_bitrate()));
        slider_value_ = bitrate_to_slider(engine_.midi_bitrate());
    }
    else if (mode == ServiceDurationMode::EventsPerSecond) {
        processing_name_ = note_attack_limit_ ? "Maximum note starts per second"
            : output_rate_limit_ ? "Maximum output rate in events per second" : "Events per second";
        processing_value_.set_range(0, 9999999);
        processing_value_.increment = 1;
        processing_value_.set_value(engine_.events_per_second());
        slider_value_ = event_rate_to_slider(engine_.events_per_second());
    }
    else {
        processing_name_ = delayed_note_off_ ? "Minimum NoteOn duration in microseconds"
            : "Processing time per event in microseconds";
        processing_value_.set_range(0, processing_value_.maximum);
    }
    if (mode == ServiceDurationMode::MidiBitrate) update_bitrate_summary();
    else if (mode == ServiceDurationMode::EventsPerSecond) update_event_rate_summary(engine_.events_per_second());
    else apply_processing_microseconds(engine_.processing_microseconds(), false);   // SetProcessingMicroseconds
    if (selected_rate_choice_ == RateModelChoice::None) slider_value_ = 0;
}

void PlayerController::apply_processing_microseconds(int64_t microseconds, bool fromNumeric)
{
    if (microseconds < 0) microseconds = 0;
    if (microseconds > 1000000) microseconds = 1000000;
    int64_t previousMicroseconds = engine_.processing_microseconds();
    PlaybackState previousState = engine_.state();
    engine_.set_processing_microseconds(microseconds);
    if (per_note_gate_) remembered_gate_us_ = microseconds;
    else if (delayed_note_off_) remembered_delayed_note_off_us_ = microseconds;
    else if (selected_rate_choice_ == RateModelChoice::ProcessingTime) remembered_processing_us_ = microseconds;
    if (!fromNumeric || processing_value_.value != microseconds) {
        processing_value_.increment = 1;
        processing_value_.maximum = 1000000;
        processing_value_.set_value(microseconds);
    }
    slider_value_ = microseconds_to_slider(microseconds);
    update_processing_summary(microseconds);
    if ((per_note_gate_ || delayed_note_off_) && previousMicroseconds != microseconds &&
        (previousState == PlaybackState::Playing || previousState == PlaybackState::Paused))
        try_restart_for_processing_mode_change(previousState);
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::update_processing_summary(int64_t)
{
    processing_tooltip_ = per_note_gate_
        ? "Click to type or drag sideways following the interval slider's scale; Shift adjusts one microsecond per pixel. Enter applies; Escape cancels. This is one note-transition interval. With traffic scaling enabled, a batch of N transmitted note messages postpones the next note batch by N intervals. Changing it safely silences and restarts at the same position."
        : delayed_note_off_
            ? "Click to type or drag sideways to set the global per-pitch minimum NoteOn duration and one note-transition interval. A derived recovery of one quarter of this value, capped at 40 ms, follows each transmitted NoteOff. With traffic scaling enabled, a batch of N transmitted note messages postpones the next note batch by N intervals. Shift adjusts one microsecond per pixel. Enter applies; Escape cancels. A live edit safely silences and restarts at the same position."
            : "Click to type or drag sideways following the processing-time slider's scale; Shift adjusts one microsecond per pixel. Enter applies; Escape cancels. A live change applies when the next event begins service.";
    slider_tooltip_ = per_note_gate_
        ? "Sets one transition interval for the per-note gate. Zero resolves at source timestamps without a positive minimum interval. Traffic scaling permits a same-boundary chord burst, then waits in proportion to the transmitted note-message count; it is not a strict instantaneous events-per-second ceiling. A live edit safely silences and restarts the scheduler at the same position."
        : delayed_note_off_
            ? "Sets global ownership timing for each pitch across tracks and channels. Earlier repetitions coalesce; eligible replacements wait through the derived, capped post-Off recovery. A live edit safely silences and restarts the scheduler."
            : "Click or drag to set the rate. Fine adjustment to 5,000 \xC2\xB5s; logarithmic above. A live edit applies when the next event begins service.";
}

void PlayerController::update_bitrate_summary()
{
    processing_tooltip_ =
        "Click to type or drag sideways following the bitrate slider's scale; Shift adjusts one bit/s per pixel. Enter applies; Escape cancels. Up/Down adjusts by 100. A live change applies when the next event begins service.";
    slider_tooltip_ =
        "Click or drag to set the bitrate. Uses 10 transmitted bits per MIDI byte. A live edit applies when the next event begins service.";
}

void PlayerController::update_event_rate_summary(int64_t rate)
{
    if (note_attack_limit_) {
        processing_tooltip_ =
            "Progressively paces velocity-positive NoteOn starts at the selected starts-per-second target. Only capacity that has matured can be used; quiet passages save no burst credit. Releases and state/safety traffic remain outside this limit. A live edit safely restarts at the same position.";
        slider_tooltip_ =
            "Sets the progressively paced NoteOn-start target. Simultaneous attacks use the shared musical ranking; rejected attacks keep their paired releases suppressed.";
        return;
    }
    if (output_rate_limit_) {
        processing_tooltip_ =
            "Progressively paces transmitted MIDI messages at the selected Maximum output rate. Only matured capacity is available; quiet passages save no burst credit. Required state, safety, and emitted-attack releases may be sent over target. A live edit safely restarts at the same position.";
        slider_tooltip_ =
            "Sets the total Output rate gate. Rejected note occurrences do not wait for catch-up; live timing uses actual completed output calls.";
        return;
    }
    processing_tooltip_ =
        "Click to type an exact rate or drag sideways. Low rates change about one event/sec per five pixels; higher rates follow the slider's scale. Shift adjusts one event/sec per pixel. Enter applies; Escape cancels. A live change applies when the next event begins service.";
    slider_tooltip_ = rate == 0
        ? "Unlimited: modeled service is immediate. Move right to choose 1 through 9,999,999 events per second."
        : "The lower half adjusts 1\xE2\x80\x93" "100 events/sec in small, even steps; the upper half covers higher rates. Type an exact value if needed. Fractional microseconds are shared across events, so rates above one million remain modeled precisely. A live edit applies when the next event begins service.";
}

void PlayerController::apply_midi_bitrate(int64_t bitrate, bool fromNumeric)
{
    if (bitrate < 1) bitrate = 1;
    if (bitrate > 100000000) bitrate = 100000000;
    engine_.set_midi_bitrate(bitrate);
    if (!fromNumeric) processing_value_.set_value(bitrate);
    slider_value_ = bitrate_to_slider(bitrate);
    update_bitrate_summary();
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::apply_events_per_second(int64_t rate, bool fromNumeric)
{
    rate = std::max<int64_t>(0, std::min<int64_t>(9999999, rate));
    int64_t previous = engine_.events_per_second();
    PlaybackState previousState = engine_.state();
    engine_.set_events_per_second(rate);
    if (note_attack_limit_) remembered_attack_rate_ = rate;
    else if (output_rate_limit_) remembered_output_rate_ = rate;
    else if (selected_rate_choice_ == RateModelChoice::EventsPerSecond) remembered_ordinary_event_rate_ = rate;
    if (!fromNumeric) processing_value_.set_value(rate);
    slider_value_ = event_rate_to_slider(rate);
    update_event_rate_summary(rate);
    if ((note_attack_limit_ || output_rate_limit_) && previous != rate &&
        (previousState == PlaybackState::Playing || previousState == PlaybackState::Paused))
        try_restart_for_processing_mode_change(previousState);
    refresh();
    schedule_analysis_refresh();
}

void PlayerController::set_processing_value(int64_t value)
{
    processing_value_.set_value(value);
    ServiceDurationMode mode = engine_.service_duration_mode();
    if (mode == ServiceDurationMode::MidiBitrate) apply_midi_bitrate(processing_value_.value, true);
    else if (mode == ServiceDurationMode::EventsPerSecond) apply_events_per_second(processing_value_.value, true);
    else apply_processing_microseconds(processing_value_.value, true);
}

void PlayerController::set_slider(int sliderValue)
{
    sliderValue = std::max(0, std::min(kSliderScaleMaximum, sliderValue));
    slider_value_ = sliderValue;
    ServiceDurationMode mode = engine_.service_duration_mode();
    if (mode == ServiceDurationMode::MidiBitrate) apply_midi_bitrate(slider_to_bitrate(sliderValue), false);
    else if (mode == ServiceDurationMode::EventsPerSecond) apply_events_per_second(slider_to_event_rate(sliderValue), false);
    else apply_processing_microseconds(slider_to_microseconds(sliderValue), false);
}

void PlayerController::din_preset() { apply_midi_bitrate(models::kFivePinDinBitrate, false); }

int64_t PlayerController::scrub_value(bool processingField, int64_t start, int64_t pixels)
{
    // DD-104: one logarithmic feel over each box's whole range (same limits as v1's mappings).
    if (processingField) {
        ServiceDurationMode mode = engine_.service_duration_mode();
        if (mode == ServiceDurationMode::MidiBitrate) return log_scrub_value(static_cast<int>(start), pixels, 1, 100000000);
        if (mode == ServiceDurationMode::EventsPerSecond) return log_scrub_value(static_cast<int>(start), pixels, 0, 9999999);
        return log_scrub_value(static_cast<int>(start), pixels, 0, 1000000);
    }
    if (per_note_gate_ || delayed_note_off_) return start + pixels / 4;   // 1..128: linear (unused: the field scrubs linearly)
    return queue_age_limit_enabled_ ? log_scrub_value(static_cast<int>(start), pixels, 1, 1800000000)
                                    : log_scrub_value(static_cast<int>(start), pixels, 1, 1000000);
}

// ------------------------------------------------------------------ output

void PlayerController::close_output(const std::shared_ptr<platform::MidiOutput>& output)
{
    if (output) output->close();
}

std::shared_ptr<platform::MidiOutput> PlayerController::open_selected_output(Failure& failure)
{
    platform::OutputStatus status;
    direct_outcome_ = DirectOutcome::NotTried;
    direct_outcome_device_ = -1;
    if (kdmapi_checked_) {
        release_active_unless(kdmapi_);
        if (!kdmapi_) { failure.message = "KDMAPI is not available."; failure.title = "Unable to start playback"; return nullptr; }
        status = kdmapi_->open(0);
        if (!status.ok()) { failure.message = status.message; return nullptr; }
        return kdmapi_;
    }
    ensure_devices();
    if (output_index_ == 0) {
        release_active_unless(null_);
        status = null_->open(0);
        if (!status.ok()) { failure.message = status.message; return nullptr; }
        return null_;
    }
    if (output_index_ - 1 >= static_cast<int>(devices_.size()) || !winmm_) {
        failure.message = "No Windows MIDI output device is selected.";
        return nullptr;
    }
    const uint32_t deviceId = devices_[static_cast<size_t>(output_index_ - 1)].device_id;
    std::string knownReason;
    if (direct_checked_ && direct_ && !direct_known_unusable(knownReason)) {
        // DD-085: try the driver's own entry point first. If this device cannot be driven that way, say so (the status
        // line) and play through standard WinMM; never switch silently.
        release_active_unless(direct_);
        status = direct_->open(deviceId);
        direct_outcome_device_ = output_index_;
        if (status.ok()) {
            direct_outcome_ = DirectOutcome::InUse;
            direct_outcome_text_ = direct_->path_description();
            return direct_;
        }
        direct_outcome_ = DirectOutcome::Unavailable;
        direct_outcome_text_ = status.message;
    }
    release_active_unless(winmm_);
    status = winmm_->open(deviceId);
    if (!status.ok()) { failure.message = status.message; return nullptr; }
    return winmm_;
}

void PlayerController::release_active_unless(const std::shared_ptr<platform::MidiOutput>& keep)
{
    if (active_output_ && active_output_ != keep) close_output(active_output_);
}

void PlayerController::toggle_direct_winmm()
{
    direct_checked_ = !direct_checked_;
    direct_outcome_ = DirectOutcome::NotTried;
    direct_outcome_device_ = -1;
    output_setting_changed();
}

// The "Direct WinMM" checkbox (DD-085, DD-103 amended by P12-CHK): whether it can be used right now, and its tooltip,
// which says in plain sentences whether Direct mode is in use, or why not, and what the user can do.
namespace {

// The path description of the direct output reads "Direct WinMM driver access: <driver file> (tested driver)" or
// "... (unverified driver: not tested)". Returns the driver file name (empty if the text has another form).
std::string direct_driver_name(const std::string& description, bool& tested)
{
    const std::string prefix = "Direct WinMM driver access: ";
    tested = description.find("(tested driver)") != std::string::npos;
    if (description.compare(0, prefix.size(), prefix) != 0) return std::string();
    std::string name = description.substr(prefix.size());
    const size_t open = name.find(" (");
    if (open != std::string::npos) name.erase(open);
    return name;
}

} // namespace

void PlayerController::direct_status(bool& enabled, std::string& tooltip) const
{
    enabled = false;
    tooltip.clear();
    if (!direct_) {
        tooltip = "Direct mode is not available on this computer.";
        return;
    }
    const std::string keepsTick = direct_checked_ ? " The tick stays and works again as soon as that changes." : std::string();
    if (kdmapi_checked_) {
        tooltip = "Direct mode is not in use. KDMAPI is ticked, and KDMAPI already sends the notes straight to the synthesizer. "
            "Untick KDMAPI if you want to use Direct WinMM instead." + keepsTick;
        return;
    }
    if (output_index_ == 0) {
        tooltip = "Direct mode is not in use. The output is None, which plays no sound. Choose a MIDI device in the list to use Direct WinMM." + keepsTick;
        return;
    }
    const bool sameDevice = direct_outcome_device_ == output_index_;
    std::string probeReason;
    if (direct_known_unusable(probeReason)) {
        // R1: found when the output was selected (no Play needed): greyed out, tick kept
        while (!probeReason.empty() && (probeReason[probeReason.size() - 1] == '.' || probeReason[probeReason.size() - 1] == ' ')) probeReason.erase(probeReason.size() - 1);
        if (!probeReason.empty() && probeReason[0] >= 'a' && probeReason[0] <= 'z') probeReason[0] = static_cast<char>(probeReason[0] - 'a' + 'A');
        const std::string device = static_cast<size_t>(output_index_) < output_items_.size() ? output_items_[static_cast<size_t>(output_index_)] : std::string("this device");
        tooltip = "Direct mode cannot be used with " + device + ". " + (probeReason.empty() ? std::string() : probeReason + ". ") +
            "This box is greyed out for this output. To use Direct mode, choose another output." +
            (direct_checked_ ? " The notes play through Windows as usual. The tick stays and works again as soon as you choose another output." : std::string());
        return;
    }
    if (!direct_checked_) {
        enabled = true;
        tooltip = "Direct mode is off. Tick this box to send the notes straight to the synthesizer's driver instead of through Windows. "
            "On Windows 8 and later this is much faster, so big files are less likely to fall behind. "
            "It does not work with Windows' own synthesizer or the MIDI Mapper. "
            "This choice is remembered for the next time you start the program.";
        return;
    }
    if (sameDevice && direct_outcome_ == DirectOutcome::InUse && direct_ && direct_->driver_stopped_answering()) {
        // DD-094: a reset or close did not return in time; the driver is not called again in this session
        enabled = true;
        tooltip = "Direct mode has stopped. The synthesizer's driver did not answer when it was asked to stop, so this program does not call it again. "
            "The next playback uses Windows as usual. Restart the program to use Direct mode with this driver again.";
        return;
    }
    if (sameDevice && direct_outcome_ == DirectOutcome::InUse) {
        enabled = true;
        bool tested = false;
        const std::string driver = direct_driver_name(direct_outcome_text_, tested);
        tooltip = "Direct mode is in use. The notes go straight to " + (driver.empty() ? std::string("the synthesizer's driver") : "the driver " + driver) + ". " +
            (tested ? "This driver has been tested with this program. " : "This driver has not been tested with this program, so listen for anything that sounds wrong. ") +
            "Untick this box to go back to the normal way through Windows.";
        return;
    }
    if (sameDevice && direct_outcome_ == DirectOutcome::Unavailable) {
        std::string reason = direct_outcome_text_;
        while (!reason.empty() && (reason[reason.size() - 1] == '.' || reason[reason.size() - 1] == ' ')) reason.erase(reason.size() - 1);
        if (!reason.empty() && reason[0] >= 'a' && reason[0] <= 'z') reason[0] = static_cast<char>(reason[0] - 'a' + 'A');
        const std::string device = static_cast<size_t>(output_index_) < output_items_.size() ? output_items_[static_cast<size_t>(output_index_)] : std::string("this device");
        tooltip = "Direct mode is not in use for " + device + ". " + (reason.empty() ? std::string() : reason + ". ") +
            "The notes play through Windows as usual, and this box is greyed out for this output. "
            "To try again, choose another output and then this one. To turn Direct mode off, choose another output first.";
        return;
    }
    enabled = true;
    tooltip = "Direct mode is on. The program checks the selected device when you press Play. "
        "If the device cannot be used this way, the notes play through Windows as usual and this box tells you why.";
}

void PlayerController::set_kdmapi(bool checked)
{
    if (kdmapi_checked_ == checked) return;
    kdmapi_checked_ = checked;
    output_setting_changed();
}

void PlayerController::select_output(int index)
{
    ensure_devices();
    if (index < 0 || index >= static_cast<int>(output_items_.size())) return;
    remembered_output_name_ = output_items_[static_cast<size_t>(index)];   // also when it is the device already shown
    if (index == output_index_) return;
    output_index_ = index;
    if (probe_ && direct_ && direct_->driver_stopped_answering()) {
        // DD-094: a driver that stopped answering counts as not usable: ask again, the check skips it
        std::lock_guard<std::mutex> lock(probe_->mutex);
        probe_->entries.clear();
    }
    start_direct_probe();   // R1
    output_setting_changed();
}

void PlayerController::output_setting_changed()
{
    if (switching_output_) return;
    PlaybackState previousState = engine_.state();
    if (previousState != PlaybackState::Playing && previousState != PlaybackState::Paused) {
        // DD-111: the direct output stays open after Stop; a change of device or path while stopped closes it now (it
        // would otherwise stay open until the next Play). Not while a timed-out playback's last call is still running.
        if (direct_ && !engine_.has_live_worker()) close_output(direct_);
        return;
    }

    PlaybackSnapshot before = engine_.snapshot();
    int64_t restartPosition = before.intended_timeline_microseconds;
    switching_output_ = true;
    Failure failure;
    bool failed = false;
    try {
        engine::EngineError error = engine_.stop();
        if (!error.ok()) throw Failure{ "Unable to change MIDI output", error.message, is_timeout(error) };
        close_output(active_output_);
        active_output_.reset();
        selected_position_us_ = restartPosition;
        reset_live_measurements();

        Failure open_failure;
        std::shared_ptr<platform::MidiOutput> replacement = open_selected_output(open_failure);
        if (!replacement) throw Failure{ "Unable to change MIDI output", open_failure.message, false };
        std::shared_ptr<engine::PlaybackSong> playbackSong = playback_song_;
        // Continuation: the events at the restart position were already sent on the old output (DD-007).
        error = engine_.start(playbackSong, replacement, selected_processing_mode(), restartPosition,
            previousState == PlaybackState::Paused, true);
        if (!error.ok()) throw Failure{ "Unable to change MIDI output", error.message, false };
        active_output_ = replacement;
        engine_song_ = playbackSong;
        output_error_.clear();
        reset_live_measurements();
    }
    catch (const Failure& f) {
        failed = true;
        failure = f;
    }
    if (failed) {
        // A timed-out in-process native send still owns the selected backend until
        // its worker returns; do not close that backend underneath it.
        if (!engine_.has_live_worker()) {
            close_output(winmm_);
            close_output(direct_);
            close_output(kdmapi_);
            close_output(null_);
            active_output_.reset();
        }
        output_error_ = failure.message;
        message(UiMessage::Error, failure.title, failure.message);
    }
    switching_output_ = false;
    refresh();
}

// ------------------------------------------------------------------ transport

void PlayerController::play_clicked()
{
    PlaybackState state = engine_.state();
    if (state == PlaybackState::Playing) {
        engine::EngineError error = engine_.pause();
        if (!error.ok()) {
            output_error_ = error.message;
            restart_live_measurement_windows();
            refresh();
            engine_failed(error, is_timeout(error) ? "Unable to pause MIDI output safely" : "Unable to pause playback");
            return;
        }
        restart_live_measurement_windows();
        refresh();
        return;
    }
    if (state == PlaybackState::Paused) {
        engine_.resume();
        restart_live_measurement_windows();
        refresh();
        return;
    }
    if (!song_) {
        message(UiMessage::Information, "No MIDI file", "Open a MIDI file first.");
        return;
    }
    if (is_unsupported_forward_queue_selection()) {
        message(UiMessage::Information, "Queue policy needs simulated slowdown",
            "This policy needs simulated slowdown: a forward-only queue cannot remove MIDI that was already sent. Choose Drop incoming complete notes or Drop newest events, or turn off Apply queue limit without slowdown in the window menu.");
        return;
    }
    ensure_devices();
    if (!kdmapi_checked_ && output_index_ != 0 && output_index_ - 1 >= static_cast<int>(devices_.size())) {
        message(UiMessage::Warning, "No MIDI output", "No Windows MIDI output device is available.");
        return;
    }
    if (selected_position_us_ >= song_->duration_us) selected_position_us_ = 0;
    // DD-094: after a Stop that timed out, the previous playback's last output call is still running. Opening the output
    // now would reset and close (and reopen) it underneath that call, which can hang a driver; wait until it returns.
    if (engine_.has_live_worker()) {
        message(UiMessage::Warning, "MIDI output is still stopping",
            "The previous playback's last MIDI output call has not returned yet. Press Play again in a moment.");
        refresh();
        return;
    }
    Failure failure;
    std::shared_ptr<platform::MidiOutput> selected = open_selected_output(failure);
    if (!selected) {
        message(UiMessage::Error, "Unable to start playback", failure.message);
        return;
    }
    std::shared_ptr<engine::PlaybackSong> playbackSong = playback_song_;
    engine::EngineError error = engine_.start(playbackSong, selected, selected_processing_mode(), selected_position_us_);
    if (!error.ok()) {
        message_explained(UiMessage::Error, "Unable to start playback", explain_engine_error(error, plain_sentence_for("Unable to start playback")));
        refresh();
        return;
    }
    active_output_ = selected;
    engine_song_ = playbackSong;
    output_error_.clear();
    reset_live_measurements();
    refresh();
}

void PlayerController::stop_clicked()
{
    engine::EngineError error = engine_.stop();
    if (!error.ok()) {
        output_error_ = error.message;
        restart_live_measurement_windows();
        refresh();
        engine_failed(error, is_timeout(error) ? "Unable to stop MIDI output safely" : "Unable to stop playback");
        return;
    }
    selected_position_us_ = 0;
    restart_live_measurement_windows();
    refresh();
}

void PlayerController::seek_by(int64_t deltaUs) { perform_seek(selected_position_us_ + deltaUs); }
void PlayerController::seek_to(int64_t targetUs) { perform_seek(targetUs); }

void PlayerController::analysis_seek(int64_t targetUs)
{
    PlaybackState before = engine_.state();
    perform_seek(targetUs);
    if (before == PlaybackState::Paused && engine_.state() == PlaybackState::Paused) {
        engine_.resume();
        restart_live_measurement_windows();
        refresh();
    }
}

void PlayerController::perform_seek(int64_t targetMicroseconds)
{
    if (!song_) return;
    targetMicroseconds = std::max<int64_t>(0, std::min<int64_t>(song_->duration_us, targetMicroseconds));
    if (engine_song_ == playback_song_) {
        engine::EngineError error = engine_.seek(targetMicroseconds);
        if (!error.ok()) {
            output_error_ = error.message;
            PlaybackSnapshot stopped = engine_.snapshot();
            selected_position_us_ = stopped.intended_timeline_microseconds;
            restart_live_measurement_windows();
            refresh();
            engine_failed(error, is_timeout(error) ? "Unable to seek safely" : "Unable to seek");
            return;
        }
    }
    selected_position_us_ = targetMicroseconds;
    reset_live_measurements();
    refresh();
}

void PlayerController::reset_statistics_clicked()
{
    engine_.reset_statistics();
    reset_live_measurements();
    refresh();
}

void PlayerController::reset_live_measurements()
{
    effective_speed_.reset();
    output_rate_.reset();
    last_dropped_events_ = 0;
    overflow_visible_until_us_ = 0;
}

void PlayerController::restart_live_measurement_windows()
{
    effective_speed_.reset();
    output_rate_.restart_window();
    last_dropped_events_ = 0;
    overflow_visible_until_us_ = 0;
}

void PlayerController::set_effective_speed_window(int64_t microseconds)
{
    if (!effective_speed_.set_window(microseconds)) return;
    refresh();
}

// ------------------------------------------------------------------ statistics (RefreshStatistics)

void PlayerController::refresh()
{
    if (constructed_) ensure_devices();   // DD-057: the constructor's own refresh() leaves a deferred listing alone
    update_load_status();
    PlaybackSnapshot snapshot = engine_.snapshot();
    if (!timeline_dragging_ && snapshot.state != PlaybackState::Stopped)
        selected_position_us_ = snapshot.intended_timeline_microseconds;
    int64_t sampleTime = clock_microseconds();
    bool synchronized = snapshot.processing_mode != ProcessingMode::PerNoteIntervalGate &&
        snapshot.processing_mode != ProcessingMode::DelayedNoteOff &&
        snapshot.processing_mode != ProcessingMode::NoteAttackLimit &&
        snapshot.processing_mode != ProcessingMode::OutputRateLimit &&
        snapshot.outstanding_events == 0 && snapshot.current_lag_microseconds == 0 &&
        snapshot.last_dispatched_timeline_microseconds <= snapshot.intended_timeline_microseconds;
    std::optional<double> speed;
    std::optional<double> outputRate;
    if (snapshot.state == PlaybackState::Playing) {
        speed = effective_speed_.add(snapshot.playback_microseconds, snapshot.intended_timeline_microseconds,
            snapshot.effective_speed_frontier_microseconds, snapshot.processed_events, synchronized, sampleTime);
        outputRate = output_rate_.add(snapshot.processed_events, sampleTime);
    }
    const format::NumberCulture& culture = env_.culture;
    bool gate = per_note_gate_ || delayed_note_off_ || note_attack_limit_ || output_rate_limit_;
    std::string configuredRate;
    if (per_note_gate_) configuredRate = format::format_per_note_frame_rate(engine_.processing_microseconds(), compact_, culture);
    else if (delayed_note_off_)
        configuredRate = format::format_delayed_note_off_duration(engine_.processing_microseconds(), compact_, culture);
    else if (note_attack_limit_)
        configuredRate = format::format_note_start_limit(engine_.events_per_second(), compact_, culture);
    else if (output_rate_limit_)
        configuredRate = format::format_output_rate_limit(engine_.events_per_second(),
            snapshot.output_rate_safety_soft_excess, compact_, culture);
    else
        configuredRate = format::format_maximum_rate(snapshot, output_rate_.maximum_observed(), compact_, culture);
    statistics_.per_note_gate = gate;
    int64_t totalDropped = format::total_dropped_events(snapshot);
    if (totalDropped > last_dropped_events_) overflow_visible_until_us_ = sampleTime + 800000;
    last_dropped_events_ = totalDropped;
    const char* slash = compact_ ? kThinSlash : " / ";
    statistics_.values[0] = format::format_time(snapshot.intended_timeline_microseconds) + slash +
        format::format_time(snapshot.last_dispatched_timeline_microseconds);
    statistics_.values[1] = format::format_n0(snapshot.outstanding_events, culture) + slash +
        format::format_n0(snapshot.maximum_queue_length, culture) +
        (snapshot.virtual_queue_active ? (compact_ ? " actual" : " actual backlog") : "");
    statistics_.values[2] = configuredRate;
    statistics_.values[3] = compact_ && outputRate.has_value()
        ? format::format_n(*outputRate, 0, culture) + " events/s"
        : format::format_output_rate(outputRate, culture);
    output_rate_text_ = format::format_output_rate(outputRate, culture);
    effective_speed_text_ = format::format_effective_speed(speed, culture);
    statistics_.values[4] = format::format_event_counts(snapshot, compact_, culture);
    statistics_.values[5] = format::format_effective_speed(speed, culture);
    statistics_.values[6] = format::format_lag_milliseconds(snapshot.maximum_lag_microseconds, culture);
    statistics_.values[7] = format::format_lag_milliseconds(snapshot.current_lag_microseconds, culture);
    statistics_.queue_limited = queue_check_ && !gate;
    statistics_.age_pressure = queue_age_limit_enabled_;
    statistics_.virtual_queue = snapshot.virtual_queue_active;
    statistics_.occupied = queue_age_limit_enabled_ ? snapshot.queue_age_microseconds
        : snapshot.virtual_queue_active ? snapshot.virtual_outstanding_events : snapshot.outstanding_events;
    statistics_.limit = queue_age_limit_enabled_ ? snapshot.queue_age_limit_microseconds : queue_value_.value;
    statistics_.overflow_pulse = sampleTime < overflow_visible_until_us_;
    statistics_.speed_window_description = format::describe_speed_window(effective_speed_.window_microseconds(), culture);
    last_statistics_state_ = snapshot.state;
}

bool PlayerController::needs_periodic_refresh()
{
    return loading_ || engine_.state() == PlaybackState::Playing;
}

// ------------------------------------------------------------------ file and load

void PlayerController::open_button_clicked(bool& wantsDialog)
{
    wantsDialog = false;
    if (loading_) {
        cancel_load();
        return;
    }
    wantsDialog = true;
}

void PlayerController::open_file(const std::string& pathUtf8) { begin_load(pathUtf8); }

void PlayerController::load_cleared_presentation()
{
    loading_file_text_.clear();
    load_overall_permille_ = load_stage_permille_ = 0;
    load_status_text_.clear();
}

void PlayerController::begin_load(const std::string& path)
{
    if (path.empty()) return;
    cancel_load();
    unload_current_song();
    auto job = std::make_shared<LoadJob>();
    job->generation = ++load_generation_;
    job->path = path;
    platform::PlatformError threadError;
    std::function<void()> wake = env_.wake_ui;
    platform::FileSystem* files = env_.files;
    int pointerSize = env_.pointer_size;
    int64_t warningBytes = env_.memory_warning_bytes;
    bool forcePreflight = force_preflight_for_tests_, forceWarning = force_warning_for_tests_;
    std::shared_ptr<LoadJob> body = job;
    loading_ = true;
    loading_file_name_ = file_name_of(path);
    load_started_ticks_ = env_.services->clock().now_ticks();
    load_pause_started_ticks_ = 0;
    load_paused_ticks_ = 0;
    last_telemetry_second_ = -1;
    last_telemetry_text_.clear();
    last_load_status_.clear();
    last_tooltip_stage_.clear();
    load_overall_permille_ = load_stage_permille_ = 0;
    load_status_text_ = std::string("Starting\xE2\x80\xA6\r\n0.0% overall");
    decision_shown_ = false;
    load_job_ = job;
    update_loading_file_telemetry(true);
    job->thread = env_.services->start_thread([body, files, pointerSize, warningBytes, wake, forcePreflight, forceWarning]() {
        std::string openError;
        std::unique_ptr<platform::ByteSource> source = files->open_read(body->path, openError);
        if (!source) {
            body->result = LoadFlowResult::Failed;
            body->error.type = "IOException";
            body->error.message = openError;
        }
        else {
            body->song = std::make_shared<Song>();
            LoadFlowOptions options;
            options.monitor = body.get();
            options.force_preflight = forcePreflight;
            options.force_warning = forceWarning;
            options.warning_threshold = warningBytes;
            options.decision = [body, wake](const PreflightCounts& counts, const MemoryProjection& projection) {
                return body->ask(counts, projection, wake);
            };
            body->result = load_with_preflight(*source, pointerSize, options, *body->song, body->error);
        }
        body->done.store(true);
        if (wake) wake();
    }, platform::ThreadPriority::Normal, "MIDI file loader", threadError);
    if (!job->thread) {
        loading_ = false;
        load_job_.reset();
        load_cleared_presentation();
        message(UiMessage::Error, "Unable to load MIDI file", threadError.message.empty()
            ? std::string("The loading thread could not be started.") : threadError.message);
    }
    refresh();
}

void PlayerController::cancel_load()
{
    if (!load_job_) return;
    std::shared_ptr<LoadJob> job = load_job_;
    load_job_.reset();
    ++load_generation_;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->cancelled.store(true);
    }
    job->condition.notify_all();
    if (job->thread) job->thread->detach();
    loading_ = false;
    no_song_reason_ = 1;
    reset_loading_telemetry_state();
    load_cleared_presentation();
    decision_shown_ = false;
    refresh();
}

void PlayerController::unload_current_song()
{
    engine_.unload();
    engine_song_.reset();
    playback_song_.reset();
    song_.reset();
    track_route_.reset();
    track_routing_settings_ = routing::TrackRoutingSettings();
    ++song_revision_;
    file_path_.clear();
    selected_position_us_ = 0;
    reset_live_measurements();
}

void PlayerController::finish_load(std::shared_ptr<LoadJob> job)
{
    if (job->thread) job->thread->join();
    load_job_.reset();
    loading_ = false;
    reset_loading_telemetry_state();
    load_cleared_presentation();
    decision_shown_ = false;
    if (job->result == LoadFlowResult::Declined || (job->result == LoadFlowResult::Failed && is_cancellation(job->error))) {
        // "MIDI loading cancelled. No MIDI file is loaded."
        no_song_reason_ = 1;
    }
    else if (job->result == LoadFlowResult::Failed) {
        no_song_reason_ = 2;
        message_explained(UiMessage::Error, "Unable to load MIDI file", explain_load_error(job->error.type, job->error.message));
    }
    else {
        no_song_reason_ = 0;
        song_ = job->song;
        file_path_ = job->path;
        selected_position_us_ = 0;
        playback_song_ = engine::make_playback_song(*song_, file_path_, song_);
        ++song_revision_;
        if (song_->running_status_repairs > 0) {
            message(UiMessage::Information, "MIDI compatibility repair",
                "This file was loaded with " + format::format_n0(song_->running_status_repairs, env_.culture) +
                " nonstandard running-status repair" + (song_->running_status_repairs == 1 ? "" : "s") +
                ". The repair was limited to an unambiguous channel message immediately after MIDI metadata.");
        }
    }
    refresh();
}

void PlayerController::pump()
{
    if (ended_flag_.exchange(0) != 0) refresh();
    if (probe_ && probe_->changed.exchange(0) != 0) refresh();   // R1: a Direct mode check has answered
    std::vector<engine::EngineError> failures;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        failures.swap(failures_);
    }
    for (size_t i = 0; i < failures.size(); ++i) {
        message_explained(UiMessage::Error, "Playback error", explain_engine_error(failures[i], plain_sentence_for("Playback error")));
        refresh();
    }
    if (load_job_ && load_job_->done.load()) finish_load(load_job_);
}

bool PlayerController::large_file_decision_pending(std::string& title, std::string& heading, std::string& text)
{
    if (!load_job_ || decision_shown_) return false;
    std::shared_ptr<LoadJob> job = load_job_;
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->decision_pending || job->decision_answered) return false;
    decision_shown_ = true;
    begin_loading_decision_pause();
    title = "Large MIDI memory warning";
    heading = "This MIDI may need a large amount of memory.";
    text = format::create_large_midi_message(file_name_of(job->path), job->counts.dispatchable_events,
        job->projection, env_.culture);
    return true;
}

void PlayerController::answer_large_file(bool accepted)
{
    if (!load_job_) { decision_shown_ = false; return; }
    std::shared_ptr<LoadJob> job = load_job_;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->decision_accepted = accepted;
        job->decision_answered = true;
    }
    job->condition.notify_all();
    decision_shown_ = false;
    end_loading_decision_pause();
}

void PlayerController::begin_loading_decision_pause()
{
    if (load_started_ticks_ == 0 || load_pause_started_ticks_ != 0) return;
    load_pause_started_ticks_ = env_.services->clock().now_ticks();
    update_loading_file_telemetry(true);
}

void PlayerController::end_loading_decision_pause()
{
    if (load_pause_started_ticks_ == 0) return;
    int64_t now = env_.services->clock().now_ticks();
    if (load_started_ticks_ != 0) load_paused_ticks_ += std::max<int64_t>(0, now - load_pause_started_ticks_);
    load_pause_started_ticks_ = 0;
    update_loading_file_telemetry(true);
}

void PlayerController::reset_loading_telemetry_state()
{
    load_started_ticks_ = 0;
    load_pause_started_ticks_ = 0;
    load_paused_ticks_ = 0;
    last_telemetry_second_ = -1;
    last_telemetry_text_.clear();
}

void PlayerController::update_loading_file_telemetry(bool force)
{
    if (!loading_ || loading_file_name_.empty()) return;
    int64_t now = env_.services->clock().now_ticks();
    int64_t elapsed = format::compute_loading_elapsed_seconds(load_started_ticks_, load_pause_started_ticks_,
        load_paused_ticks_, now, env_.services->clock().frequency());
    if (!force && elapsed == last_telemetry_second_) return;
    int64_t privateBytes = env_.private_bytes ? env_.private_bytes() : 0;
    std::string telemetry = format::format_loading_telemetry(elapsed, privateBytes, compact_, env_.culture);
    if (telemetry != last_telemetry_text_) {
        last_telemetry_text_ = telemetry;
        loading_file_text_ = "Loading " + loading_file_name_ + "\xE2\x80\xA6\r\n" + telemetry;
    }
    last_telemetry_second_ = elapsed;
}

void PlayerController::update_load_status()
{
    if (!loading_ || !load_job_) return;
    LoadProgress progress;
    {
        std::lock_guard<std::mutex> lock(load_job_->mutex);
        if (!load_job_->have_progress) return;
        progress = load_job_->latest;
    }
    load_overall_permille_ = progress.overall_permille();
    load_stage_permille_ = progress.stage_permille();
    update_loading_file_telemetry(false);
    load_status_text_ = format::format_load_status(progress.stage, load_overall_permille_, load_stage_permille_, env_.culture);
    if (last_tooltip_stage_ != progress.stage) {
        last_tooltip_stage_ = progress.stage;
        load_tooltip_text_ = progress.stage + "\r\n" +
            "The upper bar shows overall parser work; the lower bar shows the current stage. Click Cancel to stop loading.";
    }
}

// ------------------------------------------------------------------ Track routing and the windows' hooks

routing::RoutingSong PlayerController::routing_song() const
{
    routing::RoutingSong source;
    if (!song_) return source;
    source.events = models::EventSource(&song_->store);
    source.track_count = song_->track_count;
    source.duration_us = song_->duration_us;
    source.tracks = &song_->tracks;
    source.identity = &song_->store;
    return source;
}

bool PlayerController::track_routing_available() { return song_ != nullptr && !loading_ && !engine_active(); }

bool PlayerController::apply_track_route(std::shared_ptr<const routing::CompiledTrackRoute> route, bool explicitlyConfirmed,
    const routing::TrackRoutingSettings* settings, std::string& error)
{
    error.clear();
    if (engine_active()) { error = "Track routing can be changed only while playback is stopped."; return false; }
    if (!song_) { error = "Open a MIDI file before applying track routing."; return false; }
    if (route && route->requires_explicit_apply && !explicitlyConfirmed) {
        error = "This track route changes instrument identity or permits conflicts and requires explicit confirmation.";
        return false;
    }
    // Construct the immutable derived view first so a bad plan cannot disturb the current song.
    std::shared_ptr<routing::TrackRoutedEventStore> view;
    std::shared_ptr<engine::PlaybackSong> routed;
    if (route) {
        try {
            view = std::make_shared<routing::TrackRoutedEventStore>(routing_song(), route);
            struct Keep {
                std::shared_ptr<Song> song;
                std::shared_ptr<const routing::CompiledTrackRoute> route;
                std::shared_ptr<routing::TrackRoutedEventStore> view;
            };
            std::shared_ptr<Keep> keep = std::make_shared<Keep>();
            keep->song = song_;
            keep->route = route;
            keep->view = view;
            routed = engine::make_playback_song(*song_, *view, file_path_, keep);
        }
        catch (const models::ModelException& exception) {
            error = exception.message;
            return false;
        }
    }
    engine_.unload();
    engine_song_.reset();
    track_route_ = route;
    track_routing_settings_ = route && settings ? *settings : routing::TrackRoutingSettings();
    playback_song_ = route ? routed : engine::make_playback_song(*song_, file_path_, song_);
    reset_live_measurements();
    ++song_revision_;
    refresh();
    return true;
}

analysis::AnalysisConfiguration PlayerController::analysis_configuration()
{
    analysis::AnalysisConfiguration c;
    c.simulate_slowdown = engine_.simulate_slowdown();
    c.service_duration_mode = engine_.service_duration_mode();
    c.processing_us = engine_.processing_microseconds();
    c.midi_bitrate = engine_.midi_bitrate();
    c.events_per_second = engine_.events_per_second();
    bool intervalGate = per_note_gate_ || delayed_note_off_;
    c.queue_length_limit_enabled = !intervalGate && remembered_ordinary_queue_limit_enabled_;
    c.queue_length_limit = remembered_event_count_limit_;
    c.queue_age_limit_enabled = queue_age_limit_enabled_;
    c.queue_age_limit_us = remembered_queue_age_limit_;
    c.overflow_policy = selected_overflow_policy();
    c.per_note_interval_gate_enabled = per_note_gate_;
    c.delayed_note_off_enabled = delayed_note_off_;
    c.scale_note_interval_by_traffic = scale_note_interval_;
    c.output_paced_timeline = output_paced_;
    c.interval_polyphony_limit_enabled = interval_polyphony_limit_enabled_;
    c.interval_polyphony_limit = remembered_interval_polyphony_limit_;
    c.note_attack_limit_enabled = note_attack_limit_;
    c.output_rate_limit_enabled = output_rate_limit_;
    c.apply_queue_limit_without_slowdown = apply_queue_limit_without_slowdown_;
    return c;
}

std::shared_ptr<const overrides::AnalysisOverrideSnapshot> PlayerController::capture_override_snapshot()
{
    std::vector<int32_t> values(16 * overrides::kAttributeCount);
    for (int channel = 0; channel < 16; ++channel)
        for (int attribute = 0; attribute < overrides::kAttributeCount; ++attribute)
            values[static_cast<size_t>(channel * overrides::kAttributeCount + attribute)] =
                engine_.channel_override(channel, static_cast<ChannelAttribute>(attribute));
    return std::make_shared<overrides::AnalysisOverrideSnapshot>(values);
}

int64_t PlayerController::playback_position_for_readouts()
{
    PlaybackSnapshot snapshot = engine_.snapshot();
    return snapshot.state == PlaybackState::Stopped ? selected_position_us_ : snapshot.intended_timeline_microseconds;
}

// ------------------------------------------------------------------ shutdown

bool PlayerController::shutdown()
{
    cancel_load();
    engine::EngineError error = engine_.dispose();
    if (!error.ok()) {
        message_explained(UiMessage::Warning, "MIDI output is still stopping", explain_engine_error(error, "The MIDI output is still stopping."));
        return false;
    }
    close_output(winmm_);
    close_output(direct_);
    close_output(kdmapi_);
    close_output(null_);
    return true;
}

// ------------------------------------------------------------------ view

const PlayerView& PlayerController::view()
{
    PlayerView& v = view_;
    PlaybackState state = engine_.state();
    bool active = state == PlaybackState::Playing || state == PlaybackState::Paused;
    bool intervalGate = per_note_gate_ || delayed_note_off_;
    bool gate = intervalGate || note_attack_limit_ || output_rate_limit_;
    bool slowdown = engine_.simulate_slowdown();
    bool limited = queue_check_;
    ServiceDurationMode mode = engine_.service_duration_mode();
    bool forwardModel = !gate && limited && !slowdown && apply_queue_limit_without_slowdown_ &&
        is_ordinary_rate_choice(selected_rate_choice_);

    // file and output
    v.loading = loading_;
    v.open_button_text = loading_ ? "Cancel" : "Open MIDI...";
    if (loading_) {
        v.file_text = loading_file_text_;
        v.file_tooltip = file_path_.empty() ? std::string() : file_path_;
        if (load_job_)
            v.file_tooltip = load_job_->path + "\r\n" +
                "The second line shows elapsed loading time and memory committed exclusively to this MIDIBottleneck Player process. Private commit may be resident or paged out; it is not managed-heap size or working set.";
        v.file_info.clear();
        v.file_info_tooltip.clear();
    }
    else if (song_) {
        v.file_text = file_name_of(file_path_);
        v.file_tooltip = file_path_;
        format::FileSummary summary;
        summary.event_count = song_->store.count();
        summary.track_count = song_->track_count;
        summary.note_count = song_->note_count;
        summary.duration_us = song_->duration_us;
        summary.running_status_repairs = song_->running_status_repairs;
        v.file_info = format::format_file_information(summary, compact_, env_.culture);
        v.file_info_tooltip = format::format_file_information(summary, false, env_.culture);
    }
    else {
        v.file_text = "No file loaded";
        v.file_tooltip.clear();
        v.file_info = settings_note_;   // empty unless the settings file needs a word (shown until a file is loaded)
        v.file_info_tooltip.clear();
    }
    if (!settings_note_.empty())
        v.file_info_tooltip += (v.file_info_tooltip.empty() ? std::string() : std::string("\r\n\r\n")) + settings_note_ +
            (settings_note_detail_.empty() ? std::string() : "\r\n\r\nDetails: " + settings_note_detail_);
    v.load_overall_permille = load_overall_permille_;
    v.load_stage_permille = load_stage_permille_;
    v.load_status = load_status_text_;
    v.load_tooltip = load_tooltip_text_;
    v.state_text = engine::playback_state_name(state);
    v.kdmapi_checked = kdmapi_checked_;
    v.direct_checked = direct_checked_;
    direct_status(v.direct_enabled, v.direct_tooltip);
    v.output_items = output_items_;
    v.output_selected = output_index_;
    v.output_enabled = !kdmapi_checked_ && !output_items_.empty();

    // queue
    v.queue_check_checked = queue_check_;
    v.queue_check_enabled = intervalGate ? !active : (!gate && !active);
    v.queue_check_text = intervalGate ? (compact_ ? "Interval limit:" : "Interval polyphony limit:")
        : queue_age_limit_enabled_ ? (compact_ ? "Wait limit:" : "Queue waiting-time limit:")
        : (compact_ ? "Queue limit:" : "Queue length limit:");
    v.queue_check_tooltip = queue_check_tooltip_;
    v.queue_value.value = queue_value_.value;
    v.queue_value.minimum = queue_value_.minimum;
    v.queue_value.maximum = queue_value_.maximum;
    v.queue_value.increment = queue_value_.increment;
    v.queue_value.enabled = intervalGate ? (limited && !active)
        : (!gate && limited && (!active || queue_age_limit_enabled_ || !forwardModel));
    v.queue_value.visible = true;
    v.queue_value.coarse_units = 1;
    v.queue_value.coarse_pixels = 4;
    v.queue_value.fine_units = 1;
    v.queue_value.fine_pixels = 2;
    v.queue_value.scrub = intervalGate ? ScrubKind::None : queue_age_limit_enabled_ ? ScrubKind::QueueAge : ScrubKind::Queue;
    v.queue_value.name = queue_value_name_;
    v.queue_value.tooltip = queue_value_tooltip_;
    v.events_label = events_label_;
    v.overflow_items.clear();
    for (size_t i = 0; i < overflow_items_.size(); ++i) v.overflow_items.push_back(overflow_items_[i].text);
    v.overflow_selected = overflow_selected_;
    v.overflow_enabled = !intervalGate && !gate && limited && !(active && forwardModel);
    v.overflow_tooltip = overflow_tooltip_;

    // rate model
    v.rate_label = compact_ ? "Rate:" : "Rate model:";
    v.rate_choice = selected_rate_choice_;
    v.rate_forward_heading = is_ordinary_rate_choice(selected_rate_choice_) && queue_check_ && apply_queue_limit_without_slowdown_;
    v.rate_tooltip =
        "None sends without a simulated rate. Ordinary models slow output, or limit future admission when forward-only queueing is selected. The per-note gate uses the processing-time interval. Group headings cannot be selected.";
    bool none = selected_rate_choice_ == RateModelChoice::None;
    if (none) v.service_value_label.clear();
    else if (mode == ServiceDurationMode::MidiBitrate) v.service_value_label = compact_ ? "Bitrate:" : "MIDI bitrate:";
    else if (mode == ServiceDurationMode::EventsPerSecond)
        v.service_value_label = note_attack_limit_ ? "Note starts/sec:"
            : output_rate_limit_ ? (compact_ ? "Max output rate:" : "Maximum output rate:") : "Events/sec:";
    else
        v.service_value_label = delayed_note_off_ ? (compact_ ? "Min NoteOn:" : "Minimum NoteOn duration:")
            : (compact_ ? "Time/event:" : "Processing time per event:");
    v.processing_value.value = processing_value_.value;
    v.processing_value.minimum = processing_value_.minimum;
    v.processing_value.maximum = processing_value_.maximum;
    v.processing_value.increment = processing_value_.increment;
    v.processing_value.visible = !none;
    v.processing_value.enabled = gate || slowdown || forwardModel;
    v.processing_value.coarse_units = 16;
    v.processing_value.coarse_pixels = 1;
    v.processing_value.fine_units = 1;
    v.processing_value.fine_pixels = 1;
    v.processing_value.scrub = ScrubKind::Rate;
    v.processing_value.name = processing_name_;
    v.processing_value.tooltip = processing_tooltip_;
    v.service_unit = mode == ServiceDurationMode::MidiBitrate ? "bit/s"
        : mode == ServiceDurationMode::EventsPerSecond ? "" : "\xC2\xB5s";
    v.service_unit_visible = !none;
    v.din_visible = !none && !compact_ && mode == ServiceDurationMode::MidiBitrate;
    v.din_enabled = !gate && (slowdown || forwardModel) && mode == ServiceDurationMode::MidiBitrate;
    v.slider_value = none ? 0 : slider_value_;
    v.slider_enabled = gate || slowdown || forwardModel;
    v.slider_tooltip = slider_tooltip_;

    // playback
    bool unsupported = is_unsupported_forward_queue_selection();
    v.play_text = state == PlaybackState::Playing ? "Pause" : state == PlaybackState::Paused ? "Resume" : "Play";
    v.play_enabled = song_ != nullptr && !loading_ && (active || !unsupported);
    v.play_tooltip = unsupported
        ? "This overflow policy requires simulated slowdown because sent MIDI cannot be removed from a forward-only queue."
        : "";
    v.stop_enabled = active || state == PlaybackState::Completed;
    v.seek_enabled = song_ != nullptr && !loading_;
    v.position_us = selected_position_us_;
    v.duration_us = song_ ? song_->duration_us : 0;
    v.statistics = statistics_;

    // menu
    v.menu.apply_queue_limit_checked = apply_queue_limit_without_slowdown_;
    v.menu.apply_queue_limit_enabled = !(active || intervalGate);
    v.menu.queue_age_checked = queue_age_limit_enabled_;
    v.menu.queue_age_enabled = !(active || intervalGate);
    v.menu.chase_checked = chase_midi_state_;
    v.menu.chase_enabled = !active;
    v.menu.preserve_checked = preserve_queue_;
    v.menu.preserve_enabled = !active;
    v.menu.output_paced_checked = output_paced_;
    v.menu.output_paced_enabled = !active;
    v.menu.scale_interval_checked = scale_note_interval_;
    v.menu.scale_interval_enabled = !active;
    v.menu.track_routing_checked = track_route_ != nullptr;
    v.menu.track_routing_enabled = song_ != nullptr && !loading_ && !active;
    return v;
}

} // namespace app
} // namespace midibn
