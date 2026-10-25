// System Exclusive assembly and failure diagnostics (portable; no OS calls).
// Port of v1 SystemExclusiveAssembler.cs, SystemExclusivePacket/Fragment and
// SystemExclusiveDiagnostics.cs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace output {

// One SMF System Exclusive event as v1 MidiEventView exposes it to an output:
// `status` is 0xF0 or 0xF7; `data` is v1 MidiEventData (for an F0 event the
// stored bytes start with 0xF0, for an F7 event they are the raw payload).
struct SystemExclusiveEvent {
    uint8_t status = 0;
    const uint8_t* data = nullptr;
    size_t length = 0;
    int32_t event_index = 0;     // v1 MidiEventView.EventIndex
    int64_t tick = 0;            // v1 MidiEventView.AbsoluteTick
    int64_t micros = 0;          // v1 MidiEventView.IntendedMicroseconds
};

// v1 SystemExclusiveFragment.
struct SystemExclusiveFragment {
    uint8_t status = 0;
    int32_t data_length = 0;
    int32_t event_index = 0;
    int64_t tick = 0;
    int64_t micros = 0;
};

// v1 SystemExclusivePacket: a complete F0...F7 message and the events it came from.
struct SystemExclusivePacket {
    std::vector<uint8_t> bytes;
    std::vector<SystemExclusiveFragment> fragments;
};

// v1 SystemExclusiveAssembler: an F0 event starts a packet (discarding any
// unfinished one), an F7 event continues it, and only a complete F0...F7
// packet is returned. winmm drivers may reject an unframed continuation.
class SystemExclusiveAssembler {
public:
    // Returns true and fills `packet` when `event` completes a packet
    // (v1 AcceptPacket returning non-null).
    bool accept(const SystemExclusiveEvent& event, SystemExclusivePacket& packet);
    // v1 Reset: drops any unfinished packet.
    void reset();
    // True while an F0 packet is waiting for its F7 (v1 _pending != null).
    bool pending() const { return pending_; }
    // v1 IsComplete(byte[]): at least two bytes, first F0, last F7.
    static bool is_complete(const uint8_t* bytes, size_t length);

private:
    bool finish_if_complete(SystemExclusivePacket& packet);
    bool pending_ = false;
    std::vector<uint8_t> bytes_;
    std::vector<SystemExclusiveFragment> fragments_;
};

// v1 SystemExclusiveDiagnostics.DescribeFailure (UTF-8 text; lines end in CR LF
// as .NET AppendLine does on Windows). `data_pointer` is the MIDIHDR lpData
// value, `pointer_size` the process pointer size (IntPtr.Size).
std::string describe_sysex_failure(const std::string& native_operation, uint32_t error_code,
    const std::string& native_text, const std::string& source_file, const SystemExclusivePacket* packet,
    const SystemExclusiveEvent& final_event, int32_t header_size, uint32_t buffer_length, uint32_t bytes_recorded,
    uint32_t flags, uint64_t data_pointer, int pointer_size);

} // namespace output
} // namespace midibn
