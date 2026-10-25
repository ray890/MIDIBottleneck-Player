#include "settings_file.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>

namespace midibn {
namespace win32 {
namespace {

const wchar_t kPortableName[] = L"MIDIBottleneck Player.ini";
const wchar_t kFolderName[] = L"MIDIBottleneck Player";
const wchar_t kFileName[] = L"settings.ini";

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string error_detail(const char* what, DWORD code, const std::wstring& path)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "Windows error %lu while %s ", static_cast<unsigned long>(code), what);
    return std::string(buffer) + narrow(path);
}

class Win32SettingsIo : public app::SettingsIo {
public:
    explicit Win32SettingsIo(const std::wstring& path) : path_(path) {}

    ReadResult read(std::string& bytes, std::string& detail) override
    {
        bytes.clear();
        if (path_.empty()) return ReadResult::Missing;
        HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return ReadResult::Missing;
            detail = error_detail("opening", code, path_);
            return ReadResult::Failed;
        }
        // At most 70 000 bytes: a bigger file is not ours, and the parser calls anything over 64 KiB damaged.
        const DWORD limit = 70000;
        bytes.resize(limit);
        DWORD got = 0;
        BOOL ok = ReadFile(file, &bytes[0], limit, &got, nullptr);
        DWORD code = GetLastError();
        CloseHandle(file);
        if (!ok) {
            bytes.clear();
            detail = error_detail("reading", code, path_);
            return ReadResult::Failed;
        }
        bytes.resize(got);
        return ReadResult::Ok;
    }

    bool write_atomic(const std::string& bytes, std::string& detail) override
    {
        if (path_.empty()) { detail = "No folder for the settings file was found."; return false; }
        const size_t slash = path_.find_last_of(L"\\/");
        if (slash != std::wstring::npos) CreateDirectoryW(path_.substr(0, slash).c_str(), nullptr);   // one level; fails harmlessly when it exists
        const std::wstring temp = path_ + L".tmp";
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { detail = error_detail("creating", GetLastError(), temp); return false; }
        DWORD written = 0;
        BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        DWORD code = GetLastError();
        if (ok && written == bytes.size()) ok = FlushFileBuffers(file), code = GetLastError();
        else ok = FALSE;
        CloseHandle(file);
        if (!ok) {
            DeleteFileW(temp.c_str());
            detail = error_detail("writing", code, temp);
            return false;
        }
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            code = GetLastError();
            DeleteFileW(temp.c_str());
            detail = error_detail("replacing", code, path_);
            return false;
        }
        return true;
    }

private:
    std::wstring path_;
};

} // namespace

static std::wstring g_appDataOverride;

std::wstring default_settings_path(bool& portable)
{
    portable = false;
    wchar_t exe[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring folder(exe, length);
        const size_t slash = folder.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring candidate = folder.substr(0, slash + 1) + kPortableName;
            const DWORD attributes = GetFileAttributesW(candidate.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                portable = true;
                return candidate;
            }
        }
    }
    if (!g_appDataOverride.empty()) return g_appDataOverride + L"\\" + kFolderName + L"\\" + kFileName;
    wchar_t appData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData))) return std::wstring();
    return std::wstring(appData) + L"\\" + kFolderName + L"\\" + kFileName;
}

void set_appdata_override_for_tests(const std::wstring& folder) { g_appDataOverride = folder; }

std::unique_ptr<app::SettingsIo> make_settings_io(const std::wstring& explicitPath)
{
    std::wstring path = explicitPath;
    if (path.empty()) {
        bool portable = false;
        path = default_settings_path(portable);
    }
    return std::unique_ptr<app::SettingsIo>(new Win32SettingsIo(path));
}

} // namespace win32
} // namespace midibn
