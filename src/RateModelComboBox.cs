using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal enum RateModelChoice
    {
        None,
        ProcessingTime,
        MidiBitrate,
        EventsPerSecond,
        PerNoteIntervalGate
    }

    // A native ComboBox with two visual group labels. Group labels are not
    // values: mouse, keyboard, wheel, and programmatic selection all leave a
    // selectable model in place. The explicit mapping deliberately does not
    // cast a ComboBox index to ServiceDurationMode.
    internal sealed class RateModelComboBox : ComboBox
    {
        private const int NoneIndex = 0;
        private const int SlowdownHeadingIndex = 1;
        private const int ProcessingIndex = 2;
        private const int BitrateIndex = 3;
        private const int EventsIndex = 4;
        private const int GateHeadingIndex = 5;
        private const int GateIndex = 6;
        private int _lastChoiceIndex;
        private bool _restoringChoice;
        private Font _headingFont;
        private bool _forwardOnlyHeading;

        internal RateModelComboBox()
        {
            DropDownStyle = ComboBoxStyle.DropDownList;
            DrawMode = DrawMode.OwnerDrawFixed;
            DropDownWidth = 252;
            AccessibleName = "Rate model";
            AccessibleDescription = "Choose None, an ordinary MIDI processing rate, or the Per-note interval gate. Group labels cannot be selected.";
            Items.Add("None");
            Items.Add("Simulated slowdown");
            Items.Add("Processing time per event");
            Items.Add("MIDI serial bitrate");
            Items.Add("Events per second");
            Items.Add("Bandwidth / note gating");
            Items.Add("Per-note interval gate");
            _lastChoiceIndex = NoneIndex;
            SelectedIndex = NoneIndex;
            if (_headingFont == null) _headingFont = new Font(Font, FontStyle.Bold);
        }

        internal RateModelChoice SelectedChoice
        {
            get
            {
                switch (SelectedIndex)
                {
                    case ProcessingIndex: return RateModelChoice.ProcessingTime;
                    case BitrateIndex: return RateModelChoice.MidiBitrate;
                    case EventsIndex: return RateModelChoice.EventsPerSecond;
                    case GateIndex: return RateModelChoice.PerNoteIntervalGate;
                    default: return RateModelChoice.None;
                }
            }
            set
            {
                switch (value)
                {
                    case RateModelChoice.ProcessingTime: SelectedIndex = ProcessingIndex; break;
                    case RateModelChoice.MidiBitrate: SelectedIndex = BitrateIndex; break;
                    case RateModelChoice.EventsPerSecond: SelectedIndex = EventsIndex; break;
                    case RateModelChoice.PerNoteIntervalGate: SelectedIndex = GateIndex; break;
                    default: SelectedIndex = NoneIndex; break;
                }
            }
        }

        internal bool ForwardOnlyHeading
        {
            get { return _forwardOnlyHeading; }
            set
            {
                if (_forwardOnlyHeading == value) return;
                _forwardOnlyHeading = value;
                Items[SlowdownHeadingIndex] = value
                    ? "Forward-only queue admission" : "Simulated slowdown";
                Invalidate();
            }
        }

        internal static bool IsHeadingIndex(int index)
        {
            return index == SlowdownHeadingIndex || index == GateHeadingIndex;
        }

        protected override void OnSelectedIndexChanged(EventArgs e)
        {
            if (_restoringChoice) return;
            if (SelectedIndex < 0 || IsHeadingIndex(SelectedIndex))
            {
                _restoringChoice = true;
                try { SelectedIndex = _lastChoiceIndex; }
                finally { _restoringChoice = false; }
                return;
            }
            _lastChoiceIndex = SelectedIndex;
            base.OnSelectedIndexChanged(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (!e.Alt && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down ||
                e.KeyCode == Keys.Home || e.KeyCode == Keys.End))
            {
                int target = e.KeyCode == Keys.Home ? NoneIndex :
                    e.KeyCode == Keys.End ? GateIndex :
                    NextChoice(SelectedIndex, e.KeyCode == Keys.Up ? -1 : 1);
                SelectedIndex = target;
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            base.OnKeyDown(e);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            if (e.Delta != 0) SelectedIndex = NextChoice(SelectedIndex, e.Delta > 0 ? -1 : 1);
            HandledMouseEventArgs handled = e as HandledMouseEventArgs;
            if (handled != null) handled.Handled = true;
        }

        private static int NextChoice(int start, int direction)
        {
            int candidate = Math.Max(NoneIndex, Math.Min(GateIndex, start + direction));
            while (IsHeadingIndex(candidate) && candidate > NoneIndex && candidate < GateIndex)
                candidate += direction;
            return Math.Max(NoneIndex, Math.Min(GateIndex, candidate));
        }

        protected override void OnDrawItem(DrawItemEventArgs e)
        {
            if (e.Index < 0 || e.Index >= Items.Count) return;
            e.DrawBackground();
            bool heading = IsHeadingIndex(e.Index);
            string label = Items[e.Index].ToString();
            if (!heading && e.Bounds.Width < 190 && (e.State & DrawItemState.ComboBoxEdit) != 0)
            {
                if (e.Index == ProcessingIndex) label = "Time per event";
                else if (e.Index == BitrateIndex) label = "MIDI bitrate";
                else if (e.Index == EventsIndex) label = "Events/sec";
                else if (e.Index == GateIndex) label = "Per-note gate";
            }
            Rectangle bounds = e.Bounds;
            bounds.Inflate(-2, 0);
            Color color = heading ? SystemColors.GrayText : e.ForeColor;
            TextRenderer.DrawText(e.Graphics, label, heading ? _headingFont ?? Font : Font,
                bounds, color, TextFormatFlags.NoPadding | TextFormatFlags.SingleLine |
                TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
            if (!heading) e.DrawFocusRectangle();
        }

        protected override void OnFontChanged(EventArgs e)
        {
            if (_headingFont != null) _headingFont.Dispose();
            _headingFont = new Font(Font, FontStyle.Bold);
            base.OnFontChanged(e);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && _headingFont != null) { _headingFont.Dispose(); _headingFont = null; }
            base.Dispose(disposing);
        }
    }
}
