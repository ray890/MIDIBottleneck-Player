using System;
using System.Collections.Generic;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class WorkloadAnalysisProgress
    {
        public readonly string Stage;
        public readonly int OverallPermille;
        public readonly int StagePermille;

        public WorkloadAnalysisProgress(string stage, long completed, long total, int overallStart, int overallLength)
        {
            Stage = stage;
            long safeTotal = Math.Max(1, total);
            long safeCompleted = Math.Max(0, Math.Min(safeTotal, completed));
            StagePermille = (int)(safeCompleted * 1000L / safeTotal);
            OverallPermille = Math.Min(1000, overallStart + (int)(safeCompleted * overallLength / safeTotal));
        }
    }

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
        public const int MaximumBucketCount = 2000000;

        public static WorkloadAnalysis Analyze(MidiSong song)
        {
            return Analyze(song, DefaultResolution(song), null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds)
        {
            return Analyze(song, bucketMicroseconds, null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, AnalysisConfiguration configuration)
        {
            return Analyze(song, DefaultResolution(song), configuration);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds, AnalysisConfiguration configuration)
        {
            return Analyze(song, bucketMicroseconds, configuration, CancellationToken.None, null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds, AnalysisConfiguration configuration,
            CancellationToken cancellationToken)
        {
            return Analyze(song, bucketMicroseconds, configuration, cancellationToken, null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            if (song == null) throw new ArgumentNullException("song");
            int bucketCount = CalculateBucketCount(song.DurationMicroseconds, bucketMicroseconds);
            WorkloadAnalysis result = new WorkloadAnalysis
            {
                Configuration = configuration,
                TotalEvents = song.Events.Count,
                DurationMicroseconds = song.DurationMicroseconds,
                BucketMicroseconds = bucketMicroseconds,
                Buckets = new WorkloadBucket[bucketCount]
            };

            for (int i = 0; i < bucketCount; i++)
            {
                if ((i & 4095) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Initializing graph buckets", i, bucketCount, 0, 50);
                }
                result.Buckets[i] = new WorkloadBucket();
            }
            Report(progress, "Initializing graph buckets", bucketCount, bucketCount, 0, 50);

            int typeCount = Enum.GetValues(typeof(MidiEventKind)).Length;
            long[] typeEvents = new long[typeCount];
            long[] typeBytes = new long[typeCount];
            int index = 0;
            while (index < song.Events.Count)
            {
                if ((index & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Scanning events and clusters", index, song.Events.Count, 50, 650);
                }
                long timestamp = song.Events[index].IntendedMicroseconds;
                int bucketIndex = (int)Math.Min(bucketCount - 1, Math.Max(0, timestamp / bucketMicroseconds));
                int clusterEnd = index;
                while (clusterEnd < song.Events.Count && song.Events[clusterEnd].IntendedMicroseconds == timestamp)
                {
                    if ((clusterEnd & 16383) == 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        Report(progress, "Scanning events and clusters", clusterEnd, song.Events.Count, 50, 650);
                    }
                    MidiEvent midiEvent = song.Events[clusterEnd];
                    int bytes = midiEvent.Data == null ? 0 : midiEvent.Data.Length;
                    result.TotalBytes += bytes;
                    result.Buckets[bucketIndex].EventCount++;
                    result.Buckets[bucketIndex].ByteCount += bytes;
                    if (configuration != null && configuration.SimulateSlowdown)
                        result.Buckets[bucketIndex].ServiceDemandMicroseconds += ServiceDurationCalculator.CalculateMicroseconds(
                            midiEvent, configuration.ServiceDurationMode, configuration.ProcessingMicroseconds, configuration.MidiBitrate);
                    int kind = (int)midiEvent.Kind;
                    typeEvents[kind]++;
                    typeBytes[kind] += bytes;
                    clusterEnd++;
                }
                int clusterSize = clusterEnd - index;
                result.UniqueTimestamps++;
                if (clusterSize > result.LargestTimestampCluster) result.LargestTimestampCluster = clusterSize;
                if (clusterSize >= 2) result.EventsInClustersAtLeast2 += clusterSize;
                if (clusterSize >= 10) result.EventsInClustersAtLeast10 += clusterSize;
                if (clusterSize >= 50) result.EventsInClustersAtLeast50 += clusterSize;
                if (clusterSize >= 100) result.EventsInClustersAtLeast100 += clusterSize;

                if (clusterSize > result.Buckets[bucketIndex].LargestCluster)
                    result.Buckets[bucketIndex].LargestCluster = clusterSize;
                index = clusterEnd;
            }
            Report(progress, "Scanning events and clusters", song.Events.Count, song.Events.Count, 50, 650);

            double bucketSeconds = bucketMicroseconds / 1000000.0;
            for (int i = 0; i < result.Buckets.Length; i++)
            {
                if ((i & 4095) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Summarizing graph buckets", i, result.Buckets.Length, 700, 50);
                }
                result.PeakEventsPerSecond = Math.Max(result.PeakEventsPerSecond, result.Buckets[i].EventCount / bucketSeconds);
                result.PeakBytesPerSecond = Math.Max(result.PeakBytesPerSecond, result.Buckets[i].ByteCount / bucketSeconds);
            }
            Report(progress, "Summarizing graph buckets", result.Buckets.Length, result.Buckets.Length, 700, 50);
            if (song.DurationMicroseconds > 0)
                result.AverageEventsPerSecond = song.Events.Count / (song.DurationMicroseconds / 1000000.0);

            result.MessageTypes = new List<MessageTypeWorkload>(typeCount);
            for (int kind = 0; kind < typeCount; kind++)
            {
                if (typeEvents[kind] == 0) continue;
                result.MessageTypes.Add(new MessageTypeWorkload
                {
                    Kind = (MidiEventKind)kind,
                    EventCount = typeEvents[kind],
                    ByteCount = typeBytes[kind]
                });
            }

            if (configuration != null)
            {
                if (configuration.SimulateSlowdown && configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime && configuration.ProcessingMicroseconds > 0)
                    result.EventServiceCapacityPerSecond = 1000000.0 / configuration.ProcessingMicroseconds;
                if (configuration.SimulateSlowdown && configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate && configuration.MidiBitrate > 0)
                    result.ByteServiceCapacityPerSecond = configuration.MidiBitrate / 10.0;
                AnalyzePressure(song, result, configuration, cancellationToken, progress);
            }
            else Report(progress, "Complete", 1, 1, 750, 250);
            return result;
        }

        private static long DefaultResolution(MidiSong song)
        {
            long resolution = DefaultBucketMicroseconds;
            if (song != null && song.DurationMicroseconds > resolution * 5000L)
            {
                long target = (song.DurationMicroseconds + 4999L) / 5000L;
                resolution = ((target + 9999L) / 10000L) * 10000L;
            }
            return resolution;
        }

        internal static int CalculateBucketCount(long durationMicroseconds, long bucketMicroseconds)
        {
            if (bucketMicroseconds <= 0) throw new ArgumentOutOfRangeException("bucketMicroseconds");
            long bucketCount = durationMicroseconds / bucketMicroseconds + 1;
            if (bucketCount > MaximumBucketCount)
                throw new InvalidOperationException("That resolution would require " + bucketCount.ToString("N0") +
                    " graph buckets. The safety limit is " + MaximumBucketCount.ToString("N0") +
                    " buckets (approximately 128 MiB before drawing caches). Choose a larger interval.");
            return Math.Max(1, (int)bucketCount);
        }

        private static void AnalyzePressure(MidiSong song, WorkloadAnalysis result, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            if (!configuration.SimulateSlowdown ||
                configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime && configuration.ProcessingMicroseconds == 0)
            {
                result.PredictedMaximumOccupancy = song.Events.Count == 0 ? 0 : 1;
                for (int bucket = 0; bucket < result.Buckets.Length; bucket++)
                    if (result.Buckets[bucket].EventCount > 0) result.Buckets[bucket].PredictedPeakOccupancy = 1;
                Report(progress, "Queue projection (instantaneous service)", 1, 1, 750, 250);
                return;
            }
            if (!configuration.QueueLengthLimitEnabled)
            {
                AnalyzeUnlimitedPressure(song, result, configuration, cancellationToken, progress);
                return;
            }

            Queue<int> pending = new Queue<int>(Math.Min(Math.Max(4, configuration.QueueLengthLimit), 1000000));
            bool busy = false;
            long completion = 0;
            int limit = Math.Max(1, configuration.QueueLengthLimit);
            for (int i = 0; i < song.Events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting finite queue pressure", i, song.Events.Count, 750, 250);
                }
                long arrival = song.Events[i].IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    if (pending.Count > 0)
                    {
                        int next = pending.Dequeue();
                        completion = checked(completion + EventServiceMicroseconds(song.Events[next], configuration));
                    }
                    else busy = false;
                }

                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                int occupancy = pending.Count + (busy ? 1 : 0);
                if (occupancy < limit)
                {
                    if (!busy)
                    {
                        busy = true;
                        completion = checked(arrival + EventServiceMicroseconds(song.Events[i], configuration));
                    }
                    else pending.Enqueue(i);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                {
                    pending.Dequeue();
                    pending.Enqueue(i);
                    RecordDrop(result, bucketIndex, 1);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                {
                    int dropped = occupancy + 1;
                    pending.Clear();
                    busy = false;
                    while (i + 1 < song.Events.Count && song.Events[i + 1].IntendedMicroseconds <= arrival) { dropped++; i++; }
                    RecordDrop(result, bucketIndex, dropped);
                    result.PredictedBufferClears++;
                    result.Buckets[bucketIndex].PredictedBufferClears++;
                }
                else RecordDrop(result, bucketIndex, 1);

                occupancy = pending.Count + (busy ? 1 : 0);
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
            Report(progress, "Projecting finite queue pressure", song.Events.Count, song.Events.Count, 750, 250);
        }

        private static void AnalyzeUnlimitedPressure(MidiSong song, WorkloadAnalysis result, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            bool busy = false;
            long completion = 0;
            int nextPending = 0;
            for (int i = 0; i < song.Events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting unlimited queue pressure", i, song.Events.Count, 750, 250);
                }
                long arrival = song.Events[i].IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    if (nextPending < i)
                    {
                        completion = checked(completion + EventServiceMicroseconds(song.Events[nextPending], configuration));
                        nextPending++;
                    }
                    else busy = false;
                }
                if (!busy)
                {
                    busy = true;
                    completion = checked(arrival + EventServiceMicroseconds(song.Events[i], configuration));
                    nextPending = i + 1;
                }
                int occupancy = 1 + Math.Max(0, i + 1 - nextPending);
                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
            Report(progress, "Projecting unlimited queue pressure", song.Events.Count, song.Events.Count, 750, 250);
        }

        private static void RecordDrop(WorkloadAnalysis result, int bucketIndex, int count)
        {
            result.PredictedDroppedEvents += count;
            result.Buckets[bucketIndex].PredictedDroppedEvents += count;
        }

        private static long EventServiceMicroseconds(MidiEvent midiEvent, AnalysisConfiguration configuration)
        {
            return ServiceDurationCalculator.CalculateMicroseconds(midiEvent, configuration.ServiceDurationMode,
                configuration.ProcessingMicroseconds, configuration.MidiBitrate);
        }

        private static void Report(Action<WorkloadAnalysisProgress> progress, string stage, long completed, long total,
            int overallStart, int overallLength)
        {
            if (progress != null) progress(new WorkloadAnalysisProgress(stage, completed, total, overallStart, overallLength));
        }
    }
}
