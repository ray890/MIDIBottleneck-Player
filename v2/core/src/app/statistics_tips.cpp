#include "midibn/app/statistics_tips.h"

namespace midibn {
namespace app {

std::string statistics_cell_tooltip(const StatisticsText& t, int cell)
{
    std::string text;
    if (cell == 5)
        text = t.per_note_gate
            ? "How quickly the gate has resolved the source timeline compared with real playback time. It advances only after each gate frame and its output calls finish, so a blocked output stalls the measurement. Current measurement window: " + t.speed_window_description + ". Right-click to change it."
            : "Output-timeline advancement relative to elapsed playback time. Current measurement window: " + t.speed_window_description + ". Right-click to change it.";
    else if (cell == 0)
        text = "Playback timeline / MIDI output position. The output position is the source timestamp of the most recently sent MIDI event.";
    else if (cell == 2)
        text = t.per_note_gate
            ? "One frame is one configured gate interval. Each of the 128 MIDI pitches can make at most one Note On or Note Off transition in a frame, while different pitches can transition together. Other MIDI messages are not limited by this figure, so it is not the player's total event-throughput ceiling."
            : "With immediate simulated service, this is the highest observed 250 ms rolling dispatch rate since statistics were reset\xE2\x80\x94not a theoretical hardware or scheduler capacity. Nonzero service models show their theoretical configured maximum.";
    else if (cell == 4)
        text = "Sent means MIDI messages successfully dispatched. Dropped is one total covering queue overflow and note or rate-gate exclusions; detailed reason counters remain available in diagnostics. Forced Override suppression is not added again. With None, sent means logically consumed; no physical MIDI data leaves the application.";
    else if (cell == 6 || cell == 7)
        text = t.per_note_gate
            ? "Lag is how late the most recently sent MIDI event was compared with its original time in the file. In Per-note mode it includes waiting for a gate frame, scheduler delay, and a blocking output call. Different transitions in one frame can come from different source times, so current lag may vary. It cannot measure synthesizer rendering or audio-device latency."
            : "Lag is lateness through MIDI dispatch, including scheduler delay or a blocking output call. It cannot measure synthesizer rendering or audio-device latency.";
    return text;
}

} // namespace app
} // namespace midibn
