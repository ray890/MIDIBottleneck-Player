// KDMAPI MIDI output (port of v1 KdmApiMidiOutput.cs): OmniMIDI.dll is loaded
// at run time with LoadLibrary, from the application directory first and then
// System32, after a PE-architecture check. Nothing is loaded until open().
// XP-compatible: only kernel32 APIs present on Windows XP SP3 are used.
#pragma once

#include "midi_driver_discovery.h"
#include "midibn/midi_output.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace win32 {

struct LongBuffer;

// v1 IKdmApiNative: the provider's entry points (headers are MIDIHDR pointers
// passed as void*). The dynamic implementation loads the DLL; tests supply a
// fake so no provider is ever loaded.
class KdmApiNative {
public:
    virtual ~KdmApiNative() {}
    virtual bool is_available() = 0;
    virtual bool initialize_stream() = 0;
    virtual bool terminate_stream() = 0;
    virtual void reset_stream() = 0;
    virtual void send_short(uint32_t message) = 0;
    virtual uint32_t prepare_long(void* header, uint32_t headerSize) = 0;
    virtual uint32_t send_long(void* header, uint32_t headerSize) = 0;
    virtual uint32_t unprepare_long(void* header, uint32_t headerSize) = 0;
    virtual std::string version() const = 0;
    virtual std::string provider_path() const = 0;
    virtual bool supports_long_messages() const = 0;
    virtual std::string long_message_status() const = 0;
};

// Where v1 ResolveProviderPath looks (file existence only; nothing is loaded).
struct KdmApiProviderCandidate {
    std::string description;   // "application directory" / "registered driver folder" / "system directory"
    std::string path;          // UTF-8
    bool exists = false;
};
// Search order: the application directory, then the folder of each user-mode MIDI driver registered under
// Drivers32 (where an installed OmniMIDI lives, e.g. System32\OmniMIDI; the same registry view and system-folder
// resolution as Direct mode), then the system directory.
std::vector<KdmApiProviderCandidate> kdmapi_provider_candidates();
// The folders (registry order, no duplicates, the system directory itself left out because it is searched last) that
// hold the user-mode MIDI drivers listed in `entries`. Pure.
std::vector<std::wstring> kdmapi_registered_driver_folders(const std::vector<DriverEntry>& entries, const std::wstring& systemDirectory);
// The same for this PC's Drivers32 list (read only; no driver file is opened).
std::vector<std::wstring> kdmapi_installed_driver_folders();

// v1 DynamicKdmApiNative static helpers (ported for direct testing).
class DynamicKdmApiNative : public KdmApiNative {
public:
    // v1 constructor DynamicKdmApiNative(string explicitProviderPath): resolve,
    // check the PE architecture, LoadLibrary, bind the exports, query the
    // version. `explicitPath` empty means search. Returns null and sets
    // `status` on failure. THIS LOADS THE PROVIDER DLL.
    static std::unique_ptr<DynamicKdmApiNative> load(const std::wstring& explicitPath, platform::OutputStatus& status);

    // v1 ResolveProviderPath (explicit path, else application directory, else system directory).
    static platform::OutputStatus resolve_provider_path(const std::wstring& explicitPath,
        const std::wstring& applicationDirectory, const std::wstring& systemDirectory, std::wstring& resolved);
    // The same with the registered-driver folders searched between the application and the system directory.
    static platform::OutputStatus resolve_provider_path(const std::wstring& explicitPath,
        const std::wstring& applicationDirectory, const std::vector<std::wstring>& registeredFolders,
        const std::wstring& systemDirectory, std::wstring& resolved);
    // v1 ReadPeMachine: opens the file and reads its IMAGE_FILE_HEADER Machine
    // field (the file is read, never loaded).
    static platform::OutputStatus read_pe_machine(const std::wstring& path, uint16_t& machine);
    // v1 ValidateProviderArchitecture against this process (x64 0x8664, x86 0x014C).
    static platform::OutputStatus validate_provider_architecture(const std::wstring& path);

    ~DynamicKdmApiNative() override;
    bool is_available() override;
    bool initialize_stream() override;
    bool terminate_stream() override;
    void reset_stream() override;
    void send_short(uint32_t message) override;
    uint32_t prepare_long(void* header, uint32_t headerSize) override;
    uint32_t send_long(void* header, uint32_t headerSize) override;
    uint32_t unprepare_long(void* header, uint32_t headerSize) override;
    std::string version() const override { return version_; }
    std::string provider_path() const override { return providerPath_; }
    bool supports_long_messages() const override { return prepareLong_ && sendLong_ && unprepareLong_; }
    std::string long_message_status() const override { return longMessageStatus_; }

private:
    DynamicKdmApiNative() {}
    typedef int(__stdcall* BoolCall)();
    typedef void(__stdcall* VoidCall)();
    typedef void(__stdcall* ShortMessageCall)(unsigned long message);
    typedef unsigned long(__stdcall* LongMessageCall)(void* header, unsigned long headerSize);
    typedef int(__stdcall* VersionCall)(unsigned long* major, unsigned long* minor, unsigned long* build,
        unsigned long* revision);

    void* module_ = nullptr;
    BoolCall isAvailable_ = nullptr;
    BoolCall initialize_ = nullptr;
    BoolCall terminate_ = nullptr;
    VoidCall reset_ = nullptr;
    ShortMessageCall sendShort_ = nullptr;
    LongMessageCall prepareLong_ = nullptr;
    LongMessageCall sendLong_ = nullptr;
    LongMessageCall unprepareLong_ = nullptr;
    VersionCall returnVersion_ = nullptr;
    std::string version_;
    std::string longMessageStatus_;
    std::string providerPath_;
};

class KdmApiMidiOutput : public platform::MidiOutput {
public:
    // v1 KdmApiMidiOutput(): the provider is resolved and loaded by open().
    KdmApiMidiOutput();
    // v1 KdmApiMidiOutput(IKdmApiNative native, bool ownsNative): a borrowed native
    // survives close(); an owned one is released by close(), as v1 does.
    explicit KdmApiMidiOutput(KdmApiNative* borrowed);
    explicit KdmApiMidiOutput(std::unique_ptr<KdmApiNative> owned);
    // v1 KdmApiMidiOutput(string providerPath): loads immediately. THIS LOADS THE PROVIDER DLL.
    static std::unique_ptr<KdmApiMidiOutput> create_with_provider(const std::wstring& providerPath,
        platform::OutputStatus& status);
    ~KdmApiMidiOutput() override;
    KdmApiMidiOutput(const KdmApiMidiOutput&) = delete;
    KdmApiMidiOutput& operator=(const KdmApiMidiOutput&) = delete;

    const char* backend_name() const override { return "KDMAPI"; }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return std::vector<platform::OutputDeviceInfo>(); }
    platform::OutputStatus open(uint32_t deviceId) override;
    platform::OutputStatus close() override;
    bool is_open() const override { return open_; }
    platform::OutputStatus send_short(uint32_t packed) override;
    bool try_send_short(uint32_t packed, platform::OutputStatus& failure) override;   // no status object on success
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override;
    platform::OutputStatus reset() override;
    platform::OutputStatus panic() override;
    bool sysex_supported() const override { return native_ == nullptr || native_->supports_long_messages(); }
    std::string sysex_disabled_reason() const override;
    void set_source_file(const std::string& path_utf8) override { sourceFile_ = path_utf8; }

    // v1 ProviderPath (empty before a provider is loaded).
    std::string provider_path() const { return native_ ? native_->provider_path() : std::string(); }
    size_t outstanding_long_messages() const { return longBuffers_.size(); }

private:
    platform::OutputStatus close_stream(bool reportFailure);
    platform::OutputStatus send_long_packet(const output::SystemExclusivePacket& packet,
        const output::SystemExclusiveEvent& finalEvent);
    platform::OutputStatus long_error(uint32_t code, const char* operation, const output::SystemExclusivePacket& packet,
        const output::SystemExclusiveEvent& finalEvent, const void* headerCopy) const;
    void reclaim_completed_long_messages();
    void reclaim_all_long_messages();
    void release_owned_native();
    std::string unsupported_long_message() const;

    KdmApiNative* native_ = nullptr;
    std::unique_ptr<KdmApiNative> owned_;
    bool open_ = false;
    std::string sourceFile_;
    std::vector<LongBuffer*> longBuffers_;
    output::SystemExclusiveAssembler assembler_;
};

} // namespace win32
} // namespace midibn
