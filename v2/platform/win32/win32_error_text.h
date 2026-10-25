// Internal to platform/win32: the wording of the failures that the MIDI output
// backends report when Windows refuses a file or memory request. v1 showed the
// raw text of the .NET exception ("Could not find file 'P'.", "Access to the path
// 'P' is denied.", the untrimmed system message with its trailing CR LF,
// Unknown error "<n>"., "Insufficient memory to continue the execution of the
// program."); v2 says what failed and what the user can do (register entry
// DD-003, v2/docs/DIFFERENCES_FROM_V1.md). Where useful the message names the
// file, so a person can see which provider was meant.
//
//  * Opening the KDMAPI provider file for the architecture check fails:
//    provider_file_open_error().
//  * Reading that file's header comes up short: provider_file_truncated_message().
//  * A SysEx buffer cannot be allocated: out_of_memory_for_sysex_message().
//
// The OutputErrorKind of each result is an internal category (tests and the
// engine key on it); it is not shown to the user.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "midibn/midi_output.h"
#include "win32_platform.h"

#include <string>

namespace midibn {
namespace win32 {

// Not enough memory for the buffer that carries one System Exclusive message.
inline const char* out_of_memory_for_sysex_message()
{
    return "There is not enough memory to send a System Exclusive (SysEx) message to the MIDI device. "
           "Close other programs and try again.";
}

// Windows' own description of an error code, without the trailing line break
// the system text carries, or "Windows error <n>" when Windows has no text.
inline std::string system_error_description(DWORD errorCode)
{
    wchar_t buffer[512];
    DWORD length = FormatMessageW(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY,
        nullptr, errorCode, 0, buffer, static_cast<DWORD>(sizeof buffer / sizeof buffer[0]), nullptr);
    while (length != 0 && (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' || buffer[length - 1] == L' '))
        --length;
    if (length != 0) return wide_to_utf8(std::wstring(buffer, length));
    return "Windows error " + std::to_string(static_cast<int>(errorCode));
}

// The KDMAPI provider file could not be opened. `path` is the full path of the
// provider file; `errorCode` is the Win32 error from opening it.
inline platform::OutputStatus provider_file_open_error(DWORD errorCode, const std::string& path)
{
    using platform::OutputErrorKind;
    using platform::OutputStatus;
    const int32_t native = static_cast<int32_t>(errorCode);
    switch (errorCode) {
    case ERROR_FILE_NOT_FOUND:
        return OutputStatus::failure(OutputErrorKind::FileNotFound,
            "The KDMAPI provider file was not found: " + path + ". Check that OmniMIDI is installed and that the file "
            "has not been moved or deleted.", native);
    case ERROR_PATH_NOT_FOUND:
        return OutputStatus::failure(OutputErrorKind::DirectoryNotFound,
            "The folder of the KDMAPI provider file does not exist: " + path + ". Check the folder name or choose a "
            "different provider file.", native);
    case ERROR_ACCESS_DENIED:
        return OutputStatus::failure(OutputErrorKind::UnauthorizedAccess,
            "Windows denied access to the KDMAPI provider file: " + path + ". Check the file's permissions or choose "
            "a copy in a folder you can read.", native);
    case ERROR_FILENAME_EXCED_RANGE:
        return OutputStatus::failure(OutputErrorKind::PathTooLong,
            "The path of the KDMAPI provider file is too long for Windows: " + path + ". Move the file to a folder "
            "with a shorter path.", native);
    case ERROR_INVALID_DRIVE:
        return OutputStatus::failure(OutputErrorKind::DriveNotFound,
            "The drive that holds the KDMAPI provider file is not available: " + path + ". Check that the drive is "
            "connected and ready.", native);
    case ERROR_SHARING_VIOLATION:
        return OutputStatus::failure(OutputErrorKind::IoError,
            "The KDMAPI provider file is being used by another program and cannot be read right now: " + path +
            ". Close the other program and try again.", native);
    case ERROR_OPERATION_ABORTED:
        return OutputStatus::failure(OutputErrorKind::OperationCanceled,
            "Opening the KDMAPI provider file was canceled by Windows: " + path + ". Try again.", native);
    default:
        return OutputStatus::failure(OutputErrorKind::IoError,
            "The KDMAPI provider file could not be opened: " + path + ". Windows reports: " +
            system_error_description(errorCode) + " (error " + std::to_string(static_cast<int>(errorCode)) + ").", native);
    }
}

// The provider file ended before its header could be read completely.
inline std::string provider_file_truncated_message(const std::string& path)
{
    return "The KDMAPI provider file ended before its header could be read: " + path +
           ". The file may be damaged or incomplete; reinstall OmniMIDI or choose a different provider file.";
}

} // namespace win32
} // namespace midibn
