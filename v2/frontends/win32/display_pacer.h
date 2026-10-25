// Display-paced refresh for the live displays (readouts, timeline, statistics, Channels, workload graph).
//
// One small background thread, alive only while at least one window has asked for live updates, posts ONE
// registered window message per display refresh to each subscribed window. It never posts a new message to a
// window until that window has said it finished the previous one, so a slow UI thread coalesces updates instead
// of building a backlog. The thread itself does no drawing and touches no player state.
//
// Pacing sources, best first:
//   Display - DwmFlush(): returns once per desktop-compositor pass, i.e. at the monitor's refresh rate
//             (60 Hz -> 60 updates per second, 144 Hz -> 144). dwmapi.dll is loaded at run time (Windows XP has none).
//   Timer   - a precise timer (default 60 per second) on absolute QueryPerformanceCounter deadlines, so it does
//             not drift. Used on Windows XP, with composition off (Windows Vista/7 Basic theme), in a remote
//             session, when DwmFlush fails or returns implausibly fast, and when asked for by a test switch.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace midibn {
namespace ui {

enum class PaceMode { Auto, Display, Timer };

class DisplayPacer {
public:
    static DisplayPacer& instance();

    // The registered window message that subscribers receive (same value for every window).
    static UINT message();

    // Test/override settings; call before the first subscribe. maxHz caps the display mode (0 = no cap);
    // timerHz is the rate of the timer mode (default 60).
    void configure(PaceMode mode, int maxHz, int timerHz);

    // Starts delivering the message to `window` (starts the pacing thread when it is the first subscriber).
    // Returns a token for unsubscribe/finished.
    int subscribe(HWND window);
    // Stops delivery (stops the thread when it was the last subscriber). A message already posted may still arrive.
    void unsubscribe(int token);
    // The subscriber has handled the message; the next one may be posted. Unknown tokens are ignored.
    void finished(int token);

    // Adaptive display rate: caps the rate of the posted messages at `hz` per second (0 = no extra cap) while running.
    // Works in both sources (the display mode skips compositor passes on an accumulating deadline, the timer mode
    // lengthens its period). Takes effect within one pacing period; safe to call from the interface thread.
    void set_rate_limit(int hz);
    // The source's own rate in updates per second: the measured compositor rate in display mode, the timer rate in
    // timer mode (0 until the thread has measured it).
    int native_hz() const;

    // Human-readable pacing state for the test report, e.g. "display (DwmFlush)" or "timer 60 Hz (remote session)".
    std::string description() const;
    // Messages posted, and ticks dropped because the previous message was still unprocessed.
    void counters(int64_t& posted, int64_t& coalesced) const;

private:
    DisplayPacer();
    DisplayPacer(const DisplayPacer&) = delete;
    DisplayPacer& operator=(const DisplayPacer&) = delete;
    struct Impl;
    Impl* impl_;
};

} // namespace ui
} // namespace midibn
