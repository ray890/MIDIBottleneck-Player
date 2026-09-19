using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Inline storage for ordinary MIDI messages. Payloads longer than three
    // bytes retain their existing array, principally for SysEx fragments.
    // Reading an inline message never constructs a replacement byte array.
    internal struct MidiEventData
    {
        private byte[] _longPayload;
        private uint _packed;
        private int _length;

        private MidiEventData(byte[] bytes)
        {
            _longPayload = null;
            _packed = 0;
            _length = bytes == null ? 0 : bytes.Length;
            if (_length <= 3)
            {
                uint packed = 0;
                for (int i = 0; i < _length; i++) packed |= (uint)bytes[i] << (i * 8);
                _packed = packed;
            }
            else _longPayload = bytes;
        }

        public int Length { get { return _length; } }
        public byte this[int index]
        {
            get
            {
                if (index < 0 || index >= _length) throw new IndexOutOfRangeException();
                return _longPayload == null ? (byte)(_packed >> (index * 8)) : _longPayload[index];
            }
        }

        internal uint PackedShortMessage
        {
            get
            {
                if (_length > 3) throw new InvalidOperationException("The MIDI payload is not a short message.");
                return _packed;
            }
        }

        internal bool UsesHeapPayload { get { return _longPayload != null; } }
        internal byte[] LongPayload { get { return _longPayload; } }
        internal uint PackedValue { get { return _packed; } }

        internal void AppendTo(List<byte> destination)
        {
            if (destination == null) throw new ArgumentNullException("destination");
            if (_longPayload != null)
            {
                destination.AddRange(_longPayload);
                return;
            }
            for (int i = 0; i < _length; i++) destination.Add((byte)(_packed >> (i * 8)));
        }

        internal byte[] ToArray()
        {
            byte[] result = new byte[_length];
            if (_longPayload != null) Buffer.BlockCopy(_longPayload, 0, result, 0, _length);
            else for (int i = 0; i < _length; i++) result[i] = (byte)(_packed >> (i * 8));
            return result;
        }

        internal string ToHexString()
        {
            if (_length == 0) return String.Empty;
            if (_longPayload != null) return BitConverter.ToString(_longPayload);
            return BitConverter.ToString(ToArray());
        }

        public static implicit operator MidiEventData(byte[] bytes)
        {
            return new MidiEventData(bytes);
        }

        internal static MidiEventData FromShort(byte first, byte second, byte third, int length)
        {
            if (length < 0 || length > 3) throw new ArgumentOutOfRangeException("length");
            MidiEventData result = new MidiEventData();
            result._packed = (uint)(first | second << 8 | third << 16);
            result._length = length;
            return result;
        }

        internal static MidiEventData FromStorage(byte[] longPayload, uint packed, int length)
        {
            MidiEventData result = new MidiEventData();
            result._longPayload = longPayload;
            result._packed = packed;
            result._length = length;
            return result;
        }
    }

    internal enum MidiEventKind
    {
        NoteOff,
        NoteOn,
        PolyphonicAftertouch,
        ControlChange,
        ProgramChange,
        ChannelAftertouch,
        PitchBend,
        SystemExclusive,
        SystemMessage
    }

    internal sealed class MidiEvent
    {
        public long AbsoluteTick;
        public long IntendedMicroseconds;
        public int Track;
        public int Order;
        public MidiEventKind Kind;
        public int Channel;
        public byte Status;
        private byte[] _longPayload;
        // Low 24 bits are the short payload; the high byte is its length.
        // This keeps the event object compact while Data remains a read-only
        // value view for diagnostics and tests.
        private uint _packedDataAndLength;
        public MidiEventData Data
        {
            get
            {
                return MidiEventData.FromStorage(_longPayload,
                    _packedDataAndLength & 0x00FFFFFFU,
                    _longPayload == null ? (int)(_packedDataAndLength >> 24) : _longPayload.Length);
            }
            set
            {
                int length = value.Length;
                _longPayload = value.LongPayload;
                _packedDataAndLength = _longPayload == null
                    ? (value.PackedValue & 0x00FFFFFFU) | ((uint)length << 24)
                    : 0;
            }
        }

        internal int DataLength { get { return _longPayload == null ? (int)(_packedDataAndLength >> 24) : _longPayload.Length; } }
        internal uint PackedShortMessage
        {
            get
            {
                if (DataLength > 3) throw new InvalidOperationException("The MIDI payload is not a short message.");
                return _packedDataAndLength & 0x00FFFFFFU;
            }
        }
        internal byte GetDataByte(int index)
        {
            int length = DataLength;
            if (index < 0 || index >= length) throw new IndexOutOfRangeException();
            return _longPayload == null
                ? (byte)(_packedDataAndLength >> (index * 8))
                : _longPayload[index];
        }
        internal bool UsesHeapPayload { get { return _longPayload != null; } }
        public int EventIndex;

        public string Description
        {
            get
            {
                if (Channel >= 0)
                    return Kind + " ch " + (Channel + 1);
                return Kind.ToString();
            }
        }
    }

    internal sealed class MidiSong
    {
        public string FilePath;
        public long FileSizeBytes;
        public int Format;
        public int TrackCount;
        public int TicksPerQuarterNote;
        public long NoteCount;
        public List<MidiEvent> Events;
        public long DurationMicroseconds;

        private IMidiEventStore _eventStore;
        private ChannelSourceValueIndex _channelSourceValues;
        private readonly object _channelSourceValuesSync = new object();

        internal IMidiEventStore EventStore
        {
            get
            {
                ListMidiEventStore listStore = _eventStore as ListMidiEventStore;
                if (Events != null && (listStore == null || !Object.ReferenceEquals(listStore.Source, Events)))
                {
                    _eventStore = new ListMidiEventStore(Events);
                    _channelSourceValues = null;
                }
                if (_eventStore == null)
                {
                    Events = new List<MidiEvent>();
                    _eventStore = new ListMidiEventStore(Events);
                }
                return _eventStore;
            }
        }

        internal MidiEventReader GetEventReader()
        {
            return new MidiEventReader(EventStore);
        }

        internal void SetEventStore(IMidiEventStore eventStore)
        {
            if (eventStore == null) throw new ArgumentNullException("eventStore");
            _eventStore = eventStore;
            _channelSourceValues = null;
            ListMidiEventStore listStore = eventStore as ListMidiEventStore;
            Events = listStore == null ? null : listStore.Source;
        }

        internal void SetChannelSourceValueIndex(ChannelSourceValueIndex index)
        {
            _channelSourceValues = index;
        }

        internal ChannelSourceValueIndex GetChannelSourceValueIndex()
        {
            ChannelSourceValueIndex index = _channelSourceValues;
            if (index != null) return index;
            lock (_channelSourceValuesSync)
            {
                if (_channelSourceValues == null)
                    _channelSourceValues = ChannelSourceValueIndex.Build(EventStore);
                return _channelSourceValues;
            }
        }
    }

    internal enum ProcessingMode
    {
        Queue,
        Drop
    }

    internal enum OverflowPolicy
    {
        DropNewest,
        DropOldest,
        ClearBufferAndCatchUp,
        DropIncomingCompleteNotes
    }

    internal enum PlaybackState
    {
        Stopped,
        Playing,
        Paused,
        Completed
    }

    internal sealed class PlaybackSnapshot
    {
        public PlaybackState State;
        public long ProcessingMicroseconds;
        public long QueueLength;
        public long OutstandingEvents;
        public long MaximumQueueLength;
        public long ProcessedEvents;
        public long DroppedEvents;
        public long PlaybackMicroseconds;
        public long IntendedTimelineMicroseconds;
        public long LastDispatchedTimelineMicroseconds;
        public long CurrentLagMicroseconds;
        public long MaximumLagMicroseconds;
        public ServiceDurationMode ServiceDurationMode;
        public long MidiBitrate;
        public bool SimulateSlowdown;
        public bool QueueLengthLimitEnabled;
        public int QueueLengthLimit;
        public OverflowPolicy OverflowPolicy;
    }

    internal interface IMidiOutput
    {
        void Send(MidiEvent midiEvent);
        void Panic();
        void Reset();
    }

    internal interface IMidiOutputContext
    {
        string SourceFile { get; set; }
    }

    internal static class MidiOutputSafety
    {
        // Reset discards queued/native work. Panic must follow it so the
        // controller messages cannot themselves be discarded by that reset.
        public static void ResetAndSilence(IMidiOutput output)
        {
            if (output == null) return;
            Exception resetFailure = null;
            try { output.Reset(); }
            catch (Exception ex) { resetFailure = ex; }
            try { output.Panic(); }
            catch (Exception panicFailure)
            {
                if (resetFailure == null) throw;
                throw new InvalidOperationException("MIDI reset and post-reset panic both failed.",
                    new AggregateException(resetFailure, panicFailure));
            }
            if (resetFailure != null)
                throw new InvalidOperationException("MIDI reset failed; a post-reset panic was still attempted.", resetFailure);
        }
    }
}
