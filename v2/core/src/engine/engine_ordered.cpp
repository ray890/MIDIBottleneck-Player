// v1 PlaybackEngine.Ordered.cs: the preserve-on-Pause/Seek ("ordered")
// processing modes over ordered playback generations, including the
// Output-paced timeline paths.
#include "engine_internal.h"

namespace midibn {
namespace engine {

using models::EventView;
using models::kInt32Max;
using models::kInt64Max;
using models::max64;
using models::min64;
using models::OutputWorkItem;
using models::OutputWorkItemKind;
using models::PlaybackGeneration;
using models::ServiceDurationClock;

namespace {

const int32_t kMaximumLiveGenerations = 256 + 1;  // v1 MaximumPendingPreservedSeeks + 1

bool is_boundary_work(const OutputWorkItem& item)
{
    return item.kind == OutputWorkItemKind::ProtectedSeekMarker || item.kind == OutputWorkItemKind::ForcedOrChaseMidi;
}

} // namespace

models::CaptureDecision EngineCore::capture_decision()
{
    return [this](const EventView& e) { return capture_ordered_finite_decision(e); };
}

models::CaptureDecision EngineCore::preview_decision()
{
    return [this](const EventView& e) { return preview_ordered_finite_decision(e); };
}

// ---------------------------------------------------------------- unlimited Queue

void EngineCore::run_ordered_unlimited_queue_mode(platform::Waiter& waiter)
{
    OrderedUnlimitedQueueAdapter adapter(events_, kMaximumLiveGenerations);
    int64_t clockNow = now_stamp();
    int64_t baseTicks;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        baseTicks = transport_base_ticks_;
    }
    int64_t targetMicroseconds = ticks_to_micros(baseTicks);
    int32_t startIndex = start_event_index_;
    std::vector<EventView> prefix = capture_ordered_generation_prefix(startIndex, ticks_to_micros(baseTicks));
    adapter.start(targetMicroseconds, startIndex, prefix, clockNow, 0);

    std::shared_ptr<const ServiceDurationSettings> serviceSettings = settings();
    ServiceDurationClock serviceClock = serviceSettings->create_clock();
    OutputWorkItem inService;
    bool hasInService = false;
    int64_t completionStamp = kInt64Max;
    int64_t lastCompletionStamp = 0;
    bool serviceHold = false;
    int64_t pacedServiceTicks = 0;

    while (is_active()) {
        int32_t wakeGeneration = wake_generation();
        int64_t pauseStarted = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (state_ == PlaybackState::Paused) pauseStarted = now_stamp();
        }
        if (!wait_while_paused()) return;
        if (pauseStarted != 0) {
            int64_t pausedTicks = max64(0, models::wrap_sub(now_stamp(), pauseStarted));
            adapter.shift_source_clock_anchors(pausedTicks);
            if (hasInService && completionStamp < kInt64Max) completionStamp = saturating_add_ticks(completionStamp, pausedTicks);
            if (lastCompletionStamp != 0) lastCompletionStamp = saturating_add_ticks(lastCompletionStamp, pausedTicks);
            if (hasInService && output_paced_timeline()) serviceHold = begin_output_paced_hold();
        }

        int64_t now = now_stamp();
        bool controlBoundary = has_pending_ordered_live_control_boundary();
        if (!controlBoundary && (!output_paced_timeline() || !hasInService)) {
            admit_ordered_unlimited_due(adapter, now);
            append_ordered_seek_requests(adapter, now);
            admit_ordered_unlimited_due(adapter, now);
        }
        else if (!hasInService && adapter.pending_work_count() == 0) {
            apply_ordered_live_control_boundary();
            continue;
        }
        publish_ordered_unlimited_pressure(adapter, hasInService);

        if (hasInService && completionStamp <= now) {
            int64_t sendStarted = now_stamp();
            dispatch_ordered_work_item(inService);
            int64_t sendTicks = max64(0, models::wrap_sub(now_stamp(), sendStarted));
            end_output_paced_hold(serviceHold);
            serviceHold = false;
            if (output_paced_timeline())
                adapter.shift_source_clock_anchors(saturating_add_ticks(pacedServiceTicks, sendTicks));
            hasInService = false;
            lastCompletionStamp = completionStamp;
            continue;
        }

        if (!hasInService) {
            OutputWorkItem next;
            if (adapter.try_peek(next)) {
                if (is_boundary_work(next) || simulate_slowdown_.load() == 0) {
                    adapter.try_dequeue(next);
                    publish_ordered_unlimited_pressure(adapter, true);
                    int64_t sendStarted = now_stamp();
                    dispatch_ordered_work_item(next);
                    if (output_paced_timeline())
                        adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), sendStarted)));
                    continue;
                }

                adapter.try_dequeue(inService);
                hasInService = true;
                update_service_clock(serviceClock, serviceSettings);
                int64_t dueStamp = adapter.dispatch_due_clock_ticks(inService, time_);
                int64_t startStamp = max64(dueStamp, lastCompletionStamp);
                pacedServiceTicks = micros_to_ticks(serviceClock.next_microseconds(inService.midi_event));
                completionStamp = saturating_add_ticks(startStamp, pacedServiceTicks);
                serviceHold = begin_output_paced_hold();
                publish_ordered_unlimited_pressure(adapter, true);
                continue;
            }
        }

        PlaybackGeneration* tail = adapter.tail();
        if (!hasInService && adapter.pending_work_count() == 0 && tail != nullptr && !tail->is_admission_closed() &&
            tail->source_event_cursor >= event_count() && !has_preserved_seek_request())
            return;

        int64_t targetStamp = hasInService ? completionStamp : kInt64Max;
        tail = adapter.tail();
        if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
            int64_t deltaUs = max64(0, models::wrap_sub(event_micros(tail->source_event_cursor),
                tail->target_source_microseconds));
            targetStamp = min64(targetStamp, saturating_add_ticks(tail->source_clock_anchor_ticks, micros_to_ticks(deltaUs)));
        }
        if (targetStamp == kInt64Max) targetStamp = models::wrap_add(now, time_.frequency() / 100);
        wait_until_stopwatch(waiter, targetStamp, wakeGeneration);
    }
}

// ---------------------------------------------------------------- finite queue (Drop)

void EngineCore::run_ordered_finite_queue_mode(platform::Waiter& waiter)
{
    models::OrderedFiniteAdmissionCore core(events_, kMaximumLiveGenerations);
    int64_t clockNow = now_stamp();
    int64_t baseTicks;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        baseTicks = transport_base_ticks_;
    }
    int64_t startMicroseconds = ticks_to_micros(baseTicks);
    int32_t startIndex = start_event_index_;
    std::vector<EventView> prefix = capture_ordered_generation_prefix(startIndex, startMicroseconds);
    core.start(startMicroseconds, startIndex, &prefix, clockNow, 0);
    std::shared_ptr<const ServiceDurationSettings> serviceSettings = settings();
    ServiceDurationClock serviceClock = serviceSettings->create_clock();
    OutputWorkItem inService;
    bool hasInService = false;
    int64_t completionStamp = kInt64Max;
    int64_t lastCompletionStamp = 0;
    bool serviceHold = false;
    int64_t pacedServiceTicks = 0;

    while (is_active()) {
        int32_t wakeGeneration = wake_generation();
        int64_t pauseStarted = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (state_ == PlaybackState::Paused) pauseStarted = now_stamp();
        }
        if (!wait_while_paused()) return;
        if (pauseStarted != 0) {
            int64_t pausedTicks = max64(0, models::wrap_sub(now_stamp(), pauseStarted));
            core.shift_source_clock_anchors(pausedTicks);
            if (hasInService && completionStamp < kInt64Max) completionStamp = saturating_add_ticks(completionStamp, pausedTicks);
            if (lastCompletionStamp != 0) lastCompletionStamp = saturating_add_ticks(lastCompletionStamp, pausedTicks);
            if (hasInService && output_paced_timeline()) serviceHold = begin_output_paced_hold();
        }

        int64_t now = now_stamp();
        bool controlBoundary = has_pending_ordered_live_control_boundary();
        if (!controlBoundary && (!output_paced_timeline() || !hasInService)) {
            admit_ordered_finite_due(core, now);
            append_ordered_finite_seek_requests(core, now);
            admit_ordered_finite_due(core, now);
        }
        else if (!hasInService && core.pending_count() == 0) {
            apply_ordered_live_control_boundary();
            continue;
        }
        repair_and_publish_ordered_finite_pressure(core, hasInService);

        if (hasInService && completionStamp <= now) {
            int64_t sendStarted = now_stamp();
            dispatch_ordered_work_item(inService);
            int64_t sendTicks = max64(0, models::wrap_sub(now_stamp(), sendStarted));
            end_output_paced_hold(serviceHold);
            serviceHold = false;
            if (output_paced_timeline())
                core.shift_source_clock_anchors(saturating_add_ticks(pacedServiceTicks, sendTicks));
            core.complete_dispatch();
            hasInService = false;
            lastCompletionStamp = completionStamp;
            continue;
        }

        if (!hasInService) {
            OutputWorkItem next;
            if (core.try_peek_dispatch(next)) {
                if (!core.try_begin_dispatch(inService))
                    raise("InvalidOperationException", "Ordered finite dispatch lost its head item.");
                hasInService = true;
                if (is_boundary_work(inService) || simulate_slowdown_.load() == 0) {
                    int64_t sendStarted = now_stamp();
                    dispatch_ordered_work_item(inService);
                    if (output_paced_timeline())
                        core.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), sendStarted)));
                    core.complete_dispatch();
                    hasInService = false;
                    continue;
                }
                update_service_clock(serviceClock, serviceSettings);
                int64_t dueStamp = dispatch_due_clock_ticks(core.generations(), inService, time_);
                int64_t startStamp = max64(dueStamp, lastCompletionStamp);
                pacedServiceTicks = micros_to_ticks(serviceClock.next_microseconds(inService.midi_event));
                completionStamp = saturating_add_ticks(startStamp, pacedServiceTicks);
                serviceHold = begin_output_paced_hold();
                repair_and_publish_ordered_finite_pressure(core, true);
                continue;
            }
        }

        PlaybackGeneration* tail = core.tail();
        if (!hasInService && core.pending_count() == 0 && tail != nullptr && !tail->is_admission_closed() &&
            tail->source_event_cursor >= event_count() && !has_preserved_seek_request()) return;

        int64_t targetStamp = hasInService ? completionStamp : kInt64Max;
        if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
            int64_t deltaUs = max64(0, models::wrap_sub(event_micros(tail->source_event_cursor),
                tail->target_source_microseconds));
            targetStamp = min64(targetStamp, saturating_add_ticks(tail->source_clock_anchor_ticks, micros_to_ticks(deltaUs)));
        }
        if (targetStamp == kInt64Max) targetStamp = models::wrap_add(now, time_.frequency() / 100);
        wait_until_stopwatch(waiter, targetStamp, wakeGeneration);
    }
}

// ---------------------------------------------------------------- forward-only virtual queue

void EngineCore::run_ordered_virtual_forward_mode(platform::Waiter& waiter)
{
    OrderedVirtualForwardAdapter adapter(events_, kMaximumLiveGenerations);
    int64_t baseTicks;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        baseTicks = transport_base_ticks_;
    }
    int64_t startMicroseconds = ticks_to_micros(baseTicks);
    int32_t startIndex = start_event_index_;
    std::vector<EventView> prefix = capture_ordered_generation_prefix(startIndex, startMicroseconds);
    int64_t anchor = now_stamp();
    int32_t capacity = queue_length_limit_.load();
    adapter.start(startMicroseconds, startIndex, prefix, anchor, 0, capacity < 1 ? 1 : capacity);
    std::shared_ptr<const ServiceDurationSettings> serviceSettings = settings();
    ServiceDurationClock serviceClock = serviceSettings->create_clock();
    int64_t reportedDrops = 0;

    while (is_active()) {
        int32_t wakeGeneration = wake_generation();
        int64_t pauseStarted = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (state_ == PlaybackState::Paused) pauseStarted = now_stamp();
        }
        if (!wait_while_paused()) return;
        if (pauseStarted != 0) adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), pauseStarted)));

        bool controlBoundary = has_pending_ordered_live_control_boundary();
        if (controlBoundary && adapter.pending_boundary_count() == 0) {
            apply_ordered_live_control_boundary();
            continue;
        }

        OutputWorkItem boundary;
        if (adapter.try_begin_boundary_dispatch(boundary)) {
            int64_t sendStarted = now_stamp();
            dispatch_ordered_work_item(boundary);
            if (output_paced_timeline())
                adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), sendStarted)));
            adapter.complete_boundary_dispatch();
            publish_ordered_virtual_pressure(adapter);
            continue;
        }

        int64_t now = now_stamp();
        PlaybackGeneration* tail = adapter.tail();
        if (!controlBoundary && adapter.can_admit_tail_source() && tail != nullptr &&
            tail->source_event_cursor < event_count()) {
            EventView midiEvent = event_at(tail->source_event_cursor);
            int64_t deltaUs = max64(0, models::wrap_sub(midiEvent.micros, tail->target_source_microseconds));
            int64_t dueStamp = saturating_add_ticks(tail->source_clock_anchor_ticks, micros_to_ticks(deltaUs));
            if (dueStamp <= now) {
                int64_t logicalNow = models::wrap_add(tail->target_source_microseconds,
                    ticks_to_micros(max64(0, models::wrap_sub(now, tail->source_clock_anchor_ticks))));
                adapter.advance_virtual_time(logicalNow);
                bool filtered = should_filter_ordered_source(tail->source_event_cursor, midiEvent);
                update_service_clock(serviceClock, serviceSettings);
                ServiceDurationClock admittedClock = serviceClock;
                int64_t service = admittedClock.next_microseconds(midiEvent);
                OrderedVirtualAdmissionResult result = adapter.admit_next_source(midiEvent, filtered, service,
                    overflow_policy());
                if (result.accepted()) {
                    serviceClock = admittedClock;
                    dispatch_ordered_work_item(result.work_item);
                }
                if (adapter.dropped_events() > reportedDrops) {
                    int64_t delta = adapter.dropped_events() - reportedDrops;
                    record_ordered_finite_drops(static_cast<int32_t>(delta < kInt32Max ? delta : kInt32Max));
                    reportedDrops = adapter.dropped_events();
                }
                publish_ordered_virtual_pressure(adapter);
                continue;
            }
        }

        PreservedSeekRequest request;
        if (!controlBoundary && adapter.can_admit_tail_source() && try_take_one_preserved_seek_request(request)) {
            channel_overrides_.mark_all_forced_pending();
            std::vector<EventView> seekPrefix = capture_ordered_generation_prefix(request.target_event_index,
                request.target_microseconds);
            int32_t seekCapacity = queue_length_limit_.load();
            if (!adapter.try_append_seek(request.target_microseconds, request.target_event_index, seekPrefix, now, 0,
                    seekCapacity < 1 ? 1 : seekCapacity))
                raise("InvalidOperationException", "Too many ordered playback generations are pending.");
            adopt_ordered_seek_target(request);
            continue;
        }

        tail = adapter.tail();
        if (adapter.pending_boundary_count() == 0 && adapter.can_admit_tail_source() && tail != nullptr &&
            tail->source_event_cursor >= event_count() && !has_preserved_seek_request()) return;

        int64_t targetStamp = models::wrap_add(now, time_.frequency() / 100);
        if (tail != nullptr && adapter.can_admit_tail_source() && tail->source_event_cursor < event_count()) {
            int64_t nextMicros = event_micros(tail->source_event_cursor);
            targetStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
                micros_to_ticks(max64(0, models::wrap_sub(nextMicros, tail->target_source_microseconds))));
        }
        wait_until_stopwatch(waiter, targetStamp, wakeGeneration);
    }
}

// ---------------------------------------------------------------- musical gates

void EngineCore::run_ordered_musical_gate_mode(platform::Waiter& waiter)
{
    int64_t interval = processing_microseconds();
    OrderedMusicalGateAdapter adapter(events_, kMaximumLiveGenerations, mode_, interval,
        scale_note_interval_by_traffic(), interval_polyphony_limit_enabled(), interval_polyphony_limit());
    int64_t baseTicks;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        baseTicks = transport_base_ticks_;
    }
    int64_t startMicroseconds = ticks_to_micros(baseTicks);
    int32_t startIndex = start_event_index_;
    std::vector<EventView> prefix = capture_ordered_generation_prefix(startIndex, startMicroseconds);
    adapter.start(startMicroseconds, startIndex, prefix, now_stamp(), 0);
    bool hasInService = false;
    OutputWorkItem inService;
    int64_t reportedFiltered = 0;

    while (is_active()) {
        int32_t wakeGeneration = wake_generation();
        int64_t pauseStarted = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (state_ == PlaybackState::Paused) pauseStarted = now_stamp();
        }
        if (!wait_while_paused()) return;
        if (pauseStarted != 0) adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), pauseStarted)));

        int64_t now = now_stamp();
        bool controlBoundary = has_pending_ordered_live_control_boundary();
        if (!controlBoundary) {
            admit_ordered_gate_due(adapter, now);
            append_ordered_gate_seek_requests(adapter, now);
            admit_ordered_gate_due(adapter, now);
        }
        else if (!hasInService && adapter.pending_count() == 0) {
            apply_ordered_live_control_boundary();
            continue;
        }
        if (adapter.filtered_events() > reportedFiltered) {
            add_gate_filtered(adapter.filtered_events() - reportedFiltered);
            reportedFiltered = adapter.filtered_events();
        }
        publish_ordered_gate_pressure(adapter, hasInService);

        if (!hasInService) {
            OutputWorkItem next;
            if (adapter.try_peek_dispatch(next)) {
                int64_t dueStamp = is_boundary_work(next) ? now : adapter.dispatch_due_clock_ticks(next, time_);
                if (dueStamp <= now) {
                    if (!adapter.try_begin_dispatch(inService))
                        raise("InvalidOperationException", "Ordered gate dispatch lost its head item.");
                    hasInService = true;
                    int64_t sendStarted = now_stamp();
                    dispatch_ordered_work_item(inService);
                    if (output_paced_timeline())
                        adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), sendStarted)));
                    adapter.complete_dispatch();
                    hasInService = false;
                    {
                        std::lock_guard<std::recursive_mutex> lock(sync_);
                        effective_speed_frontier_microseconds_ = max64(effective_speed_frontier_microseconds_,
                            inService.logical_output_microseconds);
                    }
                    continue;
                }
            }
        }

        PlaybackGeneration* tail = adapter.tail();
        if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor >= event_count() &&
            !adapter.tail_source_complete()) {
            int64_t duration;
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                duration = song_ ? song_->duration_us : 0;
            }
            adapter.complete_tail_source(max64(tail->target_source_microseconds, duration));
            continue;
        }
        if (!hasInService && adapter.pending_count() == 0 && adapter.tail_source_complete() &&
            !has_preserved_seek_request()) return;

        int64_t targetStamp = models::wrap_add(now, time_.frequency() / 100);
        OutputWorkItem pending;
        if (adapter.try_peek_dispatch(pending) && !is_boundary_work(pending))
            targetStamp = adapter.dispatch_due_clock_ticks(pending, time_);
        else if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
            int64_t nextMicros = event_micros(tail->source_event_cursor);
            targetStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
                micros_to_ticks(max64(0, models::wrap_sub(nextMicros, tail->target_source_microseconds))));
        }
        wait_until_stopwatch(waiter, targetStamp, wakeGeneration);
    }
}

void EngineCore::admit_ordered_gate_due(OrderedMusicalGateAdapter& adapter, int64_t nowStamp)
{
    PlaybackGeneration* tail = adapter.tail();
    while (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
        int64_t sourceMicros = event_micros(tail->source_event_cursor);
        int64_t dueStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
            micros_to_ticks(max64(0, models::wrap_sub(sourceMicros, tail->target_source_microseconds))));
        if (dueStamp > nowStamp) break;
        adapter.admit_next_source_unit(capture_decision());
        tail = adapter.tail();
    }
}

void EngineCore::append_ordered_gate_seek_requests(OrderedMusicalGateAdapter& adapter, int64_t nowStamp)
{
    while (true) {
        PreservedSeekRequest request;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (preserved_seek_requests_.empty()) return;
            request = preserved_seek_requests_.front();
            preserved_seek_requests_.pop_front();
        }
        admit_ordered_gate_due(adapter, nowStamp);
        PlaybackGeneration* closing = adapter.tail();
        if (closing == nullptr) raise("InvalidOperationException", "Playback lost track of its internal queue while seeking. Stop and start playback again.");
        int64_t closingNow = models::wrap_add(closing->target_source_microseconds,
            ticks_to_micros(max64(0, models::wrap_sub(nowStamp, closing->source_clock_anchor_ticks))));
        channel_overrides_.mark_all_forced_pending();
        std::vector<EventView> prefix = capture_ordered_generation_prefix(request.target_event_index,
            request.target_microseconds);
        if (!adapter.try_append_seek(closingNow, request.target_microseconds, request.target_event_index, prefix,
                nowStamp, 0))
            raise("InvalidOperationException", "Too many ordered playback generations are pending.");
        adopt_ordered_seek_target(request);
        admit_ordered_gate_due(adapter, nowStamp);
    }
}

void EngineCore::publish_ordered_gate_pressure(OrderedMusicalGateAdapter& adapter, bool hasInService)
{
    // The snapshot adds the source events that are already due but not yet admitted (from the next source event
    // on) to the admitted work, and takes the age from the oldest waiting item. v1 never published the next source
    // event, so it counted from event 0 (or from an earlier run's value) and reported a wrong queue length,
    // outstanding count and age (register entry DD-009).
    PlaybackGeneration* tail = adapter.tail();
    int32_t nextSource = tail != nullptr ? tail->source_event_cursor : event_count();
    if (nextSource > event_count()) nextSource = event_count();
    OutputWorkItem head;
    int64_t oldestDue = adapter.pending_count() > 0 && adapter.try_peek_dispatch(head) ? head.logical_output_microseconds
                                                                                        : kInt64Max;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    has_published_structural_due_work_ = true;
    queue_length_ = adapter.pending_count();
    outstanding_events_ = queue_length_ + (hasInService ? 1 : 0);
    published_structural_next_arrival_ = nextSource;
    published_structural_oldest_due_microseconds_ = oldestDue;
    published_structural_pending_ = adapter.pending_count();
    published_structural_in_service_ = hasInService;
    if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
}

// ---------------------------------------------------------------- note start rate

void EngineCore::run_ordered_note_attack_mode(platform::Waiter& waiter)
{
    OrderedNoteAttackAdapter adapter(events_, kMaximumLiveGenerations, events_per_second());
    int64_t baseTicks;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        baseTicks = transport_base_ticks_;
    }
    int64_t startMicroseconds = ticks_to_micros(baseTicks);
    int32_t startIndex = start_event_index_;
    std::vector<EventView> prefix = capture_ordered_generation_prefix(startIndex, startMicroseconds);
    adapter.start(startMicroseconds, startIndex, prefix, now_stamp());
    int64_t reportedRejected = 0;
    int64_t reportedSuppressed = 0;

    while (is_active()) {
        int32_t wakeGeneration = wake_generation();
        int64_t pauseStarted = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (state_ == PlaybackState::Paused) pauseStarted = now_stamp();
        }
        if (!wait_while_paused()) return;
        if (pauseStarted != 0) adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), pauseStarted)));
        int64_t now = now_stamp();
        bool controlBoundary = has_pending_ordered_live_control_boundary();
        if (!controlBoundary) {
            admit_ordered_note_attack_due(adapter, now);
            append_ordered_note_attack_seeks(adapter, now);
            admit_ordered_note_attack_due(adapter, now);
        }
        else if (adapter.pending_count() == 0) {
            apply_ordered_live_control_boundary();
            continue;
        }
        if (adapter.rejected_attacks() > reportedRejected || adapter.suppressed_releases() > reportedSuppressed) {
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                musical_limit_rejected_attacks_ += adapter.rejected_attacks() - reportedRejected;
                musical_limit_suppressed_releases_ += adapter.suppressed_releases() - reportedSuppressed;
            }
            reportedRejected = adapter.rejected_attacks();
            reportedSuppressed = adapter.suppressed_releases();
        }

        OutputWorkItem next;
        if (adapter.try_peek_dispatch(next)) {
            int64_t dueStamp = is_boundary_work(next) ? now : adapter.dispatch_due_clock_ticks(next, time_);
            if (dueStamp <= now) {
                OutputWorkItem item;
                if (!adapter.try_begin_dispatch(item))
                    raise("InvalidOperationException", "Ordered note-attack dispatch lost its head item.");
                int64_t sendStarted = now_stamp();
                dispatch_ordered_work_item(item);
                if (output_paced_timeline())
                    adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), sendStarted)));
                adapter.complete_dispatch();
                continue;
            }
        }

        PlaybackGeneration* tail = adapter.tail();
        if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor >= event_count() &&
            !adapter.tail_source_complete()) {
            adapter.complete_tail_source();
            continue;
        }
        if (adapter.pending_count() == 0 && adapter.tail_source_complete() && !has_preserved_seek_request()) return;

        int64_t targetStamp = models::wrap_add(now, time_.frequency() / 100);
        if (adapter.try_peek_dispatch(next) && !is_boundary_work(next))
            targetStamp = adapter.dispatch_due_clock_ticks(next, time_);
        else if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
            int64_t sourceMicros = event_micros(tail->source_event_cursor);
            targetStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
                micros_to_ticks(max64(0, models::wrap_sub(sourceMicros, tail->target_source_microseconds))));
        }
        wait_until_stopwatch(waiter, targetStamp, wakeGeneration);
    }
}

void EngineCore::admit_ordered_note_attack_due(OrderedNoteAttackAdapter& adapter, int64_t nowStamp)
{
    PlaybackGeneration* tail = adapter.tail();
    while (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
        int64_t sourceMicros = event_micros(tail->source_event_cursor);
        int64_t dueStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
            micros_to_ticks(max64(0, models::wrap_sub(sourceMicros, tail->target_source_microseconds))));
        if (dueStamp > nowStamp) break;
        adapter.admit_next_source_tick(capture_decision());
        tail = adapter.tail();
    }
}

void EngineCore::append_ordered_note_attack_seeks(OrderedNoteAttackAdapter& adapter, int64_t nowStamp)
{
    while (true) {
        PreservedSeekRequest request;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (preserved_seek_requests_.empty()) return;
            request = preserved_seek_requests_.front();
            preserved_seek_requests_.pop_front();
        }
        admit_ordered_note_attack_due(adapter, nowStamp);
        channel_overrides_.mark_all_forced_pending();
        std::vector<EventView> prefix = capture_ordered_generation_prefix(request.target_event_index,
            request.target_microseconds);
        if (!adapter.try_append_seek(request.target_microseconds, request.target_event_index, prefix, nowStamp))
            raise("InvalidOperationException", "Too many ordered playback generations are pending.");
        adopt_ordered_seek_target(request);
        admit_ordered_note_attack_due(adapter, nowStamp);
    }
}

// ---------------------------------------------------------------- output rate

void EngineCore::run_ordered_output_rate_mode(platform::Waiter& waiter)
{
    OrderedOutputRateAdapter adapter(events_, kMaximumLiveGenerations, events_per_second(), 1000000);
    int64_t baseTicks;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        baseTicks = transport_base_ticks_;
    }
    int64_t startMicroseconds = ticks_to_micros(baseTicks);
    int32_t startIndex = start_event_index_;
    std::vector<EventView> prefix = capture_ordered_generation_prefix(startIndex, startMicroseconds);
    adapter.start(startMicroseconds, startIndex, prefix, now_stamp());
    active_output_rate_gate_.store(adapter.gate());
    try {
        run_ordered_output_rate_body(waiter, adapter);
    }
    catch (...) {
        models::OutputRateGate* expected = adapter.gate();
        active_output_rate_gate_.compare_exchange_strong(expected, nullptr);
        throw;
    }
    models::OutputRateGate* expected = adapter.gate();
    active_output_rate_gate_.compare_exchange_strong(expected, nullptr);
}

void EngineCore::run_ordered_output_rate_body(platform::Waiter& waiter, OrderedOutputRateAdapter& adapter)
{
    int64_t reportedRejected = 0;
    int64_t reportedSuppressed = 0;
    int64_t reportedOther = 0;
    while (is_active()) {
        int32_t wakeGeneration = wake_generation();
        int64_t pauseStarted = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (state_ == PlaybackState::Paused) pauseStarted = now_stamp();
        }
        if (!wait_while_paused()) return;
        if (pauseStarted != 0) adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), pauseStarted)));
        int64_t now = now_stamp();
        bool controlBoundary = has_pending_ordered_live_control_boundary();
        if (!controlBoundary) {
            admit_ordered_output_rate_due(adapter, now);
            append_ordered_output_rate_seeks(adapter, now);
            admit_ordered_output_rate_due(adapter, now);
        }
        else if (adapter.pending_count() == 0) {
            apply_ordered_live_control_boundary();
            continue;
        }
        if (adapter.rejected_attacks() > reportedRejected || adapter.suppressed_releases() > reportedSuppressed ||
            adapter.rejected_other_messages() > reportedOther) {
            {
                std::lock_guard<std::recursive_mutex> lock(sync_);
                output_rate_rejected_attacks_ += adapter.rejected_attacks() - reportedRejected;
                output_rate_suppressed_releases_ += adapter.suppressed_releases() - reportedSuppressed;
                output_rate_rejected_other_messages_ += adapter.rejected_other_messages() - reportedOther;
            }
            reportedRejected = adapter.rejected_attacks();
            reportedSuppressed = adapter.suppressed_releases();
            reportedOther = adapter.rejected_other_messages();
        }

        OutputWorkItem next;
        if (adapter.try_peek_dispatch(next)) {
            int64_t dueStamp = is_boundary_work(next) ? now : adapter.dispatch_due_clock_ticks(next, time_);
            if (dueStamp <= now) {
                OutputWorkItem item;
                if (!adapter.try_begin_dispatch(item))
                    raise("InvalidOperationException", "Ordered output-rate dispatch lost its head item.");
                int64_t sendStarted = now_stamp();
                dispatch_ordered_work_item(item);
                if (output_paced_timeline())
                    adapter.shift_source_clock_anchors(max64(0, models::wrap_sub(now_stamp(), sendStarted)));
                adapter.complete_dispatch();
                continue;
            }
        }

        PlaybackGeneration* tail = adapter.tail();
        if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor >= event_count() &&
            !adapter.tail_source_complete()) {
            adapter.complete_tail_source();
            continue;
        }
        if (adapter.pending_count() == 0 && adapter.tail_source_complete() && !has_preserved_seek_request()) return;

        int64_t targetStamp = models::wrap_add(now, time_.frequency() / 100);
        if (adapter.try_peek_dispatch(next) && !is_boundary_work(next))
            targetStamp = adapter.dispatch_due_clock_ticks(next, time_);
        else if (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
            int64_t sourceMicros = event_micros(tail->source_event_cursor);
            targetStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
                micros_to_ticks(max64(0, models::wrap_sub(sourceMicros, tail->target_source_microseconds))));
        }
        wait_until_stopwatch(waiter, targetStamp, wakeGeneration);
    }
}

void EngineCore::admit_ordered_output_rate_due(OrderedOutputRateAdapter& adapter, int64_t nowStamp)
{
    PlaybackGeneration* tail = adapter.tail();
    while (adapter.pending_count() == 0 && tail != nullptr && !tail->is_admission_closed() &&
        tail->source_event_cursor < event_count()) {
        int64_t sourceMicros = event_micros(tail->source_event_cursor);
        int64_t dueStamp = saturating_add_ticks(tail->source_clock_anchor_ticks,
            micros_to_ticks(max64(0, models::wrap_sub(sourceMicros, tail->target_source_microseconds))));
        if (dueStamp > nowStamp) break;
        adapter.admit_next_source_tick(ticks_to_micros(nowStamp), capture_decision());
        tail = adapter.tail();
    }
}

void EngineCore::append_ordered_output_rate_seeks(OrderedOutputRateAdapter& adapter, int64_t nowStamp)
{
    while (true) {
        PreservedSeekRequest request;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (preserved_seek_requests_.empty()) return;
            request = preserved_seek_requests_.front();
            preserved_seek_requests_.pop_front();
        }
        admit_ordered_output_rate_due(adapter, nowStamp);
        channel_overrides_.mark_all_forced_pending();
        std::vector<EventView> prefix = capture_ordered_generation_prefix(request.target_event_index,
            request.target_microseconds);
        if (!adapter.try_append_seek(request.target_microseconds, request.target_event_index, prefix, nowStamp))
            raise("InvalidOperationException", "Too many ordered output-rate generations are pending.");
        adopt_ordered_seek_target(request);
    }
}

// ---------------------------------------------------------------- shared ordered helpers

bool EngineCore::has_pending_ordered_live_control_boundary()
{
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        if (!channel_control_requests_.empty()) return true;
    }
    return channel_overrides_.has_pending_output();
}

void EngineCore::apply_ordered_live_control_boundary()
{
    apply_pending_channel_controls();
    apply_pending_overrides();
}

bool EngineCore::try_take_one_preserved_seek_request(PreservedSeekRequest& request)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    if (preserved_seek_requests_.empty()) return false;
    request = preserved_seek_requests_.front();
    preserved_seek_requests_.pop_front();
    return true;
}

void EngineCore::publish_ordered_virtual_pressure(const OrderedVirtualForwardAdapter& adapter)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    has_published_queue_ = true;
    queue_length_ = 0;
    outstanding_events_ = adapter.pending_boundary_count();
    virtual_queue_length_ = max64(0, static_cast<int64_t>(adapter.virtual_occupancy()) - 1);
    virtual_outstanding_events_ = adapter.virtual_occupancy();
    virtual_maximum_queue_length_ = max64(virtual_maximum_queue_length_, adapter.maximum_occupancy());
}

void EngineCore::admit_ordered_finite_due(models::OrderedFiniteAdmissionCore& core, int64_t nowStamp)
{
    PlaybackGeneration* tail = core.tail();
    while (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
        EventView midiEvent = event_at(tail->source_event_cursor);
        int64_t deltaUs = max64(0, models::wrap_sub(midiEvent.micros, tail->target_source_microseconds));
        if (saturating_add_ticks(tail->source_clock_anchor_ticks, micros_to_ticks(deltaUs)) > nowStamp) break;
        bool filtered = should_filter_ordered_source(tail->source_event_cursor, midiEvent);
        OverflowPolicy policy = overflow_policy();
        int32_t limit = queue_length_limit_.load();
        int32_t capacity = limit < 1 ? 1 : limit;
        if (policy == OverflowPolicy::ClearBufferAndCatchUp && core.is_at_or_above_capacity(capacity) && !filtered) {
            models::OrderedFiniteClearResult cleared = core.clear_open_tail_and_catch_up(midiEvent.micros,
                capture_decision());
            record_ordered_finite_drops(cleared.dropped_events());
            if (cleared.requires_safety_reset()) output_->panic();  // v1: try { _output.Panic(); } catch { }
            tail = core.tail();
            continue;
        }
        models::OrderedFiniteAdmissionResult result = core.admit_next_source(midiEvent, filtered, capacity, policy);
        int32_t dropped = static_cast<int32_t>(result.evicted.size());
        // A rejected event is a dropped event, including a NoteOff that is suppressed because its NoteOn was dropped
        // (the classic Drop path counts it; v1's ordered path did not, register entry DD-008).
        if (result.disposition == models::OrderedFiniteAdmissionDisposition::Rejected)
            dropped++;
        record_ordered_finite_drops(dropped);
        tail = core.tail();
    }
}

void EngineCore::append_ordered_finite_seek_requests(models::OrderedFiniteAdmissionCore& core, int64_t nowStamp)
{
    while (true) {
        PreservedSeekRequest request;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (preserved_seek_requests_.empty()) return;
            request = preserved_seek_requests_.front();
            preserved_seek_requests_.pop_front();
        }
        admit_ordered_finite_due(core, nowStamp);
        channel_overrides_.mark_all_forced_pending();
        std::vector<EventView> prefix = capture_ordered_generation_prefix(request.target_event_index,
            request.target_microseconds);
        if (!core.try_append_seek(request.target_microseconds, request.target_event_index, &prefix, nowStamp, 0))
            raise("InvalidOperationException", "Too many ordered playback generations are pending.");
        adopt_ordered_seek_target(request);
        admit_ordered_finite_due(core, nowStamp);
    }
}

void EngineCore::repair_and_publish_ordered_finite_pressure(models::OrderedFiniteAdmissionCore& core,
    bool hasInService)
{
    PlaybackGeneration* tail = core.tail();
    int64_t logicalNow = tail == nullptr ? 0 : models::wrap_add(tail->target_source_microseconds,
        ticks_to_micros(max64(0, models::wrap_sub(now_stamp(), tail->source_clock_anchor_ticks))));
    if (queue_age_limit_enabled_.load() != 0) {
        OverflowPolicy policy = overflow_policy();
        models::OrderedFiniteAgeRepairResult repair = core.repair_waiting_time_pressure(logicalNow,
            queue_age_limit_microseconds_.load(), policy, capture_decision());
        int32_t removed = static_cast<int32_t>(repair.evicted.size());
        if (repair.has_clear_result) removed += repair.clear_result.dropped_events();
        record_ordered_finite_drops(removed);
        if (repair.has_clear_result && repair.clear_result.requires_safety_reset())
            output_->panic();  // v1: try { _output.Panic(); } catch { }
    }
    models::OrderedFinitePressureSnapshot pressure = core.measure_due_pressure(logicalNow, preview_decision());
    std::lock_guard<std::recursive_mutex> lock(sync_);
    has_published_queue_ = true;
    published_in_service_ = hasInService;
    queue_length_ = core.pending_count();
    outstanding_events_ = pressure.total_due_count;
    queue_age_microseconds_ = pressure.current_age_microseconds;
    if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
    if (queue_age_microseconds_ > maximum_queue_age_microseconds_) maximum_queue_age_microseconds_ = queue_age_microseconds_;
}

models::OrderedFiniteCapturedDecision EngineCore::capture_ordered_finite_decision(const EventView& midiEvent)
{
    models::OrderedFiniteCapturedDecision decision;
    decision.captured_event = midiEvent;
    decision.source_filtered = should_filter_ordered_source(midiEvent.event_index(), midiEvent);
    return decision;
}

models::OrderedFiniteCapturedDecision EngineCore::preview_ordered_finite_decision(const EventView& midiEvent)
{
    ChannelAttribute attribute;
    int32_t forcedValue;
    models::OrderedFiniteCapturedDecision decision;
    decision.captured_event = midiEvent;
    decision.source_filtered = channel_routing_.should_filter(midiEvent) ||
        channel_overrides_.should_suppress(midiEvent, attribute, forcedValue);
    return decision;
}

void EngineCore::record_ordered_finite_drops(int32_t count)
{
    if (count <= 0) return;
    std::lock_guard<std::recursive_mutex> lock(sync_);
    dropped_events_ = dropped_events_ > kInt64Max - count ? kInt64Max : dropped_events_ + count;
}

void EngineCore::adopt_ordered_seek_target(const PreservedSeekRequest& request)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    start_event_index_ = request.target_event_index;
    transport_base_ticks_ = micros_to_ticks(request.target_microseconds);
    run_start_stamp_ = now_stamp();
    pending_state_chase_event_index_.store(-1);
    effective_speed_frontier_microseconds_ = request.target_microseconds;
    if (request.generation == preserved_seek_generation_) intended_timeline_override_microseconds_ = -1;
}

void EngineCore::admit_ordered_unlimited_due(OrderedUnlimitedQueueAdapter& adapter, int64_t nowStamp)
{
    PlaybackGeneration* tail = adapter.tail();
    while (tail != nullptr && !tail->is_admission_closed() && tail->source_event_cursor < event_count()) {
        EventView midiEvent = event_at(tail->source_event_cursor);
        int64_t deltaUs = max64(0, models::wrap_sub(midiEvent.micros, tail->target_source_microseconds));
        int64_t dueStamp = saturating_add_ticks(tail->source_clock_anchor_ticks, micros_to_ticks(deltaUs));
        if (dueStamp > nowStamp) break;
        bool filtered = should_filter_ordered_source(tail->source_event_cursor, midiEvent);
        if (!adapter.try_admit_next(midiEvent, filtered))
            raise("InvalidOperationException", "The ordered Queue generation could not admit due source work.");
    }
}

void EngineCore::append_ordered_seek_requests(OrderedUnlimitedQueueAdapter& adapter, int64_t nowStamp)
{
    while (true) {
        PreservedSeekRequest request;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_);
            if (preserved_seek_requests_.empty()) return;
            request = preserved_seek_requests_.front();
            preserved_seek_requests_.pop_front();
        }
        // Close the target generation only after admitting everything that
        // became due before this request was observed.
        admit_ordered_unlimited_due(adapter, nowStamp);
        channel_overrides_.mark_all_forced_pending();
        std::vector<EventView> prefix = capture_ordered_generation_prefix(request.target_event_index,
            request.target_microseconds);
        if (!adapter.try_append_seek(request.target_microseconds, request.target_event_index, prefix, nowStamp, 0))
            raise("InvalidOperationException", "Too many ordered playback generations are pending.");
        adopt_ordered_seek_target(request);
        admit_ordered_unlimited_due(adapter, nowStamp);
    }
}

std::vector<EventView> EngineCore::capture_ordered_generation_prefix(int32_t targetEventIndex,
    int64_t targetMicroseconds)
{
    std::vector<EventView> prefix;
    std::shared_ptr<const PlaybackSong> song;
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        song = song_;
    }
    if (!song) return prefix;
    if (chase_midi_state_on_play_seek_.load() != 0 && targetEventIndex > 0)
        prefix = chase_messages(*song, targetEventIndex, targetMicroseconds);
    for (int channel = 0; channel < 16; ++channel) {
        int32_t mask = channel_overrides_.take_pending_mask(channel);
        if (!channel_routing_.is_enabled(channel)) {
            channel_overrides_.requeue(channel, mask);
            continue;
        }
        while (mask != 0) {
            int32_t bit = mask & -mask;
            int attributeIndex = 0;
            for (int32_t scan = bit; (scan >>= 1) != 0;) attributeIndex++;
            mask &= ~bit;
            ChannelAttribute attribute = static_cast<ChannelAttribute>(attributeIndex);
            int32_t value = channel_overrides_.get_value(channel, attribute);
            if (value == ChannelOverrideState::kAutoValue) continue;
            prefix.push_back(ChannelOverrideState::create_message(channel, attribute, value, -1, targetMicroseconds));
        }
    }
    return prefix;
}

bool EngineCore::should_filter_ordered_source(int32_t, const EventView& midiEvent)
{
    if (channel_routing_.should_filter(midiEvent)) {
        channel_routing_.record_filtered(midiEvent.channel());
        uses_pre_admission_filter_accounting_.store(1);
        return true;
    }
    ChannelAttribute attribute;
    int32_t forcedValue;
    if (!channel_overrides_.should_suppress(midiEvent, attribute, forcedValue)) return false;
    channel_overrides_.record_suppressed(midiEvent.channel());
    uses_pre_admission_filter_accounting_.store(1);
    return true;
}

void EngineCore::dispatch_ordered_work_item(const OutputWorkItem& item)
{
    if (item.kind == OutputWorkItemKind::ProtectedSeekMarker) {
        dispatch_protected_all_notes_off();
        return;
    }
    EventView midiEvent = item.has_midi_event ? item.midi_event : event_at(item.source_event_index);
    if (item.kind == OutputWorkItemKind::ForcedOrChaseMidi) {
        bool trackChannels = channel_monitoring_enabled_.load() != 0;
        send_rate_counted_essential_event(midiEvent);
        ChannelAttribute attribute;
        int32_t value;
        bool forced = ChannelOverrideState::try_classify(midiEvent, attribute, value) && midiEvent.channel() >= 0 &&
            channel_overrides_.get_value(midiEvent.channel(), attribute) == value;
        if (trackChannels) {
            if (forced) channel_state_.record_override_applied(midiEvent.channel(), attribute, value);
            else channel_state_.record_successful(midiEvent);
            publish_channel_state(true);
        }
        return;
    }

    int64_t providerStart;
    int64_t providerEnd;
    send_scheduled_event(midiEvent, channel_monitoring_enabled_.load() != 0, providerStart, providerEnd);
    int64_t actualUs = ticks_to_micros(current_transport_ticks());
    int64_t lag = max64(0, models::wrap_sub(actualUs, item.logical_output_microseconds));
    {
        std::lock_guard<std::recursive_mutex> lock(sync_);
        processed_events_++;
        last_dispatched_microseconds_ = item.logical_output_microseconds;
        current_lag_microseconds_ = lag;
        if (lag > maximum_lag_microseconds_) maximum_lag_microseconds_ = lag;
    }
    publish_channel_state(false);
}

void EngineCore::publish_ordered_unlimited_pressure(const OrderedUnlimitedQueueAdapter& adapter, bool inService)
{
    std::lock_guard<std::recursive_mutex> lock(sync_);
    has_published_queue_ = true;
    published_in_service_ = inService;
    queue_length_ = adapter.pending_work_count();
    outstanding_events_ = queue_length_ + (inService ? 1 : 0);
    if (outstanding_events_ > maximum_queue_length_) maximum_queue_length_ = outstanding_events_;
}

} // namespace engine
} // namespace midibn
