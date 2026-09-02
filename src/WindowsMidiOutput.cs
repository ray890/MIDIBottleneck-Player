using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace MidiBottleneck
{
    internal sealed class MidiOutputDeviceInfo
    {
        public uint DeviceId;
        public string Name;
        public override string ToString() { return Name; }
    }

    internal sealed class WindowsMidiOutput : IMidiOutput, IDisposable
    {
        private const uint MhDone = 0x00000001;
        private const int MaxPnameLen = 32;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct MidiOutCaps
        {
            public ushort ManufacturerId;
            public ushort ProductId;
            public uint DriverVersion;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = MaxPnameLen)]
            public string Name;
            public ushort Technology;
            public ushort Voices;
            public ushort Notes;
            public ushort ChannelMask;
            public uint Support;
        }

        private sealed class LongBuffer
        {
            public IntPtr Data;
            public IntPtr Header;
        }

        private IntPtr _handle;
        private readonly List<LongBuffer> _longBuffers = new List<LongBuffer>();
        private readonly SystemExclusiveAssembler _systemExclusiveAssembler = new SystemExclusiveAssembler();

        public static List<MidiOutputDeviceInfo> GetDevices()
        {
            uint count = midiOutGetNumDevs();
            List<MidiOutputDeviceInfo> devices = new List<MidiOutputDeviceInfo>();
            for (uint i = 0; i < count; i++)
            {
                MidiOutCaps caps;
                uint result = midiOutGetDevCaps(new UIntPtr(i), out caps, (uint)Marshal.SizeOf(typeof(MidiOutCaps)));
                if (result == 0)
                {
                    MidiOutputDeviceInfo info = new MidiOutputDeviceInfo();
                    info.DeviceId = i;
                    info.Name = String.IsNullOrEmpty(caps.Name) ? "MIDI output " + i : caps.Name;
                    devices.Add(info);
                }
            }
            return devices;
        }

        public void Open(uint deviceId)
        {
            DisposeHandle();
            uint result = midiOutOpen(out _handle, deviceId, IntPtr.Zero, IntPtr.Zero, 0);
            ThrowIfError(result, "opening the MIDI output");
        }

        public void Send(MidiEvent midiEvent)
        {
            if (_handle == IntPtr.Zero)
                throw new InvalidOperationException("No MIDI output device is open.");
            if (midiEvent == null || midiEvent.Data == null || midiEvent.Data.Length == 0)
                return;

            ReclaimCompletedLongMessages();
            if (midiEvent.Kind == MidiEventKind.SystemExclusive)
            {
                HandleSystemExclusive(midiEvent);
                return;
            }

            uint message = midiEvent.Data[0];
            if (midiEvent.Data.Length > 1) message |= (uint)midiEvent.Data[1] << 8;
            if (midiEvent.Data.Length > 2) message |= (uint)midiEvent.Data[2] << 16;
            ThrowIfError(midiOutShortMsg(_handle, message), "sending a MIDI message");
        }

        public void Panic()
        {
            if (_handle == IntPtr.Zero) return;
            for (int channel = 0; channel < 16; channel++)
            {
                uint status = (uint)(0xB0 | channel);
                midiOutShortMsg(_handle, status | ((uint)120 << 8));
                midiOutShortMsg(_handle, status | ((uint)123 << 8));
                midiOutShortMsg(_handle, status | ((uint)64 << 8));
            }
        }

        public void Reset()
        {
            _systemExclusiveAssembler.Reset();
            if (_handle == IntPtr.Zero) return;
            midiOutReset(_handle);
            ReclaimAllLongMessages();
        }

        private void HandleSystemExclusive(MidiEvent midiEvent)
        {
            // In an SMF, F0 starts a SysEx packet while an F7 event either
            // continues that packet or contains escaped bytes. winmm drivers are
            // allowed to reject an unframed continuation, so assemble fragments
            // and submit only a complete F0...F7 packet.
            SystemExclusivePacket packet = _systemExclusiveAssembler.AcceptPacket(midiEvent);
            if (packet != null)
                SendLong(packet, midiEvent);
        }

        private void SendLong(SystemExclusivePacket packet, MidiEvent finalEvent)
        {
            byte[] bytes = packet.Bytes;
            LongBuffer buffer = new LongBuffer();
            int headerSize = Marshal.SizeOf(typeof(NativeMidiHeader));
            try
            {
                buffer.Data = Marshal.AllocHGlobal(bytes.Length);
                Marshal.Copy(bytes, 0, buffer.Data, bytes.Length);
                NativeMidiHeader header = NativeMidiHeader.CreateOutput(buffer.Data, bytes.Length);
                buffer.Header = Marshal.AllocHGlobal(headerSize);
                Marshal.StructureToPtr(header, buffer.Header, false);
                uint result = midiOutPrepareHeader(_handle, buffer.Header, (uint)headerSize);
                ThrowLongIfError(result, "midiOutPrepareHeader", packet, finalEvent, headerSize, header);
                result = midiOutLongMsg(_handle, buffer.Header, (uint)headerSize);
                if (result != 0)
                {
                    midiOutUnprepareHeader(_handle, buffer.Header, (uint)headerSize);
                    ThrowLongIfError(result, "midiOutLongMsg", packet, finalEvent, headerSize, header);
                }
                _longBuffers.Add(buffer);
            }
            catch
            {
                FreeLongBuffer(buffer);
                throw;
            }
        }

        private void ReclaimCompletedLongMessages()
        {
            int headerSize = Marshal.SizeOf(typeof(NativeMidiHeader));
            for (int i = _longBuffers.Count - 1; i >= 0; i--)
            {
                NativeMidiHeader header = (NativeMidiHeader)Marshal.PtrToStructure(_longBuffers[i].Header, typeof(NativeMidiHeader));
                if ((header.Flags & MhDone) != 0)
                {
                    midiOutUnprepareHeader(_handle, _longBuffers[i].Header, (uint)headerSize);
                    FreeLongBuffer(_longBuffers[i]);
                    _longBuffers.RemoveAt(i);
                }
            }
        }

        private void ReclaimAllLongMessages()
        {
            int headerSize = Marshal.SizeOf(typeof(NativeMidiHeader));
            for (int i = _longBuffers.Count - 1; i >= 0; i--)
            {
                midiOutUnprepareHeader(_handle, _longBuffers[i].Header, (uint)headerSize);
                FreeLongBuffer(_longBuffers[i]);
            }
            _longBuffers.Clear();
        }

        private static void FreeLongBuffer(LongBuffer buffer)
        {
            if (buffer.Header != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(buffer.Header);
                buffer.Header = IntPtr.Zero;
            }
            if (buffer.Data != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(buffer.Data);
                buffer.Data = IntPtr.Zero;
            }
        }

        private static void ThrowIfError(uint code, string action)
        {
            if (code == 0) return;
            System.Text.StringBuilder text = new System.Text.StringBuilder(256);
            midiOutGetErrorText(code, text, text.Capacity);
            throw new Win32Exception((int)code, "MIDI error while " + action + ": " + text);
        }

        private static void ThrowLongIfError(uint code, string operation, SystemExclusivePacket packet,
            MidiEvent finalEvent, int headerSize, NativeMidiHeader header)
        {
            if (code == 0) return;
            System.Text.StringBuilder nativeText = new System.Text.StringBuilder(256);
            midiOutGetErrorText(code, nativeText, nativeText.Capacity);
            string detail = SystemExclusiveDiagnostics.DescribeFailure(operation, code, nativeText.ToString(), packet,
                finalEvent, headerSize, header.BufferLength, header.BytesRecorded, header.Flags, header.Data);
            throw new Win32Exception((int)code, detail);
        }

        public void Dispose()
        {
            DisposeHandle();
            GC.SuppressFinalize(this);
        }

        private void DisposeHandle()
        {
            if (_handle == IntPtr.Zero) return;
            midiOutReset(_handle);
            ReclaimAllLongMessages();
            _systemExclusiveAssembler.Reset();
            midiOutClose(_handle);
            _handle = IntPtr.Zero;
        }

        [DllImport("winmm.dll")]
        private static extern uint midiOutGetNumDevs();
        [DllImport("winmm.dll", CharSet = CharSet.Auto)]
        private static extern uint midiOutGetDevCaps(UIntPtr deviceId, out MidiOutCaps caps, uint capsSize);
        [DllImport("winmm.dll")]
        private static extern uint midiOutOpen(out IntPtr handle, uint deviceId, IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll")]
        private static extern uint midiOutClose(IntPtr handle);
        [DllImport("winmm.dll")]
        private static extern uint midiOutReset(IntPtr handle);
        [DllImport("winmm.dll")]
        private static extern uint midiOutShortMsg(IntPtr handle, uint message);
        [DllImport("winmm.dll")]
        private static extern uint midiOutPrepareHeader(IntPtr handle, IntPtr header, uint headerSize);
        [DllImport("winmm.dll")]
        private static extern uint midiOutUnprepareHeader(IntPtr handle, IntPtr header, uint headerSize);
        [DllImport("winmm.dll")]
        private static extern uint midiOutLongMsg(IntPtr handle, IntPtr header, uint headerSize);
        [DllImport("winmm.dll", CharSet = CharSet.Auto)]
        private static extern uint midiOutGetErrorText(uint error, System.Text.StringBuilder text, int textLength);
    }
}
