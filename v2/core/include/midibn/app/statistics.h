// The measurement side of v1's MainForm.RefreshStatistics / ResetLiveMeasurements /
// RestartLiveMeasurementWindows: the effective-speed and output-rate windows,
// the "synchronized" test, and the dropped-event pulse that colours the
// queue-pressure bar for 0.8 s. The text comes from midibn/format/display_format.h.
#pragma once

#include "midibn/app/effective_speed.h"
#include "midibn/engine/engine_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace midibn {
namespace app {

// What one refresh measured.
struct StatisticsSample {
    std::optional<double> effective_speed;     // percent, only while Playing
    std::optional<double> output_rate;         // events/sec, only while Playing
    std::optional<double> maximum_observed_rate;
    bool synchronized = false;
    int64_t total_dropped = 0;
    bool overflow_pulse = false;               // a drop happened within the last 800 ms
};

class LiveStatistics {
public:
    static constexpr int64_t kOverflowPulseMicroseconds = 800000;

    EffectivePlaybackSpeed& effective_speed() { return effective_speed_; }
    const EffectivePlaybackSpeed& effective_speed() const { return effective_speed_; }
    const RollingOutputRate& output_rate() const { return output_rate_; }

    // ResetLiveMeasurements (Reset stats, new song, Stop): clears everything.
    void reset();
    // RestartLiveMeasurementWindows (live setting changes): clears the windows, keeps the maximum.
    void restart_windows();
    // One RefreshStatistics tick; `sampleTimeMicroseconds` is a monotonic clock in microseconds.
    StatisticsSample update(const engine::PlaybackSnapshot& snapshot, int64_t sampleTimeMicroseconds);

private:
    EffectivePlaybackSpeed effective_speed_;
    RollingOutputRate output_rate_;
    int64_t last_dropped_events_ = 0;
    int64_t overflow_visible_until_microseconds_ = 0;
};

// MainForm.RefreshStatistics' `synchronized` test.
bool statistics_synchronized(const engine::PlaybackSnapshot& snapshot);

// The queue-pressure bar's inputs (RefreshStatistics -> StatisticsView.SetQueuePressure).
struct QueuePressureInput {
    bool limited = false;
    int64_t occupied = 0;
    int64_t limit = 0;
    bool virtual_queue = false;
    bool age_pressure = false;
};
// `queueLimitChecked`: the queue-limit check box; the four flags: a note gate or the output-rate gate selected;
// `queueAgeLimitEnabled`; `queueLimitValue`: the queue length box's value.
QueuePressureInput queue_pressure_input(const engine::PlaybackSnapshot& snapshot, bool queueLimitChecked,
    bool perNoteIntervalGateEnabled, bool delayedNoteOffEnabled, bool noteAttackLimitEnabled,
    bool outputRateLimitEnabled, bool queueAgeLimitEnabled, int64_t queueLimitValue);

// The "Effective speed" right-click menu: the preset windows and their captions.
struct SpeedWindowPreset {
    const char* caption;
    int64_t microseconds;
};
const std::vector<SpeedWindowPreset>& speed_window_presets();
// The custom-window dialog: its starting value in milliseconds (an instantaneous window starts at 250 ms)
// and its range, and the conversion of the dialog's milliseconds back to microseconds.
constexpr int kSpeedWindowDialogMinimumMs = 25;
constexpr int kSpeedWindowDialogMaximumMs = 10000;
constexpr int kSpeedWindowDialogIncrementMs = 25;
double speed_window_dialog_initial_ms(int64_t currentWindowMicroseconds);
int64_t speed_window_from_dialog_ms(double milliseconds);

} // namespace app
} // namespace midibn
