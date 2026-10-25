using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Exact burst-tolerant pacing with an exact rolling one-second backstop.
    // The virtual due instant is rational, so no per-event interval is
    // rounded. A bounded 50 ms tolerance admits ordinary MIDI batching when
    // total demand is below the limit, while the rolling ledger prevents any
    // borrowed capacity from exceeding the configured one-second maximum.
    // Missed capacity is discarded instead of becoming quiet-period credit.
    internal sealed class ProgressiveRatePacer
    {
        private struct StampCount
        {
            internal long Timestamp;
            internal long Count;
        }

        private readonly long _rate;
        private readonly long _unitsPerSecond;
        private readonly long _burstSlots;
        private readonly Queue<StampCount> _history = new Queue<StampCount>();
        private bool _hasTail;
        private StampCount _tail;
        private bool _initialized;
        private bool _exhausted;
        private long _nextWhole;
        private long _nextRemainder;
        private long _historyCount;
        private long _reservedCount;

        internal ProgressiveRatePacer(long rate, long unitsPerSecond)
            : this(rate, unitsPerSecond, 1)
        {
        }

        internal ProgressiveRatePacer(long rate, long unitsPerSecond, long minimumBurstSlots)
        {
            if (rate < 0 || rate > 9999999) throw new ArgumentOutOfRangeException("rate");
            if (unitsPerSecond <= 0) throw new ArgumentOutOfRangeException("unitsPerSecond");
            if (minimumBurstSlots <= 0) throw new ArgumentOutOfRangeException("minimumBurstSlots");
            _rate = rate;
            _unitsPerSecond = unitsPerSecond;
            _burstSlots = rate == 0 ? Int64.MaxValue : Math.Max(minimumBurstSlots, (rate + 19) / 20);
        }

        internal long Rate { get { return _rate; } }
        internal bool IsInitializedForTesting { get { return _initialized; } }
        internal long NextWholeForTesting { get { return _nextWhole; } }
        internal long NextRemainderForTesting { get { return _nextRemainder; } }

        internal long AvailableSlots(long timestamp, long maximum)
        {
            if (maximum <= 0) return 0;
            if (_rate == 0) return maximum;
            Prepare(timestamp);
            long used = _historyCount > Int64.MaxValue - _reservedCount
                ? Int64.MaxValue : _historyCount + _reservedCount;
            long rolling = Math.Max(0, _rate - used);
            maximum = Math.Min(maximum, rolling);
            long whole = _nextWhole;
            long remainder = _nextRemainder;
            bool exhausted = _exhausted;
            long count = 0;
            while (count < maximum && !exhausted && IsWithinBurst(whole, remainder, timestamp))
            {
                count++;
                Advance(ref whole, ref remainder, ref exhausted);
            }
            return count;
        }

        internal bool TryConsume(long timestamp)
        {
            if (_rate == 0) return true;
            Prepare(timestamp);
            if (_historyCount + _reservedCount >= _rate || _exhausted ||
                !IsWithinBurst(_nextWhole, _nextRemainder, timestamp)) return false;
            Advance(ref _nextWhole, ref _nextRemainder, ref _exhausted);
            Record(timestamp);
            return true;
        }

        internal bool TryReserve(long timestamp)
        {
            return TryReserve(timestamp, false);
        }

        internal bool TryReserveAhead(long timestamp)
        {
            return TryReserve(timestamp, true);
        }

        internal long RollingAvailableSlots(long timestamp, long maximum)
        {
            if (maximum <= 0) return 0;
            if (_rate == 0) return maximum;
            Prepare(timestamp);
            return Math.Min(maximum, Math.Max(0,
                _rate - _historyCount - _reservedCount));
        }

        private bool TryReserve(long timestamp, bool allowAhead)
        {
            if (_rate == 0) return true;
            Prepare(timestamp);
            if (_historyCount + _reservedCount >= _rate || _exhausted ||
                !allowAhead && !IsWithinBurst(_nextWhole, _nextRemainder, timestamp)) return false;
            Advance(ref _nextWhole, ref _nextRemainder, ref _exhausted);
            _reservedCount++;
            return true;
        }

        internal void CompleteReservation(long timestamp)
        {
            if (_rate == 0) return;
            if (_reservedCount <= 0)
                throw new InvalidOperationException("The rate pacer has no reservation to complete.");
            Prepare(timestamp);
            _reservedCount--;
            Record(timestamp);
        }

        internal void CancelReservation()
        {
            if (_rate == 0) return;
            if (_reservedCount <= 0)
                throw new InvalidOperationException("The rate pacer has no reservation to cancel.");
            _reservedCount--;
        }

        private void Prepare(long timestamp)
        {
            timestamp = Math.Max(0, timestamp);
            Expire(timestamp);
            if (!_initialized)
            {
                _initialized = true;
                _exhausted = false;
                _nextWhole = timestamp;
                _nextRemainder = 0;
                return;
            }

            // Once the clock advances beyond the next slot, discard every
            // missed slot. The fixed burst tolerance is available again, but
            // never grows with the length of the quiet period.
            if (!_exhausted && _nextWhole < timestamp)
            {
                _nextWhole = timestamp;
                _nextRemainder = 0;
            }
        }

        private bool IsWithinBurst(long whole, long remainder, long timestamp)
        {
            long slots = _burstSlots - 1;
            // An exact due instant anywhere inside the current representable
            // clock quantum belongs to this timestamp. The final +rate-1 is
            // the integer numerator form of a strict one-quantum boundary.
            long numerator = slots * _unitsPerSecond + (_rate - 1);
            long toleranceWhole = numerator / _rate;
            long toleranceRemainder = numerator % _rate;
            long allowedWhole = timestamp > Int64.MaxValue - toleranceWhole
                ? Int64.MaxValue : timestamp + toleranceWhole;
            if (whole < allowedWhole) return true;
            if (whole > allowedWhole) return false;
            if (allowedWhole == Int64.MaxValue && timestamp > Int64.MaxValue - toleranceWhole)
                return true;
            return remainder <= toleranceRemainder;
        }

        private void Expire(long timestamp)
        {
            long cutoff = timestamp >= _unitsPerSecond
                ? timestamp - _unitsPerSecond : Int64.MinValue;
            while (_history.Count != 0 && _history.Peek().Timestamp <= cutoff)
            {
                StampCount expired = _history.Dequeue();
                _historyCount -= expired.Count;
            }
            if (_hasTail && _tail.Timestamp <= cutoff)
            {
                _historyCount -= _tail.Count;
                _tail = default(StampCount);
                _hasTail = false;
            }
        }

        private void Record(long timestamp)
        {
            if (_hasTail && _tail.Timestamp > timestamp)
                throw new InvalidOperationException("Rate-pacer timestamps must be monotonic.");
            if (_hasTail && _tail.Timestamp == timestamp)
            {
                _tail.Count++;
                _historyCount++;
                return;
            }
            if (_hasTail) _history.Enqueue(_tail);
            _tail = new StampCount { Timestamp = timestamp, Count = 1 };
            _hasTail = true;
            _historyCount++;
        }

        private void Advance(ref long whole, ref long remainder, ref bool exhausted)
        {
            long wholeStep = _unitsPerSecond / _rate;
            long remainderStep = _unitsPerSecond % _rate;
            if (whole > Int64.MaxValue - wholeStep)
            {
                exhausted = true;
                return;
            }
            whole += wholeStep;
            remainder += remainderStep;
            if (remainder >= _rate)
            {
                remainder -= _rate;
                if (whole == Int64.MaxValue)
                {
                    exhausted = true;
                    return;
                }
                whole++;
            }
        }
    }
}
