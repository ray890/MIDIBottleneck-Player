using System;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class ProcessingTrackBar : TrackBar
    {
        internal const int ScaleMaximum = 10000;
        internal const int LowRangeEnd = 6000;
        internal const long LowRangeMicroseconds = 5000;
        internal const long MaximumMicroseconds = 1000000;

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                ApplyTrackClick(e.X);
            }
            base.OnMouseDown(e);
        }

        internal void ApplyTrackClick(int x)
        {
            Value = ValueFromTrackPoint(x, ClientSize.Width, Minimum, Maximum);
        }

        internal static int ValueFromTrackPoint(int x, int width, int minimum, int maximum)
        {
            if (maximum <= minimum || width <= 1) return minimum;
            const int edge = 8;
            int usable = Math.Max(1, width - edge * 2);
            double normalized = (x - edge) / (double)usable;
            normalized = Math.Max(0.0, Math.Min(1.0, normalized));
            return minimum + (int)Math.Round(normalized * (maximum - minimum));
        }

        internal static long SliderToMicroseconds(int slider)
        {
            slider = Math.Max(0, Math.Min(ScaleMaximum, slider));
            if (slider <= LowRangeEnd)
                return (long)Math.Round(slider * (double)LowRangeMicroseconds / LowRangeEnd);
            double normalized = (slider - LowRangeEnd) / (double)(ScaleMaximum - LowRangeEnd);
            return (long)Math.Round(LowRangeMicroseconds * Math.Pow(MaximumMicroseconds / (double)LowRangeMicroseconds, normalized));
        }

        internal static int MicrosecondsToSlider(long microseconds)
        {
            if (microseconds <= 0) return 0;
            if (microseconds <= LowRangeMicroseconds)
                return Math.Max(0, Math.Min(LowRangeEnd, (int)Math.Round(microseconds * (double)LowRangeEnd / LowRangeMicroseconds)));
            microseconds = Math.Min(MaximumMicroseconds, microseconds);
            double normalized = Math.Log(microseconds / (double)LowRangeMicroseconds) /
                Math.Log(MaximumMicroseconds / (double)LowRangeMicroseconds);
            return Math.Max(LowRangeEnd, Math.Min(ScaleMaximum,
                LowRangeEnd + (int)Math.Round(normalized * (ScaleMaximum - LowRangeEnd))));
        }
    }
}
