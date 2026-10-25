// Port of v1.0.44 MidiFileContent.cs, DirectCompactMidiParser.cs,
// MidiLargeFilePreflight.cs, and MidiTrackMetadata.DecodeDisplayText.
// Results and messages are v1's except where the improvement register says otherwise
// (v2/docs/DIFFERENCES_FROM_V1.md: DD-001, DD-003, DD-012, DD-020 to DD-024); v2/harness measures the difference.
#include "midibn/midi_loader.h"
#include "midibn/tempo_clock.h"
#include "midibn/user_messages.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace midibn {
namespace {

using platform::ByteSource;

// ------------------------------------------------------------ failures

struct LoadFailure {
    std::string type;
    std::string message;
};

[[noreturn]] void fail(const char* type, const std::string& message) { throw LoadFailure{ type, message }; }
[[noreturn]] void invalid(const std::string& message) { fail("InvalidDataException", message); }
[[noreturn]] void end_of_stream(const std::string& message) { fail("EndOfStreamException", message); }

const char* const kReadBeyondEnd = messages::header_ends_early();   // v1: .NET's "Unable to read beyond the end of the stream." (DD-012)
const char kTrackEnded[] = "The MIDI track ended unexpectedly.";
const char kEventLimit[] = "The MIDI exceeds the supported 2,147,483,647-event indexed-store limit.";
const int64_t kProgressScale = 10000;

// ------------------------------------------------------------ progress

// Progress reports and cancellation checkpoints, placed exactly where v1
// reports and calls ThrowIfCancellationRequested.
class Reporter {
public:
    explicit Reporter(LoadMonitor* monitor) : monitor_(monitor) {}
    bool active() const { return monitor_ != nullptr; }
    void check()
    {
        if (monitor_ && monitor_->cancellation_requested()) fail(kCancellationType, kCancellationMessage);
    }
    void report(const std::string& stage, int completedTracks, int totalTracks, int64_t overallCompleted,
        int64_t overallTotal, int64_t stageCompleted, int64_t stageTotal)
    {
        if (monitor_)
            monitor_->progress(LoadProgress(stage, completedTracks, totalTracks, overallCompleted, overallTotal,
                stageCompleted, stageTotal));
    }
    // v1 DirectCompactMidiParser.Report: overall progress out of 10,000.
    void report(const std::string& stage, int completedTracks, int totalTracks, int64_t overallCompleted,
        int64_t stageCompleted, int64_t stageTotal)
    {
        report(stage, completedTracks, totalTracks, overallCompleted, kProgressScale, stageCompleted, stageTotal);
    }

private:
    LoadMonitor* monitor_;
};

// floor(completed * scale / total) for 0 < completed < total: v1 computes it
// in decimal and truncates, which is exact for these magnitudes.
int64_t scale_progress(int64_t completed, int64_t total, int64_t scale)
{
    if (completed <= 0 || total <= 0) return 0;
    if (completed >= total) return scale;
    if (completed <= INT64_MAX / scale) return completed * scale / total;
    // 128-bit product, then binary long division (quotient < scale).
    uint64_t a = static_cast<uint64_t>(completed), b = static_cast<uint64_t>(scale);
    uint64_t lo = (a & 0xFFFFFFFFu) * b;
    uint64_t hi = (a >> 32) * b + (lo >> 32);
    lo = (lo & 0xFFFFFFFFu) | (hi << 32);
    hi >>= 32;
    uint64_t divisor = static_cast<uint64_t>(total), remainder = 0, quotient = 0;
    for (int bit = 127; bit >= 0; --bit) {
        uint64_t next = bit >= 64 ? (hi >> (bit - 64)) & 1u : (lo >> bit) & 1u;
        bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | next;
        quotient <<= 1;
        if (carry || remainder >= divisor) { remainder -= divisor; quotient |= 1u; }
    }
    return static_cast<int64_t>(quotient);
}

std::string parsing_stage(int trackIndex, int trackCount)
{
    return "Parsing track " + std::to_string(trackIndex + 1) + " of " + std::to_string(trackCount);
}

const char kInspectingStage[] = "Inspecting large MIDI";

std::string hex2(int value)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string s;
    s.push_back(kHex[(value >> 4) & 0xF]);
    s.push_back(kHex[value & 0xF]);
    return s;
}

// ------------------------------------------------------------ container

struct Container {
    int64_t start;
    int64_t length;
    bool rmid;
};

bool matches(const uint8_t* bytes, const char* text)
{
    return bytes[0] == static_cast<uint8_t>(text[0]) && bytes[1] == static_cast<uint8_t>(text[1]) &&
        bytes[2] == static_cast<uint8_t>(text[2]) && bytes[3] == static_cast<uint8_t>(text[3]);
}

void read_exactly(ByteSource& source, int64_t offset, uint8_t* buffer, size_t count, const char* error)
{
    size_t done = 0;
    while (done < count) {
        size_t read = source.read_at(offset + static_cast<int64_t>(done), buffer + done, count - done);
        if (read == 0) invalid(error);
        done += read;
    }
}

uint32_t le32(const uint8_t* b)
{
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

Container open_container(ByteSource& source)
{
    const int64_t length = source.size();
    if (length < 4) return Container{ 0, length, false };
    uint8_t id[4];
    read_exactly(source, 0, id, 4, "The MIDI file header ended unexpectedly.");
    if (!matches(id, "RIFF")) return Container{ 0, length, false };

    if (length < 12) invalid("The RIFF/RMID header is truncated.");
    uint8_t word[4];
    read_exactly(source, 4, word, 4, "The RIFF/RMID chunk length is truncated.");
    uint32_t riffSize = le32(word);
    uint8_t type[4];
    read_exactly(source, 8, type, 4, "The RIFF/RMID type is truncated.");
    if (!matches(type, "RMID")) invalid("The RIFF file is not an RMID MIDI container.");
    if (riffSize < 4) invalid("The RIFF/RMID size is shorter than its form type.");
    int64_t riffEnd = 8 + static_cast<int64_t>(riffSize);
    if (riffEnd > length) invalid("The RIFF/RMID container extends beyond the source file.");

    int64_t position = 12;
    while (position < riffEnd) {
        if (riffEnd - position < 8) invalid("The RIFF/RMID chunk header is truncated.");
        uint8_t chunkId[4];
        read_exactly(source, position, chunkId, 4, "The RIFF/RMID chunk header is truncated.");
        read_exactly(source, position + 4, word, 4, "The RIFF/RMID chunk length is truncated.");
        uint32_t chunkLength = le32(word);
        int64_t dataStart = position + 8;
        int64_t dataEnd = dataStart + chunkLength;
        int64_t paddedEnd = dataEnd + (chunkLength & 1u);
        if (paddedEnd > riffEnd) invalid("A RIFF/RMID chunk extends beyond the declared container.");
        if (matches(chunkId, "data")) {
            if (chunkLength == 0) invalid("The RIFF/RMID data chunk is empty.");
            return Container{ dataStart, chunkLength, true };
        }
        position = paddedEnd;
    }
    invalid("The RIFF/RMID container has no MIDI data chunk.");
}

// Sequential view of the SMF payload (v1 BoundedReadStream).
class PayloadStream {
public:
    PayloadStream(ByteSource& source, const Container& c) : source_(source), start_(c.start), length_(c.length) {}
    int64_t length() const { return length_; }
    int64_t position() const { return position_; }
    size_t read(uint8_t* buffer, size_t count)
    {
        int64_t remaining = length_ - position_;
        if (remaining <= 0) return 0;
        size_t requested = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(count), remaining));
        size_t read = source_.read_at(start_ + position_, buffer, requested);
        position_ += static_cast<int64_t>(read);
        return read;
    }

private:
    ByteSource& source_;
    int64_t start_;
    int64_t length_;
    int64_t position_ = 0;
};

// v1 header access through System.IO.BinaryReader.
void require_chunk(PayloadStream& stream, const char* expected)
{
    uint8_t id[4];
    size_t got = 0;
    while (got < 4) {
        size_t n = stream.read(id + got, 4 - got);
        if (n == 0) break;
        got += n;
    }
    if (got != 4 || !matches(id, expected)) invalid(std::string("Expected MIDI chunk ") + expected + ".");
}

uint8_t header_byte(PayloadStream& stream)
{
    uint8_t value;
    if (stream.read(&value, 1) != 1) end_of_stream(kReadBeyondEnd);
    return value;
}

int read_u16be(PayloadStream& stream)
{
    int a = header_byte(stream);
    int b = header_byte(stream);
    return (a << 8) | b;
}

uint32_t read_u32be(PayloadStream& stream)
{
    uint32_t a = header_byte(stream), b = header_byte(stream), c = header_byte(stream), d = header_byte(stream);
    return (a << 24) | (b << 16) | (c << 8) | d;
}

void skip_stream(PayloadStream& stream, uint32_t length, Reporter& reporter)
{
    uint8_t buffer[4096];
    int64_t remaining = length;
    while (remaining > 0) {
        reporter.check();
        size_t read = stream.read(buffer, static_cast<size_t>(std::min<int64_t>(sizeof(buffer), remaining)));
        if (read == 0) end_of_stream("The MIDI header ended unexpectedly.");
        remaining -= static_cast<int64_t>(read);
    }
}

struct Header {
    int format;
    int track_count;
    int division;
};

Header read_header(PayloadStream& stream, Reporter& reporter)
{
    require_chunk(stream, "MThd");
    uint32_t headerLength = read_u32be(stream);
    if (headerLength < 6) invalid("The MIDI header is shorter than 6 bytes.");
    Header h;
    h.format = read_u16be(stream);
    h.track_count = read_u16be(stream);
    h.division = read_u16be(stream);
    if (h.format < 0 || h.format > 2)
        invalid("Unsupported Standard MIDI File format " + std::to_string(h.format) + ".");
    if (h.format == 2)
        invalid("Format 2 MIDI files contain independent sequences and are not supported in this version.");
    if ((h.division & 0x8000) != 0)
        invalid("SMPTE time division is not supported; this version requires PPQN timing.");
    if (h.division == 0) invalid("The MIDI file declares zero ticks per quarter note.");
    if (h.track_count <= 0) invalid("The MIDI file contains no tracks.");
    if (headerLength > 6) skip_stream(stream, headerLength - 6, reporter);
    return h;
}

// ------------------------------------------------------------ track reader

// v1 TrackReader.Fill checks cancellation before each buffer refill and
// reports progress after it, with the bytes consumed so far.
class FillHook {
public:
    virtual void before_fill() = 0;
    virtual void after_fill(int64_t consumed) = 0;

protected:
    ~FillHook() {}
};

class TrackReader {
public:
    TrackReader(PayloadStream& stream, uint32_t length, uint8_t* buffer, size_t capacity, FillHook& hook)
        : stream_(stream), buffer_(buffer), capacity_(capacity), remaining_(length), hook_(hook) {}

    bool at_end() const { return position_ == count_ && remaining_ == 0; }
    int64_t consumed() const { return consumed_; }

    uint8_t read_byte()
    {
        if (position_ == count_) fill();
        ++consumed_;
        return buffer_[position_++];
    }

    uint32_t read_vlq()
    {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t current = read_byte();
            value = (value << 7) | (current & 0x7Fu);
            if ((current & 0x80) == 0) return value;
        }
        invalid("A MIDI variable-length quantity exceeds four bytes.");
    }

    void skip(uint32_t length)
    {
        int64_t remaining = length;
        while (remaining > 0) {
            if (position_ == count_) fill();
            size_t take = static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(count_ - position_)));
            position_ += take;
            consumed_ += static_cast<int64_t>(take);
            remaining -= static_cast<int64_t>(take);
        }
    }

    void copy_payload(PayloadSegments& destination, uint32_t length)
    {
        int64_t remaining = length;
        while (remaining > 0) {
            if (position_ == count_) fill();
            size_t take = static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(count_ - position_)));
            destination.append(buffer_ + position_, take);
            position_ += take;
            consumed_ += static_cast<int64_t>(take);
            remaining -= static_cast<int64_t>(take);
        }
    }

private:
    void fill()
    {
        hook_.before_fill();
        if (remaining_ <= 0) end_of_stream(kTrackEnded);
        size_t requested = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(capacity_), remaining_));
        count_ = stream_.read(buffer_, requested);
        position_ = 0;
        if (count_ == 0) end_of_stream(kTrackEnded);
        remaining_ -= static_cast<int64_t>(count_);
        hook_.after_fill(consumed_);
    }

    PayloadStream& stream_;
    uint8_t* buffer_;
    size_t capacity_;
    int64_t remaining_;
    FillHook& hook_;
    size_t position_ = 0;
    size_t count_ = 0;
    int64_t consumed_ = 0;
};

// Load-time hook: v1 DirectCompactMidiParser.Load's TrackReader delegate.
class ParseFillHook : public FillHook {
public:
    ParseFillHook(Reporter& reporter, int trackIndex, int trackCount, int64_t priorBytes, int64_t fileLength,
        uint32_t length)
        : reporter_(reporter), track_index_(trackIndex), track_count_(trackCount), prior_bytes_(priorBytes),
          file_length_(fileLength), length_(length) {}
    void before_fill() override { reporter_.check(); }
    void after_fill(int64_t consumed) override
    {
        if (!reporter_.active()) return;
        reporter_.report(parsing_stage(track_index_, track_count_), track_index_, track_count_,
            scale_progress(prior_bytes_ + consumed, file_length_, 6000), consumed, std::max<int64_t>(1, length_));
    }

private:
    Reporter& reporter_;
    int track_index_;
    int track_count_;
    int64_t prior_bytes_;
    int64_t file_length_;
    uint32_t length_;
};

// Preflight hook: v1 MidiLargeFilePreflight.Scan's TrackReader delegate.
class ScanFillHook : public FillHook {
public:
    ScanFillHook(Reporter& reporter, int trackIndex, int trackCount, int64_t trackStart, int64_t fileLength,
        uint32_t length)
        : reporter_(reporter), track_index_(trackIndex), track_count_(trackCount), track_start_(trackStart),
          file_length_(fileLength), length_(length) {}
    void before_fill() override { reporter_.check(); }
    void after_fill(int64_t consumed) override
    {
        reporter_.report(kInspectingStage, track_index_, track_count_, track_start_ + consumed, file_length_,
            consumed, std::max<int64_t>(1, length_));
    }

private:
    Reporter& reporter_;
    int track_index_;
    int track_count_;
    int64_t track_start_;
    int64_t file_length_;
    uint32_t length_;
};

const size_t kTrackBufferSize = 65536;

// ------------------------------------------------------------ status helpers

EventKind kind_from_status(uint8_t status)
{
    switch (status & 0xF0) {
    case 0x80: return EventKind::NoteOff;
    case 0x90: return EventKind::NoteOn;
    case 0xA0: return EventKind::PolyphonicAftertouch;
    case 0xB0: return EventKind::ControlChange;
    case 0xC0: return EventKind::ProgramChange;
    case 0xD0: return EventKind::ChannelAftertouch;
    case 0xE0: return EventKind::PitchBend;
    default: invalid("Unknown MIDI channel status 0x" + hex2(status) + ".");
    }
}

int channel_data_length(uint8_t status)
{
    int high = status & 0xF0;
    return high == 0xC0 || high == 0xD0 ? 1 : 2;
}

int system_data_length(uint8_t status)
{
    switch (status) {
    case 0xF1: return 1;
    case 0xF2: return 2;
    case 0xF3: return 1;
    case 0xF6: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: return 0;
    default: invalid("Unsupported system status 0x" + hex2(status) + ".");
    }
}

bool can_repair_after_meta(uint8_t lastChannelStatus, bool immediatelyAfterMeta, uint8_t firstData)
{
    return immediatelyAfterMeta && firstData < 0x80 && lastChannelStatus >= 0x80 && lastChannelStatus < 0xF0;
}

// ------------------------------------------------------------ display text

// .NET Framework 4.8 Char.IsControl / Char.IsWhiteSpace for UTF-16 code units.
bool is_control(char16_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }
bool is_white_space(char16_t c)
{
    if (c == 0x20 || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0) return true;
    if (c < 0x1680) return false;
    return c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
        c == 0x202F || c == 0x205F || c == 0x3000;
}

// Strict UTF-8 (UTF8Encoding(false, true)): false on any invalid sequence.
bool decode_utf8_strict(const uint8_t* b, size_t n, std::u16string& out)
{
    size_t i = 0;
    while (i < n) {
        uint32_t c = b[i];
        size_t extra;
        uint32_t lowLimit = 0x80, highLimit = 0xBF;
        if (c < 0x80) { out.push_back(static_cast<char16_t>(c)); ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; c &= 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2; if (c == 0xE0) lowLimit = 0xA0; if (c == 0xED) highLimit = 0x9F; c &= 0x0F;
        }
        else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3; if (c == 0xF0) lowLimit = 0x90; if (c == 0xF4) highLimit = 0x8F; c &= 0x07;
        }
        else return false;
        if (i + extra >= n) return false;  // truncated sequence
        for (size_t k = 1; k <= extra; ++k) {
            uint8_t t = b[i + k];
            uint32_t lo = k == 1 ? lowLimit : 0x80;
            uint32_t hi = k == 1 ? highLimit : 0xBF;
            if (t < lo || t > hi) return false;
            c = (c << 6) | (t & 0x3Fu);
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
        }
        else out.push_back(static_cast<char16_t>(c));
        i += extra + 1;
    }
    return true;
}

const int kMaximumStoredBytes = 512;
const size_t kMaximumDisplayCharacters = 128;

std::u16string decode_display_text(const uint8_t* bytes, size_t count, bool& truncated)
{
    truncated = false;
    std::u16string decoded;
    if (!decode_utf8_strict(bytes, count, decoded)) {
        decoded.clear();
        for (size_t i = 0; i < count; ++i) decoded.push_back(static_cast<char16_t>(bytes[i]));
    }
    std::u16string clean;
    bool pendingSpace = false;
    for (char16_t value : decoded) {
        if (is_control(value) || is_white_space(value)) {
            if (!clean.empty()) pendingSpace = true;
            continue;
        }
        if (pendingSpace && clean.size() < kMaximumDisplayCharacters) clean.push_back(u' ');
        pendingSpace = false;
        if (clean.size() == kMaximumDisplayCharacters) {
            truncated = true;
            // DD-044: a character made of two UTF-16 units is cut whole, never in the middle (v1 could end the
            // name with half a character, which shows as a broken glyph).
            if (value >= 0xDC00 && value <= 0xDFFF && clean.back() >= 0xD800 && clean.back() <= 0xDBFF) clean.pop_back();
            break;
        }
        clean.push_back(value);
    }
    return clean;
}

std::u16string read_display_text(TrackReader& reader, uint32_t length, bool& truncated)
{
    uint32_t retained = std::min<uint32_t>(kMaximumStoredBytes, length);
    uint8_t bytes[kMaximumStoredBytes];
    for (uint32_t i = 0; i < retained; ++i) bytes[i] = reader.read_byte();
    if (length > retained) reader.skip(length - retained);
    bool decodedTruncated;
    std::u16string result = decode_display_text(bytes, retained, decodedTruncated);
    truncated = decodedTruncated || length > retained;
    return result;
}

// ------------------------------------------------------------ track parse

// One track's events before the merge: stored records (micros unused until the merge),
// with the payloads of its long SysEx messages (4 bytes or more) in a per-track store.
struct ParsedTrack {
    EventRecords events;
    PayloadSegments payloads;
    int64_t end_tick = 0;
    TrackMetadata metadata;
    int64_t running_status_repairs = 0;
};

struct TrackProgress {
    Reporter& reporter;
    int track_count;
    int64_t prior_bytes;
    int64_t file_length;
    uint32_t declared_length;
};

void parse_track(TrackReader& reader, int trackIndex, ParsedTrack& out, std::vector<TempoRecord>& tempos,
    const TrackProgress& progress)
{
    int64_t tick = 0;
    uint8_t runningStatus = 0;
    uint8_t lastChannelStatus = 0;
    bool immediatelyAfterMeta = false;
    int32_t order = 0;
    bool haveName = false, haveInstrument = false;
    const std::string trackText = std::to_string(trackIndex);
    Reporter& reporter = progress.reporter;
    const int64_t stageTotal = std::max<int64_t>(1, progress.declared_length);

    while (!reader.at_end()) {
        if ((order & 4095) == 0) {
            reporter.check();
            if (reporter.active())
                reporter.report(parsing_stage(trackIndex, progress.track_count), trackIndex, progress.track_count,
                    scale_progress(progress.prior_bytes + reader.consumed(), progress.file_length, 6000),
                    reader.consumed(), stageTotal);
        }
        uint32_t delta = reader.read_vlq();
        if (tick > INT64_MAX - static_cast<int64_t>(delta))
            fail("OverflowException", messages::event_time_too_large());
        tick += delta;
        if (reader.at_end()) invalid("Track " + trackText + " ends after a delta time.");
        uint8_t first = reader.read_byte();
        uint8_t status;
        int firstData = -1;
        bool repaired = false;
        if (first < 0x80) {
            if (runningStatus < 0x80 || runningStatus >= 0xF0) {
                if (!can_repair_after_meta(lastChannelStatus, immediatelyAfterMeta, first))
                    invalid("Invalid running status in track " + trackText + ".");
                status = lastChannelStatus;
                runningStatus = status;
                repaired = true;
            }
            else status = runningStatus;
            firstData = first;
        }
        else {
            status = first;
            if (status < 0xF0) { runningStatus = status; lastChannelStatus = status; }
        }

        PackedRecord record;
        record.tick = tick;
        record.micros = 0;
        record.track = static_cast<uint16_t>(trackIndex);
        record.packed = 0;
        record.metadata = 0;
        if (status == 0xFF) {
            runningStatus = 0;
            uint8_t metaType = reader.read_byte();
            uint32_t length = reader.read_vlq();
            if (metaType == 0x51 && length == 3) {
                int32_t a = reader.read_byte();
                int32_t b = reader.read_byte();
                int32_t c = reader.read_byte();
                int32_t tempo = (a << 16) | (b << 8) | c;
                if (tempo > 0) tempos.push_back(TempoRecord{ tick, tempo, trackIndex, order });
            }
            else if ((metaType == 0x03 && !haveName) || (metaType == 0x04 && !haveInstrument)) {
                bool truncated;
                std::u16string text = read_display_text(reader, length, truncated);
                if (!text.empty()) {
                    if (metaType == 0x03) {
                        out.metadata.name = text; out.metadata.name_truncated = truncated; haveName = true;
                    }
                    else {
                        out.metadata.instrument = text; out.metadata.instrument_truncated = truncated; haveInstrument = true;
                    }
                }
            }
            else reader.skip(length);
            immediatelyAfterMeta = true;
        }
        else if (status == 0xF0 || status == 0xF7) {
            runningStatus = 0;
            immediatelyAfterMeta = false;
            uint32_t length = reader.read_vlq();
            int prefix = status == 0xF0 ? 1 : 0;
            const int64_t payloadLength = static_cast<int64_t>(length) + prefix;
            if (payloadLength <= 3) {
                // Up to three bytes stay in the record itself (F0 prefix included).
                uint32_t bytes = 0;
                int stored = 0;
                if (prefix != 0) { bytes = 0xF0; stored = 1; }
                for (; stored < payloadLength; ++stored)
                    bytes |= static_cast<uint32_t>(reader.read_byte()) << (8 * stored);
                if (out.events.count() == INT32_MAX) invalid(kEventLimit);
                record.packed = bytes;
                record.metadata = PackedRecord::make_metadata(EventKind::SystemExclusive, -1,
                    static_cast<int>(payloadLength), false, status == 0xF7);
                out.events.append(record);
            }
            else {
                if (prefix != 0) out.payloads.append(static_cast<uint8_t>(0xF0));
                reader.copy_payload(out.payloads, length);
                if (out.events.count() == INT32_MAX) invalid(kEventLimit);
                record.metadata = PackedRecord::make_metadata(EventKind::SystemExclusive, -1, 0, true, status == 0xF7);
                out.events.append_long(record, out.payloads.length());
            }
        }
        else if (status < 0xF0) {
            int dataLength = channel_data_length(status);
            uint8_t firstValue = 0, secondValue = 0;
            int dataOffset = 0;
            if (firstData >= 0) { firstValue = static_cast<uint8_t>(firstData); dataOffset = 1; }
            while (dataOffset < dataLength) {
                uint8_t value = reader.read_byte();
                if (value >= 0x80)
                    invalid("Unexpected status byte inside a channel message in track " + trackText + ".");
                if (dataOffset == 0) firstValue = value; else secondValue = value;
                ++dataOffset;
            }
            if (out.events.count() == INT32_MAX) invalid(kEventLimit);
            record.packed = static_cast<uint32_t>(status) | (static_cast<uint32_t>(firstValue) << 8) |
                (static_cast<uint32_t>(secondValue) << 16);
            record.metadata = PackedRecord::make_metadata(kind_from_status(status), status & 0x0F, dataLength + 1, false, false);
            out.events.append(record);
            if (repaired) ++out.running_status_repairs;
            immediatelyAfterMeta = false;
        }
        else {
            runningStatus = 0;
            immediatelyAfterMeta = false;
            int dataLength = system_data_length(status);
            uint8_t firstValue = dataLength > 0 ? reader.read_byte() : 0;
            uint8_t secondValue = dataLength > 1 ? reader.read_byte() : 0;
            if (out.events.count() == INT32_MAX) invalid(kEventLimit);
            record.packed = static_cast<uint32_t>(status) | (static_cast<uint32_t>(firstValue) << 8) |
                (static_cast<uint32_t>(secondValue) << 16);
            record.metadata = PackedRecord::make_metadata(EventKind::SystemMessage, -1, dataLength + 1, false, false);
            out.events.append(record);
        }
        ++order;
    }
    if (reporter.active())
        reporter.report(parsing_stage(trackIndex, progress.track_count), trackIndex + 1, progress.track_count,
            scale_progress(progress.prior_bytes + progress.declared_length, progress.file_length, 6000),
            progress.declared_length, stageTotal);
    out.end_tick = tick;
}


// ------------------------------------------------------------ merge

int64_t time_at(TempoClock& clock, int64_t tick)
{
    int64_t micros;
    if (!clock.time_at(tick, micros)) fail("OverflowException", messages::event_time_too_large());
    return micros;
}

// One node per track with events left: its next event. The order is the stable
// merge order (tick, then track); a track never has two nodes, so nothing else
// is needed to break ties.
struct MergeNode {
    int64_t tick;
    int32_t track;
    int64_t index;
};

bool node_less(const MergeNode& a, const MergeNode& b)
{
    if (a.tick != b.tick) return a.tick < b.tick;
    return a.track < b.track;
}

void heap_push(std::vector<MergeNode>& heap, const MergeNode& node)
{
    size_t index = heap.size();
    heap.push_back(node);
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!node_less(node, heap[parent])) break;
        heap[index] = heap[parent];
        index = parent;
    }
    heap[index] = node;
}

// Puts `node` at the root and sifts it down (the root's track just advanced).
void heap_replace_top(std::vector<MergeNode>& heap, const MergeNode& node)
{
    size_t index = 0;
    for (;;) {
        size_t child = index * 2 + 1;
        if (child >= heap.size()) break;
        if (child + 1 < heap.size() && node_less(heap[child + 1], heap[child])) ++child;
        if (!node_less(heap[child], node)) break;
        heap[index] = heap[child];
        index = child;
    }
    heap[index] = node;
}

void heap_pop(std::vector<MergeNode>& heap)
{
    MergeNode last = heap.back();
    heap.pop_back();
    if (!heap.empty()) heap_replace_top(heap, last);
}

MergeNode node_for(const ParsedTrack& track, int32_t trackIndex, int64_t index)
{
    return MergeNode{ track.events.packed_at(index).tick, trackIndex, index };
}

void load_impl(ByteSource& source, const LoadOptions& options, Song& song)
{
    song = Song();
    Reporter reporter(options.monitor);
    Container container = open_container(source);
    PayloadStream stream(source, container);
    Header header = read_header(stream, reporter);

    const int trackCount = header.track_count;
    const int64_t fileLength = std::max<int64_t>(1, stream.length());
    std::vector<ParsedTrack> tracks(static_cast<size_t>(trackCount));
    std::vector<TempoRecord> tempos;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[kTrackBufferSize]);
    int64_t totalTrackBytes = 0;
    int64_t totalEvents = 0;
    int64_t totalRepairs = 0;
    int64_t endTick = 0;
    for (int t = 0; t < trackCount; ++t) {
        reporter.check();
        require_chunk(stream, "MTrk");
        uint32_t length = read_u32be(stream);
        int64_t priorBytes = totalTrackBytes;
        ParseFillHook hook(reporter, t, trackCount, priorBytes, fileLength, length);
        TrackReader reader(stream, length, buffer.get(), kTrackBufferSize, hook);
        try {
            parse_track(reader, t, tracks[static_cast<size_t>(t)], tempos,
                TrackProgress{ reporter, trackCount, priorBytes, fileLength, length });
            tracks[static_cast<size_t>(t)].events.trim();
            tracks[static_cast<size_t>(t)].payloads.trim_last_segment();
        }
        catch (const std::bad_alloc&) {
            invalid("Memory allocation failed while parsing compact track " + std::to_string(t + 1) +
                " of " + std::to_string(trackCount) + ".");
        }
        const ParsedTrack& parsed = tracks[static_cast<size_t>(t)];
        totalTrackBytes += length;
        totalEvents += parsed.events.count();
        totalRepairs += parsed.running_status_repairs;
        if (totalEvents > INT32_MAX) invalid(kEventLimit);
        if (parsed.end_tick > endTick) endTick = parsed.end_tick;
    }

    reporter.check();
    reporter.report("Preparing tempo map", trackCount, trackCount, 6000, 0, std::max(1, trackCount));
    std::stable_sort(tempos.begin(), tempos.end(), [](const TempoRecord& a, const TempoRecord& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        return a.order < b.order;
    });
    TempoClock clock(tempos, header.division);
    const char kMergingStage[] = "Merging tracks and assigning timestamps";
    const int64_t mergeTotal = std::max<int64_t>(1, totalEvents);
    reporter.report(kMergingStage, trackCount, trackCount, 6500, 0, mergeTotal);

    EventStore& store = song.store;
    std::vector<MergeNode> heap;
    heap.reserve(tracks.size());
    for (size_t t = 0; t < tracks.size(); ++t)
        if (tracks[t].events.count() > 0) heap_push(heap, node_for(tracks[t], static_cast<int32_t>(t), 0));
    int64_t noteCount = 0;
    int64_t merged = 0;
    int64_t lastEventTime = 0;
    std::vector<uint8_t> scratch(65536);
    while (!heap.empty()) {
        // DD-056: the track at the top of the heap keeps supplying events while its next event still sorts before the
        // smallest next event of the other tracks (the smaller of the root's two children): the heap is touched only
        // when the order changes, not once per event. The order produced is exactly the heap's.
        MergeNode node = heap[0];
        ParsedTrack& track = tracks[static_cast<size_t>(node.track)];
        for (;;) {
            if ((merged & 16383) == 0) {
                reporter.check();
                reporter.report(kMergingStage, trackCount, trackCount, 6500 + scale_progress(merged, totalEvents, 3000),
                    merged, mergeTotal);
            }
            const PackedRecord& provisional = track.events.packed_at(node.index);
            int64_t intended = time_at(clock, provisional.tick);
            if (store.records.count() == INT32_MAX) invalid(kEventLimit);
            const int32_t eventIndex = static_cast<int32_t>(store.records.count());
            // Only controller, program, pressure and pitch-bend messages enter the indexes (notes, polyphonic
            // aftertouch, SysEx and system messages are ignored by both), so the others skip them.
            const unsigned kindOffset = static_cast<unsigned>(provisional.metadata & PackedRecord::kKindMask) -
                static_cast<unsigned>(EventKind::ControlChange);
            if (kindOffset <= static_cast<unsigned>(EventKind::PitchBend) - static_cast<unsigned>(EventKind::ControlChange)) {
                const IndexInput indexInput = index_input(provisional);
                song.source_index.add(indexInput, eventIndex);
                song.chase_index.add(indexInput, eventIndex);
            }
            if (provisional.kind() == EventKind::NoteOn && static_cast<uint8_t>(provisional.packed >> 16) != 0)
                ++noteCount;
            PackedRecord stored = provisional;
            stored.micros = intended;
            if (provisional.is_long()) {
                int64_t offset;
                int32_t length;
                track.events.long_extent(provisional, offset, length);
                for (int64_t copied = 0; copied < length;) {
                    const size_t chunk = static_cast<size_t>(std::min<int64_t>(length - copied, static_cast<int64_t>(scratch.size())));
                    track.payloads.copy_to(offset + copied, scratch.data(), chunk);
                    store.payloads.append(scratch.data(), chunk);
                    copied += static_cast<int64_t>(chunk);
                }
                store.records.append_long(stored, store.payloads.length());
                track.payloads.release_before(offset + length);
            }
            else {
                // Improvement DD-001 (v2/docs/DIFFERENCES_FROM_V1.md): a SysEx message of 1 to 3
                // bytes keeps its bytes (v1 read them back as zeros); like every short message
                // it lives in the record.
                store.records.append(stored);
            }
            lastEventTime = intended;
            track.events.release_consumed_through(node.index);
            const int64_t next = node.index + 1;
            ++merged;
            if (next >= track.events.count()) {
                heap_pop(heap);
                track.events = EventRecords();
                track.payloads = PayloadSegments();
                break;
            }
            const MergeNode following = node_for(track, node.track, next);
            if (heap.size() > 1) {
                const MergeNode& bound = heap.size() > 2 && node_less(heap[2], heap[1]) ? heap[2] : heap[1];
                if (!node_less(following, bound)) {
                    heap_replace_top(heap, following);
                    break;
                }
            }
            node = following;
        }
    }

    int64_t duration = time_at(clock, endTick);
    if (lastEventTime > duration) duration = lastEventTime;
    reporter.check();
    reporter.report("Finalizing indexes", trackCount, trackCount, 9500, merged, std::max<int64_t>(1, merged));
    store.records.trim();
    store.payloads.trim_last_segment();
    CancellationCheck check = [&reporter]() { reporter.check(); };
    song.source_index.complete(check);
    song.chase_index.complete(check);

    song.file_size = source.size();
    song.format = header.format;
    song.track_count = header.track_count;
    song.division = header.division;
    song.note_count = noteCount;
    song.running_status_repairs = totalRepairs;
    song.duration_us = duration;
    song.tracks.resize(tracks.size());
    for (size_t t = 0; t < tracks.size(); ++t) song.tracks[t] = std::move(tracks[t].metadata);

    if (options.collect_tempo_map) {
        TempoClock mapClock(tempos, header.division);
        song.tempo_map.reserve(tempos.size());
        for (const TempoRecord& change : tempos)
            song.tempo_map.push_back(TempoMapEntry{ change.tick, time_at(mapClock, change.tick),
                change.tempo, change.track, change.order });
    }
    reporter.report("Ready", trackCount, trackCount, kProgressScale, 1, 1);
}

// ------------------------------------------------------------ preflight

const int kSourceAttributeCount = 9;
enum Attribute { BankMsb = 0, BankLsb, Program, Volume, Expression, Pan, Sustain, PitchBend, Aftertouch };

int64_t checked_increment(int64_t value, const char* name)
{
    if (value == INT64_MAX) invalid(std::string("The MIDI contains too many ") + name + " records.");
    return value + 1;
}

int64_t checked_add(int64_t value, int64_t addition, const char* name)
{
    if (addition < 0 || value > INT64_MAX - addition) invalid(std::string("The MIDI contains too much ") + name + " data.");
    return value + addition;
}

void add_dispatchable(PreflightCounts& counts, int track)
{
    counts.dispatchable_events = checked_increment(counts.dispatchable_events, "dispatchable-event");
    if (counts.dispatchable_events > INT32_MAX) invalid(kEventLimit);
    counts.events_by_track[static_cast<size_t>(track)] =
        checked_increment(counts.events_by_track[static_cast<size_t>(track)], "track event");
}

void add_source(PreflightCounts& counts, int channel, Attribute attribute)
{
    size_t slot = static_cast<size_t>(channel * kSourceAttributeCount + attribute);
    counts.source_entries_by_slot[slot] = checked_increment(counts.source_entries_by_slot[slot], "source-value entry");
    counts.source_value_entries = checked_increment(counts.source_value_entries, "source-value entry");
}

void add_source_value_entries(PreflightCounts& counts, int channel, int high, uint8_t first)
{
    if (high == 0xB0 && first == 121) {
        add_source(counts, channel, Expression);
        add_source(counts, channel, Pan);
        add_source(counts, channel, Sustain);
        add_source(counts, channel, PitchBend);
        add_source(counts, channel, Aftertouch);
        return;
    }
    Attribute attribute;
    if (high == 0xC0) attribute = Program;
    else if (high == 0xD0) attribute = Aftertouch;
    else if (high == 0xE0) attribute = PitchBend;
    else if (high == 0xB0) {
        switch (first) {
        case 0: attribute = BankMsb; break;
        case 32: attribute = BankLsb; break;
        case 7: attribute = Volume; break;
        case 11: attribute = Expression; break;
        case 10: attribute = Pan; break;
        case 64: attribute = Sustain; break;
        default: return;
        }
    }
    else return;
    add_source(counts, channel, attribute);
}

void add_chase_entries(PreflightCounts& counts, int high, uint8_t first)
{
    if (high == 0xC0 || high == 0xD0 || high == 0xE0 || (high == 0xB0 && (first < 120 || first == 121)))
        counts.chase_state_entries = checked_increment(counts.chase_state_entries, "whole-state chase entry");
    if (high == 0xB0 && (first == 6 || first == 38 || first == 96 || first == 97))
        counts.chase_parameter_value_entries =
            checked_increment(counts.chase_parameter_value_entries, "parameter-state chase entry");
}

void scan_track(TrackReader& reader, int trackIndex, PreflightCounts& counts, std::vector<uint8_t>& laneSeen,
    Reporter& reporter, int64_t trackStart, int64_t fileLength, uint32_t declaredLength)
{
    uint8_t runningStatus = 0;
    uint8_t lastChannelStatus = 0;
    bool immediatelyAfterMeta = false;
    int32_t order = 0;
    const std::string trackText = std::to_string(trackIndex);
    const size_t track = static_cast<size_t>(trackIndex);
    while (!reader.at_end()) {
        if ((order & 4095) == 0) {
            reporter.check();
            reporter.report(kInspectingStage, trackIndex, counts.track_count, trackStart + reader.consumed(),
                fileLength, reader.consumed(), std::max<int64_t>(1, declaredLength));
        }
        reader.read_vlq();
        if (reader.at_end()) invalid("Track " + trackText + " ends after a delta time.");
        uint8_t first = reader.read_byte();
        uint8_t status;
        int firstData = -1;
        bool repaired = false;
        if (first < 0x80) {
            if (runningStatus < 0x80 || runningStatus >= 0xF0) {
                if (!can_repair_after_meta(lastChannelStatus, immediatelyAfterMeta, first))
                    invalid("Invalid running status in track " + trackText + ".");
                status = lastChannelStatus;
                runningStatus = status;
                repaired = true;
            }
            else status = runningStatus;
            firstData = first;
        }
        else {
            status = first;
            if (status < 0xF0) { runningStatus = status; lastChannelStatus = status; }
        }

        if (status == 0xFF) {
            runningStatus = 0;
            uint8_t metaType = reader.read_byte();
            uint32_t length = reader.read_vlq();
            if (metaType == 0x51 && length == 3) {
                int32_t a = reader.read_byte();
                int32_t b = reader.read_byte();
                int32_t c = reader.read_byte();
                if (((a << 16) | (b << 8) | c) > 0) {
                    counts.tempo_changes = checked_increment(counts.tempo_changes, "tempo-change");
                    counts.tempo_changes_by_track[track] =
                        checked_increment(counts.tempo_changes_by_track[track], "track tempo-change");
                }
            }
            else reader.skip(length);
            immediatelyAfterMeta = true;
        }
        else if (status == 0xF0 || status == 0xF7) {
            runningStatus = 0;
            immediatelyAfterMeta = false;
            uint32_t length = reader.read_vlq();
            int64_t payloadLength = static_cast<int64_t>(length) + (status == 0xF0 ? 1 : 0);
            add_dispatchable(counts, trackIndex);
            counts.provisional_payload_bytes = checked_add(counts.provisional_payload_bytes, payloadLength, "SysEx payload");
            counts.payload_bytes_by_track[track] =
                checked_add(counts.payload_bytes_by_track[track], payloadLength, "track SysEx payload");
            if (payloadLength > 3) {
                counts.final_long_payload_bytes = checked_add(counts.final_long_payload_bytes, payloadLength, "long payload");
                counts.final_long_payload_events = checked_increment(counts.final_long_payload_events, "long payload");
            }
            reader.skip(length);
        }
        else if (status < 0xF0) {
            int dataLength = channel_data_length(status);
            uint8_t firstValue = 0, secondValue = 0;
            int dataOffset = 0;
            if (firstData >= 0) { firstValue = static_cast<uint8_t>(firstData); dataOffset = 1; }
            while (dataOffset < dataLength) {
                uint8_t value = reader.read_byte();
                if (value >= 0x80)
                    invalid("Unexpected status byte inside a channel message in track " + trackText + ".");
                if (dataOffset == 0) firstValue = value; else secondValue = value;
                ++dataOffset;
            }
            add_dispatchable(counts, trackIndex);
            int high = status & 0xF0;
            int channel = status & 0x0F;
            if (high == 0x90 && secondValue != 0) {
                counts.note_ons = checked_increment(counts.note_ons, "NoteOn");
                size_t slot = track * 16 + static_cast<size_t>(channel);
                if (!laneSeen[slot]) {
                    laneSeen[slot] = 1;
                    counts.routing_lanes = checked_increment(counts.routing_lanes, "track-routing lane");
                    counts.routing_note_lanes_by_channel[static_cast<size_t>(channel)] = checked_increment(
                        counts.routing_note_lanes_by_channel[static_cast<size_t>(channel)], "track-routing channel lane");
                }
            }
            if (high == 0xC0 || (high == 0xB0 && (firstValue == 0 || firstValue == 32))) {
                counts.routing_patch_state_events =
                    checked_increment(counts.routing_patch_state_events, "track-routing patch-state event");
                counts.routing_patch_state_events_by_channel[static_cast<size_t>(channel)] = checked_increment(
                    counts.routing_patch_state_events_by_channel[static_cast<size_t>(channel)],
                    "track-routing channel patch-state event");
            }
            add_source_value_entries(counts, channel, high, firstValue);
            add_chase_entries(counts, high, firstValue);
            if (repaired)
                counts.running_status_repairs = checked_increment(counts.running_status_repairs, "running-status repair");
            immediatelyAfterMeta = false;
        }
        else {
            runningStatus = 0;
            immediatelyAfterMeta = false;
            int dataLength = system_data_length(status);
            if (dataLength > 0) reader.read_byte();
            if (dataLength > 1) reader.read_byte();
            add_dispatchable(counts, trackIndex);
        }
        ++order;
    }
}

int64_t sat_add(int64_t left, int64_t right) { return left >= INT64_MAX - right ? INT64_MAX : left + right; }
int64_t sat_mul(int64_t left, int64_t right)
{
    return left == 0 || right == 0 ? 0 : left > INT64_MAX / right ? INT64_MAX : left * right;
}
int64_t warning_threshold(int ps, int64_t overrideBytes = 0)
{
    if (overrideBytes > 0) return overrideBytes;
    return ps == 8 ? 4LL * 1024 * 1024 * 1024 : 1024LL * 1024 * 1024;
}
int64_t divide_round_up(int64_t value, int64_t divisor) { return value == 0 ? 0 : 1 + (value - 1) / divisor; }
int64_t align(int64_t value, int alignment)
{
    int64_t remainder = value % alignment;
    return remainder == 0 ? value : sat_add(value, alignment - remainder);
}

// ---- v2 memory footprint (improvement DD-024). Every size is what this program stores, not what the .NET
// runtime of v1 would have used. `ps` is the pointer size of the build being estimated (4 or 8).
const int64_t kStoredRecordBytes = 24;           // PackedRecord
const int64_t kLongTableEntryBytes = 8;          // one end offset per SysEx of 4 bytes or more
const int64_t kAllocationOverheadBytes = 16;     // per segment allocation
const int64_t kIndexEntryBytes = 8;
const int64_t kIndexSegmentBytes = 4096 * kIndexEntryBytes;
const int64_t kTempoRecordBytes = 24;
const int64_t kReaderBufferBytes = 65536;
const int64_t kFixedWorkspaceBytes = 1024 * 1024;
const int kRecordSegmentCapacity = 2048;
const int kPayloadSegmentSize = 1024 * 1024;
const int64_t kMaximumActiveAttacks = 1000000;

int64_t record_bytes(int64_t events)
{
    return sat_add(sat_mul(events, kStoredRecordBytes),
        sat_mul(divide_round_up(events, kRecordSegmentCapacity), kAllocationOverheadBytes));
}

void preflight_impl(ByteSource& source, PreflightCounts& counts, LoadMonitor* monitor)
{
    Reporter reporter(monitor);
    Container container = open_container(source);
    PayloadStream stream(source, container);
    const int64_t fileLength = std::max<int64_t>(1, stream.length());
    reporter.report(kInspectingStage, 0, 0, 0, fileLength, 0, fileLength);
    Header header = read_header(stream, reporter);
    counts = PreflightCounts();
    counts.file_size = source.size();
    counts.format = header.format;
    counts.track_count = header.track_count;
    counts.division = header.division;
    counts.events_by_track.assign(static_cast<size_t>(header.track_count), 0);
    counts.payload_bytes_by_track.assign(static_cast<size_t>(header.track_count), 0);
    counts.tempo_changes_by_track.assign(static_cast<size_t>(header.track_count), 0);
    std::vector<uint8_t> laneSeen(static_cast<size_t>(header.track_count) * 16, 0);
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[kTrackBufferSize]);
    for (int t = 0; t < header.track_count; ++t) {
        reporter.check();
        require_chunk(stream, "MTrk");
        uint32_t length = read_u32be(stream);
        int64_t trackStart = stream.position();
        ScanFillHook hook(reporter, t, header.track_count, trackStart, fileLength, length);
        TrackReader reader(stream, length, buffer.get(), kTrackBufferSize, hook);
        scan_track(reader, t, counts, laneSeen, reporter, trackStart, fileLength, length);
    }
    for (size_t channel = 0; channel < 16; ++channel)
        counts.routing_patch_timeline_upper_bound = sat_add(counts.routing_patch_timeline_upper_bound,
            sat_mul(counts.routing_note_lanes_by_channel[channel], counts.routing_patch_state_events_by_channel[channel]));
    counts.routing_generated_state_upper_bound =
        sat_mul(3, sat_add(counts.note_ons, counts.routing_patch_state_events));
    reporter.check();
    reporter.report(kInspectingStage, header.track_count, header.track_count, fileLength, fileLength, 1, 1);
}

template <typename F>
bool guarded(LoadError& error, F body)
{
    try {
        body();
        return true;
    }
    catch (const LoadFailure& failure) {
        error.type = failure.type;
        error.message = failure.message;
    }
    catch (const std::bad_alloc&) {
        error.type = "OutOfMemoryException";
        error.message = messages::out_of_memory();
    }
    return false;
}

// Rescales progress from an inner stage into a share of the whole flow, as
// the delegates in v1 MidiFileParser.LoadWithPreflight do.
class ScaledMonitor : public LoadMonitor {
public:
    ScaledMonitor(LoadMonitor* inner, int64_t base, int64_t share) : inner_(inner), base_(base), share_(share) {}
    void progress(const LoadProgress& p) override
    {
        if (!inner_) return;
        inner_->progress(LoadProgress(p.stage, p.completed_tracks, p.total_tracks,
            base_ + p.overall_completed * share_ / p.overall_total, kProgressScale, p.stage_completed, p.stage_total));
    }
    bool cancellation_requested() override { return inner_ && inner_->cancellation_requested(); }

private:
    LoadMonitor* inner_;
    int64_t base_;
    int64_t share_;
};

} // namespace

const char kCancellationType[] = "OperationCanceledException";
const char kCancellationMessage[] = "The operation was canceled.";

LoadProgress::LoadProgress(const std::string& stage_, int completed_tracks_, int total_tracks_,
    int64_t overall_completed_, int64_t overall_total_, int64_t stage_completed_, int64_t stage_total_)
    : stage(stage_), completed_tracks(completed_tracks_), total_tracks(total_tracks_)
{
    overall_completed = std::max<int64_t>(0, std::min<int64_t>(std::max<int64_t>(1, overall_total_), overall_completed_));
    overall_total = std::max<int64_t>(1, overall_total_);
    stage_completed = std::max<int64_t>(0, std::min<int64_t>(std::max<int64_t>(1, stage_total_), stage_completed_));
    stage_total = std::max<int64_t>(1, stage_total_);
}

int LoadProgress::overall_permille() const
{ return static_cast<int>(std::min<int64_t>(1000, overall_completed * 1000 / overall_total)); }
int LoadProgress::stage_permille() const
{ return static_cast<int>(std::min<int64_t>(1000, stage_completed * 1000 / stage_total)); }

bool is_cancellation(const LoadError& error) { return error.type == kCancellationType; }

bool load_song(ByteSource& source, const LoadOptions& options, Song& song, LoadError& error)
{
    return guarded(error, [&]() { load_impl(source, options, song); });
}

bool probe_container(ByteSource& source, bool& is_rmid, LoadError& error)
{
    return guarded(error, [&]() { is_rmid = open_container(source).rmid; });
}

bool preflight_scan(ByteSource& source, PreflightCounts& counts, LoadError& error, LoadMonitor* monitor)
{
    return guarded(error, [&]() { preflight_impl(source, counts, monitor); });
}

LoadFlowResult load_with_preflight(ByteSource& source, int pointer_size, const LoadFlowOptions& options,
    Song& song, LoadError& error)
{
    LoadOptions load;
    load.collect_tempo_map = options.collect_tempo_map;
    if (!options.force_preflight && !preflight_required(source.size(), pointer_size, options.warning_threshold)) {
        load.monitor = options.monitor;
        return load_song(source, load, song, error) ? LoadFlowResult::Loaded : LoadFlowResult::Failed;
    }

    PreflightCounts counts;
    MemoryProjection projection;
    bool accepted = true;
    bool ok = guarded(error, [&]() {
        ScaledMonitor inspection(options.monitor, 0, 2500);
        preflight_impl(source, counts, options.monitor ? &inspection : nullptr);
        projection = preflight_estimate(counts, pointer_size, options.warning_threshold);
        Reporter reporter(options.monitor);
        reporter.check();
        if (options.force_warning || preflight_requires_warning(projection)) {
            reporter.report("Waiting for confirmation", counts.track_count, counts.track_count, 2500, kProgressScale, 1, 1);
            if (!options.decision || !options.decision(counts, projection)) { accepted = false; return; }
        }
        reporter.check();
    });
    if (!ok) return LoadFlowResult::Failed;
    if (!accepted) return LoadFlowResult::Declined;
    ScaledMonitor loading(options.monitor, 2500, 7500);
    load.monitor = options.monitor ? &loading : nullptr;
    return load_song(source, load, song, error) ? LoadFlowResult::Loaded : LoadFlowResult::Failed;
}

int64_t half_address_space_warning_threshold(uint64_t maximumApplicationAddress)
{
    if (maximumApplicationAddress == 0) return 0;
    const uint64_t unit = 64ULL * 1024 * 1024;
    uint64_t span = maximumApplicationAddress + 1;
    uint64_t rounded = (span + unit / 2) / unit * unit;
    if (rounded == 0) rounded = unit;
    return static_cast<int64_t>(rounded / 2);
}

MemoryProjection preflight_estimate(const PreflightCounts& counts, int ps, int64_t warningOverride)
{
    const bool wide = ps == 8;
    const int64_t indexSeriesBytes = wide ? 56 : 28;
    const int64_t parameterSeriesBytes = wide ? 176 : 96;  // series, ordered-map node while building, result entry
    const int64_t trackObjectBytes = wide ? 192 : 120;     // a track's provisional lists and metadata
    const int64_t trackMetadataBytes = wide ? 80 : 56;
    const int64_t displayTextBytes = 2 * (128 * 2 + 16);   // track name and instrument name, up to 128 UTF-16 units each

    // What the loader keeps once the song is loaded.
    int64_t names = sat_mul(counts.track_count, sat_add(trackMetadataBytes, displayTextBytes));
    int64_t storeCore = sat_add(record_bytes(counts.dispatchable_events),
        sat_add(sat_mul(counts.final_long_payload_events, kLongTableEntryBytes), counts.final_long_payload_bytes));
    int64_t retainedCore = sat_add(storeCore, names);

    int64_t indexEntries = sat_add(counts.source_value_entries,
        sat_add(counts.chase_state_entries, counts.chase_parameter_value_entries));
    int64_t parameterSeries = std::min<int64_t>(counts.chase_parameter_value_entries, 16LL * 32768LL);
    int64_t fixedSeries = 16LL * 123 + 32 + 16LL * 9;
    int64_t indexBytes = sat_add(sat_mul(indexEntries, kIndexEntryBytes), sat_add(sat_mul(fixedSeries, indexSeriesBytes),
        sat_mul(parameterSeries, parameterSeriesBytes)));
    int64_t retained = sat_add(retainedCore, indexBytes);

    // What the per-track lists hold before the merge consumes them (payload bytes are an upper bound: the
    // preflight counts short SysEx bytes too, which the loader keeps in the records).
    int64_t provisional = sat_add(sat_mul(counts.tempo_changes, kTempoRecordBytes),
        sat_mul(counts.final_long_payload_events, kLongTableEntryBytes));
    int64_t releaseGranularity = 0;
    for (int track = 0; track < counts.track_count; ++track) {
        size_t t = static_cast<size_t>(track);
        provisional = sat_add(provisional, sat_add(record_bytes(counts.events_by_track[t]), counts.payload_bytes_by_track[t]));
        provisional = sat_add(provisional, trackObjectBytes);
        // Segments are freed whole, so each track holds back up to one record segment and one payload segment.
        releaseGranularity = sat_add(releaseGranularity, std::min<int64_t>(sat_mul(counts.events_by_track[t], kStoredRecordBytes),
            kRecordSegmentCapacity * kStoredRecordBytes));
        releaseGranularity = sat_add(releaseGranularity, std::min<int64_t>(counts.payload_bytes_by_track[t], kPayloadSegmentSize));
    }

    int64_t workspace = sat_add(sat_mul(counts.track_count, 32), sat_add(kReaderBufferBytes, kFixedWorkspaceBytes));

    // The merge frees provisional records as it fills the store, so the two together stay near one copy; the
    // indexes grow on top of that. Index series start small and grow by doubling, so a series may briefly hold
    // up to twice its entries; the last 4,096-entry segment of a big series is allocated whole.
    int64_t lower = sat_add(sat_add(std::max(sat_add(provisional, names), retainedCore), indexBytes), workspace);
    int64_t indexSlack = std::min<int64_t>(indexBytes,
        sat_mul(sat_add(fixedSeries, parameterSeries), kIndexSegmentBytes));
    int64_t upper = sat_add(sat_add(lower, releaseGranularity), indexSlack);

    // Track routing (compile_route and the routed view), sized from the structures it builds.
    // Improvement DD-027: the generated Bank/Program events are kept as 12-byte groups (at most one group per
    // note start or patch-state event, three events at most per group) and the routed view is one int32 per
    // routed event, so a worst case that generates a group at every note start still costs little.
    const int64_t generatedGroupBytes = 12;
    const int64_t routedViewEntryBytes = 4;
    int64_t routedEventUpperBound = sat_add(counts.dispatchable_events, counts.routing_generated_state_upper_bound);
    int64_t generatedGroupUpperBound = sat_add(counts.note_ons, counts.routing_patch_state_events);
    int64_t routingRetained = counts.dispatchable_events;  // one destination byte per event
    routingRetained = sat_add(routingRetained, sat_mul(routedEventUpperBound, routedViewEntryBytes));
    routingRetained = sat_add(routingRetained, sat_mul(counts.note_ons, 44));  // sounding interval + sorted end index
    routingRetained = sat_add(routingRetained, sat_mul(counts.routing_patch_timeline_upper_bound, 16));
    routingRetained = sat_add(routingRetained, sat_mul(counts.routing_lanes, wide ? 1024 : 768));
    routingRetained = sat_add(routingRetained, sat_mul(generatedGroupUpperBound, generatedGroupBytes));
    int64_t routingWorkspace = sat_mul(std::min(counts.note_ons, kMaximumActiveAttacks), wide ? 64 : 44);
    routingWorkspace = sat_add(routingWorkspace, sat_mul(counts.note_ons, 22));  // interval lists while they grow
    routingWorkspace = sat_add(routingWorkspace, sat_mul(counts.routing_patch_timeline_upper_bound, 8));
    // The group list grows by doubling: the old and the new array exist together for a moment.
    routingWorkspace = sat_add(routingWorkspace, sat_mul(generatedGroupUpperBound, 2 * generatedGroupBytes));
    int64_t routingPeak = sat_add(routingRetained, routingWorkspace);
    int64_t warning = warning_threshold(ps, warningOverride);

    MemoryProjection p;
    p.retained = retained;
    p.peak_low = lower;
    p.peak_high = upper;
    p.provisional = provisional;
    p.merge_workspace = workspace;
    p.retained_names = names;
    p.routing_retained = routingRetained;
    p.routing_peak = routingPeak;
    p.retained_with_routing = sat_add(retained, routingRetained);
    p.routing_peak_total = sat_add(retained, routingPeak);
    p.pointer_size = ps;
    p.warning_threshold = warning;
    p.architecture_risk = ps == 4 && (upper >= warning || sat_add(retained, routingPeak) >= warning);
    return p;
}

bool preflight_requires_warning(const MemoryProjection& p)
{
    return p.peak_high >= p.warning_threshold || p.retained >= p.warning_threshold ||
        p.retained_with_routing >= p.warning_threshold || p.routing_peak_total >= p.warning_threshold;
}

bool preflight_required(int64_t file_size, int ps, int64_t warningOverride)
{
    const int64_t fixedExpansion = 64LL * 1024 * 1024;
    int64_t threshold = std::max<int64_t>(1024LL * 1024, (warning_threshold(ps, warningOverride) - fixedExpansion) / 64);
    return file_size >= threshold;
}

} // namespace midibn
