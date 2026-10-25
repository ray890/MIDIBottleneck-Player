// The Channels window's behaviour without any drawing: how it follows the
// player (v1 MainForm.ShowChannelMonitor, UpdateChannelSourceReadouts,
// EngineChannelControlFailed, DetachForSongReplacement / AttachSong use), and
// the request flow from the grid and the editor to the engine (override,
// historical chase, enable / disable) with its completion and error feedback.
//
// Threading: every method runs on the frontend's UI thread. Engine completions
// may arrive on the engine's worker thread; they are only queued, `Host::wake`
// (thread-safe) asks the UI thread to call `pump()`.
#pragma once

#include "midibn/app/channel_editor.h"
#include "midibn/app/channel_grid.h"
#include "midibn/engine/playback_engine.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace midibn {
namespace app {

// The engine's channel functions (PlaybackEngine, or a fake in tests).
class ChannelEngineAccess {
public:
    virtual ~ChannelEngineAccess() {}
    virtual void set_monitoring(bool enabled) = 0;
    virtual std::shared_ptr<const engine::ChannelPlaybackSnapshot> snapshot() = 0;
    virtual engine::EngineError set_override(int channel, ChannelAttribute attribute, int32_t value) = 0;
    virtual engine::EngineError set_enabled(int channel, bool enabled) = 0;
    virtual engine::EngineError chase_latest(int channel, ChannelAttribute attribute,
        engine::ChannelControlCompletion completion) = 0;
    virtual bool chase_state_for_new_monitor(engine::ChannelControlCompletion completion) = 0;
};

class PlaybackEngineChannels : public ChannelEngineAccess {
public:
    explicit PlaybackEngineChannels(engine::PlaybackEngine& engine) : engine_(engine) {}
    void set_monitoring(bool enabled) override { engine_.set_channel_monitoring(enabled); }
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> snapshot() override { return engine_.channel_snapshot(); }
    engine::EngineError set_override(int channel, ChannelAttribute attribute, int32_t value) override
    { return engine_.set_channel_override(channel, attribute, value); }
    engine::EngineError set_enabled(int channel, bool enabled) override { return engine_.set_channel_enabled(channel, enabled); }
    engine::EngineError chase_latest(int channel, ChannelAttribute attribute,
        engine::ChannelControlCompletion completion) override
    { return engine_.chase_latest_source_channel_attribute(channel, attribute, std::move(completion)); }
    bool chase_state_for_new_monitor(engine::ChannelControlCompletion completion) override
    { return engine_.chase_state_for_new_monitor(std::move(completion)); }

private:
    engine::PlaybackEngine& engine_;
};

// What the window reads from the player on each refresh.
struct ChannelSongInfo {
    bool loading = false;
    bool has_song = false;
    std::string file_name;                  // file name without the folder
    uint64_t song_revision = 0;
    // The window's message while no song is loaded and none is loading: "No MIDI file is loaded.",
    // "MIDI loading cancelled. No MIDI file is loaded." or "MIDI loading failed. No MIDI file is loaded."
    std::string absent_message = "No MIDI file is loaded.";
    bool chase_enabled = true;              // the "chase MIDI state" setting
    int64_t position_us = 0;                // intended timeline while active, else the selected position
    const engine::PlaybackSong* source = nullptr;   // the loaded song as playback sees it
};

// A transient message the frontend shows (v1 ShowFeedback / ShowMonitorStatus).
struct ChannelFeedback {
    enum Kind { CellTip, MonitorStatus };
    Kind kind = CellTip;
    int channel = -1;       // CellTip: row
    int column = -1;        // CellTip: column; the tip appears under the cell for `milliseconds`
    std::string text;       // MonitorStatus: shown at the grid's lower left corner
    int milliseconds = 2500;
};

class ChannelSession : public ChannelRequestSink {
public:
    struct Host {
        ChannelEngineAccess* engine = nullptr;
        format::NumberCulture culture;
        std::function<void()> wake;                // thread-safe: call pump() soon
        std::function<void()> overrides_changed;   // v1 ScheduleAnalysisRefresh (Analysis shows the forced values)
    };

    explicit ChannelSession(const Host& host);
    ~ChannelSession();

    ChannelGridModel& model() { return model_; }
    const ChannelGridModel& model() const { return model_; }
    ChannelEditor& editor() { return editor_; }

    // ---- following the player
    // v1 ShowChannelMonitor, after the window exists: turns monitoring on, shows the current state and,
    // when the chase setting is on, asks for the state of a new monitor.
    void start(const ChannelSongInfo& info);
    // The player refreshed (display rate while playing, otherwise after actions): song changes,
    // source readouts and the newest snapshot. Cheap when nothing changed.
    void sync(const ChannelSongInfo& info);
    // Engine completions arrived (see Host::wake). Also called by the methods that can complete at once.
    void pump();
    // The engine reported that a control could not be applied (v1 EngineChannelControlFailed).
    // `mute` is true when the failed request was a mute / unmute: the message then belongs to the Ch cell (the
    // engine reports such a request with a placeholder attribute, which v1 showed on the Bank MSB cell; DD-033).
    void control_failed(int channel, ChannelAttribute attribute, const std::string& message, bool mute = false);
    // Rebuild the cells from the last snapshot (Application scale changed).
    void rebuild();
    // The window closed: monitoring off (v1 FormClosed). The destructor does the same.
    void stop();
    // v1 DetachForSongReplacement (and MainForm turning monitoring off) / AttachSong (monitoring on).
    // sync() does both by itself from ChannelSongInfo; they are public for hosts and tests that do not use it.
    void detach(const std::string& message);
    void attach(const std::string& fileName, uint64_t songRevision);
    // The outcome of a historical chase arrived (v1 CompleteHistoricalChase); an empty `error` is success.
    void report_chase_result(int channel, ChannelAttribute attribute, const std::string& error);

    // ---- grid mouse (v1 GridCellMouseDown); returns true when the editor was opened
    enum class Button { Left, Right };
    // `typedText` is the edit control's text, applied first when the editor was typing and the click
    // ends the edit (a click on the Ch column or a right-click).
    bool cell_mouse_down(int row, int column, Button button, int screenX, int screenY,
        const std::string& typedText = std::string());
    // The pointer left the window or the grid scrolled: commit and hide the editor.
    void hide_editor(bool commitTyped, const std::string& typedText = std::string());

    // ---- what changed since the last call
    ChannelGridChanges take_changes();
    std::vector<ChannelFeedback> take_feedback();
    // Incremented whenever the detached state or the title changed.
    uint64_t view_revision() const { return view_revision_; }
    // After a request the engine may apply it later (paused or playing): the frontend keeps a short timer
    // running while this is true and calls follow_up_tick() each time it fires.
    bool follow_up_active() const { return follow_up_ > 0; }
    void follow_up_tick();

    // ---- ChannelRequestSink (also used directly by tests)
    std::string request_override(int channel, ChannelAttribute attribute, int value) override;
    std::string request_historical_chase(int channel, ChannelAttribute attribute) override;
    std::string request_enabled(int channel, bool enabled) override;

private:
    struct Completion {
        enum Kind { Chase, MonitorChase } kind = Chase;
        int channel = -1;
        ChannelAttribute attribute = ChannelAttribute::BankMsb;
        std::string error;
    };
    // Shared with the completions, which may outlive the session.
    struct Inbox {
        std::mutex mutex;
        std::vector<Completion> items;
        std::function<void()> wake;
        bool closed = false;
    };

    void update_snapshot_now();
    void update_source_readouts(const ChannelSongInfo& info);
    void show_feedback(int channel, int column, const std::string& message, bool persistTooltip = true);
    void add_chase_completion(int channel, ChannelAttribute attribute, const engine::EngineError& error);
    engine::ChannelControlCompletion make_completion(Completion::Kind kind, int channel, ChannelAttribute attribute);
    void note_request() { follow_up_ = 40; }

    Host host_;
    ChannelGridModel model_;
    ChannelEditor editor_;
    std::shared_ptr<Inbox> inbox_;
    bool monitoring_ = false;
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> last_snapshot_;
    bool snapshot_dirty_ = true;
    uint64_t attached_revision_ = 0;
    std::string attached_file_;
    uint64_t view_revision_ = 0;
    const engine::PlaybackSong* cached_song_ = nullptr;
    uint64_t cached_revision_ = 0;
    int32_t cached_index_ = -1;
    ChannelGridChanges changes_;
    std::vector<ChannelFeedback> feedback_;
    int follow_up_ = 0;
};

} // namespace app
} // namespace midibn
