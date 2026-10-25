using System.Drawing;

namespace MidiBottleneck
{
    internal static class QueuePressurePalette
    {
        internal static readonly Color Low = Color.FromArgb(65, 165, 90);
        internal static readonly Color Medium = Color.FromArgb(215, 155, 35);
        internal static readonly Color High = Color.FromArgb(210, 70, 70);
        internal static readonly Color OverLimit = Color.FromArgb(155, 75, 190);
        internal static readonly Color Background = Color.FromArgb(225, 228, 232);
        internal static readonly Color Outline = Color.FromArgb(155, 160, 166);

        internal static Color Choose(double ratio, bool overflowPulse)
        {
            if (ratio > 1.0) return OverLimit;
            if (overflowPulse || ratio >= 0.9) return High;
            return ratio >= 0.65 ? Medium : Low;
        }
    }
}
