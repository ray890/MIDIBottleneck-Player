// Direct WinMM driver access, part 1 (register entry DD-085). See midi_driver_discovery.h for the design and
// docs/ARCHITECTURE.md ("Direct WinMM driver access") for the evidence behind it. Nothing in this file runs at load
// time; a driver DLL is only touched by resolve_direct_device / describe_driver_entries / MidiDriverModule::acquire.
#include "midi_driver_discovery.h"

#include "midi_path_util.h"
#include "win32_error_text.h"
#include "win32_platform.h"

#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>

namespace midibn {
namespace win32 {

namespace {

std::wstring lower(std::wstring text)
{
    for (size_t i = 0; i < text.size(); ++i) text[i] = static_cast<wchar_t>(std::towlower(text[i]));
    return text;
}

std::wstring last_component(const std::wstring& path)
{
    size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

bool is_midi_value_name(const std::wstring& lowerName)
{
    if (lowerName.compare(0, 4, L"midi") != 0) return false;
    for (size_t i = 4; i < lowerName.size(); ++i)
        if (lowerName[i] < L'0' || lowerName[i] > L'9') return false;
    return true;   // "midi" and "midi<digits>"
}

bool starts_with_folder(const std::wstring& lowerPath, const std::wstring& folder, std::wstring& rest)
{
    if (folder.empty()) return false;
    std::wstring prefix = lower(folder);
    while (!prefix.empty() && prefix[prefix.size() - 1] == L'\\') prefix.erase(prefix.size() - 1);
    if (lowerPath.size() <= prefix.size() + 1 || lowerPath.compare(0, prefix.size(), prefix) != 0 || lowerPath[prefix.size()] != L'\\')
        return false;
    rest = lowerPath.substr(prefix.size() + 1);
    return true;
}

// A plain relative path of ordinary components (the whole path policy lives here).
bool plain_relative_path(const std::wstring& relative, std::string& problem)
{
    if (relative.empty()) { problem = "the entry is empty"; return false; }
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find(L'\\', start);
        std::wstring part = relative.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L"..") { problem = "the path contains an empty, \".\" or \"..\" part"; return false; }
        if (part.size() > 255) { problem = "a part of the path is too long"; return false; }
        for (size_t i = 0; i < part.size(); ++i) {
            wchar_t c = part[i];
            if (c < 32 || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
                problem = "the path contains a character that is not allowed in a plain file name";
                return false;
            }
        }
        wchar_t tail = part[part.size() - 1];
        if (tail == L'.' || tail == L' ') { problem = "a part of the path ends in a dot or a space"; return false; }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

bool ends_with(const std::wstring& text, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

const uint32_t kFaulted = kModFaulted;   // returned by the guarded calls when the driver code raised an exception

uint32_t guarded_call(ModMessageFn fn, uint32_t device, uint32_t message, uintptr_t instance, uintptr_t p1, uintptr_t p2)
{
    __try {
        return fn(device, message, instance, p1, p2);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return kFaulted;
    }
}

bool guarded_proc(DriverProcFn fn, uintptr_t id, void* handle, uint32_t message, intptr_t p1, intptr_t p2, intptr_t& result)
{
    __try {
        result = fn(id, handle, message, p1, p2);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
        return false;
    }
}

void* const kFakeDriverHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(0x4D42));

struct DrvConfigInfo {   // DRVCONFIGINFO: what winmm hands to DRV_OPEN of a driver listed under Drivers32
    DWORD size;
    const wchar_t* section;
    const wchar_t* alias;
};

std::wstring normalized_system_path(std::wstring path)
{
    path = lower(path);
    for (size_t pos = path.find(L"\\syswow64\\"); pos != std::wstring::npos; pos = path.find(L"\\syswow64\\", pos))
        path.replace(pos, 10, L"\\system32\\");
    if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    return path;
}

// The module this file already has loaded in the process, or null. Never loads anything: the lookup by name only
// searches modules that are already mapped.
HMODULE find_loaded_module(const std::wstring& path, const std::wstring& fileName)
{
    HMODULE module = GetModuleHandleW(path.c_str());
    if (module == nullptr) module = GetModuleHandleW(fileName.c_str());
    if (module == nullptr) return nullptr;
    wchar_t buffer[2048];
    DWORD length = GetModuleFileNameW(module, buffer, 2048);
    if (length == 0 || length >= 2048) return nullptr;
    return normalized_system_path(std::wstring(buffer, length)) == normalized_system_path(path) ? module : nullptr;
}

std::mutex g_modules_mutex;
std::map<std::wstring, std::weak_ptr<MidiDriverModule> >& module_table()
{
    static std::map<std::wstring, std::weak_ptr<MidiDriverModule> > table;
    return table;
}

std::string load_failure_text(DWORD code)
{
    if (code == ERROR_BAD_EXE_FORMAT)
        return std::string("it is built for a different kind of Windows program than this one (this is a ") +
            (sizeof(void*) == 8 ? "64-bit" : "32-bit") + " program)";
    if (code == ERROR_MOD_NOT_FOUND)
        return "Windows could not load it, or a file it depends on is missing";
    return "Windows could not load it (" + system_error_description(code) + ")";
}

std::wstring mid_name(const MIDIOUTCAPSW& caps)
{
    size_t length = 0;
    while (length < MAXPNAMELEN && caps.szPname[length] != L'\0') ++length;
    return std::wstring(caps.szPname, length);
}

DeviceIdentity identity_of(const MIDIOUTCAPSW& caps)
{
    DeviceIdentity id;
    id.name = mid_name(caps);
    id.manufacturer_id = caps.wMid;
    id.product_id = caps.wPid;
    id.driver_version = caps.vDriverVersion;
    id.technology = caps.wTechnology;
    return id;
}

} // namespace

uint32_t guarded_mod_message(ModMessageFn fn, uint32_t device, uint32_t message, uintptr_t instance, uintptr_t p1, uintptr_t p2)
{
    return guarded_call(fn, device, message, instance, p1, p2);
}

// ---------------------------------------------------------------- folders

bool running_under_wow64()
{
#if defined(_WIN64)
    return false;
#else
    typedef BOOL(WINAPI * IsWow64ProcessFn)(HANDLE, PBOOL);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    IsWow64ProcessFn fn = kernel ? reinterpret_cast<IsWow64ProcessFn>(GetProcAddress(kernel, "IsWow64Process")) : nullptr;
    BOOL wow = FALSE;
    return fn && fn(GetCurrentProcess(), &wow) && wow;
#endif
}

SystemFolders system_folders()
{
    SystemFolders folders;
    wchar_t buffer[MAX_PATH * 2];
    if (running_under_wow64()) {
        // A 32-bit process on 64-bit Windows loads 32-bit drivers from SysWOW64. GetSystemWow64DirectoryW says so
        // explicitly; the System32 spelling in a registry value means the same folder to this process.
        typedef UINT(WINAPI * GetSystemWow64DirectoryWFn)(LPWSTR, UINT);
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        GetSystemWow64DirectoryWFn fn = kernel
            ? reinterpret_cast<GetSystemWow64DirectoryWFn>(GetProcAddress(kernel, "GetSystemWow64DirectoryW")) : nullptr;
        UINT length = fn ? fn(buffer, static_cast<UINT>(sizeof buffer / sizeof buffer[0])) : 0;
        if (length != 0) {
            folders.directory.assign(buffer, length);
            UINT windows = GetWindowsDirectoryW(buffer, static_cast<UINT>(sizeof buffer / sizeof buffer[0]));
            if (windows != 0) folders.alternate = combine_path(std::wstring(buffer, windows), L"System32");
            return folders;
        }
    }
    UINT length = GetSystemDirectoryW(buffer, static_cast<UINT>(sizeof buffer / sizeof buffer[0]));
    folders.directory.assign(buffer, length);
    return folders;
}

// ---------------------------------------------------------------- Drivers32

DriverEntry classify_driver_entry(const std::wstring& name, const std::wstring& rawValue, const SystemFolders& folders)
{
    DriverEntry entry;
    entry.name = name;
    entry.value = rawValue;
    std::wstring lowerName = lower(name);
    if (lowerName == L"midimapper") {
        entry.kind = DriverEntryKind::Mapper;
        entry.problem = "this is the MIDI Mapper, which Windows routes to another device";
        return entry;
    }
    if (!is_midi_value_name(lowerName)) {
        entry.problem = "not a MIDI driver entry";
        return entry;
    }
    std::wstring value = rawValue;
    while (!value.empty() && (value[0] == L' ' || value[0] == L'\t')) value.erase(0, 1);
    while (!value.empty() && (value[value.size() - 1] == L' ' || value[value.size() - 1] == L'\t')) value.erase(value.size() - 1);
    if (value.empty()) {
        entry.problem = "the entry is empty";
        return entry;
    }
    entry.file_name = last_component(value);
    // wdmaud2.drv is the driver of Windows MIDI Services (Windows 11); like wdmaud.drv it is Windows' own.
    if (lower(entry.file_name) == L"wdmaud.drv" || lower(entry.file_name) == L"wdmaud2.drv") {
        entry.kind = DriverEntryKind::WindowsAudioDriver;
        entry.problem = lower(entry.file_name) == L"wdmaud2.drv" ? "Windows' own MIDI driver (wdmaud2.drv, Windows MIDI Services) serves it"
                                                                : "Windows' own MIDI driver (wdmaud.drv) serves it";
        return entry;
    }
    // Absolute paths are accepted only inside the system folder (either spelling); everything else must be a relative
    // path of plain components, resolved against the system folder.
    std::wstring relative;
    bool absolute = value.size() >= 3 && value[1] == L':' && value[2] == L'\\';
    if (absolute) {
        std::wstring lowerValue = lower(value);
        std::wstring rest;
        if (starts_with_folder(lowerValue, folders.directory, rest) || starts_with_folder(lowerValue, folders.alternate, rest))
            relative = value.substr(value.size() - rest.size());
        else {
            entry.problem = "it points outside the Windows system folder";
            return entry;
        }
    }
    else if (value[0] == L'\\' || value.find(L':') != std::wstring::npos) {
        entry.problem = "it points outside the Windows system folder";
        return entry;
    }
    else relative = value;
    std::string why;
    if (!plain_relative_path(relative, why)) {
        entry.problem = "it is not a plain file name inside the Windows system folder (" + why + ")";
        return entry;
    }
    std::wstring lowerRelative = lower(relative);
    if (!ends_with(lowerRelative, L".dll") && !ends_with(lowerRelative, L".drv")) {
        entry.problem = "it is not a .dll or .drv file";
        return entry;
    }
    entry.kind = DriverEntryKind::UserModeDriver;
    entry.file_name = last_component(relative);
    entry.path = combine_path(folders.directory, relative);
    if (entry.path.size() >= 260) {
        entry.kind = DriverEntryKind::Rejected;
        entry.problem = "the path is too long";
        return entry;
    }
    entry.file_exists = file_exists(entry.path);
    if (!entry.file_exists) entry.problem = "the file " + wide_to_utf8(entry.file_name) + " was not found in the Windows system folder";
    return entry;
}

std::vector<DriverEntry> read_driver_entries(const SystemFolders& folders, std::string& error)
{
    std::vector<DriverEntry> entries;
    REGSAM view = 0;
#if defined(_WIN64)
    view = KEY_WOW64_64KEY;
#else
    if (running_under_wow64()) view = KEY_WOW64_32KEY;   // the registry view the 32-bit winmm reads (WOW6432Node)
#endif
    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32", 0,
        KEY_QUERY_VALUE | view, &key);
    if (result != ERROR_SUCCESS) {
        error = "Windows' driver list (Drivers32) could not be read (" + system_error_description(static_cast<DWORD>(result)) + ")";
        return entries;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameLength = 256;
        BYTE data[2048];
        DWORD dataLength = sizeof data - 2, type = 0;
        LONG r = RegEnumValueW(key, index, name, &nameLength, nullptr, &type, data, &dataLength);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;   // an oversized value is not a driver file name
        std::wstring lowerName = lower(std::wstring(name, nameLength));
        if (lowerName != L"midimapper" && !is_midi_value_name(lowerName)) continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[dataLength] = 0;
        data[dataLength + 1] = 0;
        std::wstring value(reinterpret_cast<wchar_t*>(data));
        if (type == REG_EXPAND_SZ) {
            wchar_t expanded[2048];
            DWORD n = ExpandEnvironmentStringsW(value.c_str(), expanded, 2048);
            if (n != 0 && n <= 2048) value.assign(expanded);
        }
        entries.push_back(classify_driver_entry(std::wstring(name, nameLength), value, folders));
    }
    RegCloseKey(key);
    // Registry order is not reliable; keep a stable order: midi, midi1, midi2 ... then the mapper.
    std::stable_sort(entries.begin(), entries.end(), [](const DriverEntry& a, const DriverEntry& b) {
        auto rank = [](const DriverEntry& e) -> long {
            std::wstring n = lower(e.name);
            if (n == L"midimapper") return 1000000L;
            return n.size() == 4 ? -1L : _wtol(n.c_str() + 4);
        };
        return rank(a) < rank(b);
    });
    return entries;
}

// ---------------------------------------------------------------- start-up sequence

DriverStartup::DriverStartup(DriverProcFn proc, ModMessageFn mod, const std::wstring& alias)
    : proc_(proc), mod_(mod), alias_(alias)
{
}

bool DriverStartup::start(std::string& error)
{
    intptr_t result = 0;
    DrvConfigInfo config = { static_cast<DWORD>(sizeof(DrvConfigInfo)), L"Drivers32", alias_.c_str() };
    if (!guarded_proc(proc_, 0, kFakeDriverHandle, drv::kLoad, 0, 0, result) || result == 0) {
        error = "the driver refused to start (DRV_LOAD)";
        return false;
    }
    loaded_ = true;
    if (!guarded_proc(proc_, 0, kFakeDriverHandle, drv::kEnable, 0, 0, result) || result == 0) {
        error = "the driver refused to start (DRV_ENABLE)";
        stop();
        return false;
    }
    enabled_ = true;
    if (!guarded_proc(proc_, 0, kFakeDriverHandle, drv::kOpen, reinterpret_cast<intptr_t>(&config), 0, result) || result == 0) {
        error = "the driver refused to open (DRV_OPEN)";
        stop();
        return false;
    }
    opened_ = true;
    driver_id_ = static_cast<uintptr_t>(result);
    // winmm then asks the driver to initialise its MIDI part before it counts the devices (Wine: lolvldrv.c
    // MMDRV_InitPerType); drivers that do not need it answer "not supported".
    guarded_call(mod_, 0, drv::kMessageInit, 0, 0, 0);
    started_ = true;
    return true;
}

void DriverStartup::stop()
{
    intptr_t ignored = 0;
    if (opened_) guarded_proc(proc_, driver_id_, kFakeDriverHandle, drv::kClose, 0, 0, ignored);
    if (enabled_) guarded_proc(proc_, 0, kFakeDriverHandle, drv::kDisable, 0, 0, ignored);
    if (loaded_) guarded_proc(proc_, 0, kFakeDriverHandle, drv::kFree, 0, 0, ignored);
    opened_ = enabled_ = loaded_ = started_ = false;
    driver_id_ = 0;
}

// ---------------------------------------------------------------- loaded drivers

std::shared_ptr<MidiDriverModule> MidiDriverModule::acquire(const DriverEntry& entry, std::string& error)
{
    if (entry.kind != DriverEntryKind::UserModeDriver || !entry.file_exists) {
        error = entry.problem.empty() ? std::string("not a loadable MIDI driver") : entry.problem;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    std::wstring key = normalized_system_path(entry.path);
    std::map<std::wstring, std::weak_ptr<MidiDriverModule> >& table = module_table();
    std::map<std::wstring, std::weak_ptr<MidiDriverModule> >::iterator found = table.find(key);
    if (found != table.end()) {
        std::shared_ptr<MidiDriverModule> existing = found->second.lock();
        if (existing) return existing;
        table.erase(found);
    }

    // Was the same file already loaded (by winmm, normally) before this call? Then winmm has started the driver.
    bool alreadyLoaded = find_loaded_module(entry.path, entry.file_name) != nullptr;
    HMODULE module = LoadLibraryExW(entry.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        error = load_failure_text(GetLastError());
        return nullptr;
    }
    ModMessageFn mod = reinterpret_cast<ModMessageFn>(GetProcAddress(module, "modMessage"));
    DriverProcFn proc = reinterpret_cast<DriverProcFn>(GetProcAddress(module, "DriverProc"));
    if (mod == nullptr) {
        FreeLibrary(module);
        error = "the file has no modMessage entry point, so it is not a MIDI output driver";
        return nullptr;
    }
    bool started = false;
    uintptr_t driverId = 0;
    std::string lifecycle = "borrowed from winmm (already started by Windows; only modMessage is used)";
    if (!alreadyLoaded) {
        if (proc == nullptr) {
            FreeLibrary(module);
            error = "the file has no DriverProc entry point";
            return nullptr;
        }
        DriverStartup startup(proc, mod, entry.name);
        std::string why;
        if (!startup.start(why)) {
            FreeLibrary(module);
            error = why;
            return nullptr;
        }
        driverId = startup.driver_id();
        started = true;
        lifecycle = "loaded by this program: DRV_LOAD, DRV_ENABLE, DRV_OPEN, DRVM_INIT";
    }
    std::shared_ptr<MidiDriverModule> driver(new MidiDriverModule());
    driver->path_ = entry.path;
    driver->file_name_ = entry.file_name;
    driver->alias_ = entry.name;
    driver->module_ = module;
    driver->mod_message_ = mod;
    driver->driver_proc_ = proc;
    driver->driver_id_ = driverId;
    driver->borrowed_ = alreadyLoaded;
    driver->started_ = started;
    driver->lifecycle_ = lifecycle;
    table[key] = driver;
    return driver;
}

MidiDriverModule::~MidiDriverModule()
{
    if (started_ && driver_proc_ != nullptr) {
        intptr_t ignored = 0;
        guarded_proc(driver_proc_, driver_id_, kFakeDriverHandle, drv::kClose, 0, 0, ignored);
        guarded_proc(driver_proc_, 0, kFakeDriverHandle, drv::kDisable, 0, 0, ignored);
        guarded_proc(driver_proc_, 0, kFakeDriverHandle, drv::kFree, 0, 0, ignored);
    }
    if (module_ != nullptr) FreeLibrary(static_cast<HMODULE>(module_));
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    std::map<std::wstring, std::weak_ptr<MidiDriverModule> >& table = module_table();
    std::map<std::wstring, std::weak_ptr<MidiDriverModule> >::iterator found = table.find(normalized_system_path(path_));
    if (found != table.end() && found->second.expired()) table.erase(found);
}

// ---------------------------------------------------------------- matching

DeviceChoice choose_direct_device(const DeviceIdentity& selected, const std::vector<CandidateDevices>& candidates,
    bool windowsAudioDriverPresent)
{
    DeviceChoice choice;
    std::vector<std::pair<int, uint32_t> > matches;
    for (size_t c = 0; c < candidates.size(); ++c)
        for (size_t d = 0; d < candidates[c].devices.size(); ++d)
            if (candidates[c].devices[d] == selected) matches.push_back(std::make_pair(static_cast<int>(c), static_cast<uint32_t>(d)));
    if (matches.size() == 1) {
        choice.found = true;
        choice.candidate = matches[0].first;
        choice.local_index = matches[0].second;
        return choice;
    }
    if (matches.size() > 1) {
        std::string files;
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i != 0) files += ", ";
            files += wide_to_utf8(candidates[static_cast<size_t>(matches[i].first)].file_name);
        }
        choice.reason = "more than one driver (" + files + ") reports a device with exactly this name and identity, so the right one cannot be told apart";
        return choice;
    }
    choice.reason = "no installed MIDI driver that this program can open directly reports this device";
    if (windowsAudioDriverPresent)
        choice.reason += " (it is probably served by Windows' own MIDI driver, which covers the Microsoft GS Wavetable Synth and hardware or USB MIDI ports)";
    for (size_t c = 0; c < candidates.size(); ++c)
        if (!candidates[c].unavailable.empty())
            choice.reason += "; " + wide_to_utf8(candidates[c].file_name) + " could not be checked: " + candidates[c].unavailable;
    return choice;
}

namespace {
std::mutex g_unresponsive_mutex;
std::map<std::wstring, std::string>& unresponsive_table()
{
    static std::map<std::wstring, std::string> table;
    return table;
}
} // namespace

void mark_driver_unresponsive(const std::wstring& fileName, const std::string& what)
{
    std::lock_guard<std::mutex> lock(g_unresponsive_mutex);
    unresponsive_table()[lower(fileName)] = what;
}

bool driver_unresponsive(const std::wstring& fileName, std::string* what)
{
    std::lock_guard<std::mutex> lock(g_unresponsive_mutex);
    std::map<std::wstring, std::string>::const_iterator it = unresponsive_table().find(lower(fileName));
    if (it == unresponsive_table().end()) return false;
    if (what != nullptr) *what = it->second;
    return true;
}

void forget_unresponsive_drivers()
{
    std::lock_guard<std::mutex> lock(g_unresponsive_mutex);
    unresponsive_table().clear();
}

bool is_verified_driver(const std::wstring& fileName)
{
    std::wstring n = lower(fileName);
    return n == L"omnimidi.dll" || n == L"virtualmidisynth.dll";
}

namespace {

// Asks one loaded driver for its devices.
CandidateDevices query_devices(const DriverEntry& entry, std::shared_ptr<MidiDriverModule>& driverOut)
{
    CandidateDevices candidate;
    candidate.file_name = entry.file_name;
    std::string error;
    std::string hung;
    if (driver_unresponsive(entry.file_name, &hung)) {   // DD-094: never called again in this process
        candidate.unavailable = "it stopped answering earlier in this session (" + hung + "), so this program does not call it again until it is restarted";
        return candidate;
    }
    std::shared_ptr<MidiDriverModule> driver = MidiDriverModule::acquire(entry, error);
    if (!driver) {
        candidate.unavailable = error;
        return candidate;
    }
    uint32_t count = guarded_call(driver->mod_message(), 0, drv::kModGetNumDevs, 0, 0, 0);
    if (count == kFaulted) {
        candidate.unavailable = "the driver crashed when asked for its devices";
        return candidate;
    }
    if (count > 256) count = 256;   // not a device count
    for (uint32_t i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps;
        std::memset(&caps, 0, sizeof caps);
        uint32_t r = guarded_call(driver->mod_message(), i, drv::kModGetDevCaps, 0, reinterpret_cast<uintptr_t>(&caps), sizeof caps);
        if (r != 0) { candidate.devices.push_back(DeviceIdentity()); continue; }   // keeps the local numbering aligned
        candidate.devices.push_back(identity_of(caps));
    }
    driverOut = driver;
    return candidate;
}

} // namespace

DirectResolution resolve_direct_device(uint32_t deviceId)
{
    DirectResolution resolution;
    if (deviceId == 0xFFFFFFFFu) {
        resolution.reason = "this is the MIDI Mapper, which Windows routes to another device; direct access needs a specific device";
        return resolution;
    }
    midiOutGetNumDevs();   // lets Windows start its own MIDI drivers first (they are then borrowed, not started a second time)
    MIDIOUTCAPSW caps;
    std::memset(&caps, 0, sizeof caps);
    MMRESULT mm = midiOutGetDevCapsW(deviceId, &caps, sizeof caps);
    if (mm != MMSYSERR_NOERROR) {
        resolution.reason = "Windows does not report this output device (error " + std::to_string(mm) + ")";
        return resolution;
    }
    DeviceIdentity selected = identity_of(caps);
    resolution.device_name = wide_to_utf8(selected.name);

    SystemFolders folders = system_folders();
    std::string error;
    std::vector<DriverEntry> entries = read_driver_entries(folders, error);
    if (!error.empty()) {
        resolution.reason = error;
        return resolution;
    }
    bool windowsAudioDriver = false;
    std::vector<DriverEntry> usable;
    std::vector<std::wstring> seen;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].kind == DriverEntryKind::WindowsAudioDriver) windowsAudioDriver = true;
        if (entries[i].kind != DriverEntryKind::UserModeDriver || !entries[i].file_exists) continue;
        std::wstring key = normalized_system_path(entries[i].path);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        usable.push_back(entries[i]);
    }
    std::vector<CandidateDevices> candidates;
    std::vector<std::shared_ptr<MidiDriverModule> > drivers;
    for (size_t i = 0; i < usable.size(); ++i) {
        std::shared_ptr<MidiDriverModule> driver;
        candidates.push_back(query_devices(usable[i], driver));
        drivers.push_back(driver);
    }
    DeviceChoice choice = choose_direct_device(selected, candidates, windowsAudioDriver);
    if (!choice.found) {
        resolution.reason = choice.reason;
        return resolution;
    }
    resolution.ok = true;
    resolution.driver = drivers[static_cast<size_t>(choice.candidate)];
    resolution.mod_message = drivers[static_cast<size_t>(choice.candidate)]->mod_message();
    resolution.local_index = choice.local_index;
    resolution.driver_file = wide_to_utf8(candidates[static_cast<size_t>(choice.candidate)].file_name);
    resolution.verified = is_verified_driver(candidates[static_cast<size_t>(choice.candidate)].file_name);
    resolution.lifecycle = drivers[static_cast<size_t>(choice.candidate)]->lifecycle_description();
    return resolution;
}

std::vector<DriverReportLine> describe_driver_entries(bool let_winmm_start_first)
{
    std::vector<DriverReportLine> lines;
    if (let_winmm_start_first) midiOutGetNumDevs();
    SystemFolders folders = system_folders();
    std::string error;
    std::vector<DriverEntry> entries = read_driver_entries(folders, error);
    if (!error.empty()) {
        DriverReportLine line;
        line.state = error;
        lines.push_back(line);
        return lines;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const DriverEntry& e = entries[i];
        DriverReportLine line;
        line.name = wide_to_utf8(e.name);
        line.value = wide_to_utf8(e.value);
        line.path = wide_to_utf8(e.path);
        switch (e.kind) {
        case DriverEntryKind::UserModeDriver: line.kind = "user-mode driver"; break;
        case DriverEntryKind::WindowsAudioDriver: line.kind = "Windows audio driver (refused)"; break;
        case DriverEntryKind::Mapper: line.kind = "MIDI Mapper (refused)"; break;
        default: line.kind = "rejected"; break;
        }
        if (e.kind == DriverEntryKind::UserModeDriver && e.file_exists) {
            std::shared_ptr<MidiDriverModule> driver;
            CandidateDevices devices = query_devices(e, driver);
            if (!devices.unavailable.empty()) line.state = "cannot be used directly: " + devices.unavailable;
            else {
                line.state = (driver && driver->borrowed() ? "found, already loaded by Windows; " : "found, loaded and started by this check; ") +
                    std::to_string(devices.devices.size()) + " device(s)";
                for (size_t d = 0; d < devices.devices.size(); ++d)
                    line.state += (d == 0 ? ": " : ", ") + wide_to_utf8(devices.devices[d].name);
            }
        }
        else line.state = e.problem;
        lines.push_back(line);
    }
    return lines;
}

} // namespace win32
} // namespace midibn
