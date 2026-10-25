// midibn-parity: compares v2 traces with the v1 reference traces and checks
// the .NET decimal vectors. Built for XP as v2-xp-selftest.exe.
//
//   midibn-parity --has-sse2   (prints one line; exit code 0 = Windows reports SSE2 usable, 1 = not)
//   midibn-parity [--fixtures DIR] [--expected DIR] [--rules FILE] [--baseline DIR] [--write-baseline DIR]
//                 [--large FILE --large-expected FILE] [--large-model TRACE --large-model-fixtures DIR]
//                 [--scenarios FILE] [--engine-fixtures NAME,NAME] [--engine-only NAME,PREFIX*] [--engine-v1 DIR] [--no-engine]
//                 [--show-differences] [--write-failures DIR] [--capability-report FILE]
//                 [--trace-fixtures NAME,NAME] [--model-prefix DIVISOR] [--engine-sample STRIDE[,OFFSET]] [--capability-quick]
// With no arguments it uses fixtures\, expected\, expected\differences-from-v1.txt and expected-v2\ next to the executable.
//
// Compared for every fixture: the load/index trace (<stem>.trace) and the
// load-flow traces for pointer sizes 4 and 8 (flow\<stem>.ptr4|8.trace).
// Every model trace (models\<family>.trace) is replayed against the v2
// models and compared the same way.
// Every engine scenario (engine-scenarios.txt; v2/harness/ENGINE_SCENARIOS.md) is run on the v2 playback
// engine under the lock-step harness for each of the 23 engine fixtures, and the SHA-256 of both recordings is
// compared with the digests of v1's recordings (expected\engine\; v2/harness/TRACE_FORMAT.md). --engine-fixtures
// and --engine-only run a subset (the package default is the full set); --engine-v1 DIR (a v1 spike output directory) adds
// the first differing line to each mismatch report; --no-engine skips the section.
//
// Two tiers (v2/docs/DIFFERENCES_FROM_V1.md, v2/harness/TRACE_FORMAT.md "Difference rules" and "v2 baseline"):
//  1. v1: a line may differ from v1's expected trace only where a rule of expected\differences-from-v1.txt explains
//     the difference (each rule names an entry of the improvement register); any other difference, or a rule whose
//     targets were compared and that no longer matches anything, is a failure. Rules whose targets were not run
//     are reported as "not exercised".
//  2. v2 baseline: every v2 trace and engine recording must equal v2's own recorded digest (expected-v2\), exactly,
//     whatever v1 says. --write-baseline DIR writes a new baseline instead (v2\harness\make-v2-baseline.ps1).
//
// Quick self-test (v2/BUILDING.md, "Windows XP self-test"): --trace-fixtures runs the load and flow traces of the
// named fixtures only (file names or stems); --model-prefix DIVISOR replays only the first 1/DIVISOR of every section
// of every model trace (never fewer than 400 lines, cut at the next command; the sections are independent);
// --engine-sample STRIDE[,OFFSET] runs engine scenario #j on fixture #i only when (i + j + OFFSET) % STRIDE == 0
// (STRIDE <= 23 touches every scenario and every fixture); --capability-quick
// shortens the disk test of the probe. Whatever a subset leaves out is reported as "not exercised", never as a failure.
// --capability-report FILE: after the parity result, measures what this PC can
// do (tools/capability_probe.h), prints the report and writes machine-readable
// key=value lines to FILE; its folder is used for the temporary-file tests.
// The probe never changes the exit code, which reflects parity only.
#ifdef _WIN32
#include "capability_probe.h"
#endif
#include "difference_rules.h"
#include "engine_compare.h"
#include "midibn/model_trace.h"
#include "midibn/net_decimal.h"
#include "midibn/tempo_clock.h"
#include "midibn/sha256.h"
#include "midibn/trace.h"
#include "tool_support.h"

#include <chrono>
#include <map>
#include <set>
#include <sstream>

using namespace midibn;

namespace {

struct Totals {
    int passed = 0;
    int failed = 0;
};

// v2's own recorded digests of the traces (expected-v2\traces.sha256), compared exactly.
struct BaselineEntry {
    std::string digest;
    bool seen = false;
};

struct Context {
    tools::DifferenceRules rules;
    bool showAllDifferences = false;
    int modelDivisor = 1;                 // --model-prefix: quick run, replay a prefix of every model trace section
    int baselineSkipped = 0;              // v2 baseline checks left out because the trace was cut short
    // Tier 2: v2 baseline
    std::string baselineDir;
    bool baselineLoaded = false;
    std::map<std::string, BaselineEntry> baseline;
    int baselinePassed = 0;
    int baselineFailed = 0;
    // Baseline regeneration (--write-baseline)
    bool writing = false;
    std::map<std::string, std::string> produced;        // trace key -> digest
    std::map<std::string, std::string> producedText;    // trace key -> v2 text, for traces that differ from v1
};

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(' ');
    size_t b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream s(text);
    std::string line;
    while (std::getline(s, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string clip(const std::string& s) { return s.size() > 160 ? s.substr(0, 160) + "..." : s; }

std::string trace_key(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

std::string sha_hex(const std::string& text)
{
    Sha256 sha;
    sha.update(text);
    return sha.hex_digest();
}

// "name SHA256" lines (a '#' line is a comment).
bool load_baseline(const std::string& dir, Context& ctx)
{
    std::string text;
    if (!tools::read_file(dir + "\\traces.sha256", text)) return false;
    for (const std::string& line : lines_of(text)) {
        if (line.empty() || line[0] == '#') continue;
        size_t space = line.find(' ');
        if (space == std::string::npos) return false;
        std::string digest = trim(line.substr(space + 1));
        if (digest.size() != 64) return false;
        BaselineEntry entry;
        entry.digest = digest;
        ctx.baseline[line.substr(0, space)] = entry;
    }
    return !ctx.baseline.empty();
}

// First differing line between two line lists, as a short excerpt.
std::string first_line_difference(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return "line " + std::to_string(i + 1) + "\n      baseline: " + clip(a[i]) + "\n      actual:   " + clip(b[i]);
    return "line " + std::to_string(n + 1) + "\n      baseline: " + (n < a.size() ? clip(a[n]) : "<end>") +
        "\n      actual:   " + (n < b.size() ? clip(b[n]) : "<end>");
}

std::string text_file_name(const std::string& key)
{
    std::string name = key;
    std::replace(name.begin(), name.end(), '/', '_');
    return name;
}

// --model-prefix: keeps the first 1/divisor of every [section] of a model trace (at least 400 lines, extended to the
// next "> " command so that no command is cut from its results). The sections are independent of each other except
// that a section whose model constructors fail still prints the state the previous section left behind (the
// adaptive part of the "~" lines); `followers` receives the headers of the sections that come after a cut section.
std::string truncate_model_script(const std::string& script, int divisor, std::set<std::string>& followers)
{
    bool previousCut = false;
    std::vector<std::string> lines = lines_of(script);
    std::string out;
    size_t i = 0;
    while (i < lines.size()) {
        if (lines[i].empty() || lines[i][0] != '[' || lines[i] == "[end]") { out += lines[i] + "\n"; ++i; continue; }
        size_t j = i + 1;
        while (j < lines.size() && (lines[j].empty() || lines[j][0] != '[')) ++j;
        if (previousCut) followers.insert(lines[i]);
        size_t count = j - i;
        size_t keep = (count + static_cast<size_t>(divisor) - 1) / static_cast<size_t>(divisor);
        if (keep < 400) keep = 400;
        if (keep >= count) keep = count;
        else {
            size_t k = i + keep;
            while (k < j && lines[k].compare(0, 2, "> ") != 0) ++k;
            keep = k - i;
        }
        for (size_t k = i; k < i + keep; ++k) out += lines[k] + "\n";
        previousCut = keep < count;
        i = j;
    }
    return out;
}

// In the sections that follow a cut section, the "~" state lines are replaced by a fixed text until the section
// creates its own adaptive part (`> new adaptive`), in the expected text and in v2's output alike: before that they
// show state carried over from the end of the previous section, which this quick run did not reach.
std::string mask_carried_state(const std::string& text, const std::set<std::string>& followers)
{
    std::vector<std::string> lines = lines_of(text);
    std::string out;
    bool masking = false;
    for (const std::string& line : lines) {
        if (!line.empty() && line[0] == '[') masking = followers.count(line) != 0;
        else if (masking && line.compare(0, 14, "> new adaptive") == 0) masking = false;
        if (masking && line.compare(0, 2, "~ ") == 0) out += "~ (state carried over from the previous section: not exercised)\n";
        else out += line + "\n";
    }
    return out;
}

// Tier 2: v2's output (comment lines removed) must equal its recorded digest.
void check_baseline(const std::string& label, const std::string& traceName, const std::string& strippedActual,
    bool differsFromV1, Context& ctx)
{
    if (ctx.modelDivisor > 1 && traceName.compare(0, 7, "models\\") == 0) {
        // A model trace cut short by --model-prefix cannot match the digest of the whole trace: not exercised.
        ++ctx.baselineSkipped;
        return;
    }
    const std::string key = trace_key(traceName);
    const std::string digest = sha_hex(strippedActual);
    if (ctx.writing) {
        ctx.produced[key] = digest;
        if (differsFromV1) ctx.producedText[key] = strippedActual;
        return;
    }
    if (!ctx.baselineLoaded) return;
    std::map<std::string, BaselineEntry>::iterator it = ctx.baseline.find(key);
    if (it == ctx.baseline.end()) {
        std::printf("FAIL baseline %s: no v2 baseline entry for %s; if it is new, regenerate the baseline "
            "(v2\\harness\\make-v2-baseline.ps1)\n", label.c_str(), key.c_str());
        ++ctx.baselineFailed;
        return;
    }
    it->second.seen = true;
    if (it->second.digest == digest) { ++ctx.baselinePassed; return; }
    ++ctx.baselineFailed;
    std::printf("FAIL baseline %s: v2's output changed from its recorded baseline (SHA-256 %s.. now, %s.. recorded). If the "
        "change is intended, record it in v2\\docs\\DIFFERENCES_FROM_V1.md and regenerate the baseline "
        "(v2\\harness\\make-v2-baseline.ps1).\n", label.c_str(), digest.substr(0, 16).c_str(), it->second.digest.substr(0, 16).c_str());
    std::string stored;
    if (tools::read_file(ctx.baselineDir + "\\text\\" + text_file_name(key), stored))
        std::printf("    first difference from the recorded v2 text, %s\n",
            first_line_difference(lines_of(stored), lines_of(strippedActual)).c_str());
}

bool compare_text(const std::string& label, const std::string& traceName, const std::string& expected,
    const std::string& actual, Context& ctx, const std::string& failureDir, Totals& totals)
{
    const std::string strippedActual = strip_trace_comments(actual);
    std::vector<std::string> e = lines_of(strip_trace_comments(expected));
    std::vector<std::string> a = lines_of(strippedActual);
    tools::TraceCompareResult result = ctx.rules.compare_trace(traceName, e, a);
    check_baseline(label, traceName, strippedActual, !result.applied.empty() || !result.ok, ctx);
    if (result.ok) {
        if (result.applied.empty()) std::printf("PASS %s\n", label.c_str());
        else {
            std::printf("PASS %s (%d explained difference%s)\n", label.c_str(), static_cast<int>(result.applied.size()),
                result.applied.size() == 1 ? "" : "s");
            std::map<const tools::DifferenceRule*, int> shown, total;
            for (const tools::AppliedDifference& d : result.applied) ++total[d.rule];
            for (const tools::AppliedDifference& d : result.applied) {
                if (!ctx.showAllDifferences && shown[d.rule]++ >= 3) continue;
                const char* what = d.rule->kind == tools::RuleKind::TraceInsert ? "added line" :
                    d.rule->kind == tools::RuleKind::TraceDelete ? "removed line" : "changed line";
                std::printf("  explained by %s: %s %d\n", d.rule->id.c_str(), what, d.v2_line ? d.v2_line : d.v1_line);
                if (d.v1_line) std::printf("      v1: %s\n", clip(d.v1_text).c_str());
                if (d.v2_line) std::printf("      v2: %s\n", clip(d.v2_text).c_str());
            }
            for (std::map<const tools::DifferenceRule*, int>::const_iterator it = total.begin(); it != total.end(); ++it)
                if (!ctx.showAllDifferences && it->second > 3)
                    std::printf("  explained by %s: ... and %d more (--show-differences lists all)\n", it->first->id.c_str(), it->second - 3);
        }
        ++totals.passed;
        return true;
    }
    std::printf("FAIL %s: %s\n", label.c_str(), result.failure.c_str());
    if (!failureDir.empty()) {
        std::string safe = traceName;
        std::replace(safe.begin(), safe.end(), '\\', '_');
        std::replace(safe.begin(), safe.end(), '/', '_');
        tools::write_file(failureDir + "\\" + safe + ".v2", actual);
    }
    ++totals.failed;
    return false;
}

std::unique_ptr<platform::ByteSource> open_fixture(const std::string& path, const std::string& label, Totals& totals)
{
    static win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    if (!source) {
        std::printf("FAIL %s (%s)\n", label.c_str(), error.c_str());
        ++totals.failed;
    }
    return source;
}

void compare_load(const std::string& fixturePath, const std::string& expectedDir, const std::string& traceName,
    bool summary, const std::string& label, Context& ctx, const std::string& failureDir,
    Totals& totals)
{
    std::string expected;
    if (!tools::read_file(expectedDir + "\\" + traceName, expected)) {
        std::printf("FAIL %s (missing expected trace %s)\n", label.c_str(), traceName.c_str());
        ++totals.failed;
        return;
    }
    std::unique_ptr<platform::ByteSource> source = open_fixture(fixturePath, label, totals);
    if (!source) return;
    std::string actual = write_load_trace(*source, label, summary, "midibn-parity v2 " + tools::build_description());
    compare_text(label, traceName, expected, actual, ctx, failureDir, totals);
}

void compare_flow(const std::string& fixturePath, const std::string& expectedDir, const std::string& traceName,
    bool summary, int pointerSize, const std::string& label, Context& ctx,
    const std::string& failureDir, Totals& totals)
{
    std::string expected;
    if (!tools::read_file(expectedDir + "\\" + traceName, expected)) {
        std::printf("FAIL %s (missing expected trace %s)\n", label.c_str(), traceName.c_str());
        ++totals.failed;
        return;
    }
    std::unique_ptr<platform::ByteSource> source = open_fixture(fixturePath, label, totals);
    if (!source) return;
    std::string actual = write_flow_trace(*source, label, summary, pointerSize,
        "midibn-parity v2 " + tools::build_description());
    compare_text(label, traceName, expected, actual, ctx, failureDir, totals);
}

void compare_models(const std::string& fixturesDir, const std::string& expectedDir, Context& ctx,
    const std::string& failureDir, Totals& totals)
{
    std::vector<std::string> names = tools::list_files(expectedDir + "\\models", { ".trace" });
    if (names.empty()) {
        std::printf("FAIL no model traces found in %s\\models\n", expectedDir.c_str());
        ++totals.failed;
        return;
    }
    FixtureLoader loader = tools::fixture_loader(fixturesDir);
    for (const std::string& name : names) {
        std::string script;
        std::string label = "model " + name.substr(0, name.size() - 6);
        if (!tools::read_file(expectedDir + "\\models\\" + name, script)) {
            std::printf("FAIL %s (cannot read)\n", label.c_str());
            ++totals.failed;
            continue;
        }
        std::set<std::string> followers;
        if (ctx.modelDivisor > 1) script = mask_carried_state(truncate_model_script(script, ctx.modelDivisor, followers), followers);
        const auto replayStart = std::chrono::steady_clock::now();
        std::string actual = replay_model_trace(script, loader, "midibn-parity v2 " + tools::build_description());
        if (ctx.modelDivisor > 1) actual = mask_carried_state(actual, followers);
        compare_text(label, "models\\" + name, script, actual, ctx, failureDir, totals);
        const double replaySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - replayStart).count();
        if (replaySeconds >= 1.0) std::printf("      %s took %.1f s\n", label.c_str(), replaySeconds);
    }
}

// A model trace whose fixtures are generated on demand under fixtures\large (not committed), such as
// routing-large (the 1,000,000 held-note limit): replayed against the fixtures in `fixturesDir`.
void compare_large_model(const std::string& tracePath, const std::string& fixturesDir, Context& ctx,
    const std::string& failureDir, Totals& totals)
{
    std::string name = tracePath.substr(tracePath.find_last_of("\\/") + 1);
    std::string label = "model " + name.substr(0, name.size() - 6) + " (large)";
    std::string script;
    if (!tools::read_file(tracePath, script)) {
        std::printf("FAIL %s (cannot read %s)\n", label.c_str(), tracePath.c_str());
        ++totals.failed;
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::string actual = replay_model_trace(script, tools::fixture_loader(fixturesDir),
        "midibn-parity v2 " + tools::build_description());
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    compare_text(label, "models-large\\" + name, script, actual, ctx, failureDir, totals);
    std::printf("      %s replayed in %.1f s\n", label.c_str(), seconds);
}

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> fields;
    std::istringstream s(line);
    std::string f;
    while (s >> f) fields.push_back(f);
    return fields;
}

int g_exactSame = 0, g_exactTies = 0;

bool check_vectors(const std::string& path, int& passed, int& failed)
{
    std::string text;
    if (!tools::read_file(path, text)) {
        std::printf("FAIL decimal vectors (missing %s)\n", path.c_str());
        ++failed;
        return false;
    }
    std::istringstream lines(text);
    std::string line;
    int lineNumber = 0;
    int reported = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f = split(line);
        if (f.size() < 3 || f[0] != "D") { ++failed; continue; }
        uint32_t ppqn = static_cast<uint32_t>(std::stoul(f[1]));
        size_t steps = static_cast<size_t>(std::stoul(f[2]));
        if (f.size() != 3 + steps * 2 + 5) { ++failed; continue; }
        NetDecimal acc;
        size_t k = 3;
        for (size_t s = 0; s < steps; ++s, k += 2)
            acc = decimal_add(acc, decimal_divide(decimal_multiply(std::stoull(f[k]),
                static_cast<uint32_t>(std::stoul(f[k + 1]))), ppqn));
        NetDecimal value = decimal_add(acc, decimal_divide(decimal_multiply(std::stoull(f[k]),
            static_cast<uint32_t>(std::stoul(f[k + 1]))), ppqn));
        int64_t rounded = 0;
        std::string roundedText = decimal_round_away_to_int64(value, rounded) ? std::to_string(rounded) : "OverflowException";
        std::string accText = decimal_to_string(acc);
        std::string valueText = decimal_to_string(value);
        // The product's exact integer clock (DD-020) against the .NET result: identical, except where v1's
        // decimal noise put a true halfway value just below .5 (the decimal text then reads ...4999 and
        // the exact answer is one more).
        {
            // The vector is a chain: each step accumulates (delta x tempo) with its own tempo, and the last step
            // applies its own tempo; re-express it as tempo changes at the step boundaries.
            std::vector<TempoRecord> chain;
            int64_t at_tick = 0;
            for (size_t s = 0, at = 3; s < steps; ++s, at += 2) {
                // segment s runs from at_tick for delta ticks at tempo(s)
                int64_t delta = static_cast<int64_t>(std::stoull(f[at]));
                int32_t segmentTempo = static_cast<int32_t>(std::stoul(f[at + 1]));
                chain.push_back(TempoRecord{ at_tick, segmentTempo, 0, 0 });
                at_tick += delta;
            }
            size_t last = 3 + steps * 2;
            chain.push_back(TempoRecord{ at_tick, static_cast<int32_t>(std::stoul(f[last + 1])), 0, 0 });
            const int64_t finalTick = at_tick + static_cast<int64_t>(std::stoull(f[last]));
            TempoClock clock(chain, static_cast<int>(ppqn));
            int64_t exact = 0;
            bool exactOk = clock.time_at(finalTick, exact);
            std::string exactText = exactOk ? std::to_string(exact) : "OverflowException";
            if (exactText == roundedText) ++g_exactSame;
            else if (exactOk && roundedText != "OverflowException" && exact == std::stoll(roundedText) + 1 &&
                valueText.find('.') != std::string::npos && valueText.substr(valueText.find('.') + 1, 4) == "4999") ++g_exactTies;
            else {
                ++failed;
                if (reported++ < 5)
                    std::printf("FAIL exact clock vector line %d: .NET %s (value %s), exact %s\n", lineNumber, roundedText.c_str(),
                        valueText.c_str(), exactText.c_str());
            }
        }
        if (accText == f[k + 2] && valueText == f[k + 3] && roundedText == f[k + 4]) ++passed;
        else {
            ++failed;
            if (reported++ < 5)
                std::printf("FAIL decimal vector line %d\n      expected: %s %s %s\n      actual:   %s %s %s\n", lineNumber,
                    f[k + 2].c_str(), f[k + 3].c_str(), f[k + 4].c_str(), accText.c_str(), valueText.c_str(),
                    roundedText.c_str());
        }
    }
    return failed == 0;
}

NetDecimal parse_bits(const std::vector<std::string>& f, size_t at)
{
    NetDecimal d;
    d.lo = static_cast<uint32_t>(std::stoul(f[at]));
    d.mid = static_cast<uint32_t>(std::stoul(f[at + 1]));
    d.hi = static_cast<uint32_t>(std::stoul(f[at + 2]));
    d.scale = std::atoi(f[at + 3].c_str());
    return d;
}

std::string bits_text(const NetDecimal& d)
{
    return std::to_string(d.lo) + " " + std::to_string(d.mid) + " " + std::to_string(d.hi) + " " + std::to_string(d.scale);
}

// Primitive .NET decimal operations, compared bit for bit (decimal-ops.txt).
bool check_ops(const std::string& path, int& passed, int& failed)
{
    std::string text;
    if (!tools::read_file(path, text)) {
        std::printf("FAIL decimal ops (missing %s)\n", path.c_str());
        ++failed;
        return false;
    }
    int lineNumber = 0;
    int reported = 0;
    for (const std::string& line : lines_of(text)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') continue;
        size_t equals = line.find(" = ");
        if (equals == std::string::npos) { ++failed; continue; }
        std::vector<std::string> f = split(line.substr(0, equals));
        std::string expected = line.substr(equals + 3);
        std::string actual;
        if (f[0] == "M" && f.size() == 3)
            actual = bits_text(decimal_multiply(std::stoull(f[1]), static_cast<uint32_t>(std::stoul(f[2]))));
        else if (f[0] == "V" && f.size() == 5) {
            NetDecimal dividend;
            dividend.lo = static_cast<uint32_t>(std::stoul(f[1]));
            dividend.mid = static_cast<uint32_t>(std::stoul(f[2]));
            dividend.hi = static_cast<uint32_t>(std::stoul(f[3]));
            actual = bits_text(decimal_divide(dividend, static_cast<uint32_t>(std::stoul(f[4]))));
        }
        else if (f[0] == "A" && f.size() == 9) {
            NetDecimal sum;
            actual = decimal_add_checked(parse_bits(f, 1), parse_bits(f, 5), sum) ? bits_text(sum) : "OverflowException";
        }
        else if (f[0] == "R" && f.size() == 5) {
            int64_t rounded = 0;
            actual = decimal_round_away_to_int64(parse_bits(f, 1), rounded) ? std::to_string(rounded) : "OverflowException";
        }
        else { ++failed; continue; }
        if (actual == expected) ++passed;
        else {
            ++failed;
            if (reported++ < 5)
                std::printf("FAIL decimal op line %d: %s\n      expected: %s\n      actual:   %s\n", lineNumber,
                    clip(line.substr(0, equals)).c_str(), expected.c_str(), actual.c_str());
        }
    }
    return failed == 0;
}

#ifdef _WIN32
// Runs the capability probe; every failure is reported and swallowed so the
// exit code keeps reflecting parity only.
void run_capability_report(const std::string& reportPath, bool quick)
{
    try {
        std::wstring wide = win32::utf8_to_wide(reportPath);
        wchar_t full[MAX_PATH * 2] = {};
        DWORD n = GetFullPathNameW(wide.c_str(), MAX_PATH * 2, full, nullptr);
        std::wstring fullPath = (n && n < MAX_PATH * 2) ? std::wstring(full, n) : wide;
        size_t slash = fullPath.find_last_of(L"\\/");
        std::wstring folder = slash == std::wstring::npos ? std::wstring(L".") : fullPath.substr(0, slash);
        std::printf("\n");
        std::vector<std::string> machine;
        const auto probeStart = std::chrono::steady_clock::now();
        std::vector<std::string> human = tools::run_capability_probe(folder, machine, quick);
        for (const std::string& line : human) std::printf("%s\n", line.c_str());
        std::printf("Capability report elapsed: %.1f s%s\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - probeStart).count(),
            quick ? " (quick: 16 MB disk test)" : "");
        std::fflush(stdout);
        std::string text;
        for (const std::string& line : machine) text += line + "\r\n";
        std::string target = win32::wide_to_utf8(fullPath);
        if (!tools::write_file(target, text)) std::printf("[capability] could not write %s\n", target.c_str());
        else std::printf("[capability] machine-readable lines written to %s\n", target.c_str());
    }
    catch (...) {
        std::printf("[capability] the capability probe failed unexpectedly; the parity result above is unaffected\n");
    }
    std::fflush(stdout);
}
#endif

} // namespace

int MIDIBN_TOOL_MAIN(int argc, midibn::tools::ToolChar** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
#ifdef _WIN32
    if (args.size() == 2 && args[1] == "--has-sse2") {
        // For the XP self-test package: its launcher script decides from this
        // exit code whether the SSE2 build may run. Uses only plain x86
        // instructions in the /arch:IA32 build. PF_XMMI64 is available on XP.
        const bool present = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) != FALSE;
        std::printf("Processor SSE2 support (as reported by Windows): %s\n", present ? "yes" : "no");
        std::fflush(stdout);
        return present ? 0 : 1;
    }
#endif
    std::string base = tools::executable_directory();
    std::string fixtures = base + "\\fixtures";
    std::string expected = base + "\\expected";
    std::string rulesFile, baselineDir = base + "\\expected-v2", writeBaselineDir;
    bool rulesFileGiven = false;
    std::string large, largeExpected, failureDir, capabilityReport, largeModel, largeModelFixtures;
    std::vector<std::string> traceFixtures;   // --trace-fixtures: empty = every fixture
    bool capabilityQuick = false;
    tools::EngineCompareOptions engineOptions;
    engineOptions.scenario_file = base + "\\engine-scenarios.txt";
    bool runEngine = true;
    Context ctx;
    for (size_t i = 1; i < args.size(); i += 2) {
        if (args[i] == "--no-engine") { runEngine = false; i -= 1; continue; }
        if (args[i] == "--show-differences") { ctx.showAllDifferences = true; i -= 1; continue; }
        if (args[i] == "--capability-quick") { capabilityQuick = true; i -= 1; continue; }
        if (i + 1 >= args.size()) { std::fprintf(stderr, "missing value for %s\n", args[i].c_str()); return 2; }
        if (args[i] == "--fixtures") fixtures = args[i + 1];
        else if (args[i] == "--expected") expected = args[i + 1];
        else if (args[i] == "--rules") { rulesFile = args[i + 1]; rulesFileGiven = true; }
        else if (args[i] == "--baseline") baselineDir = args[i + 1];
        else if (args[i] == "--write-baseline") writeBaselineDir = args[i + 1];
        else if (args[i] == "--large") large = args[i + 1];
        else if (args[i] == "--large-expected") largeExpected = args[i + 1];
        else if (args[i] == "--large-model") largeModel = args[i + 1];
        else if (args[i] == "--large-model-fixtures") largeModelFixtures = args[i + 1];
        else if (args[i] == "--scenarios") engineOptions.scenario_file = args[i + 1];
        else if (args[i] == "--engine-only") engineOptions.only = args[i + 1];
        else if (args[i] == "--engine-v1") engineOptions.v1_dir = args[i + 1];
        else if (args[i] == "--engine-fixtures") {
            std::stringstream list(args[i + 1]);
            std::string item;
            while (std::getline(list, item, ',')) if (!item.empty()) engineOptions.fixtures.push_back(item);
        }
        else if (args[i] == "--trace-fixtures") {
            std::stringstream list(args[i + 1]);
            std::string item;
            while (std::getline(list, item, ',')) if (!item.empty()) traceFixtures.push_back(item);
        }
        else if (args[i] == "--model-prefix") {
            ctx.modelDivisor = std::atoi(args[i + 1].c_str());
            if (ctx.modelDivisor < 1) { std::fprintf(stderr, "--model-prefix needs a divisor >= 1\n"); return 2; }
        }
        else if (args[i] == "--engine-sample") {
            engineOptions.sample_stride = std::atoi(args[i + 1].c_str());
            size_t comma = args[i + 1].find(',');
            if (comma != std::string::npos) engineOptions.sample_offset = std::atoi(args[i + 1].c_str() + comma + 1);
            if (engineOptions.sample_stride < 1 || engineOptions.sample_offset < 0) {
                std::fprintf(stderr, "--engine-sample needs STRIDE[,OFFSET] with STRIDE >= 1\n");
                return 2;
            }
        }
        else if (args[i] == "--write-failures") failureDir = args[i + 1];
        else if (args[i] == "--capability-report") capabilityReport = args[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", args[i].c_str()); return 2; }
    }
    if (!rulesFileGiven) rulesFile = expected + "\\differences-from-v1.txt";
    ctx.writing = !writeBaselineDir.empty();
    if (ctx.writing && (!traceFixtures.empty() || ctx.modelDivisor > 1 || engineOptions.sample_stride > 1)) {
        std::fprintf(stderr, "--write-baseline needs the complete run, without --trace-fixtures, --model-prefix or --engine-sample\n");
        return 2;
    }
    if (ctx.writing && (!runEngine || !engineOptions.only.empty() || !engineOptions.fixtures.empty() || large.empty() ||
        largeModel.empty())) {
        std::fprintf(stderr, "--write-baseline needs the complete run: the engine with all scenarios and fixtures, --large and --large-model\n");
        return 2;
    }

    engineOptions.expected_dir = expected + "\\engine";
    const auto started = std::chrono::steady_clock::now();
    std::printf("MIDIBottleneck v2 parity check\n");
    std::printf("Operating system: %s\n", win32::describe_os().c_str());
    std::printf("Build: %s\n\n", tools::build_description().c_str());

    // The v1 difference rules.
    bool rulesLoaded = false;
    {
        std::string text, error;
        if (!tools::read_file(rulesFile, text)) std::printf("FAIL difference rules: cannot read %s\n", rulesFile.c_str());
        else if (!ctx.rules.load(text, error)) std::printf("FAIL difference rules %s: %s\n", rulesFile.c_str(), error.c_str());
        else rulesLoaded = true;
    }
    // The v2 baseline.
    ctx.baselineDir = baselineDir;
    if (!ctx.writing) {
        ctx.baselineLoaded = load_baseline(baselineDir, ctx);
        if (!ctx.baselineLoaded)
            std::printf("FAIL v2 baseline: %s\\traces.sha256 is missing or malformed; generate it with v2\\harness\\make-v2-baseline.ps1\n",
                baselineDir.c_str());
    }
    engineOptions.rules = &ctx.rules;
    if (ctx.writing) {
        tools::make_directory(writeBaselineDir);
        tools::make_directory(writeBaselineDir + "\\text");
        engineOptions.write_baseline_dir = writeBaselineDir + "\\engine";
    }
    else engineOptions.baseline_dir = baselineDir + "\\engine";

    Totals traces, flows, models;
    std::vector<std::string> names = tools::list_files(fixtures, { ".mid", ".rmi" });
    const int fixturesAvailable = static_cast<int>(names.size());
    if (!traceFixtures.empty()) {
        // A quick run: only the named fixtures (file name or stem), in the order of the folder.
        std::vector<std::string> chosen;
        for (const std::string& name : names)
            for (const std::string& wanted : traceFixtures)
                if (name == wanted || name.substr(0, name.find_last_of('.')) == wanted) { chosen.push_back(name); break; }
        for (const std::string& wanted : traceFixtures) {
            bool found = false;
            for (const std::string& name : chosen) if (name == wanted || name.substr(0, name.find_last_of('.')) == wanted) found = true;
            if (!found) { std::printf("FAIL --trace-fixtures: no fixture named %s in %s\n", wanted.c_str(), fixtures.c_str()); ++traces.failed; }
        }
        names = chosen;
        std::printf("Quick run: load and flow traces of %d of %d fixtures (the others are not exercised)\n\n",
            static_cast<int>(names.size()), fixturesAvailable);
    }
    const auto partStart = std::chrono::steady_clock::now();
    auto sinceStart = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - partStart).count(); };
    if (names.empty()) std::printf("FAIL no fixtures found in %s\n", fixtures.c_str());
    for (const std::string& name : names) {
        size_t dot = name.find_last_of('.');
        std::string stem = name.substr(0, dot) + "-" + name.substr(dot + 1);
        std::string path = fixtures + "\\" + name;
        compare_load(path, expected, stem + ".trace", false, name, ctx, failureDir, traces);
        for (int ps : { 4, 8 })
            compare_flow(path, expected, "flow\\" + stem + ".ptr" + std::to_string(ps) + ".trace", false, ps,
                name + " flow ptr" + std::to_string(ps), ctx, failureDir, flows);
    }
    if (!large.empty()) {
        std::string name = large.substr(large.find_last_of("\\/") + 1);
        size_t dot = name.find_last_of('.');
        std::string stem = name.substr(0, dot) + "-" + name.substr(dot + 1);
        std::string expectedDir = largeExpected.substr(0, largeExpected.find_last_of("\\/"));
        std::string traceName = largeExpected.substr(largeExpected.find_last_of("\\/") + 1);
        compare_load(large, expectedDir, traceName, true, name, ctx, failureDir, traces);
        for (int ps : { 4, 8 })
            compare_flow(large, expectedDir, "flow\\" + stem + ".ptr" + std::to_string(ps) + ".summary.trace", true, ps,
                name + " flow ptr" + std::to_string(ps), ctx, failureDir, flows);
    }

    const double secondsTraces = sinceStart();
    compare_models(fixtures, expected, ctx, failureDir, models);
    if (!largeModel.empty()) compare_large_model(largeModel, largeModelFixtures, ctx, failureDir, models);

    const double secondsModels = sinceStart() - secondsTraces;
    // Baseline entries that this run did not produce (a trace that disappeared) are stale; entries for the large
    // fixtures are only expected when the large fixtures were given. In a quick run (--trace-fixtures) the entries
    // of the fixtures left out are counted as not exercised instead.
    int baselineNotExercised = 0;
    if (ctx.baselineLoaded) {
        for (std::map<std::string, BaselineEntry>::const_iterator it = ctx.baseline.begin(); it != ctx.baseline.end(); ++it) {
            if (it->second.seen) continue;
            const std::string& key = it->first;
            bool largeLoad = key.compare(0, 6, "large-") == 0 || key.compare(0, 11, "flow/large-") == 0;
            bool largeModelEntry = key.compare(0, 13, "models-large/") == 0;
            if ((largeLoad && large.empty()) || (largeModelEntry && largeModel.empty())) continue;
            if (!traceFixtures.empty() && !largeModelEntry && key.compare(0, 7, "models/") != 0) { ++baselineNotExercised; continue; }
            if (ctx.modelDivisor > 1 && key.compare(0, 7, "models/") == 0) continue;
            std::printf("FAIL baseline %s: the v2 baseline has an entry that this run did not produce (a removed or renamed "
                "fixture or model?); regenerate the baseline (v2\\harness\\make-v2-baseline.ps1)\n", key.c_str());
            ++ctx.baselineFailed;
        }
    }

    tools::EngineCompareResult engine;
    if (baselineNotExercised > 0)
        std::printf("note v2 baseline: %d recorded traces belong to fixtures this quick run left out (not exercised)\n", baselineNotExercised);
    if (ctx.baselineSkipped > 0)
        std::printf("note v2 baseline: %d model traces were replayed in part only (quick run, first 1/%d of each section); their whole-trace digests are not exercised\n",
            ctx.baselineSkipped, ctx.modelDivisor);
    if (runEngine) {
        engineOptions.fixtures_dir = fixtures;
        engine = tools::run_engine_comparison(engineOptions);
        std::printf("%s engine scenarios: %d fixtures, %d runs, %d identical, %d differing from v1 without a rule, %d files "
            "explained by rules, %d differing from the v2 baseline, %d failed runs, %d setup failures (%.1f s)\n",
            engine.pass() ? "PASS" : "FAIL", engine.fixtures, engine.runs, engine.identical, engine.mismatched_files,
            engine.explained_files, engine.baseline_mismatches, engine.failed_runs, engine.setup_failures, engine.seconds);
        if (engine.skipped_runs > 0)
            std::printf("note engine scenarios: quick sample (every %dth pair, offset %d): %d of %d fixture/scenario pairs run, %d not exercised\n",
                engineOptions.sample_stride, engineOptions.sample_offset, engine.runs, engine.runs + engine.skipped_runs, engine.skipped_runs);
    }

    // Rules whose targets were compared must have matched something; the summary counts differences per register entry.
    if (ctx.modelDivisor > 1) ctx.rules.unexercise_unused_model_trace_rules();
    int staleRules = ctx.rules.finish(false);
    int explainedTraceDifferences = ctx.rules.total_trace_differences();

    const auto arithmeticStart = std::chrono::steady_clock::now();
    int vectorsPassed = 0, vectorsFailed = 0;
    check_vectors(expected + "\\decimal-vectors.txt", vectorsPassed, vectorsFailed);
    std::printf("exact integer clock vs .NET decimal: %d identical, %d differ at exact halfway ties (v1 rounded down)\n", g_exactSame, g_exactTies);
    std::printf("%s decimal vectors: %d passed, %d failed\n", vectorsFailed == 0 ? "PASS" : "FAIL",
        vectorsPassed, vectorsFailed);
    int opsPassed = 0, opsFailed = 0;
    check_ops(expected + "\\decimal-ops.txt", opsPassed, opsFailed);
    std::printf("%s decimal operations: %d passed, %d failed\n", opsFailed == 0 ? "PASS" : "FAIL", opsPassed, opsFailed);

    const double secondsArithmetic = std::chrono::duration<double>(std::chrono::steady_clock::now() - arithmeticStart).count();
    if (ctx.writing) {
        // Regeneration of the v2 baseline: written whether or not the comparisons above passed.
        tools::make_directory(writeBaselineDir);
        tools::make_directory(writeBaselineDir + "\\text");
        std::string digests = "# v2 trace baseline, written by v2/harness/make-v2-baseline.ps1: SHA-256 of each v2 trace with its comment "
            "lines removed. See v2/harness/TRACE_FORMAT.md (\"v2 baseline\").\n";
        for (std::map<std::string, std::string>::const_iterator it = ctx.produced.begin(); it != ctx.produced.end(); ++it)
            digests += it->first + " " + it->second + "\n";
        bool wrote = tools::write_file(writeBaselineDir + "\\traces.sha256", digests);
        for (std::map<std::string, std::string>::const_iterator it = ctx.producedText.begin(); it != ctx.producedText.end(); ++it)
            wrote = tools::write_file(writeBaselineDir + "\\text\\" + text_file_name(it->first), it->second) && wrote;
        std::printf("%s v2 baseline written to %s (%d trace digests, %d texts of traces that differ from v1)\n",
            wrote ? "OK" : "FAIL", writeBaselineDir.c_str(), static_cast<int>(ctx.produced.size()),
            static_cast<int>(ctx.producedText.size()));
        if (!wrote || engine.setup_failures != 0) return 1;
        return 0;
    }

    std::printf("%s v2 baseline: %d traces matched their recorded digests, %d did not%s\n",
        ctx.baselineFailed == 0 && ctx.baselineLoaded ? "PASS" : "FAIL", ctx.baselinePassed, ctx.baselineFailed,
        runEngine ? "; engine recordings are counted in the engine line" : "");

    bool pass = rulesLoaded && ctx.baselineLoaded && traces.failed == 0 && flows.failed == 0 && models.failed == 0 &&
        models.passed > 0 && !names.empty() && staleRules == 0 && ctx.baselineFailed == 0 && ctx.baselinePassed > 0 &&
        vectorsFailed == 0 && vectorsPassed > 0 && opsFailed == 0 && opsPassed > 0 && (!runEngine || engine.pass());
    std::printf("\nRESULT: %s  (traces %d passed, %d failed; flows %d passed, %d failed; models %d passed, %d failed; "
        "explained differences from v1 %d in traces, %d engine files; rules no longer occurring %d; v2 baseline %d passed, %d failed; "
        "decimal vectors %d passed, %d failed; decimal operations %d passed, %d failed; engine runs %d identical, %d differing)\n",
        pass ? "PASS" : "FAIL", traces.passed, traces.failed, flows.passed, flows.failed, models.passed, models.failed,
        explainedTraceDifferences, engine.explained_files, staleRules, ctx.baselinePassed, ctx.baselineFailed,
        vectorsPassed, vectorsFailed, opsPassed, opsFailed, engine.identical, engine.runs - engine.identical);
    std::printf("Operating system: %s\n", win32::describe_os().c_str());
    std::printf("Parts (seconds): traces and flows %.1f; models %.1f; engine scenarios %.1f; decimal arithmetic %.1f\n",
        secondsTraces, secondsModels, runEngine ? engine.seconds : 0.0, secondsArithmetic);
    std::printf("Elapsed: %.1f s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    std::fflush(stdout);
#ifdef _WIN32
    if (!capabilityReport.empty()) run_capability_report(capabilityReport, capabilityQuick);
#else
    if (!capabilityReport.empty()) std::printf("[capability] the capability report is available on Windows only\n");
    (void)capabilityQuick;
#endif
    return pass ? 0 : 1;
}
