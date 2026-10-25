using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Inactive until every processing mode has an ordered-generation adapter.
    // This class gives unlimited Queue mode a source/chase/marker representation
    // that can be exercised without changing the current preserving-Seek path.
    internal sealed class OrderedUnlimitedQueueAdapter
    {
        private readonly MidiEventReader _events;
        private readonly OrderedPlaybackGenerationQueue _generations;
        private int _nextGenerationId;

        internal OrderedUnlimitedQueueAdapter(MidiEventReader events,
            int maximumLiveGenerations)
        {
            _events = events;
            _generations = new OrderedPlaybackGenerationQueue(maximumLiveGenerations);
        }

        internal int LiveGenerationCount { get { return _generations.LiveGenerationCount; } }
        internal int PendingWorkCount { get { return _generations.TotalAdmittedWorkCount; } }
        internal PlaybackGeneration Tail { get { return _generations.Tail; } }

        internal void Start(long targetMicroseconds, int sourceEventCursor,
            IList<MidiEventView> chasePrefix, long sourceClockAnchorTicks,
            long frozenElapsedTicks)
        {
            if (_generations.LiveGenerationCount != 0)
                throw new InvalidOperationException("The adapter has already started.");
            PlaybackGeneration generation = CreateGeneration(++_nextGenerationId,
                targetMicroseconds, sourceEventCursor, chasePrefix,
                sourceClockAnchorTicks, frozenElapsedTicks);
            _generations.Start(generation);
        }

        internal bool TryAdmitThrough(int endExclusive)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed || endExclusive < tail.SourceEventCursor ||
                endExclusive > _events.Count) return false;
            while (tail.SourceEventCursor < endExclusive)
            {
                int sourceIndex = tail.SourceEventCursor;
                MidiEventView midiEvent = _events[sourceIndex];
                OutputWorkProtection protection = ProtectionFor(midiEvent);
                OutputWorkItem item = OutputWorkItem.Source(tail.Id, sourceIndex,
                    midiEvent.IntendedMicroseconds, protection, sourceIndex);
                if (!_generations.TryAdmitToTail(item)) return false;
                tail.SourceEventCursor++;
            }
            return true;
        }

        internal bool TryAdmitNext(MidiEventView capturedEvent, bool sourceFiltered)
        {
            PlaybackGeneration tail = Tail;
            if (tail == null || tail.IsAdmissionClosed ||
                tail.SourceEventCursor < 0 || tail.SourceEventCursor >= _events.Count)
                return false;
            int sourceIndex = tail.SourceEventCursor;
            if (sourceFiltered)
            {
                tail.SourceEventCursor++;
                return true;
            }
            OutputWorkItem item = OutputWorkItem.CapturedSource(tail.Id, sourceIndex,
                capturedEvent.IntendedMicroseconds, ProtectionFor(capturedEvent),
                sourceIndex, capturedEvent);
            if (!_generations.TryAdmitToTail(item)) return false;
            tail.SourceEventCursor++;
            return true;
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

        internal bool TryDequeue(out OutputWorkItem item)
        {
            return _generations.TryDequeueDispatch(out item);
        }

        internal bool TryPeek(out OutputWorkItem item)
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
                sourceClockAnchorTicks, frozenElapsedTicks, chaseWork, null, workBound);
        }

        private static OutputWorkProtection ProtectionFor(MidiEventView midiEvent)
        {
            if (CompleteNoteTracker.Classify(midiEvent) == CompleteNoteEventKind.NoteOff)
                return OutputWorkProtection.ProtectedRelease;
            if (CompleteNoteTracker.IsSafetyControl(midiEvent))
                return OutputWorkProtection.SafetyTraffic;
            return OutputWorkProtection.Ordinary;
        }
    }

    internal static class OrderedGenerationOutputMaterializer
    {
        internal static void Append(OutputWorkItem item, MidiEventReader source,
            IList<MidiEventView> destination)
        {
            if (destination == null) throw new ArgumentNullException("destination");
            if (item.Kind == OutputWorkItemKind.SourceMidi)
            {
                if (item.HasMidiEvent)
                {
                    destination.Add(item.MidiEvent);
                    return;
                }
                if (item.SourceEventIndex < 0 || item.SourceEventIndex >= source.Count)
                    throw new ArgumentOutOfRangeException("item");
                destination.Add(source[item.SourceEventIndex]);
                return;
            }
            if (item.Kind == OutputWorkItemKind.ProjectedGateMidi ||
                item.Kind == OutputWorkItemKind.ForcedOrChaseMidi)
            {
                if (!item.HasMidiEvent) throw new ArgumentException("MIDI work has no payload.", "item");
                destination.Add(item.MidiEvent);
                return;
            }
            if (item.Kind != OutputWorkItemKind.ProtectedSeekMarker)
                throw new ArgumentException("Unknown output work kind.", "item");
            for (int channel = 0; channel < 16; channel++)
            {
                MidiEvent marker = ChannelOverrideState.CreateControllerMessage(channel, 123, 0);
                marker.IntendedMicroseconds = item.LogicalOutputMicroseconds;
                destination.Add(MidiEventView.FromEvent(marker, -1));
            }
        }
    }
}
