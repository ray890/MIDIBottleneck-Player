// WinMM MIDI output (port of v1 WindowsMidiOutput.cs). XP-compatible: only
// winmm, kernel32 and version APIs present on Windows XP SP3 are used.
#pragma once

#include "midibn/midi_output.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace midibn {
namespace win32 {

struct LongBuffer;

class WinMmMidiOutput : public platform::MidiOutput {
public:
    // The native entry points the backend calls after open (handles and
    // headers are passed as void*). The default table calls winmm; v1's
    // internal test constructor injected only the short-message sender, this
    // table also covers the long-message calls, reset and the error text so a
    // test can drive every path without touching a device.
    struct Api {
        std::function<uint32_t(void* handle, uint32_t message)> short_msg;
        std::function<uint32_t(void* handle, void* header, uint32_t size)> prepare_header;
        std::function<uint32_t(void* handle, void* header, uint32_t size)> long_msg;
        std::function<uint32_t(void* handle, void* header, uint32_t size)> unprepare_header;
        std::function<uint32_t(void* handle)> reset;
        std::function<std::wstring(uint32_t code)> error_text;
    };
    static Api native_api();

    WinMmMidiOutput();
    // v1 internal constructor WindowsMidiOutput(Func<IntPtr, uint, uint> sendShort): a deterministic
    // boundary. The output is "open" on a fake handle and never calls midiOutOpen/Close.
    explicit WinMmMidiOutput(const Api& boundary);
    ~WinMmMidiOutput() override;
    WinMmMidiOutput(const WinMmMidiOutput&) = delete;
    WinMmMidiOutput& operator=(const WinMmMidiOutput&) = delete;

    // v1 WindowsMidiOutput.GetDevices (midiOutGetNumDevs + midiOutGetDevCapsW only).
    static std::vector<platform::OutputDeviceInfo> enumerate_devices();
    // v1 IsPreparedLongMessageUnsafeDescription / IsCumulativeByteCountContract.
    static bool is_prepared_long_message_unsafe_description(const std::wstring& description);
    static bool is_cumulative_byte_count_contract(const std::vector<uint32_t>& results);
    // v1 GetLoadedModulePath: path of the loaded winmm.dll ("not loaded" / "unknown").
    static std::string loaded_module_path();
    // The error code reported when the readiness probes of an application-local provider never gave two accepted
    // results in a row within the time limit (register entry DD-010). v1 reported the code of the last probe; when
    // that one happened to be accepted (0) the open silently succeeded although the provider had never been ready.
    static uint32_t probe_failure_code(uint32_t lastResult, uint32_t lastNonZeroResult)
    { return lastResult != 0 ? lastResult : lastNonZeroResult; }

    const char* backend_name() const override { return "WinMM"; }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return enumerate_devices(); }
    platform::OutputStatus open(uint32_t deviceId) override;
    platform::OutputStatus close() override;
    bool is_open() const override { return handle_ != nullptr; }
    platform::OutputStatus send_short(uint32_t packed) override;
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override;
    platform::OutputStatus reset() override;
    platform::OutputStatus panic() override;
    bool sysex_supported() const override { return !preparedLongMessagesUnsafe_; }
    std::string sysex_disabled_reason() const override;
    void set_source_file(const std::string& path_utf8) override { sourceFile_ = path_utf8; }

    // State exposed for tests and diagnostics.
    bool uses_cumulative_byte_count_results() const { return usesCumulativeByteCountResults_; }
    const std::string& output_identity() const { return outputIdentity_; }
    size_t outstanding_long_messages() const { return longBuffers_.size(); }

private:
    platform::OutputStatus ensure_local_provider_ready();
    platform::OutputStatus send_long_packet(const output::SystemExclusivePacket& packet,
        const output::SystemExclusiveEvent& finalEvent);
    platform::OutputStatus long_error(uint32_t code, const char* operation, const output::SystemExclusivePacket& packet,
        const output::SystemExclusiveEvent& finalEvent, const void* headerCopy) const;
    platform::OutputStatus error_status(uint32_t code, const std::string& action) const;
    void reclaim_completed_long_messages();
    void reclaim_all_long_messages();
    void dispose_handle();
    std::string compute_output_identity() const;
    std::string unsafe_provider_message() const;

    Api api_;
    void* handle_ = nullptr;
    uint32_t deviceId_ = 0;
    std::string deviceName_;
    bool haveDeviceName_ = false;
    bool preparedLongMessagesUnsafe_ = false;
    bool usesCumulativeByteCountResults_ = false;
    bool testBoundary_ = false;
    std::string outputIdentity_;
    std::string sourceFile_;
    std::vector<LongBuffer*> longBuffers_;
    output::SystemExclusiveAssembler assembler_;
};

} // namespace win32
} // namespace midibn
