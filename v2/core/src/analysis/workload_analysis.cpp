// Port of v1 WorkloadAnalysis.cs (WorkloadAnalyzer). Statement order,
// progress reports, cancellation checks, checked/unchecked arithmetic and
// floating-point operation order follow v1 exactly; see workload_analysis.h.
#include "midibn/analysis/workload_analysis.h"
#include "midibn/analysis/fast_pending_queue.h"

#include "midibn/models/delayed_note_off_gate.h"
#include "midibn/models/per_note_gate.h"
#include "midibn/models/queue.h"
#include "midibn/models/rate_gates.h"

#include <algorithm>
#include <chrono>

namespace midibn {
namespace analysis {

using models::checked_add;
using models::checked_mul;
using models::checked_sub;
using models::CompleteNoteEventKind;
using models::CompleteNoteTracker;
using models::kInt64Max;
using models::kInt64Min;
using models::kInt32Max;
using models::OverflowPolicy;
using models::ServiceDurationClock;
using models::ServiceDurationMode;
using models::wrap_add;
using models::wrap_mul;
using models::wrap_sub;

namespace {

// .NET Math.Max(double, double).
double net_max(double val1, double val2)
{
    if (val1 > val2) return val1;
    if (val1 != val1) return val1;
    return val2;
}

void check(const AnalysisControl& control)
{
    if (control.cancellation_requested && control.cancellation_requested())
        throw models::ModelException{ "OperationCanceledException", "", "The operation was canceled." };
}

void report(const AnalysisControl& control, const char* stage, int64_t completed, int64_t total, int32_t overallStart,
    int32_t overallLength)
{
    if (!control.progress) return;
    int64_t safeTotal = std::max<int64_t>(1, total);
    int64_t safeCompleted = std::max<int64_t>(0, std::min(safeTotal, completed));
    AnalysisProgress p;
    p.stage = stage;
    p.stage_permille = static_cast<int32_t>(wrap_mul(safeCompleted, 1000) / safeTotal);
    p.overall_permille = std::min<int32_t>(1000,
        overallStart + static_cast<int32_t>(wrap_mul(safeCompleted, overallLength) / safeTotal));
    control.progress(p);
}

int32_t bucket_of(int64_t micros, int64_t bucketMicroseconds, size_t bucketCount)
{
    return static_cast<int32_t>(std::min<int64_t>(static_cast<int64_t>(bucketCount) - 1,
        std::max<int64_t>(0, micros / bucketMicroseconds)));
}

// bucket_of with a remembered answer (DD-075). Events arrive in time order, so consecutive calls nearly always fall in
// the bucket of the previous call; only a change of bucket pays for the 64-bit division (a library call on 32-bit
// builds). The result is always exactly bucket_of(micros, bucketMicroseconds, bucketCount).
class BucketLocator {
public:
    BucketLocator(int64_t bucketMicroseconds, size_t bucketCount)
        : bucket_(bucketMicroseconds), last_(static_cast<int32_t>(bucketCount) - 1) {}

    // The number of buckets changed (the gate projection can grow the arrays): forget the remembered bucket.
    void set_count(size_t bucketCount)
    {
        last_ = static_cast<int32_t>(bucketCount) - 1;
        low_ = 1;
        high_ = 0;
    }

    int32_t index(int64_t micros)
    {
        if (micros >= low_ && micros < high_) return cached_;
        return locate(micros);
    }

private:
    int32_t locate(int64_t micros)
    {
        if (micros < 0) {
            low_ = kInt64Min;
            high_ = 0;
            cached_ = 0;
            return 0;
        }
        int64_t quotient = micros / bucket_;
        low_ = quotient * bucket_;
        high_ = low_ > kInt64Max - bucket_ ? kInt64Max : low_ + bucket_;
        cached_ = static_cast<int32_t>(std::min<int64_t>(last_, quotient));
        return cached_;
    }

    int64_t bucket_;
    int32_t last_;
    int64_t low_ = 1;
    int64_t high_ = 0;   // empty range until the first call
    int32_t cached_ = 0;
};

bool uses_configured_service(const AnalysisConfiguration* c)
{
    return c != nullptr && !c->per_note_interval_gate_enabled && !c->delayed_note_off_enabled &&
        !c->note_attack_limit_enabled && !c->output_rate_limit_enabled &&
        (c->simulate_slowdown || c->apply_queue_limit_without_slowdown && c->queue_length_limit_enabled);
}

ServiceDurationClock create_service_clock(const AnalysisConfiguration& c)
{
    return ServiceDurationClock(c.service_duration_mode, c.processing_us, c.midi_bitrate, c.events_per_second);
}

void record_drop(WorkloadAnalysis& result, int32_t bucketIndex, int32_t count)
{
    result.predicted_dropped_events = wrap_add(result.predicted_dropped_events, count);
    WorkloadBucket& bucket = result.buckets[static_cast<size_t>(bucketIndex)];
    bucket.predicted_dropped_events = static_cast<int32_t>(
        static_cast<uint32_t>(bucket.predicted_dropped_events) + static_cast<uint32_t>(count));
}

void record_workload_cluster(WorkloadAnalysis& result, int32_t bucketIndex, int32_t clusterSize)
{
    result.unique_timestamps++;
    if (clusterSize > result.largest_timestamp_cluster) result.largest_timestamp_cluster = clusterSize;
    if (clusterSize >= 2) result.events_in_clusters_at_least_2 += clusterSize;
    if (clusterSize >= 10) result.events_in_clusters_at_least_10 += clusterSize;
    if (clusterSize >= 50) result.events_in_clusters_at_least_50 += clusterSize;
    if (clusterSize >= 100) result.events_in_clusters_at_least_100 += clusterSize;
    WorkloadBucket& bucket = result.buckets[static_cast<size_t>(bucketIndex)];
    if (clusterSize > bucket.largest_cluster) bucket.largest_cluster = clusterSize;
}

// One cluster scan for both v1 store kinds. The compact path reads the
// record's payload length and metadata kind, the reference path the view's
// DataLength and Kind; ServiceDurationCalculator differs between them in v1
// only through which helper is called, reproduced below.
void scan_workload(const EventSource& events, WorkloadAnalysis& result, int64_t bucketMicroseconds, int32_t bucketCount,
    const AnalysisConfiguration* configuration, int64_t* typeEvents, int64_t* typeBytes, const AnalysisControl& control)
{
    const bool compact = events.is_compact();
    const int32_t count = events.count();
    const bool addService = configuration != nullptr && uses_configured_service(configuration) &&
        configuration->service_duration_mode != ServiceDurationMode::EventsPerSecond;
    BucketLocator locator(bucketMicroseconds, static_cast<size_t>(bucketCount));
    EventView view;   // only the reference path fills it
    int32_t index = 0;
    while (index < count) {
        if ((index & 16383) == 0) {
            check(control);
            report(control, "Scanning events and clusters", index, count, 50, 650);
        }
        int64_t timestamp = events.micros_at(index);
        int32_t bucketIndex = locator.index(timestamp);
        WorkloadBucket& bucket = result.buckets[static_cast<size_t>(bucketIndex)];
        int32_t clusterEnd = index;
        while (clusterEnd < count) {
            int32_t bytes;
            int kind;
            int64_t micros;
            if (compact) {
                // Reads the stored 24-byte form directly (the decoded copy of the same record gave the same
                // numbers: payload length, kind, intended time).
                const EventRecords& records = events.store->records;
                const PackedRecord& record = records.packed_at(clusterEnd);
                micros = record.micros;
                if (record.is_long()) {
                    int64_t offset;
                    records.long_extent(record, offset, bytes);
                } else bytes = record.inline_length();
                kind = record.metadata & 0x0F;
            } else {
                view = events.at(clusterEnd);
                micros = view.micros;
                bytes = view.data_length;
                kind = static_cast<int>(view.kind);
            }
            if (micros != timestamp) break;
            if ((clusterEnd & 16383) == 0) {
                check(control);
                report(control, "Scanning events and clusters", clusterEnd, count, 50, 650);
            }
            result.total_bytes = wrap_add(result.total_bytes, bytes);
            bucket.event_count++;
            bucket.byte_count = wrap_add(bucket.byte_count, bytes);
            if (addService) {
                int64_t service;
                if (compact)
                    service = configuration->service_duration_mode == ServiceDurationMode::ProcessingTime
                        ? std::max<int64_t>(0, configuration->processing_us)
                        : models::calculate_bitrate_microseconds(bytes, configuration->midi_bitrate);
                else
                    service = models::calculate_service_microseconds(view, configuration->service_duration_mode,
                        configuration->processing_us, configuration->midi_bitrate);
                bucket.service_demand_us = wrap_add(bucket.service_demand_us, service);
            }
            typeEvents[kind]++;
            typeBytes[kind] = wrap_add(typeBytes[kind], bytes);
            clusterEnd++;
        }
        record_workload_cluster(result, bucketIndex, clusterEnd - index);
        index = clusterEnd;
    }
}

void add_event_rate_demand(const EventSource& events, WorkloadAnalysis& result, int64_t bucketMicroseconds,
    const AnalysisConfiguration& configuration, const AnalysisControl& control)
{
    ServiceDurationClock clock = create_service_clock(configuration);
    BucketLocator locator(bucketMicroseconds, result.buckets.size());
    const int32_t count = events.count();
    for (int32_t i = 0; i < count; ++i) {
        if ((i & 16383) == 0) {
            check(control);
            report(control, "Calculating event-rate service demand", i, count, 750, 0);
        }
        int32_t bucket = locator.index(events.micros_at(i));
        WorkloadBucket& target = result.buckets[static_cast<size_t>(bucket)];
        target.service_demand_us = wrap_add(target.service_demand_us, clock.next_microseconds(events, i));
    }
}

void record_gate_output(WorkloadAnalysis& result, BucketLocator& locator, int64_t logicalTime, bool noteTransition)
{
    // A time inside the known duration needs no growth: the buckets were sized for at least that duration (DD-075;
    // this skips two 64-bit divisions for nearly every output event).
    if (logicalTime > result.duration_us) {
        int64_t requiredDuration = logicalTime;
        int32_t required = calculate_bucket_count(requiredDuration, result.bucket_us);
        if (required > static_cast<int32_t>(result.gate_output_buckets.size())) {
            int32_t previous = static_cast<int32_t>(result.gate_output_buckets.size());
            int32_t grown = std::min(kMaximumBucketCount,
                std::max(required, previous > kMaximumBucketCount / 2 ? kMaximumBucketCount : previous * 2));
            result.gate_output_buckets.resize(static_cast<size_t>(grown), 0);
            result.buckets.resize(static_cast<size_t>(grown));
        }
        result.duration_us = requiredDuration;
        locator.set_count(result.gate_output_buckets.size());
    }
    int32_t bucket = locator.index(logicalTime);
    result.gate_output_buckets[static_cast<size_t>(bucket)]++;
    result.gate_output_events++;
    if (noteTransition) result.gate_output_note_transitions++;
    else result.gate_output_non_note_events++;
    result.gate_output_completion_us = std::max(result.gate_output_completion_us, logicalTime);
}

void finish_gate_peak(WorkloadAnalysis& result, const AnalysisControl& control)
{
    double seconds = static_cast<double>(result.bucket_us) / 1000000.0;
    for (size_t i = 0; i < result.gate_output_buckets.size(); ++i) {
        if ((i & 4095) == 0) check(control);
        result.gate_peak_events_per_second = net_max(result.gate_peak_events_per_second,
            static_cast<double>(result.gate_output_buckets[i]) / seconds);
    }
}

void start_gate_buckets(WorkloadAnalysis& result)
{
    result.has_gate_output_buckets = true;
    result.gate_output_buckets.assign(result.buckets.size(), 0);
}

int32_t interval_limit(const AnalysisConfiguration& c)
{
    return c.interval_polyphony_limit_enabled
        ? std::max(1, std::min(kDefaultIntervalPolyphonyLimit, c.interval_polyphony_limit))
        : kDefaultIntervalPolyphonyLimit;
}

// v1 CreateOverrideFilteredSong: copies every kept event (renumbered), as a
// reference-list song; the duration is unchanged.
struct FilteredSong {
    models::EventViewList list;
    AnalysisSong song;
};

void create_override_filtered_song(const AnalysisSong& source, const overrides::AnalysisOverrideSnapshot& snapshot,
    const AnalysisControl& control, FilteredSong& filtered, int64_t& filteredCount)
{
    const int32_t count = source.events.count();
    filtered.list.events.clear();
    filtered.list.events.reserve(static_cast<size_t>(count));
    filteredCount = 0;
    for (int32_t index = 0; index < count; ++index) {
        if ((index & 16383) == 0) check(control);
        EventView midiEvent = source.events.at(index);
        if (snapshot.should_suppress(midiEvent)) {
            filteredCount++;
            continue;
        }
        filtered.list.events.push_back(to_compatibility_event(midiEvent));
    }
    filtered.song.events = EventSource(&filtered.list);
    filtered.song.duration_us = source.duration_us;
}

void apply_projected_forced_overrides(WorkloadAnalysis& result, const AnalysisConfiguration& configuration)
{
    const overrides::AnalysisOverrideSnapshot* snapshot = configuration.override_projection.get();
    if (snapshot == nullptr) return;
    int64_t forced = snapshot->forced_message_count();
    result.override_projected_forced_messages = forced;
    if (forced == 0 || !result.has_gate_output_buckets || result.gate_output_buckets.empty()) return;
    result.gate_output_buckets[0] += forced;
    result.gate_output_events += forced;
    result.gate_output_non_note_events += forced;
    double seconds = static_cast<double>(result.bucket_us) / 1000000.0;
    if (seconds > 0) {
        result.gate_peak_events_per_second = net_max(result.gate_peak_events_per_second,
            static_cast<double>(result.gate_output_buckets[0]) / seconds);
    }
}

// ------------------------------------------------------------ pressure

void record_analysis_queue_age(const FastPendingQueue& pending, const EventSource& events, WorkloadAnalysis& result,
    BucketLocator& locator,
    int64_t logicalNow, int64_t arrivalOffset)
{
    if (pending.count() == 0) return;
    int64_t queueAge = std::max<int64_t>(0,
        wrap_sub(logicalNow, checked_add(events.micros_at(pending.oldest_event_index()), arrivalOffset)));
    if (queueAge > result.predicted_peak_queue_age_us) result.predicted_peak_queue_age_us = queueAge;
    int32_t bucket = locator.index(logicalNow);
    WorkloadBucket& target = result.buckets[static_cast<size_t>(bucket)];
    if (queueAge > target.predicted_peak_queue_age_us) target.predicted_peak_queue_age_us = queueAge;
}

bool analysis_queue_age_exceeded(const FastPendingQueue& pending, const EventSource& events, int64_t logicalNow,
    int64_t limit, int64_t arrivalOffset)
{
    return pending.count() > 0 &&
        wrap_sub(logicalNow, checked_add(events.micros_at(pending.oldest_event_index()), arrivalOffset)) > limit;
}

bool apply_analysis_queue_age_limit(FastPendingQueue& pending, const EventSource& events,
    CompleteNoteTracker& completeNotes, const AnalysisConfiguration& configuration, WorkloadAnalysis& result,
    BucketLocator& locator,
    int64_t logicalNow, int64_t arrivalOffset)
{
    if (!configuration.queue_age_limit_enabled || pending.count() == 0) return false;
    record_analysis_queue_age(pending, events, result, locator, logicalNow, arrivalOffset);
    int64_t limit = std::max<int64_t>(1, configuration.queue_age_limit_us);
    int32_t bucket = locator.index(logicalNow);
    if (configuration.overflow_policy == OverflowPolicy::DropOldest) {
        while (analysis_queue_age_exceeded(pending, events, logicalNow, limit, arrivalOffset)) {
            pending.dequeue();
            record_drop(result, bucket, 1);
        }
    } else if (configuration.overflow_policy == OverflowPolicy::DropOldestCompleteNote) {
        while (analysis_queue_age_exceeded(pending, events, logicalNow, limit, arrivalOffset)) {
            int32_t attack;
            int32_t release;
            if (!pending.try_evict_oldest_complete_note_at_head(attack, release)) break;
            if (release < 0) completeNotes.mark_unsent_attack_evicted(attack);
            record_drop(result, bucket, release >= 0 ? 2 : 1);
        }
    } else if (configuration.overflow_policy == OverflowPolicy::ClearBufferAndCatchUp &&
        analysis_queue_age_exceeded(pending, events, logicalNow, limit, arrivalOffset)) {
        int32_t dropped = pending.count();
        pending.clear();
        record_drop(result, bucket, dropped);
        result.predicted_buffer_clears++;
        result.buckets[static_cast<size_t>(bucket)].predicted_buffer_clears++;
        return true;
    }
    return false;
}

void analyze_virtual_forward_pressure(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control)
{
    if (configuration.overflow_policy != OverflowPolicy::DropNewest &&
        configuration.overflow_policy != OverflowPolicy::DropIncomingCompleteNotes)
        models::throw_invalid_operation("Drop oldest events and Clear buffer require Simulate slowdown; a forward-only model cannot retract MIDI that was already sent.");

    const EventSource& events = song.events;
    models::ForwardDropQueue queue(std::max(1, configuration.queue_length_limit));
    ServiceDurationClock serviceClock = create_service_clock(configuration);
    CompleteNoteTracker completeNotes;
    int64_t lastAccepted = 0;
    BucketLocator arrivalLocator(result.bucket_us, result.buckets.size());
    const int32_t count = events.count();
    for (int32_t i = 0; i < count; ++i) {
        if ((i & 16383) == 0) {
            check(control);
            report(control, "Projecting forward-only queue pressure", i, count, 750, 250);
        }
        EventView incomingEvent = events.at(i);
        int64_t arrival = incomingEvent.micros;
        queue.advance(arrival);
        int32_t bucketIndex = arrivalLocator.index(arrival);
        CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(incomingEvent);
        if (noteKind == CompleteNoteEventKind::NoteOff && completeNotes.should_suppress_note_off(incomingEvent)) {
            record_drop(result, bucketIndex, 1);
            continue;
        }
        bool safetyAdmission = configuration.overflow_policy == OverflowPolicy::DropIncomingCompleteNotes &&
            noteKind != CompleteNoteEventKind::NoteOn;
        ServiceDurationClock admittedClock = serviceClock;
        int64_t service = admittedClock.next_microseconds(incomingEvent);
        bool accepted = queue.try_admit(arrival, service, safetyAdmission);
        if (!accepted) {
            if (noteKind == CompleteNoteEventKind::NoteOn)
                completeNotes.record_note_on(incomingEvent, false,
                    configuration.overflow_policy == OverflowPolicy::DropIncomingCompleteNotes);
            record_drop(result, bucketIndex, 1);
            continue;
        }
        serviceClock = admittedClock;
        if (noteKind == CompleteNoteEventKind::NoteOn) completeNotes.record_note_on(incomingEvent, true, false);
        lastAccepted = arrival;
        int32_t occupancy = queue.occupancy();
        if (occupancy > result.predicted_maximum_occupancy) result.predicted_maximum_occupancy = occupancy;
        WorkloadBucket& target = result.buckets[static_cast<size_t>(bucketIndex)];
        if (occupancy > target.predicted_peak_occupancy) target.predicted_peak_occupancy = occupancy;
    }
    result.predicted_output_completion_us = lastAccepted;
    report(control, "Projecting forward-only queue pressure", count, count, 750, 250);
}

void analyze_unlimited_pressure(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control)
{
    const EventSource& events = song.events;
    bool busy = false;
    int64_t completion = 0;
    int64_t lastCompleted = 0;
    ServiceDurationClock serviceClock = create_service_clock(configuration);
    int32_t nextPending = 0;
    bool outputPaced = configuration.output_paced_timeline && configuration.simulate_slowdown;
    int64_t previousSourceArrival = kInt64Min;
    int64_t pacedArrival = 0;
    BucketLocator arrivalLocator(result.bucket_us, result.buckets.size());
    const int32_t count = events.count();
    for (int32_t i = 0; i < count; ++i) {
        if ((i & 16383) == 0) {
            check(control);
            report(control, "Projecting unlimited queue pressure", i, count, 750, 250);
        }
        EventView incomingEvent = events.at(i);
        int64_t sourceArrival = incomingEvent.micros;
        int64_t arrival = sourceArrival;
        if (outputPaced && sourceArrival != previousSourceArrival) {
            if (previousSourceArrival == kInt64Min) pacedArrival = sourceArrival;
            else {
                while (busy) {
                    lastCompleted = completion;
                    if (nextPending < i)
                        completion = checked_add(completion, serviceClock.next_microseconds(events, nextPending++));
                    else busy = false;
                }
                pacedArrival = checked_add(std::max(pacedArrival, lastCompleted),
                    std::max<int64_t>(0, checked_sub(sourceArrival, previousSourceArrival)));
            }
            previousSourceArrival = sourceArrival;
        }
        if (outputPaced) arrival = pacedArrival;
        while (busy && completion <= arrival) {
            lastCompleted = completion;
            if (nextPending < i) {
                int64_t nextService = serviceClock.next_microseconds(events, nextPending);
                completion = checked_add(completion, nextService);
                nextPending++;
            } else busy = false;
        }
        if (!busy) {
            busy = true;
            completion = checked_add(arrival, serviceClock.next_microseconds(incomingEvent));
            nextPending = i + 1;
        }
        int32_t occupancy = 1 + std::max(0, i + 1 - nextPending);
        int32_t bucketIndex = arrivalLocator.index(arrival);
        if (occupancy > result.predicted_maximum_occupancy) result.predicted_maximum_occupancy = occupancy;
        WorkloadBucket& target = result.buckets[static_cast<size_t>(bucketIndex)];
        if (occupancy > target.predicted_peak_occupancy) target.predicted_peak_occupancy = occupancy;
    }
    if (busy)
        while (nextPending < count)
            completion = checked_add(completion, serviceClock.next_microseconds(events, nextPending++));
    result.predicted_output_completion_us = busy ? completion : lastCompleted;
    report(control, "Projecting unlimited queue pressure", count, count, 750, 250);
}

void analyze_pressure(const AnalysisSong& song, WorkloadAnalysis& result, const AnalysisConfiguration& configuration,
    const AnalysisControl& control)
{
    const EventSource& events = song.events;
    bool virtualForward = !configuration.simulate_slowdown && configuration.queue_length_limit_enabled &&
        configuration.apply_queue_limit_without_slowdown;
    if (virtualForward) {
        analyze_virtual_forward_pressure(song, result, configuration, control);
        return;
    }
    if (!configuration.simulate_slowdown ||
        configuration.service_duration_mode == ServiceDurationMode::ProcessingTime && configuration.processing_us == 0 ||
        configuration.service_duration_mode == ServiceDurationMode::EventsPerSecond &&
            configuration.events_per_second == 0) {
        const int32_t count = events.count();
        result.predicted_maximum_occupancy = count == 0 ? 0 : 1;
        result.predicted_output_completion_us = count == 0 ? 0 : events.micros_at(count - 1);
        for (WorkloadBucket& bucket : result.buckets)
            if (bucket.event_count > 0) bucket.predicted_peak_occupancy = 1;
        report(control, "Queue projection (instantaneous service)", 1, 1, 750, 250);
        return;
    }
    if (!configuration.queue_length_limit_enabled) {
        analyze_unlimited_pressure(song, result, configuration, control);
        return;
    }

    // Same refusal as PendingMidiQueue's constructor gave for a capacity below 1.
    if (configuration.queue_length_limit < 1) models::throw_argument_out_of_range("capacity");
    FastPendingQueue pending(events,
        configuration.overflow_policy == OverflowPolicy::DropOldestCompleteNote ||
            configuration.overflow_policy == OverflowPolicy::DropBackgroundNotes,
        configuration.overflow_policy == OverflowPolicy::DropBackgroundNotes);
    bool busy = false;
    int64_t completion = 0;
    int64_t lastCompleted = 0;
    ServiceDurationClock serviceClock = create_service_clock(configuration);
    int32_t limit = configuration.queue_age_limit_enabled ? kInt32Max : std::max(1, configuration.queue_length_limit);
    CompleteNoteTracker completeNotes;
    int32_t musicalFairnessCursor = 0;
    bool outputPaced = configuration.output_paced_timeline && configuration.simulate_slowdown;
    int64_t previousSourceArrival = kInt64Min;
    int64_t pacedArrival = 0;
    int64_t queueArrivalOffset = 0;
    BucketLocator arrivalLocator(result.bucket_us, result.buckets.size());
    BucketLocator nowLocator(result.bucket_us, result.buckets.size());
    BucketLocator skipLocator(result.bucket_us, result.buckets.size());
    const int32_t count = events.count();
    uint32_t passesSinceCheck = 0;
    for (int32_t i = 0; i < count; ++i) {
        // Cancellation is looked at every 1024 passes of this loop (a changed value in the window cancels this run
        // promptly); the progress report keeps its every-16384-events rhythm.
        if ((i & 16383) == 0) {
            check(control);
            report(control, "Projecting finite queue pressure", i, count, 750, 250);
        }
        else if ((++passesSinceCheck & 1023) == 0) check(control);
        EventView incomingEvent = events.at(i);
        int64_t sourceArrival = incomingEvent.micros;
        int64_t arrival = sourceArrival;
        if (outputPaced) {
            if (sourceArrival != previousSourceArrival) {
                if (previousSourceArrival == kInt64Min) pacedArrival = sourceArrival;
                else {
                    while (busy) {
                        lastCompleted = completion;
                        apply_analysis_queue_age_limit(pending, events, completeNotes, configuration, result, nowLocator, completion,
                            queueArrivalOffset);
                        if (pending.count() > 0) {
                            int32_t next = pending.dequeue();
                            completion = checked_add(completion, serviceClock.next_microseconds(events, next));
                        } else busy = false;
                    }
                    pacedArrival = checked_add(std::max(pacedArrival, lastCompleted),
                        std::max<int64_t>(0, checked_sub(sourceArrival, previousSourceArrival)));
                }
                previousSourceArrival = sourceArrival;
                queueArrivalOffset = wrap_sub(pacedArrival, sourceArrival);
            }
            arrival = pacedArrival;
        }
        bool clearedAtCompletion = false;
        while (busy && completion <= arrival) {
            lastCompleted = completion;
            clearedAtCompletion |= apply_analysis_queue_age_limit(pending, events, completeNotes, configuration, result, nowLocator,
                completion, queueArrivalOffset);
            if (pending.count() > 0) {
                int32_t next = pending.dequeue();
                int64_t nextService = serviceClock.next_microseconds(events, next);
                completion = checked_add(completion, nextService);
            } else busy = false;
        }
        if (clearedAtCompletion) {
            while (i < count && events.micros_at(i) <= lastCompleted) {
                int32_t skippedBucket = skipLocator.index(events.micros_at(i));
                record_drop(result, skippedBucket, 1);
                i++;
            }
            if (i >= count) break;
            i--;
            continue;
        }

        int32_t bucketIndex = arrivalLocator.index(arrival);
        CompleteNoteEventKind noteKind = CompleteNoteTracker::classify(incomingEvent);
        CompleteNoteTracker::NoteOffMatch noteMatch;
        if (noteKind == CompleteNoteEventKind::NoteOff) noteMatch = completeNotes.take_note_off(incomingEvent);
        else noteMatch.attack_index = -1;
        if (noteMatch.suppress) {
            record_drop(result, bucketIndex, 1);
            continue;
        }
        FastPendingQueue::Entry incomingEntry;
        incomingEntry.event_index = i;
        incomingEntry.paired_attack_index = noteMatch.attack_index;
        incomingEntry.is_note_on = noteKind == CompleteNoteEventKind::NoteOn;
        incomingEntry.is_note_off = noteKind == CompleteNoteEventKind::NoteOff;
        int64_t ageLimit = std::max<int64_t>(1, configuration.queue_age_limit_us);
        bool ageEvictionStalled = false;
        record_analysis_queue_age(pending, events, result, nowLocator, arrival, queueArrivalOffset);
        if (configuration.queue_age_limit_enabled && configuration.overflow_policy == OverflowPolicy::DropOldest) {
            while (analysis_queue_age_exceeded(pending, events, arrival, ageLimit, queueArrivalOffset)) {
                pending.dequeue();
                record_drop(result, bucketIndex, 1);
            }
        } else if (configuration.queue_age_limit_enabled &&
            configuration.overflow_policy == OverflowPolicy::DropOldestCompleteNote) {
            while (analysis_queue_age_exceeded(pending, events, arrival, ageLimit, queueArrivalOffset)) {
                int32_t previousOldest = pending.oldest_event_index();
                int32_t evictedAttack;
                int32_t evictedRelease;
                if (!pending.try_evict_oldest_complete_note_at_head(evictedAttack, evictedRelease)) {
                    ageEvictionStalled = true;
                    break;
                }
                if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                record_drop(result, bucketIndex, evictedRelease >= 0 ? 2 : 1);
                if (pending.count() > 0 && pending.oldest_event_index() == previousOldest) {
                    ageEvictionStalled = true;
                    break;
                }
            }
        }
        int32_t occupancy = pending.count() + (busy ? 1 : 0);
        bool ageExceeded = configuration.queue_age_limit_enabled &&
            analysis_queue_age_exceeded(pending, events, arrival, ageLimit, queueArrivalOffset);
        bool protectedTraffic = noteKind == CompleteNoteEventKind::NoteOff ||
            CompleteNoteTracker::is_safety_control(incomingEvent);
        bool safetyAdmission = configuration.overflow_policy == OverflowPolicy::DropIncomingCompleteNotes &&
            noteKind != CompleteNoteEventKind::NoteOn;
        if ((!configuration.queue_age_limit_enabled && occupancy < limit ||
            configuration.queue_age_limit_enabled && !ageExceeded) || safetyAdmission) {
            if (!busy) {
                busy = true;
                completion = checked_add(arrival, serviceClock.next_microseconds(incomingEvent));
            } else pending.enqueue(incomingEntry);
            if (noteKind == CompleteNoteEventKind::NoteOn) completeNotes.record_note_on(incomingEvent, i, true, false);
        } else if (configuration.overflow_policy == OverflowPolicy::DropOldest && pending.count() > 0) {
            EventView oldestEvent = events.at(pending.oldest_event_index());
            bool oldestProtected = CompleteNoteTracker::classify(oldestEvent) == CompleteNoteEventKind::NoteOff ||
                CompleteNoteTracker::is_safety_control(oldestEvent);
            if (!oldestProtected) {
                pending.dequeue();
                pending.enqueue(incomingEntry);
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, i, true, false);
                record_drop(result, bucketIndex, 1);
            } else if (protectedTraffic) pending.enqueue(incomingEntry);
            else {
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, i, false, false);
                record_drop(result, bucketIndex, 1);
            }
        } else if (configuration.overflow_policy == OverflowPolicy::DropOldestCompleteNote) {
            int32_t evictedAttack = -1;
            int32_t evictedRelease = -1;
            bool evicted = !ageEvictionStalled && pending.try_evict_oldest_complete_note(evictedAttack, evictedRelease);
            if (evicted) {
                if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                record_drop(result, bucketIndex, evictedRelease >= 0 ? 2 : 1);
            }
            bool ownAttackEvicted = evicted && noteKind == CompleteNoteEventKind::NoteOff &&
                noteMatch.attack_index == evictedAttack;
            if (ownAttackEvicted || (!evicted && !protectedTraffic)) {
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, i, false, true);
                record_drop(result, bucketIndex, 1);
            } else {
                pending.enqueue(incomingEntry);
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, i, true, false);
            }
        } else if (configuration.overflow_policy == OverflowPolicy::DropBackgroundNotes) {
            int32_t evictedAttack;
            int32_t evictedRelease;
            bool evicted = pending.try_evict_least_important_complete_note(musicalFairnessCursor,
                evictedAttack, evictedRelease);
            if (evicted) {
                if (evictedRelease < 0) completeNotes.mark_unsent_attack_evicted(evictedAttack);
                record_drop(result, bucketIndex, evictedRelease >= 0 ? 2 : 1);
            }
            bool ownAttackEvicted = evicted && noteKind == CompleteNoteEventKind::NoteOff &&
                noteMatch.attack_index == evictedAttack;
            if (ownAttackEvicted || (!evicted && noteKind == CompleteNoteEventKind::NoteOn)) {
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, i, false, true);
                record_drop(result, bucketIndex, 1);
            } else {
                pending.enqueue(incomingEntry);
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, i, true, false);
            }
        } else if (configuration.overflow_policy == OverflowPolicy::ClearBufferAndCatchUp) {
            // Match live Playback: the event already in service is
            // ordered/protected from a capacity edit. Clear pending
            // and incoming work, but keep its completion and clock.
            int32_t dropped = pending.count() + 1;
            pending.clear();
            while (i + 1 < count && events.micros_at(i + 1) <= arrival) { dropped++; i++; }
            record_drop(result, bucketIndex, dropped);
            result.predicted_buffer_clears++;
            result.buckets[static_cast<size_t>(bucketIndex)].predicted_buffer_clears++;
        } else {
            if (protectedTraffic) pending.enqueue(incomingEntry);
            else {
                if (noteKind == CompleteNoteEventKind::NoteOn)
                    completeNotes.record_note_on(incomingEvent, false,
                        configuration.overflow_policy == OverflowPolicy::DropIncomingCompleteNotes);
                record_drop(result, bucketIndex, 1);
            }
        }

        occupancy = pending.count() + (busy ? 1 : 0);
        int64_t queueAge = pending.count() == 0 ? 0
            : std::max<int64_t>(0, wrap_sub(arrival, events.micros_at(pending.oldest_event_index())));
        if (queueAge > result.predicted_peak_queue_age_us) result.predicted_peak_queue_age_us = queueAge;
        WorkloadBucket& target = result.buckets[static_cast<size_t>(bucketIndex)];
        if (queueAge > target.predicted_peak_queue_age_us) target.predicted_peak_queue_age_us = queueAge;
        if (occupancy > result.predicted_maximum_occupancy) result.predicted_maximum_occupancy = occupancy;
        if (occupancy > target.predicted_peak_occupancy) target.predicted_peak_occupancy = occupancy;
    }
    while (busy) {
        lastCompleted = completion;
        apply_analysis_queue_age_limit(pending, events, completeNotes, configuration, result, nowLocator, completion,
            queueArrivalOffset);
        if (pending.count() > 0) {
            int32_t next = pending.dequeue();
            completion = checked_add(completion, serviceClock.next_microseconds(events, next));
        } else busy = false;
    }
    result.predicted_output_completion_us = lastCompleted;
    report(control, "Projecting finite queue pressure", count, count, 750, 250);
}

std::shared_ptr<const AnalysisConfiguration> null_configuration() { return nullptr; }

} // namespace

// ------------------------------------------------------------ public

bool WorkloadAnalysis::has_gate_projection() const
{
    return projection_state == ProjectionState::Complete && configuration != nullptr &&
        (configuration->per_note_interval_gate_enabled || configuration->delayed_note_off_enabled ||
            configuration->note_attack_limit_enabled || configuration->output_rate_limit_enabled) &&
        has_gate_output_buckets;
}

bool WorkloadAnalysis::has_queue_projection() const
{
    return projection_state == ProjectionState::Complete &&
        (configuration == nullptr || !configuration->per_note_interval_gate_enabled &&
            !configuration->delayed_note_off_enabled && !configuration->note_attack_limit_enabled &&
            !configuration->output_rate_limit_enabled);
}

bool WorkloadCache::try_enter(int timeoutMilliseconds)
{
    std::unique_lock<std::mutex> lock(enter_mutex_);
    if (!enter_cv_.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [this] { return !entered_; })) return false;
    entered_ = true;
    return true;
}

void WorkloadCache::leave()
{
    {
        std::lock_guard<std::mutex> lock(enter_mutex_);
        entered_ = false;
    }
    enter_cv_.notify_one();
}

std::shared_ptr<const WorkloadAnalysis> WorkloadCache::find(int64_t bucketMicroseconds) const
{
    auto it = results_.find(bucketMicroseconds);
    return it == results_.end() ? nullptr : it->second;
}

void WorkloadCache::add(int64_t bucketMicroseconds, std::shared_ptr<const WorkloadAnalysis> workload)
{
    int64_t added = static_cast<int64_t>(workload->buckets.size());
    while (!order_.empty() && (results_.size() >= 2 || buckets_ + added > kMaximumBucketCount)) {
        int64_t old = order_.front();
        order_.pop_front();
        buckets_ -= static_cast<int64_t>(results_[old]->buckets.size());
        results_.erase(old);
    }
    if (results_.count(bucketMicroseconds) != 0) models::throw_duplicate_key();
    results_[bucketMicroseconds] = workload;
    order_.push_back(bucketMicroseconds);
    buckets_ += added;
}

int32_t calculate_bucket_count(int64_t durationMicroseconds, int64_t bucketMicroseconds)
{
    if (bucketMicroseconds <= 0) models::throw_argument_out_of_range("bucketMicroseconds");
    // v1 adds unchecked: Int64.Max / 1 + 1 wraps to Int64.Min (the result then reads as 1). Unsigned addition makes the
    // wrap defined on every compiler.
    int64_t bucketCount = static_cast<int64_t>(static_cast<uint64_t>(durationMicroseconds / bucketMicroseconds) + 1u);
    if (bucketCount > kMaximumBucketCount) {
        auto n0 = [](int64_t value) {
            bool negative = value < 0;
            uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
            std::string digits = std::to_string(magnitude);
            std::string text;
            for (size_t i = 0; i < digits.size(); ++i) {
                if (i != 0 && (digits.size() - i) % 3 == 0) text += ',';
                text += digits[i];
            }
            return negative ? "-" + text : text;
        };
        models::throw_invalid_operation(("That resolution would require " + n0(bucketCount) +
            " graph buckets. The safety limit is " + n0(kMaximumBucketCount) +
            " buckets (approximately 128 MiB before drawing caches). Choose a larger interval.").c_str());
    }
    return std::max(1, static_cast<int32_t>(bucketCount));
}

int64_t default_resolution(int64_t durationMicroseconds)
{
    int64_t resolution = kDefaultBucketMicroseconds;
    if (durationMicroseconds > resolution * 5000) {
        // v1 adds unchecked: the sum wraps around for a duration within 4,999 of Int64.Max. Done in unsigned arithmetic so
        // the wrap is defined (the Windows builds always wrapped; GCC and Clang treat signed overflow as impossible).
        int64_t target = static_cast<int64_t>(static_cast<uint64_t>(durationMicroseconds) + 4999u) / 5000;
        resolution = ((target + 9999) / 10000) * 10000;
    }
    return resolution;
}

EventView to_compatibility_event(const EventView& source)
{
    EventView copy = source;
    copy.source_channel = source.channel();
    copy.source_status = source.status();
    if (source.routed_channel_plus_one != 0 && source.data_length > 0) copy.bytes[0] = source.status();
    copy.source_index = source.event_index();
    copy.has_logical_index = false;
    copy.logical_index = 0;
    copy.routed_channel_plus_one = 0;
    return copy;
}

WorkloadAnalysis copy_workload(const WorkloadAnalysis& source, std::shared_ptr<const AnalysisConfiguration> configuration,
    const AnalysisControl& control)
{
    // MemberwiseClone, then fresh buckets carrying only the workload fields.
    WorkloadAnalysis copy = source;
    copy.configuration = configuration;
    copy.buckets.assign(source.buckets.size(), WorkloadBucket());
    for (size_t i = 0; i < source.buckets.size(); ++i) {
        if ((i & 4095) == 0) check(control);
        copy.buckets[i].event_count = source.buckets[i].event_count;
        copy.buckets[i].byte_count = source.buckets[i].byte_count;
        copy.buckets[i].largest_cluster = source.buckets[i].largest_cluster;
    }
    return copy;
}

void project_per_note_gate(const AnalysisSong& song, WorkloadAnalysis& result, const AnalysisConfiguration& configuration,
    const AnalysisControl& control, const GateObserver& observer)
{
    const EventSource& events = song.events;
    int32_t limit = interval_limit(configuration);
    models::PerNoteIntervalGate gate(configuration.processing_us, configuration.interval_polyphony_limit_enabled, limit);
    models::AdaptiveNoteIntervalClock adaptive(configuration.processing_us, configuration.scale_note_interval_by_traffic);
    std::vector<EventView> emissions(128);
    start_gate_buckets(result);
    BucketLocator gateLocator(result.bucket_us, result.gate_output_buckets.size());
    const int32_t count = events.count();
    int32_t next = 0;
    bool completed = false;
    int64_t actions = 0;
    while (next < count || !completed || gate.has_pending_transitions()) {
        if ((actions++ & 4095) == 0) {
            check(control);
            report(control, "Projecting Per-note intervals", next, std::max(1, count), 750, 245);
        }
        int64_t localBoundary = gate.next_boundary_microseconds();
        int64_t source = next < count ? events.micros_at(next) : kInt64Max;
        adaptive.complete_if_closed(localBoundary, source);
        int64_t boundary = adaptive.effective_boundary(localBoundary);
        if (next < count && source <= boundary) {
            int64_t tick = events.tick_at(next);
            gate.begin_source_tick(tick);
            while (next < count && events.tick_at(next) == tick) {
                if ((next & 16383) == 0) check(control);
                EventView midiEvent = events.at(next++);
                if (gate.admit(midiEvent) == models::PerNoteGateAdmission::NotNote) {
                    record_gate_output(result, gateLocator, midiEvent.micros, false);
                    if (observer) observer(midiEvent, midiEvent.micros);
                }
            }
            gate.end_source_tick();
            result.gate_filtered_note_events += gate.take_filtered_event_count();
        } else if (boundary != kInt64Max) {
            int32_t emitted = gate.emit_boundary(boundary, emissions.data(), static_cast<int32_t>(emissions.size()));
            adaptive.record_boundary_pass(boundary, emitted);
            result.gate_filtered_note_events += gate.take_filtered_event_count();
            for (int32_t i = 0; i < emitted; ++i) {
                record_gate_output(result, gateLocator, boundary, true);
                if (observer) observer(emissions[static_cast<size_t>(i)], boundary);
            }
        }
        if (next == count && !completed) {
            gate.complete_source(song.duration_us);
            result.gate_filtered_note_events += gate.take_filtered_event_count();
            completed = true;
        }
    }
    finish_gate_peak(result, control);
    report(control, "Per-note projection complete", 1, 1, 995, 5);
}

void project_delayed_note_off(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control, const GateObserver& observer)
{
    const EventSource& events = song.events;
    int32_t limit = interval_limit(configuration);
    models::DelayedNoteOffGate gate(configuration.processing_us, configuration.interval_polyphony_limit_enabled, limit);
    models::AdaptiveNoteIntervalClock adaptive(configuration.processing_us, configuration.scale_note_interval_by_traffic);
    std::vector<EventView> emissions;
    emissions.reserve(128);
    start_gate_buckets(result);
    BucketLocator gateLocator(result.bucket_us, result.gate_output_buckets.size());
    const int32_t count = events.count();
    int32_t next = 0;
    bool completed = false;
    int64_t actions = 0;
    while (next < count || !completed || gate.has_pending_transitions()) {
        if ((actions++ & 4095) == 0) {
            check(control);
            report(control, "Projecting Delayed NoteOff", next, std::max(1, count), 750, 245);
        }
        int64_t localBoundary = gate.next_boundary_microseconds();
        int64_t source = next < count ? events.micros_at(next) : kInt64Max;
        adaptive.complete_if_closed(localBoundary, source);
        int64_t boundary = adaptive.effective_boundary(localBoundary);
        // Match live scheduling: D-boundary releases win ties, while
        // exact-R source transitions participate before recovery picks
        // its one replacement candidate.
        bool sourcePrecedesRecovery = next < count && gate.source_precedes_boundary(events.at(next), boundary);
        if (boundary < source || boundary == source && !sourcePrecedesRecovery) {
            int32_t emitted = gate.emit_boundary(boundary, emissions);
            adaptive.record_boundary_pass(boundary, emitted);
            result.gate_filtered_note_events += gate.take_filtered_event_count();
            for (int32_t i = 0; i < emitted; ++i) {
                record_gate_output(result, gateLocator, boundary, true);
                if (observer) observer(emissions[static_cast<size_t>(i)], boundary);
            }
        } else if (next < count) {
            if ((next & 16383) == 0) check(control);
            EventView midiEvent = events.at(next++);
            if (gate.admit(midiEvent) == models::DelayedNoteOffAdmission::NotNote) {
                record_gate_output(result, gateLocator, midiEvent.micros, false);
                if (observer) observer(midiEvent, midiEvent.micros);
                if (models::DelayedNoteOffGate::is_note_terminating_control(midiEvent))
                    result.gate_filtered_note_events += gate.retire_channel(midiEvent.channel(), midiEvent.micros);
            }
            result.gate_filtered_note_events += gate.take_filtered_event_count();
        }
        if (next == count && !completed) {
            gate.complete_source(song.duration_us);
            result.gate_filtered_note_events += gate.take_filtered_event_count();
            completed = true;
        }
    }
    finish_gate_peak(result, control);
    report(control, "Delayed NoteOff projection complete", 1, 1, 995, 5);
}

void project_note_attack_limit(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control, const GateObserver& observer)
{
    const EventSource& events = song.events;
    models::MusicalAttackRateLimiter limiter(configuration.events_per_second);
    std::vector<EventView> emissions;
    emissions.reserve(128);
    start_gate_buckets(result);
    BucketLocator gateLocator(result.bucket_us, result.gate_output_buckets.size());
    const int32_t count = events.count();
    int32_t next = 0;
    int64_t actions = 0;
    while (next < count) {
        if ((actions++ & 4095) == 0) {
            check(control);
            report(control, "Projecting Note start rate gate", next, std::max(1, count), 750, 245);
        }
        EventView first = events.at(next);
        int64_t tick = first.tick;
        int64_t time = first.micros;
        limiter.begin_source_tick(tick);
        while (next < count && events.tick_at(next) == tick) {
            if ((next & 16383) == 0) check(control);
            limiter.admit(events.at(next++));
        }
        int32_t emitted = limiter.end_source_tick(emissions);
        int64_t rejected = limiter.take_rejected_attacks();
        int64_t suppressed = limiter.take_suppressed_releases();
        result.musical_limit_rejected_attacks += rejected;
        result.musical_limit_suppressed_releases += suppressed;
        result.gate_filtered_note_events += rejected + suppressed;
        for (int32_t index = 0; index < emitted; ++index) {
            int32_t pitch;
            bool noteOn;
            bool note = models::PerNoteIntervalGate::try_classify(emissions[static_cast<size_t>(index)], pitch, noteOn);
            record_gate_output(result, gateLocator, time, note);
            if (observer) observer(emissions[static_cast<size_t>(index)], time);
        }
    }
    limiter.complete_source();
    finish_gate_peak(result, control);
    report(control, "Note start rate projection complete", 1, 1, 995, 5);
}

void project_output_rate_limit(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control, const GateObserver& observer)
{
    const EventSource& events = song.events;
    models::OutputRateGate gate(configuration.events_per_second, 1000000);
    std::vector<EventView> emissions;
    emissions.reserve(128);
    start_gate_buckets(result);
    BucketLocator gateLocator(result.bucket_us, result.gate_output_buckets.size());
    const int32_t count = events.count();
    int32_t next = 0;
    int64_t actions = 0;
    while (next < count) {
        if ((actions++ & 4095) == 0) {
            check(control);
            report(control, "Projecting Output rate gate", next, std::max(1, count), 750, 245);
        }
        EventView first = events.at(next);
        int64_t tick = first.tick;
        int64_t time = first.micros;
        gate.begin_source_tick(tick);
        while (next < count && events.tick_at(next) == tick) {
            if ((next & 16383) == 0) check(control);
            gate.admit(events.at(next++));
        }
        int32_t emitted = gate.end_source_tick(emissions, time);
        int64_t rejected = gate.take_rejected_attacks();
        int64_t suppressed = gate.take_suppressed_releases();
        int64_t rejectedOther = gate.take_rejected_other_messages();
        result.output_rate_rejected_attacks += rejected;
        result.output_rate_suppressed_releases += suppressed;
        result.output_rate_rejected_other_messages += rejectedOther;
        result.gate_filtered_note_events += rejected + suppressed;
        for (int32_t index = 0; index < emitted; ++index) {
            EventView emitted_event = emissions[static_cast<size_t>(index)];
            if (gate.record_dispatch(time, false)) result.output_rate_safety_soft_excess++;
            record_gate_output(result, gateLocator, time, CompleteNoteTracker::classify(emitted_event) != CompleteNoteEventKind::Other);
            if (observer) observer(emitted_event, time);
        }
    }
    gate.complete_source();
    finish_gate_peak(result, control);
    report(control, "Output rate projection complete", 1, 1, 995, 5);
}

WorkloadAnalysis analyze_uncached(const AnalysisSong& song, int64_t bucketMicroseconds,
    std::shared_ptr<const AnalysisConfiguration> configuration, const AnalysisControl& control,
    WorkloadCache* reentryCache)
{
    if (configuration != nullptr && configuration->override_projection != nullptr) {
        WorkloadCache local;
        return analyze(song, bucketMicroseconds, configuration, reentryCache ? *reentryCache : local, control);
    }
    const EventSource& events = song.events;
    int32_t bucketCount = calculate_bucket_count(song.duration_us, bucketMicroseconds);
    WorkloadAnalysis result;
    result.configuration = configuration;
    result.total_events = events.count();
    result.duration_us = song.duration_us;
    result.bucket_us = bucketMicroseconds;
    result.buckets.reserve(static_cast<size_t>(bucketCount));

    for (int32_t i = 0; i < bucketCount; ++i) {
        if ((i & 4095) == 0) {
            check(control);
            report(control, "Initializing graph buckets", i, bucketCount, 0, 50);
        }
        result.buckets.push_back(WorkloadBucket());
    }
    report(control, "Initializing graph buckets", bucketCount, bucketCount, 0, 50);

    const int typeCount = 9;  // Enum.GetValues(typeof(MidiEventKind)).Length
    int64_t typeEvents[typeCount] = {};
    int64_t typeBytes[typeCount] = {};
    scan_workload(events, result, bucketMicroseconds, bucketCount, configuration.get(), typeEvents, typeBytes, control);
    report(control, "Scanning events and clusters", events.count(), events.count(), 50, 650);

    double bucketSeconds = static_cast<double>(bucketMicroseconds) / 1000000.0;
    for (size_t i = 0; i < result.buckets.size(); ++i) {
        if ((i & 4095) == 0) {
            check(control);
            report(control, "Summarizing graph buckets", static_cast<int64_t>(i),
                static_cast<int64_t>(result.buckets.size()), 700, 50);
        }
        result.peak_events_per_second = net_max(result.peak_events_per_second,
            static_cast<double>(result.buckets[i].event_count) / bucketSeconds);
        result.peak_bytes_per_second = net_max(result.peak_bytes_per_second,
            static_cast<double>(result.buckets[i].byte_count) / bucketSeconds);
    }
    report(control, "Summarizing graph buckets", static_cast<int64_t>(result.buckets.size()),
        static_cast<int64_t>(result.buckets.size()), 700, 50);
    if (song.duration_us > 0)
        result.average_events_per_second =
            static_cast<double>(events.count()) / (static_cast<double>(song.duration_us) / 1000000.0);

    auto types = std::make_shared<std::vector<MessageTypeWorkload>>();
    types->reserve(typeCount);
    for (int kind = 0; kind < typeCount; ++kind) {
        if (typeEvents[kind] == 0) continue;
        MessageTypeWorkload entry;
        entry.kind = static_cast<EventKind>(kind);
        entry.event_count = typeEvents[kind];
        entry.byte_count = typeBytes[kind];
        types->push_back(entry);
    }
    result.message_types = types;

    if (configuration != nullptr) {
        const AnalysisConfiguration& c = *configuration;
        if (c.per_note_interval_gate_enabled) {
            project_per_note_gate(song, result, c, control, nullptr);
            result.projection_state = ProjectionState::Complete;
            return result;
        }
        if (c.delayed_note_off_enabled) {
            project_delayed_note_off(song, result, c, control, nullptr);
            result.projection_state = ProjectionState::Complete;
            return result;
        }
        if (c.note_attack_limit_enabled) {
            project_note_attack_limit(song, result, c, control, nullptr);
            result.projection_state = ProjectionState::Complete;
            return result;
        }
        if (c.output_rate_limit_enabled) {
            project_output_rate_limit(song, result, c, control, nullptr);
            result.projection_state = ProjectionState::Complete;
            return result;
        }
        if (uses_configured_service(&c) && c.service_duration_mode == ServiceDurationMode::ProcessingTime &&
            c.processing_us > 0)
            result.event_service_capacity_per_second = 1000000.0 / static_cast<double>(c.processing_us);
        if (uses_configured_service(&c) && c.service_duration_mode == ServiceDurationMode::MidiBitrate &&
            c.midi_bitrate > 0)
            result.byte_service_capacity_per_second = static_cast<double>(c.midi_bitrate) / 10.0;
        if (uses_configured_service(&c) && c.service_duration_mode == ServiceDurationMode::EventsPerSecond) {
            add_event_rate_demand(events, result, bucketMicroseconds, c, control);
            result.event_service_capacity_per_second = static_cast<double>(c.events_per_second);
        }
        analyze_pressure(song, result, c, control);
    } else report(control, "File workload complete", 1, 1, 0, 750);
    return result;
}

WorkloadAnalysis analyze(const AnalysisSong& song, int64_t bucketMicroseconds,
    std::shared_ptr<const AnalysisConfiguration> configuration, WorkloadCache& cache, const AnalysisControl& control,
    const std::function<void(const WorkloadAnalysis&)>& workloadReady)
{
    std::shared_ptr<const WorkloadAnalysis> workload;
    {
        // v1: while (!Monitor.TryEnter(cache, 25)) cancellationToken.ThrowIfCancellationRequested();
        while (!cache.try_enter(25)) check(control);
        struct Leave {
            WorkloadCache& cache;
            ~Leave() { cache.leave(); }
        } leave{ cache };
        check(control);
        workload = cache.find(bucketMicroseconds);
        if (!workload) {
            workload = std::make_shared<WorkloadAnalysis>(
                analyze_uncached(song, bucketMicroseconds, null_configuration(), control, &cache));
            cache.add(bucketMicroseconds, workload);
        }
    }
    check(control);
    if (workloadReady) workloadReady(*workload);
    WorkloadAnalysis result = copy_workload(*workload, configuration, control);
    report(control, "Reusing file workload", 1, 1, 0, 750);
    if (configuration == nullptr) {
        report(control, "Complete", 1, 1, 0, 1000);
        return result;
    }
    const AnalysisConfiguration& c = *configuration;
    const AnalysisSong* projectionSong = &song;
    FilteredSong filtered;
    if (c.override_projection != nullptr) {
        int64_t filteredCount;
        create_override_filtered_song(song, *c.override_projection, control, filtered, filteredCount);
        result.override_filtered_source_messages = filteredCount;
        projectionSong = &filtered.song;
    }
    const EventSource& events = projectionSong->events;
    if (c.per_note_interval_gate_enabled) {
        project_per_note_gate(*projectionSong, result, c, control, nullptr);
        apply_projected_forced_overrides(result, c);
        result.projection_state = ProjectionState::Complete;
        return result;
    }
    if (c.delayed_note_off_enabled) {
        project_delayed_note_off(*projectionSong, result, c, control, nullptr);
        apply_projected_forced_overrides(result, c);
        result.projection_state = ProjectionState::Complete;
        return result;
    }
    if (c.note_attack_limit_enabled) {
        project_note_attack_limit(*projectionSong, result, c, control, nullptr);
        apply_projected_forced_overrides(result, c);
        result.projection_state = ProjectionState::Complete;
        return result;
    }
    if (c.output_rate_limit_enabled) {
        project_output_rate_limit(*projectionSong, result, c, control, nullptr);
        apply_projected_forced_overrides(result, c);
        result.projection_state = ProjectionState::Complete;
        return result;
    }
    if (uses_configured_service(&c)) {
        if (c.service_duration_mode == ServiceDurationMode::ProcessingTime) {
            if (c.override_projection == nullptr) {
                for (size_t i = 0; i < result.buckets.size(); ++i) {
                    if ((i & 4095) == 0) check(control);
                    result.buckets[i].service_demand_us = checked_mul(result.buckets[i].event_count, c.processing_us);
                }
            } else {
                const int32_t count = events.count();
                BucketLocator locator(bucketMicroseconds, result.buckets.size());
                for (int32_t i = 0; i < count; ++i) {
                    if ((i & 16383) == 0) check(control);
                    int32_t bucket = locator.index(events.micros_at(i));
                    WorkloadBucket& target = result.buckets[static_cast<size_t>(bucket)];
                    target.service_demand_us = wrap_add(target.service_demand_us, c.processing_us);
                }
            }
            if (c.processing_us > 0)
                result.event_service_capacity_per_second = 1000000.0 / static_cast<double>(c.processing_us);
        } else if (c.service_duration_mode == ServiceDurationMode::MidiBitrate) {
            // Per-message rounding is exact; aggregate byte totals
            // cannot substitute for the sum of rounded service times.
            BucketLocator locator(bucketMicroseconds, result.buckets.size());
            const int32_t count = events.count();
            for (int32_t i = 0; i < count; ++i) {
                if ((i & 16383) == 0) {
                    check(control);
                    report(control, "Calculating serial service demand", i, count, 750, 0);
                }
                EventView midiEvent = events.at(i);
                int32_t bucket = locator.index(midiEvent.micros);
                WorkloadBucket& target = result.buckets[static_cast<size_t>(bucket)];
                target.service_demand_us = wrap_add(target.service_demand_us,
                    models::calculate_service_microseconds(midiEvent, c.service_duration_mode, c.processing_us,
                        c.midi_bitrate));
            }
            result.byte_service_capacity_per_second = static_cast<double>(c.midi_bitrate) / 10.0;
        } else {
            add_event_rate_demand(events, result, bucketMicroseconds, c, control);
            result.event_service_capacity_per_second = static_cast<double>(c.events_per_second);
        }
    }
    analyze_pressure(*projectionSong, result, c, control);
    apply_projected_forced_overrides(result, c);
    result.projection_state = ProjectionState::Complete;
    return result;
}

} // namespace analysis
} // namespace midibn
