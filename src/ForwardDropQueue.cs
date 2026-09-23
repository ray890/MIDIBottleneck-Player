using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    // Deterministic finite service model used when playback should apply
    // forward-only admission decisions without delaying accepted output.
    // It stores bounded service durations, never MIDI payloads or event objects.
    internal sealed class ForwardDropQueue
    {
        private readonly int _capacity;
        private readonly Queue<long> _pendingService;
        private bool _busy;
        private long _completionMicroseconds;
        private int _maximumOccupancy;

        internal ForwardDropQueue(int capacity)
        {
            if (capacity < 1) throw new ArgumentOutOfRangeException("capacity");
            _capacity = capacity;
            _pendingService = new Queue<long>(Math.Min(capacity, 1000000));
        }

        internal int Occupancy { get { return _pendingService.Count + (_busy ? 1 : 0); } }
        internal int MaximumOccupancy { get { return _maximumOccupancy; } }

        internal void Advance(long sourceMicroseconds)
        {
            while (_busy && _completionMicroseconds <= sourceMicroseconds)
            {
                if (_pendingService.Count == 0)
                {
                    _busy = false;
                    break;
                }
                _completionMicroseconds = checked(_completionMicroseconds + _pendingService.Dequeue());
            }
        }

        internal bool TryAdmit(long sourceMicroseconds, long serviceMicroseconds, bool safetyAdmission)
        {
            Advance(sourceMicroseconds);
            if (Occupancy >= _capacity && !safetyAdmission) return false;
            serviceMicroseconds = Math.Max(0, serviceMicroseconds);
            if (!_busy)
            {
                _busy = true;
                _completionMicroseconds = checked(sourceMicroseconds + serviceMicroseconds);
            }
            else _pendingService.Enqueue(serviceMicroseconds);
            int occupancy = Occupancy;
            if (occupancy > _maximumOccupancy) _maximumOccupancy = occupancy;
            return true;
        }
    }
}
