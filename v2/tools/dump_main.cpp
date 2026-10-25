// midibn-dump: writes the canonical trace for one MIDI file.
//   midibn-dump <midi> [--summary] [--flow 4|8] [--out <trace>]
//   midibn-dump --models <v1 model trace> --fixtures <dir> [--out <trace>]
// --flow writes the load-flow trace for that pointer size instead of the
// load/index trace. --models replays a v1 model trace's ops against the v2
// models (TRACE_FORMAT.md, "Model traces").
#include "midibn/trace.h"
#include "tool_support.h"

int MIDIBN_TOOL_MAIN(int argc, midibn::tools::ToolChar** argv)
{
    using namespace midibn;
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: midibn-dump <midi> [--summary] [--flow 4|8] [--out <trace>]\n");
        return 2;
    }
    if (args[1] == "--models") {
        std::string script, fixtures, out;
        for (size_t i = 3; i + 1 < args.size(); i += 2) {
            if (args[i] == "--fixtures") fixtures = args[i + 1];
            else if (args[i] == "--out") out = args[i + 1];
            else { std::fprintf(stderr, "unknown option %s\n", args[i].c_str()); return 2; }
        }
        if (args.size() < 3 || !tools::read_file(args[2], script) || fixtures.empty()) {
            std::fprintf(stderr, "usage: midibn-dump --models <v1 model trace> --fixtures <dir> [--out <trace>]\n");
            return 2;
        }
        std::string trace = replay_model_trace(script, tools::fixture_loader(fixtures),
            "midibn-dump v2 " + tools::build_description());
        if (out.empty()) tools::write_stdout(trace);
        else if (!tools::write_file(out, trace)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 3; }
        return 0;
    }
    bool summary = false;
    int flowPointerSize = 0;
    std::string out;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--summary") summary = true;
        else if (args[i] == "--flow" && i + 1 < args.size()) flowPointerSize = std::atoi(args[++i].c_str());
        else if (args[i] == "--out" && i + 1 < args.size()) out = args[++i];
        else { std::fprintf(stderr, "unknown option %s\n", args[i].c_str()); return 2; }
    }
    if (flowPointerSize != 0 && flowPointerSize != 4 && flowPointerSize != 8) {
        std::fprintf(stderr, "--flow takes 4 or 8\n");
        return 2;
    }
    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(args[1], error);
    if (!source) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    std::string name = args[1].substr(args[1].find_last_of("\\/") == std::string::npos ? 0 : args[1].find_last_of("\\/") + 1);
    std::string producer = "midibn-dump v2 " + tools::build_description();
    std::string trace = flowPointerSize != 0 ? write_flow_trace(*source, name, summary, flowPointerSize, producer)
                                             : write_load_trace(*source, name, summary, producer);
    if (out.empty()) tools::write_stdout(trace);
    else if (!tools::write_file(out, trace)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 3; }
    return 0;
}
