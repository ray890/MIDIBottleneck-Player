// Wording of the failures that playback reports to the user when the system, not the song, is the cause.
// v1 showed the raw text of the .NET exception ("Exception of type 'System.OutOfMemoryException' was thrown.",
// "Arithmetic operation resulted in an overflow.", "Thread has not been started."); v2 says what failed and what
// the user can do (register entry DD-003, v2/docs/DIFFERENCES_FROM_V1.md).
#pragma once

namespace midibn {
namespace messages {

// An allocation failed somewhere in playback or loading.
inline const char* out_of_memory()
{
    return "There is not enough memory to complete this operation. Close other programs and try again.";
}

// The system refused to create the playback thread. Windows keeps its exact wording; other systems do not name Windows.
inline const char* playback_thread_not_started()
{
#ifdef _WIN32
    return "Windows could not start the playback thread (not enough memory or system resources). "
           "Close other programs and try again.";
#else
    return "The system could not start the playback thread (not enough memory or system resources). "
           "Close other programs and try again.";
#endif
}

// The file ends before its MIDI header is complete.
inline const char* header_ends_early()
{
    return "The file ended before its MIDI header was complete. It is damaged or not a complete MIDI file.";
}

// An event time (a tick position or its conversion to microseconds) is beyond what can be represented.
inline const char* event_time_too_large()
{
    return "An event time in this file is too large for the player to represent, so the file cannot be played. "
           "The file may be damaged.";
}

// The time until the next event is so large that the timer cannot be armed for it.
inline const char* wait_too_long()
{
    return "The time until the next MIDI event is too long to schedule, so playback stopped. "
           "Check the song's tempo and the playback timing settings.";
}

} // namespace messages
} // namespace midibn
