using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal sealed class SimulationResult
    {
        public readonly List<long> DispatchMicroseconds = new List<long>();
        public int DroppedEvents;
        public int MaximumQueueLength;
    }

    internal static class BottleneckSimulator
    {
        // Deterministic reference model used by tests and kept separate from the
        // real-time scheduler. A service interval completes before an arrival at
        // the exact same timestamp, so that arrival sees an available processor.
        public static SimulationResult Run(IList<long> arrivals, long serviceMicroseconds, ProcessingMode mode)
        {
            if (arrivals == null) throw new ArgumentNullException("arrivals");
            if (serviceMicroseconds < 0) throw new ArgumentOutOfRangeException("serviceMicroseconds");

            SimulationResult result = new SimulationResult();
            if (mode == ProcessingMode.Queue)
            {
                long processorAvailable = 0;
                int arrived = 0;
                int processed = 0;
                for (int i = 0; i < arrivals.Count; i++)
                {
                    long start = Math.Max(arrivals[i], processorAvailable);
                    long completion = checked(start + serviceMicroseconds);
                    result.DispatchMicroseconds.Add(completion);
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

            long busyUntil = -1;
            for (int i = 0; i < arrivals.Count; i++)
            {
                long arrival = arrivals[i];
                if (serviceMicroseconds == 0 || arrival >= busyUntil)
                {
                    busyUntil = checked(arrival + serviceMicroseconds);
                    result.DispatchMicroseconds.Add(busyUntil);
                }
                else
                {
                    result.DroppedEvents++;
                }
            }
            return result;
        }
    }
}
