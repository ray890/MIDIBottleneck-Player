using System;
using System.Runtime.InteropServices;

namespace MidiBottleneck
{
    // Windows multimedia structures are declared under the packed multimedia
    // ABI. On x64 MIDIHDR is therefore 112 bytes (not the 120 bytes produced
    // by default CLR pointer alignment).
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct NativeMidiHeader
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

        public static NativeMidiHeader CreateOutput(IntPtr data, int length)
        {
            if (data == IntPtr.Zero) throw new ArgumentException("A MIDI output buffer pointer is required.", "data");
            if (length <= 0) throw new ArgumentOutOfRangeException("length");
            NativeMidiHeader header = new NativeMidiHeader();
            header.Data = data;
            header.BufferLength = (uint)length;
            header.BytesRecorded = 0;
            return header;
        }
    }
}
