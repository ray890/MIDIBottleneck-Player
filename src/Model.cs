using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Inline storage for ordinary MIDI messages. Payloads longer than three
    // bytes retain their existing array, principally for SysEx fragments.
    // Reading an inline message never constructs a replacement byte array.
    internal struct MidiEventData
    {
        private object _payloadSource;
        private long _payloadOffset;
        private uint _packed;
        private int _length;

        private MidiEventData(byte[] bytes)
        {
            _payloadSource = null;
            _payloadOffset = 0;
            _packed = 0;
            _length = bytes == null ? 0 : bytes.Length;
            if (_length <= 3)
            {
                uint packed = 0;
                for (int i = 0; i < _length; i++) packed |= (uint)bytes[i] << (i * 8);
                _packed = packed;
            }
            else _payloadSource = bytes;
        }

        public int Length { get { return _length; } }
        public byte this[int index]
        {
            get
            {
                if (index < 0 || index >= _length) throw new IndexOutOfRangeException();
                if (_payloadSource == null) return (byte)(_packed >> (index * 8));
                byte[] bytes = _payloadSource as byte[];
                if (bytes != null) return bytes[(int)_payloadOffset + index];
                return ((CompactPayloadStore)_payloadSource).ReadByte(_payloadOffset + index);
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

        internal bool UsesHeapPayload { get { return _payloadSource is byte[]; } }
        internal byte[] LongPayload { get { return _payloadSource as byte[]; } }
        internal uint PackedValue { get { return _packed; } }

        internal void AppendTo(List<byte> destination)
        {
            if (destination == null) throw new ArgumentNullException("destination");
            if (_payloadSource != null)
            {
                byte[] bytes = _payloadSource as byte[];
                if (bytes != null)
                {
                    for (int i = 0; i < _length; i++) destination.Add(bytes[(int)_payloadOffset + i]);
                }
                else ((CompactPayloadStore)_payloadSource).AppendTo(destination, _payloadOffset, _length);
                return;
            }
            for (int i = 0; i < _length; i++) destination.Add((byte)(_packed >> (i * 8)));
        }

        internal byte[] ToArray()
        {
            byte[] result = new byte[_length];
            if (_payloadSource != null)
            {
                byte[] bytes = _payloadSource as byte[];
                if (bytes != null) Buffer.BlockCopy(bytes, (int)_payloadOffset, result, 0, _length);
                else ((CompactPayloadStore)_payloadSource).CopyTo(_payloadOffset, result, 0, _length);
            }
            else for (int i = 0; i < _length; i++) result[i] = (byte)(_packed >> (i * 8));
            return result;
        }

        internal string ToHexString()
        {
            if (_length == 0) return String.Empty;
            if (_payloadSource != null) return BitConverter.ToString(ToArray());
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
            result._payloadSource = longPayload;
            result._payloadOffset = 0;
            result._packed = packed;
            result._length = length;
            return result;
        }

        internal static MidiEventData FromSegmented(CompactPayloadStore payloadStore, long offset, uint packed, int length)
        {
            MidiEventData result = new MidiEventData();
            result._payloadSource = payloadStore;
            result._payloadOffset = offset;
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

    // Allocation-free value view used by production playback and Analysis.
    // Compact records provide their payload through the immutable segmented
    // side store; the reference backend supplies the same view over a legacy
    // MidiEvent object.
    internal struct MidiEventView
    {
        private CompactMidiEventStore _compact;
        private MidiEvent _legacy;
        private int _eventIndex;

        public long AbsoluteTick { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.AbsoluteTick : _compact.GetAbsoluteTick(_eventIndex); } }
        public long IntendedMicroseconds { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.IntendedMicroseconds : _compact.GetIntendedMicroseconds(_eventIndex); } }
        public int Track { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.Track : _compact.GetTrack(_eventIndex); } }
        public MidiEventKind Kind { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.Kind : _compact.GetKind(_eventIndex); } }
        public int Channel { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.Channel : _compact.GetChannel(_eventIndex); } }
        public byte Status { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.Status : _compact.GetStatus(_eventIndex); } }
        public int EventIndex { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _eventIndex; } }
        public MidiEventData Data { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.Data : _compact.GetData(_eventIndex); } }
        internal bool IsValid { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null || _compact != null; } }
        internal int DataLength { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.DataLength : _compact.GetDataLength(_eventIndex); } }
        internal uint PackedShortMessage { [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)] get { return _legacy != null ? _legacy.PackedShortMessage : _compact.GetPackedShortMessage(_eventIndex); } }
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)]
        internal byte GetDataByte(int index) { return _legacy != null ? _legacy.GetDataByte(index) : _compact.GetDataByte(_eventIndex, index); }

        internal static MidiEventView FromEvent(MidiEvent midiEvent, int eventIndex)
        {
            if (midiEvent == null) return default(MidiEventView);
            MidiEventView result = new MidiEventView();
            result._legacy = midiEvent;
            result._eventIndex = eventIndex;
            return result;
        }

        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)]
        internal static MidiEventView FromCompact(CompactMidiEventStore store, int eventIndex)
        {
            MidiEventView result = new MidiEventView();
            result._compact = store;
            result._eventIndex = eventIndex;
            return result;
        }

        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)]
        internal static MidiEventView Create(long tick, long microseconds, int track, MidiEventKind kind,
            int channel, byte status, int eventIndex, MidiEventData data)
        {
            MidiEvent generated = new MidiEvent
            {
                AbsoluteTick = tick,
                IntendedMicroseconds = microseconds,
                Track = track,
                Kind = kind,
                Channel = channel,
                Status = status,
                EventIndex = eventIndex,
                Data = data
            };
            return FromEvent(generated, eventIndex);
        }

        internal MidiEvent ToCompatibilityEvent()
        {
            return new MidiEvent
            {
                AbsoluteTick = AbsoluteTick,
                IntendedMicroseconds = IntendedMicroseconds,
                Track = Track,
                Kind = Kind,
                Channel = Channel,
                Status = Status,
                EventIndex = EventIndex,
                Data = Data.Length > 3 ? (MidiEventData)Data.ToArray() : Data
            };
        }

        public string Description
        {
            get { return Channel >= 0 ? Kind + " ch " + (Channel + 1) : Kind.ToString(); }
        }

        public static implicit operator MidiEventView(MidiEvent midiEvent)
        {
            return FromEvent(midiEvent, midiEvent == null ? -1 : midiEvent.EventIndex);
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
        public IList<MidiEvent> Events;
        public long DurationMicroseconds;

        private IMidiEventStore _eventStore;
        private IList<MidiEvent> _eventCompatibility;
        private ChannelSourceValueIndex _channelSourceValues;
        private readonly object _channelSourceValuesSync = new object();
        private MidiStateChaseIndex _stateChaseIndex;
        private readonly object _stateChaseSync = new object();

        internal IMidiEventStore EventStore
        {
            get
            {
                if (_eventStore == null)
                {
                    if (Events == null) Events = new List<MidiEvent>();
                    _eventStore = new ListMidiEventStore(Events);
                    _eventCompatibility = null;
                    return _eventStore;
                }
                ListMidiEventStore listStore = _eventStore as ListMidiEventStore;
                if (Events != null && listStore != null && !Object.ReferenceEquals(listStore.Source, Events))
                {
                    _eventStore = new ListMidiEventStore(Events);
                    _eventCompatibility = null;
                    _channelSourceValues = null;
                    _stateChaseIndex = null;
                }
                else if (Events != null && _eventStore != null && listStore == null &&
                    !Object.ReferenceEquals(Events, _eventCompatibility))
                {
                    _eventStore = new ListMidiEventStore(Events);
                    _eventCompatibility = null;
                    _channelSourceValues = null;
                    _stateChaseIndex = null;
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
            _stateChaseIndex = null;
            ListMidiEventStore listStore = eventStore as ListMidiEventStore;
            _eventCompatibility = listStore == null ? new MidiEventStoreCompatibilityList(eventStore) : null;
            Events = listStore == null ? _eventCompatibility : listStore.Source;
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

        internal void SetMidiStateChaseIndex(MidiStateChaseIndex index)
        { _stateChaseIndex = index; }

        internal MidiStateChaseIndex GetMidiStateChaseIndex()
        {
            MidiStateChaseIndex index = _stateChaseIndex;
            if (index != null) return index;
            lock (_stateChaseSync)
            {
                if (_stateChaseIndex == null)
                    _stateChaseIndex = MidiStateChaseIndex.Build(EventStore);
                return _stateChaseIndex;
            }
        }
    }

    internal enum ProcessingMode
    {
        Queue,
        Drop,
        PerNoteIntervalGate
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
        public long GateFilteredEvents;
        public bool VirtualQueueActive;
        public long VirtualQueueLength;
        public long VirtualOutstandingEvents;
        public long VirtualMaximumQueueLength;
        public long PlaybackMicroseconds;
        public long IntendedTimelineMicroseconds;
        public long LastDispatchedTimelineMicroseconds;
        public long EffectiveSpeedFrontierMicroseconds;
        public long CurrentLagMicroseconds;
        public long MaximumLagMicroseconds;
        public ServiceDurationMode ServiceDurationMode;
        public long MidiBitrate;
        public bool SimulateSlowdown;
        public bool QueueLengthLimitEnabled;
        public int QueueLengthLimit;
        public OverflowPolicy OverflowPolicy;
        public ProcessingMode ProcessingMode;
    }

    internal interface IMidiOutput
    {
        void Send(MidiEventView midiEvent);
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
