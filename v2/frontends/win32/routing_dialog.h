// The Track routing editor's window (v1 TrackRoutingForm): control ids and the
// test-only messages the dialog answers. show_track_routing_dialog() is declared in
// companion.h.
#pragma once

#include "companion.h"

namespace midibn {
namespace ui {

const wchar_t kRoutingDialogClass[] = L"MidiBnRoutingDialog";

// Control ids (Apply is IDOK, Cancel is IDCANCEL).
enum RoutingDialogControlId {
    RdTier = 101, RdCapacity = 102, RdCapacitySpin = 103, RdPercussion = 104,
    RdUsableFirst = 110,            // ... 110 + 15
    RdLanes = 130, RdDestination = 131, RdPreview = 132, RdStatus = 133, RdConfirm = 134, RdOriginal = 135
};

// Test-only messages (posted/sent by a driving thread; a normal session never uses them).
const UINT WM_ROUTING_STATE = WM_APP + 40;       // returns: bit 0 plan pending, 1 Apply enabled, 2 confirmation visible, 3 plan failed, 4 closed
const UINT WM_ROUTING_SCREENSHOT = WM_APP + 41;  // lParam = const wchar_t* BMP path; returns 1 when written
const UINT WM_ROUTING_ROWS = WM_APP + 42;        // returns the number of lane rows shown

} // namespace ui
} // namespace midibn
