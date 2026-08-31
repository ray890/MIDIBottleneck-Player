using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace MidiBottleneck
{
    internal interface IKdmApiNative : IDisposable
    {
        bool IsAvailable();
        bool InitializeStream();
        bool TerminateStream();
        void ResetStream();
        void SendShort(uint message);
        uint PrepareLong(IntPtr header, uint headerSize);
        uint SendLong(IntPtr header, uint headerSize);
        uint UnprepareLong(IntPtr header, uint headerSize);
    }

    internal sealed class DynamicKdmApiNative : IKdmApiNative
    {
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private delegate bool BoolCall();
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate void VoidCall();
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate void ShortMessageCall(uint message);
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate uint LongMessageCall(IntPtr header, uint headerSize);

        private IntPtr _module;
        private IntPtr _bootstrapOutput;
        private readonly BoolCall _isAvailable;
        private readonly BoolCall _initialize;
        private readonly BoolCall _terminate;
        private readonly VoidCall _reset;
        private readonly ShortMessageCall _sendShort;
        private readonly LongMessageCall _prepareLong;
        private readonly LongMessageCall _sendLong;
        private readonly LongMessageCall _unprepareLong;

        public DynamicKdmApiNative()
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            MidiOutputDeviceInfo omniMidi = null;
            for (int i = 0; i < devices.Count; i++)
            {
                if (devices[i].Name.IndexOf("OmniMIDI", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    omniMidi = devices[i];
                    break;
                }
            }
            if (omniMidi == null)
                throw new InvalidOperationException("The OmniMIDI Windows output was not found. Install OmniMIDI, or turn off KDMAPI.");
            uint openResult = midiOutOpen(out _bootstrapOutput, omniMidi.DeviceId, IntPtr.Zero, IntPtr.Zero, 0);
            if (openResult != 0)
                throw new Win32Exception((int)openResult, "OmniMIDI could not be opened to initialize KDMAPI.");
            _module = GetModuleHandle("OmniMIDI");
            if (_module == IntPtr.Zero) _module = GetModuleHandle("OmniMIDI.dll");
            if (_module == IntPtr.Zero)
            {
                midiOutClose(_bootstrapOutput);
                _bootstrapOutput = IntPtr.Zero;
                throw new Win32Exception(Marshal.GetLastWin32Error(), "The OmniMIDI driver loaded without exposing its KDMAPI module.");
            }
            try
            {
                _isAvailable = Load<BoolCall>("IsKDMAPIAvailable");
                _initialize = Load<BoolCall>("InitializeKDMAPIStream");
                _terminate = Load<BoolCall>("TerminateKDMAPIStream");
                _reset = Load<VoidCall>("ResetKDMAPIStream");
                _sendShort = Load<ShortMessageCall>("SendDirectData");
                _prepareLong = Load<LongMessageCall>("PrepareLongData");
                _sendLong = Load<LongMessageCall>("SendDirectLongData");
                _unprepareLong = Load<LongMessageCall>("UnprepareLongData");
            }
            catch
            {
                Dispose();
                throw;
            }
        }

        private T Load<T>(string name) where T : class
        {
            IntPtr address = GetProcAddress(_module, name);
            if (address == IntPtr.Zero)
                throw new EntryPointNotFoundException("OmniMIDI does not export the required KDMAPI function " + name + ".");
            return (T)(object)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }

        public bool IsAvailable() { return _isAvailable(); }
        public bool InitializeStream() { return _initialize(); }
        public bool TerminateStream() { return _terminate(); }
        public void ResetStream() { _reset(); }
        public void SendShort(uint message) { _sendShort(message); }
        public uint PrepareLong(IntPtr header, uint headerSize) { return _prepareLong(header, headerSize); }
        public uint SendLong(IntPtr header, uint headerSize) { return _sendLong(header, headerSize); }
        public uint UnprepareLong(IntPtr header, uint headerSize) { return _unprepareLong(header, headerSize); }

        public void Dispose()
        {
            _module = IntPtr.Zero;
            if (_bootstrapOutput != IntPtr.Zero)
            {
                midiOutClose(_bootstrapOutput);
                _bootstrapOutput = IntPtr.Zero;
            }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string moduleName);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);
        [DllImport("winmm.dll")]
        private static extern uint midiOutOpen(out IntPtr handle, uint deviceId, IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll")]
        private static extern uint midiOutClose(IntPtr handle);
    }

    internal sealed class KdmApiMidiOutput : IMidiOutput, IDisposable
    {
        private const uint MhDone = 0x00000001;

        [StructLayout(LayoutKind.Sequential)]
        private struct MidiHeader
        {
            public IntPtr Data;
            public uint BufferLength;
            public uint BytesRecorded;
            public IntPtr User;
            public uint Flags;
            public IntPtr Next;
            public IntPtr Reserved;
            public uint Offset;
            public IntPtr Reserved0;
            public IntPtr Reserved1;
            public IntPtr Reserved2;
            public IntPtr Reserved3;
            public IntPtr Reserved4;
            public IntPtr Reserved5;
            public IntPtr Reserved6;
            public IntPtr Reserved7;
        }

        private sealed class LongBuffer
        {
            public IntPtr Data;
            public IntPtr Header;
            public bool Prepared;
        }

        private IKdmApiNative _native;
        private readonly bool _ownsNative;
        private bool _open;
        private readonly List<LongBuffer> _longBuffers = new List<LongBuffer>();
        private readonly SystemExclusiveAssembler _systemExclusiveAssembler = new SystemExclusiveAssembler();

        public KdmApiMidiOutput()
        {
            _ownsNative = true;
        }

        internal KdmApiMidiOutput(IKdmApiNative native)
        {
            if (native == null) throw new ArgumentNullException("native");
            _native = native;
            _ownsNative = false;
        }

        public void Open()
        {
            CloseStream();
            if (_native == null) _native = new DynamicKdmApiNative();
            try
            {
                if (!_native.IsAvailable())
                    throw new InvalidOperationException("OmniMIDI reported that KDMAPI is unavailable.");
                if (!_native.InitializeStream())
                    throw new InvalidOperationException("OmniMIDI could not initialize its KDMAPI stream.");
                _open = true;
            }
            catch
            {
                if (_ownsNative && _native != null)
                {
                    _native.Dispose();
                    _native = null;
                }
                throw;
            }
        }

        public void Send(MidiEvent midiEvent)
        {
            if (!_open) throw new InvalidOperationException("The KDMAPI output is not open.");
            if (midiEvent == null || midiEvent.Data == null || midiEvent.Data.Length == 0) return;
            ReclaimCompletedLongMessages();
            if (midiEvent.Kind == MidiEventKind.SystemExclusive)
            {
                byte[] packet = _systemExclusiveAssembler.Accept(midiEvent);
                if (packet != null) SendLongPacket(packet);
                return;
            }
            _native.SendShort(PackShortMessage(midiEvent.Data));
        }

        internal static uint PackShortMessage(byte[] data)
        {
            if (data == null || data.Length == 0) return 0;
            uint message = data[0];
            if (data.Length > 1) message |= (uint)data[1] << 8;
            if (data.Length > 2) message |= (uint)data[2] << 16;
            return message;
        }

        public void Panic()
        {
            if (!_open) return;
            for (int channel = 0; channel < 16; channel++)
            {
                uint status = (uint)(0xB0 | channel);
                _native.SendShort(status | ((uint)120 << 8));
                _native.SendShort(status | ((uint)123 << 8));
                _native.SendShort(status | ((uint)64 << 8));
            }
        }

        public void Reset()
        {
            _systemExclusiveAssembler.Reset();
            if (!_open) return;
            _native.ResetStream();
            ReclaimAllLongMessages();
        }

        private void SendLongPacket(byte[] bytes)
        {
            LongBuffer buffer = new LongBuffer();
            int headerSize = Marshal.SizeOf(typeof(MidiHeader));
            try
            {
                buffer.Data = Marshal.AllocHGlobal(bytes.Length);
                Marshal.Copy(bytes, 0, buffer.Data, bytes.Length);
                MidiHeader header = new MidiHeader();
                header.Data = buffer.Data;
                header.BufferLength = (uint)bytes.Length;
                header.BytesRecorded = 0;
                buffer.Header = Marshal.AllocHGlobal(headerSize);
                Marshal.StructureToPtr(header, buffer.Header, false);
                ThrowIfError(_native.PrepareLong(buffer.Header, (uint)headerSize), "preparing a KDMAPI System Exclusive message");
                buffer.Prepared = true;
                uint result = _native.SendLong(buffer.Header, (uint)headerSize);
                if (result != 0)
                {
                    _native.UnprepareLong(buffer.Header, (uint)headerSize);
                    buffer.Prepared = false;
                    ThrowIfError(result, "sending a KDMAPI System Exclusive message");
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
            int headerSize = Marshal.SizeOf(typeof(MidiHeader));
            for (int i = _longBuffers.Count - 1; i >= 0; i--)
            {
                MidiHeader header = (MidiHeader)Marshal.PtrToStructure(_longBuffers[i].Header, typeof(MidiHeader));
                if ((header.Flags & MhDone) == 0) continue;
                _native.UnprepareLong(_longBuffers[i].Header, (uint)headerSize);
                _longBuffers[i].Prepared = false;
                FreeLongBuffer(_longBuffers[i]);
                _longBuffers.RemoveAt(i);
            }
        }

        private void ReclaimAllLongMessages()
        {
            int headerSize = Marshal.SizeOf(typeof(MidiHeader));
            for (int i = _longBuffers.Count - 1; i >= 0; i--)
            {
                if (_longBuffers[i].Prepared)
                    _native.UnprepareLong(_longBuffers[i].Header, (uint)headerSize);
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
            if (code != 0) throw new Win32Exception((int)code, "MIDI error while " + action + ".");
        }

        private void CloseStream()
        {
            if (!_open) return;
            try
            {
                _native.ResetStream();
                ReclaimAllLongMessages();
            }
            finally
            {
                _native.TerminateStream();
                _systemExclusiveAssembler.Reset();
                _open = false;
            }
        }

        public void Dispose()
        {
            CloseStream();
            if (_ownsNative && _native != null)
            {
                _native.Dispose();
                _native = null;
            }
            GC.SuppressFinalize(this);
        }
    }
}
