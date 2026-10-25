// Win32 timing and thread implementations (Milestone 5). XP-compatible:
// anything newer is resolved at run time.
#pragma once

#include "midibn/platform.h"

#include <functional>
#include <memory>
#include <string>

namespace midibn {
namespace win32 {

// "Few processors" policy (P10-THR; default OFF, a test option and a proposal, see DEVICE_MEASUREMENTS draft):
//   normal_threads  on a PC where this process may use at most 2 logical processors, v2's own threads (playback worker,
//                   display pacer) run at Normal priority instead of Above Normal;
//   yield_ms        on such a PC, a playback worker that has sent MIDI for yield_ms milliseconds without waiting (it is
//                   behind) gives the other threads a turn (SwitchToThread). 0 = never.
// Both only take effect when fewer than 3 logical processors are usable (the process affinity counts, so a test can
// restrict the player from outside). Set before the engine / threads are created; not meant to change while playing.
void set_few_cpu_policy(bool normalThreads, int yieldMilliseconds);
// True when the policy asks for Normal priority and this PC (process) has at most 2 usable logical processors.
void set_few_cpu_nap(bool nap);   // yield by sleeping 1 ms instead of SwitchToThread (test option nap=MS)
bool few_cpu_normal_threads_active();
int few_cpu_yield_milliseconds_active();   // 0 when off or not a few-processor PC

// Few-processor yield option in the main window's system menu (DD-112 decision: off by default, offered only on a PC
// with exactly one PHYSICAL processor core, so a Hyper-Threaded single-core Pentium 4 counts).
// Physical cores: GetLogicalProcessorInformation (RelationProcessorCore), loaded at run time (XP SP3 has it; older
// systems fall back to the logical processor count). 0 physical cores means "unknown".
uint32_t count_physical_cores(const void* processorInformation, size_t bytes);   // RelationProcessorCore entries in a SYSTEM_LOGICAL_PROCESSOR_INFORMATION array
bool few_cpu_yield_option_offered(uint32_t physicalCores, uint32_t logicalProcessors);   // exactly one physical core; unknown: exactly one logical processor
uint32_t physical_core_count();                  // 0 = unknown; 1 when the test switch forces it
uint32_t system_logical_processor_count();
void set_force_one_physical_core(bool force);    // test switch --force-one-core
bool few_cpu_yield_option_available();           // whether the menu item is shown
constexpr int kFewCpuYieldOptionMicroseconds = 2000;   // the interval measured in DD-112

// QueryPerformanceCounter / QueryPerformanceFrequency (v1 Stopwatch).
class QpcClock : public platform::Clock {
public:
    QpcClock();
    int64_t now_ticks() const override;
    int64_t frequency() const override { return frequency_; }

private:
    int64_t frequency_;
};

// Kernel event (v1 EventWaitHandle).
class Win32WakeEvent : public platform::WakeEvent {
public:
    explicit Win32WakeEvent(bool initiallySet = false, bool autoReset = false);
    ~Win32WakeEvent() override;
    Win32WakeEvent(const Win32WakeEvent&) = delete;
    Win32WakeEvent& operator=(const Win32WakeEvent&) = delete;
    void set() override;
    void reset() override;
    bool poll() override { return wait(0); }
    bool wait(int32_t timeoutMilliseconds) override;
    void* native_handle() const { return handle_; }

private:
    void* handle_;
};

// Port of v1 HighResolutionWaiter: a high-resolution waitable timer when
// available (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), otherwise an ordinary
// waitable timer; the timer covers all but the last 200 us. On the XP build
// the waiter also holds timeBeginPeriod(1) for its lifetime (ARCHITECTURE.md
// §4; recorded in XP_COST_LOG.md).
class Win32Waiter : public platform::Waiter {
public:
    // Returns null and sets `error` to v1's text when no timer can be created.
    static std::unique_ptr<Win32Waiter> create(Win32WakeEvent& wake, std::string& error);
    ~Win32Waiter() override;
    // Reports v1's Win32Exception texts (arming or waiting failed) and the
    // OverflowException of v1's checked(coarse * 10) as WaitResult::Failed.
    platform::WaitResult wait(int64_t microseconds, platform::PlatformError& error) override;
    void begin_spin() override;
    void spin_once() override;
    bool high_resolution() const { return high_resolution_; }

private:
    Win32Waiter(Win32WakeEvent& wake, void* timer, bool highResolution);
    Win32WakeEvent& wake_;
    void* timer_;
    bool high_resolution_;
    bool period_held_ = false;
    int spins_ = 0;
    bool yielding_ = false;
    int64_t spin_started_ = 0;
    bool single_processor_;
    int64_t qpc_frequency_ = 0;
};

// A started thread (v1 worker: new Thread, IsBackground, AboveNormal).
class Win32Thread : public platform::Thread {
public:
    // Returns null and sets `error` when the thread cannot be started.
    static std::unique_ptr<Win32Thread> start(std::function<void()> body, platform::ThreadPriority priority,
        const wchar_t* name, std::string& error);
    ~Win32Thread() override;
    void join() override;
    bool join(int32_t timeoutMilliseconds) override;
    bool is_alive() const override;
    bool is_current() const override;
    void detach() override;

private:
    Win32Thread() {}
    static unsigned __stdcall entry(void* body);
    void* handle_ = nullptr;
    unsigned thread_id_ = 0;
};

// platform::EngineServices over QpcClock, Win32WakeEvent, Win32Waiter and
// Win32Thread (the production implementation).
class Win32EngineServices : public platform::EngineServices {
public:
    platform::Clock& clock() override { return clock_; }
    std::unique_ptr<platform::WakeEvent> create_wake_event(platform::WakeEventRole role, bool initiallySet,
        bool autoReset) override;
    // `wake` must be an event this object created.
    std::unique_ptr<platform::Waiter> create_waiter(platform::WakeEvent& wake, platform::PlatformError& error) override;
    std::unique_ptr<platform::Thread> start_thread(std::function<void()> body, platform::ThreadPriority priority,
        const char* name, platform::PlatformError& error) override;
    int64_t behind_yield_interval_microseconds() const override { return few_cpu_yield_milliseconds_active() * 1000; }
    void yield_to_others() override;

private:
    QpcClock clock_;
};

} // namespace win32
} // namespace midibn
