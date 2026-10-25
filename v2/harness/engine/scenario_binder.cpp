#include "scenario_binder.h"

#include "midibn/engine/channel_state.h"

#include <cstring>
#include <map>

namespace midibn {
namespace harness {

using midibn::ChannelAttribute;
using engine::ChannelOverrideState;
using engine::EngineError;
using engine::OverflowPolicy;
using engine::PlaybackEngine;
using engine::PlaybackState;
using engine::ProcessingMode;
using engine::ServiceDurationMode;

namespace {

struct BindError {
    std::string message;
};

const char* const kOverflowNames[] = { "DropNewest", "DropOldest", "ClearBufferAndCatchUp", "DropIncomingCompleteNotes",
    "DropOldestCompleteNote", "DropBackgroundNotes" };
const char* const kServiceModeNames[] = { "ProcessingTime", "MidiBitrate", "EventsPerSecond" };
const char* const kProcessingModeNames[] = { "Queue", "Drop", "PerNoteIntervalGate", "DelayedNoteOff", "NoteAttackLimit",
    "OutputRateLimit" };
const char* const kAttributeNames[] = { "BankMsb", "BankLsb", "Program", "Volume", "Expression", "Pan", "Sustain",
    "PitchBend", "Aftertouch" };
const char* const kTierNames[] = { "ExactSafe", "GmFamily", "Aggressive" };
const char* const kPercussionNames[] = { "ReserveChannel10", "AllowAnyDestination" };

std::string kebab(const std::string& pascal)
{
    std::string text;
    for (size_t i = 0; i < pascal.size(); ++i) {
        char c = pascal[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0) text.push_back('-');
            text.push_back(static_cast<char>(c - 'A' + 'a'));
        }
        else text.push_back(c);
    }
    return text;
}

// Index of the member whose kebab-case name is `token`, or -1.
int find_enum(const char* const* names, int count, const std::string& token)
{
    for (int i = 0; i < count; ++i)
        if (kebab(names[i]) == token) return i;
    return -1;
}

int parse_enum(const char* const* names, int count, const std::string& token, const char* what)
{
    int index = find_enum(names, count, token);
    if (index < 0) throw BindError{ std::string("unknown ") + what + " '" + token + "'" };
    return index;
}

// Int64.TryParse(NumberStyles.AllowLeadingSign) within [min, max].
int64_t parse_number(const std::string& token, int64_t min, int64_t max, const std::string& what)
{
    size_t i = 0;
    bool negative = false;
    if (i < token.size() && (token[i] == '-' || token[i] == '+')) { negative = token[i] == '-'; ++i; }
    bool ok = i < token.size();
    // Accumulate as a negative number so that Int64.MinValue parses.
    int64_t acc = 0;
    for (; ok && i < token.size(); ++i) {
        char c = token[i];
        if (c < '0' || c > '9') { ok = false; break; }
        int digit = c - '0';
        if (acc < (INT64_MIN + digit) / 10) { ok = false; break; }
        acc = acc * 10 - digit;
    }
    if (ok && !negative) {
        if (acc == INT64_MIN) ok = false;
        else acc = -acc;
    }
    if (!ok || acc < min || acc > max) throw BindError{ "bad " + what + " '" + token + "'" };
    return acc;
}

typedef std::function<void(PlaybackEngine&, int64_t)> PropertySetter;

struct Property {
    std::string member;
    std::string key;
    char kind = 'n';                    // 'b' on|off, 'n' integer, 'e' enum token
    const char* const* enum_names = nullptr;
    int enum_count = 0;
    int64_t minimum = 0;
    int64_t maximum = 0;
    PropertySetter set;
};

std::map<std::string, Property> build_properties()
{
    std::map<std::string, Property> map;
    auto add_bool = [&](const char* member, PropertySetter set) {
        Property p;
        p.member = member; p.key = kebab(member); p.kind = 'b'; p.maximum = 1; p.set = set;
        map[p.key] = p;
    };
    auto add_number = [&](const char* member, int64_t min, int64_t max, PropertySetter set) {
        Property p;
        p.member = member; p.key = kebab(member); p.kind = 'n'; p.minimum = min; p.maximum = max; p.set = set;
        map[p.key] = p;
    };
    auto add_enum = [&](const char* member, const char* const* names, int count, PropertySetter set) {
        Property p;
        p.member = member; p.key = kebab(member); p.kind = 'e'; p.enum_names = names; p.enum_count = count; p.set = set;
        map[p.key] = p;
    };
    add_bool("SimulateSlowdown", [](PlaybackEngine& e, int64_t v) { e.set_simulate_slowdown(v != 0); });
    add_bool("ApplyQueueLimitWithoutSlowdown", [](PlaybackEngine& e, int64_t v) { e.set_apply_queue_limit_without_slowdown(v != 0); });
    add_bool("ChaseMidiStateOnPlaySeek", [](PlaybackEngine& e, int64_t v) { e.set_chase_midi_state_on_play_seek(v != 0); });
    add_bool("PreservePlaybackQueueOnPauseSeek", [](PlaybackEngine& e, int64_t v) { e.set_preserve_playback_queue_on_pause_seek(v != 0); });
    add_bool("OutputPacedTimeline", [](PlaybackEngine& e, int64_t v) { e.set_output_paced_timeline(v != 0); });
    add_bool("ScaleNoteIntervalByTraffic", [](PlaybackEngine& e, int64_t v) { e.set_scale_note_interval_by_traffic(v != 0); });
    add_bool("IntervalPolyphonyLimitEnabled", [](PlaybackEngine& e, int64_t v) { e.set_interval_polyphony_limit_enabled(v != 0); });
    add_number("IntervalPolyphonyLimit", INT32_MIN, INT32_MAX, [](PlaybackEngine& e, int64_t v) { e.set_interval_polyphony_limit(static_cast<int32_t>(v)); });
    add_number("QueueLengthLimit", INT32_MIN, INT32_MAX, [](PlaybackEngine& e, int64_t v) { e.set_queue_length_limit(static_cast<int32_t>(v)); });
    add_bool("QueueAgeLimitEnabled", [](PlaybackEngine& e, int64_t v) { e.set_queue_age_limit_enabled(v != 0); });
    add_number("QueueAgeLimitMicroseconds", INT64_MIN, INT64_MAX, [](PlaybackEngine& e, int64_t v) { e.set_queue_age_limit_microseconds(v); });
    add_enum("OverflowPolicy", kOverflowNames, 6, [](PlaybackEngine& e, int64_t v) { e.set_overflow_policy(static_cast<OverflowPolicy>(static_cast<int>(v))); });
    add_enum("ServiceDurationMode", kServiceModeNames, 3, [](PlaybackEngine& e, int64_t v) { e.set_service_duration_mode(static_cast<ServiceDurationMode>(static_cast<int>(v))); });
    add_number("ProcessingMicroseconds", INT64_MIN, INT64_MAX, [](PlaybackEngine& e, int64_t v) { e.set_processing_microseconds(v); });
    add_number("MidiBitrate", INT64_MIN, INT64_MAX, [](PlaybackEngine& e, int64_t v) { e.set_midi_bitrate(v); });
    add_number("EventsPerSecond", INT64_MIN, INT64_MAX, [](PlaybackEngine& e, int64_t v) { e.set_events_per_second(v); });
    return map;
}

const std::map<std::string, Property>& properties()
{
    static const std::map<std::string, Property> map = build_properties();
    return map;
}

const Property& find_property(const std::string& key)
{
    std::map<std::string, Property>::const_iterator found = properties().find(key);
    if (found == properties().end()) throw BindError{ "unknown setting key '" + key + "'" };
    return found->second;
}

// Parses a property value into the integer the setter takes, and the text
// used in log names.
int64_t parse_value(const Property& property, const std::string& token, std::string& logText)
{
    if (property.kind == 'b') {
        if (token == "on") { logText = "1"; return 1; }
        if (token == "off") { logText = "0"; return 0; }
        throw BindError{ property.key + " takes on|off" };
    }
    if (property.kind == 'e') {
        int index = find_enum(property.enum_names, property.enum_count, token);
        if (index < 0) throw BindError{ "unknown value '" + token + "' for " + property.key };
        logText = property.enum_names[index];
        return index;
    }
    int64_t value = parse_number(token, property.minimum, property.maximum, property.key + " value");
    logText = std::to_string(value);
    return value;
}

void need(const ScenarioStep& step, size_t min, size_t max)
{
    if (step.args.size() < min || step.args.size() > max)
        throw BindError{ "'" + step.verb + "' takes " + (min == max ? std::to_string(min) : std::to_string(min) + " to " + std::to_string(max)) +
            " argument(s)" };
}

struct ServiceArgs {
    ServiceDurationMode mode;
    int64_t processing, bitrate, rate;
};

ServiceArgs parse_service(const ScenarioStep& step)
{
    need(step, 4, 4);
    ServiceArgs s;
    s.mode = static_cast<ServiceDurationMode>(parse_enum(kServiceModeNames, 3, step.args[0], "service mode"));
    s.processing = parse_number(step.args[1], INT64_MIN, INT64_MAX, "processing microseconds");
    s.bitrate = parse_number(step.args[2], INT64_MIN, INT64_MAX, "bitrate");
    s.rate = parse_number(step.args[3], INT64_MIN, INT64_MAX, "events per second");
    return s;
}

struct OverrideArgs {
    int channel;
    ChannelAttribute attribute;
    int32_t value;
    std::string text;
};

OverrideArgs parse_override(const ScenarioStep& step)
{
    need(step, 3, 3);
    OverrideArgs o;
    o.channel = static_cast<int>(parse_number(step.args[0], 0, 15, "channel"));
    o.attribute = static_cast<ChannelAttribute>(parse_enum(kAttributeNames, 9, step.args[1], "channel attribute"));
    if (step.args[2] == "auto") {
        o.value = ChannelOverrideState::kAutoValue;
        o.text = "Auto";
    }
    else {
        o.value = static_cast<int32_t>(parse_number(step.args[2], ChannelOverrideState::minimum(o.attribute),
            ChannelOverrideState::maximum(o.attribute), "override value"));
        o.text = std::to_string(o.value);
    }
    return o;
}

struct ChannelArgs {
    int channel;
    bool enabled;
};

ChannelArgs parse_channel(const ScenarioStep& step)
{
    need(step, 2, 2);
    ChannelArgs c;
    c.channel = static_cast<int>(parse_number(step.args[0], 0, 15, "channel"));
    if (step.args[1] == "on") c.enabled = true;
    else if (step.args[1] == "off") c.enabled = false;
    else throw BindError{ "channel takes on|off" };
    return c;
}

SetupAction bind_setup(const ScenarioStep& step)
{
    if (step.verb == "set") {
        need(step, 2, 2);
        Property property = find_property(step.args[0]);
        std::string unused;
        int64_t value = parse_value(property, step.args[1], unused);
        return [property, value](PlaybackEngine& e) { property.set(e, value); return EngineError(); };
    }
    if (step.verb == "service") {
        ServiceArgs s = parse_service(step);
        return [s](PlaybackEngine& e) { e.set_service_configuration(s.mode, s.processing, s.bitrate, s.rate); return EngineError(); };
    }
    if (step.verb == "override") {
        OverrideArgs o = parse_override(step);
        return [o](PlaybackEngine& e) { return e.set_channel_override(o.channel, o.attribute, o.value); };
    }
    if (step.verb == "channel") {
        ChannelArgs c = parse_channel(step);
        return [c](PlaybackEngine& e) { return e.set_channel_enabled(c.channel, c.enabled); };
    }
    throw BindError{ "unknown setup directive '" + step.verb + "'" };
}

routing::TrackRoutingSettings parse_route(const ScenarioStep& step)
{
    routing::TrackRoutingSettings settings;
    for (const std::string& arg : step.args) {
        size_t eq = arg.find('=');
        if (eq == std::string::npos || eq == 0) throw BindError{ "route takes key=value arguments" };
        std::string key = arg.substr(0, eq), value = arg.substr(eq + 1);
        if (key == "tier") settings.maximum_automatic_tier = static_cast<routing::TrackRoutingTier>(parse_enum(kTierNames, 3, value, "routing tier"));
        else if (key == "cap") settings.target_capacity = static_cast<int32_t>(parse_number(value, 1, 16, "capacity"));
        else if (key == "perc") settings.percussion_policy = static_cast<routing::PercussionRoutingPolicy>(parse_enum(kPercussionNames, 2, value, "percussion policy"));
        else if (key == "usable") {
            // Int32.TryParse(AllowHexSpecifier) on exactly four characters.
            if (value.size() != 4) throw BindError{ "usable takes four hex digits" };
            int mask = 0;
            for (char c : value) {
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else throw BindError{ "usable takes four hex digits" };
                mask = mask * 16 + d;
            }
            for (int channel = 0; channel < 16; ++channel)
                settings.usable_destination_channels[static_cast<size_t>(channel)] = (mask & (1 << channel)) != 0;
        }
        else if (key == "manual") {
            size_t start = 0;
            for (;;) {
                size_t semi = value.find(';', start);
                std::string entry = value.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                // entry.Split('.', '=') must give exactly three fields.
                std::vector<std::string> f;
                size_t s = 0;
                for (;;) {
                    size_t sep = entry.find_first_of(".=", s);
                    f.push_back(entry.substr(s, sep == std::string::npos ? std::string::npos : sep - s));
                    if (sep == std::string::npos) break;
                    s = sep + 1;
                }
                if (f.size() != 3) throw BindError{ "manual takes track.channel=destination;..." };
                routing::TrackLaneKey lane(static_cast<int32_t>(parse_number(f[0], 0, 65535, "track")),
                    static_cast<int32_t>(parse_number(f[1], 0, 15, "source channel")));
                settings.manual_destinations[lane] = static_cast<int32_t>(parse_number(f[2], 0, 15, "destination"));
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }
        else throw BindError{ "unknown route key '" + key + "'" };
    }
    return settings;
}

bool pause_starts_worker(PlaybackEngine& e)
{
    return e.state() == PlaybackState::Playing && !(e.preserve_playback_queue_on_pause_seek() && e.has_live_worker());
}

bool seek_starts_worker(PlaybackEngine& e)
{
    PlaybackState state = e.state();
    return (state == PlaybackState::Playing || state == PlaybackState::Paused) &&
        !(e.preserve_playback_queue_on_pause_seek() && e.has_live_worker());
}

bool restart_starts_worker(PlaybackEngine& e)
{
    PlaybackState state = e.state();
    return state == PlaybackState::Playing || state == PlaybackState::Paused;
}

BoundCommand bind_command(const ScenarioStep& step, int64_t duration)
{
    BoundCommand command;
    command.offset = step.at.evaluate(duration);
    command.starts_worker = [](PlaybackEngine&) { return false; };
    const std::vector<std::string>& a = step.args;
    const std::string& verb = step.verb;
    if (verb == "start") {
        need(step, 2, 3);
        ProcessingMode mode = static_cast<ProcessingMode>(parse_enum(kProcessingModeNames, 6, a[0], "processing mode"));
        ScenarioTime time;
        std::string timeError;
        if (!ScenarioTime::parse(a[1], time, timeError)) throw BindError{ timeError };
        int64_t position = time.evaluate(duration);
        bool paused = false;
        if (a.size() == 3) {
            if (a[2] != "paused") throw BindError{ "start's third argument must be 'paused'" };
            paused = true;
        }
        command.name = std::string("Start(") + engine::processing_mode_name(mode) + ",pos=" + std::to_string(position) +
            (paused ? ",paused" : "") + ")";
        command.starts_worker = [](PlaybackEngine&) { return true; };
        command.apply = [mode, position, paused](PlaybackEngine& e, BindContext& c) {
            return e.start(c.song, c.output, mode, position, paused);
        };
    }
    else if (verb == "stop") {
        need(step, 0, 0);
        command.name = "Stop";
        command.apply = [](PlaybackEngine& e, BindContext&) { return e.stop(); };
    }
    else if (verb == "pause") {
        need(step, 0, 0);
        command.name = "Pause";
        command.starts_worker = pause_starts_worker;
        command.apply = [](PlaybackEngine& e, BindContext&) { return e.pause(); };
    }
    else if (verb == "resume") {
        need(step, 0, 0);
        command.name = "Resume";
        command.apply = [](PlaybackEngine& e, BindContext&) { e.resume(); return EngineError(); };
    }
    else if (verb == "seek") {
        need(step, 1, 1);
        ScenarioTime time;
        std::string timeError;
        if (!ScenarioTime::parse(a[0], time, timeError)) throw BindError{ timeError };
        int64_t target = time.evaluate(duration);
        command.name = "Seek(" + std::to_string(target) + ")";
        command.starts_worker = seek_starts_worker;
        command.apply = [target](PlaybackEngine& e, BindContext&) { return e.seek(target); };
    }
    else if (verb == "snapshot") {
        need(step, 0, 0);
        command.name = "Snapshot";
        command.apply = [](PlaybackEngine&, BindContext&) { return EngineError(); };
    }
    else if (verb == "set") {
        need(step, 2, 2);
        Property property = find_property(a[0]);
        std::string text;
        int64_t value = parse_value(property, a[1], text);
        command.name = "Set(" + property.member + "=" + text + ")";
        command.apply = [property, value](PlaybackEngine& e, BindContext&) { property.set(e, value); return EngineError(); };
    }
    else if (verb == "service") {
        ServiceArgs s = parse_service(step);
        command.name = std::string("SetService(") + kServiceModeNames[static_cast<int>(s.mode)] + "," + std::to_string(s.processing) + "us)";
        command.apply = [s](PlaybackEngine& e, BindContext&) {
            e.set_service_configuration(s.mode, s.processing, s.bitrate, s.rate);
            return EngineError();
        };
    }
    else if (verb == "restart") {
        if (a.empty()) throw BindError{ "'restart' takes a processing mode" };
        ProcessingMode mode = static_cast<ProcessingMode>(parse_enum(kProcessingModeNames, 6, a[0], "processing mode"));
        std::vector<Property> props;
        std::vector<int64_t> values;
        std::string name = std::string("Restart(") + engine::processing_mode_name(mode);
        for (size_t i = 1; i < a.size(); ++i) {
            size_t eq = a[i].find('=');
            if (eq == std::string::npos || eq == 0) throw BindError{ "restart settings are key=value" };
            Property property = find_property(a[i].substr(0, eq));
            std::string text;
            values.push_back(parse_value(property, a[i].substr(eq + 1), text));
            props.push_back(property);
            name += "," + property.member + "=" + text;
        }
        name += ")";
        command.name = name;
        command.starts_worker = restart_starts_worker;
        command.apply = [mode, props, values](PlaybackEngine& e, BindContext& c) {
            // MainForm.RestartForProcessingModeChange: remember the state and
            // the intended timeline position, Stop, apply the new settings,
            // Start again (paused when it was paused). Only while Playing or Paused.
            PlaybackState previous = e.state();
            bool active = previous == PlaybackState::Playing || previous == PlaybackState::Paused;
            int64_t position = 0;
            if (active) {
                position = e.snapshot().intended_timeline_microseconds;
                EngineError error = e.stop();
                if (!error.ok()) return error;
            }
            for (size_t i = 0; i < props.size(); ++i) props[i].set(e, values[i]);
            // Continuation: the events at the restart position were already sent (DD-007).
            if (active) return e.start(c.song, c.output, mode, position, previous == PlaybackState::Paused, true);
            return EngineError();
        };
    }
    else if (verb == "override") {
        OverrideArgs o = parse_override(step);
        command.name = "Override(ch=" + std::to_string(o.channel) + "," + kAttributeNames[static_cast<int>(o.attribute)] + "=" + o.text + ")";
        command.apply = [o](PlaybackEngine& e, BindContext&) { return e.set_channel_override(o.channel, o.attribute, o.value); };
    }
    else if (verb == "channel") {
        ChannelArgs c = parse_channel(step);
        command.name = "Channel(ch=" + std::to_string(c.channel) + ",enabled=" + (c.enabled ? "1" : "0") + ")";
        command.apply = [c](PlaybackEngine& e, BindContext&) { return e.set_channel_enabled(c.channel, c.enabled); };
    }
    else throw BindError{ "unknown command '" + verb + "'" };
    return command;
}

} // namespace

bool bind_scenario(const ScenarioDefinition& definition, int64_t duration, BoundScenario& scenario, std::string& error)
{
    scenario = BoundScenario();
    scenario.name = definition.name;
    scenario.snapshot_mode = definition.snapshots;
    scenario.stop_after_end = definition.stop_after_end;
    int line = 0;
    try {
        if (definition.has_route) {
            line = definition.route.line;
            scenario.has_route = true;
            scenario.route = parse_route(definition.route);
        }
        for (const ScenarioStep& step : definition.setup) {
            line = step.line;
            scenario.setup.push_back(bind_setup(step));
        }
        for (const ScenarioStep& step : definition.commands) {
            line = step.line;
            scenario.commands.push_back(bind_command(step, duration));
        }
    }
    catch (const BindError& ex) {
        error = "line " + std::to_string(line) + ": " + ex.message;
        return false;
    }
    return true;
}

bool validate_scenarios(const std::vector<ScenarioDefinition>& definitions, std::string& error)
{
    for (const ScenarioDefinition& definition : definitions) {
        BoundScenario scenario;
        if (!bind_scenario(definition, 1000, scenario, error)) {
            error = definition.name + ": " + error;
            return false;
        }
    }
    return true;
}

} // namespace harness
} // namespace midibn
