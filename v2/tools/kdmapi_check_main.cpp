// midibn-kdmapi-check: a silent check of WHERE the player would find its KDMAPI provider (OmniMIDI.dll) on one PC
// (register entry DD-119).
//
// It shows the folders the player searches, in order (the application folder, the folder of each user-mode MIDI driver
// registered under Drivers32 - the same registry view and system-folder rule as "Direct WinMM driver access" - and the
// system folder), whether OmniMIDI.dll exists in each, which one the player would use, and whether that file is built
// for this program's architecture (the PE header is READ; the file is never loaded). It opens no MIDI device, loads no
// driver or provider, changes no setting, needs no administrator rights and writes one text file.
//
//   midibn-kdmapi-check [--out FILE]
//     --out FILE   result file (default: kdmapi-check-result.txt beside the program)
//
// The last line is "KDMAPI-CHECK: DONE". The answer is information only: "no provider found" is a normal result.
#include "kdmapi_midi_output.h"
#include "midi_path_util.h"
#include "tool_support.h"
#include "win32_platform.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace midibn;

namespace {

const char* kToolVersion = "midibn-kdmapi-check 1 (DD-119)";

class Report {
public:
    explicit Report(const std::string& path) : path_(path)
    {
        file_ = nullptr;
        if (!path.empty() && _wfopen_s(&file_, win32::utf8_to_wide(path).c_str(), L"wb") != 0) file_ = nullptr;
    }
    ~Report() { if (file_) std::fclose(file_); }
    void line(const std::string& text = std::string())
    {
        tools::write_stdout(text + "\n");
        std::string out = text + "\r\n";
        if (file_) { std::fwrite(out.data(), 1, out.size(), file_); std::fflush(file_); }
    }
    bool file_ok() const { return path_.empty() || file_ != nullptr; }
private:
    std::string path_;
    FILE* file_ = nullptr;
};

std::string hex4(unsigned value)
{
    char b[16];
    std::snprintf(b, sizeof b, "%04X", value);
    return b;
}

const char* machine_name(unsigned machine)
{
    return machine == 0x014C ? "x86" : machine == 0x8664 ? "x64" : machine == 0x0200 ? "Itanium" : machine == 0xAA64 ? "ARM64" : "other";
}

} // namespace

static int check_main(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    std::string out = tools::executable_directory() + "\\kdmapi-check-result.txt";
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else { std::fprintf(stderr, "unknown option %s\n(see the comment at the top of tools\\kdmapi_check_main.cpp)\n", args[i].c_str()); return 2; }
    }
    Report report(out);
    if (!report.file_ok()) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 3; }

    report.line(std::string("== ") + kToolVersion + " ==");
    report.line(std::string("program: ") + (sizeof(void*) == 8 ? "64-bit (x64)" : "32-bit (x86)") +
        (win32::running_under_wow64() ? " on 64-bit Windows (WOW64)" : ""));
    report.line("windows: " + win32::describe_os());
    win32::SystemFolders folders = win32::system_folders();
    report.line("system folder this program uses: " + win32::wide_to_utf8(folders.directory));
    report.line("application folder: " + win32::wide_to_utf8(win32::application_directory()));
    report.line();

    report.line("== Where the player looks for OmniMIDI.dll (file existence only; nothing is loaded) ==");
    std::vector<win32::KdmApiProviderCandidate> candidates = win32::kdmapi_provider_candidates();
    int firstFound = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        report.line(std::to_string(i + 1) + ". " + candidates[i].description + ": " + candidates[i].path + " - " +
            (candidates[i].exists ? "found" : "not found"));
        if (candidates[i].exists && firstFound < 0) firstFound = static_cast<int>(i);
    }
    report.line();

    report.line("== Result ==");
    if (firstFound < 0) {
        report.line("KDMAPI provider: NOT FOUND (the player would say \"No KDMAPI provider was found.\")");
    } else {
        const win32::KdmApiProviderCandidate& chosen = candidates[firstFound];
        report.line("KDMAPI provider: FOUND in the " + chosen.description + " (search position " + std::to_string(firstFound + 1) + ")");
        report.line("file: " + chosen.path);
        uint16_t machine = 0;
        platform::OutputStatus status = win32::DynamicKdmApiNative::read_pe_machine(win32::utf8_to_wide(chosen.path), machine);
        if (!status.ok()) {
            report.line("architecture: could not be read: " + status.message);
        } else {
            report.line(std::string("architecture of the file: ") + machine_name(machine) + " (PE machine 0x" + hex4(machine) + ")");
            platform::OutputStatus match = win32::DynamicKdmApiNative::validate_provider_architecture(win32::utf8_to_wide(chosen.path));
            report.line(match.ok() ? std::string("architecture check: OK, matches this program")
                                   : std::string("architecture check: DOES NOT MATCH this program: ") + match.message);
        }
        // Other copies further down the list, for information.
        for (size_t i = firstFound + 1; i < candidates.size(); ++i)
            if (candidates[i].exists) report.line("another copy further down the list: " + candidates[i].path);
    }
    report.line("result file: " + out);
    report.line("KDMAPI-CHECK: DONE");
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    return check_main(argc, argv);
}
