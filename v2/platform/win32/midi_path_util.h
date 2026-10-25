// Internal to platform/win32: the few .NET System.IO / Environment behaviours
// the MIDI backends depend on (Path.GetFullPath, Path.GetDirectoryName,
// File.Exists, AppDomain.BaseDirectory, SpecialFolder.System). XP-compatible.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace midibn {
namespace win32 {

// Path.GetFullPath.
inline std::wstring full_path(const std::wstring& path)
{
    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;
    std::vector<wchar_t> buffer(needed + 1, L'\0');
    DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return length == 0 ? path : std::wstring(buffer.data(), length);
}

// Path.GetDirectoryName of an already full path: text before the last '\'
// ("C:\" keeps its root separator); empty when there is none.
inline std::wstring directory_name(const std::wstring& full)
{
    size_t cut = full.find_last_of(L'\\');
    if (cut == std::wstring::npos) return std::wstring();
    std::wstring directory = full.substr(0, cut);
    if (directory.empty() || directory[directory.size() - 1] == L':') directory += L'\\';
    return directory;
}

// File.Exists: a path that names an existing file (not a directory).
inline bool file_exists(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Path.Combine(directory, name) for a directory that may or may not end in '\'.
inline std::wstring combine_path(const std::wstring& directory, const std::wstring& name)
{
    if (directory.empty()) return name;
    wchar_t last = directory[directory.size() - 1];
    if (last == L'\\' || last == L'/' || last == L':') return directory + name;
    return directory + L'\\' + name;
}

// Directory of the running executable (AppDomain.CurrentDomain.BaseDirectory, with a trailing '\').
inline std::wstring application_directory()
{
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof buffer / sizeof buffer[0]));
    std::wstring directory = directory_name(std::wstring(buffer, length));
    if (!directory.empty() && directory[directory.size() - 1] != L'\\') directory += L'\\';
    return directory;
}

// Environment.GetFolderPath(SpecialFolder.System): System32 (SysWOW64 for a 32-bit process on 64-bit Windows).
inline std::wstring system_directory()
{
    wchar_t buffer[MAX_PATH * 2];
    UINT length = GetSystemDirectoryW(buffer, static_cast<UINT>(sizeof buffer / sizeof buffer[0]));
    return std::wstring(buffer, length);
}

} // namespace win32
} // namespace midibn
