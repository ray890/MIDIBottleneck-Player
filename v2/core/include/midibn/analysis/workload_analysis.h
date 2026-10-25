// Analysis core (Milestone 4): port of v1 WorkloadAnalysis.cs. File workload
// bucketing, raw workload, the projections for each Rate model and gate
// (sharing the Milestone 3 model classes, as v1 shares them with Playback),
// queue pressure and age, the Output-paced timeline projection, and the
// narrow forced-override projection.
//
// Deterministic core only. Analysis scheduling (request generations, worker
// tasks, the 25 ms cache-lock polling) and presentation live in v1's
// DiagnosticsForm and move to the app layer later. The per-song base
// workload cache that v1 keeps inside WorkloadAnalyzer is ported here
// (WorkloadCache), because whether it hits changes the progress sequence.
//
// Errors are thrown as models::ModelException at v1's throw points
// (ArgumentOutOfRangeException, InvalidOperationException, OverflowException,
// OperationCanceledException).
#pragma once

#include "midibn/models/model_base.h"
#include "midibn/models/ordered_admission.h"
#include "midibn/models/rate.h"
#include "midibn/overrides/override_snapshot.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <string>
#include <vector>

namespace midibn {
namespace analysis {

using models::EventSource;
using models::EventView;


const int64_t kDefaultBucketMicroseconds = 100000;
const int32_t kMaximumBucketCount = 2000000;
const int32_t kDefaultIntervalPolyphonyLimit = 128;  // v1 PlaybackEngine.DefaultIntervalPolyphonyLimit


struct WorkloadBucket {
    int32_t event_count = 0;
    int32_t largest_cluster = 0;
    int64_t byte_count = 0;
    int64_t service_demand_us = 0;
    int64_t predicted_peak_queue_age_us = 0;
    int32_t predicted_peak_occupancy = 0;
    int32_t predicted_dropped_events = 0;
    int32_t predicted_buffer_clears = 0;
};

struct AnalysisConfiguration {
    bool simulate_slowdown = false;
    models::ServiceDurationMode service_duration_mode = models::ServiceDurationMode::ProcessingTime;
    int64_t processing_us = 0;
    int64_t midi_bitrate = 0;
    int64_t events_per_second = 0;
    bool queue_length_limit_enabled = false;
    int32_t queue_length_limit = 0;
    bool queue_age_limit_enabled = false;
    int64_t queue_age_limit_us = 0;
    models::OverflowPolicy overflow_policy = models::OverflowPolicy::DropNewest;
    bool per_note_interval_gate_enabled = false;
    bool delayed_note_off_enabled = false;
    bool scale_note_interval_by_traffic = false;
    bool interval_polyphony_limit_enabled = false;
    int32_t interval_polyphony_limit = kDefaultIntervalPolyphonyLimit;
    bool note_attack_limit_enabled = false;
    bool output_rate_limit_enabled = false;
    bool output_paced_timeline = false;
    bool apply_queue_limit_without_slowdown = true;
    std::shared_ptr<const overrides::AnalysisOverrideSnapshot> override_projection;
};

struct MessageTypeWorkload {
    EventKind kind = EventKind::NoteOff;
    int64_t event_count = 0;
    int64_t byte_count = 0;
};

enum class ProjectionState : int32_t { Complete = 0, Pending = 1, Cancelled = 2, Failed = 3 };

struct WorkloadAnalysis {
    int64_t total_events = 0;
    int64_t total_bytes = 0;
    int64_t unique_timestamps = 0;
    int32_t largest_timestamp_cluster = 0;
    double average_events_per_second = 0;
    double peak_events_per_second = 0;
    double peak_bytes_per_second = 0;
    int64_t events_in_clusters_at_least_2 = 0;
    int64_t events_in_clusters_at_least_10 = 0;
    int64_t events_in_clusters_at_least_50 = 0;
    int64_t events_in_clusters_at_least_100 = 0;
    int64_t bucket_us = 0;
    int64_t duration_us = 0;
    std::vector<WorkloadBucket> buckets;
    // Shared with the cached base workload, as v1 shares the list reference.
    std::shared_ptr<const std::vector<MessageTypeWorkload>> message_types;
    std::shared_ptr<const AnalysisConfiguration> configuration;
    int32_t predicted_maximum_occupancy = 0;
    int64_t predicted_peak_queue_age_us = 0;
    int64_t predicted_dropped_events = 0;
    int32_t predicted_buffer_clears = 0;
    int64_t predicted_output_completion_us = 0;
    double event_service_capacity_per_second = 0;
    double byte_service_capacity_per_second = 0;
    // Present only for a gate projection (v1 GateOutputBuckets != null).
    bool has_gate_output_buckets = false;
    std::vector<int64_t> gate_output_buckets;
    int64_t gate_output_events = 0;
    int64_t gate_output_note_transitions = 0;
    int64_t gate_output_non_note_events = 0;
    int64_t gate_filtered_note_events = 0;
    int64_t musical_limit_rejected_attacks = 0;
    int64_t musical_limit_suppressed_releases = 0;
    int64_t output_rate_rejected_attacks = 0;
    int64_t output_rate_suppressed_releases = 0;
    int64_t output_rate_rejected_other_messages = 0;
    int64_t output_rate_safety_soft_excess = 0;
    int64_t override_projected_forced_messages = 0;
    int64_t override_filtered_source_messages = 0;
    int64_t gate_output_completion_us = 0;
    double gate_peak_events_per_second = 0;
    ProjectionState projection_state = ProjectionState::Complete;
    std::string projection_failure_reason;

    bool has_gate_projection() const;
    bool has_queue_projection() const;
};

// v1 WorkloadAnalysisProgress.
struct AnalysisProgress {
    const char* stage;
    int32_t overall_permille;
    int32_t stage_permille;
};

// Progress and cancellation (v1 Action<WorkloadAnalysisProgress> and
// CancellationToken). Both may be empty.
struct AnalysisControl {
    std::function<void(const AnalysisProgress&)> progress;
    std::function<bool()> cancellation_requested;
};

// Optional per-emission observer of a gate projection (v1's parity-test hook).
using GateObserver = std::function<void(const EventView&, int64_t)>;

// The song as Analysis reads it: its (possibly routed) event view and duration.
struct AnalysisSong {
    EventSource events;
    int64_t duration_us = 0;
};

// v1 SongWorkloads: base workloads per bucket size, at most two results and
// at most kMaximumBucketCount buckets in total, evicted oldest first.
class WorkloadCache {
public:
    std::shared_ptr<const WorkloadAnalysis> find(int64_t bucketMicroseconds) const;
    void add(int64_t bucketMicroseconds, std::shared_ptr<const WorkloadAnalysis> workload);
    std::mutex& sync() { return sync_; }
    // v1 Monitor.TryEnter(cache, timeout) / Monitor.Exit(cache): the cache is held while a base workload
    // is looked up or scanned. Waits up to `timeoutMilliseconds`; true when the caller now holds it.
    bool try_enter(int timeoutMilliseconds);
    void leave();
    std::vector<int64_t> order() const { return std::vector<int64_t>(order_.begin(), order_.end()); }
    int64_t bucket_total() const { return buckets_; }

private:
    std::mutex sync_;
    std::mutex enter_mutex_;
    std::condition_variable enter_cv_;
    bool entered_ = false;
    std::map<int64_t, std::shared_ptr<const WorkloadAnalysis>> results_;
    std::deque<int64_t> order_;
    int64_t buckets_ = 0;
};

int32_t calculate_bucket_count(int64_t durationMicroseconds, int64_t bucketMicroseconds);
int64_t default_resolution(int64_t durationMicroseconds);

// v1 WorkloadAnalyzer.Analyze(song, bucket, configuration, token, progress,
// workloadReady): base workload from the cache (or a fresh scan), copied,
// then projected for `configuration` (may be null).
WorkloadAnalysis analyze(const AnalysisSong& song, int64_t bucketMicroseconds,
    std::shared_ptr<const AnalysisConfiguration> configuration, WorkloadCache& cache, const AnalysisControl& control,
    const std::function<void(const WorkloadAnalysis&)>& workloadReady = nullptr);

// v1 WorkloadAnalyzer.AnalyzeUncached. With an override projection it
// returns analyze() over a private cache, as v1 re-enters the cached path.
WorkloadAnalysis analyze_uncached(const AnalysisSong& song, int64_t bucketMicroseconds,
    std::shared_ptr<const AnalysisConfiguration> configuration, const AnalysisControl& control,
    WorkloadCache* reentryCache = nullptr);

// The four gate projections (v1 internal entry points, with the observer).
void project_per_note_gate(const AnalysisSong& song, WorkloadAnalysis& result, const AnalysisConfiguration& configuration,
    const AnalysisControl& control, const GateObserver& observer);
void project_delayed_note_off(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control, const GateObserver& observer);
void project_note_attack_limit(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control, const GateObserver& observer);
void project_output_rate_limit(const AnalysisSong& song, WorkloadAnalysis& result,
    const AnalysisConfiguration& configuration, const AnalysisControl& control, const GateObserver& observer);

// v1 CopyWorkload (bucket workload fields only; projections cleared).
WorkloadAnalysis copy_workload(const WorkloadAnalysis& source, std::shared_ptr<const AnalysisConfiguration> configuration,
    const AnalysisControl& control);

// v1 MidiEventView.ToCompatibilityEvent: an event as a copied reference event
// (routed channel/status folded into the source fields).
EventView to_compatibility_event(const EventView& source);

} // namespace analysis
} // namespace midibn
