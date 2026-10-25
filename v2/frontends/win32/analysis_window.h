// The Analysis window (v1 DiagnosticsForm) as a companion window. companion.h declares the
// factory; this header adds what tests and the screenshot helper need.
#pragma once

#include "companion.h"

#include "midibn/app/analysis_window_model.h"

namespace midibn {
namespace ui {

class AnalysisCompanion : public CompanionWindow {
public:
    virtual app::AnalysisWindowModel& model() = 0;
    virtual HWND graph_window() const = 0;
    virtual HWND summary_window() const = 0;
    virtual HWND inspection_window() const = 0;
    virtual HWND seek_button() const = 0;
    virtual bool using_rich_edit() const = 0;
    // Handles pending worker results and due timers now (what the window's own messages do).
    virtual void process_pending() = 0;
    // Window-menu commands (v1 title-bar system menu).
    virtual bool always_on_top() const = 0;
};

// Renders the whole client area of an Analysis window (or any window) to a 32-bit BMP without touching
// the screen: the window's own paint code for the background and every visible child (WM_PRINTCLIENT / WM_PRINT).
bool save_window_bitmap(HWND window, const std::wstring& path);

} // namespace ui
} // namespace midibn
