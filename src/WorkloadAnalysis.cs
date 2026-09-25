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
        public long EventsPerSecond;
        public bool QueueLengthLimitEnabled;
        public int QueueLengthLimit;
        public OverflowPolicy OverflowPolicy;
        public bool PerNoteIntervalGateEnabled;
        public bool ApplyQueueLimitWithoutSlowdown = true;
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
        // Allocated only for a configuration-specific Per-note projection.
        // The immutable cached source workload never owns a gate schedule.
        public long[] GateOutputBuckets;
        public long GateOutputEvents;
        public long GateOutputNoteTransitions;
        public long GateOutputNonNoteEvents;
        public long GateFilteredNoteEvents;
        public long GateOutputCompletionMicroseconds;
        public double GatePeakEventsPerSecond;
        public AnalysisProjectionState ProjectionState;
        public string ProjectionFailureReason;

        public bool HasGateProjection { get { return ProjectionState == AnalysisProjectionState.Complete &&
            Configuration != null && Configuration.PerNoteIntervalGateEnabled && GateOutputBuckets != null; } }
        public bool HasQueueProjection { get { return ProjectionState == AnalysisProjectionState.Complete &&
            (Configuration == null || !Configuration.PerNoteIntervalGateEnabled); } }

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
                Configuration.EventsPerSecond == configuration.EventsPerSecond &&
                Configuration.QueueLengthLimitEnabled == configuration.QueueLengthLimitEnabled &&
                Configuration.QueueLengthLimit == configuration.QueueLengthLimit &&
                Configuration.OverflowPolicy == configuration.OverflowPolicy &&
                Configuration.PerNoteIntervalGateEnabled == configuration.PerNoteIntervalGateEnabled &&
                Configuration.ApplyQueueLimitWithoutSlowdown == configuration.ApplyQueueLimitWithoutSlowdown) return this;
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

        internal WorkloadAnalysis WithProjectionFailure(string reason)
        {
            WorkloadAnalysis copy = (WorkloadAnalysis)MemberwiseClone();
            copy.ProjectionState = AnalysisProjectionState.Failed;
            copy.ProjectionFailureReason = reason;
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
            if (configuration.PerNoteIntervalGateEnabled)
            {
                ProjectPerNoteGate(song, result, configuration, cancellationToken, progress, null);
                result.ProjectionState = AnalysisProjectionState.Complete;
                return result;
            }
            if (UsesConfiguredService(configuration))
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
                else if (configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
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
                        MidiEventView midiEvent = events[i];
                        int bucket = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, midiEvent.IntendedMicroseconds / bucketMicroseconds));
                        result.Buckets[bucket].ServiceDemandMicroseconds += EventServiceMicroseconds(midiEvent, configuration);
                    }
                    result.ByteServiceCapacityPerSecond = configuration.MidiBitrate / 10.0;
                }
                else
                {
                    AddEventRateDemand(events, result, bucketMicroseconds, configuration,
                        cancellationToken, progress);
                    result.EventServiceCapacityPerSecond = configuration.EventsPerSecond;
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
            if (events.CompactStore != null)
                ScanCompactWorkload(events.CompactStore, result, bucketMicroseconds, bucketCount, configuration,
                    typeEvents, typeBytes, cancellationToken, progress);
            else
                ScanReferenceWorkload(events, result, bucketMicroseconds, bucketCount, configuration,
                    typeEvents, typeBytes, cancellationToken, progress);
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
                if (configuration.PerNoteIntervalGateEnabled)
                {
                    ProjectPerNoteGate(song, result, configuration, cancellationToken, progress, null);
                    result.ProjectionState = AnalysisProjectionState.Complete;
                    return result;
                }
                if (UsesConfiguredService(configuration) && configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime && configuration.ProcessingMicroseconds > 0)
                    result.EventServiceCapacityPerSecond = 1000000.0 / configuration.ProcessingMicroseconds;
                if (UsesConfiguredService(configuration) && configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate && configuration.MidiBitrate > 0)
                    result.ByteServiceCapacityPerSecond = configuration.MidiBitrate / 10.0;
                if (UsesConfiguredService(configuration) && configuration.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
                {
                    AddEventRateDemand(events, result, bucketMicroseconds, configuration,
                        cancellationToken, progress);
                    result.EventServiceCapacityPerSecond = configuration.EventsPerSecond;
                }
                AnalyzePressure(song, result, configuration, cancellationToken, progress);
            }
            else Report(progress, "File workload complete", 1, 1, 0, 750);
            return result;
        }

        // Runs the same bounded live gate state machine over immutable source
        // views. No second note-selection model or per-event schedule exists.
        // The optional observer is for deterministic sequence/parity tests;
        // production passes null and allocates no object per source event.
        internal static void ProjectPerNoteGate(MidiSong song, WorkloadAnalysis result,
            AnalysisConfiguration configuration, CancellationToken cancellationToken,
            Action<WorkloadAnalysisProgress> progress, Action<MidiEventView, long> observer)
        {
            if (configuration.ProcessingMicroseconds <= 0)
                throw new InvalidOperationException("Per-note Analysis requires a nonzero interval.");
            MidiEventReader events = song.GetEventReader();
            PerNoteIntervalGate gate = new PerNoteIntervalGate(configuration.ProcessingMicroseconds);
            MidiEventView[] emissions = new MidiEventView[128];
            result.GateOutputBuckets = new long[result.Buckets.Length];
            int next = 0;
            bool completed = false;
            long actions = 0;
            while (next < events.Count || !completed || gate.HasPendingTransitions)
            {
                if ((actions++ & 4095) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting Per-note intervals", next, Math.Max(1, events.Count), 750, 245);
                }
                long boundary = gate.NextBoundaryMicroseconds;
                long source = next < events.Count ? events[next].IntendedMicroseconds : Int64.MaxValue;
                if (next < events.Count && source <= boundary)
                {
                    long tick = events[next].AbsoluteTick;
                    gate.BeginSourceTick(tick);
                    while (next < events.Count && events[next].AbsoluteTick == tick)
                    {
                        if ((next & 16383) == 0) cancellationToken.ThrowIfCancellationRequested();
                        MidiEventView midiEvent = events[next++];
                        if (gate.Admit(midiEvent) == PerNoteGateAdmission.NotNote)
                        {
                            RecordGateOutput(result, midiEvent, midiEvent.IntendedMicroseconds, false);
                            if (observer != null) observer(midiEvent, midiEvent.IntendedMicroseconds);
                        }
                    }
                    gate.EndSourceTick();
                    result.GateFilteredNoteEvents += gate.TakeFilteredEventCount();
                }
                else if (boundary != Int64.MaxValue)
                {
                    int count = gate.EmitBoundary(boundary, emissions);
                    result.GateFilteredNoteEvents += gate.TakeFilteredEventCount();
                    for (int i = 0; i < count; i++)
                    {
                        RecordGateOutput(result, emissions[i], boundary, true);
                        if (observer != null) observer(emissions[i], boundary);
                    }
                }
                if (next == events.Count && !completed)
                {
                    gate.CompleteSource(song.DurationMicroseconds);
                    result.GateFilteredNoteEvents += gate.TakeFilteredEventCount();
                    completed = true;
                }
            }
            double seconds = result.BucketMicroseconds / 1000000.0;
            for (int i = 0; i < result.GateOutputBuckets.Length; i++)
            {
                if ((i & 4095) == 0) cancellationToken.ThrowIfCancellationRequested();
                result.GatePeakEventsPerSecond = Math.Max(result.GatePeakEventsPerSecond,
                    result.GateOutputBuckets[i] / seconds);
            }
            Report(progress, "Per-note projection complete", 1, 1, 995, 5);
        }

        private static void RecordGateOutput(WorkloadAnalysis result, MidiEventView midiEvent,
            long logicalTime, bool noteTransition)
        {
            long requiredDuration = Math.Max(result.DurationMicroseconds, logicalTime);
            int required = CalculateBucketCount(requiredDuration, result.BucketMicroseconds);
            if (required > result.GateOutputBuckets.Length)
            {
                int previous = result.GateOutputBuckets.Length;
                int grown = Math.Min(MaximumBucketCount,
                    Math.Max(required, previous > MaximumBucketCount / 2 ? MaximumBucketCount : previous * 2));
                Array.Resize(ref result.GateOutputBuckets, grown);
                Array.Resize(ref result.Buckets, grown);
                for (int i = previous; i < grown; i++) result.Buckets[i] = new WorkloadBucket();
            }
            result.DurationMicroseconds = requiredDuration;
            int bucket = (int)Math.Min(result.GateOutputBuckets.Length - 1,
                Math.Max(0, logicalTime / result.BucketMicroseconds));
            result.GateOutputBuckets[bucket]++;
            result.GateOutputEvents++;
            if (noteTransition) result.GateOutputNoteTransitions++;
            else result.GateOutputNonNoteEvents++;
            result.GateOutputCompletionMicroseconds = Math.Max(result.GateOutputCompletionMicroseconds, logicalTime);
        }

        private static void ScanCompactWorkload(CompactMidiEventStore store, WorkloadAnalysis result,
            long bucketMicroseconds, int bucketCount, AnalysisConfiguration configuration,
            long[] typeEvents, long[] typeBytes, CancellationToken cancellationToken,
            Action<WorkloadAnalysisProgress> progress)
        {
            CompactMidiEventRecord[][] segments = store.RecordSegments;
            int count = store.Count;
            int index = 0;
            while (index < count)
            {
                if ((index & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Scanning events and clusters", index, count, 50, 650);
                }
                CompactMidiEventRecord first = segments[index / CompactMidiEventStore.RecordSegmentCapacity]
                    [index % CompactMidiEventStore.RecordSegmentCapacity];
                long timestamp = first.IntendedMicroseconds;
                int bucketIndex = (int)Math.Min(bucketCount - 1, Math.Max(0, timestamp / bucketMicroseconds));
                int clusterEnd = index;
                while (clusterEnd < count)
                {
                    CompactMidiEventRecord record = clusterEnd == index ? first :
                        segments[clusterEnd / CompactMidiEventStore.RecordSegmentCapacity]
                            [clusterEnd % CompactMidiEventStore.RecordSegmentCapacity];
                    if (record.IntendedMicroseconds != timestamp) break;
                    if ((clusterEnd & 16383) == 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        Report(progress, "Scanning events and clusters", clusterEnd, count, 50, 650);
                    }
                    int bytes = record.PayloadLength;
                    result.TotalBytes += bytes;
                    result.Buckets[bucketIndex].EventCount++;
                    result.Buckets[bucketIndex].ByteCount += bytes;
                    if (configuration != null && UsesConfiguredService(configuration) &&
                        configuration.ServiceDurationMode != ServiceDurationMode.EventsPerSecond)
                        result.Buckets[bucketIndex].ServiceDemandMicroseconds +=
                            configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime
                                ? Math.Max(0, configuration.ProcessingMicroseconds)
                                : ServiceDurationCalculator.CalculateBitrateMicroseconds(bytes, configuration.MidiBitrate);
                    int kind = record.Metadata & 0x0F;
                    typeEvents[kind]++;
                    typeBytes[kind] += bytes;
                    clusterEnd++;
                }
                RecordWorkloadCluster(result, bucketIndex, clusterEnd - index);
                index = clusterEnd;
            }
        }

        private static void ScanReferenceWorkload(MidiEventReader events, WorkloadAnalysis result,
            long bucketMicroseconds, int bucketCount, AnalysisConfiguration configuration,
            long[] typeEvents, long[] typeBytes, CancellationToken cancellationToken,
            Action<WorkloadAnalysisProgress> progress)
        {
            int index = 0;
            while (index < events.Count)
            {
                if ((index & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Scanning events and clusters", index, events.Count, 50, 650);
                }
                MidiEventView first = events[index];
                long timestamp = first.IntendedMicroseconds;
                int bucketIndex = (int)Math.Min(bucketCount - 1, Math.Max(0, timestamp / bucketMicroseconds));
                int clusterEnd = index;
                while (clusterEnd < events.Count)
                {
                    MidiEventView midiEvent = clusterEnd == index ? first : events[clusterEnd];
                    if (midiEvent.IntendedMicroseconds != timestamp) break;
                    if ((clusterEnd & 16383) == 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        Report(progress, "Scanning events and clusters", clusterEnd, events.Count, 50, 650);
                    }
                    int bytes = midiEvent.DataLength;
                    result.TotalBytes += bytes;
                    result.Buckets[bucketIndex].EventCount++;
                    result.Buckets[bucketIndex].ByteCount += bytes;
                    if (configuration != null && UsesConfiguredService(configuration) &&
                        configuration.ServiceDurationMode != ServiceDurationMode.EventsPerSecond)
                        result.Buckets[bucketIndex].ServiceDemandMicroseconds += ServiceDurationCalculator.CalculateMicroseconds(
                            midiEvent, configuration.ServiceDurationMode, configuration.ProcessingMicroseconds, configuration.MidiBitrate);
                    int kind = (int)midiEvent.Kind;
                    typeEvents[kind]++;
                    typeBytes[kind] += bytes;
                    clusterEnd++;
                }
                RecordWorkloadCluster(result, bucketIndex, clusterEnd - index);
                index = clusterEnd;
            }
        }

        private static void RecordWorkloadCluster(WorkloadAnalysis result, int bucketIndex, int clusterSize)
        {
            result.UniqueTimestamps++;
            if (clusterSize > result.LargestTimestampCluster) result.LargestTimestampCluster = clusterSize;
            if (clusterSize >= 2) result.EventsInClustersAtLeast2 += clusterSize;
            if (clusterSize >= 10) result.EventsInClustersAtLeast10 += clusterSize;
            if (clusterSize >= 50) result.EventsInClustersAtLeast50 += clusterSize;
            if (clusterSize >= 100) result.EventsInClustersAtLeast100 += clusterSize;
            if (clusterSize > result.Buckets[bucketIndex].LargestCluster)
                result.Buckets[bucketIndex].LargestCluster = clusterSize;
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

        private static void AddEventRateDemand(MidiEventReader events, WorkloadAnalysis result,
            long bucketMicroseconds, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            ServiceDurationClock clock = CreateServiceClock(configuration);
            for (int i = 0; i < events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Calculating event-rate service demand", i, events.Count, 750, 0);
                }
                MidiEventView midiEvent = events[i];
                int bucket = (int)Math.Min(result.Buckets.Length - 1,
                    Math.Max(0, midiEvent.IntendedMicroseconds / bucketMicroseconds));
                result.Buckets[bucket].ServiceDemandMicroseconds += clock.NextMicroseconds(midiEvent);
            }
        }

        private static ServiceDurationClock CreateServiceClock(AnalysisConfiguration configuration)
        {
            return new ServiceDurationClock(configuration.ServiceDurationMode,
                configuration.ProcessingMicroseconds, configuration.MidiBitrate,
                configuration.EventsPerSecond);
        }

        private static bool UsesConfiguredService(AnalysisConfiguration configuration)
        {
            return configuration != null && !configuration.PerNoteIntervalGateEnabled &&
                (configuration.SimulateSlowdown ||
                configuration.ApplyQueueLimitWithoutSlowdown && configuration.QueueLengthLimitEnabled);
        }

        private static void AnalyzePressure(MidiSong song, WorkloadAnalysis result, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            MidiEventReader events = song.GetEventReader();
            bool virtualForward = !configuration.SimulateSlowdown && configuration.QueueLengthLimitEnabled &&
                configuration.ApplyQueueLimitWithoutSlowdown;
            if (virtualForward)
            {
                AnalyzeVirtualForwardPressure(song, result, configuration, cancellationToken, progress);
                return;
            }
            if (!configuration.SimulateSlowdown ||
                configuration.ServiceDurationMode == ServiceDurationMode.ProcessingTime && configuration.ProcessingMicroseconds == 0 ||
                configuration.ServiceDurationMode == ServiceDurationMode.EventsPerSecond && configuration.EventsPerSecond == 0)
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

            PendingMidiQueue pending = new PendingMidiQueue(configuration.QueueLengthLimit,
                configuration.OverflowPolicy == OverflowPolicy.DropOldestCompleteNote);
            bool busy = false;
            long completion = 0;
            long lastCompleted = 0;
            ServiceDurationClock serviceClock = CreateServiceClock(configuration);
            ServiceDurationClock beforeCurrentService = serviceClock;
            int limit = Math.Max(1, configuration.QueueLengthLimit);
            CompleteNoteTracker completeNotes = new CompleteNoteTracker();
            for (int i = 0; i < events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting finite queue pressure", i, events.Count, 750, 250);
                }
                MidiEventView incomingEvent = events[i];
                long arrival = incomingEvent.IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    lastCompleted = completion;
                    if (pending.Count > 0)
                    {
                        int next = pending.Dequeue();
                        beforeCurrentService = serviceClock;
                        long nextService = serviceClock.NextMicroseconds(events[next]);
                        completion = checked(completion + nextService);
                    }
                    else busy = false;
                }

                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(incomingEvent);
                CompleteNoteTracker.NoteOffMatch noteMatch = noteKind == CompleteNoteEventKind.NoteOff
                    ? completeNotes.TakeNoteOff(incomingEvent)
                    : new CompleteNoteTracker.NoteOffMatch { AttackIndex = -1 };
                if (noteMatch.Suppress)
                {
                    RecordDrop(result, bucketIndex, 1);
                    continue;
                }
                PendingMidiQueue.Entry incomingEntry = new PendingMidiQueue.Entry
                {
                    EventIndex = i,
                    PairedAttackIndex = noteMatch.AttackIndex,
                    IsNoteOn = noteKind == CompleteNoteEventKind.NoteOn,
                    IsNoteOff = noteKind == CompleteNoteEventKind.NoteOff
                };
                int occupancy = pending.Count + (busy ? 1 : 0);
                bool safetyAdmission = configuration.OverflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                    noteKind != CompleteNoteEventKind.NoteOn;
                if (occupancy < limit || safetyAdmission)
                {
                    if (!busy)
                    {
                        busy = true;
                        beforeCurrentService = serviceClock;
                        completion = checked(arrival + serviceClock.NextMicroseconds(incomingEvent));
                    }
                    else pending.Enqueue(incomingEntry);
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, i, true, false);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                {
                    pending.Dequeue();
                    pending.Enqueue(incomingEntry);
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, i, true, false);
                    RecordDrop(result, bucketIndex, 1);
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.DropOldestCompleteNote)
                {
                    int evictedAttack;
                    int evictedRelease;
                    bool evicted = pending.TryEvictOldestCompleteNote(out evictedAttack, out evictedRelease);
                    if (evicted)
                    {
                        if (evictedRelease < 0) completeNotes.MarkUnsentAttackEvicted(evictedAttack);
                        RecordDrop(result, bucketIndex, evictedRelease >= 0 ? 2 : 1);
                    }
                    bool ownAttackEvicted = evicted && noteKind == CompleteNoteEventKind.NoteOff &&
                        noteMatch.AttackIndex == evictedAttack;
                    bool protectedTraffic = noteKind == CompleteNoteEventKind.NoteOff ||
                        CompleteNoteTracker.IsSafetyControl(incomingEvent);
                    if (ownAttackEvicted || (!evicted && !protectedTraffic))
                    {
                        if (noteKind == CompleteNoteEventKind.NoteOn)
                            completeNotes.RecordNoteOn(incomingEvent, i, false, true);
                        RecordDrop(result, bucketIndex, 1);
                    }
                    else
                    {
                        pending.Enqueue(incomingEntry);
                        if (noteKind == CompleteNoteEventKind.NoteOn)
                            completeNotes.RecordNoteOn(incomingEvent, i, true, false);
                    }
                }
                else if (configuration.OverflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                {
                    int dropped = occupancy + 1;
                    pending.Clear();
                    serviceClock = beforeCurrentService;
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
            if (busy)
                while (pending.Count > 0)
                    completion = checked(completion + serviceClock.NextMicroseconds(events[pending.Dequeue()]));
            result.PredictedOutputCompletionMicroseconds = busy ? completion : lastCompleted;
            Report(progress, "Projecting finite queue pressure", events.Count, events.Count, 750, 250);
        }

        private static void AnalyzeVirtualForwardPressure(MidiSong song, WorkloadAnalysis result,
            AnalysisConfiguration configuration, CancellationToken cancellationToken,
            Action<WorkloadAnalysisProgress> progress)
        {
            if (configuration.OverflowPolicy != OverflowPolicy.DropNewest &&
                configuration.OverflowPolicy != OverflowPolicy.DropIncomingCompleteNotes)
                throw new InvalidOperationException("Drop oldest and Clear buffer require Simulate slowdown; a forward-only model cannot retract MIDI that was already sent.");

            MidiEventReader events = song.GetEventReader();
            ForwardDropQueue queue = new ForwardDropQueue(Math.Max(1, configuration.QueueLengthLimit));
            ServiceDurationClock serviceClock = CreateServiceClock(configuration);
            CompleteNoteTracker completeNotes = new CompleteNoteTracker();
            long lastAccepted = 0;
            for (int i = 0; i < events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting forward-only queue pressure", i, events.Count, 750, 250);
                }
                MidiEventView incomingEvent = events[i];
                long arrival = incomingEvent.IntendedMicroseconds;
                queue.Advance(arrival);
                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1,
                    Math.Max(0, arrival / result.BucketMicroseconds));
                CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(incomingEvent);
                if (noteKind == CompleteNoteEventKind.NoteOff && completeNotes.ShouldSuppressNoteOff(incomingEvent))
                {
                    RecordDrop(result, bucketIndex, 1);
                    continue;
                }
                bool safetyAdmission = configuration.OverflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                    noteKind != CompleteNoteEventKind.NoteOn;
                ServiceDurationClock admittedClock = serviceClock;
                long service = admittedClock.NextMicroseconds(incomingEvent);
                bool accepted = queue.TryAdmit(arrival, service, safetyAdmission);
                if (!accepted)
                {
                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, false,
                            configuration.OverflowPolicy == OverflowPolicy.DropIncomingCompleteNotes);
                    RecordDrop(result, bucketIndex, 1);
                    continue;
                }
                serviceClock = admittedClock;
                if (noteKind == CompleteNoteEventKind.NoteOn)
                    completeNotes.RecordNoteOn(incomingEvent, true, false);
                lastAccepted = arrival;
                int occupancy = queue.Occupancy;
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
            result.PredictedOutputCompletionMicroseconds = lastAccepted;
            Report(progress, "Projecting forward-only queue pressure", events.Count, events.Count, 750, 250);
        }

        private static void AnalyzeUnlimitedPressure(MidiSong song, WorkloadAnalysis result, AnalysisConfiguration configuration,
            CancellationToken cancellationToken, Action<WorkloadAnalysisProgress> progress)
        {
            MidiEventReader events = song.GetEventReader();
            bool busy = false;
            long completion = 0;
            long lastCompleted = 0;
            ServiceDurationClock serviceClock = CreateServiceClock(configuration);
            int nextPending = 0;
            for (int i = 0; i < events.Count; i++)
            {
                if ((i & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Projecting unlimited queue pressure", i, events.Count, 750, 250);
                }
                MidiEventView incomingEvent = events[i];
                long arrival = incomingEvent.IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    lastCompleted = completion;
                    if (nextPending < i)
                    {
                        long nextService = serviceClock.NextMicroseconds(events[nextPending]);
                        completion = checked(completion + nextService);
                        nextPending++;
                    }
                    else busy = false;
                }
                if (!busy)
                {
                    busy = true;
                    completion = checked(arrival + serviceClock.NextMicroseconds(incomingEvent));
                    nextPending = i + 1;
                }
                int occupancy = 1 + Math.Max(0, i + 1 - nextPending);
                int bucketIndex = (int)Math.Min(result.Buckets.Length - 1, Math.Max(0, arrival / result.BucketMicroseconds));
                if (occupancy > result.PredictedMaximumOccupancy) result.PredictedMaximumOccupancy = occupancy;
                if (occupancy > result.Buckets[bucketIndex].PredictedPeakOccupancy)
                    result.Buckets[bucketIndex].PredictedPeakOccupancy = occupancy;
            }
            if (busy)
                while (nextPending < events.Count)
                    completion = checked(completion + serviceClock.NextMicroseconds(events[nextPending++]));
            result.PredictedOutputCompletionMicroseconds = busy ? completion : lastCompleted;
            Report(progress, "Projecting unlimited queue pressure", events.Count, events.Count, 750, 250);
        }

        private static void RecordDrop(WorkloadAnalysis result, int bucketIndex, int count)
        {
            result.PredictedDroppedEvents += count;
            result.Buckets[bucketIndex].PredictedDroppedEvents += count;
        }

        private static long EventServiceMicroseconds(MidiEventView midiEvent, AnalysisConfiguration configuration)
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
