// POSIX file operations for the remembered settings (app/settings_store.h) of the GTK player.
// The file is $XDG_CONFIG_HOME/midibottleneck/settings.ini (fallback ~/.config/midibottleneck/settings.ini). Writes go to
// a temporary file in the same folder, are flushed, and replace the old file with rename(), so a crash or a full disk
// leaves the old file intact.
#pragma once

#include "midibn/app/settings_store.h"

#include <memory>
#include <string>

namespace midibn {
namespace gtkui {

// Empty when neither XDG_CONFIG_HOME nor HOME is set.
std::string default_settings_path();
// `explicitPath` empty: the default path.
std::unique_ptr<app::SettingsIo> make_settings_io(const std::string& explicitPath);

}   // namespace gtkui
}   // namespace midibn
