using System;
using System.IO;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class MidiPreflightCounts
    {
        internal long FileSizeBytes;
        internal int Format;
        internal int TrackCount;
        internal int TicksPerQuarterNote;
        internal long DispatchableEventCount;
        internal long NoteOnCount;
        internal long FinalLongPayloadBytes;
        internal long ProvisionalPayloadBytes;
        internal long TempoChangeCount;
        internal long SourceValueEntryCount;
        internal long ChaseStateEntryCount;
        internal long ChaseParameterValueEntryCount;
        internal long RunningStatusRepairCount;
        internal long RoutingLaneCount;
        internal long RoutingPatchStateEventCount;
        internal long RoutingPatchTimelineEntryUpperBound;
        internal long RoutingGeneratedStateEventUpperBound;
        internal long[] EventsByTrack;
        internal long[] PayloadBytesByTrack;
        internal long[] TempoChangesByTrack;
        internal long[] SourceEntriesBySlot;
        internal long[] RoutingNoteLanesByChannel;
        internal long[] RoutingPatchStateEventsByChannel;
    }

    internal sealed class MidiMemoryProjection
    {
        internal long ProjectedRetainedBytes;
        internal long ProjectedPeakLowBytes;
        internal long ProjectedPeakHighBytes;
        internal long ProvisionalBytes;
        internal long MergeWorkspaceBytes;
        internal long RetainedNameAllowanceBytes;
        internal long RoutingRetainedAdditionalBytes;
        internal long RoutingPeakAdditionalBytes;
        internal long ProjectedRetainedWithRoutingBytes;
        internal long ProjectedRoutingPeakBytes;
        internal int PointerSize;
        internal bool IsArchitectureRisk;
        internal long WarningThresholdBytes;
    }

    internal sealed class MidiLargeFileInspection
    {
        internal readonly string FilePath;
        internal readonly MidiPreflightCounts Counts;
        internal readonly MidiMemoryProjection Projection;

        internal MidiLargeFileInspection(string filePath, MidiPreflightCounts counts, MidiMemoryProjection projection)
        {
            FilePath = filePath;
            Counts = counts;
            Projection = projection;
        }
    }

    internal static class MidiLargeFilePreflight
    {
        internal const long X86WarningThresholdBytes = 1024L * 1024L * 1024L;
        internal const long X64WarningThresholdBytes = 4L * 1024L * 1024L * 1024L;
        internal const long FixedExpansionAllowanceBytes = 64L * 1024L * 1024L;
        internal const int ConservativePeakBytesPerFileByte = 64;
        private const int SourceAttributeCount = 9;
        private const int TempoSegmentCapacity = 4096;

        internal static long PreflightFileSizeThreshold(int pointerSize)
        {
            long warning = WarningThreshold(pointerSize);
            return Math.Max(1024L * 1024L,
                (warning - FixedExpansionAllowanceBytes) / ConservativePeakBytesPerFileByte);
        }

        internal static bool RequiresPreflight(long fileSizeBytes, int pointerSize)
        {
            return fileSizeBytes >= PreflightFileSizeThreshold(pointerSize);
        }

        internal static bool RequiresWarning(MidiMemoryProjection projection)
        {
            if (projection == null) throw new ArgumentNullException("projection");
            return projection.ProjectedPeakHighBytes >= projection.WarningThresholdBytes ||
                projection.ProjectedRetainedBytes >= projection.WarningThresholdBytes ||
                projection.ProjectedRetainedWithRoutingBytes >= projection.WarningThresholdBytes ||
                projection.ProjectedRoutingPeakBytes >= projection.WarningThresholdBytes;
        }

        internal static MidiLargeFileInspection Inspect(string path, int pointerSize,
            CancellationToken cancellationToken, Action<MidiLoadProgress> progress)
        {
            MidiPreflightCounts counts = Scan(path, cancellationToken, progress);
            MidiMemoryProjection projection = Estimate(counts, pointerSize);
            return new MidiLargeFileInspection(Path.GetFullPath(path), counts, projection);
        }

        internal static MidiPreflightCounts Scan(string path, CancellationToken cancellationToken,
            Action<MidiLoadProgress> progress)
        {
            if (String.IsNullOrEmpty(path)) throw new ArgumentException("A MIDI file path is required.", "path");
            using (MidiFileContent content = MidiFileContent.Open(path))
            using (BinaryReader reader = new BinaryReader(content.Stream))
            {
                Stream stream = content.Stream;
                long fileLength = Math.Max(1, stream.Length);
                Report(progress, 0, fileLength, 0, fileLength, 0, 0);
                DirectCompactMidiParser.RequireChunk(reader, "MThd");
                uint headerLength = DirectCompactMidiParser.ReadUInt32BigEndian(reader);
                if (headerLength < 6) throw new InvalidDataException("The MIDI header is shorter than 6 bytes.");
                int format = DirectCompactMidiParser.ReadUInt16BigEndian(reader);
                int trackCount = DirectCompactMidiParser.ReadUInt16BigEndian(reader);
                int division = DirectCompactMidiParser.ReadUInt16BigEndian(reader);
                DirectCompactMidiParser.ValidateHeader(format, trackCount, division);
                if (headerLength > 6) DirectCompactMidiParser.SkipStream(stream, headerLength - 6, cancellationToken);

                MidiPreflightCounts counts = new MidiPreflightCounts();
                counts.FileSizeBytes = content.FileSizeBytes;
                counts.Format = format;
                counts.TrackCount = trackCount;
                counts.TicksPerQuarterNote = division;
                counts.EventsByTrack = new long[trackCount];
                counts.PayloadBytesByTrack = new long[trackCount];
                counts.TempoChangesByTrack = new long[trackCount];
                counts.SourceEntriesBySlot = new long[16 * SourceAttributeCount];
                counts.RoutingNoteLanesByChannel = new long[16];
                counts.RoutingPatchStateEventsByChannel = new long[16];
                bool[] routingLaneSeen = new bool[trackCount * 16];
                byte[] trackBuffer = new byte[65536];

                for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    DirectCompactMidiParser.RequireChunk(reader, "MTrk");
                    uint length = DirectCompactMidiParser.ReadUInt32BigEndian(reader);
                    long trackStart = stream.Position;
                    DirectCompactMidiParser.TrackReader trackReader = new DirectCompactMidiParser.TrackReader(
                        stream, length, cancellationToken, delegate(long consumed)
                        {
                            Report(progress, trackStart + consumed, fileLength, consumed, Math.Max(1L, length),
                                trackIndex, trackCount);
                        }, trackBuffer);
                    ScanTrack(trackReader, trackIndex, counts, cancellationToken, progress,
                        trackStart, fileLength, length, trackCount, routingLaneSeen);
                }
                for (int channel = 0; channel < 16; channel++)
                    counts.RoutingPatchTimelineEntryUpperBound = Add(
                        counts.RoutingPatchTimelineEntryUpperBound,
                        Multiply(counts.RoutingNoteLanesByChannel[channel],
                            counts.RoutingPatchStateEventsByChannel[channel]));
                counts.RoutingGeneratedStateEventUpperBound = Multiply(3L,
                    Add(counts.NoteOnCount, counts.RoutingPatchStateEventCount));
                cancellationToken.ThrowIfCancellationRequested();
                Report(progress, fileLength, fileLength, 1, 1, trackCount, trackCount);
                return counts;
            }
        }

        private static void ScanTrack(DirectCompactMidiParser.TrackReader reader, int trackIndex,
            MidiPreflightCounts counts, CancellationToken cancellationToken, Action<MidiLoadProgress> progress,
            long trackStart, long fileLength, uint declaredLength, int trackCount,
            bool[] routingLaneSeen)
        {
            byte runningStatus = 0;
            byte lastChannelStatus = 0;
            bool immediatelyAfterMeta = false;
            int order = 0;
            while (!reader.AtEnd)
            {
                if ((order & 4095) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, trackStart + reader.Consumed, fileLength, reader.Consumed,
                        Math.Max(1L, declaredLength), trackIndex, trackCount);
                }
                reader.ReadVariableLength();
                if (reader.AtEnd) throw new InvalidDataException("Track " + trackIndex + " ends after a delta time.");
                byte first = reader.ReadByte();
                byte status;
                int firstData = -1;
                bool repairedRunningStatus = false;
                if (first < 0x80)
                {
                    if (runningStatus < 0x80 || runningStatus >= 0xF0)
                    {
                        if (!MidiRunningStatusCompatibility.CanRepairAfterMeta(
                            lastChannelStatus, immediatelyAfterMeta, first))
                            throw new InvalidDataException("Invalid running status in track " + trackIndex + ".");
                        status = lastChannelStatus;
                        runningStatus = status;
                        repairedRunningStatus = true;
                    }
                    else status = runningStatus;
                    firstData = first;
                }
                else
                {
                    status = first;
                    if (status < 0xF0)
                    {
                        runningStatus = status;
                        lastChannelStatus = status;
                    }
                }

                if (status == 0xFF)
                {
                    runningStatus = 0;
                    byte metaType = reader.ReadByte();
                    uint length = reader.ReadVariableLength();
                    if (metaType == 0x51 && length == 3)
                    {
                        int tempo = (reader.ReadByte() << 16) | (reader.ReadByte() << 8) | reader.ReadByte();
                        if (tempo > 0)
                        {
                            counts.TempoChangeCount = CheckedIncrement(counts.TempoChangeCount, "tempo-change");
                            counts.TempoChangesByTrack[trackIndex] = CheckedIncrement(
                                counts.TempoChangesByTrack[trackIndex], "track tempo-change");
                        }
                    }
                    else reader.Skip(length);
                    immediatelyAfterMeta = true;
                }
                else if (status == 0xF0 || status == 0xF7)
                {
                    runningStatus = 0;
                    immediatelyAfterMeta = false;
                    uint length = reader.ReadVariableLength();
                    long payloadLength = (long)length + (status == 0xF0 ? 1L : 0L);
                    AddDispatchable(counts, trackIndex);
                    counts.ProvisionalPayloadBytes = CheckedAdd(counts.ProvisionalPayloadBytes, payloadLength, "SysEx payload");
                    counts.PayloadBytesByTrack[trackIndex] = CheckedAdd(
                        counts.PayloadBytesByTrack[trackIndex], payloadLength, "track SysEx payload");
                    if (payloadLength > 3)
                        counts.FinalLongPayloadBytes = CheckedAdd(counts.FinalLongPayloadBytes, payloadLength, "long payload");
                    reader.Skip(length);
                }
                else if (status < 0xF0)
                {
                    int dataLength = DirectCompactMidiParser.ChannelDataLength(status);
                    byte firstValue = 0;
                    byte secondValue = 0;
                    int dataOffset = 0;
                    if (firstData >= 0) { firstValue = (byte)firstData; dataOffset = 1; }
                    while (dataOffset < dataLength)
                    {
                        byte value = reader.ReadByte();
                        if (value >= 0x80)
                            throw new InvalidDataException("Unexpected status byte inside a channel message in track " + trackIndex + ".");
                        if (dataOffset == 0) firstValue = value; else secondValue = value;
                        dataOffset++;
                    }
                    AddDispatchable(counts, trackIndex);
                    int high = status & 0xF0;
                    if (high == 0x90 && secondValue != 0)
                    {
                        counts.NoteOnCount = CheckedIncrement(counts.NoteOnCount, "NoteOn");
                        int routingSlot = trackIndex * 16 + (status & 0x0F);
                        if (!routingLaneSeen[routingSlot])
                        {
                            routingLaneSeen[routingSlot] = true;
                            counts.RoutingLaneCount = CheckedIncrement(
                                counts.RoutingLaneCount, "track-routing lane");
                            int channel = status & 0x0F;
                            counts.RoutingNoteLanesByChannel[channel] = CheckedIncrement(
                                counts.RoutingNoteLanesByChannel[channel], "track-routing channel lane");
                        }
                    }
                    if (high == 0xC0 || high == 0xB0 &&
                        (firstValue == 0 || firstValue == 32))
                    {
                        int channel = status & 0x0F;
                        counts.RoutingPatchStateEventCount = CheckedIncrement(
                            counts.RoutingPatchStateEventCount, "track-routing patch-state event");
                        counts.RoutingPatchStateEventsByChannel[channel] = CheckedIncrement(
                            counts.RoutingPatchStateEventsByChannel[channel],
                            "track-routing channel patch-state event");
                    }
                    AddSourceValueEntries(counts, status & 0x0F, high, firstValue);
                    AddChaseEntries(counts, high, firstValue);
                    if (repairedRunningStatus)
                        counts.RunningStatusRepairCount = CheckedIncrement(
                            counts.RunningStatusRepairCount, "running-status repair");
                    immediatelyAfterMeta = false;
                }
                else
                {
                    runningStatus = 0;
                    immediatelyAfterMeta = false;
                    int dataLength = DirectCompactMidiParser.SystemDataLength(status);
                    if (dataLength > 0) reader.ReadByte();
                    if (dataLength > 1) reader.ReadByte();
                    AddDispatchable(counts, trackIndex);
                }
                order++;
            }
        }

        private static void AddDispatchable(MidiPreflightCounts counts, int trackIndex)
        {
            counts.DispatchableEventCount = CheckedIncrement(counts.DispatchableEventCount, "dispatchable-event");
            if (counts.DispatchableEventCount > Int32.MaxValue)
                throw new InvalidDataException("The MIDI exceeds the supported 2,147,483,647-event indexed-store limit.");
            counts.EventsByTrack[trackIndex] = CheckedIncrement(counts.EventsByTrack[trackIndex], "track event");
        }

        private static void AddSourceValueEntries(MidiPreflightCounts counts, int channel, int high, byte first)
        {
            if (high == 0xB0 && first == 121)
            {
                AddSource(counts, channel, ChannelAttribute.Expression);
                AddSource(counts, channel, ChannelAttribute.Pan);
                AddSource(counts, channel, ChannelAttribute.Sustain);
                AddSource(counts, channel, ChannelAttribute.PitchBend);
                AddSource(counts, channel, ChannelAttribute.Aftertouch);
                return;
            }
            ChannelAttribute attribute;
            if (high == 0xC0) attribute = ChannelAttribute.Program;
            else if (high == 0xD0) attribute = ChannelAttribute.Aftertouch;
            else if (high == 0xE0) attribute = ChannelAttribute.PitchBend;
            else if (high == 0xB0)
            {
                switch (first)
                {
                    case 0: attribute = ChannelAttribute.BankMsb; break;
                    case 32: attribute = ChannelAttribute.BankLsb; break;
                    case 7: attribute = ChannelAttribute.Volume; break;
                    case 11: attribute = ChannelAttribute.Expression; break;
                    case 10: attribute = ChannelAttribute.Pan; break;
                    case 64: attribute = ChannelAttribute.Sustain; break;
                    default: return;
                }
            }
            else return;
            AddSource(counts, channel, attribute);
        }

        private static void AddSource(MidiPreflightCounts counts, int channel, ChannelAttribute attribute)
        {
            int slot = channel * SourceAttributeCount + (int)attribute;
            counts.SourceEntriesBySlot[slot] = CheckedIncrement(counts.SourceEntriesBySlot[slot], "source-value entry");
            counts.SourceValueEntryCount = CheckedIncrement(counts.SourceValueEntryCount, "source-value entry");
        }

        private static void AddChaseEntries(MidiPreflightCounts counts, int high, byte first)
        {
            if (high == 0xC0 || high == 0xD0 || high == 0xE0 ||
                high == 0xB0 && (first < 120 || first == 121))
                counts.ChaseStateEntryCount = CheckedIncrement(counts.ChaseStateEntryCount,
                    "whole-state chase entry");
            if (high == 0xB0 && (first == 6 || first == 38 || first == 96 || first == 97))
                counts.ChaseParameterValueEntryCount = CheckedIncrement(
                    counts.ChaseParameterValueEntryCount, "parameter-state chase entry");
        }

        internal static MidiMemoryProjection Estimate(MidiPreflightCounts counts, int pointerSize)
        {
            if (counts == null) throw new ArgumentNullException("counts");
            if (pointerSize != 4 && pointerSize != 8) throw new ArgumentOutOfRangeException("pointerSize");
            long provisional = 0;
            for (int track = 0; track < counts.TrackCount; track++)
            {
                provisional = Add(provisional, SegmentedValueBytes(counts.EventsByTrack[track], 40,
                    CompactMidiEventStore.RecordSegmentCapacity, pointerSize));
                provisional = Add(provisional, SegmentedByteBytes(counts.PayloadBytesByTrack[track],
                    CompactPayloadStore.SegmentSize, pointerSize));
                provisional = Add(provisional, SegmentedValueBytes(counts.TempoChangesByTrack[track], 24,
                    TempoSegmentCapacity, pointerSize));
                provisional = Add(provisional, 3L * ObjectHeader(pointerSize));
            }
            provisional = Add(provisional, ReferenceArrayBytes(counts.TrackCount, pointerSize));

            long retained = SegmentedValueBytes(counts.DispatchableEventCount, 40,
                CompactMidiEventStore.RecordSegmentCapacity, pointerSize);
            retained = Add(retained, SegmentedByteBytes(counts.FinalLongPayloadBytes,
                CompactPayloadStore.SegmentSize, pointerSize));
            retained = Add(retained, ReferenceArrayBytes(16 * SourceAttributeCount, pointerSize));
            for (int slot = 0; slot < counts.SourceEntriesBySlot.Length; slot++)
            {
                long entries = counts.SourceEntriesBySlot[slot];
                if (entries == 0) continue;
                retained = Add(retained, ObjectHeader(pointerSize));
                retained = Add(retained, SegmentedValueBytes(entries, 8,
                    ChannelSourceValueIndex.SegmentCapacity, pointerSize));
            }
            retained = Add(retained, 3L * ObjectHeader(pointerSize));
            retained = Add(retained, ReferenceArrayBytes(16 * 123 + 32, pointerSize));
            retained = Add(retained, SegmentedValueBytes(counts.ChaseStateEntryCount, 8,
                MidiStateChaseIndex.SegmentCapacity, pointerSize));
            long chaseSeriesUpperBound = Math.Min(counts.ChaseStateEntryCount, 16L * (123 + 2));
            retained = Add(retained, Multiply(chaseSeriesUpperBound, 2L * ObjectHeader(pointerSize) + pointerSize));
            long parameterSeriesUpperBound = Math.Min(counts.ChaseParameterValueEntryCount, 16L * 32768L);
            retained = Add(retained, SegmentedValueBytes(counts.ChaseParameterValueEntryCount, 8,
                MidiStateChaseIndex.SegmentCapacity, pointerSize));
            retained = Add(retained, Multiply(parameterSeriesUpperBound,
                3L * ObjectHeader(pointerSize) + 2L * pointerSize));

            // Track and instrument names are capped at two 128-character values
            // per track.  Account for their strings and owning references even
            // though most files retain much less.
            long retainedNames = Multiply(counts.TrackCount,
                2L * (pointerSize + ObjectHeader(pointerSize) +
                    Align(ArrayHeader(pointerSize) + 128L * 2L,
                        pointerSize == 8 ? 8 : 4)));
            retained = Add(retained, retainedNames);

            long workspace = Multiply(counts.TrackCount, pointerSize == 8 ? 112L : 72L);
            workspace = Add(workspace, ReferenceArrayBytes(counts.TrackCount, pointerSize));
            workspace = Add(workspace, 2L * 1024L * 1024L);
            long lower = Add(Math.Max(provisional, retained), workspace);
            long transientSegments = CompactPayloadStore.SegmentSize +
                CompactMidiEventStore.RecordSegmentCapacity * 40L + TempoSegmentCapacity * 24L;
            long upper = Add(Add(provisional, retained), Add(workspace, transientSegments));

            long routedEventUpperBound = Add(counts.DispatchableEventCount,
                counts.RoutingGeneratedStateEventUpperBound);
            long routedEntrySize = pointerSize == 8 ? 16L : 8L;
            long routingRetained = Align(Add(ArrayHeader(pointerSize),
                counts.DispatchableEventCount), pointerSize == 8 ? 8 : 4);
            routingRetained = Add(routingRetained,
                Align(Add(ArrayHeader(pointerSize),
                    Multiply(routedEventUpperBound, routedEntrySize)),
                    pointerSize == 8 ? 8 : 4));
            routingRetained = Add(routingRetained,
                Multiply(counts.NoteOnCount, 40L));
            routingRetained = Add(routingRetained,
                Multiply(counts.RoutingPatchTimelineEntryUpperBound, 16L));
            routingRetained = Add(routingRetained,
                Multiply(counts.RoutingLaneCount, pointerSize == 8 ? 768L : 512L));
            routingRetained = Add(routingRetained,
                Multiply(counts.RoutingGeneratedStateEventUpperBound,
                    pointerSize == 8 ? 104L : 72L));
            long activeOccurrenceWorkspace = Multiply(
                Math.Min(counts.NoteOnCount, TrackRoutePlanner.MaximumActiveAttacks),
                pointerSize == 8 ? 64L : 44L);
            long routingWorkspace = Add(activeOccurrenceWorkspace,
                Align(Add(ArrayHeader(pointerSize),
                    Multiply(routedEventUpperBound, routedEntrySize)),
                    pointerSize == 8 ? 8 : 4));
            routingWorkspace = Add(routingWorkspace,
                Multiply(Add(counts.NoteOnCount,
                    counts.RoutingPatchTimelineEntryUpperBound), 16L));
            long routingPeak = Add(routingRetained, routingWorkspace);
            long warning = WarningThreshold(pointerSize);
            return new MidiMemoryProjection
            {
                ProjectedRetainedBytes = retained,
                ProjectedPeakLowBytes = lower,
                ProjectedPeakHighBytes = upper,
                ProvisionalBytes = provisional,
                MergeWorkspaceBytes = workspace,
                RetainedNameAllowanceBytes = retainedNames,
                RoutingRetainedAdditionalBytes = routingRetained,
                RoutingPeakAdditionalBytes = routingPeak,
                ProjectedRetainedWithRoutingBytes = Add(retained, routingRetained),
                ProjectedRoutingPeakBytes = Add(retained, routingPeak),
                PointerSize = pointerSize,
                WarningThresholdBytes = warning,
                IsArchitectureRisk = pointerSize == 4 &&
                    (upper >= warning || Add(retained, routingPeak) >= warning)
            };
        }

        private static long SegmentedValueBytes(long count, int elementSize, int capacity, int pointerSize)
        {
            if (count <= 0) return ReferenceArrayBytes(0, pointerSize);
            long segments = DivideRoundUp(count, capacity);
            long bytes = Multiply(count, elementSize);
            bytes = Add(bytes, Multiply(segments, ArrayHeader(pointerSize)));
            bytes = Add(bytes, ReferenceArrayBytes(segments, pointerSize));
            return bytes;
        }

        private static long SegmentedByteBytes(long count, int capacity, int pointerSize)
        { return SegmentedValueBytes(count, 1, capacity, pointerSize); }

        private static long ReferenceArrayBytes(long count, int pointerSize)
        { return Align(Add(ArrayHeader(pointerSize), Multiply(count, pointerSize)), pointerSize == 8 ? 8 : 4); }

        private static long ArrayHeader(int pointerSize) { return pointerSize == 8 ? 24 : 16; }
        private static long ObjectHeader(int pointerSize) { return pointerSize == 8 ? 24 : 12; }
        private static long WarningThreshold(int pointerSize) { return pointerSize == 8 ? X64WarningThresholdBytes : X86WarningThresholdBytes; }
        private static long DivideRoundUp(long value, long divisor) { return value == 0 ? 0 : 1 + (value - 1) / divisor; }
        private static long Align(long value, int alignment)
        {
            long remainder = value % alignment;
            return remainder == 0 ? value : Add(value, alignment - remainder);
        }
        private static long CheckedIncrement(long value, string name)
        {
            if (value == Int64.MaxValue) throw new InvalidDataException("The MIDI contains too many " + name + " records.");
            return value + 1;
        }
        private static long CheckedAdd(long value, long addition, string name)
        {
            if (addition < 0 || value > Int64.MaxValue - addition)
                throw new InvalidDataException("The MIDI contains too much " + name + " data.");
            return value + addition;
        }
        private static long Add(long left, long right)
        { return left >= Int64.MaxValue - right ? Int64.MaxValue : left + right; }
        private static long Multiply(long left, long right)
        { return left == 0 || right == 0 ? 0 : left > Int64.MaxValue / right ? Int64.MaxValue : left * right; }

        private static void Report(Action<MidiLoadProgress> progress, long overallCompleted, long overallTotal,
            long stageCompleted, long stageTotal, int completedTracks, int totalTracks)
        {
            if (progress != null) progress(new MidiLoadProgress("Inspecting large MIDI", completedTracks, totalTracks,
                overallCompleted, Math.Max(1L, overallTotal), stageCompleted, Math.Max(1L, stageTotal)));
        }
    }
}
