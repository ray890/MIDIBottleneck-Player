// Checks of "Direct WinMM driver access" (register entry DD-085) that need no device and load no driver: the
// Drivers32 path policy, the device matching rule, the driver start-up sequence on plain functions, and the direct
// output on a fake modMessage. Nothing here calls midiOut*/modMessage of a real driver. Exit code 0 when all pass.
#include "midi_driver_discovery.h"
#include "midi_long_buffer.h"
#include "winmm_direct_output.h"
#include "win32_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

using namespace midibn;
using namespace midibn::win32;
using platform::OutputErrorKind;
using platform::OutputStatus;

// ---------------------------------------------------------------- path policy

void test_entry_policy()
{
    SystemFolders folders;
    folders.directory = L"C:\\Windows\\System32";
    folders.alternate = L"C:\\Windows\\SysWOW64";   // pretend: a 32-bit process, where System32 means SysWOW64
    folders.directory = L"C:\\Windows\\SysWOW64";
    folders.alternate = L"C:\\Windows\\System32";
    auto kind = [&](const wchar_t* name, const wchar_t* value) { return classify_driver_entry(name, value, folders).kind; };

    check(kind(L"midi1", L"VirtualMIDISynth.dll") == DriverEntryKind::UserModeDriver, "policy: a bare file name is a user-mode driver");
    DriverEntry bare = classify_driver_entry(L"midi1", L"VirtualMIDISynth.dll", folders);
    check(bare.path == L"C:\\Windows\\SysWOW64\\VirtualMIDISynth.dll" && bare.file_name == L"VirtualMIDISynth.dll", "policy: resolved inside the system folder, never by bare name");
    DriverEntry sub = classify_driver_entry(L"midi7", L"OmniMIDI\\Sub.dll", folders);
    check(sub.kind == DriverEntryKind::UserModeDriver && sub.path == L"C:\\Windows\\SysWOW64\\OmniMIDI\\Sub.dll", "policy: a subfolder inside the system folder is accepted");
    check(kind(L"MIDI", L"x.drv") == DriverEntryKind::UserModeDriver, "policy: the name midi is a candidate, any case, and .drv is accepted");
    check(kind(L"midi12", L"x.dll") == DriverEntryKind::UserModeDriver, "policy: midi12 is a candidate");
    check(kind(L"midi1", L"c:\\windows\\system32\\x.dll") == DriverEntryKind::UserModeDriver, "policy: absolute path in the other spelling of the system folder");
    check(classify_driver_entry(L"midi1", L"c:\\windows\\system32\\x.dll", folders).path == L"C:\\Windows\\SysWOW64\\x.dll", "policy: ... is re-based onto this process's system folder");
    check(kind(L"midi1", L"C:\\Windows\\SysWOW64\\a\\x.dll") == DriverEntryKind::UserModeDriver, "policy: absolute path inside the folder, in a subfolder");
    check(kind(L"midi1", L"  x.dll  ") == DriverEntryKind::UserModeDriver, "policy: surrounding spaces are ignored");

    check(kind(L"midi", L"wdmaud.drv") == DriverEntryKind::WindowsAudioDriver, "policy: wdmaud.drv is Windows' own driver");
    check(kind(L"midi3", L"WDMAUD.DRV") == DriverEntryKind::WindowsAudioDriver, "policy: wdmaud.drv in any case");
    check(kind(L"midi3", L"C:\\Windows\\System32\\wdmaud.drv") == DriverEntryKind::WindowsAudioDriver, "policy: wdmaud.drv by full path");
    check(kind(L"midi", L"wdmaud2.drv") == DriverEntryKind::WindowsAudioDriver, "policy: wdmaud2.drv (Windows MIDI Services) is Windows' own driver");
    check(kind(L"midi2", L"WDMAUD2.DRV") == DriverEntryKind::WindowsAudioDriver, "policy: wdmaud2.drv in any case");
    check(kind(L"midi2", L"C:\\Windows\\System32\\wdmaud2.drv") == DriverEntryKind::WindowsAudioDriver, "policy: wdmaud2.drv by full path");
    check(kind(L"midimapper", L"OmniMIDI\\OmniMapper.dll") == DriverEntryKind::Mapper, "policy: midimapper is the MIDI Mapper (refused)");
    check(kind(L"MidiMapper", L"midimap.dll") == DriverEntryKind::Mapper, "policy: MidiMapper in any case");
    check(kind(L"wave", L"x.dll") == DriverEntryKind::Rejected, "policy: not a midi entry");
    check(kind(L"midi1x", L"x.dll") == DriverEntryKind::Rejected, "policy: midi1x is not a midi entry");
    check(kind(L"midi1", L"") == DriverEntryKind::Rejected, "policy: empty value rejected");

    check(kind(L"midi1", L"..\\evil.dll") == DriverEntryKind::Rejected, "policy: .. rejected");
    check(kind(L"midi1", L"a\\..\\b.dll") == DriverEntryKind::Rejected, "policy: .. in the middle rejected");
    check(kind(L"midi1", L".\\x.dll") == DriverEntryKind::Rejected, "policy: . part rejected");
    check(kind(L"midi1", L"a//b.dll") == DriverEntryKind::Rejected, "policy: forward slashes rejected");
    check(kind(L"midi1", L"a\\\\b.dll") == DriverEntryKind::Rejected, "policy: empty part rejected");
    check(kind(L"midi1", L"\\\\server\\share\\x.dll") == DriverEntryKind::Rejected, "policy: UNC path rejected");
    check(kind(L"midi1", L"\\Windows\\System32\\x.dll") == DriverEntryKind::Rejected, "policy: rooted path rejected");
    check(kind(L"midi1", L"C:x.dll") == DriverEntryKind::Rejected, "policy: drive-relative path rejected");
    check(kind(L"midi1", L"D:\\Program Files\\x\\x.dll") == DriverEntryKind::Rejected, "policy: a path outside the system folder is rejected");
    check(kind(L"midi1", L"C:\\Windows\\System32X\\x.dll") == DriverEntryKind::Rejected, "policy: a folder that only starts like the system folder is rejected");
    check(kind(L"midi1", L"C:\\Windows\\System32\\..\\x.dll") == DriverEntryKind::Rejected, "policy: .. inside an absolute path rejected");
    check(kind(L"midi1", L"x.dll:stream") == DriverEntryKind::Rejected, "policy: alternate data stream rejected");
    check(kind(L"midi1", L"x*.dll") == DriverEntryKind::Rejected, "policy: wildcard rejected");
    check(kind(L"midi1", L"x.exe") == DriverEntryKind::Rejected, "policy: not a .dll or .drv");
    check(kind(L"midi1", L"x.dll.") == DriverEntryKind::Rejected, "policy: trailing dot rejected");
    check(kind(L"midi1", L"x\\ .dll ") != DriverEntryKind::WindowsAudioDriver, "policy: odd spacing is never Windows' driver");
    check(!classify_driver_entry(L"midi1", L"..\\evil.dll", folders).problem.empty(), "policy: a rejection carries a reason");
    check(classify_driver_entry(L"midi1", L"..\\evil.dll", folders).path.empty(), "policy: a rejected entry has no path to load");

    // the registry on this PC: every user-mode entry resolves inside this process's system folder
    SystemFolders real = system_folders();
    check(!real.directory.empty(), "system folder is known");
    std::string error;
    std::vector<DriverEntry> entries = read_driver_entries(real, error);
    check(error.empty(), "Drivers32 can be read: " + error);
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].kind == DriverEntryKind::UserModeDriver)
            check(entries[i].path.size() > real.directory.size() && _wcsnicmp(entries[i].path.c_str(), real.directory.c_str(), real.directory.size()) == 0,
                "registry entry " + wide_to_utf8(entries[i].name) + " resolves inside the system folder");
#if defined(_WIN64)
    check(!running_under_wow64(), "a 64-bit program is not under WOW64");
#endif
}

// ---------------------------------------------------------------- matching

DeviceIdentity ident(const wchar_t* name, uint16_t mid = 65535, uint16_t pid = 1, uint32_t ver = 0x20D, uint16_t tech = 7)
{
    DeviceIdentity d;
    d.name = name;
    d.manufacturer_id = mid;
    d.product_id = pid;
    d.driver_version = ver;
    d.technology = tech;
    return d;
}

void test_matching()
{
    std::vector<CandidateDevices> c(2);
    c[0].file_name = L"A.dll";
    c[0].devices.push_back(ident(L"Port 1"));
    c[0].devices.push_back(ident(L"Port 2"));
    c[1].file_name = L"B.dll";
    c[1].devices.push_back(ident(L"Other", 1, 2, 3, 4));
    DeviceChoice r = choose_direct_device(ident(L"Port 2"), c, true);
    check(r.found && r.candidate == 0 && r.local_index == 1, "match: the driver's own device number is used, not the Windows number");
    r = choose_direct_device(ident(L"Other", 1, 2, 3, 4), c, true);
    check(r.found && r.candidate == 1 && r.local_index == 0, "match: second driver");
    r = choose_direct_device(ident(L"Port 2", 65535, 1, 0x20D, 6), c, true);
    check(!r.found && r.reason.find("no installed MIDI driver") != std::string::npos, "match: technology must match too");
    r = choose_direct_device(ident(L"Port 2", 65535, 2), c, true);
    check(!r.found, "match: product id must match");
    r = choose_direct_device(ident(L"Port 2", 7), c, true);
    check(!r.found, "match: manufacturer id must match");
    r = choose_direct_device(ident(L"Port 2", 65535, 1, 0x20E), c, true);
    check(!r.found, "match: driver version must match");
    r = choose_direct_device(ident(L"Microsoft GS Wavetable Synth", 1, 27, 0x100, 7), c, true);
    check(!r.found && r.reason.find("Microsoft GS Wavetable Synth") != std::string::npos && r.reason.find("Windows' own MIDI driver") != std::string::npos,
        "match: Windows' own device gets the explanation");
    r = choose_direct_device(ident(L"Microsoft GS Wavetable Synth", 1, 27, 0x100, 7), c, false);
    check(!r.found && r.reason.find("Windows' own") == std::string::npos, "match: no wdmaud entry, no wdmaud sentence");
    c[1].devices.push_back(ident(L"Port 1"));
    r = choose_direct_device(ident(L"Port 1"), c, true);
    check(!r.found && r.reason.find("cannot be told apart") != std::string::npos && r.reason.find("A.dll") != std::string::npos && r.reason.find("B.dll") != std::string::npos,
        "match: two identical devices are refused, naming both drivers");
    std::vector<CandidateDevices> broken(1);
    broken[0].file_name = L"Broken.dll";
    broken[0].unavailable = "Windows could not load it";
    r = choose_direct_device(ident(L"X"), broken, false);
    check(!r.found && r.reason.find("Broken.dll could not be checked: Windows could not load it") != std::string::npos, "match: an unavailable driver is named in the reason");
    r = choose_direct_device(ident(L"X"), std::vector<CandidateDevices>(), true);
    check(!r.found, "match: no candidates");
    check(is_verified_driver(L"OmniMIDI.dll") && is_verified_driver(L"VirtualMIDISynth.DLL") && !is_verified_driver(L"Other.dll"), "verified list: OmniMIDI and VirtualMIDISynth only");
    DirectResolution mapper = resolve_direct_device(0xFFFFFFFFu);
    check(!mapper.ok && mapper.reason.find("MIDI Mapper") != std::string::npos, "the MIDI Mapper id is refused");
}

// ---------------------------------------------------------------- start-up sequence

std::vector<std::string> g_log;
intptr_t g_open_result = 1;
intptr_t g_load_result = 1;

intptr_t __stdcall fake_proc(uintptr_t id, void*, uint32_t message, intptr_t, intptr_t)
{
    const char* names[] = { "?", "LOAD", "ENABLE", "OPEN", "CLOSE", "DISABLE", "FREE" };
    g_log.push_back(std::string(message < 7 ? names[message] : "?") + ":" + std::to_string(id));
    if (message == drv::kLoad) return g_load_result;
    if (message == drv::kOpen) return g_open_result;
    return 1;
}
uint32_t __stdcall fake_mod_for_startup(uint32_t device, uint32_t message, uintptr_t, uintptr_t, uintptr_t)
{
    g_log.push_back("mod:" + std::to_string(message) + ":" + std::to_string(device));
    return 8;
}

void test_startup_sequence()
{
    g_log.clear();
    g_open_result = 77;
    g_load_result = 1;
    {
        DriverStartup s(&fake_proc, &fake_mod_for_startup, L"midi1");
        std::string error;
        check(s.start(error) && s.started() && s.driver_id() == 77, "startup: succeeds and keeps the id DRV_OPEN returned");
        check(g_log == std::vector<std::string>({ "LOAD:0", "ENABLE:0", "OPEN:0", "mod:100:0" }), "startup: DRV_LOAD, DRV_ENABLE, DRV_OPEN, then DRVM_INIT");
        g_log.clear();
        s.stop();
        check(g_log == std::vector<std::string>({ "CLOSE:77", "DISABLE:0", "FREE:0" }), "shutdown: DRV_CLOSE with the id, DRV_DISABLE, DRV_FREE");
        g_log.clear();
        s.stop();
        check(g_log.empty(), "shutdown: a second stop does nothing");
    }
    g_log.clear();
    g_open_result = 0;
    {
        DriverStartup s(&fake_proc, &fake_mod_for_startup, L"midi1");
        std::string error;
        check(!s.start(error) && !s.started() && error.find("DRV_OPEN") != std::string::npos, "startup: a driver that refuses DRV_OPEN fails with a reason");
        check(g_log == std::vector<std::string>({ "LOAD:0", "ENABLE:0", "OPEN:0", "DISABLE:0", "FREE:0" }), "startup: a refused open is undone (disable, free), nothing closed");
    }
    g_log.clear();
    g_open_result = 5;
    g_load_result = 0;
    {
        DriverStartup s(&fake_proc, &fake_mod_for_startup, L"midi1");
        std::string error;
        check(!s.start(error) && g_log == std::vector<std::string>({ "LOAD:0" }), "startup: a driver that refuses DRV_LOAD stops there");
    }
}

// ---------------------------------------------------------------- the output on a fake modMessage

#pragma pack(push, 1)
struct FakeOpenDesc { HMIDI midi; DWORD_PTR callback; DWORD_PTR instance; DWORD_PTR devNode; DWORD ids; DWORD s0, s1; };
#pragma pack(pop)

struct Call { uint32_t device, message; uintptr_t instance, p1, p2; };
std::vector<Call> g_calls;
std::vector<std::vector<uint8_t> > g_sysex_seen;
uintptr_t g_open_instance_value = 0x1234;
uint32_t g_open_result_code = 0;
uint32_t g_prepare_result = 8;       // NOTSUPPORTED, like VirtualMIDISynth
uint32_t g_longdata_result = 0;
uint32_t g_data_busy_count = 0;      // answer MIDIERR_NOTREADY this many times before accepting
std::atomic<bool> g_data_busy_forever{ false };
uint32_t g_data_result = 0;
bool g_complete_long = true;
typedef void(CALLBACK* Callback)(HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
Callback g_callback = nullptr;
DWORD_PTR g_callback_instance = 0;
uint32_t g_flags_seen_at_longdata = 0;

// DD-094: a driver whose MODM_DATA stays inside the call while its queue is "full" (OmniMIDI waits like this), and a
// record of any reset or close that arrives while such a call is still inside.
std::mutex g_calls_mutex;
std::atomic<bool> g_data_blocks{ false };
std::atomic<int> g_data_inside{ 0 };
std::atomic<int> g_control_during_data{ 0 };
std::atomic<int> g_busy_answers{ 0 };
std::atomic<bool> g_reset_blocks{ false };
std::atomic<bool> g_close_blocks{ false };
std::atomic<int> g_control_inside{ 0 };

uint32_t __stdcall fake_mod(uint32_t device, uint32_t message, uintptr_t instance, uintptr_t p1, uintptr_t p2)
{
    Call call = { device, message, instance, p1, p2 };
    {
        std::lock_guard<std::mutex> lock(g_calls_mutex);
        g_calls.push_back(call);
    }
    if ((message == drv::kModReset || message == drv::kModClose || message == drv::kModOpen) && g_data_inside.load() > 0)
        g_control_during_data.fetch_add(1);
    if (message == drv::kModData && g_data_blocks.load()) {
        g_data_inside.fetch_add(1);
        while (g_data_blocks.load()) Sleep(1);
        g_data_inside.fetch_sub(1);
        return 0;
    }
    if (message == drv::kModData && g_data_busy_forever) { g_busy_answers.fetch_add(1); return 67; }
    // DD-094: a driver whose MODM_RESET or MODM_CLOSE does not return (here: until released, at most 5 s)
    if ((message == drv::kModReset && g_reset_blocks.load()) || (message == drv::kModClose && g_close_blocks.load())) {
        std::atomic<bool>& flag = message == drv::kModReset ? g_reset_blocks : g_close_blocks;
        g_control_inside.fetch_add(1);
        for (DWORD start = GetTickCount(); flag.load() && GetTickCount() - start < 5000;) Sleep(1);
        g_control_inside.fetch_sub(1);
        return 0;
    }
    switch (message) {
    case drv::kModOpen: {
        const FakeOpenDesc* d = reinterpret_cast<const FakeOpenDesc*>(p1);
        g_callback = reinterpret_cast<Callback>(d->callback);
        g_callback_instance = d->instance;
        if (g_open_result_code == 0) *reinterpret_cast<uintptr_t*>(instance) = g_open_instance_value;
        return g_open_result_code;
    }
    case drv::kModData:
        if (g_data_busy_forever) return 67;
        if (g_data_busy_count > 0) { --g_data_busy_count; return 67; }
        return g_data_result;
    case drv::kModPrepare: return g_prepare_result;
    case drv::kModLongData: {
        MIDIHDR* h = reinterpret_cast<MIDIHDR*>(p1);
        g_flags_seen_at_longdata = h->dwFlags;
        if (g_longdata_result != 0) return g_longdata_result;
        g_sysex_seen.push_back(std::vector<uint8_t>(reinterpret_cast<uint8_t*>(h->lpData), reinterpret_cast<uint8_t*>(h->lpData) + h->dwBufferLength));
        if (g_complete_long) {
            h->dwFlags |= MHDR_DONE;
            g_callback(nullptr, 0x3C9, g_callback_instance, p1, 0);   // MOM_DONE
        }
        return 0;
    }
    default: return 0;
    }
}

int count_calls(uint32_t message)
{
    int n = 0;
    for (size_t i = 0; i < g_calls.size(); ++i) if (g_calls[i].message == message) ++n;
    return n;
}

DirectResolution fake_resolution()
{
    DirectResolution r;
    r.ok = true;
    r.mod_message = &fake_mod;
    r.local_index = 3;
    r.device_name = "Fake Synth";
    r.driver_file = "Fake.dll";
    r.verified = false;
    r.lifecycle = "fake";
    return r;
}

void reset_fake()
{
    g_calls.clear();
    g_sysex_seen.clear();
    g_open_instance_value = 0x1234;
    g_open_result_code = 0;
    g_prepare_result = 8;
    g_longdata_result = 0;
    g_data_busy_count = 0;
    g_data_busy_forever = false;
    g_data_result = 0;
    g_complete_long = true;
    g_callback = nullptr;
    g_data_blocks = false;
    g_data_inside = 0;
    g_control_during_data = 0;
    g_busy_answers = 0;
    g_reset_blocks = false;
    g_close_blocks = false;
}

// Releases a blocked fake reset/close and waits until the helper thread has left the fake driver.
void release_blocked_control()
{
    g_reset_blocks = false;
    g_close_blocks = false;
    for (int i = 0; i < 2000 && g_control_inside.load() != 0; ++i) Sleep(1);
}

std::wstring fake_error_text(uint32_t code) { return L"fake text " + std::to_wstring(code); }

void test_direct_output()
{
    reset_fake();
    {
        std::shared_ptr<int> released = std::make_shared<int>(0);
        WinMmDirectOutput out([&](uint32_t id) {
            DirectResolution r = fake_resolution();
            r.driver = released;
            check(id == 9, "output: the WinMM device id reaches the resolver");
            return r;
        }, fake_error_text);
        check(!out.is_open(), "output: closed at first");
        OutputStatus s = out.send_short(0x90);
        check(s.kind == OutputErrorKind::InvalidOperation, "output: sending while closed fails");
        s = out.open(9);
        check(s.ok() && out.is_open() && out.driver_file() == "Fake.dll", "output: opens through the resolver");
        check(g_calls.size() == 1 && g_calls[0].message == drv::kModOpen && g_calls[0].device == 3, "output: MODM_OPEN to the driver's own device number");
        check(g_calls[0].p2 == 0x00030000, "output: MODM_OPEN asks for CALLBACK_FUNCTION");
        check(g_callback != nullptr && g_callback_instance != 0, "output: the open descriptor carries this program's callback and instance token");
        check(out.path_description().find("Fake.dll") != std::string::npos && out.path_description().find("unverified") != std::string::npos, "output: the status text names the driver and says unverified");

        g_calls.clear();
        s = out.send_short(0x0014B0);
        check(s.ok() && g_calls.size() == 1 && g_calls[0].message == drv::kModData && g_calls[0].p1 == 0x0014B0 && g_calls[0].instance == 0x1234 && g_calls[0].device == 3,
            "output: a short message is MODM_DATA with the instance value from MODM_OPEN");

        // SysEx: prepare answers "not supported" (VirtualMIDISynth): this side marks the header prepared
        g_calls.clear();
        std::vector<uint8_t> small = { 0xF0, 0x7D, 0xF7 };
        output::SystemExclusiveEvent e;
        e.status = 0xF0; e.data = small.data(); e.length = small.size();
        s = out.send_long(e);
        check(s.ok(), "output: a 3-byte SysEx is sent");
        check(g_calls.size() == 2 && g_calls[0].message == drv::kModPrepare && g_calls[1].message == drv::kModLongData, "output: MODM_PREPARE then MODM_LONGDATA");
        check((g_flags_seen_at_longdata & MHDR_PREPARED) != 0, "output: when the driver does not support prepare, the header is marked prepared before MODM_LONGDATA");
        check(g_sysex_seen.size() == 1 && g_sysex_seen[0] == small, "output: the driver received exactly F0 7D F7");
        check(out.long_completions() == 1, "output: MOM_DONE reached the callback and was counted");
        g_calls.clear();
        s = out.send_short(0x003C90);   // reclaims the completed buffer first
        check(count_calls(drv::kModUnprepare) == 1 && out.outstanding_long_messages() == 0, "output: a completed SysEx buffer is unprepared and freed at the next send");

        // a SysEx in two events (F0 ... then F7 continuation) is one packet
        g_calls.clear();
        g_sysex_seen.clear();
        std::vector<uint8_t> first = { 0xF0, 0x7D, 0x01 }, second = { 0x02, 0xF7 };
        output::SystemExclusiveEvent a, b;
        a.status = 0xF0; a.data = first.data(); a.length = first.size();
        b.status = 0xF7; b.data = second.data(); b.length = second.size();
        s = out.send_long(a);
        check(s.ok() && g_sysex_seen.empty(), "output: an unfinished SysEx is held back");
        s = out.send_long(b);
        check(s.ok() && g_sysex_seen.size() == 1 && g_sysex_seen[0] == std::vector<uint8_t>({ 0xF0, 0x7D, 0x01, 0x02, 0xF7 }), "output: the continuation completes one packet");

        // prepare supported by the driver (OmniMIDI): the driver sets the flag, this side does not
        g_prepare_result = 0;
        g_calls.clear();
        s = out.send_long(e);
        check(s.ok() && (g_flags_seen_at_longdata & MHDR_PREPARED) == 0, "output: when the driver prepares, this side does not touch the flag");

        // LONGDATA fails: unprepare, error text with the operation
        g_longdata_result = 65;   // MIDIERR_STILLPLAYING
        g_calls.clear();
        s = out.send_long(e);
        check(!s.ok() && s.native_code == 65 && s.message.find("MODM_LONGDATA") != std::string::npos && s.message.find("fake text 65") != std::string::npos,
            "output: a failing MODM_LONGDATA is an error naming the operation and the driver's code");
        // one unprepare for the previous, completed buffer (reclaimed first) and one for the buffer that just failed
        check(count_calls(drv::kModUnprepare) == 2 && out.outstanding_long_messages() == 0, "output: a failed MODM_LONGDATA unprepares and frees its buffer");
        g_longdata_result = 0;
        g_complete_long = false;
        g_calls.clear();
        s = out.send_long(e);
        check(s.ok() && out.outstanding_long_messages() >= 1, "output: a SysEx that has not completed stays outstanding");
        s = out.reset();
        check(s.ok() && count_calls(drv::kModReset) == 1 && out.outstanding_long_messages() == 0, "output: reset is MODM_RESET and frees every outstanding buffer");
        g_complete_long = true;

        // errors from MODM_DATA
        g_data_result = 11;
        s = out.send_short(0x90);
        check(!s.ok() && s.native_code == 11 && s.message.find("sending a short MIDI message") != std::string::npos && s.message.find("Fake Synth") != std::string::npos,
            "output: a failing MODM_DATA is an error naming the device and the driver's code");
        g_data_result = 0;

        // panic: 48 controller messages
        g_calls.clear();
        s = out.panic();
        check(s.ok() && count_calls(drv::kModData) == 48, "output: panic is 48 MODM_DATA messages");

        // MIDIERR_NOTREADY: wait and try again
        g_calls.clear();
        g_data_busy_count = 5;
        s = out.send_short(0x90);
        check(s.ok() && count_calls(drv::kModData) == 6 && out.busy_waits() == 1, "output: a busy driver (MIDIERR_NOTREADY) is retried until it accepts, once counted");
        g_calls.clear();
        s = out.close();
        check(s.ok() && !out.is_open(), "output: close");
        check(g_calls.size() >= 2 && g_calls[g_calls.size() - 2].message == drv::kModReset && g_calls[g_calls.size() - 1].message == drv::kModClose, "output: close sends MODM_RESET then MODM_CLOSE last");
        check(released.use_count() == 1, "output: the driver is released after close");
        g_calls.clear();
        s = out.close();
        check(s.ok() && g_calls.empty(), "output: closing twice does nothing");
        // late callbacks after close are harmless (the token is gone)
        if (g_callback) g_callback(nullptr, 0x3C9, g_callback_instance, 0, 0);
        check(true, "output: a callback after close is ignored");
    }
    // a driver that stays busy: the send fails after a bounded wait with the driver's own text
    {
        reset_fake();
        WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
        check(out.open(1).ok(), "busy: opens");
        g_data_busy_forever = true;
        DWORD start = GetTickCount();
        OutputStatus s = out.send_short(0x90);
        DWORD took = GetTickCount() - start;
        check(!s.ok() && s.native_code == 67 && took >= 1900 && took < 6000, "busy: a driver that stays busy for 2 s fails the send (took " + std::to_string(took) + " ms)");
        g_data_busy_forever = false;
        out.close();
    }
    // the driver refuses MODM_OPEN: not-supported status with the driver's text, driver released
    {
        reset_fake();
        std::shared_ptr<int> released = std::make_shared<int>(0);
        g_open_result_code = 4;   // MMSYSERR_ALLOCATED
        WinMmDirectOutput out([&](uint32_t) { DirectResolution r = fake_resolution(); r.driver = released; return r; }, fake_error_text);
        OutputStatus s = out.open(1);
        check(!s.ok() && s.kind == OutputErrorKind::NotSupported && s.message.find("Fake.dll") != std::string::npos && s.message.find("fake text 4") != std::string::npos && !out.is_open(),
            "open refused: fallback-style status naming the driver and its text");
        check(released.use_count() == 1, "open refused: the driver is released again");
    }
    // the resolver says no: NotSupported with its reason, verbatim
    {
        WinMmDirectOutput out([](uint32_t) { DirectResolution r; r.ok = false; r.reason = "because"; return r; }, fake_error_text);
        OutputStatus s = out.open(1);
        check(!s.ok() && s.kind == OutputErrorKind::NotSupported && s.message == "because" && !out.is_open(), "unavailable device: NotSupported with the reason");
    }
    // verified driver text
    {
        reset_fake();
        WinMmDirectOutput out([](uint32_t) { DirectResolution r = fake_resolution(); r.verified = true; return r; }, fake_error_text);
        out.open(1);
        check(out.path_description().find("tested driver") != std::string::npos && out.path_description().find("unverified") == std::string::npos, "verified driver: status says tested");
        out.close();
    }
}

// DD-094: a close (Stop that gave up, the window closing, the next Play) while another thread is inside MODM_DATA of a
// "full" driver: no reset or close underneath that call, a bounded wait, nothing more sent, no second open, and the
// device reset and closed by that thread as soon as its call returns.
void test_close_while_driver_full()
{
    reset_fake();
    std::shared_ptr<int> released = std::make_shared<int>(0);
    {
        WinMmDirectOutput out([&](uint32_t) { DirectResolution r = fake_resolution(); r.driver = released; return r; }, fake_error_text);
        check(out.open(1).ok(), "full driver: opens");
        out.set_close_wait_milliseconds(200);
        g_calls.clear();
        g_data_blocks = true;
        OutputStatus sent;
        std::thread sender([&] { sent = out.send_short(0x0014B0); });
        for (int i = 0; i < 2000 && g_data_inside.load() == 0; ++i) Sleep(1);
        check(g_data_inside.load() == 1 && out.calls_in_progress() == 1, "full driver: the sender is inside MODM_DATA");

        DWORD start = GetTickCount();
        OutputStatus closed = out.close();
        DWORD took = GetTickCount() - start;
        check(!closed.ok() && closed.kind == OutputErrorKind::InvalidOperation && closed.message.find("has not returned") != std::string::npos,
            "full driver: close reports that a driver call has not returned (" + closed.message + ")");
        check(took >= 150 && took < 1500, "full driver: close returns after its bounded wait (took " + std::to_string(took) + " ms)");
        check(!out.is_open() && out.close_pending(), "full driver: the output counts as closed and the close is pending");
        check(count_calls(drv::kModReset) == 0 && count_calls(drv::kModClose) == 0 && g_control_during_data.load() == 0,
            "full driver: no MODM_RESET or MODM_CLOSE underneath the call in progress");

        // nothing more reaches the driver while the close is pending; no second open
        OutputStatus s = out.send_short(0x003C90);
        check(!s.ok() && s.kind == OutputErrorKind::InvalidOperation && count_calls(drv::kModData) == 1, "full driver: a send after the close request is refused and not sent");
        check(out.panic().ok() && out.reset().ok() && count_calls(drv::kModData) == 1 && count_calls(drv::kModReset) == 0,
            "full driver: panic and reset do nothing while the close is pending (the close resets)");
        s = out.open(1);
        check(!s.ok() && count_calls(drv::kModOpen) == 0 && out.close_pending(), "full driver: open is refused while the close is pending, MODM_OPEN not sent");
        check(out.close().ok() == false && out.close_pending(), "full driver: a second close also returns at once");

        g_data_blocks = false;   // the driver's queue drains; the call returns
        sender.join();
        check(sent.ok(), "full driver: the call that was inside completes normally");
        check(!out.close_pending() && count_calls(drv::kModReset) == 1 && count_calls(drv::kModClose) == 1 && g_control_during_data.load() == 0,
            "full driver: the sender's thread reset and closed the device after its call returned");
        check(g_calls.size() >= 2 && g_calls[g_calls.size() - 2].message == drv::kModReset && g_calls.back().message == drv::kModClose, "full driver: MODM_RESET then MODM_CLOSE, last");
        check(released.use_count() == 1, "full driver: the driver is released after the deferred close");

        // afterwards the device opens again and closes at once
        check(out.open(1).ok() && out.is_open(), "full driver: opens again once the deferred close is done");
        check(out.close().ok() && !out.close_pending(), "full driver: an ordinary close is immediate");
    }
    // a driver that keeps answering "busy" (MIDIERR_NOTREADY): a close stops the retrying at once
    {
        reset_fake();
        WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
        check(out.open(1).ok(), "busy retry: opens");
        out.set_close_wait_milliseconds(1500);
        g_data_busy_forever = true;
        OutputStatus sent;
        DWORD sendTook = 0;
        std::thread sender([&] { DWORD a = GetTickCount(); sent = out.send_short(0x90); sendTook = GetTickCount() - a; });
        for (int i = 0; i < 2000 && g_busy_answers.load() < 3; ++i) Sleep(1);
        DWORD start = GetTickCount();
        OutputStatus closed = out.close();
        DWORD took = GetTickCount() - start;
        sender.join();
        g_data_busy_forever = false;
        check(closed.ok() && took < 1000, "busy retry: the close waits only until the retrying stops (took " + std::to_string(took) + " ms)");
        check(!sent.ok() && sent.message.find("closed while the driver was still busy") != std::string::npos && sendTook < 1500,
            "busy retry: the send gives up when the close is asked for (" + sent.message + ", " + std::to_string(sendTook) + " ms)");
        check(g_calls.size() >= 2 && g_calls[g_calls.size() - 2].message == drv::kModReset && g_calls.back().message == drv::kModClose && g_control_during_data.load() == 0,
            "busy retry: then MODM_RESET and MODM_CLOSE");
    }
}

// DD-094 (second part): a driver whose MODM_CLOSE or MODM_RESET never returns (OmniMIDI was seen to hang in the close
// step): the call is given up after its time limit, nothing more is sent, the driver is never called again in this
// process (not by this output, not by a new one), and the helper thread keeps the driver loaded until it returns.
void test_driver_stops_answering()
{
    // MODM_CLOSE does not return
    {
        reset_fake();
        forget_unresponsive_drivers();
        std::shared_ptr<int> released = std::make_shared<int>(0);
        WinMmDirectOutput out([&](uint32_t) { DirectResolution r = fake_resolution(); r.driver = released; return r; }, fake_error_text);
        check(out.open(1).ok() && !out.driver_stopped_answering(), "hung close: opens");
        out.set_control_wait_milliseconds(200);
        out.set_track_data_time(true);
        check(out.milliseconds_since_last_data() == -1, "hung close: no MODM_DATA yet");
        check(out.send_short(0x0014B0).ok() && out.milliseconds_since_last_data() >= 0, "hung close: the last MODM_DATA time is kept");
        g_close_blocks = true;
        DWORD start = GetTickCount();
        OutputStatus s = out.close();
        DWORD took = GetTickCount() - start;
        check(!s.ok() && s.message.find("stopped answering") != std::string::npos && s.message.find("MODM_CLOSE") != std::string::npos,
            "hung close: close reports that the driver stopped answering in MODM_CLOSE (" + s.message + ")");
        check(took >= 150 && took < 1500, "hung close: close returns after the time limit (took " + std::to_string(took) + " ms)");
        check(count_calls(drv::kModReset) == 1 && count_calls(drv::kModClose) == 1, "hung close: MODM_RESET returned, MODM_CLOSE was the call that hung");
        check(out.driver_stopped_answering() && out.stopped_answering_call() == drv::kModClose && !out.is_open(), "hung close: the output says the driver stopped answering");
        check(out.stopped_answering_after_data_milliseconds() >= 0 && out.stopped_answering_after_data_milliseconds() < 1000,
            "hung close: the time from the last MODM_DATA to the hung call is recorded (" + std::to_string(out.stopped_answering_after_data_milliseconds()) + " ms)");
        std::string what;
        check(driver_unresponsive(L"FAKE.DLL", &what) && what.find("MODM_CLOSE did not return") != std::string::npos, "hung close: the driver is recorded process-wide (any case)");
        check(released.use_count() == 2, "hung close: the helper thread still inside the driver keeps it loaded; the output let go of it");

        g_calls.clear();
        s = out.open(1);
        check(!s.ok() && s.kind == OutputErrorKind::NotSupported && s.message.find("stopped answering earlier in this session") != std::string::npos &&
            count_calls(drv::kModOpen) == 0, "hung close: the driver is not opened again (" + s.message + ")");
        check(out.driver_stopped_answering(), "hung close: still reported after the refused open (status line)");
        check(!out.send_short(0x90).ok() && out.panic().ok() && out.reset().ok() && g_calls.empty(), "hung close: nothing more reaches the driver");
        WinMmDirectOutput other([&](uint32_t) { DirectResolution r = fake_resolution(); r.driver = released; return r; }, fake_error_text);
        s = other.open(1);
        check(!s.ok() && s.kind == OutputErrorKind::NotSupported && count_calls(drv::kModOpen) == 0, "hung close: a new output does not open that driver either");
        release_blocked_control();
        for (int i = 0; i < 1000 && released.use_count() != 1; ++i) Sleep(1);
        check(released.use_count() == 1, "hung close: once the call returns, the helper thread lets go of the driver");
        forget_unresponsive_drivers();
    }
    // MODM_RESET does not return (Stop's reset on the engine's thread)
    {
        reset_fake();
        WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
        check(out.open(1).ok(), "hung reset: opens");
        out.set_control_wait_milliseconds(200);
        g_reset_blocks = true;
        DWORD start = GetTickCount();
        OutputStatus s = out.reset();
        DWORD took = GetTickCount() - start;
        check(!s.ok() && s.message.find("MODM_RESET") != std::string::npos && took < 1500 && out.driver_stopped_answering(),
            "hung reset: reset gives up after the time limit and says so (took " + std::to_string(took) + " ms)");
        g_calls.clear();
        check(out.panic().ok() && !out.send_short(0x90).ok() && g_calls.empty(), "hung reset: no all-notes-off or other message is sent afterwards");
        s = out.close();
        check(s.ok() && g_calls.empty() && !out.is_open(), "hung reset: close calls nothing (no MODM_CLOSE to a driver that stopped answering)");
        release_blocked_control();
        forget_unresponsive_drivers();
    }
    // a close that waits for a reset (on the engine's thread) which then does not return: the close calls nothing more
    {
        reset_fake();
        WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
        check(out.open(1).ok(), "hung reset during close: opens");
        out.set_control_wait_milliseconds(300);
        g_reset_blocks = true;
        OutputStatus resetStatus;
        std::thread engineThread([&] { resetStatus = out.reset(); });
        for (int i = 0; i < 2000 && g_control_inside.load() == 0; ++i) Sleep(1);
        OutputStatus closed = out.close();
        engineThread.join();
        check(!resetStatus.ok() && out.driver_stopped_answering(), "hung reset during close: the reset gives up");
        check(count_calls(drv::kModReset) == 1 && count_calls(drv::kModClose) == 0 && !out.is_open(),
            "hung reset during close: the close sends no second MODM_RESET and no MODM_CLOSE (" + closed.message + ")");
        release_blocked_control();
        forget_unresponsive_drivers();
    }
    // the comparison styles of the stress tool
    {
        reset_fake();
        WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
        check(out.open(1).ok(), "styles: opens");
        out.set_reset_style(WinMmDirectOutput::ResetStyle::NotesOff, 0);
        g_calls.clear();
        check(out.reset().ok() && count_calls(drv::kModReset) == 0 && count_calls(drv::kModData) == 48, "notesoff: reset is the 48 controller messages, no MODM_RESET");
        g_calls.clear();
        check(out.close().ok() && count_calls(drv::kModReset) == 0 && count_calls(drv::kModData) == 48 && !g_calls.empty() && g_calls.back().message == drv::kModClose,
            "notesoff: close sends the controller messages, then MODM_CLOSE, no MODM_RESET");
        check(out.open(1).ok(), "settle: opens");
        out.set_reset_style(WinMmDirectOutput::ResetStyle::DriverReset, 120);
        g_calls.clear();
        DWORD start = GetTickCount();
        check(out.reset().ok() && count_calls(drv::kModReset) == 1, "settle: reset is MODM_RESET");
        DWORD took = GetTickCount() - start;
        check(took >= 100, "settle: MODM_RESET waits the settle time first (took " + std::to_string(took) + " ms)");
        out.set_reset_style(WinMmDirectOutput::ResetStyle::DriverReset, 0);
        out.close();
    }
}

// ---------------------------------------------------------------- DD-110 (cheap send gate) and DD-111 (device kept open)

std::atomic<int> g_stress_inside{ 0 };
std::atomic<int> g_stress_control_under_send{ 0 };
std::atomic<int> g_stress_opens{ 0 }, g_stress_closes{ 0 };

uint32_t __stdcall stress_mod(uint32_t, uint32_t message, uintptr_t instance, uintptr_t, uintptr_t)
{
    if (message == drv::kModData) {
        g_stress_inside.fetch_add(1);
        for (volatile int spin = 0; spin < 20; ++spin) {}   // a few instructions inside the "driver"
        g_stress_inside.fetch_sub(1);
        return 0;
    }
    if (message == drv::kModReset || message == drv::kModClose || message == drv::kModOpen) {
        if (g_stress_inside.load() != 0) g_stress_control_under_send.fetch_add(1);   // a reset/close/open under a send
        if (message == drv::kModOpen) { g_stress_opens.fetch_add(1); *reinterpret_cast<uintptr_t*>(instance) = 0x55; }
        if (message == drv::kModClose) g_stress_closes.fetch_add(1);
    }
    return 0;
}

DirectResolution stress_resolution()
{
    DirectResolution r = fake_resolution();
    r.mod_message = &stress_mod;
    return r;
}

void test_cheap_gate_close_race()
{
    // The gate of send_short (one interlocked operation in, a plain store out): a close from another thread while the
    // sender is sending in a tight loop never runs a reset, close or open underneath a send; after close() returns,
    // nothing more is sent; and the pending close is always finished (never left behind).
    g_stress_inside = 0; g_stress_control_under_send = 0; g_stress_opens = 0; g_stress_closes = 0;
    int cyclesWithSends = 0, cleanCloses = 0, deferredCloses = 0;
    const int cycles = 300;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        WinMmDirectOutput out([](uint32_t) { return stress_resolution(); }, fake_error_text);
        if (!out.open(1).ok()) { check(false, "race: opens"); return; }
        std::atomic<bool> go{ false }, stop{ false };
        std::atomic<long> sent{ 0 };
        std::atomic<long> refusedAfterClose{ 0 };
        const bool batches = (cycle & 2) != 0;   // cycles 2, 3, 6, 7 ...: the sender brackets 64 sends at a time with a batch
        std::thread sender([&] {
            while (!go.load()) {}
            bool refused = false;
            while (!stop.load() && !refused) {
                if (batches) out.begin_send_batch();
                for (int i = 0; i < (batches ? 64 : 1) && !refused; ++i) {
                    if (out.send_short(0x0014B0 + (static_cast<uint32_t>(sent.load()) & 0x7F)).ok()) ++sent;
                    else { ++refusedAfterClose; refused = true; }   // the first refused send ends this sender
                }
                if (batches) out.end_send_batch();
            }
        });
        if (cycle & 1) out.set_close_wait_milliseconds(0);   // odd cycles: the close does not wait, so it is often left to the sender
        go = true;
        for (int w = 0; w < 5000 && sent.load() < 20 + (cycle % 7) * 50; ++w) YieldProcessor();
        OutputStatus closed = out.close();
        stop = true;
        sender.join();
        if (sent.load() > 0) ++cyclesWithSends;
        if (closed.ok()) ++cleanCloses;
        else ++deferredCloses;
        const bool finished = !out.close_pending();
        const int closesNow = g_stress_closes.load();
        if (!finished || closesNow != cycle + 1) { check(false, "race: cycle " + std::to_string(cycle) + " close finished=" + (finished ? "yes" : "no") + " closes=" + std::to_string(closesNow)); break; }
        if (g_stress_control_under_send.load() != 0) { check(false, "race: a reset/close ran under a send in cycle " + std::to_string(cycle)); break; }
    }
    check(g_stress_control_under_send.load() == 0, "race: no reset, close or open ever ran underneath a send (" + std::to_string(cycles) + " cycles)");
    check(g_stress_closes.load() == cycles && g_stress_opens.load() == cycles, "race: every cycle opened once and its close was finished exactly once (opens " +
        std::to_string(g_stress_opens.load()) + ", closes " + std::to_string(g_stress_closes.load()) + ")");
    check(cyclesWithSends > cycles / 2, "race: the sender was sending in most cycles (" + std::to_string(cyclesWithSends) + " of " + std::to_string(cycles) + "; " +
        std::to_string(deferredCloses) + " closes were left to the sender, " + std::to_string(cleanCloses) + " done by close())");
}

void test_cheap_gate_counts()
{
    reset_fake();
    WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
    check(out.open(1).ok() && out.calls_in_progress() == 0, "gate: no call in progress after open");
    for (int i = 0; i < 1000; ++i) out.send_short(0x0014B0);
    check(out.calls_in_progress() == 0, "gate: nothing left counted after 1000 sends");
    check(out.close().ok() && !out.send_short(0x0014B0).ok() && out.calls_in_progress() == 0, "gate: a refused send leaves nothing counted");

    // a batch holds the gate once for all its sends
    check(out.open(1).ok(), "batch: opens");
    out.begin_send_batch();
    bool allOk = true;
    for (int i = 0; i < 500; ++i) allOk = out.send_short(0x0014B0).ok() && allOk;
    check(allOk && out.calls_in_progress() == 1, "batch: 500 sends under one entry (one call counted while the batch is open)");
    out.end_send_batch();
    check(out.calls_in_progress() == 0 && out.send_short(0x0014B0).ok() && out.calls_in_progress() == 0, "batch: nothing left counted after it ends; single sends work again");
    out.end_send_batch();   // an extra end does nothing
    check(out.calls_in_progress() == 0, "batch: an extra end_send_batch does nothing");

    // a close from another thread during a batch: the batch's sends are refused from then on, the close is not made
    // underneath a send, and it completes when the batch ends
    reset_fake();
    g_calls.clear();
    std::atomic<bool> inBatch{ false }, closeAsked{ false };
    std::atomic<long> okSends{ 0 }, refusedSends{ 0 };
    std::thread sender([&] {
        out.begin_send_batch();
        inBatch = true;
        while (true) {
            if (out.send_short(0x0014B0).ok()) ++okSends;
            else { ++refusedSends; break; }
        }
        out.end_send_batch();
    });
    for (int i = 0; i < 2000 && !inBatch.load(); ++i) Sleep(1);
    Sleep(20);
    OutputStatus closed = out.close();
    sender.join();
    check(closed.ok() && refusedSends.load() == 1 && okSends.load() > 0, "batch: close() during a batch waits for the batch, the next send is refused, close succeeds (" +
        std::to_string(okSends.load()) + " sends before it)");
    check(!out.close_pending() && g_calls.size() >= 2 && g_calls[g_calls.size() - 2].message == drv::kModReset && g_calls.back().message == drv::kModClose &&
        g_control_during_data.load() == 0, "batch: MODM_RESET and MODM_CLOSE came last, after the sends");
}

// The cheap success path (scan P11 item 1): try_send_short sends the same MODM_DATA, builds no status when the send
// works, and on a failure stores exactly the status send_short returns (same kind, code and text).
void test_try_send_short()
{
    reset_fake();
    WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
    OutputStatus failure;
    check(!out.try_send_short(0x90, failure) && failure.kind == OutputErrorKind::InvalidOperation &&
        failure.message == out.send_short(0x90).message, "try_send_short: closed output gives send_short's status text");
    check(out.open(1).ok(), "try_send_short: opens");
    g_calls.clear();
    failure = OutputStatus::failure(OutputErrorKind::Win32, "left alone", 5);
    check(out.try_send_short(0x0014B0, failure) && g_calls.size() == 1 && g_calls[0].message == drv::kModData && g_calls[0].p1 == 0x0014B0,
        "try_send_short: one MODM_DATA, returns true");
    check(failure.kind == OutputErrorKind::Win32 && failure.message == "left alone", "try_send_short: a success does not touch the status");
    g_data_result = 11;
    OutputStatus expected = out.send_short(0x90);
    check(!out.try_send_short(0x90, failure) && failure.kind == expected.kind && failure.native_code == expected.native_code &&
        failure.message == expected.message && failure.inner == expected.inner && failure.native_code == 11,
        "try_send_short: a failing MODM_DATA stores the same status as send_short");
    g_data_result = 0;
    out.begin_send_batch();
    bool allTrue = true;
    for (int i = 0; i < 100; ++i) allTrue = out.try_send_short(0x0014B0, failure) && allTrue;
    out.end_send_batch();
    check(allTrue && out.calls_in_progress() == 0, "try_send_short: inside a batch, and nothing is left counted");
    // through the base class: the default implementation of an output that only has send_short
    midibn::platform::MidiOutput& base = out;
    check(base.try_send_short(0x0014B0, failure), "try_send_short: reachable through the interface");
    out.close();
    check(!base.try_send_short(0x0014B0, failure) && failure.kind == OutputErrorKind::InvalidOperation, "try_send_short: after close it is refused");
}

void test_keep_open_across_stop()
{
    reset_fake();
    {
        WinMmDirectOutput out([](uint32_t) { return fake_resolution(); }, fake_error_text);
        check(out.open(1).ok() && out.is_open() && count_calls(drv::kModOpen) == 1, "keep open: first open reaches the driver once");
        for (int cycle = 0; cycle < 6; ++cycle) {
            check(out.send_short(0x0014B0).ok() && out.send_short(0x003C90).ok(), "keep open: playing");
            check(out.reset().ok() && out.panic().ok(), "keep open: Stop's reset and all-notes-off messages");
            check(out.open(1).ok() && out.is_open(), "keep open: the next Play's open succeeds");
        }
        check(count_calls(drv::kModOpen) == 1 && count_calls(drv::kModClose) == 0 && out.kept_open_count() == 6,
            "keep open: six Play/Stop cycles caused no MODM_CLOSE and no second MODM_OPEN");
        check(count_calls(drv::kModReset) == 6, "keep open: each Stop still reset the device (MODM_RESET)");
        // a different device: closed and opened again
        check(out.open(2).ok() && count_calls(drv::kModOpen) == 2 && count_calls(drv::kModClose) == 1 && out.kept_open_count() == 6,
            "keep open: a different device closes the first and opens the second");
        // an explicit close (a path change, exit) really closes; the next open opens again
        check(out.close().ok() && !out.is_open() && count_calls(drv::kModClose) == 2, "keep open: close() closes the device");
        check(out.open(2).ok() && count_calls(drv::kModOpen) == 3 && out.kept_open_count() == 6, "keep open: opening after a close opens again");
        // a sender stuck in the driver: the close is still bounded and deferred, and open() does not reopen under it
        out.set_close_wait_milliseconds(100);
        g_data_blocks = true;
        std::thread sender([&] { out.send_short(0x0014B0); });
        for (int i = 0; i < 2000 && g_data_inside.load() == 0; ++i) Sleep(1);
        const int opensBefore = count_calls(drv::kModOpen), closesBefore = count_calls(drv::kModClose);
        check(out.open(2).ok() && count_calls(drv::kModOpen) == opensBefore && count_calls(drv::kModClose) == closesBefore,
            "keep open: open() of the same device while a send is inside the driver neither closes nor reopens");
        check(!out.close().ok() && out.close_pending(), "keep open: a close during that send is bounded and left to the sender (DD-094 unchanged)");
        g_data_blocks = false;
        sender.join();
        check(!out.close_pending() && count_calls(drv::kModClose) == closesBefore + 1, "keep open: the sender finished the close");
    }
}

} // namespace

int main()
{
    test_entry_policy();
    test_matching();
    test_startup_sequence();
    test_direct_output();
    test_close_while_driver_full();
    test_driver_stops_answering();
    test_cheap_gate_counts();
    test_try_send_short();
    test_keep_open_across_stop();
    test_cheap_gate_close_race();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
