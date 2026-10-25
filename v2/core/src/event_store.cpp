#include "midibn/event_store.h"

#include <algorithm>
#include <cstring>

namespace midibn {

const char* event_kind_name(EventKind kind)
{
    switch (kind) {
    case EventKind::NoteOff: return "NoteOff";
    case EventKind::NoteOn: return "NoteOn";
    case EventKind::PolyphonicAftertouch: return "PolyphonicAftertouch";
    case EventKind::ControlChange: return "ControlChange";
    case EventKind::ProgramChange: return "ProgramChange";
    case EventKind::ChannelAftertouch: return "ChannelAftertouch";
    case EventKind::PitchBend: return "PitchBend";
    case EventKind::SystemExclusive: return "SystemExclusive";
    case EventKind::SystemMessage: return "SystemMessage";
    }
    return "Unknown";
}

void RecordSegments::release_segment_of(int64_t index)
{
    if (index < 0 || index >= count_) return;
    segments_[static_cast<size_t>(index >> kSegmentShift)].reset();
}

void RecordSegments::trim_last_segment()
{
    int used = static_cast<int>(count_ & (kSegmentCapacity - 1));
    if (segments_.empty() || used == 0) return;
    std::unique_ptr<PackedRecord[]> trimmed(new PackedRecord[used]);
    std::memcpy(trimmed.get(), segments_.back().get(), sizeof(PackedRecord) * used);
    segments_.back() = std::move(trimmed);
}

void EventRecords::trim()
{
    segments_.trim_last_segment();
    long_ends_.shrink_to_fit();
}

void PayloadSegments::append(uint8_t value)
{
    int64_t offset = length_ % kSegmentSize;
    if (offset == 0) segments_.emplace_back(new uint8_t[static_cast<size_t>(kSegmentSize)]);
    segments_.back()[static_cast<size_t>(offset)] = value;
    ++length_;
}

void PayloadSegments::append(const uint8_t* data, size_t count)
{
    while (count > 0) {
        int64_t offset = length_ % kSegmentSize;
        if (offset == 0) segments_.emplace_back(new uint8_t[static_cast<size_t>(kSegmentSize)]);
        size_t take = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(count), kSegmentSize - offset));
        std::memcpy(segments_.back().get() + offset, data, take);
        data += take;
        count -= take;
        length_ += static_cast<int64_t>(take);
    }
}

void PayloadSegments::copy_to(int64_t offset, uint8_t* destination, size_t count) const
{
    while (count > 0) {
        size_t segment = static_cast<size_t>(offset / kSegmentSize);
        int64_t inner = offset % kSegmentSize;
        size_t take = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(count), kSegmentSize - inner));
        std::memcpy(destination, segments_[segment].get() + inner, take);
        destination += take;
        offset += static_cast<int64_t>(take);
        count -= take;
    }
}

void PayloadSegments::release_before(int64_t offset_exclusive)
{
    if (offset_exclusive <= 0) return;
    size_t complete = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(segments_.size()),
        offset_exclusive / kSegmentSize));
    for (size_t i = 0; i < complete; ++i) segments_[i].reset();
}

void PayloadSegments::trim_last_segment()
{
    int64_t used = length_ % kSegmentSize;
    if (segments_.empty() || used == 0) return;
    std::unique_ptr<uint8_t[]> trimmed(new uint8_t[static_cast<size_t>(used)]);
    std::memcpy(trimmed.get(), segments_.back().get(), static_cast<size_t>(used));
    segments_.back() = std::move(trimmed);
    last_size_ = used;
}

} // namespace midibn
