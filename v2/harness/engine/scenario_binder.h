// Binds parsed scenario steps (scenario_file.h) to the v2 engine's public API:
// a port of the v1 reference harness's ScenarioBinder.cs. Every property key,
// enum token and command name is specified in v2/harness/ENGINE_SCENARIOS.md.
// Tokens that name a v1 enum member are the member name in lower-case
// kebab-case ("DropOldestCompleteNote" -> "drop-oldest-complete-note"); keys
// that name a PlaybackEngine property are the property name in the same form.
// Log names (the text after "H CMD ") use the v1 member names.
#pragma once

#include "midibn/engine/playback_engine.h"
#include "midibn/routing/track_routing.h"
#include "scenario_file.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace harness {

// What a command needs besides the engine: the song and output to play
// (v1 RendezvousSpike.CurrentSong / CurrentOutput).
struct BindContext {
    std::shared_ptr<const engine::PlaybackSong> song;
    std::shared_ptr<platform::MidiOutput> output;
};

typedef std::function<engine::EngineError(engine::PlaybackEngine&)> SetupAction;

// One scripted command (SpikeCommand).
struct BoundCommand {
    int64_t offset = 0;                 // virtual microseconds after the scenario base
    std::string name;                   // the text after "H CMD "
    // Evaluated just before apply: does this command create a new worker
    // thread? (Start always does; a non-preserving Pause or Seek does when the
    // engine is active; a preserving one, Resume and Stop never do.)
    std::function<bool(engine::PlaybackEngine&)> starts_worker;
    // Returns the exception v1 would have thrown (ok() when none).
    std::function<engine::EngineError(engine::PlaybackEngine&, BindContext&)> apply;
};

// One scenario bound to a song duration (SpikeScenario).
struct BoundScenario {
    std::string name;
    int snapshot_mode = 0;              // 0 none, 1 SNAP, 2 SNAP + SNAPX
    bool stop_after_end = true;
    bool has_route = false;
    routing::TrackRoutingSettings route;
    std::vector<SetupAction> setup;
    std::vector<BoundCommand> commands;
};

// Binds every scenario against a dummy duration so malformed steps are
// reported before any run starts. Returns false with `error` set.
bool validate_scenarios(const std::vector<ScenarioDefinition>& definitions, std::string& error);

// duration = max(1000, song duration in microseconds).
bool bind_scenario(const ScenarioDefinition& definition, int64_t duration, BoundScenario& scenario, std::string& error);

} // namespace harness
} // namespace midibn
