// midibn-large-address-probe: checks that the 32-bit programs (large-address-aware, DD-060) still talk to a MIDI output
// correctly when the memory they use lies ABOVE 2 GB (a 32-bit pointer whose top bit is set).
//
// Why: a 32-bit program marked large-address-aware gets addresses up to 4 GB on a 64-bit Windows. A pointer handed to a
// MIDI driver (the SysEx buffer and its header) can then have the top bit set; a driver or a piece of our own code that
// treats it as a signed number would break. This tool makes that case happen on purpose and checks every answer.
//
//   midibn-large-address-probe [--path none|fake-direct|standard|kdmapi|direct] [--device ID] [--list]
//                              [--no-exhaust] [--out FILE]
//     --path none         (default) the None output: no device, no driver, no sound.
//     --path fake-direct  the real direct-output code on an in-process no-op driver: no device, no driver, no sound.
//     --path standard     standard WinMM (midiOutOpen ...) on --device ID.        } these three open a REAL output:
//     --path kdmapi       KDMAPI (OmniMIDI's own stream; --device is ignored).    } run them only silently, with the
//     --path direct       direct WinMM driver access on --device ID.              } driver set up for no sound.
//     --list              print the WinMM output devices (id and name) and exit; opens nothing.
//     --no-exhaust        do not fill the low 2 GB first (the probe then only reports where memory landed).
//
// Steps: (1) reserve address space below 2 GB until a new reservation can only be placed above 2 GB (checked by printing
// pointer values); (2) route every operator new of this program to a private heap created afterwards (so the output
// classes' own buffers, the MIDIHDR and the SysEx copy, land above 2 GB too, counted and checked); (3) open the output,
// send controller 121 (reset all controllers) and 123 (all notes off) on channel 1, then a SysEx "GM System On"
// (F0 7E 7F 09 01 F7) taken from a buffer that lies above 2 GB, then reset and panic, check every return code, close.
// The only messages are silent ones. Exit code 0 = all steps passed (or the situation cannot be tested here, said so).
#include "midibn/midi_output.h"
#include "midibn/null_output.h"
#include "midi_driver_discovery.h"
#include "tool_support.h"
#include "win32_platform.h"
#include "winmm_direct_output.h"
#include "winmm_midi_output.h"
#include "kdmapi_midi_output.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <string>
#include <vector>

using namespace midibn;

namespace {

const uintptr_t kTwoGiB = 0x80000000u;

// ------------------------------------------------------------ allocation routing and counting

HANDLE g_high_heap = nullptr;
std::atomic<bool> g_armed(false);
std::atomic<long> g_low_allocations(0), g_high_allocations(0);

struct AllocHeader { HANDLE heap; size_t pad[3]; };   // 16 bytes on x86 and 32 on x64: keeps the user block aligned

void* counted_alloc(size_t size)
{
    HANDLE heap = g_armed.load() && g_high_heap ? g_high_heap : GetProcessHeap();
    AllocHeader* block = static_cast<AllocHeader*>(HeapAlloc(heap, 0, size + sizeof(AllocHeader)));
    if (!block) return nullptr;
    block->heap = heap;
    void* user = block + 1;
    if (g_armed.load()) {
        if (reinterpret_cast<uintptr_t>(user) >= kTwoGiB) g_high_allocations.fetch_add(1);
        else g_low_allocations.fetch_add(1);
    }
    return user;
}

void counted_free(void* user)
{
    if (!user) return;
    AllocHeader* block = static_cast<AllocHeader*>(user) - 1;
    HeapFree(block->heap, 0, block);
}

} // namespace

void* operator new(size_t size) { void* p = counted_alloc(size); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { void* p = counted_alloc(size); if (!p) throw std::bad_alloc(); return p; }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

namespace {

class Report {
public:
    explicit Report(const std::string& path)
    {
        if (!path.empty() && _wfopen_s(&file_, win32::utf8_to_wide(path).c_str(), L"wb") != 0) file_ = nullptr;
    }
    ~Report() { if (file_) std::fclose(file_); }
    void line(const std::string& text = std::string())
    {
        tools::write_stdout(text + "\n");
        if (file_) { std::string out = text + "\r\n"; std::fwrite(out.data(), 1, out.size(), file_); std::fflush(file_); }
    }
private:
    FILE* file_ = nullptr;
};

std::string hex(uintptr_t value)
{
    char b[32];
    std::snprintf(b, sizeof b, "0x%08llX", static_cast<unsigned long long>(value));
    return b;
}

bool image_is_large_address_aware()
{
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

// Reserves address space (no memory is committed) from the bottom until a new reservation can only be placed at or above
// 2 GB. Returns the first address at or above 2 GB that a new 64 KB reservation got; 0 when there is no such address.
uintptr_t exhaust_low_addresses(size_t& reservedMiB, size_t& pieces)
{
    reservedMiB = 0;
    pieces = 0;
    static const size_t sizes[] = { 256u << 20, 64u << 20, 16u << 20, 4u << 20, 1u << 20, 256u << 10, 64u << 10 };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        for (;;) {
            void* p = VirtualAlloc(nullptr, sizes[s], MEM_RESERVE, PAGE_NOACCESS);
            if (!p) break;   // no hole of this size anywhere (and none above 2 GB either)
            if (reinterpret_cast<uintptr_t>(p) >= kTwoGiB) {   // the low area has no hole of this size left
                VirtualFree(p, 0, MEM_RELEASE);
                break;
            }
            reservedMiB += sizes[s] >> 20;
            ++pieces;
        }
    }
    void* probe = VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!probe) return 0;
    const uintptr_t address = reinterpret_cast<uintptr_t>(probe);
    VirtualFree(probe, 0, MEM_RELEASE);
    return address >= kTwoGiB ? address : 0;
}

struct Steps {
    int failed = 0;
    std::string first;
    void note(const platform::OutputStatus& s, const std::string& what)
    {
        if (s.ok()) return;
        ++failed;
        if (first.empty()) first = what + ": " + s.message;
    }
};

// A no-op driver entry point for --path fake-direct (the same idea as the direct check's --fake-driver).
uint32_t __stdcall probe_mod_message(uint32_t, uint32_t message, uintptr_t instance, uintptr_t, uintptr_t)
{
    if (message == win32::drv::kModOpen) *reinterpret_cast<uintptr_t*>(instance) = 0x1234;
    return 0;
}

win32::DirectResolution probe_resolution(uint32_t)
{
    win32::DirectResolution r;
    r.ok = true;
    r.mod_message = &probe_mod_message;
    r.device_name = "in-process no-op driver";
    r.driver_file = "FAKE-NOOP.DLL";
    r.verified = true;
    r.lifecycle = "none";
    return r;
}

int probe_main(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    std::string path = "none", outFile = tools::executable_directory() + "\\large-address-probe-result.txt";
    int device = -1;
    bool list = false, exhaust = true;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--path" && i + 1 < args.size()) path = args[++i];
        else if (args[i] == "--device" && i + 1 < args.size()) device = std::atoi(args[++i].c_str());
        else if (args[i] == "--out" && i + 1 < args.size()) outFile = args[++i];
        else if (args[i] == "--list") list = true;
        else if (args[i] == "--no-exhaust") exhaust = false;
        else { std::fprintf(stderr, "unknown option %s\n(see the comment at the top of tools\\large_address_probe_main.cpp)\n", args[i].c_str()); return 2; }
    }
    Report r(outFile);
    r.line("== midibn-large-address-probe 1 ==");
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    r.line(std::string("program: ") + (sizeof(void*) == 8 ? "64-bit" : "32-bit") + (win32::running_under_wow64() ? " on 64-bit Windows (WOW64)" : "") +
        ", large-address-aware flag " + (image_is_large_address_aware() ? "set" : "NOT set") +
        ", highest usable address " + hex(reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress)));
    r.line("windows: " + win32::describe_os());
    if (list) {
        std::vector<platform::OutputDeviceInfo> devices = win32::WinMmMidiOutput::enumerate_devices();
        for (size_t i = 0; i < devices.size(); ++i) r.line("device " + std::to_string(devices[i].device_id) + ": " + devices[i].name);
        r.line("PROBE: LISTED");
        return 0;
    }
    if (path != "none" && path != "fake-direct" && path != "standard" && path != "kdmapi" && path != "direct") {
        r.line("unknown --path " + path);
        return 2;
    }
    if ((path == "standard" || path == "direct") && device < 0) {
        r.line("--path " + path + " needs --device ID (see --list). Nothing was opened.");
        return 2;
    }

    // (1) fill the low 2 GB of address space
    uintptr_t firstHigh = 0;
    if (exhaust) {
        size_t mib = 0, pieces = 0;
        firstHigh = exhaust_low_addresses(mib, pieces);
        r.line("address space below 2 GB: " + std::to_string(mib) + " MiB reserved in " + std::to_string(pieces) + " pieces");
        if (firstHigh == 0) {
            r.line("A new 64 KB allocation still lands below 2 GB (or none is possible): this process has no address space above 2 GB");
            r.line("(a 32-bit Windows without the /3GB option, or the large-address-aware flag is not set). Nothing to test here.");
            r.line("PROBE: SKIPPED (no memory above 2 GB in this process)");
            return 0;
        }
        r.line("first new allocation after that: " + hex(firstHigh) + " (above 2 GB: yes)");
    }
    // (2) a heap created now lives above 2 GB; every operator new goes there from now on
    g_high_heap = HeapCreate(0, 0, 0);
    uint8_t* sysexBuffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_high_heap || !sysexBuffer) { r.line("could not create the high heap or the SysEx buffer"); r.line("PROBE: FAILED"); return 1; }
    static const uint8_t gmSystemOn[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    std::memcpy(sysexBuffer, gmSystemOn, sizeof gmSystemOn);
    g_armed.store(true);
    std::unique_ptr<int> heapProbe(new int(0));
    const bool bufferHigh = reinterpret_cast<uintptr_t>(sysexBuffer) >= kTwoGiB;
    const bool heapHigh = reinterpret_cast<uintptr_t>(heapProbe.get()) >= kTwoGiB;
    r.line("SysEx source buffer at " + hex(reinterpret_cast<uintptr_t>(sysexBuffer)) + " (above 2 GB: " + (bufferHigh ? "yes" : "NO") + ")");
    r.line("a plain 'new' returns " + hex(reinterpret_cast<uintptr_t>(heapProbe.get())) + " (above 2 GB: " + (heapHigh ? "yes" : "NO") + ")");
    heapProbe.reset();
    if (exhaust && (!bufferHigh || !heapHigh)) { r.line("PROBE: FAILED (memory did not land above 2 GB as arranged)"); return 1; }

    // (3) the output
    std::unique_ptr<platform::MidiOutput> output;
    if (path == "none") output.reset(new platform::NullMidiOutput());
    else if (path == "fake-direct") output.reset(new win32::WinMmDirectOutput(&probe_resolution));
    else if (path == "standard") output.reset(new win32::WinMmMidiOutput());
    else if (path == "kdmapi") output.reset(new win32::KdmApiMidiOutput());
    else output.reset(new win32::WinMmDirectOutput());
    r.line("path: " + path + " (" + output->backend_name() + ")" + (device >= 0 ? ", device " + std::to_string(device) : std::string()));

    Steps steps;
    platform::OutputStatus s = output->open(device < 0 ? 0u : static_cast<uint32_t>(device));
    steps.note(s, "open");
    if (s.ok()) {
        steps.note(output->send_short(0x0000B0u | (121u << 8)), "send controller 121 (reset all controllers)");
        steps.note(output->send_short(0x0000B0u | (123u << 8)), "send controller 123 (all notes off)");
        output::SystemExclusiveEvent e;
        e.status = 0xF0;
        e.data = sysexBuffer;
        e.length = sizeof gmSystemOn;
        steps.note(output->send_long(e), "send SysEx GM System On from a buffer above 2 GB");
        Sleep(200);   // lets the driver finish the SysEx buffer before reset / close
        steps.note(output->reset(), "reset");
        steps.note(output->panic(), "panic (controllers 120, 123, 64)");
        steps.note(output->close(), "close");
    }
    const long low = g_low_allocations.load(), high = g_high_allocations.load();
    r.line("program memory requests while the output was in use: " + std::to_string(high) + " above 2 GB, " + std::to_string(low) + " below");
    const bool pass = steps.failed == 0 && (!exhaust || low == 0);
    if (steps.failed) r.line("first failure: " + steps.first);
    r.line(std::string("RESULT path=") + path + " steps_failed=" + std::to_string(steps.failed) + " allocations_above_2gb=" + std::to_string(high) +
        " allocations_below_2gb=" + std::to_string(low) + " sysex_buffer=" + hex(reinterpret_cast<uintptr_t>(sysexBuffer)));
    r.line(pass ? "PROBE: PASS" : "PROBE: FAILED");
    return pass ? 0 : 1;
}

} // namespace

// The result is complete when probe_main returns; the process then ends at once (a driver's exit code must not hold it).
int wmain(int argc, wchar_t** argv)
{
    int code = probe_main(argc, argv);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
