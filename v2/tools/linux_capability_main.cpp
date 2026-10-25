// midibn-linux-capability: the Linux capability report (measurements, not pass/fail). Opens no MIDI port, plays no sound.
//   midibn-linux-capability [--quick] [--machine]
// --machine also prints the "section.key=value" lines after the human-readable report.
#include "linux_capability.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    bool quick = false, machineOutput = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else if (std::strcmp(argv[i], "--machine") == 0) machineOutput = true;
        else { std::fprintf(stderr, "usage: midibn-linux-capability [--quick] [--machine]\n"); return 2; }
    }
    std::vector<std::string> machine;
    std::vector<std::string> lines = midibn::posix::run_linux_capability_report(machine, quick);
    for (size_t i = 0; i < lines.size(); ++i) std::printf("%s\n", lines[i].c_str());
    if (machineOutput) for (size_t i = 0; i < machine.size(); ++i) std::printf("%s\n", machine[i].c_str());
    return 0;
}
