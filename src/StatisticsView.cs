using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class StatisticsView : Control
    {
        private static readonly string[] Captions = new string[]
        {
            "Theoretical maximum rate:", "Current queue length:",
            "Maximum queue length:", "Events processed:",
            "Events dropped:", "Playback clock:",
            "Intended timeline position:", "Last dispatched source position:",
            "Current simulated lag:", "Maximum simulated lag:"
        };

        private readonly string[] _values = new string[10];
        private readonly Font _captionFont;
        private readonly Font _valueFont;

        public StatisticsView()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;
            _captionFont = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            _valueFont = new Font("Consolas", 9F, FontStyle.Bold, GraphicsUnit.Point);
            for (int i = 0; i < _values.Length; i++) _values[i] = "0";
            Height = 120;
            MinimumSize = new Size(680, 120);
            TabStop = false;
        }

        public bool SetValues(string[] values)
        {
            if (values == null || values.Length != _values.Length)
                throw new ArgumentException("Exactly ten statistic values are required.", "values");
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

        internal bool UsesDoubleBuffer { get { return DoubleBuffered; } }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.Clear(BackColor);
            int half = ClientSize.Width / 2;
            int rowHeight = Math.Max(24, ClientSize.Height / 5);
            for (int row = 0; row < 5; row++)
            {
                DrawCell(e.Graphics, 0, row * 2, row, half, rowHeight);
                DrawCell(e.Graphics, half, row * 2 + 1, row, ClientSize.Width - half, rowHeight);
            }
        }

        private void DrawCell(Graphics graphics, int left, int index, int row, int width, int rowHeight)
        {
            Rectangle caption = new Rectangle(left + 3, row * rowHeight, Math.Max(80, width - 165), rowHeight);
            Rectangle value = new Rectangle(left + Math.Max(85, width - 162), row * rowHeight, 150, rowHeight);
            TextFormatFlags vertical = TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding;
            TextRenderer.DrawText(graphics, Captions[index], _captionFont, caption, ForeColor, vertical | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            TextRenderer.DrawText(graphics, _values[index], _valueFont, value, ForeColor, vertical | TextFormatFlags.Right | TextFormatFlags.EndEllipsis);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _captionFont.Dispose();
                _valueFont.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}
