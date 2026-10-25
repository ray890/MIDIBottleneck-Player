#include "midibn/virtual_platform.h"

namespace midibn {
namespace platform {

void VirtualClock::advance_microseconds(int64_t microseconds)
{
    // Split quotient/remainder so large values cannot overflow (as v1's
    // MicrosecondsToTicks does).
    int64_t whole = microseconds / 1000000;
    int64_t rest = microseconds % 1000000;
    now_ += whole * frequency_ + rest * frequency_ / 1000000;
}

bool VirtualWakeEvent::poll()
{
    bool was = set_;
    if (was && auto_reset_) set_ = false;
    return was;
}

bool VirtualWakeEvent::wait(int32_t)
{
    return poll();
}

WaitResult VirtualWaiter::wait(int64_t microseconds, PlatformError&)
{
    if (microseconds <= 0) return WaitResult::Elapsed;
    if (wake_.poll()) return WaitResult::Woken;
    clock_.advance_microseconds(microseconds);
    return wake_.poll() ? WaitResult::Woken : WaitResult::Elapsed;
}

} // namespace platform
} // namespace midibn
