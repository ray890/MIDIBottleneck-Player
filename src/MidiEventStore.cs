using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Threading;

namespace MidiBottleneck
{
    // Read-only boundary used by playback and analysis.  The list-backed
    // implementation remains the reference backend while compact backends are
    // introduced incrementally.
    internal interface IMidiEventStore
    {
        int Count { get; }
        MidiEventView GetEvent(int index);
        int LowerBoundByTime(long microseconds);
    }

    internal sealed class ListMidiEventStore : IMidiEventStore
    {
        private readonly IList<MidiEvent> _events;

        public ListMidiEventStore(IList<MidiEvent> events)
        {
            if (events == null) throw new ArgumentNullException("events");
            _events = events;
        }

        internal IList<MidiEvent> Source { get { return _events; } }
        public int Count { get { return _events.Count; } }
        public MidiEventView GetEvent(int index) { return MidiEventView.FromEvent(_events[index], index); }

        public int LowerBoundByTime(long microseconds)
        {
            int low = 0;
            int high = _events.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (_events[middle].IntendedMicroseconds < microseconds) low = middle + 1;
                else high = middle;
            }
            return low;
        }

    }

    // A cached reader keeps the reference List path concrete in hot loops,
    // avoiding an interface call per event while retaining one store contract.
    internal struct MidiEventReader
    {
        private readonly IMidiEventStore _store;
        private readonly IList<MidiEvent> _list;
        private readonly CompactMidiEventStore _compact;

        internal MidiEventReader(IMidiEventStore store)
        {
            if (store == null) throw new ArgumentNullException("store");
            _store = store;
            ListMidiEventStore listStore = store as ListMidiEventStore;
            _list = listStore == null ? null : listStore.Source;
            _compact = store as CompactMidiEventStore;
        }

        public int Count { get { return _list != null ? _list.Count : _compact != null ? _compact.Count : _store.Count; } }
        internal CompactMidiEventStore CompactStore { get { return _compact; } }
        public MidiEventView this[int index]
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get
            {
                if (_list != null) return MidiEventView.FromEvent(_list[index], index);
                return _compact != null ? _compact.GetEvent(index) : _store.GetEvent(index);
            }
        }

        public int LowerBoundByTime(long microseconds)
        {
            if (_list == null) return _compact != null
                ? _compact.LowerBoundByTime(microseconds) : _store.LowerBoundByTime(microseconds);
            int low = 0;
            int high = _list.Count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                if (_list[middle].IntendedMicroseconds < microseconds) low = middle + 1;
                else high = middle;
            }
            return low;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal void GetWorkloadFields(int index, out long microseconds, out int dataLength, out MidiEventKind kind)
        {
            if (_list != null)
            {
                MidiEvent midiEvent = _list[index];
                microseconds = midiEvent.IntendedMicroseconds;
                dataLength = midiEvent.DataLength;
                kind = midiEvent.Kind;
                return;
            }
            if (_compact != null)
            {
                _compact.GetWorkloadFields(index, out microseconds, out dataLength, out kind);
                return;
            }
            MidiEventView view = _store.GetEvent(index);
            microseconds = view.IntendedMicroseconds;
            dataLength = view.DataLength;
            kind = view.Kind;
        }
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct CompactMidiEventRecord
    {
        internal long AbsoluteTick;
        internal long IntendedMicroseconds;
        internal long PayloadOffset;
        internal int Track;
        internal int PayloadLength;
        internal uint PackedShortMessage;
        internal ushort Metadata;
        internal byte Status;
        internal byte Reserved;
    }

    internal sealed class CompactPayloadStore
    {
        internal const int SegmentSize = 1024 * 1024;
        private readonly byte[][] _segments;
        private readonly long _length;

        internal CompactPayloadStore(byte[][] segments, long length)
        {
            _segments = segments ?? new byte[0][];
            _length = length;
        }

        internal long Length { get { return _length; } }
        internal int SegmentCount { get { return _segments.Length; } }

        internal byte ReadByte(long offset)
        {
            if (offset < 0 || offset >= _length) throw new ArgumentOutOfRangeException("offset");
            return _segments[(int)(offset / SegmentSize)][(int)(offset % SegmentSize)];
        }

        internal void CopyTo(long offset, byte[] destination, int destinationOffset, int count)
        {
            if (destination == null) throw new ArgumentNullException("destination");
            if (offset < 0 || count < 0 || offset + count > _length) throw new ArgumentOutOfRangeException("offset");
            int remaining = count;
            while (remaining > 0)
            {
                int segmentIndex = (int)(offset / SegmentSize);
                int segmentOffset = (int)(offset % SegmentSize);
                int copy = Math.Min(remaining, _segments[segmentIndex].Length - segmentOffset);
                Buffer.BlockCopy(_segments[segmentIndex], segmentOffset, destination, destinationOffset, copy);
                offset += copy;
                destinationOffset += copy;
                remaining -= copy;
            }
        }

        internal void AppendTo(List<byte> destination, long offset, int count)
        {
            byte[] buffer = new byte[Math.Min(8192, Math.Max(1, count))];
            int remaining = count;
            while (remaining > 0)
            {
                int copy = Math.Min(remaining, buffer.Length);
                CopyTo(offset, buffer, 0, copy);
                for (int i = 0; i < copy; i++) destination.Add(buffer[i]);
                offset += copy;
                remaining -= copy;
            }
        }
    }

    internal sealed class CompactMidiEventStore : IMidiEventStore
    {
        internal const int RecordSegmentCapacity = 65536;
        private readonly CompactMidiEventRecord[][] _segments;
        private readonly CompactPayloadStore _payloads;
        private readonly int _count;

        internal CompactMidiEventStore(CompactMidiEventRecord[][] segments, CompactPayloadStore payloads, int count)
        {
            _segments = segments ?? new CompactMidiEventRecord[0][];
            _payloads = payloads ?? new CompactPayloadStore(null, 0);
            _count = count;
        }

        public int Count { get { return _count; } }
        internal int RecordSegmentCount { get { return _segments.Length; } }
        internal CompactMidiEventRecord[][] RecordSegments { get { return _segments; } }
        internal CompactPayloadStore Payloads { get { return _payloads; } }
        internal static int RecordSize { get { return Marshal.SizeOf(typeof(CompactMidiEventRecord)); } }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public MidiEventView GetEvent(int index)
        {
            if (index < 0 || index >= _count) throw new ArgumentOutOfRangeException("index");
            return MidiEventView.FromCompact(this, index);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private CompactMidiEventRecord[] Segment(int index) { return _segments[index / RecordSegmentCapacity]; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private int Offset(int index) { return index % RecordSegmentCapacity; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal long GetAbsoluteTick(int index) { return Segment(index)[Offset(index)].AbsoluteTick; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal long GetIntendedMicroseconds(int index) { return Segment(index)[Offset(index)].IntendedMicroseconds; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal int GetTrack(int index) { return Segment(index)[Offset(index)].Track; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal MidiEventKind GetKind(int index) { return (MidiEventKind)(Segment(index)[Offset(index)].Metadata & 0x0F); }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal int GetChannel(int index) { return ((Segment(index)[Offset(index)].Metadata >> 4) & 0x1F) - 1; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal byte GetStatus(int index) { return Segment(index)[Offset(index)].Status; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal int GetDataLength(int index) { return Segment(index)[Offset(index)].PayloadLength; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal uint GetPackedShortMessage(int index) { return Segment(index)[Offset(index)].PackedShortMessage; }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal byte GetDataByte(int eventIndex, int byteIndex)
        {
            CompactMidiEventRecord record = Segment(eventIndex)[Offset(eventIndex)];
            if (byteIndex < 0 || byteIndex >= record.PayloadLength) throw new IndexOutOfRangeException();
            return record.PayloadOffset < 0
                ? (byte)(record.PackedShortMessage >> (byteIndex * 8))
                : _payloads.ReadByte(record.PayloadOffset + byteIndex);
        }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal MidiEventData GetData(int index)
        {
            CompactMidiEventRecord record = Segment(index)[Offset(index)];
            return record.PayloadOffset < 0
                ? MidiEventData.FromShort((byte)record.PackedShortMessage,
                    (byte)(record.PackedShortMessage >> 8), (byte)(record.PackedShortMessage >> 16), record.PayloadLength)
                : MidiEventData.FromSegmented(_payloads, record.PayloadOffset, 0, record.PayloadLength);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal void GetWorkloadFields(int index, out long microseconds, out int dataLength, out MidiEventKind kind)
        {
            CompactMidiEventRecord record = _segments[index / RecordSegmentCapacity][index % RecordSegmentCapacity];
            microseconds = record.IntendedMicroseconds;
            dataLength = record.PayloadLength;
            kind = (MidiEventKind)(record.Metadata & 0x0F);
        }

        public int LowerBoundByTime(long microseconds)
        {
            int low = 0;
            int high = _count;
            while (low < high)
            {
                int middle = low + ((high - low) >> 1);
                CompactMidiEventRecord record = _segments[middle / RecordSegmentCapacity][middle % RecordSegmentCapacity];
                if (record.IntendedMicroseconds < microseconds) low = middle + 1;
                else high = middle;
            }
            return low;
        }

        internal static CompactMidiEventStore Convert(IList<MidiEvent> source, CancellationToken cancellationToken,
            Action<long, long> progress)
        {
            if (source == null) throw new ArgumentNullException("source");
            Builder builder = new Builder();
            for (int index = 0; index < source.Count; index++)
            {
                if ((index & 16383) == 0)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (progress != null) progress(index, source.Count);
                }
                MidiEvent midiEvent = source[index];
                builder.Add(midiEvent);
                // Release converted objects and their long arrays progressively.
                source[index] = null;
            }
            if (progress != null) progress(source.Count, source.Count);
            return builder.Complete();
        }

        internal sealed class Builder
        {
            private readonly List<CompactMidiEventRecord[]> _recordSegments = new List<CompactMidiEventRecord[]>();
            private readonly List<byte[]> _payloadSegments = new List<byte[]>();
            private CompactMidiEventRecord[] _currentRecords;
            private byte[] _currentPayload;
            private int _recordOffset;
            private int _payloadOffset;
            private int _count;
            private long _payloadLength;

            internal void Add(MidiEvent midiEvent)
            {
                if (midiEvent == null) throw new ArgumentNullException("midiEvent");
                if (_currentRecords == null || _recordOffset == _currentRecords.Length)
                {
                    _currentRecords = new CompactMidiEventRecord[RecordSegmentCapacity];
                    _recordSegments.Add(_currentRecords);
                    _recordOffset = 0;
                }
                CompactMidiEventRecord record = new CompactMidiEventRecord();
                record.AbsoluteTick = midiEvent.AbsoluteTick;
                record.IntendedMicroseconds = midiEvent.IntendedMicroseconds;
                record.Track = midiEvent.Track;
                record.PayloadLength = midiEvent.DataLength;
                record.Status = midiEvent.Status;
                record.Metadata = (ushort)(((int)midiEvent.Kind & 0x0F) | ((midiEvent.Channel + 1) << 4));
                if (midiEvent.DataLength <= 3)
                {
                    record.PayloadOffset = -1;
                    record.PackedShortMessage = midiEvent.PackedShortMessage;
                }
                else
                {
                    record.PayloadOffset = _payloadLength;
                    MidiEventData data = midiEvent.Data;
                    for (int i = 0; i < data.Length; i++) AppendPayloadByte(data[i]);
                }
                _currentRecords[_recordOffset++] = record;
                _count++;
            }

            private void AppendPayloadByte(byte value)
            {
                if (_currentPayload == null || _payloadOffset == _currentPayload.Length)
                {
                    _currentPayload = new byte[CompactPayloadStore.SegmentSize];
                    _payloadSegments.Add(_currentPayload);
                    _payloadOffset = 0;
                }
                _currentPayload[_payloadOffset++] = value;
                _payloadLength++;
            }

            internal CompactMidiEventStore Complete()
            {
                if (_currentRecords != null && _recordOffset > 0 && _recordOffset < _currentRecords.Length)
                {
                    CompactMidiEventRecord[] trimmed = new CompactMidiEventRecord[_recordOffset];
                    Array.Copy(_currentRecords, trimmed, _recordOffset);
                    _recordSegments[_recordSegments.Count - 1] = trimmed;
                }
                if (_currentPayload != null && _payloadOffset > 0 && _payloadOffset < _currentPayload.Length)
                {
                    byte[] trimmed = new byte[_payloadOffset];
                    Buffer.BlockCopy(_currentPayload, 0, trimmed, 0, _payloadOffset);
                    _payloadSegments[_payloadSegments.Count - 1] = trimmed;
                }
                return new CompactMidiEventStore(_recordSegments.ToArray(),
                    new CompactPayloadStore(_payloadSegments.ToArray(), _payloadLength), _count);
            }
        }
    }

    internal sealed class MidiEventStoreCompatibilityList : IList<MidiEvent>
    {
        private readonly IMidiEventStore _store;
        internal MidiEventStoreCompatibilityList(IMidiEventStore store) { _store = store; }
        public int Count { get { return _store.Count; } }
        public bool IsReadOnly { get { return true; } }
        public MidiEvent this[int index] { get { return _store.GetEvent(index).ToCompatibilityEvent(); } set { throw new NotSupportedException(); } }
        public int IndexOf(MidiEvent item) { return -1; }
        public bool Contains(MidiEvent item) { return false; }
        public void CopyTo(MidiEvent[] array, int arrayIndex) { for (int i = 0; i < Count; i++) array[arrayIndex + i] = this[i]; }
        public IEnumerator<MidiEvent> GetEnumerator() { for (int i = 0; i < Count; i++) yield return this[i]; }
        IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
        public void Add(MidiEvent item) { throw new NotSupportedException(); }
        public void Clear() { throw new NotSupportedException(); }
        public void Insert(int index, MidiEvent item) { throw new NotSupportedException(); }
        public bool Remove(MidiEvent item) { throw new NotSupportedException(); }
        public void RemoveAt(int index) { throw new NotSupportedException(); }
    }
}
