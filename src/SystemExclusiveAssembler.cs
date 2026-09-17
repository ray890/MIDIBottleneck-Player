using System;
using System.Collections.Generic;

namespace MidiBottleneck
{
    internal sealed class SystemExclusiveFragment
    {
        public byte Status;
        public int DataLength;
        public int EventIndex;
        public long Tick;
        public long IntendedMicroseconds;
    }

    internal sealed class SystemExclusivePacket
    {
        public byte[] Bytes;
        public List<SystemExclusiveFragment> Fragments;
    }

    internal sealed class SystemExclusiveAssembler
    {
        private List<byte> _pending;
        private List<SystemExclusiveFragment> _pendingFragments;

        public byte[] Accept(MidiEvent midiEvent)
        {
            SystemExclusivePacket packet = AcceptPacket(midiEvent);
            return packet == null ? null : packet.Bytes;
        }

        public SystemExclusivePacket AcceptPacket(MidiEvent midiEvent)
        {
            if (midiEvent == null) throw new ArgumentNullException("midiEvent");
            if (midiEvent.Kind != MidiEventKind.SystemExclusive)
                throw new ArgumentException("The event is not System Exclusive.", "midiEvent");

            if (midiEvent.Status == 0xF0)
            {
                _pending = new List<byte>(midiEvent.Data.Length + 32);
                _pendingFragments = new List<SystemExclusiveFragment>();
                midiEvent.Data.AppendTo(_pending);
                _pendingFragments.Add(CreateFragment(midiEvent));
                return FinishIfComplete();
            }

            if (_pending != null)
            {
                midiEvent.Data.AppendTo(_pending);
                _pendingFragments.Add(CreateFragment(midiEvent));
                return FinishIfComplete();
            }

            if (IsComplete(midiEvent.Data))
            {
                SystemExclusivePacket direct = new SystemExclusivePacket();
                direct.Bytes = midiEvent.Data.ToArray();
                direct.Fragments = new List<SystemExclusiveFragment>();
                direct.Fragments.Add(CreateFragment(midiEvent));
                return direct;
            }
            return null;
        }

        public void Reset()
        {
            _pending = null;
            _pendingFragments = null;
        }

        private SystemExclusivePacket FinishIfComplete()
        {
            if (_pending == null || _pending.Count < 2 || _pending[_pending.Count - 1] != 0xF7)
                return null;
            byte[] bytes = _pending.ToArray();
            List<SystemExclusiveFragment> fragments = _pendingFragments;
            _pending = null;
            _pendingFragments = null;
            if (!IsComplete(bytes)) return null;
            SystemExclusivePacket packet = new SystemExclusivePacket();
            packet.Bytes = bytes;
            packet.Fragments = fragments;
            return packet;
        }

        private static SystemExclusiveFragment CreateFragment(MidiEvent midiEvent)
        {
            SystemExclusiveFragment fragment = new SystemExclusiveFragment();
            fragment.Status = midiEvent.Status;
            fragment.DataLength = midiEvent.Data.Length;
            fragment.EventIndex = midiEvent.EventIndex;
            fragment.Tick = midiEvent.AbsoluteTick;
            fragment.IntendedMicroseconds = midiEvent.IntendedMicroseconds;
            return fragment;
        }

        public static bool IsComplete(byte[] bytes)
        {
            return bytes != null && bytes.Length >= 2 && bytes[0] == 0xF0 && bytes[bytes.Length - 1] == 0xF7;
        }

        private static bool IsComplete(MidiEventData bytes)
        {
            return bytes.Length >= 2 && bytes[0] == 0xF0 && bytes[bytes.Length - 1] == 0xF7;
        }
    }
}
