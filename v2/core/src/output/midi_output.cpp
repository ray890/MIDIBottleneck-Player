#include "midibn/midi_output.h"
#include "midibn/null_output.h"

namespace midibn {
namespace platform {

// v1 KdmApiMidiOutput.PackShortMessage (KdmApiMidiOutput.cs lines 310-317).
uint32_t pack_short_message(const uint8_t* data, size_t length)
{
    if (data == nullptr || length == 0) return 0;
    uint32_t message = data[0];
    if (length > 1) message |= static_cast<uint32_t>(data[1]) << 8;
    if (length > 2) message |= static_cast<uint32_t>(data[2]) << 16;
    return message;
}

// v1 Panic loops: WindowsMidiOutput.cs lines 184-201 and KdmApiMidiOutput.cs
// lines 319-329 (both: per channel CC120, CC123, CC64 = 0).
PanicMessage panic_message(int index)
{
    static const struct { uint32_t controller; const char* name; } kSteps[3] = {
        { 120, "CC120" }, { 123, "CC123" }, { 64, "sustain-off" }
    };
    uint32_t channel = static_cast<uint32_t>(index / 3);
    uint32_t status = 0xB0u | channel;
    PanicMessage message;
    message.packed = status | (kSteps[index % 3].controller << 8);
    message.name = kSteps[index % 3].name;
    return message;
}

// v1 MidiOutputSafety.ResetAndSilence (Model.cs lines 564-583).
OutputStatus reset_and_silence(MidiOutput* output)
{
    if (output == nullptr) return OutputStatus::success();
    OutputStatus resetFailure = output->reset();
    OutputStatus panicFailure = output->panic();
    if (!panicFailure.ok()) {
        if (resetFailure.ok()) return panicFailure;   // v1 rethrows the panic exception unchanged
        return OutputStatus::failure(OutputErrorKind::InvalidOperation, "MIDI reset and post-reset panic both failed.");
    }
    if (!resetFailure.ok()) {
        OutputStatus status = OutputStatus::failure(OutputErrorKind::InvalidOperation,
            "MIDI reset failed; a post-reset panic was still attempted.");
        status.inner = resetFailure.message;
        return status;
    }
    return OutputStatus::success();
}

// ---------------------------------------------------------------- RecordingMidiOutput

OutputStatus RecordingMidiOutput::close()
{
    assembler_.reset();
    open_ = false;
    return OutputStatus::success();
}

OutputStatus RecordingMidiOutput::send_short(uint32_t packed)
{
    if (!open_) return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    RecordedMessage message;
    message.kind = RecordedMessage::Short;
    message.packed = packed;
    messages.push_back(message);
    return OutputStatus::success();
}

OutputStatus RecordingMidiOutput::send_long(const output::SystemExclusiveEvent& fragment)
{
    if (!open_) return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    if (fragment.length == 0) return OutputStatus::success();
    output::SystemExclusivePacket packet;
    if (assembler_.accept(fragment, packet)) {
        RecordedMessage message;
        message.kind = RecordedMessage::Long;
        message.packed = 0;
        message.bytes = packet.bytes;
        messages.push_back(message);
    }
    return OutputStatus::success();
}

OutputStatus RecordingMidiOutput::reset()
{
    assembler_.reset();
    if (!open_) return OutputStatus::success();
    RecordedMessage message;
    message.kind = RecordedMessage::Reset;
    message.packed = 0;
    messages.push_back(message);
    return OutputStatus::success();
}

OutputStatus RecordingMidiOutput::panic()
{
    if (!open_) return OutputStatus::success();
    for (int i = 0; i < kPanicMessageCount; ++i) {
        RecordedMessage message;
        message.kind = RecordedMessage::Short;
        message.packed = panic_message(i).packed;
        messages.push_back(message);
    }
    return OutputStatus::success();
}

} // namespace platform
} // namespace midibn
