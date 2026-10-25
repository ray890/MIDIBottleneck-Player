// A deterministic, lock-step implementation of platform::EngineServices for
// engine tests (Milestone 5, Group D). It models the rendezvous rules of the
// v1 engine-harness spike (ENGINE_HARNESS_FEASIBILITY.md) in a simplified
// form, so the real engine runs on a real worker thread but every blocking
// point is a hand-off with the test (control) thread:
//  - virtual time: a 1 MHz clock that moves only when the driver advances it,
//    when a spin happens (1 us per spin_once), or when a test output adds
//    a send cost (advance());
//  - the worker parks in Waiter::wait (deadline = now + us) and in the
//    paused worker's untimed WakeEvent::wait(-1); a wake signal releases it;
//  - the control thread's timed WakeEvent::wait (v1 _pauseAcknowledged
//    WaitOne(2000)) lends the turn to the worker: a parked worker whose
//    deadline falls inside the control deadline is released at that deadline,
//    otherwise the wait times out in virtual time;
//  - a worker started while a command runs waits at a start gate until the
//    command returns (the spike's start gate);
//  - Thread::join(timeout) waits (in real time) for the worker to finish.
// Every real-time wait has a safety limit; a breached limit marks the
// services as hung (hung()) and releases the waiter so a test fails instead
// of hanging.
//
// Group E's engine-comparison harness implements the same interface with the full
// spike protocol (yield after a send once a command is due, command
// delivery at the first scheduling point at or after its time).
#pragma once

#include "midibn/platform.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace midibn {
namespace lockstep {

class LockStepEngineServices;

class LockStepClock : public platform::Clock {
public:
    explicit LockStepClock(LockStepEngineServices& owner) : owner_(owner) {}
    int64_t now_ticks() const override;
    int64_t frequency() const override { return 1000000; }

private:
    LockStepEngineServices& owner_;
};

class LockStepEngineServices : public platform::EngineServices {
public:
    explicit LockStepEngineServices(int realTimeLimitMilliseconds = 20000);
    ~LockStepEngineServices() override;

    platform::Clock& clock() override { return clock_; }
    std::unique_ptr<platform::WakeEvent> create_wake_event(platform::WakeEventRole role, bool initiallySet,
        bool autoReset) override;
    std::unique_ptr<platform::Waiter> create_waiter(platform::WakeEvent& wake, platform::PlatformError& error) override;
    std::unique_ptr<platform::Thread> start_thread(std::function<void()> body, platform::ThreadPriority priority,
        const char* name, platform::PlatformError& error) override;

    // ---- driver (control thread)
    int64_t now() const;
    // Adds virtual time (a test output's send cost); callable from any thread.
    void advance(int64_t microseconds);
    // Runs `command` as one harness command (start gate held while it runs),
    // then waits until the worker is parked or no worker is alive.
    void command(const std::function<void()>& command);
    // Waits (real time) until the worker is parked or no worker is alive.
    bool settle();
    // Releases parked timed waits whose deadline is <= `time` (advancing
    // virtual time to each deadline), then moves virtual time to `time`.
    void run_until(int64_t time);
    // Releases every timed park until the worker exits or parks without a
    // deadline (paused). Returns false if `maximumParks` was exceeded.
    bool run_until_idle(int64_t maximumParks = 100000000);
    bool worker_alive() const;
    bool worker_parked_untimed() const;
    bool on_worker_thread() const;
    int64_t parks() const;
    int64_t spins() const;
    bool hung() const;

private:
    friend class LockStepClock;
    friend class LockStepWakeEvent;
    friend class LockStepWaiter;
    friend class LockStepThread;

    struct ThreadState {
        std::thread thread;
        std::thread::id id;
        bool alive = true;
        bool started_body = false;
        bool detached = false;
        std::mutex join_mutex;
    };

    bool wait_real(std::unique_lock<std::mutex>& lock, const std::function<bool()>& done);
    bool quiescent_locked() const;
    bool any_alive_locked() const;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    LockStepClock clock_;
    int64_t now_ = 0;
    int real_time_limit_ms_;
    bool command_active_ = false;
    std::thread::id worker_id_;
    bool parked_ = false;
    bool park_untimed_ = false;
    bool park_released_ = false;
    bool go_ = false;
    int64_t park_deadline_ = 0;
    int64_t park_seq_ = 0;
    int64_t parks_ = 0;
    int64_t spins_ = 0;
    bool hung_ = false;
    std::vector<std::shared_ptr<ThreadState>> threads_;
};

} // namespace lockstep
} // namespace midibn
