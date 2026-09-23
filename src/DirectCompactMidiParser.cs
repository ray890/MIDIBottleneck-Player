using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace MidiBottleneck
{
    // Production SMF parser. Track data, provisional events, tempo changes,
    // final events, long payloads, and source-value indexes are all bounded or
    // segmented; no complete legacy MidiEvent graph is created.
    internal static class DirectCompactMidiParser
    {
        private const long ProgressScale = 10000;
        private const int TempoSegmentCapacity = 4096;

        private sealed class ParsedTrack
        {
            internal CompactMidiEventStore Events;
            internal TempoStore Tempos;
            internal long EndTick;
        }

        private struct TempoRecord
        {
            internal long Tick;
            internal int MicrosecondsPerQuarter;
            internal int Track;
            internal int Order;
        }

        private sealed class TempoStore
        {
            private readonly TempoRecord[][] _segments;
            internal readonly int Count;
            internal TempoStore(TempoRecord[][] segments, int count) { _segments = segments; Count = count; }
            internal TempoRecord this[int index] { get { return _segments[index / TempoSegmentCapacity][index % TempoSegmentCapacity]; } }
            internal void ReleaseConsumedThrough(int index)
            {
                TempoRecord[] segment = _segments[index / TempoSegmentCapacity];
                if ((index % TempoSegmentCapacity) == segment.Length - 1)
                    _segments[index / TempoSegmentCapacity] = null;
            }
        }

        private sealed class TempoBuilder
        {
            private readonly List<TempoRecord[]> _segments = new List<TempoRecord[]>();
            private TempoRecord[] _current;
            private int _offset;
            private int _count;
            internal void Add(long tick, int tempo, int track, int order)
            {
                if (_current == null || _offset == _current.Length)
                {
                    _current = new TempoRecord[TempoSegmentCapacity];
                    _segments.Add(_current);
                    _offset = 0;
                }
                _current[_offset++] = new TempoRecord { Tick = tick, MicrosecondsPerQuarter = tempo, Track = track, Order = order };
                _count++;
            }
            internal TempoStore Complete()
            {
                if (_current != null && _offset != _current.Length)
                {
                    TempoRecord[] trimmed = new TempoRecord[_offset];
                    Array.Copy(_current, trimmed, _offset);
                    _segments[_segments.Count - 1] = trimmed;
                }
                return new TempoStore(_segments.ToArray(), _count);
            }
        }

        private sealed class EventNode { internal int Track; internal int Index; }
        private sealed class TempoNode { internal int Track; internal int Index; }

        private sealed class TempoClock
        {
            private readonly ParsedTrack[] _tracks;
            private readonly List<TempoNode> _heap;
            private readonly int _ppqn;
            private long _previousTick;
            private decimal _microseconds;
            private int _tempo = 500000;

            internal TempoClock(ParsedTrack[] tracks, int ppqn)
            {
                _tracks = tracks;
                _ppqn = ppqn;
                _heap = new List<TempoNode>(tracks.Length);
                for (int track = 0; track < tracks.Length; track++)
                    if (tracks[track].Tempos.Count > 0) Push(new TempoNode { Track = track, Index = 0 });
            }

            internal long TimeAt(long tick)
            {
                while (_heap.Count > 0)
                {
                    TempoNode first = _heap[0];
                    TempoRecord change = _tracks[first.Track].Tempos[first.Index];
                    if (change.Tick > tick) break;
                    first = Pop();
                    change = _tracks[first.Track].Tempos[first.Index];
                    _microseconds += ((decimal)(change.Tick - _previousTick) * _tempo) / _ppqn;
                    _previousTick = change.Tick;
                    _tempo = change.MicrosecondsPerQuarter;
                    _tracks[first.Track].Tempos.ReleaseConsumedThrough(first.Index);
                    first.Index++;
                    if (first.Index < _tracks[first.Track].Tempos.Count) Push(first);
                }
                decimal value = _microseconds + ((decimal)(tick - _previousTick) * _tempo) / _ppqn;
                return Decimal.ToInt64(Decimal.Round(value, 0, MidpointRounding.AwayFromZero));
            }

            private int Compare(TempoNode left, TempoNode right)
            {
                TempoRecord a = _tracks[left.Track].Tempos[left.Index];
                TempoRecord b = _tracks[right.Track].Tempos[right.Index];
                int compare = a.Tick.CompareTo(b.Tick);
                if (compare != 0) return compare;
                compare = a.Track.CompareTo(b.Track);
                return compare != 0 ? compare : a.Order.CompareTo(b.Order);
            }
            private void Push(TempoNode node)
            {
                int index = _heap.Count;
                _heap.Add(node);
                while (index > 0)
                {
                    int parent = (index - 1) / 2;
                    if (Compare(_heap[parent], node) <= 0) break;
                    _heap[index] = _heap[parent];
                    index = parent;
                }
                _heap[index] = node;
            }
            private TempoNode Pop()
            {
                TempoNode first = _heap[0];
                TempoNode last = _heap[_heap.Count - 1];
                _heap.RemoveAt(_heap.Count - 1);
                if (_heap.Count == 0) return first;
                int index = 0;
                while (true)
                {
                    int child = index * 2 + 1;
                    if (child >= _heap.Count) break;
                    if (child + 1 < _heap.Count && Compare(_heap[child + 1], _heap[child]) < 0) child++;
                    if (Compare(last, _heap[child]) <= 0) break;
                    _heap[index] = _heap[child];
                    index = child;
                }
                _heap[index] = last;
                return first;
            }
        }

        internal sealed class TrackReader
        {
            private readonly Stream _stream;
            private readonly CancellationToken _cancellationToken;
            private readonly Action<long> _progress;
            private readonly byte[] _buffer;
            private long _streamRemaining;
            private int _position;
            private int _count;
            private long _consumed;

            internal TrackReader(Stream stream, uint length, CancellationToken cancellationToken, Action<long> progress,
                byte[] buffer)
            {
                if (buffer == null || buffer.Length == 0) throw new ArgumentException("A track-reader buffer is required.", "buffer");
                _stream = stream; _streamRemaining = length; _cancellationToken = cancellationToken; _progress = progress; _buffer = buffer;
            }
            internal bool AtEnd { get { return _position == _count && _streamRemaining == 0; } }
            internal long Consumed { get { return _consumed; } }

            internal byte ReadByte()
            {
                if (_position == _count) Fill();
                byte value = _buffer[_position++];
                _consumed++;
                return value;
            }

            internal uint ReadVariableLength()
            {
                uint value = 0;
                for (int count = 0; count < 4; count++)
                {
                    byte current = ReadByte();
                    value = (value << 7) | (uint)(current & 0x7F);
                    if ((current & 0x80) == 0) return value;
                }
                throw new InvalidDataException("A MIDI variable-length quantity exceeds four bytes.");
            }

            internal void Skip(uint length)
            {
                long remaining = length;
                while (remaining > 0)
                {
                    if (_position == _count) Fill();
                    int take = (int)Math.Min(remaining, _count - _position);
                    _position += take;
                    _consumed += take;
                    remaining -= take;
                }
            }

            internal void CopyPayload(CompactMidiEventStore.Builder builder, uint length)
            {
                long remaining = length;
                while (remaining > 0)
                {
                    if (_position == _count) Fill();
                    int take = (int)Math.Min(remaining, _count - _position);
                    int end = _position + take;
                    while (_position < end) builder.AppendPayloadByte(_buffer[_position++]);
                    _consumed += take;
                    remaining -= take;
                }
            }

            private void Fill()
            {
                _cancellationToken.ThrowIfCancellationRequested();
                if (_streamRemaining <= 0) throw new EndOfStreamException("The MIDI track ended unexpectedly.");
                int requested = (int)Math.Min(_buffer.Length, _streamRemaining);
                _count = _stream.Read(_buffer, 0, requested);
                _position = 0;
                if (_count <= 0) throw new EndOfStreamException("The MIDI track ended unexpectedly.");
                _streamRemaining -= _count;
                if (_progress != null) _progress(_consumed);
            }
        }

        internal static MidiSong Load(string path, CancellationToken cancellationToken, Action<MidiLoadProgress> progress)
        {
            if (String.IsNullOrEmpty(path)) throw new ArgumentException("A MIDI file path is required.", "path");
            using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (BinaryReader reader = new BinaryReader(stream))
            {
                RequireChunk(reader, "MThd");
                uint headerLength = ReadUInt32BigEndian(reader);
                if (headerLength < 6) throw new InvalidDataException("The MIDI header is shorter than 6 bytes.");
                int format = ReadUInt16BigEndian(reader);
                int trackCount = ReadUInt16BigEndian(reader);
                int division = ReadUInt16BigEndian(reader);
                ValidateHeader(format, trackCount, division);
                if (headerLength > 6) SkipStream(stream, headerLength - 6, cancellationToken);

                ParsedTrack[] tracks = new ParsedTrack[trackCount];
                long totalTrackBytes = 0;
                long totalEvents = 0;
                long endTick = 0;
                long fileLength = Math.Max(1, stream.Length);
                byte[] trackBuffer = new byte[65536];
                for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    RequireChunk(reader, "MTrk");
                    uint length = ReadUInt32BigEndian(reader);
                    long priorBytes = totalTrackBytes;
                    TrackReader trackReader = new TrackReader(stream, length, cancellationToken, delegate(long consumed)
                    {
                        Report(progress, "Parsing track " + (trackIndex + 1) + " of " + trackCount,
                            trackIndex, trackCount, Scale(priorBytes + consumed, fileLength, 6000), consumed, Math.Max(1L, length));
                    }, trackBuffer);
                    ParsedTrack track;
                    try { track = ParseTrack(trackReader, trackIndex, cancellationToken, progress, trackCount, priorBytes, fileLength, length); }
                    catch (OutOfMemoryException ex)
                    { throw new InvalidDataException("Memory allocation failed while parsing compact track " + (trackIndex + 1) + " of " + trackCount + ".", ex); }
                    tracks[trackIndex] = track;
                    totalTrackBytes += length;
                    totalEvents += track.Events.Count;
                    if (totalEvents > Int32.MaxValue)
                        throw new InvalidDataException("The MIDI exceeds the supported 2,147,483,647-event indexed-store limit.");
                    if (track.EndTick > endTick) endTick = track.EndTick;
                }

                cancellationToken.ThrowIfCancellationRequested();
                Report(progress, "Preparing tempo map", trackCount, trackCount, 6000, 0, Math.Max(1, trackCount));
                TempoClock clock = new TempoClock(tracks, division);
                Report(progress, "Merging tracks and assigning timestamps", trackCount, trackCount, 6500, 0, Math.Max(1, totalEvents));
                CompactMidiEventStore.Builder finalBuilder = new CompactMidiEventStore.Builder();
                ChannelSourceValueIndex.Builder sourceIndex = ChannelSourceValueIndex.CreateBuilder();
                MidiStateChaseIndex.Builder chaseIndex = MidiStateChaseIndex.CreateBuilder();
                List<EventNode> heap = new List<EventNode>(trackCount);
                for (int track = 0; track < tracks.Length; track++)
                    if (tracks[track].Events.Count > 0) PushEvent(heap, new EventNode { Track = track, Index = 0 }, tracks);
                long noteCount = 0;
                long merged = 0;
                long lastEventTime = 0;
                while (heap.Count > 0)
                {
                    if ((merged & 16383) == 0)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        Report(progress, "Merging tracks and assigning timestamps", trackCount, trackCount,
                            6500 + Scale(merged, totalEvents, 3000), merged, Math.Max(1, totalEvents));
                    }
                    EventNode node = PopEvent(heap, tracks);
                    CompactMidiEventStore provisional = tracks[node.Track].Events;
                    MidiEventView value = provisional.GetEvent(node.Index);
                    long intended = clock.TimeAt(value.AbsoluteTick);
                    int eventIndex = finalBuilder.Count;
                    sourceIndex.Add(value, eventIndex);
                    chaseIndex.Add(value, eventIndex);
                    if (value.Kind == MidiEventKind.NoteOn && value.DataLength >= 3 && value.GetDataByte(2) != 0) noteCount++;
                    finalBuilder.Add(value, intended);
                    lastEventTime = intended;
                    provisional.ReleaseConsumedThrough(node.Index);
                    node.Index++;
                    merged++;
                    if (node.Index < provisional.Count) PushEvent(heap, node, tracks);
                    else tracks[node.Track].Events = null;
                }

                long duration = clock.TimeAt(endTick);
                if (lastEventTime > duration) duration = lastEventTime;
                cancellationToken.ThrowIfCancellationRequested();
                Report(progress, "Finalizing indexes", trackCount, trackCount, 9500, merged, Math.Max(1, merged));
                CompactMidiEventStore finalStore = finalBuilder.Complete();
                ChannelSourceValueIndex completedIndex = sourceIndex.Complete(finalStore, cancellationToken);
                MidiStateChaseIndex completedChase = chaseIndex.Complete(cancellationToken);
                MidiSong song = new MidiSong();
                song.FilePath = Path.GetFullPath(path);
                song.FileSizeBytes = stream.Length;
                song.Format = format;
                song.TrackCount = trackCount;
                song.TicksPerQuarterNote = division;
                song.NoteCount = noteCount;
                song.DurationMicroseconds = duration;
                song.SetEventStore(finalStore);
                song.SetChannelSourceValueIndex(completedIndex);
                song.SetMidiStateChaseIndex(completedChase);
                Report(progress, "Ready", trackCount, trackCount, ProgressScale, 1, 1);
                return song;
            }
        }

        private static ParsedTrack ParseTrack(TrackReader reader, int trackIndex, CancellationToken cancellationToken,
            Action<MidiLoadProgress> progress, int trackCount, long priorBytes, long fileLength, uint declaredLength)
        {
            CompactMidiEventStore.Builder events = new CompactMidiEventStore.Builder();
            TempoBuilder tempos = new TempoBuilder();
            long tick = 0;
            byte runningStatus = 0;
            int order = 0;
            while (!reader.AtEnd)
            {
                if ((order & 4095) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    Report(progress, "Parsing track " + (trackIndex + 1) + " of " + trackCount,
                        trackIndex, trackCount, Scale(priorBytes + reader.Consumed, fileLength, 6000),
                        reader.Consumed, Math.Max(1L, declaredLength));
                }
                tick = checked(tick + reader.ReadVariableLength());
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
                        if (tempo > 0) tempos.Add(tick, tempo, trackIndex, order);
                    }
                    else reader.Skip(length);
                }
                else if (status == 0xF0 || status == 0xF7)
                {
                    runningStatus = 0;
                    uint length = reader.ReadVariableLength();
                    int prefix = status == 0xF0 ? 1 : 0;
                    int payloadLength = checked((int)length + prefix);
                    long payloadOffset = events.PayloadLength;
                    if (prefix != 0) events.AppendPayloadByte(0xF0);
                    reader.CopyPayload(events, length);
                    events.AddRaw(tick, order, trackIndex, MidiEventKind.SystemExclusive, -1, status,
                        payloadLength, 0, payloadOffset);
                }
                else if (status < 0xF0)
                {
                    int dataLength = ChannelDataLength(status);
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
                    uint packed = (uint)(status | (firstValue << 8) | (secondValue << 16));
                    events.AddRaw(tick, order, trackIndex, KindFromStatus(status), status & 0x0F, status,
                        dataLength + 1, packed, -1);
                }
                else
                {
                    runningStatus = 0;
                    int dataLength = SystemDataLength(status);
                    byte firstValue = dataLength > 0 ? reader.ReadByte() : (byte)0;
                    byte secondValue = dataLength > 1 ? reader.ReadByte() : (byte)0;
                    uint packed = (uint)(status | (firstValue << 8) | (secondValue << 16));
                    events.AddRaw(tick, order, trackIndex, MidiEventKind.SystemMessage, -1, status,
                        dataLength + 1, packed, -1);
                }
                order++;
            }
            Report(progress, "Parsing track " + (trackIndex + 1) + " of " + trackCount,
                trackIndex + 1, trackCount, Scale(priorBytes + declaredLength, fileLength, 6000), declaredLength, Math.Max(1L, declaredLength));
            return new ParsedTrack { Events = events.Complete(), Tempos = tempos.Complete(), EndTick = tick };
        }

        private static void PushEvent(List<EventNode> heap, EventNode node, ParsedTrack[] tracks)
        {
            int index = heap.Count;
            heap.Add(node);
            while (index > 0)
            {
                int parent = (index - 1) / 2;
                if (CompareEvents(heap[parent], node, tracks) <= 0) break;
                heap[index] = heap[parent];
                index = parent;
            }
            heap[index] = node;
        }

        private static EventNode PopEvent(List<EventNode> heap, ParsedTrack[] tracks)
        {
            EventNode first = heap[0];
            EventNode last = heap[heap.Count - 1];
            heap.RemoveAt(heap.Count - 1);
            if (heap.Count == 0) return first;
            int index = 0;
            while (true)
            {
                int child = index * 2 + 1;
                if (child >= heap.Count) break;
                if (child + 1 < heap.Count && CompareEvents(heap[child + 1], heap[child], tracks) < 0) child++;
                if (CompareEvents(last, heap[child], tracks) <= 0) break;
                heap[index] = heap[child];
                index = child;
            }
            heap[index] = last;
            return first;
        }

        private static int CompareEvents(EventNode left, EventNode right, ParsedTrack[] tracks)
        {
            MidiEventView a = tracks[left.Track].Events.GetEvent(left.Index);
            MidiEventView b = tracks[right.Track].Events.GetEvent(right.Index);
            int compare = a.AbsoluteTick.CompareTo(b.AbsoluteTick);
            if (compare != 0) return compare;
            compare = a.Track.CompareTo(b.Track);
            return compare != 0 ? compare : a.IntendedMicroseconds.CompareTo(b.IntendedMicroseconds);
        }

        internal static void ValidateHeader(int format, int trackCount, int division)
        {
            if (format < 0 || format > 2) throw new InvalidDataException("Unsupported Standard MIDI File format " + format + ".");
            if (format == 2) throw new InvalidDataException("Format 2 MIDI files contain independent sequences and are not supported in this version.");
            if ((division & 0x8000) != 0) throw new InvalidDataException("SMPTE time division is not supported; this version requires PPQN timing.");
            if (division == 0) throw new InvalidDataException("The MIDI file declares zero ticks per quarter note.");
            if (trackCount <= 0) throw new InvalidDataException("The MIDI file contains no tracks.");
        }

        internal static MidiEventKind KindFromStatus(byte status)
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
        internal static int ChannelDataLength(byte status) { int high = status & 0xF0; return high == 0xC0 || high == 0xD0 ? 1 : 2; }
        internal static int SystemDataLength(byte status)
        {
            switch (status)
            {
                case 0xF1: return 1; case 0xF2: return 2; case 0xF3: return 1;
                case 0xF6: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: return 0;
                default: throw new InvalidDataException("Unsupported system status 0x" + status.ToString("X2") + ".");
            }
        }
        internal static void RequireChunk(BinaryReader reader, string expected)
        {
            byte[] id = reader.ReadBytes(4);
            if (id.Length != 4 || id[0] != expected[0] || id[1] != expected[1] || id[2] != expected[2] || id[3] != expected[3])
                throw new InvalidDataException("Expected MIDI chunk " + expected + ".");
        }
        internal static int ReadUInt16BigEndian(BinaryReader reader) { return (reader.ReadByte() << 8) | reader.ReadByte(); }
        internal static uint ReadUInt32BigEndian(BinaryReader reader)
        {
            uint a = reader.ReadByte(), b = reader.ReadByte(), c = reader.ReadByte(), d = reader.ReadByte();
            return (a << 24) | (b << 16) | (c << 8) | d;
        }
        internal static void SkipStream(Stream stream, uint length, CancellationToken cancellationToken)
        {
            byte[] buffer = new byte[4096];
            long remaining = length;
            while (remaining > 0)
            {
                cancellationToken.ThrowIfCancellationRequested();
                int read = stream.Read(buffer, 0, (int)Math.Min(buffer.Length, remaining));
                if (read <= 0) throw new EndOfStreamException("The MIDI header ended unexpectedly.");
                remaining -= read;
            }
        }
        private static long Scale(long completed, long total, long scale)
        {
            if (completed <= 0 || total <= 0) return 0;
            if (completed >= total) return scale;
            return (long)(((decimal)completed * scale) / total);
        }
        private static void Report(Action<MidiLoadProgress> progress, string stage, int completedTracks, int totalTracks,
            long overall, long stageCompleted, long stageTotal)
        {
            if (progress != null) progress(new MidiLoadProgress(stage, completedTracks, totalTracks,
                overall, ProgressScale, stageCompleted, stageTotal));
        }
    }
}
