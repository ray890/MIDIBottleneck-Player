// Capability probe: measures what this PC can do (processor, timers, threads,
// memory, large files, MIDI device names, fonts and screen). It reports
// measurements; it never decides pass or fail and never changes the parity
// result. It opens no MIDI output, plays no sound, shows no window, needs no
// administrator rights, and deletes every temporary file it creates.
//
// Windows XP SP3 compatible: anything newer is looked up with GetProcAddress
// at run time and reported "not available" when missing.
#pragma once

#include <string>
#include <vector>

namespace midibn {
namespace tools {

// Runs every probe and returns human-readable report lines (the first line is
// the heading "CAPABILITY REPORT (measurements, not pass/fail)"). `machine` is
// filled with "key=value" lines (ASCII). `packageFolder` is the folder used
// for the temporary-file tests (it must be writable; a failure is reported,
// not thrown). Never throws; a failing probe is reported as "error=<text>" and
// the others still run.
// `quick` shortens the disk throughput test (16 MB instead of 64 MB; the quick self-test uses it).
std::vector<std::string> run_capability_probe(const std::wstring& packageFolder, std::vector<std::string>& machine, bool quick = false);

} // namespace tools
} // namespace midibn
