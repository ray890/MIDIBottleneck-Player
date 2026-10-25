#include "linux_platform.h"

#include "midibn/user_messages.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>

#include <fcntl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

namespace midibn {
namespace posix {
namespace {

class PosixFile : public platform::ByteSource {
public:
    PosixFile(int descriptor, int64_t size) : descriptor_(descriptor), size_(size) {}
    ~PosixFile() override { ::close(descriptor_); }
    int64_t size() const override { return size_; }
    size_t read_at(int64_t offset, uint8_t* buffer, size_t count) override
    {
        if (offset >= size_ || count == 0) return 0;
        size_t total = 0;
        while (total < count) {
            size_t chunk = count - total > 0x40000000u ? 0x40000000u : count - total;
            ssize_t got = ::pread(descriptor_, buffer + total, chunk, static_cast<off_t>(offset + static_cast<int64_t>(total)));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            total += static_cast<size_t>(got);
        }
        return total;
    }

private:
    int descriptor_;
    int64_t size_;
};

uint64_t status_bytes(const char* key)
{
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) return 0;
    uint64_t value = 0;
    char line[256];
    size_t keyLength = std::strlen(key);
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, keyLength) == 0 && line[keyLength] == ':') {
            value = std::strtoull(line + keyLength + 1, nullptr, 10) * 1024u;   // the file reports kB
            break;
        }
    }
    std::fclose(file);
    return value;
}

int64_t monotonic_ns()
{
    struct timespec now;
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

void absolute_timespec(int64_t ns, struct timespec& out)
{
    out.tv_sec = static_cast<time_t>(ns / 1000000000);
    out.tv_nsec = static_cast<long>(ns % 1000000000);
}

} // namespace

std::unique_ptr<platform::ByteSource> PosixFileSystem::open_read(const std::string& path_utf8, std::string& error)
{
    int descriptor = ::open(path_utf8.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0 && errno == ENOENT && path_utf8.find('\\') != std::string::npos) {
        // The tests and tools build paths with Windows-style "\"; a path that does not exist as written is tried
        // once more with every "\" read as "/". (The player's own paths never contain a backslash.)
        std::string native = path_utf8;
        std::replace(native.begin(), native.end(), '\\', '/');
        descriptor = ::open(native.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (descriptor < 0) {
        error = "Could not open " + path_utf8 + " (system error " + std::to_string(errno) + ").";
        return nullptr;
    }
    struct stat info;
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        error = "Could not read the size of " + path_utf8 + ".";
        ::close(descriptor);
        return nullptr;
    }
    ::posix_fadvise(descriptor, 0, 0, POSIX_FADV_SEQUENTIAL);   // the loader reads front to back
    return std::unique_ptr<platform::ByteSource>(new PosixFile(descriptor, static_cast<int64_t>(info.st_size)));
}

ProcessMemory process_memory()
{
    ProcessMemory result;
    result.peak_working_set = status_bytes("VmHWM");
    result.peak_private = status_bytes("VmPeak");
    result.current_private = status_bytes("VmSize");
    return result;
}

std::string describe_os()
{
    struct utsname name;
    std::string text = "Linux";
    if (::uname(&name) == 0) text = std::string(name.sysname) + " " + name.release;
    return text + (sizeof(void*) == 8 ? " (x64 process)" : " (x86 process)");
}

int64_t performance_counter() { return monotonic_ns(); }

double milliseconds_since(int64_t start_counter)
{
    return static_cast<double>(performance_counter() - start_counter) / 1000000.0;
}

// ---------------------------------------------------------------- wake event

PosixWakeEvent::PosixWakeEvent(bool initiallySet, bool autoReset) : signalled_(initiallySet), auto_reset_(autoReset)
{
    ::pthread_mutex_init(&mutex_, nullptr);
    pthread_condattr_t attributes;
    ::pthread_condattr_init(&attributes);
    ::pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);   // absolute deadlines are CLOCK_MONOTONIC times
    ::pthread_cond_init(&condition_, &attributes);
    ::pthread_condattr_destroy(&attributes);
}

PosixWakeEvent::~PosixWakeEvent()
{
    ::pthread_cond_destroy(&condition_);
    ::pthread_mutex_destroy(&mutex_);
}

void PosixWakeEvent::set()
{
    ::pthread_mutex_lock(&mutex_);
    signalled_ = true;
    ::pthread_cond_broadcast(&condition_);
    ::pthread_mutex_unlock(&mutex_);
}

void PosixWakeEvent::reset()
{
    ::pthread_mutex_lock(&mutex_);
    signalled_ = false;
    ::pthread_mutex_unlock(&mutex_);
}

bool PosixWakeEvent::wait(int32_t timeoutMilliseconds)
{
    if (timeoutMilliseconds > 0) return wait_until(monotonic_ns() + static_cast<int64_t>(timeoutMilliseconds) * 1000000);
    bool got;
    ::pthread_mutex_lock(&mutex_);
    if (timeoutMilliseconds < 0) while (!signalled_) ::pthread_cond_wait(&condition_, &mutex_);
    got = signalled_;
    if (got && auto_reset_) signalled_ = false;
    ::pthread_mutex_unlock(&mutex_);
    return got;
}

bool PosixWakeEvent::wait_until(int64_t deadline_ns)
{
    struct timespec deadline;
    absolute_timespec(deadline_ns, deadline);
    ::pthread_mutex_lock(&mutex_);
    while (!signalled_) {
        if (::pthread_cond_timedwait(&condition_, &mutex_, &deadline) == ETIMEDOUT) break;
    }
    bool got = signalled_;
    if (got && auto_reset_) signalled_ = false;
    ::pthread_mutex_unlock(&mutex_);
    return got;
}

// ---------------------------------------------------------------- waiter

platform::WaitResult PosixWaiter::wait(int64_t microseconds, platform::PlatformError& error)
{
    if (microseconds <= 0) return platform::WaitResult::Elapsed;
    if (wake_.poll()) return platform::WaitResult::Woken;
    // The kernel handles the coarse portion; the caller spins the final 200 microseconds (v1 HighResolutionWaiter).
    int64_t coarse = microseconds - 200;
    if (coarse > 0) {
        if (coarse > INT64_MAX / 10) {   // the Win32 waiter's checked(coarse * 10), kept so both report the same
            error.type = "OverflowException";
            error.message = messages::wait_too_long();
            return platform::WaitResult::Failed;
        }
        // An absolute CLOCK_MONOTONIC deadline: a late wake-up cannot make the wait drift. Times beyond what
        // clock_nanosleep can hold (about 292 years) are not representable and were rejected above.
        int64_t deadline = monotonic_ns() + coarse * 1000;
        if (wake_.wait_until(deadline)) return platform::WaitResult::Woken;
    }
    return wake_.poll() ? platform::WaitResult::Woken : platform::WaitResult::Elapsed;
}

// The final approach (last ~200 microseconds): keep the processor and let the caller re-read the clock after every
// spin; a pause instruction keeps the sibling hyper-thread usable.
void PosixWaiter::spin_once()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    ::sched_yield();
#endif
}

// ---------------------------------------------------------------- thread

namespace {
thread_local const void* g_current_thread_state = nullptr;
}

struct PosixThread::State {
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool finished = false;
    PriorityOutcome outcome = PriorityOutcome::NotRequested;
    int nice = 100;
};

std::unique_ptr<PosixThread> PosixThread::start(std::function<void()>&& body, platform::ThreadPriority priority,
    const char* name, std::string& error)
{
    std::unique_ptr<PosixThread> result(new PosixThread());
    result->state_ = std::make_shared<State>();
    // The body is moved into the thread and destroyed on it, so an abandoned thread never depends on this object.
    struct Argument {
        std::shared_ptr<State> state;
        std::function<void()> body;
        platform::ThreadPriority priority;
        std::string name;
    };
    Argument* argument = nullptr;
    try {
        argument = new Argument{ result->state_, std::move(body), priority, name != nullptr ? name : "" };
        body = nullptr;   // see the header: no capture may stay behind in the caller thread
    } catch (...) {
        error = "Unable to start the playback thread.";
        return nullptr;
    }
    auto entry = [](void* raw) -> void* {
        std::unique_ptr<Argument> mine(static_cast<Argument*>(raw));
        std::shared_ptr<State> state = mine->state;
        g_current_thread_state = state.get();
        if (!mine->name.empty()) {
            char shortName[16];
            std::strncpy(shortName, mine->name.c_str(), 15);
            shortName[15] = '\0';
            ::pthread_setname_np(::pthread_self(), shortName);
        }
        // Wake-ups of this thread should be as close to the deadline as the kernel can make them: the default timer
        // slack of 50 microseconds would be added to every timed wait. Per thread, needs no privilege.
        ::prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
        PriorityOutcome outcome = PriorityOutcome::NotRequested;
        if (mine->priority == platform::ThreadPriority::AboveNormal) {
            // A modest raise (never real-time scheduling). Unprivileged processes are refused; stay at Normal then.
            outcome = ::setpriority(PRIO_PROCESS, 0, kAboveNormalNice) == 0 ? PriorityOutcome::Raised : PriorityOutcome::Refused;
        }
        errno = 0;
        int nice = ::getpriority(PRIO_PROCESS, 0);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->outcome = outcome;
            state->nice = errno == 0 ? nice : 100;
            state->started = true;
        }
        state->condition.notify_all();
        std::function<void()> work = std::move(mine->body);
        mine.reset();
        work();
        work = nullptr;   // destroyed on this thread
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->finished = true;
        }
        state->condition.notify_all();
        return nullptr;
    };
    if (::pthread_create(&result->thread_, nullptr, entry, argument) != 0) {
        delete argument;
        error = "Unable to start the playback thread.";
        return nullptr;
    }
    result->started_ = true;
    std::unique_lock<std::mutex> lock(result->state_->mutex);
    result->state_->condition.wait(lock, [&] { return result->state_->started; });
    return result;
}

PosixThread::~PosixThread() { join(); }

void PosixThread::join()
{
    if (!started_) return;
    ::pthread_join(thread_, nullptr);
    started_ = false;
}

bool PosixThread::join(int32_t timeoutMilliseconds)
{
    if (!started_) return true;
    if (timeoutMilliseconds >= 0) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->condition.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds),
                [this] { return state_->finished; })) return false;
    }
    ::pthread_join(thread_, nullptr);
    started_ = false;
    return true;
}

bool PosixThread::is_alive() const
{
    if (!started_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return !state_->finished;
}

// Stays true on the thread itself after detach(), as the Win32 thread does (the engine asks from the abandoned worker):
// the answer comes from the state the thread owns, not from this object's thread handle.
bool PosixThread::is_current() const { return state_ != nullptr && g_current_thread_state == state_.get(); }

void PosixThread::detach()
{
    if (!started_) return;
    ::pthread_detach(thread_);
    started_ = false;
}

PriorityOutcome PosixThread::priority_outcome() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->outcome;
}

int PosixThread::nice_value() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->nice;
}

// ---------------------------------------------------------------- engine services

std::unique_ptr<platform::WakeEvent> PosixEngineServices::create_wake_event(platform::WakeEventRole, bool initiallySet,
    bool autoReset)
{
    return std::unique_ptr<platform::WakeEvent>(new PosixWakeEvent(initiallySet, autoReset));
}

std::unique_ptr<platform::Waiter> PosixEngineServices::create_waiter(platform::WakeEvent& wake, platform::PlatformError&)
{
    return std::unique_ptr<platform::Waiter>(new PosixWaiter(static_cast<PosixWakeEvent&>(wake)));
}

std::unique_ptr<platform::Thread> PosixEngineServices::start_thread(std::function<void()> body,
    platform::ThreadPriority priority, const char* name, platform::PlatformError& error)
{
    std::string text;
    std::unique_ptr<PosixThread> thread = PosixThread::start(std::move(body), priority, name, text);
    if (!thread) {
        error.type = "OutOfMemoryException";
        error.message = messages::playback_thread_not_started();
        return nullptr;
    }
    return std::unique_ptr<platform::Thread>(thread.release());
}

void PosixEngineServices::yield_to_others() { ::sched_yield(); }

} // namespace posix
} // namespace midibn
