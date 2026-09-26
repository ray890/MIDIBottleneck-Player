using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class TimelineSeekEventArgs : EventArgs
    {
        public readonly long PositionMicroseconds;
        public TimelineSeekEventArgs(long positionMicroseconds) { PositionMicroseconds = positionMicroseconds; }
    }

    internal sealed class PlaybackTimelineView : Control
    {
        private Font _timeFont;
        private long _positionMicroseconds;
        private long _durationMicroseconds;
        private long _dragPositionMicroseconds;
        private bool _dragging;
        private int _applicationScalePercent = 100;

        public event EventHandler<TimelineSeekEventArgs> SeekRequested;

        public PlaybackTimelineView()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.Selectable, true);
            DoubleBuffered = true;
            TabStop = true;
            Height = 42;
            MinimumSize = new Size(300, 42);
            _timeFont = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point);
        }

        internal int ApplicationScalePercent
        {
            set
            {
                value = Math.Max(50, Math.Min(200, value));
                _applicationScalePercent = value;
                Font replacement = new Font("Consolas", 9F * value / 100F,
                    FontStyle.Regular, GraphicsUnit.Point);
                Font previous = _timeFont;
                _timeFont = replacement;
                if (previous != null) previous.Dispose();
                Invalidate();
            }
        }

        public bool IsDragging { get { return _dragging; } }
        public bool UsesDoubleBuffer { get { return DoubleBuffered; } }
        public long PositionMicroseconds { get { return _dragging ? _dragPositionMicroseconds : _positionMicroseconds; } }
        public long DurationMicroseconds { get { return _durationMicroseconds; } }

        public bool SetTimeline(long positionMicroseconds, long durationMicroseconds)
        {
            durationMicroseconds = Math.Max(0, durationMicroseconds);
            positionMicroseconds = Math.Max(0, Math.Min(durationMicroseconds, positionMicroseconds));
            if (_positionMicroseconds == positionMicroseconds && _durationMicroseconds == durationMicroseconds)
                return false;
            _positionMicroseconds = positionMicroseconds;
            _durationMicroseconds = durationMicroseconds;
            if (!_dragging) _dragPositionMicroseconds = positionMicroseconds;
            Invalidate();
            return true;
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left || !Enabled) return;
            Focus();
            Capture = true;
            _dragging = true;
            _dragPositionMicroseconds = PositionFromX(e.X);
            Invalidate();
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (!_dragging) return;
            long position = PositionFromX(e.X);
            if (position != _dragPositionMicroseconds)
            {
                _dragPositionMicroseconds = position;
                Invalidate();
            }
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (!_dragging || e.Button != MouseButtons.Left) return;
            _dragPositionMicroseconds = PositionFromX(e.X);
            _positionMicroseconds = _dragPositionMicroseconds;
            _dragging = false;
            Capture = false;
            Invalidate();
            RaiseSeekRequested();
        }

        protected override bool IsInputKey(Keys keyData)
        {
            Keys key = keyData & Keys.KeyCode;
            if (key == Keys.Left || key == Keys.Right || key == Keys.Home || key == Keys.End || key == Keys.PageUp || key == Keys.PageDown)
                return true;
            return base.IsInputKey(keyData);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if (!Enabled) return;
            long position = _positionMicroseconds;
            if (e.KeyCode == Keys.Home) position = 0;
            else if (e.KeyCode == Keys.End) position = _durationMicroseconds;
            else if (e.KeyCode == Keys.Left) position -= 1000000;
            else if (e.KeyCode == Keys.Right) position += 1000000;
            else if (e.KeyCode == Keys.PageDown) position -= 5000000;
            else if (e.KeyCode == Keys.PageUp) position += 5000000;
            else return;
            _positionMicroseconds = Math.Max(0, Math.Min(_durationMicroseconds, position));
            _dragPositionMicroseconds = _positionMicroseconds;
            Invalidate();
            e.Handled = true;
            RaiseSeekRequested();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.Clear(BackColor);
            int timeWidth = ScaleMetric(168);
            int trackLeft = ScaleMetric(8);
            int trackHeight = ScaleMetric(8);
            Rectangle track = new Rectangle(trackLeft, (ClientSize.Height - trackHeight) / 2,
                Math.Max(ScaleMetric(20), ClientSize.Width - timeWidth - ScaleMetric(18)), trackHeight);
            long shownPosition = PositionMicroseconds;
            double fraction = _durationMicroseconds <= 0 ? 0 : Math.Max(0, Math.Min(1, (double)shownPosition / _durationMicroseconds));
            int fillWidth = (int)Math.Round(track.Width * fraction);

            using (Brush background = new SolidBrush(Enabled ? Color.FromArgb(205, 209, 214) : Color.FromArgb(225, 225, 225)))
            using (Brush fill = new SolidBrush(Enabled ? Color.FromArgb(52, 120, 214) : Color.FromArgb(160, 170, 180)))
            using (Pen border = new Pen(Color.FromArgb(125, 130, 138)))
            {
                e.Graphics.FillRectangle(background, track);
                if (fillWidth > 0) e.Graphics.FillRectangle(fill, new Rectangle(track.X, track.Y, fillWidth, track.Height));
                e.Graphics.DrawRectangle(border, track);
                int thumbX = track.X + fillWidth;
                int thumbWidth = ScaleMetric(12);
                int thumbHeight = ScaleMetric(16);
                e.Graphics.FillEllipse(fill, thumbX - (thumbWidth / 2),
                    track.Y + ((track.Height - thumbHeight) / 2), thumbWidth, thumbHeight);
            }

            Rectangle timeRectangle = new Rectangle(ClientSize.Width - timeWidth, 0,
                timeWidth - ScaleMetric(4), ClientSize.Height);
            string time = FormatTime(shownPosition) + " / " + FormatTime(_durationMicroseconds);
            TextRenderer.DrawText(e.Graphics, time, _timeFont, timeRectangle, ForeColor,
                TextFormatFlags.Right | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);

            if (Focused && ShowFocusCues)
                ControlPaint.DrawFocusRectangle(e.Graphics, new Rectangle(1, 1, ClientSize.Width - 3, ClientSize.Height - 3));
        }

        private long PositionFromX(int x)
        {
            int timeWidth = ScaleMetric(168);
            int left = ScaleMetric(8);
            int width = Math.Max(ScaleMetric(20), ClientSize.Width - timeWidth - ScaleMetric(18));
            double fraction = Math.Max(0, Math.Min(1, (double)(x - left) / width));
            return (long)Math.Round(_durationMicroseconds * fraction);
        }

        private int ScaleMetric(int value)
        {
            return Math.Max(1, (value * _applicationScalePercent + 50) / 100);
        }

        private void RaiseSeekRequested()
        {
            EventHandler<TimelineSeekEventArgs> handler = SeekRequested;
            if (handler != null) handler(this, new TimelineSeekEventArgs(_positionMicroseconds));
        }

        private static string FormatTime(long microseconds)
        {
            if (microseconds < 0) microseconds = 0;
            TimeSpan value = TimeSpan.FromTicks(microseconds * 10);
            int totalMinutes = (int)value.TotalMinutes;
            return String.Format(CultureInfo.CurrentCulture, "{0:00}:{1:00}.{2:000}", totalMinutes, value.Seconds, value.Milliseconds);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) _timeFont.Dispose();
            base.Dispose(disposing);
        }
    }
}
