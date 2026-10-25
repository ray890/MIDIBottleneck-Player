// Group 1 models (Milestone 3): service durations and rate pacers.
// Ports of v1 ServiceDurationCalculator.cs, ProgressiveRatePacer.cs and
// AdaptiveNoteIntervalClock.cs with identical integer arithmetic.
#pragma once

#include "midibn/models/model_base.h"

#include <deque>

namespace midibn {
namespace models {

struct TraceAccess;

enum class ServiceDurationMode : int32_t { ProcessingTime = 0, MidiBitrate = 1, EventsPerSecond = 2 };

// v1 ServiceDurationClock (a value type): the events/sec remainder carries
// the fractional microsecond between accepted service starts.
class ServiceDurationClock {
public:
    ServiceDurationClock(ServiceDurationMode mode, int64_t processingMicroseconds, int64_t midiBitrate,
        int64_t eventsPerSecond);
    int64_t next_microseconds(const EventView& midiEvent);
    // The same as next_microseconds(events.at(index)), but the event is decoded only in the mode that reads it (MIDI
    // bitrate); the Analysis queue projection asks this once per serviced event (DD-096).
    int64_t next_microseconds(const EventSource& events, int32_t index)
    {
        if (mode_ == ServiceDurationMode::ProcessingTime) return processing_microseconds_;   // clamped to >= 0
        if (mode_ == ServiceDurationMode::EventsPerSecond) return next_events_per_second();
        return next_microseconds(events.at(index));
    }
    bool is_immediate() const;
    int64_t total_for_next_events(int64_t count) const;

private:
    friend struct TraceAccess;
    int64_t next_events_per_second();
    ServiceDurationMode mode_;
    int64_t processing_microseconds_;
    int64_t midi_bitrate_;
    int64_t events_per_second_;
    int64_t remainder_ = 0;
};

// v1 ServiceDurationSettings.IsImmediate (on the raw, unclamped settings).
bool service_settings_immediate(ServiceDurationMode mode, int64_t processingMicroseconds, int64_t eventsPerSecond);

const int64_t kFivePinDinBitrate = 31250;
int64_t calculate_service_microseconds(const EventView& midiEvent, ServiceDurationMode mode,
    int64_t processingMicroseconds, int64_t midiBitrate);
int64_t calculate_bitrate_microseconds(int32_t midiByteCount, int64_t bitrate);

// v1 ProgressiveRatePacer: exact rational pacing with a 50 ms burst
// tolerance and a rolling one-second ledger.
class ProgressiveRatePacer {
public:
    ProgressiveRatePacer(int64_t rate, int64_t unitsPerSecond, int64_t minimumBurstSlots = 1);

    int64_t rate() const { return rate_; }
    bool is_initialized() const { return initialized_; }
    int64_t next_whole() const { return next_whole_; }
    int64_t next_remainder() const { return next_remainder_; }

    int64_t available_slots(int64_t timestamp, int64_t maximum);
    bool try_consume(int64_t timestamp);
    bool try_reserve(int64_t timestamp) { return try_reserve(timestamp, false); }
    bool try_reserve_ahead(int64_t timestamp) { return try_reserve(timestamp, true); }
    int64_t rolling_available_slots(int64_t timestamp, int64_t maximum);
    void complete_reservation(int64_t timestamp);
    void cancel_reservation();

private:
    friend struct TraceAccess;
    struct StampCount {
        int64_t timestamp;
        int64_t count;
    };

    bool try_reserve(int64_t timestamp, bool allowAhead);
    void prepare(int64_t timestamp);
    bool is_within_burst(int64_t whole, int64_t remainder, int64_t timestamp) const;
    void expire(int64_t timestamp);
    void record(int64_t timestamp);
    void advance(int64_t& whole, int64_t& remainder, bool& exhausted) const;

    int64_t rate_;
    int64_t units_per_second_;
    int64_t burst_slots_;
    std::deque<StampCount> history_;
    bool has_tail_ = false;
    StampCount tail_ = { 0, 0 };
    bool initialized_ = false;
    bool exhausted_ = false;
    int64_t next_whole_ = 0;
    int64_t next_remainder_ = 0;
    int64_t history_count_ = 0;
    int64_t reserved_count_ = 0;
};

// v1 AdaptiveNoteIntervalClock, shared by Playback and Analysis.
class AdaptiveNoteIntervalClock {
public:
    AdaptiveNoteIntervalClock(int64_t intervalMicroseconds, bool enabled);

    bool enabled() const { return enabled_; }
    int64_t next_allowed_boundary() const { return next_allowed_boundary_; }
    int64_t open_boundary() const { return open_boundary_; }

    int64_t effective_boundary(int64_t localBoundaryMicroseconds) const;
    void record_boundary_pass(int64_t boundaryMicroseconds, int32_t emittedNoteMessages);
    bool complete_if_closed(int64_t nextLocalBoundaryMicroseconds, int64_t nextSourceMicroseconds);
    void clear();

    static int64_t saturating_multiply(int64_t value, int64_t multiplier);
    static int64_t saturating_add(int64_t value, int64_t increment);

private:
    friend struct TraceAccess;
    bool enabled_;
    int64_t interval_microseconds_;
    int64_t next_allowed_boundary_ = 0;
    int64_t open_boundary_ = kInt64Max;
    int32_t open_output_count_ = 0;
};

} // namespace models
} // namespace midibn
