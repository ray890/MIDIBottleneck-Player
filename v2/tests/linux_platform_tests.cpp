// Linux platform layer tests (Milestone 8, stage 3): clock, wake events, waiter timing, threads (start, join, timeout,
// is_current after detach, name, priority fallback), file reads and process memory. Timing bounds are deliberately loose
// (a busy build machine must not fail them); the measured values are printed. Opens no MIDI device. Exit code 0 = pass.
#include "linux_platform.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <pthread.h>
#include <unistd.h>

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

void sleep_ms(int milliseconds) { ::usleep(static_cast<useconds_t>(milliseconds) * 1000); }

using namespace midibn;
using namespace midibn::posix;

void test_clock()
{
    MonotonicClock clock;
    check(clock.frequency() == 1000000000, "clock ticks are nanoseconds");
    int64_t a = clock.now_ticks();
    sleep_ms(5);
    int64_t b = clock.now_ticks();
    check(b - a >= 5000000 && b - a < 500000000, "clock advances about 5 ms across a 5 ms sleep");
    bool monotonic = true;
    int64_t last = clock.now_ticks();
    for (int i = 0; i < 100000; ++i) { int64_t t = clock.now_ticks(); if (t < last) monotonic = false; last = t; }
    check(monotonic, "clock never goes backwards");
}

void test_events()
{
    PosixWakeEvent manual;
    check(!manual.poll(), "new manual event is not set");
    manual.set();
    check(manual.poll() && manual.poll(), "manual-reset event stays set");
    manual.reset();
    check(!manual.poll(), "reset clears it");
    PosixWakeEvent pulse(true, true);
    check(pulse.poll() && !pulse.poll(), "auto-reset event is consumed by a poll");
    int64_t start = performance_counter();
    check(!manual.wait(20), "wait(20) on an unset event times out");
    double ms = milliseconds_since(start);
    check(ms >= 19.0 && ms < 400.0, "a 20 ms wait lasts about 20 ms");
    std::printf("  event wait(20 ms) took %.2f ms\n", ms);
    // A signal from another thread ends an untimed wait.
    std::string error;
    PosixWakeEvent gate;
    std::atomic<bool> woke(false);
    std::unique_ptr<PosixThread> waiter = PosixThread::start([&] { gate.wait(-1); woke = true; }, platform::ThreadPriority::Normal, "t-wait", error);
    check(waiter != nullptr, "wait thread starts");
    sleep_ms(20);
    check(!woke.load(), "untimed wait keeps waiting");
    gate.set();
    check(waiter->join(2000) && woke.load(), "set() ends an untimed wait");
    // wait_until with a deadline already past returns at once.
    PosixWakeEvent never;
    int64_t t0 = performance_counter();
    check(!never.wait_until(t0 - 1000), "a deadline in the past times out immediately");
    check(milliseconds_since(t0) < 50.0, "... without waiting");
}

void test_waiter()
{
    PosixWakeEvent wake;
    PosixWaiter waiter(wake);
    platform::PlatformError error;
    check(waiter.wait(0, error) == platform::WaitResult::Elapsed, "zero wait is Elapsed");
    check(waiter.wait(-5, error) == platform::WaitResult::Elapsed, "negative wait is Elapsed");
    check(waiter.wait_microseconds(100), "a wait shorter than the spin window is Elapsed");
    wake.set();
    check(waiter.wait(1000000, error) == platform::WaitResult::Woken, "a set event makes the wait Woken at once");
    check(waiter.wait(0, error) == platform::WaitResult::Elapsed, "... but a zero wait still returns Elapsed first");
    wake.reset();
    error = platform::PlatformError();
    check(waiter.wait(INT64_MAX / 4, error) == platform::WaitResult::Failed && error.type == "OverflowException" &&
        !error.message.empty(), "an enormous wait fails with the overflow error");

    // Accuracy: the wait returns about 200 us BEFORE the target and never after it by much; with the caller's spin the
    // target is met. "early" is how long before the target the call returned.
    for (int us = 500; us <= 5000; us *= (us == 500 ? 4 : 5)) {
        std::vector<double> early, late;
        for (int i = 0; i < 40; ++i) {
            int64_t begin = performance_counter();
            platform::WaitResult result = waiter.wait(us, error);
            int64_t returned = performance_counter();
            int64_t target = begin + static_cast<int64_t>(us) * 1000;
            early.push_back(static_cast<double>(target - returned) / 1000.0);
            while (performance_counter() < target) waiter.spin_once();
            late.push_back(static_cast<double>(performance_counter() - target) / 1000.0);
            check(result == platform::WaitResult::Elapsed, "an unsignalled wait is Elapsed");
        }
        std::sort(early.begin(), early.end());
        std::sort(late.begin(), late.end());
        std::printf("  wait %d us: returned %.0f us before the target (median), after spinning %.1f us late (median), %.1f us (max)\n",
            us, early[early.size() / 2], late[late.size() / 2], late.back());
        check(early[early.size() / 2] > 0.0 && early.front() > -20000.0, "the coarse wait returns before the target (median) and never overshoots it by more than 20 ms");
        check(late[late.size() / 2] < 300.0, "the spun dispatch is within 300 us of the target (median)");
    }
    // A signal from another thread cuts a long wait short.
    std::string text;
    std::unique_ptr<PosixThread> setter = PosixThread::start([&] { sleep_ms(30); wake.set(); }, platform::ThreadPriority::Normal, nullptr, text);
    int64_t start = performance_counter();
    platform::WaitResult result = waiter.wait(5000000, error);
    double ms = milliseconds_since(start);
    check(result == platform::WaitResult::Woken && ms < 1000.0, "a signal from another thread ends a 5 s wait early");
    std::printf("  a 5 s wait cut by a signal after 30 ms returned after %.1f ms\n", ms);
    setter->join();
}

struct Gate {
    PosixWakeEvent started{false, false};
    PosixWakeEvent release{false, false};
};

void test_threads()
{
    std::string error;
    // Start, run, join.
    std::atomic<int> counter(0);
    std::unique_ptr<PosixThread> quick = PosixThread::start([&] { counter = 7; }, platform::ThreadPriority::Normal, "quick", error);
    check(quick != nullptr, "thread starts");
    quick->join();
    check(counter.load() == 7 && !quick->is_alive(), "join waits for the body; the thread is no longer alive");
    check(quick->join(10), "joining a finished thread again is true");

    // Name, is_current inside and outside, priority outcome.
    char nameInside[32] = "";
    std::atomic<int> currentInside(-1);
    Gate gate;
    PosixThread* raw = nullptr;
    std::unique_ptr<PosixThread> named = PosixThread::start([&] {
        ::pthread_getname_np(::pthread_self(), nameInside, sizeof(nameInside));
        gate.started.set();
        gate.release.wait(-1);
        currentInside = raw != nullptr && raw->is_current() ? 1 : 0;
    }, platform::ThreadPriority::AboveNormal, "MIDI bottleneck scheduler", error);
    check(named != nullptr, "named thread starts");
    raw = named.get();
    check(gate.started.wait(2000), "named thread runs");
    check(named->is_alive(), "a running thread is alive");
    check(!named->is_current(), "is_current is false on another thread");
    check(!named->join(30), "join(30 ms) on a blocked thread times out");
    check(named->is_alive(), "... and the thread is still alive");
    gate.release.set();
    check(named->join(2000), "join with a timeout succeeds once the body returns");
    check(currentInside.load() == 1, "is_current is true inside the thread's own body");
    check(std::strcmp(nameInside, "MIDI bottleneck") == 0, "the name is set, cut to 15 characters");
    PriorityOutcome outcome = named->priority_outcome();
    std::printf("  AboveNormal request (nice %d), euid %d: %s, thread nice %d\n", kAboveNormalNice, static_cast<int>(::geteuid()),
        outcome == PriorityOutcome::Raised ? "raised" : outcome == PriorityOutcome::Refused ? "refused" : "not requested", named->nice_value());
    check(outcome == PriorityOutcome::Raised || outcome == PriorityOutcome::Refused, "the priority request has a recorded outcome");
    check(outcome != PriorityOutcome::Raised || named->nice_value() == kAboveNormalNice, "a raised thread runs at the requested nice value");
    if (::geteuid() == 0) check(outcome == PriorityOutcome::Raised, "as root the raise is granted");
    check(outcome != PriorityOutcome::Refused || named->nice_value() >= 0, "a refused thread silently stays at its normal priority");

    // Normal priority asks for nothing.
    std::unique_ptr<PosixThread> normal = PosixThread::start([] {}, platform::ThreadPriority::Normal, nullptr, error);
    normal->join();
    check(normal->priority_outcome() == PriorityOutcome::NotRequested, "Normal priority requests nothing");

    // Detach: the abandoned worker is no longer alive from the owner's view but its body keeps running, and
    // is_current() stays true on it (the engine's final reset depends on that).
    Gate abandoned;
    std::atomic<int> currentAfterDetach(-1);
    std::atomic<bool> finished(false);
    PosixThread* rawAbandoned = nullptr;
    std::unique_ptr<PosixThread> worker = PosixThread::start([&] {
        abandoned.started.set();
        abandoned.release.wait(-1);
        currentAfterDetach = rawAbandoned->is_current() ? 1 : 0;
        finished = true;
    }, platform::ThreadPriority::Normal, "abandoned", error);
    rawAbandoned = worker.get();
    abandoned.started.wait(2000);
    worker->detach();
    check(!worker->is_alive(), "after detach() is_alive is false");
    check(!worker->is_current(), "after detach() is_current is false on the owner");
    abandoned.release.set();
    for (int i = 0; i < 200 && !finished.load(); ++i) sleep_ms(10);
    check(finished.load(), "the abandoned body keeps running to its end");
    check(currentAfterDetach.load() == 1, "is_current() is true on the abandoned worker after detach()");
    worker.reset();

    // Destroying a thread that was neither joined nor detached joins it.
    std::atomic<bool> done(false);
    {
        std::unique_ptr<PosixThread> scoped = PosixThread::start([&] { sleep_ms(30); done = true; }, platform::ThreadPriority::Normal, nullptr, error);
    }
    check(done.load(), "destroying a running thread joins it");

    // The body is destroyed on its own thread (its captures never outlive an abandoned thread's use).
    struct Probe { std::atomic<unsigned long>* where; ~Probe() { *where = static_cast<unsigned long>(::pthread_self()); } };
    std::atomic<unsigned long> destroyedOn{0};
    std::atomic<unsigned long> ranOn{0};
    {
        std::shared_ptr<Probe> probe(new Probe{ &destroyedOn });
        std::unique_ptr<PosixThread> thread = PosixThread::start([probe = std::move(probe), &ranOn] { ranOn = static_cast<unsigned long>(::pthread_self()); }, platform::ThreadPriority::Normal, nullptr, error);
        thread->join();
    }
    check(destroyedOn.load() == ranOn.load() && ranOn.load() != 0 && ranOn.load() != static_cast<unsigned long>(::pthread_self()),
        "the body's captures are destroyed on the worker thread");
}

void test_engine_services()
{
    PosixEngineServices services;
    check(services.clock().frequency() == 1000000000, "services clock is nanoseconds");
    std::unique_ptr<platform::WakeEvent> wake = services.create_wake_event(platform::WakeEventRole::Wake, false, false);
    platform::PlatformError error;
    std::unique_ptr<platform::Waiter> waiter = services.create_waiter(*wake, error);
    check(wake != nullptr && waiter != nullptr, "services create an event and a waiter");
    std::atomic<bool> ran(false);
    std::unique_ptr<platform::Thread> thread = services.start_thread([&] { ran = true; }, platform::ThreadPriority::AboveNormal, "MIDI bottleneck scheduler", error);
    check(thread != nullptr, "services start a thread");
    thread->join();
    check(ran.load(), "the service thread ran its body");
    services.yield_to_others();
}

void test_files()
{
    const char* tmpEnv = std::getenv("TMPDIR");
    const std::string tmpDir = (tmpEnv && *tmpEnv) ? std::string(tmpEnv) : std::string("/tmp");   // Android apps have no /tmp
    std::string pathTemplate = tmpDir + "/midibn-linux-platform-XXXXXX";
    char* path = &pathTemplate[0];
    int descriptor = ::mkstemp(path);
    check(descriptor >= 0, "temporary file created");
    std::vector<uint8_t> content(100000);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<uint8_t>(i * 31u + 7u);
    check(::write(descriptor, content.data(), content.size()) == static_cast<ssize_t>(content.size()), "temporary file written");
    ::close(descriptor);

    PosixFileSystem files;
    std::string error;
    std::unique_ptr<platform::ByteSource> source = files.open_read(path, error);
    check(source != nullptr && source->size() == 100000, "file opens and reports its size");
    if (source) {
        std::vector<uint8_t> buffer(500);
        check(source->read_at(99000, buffer.data(), 500) == 500 && std::memcmp(buffer.data(), content.data() + 99000, 500) == 0, "read_at reads the bytes at an offset");
        check(source->read_at(99800, buffer.data(), 500) == 200 && std::memcmp(buffer.data(), content.data() + 99800, 200) == 0, "a read past the end returns what is there");
        check(source->read_at(100000, buffer.data(), 500) == 0 && source->read_at(5000000, buffer.data(), 500) == 0, "a read at or beyond the end returns 0");
        check(source->read_at(0, buffer.data(), 0) == 0, "a zero-length read returns 0");
        std::vector<uint8_t> all(100000);
        check(source->read_at(0, all.data(), all.size()) == all.size() && all == content, "the whole file reads back identically");
        check(source->read_at(10, buffer.data(), 3) == 3 && source->read_at(0, buffer.data() + 3, 3) == 3, "reads are independent of each other (no shared position)");
    }
    source.reset();

    // UTF-8 path.
    std::string utf8 = tmpDir + std::string("/midibn-t\xC3\xA9st-\xE2\x82\xAC-") + std::to_string(::getpid()) + ".mid";
    std::FILE* file = std::fopen(utf8.c_str(), "wb");
    check(file != nullptr, "file with a UTF-8 name created");
    if (file) { std::fwrite("MThd", 1, 4, file); std::fclose(file); }
    source = files.open_read(utf8, error);
    check(source != nullptr && source->size() == 4, "a UTF-8 path opens");
    source.reset();
    ::unlink(utf8.c_str());

    // The Windows-style separator used by the tests is read as "/" when the path does not exist as written.
    std::string backslash = path;
    std::replace(backslash.begin(), backslash.end(), '/', '\\');
    check(files.open_read(backslash, error) != nullptr, "a path written with backslashes opens (test-suite convention)");

    source = files.open_read(tmpDir + "/midibn-no-such-file", error);
    check(source == nullptr && error.find("Could not open " + tmpDir + "/midibn-no-such-file") == 0, "a missing file gives a plain message");
    source = files.open_read(tmpDir, error);
    check(source == nullptr && error.find("Could not read the size") == 0, "a directory is not a file");
    ::unlink(path);
}

void test_memory()
{
    std::vector<char> big(20 * 1024 * 1024, 1);   // touch 20 MB so the peak is clearly non-zero
    // Clang (the Android NDK) removes an allocation whose contents are never read; the volatile reads keep the 20 MB real.
    volatile const char* touched = big.data();
    long sum = 0;
    for (size_t i = 0; i < big.size(); i += 4096) sum += touched[i];
    check(sum != 0 || big.size() == 0, "the test buffer was touched");
    ProcessMemory memory = process_memory();
    check(memory.peak_working_set > 20u * 1024 * 1024, "peak resident size is reported (VmHWM)");
    check(memory.peak_private >= memory.peak_working_set, "peak virtual size is at least the peak resident size (VmPeak)");
    check(memory.current_private > 0, "current virtual size is reported");
    std::printf("  peak resident %llu KB, peak virtual %llu KB, current virtual %llu KB (%zu)\n",
        static_cast<unsigned long long>(memory.peak_working_set / 1024), static_cast<unsigned long long>(memory.peak_private / 1024),
        static_cast<unsigned long long>(memory.current_private / 1024), big.size());
    check(describe_os().find("Linux ") == 0, "describe_os starts with Linux");
}

} // namespace

int main()
{
    test_clock();
    test_events();
    test_waiter();
    test_threads();
    test_engine_services();
    test_files();
    test_memory();
    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
