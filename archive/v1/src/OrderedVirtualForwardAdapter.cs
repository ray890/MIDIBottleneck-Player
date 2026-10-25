using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum OrderedVirtualAdmissionDisposition
    {
        Accepted,
        Rejected,
        Filtered
    }

    internal sealed class OrderedVirtualAdmissionResult
    {
        internal readonly OrderedVirtualAdmissionDisposition Disposition;
        internal readonly OutputWorkItem WorkItem;
        internal readonly bool PairedReleaseSuppressed;

        internal OrderedVirtualAdmissionResult(
            OrderedVirtualAdmissionDisposition disposition,
            OutputWorkItem workItem, bool pairedReleaseSuppressed)
        {
            Disposition = disposition;
            WorkItem = workItem;
            PairedReleaseSuppressed = pairedReleaseSuppressed;
        }

        internal bool Accepted
        {
            get { return Disposition == OrderedVirtualAdmissionDisposition.Accepted; }
        }
    }

    // Inactive ordered-generation adapter for forward-only queue admission.
    // Accepted source MIDI is returned for immediate dispatch and is never
    // retained in the generation deque. Only marker/chase boundaries remain
    // queued, so target source admission cannot pass an earlier boundary.
    internal sealed class OrderedVirtualForwardAdapter
    {
        private sealed class GenerationState
        {
            internal readonly ForwardDropQueue Queue;
            internal readonly CompleteNoteTracker CompleteNotes = new CompleteNoteTracker();

            internal GenerationState(int capacity)
            {
                Queue = new ForwardDropQueue(capacity);
            }
        }

        private readonly MidiEventReader _events;
        private readonly OrderedPlaybackGenerationQueue _generations;
        private readonly Dictionary<int, GenerationState> _states =
            new Dictionary<int, GenerationState>();
        private int _nextGenerationId;
        private int _activePressureGenerationId;
        private bool _hasBoundaryInService;
        private OutputWorkItem _boundaryInService;
        private long _acceptedEvents;
        private long _droppedEvents;
        private long _filteredEvents;
        private int _maximumOccupancy;

        internal OrderedVirtualForwardAdapter(MidiEventReader events,
            int maximumLiveGenerations)
        {
            _events = events;
            _generations = new OrderedPlaybackGenerationQueue(maximumLiveGenerations);
        }

        internal int LiveGenerationCount { get { return _generations.LiveGenerationCount; } }
        internal int PendingBoundaryCount { get { return _generations.TotalAdmittedWorkCount; } }
        internal long AcceptedEvents { get { return _acceptedEvents; } }
        internal long DroppedEvents { get { return _droppedEvents; } }
        internal long FilteredEvents { get { return _filteredEvents; } }
        internal int MaximumOccupancy { get { return _maximumOccupancy; } }
        internal int ActivePressureGenerationId { get { return _activePressureGenerationId; } }
        internal PlaybackGeneration Tail { get { return _generations.Tail; } }
        internal bool CanAdmitTailSource
        {
            get { return _generations.OpenTailIsReadyForImmediateSource; }
        }
        internal int VirtualOccupancy
        {
            get
            {
                GenerationState state;
                return _states.TryGetValue(_activePressureGenerationId, out state)
                    ? state.Queue.Occupancy : 0;
            }
        }

        internal void Start(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks, int capacity)
        {
            if (_generations.LiveGenerationCount != 0)
                throw new InvalidOperationException("The virtual adapter has already started.");
            int id = ++_nextGenerationId;
            PlaybackGeneration generation = CreateGeneration(id, targetMicroseconds,
                sourceEventCursor, chasePrefix, sourceClockAnchorTicks,
                frozenElapsedTicks, capacity);
            _generations.Start(generation);
            _activePressureGenerationId = id;
        }

        internal bool TryAppendSeek(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks, int capacity)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null) return false;
            int nextId = _nextGenerationId + 1;
            PlaybackGeneration next = CreateGeneration(nextId, targetMicroseconds,
                sourceEventCursor, chasePrefix, sourceClockAnchorTicks,
                frozenElapsedTicks, capacity);
            if (!_generations.TryAppendSeek(
                OutputWorkItem.SeekMarker(tail.Id, targetMicroseconds), next))
            {
                _states.Remove(nextId);
                return false;
            }
            _nextGenerationId = nextId;
            return true;
        }

        internal OrderedVirtualAdmissionResult AdmitNextSource(
            long serviceMicroseconds, OverflowPolicy policy)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No virtual source event is available.");
            return AdmitNextSource(_events[tail.SourceEventCursor], false,
                serviceMicroseconds, policy);
        }

        internal OrderedVirtualAdmissionResult AdmitNextSource(
            MidiEventView capturedEvent, bool sourceFiltered,
            long serviceMicroseconds, OverflowPolicy policy)
        {
            if (serviceMicroseconds < 0)
                throw new ArgumentOutOfRangeException("serviceMicroseconds");
            if (policy != OverflowPolicy.DropNewest &&
                policy != OverflowPolicy.DropIncomingCompleteNotes)
                throw new NotSupportedException(
                    "Forward-only admission cannot retract already dispatched MIDI.");
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed ||
                tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No open virtual source event is available.");
            if (!_generations.OpenTailIsReadyForImmediateSource ||
                _activePressureGenerationId != tail.Id)
                throw new InvalidOperationException(
                    "A marker or chase boundary must dispatch before target source MIDI.");

            int sourceIndex = tail.SourceEventCursor;
            if (!capturedEvent.IsValid || capturedEvent.EventIndex != sourceIndex)
                throw new ArgumentException(
                    "Captured virtual source work does not match the tail cursor.",
                    "capturedEvent");
            tail.SourceEventCursor++;
            GenerationState state = _states[tail.Id];
            CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(capturedEvent);
            CompleteNoteTracker.NoteOffMatch noteMatch =
                default(CompleteNoteTracker.NoteOffMatch);
            if (sourceFiltered)
            {
                if (noteKind == CompleteNoteEventKind.NoteOn)
                    state.CompleteNotes.RecordFilteredNoteOn(capturedEvent, sourceIndex);
                else if (noteKind == CompleteNoteEventKind.NoteOff)
                    noteMatch = state.CompleteNotes.TakeNoteOff(capturedEvent);
                _filteredEvents++;
                return new OrderedVirtualAdmissionResult(
                    OrderedVirtualAdmissionDisposition.Filtered,
                    default(OutputWorkItem), noteMatch.Suppress);
            }

            if (noteKind == CompleteNoteEventKind.NoteOff)
            {
                noteMatch = state.CompleteNotes.TakeNoteOff(capturedEvent);
                if (noteMatch.Suppress)
                {
                    _droppedEvents++;
                    return new OrderedVirtualAdmissionResult(
                        OrderedVirtualAdmissionDisposition.Rejected,
                        default(OutputWorkItem), true);
                }
            }

            bool safetyAdmission = policy == OverflowPolicy.DropIncomingCompleteNotes &&
                noteKind != CompleteNoteEventKind.NoteOn;
            bool accepted = state.Queue.TryAdmit(capturedEvent.IntendedMicroseconds,
                serviceMicroseconds, safetyAdmission);
            RecordOccupancy(state.Queue.Occupancy);
            if (!accepted)
            {
                if (noteKind == CompleteNoteEventKind.NoteOn)
                    state.CompleteNotes.RecordNoteOn(capturedEvent, sourceIndex, false,
                        policy == OverflowPolicy.DropIncomingCompleteNotes);
                _droppedEvents++;
                return new OrderedVirtualAdmissionResult(
                    OrderedVirtualAdmissionDisposition.Rejected,
                    default(OutputWorkItem), false);
            }
            if (noteKind == CompleteNoteEventKind.NoteOn)
                state.CompleteNotes.RecordNoteOn(capturedEvent, sourceIndex, true, false);
            _acceptedEvents++;
            OutputWorkProtection protection = noteKind == CompleteNoteEventKind.NoteOn
                ? OutputWorkProtection.OrdinaryNoteAttack :
                noteKind == CompleteNoteEventKind.NoteOff
                    ? OutputWorkProtection.ProtectedRelease :
                CompleteNoteTracker.IsSafetyControl(capturedEvent)
                    ? OutputWorkProtection.SafetyTraffic : OutputWorkProtection.Ordinary;
            long occurrenceId = noteKind == CompleteNoteEventKind.NoteOn
                ? sourceIndex : noteKind == CompleteNoteEventKind.NoteOff &&
                    noteMatch.AttackIndex >= 0 ? noteMatch.AttackIndex : sourceIndex;
            return new OrderedVirtualAdmissionResult(
                OrderedVirtualAdmissionDisposition.Accepted,
                OutputWorkItem.CapturedSource(tail.Id, sourceIndex,
                    capturedEvent.IntendedMicroseconds, protection,
                    occurrenceId, capturedEvent), false);
        }

        internal void AdvanceVirtualTime(long sourceMicroseconds)
        {
            if (sourceMicroseconds < 0)
                throw new ArgumentOutOfRangeException("sourceMicroseconds");
            GenerationState state;
            if (!_states.TryGetValue(_activePressureGenerationId, out state)) return;
            state.Queue.Advance(sourceMicroseconds);
            RecordOccupancy(state.Queue.Occupancy);
        }

        internal bool TryBeginBoundaryDispatch(out OutputWorkItem item)
        {
            if (_hasBoundaryInService)
            {
                item = default(OutputWorkItem);
                return false;
            }
            if (!_generations.TryDequeueDispatch(out item)) return false;
            _boundaryInService = item;
            _hasBoundaryInService = true;
            return true;
        }

        internal void CompleteBoundaryDispatch()
        {
            if (!_hasBoundaryInService)
                throw new InvalidOperationException("No virtual boundary work is in service.");
            if (_boundaryInService.Kind == OutputWorkItemKind.ProtectedSeekMarker)
            {
                int completedGeneration = _boundaryInService.GenerationId;
                GenerationState completedState;
                if (_states.TryGetValue(completedGeneration, out completedState))
                    RecordOccupancy(completedState.Queue.MaximumOccupancy);
                _states.Remove(completedGeneration);
                _activePressureGenerationId = completedGeneration + 1;
            }
            _boundaryInService = default(OutputWorkItem);
            _hasBoundaryInService = false;
        }

        internal void ShiftSourceClockAnchors(long deltaTicks)
        {
            _generations.ShiftSourceClockAnchors(deltaTicks);
        }

        private PlaybackGeneration CreateGeneration(int id,
            long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks, int capacity)
        {
            if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
            if (sourceEventCursor < 0 || sourceEventCursor > _events.Count)
                throw new ArgumentOutOfRangeException("sourceEventCursor");
            List<OutputWorkItem> chaseWork = new List<OutputWorkItem>();
            if (chasePrefix != null)
                for (int index = 0; index < chasePrefix.Count; index++)
                    chaseWork.Add(OutputWorkItem.ForcedOrChase(id,
                        targetMicroseconds, OutputWorkProtection.SafetyTraffic,
                        chasePrefix[index]));
            GenerationState state = new GenerationState(capacity);
            _states.Add(id, state);
            return new PlaybackGeneration(id, targetMicroseconds,
                sourceEventCursor, sourceClockAnchorTicks, frozenElapsedTicks,
                chaseWork, state, Math.Max(1, chaseWork.Count + 1));
        }

        private void RecordOccupancy(int occupancy)
        {
            if (occupancy > _maximumOccupancy) _maximumOccupancy = occupancy;
        }
    }
}
