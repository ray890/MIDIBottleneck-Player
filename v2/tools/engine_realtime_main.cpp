// midibn-engine-realtime: real-time property-check driver for the v2 playback
// engine (Milestone 5 exit test; v2/docs/ENGINE_PROPERTY_CHECKS.md).
//
// The counterpart of v1-realtime.exe (v2/harness/v1-reference/realtime/): the
// same scenario file and vocabulary (v2/harness/ENGINE_SCENARIOS.md, with every
// time REAL elapsed microseconds since the run began), the same log format, but
// the v2 engine over the Win32 EngineServices (QueryPerformanceCounter clock,
// waitable-timer waiter, kernel events, real threads) and a recording output
// that sends nowhere: no MIDI device is opened and no provider DLL is loaded.
//
// Usage:
//   midibn-engine-realtime list   <scenarioFile>
//   midibn-engine-realtime events <fixture.mid> <outFile>        (the song's event list)
//   midibn-engine-realtime run    <scenarioFile> <scenario> <fixture.mid> <outLog>
#include "midibn/engine/playback_engine.h"
#include "midibn/midi_loader.h"
#include "midibn/null_output.h"
#include "tool_support.h"
#include "win32_platform.h"
#include "win32_timing.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace midibn;

namespace {

// ---------------------------------------------------------------- scenario file (ScenarioFile.cs)

struct ScenarioTime {
    // {numerator, denominator}; denominator 0 = absolute microseconds.
    std::vector<std::pair<int64_t, int64_t>> terms;

    static int64_t parse_unsigned(const std::string& text, int64_t maximum)
    {
        if (text.empty() || text.size() > 15) throw std::runtime_error("bad unsigned integer '" + text + "' in time");
        int64_t value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') throw std::runtime_error("bad unsigned integer '" + text + "' in time");
            value = value * 10 + (c - '0');
        }
        if (value > maximum) throw std::runtime_error("bad unsigned integer '" + text + "' in time");
        return value;
    }
    static ScenarioTime parse(const std::string& text)
    {
        if (text.empty()) throw std::runtime_error("empty time");
        ScenarioTime time;
        size_t start = 0;
        while (true) {
            size_t plus = text.find('+', start);
            std::string term = text.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            size_t slash = term.find('/');
            if (slash == std::string::npos)
                time.terms.push_back({ parse_unsigned(term, 1000000000000LL), 0 });
            else {
                int64_t n = parse_unsigned(term.substr(0, slash), 1000000);
                int64_t d = parse_unsigned(term.substr(slash + 1), 1000000);
                if (d == 0) throw std::runtime_error("zero denominator in time '" + text + "'");
                time.terms.push_back({ n, d });
            }
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        return time;
    }
    int64_t evaluate(int64_t duration) const
    {
        int64_t total = 0;
        for (const auto& t : terms) total += t.second == 0 ? t.first : duration * t.first / t.second;
        return total;
    }
};

struct Step {
    int line = 0;
    bool has_at = false;
    ScenarioTime at;
    std::string verb;
    std::vector<std::string> args;
};

struct Definition {
    std::string name;
    std::string description;
    bool stop_after_end = true;
    bool has_route = false;
    int route_line = 0;
    std::vector<Step> setup;
    std::vector<Step> commands;
};

std::vector<std::string> split_tokens(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) { tokens.push_back(current); current.clear(); }
        }
        else current.push_back(c);
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

void require_name(const std::string& name)
{
    if (name.empty()) throw std::runtime_error("empty name");
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            throw std::runtime_error("name '" + name + "' must be lower-case letters, digits and '-'");
}

std::vector<Definition> load_scenarios(const std::string& path)
{
    std::string content;
    if (!tools::read_file(path, content)) throw std::runtime_error("cannot read " + path);
    std::vector<std::string> lines;
    {
        std::string current;
        for (size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c == '\r') { if (i + 1 < content.size() && content[i + 1] == '\n') ++i; lines.push_back(current); current.clear(); }
            else if (c == '\n') { lines.push_back(current); current.clear(); }
            else current.push_back(c);
        }
        lines.push_back(current);
    }
    std::vector<Definition> scenarios;
    std::map<std::string, std::vector<Step>> profiles;
    Definition* current = nullptr;
    std::vector<Step>* profile = nullptr;
    std::string profile_name;
    bool saw_format = false;
    for (size_t index = 0; index < lines.size(); ++index) {
        int line_number = static_cast<int>(index) + 1;
        std::string text = lines[index];
        size_t hash = text.find('#');
        if (hash != std::string::npos) text = text.substr(0, hash);
        std::vector<std::string> tokens = split_tokens(text);
        if (tokens.empty()) continue;
        std::string verb = tokens[0];
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());
        try {
            if (!saw_format) {
                if (verb != "format" || args.size() != 1 || args[0] != "1") throw std::runtime_error("the first directive must be 'format 1'");
                saw_format = true;
                continue;
            }
            auto make_step = [&](const std::string& v, const std::vector<std::string>& a) {
                Step s; s.line = line_number; s.verb = v; s.args = a; return s;
            };
            auto add_setup = [&](Definition& d, const Step& s) {
                if (s.verb == "route") {
                    if (d.has_route) throw std::runtime_error("more than one 'route' in a scenario");
                    d.has_route = true; d.route_line = s.line;
                    return;
                }
                d.setup.push_back(s);
            };
            if (verb == "scenario") {
                if (profile) throw std::runtime_error("'scenario' inside a profile");
                if (args.size() != 1) throw std::runtime_error("'scenario' takes 1 argument(s)");
                require_name(args[0]);
                for (const Definition& d : scenarios) if (d.name == args[0]) throw std::runtime_error("duplicate scenario '" + args[0] + "'");
                scenarios.push_back(Definition());
                current = &scenarios.back();
                current->name = args[0];
                continue;
            }
            if (verb == "profile") {
                if (profile) throw std::runtime_error("nested 'profile'");
                if (args.size() != 1) throw std::runtime_error("'profile' takes 1 argument(s)");
                require_name(args[0]);
                if (profiles.count(args[0])) throw std::runtime_error("duplicate profile '" + args[0] + "'");
                profile_name = args[0];
                profile = &profiles[profile_name];
                current = nullptr;
                continue;
            }
            if (verb == "end-profile") {
                if (!profile) throw std::runtime_error("'end-profile' outside a profile");
                profile = nullptr;
                continue;
            }
            if (profile) {
                if (verb == "at" || verb == "use" || verb == "describe" || verb == "snapshots" || verb == "stop-after-end")
                    throw std::runtime_error("'" + verb + "' is not allowed inside a profile");
                profile->push_back(make_step(verb, args));
                continue;
            }
            if (!current) throw std::runtime_error("'" + verb + "' before the first scenario");
            if (verb == "describe") {
                std::string joined;
                for (size_t i = 0; i < args.size(); ++i) joined += (i ? " " : "") + args[i];
                current->description = joined;
            }
            else if (verb == "snapshots") {
                if (args.size() != 1) throw std::runtime_error("'snapshots' takes 1 argument(s)");
                if (args[0] != "none" && args[0] != "on" && args[0] != "full") throw std::runtime_error("snapshots takes none|on|full");
            }
            else if (verb == "stop-after-end") {
                if (args.size() != 1) throw std::runtime_error("'stop-after-end' takes 1 argument(s)");
                if (args[0] == "on") current->stop_after_end = true;
                else if (args[0] == "off") current->stop_after_end = false;
                else throw std::runtime_error("stop-after-end takes on|off");
            }
            else if (verb == "use") {
                if (args.size() != 1) throw std::runtime_error("'use' takes 1 argument(s)");
                if (!current->commands.empty()) throw std::runtime_error("'use' after the first 'at'");
                auto found = profiles.find(args[0]);
                if (found == profiles.end()) throw std::runtime_error("unknown profile '" + args[0] + "'");
                for (const Step& s : found->second) add_setup(*current, s);
            }
            else if (verb == "at") {
                if (args.size() < 2) throw std::runtime_error("'at' takes a time and a command");
                Step command;
                command.line = line_number;
                command.has_at = true;
                command.at = ScenarioTime::parse(args[0]);
                command.verb = args[1];
                command.args.assign(args.begin() + 2, args.end());
                current->commands.push_back(command);
            }
            else {
                if (!current->commands.empty()) throw std::runtime_error("setup directive '" + verb + "' after the first 'at'");
                add_setup(*current, make_step(verb, args));
            }
        }
        catch (const std::runtime_error& ex) {
            throw std::runtime_error(path + " line " + std::to_string(line_number) + ": " + ex.what());
        }
    }
    if (profile) throw std::runtime_error(path + ": profile '" + profile_name + "' is not closed with end-profile");
    if (!saw_format) throw std::runtime_error(path + ": missing 'format 1'");
    return scenarios;
}

// ---------------------------------------------------------------- binder (ScenarioBinder.cs)

using engine::PlaybackEngine;
using engine::PlaybackState;
using engine::ProcessingMode;
using models::OverflowPolicy;
using models::ServiceDurationMode;

struct Ctx {
    PlaybackEngine* engine = nullptr;
    std::shared_ptr<const engine::PlaybackSong> song;
    std::shared_ptr<platform::MidiOutput> output;
};

std::string kebab(const std::string& camel)
{
    std::string out;
    for (size_t i = 0; i < camel.size(); ++i) {
        char c = camel[i];
        if (c >= 'A' && c <= 'Z') { if (i > 0) out.push_back('-'); out.push_back(static_cast<char>(c - 'A' + 'a')); }
        else out.push_back(c);
    }
    return out;
}

struct EnumValue { const char* name; int64_t value; };

const EnumValue kOverflowPolicies[] = { { "DropNewest", 0 }, { "DropOldest", 1 }, { "ClearBufferAndCatchUp", 2 },
    { "DropIncomingCompleteNotes", 3 }, { "DropOldestCompleteNote", 4 }, { "DropBackgroundNotes", 5 } };
const EnumValue kServiceModes[] = { { "ProcessingTime", 0 }, { "MidiBitrate", 1 }, { "EventsPerSecond", 2 } };
const EnumValue kProcessingModes[] = { { "Queue", 0 }, { "Drop", 1 }, { "PerNoteIntervalGate", 2 }, { "DelayedNoteOff", 3 },
    { "NoteAttackLimit", 4 }, { "OutputRateLimit", 5 } };
const EnumValue kAttributes[] = { { "BankMsb", 0 }, { "BankLsb", 1 }, { "Program", 2 }, { "Volume", 3 }, { "Expression", 4 },
    { "Pan", 5 }, { "Sustain", 6 }, { "PitchBend", 7 }, { "Aftertouch", 8 } };

template <size_t N>
EnumValue parse_enum(const EnumValue (&table)[N], const std::string& token, const char* what)
{
    for (const EnumValue& e : table)
        if (kebab(e.name) == token) return e;
    throw std::runtime_error(std::string("unknown ") + what + " '" + token + "'");
}

int64_t parse_number(const std::string& token, int64_t min, int64_t max, const std::string& what)
{
    if (token.empty()) throw std::runtime_error("bad " + what + " '" + token + "'");
    size_t i = 0;
    bool negative = false;
    if (token[0] == '-' || token[0] == '+') { negative = token[0] == '-'; i = 1; }
    if (i >= token.size() || token.size() - i > 18) throw std::runtime_error("bad " + what + " '" + token + "'");
    int64_t value = 0;
    for (; i < token.size(); ++i) {
        if (token[i] < '0' || token[i] > '9') throw std::runtime_error("bad " + what + " '" + token + "'");
        value = value * 10 + (token[i] - '0');
    }
    if (negative) value = -value;
    if (value < min || value > max) throw std::runtime_error("bad " + what + " '" + token + "'");
    return value;
}

struct Property {
    std::string member;
    std::string key;
    char kind;   // 'b' on|off, 'n' integer, 'e' enum
    const EnumValue* enum_table = nullptr;
    size_t enum_count = 0;
    std::function<void(PlaybackEngine&, int64_t)> set;
};

std::map<std::string, Property> build_properties()
{
    std::map<std::string, Property> map;
    auto add = [&](const char* member, char kind, std::function<void(PlaybackEngine&, int64_t)> set,
                   const EnumValue* table = nullptr, size_t count = 0) {
        Property p; p.member = member; p.key = kebab(member); p.kind = kind; p.set = std::move(set);
        p.enum_table = table; p.enum_count = count;
        map[p.key] = p;
    };
    add("SimulateSlowdown", 'b', [](PlaybackEngine& e, int64_t v) { e.set_simulate_slowdown(v != 0); });
    add("ApplyQueueLimitWithoutSlowdown", 'b', [](PlaybackEngine& e, int64_t v) { e.set_apply_queue_limit_without_slowdown(v != 0); });
    add("ChaseMidiStateOnPlaySeek", 'b', [](PlaybackEngine& e, int64_t v) { e.set_chase_midi_state_on_play_seek(v != 0); });
    add("PreservePlaybackQueueOnPauseSeek", 'b', [](PlaybackEngine& e, int64_t v) { e.set_preserve_playback_queue_on_pause_seek(v != 0); });
    add("OutputPacedTimeline", 'b', [](PlaybackEngine& e, int64_t v) { e.set_output_paced_timeline(v != 0); });
    add("ScaleNoteIntervalByTraffic", 'b', [](PlaybackEngine& e, int64_t v) { e.set_scale_note_interval_by_traffic(v != 0); });
    add("IntervalPolyphonyLimitEnabled", 'b', [](PlaybackEngine& e, int64_t v) { e.set_interval_polyphony_limit_enabled(v != 0); });
    add("IntervalPolyphonyLimit", 'n', [](PlaybackEngine& e, int64_t v) { e.set_interval_polyphony_limit(static_cast<int32_t>(v)); });
    add("QueueLengthLimit", 'n', [](PlaybackEngine& e, int64_t v) { e.set_queue_length_limit(static_cast<int32_t>(v)); });
    add("QueueAgeLimitEnabled", 'b', [](PlaybackEngine& e, int64_t v) { e.set_queue_age_limit_enabled(v != 0); });
    add("QueueAgeLimitMicroseconds", 'n', [](PlaybackEngine& e, int64_t v) { e.set_queue_age_limit_microseconds(v); });
    add("OverflowPolicy", 'e', [](PlaybackEngine& e, int64_t v) { e.set_overflow_policy(static_cast<OverflowPolicy>(v)); },
        kOverflowPolicies, sizeof(kOverflowPolicies) / sizeof(kOverflowPolicies[0]));
    add("ServiceDurationMode", 'e', [](PlaybackEngine& e, int64_t v) { e.set_service_duration_mode(static_cast<ServiceDurationMode>(v)); },
        kServiceModes, sizeof(kServiceModes) / sizeof(kServiceModes[0]));
    add("ProcessingMicroseconds", 'n', [](PlaybackEngine& e, int64_t v) { e.set_processing_microseconds(v); });
    add("MidiBitrate", 'n', [](PlaybackEngine& e, int64_t v) { e.set_midi_bitrate(v); });
    add("EventsPerSecond", 'n', [](PlaybackEngine& e, int64_t v) { e.set_events_per_second(v); });
    return map;
}

const std::map<std::string, Property>& properties()
{
    static const std::map<std::string, Property> map = build_properties();
    return map;
}

const Property& find_property(const std::string& key)
{
    auto it = properties().find(key);
    if (it == properties().end()) throw std::runtime_error("unknown setting key '" + key + "'");
    return it->second;
}

int64_t parse_value(const Property& p, const std::string& token, std::string& log_text)
{
    if (p.kind == 'b') {
        if (token == "on") { log_text = "1"; return 1; }
        if (token == "off") { log_text = "0"; return 0; }
        throw std::runtime_error(p.key + " takes on|off");
    }
    if (p.kind == 'e') {
        for (size_t i = 0; i < p.enum_count; ++i)
            if (kebab(p.enum_table[i].name) == token) { log_text = p.enum_table[i].name; return p.enum_table[i].value; }
        throw std::runtime_error("unknown value '" + token + "' for " + p.key);
    }
    int64_t value = parse_number(token, INT64_MIN + 1, INT64_MAX, p.key + " value");
    log_text = std::to_string(value);
    return value;
}

void need(const Step& step, size_t min, size_t max)
{
    if (step.args.size() < min || step.args.size() > max)
        throw std::runtime_error("'" + step.verb + "' takes " + (min == max ? std::to_string(min) : std::to_string(min) + " to " + std::to_string(max)) + " argument(s)");
}

struct Command {
    int64_t offset = 0;
    std::string name;
    std::function<engine::EngineError(Ctx&)> apply;
};

struct ServiceArgs { ServiceDurationMode mode; int64_t processing, bitrate, rate; };
ServiceArgs parse_service(const Step& step)
{
    need(step, 4, 4);
    ServiceArgs a;
    a.mode = static_cast<ServiceDurationMode>(parse_enum(kServiceModes, step.args[0], "service mode").value);
    a.processing = parse_number(step.args[1], INT64_MIN + 1, INT64_MAX, "processing microseconds");
    a.bitrate = parse_number(step.args[2], INT64_MIN + 1, INT64_MAX, "bitrate");
    a.rate = parse_number(step.args[3], INT64_MIN + 1, INT64_MAX, "events per second");
    return a;
}

struct OverrideArgs { int channel; ChannelAttribute attribute; int32_t value; std::string text; };
OverrideArgs parse_override(const Step& step)
{
    need(step, 3, 3);
    OverrideArgs a;
    a.channel = static_cast<int>(parse_number(step.args[0], 0, 15, "channel"));
    EnumValue attr = parse_enum(kAttributes, step.args[1], "channel attribute");
    a.attribute = static_cast<ChannelAttribute>(attr.value);
    if (step.args[2] == "auto") { a.value = engine::ChannelOverrideState::kAutoValue; a.text = "Auto"; }
    else {
        int64_t v = parse_number(step.args[2], engine::ChannelOverrideState::minimum(a.attribute),
            engine::ChannelOverrideState::maximum(a.attribute), "override value");
        a.value = static_cast<int32_t>(v);
        a.text = std::to_string(v);
    }
    return a;
}

struct ChannelArgs { int channel; bool enabled; };
ChannelArgs parse_channel(const Step& step)
{
    need(step, 2, 2);
    ChannelArgs a;
    a.channel = static_cast<int>(parse_number(step.args[0], 0, 15, "channel"));
    if (step.args[1] == "on") a.enabled = true;
    else if (step.args[1] == "off") a.enabled = false;
    else throw std::runtime_error("channel takes on|off");
    return a;
}

// Setup directives run as engine calls on a fresh engine before the first command.
std::function<void(PlaybackEngine&)> bind_setup(const Step& step)
{
    try {
        if (step.verb == "set") {
            need(step, 2, 2);
            const Property& p = find_property(step.args[0]);
            std::string unused;
            int64_t value = parse_value(p, step.args[1], unused);
            return [&p, value](PlaybackEngine& e) { p.set(e, value); };
        }
        if (step.verb == "service") {
            ServiceArgs a = parse_service(step);
            return [a](PlaybackEngine& e) { e.set_service_configuration(a.mode, a.processing, a.bitrate, a.rate); };
        }
        if (step.verb == "override") {
            OverrideArgs a = parse_override(step);
            return [a](PlaybackEngine& e) { e.set_channel_override(a.channel, a.attribute, a.value); };
        }
        if (step.verb == "channel") {
            ChannelArgs a = parse_channel(step);
            return [a](PlaybackEngine& e) { e.set_channel_enabled(a.channel, a.enabled); };
        }
        throw std::runtime_error("unknown setup directive '" + step.verb + "'");
    }
    catch (const std::runtime_error& ex) {
        throw std::runtime_error("line " + std::to_string(step.line) + ": " + ex.what());
    }
}

Command bind_command(const Step& step, int64_t duration)
{
    Command c;
    c.offset = step.at.evaluate(duration);
    const std::vector<std::string>& a = step.args;
    try {
        if (step.verb == "start") {
            need(step, 2, 3);
            EnumValue mode = parse_enum(kProcessingModes, a[0], "processing mode");
            int64_t position = ScenarioTime::parse(a[1]).evaluate(duration);
            bool paused = false;
            if (a.size() == 3) {
                if (a[2] != "paused") throw std::runtime_error("start's third argument must be 'paused'");
                paused = true;
            }
            c.name = std::string("Start(") + mode.name + ",pos=" + std::to_string(position) + (paused ? ",paused" : "") + ")";
            ProcessingMode pm = static_cast<ProcessingMode>(mode.value);
            c.apply = [pm, position, paused](Ctx& x) { return x.engine->start(x.song, x.output, pm, position, paused); };
        }
        else if (step.verb == "stop") {
            need(step, 0, 0);
            c.name = "Stop";
            c.apply = [](Ctx& x) { return x.engine->stop(); };
        }
        else if (step.verb == "pause") {
            need(step, 0, 0);
            c.name = "Pause";
            c.apply = [](Ctx& x) { return x.engine->pause(); };
        }
        else if (step.verb == "resume") {
            need(step, 0, 0);
            c.name = "Resume";
            c.apply = [](Ctx& x) { x.engine->resume(); return engine::EngineError::none(); };
        }
        else if (step.verb == "seek") {
            need(step, 1, 1);
            int64_t target = ScenarioTime::parse(a[0]).evaluate(duration);
            c.name = "Seek(" + std::to_string(target) + ")";
            c.apply = [target](Ctx& x) { return x.engine->seek(target); };
        }
        else if (step.verb == "snapshot") {
            need(step, 0, 0);
            c.name = "Snapshot";
            c.apply = [](Ctx&) { return engine::EngineError::none(); };
        }
        else if (step.verb == "set") {
            need(step, 2, 2);
            const Property& p = find_property(a[0]);
            std::string text;
            int64_t value = parse_value(p, a[1], text);
            c.name = "Set(" + p.member + "=" + text + ")";
            c.apply = [&p, value](Ctx& x) { p.set(*x.engine, value); return engine::EngineError::none(); };
        }
        else if (step.verb == "service") {
            ServiceArgs s = parse_service(step);
            const char* modeName = kServiceModes[static_cast<int>(s.mode)].name;
            c.name = std::string("SetService(") + modeName + "," + std::to_string(s.processing) + "us)";
            c.apply = [s](Ctx& x) { x.engine->set_service_configuration(s.mode, s.processing, s.bitrate, s.rate); return engine::EngineError::none(); };
        }
        else if (step.verb == "restart") {
            if (a.empty()) throw std::runtime_error("'restart' takes a processing mode");
            EnumValue mode = parse_enum(kProcessingModes, a[0], "processing mode");
            std::vector<const Property*> props;
            std::vector<int64_t> values;
            std::string name = std::string("Restart(") + mode.name;
            for (size_t i = 1; i < a.size(); ++i) {
                size_t eq = a[i].find('=');
                if (eq == std::string::npos || eq == 0) throw std::runtime_error("restart settings are key=value");
                const Property& p = find_property(a[i].substr(0, eq));
                std::string text;
                values.push_back(parse_value(p, a[i].substr(eq + 1), text));
                props.push_back(&p);
                name += "," + p.member + "=" + text;
            }
            name += ")";
            c.name = name;
            ProcessingMode pm = static_cast<ProcessingMode>(mode.value);
            c.apply = [pm, props, values](Ctx& x) {
                // MainForm.RestartForProcessingModeChange: only while Playing or Paused.
                PlaybackState previous = x.engine->state();
                bool active = previous == PlaybackState::Playing || previous == PlaybackState::Paused;
                int64_t position = 0;
                if (active) {
                    position = x.engine->snapshot().intended_timeline_microseconds;
                    engine::EngineError e = x.engine->stop();
                    if (!e.ok()) return e;
                }
                for (size_t i = 0; i < props.size(); ++i) props[i]->set(*x.engine, values[i]);
                if (active) return x.engine->start(x.song, x.output, pm, position, previous == PlaybackState::Paused);
                return engine::EngineError::none();
            };
        }
        else if (step.verb == "override") {
            OverrideArgs o = parse_override(step);
            c.name = "Override(ch=" + std::to_string(o.channel) + "," + kAttributes[static_cast<int>(o.attribute)].name + "=" + o.text + ")";
            c.apply = [o](Ctx& x) { return x.engine->set_channel_override(o.channel, o.attribute, o.value); };
        }
        else if (step.verb == "channel") {
            ChannelArgs ch = parse_channel(step);
            c.name = "Channel(ch=" + std::to_string(ch.channel) + ",enabled=" + (ch.enabled ? "1" : "0") + ")";
            c.apply = [ch](Ctx& x) { return x.engine->set_channel_enabled(ch.channel, ch.enabled); };
        }
        else throw std::runtime_error("unknown command '" + step.verb + "'");
    }
    catch (const std::runtime_error& ex) {
        throw std::runtime_error("line " + std::to_string(step.line) + ": " + ex.what());
    }
    return c;
}

// ---------------------------------------------------------------- log, clock, output

class Recorder {
public:
    explicit Recorder(const win32::QpcClock& clock) : clock_(clock), base_(clock.now_ticks()) { text_.reserve(1 << 20); }
    void rebase() { base_ = clock_.now_ticks(); }
    int64_t now() const
    {
        int64_t ticks = clock_.now_ticks() - base_;
        int64_t f = clock_.frequency();
        return (ticks / f) * 1000000LL + ((ticks % f) * 1000000LL) / f;
    }
    void line(const std::string& text) { std::lock_guard<std::mutex> lock(m_); text_ += text; text_ += '\n'; }
    void line(int64_t t, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(m_);
        text_ += std::to_string(t);
        text_ += ' ';
        text_ += text;
        text_ += '\n';
    }
    std::string text() { std::lock_guard<std::mutex> lock(m_); return text_; }

private:
    const win32::QpcClock& clock_;
    int64_t base_;
    std::mutex m_;
    std::string text_;
};

std::string hex_bytes(const uint8_t* data, size_t length)
{
    static const char digits[] = "0123456789ABCDEF";
    if (length == 0) return "-";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) { out.push_back(digits[data[i] >> 4]); out.push_back(digits[data[i] & 15]); }
    return out;
}

// What the engine's send observer saw for the send that is about to reach the
// output on this thread (the output gets only packed bytes).
struct PendingSend {
    bool valid = false;
    EventKind kind = EventKind::NoteOff;
    int32_t src = 0;
    int32_t length = 0;
};
thread_local PendingSend t_pending;

class RecordingNoDeviceOutput : public platform::MidiOutput {
public:
    explicit RecordingNoDeviceOutput(Recorder& recorder) : recorder_(recorder) {}
    const char* backend_name() const override { return "RealtimeRecording"; }
    std::vector<platform::OutputDeviceInfo> enumerate() const override { return std::vector<platform::OutputDeviceInfo>(); }
    platform::OutputStatus open(uint32_t) override { return platform::OutputStatus::success(); }
    platform::OutputStatus close() override { return platform::OutputStatus::success(); }
    bool is_open() const override { return true; }
    platform::OutputStatus send_short(uint32_t packed) override
    {
        int64_t t = recorder_.now();
        PendingSend p = t_pending;
        t_pending.valid = false;
        uint8_t bytes[3] = { static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8), static_cast<uint8_t>(packed >> 16) };
        size_t length = p.valid ? static_cast<size_t>(std::min<int32_t>(p.length, 3)) : 3;
        record(t, p, bytes, length);
        return platform::OutputStatus::success();
    }
    platform::OutputStatus send_long(const output::SystemExclusiveEvent& fragment) override
    {
        int64_t t = recorder_.now();
        PendingSend p = t_pending;
        t_pending.valid = false;
        record(t, p, fragment.data, fragment.length);
        return platform::OutputStatus::success();
    }
    platform::OutputStatus reset() override { recorder_.line(recorder_.now(), "RESET"); return platform::OutputStatus::success(); }
    platform::OutputStatus panic() override { recorder_.line(recorder_.now(), "PANIC"); return platform::OutputStatus::success(); }
    bool sysex_supported() const override { return true; }
    std::string sysex_disabled_reason() const override { return std::string(); }
    void set_source_file(const std::string&) override {}
    int64_t sends() const { return sends_.load(); }

private:
    void record(int64_t t, const PendingSend& p, const uint8_t* data, size_t length)
    {
        std::string line = "SEND ";
        line += p.valid ? event_kind_name(p.kind) : "Unknown";
        line += " src=" + std::to_string(p.valid ? p.src : -999) + " ";
        line += hex_bytes(data, length);
        recorder_.line(t, line);
        ++sends_;
    }
    Recorder& recorder_;
    std::atomic<int64_t> sends_{ 0 };
};

// ---------------------------------------------------------------- song loading

struct LoadedSong {
    std::shared_ptr<Song> song;
    std::shared_ptr<engine::PlaybackSong> playback;
};

LoadedSong load_fixture(const std::string& path)
{
    win32::Win32FileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    if (!source) throw std::runtime_error("cannot open " + path + ": " + error);
    LoadedSong loaded;
    loaded.song = std::make_shared<Song>();
    LoadError loadError;
    if (!load_song(*source, LoadOptions(), *loaded.song, loadError))
        throw std::runtime_error("cannot load " + path + ": " + loadError.type + ": " + loadError.message);
    loaded.playback = engine::make_playback_song(*loaded.song, path, loaded.song);
    return loaded;
}

std::string event_hex(const engine::PlaybackSong& song, int32_t index)
{
    models::EventView e = song.events.at(index);
    if (e.kind == EventKind::SystemExclusive && e.data_length > 3 && song.payload_store != nullptr) {
        std::vector<uint8_t> bytes(static_cast<size_t>(e.data_length));
        const EventRecord& record = song.payload_store->records[e.source_index];
        if (record.payload_offset >= 0)
            song.payload_store->payloads.copy_to(record.payload_offset, bytes.data(), bytes.size());
        return hex_bytes(bytes.data(), bytes.size());
    }
    return hex_bytes(e.bytes, static_cast<size_t>(std::min<int32_t>(e.data_length, 3)));
}

std::string stem_of(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// ---------------------------------------------------------------- commands: list, events, run

std::string flat(const std::string& text)
{
    std::string out = text;
    for (char& c : out) if (c == '\r' || c == '\n') c = ' ';
    return out;
}

std::string snapshot_text(const engine::PlaybackSnapshot& s)
{
    std::ostringstream o;
    o << "state=" << engine::playback_state_name(s.state) << " processed=" << s.processed_events
      << " dropped=" << s.dropped_events << " queue=" << s.queue_length << " outstanding=" << s.outstanding_events
      << " maxQueue=" << s.maximum_queue_length << " gateFiltered=" << s.gate_filtered_events
      << " musicalRejectedAttacks=" << s.musical_limit_rejected_attacks
      << " musicalSuppressedReleases=" << s.musical_limit_suppressed_releases
      << " rateRejectedAttacks=" << s.output_rate_rejected_attacks
      << " rateSuppressedReleases=" << s.output_rate_suppressed_releases
      << " rateRejectedOther=" << s.output_rate_rejected_other_messages
      << " rateSafetySoftExcess=" << s.output_rate_safety_soft_excess
      << " rateSafetyBypass=" << s.output_rate_safety_bypass_messages
      << " virtualQueue=" << s.virtual_queue_length << " virtualMaxQueue=" << s.virtual_maximum_queue_length
      << " position=" << s.playback_microseconds << " intended=" << s.intended_timeline_microseconds
      << " lastDispatched=" << s.last_dispatched_timeline_microseconds
      << " frontier=" << s.effective_speed_frontier_microseconds
      << " lag=" << s.current_lag_microseconds << " maxLag=" << s.maximum_lag_microseconds;
    return o.str();
}

void sleep_ms(unsigned ms) { Sleep(ms); }

int cmd_list(const std::string& file)
{
    std::vector<Definition> definitions = load_scenarios(file);
    for (const Definition& d : definitions) {
        if (d.has_route) throw std::runtime_error(file + " line " + std::to_string(d.route_line) + ": 'route' is not supported by the real-time programs");
        for (const Step& s : d.setup) bind_setup(s);
        for (const Step& s : d.commands) bind_command(s, 1000);
        tools::write_stdout(d.name + "\n");
    }
    return 0;
}

int cmd_events(const std::string& fixture, const std::string& outFile)
{
    LoadedSong loaded = load_fixture(fixture);
    const engine::PlaybackSong& song = *loaded.playback;
    int32_t count = song.events.count();
    std::string text = "SONG count=" + std::to_string(count) + " duration=" + std::to_string(song.duration_us) + "\n";
    for (int32_t i = 0; i < count; ++i) {
        models::EventView e = song.events.at(i);
        text += std::to_string(i) + " " + std::to_string(e.micros) + " " + event_kind_name(e.kind) + " " + event_hex(song, i) + "\n";
    }
    if (!tools::write_file(outFile, text)) throw std::runtime_error("cannot write " + outFile);
    tools::write_stdout("events " + std::to_string(count) + " duration_us " + std::to_string(song.duration_us) + "\n");
    return 0;
}

struct Watchdog {
    std::atomic<bool> done{ false };
};

// Optional JIT-equivalent warm-up (argument "warm"): plays the 69 ms f0-all-message-types fixture once in the
// scenario's first processing mode, with its settings, on a throwaway engine and the None output, so that the
// run under test does not pay first-use costs (page faults, thread start-up). Mirrors v1-realtime.exe.
void warm_up(const std::string& fixture, const std::vector<std::function<void(PlaybackEngine&)>>& setup,
    const std::vector<Command>& commands, const std::shared_ptr<win32::Win32EngineServices>& services)
{
    try {
        size_t slash = fixture.find_last_of("\\/");
        std::string dir = slash == std::string::npos ? std::string(".") : fixture.substr(0, slash);
        std::string warmFile = dir + "\\f0-all-message-types.mid";
        ProcessingMode mode = ProcessingMode::Queue;
        for (const Command& c : commands) {
            if (c.name.compare(0, 6, "Start(") != 0) continue;
            std::string text = c.name.substr(6, c.name.find(',') - 6);
            for (const EnumValue& e : kProcessingModes)
                if (text == e.name) mode = static_cast<ProcessingMode>(e.value);
            break;
        }
        LoadedSong warm = load_fixture(warmFile);
        std::shared_ptr<platform::NullMidiOutput> none = std::make_shared<platform::NullMidiOutput>();
        PlaybackEngine engine(services);
        for (const auto& s : setup) s(engine);
        engine.start(warm.playback, none, mode, 0, false);
        engine.pause();
        engine.resume();
        engine.seek(10000);
        win32::QpcClock clock;
        int64_t begin = clock.now_ticks();
        while (engine.state() == PlaybackState::Playing && (clock.now_ticks() - begin) * 1000 / clock.frequency() < 800) sleep_ms(2);
        engine.stop();
    }
    catch (const std::exception&) {
        // Best effort.
    }
}

int cmd_run(const std::string& scenarioFile, const std::string& scenarioName, const std::string& fixture,
    const std::string& outLog, bool warm)
{
    std::vector<Definition> definitions = load_scenarios(scenarioFile);
    const Definition* definition = nullptr;
    for (const Definition& d : definitions) {
        if (d.has_route) throw std::runtime_error(scenarioFile + " line " + std::to_string(d.route_line) + ": 'route' is not supported by the real-time programs");
        if (d.name == scenarioName) definition = &d;
    }
    if (!definition) throw std::runtime_error("no scenario '" + scenarioName + "' in " + scenarioFile);

    LoadedSong loaded = load_fixture(fixture);
    int64_t duration = std::max<int64_t>(1000, loaded.playback->duration_us);
    std::vector<std::function<void(PlaybackEngine&)>> setup;
    for (const Step& s : definition->setup) setup.push_back(bind_setup(s));
    std::vector<Command> commands;
    for (const Step& s : definition->commands) commands.push_back(bind_command(s, duration));

    std::shared_ptr<win32::Win32EngineServices> services = std::make_shared<win32::Win32EngineServices>();
    if (warm) warm_up(fixture, setup, commands, services);
    win32::QpcClock clock;
    Recorder recorder(clock);
    std::shared_ptr<RecordingNoDeviceOutput> output = std::make_shared<RecordingNoDeviceOutput>(recorder);
    const char* arch = sizeof(void*) == 8 ? "x64" : "x86";
    recorder.line("RUN program=v2 arch=" + std::string(arch) + " scenario=" + scenarioName + " fixture=" + stem_of(fixture) +
        " duration=" + std::to_string(loaded.playback->duration_us) + " events=" + std::to_string(loaded.playback->events.count()) +
        " timerFrequency=" + std::to_string(clock.frequency()));
    recorder.line("DESCRIBE " + definition->description);
    for (const Step& s : definition->setup) {
        std::string t = "PLAN-SETUP " + s.verb;
        for (const std::string& a : s.args) t += " " + a;
        recorder.line(t);
    }
    for (const Command& c : commands) recorder.line("PLAN-AT " + std::to_string(c.offset) + " " + c.name);

    // Watchdog: a run that outlives its cap by a wide margin writes what it has and exits (never a pass).
    int64_t capUs = 40 * duration + 30000000LL;
    std::shared_ptr<Watchdog> watchdog = std::make_shared<Watchdog>();
    std::thread([&recorder, watchdog, capUs, outLog]() {
        win32::QpcClock c;
        int64_t start = c.now_ticks();
        while (!watchdog->done.load()) {
            sleep_ms(100);
            if ((c.now_ticks() - start) * 1000000LL / c.frequency() > capUs + 30000000LL) {
                recorder.line(recorder.now(), "WATCHDOG run exceeded its cap by 30 s");
                tools::write_file(outLog, recorder.text());
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    }).detach();

    int exit_code = 0;
    int64_t sendsAtEnd = 0;
    {
        PlaybackEngine engine(services);
        engine.set_send_observer([](const models::EventView& e) {
            t_pending.valid = true;
            t_pending.kind = e.kind;
            t_pending.src = e.event_index();
            t_pending.length = e.data_length;
        });
        engine.set_playback_ended_handler([&recorder] { recorder.line(recorder.now(), "EVENT PlaybackEnded"); });
        engine.set_playback_failed_handler([&recorder](const engine::EngineError& e) {
            recorder.line(recorder.now(), "EVENT PlaybackFailed " + e.type + ": " + flat(e.message));
        });
        engine.set_channel_control_failed_handler([&recorder](int channel, ChannelAttribute attribute, const engine::EngineError& e) {
            recorder.line(recorder.now(), "EVENT ChannelControlFailed ch=" + std::to_string(channel) + " " +
                kAttributes[static_cast<int>(attribute)].name + " " + e.type + ": " + flat(e.message));
        });

        Ctx ctx;
        ctx.engine = &engine;
        ctx.song = loaded.playback;
        ctx.output = output;
        for (auto& s : setup) s(engine);
        recorder.rebase();
        recorder.line(recorder.now(), "BEGIN");

        int64_t lastCommandAt = recorder.now();
        auto apply = [&](const std::string& name, const std::function<engine::EngineError(Ctx&)>& fn) {
            recorder.line(recorder.now(), "SNAP " + snapshot_text(engine.snapshot()));
            recorder.line(recorder.now(), "CMD " + name);
            engine::EngineError e = fn(ctx);
            if (!e.ok()) recorder.line(recorder.now(), "CMD-EXCEPTION " + name + " " + e.type + ": " + flat(e.message));
            lastCommandAt = recorder.now();
            recorder.line(lastCommandAt, "CMD-DONE");
        };

        int64_t lastSample = 0;
        size_t next = 0;
        bool endedInTime = true;
        while (true) {
            int64_t now = recorder.now();
            if (now - lastSample >= 20000) {
                lastSample = now;
                recorder.line(now, "SNAP " + snapshot_text(engine.snapshot()));
            }
            if (next < commands.size()) {
                const Command& command = commands[next];
                if (now >= command.offset) {
                    ++next;
                    apply(command.name, command.apply);
                    recorder.line(recorder.now(), "SNAP " + snapshot_text(engine.snapshot()));
                    lastSample = recorder.now();
                    continue;
                }
                int64_t remaining = command.offset - now;
                if (remaining > 20000) sleep_ms(1); else std::this_thread::yield();
                continue;
            }
            PlaybackState state = engine.state();
            bool alive = engine.has_live_worker();
            if (!alive && state != PlaybackState::Playing) break;
            if (state == PlaybackState::Paused && now - lastCommandAt > 200000) {
                apply("Stop(paused at end)", [](Ctx& x) { return x.engine->stop(); });
                break;
            }
            if (now > capUs) {
                recorder.line(now, "TIMEOUT run exceeded the cap");
                endedInTime = false;
                break;
            }
            sleep_ms(1);
        }
        if (definition->stop_after_end || !endedInTime)
            apply("Stop(after end)", [](Ctx& x) { return x.engine->stop(); });
        recorder.line(recorder.now(), "FINAL " + snapshot_text(engine.snapshot()) + " sends=" + std::to_string(output->sends()));
        sendsAtEnd = output->sends();
        engine::EngineError disposeError = engine.dispose();
        if (!disposeError.ok())
            recorder.line(recorder.now(), "DISPOSE-ERROR " + disposeError.type + ": " + flat(disposeError.message));
        if (!endedInTime) exit_code = 1;
    }
    recorder.line(recorder.now(), "END");
    watchdog->done.store(true);
    if (!tools::write_file(outLog, recorder.text())) throw std::runtime_error("cannot write " + outLog);
    tools::write_stdout(std::string("v2 ") + arch + " " + scenarioName + " " + stem_of(fixture) + " sends=" + std::to_string(sendsAtEnd) + "\n");
    return exit_code;
}

void usage()
{
    tools::write_stdout(
        "usage: midibn-engine-realtime list   <scenarioFile>\n"
        "       midibn-engine-realtime events <fixture.mid> <outFile>\n"
        "       midibn-engine-realtime run    <scenarioFile> <scenario> <fixture.mid> <outLog>\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args = tools::utf8_arguments(argc, argv);
    try {
        if (args.size() >= 3 && args[1] == "list") return cmd_list(args[2]);
        if (args.size() >= 4 && args[1] == "events") return cmd_events(args[2], args[3]);
        if (args.size() >= 6 && args[1] == "run") return cmd_run(args[2], args[3], args[4], args[5], args.size() >= 7 && args[6] == "warm");
    }
    catch (const std::exception& ex) {
        tools::write_stdout(std::string("midibn-engine-realtime ERROR: ") + ex.what() + "\n");
        return 2;
    }
    usage();
    return 2;
}
