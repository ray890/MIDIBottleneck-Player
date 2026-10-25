// Direct WinMM driver access, part 1 (register entry DD-085): finding the installed user-mode MIDI driver that
// serves a WinMM output device, loading it, and talking to it through its own entry points (DriverProc and
// modMessage), the way winmm.dll does for every midiOut call.
//
// Nothing here is third-party code and nothing is installed: the driver is a DLL the user already has (listed under
// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32 as midi, midi1, midi2 ...). It is loaded only by its full
// path inside the Windows system folder.
//
// Why: on Windows 8 and later the system midiOutShortMsg path is far slower than calling the synthesizer driver
// directly (docs/DEVICE_MEASUREMENTS.md; prompt 007: ~0.04 M events/s through system WinMM, 3.8-4.6 M through a
// direct call). This is the built-in, driver-independent version of what an application-local winmm replacement does
// for one synthesizer.
//
// XP-compatible: only APIs of Windows XP SP3 are used (IsWow64Process and GetSystemWow64Directory are looked up at
// run time). Not thread-safe by itself: the output object calls it from the engine's worker thread (open, send,
// close); the process-wide driver table has its own lock because the discovery can also run on the UI thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace win32 {

// ---------------------------------------------------------------- driver interface constants
// Restated from the Windows driver kit (mmddk.h, mmsystem.h) so the XP build needs no mmddk.h. The values are part of
// the Windows driver ABI and have not changed since Windows 95.
namespace drv {
const uint32_t kLoad = 1;          // DRV_LOAD
const uint32_t kEnable = 2;        // DRV_ENABLE
const uint32_t kOpen = 3;          // DRV_OPEN
const uint32_t kClose = 4;         // DRV_CLOSE
const uint32_t kDisable = 5;       // DRV_DISABLE
const uint32_t kFree = 6;          // DRV_FREE
const uint32_t kMessageInit = 100; // DRVM_INIT (sent by winmm to a MIDI driver's modMessage after loading it)
const uint32_t kModGetNumDevs = 1; // MODM_GETNUMDEVS
const uint32_t kModGetDevCaps = 2; // MODM_GETDEVCAPS
const uint32_t kModOpen = 3;       // MODM_OPEN
const uint32_t kModClose = 4;      // MODM_CLOSE
const uint32_t kModPrepare = 5;    // MODM_PREPARE
const uint32_t kModUnprepare = 6;  // MODM_UNPREPARE
const uint32_t kModData = 7;       // MODM_DATA (short message)
const uint32_t kModLongData = 8;   // MODM_LONGDATA
const uint32_t kModReset = 9;      // MODM_RESET
} // namespace drv

// The driver's two entry points. modMessage(deviceIndexInThatDriver, message, instance, param1, param2).
typedef uint32_t(__stdcall* ModMessageFn)(uint32_t device, uint32_t message, uintptr_t instance, uintptr_t param1,
    uintptr_t param2);
typedef intptr_t(__stdcall* DriverProcFn)(uintptr_t driverId, void* driverHandle, uint32_t message, intptr_t param1,
    intptr_t param2);

// modMessage called inside an exception guard (access violations and the like raised by the driver code): returns
// kModFaulted instead of crashing the program. The per-event path (MODM_DATA) deliberately does not use it.
const uint32_t kModFaulted = 0xFFFFFFF0u;
uint32_t guarded_mod_message(ModMessageFn fn, uint32_t device, uint32_t message, uintptr_t instance, uintptr_t param1,
    uintptr_t param2);

// ---------------------------------------------------------------- Drivers32 entries

enum class DriverEntryKind {
    UserModeDriver,        // a DLL inside the system folder that can be loaded and asked directly
    WindowsAudioDriver,    // wdmaud.drv: Windows' own MIDI driver (Microsoft GS Wavetable Synth, hardware and USB ports)
    Mapper,                // the midimapper entry (the MIDI Mapper)
    Rejected               // value that is empty, outside the system folder, not a .dll/.drv, or malformed
};

struct DriverEntry {
    std::wstring name;     // value name: midi, midi1 ...
    std::wstring value;    // the registry text
    DriverEntryKind kind = DriverEntryKind::Rejected;
    std::wstring path;     // UserModeDriver: full path of the file inside the system folder
    std::wstring file_name; // last path component (lower case is not applied)
    bool file_exists = false;
    std::string problem;   // Rejected / missing file: plain-language reason
};

// The folders the driver files are looked up in. `directory` is the system folder this process's winmm loads drivers
// from: the 64-bit System32 for a 64-bit process, SysWOW64 for a 32-bit process on 64-bit Windows, System32 on
// 32-bit Windows. `alternate` is the other spelling of the same folder (System32 for a 32-bit process on 64-bit
// Windows, where a registry value written as System32 means SysWOW64 to that process); empty when there is none.
struct SystemFolders {
    std::wstring directory;
    std::wstring alternate;
};
SystemFolders system_folders();
// True when this is a 32-bit process on 64-bit Windows (WOW64).
bool running_under_wow64();

// Decides what one Drivers32 value means. Pure (the file check is the only file access); this is the whole policy:
//  - a value name is a candidate only if it is `midi` or `midi<digits>` (any case); `midimapper` is the Mapper;
//  - a value whose file name is wdmaud.drv or wdmaud2.drv is the Windows audio driver, refused;
//  - otherwise the value must be a relative path made of plain components (no drive, no leading backslash or UNC
//    prefix, no "." or ".." component, no forward slash, no wildcard or stream characters, ending in .dll or .drv),
//    resolved against the system folder (so OmniMIDI\OmniMapper.dll means System32\OmniMIDI\OmniMapper.dll), or an
//    absolute path that is inside the system folder (either spelling); anything else is Rejected with the reason.
DriverEntry classify_driver_entry(const std::wstring& name, const std::wstring& value, const SystemFolders& folders);

// Reads every midi / midiN / midimapper value of Drivers32 from the registry view that this process's winmm uses
// (KEY_WOW64_64KEY for a 64-bit process, KEY_WOW64_32KEY for a 32-bit process on 64-bit Windows, the plain key on
// 32-bit Windows), in registry order. Read only. `error` is set (and the list empty) if the key cannot be read.
std::vector<DriverEntry> read_driver_entries(const SystemFolders& folders, std::string& error);

// ---------------------------------------------------------------- loaded drivers

// One driver DLL in this process. If winmm (or anything else in this process) had already loaded the same file,
// winmm has already run the driver's start-up (DRV_LOAD, DRV_ENABLE, DRV_OPEN, DRVM_INIT) and this object only talks to
// modMessage ("borrowed"); the driver is never initialised a second time (DD-085 design notes in ARCHITECTURE.md).
// Otherwise this object loads the file by its full path and runs that start-up itself, and the reverse at the end.
class MidiDriverModule {
public:
    // `error` is set (in plain language) and null returned if the file cannot be loaded or does not have a modMessage
    // entry point or the driver refuses DRV_OPEN. One object per file is shared process-wide.
    static std::shared_ptr<MidiDriverModule> acquire(const DriverEntry& entry, std::string& error);
    ~MidiDriverModule();
    MidiDriverModule(const MidiDriverModule&) = delete;
    MidiDriverModule& operator=(const MidiDriverModule&) = delete;

    ModMessageFn mod_message() const { return mod_message_; }
    const std::wstring& path() const { return path_; }
    const std::wstring& file_name() const { return file_name_; }
    // True when winmm had already started the driver; false when this object ran DRV_LOAD ... DRVM_INIT itself.
    bool borrowed() const { return borrowed_; }
    // "borrowed from winmm" or the messages sent, for diagnostics and the Windows 11 check.
    std::string lifecycle_description() const { return lifecycle_; }

private:
    MidiDriverModule() {}
    std::wstring path_;
    std::wstring file_name_;
    std::wstring alias_;         // the Drivers32 value name, handed to DRV_OPEN like winmm does
    void* module_ = nullptr;     // HMODULE; one LoadLibrary reference is held for the object's lifetime
    ModMessageFn mod_message_ = nullptr;
    DriverProcFn driver_proc_ = nullptr;
    uintptr_t driver_id_ = 0;    // DRV_OPEN's result, passed to the later DriverProc calls
    bool borrowed_ = false;
    bool started_ = false;       // DRV_LOAD/ENABLE/OPEN were sent by this object
    std::string lifecycle_;
};

// The start-up and shut-down sequence on its own (so it can be tested with plain functions, without any DLL):
// DRV_LOAD, DRV_ENABLE, DRV_OPEN, DRVM_INIT and, at the end, DRV_CLOSE, DRV_DISABLE, DRV_FREE. DRV_LOAD, DRV_ENABLE,
// DRV_DISABLE and DRV_FREE carry driver id 0; DRV_OPEN returns the id used by DRV_CLOSE; lParam1 of DRV_OPEN is a
// DRVCONFIGINFO (section "Drivers32", the value name as alias).
class DriverStartup {
public:
    DriverStartup(DriverProcFn proc, ModMessageFn mod, const std::wstring& alias);
    // False (with `error`) if DRV_OPEN returns 0 (the driver refuses to open); anything started is then undone.
    bool start(std::string& error);
    void stop();
    bool started() const { return started_; }
    uintptr_t driver_id() const { return driver_id_; }
private:
    DriverProcFn proc_;
    ModMessageFn mod_;
    std::wstring alias_;
    uintptr_t driver_id_ = 0;
    bool started_ = false;
    bool loaded_ = false, enabled_ = false, opened_ = false;
};

// ---------------------------------------------------------------- matching a WinMM device to a driver device

// The identity fields WinMM and the driver both report (MIDIOUTCAPSW).
struct DeviceIdentity {
    std::wstring name;
    uint16_t manufacturer_id = 0;
    uint16_t product_id = 0;
    uint32_t driver_version = 0;
    uint16_t technology = 0;
    bool operator==(const DeviceIdentity& o) const
    {
        return name == o.name && manufacturer_id == o.manufacturer_id && product_id == o.product_id &&
            driver_version == o.driver_version && technology == o.technology;
    }
};

// What one candidate driver reports (its device list in its own numbering), or why it could not be asked.
struct CandidateDevices {
    std::wstring file_name;
    std::vector<DeviceIdentity> devices;
    std::string unavailable;   // non-empty: this driver could not be loaded / opened / asked (reason)
};

struct DeviceChoice {
    bool found = false;
    int candidate = -1;
    uint32_t local_index = 0;
    std::string reason;        // when not found: plain-language reason
};

// The matching rule: exactly one device among all candidates with the same name, manufacturer id, product id,
// driver version and technology as the selected WinMM device. Two matches are ambiguous and refused (they would be
// indistinguishable, so the wrong synthesizer could be driven).
DeviceChoice choose_direct_device(const DeviceIdentity& selected, const std::vector<CandidateDevices>& candidates,
    bool windowsAudioDriverPresent);

// ---------------------------------------------------------------- resolving

struct DirectResolution {
    bool ok = false;
    std::string reason;                          // when not ok: why direct access is not available (plain language)
    ModMessageFn mod_message = nullptr;          // the driver's entry point (tests supply a fake)
    std::shared_ptr<void> driver;                // keeps the driver loaded for as long as the caller holds it
    uint32_t local_index = 0;                    // the device's number inside the driver
    std::string device_name;                     // UTF-8
    std::string driver_file;                     // UTF-8 file name, e.g. VirtualMIDISynth.dll
    bool verified = false;                       // the driver is on the tested list
    std::string lifecycle;                       // "borrowed from winmm" or the sequence this program ran
};

// DD-094: drivers that stopped answering in this process (a reset or close that did not return within its time limit).
// Such a driver is never called again by this program until it is restarted: the resolver skips it (its devices then
// play through standard WinMM) and the direct output refuses to open it. Keyed by file name, any case; `what` says
// which call hung. Thread-safe.
void mark_driver_unresponsive(const std::wstring& file_name, const std::string& what);
bool driver_unresponsive(const std::wstring& file_name, std::string* what = nullptr);
void forget_unresponsive_drivers();   // tests only

// Drivers that have been tested with this feature (file names, any case). Everything else is allowed but reported as
// "unverified" in the status line.
bool is_verified_driver(const std::wstring& file_name);

// The whole chain for one WinMM device id: refuses the MIDI Mapper, asks winmm for the device's identity, reads
// Drivers32, loads / borrows the candidate drivers in registry order (stops at the first match) and returns the
// driver, its local device number and the verified flag. Never opens the device.
DirectResolution resolve_direct_device(uint32_t winmm_device_id);

// Text for the Windows 11 check and the self-tests: a read-only report of Drivers32 and, per user-mode driver, whether
// it can be found, loaded and asked for its devices. `loaded_by_winmm` tells whether winmm had loaded the file.
struct DriverReportLine {
    std::string name, value, kind, path, state;
};
// `let_winmm_start_first`: call midiOutGetNumDevs() first (as the player does), so the drivers are already running
// ("borrowed"); false asks the drivers while winmm has not touched them, so this program loads and starts them itself
// (and stops and unloads them again before returning) - a check of the second start-up path.
std::vector<DriverReportLine> describe_driver_entries(bool let_winmm_start_first);

} // namespace win32
} // namespace midibn
