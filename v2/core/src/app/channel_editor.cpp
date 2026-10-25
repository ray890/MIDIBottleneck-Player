// The editor logic of v1 ChannelMonitorForm.cs and the channel-field mode of ScrubOrTypeTextBox.cs.
#include "midibn/app/channel_editor.h"

#include <climits>
#include <cstdlib>

namespace midibn {
namespace app {

ChannelEditorSpec channel_editor_spec(const ChannelGridModel& model, int channel, ChannelAttribute attribute)
{
    ChannelEditorSpec s;
    s.channel = channel;
    s.attribute = attribute;
    s.value = model.current_attribute_value(channel, attribute);
    s.forced = model.is_forced(channel, attribute);
    s.historical = model.is_historical(channel, attribute);
    s.minimum = engine::ChannelOverrideState::minimum(attribute);
    s.maximum = engine::ChannelOverrideState::maximum(attribute);
    s.display_offset = attribute == ChannelAttribute::Program ? 1 : 0;
    bool bend = attribute == ChannelAttribute::PitchBend;
    s.coarse_units = bend ? 16 : 1;
    s.coarse_pixels = bend ? 1 : 8;
    s.fine_units = 1;
    s.fine_pixels = bend ? 1 : 16;
    s.help = ChannelGridModel::attribute_help(attribute, channel);
    return s;
}

ChannelEditor::ChannelEditor(const ChannelGridModel& model, ChannelRequestSink& sink, const format::NumberCulture& culture)
    : model_(model), sink_(sink), culture_(culture)
{
}

void ChannelEditor::open(int channel, ChannelAttribute attribute)
{
    finish_gesture();
    spec_ = channel_editor_spec(model_, channel, attribute);
    const ChannelGridModel* model = &model_;
    field_.configure(spec_.value, spec_.minimum, spec_.maximum, spec_.display_offset, spec_.coarse_units,
        spec_.coarse_pixels, spec_.fine_units, spec_.fine_pixels, spec_.forced, spec_.historical,
        [model, channel, attribute](int value) { return model->format_attribute(channel, attribute, value); });
    tooltip_ = spec_.help;
    last_error_.clear();
    bold_ = spec_.forced;
    open_ = true;
}

ChannelEditor::Outcome ChannelEditor::close(bool commitTyped, const std::string& typedText)
{
    Outcome outcome;
    if (!open_) return outcome;
    if (field_.typing()) {
        // v1 FinishHostInteraction: commit like a focus loss, or abandon.
        if (commitTyped) outcome = commit_typed(typedText, false);
        else abandon_typing();
    }
    finish_gesture();
    open_ = false;
    spec_.channel = -1;
    return outcome;
}

void ChannelEditor::finish_gesture()
{
    armed_ = false;
    scrubbing_ = false;
    ignore_warp_ = false;
    field_.end_gesture(false);
}

void ChannelEditor::end_gesture() { finish_gesture(); }

ChannelEditor::Outcome ChannelEditor::apply_request(int value)
{
    Outcome outcome;
    ScrubField::Change change = field_.request_value(value);
    if (!change.changed) return outcome;
    // v1 EditorValueRequested does nothing while no cell is being edited (the control keeps the value).
    if (spec_.channel < 0) return outcome;
    outcome.requested = true;
    bold_ = true;
    std::string error = sink_.request_override(spec_.channel, spec_.attribute, change.value);
    if (error.empty()) {
        last_error_.clear();
        tooltip_ = spec_.help;
    }
    else {
        last_error_ = "Could not apply: " + error;
        tooltip_ = last_error_;
        outcome.error = last_error_;
    }
    return outcome;
}

void ChannelEditor::pointer_down(int screenX, int screenY)
{
    armed_ = true;
    start_x_ = screenX;
    anchor_x_ = screenX;
    anchor_y_ = screenY;
    field_.begin_gesture();
    last_error_.clear();
}

ChannelEditor::PointerResult ChannelEditor::scrub_by(int pixels, bool fine)
{
    PointerResult result;
    std::optional<int> value = field_.scrub(pixels, fine);
    if (!value) return result;
    Outcome outcome = apply_request(*value);
    result.requested = outcome.requested;
    result.error = outcome.error;
    return result;
}

ChannelEditor::PointerResult ChannelEditor::pointer_move(int screenX, int screenY, bool fine, bool leftDown)
{
    PointerResult result;
    if (!armed_) return result;
    if (!leftDown) {
        finish_gesture();
        result.gesture_ended = true;
        return result;
    }
    int total = screenX - start_x_;
    if (!scrubbing_ && std::abs(total) > ScrubField::kDragThreshold) {
        scrubbing_ = true;
        anchor_x_ = screenX;
        anchor_y_ = screenY;
        field_.start_scrubbing();
        result.scrub_started = true;
    }
    if (!scrubbing_) return result;
    if (ignore_warp_ && screenX == anchor_x_ && screenY == anchor_y_) {
        ignore_warp_ = false;
        return result;
    }
    int delta = screenX - anchor_x_;
    if (delta == 0) return result;
    PointerResult applied = scrub_by(delta, fine);
    result.requested = applied.requested;
    result.error = applied.error;
    result.warp = true;
    result.warp_x = anchor_x_;
    result.warp_y = anchor_y_;
    ignore_warp_ = true;
    return result;
}

bool ChannelEditor::pointer_up()
{
    if (!armed_) return false;
    if (scrubbing_) {
        finish_gesture();
        return false;
    }
    armed_ = false;
    begin_typing();
    return true;
}

std::string ChannelEditor::begin_typing()
{
    finish_gesture();
    typing_text_ = field_.begin_typing(culture_);
    return typing_text_;
}

ChannelEditor::Outcome ChannelEditor::commit_typed(const std::string& text, bool explicitCommit)
{
    Outcome outcome;
    ScrubField::CommitResult r = field_.commit_typed(text, explicitCommit, culture_);
    if (!r.accepted) {
        last_error_ = r.error;
        outcome.error = r.error;
        return outcome;
    }
    if (r.request) outcome = apply_request(r.value);
    else last_error_.clear();
    return outcome;
}

void ChannelEditor::abandon_typing()
{
    if (field_.typing()) {
        last_error_.clear();
        field_.abandon_typing();
    }
    finish_gesture();
}

std::string ChannelEditor::adjust_typed(const std::string& text, int direction)
{
    if (!field_.typing()) return text;
    last_error_.clear();
    return field_.adjust_typed_text(text, direction, culture_);
}

ChannelEditor::RightClickResult ChannelEditor::right_click()
{
    RightClickResult result;
    if (field_.forced()) {
        // v1 RequestAuto, then EditorAutoRequested hides the editor on success (and does nothing
        // while no cell is being edited, which counts as success).
        result.handled = true;
        std::string error = spec_.channel < 0 ? std::string() : sink_.request_override(spec_.channel, spec_.attribute, INT32_MIN);
        if (error.empty()) {
            last_error_.clear();
            field_.set_forced(false);
            result.close = true;
        }
        else {
            last_error_ = "Could not return to Auto: " + error;
            result.error = last_error_;
        }
    }
    else if (spec_.historical) {
        // v1 RequestChase through EditorChaseRequested.
        result.handled = true;
        std::string error;
        if (spec_.channel < 0) {
            // nothing is being edited: v1's handler returns without a request
        }
        else if (model_.has_snapshot() && !model_.channel_state(spec_.channel).enabled)
            error = "Enable this MIDI channel before restoring its source value.";
        else
            error = sink_.request_historical_chase(spec_.channel, spec_.attribute);
        if (error.empty()) {
            last_error_.clear();
            result.close = true;
        }
        else {
            last_error_ = "Could not restore source value: " + error;
            result.error = last_error_;
        }
    }
    return result;
}

} // namespace app
} // namespace midibn
