#include "midibn/models/model_base.h"

namespace midibn {
namespace models {

void throw_argument_out_of_range(const char* param)
{
    throw ModelException{ "ArgumentOutOfRangeException", param, "Specified argument was out of the range of valid values." };
}

void throw_argument(const char* param, const char* message)
{
    throw ModelException{ "ArgumentException", param ? param : "", message };
}

void throw_argument_null(const char* param)
{
    throw ModelException{ "ArgumentNullException", param, "Value cannot be null." };
}

void throw_invalid_operation(const char* message)
{
    throw ModelException{ "InvalidOperationException", "", message };
}

void throw_not_supported(const char* message)
{
    throw ModelException{ "NotSupportedException", "", message };
}

void throw_overflow()
{
    throw ModelException{ "OverflowException", "", "Arithmetic operation resulted in an overflow." };
}

void throw_index_out_of_range()
{
    throw ModelException{ "IndexOutOfRangeException", "", "Index was outside the bounds of the array." };
}

void throw_duplicate_key()
{
    throw ModelException{ "ArgumentException", "", "An item with the same key has already been added." };
}

int64_t checked_mul_slow(int64_t a, int64_t b)
{
    if (a == 0 || b == 0) return 0;
    if (a > 0) {
        if (b > 0) { if (a > kInt64Max / b) throw_overflow(); }
        else if (b < kInt64Min / a) throw_overflow();
    }
    else {
        if (b > 0) { if (a < kInt64Min / b) throw_overflow(); }
        else if (b < kInt64Max / a) throw_overflow();
    }
    return a * b;
}

EventView EventView::with_output_channel(int newChannel) const
{
    if (channel() < 0) return *this;
    if (newChannel < 0 || newChannel > 15) throw_argument_out_of_range("channel");
    EventView result = *this;
    result.routed_channel_plus_one = static_cast<uint8_t>(newChannel + 1);
    return result;
}

EventView EventView::with_logical_event_index(int32_t index) const
{
    EventView result = *this;
    result.logical_index = index;
    result.has_logical_index = true;
    return result;
}

EventView EventView::create(int64_t tick, int64_t micros, int32_t track, EventKind kind, int channel, uint8_t status,
    int32_t eventIndex, uint8_t data1, uint8_t data2, int length)
{
    if (length < 0 || length > 3) throw_argument_out_of_range("length");
    EventView v;
    v.valid = true;
    v.tick = tick;
    v.micros = micros;
    v.track = track;
    v.kind = kind;
    v.source_channel = channel;
    v.source_status = status;
    v.source_index = eventIndex;
    v.data_length = length;
    v.bytes[0] = status;
    v.bytes[1] = data1;
    v.bytes[2] = data2;
    return v;
}

int32_t EventSource::lower_bound_by_time(int64_t micros) const
{
    int32_t low = 0;
    int32_t high = count();
    while (low < high) {
        int32_t middle = low + ((high - low) >> 1);
        int64_t value = store ? store->records[middle].micros : views->at(middle).micros;
        if (value < micros) low = middle + 1;
        else high = middle;
    }
    return low;
}

EventView EventViewList::at(int32_t index) const
{
    EventView v = events[static_cast<size_t>(index)];
    v.source_index = index;
    v.has_logical_index = false;
    v.logical_index = 0;
    v.routed_channel_plus_one = 0;
    return v;
}

} // namespace models
} // namespace midibn
