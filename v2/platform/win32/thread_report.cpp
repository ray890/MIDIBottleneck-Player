#define WIN32_LEAN_AND_MEAN
#include "thread_report.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>

namespace midibn {
namespace win32 {

namespace {

typedef LONG(NTAPI* NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);

const ULONG kThreadQuerySetWin32StartAddress = 9;

std::string narrow(const wchar_t* text)
{
    std::string out;
    for (; *text; ++text) out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return out;
}

std::string file_name_of(const wchar_t* path)
{
    const wchar_t* slash = wcsrchr(path, L'\\');
    return narrow(slash ? slash + 1 : path);
}

} // namespace

const char* thread_priority_name(int priority)
{
    switch (priority) {
    case -15: return "Idle";
    case -2: return "Lowest";
    case -1: return "Below Normal";
    case 0: return "Normal";
    case 1: return "Above Normal";
    case 2: return "Highest";
    case 15: return "Time Critical";
    default: return "Other";
    }
}

std::string process_priority_class_name()
{
    switch (GetPriorityClass(GetCurrentProcess())) {
    case IDLE_PRIORITY_CLASS: return "Idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "Below Normal";
    case NORMAL_PRIORITY_CLASS: return "Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "Above Normal";
    case HIGH_PRIORITY_CLASS: return "High";
    case REALTIME_PRIORITY_CLASS: return "Realtime";
    default: return "unknown";
    }
}

std::vector<ThreadInfo> snapshot_threads()
{
    std::vector<ThreadInfo> result;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQueryInformationThreadFn query = ntdll
        ? reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread")) : nullptr;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    GetThreadDescriptionFn describe = kernel
        ? reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel, "GetThreadDescription")) : nullptr;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        ThreadInfo info;
        info.id = entry.th32ThreadID;
        info.module = "(unknown)";
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, info.id);
        if (h == nullptr) {
            result.push_back(info);
            continue;
        }
        int p = GetThreadPriority(h);
        if (p != THREAD_PRIORITY_ERROR_RETURN) { info.priority = p; info.priority_known = true; }
        FILETIME created, exited, kernelTime, userTime;
        if (GetThreadTimes(h, &created, &exited, &kernelTime, &userTime)) {
            ULARGE_INTEGER k, u;
            k.LowPart = kernelTime.dwLowDateTime; k.HighPart = kernelTime.dwHighDateTime;
            u.LowPart = userTime.dwLowDateTime; u.HighPart = userTime.dwHighDateTime;
            info.cpu_ms = static_cast<double>(k.QuadPart + u.QuadPart) / 10000.0;
            info.cpu_known = true;
        }
        if (query != nullptr) {
            PVOID address = nullptr;
            ULONG returned = 0;
            if (query(h, kThreadQuerySetWin32StartAddress, &address, sizeof(address), &returned) >= 0 && address != nullptr) {
                info.start_address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
                HMODULE module = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(address), &module) && module != nullptr) {
                    wchar_t path[MAX_PATH];
                    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
                    if (n > 0 && n < MAX_PATH) info.module = file_name_of(path);
                    info.module_offset = info.start_address - static_cast<uint64_t>(reinterpret_cast<uintptr_t>(module));
                }
                else info.module = "(no module: allocated memory)";
            }
        }
        if (describe != nullptr) {
            PWSTR text = nullptr;
            if (SUCCEEDED(describe(h, &text)) && text != nullptr) {
                info.name = narrow(text);
                LocalFree(text);
            }
        }
        CloseHandle(h);
        result.push_back(info);
    }
    CloseHandle(snap);
    return result;
}

std::set<uint32_t> thread_ids(const std::vector<ThreadInfo>& threads)
{
    std::set<uint32_t> ids;
    for (size_t i = 0; i < threads.size(); ++i) ids.insert(threads[i].id);
    return ids;
}

std::string format_thread_table(const std::vector<ThreadInfo>& threads, const std::set<uint32_t>* known, uint32_t self_id)
{
    std::string out;
    char line[512];
    std::snprintf(line, sizeof(line), "process priority class: %s; %u thread(s)\n", process_priority_class_name().c_str(),
        static_cast<unsigned>(threads.size()));
    out += line;
    out += "  thread-id  priority         cpu-ms      start-module (offset)                      name / note\n";
    for (size_t i = 0; i < threads.size(); ++i) {
        const ThreadInfo& t = threads[i];
        char cpu[32] = "?";
        if (t.cpu_known) std::snprintf(cpu, sizeof(cpu), "%.1f", t.cpu_ms);
        char pr[40];
        if (t.priority_known) std::snprintf(pr, sizeof(pr), "%s (%d)", thread_priority_name(t.priority), t.priority);
        else std::snprintf(pr, sizeof(pr), "(no access)");
        char where[200];
        if (t.start_address != 0) std::snprintf(where, sizeof(where), "%s+0x%llx", t.module.c_str(), static_cast<unsigned long long>(t.module_offset));
        else std::snprintf(where, sizeof(where), "%s", t.module.c_str());
        std::string note = t.name;
        if (known != nullptr && known->find(t.id) == known->end()) note += note.empty() ? "NEW" : "  NEW";
        if (t.id == self_id) note += note.empty() ? "(this tool's own thread)" : "  (this tool's own thread)";
        std::snprintf(line, sizeof(line), "  %-9u  %-16s %-10s  %-40s %s\n", static_cast<unsigned>(t.id), pr, cpu, where, note.c_str());
        out += line;
    }
    return out;
}

uint32_t usable_logical_processors()
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

} // namespace win32
} // namespace midibn
