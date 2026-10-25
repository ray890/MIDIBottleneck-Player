// MIDI output contract (Milestone 6; ARCHITECTURE.md section 3 "Output
// contract" and section 4 "MidiOutput"). Port of v1 Model.cs IMidiOutput,
// IMidiOutputContext and MidiOutputSafety. Backends: WinMM and KDMAPI
// (platform/win32), None and the recording test double (core/src/output).
//
// Errors cross this boundary as OutputStatus values carrying the user-visible
// message text (v1's wording, or the plain sentences of register entry DD-003);
// nothing here throws. Calls are synchronous and
// made from the worker thread or, with no worker, from the app thread under
// the engine lock (as v1 does); a backend does no locking of its own.
#pragma once

#include "midibn/sysex_assembler.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace midibn {
namespace platform {

// The .NET exception type v1 would have thrown (decides how a caller words or
// classifies the failure; `message` is v1's exception Message).
enum class OutputErrorKind : uint8_t {
    None = 0,
    InvalidOperation,   // InvalidOperationException
    NotSupported,       // NotSupportedException
    Win32,              // Win32Exception; native_code is the (signed) native code
    FileNotFound,       // FileNotFoundException
    BadImageFormat,     // BadImageFormatException
    EntryPointNotFound, // EntryPointNotFoundException
    DirectoryNotFound,  // DirectoryNotFoundException
    DriveNotFound,      // DriveNotFoundException
    PathTooLong,        // PathTooLongException
    UnauthorizedAccess, // UnauthorizedAccessException
    IoError,            // IOException
    OperationCanceled,  // OperationCanceledException
    OutOfMemory         // OutOfMemoryException
};

struct OutputStatus {
    OutputErrorKind kind = OutputErrorKind::None;
    int32_t native_code = 0;
    std::string message;   // UTF-8, v1 text
    std::string inner;     // inner exception messages (v1 InnerException), newline separated; usually empty

    bool ok() const { return kind == OutputErrorKind::None; }
    static OutputStatus success() { return OutputStatus(); }
    static OutputStatus failure(OutputErrorKind kind, std::string message, int32_t nativeCode = 0)
    {
        OutputStatus status;
        status.kind = kind;
        status.native_code = nativeCode;
        status.message = std::move(message);
        return status;
    }
};

// One output device with its WinMM MIDIOUTCAPS fields (v1 MidiOutputDeviceInfo
// carries only DeviceId and Name; the capability fields are additions for the
// enumeration tool and the device list).
struct OutputDeviceInfo {
    uint32_t device_id = 0;
    std::string name;            // UTF-8; v1 substitutes "MIDI output <id>" for an empty name
    uint16_t manufacturer_id = 0;
    uint16_t product_id = 0;
    uint32_t driver_version = 0;
    uint16_t technology = 0;     // MOD_* value
    uint16_t voices = 0;
    uint16_t notes = 0;
    uint16_t channel_mask = 0;
    uint32_t support = 0;        // MIDICAPS_* flags
};

class MidiOutput {
public:
    virtual ~MidiOutput() {}

    virtual const char* backend_name() const = 0;
    // v1 GetDevices (WinMM). KDMAPI and None have no device list and return nothing.
    virtual std::vector<OutputDeviceInfo> enumerate() const = 0;

    // v1 Open(uint deviceId) for WinMM; KDMAPI and None ignore `device_id`.
    virtual OutputStatus open(uint32_t device_id) = 0;
    // v1 Close/Dispose. A reported failure leaves the output closed.
    virtual OutputStatus close() = 0;
    virtual bool is_open() const = 0;

    // v1 Send for a non-SysEx event: `packed` is v1 PackedShortMessage
    // (status | data1 << 8 | data2 << 16). The caller skips events with no data
    // (v1 returns at DataLength == 0).
    virtual OutputStatus send_short(uint32_t packed) = 0;
    // The same send with a cheap success path (scan P11 item 1): returns true when the message was sent and builds no
    // status then. On failure it returns false and stores exactly the status send_short would have returned in
    // `failure` (same kind, text, native code). The default calls send_short; the direct WinMM output, the None
    // output and KDMAPI override it so the per-message path creates and destroys no strings.
    virtual bool try_send_short(uint32_t packed, OutputStatus& failure)
    {
        OutputStatus status = send_short(packed);
        if (status.ok()) return true;
        failure = std::move(status);
        return false;
    }
    // v1 Send for a SysEx event: the fragment is assembled and only a complete
    // F0...F7 packet is submitted. A zero-length fragment is ignored (v1 DataLength == 0).
    virtual OutputStatus send_long(const output::SystemExclusiveEvent& fragment) = 0;
    // v1 Reset: discards queued and native work and any unfinished SysEx.
    virtual OutputStatus reset() = 0;
    // v1 Panic: CC120, CC123, CC64=0 on every channel (see panic_message).
    virtual OutputStatus panic() = 0;

    // Capability flags. SysEx is "disabled" when the selected provider cannot
    // take prepared long messages safely; send_long then fails with the same
    // text sysex_disabled_reason returns. A KDMAPI provider is optimistic
    // (true) until it has been loaded.
    virtual bool sysex_supported() const = 0;
    virtual std::string sysex_disabled_reason() const = 0;

    // v1 IMidiOutputContext.SourceFile (UTF-8), used in SysEx failure text.
    virtual void set_source_file(const std::string& path_utf8) = 0;

    // Which path this output actually uses, in one plain sentence for the status line, or empty when there is nothing to
    // say (DD-085: the direct WinMM output names its driver and whether that driver has been tested).
    virtual std::string path_description() const { return std::string(); }
    // DD-094: true once a reset or close of this output's driver did not return within its time limit; the output then
    // sends nothing more and the driver is not called again in this process. Safe to call from any thread.
    virtual bool driver_stopped_answering() const { return false; }

    // DD-110: optional. The calling thread is about to make many send_short calls in a row (a burst of events
    // that are due at once) and calls end_send_batch() right after the last one, whatever happens in between (use
    // SendBatch below). An output that guards every call against a concurrent close (the direct WinMM output) takes that
    // guard once for the whole batch instead of once per message, which is much cheaper on old processors. Same thread
    // for both calls; a batch is short (milliseconds) and never spans a wait. The default does nothing.
    virtual void begin_send_batch() {}
    virtual void end_send_batch() {}
};

// Brackets a run of send_short calls (see MidiOutput::begin_send_batch). A null output pointer does nothing.
class SendBatch {
public:
    explicit SendBatch(MidiOutput* output) : output_(output) { if (output_) output_->begin_send_batch(); }
    ~SendBatch() { if (output_) output_->end_send_batch(); }
    SendBatch(const SendBatch&) = delete;
    SendBatch& operator=(const SendBatch&) = delete;
private:
    MidiOutput* output_;
};

// ---------------------------------------------------------------- shared v1 rules

// v1 KdmApiMidiOutput.PackShortMessage: data[0] | data[1] << 8 | data[2] << 16,
// reading at most three bytes; 0 for an empty message.
uint32_t pack_short_message(const uint8_t* data, size_t length);

// v1 Panic order, shared by every backend: for each channel 0..15,
// CC120 (all sound off), CC123 (all notes off), CC64 = 0 (sustain off).
struct PanicMessage {
    uint32_t packed;
    const char* name;   // v1 WinMM error wording: "CC120", "CC123", "sustain-off"
};
const int kPanicMessageCount = 48;
PanicMessage panic_message(int index);   // 0 <= index < kPanicMessageCount

// v1 MidiOutputSafety.ResetAndSilence: Reset, then Panic even if Reset failed,
// so the controller messages cannot be discarded by the reset.
OutputStatus reset_and_silence(MidiOutput* output);

} // namespace platform
} // namespace midibn
