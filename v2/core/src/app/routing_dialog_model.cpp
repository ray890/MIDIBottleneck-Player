// Port of the state logic of v1 TrackRoutingForm.cs (see routing_dialog_model.h).
#include "midibn/app/routing_dialog_model.h"

#include "midibn/models/model_base.h"

#include <new>

namespace midibn {
namespace app {

namespace {

// v1 GeneralMidiProgramCatalog.Names.
const char* const kProgramNames[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag Pipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"
};

std::u16string ascii(const std::string& text) { return std::u16string(text.begin(), text.end()); }

std::u16string number(int value) { return ascii(std::to_string(value)); }

std::u16string bank_part(int value) { return value < 0 ? std::u16string(u"?") : number(value); }

// UTF-8 (exception messages) to UTF-16.
std::u16string from_utf8(const std::string& utf8)
{
    std::u16string out;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = c;
        size_t extra = 0;
        if (c >= 0xF0) { cp = c & 0x07u; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1Fu; extra = 1; }
        if ((c >= 0x80 && extra == 0) || (extra != 0 && i + extra >= utf8.size())) {
            out += static_cast<char16_t>(0xFFFD);
            ++i;
            continue;
        }
        for (size_t k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        i += extra + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        }
        else out += static_cast<char16_t>(cp);
    }
    return out;
}

} // namespace

std::u16string describe_patch(const routing::MidiPatchIdentity& patch)
{
    if (patch.program < 0 || patch.program >= 128) return u"No valid Program Change was found for this source lane.";
    std::u16string program = u"Program " + number(patch.program + 1) + u" — " + ascii(kProgramNames[patch.program]);
    if (patch.is_general_midi_bank()) return program + u" (General MIDI).";
    return program + u" (General MIDI reference only; bank " + bank_part(patch.bank_msb) + u"/" + bank_part(patch.bank_lsb) +
        u" may use a different sound).";
}

// ---------------------------------------------------------------- texts

const char16_t* RoutingDialogTexts::title() { return u"Track routing — MIDIBottleneck Player"; }

const char16_t* RoutingDialogTexts::introduction()
{
    return u"Build a session-only suggested route for each MIDI track and channel combination. "
           u"One MIDI track can appear more than once when it uses multiple channels. "
           u"Review every destination and warning, correct rows when needed, then Apply. "
           u"The original MIDI data is never changed.";
}

const char16_t* RoutingDialogTexts::tier_item(int index)
{
    static const char16_t* const items[3] = { u"Exact-safe only", u"GM-family (confirmation required)",
        u"Aggressive (confirmation required)" };
    return index >= 0 && index < 3 ? items[index] : u"";
}

const char16_t* RoutingDialogTexts::percussion_item(int index)
{
    static const char16_t* const items[2] = { u"Reserve destination channel 10", u"Allow any selected destination" };
    return index >= 0 && index < 2 ? items[index] : u"";
}

const char16_t* RoutingDialogTexts::lanes_heading() { return u"MIDI tracks and channels with proposed destinations"; }
const char16_t* RoutingDialogTexts::preview_heading() { return u"Complete plan preview"; }
const char16_t* RoutingDialogTexts::lane_caption() { return u"Selected MIDI track/channel output:"; }

const char16_t* RoutingDialogTexts::correction_help()
{
    return u"Select a table row to choose the MIDI track and channel being edited. "
           u"Automatic suggestion restores planned routing; choosing an output channel "
           u"manually corrects that row's destination.";
}

const char16_t* RoutingDialogTexts::confirm_text()
{
    return u"I understand the listed retimbrings, conflicts, handoffs, or manual-route warnings.";
}

std::u16string RoutingDialogTexts::destination_item(int comboIndex)
{
    if (comboIndex <= 0) return u"Automatic suggestion";
    return u"Output channel " + number(comboIndex);
}

// ---------------------------------------------------------------- model

RoutingDialogModel::RoutingDialogModel(const routing::RoutingSong& song, const routing::TrackRoutingSettings& initial,
    bool hadActiveRoute, const format::NumberCulture& culture)
    : song_(song), culture_(culture), settings_(initial), had_active_route_(hadActiveRoute)
{
    used_.fill(false);
    tier_index_ = static_cast<int>(settings_.maximum_automatic_tier);
    capacity_ = settings_.target_capacity < 1 ? 1 : settings_.target_capacity > 16 ? 16 : settings_.target_capacity;
    percussion_index_ = static_cast<int>(settings_.percussion_policy);
    generation_ = 1;
}

void RoutingDialogModel::settings_changed()
{
    ++generation_;
    pending_ = true;
    confirmed_ = false;
}

void RoutingDialogModel::set_tier(int index)
{
    if (index < 0 || index > 2 || index == tier_index_) return;
    tier_index_ = index;
    settings_.maximum_automatic_tier = static_cast<routing::TrackRoutingTier>(index);
    settings_changed();
}

void RoutingDialogModel::set_capacity(int value)
{
    value = value < 1 ? 1 : value > 16 ? 16 : value;
    if (value == capacity_) return;
    capacity_ = value;
    settings_.target_capacity = value;
    settings_changed();
}

void RoutingDialogModel::set_percussion(int index)
{
    if (index < 0 || index > 1 || index == percussion_index_) return;
    percussion_index_ = index;
    settings_.percussion_policy = static_cast<routing::PercussionRoutingPolicy>(index);
    settings_changed();
}

void RoutingDialogModel::set_usable(int channel, bool usable)
{
    if (channel < 0 || channel > 15) return;
    if (settings_.usable_destination_channels[static_cast<size_t>(channel)] == usable) return;
    settings_.usable_destination_channels[static_cast<size_t>(channel)] = usable;
    settings_changed();
}

void RoutingDialogModel::select_row(int row)
{
    selected_row_ = row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
}

bool RoutingDialogModel::selected_lane(routing::TrackLaneKey& lane) const
{
    if (selected_row_ < 0 || selected_row_ >= static_cast<int>(rows_.size())) return false;
    lane = rows_[static_cast<size_t>(selected_row_)].row.lane;
    return true;
}

int RoutingDialogModel::destination_index() const
{
    routing::TrackLaneKey lane;
    if (!selected_lane(lane)) return -1;
    return routing::route_correction_index(settings_, lane);
}

void RoutingDialogModel::set_selected_lane_destination(int comboIndex)
{
    routing::TrackLaneKey lane;
    if (comboIndex < 0 || comboIndex > 16 || !selected_lane(lane)) return;
    if (comboIndex == destination_index()) return;
    if (comboIndex == 0) settings_.manual_destinations.erase(lane);
    else settings_.manual_destinations[lane] = comboIndex - 1;
    settings_changed();
}

void RoutingDialogModel::set_lossy_confirmed(bool checked) { confirmed_ = checked; }

bool RoutingDialogModel::apply_enabled() const
{
    if (pending_ || failed_) return false;
    return routing::route_apply_state(compiled_.get(), confirmed_).apply_enabled;
}

bool RoutingDialogModel::lossy_confirmation_visible() const
{
    if (pending_ || failed_) return false;
    return routing::route_apply_state(compiled_.get(), confirmed_).lossy_confirmation_visible;
}

bool RoutingDialogModel::status_is_error() const
{
    if (pending_) return false;
    if (failed_) return true;
    return routing::route_apply_state(compiled_.get(), confirmed_).status_error;
}

std::u16string RoutingDialogModel::status_text() const
{
    if (pending_) return u"Calculating the routing plan…";
    if (failed_) return failure_message_ + u" The current route remains unchanged.";
    return routing::route_apply_state(compiled_.get(), confirmed_).status_text;
}

bool RoutingDialogModel::apply()
{
    if (finished() || !apply_enabled()) return false;
    bool explicitlyConfirmed = false;
    if (!routing::route_apply(compiled_.get(), confirmed_, explicitlyConfirmed)) return false;
    outcome_.kind = RoutingDialogOutcome::Apply;
    outcome_.route = compiled_;
    outcome_.settings = settings_;
    outcome_.explicitly_confirmed = explicitlyConfirmed;
    return true;
}

bool RoutingDialogModel::use_original()
{
    if (finished() || !had_active_route_) return false;
    outcome_.kind = RoutingDialogOutcome::UseOriginal;
    outcome_.route.reset();
    outcome_.settings = routing::TrackRoutingSettings();
    outcome_.explicitly_confirmed = false;
    return true;
}

void RoutingDialogModel::rebuild_rows()
{
    rows_.clear();
    if (!compiled_) return;
    std::vector<routing::RouteLaneRow> lanes = routing::route_lane_rows(*compiled_, culture_);
    rows_.reserve(lanes.size());
    for (size_t i = 0; i < lanes.size(); ++i) {
        RoutingDialogRow row;
        row.row = lanes[i];
        row.patch_tooltip = describe_patch(compiled_->lanes[i]->representative_patch);
        rows_.push_back(row);
    }
}

void RoutingDialogModel::adopt(std::shared_ptr<const routing::CompiledTrackRoute> route)
{
    routing::TrackLaneKey previous;
    bool hadLane = selected_lane(previous);
    compiled_ = route;
    used_ = routing::route_used_channels(*compiled_);
    rebuild_rows();
    preview_ = routing::route_preview_text(*compiled_, settings_.maximum_automatic_tier, culture_);
    // v1 RebuildLaneGrid: keep the selected lane when it is still listed, else the first row.
    selected_row_ = rows_.empty() ? -1 : 0;
    if (hadLane)
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].row.lane == previous) { selected_row_ = static_cast<int>(i); break; }
}

bool RoutingDialogModel::plan_ready(uint64_t generation, std::shared_ptr<const routing::CompiledTrackRoute> route)
{
    if (generation != generation_ || !route) return false;
    pending_ = false;
    failed_ = false;
    failure_message_.clear();
    adopt(route);
    return true;
}

bool RoutingDialogModel::plan_failed(uint64_t generation, const std::string& message)
{
    if (generation != generation_) return false;
    pending_ = false;
    failed_ = true;
    failure_message_ = from_utf8(message);
    compiled_.reset();
    used_.fill(false);
    rows_.clear();
    selected_row_ = -1;
    preview_ = u"THE ROUTING PLAN COULD NOT BE CALCULATED\r\n\r\n" + failure_message_ + u"\r\n";
    return true;
}

void RoutingDialogModel::recompute_now()
{
    uint64_t generation = generation_;
    try {
        plan_ready(generation, routing::compile_route(song_, settings_));
    }
    catch (const models::ModelException& exception) {
        plan_failed(generation, exception.message);
    }
    catch (const std::bad_alloc&) {
        plan_failed(generation, "There is not enough memory to calculate the routing plan.");
    }
}

} // namespace app
} // namespace midibn
