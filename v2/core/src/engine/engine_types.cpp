#include "midibn/engine/engine_types.h"

#include "midibn/midi_loader.h"

namespace midibn {
namespace engine {

const char* processing_mode_name(ProcessingMode mode)
{
    switch (mode) {
    case ProcessingMode::Queue: return "Queue";
    case ProcessingMode::Drop: return "Drop";
    case ProcessingMode::PerNoteIntervalGate: return "PerNoteIntervalGate";
    case ProcessingMode::DelayedNoteOff: return "DelayedNoteOff";
    case ProcessingMode::NoteAttackLimit: return "NoteAttackLimit";
    case ProcessingMode::OutputRateLimit: return "OutputRateLimit";
    }
    return "?";
}

const char* playback_state_name(PlaybackState state)
{
    switch (state) {
    case PlaybackState::Stopped: return "Stopped";
    case PlaybackState::Playing: return "Playing";
    case PlaybackState::Paused: return "Paused";
    case PlaybackState::Completed: return "Completed";
    }
    return "?";
}

// ---------------------------------------------------------------- song

bool PlaybackSong::try_get_latest_source_value(int channel, ChannelAttribute attribute, int64_t position_us,
    int& value) const
{
    value = 0;
    if (channel < 0 || channel >= 16 || source_values == nullptr) return false;
    const IndexSeries& series = source_values->slot(channel * kChannelAttributeCount + static_cast<int>(attribute));
    if (series.count() == 0) return false;
    int32_t low = 0;
    int32_t high = series.count();
    while (low < high) {
        int32_t middle = low + ((high - low) >> 1);
        int32_t eventIndex = series[middle].event_index;
        int64_t micros = events.is_compact() ? events.store->records[eventIndex].micros : events.at(eventIndex).micros;
        if (micros <= position_us) low = middle + 1;
        else high = middle;
    }
    if (low == 0) return false;
    value = series[low - 1].value;
    return true;
}

std::shared_ptr<PlaybackSong> make_playback_song(const Song& song, const std::string& filePathUtf8,
    std::shared_ptr<const void> keepAlive)
{
    std::shared_ptr<PlaybackSong> result = std::make_shared<PlaybackSong>();
    result->events = models::EventSource(&song.store);
    result->payload_store = &song.store;
    result->duration_us = song.duration_us;
    result->file_path = filePathUtf8;
    result->chase_index = &song.chase_index;
    result->source_values = &song.source_index;
    result->keep_alive = std::move(keepAlive);
    return result;
}

std::shared_ptr<PlaybackSong> make_playback_song(const Song& song, const models::EventViewStore& view,
    const std::string& filePathUtf8, std::shared_ptr<const void> keepAlive)
{
    std::shared_ptr<PlaybackSong> result = std::make_shared<PlaybackSong>();
    result->events = models::EventSource(&view);
    result->payload_store = &song.store;
    result->duration_us = song.duration_us;
    result->file_path = filePathUtf8;
    // v1 MidiStateChaseIndex.Build / ChannelSourceValueIndex.Build over the
    // song's (routed) EventStore: Builder.Add(events.GetEvent(i), i).
    std::shared_ptr<ChaseIndex> chase = std::make_shared<ChaseIndex>();
    std::shared_ptr<SourceValueIndex> values = std::make_shared<SourceValueIndex>();
    int32_t count = view.count();
    for (int32_t i = 0; i < count; ++i) {
        models::EventView e = view.at(i);
        if (!e.is_valid()) continue;
        IndexInput input;
        input.channel = e.channel();
        input.status = e.status();
        input.length = e.data_length;
        input.data1 = e.data_length > 1 ? e.data_byte(1) : 0;
        input.data2 = e.data_length > 2 ? e.data_byte(2) : 0;
        chase->add(input, i);
        values->add(input, i);
    }
    chase->complete(CancellationCheck());
    values->complete(CancellationCheck());
    result->owned_chase_index = chase;
    result->owned_source_values = values;
    result->chase_index = chase.get();
    result->source_values = values.get();
    result->keep_alive = std::move(keepAlive);
    return result;
}

// ---------------------------------------------------------------- time

namespace {

// Unsigned 128-bit helpers for v1's decimal branch (frequency above
// Int64.MaxValue / 1,000,000). Portable: no __int128, no intrinsics.
struct U128 {
    uint64_t hi;
    uint64_t lo;
};

U128 multiply(uint64_t a, uint64_t b)
{
    uint64_t aLo = a & 0xFFFFFFFFu, aHi = a >> 32;
    uint64_t bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
    uint64_t p0 = aLo * bLo;
    uint64_t p1 = aLo * bHi;
    uint64_t p2 = aHi * bLo;
    uint64_t p3 = aHi * bHi;
    uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
    U128 r;
    r.lo = (middle << 32) | (p0 & 0xFFFFFFFFu);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    return r;
}

// Truncating division of a 128-bit value by a non-zero 64-bit divisor;
// returns false when the quotient does not fit 64 bits.
bool divide(U128 value, uint64_t divisor, uint64_t& quotient)
{
    if (value.hi >= divisor) return false;
    uint64_t remainder = value.hi;
    uint64_t q = 0;
    for (int bit = 63; bit >= 0; --bit) {
        bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((value.lo >> bit) & 1u);
        if (carry || remainder >= divisor) {
            remainder -= divisor;
            q |= static_cast<uint64_t>(1) << bit;
        }
    }
    quotient = q;
    return true;
}

uint64_t magnitude(int64_t value)
{
    return value < 0 ? static_cast<uint64_t>(0) - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}

// (long)(((decimal)a * b) / divisor) for non-negative b and divisor > 0.
// The product must fit .NET decimal's 96-bit mantissa (else OverflowException).
// The quotient is truncated toward zero; v1's decimal quotient keeps up to
// 28 significant digits, so this is exact whenever the division is exact at
// scale <= 28 (always for the microseconds-to-ticks direction; see
// ENGINE_PORT_NOTES.md for the ticks-to-microseconds limit).
int64_t decimal_multiply_divide(int64_t a, int64_t b, int64_t divisor)
{
    U128 product = multiply(magnitude(a), static_cast<uint64_t>(b));
    if ((product.hi >> 32) != 0) models::throw_overflow();
    uint64_t q = 0;
    if (!divide(product, static_cast<uint64_t>(divisor), q)) models::throw_overflow();
    if (a < 0) {
        if (q > static_cast<uint64_t>(models::kInt64Max) + 1u) models::throw_overflow();
        return static_cast<int64_t>(static_cast<uint64_t>(0) - q);
    }
    if (q > static_cast<uint64_t>(models::kInt64Max)) models::throw_overflow();
    return static_cast<int64_t>(q);
}

} // namespace

int64_t TimeConversion::microseconds_to_ticks(int64_t microseconds) const
{
    const int64_t scale = 1000000;
    if (frequency_ <= models::kInt64Max / scale)
        return models::checked_add(models::checked_mul(microseconds / scale, frequency_),
            models::checked_mul(microseconds % scale, frequency_) / scale);
    return decimal_multiply_divide(microseconds, frequency_, scale);
}

int64_t TimeConversion::ticks_to_microseconds(int64_t ticks) const
{
    const int64_t scale = 1000000;
    if (frequency_ <= models::kInt64Max / scale)
        return models::checked_add(models::checked_mul(ticks / frequency_, scale),
            models::checked_mul(ticks % frequency_, scale) / frequency_);
    return decimal_multiply_divide(ticks, scale, frequency_);
}

int64_t dispatch_due_clock_ticks(const models::OrderedPlaybackGenerationQueue& generations,
    const models::OutputWorkItem& item, const TimeConversion& time)
{
    models::PlaybackGeneration* generation = generations.find_generation(item.generation_id);
    if (generation == nullptr) models::throw_invalid_operation("The work item's generation has retired.");
    int64_t deltaMicroseconds = models::max64(0,
        item.logical_output_microseconds - generation->target_source_microseconds);
    int64_t deltaTicks = time.microseconds_to_ticks(deltaMicroseconds);
    return generation->source_clock_anchor_ticks > models::kInt64Max - deltaTicks
        ? models::kInt64Max : generation->source_clock_anchor_ticks + deltaTicks;
}

} // namespace engine
} // namespace midibn
