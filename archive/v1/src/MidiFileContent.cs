using System;
using System.IO;

namespace MidiBottleneck
{
    // Opens either a bare Standard MIDI File or the bounded SMF payload in a
    // RIFF/RMID container. The payload remains a view over the source file;
    // no container-sized copy is created.
    internal sealed class MidiFileContent : IDisposable
    {
        private sealed class BoundedReadStream : Stream
        {
            private readonly Stream _source;
            private readonly long _start;
            private readonly long _length;
            private long _position;

            internal BoundedReadStream(Stream source, long start, long length)
            {
                _source = source;
                _start = start;
                _length = length;
                _source.Position = start;
            }

            public override bool CanRead { get { return true; } }
            public override bool CanSeek { get { return true; } }
            public override bool CanWrite { get { return false; } }
            public override long Length { get { return _length; } }
            public override long Position
            {
                get { return _position; }
                set { Seek(value, SeekOrigin.Begin); }
            }
            public override void Flush() { }
            public override int Read(byte[] buffer, int offset, int count)
            {
                if (buffer == null) throw new ArgumentNullException("buffer");
                if (offset < 0 || count < 0 || offset > buffer.Length - count)
                    throw new ArgumentOutOfRangeException();
                long remaining = _length - _position;
                if (remaining <= 0) return 0;
                int requested = (int)Math.Min((long)count, remaining);
                _source.Position = _start + _position;
                int read = _source.Read(buffer, offset, requested);
                _position += read;
                return read;
            }
            public override int ReadByte()
            {
                if (_position >= _length) return -1;
                _source.Position = _start + _position;
                int value = _source.ReadByte();
                if (value >= 0) _position++;
                return value;
            }
            public override long Seek(long offset, SeekOrigin origin)
            {
                long target;
                if (origin == SeekOrigin.Begin) target = offset;
                else if (origin == SeekOrigin.Current) target = checked(_position + offset);
                else if (origin == SeekOrigin.End) target = checked(_length + offset);
                else throw new ArgumentOutOfRangeException("origin");
                if (target < 0 || target > _length) throw new IOException("Attempted to seek outside the bounded MIDI payload.");
                _position = target;
                return target;
            }
            public override void SetLength(long value) { throw new NotSupportedException(); }
            public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        }

        private readonly FileStream _file;
        internal readonly Stream Stream;
        internal readonly long FileSizeBytes;
        internal readonly bool IsRmid;

        private MidiFileContent(FileStream file, long payloadStart, long payloadLength, bool isRmid)
        {
            _file = file;
            FileSizeBytes = file.Length;
            IsRmid = isRmid;
            Stream = new BoundedReadStream(file, payloadStart, payloadLength);
        }

        internal static MidiFileContent Open(string path)
        {
            if (String.IsNullOrEmpty(path)) throw new ArgumentException("A MIDI file path is required.", "path");
            FileStream file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            try
            {
                if (file.Length < 4) return new MidiFileContent(file, 0, file.Length, false);
                byte[] id = ReadExactly(file, 4, "The MIDI file header ended unexpectedly.");
                file.Position = 0;
                if (!Matches(id, "RIFF")) return new MidiFileContent(file, 0, file.Length, false);

                if (file.Length < 12) throw new InvalidDataException("The RIFF/RMID header is truncated.");
                file.Position = 4;
                uint riffSize = ReadUInt32LittleEndian(file);
                byte[] type = ReadExactly(file, 4, "The RIFF/RMID type is truncated.");
                if (!Matches(type, "RMID")) throw new InvalidDataException("The RIFF file is not an RMID MIDI container.");
                if (riffSize < 4) throw new InvalidDataException("The RIFF/RMID size is shorter than its form type.");
                long riffEnd = checked(8L + riffSize);
                if (riffEnd > file.Length) throw new InvalidDataException("The RIFF/RMID container extends beyond the source file.");

                long position = 12;
                while (position < riffEnd)
                {
                    if (riffEnd - position < 8) throw new InvalidDataException("The RIFF/RMID chunk header is truncated.");
                    file.Position = position;
                    byte[] chunkId = ReadExactly(file, 4, "The RIFF/RMID chunk header is truncated.");
                    uint chunkLength = ReadUInt32LittleEndian(file);
                    long dataStart = file.Position;
                    long dataEnd = checked(dataStart + chunkLength);
                    long paddedEnd = checked(dataEnd + (chunkLength & 1U));
                    if (paddedEnd > riffEnd) throw new InvalidDataException("A RIFF/RMID chunk extends beyond the declared container.");
                    if (Matches(chunkId, "data"))
                    {
                        if (chunkLength == 0) throw new InvalidDataException("The RIFF/RMID data chunk is empty.");
                        return new MidiFileContent(file, dataStart, chunkLength, true);
                    }
                    position = paddedEnd;
                }
                throw new InvalidDataException("The RIFF/RMID container has no MIDI data chunk.");
            }
            catch
            {
                file.Dispose();
                throw;
            }
        }

        public void Dispose()
        {
            Stream.Dispose();
            _file.Dispose();
        }

        private static byte[] ReadExactly(Stream stream, int count, string error)
        {
            byte[] bytes = new byte[count];
            int offset = 0;
            while (offset < count)
            {
                int read = stream.Read(bytes, offset, count - offset);
                if (read <= 0) throw new InvalidDataException(error);
                offset += read;
            }
            return bytes;
        }

        private static uint ReadUInt32LittleEndian(Stream stream)
        {
            byte[] bytes = ReadExactly(stream, 4, "The RIFF/RMID chunk length is truncated.");
            return (uint)(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24);
        }

        private static bool Matches(byte[] bytes, string value)
        {
            return bytes.Length == 4 && bytes[0] == value[0] && bytes[1] == value[1] &&
                bytes[2] == value[2] && bytes[3] == value[3];
        }
    }

    internal static class MidiRunningStatusCompatibility
    {
        internal static bool CanRepairAfterMeta(byte lastChannelStatus, bool immediatelyAfterMeta, byte firstData)
        {
            return immediatelyAfterMeta && firstData < 0x80 &&
                lastChannelStatus >= 0x80 && lastChannelStatus < 0xF0;
        }
    }
}
