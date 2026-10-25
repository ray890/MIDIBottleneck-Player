#include "midibn/app/analysis_view.h"

#include <algorithm>

namespace midibn {
namespace app {

using analysis::AnalysisConfiguration;
using analysis::ProjectionState;
using analysis::WorkloadAnalysis;

namespace {

bool same_override(const std::shared_ptr<const overrides::AnalysisOverrideSnapshot>& left,
    const std::shared_ptr<const overrides::AnalysisOverrideSnapshot>& right)
{
    if (left == right) return true;
    return left != nullptr && right != nullptr && left->signature() == right->signature();
}

bool gate_selected(const AnalysisConfiguration& c)
{
    return c.per_note_interval_gate_enabled || c.delayed_note_off_enabled || c.note_attack_limit_enabled ||
        c.output_rate_limit_enabled;
}

} // namespace

bool same_analysis_configuration(const AnalysisConfiguration* l, const AnalysisConfiguration* r)
{
    if (l == nullptr || r == nullptr) return l == r;
    return l->simulate_slowdown == r->simulate_slowdown && l->service_duration_mode == r->service_duration_mode &&
        l->processing_us == r->processing_us && l->midi_bitrate == r->midi_bitrate &&
        l->events_per_second == r->events_per_second && l->queue_length_limit_enabled == r->queue_length_limit_enabled &&
        l->queue_length_limit == r->queue_length_limit && l->queue_age_limit_enabled == r->queue_age_limit_enabled &&
        l->queue_age_limit_us == r->queue_age_limit_us && l->overflow_policy == r->overflow_policy &&
        l->per_note_interval_gate_enabled == r->per_note_interval_gate_enabled &&
        l->delayed_note_off_enabled == r->delayed_note_off_enabled &&
        l->scale_note_interval_by_traffic == r->scale_note_interval_by_traffic &&
        l->interval_polyphony_limit_enabled == r->interval_polyphony_limit_enabled &&
        l->interval_polyphony_limit == r->interval_polyphony_limit &&
        l->note_attack_limit_enabled == r->note_attack_limit_enabled &&
        l->output_rate_limit_enabled == r->output_rate_limit_enabled &&
        l->output_paced_timeline == r->output_paced_timeline &&
        l->apply_queue_limit_without_slowdown == r->apply_queue_limit_without_slowdown &&
        same_override(l->override_projection, r->override_projection);
}

bool AnalysisResultView::uses_gate() const { return configuration != nullptr && gate_selected(*configuration); }

bool AnalysisResultView::has_gate_projection() const
{
    return state == ProjectionState::Complete && configuration != nullptr && gate_selected(*configuration) &&
        data != nullptr && data->has_gate_output_buckets;
}

bool AnalysisResultView::has_queue_projection() const
{
    return state == ProjectionState::Complete && (configuration == nullptr || !gate_selected(*configuration));
}

bool AnalysisResultView::uses_byte_rate() const
{
    return data != nullptr && configuration != nullptr && !gate_selected(*configuration) &&
        configuration->service_duration_mode == models::ServiceDurationMode::MidiBitrate;
}

AnalysisResultView AnalysisResultView::of(std::shared_ptr<const WorkloadAnalysis> data)
{
    AnalysisResultView view;
    view.configuration = data->configuration;
    view.state = data->projection_state;
    view.failure_reason = data->projection_failure_reason;
    view.data = std::move(data);
    return view;
}

AnalysisResultView AnalysisResultView::preview(std::shared_ptr<const WorkloadAnalysis> baseWorkload,
    std::shared_ptr<const AnalysisConfiguration> configuration)
{
    AnalysisResultView view;
    view.data = std::move(baseWorkload);
    view.configuration = std::move(configuration);
    view.state = ProjectionState::Pending;
    return view;
}

AnalysisResultView AnalysisResultView::with_configuration(std::shared_ptr<const AnalysisConfiguration> value) const
{
    if (configuration != nullptr && value != nullptr && same_analysis_configuration(configuration.get(), value.get())) return *this;
    AnalysisResultView copy = *this;
    copy.configuration = std::move(value);
    return copy;
}

AnalysisResultView AnalysisResultView::with_projection_state(ProjectionState value) const
{
    if (state == value) return *this;
    AnalysisResultView copy = *this;
    copy.state = value;
    return copy;
}

AnalysisResultView AnalysisResultView::with_projection_failure(const std::string& reason) const
{
    AnalysisResultView copy = *this;
    copy.state = ProjectionState::Failed;
    copy.failure_reason = reason;
    return copy;
}

int32_t analysis_bucket_index_at(const AnalysisResultView& view, int64_t timeMicroseconds)
{
    int64_t last = static_cast<int64_t>(view.data->buckets.size()) - 1;
    int64_t index = timeMicroseconds / std::max<int64_t>(1, view.data->bucket_us);
    return static_cast<int32_t>(std::max<int64_t>(0, std::min(last, index)));
}

const analysis::WorkloadBucket* analysis_bucket_at(const AnalysisResultView& view, int64_t timeMicroseconds)
{
    if (!view.valid() || view.data->buckets.empty()) return nullptr;
    return &view.data->buckets[static_cast<size_t>(analysis_bucket_index_at(view, timeMicroseconds))];
}

} // namespace app
} // namespace midibn
