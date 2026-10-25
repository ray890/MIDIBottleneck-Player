// v1 PlaybackEngine.cs: the worker (PlaybackWorker) and the classic
// (non-preserving) processing modes: Queue, Drop (finite queue and the
// forward-only virtual queue), Per-note interval gate, Delayed NoteOff, Note
// start rate gate and Output rate gate, with their dispatch helpers.
//
// v1's Drop mode also writes a DropTraceRecorder trace when one is attached
// (traceEnabled). Production never attaches one (MainForm never calls
// SetDropTrace), so traceEnabled is always false here: the trace-only
// branches and their bookkeeping (cluster size, consecutive drops, busy-until
// estimates, maximum buffer occupancy) are not ported, and every branch v1
// selects with traceEnabled == false is.
#include "engine_internal.h"

namespace midibn {
namespace engine {

using models::CompleteNoteEventKind;
using models::CompleteNoteTracker;
using models::EventView;
using models::kInt32Max;
using models::kInt64Max;
using models::max64;
using models::min64;
using models::PendingMidiQueue;
using models::ServiceDurationClock;

namespace {

int32_t wrap_add32(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

} // namespace

// ---------------------------------------------------------------- worker

void EngineCore::playback_worker()
{
    EngineError failure;
    bool hasFailure = false;
    bool completed = false;
    hasFailure = !capture([&] {
        platform::PlatformError waiterError;
        std::unique_ptr<platform::Waiter> waiter = services_.create_waiter(*wake_, waiterError);
        if (!waiter) raise(waiterError.type.c_str(), waiterError.message);
        ProcessingMode mode = mode_;
        if (preserve_playback_queue_on_pause_seek() &&
            (mode == ProcessingMode::Queue || mode == ProcessingMode::Drop ||
             mode == ProcessingMode::PerNoteIntervalGate || mode == ProcessingMode::DelayedNoteOff ||
             mode == ProcessingMode::NoteAttackLimit || mode == ProcessingMode::OutputRateLimit)) {
            if (mode == ProcessingMode::Queue) run_ordered_unlimited_queue_mode(*waiter);
            else if (!virtual_forward_drop_active_) {
                if (mode == ProcessingMode::Drop) run_ordered_finite_queue_mode(*waiter);
                else if (mode == ProcessingMode::PerNoteIntervalGate || mode == ProcessingMode::DelayedNoteOff)
                    run_ordered_musical_gate_mode(*waiter);
                else if (mode == ProcessingMode::OutputRateLimit) run_ordered_output_rate_mode(*waiter);
                else run_ordered_note_attack_mode(*waiter);
            }
            else run_ordered_virtual_forward_mode(*waiter);
        }
        else {
            if (mode == ProcessingMode::Queue) run_queue_mode(*waiter);
            else if (mode == ProcessingMode::Drop) run_drop_mode(*waiter);
            else if (mode == ProcessingMode::PerNoteIntervalGate) run_per_note_interval_gate(*waiter);
            else if (mode == ProcessingMode::DelayedNoteOff) run_delayed_note_off(*waiter);
            else if (mode == ProcessingMode::OutputRateLimit) run_output_rate_limit(*waiter);
            else run_note_attack_limit(*waiter);
        }
        waiter.reset();  // v1 `using` disposes the waiter here
        std::lock_guard<std::recursive_mutex> lock(sync_);
        completed = state_ == PlaybackState::Playing && song_ != nullptr;
    }, failure);

    if (hasFailure) {
        if (output_ != nullptr) output_->panic();  // v1: try { _output.Panic(); } catch { }
        if (channel_monitoring_enabled_.load() != 0) {
            channel_state_.panic_direct();
            publish_channel_state(true);
        }
    }

    platform::MidiOutput* finalSilenceOutput = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (thread_ && thread_->is_current() && silence_when_worker_exits_) {
            finalSilenceOutput = output_;
            silence_when_worker_exits_ = false;
        }
    }
    bool hasFinalSilenceFailure = false;
    EngineError finalSilenceFailure;
    if (finalSilenceOutput != nullptr) {
        platform::OutputStatus status = platform::reset_and_silence(finalSilenceOutput);
        if (!status.ok()) {
            hasFinalSilenceFailure = true;
            finalSilenceFailure = output_error(status);
        }
    }

    std::vector<ControlRequestPtr> abandonedControls;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (!(thread_ && thread_->is_current())) return;
        if (hasFinalSilenceFailure) {
            has_worker_exit_silence_failure_ = true;
            worker_exit_silence_failure_ = finalSilenceFailure;
        }
        if (hasFailure || completed) {
            if (state_ == PlaybackState::Playing) freeze_transport_locked();
            state_ = !hasFailure ? PlaybackState::Completed : PlaybackState::Stopped;
            preserved_seek_requests_.clear();
            intended_timeline_override_microseconds_ = -1;
        }
        thread_.reset();
        queue_length_ = 0;
        outstanding_events_ = 0;
        current_lag_microseconds_ = 0;
        abandonedControls = drain_channel_control_requests_locked();
    }
    publish_channel_state(true);
    complete_retired_channel_controls(abandonedControls, hasFailure ? failure
        : EngineError::make("OperationCanceledException",
            "Playback ended before the channel-control request reached the output boundary."));

    std::function<void(const EngineError&)> failedHandler;
    std::function<void()> endedHandler;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        failedHandler = playback_failed_;
        endedHandler = playback_ended_;
    }
    if (hasFailure) {
        if (failedHandler) failedHandler(failure);
    }
    else if (completed) {
        if (endedHandler) endedHandler();
    }
}

bool EngineCore::has_preserved_seek_request()
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    return !preserved_seek_requests_.empty();
}

void EngineCore::dispatch_protected_all_notes_off()
{
    bool trackChannels = channel_monitoring_enabled_.load() != 0;
    for (int channel = 0; channel < 16; ++channel) {
        if (!is_active()) return;
        EventView view = ChannelOverrideState::create_controller_message(channel, 123, 0, -1);
        send_protected_event(view);
        if (trackChannels) channel_state_.record_successful(view);
    }
    if (trackChannels) publish_channel_state(true);
}

// ---------------------------------------------------------------- Queue

void EngineCore::run_queue_mode(platform::Waiter& waiter)
{
    std::shared_ptr<const ServiceDurationSettings> serviceSettings = settings();
    ServiceDurationClock serviceClock = serviceSettings->create_clock();
    ServiceDurationClock beforeCurrentService = serviceClock;
    int32_t nextArrival = start_event_index_;
    int32_t nextProcess = start_event_index_;
    int32_t inService = -1;
    int64_t completionTicks = 0;
    int64_t completionStamp = kInt64Max;
    bool serviceHold = false;
    int64_t lastCompletionTicks = 0;
    int64_t eligiblePending = 0;
    FilteredSourceEvents filtered;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t overrideGeneration = channel_overrides_.filter_generation();
    if (has_active_source_filters()) uses_pre_admission_filter_accounting_.store(1);

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t now = current_transport_ticks();

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        int32_t currentOverrideGeneration = channel_overrides_.filter_generation();
        if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration) {
            if (has_active_source_filters()) uses_pre_admission_filter_accounting_.store(1);
            retire_unlimited_filtered_backlog(nextProcess, nextArrival, filtered, eligiblePending, inService,
                completionTicks, lastCompletionTicks, now, serviceClock, beforeCurrentService);
            if (inService < 0 && serviceHold) {
                end_output_paced_hold(serviceHold);
                serviceHold = false;
                completionStamp = kInt64Max;
            }
            else if (inService >= 0 && !serviceHold && completionTicks < kInt64Max)
                start_output_paced_service_hold(max64(0, models::wrap_sub(completionTicks, now)), serviceHold,
                    completionStamp);
            routingGeneration = currentRoutingGeneration;
            overrideGeneration = currentOverrideGeneration;
        }

        int32_t due = seekPending ? nextArrival : find_first_event_after_transport(nextArrival, now);
        admit_unlimited_range(nextArrival, due, filtered, eligiblePending);
        nextArrival = due;
        publish_unlimited_queue(nextProcess, eligiblePending, inService >= 0);

        if (inService >= 0 && (output_paced_timeline() ? completionStamp <= now_stamp() : completionTicks <= now)) {
            dispatch(inService, now);
            end_output_paced_hold(serviceHold);
            serviceHold = false;
            inService = -1;
            lastCompletionTicks = completionTicks;
            completionStamp = kInt64Max;
            continue;
        }

        if (inService < 0 && eligiblePending > 0) {
            update_service_clock(serviceClock, serviceSettings);
            if (simulate_slowdown_.load() == 0 || serviceClock.is_immediate()) {
                publish_unlimited_queue(nextProcess, max64(0, eligiblePending - 1), true);
                dispatch_immediate_filtered_range(nextProcess, nextArrival, now, filtered, eligiblePending,
                    serviceSettings);
                int64_t afterDispatch = current_transport_ticks();
                due = find_first_event_after_transport(nextArrival, afterDispatch);
                admit_unlimited_range(nextArrival, due, filtered, eligiblePending);
                nextArrival = due;
                publish_unlimited_queue(nextProcess, eligiblePending, false);
                continue;
            }
            inService = take_next_eligible(nextProcess, nextArrival, filtered);
            if (inService < 0) {
                eligiblePending = 0;
                continue;
            }
            eligiblePending--;
            int64_t startTicks = max64(event_ticks(inService), lastCompletionTicks);
            update_service_clock(serviceClock, serviceSettings);
            beforeCurrentService = serviceClock;
            int64_t serviceTicks = micros_to_ticks(serviceClock.next_microseconds(event_at(inService)));
            completionTicks = models::checked_add(startTicks, serviceTicks);
            serviceHold = begin_output_paced_hold();
            completionStamp = serviceHold ? saturating_add_ticks(now_stamp(), serviceTicks) : kInt64Max;
            publish_unlimited_queue(nextProcess, eligiblePending, true);
            continue;
        }

        if ((seekPending || nextArrival >= event_count()) && inService < 0 && eligiblePending == 0) return;

        if (inService >= 0 && serviceHold) {
            int64_t first = current_transport_ticks();
            int64_t second = current_transport_ticks();
            publish_channel_state_before_wait(first, second);
            wait_until_stopwatch(waiter, completionStamp, iterationWakeGeneration);
            continue;
        }
        int64_t target = inService >= 0 ? completionTicks : kInt64Max;
        if (nextArrival < event_count()) target = min64(target, event_ticks(nextArrival));
        if (inService < 0 && eligiblePending == 0) set_current_lag(0);
        publish_channel_state_before_wait(target, now);
        wait_until(waiter, target, iterationWakeGeneration);
    }
}

// ---------------------------------------------------------------- Drop

void EngineCore::run_drop_mode(platform::Waiter& waiter)
{
    if (virtual_forward_drop_active_) {
        run_virtual_forward_drop_mode(waiter);
        return;
    }
    std::shared_ptr<const ServiceDurationSettings> serviceSettings = settings();
    ServiceDurationClock serviceClock = serviceSettings->create_clock();
    ServiceDurationClock beforeCurrentService = serviceClock;
    int32_t nextArrival = start_event_index_;
    int32_t inService = -1;
    int64_t completionTicks = kInt64Max;
    int64_t completionStamp = kInt64Max;
    bool serviceHold = false;
    bool ageLimited = queue_age_limit_enabled_.load() != 0;
    const OverflowPolicy queuePolicy = overflow_policy();  // read once (DD-040; v1 read the setting twice)
    bool noteIndex = queuePolicy == OverflowPolicy::DropOldestCompleteNote ||
        queuePolicy == OverflowPolicy::DropBackgroundNotes;
    PendingMidiQueue pending(ageLimited ? kInt32Max : 1, noteIndex);
    CompleteNoteTracker completeNotes;
    FilteredSourceEvents filtered;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t overrideGeneration = channel_overrides_.filter_generation();
    bool admittingExternalBacklog = false;
    int64_t externalBacklogServiceStart = 0;
    int32_t musicalFairnessCursor = 0;
    if (has_active_source_filters()) uses_pre_admission_filter_accounting_.store(1);

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t now = current_transport_ticks();
        publish_drop_queue(nextArrival, pending, inService >= 0, ticks_to_micros(now));

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        int32_t currentOverrideGeneration = channel_overrides_.filter_generation();
        if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration) {
            if (has_active_source_filters()) uses_pre_admission_filter_accounting_.store(1);
            retire_drop_filtered_backlog(filtered, pending, completeNotes, inService, completionTicks, now, serviceClock,
                beforeCurrentService);
            if (inService < 0 && serviceHold) {
                end_output_paced_hold(serviceHold);
                serviceHold = false;
                completionStamp = kInt64Max;
            }
            else if (inService >= 0 && !serviceHold && completionTicks < kInt64Max)
                start_output_paced_service_hold(max64(0, models::wrap_sub(completionTicks, now)), serviceHold,
                    completionStamp);
            routingGeneration = currentRoutingGeneration;
            overrideGeneration = currentOverrideGeneration;
        }
        int64_t arrivalTicks = !seekPending && nextArrival < event_count() ? event_ticks(nextArrival) : kInt64Max;

        if (admittingExternalBacklog && arrivalTicks > now) {
            admittingExternalBacklog = false;
            if (inService < 0 && pending.count() > 0) {
                inService = pending.dequeue();
                update_service_clock(serviceClock, serviceSettings);
                beforeCurrentService = serviceClock;
                int64_t serviceStart = simulate_slowdown_.load() == 0 ? now : externalBacklogServiceStart;
                int64_t serviceTicks = micros_to_ticks(next_playback_service_microseconds(serviceClock, event_at(inService)));
                completionTicks = models::checked_add(serviceStart, serviceTicks);
                start_output_paced_service_hold(serviceTicks, serviceHold, completionStamp);
            }
            update_queue(pending.count(), inService >= 0);
        }

        bool serviceComplete = inService >= 0 && (serviceHold
            ? arrivalTicks > now && completionStamp <= now_stamp()
            : completionTicks <= arrivalTicks && completionTicks <= now);
        if (serviceComplete) {
            int64_t completedAt = completionTicks;
            int64_t providerSendStart;
            int64_t providerSendEnd;
            dispatch(inService, now, providerSendStart, providerSendEnd);
            end_output_paced_hold(serviceHold);
            serviceHold = false;
            completionStamp = kInt64Max;
            now = current_transport_ticks();
            inService = -1;
            completionTicks = kInt64Max;
            if (!seekPending && nextArrival < event_count() && event_ticks(nextArrival) > providerSendStart &&
                event_ticks(nextArrival) <= providerSendEnd) {
                // A synchronous provider call allowed later source arrivals to
                // become overdue; admit that range through the finite queue first.
                admittingExternalBacklog = true;
                externalBacklogServiceStart = completedAt;
            }
            if (!seekPending && ageLimited && pending.count() > 0) {
                int64_t logicalNowMicroseconds = ticks_to_micros(completedAt);
                int64_t ageLimit = queue_age_limit_microseconds_.load();
                OverflowPolicy completionPolicy = overflow_policy();
                {
                    std::lock_guard<std::recursive_mutex> lock(sync_);
                    int64_t observedAge = max64(0, models::wrap_sub(logicalNowMicroseconds,
                        event_micros(pending.oldest_event_index())));
                    if (observedAge > maximum_queue_age_microseconds_) maximum_queue_age_microseconds_ = observedAge;
                }
                if (completionPolicy == OverflowPolicy::DropOldest) {
                    while (queue_age_exceeded(pending, logicalNowMicroseconds, ageLimit)) {
                        int32_t evicted = pending.dequeue();
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_++;
                        }
                        record_channel_drop(evicted);
                    }
                }
                else if (completionPolicy == OverflowPolicy::DropOldestCompleteNote) {
                    while (queue_age_exceeded(pending, logicalNowMicroseconds, ageLimit)) {
                        int32_t evictedAttack;
                        int32_t evictedRelease;
                        if (!pending.try_evict_oldest_complete_note_at_head(evictedAttack, evictedRelease)) break;
                        if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                        int32_t discarded = evictedRelease >= 0 ? 2 : 1;
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_ += discarded;
                        }
                        record_channel_drop(evictedAttack);
                        if (evictedRelease >= 0) record_channel_drop(evictedRelease);
                    }
                }
                else if (completionPolicy == OverflowPolicy::ClearBufferAndCatchUp &&
                    queue_age_exceeded(pending, logicalNowMicroseconds, ageLimit)) {
                    int32_t clearedCount = pending.count();
                    bool trackChannels = channel_monitoring_enabled_.load() != 0;
                    std::vector<int32_t> pendingIndices = pending.event_indices();
                    for (int32_t pendingIndex : pendingIndices)
                        if (trackChannels) channel_state_.record_dropped(event_at(pendingIndex));
                    pending.clear();
                    int32_t catchUp = find_first_event_after_transport(nextArrival, completedAt);
                    int32_t skippedDrops = 0;
                    for (int32_t skipped = nextArrival; skipped < catchUp; ++skipped) {
                        if (has_active_source_filters() && mark_new_source_filtered(skipped, filtered)) {
                            EventView filteredEvent = event_at(skipped);
                            CompleteNoteEventKind filteredKind = CompleteNoteTracker::classify(filteredEvent);
                            if (filteredKind == CompleteNoteEventKind::NoteOn)
                                completeNotes.record_filtered_note_on(filteredEvent, skipped);
                            else if (filteredKind == CompleteNoteEventKind::NoteOff)
                                completeNotes.take_note_off(filteredEvent);
                            continue;
                        }
                        skippedDrops++;
                        if (trackChannels) channel_state_.record_dropped(event_at(skipped));
                    }
                    clearedCount = wrap_add32(clearedCount, skippedDrops);
                    {
                        std::lock_guard<std::recursive_mutex> lock(sync_);
                        dropped_events_ += clearedCount;
                    }
                    nextArrival = catchUp;
                    update_queue(0, false);
                    set_current_lag(0);
                    output_->panic();  // v1: try { _output.Panic(); } catch { }
                    channel_overrides_.mark_all_forced_pending();
                    if (trackChannels) {
                        channel_state_.panic_direct();
                        publish_channel_state(true);
                    }
                    continue;
                }
            }
            if (!admittingExternalBacklog && pending.count() > 0) {
                inService = pending.dequeue();
                // Backlogged service begins at the preceding logical completion.
                int64_t serviceStart = completedAt;
                update_service_clock(serviceClock, serviceSettings);
                beforeCurrentService = serviceClock;
                int64_t serviceTicks = micros_to_ticks(next_playback_service_microseconds(serviceClock, event_at(inService)));
                completionTicks = models::checked_add(serviceStart, serviceTicks);
                start_output_paced_service_hold(serviceTicks, serviceHold, completionStamp);
            }
            update_queue(pending.count(), inService >= 0);
            continue;
        }

        if (nextArrival < event_count() && arrivalTicks <= now) {
            if (has_active_source_filters() && mark_new_source_filtered(nextArrival, filtered)) {
                EventView filteredEvent = event_at(nextArrival);
                CompleteNoteEventKind filteredKind = CompleteNoteTracker::classify(filteredEvent);
                if (filteredKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_filtered_note_on(filteredEvent, nextArrival);
                else if (filteredKind == CompleteNoteEventKind::NoteOff)
                    completeNotes.take_note_off(filteredEvent);
                nextArrival++;
                update_queue(pending.count(), inService >= 0);
                continue;
            }
            update_service_clock(serviceClock, serviceSettings);
            if (!admittingExternalBacklog && inService < 0 && pending.count() == 0 &&
                (simulate_slowdown_.load() == 0 || serviceClock.is_immediate())) {
                int32_t dueEnd = nextArrival + 1;
                int32_t dueLimit = event_count() < wrap_add32(nextArrival, 2048) ? event_count()
                                                                                 : wrap_add32(nextArrival, 2048);
                int64_t latestDue;
                if (time_.latest_microseconds_due(now, latestDue)) {
                    while (dueEnd < dueLimit && event_micros(dueEnd) <= latestDue) dueEnd++;   // DD-066
                }
                else {
                    while (dueEnd < dueLimit && event_ticks(dueEnd) <= now) dueEnd++;
                }
                publish_drop_queue(nextArrival + 1, 0, true);
                if (has_active_source_filters()) {
                    int64_t eligibleImmediate = static_cast<int64_t>(dueEnd) - nextArrival;
                    dispatch_immediate_filtered_range(nextArrival, dueEnd, now, filtered, eligibleImmediate,
                        serviceSettings, true);
                }
                else {
                    dispatch_immediate_range(nextArrival, dueEnd, now, serviceSettings);
                }
                int64_t afterImmediate = current_transport_ticks();
                if (nextArrival < event_count() && event_ticks(nextArrival) <= afterImmediate &&
                    models::wrap_sub(afterImmediate, now) >= time_.frequency() / 125) {
                    admittingExternalBacklog = true;
                    externalBacklogServiceStart = now;
                }
                publish_drop_queue(nextArrival, 0, false);
                continue;
            }

            EventView incomingEvent = event_at(nextArrival);
            CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(incomingEvent);
            OverflowPolicy overflowPolicy = overflow_policy();
            pending.set_note_index_enabled(overflowPolicy == OverflowPolicy::DropOldestCompleteNote ||
                overflowPolicy == OverflowPolicy::DropBackgroundNotes);
            CompleteNoteTracker::NoteOffMatch noteMatch;
            if (noteKind == CompleteNoteEventKind::NoteOff) noteMatch = completeNotes.take_note_off(incomingEvent);
            else noteMatch.attack_index = -1;
            if (noteMatch.filtered) {
                channel_routing_.record_filtered(incomingEvent.channel());
                uses_pre_admission_filter_accounting_.store(1);
                nextArrival++;
                update_queue(pending.count(), inService >= 0);
                continue;
            }
            if (noteMatch.suppress || ((overflowPolicy == OverflowPolicy::DropOldestCompleteNote ||
                overflowPolicy == OverflowPolicy::DropBackgroundNotes) &&
                noteKind == CompleteNoteEventKind::NoteOff && noteMatch.attack_index >= 0 && !noteMatch.accepted)) {
                {
                    std::lock_guard<std::recursive_mutex> lock(sync_);
                    dropped_events_++;
                }
                record_channel_drop(nextArrival);
                nextArrival++;
                update_queue(pending.count(), inService >= 0);
                continue;
            }

            PendingMidiQueue::Entry incomingEntry;
            incomingEntry.event_index = nextArrival;
            incomingEntry.paired_attack_index = noteMatch.attack_index;
            incomingEntry.is_note_on = noteKind == CompleteNoteEventKind::NoteOn;
            incomingEntry.is_note_off = noteKind == CompleteNoteEventKind::NoteOff;
            int64_t arrivalMicroseconds = admittingExternalBacklog ? ticks_to_micros(now) : incomingEvent.micros;
            int64_t ageLimit = queue_age_limit_microseconds_.load();
            bool ageEvictionStalled = false;
            if (ageLimited && overflowPolicy == OverflowPolicy::DropOldest) {
                while (queue_age_exceeded(pending, arrivalMicroseconds, ageLimit)) {
                    int32_t evicted = pending.dequeue();
                    {
                        std::lock_guard<std::recursive_mutex> lock(sync_);
                        dropped_events_++;
                    }
                    record_channel_drop(evicted);
                }
            }
            else if (ageLimited && overflowPolicy == OverflowPolicy::DropOldestCompleteNote) {
                while (queue_age_exceeded(pending, arrivalMicroseconds, ageLimit)) {
                    int32_t previousOldest = pending.oldest_event_index();
                    int32_t evictedAttack;
                    int32_t evictedRelease;
                    if (!pending.try_evict_oldest_complete_note_at_head(evictedAttack, evictedRelease)) {
                        ageEvictionStalled = true;
                        break;
                    }
                    if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                    int32_t discarded = evictedRelease >= 0 ? 2 : 1;
                    {
                        std::lock_guard<std::recursive_mutex> lock(sync_);
                        dropped_events_ += discarded;
                    }
                    record_channel_drop(evictedAttack);
                    if (evictedRelease >= 0) record_channel_drop(evictedRelease);
                    // A later removable note cannot repair a protected overdue head.
                    if (pending.count() > 0 && pending.oldest_event_index() == previousOldest) {
                        ageEvictionStalled = true;
                        break;
                    }
                }
            }
            int32_t outstanding = pending.count() + (inService >= 0 ? 1 : 0);
            int32_t bufferCapacity = ageLimited ? kInt32Max : queue_length_limit_.load();
            bool ageExceeded = ageLimited && queue_age_exceeded(pending, arrivalMicroseconds, ageLimit);
            bool protectedTraffic = noteKind == CompleteNoteEventKind::NoteOff ||
                CompleteNoteTracker::is_safety_control(incomingEvent);
            bool safetyAdmission = overflowPolicy == OverflowPolicy::DropIncomingCompleteNotes &&
                noteKind != CompleteNoteEventKind::NoteOn;
            if (((!ageLimited && outstanding < bufferCapacity) || (ageLimited && !ageExceeded)) || safetyAdmission) {
                int32_t acceptedIndex = nextArrival;
                if (inService < 0 && !admittingExternalBacklog) {
                    inService = acceptedIndex;
                    int64_t serviceStart = arrivalTicks;
                    update_service_clock(serviceClock, serviceSettings);
                    beforeCurrentService = serviceClock;
                    int64_t serviceTicks = micros_to_ticks(next_playback_service_microseconds(serviceClock,
                        event_at(inService)));
                    completionTicks = models::checked_add(serviceStart, serviceTicks);
                    start_output_paced_service_hold(serviceTicks, serviceHold, completionStamp);
                }
                else {
                    pending.enqueue(incomingEntry);
                }
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, acceptedIndex, true, false);
                update_queue(pending.count(), inService >= 0);
            }
            else {
                if (overflowPolicy == OverflowPolicy::DropOldest && pending.count() > 0) {
                    EventView oldestEvent = event_at(pending.oldest_event_index());
                    bool oldestProtected = CompleteNoteTracker::classify(oldestEvent) == CompleteNoteEventKind::NoteOff ||
                        CompleteNoteTracker::is_safety_control(oldestEvent);
                    if (!oldestProtected) {
                        int32_t evicted = pending.dequeue();
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_++;
                        }
                        record_channel_drop(evicted);
                        pending.enqueue(incomingEntry);
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, true, false);
                    }
                    else if (protectedTraffic) {
                        // A protected head cannot be removed safely: admit the
                        // arriving release/safety message as soft-limit traffic.
                        pending.enqueue(incomingEntry);
                    }
                    else {
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, false, false);
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_++;
                        }
                        record_channel_drop(nextArrival);
                    }
                    update_queue(pending.count(), inService >= 0);
                }
                else if (overflowPolicy == OverflowPolicy::DropOldestCompleteNote) {
                    int32_t evictedAttack = -1;
                    int32_t evictedRelease = -1;
                    bool evicted = !ageEvictionStalled &&
                        pending.try_evict_oldest_complete_note(evictedAttack, evictedRelease);
                    if (evicted) {
                        if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                        int32_t discarded = evictedRelease >= 0 ? 2 : 1;
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_ += discarded;
                        }
                        record_channel_drop(evictedAttack);
                        if (evictedRelease >= 0) record_channel_drop(evictedRelease);
                    }
                    bool ownAttackEvicted = evicted && noteKind == CompleteNoteEventKind::NoteOff &&
                        noteMatch.attack_index == evictedAttack;
                    if (ownAttackEvicted || (!evicted && !protectedTraffic)) {
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, false, true);
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_++;
                        }
                        record_channel_drop(nextArrival);
                    }
                    else {
                        pending.enqueue(incomingEntry);
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, true, false);
                    }
                    update_queue(pending.count(), inService >= 0);
                }
                else if (overflowPolicy == OverflowPolicy::DropBackgroundNotes) {
                    int32_t evictedAttack = 0;
                    int32_t evictedRelease = 0;
                    bool evicted = pending.try_evict_least_important_complete_note(events_, musicalFairnessCursor,
                        evictedAttack, evictedRelease);
                    if (evicted) {
                        if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                        int32_t discarded = evictedRelease >= 0 ? 2 : 1;
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_ += discarded;
                        }
                        record_channel_drop(evictedAttack);
                        if (evictedRelease >= 0) record_channel_drop(evictedRelease);
                    }
                    bool ownAttackEvicted = evicted && noteKind == CompleteNoteEventKind::NoteOff &&
                        noteMatch.attack_index == evictedAttack;
                    if (ownAttackEvicted || (!evicted && noteKind == CompleteNoteEventKind::NoteOn)) {
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, false, true);
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_++;
                        }
                        record_channel_drop(nextArrival);
                    }
                    else {
                        pending.enqueue(incomingEntry);
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, true, false);
                    }
                    update_queue(pending.count(), inService >= 0);
                }
                else if (overflowPolicy == OverflowPolicy::ClearBufferAndCatchUp) {
                    // A live capacity decrease never retracts the event already in
                    // service. Clear only pending/source work.
                    int32_t clearedCount = pending.count();
                    bool trackChannels = channel_monitoring_enabled_.load() != 0;
                    if (trackChannels) {
                        std::vector<int32_t> pendingIndices = pending.event_indices();
                        for (int32_t pendingIndex : pendingIndices) channel_state_.record_dropped(event_at(pendingIndex));
                    }
                    pending.clear();

                    // DD-041: a skipped event of a muted channel (or one that a forced override replaces) is filtered,
                    // not dropped, exactly as in the waiting-time Clear buffer above (v1 counted it as dropped here).
                    int32_t catchUp = find_first_event_after_transport(nextArrival, now);
                    int32_t skippedDrops = 0;
                    for (int32_t skipped = nextArrival; skipped < catchUp; ++skipped) {
                        if (has_active_source_filters() && mark_new_source_filtered(skipped, filtered)) {
                            EventView filteredEvent = event_at(skipped);
                            CompleteNoteEventKind filteredKind = CompleteNoteTracker::classify(filteredEvent);
                            if (filteredKind == CompleteNoteEventKind::NoteOn)
                                completeNotes.record_filtered_note_on(filteredEvent, skipped);
                            else if (filteredKind == CompleteNoteEventKind::NoteOff)
                                completeNotes.take_note_off(filteredEvent);
                            continue;
                        }
                        skippedDrops++;
                        if (trackChannels) channel_state_.record_dropped(event_at(skipped));
                    }
                    clearedCount = wrap_add32(clearedCount, skippedDrops);
                    {
                        std::lock_guard<std::recursive_mutex> lock(sync_);
                        dropped_events_ += clearedCount;
                    }
                    nextArrival = catchUp;
                    update_queue(0, inService >= 0);
                    set_current_lag(0);
                    output_->panic();  // v1: try { _output.Panic(); } catch { }
                    channel_overrides_.mark_all_forced_pending();
                    if (trackChannels) {
                        channel_state_.panic_direct();
                        publish_channel_state(true);
                    }
                    continue;
                }
                else if (overflowPolicy == OverflowPolicy::DropIncomingCompleteNotes &&
                    noteKind == CompleteNoteEventKind::NoteOn) {
                    completeNotes.record_note_on(incomingEvent, nextArrival, false, true);
                    {
                        std::lock_guard<std::recursive_mutex> lock(sync_);
                        dropped_events_++;
                    }
                    record_channel_drop(nextArrival);
                }
                else {
                    if (protectedTraffic) {
                        pending.enqueue(incomingEntry);
                        update_queue(pending.count(), inService >= 0);
                    }
                    else {
                        if (noteKind == CompleteNoteEventKind::NoteOn)
                            completeNotes.record_note_on(incomingEvent, nextArrival, false, false);
                        {
                            std::lock_guard<std::recursive_mutex> lock(sync_);
                            dropped_events_++;
                        }
                        record_channel_drop(nextArrival);
                    }
                }
            }
            nextArrival++;
            continue;
        }

        if ((seekPending || nextArrival >= event_count()) && inService < 0 && pending.count() == 0) return;

        if (inService >= 0 && serviceHold && arrivalTicks > now) {
            publish_channel_state_before_wait(now, now);
            wait_until_stopwatch(waiter, completionStamp, iterationWakeGeneration);
            continue;
        }
        int64_t target = min64(arrivalTicks, completionTicks);
        if (inService < 0) set_current_lag(0);
        publish_channel_state_before_wait(target, now);
        wait_until(waiter, target, iterationWakeGeneration);
    }
}

void EngineCore::run_virtual_forward_drop_mode(platform::Waiter& waiter)
{
    std::shared_ptr<const ServiceDurationSettings> serviceSettings = settings();
    ServiceDurationClock serviceClock = serviceSettings->create_clock();
    int32_t nextArrival = start_event_index_;
    int32_t bufferCapacity = queue_length_limit_.load();
    models::ForwardDropQueue virtualQueue(bufferCapacity);
    CompleteNoteTracker completeNotes;
    FilteredSourceEvents filtered;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t overrideGeneration = channel_overrides_.filter_generation();
    if (has_active_source_filters()) uses_pre_admission_filter_accounting_.store(1);

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t nowTicks = current_transport_ticks();
        publish_drop_queue(nextArrival, 0, false);

        if (seekPending) return;

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        int32_t currentOverrideGeneration = channel_overrides_.filter_generation();
        if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration) {
            routingGeneration = currentRoutingGeneration;
            overrideGeneration = currentOverrideGeneration;
            if (has_active_source_filters()) uses_pre_admission_filter_accounting_.store(1);
        }

        if (nextArrival < event_count() && event_ticks(nextArrival) <= nowTicks) {
            int32_t eventIndex = nextArrival++;
            EventView incomingEvent = event_at(eventIndex);
            int64_t arrival = incomingEvent.micros;
            virtualQueue.advance(arrival);
            publish_virtual_queue(virtualQueue);

            if (has_active_source_filters() && mark_new_source_filtered(eventIndex, filtered)) continue;

            CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(incomingEvent);
            if (noteKind == CompleteNoteEventKind::NoteOff && completeNotes.should_suppress_note_off(incomingEvent)) {
                record_virtual_drop(eventIndex);
                continue;
            }

            OverflowPolicy policy = overflow_policy();
            if (policy != OverflowPolicy::DropNewest && policy != OverflowPolicy::DropIncomingCompleteNotes)
                raise("InvalidOperationException", "The selected overflow policy requires Simulate slowdown because it would retract MIDI that the forward-only model has already sent.");
            bool safetyAdmission = policy == OverflowPolicy::DropIncomingCompleteNotes &&
                noteKind != CompleteNoteEventKind::NoteOn;
            update_service_clock(serviceClock, serviceSettings);
            ServiceDurationClock admittedClock = serviceClock;
            int64_t service = admittedClock.next_microseconds(incomingEvent);
            bool accepted = virtualQueue.try_admit(arrival, service, safetyAdmission);
            if (!accepted) {
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, false, policy == OverflowPolicy::DropIncomingCompleteNotes);
                record_virtual_drop(eventIndex);
                publish_virtual_queue(virtualQueue);
                continue;
            }

            serviceClock = admittedClock;

            if (noteKind == CompleteNoteEventKind::NoteOn) completeNotes.record_note_on(incomingEvent, true, false);
            publish_virtual_queue(virtualQueue);
            publish_drop_queue(nextArrival, 0, true);
            dispatch(eventIndex, nowTicks);
            publish_drop_queue(nextArrival, 0, false);
            continue;
        }

        int64_t nowMicroseconds = ticks_to_micros(nowTicks);
        virtualQueue.advance(nowMicroseconds);
        publish_virtual_queue(virtualQueue);
        if (nextArrival >= event_count()) return;
        set_current_lag(0);
        int64_t target = event_ticks(nextArrival);
        publish_channel_state_before_wait(target, nowTicks);
        wait_until(waiter, target, iterationWakeGeneration);
    }
}

void EngineCore::record_virtual_drop(int32_t eventIndex)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        dropped_events_++;
    }
    record_channel_drop(eventIndex);
}

void EngineCore::publish_virtual_queue(const models::ForwardDropQueue& queue)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    virtual_queue_length_ = max64(0, static_cast<int64_t>(queue.occupancy()) - 1);
    virtual_outstanding_events_ = queue.occupancy();
    virtual_maximum_queue_length_ = max64(virtual_maximum_queue_length_, queue.occupancy());
}

// ---------------------------------------------------------------- Per-note interval gate

void EngineCore::run_per_note_interval_gate(platform::Waiter& waiter)
{
    int64_t interval = processing_microseconds();
    int32_t intervalLimit = interval_polyphony_limit_enabled() ? interval_polyphony_limit()
                                                               : kDefaultIntervalPolyphonyLimit;
    std::unique_ptr<models::PerNoteIntervalGate> gatePointer(new models::PerNoteIntervalGate(interval,
        interval_polyphony_limit_enabled(), intervalLimit));
    models::PerNoteIntervalGate& gate = *gatePointer;
    models::AdaptiveNoteIntervalClock adaptive(interval, scale_note_interval_by_traffic());
    std::vector<EventView> boundaryEvents(128);
    int32_t nextArrival = start_event_index_;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t disableGenerations[16];
    for (int channel = 0; channel < 16; ++channel) disableGenerations[channel] = channel_routing_.disable_generation(channel);
    FilteredSourceEvents sourceFilters;
    bool sourceCompleted = false;

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t nowMicroseconds = ticks_to_micros(current_transport_ticks());

        if (seekPending && !sourceCompleted) {
            gate.complete_source(nowMicroseconds);
            add_gate_filtered(gate.take_filtered_event_count());
            sourceCompleted = true;
        }

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        if (currentRoutingGeneration != routingGeneration) {
            for (int channel = 0; channel < 16; ++channel) {
                int32_t disabledAt = channel_routing_.disable_generation(channel);
                if (disabledAt == disableGenerations[channel]) continue;
                disableGenerations[channel] = disabledAt;
                int32_t retired = gate.retire_channel(channel, nowMicroseconds);
                for (int32_t index = 0; index < retired; ++index) channel_routing_.record_filtered(channel);
            }
            routingGeneration = currentRoutingGeneration;
            publish_channel_state(true);
        }

        bool didWork = false;
        int32_t examined = 0;
        int64_t batchStarted = now_stamp();
        while (is_playing() && dispatch_suspended_.load() == 0) {
            int64_t localBoundary = gate.next_boundary_microseconds();
            int64_t nextSource = !seekPending && nextArrival < event_count() ? event_micros(nextArrival) : kInt64Max;
            adaptive.complete_if_closed(localBoundary, nextSource);
            int64_t nextBoundary = adaptive.effective_boundary(localBoundary);
            int64_t nextAction = min64(nextBoundary, nextSource);
            if (nextAction > nowMicroseconds) break;

            // Source events exactly on a boundary are admitted before that
            // boundary is resolved; one absolute tick is admitted together.
            if (nextSource <= nextBoundary) {
                int64_t sourceTick = event_at(nextArrival).tick;
                gate.begin_source_tick(sourceTick);
                while (nextArrival < event_count() && event_at(nextArrival).tick == sourceTick) {
                    int32_t eventIndex = nextArrival++;
                    examined++;
                    if (has_active_source_filters() && mark_new_source_filtered(eventIndex, sourceFilters)) continue;
                    EventView midiEvent = event_at(eventIndex);
                    models::PerNoteGateAdmission admission = gate.admit(midiEvent);
                    if (admission == models::PerNoteGateAdmission::NotNote) {
                        publish_structural_due_work(nextArrival, 0, kInt64Max, true, midiEvent.micros,
                            gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                        dispatch_midi_event(midiEvent);
                        publish_structural_due_work(nextArrival, 0, kInt64Max, false, kInt64Max,
                            gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                    }
                    if (should_retire_worker_at_dispatch_boundary()) return;
                }
                gate.end_source_tick();
                add_gate_filtered(gate.take_filtered_event_count());
            }
            else {
                int32_t count = gate.emit_boundary(nextBoundary, boundaryEvents.data(),
                    static_cast<int32_t>(boundaryEvents.size()));
                adaptive.record_boundary_pass(nextBoundary, count);
                add_gate_filtered(gate.take_filtered_event_count());
                for (int32_t index = 0; index < count; ++index) {
                    // A native send may have been blocked while a transport
                    // boundary was requested; do not emit the rest of the batch.
                    if (should_retire_worker_at_dispatch_boundary()) return;
                    publish_structural_due_work(nextArrival, count - index - 1, nextBoundary, true, nextBoundary,
                        gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                    dispatch_midi_event(boundaryEvents[static_cast<size_t>(index)]);
                    publish_structural_due_work(nextArrival, count - index - 1, nextBoundary, false, kInt64Max,
                        gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                }
                advance_gate_effective_speed_frontier(nextBoundary);
                examined += count > 1 ? count : 1;
            }
            didWork = true;
            nowMicroseconds = ticks_to_micros(current_transport_ticks());
            if (examined >= 2048 || models::wrap_sub(now_stamp(), batchStarted) >= time_.frequency() / 125) break;
        }

        int64_t nextSourceAfterWork = !seekPending && nextArrival < event_count() ? event_micros(nextArrival) : kInt64Max;
        adaptive.complete_if_closed(gate.next_boundary_microseconds(), nextSourceAfterWork);
        publish_structural_due_work(nextArrival, 0, kInt64Max, false, kInt64Max, gate.next_boundary_microseconds(),
            gate.next_boundary_output_count());
        if (!seekPending && nextArrival >= event_count() && !sourceCompleted) {
            std::shared_ptr<const PlaybackSong> song;
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                song = song_;
            }
            gate.complete_source(!song ? nowMicroseconds : song->duration_us);
            add_gate_filtered(gate.take_filtered_event_count());
            sourceCompleted = true;
        }
        if ((seekPending || nextArrival >= event_count()) && !gate.has_pending_transitions()) return;

        int64_t targetMicroseconds = adaptive.effective_boundary(gate.next_boundary_microseconds());
        if (!seekPending && nextArrival < event_count())
            targetMicroseconds = min64(targetMicroseconds, event_micros(nextArrival));
        int64_t nowTicks = current_transport_ticks();
        int64_t targetTicks = micros_to_ticks(targetMicroseconds);
        publish_channel_state_before_wait(targetTicks, nowTicks);
        if (!didWork || targetTicks > nowTicks) wait_until(waiter, targetTicks, iterationWakeGeneration);
    }
}

// ---------------------------------------------------------------- Delayed NoteOff

void EngineCore::run_delayed_note_off(platform::Waiter& waiter)
{
    int64_t interval = processing_microseconds();
    int32_t intervalLimit = interval_polyphony_limit_enabled() ? interval_polyphony_limit()
                                                               : kDefaultIntervalPolyphonyLimit;
    std::unique_ptr<models::DelayedNoteOffGate> gatePointer(new models::DelayedNoteOffGate(interval,
        interval_polyphony_limit_enabled(), intervalLimit));
    models::DelayedNoteOffGate& gate = *gatePointer;
    models::AdaptiveNoteIntervalClock adaptive(interval, scale_note_interval_by_traffic());
    std::vector<EventView> boundaryEvents;
    boundaryEvents.reserve(128);
    int32_t nextArrival = start_event_index_;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t disableGenerations[16];
    for (int channel = 0; channel < 16; ++channel) disableGenerations[channel] = channel_routing_.disable_generation(channel);
    FilteredSourceEvents sourceFilters;
    bool sourceCompleted = false;

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t nowMicroseconds = ticks_to_micros(current_transport_ticks());

        if (seekPending && !sourceCompleted) {
            gate.complete_source(nowMicroseconds);
            sourceCompleted = true;
        }

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        if (currentRoutingGeneration != routingGeneration) {
            for (int channel = 0; channel < 16; ++channel) {
                int32_t disabledAt = channel_routing_.disable_generation(channel);
                if (disabledAt == disableGenerations[channel]) continue;
                disableGenerations[channel] = disabledAt;
                int32_t retired = gate.retire_channel(channel, nowMicroseconds);
                for (int32_t index = 0; index < retired; ++index) channel_routing_.record_filtered(channel);
            }
            routingGeneration = currentRoutingGeneration;
            publish_channel_state(true);
        }

        bool didWork = false;
        int32_t examined = 0;
        int64_t batchStarted = now_stamp();
        while (is_playing() && dispatch_suspended_.load() == 0) {
            int64_t localBoundary = gate.next_boundary_microseconds();
            int64_t nextSource = !seekPending && nextArrival < event_count() ? event_micros(nextArrival) : kInt64Max;
            adaptive.complete_if_closed(localBoundary, nextSource);
            int64_t nextBoundary = adaptive.effective_boundary(localBoundary);
            int64_t nextAction = min64(nextBoundary, nextSource);
            if (nextAction > nowMicroseconds) break;

            // D-boundary releases win exact-time ties; recovery candidates at
            // the exact R boundary are admitted first.
            bool sourcePrecedesRecovery = nextArrival < event_count() &&
                gate.source_precedes_boundary(event_at(nextArrival), nextBoundary);
            if (nextBoundary < nextSource || (nextBoundary == nextSource && !sourcePrecedesRecovery)) {
                int32_t count = gate.emit_boundary(nextBoundary, boundaryEvents);
                adaptive.record_boundary_pass(nextBoundary, count);
                add_gate_filtered(gate.take_filtered_event_count());
                for (int32_t index = 0; index < count; ++index) {
                    if (should_retire_worker_at_dispatch_boundary()) return;
                    publish_structural_due_work(nextArrival, count - index - 1, nextBoundary, true, nextBoundary,
                        gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                    dispatch_midi_event(boundaryEvents[static_cast<size_t>(index)]);
                    publish_structural_due_work(nextArrival, count - index - 1, nextBoundary, false, kInt64Max,
                        gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                }
                advance_gate_effective_speed_frontier(nextBoundary);
                examined += count > 1 ? count : 1;
            }
            else {
                int32_t eventIndex = nextArrival++;
                examined++;
                if (!has_active_source_filters() || !mark_new_source_filtered(eventIndex, sourceFilters)) {
                    EventView midiEvent = event_at(eventIndex);
                    models::DelayedNoteOffAdmission admission = gate.admit(midiEvent);
                    if (admission == models::DelayedNoteOffAdmission::NotNote) {
                        publish_structural_due_work(nextArrival, 0, kInt64Max, true, midiEvent.micros,
                            gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                        dispatch_midi_event(midiEvent);
                        publish_structural_due_work(nextArrival, 0, kInt64Max, false, kInt64Max,
                            gate.next_boundary_microseconds(), gate.next_boundary_output_count());
                        if (models::DelayedNoteOffGate::is_note_terminating_control(midiEvent))
                            add_gate_filtered(gate.retire_channel(midiEvent.channel(), midiEvent.micros));
                    }
                }
                if (should_retire_worker_at_dispatch_boundary()) return;
                add_gate_filtered(gate.take_filtered_event_count());
            }
            didWork = true;
            nowMicroseconds = ticks_to_micros(current_transport_ticks());
            if (examined >= 2048 || models::wrap_sub(now_stamp(), batchStarted) >= time_.frequency() / 125) break;
        }

        int64_t nextSourceAfterWork = !seekPending && nextArrival < event_count() ? event_micros(nextArrival) : kInt64Max;
        adaptive.complete_if_closed(gate.next_boundary_microseconds(), nextSourceAfterWork);
        publish_structural_due_work(nextArrival, 0, kInt64Max, false, kInt64Max, gate.next_boundary_microseconds(),
            gate.next_boundary_output_count());
        if (!seekPending && nextArrival >= event_count() && !sourceCompleted) {
            std::shared_ptr<const PlaybackSong> song;
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                song = song_;
            }
            gate.complete_source(!song ? nowMicroseconds : song->duration_us);
            add_gate_filtered(gate.take_filtered_event_count());
            sourceCompleted = true;
        }
        if ((seekPending || nextArrival >= event_count()) && !gate.has_pending_transitions()) return;

        int64_t targetMicroseconds = adaptive.effective_boundary(gate.next_boundary_microseconds());
        if (!seekPending && nextArrival < event_count())
            targetMicroseconds = min64(targetMicroseconds, event_micros(nextArrival));
        int64_t nowTicks = current_transport_ticks();
        int64_t targetTicks = micros_to_ticks(targetMicroseconds);
        publish_channel_state_before_wait(targetTicks, nowTicks);
        if (!didWork || targetTicks > nowTicks) wait_until(waiter, targetTicks, iterationWakeGeneration);
    }
}

// ---------------------------------------------------------------- Note start rate gate

void EngineCore::run_note_attack_limit(platform::Waiter& waiter)
{
    std::unique_ptr<models::MusicalAttackRateLimiter> limiterPointer(
        new models::MusicalAttackRateLimiter(events_per_second()));
    models::MusicalAttackRateLimiter& limiter = *limiterPointer;
    std::vector<EventView> emissions;
    emissions.reserve(128);
    int32_t nextArrival = start_event_index_;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t disableGenerations[16];
    for (int channel = 0; channel < 16; ++channel) disableGenerations[channel] = channel_routing_.disable_generation(channel);
    FilteredSourceEvents sourceFilters;

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t nowMicroseconds = ticks_to_micros(current_transport_ticks());

        if (seekPending) return;

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        if (currentRoutingGeneration != routingGeneration) {
            for (int channel = 0; channel < 16; ++channel) {
                int32_t disabledAt = channel_routing_.disable_generation(channel);
                if (disabledAt == disableGenerations[channel]) continue;
                disableGenerations[channel] = disabledAt;
                limiter.retire_channel(channel);
            }
            routingGeneration = currentRoutingGeneration;
            publish_channel_state(true);
        }

        bool didWork = false;
        int32_t examined = 0;
        int64_t batchStarted = now_stamp();
        while (nextArrival < event_count() && is_playing() && dispatch_suspended_.load() == 0 &&
            event_micros(nextArrival) <= nowMicroseconds) {
            int64_t sourceTick = event_at(nextArrival).tick;
            int64_t sourceTime = event_micros(nextArrival);
            limiter.begin_source_tick(sourceTick);
            while (nextArrival < event_count() && event_at(nextArrival).tick == sourceTick) {
                int32_t eventIndex = nextArrival++;
                examined++;
                if (has_active_source_filters() && mark_new_source_filtered(eventIndex, sourceFilters)) continue;
                limiter.admit(event_at(eventIndex));
            }
            int32_t count = limiter.end_source_tick(emissions);
            int64_t rejected = limiter.take_rejected_attacks();
            int64_t suppressed = limiter.take_suppressed_releases();
            add_musical_limit_exclusions(rejected, suppressed);
            for (int32_t index = 0; index < count; ++index) {
                if (should_retire_worker_at_dispatch_boundary()) return;
                publish_structural_due_work(nextArrival, count - index - 1, sourceTime, true, sourceTime, kInt64Max, 0);
                dispatch_midi_event(emissions[static_cast<size_t>(index)]);
                publish_structural_due_work(nextArrival, count - index - 1, sourceTime, false, kInt64Max, kInt64Max, 0);
            }
            advance_gate_effective_speed_frontier(sourceTime);
            didWork = true;
            nowMicroseconds = ticks_to_micros(current_transport_ticks());
            if (examined >= 2048 || models::wrap_sub(now_stamp(), batchStarted) >= time_.frequency() / 125) break;
        }
        publish_structural_due_work(nextArrival, 0, kInt64Max, false, kInt64Max, kInt64Max, 0);
        if (nextArrival >= event_count()) {
            limiter.complete_source();
            return;
        }
        int64_t nowTicks = current_transport_ticks();
        int64_t targetTicks = micros_to_ticks(event_micros(nextArrival));
        publish_channel_state_before_wait(targetTicks, nowTicks);
        if (!didWork || targetTicks > nowTicks) wait_until(waiter, targetTicks, iterationWakeGeneration);
    }
}

// ---------------------------------------------------------------- Output rate gate

void EngineCore::run_output_rate_limit(platform::Waiter& waiter)
{
    std::unique_ptr<models::OutputRateGate> gate(new models::OutputRateGate(events_per_second(), 1000000));
    active_output_rate_gate_.store(gate.get());
    try {
        run_output_rate_limit_body(waiter, *gate);
    }
    catch (...) {
        models::OutputRateGate* expected = gate.get();
        active_output_rate_gate_.compare_exchange_strong(expected, nullptr);
        throw;
    }
    models::OutputRateGate* expected = gate.get();
    active_output_rate_gate_.compare_exchange_strong(expected, nullptr);
}

void EngineCore::run_output_rate_limit_body(platform::Waiter& waiter, models::OutputRateGate& gate)
{
    std::vector<EventView> emissions;
    emissions.reserve(128);
    int32_t nextArrival = start_event_index_;
    int32_t routingGeneration = channel_routing_.filter_generation();
    int32_t disableGenerations[16];
    for (int channel = 0; channel < 16; ++channel) disableGenerations[channel] = channel_routing_.disable_generation(channel);
    FilteredSourceEvents sourceFilters;

    while (is_active()) {
        int32_t iterationWakeGeneration = wake_generation();
        apply_pending_channel_controls();
        if (!wait_while_paused()) return;
        bool seekPending = has_preserved_seek_request();
        if (!seekPending) apply_pending_state_chase();
        apply_pending_overrides();
        int64_t nowMicroseconds = ticks_to_micros(current_transport_ticks());

        if (seekPending) return;

        int32_t currentRoutingGeneration = channel_routing_.filter_generation();
        if (currentRoutingGeneration != routingGeneration) {
            for (int channel = 0; channel < 16; ++channel) {
                int32_t disabledAt = channel_routing_.disable_generation(channel);
                if (disabledAt == disableGenerations[channel]) continue;
                disableGenerations[channel] = disabledAt;
                gate.retire_channel(channel);
            }
            routingGeneration = currentRoutingGeneration;
            publish_channel_state(true);
        }

        bool didWork = false;
        int32_t examined = 0;
        int64_t batchStarted = now_stamp();
        while (nextArrival < event_count() && is_playing() && dispatch_suspended_.load() == 0 &&
            event_micros(nextArrival) <= nowMicroseconds) {
            int64_t sourceTick = event_at(nextArrival).tick;
            int64_t sourceTime = event_micros(nextArrival);
            gate.begin_source_tick(sourceTick);
            while (nextArrival < event_count() && event_at(nextArrival).tick == sourceTick) {
                int32_t eventIndex = nextArrival++;
                examined++;
                if (has_active_source_filters() && mark_new_source_filtered(eventIndex, sourceFilters)) continue;
                gate.admit(event_at(eventIndex));
            }
            int32_t count = gate.end_source_tick(emissions, ticks_to_micros(now_stamp()));
            int64_t rejected = gate.take_rejected_attacks();
            int64_t suppressed = gate.take_suppressed_releases();
            int64_t other = gate.take_rejected_other_messages();
            add_output_rate_exclusions(rejected, suppressed, other);
            for (int32_t index = 0; index < count; ++index) {
                if (should_retire_worker_at_dispatch_boundary()) return;
                publish_structural_due_work(nextArrival, count - index - 1, sourceTime, true, sourceTime, kInt64Max, 0);
                dispatch_midi_event(emissions[static_cast<size_t>(index)]);
                publish_structural_due_work(nextArrival, count - index - 1, sourceTime, false, kInt64Max, kInt64Max, 0);
            }
            advance_gate_effective_speed_frontier(sourceTime);
            didWork = true;
            nowMicroseconds = ticks_to_micros(current_transport_ticks());
            if (examined >= 2048 || models::wrap_sub(now_stamp(), batchStarted) >= time_.frequency() / 125) break;
        }
        publish_structural_due_work(nextArrival, 0, kInt64Max, false, kInt64Max, kInt64Max, 0);
        if (nextArrival >= event_count()) {
            gate.complete_source();
            int64_t rejected = gate.take_rejected_attacks();
            int64_t suppressed = gate.take_suppressed_releases();
            int64_t other = gate.take_rejected_other_messages();
            add_output_rate_exclusions(rejected, suppressed, other);
            return;
        }
        int64_t nowTicks = current_transport_ticks();
        int64_t targetTicks = micros_to_ticks(event_micros(nextArrival));
        publish_channel_state_before_wait(targetTicks, nowTicks);
        if (!didWork || targetTicks > nowTicks) wait_until(waiter, targetTicks, iterationWakeGeneration);
    }
}

void EngineCore::add_musical_limit_exclusions(int64_t rejectedAttacks, int64_t suppressedReleases)
{
    if (rejectedAttacks == 0 && suppressedReleases == 0) return;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    musical_limit_rejected_attacks_ += rejectedAttacks;
    musical_limit_suppressed_releases_ += suppressedReleases;
}

void EngineCore::add_output_rate_exclusions(int64_t rejectedAttacks, int64_t suppressedReleases,
    int64_t rejectedOtherMessages)
{
    if (rejectedAttacks == 0 && suppressedReleases == 0 && rejectedOtherMessages == 0) return;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    output_rate_rejected_attacks_ += rejectedAttacks;
    output_rate_suppressed_releases_ += suppressedReleases;
    output_rate_rejected_other_messages_ += rejectedOtherMessages;
}

void EngineCore::add_gate_filtered(int64_t count)
{
    if (count == 0) return;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    gate_filtered_events_ += count;
}

void EngineCore::advance_gate_effective_speed_frontier(int64_t sourceMicroseconds)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    int64_t duration = !song_ ? sourceMicroseconds : song_->duration_us;
    int64_t resolved = min64(sourceMicroseconds, duration);
    if (resolved > effective_speed_frontier_microseconds_) effective_speed_frontier_microseconds_ = resolved;
}

// ---------------------------------------------------------------- dispatch

void EngineCore::dispatch(int32_t eventIndex, int64_t actualTransportTicks)
{
    int64_t providerSendStart;
    int64_t providerSendEnd;
    dispatch(eventIndex, actualTransportTicks, providerSendStart, providerSendEnd);
}

void EngineCore::dispatch(int32_t eventIndex, int64_t, int64_t& providerSendStart, int64_t& providerSendEnd)
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        published_in_service_event_ = eventIndex;
    }
    dispatch_midi_event(event_at(eventIndex), providerSendStart, providerSendEnd);
}

void EngineCore::dispatch_midi_event(const EventView& midiEvent)
{
    int64_t providerSendStart;
    int64_t providerSendEnd;
    dispatch_midi_event(midiEvent, providerSendStart, providerSendEnd);
}

void EngineCore::dispatch_midi_event(const EventView& midiEvent, int64_t& providerSendStart, int64_t& providerSendEnd)
{
    send_scheduled_event(midiEvent, channel_monitoring_enabled_.load() != 0, providerSendStart, providerSendEnd);
    int64_t actualUs = ticks_to_micros(current_transport_ticks());
    int64_t lag = max64(0, models::wrap_sub(actualUs, midiEvent.micros));
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        processed_events_++;
        last_dispatched_microseconds_ = midiEvent.micros;
        current_lag_microseconds_ = lag;
        if (lag > maximum_lag_microseconds_) maximum_lag_microseconds_ = lag;
    }
    publish_channel_state(false);
}

void EngineCore::dispatch_immediate_range(int32_t& nextIndex, int32_t endExclusive, int64_t transportAtBatchStart,
    const std::shared_ptr<const ServiceDurationSettings>& serviceSettings)
{
    dispatch_immediate_burst(nextIndex, endExclusive, transportAtBatchStart, nullptr, nullptr, serviceSettings, false);
}

void EngineCore::dispatch_immediate_filtered_range(int32_t& nextIndex, int32_t endExclusive,
    int64_t transportAtBatchStart, FilteredSourceEvents& filtered, int64_t& eligiblePending,
    const std::shared_ptr<const ServiceDurationSettings>& serviceSettings, bool classifySourceAtDispatch)
{
    dispatch_immediate_burst(nextIndex, endExclusive, transportAtBatchStart, &filtered, &eligiblePending,
        serviceSettings, classifySourceAtDispatch);
}

// Sends the events that are already due, back to back (register entry DD-065). Per event the loop does only what
// must happen per event: the stop/settings checks, the filter check, one relaxed store of the event being sent (the
// published "in service" state), and the send itself. The clock is read once per group of up to 16 events, not once
// per event: after each group the loop computes the lag (current: of the group's last event; maximum: of its first,
// the oldest, both measured at the end of the group, so never smaller than with a read per event and larger by at
// most the group's duration), and decides the size of the next group from how long this one took. A group is meant
// to last about 2 microseconds: a fast output gives groups of 16, a slow device (tens of microseconds per call) gives
// groups of one, which is exactly the former per-event behaviour (the lock-step harness's outputs take 320
// microseconds of virtual time per message, so there every group is one event). The burst still ends after 2048
// events or 8 ms.
void EngineCore::dispatch_immediate_burst(int32_t& nextIndex, int32_t endExclusive, int64_t transportAtBatchStart,
    FilteredSourceEvents* filtered, int64_t* eligiblePending,
    const std::shared_ptr<const ServiceDurationSettings>& serviceSettings, bool classifySourceAtDispatch)
{
    const int32_t chunkSize = 2048;
    const int32_t maximumGroup = 16;
    int64_t stopwatchAtBatchStart = now_stamp();
    if (nextIndex >= endExclusive || dispatch_suspended_.load() != 0) return;
    const int64_t burstLimitTicks = time_.frequency() / 125;                         // 8 ms
    const int64_t groupBudgetTicks = max64(1, time_.frequency() / 500000);           // about 2 microseconds
    int32_t examined = 0;
    int32_t sent = 0;
    int64_t lastTimeline = 0;
    int64_t currentLag = 0;
    int64_t maximumLag = 0;
    bool trackChannels = channel_monitoring_enabled_.load() != 0;
    // Without an observer or a derived (routed) event view, an event is sent straight from its stored form; otherwise
    // it is decoded to an EventView first, as before. Channel monitoring no longer forces the decoded form: what the
    // monitor records is taken from the stored form too (DD-092).
    const bool direct = !send_observer_ && events_.is_compact();
    // With channel monitoring on, the group's sends are recorded together when the group closes: one tracker lock per
    // group instead of one per message, and the monitor's publication check uses the group's clock reading instead of
    // reading the clock before every send (DD-092). A group holds at most `maximumGroup` sends.
    ChannelStateTracker::SentRecord sentRecords[maximumGroup];
    int32_t sentRecordCount = 0;
    uint32_t settingsGeneration = settings_generation() - 1;   // the first event compares the pointers itself
    bool burstOpen = false;
    int32_t groupLimit = 1;
    int32_t inGroup = 0;
    int64_t groupFirstTimeline = 0;
    int64_t previousStamp = stopwatchAtBatchStart;
    auto recordGroup = [&]() {
        if (sentRecordCount == 0) return;
        int32_t count = sentRecordCount;
        sentRecordCount = 0;
        channel_state_.record_successful_batch(sentRecords, count);
    };
    auto closeGroup = [&]() -> int64_t {
        recordGroup();
        int64_t stamp = now_stamp();
        int64_t elapsedTicks = models::wrap_sub(stamp, stopwatchAtBatchStart);
        int64_t actualMicroseconds = ticks_to_micros(models::wrap_add(transportAtBatchStart, elapsedTicks));
        currentLag = max64(0, models::wrap_sub(actualMicroseconds, lastTimeline));
        int64_t oldestLag = max64(0, models::wrap_sub(actualMicroseconds, groupFirstTimeline));
        if (oldestLag > maximumLag) maximumLag = oldestLag;
        if (currentLag > maximumLag) maximumLag = currentLag;
        int64_t groupTicks = max64(1, models::wrap_sub(stamp, previousStamp));
        previousStamp = stamp;
        int64_t wanted = static_cast<int64_t>(inGroup) * groupBudgetTicks / groupTicks;
        groupLimit = wanted < 1 ? 1 : (wanted > maximumGroup ? maximumGroup : static_cast<int32_t>(wanted));
        inGroup = 0;
        if (trackChannels) publish_channel_state_at(stamp, false);
        return elapsedTicks;
    };
    auto finish = [&] {
        if (sent > 0 || burstOpen) {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (sent > 0) {
                processed_events_ += sent;
                last_dispatched_microseconds_ = lastTimeline;
                current_lag_microseconds_ = currentLag;
                if (maximumLag > maximum_lag_microseconds_) maximum_lag_microseconds_ = maximumLag;
            }
            if (burstOpen) end_immediate_burst_locked();
        }
        publish_channel_state(false);
    };
    // DD-110: the output may guard its calls once for the whole burst (at most 2048 sends or 8 ms) instead of once
    // per message; the batch ends before the bookkeeping below, which takes the engine lock.
    bool batching = output_ != nullptr;
    if (batching) output_->begin_send_batch();
    auto endBatch = [&] {
        if (!batching) return;
        batching = false;
        output_->end_send_batch();
    };
    try {
        while (nextIndex < endExclusive && examined < chunkSize) {
            // A native call can remain blocked while Stop, Seek, or Pause is
            // requested; observe that before every subsequent send.
            if (dispatch_suspended_.load() != 0) break;
            // A new settings object is noticed by its generation number; the pointer comparison confirms it.
            if (settings_generation() != settingsGeneration) {
                if (settings() != serviceSettings) break;
                settingsGeneration = settings_generation();
            }
            int32_t eventIndex = nextIndex++;
            examined++;
            if (filtered != nullptr) {
                if (classifySourceAtDispatch) {
                    if (mark_source_filtered(eventIndex, *filtered)) continue;
                }
                else if (filtered->any() && filtered->contains(eventIndex)) continue;
            }
            if (!burstOpen) {
                begin_immediate_burst(nextIndex, eventIndex);
                burstOpen = true;
            }
            else burst_event_.store(eventIndex, std::memory_order_relaxed);
            int64_t intendedMicroseconds = send_burst_event(eventIndex, direct,
                trackChannels ? &sentRecords[sentRecordCount] : nullptr);
            if (trackChannels) ++sentRecordCount;
            if (eligiblePending != nullptr && *eligiblePending > 0) --*eligiblePending;
            if (inGroup == 0) groupFirstTimeline = intendedMicroseconds;
            lastTimeline = intendedMicroseconds;
            sent++;
            inGroup++;
            // Return to admission/publication after at most 2048 sends or 8 ms.
            if (inGroup >= groupLimit && closeGroup() >= burstLimitTicks) break;
        }
        endBatch();
        if (inGroup > 0) closeGroup();
    }
    catch (...) {
        endBatch();
        if (inGroup > 0) {
            try { closeGroup(); } catch (...) {}
        }
        finish();
        throw;
    }
    finish();
}

// One send of an immediate burst; returns the event's intended time. The direct form reads the stored record and
// builds the three message bytes without an EventView; the other form is the former send_scheduled_event.
int64_t EngineCore::send_burst_event(int32_t eventIndex, bool direct, ChannelStateTracker::SentRecord* sent)
{
    if (direct) {
        const PackedRecord& record = events_.store->records.packed_at(eventIndex);
        if (!record.is_long() && record.kind() != EventKind::SystemExclusive) {
            // The same message bytes EventView::from_store builds: the first `length` bytes of the inline word.
            static const uint32_t kInlineMask[4] = { 0u, 0xFFu, 0xFFFFu, 0xFFFFFFu };
            const int length = record.inline_length();
            const bool pacedHold = output_paced_timeline_.load() != 0 && begin_output_paced_hold();
            yield_if_behind();
            if (length != 0) {   // v1 returns before the send for a message without data
                try {
                    // The cheap form: a status with text is built only when the send fails.
                    if (!output_->try_send_short(record.packed & kInlineMask[length], send_failure_)) check_output(send_failure_);
                }
                catch (...) {
                    if (pacedHold) end_output_paced_hold(true);
                    throw;
                }
            }
            if (pacedHold) end_output_paced_hold(true);
            if (active_output_rate_gate_.load() != nullptr)
                record_output_rate_dispatch(event_at(eventIndex), ticks_to_micros(now_stamp()), false);
            if (sent != nullptr) *sent = ChannelStateTracker::sent_record(record);
            return record.micros;
        }
    }
    EventView midiEvent = event_at(eventIndex);
    send_scheduled_event(midiEvent, false);
    if (sent != nullptr) *sent = ChannelStateTracker::sent_record(midiEvent);
    return midiEvent.micros;
}

void EngineCore::retire_drop_filtered_backlog(FilteredSourceEvents& filtered, PendingMidiQueue& pending,
    CompleteNoteTracker& completeNotes, int32_t& inService, int64_t& completionTicks, int64_t now,
    ServiceDurationClock& serviceClock, ServiceDurationClock& beforeCurrentService)
{
    bool changed = false;
    if (inService >= 0 && mark_source_filtered(inService, filtered)) {
        if (CompleteNoteTracker::classify(event_at(inService)) == CompleteNoteEventKind::NoteOn)
            completeNotes.mark_unsent_attack_filtered(inService);
        inService = -1;
        completionTicks = kInt64Max;
        serviceClock = beforeCurrentService;
        changed = true;
    }
    int32_t pendingCount = pending.count();
    for (int32_t i = 0; i < pendingCount; ++i) {
        PendingMidiQueue::Entry entry = pending.dequeue_entry();
        int32_t eventIndex = entry.event_index;
        if (mark_source_filtered(eventIndex, filtered)) {
            if (CompleteNoteTracker::classify(event_at(eventIndex)) == CompleteNoteEventKind::NoteOn)
                completeNotes.mark_unsent_attack_filtered(eventIndex);
            changed = true;
        }
        else pending.enqueue(entry);
    }
    if (inService < 0 && pending.count() > 0) {
        inService = pending.dequeue();
        beforeCurrentService = serviceClock;
        completionTicks = models::checked_add(now,
            micros_to_ticks(next_playback_service_microseconds(serviceClock, event_at(inService))));
    }
    if (changed) {
        update_queue(pending.count(), inService >= 0);
        publish_channel_state(true);
    }
}

void EngineCore::admit_unlimited_range(int32_t startInclusive, int32_t endExclusive, FilteredSourceEvents& filtered,
    int64_t& eligiblePending)
{
    if (startInclusive >= endExclusive) return;
    if (!has_active_source_filters()) {
        eligiblePending += static_cast<int64_t>(endExclusive) - startInclusive;
        return;
    }
    for (int32_t index = startInclusive; index < endExclusive; ++index)
        if (!mark_new_source_filtered(index, filtered)) eligiblePending++;
    publish_channel_state(false);
}

void EngineCore::retire_unlimited_filtered_backlog(int32_t nextProcess, int32_t nextArrival,
    FilteredSourceEvents& filtered, int64_t& eligiblePending, int32_t& inService, int64_t& completionTicks,
    int64_t& lastCompletionTicks, int64_t now, ServiceDurationClock& serviceClock,
    ServiceDurationClock& beforeCurrentService)
{
    bool changed = false;
    if (inService >= 0 && mark_source_filtered(inService, filtered)) {
        inService = -1;
        completionTicks = 0;
        lastCompletionTicks = now;
        serviceClock = beforeCurrentService;
        changed = true;
    }
    for (int32_t index = nextProcess; index < nextArrival; ++index) {
        if (filtered.contains(index)) continue;
        if (mark_source_filtered(index, filtered)) {
            if (eligiblePending > 0) eligiblePending--;
            changed = true;
        }
    }
    if (changed) publish_channel_state(true);
}

int32_t EngineCore::take_next_eligible(int32_t& nextProcess, int32_t nextArrival, FilteredSourceEvents& filtered)
{
    while (nextProcess < nextArrival) {
        int32_t candidate = nextProcess++;
        if (!filtered.contains(candidate)) return candidate;
    }
    return -1;
}

} // namespace engine
} // namespace midibn
