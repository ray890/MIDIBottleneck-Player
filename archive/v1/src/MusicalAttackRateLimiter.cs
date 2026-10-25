using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Deterministic NoteOn-only progressive pacer. Accepted and rejected source
    // occurrences remain paired until their releases arrive; Playback and
    // Analysis both consume this exact logical-time state machine.
    internal sealed class MusicalAttackRateLimiter
    {
        private const int ChannelCount = 16;
        private const int PitchCount = 128;
        private const int SlotCount = ChannelCount * PitchCount;
        private const int ChunkSize = 4096;
        private const int MaximumLiveOccurrences = 1000000;
        private const long MicrosecondsPerSecond = 1000000;

        private struct Occurrence
        {
            internal bool InUse;
            internal bool Accepted;
            internal int Channel;
            internal int Pitch;
            internal int Previous;
            internal int Next;
            internal int NextFree;
        }

        private struct Candidate
        {
            internal int TickIndex;
            internal int Pitch;
            internal int Velocity;
            internal int Track;
            internal int Channel;
            internal bool Accepted;
        }

        private readonly long _rate;
        private readonly List<Occurrence[]> _chunks = new List<Occurrence[]>();
        private readonly int[] _head = new int[SlotCount];
        private readonly int[] _tail = new int[SlotCount];
        private readonly List<MidiEventView> _tickEvents = new List<MidiEventView>();
        private readonly List<Candidate> _candidates = new List<Candidate>();
        private readonly ProgressiveRatePacer _pacer;
        private readonly MusicalNotePrioritySelector _musicalPriority =
            new MusicalNotePrioritySelector();
        private MusicalNotePriorityCandidate[] _musicalCandidates =
            new MusicalNotePriorityCandidate[16];
        private int[] _activeCandidateIndices = new int[16];
        private int _free = -1;
        private int _nextUnused;
        private int _live;
        private int _fairnessCursor;
        private long _rejectedAttacks;
        private long _suppressedReleases;

        internal MusicalAttackRateLimiter(long attacksPerSecond)
        {
            if (attacksPerSecond < 0 || attacksPerSecond > 9999999)
                throw new ArgumentOutOfRangeException("attacksPerSecond");
            _rate = attacksPerSecond;
            _pacer = new ProgressiveRatePacer(attacksPerSecond, MicrosecondsPerSecond);
            for (int slot = 0; slot < SlotCount; slot++) _head[slot] = _tail[slot] = -1;
        }

        internal long AttacksPerSecond { get { return _rate; } }
        internal long AvailableStartsForTesting(long timestamp, long maximum)
        { return _pacer.AvailableSlots(timestamp, maximum); }
        internal long NextPacedWholeForTesting { get { return _pacer.NextWholeForTesting; } }
        internal long NextPacedRemainderForTesting { get { return _pacer.NextRemainderForTesting; } }
        internal int LiveOccurrenceCountForTesting { get { return _live; } }
        internal int OccurrenceSegmentCountForTesting { get { return _chunks.Count; } }

        internal void BeginSourceTick(long absoluteTick)
        {
            if (_tickEvents.Count != 0) throw new InvalidOperationException("The previous source tick was not completed.");
        }

        internal void Admit(MidiEventView midiEvent)
        {
            _tickEvents.Add(midiEvent);
        }

        internal int EndSourceTick(List<MidiEventView> emissions)
        {
            if (emissions == null) throw new ArgumentNullException("emissions");
            emissions.Clear();
            if (_tickEvents.Count == 0) return 0;
            long timeMicroseconds = Math.Max(0, _tickEvents[0].IntendedMicroseconds);
            BuildAttackPriorities();
            SelectAttacks(timeMicroseconds);

            int candidateIndex = 0;
            for (int index = 0; index < _tickEvents.Count; index++)
            {
                MidiEventView midiEvent = _tickEvents[index];
                int pitch;
                bool noteOn;
                if (!PerNoteIntervalGate.TryClassify(midiEvent, out pitch, out noteOn))
                {
                    emissions.Add(midiEvent);
                    continue;
                }
                if (noteOn)
                {
                    while (_candidates[candidateIndex].TickIndex != index) candidateIndex++;
                    bool accepted = _candidates[candidateIndex].Accepted;
                    AppendOccurrence(midiEvent.Channel, pitch, accepted);
                    if (accepted) emissions.Add(midiEvent);
                    else _rejectedAttacks++;
                    candidateIndex++;
                    continue;
                }
                int node = RemoveOldest(midiEvent.Channel, pitch);
                if (node < 0)
                    emissions.Add(midiEvent);
                else
                {
                    Occurrence occurrence = Get(node);
                    if (occurrence.Accepted) emissions.Add(midiEvent);
                    else _suppressedReleases++;
                    Free(node);
                }
            }
            _tickEvents.Clear();
            _candidates.Clear();
            return emissions.Count;
        }

        internal void CompleteSource()
        {
            _tickEvents.Clear();
            _candidates.Clear();
            for (int slot = 0; slot < SlotCount; slot++)
            {
                int node = _head[slot];
                while (node >= 0)
                {
                    int next = Get(node).Next;
                    Free(node);
                    node = next;
                }
                _head[slot] = _tail[slot] = -1;
            }
        }

        internal void RetireChannel(int channel)
        {
            if (channel < 0 || channel >= ChannelCount) return;
            for (int pitch = 0; pitch < PitchCount; pitch++)
            {
                int slot = Slot(channel, pitch);
                int node = _head[slot];
                while (node >= 0)
                {
                    int next = Get(node).Next;
                    Free(node);
                    node = next;
                }
                _head[slot] = _tail[slot] = -1;
            }
        }

        internal long TakeRejectedAttacks()
        {
            long value = _rejectedAttacks;
            _rejectedAttacks = 0;
            return value;
        }

        internal long TakeSuppressedReleases()
        {
            long value = _suppressedReleases;
            _suppressedReleases = 0;
            return value;
        }

        private void BuildAttackPriorities()
        {
            _candidates.Clear();
            for (int index = 0; index < _tickEvents.Count; index++)
            {
                MidiEventView midiEvent = _tickEvents[index];
                int pitch;
                bool noteOn;
                if (!PerNoteIntervalGate.TryClassify(midiEvent, out pitch, out noteOn) || !noteOn) continue;
                _candidates.Add(new Candidate { TickIndex = index,
                    Pitch = pitch,
                    Velocity = midiEvent.GetDataByte(2) & 0x7F,
                    Track = midiEvent.Track, Channel = midiEvent.Channel });
            }
        }

        private void EnsureMusicalCapacity(int count)
        {
            if (_musicalCandidates.Length >= count) return;
            int capacity = _musicalCandidates.Length;
            while (capacity < count && capacity <= Int32.MaxValue / 2) capacity *= 2;
            if (capacity < count) capacity = count;
            _musicalCandidates = new MusicalNotePriorityCandidate[capacity];
            _activeCandidateIndices = new int[capacity];
        }

        private void SelectAttacks(long timeMicroseconds)
        {
            int candidateCount = _candidates.Count;
            if (candidateCount == 0) return;
            EnsureMusicalCapacity(candidateCount);
            long available = _pacer.AvailableSlots(timeMicroseconds, candidateCount);
            int admitted = (int)Math.Min(candidateCount, available);
            for (int index = 0; index < _candidates.Count; index++)
            {
                Candidate candidate = _candidates[index];
                candidate.Accepted = true;
                _candidates[index] = candidate;
                _activeCandidateIndices[index] = index;
            }
            int activeCount = candidateCount;
            while (activeCount > admitted)
            {
                for (int index = 0; index < activeCount; index++)
                {
                    Candidate candidate = _candidates[_activeCandidateIndices[index]];
                    _musicalCandidates[index] = new MusicalNotePriorityCandidate
                    {
                        Pitch = candidate.Pitch,
                        Velocity = candidate.Velocity,
                        Track = candidate.Track,
                        Channel = candidate.Channel,
                        SourceOrder = candidate.TickIndex,
                        IntendedMicroseconds = timeMicroseconds,
                        // The streaming limiter has not seen the future paired
                        // release yet. Unknown durations remain equal rather
                        // than guessing from wall-clock dispatch timing.
                        DurationMicroseconds = Int64.MaxValue
                    };
                }
                int victim = _musicalPriority.SelectLeastImportant(
                    _musicalCandidates, activeCount, _fairnessCursor);
                _fairnessCursor = _musicalPriority.NextFairnessCursor(
                    _musicalCandidates[victim]);
                int candidateIndex = _activeCandidateIndices[victim];
                Candidate rejected = _candidates[candidateIndex];
                rejected.Accepted = false;
                _candidates[candidateIndex] = rejected;
                activeCount--;
                _activeCandidateIndices[victim] = _activeCandidateIndices[activeCount];
            }
            if (_rate != 0)
                for (int index = 0; index < candidateCount; index++)
                    if (_candidates[index].Accepted && !_pacer.TryConsume(timeMicroseconds))
                        throw new InvalidOperationException("The Note start pacing decision lost its due slot.");
        }

        private void AppendOccurrence(int channel, int pitch, bool accepted)
        {
            if (_live >= MaximumLiveOccurrences)
                throw new InvalidOperationException("The Note start rate gate has more than 1,000,000 unmatched source occurrences.");
            int node;
            if (_free >= 0)
            {
                node = _free;
                _free = Get(node).NextFree;
            }
            else
            {
                node = _nextUnused++;
                if (node / ChunkSize == _chunks.Count) _chunks.Add(new Occurrence[ChunkSize]);
            }
            int slot = Slot(channel, pitch);
            Occurrence occurrence = new Occurrence { InUse = true, Accepted = accepted,
                Channel = channel, Pitch = pitch, Previous = _tail[slot], Next = -1, NextFree = -1 };
            if (_tail[slot] >= 0)
            {
                Occurrence tail = Get(_tail[slot]);
                tail.Next = node;
                Set(_tail[slot], tail);
            }
            else _head[slot] = node;
            _tail[slot] = node;
            Set(node, occurrence);
            _live++;
        }

        private int RemoveOldest(int channel, int pitch)
        {
            int slot = Slot(channel, pitch);
            int node = _head[slot];
            if (node < 0) return -1;
            Occurrence occurrence = Get(node);
            _head[slot] = occurrence.Next;
            if (occurrence.Next >= 0)
            {
                Occurrence next = Get(occurrence.Next);
                next.Previous = -1;
                Set(occurrence.Next, next);
            }
            else _tail[slot] = -1;
            occurrence.Previous = occurrence.Next = -1;
            Set(node, occurrence);
            return node;
        }

        private void Free(int node)
        {
            Occurrence occurrence = Get(node);
            if (!occurrence.InUse) return;
            Set(node, new Occurrence { NextFree = _free, Previous = -1, Next = -1 });
            _free = node;
            _live--;
        }

        private Occurrence Get(int node) { return _chunks[node / ChunkSize][node % ChunkSize]; }
        private void Set(int node, Occurrence occurrence) { _chunks[node / ChunkSize][node % ChunkSize] = occurrence; }
        private static int Slot(int channel, int pitch) { return channel * PitchCount + pitch; }
    }
}
