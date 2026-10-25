#define WIN32_LEAN_AND_MEAN
#include "win32_timing.h"

#include "midibn/user_messages.h"

#include <windows.h>
#include <mmsystem.h>
#include <process.h>

#include <climits>
#include <stdexcept>
#include <vector>

namespace midibn {
namespace win32 {

namespace {

const DWORD kCreateWaitableTimerHighResolution = 0x00000002;
const DWORD kTimerAllAccess = 0x1F0003;

typedef HANDLE(WINAPI* CreateWaitableTimerExWFn)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
typedef HRESULT(WINAPI* SetThreadDescriptionFn)(HANDLE, PCWSTR);

volatile LONG g_policy_normal_threads = 0;
volatile LONG g_policy_yield_ms = 0;
volatile LONG g_policy_nap = 0;
volatile bool g_force_one_core = false;

uint32_t usable_processors()
{
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || processMask == 0) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
    }
    uint32_t count = 0;
    for (DWORD_PTR m = processMask; m != 0; m &= m - 1) ++count;
    return count ? count : 1;
}

} // namespace

void set_few_cpu_policy(bool normalThreads, int yieldMilliseconds)
{
    InterlockedExchange(&g_policy_normal_threads, normalThreads ? 1 : 0);
    InterlockedExchange(&g_policy_yield_ms, yieldMilliseconds < 0 ? 0 : yieldMilliseconds);
}

uint32_t count_physical_cores(const void* processorInformation, size_t bytes)
{
    const SYSTEM_LOGICAL_PROCESSOR_INFORMATION* entries = static_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(processorInformation);
    size_t count = bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    uint32_t cores = 0;
    for (size_t i = 0; entries && i < count; ++i)
        if (entries[i].Relationship == RelationProcessorCore) ++cores;
    return cores;
}

bool few_cpu_yield_option_offered(uint32_t physicalCores, uint32_t logicalProcessors)
{
    return physicalCores != 0 ? physicalCores == 1 : logicalProcessors == 1;
}

uint32_t system_logical_processor_count()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
}

uint32_t physical_core_count()
{
    if (g_force_one_core) return 1;
    typedef BOOL(WINAPI* GetLogicalProcessorInformationFn)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    GetLogicalProcessorInformationFn fn = kernel ? reinterpret_cast<GetLogicalProcessorInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "GetLogicalProcessorInformation"))) : nullptr;
    if (!fn) return 0;
    DWORD bytes = 0;
    fn(nullptr, &bytes);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 0;
    std::vector<unsigned char> buffer(bytes);
    if (!fn(reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buffer.data()), &bytes)) return 0;
    return count_physical_cores(buffer.data(), bytes);
}

void set_force_one_physical_core(bool force) { g_force_one_core = force; }

bool few_cpu_yield_option_available() { return few_cpu_yield_option_offered(physical_core_count(), system_logical_processor_count()); }

bool few_cpu_normal_threads_active() { return g_policy_normal_threads != 0 && usable_processors() <= 2; }

int few_cpu_yield_milliseconds_active() { return g_policy_yield_ms > 0 && usable_processors() <= 2 ? static_cast<int>(g_policy_yield_ms) : 0; }

// SwitchToThread only hands the processor to a thread of the same or higher priority (measured: it does not help a Normal
// thread, P10-THR); a 1 ms nap (Sleep(1)) lets every ready thread run. Nap mode is the option "nap=MS".
void set_few_cpu_nap(bool nap) { InterlockedExchange(&g_policy_nap, nap ? 1 : 0); }
void Win32EngineServices::yield_to_others() { if (g_policy_nap) Sleep(1); else SwitchToThread(); }

QpcClock::QpcClock()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    frequency_ = f.QuadPart;
}

int64_t QpcClock::now_ticks() const
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

Win32WakeEvent::Win32WakeEvent(bool initiallySet, bool autoReset)
    : handle_(CreateEventW(nullptr, autoReset ? FALSE : TRUE, initiallySet ? TRUE : FALSE, nullptr))
{
    if (handle_ == nullptr) throw std::runtime_error("Unable to create a wake event.");
}

Win32WakeEvent::~Win32WakeEvent() { CloseHandle(handle_); }
void Win32WakeEvent::set() { SetEvent(handle_); }
void Win32WakeEvent::reset() { ResetEvent(handle_); }

bool Win32WakeEvent::wait(int32_t timeoutMilliseconds)
{
    DWORD timeout = timeoutMilliseconds < 0 ? INFINITE : static_cast<DWORD>(timeoutMilliseconds);
    return WaitForSingleObject(handle_, timeout) == WAIT_OBJECT_0;
}

std::unique_ptr<Win32Waiter> Win32Waiter::create(Win32WakeEvent& wake, std::string& error)
{
    HANDLE timer = nullptr;
    bool highResolution = false;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    CreateWaitableTimerExWFn createEx = kernel
        ? reinterpret_cast<CreateWaitableTimerExWFn>(GetProcAddress(kernel, "CreateWaitableTimerExW")) : nullptr;
    if (createEx != nullptr) {
        timer = createEx(nullptr, nullptr, kCreateWaitableTimerHighResolution, kTimerAllAccess);
        highResolution = timer != nullptr;
    }
    if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (timer == nullptr) {
        error = "Unable to create the playback timer.";
        return nullptr;
    }
    return std::unique_ptr<Win32Waiter>(new Win32Waiter(wake, timer, highResolution));
}

namespace {

// .NET Framework Environment.ProcessorCount (as PlatformHelper.IsSingleProcessor
// reads it): the processors this process may run on.
bool single_processor()
{
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0) {
        int count = 0;
        for (DWORD_PTR m = processMask; m != 0; m &= m - 1) ++count;
        return count == 1;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors == 1;
}

} // namespace

Win32Waiter::Win32Waiter(Win32WakeEvent& wake, void* timer, bool highResolution)
    : wake_(wake), timer_(timer), high_resolution_(highResolution), single_processor_(single_processor())
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = frequency.QuadPart;
#ifdef MIDIBN_XP_BUILD
    period_held_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
}

Win32Waiter::~Win32Waiter()
{
    CancelWaitableTimer(timer_);
    CloseHandle(timer_);
    if (period_held_) timeEndPeriod(1);
}

platform::WaitResult Win32Waiter::wait(int64_t microseconds, platform::PlatformError& error)
{
    if (microseconds <= 0) return platform::WaitResult::Elapsed;
    if (wake_.poll()) return platform::WaitResult::Woken;
    // Let the kernel timer handle the coarse portion, then yield/spin only
    // for the final 200 microseconds (v1 HighResolutionWaiter).
    int64_t coarse = microseconds - 200;
    if (coarse > 0) {
        if (coarse > INT64_MAX / 10) {
            error.type = "OverflowException";
            error.message = messages::wait_too_long();
            return platform::WaitResult::Failed;
        }
        LARGE_INTEGER due;
        due.QuadPart = -(coarse * 10);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            error.type = "Win32Exception";
            error.message = "Unable to arm the playback timer.";
            return platform::WaitResult::Failed;
        }
        HANDLE handles[2] = { timer_, wake_.native_handle() };
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_FAILED) {
            error.type = "Win32Exception";
            error.message = "The playback timer wait failed.";
            return platform::WaitResult::Failed;
        }
        if (wait == WAIT_OBJECT_0 + 1) return platform::WaitResult::Woken;
    }
    return wake_.poll() ? platform::WaitResult::Woken : platform::WaitResult::Elapsed;
}

void Win32Waiter::begin_spin()
{
    spins_ = 0;
    yielding_ = single_processor_;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    spin_started_ = now.QuadPart;
}

// The final approach to a dispatch time: the last ~200 microseconds that the
// timer does not cover. v1 handed this to .NET's SpinWait, whose busy spins
// last only about 40 microseconds; after that it yields and, on every 20th
// yield, sleeps one timer period (up to 15.6 ms), so v1 sent notes late by as
// much as a timer tick (measured: median 6.5 ms and 99th percentile 14.9 ms on
// a dense song). v2 keeps the processor and the caller re-reads the clock
// after every spin, so the dispatch happens within a few microseconds of its
// time. The CPU cost stays bounded: the timer covers all but the last 200
// microseconds, and if the approach ever lasts longer than 1 ms (a late
// timer, an unusual target) or the machine has a single processor, the
// thread gives the rest of its time slice away on each spin instead.
void Win32Waiter::spin_once()
{
    if (!yielding_) {
        YieldProcessor();
        // Look at the clock only on every 64th spin.
        if ((++spins_ & 63) == 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart - spin_started_ > qpc_frequency_ / 1000) yielding_ = true;
        }
        return;
    }
    SwitchToThread();
}

std::unique_ptr<Win32Thread> Win32Thread::start(std::function<void()> body, platform::ThreadPriority priority,
    const wchar_t* name, std::string& error)
{
    std::unique_ptr<Win32Thread> thread(new Win32Thread());
    // The body is owned by the thread itself and destroyed on it, so a
    // detached (abandoned) thread never depends on this object.
    std::function<void()>* owned = new std::function<void()>(std::move(body));
    unsigned id = 0;
    uintptr_t handle = _beginthreadex(nullptr, 0, &Win32Thread::entry, owned, CREATE_SUSPENDED, &id);
    if (handle == 0) {
        delete owned;
        error = "Unable to start the playback thread.";
        return nullptr;
    }
    thread->handle_ = reinterpret_cast<void*>(handle);
    thread->thread_id_ = id;
    if (priority == platform::ThreadPriority::AboveNormal && !few_cpu_normal_threads_active())
        SetThreadPriority(thread->handle_, THREAD_PRIORITY_ABOVE_NORMAL);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    SetThreadDescriptionFn describe = kernel
        ? reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel, "SetThreadDescription")) : nullptr;
    if (describe != nullptr && name != nullptr) describe(thread->handle_, name);
    ResumeThread(thread->handle_);
    return thread;
}

unsigned __stdcall Win32Thread::entry(void* body)
{
    std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(body));
    (*owned)();
    return 0;
}

void Win32Thread::detach()
{
    if (handle_ == nullptr) return;
    CloseHandle(handle_);
    handle_ = nullptr;
}

void Win32Thread::join()
{
    if (handle_ == nullptr) return;
    WaitForSingleObject(handle_, INFINITE);
    CloseHandle(handle_);
    handle_ = nullptr;
}

bool Win32Thread::join(int32_t timeoutMilliseconds)
{
    if (handle_ == nullptr) return true;
    DWORD timeout = timeoutMilliseconds < 0 ? INFINITE : static_cast<DWORD>(timeoutMilliseconds);
    if (WaitForSingleObject(handle_, timeout) != WAIT_OBJECT_0) return false;
    CloseHandle(handle_);
    handle_ = nullptr;
    return true;
}

bool Win32Thread::is_alive() const
{
    return handle_ != nullptr && WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
}

bool Win32Thread::is_current() const
{
    return thread_id_ != 0 && GetCurrentThreadId() == thread_id_;
}

Win32Thread::~Win32Thread() { join(); }

// ---------------------------------------------------------------- engine services

std::unique_ptr<platform::WakeEvent> Win32EngineServices::create_wake_event(platform::WakeEventRole, bool initiallySet,
    bool autoReset)
{
    return std::unique_ptr<platform::WakeEvent>(new Win32WakeEvent(initiallySet, autoReset));
}

std::unique_ptr<platform::Waiter> Win32EngineServices::create_waiter(platform::WakeEvent& wake,
    platform::PlatformError& error)
{
    std::string text;
    std::unique_ptr<Win32Waiter> waiter = Win32Waiter::create(static_cast<Win32WakeEvent&>(wake), text);
    if (!waiter) {
        error.type = "Win32Exception";
        error.message = text;
        return nullptr;
    }
    return std::unique_ptr<platform::Waiter>(waiter.release());
}

std::unique_ptr<platform::Thread> Win32EngineServices::start_thread(std::function<void()> body,
    platform::ThreadPriority priority, const char* name, platform::PlatformError& error)
{
    std::wstring wide;
    for (const char* p = name; p != nullptr && *p != 0; ++p) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    std::string text;
    std::unique_ptr<Win32Thread> thread = Win32Thread::start(std::move(body), priority, wide.c_str(), text);
    if (!thread) {
        // CreateThread failed (v1 showed .NET's OutOfMemoryException text here; register entry DD-003).
        error.type = "OutOfMemoryException";
        error.message = messages::playback_thread_not_started();
        return nullptr;
    }
    return std::unique_ptr<platform::Thread>(thread.release());
}

} // namespace win32
} // namespace midibn
