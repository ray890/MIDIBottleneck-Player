using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Exact progressive admission for total transmitted MIDI. Ordinary
    // capacity advances only when a dispatch consumes a due rational slot.
    // Protected excess is sent but never advances or rewrites that schedule.
    internal sealed class OutputRateGate
    {
        private const int ChannelCount = 16;
        private const int PitchCount = 128;
        private const int SlotCount = ChannelCount * PitchCount;
        private const int ChunkSize = 4096;
        private const int MaximumLiveOccurrences = 1000000;

        private enum PlanKind : byte
        {
            RequiredRelease,
            OrdinaryMessage,
            Suppressed,
            CandidateAttack,
            CandidateRelease
        }

        private struct Occurrence
        {
            internal bool InUse;
            internal bool Accepted;
            internal int CandidateIndex;
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
            internal int MessageCost;
            internal int OccurrenceNode;
            internal bool Accepted;
        }

        private struct EventPlan
        {
            internal PlanKind Kind;
            internal int CandidateIndex;
            internal bool Accepted;
        }

        private readonly long _rate;
        private readonly ProgressiveRatePacer _pacer;
        private readonly List<Occurrence[]> _chunks = new List<Occurrence[]>();
        private readonly int[] _head = new int[SlotCount];
        private readonly int[] _tail = new int[SlotCount];
        private readonly List<MidiEventView> _tickEvents = new List<MidiEventView>();
        private readonly List<EventPlan> _plans = new List<EventPlan>();
        private readonly List<Candidate> _candidates = new List<Candidate>();
        private readonly Queue<bool> _pendingRequiredRelease = new Queue<bool>();
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
        private long _rejectedOtherMessages;

        internal OutputRateGate(long eventsPerSecond, long clockUnitsPerSecond)
        {
            if (eventsPerSecond < 0 || eventsPerSecond > 9999999)
                throw new ArgumentOutOfRangeException("eventsPerSecond");
            if (clockUnitsPerSecond <= 0)
                throw new ArgumentOutOfRangeException("clockUnitsPerSecond");
            _rate = eventsPerSecond;
            _pacer = new ProgressiveRatePacer(eventsPerSecond, clockUnitsPerSecond);
            for (int slot = 0; slot < SlotCount; slot++)
                _head[slot] = _tail[slot] = -1;
        }

        internal long EventsPerSecond { get { return _rate; } }
        internal long NextPacedWholeForTesting { get { return _pacer.NextWholeForTesting; } }
        internal long NextPacedRemainderForTesting { get { return _pacer.NextRemainderForTesting; } }
        internal int LiveOccurrenceCountForTesting { get { return _live; } }
        internal long AvailableEvents(long timestamp)
        {
            return _pacer.AvailableSlots(timestamp, _rate == 0 ? Int64.MaxValue : _rate);
        }
        internal long AvailableEvents(long timestamp, long maximum)
        {
            return _pacer.AvailableSlots(timestamp, maximum);
        }

        internal void BeginSourceTick(long absoluteTick)
        {
            if (_tickEvents.Count != 0)
                throw new InvalidOperationException("The previous output-rate source tick was not completed.");
        }

        internal void Admit(MidiEventView midiEvent)
        {
            _tickEvents.Add(midiEvent);
        }

        internal int EndSourceTick(List<MidiEventView> emissions, long decisionTimestamp)
        {
            if (emissions == null) throw new ArgumentNullException("emissions");
            emissions.Clear();
            if (_tickEvents.Count == 0) return 0;
            BuildPlans();
            SelectAttacks(decisionTimestamp);

            for (int index = 0; index < _tickEvents.Count; index++)
            {
                EventPlan plan = _plans[index];
                if (plan.Kind == PlanKind.RequiredRelease ||
                    plan.Kind == PlanKind.OrdinaryMessage && plan.Accepted)
                {
                    emissions.Add(_tickEvents[index]);
                    QueueDispatch(plan.Kind == PlanKind.RequiredRelease);
                    continue;
                }
                if (plan.Kind == PlanKind.OrdinaryMessage)
                {
                    _rejectedOtherMessages++;
                    continue;
                }
                if (plan.Kind == PlanKind.Suppressed)
                {
                    _suppressedReleases++;
                    continue;
                }
                Candidate candidate = _candidates[plan.CandidateIndex];
                if (candidate.Accepted)
                {
                    emissions.Add(_tickEvents[index]);
                    QueueDispatch(plan.Kind == PlanKind.CandidateRelease);
                }
                else if (plan.Kind == PlanKind.CandidateAttack)
                    _rejectedAttacks++;
                else
                    _suppressedReleases++;
            }

            for (int index = 0; index < _candidates.Count; index++)
            {
                Candidate candidate = _candidates[index];
                if (candidate.OccurrenceNode < 0) continue;
                Occurrence occurrence = Get(candidate.OccurrenceNode);
                if (!occurrence.InUse) continue;
                occurrence.Accepted = candidate.Accepted;
                occurrence.CandidateIndex = -1;
                Set(candidate.OccurrenceNode, occurrence);
            }

            _tickEvents.Clear();
            _plans.Clear();
            _candidates.Clear();
            return emissions.Count;
        }

        // Returns true only for a required admitted-note release whose source
        // timing could not consume an ordinary slot. Routine state/controllers
        // never bypass the gate; cleanup traffic does not call this method.
        internal bool RecordDispatch(long timestamp, bool requiredRelease)
        {
            if (_pendingRequiredRelease.Count != 0)
            {
                requiredRelease = _pendingRequiredRelease.Dequeue();
                _pacer.CompleteReservation(timestamp);
                return false;
            }
            if (_rate == 0) return false;
            bool consumed = _pacer.TryConsume(timestamp);
            return requiredRelease && !consumed;
        }

        internal bool RecordExceptionalDispatch(long timestamp)
        {
            if (_rate == 0) return false;
            return !_pacer.TryConsume(timestamp);
        }

        internal void CompleteSource()
        {
            _tickEvents.Clear();
            _plans.Clear();
            _candidates.Clear();
            ClearOccurrences();
        }

        internal void RetireChannel(int channel)
        {
            if (channel < 0 || channel >= ChannelCount) return;
            for (int pitch = 0; pitch < PitchCount; pitch++)
                ClearSlot(Slot(channel, pitch));
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

        internal long TakeRejectedOtherMessages()
        {
            long value = _rejectedOtherMessages;
            _rejectedOtherMessages = 0;
            return value;
        }

        internal static bool IsProtectedTraffic(MidiEventView midiEvent)
        {
            return CompleteNoteTracker.Classify(midiEvent) != CompleteNoteEventKind.NoteOn;
        }

        private void BuildPlans()
        {
            _plans.Clear();
            _candidates.Clear();
            for (int index = 0; index < _tickEvents.Count; index++)
            {
                MidiEventView midiEvent = _tickEvents[index];
                CompleteNoteEventKind kind = CompleteNoteTracker.Classify(midiEvent);
                if (kind == CompleteNoteEventKind.Other)
                {
                    _plans.Add(new EventPlan { Kind = PlanKind.OrdinaryMessage,
                        CandidateIndex = -1, Accepted = true });
                    continue;
                }
                int pitch = midiEvent.GetDataByte(1) & 0x7F;
                if (kind == CompleteNoteEventKind.NoteOn)
                {
                    int candidateIndex = _candidates.Count;
                    int node = AppendOccurrence(midiEvent.Channel, pitch, candidateIndex);
                    _candidates.Add(new Candidate
                    {
                        TickIndex = index,
                        Pitch = pitch,
                        Velocity = midiEvent.GetDataByte(2) & 0x7F,
                        Track = midiEvent.Track,
                        Channel = midiEvent.Channel,
                        MessageCost = 1,
                        OccurrenceNode = node,
                        Accepted = true
                    });
                    _plans.Add(new EventPlan
                    {
                        Kind = PlanKind.CandidateAttack,
                        CandidateIndex = candidateIndex
                    });
                    continue;
                }

                Occurrence occurrence;
                if (!RemoveOldest(midiEvent.Channel, pitch, out occurrence))
                {
                    _plans.Add(new EventPlan { Kind = PlanKind.OrdinaryMessage,
                        CandidateIndex = -1, Accepted = true });
                    continue;
                }
                if (occurrence.CandidateIndex >= 0)
                {
                    Candidate candidate = _candidates[occurrence.CandidateIndex];
                    candidate.MessageCost++;
                    candidate.OccurrenceNode = -1;
                    _candidates[occurrence.CandidateIndex] = candidate;
                    _plans.Add(new EventPlan
                    {
                        Kind = PlanKind.CandidateRelease,
                        CandidateIndex = occurrence.CandidateIndex
                    });
                }
                else
                    _plans.Add(new EventPlan
                    {
                        Kind = occurrence.Accepted ? PlanKind.RequiredRelease : PlanKind.Suppressed,
                        CandidateIndex = -1
                    });
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

        private void SelectAttacks(long decisionTimestamp)
        {
            int candidateCount = _candidates.Count;
            long requiredReleases = 0;
            long ordinaryMessages = 0;
            for (int index = 0; index < _plans.Count; index++)
                if (_plans[index].Kind == PlanKind.RequiredRelease) requiredReleases++;
                else if (_plans[index].Kind == PlanKind.OrdinaryMessage) ordinaryMessages++;
            long newAvailable = _pacer.AvailableSlots(decisionTimestamp,
                ordinaryMessages + candidateCount * 2L);
            if (ordinaryMessages == 0 && candidateCount != 0 && newAvailable == 1 &&
                _pacer.RollingAvailableSlots(decisionTimestamp, 2) >= 2)
                newAvailable = 2;
            long available = requiredReleases + newAvailable;
            long ordinaryAccepted = Math.Max(0, Math.Min(ordinaryMessages,
                available - requiredReleases));
            for (int index = 0; index < _plans.Count; index++)
                if (_plans[index].Kind == PlanKind.OrdinaryMessage)
                {
                    EventPlan plan = _plans[index];
                    plan.Accepted = ordinaryAccepted-- > 0;
                    _plans[index] = plan;
                }
            long planned = requiredReleases + Math.Min(ordinaryMessages,
                Math.Max(0, available - requiredReleases));
            if (candidateCount == 0)
            {
                Reserve(decisionTimestamp, planned - requiredReleases);
                return;
            }
            EnsureMusicalCapacity(candidateCount);
            for (int index = 0; index < candidateCount; index++)
            {
                Candidate candidate = _candidates[index];
                candidate.Accepted = true;
                _candidates[index] = candidate;
                _activeCandidateIndices[index] = index;
                planned += candidate.MessageCost + (candidate.OccurrenceNode >= 0 ? 1 : 0);
            }
            int activeCount = candidateCount;
            while (activeCount > 0 && planned > available)
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
                        IntendedMicroseconds = _tickEvents[candidate.TickIndex].IntendedMicroseconds,
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
                planned -= rejected.MessageCost + (rejected.OccurrenceNode >= 0 ? 1 : 0);
                activeCount--;
                _activeCandidateIndices[victim] = _activeCandidateIndices[activeCount];
            }
            Reserve(decisionTimestamp, planned - requiredReleases);
        }

        private void Reserve(long decisionTimestamp, long count)
        {
            bool usedOccurrenceExtension = false;
            for (long index = 0; index < count; index++)
                if (!_pacer.TryReserve(decisionTimestamp))
                {
                    if (usedOccurrenceExtension || !_pacer.TryReserveAhead(decisionTimestamp))
                        throw new InvalidOperationException("The Output rate pacing decision lost its reserved slot.");
                    usedOccurrenceExtension = true;
                }
        }

        private void QueueDispatch(bool requiredRelease)
        {
            if (_rate == 0) return;
            _pendingRequiredRelease.Enqueue(requiredRelease);
        }

        private int AppendOccurrence(int channel, int pitch, int candidateIndex)
        {
            if (_live >= MaximumLiveOccurrences)
                throw new InvalidOperationException("The Output rate gate has more than 1,000,000 unmatched source occurrences.");
            int node;
            if (_free >= 0)
            {
                node = _free;
                _free = Get(node).NextFree;
            }
            else
            {
                node = _nextUnused++;
                if (node / ChunkSize == _chunks.Count)
                    _chunks.Add(new Occurrence[ChunkSize]);
            }
            int slot = Slot(channel, pitch);
            Occurrence occurrence = new Occurrence
            {
                InUse = true,
                Accepted = true,
                CandidateIndex = candidateIndex,
                Channel = channel,
                Pitch = pitch,
                Previous = _tail[slot],
                Next = -1,
                NextFree = -1
            };
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
            return node;
        }

        private bool RemoveOldest(int channel, int pitch, out Occurrence occurrence)
        {
            int slot = Slot(channel, pitch);
            int node = _head[slot];
            if (node < 0)
            {
                occurrence = default(Occurrence);
                return false;
            }
            occurrence = Get(node);
            _head[slot] = occurrence.Next;
            if (occurrence.Next >= 0)
            {
                Occurrence next = Get(occurrence.Next);
                next.Previous = -1;
                Set(occurrence.Next, next);
            }
            else _tail[slot] = -1;
            Free(node, false);
            return true;
        }

        private void ClearOccurrences()
        {
            for (int slot = 0; slot < SlotCount; slot++) ClearSlot(slot);
        }

        private void ClearSlot(int slot)
        {
            int node = _head[slot];
            while (node >= 0)
            {
                int next = Get(node).Next;
                Free(node, true);
                node = next;
            }
            _head[slot] = _tail[slot] = -1;
        }

        private void Free(int node, bool cancelReservation)
        {
            Occurrence occurrence = Get(node);
            if (!occurrence.InUse) return;
            if (cancelReservation && occurrence.Accepted && occurrence.CandidateIndex < 0)
                _pacer.CancelReservation();
            Set(node, new Occurrence
            {
                CandidateIndex = -1,
                Previous = -1,
                Next = -1,
                NextFree = _free
            });
            _free = node;
            _live--;
        }

        private Occurrence Get(int node)
        {
            return _chunks[node / ChunkSize][node % ChunkSize];
        }

        private void Set(int node, Occurrence occurrence)
        {
            _chunks[node / ChunkSize][node % ChunkSize] = occurrence;
        }

        private static int Slot(int channel, int pitch)
        {
            return channel * PitchCount + pitch;
        }
    }
}
