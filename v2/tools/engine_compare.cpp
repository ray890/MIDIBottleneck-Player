#include "engine_compare.h"

#include "difference_rules.h"
#include "midibn/midi_loader.h"
#include "midibn/sha256.h"
#include "scenario_binder.h"
#include "scenario_file.h"
#include "scenario_runner.h"
#include "tool_support.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>

namespace midibn {
namespace tools {

namespace {

using harness::EngineRunResult;
using harness::ScenarioDefinition;

struct DigestPair {
    std::string out;
    std::string sched;
};

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        size_t stop = end;
        if (stop > start && text[stop - 1] == '\r') --stop;
        lines.push_back(text.substr(start, stop - start));
        start = end + 1;
    }
    return lines;
}

std::vector<std::string> split_words(const std::string& line)
{
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t s = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > s) words.push_back(line.substr(s, i - s));
    }
    return words;
}

std::string sha_hex(const std::string& bytes)
{
    Sha256 sha;
    sha.update(bytes);
    return sha.hex_digest();
}

bool matches_only(const std::string& name, const std::string& only)
{
    if (only.empty()) return true;
    size_t start = 0;
    while (start <= only.size()) {
        size_t comma = only.find(',', start);
        std::string pattern = only.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!pattern.empty()) {
            if (pattern.back() == '*') {
                if (name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0) return true;
            }
            else if (name == pattern) return true;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

// "<scenario> <out sha256> <sched sha256>" lines; '#' lines are comments.
bool parse_digest_file(const std::string& text, std::map<std::string, DigestPair>& digests)
{
    for (const std::string& line : split_lines(text)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> w = split_words(line);
        if (w.size() != 3 || w[1].size() != 64 || w[2].size() != 64) return false;
        DigestPair pair;
        pair.out = w[1];
        pair.sched = w[2];
        digests[w[0]] = pair;
    }
    return true;
}

std::string clip(const std::string& s)
{
    return s.size() <= 220 ? s : s.substr(0, 220) + "...";
}

// First differing line of two recordings, as a short excerpt for a human (the line before it for context, then
// the v1 and v2 lines, and the line counts).
std::string first_difference(const std::string& v1, const std::string& v2)
{
    std::vector<std::string> a = split_lines(v1), b = split_lines(v2);
    size_t n = a.size() < b.size() ? a.size() : b.size();
    const std::string counts = "(v1 has " + std::to_string(a.size()) + " lines, v2 " + std::to_string(b.size()) + ")";
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return "line " + std::to_string(i + 1) + " " + counts + ":" +
                (i > 0 ? "\n    (line before) " + clip(a[i - 1]) : std::string()) +
                "\n    v1: " + clip(a[i]) + "\n    v2: " + clip(b[i]);
    }
    if (a.size() != b.size())
        return "line " + std::to_string(n + 1) + ": " + (a.size() > b.size() ? "v2 ends" : "v1 ends") + " " + counts +
            "\n    " + (a.size() > b.size() ? "v1: " + clip(a[n]) : "v2: " + clip(b[n]));
    return "no line differs (line-ending or trailing-byte difference)";
}


} // namespace

std::string describe_first_difference(const std::string& v1, const std::string& v2)
{
    return first_difference(v1, v2);
}

std::string scenario_file_digest(const std::string& rawText)
{
    std::string text;
    text.reserve(rawText.size());
    for (size_t i = 0; i < rawText.size(); ++i) {
        char c = rawText[i];
        if (c == '\r') {
            text.push_back('\n');
            if (i + 1 < rawText.size() && rawText[i + 1] == '\n') ++i;
        }
        else text.push_back(c);
    }
    return sha_hex(text);
}

EngineCompareResult run_engine_comparison(const EngineCompareOptions& options)
{
    EngineCompareResult total;
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    std::string scenarioText;
    if (!read_file(options.scenario_file, scenarioText)) {
        std::printf("FAIL engine scenarios: cannot read %s\n", options.scenario_file.c_str());
        ++total.setup_failures;
        return total;
    }
    std::vector<ScenarioDefinition> scenarios;
    std::string error;
    if (!harness::load_scenario_file(scenarioText, options.scenario_file, scenarios, error) ||
        !harness::validate_scenarios(scenarios, error)) {
        std::printf("FAIL engine scenarios: SCENARIO FILE ERROR: %s\n", error.c_str());
        ++total.setup_failures;
        return total;
    }
    std::vector<ScenarioDefinition> selected;
    for (const ScenarioDefinition& d : scenarios)
        if (matches_only(d.name, options.only)) selected.push_back(d);
    if (selected.empty()) {
        std::printf("FAIL engine scenarios: no scenario matches '%s'\n", options.only.c_str());
        ++total.setup_failures;
        return total;
    }

    const bool writeBaseline = !options.write_baseline_dir.empty();
    if (writeBaseline && (!options.only.empty() || !options.fixtures.empty())) {
        std::printf("FAIL engine baseline: a baseline can only be written from the full set of scenarios and fixtures\n");
        ++total.setup_failures;
        return total;
    }
    const bool compareDigests = !options.expected_dir.empty();
    const bool compareBaseline = !options.baseline_dir.empty() && !writeBaseline;
    const std::string scenarioDigest = scenario_file_digest(scenarioText);
    if (compareDigests) {
        std::string manifest;
        if (!read_file(options.expected_dir + "\\manifest.txt", manifest)) {
            std::printf("FAIL engine digests: cannot read %s\\manifest.txt\n", options.expected_dir.c_str());
            ++total.setup_failures;
            return total;
        }
        std::string digest;
        for (const std::string& line : split_lines(manifest)) {
            std::vector<std::string> w = split_words(line);
            if (w.size() == 2 && w[0] == "scenarios-sha256") digest = w[1];
        }
        if (digest != scenarioDigest) {
            std::printf("FAIL engine digests: %s is not the scenario file the digests were made with "
                "(manifest %s, file %s)\n", options.scenario_file.c_str(), digest.c_str(), scenarioDigest.c_str());
            ++total.setup_failures;
            return total;
        }
    }
    if (compareBaseline) {
        std::string manifest;
        if (!read_file(options.baseline_dir + "\\manifest.txt", manifest)) {
            std::printf("FAIL engine v2 baseline: cannot read %s\\manifest.txt\n", options.baseline_dir.c_str());
            ++total.setup_failures;
            return total;
        }
        std::string digest;
        for (const std::string& line : split_lines(manifest)) {
            std::vector<std::string> w = split_words(line);
            if (w.size() == 2 && w[0] == "scenarios-sha256") digest = w[1];
        }
        if (digest != scenarioDigest) {
            std::printf("FAIL engine v2 baseline: %s is not the scenario file the v2 baseline was made with "
                "(baseline %s, file %s); regenerate it with v2\\harness\\make-v2-baseline.ps1\n",
                options.scenario_file.c_str(), digest.c_str(), scenarioDigest.c_str());
            ++total.setup_failures;
            return total;
        }
    }
    if (!options.out_dir.empty()) make_directory(options.out_dir);
    if (writeBaseline) make_directory(options.write_baseline_dir);

    const std::vector<std::string>& fixtureNames = options.fixtures.empty() ? harness::engine_fixture_names() : options.fixtures;
    win32::Win32FileSystem files;
    const bool sampling = options.sample_stride > 1;
    std::set<DifferenceRule*> sampledRules;      // engine rules some sampled run was covered by
    int fixtureIndex = -1;
    for (const std::string& fixture : fixtureNames) {
        ++fixtureIndex;
        std::string stem = harness::fixture_stem(fixture);
        std::map<std::string, DigestPair> digests;
        if (compareDigests) {
            std::string text;
            if (!read_file(options.expected_dir + "\\" + stem + ".digests", text) || !parse_digest_file(text, digests)) {
                std::printf("FAIL engine %s: digest list %s\\%s.digests is missing or malformed\n", fixture.c_str(),
                    options.expected_dir.c_str(), stem.c_str());
                ++total.setup_failures;
                continue;
            }
        }
        std::map<std::string, DigestPair> baseline;
        if (compareBaseline) {
            std::string text;
            if (!read_file(options.baseline_dir + "\\" + stem + ".digests", text) || !parse_digest_file(text, baseline)) {
                std::printf("FAIL engine %s: v2 baseline list %s\\%s.digests is missing or malformed; regenerate it with "
                    "v2\\harness\\make-v2-baseline.ps1\n", fixture.c_str(), options.baseline_dir.c_str(), stem.c_str());
                ++total.setup_failures;
                continue;
            }
        }
        std::string loadMessage;
        std::unique_ptr<platform::ByteSource> source = files.open_read(options.fixtures_dir + "\\" + fixture, loadMessage);
        std::shared_ptr<Song> song = std::make_shared<Song>();
        LoadError loadError;
        if (!source || !load_song(*source, LoadOptions(), *song, loadError)) {
            std::printf("FAIL engine %s: cannot load fixture (%s)\n", fixture.c_str(),
                source ? loadError.message.c_str() : loadMessage.c_str());
            ++total.setup_failures;
            continue;
        }
        ++total.fixtures;
        std::set<DifferenceRule*> touched;       // rules that covered at least one run of this fixture
        std::map<DifferenceRule*, int> printed;  // explained-run notes printed per rule (kept short)
        std::vector<std::string> baselineLines;
        int scenarioIndex = -1;
        for (const ScenarioDefinition& definition : selected) {
            ++scenarioIndex;
            if (sampling && (fixtureIndex + scenarioIndex + options.sample_offset) % options.sample_stride != 0) {
                ++total.skipped_runs;
                continue;
            }
            EngineRunResult run;
            bool clean = harness::run_engine_scenario(song, fixture, definition, run);
            ++total.runs;
            std::vector<DifferenceRule*> covering;
            if (options.rules) covering = options.rules->engine_rules(definition.name, stem);
            sampledRules.insert(covering.begin(), covering.end());
            // Named normalizers map v2's .out recording to what v1 would have recorded.
            std::string compared = run.out;
            for (DifferenceRule* rule : covering) {
                if (rule->normalizer.empty() || !rule->files_out) continue;
                Normalizer normalize = DifferenceRules::find_normalizer(rule->normalizer);
                int changed = 0;
                compared = normalize(compared, changed);
                touched.insert(rule);
                if (changed > 0) {
                    ++rule->uses;
                    rule->lines += changed;
                    rule->lines_by_fixture[stem] += changed;
                }
            }
            if (options.print_each_run) std::printf("%s\n", harness::console_summary(definition.name, fixture, run).c_str());
            bool identical = clean;
            if (!clean) {
                ++total.failed_runs;
                std::printf("FAIL engine %s / %s: run error: %s\n", fixture.c_str(), definition.name.c_str(), run.error.c_str());
            }
            std::string outName = definition.name + "__" + stem + ".out.txt";
            std::string schedName = definition.name + "__" + stem + ".sched.txt";
            const std::string rawHash[2] = { sha_hex(run.out), sha_hex(run.sched) };
            const std::string comparedHash[2] = { compared == run.out ? rawHash[0] : sha_hex(compared), rawHash[1] };

            // Tier 1: v1. A difference must be explained by a rule.
            for (int which = 0; which < 2; ++which) {
                const std::string& name = which == 0 ? outName : schedName;
                const std::string& actualText = which == 0 ? compared : run.sched;
                std::string v1text;
                bool haveV1 = false;
                bool differs = false;
                if (compareDigests) {
                    std::map<std::string, DigestPair>::const_iterator expected = digests.find(definition.name);
                    if (expected == digests.end()) {
                        if (which == 0) {
                            std::printf("FAIL engine %s / %s: no expected digest\n", fixture.c_str(), definition.name.c_str());
                            ++total.mismatched_files;
                            identical = false;
                        }
                        continue;
                    }
                    const std::string& want = which == 0 ? expected->second.out : expected->second.sched;
                    differs = comparedHash[which] != want;
                    if (!differs) continue;
                    if (!options.v1_dir.empty()) haveV1 = read_file(options.v1_dir + "\\" + name, v1text);
                    DifferenceRule* explainer = nullptr;
                    for (DifferenceRule* rule : covering)
                        if (rule->explained && (which == 0 ? rule->files_out : rule->files_sched)) { explainer = rule; break; }
                    if (explainer) {
                        ++explainer->uses;
                        touched.insert(explainer);
                        ++total.explained_files;
                        if (printed[explainer]++ < 3 || haveV1)
                            std::printf("note engine %s / %s: %s differs from v1, explained by %s%s\n", fixture.c_str(),
                                definition.name.c_str(), name.c_str(), explainer->id.c_str(),
                                haveV1 ? ("\n  first difference, " + first_difference(v1text, actualText)).c_str() : "");
                        continue;
                    }
                    identical = false;
                    ++total.mismatched_files;
                    std::printf("FAIL engine %s / %s: %s differs from v1 and no rule explains it (v2 %s.. expected %s..)\n",
                        fixture.c_str(), definition.name.c_str(), name.c_str(), comparedHash[which].substr(0, 16).c_str(),
                        want.substr(0, 16).c_str());
                    if (!options.v1_dir.empty()) {
                        if (haveV1) std::printf("  first difference, %s\n", first_difference(v1text, actualText).c_str());
                        else std::printf("  (v1 file %s\\%s not found)\n", options.v1_dir.c_str(), name.c_str());
                    }
                }
                else if (!options.v1_dir.empty()) {
                    // No digests: compare the recordings with the v1 files directly.
                    if (!read_file(options.v1_dir + "\\" + name, v1text)) {
                        std::printf("FAIL engine %s / %s: v1 file %s\\%s not found\n", fixture.c_str(), definition.name.c_str(),
                            options.v1_dir.c_str(), name.c_str());
                        ++total.mismatched_files;
                        identical = false;
                        continue;
                    }
                    if (v1text == actualText) continue;
                    DifferenceRule* explainer = nullptr;
                    for (DifferenceRule* rule : covering)
                        if (rule->explained && (which == 0 ? rule->files_out : rule->files_sched)) { explainer = rule; break; }
                    if (explainer) {
                        ++explainer->uses;
                        touched.insert(explainer);
                        ++total.explained_files;
                        std::printf("note engine %s / %s: %s differs from v1, explained by %s\n  first difference, %s\n",
                            fixture.c_str(), definition.name.c_str(), name.c_str(), explainer->id.c_str(),
                            first_difference(v1text, actualText).c_str());
                        continue;
                    }
                    ++total.mismatched_files;
                    identical = false;
                    std::printf("FAIL engine %s / %s: %s differs from v1 and no rule explains it\n  first difference, %s\n",
                        fixture.c_str(), definition.name.c_str(), name.c_str(), first_difference(v1text, actualText).c_str());
                }
            }

            // Tier 2: the v2 baseline, exact and independent of v1 and of the rules.
            if (compareBaseline) {
                std::map<std::string, DigestPair>::const_iterator base = baseline.find(definition.name);
                if (base == baseline.end()) {
                    std::printf("FAIL engine %s / %s: no v2 baseline entry (regenerate with v2\\harness\\make-v2-baseline.ps1 if this scenario is new)\n",
                        fixture.c_str(), definition.name.c_str());
                    ++total.baseline_mismatches;
                    identical = false;
                }
                else {
                    for (int which = 0; which < 2; ++which) {
                        const std::string& want = which == 0 ? base->second.out : base->second.sched;
                        if (rawHash[which] == want) continue;
                        std::printf("FAIL engine %s / %s: %s differs from the v2 baseline (v2 %s.. baseline %s..); if the change is "
                            "intended, record it in v2\\docs\\DIFFERENCES_FROM_V1.md and regenerate with v2\\harness\\make-v2-baseline.ps1\n",
                            fixture.c_str(), definition.name.c_str(), (which == 0 ? outName : schedName).c_str(),
                            rawHash[which].substr(0, 16).c_str(), want.substr(0, 16).c_str());
                        ++total.baseline_mismatches;
                        identical = false;
                    }
                }
            }
            if (writeBaseline) baselineLines.push_back(definition.name + " " + rawHash[0] + " " + rawHash[1]);
            if (identical) ++total.identical;
            if (!identical && options.fail_fast) {
                total.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                return total;
            }
            if (!options.out_dir.empty() && (!options.write_only_differences || !identical)) {
                write_file(options.out_dir + "\\" + outName, run.out);
                write_file(options.out_dir + "\\" + schedName, run.sched);
            }
        }
        if (writeBaseline) {
            std::sort(baselineLines.begin(), baselineLines.end());
            std::string text = "# v2 engine recordings of " + stem + ": SHA-256 of .out.txt and .sched.txt per scenario (make-v2-baseline.ps1)\n";
            for (const std::string& line : baselineLines) text += line + "\n";
            if (!write_file(options.write_baseline_dir + "\\" + stem + ".digests", text)) {
                std::printf("FAIL engine baseline: cannot write %s\\%s.digests\n", options.write_baseline_dir.c_str(), stem.c_str());
                ++total.setup_failures;
            }
        }
        if (options.only.empty() && !sampling) {
            // Normalizer pins: the number of recording lines a normalizer changes in a fixture is fixed.
            for (DifferenceRule* rule : touched) {
                if (rule->normalizer.empty()) continue;
                int changed = rule->lines_by_fixture.count(stem) ? rule->lines_by_fixture[stem] : 0;
                if (changed > 0)
                    std::printf("note engine %s: %s (normalizer %s) mapped %d recording lines\n", fixture.c_str(),
                        rule->id.c_str(), rule->normalizer.c_str(), changed);
                std::map<std::string, int>::const_iterator pin = rule->pins.find(stem);
                if (pin != rule->pins.end() && pin->second != changed) {
                    std::printf("FAIL engine %s: %s (normalizer %s) changed %d recording lines, the rule pins %d\n",
                        fixture.c_str(), rule->id.c_str(), rule->normalizer.c_str(), changed, pin->second);
                    ++total.mismatched_files;
                }
            }
        }
        if ((compareDigests || compareBaseline) && options.only.empty()) {
            // Every digest must have been used: a digest without a scenario is stale.
            const std::map<std::string, DigestPair>* lists[2] = { &digests, &baseline };
            const char* names[2] = { "digest", "v2 baseline entry" };
            for (int k = 0; k < 2; ++k) {
                for (std::map<std::string, DigestPair>::const_iterator it = lists[k]->begin(); it != lists[k]->end(); ++it) {
                    bool known = false;
                    for (const ScenarioDefinition& d : selected) if (d.name == it->first) { known = true; break; }
                    if (!known) {
                        std::printf("FAIL engine %s / %s: %s has no scenario in the scenario file\n", fixture.c_str(), it->first.c_str(), names[k]);
                        ++(k == 0 ? total.mismatched_files : total.baseline_mismatches);
                    }
                }
            }
        }
    }
    // A quick sample may miss the runs a rule explains: such a rule is reported as not exercised, not as stale.
    if (sampling)
        for (DifferenceRule* rule : sampledRules)
            if (rule->uses == 0) rule->exercised = false;
    if (writeBaseline) {
        std::string manifest = "# v2 engine baseline manifest, written by v2/harness/make-v2-baseline.ps1. See v2/harness/TRACE_FORMAT.md.\n"
            "format 1\nscenarios-sha256 " + scenarioDigest + "\nscenarios " + std::to_string(selected.size()) +
            "\nfixtures " + std::to_string(total.fixtures) + "\n";
        if (!write_file(options.write_baseline_dir + "\\manifest.txt", manifest)) {
            std::printf("FAIL engine baseline: cannot write %s\\manifest.txt\n", options.write_baseline_dir.c_str());
            ++total.setup_failures;
        }
    }
    total.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return total;
}

} // namespace tools
} // namespace midibn
