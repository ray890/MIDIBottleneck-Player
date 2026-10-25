using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal enum OrderedFiniteAdmissionDisposition
    {
        Accepted,
        AcceptedSoftLimit,
        Rejected,
        Filtered
    }

    internal sealed class OrderedFiniteAdmissionResult
    {
        internal readonly OrderedFiniteAdmissionDisposition Disposition;
        internal readonly OutputWorkItem[] Evicted;
        internal readonly bool PairedReleaseSuppressed;

        internal OrderedFiniteAdmissionResult(OrderedFiniteAdmissionDisposition disposition,
            IList<OutputWorkItem> evicted, bool pairedReleaseSuppressed = false)
        {
            Disposition = disposition;
            PairedReleaseSuppressed = pairedReleaseSuppressed;
            if (evicted == null || evicted.Count == 0)
                Evicted = new OutputWorkItem[0];
            else
            {
                Evicted = new OutputWorkItem[evicted.Count];
                evicted.CopyTo(Evicted, 0);
            }
        }

        internal bool Accepted
        {
            get
            {
                return Disposition == OrderedFiniteAdmissionDisposition.Accepted ||
                    Disposition == OrderedFiniteAdmissionDisposition.AcceptedSoftLimit;
            }
        }
    }

    internal struct OrderedFiniteCapturedDecision
    {
        internal readonly MidiEventView CapturedEvent;
        internal readonly bool SourceFiltered;

        internal OrderedFiniteCapturedDecision(MidiEventView capturedEvent,
            bool sourceFiltered)
        {
            CapturedEvent = capturedEvent;
            SourceFiltered = sourceFiltered;
        }
    }

    internal sealed class OrderedFiniteClearResult
    {
        internal readonly OutputWorkItem[] ClearedWork;
        internal readonly int FirstSkippedSourceEventIndex;
        internal readonly int SourceEventCursorAfter;
        internal readonly int DroppedSkippedSourceEvents;
        internal readonly int FilteredSkippedSourceEvents;

        internal OrderedFiniteClearResult(IList<OutputWorkItem> clearedWork,
            int firstSkippedSourceEventIndex, int sourceEventCursorAfter,
            int droppedSkippedSourceEvents, int filteredSkippedSourceEvents)
        {
            ClearedWork = new OutputWorkItem[clearedWork.Count];
            clearedWork.CopyTo(ClearedWork, 0);
            FirstSkippedSourceEventIndex = firstSkippedSourceEventIndex;
            SourceEventCursorAfter = sourceEventCursorAfter;
            DroppedSkippedSourceEvents = droppedSkippedSourceEvents;
            FilteredSkippedSourceEvents = filteredSkippedSourceEvents;
        }

        internal int DroppedEvents
        {
            get { return ClearedWork.Length + DroppedSkippedSourceEvents; }
        }

        internal bool RequiresSafetyReset
        {
            get
            {
                return ClearedWork.Length != 0 ||
                    SourceEventCursorAfter != FirstSkippedSourceEventIndex;
            }
        }
    }

    internal sealed class OrderedFinitePressureSnapshot
    {
        internal readonly int AdmittedOutstandingCount;
        internal readonly int ExternalDueCount;
        internal readonly int TotalDueCount;
        internal readonly long OldestDueMicroseconds;
        internal readonly long CurrentAgeMicroseconds;

        internal OrderedFinitePressureSnapshot(int admittedOutstandingCount,
            int externalDueCount, long oldestDueMicroseconds,
            long currentAgeMicroseconds)
        {
            AdmittedOutstandingCount = admittedOutstandingCount;
            ExternalDueCount = externalDueCount;
            TotalDueCount = admittedOutstandingCount > Int32.MaxValue - externalDueCount
                ? Int32.MaxValue : admittedOutstandingCount + externalDueCount;
            OldestDueMicroseconds = oldestDueMicroseconds;
            CurrentAgeMicroseconds = currentAgeMicroseconds;
        }
    }

    internal sealed class OrderedFiniteAgeRepairResult
    {
        internal readonly OutputWorkItem[] Evicted;
        internal readonly OrderedFiniteClearResult ClearResult;
        internal readonly bool StalledByInServiceOrPreservationBarrier;
        internal readonly long AgeAfterMicroseconds;

        internal OrderedFiniteAgeRepairResult(IList<OutputWorkItem> evicted,
            OrderedFiniteClearResult clearResult, bool stalled,
            long ageAfterMicroseconds)
        {
            Evicted = new OutputWorkItem[evicted.Count];
            evicted.CopyTo(Evicted, 0);
            ClearResult = clearResult;
            StalledByInServiceOrPreservationBarrier = stalled;
            AgeAfterMicroseconds = ageAfterMicroseconds;
        }
    }

    // Inactive finite-queue admission sublayers: generation ordering, live
    // count capacity, protected soft excess, preservation barriers,
    // per-generation complete-note pairing, immutable source filtering, and
    // clear/catch-up bounded to the open tail. Age pressure and aggregate
    // external-backlog publication remain layered work.
    internal sealed class OrderedFiniteAdmissionCore
    {
        private sealed class GenerationState
        {
            internal readonly CompleteNoteTracker CompleteNotes = new CompleteNoteTracker();
            internal int MusicalFairnessCursor;
        }

        private readonly MidiEventReader _events;
        private readonly OrderedPlaybackGenerationQueue _generations;
        private int _nextGenerationId;
        private bool _hasInService;
        private OutputWorkItem _inService;

        internal OrderedFiniteAdmissionCore(MidiEventReader events,
            int maximumLiveGenerations)
        {
            _events = events;
            _generations = new OrderedPlaybackGenerationQueue(maximumLiveGenerations);
        }

        internal int LiveGenerationCount { get { return _generations.LiveGenerationCount; } }
        internal int PendingCount { get { return _generations.TotalAdmittedWorkCount; } }
        internal int OutstandingCount
        {
            get
            {
                int pending = PendingCount;
                return _hasInService && pending < Int32.MaxValue ? pending + 1 : pending;
            }
        }
        internal bool HasInService { get { return _hasInService; } }
        internal PlaybackGeneration Tail { get { return _generations.Tail; } }

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

        internal bool IsAtOrAboveCapacity(int capacity)
        {
            if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
            return OutstandingCount >= capacity;
        }

        internal void Start(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks)
        {
            if (_generations.LiveGenerationCount != 0)
                throw new InvalidOperationException("The finite admission core has already started.");
            PlaybackGeneration generation = CreateGeneration(++_nextGenerationId,
                targetMicroseconds, sourceEventCursor, chasePrefix,
                sourceClockAnchorTicks, frozenElapsedTicks);
            _generations.Start(generation);
        }

        internal OrderedFiniteAdmissionResult AdmitNextSource(int capacity,
            OverflowPolicy policy)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed || tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No tail source event is available for admission.");
            return AdmitNextSource(_events[tail.SourceEventCursor], false, capacity, policy);
        }

        internal OrderedFiniteAdmissionResult AdmitNextSource(MidiEventView capturedEvent,
            bool sourceFiltered, int capacity, OverflowPolicy policy)
        {
            if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
            if (policy != OverflowPolicy.DropNewest && policy != OverflowPolicy.DropOldest &&
                policy != OverflowPolicy.DropOldestCompleteNote &&
                policy != OverflowPolicy.DropIncomingCompleteNotes &&
                policy != OverflowPolicy.DropBackgroundNotes &&
                policy != OverflowPolicy.ClearBufferAndCatchUp)
                throw new NotSupportedException("The finite admission policy is not supported.");
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed || tail.SourceEventCursor >= _events.Count)
                throw new InvalidOperationException("No tail source event is available for admission.");

            int sourceIndex = tail.SourceEventCursor;
            MidiEventView midiEvent = capturedEvent;
            if (!midiEvent.IsValid || midiEvent.EventIndex != sourceIndex)
                throw new ArgumentException("Captured source work does not match the tail cursor.", "capturedEvent");
            tail.SourceEventCursor++;
            GenerationState state = (GenerationState)tail.ModeState;
            CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(midiEvent);
            if (sourceFiltered)
            {
                if (noteKind == CompleteNoteEventKind.NoteOn)
                    state.CompleteNotes.RecordFilteredNoteOn(midiEvent, sourceIndex);
                else if (noteKind == CompleteNoteEventKind.NoteOff)
                    state.CompleteNotes.TakeNoteOff(midiEvent);
                return new OrderedFiniteAdmissionResult(
                    OrderedFiniteAdmissionDisposition.Filtered, null);
            }
            CompleteNoteTracker.NoteOffMatch noteMatch = default(CompleteNoteTracker.NoteOffMatch);
            if (noteKind == CompleteNoteEventKind.NoteOff)
            {
                noteMatch = state.CompleteNotes.TakeNoteOff(midiEvent);
                if (noteMatch.Suppress)
                    return new OrderedFiniteAdmissionResult(
                        noteMatch.Filtered ? OrderedFiniteAdmissionDisposition.Filtered :
                            OrderedFiniteAdmissionDisposition.Rejected, null, true);
            }
            OutputWorkProtection protection = ProtectionFor(midiEvent, noteKind);
            long occurrenceId = noteKind == CompleteNoteEventKind.NoteOn ? sourceIndex :
                noteKind == CompleteNoteEventKind.NoteOff ? noteMatch.AttackIndex : sourceIndex;
            OutputWorkItem incoming = OutputWorkItem.CapturedSource(tail.Id, sourceIndex,
                midiEvent.IntendedMicroseconds, protection, occurrenceId, midiEvent);
            List<OutputWorkItem> evicted = null;
            bool ownAttackEvicted = false;

            if (policy == OverflowPolicy.DropOldest)
            {
                while (OutstandingCount >= capacity)
                {
                    OutputWorkItem removed;
                    if (!_generations.TryEvictOldestOrdinaryFromTail(out removed)) break;
                    if (evicted == null) evicted = new List<OutputWorkItem>();
                    evicted.Add(removed);
                }
            }
            else if (policy == OverflowPolicy.DropOldestCompleteNote)
            {
                while (OutstandingCount >= capacity)
                {
                    OutputWorkItem attack;
                    OutputWorkItem release;
                    bool hasRelease;
                    if (!_generations.TryEvictOldestCompleteNoteFromTail(
                        out attack, out release, out hasRelease)) break;
                    if (evicted == null) evicted = new List<OutputWorkItem>();
                    evicted.Add(attack);
                    if (hasRelease) evicted.Add(release);
                    else state.CompleteNotes.MarkUnsentAttackEvicted(attack.SourceEventIndex);
                    if (noteKind == CompleteNoteEventKind.NoteOff &&
                        noteMatch.AttackIndex == attack.SourceEventIndex)
                        ownAttackEvicted = true;
                }
                if (ownAttackEvicted)
                    return new OrderedFiniteAdmissionResult(
                        OrderedFiniteAdmissionDisposition.Rejected, evicted, true);
            }
            else if (policy == OverflowPolicy.DropBackgroundNotes)
            {
                while (OutstandingCount >= capacity)
                {
                    OutputWorkItem attack;
                    OutputWorkItem release;
                    bool hasRelease;
                    if (!_generations.TryEvictLeastImportantCompleteNoteFromTail(
                        ref state.MusicalFairnessCursor, out attack, out release,
                        out hasRelease)) break;
                    if (evicted == null) evicted = new List<OutputWorkItem>();
                    evicted.Add(attack);
                    if (hasRelease) evicted.Add(release);
                    else state.CompleteNotes.MarkUnsentAttackEvicted(
                        attack.SourceEventIndex);
                    if (noteKind == CompleteNoteEventKind.NoteOff &&
                        noteMatch.AttackIndex == attack.SourceEventIndex)
                        ownAttackEvicted = true;
                }
                if (ownAttackEvicted)
                    return new OrderedFiniteAdmissionResult(
                        OrderedFiniteAdmissionDisposition.Rejected, evicted, true);
            }

            if (OutstandingCount >= capacity)
            {
                if ((policy == OverflowPolicy.DropIncomingCompleteNotes ||
                    policy == OverflowPolicy.DropBackgroundNotes) &&
                    noteKind != CompleteNoteEventKind.NoteOn)
                {
                    AdmitOrThrow(incoming);
                    return new OrderedFiniteAdmissionResult(
                        OrderedFiniteAdmissionDisposition.AcceptedSoftLimit, evicted);
                }
                if (protection == OutputWorkProtection.Ordinary ||
                    protection == OutputWorkProtection.OrdinaryNoteAttack)
                {
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        state.CompleteNotes.RecordNoteOn(midiEvent, sourceIndex, false,
                            policy == OverflowPolicy.DropOldestCompleteNote ||
                            policy == OverflowPolicy.DropIncomingCompleteNotes ||
                            policy == OverflowPolicy.DropBackgroundNotes);
                    return new OrderedFiniteAdmissionResult(
                        OrderedFiniteAdmissionDisposition.Rejected, evicted);
                }
                AdmitOrThrow(incoming);
                return new OrderedFiniteAdmissionResult(
                    OrderedFiniteAdmissionDisposition.AcceptedSoftLimit, evicted);
            }

            AdmitOrThrow(incoming);
            if (noteKind == CompleteNoteEventKind.NoteOn)
                state.CompleteNotes.RecordNoteOn(midiEvent, sourceIndex, true, false);
            return new OrderedFiniteAdmissionResult(
                OrderedFiniteAdmissionDisposition.Accepted, evicted);
        }

        internal bool TryAppendSeek(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null) return false;
            int nextId = _nextGenerationId + 1;
            PlaybackGeneration next = CreateGeneration(nextId, targetMicroseconds,
                sourceEventCursor, chasePrefix, sourceClockAnchorTicks, frozenElapsedTicks);
            if (!_generations.TryAppendSeek(
                OutputWorkItem.SeekMarker(tail.Id, targetMicroseconds), next)) return false;
            _nextGenerationId = nextId;
            return true;
        }

        // Clear/catch-up is deliberately scoped to the current open tail.
        // Closed generations, their markers, and the in-service head are
        // preservation barriers and can never be removed by this operation.
        // A caller may supply immutable routing/filter capture for skipped
        // source work; without it the binary time search remains O(log n).
        internal OrderedFiniteClearResult ClearOpenTailAndCatchUp(
            long throughMicroseconds,
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            if (throughMicroseconds < 0)
                throw new ArgumentOutOfRangeException("throughMicroseconds");
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed)
                throw new InvalidOperationException("No open finite generation can catch up.");

            List<OutputWorkItem> cleared = new List<OutputWorkItem>();
            if (!_generations.TryClearOpenTail(cleared))
                throw new InvalidOperationException("The finite tail could not be cleared.");

            int firstSkipped = tail.SourceEventCursor;
            int catchUp;
            if (throughMicroseconds == Int64.MaxValue) catchUp = _events.Count;
            else catchUp = _events.LowerBoundByTime(throughMicroseconds + 1);
            if (catchUp < firstSkipped) catchUp = firstSkipped;

            int droppedSkipped = 0;
            int filteredSkipped = 0;
            GenerationState state = (GenerationState)tail.ModeState;
            if (captureDecision == null)
                droppedSkipped = catchUp - firstSkipped;
            else
            {
                for (int sourceIndex = firstSkipped; sourceIndex < catchUp; sourceIndex++)
                {
                    OrderedFiniteCapturedDecision decision = captureDecision(_events[sourceIndex]);
                    MidiEventView captured = decision.CapturedEvent;
                    if (!captured.IsValid || captured.EventIndex != sourceIndex)
                        throw new InvalidOperationException(
                            "A catch-up source decision did not preserve source identity.");
                    if (!decision.SourceFiltered)
                    {
                        droppedSkipped++;
                        continue;
                    }
                    filteredSkipped++;
                    CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(captured);
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        state.CompleteNotes.RecordFilteredNoteOn(captured, sourceIndex);
                    else if (noteKind == CompleteNoteEventKind.NoteOff)
                        state.CompleteNotes.TakeNoteOff(captured);
                }
            }
            tail.SourceEventCursor = catchUp;
            return new OrderedFiniteClearResult(cleared, firstSkipped, catchUp,
                droppedSkipped, filteredSkipped);
        }

        // Publishes already admitted generations separately from source work
        // that is due at the open tail but has not yet been admitted (for
        // example while a synchronous provider Send blocks the worker).
        internal OrderedFinitePressureSnapshot MeasureDuePressure(
            long logicalNowMicroseconds,
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            if (logicalNowMicroseconds < 0)
                throw new ArgumentOutOfRangeException("logicalNowMicroseconds");
            int admitted = OutstandingCount;
            long oldest = Int64.MaxValue;
            if (_hasInService) oldest = _inService.LogicalOutputMicroseconds;
            OutputWorkItem pendingHead;
            if (_generations.TryPeekDispatch(out pendingHead))
                oldest = Math.Min(oldest, pendingHead.LogicalOutputMicroseconds);

            int externalDue = 0;
            PlaybackGeneration tail = Tail;
            if (tail != null && !tail.IsAdmissionClosed)
            {
                int dueEnd = logicalNowMicroseconds == Int64.MaxValue
                    ? _events.Count : _events.LowerBoundByTime(logicalNowMicroseconds + 1);
                int first = Math.Max(0, tail.SourceEventCursor);
                for (int sourceIndex = first; sourceIndex < dueEnd; sourceIndex++)
                {
                    MidiEventView sourceEvent = _events[sourceIndex];
                    if (captureDecision != null)
                    {
                        OrderedFiniteCapturedDecision decision = captureDecision(sourceEvent);
                        if (!decision.CapturedEvent.IsValid ||
                            decision.CapturedEvent.EventIndex != sourceIndex)
                            throw new InvalidOperationException(
                                "A pressure source decision did not preserve source identity.");
                        if (decision.SourceFiltered) continue;
                        sourceEvent = decision.CapturedEvent;
                    }
                    if (externalDue < Int32.MaxValue) externalDue++;
                    oldest = Math.Min(oldest, sourceEvent.IntendedMicroseconds);
                }
            }
            long age = oldest == Int64.MaxValue ? 0 :
                Math.Max(0, logicalNowMicroseconds - oldest);
            return new OrderedFinitePressureSnapshot(admitted, externalDue, oldest, age);
        }

        // Waiting-time limits use strict age > limit. Repair may remove only
        // an eligible head in the current open generation; an in-service item,
        // older closed generation, marker, chase/state item, or protected
        // release stalls repair instead of causing unrelated later eviction.
        internal OrderedFiniteAgeRepairResult RepairWaitingTimePressure(
            long logicalNowMicroseconds, long ageLimitMicroseconds,
            OverflowPolicy policy,
            Func<MidiEventView, OrderedFiniteCapturedDecision> captureDecision)
        {
            if (logicalNowMicroseconds < 0)
                throw new ArgumentOutOfRangeException("logicalNowMicroseconds");
            if (ageLimitMicroseconds < 0)
                throw new ArgumentOutOfRangeException("ageLimitMicroseconds");
            if (policy != OverflowPolicy.DropOldest &&
                policy != OverflowPolicy.DropOldestCompleteNote &&
                policy != OverflowPolicy.ClearBufferAndCatchUp)
                throw new NotSupportedException(
                    "Waiting-time pressure requires an oldest-removal or clear policy.");

            List<OutputWorkItem> evicted = new List<OutputWorkItem>();
            OrderedFiniteClearResult clearResult = null;
            bool stalled = false;
            while (AdmittedAge(logicalNowMicroseconds) > ageLimitMicroseconds)
            {
                if (_hasInService)
                {
                    stalled = true;
                    break;
                }
                if (policy == OverflowPolicy.ClearBufferAndCatchUp)
                {
                    if (!_generations.OpenTailIsDispatchHead)
                    {
                        stalled = true;
                        break;
                    }
                    clearResult = ClearOpenTailAndCatchUp(logicalNowMicroseconds,
                        captureDecision);
                    break;
                }
                if (policy == OverflowPolicy.DropOldest)
                {
                    OutputWorkItem removed;
                    if (!_generations.TryEvictOrdinaryAtOpenHead(out removed))
                    {
                        stalled = true;
                        break;
                    }
                    evicted.Add(removed);
                    continue;
                }

                OutputWorkItem attack;
                OutputWorkItem release;
                bool hasRelease;
                if (!_generations.TryEvictCompleteNoteAtOpenHead(
                    out attack, out release, out hasRelease))
                {
                    stalled = true;
                    break;
                }
                evicted.Add(attack);
                if (hasRelease) evicted.Add(release);
                else ((GenerationState)Tail.ModeState).CompleteNotes
                    .MarkUnsentAttackEvicted(attack.SourceEventIndex);
            }
            return new OrderedFiniteAgeRepairResult(evicted, clearResult, stalled,
                AdmittedAge(logicalNowMicroseconds));
        }

        private long AdmittedAge(long logicalNowMicroseconds)
        {
            long oldest = Int64.MaxValue;
            if (_hasInService) oldest = _inService.LogicalOutputMicroseconds;
            OutputWorkItem pending;
            if (_generations.TryPeekDispatch(out pending))
                oldest = Math.Min(oldest, pending.LogicalOutputMicroseconds);
            return oldest == Int64.MaxValue ? 0 :
                Math.Max(0, logicalNowMicroseconds - oldest);
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
                throw new InvalidOperationException("No ordered finite work item is in service.");
            _inService = default(OutputWorkItem);
            _hasInService = false;
        }

        private void AdmitOrThrow(OutputWorkItem item)
        {
            if (!_generations.TryAdmitToTail(item))
                throw new InvalidOperationException("The ordered finite tail rejected bounded source work.");
        }

        private PlaybackGeneration CreateGeneration(int id, long targetMicroseconds,
            int sourceEventCursor, IList<MidiEventView> chasePrefix,
            long sourceClockAnchorTicks, long frozenElapsedTicks)
        {
            if (sourceEventCursor < 0 || sourceEventCursor > _events.Count)
                throw new ArgumentOutOfRangeException("sourceEventCursor");
            List<OutputWorkItem> chaseWork = new List<OutputWorkItem>();
            if (chasePrefix != null)
                for (int i = 0; i < chasePrefix.Count; i++)
                    chaseWork.Add(OutputWorkItem.ForcedOrChase(id, targetMicroseconds,
                        OutputWorkProtection.SafetyTraffic, chasePrefix[i]));
            long requestedBound = (long)_events.Count + chaseWork.Count + 1;
            int workBound = requestedBound > Int32.MaxValue
                ? Int32.MaxValue : Math.Max(1, (int)requestedBound);
            return new PlaybackGeneration(id, targetMicroseconds, sourceEventCursor,
                sourceClockAnchorTicks, frozenElapsedTicks, chaseWork,
                new GenerationState(), workBound);
        }

        private static OutputWorkProtection ProtectionFor(MidiEventView midiEvent,
            CompleteNoteEventKind noteKind)
        {
            if (noteKind == CompleteNoteEventKind.NoteOn)
                return OutputWorkProtection.OrdinaryNoteAttack;
            if (noteKind == CompleteNoteEventKind.NoteOff)
                return OutputWorkProtection.ProtectedRelease;
            if (CompleteNoteTracker.IsSafetyControl(midiEvent))
                return OutputWorkProtection.SafetyTraffic;
            return OutputWorkProtection.Ordinary;
        }
    }
}
