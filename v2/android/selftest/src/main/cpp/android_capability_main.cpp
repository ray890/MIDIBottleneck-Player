// Android capability report (measurements, not pass/fail) of the MIDIBottleneck Self-test app. Modeled on the Linux report
// (platform/linux/linux_capability.cpp). Opens no MIDI port, plays no sound. Runs as test program libst_capability.so.
//   [--quick] [--machine]
// --machine also prints "section.key=value" lines after the readable report.
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/auxv.h>
#include <sys/utsname.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "android_platform.h"
#include "linux_platform.h"

using namespace midibn;

namespace {

std::vector<std::string> g_human, g_machine;
std::string g_key;

void section(const std::string& title, const std::string& key) { g_human.push_back(""); g_human.push_back(title); g_key = key; }
void rec(const std::string& name, const std::string& value, const std::string& note = std::string())
{
    g_human.push_back("  " + name + ": " + value + (note.empty() ? "" : "  (" + note + ")"));
    g_machine.push_back(g_key + "." + name + "=" + value);
}
std::string number(long long v) { return std::to_string(v); }
std::string decimal(double v, int digits) { char b[64]; std::snprintf(b, sizeof(b), "%.*f", digits, v); return b; }
std::string property(const char* name)
{
    char value[PROP_VALUE_MAX] = {};
    int length = __system_property_get(name, value);
    return length > 0 ? std::string(value, static_cast<size_t>(length)) : std::string();
}
std::string or_unknown(const std::string& s) { return s.empty() ? "unknown" : s; }

std::string read_first_line(const std::string& path)
{
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line)) return std::string();
    return line;
}
std::string line_value(const std::string& path, const std::string& key)
{
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            size_t colon = line.find(':', key.size() - 1 > 0 ? key.size() - 1 : 0);
            std::string value = colon == std::string::npos ? line.substr(key.size()) : line.substr(colon + 1);
            size_t start = value.find_first_not_of(" \t");
            size_t end = value.find_last_not_of(" \t\r");
            return start == std::string::npos ? std::string() : value.substr(start, end - start + 1);
        }
    }
    return std::string();
}

int64_t now_ns() { return posix::performance_counter(); }
pid_t thread_id() { return static_cast<pid_t>(::syscall(__NR_gettid)); }

void probe_system()
{
    section("1. Device and system", "device");
    rec("summary", android::describe_device());
    rec("manufacturer", or_unknown(property("ro.product.manufacturer")));
    rec("model", or_unknown(property("ro.product.model")));
    rec("device", or_unknown(property("ro.product.device")));
    rec("android_release", or_unknown(property("ro.build.version.release")));
    rec("api_level", or_unknown(property("ro.build.version.sdk")));
    rec("security_patch", or_unknown(property("ro.build.version.security_patch")));
    rec("build_fingerprint", or_unknown(property("ro.build.fingerprint")));
    rec("abi_list", or_unknown(property("ro.product.cpu.abilist")));
    rec("compiled_abi_of_this_program", android::compiled_abi());
    struct utsname name;
    ::uname(&name);
    rec("kernel", std::string(name.sysname) + " " + name.release);
    rec("machine", name.machine);
    rec("page_size_bytes", number(::sysconf(_SC_PAGESIZE)), "16384 on 16 KB devices");
}

void probe_processor()
{
    section("2. Processor", "processor");
    std::string soc = property("ro.soc.model");
    rec("soc_model", or_unknown(soc), "ro.soc.model, Android 12+");
    rec("soc_manufacturer", or_unknown(property("ro.soc.manufacturer")));
    rec("board_platform", or_unknown(property("ro.board.platform")));
    rec("hardware", or_unknown(property("ro.hardware")));
    std::string cpuModel = line_value("/proc/cpuinfo", "Hardware");
    if (cpuModel.empty()) cpuModel = line_value("/proc/cpuinfo", "model name");
    rec("cpuinfo_hardware", or_unknown(cpuModel));
    rec("cpuinfo_features", or_unknown(line_value("/proc/cpuinfo", "Features")));
    long configured = ::sysconf(_SC_NPROCESSORS_CONF), online = ::sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t set;
    CPU_ZERO(&set);
    long usable = ::sched_getaffinity(0, sizeof(set), &set) == 0 ? CPU_COUNT(&set) : online;
    rec("logical_processors_configured", number(configured));
    rec("logical_processors_online", number(online));
    rec("usable_logical_processors", number(usable), "this process's affinity");
    int readable = 0;
    for (long cpu = 0; cpu < configured && cpu < 64; ++cpu) {
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/";
        std::string max = read_first_line(base + "cpuinfo_max_freq"), min = read_first_line(base + "cpuinfo_min_freq");
        std::string cur = read_first_line(base + "scaling_cur_freq"), gov = read_first_line(base + "scaling_governor");
        if (!max.empty() || !cur.empty()) ++readable;
        std::string text = "max " + (max.empty() ? std::string("?") : decimal(std::atof(max.c_str()) / 1000.0, 0)) + " MHz, min " +
            (min.empty() ? std::string("?") : decimal(std::atof(min.c_str()) / 1000.0, 0)) + " MHz, now " +
            (cur.empty() ? std::string("?") : decimal(std::atof(cur.c_str()) / 1000.0, 0)) + " MHz, governor " + or_unknown(gov);
        rec("cpu" + number(cpu), text);
    }
    rec("frequencies_readable", readable > 0 ? "yes" : "no", readable > 0 ? "" : "Android does not let apps read them on this device");
}

void probe_timing(bool quick)
{
    section("3. Clock and timer precision", "timing");
    const clockid_t clocks[] = { CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_MONOTONIC_RAW, CLOCK_REALTIME };
    const char* clockNames[] = { "CLOCK_MONOTONIC", "CLOCK_BOOTTIME", "CLOCK_MONOTONIC_RAW", "CLOCK_REALTIME" };
    for (int i = 0; i < 4; ++i) {
        struct timespec res = {};
        if (::clock_getres(clocks[i], &res) == 0)
            rec(std::string("resolution_ns_") + clockNames[i], number(static_cast<long long>(res.tv_sec) * 1000000000 + res.tv_nsec));
    }
    const int reads = 20000;
    int64_t start = now_ns(), sink = 0;
    for (int i = 0; i < reads; ++i) sink += now_ns();
    rec("clock_read_ns", decimal(static_cast<double>(now_ns() - start) / reads, 1), sink == 1 ? "" : "cost of one clock read");

    const int rounds = quick ? 20 : 100;
    // The core's waiter against its deadline (the engine's waiting method).
    posix::PosixWakeEvent wake;
    posix::PosixWaiter waiter(wake);
    const long long cases[] = { 2000, 500 };
    for (int c = 0; c < 2; ++c) {
        std::vector<double> late;
        for (int i = 0; i < rounds; ++i) {
            int64_t begin = now_ns();
            platform::PlatformError error;
            waiter.wait(cases[c], error);
            int64_t target = begin + cases[c] * 1000;
            while (now_ns() < target) waiter.spin_once();
            late.push_back(static_cast<double>(now_ns() - target) / 1000.0);
        }
        std::sort(late.begin(), late.end());
        rec("core_waiter_" + number(cases[c]) + "us_late_median_us", decimal(late[late.size() / 2], 1));
        rec("core_waiter_" + number(cases[c]) + "us_late_p99_us", decimal(late[late.size() * 99 / 100], 1));
        rec("core_waiter_" + number(cases[c]) + "us_late_max_us", decimal(late.back(), 1));
    }
    // Plain sleeps: how late does the system return?
    const long long sleeps[] = { 1000, 200 };
    for (int c = 0; c < 2; ++c) {
        std::vector<double> late;
        for (int i = 0; i < rounds; ++i) {
            struct timespec t0;
            ::clock_gettime(CLOCK_MONOTONIC, &t0);
            int64_t begin = static_cast<int64_t>(t0.tv_sec) * 1000000000 + t0.tv_nsec;
            struct timespec pause = { 0, static_cast<long>(sleeps[c] * 1000) };
            ::nanosleep(&pause, nullptr);
            late.push_back(static_cast<double>(now_ns() - begin - sleeps[c] * 1000) / 1000.0);
        }
        std::sort(late.begin(), late.end());
        rec("nanosleep_" + number(sleeps[c]) + "us_late_median_us", decimal(late[late.size() / 2], 1));
        rec("nanosleep_" + number(sleeps[c]) + "us_late_p99_us", decimal(late[late.size() * 99 / 100], 1));
        rec("nanosleep_" + number(sleeps[c]) + "us_late_max_us", decimal(late.back(), 1));
    }
    rec("timer_slack_ns_thread", number(::prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL)));
}

double sleep_late_median_us(int rounds)
{
    std::vector<double> late;
    for (int i = 0; i < rounds; ++i) {
        int64_t begin = now_ns();
        struct timespec pause = { 0, 1000000 };
        ::nanosleep(&pause, nullptr);
        late.push_back(static_cast<double>(now_ns() - begin - 1000000) / 1000.0);
    }
    std::sort(late.begin(), late.end());
    return late[late.size() / 2];
}

void probe_threads(bool quick)
{
    section("4. Thread priority", "threads");
    struct rlimit limit;
    if (::getrlimit(RLIMIT_NICE, &limit) == 0) rec("rlimit_nice", number(static_cast<long long>(limit.rlim_cur)), "20 - lowest nice allowed; 0 = no raise allowed");
    rec("effective_user_id", number(::geteuid()));
    const pid_t tid = thread_id();
    errno = 0;
    int original = ::getpriority(PRIO_PROCESS, tid);
    rec("nice_at_start", number(original));
    const int rounds = quick ? 20 : 60;
    rec("sleep_1ms_late_median_us_at_start", decimal(sleep_late_median_us(rounds), 1));
    const int requests[] = { -1, -2, -4, -8, -16, -19 };
    int best = original;
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        errno = 0;
        int rc = ::setpriority(PRIO_PROCESS, tid, requests[i]);
        int err = errno;
        errno = 0;
        int nowNice = ::getpriority(PRIO_PROCESS, tid);
        std::string name = "setpriority_" + number(requests[i]);
        if (rc == 0 && nowNice == requests[i]) {
            rec(name, "accepted", "sleep 1 ms late median " + decimal(sleep_late_median_us(rounds), 1) + " us");
            best = requests[i];
        } else {
            rec(name, "refused", std::string(std::strerror(err)) + ", nice stays " + number(nowNice));
        }
    }
    rec("lowest_nice_achieved", number(best));
    ::setpriority(PRIO_PROCESS, tid, original);
    struct sched_param param;
    std::memset(&param, 0, sizeof(param));
    param.sched_priority = 1;
    errno = 0;
    int rc = ::sched_setscheduler(0, SCHED_FIFO, &param);
    rec("sched_fifo", rc == 0 ? "accepted" : "refused", rc == 0 ? "" : std::strerror(errno));
    if (rc == 0) { param.sched_priority = 0; ::sched_setscheduler(0, SCHED_OTHER, &param); }

    // The core's own thread helper (what the player's scheduler thread uses).
    std::string error;
    std::unique_ptr<posix::PosixThread> thread = posix::PosixThread::start([] {}, platform::ThreadPriority::AboveNormal, "MIDI bottleneck scheduler", error);
    if (thread) {
        posix::PriorityOutcome outcome = thread->priority_outcome();
        rec("core_thread_above_normal", outcome == posix::PriorityOutcome::Raised ? "raised" : outcome == posix::PriorityOutcome::Refused ? "refused (stays Normal)" : "not requested");
        rec("core_thread_nice", number(thread->nice_value()));
        thread->join();
    } else {
        rec("core_thread_start", "failed: " + error);
    }
}

void probe_memory()
{
    section("5. Memory", "memory");
    rec("MemTotal", or_unknown(line_value("/proc/meminfo", "MemTotal")));
    rec("MemAvailable", or_unknown(line_value("/proc/meminfo", "MemAvailable")));
    rec("SwapTotal", or_unknown(line_value("/proc/meminfo", "SwapTotal")));
    posix::ProcessMemory memory = posix::process_memory();
    rec("peak_resident_bytes", number(static_cast<long long>(memory.peak_working_set)), "VmHWM of this test process");
    rec("peak_virtual_bytes", number(static_cast<long long>(memory.peak_private)), "VmPeak");
}

void probe_storage_midi()
{
    section("6. Native MIDI", "midi");
    rec("opened_a_port", "no", "this report opens nothing; the app's MIDI device list and send check are separate");
}

} // namespace

int main(int argc, char** argv)
{
    bool quick = false, machine = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else if (std::strcmp(argv[i], "--machine") == 0) machine = true;
    }
    g_human.push_back("ANDROID CAPABILITY REPORT, native part (measurements, not pass/fail)");
    probe_system();
    probe_processor();
    probe_timing(quick);
    probe_threads(quick);
    probe_memory();
    probe_storage_midi();
    for (size_t i = 0; i < g_human.size(); ++i) std::printf("%s\n", g_human[i].c_str());
    if (machine) for (size_t i = 0; i < g_machine.size(); ++i) std::printf("%s\n", g_machine[i].c_str());
    return 0;
}
