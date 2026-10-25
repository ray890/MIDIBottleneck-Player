// The player's companion windows (Analysis, Channels, Track routing) and how the
// main window hosts them. A companion window is an ordinary top-level window that
// the main window owns; it reads everything through the PlayerController and never
// touches the engine on its own timer: the main window calls on_refresh() after each
// refresh (display rate while playing, otherwise only after an action), so an idle
// player repaints nothing.
#pragma once

#include "ui_common.h"

#include "midibn/app/player_controller.h"

#include <functional>
#include <memory>

namespace midibn {
namespace ui {

class CompanionWindow {
public:
    virtual ~CompanionWindow() {}
    virtual HWND hwnd() const = 0;
    // The main window refreshed (statistics, position, state changed or may have changed).
    virtual void on_refresh() = 0;
    // The Application scale (UiContext) changed: rebuild fonts and layout.
    virtual void on_scale_changed() = 0;
    // Close the window (the main window is closing or a new file replaces the song).
    virtual void close() = 0;
};

struct CompanionHost {
    app::PlayerController* controller = nullptr;
    UiContext* ctx = nullptr;
    HWND main_window = nullptr;
    // v1 PerformAnalysisSeek: seek the player (a paused player resumes after the seek).
    std::function<void(int64_t microseconds)> analysis_seek;
    // v1 TransportToggleRequested: the same as pressing Play/Pause on the main window.
    std::function<void()> toggle_transport;
    // v1 ChannelsRequested: open or activate the Channels window.
    std::function<void()> open_channels;
    // Ask the main window for a refresh now (after the window changed engine state).
    std::function<void()> request_refresh;
};

// analysis_window.cpp: opens a new Analysis window for the current song (v1 ShowAnalysisClicked).
std::unique_ptr<CompanionWindow> create_analysis_window(CompanionHost& host);
// channels_window.cpp: the single Channels window (v1 ShowChannelMonitor).
std::unique_ptr<CompanionWindow> create_channels_window(CompanionHost& host);
// routing_dialog.cpp: the modal Track routing editor (v1 ShowTrackRoutingDialog). Applies the
// result to the controller itself and shows its own error messages.
void show_track_routing_dialog(CompanionHost& host);

} // namespace ui
} // namespace midibn
