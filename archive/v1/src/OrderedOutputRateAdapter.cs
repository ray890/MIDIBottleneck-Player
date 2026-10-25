using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Preserving-Seek adapter for Output rate gate. It retains only complete
    // already-admitted source ticks and ordered markers/chase work; rejected
    // source occurrences are never queued for later catch-up.
    internal sealed class OrderedOutputRateAdapter
    {
        private sealed class GenerationState
        {
            internal readonly CompleteNoteTracker SourceFilters = new CompleteNoteTracker();
            internal readonly List<MidiEventView> Emissions = new List<MidiEventView>();
            internal bool SourceComplete;
        }

        private readonly MidiEventReader _events;
        private readonly OrderedPlaybackGenerationQueue _generations;
        private readonly OutputRateGate _gate;
        private int _nextGenerationId;
        private bool _hasInService;
        private long _rejectedAttacks;
        private long _suppressedReleases;
        private long _rejectedOtherMessages;

        internal OrderedOutputRateAdapter(MidiEventReader events,
            int maximumLiveGenerations, long eventsPerSecond, long clockUnitsPerSecond)
        {
            _events = events;
            _generations = new OrderedPlaybackGenerationQueue(maximumLiveGenerations);
            _gate = new OutputRateGate(eventsPerSecond, clockUnitsPerSecond);
        }

        internal OutputRateGate Gate { get { return _gate; } }
        internal PlaybackGeneration Tail { get { return _generations.Tail; } }
        internal int PendingCount { get { return _generations.TotalAdmittedWorkCount; } }
        internal long RejectedAttacks { get { return _rejectedAttacks; } }
        internal long SuppressedReleases { get { return _suppressedReleases; } }
        internal long RejectedOtherMessages { get { return _rejectedOtherMessages; } }
        internal bool TailSourceComplete
        {
            get
            {
                PlaybackGeneration tail = Tail;
                return tail != null && ((GenerationState)tail.ModeState).SourceComplete;
            }
        }

        internal void Start(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks)
        {
            PlaybackGeneration generation = CreateGeneration(++_nextGenerationId,
                targetMicroseconds, sourceEventCursor, chasePrefix, sourceClockAnchorTicks);
            _generations.Start(generation);
        }

        internal int AdmitNextSourceTick(long decisionTimestamp,
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed ||
                tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No output-rate source work is available.");
            GenerationState state = (GenerationState)tail.ModeState;
            long tick = _events[tail.SourceEventCursor].AbsoluteTick;
            _gate.BeginSourceTick(tick);
            int count = 0;
            while (tail.SourceEventCursor < _events.Count &&
                _events[tail.SourceEventCursor].AbsoluteTick == tick)
            {
                MidiEventView source = _events[tail.SourceEventCursor];
                OrderedFiniteCapturedDecision decision = captureDecision == null
                    ? new OrderedFiniteCapturedDecision(source, false)
                    : captureDecision(source);
                if (!decision.CapturedEvent.IsValid ||
                    decision.CapturedEvent.EventIndex != tail.SourceEventCursor)
                    throw new InvalidOperationException("Captured output-rate work lost source identity.");
                tail.SourceEventCursor++;
                count++;
                CompleteNoteEventKind kind = CompleteNoteTracker.Classify(decision.CapturedEvent);
                if (decision.SourceFiltered)
                {
                    if (kind == CompleteNoteEventKind.NoteOn)
                        state.SourceFilters.RecordFilteredNoteOn(
                            decision.CapturedEvent, decision.CapturedEvent.EventIndex);
                    else if (kind == CompleteNoteEventKind.NoteOff)
                        state.SourceFilters.TakeNoteOff(decision.CapturedEvent);
                    continue;
                }
                if (kind == CompleteNoteEventKind.NoteOff &&
                    state.SourceFilters.TakeNoteOff(decision.CapturedEvent).Suppress)
                    continue;
                _gate.Admit(decision.CapturedEvent);
            }
            _gate.EndSourceTick(state.Emissions, decisionTimestamp);
            for (int index = 0; index < state.Emissions.Count; index++)
            {
                MidiEventView emitted = state.Emissions[index];
                CompleteNoteEventKind kind = CompleteNoteTracker.Classify(emitted);
                OutputWorkProtection protection = kind == CompleteNoteEventKind.NoteOn
                    ? OutputWorkProtection.OrdinaryNoteAttack :
                    kind == CompleteNoteEventKind.NoteOff
                        ? OutputWorkProtection.ProtectedRelease :
                    CompleteNoteTracker.IsSafetyControl(emitted)
                        ? OutputWorkProtection.SafetyTraffic : OutputWorkProtection.Ordinary;
                long occurrence = emitted.EventIndex >= 0 ? emitted.EventIndex : index;
                if (!tail.TryAdmit(OutputWorkItem.CapturedSource(tail.Id,
                    emitted.EventIndex, emitted.IntendedMicroseconds,
                    protection, occurrence, emitted)))
                    throw new InvalidOperationException("The output-rate generation exceeded its work bound.");
            }
            TakeCounts();
            return count;
        }

        internal bool TryAppendSeek(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || !_generations.CanAppendSeek) return false;
            CompleteSource(tail);
            int nextId = _nextGenerationId + 1;
            PlaybackGeneration next = CreateGeneration(nextId, targetMicroseconds,
                sourceEventCursor, chasePrefix, sourceClockAnchorTicks);
            if (!_generations.TryAppendSeek(
                OutputWorkItem.SeekMarker(tail.Id, targetMicroseconds), next)) return false;
            _nextGenerationId = nextId;
            return true;
        }

        internal void CompleteTailSource()
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed)
                throw new InvalidOperationException("No output-rate tail can complete.");
            CompleteSource(tail);
        }

        internal bool TryPeekDispatch(out OutputWorkItem item)
        {
            return _generations.TryPeekDispatch(out item);
        }

        internal bool TryBeginDispatch(out OutputWorkItem item)
        {
            if (_hasInService)
            {
                item = default(OutputWorkItem);
                return false;
            }
            if (!_generations.TryDequeueDispatch(out item)) return false;
            _hasInService = true;
            return true;
        }

        internal void CompleteDispatch()
        {
            if (!_hasInService)
                throw new InvalidOperationException("No output-rate work is in service.");
            _hasInService = false;
        }

        internal long DispatchDueClockTicks(OutputWorkItem item)
        {
            PlaybackGeneration generation = _generations.FindGeneration(item.GenerationId);
            if (generation == null)
                throw new InvalidOperationException("The work item's generation has retired.");
            long deltaUs = Math.Max(0,
                item.LogicalOutputMicroseconds - generation.TargetSourceMicroseconds);
            long deltaTicks = PlaybackEngine.MicrosecondsToTicks(deltaUs);
            return generation.SourceClockAnchorTicks > Int64.MaxValue - deltaTicks
                ? Int64.MaxValue : generation.SourceClockAnchorTicks + deltaTicks;
        }

        internal void ShiftSourceClockAnchors(long deltaTicks)
        {
            _generations.ShiftSourceClockAnchors(deltaTicks);
        }

        private void CompleteSource(PlaybackGeneration generation)
        {
            GenerationState state = (GenerationState)generation.ModeState;
            if (state.SourceComplete) return;
            _gate.CompleteSource();
            state.SourceComplete = true;
            TakeCounts();
        }

        private void TakeCounts()
        {
            _rejectedAttacks += _gate.TakeRejectedAttacks();
            _suppressedReleases += _gate.TakeSuppressedReleases();
            _rejectedOtherMessages += _gate.TakeRejectedOtherMessages();
        }

        private PlaybackGeneration CreateGeneration(int id, long targetMicroseconds,
            int sourceEventCursor, IList<MidiEventView> chasePrefix,
            long sourceClockAnchorTicks)
        {
            List<OutputWorkItem> prefix = new List<OutputWorkItem>();
            if (chasePrefix != null)
                for (int index = 0; index < chasePrefix.Count; index++)
                    prefix.Add(OutputWorkItem.ForcedOrChase(id, targetMicroseconds,
                        OutputWorkProtection.SafetyTraffic, chasePrefix[index]));
            long requested = (long)_events.Count + prefix.Count + 1;
            int bound = requested > Int32.MaxValue ? Int32.MaxValue :
                Math.Max(1, (int)requested);
            return new PlaybackGeneration(id, targetMicroseconds,
                sourceEventCursor, sourceClockAnchorTicks, 0, prefix,
                new GenerationState(), bound);
        }
    }
}
