using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed partial class PlaybackEngine
    {
        private void RunOrderedUnlimitedQueueMode(HighResolutionWaiter waiter)
        {
            OrderedUnlimitedQueueAdapter adapter = new OrderedUnlimitedQueueAdapter(
                _events, MaximumPendingPreservedSeeks + 1);
            long clockNow = Stopwatch.GetTimestamp();
            adapter.Start(TicksToMicroseconds(_transportBaseTicks), _startEventIndex,
                CaptureOrderedGenerationPrefix(_startEventIndex,
                    TicksToMicroseconds(_transportBaseTicks)), clockNow, 0);

            ServiceDurationSettings serviceSettings = Volatile.Read(ref _serviceSettings);
            ServiceDurationClock serviceClock = serviceSettings.CreateClock();
            OutputWorkItem inService = default(OutputWorkItem);
            bool hasInService = false;
            long completionStamp = Int64.MaxValue;
            long lastCompletionStamp = 0;
            bool serviceHold = false;
            long pacedServiceTicks = 0;

            while (IsActive())
            {
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                long pauseStarted = 0;
                lock (_sync)
                    if (_state == PlaybackState.Paused) pauseStarted = Stopwatch.GetTimestamp();
                if (!WaitWhilePaused()) return;
                if (pauseStarted != 0)
                {
                    long pausedTicks = Math.Max(0, Stopwatch.GetTimestamp() - pauseStarted);
                    adapter.ShiftSourceClockAnchors(pausedTicks);
                    if (hasInService && completionStamp < Int64.MaxValue)
                        completionStamp = completionStamp > Int64.MaxValue - pausedTicks
                            ? Int64.MaxValue : completionStamp + pausedTicks;
                    if (lastCompletionStamp != 0)
                        lastCompletionStamp = lastCompletionStamp > Int64.MaxValue - pausedTicks
                            ? Int64.MaxValue : lastCompletionStamp + pausedTicks;
                    if (hasInService && OutputPacedTimeline)
                        serviceHold = BeginOutputPacedHold();
                }

                long now = Stopwatch.GetTimestamp();
                bool controlBoundary = HasPendingOrderedLiveControlBoundary();
                if (!controlBoundary && (!OutputPacedTimeline || !hasInService))
                {
                    AdmitOrderedUnlimitedDue(adapter, now);
                    AppendOrderedSeekRequests(adapter, now);
                    AdmitOrderedUnlimitedDue(adapter, now);
                }
                else if (!hasInService && adapter.PendingWorkCount == 0)
                {
                    ApplyOrderedLiveControlBoundary();
                    continue;
                }
                PublishOrderedUnlimitedPressure(adapter, hasInService);

                if (hasInService && completionStamp <= now)
                {
                    long sendStarted = Stopwatch.GetTimestamp();
                    DispatchOrderedWorkItem(inService);
                    long sendTicks = Math.Max(0, Stopwatch.GetTimestamp() - sendStarted);
                    EndOutputPacedHold(serviceHold);
                    serviceHold = false;
                    if (OutputPacedTimeline)
                        adapter.ShiftSourceClockAnchors(SaturatingAddTicks(
                            pacedServiceTicks, sendTicks));
                    hasInService = false;
                    lastCompletionStamp = completionStamp;
                    continue;
                }

                if (!hasInService)
                {
                    OutputWorkItem next;
                    if (adapter.TryPeek(out next))
                    {
                        if (next.Kind == OutputWorkItemKind.ProtectedSeekMarker ||
                            next.Kind == OutputWorkItemKind.ForcedOrChaseMidi ||
                            Volatile.Read(ref _simulateSlowdown) == 0)
                        {
                            adapter.TryDequeue(out next);
                            PublishOrderedUnlimitedPressure(adapter, true);
                            long sendStarted = Stopwatch.GetTimestamp();
                            DispatchOrderedWorkItem(next);
                            if (OutputPacedTimeline)
                                adapter.ShiftSourceClockAnchors(Math.Max(0,
                                    Stopwatch.GetTimestamp() - sendStarted));
                            continue;
                        }

                        adapter.TryDequeue(out inService);
                        hasInService = true;
                        UpdateServiceClock(ref serviceClock, ref serviceSettings);
                        long dueStamp = adapter.DispatchDueClockTicks(inService);
                        long startStamp = Math.Max(dueStamp, lastCompletionStamp);
                        pacedServiceTicks = MicrosecondsToTicks(
                            serviceClock.NextMicroseconds(inService.MidiEvent));
                        completionStamp = SaturatingAddTicks(startStamp, pacedServiceTicks);
                        serviceHold = BeginOutputPacedHold();
                        PublishOrderedUnlimitedPressure(adapter, true);
                        continue;
                    }
                }

                PlaybackGeneration tail = adapter.Tail;
                if (!hasInService && adapter.PendingWorkCount == 0 && tail != null &&
                    !tail.IsAdmissionClosed && tail.SourceEventCursor >= _events.Count &&
                    !HasPreservedSeekRequest())
                    return;

                long targetStamp = hasInService ? completionStamp : Int64.MaxValue;
                tail = adapter.Tail;
                if (tail != null && !tail.IsAdmissionClosed && tail.SourceEventCursor < _events.Count)
                {
                    MidiEventView nextSource = _events[tail.SourceEventCursor];
                    long deltaUs = Math.Max(0, nextSource.IntendedMicroseconds -
                        tail.TargetSourceMicroseconds);
                    targetStamp = Math.Min(targetStamp, SaturatingAddTicks(
                        tail.SourceClockAnchorTicks, MicrosecondsToTicks(deltaUs)));
                }
                if (targetStamp == Int64.MaxValue) targetStamp = now + Stopwatch.Frequency / 100;
                WaitUntilStopwatch(waiter, targetStamp, wakeGeneration);
            }
        }

        private void RunOrderedFiniteQueueMode(HighResolutionWaiter waiter)
        {
            OrderedFiniteAdmissionCore core = new OrderedFiniteAdmissionCore(
                _events, MaximumPendingPreservedSeeks + 1);
            long clockNow = Stopwatch.GetTimestamp();
            long startMicroseconds = TicksToMicroseconds(_transportBaseTicks);
            core.Start(startMicroseconds, _startEventIndex,
                CaptureOrderedGenerationPrefix(_startEventIndex, startMicroseconds),
                clockNow, 0);
            ServiceDurationSettings serviceSettings = Volatile.Read(ref _serviceSettings);
            ServiceDurationClock serviceClock = serviceSettings.CreateClock();
            OutputWorkItem inService = default(OutputWorkItem);
            bool hasInService = false;
            long completionStamp = Int64.MaxValue;
            long lastCompletionStamp = 0;
            bool serviceHold = false;
            long pacedServiceTicks = 0;

            while (IsActive())
            {
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                long pauseStarted = 0;
                lock (_sync)
                    if (_state == PlaybackState.Paused) pauseStarted = Stopwatch.GetTimestamp();
                if (!WaitWhilePaused()) return;
                if (pauseStarted != 0)
                {
                    long pausedTicks = Math.Max(0, Stopwatch.GetTimestamp() - pauseStarted);
                    core.ShiftSourceClockAnchors(pausedTicks);
                    if (hasInService && completionStamp < Int64.MaxValue)
                        completionStamp = SaturatingAddTicks(completionStamp, pausedTicks);
                    if (lastCompletionStamp != 0)
                        lastCompletionStamp = SaturatingAddTicks(lastCompletionStamp, pausedTicks);
                    if (hasInService && OutputPacedTimeline)
                        serviceHold = BeginOutputPacedHold();
                }

                long now = Stopwatch.GetTimestamp();
                bool controlBoundary = HasPendingOrderedLiveControlBoundary();
                if (!controlBoundary && (!OutputPacedTimeline || !hasInService))
                {
                    AdmitOrderedFiniteDue(core, now);
                    AppendOrderedFiniteSeekRequests(core, now);
                    AdmitOrderedFiniteDue(core, now);
                }
                else if (!hasInService && core.PendingCount == 0)
                {
                    ApplyOrderedLiveControlBoundary();
                    continue;
                }
                RepairAndPublishOrderedFinitePressure(core, hasInService);

                if (hasInService && completionStamp <= now)
                {
                    long sendStarted = Stopwatch.GetTimestamp();
                    DispatchOrderedWorkItem(inService);
                    long sendTicks = Math.Max(0, Stopwatch.GetTimestamp() - sendStarted);
                    EndOutputPacedHold(serviceHold);
                    serviceHold = false;
                    if (OutputPacedTimeline)
                        core.ShiftSourceClockAnchors(SaturatingAddTicks(
                            pacedServiceTicks, sendTicks));
                    core.CompleteDispatch();
                    hasInService = false;
                    lastCompletionStamp = completionStamp;
                    continue;
                }

                if (!hasInService)
                {
                    OutputWorkItem next;
                    if (core.TryPeekDispatch(out next))
                    {
                        if (!core.TryBeginDispatch(out inService))
                            throw new InvalidOperationException("Ordered finite dispatch lost its head item.");
                        hasInService = true;
                        if (inService.Kind == OutputWorkItemKind.ProtectedSeekMarker ||
                            inService.Kind == OutputWorkItemKind.ForcedOrChaseMidi ||
                            Volatile.Read(ref _simulateSlowdown) == 0)
                        {
                            long sendStarted = Stopwatch.GetTimestamp();
                            DispatchOrderedWorkItem(inService);
                            if (OutputPacedTimeline)
                                core.ShiftSourceClockAnchors(Math.Max(0,
                                    Stopwatch.GetTimestamp() - sendStarted));
                            core.CompleteDispatch();
                            hasInService = false;
                            continue;
                        }
                        UpdateServiceClock(ref serviceClock, ref serviceSettings);
                        long dueStamp = core.DispatchDueClockTicks(inService);
                        long startStamp = Math.Max(dueStamp, lastCompletionStamp);
                        pacedServiceTicks = MicrosecondsToTicks(
                            serviceClock.NextMicroseconds(inService.MidiEvent));
                        completionStamp = SaturatingAddTicks(startStamp, pacedServiceTicks);
                        serviceHold = BeginOutputPacedHold();
                        RepairAndPublishOrderedFinitePressure(core, true);
                        continue;
                    }
                }

                PlaybackGeneration tail = core.Tail;
                if (!hasInService && core.PendingCount == 0 && tail != null &&
                    !tail.IsAdmissionClosed && tail.SourceEventCursor >= _events.Count &&
                    !HasPreservedSeekRequest()) return;

                long targetStamp = hasInService ? completionStamp : Int64.MaxValue;
                if (tail != null && !tail.IsAdmissionClosed && tail.SourceEventCursor < _events.Count)
                {
                    MidiEventView nextSource = _events[tail.SourceEventCursor];
                    long deltaUs = Math.Max(0, nextSource.IntendedMicroseconds -
                        tail.TargetSourceMicroseconds);
                    targetStamp = Math.Min(targetStamp, SaturatingAddTicks(
                        tail.SourceClockAnchorTicks, MicrosecondsToTicks(deltaUs)));
                }
                if (targetStamp == Int64.MaxValue) targetStamp = now + Stopwatch.Frequency / 100;
                WaitUntilStopwatch(waiter, targetStamp, wakeGeneration);
            }
        }

        private void RunOrderedVirtualForwardMode(HighResolutionWaiter waiter)
        {
            OrderedVirtualForwardAdapter adapter = new OrderedVirtualForwardAdapter(
                _events, MaximumPendingPreservedSeeks + 1);
            long startMicroseconds = TicksToMicroseconds(_transportBaseTicks);
            adapter.Start(startMicroseconds, _startEventIndex,
                CaptureOrderedGenerationPrefix(_startEventIndex, startMicroseconds),
                Stopwatch.GetTimestamp(), 0, Math.Max(1, Volatile.Read(ref _queueLengthLimit)));
            ServiceDurationSettings serviceSettings = Volatile.Read(ref _serviceSettings);
            ServiceDurationClock serviceClock = serviceSettings.CreateClock();
            long reportedDrops = 0;

            while (IsActive())
            {
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                long pauseStarted = 0;
                lock (_sync)
                    if (_state == PlaybackState.Paused) pauseStarted = Stopwatch.GetTimestamp();
                if (!WaitWhilePaused()) return;
                if (pauseStarted != 0)
                    adapter.ShiftSourceClockAnchors(Math.Max(0,
                        Stopwatch.GetTimestamp() - pauseStarted));

                bool controlBoundary = HasPendingOrderedLiveControlBoundary();
                if (controlBoundary && adapter.PendingBoundaryCount == 0)
                {
                    ApplyOrderedLiveControlBoundary();
                    continue;
                }

                OutputWorkItem boundary;
                if (adapter.TryBeginBoundaryDispatch(out boundary))
                {
                    long sendStarted = Stopwatch.GetTimestamp();
                    DispatchOrderedWorkItem(boundary);
                    if (OutputPacedTimeline)
                        adapter.ShiftSourceClockAnchors(Math.Max(0,
                            Stopwatch.GetTimestamp() - sendStarted));
                    adapter.CompleteBoundaryDispatch();
                    PublishOrderedVirtualPressure(adapter);
                    continue;
                }

                long now = Stopwatch.GetTimestamp();
                PlaybackGeneration tail = adapter.Tail;
                if (!controlBoundary && adapter.CanAdmitTailSource && tail != null &&
                    tail.SourceEventCursor < _events.Count)
                {
                    MidiEventView midiEvent = _events[tail.SourceEventCursor];
                    long deltaUs = Math.Max(0, midiEvent.IntendedMicroseconds -
                        tail.TargetSourceMicroseconds);
                    long dueStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                        MicrosecondsToTicks(deltaUs));
                    if (dueStamp <= now)
                    {
                        long logicalNow = tail.TargetSourceMicroseconds +
                            TicksToMicroseconds(Math.Max(0, now - tail.SourceClockAnchorTicks));
                        adapter.AdvanceVirtualTime(logicalNow);
                        bool filtered = ShouldFilterOrderedSource(
                            tail.SourceEventCursor, midiEvent);
                        UpdateServiceClock(ref serviceClock, ref serviceSettings);
                        ServiceDurationClock admittedClock = serviceClock;
                        long service = admittedClock.NextMicroseconds(midiEvent);
                        OrderedVirtualAdmissionResult result = adapter.AdmitNextSource(
                            midiEvent, filtered, service,
                            (OverflowPolicy)Volatile.Read(ref _overflowPolicy));
                        if (result.Accepted)
                        {
                            serviceClock = admittedClock;
                            DispatchOrderedWorkItem(result.WorkItem);
                        }
                        if (adapter.DroppedEvents > reportedDrops)
                        {
                            RecordOrderedFiniteDrops((int)Math.Min(Int32.MaxValue,
                                adapter.DroppedEvents - reportedDrops));
                            reportedDrops = adapter.DroppedEvents;
                        }
                        PublishOrderedVirtualPressure(adapter);
                        continue;
                    }
                }

                PreservedSeekRequest request;
                if (!controlBoundary && adapter.CanAdmitTailSource &&
                    TryTakeOnePreservedSeekRequest(out request))
                {
                    _channelOverrides.MarkAllForcedPending();
                    IList<MidiEventView> prefix = CaptureOrderedGenerationPrefix(
                        request.TargetEventIndex, request.TargetMicroseconds);
                    if (!adapter.TryAppendSeek(request.TargetMicroseconds,
                        request.TargetEventIndex, prefix, now, 0,
                        Math.Max(1, Volatile.Read(ref _queueLengthLimit))))
                        throw new InvalidOperationException("Too many ordered playback generations are pending.");
                    AdoptOrderedSeekTarget(request);
                    continue;
                }

                tail = adapter.Tail;
                if (adapter.PendingBoundaryCount == 0 && adapter.CanAdmitTailSource &&
                    tail != null && tail.SourceEventCursor >= _events.Count &&
                    !HasPreservedSeekRequest()) return;

                long targetStamp = now + Stopwatch.Frequency / 100;
                if (tail != null && adapter.CanAdmitTailSource &&
                    tail.SourceEventCursor < _events.Count)
                {
                    MidiEventView next = _events[tail.SourceEventCursor];
                    targetStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                        MicrosecondsToTicks(Math.Max(0,
                            next.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                }
                WaitUntilStopwatch(waiter, targetStamp, wakeGeneration);
            }
        }

        private void RunOrderedMusicalGateMode(HighResolutionWaiter waiter)
        {
            long interval = ProcessingMicroseconds;
            OrderedMusicalGateAdapter adapter = new OrderedMusicalGateAdapter(
                _events, MaximumPendingPreservedSeeks + 1, _mode, interval,
                ScaleNoteIntervalByTraffic, IntervalPolyphonyLimitEnabled,
                IntervalPolyphonyLimit);
            long startMicroseconds = TicksToMicroseconds(_transportBaseTicks);
            adapter.Start(startMicroseconds, _startEventIndex,
                CaptureOrderedGenerationPrefix(_startEventIndex, startMicroseconds),
                Stopwatch.GetTimestamp(), 0);
            bool hasInService = false;
            OutputWorkItem inService = default(OutputWorkItem);
            long reportedFiltered = 0;

            while (IsActive())
            {
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                long pauseStarted = 0;
                lock (_sync)
                    if (_state == PlaybackState.Paused) pauseStarted = Stopwatch.GetTimestamp();
                if (!WaitWhilePaused()) return;
                if (pauseStarted != 0)
                    adapter.ShiftSourceClockAnchors(Math.Max(0,
                        Stopwatch.GetTimestamp() - pauseStarted));

                long now = Stopwatch.GetTimestamp();
                bool controlBoundary = HasPendingOrderedLiveControlBoundary();
                if (!controlBoundary)
                {
                    AdmitOrderedGateDue(adapter, now);
                    AppendOrderedGateSeekRequests(adapter, now);
                    AdmitOrderedGateDue(adapter, now);
                }
                else if (!hasInService && adapter.PendingCount == 0)
                {
                    ApplyOrderedLiveControlBoundary();
                    continue;
                }
                if (adapter.FilteredEvents > reportedFiltered)
                {
                    AddGateFiltered(adapter.FilteredEvents - reportedFiltered);
                    reportedFiltered = adapter.FilteredEvents;
                }
                PublishOrderedGatePressure(adapter, hasInService);

                if (!hasInService)
                {
                    OutputWorkItem next;
                    if (adapter.TryPeekDispatch(out next))
                    {
                        long dueStamp = next.Kind == OutputWorkItemKind.ProtectedSeekMarker ||
                            next.Kind == OutputWorkItemKind.ForcedOrChaseMidi
                            ? now : adapter.DispatchDueClockTicks(next);
                        if (dueStamp <= now)
                        {
                            if (!adapter.TryBeginDispatch(out inService))
                                throw new InvalidOperationException("Ordered gate dispatch lost its head item.");
                            hasInService = true;
                            Action<long> dispatchHook = _gateBoundaryDispatchHookForTesting;
                            if (dispatchHook != null &&
                                inService.Kind == OutputWorkItemKind.ProjectedGateMidi)
                                dispatchHook(inService.LogicalOutputMicroseconds);
                            if (inService.Kind == OutputWorkItemKind.ProjectedGateMidi)
                                TraceGateLogicalOutputForTesting(
                                    inService.LogicalOutputMicroseconds,
                                    inService.MidiEvent);
                            long sendStarted = Stopwatch.GetTimestamp();
                            DispatchOrderedWorkItem(inService);
                            if (OutputPacedTimeline)
                                adapter.ShiftSourceClockAnchors(Math.Max(0,
                                    Stopwatch.GetTimestamp() - sendStarted));
                            adapter.CompleteDispatch();
                            hasInService = false;
                            lock (_sync)
                                _effectiveSpeedFrontierMicroseconds = Math.Max(
                                    _effectiveSpeedFrontierMicroseconds,
                                    inService.LogicalOutputMicroseconds);
                            continue;
                        }
                    }
                }

                PlaybackGeneration tail = adapter.Tail;
                if (tail != null && !tail.IsAdmissionClosed &&
                    tail.SourceEventCursor >= _events.Count && !adapter.TailSourceComplete)
                {
                    adapter.CompleteTailSource(Math.Max(tail.TargetSourceMicroseconds,
                        _song.DurationMicroseconds));
                    continue;
                }
                if (!hasInService && adapter.PendingCount == 0 &&
                    adapter.TailSourceComplete && !HasPreservedSeekRequest()) return;

                long targetStamp = now + Stopwatch.Frequency / 100;
                OutputWorkItem pending;
                if (adapter.TryPeekDispatch(out pending) &&
                    pending.Kind != OutputWorkItemKind.ProtectedSeekMarker &&
                    pending.Kind != OutputWorkItemKind.ForcedOrChaseMidi)
                    targetStamp = adapter.DispatchDueClockTicks(pending);
                else if (tail != null && !tail.IsAdmissionClosed &&
                    tail.SourceEventCursor < _events.Count)
                {
                    MidiEventView nextSource = _events[tail.SourceEventCursor];
                    targetStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                        MicrosecondsToTicks(Math.Max(0,
                            nextSource.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                }
                WaitUntilStopwatch(waiter, targetStamp, wakeGeneration);
            }
        }

        private void RunOrderedNoteAttackMode(HighResolutionWaiter waiter)
        {
            OrderedNoteAttackAdapter adapter = new OrderedNoteAttackAdapter(
                _events, MaximumPendingPreservedSeeks + 1, EventsPerSecond);
            long startMicroseconds = TicksToMicroseconds(_transportBaseTicks);
            adapter.Start(startMicroseconds, _startEventIndex,
                CaptureOrderedGenerationPrefix(_startEventIndex, startMicroseconds),
                Stopwatch.GetTimestamp());
            long reportedRejected = 0;
            long reportedSuppressed = 0;

            while (IsActive())
            {
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                long pauseStarted = 0;
                lock (_sync)
                    if (_state == PlaybackState.Paused) pauseStarted = Stopwatch.GetTimestamp();
                if (!WaitWhilePaused()) return;
                if (pauseStarted != 0)
                    adapter.ShiftSourceClockAnchors(Math.Max(0,
                        Stopwatch.GetTimestamp() - pauseStarted));
                long now = Stopwatch.GetTimestamp();
                bool controlBoundary = HasPendingOrderedLiveControlBoundary();
                if (!controlBoundary)
                {
                    AdmitOrderedNoteAttackDue(adapter, now);
                    AppendOrderedNoteAttackSeeks(adapter, now);
                    AdmitOrderedNoteAttackDue(adapter, now);
                }
                else if (adapter.PendingCount == 0)
                {
                    ApplyOrderedLiveControlBoundary();
                    continue;
                }
                if (adapter.RejectedAttacks > reportedRejected ||
                    adapter.SuppressedReleases > reportedSuppressed)
                {
                    lock (_sync)
                    {
                        _musicalLimitRejectedAttacks +=
                            adapter.RejectedAttacks - reportedRejected;
                        _musicalLimitSuppressedReleases +=
                            adapter.SuppressedReleases - reportedSuppressed;
                    }
                    reportedRejected = adapter.RejectedAttacks;
                    reportedSuppressed = adapter.SuppressedReleases;
                }

                OutputWorkItem next;
                if (adapter.TryPeekDispatch(out next))
                {
                    long dueStamp = next.Kind == OutputWorkItemKind.ProtectedSeekMarker ||
                        next.Kind == OutputWorkItemKind.ForcedOrChaseMidi
                        ? now : adapter.DispatchDueClockTicks(next);
                    if (dueStamp <= now)
                    {
                        OutputWorkItem item;
                        if (!adapter.TryBeginDispatch(out item))
                            throw new InvalidOperationException("Ordered note-attack dispatch lost its head item.");
                        long sendStarted = Stopwatch.GetTimestamp();
                        DispatchOrderedWorkItem(item);
                        if (OutputPacedTimeline)
                            adapter.ShiftSourceClockAnchors(Math.Max(0,
                                Stopwatch.GetTimestamp() - sendStarted));
                        adapter.CompleteDispatch();
                        continue;
                    }
                }

                PlaybackGeneration tail = adapter.Tail;
                if (tail != null && !tail.IsAdmissionClosed &&
                    tail.SourceEventCursor >= _events.Count && !adapter.TailSourceComplete)
                {
                    adapter.CompleteTailSource();
                    continue;
                }
                if (adapter.PendingCount == 0 && adapter.TailSourceComplete &&
                    !HasPreservedSeekRequest()) return;

                long targetStamp = now + Stopwatch.Frequency / 100;
                if (adapter.TryPeekDispatch(out next) &&
                    next.Kind != OutputWorkItemKind.ProtectedSeekMarker &&
                    next.Kind != OutputWorkItemKind.ForcedOrChaseMidi)
                    targetStamp = adapter.DispatchDueClockTicks(next);
                else if (tail != null && !tail.IsAdmissionClosed &&
                    tail.SourceEventCursor < _events.Count)
                {
                    MidiEventView source = _events[tail.SourceEventCursor];
                    targetStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                        MicrosecondsToTicks(Math.Max(0,
                            source.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                }
                WaitUntilStopwatch(waiter, targetStamp, wakeGeneration);
            }
        }

        private void AdmitOrderedNoteAttackDue(OrderedNoteAttackAdapter adapter,
            long nowStamp)
        {
            PlaybackGeneration tail = adapter.Tail;
            while (tail != null && !tail.IsAdmissionClosed &&
                tail.SourceEventCursor < _events.Count)
            {
                MidiEventView source = _events[tail.SourceEventCursor];
                long dueStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                    MicrosecondsToTicks(Math.Max(0,
                        source.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                if (dueStamp > nowStamp) break;
                adapter.AdmitNextSourceTick(CaptureOrderedFiniteDecision);
                tail = adapter.Tail;
            }
        }

        private void AppendOrderedNoteAttackSeeks(OrderedNoteAttackAdapter adapter,
            long nowStamp)
        {
            while (true)
            {
                PreservedSeekRequest request;
                lock (_sync)
                {
                    if (_preservedSeekRequests.Count == 0) return;
                    request = _preservedSeekRequests.Dequeue();
                }
                AdmitOrderedNoteAttackDue(adapter, nowStamp);
                _channelOverrides.MarkAllForcedPending();
                IList<MidiEventView> prefix = CaptureOrderedGenerationPrefix(
                    request.TargetEventIndex, request.TargetMicroseconds);
                if (!adapter.TryAppendSeek(request.TargetMicroseconds,
                    request.TargetEventIndex, prefix, nowStamp))
                    throw new InvalidOperationException("Too many ordered playback generations are pending.");
                AdoptOrderedSeekTarget(request);
                AdmitOrderedNoteAttackDue(adapter, nowStamp);
            }
        }

        private void RunOrderedOutputRateMode(HighResolutionWaiter waiter)
        {
            OrderedOutputRateAdapter adapter = new OrderedOutputRateAdapter(
                _events, MaximumPendingPreservedSeeks + 1,
                EventsPerSecond, 1000000L);
            long startMicroseconds = TicksToMicroseconds(_transportBaseTicks);
            adapter.Start(startMicroseconds, _startEventIndex,
                CaptureOrderedGenerationPrefix(_startEventIndex, startMicroseconds),
                Stopwatch.GetTimestamp());
            long reportedRejected = 0;
            long reportedSuppressed = 0;
            long reportedOther = 0;
            _activeOutputRateGate = adapter.Gate;
            try
            {
                while (IsActive())
                {
                    int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                    long pauseStarted = 0;
                    lock (_sync)
                        if (_state == PlaybackState.Paused)
                            pauseStarted = Stopwatch.GetTimestamp();
                    if (!WaitWhilePaused()) return;
                    if (pauseStarted != 0)
                        adapter.ShiftSourceClockAnchors(Math.Max(0,
                            Stopwatch.GetTimestamp() - pauseStarted));
                    long now = Stopwatch.GetTimestamp();
                    bool controlBoundary = HasPendingOrderedLiveControlBoundary();
                    if (!controlBoundary)
                    {
                        AdmitOrderedOutputRateDue(adapter, now);
                        AppendOrderedOutputRateSeeks(adapter, now);
                        AdmitOrderedOutputRateDue(adapter, now);
                    }
                    else if (adapter.PendingCount == 0)
                    {
                        ApplyOrderedLiveControlBoundary();
                        continue;
                    }
                    if (adapter.RejectedAttacks > reportedRejected ||
                        adapter.SuppressedReleases > reportedSuppressed ||
                        adapter.RejectedOtherMessages > reportedOther)
                    {
                        lock (_sync)
                        {
                            _outputRateRejectedAttacks +=
                                adapter.RejectedAttacks - reportedRejected;
                            _outputRateSuppressedReleases +=
                                adapter.SuppressedReleases - reportedSuppressed;
                            _outputRateRejectedOtherMessages +=
                                adapter.RejectedOtherMessages - reportedOther;
                        }
                        reportedRejected = adapter.RejectedAttacks;
                        reportedSuppressed = adapter.SuppressedReleases;
                        reportedOther = adapter.RejectedOtherMessages;
                    }

                    OutputWorkItem next;
                    if (adapter.TryPeekDispatch(out next))
                    {
                        long dueStamp = next.Kind == OutputWorkItemKind.ProtectedSeekMarker ||
                            next.Kind == OutputWorkItemKind.ForcedOrChaseMidi
                            ? now : adapter.DispatchDueClockTicks(next);
                        if (dueStamp <= now)
                        {
                            OutputWorkItem item;
                            if (!adapter.TryBeginDispatch(out item))
                                throw new InvalidOperationException(
                                    "Ordered output-rate dispatch lost its head item.");
                            long sendStarted = Stopwatch.GetTimestamp();
                            DispatchOrderedWorkItem(item);
                            if (OutputPacedTimeline)
                                adapter.ShiftSourceClockAnchors(Math.Max(0,
                                    Stopwatch.GetTimestamp() - sendStarted));
                            adapter.CompleteDispatch();
                            continue;
                        }
                    }

                    PlaybackGeneration tail = adapter.Tail;
                    if (tail != null && !tail.IsAdmissionClosed &&
                        tail.SourceEventCursor >= _events.Count &&
                        !adapter.TailSourceComplete)
                    {
                        adapter.CompleteTailSource();
                        continue;
                    }
                    if (adapter.PendingCount == 0 && adapter.TailSourceComplete &&
                        !HasPreservedSeekRequest()) return;

                    long targetStamp = now + Stopwatch.Frequency / 100;
                    if (adapter.TryPeekDispatch(out next) &&
                        next.Kind != OutputWorkItemKind.ProtectedSeekMarker &&
                        next.Kind != OutputWorkItemKind.ForcedOrChaseMidi)
                        targetStamp = adapter.DispatchDueClockTicks(next);
                    else if (tail != null && !tail.IsAdmissionClosed &&
                        tail.SourceEventCursor < _events.Count)
                    {
                        MidiEventView source = _events[tail.SourceEventCursor];
                        targetStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                            MicrosecondsToTicks(Math.Max(0,
                                source.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                    }
                    WaitUntilStopwatch(waiter, targetStamp, wakeGeneration);
                }
            }
            finally
            {
                if (Object.ReferenceEquals(_activeOutputRateGate, adapter.Gate))
                    _activeOutputRateGate = null;
            }
        }

        private void AdmitOrderedOutputRateDue(OrderedOutputRateAdapter adapter,
            long nowStamp)
        {
            PlaybackGeneration tail = adapter.Tail;
            while (adapter.PendingCount == 0 && tail != null &&
                !tail.IsAdmissionClosed && tail.SourceEventCursor < _events.Count)
            {
                MidiEventView source = _events[tail.SourceEventCursor];
                long dueStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                    MicrosecondsToTicks(Math.Max(0,
                        source.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                if (dueStamp > nowStamp) break;
                adapter.AdmitNextSourceTick(
                    TicksToMicroseconds(nowStamp), CaptureOrderedFiniteDecision);
                tail = adapter.Tail;
            }
        }

        private void AppendOrderedOutputRateSeeks(OrderedOutputRateAdapter adapter,
            long nowStamp)
        {
            while (true)
            {
                PreservedSeekRequest request;
                lock (_sync)
                {
                    if (_preservedSeekRequests.Count == 0) return;
                    request = _preservedSeekRequests.Dequeue();
                }
                AdmitOrderedOutputRateDue(adapter, nowStamp);
                _channelOverrides.MarkAllForcedPending();
                IList<MidiEventView> prefix = CaptureOrderedGenerationPrefix(
                    request.TargetEventIndex, request.TargetMicroseconds);
                if (!adapter.TryAppendSeek(request.TargetMicroseconds,
                    request.TargetEventIndex, prefix, nowStamp))
                    throw new InvalidOperationException(
                        "Too many ordered output-rate generations are pending.");
                AdoptOrderedSeekTarget(request);
            }
        }

        private void AdmitOrderedGateDue(OrderedMusicalGateAdapter adapter, long nowStamp)
        {
            PlaybackGeneration tail = adapter.Tail;
            while (tail != null && !tail.IsAdmissionClosed &&
                tail.SourceEventCursor < _events.Count)
            {
                MidiEventView source = _events[tail.SourceEventCursor];
                long dueStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                    MicrosecondsToTicks(Math.Max(0,
                        source.IntendedMicroseconds - tail.TargetSourceMicroseconds)));
                if (dueStamp > nowStamp) break;
                adapter.AdmitNextSourceUnit(CaptureOrderedFiniteDecision);
                tail = adapter.Tail;
            }
        }

        private void AppendOrderedGateSeekRequests(OrderedMusicalGateAdapter adapter,
            long nowStamp)
        {
            while (true)
            {
                PreservedSeekRequest request;
                lock (_sync)
                {
                    if (_preservedSeekRequests.Count == 0) return;
                    request = _preservedSeekRequests.Dequeue();
                }
                AdmitOrderedGateDue(adapter, nowStamp);
                PlaybackGeneration closing = adapter.Tail;
                long closingNow = closing.TargetSourceMicroseconds +
                    TicksToMicroseconds(Math.Max(0,
                        nowStamp - closing.SourceClockAnchorTicks));
                _channelOverrides.MarkAllForcedPending();
                IList<MidiEventView> prefix = CaptureOrderedGenerationPrefix(
                    request.TargetEventIndex, request.TargetMicroseconds);
                if (!adapter.TryAppendSeek(closingNow, request.TargetMicroseconds,
                    request.TargetEventIndex, prefix, nowStamp, 0))
                    throw new InvalidOperationException("Too many ordered playback generations are pending.");
                AdoptOrderedSeekTarget(request);
                AdmitOrderedGateDue(adapter, nowStamp);
            }
        }

        private void PublishOrderedGatePressure(OrderedMusicalGateAdapter adapter,
            bool hasInService)
        {
            lock (_sync)
            {
                _hasPublishedStructuralDueWork = true;
                _queueLength = adapter.PendingCount;
                _outstandingEvents = _queueLength + (hasInService ? 1 : 0);
                _publishedStructuralPending = adapter.PendingCount;
                _publishedStructuralInService = hasInService;
                if (_outstandingEvents > _maximumQueueLength)
                    _maximumQueueLength = _outstandingEvents;
            }
        }

        private bool HasPendingOrderedLiveControlBoundary()
        {
            lock (_sync)
                if (_channelControlRequests.Count != 0) return true;
            return _channelOverrides.HasPendingOutput;
        }

        private void ApplyOrderedLiveControlBoundary()
        {
            ApplyPendingChannelControls();
            ApplyPendingOverrides();
        }

        private bool TryTakeOnePreservedSeekRequest(out PreservedSeekRequest request)
        {
            lock (_sync)
            {
                if (_preservedSeekRequests.Count == 0)
                {
                    request = null;
                    return false;
                }
                request = _preservedSeekRequests.Dequeue();
                return true;
            }
        }

        private void PublishOrderedVirtualPressure(OrderedVirtualForwardAdapter adapter)
        {
            lock (_sync)
            {
                _hasPublishedQueue = true;
                _queueLength = 0;
                _outstandingEvents = adapter.PendingBoundaryCount;
                _virtualQueueLength = Math.Max(0, adapter.VirtualOccupancy - 1);
                _virtualOutstandingEvents = adapter.VirtualOccupancy;
                _virtualMaximumQueueLength = Math.Max(_virtualMaximumQueueLength,
                    adapter.MaximumOccupancy);
            }
        }

        private void AdmitOrderedFiniteDue(OrderedFiniteAdmissionCore core, long nowStamp)
        {
            PlaybackGeneration tail = core.Tail;
            while (tail != null && !tail.IsAdmissionClosed &&
                tail.SourceEventCursor < _events.Count)
            {
                MidiEventView midiEvent = _events[tail.SourceEventCursor];
                long deltaUs = Math.Max(0, midiEvent.IntendedMicroseconds -
                    tail.TargetSourceMicroseconds);
                if (SaturatingAddTicks(tail.SourceClockAnchorTicks,
                    MicrosecondsToTicks(deltaUs)) > nowStamp) break;
                bool filtered = ShouldFilterOrderedSource(tail.SourceEventCursor, midiEvent);
                OverflowPolicy policy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                int capacity = Math.Max(1, Volatile.Read(ref _queueLengthLimit));
                if (policy == OverflowPolicy.ClearBufferAndCatchUp &&
                    core.IsAtOrAboveCapacity(capacity) && !filtered)
                {
                    OrderedFiniteClearResult cleared = core.ClearOpenTailAndCatchUp(
                        midiEvent.IntendedMicroseconds,
                        CaptureOrderedFiniteDecision);
                    RecordOrderedFiniteDrops(cleared.DroppedEvents);
                    if (cleared.RequiresSafetyReset)
                    {
                        try { _output.Panic(); }
                        catch { }
                    }
                    tail = core.Tail;
                    continue;
                }
                OrderedFiniteAdmissionResult result = core.AdmitNextSource(
                    midiEvent, filtered, capacity, policy);
                int dropped = result.Evicted.Length;
                if (result.Disposition == OrderedFiniteAdmissionDisposition.Rejected &&
                    !result.PairedReleaseSuppressed) dropped++;
                RecordOrderedFiniteDrops(dropped);
                tail = core.Tail;
            }
        }

        private void AppendOrderedFiniteSeekRequests(OrderedFiniteAdmissionCore core,
            long nowStamp)
        {
            while (true)
            {
                PreservedSeekRequest request;
                lock (_sync)
                {
                    if (_preservedSeekRequests.Count == 0) return;
                    request = _preservedSeekRequests.Dequeue();
                }
                AdmitOrderedFiniteDue(core, nowStamp);
                _channelOverrides.MarkAllForcedPending();
                IList<MidiEventView> prefix = CaptureOrderedGenerationPrefix(
                    request.TargetEventIndex, request.TargetMicroseconds);
                if (!core.TryAppendSeek(request.TargetMicroseconds,
                    request.TargetEventIndex, prefix, nowStamp, 0))
                    throw new InvalidOperationException("Too many ordered playback generations are pending.");
                AdoptOrderedSeekTarget(request);
                AdmitOrderedFiniteDue(core, nowStamp);
            }
        }

        private void RepairAndPublishOrderedFinitePressure(
            OrderedFiniteAdmissionCore core, bool hasInService)
        {
            PlaybackGeneration tail = core.Tail;
            long logicalNow = tail == null ? 0 : tail.TargetSourceMicroseconds +
                TicksToMicroseconds(Math.Max(0,
                    Stopwatch.GetTimestamp() - tail.SourceClockAnchorTicks));
            if (Volatile.Read(ref _queueAgeLimitEnabled) != 0)
            {
                OverflowPolicy policy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                OrderedFiniteAgeRepairResult repair = core.RepairWaitingTimePressure(
                    logicalNow, Interlocked.Read(ref _queueAgeLimitMicroseconds),
                    policy, CaptureOrderedFiniteDecision);
                int removed = repair.Evicted.Length;
                if (repair.ClearResult != null) removed += repair.ClearResult.DroppedEvents;
                RecordOrderedFiniteDrops(removed);
                if (repair.ClearResult != null && repair.ClearResult.RequiresSafetyReset)
                {
                    try { _output.Panic(); }
                    catch { }
                }
            }
            OrderedFinitePressureSnapshot pressure = core.MeasureDuePressure(
                logicalNow, PreviewOrderedFiniteDecision);
            lock (_sync)
            {
                _hasPublishedQueue = true;
                _publishedInService = hasInService;
                _queueLength = core.PendingCount;
                _outstandingEvents = pressure.TotalDueCount;
                _queueAgeMicroseconds = pressure.CurrentAgeMicroseconds;
                if (_outstandingEvents > _maximumQueueLength)
                    _maximumQueueLength = _outstandingEvents;
                if (_queueAgeMicroseconds > _maximumQueueAgeMicroseconds)
                    _maximumQueueAgeMicroseconds = _queueAgeMicroseconds;
            }
        }

        private OrderedFiniteCapturedDecision CaptureOrderedFiniteDecision(
            MidiEventView midiEvent)
        {
            return new OrderedFiniteCapturedDecision(midiEvent,
                ShouldFilterOrderedSource(midiEvent.EventIndex, midiEvent));
        }

        private OrderedFiniteCapturedDecision PreviewOrderedFiniteDecision(
            MidiEventView midiEvent)
        {
            ChannelAttribute attribute;
            int forcedValue;
            bool filtered = _channelRouting.ShouldFilter(midiEvent) ||
                _channelOverrides.ShouldSuppress(midiEvent, out attribute, out forcedValue);
            return new OrderedFiniteCapturedDecision(midiEvent, filtered);
        }

        private void RecordOrderedFiniteDrops(int count)
        {
            if (count <= 0) return;
            lock (_sync)
                _droppedEvents = _droppedEvents > Int64.MaxValue - count
                    ? Int64.MaxValue : _droppedEvents + count;
        }

        private void AdoptOrderedSeekTarget(PreservedSeekRequest request)
        {
            lock (_sync)
            {
                _startEventIndex = request.TargetEventIndex;
                _transportBaseTicks = MicrosecondsToTicks(request.TargetMicroseconds);
                _runStartStamp = Stopwatch.GetTimestamp();
                _pendingStateChaseEventIndex = -1;
                _effectiveSpeedFrontierMicroseconds = request.TargetMicroseconds;
                if (request.Generation == _preservedSeekGeneration)
                    _intendedTimelineOverrideMicroseconds = -1;
            }
        }

        private void AdmitOrderedUnlimitedDue(OrderedUnlimitedQueueAdapter adapter, long nowStamp)
        {
            PlaybackGeneration tail = adapter.Tail;
            while (tail != null && !tail.IsAdmissionClosed &&
                tail.SourceEventCursor < _events.Count)
            {
                MidiEventView midiEvent = _events[tail.SourceEventCursor];
                long deltaUs = Math.Max(0, midiEvent.IntendedMicroseconds -
                    tail.TargetSourceMicroseconds);
                long dueStamp = SaturatingAddTicks(tail.SourceClockAnchorTicks,
                    MicrosecondsToTicks(deltaUs));
                if (dueStamp > nowStamp) break;
                bool filtered = ShouldFilterOrderedSource(tail.SourceEventCursor, midiEvent);
                if (!adapter.TryAdmitNext(midiEvent, filtered))
                    throw new InvalidOperationException("The ordered Queue generation could not admit due source work.");
            }
        }

        private void AppendOrderedSeekRequests(OrderedUnlimitedQueueAdapter adapter, long nowStamp)
        {
            while (true)
            {
                PreservedSeekRequest request;
                lock (_sync)
                {
                    if (_preservedSeekRequests.Count == 0) return;
                    request = _preservedSeekRequests.Dequeue();
                }

                // Close the target generation only after admitting everything
                // that became due before this request was observed.
                AdmitOrderedUnlimitedDue(adapter, nowStamp);
                _channelOverrides.MarkAllForcedPending();
                IList<MidiEventView> prefix = CaptureOrderedGenerationPrefix(
                    request.TargetEventIndex, request.TargetMicroseconds);
                if (!adapter.TryAppendSeek(request.TargetMicroseconds,
                    request.TargetEventIndex, prefix, nowStamp, 0))
                    throw new InvalidOperationException("Too many ordered playback generations are pending.");

                AdoptOrderedSeekTarget(request);
                AdmitOrderedUnlimitedDue(adapter, nowStamp);
            }
        }

        private IList<MidiEventView> CaptureOrderedGenerationPrefix(
            int targetEventIndex, long targetMicroseconds)
        {
            List<MidiEventView> prefix = new List<MidiEventView>();
            MidiSong song;
            lock (_sync) song = _song;
            if (song == null) return prefix;
            if (Volatile.Read(ref _chaseMidiStateOnPlaySeek) != 0 && targetEventIndex > 0)
            {
                IList<MidiEvent> chase = song.GetMidiStateChaseIndex().CreateMessages(
                    targetEventIndex, _channelRouting, _channelOverrides);
                for (int i = 0; i < chase.Count; i++)
                {
                    chase[i].IntendedMicroseconds = targetMicroseconds;
                    prefix.Add(MidiEventView.FromEvent(chase[i], -1));
                }
            }
            for (int channel = 0; channel < 16; channel++)
            {
                int mask = _channelOverrides.TakePendingMask(channel);
                if (!_channelRouting.IsEnabled(channel))
                {
                    _channelOverrides.Requeue(channel, mask);
                    continue;
                }
                while (mask != 0)
                {
                    int bit = mask & -mask;
                    int attributeIndex = 0;
                    for (int scan = bit; (scan >>= 1) != 0; ) attributeIndex++;
                    mask &= ~bit;
                    ChannelAttribute attribute = (ChannelAttribute)attributeIndex;
                    int value = _channelOverrides.GetValue(channel, attribute);
                    if (value == ChannelOverrideState.AutoValue) continue;
                    MidiEvent forced = ChannelOverrideState.CreateMessage(channel, attribute, value);
                    forced.IntendedMicroseconds = targetMicroseconds;
                    prefix.Add(MidiEventView.FromEvent(forced, -1));
                }
            }
            return prefix;
        }

        private bool ShouldFilterOrderedSource(int sourceIndex, MidiEventView midiEvent)
        {
            if (_channelRouting.ShouldFilter(midiEvent))
            {
                _channelRouting.RecordFiltered(midiEvent.Channel);
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                SourceDispatchTrace trace = _sourceDispatchTrace;
                if (trace != null)
                    trace.RecordSource(sourceIndex, midiEvent, DescribeProcessingPath(_mode),
                        _sourceDispatchTraceSession, _channelOverrides.FilterGeneration,
                        true, false, "ChannelDisabled", null, ChannelOverrideState.AutoValue);
                return true;
            }
            ChannelAttribute attribute;
            int forcedValue;
            if (!_channelOverrides.ShouldSuppress(midiEvent, out attribute, out forcedValue))
                return false;
            _channelOverrides.RecordSuppressed(midiEvent.Channel);
            Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
            SourceDispatchTrace overrideTrace = _sourceDispatchTrace;
            if (overrideTrace != null)
                overrideTrace.RecordSource(sourceIndex, midiEvent, DescribeProcessingPath(_mode),
                    _sourceDispatchTraceSession, _channelOverrides.FilterGeneration,
                    true, false, "ForcedOverride", attribute.ToString(), forcedValue);
            return true;
        }

        private void DispatchOrderedWorkItem(OutputWorkItem item)
        {
            if (item.Kind == OutputWorkItemKind.ProtectedSeekMarker)
            {
                DispatchProtectedAllNotesOff();
                return;
            }
            MidiEventView midiEvent = item.HasMidiEvent
                ? item.MidiEvent : _events[item.SourceEventIndex];
            if (item.Kind == OutputWorkItemKind.ForcedOrChaseMidi)
            {
                bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
                SendRateCountedEssentialEvent(midiEvent);
                ChannelAttribute attribute;
                int value;
                bool forced = ChannelOverrideState.TryClassify(midiEvent, out attribute, out value) &&
                    midiEvent.Channel >= 0 &&
                    _channelOverrides.GetValue(midiEvent.Channel, attribute) == value;
                RecordGeneratedSourceTrace(midiEvent.ToCompatibilityEvent(),
                    forced ? "ForcedOverride" : "StateChase",
                    forced ? attribute.ToString() : "PlaySeek");
                if (trackChannels)
                {
                    if (forced)
                        _channelState.RecordOverrideApplied(midiEvent.Channel, attribute, value);
                    else
                        _channelState.RecordSuccessful(midiEvent);
                    PublishChannelState(true);
                }
                return;
            }

            long providerStart;
            long providerEnd;
            SendScheduledEvent(midiEvent, Volatile.Read(ref _channelMonitoringEnabled) != 0,
                out providerStart, out providerEnd);
            long actualUs = TicksToMicroseconds(CurrentTransportTicks());
            long lag = Math.Max(0, actualUs - item.LogicalOutputMicroseconds);
            lock (_sync)
            {
                _processedEvents++;
                _lastDispatchedMicroseconds = item.LogicalOutputMicroseconds;
                _currentLagMicroseconds = lag;
                if (lag > _maximumLagMicroseconds) _maximumLagMicroseconds = lag;
            }
            PublishChannelState(false);
        }

        private void PublishOrderedUnlimitedPressure(OrderedUnlimitedQueueAdapter adapter,
            bool inService)
        {
            lock (_sync)
            {
                _hasPublishedQueue = true;
                _publishedInService = inService;
                _queueLength = adapter.PendingWorkCount;
                _outstandingEvents = _queueLength + (inService ? 1 : 0);
                if (_outstandingEvents > _maximumQueueLength)
                    _maximumQueueLength = _outstandingEvents;
            }
        }

        private void WaitUntilStopwatch(HighResolutionWaiter waiter, long targetStamp,
            int wakeGeneration)
        {
            while (IsActive())
            {
                _wake.Reset();
                if (wakeGeneration != Volatile.Read(ref _wakeGeneration) || !IsPlaying()) return;
                long remaining = targetStamp - Stopwatch.GetTimestamp();
                if (remaining <= 0) return;
                long remainingUs = Math.Max(1, TicksToMicroseconds(remaining));
                if (!waiter.WaitMicroseconds(remainingUs)) return;
                SpinWait spin = new SpinWait();
                while (targetStamp > Stopwatch.GetTimestamp() && IsPlaying())
                {
                    if (_wake.WaitOne(0)) return;
                    spin.SpinOnce();
                }
                return;
            }
        }

        private static long SaturatingAddTicks(long left, long right)
        {
            if (right <= 0) return left;
            return left > Int64.MaxValue - right ? Int64.MaxValue : left + right;
        }
    }
}
