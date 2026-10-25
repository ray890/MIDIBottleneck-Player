// Exact ports of v1's EffectivePlaybackSpeed and RollingOutputRate
// (EffectivePlaybackSpeed.cs): the measurement windows behind the
// "Effective speed" and "Output rate" statistics. Doubles are computed in
// v1's operation order.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace midibn {
namespace app {

class EffectivePlaybackSpeed {
public:
    static constexpr int64_t kInstantaneousWindowMicroseconds = 0;
    static constexpr int64_t kDefaultWindowMicroseconds = 250000;
    static constexpr int64_t kMinimumCustomWindowMicroseconds = 25000;
    static constexpr int64_t kMaximumCustomWindowMicroseconds = 10000000;

    // 0 (instantaneous) or 25 ms .. 10 s.
    static bool is_valid_window(int64_t microseconds);
    // v1's WindowMicroseconds setter. False (nothing changed) where v1 throws
    // ArgumentOutOfRange; a changed window clears the samples.
    bool set_window(int64_t microseconds);
    int64_t window_microseconds() const { return window_microseconds_; }
    void reset() { samples_.clear(); }

    // `synchronized` selects the intended timeline as the frontier instead of the output frontier.
    std::optional<double> add(int64_t playbackClockMicroseconds, int64_t intendedTimelineMicroseconds,
        int64_t outputFrontierMicroseconds, int64_t processedEvents, bool synchronized, int64_t sampleTimeMicroseconds);

private:
    struct Sample {
        int64_t clock_microseconds;
        int64_t frontier_microseconds;
        int64_t processed_events;
        int64_t sample_time_microseconds;
    };
    std::optional<double> calculate_instantaneous(const Sample& current) const;
    std::optional<double> calculate_regression(const Sample& current) const;

    std::vector<Sample> samples_;
    int64_t window_microseconds_ = kDefaultWindowMicroseconds;
};

class RollingOutputRate {
public:
    void reset();
    void restart_window() { samples_.clear(); }
    std::optional<double> maximum_observed() const { return maximum_observed_; }
    std::optional<double> add(int64_t processedEvents, int64_t sampleTimeMicroseconds);

private:
    struct Sample {
        int64_t count;
        int64_t time_microseconds;
    };
    static constexpr int64_t kWindowMicroseconds = 1000000;
    std::deque<Sample> samples_;
    std::optional<double> maximum_observed_;
};

} // namespace app
} // namespace midibn
