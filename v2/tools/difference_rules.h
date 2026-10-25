// The v1/v2 difference rules (v2/harness/expected/differences-from-v1.txt).
//
// v2 is not required to reproduce v1.0.44 exactly any more: every difference
// between v1's recorded output and v2's output must be explained by an entry
// of the improvement register (v2/docs/DIFFERENCES_FROM_V1.md, ids DD-NNN),
// and each entry names the rules that classify the resulting differences.
// This module reads the rule file and applies the rules for the parity
// runner (midibn-parity, v2-xp-selftest) and the engine comparison.
// The grammar is documented in v2/harness/TRACE_FORMAT.md ("Difference rules").
//
// Rule kinds
//   trace   <id> <trace glob> /<v1 regex>/ /<v2 regex>/   lines that differ at the same position
//   insert  <id> <trace glob> /<v2 regex>/                a v2 line that v1 does not have
//   delete  <id> <trace glob> /<v1 regex>/                a v1 line that v2 no longer has
//   engine  <id> <scenario glob>@<fixture glob> normalize=<name> [pin=<fixture>:<n>,...] [files=out|sched|both]
//   engine  <id> <scenario glob>@<fixture glob> explained [files=out|sched|both]
#pragma once

#include <map>
#include <regex>
#include <string>
#include <vector>

namespace midibn {
namespace tools {

enum class RuleKind { TraceLine, TraceInsert, TraceDelete, Engine };

struct DifferenceRule {
    RuleKind kind = RuleKind::TraceLine;
    std::string id;                   // DD-NNN
    int source_line = 0;              // line in the rule file
    std::string text;                 // the rule as written (for messages)
    std::string target;               // trace glob, or scenario glob (engine)
    std::string fixture_target;       // fixture glob (engine)
    std::regex v1, v2;
    std::string v2_pattern;           // trace rule: the v2 regex as written
    bool v2_dynamic = false;          // the v2 regex uses ${N} (text captured by group N of the v1 regex)
    // engine
    std::string normalizer;           // named normalizer applied to v2's .out recording before the comparison
    bool explained = false;           // v2's recording may differ from v1's (it must still equal the v2 baseline)
    bool files_out = true;            // which recording files the rule covers
    bool files_sched = true;
    std::map<std::string, int> pins;  // fixture stem -> number of recording lines the normalizer must change
    // bookkeeping
    bool exercised = false;           // some compared target matched the rule's glob
    bool unchecked = false;           // the comparison of a matching target stopped at a failure before this rule could be used
    int uses = 0;                     // differences / runs the rule explained
    int lines = 0;                    // engine: recording lines changed by the normalizer
    std::map<std::string, int> lines_by_fixture;
};

// What one trace comparison did.
struct AppliedDifference {
    const DifferenceRule* rule = nullptr;
    int v1_line = 0;                  // 1-based, after comment lines are removed; 0 = none
    int v2_line = 0;
    std::string v1_text, v2_text;
};

struct TraceCompareResult {
    bool ok = true;
    std::string failure;              // first unexplained difference, multi-line text
    std::vector<AppliedDifference> applied;
};

typedef std::string (*Normalizer)(const std::string& recording, int& changed_lines);

class DifferenceRules {
public:
    // Parses the rule file text. On error returns false and sets `error` (with the line number).
    bool load(const std::string& text, std::string& error);

    // Compares two traces (comment lines already removed, one string per line).
    // Lines may differ only where a rule explains the difference.
    TraceCompareResult compare_trace(const std::string& trace_name, const std::vector<std::string>& v1,
        const std::vector<std::string>& v2);

    // Engine recordings: the rules that cover (scenario, fixture stem) in rule-file order.
    std::vector<DifferenceRule*> engine_rules(const std::string& scenario, const std::string& stem);
    static Normalizer find_normalizer(const std::string& name);

    // Prints one FAIL line per stale rule ("no longer occurring"), one note per rule that was not exercised,
    // and the per-id counts. Returns the number of failures (stale rules).
    // engine_only: the caller ran only the engine comparison, so trace rules are left out of the report.
    int finish(bool engine_only);

    // Quick run (--model-prefix): a trace rule for a model trace that explained nothing may describe a line in
    // the part of the trace that was cut, so it is reported as not exercised instead of stale.
    void unexercise_unused_model_trace_rules();

    size_t size() const { return rules_.size(); }
    int total_trace_differences() const;

    static bool glob_match(const std::string& pattern, const std::string& text);   // * ? and comma-separated alternatives

private:
    std::vector<DifferenceRule> rules_;
};

} // namespace tools
} // namespace midibn
