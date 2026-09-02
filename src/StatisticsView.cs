using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class StatisticsView : Control
    {
        private static readonly string[] Captions = new string[]
        {
            "Theoretical maximum rate:", "Queue current / maximum:",
            "Events processed / dropped:", "Effective playback speed:",
            "Current simulated lag:", "Maximum simulated lag:",
            "Timeline / MIDI output:", "Current output rate:"
        };
        private static readonly string[] CompactCaptions = new string[]
        {
            "Maximum rate:", "Queue now / max:",
            "Events sent / dropped:", "Effective speed:",
            "Current lag:", "Maximum lag:",
            "Timeline / output:", "Output rate:"
        };

        private readonly string[] _values = new string[8];
        private readonly Font _captionFont;
        private readonly Font _valueFont;
        private readonly ToolTip _toolTip;
        private bool _compact;
        private bool _queueLimited;
        private long _occupied;
        private long _limit;
        private bool _overflowPulse;
        private int _lastToolTipCell = -1;
        private string _speedMeasurementDescription = "250 ms";

        internal event EventHandler<MouseEventArgs> EffectiveSpeedContextRequested;

        public StatisticsView()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;
            _captionFont = new Font("Segoe UI", 8.5F, FontStyle.Regular, GraphicsUnit.Point);
            _valueFont = new Font("Consolas", 9F, FontStyle.Bold, GraphicsUnit.Point);
            _toolTip = new ToolTip();
            for (int i = 0; i < _values.Length; i++) _values[i] = "—";
            Height = 104;
            MinimumSize = new Size(0, 104);
            TabStop = false;
        }

        internal bool Compact
        {
            get { return _compact; }
            set { if (_compact != value) { _compact = value; Invalidate(); } }
        }

        public bool SetValues(string[] values)
        {
            if (values == null || values.Length != _values.Length)
                throw new ArgumentException("Exactly eight statistic values are required.", "values");
            bool changed = false;
            for (int i = 0; i < _values.Length; i++)
            {
                string value = values[i] ?? String.Empty;
                if (!String.Equals(_values[i], value, StringComparison.Ordinal))
                {
                    _values[i] = value;
                    changed = true;
                }
            }
            if (changed) Invalidate();
            return changed;
        }

        internal void SetQueuePressure(bool limited, long occupied, long limit, bool overflowPulse)
        {
            occupied = Math.Max(0, occupied);
            limit = Math.Max(1, limit);
            bool heightChanged = _queueLimited != limited;
            if (!heightChanged && _occupied == occupied && _limit == limit && _overflowPulse == overflowPulse) return;
            _queueLimited = limited;
            _occupied = occupied;
            _limit = limit;
            _overflowPulse = overflowPulse;
            if (heightChanged)
            {
                Height = limited ? 122 : 104;
                MinimumSize = new Size(0, Height);
                if (Parent != null) Parent.PerformLayout();
            }
            Invalidate();
        }

        internal bool UsesDoubleBuffer { get { return DoubleBuffered; } }
        internal bool QueuePressureVisible { get { return _queueLimited; } }
        internal double QueuePressureRatio { get { return _queueLimited ? Math.Min(1.0, _occupied / (double)Math.Max(1, _limit)) : 0; } }
        internal int ColumnCount { get { return 2; } }
        internal string SpeedMeasurementDescription
        {
            get { return _speedMeasurementDescription; }
            set { _speedMeasurementDescription = String.IsNullOrEmpty(value) ? "250 ms" : value; }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.Clear(BackColor);
            int columnWidth = Math.Max(1, ClientSize.Width / 2);
            int contentHeight = Math.Max(1, ClientSize.Height - (_queueLimited ? 18 : 0));
            int rowHeight = Math.Max(22, contentHeight / 4);
            for (int index = 0; index < _values.Length; index++)
            {
                int column = index % 2;
                int row = index / 2;
                DrawCell(e.Graphics, column * columnWidth, index, row, columnWidth, rowHeight);
            }
            if (_queueLimited) DrawQueuePressure(e.Graphics);
        }

        private void DrawCell(Graphics graphics, int left, int index, int row, int width, int rowHeight)
        {
            int valueWidth = index == 6 ? Math.Min(190, width * 3 / 5) :
                index == 0 || index == 7 ? Math.Min(170, width / 2) : Math.Min(145, width / 2);
            int gap = _compact ? 3 : 6;
            Rectangle caption = new Rectangle(left + 3, row * rowHeight, Math.Max(55, width - valueWidth - gap - 5), rowHeight);
            Rectangle value = new Rectangle(left + width - valueWidth - 3, row * rowHeight, valueWidth, rowHeight);
            TextFormatFlags vertical = TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
            TextRenderer.DrawText(graphics, _compact ? CompactCaptions[index] : Captions[index], _captionFont, caption, ForeColor,
                vertical | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            TextRenderer.DrawText(graphics, _values[index], _valueFont, value, ForeColor,
                vertical | TextFormatFlags.Right | TextFormatFlags.EndEllipsis);
        }

        private int CellAt(Point point)
        {
            int contentHeight = ClientSize.Height - (_queueLimited ? 18 : 0);
            if (point.Y < 0 || point.Y >= contentHeight || point.X < 0 || point.X >= ClientSize.Width) return -1;
            int column = point.X < ClientSize.Width / 2 ? 0 : 1;
            int row = Math.Min(3, point.Y * 4 / Math.Max(1, contentHeight));
            return row * 2 + column;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            int cell = CellAt(e.Location);
            if (cell == _lastToolTipCell) return;
            _lastToolTipCell = cell;
            string text = cell == 3 ? "Output-timeline advancement relative to elapsed playback time. Current measurement window: " +
                _speedMeasurementDescription + ". Right-click to change it." :
                cell == 6 ? "Playback timeline / MIDI output position. The output position is the source timestamp of the most recently sent MIDI event." : String.Empty;
            _toolTip.SetToolTip(this, text);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (e.Button == MouseButtons.Right && CellAt(e.Location) == 3)
            {
                EventHandler<MouseEventArgs> handler = EffectiveSpeedContextRequested;
                if (handler != null) handler(this, e);
            }
        }

        private void DrawQueuePressure(Graphics graphics)
        {
            Rectangle bar = new Rectangle(3, ClientSize.Height - 14, Math.Max(10, ClientSize.Width - 150), 9);
            double ratio = QueuePressureRatio;
            Color color = _overflowPulse || ratio >= 0.9 ? Color.FromArgb(210, 70, 70) :
                ratio >= 0.65 ? Color.FromArgb(215, 155, 35) : Color.FromArgb(65, 165, 90);
            using (Brush background = new SolidBrush(Color.FromArgb(225, 228, 232)))
            using (Brush fill = new SolidBrush(color))
            using (Pen outline = new Pen(Color.FromArgb(155, 160, 166)))
            {
                graphics.FillRectangle(background, bar);
                graphics.FillRectangle(fill, new Rectangle(bar.X, bar.Y, (int)Math.Round(bar.Width * ratio), bar.Height));
                graphics.DrawRectangle(outline, bar);
            }
            string text = _occupied.ToString("N0") + " / " + _limit.ToString("N0") + " — " + Math.Round(100 * ratio).ToString("N0") + "%";
            TextRenderer.DrawText(graphics, text, _valueFont,
                new Rectangle(bar.Right + 7, bar.Y - 4, Math.Max(110, ClientSize.Width - bar.Right - 10), 18), color,
                TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _captionFont.Dispose();
                _valueFont.Dispose();
                _toolTip.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}
