// A WorkloadAnalysis as an Analysis window holds and shows it (v1 keeps one immutable
// WorkloadAnalysis object and makes shallow copies with WithConfiguration /
// WithProjectionState / WithProjectionFailure / CreatePresentationAnalysis). Here the big
// bucket arrays stay in one shared, immutable `data`; a view only swaps the small parts
// v1's copies change, so previews, cancelled states and cache hits cost nothing.
#pragma once

#include "midibn/analysis/workload_analysis.h"
#include "midibn/format/net_format.h"

#include <cstdint>
#include <memory>
#include <string>

namespace midibn {
namespace app {

struct AnalysisResultView {
    std::shared_ptr<const analysis::WorkloadAnalysis> data;
    std::shared_ptr<const analysis::AnalysisConfiguration> configuration;   // may be null (v1 Configuration == null)
    analysis::ProjectionState state = analysis::ProjectionState::Complete;
    std::string failure_reason;

    bool valid() const { return data != nullptr; }
    const analysis::WorkloadAnalysis& workload() const { return *data; }

    // v1 HasGateProjection / HasQueueProjection (on this view's configuration and state).
    bool uses_gate() const;                    // the configuration selects one of the four gates
    bool has_gate_projection() const;
    bool has_queue_projection() const;
    bool queue_limit_enabled() const { return configuration != nullptr && configuration->queue_length_limit_enabled; }
    // WorkloadGraph.UsesByteRate: queue model in MIDI-bitrate mode.
    bool uses_byte_rate() const;

    int64_t bucket_us() const { return data->bucket_us; }
    int64_t duration_us() const { return data->duration_us; }
    size_t bucket_count() const { return data->buckets.size(); }

    // A finished analysis (its own configuration, state and failure text).
    static AnalysisResultView of(std::shared_ptr<const analysis::WorkloadAnalysis> data);
    // v1 WorkloadBaseAnalysis.CreatePresentationAnalysis: the workload-only preview, projection pending.
    static AnalysisResultView preview(std::shared_ptr<const analysis::WorkloadAnalysis> baseWorkload,
        std::shared_ptr<const analysis::AnalysisConfiguration> configuration);
    // v1 WithConfiguration: the same view when the configuration is equal (override snapshots compare by signature).
    AnalysisResultView with_configuration(std::shared_ptr<const analysis::AnalysisConfiguration> value) const;
    AnalysisResultView with_projection_state(analysis::ProjectionState value) const;
    AnalysisResultView with_projection_failure(const std::string& reason) const;
};

// v1 WithConfiguration's field-by-field equality (null equals only null).
bool same_analysis_configuration(const analysis::AnalysisConfiguration* left, const analysis::AnalysisConfiguration* right);

// WorkloadGraph.BucketAt: the bucket holding a time (clamped), or null when there are no buckets.
const analysis::WorkloadBucket* analysis_bucket_at(const AnalysisResultView& view, int64_t timeMicroseconds);
// The clamped index DiagnosticsForm.UpdateInspection computes for a time.
int32_t analysis_bucket_index_at(const AnalysisResultView& view, int64_t timeMicroseconds);

} // namespace app
} // namespace midibn
