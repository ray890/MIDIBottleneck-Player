// Timing and thread platform tests (Milestone 5): the virtual implementations
// used by deterministic engine tests, and the Win32 ones. Exit code 0 when all pass.
#include "midibn/virtual_platform.h"
#include "win32_timing.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

} // namespace

int main()
{
    using namespace midibn;

    // Virtual clock and waiter.
    platform::VirtualClock clock(10000000, 5);
    clock.advance_microseconds(1500000);
    check(clock.now_ticks() == 15000005, "virtual clock converts microseconds to ticks");
    platform::VirtualWakeEvent wake;
    platform::VirtualWaiter waiter(clock, wake);
    check(waiter.wait_microseconds(0) && clock.now_ticks() == 15000005, "zero wait returns at once");
    check(waiter.wait_microseconds(250) && clock.now_ticks() == 15002505, "wait advances the clock");
    wake.set();
    check(!waiter.wait_microseconds(1000) && clock.now_ticks() == 15002505, "signalled wake returns false, no advance");
    check(wake.is_set(), "manual-reset wake stays set");
    platform::VirtualWakeEvent pulse(true, true);
    check(pulse.poll() && !pulse.poll(), "auto-reset wake is consumed by a poll");

    // Few-processor yield option: one PHYSICAL core decides (a Hyper-Threaded Pentium 4 has 2 logical, 1 physical).
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[4] = {};
        info[0].Relationship = RelationProcessorCore;
        info[1].Relationship = RelationCache;
        info[2].Relationship = RelationNumaNode;
        info[3].Relationship = RelationProcessorPackage;
        check(win32::count_physical_cores(info, sizeof(info)) == 1, "one processor-core entry among cache/node/package entries counts 1");
        info[3].Relationship = RelationProcessorCore;
        check(win32::count_physical_cores(info, sizeof(info)) == 2, "two core entries count 2");
        check(win32::count_physical_cores(info, 0) == 0 && win32::count_physical_cores(nullptr, 100) == 0, "empty input counts 0");
        check(win32::few_cpu_yield_option_offered(1, 2), "1 physical core / 2 logical (Hyper-Threading): offered");
        check(win32::few_cpu_yield_option_offered(1, 1), "1 physical core / 1 logical: offered");
        check(!win32::few_cpu_yield_option_offered(2, 2) && !win32::few_cpu_yield_option_offered(2, 4) && !win32::few_cpu_yield_option_offered(6, 12),
            "2 or more physical cores: not offered");
        check(win32::few_cpu_yield_option_offered(0, 1), "physical count unknown, 1 logical processor: offered");
        check(!win32::few_cpu_yield_option_offered(0, 2) && !win32::few_cpu_yield_option_offered(0, 12), "physical count unknown, 2 or more logical: not offered");
        win32::set_force_one_physical_core(true);
        check(win32::physical_core_count() == 1 && win32::few_cpu_yield_option_available(), "test switch forces one physical core");
        win32::set_force_one_physical_core(false);
        check(win32::physical_core_count() != 1 || win32::few_cpu_yield_option_available() == win32::few_cpu_yield_option_offered(1, win32::system_logical_processor_count()),
            "without the switch the real core count decides");
    }

    // Win32 clock.
    win32::QpcClock qpc;
    int64_t a = qpc.now_ticks();
    int64_t b = qpc.now_ticks();
    check(qpc.frequency() > 0 && b >= a, "QPC clock is monotonic with a positive frequency");

    // Win32 waiter: a short wait completes; a signalled wake returns false at once.
    win32::Win32WakeEvent event;
    std::string error;
    std::unique_ptr<win32::Win32Waiter> timer = win32::Win32Waiter::create(event, error);
    check(timer != nullptr, "waitable timer created");
    if (timer) {
        int64_t start = qpc.now_ticks();
        bool completed = timer->wait_microseconds(3000);
        double waitedUs = static_cast<double>(qpc.now_ticks() - start) * 1e6 / static_cast<double>(qpc.frequency());
        check(completed && waitedUs >= 2700.0, "3 ms wait completes, waking at most ~200 us early");
        std::printf("waiter: %s timer, 3 ms wait took %.0f us\n", timer->high_resolution() ? "high-resolution" : "ordinary",
            waitedUs);
        event.set();
        start = qpc.now_ticks();
        check(!timer->wait_microseconds(1000000), "signalled wake interrupts a 1 s wait");
        check(static_cast<double>(qpc.now_ticks() - start) / static_cast<double>(qpc.frequency()) < 0.5,
            "interrupted wait returns promptly");
        event.reset();
    }

    // Win32 thread: runs, joins, and a waiting thread is woken by the event.
    std::atomic<int> ran(0);
    win32::Win32WakeEvent go;
    std::unique_ptr<win32::Win32Thread> worker = win32::Win32Thread::start(
        [&] {
            if (go.wait(5000)) ran.store(1);
        },
        platform::ThreadPriority::AboveNormal, L"midibn test worker", error);
    check(worker != nullptr, "worker thread started");
    go.set();
    if (worker) worker->join();
    check(ran.load() == 1, "worker woke on the event and finished");

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
