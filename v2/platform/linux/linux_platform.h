// The Linux platform layer (Milestone 8, stages 3 and 4; ARCHITECTURE.md section 4): POSIX implementations of the core
// platform interfaces. It replaces the stand-in platform/portable of stage 2.
//
//   Clock        CLOCK_MONOTONIC, nanosecond ticks
//   WakeEvent    mutex + condition variable on CLOCK_MONOTONIC (absolute deadlines)
//   Waiter       absolute-deadline timed wait for all but the last 200 microseconds (the caller spins the rest);
//                the same split and the same return codes as the Win32 waiter
//   Thread       pthreads: name (cut to the 15 characters Linux allows), a modest priority raise that falls back
//                silently, join with timeout, is_alive, is_current (true on an abandoned worker after detach)
//   FileSystem   UTF-8 paths, fstat size, pread, posix_fadvise(SEQUENTIAL)
//   Memory       VmHWM / VmPeak from /proc/self/status
//
// The tests and tools were written against the Win32 class names. win32_platform.h and win32_timing.h in this folder
// (found only on non-Windows builds) map those names onto the classes below, so no test source changed.
// Nothing in this file opens a MIDI device. The ALSA output is in alsa_midi_output.h.
#pragma once

#include "midibn/platform.h"

#include <pthread.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace midibn {
namespace posix {

class PosixFileSystem : public platform::FileSystem {
public:
    std::unique_ptr<platform::ByteSource> open_read(const std::string& path_utf8, std::string& error) override;
};

struct ProcessMemory {
    uint64_t peak_working_set = 0;   // peak resident set: VmHWM
    uint64_t peak_private = 0;       // peak virtual size: VmPeak (the closest peak to Windows' peak commit; an upper bound)
    uint64_t current_private = 0;    // current virtual size: VmSize
};
ProcessMemory process_memory();

// "Linux <release> (<arch> process)"
std::string describe_os();

double milliseconds_since(int64_t start_counter);
int64_t performance_counter();   // nanoseconds, CLOCK_MONOTONIC

class MonotonicClock : public platform::Clock {
public:
    int64_t now_ticks() const override { return performance_counter(); }
    int64_t frequency() const override { return 1000000000; }
};

class PosixWakeEvent : public platform::WakeEvent {
public:
    explicit PosixWakeEvent(bool initiallySet = false, bool autoReset = false);
    ~PosixWakeEvent() override;
    PosixWakeEvent(const PosixWakeEvent&) = delete;
    PosixWakeEvent& operator=(const PosixWakeEvent&) = delete;
    void set() override;
    void reset() override;
    bool poll() override { return wait(0); }
    bool wait(int32_t timeoutMilliseconds) override;
    // Waits until the CLOCK_MONOTONIC time `deadline_ns` for the signal (consumes it for an auto-reset event).
    bool wait_until(int64_t deadline_ns);

private:
    pthread_mutex_t mutex_;
    pthread_cond_t condition_;
    bool signalled_;
    bool auto_reset_;
};

// Coarse wait to an absolute deadline `microseconds - 200` ahead (cut short by the wake event); the caller spins the
// last ~200 microseconds with spin_once().
class PosixWaiter : public platform::Waiter {
public:
    explicit PosixWaiter(PosixWakeEvent& wake) : wake_(wake) {}
    platform::WaitResult wait(int64_t microseconds, platform::PlatformError& error) override;
    void spin_once() override;

private:
    PosixWakeEvent& wake_;
};

// What a started thread got of the requested priority (for tests and the capability report).
enum class PriorityOutcome { NotRequested, Raised, Refused };

// The nice value requested for ThreadPriority::AboveNormal. An unprivileged process is refused (nice below 0 needs
// CAP_SYS_NICE or RLIMIT_NICE); the thread then silently stays at Normal. Real-time scheduling is never used.
constexpr int kAboveNormalNice = -3;

class PosixThread : public platform::Thread {
public:
    // Returns null and sets `error` when the thread cannot be started. `name` may be null (UTF-8, cut to 15 bytes).
    // `body` is taken by rvalue reference and emptied here: libc++ copies a small std::function on move and leaves the source
    // holding its captures, which would let the caller thread drop the last reference to them after the new thread ran.
    static std::unique_ptr<PosixThread> start(std::function<void()>&& body, platform::ThreadPriority priority,
        const char* name, std::string& error);
    ~PosixThread() override;
    void join() override;
    bool join(int32_t timeoutMilliseconds) override;
    bool is_alive() const override;
    bool is_current() const override;
    void detach() override;
    // Known once the thread has started (start() waits for it).
    PriorityOutcome priority_outcome() const;
    // The nice value the thread ran at (-20..19) as seen from the thread itself.
    int nice_value() const;

private:
    struct State;
    PosixThread() {}
    std::shared_ptr<State> state_;
    pthread_t thread_ = pthread_t();
    bool started_ = false;
};

class PosixEngineServices : public platform::EngineServices {
public:
    platform::Clock& clock() override { return clock_; }
    std::unique_ptr<platform::WakeEvent> create_wake_event(platform::WakeEventRole role, bool initiallySet,
        bool autoReset) override;
    std::unique_ptr<platform::Waiter> create_waiter(platform::WakeEvent& wake, platform::PlatformError& error) override;
    std::unique_ptr<platform::Thread> start_thread(std::function<void()> body, platform::ThreadPriority priority,
        const char* name, platform::PlatformError& error) override;
    void yield_to_others() override;

private:
    MonotonicClock clock_;
};

} // namespace posix
} // namespace midibn
