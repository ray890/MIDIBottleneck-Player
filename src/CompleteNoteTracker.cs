using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum CompleteNoteEventKind
    {
        Other,
        NoteOn,
        NoteOff
    }

    // Tracks only currently unmatched note occurrences. It is indexed by the
    // fixed 16-channel / 128-key MIDI domain and allocates a small FIFO only
    // for a key that actually overlaps. No permanent per-event pairing data is
    // added to MidiEvent or the loaded song.
    internal sealed class CompleteNoteTracker
    {
        private const byte Accepted = 1;
        private const byte RejectedByCompleteNotePolicy = 2;
        private const byte RejectedByOtherPolicy = 3;
        private readonly Queue<byte>[] _occurrences = new Queue<byte>[16 * 128];

        internal static CompleteNoteEventKind Classify(MidiEvent midiEvent)
        {
            if (midiEvent == null || midiEvent.DataLength < 3)
                return CompleteNoteEventKind.Other;
            int command = midiEvent.Status & 0xF0;
            if (command == 0x90)
                return midiEvent.GetDataByte(2) == 0 ? CompleteNoteEventKind.NoteOff : CompleteNoteEventKind.NoteOn;
            return command == 0x80 ? CompleteNoteEventKind.NoteOff : CompleteNoteEventKind.Other;
        }

        internal void RecordNoteOn(MidiEvent midiEvent, bool accepted, bool rejectedByCompleteNotePolicy)
        {
            int slot = Slot(midiEvent);
            if (slot < 0) return;
            Queue<byte> queue = _occurrences[slot];
            if (queue == null) _occurrences[slot] = queue = new Queue<byte>(2);
            queue.Enqueue(accepted ? Accepted :
                rejectedByCompleteNotePolicy ? RejectedByCompleteNotePolicy : RejectedByOtherPolicy);
        }

        // FIFO occurrence pairing is deterministic for overlapping notes on
        // the same channel/key. An unmatched NoteOff is never suppressed.
        internal bool ShouldSuppressNoteOff(MidiEvent midiEvent)
        {
            int slot = Slot(midiEvent);
            if (slot < 0) return false;
            Queue<byte> queue = _occurrences[slot];
            if (queue == null || queue.Count == 0) return false;
            byte occurrence = queue.Dequeue();
            return occurrence == RejectedByCompleteNotePolicy;
        }

        private static int Slot(MidiEvent midiEvent)
        {
            if (midiEvent == null || midiEvent.DataLength < 2) return -1;
            int channel = midiEvent.Status & 0x0F;
            int key = midiEvent.GetDataByte(1) & 0x7F;
            return channel * 128 + key;
        }
    }
}
