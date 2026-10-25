// Playback engine value types (Milestone 5, Group D): v1 Model.cs
// ProcessingMode, PlaybackState, PlaybackSnapshot; ServiceDurationCalculator.cs
// ServiceDurationSettings; the error value that stands for a .NET exception;
// the song as the engine sees it (v1 MidiSong); and v1's transport time
// conversion (PlaybackEngine.MicrosecondsToTicks / TicksToMicroseconds).
#pragma once

#include "midibn/channel_index.h"
#include "midibn/event_store.h"
#include "midibn/models/model_base.h"
#include "midibn/models/ordered_admission.h"
#include "midibn/models/rate.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
struct Song;

namespace engine {

using models::OverflowPolicy;
using models::ServiceDurationMode;

// v1 Model.cs ProcessingMode (the order is part of the API).
enum class ProcessingMode : int32_t {
    Queue = 0,
    Drop,
    PerNoteIntervalGate,
    DelayedNoteOff,
    NoteAttackLimit,
    OutputRateLimit
};

// v1 Model.cs PlaybackState.
enum class PlaybackState : int32_t { Stopped = 0, Playing, Paused, Completed };

const char* processing_mode_name(ProcessingMode mode);
const char* playback_state_name(PlaybackState state);

// A .NET exception as v1 would raise it: the exception type name (as
// Exception.GetType().Name), its Message, and the first inner exception
// (InnerException), when there is one. Every public engine operation that
// throws in v1 returns this instead (ok() when v1 does not throw).
struct EngineError {
    std::string type;
    std::string message;
    std::string inner_type;
    std::string inner_message;

    bool ok() const { return type.empty(); }
    static EngineError none() { return EngineError(); }
    static EngineError make(const char* typeName, const std::string& text)
    {
        EngineError error;
        error.type = typeName;
        error.message = text;
        return error;
    }
};

// v1 ServiceDurationSettings: published as one immutable object; the
// worker compares object identity (v1 Object.ReferenceEquals), so a
// replacement with equal values still restarts the service phase.
struct ServiceDurationSettings {
    ServiceDurationMode mode;
    int64_t processing_microseconds;
    int64_t midi_bitrate;
    int64_t events_per_second;

    ServiceDurationSettings(ServiceDurationMode m, int64_t processing, int64_t bitrate, int64_t eps)
        : mode(m), processing_microseconds(processing), midi_bitrate(bitrate), events_per_second(eps) {}
    models::ServiceDurationClock create_clock() const
    { return models::ServiceDurationClock(mode, processing_microseconds, midi_bitrate, events_per_second); }
    bool is_immediate() const
    {
        return (mode == ServiceDurationMode::ProcessingTime && processing_microseconds == 0) ||
            (mode == ServiceDurationMode::EventsPerSecond && events_per_second == 0);
    }
};

// v1 Model.cs PlaybackSnapshot (same fields, same values).
struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::Stopped;
    int64_t processing_microseconds = 0;
    int64_t queue_length = 0;
    int64_t outstanding_events = 0;
    int64_t maximum_queue_length = 0;
    bool queue_age_limit_enabled = false;
    int64_t queue_age_limit_microseconds = 0;
    int64_t queue_age_microseconds = 0;
    int64_t maximum_queue_age_microseconds = 0;
    int64_t processed_events = 0;
    int64_t dropped_events = 0;
    int64_t gate_filtered_events = 0;
    int64_t musical_limit_rejected_attacks = 0;
    int64_t musical_limit_suppressed_releases = 0;
    int64_t output_rate_rejected_attacks = 0;
    int64_t output_rate_suppressed_releases = 0;
    int64_t output_rate_rejected_other_messages = 0;
    int64_t output_rate_safety_soft_excess = 0;
    int64_t output_rate_safety_bypass_messages = 0;
    bool virtual_queue_active = false;
    int64_t virtual_queue_length = 0;
    int64_t virtual_outstanding_events = 0;
    int64_t virtual_maximum_queue_length = 0;
    int64_t playback_microseconds = 0;
    int64_t intended_timeline_microseconds = 0;
    int64_t last_dispatched_timeline_microseconds = 0;
    int64_t effective_speed_frontier_microseconds = 0;
    int64_t current_lag_microseconds = 0;
    int64_t maximum_lag_microseconds = 0;
    ServiceDurationMode service_duration_mode = ServiceDurationMode::ProcessingTime;
    int64_t midi_bitrate = 0;
    int64_t events_per_second = 0;
    bool simulate_slowdown = false;
    bool queue_length_limit_enabled = false;
    int32_t queue_length_limit = 0;
    OverflowPolicy overflow_policy = OverflowPolicy::DropNewest;
    ProcessingMode processing_mode = ProcessingMode::Queue;
};

// The song as v1's engine uses MidiSong: its event reader (the compact
// store, or the immutable track-routed view), its duration, file path, and
// the whole-state chase and source-value indexes built over that same event
// sequence. Compare by identity (v1 compares MidiSong references).
//
// SysEx payloads are read from `payload_store` at the event's source index
// (a routed view keeps source events' indices; generated routing events are
// never SysEx).
struct PlaybackSong {
    models::EventSource events;
    const EventStore* payload_store = nullptr;
    int64_t duration_us = 0;
    std::string file_path;                         // UTF-8 (v1 MidiSong.FilePath)
    const ChaseIndex* chase_index = nullptr;       // over `events`
    const SourceValueIndex* source_values = nullptr;  // over `events`

    // Owned indexes when they had to be built for a derived view (v1 builds
    // them lazily from MidiSong.EventStore).
    std::shared_ptr<ChaseIndex> owned_chase_index;
    std::shared_ptr<SourceValueIndex> owned_source_values;
    std::shared_ptr<const void> keep_alive;        // keeps the source data alive

    // v1 ChannelSourceValueIndex.TryGetLatest over `events`.
    bool try_get_latest_source_value(int channel, ChannelAttribute attribute, int64_t position_us, int& value) const;
};

// The loaded song's own store and indexes. `song` must outlive the result
// unless `keep_alive` owns it.
std::shared_ptr<PlaybackSong> make_playback_song(const Song& song, const std::string& filePathUtf8,
    std::shared_ptr<const void> keepAlive = nullptr);
// A derived view (e.g. routing::TrackRoutedEventStore) over `song`'s store:
// builds the chase and source-value indexes over the view, as v1 does.
std::shared_ptr<PlaybackSong> make_playback_song(const Song& song, const models::EventViewStore& view,
    const std::string& filePathUtf8, std::shared_ptr<const void> keepAlive = nullptr);

// v1 PlaybackEngine.MicrosecondsToTicks / TicksToMicroseconds for one clock
// frequency: the split quotient/remainder formula under `checked` (overflow
// throws models::ModelException OverflowException), and v1's decimal branch
// for a frequency above Int64.MaxValue / 1,000,000.
class TimeConversion {
public:
    explicit TimeConversion(int64_t frequency) : frequency_(frequency) {}
    int64_t frequency() const { return frequency_; }
    int64_t microseconds_to_ticks(int64_t microseconds) const;
    int64_t ticks_to_microseconds(int64_t ticks) const;
    // The largest non-negative `microseconds` with microseconds_to_ticks(microseconds) <= ticks, so that "is this
    // event due" is one comparison of the event's time with a number computed once, instead of a conversion (two
    // multiplications and a division) for every event. Returns false, and the caller converts each event as before,
    // when the answer is not available in 64-bit arithmetic (negative ticks, a huge tick count, or the decimal
    // branch of a very fast clock).
    bool latest_microseconds_due(int64_t ticks, int64_t& microseconds) const
    {
        const int64_t scale = 1000000;
        if (frequency_ > models::kInt64Max / scale || ticks < 0 || ticks >= models::kInt64Max / scale - 1) return false;
        // microseconds_to_ticks(m) is floor(m * frequency / 1e6) for m >= 0, which is <= ticks exactly when
        // m * frequency <= (ticks + 1) * 1e6 - 1.
        microseconds = ((ticks + 1) * scale - 1) / frequency_;
        return true;
    }

private:
    int64_t frequency_;
};

// v1 PlaybackEngine.SaturatingAddTicks.
inline int64_t saturating_add_ticks(int64_t left, int64_t right)
{
    if (right <= 0) return left;
    return left > models::kInt64Max - right ? models::kInt64Max : left + right;
}

// v1 Ordered*Adapter / OrderedFiniteAdmissionCore.DispatchDueClockTicks.
int64_t dispatch_due_clock_ticks(const models::OrderedPlaybackGenerationQueue& generations,
    const models::OutputWorkItem& item, const TimeConversion& time);

} // namespace engine
} // namespace midibn
