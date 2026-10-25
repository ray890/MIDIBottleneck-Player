// The Rate model choices of v1's main window (RateModelComboBox.cs) and the
// pure helpers MainForm uses to relate them to the engine's settings.
#pragma once

#include "midibn/engine/engine_types.h"
#include "midibn/models/rate.h"

#include <string>

namespace midibn {
namespace app {

// v1 RateModelChoice (the order is part of the persisted/tested API).
enum class RateModelChoice {
    None,
    ProcessingTime,
    MidiBitrate,
    EventsPerSecond,
    PerNoteIntervalGate,
    DelayedNoteOff,
    NoteAttackLimit,
    OutputRateLimit
};

const int kRateModelChoiceCount = 8;

inline bool is_ordinary_rate_choice(RateModelChoice choice)
{
    return choice == RateModelChoice::ProcessingTime || choice == RateModelChoice::MidiBitrate ||
        choice == RateModelChoice::EventsPerSecond;
}

inline bool is_musical_rate_choice(RateModelChoice choice)
{
    return choice == RateModelChoice::PerNoteIntervalGate || choice == RateModelChoice::DelayedNoteOff ||
        choice == RateModelChoice::NoteAttackLimit || choice == RateModelChoice::OutputRateLimit;
}

inline bool is_interval_gate_choice(RateModelChoice choice)
{
    return choice == RateModelChoice::PerNoteIntervalGate || choice == RateModelChoice::DelayedNoteOff;
}

inline models::ServiceDurationMode ordinary_mode_for_choice(RateModelChoice choice)
{
    switch (choice) {
    case RateModelChoice::MidiBitrate: return models::ServiceDurationMode::MidiBitrate;
    case RateModelChoice::EventsPerSecond: return models::ServiceDurationMode::EventsPerSecond;
    default: return models::ServiceDurationMode::ProcessingTime;
    }
}

inline RateModelChoice choice_for_ordinary_mode(models::ServiceDurationMode mode)
{
    switch (mode) {
    case models::ServiceDurationMode::MidiBitrate: return RateModelChoice::MidiBitrate;
    case models::ServiceDurationMode::EventsPerSecond: return RateModelChoice::EventsPerSecond;
    default: return RateModelChoice::ProcessingTime;
    }
}

// v1 RateModelComboBox.FullLabel / FallbackLabel (the collapsed field shows
// the fallback when the full text does not fit).
const char* rate_choice_full_label(RateModelChoice choice);
const char* rate_choice_fallback_label(RateModelChoice choice);

// The popup's static group headings (v1 SlowdownHeadingText / "Bandwidth / note gating").
inline const char* rate_slowdown_heading(bool forwardOnly)
{
    return forwardOnly ? "Forward-only queue admission" : "Simulated slowdown";
}
inline const char* rate_gate_heading() { return "Bandwidth / note gating"; }

} // namespace app
} // namespace midibn
