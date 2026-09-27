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

    // The collapsed field is a native ComboBox containing only real choices.
    // Its popup is a system-rendered ToolStripDropDown so the two group labels
    // are actual static text rather than selectable/list-accessible items.
    // The explicit mapping deliberately does not cast a display index to
    // ServiceDurationMode.
    internal sealed class RateModelComboBox : ComboBox
    {
        private const int CbShowDropDown = 0x014F;
        private const int NoneIndex = 0;
        private const int ProcessingIndex = 1;
        private const int BitrateIndex = 2;
        private const int EventsIndex = 3;
        private const int GateIndex = 4;
        private ToolStripDropDownMenu _groupedDropDown;
        private ToolStripLabel _slowdownHeadingItem;
        private ToolStripLabel _gateHeadingItem;
        private Font _headingFont;
        private bool _forwardOnlyHeading;
        private bool _compactDisplay;
        private int _popupWidth = 252;

        internal RateModelComboBox()
        {
            DropDownStyle = ComboBoxStyle.DropDownList;
            // The native collapsed field deliberately remains normally drawn
            // so its themed field, border, arrow, disabled state, and focus
            // treatment match the adjacent Overflow ComboBox.
            DrawMode = DrawMode.Normal;
            DropDownWidth = 252;
            AccessibleName = "Rate model";
            AccessibleDescription = "Choose None, an ordinary MIDI processing rate, or the Per-note interval gate. Static group labels organize the choices.";
            Items.Add("None");
            Items.Add("Processing time per event");
            Items.Add("MIDI serial bitrate");
            Items.Add("Events per second");
            Items.Add("Per-note interval gate");
            SelectedIndex = NoneIndex;
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
                if (_slowdownHeadingItem != null)
                    _slowdownHeadingItem.Text = SlowdownHeadingText;
            }
        }

        internal int ApplicationScalePercent
        {
            set
            {
                value = Math.Max(50, Math.Min(200, value));
                DropDownWidth = Math.Max(1, (252 * value + 50) / 100);
                _popupWidth = DropDownWidth;
                if (_groupedDropDown != null)
                {
                    _groupedDropDown.MinimumSize = new Size(_popupWidth, 0);
                    UpdateHeadingSizes();
                }
                Invalidate();
            }
        }

        internal bool CompactDisplay
        {
            get { return _compactDisplay; }
            set
            {
                if (_compactDisplay == value) return;
                RateModelChoice selected = SelectedChoice;
                _compactDisplay = value;
                Items[ProcessingIndex] = value ? "Time per event" : "Processing time per event";
                Items[BitrateIndex] = value ? "MIDI bitrate" : "MIDI serial bitrate";
                Items[EventsIndex] = value ? "Events/sec" : "Events per second";
                Items[GateIndex] = value ? "Per-note gate" : "Per-note interval gate";
                SelectedChoice = selected;
            }
        }

        private string SlowdownHeadingText
        {
            get { return _forwardOnlyHeading ? "Forward-only queue admission" : "Simulated slowdown"; }
        }

        private void EnsureGroupedDropDown()
        {
            if (_groupedDropDown != null) return;
            _groupedDropDown = new ToolStripDropDownMenu();
            _groupedDropDown.AutoSize = true;
            _groupedDropDown.AutoClose = true;
            _groupedDropDown.ShowImageMargin = false;
            _groupedDropDown.ShowCheckMargin = false;
            _groupedDropDown.RenderMode = ToolStripRenderMode.System;
            _groupedDropDown.Font = Font;
            _groupedDropDown.MinimumSize = new Size(_popupWidth, 0);
            _headingFont = new Font(Font, FontStyle.Bold);
            AddChoice("None", RateModelChoice.None);
            _slowdownHeadingItem = AddHeading(SlowdownHeadingText);
            AddChoice("Processing time per event", RateModelChoice.ProcessingTime);
            AddChoice("MIDI serial bitrate", RateModelChoice.MidiBitrate);
            AddChoice("Events per second", RateModelChoice.EventsPerSecond);
            _gateHeadingItem = AddHeading("Bandwidth / note gating");
            AddChoice("Per-note interval gate", RateModelChoice.PerNoteIntervalGate);
        }

        private ToolStripLabel AddHeading(string text)
        {
            ToolStripLabel heading = new ToolStripLabel(text);
            heading.Enabled = false;
            heading.AutoSize = false;
            heading.Font = _headingFont;
            heading.ForeColor = SystemColors.GrayText;
            heading.BackColor = SystemColors.Control;
            heading.AccessibleName = text;
            heading.AccessibleRole = AccessibleRole.StaticText;
            heading.TextAlign = ContentAlignment.MiddleLeft;
            heading.Padding = new Padding(5, 0, 2, 0);
            heading.Margin = new Padding(0, 2, 0, 1);
            _groupedDropDown.Items.Add(heading);
            UpdateHeadingSize(heading);
            return heading;
        }

        private void UpdateHeadingSizes()
        {
            UpdateHeadingSize(_slowdownHeadingItem);
            UpdateHeadingSize(_gateHeadingItem);
        }

        private void UpdateHeadingSize(ToolStripLabel heading)
        {
            if (heading == null) return;
            heading.Size = new Size(Math.Max(1, _popupWidth - 2),
                Math.Max(18, Font.Height + 6));
        }

        private void AddChoice(string text, RateModelChoice choice)
        {
            ToolStripMenuItem item = new ToolStripMenuItem(text);
            item.Tag = choice;
            item.AccessibleName = text;
            item.AccessibleRole = AccessibleRole.MenuItem;
            item.Click += delegate
            {
                SelectedChoice = (RateModelChoice)item.Tag;
                Select();
            };
            _groupedDropDown.Items.Add(item);
        }

        private void ShowGroupedDropDown()
        {
            if (!Enabled || !Visible || !IsHandleCreated) return;
            EnsureGroupedDropDown();
            if (_groupedDropDown.Visible) return;
            _groupedDropDown.Show(this, new Point(0, Height));
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (!e.Alt && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down ||
                e.KeyCode == Keys.Home || e.KeyCode == Keys.End ||
                e.KeyCode == Keys.PageUp || e.KeyCode == Keys.PageDown))
            {
                int target = e.KeyCode == Keys.Home ? NoneIndex :
                    e.KeyCode == Keys.End ? GateIndex :
                    NextChoice(SelectedIndex, e.KeyCode == Keys.Up || e.KeyCode == Keys.PageUp ? -1 : 1);
                SelectedIndex = target;
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            base.OnKeyDown(e);
        }

        protected override void OnKeyPress(KeyPressEventArgs e)
        {
            // Explicit navigation avoids native prefix-search differences and
            // keeps selection deterministic across themed/native versions.
            if (!Char.IsControl(e.KeyChar)) { e.Handled = true; return; }
            base.OnKeyPress(e);
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
            return Math.Max(NoneIndex, Math.Min(GateIndex, candidate));
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == CbShowDropDown && m.WParam != IntPtr.Zero)
            {
                ShowGroupedDropDown();
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == CbShowDropDown && m.WParam == IntPtr.Zero && _groupedDropDown != null)
                _groupedDropDown.Close(ToolStripDropDownCloseReason.CloseCalled);
            base.WndProc(ref m);
        }

        protected override void OnFontChanged(EventArgs e)
        {
            base.OnFontChanged(e);
            DisposeGroupedDropDown();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) DisposeGroupedDropDown();
            base.Dispose(disposing);
        }

        private void DisposeGroupedDropDown()
        {
            if (_groupedDropDown != null) _groupedDropDown.Dispose();
            if (_headingFont != null) _headingFont.Dispose();
            _groupedDropDown = null;
            _slowdownHeadingItem = null;
            _gateHeadingItem = null;
            _headingFont = null;
        }

        internal int SemanticChoiceCountForTesting { get { return Items.Count; } }
        internal AccessibleRole[] HeadingRolesForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                return new AccessibleRole[] { _slowdownHeadingItem.AccessibleRole,
                    _gateHeadingItem.AccessibleRole };
            }
        }
        internal bool[] HeadingEnabledForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                return new bool[] { _slowdownHeadingItem.Enabled, _gateHeadingItem.Enabled };
            }
        }
        internal bool[] HeadingSelectableForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                return new bool[] { _slowdownHeadingItem.CanSelect, _gateHeadingItem.CanSelect };
            }
        }
        internal bool PopupVisibleForTesting
        { get { return _groupedDropDown != null && _groupedDropDown.Visible; } }
        internal string[] HeadingTextsForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                return new string[] { _slowdownHeadingItem.Text, _gateHeadingItem.Text };
            }
        }
        internal Rectangle[] HeadingBoundsForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                return new Rectangle[] { _slowdownHeadingItem.Bounds, _gateHeadingItem.Bounds };
            }
        }
        internal Bitmap CapturePopupForTesting()
        {
            EnsureGroupedDropDown();
            Size size = _groupedDropDown.GetPreferredSize(Size.Empty);
            Bitmap bitmap = new Bitmap(Math.Max(1, size.Width), Math.Max(1, size.Height));
            _groupedDropDown.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
            return bitmap;
        }
    }
}
