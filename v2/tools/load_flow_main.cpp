// midibn-load-flow: one timed load THROUGH THE PLAYER'S LOAD FLOW in a fresh process (DD-055). midibn-perf times the
// parser alone; the player first inspects a big file (the preflight scan, for files of 15 MiB and more in a 32-bit
// build and 63 MiB and more in a 64-bit one) and only then parses it. This tool runs both steps as the player does and
// prints, with the same keys as midibn-perf, the time of the whole flow, of the inspection and of the parse:
//
//   midibn-load-flow [--force-preflight] <midi>
//
//   flow-ms        the whole load flow (inspection + parse), what the person waits for
//   preflight-ms   the inspection pass (0 when the file is below the size that needs one)
//   load-ms        the parse pass (the figure midibn-perf calls load-ms)
//
// The large-file question is answered "continue" automatically; nothing is played and no device is touched.
#include "midibn/midi_loader.h"
#include "tool_support.h"
#include "win32_platform.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

using namespace midibn;

// Time at which the flow first reaches each stage name.
class FlowTimer : public LoadMonitor {
public:
    int64_t inspection_end = 0;
    int64_t parse_begin = 0;
    void progress(const LoadProgress& p) override
    {
        const bool inspecting = p.stage == "Inspecting large MIDI" || p.stage == "Waiting for confirmation";
        if (!inspecting && parse_begin == 0) {
            parse_begin = win32::performance_counter();
            if (inspection_end == 0) inspection_end = parse_begin;
        }
        if (inspecting) inspection_end = win32::performance_counter();
    }
    bool cancellation_requested() override { return false; }
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    bool force = false;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--force-preflight") force = true;
        else { args.clear(); break; }
    }
    if (args.size() < 2) { std::fprintf(stderr, "usage: midibn-load-flow [--force-preflight] <midi>\n"); return 2; }
    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(args.back(), error);
    if (!source) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    std::unique_ptr<Song> song(new Song());
    LoadError loadError;
    FlowTimer timer;
    LoadFlowOptions options;
    options.monitor = &timer;
    options.force_preflight = force;
    options.decision = [](const PreflightCounts&, const MemoryProjection&) { return true; };
    LARGE_INTEGER frequencyValue;
    QueryPerformanceFrequency(&frequencyValue);
    const double frequency = static_cast<double>(frequencyValue.QuadPart);
    const int64_t start = win32::performance_counter();
    LoadFlowResult result = load_with_preflight(*source, static_cast<int>(sizeof(void*)), options, *song, loadError);
    const int64_t end = win32::performance_counter();
    if (result != LoadFlowResult::Loaded) {
        std::fprintf(stderr, "%s: %s\n", loadError.type.c_str(), loadError.message.c_str());
        return 1;
    }
    const bool inspected = timer.inspection_end != 0 && timer.parse_begin != 0 && timer.inspection_end > start;
    std::printf("implementation=v2-core-flow\n");
    std::printf("architecture=%s\n", sizeof(void*) == 8 ? "x64" : "x86");
    std::printf("build=%s\n", tools::build_description().c_str());
    std::printf("events=%lld\n", static_cast<long long>(song->store.count()));
    std::printf("flow-ms=%.1f\n", (end - start) * 1000.0 / frequency);
    std::printf("preflight-ms=%.1f\n", inspected ? (timer.parse_begin - start) * 1000.0 / frequency : 0.0);
    std::printf("load-ms=%.1f\n", inspected ? (end - timer.parse_begin) * 1000.0 / frequency : (end - start) * 1000.0 / frequency);
    win32::ProcessMemory memory = win32::process_memory();
    std::printf("peak-working-set-bytes=%llu\n", static_cast<unsigned long long>(memory.peak_working_set));
    std::printf("peak-private-bytes=%llu\n", static_cast<unsigned long long>(memory.peak_private));
    return 0;
}
