// v1 PlaybackEngine.cs: construction, settings, transport (Start, Seek,
// Pause, Resume, Stop, Unload), channel controls, snapshots, publication,
// output sends, waits and transport time. The worker loops are in
// engine_worker.cpp (classic modes) and engine_ordered.cpp (ordered modes).
#include "engine_internal.h"

#include "midibn/models/rate.h"

#include <string>

namespace midibn {
namespace engine {

using models::EventView;
using models::kInt64Max;
using models::max64;
using models::min64;

namespace {

int32_t wrap_increment(int32_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value) + 1u); }

const char* output_error_type(platform::OutputErrorKind kind)
{
    switch (kind) {
    case platform::OutputErrorKind::None: return "";
    case platform::OutputErrorKind::InvalidOperation: return "InvalidOperationException";
    case platform::OutputErrorKind::NotSupported: return "NotSupportedException";
    case platform::OutputErrorKind::Win32: return "Win32Exception";
    case platform::OutputErrorKind::FileNotFound: return "FileNotFoundException";
    case platform::OutputErrorKind::BadImageFormat: return "BadImageFormatException";
    case platform::OutputErrorKind::EntryPointNotFound: return "EntryPointNotFoundException";
    case platform::OutputErrorKind::DirectoryNotFound: return "DirectoryNotFoundException";
    case platform::OutputErrorKind::DriveNotFound: return "DriveNotFoundException";
    case platform::OutputErrorKind::PathTooLong: return "PathTooLongException";
    case platform::OutputErrorKind::UnauthorizedAccess: return "UnauthorizedAccessException";
    case platform::OutputErrorKind::IoError: return "IOException";
    case platform::OutputErrorKind::OperationCanceled: return "OperationCanceledException";
    case platform::OutputErrorKind::OutOfMemory: return "OutOfMemoryException";
    }
    return "Exception";
}

// The name of a setting as the person sees it, for the messages about values outside their range.
std::string setting_name(const std::string& parameter)
{
    if (parameter == "eventsPerSecond") return "events-per-second setting";
    if (parameter == "attacksPerSecond") return "note-starts-per-second setting";
    if (parameter == "intervalMicroseconds") return "note interval setting";
    if (parameter == "intervalPolyphonyLimit") return "notes-per-interval setting";
    if (parameter == "minimumActiveMicroseconds") return "minimum note length setting";
    if (parameter == "bitrate" || parameter == "midiBitrate") return "MIDI bitrate setting";
    if (parameter == "capacity") return "queue length limit";
    if (parameter == "channel") return "MIDI channel number";
    if (parameter == "value") return "channel override value";
    if (parameter == "attribute") return "channel attribute";
    if (parameter.empty()) return "setting";
    return "setting \"" + parameter + "\"";
}

} // namespace

// ---------------------------------------------------------------- errors

EngineError EngineCore::from_model(const models::ModelException& exception)
{
    EngineError error;
    error.type = exception.type;
    error.message = exception.message;
    // The models throw with the texts of the .NET exceptions v1 raised (their traces are compared with v1's);
    // the engine states what went wrong in plain words instead (register entry DD-003). The messages of the
    // models' own checks ("The rate pacer has no reservation to complete." ...) are already clear and stay.
    const std::string& type = exception.type;
    if (type == "ArgumentOutOfRangeException") {
        error.message = "The " + setting_name(exception.param) + " is outside the range the player accepts. "
                        "Choose a value inside the allowed range.";
    }
    else if (type == "OverflowException") {
        error.message = "A playback calculation became too large for the player to handle. "
                        "Use a smaller processing time, MIDI bitrate or events-per-second setting.";
    }
    else if (type == "IndexOutOfRangeException") {
        error.message = "Playback hit an internal limit (an index outside its range). "
                        "Stop and start playback again; if it keeps happening, report it with the song.";
    }
    else if (type == "ArgumentNullException") {
        error.message = "A value that playback needs was missing. Stop and start playback again.";
    }
    return error;
}

void EngineCore::raise(const char* type, const std::string& message)
{
    throw EngineError::make(type, message);
}

EngineError EngineCore::output_error(const platform::OutputStatus& status)
{
    EngineError error = EngineError::make(output_error_type(status.kind), status.message);
    if (!status.inner.empty()) error.inner_message = status.inner;
    return error;
}

void EngineCore::check_output(const platform::OutputStatus& status)
{
    if (!status.ok()) throw output_error(status);
}

void EngineCore::ChannelControlRequest::complete(const EngineError& error)
{
    if (completed.exchange(1) != 0) return;
    if (!completion) return;
    try {
        completion(error);
    }
    catch (...) {
    }
}

// ---------------------------------------------------------------- construction

namespace {

// v1 keeps `_thread` pointing at a Thread whose Start threw: IsAlive is false
// and a later Join throws ThreadStateException (see stop_worker_for_transition).
class UnstartedThread : public platform::Thread {
public:
    void join() override {}
    bool join(int32_t) override { return false; }
    bool is_alive() const override { return false; }
    bool is_current() const override { return false; }
    void detach() override {}
};

} // namespace

EngineCore::EngineCore(std::shared_ptr<platform::EngineServices> services)
    : services_owner_(std::move(services)), services_(*services_owner_), clock_(services_.clock()),
      time_(services_.clock().frequency()),
      service_settings_(std::make_shared<const ServiceDurationSettings>(ServiceDurationMode::ProcessingTime, 0,
          models::kFivePinDinBitrate, 10000))
{
    set_behind_yield_microseconds(services_.behind_yield_interval_microseconds());
    // v1 field initialisers: _wake, then _pauseAcknowledged (both ManualReset, clear).
    wake_ = services_.create_wake_event(platform::WakeEventRole::Wake, false, false);
    pause_acknowledged_ = services_.create_wake_event(platform::WakeEventRole::PauseAcknowledged, false, false);
}

EngineCore::~EngineCore()
{
    // The last owner is either PlaybackEngine (after Dispose, with no live
    // worker or with every live worker already detached) or the closure of
    // the last worker, released on that worker's own thread. Neither may wait
    // for a worker here: v1 never waits beyond its bounded Join.
    for (std::shared_ptr<platform::Thread>& thread : threads_)
        if (thread->is_current() || thread->is_alive()) thread->detach();
}

void EngineCore::abandon_workers()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    // v1: a worker still blocked in an output call after Dispose's bounded
    // waits is a background thread the process simply leaves behind.
    for (std::shared_ptr<platform::Thread>& thread : threads_)
        if (thread->is_alive()) thread->detach();
    // Nobody can observe the abandoned worker's events any more.
    playback_ended_ = std::function<void()>();
    playback_failed_ = std::function<void(const EngineError&)>();
    channel_control_failed_ = std::function<void(int, ChannelAttribute, const EngineError&)>();
    channel_control_failure_detail_ = std::function<void(int, ChannelAttribute, bool, const EngineError&)>();
}

void EngineCore::set_playback_ended_handler(std::function<void()> handler)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    playback_ended_ = std::move(handler);
}

void EngineCore::set_playback_failed_handler(std::function<void(const EngineError&)> handler)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    playback_failed_ = std::move(handler);
}

void EngineCore::set_channel_control_failed_handler(std::function<void(int, ChannelAttribute, const EngineError&)> handler)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    channel_control_failed_ = std::move(handler);
}

void EngineCore::set_channel_control_failure_detail_handler(std::function<void(int, ChannelAttribute, bool, const EngineError&)> handler)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    channel_control_failure_detail_ = std::move(handler);
}

EngineError EngineCore::dispose()
{
    // v1 Dispose: Unload, then the two wait handles are disposed (they stay
    // allocated here until destruction).
    return unload();
}

int64_t EngineCore::event_micros(int32_t index) const
{
    // micros_at reads the stored time alone; records[index] would decode the whole record first.
    return events_.is_compact() ? events_.store->records.micros_at(index) : events_.views->at(index).micros;
}

// ---------------------------------------------------------------- settings

PlaybackState EngineCore::state()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return state_;
}

void EngineCore::set_processing_microseconds(int64_t value)
{
    if (value < 0) value = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        std::shared_ptr<const ServiceDurationSettings> previous = settings();
        if (previous->processing_microseconds == value) return;
        publish_settings(std::make_shared<const ServiceDurationSettings>(previous->mode, value,
            previous->midi_bitrate, previous->events_per_second));
    }
    signal_wake();
}

void EngineCore::set_service_duration_mode(ServiceDurationMode value)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        std::shared_ptr<const ServiceDurationSettings> previous = settings();
        if (previous->mode == value) return;
        publish_settings(std::make_shared<const ServiceDurationSettings>(value, previous->processing_microseconds,
            previous->midi_bitrate, previous->events_per_second));
    }
    signal_wake();
}

void EngineCore::set_midi_bitrate(int64_t value)
{
    if (value < 1) value = 1;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        std::shared_ptr<const ServiceDurationSettings> previous = settings();
        if (previous->midi_bitrate == value) return;
        publish_settings(std::make_shared<const ServiceDurationSettings>(previous->mode,
            previous->processing_microseconds, value, previous->events_per_second));
    }
    signal_wake();
}

void EngineCore::set_events_per_second(int64_t value)
{
    if (value < 0) value = 0;
    if (value > 9999999) value = 9999999;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        std::shared_ptr<const ServiceDurationSettings> previous = settings();
        if (previous->events_per_second == value) return;
        publish_settings(std::make_shared<const ServiceDurationSettings>(previous->mode,
            previous->processing_microseconds, previous->midi_bitrate, value));
    }
    signal_wake();
}

void EngineCore::set_service_configuration(ServiceDurationMode mode, int64_t processingMicroseconds,
    int64_t midiBitrate, int64_t eventsPerSecond)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        // v1 always publishes a new settings object here, even for equal values.
        publish_settings(std::make_shared<const ServiceDurationSettings>(mode, max64(0, processingMicroseconds),
            max64(1, midiBitrate), max64(0, min64(9999999, eventsPerSecond))));
    }
    signal_wake();
}

void EngineCore::set_simulate_slowdown(bool value)
{
    simulate_slowdown_.store(value ? 1 : 0);
    signal_wake();
}

void EngineCore::set_chase_midi_state_on_play_seek(bool value)
{
    chase_midi_state_on_play_seek_.store(value ? 1 : 0);
    if (!value) pending_state_chase_event_index_.exchange(-1);
}

void EngineCore::set_interval_polyphony_limit(int32_t value)
{
    if (value < 1) value = 1;
    if (value > kDefaultIntervalPolyphonyLimit) value = kDefaultIntervalPolyphonyLimit;
    interval_polyphony_limit_.store(value);
}

void EngineCore::set_queue_length_limit(int32_t value)
{
    if (value < 1) value = 1;
    if (value > 1000000) value = 1000000;
    queue_length_limit_.store(value);
    signal_wake();
}

void EngineCore::set_queue_age_limit_enabled(bool value)
{
    queue_age_limit_enabled_.store(value ? 1 : 0);
    signal_wake();
}

void EngineCore::set_queue_age_limit_microseconds(int64_t value)
{
    if (value < 1) value = 1;
    if (value > 3600000000LL) value = 3600000000LL;
    queue_age_limit_microseconds_.exchange(value);
    signal_wake();
}

// ---------------------------------------------------------------- transport

EngineError EngineCore::start(std::shared_ptr<const PlaybackSong> song, std::shared_ptr<platform::MidiOutput> output,
    ProcessingMode mode, int64_t startMicroseconds, bool startPaused, bool continuation)
{
    EngineError error;
    capture([&] { start_impl(song, output, mode, startMicroseconds, startPaused, continuation); }, error);
    return error;
}

void EngineCore::start_impl(std::shared_ptr<const PlaybackSong> song, std::shared_ptr<platform::MidiOutput> output,
    ProcessingMode mode, int64_t startMicroseconds, bool startPaused, bool continuation)
{
    if (!song) raise("ArgumentNullException", "No song was supplied to start playback.");
    if (output == nullptr) raise("ArgumentNullException", "No MIDI output was supplied to start playback.");
    bool virtualForward = mode == ProcessingMode::Drop && simulate_slowdown_.load() == 0 &&
        apply_queue_limit_without_slowdown_.load() != 0;
    OverflowPolicy startPolicy = static_cast<OverflowPolicy>(overflow_policy_.load());
    bool ageLimited = mode == ProcessingMode::Drop && queue_age_limit_enabled_.load() != 0;
    if (ageLimited && virtualForward)
        raise("InvalidOperationException", "Queue waiting-time limits require a retained delayed queue and cannot be combined with forward-only admission.");
    if (ageLimited && startPolicy != OverflowPolicy::DropOldest && startPolicy != OverflowPolicy::DropOldestCompleteNote &&
        startPolicy != OverflowPolicy::ClearBufferAndCatchUp)
        raise("InvalidOperationException", "Choose Drop oldest complete note, Drop oldest events, or Clear buffer for a queue waiting-time limit.");
    if (virtualForward && startPolicy != OverflowPolicy::DropNewest && startPolicy != OverflowPolicy::DropIncomingCompleteNotes)
        raise("InvalidOperationException", "This overflow policy can remove MIDI that was already sent. Turn on Simulate slowdown, or choose Drop incoming complete notes or Drop newest events.");
    stop_worker_for_transition("stopping playback", ChannelTransitionBoundary::Stop);
    output->set_source_file(song->file_path);
    startMicroseconds = clamp_position(*song, startMicroseconds);
    std::lock_guard<std::recursive_mutex> lock(sync_);
    song_ = song;
    events_ = song->events;
    payload_store_ = song->payload_store;
    output_ = output.get();
    output_owner_ = output;
    mode_ = mode;
    virtual_forward_drop_active_ = virtualForward;
    start_event_index_ = continuation ? find_first_event_after(*song, startMicroseconds)
                                      : find_first_event_at_or_after(*song, startMicroseconds);
    pending_state_chase_event_index_.store(chase_midi_state_on_play_seek() && start_event_index_ > 0
        ? start_event_index_ : -1);
    has_published_queue_ = false;
    has_published_structural_due_work_ = false;
    uses_pre_admission_filter_accounting_.store(0);
    transport_base_ticks_ = micros_to_ticks(startMicroseconds);
    run_start_stamp_ = now_stamp();
    output_paced_hold_depth_ = 0;
    output_paced_hold_start_stamp_ = 0;
    output_paced_hold_transport_ticks_ = 0;
    reset_statistics_locked();
    if (channel_monitoring_enabled_.load() != 0) {
        channel_state_.reset_all_direct();
        std::shared_ptr<ChannelPlaybackSnapshot> channelSnapshot = channel_state_.create_snapshot();
        channel_overrides_.apply_to_snapshot(*channelSnapshot);
        channel_routing_.apply_to_snapshot(*channelSnapshot);
        std::atomic_store(&published_channel_state_, std::shared_ptr<const ChannelPlaybackSnapshot>(channelSnapshot));
    }
    channel_overrides_.mark_all_forced_pending();
    last_dispatched_microseconds_ = startMicroseconds;
    effective_speed_frontier_microseconds_ = startMicroseconds;
    state_ = startPaused ? PlaybackState::Paused : PlaybackState::Playing;
    dispatch_suspended_.store(startPaused ? 1 : 0);
    silence_when_worker_exits_ = false;
    has_worker_exit_silence_failure_ = false;
    worker_exit_silence_failure_ = EngineError();
    try {
        start_worker_locked();
    }
    catch (...) {
        // DD-045: a playback thread that cannot be started leaves the engine Stopped (v1 stayed Playing with no
        // worker, so the window kept showing a playback that was not running).
        state_ = PlaybackState::Stopped;
        dispatch_suspended_.store(0);
        throw;
    }
}

void EngineCore::reap_finished_threads_locked()
{
    for (size_t i = 0; i < threads_.size();) {
        platform::Thread* t = threads_[i].get();
        if (t != thread_.get() && !t->is_current() && !t->is_alive()) threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void EngineCore::start_worker_locked()
{
    // v1: _thread = new Thread(PlaybackWorker) { Name = "MIDI bottleneck scheduler",
    // IsBackground = true, Priority = AboveNormal }; _thread.Start();
    reap_finished_threads_locked();
    platform::PlatformError error;
    // The body owns the core: an abandoned worker keeps everything it uses.
    std::shared_ptr<EngineCore> self = shared_from_this();
    std::unique_ptr<platform::Thread> started = services_.start_thread([self] { self->playback_worker(); },
        platform::ThreadPriority::AboveNormal, "MIDI bottleneck scheduler", error);
    if (!started) {
        // v1 assigned _thread before Thread.Start threw, so its next Stop failed too; v2 keeps the marker
        // (Stop recognises it and stops cleanly, DD-003), start_impl returns the engine to Stopped (DD-045)
        // and the cause is reported clearly.
        unstarted_ = std::make_shared<UnstartedThread>();
        thread_ = unstarted_;
        raise("OutOfMemoryException", error.message.empty() ? messages::playback_thread_not_started() : error.message);
    }
    thread_ = std::shared_ptr<platform::Thread>(started.release());
    threads_.push_back(thread_);
}

EngineError EngineCore::seek(int64_t targetMicroseconds)
{
    EngineError error;
    capture([&] { seek_impl(targetMicroseconds); }, error);
    return error;
}

void EngineCore::seek_impl(int64_t targetMicroseconds)
{
    std::shared_ptr<const PlaybackSong> song;
    PlaybackState previousState;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        song = song_;
        if (!song) return;
        targetMicroseconds = clamp_position(*song, targetMicroseconds);
        previousState = state_;
        if (preserve_playback_queue_on_pause_seek() &&
            (previousState == PlaybackState::Playing || previousState == PlaybackState::Paused) &&
            thread_ && thread_->is_alive()) {
            if (static_cast<int32_t>(preserved_seek_requests_.size()) >= kMaximumPendingPreservedSeeks)
                raise("InvalidOperationException", "Too many playback-preserving Seek requests are pending. Resume or wait for the retained queue to advance before seeking again.");
            preserved_seek_generation_ = wrap_increment(preserved_seek_generation_);
            PreservedSeekRequest request;
            request.generation = preserved_seek_generation_;
            request.target_microseconds = targetMicroseconds;
            request.target_event_index = find_first_event_at_or_after(*song, targetMicroseconds);
            preserved_seek_requests_.push_back(request);
            intended_timeline_override_microseconds_ = targetMicroseconds;
            signal_wake();
            return;
        }
    }

    stop_worker_for_transition("seeking", ChannelTransitionBoundary::Seek);

    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        transport_base_ticks_ = micros_to_ticks(targetMicroseconds);
        start_event_index_ = find_first_event_at_or_after(*song, targetMicroseconds);
        pending_state_chase_event_index_.store(chase_midi_state_on_play_seek() && start_event_index_ > 0
            ? start_event_index_ : -1);
        has_published_queue_ = false;
        has_published_structural_due_work_ = false;
        uses_pre_admission_filter_accounting_.store(0);
        reset_statistics_locked();
        last_dispatched_microseconds_ = targetMicroseconds;
        effective_speed_frontier_microseconds_ = targetMicroseconds;
        intended_timeline_override_microseconds_ = -1;
        preserved_seek_requests_.clear();
        thread_.reset();

        if (previousState == PlaybackState::Playing || previousState == PlaybackState::Paused) {
            state_ = previousState;
            dispatch_suspended_.store(state_ == PlaybackState::Playing ? 0 : 1);
            if (state_ == PlaybackState::Playing) run_start_stamp_ = now_stamp();
            start_worker_locked();
        }
        else {
            state_ = PlaybackState::Stopped;
            dispatch_suspended_.store(1);
        }
    }
    signal_wake();
}

EngineError EngineCore::pause()
{
    EngineError error;
    capture([&] { pause_impl(); }, error);
    return error;
}

void EngineCore::pause_impl()
{
    std::shared_ptr<const PlaybackSong> song;
    int32_t preservingGeneration = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (state_ != PlaybackState::Playing) return;
        song = song_;
        if (preserve_playback_queue_on_pause_seek() && thread_ && thread_->is_alive()) {
            freeze_transport_locked();
            state_ = PlaybackState::Paused;
            dispatch_suspended_.store(1);
            pause_request_generation_ = wrap_increment(pause_request_generation_);
            preservingGeneration = pause_request_generation_;
            pause_acknowledged_->reset();
        }
    }

    if (preservingGeneration != 0) {
        signal_wake();
        int32_t timeout = worker_stop_timeout_milliseconds_.load();
        if (pause_acknowledged_->wait(timeout)) return;
        stop_worker_for_transition("acknowledging playback-preserving Pause", ChannelTransitionBoundary::Pause);
        raise("PlaybackWorkerTimeoutException", "The MIDI output call did not return within " + std::to_string(timeout) +
            " ms while pausing with queue preservation. Playback was stopped and no replacement scheduler was started.");
    }

    // Pause is a native-output boundary, not merely a scheduler flag.
    stop_worker_for_transition("pausing playback", ChannelTransitionBoundary::Pause);

    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (!song || song_ != song) return;
        int64_t pausedMicroseconds = clamp_position(*song, ticks_to_micros(transport_base_ticks_));
        transport_base_ticks_ = micros_to_ticks(pausedMicroseconds);
        // The events at the paused time have already been sent; v1 restarted at the first event at or after it and so
        // sent an event that landed exactly on the paused time a second time (register entry DD-007).
        start_event_index_ = find_first_event_after(*song, pausedMicroseconds);
        pending_state_chase_event_index_.store(chase_midi_state_on_play_seek() && start_event_index_ > 0
            ? start_event_index_ : -1);
        effective_speed_frontier_microseconds_ = pausedMicroseconds;
        has_published_queue_ = false;
        has_published_structural_due_work_ = false;
        uses_pre_admission_filter_accounting_.store(0);
        state_ = PlaybackState::Paused;
        dispatch_suspended_.store(1);
        silence_when_worker_exits_ = false;
        has_worker_exit_silence_failure_ = false;
        worker_exit_silence_failure_ = EngineError();
        start_worker_locked();
    }
    signal_wake();
}

void EngineCore::resume()
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (state_ != PlaybackState::Paused) return;
        run_start_stamp_ = now_stamp();
        state_ = PlaybackState::Playing;
        dispatch_suspended_.store(0);
    }
    signal_wake();
}

EngineError EngineCore::stop()
{
    EngineError error;
    capture([&] { stop_worker_for_transition("stopping playback", ChannelTransitionBoundary::Stop); }, error);
    return error;
}

void EngineCore::stop_worker_for_transition(const char* operation, ChannelTransitionBoundary channelBoundary)
{
    std::shared_ptr<platform::Thread> thread;
    platform::MidiOutput* output;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (state_ == PlaybackState::Playing) freeze_transport_locked();
        state_ = PlaybackState::Stopped;
        dispatch_suspended_.store(1);
        thread = thread_;
        // A thread whose Start failed has nothing to wait for: v1 joined it and failed again ("Thread has not been
        // started"), so Stop could never recover; v2 stops cleanly (register entry DD-003).
        if (thread && thread == unstarted_) {
            thread_.reset();
            unstarted_.reset();
            thread.reset();
        }
        output = output_;
        has_worker_exit_silence_failure_ = false;
        worker_exit_silence_failure_ = EngineError();
        silence_when_worker_exits_ = thread != nullptr;
        intended_timeline_override_microseconds_ = -1;
        preserved_seek_requests_.clear();
    }
    signal_wake();

    if (!thread) {
        if (output != nullptr) check_output(platform::reset_and_silence(output));
    }
    else if (!thread->is_current()) {
        int32_t timeout = worker_stop_timeout_milliseconds_.load();
        if (!thread->join(timeout)) {
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                queue_length_ = 0;
                outstanding_events_ = 0;
                current_lag_microseconds_ = 0;
            }
            raise("PlaybackWorkerTimeoutException", "The MIDI output call did not return within " +
                std::to_string(timeout) + " ms while " + operation +
                ". Playback remains stopped and no replacement scheduler was started. "
                "The output is not reset concurrently with its active call; a final reset and all-notes-off panic "
                "will run after that call returns.");
        }

        bool hasExitFailure;
        EngineError exitSilenceFailure;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            hasExitFailure = has_worker_exit_silence_failure_;
            exitSilenceFailure = worker_exit_silence_failure_;
            has_worker_exit_silence_failure_ = false;
            worker_exit_silence_failure_ = EngineError();
        }
        if (hasExitFailure) {
            EngineError error = EngineError::make("InvalidOperationException",
                std::string("The scheduler stopped, but MIDI reset/all-notes-off failed while ") + operation + ".");
            error.inner_type = exitSilenceFailure.type;
            error.inner_message = exitSilenceFailure.message;
            throw error;
        }
    }

    std::vector<ControlRequestPtr> retiredControls;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        queue_length_ = 0;
        outstanding_events_ = 0;
        current_lag_microseconds_ = 0;
        if (channelBoundary == ChannelTransitionBoundary::Stop) effective_speed_frontier_microseconds_ = 0;
        retiredControls = drain_channel_control_requests_locked();
    }
    complete_retired_channel_controls(retiredControls, EngineError::make("OperationCanceledException",
        std::string("The channel-control request was retired by the ") + operation + " boundary."));
    channel_overrides_.mark_all_forced_pending();
    if (channelBoundary == ChannelTransitionBoundary::Stop) {
        channel_overrides_.reset_statistics();
        channel_routing_.reset_statistics();
    }
    if (channel_monitoring_enabled_.load() != 0) {
        if (channelBoundary == ChannelTransitionBoundary::Pause) channel_state_.pause_boundary_direct();
        else if (channelBoundary == ChannelTransitionBoundary::Seek) channel_state_.seek_boundary_direct();
        else channel_state_.stop_boundary_direct();
        publish_channel_state(true);
    }
}

bool EngineCore::has_live_worker()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return thread_ && thread_->is_alive();
}

EngineError EngineCore::unload()
{
    EngineError error;
    capture([&] { unload_impl(); }, error);
    return error;
}

void EngineCore::unload_impl()
{
    stop_worker_for_transition("stopping playback", ChannelTransitionBoundary::Stop);
    std::lock_guard<std::recursive_mutex> lock(sync_);
    if (thread_ && thread_->is_alive())
        raise("InvalidOperationException", "The MIDI scheduler did not stop in time, so its song cannot yet be unloaded safely.");
    if (output_ != nullptr) output_->set_source_file(std::string());
    song_.reset();
    events_ = models::EventSource();
    payload_store_ = nullptr;
    output_ = nullptr;
    output_owner_.reset();
    start_event_index_ = 0;
    pending_state_chase_event_index_.store(-1);
    intended_timeline_override_microseconds_ = -1;
    preserved_seek_requests_.clear();
    last_dispatched_microseconds_ = 0;
    effective_speed_frontier_microseconds_ = 0;
    transport_base_ticks_ = 0;
    reset_statistics_locked();
    channel_overrides_.clear();
    channel_routing_.clear();
    channel_control_requests_.clear();
    if (channel_monitoring_enabled_.load() != 0) {
        channel_state_.reset_all_direct();
        std::shared_ptr<ChannelPlaybackSnapshot> channelSnapshot = channel_state_.create_snapshot();
        channel_overrides_.apply_to_snapshot(*channelSnapshot);
        channel_routing_.apply_to_snapshot(*channelSnapshot);
        std::atomic_store(&published_channel_state_, std::shared_ptr<const ChannelPlaybackSnapshot>(channelSnapshot));
    }
}

bool EngineCore::has_attached_song()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return song_ != nullptr;
}

bool EngineCore::has_attached_output()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return output_ != nullptr;
}

// ---------------------------------------------------------------- statistics and channels

void EngineCore::reset_statistics()
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        maximum_queue_length_ = outstanding_events_;
        maximum_queue_age_microseconds_ = queue_age_microseconds_;
        processed_events_ = 0;
        dropped_events_ = 0;
        gate_filtered_events_ = 0;
        musical_limit_rejected_attacks_ = 0;
        musical_limit_suppressed_releases_ = 0;
        output_rate_rejected_attacks_ = 0;
        output_rate_suppressed_releases_ = 0;
        output_rate_rejected_other_messages_ = 0;
        output_rate_safety_soft_excess_ = 0;
        output_rate_safety_bypass_messages_ = 0;
        virtual_maximum_queue_length_ = virtual_outstanding_events_;
        maximum_lag_microseconds_ = current_lag_microseconds_;
    }
    channel_overrides_.reset_statistics();
    channel_routing_.reset_statistics();
    if (channel_monitoring_enabled_.load() != 0) {
        channel_state_.request_statistics_reset();
        std::shared_ptr<const ChannelPlaybackSnapshot> published = std::atomic_load(&published_channel_state_);
        if (published) {
            std::shared_ptr<ChannelPlaybackSnapshot> reset = published->with_statistics_reset();
            channel_overrides_.apply_to_snapshot(*reset);
            channel_routing_.apply_to_snapshot(*reset);
            std::atomic_store(&published_channel_state_, std::shared_ptr<const ChannelPlaybackSnapshot>(reset));
        }
    }
}

void EngineCore::set_channel_monitoring(bool enabled)
{
    channel_monitoring_enabled_.store(enabled ? 1 : 0);
    if (enabled) {
        channel_state_.request_full_reset();
        std::shared_ptr<ChannelPlaybackSnapshot> snapshot = ChannelPlaybackSnapshot::empty();
        channel_overrides_.apply_to_snapshot(*snapshot);
        channel_routing_.apply_to_snapshot(*snapshot);
        std::atomic_store(&published_channel_state_, std::shared_ptr<const ChannelPlaybackSnapshot>(snapshot));
        last_channel_publish_stamp_.exchange(0);
    }
    else std::atomic_store(&published_channel_state_, std::shared_ptr<const ChannelPlaybackSnapshot>());
}

int32_t EngineCore::channel_override(int channel, ChannelAttribute attribute) const
{
    if (channel < 0 || channel >= 16 || static_cast<int>(attribute) < 0 ||
        static_cast<int>(attribute) >= ChannelOverrideState::kAttributeCount)
        return ChannelOverrideState::kAutoValue;
    return channel_overrides_.get_value(channel, attribute);
}

bool EngineCore::is_channel_enabled(int channel) const
{
    if (channel < 0 || channel >= 16) return false;
    return channel_routing_.is_enabled(channel);
}

std::shared_ptr<const ChannelPlaybackSnapshot> EngineCore::channel_snapshot() const
{
    return std::atomic_load(&published_channel_state_);
}

bool EngineCore::chase_state_for_new_monitor(ChannelControlCompletion completion)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        std::shared_ptr<const PlaybackSong> song = song_;
        if (!thread_ || !song || output_ == nullptr || chase_midi_state_on_play_seek_.load() == 0) return false;
        ControlRequestPtr request = std::make_shared<ChannelControlRequest>();
        request->kind = ChannelControlKind::MonitorStateChase;
        request->expected_song = song;
        request->channel = -1;
        request->completion = std::move(completion);
        channel_control_requests_.push_back(request);
    }
    signal_wake();
    return true;
}

EngineError EngineCore::set_channel_override(int channel, ChannelAttribute attribute, int32_t value)
{
    EngineError error;
    capture([&] {
        if (!channel_overrides_.set_value(channel, attribute, value)) return;
        if (value == ChannelOverrideState::kAutoValue) {
            if (channel_monitoring_enabled_.load() != 0) channel_state_.mark_attribute_historical_direct(channel, attribute);
            publish_channel_state(true);
            return;
        }

        bool sentDirectly = false;
        try {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (!thread_ && output_ != nullptr && channel_routing_.is_enabled(channel)) {
                // v1 sends the MidiEvent through an implicit MidiEventView conversion (event index 0).
                EventView message = ChannelOverrideState::create_message(channel, attribute, value, 0);
                send_protected_event(message);
                if (channel_monitoring_enabled_.load() != 0) {
                    channel_state_.record_override_applied(channel, attribute, value);
                    if (channel_overrides_.get_value(channel, attribute) != value)
                        channel_state_.mark_attribute_historical_direct(channel, attribute);
                }
                // Consume the pending bit installed by SetValue. No worker
                // can start while the lifecycle lock is held.
                int32_t pending = channel_overrides_.take_pending_mask(channel);
                channel_overrides_.requeue(channel, pending & ~(1 << static_cast<int>(attribute)));
                sentDirectly = true;
            }
        }
        catch (...) {
            publish_channel_state(true);
            throw;
        }
        if (!sentDirectly) signal_wake();
        publish_channel_state(true);
    }, error);
    return error;
}

EngineError EngineCore::set_channel_enabled(int channel, bool enabled)
{
    EngineError error;
    capture([&] {
        ControlRequestPtr request = std::make_shared<ChannelControlRequest>();
        request->kind = ChannelControlKind::SetEnabled;
        request->channel = channel;
        request->attribute = ChannelAttribute::BankMsb;
        request->value = enabled ? 1 : 0;
        submit_channel_control(request);
    }, error);
    return error;
}

EngineError EngineCore::chase_latest_source_channel_attribute(int channel, ChannelAttribute attribute,
    ChannelControlCompletion completion)
{
    EngineError error;
    capture([&] {
        if (!channel_routing_.is_enabled(channel)) {
            EngineError failure = EngineError::make("InvalidOperationException",
                "Enable this MIDI channel before restoring its source value.");
            if (completion) { completion(failure); return; }
            throw failure;
        }
        std::shared_ptr<const PlaybackSong> song;
        int64_t positionMicroseconds;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            song = song_;
            positionMicroseconds = !song ? 0 : ticks_to_micros(current_transport_ticks_locked());
        }
        int value = 0;
        if (!song || !song->try_get_latest_source_value(channel, attribute, clamp_position(*song, positionMicroseconds),
                value)) {
            EngineError failure = EngineError::make("InvalidOperationException",
                "No source MIDI value for this channel attribute exists at the current position.");
            if (completion) { completion(failure); return; }
            throw failure;
        }
        ControlRequestPtr request = std::make_shared<ChannelControlRequest>();
        request->kind = ChannelControlKind::Chase;
        request->channel = channel;
        request->attribute = attribute;
        request->value = value;
        request->completion = completion;
        submit_channel_control(request);
    }, error);
    return error;
}

EngineError EngineCore::send_explicit_channel_attribute(int channel, ChannelAttribute attribute, int32_t value,
    ChannelControlCompletion completion)
{
    EngineError error;
    capture([&] {
        ControlRequestPtr request = std::make_shared<ChannelControlRequest>();
        request->kind = ChannelControlKind::Chase;
        request->channel = channel;
        request->attribute = attribute;
        request->value = value;
        request->completion = completion;
        submit_channel_control(request);
    }, error);
    return error;
}

void EngineCore::submit_channel_control(const ControlRequestPtr& request)
{
    bool queued;
    bool failed = false;
    EngineError immediateError;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        queued = thread_ != nullptr;
        if (queued) channel_control_requests_.push_back(request);
        else {
            failed = !capture([&] {
                execute_channel_control(*request);
                if (request->kind == ChannelControlKind::SetEnabled && request->value != 0) apply_pending_overrides();
            }, immediateError);
        }
    }
    if (queued) {
        // Nothing is observable until the worker reaches the ordered boundary.
        signal_wake();
        return;
    }
    // Publish first so a callback cannot read the pre-control snapshot.
    publish_channel_state(true);
    request->complete(failed ? immediateError : EngineError());
    if (failed && !request->has_completion()) throw immediateError;
}

void EngineCore::apply_pending_channel_controls()
{
    while (true) {
        ControlRequestPtr request;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (channel_control_requests_.empty()) return;
            request = channel_control_requests_.front();
            channel_control_requests_.pop_front();
        }
        EngineError error;
        bool failed = !capture([&] { execute_channel_control(*request); }, error);
        if (failed) {
            std::function<void(int, ChannelAttribute, const EngineError&)> handler;
            std::function<void(int, ChannelAttribute, bool, const EngineError&)> detail;
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                handler = channel_control_failed_;
                detail = channel_control_failure_detail_;
            }
            if (handler) handler(request->channel, request->attribute, error);
            if (detail) detail(request->channel, request->attribute, request->kind == ChannelControlKind::SetEnabled, error);
        }
        publish_channel_state(true);
        // The callback may immediately refresh Channels. It must never
        // outrun the snapshot representing this exact control result.
        request->complete(failed ? error : EngineError());
    }
}

std::vector<EngineCore::ControlRequestPtr> EngineCore::drain_channel_control_requests_locked()
{
    std::vector<ControlRequestPtr> requests;
    if (channel_control_requests_.empty()) return requests;
    requests.reserve(channel_control_requests_.size());
    while (!channel_control_requests_.empty()) {
        requests.push_back(channel_control_requests_.front());
        channel_control_requests_.pop_front();
    }
    return requests;
}

void EngineCore::complete_retired_channel_controls(const std::vector<ControlRequestPtr>& requests,
    const EngineError& error)
{
    for (const ControlRequestPtr& request : requests) request->complete(error);
}

void EngineCore::execute_channel_control(const ChannelControlRequest& request)
{
    if (request.kind == ChannelControlKind::MonitorStateChase) {
        std::shared_ptr<const PlaybackSong> song;
        platform::MidiOutput* output;
        int64_t position;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            song = song_;
            output = output_;
            position = ticks_to_micros(current_transport_ticks_locked());
        }
        if (song != request.expected_song || output == nullptr)
            raise("InvalidOperationException", "The MIDI file or output changed before Channels could refresh.");
        if (chase_midi_state_on_play_seek_.load() == 0 || channel_monitoring_enabled_.load() == 0) return;
        int32_t exclusive = song->events.lower_bound_by_time(clamp_position(*song, position));
        std::vector<EventView> messages = chase_messages(*song, exclusive, 0);
        for (const EventView& message : messages) {
            if (!is_active() || channel_monitoring_enabled_.load() == 0)
                raise("InvalidOperationException", "Playback changed before Channels could refresh.");
            send_rate_counted_essential_event(*output, message);
            if (channel_monitoring_enabled_.load() != 0) channel_state_.record_monitor_state_chase_applied(message);
        }
        return;
    }
    if (request.kind == ChannelControlKind::SetEnabled) {
        bool enabled = request.value != 0;
        if (enabled) {
            if (channel_routing_.set_enabled(request.channel, true))
                channel_overrides_.mark_channel_forced_pending(request.channel);
            return;
        }
        if (!channel_routing_.is_enabled(request.channel)) return;
        if (output_ != nullptr) {
            send_protected_event(ChannelOverrideState::create_controller_message(request.channel, 64, 0, 0));
            send_protected_event(ChannelOverrideState::create_controller_message(request.channel, 120, 0, 0));
            send_protected_event(ChannelOverrideState::create_controller_message(request.channel, 123, 0, 0));
        }
        channel_routing_.set_enabled(request.channel, false);
        if (channel_monitoring_enabled_.load() != 0) channel_state_.silence_channel_direct(request.channel);
        return;
    }
    if (output_ == nullptr) raise("InvalidOperationException", "No MIDI output session is available.");
    if (!channel_routing_.is_enabled(request.channel))
        raise("InvalidOperationException", "Enable this MIDI channel before restoring its source value.");
    send_rate_counted_essential_event(*output_,
        ChannelOverrideState::create_message(request.channel, request.attribute, request.value, 0));
    if (channel_monitoring_enabled_.load() != 0)
        channel_state_.record_manual_chase_applied(request.channel, request.attribute, request.value);
}

// ---------------------------------------------------------------- snapshot

PlaybackSnapshot EngineCore::snapshot()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    settle_immediate_burst_locked();
    int64_t playbackTicks = current_transport_ticks_locked();
    bool active = state_ == PlaybackState::Playing || state_ == PlaybackState::Paused;
    // Source arrivals continue while an output call is blocked.
    if (mode_ == ProcessingMode::Queue && has_published_queue_ && active) {
        measure_published_due_work(published_next_process_, 0, -1, kInt64Max, published_in_service_,
            published_in_service_due_microseconds(), kInt64Max, 0, playbackTicks);
    }
    else if (mode_ == ProcessingMode::Drop && has_published_queue_ && active) {
        measure_published_due_work(published_drop_next_arrival_, published_drop_pending_,
            published_oldest_pending_event_, kInt64Max, published_in_service_, published_in_service_due_microseconds(),
            kInt64Max, 0, playbackTicks);
    }
    else if (has_published_structural_due_work_ && active) {
        measure_published_due_work(published_structural_next_arrival_, published_structural_pending_, -1,
            published_structural_oldest_due_microseconds_, published_structural_in_service_,
            published_structural_in_service_due_microseconds_, published_structural_future_due_microseconds_,
            published_structural_future_count_, playbackTicks);
    }
    int64_t playbackUs = ticks_to_micros(playbackTicks);
    PlaybackSnapshot s;
    s.state = state_;
    std::shared_ptr<const ServiceDurationSettings> current = settings();
    s.processing_microseconds = current->processing_microseconds;
    s.service_duration_mode = current->mode;
    s.midi_bitrate = current->midi_bitrate;
    s.events_per_second = current->events_per_second;
    s.simulate_slowdown = simulate_slowdown_.load() != 0;
    s.queue_length_limit_enabled = mode_ == ProcessingMode::Drop;
    s.processing_mode = mode_;
    s.queue_length_limit = queue_length_limit_.load();
    s.queue_age_limit_enabled = queue_age_limit_enabled_.load() != 0;
    s.queue_age_limit_microseconds = queue_age_limit_microseconds_.load();
    s.queue_age_microseconds = queue_age_microseconds_;
    s.maximum_queue_age_microseconds = maximum_queue_age_microseconds_;
    s.overflow_policy = static_cast<OverflowPolicy>(overflow_policy_.load());
    s.queue_length = queue_length_;
    s.outstanding_events = outstanding_events_;
    s.maximum_queue_length = maximum_queue_length_;
    s.processed_events = processed_events_;
    s.dropped_events = dropped_events_;
    s.gate_filtered_events = gate_filtered_events_;
    s.musical_limit_rejected_attacks = musical_limit_rejected_attacks_;
    s.musical_limit_suppressed_releases = musical_limit_suppressed_releases_;
    s.output_rate_rejected_attacks = output_rate_rejected_attacks_;
    s.output_rate_suppressed_releases = output_rate_suppressed_releases_;
    s.output_rate_rejected_other_messages = output_rate_rejected_other_messages_;
    s.output_rate_safety_soft_excess = output_rate_safety_soft_excess_;
    s.output_rate_safety_bypass_messages = output_rate_safety_bypass_messages_;
    s.virtual_queue_active = virtual_forward_drop_active_;
    s.virtual_queue_length = virtual_queue_length_;
    s.virtual_outstanding_events = virtual_outstanding_events_;
    s.virtual_maximum_queue_length = virtual_maximum_queue_length_;
    s.playback_microseconds = playbackUs;
    int64_t intended = intended_timeline_override_microseconds_ >= 0 ? intended_timeline_override_microseconds_ : playbackUs;
    s.intended_timeline_microseconds = !song_ ? 0 : min64(intended, song_->duration_us);
    s.last_dispatched_timeline_microseconds = last_dispatched_microseconds_;
    s.effective_speed_frontier_microseconds = mode_ == ProcessingMode::PerNoteIntervalGate ||
        mode_ == ProcessingMode::DelayedNoteOff || mode_ == ProcessingMode::OutputRateLimit
        ? effective_speed_frontier_microseconds_ : last_dispatched_microseconds_;
    s.current_lag_microseconds = current_lag_microseconds_;
    s.maximum_lag_microseconds = maximum_lag_microseconds_;
    return s;
}

void EngineCore::measure_published_due_work(int32_t nextSource, int64_t admittedPending, int32_t admittedOldestEvent,
    int64_t admittedOldestDueMicroseconds, bool inService, int64_t inServiceDueMicroseconds,
    int64_t structuralDueMicroseconds, int32_t structuralDueCount, int64_t playbackTicks)
{
    int32_t dueEnd = preserved_seek_requests_.empty() ? find_first_event_after_transport(nextSource, playbackTicks)
                                                      : nextSource;
    int64_t newlyDue = count_snapshot_eligible(nextSource, dueEnd);
    int64_t playbackMicroseconds = ticks_to_micros(playbackTicks);
    int64_t dueStructural = structuralDueCount > 0 && structuralDueMicroseconds <= playbackMicroseconds
        ? structuralDueCount : 0;
    queue_length_ = max64(0, admittedPending + newlyDue + dueStructural);
    outstanding_events_ = queue_length_ + (inService ? 1 : 0);
    if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;

    int64_t oldest = kInt64Max;
    if (inService && inServiceDueMicroseconds != kInt64Max) oldest = min64(oldest, inServiceDueMicroseconds);
    if (admittedPending > 0) {
        if (admittedOldestEvent >= 0) oldest = min64(oldest, event_micros(admittedOldestEvent));
        if (admittedOldestDueMicroseconds != kInt64Max) oldest = min64(oldest, admittedOldestDueMicroseconds);
    }
    if (snapshot_eligible_.first_eligible() >= 0) oldest = min64(oldest, event_micros(snapshot_eligible_.first_eligible()));
    if (dueStructural > 0) oldest = min64(oldest, structuralDueMicroseconds);
    queue_age_microseconds_ = oldest == kInt64Max ? 0 : max64(0, models::wrap_sub(playbackMicroseconds, oldest));
    if (queue_age_microseconds_ > maximum_queue_age_microseconds_) maximum_queue_age_microseconds_ = queue_age_microseconds_;
}

int64_t EngineCore::published_in_service_due_microseconds()
{
    return published_in_service_event_ >= 0 && published_in_service_event_ < event_count()
        ? event_micros(published_in_service_event_) : kInt64Max;
}

int64_t EngineCore::count_snapshot_eligible(int32_t startInclusive, int32_t endExclusive)
{
    // v1's decisions and results; the counter no longer rescans the backlog when the start moves (DD-093).
    bool filtersActive = has_active_source_filters();
    int32_t routingGeneration = filtersActive ? channel_routing_.filter_generation() : 0;
    int32_t overrideGeneration = filtersActive ? channel_overrides_.filter_generation() : 0;
    return snapshot_eligible_.count(startInclusive, endExclusive, filtersActive, routingGeneration, overrideGeneration,
        [this](int32_t index) {
            if (passes_source_filters_quickly(index)) return true;
            EventView midiEvent = event_at(index);
            if (channel_routing_.should_filter(midiEvent)) return false;
            ChannelAttribute attribute;
            int32_t forcedValue;
            return !channel_overrides_.should_suppress(midiEvent, attribute, forcedValue);
        });
}

// ---------------------------------------------------------------- publication

void EngineCore::update_queue(int64_t queue, bool inService)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        queue_length_ = queue;
        outstanding_events_ = queue + (inService ? 1 : 0);
        if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
    }
    publish_channel_state(false);
}

void EngineCore::publish_structural_due_work(int32_t nextArrival, int64_t pending, int64_t oldestPendingDueMicroseconds,
    bool inService, int64_t inServiceDueMicroseconds, int64_t futureDueMicroseconds, int32_t futureDueCount)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        has_published_structural_due_work_ = true;
        published_structural_next_arrival_ = nextArrival;
        published_structural_pending_ = max64(0, pending);
        published_structural_oldest_due_microseconds_ = pending > 0 ? oldestPendingDueMicroseconds : kInt64Max;
        published_structural_in_service_ = inService;
        published_structural_in_service_due_microseconds_ = inService ? inServiceDueMicroseconds : kInt64Max;
        published_structural_future_due_microseconds_ = futureDueMicroseconds;
        published_structural_future_count_ = futureDueCount < 0 ? 0 : futureDueCount;
        queue_length_ = published_structural_pending_;
        outstanding_events_ = queue_length_ + (inService ? 1 : 0);
        if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
    }
    publish_channel_state(false);
}

// The first send of an immediate burst: publishes what v1 publishes before every send of such a burst (an event is in
// service, nothing else is waiting) with the engine lock, once. The event being sent is afterwards written with
// `burst_event_` alone (register entry DD-065); snapshot() and the end of the burst fold it into the published fields,
// so a reader sees the same values it saw when every event took the lock.
void EngineCore::begin_immediate_burst(int32_t nextSource, int32_t eventIndex)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        published_in_service_ = true;
        published_in_service_event_ = eventIndex;
        has_published_queue_ = true;
        if (mode_ == ProcessingMode::Queue) published_next_process_ = nextSource;
        else {
            published_drop_next_arrival_ = nextSource;
            published_drop_pending_ = 0;
            published_oldest_pending_event_ = -1;
        }
        queue_length_ = 0;
        outstanding_events_ = 1;
        if (maximum_queue_length_ < 1) maximum_queue_length_ = 1;
        burst_event_.store(eventIndex, std::memory_order_relaxed);
    }
    publish_channel_state(false);
}

// Called with the engine lock held. The burst always publishes "next = event + 1".
void EngineCore::settle_immediate_burst_locked()
{
    int32_t event = burst_event_.load(std::memory_order_relaxed);
    if (event < 0) return;
    published_in_service_event_ = event;
    if (mode_ == ProcessingMode::Queue) published_next_process_ = event + 1;
    else published_drop_next_arrival_ = event + 1;
}

void EngineCore::end_immediate_burst_locked()
{
    settle_immediate_burst_locked();
    burst_event_.store(-1, std::memory_order_relaxed);
}

void EngineCore::publish_drop_queue(int32_t nextArrival, int32_t pending, bool inService)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        published_drop_next_arrival_ = nextArrival;
        published_drop_pending_ = pending < 0 ? 0 : pending;
        published_in_service_ = inService;
        if (!inService) published_in_service_event_ = -1;
        published_oldest_pending_event_ = -1;
        has_published_queue_ = true;
        queue_length_ = published_drop_pending_;
        outstanding_events_ = queue_length_ + (inService ? 1 : 0);
        if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
    }
    publish_channel_state(false);
}

void EngineCore::publish_drop_queue(int32_t nextArrival, const models::PendingMidiQueue& pending, bool inService,
    int64_t logicalNowMicroseconds)
{
    int32_t oldest = pending.count() == 0 ? -1 : pending.oldest_event_index();
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        published_drop_next_arrival_ = nextArrival;
        published_drop_pending_ = pending.count();
        published_in_service_ = inService;
        if (!inService) published_in_service_event_ = -1;
        published_oldest_pending_event_ = oldest;
        has_published_queue_ = true;
        queue_length_ = pending.count();
        outstanding_events_ = queue_length_ + (inService ? 1 : 0);
        if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
        queue_age_microseconds_ = oldest < 0 ? 0 : max64(0, models::wrap_sub(logicalNowMicroseconds, event_micros(oldest)));
        if (queue_age_microseconds_ > maximum_queue_age_microseconds_) maximum_queue_age_microseconds_ = queue_age_microseconds_;
    }
    publish_channel_state(false);
}

bool EngineCore::queue_age_exceeded(const models::PendingMidiQueue& pending, int64_t logicalNowMicroseconds,
    int64_t limit)
{
    if (pending.count() == 0) return false;
    return models::wrap_sub(logicalNowMicroseconds, event_micros(pending.oldest_event_index())) > limit;
}

void EngineCore::publish_unlimited_queue(int32_t nextProcess, int64_t eligiblePending, bool inService)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    published_next_process_ = nextProcess;
    published_in_service_ = inService;
    if (!inService) published_in_service_event_ = -1;
    has_published_queue_ = true;
    queue_length_ = max64(0, eligiblePending);
    outstanding_events_ = queue_length_ + (inService ? 1 : 0);
    maximum_queue_length_ = max64(maximum_queue_length_, outstanding_events_);
}

void EngineCore::set_current_lag(int64_t lag)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    current_lag_microseconds_ = lag;
}

void EngineCore::record_channel_drop(int32_t eventIndex)
{
    if (channel_monitoring_enabled_.load() == 0) return;
    channel_state_.record_dropped(event_at(eventIndex));
    publish_channel_state(false);
}

void EngineCore::publish_channel_state(bool force)
{
    if (channel_monitoring_enabled_.load() == 0) return;
    publish_channel_state_at(now_stamp(), force);
}

void EngineCore::publish_channel_state_at(int64_t now, bool force)
{
    if (channel_monitoring_enabled_.load() == 0) return;
    int64_t previous = last_channel_publish_stamp_.load();
    if (!force && previous != 0 && models::wrap_sub(now, previous) < time_.frequency() / 60) return;
    last_channel_publish_stamp_.exchange(now);
    std::shared_ptr<ChannelPlaybackSnapshot> snapshot = channel_state_.create_snapshot();
    channel_overrides_.apply_to_snapshot(*snapshot);
    channel_routing_.apply_to_snapshot(*snapshot);
    std::atomic_store(&published_channel_state_, std::shared_ptr<const ChannelPlaybackSnapshot>(snapshot));
}

void EngineCore::publish_channel_state_before_wait(int64_t targetTicks, int64_t nowTicks)
{
    if (channel_monitoring_enabled_.load() == 0) return;
    // Publish a completed short burst before sleeping for a sparse gap.
    if (targetTicks > models::wrap_add(nowTicks, time_.frequency() / 60)) publish_channel_state(true);
    else publish_channel_state(false);
}

// ---------------------------------------------------------------- output sends

void EngineCore::set_behind_yield_microseconds(int64_t microseconds)
{
    behind_last_stamp_.store(0, std::memory_order_relaxed);
    behind_yield_ticks_.store(microseconds > 0 ? std::max<int64_t>(1, microseconds * clock_.frequency() / 1000000) : 0,
        std::memory_order_relaxed);
}

void EngineCore::yield_if_behind_slow()
{
    if ((behind_send_count_.fetch_add(1, std::memory_order_relaxed) & 63u) != 63u) return;
    // Behind: sending for a while without a wait. Give the other threads a turn (once per interval).
    int64_t now = clock_.now_ticks();
    int64_t last = behind_last_stamp_.load(std::memory_order_relaxed);
    if (last == 0) behind_last_stamp_.store(now, std::memory_order_relaxed);
    else if (now - last >= behind_yield_ticks_.load(std::memory_order_relaxed)) {
        services_.yield_to_others();
        behind_last_stamp_.store(clock_.now_ticks(), std::memory_order_relaxed);
    }
}

void EngineCore::send_to_output(platform::MidiOutput& output, const EventView& midiEvent)
{
    if (send_observer_) send_observer_(midiEvent);
    yield_if_behind();
    if (midiEvent.kind == EventKind::SystemExclusive) {
        // v1 IMidiOutput.Send with a SysEx event: the backend assembles fragments.
        std::vector<uint8_t> bytes(static_cast<size_t>(midiEvent.data_length > 0 ? midiEvent.data_length : 0));
        const EventStore* store = payload_store_;
        if (midiEvent.data_length > 3 && store != nullptr) {
            const EventRecord& record = store->records[midiEvent.source_index];
            if (record.payload_offset >= 0)
                store->payloads.copy_to(record.payload_offset, bytes.data(), bytes.size());
        }
        else {
            for (int i = 0; i < midiEvent.data_length && i < 3; ++i) bytes[static_cast<size_t>(i)] = midiEvent.bytes[i];
        }
        output::SystemExclusiveEvent fragment;
        fragment.status = midiEvent.status();
        fragment.data = bytes.empty() ? nullptr : bytes.data();
        fragment.length = bytes.size();
        fragment.event_index = midiEvent.event_index();
        fragment.tick = midiEvent.tick;
        fragment.micros = midiEvent.micros;
        check_output(output.send_long(fragment));
        return;
    }
    // The v2 output contract leaves the v1 DataLength == 0 early return to the caller.
    if (midiEvent.data_length == 0) return;
    uint32_t packed = static_cast<uint32_t>(midiEvent.routed_channel_plus_one != 0 ? midiEvent.status() : midiEvent.bytes[0]) |
        (static_cast<uint32_t>(midiEvent.bytes[1]) << 8) | (static_cast<uint32_t>(midiEvent.bytes[2]) << 16);
    check_output(output.send_short(packed));
}

bool EngineCore::send_scheduled_event(const EventView& midiEvent, bool trackChannels)
{
    // The callers of this form do not use the transport time around the send, so it is not read (each read locks
    // the engine and reads the clock; register entry DD-011).
    bool pacedHold = begin_output_paced_hold();
    try {
        send_to_output(*output_, midiEvent);
    }
    catch (...) {
        end_output_paced_hold(pacedHold);
        throw;
    }
    end_output_paced_hold(pacedHold);
    if (active_output_rate_gate_.load() != nullptr) record_output_rate_dispatch(midiEvent, ticks_to_micros(now_stamp()), false);
    if (trackChannels) channel_state_.record_successful(midiEvent);
    return true;
}

bool EngineCore::send_scheduled_event(const EventView& midiEvent, bool trackChannels, int64_t& providerSendStart,
    int64_t& providerSendEnd)
{
    providerSendStart = current_transport_ticks();
    bool pacedHold = begin_output_paced_hold();
    try {
        send_to_output(*output_, midiEvent);
    }
    catch (...) {
        end_output_paced_hold(pacedHold);
        throw;
    }
    end_output_paced_hold(pacedHold);
    providerSendEnd = current_transport_ticks();
    if (active_output_rate_gate_.load() != nullptr) record_output_rate_dispatch(midiEvent, ticks_to_micros(now_stamp()), false);
    if (trackChannels) channel_state_.record_successful(midiEvent);
    return true;
}

void EngineCore::send_protected_event(platform::MidiOutput& output, const EventView& midiEvent)
{
    bool pacedHold = begin_output_paced_hold();
    try {
        send_to_output(output, midiEvent);
    }
    catch (...) {
        end_output_paced_hold(pacedHold);
        throw;
    }
    end_output_paced_hold(pacedHold);
    if (active_output_rate_gate_.load() != nullptr) {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        output_rate_safety_bypass_messages_++;
    }
}

void EngineCore::send_rate_counted_essential_event(platform::MidiOutput& output, const EventView& midiEvent)
{
    bool pacedHold = begin_output_paced_hold();
    try {
        send_to_output(output, midiEvent);
    }
    catch (...) {
        end_output_paced_hold(pacedHold);
        throw;
    }
    end_output_paced_hold(pacedHold);
    models::OutputRateGate* gate = active_output_rate_gate_.load();
    if (gate != nullptr && gate->record_exceptional_dispatch(ticks_to_micros(now_stamp()))) {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        output_rate_safety_soft_excess_++;
    }
}

void EngineCore::record_output_rate_dispatch(const EventView&, int64_t dispatchTimestamp, bool protectedTraffic)
{
    models::OutputRateGate* gate = active_output_rate_gate_.load();
    if (gate == nullptr || !gate->record_dispatch(dispatchTimestamp, protectedTraffic)) return;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    output_rate_safety_soft_excess_++;
}

// ---------------------------------------------------------------- pending work at boundaries

void EngineCore::apply_pending_overrides()
{
    if (!channel_overrides_.any_overrides() || output_ == nullptr) return;
    bool trackChannels = channel_monitoring_enabled_.load() != 0;
    bool applied = false;
    for (int channel = 0; channel < 16; ++channel) {
        int32_t mask = channel_overrides_.take_pending_mask(channel);
        if (!channel_routing_.is_enabled(channel)) {
            channel_overrides_.requeue(channel, mask);
            continue;
        }
        while (mask != 0) {
            int32_t bit = mask & -mask;
            int attributeIndex = 0;
            int32_t scan = bit;
            while ((scan >>= 1) != 0) attributeIndex++;
            ChannelAttribute attribute = static_cast<ChannelAttribute>(attributeIndex);
            int32_t value = channel_overrides_.get_value(channel, attribute);
            mask &= ~bit;
            if (value == ChannelOverrideState::kAutoValue) continue;
            try {
                // v1: SendRateCountedEssentialEvent(MidiEvent) - implicit view, event index 0.
                EventView forcedMessage = ChannelOverrideState::create_message(channel, attribute, value, 0);
                send_rate_counted_essential_event(forcedMessage);
                if (trackChannels) {
                    channel_state_.record_override_applied(channel, attribute, value);
                    // Preserve the wire value as historical unless it is still the forced value.
                    if (channel_overrides_.get_value(channel, attribute) != value)
                        channel_state_.mark_attribute_historical_direct(channel, attribute);
                }
                applied = true;
            }
            catch (...) {
                channel_overrides_.requeue(channel, mask | bit);
                throw;
            }
        }
    }
    if (applied) publish_channel_state(true);
}

std::vector<EventView> EngineCore::chase_messages(const PlaybackSong& song, int32_t eventIndexExclusive,
    int64_t micros) const
{
    std::vector<EventView> result;
    if (song.chase_index == nullptr) return result;
    ChannelRouting routing = channel_routing_.routing_snapshot();
    ChannelOverrideValues overrides = channel_overrides_.values_snapshot();
    std::vector<ChaseMessage> messages = song.chase_index->messages(eventIndexExclusive, &routing, &overrides);
    result.reserve(messages.size());
    for (const ChaseMessage& m : messages) {
        // v1 MidiEventView.FromEvent(message, -1) over a generated MidiEvent.
        result.push_back(EventView::create(0, micros, 0, m.kind, m.bytes[0] & 0x0F, m.bytes[0], -1, m.bytes[1],
            m.length > 2 ? m.bytes[2] : 0, m.length));
    }
    return result;
}

void EngineCore::apply_pending_state_chase()
{
    int32_t eventIndexExclusive = pending_state_chase_event_index_.exchange(-1);
    if (eventIndexExclusive < 0 || chase_midi_state_on_play_seek_.load() == 0) return;
    std::shared_ptr<const PlaybackSong> song;
    platform::MidiOutput* output;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        song = song_;
        output = output_;
    }
    if (!song || output == nullptr) return;
    std::vector<EventView> messages = chase_messages(*song, eventIndexExclusive, 0);
    bool trackChannels = channel_monitoring_enabled_.load() != 0;
    for (const EventView& message : messages) {
        if (!is_active()) return;
        send_rate_counted_essential_event(*output, message);
        if (trackChannels) channel_state_.record_successful(message);
    }
    if (!messages.empty()) publish_channel_state(true);
}

// ---------------------------------------------------------------- source filters

bool EngineCore::has_active_source_filters() const
{
    return channel_routing_.any_disabled() || channel_overrides_.any_overrides();
}

bool EngineCore::mark_source_filtered(int32_t eventIndex, FilteredSourceEvents& filtered)
{
    if (filtered.contains(eventIndex)) return true;
    return mark_new_source_filtered(eventIndex, filtered);
}

// An exact shortcut of the two source filters (DD-093): a stored event whose channel is neither disabled nor has an
// override (or that has no channel) passes both, which its 24-byte record shows without building an event view. False
// means "decide it the full way".
bool EngineCore::passes_source_filters_quickly(int32_t eventIndex) const
{
    if (!events_.is_compact()) return false;
    int channel = events_.store->records.packed_at(eventIndex).channel();
    if (channel < 0 || channel >= 16) return true;
    return (channel_routing_.disabled_mask() & (1 << channel)) == 0 && channel_overrides_.status_mask(channel) == 0;
}

bool EngineCore::mark_new_source_filtered(int32_t eventIndex, FilteredSourceEvents& filtered)
{
    if (passes_source_filters_quickly(eventIndex)) return false;
    EventView midiEvent = event_at(eventIndex);
    if (channel_routing_.should_filter(midiEvent)) {
        filtered.add(eventIndex);
        channel_routing_.record_filtered(midiEvent.channel());
        uses_pre_admission_filter_accounting_.store(1);
        return true;
    }
    ChannelAttribute attribute;
    int32_t forcedValue;
    if (channel_overrides_.should_suppress(midiEvent, attribute, forcedValue)) {
        filtered.add(eventIndex);
        channel_overrides_.record_suppressed(midiEvent.channel());
        uses_pre_admission_filter_accounting_.store(1);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- service clock

int64_t EngineCore::next_playback_service_microseconds(models::ServiceDurationClock& clock,
    const EventView& midiEvent)
{
    // Source filtering and finite-queue bookkeeping must not reactivate the
    // remembered service model while Rate is None.
    return simulate_slowdown_.load() == 0 ? 0 : clock.next_microseconds(midiEvent);
}

void EngineCore::update_service_clock(models::ServiceDurationClock& clock,
    std::shared_ptr<const ServiceDurationSettings>& current)
{
    std::shared_ptr<const ServiceDurationSettings> latest = settings();
    if (latest == current) return;
    current = latest;
    // A new model starts with a fresh fractional phase at the next service start.
    clock = latest->create_clock();
}

// ---------------------------------------------------------------- waits

bool EngineCore::wait_while_paused()
{
    while (true) {
        PlaybackState state;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            state = state_;
        }
        if (state == PlaybackState::Playing) return true;
        if (state != PlaybackState::Paused) return false;
        acknowledge_preserved_pause();
        int32_t wakeGeneration = wake_generation();
        apply_pending_channel_controls();
        wake_->reset();
        if (wakeGeneration != wake_generation()) continue;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            state = state_;
        }
        if (state == PlaybackState::Paused) wake_->wait(-1);
    }
}

void EngineCore::acknowledge_preserved_pause()
{
    bool signal = false;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (state_ == PlaybackState::Paused && preserve_playback_queue_on_pause_seek() &&
            pause_acknowledged_generation_ != pause_request_generation_) {
            pause_acknowledged_generation_ = pause_request_generation_;
            signal = true;
        }
    }
    if (signal) pause_acknowledged_->set();
}

void EngineCore::signal_wake()
{
    wake_generation_.fetch_add(1u);
    wake_->set();
}

bool EngineCore::wait_microseconds(platform::Waiter& waiter, int64_t microseconds)
{
    // Waiting gives every other thread the processor, so the "behind" stretch starts again after this wait.
    if (behind_yield_ticks_.load(std::memory_order_relaxed) != 0) behind_last_stamp_.store(clock_.now_ticks(), std::memory_order_relaxed);
    platform::PlatformError error;
    platform::WaitResult result = waiter.wait(microseconds, error);
    if (result == platform::WaitResult::Failed) raise(error.type.c_str(), error.message);
    return result == platform::WaitResult::Elapsed;
}

void EngineCore::wait_until(platform::Waiter& waiter, int64_t targetTicks, int32_t iterationWakeGeneration)
{
    while (is_active()) {
        wake_->reset();
        if (iterationWakeGeneration != wake_generation()) return;
        if (!is_playing()) return;
        int64_t remainingTicks = models::wrap_sub(targetTicks, current_transport_ticks());
        if (remainingTicks <= 0) return;
        int64_t remainingUs = max64(1, ticks_to_micros(remainingTicks));
        if (!wait_microseconds(waiter, remainingUs)) return;

        // The waitable timer deliberately wakes about 200 us early.
        waiter.begin_spin(); // v1: SpinWait spin = new SpinWait();
        while (targetTicks > current_transport_ticks() && is_playing()) {
            if (wake_->poll()) return;
            waiter.spin_once();
        }
        return;
    }
}

void EngineCore::wait_until_stopwatch(platform::Waiter& waiter, int64_t targetStamp, int32_t wakeGeneration)
{
    while (is_active()) {
        wake_->reset();
        if (wakeGeneration != this->wake_generation() || !is_playing()) return;
        int64_t remaining = models::wrap_sub(targetStamp, now_stamp());
        if (remaining <= 0) return;
        int64_t remainingUs = max64(1, ticks_to_micros(remaining));
        if (!wait_microseconds(waiter, remainingUs)) return;
        waiter.begin_spin(); // v1: SpinWait spin = new SpinWait();
        while (targetStamp > now_stamp() && is_playing()) {
            if (wake_->poll()) return;
            waiter.spin_once();
        }
        return;
    }
}

bool EngineCore::is_active()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return state_ == PlaybackState::Playing || state_ == PlaybackState::Paused;
}

bool EngineCore::is_playing()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return state_ == PlaybackState::Playing;
}

bool EngineCore::should_retire_worker_at_dispatch_boundary()
{
    if (dispatch_suspended_.load() == 0 && is_playing()) return false;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return !(state_ == PlaybackState::Paused && preserve_playback_queue_on_pause_seek());
}

// ---------------------------------------------------------------- transport time

int64_t EngineCore::current_transport_ticks()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return current_transport_ticks_locked();
}

int64_t EngineCore::current_transport_ticks_locked()
{
    if (state_ == PlaybackState::Playing) {
        if (output_paced_hold_depth_ > 0) return output_paced_hold_transport_ticks_;
        return models::wrap_sub(models::wrap_add(transport_base_ticks_, now_stamp()), run_start_stamp_);
    }
    return transport_base_ticks_;
}

void EngineCore::freeze_transport_locked()
{
    transport_base_ticks_ = current_transport_ticks_locked();
    run_start_stamp_ = now_stamp();
    output_paced_hold_depth_ = 0;
    output_paced_hold_start_stamp_ = 0;
    output_paced_hold_transport_ticks_ = 0;
}

bool EngineCore::begin_output_paced_hold()
{
    if (output_paced_timeline_.load() == 0) return false;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    if (state_ != PlaybackState::Playing) return false;
    if (output_paced_hold_depth_ == 0) {
        int64_t now = now_stamp();
        output_paced_hold_transport_ticks_ = models::wrap_sub(models::wrap_add(transport_base_ticks_, now), run_start_stamp_);
        output_paced_hold_start_stamp_ = now;
    }
    output_paced_hold_depth_++;
    return true;
}

void EngineCore::end_output_paced_hold(bool entered)
{
    if (!entered) return;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    if (output_paced_hold_depth_ <= 0) return;
    output_paced_hold_depth_--;
    if (output_paced_hold_depth_ != 0) return;
    if (state_ == PlaybackState::Playing)
        run_start_stamp_ = saturating_add_ticks(run_start_stamp_,
            max64(0, models::wrap_sub(now_stamp(), output_paced_hold_start_stamp_)));
    output_paced_hold_start_stamp_ = 0;
    output_paced_hold_transport_ticks_ = 0;
}

void EngineCore::start_output_paced_service_hold(int64_t serviceTicks, bool& entered, int64_t& completionStamp)
{
    entered = begin_output_paced_hold();
    completionStamp = entered ? saturating_add_ticks(now_stamp(), max64(0, serviceTicks)) : kInt64Max;
}

void EngineCore::reset_statistics_locked()
{
    queue_length_ = 0;
    outstanding_events_ = 0;
    maximum_queue_length_ = 0;
    queue_age_microseconds_ = 0;
    maximum_queue_age_microseconds_ = 0;
    processed_events_ = 0;
    dropped_events_ = 0;
    gate_filtered_events_ = 0;
    musical_limit_rejected_attacks_ = 0;
    musical_limit_suppressed_releases_ = 0;
    output_rate_rejected_attacks_ = 0;
    output_rate_suppressed_releases_ = 0;
    output_rate_rejected_other_messages_ = 0;
    output_rate_safety_soft_excess_ = 0;
    output_rate_safety_bypass_messages_ = 0;
    virtual_queue_length_ = 0;
    virtual_outstanding_events_ = 0;
    virtual_maximum_queue_length_ = 0;
    last_dispatched_microseconds_ = 0;
    effective_speed_frontier_microseconds_ = 0;
    current_lag_microseconds_ = 0;
    maximum_lag_microseconds_ = 0;
    snapshot_eligible_.reset();
    published_oldest_pending_event_ = -1;
}

int64_t EngineCore::clamp_position(const PlaybackSong& song, int64_t microseconds)
{
    if (microseconds < 0) return 0;
    if (microseconds > song.duration_us) return song.duration_us;
    return microseconds;
}

int32_t EngineCore::find_first_event_after_transport(int32_t startIndex, int64_t transportTicks) const
{
    int32_t low = startIndex < 0 ? 0 : startIndex;
    int32_t high = event_count();
    int64_t latestDue;
    if (time_.latest_microseconds_due(transportTicks, latestDue)) {
        // Compare the stored times with one threshold (no conversion per probe), and look near the start first: the
        // answer is usually a few events ahead of where the last search ended (register entry DD-066).
        int32_t probe = low;
        int32_t step = 1;
        while (probe < high) {
            if (event_micros(probe) > latestDue) { high = probe; break; }
            low = probe + 1;
            if (high - probe <= step) break;
            probe += step;
            step <<= 1;
        }
        while (low < high) {
            int32_t middle = low + ((high - low) / 2);
            if (event_micros(middle) <= latestDue) low = middle + 1;
            else high = middle;
        }
        return low;
    }
    while (low < high) {
        int32_t middle = low + ((high - low) / 2);
        if (event_ticks(middle) <= transportTicks) low = middle + 1;
        else high = middle;
    }
    return low;
}

} // namespace engine
} // namespace midibn
