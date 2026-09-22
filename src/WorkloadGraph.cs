using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class PlaybackOverlayData
    {
        public string State;
        public string TimelineAndOutput;
        public string Queue;
        public string Events;
        public string OutputRate;
        public string EffectiveSpeed;
        public string Lag;
    }

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

    internal sealed class TimelineAxisTick
    {
        public long TimeMicroseconds;
        public int X;
        public string Label;
        public Rectangle LabelBounds;
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
        private bool _mouseDownAtEdge;
        private bool _dragging;
        private Point _dragOrigin;
        private long _dragViewStart;
        private Bitmap _staticLayer;
        private long _lastInspectionNotificationTicks;
        private PlaybackOverlayData _overlayData;
        private PlaybackState _playbackState;
        private bool _playbackStatisticsVisible;
        private int _staticLayerBuildCount;
        private readonly Timer _dynamicTimer;
        private Point _pendingPointer;
        private bool _hoverUpdatePending;
        private bool _dynamicRepaintPending;
        private int _dynamicPaintCount;
        private int _lastResolutionPlotWidth;

        public event EventHandler<WorkloadSelectionEventArgs> InspectionChanged;
        public event EventHandler<WorkloadSelectionEventArgs> SeekRequested;
        public event EventHandler ViewportChanged;
        private string _emptyMessage = "No analysis available.";

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
            _dynamicTimer = new Timer();
            _dynamicTimer.Interval = 16;
            _dynamicTimer.Tick += ProcessDynamicUpdates;
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
                InvalidateStaticLayer();
            }
        }

        internal long? PinnedTimeMicroseconds { get { return _pinnedTime; } }
        internal long? HoverTimeMicroseconds { get { return _hoverTime; } }
        internal long? InspectedTimeMicroseconds { get { return _hoverTime ?? _pinnedTime; } }
        internal bool UsesByteRate { get { return _analysis != null && _analysis.Configuration != null && _analysis.Configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate; } }
        internal long ViewStartMicroseconds { get { return _viewStart; } }
        internal long ViewEndMicroseconds { get { return _viewEnd; } }
        internal AnalysisFollowTarget FollowTarget { get { return _followTarget; } set { _followTarget = value; ApplyFollow(); } }
        internal bool PlaybackStatisticsVisible
        {
            get { return _playbackStatisticsVisible; }
            set { if (_playbackStatisticsVisible != value) { _playbackStatisticsVisible = value; Invalidate(); } }
        }
        internal bool PlaybackStatisticsDrawn
        {
            get { return _playbackStatisticsVisible && _overlayData != null && (_playbackState == PlaybackState.Playing || _playbackState == PlaybackState.Paused); }
        }
        internal int StaticLayerBuildCount { get { return _staticLayerBuildCount; } }
        internal long? PlaybackTimelinePosition { get { return _playbackTimeline; } }
        internal long? MidiOutputPosition { get { return _midiOutputPosition; } }
        internal int DynamicPaintCount { get { return _dynamicPaintCount; } }
        internal string EmptyMessage { get { return _emptyMessage; } set { _emptyMessage = value ?? String.Empty; Invalidate(); } }

        internal void DetachAnalysis(string emptyMessage)
        {
            _analysis = null;
            _hoverTime = null;
            _pinnedTime = null;
            _playbackTimeline = null;
            _midiOutputPosition = null;
            _overlayData = null;
            _viewStart = 0;
            _viewEnd = 0;
            _mouseDown = false;
            _dragging = false;
            _hoverUpdatePending = false;
            _dynamicRepaintPending = false;
            _emptyMessage = emptyMessage ?? "No MIDI file is attached.";
            InvalidateStaticLayer();
        }

        internal Rectangle GraphArea
        {
            get
            {
                long span = Math.Max(1, _viewEnd - _viewStart);
                string endLabel = FormatAxisClock(Math.Max(_viewStart, _viewEnd), ChooseTimelineTickInterval(span, Math.Max(100, ClientSize.Width)));
                int horizontalAllowance = Math.Max(42, TextRenderer.MeasureText(endLabel, _labelFont).Width / 2 + 8);
                int top = 42;
                bool finite = _analysis != null && _analysis.HasQueueProjection &&
                    _analysis.Configuration != null && _analysis.Configuration.QueueLengthLimitEnabled;
                bool workloadOnly = _analysis != null && !_analysis.HasQueueProjection;
                bool narrow = ClientSize.Width - horizontalAllowance * 2 < 430;
                int bottomAllowance = finite ? (narrow ? 121 : 104) : (narrow ? 104 : 87);
                if (workloadOnly) bottomAllowance += 17;
                return new Rectangle(horizontalAllowance, top,
                    Math.Max(20, ClientSize.Width - horizontalAllowance * 2),
                    Math.Max(50, ClientSize.Height - top - bottomAllowance));
            }
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

        internal bool TryGetPinTime(Point point, out long timeMicroseconds)
        {
            timeMicroseconds = 0;
            if (_analysis == null || _analysis.DurationMicroseconds <= 0) return false;
            Rectangle area = GraphArea;
            const int edgeTolerance = 6;
            if (point.Y < area.Top || point.Y >= area.Bottom ||
                point.X < area.Left - edgeTolerance || point.X > area.Right + edgeTolerance)
                return false;
            timeMicroseconds = TimeAtClientX(Math.Max(area.Left, Math.Min(area.Right, point.X)));
            return true;
        }

        internal void ResetZoom()
        {
            _viewStart = 0;
            _viewEnd = _analysis == null ? 1 : Math.Max(1, _analysis.DurationMicroseconds);
            InvalidateStaticLayer();
            RaiseViewportChanged();
        }

        internal void ZoomAtClientX(int x, int wheelDelta)
        {
            if (_analysis == null || _analysis.DurationMicroseconds <= 0 || wheelDelta == 0) return;
            long duration = _analysis.DurationMicroseconds;
            long oldSpan = Math.Max(1, _viewEnd - _viewStart);
            double factor = Math.Pow(0.8, wheelDelta / 120.0);
            long minimum = Math.Max(1, duration / 100000);
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
            InvalidateStaticLayer();
            RaiseViewportChanged();
        }

        private void RaiseViewportChanged()
        {
            EventHandler handler = ViewportChanged;
            if (handler != null) handler(this, EventArgs.Empty);
        }

        internal void SetPlaybackPositions(long playbackTimeline, long midiOutputPosition)
        {
            playbackTimeline = Math.Max(0, playbackTimeline);
            midiOutputPosition = Math.Max(0, midiOutputPosition);
            if (_playbackTimeline == playbackTimeline && _midiOutputPosition == midiOutputPosition) return;
            _playbackTimeline = playbackTimeline;
            _midiOutputPosition = midiOutputPosition;
            ApplyFollow();
            RequestDynamicRepaint();
        }

        internal void SetPlaybackOverlay(PlaybackState state, PlaybackOverlayData data)
        {
            _playbackState = state;
            _overlayData = data;
            if (_playbackStatisticsVisible) RequestDynamicRepaint();
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
            if (_mouseDown && !_mouseDownAtEdge)
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
            _pendingPointer = e.Location;
            _hoverUpdatePending = true;
            RequestDynamicRepaint();
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            base.OnMouseLeave(e);
            if (_mouseDown) return;
            _hoverUpdatePending = false;
            _hoverTime = null;
            if (_pinnedTime.HasValue) OnInspectionChanged(_pinnedTime.Value, true);
            RequestDynamicRepaint();
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            long ignored;
            if (e.Button != MouseButtons.Left || !TryGetPinTime(e.Location, out ignored)) return;
            _mouseDown = true;
            _mouseDownAtEdge = !GraphArea.Contains(e.Location);
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
            _mouseDownAtEdge = false;
            _dragging = false;
            Capture = false;
            Cursor = Cursors.Cross;
            long pinTime;
            if (!wasDragging && TryGetPinTime(e.Location, out pinTime))
            {
                _pinnedTime = pinTime;
                OnInspectionChanged(_pinnedTime.Value, true);
                Invalidate();
            }
        }

        protected override void OnMouseDoubleClick(MouseEventArgs e)
        {
            base.OnMouseDoubleClick(e);
            long pinTime;
            if (e.Button != MouseButtons.Left || !TryGetPinTime(e.Location, out pinTime)) return;
            _pinnedTime = pinTime;
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
            _dynamicPaintCount++;
            base.OnPaint(e);
            if (_analysis == null || _analysis.Buckets == null || _analysis.Buckets.Length == 0)
            {
                e.Graphics.Clear(BackColor);
                TextRenderer.DrawText(e.Graphics, _emptyMessage, _labelFont, ClientRectangle, Color.Silver,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
                return;
            }
            EnsureStaticLayer();
            e.Graphics.DrawImageUnscaled(_staticLayer, Point.Empty);
            bool byteModel = UsesByteRate;
            Rectangle area = GraphArea;
            DrawLiveMarkers(e.Graphics, area);
            DrawPlaybackStatistics(e.Graphics, area);
            DrawInspection(e.Graphics, area, byteModel);
        }

        private void RequestDynamicRepaint()
        {
            _dynamicRepaintPending = true;
            if (!_dynamicTimer.Enabled) _dynamicTimer.Start();
        }

        private void ProcessDynamicUpdates(object sender, EventArgs e)
        {
            if (IsDisposed || !IsHandleCreated) return;
            if (_hoverUpdatePending && _analysis != null)
            {
                long time = TimeAtClientX(_pendingPointer.X);
                _hoverTime = time;
                long now = Stopwatch.GetTimestamp();
                if (_lastInspectionNotificationTicks == 0 || now - _lastInspectionNotificationTicks >= Stopwatch.Frequency / 30)
                {
                    _hoverUpdatePending = false;
                    _lastInspectionNotificationTicks = now;
                    OnInspectionChanged(time, false);
                }
            }
            if (_dynamicRepaintPending)
            {
                _dynamicRepaintPending = false;
                Invalidate();
            }
            if (!_hoverUpdatePending && !_dynamicRepaintPending) _dynamicTimer.Stop();
        }

        private void EnsureStaticLayer()
        {
            if (_staticLayer != null && _staticLayer.Width == Math.Max(1, ClientSize.Width) && _staticLayer.Height == Math.Max(1, ClientSize.Height)) return;
            if (_staticLayer != null) _staticLayer.Dispose();
            _staticLayer = new Bitmap(Math.Max(1, ClientSize.Width), Math.Max(1, ClientSize.Height));
            _staticLayerBuildCount++;
            using (Graphics graphics = Graphics.FromImage(_staticLayer))
            {
                graphics.Clear(BackColor);
                bool byteModel = UsesByteRate;
                string scope = (_viewStart == 0 && _viewEnd >= _analysis.DurationMicroseconds ? "WHOLE FILE   " : "VISIBLE RANGE   ") +
                    FormatClock(_viewStart) + " – " + FormatClock(_viewEnd);
                Rectangle area = GraphArea;
                TextRenderer.DrawText(graphics, scope, _labelFont, new Rectangle(area.Left, 5, area.Width, 20), Color.Gainsboro,
                    TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
                string title = byteModel ? "MIDI bytes/sec" : "Events/sec";
                string unit = byteModel ? "bytes/sec" : "events/sec";
                double peak = byteModel ? _analysis.PeakBytesPerSecond : _analysis.PeakEventsPerSecond;
                double capacity = _analysis.HasQueueProjection
                    ? (byteModel ? _analysis.ByteServiceCapacityPerSecond : _analysis.EventServiceCapacityPerSecond) : 0;
                DrawPanel(graphics, area, byteModel, title, unit, peak, capacity);
                DrawTimelineAxis(graphics, area);
                DrawMarkerLanes(graphics, area);
            }
        }

        private void InvalidateStaticLayer()
        {
            if (_staticLayer != null) { _staticLayer.Dispose(); _staticLayer = null; }
            Invalidate();
        }

        protected override void OnResize(EventArgs e)
        {
            InvalidateStaticLayer();
            base.OnResize(e);
            // Auto resolution depends on horizontal plot density, not height.
            // Busy/status controls can alter the graph height when they hide;
            // treating that layout-only resize as navigation restarted a
            // calculation immediately after the user pressed Cancel.
            int plotWidth = GraphArea.Width;
            if (plotWidth != _lastResolutionPlotWidth)
            {
                _lastResolutionPlotWidth = plotWidth;
                RaiseViewportChanged();
            }
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
                        predictedOverflow |= _analysis.HasQueueProjection &&
                            _analysis.Buckets[bucket].PredictedDroppedEvents > 0;
                    }
                    if (predictedOverflow && _analysis.HasQueueProjection && _analysis.Configuration != null &&
                        _analysis.Configuration.QueueLengthLimitEnabled)
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
            TextRenderer.DrawText(graphics, title, _labelFont, new Rectangle(area.Left, area.Top - 22, area.Width / 3, 20), ForeColor,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
            string scale;
            if (area.Width < 430)
            {
                scale = "peak " + peak.ToString("N0");
                if (capacity > 0) scale += "  •  max " + capacity.ToString("N0") + "/s";
            }
            else
            {
                scale = "peak " + peak.ToString("N1") + " " + unit;
                if (capacity > 0) scale += "   maximum rate " + capacity.ToString("N1") + " " + unit;
            }
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
            System.Collections.Generic.IList<TimelineAxisTick> ticks = BuildTimelineTicks(area);
            using (Pen tickPen = new Pen(Color.FromArgb(155, 165, 175), 1F))
            {
                for (int index = 0; index < ticks.Count; index++)
                {
                    TimelineAxisTick tick = ticks[index];
                    graphics.DrawLine(tickPen, tick.X, area.Bottom + 1, tick.X, area.Bottom + 4);
                    TextRenderer.DrawText(graphics, tick.Label, _labelFont, tick.LabelBounds, Color.Silver,
                        TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
                }
            }
        }

        internal System.Collections.Generic.IList<TimelineAxisTick> TimelineTicks
        {
            get { return BuildTimelineTicks(GraphArea); }
        }

        private System.Collections.Generic.IList<TimelineAxisTick> BuildTimelineTicks(Rectangle area)
        {
            System.Collections.Generic.List<TimelineAxisTick> result = new System.Collections.Generic.List<TimelineAxisTick>();
            if (_viewEnd <= _viewStart || area.Width <= 0) return result;
            long span = _viewEnd - _viewStart;
            long interval = ChooseTimelineTickInterval(span, area.Width);
            long remainder = _viewStart % interval;
            long first = remainder == 0 ? _viewStart : checked(_viewStart + (interval - remainder));
            const int safetyGap = 10;
            int previousRight = Int32.MinValue;
            for (long time = first; time <= _viewEnd; )
            {
                string label = FormatAxisClock(time, interval);
                Size measured = TextRenderer.MeasureText(label, _labelFont, Size.Empty,
                    TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
                int x = area.Left + (int)Math.Round(area.Width * (time - _viewStart) / (double)span);
                Rectangle bounds = new Rectangle(x - measured.Width / 2, area.Bottom + 5, measured.Width, Math.Max(16, measured.Height));
                if (bounds.Left >= 2 && bounds.Right <= ClientSize.Width - 2 && bounds.Left >= previousRight + safetyGap)
                {
                    result.Add(new TimelineAxisTick { TimeMicroseconds = time, X = x, Label = label, LabelBounds = bounds });
                    previousRight = bounds.Right;
                }
                if (time > Int64.MaxValue - interval) break;
                time += interval;
            }
            return result;
        }

        internal static long ChooseTimelineTickInterval(long visibleSpanMicroseconds, int plotWidth)
        {
            long[] nice = new long[]
            {
                1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
                1000000, 2000000, 5000000, 10000000, 15000000, 30000000,
                60000000, 120000000, 300000000, 600000000, 900000000,
                1800000000, 3600000000, 7200000000, 18000000000, 36000000000
            };
            visibleSpanMicroseconds = Math.Max(1, visibleSpanMicroseconds);
            int targetTicks = Math.Max(2, plotWidth / 76);
            long minimum = Math.Max(1, (visibleSpanMicroseconds + targetTicks - 1) / targetTicks);
            for (int i = 0; i < nice.Length; i++) if (nice[i] >= minimum) return nice[i];
            long hours = 3600000000L;
            long multiples = (minimum + hours - 1) / hours;
            return checked(Math.Max(1, multiples) * hours);
        }

        internal static string FormatAxisClock(long microseconds, long intervalMicroseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            bool hours = value.TotalHours >= 1;
            string prefix = hours
                ? ((long)value.TotalHours).ToString("00") + ":" + value.Minutes.ToString("00") + ":" + value.Seconds.ToString("00")
                : ((long)value.TotalMinutes).ToString("00") + ":" + value.Seconds.ToString("00");
            if (intervalMicroseconds < 1000000)
                return prefix + "." + value.Milliseconds.ToString("000");
            return prefix;
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
                    if (_analysis.HasQueueProjection && _analysis.Configuration != null &&
                        _analysis.Configuration.QueueLengthLimitEnabled)
                    {
                        double ratio = occupancy / (double)Math.Max(1, _analysis.Configuration.QueueLengthLimit);
                        graphics.FillRectangle(ratio >= 0.9 ? high : ratio >= 0.65 ? medium : low, area.Left + x, pressureY, 1, 6);
                    }
                }
            }
            bool finite = _analysis.HasQueueProjection && _analysis.Configuration != null &&
                _analysis.Configuration.QueueLengthLimitEnabled;
            bool narrow = area.Width < 430;
            if (narrow)
            {
                DrawLegendLine(graphics, area, 47, "◆ Magenta: burst ≥50 simultaneous events");
                int next = 64;
                if (finite)
                {
                    DrawLegendLine(graphics, area, next, "Red area: overflow  •  Strip: queue pressure");
                    next += 17;
                }
                DrawLegendLine(graphics, area, next, "White dots: playback timeline");
                DrawLegendLine(graphics, area, next + 17, "Green dash-dot: MIDI output  •  " + FormatResolution(_analysis.BucketMicroseconds));
                if (!_analysis.HasQueueProjection)
                    DrawLegendLine(graphics, area, next + 34,
                        _analysis.ProjectionState == AnalysisProjectionState.Pending
                            ? "Workload only — queue projection pending"
                            : _analysis.ProjectionState == AnalysisProjectionState.Cancelled
                                ? "Workload only — projection cancelled"
                                : "Workload only — queue projection failed");
                return;
            }

            DrawLegendLine(graphics, area, 47, "Magenta diamond: ≥50 simultaneous events");
            if (finite)
                DrawLegendLine(graphics, area, 64, "Red: predicted overflow  •  Strip: predicted queue pressure");
            string liveLegend = "White dots: playback timeline  •  Green dash-dot: MIDI output  •  " +
                FormatResolution(_analysis.BucketMicroseconds);
            DrawLegendLine(graphics, area, finite ? 81 : 64, liveLegend);
            if (!_analysis.HasQueueProjection)
                DrawLegendLine(graphics, area, finite ? 98 : 81,
                    _analysis.ProjectionState == AnalysisProjectionState.Pending
                        ? "Workload only — queue projection pending"
                        : _analysis.ProjectionState == AnalysisProjectionState.Cancelled
                            ? "Workload only — projection cancelled"
                            : "Workload only — queue projection failed");
        }

        private void DrawLegendLine(Graphics graphics, Rectangle area, int offset, string text)
        {
            TextRenderer.DrawText(graphics, text, _labelFont, new Rectangle(area.Left, area.Bottom + offset, area.Width, 18), Color.Silver,
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
            int tipY = area.Top + 5;
            Rectangle overlay = PlaybackStatisticsBounds(area);
            if (PlaybackStatisticsDrawn && new Rectangle(tipX, tipY, 210, 20).IntersectsWith(overlay))
                tipY = Math.Min(area.Bottom - 20, overlay.Bottom + 5);
            using (Brush background = new SolidBrush(Color.FromArgb(220, 40, 44, 50))) graphics.FillRectangle(background, tipX, tipY, 210, 20);
            TextRenderer.DrawText(graphics, tip, _labelFont, new Rectangle(tipX + 4, tipY, 204, 20), Color.White,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        private void DrawPlaybackStatistics(Graphics graphics, Rectangle area)
        {
            if (!PlaybackStatisticsDrawn) return;
            string[] lines = new string[]
            {
                "Playback: " + _overlayData.State,
                "Timeline / output: " + _overlayData.TimelineAndOutput,
                "Queue now / maximum: " + _overlayData.Queue,
                "Events sent / dropped: " + _overlayData.Events,
                "Output rate: " + _overlayData.OutputRate,
                "Effective speed: " + _overlayData.EffectiveSpeed,
                "Lag current / maximum: " + _overlayData.Lag
            };
            Rectangle panel = PlaybackStatisticsBounds(area);
            using (Brush background = new SolidBrush(Color.FromArgb(218, 12, 15, 19)))
            using (Pen outline = new Pen(Color.FromArgb(130, 190, 195, 202)))
            {
                graphics.FillRectangle(background, panel);
                graphics.DrawRectangle(outline, panel);
            }
            for (int i = 0; i < lines.Length; i++)
                TextRenderer.DrawText(graphics, lines[i], _labelFont,
                    new Rectangle(panel.Left + 7, panel.Top + 4 + i * 17, panel.Width - 14, 17), Color.WhiteSmoke,
                    TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
        }

        internal Rectangle PlaybackStatisticsBounds(Rectangle area)
        {
            int width = Math.Min(310, Math.Max(220, area.Width - 16));
            int height = 8 + 7 * 17;
            return new Rectangle(area.Left + 7, area.Top + 7, width, height);
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00") + ":" + value.Seconds.ToString("00");
        }

        private static string FormatResolution(long microseconds)
        {
            return microseconds >= 1000000 ? (microseconds / 1000000.0).ToString("0.###") + " s" :
                (microseconds / 1000.0).ToString("0.###") + " ms";
        }

        internal static string FormatClockDetailed(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00") + ":" + value.Seconds.ToString("00") + "." + value.Milliseconds.ToString("000");
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) { _dynamicTimer.Stop(); _dynamicTimer.Dispose(); if (_staticLayer != null) _staticLayer.Dispose(); _labelFont.Dispose(); _toolTip.Dispose(); }
            base.Dispose(disposing);
        }
    }
}
