using System;

namespace MidiBottleneck
{
    internal enum PerNoteGateAdmission
    {
        NotNote,
        Accepted,
        Filtered
    }

    // Deterministic fixed-size state for the live per-note interval gate.
    // Pitch is global across channels; an accepted Note On owns its pitch until
    // the matching-channel Note Off is emitted. No per-event collections or
    // hash tables are used.
    internal sealed class PerNoteIntervalGate
    {
        private readonly long _intervalMicroseconds;
        private readonly bool[] _active = new bool[128];
        private readonly int[] _owner = new int[128];
        private readonly MidiEvent[] _pendingOn = new MidiEvent[128];
        private readonly MidiEvent[] _pendingOff = new MidiEvent[128];
        private readonly long[] _onBoundary = new long[128];
        private readonly long[] _offBoundary = new long[128];
        private long _nextBoundary = Int64.MaxValue;

        internal PerNoteIntervalGate(long intervalMicroseconds)
        {
            if (intervalMicroseconds <= 0) throw new ArgumentOutOfRangeException("intervalMicroseconds");
            _intervalMicroseconds = intervalMicroseconds;
            for (int pitch = 0; pitch < _owner.Length; pitch++) _owner[pitch] = -1;
        }

        internal long IntervalMicroseconds { get { return _intervalMicroseconds; } }
        internal long NextBoundaryMicroseconds { get { return _nextBoundary; } }
        internal bool HasPendingTransitions { get { return _nextBoundary != Int64.MaxValue; } }

        internal PerNoteGateAdmission Admit(MidiEvent midiEvent)
        {
            int pitch;
            bool noteOn;
            if (!TryClassify(midiEvent, out pitch, out noteOn)) return PerNoteGateAdmission.NotNote;

            int channel = midiEvent.Channel;
            if (noteOn)
            {
                if (_active[pitch] || _pendingOn[pitch] != null)
                    return PerNoteGateAdmission.Filtered;
                _owner[pitch] = channel;
                _pendingOn[pitch] = midiEvent;
                _onBoundary[pitch] = BoundaryAfter(midiEvent.IntendedMicroseconds);
                IncludeBoundary(_onBoundary[pitch]);
                return PerNoteGateAdmission.Accepted;
            }

            if (_owner[pitch] != channel || (!_active[pitch] && _pendingOn[pitch] == null) ||
                _pendingOff[pitch] != null)
                return PerNoteGateAdmission.Filtered;

            _pendingOff[pitch] = midiEvent;
            long boundary = BoundaryAfter(midiEvent.IntendedMicroseconds);
            if (_pendingOn[pitch] != null)
                boundary = Math.Max(boundary, AddInterval(_onBoundary[pitch]));
            _offBoundary[pitch] = boundary;
            IncludeBoundary(boundary);
            return PerNoteGateAdmission.Accepted;
        }

        // Emits at most one transition per pitch, in ascending pitch order.
        // output is caller-owned and must contain at least 128 entries.
        internal int EmitBoundary(long boundaryMicroseconds, MidiEvent[] output)
        {
            if (output == null || output.Length < 128) throw new ArgumentException("A 128-entry output buffer is required.", "output");
            int count = 0;
            for (int pitch = 0; pitch < 128; pitch++)
            {
                MidiEvent noteOn = _pendingOn[pitch];
                if (noteOn != null && _onBoundary[pitch] <= boundaryMicroseconds)
                {
                    output[count++] = noteOn;
                    _pendingOn[pitch] = null;
                    _onBoundary[pitch] = 0;
                    _active[pitch] = true;
                    continue;
                }

                MidiEvent noteOff = _pendingOff[pitch];
                if (noteOff != null && _offBoundary[pitch] <= boundaryMicroseconds)
                {
                    output[count++] = noteOff;
                    _pendingOff[pitch] = null;
                    _offBoundary[pitch] = 0;
                    _active[pitch] = false;
                    _owner[pitch] = -1;
                }
            }
            RecalculateNextBoundary();
            return count;
        }

        // A live channel-disable control has already sent its safety messages.
        // Retire accepted-but-unsent transitions and forget active ownership so
        // the muted channel cannot later emit or suppress a pitch transition.
        internal int RetireChannel(int channel, MidiEvent[] retired)
        {
            if (channel < 0 || channel >= 16) return 0;
            int count = 0;
            for (int pitch = 0; pitch < 128; pitch++)
            {
                if (_owner[pitch] != channel) continue;
                if (_pendingOn[pitch] != null)
                {
                    if (retired != null && count < retired.Length) retired[count] = _pendingOn[pitch];
                    count++;
                }
                if (_pendingOff[pitch] != null)
                {
                    if (retired != null && count < retired.Length) retired[count] = _pendingOff[pitch];
                    count++;
                }
                _pendingOn[pitch] = null;
                _pendingOff[pitch] = null;
                _onBoundary[pitch] = 0;
                _offBoundary[pitch] = 0;
                _active[pitch] = false;
                _owner[pitch] = -1;
            }
            RecalculateNextBoundary();
            return count;
        }

        internal bool IsActive(int pitch) { return pitch >= 0 && pitch < 128 && _active[pitch]; }
        internal int OwnerChannel(int pitch) { return pitch < 0 || pitch >= 128 ? -1 : _owner[pitch]; }

        private long BoundaryAfter(long microseconds)
        {
            if (microseconds < 0) microseconds = 0;
            long quotient = microseconds / _intervalMicroseconds;
            if (quotient >= (Int64.MaxValue / _intervalMicroseconds) - 1) return Int64.MaxValue;
            return (quotient + 1) * _intervalMicroseconds;
        }

        private long AddInterval(long boundary)
        {
            return boundary > Int64.MaxValue - _intervalMicroseconds ? Int64.MaxValue : boundary + _intervalMicroseconds;
        }

        private void IncludeBoundary(long boundary)
        {
            if (boundary < _nextBoundary) _nextBoundary = boundary;
        }

        private void RecalculateNextBoundary()
        {
            long next = Int64.MaxValue;
            for (int pitch = 0; pitch < 128; pitch++)
            {
                if (_pendingOn[pitch] != null && _onBoundary[pitch] < next) next = _onBoundary[pitch];
                if (_pendingOff[pitch] != null && _offBoundary[pitch] < next) next = _offBoundary[pitch];
            }
            _nextBoundary = next;
        }

        internal static bool TryClassify(MidiEvent midiEvent, out int pitch, out bool noteOn)
        {
            pitch = -1;
            noteOn = false;
            if (midiEvent == null || midiEvent.Channel < 0 || midiEvent.Channel >= 16 || midiEvent.DataLength < 3)
                return false;
            int command = midiEvent.Status & 0xF0;
            if (command != 0x80 && command != 0x90) return false;
            pitch = midiEvent.GetDataByte(1) & 0x7F;
            noteOn = command == 0x90 && midiEvent.GetDataByte(2) != 0;
            return true;
        }
    }
}
