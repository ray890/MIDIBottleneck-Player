using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    // Label's normal erase/text sequence is visible during rapid progress
    // changes. Compose the background and text together in the same buffer.
    internal sealed class BufferedStatusLabel : Label
    {
        private int _paintCount;
        public BufferedStatusLabel()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint |
                ControlStyles.OptimizedDoubleBuffer | ControlStyles.Opaque |
                ControlStyles.ResizeRedraw, true);
            DoubleBuffered = true;
        }

        internal bool UsesAtomicPainting
        {
            get { return DoubleBuffered && GetStyle(ControlStyles.Opaque) && GetStyle(ControlStyles.AllPaintingInWmPaint); }
        }
        internal int PaintCount { get { return _paintCount; } }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            // OnPaint composes background and glyphs into the same buffer.  In
            // particular, do not let WM_ERASEBKGND expose the blank label
            // between successive loading-status strings.
        }

        protected override void OnTextChanged(EventArgs e)
        {
            // Label.OnTextChanged participates in preferred-size/layout work
            // even though these status surfaces occupy fixed table cells.  The
            // text is presentation-only, so invalidate this control without
            // asking its parent table to lay out again.
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            _paintCount++;
            Color background = BackColor;
            if (background == Color.Transparent)
                background = Parent == null ? SystemColors.Control : Parent.BackColor;
            e.Graphics.Clear(background);

            TextFormatFlags flags = TextFormatFlags.NoPadding | TextFormatFlags.PreserveGraphicsClipping;
            if (AutoEllipsis) flags |= TextFormatFlags.EndEllipsis;
            flags |= TextFormatFlags.SingleLine;
            switch (TextAlign)
            {
                case ContentAlignment.TopCenter:
                case ContentAlignment.MiddleCenter:
                case ContentAlignment.BottomCenter: flags |= TextFormatFlags.HorizontalCenter; break;
                case ContentAlignment.TopRight:
                case ContentAlignment.MiddleRight:
                case ContentAlignment.BottomRight: flags |= TextFormatFlags.Right; break;
                default: flags |= TextFormatFlags.Left; break;
            }
            switch (TextAlign)
            {
                case ContentAlignment.MiddleLeft:
                case ContentAlignment.MiddleCenter:
                case ContentAlignment.MiddleRight: flags |= TextFormatFlags.VerticalCenter; break;
                case ContentAlignment.BottomLeft:
                case ContentAlignment.BottomCenter:
                case ContentAlignment.BottomRight: flags |= TextFormatFlags.Bottom; break;
                default: flags |= TextFormatFlags.Top; break;
            }
            string text = Text ?? String.Empty;
            int lineBreak = text.IndexOfAny(new char[] { '\r', '\n' });
            if (lineBreak >= 0)
            {
                string first = text.Substring(0, lineBreak);
                int secondStart = lineBreak;
                while (secondStart < text.Length && (text[secondStart] == '\r' || text[secondStart] == '\n')) secondStart++;
                string second = text.Substring(secondStart);
                int firstHeight = ClientRectangle.Height / 2;
                Rectangle firstBounds = new Rectangle(ClientRectangle.X, ClientRectangle.Y, ClientRectangle.Width, firstHeight);
                Rectangle secondBounds = new Rectangle(ClientRectangle.X, ClientRectangle.Y + firstHeight,
                    ClientRectangle.Width, ClientRectangle.Height - firstHeight);
                TextRenderer.DrawText(e.Graphics, first, Font, firstBounds,
                    Enabled ? ForeColor : SystemColors.GrayText, background, flags | TextFormatFlags.VerticalCenter);
                TextRenderer.DrawText(e.Graphics, second, Font, secondBounds,
                    Enabled ? ForeColor : SystemColors.GrayText, background, flags | TextFormatFlags.VerticalCenter);
            }
            else
            {
                TextRenderer.DrawText(e.Graphics, text, Font, ClientRectangle,
                    Enabled ? ForeColor : SystemColors.GrayText, background, flags);
            }
        }
    }
}
