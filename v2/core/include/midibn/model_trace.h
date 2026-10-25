// Model traces (Milestone 3): replays the op lines of a v1 model trace
// against the v2 models and writes the v2 trace in the same format
// (v2/harness/TRACE_FORMAT.md, "Model traces").
#pragma once

#include "midibn/midi_loader.h"

#include <functional>
#include <memory>
#include <string>

namespace midibn {

// Loads a fixture by file name; returns null and fills `error` on failure.
using FixtureLoader = std::function<std::unique_ptr<Song>(const std::string& name, std::string& error)>;

// `script` is a v1 model trace. Its "> " op lines are replayed in order; a
// "~" state line is written after an op exactly when the script has one
// there (the script decides what to observe, never what the result is).
std::string replay_model_trace(const std::string& script, const FixtureLoader& loader, const std::string& producer);

} // namespace midibn
