// Runs one (fixture, scenario) pair of the engine comparison with v1: a port
// of the v1 reference harness's RendezvousSpike.cs RunScenario/Drive. It
// creates a new v2 engine over the lock-step services, applies the setup
// lines, drives the scripted commands under the spike's protocol and returns
// the two recordings, byte-identical to the spike's .out.txt and .sched.txt.
// The protocol and every recorded line are specified in
// v2/harness/ENGINE_SCENARIOS.md sections 8 and 9.
#pragma once

#include "midibn/midi_loader.h"
#include "scenario_file.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace harness {

struct EngineRunResult {
    std::string out;              // the .out.txt bytes
    std::string sched;            // the .sched.txt bytes
    std::string error;            // empty when the run completed cleanly
    int64_t sends = 0;
    int64_t parks = 0;
    int64_t go_resumes = 0;
    int64_t wake_resumes = 0;
    int64_t spins = 0;
    int64_t yields = 0;
    int64_t waits = 0;
    int64_t control_waits = 0;
    int64_t control_timeouts = 0;
    int64_t virtual_end = 0;      // virtual microseconds after the scenario base
    int64_t real_milliseconds = 0;  // wall-clock time of the run (not part of any comparison)
    bool ok() const { return error.empty(); }
};

// The 23 valid fixtures of the Milestone 3 evidence matrix (file names).
const std::vector<std::string>& engine_fixture_names();

// "models-dense.mid" -> "models-dense".
std::string fixture_stem(const std::string& fileName);

// Runs `definition` on the loaded song. `fixtureFileName` is the fixture's
// file name (used for the song's path). Returns false when the run did not
// complete cleanly (result.error says why; the recordings are still filled).
bool run_engine_scenario(const std::shared_ptr<Song>& song, const std::string& fixtureFileName,
    const ScenarioDefinition& definition, EngineRunResult& result);

// The spike's per-run console summary line (without its wall-clock field),
// for comparing counters with the spike: scenario, fixture, 16-hex-digit
// prefixes of the SHA-256 of both recordings, counters.
std::string console_summary(const std::string& scenario, const std::string& fixtureFileName, const EngineRunResult& result);

} // namespace harness
} // namespace midibn
