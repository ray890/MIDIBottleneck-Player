// midibn-engine-harness: the v2 side of the engine comparison with v1 (Milestone 5, Group E): every
// recorded line is compared exactly, and a difference passes only when a rule of the improvement
// register explains it (v2/docs/DIFFERENCES_FROM_V1.md). Runs the scripted engine scenarios
// (v2/harness/engine-scenarios.txt, specified in v2/harness/ENGINE_SCENARIOS.md)
// on every listed fixture against the v2 engine under the lock-step
// harness (v2/harness/engine/), the C++ twin of the v1 reference harness's
// rendezvous spike, and writes/compares the recordings.
//
//   midibn-engine-harness --list [--scenarios FILE]
//   midibn-engine-harness [--scenarios FILE] [--only NAME,PREFIX*] [--fixtures DIR] [--out DIR]
//                         [--expected DIR] [--rules FILE] [--baseline DIR] [--write-baseline DIR]
//                         [--v1 DIR] [--differences-only] [--verbose] [--fail-fast]
//                         [fixture.mid ...]
//
// With no fixture names it runs the 23 valid fixtures. --out DIR writes
// <scenario>__<fixture>.out.txt and .sched.txt (raw v2 recordings).
// --expected DIR (v2\harness\expected\engine) compares the SHA-256 of every
// recording with the committed v1 digests; a difference from v1 passes only
// where an engine rule of --rules FILE (v2\harness\expected\differences-from-v1.txt)
// explains it (v2\harness\TRACE_FORMAT.md, "Difference rules"), and a rule that
// explains nothing any more fails. --baseline DIR (v2\harness\expected-v2\engine)
// also requires every raw v2 recording to equal v2's own recorded digest;
// --write-baseline DIR writes such digests instead (full set only).
// --v1 DIR (a spike output directory, e.g. v2\out\spike\runs-lockstep-m5ext\x64\run1)
// reports the first differing line of any recording that differs from v1, as a
// short excerpt (v1 text next to v2 text), also for explained differences; without
// --expected it compares the files in DIR directly. --verbose prints the
// spike-style summary line of every run. Exit code 0 when every run completed
// and nothing unexplained differs.
#include "difference_rules.h"
#include "engine_compare.h"
#include "scenario_binder.h"
#include "scenario_file.h"
#include "scenario_runner.h"
#include "tool_support.h"

#include <cstdio>

using namespace midibn;

int MIDIBN_TOOL_MAIN(int argc, midibn::tools::ToolChar** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    tools::EngineCompareOptions options;
    std::string base = tools::executable_directory();
    options.fixtures_dir = base + "\\fixtures";
    options.scenario_file = base + "\\engine-scenarios.txt";
    bool list = false;
    std::string rulesFile;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        bool hasValue = i + 1 < args.size();
        if (a == "--list") list = true;
        else if (a == "--differences-only") options.write_only_differences = true;
        else if (a == "--verbose") options.print_each_run = true;
        else if (a == "--fail-fast") options.fail_fast = true;
        else if (a == "--scenarios" && hasValue) options.scenario_file = args[++i];
        else if (a == "--only" && hasValue) options.only = args[++i];
        else if (a == "--fixtures" && hasValue) options.fixtures_dir = args[++i];
        else if (a == "--out" && hasValue) options.out_dir = args[++i];
        else if (a == "--expected" && hasValue) options.expected_dir = args[++i];
        else if (a == "--v1" && hasValue) options.v1_dir = args[++i];
        else if (a == "--rules" && hasValue) rulesFile = args[++i];
        else if (a == "--baseline" && hasValue) options.baseline_dir = args[++i];
        else if (a == "--write-baseline" && hasValue) options.write_baseline_dir = args[++i];
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
        else options.fixtures.push_back(a);
    }

    if (list) {
        std::string text, error;
        std::vector<harness::ScenarioDefinition> scenarios;
        if (!tools::read_file(options.scenario_file, text) ||
            !harness::load_scenario_file(text, options.scenario_file, scenarios, error) ||
            !harness::validate_scenarios(scenarios, error)) {
            std::printf("SCENARIO FILE ERROR: %s\n", error.empty() ? "cannot read the scenario file" : error.c_str());
            return 2;
        }
        for (const harness::ScenarioDefinition& s : scenarios) std::printf("%s\n", s.name.c_str());
        std::printf("%d scenarios in %s\n", static_cast<int>(scenarios.size()), options.scenario_file.c_str());
        return 0;
    }

    tools::DifferenceRules rules;
    if (!rulesFile.empty()) {
        std::string text, error;
        if (!tools::read_file(rulesFile, text) || !rules.load(text, error)) {
            std::printf("FAIL rules file %s: %s\n", rulesFile.c_str(), error.empty() ? "cannot read it" : error.c_str());
            return 2;
        }
        options.rules = &rules;
    }
    tools::EngineCompareResult result = tools::run_engine_comparison(options);
    int staleRules = (options.rules && !options.fail_fast) ? rules.finish(true) : 0;
    std::printf("engine harness: %d fixtures, %d runs: %d identical, %d differing files from v1, %d explained by rules, "
        "%d differing from the v2 baseline, %d failed runs, %d setup failures (%.1f s)\n", result.fixtures, result.runs,
        result.identical, result.mismatched_files, result.explained_files, result.baseline_mismatches, result.failed_runs,
        result.setup_failures, result.seconds);
    bool comparing = !options.expected_dir.empty() || !options.v1_dir.empty() || !options.baseline_dir.empty();
    bool pass = result.pass() && staleRules == 0;
    std::printf("RESULT: %s%s\n", pass ? "PASS" : "FAIL", comparing ? "" : " (no comparison requested)");
    return pass ? 0 : 1;
}
