// The General MIDI program catalog and the Channel 10 drum-kit naming rules
// (v1 GeneralMidiProgramCatalog.cs), for the Channels window and the Track
// routing preview. All text is UTF-8. Wording and data are v1's, byte for byte;
// the tables in v2/harness/expected/channels-ui were written by v1's own code.
#pragma once

#include <string>

namespace midibn {
namespace app {

const int kGeneralMidiProgramCount = 128;

// v1 GeneralMidiProgramCatalog.Name: the instrument of a zero-based program,
// "Unknown program" outside 0..127.
std::string general_midi_program_name(int zeroBasedProgram);

// v1 ChannelProgramName: melodic channels give the instrument name; Channel 10
// (zero-based channel 9) gives "<kit> (<standard>)" for a recognised GM2, GS or
// XG drum kit and otherwise "Drum kit <program + 1>". Banks below 0 are unknown.
std::string channel_program_name(int zeroBasedChannel, int bankMsb, int bankLsb, int zeroBasedProgram);

// v1 ChannelProgramReference: the explanation shown in the Program cell's tooltip
// on Channel 10 (empty on every other channel).
std::string channel_program_reference(int zeroBasedChannel, int bankMsb, int bankLsb, int zeroBasedProgram);

// v1 TryDrumKitName: the recognised drum kit of a bank and program.
bool try_drum_kit_name(int bankMsb, int bankLsb, int zeroBasedProgram, std::string& name, std::string& standard);

// v1 DescribePatch (Track routing preview): "Program 25 - Acoustic Guitar (nylon) (General MIDI)."
// (the dash is U+2014 in the real text).
std::string describe_patch(int bankMsb, int bankLsb, int zeroBasedProgram);

} // namespace app
} // namespace midibn
