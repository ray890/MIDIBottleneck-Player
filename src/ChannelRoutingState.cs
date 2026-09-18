using System;
using System.Threading;

namespace MidiBottleneck
{
    // Fixed-size, allocation-free source-channel output filtering. Queue and
    // service accounting happen before this boundary.
    internal sealed class ChannelRoutingState
    {
        private readonly long[] _mutedFiltered = new long[16];
        private int _disabledMask;

        internal bool IsEnabled(int channel)
        {
            ValidateChannel(channel);
            return (Volatile.Read(ref _disabledMask) & (1 << channel)) == 0;
        }

        internal bool SetEnabled(int channel, bool enabled)
        {
            ValidateChannel(channel);
            while (true)
            {
                int previous = Volatile.Read(ref _disabledMask);
                int next = enabled ? previous & ~(1 << channel) : previous | (1 << channel);
                if (next == previous) return false;
                if (Interlocked.CompareExchange(ref _disabledMask, next, previous) == previous) return true;
            }
        }

        internal bool ShouldFilter(MidiEvent midiEvent)
        {
            int channel = midiEvent == null ? -1 : midiEvent.Channel;
            return channel >= 0 && channel < 16 && (Volatile.Read(ref _disabledMask) & (1 << channel)) != 0;
        }

        internal void RecordFiltered(int channel)
        {
            if (channel >= 0 && channel < 16) Interlocked.Increment(ref _mutedFiltered[channel]);
        }

        internal void ApplyToSnapshot(ChannelPlaybackSnapshot snapshot)
        {
            if (snapshot == null || snapshot.Channels == null) return;
            int disabled = Volatile.Read(ref _disabledMask);
            for (int channel = 0; channel < Math.Min(16, snapshot.Channels.Length); channel++)
            {
                MidiChannelSnapshot state = snapshot.Channels[channel];
                state.Enabled = (disabled & (1 << channel)) == 0;
                state.MutedFilteredEvents = Interlocked.Read(ref _mutedFiltered[channel]);
                snapshot.Channels[channel] = state;
            }
        }

        internal void ResetStatistics()
        {
            for (int channel = 0; channel < 16; channel++) Interlocked.Exchange(ref _mutedFiltered[channel], 0);
        }

        internal void Clear()
        {
            Volatile.Write(ref _disabledMask, 0);
            ResetStatistics();
        }

        private static void ValidateChannel(int channel)
        {
            if (channel < 0 || channel >= 16) throw new ArgumentOutOfRangeException("channel");
        }
    }
}
