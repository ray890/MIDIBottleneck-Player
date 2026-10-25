// The engine comparison harness's platform services: a port of the v1 reference
// harness's lock-step rendezvous (LockStepScheduler.cs, EventWaitHandleShadow.cs,
// the VirtualClockShims.cs types) as a platform::EngineServices implementation
// for the v2 engine.
//
// Exactly one of {harness thread, worker thread} runs engine code at a time:
//  - the worker runs until it parks (a timed Waiter::wait, the paused worker's
//    untimed WakeEvent::wait, or a yield at the end of a completed output send
//    once a scripted command is due) and hands the turn to the harness;
//  - the harness either applies the next scripted command (virtual time moved
//    to the command time) or advances virtual time to the parked deadline and
//    releases the worker;
//  - a parked worker is also released by the engine's wake event (or, for a
//    send-boundary yield, by a change of the engine's wake generation).
// The method is the C# spike's, line for line, with real threads; see
// v2/harness/ENGINE_SCENARIOS.md section 8 and the "v2 harness" section there.
// Every real-time wait has a 20 s limit; a breached limit raises the same
// TimeoutException text as the spike.
#pragma once

#include "midibn/engine/engine_types.h"
#include "midibn/platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace midibn {
namespace harness {

class LockStepEvent;
class LockStepWaiter;
class LockStepThread;
class LockStepClock;

class LockStepScheduler : public platform::EngineServices {
public:
    explicit LockStepScheduler(int hangMilliseconds);
    ~LockStepScheduler() override;
    LockStepScheduler(const LockStepScheduler&) = delete;
    LockStepScheduler& operator=(const LockStepScheduler&) = delete;

    // ---- platform::EngineServices
    platform::Clock& clock() override;
    std::unique_ptr<platform::WakeEvent> create_wake_event(platform::WakeEventRole role, bool initiallySet,
        bool autoReset) override;
    std::unique_ptr<platform::Waiter> create_waiter(platform::WakeEvent& wake, platform::PlatformError& error) override;
    std::unique_ptr<platform::Thread> start_thread(std::function<void()> body, platform::ThreadPriority priority,
        const char* name, platform::PlatformError& error) override;

    // ---- virtual time (VirtualTime in the spike): one tick is one microsecond.
    int64_t now() const { return ticks_.load(); }
    void set_time(int64_t value) { ticks_.store(value); }
    void advance(int64_t microseconds) { if (microseconds > 0) ticks_.fetch_add(microseconds); }

    // ---- installed by the driver before the first command
    std::function<int32_t()> engine_wake_generation;
    std::function<bool()> engine_playing;
    std::atomic<int64_t> next_command_time{ INT64_MAX };

    // ---- worker side
    // Output side: after a completed send, yield when a scripted command is due.
    void send_boundary();
    // True on the registered worker thread.
    bool is_worker_thread() const;

    // ---- harness side
    void begin_command();
    void end_command();
    int worker_generation();
    int64_t park_seq();
    void wait_for_worker_generation(int generation);
    // Waits until the worker is parked in a park newer than seenSeq (true,
    // with its deadline) or has exited / none exists (false).
    bool wait_quiescent(int64_t seenSeq, int64_t& deadline, int64_t& parkSeq);
    bool command_released_worker(int64_t parkSeq, int32_t wakeGenerationBefore);
    void release_to_deadline(int64_t deadline);
    void note(const std::string& line);
    // Releases every waiting party; used after a harness error so threads can end.
    void abort();
    // abort(), then waits for every started thread to end (except the calling one).
    void abort_and_join();

    // ---- results
    const std::string& trace() const { return trace_; }
    int64_t parks = 0;
    int64_t go_resumes = 0;
    int64_t wake_resumes = 0;
    std::atomic<int64_t> spin_count{ 0 };
    int64_t send_yields = 0;
    int64_t event_waits = 0;
    int64_t control_waits = 0;
    int64_t control_timeouts = 0;
    std::string protocol_error;   // guarded by gate_ while threads run
    // The newest park the running harness command already accounts for.
    std::atomic<int64_t> command_seen_seq{ 0 };

    static constexpr int64_t kMaximumIdleReads = 5000000;

private:
    friend class LockStepEvent;
    friend class LockStepWaiter;
    friend class LockStepThread;
    friend class LockStepClock;

    struct ThreadRecord {
        std::thread thread;
        std::atomic<bool> alive{ true };
        bool detached = false;
    };

    // Park machinery (all callers hold no lock).
    bool park(LockStepEvent* wake, int64_t microseconds, const char* kind);
    int park_core(LockStepEvent* wake, int64_t microseconds, const char* kind);
    bool wait_from_worker(LockStepEvent& handle, int32_t milliseconds);
    bool wait_from_control(LockStepEvent& handle, int32_t milliseconds);
    bool control_timeout(int64_t controlDeadline);
    void worker_waiter_disposed();
    bool register_worker(platform::PlatformError& error);
    void clock_read(int64_t now);
    [[noreturn]] void raise_timeout(const std::string& text);
    [[noreturn]] void raise_hang_locked(const std::string& text);
    void trace_line(const char* kind, int64_t t, bool hasUntil, int64_t until, bool untilNone);
    // Every state change is followed by notify(). wait_slice() waits for a change for at most `milliseconds`
    // (the caller re-checks its condition under the lock; the timeout also covers a missed notification).
    void notify();
    void wait_slice(std::unique_lock<std::mutex>& lock, int milliseconds);

    const int hang_ms_;
    std::atomic<int64_t> ticks_{ 0 };
    mutable std::mutex gate_;
    std::condition_variable cv_;
    bool go_ = false;                       // AutoResetEvent _go
    std::atomic<bool> aborted_{ false };
    bool command_active_ = false;
    std::atomic<ThreadRecord*> worker_{ nullptr };  // the registered worker (_worker)
    int worker_generation_ = 0;
    bool parked_ = false;
    int64_t park_deadline_ = 0;
    int64_t park_seq_ = 0;
    int32_t park_generation_ = 0;
    int64_t idle_read_time_ = -1;           // worker thread only
    int64_t idle_reads_ = 0;
    std::string trace_;
    std::vector<std::shared_ptr<ThreadRecord>> threads_;
    std::unique_ptr<LockStepClock> clock_;
};

} // namespace harness
} // namespace midibn
