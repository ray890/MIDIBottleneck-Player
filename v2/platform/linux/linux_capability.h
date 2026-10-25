// Linux capability report (Milestone 8, stage 3/4): what this machine can do, as measurements, not pass/fail. The shape
// follows the Windows capability probe (tools/capability_probe.h): a heading, numbered sections of human-readable lines,
// and "section.key=value" machine lines. It plays no sound, opens no MIDI port (it only checks whether /dev/snd/seq
// exists), shows no window and needs no privileges.
#pragma once

#include <string>
#include <vector>

namespace midibn {
namespace posix {

// Returns the human-readable lines (the first is "CAPABILITY REPORT (measurements, not pass/fail)"); `machine` receives the
// "key=value" lines. `quick` shortens the timing measurements.
std::vector<std::string> run_linux_capability_report(std::vector<std::string>& machine, bool quick = false);

} // namespace posix
} // namespace midibn
