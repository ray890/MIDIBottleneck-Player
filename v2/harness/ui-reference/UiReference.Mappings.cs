// UiReference, part 2: slider, scrub, timeline, font and scale tables.
//
// Grids (the integer loops below are repeated exactly in v2/tests/ui_tests.cpp):
//   microseconds-slider    A: v = -3..6000;  B: for s = 0..10000: m = SliderToMicroseconds(s); push m;
//                          if s > 6000 and s % 3 == 0 push m-1, m+1;  C: v = 5000 + i*i (i = 0,1,...) while v <= 1100000
//   bitrate-slider         A: v = -5..300;  B: for s = 0..10000: b = SliderToBitrate(s); push b;
//                          if s % 10 == 0 push b-1, b+1;  C: v = 100; while v <= 200000000: push v; v += max(1, v/200)
//   eventrate-slider       A: v = -3..3000; B: like bitrate with SliderToEventRate;
//                          C: v = 100; while v <= 20000000: push v; v += max(1, v/200)
//   processing-eventrate   v = -3..2100, then v = 2100; while v <= 2000000: push v; v += max(1, v/50)
//   eventrate-processing   the same inputs
//   track-point            for (min,max) in RANGES, width in WIDTHS, x = -10..width+10 step (width <= 40 ? 1 : 3)
//   scrub-*                for start in STARTS: for px in PIXELS; then for start in DENSE: for px = -700..700
//   timeline-*             see WriteTimeline
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Reflection;
using System.Runtime.Serialization;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class UiReference
    {
        private static void WriteMappingTables()
        {
            Thread.CurrentThread.CurrentCulture = EnUs;
            WriteSliders();
            WriteScrubs();
            WriteTimeline();
            WriteScaleAndFonts();
        }

        private static long BitrateSlider(long bitrate) { return (int)CallStatic(MainFormType, "BitrateToSlider", bitrate); }
        private static long SliderBitrate(int slider) { return (long)CallStatic(MainFormType, "SliderToBitrate", slider); }

        private static void WriteSliders()
        {
            using (Table t = new Table("slider-microseconds.txt"))
            {
                t.Comment("ProcessingTrackBar.SliderToMicroseconds(s) for s = 0..10000, one per line.");
                for (int s = 0; s <= 10000; s++) t.Raw(I(ProcessingTrackBar.SliderToMicroseconds(s)));
            }
            using (Table t = new Table("microseconds-slider.txt"))
            {
                t.Comment("ProcessingTrackBar.MicrosecondsToSlider over grid A, B, C (see UiReference.Mappings.cs header).");
                for (long v = -3; v <= 6000; v++) t.Raw(I(ProcessingTrackBar.MicrosecondsToSlider(v)));
                for (int s = 0; s <= 10000; s++)
                {
                    long m = ProcessingTrackBar.SliderToMicroseconds(s);
                    t.Raw(I(ProcessingTrackBar.MicrosecondsToSlider(m)));
                    if (s > 6000 && s % 3 == 0)
                    {
                        t.Raw(I(ProcessingTrackBar.MicrosecondsToSlider(m - 1)));
                        t.Raw(I(ProcessingTrackBar.MicrosecondsToSlider(m + 1)));
                    }
                }
                for (long i = 0; ; i++)
                {
                    long v = 5000 + i * i;
                    if (v > 1100000) break;
                    t.Raw(I(ProcessingTrackBar.MicrosecondsToSlider(v)));
                }
            }
            using (Table t = new Table("slider-bitrate.txt"))
            {
                t.Comment("MainForm.SliderToBitrate(s) for s = 0..10000, one per line.");
                for (int s = 0; s <= 10000; s++) t.Raw(I(SliderBitrate(s)));
            }
            using (Table t = new Table("bitrate-slider.txt"))
            {
                t.Comment("MainForm.BitrateToSlider over grid A, B, C.");
                for (long v = -5; v <= 300; v++) t.Raw(I(BitrateSlider(v)));
                for (int s = 0; s <= 10000; s++)
                {
                    long b = SliderBitrate(s);
                    t.Raw(I(BitrateSlider(b)));
                    if (s % 10 == 0)
                    {
                        t.Raw(I(BitrateSlider(b - 1)));
                        t.Raw(I(BitrateSlider(b + 1)));
                    }
                }
                for (long v = 100; v <= 200000000; v += Math.Max(1, v / 200)) t.Raw(I(BitrateSlider(v)));
            }
            using (Table t = new Table("slider-eventrate.txt"))
            {
                t.Comment("MainForm.SliderToEventRate(s) for s = 0..10000, one per line.");
                for (int s = 0; s <= 10000; s++) t.Raw(I(MainForm.SliderToEventRate(s)));
            }
            using (Table t = new Table("eventrate-slider.txt"))
            {
                t.Comment("MainForm.EventRateToSlider over grid A, B, C.");
                for (long v = -3; v <= 3000; v++) t.Raw(I(MainForm.EventRateToSlider(v)));
                for (int s = 0; s <= 10000; s++)
                {
                    long b = MainForm.SliderToEventRate(s);
                    t.Raw(I(MainForm.EventRateToSlider(b)));
                    if (s % 10 == 0)
                    {
                        t.Raw(I(MainForm.EventRateToSlider(b - 1)));
                        t.Raw(I(MainForm.EventRateToSlider(b + 1)));
                    }
                }
                for (long v = 100; v <= 20000000; v += Math.Max(1, v / 200)) t.Raw(I(MainForm.EventRateToSlider(v)));
            }
            using (Table t = new Table("processing-eventrate.txt"))
            {
                t.Comment("MainForm.ProcessingToEventRate over -3..2100 then v += max(1, v/50) up to 2000000.");
                for (long v = -3; v <= 2100; v++) t.Raw(I(MainForm.ProcessingToEventRate(v)));
                for (long v = 2100; v <= 2000000; v += Math.Max(1, v / 50)) t.Raw(I(MainForm.ProcessingToEventRate(v)));
            }
            using (Table t = new Table("eventrate-processing.txt"))
            {
                t.Comment("MainForm.EventRateToProcessing over the same inputs.");
                for (long v = -3; v <= 2100; v++) t.Raw(I(MainForm.EventRateToProcessing(v)));
                for (long v = 2100; v <= 2000000; v += Math.Max(1, v / 50)) t.Raw(I(MainForm.EventRateToProcessing(v)));
            }
            using (Table t = new Table("track-point.txt"))
            {
                t.Comment("ProcessingTrackBar.ValueFromTrackPoint(x, width, min, max); loops in the header of UiReference.Mappings.cs.");
                int[][] ranges = { new int[] { 0, 10000 }, new int[] { 0, 100 }, new int[] { 5, 9 }, new int[] { 0, 1 },
                    new int[] { 0, 0 }, new int[] { 3, 2 }, new int[] { -5, 5 } };
                int[] widths = { -1, 0, 1, 2, 3, 10, 17, 18, 19, 40, 100, 299, 300, 801 };
                for (int ri = 0; ri < ranges.Length; ri++)
                    for (int wi = 0; wi < widths.Length; wi++)
                    {
                        int step = widths[wi] <= 40 ? 1 : 3;
                        for (int x = -10; x <= widths[wi] + 10; x += step)
                            t.Raw(I(ProcessingTrackBar.ValueFromTrackPoint(x, widths[wi], ranges[ri][0], ranges[ri][1])));
                    }
            }
        }

        // ---------------------------------------------------------------- scrubs

        private static readonly long[] ScrubPixels = BuildScrubPixels();

        private static long[] BuildScrubPixels()
        {
            List<long> list = new List<long>(new long[] { -2000001, -2000000, -1000001, -1000000, -999999, -100000, -10000,
                -5001, -5000, -1000, -501, -500, -100, -50 });
            for (long p = -40; p <= 40; p++) list.Add(p);
            list.AddRange(new long[] { 50, 100, 499, 500, 501, 1000, 5000, 5001, 10000, 100000, 999999, 1000000, 1000001, 2000000, 2000001 });
            return list.ToArray();
        }

        private static void WriteScrubs()
        {
            // The grids below are repeated in ui_tests.cpp.
            ScrubTable("scrub-rate-processing.txt", ServiceDurationMode.ProcessingTime,
                new int[] { 0, 1, 2, 3, 10, 100, 1000, 4999, 5000, 5001, 5002, 6000, 10000, 50000, 123456, 500000, 999999, 1000000, 1000001, -1 },
                new int[] { 0, 5000 });
            ScrubTable("scrub-rate-bitrate.txt", ServiceDurationMode.MidiBitrate,
                new int[] { 0, 1, 50, 99, 100, 101, 300, 1000, 31250, 38400, 115200, 1000000, 12345678, 99999999, 100000000, 100000001 },
                new int[] { 99, 31250 });
            ScrubTable("scrub-rate-eventrate.txt", ServiceDurationMode.EventsPerSecond,
                new int[] { 0, 1, 2, 50, 99, 100, 101, 500, 1000, 9999, 10000, 100000, 1234567, 9999998, 9999999, 10000000 },
                new int[] { 50, 100 });
            using (Table t = new Table("scrub-queue.txt"))
            {
                t.Comment("MainForm.MapQueueScrubValue(start, px): starts then dense starts, as in the header.");
                int[] starts = { -3, 0, 1, 2, 3, 10, 100, 250, 499, 500, 501, 502, 1000, 2000, 10000, 50000, 123456, 999999, 1000000, 1000001 };
                int[] dense = { 500, 1 };
                for (int i = 0; i < starts.Length; i++)
                    for (int p = 0; p < ScrubPixels.Length; p++) t.Raw(I(MainForm.MapQueueScrubValue(starts[i], ScrubPixels[p])));
                for (int i = 0; i < dense.Length; i++)
                    for (long p = -700; p <= 700; p++) t.Raw(I(MainForm.MapQueueScrubValue(dense[i], p)));
            }
            using (Table t = new Table("scrub-queue-age.txt"))
            {
                t.Comment("MainForm.MapQueueAgeScrubValue(start, px).");
                int[] starts = { -5, 0, 1, 10, 249, 250, 251, 1000, 100000, 999999, 1000000, 1000001, 5000000, 100000000, 1799999999, 1800000000, 1800000001 };
                int[] dense = { 1000000, 1000 };
                for (int i = 0; i < starts.Length; i++)
                    for (int p = 0; p < ScrubPixels.Length; p++) t.Raw(I(MainForm.MapQueueAgeScrubValue(starts[i], ScrubPixels[p])));
                for (int i = 0; i < dense.Length; i++)
                    for (long p = -700; p <= 700; p++) t.Raw(I(MainForm.MapQueueAgeScrubValue(dense[i], p)));
            }
        }

        private static void ScrubTable(string name, ServiceDurationMode mode, int[] starts, int[] dense)
        {
            using (Table t = new Table(name))
            {
                t.Comment("MainForm.MapRateScrubValue(" + mode.ToString() + ", start, px): starts x pixels, then dense starts x -700..700.");
                for (int i = 0; i < starts.Length; i++)
                    for (int p = 0; p < ScrubPixels.Length; p++) t.Raw(I(MainForm.MapRateScrubValue(mode, starts[i], ScrubPixels[p])));
                for (int i = 0; i < dense.Length; i++)
                    for (long p = -700; p <= 700; p++) t.Raw(I(MainForm.MapRateScrubValue(mode, dense[i], p)));
            }
        }

        // ---------------------------------------------------------------- timeline

        private static void WriteTimeline()
        {
            long[] durations = { 0, 1, 999, 1000000, 123456789, 3600000000L };
            int[] widths = { 300, 777 };
            using (Table t = new Table("timeline-position.txt"))
            {
                t.Comment("PlaybackTimelineView.PositionFromX through a real control. Row: controlWidth, duration, position, x, trackLeft, trackWidth, result.");
                using (PlaybackTimelineView view = new PlaybackTimelineView())
                {
                    MethodInfo positionFromX = typeof(PlaybackTimelineView).GetMethod("PositionFromX", Hidden);
                    MethodInfo layout = typeof(PlaybackTimelineView).GetMethod("CalculateLayout", Hidden);
                    for (int wi = 0; wi < widths.Length; wi++)
                    {
                        view.Size = new Size(widths[wi], 42);
                        for (int di = 0; di < durations.Length; di++)
                        {
                            long[] positions = { 0, durations[di] / 2 };
                            for (int pi = 0; pi < positions.Length; pi++)
                            {
                                view.SetTimeline(positions[pi], durations[di]);
                                long shown = view.PositionMicroseconds;
                                Rectangle track;
                                using (Graphics graphics = view.CreateGraphics())
                                {
                                    string time = (string)CallStatic(typeof(PlaybackTimelineView), "FormatTime", shown) + " / " +
                                        (string)CallStatic(typeof(PlaybackTimelineView), "FormatTime", durations[di]);
                                    object[] args = new object[] { graphics, time, null };
                                    track = (Rectangle)layout.Invoke(view, args);
                                }
                                for (int k = 0; k <= 36; k++)
                                {
                                    int x = -6 + (k * (widths[wi] + 12)) / 36;
                                    long result = (long)positionFromX.Invoke(view, new object[] { x });
                                    double fraction = Math.Max(0, Math.Min(1, (double)(x - track.Left) / track.Width));
                                    long copy = (long)Math.Round(durations[di] * fraction);
                                    if (copy != result) throw new InvalidOperationException("PositionFromX arithmetic differs from its copy");
                                    t.Row(I(widths[wi]), I(durations[di]), I(shown), I(x), I(track.Left), I(track.Width), I(result));
                                }
                            }
                        }
                    }
                }
            }
            using (Table t = new Table("timeline-keys.txt"))
            {
                t.Comment("PlaybackTimelineView.OnKeyDown through a real control. Row: key, position, duration, seek target.");
                Keys[] keys = { Keys.Home, Keys.End, Keys.Left, Keys.Right, Keys.PageUp, Keys.PageDown };
                string[] names = { "Home", "End", "Left", "Right", "PageUp", "PageDown" };
                long[] durs = { 0, 1, 999999, 1000000, 4999999, 5000000, 5000001, 123456789, 3600000000L };
                MethodInfo onKeyDown = typeof(PlaybackTimelineView).GetMethod("OnKeyDown", Hidden);
                using (PlaybackTimelineView view = new PlaybackTimelineView())
                {
                    long seek = -1;
                    view.SeekRequested += delegate(object s, TimelineSeekEventArgs e) { seek = e.PositionMicroseconds; };
                    for (int d = 0; d < durs.Length; d++)
                    {
                        long[] positions = { 0, 1, 500000, 999999, 1000000, 1000001, 4999999, 5000000, durs[d] - 1, durs[d], durs[d] / 2 };
                        for (int p = 0; p < positions.Length; p++)
                        {
                            for (int k = 0; k < keys.Length; k++)
                            {
                                view.SetTimeline(positions[p], durs[d]);
                                long start = view.PositionMicroseconds;
                                seek = -1;
                                onKeyDown.Invoke(view, new object[] { new KeyEventArgs(keys[k]) });
                                t.Row(names[k], I(start), I(durs[d]), I(seek));
                            }
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------- scale metrics and fonts

        private static void WriteScaleAndFonts()
        {
            int[] canonical = { -50, -17, -8, -4, -3, -1, 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 64, 100, 235, 640 };
            using (Table t = new Table("scale-metric.txt"))
            {
                t.Comment("MainForm.ScaleMetric(canonical) at _uiScalePercent = 50..200 (percent-major, then canonical order).");
                object form = FormatterServices.GetUninitializedObject(typeof(MainForm));
                FieldInfo percentField = typeof(MainForm).GetField("_uiScalePercent", Hidden);
                MethodInfo scale = typeof(MainForm).GetMethod("ScaleMetric", Hidden);
                for (int percent = 50; percent <= 200; percent++)
                {
                    percentField.SetValue(form, percent);
                    for (int i = 0; i < canonical.Length; i++) t.Raw(I((int)scale.Invoke(form, new object[] { canonical[i] })));
                }
            }
            using (Table t = new Table("scale-metric-min1.txt"))
            {
                t.Comment("PlaybackTimelineView.ScaleMetric(value) after ApplicationScalePercent = 50..200 (same order).");
                MethodInfo scale = typeof(PlaybackTimelineView).GetMethod("ScaleMetric", Hidden);
                using (PlaybackTimelineView view = new PlaybackTimelineView())
                {
                    for (int percent = 50; percent <= 200; percent++)
                    {
                        view.ApplicationScalePercent = percent;
                        for (int i = 0; i < canonical.Length; i++) t.Raw(I((int)scale.Invoke(view, new object[] { canonical[i] })));
                    }
                }
            }
            using (Table t = new Table("ui-font.txt"))
            {
                t.Comment("UiScaleFont.CreateUi / CreateMonospace: row kind (ui|mono), canonical points, percent, font name, point size (float bits).");
                float[] points = { 6.5F, 8F, 9F, 12F };
                int[] percents = { -5, 0, 49, 50, 51, 60, 74, 75, 76, 90, 99, 100, 101, 125, 150, 175, 199, 200, 201, 300 };
                for (int kind = 0; kind < 2; kind++)
                    for (int pi = 0; pi < points.Length; pi++)
                        for (int ci = 0; ci < percents.Length; ci++)
                        {
                            using (System.Drawing.Font font = kind == 0
                                ? UiScaleFont.CreateUi(points[pi], FontStyle.Regular, percents[ci])
                                : UiScaleFont.CreateMonospace(points[pi], FontStyle.Regular, percents[ci]))
                            {
                                t.Row(kind == 0 ? "ui" : "mono", FloatText(points[pi]), I(percents[ci]), font.Name, FloatBits(font.Size));
                            }
                        }
            }
        }

        internal static string FloatBits(float value)
        {
            return BitConverter.ToInt32(BitConverter.GetBytes(value), 0).ToString("x8", CultureInfo.InvariantCulture);
        }

        internal static string FloatText(float value)
        {
            return FloatBits(value);
        }
    }
}
