// Portable output backends that make no sound and touch no OS API:
//  - NullMidiOutput: v1 NullMidiOutput, the diagnostic "None" sink.
//  - RecordingMidiOutput: a test double (not in v1) that behaves like a real
//    backend (open state, SysEx assembly, v1 Panic order) and records what a
//    device would have received.
#pragma once

#include "midibn/midi_output.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace platform {

// v1 NullMidiOutput.cs: every call succeeds and does nothing. DisplayName is
// the text v1 shows for the synthetic selector item.
class NullMidiOutput : public MidiOutput {
public:
    static const char* display_name() { return "None"; }

    const char* backend_name() const override { return "None"; }
    std::vector<OutputDeviceInfo> enumerate() const override { return std::vector<OutputDeviceInfo>(); }
    OutputStatus open(uint32_t) override { return OutputStatus::success(); }
    OutputStatus close() override { source_file_.clear(); return OutputStatus::success(); }   // v1 Close clears SourceFile
    bool is_open() const override { return true; }
    OutputStatus send_short(uint32_t) override { return OutputStatus::success(); }
    OutputStatus send_long(const output::SystemExclusiveEvent&) override { return OutputStatus::success(); }
    OutputStatus reset() override { return OutputStatus::success(); }
    OutputStatus panic() override { return OutputStatus::success(); }
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string& path_utf8) override { source_file_ = path_utf8; }
    const std::string& source_file() const { return source_file_; }

private:
    std::string source_file_;
};

// One thing the recording output received, in order.
struct RecordedMessage {
    enum Kind { Short, Long, Reset } kind;
    uint32_t packed;               // Short
    std::vector<uint8_t> bytes;    // Long: the assembled F0...F7 packet
};

class RecordingMidiOutput : public MidiOutput {
public:
    const char* backend_name() const override { return "Recording"; }
    std::vector<OutputDeviceInfo> enumerate() const override { return std::vector<OutputDeviceInfo>(); }
    OutputStatus open(uint32_t) override { open_ = true; return OutputStatus::success(); }
    OutputStatus close() override;
    bool is_open() const override { return open_; }
    OutputStatus send_short(uint32_t packed) override;
    OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override;
    OutputStatus reset() override;
    OutputStatus panic() override;
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string& path_utf8) override { source_file_ = path_utf8; }

    std::vector<RecordedMessage> messages;

private:
    bool open_ = false;
    std::string source_file_;
    output::SystemExclusiveAssembler assembler_;
};

} // namespace platform
} // namespace midibn
