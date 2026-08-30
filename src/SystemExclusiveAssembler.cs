using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal sealed class SystemExclusiveAssembler
    {
        private List<byte> _pending;

        public byte[] Accept(MidiEvent midiEvent)
        {
            if (midiEvent == null) throw new ArgumentNullException("midiEvent");
            if (midiEvent.Kind != MidiEventKind.SystemExclusive)
                throw new ArgumentException("The event is not System Exclusive.", "midiEvent");

            if (midiEvent.Status == 0xF0)
            {
                _pending = new List<byte>(midiEvent.Data.Length + 32);
                _pending.AddRange(midiEvent.Data);
                return FinishIfComplete();
            }

            if (_pending != null)
            {
                _pending.AddRange(midiEvent.Data);
                return FinishIfComplete();
            }

            if (IsComplete(midiEvent.Data))
                return (byte[])midiEvent.Data.Clone();
            return null;
        }

        public void Reset()
        {
            _pending = null;
        }

        private byte[] FinishIfComplete()
        {
            if (_pending == null || _pending.Count < 2 || _pending[_pending.Count - 1] != 0xF7)
                return null;
            byte[] packet = _pending.ToArray();
            _pending = null;
            return IsComplete(packet) ? packet : null;
        }

        public static bool IsComplete(byte[] bytes)
        {
            return bytes != null && bytes.Length >= 2 && bytes[0] == 0xF0 && bytes[bytes.Length - 1] == 0xF7;
        }
    }
}
