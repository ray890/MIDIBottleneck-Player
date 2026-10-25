#include "linux_capability.h"

#include "linux_platform.h"

#include <alsa/asoundlib.h>
#include <gnu/libc-version.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

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

class Report {
public:
    Report(std::vector<std::string>& human, std::vector<std::string>& machine) : human_(human), machine_(machine) {}
    void section(const std::string& title, const std::string& key) { human_.push_back(""); human_.push_back(title); key_ = key; }
    void rec(const std::string& name, const std::string& value, const std::string& note = std::string())
    {
        human_.push_back("  " + name + ": " + value + (note.empty() ? "" : "  (" + note + ")"));
        machine_.push_back(key_ + "." + name + "=" + value);
    }

private:
    std::vector<std::string>& human_;
    std::vector<std::string>& machine_;
    std::string key_;
};

std::string number(long long value) { return std::to_string(value); }
std::string decimal(double value, int digits)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return buffer;
}

std::string first_line_value(const std::string& path, const std::string& key)
{
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            std::string value = line.substr(key.size());
            size_t start = value.find_first_not_of(" \t:=\"");
            size_t end = value.find_last_not_of(" \t\"");
            if (start == std::string::npos) return std::string();
            return value.substr(start, end - start + 1);
        }
    }
    return std::string();
}

int64_t ns_now() { return performance_counter(); }

// Physical cores: distinct (package, core) pairs in sysfs; 0 when sysfs does not say.
int physical_core_count()
{
    std::set<std::pair<std::string, std::string>> cores;
    for (int cpu = 0; cpu < 4096; ++cpu) {
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        std::ifstream coreFile(base + "core_id");
        if (!coreFile) { if (cpu > 0 && access(("/sys/devices/system/cpu/cpu" + std::to_string(cpu)).c_str(), F_OK) != 0) break; continue; }
        std::string core, package;
        coreFile >> core;
        std::ifstream packageFile(base + "physical_package_id");
        packageFile >> package;
        cores.insert(std::make_pair(package, core));
    }
    return static_cast<int>(cores.size());
}

void probe_processor(Report& r)
{
    struct utsname name;
    ::uname(&name);
    r.rec("kernel", std::string(name.sysname) + " " + name.release);
    r.rec("architecture", name.machine);
    std::string distribution = first_line_value("/etc/os-release", "PRETTY_NAME");
    r.rec("distribution", distribution.empty() ? "unknown" : distribution);
    r.rec("glibc", gnu_get_libc_version());
    std::string cpu = first_line_value("/proc/cpuinfo", "model name");
    if (cpu.empty()) cpu = first_line_value("/proc/cpuinfo", "Model");
    r.rec("cpu_model", cpu.empty() ? "unknown" : cpu);
    long logical = ::sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t set;
    CPU_ZERO(&set);
    long usable = ::sched_getaffinity(0, sizeof(set), &set) == 0 ? CPU_COUNT(&set) : logical;
    int physical = physical_core_count();
    r.rec("logical_processors", number(logical));
    r.rec("usable_logical_processors", number(usable), "this process's affinity");
    r.rec("physical_cores", physical > 0 ? number(physical) : "unknown");
}

void probe_timing(Report& r, bool quick)
{
    struct timespec resolution;
    ::clock_getres(CLOCK_MONOTONIC, &resolution);
    r.rec("clock_resolution_ns", number(static_cast<long long>(resolution.tv_sec) * 1000000000 + resolution.tv_nsec), "CLOCK_MONOTONIC");
    // Cost of one clock read.
    const int reads = 20000;
    int64_t start = ns_now();
    int64_t sink = 0;
    for (int i = 0; i < reads; ++i) sink += ns_now();
    int64_t spent = ns_now() - start;
    r.rec("clock_read_ns", decimal(static_cast<double>(spent) / reads, 1), sink == 1 ? "" : "per call");
    // The waiter against its deadline: how late does a 2 ms and a 500 microsecond wait return?
    struct Case { int64_t us; };
    const Case cases[] = { { 2000 }, { 500 } };
    const int rounds = quick ? 20 : 100;
    PosixWakeEvent wake;
    PosixWaiter waiter(wake);
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        std::vector<double> late;
        for (int i = 0; i < rounds; ++i) {
            int64_t begin = ns_now();
            platform::PlatformError error;
            waiter.wait(cases[c].us, error);   // returns about 200 microseconds before the target
            int64_t target = begin + cases[c].us * 1000;
            while (ns_now() < target) waiter.spin_once();
            late.push_back(static_cast<double>(ns_now() - target) / 1000.0);
        }
        std::sort(late.begin(), late.end());
        r.rec("wait_" + number(cases[c].us) + "us_late_median_us", decimal(late[late.size() / 2], 1));
        r.rec("wait_" + number(cases[c].us) + "us_late_max_us", decimal(late.back(), 1));
    }
    r.rec("timer_slack_ns_main_thread", number(::prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL)), "threads started by the player set 1");
}

void probe_threads(Report& r)
{
    struct rlimit limit;
    if (::getrlimit(RLIMIT_NICE, &limit) == 0) r.rec("rlimit_nice", number(static_cast<long long>(limit.rlim_cur)), "20 - nice floor; 0 = no raise allowed");
    r.rec("effective_user_id", number(::geteuid()), ::geteuid() == 0 ? "root" : "unprivileged");
    std::string error;
    std::unique_ptr<PosixThread> thread = PosixThread::start([] {}, platform::ThreadPriority::AboveNormal, "MIDI bottleneck scheduler", error);
    if (thread) {
        PriorityOutcome outcome = thread->priority_outcome();
        r.rec("above_normal_request_nice", number(kAboveNormalNice));
        r.rec("above_normal_result", outcome == PriorityOutcome::Raised ? "raised" : outcome == PriorityOutcome::Refused ? "refused (thread stays at Normal)" : "not requested");
        r.rec("above_normal_thread_nice", number(thread->nice_value()));
        thread->join();
    } else {
        r.rec("thread_start", "failed: " + error);
    }
}

void probe_memory(Report& r)
{
    ProcessMemory memory = process_memory();
    r.rec("peak_resident_bytes", number(static_cast<long long>(memory.peak_working_set)), "VmHWM");
    r.rec("peak_virtual_bytes", number(static_cast<long long>(memory.peak_private)), "VmPeak, used as the peak commit");
    std::string total = first_line_value("/proc/meminfo", "MemTotal");
    r.rec("physical_memory", total.empty() ? "unknown" : total, "MemTotal");
}

void probe_midi(Report& r)
{
    r.rec("alsa_library_version", snd_asoundlib_version());
    struct stat info;
    bool present = ::stat("/dev/snd/seq", &info) == 0;
    r.rec("dev_snd_seq", present ? "present" : "absent");
    if (present) r.rec("dev_snd_seq_access", ::access("/dev/snd/seq", R_OK | W_OK) == 0 ? "readable and writable" : "no access for this user");
    r.rec("opened_a_port", "no", "this report only checks the device file");
}

} // namespace

std::vector<std::string> run_linux_capability_report(std::vector<std::string>& machine, bool quick)
{
    std::vector<std::string> human;
    machine.clear();
    Report r(human, machine);
    human.push_back("CAPABILITY REPORT (measurements, not pass/fail)");
    int64_t started = ns_now();
    r.section("1. Processor and system", "processor");
    probe_processor(r);
    r.section("2. Timing", "timing");
    probe_timing(r, quick);
    r.section("3. Threads", "threads");
    probe_threads(r);
    r.section("4. Memory", "memory");
    probe_memory(r);
    r.section("5. MIDI (no port is opened)", "midi");
    probe_midi(r);
    human.push_back("");
    machine.push_back("probe.total_elapsed_ms=" + decimal(static_cast<double>(ns_now() - started) / 1e6, 0));
    return human;
}

} // namespace posix
} // namespace midibn
