using System;
using System.Drawing;

namespace MidiBottleneck
{
    // Small Segoe UI and Consolas outlines lose substantial detail below the
    // canonical size. Tahoma/Lucida Console have strong Windows bitmap/hinting
    // behavior at tiny sizes. Geometry still comes from the canonical scale;
    // only the sub-100% font face and a modest readability floor change.
    internal static class UiScaleFont
    {
        internal static Font CreateUi(float canonicalPoints, FontStyle style, int percent)
        {
            return Create("Segoe UI", "Tahoma", canonicalPoints, style, percent);
        }

        internal static Font CreateMonospace(float canonicalPoints, FontStyle style, int percent)
        {
            return Create("Consolas", "Lucida Console", canonicalPoints, style, percent);
        }

        private static Font Create(string canonicalFamily, string smallFamily,
            float canonicalPoints, FontStyle style, int percent)
        {
            percent = Math.Max(50, Math.Min(200, percent));
            float points = canonicalPoints * percent / 100F;
            if (percent == 50) points = Math.Max(points, 5.5F);
            else if (percent == 75) points = Math.Max(points, 7F);
            string family = percent < 100 ? smallFamily : canonicalFamily;
            try { return new Font(family, points, style, GraphicsUnit.Point); }
            catch (ArgumentException)
            {
                return new Font(canonicalFamily, points, style, GraphicsUnit.Point);
            }
        }
    }
}
