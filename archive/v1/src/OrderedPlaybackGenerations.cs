using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum OutputWorkItemKind
    {
        SourceMidi,
        ProjectedGateMidi,
        ForcedOrChaseMidi,
        ProtectedSeekMarker
    }

    internal enum OutputWorkProtection
    {
        Ordinary,
        OrdinaryNoteAttack,
        ProtectedRelease,
        SafetyTraffic,
        PreservationBarrier
    }

    internal struct OutputWorkItem
    {
        internal readonly int GenerationId;
        internal readonly OutputWorkItemKind Kind;
        internal readonly int SourceEventIndex;
        internal readonly long LogicalOutputMicroseconds;
        internal readonly OutputWorkProtection Protection;
        internal readonly long OccurrenceId;
        internal readonly MidiEventView MidiEvent;
        internal readonly bool HasMidiEvent;

        private OutputWorkItem(int generationId, OutputWorkItemKind kind,
            int sourceEventIndex, long logicalOutputMicroseconds,
            OutputWorkProtection protection, long occurrenceId,
            MidiEventView midiEvent, bool hasMidiEvent)
        {
            if (generationId < 0) throw new ArgumentOutOfRangeException("generationId");
            if (logicalOutputMicroseconds < 0)
                throw new ArgumentOutOfRangeException("logicalOutputMicroseconds");
            GenerationId = generationId;
            Kind = kind;
            SourceEventIndex = sourceEventIndex;
            LogicalOutputMicroseconds = logicalOutputMicroseconds;
            Protection = protection;
            OccurrenceId = occurrenceId;
            MidiEvent = midiEvent;
            HasMidiEvent = hasMidiEvent;
        }

        internal static OutputWorkItem Source(int generationId, int sourceEventIndex,
            long logicalOutputMicroseconds, OutputWorkProtection protection, long occurrenceId)
        {
            if (sourceEventIndex < 0) throw new ArgumentOutOfRangeException("sourceEventIndex");
            return new OutputWorkItem(generationId, OutputWorkItemKind.SourceMidi,
                sourceEventIndex, logicalOutputMicroseconds, protection, occurrenceId,
                default(MidiEventView), false);
        }

        internal static OutputWorkItem CapturedSource(int generationId, int sourceEventIndex,
            long logicalOutputMicroseconds, OutputWorkProtection protection, long occurrenceId,
            MidiEventView midiEvent)
        {
            if (sourceEventIndex < 0) throw new ArgumentOutOfRangeException("sourceEventIndex");
            return new OutputWorkItem(generationId, OutputWorkItemKind.SourceMidi,
                sourceEventIndex, logicalOutputMicroseconds, protection, occurrenceId,
                midiEvent, true);
        }

        internal static OutputWorkItem ProjectedGate(int generationId, int sourceEventIndex,
            long logicalOutputMicroseconds, OutputWorkProtection protection, long occurrenceId,
            MidiEventView midiEvent)
        {
            return new OutputWorkItem(generationId, OutputWorkItemKind.ProjectedGateMidi,
                sourceEventIndex, logicalOutputMicroseconds, protection, occurrenceId,
                midiEvent, true);
        }

        internal static OutputWorkItem ForcedOrChase(int generationId,
            long logicalOutputMicroseconds, OutputWorkProtection protection,
            MidiEventView midiEvent)
        {
            return new OutputWorkItem(generationId, OutputWorkItemKind.ForcedOrChaseMidi,
                -1, logicalOutputMicroseconds, protection, -1, midiEvent, true);
        }

        internal static OutputWorkItem SeekMarker(int generationId,
            long logicalOutputMicroseconds)
        {
            return new OutputWorkItem(generationId, OutputWorkItemKind.ProtectedSeekMarker,
                -1, logicalOutputMicroseconds, OutputWorkProtection.PreservationBarrier,
                -1, default(MidiEventView), false);
        }
    }

    internal sealed class PlaybackGeneration
    {
        private readonly LinkedList<OutputWorkItem> _admitted = new LinkedList<OutputWorkItem>();
        private readonly OutputWorkItem[] _chasePrefix;
        private readonly int _maximumAdmittedWork;
        private readonly MusicalNotePrioritySelector _musicalPriority =
            new MusicalNotePrioritySelector();
        private MusicalNotePriorityCandidate[] _musicalCandidates =
            new MusicalNotePriorityCandidate[16];
        private LinkedListNode<OutputWorkItem>[] _musicalAttackNodes =
            new LinkedListNode<OutputWorkItem>[16];
        private LinkedListNode<OutputWorkItem>[] _musicalReleaseNodes =
            new LinkedListNode<OutputWorkItem>[16];
        private bool _admissionClosed;

        internal readonly int Id;
        internal readonly long TargetSourceMicroseconds;
        internal int SourceEventCursor;
        internal long SourceClockAnchorTicks;
        internal long FrozenElapsedTicks;
        internal object ModeState;

        internal PlaybackGeneration(int id, long targetSourceMicroseconds,
            int sourceEventCursor, long sourceClockAnchorTicks, long frozenElapsedTicks,
            IList<OutputWorkItem> chasePrefix, object modeState, int maximumAdmittedWork)
        {
            if (id < 0) throw new ArgumentOutOfRangeException("id");
            if (targetSourceMicroseconds < 0)
                throw new ArgumentOutOfRangeException("targetSourceMicroseconds");
            if (sourceEventCursor < 0) throw new ArgumentOutOfRangeException("sourceEventCursor");
            if (maximumAdmittedWork < 1) throw new ArgumentOutOfRangeException("maximumAdmittedWork");
            Id = id;
            TargetSourceMicroseconds = targetSourceMicroseconds;
            SourceEventCursor = sourceEventCursor;
            SourceClockAnchorTicks = sourceClockAnchorTicks;
            FrozenElapsedTicks = frozenElapsedTicks;
            ModeState = modeState;
            _maximumAdmittedWork = maximumAdmittedWork;
            _chasePrefix = chasePrefix == null
                ? new OutputWorkItem[0] : CopyAndValidateChase(id, chasePrefix);
            if (_chasePrefix.Length > maximumAdmittedWork)
                throw new ArgumentException("The chase prefix exceeds the admitted-work bound.", "chasePrefix");
            for (int i = 0; i < _chasePrefix.Length; i++) _admitted.AddLast(_chasePrefix[i]);
        }

        internal bool IsAdmissionClosed { get { return _admissionClosed; } }
        internal int AdmittedWorkCount { get { return _admitted.Count; } }
        internal int ChasePrefixCount { get { return _chasePrefix.Length; } }

        internal OutputWorkItem ChasePrefixAt(int index)
        {
            return _chasePrefix[index];
        }

        internal bool TryAdmit(OutputWorkItem item)
        {
            if (_admissionClosed || item.GenerationId != Id ||
                _admitted.Count >= _maximumAdmittedWork) return false;
            if (item.Kind == OutputWorkItemKind.ProtectedSeekMarker) return false;
            _admitted.AddLast(item);
            return true;
        }

        internal bool TryEvictOldestOrdinary(out OutputWorkItem item)
        {
            for (LinkedListNode<OutputWorkItem> node = _admitted.First;
                node != null; node = node.Next)
            {
                if (node.Value.Protection != OutputWorkProtection.Ordinary &&
                    node.Value.Protection != OutputWorkProtection.OrdinaryNoteAttack) continue;
                item = node.Value;
                _admitted.Remove(node);
                return true;
            }
            item = default(OutputWorkItem);
            return false;
        }

        internal bool TryEvictOrdinaryAtHead(out OutputWorkItem item)
        {
            LinkedListNode<OutputWorkItem> head = _admitted.First;
            if (head == null ||
                head.Value.Protection != OutputWorkProtection.Ordinary &&
                head.Value.Protection != OutputWorkProtection.OrdinaryNoteAttack)
            {
                item = default(OutputWorkItem);
                return false;
            }
            item = head.Value;
            _admitted.RemoveFirst();
            return true;
        }

        internal bool TryEvictOldestCompleteNote(out OutputWorkItem attack,
            out OutputWorkItem release, out bool hasRelease)
        {
            for (LinkedListNode<OutputWorkItem> node = _admitted.First;
                node != null; node = node.Next)
            {
                if (node.Value.Protection != OutputWorkProtection.OrdinaryNoteAttack) continue;
                LinkedListNode<OutputWorkItem> pairedRelease = node.Next;
                while (pairedRelease != null &&
                    (pairedRelease.Value.Protection != OutputWorkProtection.ProtectedRelease ||
                    pairedRelease.Value.OccurrenceId != node.Value.OccurrenceId))
                    pairedRelease = pairedRelease.Next;
                attack = node.Value;
                if (pairedRelease != null)
                {
                    release = pairedRelease.Value;
                    _admitted.Remove(pairedRelease);
                    hasRelease = true;
                }
                else
                {
                    release = default(OutputWorkItem);
                    hasRelease = false;
                }
                _admitted.Remove(node);
                return true;
            }
            attack = default(OutputWorkItem);
            release = default(OutputWorkItem);
            hasRelease = false;
            return false;
        }

        internal bool TryEvictCompleteNoteAtHead(out OutputWorkItem attack,
            out OutputWorkItem release, out bool hasRelease)
        {
            LinkedListNode<OutputWorkItem> head = _admitted.First;
            if (head == null ||
                head.Value.Protection != OutputWorkProtection.OrdinaryNoteAttack)
            {
                attack = default(OutputWorkItem);
                release = default(OutputWorkItem);
                hasRelease = false;
                return false;
            }
            LinkedListNode<OutputWorkItem> pairedRelease = head.Next;
            while (pairedRelease != null &&
                (pairedRelease.Value.Protection != OutputWorkProtection.ProtectedRelease ||
                pairedRelease.Value.OccurrenceId != head.Value.OccurrenceId))
                pairedRelease = pairedRelease.Next;
            attack = head.Value;
            if (pairedRelease != null)
            {
                release = pairedRelease.Value;
                _admitted.Remove(pairedRelease);
                hasRelease = true;
            }
            else
            {
                release = default(OutputWorkItem);
                hasRelease = false;
            }
            _admitted.RemoveFirst();
            return true;
        }

        internal bool TryEvictLeastImportantCompleteNote(ref int fairnessCursor,
            out OutputWorkItem attack, out OutputWorkItem release,
            out bool hasRelease)
        {
            EnsureMusicalCapacity(_admitted.Count);
            int candidateCount = 0;
            for (LinkedListNode<OutputWorkItem> node = _admitted.First;
                node != null; node = node.Next)
            {
                OutputWorkItem item = node.Value;
                if (item.Protection != OutputWorkProtection.OrdinaryNoteAttack ||
                    !item.HasMidiEvent) continue;
                LinkedListNode<OutputWorkItem> pairedRelease = node.Next;
                while (pairedRelease != null &&
                    (pairedRelease.Value.Protection != OutputWorkProtection.ProtectedRelease ||
                    pairedRelease.Value.OccurrenceId != item.OccurrenceId))
                    pairedRelease = pairedRelease.Next;
                long duration = pairedRelease == null ? Int64.MaxValue :
                    Math.Max(0, pairedRelease.Value.LogicalOutputMicroseconds -
                        item.LogicalOutputMicroseconds);
                _musicalCandidates[candidateCount] = new MusicalNotePriorityCandidate
                {
                    Pitch = item.MidiEvent.GetDataByte(1) & 0x7F,
                    Velocity = item.MidiEvent.GetDataByte(2) & 0x7F,
                    Track = item.MidiEvent.Track,
                    Channel = item.MidiEvent.Channel,
                    SourceOrder = item.SourceEventIndex,
                    IntendedMicroseconds = item.LogicalOutputMicroseconds,
                    DurationMicroseconds = duration
                };
                _musicalAttackNodes[candidateCount] = node;
                _musicalReleaseNodes[candidateCount] = pairedRelease;
                candidateCount++;
            }
            if (candidateCount == 0)
            {
                attack = default(OutputWorkItem);
                release = default(OutputWorkItem);
                hasRelease = false;
                return false;
            }

            int victimIndex = _musicalPriority.SelectLeastImportant(
                _musicalCandidates, candidateCount, fairnessCursor);
            LinkedListNode<OutputWorkItem> victimAttack = _musicalAttackNodes[victimIndex];
            LinkedListNode<OutputWorkItem> victimRelease = _musicalReleaseNodes[victimIndex];
            attack = victimAttack.Value;
            if (victimRelease != null)
            {
                release = victimRelease.Value;
                _admitted.Remove(victimRelease);
                hasRelease = true;
            }
            else
            {
                release = default(OutputWorkItem);
                hasRelease = false;
            }
            _admitted.Remove(victimAttack);
            fairnessCursor = _musicalPriority.NextFairnessCursor(
                _musicalCandidates[victimIndex]);
            return true;
        }

        private void EnsureMusicalCapacity(int count)
        {
            if (_musicalCandidates.Length >= count) return;
            int capacity = _musicalCandidates.Length;
            while (capacity < count && capacity <= Int32.MaxValue / 2) capacity *= 2;
            if (capacity < count) capacity = count;
            Array.Resize(ref _musicalCandidates, capacity);
            Array.Resize(ref _musicalAttackNodes, capacity);
            Array.Resize(ref _musicalReleaseNodes, capacity);
        }

        internal bool TryClearOpenAdmitted(List<OutputWorkItem> cleared)
        {
            if (_admissionClosed) return false;
            if (cleared == null) throw new ArgumentNullException("cleared");
            for (LinkedListNode<OutputWorkItem> node = _admitted.First;
                node != null; node = node.Next)
                cleared.Add(node.Value);
            _admitted.Clear();
            return true;
        }

        internal bool CanAppendMarker
        {
            get { return !_admissionClosed && _admitted.Count < _maximumAdmittedWork; }
        }

        internal void CloseWithMarker(OutputWorkItem marker)
        {
            if (!CanAppendMarker) throw new InvalidOperationException("Generation cannot accept its marker.");
            if (marker.GenerationId != Id || marker.Kind != OutputWorkItemKind.ProtectedSeekMarker ||
                marker.Protection != OutputWorkProtection.PreservationBarrier)
                throw new ArgumentException("The work item is not this generation's protected Seek marker.", "marker");
            _admitted.AddLast(marker);
            _admissionClosed = true;
        }

        internal bool TryPeek(out OutputWorkItem item)
        {
            if (_admitted.Count == 0) { item = default(OutputWorkItem); return false; }
            item = _admitted.First.Value;
            return true;
        }

        internal bool TryDequeue(out OutputWorkItem item)
        {
            if (_admitted.Count == 0) { item = default(OutputWorkItem); return false; }
            item = _admitted.First.Value;
            _admitted.RemoveFirst();
            return true;
        }

        private static OutputWorkItem[] CopyAndValidateChase(int generationId,
            IList<OutputWorkItem> chasePrefix)
        {
            OutputWorkItem[] copy = new OutputWorkItem[chasePrefix.Count];
            for (int i = 0; i < copy.Length; i++)
            {
                OutputWorkItem item = chasePrefix[i];
                if (item.GenerationId != generationId ||
                    item.Kind != OutputWorkItemKind.ForcedOrChaseMidi)
                    throw new ArgumentException("A chase prefix contains an invalid work item.", "chasePrefix");
                copy[i] = item;
            }
            return copy;
        }
    }

    internal sealed class OrderedPlaybackGenerationQueue
    {
        private readonly LinkedList<PlaybackGeneration> _generations =
            new LinkedList<PlaybackGeneration>();
        private readonly int _maximumLiveGenerations;

        internal OrderedPlaybackGenerationQueue(int maximumLiveGenerations)
        {
            if (maximumLiveGenerations < 1)
                throw new ArgumentOutOfRangeException("maximumLiveGenerations");
            _maximumLiveGenerations = maximumLiveGenerations;
        }

        internal int LiveGenerationCount { get { return _generations.Count; } }
        internal int TotalAdmittedWorkCount
        {
            get
            {
                int count = 0;
                for (LinkedListNode<PlaybackGeneration> node = _generations.First;
                    node != null; node = node.Next)
                    count = count > Int32.MaxValue - node.Value.AdmittedWorkCount
                        ? Int32.MaxValue : count + node.Value.AdmittedWorkCount;
                return count;
            }
        }
        internal PlaybackGeneration Head { get { return _generations.First == null ? null : _generations.First.Value; } }
        internal PlaybackGeneration Tail { get { return _generations.Last == null ? null : _generations.Last.Value; } }
        internal bool CanAppendSeek
        {
            get
            {
                PlaybackGeneration tail = Tail;
                return tail != null && _generations.Count < _maximumLiveGenerations &&
                    !tail.IsAdmissionClosed && tail.CanAppendMarker;
            }
        }

        internal void Start(PlaybackGeneration generation)
        {
            if (generation == null) throw new ArgumentNullException("generation");
            if (_generations.Count != 0) throw new InvalidOperationException("The generation queue has already started.");
            _generations.AddLast(generation);
        }

        internal bool TryAdmitToTail(OutputWorkItem item)
        {
            PlaybackGeneration tail = Tail;
            return tail != null && tail.TryAdmit(item);
        }

        internal bool TryEvictOldestOrdinaryFromTail(out OutputWorkItem item)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed)
            {
                item = default(OutputWorkItem);
                return false;
            }
            return tail.TryEvictOldestOrdinary(out item);
        }

        internal bool TryEvictOldestCompleteNoteFromTail(out OutputWorkItem attack,
            out OutputWorkItem release, out bool hasRelease)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed)
            {
                attack = default(OutputWorkItem);
                release = default(OutputWorkItem);
                hasRelease = false;
                return false;
            }
            return tail.TryEvictOldestCompleteNote(out attack, out release, out hasRelease);
        }

        internal bool TryClearOpenTail(List<OutputWorkItem> cleared)
        {
            PlaybackGeneration tail = Tail;
            return tail != null && tail.TryClearOpenAdmitted(cleared);
        }

        internal bool OpenTailIsDispatchHead
        {
            get
            {
                RetireEmptyClosedHeads();
                PlaybackGeneration head = Head;
                PlaybackGeneration tail = Tail;
                return head != null && Object.ReferenceEquals(head, tail) &&
                    !tail.IsAdmissionClosed;
            }
        }

        internal bool OpenTailIsReadyForImmediateSource
        {
            get
            {
                return OpenTailIsDispatchHead && Tail.AdmittedWorkCount == 0;
            }
        }

        internal bool TryEvictOrdinaryAtOpenHead(out OutputWorkItem item)
        {
            if (!OpenTailIsDispatchHead)
            {
                item = default(OutputWorkItem);
                return false;
            }
            return Tail.TryEvictOrdinaryAtHead(out item);
        }

        internal bool TryEvictCompleteNoteAtOpenHead(out OutputWorkItem attack,
            out OutputWorkItem release, out bool hasRelease)
        {
            if (!OpenTailIsDispatchHead)
            {
                attack = default(OutputWorkItem);
                release = default(OutputWorkItem);
                hasRelease = false;
                return false;
            }
            return Tail.TryEvictCompleteNoteAtHead(out attack, out release, out hasRelease);
        }

        internal bool TryEvictLeastImportantCompleteNoteFromTail(ref int fairnessCursor,
            out OutputWorkItem attack, out OutputWorkItem release,
            out bool hasRelease)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed)
            {
                attack = default(OutputWorkItem);
                release = default(OutputWorkItem);
                hasRelease = false;
                return false;
            }
            return tail.TryEvictLeastImportantCompleteNote(ref fairnessCursor,
                out attack, out release, out hasRelease);
        }

        internal bool TryAppendSeek(OutputWorkItem marker, PlaybackGeneration nextGeneration)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || nextGeneration == null || _generations.Count >= _maximumLiveGenerations)
                return false;
            if (tail.IsAdmissionClosed || !tail.CanAppendMarker || nextGeneration.Id <= tail.Id ||
                marker.GenerationId != tail.Id || marker.Kind != OutputWorkItemKind.ProtectedSeekMarker ||
                marker.Protection != OutputWorkProtection.PreservationBarrier)
                return false;
            tail.CloseWithMarker(marker);
            _generations.AddLast(nextGeneration);
            return true;
        }

        internal bool TryPeekDispatch(out OutputWorkItem item)
        {
            RetireEmptyClosedHeads();
            PlaybackGeneration head = Head;
            if (head == null) { item = default(OutputWorkItem); return false; }
            return head.TryPeek(out item);
        }

        internal bool TryDequeueDispatch(out OutputWorkItem item)
        {
            RetireEmptyClosedHeads();
            PlaybackGeneration head = Head;
            if (head == null) { item = default(OutputWorkItem); return false; }
            if (!head.TryDequeue(out item)) return false;
            RetireEmptyClosedHeads();
            return true;
        }

        internal PlaybackGeneration FindGeneration(int generationId)
        {
            for (LinkedListNode<PlaybackGeneration> node = _generations.First;
                node != null; node = node.Next)
                if (node.Value.Id == generationId) return node.Value;
            return null;
        }

        internal void ShiftSourceClockAnchors(long deltaTicks)
        {
            if (deltaTicks <= 0) return;
            for (LinkedListNode<PlaybackGeneration> node = _generations.First;
                node != null; node = node.Next)
            {
                long anchor = node.Value.SourceClockAnchorTicks;
                node.Value.SourceClockAnchorTicks = anchor > Int64.MaxValue - deltaTicks
                    ? Int64.MaxValue : anchor + deltaTicks;
            }
        }

        private void RetireEmptyClosedHeads()
        {
            while (_generations.First != null &&
                _generations.First.Value.IsAdmissionClosed &&
                _generations.First.Value.AdmittedWorkCount == 0)
                _generations.RemoveFirst();
        }
    }
}
