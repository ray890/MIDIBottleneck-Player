// A read-only list of the threads in this process: priority, where each thread starts (the module its start address
// lies in), how much processor time it has used. Silent: it only reads. Used by midibn-thread-report (opens an output
// and lists threads before, during and after) and by the player's test option --thread-report FILE.
//
// XP-compatible: the start address comes from ntdll's NtQueryInformationThread (class ThreadQuerySetWin32StartAddress),
// looked up with GetProcAddress; if it is missing or refuses, the start module is "(unknown)". The thread name comes from
// GetThreadDescription where Windows has it (10 and later), otherwise it is empty.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace midibn {
namespace win32 {

struct ThreadInfo {
    uint32_t id = 0;
    int priority = 0;                 // GetThreadPriority: -15 idle ... 0 normal ... 15 time critical
    bool priority_known = false;
    uint64_t start_address = 0;       // 0 when unknown
    std::string module;               // file name only ("OmniMIDI.dll"), "(unknown)" when not found
    uint64_t module_offset = 0;
    double cpu_ms = 0;                // kernel + user time so far
    bool cpu_known = false;
    std::string name;                 // SetThreadDescription text, may be empty
};

const char* thread_priority_name(int priority);
// Lists the threads of the calling process (the calling thread included, marked by the caller if wanted).
std::vector<ThreadInfo> snapshot_threads();
// "Normal", "Above Normal", ... of this process's priority class.
std::string process_priority_class_name();
// One line per thread; a thread whose id is not in `known` gets "NEW" (pass nullptr for no marking); `self_id`
// (when not 0) gets "(this tool's own thread)". Header line first.
std::string format_thread_table(const std::vector<ThreadInfo>& threads, const std::set<uint32_t>* known, uint32_t self_id);
std::set<uint32_t> thread_ids(const std::vector<ThreadInfo>& threads);
// The number of logical processors this process may run on (its affinity mask), at least 1.
uint32_t usable_logical_processors();

} // namespace win32
} // namespace midibn
