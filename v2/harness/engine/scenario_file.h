// Parser for the committed data-driven scenario file
// (v2/harness/engine-scenarios.txt), a port of the v1 reference harness's
// ScenarioFile.cs. The grammar and every key are specified in
// v2/harness/ENGINE_SCENARIOS.md. This file only reads text; scenario_binder.h
// turns the parsed steps into calls on the v2 engine.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace midibn {
namespace harness {

// A command or position time: a sum of terms, each either "N/M" (the integer
// D * N / M, multiplication first, truncating division, D = max(1000, song
// duration in microseconds)) or a plain unsigned integer (absolute
// microseconds). Written "4/10+50000".
class ScenarioTime {
public:
    // Returns false and fills `error` (the text after "bad ..."/"empty time").
    static bool parse(const std::string& text, ScenarioTime& result, std::string& error);
    int64_t evaluate(int64_t duration) const;

private:
    std::vector<std::pair<int64_t, int64_t>> terms_;  // {numerator, denominator}; denominator 0 = absolute
};

// One directive line (setup directive or "at" command), tokenised.
struct ScenarioStep {
    int line = 0;
    bool has_at = false;
    ScenarioTime at;
    std::string verb;
    std::vector<std::string> args;
};

struct ScenarioDefinition {
    std::string name;
    int line = 0;
    std::string description;
    int snapshots = 0;               // 0 none, 1 on, 2 full
    bool stop_after_end = true;
    bool has_route = false;
    ScenarioStep route;              // valid when has_route
    std::vector<ScenarioStep> setup;
    std::vector<ScenarioStep> commands;
};

// Parses the whole file text (UTF-8, LF or CRLF). On failure returns false
// and `error` is "<path> line <n>: <reason>" (the v1 reference harness's text).
bool load_scenario_file(const std::string& text, const std::string& path, std::vector<ScenarioDefinition>& scenarios,
    std::string& error);

} // namespace harness
} // namespace midibn
