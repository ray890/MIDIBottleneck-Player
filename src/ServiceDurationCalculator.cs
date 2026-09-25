using System;

namespace MidiBottleneck
{
    internal enum ServiceDurationMode
    {
        ProcessingTime,
        MidiBitrate,
        EventsPerSecond
    }

    // Published as one reference so a worker never combines a new mode with
    // values from an older UI edit. Replaced only when a setting changes.
    internal sealed class ServiceDurationSettings
    {
        internal readonly ServiceDurationMode Mode;
        internal readonly long ProcessingMicroseconds;
        internal readonly long MidiBitrate;
        internal readonly long EventsPerSecond;

        internal ServiceDurationSettings(ServiceDurationMode mode, long processingMicroseconds,
            long midiBitrate, long eventsPerSecond)
        {
            Mode = mode;
            ProcessingMicroseconds = processingMicroseconds;
            MidiBitrate = midiBitrate;
            EventsPerSecond = eventsPerSecond;
        }

        internal ServiceDurationClock CreateClock()
        { return new ServiceDurationClock(Mode, ProcessingMicroseconds, MidiBitrate, EventsPerSecond); }

        internal bool IsImmediate
        { get { return Mode == ServiceDurationMode.ProcessingTime && ProcessingMicroseconds == 0 ||
            Mode == ServiceDurationMode.EventsPerSecond && EventsPerSecond == 0; } }
    }

    // One playback/Analysis generation owns one clock. For a direct event
    // rate, the remainder carries the fractional microsecond between accepted
    // service starts: 3,000 events/sec becomes 333, 333, 334 us, not a
    // permanently rounded reciprocal. The value type is allocation-free and
    // can be copied when a projection needs a non-mutating look-ahead.
    internal struct ServiceDurationClock
    {
        private readonly ServiceDurationMode _mode;
        private readonly long _processingMicroseconds;
        private readonly long _midiBitrate;
        private readonly long _eventsPerSecond;
        private long _remainder;

        internal ServiceDurationClock(ServiceDurationMode mode, long processingMicroseconds,
            long midiBitrate, long eventsPerSecond)
        {
            _mode = mode;
            _processingMicroseconds = Math.Max(0, processingMicroseconds);
            _midiBitrate = Math.Max(1, midiBitrate);
            _eventsPerSecond = Math.Max(0, Math.Min(9999999, eventsPerSecond));
            _remainder = 0;
        }

        internal long NextMicroseconds(MidiEventView midiEvent)
        {
            if (_mode == ServiceDurationMode.EventsPerSecond)
            {
                if (_eventsPerSecond == 0) return 0;
                long numerator = _remainder + 1000000L;
                long duration = numerator / _eventsPerSecond;
                _remainder = numerator % _eventsPerSecond;
                return duration;
            }
            return ServiceDurationCalculator.CalculateMicroseconds(midiEvent, _mode,
                _processingMicroseconds, _midiBitrate);
        }

        internal bool IsImmediate
        {
            get
            {
                return _mode == ServiceDurationMode.ProcessingTime && _processingMicroseconds == 0 ||
                    _mode == ServiceDurationMode.EventsPerSecond && _eventsPerSecond == 0;
            }
        }

        internal long TotalForNextEvents(long count)
        {
            if (count <= 0) return 0;
            if (_mode == ServiceDurationMode.EventsPerSecond)
            {
                if (_eventsPerSecond == 0) return 0;
                return checked((_remainder + checked(count * 1000000L)) / _eventsPerSecond);
            }
            if (_mode == ServiceDurationMode.ProcessingTime)
                return checked(count * _processingMicroseconds);
            throw new InvalidOperationException("MIDI bitrate service depends on each message's byte count.");
        }
    }

    internal static class ServiceDurationCalculator
    {
        public const long FivePinDinBitrate = 31250;

        public static long CalculateMicroseconds(MidiEventView midiEvent, ServiceDurationMode mode, long processingMicroseconds, long midiBitrate)
        {
            if (mode == ServiceDurationMode.ProcessingTime)
                return Math.Max(0, processingMicroseconds);
            if (mode == ServiceDurationMode.EventsPerSecond)
                throw new InvalidOperationException("Events/sec requires a generation-owned ServiceDurationClock.");
            if (!midiEvent.IsValid) throw new ArgumentException("A valid MIDI event is required.", "midiEvent");
            int byteCount = midiEvent.DataLength;
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
