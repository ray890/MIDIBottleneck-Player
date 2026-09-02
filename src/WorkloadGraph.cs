using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class WorkloadSelectionEventArgs : EventArgs
    {
        public readonly long TimeMicroseconds;
        public readonly bool Pinned;
        public WorkloadSelectionEventArgs(long timeMicroseconds, bool pinned)
        {
            TimeMicroseconds = timeMicroseconds;
            Pinned = pinned;
        }
    }

    internal enum AnalysisFollowTarget
    {
        Off,
        PlaybackTimeline,
        MidiOutput
    }

    internal sealed class WorkloadGraph : Control
    {
        private WorkloadAnalysis _analysis;
        private readonly Font _labelFont;
        private readonly ToolTip _toolTip;
        private long? _hoverTime;
        private long? _pinnedTime;
        private long? _playbackTimeline;
        private long? _midiOutputPosition;
        private long _viewStart;
        private long _viewEnd;
        private AnalysisFollowTarget _followTarget;
        private bool _mouseDown;
        private bool _dragging;
        private Point _dragOrigin;
        private long _dragViewStart;

        public event EventHandler<WorkloadSelectionEventArgs> InspectionChanged;
        public event EventHandler<WorkloadSelectionEventArgs> SeekRequested;

        public WorkloadGraph()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;
            BackColor = Color.FromArgb(24, 27, 32);
            ForeColor = Color.Gainsboro;
            _labelFont = new Font("Segoe UI", 8F, FontStyle.Regular, GraphicsUnit.Point);
            _toolTip = new ToolTip();
            _toolTip.SetToolTip(this, "Wheel: zoom around cursor. Drag: pan. Click: pin. Double-click: seek.");
            Cursor = Cursors.Cross;
        }

        public WorkloadAnalysis Analysis
        {
            get { return _analysis; }
            set
            {
                bool first = _analysis == null;
                long oldDuration = _analysis == null ? 0 : _analysis.DurationMicroseconds;
                _analysis = value;
                if (_analysis != null)
                {
                    if (first || oldDuration <= 0 || _viewEnd <= _viewStart)
                        ResetZoom();
                    else
                    {
                        _viewStart = Math.Max(0, Math.Min(_analysis.DurationMicroseconds, _viewStart));
                        _viewEnd = Math.Max(_viewStart + 1, Math.Min(_analysis.DurationMicroseconds, _viewEnd));
                    }
                    if (_pinnedTime.HasValue) _pinnedTime = Math.Min(_analysis.DurationMicroseconds, _pinnedTime.Value);
                }
                Invalidate();
            }
        }

        internal long? PinnedTimeMicroseconds { get { return _pinnedTime; } }
        internal long? HoverTimeMicroseconds { get { return _hoverTime; } }
        internal long? InspectedTimeMicroseconds { get { return _hoverTime ?? _pinnedTime; } }
        internal bool UsesByteRate { get { return _analysis != null && _analysis.Configuration != null && _analysis.Configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate; } }
        internal long ViewStartMicroseconds { get { return _viewStart; } }
        internal long ViewEndMicroseconds { get { return _viewEnd; } }
        internal AnalysisFollowTarget FollowTarget { get { return _followTarget; } set { _followTarget = value; ApplyFollow(); } }

        internal Rectangle GraphArea
        {
            get { return new Rectangle(54, 45, Math.Max(20, ClientSize.Width - 70), Math.Max(50, ClientSize.Height - 168)); }
        }

        internal long TimeAtClientX(int x)
        {
            if (_analysis == null || _analysis.DurationMicroseconds <= 0) return 0;
            Rectangle area = GraphArea;
            double normalized = (x - area.Left) / (double)Math.Max(1, area.Width);
            normalized = Math.Max(0, Math.Min(1, normalized));
            return _viewStart + (long)Math.Round(normalized * Math.Max(1, _viewEnd - _viewStart));
        }

        private int ClientXAtTime(long time)
        {
            Rectangle area = GraphArea;
            return area.Left + (int)Math.Round(area.Width * (time - _viewStart) / (double)Math.Max(1, _viewEnd - _viewStart));
        }

        internal WorkloadBucket BucketAt(long timeMicroseconds)
        {
            if (_analysis == null || _analysis.Buckets == null || _analysis.Buckets.Length == 0) return null;
            int index = (int)Math.Max(0, Math.Min(_analysis.Buckets.Length - 1,
                timeMicroseconds / Math.Max(1, _analysis.BucketMicroseconds)));
            return _analysis.Buckets[index];
        }

        internal void InspectAtClientX(int x, bool pinned)
        {
            long time = TimeAtClientX(x);
            _hoverTime = time;
            if (pinned) _pinnedTime = time;
            OnInspectionChanged(time, pinned);
            Invalidate();
        }

        internal void ResetZoom()
        {
            _viewStart = 0;
            _viewEnd = _analysis == null ? 1 : Math.Max(1, _analysis.DurationMicroseconds);
            Invalidate();
        }

        internal void ZoomAtClientX(int x, int wheelDelta)
        {
            if (_analysis == null || _analysis.DurationMicroseconds <= 0 || wheelDelta == 0) return;
            long duration = _analysis.DurationMicroseconds;
            long oldSpan = Math.Max(1, _viewEnd - _viewStart);
            double factor = Math.Pow(0.8, wheelDelta / 120.0);
            long minimum = Math.Max(_analysis.BucketMicroseconds * 2, Math.Max(1, duration / 2000));
            long newSpan = Math.Max(minimum, Math.Min(duration, (long)Math.Round(oldSpan * factor)));
            long anchor = TimeAtClientX(x);
            Rectangle area = GraphArea;
            double ratio = Math.Max(0, Math.Min(1, (x - area.Left) / (double)Math.Max(1, area.Width)));
            long start = anchor - (long)Math.Round(newSpan * ratio);
            SetViewport(start, start + newSpan);
        }

        internal void PanByPixels(int pixels)
        {
            long span = Math.Max(1, _viewEnd - _viewStart);
            long delta = (long)Math.Round(-pixels * span / (double)Math.Max(1, GraphArea.Width));
            SetViewport(_viewStart + delta, _viewEnd + delta);
        }

        private void SetViewport(long start, long end)
        {
            if (_analysis == null) return;
            long duration = Math.Max(1, _analysis.DurationMicroseconds);
            long span = Math.Max(1, Math.Min(duration, end - start));
            if (start < 0) start = 0;
            if (start + span > duration) start = duration - span;
            _viewStart = Math.Max(0, start);
            _viewEnd = _viewStart + span;
            Invalidate();
        }

        internal void SetPlaybackPositions(long playbackTimeline, long midiOutputPosition)
        {
            playbackTimeline = Math.Max(0, playbackTimeline);
            midiOutputPosition = Math.Max(0, midiOutputPosition);
            if (_playbackTimeline == playbackTimeline && _midiOutputPosition == midiOutputPosition) return;
            _playbackTimeline = playbackTimeline;
            _midiOutputPosition = midiOutputPosition;
            ApplyFollow();
            Invalidate();
        }

        private void ApplyFollow()
        {
            if (_analysis == null || _followTarget == AnalysisFollowTarget.Off) return;
            long? marker = _followTarget == AnalysisFollowTarget.PlaybackTimeline ? _playbackTimeline : _midiOutputPosition;
            if (!marker.HasValue || _viewStart == 0 && _viewEnd >= _analysis.DurationMicroseconds) return;
            long span = Math.Max(1, _viewEnd - _viewStart);
            long left = _viewStart + span * 15 / 100;
            long right = _viewStart + span * 85 / 100;
            if (marker.Value >= left && marker.Value <= right) return;
            long desired = marker.Value < left ? marker.Value - span * 30 / 100 : marker.Value - span * 70 / 100;
            SetViewport(desired, desired + span);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e);
            if (GraphArea.Contains(e.Location)) ZoomAtClientX(e.X, e.Delta);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (_mouseDown)
            {
                int dx = e.X - _dragOrigin.X;
                if (!_dragging && Math.Abs(dx) >= 4) _dragging = true;
                if (_dragging)
                {
                    long span = Math.Max(1, _viewEnd - _viewStart);
                    long shift = (long)Math.Round(-dx * span / (double)Math.Max(1, GraphArea.Width));
                    SetViewport(_dragViewStart + shift, _dragViewStart + shift + span);
                    Cursor = Cursors.SizeWE;
                    return;
                }
            }
            if (_analysis == null || !GraphArea.Contains(e.Location)) return;
            long time = TimeAtClientX(e.X);
            if (_hoverTime == time) return;
            _hoverTime = time;
            OnInspectionChanged(time, false);
            Invalidate();
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            if (_mouseDown) return;
            _hoverTime = null;
            if (_pinnedTime.HasValue) OnInspectionChanged(_pinnedTime.Value, true);
            Invalidate();
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left || _analysis == null || !GraphArea.Contains(e.Location)) return;
            _mouseDown = true;
            _dragging = false;
            _dragOrigin = e.Location;
            _dragViewStart = _viewStart;
            Capture = true;
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (e.Button != MouseButtons.Left || !_mouseDown) return;
            bool wasDragging = _dragging;
            _mouseDown = false;
            _dragging = false;
            Capture = false;
            Cursor = Cursors.Cross;
            if (!wasDragging && GraphArea.Contains(e.Location))
            {
                _pinnedTime = TimeAtClientX(e.X);
                OnInspectionChanged(_pinnedTime.Value, true);
                Invalidate();
            }
        }

        protected override void OnMouseDoubleClick(MouseEventArgs e)
        {
            base.OnMouseDoubleClick(e);
            if (e.Button != MouseButtons.Left || _analysis == null || !GraphArea.Contains(e.Location)) return;
            _pinnedTime = TimeAtClientX(e.X);
            EventHandler<WorkloadSelectionEventArgs> handler = SeekRequested;
            if (handler != null) handler(this, new WorkloadSelectionEventArgs(_pinnedTime.Value, true));
        }

        private void OnInspectionChanged(long time, bool pinned)
        {
            EventHandler<WorkloadSelectionEventArgs> handler = InspectionChanged;
            if (handler != null) handler(this, new WorkloadSelectionEventArgs(time, pinned));
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.Clear(BackColor);
            if (_analysis == null || _analysis.Buckets == null || _analysis.Buckets.Length == 0) return;
            bool byteModel = UsesByteRate;
            string scope = (_viewStart == 0 && _viewEnd >= _analysis.DurationMicroseconds ? "WHOLE FILE   " : "VISIBLE RANGE   ") +
                FormatClock(_viewStart) + " – " + FormatClock(_viewEnd);
            TextRenderer.DrawText(e.Graphics, scope, _labelFont, new Rectangle(54, 2, ClientSize.Width - 70, 20), Color.Gainsboro,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);

            Rectangle area = GraphArea;
            string title = byteModel ? "MIDI bytes/sec" : "Events/sec";
            string unit = byteModel ? "bytes/sec" : "events/sec";
            double peak = byteModel ? _analysis.PeakBytesPerSecond : _analysis.PeakEventsPerSecond;
            double capacity = byteModel ? _analysis.ByteServiceCapacityPerSecond : _analysis.EventServiceCapacityPerSecond;
            DrawPanel(e.Graphics, area, byteModel, title, unit, peak, capacity);
            DrawLiveMarkers(e.Graphics, area);
            DrawTimelineAxis(e.Graphics, area);
            DrawMarkerLanes(e.Graphics, area);
            DrawInspection(e.Graphics, area, byteModel);
        }

        private void BucketRangeAtPixel(int x, Rectangle area, out int first, out int last)
        {
            long start = _viewStart + (long)((x / (double)Math.Max(1, area.Width)) * (_viewEnd - _viewStart));
            long end = _viewStart + (long)(((x + 1) / (double)Math.Max(1, area.Width)) * (_viewEnd - _viewStart));
            first = (int)Math.Max(0, Math.Min(_analysis.Buckets.Length - 1, start / Math.Max(1, _analysis.BucketMicroseconds)));
            last = (int)Math.Max(first + 1, Math.Min(_analysis.Buckets.Length, end / Math.Max(1, _analysis.BucketMicroseconds) + 1));
        }

        private void DrawPanel(Graphics graphics, Rectangle area, bool bytes, string title, string unit, double peak, double capacity)
        {
            using (Pen grid = new Pen(Color.FromArgb(65, 75, 86)))
            using (Pen plot = new Pen(bytes ? Color.FromArgb(255, 174, 66) : Color.FromArgb(63, 190, 255), 1.5F))
            using (Pen capacityPen = new Pen(Color.FromArgb(220, 110, 220, 130), 1F))
            using (Brush overflow = new SolidBrush(Color.FromArgb(65, 235, 65, 65)))
            {
                capacityPen.DashStyle = DashStyle.Dash;
                graphics.DrawRectangle(grid, area);
                for (int i = 1; i < 4; i++) graphics.DrawLine(grid, area.Left, area.Top + area.Height * i / 4, area.Right, area.Top + area.Height * i / 4);
                double bucketSeconds = _analysis.BucketMicroseconds / 1000000.0;
                Point previous = new Point(area.Left, area.Bottom);
                for (int x = 0; x < area.Width; x++)
                {
                    int first, last;
                    BucketRangeAtPixel(x, area, out first, out last);
                    double value = 0;
                    bool predictedOverflow = false;
                    for (int bucket = first; bucket < last; bucket++)
                    {
                        double current = bytes ? _analysis.Buckets[bucket].ByteCount / bucketSeconds : _analysis.Buckets[bucket].EventCount / bucketSeconds;
                        value = Math.Max(value, current);
                        predictedOverflow |= _analysis.Buckets[bucket].PredictedDroppedEvents > 0;
                    }
                    if (predictedOverflow && _analysis.Configuration != null && _analysis.Configuration.QueueLengthLimitEnabled)
                        graphics.FillRectangle(overflow, area.Left + x, area.Top, 1, area.Height);
                    int y = peak <= 0 ? area.Bottom : area.Bottom - (int)Math.Round((value / peak) * area.Height);
                    Point currentPoint = new Point(area.Left + x, Math.Max(area.Top, Math.Min(area.Bottom, y)));
                    if (x > 0) graphics.DrawLine(plot, previous, currentPoint);
                    previous = currentPoint;
                }
                if (capacity > 0 && peak > 0 && capacity <= peak)
                {
                    int capacityY = area.Bottom - (int)Math.Round((capacity / peak) * area.Height);
                    graphics.DrawLine(capacityPen, area.Left, capacityY, area.Right, capacityY);
                }
            }
            TextRenderer.DrawText(graphics, title, _labelFont, new Rectangle(area.Left, area.Top - 22, area.Width / 2, 20), ForeColor,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            string scale = "peak " + peak.ToString("N1") + " " + unit;
            if (capacity > 0) scale += "   maximum rate " + capacity.ToString("N1") + " " + unit;
            TextRenderer.DrawText(graphics, scale, _labelFont, new Rectangle(area.Left + area.Width / 3, area.Top - 22, area.Width * 2 / 3, 20), ForeColor,
                TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        private void DrawLiveMarkers(Graphics graphics, Rectangle area)
        {
            DrawLiveMarker(graphics, area, _playbackTimeline, Color.FromArgb(225, 205, 225, 255), DashStyle.Dot);
            DrawLiveMarker(graphics, area, _midiOutputPosition, Color.FromArgb(225, 130, 230, 185), DashStyle.DashDot);
        }

        private void DrawLiveMarker(Graphics graphics, Rectangle area, long? time, Color color, DashStyle dash)
        {
            if (!time.HasValue || time.Value < _viewStart || time.Value > _viewEnd) return;
            int x = ClientXAtTime(time.Value);
            using (Pen pen = new Pen(color, 1F)) { pen.DashStyle = dash; graphics.DrawLine(pen, x, area.Top, x, area.Bottom); }
        }

        private void DrawTimelineAxis(Graphics graphics, Rectangle area)
        {
            for (int tick = 0; tick <= 4; tick++)
            {
                int x = area.Left + area.Width * tick / 4;
                long time = _viewStart + (_viewEnd - _viewStart) * tick / 4;
                TextRenderer.DrawText(graphics, FormatClock(time), _labelFont, new Rectangle(x - 42, area.Bottom + 2, 84, 18), Color.Silver,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            }
        }

        private void DrawMarkerLanes(Graphics graphics, Rectangle area)
        {
            int clusterY = area.Bottom + 25;
            int pressureY = area.Bottom + 38;
            using (Brush cluster = new SolidBrush(Color.FromArgb(225, 195, 90, 220)))
            using (Brush low = new SolidBrush(Color.FromArgb(100, 75, 150, 95)))
            using (Brush medium = new SolidBrush(Color.FromArgb(150, 230, 170, 45)))
            using (Brush high = new SolidBrush(Color.FromArgb(190, 230, 65, 65)))
            {
                for (int x = 0; x < area.Width; x++)
                {
                    int first, last;
                    BucketRangeAtPixel(x, area, out first, out last);
                    int clusterSize = 0;
                    int occupancy = 0;
                    for (int bucket = first; bucket < last; bucket++)
                    {
                        clusterSize = Math.Max(clusterSize, _analysis.Buckets[bucket].LargestCluster);
                        occupancy = Math.Max(occupancy, _analysis.Buckets[bucket].PredictedPeakOccupancy);
                    }
                    if (clusterSize >= 50)
                    {
                        Point[] diamond = new Point[] { new Point(area.Left + x, clusterY - 3), new Point(area.Left + x + 3, clusterY), new Point(area.Left + x, clusterY + 3), new Point(area.Left + x - 3, clusterY) };
                        graphics.FillPolygon(cluster, diamond);
                    }
                    if (_analysis.Configuration != null && _analysis.Configuration.QueueLengthLimitEnabled)
                    {
                        double ratio = occupancy / (double)Math.Max(1, _analysis.Configuration.QueueLengthLimit);
                        graphics.FillRectangle(ratio >= 0.9 ? high : ratio >= 0.65 ? medium : low, area.Left + x, pressureY, 1, 6);
                    }
                }
            }
            string legend = "◆ Simultaneous burst ≥50   red shading: predicted overflow";
            if (_analysis.Configuration != null && _analysis.Configuration.QueueLengthLimitEnabled)
                legend += "   pressure strip: finite-buffer occupancy";
            TextRenderer.DrawText(graphics, legend, _labelFont, new Rectangle(area.Left, area.Bottom + 47, area.Width, 18), Color.Silver,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
            string liveLegend = "·· Playback timeline    ·– MIDI output position (last sent event)    fixed " +
                (_analysis.BucketMicroseconds / 1000.0).ToString("N0") + " ms buckets";
            TextRenderer.DrawText(graphics, liveLegend, _labelFont, new Rectangle(area.Left, area.Bottom + 64, area.Width, 18), Color.Silver,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
        }

        private void DrawInspection(Graphics graphics, Rectangle area, bool bytes)
        {
            if (_pinnedTime.HasValue && _pinnedTime.Value >= _viewStart && _pinnedTime.Value <= _viewEnd)
            {
                int pinX = ClientXAtTime(_pinnedTime.Value);
                using (Pen pin = new Pen(Color.White, 1F)) { pin.DashStyle = DashStyle.Dash; graphics.DrawLine(pin, pinX, area.Top, pinX, area.Bottom); }
            }
            long? inspected = _hoverTime ?? _pinnedTime;
            if (!inspected.HasValue || inspected.Value < _viewStart || inspected.Value > _viewEnd) return;
            int x = ClientXAtTime(inspected.Value);
            if (_hoverTime.HasValue)
            {
                using (Pen hover = new Pen(Color.FromArgb(230, 245, 215, 100), 1F)) graphics.DrawLine(hover, x, area.Top, x, area.Bottom);
            }
            WorkloadBucket bucket = BucketAt(inspected.Value);
            double seconds = _analysis.BucketMicroseconds / 1000000.0;
            double value = bucket == null ? 0 : (bytes ? bucket.ByteCount : bucket.EventCount) / seconds;
            string tip = FormatClockDetailed(inspected.Value) + "   " + value.ToString("N1") + (bytes ? " bytes/sec" : " events/sec");
            int tipX = Math.Max(area.Left, Math.Min(area.Right - 215, x + 7));
            using (Brush background = new SolidBrush(Color.FromArgb(220, 40, 44, 50))) graphics.FillRectangle(background, tipX, area.Top + 5, 210, 20);
            TextRenderer.DrawText(graphics, tip, _labelFont, new Rectangle(tipX + 4, area.Top + 5, 204, 20), Color.White,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00") + ":" + value.Seconds.ToString("00");
        }

        internal static string FormatClockDetailed(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00") + ":" + value.Seconds.ToString("00") + "." + value.Milliseconds.ToString("000");
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) { _labelFont.Dispose(); _toolTip.Dispose(); }
            base.Dispose(disposing);
        }
    }
}
