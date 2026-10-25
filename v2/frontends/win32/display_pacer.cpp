#define WIN32_LEAN_AND_MEAN
#include "display_pacer.h"
#include "win32_timing.h"

#include <mmsystem.h>
#include <process.h>

#include <vector>

namespace midibn {
namespace ui {

namespace {

typedef HRESULT(WINAPI* DwmFlushFn)();
typedef HRESULT(WINAPI* DwmIsCompositionEnabledFn)(BOOL*);
typedef HANDLE(WINAPI* CreateWaitableTimerExWFn)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

const DWORD kHighResolutionTimer = 0x00000002;
const DWORD kTimerAllAccess = 0x1F0003;
const DWORD kSearchSystem32 = 0x00000800;   // LOAD_LIBRARY_SEARCH_SYSTEM32

// dwmapi.dll from the system folder only (never the application folder). Windows XP has none: returns null.
HMODULE load_dwmapi()
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel && GetProcAddress(kernel, "AddDllDirectory"))   // the LOAD_LIBRARY_SEARCH_* flags exist
        return LoadLibraryExW(L"dwmapi.dll", nullptr, kSearchSystem32);
    wchar_t folder[MAX_PATH];
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH - 16) return nullptr;
    std::wstring path = std::wstring(folder, length) + L"\\dwmapi.dll";
    return LoadLibraryW(path.c_str());
}

int64_t qpc_frequency()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

int64_t qpc_now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

} // namespace

struct DisplayPacer::Impl {
    struct Subscriber {
        int token;
        HWND window;
        volatile LONG pending;
    };
    // One pacing thread run. Owned by the thread itself (deleted when the thread ends); the pacer only signals it.
    struct Run {
        HANDLE stop_event;
        volatile LONG stop;
        DisplayPacer::Impl* owner;
    };

    CRITICAL_SECTION lock;
    std::vector<Subscriber*> subscribers;
    int next_token = 1;
    Run* run = nullptr;
    PaceMode mode = PaceMode::Auto;
    int max_hz = 0;
    int timer_hz = 60;
    volatile LONG posted = 0;     // 32-bit counters are enough for a diagnostic
    volatile LONG coalesced = 0;
    volatile LONG rate_limit = 0;   // adaptive display rate: extra cap in updates per second (0 = none)
    volatile LONG native_rate = 0;  // measured/assumed rate of the source
    char state[96] = "idle";
    UINT message = 0;

    Impl() { InitializeCriticalSection(&lock); }

    void set_state(const char* text)
    {
        EnterCriticalSection(&lock);
        lstrcpynA(state, text, sizeof(state));
        LeaveCriticalSection(&lock);
    }

    // One pacing tick: post the message to every subscriber that has finished the previous one.
    void tick()
    {
        EnterCriticalSection(&lock);
        for (size_t i = 0; i < subscribers.size(); ++i) {
            Subscriber* s = subscribers[i];
            if (InterlockedCompareExchange(&s->pending, 1, 0) == 0) {
                if (PostMessageW(s->window, message, static_cast<WPARAM>(s->token), 0)) InterlockedIncrement(&posted);
                else InterlockedExchange(&s->pending, 0);   // window gone
            }
            else InterlockedIncrement(&coalesced);
        }
        LeaveCriticalSection(&lock);
    }

    // ---- timer mode: absolute deadlines on the performance counter, no drift ------------------------
    void run_timer(Run* r, int hz)
    {
        if (hz < 1) hz = 60;
        HANDLE timer = nullptr;
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        CreateWaitableTimerExWFn createEx = kernel ? reinterpret_cast<CreateWaitableTimerExWFn>(GetProcAddress(kernel, "CreateWaitableTimerExW")) : nullptr;
        if (createEx) timer = createEx(nullptr, nullptr, kHighResolutionTimer, kTimerAllAccess);
        bool highResolution = timer != nullptr;
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!timer) return;
        // Without a high-resolution timer the wake-up granularity is the system tick; ask for 1 ms only while we run.
        bool period = !highResolution && timeBeginPeriod(1) == TIMERR_NOERROR;
        const int64_t frequency = qpc_frequency();
        int64_t base = qpc_now();
        int64_t k = 1;
        const int nativeHz = hz;
        InterlockedExchange(&native_rate, nativeHz);
        HANDLE handles[2] = { timer, r->stop_event };
        while (!r->stop) {
            // Adaptive display rate: a lower cap lengthens the period; re-base the deadlines when it changes.
            int wanted = rate_limit > 0 && rate_limit < nativeHz ? static_cast<int>(rate_limit) : nativeHz;
            if (wanted != hz) {
                hz = wanted;
                base = qpc_now();
                k = 1;
            }
            int64_t deadline = base + k * frequency / hz;
            int64_t now = qpc_now();
            if (now - deadline > frequency / hz) {              // stalled (sleep, debugger): skip, do not burst
                k = (now - base) * hz / frequency + 1;
                deadline = base + k * frequency / hz;
            }
            if (deadline > now) {
                LARGE_INTEGER due;
                due.QuadPart = -((deadline - now) * 10000000 / frequency);
                if (due.QuadPart == 0) due.QuadPart = -1;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) break;
            }
            if (r->stop) break;
            tick();
            ++k;
        }
        CancelWaitableTimer(timer);
        CloseHandle(timer);
        if (period) timeEndPeriod(1);
    }

    // ---- display mode: one tick per compositor pass ---------------------------------------------------
    // Returns the reason it gave up (null = it ended because it was told to stop).
    const char* run_display(Run* r, DwmFlushFn flush, DwmIsCompositionEnabledFn enabled)
    {
        BOOL composing = FALSE;
        if (enabled && FAILED(enabled(&composing))) composing = FALSE;
        if (!composing) return "desktop composition is off";
        const int64_t frequency = qpc_frequency();
        int64_t last = 0, due = 0, frame = 0;
        int fast = 0;
        for (;;) {
            HRESULT result = flush();
            if (r->stop) return nullptr;
            if (FAILED(result)) return "DwmFlush failed";
            int64_t now = qpc_now();
            if (last != 0) {
                // A flush that returns far faster than any monitor refreshes is not a vertical-blank signal.
                if (now - last < frequency / 600) {
                    if (++fast >= 5) return "DwmFlush returned too fast";
                    continue;
                }
                fast = 0;
            }
            if (last != 0) {
                // smoothed length of one compositor pass (the screen's own rate), for the cap's rounding
                frame = frame == 0 ? now - last : (frame * 7 + (now - last)) / 8;
                InterlockedExchange(&native_rate, static_cast<LONG>((frequency + frame / 2) / frame));
            }
            last = now;
            // Cap (test switch and adaptive display rate): post on an accumulating deadline, half a pass early, so a
            // cap of 60 on a 144 Hz screen averages 60 posts per second instead of rounding to every other pass.
            int cap = max_hz;
            if (rate_limit > 0 && (cap == 0 || rate_limit < cap)) cap = static_cast<int>(rate_limit);
            if (cap > 0 && frame != 0 && frame * cap < frequency) {   // the cap is below the screen's rate
                const int64_t period = frequency / cap;
                if (due == 0 || now - due > period) due = now;       // first post, or fell behind: no burst
                if (now < due - frame / 2) continue;
                due += period;
            }
            else due = 0;
            tick();
        }
    }

    void thread_body(Run* r)
    {
        // Drawing must never be favoured over playback: this thread is never above the playback worker (Above Normal),
        // and at Normal with the few-processor option (the same rule as the worker's, win32_timing.h).
        if (!win32::few_cpu_normal_threads_active()) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        const char* why = "timer requested";
        if (mode != PaceMode::Timer) {
            DwmFlushFn flush = nullptr;
            DwmIsCompositionEnabledFn enabled = nullptr;
            if (GetSystemMetrics(SM_REMOTESESSION)) why = "remote session";
            else if (HMODULE dwm = load_dwmapi()) {
                flush = reinterpret_cast<DwmFlushFn>(GetProcAddress(dwm, "DwmFlush"));
                enabled = reinterpret_cast<DwmIsCompositionEnabledFn>(GetProcAddress(dwm, "DwmIsCompositionEnabled"));
                why = "DwmFlush unavailable";
            }
            else why = "no dwmapi.dll";
            if (flush) {
                set_state("display (DwmFlush)");
                why = run_display(r, flush, enabled);
                if (!why) return;
            }
        }
        if (r->stop) return;
        char text[96];
        wsprintfA(text, "timer %d Hz (%s)", timer_hz, why);
        set_state(text);
        run_timer(r, timer_hz);
    }

    static unsigned __stdcall thread_main(void* argument)
    {
        Run* r = static_cast<Run*>(argument);
        r->owner->thread_body(r);
        CloseHandle(r->stop_event);
        delete r;
        return 0;
    }

    // Called with the lock held.
    void start_locked()
    {
        Run* r = new Run();
        r->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        r->stop = 0;
        r->owner = this;
        uintptr_t thread = r->stop_event ? _beginthreadex(nullptr, 0, thread_main, r, 0, nullptr) : 0;
        if (thread == 0) {
            if (r->stop_event) CloseHandle(r->stop_event);
            delete r;
            return;
        }
        CloseHandle(reinterpret_cast<HANDLE>(thread));
        run = r;
    }

    void stop_locked()
    {
        if (!run) return;
        InterlockedExchange(&run->stop, 1);
        SetEvent(run->stop_event);   // the run owns itself; a flush in progress ends within one display refresh
        run = nullptr;
    }
};

DisplayPacer::DisplayPacer() : impl_(new Impl())
{
    impl_->message = RegisterWindowMessageW(L"MIDIBottleneckV2.DisplayRefresh");
}

DisplayPacer& DisplayPacer::instance()
{
    static DisplayPacer* pacer = new DisplayPacer();   // lives until the process ends (the thread may outlive main)
    return *pacer;
}

UINT DisplayPacer::message() { return instance().impl_->message; }

void DisplayPacer::configure(PaceMode mode, int maxHz, int timerHz)
{
    impl_->mode = mode;
    impl_->max_hz = maxHz > 0 ? maxHz : 0;
    impl_->timer_hz = timerHz > 0 ? timerHz : 60;
}

int DisplayPacer::subscribe(HWND window)
{
    Impl& d = *impl_;
    EnterCriticalSection(&d.lock);
    Impl::Subscriber* s = new Impl::Subscriber();
    s->token = d.next_token++;
    s->window = window;
    s->pending = 0;
    d.subscribers.push_back(s);
    if (!d.run) d.start_locked();
    int token = s->token;
    LeaveCriticalSection(&d.lock);
    return token;
}

void DisplayPacer::unsubscribe(int token)
{
    Impl& d = *impl_;
    EnterCriticalSection(&d.lock);
    for (size_t i = 0; i < d.subscribers.size(); ++i)
        if (d.subscribers[i]->token == token) {
            delete d.subscribers[i];
            d.subscribers.erase(d.subscribers.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    if (d.subscribers.empty()) d.stop_locked();
    LeaveCriticalSection(&d.lock);
}

void DisplayPacer::finished(int token)
{
    Impl& d = *impl_;
    EnterCriticalSection(&d.lock);
    for (size_t i = 0; i < d.subscribers.size(); ++i)
        if (d.subscribers[i]->token == token) {
            InterlockedExchange(&d.subscribers[i]->pending, 0);
            break;
        }
    LeaveCriticalSection(&d.lock);
}

std::string DisplayPacer::description() const
{
    Impl& d = *impl_;
    EnterCriticalSection(&d.lock);
    std::string text = d.state;
    LeaveCriticalSection(&d.lock);
    return text;
}

void DisplayPacer::set_rate_limit(int hz) { InterlockedExchange(&impl_->rate_limit, hz > 0 ? hz : 0); }

int DisplayPacer::native_hz() const { return static_cast<int>(impl_->native_rate); }

void DisplayPacer::counters(int64_t& posted, int64_t& coalesced) const
{
    posted = impl_->posted;
    coalesced = impl_->coalesced;
}

} // namespace ui
} // namespace midibn
