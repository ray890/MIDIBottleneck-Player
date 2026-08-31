using System;

namespace MidiBottleneck
{
    internal enum ServiceDurationMode
    {
        ProcessingTime,
        MidiBitrate
    }

    internal static class ServiceDurationCalculator
    {
        public const long FivePinDinBitrate = 31250;

        public static long CalculateMicroseconds(MidiEvent midiEvent, ServiceDurationMode mode, long processingMicroseconds, long midiBitrate)
        {
            if (mode == ServiceDurationMode.ProcessingTime)
                return Math.Max(0, processingMicroseconds);
            if (midiEvent == null) throw new ArgumentNullException("midiEvent");
            int byteCount = midiEvent.Data == null ? 0 : midiEvent.Data.Length;
            return CalculateBitrateMicroseconds(byteCount, midiBitrate);
        }

        public static long CalculateBitrateMicroseconds(int midiByteCount, long bitrate)
        {
            if (midiByteCount < 0) throw new ArgumentOutOfRangeException("midiByteCount");
            if (bitrate <= 0) throw new ArgumentOutOfRangeException("bitrate");
            if (midiByteCount == 0) return 0;
            // MIDI serial framing uses one start bit, eight data bits, and one
            // stop bit: ten transmitted bits per MIDI byte. Round upward so a
            // non-integral microsecond duration never understates wire time.
            long numerator = checked((long)midiByteCount * 10L * 1000000L);
            return checked((numerator + bitrate - 1) / bitrate);
        }
    }
}
