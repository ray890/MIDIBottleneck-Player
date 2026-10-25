// Deterministic implementations of the timing interfaces (ARCHITECTURE.md §4:
// "a VirtualClock implementation is mandatory for deterministic engine
// tests"). Time moves only when the test or a wait advances it.
#pragma once

#include "midibn/platform.h"

#include <cstdint>

namespace midibn {
namespace platform {

class VirtualClock : public Clock {
public:
    explicit VirtualClock(int64_t frequency = 1000000, int64_t start = 0) : frequency_(frequency), now_(start) {}
    int64_t now_ticks() const override { return now_; }
    int64_t frequency() const override { return frequency_; }
    void advance_ticks(int64_t ticks) { now_ += ticks; }
    // Advances by whole microseconds converted with the clock frequency (truncating).
    void advance_microseconds(int64_t microseconds);

private:
    int64_t frequency_;
    int64_t now_;
};

// Single-threaded wake event: wait() returns the current state at once
// (a test that needs blocking hand-offs uses the lock-step harness instead).
class VirtualWakeEvent : public WakeEvent {
public:
    explicit VirtualWakeEvent(bool initiallySet = false, bool autoReset = false)
        : set_(initiallySet), auto_reset_(autoReset) {}
    void set() override { set_ = true; }
    void reset() override { set_ = false; }
    bool poll() override;
    bool wait(int32_t timeoutMilliseconds) override;
    bool is_set() const { return set_; }

private:
    bool set_;
    bool auto_reset_;
};

// Waits by advancing the virtual clock by the full amount (no early wake),
// unless the wake event is already signalled. Each spin advances one tick.
class VirtualWaiter : public Waiter {
public:
    VirtualWaiter(VirtualClock& clock, WakeEvent& wake) : clock_(clock), wake_(wake) {}
    WaitResult wait(int64_t microseconds, PlatformError& error) override;
    void spin_once() override { clock_.advance_ticks(1); }

private:
    VirtualClock& clock_;
    WakeEvent& wake_;
};

} // namespace platform
} // namespace midibn
