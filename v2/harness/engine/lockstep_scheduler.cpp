#include "lockstep_scheduler.h"

#include "midibn/user_messages.h"

#include <chrono>

namespace midibn {
namespace harness {

namespace {

// The ThreadRecord of the calling thread when it was started by the scheduler.
thread_local void* t_record = nullptr;

typedef std::chrono::steady_clock RealClock;

int64_t saturating_add(int64_t a, int64_t b)
{
    return a > INT64_MAX - b ? INT64_MAX : a + b;
}

void append_number(std::string& text, int64_t value)
{
    char buffer[24];
    int n = 0;
    bool negative = value < 0;
    uint64_t v = negative ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    do {
        buffer[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    if (negative) text.push_back('-');
    while (n > 0) text.push_back(buffer[--n]);
}

engine::EngineError timeout_error(const std::string& protocolText)
{
    return engine::EngineError::make("TimeoutException", "LockStepScheduler: " + protocolText);
}

} // namespace

// ---------------------------------------------------------------- clock

class LockStepClock : public platform::Clock {
public:
    explicit LockStepClock(LockStepScheduler& owner) : owner_(owner) {}
    // v1 Stopwatch.GetTimestamp: the virtual clock, with the livelock guard.
    int64_t now_ticks() const override
    {
        int64_t now = owner_.now();
        owner_.clock_read(now);
        return now;
    }
    int64_t frequency() const override { return 1000000; }

private:
    LockStepScheduler& owner_;
};

// ---------------------------------------------------------------- events

// The shadow EventWaitHandle of EventWaitHandleShadow.cs: the state is an
// ordinary flag (guarded by the scheduler's gate); only the two blocking waits
// are routed to the scheduler.
class LockStepEvent : public platform::WakeEvent {
public:
    LockStepEvent(LockStepScheduler& owner, bool initiallySet, bool autoReset)
        : owner_(owner), set_(initiallySet), auto_reset_(autoReset) {}

    void set() override
    {
        {
            std::lock_guard<std::mutex> lock(owner_.gate_);
            set_ = true;
        }
        owner_.notify();
    }
    void reset() override
    {
        std::lock_guard<std::mutex> lock(owner_.gate_);
        set_ = false;
    }
    bool poll() override
    {
        std::lock_guard<std::mutex> lock(owner_.gate_);
        return poll_locked();
    }
    bool wait(int32_t timeoutMilliseconds) override
    {
        if (timeoutMilliseconds < -1) throw engine::EngineError::make("ArgumentOutOfRangeException", "millisecondsTimeout");
        if (timeoutMilliseconds == 0) return poll();
        if (owner_.is_worker_thread()) return owner_.wait_from_worker(*this, timeoutMilliseconds);
        if (timeoutMilliseconds < 0)
            throw engine::EngineError::make("InvalidOperationException",
                "Untimed EventWaitHandle wait on a non-worker thread would block the lock-step schedule.");
        return owner_.wait_from_control(*this, timeoutMilliseconds);
    }

    // PollSignaled (consumes the signal of an auto-reset event).
    bool poll_locked()
    {
        bool was = set_;
        if (was && auto_reset_) set_ = false;
        return was;
    }
    bool is_set_locked() const { return set_; }
    bool auto_reset() const { return auto_reset_; }
    void consume_locked() { if (auto_reset_) set_ = false; }

private:
    LockStepScheduler& owner_;
    bool set_;
    bool auto_reset_;
};

// ---------------------------------------------------------------- waiter

// HighResolutionWaiter of VirtualClockShims.cs. Construction (registering the
// worker, and the start gate) happens in create_waiter.
class LockStepWaiter : public platform::Waiter {
public:
    LockStepWaiter(LockStepScheduler& owner, LockStepEvent& wake) : owner_(owner), wake_(wake) {}
    ~LockStepWaiter() override { owner_.worker_waiter_disposed(); }

    platform::WaitResult wait(int64_t microseconds, platform::PlatformError& error) override
    {
        if (microseconds <= 0) return platform::WaitResult::Elapsed;
        if (wake_.poll()) return platform::WaitResult::Woken;
        // Keep v1's overflow behaviour for absurd waits (checked coarse*10).
        int64_t coarse = microseconds - 200;
        if (coarse > 0 && coarse > INT64_MAX / 10) {
            error.type = "OverflowException";
            error.message = messages::wait_too_long();
            return platform::WaitResult::Failed;
        }
        try {
            return owner_.park(&wake_, microseconds, "PARK") ? platform::WaitResult::Elapsed : platform::WaitResult::Woken;
        }
        catch (const engine::EngineError& failure) {
            error.type = failure.type;
            error.message = failure.message;
            return platform::WaitResult::Failed;
        }
    }

    // v1's SpinWait shim: each spin is one virtual microsecond.
    void spin_once() override
    {
        owner_.spin_count.fetch_add(1);
        owner_.advance(1);
    }

private:
    LockStepScheduler& owner_;
    LockStepEvent& wake_;
};

// ---------------------------------------------------------------- thread

class LockStepThread : public platform::Thread {
public:
    LockStepThread(LockStepScheduler& owner, std::shared_ptr<LockStepScheduler::ThreadRecord> record)
        : owner_(owner), record_(std::move(record)) {}

    void join() override
    {
        std::unique_lock<std::mutex> lock(owner_.gate_);
        owner_.cv_.wait_for(lock, std::chrono::milliseconds(owner_.hang_ms_), [&] { return !record_->alive.load(); });
    }
    bool join(int32_t timeoutMilliseconds) override
    {
        // Thread.Join(ms) is a real-time wait, as in the spike.
        std::unique_lock<std::mutex> lock(owner_.gate_);
        return owner_.cv_.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds < 0 ? owner_.hang_ms_ : timeoutMilliseconds),
            [&] { return !record_->alive.load(); });
    }
    bool is_alive() const override { return record_->alive.load(); }
    bool is_current() const override { return t_record == record_.get(); }
    void detach() override
    {
        std::lock_guard<std::mutex> lock(owner_.gate_);
        record_->detached = true;
    }

private:
    LockStepScheduler& owner_;
    std::shared_ptr<LockStepScheduler::ThreadRecord> record_;
};

// ---------------------------------------------------------------- scheduler

LockStepScheduler::LockStepScheduler(int hangMilliseconds)
    : hang_ms_(hangMilliseconds), clock_(new LockStepClock(*this))
{
}

void LockStepScheduler::notify()
{
    cv_.notify_all();
}

void LockStepScheduler::wait_slice(std::unique_lock<std::mutex>& lock, int milliseconds)
{
    cv_.wait_for(lock, std::chrono::milliseconds(milliseconds));
}
LockStepScheduler::~LockStepScheduler()
{
    abort_and_join();
}

void LockStepScheduler::abort_and_join()
{
    abort();
    std::vector<std::shared_ptr<ThreadRecord>> threads;
    {
        std::lock_guard<std::mutex> lock(gate_);
        threads = threads_;
    }
    for (std::shared_ptr<ThreadRecord>& record : threads) {
        if (!record->thread.joinable()) continue;
        // The last owner may be an engine core released on its own worker
        // thread: that thread cannot join itself.
        if (t_record == record.get()) record->thread.detach();
        else record->thread.join();
    }
}

void LockStepScheduler::abort()
{
    {
        std::lock_guard<std::mutex> lock(gate_);
        aborted_ = true;
    }
    notify();
}

platform::Clock& LockStepScheduler::clock()
{
    return *clock_;
}

std::unique_ptr<platform::WakeEvent> LockStepScheduler::create_wake_event(platform::WakeEventRole, bool initiallySet,
    bool autoReset)
{
    return std::unique_ptr<platform::WakeEvent>(new LockStepEvent(*this, initiallySet, autoReset));
}

std::unique_ptr<platform::Waiter> LockStepScheduler::create_waiter(platform::WakeEvent& wake, platform::PlatformError& error)
{
    if (!register_worker(error)) return nullptr;
    return std::unique_ptr<platform::Waiter>(new LockStepWaiter(*this, static_cast<LockStepEvent&>(wake)));
}

std::unique_ptr<platform::Thread> LockStepScheduler::start_thread(std::function<void()> body, platform::ThreadPriority,
    const char*, platform::PlatformError&)
{
    std::shared_ptr<ThreadRecord> record = std::make_shared<ThreadRecord>();
    {
        std::lock_guard<std::mutex> lock(gate_);
        threads_.push_back(record);
    }
    record->thread = std::thread([this, record, body]() mutable {
        t_record = record.get();
        try { body(); }
        catch (...) {}
        // The body owns the engine core: destroy it on this thread, then end.
        body = std::function<void()>();
        {
            std::lock_guard<std::mutex> lock(gate_);
            record->alive.store(false);
        }
        notify();
    });
    return std::unique_ptr<platform::Thread>(new LockStepThread(*this, record));
}

bool LockStepScheduler::is_worker_thread() const
{
    void* current = t_record;
    return current != nullptr && current == static_cast<void*>(worker_.load());
}

void LockStepScheduler::trace_line(const char* kind, int64_t t, bool hasUntil, int64_t until, bool untilNone)
{
    trace_ += kind;
    trace_ += " t=";
    append_number(trace_, t);
    if (hasUntil) {
        trace_ += " until=";
        if (untilNone) trace_ += "none";
        else append_number(trace_, until);
    }
    trace_.push_back('\n');
}

void LockStepScheduler::raise_timeout(const std::string& text)
{
    {
        std::lock_guard<std::mutex> lock(gate_);
        protocol_error = text;
    }
    throw timeout_error(text);
}

void LockStepScheduler::raise_hang_locked(const std::string& text)
{
    parked_ = false;
    protocol_error = text;
    throw timeout_error(text);
}

// ---- worker side ---------------------------------------------------------

// Start gate and worker registration (the HighResolutionWaiter constructor in
// the spike). A harness command that creates a worker and then keeps running
// engine code must not overlap that worker: while a command is active a newly
// started worker waits here, before it has done anything.
bool LockStepScheduler::register_worker(platform::PlatformError& error)
{
    RealClock::time_point started = RealClock::now();
    std::unique_lock<std::mutex> lock(gate_);
    while (command_active_ && !aborted_) {
        if (RealClock::now() - started > std::chrono::milliseconds(hang_ms_)) {
            protocol_error = "new worker held longer than " + std::to_string(hang_ms_) + " ms while a command was active";
            error.type = "TimeoutException";
            error.message = "LockStepScheduler: " + protocol_error;
            return false;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(5));
    }
    if (aborted_) {
        error.type = "TimeoutException";
        error.message = "LockStepScheduler: aborted";
        return false;
    }
    worker_ = static_cast<ThreadRecord*>(t_record);
    worker_generation_++;
    trace_ += "WORKER-START gen=";
    append_number(trace_, worker_generation_);
    trace_ += " t=";
    append_number(trace_, now());
    trace_.push_back('\n');
    notify();
    return true;
}

void LockStepScheduler::begin_command()
{
    std::lock_guard<std::mutex> lock(gate_);
    command_active_ = true;
}

void LockStepScheduler::end_command()
{
    {
        std::lock_guard<std::mutex> lock(gate_);
        command_active_ = false;
    }
    notify();
}

// Clock-polling livelock guard (spike: LockStepScheduler.ClockRead). A worker
// that reads the virtual clock over and over while the virtual time cannot
// move is failed with a TimeoutException after MaximumIdleReads reads at one
// virtual time, instead of hanging.
void LockStepScheduler::clock_read(int64_t now)
{
    void* current = t_record;
    if (current == nullptr || current != static_cast<void*>(worker_.load())) return;
    if (now != idle_read_time_) { idle_read_time_ = now; idle_reads_ = 0; return; }
    if (++idle_reads_ < kMaximumIdleReads) return;
    idle_reads_ = 0;
    raise_timeout("worker polled the clock " + std::to_string(kMaximumIdleReads) + " times at virtual time " +
        std::to_string(now) + " (busy-poll livelock)");
}

void LockStepScheduler::worker_waiter_disposed()
{
    std::lock_guard<std::mutex> lock(gate_);
    trace_line("WORKER-WAITER-DISPOSE", now(), false, 0, false);
}

// Second rendezvous kind: after each completed output send, if a scripted
// command is due, the worker yields with deadline = now so the harness can
// apply the command "during" the worker's dispatch run. The yield is released
// by the engine's wake generation (SignalWake increments it before Set).
// Yields only while the engine is Playing: Stop/Pause/Seek set Stopped before
// SignalWake and then Join, so a worker draining after them must not block.
void LockStepScheduler::send_boundary()
{
    if (!is_worker_thread()) return;
    if (now() < next_command_time.load()) return;
    if (!engine_wake_generation || !engine_playing || !engine_playing()) return;
    send_yields++;
    park(nullptr, 0, "YIELD");
}

// Returns true when the wait ran its course (v1 WaitMicroseconds returning true).
bool LockStepScheduler::park(LockStepEvent* wake, int64_t microseconds, const char* kind)
{
    int outcome = park_core(wake, microseconds, kind);
    if (outcome == 1) return false;
    return wake == nullptr || !wake->poll();
}

// Worker side of the shadow EventWaitHandle: v1's blocking wait on an event.
// Untimed (milliseconds < 0) is a park with no deadline that only Set
// releases. Returns true when the event was signalled.
bool LockStepScheduler::wait_from_worker(LockStepEvent& handle, int32_t milliseconds)
{
    if (handle.poll()) return true;
    bool untimed = milliseconds < 0;
    int64_t micro = untimed ? INT64_MAX : static_cast<int64_t>(milliseconds) * 1000;
    event_waits++;
    return park_core(&handle, micro, untimed ? "WAIT" : "WAITT") == 1;
}

// Returns 1 when released by the wake handle, 0 when released by the harness
// (the go signal) at the deadline.
int LockStepScheduler::park_core(LockStepEvent* wake, int64_t microseconds, const char* kind)
{
    int64_t now = this->now();
    idle_read_time_ = -1;
    int64_t deadline = now > INT64_MAX - microseconds ? INT64_MAX : now + microseconds;
    int32_t generation = engine_wake_generation ? engine_wake_generation() : 0;
    std::unique_lock<std::mutex> lock(gate_);
    parked_ = true;
    park_deadline_ = deadline;
    park_generation_ = generation;
    park_seq_++;
    parks++;
    trace_line(kind, now, true, deadline, microseconds == INT64_MAX);
    notify();

    bool woken = false;
    RealClock::time_point started = RealClock::now();
    for (;;) {
        if (go_) { go_ = false; woken = false; break; }           // WaitAny: _go first
        if (wake != nullptr) {
            if (wake->is_set_locked()) { wake->consume_locked(); woken = true; break; }
        }
        else if (engine_wake_generation() != generation) { woken = true; break; }
        if (aborted_) raise_hang_locked("aborted");
        if (RealClock::now() - started > std::chrono::milliseconds(hang_ms_))
            raise_hang_locked("worker parked longer than " + std::to_string(hang_ms_) + " ms real time");
        wait_slice(lock, 1);
    }
    parked_ = false;
    notify();
    if (woken) {
        wake_resumes++;
        trace_line("RESUME-WAKE", this->now(), false, 0, false);
        if (go_) { go_ = false; protocol_error = "go and wake were both pending"; }
        return 1;
    }
    go_resumes++;
    trace_line("RESUME-GO", this->now(), false, 0, false);
    return 0;
}

// ---- harness side --------------------------------------------------------

int LockStepScheduler::worker_generation()
{
    std::lock_guard<std::mutex> lock(gate_);
    return worker_generation_;
}

int64_t LockStepScheduler::park_seq()
{
    std::lock_guard<std::mutex> lock(gate_);
    return park_seq_;
}

void LockStepScheduler::wait_for_worker_generation(int generation)
{
    std::unique_lock<std::mutex> lock(gate_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(hang_ms_), [&] { return worker_generation_ >= generation || aborted_; })) {
        protocol_error = "worker never started";
        throw timeout_error("worker never started");
    }
}

bool LockStepScheduler::wait_quiescent(int64_t seenSeq, int64_t& deadline, int64_t& parkSeqOut)
{
    RealClock::time_point started = RealClock::now();
    std::unique_lock<std::mutex> lock(gate_);
    for (;;) {
        if (parked_ && park_seq_ > seenSeq) {
            deadline = park_deadline_;
            parkSeqOut = park_seq_;
            return true;
        }
        ThreadRecord* worker = worker_.load();
        if (worker == nullptr || !worker->alive.load()) break;
        if (aborted_) throw timeout_error("aborted");
        if (RealClock::now() - started > std::chrono::milliseconds(hang_ms_)) {
            protocol_error = "worker neither parked nor exited (hang)";
            throw timeout_error("worker neither parked nor exited (hang)");
        }
        wait_slice(lock, 2);
    }
    deadline = 0;
    parkSeqOut = park_seq_;
    return false;
}

// After a harness command ran while the worker was parked at parkSeq: did the
// command release the worker (SignalWake moved the wake generation) or has the
// worker left that park / exited?
bool LockStepScheduler::command_released_worker(int64_t parkSeqValue, int32_t wakeGenerationBefore)
{
    if (engine_wake_generation() != wakeGenerationBefore) return true;
    std::lock_guard<std::mutex> lock(gate_);
    return !(parked_ && park_seq_ == parkSeqValue);
}

// Control-thread side of the shadow EventWaitHandle: a wait with a timeout in
// virtual milliseconds (v1: _pauseAcknowledged.WaitOne in Pause, called from
// inside a harness command). The control thread does not block the schedule;
// it lends the token to the worker:
//  - the worker still leaving a park that the command's wake signal releases,
//    or running toward its next park: wait for it (real time only);
//  - the worker parked for good with the handle still clear: if that park's
//    deadline is within the control deadline, advance virtual time to it and
//    release the worker; otherwise virtual time moves to the control deadline
//    and the wait times out;
//  - the handle set, at any point: return true.
bool LockStepScheduler::wait_from_control(LockStepEvent& handle, int32_t milliseconds)
{
    int64_t controlDeadline = milliseconds < 0 ? INT64_MAX : saturating_add(now(), static_cast<int64_t>(milliseconds) * 1000);
    RealClock::time_point real = RealClock::now();
    control_waits++;
    int64_t releasedSeq = -1;
    for (;;) {
        bool parked, alive;
        int64_t deadline, seq;
        int32_t generation;
        {
            std::lock_guard<std::mutex> lock(gate_);
            parked = parked_;
            deadline = park_deadline_;
            seq = park_seq_;
            generation = park_generation_;
            ThreadRecord* w = worker_.load();
            alive = w != nullptr && w->alive.load();
        }
        bool stuck = parked && seq != releasedSeq &&
            (seq > command_seen_seq.load() || engine_wake_generation() == generation);
        if (stuck) {
            if (handle.poll()) return true;
            if (controlDeadline == INT64_MAX && deadline == INT64_MAX)
                throw engine::EngineError::make("InvalidOperationException",
                    "LockStepScheduler: control wait and worker park both have no deadline");
            if (deadline != INT64_MAX && deadline <= controlDeadline) {
                note("CONTROL-WAIT-RELEASE t=" + std::to_string(now()) + " until=" + std::to_string(deadline));
                command_seen_seq.store(seq);
                releasedSeq = seq;
                release_to_deadline(deadline);
                real = RealClock::now();
                continue;
            }
            return control_timeout(controlDeadline);
        }
        if (!parked && !alive) {
            if (handle.poll()) return true;
            return control_timeout(controlDeadline);
        }
        if (handle.poll()) return true;
        if (aborted_) throw timeout_error("aborted");
        if (RealClock::now() - real > std::chrono::milliseconds(hang_ms_)) {
            raise_timeout("control wait: worker neither signalled nor parked within " + std::to_string(hang_ms_) +
                " ms real time");
        }
        std::unique_lock<std::mutex> lock(gate_);
        wait_slice(lock, 1);
    }
}

bool LockStepScheduler::control_timeout(int64_t controlDeadline)
{
    if (controlDeadline != INT64_MAX && now() < controlDeadline) set_time(controlDeadline);
    control_timeouts++;
    note("CONTROL-WAIT-TIMEOUT t=" + std::to_string(now()));
    return false;
}

void LockStepScheduler::release_to_deadline(int64_t deadline)
{
    if (now() < deadline) set_time(deadline);
    {
        std::lock_guard<std::mutex> lock(gate_);
        go_ = true;
    }
    notify();
}

void LockStepScheduler::note(const std::string& line)
{
    std::lock_guard<std::mutex> lock(gate_);
    trace_ += line;
    trace_.push_back('\n');
}

} // namespace harness
} // namespace midibn
