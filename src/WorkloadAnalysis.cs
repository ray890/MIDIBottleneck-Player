using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal sealed class WorkloadBucket
    {
        public int EventCount;
        public long ByteCount;
        public int LargestCluster;
        public long ServiceDemandMicroseconds;
        public int PredictedPeakOccupancy;
        public int PredictedDroppedEvents;
        public int PredictedBufferClears;
    }

    internal sealed class AnalysisConfiguration
    {
        public bool SimulateSlowdown;
        public ServiceDurationMode ServiceDurationMode;
        public long ProcessingMicroseconds;
        public long MidiBitrate;
        public bool QueueLengthLimitEnabled;
        public int QueueLengthLimit;
        public OverflowPolicy OverflowPolicy;
    }

    internal sealed class MessageTypeWorkload
    {
        public MidiEventKind Kind;
        public long EventCount;
        public long ByteCount;
    }

    internal sealed class WorkloadAnalysis
    {
        public long TotalEvents;
        public long TotalBytes;
        public long UniqueTimestamps;
        public int LargestTimestampCluster;
        public double AverageEventsPerSecond;
        public double PeakEventsPerSecond;
        public double PeakBytesPerSecond;
        public long EventsInClustersAtLeast2;
        public long EventsInClustersAtLeast10;
        public long EventsInClustersAtLeast50;
        public long EventsInClustersAtLeast100;
        public long BucketMicroseconds;
        public long DurationMicroseconds;
        public WorkloadBucket[] Buckets;
        public List<MessageTypeWorkload> MessageTypes;
        public AnalysisConfiguration Configuration;
        public int PredictedMaximumOccupancy;
        public long PredictedDroppedEvents;
        public int PredictedBufferClears;
        public double EventServiceCapacityPerSecond;
        public double ByteServiceCapacityPerSecond;
    }

    internal static class WorkloadAnalyzer
    {
        public const long DefaultBucketMicroseconds = 100000;

        public static WorkloadAnalysis Analyze(MidiSong song)
        {
            long bucketMicroseconds = DefaultBucketMicroseconds;
            if (song != null && song.DurationMicroseconds > bucketMicroseconds * 5000L)
            {
                long target = (song.DurationMicroseconds + 4999L) / 5000L;
                bucketMicroseconds = ((target + 9999L) / 10000L) * 10000L;
            }
            return Analyze(song, bucketMicroseconds, null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds)
        {
            return Analyze(song, bucketMicroseconds, null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, AnalysisConfiguration configuration)
        {
            long bucketMicroseconds = DefaultBucketMicroseconds;
            if (song != null && song.DurationMicroseconds > bucketMicroseconds * 5000L)
            {
                long target = (song.DurationMicroseconds + 4999L) / 5000L;
                bucketMicroseconds = ((target + 9999L) / 10000L) * 10000L;
            }
            return Analyze(song, bucketMicroseconds, configuration);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds, AnalysisConfiguration configuration)
        {
            if (song == null) throw new ArgumentNullException("song");
            if (bucketMicroseconds <= 0) throw new ArgumentOutOfRangeException("bucketMicroseconds");

            WorkloadAnalysis result = new WorkloadAnalysis();
            result.Configuration = configuration;
            result.TotalEvents = song.Events.Count;
            result.DurationMicroseconds = song.DurationMicroseconds;
            result.BucketMicroseconds = bucketMicroseconds;
            int bucketCount = Math.Max(1, (int)Math.Min(Int32.MaxValue, (song.DurationMicroseconds / bucketMicroseconds) + 1));
            result.Buckets = new WorkloadBucket[bucketCount];
            for (int i = 0; i < bucketCount; i++) result.Buckets[i] = new WorkloadBucket();

            Dictionary<MidiEventKind, MessageTypeWorkload> types = new Dictionary<MidiEventKind, MessageTypeWorkload>();
            int index = 0;
            while (index < song.Events.Count)
            {
                long timestamp = song.Events[index].IntendedMicroseconds;
                int clusterEnd = index + 1;
                while (clusterEnd < song.Events.Count && song.Events[clusterEnd].IntendedMicroseconds == timestamp) clusterEnd++;
                int clusterSize = clusterEnd - index;
                result.UniqueTimestamps++;
                if (clusterSize > result.LargestTimestampCluster) result.LargestTimestampCluster = clusterSize;
                if (clusterSize >= 2) result.EventsInClustersAtLeast2 += clusterSize;
                if (clusterSize >= 10) result.EventsInClustersAtLeast10 += clusterSize;
                if (clusterSize >= 50) result.EventsInClustersAtLeast50 += clusterSize;
                if (clusterSize >= 100) result.EventsInClustersAtLeast100 += clusterSize;

                int bucketIndex = (int)Math.Min(bucketCount - 1, Math.Max(0, timestamp / bucketMicroseconds));
                if (clusterSize > result.Buckets[bucketIndex].LargestCluster)
                    result.Buckets[bucketIndex].LargestCluster = clusterSize;

                for (int eventIndex = index; eventIndex < clusterEnd; eventIndex++)
                {
                    MidiEvent midiEvent = song.Events[eventIndex];
                    int bytes = midiEvent.Data == null ? 0 : midiEvent.Data.Length;
                    result.TotalBytes += bytes;
                    result.Buckets[bucketIndex].EventCount++;
                    result.Buckets[bucketIndex].ByteCount += bytes;
                    if (configuration != null && configuration.SimulateSlowdown)
                        result.Buckets[bucketIndex].ServiceDemandMicroseconds += ServiceDurationCalculator.CalculateMicroseconds(
                            midiEvent, configuration.ServiceDurationMode, configuration.ProcessingMicroseconds, configuration.MidiBitrate);

                    MessageTypeWorkload type;
                    if (!types.TryGetValue(midiEvent.Kind, out type))
                    {
                        type = new MessageTypeWorkload();
                        type.Kind = midiEvent.Kind;
                        types.Add(midiEvent.Kind, type);
                    }
                    type.EventCount++;
                    type.ByteCount += bytes;
                }
                index = clusterEnd;
            }

            double bucketSeconds = bucketMicroseconds / 1000000.0;
            for (int i = 0; i < result.Buckets.Length; i++)
            {
                result.PeakEventsPerSecond = Math.Max(result.PeakEventsPerSecond, result.Buckets[i].EventCount / bucketSeconds);
                result.PeakBytesPerSecond = Math.Max(result.PeakBytesPerSecond, result.Buckets[i].ByteCount / bucketSeconds);
            }
            if (song.DurationMicroseconds > 0)
                result.AverageEventsPerSecond = song.Events.Count / (song.DurationMicroseconds / 1000000.0);

            result.MessageTypes = new List<MessageTypeWorkload>(types.Values);
            result.MessageTypes.Sort(delegate(MessageTypeWorkload left, MessageTypeWorkload right) { return left.Kind.CompareTo(right.Kind); });
            if (configuration != null)
            {
                if (configuration.SimulateSlowdown && configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime && configuration.ProcessingMicroseconds > 0)
                    result.EventServiceCapacityPerSecond = 1000000.0 / configuration.ProcessingMicroseconds;
                if (configuration.SimulateSlowdown && configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate && configuration.MidiBitrate > 0)
                    result.ByteServiceCapacityPerSecond = configuration.MidiBitrate / 10.0;
                AnalyzePressure(song, result, configuration);
            }
            return result;
        }

        private static void AnalyzePressure(MidiSong song, WorkloadAnalysis result, AnalysisConfiguration configuration)
        {
            Queue<int> pending = new Queue<int>();
            bool busy = false;
            long completion = 0;
            int inService = -1;
            int limit = Math.Max(1, configuration.QueueLengthLimit);

            for (int i = 0; i < song.Events.Count; i++)
            {
                long arrival = song.Events[i].IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    if (pending.Count > 0)
                    {
                        inService = pending.Dequeue();
                        completion = checked(completion + EventServiceMicroseconds(song.Events[inService], configuration));
                    }
                    else
                    {
                        busy = false;
                        inService = -1;
                    }
                }

                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                int occupancy = pending.Count + (busy ? 1 : 0);
                bool hasRoom = !configuration.QueueLengthLimitEnabled || occupancy < limit;
                if (hasRoom)
                {
                    if (!busy)
                    {
                        busy = true;
                        inService = i;
                        completion = checked(arrival + EventServiceMicroseconds(song.Events[i], configuration));
                    }
                    else pending.Enqueue(i);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                {
                    pending.Dequeue();
                    pending.Enqueue(i);
                    result.PredictedDroppedEvents++;
                    result.Buckets[bucketIndex].PredictedDroppedEvents++;
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                {
                    int dropped = occupancy + 1;
                    pending.Clear();
                    busy = false;
                    inService = -1;
                    while (i + 1 < song.Events.Count && song.Events[i + 1].IntendedMicroseconds <= arrival)
                    {
                        dropped++;
                        i++;
                    }
                    result.PredictedDroppedEvents += dropped;
                    result.PredictedBufferClears++;
                    result.Buckets[bucketIndex].PredictedDroppedEvents += dropped;
                    result.Buckets[bucketIndex].PredictedBufferClears++;
                }
                else
                {
                    result.PredictedDroppedEvents++;
                    result.Buckets[bucketIndex].PredictedDroppedEvents++;
                }

                occupancy = pending.Count + (busy ? 1 : 0);
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
        }

        private static long EventServiceMicroseconds(MidiEvent midiEvent, AnalysisConfiguration configuration)
        {
            if (!configuration.SimulateSlowdown) return 0;
            return ServiceDurationCalculator.CalculateMicroseconds(midiEvent, configuration.ServiceDurationMode,
                configuration.ProcessingMicroseconds, configuration.MidiBitrate);
        }
    }
}
