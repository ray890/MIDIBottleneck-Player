// Shared helpers for the command-line tools (not part of the core). Windows uses the Win32 API directly; Linux
// (Milestone 8) uses POSIX. On Linux the tools keep the Windows-style "\" in the paths they build, and the file
// helpers below turn each "\" into "/" before touching the file system.
#pragma once

#include "midibn/model_trace.h"
#include "win32_platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// The entry point: wmain with wide arguments on Windows, main on Linux.
#ifdef _WIN32
#define MIDIBN_TOOL_MAIN wmain
#else
#define MIDIBN_TOOL_MAIN main
#endif

namespace midibn {
namespace tools {

#ifdef _WIN32
typedef wchar_t ToolChar;

inline std::vector<std::string> utf8_arguments(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(win32::wide_to_utf8(argv[i]));
    return args;
}

inline bool read_file(const std::string& path, std::string& contents)
{
    HANDLE h = CreateFileW(win32::utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok) {
        contents.assign(static_cast<size_t>(size.QuadPart), '\0');
        size_t done = 0;
        while (ok && done < contents.size()) {
            DWORD read = 0;
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - done, 1u << 24));
            ok = ReadFile(h, &contents[done], chunk, &read, nullptr) != 0 && read != 0;
            done += read;
        }
    }
    CloseHandle(h);
    return ok;
}

inline bool write_file(const std::string& path, const std::string& contents)
{
    HANDLE h = CreateFileW(win32::utf8_to_wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = contents.empty() ||
        (WriteFile(h, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) != 0 &&
            written == contents.size());
    CloseHandle(h);
    return ok;
}

// File names (not paths) in `directory` whose extension is in `extensions`, sorted.
inline std::vector<std::string> list_files(const std::string& directory, const std::vector<std::string>& extensions)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(win32::utf8_to_wide(directory + "\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = win32::wide_to_utf8(data.cFileName);
        for (const std::string& ext : extensions)
            if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
                names.push_back(name);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end());
    return names;
}

// Creates one directory level; does nothing when it exists or cannot be made.
inline void make_directory(const std::string& path)
{
    CreateDirectoryW(win32::utf8_to_wide(path).c_str(), nullptr);
}

inline std::string executable_directory()
{
    wchar_t buffer[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    std::wstring path(buffer, length);
    size_t slash = path.find_last_of(L"\\/");
    return win32::wide_to_utf8(slash == std::wstring::npos ? L"." : path.substr(0, slash));
}
#else
typedef char ToolChar;

inline std::vector<std::string> utf8_arguments(int argc, char** argv)
{
    return std::vector<std::string>(argv, argv + argc);   // the arguments are UTF-8 already
}

// A path as the tools build it ("dir\\name") turned into a Linux path.
inline std::string native_path(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

inline bool read_file(const std::string& path, std::string& contents)
{
    std::FILE* file = std::fopen(native_path(path).c_str(), "rb");
    if (file == nullptr) return false;
    contents.clear();
    char buffer[1 << 16];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) contents.append(buffer, got);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

inline bool write_file(const std::string& path, const std::string& contents)
{
    std::FILE* file = std::fopen(native_path(path).c_str(), "wb");
    if (file == nullptr) return false;
    bool ok = contents.empty() || std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    ok = std::fclose(file) == 0 && ok;
    return ok;
}

// File names (not paths) in `directory` whose extension is in `extensions`, sorted.
inline std::vector<std::string> list_files(const std::string& directory, const std::vector<std::string>& extensions)
{
    std::vector<std::string> names;
    DIR* dir = opendir(native_path(directory).c_str());
    if (dir == nullptr) return names;
    while (struct dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        struct stat info;
        if (stat(native_path(directory + "/" + name).c_str(), &info) != 0 || S_ISDIR(info.st_mode)) continue;
        for (const std::string& ext : extensions)
            if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
                names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

// Creates one directory level; does nothing when it exists or cannot be made.
inline void make_directory(const std::string& path)
{
    mkdir(native_path(path).c_str(), 0777);
}

inline std::string executable_directory()
{
    char buffer[4096];
    ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return ".";
    std::string path(buffer, static_cast<size_t>(length));
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}
#endif

inline void write_stdout(const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// Loads model-trace fixtures by name from directory.
inline FixtureLoader fixture_loader(const std::string& directory)
{
    return [directory](const std::string& name, std::string& error) -> std::unique_ptr<Song> {
        win32::Win32FileSystem files;
        std::unique_ptr<platform::ByteSource> source = files.open_read(directory + "\\" + name, error);
        if (!source) return nullptr;
        std::unique_ptr<Song> song(new Song());
        LoadError loadError;
        if (!load_song(*source, LoadOptions(), *song, loadError)) {
            error = loadError.type + ": " + loadError.message;
            return nullptr;
        }
        return song;
    };
}

inline std::string build_description()
{
#ifdef _MSC_VER
    std::string text = std::string(sizeof(void*) == 8 ? "x64" : "x86") + " MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    std::string text = std::string(sizeof(void*) == 8 ? "x64" : "x86") + " Clang " + __clang_version__;
#else
    std::string text = std::string(sizeof(void*) == 8 ? "x64" : "x86") + " GCC " + __VERSION__;
#endif
#if defined(MIDIBN_XP_BUILD)
    text += " XP build (v141_xp, _WIN32_WINNT=0x0501)";
#endif
#if defined(MIDIBN_XP_IA32_BUILD)
    text += ", /arch:IA32 (no SSE/SSE2 code in the program itself)";
#endif
    return text;
}

} // namespace tools
} // namespace midibn
