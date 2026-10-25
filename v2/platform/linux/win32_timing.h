// Non-Windows builds only: the Win32 timing class names the tests and tools use, mapped onto the Linux platform layer
// (linux_platform.h). The Windows build uses platform/win32/win32_timing.h instead.
#pragma once

#include "linux_platform.h"

namespace midibn {
namespace win32 {

using QpcClock = posix::MonotonicClock;
using Win32WakeEvent = posix::PosixWakeEvent;
using Win32Thread = posix::PosixThread;
using Win32EngineServices = posix::PosixEngineServices;

// The Linux waiter is created with the same call the Win32 one has.
struct Win32Waiter {
    static std::unique_ptr<posix::PosixWaiter> create(Win32WakeEvent& wake, std::string&)
    {
        return std::unique_ptr<posix::PosixWaiter>(new posix::PosixWaiter(wake));
    }
};

// The few-processor policy is a Windows option; the Linux layer never changes thread behaviour for it.
inline void set_few_cpu_policy(bool, int) {}
inline void set_few_cpu_nap(bool) {}
inline bool few_cpu_normal_threads_active() { return false; }
inline int few_cpu_yield_milliseconds_active() { return 0; }

} // namespace win32
} // namespace midibn
