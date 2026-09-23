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
        internal long[] EventsByTrack;
        internal long[] PayloadBytesByTrack;
        internal long[] TempoChangesByTrack;
        internal long[] SourceEntriesBySlot;
    }

    internal sealed class MidiMemoryProjection
    {
        internal long ProjectedRetainedBytes;
        internal long ProjectedPeakLowBytes;
        internal long ProjectedPeakHighBytes;
        internal long ProvisionalBytes;
        internal long MergeWorkspaceBytes;
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
                projection.ProjectedRetainedBytes >= projection.WarningThresholdBytes;
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
            using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (BinaryReader reader = new BinaryReader(stream))
            {
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
                counts.FileSizeBytes = stream.Length;
                counts.Format = format;
                counts.TrackCount = trackCount;
                counts.TicksPerQuarterNote = division;
                counts.EventsByTrack = new long[trackCount];
                counts.PayloadBytesByTrack = new long[trackCount];
                counts.TempoChangesByTrack = new long[trackCount];
                counts.SourceEntriesBySlot = new long[16 * SourceAttributeCount];
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
                        trackStart, fileLength, length, trackCount);
                }
                cancellationToken.ThrowIfCancellationRequested();
                Report(progress, fileLength, fileLength, 1, 1, trackCount, trackCount);
                return counts;
            }
        }

        private static void ScanTrack(DirectCompactMidiParser.TrackReader reader, int trackIndex,
            MidiPreflightCounts counts, CancellationToken cancellationToken, Action<MidiLoadProgress> progress,
            long trackStart, long fileLength, uint declaredLength, int trackCount)
        {
            byte runningStatus = 0;
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
                if (first < 0x80)
                {
                    if (runningStatus < 0x80 || runningStatus >= 0xF0)
                        throw new InvalidDataException("Invalid running status in track " + trackIndex + ".");
                    status = runningStatus;
                    firstData = first;
                }
                else
                {
                    status = first;
                    if (status < 0xF0) runningStatus = status;
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
                }
                else if (status == 0xF0 || status == 0xF7)
                {
                    runningStatus = 0;
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
                        counts.NoteOnCount = CheckedIncrement(counts.NoteOnCount, "NoteOn");
                    AddSourceValueEntries(counts, status & 0x0F, high, firstValue);
                }
                else
                {
                    runningStatus = 0;
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

            long workspace = Multiply(counts.TrackCount, pointerSize == 8 ? 112L : 72L);
            workspace = Add(workspace, ReferenceArrayBytes(counts.TrackCount, pointerSize));
            workspace = Add(workspace, 2L * 1024L * 1024L);
            long lower = Add(Math.Max(provisional, retained), workspace);
            long transientSegments = CompactPayloadStore.SegmentSize +
                CompactMidiEventStore.RecordSegmentCapacity * 40L + TempoSegmentCapacity * 24L;
            long upper = Add(Add(provisional, retained), Add(workspace, transientSegments));
            long warning = WarningThreshold(pointerSize);
            return new MidiMemoryProjection
            {
                ProjectedRetainedBytes = retained,
                ProjectedPeakLowBytes = lower,
                ProjectedPeakHighBytes = upper,
                ProvisionalBytes = provisional,
                MergeWorkspaceBytes = workspace,
                PointerSize = pointerSize,
                WarningThresholdBytes = warning,
                IsArchitectureRisk = pointerSize == 4 && upper >= warning
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
