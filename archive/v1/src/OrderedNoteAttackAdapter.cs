using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Per-generation adapter for the Note start rate gate. The shared rational
    // ledger makes admitted output explicit so preserving Seek can order it
    // with markers and chase.
    internal sealed class OrderedNoteAttackAdapter
    {
        private sealed class GenerationState
        {
            internal readonly MusicalAttackRateLimiter Limiter;
            internal readonly CompleteNoteTracker SourceFilters = new CompleteNoteTracker();
            internal readonly List<MidiEventView> Emissions = new List<MidiEventView>();
            internal bool SourceComplete;

            internal GenerationState(long attacksPerSecond)
            {
                Limiter = new MusicalAttackRateLimiter(attacksPerSecond);
            }
        }

        private readonly MidiEventReader _events;
        private readonly OrderedPlaybackGenerationQueue _generations;
        private readonly long _attacksPerSecond;
        private int _nextGenerationId;
        private bool _hasInService;
        private long _rejectedAttacks;
        private long _suppressedReleases;

        internal OrderedNoteAttackAdapter(MidiEventReader events,
            int maximumLiveGenerations, long attacksPerSecond)
        {
            _events = events;
            _generations = new OrderedPlaybackGenerationQueue(maximumLiveGenerations);
            _attacksPerSecond = attacksPerSecond;
        }

        internal PlaybackGeneration Tail { get { return _generations.Tail; } }
        internal int PendingCount { get { return _generations.TotalAdmittedWorkCount; } }
        internal long RejectedAttacks { get { return _rejectedAttacks; } }
        internal long SuppressedReleases { get { return _suppressedReleases; } }
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

        internal int AdmitNextSourceTick(
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed ||
                tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No note-start source work is available.");
            GenerationState state = (GenerationState)tail.ModeState;
            long tick = _events[tail.SourceEventCursor].AbsoluteTick;
            state.Limiter.BeginSourceTick(tick);
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
                    throw new InvalidOperationException("Captured note-start work lost source identity.");
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
                state.Limiter.Admit(decision.CapturedEvent);
            }
            state.Limiter.EndSourceTick(state.Emissions);
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
                    throw new InvalidOperationException("The note-start generation exceeded its work bound.");
            }
            TakeCounts(state);
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
                throw new InvalidOperationException("No note-start tail can complete.");
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
            if (!_hasInService) throw new InvalidOperationException("No note-start work is in service.");
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
            state.Limiter.CompleteSource();
            state.SourceComplete = true;
            TakeCounts(state);
        }

        private void TakeCounts(GenerationState state)
        {
            _rejectedAttacks += state.Limiter.TakeRejectedAttacks();
            _suppressedReleases += state.Limiter.TakeSuppressedReleases();
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
                new GenerationState(_attacksPerSecond), bound);
        }
    }
}
