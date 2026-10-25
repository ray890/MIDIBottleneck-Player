// The player's small windows: About, the large-file warning, the Effective
// speed measurement window, and Help (F1).
#pragma once

#include "ui_common.h"

#include <cstdint>
#include <string>

namespace midibn {
namespace ui {

void register_dialog_classes(HINSTANCE instance);

void show_about_dialog(HWND owner, UiContext& ctx);
// True when the user chose Continue.
bool show_large_file_dialog(HWND owner, UiContext& ctx, const std::string& title, const std::string& heading,
    const std::string& text);
// True when the user accepted; `newMicroseconds` receives the chosen window.
bool show_speed_window_dialog(HWND owner, UiContext& ctx, int64_t currentMicroseconds, int64_t& newMicroseconds);
// Creates or reuses the Help window and selects the topic (0..7).
HWND show_help_window(HWND owner, UiContext& ctx, HWND existing, int topic);
void help_window_scale_changed(HWND help, UiContext& ctx);

} // namespace ui
} // namespace midibn
