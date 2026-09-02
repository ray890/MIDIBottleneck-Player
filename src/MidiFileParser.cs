using System;
using System.Collections.Generic;
using System.IO;

namespace MidiBottleneck
{
    internal static class MidiFileParser
    {
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

                List<MidiEvent> allEvents = new List<MidiEvent>();
                List<TempoChange> allTempos = new List<TempoChange>();
                long endTick = 0;

                for (int trackIndex = 0; trackIndex < trackCount; trackIndex++)
                {
                    RequireChunk(reader, "MTrk");
                    uint trackLength = ReadUInt32BigEndian(reader);
                    if (trackLength > Int32.MaxValue)
                        throw new InvalidDataException("A MIDI track is too large to load.");
                    byte[] bytes = reader.ReadBytes((int)trackLength);
                    if (bytes.Length != (int)trackLength)
                        throw new EndOfStreamException("The MIDI track ended unexpectedly.");

                    ParsedTrack track = ParseTrack(bytes, trackIndex);
                    allEvents.AddRange(track.Events);
                    allTempos.AddRange(track.Tempos);
                    if (track.EndTick > endTick)
                        endTick = track.EndTick;
                }

                allEvents.Sort(CompareEvents);
                allTempos.Sort(CompareTempos);
                AssignRealTimes(allEvents, allTempos, division);
                string fullPath = Path.GetFullPath(path);
                for (int eventIndex = 0; eventIndex < allEvents.Count; eventIndex++)
                {
                    allEvents[eventIndex].EventIndex = eventIndex;
                    allEvents[eventIndex].SourceFile = fullPath;
                }

                long duration = TickToMicroseconds(endTick, allTempos, division);
                if (allEvents.Count > 0 && allEvents[allEvents.Count - 1].IntendedMicroseconds > duration)
                    duration = allEvents[allEvents.Count - 1].IntendedMicroseconds;

                MidiSong song = new MidiSong();
                song.FilePath = fullPath;
                song.Format = format;
                song.TrackCount = trackCount;
                song.TicksPerQuarterNote = division;
                song.Events = allEvents;
                song.DurationMicroseconds = duration;
                return song;
            }
        }

        private static ParsedTrack ParseTrack(byte[] bytes, int trackIndex)
        {
            ParsedTrack result = new ParsedTrack();
            int position = 0;
            long tick = 0;
            byte runningStatus = 0;
            int order = 0;

            while (position < bytes.Length)
            {
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
                    byte[] data = new byte[dataLength + 1];
                    data[0] = status;
                    int dataOffset = 1;
                    if (firstData >= 0)
                        data[dataOffset++] = (byte)firstData;
                    while (dataOffset < data.Length)
                    {
                        byte value = ReadByte(bytes, ref position);
                        if (value >= 0x80)
                            throw new InvalidDataException("Unexpected status byte inside a channel message in track " + trackIndex + ".");
                        data[dataOffset++] = value;
                    }
                    MidiEventKind kind = KindFromStatus(status);
                    result.Events.Add(CreateEvent(tick, trackIndex, order, kind, status & 0x0F, status, data));
                }
                else
                {
                    runningStatus = 0;
                    int dataLength = SystemDataLength(status);
                    byte[] data = new byte[dataLength + 1];
                    data[0] = status;
                    for (int i = 1; i < data.Length; i++)
                        data[i] = ReadByte(bytes, ref position);
                    result.Events.Add(CreateEvent(tick, trackIndex, order, MidiEventKind.SystemMessage, -1, status, data));
                }

                order++;
            }

            result.EndTick = tick;
            return result;
        }

        private static MidiEvent CreateEvent(long tick, int track, int order, MidiEventKind kind, int channel, byte status, byte[] data)
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

        private static void AssignRealTimes(List<MidiEvent> events, List<TempoChange> tempos, int ppqn)
        {
            int tempoIndex = 0;
            long previousTick = 0;
            decimal currentMicroseconds = 0m;
            int currentTempo = 500000;

            for (int eventIndex = 0; eventIndex < events.Count; eventIndex++)
            {
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
