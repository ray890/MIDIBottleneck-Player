#include "win32_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#include <memory>

namespace midibn {
namespace win32 {
namespace {

class Win32File : public platform::ByteSource {
public:
    Win32File(HANDLE handle, int64_t size) : handle_(handle), size_(size) {}
    ~Win32File() override { CloseHandle(handle_); }
    int64_t size() const override { return size_; }
    size_t read_at(int64_t offset, uint8_t* buffer, size_t count) override
    {
        if (offset >= size_ || count == 0) return 0;
        if (offset != position_) {
            LARGE_INTEGER target;
            target.QuadPart = offset;
            if (!SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN)) return 0;
            position_ = offset;
        }
        size_t total = 0;
        while (total < count) {
            DWORD chunk = static_cast<DWORD>(count - total > 0x40000000u ? 0x40000000u : count - total);
            DWORD read = 0;
            if (!ReadFile(handle_, buffer + total, chunk, &read, nullptr) || read == 0) break;
            total += read;
            position_ += read;
        }
        return total;
    }

private:
    HANDLE handle_;
    int64_t size_;
    int64_t position_ = 0;
};

} // namespace

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], length);
    return result;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], length, nullptr, nullptr);
    return result;
}

std::unique_ptr<platform::ByteSource> Win32FileSystem::open_read(const std::string& path_utf8, std::string& error)
{
    HANDLE handle = CreateFileW(utf8_to_wide(path_utf8).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "Could not open " + path_utf8 + " (Windows error " + std::to_string(GetLastError()) + ").";
        return nullptr;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size)) {
        error = "Could not read the size of " + path_utf8 + ".";
        CloseHandle(handle);
        return nullptr;
    }
    return std::unique_ptr<platform::ByteSource>(new Win32File(handle, size.QuadPart));
}

ProcessMemory process_memory()
{
    ProcessMemory result;
    PROCESS_MEMORY_COUNTERS counters;
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        result.peak_working_set = counters.PeakWorkingSetSize;
        result.peak_private = counters.PeakPagefileUsage;
        result.current_private = counters.PagefileUsage;
    }
    return result;
}

std::string describe_os()
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW info;
    ZeroMemory(&info, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn getVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!getVersion || getVersion(&info) != 0) {
        info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
#pragma warning(suppress : 4996)  // fallback only; RtlGetVersion exists on XP and later
        GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&info));
    }
    std::string text = "Windows " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
        " build " + std::to_string(info.dwBuildNumber);
    std::string servicePack = wide_to_utf8(info.szCSDVersion);
    if (!servicePack.empty()) text += " " + servicePack;
    text += sizeof(void*) == 8 ? " (x64 process" : " (x86 process";
    typedef BOOL(WINAPI * IsWow64ProcessFn)(HANDLE, PBOOL);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    IsWow64ProcessFn isWow64 = kernel ? reinterpret_cast<IsWow64ProcessFn>(GetProcAddress(kernel, "IsWow64Process")) : nullptr;
    BOOL wow64 = FALSE;
    if (isWow64 && isWow64(GetCurrentProcess(), &wow64) && wow64) text += ", WOW64";
    return text + ")";
}

int64_t performance_counter()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double milliseconds_since(int64_t start_counter)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(performance_counter() - start_counter) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

} // namespace win32
} // namespace midibn
