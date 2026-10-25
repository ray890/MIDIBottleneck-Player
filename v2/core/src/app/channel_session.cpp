// Port of the Channels parts of v1 MainForm.cs and the request flow of ChannelMonitorForm.cs.
#include "midibn/app/channel_session.h"

#include <atomic>
#include <climits>

namespace midibn {
namespace app {

namespace {

const char kLoadingMessage[] = "Loading new MIDI\xE2\x80\xA6";   // U+2026
const char kNoSongMessage[] = "No MIDI file is loaded.";

std::string error_text(const engine::EngineError& error)
{
    return !error.message.empty() ? error.message : error.type;
}

} // namespace

ChannelSession::ChannelSession(const Host& host)
    : host_(host), model_(host.culture), editor_(model_, *this, host.culture), inbox_(std::make_shared<Inbox>())
{
    inbox_->wake = host.wake;
}

ChannelSession::~ChannelSession()
{
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        inbox_->closed = true;
        inbox_->items.clear();
    }
    stop();
}

void ChannelSession::stop()
{
    // v1 FormClosed / CloseChannelMonitor (only while this window had monitoring on, so that a window
    // opened after it keeps its own).
    if (monitoring_ && host_.engine) host_.engine->set_monitoring(false);
    monitoring_ = false;
}

// ---------------------------------------------------------------- following the player

void ChannelSession::start(const ChannelSongInfo& info)
{
    host_.engine->set_monitoring(true);
    monitoring_ = true;
    cached_song_ = nullptr;
    cached_index_ = -1;
    ++view_revision_;
    if (!info.has_song) {
        ChannelGridChanges changes;
        model_.detach(kNoSongMessage, &changes);
        changes_.mark_all();
        return;
    }
    model_.attach(info.file_name);
    attached_file_ = info.file_name;
    attached_revision_ = info.song_revision;
    update_source_readouts(info);
    update_snapshot_now();
    if (info.chase_enabled)
        host_.engine->chase_state_for_new_monitor(make_completion(Completion::MonitorChase, -1, ChannelAttribute::BankMsb));
    changes_.mark_all();
}

void ChannelSession::detach(const std::string& message)
{
    if (!model_.detached() || model_.detached_message() != message) {
        editor_.close(false);
        ChannelGridChanges changes;
        model_.detach(message, &changes);
        changes_.mark_all();
        last_snapshot_.reset();
        snapshot_dirty_ = true;
        cached_song_ = nullptr;
        cached_index_ = -1;
        ++view_revision_;
    }
    if (monitoring_) {
        host_.engine->set_monitoring(false);   // v1 UnloadCurrentSong(preserve): SetChannelMonitoring(false)
        monitoring_ = false;
    }
}

void ChannelSession::attach(const std::string& fileName, uint64_t songRevision)
{
    // v1 MainForm after a successful load: SetChannelMonitoring(true), AttachSong, UpdateSnapshot.
    host_.engine->set_monitoring(true);
    monitoring_ = true;
    model_.attach(fileName);
    attached_file_ = fileName;
    attached_revision_ = songRevision;
    last_snapshot_.reset();
    snapshot_dirty_ = true;
    cached_song_ = nullptr;
    cached_index_ = -1;
    changes_.mark_all();
    ++view_revision_;
}

void ChannelSession::report_chase_result(int channel, ChannelAttribute attribute, const std::string& error)
{
    engine::EngineError e;
    if (!error.empty()) e = engine::EngineError::make("Exception", error);
    add_chase_completion(channel, attribute, e);
    pump();
}

void ChannelSession::update_source_readouts(const ChannelSongInfo& info)
{
    if (info.source == nullptr) return;
    if (!info.chase_enabled) {
        if (model_.has_source_readouts()) {
            model_.set_source_readouts(nullptr);
            snapshot_dirty_ = true;
        }
        cached_index_ = -1;
        return;
    }
    int32_t exclusive = info.source->events.lower_bound_by_time(info.position_us);
    if (info.source == cached_song_ && info.song_revision == cached_revision_ && exclusive == cached_index_) return;
    if (info.source->chase_index == nullptr) return;
    std::array<ChannelReadout, 16> readouts = info.source->chase_index->attribute_readouts(exclusive);
    model_.set_source_readouts(&readouts);
    cached_song_ = info.source;
    cached_revision_ = info.song_revision;
    cached_index_ = exclusive;
    snapshot_dirty_ = true;
}

void ChannelSession::update_snapshot_now()
{
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> snapshot = host_.engine->snapshot();
    if (!snapshot) return;
    last_snapshot_ = snapshot;
    snapshot_dirty_ = false;
    model_.update_snapshot(*snapshot, &changes_);
}

void ChannelSession::sync(const ChannelSongInfo& info)
{
    if (info.loading) {
        detach(kLoadingMessage);
        return;
    }
    if (!info.has_song) {
        detach(info.absent_message.empty() ? std::string(kNoSongMessage) : info.absent_message);
        return;
    }
    if (model_.detached()) {
        attach(info.file_name, info.song_revision);   // v1 after a successful load
    }
    else if (info.song_revision != attached_revision_) {
        // Re-routed (or replaced without being seen loading): the readouts must be rebuilt.
        attached_revision_ = info.song_revision;
        cached_song_ = nullptr;
        cached_index_ = -1;
        snapshot_dirty_ = true;
        if (info.file_name != attached_file_) {
            attached_file_ = info.file_name;
            model_.attach(info.file_name);
            ++view_revision_;
        }
    }
    update_source_readouts(info);
    std::shared_ptr<const engine::ChannelPlaybackSnapshot> snapshot = host_.engine->snapshot();
    if (snapshot && (snapshot != last_snapshot_ || snapshot_dirty_)) {
        last_snapshot_ = snapshot;
        snapshot_dirty_ = false;
        model_.update_snapshot(*snapshot, &changes_);
    }
}

void ChannelSession::rebuild()
{
    model_.refresh_from_last(&changes_);
    changes_.mark_all();
}

// ---------------------------------------------------------------- completions

engine::ChannelControlCompletion ChannelSession::make_completion(Completion::Kind kind, int channel,
    ChannelAttribute attribute)
{
    std::shared_ptr<Inbox> inbox = inbox_;
    std::shared_ptr<std::atomic<int> > done = std::make_shared<std::atomic<int> >(0);
    return [inbox, done, kind, channel, attribute](const engine::EngineError& error) {
        if (done->exchange(1) != 0) return;
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if (inbox->closed) return;
            Completion c;
            c.kind = kind;
            c.channel = channel;
            c.attribute = attribute;
            c.error = error.ok() ? std::string() : error_text(error);
            inbox->items.push_back(c);
            wake = inbox->wake;
        }
        if (wake) wake();
    };
}

void ChannelSession::add_chase_completion(int channel, ChannelAttribute attribute, const engine::EngineError& error)
{
    make_completion(Completion::Chase, channel, attribute)(error);
}

void ChannelSession::pump()
{
    std::vector<Completion> items;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        items.swap(inbox_->items);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const Completion& c = items[i];
        if (c.kind == Completion::Chase) {
            // The snapshot refresh MainForm queues, then v1 CompleteHistoricalChase's feedback (shown last so
            // that the refresh does not replace the cell tooltip it sets).
            update_snapshot_now();
            if (!c.error.empty() && c.channel >= 0 && c.channel < 16)
                show_feedback(c.channel, channel_column_for_attribute(c.attribute), c.error);
        }
        else {
            update_snapshot_now();
            if (!c.error.empty()) {
                ChannelFeedback f;
                f.kind = ChannelFeedback::MonitorStatus;
                f.text = "Could not refresh MIDI state: " + c.error;
                f.milliseconds = 3500;
                feedback_.push_back(f);
            }
        }
    }
}

void ChannelSession::control_failed(int channel, ChannelAttribute attribute, const std::string& message, bool mute)
{
    update_snapshot_now();
    if (channel < 0 || channel >= 16) return;
    show_feedback(channel, mute ? static_cast<int>(ColChannel) : channel_column_for_attribute(attribute), message);
}

void ChannelSession::show_feedback(int channel, int column, const std::string& message, bool persistTooltip)
{
    if (message.empty()) return;
    if (persistTooltip) model_.set_cell_tooltip(channel, column, message);
    ChannelFeedback f;
    f.kind = ChannelFeedback::CellTip;
    f.channel = channel;
    f.column = column;
    f.text = message;
    f.milliseconds = 2500;
    feedback_.push_back(f);
}

// ---------------------------------------------------------------- requests

std::string ChannelSession::request_override(int channel, ChannelAttribute attribute, int value)
{
    engine::EngineError error = host_.engine->set_override(channel, attribute, value);
    update_snapshot_now();
    if (!error.ok()) return error_text(error);
    note_request();
    if (host_.overrides_changed) host_.overrides_changed();
    return std::string();
}

std::string ChannelSession::request_historical_chase(int channel, ChannelAttribute attribute)
{
    // The completion carries the outcome (errors included); only a failure to submit comes back here.
    engine::ChannelControlCompletion completion = make_completion(Completion::Chase, channel, attribute);
    engine::EngineError error = host_.engine->chase_latest(channel, attribute, completion);
    note_request();
    if (!error.ok()) completion(error);   // v1: request.Complete(ex)
    pump();
    return error.ok() ? std::string() : error_text(error);
}

std::string ChannelSession::request_enabled(int channel, bool enabled)
{
    engine::EngineError error = host_.engine->set_enabled(channel, enabled);
    update_snapshot_now();
    note_request();
    return error.ok() ? std::string() : error_text(error);
}

bool ChannelSession::cell_mouse_down(int row, int column, Button button, int screenX, int screenY,
    const std::string& typedText)
{
    if (row < 0 || row >= 16 || column < 0 || column >= kChannelColumnCount) return false;
    if (column == ColChannel) {
        hide_editor(true, typedText);
        if (button == Button::Left && model_.has_snapshot()) {
            std::string error = request_enabled(row, !model_.channel_state(row).enabled);
            if (!error.empty()) show_feedback(row, ColChannel, error);
        }
        return false;
    }
    const ChannelColumnInfo& info = channel_column_info(column);
    if (!info.is_attribute) {
        hide_editor(true, typedText);
        return false;
    }
    ChannelAttribute attribute = info.attribute;
    if (button == Button::Right) {
        if (model_.is_forced(row, attribute)) {
            std::string error = request_override(row, attribute, INT32_MIN);
            if (!error.empty()) show_feedback(row, column, error, false);   // v1 dropped this error
        }
        else if (model_.is_historical(row, attribute)) {
            if (!model_.channel_state(row).enabled)
                show_feedback(row, column, "Enable the channel before restoring its source value.");
            else
                request_historical_chase(row, attribute);
        }
        hide_editor(true, typedText);
        return false;
    }
    editor_.open(row, attribute);
    editor_.pointer_down(screenX, screenY);
    return true;
}

void ChannelSession::hide_editor(bool commitTyped, const std::string& typedText)
{
    editor_.close(commitTyped, typedText);
}

ChannelGridChanges ChannelSession::take_changes()
{
    ChannelGridChanges out = changes_;
    changes_ = ChannelGridChanges();
    return out;
}

std::vector<ChannelFeedback> ChannelSession::take_feedback()
{
    std::vector<ChannelFeedback> out;
    out.swap(feedback_);
    return out;
}

void ChannelSession::follow_up_tick()
{
    if (follow_up_ > 0) --follow_up_;
}

} // namespace app
} // namespace midibn
