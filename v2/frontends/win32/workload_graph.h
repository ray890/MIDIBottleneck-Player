// The Analysis window's workload graph control (v1 WorkloadGraph): plain GDI, double
// buffered. All behaviour lives in app::WorkloadGraphModel (through the window model);
// this file only draws the model's frames and forwards pointer input.
#pragma once

#include "ui_common.h"

#include "midibn/app/analysis_window_model.h"

namespace midibn {
namespace ui {

const wchar_t kWorkloadGraphClass[] = L"MidiBnWorkloadGraph";

// Notification (WM_COMMAND to the parent): the model changed through pointer input (inspection text,
// pin, viewport); the parent re-reads what it shows.
const WORD GN_MODEL_CHANGED = 0x4101;

void register_workload_graph_class(HINSTANCE instance);
HWND create_workload_graph(HWND parent, int id, UiContext* ctx, app::AnalysisWindowModel* model);
// The Application scale changed: new label font and metrics.
void workload_graph_scale_changed(HWND graph);
// Repaints (the model has something new to show).
void workload_graph_invalidate(HWND graph);
// Draws the graph into `dc` (client size w x h) exactly as WM_PAINT does; used by WM_PRINTCLIENT.
void workload_graph_paint_to(HWND graph, HDC dc);

} // namespace ui
} // namespace midibn
