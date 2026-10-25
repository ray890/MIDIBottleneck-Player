using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal enum RateModelChoice
    {
        None,
        ProcessingTime,
        MidiBitrate,
        EventsPerSecond,
        PerNoteIntervalGate,
        DelayedNoteOff,
        NoteAttackLimit,
        OutputRateLimit
    }

    // The collapsed field is a native ComboBox containing only real choices.
    // Its popup is a system-rendered ToolStripDropDown so the two group labels
    // are actual static text rather than selectable/list-accessible items.
    // The explicit mapping deliberately does not cast a display index to
    // ServiceDurationMode.
    internal sealed class RateModelComboBox : ComboBox, IMessageFilter
    {
        private sealed class GroupedRateRenderer : ToolStripSystemRenderer
        {
            protected override void OnRenderLabelBackground(ToolStripItemRenderEventArgs e)
            {
                if (e.Item != null && Object.Equals(e.Item.Tag, "RateHeading"))
                {
                    using (Brush background = new SolidBrush(HeadingBandColor))
                        e.Graphics.FillRectangle(background,
                            new Rectangle(Point.Empty, e.Item.Size));
                }
                else
                    base.OnRenderLabelBackground(e);
            }
        }

        private const int CbShowDropDown = 0x014F;
        private const int WmLeftButtonDown = 0x0201;
        private const int WmLeftButtonUp = 0x0202;
        private const int WmLeftButtonDoubleClick = 0x0203;
        private const int WmKeyDown = 0x0100;
        private const int WmKeyUp = 0x0101;
        private const int WmSystemKeyDown = 0x0104;
        private const int WmSystemKeyUp = 0x0105;
        private const int NoneIndex = 0;
        private const int ProcessingIndex = 1;
        private const int BitrateIndex = 2;
        private const int EventsIndex = 3;
        private const int GateIndex = 4;
        private const int DelayedNoteOffIndex = 5;
        private const int NoteAttackLimitIndex = 6;
        private const int OutputRateLimitIndex = 7;
        private static Color HeadingBandColor
        {
            get { return SystemColors.ControlLightLight; }
        }
        private ToolStripDropDown _groupedDropDown;
        private ToolStripLabel _slowdownHeadingItem;
        private ToolStripLabel _gateHeadingItem;
        private Font _headingFont;
        private bool _forwardOnlyHeading;
        private bool _updatingCollapsedLabels;
        private int _popupWidth = 1;
        private int _realizedItemWidth;
        private int _applicationScalePercent = 100;
        private bool _popupMessageFilterInstalled;
        private bool _activationKeyDown;
        private Form _popupOwnerForm;

        internal RateModelComboBox()
        {
            DropDownStyle = ComboBoxStyle.DropDownList;
            // The native collapsed field deliberately remains normally drawn
            // so its themed field, border, arrow, disabled state, and focus
            // treatment match the adjacent Overflow ComboBox.
            DrawMode = DrawMode.Normal;
            DropDownWidth = 1;
            AccessibleName = "Rate model";
            AccessibleDescription = "Choose None, an ordinary MIDI processing rate, or a musical note-timing model. Static group labels organize the choices.";
            Items.Add("None");
            Items.Add("Processing time per event");
            Items.Add("MIDI serial bitrate");
            Items.Add("Events per second");
            Items.Add("Per-note interval gate");
            Items.Add("Delayed NoteOff");
            Items.Add("Note start rate gate");
            Items.Add("Output rate gate");
            SelectedIndex = NoneIndex;
            UpdateCollapsedLabels();
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
                    case DelayedNoteOffIndex: return RateModelChoice.DelayedNoteOff;
                    case NoteAttackLimitIndex: return RateModelChoice.NoteAttackLimit;
                    case OutputRateLimitIndex: return RateModelChoice.OutputRateLimit;
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
                    case RateModelChoice.DelayedNoteOff: SelectedIndex = DelayedNoteOffIndex; break;
                    case RateModelChoice.NoteAttackLimit: SelectedIndex = NoteAttackLimitIndex; break;
                    case RateModelChoice.OutputRateLimit: SelectedIndex = OutputRateLimitIndex; break;
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
                DisposeGroupedDropDown();
            }
        }

        internal int ApplicationScalePercent
        {
            set
            {
                value = Math.Max(50, Math.Min(200, value));
                _applicationScalePercent = value;
                DisposeGroupedDropDown();
                UpdateCollapsedLabels();
                Invalidate();
            }
        }

        private string SlowdownHeadingText
        {
            get { return _forwardOnlyHeading ? "Forward-only queue admission" : "Simulated slowdown"; }
        }

        private void EnsureGroupedDropDown()
        {
            if (_groupedDropDown != null) return;
            // A plain ToolStripDropDown avoids the permanent submenu-arrow
            // gutter reserved by ToolStripDropDownMenu.  No row in this popup
            // owns a submenu, so that gutter was only asymmetric blank space.
            _groupedDropDown = new ToolStripDropDown();
            _groupedDropDown.AutoSize = false;
            // Outside-click behavior is handled by the control's message
            // filter so a click on the owner cannot be processed once as an
            // automatic close and again as a fresh native ComboBox opening.
            _groupedDropDown.AutoClose = false;
            _groupedDropDown.LayoutStyle = ToolStripLayoutStyle.VerticalStackWithOverflow;
            _groupedDropDown.Renderer = new GroupedRateRenderer();
            _groupedDropDown.Font = Font;
            _groupedDropDown.BackColor = SystemColors.Menu;
            _groupedDropDown.MinimumSize = new Size(_popupWidth, 0);
            _groupedDropDown.Closed += delegate { RemovePopupMessageFilter(); };
            _headingFont = new Font(Font, FontStyle.Bold);
            AddChoice("None", RateModelChoice.None);
            _slowdownHeadingItem = AddHeading(SlowdownHeadingText);
            AddChoice("Processing time per event", RateModelChoice.ProcessingTime);
            AddChoice("MIDI serial bitrate", RateModelChoice.MidiBitrate);
            AddChoice("Events per second", RateModelChoice.EventsPerSecond);
            _gateHeadingItem = AddHeading("Bandwidth / note gating");
            AddChoice("Per-note interval gate", RateModelChoice.PerNoteIntervalGate);
            AddChoice("Delayed NoteOff", RateModelChoice.DelayedNoteOff);
            AddChoice("Note start rate gate", RateModelChoice.NoteAttackLimit);
            AddChoice("Output rate gate", RateModelChoice.OutputRateLimit);
            RecalculatePopupWidth();
        }

        private ToolStripLabel AddHeading(string text)
        {
            ToolStripLabel heading = new ToolStripLabel(text);
            // ToolStripLabel is intrinsically nonselectable.  Keep it enabled
            // so the current theme does not fade the group name into disabled
            // gray; accessibility still exposes static text, never a choice.
            heading.Enabled = true;
            heading.Tag = "RateHeading";
            heading.AutoSize = false;
            heading.Font = _headingFont;
            heading.ForeColor = SystemColors.ControlText;
            heading.BackColor = HeadingBandColor;
            heading.AccessibleName = text;
            heading.AccessibleRole = AccessibleRole.StaticText;
            heading.TextAlign = ContentAlignment.MiddleLeft;
            heading.Padding = new Padding(ScaleMetric(9), 0, ScaleMetric(3), 0);
            heading.Margin = new Padding(0, ScaleMetric(5), 0, ScaleMetric(2));
            heading.Paint += PaintHeadingSeparator;
            heading.Click += delegate
            {
                CloseGroupedDropDown();
            };
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
            int width = _realizedItemWidth > 0 ? _realizedItemWidth :
                _groupedDropDown == null ? _popupWidth :
                Math.Max(1, _groupedDropDown.DisplayRectangle.Width);
            heading.Size = new Size(Math.Max(1, width),
                Math.Max(ScaleMetric(20), Font.Height + ScaleMetric(8)));
            heading.Padding = new Padding(ScaleMetric(9), 0, ScaleMetric(3), 0);
            heading.Margin = new Padding(0, ScaleMetric(5), 0, ScaleMetric(2));
        }

        private void PaintHeadingSeparator(object sender, PaintEventArgs e)
        {
            ToolStripLabel heading = sender as ToolStripLabel;
            if (heading == null) return;
            using (Pen separator = new Pen(SystemColors.ControlDark))
                e.Graphics.DrawLine(separator, 0, heading.Height - 1,
                    Math.Max(0, heading.Width - 1), heading.Height - 1);
        }

        private void AddChoice(string text, RateModelChoice choice)
        {
            ToolStripMenuItem item = new ToolStripMenuItem(text);
            item.AutoSize = false;
            item.Tag = choice;
            item.AccessibleName = text;
            item.AccessibleRole = AccessibleRole.MenuItem;
            item.Padding = new Padding(ScaleMetric(20), 0, ScaleMetric(4), 0);
            item.Click += delegate
            {
                SelectedChoice = (RateModelChoice)item.Tag;
                CloseGroupedDropDown();
                Select();
            };
            _groupedDropDown.Items.Add(item);
        }

        private void UpdateChoiceIndents()
        {
            if (_groupedDropDown == null) return;
            for (int i = 0; i < _groupedDropDown.Items.Count; i++)
            {
                ToolStripMenuItem item = _groupedDropDown.Items[i] as ToolStripMenuItem;
                if (item != null)
                    item.Padding = new Padding(ScaleMetric(20), 0, ScaleMetric(4), 0);
            }
        }

        private int ScaleMetric(int value)
        {
            return Math.Max(1, (value * _applicationScalePercent + 50) / 100);
        }

        private void RecalculatePopupWidth()
        {
            if (_groupedDropDown == null) return;
            int requiredContent = 0;
            for (int index = 0; index < _groupedDropDown.Items.Count; index++)
            {
                ToolStripItem item = _groupedDropDown.Items[index];
                ToolStripMenuItem choice = item as ToolStripMenuItem;
                if (choice != null)
                {
                    int textWidth = TextRenderer.MeasureText(choice.Text, choice.Font,
                        Size.Empty, TextFormatFlags.SingleLine |
                        TextFormatFlags.NoPadding).Width;
                    requiredContent = Math.Max(requiredContent, textWidth +
                        choice.Padding.Horizontal + ScaleMetric(4));
                }
                else
                {
                    ToolStripLabel heading = item as ToolStripLabel;
                    if (heading != null)
                    {
                        int textWidth = TextRenderer.MeasureText(heading.Text, heading.Font,
                            Size.Empty, TextFormatFlags.SingleLine |
                            TextFormatFlags.NoPadding).Width;
                        requiredContent = Math.Max(requiredContent,
                            textWidth + heading.Padding.Horizontal);
                    }
                }
            }
            int chrome = _groupedDropDown.Padding.Horizontal +
                Math.Max(2, SystemInformation.BorderSize.Width * 2);
            _popupWidth = Math.Max(1, requiredContent + chrome);
            DropDownWidth = _popupWidth;
            _groupedDropDown.MinimumSize = new Size(_popupWidth, 0);
            _realizedItemWidth = requiredContent;
            for (int index = 0; index < _groupedDropDown.Items.Count; index++)
            {
                ToolStripMenuItem choice = _groupedDropDown.Items[index] as ToolStripMenuItem;
                if (choice != null)
                {
                    int preferredHeight = Math.Max(ScaleMetric(20),
                        choice.Font.Height + ScaleMetric(8));
                    choice.Size = new Size(requiredContent, preferredHeight);
                }
            }
            UpdateHeadingSizes();
            int requiredHeight = _groupedDropDown.Padding.Vertical +
                Math.Max(2, SystemInformation.BorderSize.Height * 2);
            for (int index = 0; index < _groupedDropDown.Items.Count; index++)
            {
                ToolStripItem item = _groupedDropDown.Items[index];
                requiredHeight += item.Height + item.Margin.Vertical;
            }
            _groupedDropDown.Size = new Size(_popupWidth, Math.Max(1, requiredHeight));
            _realizedItemWidth = 0;
        }

        private void UpdateCollapsedLabels()
        {
            if (_updatingCollapsedLabels || Items.Count <= OutputRateLimitIndex) return;
            _updatingCollapsedLabels = true;
            try
            {
                RateModelChoice selected = SelectedChoice;
                int usable = CollapsedTextWidth();
                for (int index = NoneIndex; index <= OutputRateLimitIndex; index++)
                {
                    RateModelChoice choice = ChoiceForIndex(index);
                    string full = FullLabel(choice);
                    int measured = TextRenderer.MeasureText(full, Font, Size.Empty,
                        TextFormatFlags.SingleLine | TextFormatFlags.NoPadding).Width;
                    string display = measured <= usable ? full : FallbackLabel(choice);
                    if (!String.Equals(Items[index] as string, display,
                        StringComparison.Ordinal)) Items[index] = display;
                }
                SelectedChoice = selected;
            }
            finally { _updatingCollapsedLabels = false; }
        }

        private int CollapsedTextWidth()
        {
            int itemWidth = Math.Max(0, ClientSize.Width -
                SystemInformation.VerticalScrollBarWidth);
            if (IsHandleCreated)
            {
                ComboBoxInfo info = new ComboBoxInfo();
                info.Size = Marshal.SizeOf(typeof(ComboBoxInfo));
                if (GetComboBoxInfo(Handle, ref info))
                    itemWidth = Math.Max(0, info.Item.Right - info.Item.Left);
            }
            return Math.Max(0, itemWidth - ScaleMetric(4));
        }

        private static RateModelChoice ChoiceForIndex(int index)
        {
            switch (index)
            {
                case ProcessingIndex: return RateModelChoice.ProcessingTime;
                case BitrateIndex: return RateModelChoice.MidiBitrate;
                case EventsIndex: return RateModelChoice.EventsPerSecond;
                case GateIndex: return RateModelChoice.PerNoteIntervalGate;
                case DelayedNoteOffIndex: return RateModelChoice.DelayedNoteOff;
                case NoteAttackLimitIndex: return RateModelChoice.NoteAttackLimit;
                case OutputRateLimitIndex: return RateModelChoice.OutputRateLimit;
                default: return RateModelChoice.None;
            }
        }

        private static string FullLabel(RateModelChoice choice)
        {
            switch (choice)
            {
                case RateModelChoice.ProcessingTime: return "Processing time per event";
                case RateModelChoice.MidiBitrate: return "MIDI serial bitrate";
                case RateModelChoice.EventsPerSecond: return "Events per second";
                case RateModelChoice.PerNoteIntervalGate: return "Per-note interval gate";
                case RateModelChoice.DelayedNoteOff: return "Delayed NoteOff";
                case RateModelChoice.NoteAttackLimit: return "Note start rate gate";
                case RateModelChoice.OutputRateLimit: return "Output rate gate";
                default: return "None";
            }
        }

        private static string FallbackLabel(RateModelChoice choice)
        {
            switch (choice)
            {
                case RateModelChoice.ProcessingTime: return "Time per event";
                case RateModelChoice.MidiBitrate: return "MIDI bitrate";
                case RateModelChoice.EventsPerSecond: return "Events/sec";
                case RateModelChoice.PerNoteIntervalGate: return "Per-note gate";
                case RateModelChoice.NoteAttackLimit: return "Note start gate";
                case RateModelChoice.OutputRateLimit: return "Output gate";
                default: return FullLabel(choice);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeRectangle
        {
            internal int Left, Top, Right, Bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ComboBoxInfo
        {
            internal int Size;
            internal NativeRectangle Item;
            internal NativeRectangle Button;
            internal int ButtonState;
            internal IntPtr ComboHandle;
            internal IntPtr ItemHandle;
            internal IntPtr ListHandle;
        }

        [DllImport("user32.dll")]
        private static extern bool GetComboBoxInfo(IntPtr comboBox, ref ComboBoxInfo info);

        private void ShowGroupedDropDown()
        {
            if (!Enabled || !Visible || !IsHandleCreated) return;
            EnsureGroupedDropDown();
            if (_groupedDropDown.Visible) return;
            // Install before Show; ToolStripDropDown installs its own
            // auto-close filter during Show, and the owner must see an owner
            // click first so one click cannot become close-then-reopen.
            InstallPopupMessageFilter();
            _groupedDropDown.Show(this, new Point(0, Height));
            BalanceRealizedPopupWidth();
        }

        private void BalanceRealizedPopupWidth()
        {
            if (_groupedDropDown == null || !_groupedDropDown.Visible) return;
            // Size the heading banner from the realized client area.  The
            // requested width is calculated from content, while ToolStrip's
            // renderer owns its exact border and display rectangle.
            for (int pass = 0; pass < 2; pass++)
            {
                _groupedDropDown.PerformLayout();
                int width = Math.Max(1, _groupedDropDown.DisplayRectangle.Width);
                if (_realizedItemWidth == width) break;
                _realizedItemWidth = width;
                UpdateHeadingSizes();
            }
        }

        private void InstallPopupMessageFilter()
        {
            if (_popupMessageFilterInstalled) return;
            Application.AddMessageFilter(this);
            _popupMessageFilterInstalled = true;
            _popupOwnerForm = FindForm();
            if (_popupOwnerForm != null)
                _popupOwnerForm.Deactivate += PopupOwnerDeactivated;
        }

        private void RemovePopupMessageFilter()
        {
            if (!_popupMessageFilterInstalled) return;
            Application.RemoveMessageFilter(this);
            _popupMessageFilterInstalled = false;
            if (_popupOwnerForm != null)
                _popupOwnerForm.Deactivate -= PopupOwnerDeactivated;
            _popupOwnerForm = null;
        }

        private void PopupOwnerDeactivated(object sender, EventArgs e)
        {
            CloseGroupedDropDown();
        }

        public bool PreFilterMessage(ref Message message)
        {
            if (!_popupMessageFilterInstalled || _groupedDropDown == null ||
                !_groupedDropDown.Visible) return false;
            if (message.Msg == WmLeftButtonDown ||
                message.Msg == WmLeftButtonDoubleClick)
            {
                if (message.HWnd == _groupedDropDown.Handle) return false;
                if (message.HWnd != Handle)
                {
                    CloseGroupedDropDown();
                    return false;
                }
                // Own this physical action once: close the replacement popup
                // and consume the owner click so native ComboBox processing
                // cannot interpret it as a fresh opening.
                CloseGroupedDropDown();
                Focus();
                return true;
            }
            return false;
        }

        private void ToggleGroupedDropDown()
        {
            if (_groupedDropDown != null && _groupedDropDown.Visible)
                CloseGroupedDropDown();
            else ShowGroupedDropDown();
        }

        private void CloseGroupedDropDown()
        {
            if (_groupedDropDown != null && _groupedDropDown.Visible)
                _groupedDropDown.Close(ToolStripDropDownCloseReason.CloseCalled);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                Focus();
                ToggleGroupedDropDown();
                return;
            }
            base.OnMouseDown(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Escape && _groupedDropDown != null &&
                _groupedDropDown.Visible)
            {
                CloseGroupedDropDown();
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            if (e.KeyCode == Keys.F4 || e.Alt &&
                (e.KeyCode == Keys.Down || e.KeyCode == Keys.Up))
            {
                if (!_activationKeyDown)
                {
                    _activationKeyDown = true;
                    ToggleGroupedDropDown();
                }
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            if (!e.Alt && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down ||
                e.KeyCode == Keys.Home || e.KeyCode == Keys.End ||
                e.KeyCode == Keys.PageUp || e.KeyCode == Keys.PageDown))
            {
                int target = e.KeyCode == Keys.Home ? NoneIndex :
                    e.KeyCode == Keys.End ? OutputRateLimitIndex :
                    NextChoice(SelectedIndex, e.KeyCode == Keys.Up || e.KeyCode == Keys.PageUp ? -1 : 1);
                SelectedIndex = target;
                e.Handled = true;
                e.SuppressKeyPress = true;
                return;
            }
            base.OnKeyDown(e);
        }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.F4 || e.KeyCode == Keys.Down || e.KeyCode == Keys.Up)
                _activationKeyDown = false;
            base.OnKeyUp(e);
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
            int candidate = Math.Max(NoneIndex, Math.Min(OutputRateLimitIndex, start + direction));
            return Math.Max(NoneIndex, Math.Min(OutputRateLimitIndex, candidate));
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WmKeyDown && m.WParam == (IntPtr)Keys.Escape &&
                _groupedDropDown != null && _groupedDropDown.Visible)
            {
                CloseGroupedDropDown();
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmKeyDown && m.WParam == (IntPtr)Keys.Enter &&
                _groupedDropDown != null && _groupedDropDown.Visible)
            {
                CloseGroupedDropDown();
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmKeyDown && m.WParam == (IntPtr)Keys.F4)
            {
                if (!_activationKeyDown)
                {
                    _activationKeyDown = true;
                    ToggleGroupedDropDown();
                }
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmKeyUp && m.WParam == (IntPtr)Keys.F4)
            {
                _activationKeyDown = false;
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmLeftButtonDown || m.Msg == WmLeftButtonDoubleClick)
            {
                Focus();
                ToggleGroupedDropDown();
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmLeftButtonUp)
            {
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmSystemKeyDown &&
                (m.WParam == (IntPtr)Keys.Down || m.WParam == (IntPtr)Keys.Up))
            {
                if (!_activationKeyDown)
                {
                    _activationKeyDown = true;
                    ToggleGroupedDropDown();
                }
                m.Result = IntPtr.Zero;
                return;
            }
            if (m.Msg == WmSystemKeyUp &&
                (m.WParam == (IntPtr)Keys.Down || m.WParam == (IntPtr)Keys.Up))
            {
                _activationKeyDown = false;
                m.Result = IntPtr.Zero;
                return;
            }
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
            UpdateCollapsedLabels();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            UpdateCollapsedLabels();
        }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            UpdateCollapsedLabels();
        }

        protected override void OnSelectedIndexChanged(EventArgs e)
        {
            if (_updatingCollapsedLabels) return;
            UpdateCollapsedLabels();
            base.OnSelectedIndexChanged(e);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) DisposeGroupedDropDown();
            base.Dispose(disposing);
        }

        private void DisposeGroupedDropDown()
        {
            RemovePopupMessageFilter();
            if (_groupedDropDown != null) _groupedDropDown.Dispose();
            if (_headingFont != null) _headingFont.Dispose();
            _groupedDropDown = null;
            _slowdownHeadingItem = null;
            _gateHeadingItem = null;
            _headingFont = null;
            _realizedItemWidth = 0;
        }

        internal int SemanticChoiceCountForTesting { get { return Items.Count; } }
        internal static Color HeadingBandColorForTesting { get { return HeadingBandColor; } }
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
        internal Rectangle[] ChoiceBoundsForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                System.Collections.Generic.List<Rectangle> result =
                    new System.Collections.Generic.List<Rectangle>();
                for (int i = 0; i < _groupedDropDown.Items.Count; i++)
                {
                    ToolStripMenuItem item = _groupedDropDown.Items[i] as ToolStripMenuItem;
                    if (item != null) result.Add(item.Bounds);
                }
                return result.ToArray();
            }
        }
        internal IntPtr PopupHandleForTesting
        { get { return _groupedDropDown == null ? IntPtr.Zero : _groupedDropDown.Handle; } }
        internal Rectangle PopupScreenBoundsForTesting
        { get { return _groupedDropDown == null ? Rectangle.Empty : _groupedDropDown.Bounds; } }
        internal Rectangle PopupDisplayScreenBoundsForTesting
        {
            get
            {
                return _groupedDropDown == null ? Rectangle.Empty :
                    _groupedDropDown.RectangleToScreen(_groupedDropDown.DisplayRectangle);
            }
        }
        internal Rectangle[] HeadingScreenBoundsForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                return new Rectangle[] {
                    _groupedDropDown.RectangleToScreen(_slowdownHeadingItem.Bounds),
                    _groupedDropDown.RectangleToScreen(_gateHeadingItem.Bounds) };
            }
        }
        internal Rectangle[] ChoiceScreenBoundsForTesting
        {
            get
            {
                EnsureGroupedDropDown();
                System.Collections.Generic.List<Rectangle> result =
                    new System.Collections.Generic.List<Rectangle>();
                for (int i = 0; i < _groupedDropDown.Items.Count; i++)
                {
                    ToolStripMenuItem item = _groupedDropDown.Items[i] as ToolStripMenuItem;
                    if (item != null)
                        result.Add(_groupedDropDown.RectangleToScreen(item.Bounds));
                }
                return result.ToArray();
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
