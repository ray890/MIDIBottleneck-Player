using System;
using System.Threading;

namespace MidiBottleneck
{
    internal enum ChannelAttribute
    {
        BankMsb = 0,
        BankLsb = 1,
        Program = 2,
        Volume = 3,
        Expression = 4,
        Pan = 5,
        Sustain = 6,
        PitchBend = 7,
        Aftertouch = 8
    }

    internal sealed class ChannelOverrideState
    {
        internal const int AttributeCount = 9;
        internal const int AutoValue = Int32.MinValue;
        private readonly int[] _values = new int[16 * AttributeCount];
        private readonly int[] _pendingMasks = new int[16];
        private readonly int[] _statusMasks = new int[16];
        private readonly long[] _suppressed = new long[16];
        private readonly object _changeSync = new object();
        private int _anyOverrides;
        private int _filterGeneration;

        internal ChannelOverrideState()
        {
            for (int i = 0; i < _values.Length; i++) _values[i] = AutoValue;
        }

        internal bool AnyOverrides { get { return Volatile.Read(ref _anyOverrides) != 0; } }
        internal int FilterGeneration { get { return Volatile.Read(ref _filterGeneration); } }

        internal int GetValue(int channel, ChannelAttribute attribute)
        {
            return Volatile.Read(ref _values[Index(channel, attribute)]);
        }

        internal bool SetValue(int channel, ChannelAttribute attribute, int value)
        {
            Validate(channel, attribute, value);
            lock (_changeSync)
            {
                int index = Index(channel, attribute);
                if (_values[index] == value) return false;
                Volatile.Write(ref _values[index], value);
                if (value != AutoValue) _pendingMasks[channel] |= 1 << (int)attribute;
                RecalculateAnyOverridesLocked();
                unchecked { _filterGeneration++; }
                return true;
            }
        }

        internal void Clear()
        {
            lock (_changeSync)
            {
                for (int i = 0; i < _values.Length; i++) Volatile.Write(ref _values[i], AutoValue);
                Array.Clear(_pendingMasks, 0, _pendingMasks.Length);
                Array.Clear(_statusMasks, 0, _statusMasks.Length);
                Array.Clear(_suppressed, 0, _suppressed.Length);
                Volatile.Write(ref _anyOverrides, 0);
                unchecked { _filterGeneration++; }
            }
        }

        internal void ResetStatistics()
        {
            for (int channel = 0; channel < 16; channel++) Interlocked.Exchange(ref _suppressed[channel], 0);
        }

        internal void ResetSentStatisticsPreservingDropped()
        {
            // Override suppression is neither a sent event nor a finite-queue
            // drop. Preserve it across Seek just as channel Drop counts are
            // preserved by the monitor contract.
        }

        internal void MarkAllForcedPending()
        {
            if (!AnyOverrides) return;
            lock (_changeSync)
            {
                for (int channel = 0; channel < 16; channel++)
                {
                    int mask = 0;
                    for (int attribute = 0; attribute < AttributeCount; attribute++)
                        if (_values[channel * AttributeCount + attribute] != AutoValue) mask |= 1 << attribute;
                    _pendingMasks[channel] |= mask;
                }
            }
        }

        internal void MarkChannelForcedPending(int channel)
        {
            if (!AnyOverrides || channel < 0 || channel >= 16) return;
            lock (_changeSync)
            {
                int mask = 0;
                for (int attribute = 0; attribute < AttributeCount; attribute++)
                    if (_values[channel * AttributeCount + attribute] != AutoValue) mask |= 1 << attribute;
                _pendingMasks[channel] |= mask;
            }
        }

        internal int TakePendingMask(int channel)
        {
            lock (_changeSync)
            {
                int mask = _pendingMasks[channel];
                _pendingMasks[channel] = 0;
                return mask;
            }
        }

        internal void Requeue(int channel, int mask)
        {
            if (mask == 0) return;
            lock (_changeSync) _pendingMasks[channel] |= mask;
        }

        internal bool ShouldSuppress(MidiEvent midiEvent, out ChannelAttribute attribute, out int forcedValue)
        {
            attribute = ChannelAttribute.BankMsb;
            forcedValue = AutoValue;
            if (!AnyOverrides || midiEvent == null || midiEvent.Channel < 0 || midiEvent.Channel >= 16) return false;
            int status = midiEvent.Status & 0xF0;
            int statusBit = status == 0xB0 ? 1 : status == 0xC0 ? 2 : status == 0xE0 ? 4 : status == 0xD0 ? 8 : 0;
            if (statusBit == 0 || (Volatile.Read(ref _statusMasks[midiEvent.Channel]) & statusBit) == 0) return false;
            if (status == 0xB0 && midiEvent.DataLength > 2 && midiEvent.GetDataByte(1) == 121)
            {
                if (ConflictsWithReset(midiEvent.Channel, ChannelAttribute.Expression, 127, out forcedValue)) { attribute = ChannelAttribute.Expression; return true; }
                if (ConflictsWithReset(midiEvent.Channel, ChannelAttribute.Pan, 64, out forcedValue)) { attribute = ChannelAttribute.Pan; return true; }
                if (ConflictsWithReset(midiEvent.Channel, ChannelAttribute.Sustain, 0, out forcedValue)) { attribute = ChannelAttribute.Sustain; return true; }
                if (ConflictsWithReset(midiEvent.Channel, ChannelAttribute.PitchBend, 0, out forcedValue)) { attribute = ChannelAttribute.PitchBend; return true; }
                if (ConflictsWithReset(midiEvent.Channel, ChannelAttribute.Aftertouch, 0, out forcedValue)) { attribute = ChannelAttribute.Aftertouch; return true; }
                return false;
            }
            int sourceValue;
            if (!TryClassify(midiEvent, out attribute, out sourceValue)) return false;
            forcedValue = GetValue(midiEvent.Channel, attribute);
            return forcedValue != AutoValue && sourceValue != forcedValue;
        }

        internal void RecordSuppressed(int channel)
        {
            if (channel >= 0 && channel < 16) Interlocked.Increment(ref _suppressed[channel]);
        }

        internal void ApplyToSnapshot(ChannelPlaybackSnapshot snapshot)
        {
            if (snapshot == null || snapshot.Channels == null) return;
            for (int channel = 0; channel < Math.Min(16, snapshot.Channels.Length); channel++)
            {
                MidiChannelSnapshot state = snapshot.Channels[channel];
                state.OverrideSuppressedEvents = Interlocked.Read(ref _suppressed[channel]);
                int mask = 0;
                for (int attribute = 0; attribute < AttributeCount; attribute++)
                {
                    int value = GetValue(channel, (ChannelAttribute)attribute);
                    if (value != AutoValue)
                    {
                        mask |= 1 << attribute;
                        SetForcedValue(ref state, (ChannelAttribute)attribute, value);
                    }
                }
                state.ForcedAttributeMask = mask;
                lock (_changeSync) state.PendingForcedAttributeMask = _pendingMasks[channel] & mask;
                snapshot.Channels[channel] = state;
            }
        }

        internal static MidiEvent CreateMessage(int channel, ChannelAttribute attribute, int value)
        {
            byte status;
            byte first;
            byte second;
            switch (attribute)
            {
                case ChannelAttribute.BankMsb: status = (byte)(0xB0 | channel); first = 0; second = (byte)value; break;
                case ChannelAttribute.BankLsb: status = (byte)(0xB0 | channel); first = 32; second = (byte)value; break;
                case ChannelAttribute.Program:
                    status = (byte)(0xC0 | channel);
                    return NewMessage(channel, MidiEventKind.ProgramChange, status, new byte[] { status, (byte)value });
                case ChannelAttribute.Volume: status = (byte)(0xB0 | channel); first = 7; second = (byte)value; break;
                case ChannelAttribute.Expression: status = (byte)(0xB0 | channel); first = 11; second = (byte)value; break;
                case ChannelAttribute.Pan: status = (byte)(0xB0 | channel); first = 10; second = (byte)value; break;
                case ChannelAttribute.Sustain: status = (byte)(0xB0 | channel); first = 64; second = (byte)(value == 0 ? 0 : 127); break;
                case ChannelAttribute.PitchBend:
                    status = (byte)(0xE0 | channel);
                    int raw = value + 8192;
                    return NewMessage(channel, MidiEventKind.PitchBend, status,
                        new byte[] { status, (byte)(raw & 0x7F), (byte)((raw >> 7) & 0x7F) });
                case ChannelAttribute.Aftertouch:
                    status = (byte)(0xD0 | channel);
                    return NewMessage(channel, MidiEventKind.SystemMessage, status, new byte[] { status, (byte)value });
                default: throw new ArgumentOutOfRangeException("attribute");
            }
            return NewMessage(channel, MidiEventKind.ControlChange, status, new byte[] { status, first, second });
        }

        internal static MidiEvent CreateControllerMessage(int channel, int controller, int value)
        {
            byte status = (byte)(0xB0 | channel);
            return NewMessage(channel, MidiEventKind.ControlChange, status,
                new byte[] { status, (byte)controller, (byte)value });
        }

        internal static bool TryClassify(MidiEvent midiEvent, out ChannelAttribute attribute, out int value)
        {
            attribute = ChannelAttribute.BankMsb;
            value = 0;
            int status = midiEvent.Status & 0xF0;
            int first = midiEvent.DataLength > 1 ? midiEvent.GetDataByte(1) : 0;
            int second = midiEvent.DataLength > 2 ? midiEvent.GetDataByte(2) : 0;
            if (status == 0xC0) { attribute = ChannelAttribute.Program; value = first; return true; }
            if (status == 0xD0) { attribute = ChannelAttribute.Aftertouch; value = first; return true; }
            if (status == 0xE0) { attribute = ChannelAttribute.PitchBend; value = ((second << 7) | first) - 8192; return true; }
            if (status != 0xB0) return false;
            switch (first)
            {
                case 0: attribute = ChannelAttribute.BankMsb; value = second; return true;
                case 32: attribute = ChannelAttribute.BankLsb; value = second; return true;
                case 7: attribute = ChannelAttribute.Volume; value = second; return true;
                case 11: attribute = ChannelAttribute.Expression; value = second; return true;
                case 10: attribute = ChannelAttribute.Pan; value = second; return true;
                case 64: attribute = ChannelAttribute.Sustain; value = second >= 64 ? 1 : 0; return true;
                default: return false;
            }
        }

        internal static int Minimum(ChannelAttribute attribute)
        {
            return attribute == ChannelAttribute.PitchBend ? -8192 : 0;
        }

        internal static int Maximum(ChannelAttribute attribute)
        {
            return attribute == ChannelAttribute.PitchBend ? 8191 :
                attribute == ChannelAttribute.Sustain ? 1 : 127;
        }

        private static MidiEvent NewMessage(int channel, MidiEventKind kind, byte status, byte[] data)
        {
            return new MidiEvent { Channel = channel, Kind = kind, Status = status, Data = data };
        }

        private static void SetForcedValue(ref MidiChannelSnapshot state, ChannelAttribute attribute, int value)
        {
            switch (attribute)
            {
                case ChannelAttribute.BankMsb: state.ForcedBankMsb = value; break;
                case ChannelAttribute.BankLsb: state.ForcedBankLsb = value; break;
                case ChannelAttribute.Program: state.ForcedProgram = value; break;
                case ChannelAttribute.Volume: state.ForcedVolume = value; break;
                case ChannelAttribute.Expression: state.ForcedExpression = value; break;
                case ChannelAttribute.Pan: state.ForcedPan = value; break;
                case ChannelAttribute.Sustain: state.ForcedSustain = value; break;
                case ChannelAttribute.PitchBend: state.ForcedPitchBend = value; break;
                case ChannelAttribute.Aftertouch: state.ForcedAftertouch = value; break;
            }
        }

        private static int Index(int channel, ChannelAttribute attribute)
        {
            return channel * AttributeCount + (int)attribute;
        }

        private bool ConflictsWithReset(int channel, ChannelAttribute attribute, int resetValue, out int forcedValue)
        {
            forcedValue = GetValue(channel, attribute);
            return forcedValue != AutoValue && forcedValue != resetValue;
        }

        private static void Validate(int channel, ChannelAttribute attribute, int value)
        {
            if (channel < 0 || channel >= 16) throw new ArgumentOutOfRangeException("channel");
            if ((int)attribute < 0 || (int)attribute >= AttributeCount) throw new ArgumentOutOfRangeException("attribute");
            if (value != AutoValue && (value < Minimum(attribute) || value > Maximum(attribute)))
                throw new ArgumentOutOfRangeException("value");
        }

        private void RecalculateAnyOverridesLocked()
        {
            bool any = false;
            for (int channel = 0; channel < 16; channel++)
            {
                int mask = 0;
                for (int attribute = 0; attribute < AttributeCount; attribute++)
                {
                    if (_values[channel * AttributeCount + attribute] == AutoValue) continue;
                    any = true;
                    ChannelAttribute kind = (ChannelAttribute)attribute;
                    if (kind == ChannelAttribute.Program) mask |= 2;
                    else if (kind == ChannelAttribute.PitchBend) mask |= 4;
                    else if (kind == ChannelAttribute.Aftertouch) mask |= 8;
                    else mask |= 1;
                }
                Volatile.Write(ref _statusMasks[channel], mask);
            }
            Volatile.Write(ref _anyOverrides, any ? 1 : 0);
        }
    }
}
