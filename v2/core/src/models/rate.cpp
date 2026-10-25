#include "midibn/models/rate.h"

namespace midibn {
namespace models {

// ------------------------------------------------------ service durations

ServiceDurationClock::ServiceDurationClock(ServiceDurationMode mode, int64_t processingMicroseconds,
    int64_t midiBitrate, int64_t eventsPerSecond)
    : mode_(mode),
      processing_microseconds_(max64(0, processingMicroseconds)),
      midi_bitrate_(max64(1, midiBitrate)),
      events_per_second_(max64(0, min64(9999999, eventsPerSecond)))
{
}

int64_t ServiceDurationClock::next_microseconds(const EventView& midiEvent)
{
    if (mode_ == ServiceDurationMode::EventsPerSecond) return next_events_per_second();
    return calculate_service_microseconds(midiEvent, mode_, processing_microseconds_, midi_bitrate_);
}

int64_t ServiceDurationClock::next_events_per_second()
{
    if (events_per_second_ == 0) return 0;
    int64_t numerator = remainder_ + 1000000;
    // The rate is at most 9,999,999 and the remainder below it, so both operands nearly always fit in 32 bits: then
    // one 32-bit division gives the same quotient and remainder as the 64-bit pair, which on the 32-bit builds are two
    // library calls (DD-096).
    if (static_cast<uint64_t>(numerator) <= 0xFFFFFFFFu) {
        const uint32_t n = static_cast<uint32_t>(numerator);
        const uint32_t d = static_cast<uint32_t>(events_per_second_);
        remainder_ = static_cast<int64_t>(n % d);
        return static_cast<int64_t>(n / d);
    }
    int64_t duration = numerator / events_per_second_;
    remainder_ = numerator % events_per_second_;
    return duration;
}

bool ServiceDurationClock::is_immediate() const
{
    return (mode_ == ServiceDurationMode::ProcessingTime && processing_microseconds_ == 0) ||
        (mode_ == ServiceDurationMode::EventsPerSecond && events_per_second_ == 0);
}

int64_t ServiceDurationClock::total_for_next_events(int64_t count) const
{
    if (count <= 0) return 0;
    if (mode_ == ServiceDurationMode::EventsPerSecond) {
        if (events_per_second_ == 0) return 0;
        return checked_add(remainder_, checked_mul(count, 1000000)) / events_per_second_;
    }
    if (mode_ == ServiceDurationMode::ProcessingTime) return checked_mul(count, processing_microseconds_);
    throw_invalid_operation("MIDI bitrate service depends on each message's byte count.");
}

bool service_settings_immediate(ServiceDurationMode mode, int64_t processingMicroseconds, int64_t eventsPerSecond)
{
    return (mode == ServiceDurationMode::ProcessingTime && processingMicroseconds == 0) ||
        (mode == ServiceDurationMode::EventsPerSecond && eventsPerSecond == 0);
}

int64_t calculate_service_microseconds(const EventView& midiEvent, ServiceDurationMode mode,
    int64_t processingMicroseconds, int64_t midiBitrate)
{
    if (mode == ServiceDurationMode::ProcessingTime) return max64(0, processingMicroseconds);
    if (mode == ServiceDurationMode::EventsPerSecond)
        throw_invalid_operation("Events/sec requires a generation-owned ServiceDurationClock.");
    if (!midiEvent.is_valid()) throw_argument("midiEvent", "A valid MIDI event is required.");
    return calculate_bitrate_microseconds(midiEvent.data_length, midiBitrate);
}

int64_t calculate_bitrate_microseconds(int32_t midiByteCount, int64_t bitrate)
{
    if (midiByteCount < 0) throw_argument_out_of_range("midiByteCount");
    if (bitrate <= 0) throw_argument_out_of_range("bitrate");
    if (midiByteCount == 0) return 0;
    // Ten transmitted bits per MIDI byte; rounded up (v1 comment).
    // (At most 2^31 x 10 x 10^6 = 2.1 x 10^16, so the product cannot overflow.) v1 computed
    // (numerator + bitrate - 1) / bitrate, which threw OverflowException for a bitrate near Int64.Max; the
    // rounded-up quotient is the same without the intermediate sum (register entry DD-004).
    int64_t numerator = checked_mul(checked_mul(midiByteCount, 10), 1000000);
    int64_t quotient = numerator / bitrate;
    return numerator % bitrate != 0 ? quotient + 1 : quotient;
}

// ------------------------------------------------------ progressive pacer

ProgressiveRatePacer::ProgressiveRatePacer(int64_t rate, int64_t unitsPerSecond, int64_t minimumBurstSlots)
{
    if (rate < 0 || rate > 9999999) throw_argument_out_of_range("rate");
    if (unitsPerSecond <= 0) throw_argument_out_of_range("unitsPerSecond");
    if (minimumBurstSlots <= 0) throw_argument_out_of_range("minimumBurstSlots");
    rate_ = rate;
    units_per_second_ = unitsPerSecond;
    burst_slots_ = rate == 0 ? kInt64Max : max64(minimumBurstSlots, (rate + 19) / 20);
}

int64_t ProgressiveRatePacer::available_slots(int64_t timestamp, int64_t maximum)
{
    if (maximum <= 0) return 0;
    if (rate_ == 0) return maximum;
    prepare(timestamp);
    int64_t used = history_count_ > kInt64Max - reserved_count_ ? kInt64Max : history_count_ + reserved_count_;
    int64_t rolling = max64(0, wrap_sub(rate_, used));
    maximum = min64(maximum, rolling);
    int64_t whole = next_whole_;
    int64_t remainder = next_remainder_;
    bool exhausted = exhausted_;
    int64_t count = 0;
    while (count < maximum && !exhausted && is_within_burst(whole, remainder, timestamp)) {
        count++;
        advance(whole, remainder, exhausted);
    }
    return count;
}

bool ProgressiveRatePacer::try_consume(int64_t timestamp)
{
    if (rate_ == 0) return true;
    prepare(timestamp);
    if (wrap_add(history_count_, reserved_count_) >= rate_ || exhausted_ ||
        !is_within_burst(next_whole_, next_remainder_, timestamp)) return false;
    advance(next_whole_, next_remainder_, exhausted_);
    record(max64(0, timestamp));   // the same clamped time that expiry uses (v1 recorded the unclamped one; DD-004)
    return true;
}

int64_t ProgressiveRatePacer::rolling_available_slots(int64_t timestamp, int64_t maximum)
{
    if (maximum <= 0) return 0;
    if (rate_ == 0) return maximum;
    prepare(timestamp);
    return min64(maximum, max64(0, wrap_sub(wrap_sub(rate_, history_count_), reserved_count_)));
}

bool ProgressiveRatePacer::try_reserve(int64_t timestamp, bool allowAhead)
{
    if (rate_ == 0) return true;
    prepare(timestamp);
    if (wrap_add(history_count_, reserved_count_) >= rate_ || exhausted_ ||
        (!allowAhead && !is_within_burst(next_whole_, next_remainder_, timestamp))) return false;
    advance(next_whole_, next_remainder_, exhausted_);
    reserved_count_++;
    return true;
}

void ProgressiveRatePacer::complete_reservation(int64_t timestamp)
{
    if (rate_ == 0) return;
    if (reserved_count_ <= 0) throw_invalid_operation("The rate pacer has no reservation to complete.");
    prepare(timestamp);
    reserved_count_--;
    record(max64(0, timestamp));   // the same clamped time that expiry uses (v1 recorded the unclamped one; DD-004)
}

void ProgressiveRatePacer::cancel_reservation()
{
    if (rate_ == 0) return;
    if (reserved_count_ <= 0) throw_invalid_operation("The rate pacer has no reservation to cancel.");
    reserved_count_--;
}

void ProgressiveRatePacer::prepare(int64_t timestamp)
{
    timestamp = max64(0, timestamp);
    expire(timestamp);
    if (!initialized_) {
        initialized_ = true;
        exhausted_ = false;
        next_whole_ = timestamp;
        next_remainder_ = 0;
        return;
    }
    // Missed slots are discarded once the clock passes the next slot.
    if (!exhausted_ && next_whole_ < timestamp) {
        next_whole_ = timestamp;
        next_remainder_ = 0;
    }
}

bool ProgressiveRatePacer::is_within_burst(int64_t whole, int64_t remainder, int64_t timestamp) const
{
    int64_t slots = wrap_sub(burst_slots_, 1);
    int64_t numerator = wrap_add(wrap_mul(slots, units_per_second_), rate_ - 1);
    int64_t toleranceWhole = numerator / rate_;
    int64_t toleranceRemainder = numerator % rate_;
    int64_t allowedWhole = timestamp > wrap_sub(kInt64Max, toleranceWhole) ? kInt64Max : wrap_add(timestamp, toleranceWhole);
    if (whole < allowedWhole) return true;
    if (whole > allowedWhole) return false;
    if (allowedWhole == kInt64Max && timestamp > wrap_sub(kInt64Max, toleranceWhole)) return true;
    return remainder <= toleranceRemainder;
}

void ProgressiveRatePacer::expire(int64_t timestamp)
{
    int64_t cutoff = timestamp >= units_per_second_ ? timestamp - units_per_second_ : kInt64Min;
    while (!history_.empty() && history_.front().timestamp <= cutoff) {
        history_count_ = wrap_sub(history_count_, history_.front().count);
        history_.pop_front();
    }
    if (has_tail_ && tail_.timestamp <= cutoff) {
        history_count_ = wrap_sub(history_count_, tail_.count);
        tail_ = StampCount{ 0, 0 };
        has_tail_ = false;
    }
}

void ProgressiveRatePacer::record(int64_t timestamp)
{
    if (has_tail_ && tail_.timestamp > timestamp) throw_invalid_operation("Rate-pacer timestamps must be monotonic.");
    if (has_tail_ && tail_.timestamp == timestamp) {
        tail_.count++;
        history_count_++;
        return;
    }
    if (has_tail_) history_.push_back(tail_);
    tail_ = StampCount{ timestamp, 1 };
    has_tail_ = true;
    history_count_++;
}

void ProgressiveRatePacer::advance(int64_t& whole, int64_t& remainder, bool& exhausted) const
{
    int64_t wholeStep = units_per_second_ / rate_;
    int64_t remainderStep = units_per_second_ % rate_;
    if (whole > kInt64Max - wholeStep) {
        exhausted = true;
        return;
    }
    whole += wholeStep;
    remainder += remainderStep;
    if (remainder >= rate_) {
        remainder -= rate_;
        if (whole == kInt64Max) {
            exhausted = true;
            return;
        }
        whole++;
    }
}

// ------------------------------------------------- adaptive note interval

AdaptiveNoteIntervalClock::AdaptiveNoteIntervalClock(int64_t intervalMicroseconds, bool enabled)
{
    if (intervalMicroseconds < 0) throw_argument_out_of_range("intervalMicroseconds");
    interval_microseconds_ = intervalMicroseconds;
    enabled_ = enabled;
}

int64_t AdaptiveNoteIntervalClock::effective_boundary(int64_t localBoundaryMicroseconds) const
{
    if (!enabled_ || localBoundaryMicroseconds == kInt64Max) return localBoundaryMicroseconds;
    if (open_boundary_ != kInt64Max && localBoundaryMicroseconds <= open_boundary_) return open_boundary_;
    return max64(localBoundaryMicroseconds, next_allowed_boundary_);
}

void AdaptiveNoteIntervalClock::record_boundary_pass(int64_t boundaryMicroseconds, int32_t emittedNoteMessages)
{
    if (!enabled_) return;
    if (open_boundary_ == kInt64Max) open_boundary_ = boundaryMicroseconds;
    else if (open_boundary_ != boundaryMicroseconds)
        throw_invalid_operation("An adaptive note boundary was advanced before its batch closed.");
    if (emittedNoteMessages <= 0) return;
    open_output_count_ = open_output_count_ > kInt32Max - emittedNoteMessages ? kInt32Max
        : open_output_count_ + emittedNoteMessages;
}

bool AdaptiveNoteIntervalClock::complete_if_closed(int64_t nextLocalBoundaryMicroseconds,
    int64_t nextSourceMicroseconds)
{
    if (!enabled_ || open_boundary_ == kInt64Max || nextLocalBoundaryMicroseconds <= open_boundary_ ||
        nextSourceMicroseconds <= open_boundary_) return false;
    int64_t multiplier = max64(1, open_output_count_);
    int64_t delay = saturating_multiply(interval_microseconds_, multiplier);
    next_allowed_boundary_ = saturating_add(open_boundary_, delay);
    open_boundary_ = kInt64Max;
    open_output_count_ = 0;
    return true;
}

void AdaptiveNoteIntervalClock::clear()
{
    next_allowed_boundary_ = 0;
    open_boundary_ = kInt64Max;
    open_output_count_ = 0;
}

int64_t AdaptiveNoteIntervalClock::saturating_multiply(int64_t value, int64_t multiplier)
{
    if (value <= 0 || multiplier <= 0) return 0;
    return value > kInt64Max / multiplier ? kInt64Max : value * multiplier;
}

int64_t AdaptiveNoteIntervalClock::saturating_add(int64_t value, int64_t increment)
{
    if (increment <= 0) return value;
    return value > kInt64Max - increment ? kInt64Max : value + increment;
}

} // namespace models
} // namespace midibn
