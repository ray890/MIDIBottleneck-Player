#include "midibn/app/rate_model.h"

namespace midibn {
namespace app {

const char* rate_choice_full_label(RateModelChoice choice)
{
    switch (choice) {
    case RateModelChoice::ProcessingTime: return "Processing time per event";
    case RateModelChoice::MidiBitrate: return "MIDI serial bitrate";
    case RateModelChoice::EventsPerSecond: return "Events per second";
    case RateModelChoice::PerNoteIntervalGate: return "Per-note interval gate";
    case RateModelChoice::DelayedNoteOff: return "Delayed NoteOff";
    case RateModelChoice::NoteAttackLimit: return "Note start rate gate";
    case RateModelChoice::OutputRateLimit: return "Output rate gate";
    default: return "None";
    }
}

const char* rate_choice_fallback_label(RateModelChoice choice)
{
    switch (choice) {
    case RateModelChoice::ProcessingTime: return "Time per event";
    case RateModelChoice::MidiBitrate: return "MIDI bitrate";
    case RateModelChoice::EventsPerSecond: return "Events/sec";
    case RateModelChoice::PerNoteIntervalGate: return "Per-note gate";
    case RateModelChoice::NoteAttackLimit: return "Note start gate";
    case RateModelChoice::OutputRateLimit: return "Output gate";
    default: return rate_choice_full_label(choice);
    }
}

} // namespace app
} // namespace midibn
