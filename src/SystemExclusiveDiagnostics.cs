using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace MidiBottleneck
{
    internal static class SystemExclusiveDiagnostics
    {
        public static string DescribeFailure(string nativeOperation, uint errorCode, string nativeText,
            string sourceFile, SystemExclusivePacket packet, MidiEvent finalEvent, int headerSize, uint bufferLength,
            uint bytesRecorded, uint flags, IntPtr dataPointer)
        {
            StringBuilder text = new StringBuilder();
            text.Append("MIDI System Exclusive failure during ").Append(nativeOperation)
                .Append(" (native code ").Append(errorCode).Append(")");
            if (!String.IsNullOrEmpty(nativeText)) text.Append(": ").Append(nativeText);
            text.AppendLine().Append("Source: ").Append(String.IsNullOrEmpty(sourceFile) ? "unknown" : Path.GetFileName(sourceFile));
            text.Append("; event #").Append(finalEvent.EventIndex)
                .Append("; tick ").Append(finalEvent.AbsoluteTick)
                .Append("; source time ").Append(finalEvent.IntendedMicroseconds.ToString(CultureInfo.InvariantCulture)).Append(" µs");
            text.AppendLine().Append("Fragments: ");
            if (packet == null || packet.Fragments == null || packet.Fragments.Count == 0) text.Append("none recorded");
            else
            {
                for (int i = 0; i < packet.Fragments.Count; i++)
                {
                    if (i > 0) text.Append(" → ");
                    SystemExclusiveFragment fragment = packet.Fragments[i];
                    text.Append("F").Append((fragment.Status & 0x0F).ToString("X1"))
                        .Append("[#").Append(fragment.EventIndex).Append(",tick=").Append(fragment.Tick)
                        .Append(",bytes=").Append(fragment.DataLength).Append("]");
                }
            }
            byte[] bytes = packet == null ? null : packet.Bytes;
            text.AppendLine().Append("Packet: ").Append(bytes == null ? 0 : bytes.Length).Append(" bytes; framed F0…F7: ")
                .Append(SystemExclusiveAssembler.IsComplete(bytes) ? "yes" : "no")
                .Append("; leading ").Append(FormatEdge(bytes, true))
                .Append("; trailing ").Append(FormatEdge(bytes, false));
            text.AppendLine().Append("MIDIHDR: size=").Append(headerSize)
                .Append(", dwBufferLength=").Append(bufferLength)
                .Append(", dwBytesRecorded=").Append(bytesRecorded)
                .Append(", dwFlags=0x").Append(flags.ToString("X8"))
                .Append(", data=0x").Append(dataPointer.ToInt64().ToString("X"))
                .Append(", pointer alignment=").Append(dataPointer == IntPtr.Zero ? -1 : dataPointer.ToInt64() & (IntPtr.Size - 1));
            return text.ToString();
        }

        private static string FormatEdge(byte[] bytes, bool leading)
        {
            if (bytes == null || bytes.Length == 0) return "(empty)";
            int count = Math.Min(16, bytes.Length);
            int start = leading ? 0 : bytes.Length - count;
            StringBuilder result = new StringBuilder();
            for (int i = 0; i < count; i++)
            {
                if (i > 0) result.Append(' ');
                result.Append(bytes[start + i].ToString("X2"));
            }
            return result.ToString();
        }
    }
}
