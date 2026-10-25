// Load progress and cancellation (v1 MidiLoadProgress and the
// CancellationToken checkpoints of DirectCompactMidiParser /
// MidiLargeFilePreflight). The core never blocks or reads the time; the app
// supplies a monitor that receives progress and answers whether the user has
// cancelled.
#pragma once

#include <cstdint>
#include <string>

namespace midibn {

struct LoadProgress {
    std::string stage;
    int completed_tracks = 0;
    int total_tracks = 0;
    int64_t overall_completed = 0;
    int64_t overall_total = 1;
    int64_t stage_completed = 0;
    int64_t stage_total = 1;

    LoadProgress() {}
    // Clamps exactly as v1's MidiLoadProgress constructor does.
    LoadProgress(const std::string& stage, int completed_tracks, int total_tracks, int64_t overall_completed,
        int64_t overall_total, int64_t stage_completed, int64_t stage_total);

    int overall_permille() const;
    int stage_permille() const;
};

class LoadMonitor {
public:
    virtual ~LoadMonitor() {}
    virtual void progress(const LoadProgress& progress) = 0;
    // Polled at v1's cancellation checkpoints; true ends the load with
    // OperationCanceledException "The operation was canceled.".
    virtual bool cancellation_requested() = 0;
};

extern const char kCancellationType[];
extern const char kCancellationMessage[];

} // namespace midibn
