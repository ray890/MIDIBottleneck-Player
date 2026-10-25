#include "midibn/app/statistics.h"

#include "midibn/format/display_format.h"
#include "midibn/format/net_format.h"

#include <algorithm>

namespace midibn {
namespace app {

bool statistics_synchronized(const engine::PlaybackSnapshot& s)
{
    using engine::ProcessingMode;
    return s.processing_mode != ProcessingMode::PerNoteIntervalGate &&
        s.processing_mode != ProcessingMode::DelayedNoteOff &&
        s.processing_mode != ProcessingMode::NoteAttackLimit &&
        s.processing_mode != ProcessingMode::OutputRateLimit &&
        s.outstanding_events == 0 && s.current_lag_microseconds == 0 &&
        s.last_dispatched_timeline_microseconds <= s.intended_timeline_microseconds;
}

void LiveStatistics::reset()
{
    effective_speed_.reset();
    output_rate_.reset();
    last_dropped_events_ = 0;
    overflow_visible_until_microseconds_ = 0;
}

void LiveStatistics::restart_windows()
{
    effective_speed_.reset();
    output_rate_.restart_window();
    last_dropped_events_ = 0;
    overflow_visible_until_microseconds_ = 0;
}

StatisticsSample LiveStatistics::update(const engine::PlaybackSnapshot& snapshot, int64_t sampleTime)
{
    StatisticsSample sample;
    sample.synchronized = statistics_synchronized(snapshot);
    bool playing = snapshot.state == engine::PlaybackState::Playing;
    if (playing) {
        sample.effective_speed = effective_speed_.add(snapshot.playback_microseconds,
            snapshot.intended_timeline_microseconds, snapshot.effective_speed_frontier_microseconds,
            snapshot.processed_events, sample.synchronized, sampleTime);
        sample.output_rate = output_rate_.add(snapshot.processed_events, sampleTime);
    }
    sample.maximum_observed_rate = output_rate_.maximum_observed();
    int64_t totalDropped = format::total_dropped_events(snapshot);
    if (totalDropped > last_dropped_events_) overflow_visible_until_microseconds_ = sampleTime + kOverflowPulseMicroseconds;
    last_dropped_events_ = totalDropped;
    sample.total_dropped = totalDropped;
    sample.overflow_pulse = sampleTime < overflow_visible_until_microseconds_;
    return sample;
}

QueuePressureInput queue_pressure_input(const engine::PlaybackSnapshot& snapshot, bool queueLimitChecked,
    bool perNoteIntervalGateEnabled, bool delayedNoteOffEnabled, bool noteAttackLimitEnabled,
    bool outputRateLimitEnabled, bool queueAgeLimitEnabled, int64_t queueLimitValue)
{
    QueuePressureInput input;
    input.limited = queueLimitChecked && !perNoteIntervalGateEnabled && !delayedNoteOffEnabled &&
        !noteAttackLimitEnabled && !outputRateLimitEnabled;
    input.occupied = queueAgeLimitEnabled ? snapshot.queue_age_microseconds :
        snapshot.virtual_queue_active ? snapshot.virtual_outstanding_events : snapshot.outstanding_events;
    input.limit = queueAgeLimitEnabled ? snapshot.queue_age_limit_microseconds : queueLimitValue;
    input.occupied = std::max<int64_t>(0, input.occupied);   // StatisticsView.SetQueuePressure
    input.limit = std::max<int64_t>(1, input.limit);
    input.virtual_queue = snapshot.virtual_queue_active;
    input.age_pressure = queueAgeLimitEnabled;
    return input;
}

const std::vector<SpeedWindowPreset>& speed_window_presets()
{
    static const std::vector<SpeedWindowPreset> presets = {
        { "Instantaneous", 0 }, { "100 ms", 100000 }, { "250 ms", 250000 }, { "500 ms", 500000 }, { "1.5 s", 1500000 }
    };
    return presets;
}

double speed_window_dialog_initial_ms(int64_t currentWindowMicroseconds)
{
    int64_t window = currentWindowMicroseconds == 0 ? 250000 : currentWindowMicroseconds;
    double milliseconds = static_cast<double>(window) / 1000.0;
    return std::max(static_cast<double>(kSpeedWindowDialogMinimumMs),
        std::min(static_cast<double>(kSpeedWindowDialogMaximumMs), milliseconds));
}

int64_t speed_window_from_dialog_ms(double milliseconds)
{
    return format::round_half_even(milliseconds * 1000.0);
}

} // namespace app
} // namespace midibn
