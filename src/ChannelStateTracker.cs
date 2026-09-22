using System;
using System.Threading;

namespace MidiBottleneck
{
    internal struct MidiChannelSnapshot
    {
        public int Channel;
        public int KeysDown;
        public int PeakKeysDown;
        public long SentEvents;
        public long DroppedEvents;
        public long OverrideSuppressedEvents;
        public long MutedFilteredEvents;
        public bool Enabled;
        public int BankMsb;
        public int BankLsb;
        public int Program;
        public int Volume;
        public int Expression;
        public int Pan;
        public int Sustain;
        public int PitchBend;
        public int ChannelPressure;
        public long LastDispatchedMicroseconds;
        public int HistoricalAttributeMask;
        public bool LastPositionHistorical;
        public int ForcedAttributeMask;
        public int PendingForcedAttributeMask;
        public int ForcedBankMsb;
        public int ForcedBankLsb;
        public int ForcedProgram;
        public int ForcedVolume;
        public int ForcedExpression;
        public int ForcedPan;
        public int ForcedSustain;
        public int ForcedPitchBend;
        public int ForcedAftertouch;
    }

    internal sealed class ChannelPlaybackSnapshot
    {
        internal readonly MidiChannelSnapshot[] Channels;

        internal ChannelPlaybackSnapshot(MidiChannelSnapshot[] channels)
        {
            Channels = channels;
        }

        internal static ChannelPlaybackSnapshot Empty()
        {
            MidiChannelSnapshot[] channels = new MidiChannelSnapshot[16];
            for (int i = 0; i < channels.Length; i++) channels[i] = Unknown(i);
            return new ChannelPlaybackSnapshot(channels);
        }

        internal static MidiChannelSnapshot Unknown(int channel)
        {
            return new MidiChannelSnapshot
            {
                Channel = channel,
                Enabled = true,
                BankMsb = -1,
                BankLsb = -1,
                Program = -1,
                Volume = -1,
                Expression = -1,
                Pan = -1,
                Sustain = -1,
                PitchBend = Int32.MinValue,
                ChannelPressure = -1,
                LastDispatchedMicroseconds = -1
            };
        }

        internal ChannelPlaybackSnapshot WithStatisticsReset()
        {
            MidiChannelSnapshot[] copy = (MidiChannelSnapshot[])Channels.Clone();
            for (int i = 0; i < copy.Length; i++)
            {
                copy[i].SentEvents = 0;
                copy[i].DroppedEvents = 0;
                copy[i].PeakKeysDown = copy[i].KeysDown;
            }
            return new ChannelPlaybackSnapshot(copy);
        }
    }

    // Written only by the ordered scheduler worker while playback is active.
    // UI/reset requests are generations applied at the next bounded scheduler
    // publication, avoiding a lock or allocation on every MIDI event.
    internal sealed class ChannelStateTracker
    {
        private readonly MidiChannelSnapshot[] _channels = new MidiChannelSnapshot[16];
        private readonly int[] _keyOccurrences = new int[16 * 128];
        private int _requestedFullReset;
        private int _appliedFullReset;
        private int _requestedStatisticsReset;
        private int _appliedStatisticsReset;

        internal ChannelStateTracker()
        {
            ResetAllDirect();
        }

        internal void RequestFullReset()
        {
            Interlocked.Increment(ref _requestedFullReset);
        }

        internal void RequestStatisticsReset()
        {
            Interlocked.Increment(ref _requestedStatisticsReset);
        }

        internal void ResetAllDirect()
        {
            Array.Clear(_keyOccurrences, 0, _keyOccurrences.Length);
            for (int channel = 0; channel < 16; channel++)
                _channels[channel] = ChannelPlaybackSnapshot.Unknown(channel);
            _appliedFullReset = Volatile.Read(ref _requestedFullReset);
            _appliedStatisticsReset = Volatile.Read(ref _requestedStatisticsReset);
        }

        internal void PauseBoundaryDirect()
        {
            ApplyRequests();
            Array.Clear(_keyOccurrences, 0, _keyOccurrences.Length);
            for (int channel = 0; channel < 16; channel++)
            {
                _channels[channel].KeysDown = 0;
                _channels[channel].HistoricalAttributeMask |= KnownAttributeMask(_channels[channel]);
                if (_channels[channel].LastDispatchedMicroseconds >= 0)
                    _channels[channel].LastPositionHistorical = true;
            }
        }

        internal void SeekBoundaryDirect()
        {
            ApplyRequests();
            Array.Clear(_keyOccurrences, 0, _keyOccurrences.Length);
            for (int channel = 0; channel < 16; channel++)
            {
                _channels[channel].KeysDown = 0;
                _channels[channel].PeakKeysDown = 0;
                _channels[channel].SentEvents = 0;
                _channels[channel].HistoricalAttributeMask |= KnownAttributeMask(_channels[channel]);
                if (_channels[channel].LastDispatchedMicroseconds >= 0)
                    _channels[channel].LastPositionHistorical = true;
            }
        }

        internal void StopBoundaryDirect()
        {
            ResetAllDirect();
        }

        internal void PanicDirect()
        {
            ApplyRequests();
            Array.Clear(_keyOccurrences, 0, _keyOccurrences.Length);
            for (int channel = 0; channel < 16; channel++)
            {
                _channels[channel].KeysDown = 0;
                _channels[channel].Sustain = 0;
                _channels[channel].HistoricalAttributeMask &= ~(1 << (int)ChannelAttribute.Sustain);
            }
        }

        internal void SilenceChannelDirect(int channel)
        {
            ApplyRequests();
            if (channel < 0 || channel >= 16) return;
            MidiChannelSnapshot state = _channels[channel];
            ClearChannelKeys(channel, ref state);
            state.Sustain = 0;
            ClearHistorical(ref state, ChannelAttribute.Sustain);
            _channels[channel] = state;
        }

        internal void MarkAttributeHistoricalDirect(int channel, ChannelAttribute attribute)
        {
            ApplyRequests();
            if (channel < 0 || channel >= 16) return;
            MidiChannelSnapshot state = _channels[channel];
            if ((KnownAttributeMask(state) & (1 << (int)attribute)) != 0)
                state.HistoricalAttributeMask |= 1 << (int)attribute;
            _channels[channel] = state;
        }

        internal void RecordManualChaseApplied(int channel, ChannelAttribute attribute, int value)
        {
            RecordOverrideApplied(channel, attribute, value);
        }

        internal void RecordSuccessful(MidiEventView midiEvent)
        {
            ApplyRequests();
            int channel = midiEvent.Channel;
            if (channel < 0 || channel >= 16) return;
            MidiChannelSnapshot state = _channels[channel];
            state.SentEvents++;
            state.LastDispatchedMicroseconds = midiEvent.IntendedMicroseconds;
            state.LastPositionHistorical = false;
            int status = midiEvent.Status & 0xF0;
            int first = midiEvent.DataLength > 1 ? midiEvent.GetDataByte(1) : 0;
            int second = midiEvent.DataLength > 2 ? midiEvent.GetDataByte(2) : 0;
            if (status == 0x90 && second > 0) NoteOn(channel, first, ref state);
            else if (status == 0x80 || status == 0x90) NoteOff(channel, first, ref state);
            else if (status == 0xB0) ApplyController(channel, first, second, ref state);
            else if (status == 0xC0) { state.Program = first; ClearHistorical(ref state, ChannelAttribute.Program); }
            else if (status == 0xD0) { state.ChannelPressure = first; ClearHistorical(ref state, ChannelAttribute.Aftertouch); }
            else if (status == 0xE0) { state.PitchBend = ((second << 7) | first) - 8192; ClearHistorical(ref state, ChannelAttribute.PitchBend); }
            _channels[channel] = state;
        }

        internal void RecordOverrideApplied(int channel, ChannelAttribute attribute, int value)
        {
            ApplyRequests();
            if (channel < 0 || channel >= 16) return;
            MidiChannelSnapshot state = _channels[channel];
            switch (attribute)
            {
                case ChannelAttribute.BankMsb: state.BankMsb = value; break;
                case ChannelAttribute.BankLsb: state.BankLsb = value; break;
                case ChannelAttribute.Program: state.Program = value; break;
                case ChannelAttribute.Volume: state.Volume = value; break;
                case ChannelAttribute.Expression: state.Expression = value; break;
                case ChannelAttribute.Pan: state.Pan = value; break;
                case ChannelAttribute.Sustain: state.Sustain = value; break;
                case ChannelAttribute.PitchBend: state.PitchBend = value; break;
                case ChannelAttribute.Aftertouch: state.ChannelPressure = value; break;
            }
            ClearHistorical(ref state, attribute);
            _channels[channel] = state;
        }

        internal void RecordDropped(MidiEventView midiEvent)
        {
            ApplyRequests();
            int channel = midiEvent.Channel;
            if (channel >= 0 && channel < 16) _channels[channel].DroppedEvents++;
        }

        internal ChannelPlaybackSnapshot CreateSnapshot()
        {
            ApplyRequests();
            return new ChannelPlaybackSnapshot((MidiChannelSnapshot[])_channels.Clone());
        }

        private void ApplyRequests()
        {
            int full = Volatile.Read(ref _requestedFullReset);
            if (full != _appliedFullReset)
            {
                ResetAllDirect();
                _appliedFullReset = full;
            }
            int statistics = Volatile.Read(ref _requestedStatisticsReset);
            if (statistics != _appliedStatisticsReset)
            {
                for (int channel = 0; channel < 16; channel++)
                {
                    _channels[channel].SentEvents = 0;
                    _channels[channel].DroppedEvents = 0;
                    _channels[channel].PeakKeysDown = _channels[channel].KeysDown;
                }
                _appliedStatisticsReset = statistics;
            }
        }

        private void NoteOn(int channel, int key, ref MidiChannelSnapshot state)
        {
            if (key < 0 || key > 127) return;
            int index = channel * 128 + key;
            if (_keyOccurrences[index] == 0) state.KeysDown++;
            if (_keyOccurrences[index] != Int32.MaxValue) _keyOccurrences[index]++;
            if (state.KeysDown > state.PeakKeysDown) state.PeakKeysDown = state.KeysDown;
        }

        private void NoteOff(int channel, int key, ref MidiChannelSnapshot state)
        {
            if (key < 0 || key > 127) return;
            int index = channel * 128 + key;
            if (_keyOccurrences[index] == 0) return;
            _keyOccurrences[index]--;
            if (_keyOccurrences[index] == 0 && state.KeysDown > 0) state.KeysDown--;
        }

        private void ApplyController(int channel, int controller, int value, ref MidiChannelSnapshot state)
        {
            switch (controller)
            {
                case 0: state.BankMsb = value; ClearHistorical(ref state, ChannelAttribute.BankMsb); break;
                case 7: state.Volume = value; ClearHistorical(ref state, ChannelAttribute.Volume); break;
                case 10: state.Pan = value; ClearHistorical(ref state, ChannelAttribute.Pan); break;
                case 11: state.Expression = value; ClearHistorical(ref state, ChannelAttribute.Expression); break;
                case 32: state.BankLsb = value; ClearHistorical(ref state, ChannelAttribute.BankLsb); break;
                case 64: state.Sustain = value >= 64 ? 1 : 0; ClearHistorical(ref state, ChannelAttribute.Sustain); break;
                case 120:
                case 123:
                    ClearChannelKeys(channel, ref state);
                    break;
                case 121:
                    state.Expression = 127;
                    state.Pan = 64;
                    state.Sustain = 0;
                    state.PitchBend = 0;
                    state.ChannelPressure = 0;
                    state.HistoricalAttributeMask = 0;
                    break;
            }
        }

        private void ClearChannelKeys(int channel, ref MidiChannelSnapshot state)
        {
            Array.Clear(_keyOccurrences, channel * 128, 128);
            state.KeysDown = 0;
        }

        private static void ClearHistorical(ref MidiChannelSnapshot state, ChannelAttribute attribute)
        {
            state.HistoricalAttributeMask &= ~(1 << (int)attribute);
        }

        private static int KnownAttributeMask(MidiChannelSnapshot state)
        {
            int mask = 0;
            if (state.BankMsb >= 0) mask |= 1 << (int)ChannelAttribute.BankMsb;
            if (state.BankLsb >= 0) mask |= 1 << (int)ChannelAttribute.BankLsb;
            if (state.Program >= 0) mask |= 1 << (int)ChannelAttribute.Program;
            if (state.Volume >= 0) mask |= 1 << (int)ChannelAttribute.Volume;
            if (state.Expression >= 0) mask |= 1 << (int)ChannelAttribute.Expression;
            if (state.Pan >= 0) mask |= 1 << (int)ChannelAttribute.Pan;
            if (state.Sustain >= 0) mask |= 1 << (int)ChannelAttribute.Sustain;
            if (state.PitchBend != Int32.MinValue) mask |= 1 << (int)ChannelAttribute.PitchBend;
            if (state.ChannelPressure >= 0) mask |= 1 << (int)ChannelAttribute.Aftertouch;
            return mask;
        }
    }
}
