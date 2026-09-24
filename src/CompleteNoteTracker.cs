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
        private const byte RejectedBySourceFilter = 4;
        private struct Occurrence
        {
            internal int AttackIndex;
            internal byte State;
        }

        internal struct NoteOffMatch
        {
            internal int AttackIndex;
            internal bool Accepted;
            internal bool Suppress;
            internal bool Filtered;
        }

        private readonly Queue<Occurrence>[] _occurrences = new Queue<Occurrence>[16 * 128];
        // These sets contain only source occurrences still awaiting their release.
        // An evicted attack is removed from both sets when its release arrives.
        private readonly HashSet<int> _unmatchedAccepted = new HashSet<int>();
        private readonly HashSet<int> _evictedUnmatched = new HashSet<int>();
        private readonly HashSet<int> _filteredUnmatched = new HashSet<int>();

        internal static CompleteNoteEventKind Classify(MidiEventView midiEvent)
        {
            if (!midiEvent.IsValid || midiEvent.DataLength < 3)
                return CompleteNoteEventKind.Other;
            int command = midiEvent.Status & 0xF0;
            if (command == 0x90)
                return midiEvent.GetDataByte(2) == 0 ? CompleteNoteEventKind.NoteOff : CompleteNoteEventKind.NoteOn;
            return command == 0x80 ? CompleteNoteEventKind.NoteOff : CompleteNoteEventKind.Other;
        }

        internal static bool IsSafetyControl(MidiEventView midiEvent)
        {
            if (!midiEvent.IsValid || midiEvent.DataLength < 3 ||
                (midiEvent.Status & 0xF0) != 0xB0) return false;
            int controller = midiEvent.GetDataByte(1);
            int value = midiEvent.GetDataByte(2);
            return (controller == 64 && value < 64) || (controller >= 120 && controller <= 127);
        }

        internal void RecordNoteOn(MidiEventView midiEvent, bool accepted, bool rejectedByCompleteNotePolicy)
        {
            RecordNoteOn(midiEvent, -1, accepted, rejectedByCompleteNotePolicy);
        }

        internal void RecordNoteOn(MidiEventView midiEvent, int attackIndex,
            bool accepted, bool rejectedByCompleteNotePolicy)
        {
            int slot = Slot(midiEvent);
            if (slot < 0) return;
            Queue<Occurrence> queue = _occurrences[slot];
            if (queue == null) _occurrences[slot] = queue = new Queue<Occurrence>(2);
            queue.Enqueue(new Occurrence
            {
                AttackIndex = attackIndex,
                State = accepted ? Accepted :
                    rejectedByCompleteNotePolicy ? RejectedByCompleteNotePolicy : RejectedByOtherPolicy
            });
            if (accepted && attackIndex >= 0) _unmatchedAccepted.Add(attackIndex);
        }

        internal void RecordFilteredNoteOn(MidiEventView midiEvent, int attackIndex)
        {
            int slot = Slot(midiEvent);
            if (slot < 0) return;
            Queue<Occurrence> queue = _occurrences[slot];
            if (queue == null) _occurrences[slot] = queue = new Queue<Occurrence>(2);
            queue.Enqueue(new Occurrence { AttackIndex = attackIndex, State = RejectedBySourceFilter });
        }

        // FIFO occurrence pairing is deterministic for overlapping notes on
        // the same channel/key. An unmatched NoteOff is never suppressed.
        internal bool ShouldSuppressNoteOff(MidiEventView midiEvent)
        {
            return TakeNoteOff(midiEvent).Suppress;
        }

        internal NoteOffMatch TakeNoteOff(MidiEventView midiEvent)
        {
            int slot = Slot(midiEvent);
            if (slot < 0) return new NoteOffMatch { AttackIndex = -1 };
            Queue<Occurrence> queue = _occurrences[slot];
            if (queue == null || queue.Count == 0) return new NoteOffMatch { AttackIndex = -1 };
            Occurrence occurrence = queue.Dequeue();
            bool evicted = occurrence.AttackIndex >= 0 && _evictedUnmatched.Remove(occurrence.AttackIndex);
            bool filtered = occurrence.AttackIndex >= 0 && _filteredUnmatched.Remove(occurrence.AttackIndex);
            if (occurrence.AttackIndex >= 0) _unmatchedAccepted.Remove(occurrence.AttackIndex);
            return new NoteOffMatch
            {
                AttackIndex = occurrence.AttackIndex,
                Accepted = occurrence.State == Accepted && !evicted && !filtered,
                Suppress = occurrence.State == RejectedByCompleteNotePolicy ||
                    occurrence.State == RejectedBySourceFilter || evicted || filtered,
                Filtered = occurrence.State == RejectedBySourceFilter || filtered
            };
        }

        internal void MarkUnsentAttackEvicted(int attackIndex)
        {
            if (_unmatchedAccepted.Contains(attackIndex)) _evictedUnmatched.Add(attackIndex);
        }

        internal void MarkUnsentAttackFiltered(int attackIndex)
        {
            if (_unmatchedAccepted.Contains(attackIndex))
            {
                _evictedUnmatched.Remove(attackIndex);
                _filteredUnmatched.Add(attackIndex);
            }
        }

        private static int Slot(MidiEventView midiEvent)
        {
            if (!midiEvent.IsValid || midiEvent.DataLength < 2) return -1;
            int channel = midiEvent.Status & 0x0F;
            int key = midiEvent.GetDataByte(1) & 0x7F;
            return channel * 128 + key;
        }
    }
}
