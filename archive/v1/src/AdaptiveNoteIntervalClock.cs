using System;

namespace MidiBottleneck
{
    // Shared by live Playback and static Analysis. A logical note boundary
    // may require several gate-resolution passes (for example an R=0
    // Delayed NoteOff Off/candidate/On sequence), so the clock accumulates
    // every transmitted note message until both source admission and the
    // gate have advanced beyond that boundary.
    internal sealed class AdaptiveNoteIntervalClock
    {
        private readonly bool _enabled;
        private readonly long _intervalMicroseconds;
        private long _nextAllowedBoundary;
        private long _openBoundary = Int64.MaxValue;
        private int _openOutputCount;

        internal AdaptiveNoteIntervalClock(long intervalMicroseconds, bool enabled)
        {
            if (intervalMicroseconds < 0)
                throw new ArgumentOutOfRangeException("intervalMicroseconds");
            _intervalMicroseconds = intervalMicroseconds;
            _enabled = enabled;
        }

        internal bool Enabled { get { return _enabled; } }
        internal long NextAllowedBoundaryMicroseconds { get { return _nextAllowedBoundary; } }
        internal long OpenBoundaryMicroseconds { get { return _openBoundary; } }

        internal long EffectiveBoundary(long localBoundaryMicroseconds)
        {
            if (!_enabled || localBoundaryMicroseconds == Int64.MaxValue)
                return localBoundaryMicroseconds;
            if (_openBoundary != Int64.MaxValue && localBoundaryMicroseconds <= _openBoundary)
                return _openBoundary;
            return Math.Max(localBoundaryMicroseconds, _nextAllowedBoundary);
        }

        internal void RecordBoundaryPass(long boundaryMicroseconds, int emittedNoteMessages)
        {
            if (!_enabled) return;
            if (_openBoundary == Int64.MaxValue)
                _openBoundary = boundaryMicroseconds;
            else if (_openBoundary != boundaryMicroseconds)
                throw new InvalidOperationException("An adaptive note boundary was advanced before its batch closed.");
            if (emittedNoteMessages <= 0) return;
            _openOutputCount = _openOutputCount > Int32.MaxValue - emittedNoteMessages
                ? Int32.MaxValue : _openOutputCount + emittedNoteMessages;
        }

        internal bool CompleteIfClosed(long nextLocalBoundaryMicroseconds,
            long nextSourceMicroseconds)
        {
            if (!_enabled || _openBoundary == Int64.MaxValue ||
                nextLocalBoundaryMicroseconds <= _openBoundary ||
                nextSourceMicroseconds <= _openBoundary) return false;
            long multiplier = Math.Max(1L, _openOutputCount);
            long delay = SaturatingMultiply(_intervalMicroseconds, multiplier);
            _nextAllowedBoundary = SaturatingAdd(_openBoundary, delay);
            _openBoundary = Int64.MaxValue;
            _openOutputCount = 0;
            return true;
        }

        internal void Clear()
        {
            _nextAllowedBoundary = 0;
            _openBoundary = Int64.MaxValue;
            _openOutputCount = 0;
        }

        internal static long SaturatingMultiply(long value, long multiplier)
        {
            if (value <= 0 || multiplier <= 0) return 0;
            return value > Int64.MaxValue / multiplier
                ? Int64.MaxValue : value * multiplier;
        }

        internal static long SaturatingAdd(long value, long increment)
        {
            if (increment <= 0) return value;
            return value > Int64.MaxValue - increment
                ? Int64.MaxValue : value + increment;
        }
    }
}
