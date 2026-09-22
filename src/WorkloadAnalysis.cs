using System;
using System.Collections.Generic;
using System.Threading;
using System.Runtime.CompilerServices;

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
        public bool PerNoteIntervalGateEnabled;
    }

    internal sealed class MessageTypeWorkload
    {
        public MidiEventKind Kind;
        public long EventCount;
        public long ByteCount;
    }

    internal enum AnalysisProjectionState
    {
        Complete,
        Pending,
        Cancelled,
        Failed
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
        public long PredictedOutputCompletionMicroseconds;
        public double EventServiceCapacityPerSecond;
        public double ByteServiceCapacityPerSecond;
        public AnalysisProjectionState ProjectionState;

        public bool HasQueueProjection { get { return ProjectionState == AnalysisProjectionState.Complete; } }

        internal WorkloadAnalysis CopyWorkload(AnalysisConfiguration configuration, CancellationToken token)
        {
            WorkloadAnalysis copy = (WorkloadAnalysis)MemberwiseClone();
            copy.Configuration = configuration;
            copy.Buckets = new WorkloadBucket[Buckets.Length];
            for (int i = 0; i < Buckets.Length; i++)
            {
                if ((i & 4095) == 0) token.ThrowIfCancellationRequested();
                copy.Buckets[i] = new WorkloadBucket { EventCount = Buckets[i].EventCount,
                    ByteCount = Buckets[i].ByteCount, LargestCluster = Buckets[i].LargestCluster };
            }
            return copy;
        }

        internal WorkloadAnalysis WithConfiguration(AnalysisConfiguration configuration)
        {
            if (Configuration != null && configuration != null &&
                Configuration.SimulateSlowdown == configuration.SimulateSlowdown &&
                Configuration.ServiceDurationMode == configuration.ServiceDurationMode &&
                Configuration.ProcessingMicroseconds == configuration.ProcessingMicroseconds &&
                Configuration.MidiBitrate == configuration.MidiBitrate &&
                Configuration.QueueLengthLimitEnabled == configuration.QueueLengthLimitEnabled &&
                Configuration.QueueLengthLimit == configuration.QueueLengthLimit &&
                Configuration.OverflowPolicy == configuration.OverflowPolicy &&
                Configuration.PerNoteIntervalGateEnabled == configuration.PerNoteIntervalGateEnabled) return this;
            WorkloadAnalysis copy = (WorkloadAnalysis)MemberwiseClone();
            copy.Configuration = configuration;
            return copy;
        }

        internal WorkloadAnalysis WithProjectionState(AnalysisProjectionState state)
        {
            if (ProjectionState == state) return this;
            WorkloadAnalysis copy = (WorkloadAnalysis)MemberwiseClone();
            copy.ProjectionState = state;
            return copy;
        }
    }

    // Immutable ownership boundary around the reusable file-workload cache.
    // No mutable bucket or message-type collection is exposed. Consumers can
    // request an independent presentation copy without reaching into cache
    // ownership or allowing projection work to mutate a published preview.
    internal sealed class WorkloadBaseAnalysis
    {
        private readonly WorkloadAnalysis _workload;

        internal WorkloadBaseAnalysis(WorkloadAnalysis workload)
        {
            if (workload == null) throw new ArgumentNullException("workload");
            _workload = workload;
        }

        internal long BucketMicroseconds { get { return _workload.BucketMicroseconds; } }

        internal WorkloadAnalysis CreatePresentationAnalysis(AnalysisConfiguration configuration,
            CancellationToken cancellationToken)
        {
            WorkloadAnalysis preview = _workload.CopyWorkload(configuration, cancellationToken);
            preview.ProjectionState = AnalysisProjectionState.Pending;
            return preview;
        }
    }

    internal static class WorkloadAnalyzer
    {
        public const long DefaultBucketMicroseconds = 100000;
        public const int MaximumBucketCount = 2000000;
        private sealed class SongWorkloads
        {
            public readonly Dictionary<long, WorkloadAnalysis> Results = new Dictionary<long, WorkloadAnalysis>();
            public readonly Queue<long> Order = new Queue<long>();
            public int Buckets;
        }
        private static readonly ConditionalWeakTable<MidiSong, SongWorkloads> Workloads = new ConditionalWeakTable<MidiSong, SongWorkloads>();
        internal static long WorkloadScanCount;

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
            return Analyze(song, bucketMicroseconds, configuration, cancellationToken, progress, null);
        }

        public static WorkloadAnalysis Analyze(MidiSong song, long bucketMicroseconds, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress,
            Action<WorkloadBaseAnalysis> workloadReady)
        {
            if (song == null) throw new ArgumentNullException("song");
            MidiEventReader events = song.GetEventReader();
            SongWorkloads cache = Workloads.GetOrCreateValue(song);
            WorkloadAnalysis workload;
            while (!Monitor.TryEnter(cache, 25)) cancellationToken.ThrowIfCancellationRequested();
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!cache.Results.TryGetValue(bucketMicroseconds, out workload))
                {
                    workload = AnalyzeUncached(song, bucketMicroseconds, null, cancellationToken, progress);
                    while (cache.Order.Count > 0 && (cache.Results.Count >= 2 ||
                        cache.Buckets + workload.Buckets.Length > MaximumBucketCount))
                    {
                        long old = cache.Order.Dequeue();
                        cache.Buckets -= cache.Results[old].Buckets.Length;
                        cache.Results.Remove(old);
                    }
                    cache.Results.Add(bucketMicroseconds, workload);
                    cache.Order.Enqueue(bucketMicroseconds);
                    cache.Buckets += workload.Buckets.Length;
                }
            }
            finally { Monitor.Exit(cache); }
            cancellationToken.ThrowIfCancellationRequested();
            if (workloadReady != null) workloadReady(new WorkloadBaseAnalysis(workload));
            WorkloadAnalysis result = workload.CopyWorkload(configuration, cancellationToken);
            Report(progress, "Reusing file workload", 1, 1, 0, 750);
            if (configuration == null) { Report(progress, "Complete", 1, 1, 0, 1000); return result; }
            if (configuration.SimulateSlowdown)
            {
                if (configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime)
                {
                    for (int i = 0; i < result.Buckets.Length; i++)
                    {
                        if ((i & 4095) == 0) cancellationToken.ThrowIfCancellationRequested();
                        result.Buckets[i].ServiceDemandMicroseconds = checked(result.Buckets[i].EventCount * configuration.ProcessingMicroseconds);
                    }
                    if (configuration.ProcessingMicroseconds > 0)
                        result.EventServiceCapacityPerSecond = 1000000.0 / configuration.ProcessingMicroseconds;
                }
                else
                {
                    // Per-message rounding is exact; aggregate byte totals
                    // cannot substitute for the sum of rounded service times.
                    for (int i = 0; i < events.Count; i++)
                    {
                        if ((i & 16383) == 0)
                        {
                            cancellationToken.ThrowIfCancellationRequested();
                            Report(progress, "Calculating serial service demand", i, events.Count, 750, 0);
                        }
                        MidiEvent midiEvent = events[i];
                        int bucket = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, midiEvent.IntendedMicroseconds / bucketMicroseconds));
                        result.Buckets[bucket].ServiceDemandMicroseconds += EventServiceMicroseconds(midiEvent, configuration);
                    }
                    result.ByteServiceCapacityPerSecond = configuration.MidiBitrate / 10.0;
                }
            }
            AnalyzePressure(song, result, configuration, cancellationToken, progress);
            result.ProjectionState = AnalysisProjectionState.Complete;
            return result;
        }

        internal static WorkloadAnalysis AnalyzeUncached(MidiSong song, long bucketMicroseconds, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            if (song == null) throw new ArgumentNullException("song");
            MidiEventReader events = song.GetEventReader();
            Interlocked.Increment(ref WorkloadScanCount);
            int bucketCount = CalculateBucketCount(song.DurationMicroseconds, bucketMicroseconds);
            WorkloadAnalysis result = new WorkloadAnalysis
            {
                Configuration = configuration,
                TotalEvents = events.Count,
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
            while (index < events.Count)
            {
                if ((index & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Scanning events and clusters", index, events.Count, 50, 650);
                }
                long timestamp = events[index].IntendedMicroseconds;
                int bucketIndex = (int)Math.Min(bucketCount - 1, Math.Max(0, timestamp / bucketMicroseconds));
                int clusterEnd = index;
                while (clusterEnd < events.Count && events[clusterEnd].IntendedMicroseconds == timestamp)
                {
                    if ((clusterEnd & 16383) == 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        Report(progress, "Scanning events and clusters", clusterEnd, events.Count, 50, 650);
                    }
                    MidiEvent midiEvent = events[clusterEnd];
                    int bytes = midiEvent.DataLength;
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
            Report(progress, "Scanning events and clusters", events.Count, events.Count, 50, 650);

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
                result.AverageEventsPerSecond = events.Count / (song.DurationMicroseconds / 1000000.0);

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
            else Report(progress, "File workload complete", 1, 1, 0, 750);
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
            MidiEventReader events = song.GetEventReader();
            if (!configuration.SimulateSlowdown ||
                configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime && configuration.ProcessingMicroseconds == 0)
            {
                result.PredictedMaximumOccupancy = events.Count == 0 ? 0 : 1;
                result.PredictedOutputCompletionMicroseconds = events.Count == 0
                    ? 0 : events[events.Count - 1].IntendedMicroseconds;
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
            long lastCompleted = 0;
            long pendingServiceMicroseconds = 0;
            int limit = Math.Max(1, configuration.QueueLengthLimit);
            CompleteNoteTracker completeNotes = new CompleteNoteTracker();
            for (int i = 0; i < events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting finite queue pressure", i, events.Count, 750, 250);
                }
                long arrival = events[i].IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    lastCompleted = completion;
                    if (pending.Count > 0)
                    {
                        int next = pending.Dequeue();
                        long nextService = EventServiceMicroseconds(events[next], configuration);
                        pendingServiceMicroseconds = checked(pendingServiceMicroseconds - nextService);
                        completion = checked(completion + nextService);
                    }
                    else busy = false;
                }

                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                MidiEvent incomingEvent = events[i];
                CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(incomingEvent);
                if (noteKind == CompleteNoteEventKind.NoteOff && completeNotes.ShouldSuppressNoteOff(incomingEvent))
                {
                    RecordDrop(result, bucketIndex, 1);
                    continue;
                }
                int occupancy = pending.Count + (busy ? 1 : 0);
                bool safetyAdmission = configuration.OverflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                    noteKind != CompleteNoteEventKind.NoteOn;
                if (occupancy < limit || safetyAdmission)
                {
                    if (!busy)
                    {
                        busy = true;
                        completion = checked(arrival + EventServiceMicroseconds(events[i], configuration));
                    }
                    else
                    {
                        pending.Enqueue(i);
                        pendingServiceMicroseconds = checked(pendingServiceMicroseconds +
                            EventServiceMicroseconds(events[i], configuration));
                    }
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, true, false);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                {
                    int removed = pending.Dequeue();
                    pendingServiceMicroseconds = checked(pendingServiceMicroseconds -
                        EventServiceMicroseconds(events[removed], configuration));
                    pending.Enqueue(i);
                    pendingServiceMicroseconds = checked(pendingServiceMicroseconds +
                        EventServiceMicroseconds(events[i], configuration));
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, true, false);
                    RecordDrop(result, bucketIndex, 1);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                {
                    int dropped = occupancy + 1;
                    pending.Clear();
                    pendingServiceMicroseconds = 0;
                    busy = false;
                    while (i + 1 < events.Count && events[i + 1].IntendedMicroseconds <= arrival) { dropped++; i++; }
                    RecordDrop(result, bucketIndex, dropped);
                    result.PredictedBufferClears++;
                    result.Buckets[bucketIndex].PredictedBufferClears++;
                }
                else
                {
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, false,
                            configuration.OverflowPolicy == OverflowPolicy.DropIncomingCompleteNotes);
                    RecordDrop(result, bucketIndex, 1);
                }

                occupancy = pending.Count + (busy ? 1 : 0);
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
            result.PredictedOutputCompletionMicroseconds = busy
                ? checked(completion + pendingServiceMicroseconds) : lastCompleted;
            Report(progress, "Projecting finite queue pressure", events.Count, events.Count, 750, 250);
        }

        private static void AnalyzeUnlimitedPressure(MidiSong song, WorkloadAnalysis result, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            MidiEventReader events = song.GetEventReader();
            bool busy = false;
            long completion = 0;
            long lastCompleted = 0;
            long pendingServiceMicroseconds = 0;
            int nextPending = 0;
            for (int i = 0; i < events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting unlimited queue pressure", i, events.Count, 750, 250);
                }
                long arrival = events[i].IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    lastCompleted = completion;
                    if (nextPending < i)
                    {
                        long nextService = EventServiceMicroseconds(events[nextPending], configuration);
                        pendingServiceMicroseconds = checked(pendingServiceMicroseconds - nextService);
                        completion = checked(completion + nextService);
                        nextPending++;
                    }
                    else busy = false;
                }
                if (!busy)
                {
                    busy = true;
                    completion = checked(arrival + EventServiceMicroseconds(events[i], configuration));
                    nextPending = i + 1;
                }
                else
                    pendingServiceMicroseconds = checked(pendingServiceMicroseconds +
                        EventServiceMicroseconds(events[i], configuration));
                int occupancy = 1 + Math.Max(0, i + 1 - nextPending);
                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
            result.PredictedOutputCompletionMicroseconds = busy
                ? checked(completion + pendingServiceMicroseconds) : lastCompleted;
            Report(progress, "Projecting unlimited queue pressure", events.Count, events.Count, 750, 250);
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
