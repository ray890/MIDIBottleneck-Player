using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Inactive per-generation adapter shared by Per-note and corrected global
    // Delayed NoteOff. Each generation owns an isolated gate, adaptive clock,
    // source-filter pairing state, and explicit projected output work.
    internal sealed class OrderedMusicalGateAdapter
    {
        private sealed class GenerationState
        {
            internal readonly ProcessingMode Mode;
            internal readonly PerNoteIntervalGate PerNote;
            internal readonly DelayedNoteOffGate Delayed;
            internal readonly AdaptiveNoteIntervalClock Adaptive;
            internal readonly CompleteNoteTracker SourceFilters = new CompleteNoteTracker();
            internal bool SourceComplete;

            internal GenerationState(ProcessingMode mode, long intervalMicroseconds,
                bool scaleByTraffic, bool intervalPolyphonyLimitEnabled,
                int intervalPolyphonyLimit)
            {
                Mode = mode;
                if (mode == ProcessingMode.PerNoteIntervalGate)
                    PerNote = new PerNoteIntervalGate(intervalMicroseconds,
                        intervalPolyphonyLimitEnabled,
                        intervalPolyphonyLimit);
                else if (mode == ProcessingMode.DelayedNoteOff)
                    Delayed = new DelayedNoteOffGate(intervalMicroseconds,
                        intervalPolyphonyLimitEnabled,
                        intervalPolyphonyLimit);
                else throw new ArgumentOutOfRangeException("mode");
                Adaptive = new AdaptiveNoteIntervalClock(intervalMicroseconds,
                    scaleByTraffic);
            }

            internal long NextBoundaryMicroseconds
            {
                get
                {
                    return Mode == ProcessingMode.PerNoteIntervalGate
                        ? PerNote.NextBoundaryMicroseconds
                        : Delayed.NextBoundaryMicroseconds;
                }
            }

            internal bool HasPendingTransitions
            {
                get
                {
                    return Mode == ProcessingMode.PerNoteIntervalGate
                        ? PerNote.HasPendingTransitions : Delayed.HasPendingTransitions;
                }
            }
        }

        private readonly MidiEventReader _events;
        private readonly OrderedPlaybackGenerationQueue _generations;
        private readonly ProcessingMode _mode;
        private readonly long _intervalMicroseconds;
        private readonly bool _scaleByTraffic;
        private readonly bool _intervalPolyphonyLimitEnabled;
        private readonly int _intervalPolyphonyLimit;
        private readonly MidiEventView[] _emissions = new MidiEventView[128];
        private readonly List<MidiEventView> _delayedEmissions =
            new List<MidiEventView>(256);
        private int _nextGenerationId;
        private bool _hasInService;
        private OutputWorkItem _inService;
        private long _filteredEvents;

        internal OrderedMusicalGateAdapter(MidiEventReader events,
            int maximumLiveGenerations, ProcessingMode mode,
            long intervalMicroseconds, bool scaleByTraffic,
            bool intervalPolyphonyLimitEnabled = false,
            int intervalPolyphonyLimit = PlaybackEngine.DefaultIntervalPolyphonyLimit)
        {
            if (mode != ProcessingMode.PerNoteIntervalGate &&
                mode != ProcessingMode.DelayedNoteOff)
                throw new ArgumentOutOfRangeException("mode");
            if (intervalMicroseconds < 0)
                throw new ArgumentOutOfRangeException("intervalMicroseconds");
            _events = events;
            _generations = new OrderedPlaybackGenerationQueue(maximumLiveGenerations);
            _mode = mode;
            _intervalMicroseconds = intervalMicroseconds;
            _scaleByTraffic = scaleByTraffic;
            _intervalPolyphonyLimitEnabled = intervalPolyphonyLimitEnabled;
            _intervalPolyphonyLimit = intervalPolyphonyLimitEnabled
                ? Math.Max(1, Math.Min(PlaybackEngine.DefaultIntervalPolyphonyLimit,
                    intervalPolyphonyLimit))
                : PlaybackEngine.DefaultIntervalPolyphonyLimit;
        }

        internal int LiveGenerationCount { get { return _generations.LiveGenerationCount; } }
        internal int PendingCount { get { return _generations.TotalAdmittedWorkCount; } }
        internal long FilteredEvents { get { return _filteredEvents; } }
        internal PlaybackGeneration Tail { get { return _generations.Tail; } }
        internal bool TailSourceComplete
        {
            get
            {
                PlaybackGeneration tail = Tail;
                return tail != null && ((GenerationState)tail.ModeState).SourceComplete;
            }
        }

        internal bool TryPeekDispatch(out OutputWorkItem item)
        {
            return _generations.TryPeekDispatch(out item);
        }

        internal long DispatchDueClockTicks(OutputWorkItem item)
        {
            PlaybackGeneration generation = _generations.FindGeneration(item.GenerationId);
            if (generation == null)
                throw new InvalidOperationException("The work item's generation has retired.");
            long deltaMicroseconds = Math.Max(0,
                item.LogicalOutputMicroseconds - generation.TargetSourceMicroseconds);
            long deltaTicks = PlaybackEngine.MicrosecondsToTicks(deltaMicroseconds);
            return generation.SourceClockAnchorTicks > Int64.MaxValue - deltaTicks
                ? Int64.MaxValue : generation.SourceClockAnchorTicks + deltaTicks;
        }

        internal void ShiftSourceClockAnchors(long deltaTicks)
        {
            _generations.ShiftSourceClockAnchors(deltaTicks);
        }

        internal void Start(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks)
        {
            if (_generations.LiveGenerationCount != 0)
                throw new InvalidOperationException("The musical gate adapter has already started.");
            PlaybackGeneration generation = CreateGeneration(++_nextGenerationId,
                targetMicroseconds, sourceEventCursor, chasePrefix,
                sourceClockAnchorTicks, frozenElapsedTicks);
            _generations.Start(generation);
        }

        // Admits one deterministic source unit: a complete absolute-tick group
        // for Per-note, or one event for Delayed NoteOff. Gate boundaries that
        // precede the source unit become explicit work before it is admitted.
        internal int AdmitNextSourceUnit(
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed ||
                tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No open musical-gate source work is available.");
            GenerationState state = (GenerationState)tail.ModeState;
            if (state.SourceComplete)
                throw new InvalidOperationException("The musical-gate source is already complete.");
            MidiEventView first = _events[tail.SourceEventCursor];
            ResolveBoundariesBeforeSource(tail, state, first);

            if (_mode == ProcessingMode.DelayedNoteOff)
            {
                AdmitCapturedSource(tail, state, first, captureDecision);
                return 1;
            }

            long tick = first.AbsoluteTick;
            state.PerNote.BeginSourceTick(tick);
            int admitted = 0;
            while (tail.SourceEventCursor < _events.Count &&
                _events[tail.SourceEventCursor].AbsoluteTick == tick)
            {
                MidiEventView sourceEvent = _events[tail.SourceEventCursor];
                AdmitCapturedSource(tail, state, sourceEvent, captureDecision);
                admitted++;
            }
            state.PerNote.EndSourceTick();
            TakeGateFiltered(state);
            return admitted;
        }

        internal bool TryAppendSeek(long closingSourceMicroseconds,
            long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || !_generations.CanAppendSeek) return false;
            CompleteGenerationSource(tail, closingSourceMicroseconds);
            int nextId = _nextGenerationId + 1;
            PlaybackGeneration next = CreateGeneration(nextId, targetMicroseconds,
                sourceEventCursor, chasePrefix, sourceClockAnchorTicks,
                frozenElapsedTicks);
            if (!_generations.TryAppendSeek(
                OutputWorkItem.SeekMarker(tail.Id, targetMicroseconds), next))
                throw new InvalidOperationException(
                    "A preflighted musical-gate Seek boundary could not be appended.");
            _nextGenerationId = nextId;
            return true;
        }

        internal void CompleteTailSource(long sourceEndMicroseconds)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed)
                throw new InvalidOperationException("No open musical-gate generation can complete.");
            CompleteGenerationSource(tail, sourceEndMicroseconds);
        }

        internal bool TryBeginDispatch(out OutputWorkItem item)
        {
            if (_hasInService)
            {
                item = default(OutputWorkItem);
                return false;
            }
            if (!_generations.TryDequeueDispatch(out item)) return false;
            _inService = item;
            _hasInService = true;
            return true;
        }

        internal void CompleteDispatch()
        {
            if (!_hasInService)
                throw new InvalidOperationException("No musical-gate work is in service.");
            _inService = default(OutputWorkItem);
            _hasInService = false;
        }

        private void AdmitCapturedSource(PlaybackGeneration generation,
            GenerationState state, MidiEventView sourceEvent,
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            int sourceIndex = generation.SourceEventCursor;
            OrderedFiniteCapturedDecision decision = captureDecision == null
                ? new OrderedFiniteCapturedDecision(sourceEvent, false)
                : captureDecision(sourceEvent);
            MidiEventView captured = decision.CapturedEvent;
            if (!captured.IsValid || captured.EventIndex != sourceIndex)
                throw new InvalidOperationException(
                    "A musical-gate source decision did not preserve source identity.");
            generation.SourceEventCursor++;
            CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(captured);
            CompleteNoteTracker.NoteOffMatch noteMatch =
                default(CompleteNoteTracker.NoteOffMatch);
            if (decision.SourceFiltered)
            {
                if (noteKind == CompleteNoteEventKind.NoteOn)
                    state.SourceFilters.RecordFilteredNoteOn(captured, sourceIndex);
                else if (noteKind == CompleteNoteEventKind.NoteOff)
                    state.SourceFilters.TakeNoteOff(captured);
                _filteredEvents++;
                return;
            }
            if (noteKind == CompleteNoteEventKind.NoteOn)
                state.SourceFilters.RecordNoteOn(captured, sourceIndex, true, false);
            else if (noteKind == CompleteNoteEventKind.NoteOff)
            {
                noteMatch = state.SourceFilters.TakeNoteOff(captured);
                if (noteMatch.Suppress)
                {
                    _filteredEvents++;
                    return;
                }
            }

            bool notNote;
            if (_mode == ProcessingMode.PerNoteIntervalGate)
                notNote = state.PerNote.Admit(captured) == PerNoteGateAdmission.NotNote;
            else
                notNote = state.Delayed.Admit(captured) == DelayedNoteOffAdmission.NotNote;
            if (notNote)
                AdmitOrThrow(generation, OutputWorkItem.CapturedSource(generation.Id,
                    sourceIndex, captured.IntendedMicroseconds,
                    ProtectionFor(captured), sourceIndex, captured));
            TakeGateFiltered(state);
        }

        private void ResolveBoundariesBeforeSource(PlaybackGeneration generation,
            GenerationState state, MidiEventView sourceEvent)
        {
            while (state.HasPendingTransitions)
            {
                long localBoundary = state.NextBoundaryMicroseconds;
                state.Adaptive.CompleteIfClosed(localBoundary,
                    sourceEvent.IntendedMicroseconds);
                long boundary = state.Adaptive.EffectiveBoundary(localBoundary);
                bool emit = _mode == ProcessingMode.PerNoteIntervalGate
                    ? boundary < sourceEvent.IntendedMicroseconds
                    : boundary < sourceEvent.IntendedMicroseconds ||
                        boundary == sourceEvent.IntendedMicroseconds &&
                        !state.Delayed.SourcePrecedesBoundary(sourceEvent, boundary);
                if (!emit) break;
                EmitBoundary(generation, state, boundary);
            }
        }

        private void CompleteGenerationSource(PlaybackGeneration generation,
            long sourceEndMicroseconds)
        {
            GenerationState state = (GenerationState)generation.ModeState;
            if (!state.SourceComplete)
            {
                if (_mode == ProcessingMode.PerNoteIntervalGate)
                    state.PerNote.CompleteSource(sourceEndMicroseconds);
                else state.Delayed.CompleteSource(sourceEndMicroseconds);
                state.SourceComplete = true;
                TakeGateFiltered(state);
            }
            while (state.HasPendingTransitions)
            {
                long localBoundary = state.NextBoundaryMicroseconds;
                state.Adaptive.CompleteIfClosed(localBoundary, Int64.MaxValue);
                long boundary = state.Adaptive.EffectiveBoundary(localBoundary);
                EmitBoundary(generation, state, boundary);
            }
        }

        private void EmitBoundary(PlaybackGeneration generation,
            GenerationState state, long boundaryMicroseconds)
        {
            int count;
            if (_mode == ProcessingMode.PerNoteIntervalGate)
                count = state.PerNote.EmitBoundary(boundaryMicroseconds, _emissions);
            else count = state.Delayed.EmitBoundary(boundaryMicroseconds,
                _delayedEmissions);
            state.Adaptive.RecordBoundaryPass(boundaryMicroseconds, count);
            TakeGateFiltered(state);
            for (int index = 0; index < count; index++)
            {
                MidiEventView emitted = _mode == ProcessingMode.PerNoteIntervalGate
                    ? _emissions[index] : _delayedEmissions[index];
                int sourceIndex = emitted.EventIndex;
                long occurrenceId = sourceIndex >= 0 ? sourceIndex :
                    ((long)generation.Id << 32) | (uint)(emitted.GetDataByte(1) & 0x7F);
                AdmitOrThrow(generation, OutputWorkItem.ProjectedGate(generation.Id,
                    sourceIndex, boundaryMicroseconds, ProtectionFor(emitted),
                    occurrenceId, emitted));
            }
        }

        private void TakeGateFiltered(GenerationState state)
        {
            long filtered = _mode == ProcessingMode.PerNoteIntervalGate
                ? state.PerNote.TakeFilteredEventCount()
                : state.Delayed.TakeFilteredEventCount();
            _filteredEvents = _filteredEvents > Int64.MaxValue - filtered
                ? Int64.MaxValue : _filteredEvents + filtered;
        }

        private void AdmitOrThrow(PlaybackGeneration generation, OutputWorkItem item)
        {
            if (!generation.TryAdmit(item))
                throw new InvalidOperationException(
                    "The musical-gate generation exceeded its admitted-work bound.");
        }

        private PlaybackGeneration CreateGeneration(int id,
            long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks)
        {
            if (sourceEventCursor < 0 || sourceEventCursor > _events.Count)
                throw new ArgumentOutOfRangeException("sourceEventCursor");
            List<OutputWorkItem> chaseWork = new List<OutputWorkItem>();
            if (chasePrefix != null)
                for (int index = 0; index < chasePrefix.Count; index++)
                    chaseWork.Add(OutputWorkItem.ForcedOrChase(id,
                        targetMicroseconds, OutputWorkProtection.SafetyTraffic,
                        chasePrefix[index]));
            long requestedBound = (long)_events.Count * 3L +
                chaseWork.Count + 257L;
            int workBound = requestedBound > Int32.MaxValue
                ? Int32.MaxValue : Math.Max(1, (int)requestedBound);
            return new PlaybackGeneration(id, targetMicroseconds,
                sourceEventCursor, sourceClockAnchorTicks, frozenElapsedTicks,
                chaseWork, new GenerationState(_mode, _intervalMicroseconds,
                    _scaleByTraffic, _intervalPolyphonyLimitEnabled,
                    _intervalPolyphonyLimit), workBound);
        }

        private static OutputWorkProtection ProtectionFor(MidiEventView midiEvent)
        {
            CompleteNoteEventKind kind = CompleteNoteTracker.Classify(midiEvent);
            if (kind == CompleteNoteEventKind.NoteOn)
                return OutputWorkProtection.OrdinaryNoteAttack;
            if (kind == CompleteNoteEventKind.NoteOff)
                return OutputWorkProtection.ProtectedRelease;
            if (CompleteNoteTracker.IsSafetyControl(midiEvent))
                return OutputWorkProtection.SafetyTraffic;
            return OutputWorkProtection.Ordinary;
        }
    }
}
