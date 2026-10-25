using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal sealed class SimulationResult
    {
        public readonly List<long> DispatchMicroseconds = new List<long>();
        public int DroppedEvents;
        public int MaximumQueueLength;
        public int BufferClearCount;
        public readonly List<int> DispatchedEventIndices = new List<int>();
    }

    internal static class BottleneckSimulator
    {
        // Deterministic reference model used by tests and kept separate from the
        // real-time scheduler. A service interval completes before an arrival at
        // the exact same timestamp, so that arrival sees an available processor.
        public static SimulationResult Run(IList<long> arrivals, long serviceMicroseconds, ProcessingMode mode)
        {
            return Run(arrivals, serviceMicroseconds, mode, PlaybackEngine.DefaultDropBufferCapacity);
        }

        public static SimulationResult Run(IList<long> arrivals, long serviceMicroseconds, ProcessingMode mode, int dropBufferCapacity)
        {
            return Run(arrivals, serviceMicroseconds, true, mode == ProcessingMode.Drop, dropBufferCapacity, OverflowPolicy.DropNewest);
        }

        public static SimulationResult Run(IList<long> arrivals, long serviceMicroseconds, bool simulateSlowdown,
            bool queueLengthLimitEnabled, int queueLengthLimit, OverflowPolicy overflowPolicy)
        {
            if (arrivals == null) throw new ArgumentNullException("arrivals");
            if (overflowPolicy == OverflowPolicy.DropOldestCompleteNote)
                throw new NotSupportedException("This timestamp-only reference model has no note identities; use the MIDI-event Analysis projection.");
            if (serviceMicroseconds < 0) throw new ArgumentOutOfRangeException("serviceMicroseconds");
            if (queueLengthLimit < 1) throw new ArgumentOutOfRangeException("queueLengthLimit");
            if (!simulateSlowdown) serviceMicroseconds = 0;

            SimulationResult result = new SimulationResult();
            if (!queueLengthLimitEnabled)
            {
                long processorAvailable = 0;
                int arrived = 0;
                int processed = 0;
                for (int i = 0; i < arrivals.Count; i++)
                {
                    long start = Math.Max(arrivals[i], processorAvailable);
                    long completion = checked(start + serviceMicroseconds);
                    result.DispatchMicroseconds.Add(completion);
                    result.DispatchedEventIndices.Add(i);
                    processorAvailable = completion;

                    while (arrived < arrivals.Count && arrivals[arrived] <= completion)
                        arrived++;
                    processed++;
                    int queue = arrived - processed;
                    if (queue > result.MaximumQueueLength)
                        result.MaximumQueueLength = queue;
                }
                return result;
            }

            Queue<int> pending = new Queue<int>();
            bool busy = false;
            long boundedCompletion = 0;
            int inServiceIndex = -1;
            for (int i = 0; i < arrivals.Count; i++)
            {
                long arrival = arrivals[i];
                while (busy && boundedCompletion <= arrival)
                {
                    result.DispatchMicroseconds.Add(boundedCompletion);
                    // The dispatched index is the event currently in service.
                    // It is carried separately because Drop oldest may replace
                    // pending entries without disturbing the active service.
                    result.DispatchedEventIndices.Add(inServiceIndex);
                    if (pending.Count > 0)
                    {
                        inServiceIndex = pending.Dequeue();
                        boundedCompletion = checked(boundedCompletion + serviceMicroseconds);
                    }
                    else
                    {
                        busy = false;
                        inServiceIndex = -1;
                    }
                }

                int occupancy = pending.Count + (busy ? 1 : 0);
                if (occupancy < queueLengthLimit)
                {
                    if (!busy)
                    {
                        busy = true;
                        inServiceIndex = i;
                        boundedCompletion = checked(arrival + serviceMicroseconds);
                    }
                    else pending.Enqueue(i);
                    if (pending.Count > result.MaximumQueueLength) result.MaximumQueueLength = pending.Count;
                }
                else
                {
                    if (overflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                    {
                        pending.Dequeue();
                        result.DroppedEvents++;
                        pending.Enqueue(i);
                    }
                    else if (overflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                    {
                        result.DroppedEvents += occupancy + 1;
                        result.BufferClearCount++;
                        pending.Clear();
                        busy = false;
                        inServiceIndex = -1;
                        while (i + 1 < arrivals.Count && arrivals[i + 1] <= arrival)
                        {
                            result.DroppedEvents++;
                            i++;
                        }
                    }
                    else result.DroppedEvents++;
                }
            }

            while (busy)
            {
                result.DispatchMicroseconds.Add(boundedCompletion);
                result.DispatchedEventIndices.Add(inServiceIndex);
                if (pending.Count > 0)
                {
                    inServiceIndex = pending.Dequeue();
                    boundedCompletion = checked(boundedCompletion + serviceMicroseconds);
                }
                else busy = false;
            }
            return result;
        }

    }
}
