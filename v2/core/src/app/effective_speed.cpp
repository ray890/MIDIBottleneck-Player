#include "midibn/app/effective_speed.h"

#include <algorithm>
#include <cmath>

namespace midibn {
namespace app {

bool EffectivePlaybackSpeed::is_valid_window(int64_t microseconds)
{
    return microseconds == kInstantaneousWindowMicroseconds ||
        (microseconds >= kMinimumCustomWindowMicroseconds && microseconds <= kMaximumCustomWindowMicroseconds);
}

bool EffectivePlaybackSpeed::set_window(int64_t microseconds)
{
    if (!is_valid_window(microseconds)) return false;
    if (window_microseconds_ == microseconds) return true;
    window_microseconds_ = microseconds;
    reset();
    return true;
}

std::optional<double> EffectivePlaybackSpeed::add(int64_t playbackClockMicroseconds,
    int64_t intendedTimelineMicroseconds, int64_t outputFrontierMicroseconds, int64_t processedEvents,
    bool synchronized, int64_t sampleTimeMicroseconds)
{
    int64_t frontier = synchronized ? intendedTimelineMicroseconds : outputFrontierMicroseconds;
    if (!samples_.empty()) {
        const Sample& last = samples_.back();
        if (playbackClockMicroseconds < last.clock_microseconds || frontier < last.frontier_microseconds ||
            processedEvents < last.processed_events || sampleTimeMicroseconds <= last.sample_time_microseconds)
            reset();
    }

    Sample current;
    current.clock_microseconds = playbackClockMicroseconds;
    current.frontier_microseconds = frontier;
    current.processed_events = processedEvents;
    current.sample_time_microseconds = sampleTimeMicroseconds;
    samples_.push_back(current);

    int64_t retention = window_microseconds_ == kInstantaneousWindowMicroseconds
        ? kMaximumCustomWindowMicroseconds : std::max<int64_t>(window_microseconds_ * 2, 500000);
    size_t remove = 0;
    while (remove + 1 < samples_.size() &&
        sampleTimeMicroseconds - samples_[remove + 1].sample_time_microseconds > retention)
        remove++;
    if (remove > 0) samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(remove));

    return window_microseconds_ == kInstantaneousWindowMicroseconds
        ? calculate_instantaneous(current) : calculate_regression(current);
}

std::optional<double> EffectivePlaybackSpeed::calculate_instantaneous(const Sample& current) const
{
    for (int i = static_cast<int>(samples_.size()) - 2; i >= 0; i--) {
        const Sample& previous = samples_[static_cast<size_t>(i)];
        if (previous.frontier_microseconds == current.frontier_microseconds) continue;
        int64_t clockAdvance = current.clock_microseconds - previous.clock_microseconds;
        int64_t frontierAdvance = current.frontier_microseconds - previous.frontier_microseconds;
        if (clockAdvance <= 0 || frontierAdvance <= 0) return std::nullopt;
        return 100.0 * static_cast<double>(frontierAdvance) / static_cast<double>(clockAdvance);
    }
    return std::nullopt;
}

std::optional<double> EffectivePlaybackSpeed::calculate_regression(const Sample& current) const
{
    int64_t from = current.sample_time_microseconds - window_microseconds_;
    size_t first = 0;
    while (first + 1 < samples_.size() && samples_[first + 1].sample_time_microseconds < from) first++;
    int count = static_cast<int>(samples_.size() - first);
    int64_t span = current.sample_time_microseconds - samples_[first].sample_time_microseconds;
    int64_t minimumSpan = std::min<int64_t>(100000, std::max<int64_t>(25000, window_microseconds_ / 2));
    if (count < 3 || span < minimumSpan) return std::nullopt;

    double originX = static_cast<double>(samples_[first].clock_microseconds);
    double originY = static_cast<double>(samples_[first].frontier_microseconds);
    double meanX = 0;
    double meanY = 0;
    for (size_t i = first; i < samples_.size(); i++) {
        meanX += static_cast<double>(samples_[i].clock_microseconds) - originX;
        meanY += static_cast<double>(samples_[i].frontier_microseconds) - originY;
    }
    meanX /= count;
    meanY /= count;
    double covariance = 0;
    double variance = 0;
    for (size_t i = first; i < samples_.size(); i++) {
        double x = (static_cast<double>(samples_[i].clock_microseconds) - originX) - meanX;
        double y = (static_cast<double>(samples_[i].frontier_microseconds) - originY) - meanY;
        covariance += x * y;
        variance += x * x;
    }
    if (variance <= 0 || std::fabs(covariance) < 0.0001) return std::nullopt;
    return 100.0 * covariance / variance;
}

void RollingOutputRate::reset()
{
    samples_.clear();
    maximum_observed_.reset();
}

std::optional<double> RollingOutputRate::add(int64_t processedEvents, int64_t sampleTimeMicroseconds)
{
    if (!samples_.empty()) {
        const Sample& last = samples_.back();
        if (processedEvents < last.count || sampleTimeMicroseconds <= last.time_microseconds) reset();
    }
    Sample current;
    current.count = processedEvents;
    current.time_microseconds = sampleTimeMicroseconds;
    samples_.push_back(current);
    while (samples_.size() > 2 && sampleTimeMicroseconds - samples_.front().time_microseconds > kWindowMicroseconds)
        samples_.pop_front();
    const Sample& first = samples_.front();
    int64_t elapsed = sampleTimeMicroseconds - first.time_microseconds;
    int64_t count = processedEvents - first.count;
    if (elapsed < 100000 || count <= 0) return std::nullopt;
    double rate = static_cast<double>(count) * 1000000.0 / static_cast<double>(elapsed);
    if (!maximum_observed_.has_value() || rate > *maximum_observed_) maximum_observed_ = rate;
    return rate;
}

} // namespace app
} // namespace midibn
