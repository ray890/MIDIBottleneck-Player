using System;

namespace MidiBottleneck
{
    // A deliberately explicit diagnostic sink.  It implements the same output
    // contract as native backends so the scheduler, simulation and statistics
    // paths remain identical, while Send itself is an allocation-free no-op.
    internal sealed class NullMidiOutput : IMidiOutput, IMidiOutputContext, IDisposable
    {
        internal const string DisplayName = "None (no output)";

        public string SourceFile { get; set; }

        public void Open() { }
        public void Close() { SourceFile = null; }
        public void Send(MidiEvent midiEvent) { }
        public void Panic() { }
        public void Reset() { }
        public void Dispose() { Close(); }
    }

    // Keep the synthetic selector item distinct from MidiOutputDeviceInfo so it
    // can never be mistaken for, or offset, a native WinMM device identifier.
    internal sealed class NoMidiOutputSelection
    {
        internal static readonly NoMidiOutputSelection Instance = new NoMidiOutputSelection();
        private NoMidiOutputSelection() { }
        public override string ToString() { return NullMidiOutput.DisplayName; }
    }
}
