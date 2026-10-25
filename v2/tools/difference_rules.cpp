#include "difference_rules.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace midibn {
namespace tools {

namespace {

std::string clip(const std::string& s) { return s.size() > 160 ? s.substr(0, 160) + "..." : s; }

bool glob_one(const char* p, const char* t)
{
    // Iterative wildcard match: '*' = any run of characters (including none), '?' = any one character.
    const char* star = nullptr;
    const char* resume = nullptr;
    while (*t) {
        if (*p == '*') { star = p++; resume = t; }
        else if (*p == '?' || *p == *t) { ++p; ++t; }
        else if (star) { p = star + 1; t = ++resume; }
        else return false;
    }
    while (*p == '*') ++p;
    return *p == 0;
}

std::string normalize_name(std::string name)
{
    for (char& c : name) if (c == '\\') c = '/';
    return name;
}

// ---- named normalizers (engine recordings) ----

// short-sysex-zeroed (DD-001): v1 reads a SysEx event of 1 to 3 stored bytes back as zeros, v2 keeps the bytes of
// the file. The only effect on a recording is the hex of the SEND line of such an event; v2's recording is mapped to
// what v1 would have recorded: the bytes of a "SEND SystemExclusive" line of 1 to 3 bytes become zeros.
std::string normalize_short_sysex_zeroed(const std::string& recording, int& changedLines)
{
    static const std::string marker = " SEND SystemExclusive src=";
    std::string result;
    result.reserve(recording.size());
    size_t start = 0;
    while (start < recording.size()) {
        size_t end = recording.find('\n', start);
        size_t stop = end == std::string::npos ? recording.size() : end;
        size_t at = recording.find(marker, start);
        bool changed = false;
        if (at != std::string::npos && at < stop) {
            size_t hexStart = recording.find(' ', at + marker.size());
            size_t hexEnd = hexStart == std::string::npos ? std::string::npos : recording.find(' ', hexStart + 1);
            if (hexStart != std::string::npos && hexEnd != std::string::npos && hexEnd <= stop) {
                size_t digits = hexEnd - (hexStart + 1);
                if (digits == 2 || digits == 4 || digits == 6) {
                    result.append(recording, start, hexStart + 1 - start);
                    result.append(digits, '0');
                    result.append(recording, hexEnd, stop - hexEnd);
                    changed = true;
                    ++changedLines;
                }
            }
        }
        if (!changed) result.append(recording, start, stop - start);
        if (end != std::string::npos) result.push_back('\n');
        start = stop + 1;
    }
    return result;
}

// ---- rule file tokens ----

struct Token {
    std::string text;
    bool is_regex = false;
};

// Whitespace-separated tokens; a token that starts with '/' is a regular expression that runs to the next '/'
// not preceded by a backslash (it may contain spaces). Returns false with `error` for an unterminated regex.
bool tokenize(const std::string& line, std::vector<Token>& tokens, std::string& error)
{
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ' || line[i] == '\t') { ++i; continue; }
        Token t;
        if (line[i] == '/') {
            size_t j = i + 1;
            bool closed = false;
            std::string body;
            while (j < line.size()) {
                if (line[j] == '\\' && j + 1 < line.size()) { body.push_back(line[j]); body.push_back(line[j + 1]); j += 2; continue; }
                if (line[j] == '/') { closed = true; ++j; break; }
                body.push_back(line[j++]);
            }
            if (!closed) { error = "regular expression is missing its closing '/'"; return false; }
            if (j < line.size() && line[j] != ' ' && line[j] != '\t') { error = "text directly after a regular expression's closing '/'"; return false; }
            t.text = body;
            t.is_regex = true;
            i = j;
        }
        else {
            size_t j = i;
            while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
            t.text = line.substr(i, j - i);
            i = j;
        }
        tokens.push_back(t);
    }
    return true;
}

bool valid_id(const std::string& id)
{
    if (id.size() < 6 || id.compare(0, 3, "DD-") != 0) return false;
    for (size_t i = 3; i < id.size(); ++i) if (id[i] < '0' || id[i] > '9') return false;
    return true;
}

bool compile(const std::string& pattern, std::regex& out, std::string& error)
{
    try {
        out = std::regex(pattern, std::regex::ECMAScript);
        return true;
    }
    catch (const std::regex_error& e) {
        error = std::string("bad regular expression /") + pattern + "/: " + e.what();
        return false;
    }
}

bool regex_whole(const std::regex& re, const std::string& line)
{
    try { return std::regex_match(line, re); }
    catch (const std::regex_error&) { return false; }   // the matcher gave up (too complex for the line): treated as no match
}

std::string regex_escape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// A trace-line rule matches when the whole v1 line matches the v1 regex and the whole v2 line matches the v2 regex;
// in the v2 regex, ${N} stands for the text the v1 regex captured in group N (so a rule can say "the same line except ..."),
// and ${N+1} for that text read as a number, plus one.
bool trace_line_rule_matches(const DifferenceRule& rule, const std::string& v1Line, const std::string& v2Line)
{
    std::smatch groups;
    try {
        if (!std::regex_match(v1Line, groups, rule.v1)) return false;
        if (!rule.v2_dynamic) return std::regex_match(v2Line, rule.v2);
        std::string pattern = rule.v2_pattern;
        for (size_t at = pattern.find("${"); at != std::string::npos; at = pattern.find("${", at)) {
            size_t close = pattern.find('}', at);
            const std::string placeholder = pattern.substr(at + 2, close - at - 2);
            size_t group = static_cast<size_t>(std::atoi(placeholder.c_str()));
            std::string captured = groups[group].str();
            if (placeholder.size() > 2 && placeholder.compare(placeholder.size() - 2, 2, "+1") == 0) {
                // ${N+1}: the captured number plus one (a time that is exactly one microsecond later).
                if (captured.empty() || captured.find_first_not_of("0123456789") != std::string::npos) return false;
                captured = std::to_string(std::stoll(captured) + 1);
            }
            std::string text = regex_escape(captured);
            pattern.replace(at, close - at + 1, text);
            at += text.size();
        }
        return std::regex_match(v2Line, std::regex(pattern, std::regex::ECMAScript));
    }
    catch (const std::regex_error&) { return false; }
}

} // namespace

bool DifferenceRules::glob_match(const std::string& pattern, const std::string& text)
{
    size_t start = 0;
    while (start <= pattern.size()) {
        size_t comma = pattern.find(',', start);
        std::string alt = pattern.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (glob_one(alt.c_str(), text.c_str())) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

Normalizer DifferenceRules::find_normalizer(const std::string& name)
{
    if (name == "short-sysex-zeroed") return &normalize_short_sysex_zeroed;
    return nullptr;
}

bool DifferenceRules::load(const std::string& text, std::string& error)
{
    rules_.clear();
    std::istringstream stream(text);
    std::string raw;
    int lineNumber = 0;
    bool versionSeen = false;
    while (std::getline(stream, raw)) {
        ++lineNumber;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        size_t first = raw.find_first_not_of(" \t");
        if (first == std::string::npos || raw[first] == '#') continue;
        std::vector<Token> tokens;
        std::string tokenError;
        auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(lineNumber) + ": " + why;
            return false;
        };
        if (!tokenize(raw, tokens, tokenError)) return fail(tokenError);
        if (tokens[0].text == "format") {
            if (tokens.size() != 2 || tokens[1].text != "1") return fail("unsupported rule file format (expected 'format 1')");
            versionSeen = true;
            continue;
        }
        if (!versionSeen) return fail("the rule file must start with 'format 1'");
        DifferenceRule rule;
        rule.source_line = lineNumber;
        rule.text = raw.substr(first);
        const std::string& kind = tokens[0].text;
        if (tokens.size() < 4 || tokens[0].is_regex || tokens[1].is_regex || tokens[2].is_regex)
            return fail("expected: <kind> <DD-id> <target> <arguments>");
        if (!valid_id(tokens[1].text)) return fail("'" + tokens[1].text + "' is not an improvement-register id (DD-NNN)");
        rule.id = tokens[1].text;
        if (kind == "trace" || kind == "insert" || kind == "delete") {
            rule.target = normalize_name(tokens[2].text);
            size_t regexes = kind == "trace" ? 2 : 1;
            if (tokens.size() != 3 + regexes) return fail("'" + kind + "' takes " + std::to_string(regexes) + " regular expression(s) in /slashes/");
            for (size_t k = 3; k < tokens.size(); ++k) if (!tokens[k].is_regex) return fail("expected a regular expression in /slashes/, found '" + tokens[k].text + "'");
            std::string reError;
            if (kind == "trace") {
                rule.kind = RuleKind::TraceLine;
                if (!compile(tokens[3].text, rule.v1, reError)) return fail(reError);
                rule.v2_pattern = tokens[4].text;
                rule.v2_dynamic = rule.v2_pattern.find("${") != std::string::npos;
                if (!rule.v2_dynamic) {
                    if (!compile(rule.v2_pattern, rule.v2, reError)) return fail(reError);
                }
                else {
                    // ${N} stands for the text the v1 regex captured in group N: check the group numbers and that the
                    // pattern compiles with every placeholder replaced by a sample.
                    std::string sample = rule.v2_pattern;
                    for (size_t at = sample.find("${"); at != std::string::npos; at = sample.find("${", at)) {
                        size_t close = sample.find('}', at);
                        if (close == std::string::npos || close == at + 2) return fail("a ${N} placeholder is not closed or has no group number");
                        int group = std::atoi(sample.substr(at + 2, close - at - 2).c_str());
                        if (group < 1 || group > static_cast<int>(rule.v1.mark_count()))
                            return fail("${" + std::to_string(group) + "} refers to a group the v1 regular expression does not have");
                        sample.replace(at, close - at + 1, "x");
                    }
                    std::regex probe;
                    if (!compile(sample, probe, reError)) return fail(reError);
                }
            }
            else if (kind == "insert") {
                rule.kind = RuleKind::TraceInsert;
                if (!compile(tokens[3].text, rule.v2, reError)) return fail(reError);
            }
            else {
                rule.kind = RuleKind::TraceDelete;
                if (!compile(tokens[3].text, rule.v1, reError)) return fail(reError);
            }
        }
        else if (kind == "engine") {
            rule.kind = RuleKind::Engine;
            size_t at = tokens[2].text.find('@');
            if (at == std::string::npos || at == 0 || at + 1 >= tokens[2].text.size())
                return fail("engine target must be <scenario glob>@<fixture glob>");
            rule.target = tokens[2].text.substr(0, at);
            rule.fixture_target = tokens[2].text.substr(at + 1);
            for (size_t k = 3; k < tokens.size(); ++k) {
                if (tokens[k].is_regex) return fail("engine rules take no regular expression");
                const std::string& arg = tokens[k].text;
                if (arg == "explained") rule.explained = true;
                else if (arg.compare(0, 10, "normalize=") == 0) rule.normalizer = arg.substr(10);
                else if (arg.compare(0, 6, "files=") == 0) {
                    std::string v = arg.substr(6);
                    if (v == "out") { rule.files_out = true; rule.files_sched = false; }
                    else if (v == "sched") { rule.files_out = false; rule.files_sched = true; }
                    else if (v == "both") { rule.files_out = rule.files_sched = true; }
                    else return fail("files= must be out, sched or both");
                }
                else if (arg.compare(0, 4, "pin=") == 0) {
                    std::string list = arg.substr(4);
                    size_t start = 0;
                    while (start < list.size()) {
                        size_t comma = list.find(',', start);
                        std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                        size_t colon = item.rfind(':');
                        if (colon == std::string::npos || colon == 0 || colon + 1 >= item.size()) return fail("pin= entries are <fixture>:<count>");
                        rule.pins[item.substr(0, colon)] = std::atoi(item.c_str() + colon + 1);
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }
                else return fail("unknown engine argument '" + arg + "'");
            }
            if (rule.explained == !rule.normalizer.empty()) return fail("an engine rule needs exactly one of normalize=<name> and explained");
            if (!rule.normalizer.empty() && !find_normalizer(rule.normalizer)) return fail("unknown normalizer '" + rule.normalizer + "'");
            if (!rule.pins.empty() && rule.normalizer.empty()) return fail("pin= applies to normalize= rules only");
        }
        else return fail("unknown rule kind '" + kind + "' (trace, insert, delete, engine)");
        rules_.push_back(rule);
    }
    if (!versionSeen) { error = "the rule file is empty or has no 'format 1' line"; return false; }
    return true;
}

TraceCompareResult DifferenceRules::compare_trace(const std::string& traceName, const std::vector<std::string>& v1,
    const std::vector<std::string>& v2)
{
    TraceCompareResult result;
    const std::string name = normalize_name(traceName);
    std::vector<DifferenceRule*> candidates;
    for (DifferenceRule& r : rules_) {
        if (r.kind == RuleKind::Engine) continue;
        if (!glob_match(r.target, name)) continue;
        r.exercised = true;
        candidates.push_back(&r);
    }
    size_t i = 0, j = 0;
    while (i < v1.size() || j < v2.size()) {
        if (i < v1.size() && j < v2.size() && v1[i] == v2[j]) { ++i; ++j; continue; }
        DifferenceRule* hit = nullptr;
        enum { Pair, Insert, Delete } how = Pair;
        if (i < v1.size() && j < v2.size())
            for (DifferenceRule* r : candidates)
                if (r->kind == RuleKind::TraceLine && trace_line_rule_matches(*r, v1[i], v2[j])) { hit = r; break; }
        if (!hit && j < v2.size()) {
            for (DifferenceRule* r : candidates)
                if (r->kind == RuleKind::TraceInsert && regex_whole(r->v2, v2[j])) { hit = r; how = Insert; break; }
        }
        if (!hit && i < v1.size()) {
            for (DifferenceRule* r : candidates)
                if (r->kind == RuleKind::TraceDelete && regex_whole(r->v1, v1[i])) { hit = r; how = Delete; break; }
        }
        if (!hit) {
            std::string where = i == j ? "line " + std::to_string(i + 1)
                : "v1 line " + std::to_string(i + 1) + " / v2 line " + std::to_string(j + 1);
            result.failure = where + "\n      expected (v1): " + (i < v1.size() ? clip(v1[i]) : std::string("<end>")) +
                "\n      actual   (v2): " + (j < v2.size() ? clip(v2[j]) : std::string("<end>"));
            result.ok = false;
            // The comparison stopped here: later differences of this trace were not checked.
            for (DifferenceRule* r : candidates) if (r->uses == 0) r->unchecked = true;
            return result;
        }
        AppliedDifference d;
        d.rule = hit;
        ++hit->uses;
        if (how != Insert) { d.v1_line = static_cast<int>(i + 1); d.v1_text = v1[i]; ++i; }
        if (how != Delete) { d.v2_line = static_cast<int>(j + 1); d.v2_text = v2[j]; ++j; }
        result.applied.push_back(d);
    }
    return result;
}

std::vector<DifferenceRule*> DifferenceRules::engine_rules(const std::string& scenario, const std::string& stem)
{
    std::vector<DifferenceRule*> found;
    for (DifferenceRule& r : rules_) {
        if (r.kind != RuleKind::Engine) continue;
        if (!glob_match(r.target, scenario) || !glob_match(r.fixture_target, stem)) continue;
        r.exercised = true;
        found.push_back(&r);
    }
    return found;
}

int DifferenceRules::total_trace_differences() const
{
    int n = 0;
    for (const DifferenceRule& r : rules_) if (r.kind != RuleKind::Engine) n += r.uses;
    return n;
}

void DifferenceRules::unexercise_unused_model_trace_rules()
{
    for (DifferenceRule& r : rules_)
        if (r.kind != RuleKind::Engine && r.uses == 0 && r.target.compare(0, 7, "models/") == 0) r.exercised = false;
}

int DifferenceRules::finish(bool engineOnly)
{
    int stale = 0;
    struct Counts { int rules = 0, traceLines = 0, inserted = 0, deleted = 0, engineLines = 0, engineRuns = 0, notExercised = 0; };
    std::map<std::string, Counts> perId;
    for (const DifferenceRule& r : rules_) {
        if (engineOnly && r.kind != RuleKind::Engine) continue;   // the engine harness alone does not run the traces
        Counts& c = perId[r.id];
        ++c.rules;
        if (!r.exercised) {
            ++c.notExercised;
            std::printf("note rule %s (rules file line %d) not exercised: none of its targets was run in this run (in a quick run, the sampled runs it covers were not changed by it)\n", r.id.c_str(), r.source_line);
            continue;
        }
        if (r.uses == 0 && !r.unchecked) {
            ++stale;
            std::printf("FAIL rule %s (rules file line %d: %s) no longer occurs: its targets were compared and nothing differs any more. "
                "Remove the rule, or restore the behaviour it explains.\n", r.id.c_str(), r.source_line, clip(r.text).c_str());
        }
        switch (r.kind) {
        case RuleKind::TraceLine: c.traceLines += r.uses; break;
        case RuleKind::TraceInsert: c.inserted += r.uses; break;
        case RuleKind::TraceDelete: c.deleted += r.uses; break;
        case RuleKind::Engine:
            if (r.explained) c.engineRuns += r.uses; else c.engineLines += r.lines;
            break;
        }
    }
    for (std::map<std::string, Counts>::const_iterator it = perId.begin(); it != perId.end(); ++it) {
        const Counts& c = it->second;
        std::string parts;
        auto add = [&](int n, const char* what) {
            if (n == 0) return;
            if (!parts.empty()) parts += ", ";
            parts += std::to_string(n) + " " + what;
        };
        add(c.traceLines, "changed trace lines");
        add(c.inserted, "added trace lines");
        add(c.deleted, "removed trace lines");
        add(c.engineLines, "engine recording lines normalized");
        add(c.engineRuns, "engine runs explained");
        if (parts.empty()) parts = "nothing found";
        std::printf("register %s: %s (%d rule%s%s)\n", it->first.c_str(), parts.c_str(), c.rules, c.rules == 1 ? "" : "s",
            c.notExercised ? (", " + std::to_string(c.notExercised) + " not exercised").c_str() : "");
    }
    return stale;
}

} // namespace tools
} // namespace midibn
