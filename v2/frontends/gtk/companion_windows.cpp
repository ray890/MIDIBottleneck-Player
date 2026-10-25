// Opens the companion windows from the main window. (Analysis, Channels and Track routing are added one by one.)
#include "main_window.h"

namespace midibn {
namespace gtkui {

void MainWindow::open_analysis()
{
    if (!controller_->has_song()) return;
    std::unique_ptr<CompanionWindow> window = create_analysis_window(host_);
    if (window) analysis_windows_.push_back(std::move(window));
    after_action();
}

void MainWindow::open_channels()
{
    if (channels_window_ && channels_window_->alive()) {
        gtk_window_present(GTK_WINDOW(channels_window_->widget()));
        return;
    }
    channels_window_ = create_channels_window(host_);
    after_action();
}
void MainWindow::open_routing()
{
    if (!controller_->track_routing_available() || routing_window_) return;
    show_track_routing_dialog(host_, &routing_window_);
    routing_window_ = nullptr;
    after_action();
}

}   // namespace gtkui
}   // namespace midibn
