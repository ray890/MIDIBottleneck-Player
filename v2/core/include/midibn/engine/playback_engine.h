// The playback engine (Milestone 5, Group D): the port of v1 PlaybackEngine.cs and
// PlaybackEngine.Ordered.cs (v1.0.44), with the improvements of the register
// (v2/docs/DIFFERENCES_FROM_V1.md); every other behaviour is v1's.
//
// Platform access goes only through platform::EngineServices (clock, the two
// wake events, the waiter, worker threads) and the platform::MidiOutput the
// caller passes to start(). Nothing blocks except the engine mutex and those
// objects. v1 `lock (_sync)` is one engine mutex (recursive, because v1's
// Monitor is re-entered on the same thread, e.g. SetChannelOverride sending
// under the lock); v1 Volatile/Interlocked fields are std::atomic.
//
// v1 throws .NET exceptions from its public members; each corresponding
// member here returns an EngineError carrying v1's exception type name and
// Message (ok() when v1 does not throw). The engine's events (v1
// PlaybackEnded, PlaybackFailed, ChannelControlFailed) are callbacks invoked
// on the thread v1 raises them on (the worker thread).
//
// Public member map, v1 PlaybackEngine -> v2 PlaybackEngine:
//   DefaultQueueLengthLimit, DefaultQueueAgeLimitMicroseconds,
//   DefaultDropBufferCapacity, DefaultIntervalPolyphonyLimit
//                                        -> kDefault... constants
//   PlaybackEnded / PlaybackFailed / ChannelControlFailed (events)
//                                        -> set_playback_ended_handler / set_playback_failed_handler /
//                                           set_channel_control_failed_handler
//   State                                -> state()
//   ProcessingMicroseconds {get; set}    -> processing_microseconds() / set_processing_microseconds()
//   ServiceDurationMode {get; set}       -> service_duration_mode() / set_service_duration_mode()
//   MidiBitrate {get; set}               -> midi_bitrate() / set_midi_bitrate()
//   EventsPerSecond {get; set}           -> events_per_second() / set_events_per_second()
//   SetServiceConfiguration(...)         -> set_service_configuration(...)
//   SimulateSlowdown {get; set}          -> simulate_slowdown() / set_simulate_slowdown()
//   ApplyQueueLimitWithoutSlowdown       -> apply_queue_limit_without_slowdown() / set_...()
//   ChaseMidiStateOnPlaySeek             -> chase_midi_state_on_play_seek() / set_...()
//   PreservePlaybackQueueOnPauseSeek     -> preserve_playback_queue_on_pause_seek() / set_...()
//   OutputPacedTimeline                  -> output_paced_timeline() / set_output_paced_timeline()
//   ScaleNoteIntervalByTraffic           -> scale_note_interval_by_traffic() / set_...()
//   IntervalPolyphonyLimitEnabled        -> interval_polyphony_limit_enabled() / set_...()
//   IntervalPolyphonyLimit               -> interval_polyphony_limit() / set_interval_polyphony_limit()
//   QueueLengthLimit                     -> queue_length_limit() / set_queue_length_limit()
//   QueueAgeLimitEnabled                 -> queue_age_limit_enabled() / set_queue_age_limit_enabled()
//   QueueAgeLimitMicroseconds            -> queue_age_limit_microseconds() / set_queue_age_limit_microseconds()
//   DropBufferCapacity                   -> drop_buffer_capacity() / set_drop_buffer_capacity()
//   OverflowPolicy                       -> overflow_policy() / set_overflow_policy()
//   Start(song, output, mode[, startMicroseconds[, startPaused]])
//                                        -> start(song, output, mode, startMicroseconds = 0, startPaused = false)
//   Seek(targetMicroseconds)             -> seek()
//   Pause() / Resume() / Stop()          -> pause() / resume() / stop()
//   WorkerStopTimeoutMilliseconds        -> worker_stop_timeout_milliseconds() / set_...()
//   HasLiveWorker                        -> has_live_worker()
//   Unload()                             -> unload()
//   HasAttachedSong / HasAttachedOutput  -> has_attached_song() / has_attached_output()
//   ResetStatistics()                    -> reset_statistics()
//   SetChannelMonitoring(enabled)        -> set_channel_monitoring()
//   ChaseStateForNewMonitor(completion)  -> chase_state_for_new_monitor()
//   GetChannelSnapshot()                 -> channel_snapshot()
//   GetChannelOverride(channel, attr)    -> channel_override()
//   SetChannelOverride(channel, attr, v) -> set_channel_override()
//   IsChannelEnabled(channel)            -> is_channel_enabled()
//   SetChannelEnabled(channel, enabled)  -> set_channel_enabled()
//   ChaseLatestSourceChannelAttribute(channel, attr[, completion])
//                                        -> chase_latest_source_channel_attribute()
//   SendExplicitChannelAttribute(channel, attr, value[, completion])
//                                        -> send_explicit_channel_attribute()
//   GetSnapshot()                        -> snapshot()
//   MicrosecondsToTicks / TicksToMicroseconds (static)
//                                        -> time().microseconds_to_ticks() / ticks_to_microseconds()
//   Dispose()                            -> dispose() (the destructor also disposes)
//   SetDropTrace, SetSourceDispatchTrace, SetGateBoundaryDispatchHookForTesting,
//   SetGateLogicalOutputTraceForTesting  -> not ported: v1 test/diagnostic hooks that MainForm never
//                                           sets; with them unset v1 takes exactly the paths ported here.
// Additions for the lock-step harness (no v1 equivalent, no behaviour):
//   wake_generation()                    -> v1 _wakeGeneration (the spike's release signal for yields)
//   set_send_observer()                  -> called on the sending thread right before every output send
//                                           with the event as v1 passes it to IMidiOutput.Send.
#pragma once

#include "midibn/engine/channel_state.h"
#include "midibn/engine/engine_types.h"
#include "midibn/engine/ordered_adapters.h"
#include "midibn/engine/snapshot_eligible_counter.h"
#include "midibn/midi_output.h"
#include "midibn/models/queue.h"
#include "midibn/platform.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace midibn {
namespace engine {

// v1 Action<Exception> completion: called with error.ok() on success.
using ChannelControlCompletion = std::function<void(const EngineError& error)>;

// The engine state and logic, shared between the owning PlaybackEngine and
// its worker threads (each worker body holds a shared_ptr to it), so a worker
// that v1 would abandon can never touch freed memory.
class EngineCore : public std::enable_shared_from_this<EngineCore> {
public:
    static constexpr int32_t kDefaultQueueLengthLimit = 2000;
    static constexpr int64_t kDefaultQueueAgeLimitMicroseconds = 1000000;
    static constexpr int32_t kDefaultDropBufferCapacity = kDefaultQueueLengthLimit;
    static constexpr int32_t kDefaultIntervalPolyphonyLimit = 128;

    explicit EngineCore(std::shared_ptr<platform::EngineServices> services);
    ~EngineCore();
    EngineCore(const EngineCore&) = delete;
    EngineCore& operator=(const EngineCore&) = delete;

    // ---- events
    void set_playback_ended_handler(std::function<void()> handler);
    void set_playback_failed_handler(std::function<void(const EngineError&)> handler);
    void set_channel_control_failed_handler(std::function<void(int, ChannelAttribute, const EngineError&)> handler);
    // Like the handler above and called together with it, and also says whether the failed request was an
    // enable / disable (mute) request, whose `attribute` argument is only a placeholder (v1 reports it as
    // Bank MSB; the Channels window shows that failure on the Ch cell, DD-033).
    void set_channel_control_failure_detail_handler(std::function<void(int, ChannelAttribute, bool, const EngineError&)> handler);
    // Owner teardown (PlaybackEngine's destructor, after dispose()): detaches
    // every worker still running (v1 abandons it) and drops the handlers.
    void abandon_workers();

    // ---- settings
    PlaybackState state();
    int64_t processing_microseconds() const { return settings()->processing_microseconds; }
    void set_processing_microseconds(int64_t value);
    ServiceDurationMode service_duration_mode() const { return settings()->mode; }
    void set_service_duration_mode(ServiceDurationMode value);
    int64_t midi_bitrate() const { return settings()->midi_bitrate; }
    void set_midi_bitrate(int64_t value);
    int64_t events_per_second() const { return settings()->events_per_second; }
    void set_events_per_second(int64_t value);
    void set_service_configuration(ServiceDurationMode mode, int64_t processingMicroseconds, int64_t midiBitrate,
        int64_t eventsPerSecond);
    bool simulate_slowdown() const { return simulate_slowdown_.load() != 0; }
    void set_simulate_slowdown(bool value);
    bool apply_queue_limit_without_slowdown() const { return apply_queue_limit_without_slowdown_.load() != 0; }
    void set_apply_queue_limit_without_slowdown(bool value) { apply_queue_limit_without_slowdown_.store(value ? 1 : 0); }
    bool chase_midi_state_on_play_seek() const { return chase_midi_state_on_play_seek_.load() != 0; }
    void set_chase_midi_state_on_play_seek(bool value);
    bool preserve_playback_queue_on_pause_seek() const { return preserve_playback_queue_on_pause_seek_.load() != 0; }
    void set_preserve_playback_queue_on_pause_seek(bool value) { preserve_playback_queue_on_pause_seek_.store(value ? 1 : 0); }
    bool output_paced_timeline() const { return output_paced_timeline_.load() != 0; }
    void set_output_paced_timeline(bool value) { output_paced_timeline_.store(value ? 1 : 0); }
    // Few-processor yield option: the "behind" interval in microseconds (0 = off); may change while playing.
    void set_behind_yield_microseconds(int64_t microseconds);
    bool scale_note_interval_by_traffic() const { return scale_note_interval_by_traffic_.load() != 0; }
    void set_scale_note_interval_by_traffic(bool value) { scale_note_interval_by_traffic_.store(value ? 1 : 0); }
    bool interval_polyphony_limit_enabled() const { return interval_polyphony_limit_enabled_.load() != 0; }
    void set_interval_polyphony_limit_enabled(bool value) { interval_polyphony_limit_enabled_.store(value ? 1 : 0); }
    int32_t interval_polyphony_limit() const { return interval_polyphony_limit_.load(); }
    void set_interval_polyphony_limit(int32_t value);
    int32_t queue_length_limit() const { return queue_length_limit_.load(); }
    void set_queue_length_limit(int32_t value);
    bool queue_age_limit_enabled() const { return queue_age_limit_enabled_.load() != 0; }
    void set_queue_age_limit_enabled(bool value);
    int64_t queue_age_limit_microseconds() const { return queue_age_limit_microseconds_.load(); }
    void set_queue_age_limit_microseconds(int64_t value);
    int32_t drop_buffer_capacity() const { return queue_length_limit(); }
    void set_drop_buffer_capacity(int32_t value) { set_queue_length_limit(value); }
    OverflowPolicy overflow_policy() const { return static_cast<OverflowPolicy>(overflow_policy_.load()); }
    void set_overflow_policy(OverflowPolicy value) { overflow_policy_.store(static_cast<int32_t>(value)); }
    int32_t worker_stop_timeout_milliseconds() const { return worker_stop_timeout_milliseconds_.load(); }
    void set_worker_stop_timeout_milliseconds(int32_t value) { worker_stop_timeout_milliseconds_.store(value < 1 ? 1 : value); }

    // ---- transport
    // The core shares ownership of the song and the output.
    // `continuation` (DD-007): the caller is restarting where playback just was (a setting changed, the output
    // changed), so the events at exactly `startMicroseconds` have already been sent and are not sent again.
    // False for a start the person chose (from the beginning, or a position they picked): events at the start
    // position play.
    EngineError start(std::shared_ptr<const PlaybackSong> song, std::shared_ptr<platform::MidiOutput> output, ProcessingMode mode,
        int64_t startMicroseconds = 0, bool startPaused = false, bool continuation = false);
    EngineError seek(int64_t targetMicroseconds);
    EngineError pause();
    void resume();
    EngineError stop();
    EngineError unload();
    EngineError dispose();
    bool has_live_worker();
    bool has_attached_song();
    bool has_attached_output();

    // ---- statistics and channels
    PlaybackSnapshot snapshot();
    void reset_statistics();
    void set_channel_monitoring(bool enabled);
    bool chase_state_for_new_monitor(ChannelControlCompletion completion);
    std::shared_ptr<const ChannelPlaybackSnapshot> channel_snapshot() const;
    // v1 GetChannelOverride / IsChannelEnabled throw for a channel outside
    // 0..15 (IndexOutOfRange / ArgumentOutOfRange); these return
    // ChannelOverrideState::kAutoValue / false instead (precondition: 0..15).
    int32_t channel_override(int channel, ChannelAttribute attribute) const;
    EngineError set_channel_override(int channel, ChannelAttribute attribute, int32_t value);
    bool is_channel_enabled(int channel) const;
    EngineError set_channel_enabled(int channel, bool enabled);
    EngineError chase_latest_source_channel_attribute(int channel, ChannelAttribute attribute,
        ChannelControlCompletion completion = ChannelControlCompletion());
    EngineError send_explicit_channel_attribute(int channel, ChannelAttribute attribute, int32_t value,
        ChannelControlCompletion completion = ChannelControlCompletion());

    // ---- time and harness support
    const TimeConversion& time() const { return time_; }
    int32_t wake_generation() const { return static_cast<int32_t>(wake_generation_.load()); }
    void set_send_observer(std::function<void(const models::EventView&)> observer) { send_observer_ = std::move(observer); }

private:
    enum class ChannelTransitionBoundary { Pause, Seek, Stop };
    enum class ChannelControlKind { SetEnabled, Chase, MonitorStateChase };

    struct PreservedSeekRequest {
        int32_t generation;
        int64_t target_microseconds;
        int32_t target_event_index;
    };

    struct ChannelControlRequest {
        ChannelControlKind kind = ChannelControlKind::SetEnabled;
        int channel = -1;
        ChannelAttribute attribute = ChannelAttribute::BankMsb;
        int32_t value = 0;
        std::shared_ptr<const PlaybackSong> expected_song;
        ChannelControlCompletion completion;
        std::atomic<int32_t> completed{ 0 };
        bool has_completion() const { return static_cast<bool>(completion); }
        void complete(const EngineError& error);
    };
    using ControlRequestPtr = std::shared_ptr<ChannelControlRequest>;

    // A captured exception being propagated (see capture()).
    template <typename Body> static bool capture(Body&& body, EngineError& error);
    static EngineError from_model(const models::ModelException& exception);
    [[noreturn]] static void raise(const char* type, const std::string& message);
    [[noreturn]] static void raise(const EngineError& error) { throw error; }
    static void check_output(const platform::OutputStatus& status);
    static EngineError output_error(const platform::OutputStatus& status);

    std::shared_ptr<const ServiceDurationSettings> settings() const { return std::atomic_load(&service_settings_); }
    void publish_settings(std::shared_ptr<const ServiceDurationSettings> next)
    {
        std::atomic_store(&service_settings_, next);
        settings_generation_.fetch_add(1);
    }
    // Changes with every publish_settings: lets the hot loop notice a new settings object with one atomic read
    // instead of loading the shared pointer for every event.
    uint32_t settings_generation() const { return settings_generation_.load(); }
    int64_t now_stamp() const { return clock_.now_ticks(); }
    int64_t micros_to_ticks(int64_t us) const { return time_.microseconds_to_ticks(us); }
    int64_t ticks_to_micros(int64_t ticks) const { return time_.ticks_to_microseconds(ticks); }
    int64_t event_micros(int32_t index) const;
    models::EventView event_at(int32_t index) const { return events_.at(index); }
    int32_t event_count() const { return events_.count(); }

    // transport (PlaybackEngine.cs)
    void start_impl(std::shared_ptr<const PlaybackSong> song, std::shared_ptr<platform::MidiOutput> output, ProcessingMode mode,
        int64_t startMicroseconds, bool startPaused, bool continuation);
    void seek_impl(int64_t targetMicroseconds);
    void pause_impl();
    void stop_worker_for_transition(const char* operation, ChannelTransitionBoundary channelBoundary);
    void unload_impl();
    void start_worker_locked();
    void reap_finished_threads_locked();

    // channel controls
    void submit_channel_control(const ControlRequestPtr& request);
    void apply_pending_channel_controls();
    std::vector<ControlRequestPtr> drain_channel_control_requests_locked();
    static void complete_retired_channel_controls(const std::vector<ControlRequestPtr>& requests,
        const EngineError& error);
    void execute_channel_control(const ChannelControlRequest& request);

    // worker
    void playback_worker();
    bool has_preserved_seek_request();
    void dispatch_protected_all_notes_off();
    void run_queue_mode(platform::Waiter& waiter);
    void run_drop_mode(platform::Waiter& waiter);
    void run_virtual_forward_drop_mode(platform::Waiter& waiter);
    void record_virtual_drop(int32_t eventIndex);
    void publish_virtual_queue(const models::ForwardDropQueue& queue);
    void run_per_note_interval_gate(platform::Waiter& waiter);
    void run_delayed_note_off(platform::Waiter& waiter);
    void run_note_attack_limit(platform::Waiter& waiter);
    void run_output_rate_limit(platform::Waiter& waiter);
    void run_output_rate_limit_body(platform::Waiter& waiter, models::OutputRateGate& gate);
    void add_musical_limit_exclusions(int64_t rejectedAttacks, int64_t suppressedReleases);
    void add_output_rate_exclusions(int64_t rejectedAttacks, int64_t suppressedReleases, int64_t rejectedOtherMessages);
    void add_gate_filtered(int64_t count);
    void advance_gate_effective_speed_frontier(int64_t sourceMicroseconds);
    void dispatch(int32_t eventIndex, int64_t actualTransportTicks);
    void dispatch(int32_t eventIndex, int64_t actualTransportTicks, int64_t& providerSendStart, int64_t& providerSendEnd);
    void dispatch_midi_event(const models::EventView& midiEvent);
    void dispatch_midi_event(const models::EventView& midiEvent, int64_t& providerSendStart, int64_t& providerSendEnd);
    void dispatch_immediate_range(int32_t& nextIndex, int32_t endExclusive, int64_t transportAtBatchStart,
        const std::shared_ptr<const ServiceDurationSettings>& serviceSettings);

    class FilteredSourceEvents;
    void dispatch_immediate_filtered_range(int32_t& nextIndex, int32_t endExclusive, int64_t transportAtBatchStart,
        FilteredSourceEvents& filtered, int64_t& eligiblePending,
        const std::shared_ptr<const ServiceDurationSettings>& serviceSettings, bool classifySourceAtDispatch = false);
    // The loop behind both (register entry DD-065): `filtered` and `eligiblePending` are null for the unfiltered form.
    void dispatch_immediate_burst(int32_t& nextIndex, int32_t endExclusive, int64_t transportAtBatchStart,
        FilteredSourceEvents* filtered, int64_t* eligiblePending,
        const std::shared_ptr<const ServiceDurationSettings>& serviceSettings, bool classifySourceAtDispatch);
    // `sent`: when not null (channel monitoring on), receives what the channel monitor records for this send (DD-092).
    int64_t send_burst_event(int32_t eventIndex, bool direct, ChannelStateTracker::SentRecord* sent);
    // Filled by MidiOutput::try_send_short only when a send fails (so a send that works builds no status and no
    // string); used by the sending thread only, never read as a result unless the call returned false.
    platform::OutputStatus send_failure_;
    void begin_immediate_burst(int32_t nextSource, int32_t eventIndex);
    void end_immediate_burst_locked();
    void settle_immediate_burst_locked();
    void update_queue(int64_t queue, bool inService);
    void publish_structural_due_work(int32_t nextArrival, int64_t pending, int64_t oldestPendingDueMicroseconds,
        bool inService, int64_t inServiceDueMicroseconds, int64_t futureDueMicroseconds, int32_t futureDueCount);
    void publish_drop_queue(int32_t nextArrival, int32_t pending, bool inService);
    void publish_drop_queue(int32_t nextArrival, const models::PendingMidiQueue& pending, bool inService,
        int64_t logicalNowMicroseconds);
    bool queue_age_exceeded(const models::PendingMidiQueue& pending, int64_t logicalNowMicroseconds, int64_t limit);
    void measure_published_due_work(int32_t nextSource, int64_t admittedPending, int32_t admittedOldestEvent,
        int64_t admittedOldestDueMicroseconds, bool inService, int64_t inServiceDueMicroseconds,
        int64_t structuralDueMicroseconds, int32_t structuralDueCount, int64_t playbackTicks);
    int64_t published_in_service_due_microseconds();
    int64_t count_snapshot_eligible(int32_t startInclusive, int32_t endExclusive);
    void record_channel_drop(int32_t eventIndex);
    void publish_channel_state(bool force);
    // The same with a clock reading the caller already has (`nowStamp`, from now_stamp()).
    void publish_channel_state_at(int64_t nowStamp, bool force);
    void send_to_output(platform::MidiOutput& output, const models::EventView& midiEvent);
    // P10-THR: call before each send; does nothing unless the few-processor yield option is on (behind_yield_ticks_ != 0).
    void yield_if_behind() { if (behind_yield_ticks_.load(std::memory_order_relaxed) != 0) yield_if_behind_slow(); }
    void yield_if_behind_slow();
    bool send_scheduled_event(const models::EventView& midiEvent, bool trackChannels);
    bool send_scheduled_event(const models::EventView& midiEvent, bool trackChannels, int64_t& providerSendStart,
        int64_t& providerSendEnd);
    void send_protected_event(const models::EventView& midiEvent) { send_protected_event(*output_, midiEvent); }
    void send_protected_event(platform::MidiOutput& output, const models::EventView& midiEvent);
    void send_rate_counted_essential_event(platform::MidiOutput& output, const models::EventView& midiEvent);
    void send_rate_counted_essential_event(const models::EventView& midiEvent)
    { send_rate_counted_essential_event(*output_, midiEvent); }
    void record_output_rate_dispatch(const models::EventView& midiEvent, int64_t dispatchTimestamp, bool protectedTraffic);
    void retire_drop_filtered_backlog(FilteredSourceEvents& filtered, models::PendingMidiQueue& pending,
        models::CompleteNoteTracker& completeNotes, int32_t& inService, int64_t& completionTicks, int64_t now,
        models::ServiceDurationClock& serviceClock, models::ServiceDurationClock& beforeCurrentService);
    bool has_active_source_filters() const;
    bool passes_source_filters_quickly(int32_t eventIndex) const;
    bool mark_source_filtered(int32_t eventIndex, FilteredSourceEvents& filtered);
    bool mark_new_source_filtered(int32_t eventIndex, FilteredSourceEvents& filtered);
    void admit_unlimited_range(int32_t startInclusive, int32_t endExclusive, FilteredSourceEvents& filtered,
        int64_t& eligiblePending);
    void retire_unlimited_filtered_backlog(int32_t nextProcess, int32_t nextArrival, FilteredSourceEvents& filtered,
        int64_t& eligiblePending, int32_t& inService, int64_t& completionTicks, int64_t& lastCompletionTicks,
        int64_t now, models::ServiceDurationClock& serviceClock, models::ServiceDurationClock& beforeCurrentService);
    int32_t take_next_eligible(int32_t& nextProcess, int32_t nextArrival, FilteredSourceEvents& filtered);
    void apply_pending_overrides();
    void apply_pending_state_chase();
    void publish_channel_state_before_wait(int64_t targetTicks, int64_t nowTicks);
    void publish_unlimited_queue(int32_t nextProcess, int64_t eligiblePending, bool inService);
    void set_current_lag(int64_t lag);
    int64_t next_playback_service_microseconds(models::ServiceDurationClock& clock, const models::EventView& midiEvent);
    void update_service_clock(models::ServiceDurationClock& clock,
        std::shared_ptr<const ServiceDurationSettings>& settings);
    bool wait_while_paused();
    void acknowledge_preserved_pause();
    void signal_wake();
    bool wait_microseconds(platform::Waiter& waiter, int64_t microseconds);
    void wait_until(platform::Waiter& waiter, int64_t targetTicks, int32_t iterationWakeGeneration);
    bool is_active();
    bool is_playing();
    bool should_retire_worker_at_dispatch_boundary();
    int64_t current_transport_ticks();
    int64_t current_transport_ticks_locked();
    void freeze_transport_locked();
    bool begin_output_paced_hold();
    void end_output_paced_hold(bool entered);
    void start_output_paced_service_hold(int64_t serviceTicks, bool& entered, int64_t& completionStamp);
    int64_t event_ticks(int32_t eventIndex) const { return micros_to_ticks(event_micros(eventIndex)); }
    void reset_statistics_locked();
    static int64_t clamp_position(const PlaybackSong& song, int64_t microseconds);
    static int32_t find_first_event_at_or_after(const PlaybackSong& song, int64_t microseconds)
    { return song.events.lower_bound_by_time(microseconds); }
    // The first event strictly after `microseconds` (the events at that time have already been sent).
    static int32_t find_first_event_after(const PlaybackSong& song, int64_t microseconds)
    { return song.events.lower_bound_by_time(microseconds == models::kInt64Max ? microseconds : microseconds + 1); }
    int32_t find_first_event_after_transport(int32_t startIndex, int64_t transportTicks) const;
    std::vector<models::EventView> chase_messages(const PlaybackSong& song, int32_t eventIndexExclusive,
        int64_t micros) const;

    // ordered (PlaybackEngine.Ordered.cs)
    void run_ordered_unlimited_queue_mode(platform::Waiter& waiter);
    void run_ordered_finite_queue_mode(platform::Waiter& waiter);
    void run_ordered_virtual_forward_mode(platform::Waiter& waiter);
    void run_ordered_musical_gate_mode(platform::Waiter& waiter);
    void run_ordered_note_attack_mode(platform::Waiter& waiter);
    void admit_ordered_note_attack_due(OrderedNoteAttackAdapter& adapter, int64_t nowStamp);
    void append_ordered_note_attack_seeks(OrderedNoteAttackAdapter& adapter, int64_t nowStamp);
    void run_ordered_output_rate_mode(platform::Waiter& waiter);
    void run_ordered_output_rate_body(platform::Waiter& waiter, OrderedOutputRateAdapter& adapter);
    void admit_ordered_output_rate_due(OrderedOutputRateAdapter& adapter, int64_t nowStamp);
    void append_ordered_output_rate_seeks(OrderedOutputRateAdapter& adapter, int64_t nowStamp);
    void admit_ordered_gate_due(OrderedMusicalGateAdapter& adapter, int64_t nowStamp);
    void append_ordered_gate_seek_requests(OrderedMusicalGateAdapter& adapter, int64_t nowStamp);
    void publish_ordered_gate_pressure(OrderedMusicalGateAdapter& adapter, bool hasInService);
    bool has_pending_ordered_live_control_boundary();
    void apply_ordered_live_control_boundary();
    bool try_take_one_preserved_seek_request(PreservedSeekRequest& request);
    void publish_ordered_virtual_pressure(const OrderedVirtualForwardAdapter& adapter);
    void admit_ordered_finite_due(models::OrderedFiniteAdmissionCore& core, int64_t nowStamp);
    void append_ordered_finite_seek_requests(models::OrderedFiniteAdmissionCore& core, int64_t nowStamp);
    void repair_and_publish_ordered_finite_pressure(models::OrderedFiniteAdmissionCore& core, bool hasInService);
    models::OrderedFiniteCapturedDecision capture_ordered_finite_decision(const models::EventView& midiEvent);
    models::OrderedFiniteCapturedDecision preview_ordered_finite_decision(const models::EventView& midiEvent);
    void record_ordered_finite_drops(int32_t count);
    void adopt_ordered_seek_target(const PreservedSeekRequest& request);
    void admit_ordered_unlimited_due(OrderedUnlimitedQueueAdapter& adapter, int64_t nowStamp);
    void append_ordered_seek_requests(OrderedUnlimitedQueueAdapter& adapter, int64_t nowStamp);
    std::vector<models::EventView> capture_ordered_generation_prefix(int32_t targetEventIndex, int64_t targetMicroseconds);
    bool should_filter_ordered_source(int32_t sourceIndex, const models::EventView& midiEvent);
    void dispatch_ordered_work_item(const models::OutputWorkItem& item);
    void publish_ordered_unlimited_pressure(const OrderedUnlimitedQueueAdapter& adapter, bool inService);
    void wait_until_stopwatch(platform::Waiter& waiter, int64_t targetStamp, int32_t wakeGeneration);
    models::CaptureDecision capture_decision();
    models::CaptureDecision preview_decision();

    static constexpr int32_t kMaximumPendingPreservedSeeks = 256;

    std::shared_ptr<platform::EngineServices> services_owner_;
    platform::EngineServices& services_;
    platform::Clock& clock_;
    // P10-THR: yield to other threads while behind (0 = off). Ticks of clock_; the stamp is the last wait or yield.
    std::atomic<int64_t> behind_yield_ticks_{ 0 };
    std::atomic<int64_t> behind_last_stamp_{ 0 };
    std::atomic<uint32_t> behind_send_count_{ 0 };
    TimeConversion time_;
    mutable std::recursive_mutex sync_;
    std::unique_ptr<platform::WakeEvent> wake_;
    std::unique_ptr<platform::WakeEvent> pause_acknowledged_;
    std::atomic<uint32_t> wake_generation_{ 0 };
    std::shared_ptr<platform::Thread> thread_;           // v1 _thread
    std::vector<std::shared_ptr<platform::Thread>> threads_;  // every started worker not yet reaped
    const EventStore* payload_store_ = nullptr;          // SysEx payloads of the attached song
    std::shared_ptr<const PlaybackSong> song_;
    models::EventSource events_;
    platform::MidiOutput* output_ = nullptr;
    std::shared_ptr<platform::MidiOutput> output_owner_;  // keeps output_ alive for an abandoned worker
    std::shared_ptr<platform::Thread> unstarted_;         // v1 _thread whose Start failed
    ProcessingMode mode_ = ProcessingMode::Queue;
    int32_t start_event_index_ = 0;
    PlaybackState state_ = PlaybackState::Stopped;
    std::shared_ptr<const ServiceDurationSettings> service_settings_;
    std::atomic<uint32_t> settings_generation_{ 0 };
    std::atomic<int32_t> queue_length_limit_{ kDefaultQueueLengthLimit };
    std::atomic<int32_t> queue_age_limit_enabled_{ 0 };
    std::atomic<int64_t> queue_age_limit_microseconds_{ kDefaultQueueAgeLimitMicroseconds };
    std::atomic<int32_t> simulate_slowdown_{ 1 };
    std::atomic<int32_t> apply_queue_limit_without_slowdown_{ 0 };
    std::atomic<int32_t> chase_midi_state_on_play_seek_{ 1 };
    std::atomic<int32_t> preserve_playback_queue_on_pause_seek_{ 0 };
    std::atomic<int32_t> output_paced_timeline_{ 0 };
    std::atomic<int32_t> scale_note_interval_by_traffic_{ 0 };
    std::atomic<int32_t> interval_polyphony_limit_enabled_{ 0 };
    std::atomic<int32_t> interval_polyphony_limit_{ kDefaultIntervalPolyphonyLimit };
    std::atomic<int32_t> pending_state_chase_event_index_{ -1 };
    int32_t pause_request_generation_ = 0;
    int32_t pause_acknowledged_generation_ = 0;
    int32_t preserved_seek_generation_ = 0;
    std::deque<PreservedSeekRequest> preserved_seek_requests_;
    int64_t intended_timeline_override_microseconds_ = -1;
    bool virtual_forward_drop_active_ = false;
    std::atomic<int32_t> overflow_policy_{ 0 };
    int64_t transport_base_ticks_ = 0;
    int64_t run_start_stamp_ = 0;
    int32_t output_paced_hold_depth_ = 0;
    int64_t output_paced_hold_start_stamp_ = 0;
    int64_t output_paced_hold_transport_ticks_ = 0;
    std::atomic<int32_t> dispatch_suspended_{ 1 };
    // While an immediate burst runs (dispatch_immediate_burst): the event being sent, or -1. The worker stores it with
    // a relaxed write per event instead of taking the engine lock; snapshot() folds it into the published fields.
    std::atomic<int32_t> burst_event_{ -1 };
    bool silence_when_worker_exits_ = false;
    bool has_worker_exit_silence_failure_ = false;
    EngineError worker_exit_silence_failure_;
    std::atomic<int32_t> worker_stop_timeout_milliseconds_{ 2000 };

    int64_t queue_length_ = 0;
    int64_t outstanding_events_ = 0;
    int64_t maximum_queue_length_ = 0;
    int64_t queue_age_microseconds_ = 0;
    int64_t maximum_queue_age_microseconds_ = 0;
    int64_t processed_events_ = 0;
    int64_t dropped_events_ = 0;
    int64_t gate_filtered_events_ = 0;
    int64_t musical_limit_rejected_attacks_ = 0;
    int64_t musical_limit_suppressed_releases_ = 0;
    int64_t output_rate_rejected_attacks_ = 0;
    int64_t output_rate_suppressed_releases_ = 0;
    int64_t output_rate_rejected_other_messages_ = 0;
    int64_t output_rate_safety_soft_excess_ = 0;
    int64_t output_rate_safety_bypass_messages_ = 0;
    std::atomic<models::OutputRateGate*> active_output_rate_gate_{ nullptr };
    int64_t virtual_queue_length_ = 0;
    int64_t virtual_outstanding_events_ = 0;
    int64_t virtual_maximum_queue_length_ = 0;
    int64_t last_dispatched_microseconds_ = 0;
    int64_t effective_speed_frontier_microseconds_ = 0;
    int64_t current_lag_microseconds_ = 0;
    int64_t maximum_lag_microseconds_ = 0;
    int32_t published_next_process_ = 0;
    int32_t published_drop_next_arrival_ = 0;
    int32_t published_drop_pending_ = 0;
    bool published_in_service_ = false;
    int32_t published_in_service_event_ = -1;
    bool has_published_queue_ = false;
    int32_t published_oldest_pending_event_ = -1;
    bool has_published_structural_due_work_ = false;
    int32_t published_structural_next_arrival_ = 0;
    int64_t published_structural_pending_ = 0;
    int64_t published_structural_oldest_due_microseconds_ = models::kInt64Max;
    bool published_structural_in_service_ = false;
    int64_t published_structural_in_service_due_microseconds_ = models::kInt64Max;
    int64_t published_structural_future_due_microseconds_ = models::kInt64Max;
    int32_t published_structural_future_count_ = 0;
    SnapshotEligibleCounter snapshot_eligible_;   // v1 _snapshotFilterStart/End, _snapshotEligibleCount, ... (DD-093)
    std::atomic<int32_t> uses_pre_admission_filter_accounting_{ 0 };
    ChannelStateTracker channel_state_;
    ChannelOverrideState channel_overrides_;
    ChannelRoutingState channel_routing_;
    std::deque<ControlRequestPtr> channel_control_requests_;
    std::shared_ptr<const ChannelPlaybackSnapshot> published_channel_state_;
    std::atomic<int32_t> channel_monitoring_enabled_{ 0 };
    std::atomic<int64_t> last_channel_publish_stamp_{ 0 };

    std::function<void()> playback_ended_;
    std::function<void(const EngineError&)> playback_failed_;
    std::function<void(int, ChannelAttribute, const EngineError&)> channel_control_failed_;
    std::function<void(int, ChannelAttribute, bool, const EngineError&)> channel_control_failure_detail_;
    std::function<void(const models::EventView&)> send_observer_;
};

} // namespace engine

namespace engine {

// The engine as its owner sees it: v1 PlaybackEngine's public members (see
// the map at the top of this file), forwarded to the shared EngineCore.
// The destructor is v1 Dispose: it stops playback within v1's bounded waits,
// then abandons (detaches) any worker still blocked in an output call; the
// worker keeps the core, the services and the output alive until it returns.
class PlaybackEngine {
public:
    static constexpr int32_t kDefaultQueueLengthLimit = EngineCore::kDefaultQueueLengthLimit;
    static constexpr int64_t kDefaultQueueAgeLimitMicroseconds = EngineCore::kDefaultQueueAgeLimitMicroseconds;
    static constexpr int32_t kDefaultDropBufferCapacity = EngineCore::kDefaultDropBufferCapacity;
    static constexpr int32_t kDefaultIntervalPolyphonyLimit = EngineCore::kDefaultIntervalPolyphonyLimit;

    explicit PlaybackEngine(std::shared_ptr<platform::EngineServices> services)
        : core_(std::make_shared<EngineCore>(std::move(services))) {}
    ~PlaybackEngine()
    {
        core_->dispose();
        core_->abandon_workers();
    }
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    void set_playback_ended_handler(std::function<void()> h) { core_->set_playback_ended_handler(std::move(h)); }
    void set_playback_failed_handler(std::function<void(const EngineError&)> h) { core_->set_playback_failed_handler(std::move(h)); }
    void set_channel_control_failed_handler(std::function<void(int, ChannelAttribute, const EngineError&)> h)
    { core_->set_channel_control_failed_handler(std::move(h)); }
    void set_channel_control_failure_detail_handler(std::function<void(int, ChannelAttribute, bool, const EngineError&)> h)
    { core_->set_channel_control_failure_detail_handler(std::move(h)); }

    PlaybackState state() { return core_->state(); }
    int64_t processing_microseconds() const { return core_->processing_microseconds(); }
    void set_processing_microseconds(int64_t v) { core_->set_processing_microseconds(v); }
    ServiceDurationMode service_duration_mode() const { return core_->service_duration_mode(); }
    void set_service_duration_mode(ServiceDurationMode v) { core_->set_service_duration_mode(v); }
    int64_t midi_bitrate() const { return core_->midi_bitrate(); }
    void set_midi_bitrate(int64_t v) { core_->set_midi_bitrate(v); }
    int64_t events_per_second() const { return core_->events_per_second(); }
    void set_events_per_second(int64_t v) { core_->set_events_per_second(v); }
    void set_service_configuration(ServiceDurationMode mode, int64_t processingMicroseconds, int64_t midiBitrate,
        int64_t eventsPerSecond)
    { core_->set_service_configuration(mode, processingMicroseconds, midiBitrate, eventsPerSecond); }
    bool simulate_slowdown() const { return core_->simulate_slowdown(); }
    void set_simulate_slowdown(bool v) { core_->set_simulate_slowdown(v); }
    bool apply_queue_limit_without_slowdown() const { return core_->apply_queue_limit_without_slowdown(); }
    void set_apply_queue_limit_without_slowdown(bool v) { core_->set_apply_queue_limit_without_slowdown(v); }
    bool chase_midi_state_on_play_seek() const { return core_->chase_midi_state_on_play_seek(); }
    void set_chase_midi_state_on_play_seek(bool v) { core_->set_chase_midi_state_on_play_seek(v); }
    bool preserve_playback_queue_on_pause_seek() const { return core_->preserve_playback_queue_on_pause_seek(); }
    void set_preserve_playback_queue_on_pause_seek(bool v) { core_->set_preserve_playback_queue_on_pause_seek(v); }
    bool output_paced_timeline() const { return core_->output_paced_timeline(); }
    void set_output_paced_timeline(bool v) { core_->set_output_paced_timeline(v); }
    void set_behind_yield_microseconds(int64_t microseconds) { core_->set_behind_yield_microseconds(microseconds); }
    bool scale_note_interval_by_traffic() const { return core_->scale_note_interval_by_traffic(); }
    void set_scale_note_interval_by_traffic(bool v) { core_->set_scale_note_interval_by_traffic(v); }
    bool interval_polyphony_limit_enabled() const { return core_->interval_polyphony_limit_enabled(); }
    void set_interval_polyphony_limit_enabled(bool v) { core_->set_interval_polyphony_limit_enabled(v); }
    int32_t interval_polyphony_limit() const { return core_->interval_polyphony_limit(); }
    void set_interval_polyphony_limit(int32_t v) { core_->set_interval_polyphony_limit(v); }
    int32_t queue_length_limit() const { return core_->queue_length_limit(); }
    void set_queue_length_limit(int32_t v) { core_->set_queue_length_limit(v); }
    bool queue_age_limit_enabled() const { return core_->queue_age_limit_enabled(); }
    void set_queue_age_limit_enabled(bool v) { core_->set_queue_age_limit_enabled(v); }
    int64_t queue_age_limit_microseconds() const { return core_->queue_age_limit_microseconds(); }
    void set_queue_age_limit_microseconds(int64_t v) { core_->set_queue_age_limit_microseconds(v); }
    int32_t drop_buffer_capacity() const { return core_->drop_buffer_capacity(); }
    void set_drop_buffer_capacity(int32_t v) { core_->set_drop_buffer_capacity(v); }
    OverflowPolicy overflow_policy() const { return core_->overflow_policy(); }
    void set_overflow_policy(OverflowPolicy v) { core_->set_overflow_policy(v); }
    int32_t worker_stop_timeout_milliseconds() const { return core_->worker_stop_timeout_milliseconds(); }
    void set_worker_stop_timeout_milliseconds(int32_t v) { core_->set_worker_stop_timeout_milliseconds(v); }

    // The engine shares ownership of the song and the output (v1's worker
    // keeps them reachable the same way).
    EngineError start(std::shared_ptr<const PlaybackSong> song, std::shared_ptr<platform::MidiOutput> output,
        ProcessingMode mode, int64_t startMicroseconds = 0, bool startPaused = false, bool continuation = false)
    { return core_->start(std::move(song), std::move(output), mode, startMicroseconds, startPaused, continuation); }
    EngineError seek(int64_t targetMicroseconds) { return core_->seek(targetMicroseconds); }
    EngineError pause() { return core_->pause(); }
    void resume() { core_->resume(); }
    EngineError stop() { return core_->stop(); }
    EngineError unload() { return core_->unload(); }
    EngineError dispose() { return core_->dispose(); }
    bool has_live_worker() { return core_->has_live_worker(); }
    bool has_attached_song() { return core_->has_attached_song(); }
    bool has_attached_output() { return core_->has_attached_output(); }

    PlaybackSnapshot snapshot() { return core_->snapshot(); }
    void reset_statistics() { core_->reset_statistics(); }
    void set_channel_monitoring(bool enabled) { core_->set_channel_monitoring(enabled); }
    bool chase_state_for_new_monitor(ChannelControlCompletion completion)
    { return core_->chase_state_for_new_monitor(std::move(completion)); }
    std::shared_ptr<const ChannelPlaybackSnapshot> channel_snapshot() const { return core_->channel_snapshot(); }
    int32_t channel_override(int channel, ChannelAttribute attribute) const { return core_->channel_override(channel, attribute); }
    EngineError set_channel_override(int channel, ChannelAttribute attribute, int32_t value)
    { return core_->set_channel_override(channel, attribute, value); }
    bool is_channel_enabled(int channel) const { return core_->is_channel_enabled(channel); }
    EngineError set_channel_enabled(int channel, bool enabled) { return core_->set_channel_enabled(channel, enabled); }
    EngineError chase_latest_source_channel_attribute(int channel, ChannelAttribute attribute,
        ChannelControlCompletion completion = ChannelControlCompletion())
    { return core_->chase_latest_source_channel_attribute(channel, attribute, std::move(completion)); }
    EngineError send_explicit_channel_attribute(int channel, ChannelAttribute attribute, int32_t value,
        ChannelControlCompletion completion = ChannelControlCompletion())
    { return core_->send_explicit_channel_attribute(channel, attribute, value, std::move(completion)); }

    const TimeConversion& time() const { return core_->time(); }
    int32_t wake_generation() const { return core_->wake_generation(); }
    // Harness hook: must outlive any worker the engine abandons.
    void set_send_observer(std::function<void(const models::EventView&)> observer)
    { core_->set_send_observer(std::move(observer)); }

private:
    std::shared_ptr<EngineCore> core_;
};

} // namespace engine
} // namespace midibn