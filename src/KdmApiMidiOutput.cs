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
        string ProviderPath { get; }
        bool SupportsLongMessages { get; }
        string LongMessageStatus { get; }
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
        private string _longMessageStatus;

        public DynamicKdmApiNative()
            : this(null)
        {
        }

        internal DynamicKdmApiNative(string explicitProviderPath)
        {
            // A deliberately colocated provider wins over the installed system
            // provider.  Once selected, failure is reported for that exact file;
            // silently falling through to a different synthesizer would make the
            // user's selection misleading.  No WinMM device is opened or used as
            // a brand/registration gate for this direct API.
            string modulePath = ResolveProviderPath(explicitProviderPath,
                AppDomain.CurrentDomain.BaseDirectory,
                Environment.GetFolderPath(Environment.SpecialFolder.System));
            ValidateProviderArchitecture(modulePath);
            _module = LoadLibrary(modulePath);
            if (_module == IntPtr.Zero)
            {
                string architecture = IntPtr.Size == 8 ? "x64" : "x86";
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "The selected " + architecture + " KDMAPI provider could not be loaded: " + modulePath);
            }
            ProviderPath = GetLoadedModulePath(_module, modulePath);
            try
            {
                _isAvailable = Load<BoolCall>("IsKDMAPIAvailable");
                _initialize = Load<BoolCall>("InitializeKDMAPIStream");
                _terminate = Load<BoolCall>("TerminateKDMAPIStream");
                _reset = Load<VoidCall>("ResetKDMAPIStream");
                _sendShort = Load<ShortMessageCall>("SendDirectData");
                _prepareLong = LoadOptional<LongMessageCall>("PrepareLongData");
                _sendLong = LoadOptional<LongMessageCall>("SendDirectLongData");
                _unprepareLong = LoadOptional<LongMessageCall>("UnprepareLongData");
                int longExportCount = (_prepareLong == null ? 0 : 1) + (_sendLong == null ? 0 : 1) +
                    (_unprepareLong == null ? 0 : 1);
                if (longExportCount == 3)
                    _longMessageStatus = "prepared long-message exports available";
                else
                {
                    List<string> missing = new List<string>();
                    if (_prepareLong == null) missing.Add("PrepareLongData");
                    if (_sendLong == null) missing.Add("SendDirectLongData");
                    if (_unprepareLong == null) missing.Add("UnprepareLongData");
                    _longMessageStatus = "prepared long-message contract incomplete; missing " + String.Join(", ", missing.ToArray());
                }
                _returnVersion = LoadOptional<VersionCall>("ReturnKDMAPIVer");
                if (_returnVersion == null)
                    _version = "not reported";
                else
                {
                    uint major, minor, build, revision;
                    if (!_returnVersion(out major, out minor, out build, out revision))
                        throw new InvalidOperationException("The KDMAPI provider rejected its version query: " + ProviderPath);
                    _version = major + "." + minor + "." + build + "." + revision;
                }
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
                throw new EntryPointNotFoundException("The KDMAPI provider does not export the required function " +
                    name + ": " + ProviderPath);
            return (T)(object)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }

        private T LoadOptional<T>(string name) where T : class
        {
            IntPtr address = GetProcAddress(_module, name);
            return address == IntPtr.Zero ? null : (T)(object)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }

        internal static string ResolveProviderPath(string explicitProviderPath, string applicationDirectory,
            string systemDirectory)
        {
            if (!String.IsNullOrWhiteSpace(explicitProviderPath))
            {
                string requested = Path.GetFullPath(explicitProviderPath);
                if (!File.Exists(requested))
                    throw new FileNotFoundException("The selected KDMAPI provider was not found.", requested);
                return requested;
            }

            string local = Path.Combine(applicationDirectory ?? String.Empty, "OmniMIDI.dll");
            if (File.Exists(local)) return Path.GetFullPath(local);
            string installed = Path.Combine(systemDirectory ?? String.Empty, "OmniMIDI.dll");
            if (File.Exists(installed)) return Path.GetFullPath(installed);
            throw new FileNotFoundException("No KDMAPI provider was found. Place a matching OmniMIDI.dll beside the application, install OmniMIDI, or turn off KDMAPI.");
        }

        internal static ushort ReadPeMachine(string path)
        {
            using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (BinaryReader reader = new BinaryReader(stream))
            {
                if (stream.Length < 64 || reader.ReadUInt16() != 0x5A4D)
                    throw new BadImageFormatException("The KDMAPI provider is not a valid Windows PE image: " + path);
                stream.Position = 0x3C;
                int peOffset = reader.ReadInt32();
                if (peOffset < 0 || (long)peOffset + 6 > stream.Length)
                    throw new BadImageFormatException("The KDMAPI provider has an invalid PE header: " + path);
                stream.Position = peOffset;
                if (reader.ReadUInt32() != 0x00004550)
                    throw new BadImageFormatException("The KDMAPI provider has an invalid PE signature: " + path);
                return reader.ReadUInt16();
            }
        }

        internal static void ValidateProviderArchitecture(string path)
        {
            ushort actual = ReadPeMachine(path);
            ushort expected = IntPtr.Size == 8 ? (ushort)0x8664 : (ushort)0x014C;
            if (actual != expected)
                throw new BadImageFormatException("The KDMAPI provider architecture does not match this " +
                    (IntPtr.Size == 8 ? "x64" : "x86") + " application: " + path +
                    " (PE machine 0x" + actual.ToString("X4") + ").");
        }

        private static string GetLoadedModulePath(IntPtr module, string fallback)
        {
            System.Text.StringBuilder path = new System.Text.StringBuilder(1024);
            uint length = GetModuleFileName(module, path, path.Capacity);
            return length == 0 ? fallback : path.ToString();
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
        public string ProviderPath { get; private set; }
        public bool SupportsLongMessages { get { return _prepareLong != null && _sendLong != null && _unprepareLong != null; } }
        public string LongMessageStatus { get { return _longMessageStatus; } }

        public void Dispose()
        {
            _version = null;
            _longMessageStatus = null;
            ProviderPath = null;
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
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetModuleFileName(IntPtr module, System.Text.StringBuilder fileName, int size);
    }

    internal sealed class KdmApiMidiOutput : IMidiOutput, IMidiOutputContext, IDisposable
    {
        private const uint MhDone = 0x00000001;
        private static readonly int HeaderSize = Marshal.SizeOf(typeof(NativeMidiHeader));

        private sealed class LongBuffer
        {
            public IntPtr Data;
            public IntPtr Header;
            public bool Prepared;
        }

        private IKdmApiNative _native;
        private readonly bool _ownsNative;
        private bool _open;
        public string SourceFile { get; set; }
        private readonly List<LongBuffer> _longBuffers = new List<LongBuffer>();
        private readonly SystemExclusiveAssembler _systemExclusiveAssembler = new SystemExclusiveAssembler();

        public KdmApiMidiOutput()
        {
            _ownsNative = true;
        }

        internal KdmApiMidiOutput(string providerPath)
        {
            _native = new DynamicKdmApiNative(providerPath);
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
                    throw new InvalidOperationException("The KDMAPI provider reported that direct output is unavailable: " + _native.ProviderPath);
                if (!_native.InitializeStream())
                    throw new InvalidOperationException("The KDMAPI provider rejected stream initialization (KDMAPI " +
                        _native.Version + "): " + _native.ProviderPath);
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

        public void Send(MidiEventView midiEvent)
        {
            if (!_open) throw new InvalidOperationException("The KDMAPI output is not open.");
            if (midiEvent.DataLength == 0) return;
            ReclaimCompletedLongMessages();
            if (midiEvent.Kind == MidiEventKind.SystemExclusive)
            {
                SystemExclusivePacket packet = _systemExclusiveAssembler.AcceptPacket(midiEvent);
                if (packet != null) SendLongPacket(packet, midiEvent);
                return;
            }
            _native.SendShort(midiEvent.PackedShortMessage);
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

        private void SendLongPacket(SystemExclusivePacket packet, MidiEventView finalEvent)
        {
            if (!_native.SupportsLongMessages)
                throw new NotSupportedException("The selected KDMAPI provider can send short MIDI messages but does not export " +
                    "PrepareLongData, SendDirectLongData, and UnprepareLongData. System Exclusive playback requires that prepared " +
                    "long-message contract (" + _native.LongMessageStatus + "). Provider: " + _native.ProviderPath);
            byte[] bytes = packet.Bytes;
            LongBuffer buffer = new LongBuffer();
            int headerSize = HeaderSize;
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
            if (_longBuffers.Count == 0) return;
            int headerSize = HeaderSize;
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
            if (_longBuffers.Count == 0) return;
            int headerSize = HeaderSize;
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

        private void ThrowLongIfError(uint code, string operation, SystemExclusivePacket packet,
            MidiEventView finalEvent, int headerSize, NativeMidiHeader header)
        {
            if (code == 0) return;
            string detail = SystemExclusiveDiagnostics.DescribeFailure(operation, code, null, SourceFile, packet, finalEvent,
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
                throw new InvalidOperationException("The KDMAPI provider rejected stream termination: " + _native.ProviderPath);
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

        internal string ProviderPath { get { return _native == null ? null : _native.ProviderPath; } }

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
