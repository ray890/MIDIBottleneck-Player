// Internal to the model-trace replay (model_trace.cpp and the per-family
// replay files). Formats here must match v2/harness/v1-reference/V1ModelHarness*.cs.
#pragma once

#include "midibn/model_trace.h"
#include "midibn/models/ordered_admission.h"
#include "midibn/models/rate.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace midibn {
namespace models {

class PerNoteIntervalGate;
class DelayedNoteOffGate;
class MusicalAttackRateLimiter;
class OutputRateGate;

struct GateInstances;  // defined with the gate replay
struct GateInstancesDeleter { void operator()(GateInstances* p) const; };
struct AnalysisReplay;  // defined with the Analysis and routing replay
struct AnalysisReplayDeleter { void operator()(AnalysisReplay* p) const; };

struct ReplayContext {
    const FixtureLoader* loader = nullptr;
    std::string family;  // from the trace's "family" line
    std::shared_ptr<Song> song;
    int64_t song_serial = 0;  // counts fixture loads (v1 creates a new MidiSong each time)
    EventSource events;

    // Group 1
    std::unique_ptr<ServiceDurationClock> clock;
    std::unique_ptr<ProgressiveRatePacer> pacer;
    std::unique_ptr<AdaptiveNoteIntervalClock> adaptive;
    // Group 2
    std::unique_ptr<PendingMidiQueue> pending;
    std::unique_ptr<ForwardDropQueue> fdq;
    std::unique_ptr<CompleteNoteTracker> tracker;
    std::unique_ptr<OrderedFiniteAdmissionCore> ordered;
    MusicalNotePrioritySelector selector;
    // Groups 3-6
    std::unique_ptr<GateInstances, GateInstancesDeleter> gates;
    // Milestone 4
    std::unique_ptr<AnalysisReplay, AnalysisReplayDeleter> analysis;
};

using Args = std::vector<std::string>;

struct OpOutput {
    bool has_result = false;
    std::string result;
    std::function<std::string()> state;
    void set(const std::string& text) { has_result = true; result = text; }
};

// Formatting shared with the C# harness.
std::string s(int64_t value);
inline std::string b(bool value) { return value ? "1" : "0"; }
std::string hex2(unsigned value);
std::string digest(const std::string& text);  // FNV-1a 64, 16 lower-case hex digits
std::string ev_text(const EventView& e);
// Append forms (no temporaries) for the large per-op state texts.
void append_number(std::string& text, int64_t value);
void append_event(std::string& text, const EventView& e);
std::string item_text(const OutputWorkItem& item);
std::string items_text(const std::vector<OutputWorkItem>& items);

int64_t to_i64(const std::string& text);
EventView parse_event(const ReplayContext& context, const std::string& token);

// Per-family handlers: return false when the op name is not theirs.
bool replay_rate_op(ReplayContext& context, const Args& args, OpOutput& out);
bool replay_queue_op(ReplayContext& context, const Args& args, OpOutput& out);
bool replay_ordered_op(ReplayContext& context, const Args& args, OpOutput& out);
bool replay_gate_op(ReplayContext& context, const Args& args, OpOutput& out);
bool replay_analysis_op(ReplayContext& context, const Args& args, OpOutput& out);

// State texts reused across families.
std::string pacer_state(const ProgressiveRatePacer* pacer);
std::string adaptive_state(const AdaptiveNoteIntervalClock& adaptive);
std::string tracker_text(const CompleteNoteTracker& tracker);
std::string adaptive_family_state(ReplayContext& context);

} // namespace models
} // namespace midibn
