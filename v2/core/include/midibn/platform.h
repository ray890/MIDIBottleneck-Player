// Platform interfaces used by the portable core. The core never calls the
// operating system directly; implementations live in v2/platform/<os>.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace midibn {
namespace platform {

// Random-access read-only byte source (a file, in practice).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual int64_t size() const = 0;
    // Reads up to `count` bytes at `offset`; returns bytes read (0 at end).
    virtual size_t read_at(int64_t offset, uint8_t* buffer, size_t count) = 0;
};

// Opens byte sources by path. Paths are UTF-8 in the core.
class FileSystem {
public:
    virtual ~FileSystem() = default;
    // Returns null and sets `error` (user-visible text) when the file cannot be opened.
    virtual std::unique_ptr<ByteSource> open_read(const std::string& path_utf8, std::string& error) = 0;
};

// ---------------------------------------------------------------- timing and threads
// (Milestone 5; ARCHITECTURE.md §4). v1 reads time only through
// Stopwatch.GetTimestamp/Frequency and waits only through HighResolutionWaiter,
// SpinWait and the two EventWaitHandles _wake and _pauseAcknowledged.

// Monotonic tick source (v1 Stopwatch.GetTimestamp and Stopwatch.Frequency).
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t now_ticks() const = 0;
    virtual int64_t frequency() const = 0;
};

// A wake signal (v1 EventWaitHandle). Manual reset unless created as auto reset.
class WakeEvent {
public:
    virtual ~WakeEvent() = default;
    virtual void set() = 0;
    virtual void reset() = 0;
    // v1 WaitOne(0): true when signalled (an auto-reset event is consumed).
    virtual bool poll() = 0;
    // v1 WaitOne(timeout); a negative timeout waits without limit.
    virtual bool wait(int32_t timeoutMilliseconds) = 0;
};

// A failure that v1 raises as a .NET exception at the platform boundary:
// the exception type name (e.g. "Win32Exception", "OverflowException") and
// its Message text. Platform implementations report it; nothing throws.
struct PlatformError {
    std::string type;
    std::string message;
};

// Outcome of one v1 HighResolutionWaiter.WaitMicroseconds call.
enum class WaitResult {
    Elapsed,  // v1 returned true (the wait ran its course)
    Woken,    // v1 returned false (the wake event was or became signalled)
    Failed    // v1 threw; the error is filled in
};

// v1 HighResolutionWaiter.WaitMicroseconds: waits about `microseconds`
// (a real implementation wakes up to ~200 us early; the caller spins the
// rest), and returns Woken as soon as the wake event is signalled.
class Waiter {
public:
    virtual ~Waiter() = default;
    virtual WaitResult wait(int64_t microseconds, PlatformError& error) = 0;
    // Convenience form with v1's bool result (a failure reads as false).
    bool wait_microseconds(int64_t microseconds)
    {
        PlatformError error;
        return wait(microseconds, error) == WaitResult::Elapsed;
    }
    // v1 `new SpinWait()` before the final sub-timer spin loop: starts a fresh
    // spin sequence (a real implementation resets its spin count).
    virtual void begin_spin() {}
    // v1 SpinWait.SpinOnce during the final sub-timer interval.
    virtual void spin_once() = 0;
};

enum class ThreadPriority { Normal, AboveNormal };

// A started thread (v1 System.Threading.Thread as the engine uses it).
class Thread {
public:
    virtual ~Thread() = default;
    // Waits without limit for the thread to finish.
    virtual void join() = 0;
    // v1 Thread.Join(millisecondsTimeout): true when the thread finished.
    virtual bool join(int32_t timeoutMilliseconds) = 0;
    // v1 Thread.IsAlive: true from start until the body has returned.
    virtual bool is_alive() const = 0;
    // v1 `thread == Thread.CurrentThread`.
    virtual bool is_current() const = 0;
    // Gives up the thread without waiting for it (v1 abandons a background
    // worker that is still blocked in an output call). The body keeps
    // running and owns everything it uses; is_alive() is false afterwards.
    // Destroying a Thread that was neither joined nor detached joins it.
    virtual void detach() = 0;
};

// Which of the engine's two v1 EventWaitHandles an event stands for (both
// ManualReset in v1). A lock-step harness uses the role to model the two
// blocking waits: the paused worker's untimed wait on Wake, and the control
// thread's timed wait on PauseAcknowledged.
enum class WakeEventRole { Wake, PauseAcknowledged };

// Everything the playback engine needs from the platform (ARCHITECTURE.md
// §4 and §7). The engine never blocks except through these objects and its
// own mutex:
//  - clock(): every v1 Stopwatch.GetTimestamp/Frequency read;
//  - create_wake_event(): called twice in the engine constructor, in v1's
//    field order (_wake, then _pauseAcknowledged);
//  - create_waiter(): called first thing on each worker thread (v1
//    constructs HighResolutionWaiter there), so it also identifies the
//    worker; the waiter is destroyed on that thread before the worker's exit
//    bookkeeping (v1 `using`);
//  - start_thread(): v1 `new Thread(PlaybackWorker)` with the name "MIDI
//    bottleneck scheduler" and AboveNormal priority, called while the engine
//    mutex is held (as v1 does). The returned object reports is_alive() from
//    this call until the body returns. The body owns what it captures (the
//    engine core): the implementation must destroy the body on the thread
//    that ran it, not keep it in the Thread object.
//
// The engine shares ownership of its services (shared_ptr), so a worker the
// engine abandoned can never outlive the objects it uses.
class EngineServices {
public:
    virtual ~EngineServices() = default;
    virtual Clock& clock() = 0;
    virtual std::unique_ptr<WakeEvent> create_wake_event(WakeEventRole role, bool initiallySet, bool autoReset) = 0;
    // Returns null and fills `error` when no waiter can be created (v1:
    // Win32Exception "Unable to create the playback timer.").
    virtual std::unique_ptr<Waiter> create_waiter(WakeEvent& wake, PlatformError& error) = 0;
    // Returns null and fills `error` when the thread cannot be started.
    virtual std::unique_ptr<Thread> start_thread(std::function<void()> body, ThreadPriority priority,
        const char* name, PlatformError& error) = 0;
    // Optional "be polite while behind" (P10-THR, default off): when this returns more than 0, a worker that has sent
    // MIDI for that many microseconds without once waiting (it is behind) calls yield_to_others() so other threads
    // (the synthesizer's audio thread) get processor time. Musical behaviour is unchanged: nothing is skipped or delayed
    // by more than the other threads' time slice.
    virtual int64_t behind_yield_interval_microseconds() const { return 0; }
    virtual void yield_to_others() {}
};

} // namespace platform
} // namespace midibn
