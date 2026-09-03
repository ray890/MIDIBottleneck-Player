using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class StatisticsView : Control
    {
        private static readonly string[] Captions = new string[]
        {
            "Timeline / output:", "Queue now / maximum:",
            "Maximum rate:", "Output rate:",
            "Events sent / dropped:", "Effective speed:",
            "Maximum lag:", "Current lag:"
        };
        private static readonly string[] CompactCaptions = new string[]
        {
            "Timeline / output:", "Queue now / max:",
            "Maximum rate:", "Output rate:",
            "Sent / dropped:", "Effective speed:",
            "Maximum lag:", "Current lag:"
        };

        private readonly string[] _values = new string[8];
        private readonly Font _captionFont;
        private readonly Font _valueFont;
        private readonly ToolTip _toolTip;
        private readonly bool[] _captionTruncated = new bool[8];
        private readonly bool[] _valueTruncated = new bool[8];
        private readonly int[] _lastValueWidths = new int[8];
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
            set
            {
                if (_compact == value) return;
                _compact = value;
                Height = value ? 96 : 104;
                MinimumSize = new Size(0, Height);
                Invalidate();
            }
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
            if (changed) _lastToolTipCell = -1;
            return changed;
        }

        internal void SetQueuePressure(bool limited, long occupied, long limit, bool overflowPulse)
        {
            occupied = Math.Max(0, occupied);
            limit = Math.Max(1, limit);
            bool visibilityChanged = _queueLimited != limited;
            if (!visibilityChanged && _occupied == occupied && _limit == limit && _overflowPulse == overflowPulse) return;
            _queueLimited = limited;
            _occupied = occupied;
            _limit = limit;
            _overflowPulse = overflowPulse;
            Invalidate();
        }

        internal bool UsesDoubleBuffer { get { return DoubleBuffered; } }
        internal bool QueuePressureVisible { get { return _queueLimited; } }
        internal double QueuePressureRatio { get { return _queueLimited ? Math.Min(1.0, _occupied / (double)Math.Max(1, _limit)) : 0; } }
        internal int ColumnCount { get { return 2; } }
        internal static string CaptionAt(int index) { return Captions[index]; }
        internal bool ValueWasTruncated(int index) { return _valueTruncated[index]; }
        internal int ValueAllocationWidth(int index) { return _lastValueWidths[index]; }
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
            int rowHeight = Math.Max(20, contentHeight / 4);
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
            int gap = _compact ? 3 : 6;
            TextFormatFlags vertical = TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
            string captionText = _compact ? CompactCaptions[index] : Captions[index];
            int measuredValue = TextRenderer.MeasureText(graphics, _values[index], _valueFont, Size.Empty,
                vertical).Width + 2;
            int measuredCaption = TextRenderer.MeasureText(graphics, captionText, _captionFont, Size.Empty,
                vertical).Width + 2;
            int innerWidth = Math.Max(1, width - 6);
            int minimumCaption = _compact ? 45 : 60;
            int valueWidth = Math.Min(measuredValue, Math.Max(24, innerWidth - minimumCaption - gap));
            _lastValueWidths[index] = valueWidth;
            int captionWidth = Math.Max(1, innerWidth - valueWidth - gap);
            Rectangle caption = new Rectangle(left + 3, row * rowHeight, captionWidth, rowHeight);
            Rectangle value = new Rectangle(caption.Right + gap, row * rowHeight, valueWidth, rowHeight);
            _captionTruncated[index] = measuredCaption > caption.Width;
            _valueTruncated[index] = measuredValue > value.Width;
            TextRenderer.DrawText(graphics, captionText, _captionFont, caption, ForeColor,
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
            string text = cell == 5 ? "Output-timeline advancement relative to elapsed playback time. Current measurement window: " +
                _speedMeasurementDescription + ". Right-click to change it." :
                cell == 0 ? "Playback timeline / MIDI output position. The output position is the source timestamp of the most recently sent MIDI event." :
                cell == 6 || cell == 7 ? "Lag is lateness through MIDI dispatch, including scheduler delay or a blocking output call. It cannot measure synthesizer rendering or audio-device latency." : String.Empty;
            if (cell >= 0 && (_captionTruncated[cell] || _valueTruncated[cell]))
            {
                string full = Captions[cell] + " " + _values[cell];
                text = String.IsNullOrEmpty(text) ? full : full + Environment.NewLine + text;
            }
            _toolTip.SetToolTip(this, text);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (e.Button == MouseButtons.Right && CellAt(e.Location) == 5)
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
