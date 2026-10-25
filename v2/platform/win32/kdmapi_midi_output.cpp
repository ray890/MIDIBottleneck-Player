// KDMAPI MIDI output: port of v1 src/KdmApiMidiOutput.cs (line references are
// to that file). The provider DLL is only loaded by DynamicKdmApiNative::load,
// which KdmApiMidiOutput::open (and create_with_provider) call; no other code
// in this file touches the provider.
#include "kdmapi_midi_output.h"

#include "win32_error_text.h"
#include "midi_long_buffer.h"
#include "midi_path_util.h"
#include "win32_platform.h"

#include <cstdio>
#include <cwctype>

namespace midibn {
namespace win32 {

using platform::OutputErrorKind;
using platform::OutputStatus;

namespace {

const wchar_t kProviderFileName[] = L"OmniMIDI.dll";

const char* architecture_name() { return sizeof(void*) == 8 ? "x64" : "x86"; }

std::wstring lower_text(std::wstring text)
{
    for (size_t i = 0; i < text.size(); ++i) text[i] = static_cast<wchar_t>(std::towlower(text[i]));
    return text;
}

// v1 GetLoadedModulePath(module, fallback) (lines 178-183).
std::wstring loaded_module_path(HMODULE module, const std::wstring& fallback)
{
    wchar_t buffer[1024];
    DWORD length = GetModuleFileNameW(module, buffer, 1024);
    return length == 0 ? fallback : std::wstring(buffer, length);
}

// Reads `count` bytes at `offset`; false on any short read.
bool read_exact(HANDLE file, int64_t offset, void* buffer, DWORD count)
{
    LARGE_INTEGER target;
    target.QuadPart = offset;
    if (!SetFilePointerEx(file, target, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(file, buffer, count, &read, nullptr) && read == count;
}

} // namespace

// ---------------------------------------------------------------- provider search

std::vector<std::wstring> kdmapi_registered_driver_folders(const std::vector<DriverEntry>& entries, const std::wstring& systemDirectory)
{
    std::vector<std::wstring> folders;
    std::wstring system = lower_text(full_path(systemDirectory));
    while (!system.empty() && system[system.size() - 1] == L'\\') system.erase(system.size() - 1);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].kind != DriverEntryKind::UserModeDriver) continue;
        std::wstring folder = directory_name(entries[i].path);
        while (!folder.empty() && folder[folder.size() - 1] == L'\\') folder.erase(folder.size() - 1);
        if (folder.empty()) continue;
        std::wstring key = lower_text(folder);
        if (key == system) continue;   // the system directory is searched last anyway
        bool seen = false;
        for (size_t k = 0; k < folders.size(); ++k)
            if (lower_text(folders[k]) == key) { seen = true; break; }
        if (!seen) folders.push_back(folder);
    }
    return folders;
}

std::vector<std::wstring> kdmapi_installed_driver_folders()
{
    std::string error;
    SystemFolders folders = system_folders();   // SysWOW64 / registry view of a 32-bit process on 64-bit Windows, as Direct mode
    std::vector<DriverEntry> entries = read_driver_entries(folders, error);
    return kdmapi_registered_driver_folders(entries, folders.directory);
}

// v1 ResolveProviderPath's search order (lines 143-146), existence only; v2 adds the registered driver folders.
std::vector<KdmApiProviderCandidate> kdmapi_provider_candidates()
{
    std::vector<KdmApiProviderCandidate> candidates;
    KdmApiProviderCandidate local;
    local.description = "application directory";
    std::wstring localPath = full_path(combine_path(application_directory(), kProviderFileName));
    local.path = wide_to_utf8(localPath);
    local.exists = file_exists(localPath);
    candidates.push_back(local);
    std::vector<std::wstring> folders = kdmapi_installed_driver_folders();
    for (size_t i = 0; i < folders.size(); ++i) {
        KdmApiProviderCandidate registered;
        registered.description = "registered driver folder (Drivers32)";
        std::wstring registeredPath = full_path(combine_path(folders[i], kProviderFileName));
        registered.path = wide_to_utf8(registeredPath);
        registered.exists = file_exists(registeredPath);
        candidates.push_back(registered);
    }
    KdmApiProviderCandidate installed;
    installed.description = "system directory";
    std::wstring installedPath = full_path(combine_path(system_directory(), kProviderFileName));
    installed.path = wide_to_utf8(installedPath);
    installed.exists = file_exists(installedPath);
    candidates.push_back(installed);
    return candidates;
}

// ---------------------------------------------------------------- DynamicKdmApiNative

// v1 ResolveProviderPath (lines 132-148).
OutputStatus DynamicKdmApiNative::resolve_provider_path(const std::wstring& explicitPath,
    const std::wstring& applicationDirectory, const std::wstring& systemDirectory, std::wstring& resolved)
{
    return resolve_provider_path(explicitPath, applicationDirectory, std::vector<std::wstring>(), systemDirectory, resolved);
}

OutputStatus DynamicKdmApiNative::resolve_provider_path(const std::wstring& explicitPath,
    const std::wstring& applicationDirectory, const std::vector<std::wstring>& registeredFolders,
    const std::wstring& systemDirectory, std::wstring& resolved)
{
    bool blank = true;   // String.IsNullOrWhiteSpace
    for (size_t i = 0; i < explicitPath.size(); ++i)
        if (!iswspace(explicitPath[i])) { blank = false; break; }
    if (!blank) {
        std::wstring requested = full_path(explicitPath);
        if (!file_exists(requested))
            return OutputStatus::failure(OutputErrorKind::FileNotFound, "The selected KDMAPI provider was not found.");
        resolved = requested;
        return OutputStatus::success();
    }
    std::wstring local = combine_path(applicationDirectory, kProviderFileName);
    if (file_exists(local)) { resolved = full_path(local); return OutputStatus::success(); }
    // Where the installed driver is registered (Drivers32), e.g. System32\OmniMIDI on Windows XP.
    for (size_t i = 0; i < registeredFolders.size(); ++i) {
        std::wstring registered = combine_path(registeredFolders[i], kProviderFileName);
        if (file_exists(registered)) { resolved = full_path(registered); return OutputStatus::success(); }
    }
    std::wstring installed = combine_path(systemDirectory, kProviderFileName);
    if (file_exists(installed)) { resolved = full_path(installed); return OutputStatus::success(); }
    return OutputStatus::failure(OutputErrorKind::FileNotFound,
        "No KDMAPI provider was found. Place a matching OmniMIDI.dll beside the application, install OmniMIDI, or turn off KDMAPI.");
}

// v1 ReadPeMachine (lines 150-166).
OutputStatus DynamicKdmApiNative::read_pe_machine(const std::wstring& path, uint16_t& machine)
{
    const std::string shown = wide_to_utf8(path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // v1 showed the raw .NET text here; v2 names the file and says what to do
        // (win32_error_text.h, register entry DD-003).
        return provider_file_open_error(GetLastError(), wide_to_utf8(full_path(path)));
    }
    LARGE_INTEGER size;
    size.QuadPart = 0;
    GetFileSizeEx(file, &size);
    OutputStatus status;
    // A read that comes up short (v1 showed the raw end-of-stream text;
    // unreachable in practice: both hold the file with read-only sharing).
    const OutputStatus endOfStream = OutputStatus::failure(OutputErrorKind::IoError,
        provider_file_truncated_message(wide_to_utf8(full_path(path))));
    uint16_t dosMagic = 0;
    if (size.QuadPart < 64) {
        status = OutputStatus::failure(OutputErrorKind::BadImageFormat,
            "The KDMAPI provider is not a valid Windows PE image: " + shown);
    } else if (!read_exact(file, 0, &dosMagic, 2)) {
        status = endOfStream;
    } else if (dosMagic != 0x5A4D) {
        status = OutputStatus::failure(OutputErrorKind::BadImageFormat,
            "The KDMAPI provider is not a valid Windows PE image: " + shown);
    } else {
        int32_t peOffset = 0;
        if (!read_exact(file, 0x3C, &peOffset, 4)) {
            status = endOfStream;
        } else if (peOffset < 0 || static_cast<int64_t>(peOffset) + 6 > size.QuadPart) {
            status = OutputStatus::failure(OutputErrorKind::BadImageFormat,
                "The KDMAPI provider has an invalid PE header: " + shown);
        } else {
            uint32_t signature = 0;
            if (!read_exact(file, peOffset, &signature, 4)) {
                status = endOfStream;
            } else if (signature != 0x00004550) {
                status = OutputStatus::failure(OutputErrorKind::BadImageFormat,
                    "The KDMAPI provider has an invalid PE signature: " + shown);
            } else if (!read_exact(file, static_cast<int64_t>(peOffset) + 4, &machine, 2)) {
                status = endOfStream;
            }
        }
    }
    CloseHandle(file);
    return status;
}

// v1 ValidateProviderArchitecture (lines 168-176).
OutputStatus DynamicKdmApiNative::validate_provider_architecture(const std::wstring& path)
{
    uint16_t actual = 0;
    OutputStatus status = read_pe_machine(path, actual);
    if (!status.ok()) return status;
    const uint16_t expected = sizeof(void*) == 8 ? 0x8664 : 0x014C;
    if (actual != expected) {
        char machine[16];
        std::snprintf(machine, sizeof machine, "%04X", static_cast<unsigned>(actual));
        return OutputStatus::failure(OutputErrorKind::BadImageFormat,
            std::string("The KDMAPI provider architecture does not match this ") + architecture_name() +
            " application: " + wide_to_utf8(path) + " (PE machine 0x" + machine + ").");
    }
    return OutputStatus::success();
}

// v1 DynamicKdmApiNative constructor (lines 58-115).
std::unique_ptr<DynamicKdmApiNative> DynamicKdmApiNative::load(const std::wstring& explicitPath, OutputStatus& status)
{
    // A deliberately colocated provider wins over the installed system
    // provider. Once selected, failure is reported for that exact file;
    // silently falling through to a different synthesizer would make the
    // user's selection misleading. No WinMM device is opened or used as a
    // brand/registration gate for this direct API.
    std::wstring modulePath;
    status = resolve_provider_path(explicitPath, application_directory(), kdmapi_installed_driver_folders(), system_directory(), modulePath);
    if (!status.ok()) return nullptr;
    status = validate_provider_architecture(modulePath);
    if (!status.ok()) return nullptr;
    HMODULE module = LoadLibraryW(modulePath.c_str());
    if (module == nullptr) {
        DWORD error = GetLastError();
        status = OutputStatus::failure(OutputErrorKind::Win32, std::string("The selected ") + architecture_name() +
            " KDMAPI provider could not be loaded: " + wide_to_utf8(modulePath), static_cast<int32_t>(error));
        return nullptr;
    }
    std::unique_ptr<DynamicKdmApiNative> native(new DynamicKdmApiNative());
    native->module_ = module;   // the destructor frees it on any failure below
    native->providerPath_ = wide_to_utf8(loaded_module_path(module, modulePath));

    struct Required { const char* name; FARPROC* slot; };
    FARPROC isAvailable = nullptr, initialize = nullptr, terminate = nullptr, reset = nullptr, sendShort = nullptr;
    const Required required[] = {
        { "IsKDMAPIAvailable", &isAvailable }, { "InitializeKDMAPIStream", &initialize },
        { "TerminateKDMAPIStream", &terminate }, { "ResetKDMAPIStream", &reset }, { "SendDirectData", &sendShort }
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        *required[i].slot = GetProcAddress(module, required[i].name);
        if (*required[i].slot == nullptr) {
            status = OutputStatus::failure(OutputErrorKind::EntryPointNotFound,
                std::string("The KDMAPI provider does not export the required function ") + required[i].name + ": " +
                native->providerPath_);
            return nullptr;
        }
    }
    native->isAvailable_ = reinterpret_cast<BoolCall>(isAvailable);
    native->initialize_ = reinterpret_cast<BoolCall>(initialize);
    native->terminate_ = reinterpret_cast<BoolCall>(terminate);
    native->reset_ = reinterpret_cast<VoidCall>(reset);
    native->sendShort_ = reinterpret_cast<ShortMessageCall>(sendShort);
    native->prepareLong_ = reinterpret_cast<LongMessageCall>(GetProcAddress(module, "PrepareLongData"));
    native->sendLong_ = reinterpret_cast<LongMessageCall>(GetProcAddress(module, "SendDirectLongData"));
    native->unprepareLong_ = reinterpret_cast<LongMessageCall>(GetProcAddress(module, "UnprepareLongData"));
    int longExportCount = (native->prepareLong_ ? 1 : 0) + (native->sendLong_ ? 1 : 0) + (native->unprepareLong_ ? 1 : 0);
    if (longExportCount == 3) native->longMessageStatus_ = "prepared long-message exports available";
    else {
        std::string missing;
        const char* separator = "";
        if (!native->prepareLong_) { missing += "PrepareLongData"; separator = ", "; }
        if (!native->sendLong_) { missing += separator; missing += "SendDirectLongData"; separator = ", "; }
        if (!native->unprepareLong_) { missing += separator; missing += "UnprepareLongData"; }
        native->longMessageStatus_ = "prepared long-message contract incomplete; missing " + missing;
    }
    native->returnVersion_ = reinterpret_cast<VersionCall>(GetProcAddress(module, "ReturnKDMAPIVer"));
    if (native->returnVersion_ == nullptr) native->version_ = "not reported";
    else {
        unsigned long major = 0, minor = 0, build = 0, revision = 0;
        if (!native->returnVersion_(&major, &minor, &build, &revision)) {
            status = OutputStatus::failure(OutputErrorKind::InvalidOperation,
                "The KDMAPI provider rejected its version query: " + native->providerPath_);
            return nullptr;
        }
        native->version_ = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build) + "." +
            std::to_string(revision);
    }
    status = OutputStatus::success();
    return native;
}

// v1 Dispose (lines 198-217).
DynamicKdmApiNative::~DynamicKdmApiNative()
{
    if (module_ != nullptr) FreeLibrary(static_cast<HMODULE>(module_));
}

bool DynamicKdmApiNative::is_available() { return isAvailable_() != 0; }
bool DynamicKdmApiNative::initialize_stream() { return initialize_() != 0; }
bool DynamicKdmApiNative::terminate_stream() { return terminate_() != 0; }
void DynamicKdmApiNative::reset_stream() { reset_(); }
void DynamicKdmApiNative::send_short(uint32_t message) { sendShort_(message); }
uint32_t DynamicKdmApiNative::prepare_long(void* header, uint32_t headerSize) { return prepareLong_(header, headerSize); }
uint32_t DynamicKdmApiNative::send_long(void* header, uint32_t headerSize) { return sendLong_(header, headerSize); }
uint32_t DynamicKdmApiNative::unprepare_long(void* header, uint32_t headerSize) { return unprepareLong_(header, headerSize); }

// ---------------------------------------------------------------- KdmApiMidiOutput

KdmApiMidiOutput::KdmApiMidiOutput() {}                                       // v1 lines 249-252
KdmApiMidiOutput::KdmApiMidiOutput(KdmApiNative* borrowed) : native_(borrowed) {}   // v1 lines 260-270, ownsNative false
KdmApiMidiOutput::KdmApiMidiOutput(std::unique_ptr<KdmApiNative> owned) : native_(owned.get()), owned_(std::move(owned)) {}

// v1 lines 254-258.
std::unique_ptr<KdmApiMidiOutput> KdmApiMidiOutput::create_with_provider(const std::wstring& providerPath, OutputStatus& status)
{
    std::unique_ptr<DynamicKdmApiNative> native = DynamicKdmApiNative::load(providerPath, status);
    if (!native) return nullptr;
    return std::unique_ptr<KdmApiMidiOutput>(new KdmApiMidiOutput(std::unique_ptr<KdmApiNative>(std::move(native))));
}

KdmApiMidiOutput::~KdmApiMidiOutput()   // v1 Dispose (lines 467-472)
{
    close_stream(false);
    release_owned_native();
}

// v1 ReleaseOwnedNative (lines 474-481).
void KdmApiMidiOutput::release_owned_native()
{
    if (owned_) {
        owned_.reset();
        native_ = nullptr;
    }
}

// v1 Open (lines 272-294). `deviceId` is ignored: KDMAPI has a single stream.
OutputStatus KdmApiMidiOutput::open(uint32_t)
{
    OutputStatus status = close_stream(true);
    if (!status.ok()) return status;
    if (native_ == nullptr) {
        std::unique_ptr<DynamicKdmApiNative> loaded = DynamicKdmApiNative::load(std::wstring(), status);
        if (!loaded) return status;
        owned_ = std::unique_ptr<KdmApiNative>(std::move(loaded));
        native_ = owned_.get();
    }
    if (!native_->is_available())
        status = OutputStatus::failure(OutputErrorKind::InvalidOperation,
            "The KDMAPI provider reported that direct output is unavailable: " + native_->provider_path());
    else if (!native_->initialize_stream())
        status = OutputStatus::failure(OutputErrorKind::InvalidOperation,
            "The KDMAPI provider rejected stream initialization (KDMAPI " + native_->version() + "): " +
            native_->provider_path());
    if (!status.ok()) {
        release_owned_native();
        return status;
    }
    open_ = true;
    return OutputStatus::success();
}

// v1 CloseStream (lines 424-449). v1 also catches exceptions thrown by the
// native reset; native calls cannot throw here, so that branch has no equivalent.
OutputStatus KdmApiMidiOutput::close_stream(bool reportFailure)
{
    if (!open_) return OutputStatus::success();
    native_->reset_stream();
    reclaim_all_long_messages();
    bool terminated = native_->terminate_stream();
    assembler_.reset();
    open_ = false;
    if (reportFailure && !terminated)
        return OutputStatus::failure(OutputErrorKind::InvalidOperation,
            "The KDMAPI provider rejected stream termination: " + native_->provider_path());
    return OutputStatus::success();
}

// v1 Close (lines 451-463): a rejected termination must not leave the module
// loaded for the next output transition.
OutputStatus KdmApiMidiOutput::close()
{
    OutputStatus status = close_stream(true);
    release_owned_native();
    return status;
}

// v1 Send, non-SysEx branch (lines 296-308).
OutputStatus KdmApiMidiOutput::send_short(uint32_t packed)
{
    OutputStatus failure;
    try_send_short(packed, failure);
    return failure;
}

bool KdmApiMidiOutput::try_send_short(uint32_t packed, OutputStatus& failure)
{
    if (!open_) {
        failure = OutputStatus::failure(OutputErrorKind::InvalidOperation, "The KDMAPI output is not open.");
        return false;
    }
    reclaim_completed_long_messages();
    native_->send_short(packed);
    return true;
}

// v1 Send, SysEx branch (lines 296-306).
OutputStatus KdmApiMidiOutput::send_long(const output::SystemExclusiveEvent& fragment)
{
    if (!open_) return OutputStatus::failure(OutputErrorKind::InvalidOperation, "The KDMAPI output is not open.");
    if (fragment.length == 0) return OutputStatus::success();
    reclaim_completed_long_messages();
    output::SystemExclusivePacket packet;
    if (assembler_.accept(fragment, packet)) return send_long_packet(packet, fragment);
    return OutputStatus::success();
}

// v1 Panic (lines 319-329); order from panic_message().
OutputStatus KdmApiMidiOutput::panic()
{
    if (!open_) return OutputStatus::success();
    for (int i = 0; i < platform::kPanicMessageCount; ++i) native_->send_short(platform::panic_message(i).packed);
    return OutputStatus::success();
}

// v1 Reset (lines 331-337).
OutputStatus KdmApiMidiOutput::reset()
{
    assembler_.reset();
    if (!open_) return OutputStatus::success();
    native_->reset_stream();
    reclaim_all_long_messages();
    return OutputStatus::success();
}

// v1 SendLongPacket's NotSupportedException text (lines 341-344).
std::string KdmApiMidiOutput::unsupported_long_message() const
{
    return "The selected KDMAPI provider can send short MIDI messages but does not export "
        "PrepareLongData, SendDirectLongData, and UnprepareLongData. System Exclusive playback requires that prepared "
        "long-message contract (" + native_->long_message_status() + "). Provider: " + native_->provider_path();
}

std::string KdmApiMidiOutput::sysex_disabled_reason() const
{
    return (native_ != nullptr && !native_->supports_long_messages()) ? unsupported_long_message() : std::string();
}

// v1 ThrowLongIfError (lines 415-422): no native error text for KDMAPI.
OutputStatus KdmApiMidiOutput::long_error(uint32_t code, const char* operation,
    const output::SystemExclusivePacket& packet, const output::SystemExclusiveEvent& finalEvent,
    const void* headerCopy) const
{
    if (code == 0) return OutputStatus::success();
    const MIDIHDR& header = *static_cast<const MIDIHDR*>(headerCopy);
    std::string detail = output::describe_sysex_failure(operation, code, std::string(), sourceFile_, &packet, finalEvent,
        static_cast<int32_t>(sizeof(MIDIHDR)), header.dwBufferLength, header.dwBytesRecorded, header.dwFlags,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(header.lpData)), static_cast<int>(sizeof(void*)));
    return OutputStatus::failure(OutputErrorKind::Win32, detail, static_cast<int32_t>(code));
}

// v1 SendLongPacket (lines 339-371).
OutputStatus KdmApiMidiOutput::send_long_packet(const output::SystemExclusivePacket& packet,
    const output::SystemExclusiveEvent& finalEvent)
{
    if (!native_->supports_long_messages())
        return OutputStatus::failure(OutputErrorKind::NotSupported, unsupported_long_message());
    LongBuffer* buffer = allocate_long_buffer(packet.bytes.data(), packet.bytes.size());
    if (buffer == nullptr)
        return OutputStatus::failure(OutputErrorKind::OutOfMemory, out_of_memory_for_sysex_message());
    const uint32_t headerSize = static_cast<uint32_t>(sizeof(MIDIHDR));
    MIDIHDR headerCopy = *buffer->header;
    OutputStatus status = long_error(native_->prepare_long(buffer->header, headerSize), "PrepareLongData", packet,
        finalEvent, &headerCopy);
    if (!status.ok()) {
        free_long_buffer(buffer);
        return status;
    }
    buffer->prepared = true;
    uint32_t result = native_->send_long(buffer->header, headerSize);
    if (result != 0) {
        native_->unprepare_long(buffer->header, headerSize);
        buffer->prepared = false;
        status = long_error(result, "SendDirectLongData", packet, finalEvent, &headerCopy);
        free_long_buffer(buffer);
        return status;
    }
    longBuffers_.push_back(buffer);
    return OutputStatus::success();
}

// v1 ReclaimCompletedLongMessages (lines 373-386).
void KdmApiMidiOutput::reclaim_completed_long_messages()
{
    if (longBuffers_.empty()) return;
    const uint32_t headerSize = static_cast<uint32_t>(sizeof(MIDIHDR));
    for (size_t i = longBuffers_.size(); i-- > 0;) {
        if (!long_buffer_done(*longBuffers_[i])) continue;
        native_->unprepare_long(longBuffers_[i]->header, headerSize);
        longBuffers_[i]->prepared = false;
        free_long_buffer(longBuffers_[i]);
        longBuffers_.erase(longBuffers_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// v1 ReclaimAllLongMessages (lines 388-399).
void KdmApiMidiOutput::reclaim_all_long_messages()
{
    if (longBuffers_.empty()) return;
    const uint32_t headerSize = static_cast<uint32_t>(sizeof(MIDIHDR));
    for (size_t i = longBuffers_.size(); i-- > 0;) {
        if (longBuffers_[i]->prepared) native_->unprepare_long(longBuffers_[i]->header, headerSize);
        free_long_buffer(longBuffers_[i]);
    }
    longBuffers_.clear();
}

} // namespace win32
} // namespace midibn
