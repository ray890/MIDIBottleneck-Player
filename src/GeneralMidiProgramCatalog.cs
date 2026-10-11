using System;
using System.Globalization;

namespace MidiBottleneck
{
    internal static class GeneralMidiProgramCatalog
    {
        private static readonly string[] Names = new string[]
        {
            "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
            "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
            "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
            "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
            "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
            "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
            "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag Pipe", "Fiddle", "Shanai",
            "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"
        };

        internal static string Name(int zeroBasedProgram)
        {
            return zeroBasedProgram >= 0 && zeroBasedProgram < Names.Length
                ? Names[zeroBasedProgram] : "Unknown program";
        }

        internal static string ChannelProgramName(int zeroBasedChannel, int bankMsb,
            int bankLsb, int zeroBasedProgram)
        {
            if (zeroBasedChannel != 9) return Name(zeroBasedProgram);
            string kit;
            string standard;
            return TryDrumKitName(bankMsb, bankLsb, zeroBasedProgram, out kit, out standard)
                ? kit + " (" + standard + ")"
                : "Drum kit " + (zeroBasedProgram + 1).ToString(CultureInfo.CurrentCulture);
        }

        internal static string ChannelProgramReference(int zeroBasedChannel, int bankMsb,
            int bankLsb, int zeroBasedProgram)
        {
            if (zeroBasedChannel != 9) return String.Empty;
            string kit;
            string standard;
            string bank = BankText(bankMsb) + "/" + BankText(bankLsb);
            if (TryDrumKitName(bankMsb, bankLsb, zeroBasedProgram, out kit, out standard))
                return standard + " percussion reference for Bank " + bank +
                    ". Device-specific drum mappings can differ.";
            return "Channel 10 percussion fallback for unrecognized Bank " + bank +
                ". Device-specific drum mappings can differ.";
        }

        private static bool TryDrumKitName(int bankMsb, int bankLsb, int program,
            out string name, out string standard)
        {
            name = null;
            standard = null;
            if (bankLsb != 0) return false;

            // GM2 standardized rhythm-set bank.
            if (bankMsb == 120)
            {
                standard = "GM2";
                switch (program)
                {
                    case 0: name = "Standard Set"; break;
                    case 8: name = "Room Set"; break;
                    case 16: name = "Power Set"; break;
                    case 24: name = "Electronic Set"; break;
                    case 25: name = "Analog Set"; break;
                    case 32: name = "Jazz Set"; break;
                    case 40: name = "Brush Set"; break;
                    case 48: name = "Orchestra Set"; break;
                    case 56: name = "SFX Set"; break;
                }
                return name != null;
            }

            // Common GS rhythm-set program map. Program 1 is also the usual
            // GM-compatible standard kit when Bank 0/0 is observed.
            if (bankMsb == 0)
            {
                standard = program == 0 ? "GM/GS common" : "common GS";
                switch (program)
                {
                    case 0: name = "Standard Set"; break;
                    case 8: name = "Room Set"; break;
                    case 16: name = "Power Set"; break;
                    case 24: name = "Electronic Set"; break;
                    case 25: name = "TR-808 Set"; break;
                    case 32: name = "Jazz Set"; break;
                    case 40: name = "Brush Set"; break;
                    case 48: name = "Orchestra Set"; break;
                    case 56: name = "SFX Set"; break;
                }
                return name != null;
            }

            // Yamaha XG drum/SFX banks.
            if (bankMsb == 127)
            {
                standard = "XG";
                switch (program)
                {
                    case 0: name = "Standard Kit 1"; break;
                    case 1: name = "Standard Kit 2"; break;
                    case 8: name = "Room Kit"; break;
                    case 16: name = "Rock Kit"; break;
                    case 24: name = "Electro Kit"; break;
                    case 25: name = "Analog Kit"; break;
                    case 32: name = "Jazz Kit"; break;
                    case 40: name = "Brush Kit"; break;
                    case 48: name = "Classic Kit"; break;
                }
                return name != null;
            }
            if (bankMsb == 126 && (program == 0 || program == 1))
            {
                standard = "XG";
                name = program == 0 ? "SFX Kit 1" : "SFX Kit 2";
                return true;
            }
            return false;
        }

        private static string BankText(int value)
        {
            return value < 0 ? "?" : value.ToString(CultureInfo.CurrentCulture);
        }

        internal static string DescribePatch(MidiPatchIdentity patch)
        {
            if (patch.Program < 0 || patch.Program >= Names.Length)
                return "No valid Program Change was found for this source lane.";
            string program = "Program " + (patch.Program + 1).ToString(
                CultureInfo.CurrentCulture) + " — " + Name(patch.Program);
            if (patch.IsGeneralMidiBank) return program + " (General MIDI).";
            string bank = (patch.BankMsb < 0 ? "?" : patch.BankMsb.ToString(
                CultureInfo.CurrentCulture)) + "/" +
                (patch.BankLsb < 0 ? "?" : patch.BankLsb.ToString(
                CultureInfo.CurrentCulture));
            return program + " (General MIDI reference only; bank " + bank +
                " may use a different sound).";
        }
    }
}
