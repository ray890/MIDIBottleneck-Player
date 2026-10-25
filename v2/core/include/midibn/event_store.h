// Compact immutable MIDI event storage. v1's store used 40-byte records
// (CompactMidiEventRecord); v2 stores 24 bytes per event (improvement DD-003,
// v2/docs/DIFFERENCES_FROM_V1.md) in 2,048-record segments and keeps long
// SysEx payloads in 1 MiB byte segments. No per-event objects.
//
// What a stored event is, in 24 bytes:
//   tick        8   absolute tick
//   micros      8   intended time in microseconds (provisional records: unused)
//   packed      4   the message bytes (status, data 1, data 2) of every message of
//                   up to 3 bytes, SysEx included; for a long SysEx (4 bytes or more)
//                   the number of its entry in the long-payload table
//   track       2   SMF track numbers are 16 bits
//   metadata    2   kind, channel, SysEx terminator, long flag, inline length
// The length, offset and status that v1 kept in separate fields are derived:
// short messages carry their length in the metadata and their status in the
// first message byte; a long SysEx finds its payload through the table, which
// holds the end offset of each long payload (its start is the previous end).
//
// Callers read events through EventStore::records[i], which returns the wide
// decoded EventRecord by value (the shape of v1's record), so code written
// against the 40-byte layout keeps working unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace midibn {

enum class EventKind : uint8_t {
    NoteOff = 0,
    NoteOn,
    PolyphonicAftertouch,
    ControlChange,
    ProgramChange,
    ChannelAftertouch,
    PitchBend,
    SystemExclusive,
    SystemMessage
};

const char* event_kind_name(EventKind kind);

// An event as callers see it (the v1 record shape), decoded from the 24-byte
// stored form on every read.
struct EventRecord {
    int64_t tick = 0;
    int64_t micros = 0;
    int64_t payload_offset = -1;  // offset in the long-payload store; -1 for inline messages
    int32_t track = 0;
    int32_t payload_length = 0;   // stored message length in bytes
    uint32_t packed = 0;          // inline bytes, little-endian; 0 for long payloads
    uint16_t metadata = 0;        // kind (low 4 bits) | (channel + 1) << 4
    uint8_t status = 0;
    uint8_t reserved = 0;

    EventKind kind() const { return static_cast<EventKind>(metadata & 0x0F); }
    int channel() const { return ((metadata >> 4) & 0x1F) - 1; }
};

// The stored form.
struct PackedRecord {
    int64_t tick;
    int64_t micros;
    uint32_t packed;
    uint16_t track;
    uint16_t metadata;

    // metadata layout: bits 0-3 kind, 4-8 channel + 1, 9 SysEx F7 (else F0),
    // 10 long payload (packed = table index), 11-12 inline message length.
    static constexpr uint16_t kKindMask = 0x000F;
    static constexpr int kChannelShift = 4;
    static constexpr uint16_t kChannelMask = 0x01F0;
    static constexpr uint16_t kSysExEscape = 0x0200;
    static constexpr uint16_t kLongPayload = 0x0400;
    static constexpr int kLengthShift = 11;

    EventKind kind() const { return static_cast<EventKind>(metadata & kKindMask); }
    int channel() const { return ((metadata >> kChannelShift) & 0x1F) - 1; }
    bool is_long() const { return (metadata & kLongPayload) != 0; }
    int inline_length() const { return (metadata >> kLengthShift) & 3; }
    uint8_t status() const
    {
        return kind() == EventKind::SystemExclusive ? static_cast<uint8_t>((metadata & kSysExEscape) ? 0xF7 : 0xF0)
                                                    : static_cast<uint8_t>(packed & 0xFF);
    }
    static uint16_t make_metadata(EventKind kind, int channel, int inline_length, bool long_payload, bool sysex_escape)
    {
        return static_cast<uint16_t>((static_cast<int>(kind) & kKindMask) | ((channel + 1) << kChannelShift) |
            (sysex_escape ? kSysExEscape : 0) | (long_payload ? kLongPayload : 0) | (inline_length << kLengthShift));
    }
};
static_assert(sizeof(PackedRecord) == 24, "PackedRecord must stay 24 bytes");

// Fixed-size segments of stored records; a segment is released once its last
// record has been consumed (the loader's per-track provisional lists).
class RecordSegments {
public:
    static constexpr int kSegmentShift = 11;
    static constexpr int kSegmentCapacity = 1 << kSegmentShift;  // 2,048

    void append(const PackedRecord& record)
    {
        const int offset = static_cast<int>(count_ & (kSegmentCapacity - 1));
        if (offset == 0) segments_.emplace_back(new PackedRecord[kSegmentCapacity]);
        segments_.back()[offset] = record;
        ++count_;
    }
    int64_t count() const { return count_; }
    const PackedRecord& operator[](int64_t index) const
    { return segments_[static_cast<size_t>(index >> kSegmentShift)][index & (kSegmentCapacity - 1)]; }
    // Frees a segment once its last record has been consumed.
    void release_consumed_through(int64_t index)
    {
        if ((index & (kSegmentCapacity - 1)) == kSegmentCapacity - 1 || index == count_ - 1) release_segment_of(index);
    }
    void trim_last_segment();
    size_t segment_count() const { return segments_.size(); }

private:
    void release_segment_of(int64_t index);
    std::vector<std::unique_ptr<PackedRecord[]>> segments_;
    int64_t count_ = 0;
};

class PayloadSegments {
public:
    static constexpr int64_t kSegmentSize = 1024 * 1024;

    void append(uint8_t value);
    void append(const uint8_t* data, size_t count);
    int64_t length() const { return length_; }
    uint8_t at(int64_t offset) const
    { return segments_[static_cast<size_t>(offset / kSegmentSize)][offset % kSegmentSize]; }
    void copy_to(int64_t offset, uint8_t* destination, size_t count) const;
    void release_before(int64_t offset_exclusive);
    void trim_last_segment();

private:
    std::vector<std::unique_ptr<uint8_t[]>> segments_;
    int64_t length_ = 0;
    int64_t last_size_ = kSegmentSize;
};

// The stored events plus the long-payload table.
class EventRecords {
public:
    int64_t count() const { return segments_.count(); }
    // Decoded copy of event `index` (the v1 record shape).
    EventRecord operator[](int64_t index) const { return decode(segments_[index]); }
    // The stored form, for code that wants the cheapest read.
    const PackedRecord& packed_at(int64_t index) const { return segments_[index]; }
    int64_t micros_at(int64_t index) const { return segments_[index].micros; }
    // Start and length of the payload of a long event (its table entry).
    void long_extent(const PackedRecord& record, int64_t& offset, int32_t& length) const
    {
        const size_t k = record.packed;
        offset = k == 0 ? 0 : long_ends_[k - 1];
        length = static_cast<int32_t>(long_ends_[k] - offset);
    }

    // Appends an inline event (its message bytes are in `record.packed`).
    void append(const PackedRecord& record) { segments_.append(record); }
    // Appends a long event whose payload ends at `payload_end` in the payload store.
    void append_long(PackedRecord record, int64_t payload_end)
    {
        record.packed = static_cast<uint32_t>(long_ends_.size());
        long_ends_.push_back(payload_end);
        segments_.append(record);
    }
    int64_t long_event_count() const { return static_cast<int64_t>(long_ends_.size()); }
    void release_consumed_through(int64_t index) { segments_.release_consumed_through(index); }
    void trim();
    size_t segment_count() const { return segments_.segment_count(); }

private:
    EventRecord decode(const PackedRecord& stored) const;

    RecordSegments segments_;
    std::vector<int64_t> long_ends_;
};

// The final immutable song store.
struct EventStore {
    EventRecords records;
    PayloadSegments payloads;

    int64_t count() const { return records.count(); }
    // Returns the stored byte at `index` of event `event` (v1 GetDataByte).
    uint8_t data_byte(int64_t event, int index) const
    {
        const PackedRecord& r = records.packed_at(event);
        if (!r.is_long()) return static_cast<uint8_t>(r.packed >> (index * 8));
        int64_t offset;
        int32_t length;
        records.long_extent(r, offset, length);
        return payloads.at(offset + index);
    }
};

inline EventRecord EventRecords::decode(const PackedRecord& stored) const
{
    EventRecord r;
    r.tick = stored.tick;
    r.micros = stored.micros;
    r.track = stored.track;
    r.metadata = static_cast<uint16_t>(stored.metadata & (PackedRecord::kKindMask | PackedRecord::kChannelMask));
    r.status = stored.status();
    if (stored.is_long()) {
        int64_t offset;
        int32_t length;
        long_extent(stored, offset, length);
        r.payload_offset = offset;
        r.payload_length = length;
        r.packed = 0;
    }
    else {
        r.payload_offset = -1;
        r.payload_length = stored.inline_length();
        r.packed = stored.packed;
    }
    return r;
}

} // namespace midibn
