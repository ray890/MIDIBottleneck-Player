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

        internal void ResetProviderStateDirect()
        {
            Array.Clear(_keyOccurrences, 0, _keyOccurrences.Length);
            for (int channel = 0; channel < 16; channel++)
            {
                long sent = _channels[channel].SentEvents;
                long dropped = _channels[channel].DroppedEvents;
                int peak = _channels[channel].PeakKeysDown;
                _channels[channel] = ChannelPlaybackSnapshot.Unknown(channel);
                _channels[channel].SentEvents = sent;
                _channels[channel].DroppedEvents = dropped;
                _channels[channel].PeakKeysDown = peak;
            }
        }

        internal void PanicDirect()
        {
            ApplyRequests();
            Array.Clear(_keyOccurrences, 0, _keyOccurrences.Length);
            for (int channel = 0; channel < 16; channel++)
            {
                _channels[channel].KeysDown = 0;
                _channels[channel].Sustain = 0;
            }
        }

        internal void RecordSuccessful(MidiEvent midiEvent)
        {
            ApplyRequests();
            int channel = midiEvent.Channel;
            if (channel < 0 || channel >= 16) return;
            MidiChannelSnapshot state = _channels[channel];
            state.SentEvents++;
            state.LastDispatchedMicroseconds = midiEvent.IntendedMicroseconds;
            int status = midiEvent.Status & 0xF0;
            int first = midiEvent.DataLength > 1 ? midiEvent.GetDataByte(1) : 0;
            int second = midiEvent.DataLength > 2 ? midiEvent.GetDataByte(2) : 0;
            if (status == 0x90 && second > 0) NoteOn(channel, first, ref state);
            else if (status == 0x80 || status == 0x90) NoteOff(channel, first, ref state);
            else if (status == 0xB0) ApplyController(channel, first, second, ref state);
            else if (status == 0xC0) state.Program = first;
            else if (status == 0xD0) state.ChannelPressure = first;
            else if (status == 0xE0) state.PitchBend = ((second << 7) | first) - 8192;
            _channels[channel] = state;
        }

        internal void RecordDropped(MidiEvent midiEvent)
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
                case 0: state.BankMsb = value; break;
                case 7: state.Volume = value; break;
                case 10: state.Pan = value; break;
                case 11: state.Expression = value; break;
                case 32: state.BankLsb = value; break;
                case 64: state.Sustain = value >= 64 ? 1 : 0; break;
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
                    break;
            }
        }

        private void ClearChannelKeys(int channel, ref MidiChannelSnapshot state)
        {
            Array.Clear(_keyOccurrences, channel * 128, 128);
            state.KeysDown = 0;
        }
    }
}
