// Port of v1 GeneralMidiProgramCatalog.cs.
#include "midibn/app/channel_catalog.h"

#include "midibn/format/net_format.h"

namespace midibn {
namespace app {

namespace {

const char* const kNames[kGeneralMidiProgramCount] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag Pipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"
};

const char kEmDash[] = "\xE2\x80\x94";

std::string bank_text(int value)
{
    return value < 0 ? std::string("?") : format::format_integer(value);
}

} // namespace

std::string general_midi_program_name(int zeroBasedProgram)
{
    return zeroBasedProgram >= 0 && zeroBasedProgram < kGeneralMidiProgramCount ? std::string(kNames[zeroBasedProgram])
                                                                              : std::string("Unknown program");
}

bool try_drum_kit_name(int bankMsb, int bankLsb, int program, std::string& name, std::string& standard)
{
    name.clear();
    standard.clear();
    if (bankLsb != 0) return false;

    // GM2 standardized rhythm-set bank.
    if (bankMsb == 120) {
        standard = "GM2";
        switch (program) {
        case 0: name = "Standard Set"; break;
        case 8: name = "Room Set"; break;
        case 16: name = "Power Set"; break;
        case 24: name = "Electronic Set"; break;
        case 25: name = "Analog Set"; break;
        case 32: name = "Jazz Set"; break;
        case 40: name = "Brush Set"; break;
        case 48: name = "Orchestra Set"; break;
        case 56: name = "SFX Set"; break;
        default: break;
        }
        return !name.empty();
    }

    // Common GS rhythm-set program map. Program 1 is also the usual
    // GM-compatible standard kit when Bank 0/0 is observed.
    if (bankMsb == 0) {
        standard = program == 0 ? "GM/GS common" : "common GS";
        switch (program) {
        case 0: name = "Standard Set"; break;
        case 8: name = "Room Set"; break;
        case 16: name = "Power Set"; break;
        case 24: name = "Electronic Set"; break;
        case 25: name = "TR-808 Set"; break;
        case 32: name = "Jazz Set"; break;
        case 40: name = "Brush Set"; break;
        case 48: name = "Orchestra Set"; break;
        case 56: name = "SFX Set"; break;
        default: break;
        }
        return !name.empty();
    }

    // Yamaha XG drum/SFX banks.
    if (bankMsb == 127) {
        standard = "XG";
        switch (program) {
        case 0: name = "Standard Kit 1"; break;
        case 1: name = "Standard Kit 2"; break;
        case 8: name = "Room Kit"; break;
        case 16: name = "Rock Kit"; break;
        case 24: name = "Electro Kit"; break;
        case 25: name = "Analog Kit"; break;
        case 32: name = "Jazz Kit"; break;
        case 40: name = "Brush Kit"; break;
        case 48: name = "Classic Kit"; break;
        default: break;
        }
        return !name.empty();
    }
    if (bankMsb == 126 && (program == 0 || program == 1)) {
        standard = "XG";
        name = program == 0 ? "SFX Kit 1" : "SFX Kit 2";
        return true;
    }
    // v1 leaves both outputs null here.
    standard.clear();
    return false;
}

std::string channel_program_name(int zeroBasedChannel, int bankMsb, int bankLsb, int zeroBasedProgram)
{
    if (zeroBasedChannel != 9) return general_midi_program_name(zeroBasedProgram);
    std::string kit, standard;
    if (try_drum_kit_name(bankMsb, bankLsb, zeroBasedProgram, kit, standard)) return kit + " (" + standard + ")";
    return "Drum kit " + format::format_integer(static_cast<int64_t>(zeroBasedProgram) + 1);
}

std::string channel_program_reference(int zeroBasedChannel, int bankMsb, int bankLsb, int zeroBasedProgram)
{
    if (zeroBasedChannel != 9) return std::string();
    std::string kit, standard;
    std::string bank = bank_text(bankMsb) + "/" + bank_text(bankLsb);
    if (try_drum_kit_name(bankMsb, bankLsb, zeroBasedProgram, kit, standard))
        return standard + " percussion reference for Bank " + bank + ". Device-specific drum mappings can differ.";
    return "Channel 10 percussion fallback for unrecognized Bank " + bank + ". Device-specific drum mappings can differ.";
}

std::string describe_patch(int bankMsb, int bankLsb, int program)
{
    if (program < 0 || program >= kGeneralMidiProgramCount)
        return "No valid Program Change was found for this source lane.";
    std::string text = "Program " + format::format_integer(static_cast<int64_t>(program) + 1) + " " + kEmDash + " " +
        general_midi_program_name(program);
    if (bankMsb == 0 && bankLsb == 0) return text + " (General MIDI).";
    return text + " (General MIDI reference only; bank " + bank_text(bankMsb) + "/" + bank_text(bankLsb) +
        " may use a different sound).";
}

} // namespace app
} // namespace midibn
