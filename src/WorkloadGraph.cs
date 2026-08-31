using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class WorkloadGraph : Control
    {
        private WorkloadAnalysis _analysis;
        private readonly Font _labelFont;

        public WorkloadGraph()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;
            BackColor = Color.FromArgb(24, 27, 32);
            ForeColor = Color.Gainsboro;
            _labelFont = new Font("Segoe UI", 8F, FontStyle.Regular, GraphicsUnit.Point);
        }

        public WorkloadAnalysis Analysis
        {
            get { return _analysis; }
            set { _analysis = value; Invalidate(); }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.Clear(BackColor);
            if (_analysis == null || _analysis.Buckets == null || _analysis.Buckets.Length == 0) return;

            string scope = "WHOLE FILE   " + FormatClock(0) + " – " + FormatClock(_analysis.DurationMicroseconds);
            TextRenderer.DrawText(e.Graphics, scope, _labelFont, new Rectangle(54, 2, ClientSize.Width - 70, 20), Color.Gainsboro,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            Rectangle eventsArea = new Rectangle(54, 42, Math.Max(20, ClientSize.Width - 70), Math.Max(30, (ClientSize.Height - 158) / 2));
            Rectangle bytesArea = new Rectangle(54, eventsArea.Bottom + 34, eventsArea.Width, eventsArea.Height);
            DrawPanel(e.Graphics, eventsArea, true, Color.FromArgb(63, 190, 255), "Events/sec", "events/sec", _analysis.PeakEventsPerSecond, _analysis.EventServiceCapacityPerSecond);
            DrawPanel(e.Graphics, bytesArea, false, Color.FromArgb(255, 174, 66), "MIDI bytes/sec", "bytes/sec", _analysis.PeakBytesPerSecond, _analysis.ByteServiceCapacityPerSecond);
            DrawTimelineAxis(e.Graphics, bytesArea);
            DrawPressureStrip(e.Graphics, bytesArea);
            string window = "Fixed " + (_analysis.BucketMicroseconds / 1000.0).ToString("N0") + " ms buckets   •   magenta: cluster ≥50   •   red: predicted overflow";
            TextRenderer.DrawText(e.Graphics, window, _labelFont, new Rectangle(54, ClientSize.Height - 22, eventsArea.Width, 20), Color.Silver,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        private void DrawPanel(Graphics graphics, Rectangle area, bool events, Color color, string title, string unit, double peak, double capacity)
        {
            using (Pen grid = new Pen(Color.FromArgb(65, 75, 86)))
            using (Pen plot = new Pen(color, 1.5F))
            using (Pen cluster = new Pen(Color.FromArgb(230, 235, 75, 205), 2F))
            using (Pen overflow = new Pen(Color.FromArgb(235, 235, 65, 65), 2F))
            using (Pen capacityPen = new Pen(Color.FromArgb(220, 110, 220, 130), 1F))
            {
                capacityPen.DashStyle = System.Drawing.Drawing2D.DashStyle.Dash;
                graphics.DrawRectangle(grid, area);
                for (int i = 1; i < 4; i++)
                {
                    int y = area.Top + (area.Height * i / 4);
                    graphics.DrawLine(grid, area.Left, y, area.Right, y);
                }
                double bucketSeconds = _analysis.BucketMicroseconds / 1000000.0;
                Point previous = new Point(area.Left, area.Bottom);
                for (int x = 0; x < area.Width; x++)
                {
                    int first = x * _analysis.Buckets.Length / area.Width;
                    int last = Math.Max(first + 1, (x + 1) * _analysis.Buckets.Length / area.Width);
                    double value = 0;
                    bool denseCluster = false;
                    bool predictedOverflow = false;
                    for (int bucket = first; bucket < last && bucket < _analysis.Buckets.Length; bucket++)
                    {
                        double current = events ? _analysis.Buckets[bucket].EventCount / bucketSeconds : _analysis.Buckets[bucket].ByteCount / bucketSeconds;
                        value = Math.Max(value, current);
                        if (_analysis.Buckets[bucket].LargestCluster >= 50) denseCluster = true;
                        if (_analysis.Buckets[bucket].PredictedDroppedEvents > 0) predictedOverflow = true;
                    }
                    int y = peak <= 0 ? area.Bottom : area.Bottom - (int)Math.Round((value / peak) * area.Height);
                    Point currentPoint = new Point(area.Left + x, Math.Max(area.Top, Math.Min(area.Bottom, y)));
                    if (x > 0) graphics.DrawLine(plot, previous, currentPoint);
                    if (predictedOverflow) graphics.DrawLine(overflow, area.Left + x, area.Top, area.Left + x, area.Top + 9);
                    if (denseCluster) graphics.DrawLine(cluster, area.Left + x, area.Bottom - 8, area.Left + x, area.Bottom);
                    previous = currentPoint;
                }
                if (capacity > 0 && peak > 0 && capacity <= peak)
                {
                    int capacityY = area.Bottom - (int)Math.Round((capacity / peak) * area.Height);
                    graphics.DrawLine(capacityPen, area.Left, capacityY, area.Right, capacityY);
                }
            }

            TextRenderer.DrawText(graphics, title, _labelFont, new Rectangle(area.Left, area.Top - 22, area.Width / 2, 20), color,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            string scale = "peak " + peak.ToString("N1") + " " + unit;
            if (capacity > 0) scale += "   maximum rate " + capacity.ToString("N1") + " " + unit;
            TextRenderer.DrawText(graphics, scale, _labelFont, new Rectangle(area.Left + area.Width / 3, area.Top - 22, area.Width * 2 / 3, 20), ForeColor,
                TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        private void DrawTimelineAxis(Graphics graphics, Rectangle area)
        {
            for (int tick = 0; tick <= 4; tick++)
            {
                int x = area.Left + area.Width * tick / 4;
                long time = _analysis.DurationMicroseconds * tick / 4;
                Rectangle label = new Rectangle(x - 42, area.Bottom + 2, 84, 18);
                TextRenderer.DrawText(graphics, FormatClock(time), _labelFont, label, Color.Silver,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            }
        }

        private void DrawPressureStrip(Graphics graphics, Rectangle area)
        {
            AnalysisConfiguration configuration = _analysis.Configuration;
            if (configuration == null || !configuration.QueueLengthLimitEnabled) return;
            int y = area.Bottom + 22;
            using (Brush low = new SolidBrush(Color.FromArgb(75, 75, 150, 95)))
            using (Brush medium = new SolidBrush(Color.FromArgb(130, 230, 170, 45)))
            using (Brush high = new SolidBrush(Color.FromArgb(180, 230, 65, 65)))
            {
                for (int x = 0; x < area.Width; x++)
                {
                    int first = x * _analysis.Buckets.Length / area.Width;
                    int last = Math.Max(first + 1, (x + 1) * _analysis.Buckets.Length / area.Width);
                    int occupancy = 0;
                    for (int bucket = first; bucket < last && bucket < _analysis.Buckets.Length; bucket++)
                        occupancy = Math.Max(occupancy, _analysis.Buckets[bucket].PredictedPeakOccupancy);
                    double ratio = (double)occupancy / Math.Max(1, configuration.QueueLengthLimit);
                    Brush brush = ratio >= 1 ? high : ratio >= 0.65 ? medium : low;
                    graphics.FillRectangle(brush, area.Left + x, y, 1, 5);
                }
            }
            TextRenderer.DrawText(graphics, "predicted buffer pressure", _labelFont, new Rectangle(area.Left, y + 5, 180, 16), Color.Silver,
                TextFormatFlags.Left | TextFormatFlags.SingleLine);
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00") + ":" + value.Seconds.ToString("00");
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) _labelFont.Dispose();
            base.Dispose(disposing);
        }
    }
}
