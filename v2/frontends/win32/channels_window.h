// The Channels window (v1 ChannelMonitorForm): a top-level window with the 16-channel grid, the explanation
// line, the system-menu "Always on top" item, and auto-fit of the window to the grid. companion.h describes how
// the main window hosts it; create_channels_window() is the factory. This header adds what tests need.
#pragma once

#include "channel_grid.h"
#include "companion.h"

#include <atomic>
#include <memory>
#include <string>

namespace midibn {
namespace ui {

// Posted by the session when an engine completion arrived (any thread).
const UINT WM_MIDIBN_CHANNELS_WAKE = WM_APP + 32;
// Posted after a forced value changed: the Analysis windows must refresh (v1 ScheduleAnalysisRefresh).
const UINT WM_MIDIBN_CHANNELS_REFRESH = WM_APP + 33;

// The show command used when the window opens (tests use SW_SHOWNOACTIVATE or SW_SHOWMINNOACTIVE).
extern int g_channels_window_show_command;

class ChannelsWindow : public CompanionWindow {
public:
    ~ChannelsWindow() override;
    // Null when no song is loaded (v1 ShowChannelMonitor does nothing then).
    static std::unique_ptr<ChannelsWindow> create(CompanionHost& host);

    HWND hwnd() const override { return window_; }
    void on_refresh() override;
    void on_scale_changed() override;
    void close() override;

    // ---- for tests and tools
    app::ChannelSession& session() { return *session_; }
    ChannelGrid& grid() { return grid_; }
    bool auto_fit_enabled() const { return auto_fit_; }
    bool always_on_top() const { return always_on_top_; }
    void fit_window_to_grid(bool includeHeight);
    // Writes the window's client area (drawn by its own paint code, never from the screen) as a 32-bit BMP.
    bool write_bitmap(const std::wstring& path);
    std::wstring explanation_text() const;
    void set_always_on_top(bool on);

private:
    ChannelsWindow(CompanionHost& host);
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    bool initialize();
    void build_children();
    app::ChannelSongInfo build_info();
    void process_session_output();
    void update_timers();
    void layout();
    int label_height(int width);
    RECT work_area() const;
    void clamp_to_work_area();
    int outer_width_for_client(int clientWidth) const;
    void note_manual_resize();
    double scale_factor() const;

    CompanionHost host_;
    std::shared_ptr<std::atomic<HWND> > wake_target_;
    std::unique_ptr<app::PlaybackEngineChannels> access_;
    std::unique_ptr<app::ChannelSession> session_;
    ChannelGrid grid_;
    HWND window_ = nullptr;
    HWND explanation_ = nullptr;
    uint64_t seen_view_revision_ = ~0ull;
    bool auto_fit_ = true;
    bool fitting_ = false;
    bool in_size_move_ = false;
    bool settled_ = false;
    SIZE size_at_enter_ = { 0, 0 };
    double canonical_client_width_ = 1240;
    double canonical_client_height_ = 430;
    bool always_on_top_ = false;
    bool follow_up_timer_ = false;
    bool closing_ = false;
};

} // namespace ui
} // namespace midibn
