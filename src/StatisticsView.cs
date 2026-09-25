using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class StatisticsView : Control
    {
        private const string CompactSlash = "\u2009/\u2009";
        private static readonly string[] Captions = new string[]
        {
            "Timeline / output:", "Queue now / maximum:",
            "Maximum rate:", "Output rate:",
            "Events sent / dropped:", "Effective speed:",
            "Maximum lag:", "Current lag:"
        };
        private static readonly string[][] CompactCaptionChoices = new string[][]
        {
            new string[] { "Timeline/output:", "Timeline:", "Time:" },
            new string[] { "Queue now/max:", "Queue:" },
            new string[] { "Max rate:", "Rate:" },
            new string[] { "Output rate:", "Output:" },
            new string[] { "Sent/dropped:", "Events:" },
            new string[] { "Effective speed:", "Speed:" },
            new string[] { "Max lag:" },
            new string[] { "Current lag:" }
        };
        private static readonly string[] PerNoteCompactEventCaptions =
            new string[] { "Sent/excluded:", "Events:" };

        private readonly string[] _values = new string[8];
        private readonly Font _captionFont;
        private readonly Font _valueFont;
        private readonly ToolTip _toolTip;
        private readonly bool[] _captionTruncated = new bool[8];
        private readonly bool[] _valueTruncated = new bool[8];
        private readonly int[] _lastValueWidths = new int[8];
        private readonly string[] _selectedCaptions = new string[8];
        private bool _compact;
        private bool _queueLimited;
        private long _occupied;
        private long _limit;
        private bool _overflowPulse;
        private int _lastToolTipCell = -1;
        private string _speedMeasurementDescription = "250 ms";
        private bool _perNoteIntervalGate;
        private bool _virtualQueuePressure;

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
                int height = value ? Math.Max(84, 4 * (Math.Max(_captionFont.Height, _valueFont.Height) + 1) + 18) : 104;
                // Lower the constraint first: assigning Height while the old
                // 104-pixel minimum is active silently restores that old height.
                MinimumSize = new Size(0, height);
                Height = height;
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
            SetQueuePressure(limited, occupied, limit, overflowPulse, false);
        }

        internal void SetQueuePressure(bool limited, long occupied, long limit, bool overflowPulse,
            bool virtualQueuePressure)
        {
            occupied = Math.Max(0, occupied);
            limit = Math.Max(1, limit);
            bool visibilityChanged = _queueLimited != limited;
            if (!visibilityChanged && _occupied == occupied && _limit == limit &&
                _overflowPulse == overflowPulse && _virtualQueuePressure == virtualQueuePressure) return;
            _queueLimited = limited;
            _occupied = occupied;
            _limit = limit;
            _overflowPulse = overflowPulse;
            _virtualQueuePressure = virtualQueuePressure;
            Invalidate();
        }

        internal bool UsesDoubleBuffer { get { return DoubleBuffered; } }
        internal bool QueuePressureVisible { get { return _queueLimited; } }
        internal double QueuePressureRatio { get { return _queueLimited ? _occupied / (double)Math.Max(1, _limit) : 0; } }
        internal int ColumnCount { get { return 2; } }
        internal static string CaptionAt(int index) { return Captions[index]; }
        internal static string CompactCaptionAt(int index) { return CompactCaptionChoices[index][0]; }
        internal string SelectedCaptionAt(int index) { return _selectedCaptions[index] ?? (_compact ? CompactCaptionChoices[index][0] : Captions[index]); }
        internal bool ValueWasTruncated(int index) { return _valueTruncated[index]; }
        internal int ValueAllocationWidth(int index) { return _lastValueWidths[index]; }
        internal string ValueAt(int index) { return _values[index]; }
        internal string SpeedMeasurementDescription
        {
            get { return _speedMeasurementDescription; }
            set { _speedMeasurementDescription = String.IsNullOrEmpty(value) ? "250 ms" : value; }
        }
        internal bool PerNoteIntervalGate
        {
            get { return _perNoteIntervalGate; }
            set { if (_perNoteIntervalGate != value) { _perNoteIntervalGate = value; Invalidate(); } }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.Clear(BackColor);
            int columnWidth = Math.Max(1, ClientSize.Width / 2);
            int contentHeight = Math.Max(1, ClientSize.Height - (_queueLimited ? 18 : 0));
            int rowHeight = Math.Max(Math.Max(_captionFont.Height, _valueFont.Height), contentHeight / 4);
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
            int gap = _compact ? 2 : 6;
            TextFormatFlags vertical = TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
            string captionText = _perNoteIntervalGate && index == 4
                ? (_compact ? "Sent/excluded:" : "Events sent / excluded:")
                : (_compact ? CompactCaptionChoices[index][0] : Captions[index]);
            int measuredValue = MeasureStatisticValue(graphics, _values[index], vertical);
            int innerWidth = Math.Max(1, width - 4);
            int minimumCaption = _compact ? 16 : 60;
            int valueWidth = Math.Min(measuredValue, Math.Max(24, innerWidth - minimumCaption - gap));
            _lastValueWidths[index] = valueWidth;
            int captionWidth = Math.Max(1, innerWidth - valueWidth - gap);
            if (_compact)
            {
                string[] choices = _perNoteIntervalGate && index == 4
                    ? PerNoteCompactEventCaptions : CompactCaptionChoices[index];
                captionText = choices[choices.Length - 1];
                for (int choice = 0; choice < choices.Length; choice++)
                {
                    int measured = TextRenderer.MeasureText(graphics, choices[choice], _captionFont, Size.Empty, vertical).Width;
                    if (measured <= captionWidth) { captionText = choices[choice]; break; }
                }
            }
            _selectedCaptions[index] = captionText;
            int measuredCaption = TextRenderer.MeasureText(graphics, captionText, _captionFont, Size.Empty, vertical).Width;
            Rectangle caption = new Rectangle(left + 2, row * rowHeight, captionWidth, rowHeight);
            Rectangle value = new Rectangle(caption.Right + gap, row * rowHeight, valueWidth, rowHeight);
            _captionTruncated[index] = measuredCaption > caption.Width;
            _valueTruncated[index] = measuredValue > value.Width;
            TextRenderer.DrawText(graphics, captionText, _captionFont, caption, ForeColor,
                vertical | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            DrawStatisticValue(graphics, _values[index], value, vertical, measuredValue);
        }

        private int MeasureStatisticValue(Graphics graphics, string text, TextFormatFlags flags)
        {
            if (!_compact || text.IndexOf(CompactSlash, StringComparison.Ordinal) < 0)
                return TextRenderer.MeasureText(graphics, text, _valueFont, Size.Empty, flags).Width;
            int width = 0;
            int start = 0;
            int slash = text.IndexOf(CompactSlash, start, StringComparison.Ordinal);
            while (slash >= 0)
            {
                if (slash > start)
                    width += TextRenderer.MeasureText(graphics, text.Substring(start, slash - start),
                        _valueFont, Size.Empty, flags).Width;
                width += TextRenderer.MeasureText(graphics, "/", _valueFont, Size.Empty, flags).Width + 4;
                start = slash + CompactSlash.Length;
                slash = text.IndexOf(CompactSlash, start, StringComparison.Ordinal);
            }
            if (start < text.Length)
                width += TextRenderer.MeasureText(graphics, text.Substring(start), _valueFont,
                    Size.Empty, flags).Width;
            return width;
        }

        internal int MeasureStatisticValueForTesting(Graphics graphics, string text)
        {
            return MeasureStatisticValue(graphics, text, TextFormatFlags.NoPadding | TextFormatFlags.SingleLine);
        }

        private void DrawStatisticValue(Graphics graphics, string text, Rectangle bounds,
            TextFormatFlags flags, int measuredWidth)
        {
            if (!_compact || text.IndexOf(CompactSlash, StringComparison.Ordinal) < 0 || measuredWidth > bounds.Width)
            {
                TextRenderer.DrawText(graphics, text, _valueFont, bounds, ForeColor,
                    flags | TextFormatFlags.Right | TextFormatFlags.EndEllipsis);
                return;
            }
            int x = bounds.Right - measuredWidth;
            int start = 0;
            int slash = text.IndexOf(CompactSlash, start, StringComparison.Ordinal);
            while (slash >= 0)
            {
                if (slash > start)
                {
                    string part = text.Substring(start, slash - start);
                    int width = TextRenderer.MeasureText(graphics, part, _valueFont, Size.Empty, flags).Width;
                    TextRenderer.DrawText(graphics, part, _valueFont,
                        new Rectangle(x, bounds.Top, width, bounds.Height), ForeColor, flags | TextFormatFlags.Left);
                    x += width;
                }
                x += 2;
                int slashWidth = TextRenderer.MeasureText(graphics, "/", _valueFont, Size.Empty, flags).Width;
                TextRenderer.DrawText(graphics, "/", _valueFont,
                    new Rectangle(x, bounds.Top, slashWidth, bounds.Height), ForeColor, flags | TextFormatFlags.Left);
                x += slashWidth + 2;
                start = slash + CompactSlash.Length;
                slash = text.IndexOf(CompactSlash, start, StringComparison.Ordinal);
            }
            if (start < text.Length)
                TextRenderer.DrawText(graphics, text.Substring(start), _valueFont,
                    new Rectangle(x, bounds.Top, bounds.Right - x, bounds.Height), ForeColor, flags | TextFormatFlags.Left);
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
            if (_queueLimited && e.Y >= ClientSize.Height - 18)
            {
                _lastToolTipCell = -2;
                _toolTip.SetToolTip(this, _virtualQueuePressure
                    ? "Virtual pressure uses the selected rate model to decide whether a newly arriving MIDI event fits. Accepted MIDI is sent immediately. This is separate from the actual unsent scheduler/output backlog shown above, and it does not predict delay inside a driver or synthesizer."
                    : "Finite-buffer occupancy includes the event currently in service. Safety-preserving overflow policies may temporarily exceed the configured soft limit so a required Note Off or non-note message is not discarded.");
                return;
            }
            if (cell == _lastToolTipCell) return;
            _lastToolTipCell = cell;
            string text = cell == 5 ? (_perNoteIntervalGate
                ? "How quickly the gate has resolved the source timeline compared with real playback time. It advances only after each gate frame and its output calls finish, so a blocked output stalls the measurement. Current measurement window: " + _speedMeasurementDescription + ". Right-click to change it."
                : "Output-timeline advancement relative to elapsed playback time. Current measurement window: " +
                    _speedMeasurementDescription + ". Right-click to change it.") :
                cell == 0 ? "Playback timeline / MIDI output position. The output position is the source timestamp of the most recently sent MIDI event." :
                cell == 2 ? (_perNoteIntervalGate
                    ? "One frame is one configured gate interval. Each of the 128 MIDI pitches can make at most one Note On or Note Off transition in a frame, while different pitches can transition together. Other MIDI messages are not limited by this figure, so it is not the player's total event-throughput ceiling."
                    : "With immediate simulated service, this is the highest observed 250 ms rolling dispatch rate since statistics were reset—not a theoretical hardware or scheduler capacity. Nonzero service models show their theoretical configured maximum.") :
                cell == 4 ? "Sent means MIDI messages successfully dispatched. Dropped means queue overflow; gate-filtered means note messages combined or excluded by the per-note gate. These are separate counts. With None, sent means logically consumed; no physical MIDI data leaves the application." :
                cell == 6 || cell == 7 ? (_perNoteIntervalGate
                    ? "Lag is how late the most recently sent MIDI event was compared with its original time in the file. In Per-note mode it includes waiting for a gate frame, scheduler delay, and a blocking output call. Different transitions in one frame can come from different source times, so current lag may vary. It cannot measure synthesizer rendering or audio-device latency."
                    : "Lag is lateness through MIDI dispatch, including scheduler delay or a blocking output call. It cannot measure synthesizer rendering or audio-device latency.") : String.Empty;
            if (cell >= 0 && (_captionTruncated[cell] || _valueTruncated[cell]))
            {
                string full = (_perNoteIntervalGate && cell == 4
                    ? "Events sent / excluded (gate-filtered and queue-dropped separately):"
                    : Captions[cell]) + " " + _values[cell];
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
                graphics.FillRectangle(fill, new Rectangle(bar.X, bar.Y,
                    (int)Math.Round(bar.Width * Math.Min(1.0, ratio)), bar.Height));
                graphics.DrawRectangle(outline, bar);
            }
            string text = (_virtualQueuePressure ? "Virtual " : String.Empty) + _occupied.ToString("N0") + " / " +
                _limit.ToString("N0") + " — " + Math.Round(100 * ratio).ToString("N0") + "%";
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
