using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class MidiLoadProgress
    {
        public readonly string Stage;
        public readonly int CompletedTracks;
        public readonly int TotalTracks;
        public readonly long OverallCompleted;
        public readonly long OverallTotal;
        public readonly long StageCompleted;
        public readonly long StageTotal;

        public MidiLoadProgress(string stage, int completedTracks, int totalTracks)
            : this(stage, completedTracks, totalTracks, completedTracks, Math.Max(1, totalTracks), completedTracks, Math.Max(1, totalTracks))
        {
        }

        public MidiLoadProgress(string stage, int completedTracks, int totalTracks,
            long overallCompleted, long overallTotal, long stageCompleted, long stageTotal)
        {
            Stage = stage;
            CompletedTracks = completedTracks;
            TotalTracks = totalTracks;
            OverallCompleted = Math.Max(0, Math.Min(Math.Max(1, overallTotal), overallCompleted));
            OverallTotal = Math.Max(1, overallTotal);
            StageCompleted = Math.Max(0, Math.Min(Math.Max(1, stageTotal), stageCompleted));
            StageTotal = Math.Max(1, stageTotal);
        }

        public int OverallPermille { get { return (int)Math.Min(1000, OverallCompleted * 1000L / OverallTotal); } }
        public int StagePermille { get { return (int)Math.Min(1000, StageCompleted * 1000L / StageTotal); } }
    }

    internal static class MidiFileParser
    {
        private const long ProgressScale = 10000;
        private const long LegacyMaximumSingleArrayBytes = 0x7FEFFFFF;
        private const long VeryLargeReferenceArrayElementLimit = 0x7FEFFFFF;
        private const long ArraySafetyOverheadBytes = 64;
        private sealed class TempoChange
        {
            public long Tick;
            public int MicrosecondsPerQuarter;
            public int Track;
            public int Order;
        }

        private sealed class ParsedTrack
        {
            public readonly List<MidiEvent> Events = new List<MidiEvent>();
            public readonly List<TempoChange> Tempos = new List<TempoChange>();
            public long EndTick;
        }

        public static MidiSong Load(string path)
        {
            return Load(path, CancellationToken.None, null);
        }

        public static MidiSong Load(string path, CancellationToken cancellationToken, Action<MidiLoadProgress> progress)
        {
            if (String.IsNullOrEmpty(path))
                throw new ArgumentException("A MIDI file path is required.", "path");

            using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (BinaryReader reader = new BinaryReader(stream))
            {
                RequireChunk(reader, "MThd");
                uint headerLength = ReadUInt32BigEndian(reader);
                if (headerLength < 6)
                    throw new InvalidDataException("The MIDI header is shorter than 6 bytes.");

                int format = ReadUInt16BigEndian(reader);
                int trackCount = ReadUInt16BigEndian(reader);
                int division = ReadUInt16BigEndian(reader);
                if (format < 0 || format > 2)
                    throw new InvalidDataException("Unsupported Standard MIDI File format " + format + ".");
                if (format == 2)
                    throw new InvalidDataException("Format 2 MIDI files contain independent sequences and are not supported in this version.");
                if ((division & 0x8000) != 0)
                    throw new InvalidDataException("SMPTE time division is not supported; this version requires PPQN timing.");
                if (division == 0)
                    throw new InvalidDataException("The MIDI file declares zero ticks per quarter note.");
                if (trackCount <= 0)
                    throw new InvalidDataException("The MIDI file contains no tracks.");

                if (headerLength > 6)
                    SkipExactly(reader, headerLength - 6);

                ParsedTrack[] parsedTracks = new ParsedTrack[trackCount];
                List<TempoChange> allTempos = new List<TempoChange>();
                long endTick = 0;
                long parsedTrackBytes = 0;
                long parsedEventTotal = 0;
                long fileLength = Math.Max(1, stream.Length);

                for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    RequireChunk(reader, "MTrk");
                    uint trackLength = ReadUInt32BigEndian(reader);
                    if (trackLength > Int32.MaxValue)
                        throw new InvalidDataException("A MIDI track is too large to load.");
                    int currentTrackLength = (int)trackLength;
                    byte[] bytes;
                    ParsedTrack track;
                    try
                    {
                        bytes = ReadTrackBytes(stream, currentTrackLength, cancellationToken, delegate(int read)
                        {
                            ReportTrackProgress(progress, "Reading track " + (trackIndex + 1) + " of " + trackCount,
                                trackIndex, trackCount, parsedTrackBytes * 2 + read, fileLength * 2, read, currentTrackLength);
                        });

                        track = ParseTrack(bytes, trackIndex, cancellationToken, delegate(int position)
                        {
                            ReportTrackProgress(progress, "Parsing track " + (trackIndex + 1) + " of " + trackCount,
                                trackIndex, trackCount, parsedTrackBytes * 2 + currentTrackLength + position,
                                fileLength * 2, position, currentTrackLength);
                        });
                    }
                    catch (OutOfMemoryException ex)
                    {
                        throw RunningAllocationException(parsedEventTotal, trackIndex + 1, trackCount, ex);
                    }
                    parsedTracks[trackIndex] = track;
                    parsedTrackBytes += currentTrackLength;
                    parsedEventTotal += track.Events.Count;
                    ValidateObservedContiguousEventCount(parsedEventTotal, trackIndex + 1, trackCount);
                    allTempos.AddRange(track.Tempos);
                    if (track.EndTick > endTick)
                        endTick = track.EndTick;
                }

                cancellationToken.ThrowIfCancellationRequested();
                Report(progress, "Merging tracks", trackCount, trackCount, 5000, ProgressScale, 0, Math.Max(1, parsedEventTotal));
                List<MidiEvent> allEvents = MergeTracks(parsedTracks, cancellationToken, delegate(long completed, long total)
                {
                    Report(progress, "Merging tracks", trackCount, trackCount,
                        5000 + ScaleProgress(completed, total, 2500), ProgressScale, completed, total);
                });
                allTempos.Sort(CompareTempos);
                Report(progress, "Assigning playback timestamps", trackCount, trackCount, 7500, ProgressScale, 0, Math.Max(1, allEvents.Count));
                AssignRealTimes(allEvents, allTempos, division, cancellationToken, delegate(long completed, long total)
                {
                    Report(progress, "Assigning playback timestamps", trackCount, trackCount,
                        7500 + ScaleProgress(completed, total, 1500), ProgressScale, completed, total);
                });
                string fullPath = Path.GetFullPath(path);
                long noteCount = 0;
                Report(progress, "Finalizing events", trackCount, trackCount, 9000, ProgressScale, 0, Math.Max(1, allEvents.Count));
                for (int eventIndex = 0; eventIndex < allEvents.Count; eventIndex++)
                {
                    if ((eventIndex & 16383) == 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        Report(progress, "Finalizing events", trackCount, trackCount,
                            9000 + ScaleProgress(eventIndex, allEvents.Count, 1000), ProgressScale, eventIndex, allEvents.Count);
                    }
                    allEvents[eventIndex].EventIndex = eventIndex;
                    MidiEvent midiEvent = allEvents[eventIndex];
                    if (midiEvent.Kind == MidiEventKind.NoteOn && midiEvent.DataLength >= 3 && midiEvent.GetDataByte(2) != 0)
                        noteCount++;
                }

                long duration = TickToMicroseconds(endTick, allTempos, division);
                if (allEvents.Count > 0 && allEvents[allEvents.Count - 1].IntendedMicroseconds > duration)
                    duration = allEvents[allEvents.Count - 1].IntendedMicroseconds;

                MidiSong song = new MidiSong();
                song.FilePath = fullPath;
                song.FileSizeBytes = stream.Length;
                song.Format = format;
                song.TrackCount = trackCount;
                song.TicksPerQuarterNote = division;
                song.NoteCount = noteCount;
                song.Events = allEvents;
                song.DurationMicroseconds = duration;
                Report(progress, "Ready", trackCount, trackCount, ProgressScale, ProgressScale, 1, 1);
                return song;
            }
        }

        private static ParsedTrack ParseTrack(byte[] bytes, int trackIndex, CancellationToken cancellationToken, Action<int> progress)
        {
            ParsedTrack result = new ParsedTrack();
            int position = 0;
            long tick = 0;
            byte runningStatus = 0;
            int order = 0;

            while (position < bytes.Length)
            {
                if ((order & 4095) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (progress != null) progress(position);
                }
                uint delta = ReadVariableLength(bytes, ref position);
                tick = checked(tick + delta);
                if (position >= bytes.Length)
                    throw new InvalidDataException("Track " + trackIndex + " ends after a delta time.");

                byte first = bytes[position++];
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
                    if (status < 0xF0)
                        runningStatus = status;
                }

                if (status == 0xFF)
                {
                    runningStatus = 0;
                    byte metaType = ReadByte(bytes, ref position);
                    uint length = ReadVariableLength(bytes, ref position);
                    EnsureAvailable(bytes, position, length);
                    if (metaType == 0x51 && length == 3)
                    {
                        int tempo = (bytes[position] << 16) | (bytes[position + 1] << 8) | bytes[position + 2];
                        if (tempo > 0)
                        {
                            TempoChange change = new TempoChange();
                            change.Tick = tick;
                            change.MicrosecondsPerQuarter = tempo;
                            change.Track = trackIndex;
                            change.Order = order;
                            result.Tempos.Add(change);
                        }
                    }
                    position += (int)length;
                }
                else if (status == 0xF0 || status == 0xF7)
                {
                    runningStatus = 0;
                    uint length = ReadVariableLength(bytes, ref position);
                    EnsureAvailable(bytes, position, length);
                    int prefix = status == 0xF0 ? 1 : 0;
                    byte[] data = new byte[(int)length + prefix];
                    if (prefix == 1)
                        data[0] = 0xF0;
                    Buffer.BlockCopy(bytes, position, data, prefix, (int)length);
                    position += (int)length;
                    result.Events.Add(CreateEvent(tick, trackIndex, order, MidiEventKind.SystemExclusive, -1, status, data));
                }
                else if (status < 0xF0)
                {
                    int dataLength = ChannelDataLength(status);
                    byte firstValue = 0;
                    byte secondValue = 0;
                    int dataOffset = 0;
                    if (firstData >= 0)
                    {
                        firstValue = (byte)firstData;
                        dataOffset++;
                    }
                    while (dataOffset < dataLength)
                    {
                        byte value = ReadByte(bytes, ref position);
                        if (value >= 0x80)
                            throw new InvalidDataException("Unexpected status byte inside a channel message in track " + trackIndex + ".");
                        if (dataOffset == 0) firstValue = value;
                        else secondValue = value;
                        dataOffset++;
                    }
                    MidiEventKind kind = KindFromStatus(status);
                    MidiEventData data = MidiEventData.FromShort(status, firstValue, secondValue, dataLength + 1);
                    result.Events.Add(CreateEvent(tick, trackIndex, order, kind, status & 0x0F, status, data));
                }
                else
                {
                    runningStatus = 0;
                    int dataLength = SystemDataLength(status);
                    byte firstValue = dataLength > 0 ? ReadByte(bytes, ref position) : (byte)0;
                    byte secondValue = dataLength > 1 ? ReadByte(bytes, ref position) : (byte)0;
                    MidiEventData data = MidiEventData.FromShort(status, firstValue, secondValue, dataLength + 1);
                    result.Events.Add(CreateEvent(tick, trackIndex, order, MidiEventKind.SystemMessage, -1, status, data));
                }

                order++;
            }

            result.EndTick = tick;
            if (progress != null) progress(bytes.Length);
            return result;
        }

        private static MidiEvent CreateEvent(long tick, int track, int order, MidiEventKind kind, int channel, byte status, MidiEventData data)
        {
            MidiEvent midiEvent = new MidiEvent();
            midiEvent.AbsoluteTick = tick;
            midiEvent.Track = track;
            midiEvent.Order = order;
            midiEvent.Kind = kind;
            midiEvent.Channel = channel;
            midiEvent.Status = status;
            midiEvent.Data = data;
            return midiEvent;
        }

        private static void AssignRealTimes(List<MidiEvent> events, List<TempoChange> tempos, int ppqn,
            CancellationToken cancellationToken, Action<long, long> progress)
        {
            int tempoIndex = 0;
            long previousTick = 0;
            decimal currentMicroseconds = 0m;
            int currentTempo = 500000;

            for (int eventIndex = 0; eventIndex < events.Count; eventIndex++)
            {
                if ((eventIndex & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (progress != null) progress(eventIndex, events.Count);
                }
                MidiEvent midiEvent = events[eventIndex];
                while (tempoIndex < tempos.Count && tempos[tempoIndex].Tick <= midiEvent.AbsoluteTick)
                {
                    TempoChange change = tempos[tempoIndex];
                    currentMicroseconds += ((decimal)(change.Tick - previousTick) * currentTempo) / ppqn;
                    previousTick = change.Tick;
                    currentTempo = change.MicrosecondsPerQuarter;
                    tempoIndex++;
                }
                decimal eventTime = currentMicroseconds + ((decimal)(midiEvent.AbsoluteTick - previousTick) * currentTempo) / ppqn;
                midiEvent.IntendedMicroseconds = Decimal.ToInt64(Decimal.Round(eventTime, 0, MidpointRounding.AwayFromZero));
            }
            if (progress != null) progress(events.Count, events.Count);
        }

        private static byte[] ReadTrackBytes(Stream stream, int length, CancellationToken cancellationToken, Action<int> progress)
        {
            byte[] bytes = new byte[length];
            int offset = 0;
            while (offset < length)
            {
                cancellationToken.ThrowIfCancellationRequested();
                int read = stream.Read(bytes, offset, Math.Min(65536, length - offset));
                if (read <= 0) throw new EndOfStreamException("The MIDI track ended unexpectedly.");
                offset += read;
                if (progress != null) progress(offset);
            }
            return bytes;
        }

        private sealed class MergeNode
        {
            public int Track;
            public int Index;
        }

        private static List<MidiEvent> MergeTracks(ParsedTrack[] tracks, CancellationToken cancellationToken, Action<long, long> progress)
        {
            long total = 0;
            for (int i = 0; i < tracks.Length; i++) total += tracks[i].Events.Count;
            ValidateContiguousEventCount(total);
            List<MidiEvent> merged;
            try { merged = new List<MidiEvent>((int)total); }
            catch (Exception ex)
            {
                if (!(ex is OutOfMemoryException) && !(ex is ArgumentOutOfRangeException)) throw;
                throw CompletedAllocationException(total, ex);
            }
            List<MergeNode> heap = new List<MergeNode>(tracks.Length);
            for (int track = 0; track < tracks.Length; track++)
            {
                if (tracks[track].Events.Count == 0) continue;
                PushNode(heap, new MergeNode { Track = track, Index = 0 }, tracks);
            }
            while (heap.Count > 0)
            {
                if ((merged.Count & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (progress != null) progress(merged.Count, total);
                }
                MergeNode node = PopNode(heap, tracks);
                merged.Add(tracks[node.Track].Events[node.Index]);
                node.Index++;
                if (node.Index < tracks[node.Track].Events.Count) PushNode(heap, node, tracks);
                else
                {
                    tracks[node.Track].Events.Clear();
                    tracks[node.Track].Events.TrimExcess();
                }
            }
            if (progress != null) progress(total, total);
            return merged;
        }

        private static void PushNode(List<MergeNode> heap, MergeNode node, ParsedTrack[] tracks)
        {
            int index = heap.Count;
            heap.Add(node);
            while (index > 0)
            {
                int parent = (index - 1) / 2;
                if (CompareMergeNodes(heap[parent], node, tracks) <= 0) break;
                heap[index] = heap[parent];
                index = parent;
            }
            heap[index] = node;
        }

        private static MergeNode PopNode(List<MergeNode> heap, ParsedTrack[] tracks)
        {
            MergeNode first = heap[0];
            MergeNode last = heap[heap.Count - 1];
            heap.RemoveAt(heap.Count - 1);
            if (heap.Count == 0) return first;
            int index = 0;
            while (true)
            {
                int child = index * 2 + 1;
                if (child >= heap.Count) break;
                if (child + 1 < heap.Count && CompareMergeNodes(heap[child + 1], heap[child], tracks) < 0) child++;
                if (CompareMergeNodes(last, heap[child], tracks) <= 0) break;
                heap[index] = heap[child];
                index = child;
            }
            heap[index] = last;
            return first;
        }

        private static int CompareMergeNodes(MergeNode left, MergeNode right, ParsedTrack[] tracks)
        {
            return CompareEvents(tracks[left.Track].Events[left.Index], tracks[right.Track].Events[right.Index]);
        }

        private static void ReportTrackProgress(Action<MidiLoadProgress> progress, string stage, int completedTracks,
            int totalTracks, long completedWork, long totalWork, long stageCompleted, long stageTotal)
        {
            long overall = ScaleProgress(completedWork, totalWork, 5000);
            Report(progress, stage, completedTracks, totalTracks, overall, ProgressScale, stageCompleted, stageTotal);
        }

        private static long ScaleProgress(long completed, long total, long scale)
        {
            if (completed <= 0 || total <= 0) return 0;
            if (completed >= total) return scale;
            return (long)(((decimal)completed * scale) / total);
        }

        private static void Report(Action<MidiLoadProgress> progress, string stage, int completedTracks, int totalTracks,
            long overallCompleted, long overallTotal, long stageCompleted, long stageTotal)
        {
            if (progress != null)
                progress(new MidiLoadProgress(stage, completedTracks, totalTracks,
                    overallCompleted, overallTotal, stageCompleted, stageTotal));
        }

        internal static long MaximumContiguousEventCount
        {
            get { return CalculateMaximumContiguousEventCount(IntPtr.Size, VeryLargeArraysConfigured()); }
        }

        internal static long CalculateMaximumContiguousEventCount(int pointerSize, bool veryLargeArraysEnabled)
        {
            if (pointerSize != 4 && pointerSize != 8) throw new ArgumentOutOfRangeException("pointerSize");
            if (pointerSize == 8 && veryLargeArraysEnabled)
                return Math.Min(Int32.MaxValue, VeryLargeReferenceArrayElementLimit);
            return Math.Min(Int32.MaxValue, (LegacyMaximumSingleArrayBytes - ArraySafetyOverheadBytes) / pointerSize);
        }

        internal static bool VeryLargeArraysConfigured()
        {
            if (IntPtr.Size != 8) return false;
            try
            {
                string configuration = AppDomain.CurrentDomain.SetupInformation.ConfigurationFile;
                if (String.IsNullOrEmpty(configuration) || !File.Exists(configuration)) return false;
                string text = File.ReadAllText(configuration);
                int setting = text.IndexOf("gcAllowVeryLargeObjects", StringComparison.OrdinalIgnoreCase);
                if (setting < 0) return false;
                int close = text.IndexOf('>', setting);
                if (close < 0) close = text.Length;
                string element = text.Substring(setting, close - setting);
                return element.IndexOf("enabled=\"true\"", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    element.IndexOf("enabled='true'", StringComparison.OrdinalIgnoreCase) >= 0;
            }
            catch { return false; }
        }

        internal static void ValidateContiguousEventCount(long eventCount)
        {
            if (eventCount < 0 || eventCount > MaximumContiguousEventCount)
                throw ContiguousEventStorageException(eventCount, true, null);
        }

        internal static void ValidateObservedContiguousEventCount(long eventCount, int completedTracks, int totalTracks)
        {
            if (eventCount < 0 || eventCount > MaximumContiguousEventCount)
                throw ContiguousEventStorageException(eventCount, false, null, completedTracks, totalTracks);
        }

        private static InvalidDataException ContiguousEventStorageException(long eventCount, bool complete, Exception inner,
            int completedTracks = 0, int totalTracks = 0)
        {
            string count = complete
                ? "The complete MIDI contains " + eventCount.ToString("N0") + " dispatchable events"
                : "After " + completedTracks + " of " + totalTracks + " tracks, at least " + eventCount.ToString("N0") +
                    " dispatchable events have been observed; the complete file total is not yet known";
            string message = count + ", exceeding this process's structural contiguous event-storage limit of " +
                MaximumContiguousEventCount.ToString("N0") + " events. " +
                (IntPtr.Size == 8 && VeryLargeArraysConfigured()
                    ? "The x64 very-large-array limit is already enabled; larger files require a future segmented-storage design."
                    : "This process uses the legacy array byte-size limit; use the configured x64 release for a higher structural ceiling.");
            return inner == null ? new InvalidDataException(message) : new InvalidDataException(message, inner);
        }

        private static InvalidDataException RunningAllocationException(long priorEventCount, int currentTrack, int totalTracks,
            Exception inner)
        {
            return new InvalidDataException("Memory allocation failed while parsing track " + currentTrack + " of " + totalTracks +
                ". At least " + priorEventCount.ToString("N0") + " dispatchable events were completed in earlier tracks, but " +
                "the complete file total is not yet known. This is an allocation failure, not proof that the structural event-count limit was exceeded.", inner);
        }

        private static InvalidDataException CompletedAllocationException(long eventCount, Exception inner)
        {
            return new InvalidDataException("The complete MIDI contains " + eventCount.ToString("N0") +
                " dispatchable events and is within the structural limit of " + MaximumContiguousEventCount.ToString("N0") +
                ", but the final contiguous event-reference array could not be allocated. Available contiguous address space or committed memory was insufficient.", inner);
        }

        private static long TickToMicroseconds(long targetTick, List<TempoChange> tempos, int ppqn)
        {
            long previousTick = 0;
            decimal microseconds = 0m;
            int tempo = 500000;
            for (int i = 0; i < tempos.Count && tempos[i].Tick <= targetTick; i++)
            {
                TempoChange change = tempos[i];
                microseconds += ((decimal)(change.Tick - previousTick) * tempo) / ppqn;
                previousTick = change.Tick;
                tempo = change.MicrosecondsPerQuarter;
            }
            microseconds += ((decimal)(targetTick - previousTick) * tempo) / ppqn;
            return Decimal.ToInt64(Decimal.Round(microseconds, 0, MidpointRounding.AwayFromZero));
        }

        private static int CompareEvents(MidiEvent left, MidiEvent right)
        {
            int compare = left.AbsoluteTick.CompareTo(right.AbsoluteTick);
            if (compare != 0) return compare;
            compare = left.Track.CompareTo(right.Track);
            if (compare != 0) return compare;
            return left.Order.CompareTo(right.Order);
        }

        private static int CompareTempos(TempoChange left, TempoChange right)
        {
            int compare = left.Tick.CompareTo(right.Tick);
            if (compare != 0) return compare;
            compare = left.Track.CompareTo(right.Track);
            if (compare != 0) return compare;
            return left.Order.CompareTo(right.Order);
        }

        private static MidiEventKind KindFromStatus(byte status)
        {
            switch (status & 0xF0)
            {
                case 0x80: return MidiEventKind.NoteOff;
                case 0x90: return MidiEventKind.NoteOn;
                case 0xA0: return MidiEventKind.PolyphonicAftertouch;
                case 0xB0: return MidiEventKind.ControlChange;
                case 0xC0: return MidiEventKind.ProgramChange;
                case 0xD0: return MidiEventKind.ChannelAftertouch;
                case 0xE0: return MidiEventKind.PitchBend;
                default: throw new InvalidDataException("Unknown MIDI channel status 0x" + status.ToString("X2") + ".");
            }
        }

        private static int ChannelDataLength(byte status)
        {
            int high = status & 0xF0;
            return high == 0xC0 || high == 0xD0 ? 1 : 2;
        }

        private static int SystemDataLength(byte status)
        {
            switch (status)
            {
                case 0xF1: return 1;
                case 0xF2: return 2;
                case 0xF3: return 1;
                case 0xF6:
                case 0xF8:
                case 0xF9:
                case 0xFA:
                case 0xFB:
                case 0xFC:
                case 0xFD:
                case 0xFE: return 0;
                default: throw new InvalidDataException("Unsupported system status 0x" + status.ToString("X2") + ".");
            }
        }

        private static uint ReadVariableLength(byte[] bytes, ref int position)
        {
            uint value = 0;
            for (int count = 0; count < 4; count++)
            {
                byte current = ReadByte(bytes, ref position);
                value = (value << 7) | (uint)(current & 0x7F);
                if ((current & 0x80) == 0)
                    return value;
            }
            throw new InvalidDataException("A MIDI variable-length quantity exceeds four bytes.");
        }

        private static byte ReadByte(byte[] bytes, ref int position)
        {
            if (position >= bytes.Length)
                throw new EndOfStreamException("The MIDI track ended unexpectedly.");
            return bytes[position++];
        }

        private static void EnsureAvailable(byte[] bytes, int position, uint length)
        {
            if (length > Int32.MaxValue || position < 0 || position + (long)length > bytes.Length)
                throw new EndOfStreamException("The MIDI event data ended unexpectedly.");
        }

        private static void RequireChunk(BinaryReader reader, string expected)
        {
            byte[] id = reader.ReadBytes(4);
            if (id.Length != 4 || id[0] != expected[0] || id[1] != expected[1] || id[2] != expected[2] || id[3] != expected[3])
                throw new InvalidDataException("Expected MIDI chunk " + expected + ".");
        }

        private static int ReadUInt16BigEndian(BinaryReader reader)
        {
            int high = reader.ReadByte();
            int low = reader.ReadByte();
            return (high << 8) | low;
        }

        private static uint ReadUInt32BigEndian(BinaryReader reader)
        {
            uint a = reader.ReadByte();
            uint b = reader.ReadByte();
            uint c = reader.ReadByte();
            uint d = reader.ReadByte();
            return (a << 24) | (b << 16) | (c << 8) | d;
        }

        private static void SkipExactly(BinaryReader reader, uint count)
        {
            if (count > Int32.MaxValue || reader.ReadBytes((int)count).Length != (int)count)
                throw new EndOfStreamException("The MIDI header ended unexpectedly.");
        }
    }
}
