using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class MidiOutputDeviceInfo
    {
        public uint DeviceId;
        public string Name;
        public override string ToString() { return Name; }
    }

    internal sealed class WindowsMidiOutput : IMidiOutput, IMidiOutputContext, IDisposable
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
        private uint _deviceId;
        private string _deviceName;
        private bool _preparedLongMessagesUnsafe;
        private bool _usesCumulativeByteCountResults;
        private readonly Func<IntPtr, uint, uint> _sendShort;
        private readonly bool _testBoundary;
        private string _outputIdentity;
        private static readonly int HeaderSize = Marshal.SizeOf(typeof(NativeMidiHeader));
        private readonly List<LongBuffer> _longBuffers = new List<LongBuffer>();
        private readonly SystemExclusiveAssembler _systemExclusiveAssembler = new SystemExclusiveAssembler();
        public string SourceFile { get; set; }

        public WindowsMidiOutput() { _sendShort = midiOutShortMsg; }

        // Deterministic boundary injection exercises the production adapter's
        // packing, reclamation and result handling without opening hardware.
        internal WindowsMidiOutput(Func<IntPtr, uint, uint> sendShort)
        {
            _sendShort = sendShort;
            _testBoundary = true;
            _handle = new IntPtr(1);
            _deviceName = "Deterministic WinMM boundary";
            _outputIdentity = OutputIdentity();
        }

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
            string deviceName = GetDeviceName(deviceId);
            uint result = midiOutOpen(out _handle, deviceId, IntPtr.Zero, IntPtr.Zero, 0);
            ThrowIfError(result, "opening MIDI output " + deviceName + " (device " + deviceId + ")");
            _deviceId = deviceId;
            _deviceName = deviceName;
            _outputIdentity = OutputIdentity();
            try { EnsureLocalProviderReady(); }
            catch
            {
                DisposeHandle();
                throw;
            }
        }

        private void EnsureLocalProviderReady()
        {
            _preparedLongMessagesUnsafe = false;
            _usesCumulativeByteCountResults = false;
            string modulePath = GetLoadedModulePath();
            string moduleDirectory = null;
            try { moduleDirectory = Path.GetDirectoryName(Path.GetFullPath(modulePath)); }
            catch { }
            if (!String.Equals(moduleDirectory, Path.GetFullPath(AppDomain.CurrentDomain.BaseDirectory).TrimEnd(Path.DirectorySeparatorChar),
                StringComparison.OrdinalIgnoreCase)) return;

            string description = null;
            try { description = FileVersionInfo.GetVersionInfo(modulePath).FileDescription; }
            catch { }
            _preparedLongMessagesUnsafe = IsPreparedLongMessageUnsafeDescription(description);

            // Some application-local wrappers return from midiOutOpen before
            // their synth worker is ready.  Require two consecutive standard
            // MMSYSERR_NOERROR results from harmless sustain-off probes.  No
            // nonzero result is ever accepted for real song data.
            const uint sustainOffChannelOne = 0x0040B0;
            List<uint> observed = new List<uint>();
            Stopwatch timer = Stopwatch.StartNew();
            int consecutiveSuccesses = 0;
            uint last = 0;
            do
            {
                last = midiOutShortMsg(_handle, sustainOffChannelOne);
                if (observed.Count < 16) observed.Add(last);
                if (observed.Count >= 4 && IsCumulativeByteCountContract(observed))
                {
                    _usesCumulativeByteCountResults = true;
                    return;
                }
                if (last == 0)
                {
                    consecutiveSuccesses++;
                    if (consecutiveSuccesses >= 2) return;
                }
                else consecutiveSuccesses = 0;
                Thread.Sleep(10);
            }
            while (timer.ElapsedMilliseconds < 5000);
            ThrowIfError(last, "waiting for two accepted initialization probes from " + OutputIdentity() +
                "; first returns " + String.Join(", ", observed.ConvertAll(delegate(uint value) { return value.ToString(); }).ToArray()));
        }

        internal static bool IsPreparedLongMessageUnsafeDescription(string description)
        {
            return String.Equals(description, "WinMM to KDMAPI or syndrv", StringComparison.OrdinalIgnoreCase);
        }

        internal static bool IsCumulativeByteCountContract(IList<uint> results)
        {
            if (results == null || results.Count < 4) return false;
            return results[0] == 0 && results[1] == 3 && results[2] == 6 && results[3] == 9;
        }

        public void Send(MidiEvent midiEvent)
        {
            if (_handle == IntPtr.Zero)
                throw new InvalidOperationException("No MIDI output device is open.");
            if (midiEvent == null || midiEvent.DataLength == 0)
                return;

            ReclaimCompletedLongMessages();
            if (midiEvent.Kind == MidiEventKind.SystemExclusive)
            {
                HandleSystemExclusive(midiEvent);
                return;
            }

            uint message = midiEvent.PackedShortMessage;
            uint result = _sendShort(_handle, message);
            if (result != 0 && !_usesCumulativeByteCountResults)
                ThrowIfError(result, "sending a short MIDI message through " + _outputIdentity);
        }

        public void Panic()
        {
            if (_handle == IntPtr.Zero) return;
            for (int channel = 0; channel < 16; channel++)
            {
                uint status = (uint)(0xB0 | channel);
                SendPanicMessage(status | ((uint)120 << 8), "CC120");
                SendPanicMessage(status | ((uint)123 << 8), "CC123");
                SendPanicMessage(status | ((uint)64 << 8), "sustain-off");
            }
        }

        private void SendPanicMessage(uint message, string name)
        {
            uint result = _sendShort(_handle, message);
            if (result != 0 && !_usesCumulativeByteCountResults)
                ThrowIfError(result, "sending " + name + " panic through " + _outputIdentity);
        }

        private string OutputIdentity()
        {
            return (_deviceName ?? "Windows MIDI output") + " (device " + _deviceId + ", handle 0x" +
                _handle.ToInt64().ToString("X") + ", module " + GetLoadedModulePath() + ")";
        }

        private static string GetDeviceName(uint deviceId)
        {
            List<MidiOutputDeviceInfo> devices = GetDevices();
            for (int i = 0; i < devices.Count; i++)
                if (devices[i].DeviceId == deviceId) return devices[i].Name;
            return "MIDI output " + deviceId;
        }

        internal static string GetLoadedModulePath()
        {
            IntPtr module = GetModuleHandle("winmm.dll");
            if (module == IntPtr.Zero) return "not loaded";
            System.Text.StringBuilder path = new System.Text.StringBuilder(1024);
            uint length = GetModuleFileName(module, path, path.Capacity);
            return length == 0 ? "unknown" : path.ToString();
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
            if (_preparedLongMessagesUnsafe)
                throw new NotSupportedException("The selected application-local WinMM provider does not expose a safely verifiable " +
                    "standard prepared-long-message path. System Exclusive playback was stopped instead of risking the native " +
                    "midiOutLongMsg failure reproduced with this provider. Module: " +
                    GetLoadedModulePath());
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
            if (_longBuffers.Count == 0) return;
            int headerSize = HeaderSize;
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
            throw new Win32Exception((int)code, "MIDI error while " + action + " (native code " + code + "): " + text);
        }

        private void ThrowLongIfError(uint code, string operation, SystemExclusivePacket packet,
            MidiEvent finalEvent, int headerSize, NativeMidiHeader header)
        {
            if (code == 0) return;
            System.Text.StringBuilder nativeText = new System.Text.StringBuilder(256);
            midiOutGetErrorText(code, nativeText, nativeText.Capacity);
            string detail = SystemExclusiveDiagnostics.DescribeFailure(operation, code, nativeText.ToString(), SourceFile, packet,
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
            if (_testBoundary) { _handle = IntPtr.Zero; return; }
            midiOutReset(_handle);
            ReclaimAllLongMessages();
            _systemExclusiveAssembler.Reset();
            midiOutClose(_handle);
            _handle = IntPtr.Zero;
            _deviceName = null;
            _outputIdentity = null;
            _deviceId = 0;
            _preparedLongMessagesUnsafe = false;
            _usesCumulativeByteCountResults = false;
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
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string moduleName);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetModuleFileName(IntPtr module, System.Text.StringBuilder fileName, int size);
    }
}
