// Win32 file operations for the remembered settings (app/settings_store.h).
//
// Where the file lives:
//   - portable mode: a file named "MIDIBottleneck Player.ini" next to the program (an empty file is enough to switch it on);
//     the program then uses that file and writes nowhere else;
//   - otherwise: %APPDATA%\MIDIBottleneck Player\settings.ini (shared by the x64, x86 and XP builds - the same player).
// The program never writes into its own folder unless the portable file is already there, and never touches the registry.
#pragma once

#include "midibn/app/settings_store.h"

#include <memory>
#include <string>

namespace midibn {
namespace win32 {

// Path the player would use by default, and whether it is the portable file. Empty on failure (no usable folder).
std::wstring default_settings_path(bool& portable);
// Test switch --settings-appdata DIR: use DIR instead of the real Application Data folder (so a test never touches the user's file).
void set_appdata_override_for_tests(const std::wstring& folder);
// `explicitPath` empty: the default path. Never null (a path that cannot be found simply reads as "Missing").
std::unique_ptr<app::SettingsIo> make_settings_io(const std::wstring& explicitPath);

} // namespace win32
} // namespace midibn
