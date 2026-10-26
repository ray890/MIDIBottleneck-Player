// WinMM MIDI output: port of v1 src/WindowsMidiOutput.cs. Line references are
// to that file. Nothing in this file runs at load time; midiOutOpen,
// midiOutShortMsg, midiOutLongMsg and midiOutReset are only reached through
// open/send/reset/panic/close.
#include "winmm_midi_output.h"

#include "win32_error_text.h"
#include "midi_long_buffer.h"
#include "midi_path_util.h"
#include "win32_platform.h"

#include <cstdio>
#include <cstring>

namespace midibn {
namespace win32 {

using platform::OutputDeviceInfo;
using platform::OutputErrorKind;
using platform::OutputStatus;

namespace {

// v1 GetLoadedModulePath (lines 217-224), wide form.
std::wstring loaded_module_path_wide()
{
    HMODULE module = GetModuleHandleW(L"winmm.dll");
    if (module == nullptr) return L"not loaded";
    wchar_t buffer[1024];
    DWORD length = GetModuleFileNameW(module, buffer, 1024);
    return length == 0 ? std::wstring(L"unknown") : std::wstring(buffer, length);
}

// The FileDescription of the file's version resource, which tells a KDMAPI/syndrv
// wrapper from a real winmm: the file's first listed language, then the US-English
// and language-neutral blocks as fallbacks (the order is a choice, not a contract:
// only the description text matters). Empty when there is none.
std::wstring file_description(const std::wstring& path)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return std::wstring();
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return std::wstring();
    std::vector<DWORD> candidates;
    struct Translation { WORD language; WORD codePage; };
    void* table = nullptr;
    UINT tableLength = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &table, &tableLength) && tableLength >= sizeof(Translation)) {
        const Translation* first = static_cast<const Translation*>(table);
        candidates.push_back((static_cast<DWORD>(first->language) << 16) | first->codePage);
    }
    candidates.push_back(0x040904B0);
    candidates.push_back(0x040904E4);
    candidates.push_back(0x04090000);
    for (size_t i = 0; i < candidates.size(); ++i) {
        wchar_t block[96];
        swprintf_s(block, L"\\StringFileInfo\\%04x%04x\\FileDescription", candidates[i] >> 16, candidates[i] & 0xFFFF);
        void* text = nullptr;
        UINT textLength = 0;
        if (VerQueryValueW(data.data(), block, &text, &textLength) && text != nullptr)
            return std::wstring(static_cast<const wchar_t*>(text));
    }
    return std::wstring();
}

std::string hex_handle(void* handle)
{
    // v1 _handle.ToInt64().ToString("X"): IntPtr.ToInt64 sign-extends on a 32-bit process.
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%llX", static_cast<unsigned long long>(
        static_cast<int64_t>(reinterpret_cast<intptr_t>(handle))));
    return buffer;
}

uint32_t native_short(void* handle, uint32_t message)
{ return midiOutShortMsg(static_cast<HMIDIOUT>(handle), message); }
uint32_t native_prepare(void* handle, void* header, uint32_t size)
{ return midiOutPrepareHeader(static_cast<HMIDIOUT>(handle), static_cast<LPMIDIHDR>(header), size); }
uint32_t native_long(void* handle, void* header, uint32_t size)
{ return midiOutLongMsg(static_cast<HMIDIOUT>(handle), static_cast<LPMIDIHDR>(header), size); }
uint32_t native_unprepare(void* handle, void* header, uint32_t size)
{ return midiOutUnprepareHeader(static_cast<HMIDIOUT>(handle), static_cast<LPMIDIHDR>(header), size); }
uint32_t native_reset(void* handle)
{ return midiOutReset(static_cast<HMIDIOUT>(handle)); }
std::wstring native_error_text(uint32_t code)
{
    wchar_t text[256] = {};
    midiOutGetErrorTextW(code, text, 256);   // v1 StringBuilder(256)
    return std::wstring(text);
}

} // namespace

WinMmMidiOutput::Api WinMmMidiOutput::native_api()
{
    Api api;
    api.short_msg = native_short;
    api.prepare_header = native_prepare;
    api.long_msg = native_long;
    api.unprepare_header = native_unprepare;
    api.reset = native_reset;
    api.error_text = native_error_text;
    return api;
}

// v1 line 57.
WinMmMidiOutput::WinMmMidiOutput() : api_(native_api()) {}

// v1 lines 61-68.
WinMmMidiOutput::WinMmMidiOutput(const Api& boundary) : api_(boundary)
{
    testBoundary_ = true;
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(1));
    deviceName_ = "Deterministic WinMM boundary";
    haveDeviceName_ = true;
    outputIdentity_ = compute_output_identity();
}

WinMmMidiOutput::~WinMmMidiOutput() { dispose_handle(); }   // v1 Dispose

// v1 GetDevices (lines 70-87).
std::vector<OutputDeviceInfo> WinMmMidiOutput::enumerate_devices()
{
    std::vector<OutputDeviceInfo> devices;
    UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps;
        std::memset(&caps, 0, sizeof caps);
        MMRESULT result = midiOutGetDevCapsW(i, &caps, sizeof caps);
        if (result != MMSYSERR_NOERROR) continue;
        OutputDeviceInfo info;
        info.device_id = i;
        size_t nameLength = 0;
        while (nameLength < MAXPNAMELEN && caps.szPname[nameLength] != L'\0') ++nameLength;
        info.name = nameLength == 0 ? "MIDI output " + std::to_string(i)
                                    : wide_to_utf8(std::wstring(caps.szPname, nameLength));
        info.manufacturer_id = caps.wMid;
        info.product_id = caps.wPid;
        info.driver_version = caps.vDriverVersion;
        info.technology = caps.wTechnology;
        info.voices = caps.wVoices;
        info.notes = caps.wNotes;
        info.channel_mask = caps.wChannelMask;
        info.support = caps.dwSupport;
        devices.push_back(info);
    }
    return devices;
}

// v1 lines 153-156.
bool WinMmMidiOutput::is_prepared_long_message_unsafe_description(const std::wstring& description)
{
    return _wcsicmp(description.c_str(), L"WinMM to KDMAPI or syndrv") == 0;
}

// v1 lines 158-162.
bool WinMmMidiOutput::is_cumulative_byte_count_contract(const std::vector<uint32_t>& results)
{
    if (results.size() < 4) return false;
    return results[0] == 0 && results[1] == 3 && results[2] == 6 && results[3] == 9;
}

std::string WinMmMidiOutput::loaded_module_path() { return wide_to_utf8(loaded_module_path_wide()); }

// v1 ThrowIfError (lines 320-326): returns success for code 0.
OutputStatus WinMmMidiOutput::error_status(uint32_t code, const std::string& action) const
{
    if (code == 0) return OutputStatus::success();
    std::string text = wide_to_utf8(api_.error_text(code));
    return OutputStatus::failure(OutputErrorKind::Win32, "MIDI error while " + action + " (native code " +
        std::to_string(code) + "): " + text, static_cast<int32_t>(code));
}

// v1 ThrowLongIfError (lines 328-337). `headerCopy` is the header as built
// before prepare (v1 passes its local copy, so dwFlags prints as zero).
OutputStatus WinMmMidiOutput::long_error(uint32_t code, const char* operation,
    const output::SystemExclusivePacket& packet, const output::SystemExclusiveEvent& finalEvent,
    const void* headerCopy) const
{
    if (code == 0) return OutputStatus::success();
    const MIDIHDR& header = *static_cast<const MIDIHDR*>(headerCopy);
    std::string nativeText = wide_to_utf8(api_.error_text(code));
    std::string detail = output::describe_sysex_failure(operation, code, nativeText, sourceFile_, &packet, finalEvent,
        static_cast<int32_t>(sizeof(MIDIHDR)), header.dwBufferLength, header.dwBytesRecorded, header.dwFlags,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(header.lpData)), static_cast<int>(sizeof(void*)));
    return OutputStatus::failure(OutputErrorKind::Win32, detail, static_cast<int32_t>(code));
}

// v1 OutputIdentity (lines 203-207).
std::string WinMmMidiOutput::compute_output_identity() const
{
    return (haveDeviceName_ ? deviceName_ : std::string("Windows MIDI output")) + " (device " + std::to_string(deviceId_) +
        ", handle 0x" + hex_handle(handle_) + ", module " + loaded_module_path() + ")";
}

// v1 SendLong's NotSupportedException text (lines 247-251).
std::string WinMmMidiOutput::unsafe_provider_message() const
{
    return "The selected application-local WinMM provider does not expose a safely verifiable "
        "standard prepared-long-message path. System Exclusive playback was stopped instead of risking the native "
        "midiOutLongMsg failure reproduced with this provider. Module: " + loaded_module_path();
}

std::string WinMmMidiOutput::sysex_disabled_reason() const
{
    return preparedLongMessagesUnsafe_ ? unsafe_provider_message() : std::string();
}

// v1 Open (lines 89-104).
OutputStatus WinMmMidiOutput::open(uint32_t deviceId)
{
    dispose_handle();
    // v1 GetDeviceName (lines 209-215): re-enumerates; falls back to "MIDI output <id>".
    std::string deviceName = "MIDI output " + std::to_string(deviceId);
    std::vector<OutputDeviceInfo> devices = enumerate_devices();
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i].device_id == deviceId) { deviceName = devices[i].name; break; }

    HMIDIOUT handle = nullptr;
    MMRESULT result = midiOutOpen(&handle, deviceId, 0, 0, CALLBACK_NULL);
    OutputStatus status = error_status(result, "opening MIDI output " + deviceName + " (device " +
        std::to_string(deviceId) + ")");
    if (!status.ok()) return status;
    handle_ = handle;
    deviceId_ = deviceId;
    deviceName_ = deviceName;
    haveDeviceName_ = true;
    outputIdentity_ = compute_output_identity();
    status = ensure_local_provider_ready();
    if (!status.ok()) {
        dispose_handle();
        return status;
    }
    // A USB MIDI device that was plugged in a moment ago can refuse every message of its first session with
    // MMSYSERR_ERROR, while the next session works (seen with a DreamBlaster X2 and a CH345 "USB2.0-MIDI" on
    // Windows 10). One harmless Sustain Off on channel 1 tells; when it is refused that way the device is
    // closed and opened once more. A second refusal is reported like any failed open.
    const uint32_t sustainOffChannelOne = 0x0040B0;
    if (!usesCumulativeByteCountResults_ && api_.short_msg(handle_, sustainOffChannelOne) == MMSYSERR_ERROR) {
        midiOutClose(static_cast<HMIDIOUT>(handle_));
        handle_ = nullptr;
        Sleep(50);
        handle = nullptr;
        result = midiOutOpen(&handle, deviceId, 0, 0, CALLBACK_NULL);
        status = error_status(result, "opening MIDI output " + deviceName + " (device " + std::to_string(deviceId) +
            ") a second time, after it refused a first harmless message");
        if (!status.ok()) {
            deviceName_.clear();
            haveDeviceName_ = false;
            outputIdentity_.clear();
            return status;
        }
        handle_ = handle;
        const uint32_t again = api_.short_msg(handle_, sustainOffChannelOne);
        if (again != 0) {
            status = error_status(again, "sending a first harmless message (Sustain Off, channel 1) to " + outputIdentity_ +
                " after opening it a second time");
            dispose_handle();
            return status;
        }
    }
    return OutputStatus::success();
}

// v1 EnsureLocalProviderReady (lines 106-151).
OutputStatus WinMmMidiOutput::ensure_local_provider_ready()
{
    preparedLongMessagesUnsafe_ = false;
    usesCumulativeByteCountResults_ = false;
    std::wstring modulePath = loaded_module_path_wide();
    // DD-043: when Windows cannot say which winmm.dll is loaded there is no application-local provider to check.
    // (v1 treated the placeholder text "not loaded" / "unknown" as a file name beside the current directory, so the
    // provider checks could run against nothing whenever the current directory was the application directory.)
    if (modulePath == L"not loaded" || modulePath == L"unknown") return OutputStatus::success();
    // The directory of the loaded module compared with the application directory (without its trailing separator,
    // ignoring case).
    std::wstring moduleDirectory = directory_name(full_path(modulePath));
    std::wstring baseDirectory = full_path(application_directory());
    while (!baseDirectory.empty() && baseDirectory[baseDirectory.size() - 1] == L'\\') baseDirectory.erase(baseDirectory.size() - 1);
    if (_wcsicmp(moduleDirectory.c_str(), baseDirectory.c_str()) != 0) return OutputStatus::success();

    preparedLongMessagesUnsafe_ = is_prepared_long_message_unsafe_description(file_description(modulePath));

    // Some application-local wrappers return from midiOutOpen before their
    // synth worker is ready. Require two consecutive standard MMSYSERR_NOERROR
    // results from harmless sustain-off probes. No nonzero result is ever
    // accepted for real song data.
    const uint32_t sustainOffChannelOne = 0x0040B0;
    std::vector<uint32_t> observed;
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    int consecutiveSuccesses = 0;
    uint32_t last = 0;
    uint32_t lastNonZero = 0;
    do {
        last = api_.short_msg(handle_, sustainOffChannelOne);
        if (last != 0) lastNonZero = last;
        if (observed.size() < 16) observed.push_back(last);
        if (observed.size() >= 4 && is_cumulative_byte_count_contract(observed)) {
            usesCumulativeByteCountResults_ = true;
            return OutputStatus::success();
        }
        if (last == 0) {
            ++consecutiveSuccesses;
            if (consecutiveSuccesses >= 2) return OutputStatus::success();
        } else consecutiveSuccesses = 0;
        Sleep(10);
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart < 5000);
    std::string joined;
    for (size_t i = 0; i < observed.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += std::to_string(observed[i]);
    }
    // The probes never gave two accepted results in a row: the open fails (v1 passed the code of the last probe, and
    // when that one was 0 the open succeeded; register entry DD-010).
    return error_status(probe_failure_code(last, lastNonZero), "waiting for two accepted initialization probes from " + compute_output_identity() +
        "; first returns " + joined);
}

// v1 Send, non-SysEx branch (lines 164-182).
OutputStatus WinMmMidiOutput::send_short(uint32_t packed)
{
    if (handle_ == nullptr)
        return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    reclaim_completed_long_messages();
    uint32_t result = api_.short_msg(handle_, packed);
    if (result != 0 && !usesCumulativeByteCountResults_)
        return error_status(result, "sending a short MIDI message through " + outputIdentity_);
    return OutputStatus::success();
}

// v1 Send, SysEx branch + HandleSystemExclusive (lines 164-176, 234-243).
OutputStatus WinMmMidiOutput::send_long(const output::SystemExclusiveEvent& fragment)
{
    if (handle_ == nullptr)
        return OutputStatus::failure(OutputErrorKind::InvalidOperation, "No MIDI output device is open.");
    if (fragment.length == 0) return OutputStatus::success();
    reclaim_completed_long_messages();
    // In an SMF, F0 starts a SysEx packet while an F7 event either continues
    // that packet or contains escaped bytes. winmm drivers may reject an
    // unframed continuation, so assemble fragments and submit only a complete
    // F0...F7 packet.
    output::SystemExclusivePacket packet;
    if (assembler_.accept(fragment, packet)) return send_long_packet(packet, fragment);
    return OutputStatus::success();
}

// v1 SendLong (lines 245-277).
OutputStatus WinMmMidiOutput::send_long_packet(const output::SystemExclusivePacket& packet,
    const output::SystemExclusiveEvent& finalEvent)
{
    if (preparedLongMessagesUnsafe_)
        return OutputStatus::failure(OutputErrorKind::NotSupported, unsafe_provider_message());
    LongBuffer* buffer = allocate_long_buffer(packet.bytes.data(), packet.bytes.size());
    if (buffer == nullptr)
        return OutputStatus::failure(OutputErrorKind::OutOfMemory, out_of_memory_for_sysex_message());
    const uint32_t headerSize = static_cast<uint32_t>(sizeof(MIDIHDR));
    MIDIHDR headerCopy = *buffer->header;   // v1 passes its pre-prepare struct copy to the diagnostics
    uint32_t result = api_.prepare_header(handle_, buffer->header, headerSize);
    OutputStatus status = long_error(result, "midiOutPrepareHeader", packet, finalEvent, &headerCopy);
    if (!status.ok()) {
        free_long_buffer(buffer);
        return status;
    }
    result = api_.long_msg(handle_, buffer->header, headerSize);
    if (result != 0) {
        api_.unprepare_header(handle_, buffer->header, headerSize);
        status = long_error(result, "midiOutLongMsg", packet, finalEvent, &headerCopy);
        free_long_buffer(buffer);
        return status;
    }
    longBuffers_.push_back(buffer);
    return OutputStatus::success();
}

// v1 Panic and SendPanicMessage (lines 184-201); order from panic_message().
OutputStatus WinMmMidiOutput::panic()
{
    if (handle_ == nullptr) return OutputStatus::success();
    for (int i = 0; i < platform::kPanicMessageCount; ++i) {
        platform::PanicMessage message = platform::panic_message(i);
        uint32_t result = api_.short_msg(handle_, message.packed);
        if (result != 0 && !usesCumulativeByteCountResults_)
            return error_status(result, std::string("sending ") + message.name + " panic through " + outputIdentity_);
    }
    return OutputStatus::success();
}

// v1 Reset (lines 226-232). The midiOutReset result is ignored, as in v1.
OutputStatus WinMmMidiOutput::reset()
{
    assembler_.reset();
    if (handle_ == nullptr) return OutputStatus::success();
    api_.reset(handle_);
    reclaim_all_long_messages();
    return OutputStatus::success();
}

// v1 ReclaimCompletedLongMessages (lines 279-293).
void WinMmMidiOutput::reclaim_completed_long_messages()
{
    if (longBuffers_.empty()) return;
    const uint32_t headerSize = static_cast<uint32_t>(sizeof(MIDIHDR));
    for (size_t i = longBuffers_.size(); i-- > 0;) {
        if (long_buffer_done(*longBuffers_[i])) {
            api_.unprepare_header(handle_, longBuffers_[i]->header, headerSize);
            free_long_buffer(longBuffers_[i]);
            longBuffers_.erase(longBuffers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

// v1 ReclaimAllLongMessages (lines 295-304).
void WinMmMidiOutput::reclaim_all_long_messages()
{
    const uint32_t headerSize = static_cast<uint32_t>(sizeof(MIDIHDR));
    for (size_t i = longBuffers_.size(); i-- > 0;) {
        api_.unprepare_header(handle_, longBuffers_[i]->header, headerSize);
        free_long_buffer(longBuffers_[i]);
    }
    longBuffers_.clear();
}

OutputStatus WinMmMidiOutput::close()
{
    dispose_handle();
    return OutputStatus::success();
}

// v1 DisposeHandle (lines 345-359).
void WinMmMidiOutput::dispose_handle()
{
    if (handle_ == nullptr) return;
    if (testBoundary_) { handle_ = nullptr; return; }
    api_.reset(handle_);
    reclaim_all_long_messages();
    assembler_.reset();
    midiOutClose(static_cast<HMIDIOUT>(handle_));
    handle_ = nullptr;
    deviceName_.clear();
    haveDeviceName_ = false;
    outputIdentity_.clear();
    deviceId_ = 0;
    preparedLongMessagesUnsafe_ = false;
    usesCumulativeByteCountResults_ = false;
}

} // namespace win32
} // namespace midibn
