#include "lockstep_services.h"

#include "midibn/user_messages.h"

#include <chrono>
#include <cstdio>

namespace midibn {
namespace lockstep {

namespace {

int64_t saturating_add(int64_t a, int64_t b)
{
    const int64_t max = INT64_MAX;
    return b > 0 && a > max - b ? max : a + b;
}

} // namespace

// ---------------------------------------------------------------- events

class LockStepWakeEvent : public platform::WakeEvent {
public:
    LockStepWakeEvent(LockStepEngineServices& owner, platform::WakeEventRole role, bool initiallySet, bool autoReset)
        : owner_(owner), role_(role), set_(initiallySet), auto_reset_(autoReset) {}

    void set() override
    {
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        set_ = true;
        // A wake signal releases a parked worker (v1: the waitable-timer wait
        // returns on _wake; the paused WaitOne returns on Set).
        if (role_ == platform::WakeEventRole::Wake && owner_.parked_) owner_.park_released_ = true;
        owner_.cv_.notify_all();
    }
    void reset() override
    {
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        set_ = false;
    }
    bool poll() override
    {
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        return poll_locked();
    }
    bool wait(int32_t timeoutMilliseconds) override;

    bool poll_locked()
    {
        bool was = set_;
        if (was && auto_reset_) set_ = false;
        return was;
    }
    bool is_set_locked() const { return set_; }

private:
    LockStepEngineServices& owner_;
    platform::WakeEventRole role_;
    bool set_;
    bool auto_reset_;
};

bool LockStepWakeEvent::wait(int32_t timeoutMilliseconds)
{
    std::unique_lock<std::mutex> lock(owner_.mutex_);
    if (timeoutMilliseconds == 0) return poll_locked();
    if (poll_locked()) return true;
    if (std::this_thread::get_id() == owner_.worker_id_) {
        // The paused worker's untimed wait: a park with no deadline that only
        // Set releases (a timed worker wait is not used by v1).
        owner_.parked_ = true;
        owner_.park_untimed_ = true;
        owner_.park_released_ = false;
        owner_.park_seq_++;
        owner_.parks_++;
        owner_.cv_.notify_all();
        owner_.wait_real(lock, [&] { return set_; });
        owner_.parked_ = false;
        owner_.park_untimed_ = false;
        owner_.cv_.notify_all();
        poll_locked();
        return true;
    }
    // Control thread (v1 _pauseAcknowledged.WaitOne(timeout)).
    int64_t controlDeadline = timeoutMilliseconds < 0 ? INT64_MAX
                                                      : saturating_add(owner_.now_, static_cast<int64_t>(timeoutMilliseconds) * 1000);
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    while (true) {
        if (poll_locked()) return true;
        bool stuck = owner_.parked_ && !owner_.park_released_;
        if (stuck) {
            if (!owner_.park_untimed_ && owner_.park_deadline_ <= controlDeadline) {
                if (owner_.now_ < owner_.park_deadline_) owner_.now_ = owner_.park_deadline_;
                owner_.go_ = true;
                owner_.park_released_ = true;
                owner_.cv_.notify_all();
                started = std::chrono::steady_clock::now();
            }
            else {
                if (controlDeadline != INT64_MAX && owner_.now_ < controlDeadline) owner_.now_ = controlDeadline;
                return false;
            }
        }
        else if (!owner_.parked_ && !owner_.any_alive_locked()) {
            if (controlDeadline != INT64_MAX && owner_.now_ < controlDeadline) owner_.now_ = controlDeadline;
            return false;
        }
        owner_.cv_.wait_for(lock, std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(owner_.real_time_limit_ms_)) {
            owner_.hung_ = true;
            return false;
        }
    }
}

// ---------------------------------------------------------------- waiter

class LockStepWaiter : public platform::Waiter {
public:
    LockStepWaiter(LockStepEngineServices& owner, LockStepWakeEvent& wake) : owner_(owner), wake_(wake) {}

    platform::WaitResult wait(int64_t microseconds, platform::PlatformError& error) override
    {
        if (microseconds <= 0) return platform::WaitResult::Elapsed;
        std::unique_lock<std::mutex> lock(owner_.mutex_);
        if (wake_.poll_locked()) return platform::WaitResult::Woken;
        int64_t coarse = microseconds - 200;
        if (coarse > 0 && coarse > INT64_MAX / 10) {
            // v1 HighResolutionWaiter: checked(coarse * 10).
            error.type = "OverflowException";
            error.message = messages::wait_too_long();
            return platform::WaitResult::Failed;
        }
        owner_.parked_ = true;
        owner_.park_untimed_ = false;
        owner_.park_released_ = false;
        owner_.go_ = false;
        owner_.park_deadline_ = saturating_add(owner_.now_, microseconds);
        owner_.park_seq_++;
        owner_.parks_++;
        owner_.cv_.notify_all();
        owner_.wait_real(lock, [&] { return owner_.go_ || wake_.is_set_locked(); });
        bool byGo = owner_.go_;
        owner_.go_ = false;
        owner_.parked_ = false;
        owner_.cv_.notify_all();
        if (!byGo) return platform::WaitResult::Woken;
        return wake_.poll_locked() ? platform::WaitResult::Woken : platform::WaitResult::Elapsed;
    }

    void spin_once() override
    {
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        owner_.now_++;
        owner_.spins_++;
    }

private:
    LockStepEngineServices& owner_;
    LockStepWakeEvent& wake_;
};

// ---------------------------------------------------------------- thread

class LockStepThread : public platform::Thread {
public:
    LockStepThread(LockStepEngineServices& owner, std::shared_ptr<LockStepEngineServices::ThreadState> state)
        : owner_(owner), state_(std::move(state)) {}
    ~LockStepThread() override
    {
        bool detached;
        {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            detached = state_->detached;
        }
        if (!detached) join();
    }

    void join() override
    {
        std::lock_guard<std::mutex> joinLock(state_->join_mutex);
        if (state_->thread.joinable() && std::this_thread::get_id() != state_->id) state_->thread.join();
    }
    bool join(int32_t timeoutMilliseconds) override
    {
        {
            std::unique_lock<std::mutex> lock(owner_.mutex_);
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
            while (state_->alive) {
                // A worker that parks again instead of exiting would only
                // return through the timeout (v1: Thread.Join(2000) fails).
                if (owner_.parked_ && !owner_.park_released_ && owner_.worker_id_ == state_->id) {
                    owner_.now_ = saturating_add(owner_.now_, static_cast<int64_t>(timeoutMilliseconds) * 1000);
                    return false;
                }
                owner_.cv_.wait_for(lock, std::chrono::milliseconds(1));
                if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(owner_.real_time_limit_ms_)) {
                    owner_.hung_ = true;
                    return false;
                }
            }
        }
        join();
        return true;
    }
    bool is_alive() const override
    {
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        return state_->alive;
    }
    bool is_current() const override { return std::this_thread::get_id() == state_->id; }
    void detach() override
    {
        // The services object still waits for the body to finish in its
        // destructor, so the detached body never outlives the services.
        std::lock_guard<std::mutex> lock(owner_.mutex_);
        state_->detached = true;
    }

private:
    LockStepEngineServices& owner_;
    std::shared_ptr<LockStepEngineServices::ThreadState> state_;
};

// ---------------------------------------------------------------- services

int64_t LockStepClock::now_ticks() const
{
    std::lock_guard<std::mutex> lock(owner_.mutex_);
    return owner_.now_;
}

LockStepEngineServices::LockStepEngineServices(int realTimeLimitMilliseconds)
    : clock_(*this), real_time_limit_ms_(realTimeLimitMilliseconds)
{
}

LockStepEngineServices::~LockStepEngineServices()
{
    std::vector<std::shared_ptr<ThreadState>> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads = threads_;
    }
    for (std::shared_ptr<ThreadState>& t : threads) {
        std::lock_guard<std::mutex> joinLock(t->join_mutex);
        if (!t->thread.joinable()) continue;
        // The last owner may be an engine core released on its own worker
        // thread: that thread cannot join itself.
        if (std::this_thread::get_id() == t->id) t->thread.detach();
        else t->thread.join();
    }
}

std::unique_ptr<platform::WakeEvent> LockStepEngineServices::create_wake_event(platform::WakeEventRole role,
    bool initiallySet, bool autoReset)
{
    return std::unique_ptr<platform::WakeEvent>(new LockStepWakeEvent(*this, role, initiallySet, autoReset));
}

std::unique_ptr<platform::Waiter> LockStepEngineServices::create_waiter(platform::WakeEvent& wake,
    platform::PlatformError&)
{
    {
        // Called first thing on the worker thread: it identifies the worker.
        std::lock_guard<std::mutex> lock(mutex_);
        worker_id_ = std::this_thread::get_id();
    }
    return std::unique_ptr<platform::Waiter>(new LockStepWaiter(*this, static_cast<LockStepWakeEvent&>(wake)));
}

std::unique_ptr<platform::Thread> LockStepEngineServices::start_thread(std::function<void()> body,
    platform::ThreadPriority, const char*, platform::PlatformError&)
{
    std::shared_ptr<ThreadState> state = std::make_shared<ThreadState>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads_.push_back(state);
    }
    state->thread = std::thread([this, state, body] {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            state->id = std::this_thread::get_id();
            state->started_body = true;
            cv_.notify_all();
            // Start gate: run only once the command that started us returned.
            wait_real(lock, [&] { return !command_active_; });
        }
        body();
        std::lock_guard<std::mutex> lock(mutex_);
        state->alive = false;
        if (worker_id_ == state->id) worker_id_ = std::thread::id();
        cv_.notify_all();
    });
    {
        // Make the thread id known before returning (v1 Thread.Start).
        std::unique_lock<std::mutex> lock(mutex_);
        wait_real(lock, [&] { return state->started_body; });
    }
    return std::unique_ptr<platform::Thread>(new LockStepThread(*this, state));
}

bool LockStepEngineServices::wait_real(std::unique_lock<std::mutex>& lock, const std::function<bool()>& done)
{
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    while (!done()) {
        cv_.wait_for(lock, std::chrono::milliseconds(2));
        if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(real_time_limit_ms_)) {
            if (!hung_) std::printf("lock-step: real-time limit exceeded\n");
            hung_ = true;
            return false;
        }
    }
    return true;
}

int64_t LockStepEngineServices::now() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return now_;
}

void LockStepEngineServices::advance(int64_t microseconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (microseconds > 0) now_ = saturating_add(now_, microseconds);
}

bool LockStepEngineServices::any_alive_locked() const
{
    for (const std::shared_ptr<ThreadState>& t : threads_)
        if (t->alive) return true;
    return false;
}

bool LockStepEngineServices::quiescent_locked() const
{
    if (parked_ && !park_released_) return true;
    return !any_alive_locked();
}

void LockStepEngineServices::command(const std::function<void()>& command)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_active_ = true;
    }
    command();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_active_ = false;
        cv_.notify_all();
    }
    settle();
}

bool LockStepEngineServices::settle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    return wait_real(lock, [&] { return quiescent_locked(); });
}

void LockStepEngineServices::run_until(int64_t time)
{
    while (true) {
        settle();
        std::unique_lock<std::mutex> lock(mutex_);
        if (hung_) return;
        if (parked_ && !park_released_ && !park_untimed_ && park_deadline_ <= time) {
            if (now_ < park_deadline_) now_ = park_deadline_;
            go_ = true;
            park_released_ = true;
            cv_.notify_all();
            continue;
        }
        if (now_ < time) now_ = time;
        return;
    }
}

bool LockStepEngineServices::run_until_idle(int64_t maximumParks)
{
    int64_t released = 0;
    while (true) {
        settle();
        std::unique_lock<std::mutex> lock(mutex_);
        if (hung_) return false;
        if (!any_alive_locked()) return true;
        if (parked_ && park_untimed_) return true;
        if (parked_ && !park_released_) {
            if (++released > maximumParks) return false;
            if (now_ < park_deadline_) now_ = park_deadline_;
            go_ = true;
            park_released_ = true;
            cv_.notify_all();
        }
    }
}

bool LockStepEngineServices::worker_alive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return any_alive_locked();
}

bool LockStepEngineServices::worker_parked_untimed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return parked_ && park_untimed_;
}

bool LockStepEngineServices::on_worker_thread() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_id_ != std::thread::id() && std::this_thread::get_id() == worker_id_;
}

int64_t LockStepEngineServices::parks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return parks_;
}

int64_t LockStepEngineServices::spins() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return spins_;
}

bool LockStepEngineServices::hung() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hung_;
}

} // namespace lockstep
} // namespace midibn
