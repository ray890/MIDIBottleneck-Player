using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
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
        string Version { get; }
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
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private delegate bool VersionCall(out uint major, out uint minor, out uint build, out uint revision);

        private IntPtr _module;
        private BoolCall _isAvailable;
        private BoolCall _initialize;
        private BoolCall _terminate;
        private VoidCall _reset;
        private ShortMessageCall _sendShort;
        private LongMessageCall _prepareLong;
        private LongMessageCall _sendLong;
        private LongMessageCall _unprepareLong;
        private VersionCall _returnVersion;
        private string _version;

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

            // KDMAPI is a direct API. Opening OmniMIDI through WinMM merely to
            // locate the DLL also starts its synth stream; the driver's own
            // InitializeKDMAPIStream then correctly refuses a second stream.
            // Own a module reference instead, so delegates remain valid without
            // retaining a conflicting WinMM output instance.
            string modulePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "OmniMIDI.dll");
            _module = LoadLibrary(modulePath);
            if (_module == IntPtr.Zero) _module = LoadLibrary("OmniMIDI.dll");
            if (_module == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "The OmniMIDI output was found, but its KDMAPI module could not be loaded.");
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
                _returnVersion = Load<VersionCall>("ReturnKDMAPIVer");
                uint major, minor, build, revision;
                if (!_returnVersion(out major, out minor, out build, out revision))
                    throw new InvalidOperationException("OmniMIDI loaded, but its KDMAPI version query was rejected.");
                _version = major + "." + minor + "." + build + "." + revision;
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
        public string Version { get { return _version; } }

        public void Dispose()
        {
            _version = null;
            _isAvailable = null;
            _initialize = null;
            _terminate = null;
            _reset = null;
            _sendShort = null;
            _prepareLong = null;
            _sendLong = null;
            _unprepareLong = null;
            _returnVersion = null;
            if (_module != IntPtr.Zero)
            {
                FreeLibrary(_module);
                _module = IntPtr.Zero;
            }
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibrary(string fileName);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);
    }

    internal sealed class KdmApiMidiOutput : IMidiOutput, IDisposable
    {
        private const uint MhDone = 0x00000001;

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
            : this(native, false)
        {
        }

        internal KdmApiMidiOutput(IKdmApiNative native, bool ownsNative)
        {
            if (native == null) throw new ArgumentNullException("native");
            _native = native;
            _ownsNative = ownsNative;
        }

        public void Open()
        {
            CloseStream(true);
            if (_native == null) _native = new DynamicKdmApiNative();
            try
            {
                if (!_native.IsAvailable())
                    throw new InvalidOperationException("OmniMIDI loaded, but reported that KDMAPI is unavailable.");
                if (!_native.InitializeStream())
                    throw new InvalidOperationException("OmniMIDI rejected KDMAPI stream initialization (KDMAPI " + _native.Version + ").");
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
                SystemExclusivePacket packet = _systemExclusiveAssembler.AcceptPacket(midiEvent);
                if (packet != null) SendLongPacket(packet, midiEvent);
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

        private void SendLongPacket(SystemExclusivePacket packet, MidiEvent finalEvent)
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
                ThrowLongIfError(_native.PrepareLong(buffer.Header, (uint)headerSize), "PrepareLongData", packet, finalEvent, headerSize, header);
                buffer.Prepared = true;
                uint result = _native.SendLong(buffer.Header, (uint)headerSize);
                if (result != 0)
                {
                    _native.UnprepareLong(buffer.Header, (uint)headerSize);
                    buffer.Prepared = false;
                    ThrowLongIfError(result, "SendDirectLongData", packet, finalEvent, headerSize, header);
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
                if ((header.Flags & MhDone) == 0) continue;
                _native.UnprepareLong(_longBuffers[i].Header, (uint)headerSize);
                _longBuffers[i].Prepared = false;
                FreeLongBuffer(_longBuffers[i]);
                _longBuffers.RemoveAt(i);
            }
        }

        private void ReclaimAllLongMessages()
        {
            int headerSize = Marshal.SizeOf(typeof(NativeMidiHeader));
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

        private static void ThrowLongIfError(uint code, string operation, SystemExclusivePacket packet,
            MidiEvent finalEvent, int headerSize, NativeMidiHeader header)
        {
            if (code == 0) return;
            string detail = SystemExclusiveDiagnostics.DescribeFailure(operation, code, null, packet, finalEvent,
                headerSize, header.BufferLength, header.BytesRecorded, header.Flags, header.Data);
            throw new Win32Exception((int)code, detail);
        }

        private void CloseStream(bool reportFailure)
        {
            if (!_open) return;
            Exception resetFailure = null;
            bool terminated = false;
            try
            {
                try
                {
                    _native.ResetStream();
                    ReclaimAllLongMessages();
                }
                catch (Exception ex) { resetFailure = ex; }
            }
            finally
            {
                try { terminated = _native.TerminateStream(); }
                catch (Exception ex) { if (resetFailure == null) resetFailure = ex; }
                _systemExclusiveAssembler.Reset();
                _open = false;
            }
            if (reportFailure && resetFailure != null)
                throw new InvalidOperationException("KDMAPI reset/cleanup failed before stream termination.", resetFailure);
            if (reportFailure && !terminated)
                throw new InvalidOperationException("OmniMIDI rejected KDMAPI stream termination.");
        }

        internal void Close()
        {
            try
            {
                CloseStream(true);
            }
            finally
            {
                // A rejected termination must not leave delegates rooted in a module that
                // the next output transition expects to load afresh.
                ReleaseOwnedNative();
            }
        }

        public void Dispose()
        {
            CloseStream(false);
            ReleaseOwnedNative();
            GC.SuppressFinalize(this);
        }

        private void ReleaseOwnedNative()
        {
            if (_ownsNative && _native != null)
            {
                _native.Dispose();
                _native = null;
            }
        }
    }
}
