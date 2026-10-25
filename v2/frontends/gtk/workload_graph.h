// The Analysis window's workload graph as a GTK drawing area (cairo + pango). All behaviour is in
// app::WorkloadGraphModel (behind app::AnalysisWindowModel); this file feeds pointer events in and draws the two frames.
#pragma once

#include "gtk_util.h"

#include "midibn/app/analysis_window_model.h"

#include <functional>
#include <memory>

namespace midibn {
namespace gtkui {

class WorkloadGraphWidget {
public:
    // `changed` is called after every input that may have changed the model (the window then syncs its other controls).
    WorkloadGraphWidget(app::AnalysisWindowModel* model, std::function<void()> changed);
    ~WorkloadGraphWidget();
    WorkloadGraphWidget(const WorkloadGraphWidget&) = delete;
    WorkloadGraphWidget& operator=(const WorkloadGraphWidget&) = delete;

    GtkWidget* widget() const { return area_; }
    void scale_changed();   // the Application scale changed: new font, new static layer
    void invalidate();      // the model changed: repaint when it asks for it, run the animation while it needs one
    void detach();          // the window is closing: stop calling into the model

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    GtkWidget* area_ = nullptr;
};

}   // namespace gtkui
}   // namespace midibn
