#include "scenario_file.h"

#include <map>
#include <set>

namespace midibn {
namespace harness {

namespace {

bool parse_unsigned(const std::string& text, int64_t maximum, int64_t& value)
{
    if (text.empty() || text.size() > 15) return false;
    int64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v > maximum) return false;
    value = v;
    return true;
}

struct FormatError {
    std::string message;
};

void require_args(const std::string& verb, const std::vector<std::string>& args, size_t count)
{
    if (args.size() != count)
        throw FormatError{ "'" + verb + "' takes " + std::to_string(count) + " argument(s)" };
}

void require_name(const std::string& name)
{
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            throw FormatError{ "name '" + name + "' must be lower-case letters, digits and '-'" };
    if (name.empty()) throw FormatError{ "empty name" };
}

ScenarioStep make_step(int line, const std::string& verb, const std::vector<std::string>& args)
{
    ScenarioStep step;
    step.line = line;
    step.verb = verb;
    step.args = args;
    return step;
}

void add_setup(ScenarioDefinition& scenario, const ScenarioStep& step)
{
    if (step.verb == "route") {
        if (scenario.has_route) throw FormatError{ "more than one 'route' in a scenario" };
        scenario.has_route = true;
        scenario.route = step;
        return;
    }
    scenario.setup.push_back(step);
}

} // namespace

bool ScenarioTime::parse(const std::string& text, ScenarioTime& result, std::string& error)
{
    if (text.empty()) { error = "empty time"; return false; }
    result.terms_.clear();
    size_t start = 0;
    for (;;) {
        size_t plus = text.find('+', start);
        std::string term = text.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        size_t slash = term.find('/');
        if (slash == std::string::npos) {
            int64_t v;
            if (!parse_unsigned(term, 1000000000000LL, v)) { error = "bad unsigned integer '" + term + "' in time"; return false; }
            result.terms_.push_back({ v, 0 });
        }
        else {
            int64_t n, d;
            std::string ns = term.substr(0, slash), ds = term.substr(slash + 1);
            if (!parse_unsigned(ns, 1000000, n)) { error = "bad unsigned integer '" + ns + "' in time"; return false; }
            if (!parse_unsigned(ds, 1000000, d)) { error = "bad unsigned integer '" + ds + "' in time"; return false; }
            if (d == 0) { error = "zero denominator in time '" + text + "'"; return false; }
            result.terms_.push_back({ n, d });
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

int64_t ScenarioTime::evaluate(int64_t duration) const
{
    int64_t total = 0;
    for (const std::pair<int64_t, int64_t>& term : terms_)
        total += term.second == 0 ? term.first : duration * term.first / term.second;
    return total;
}

bool load_scenario_file(const std::string& rawText, const std::string& path, std::vector<ScenarioDefinition>& scenarios,
    std::string& error)
{
    // CRLF and lone CR become LF; then split at LF.
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
    // A UTF-8 byte order mark is not text.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);

    scenarios.clear();
    std::map<std::string, std::vector<ScenarioStep>> profiles;
    std::set<std::string> scenarioNames;
    int current = -1;                 // index into scenarios
    bool inProfile = false;
    std::vector<ScenarioStep> profile;
    std::string profileName;
    bool sawFormat = false;
    int lineNumber = 0;
    size_t position = 0;
    while (position <= text.size()) {
        size_t newline = text.find('\n', position);
        std::string line = text.substr(position, newline == std::string::npos ? std::string::npos : newline - position);
        position = newline == std::string::npos ? text.size() + 1 : newline + 1;
        ++lineNumber;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::vector<std::string> tokens;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            size_t s = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            if (i > s) tokens.push_back(line.substr(s, i - s));
        }
        if (tokens.empty()) continue;
        std::string verb = tokens[0];
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());
        try {
            if (!sawFormat) {
                if (verb != "format" || args.size() != 1 || args[0] != "1")
                    throw FormatError{ "the first directive must be 'format 1'" };
                sawFormat = true;
                continue;
            }
            if (verb == "scenario") {
                if (inProfile) throw FormatError{ "'scenario' inside a profile" };
                require_args(verb, args, 1);
                require_name(args[0]);
                if (!scenarioNames.insert(args[0]).second) throw FormatError{ "duplicate scenario '" + args[0] + "'" };
                ScenarioDefinition d;
                d.name = args[0];
                d.line = lineNumber;
                scenarios.push_back(d);
                current = static_cast<int>(scenarios.size()) - 1;
                continue;
            }
            if (verb == "profile") {
                if (inProfile) throw FormatError{ "nested 'profile'" };
                require_args(verb, args, 1);
                require_name(args[0]);
                if (profiles.count(args[0]) != 0) throw FormatError{ "duplicate profile '" + args[0] + "'" };
                profileName = args[0];
                inProfile = true;
                profile.clear();
                current = -1;
                continue;
            }
            if (verb == "end-profile") {
                if (!inProfile) throw FormatError{ "'end-profile' outside a profile" };
                profiles[profileName] = profile;
                inProfile = false;
                continue;
            }
            if (inProfile) {
                if (verb == "at" || verb == "use" || verb == "describe" || verb == "snapshots" || verb == "stop-after-end")
                    throw FormatError{ "'" + verb + "' is not allowed inside a profile" };
                profile.push_back(make_step(lineNumber, verb, args));
                continue;
            }
            if (current < 0) throw FormatError{ "'" + verb + "' before the first scenario" };
            ScenarioDefinition& scenario = scenarios[static_cast<size_t>(current)];
            if (verb == "describe") {
                std::string joined;
                for (size_t k = 0; k < args.size(); ++k) { if (k != 0) joined += " "; joined += args[k]; }
                scenario.description = joined;
            }
            else if (verb == "snapshots") {
                require_args(verb, args, 1);
                if (args[0] == "none") scenario.snapshots = 0;
                else if (args[0] == "on") scenario.snapshots = 1;
                else if (args[0] == "full") scenario.snapshots = 2;
                else throw FormatError{ "snapshots takes none|on|full" };
            }
            else if (verb == "stop-after-end") {
                require_args(verb, args, 1);
                if (args[0] == "on") scenario.stop_after_end = true;
                else if (args[0] == "off") scenario.stop_after_end = false;
                else throw FormatError{ "stop-after-end takes on|off" };
            }
            else if (verb == "use") {
                require_args(verb, args, 1);
                if (!scenario.commands.empty()) throw FormatError{ "'use' after the first 'at'" };
                std::map<std::string, std::vector<ScenarioStep>>::const_iterator used = profiles.find(args[0]);
                if (used == profiles.end()) throw FormatError{ "unknown profile '" + args[0] + "'" };
                for (const ScenarioStep& step : used->second) add_setup(scenario, step);
            }
            else if (verb == "at") {
                if (args.size() < 2) throw FormatError{ "'at' takes a time and a command" };
                ScenarioStep command;
                command.line = lineNumber;
                command.has_at = true;
                std::string timeError;
                if (!ScenarioTime::parse(args[0], command.at, timeError)) throw FormatError{ timeError };
                command.verb = args[1];
                command.args.assign(args.begin() + 2, args.end());
                scenario.commands.push_back(command);
            }
            else {
                if (!scenario.commands.empty()) throw FormatError{ "setup directive '" + verb + "' after the first 'at'" };
                add_setup(scenario, make_step(lineNumber, verb, args));
            }
        }
        catch (const FormatError& ex) {
            error = path + " line " + std::to_string(lineNumber) + ": " + ex.message;
            return false;
        }
    }
    if (inProfile) { error = path + ": profile '" + profileName + "' is not closed with end-profile"; return false; }
    if (!sawFormat) { error = path + ": missing 'format 1'"; return false; }
    return true;
}

} // namespace harness
} // namespace midibn
