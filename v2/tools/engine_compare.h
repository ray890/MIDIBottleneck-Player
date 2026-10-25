// The engine comparison shared by midibn-parity (and the XP self-test) and
// midibn-engine-harness: runs the committed engine scenarios
// (v2/harness/engine-scenarios.txt) on the v2 engine under the lock-step
// harness and checks the SHA-256 of every recording in two independent tiers:
//   1. against the digests of the v1 reference recordings (v2/harness/expected/engine/);
//      a difference must be explained by an engine rule of the difference rules
//      (v2/harness/expected/differences-from-v1.txt: a named normalizer applied to
//      v2's recording before hashing, or "explained");
//   2. against v2's own baseline (v2/harness/expected-v2/engine/), exactly,
//      whatever v1 says. See v2/harness/ENGINE_SCENARIOS.md and v2/harness/TRACE_FORMAT.md.
#pragma once

#include <string>
#include <vector>

namespace midibn {
namespace tools {

class DifferenceRules;

struct EngineCompareOptions {
    std::string fixtures_dir;             // the fixture files
    std::string scenario_file;            // engine-scenarios.txt
    std::string expected_dir;             // expected\engine: manifest.txt and <stem>.digests; empty = no digest comparison
    std::string v1_dir;                   // optional: a spike output directory (v1's recordings), for the first differing line
    std::string out_dir;                  // optional: write <scenario>__<stem>.out.txt / .sched.txt here
    bool write_only_differences = false;  // with out_dir: write only runs that differ from the digests
    std::string only;                     // scenario names, comma separated, trailing * = prefix
    std::vector<std::string> fixtures;    // fixture file names; empty = the 23 valid fixtures
    int sample_stride = 0;                // > 1: quick sample: scenario #j (file order) runs on fixture #i (list order) only when (i + j + sample_offset) % sample_stride == 0
    int sample_offset = 0;
    bool print_each_run = false;          // print the spike-style summary line of every run
    bool fail_fast = false;               // stop at the first differing or failed run (mutation check)
    DifferenceRules* rules = nullptr;     // the difference rules (engine kind); null = no difference is explained
    std::string baseline_dir;             // expected-v2\engine: v2's own digests, compared exactly; empty = skip
    std::string write_baseline_dir;       // write v2's digests here instead of comparing (full set only)
};

struct EngineCompareResult {
    int fixtures = 0;
    int runs = 0;                         // (fixture, scenario) pairs run
    int identical = 0;                    // pairs whose out and sched match (v1 digests, or explained) and the v2 baseline
    int mismatched_files = 0;             // out/sched files that differ from v1 without an explaining rule
    int explained_files = 0;              // out/sched files that differ from v1 and are explained by a rule of kind "explained"
    int baseline_mismatches = 0;          // out/sched files that differ from the v2 baseline
    int failed_runs = 0;                  // runs the harness reported an error for
    int setup_failures = 0;               // unreadable fixture, scenario file or digests
    int skipped_runs = 0;                 // (fixture, scenario) pairs left out by the quick sample
    double seconds = 0;
    bool pass() const {
        return setup_failures == 0 && failed_runs == 0 && mismatched_files == 0 && baseline_mismatches == 0 && runs > 0;
    }
};

// Prints one line per problem (lines starting with FAIL) and returns the totals.
EngineCompareResult run_engine_comparison(const EngineCompareOptions& options);

// SHA-256 (uppercase hex) of the scenario file text with CRLF/CR turned into LF.
std::string scenario_file_digest(const std::string& text);

// A short human-readable excerpt of the first line where two recordings differ (v1 text versus v2 text),
// with the line before it and both line counts. Used for evidence in the improvement register.
std::string describe_first_difference(const std::string& v1, const std::string& v2);

} // namespace tools
} // namespace midibn
