using System;
using System.Drawing;
using System.Globalization;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class ChannelOverrideRequestEventArgs : EventArgs
    {
        internal readonly int Channel;
        internal readonly ChannelAttribute Attribute;
        internal readonly int Value;
        internal Exception Error;

        internal ChannelOverrideRequestEventArgs(int channel, ChannelAttribute attribute, int value)
        {
            Channel = channel;
            Attribute = attribute;
            Value = value;
        }
    }

    internal sealed class ChannelEnabledRequestEventArgs : EventArgs
    {
        internal readonly int Channel;
        internal readonly bool Enabled;
        internal Exception Error;
        internal ChannelEnabledRequestEventArgs(int channel, bool enabled) { Channel = channel; Enabled = enabled; }
    }

    internal sealed class ChannelChaseRequestEventArgs : EventArgs
    {
        internal readonly int Channel;
        internal readonly ChannelAttribute Attribute;
        internal Exception Error;
        private readonly Action<Exception> _completion;
        private int _completed;
        internal ChannelChaseRequestEventArgs(int channel, ChannelAttribute attribute, Action<Exception> completion)
        { Channel = channel; Attribute = attribute; _completion = completion; }
        internal void Complete(Exception error)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0) return;
            if (_completion != null) _completion(error);
        }
    }

    internal sealed class ChannelMonitorForm : Form
    {
        private readonly BufferedDataGridView _grid;
        private readonly Font _historicalFont;
        private readonly Font _forcedFont;
        private readonly Label _explanation;
        private readonly Label _detachedOverlay;
        private readonly Panel _gridHost;
        private readonly ScrubOrTypeTextBox _editor;
        private readonly ToolTip _feedbackTip = new ToolTip();
        private MidiChannelSnapshot[] _lastChannels;
        private MidiChannelSnapshot[] _sourceReadouts;
        private int _editorChannel = -1;
        private ChannelAttribute _editorAttribute;
        private bool _fittingWindow;
        private bool _detectManualResize;
        private bool _autoFitWindow = true;
        private bool _fitScheduled;

        internal event EventHandler<ChannelOverrideRequestEventArgs> OverrideRequested;
        internal event EventHandler<ChannelEnabledRequestEventArgs> ChannelEnabledRequested;
        internal event EventHandler<ChannelChaseRequestEventArgs> HistoricalChaseRequested;

        internal ChannelMonitorForm(string fileName)
        {
            ProductIcon.Apply(this);
            Text = "MIDIBottleneck Player — MIDI Channel Monitor — " + (fileName ?? String.Empty);
            StartPosition = FormStartPosition.Manual;
            ClientSize = new Size(1240, 430);
            MinimumSize = new Size(780, 400);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            _historicalFont = new Font(Font, FontStyle.Italic);
            _forcedFont = new Font(Font, FontStyle.Bold);

            TableLayoutPanel layout = new TableLayoutPanel();
            layout.Dock = DockStyle.Fill;
            layout.Padding = new Padding(6);
            layout.ColumnCount = 1;
            layout.RowCount = 2;
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            _grid = new BufferedDataGridView();
            _grid.Dock = DockStyle.Fill;
            _grid.ReadOnly = true;
            _grid.AllowUserToAddRows = false;
            _grid.AllowUserToDeleteRows = false;
            _grid.AllowUserToResizeRows = false;
            _grid.ColumnHeadersHeightSizeMode = DataGridViewColumnHeadersHeightSizeMode.DisableResizing;
            _grid.MultiSelect = false;
            _grid.SelectionMode = DataGridViewSelectionMode.CellSelect;
            _grid.RowHeadersVisible = false;
            _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.None;
            _grid.BackgroundColor = SystemColors.Window;
            AddColumn("Channel", "Ch", 48, "Click to enable or disable this MIDI channel. Disabled source events are filtered before scheduler admission and do not consume queue/service work.", null);
            AddColumn("KeysDown", "Polyphony", 72, "MIDI-observed held-key polyphony from successfully dispatched Note On/Off messages. This is not necessarily the synthesizer's internal voice count.", null);
            AddColumn("PeakKeys", "Peak polyphony", 90, "Highest MIDI-observed held-key polyphony since Reset stats.", null);
            AddColumn("Sent", "Sent", 76, "Successfully dispatched source channel messages. Override injections are not source events.", null);
            AddColumn("Dropped", "Dropped", 68, "Channel messages rejected by the selected finite-queue overflow policy.", null);
            AddColumn("Suppressed", "Override filtered", 108, "Source messages filtered before scheduler admission because they conflict with a forced channel value. They consume no queue/service work and are not queue-overflow drops.", null);
            string editHelp = " Click to type or drag horizontally. Right-click releases a forced value to historical Auto; right-click a historical value to restore the latest source value for that attribute.";
            AddColumn("BankMsb", "Bank MSB", 72, "Latest successfully dispatched Bank Select MSB (CC0)." + editHelp, ChannelAttribute.BankMsb);
            AddColumn("BankLsb", "Bank LSB", 72, "Latest successfully dispatched Bank Select LSB (CC32)." + editHelp, ChannelAttribute.BankLsb);
            AddColumn("Program", "Program", 158, "Latest Program Change, displayed as 1–128 with a General MIDI reference name. The actual sound can differ with non-GM banks, soundfonts, and synthesizers." + editHelp, ChannelAttribute.Program);
            AddColumn("Volume", "Volume", 60, "Latest Channel Volume (CC7)." + editHelp, ChannelAttribute.Volume);
            AddColumn("Expression", "Expr", 54, "Latest Expression (CC11)." + editHelp, ChannelAttribute.Expression);
            AddColumn("Pan", "Pan", 48, "Latest Pan (CC10)." + editHelp, ChannelAttribute.Pan);
            AddColumn("Sustain", "Sustain", 68, "Latest Sustain Pedal (CC64) state. Panic safety always sends sustain off before a forced On is reapplied at the next playback boundary." + editHelp, ChannelAttribute.Sustain);
            AddColumn("Bend", "Pitch bend", 76, "Latest pitch bend, centered at 0. Drag by 16 units per pixel or hold Shift for single-unit changes." + editHelp, ChannelAttribute.PitchBend);
            AddColumn("Aftertouch", "Aftertouch", 72, "Latest MIDI channel pressure/channel aftertouch." + editHelp, ChannelAttribute.Aftertouch);
            AddColumn("Position", "Position", 72, "Source timestamp of the most recent successfully dispatched channel message.", null);
            for (int channel = 0; channel < 16; channel++)
            {
                int row = _grid.Rows.Add();
                _grid.Rows[row].Cells[0].Value = (channel + 1).ToString(CultureInfo.CurrentCulture);
                for (int column = 1; column < _grid.Columns.Count; column++) _grid.Rows[row].Cells[column].Value = "—";
            }
            _grid.CellMouseDown += GridCellMouseDown;
            _grid.MouseMove += GridMouseMove;
            _grid.MouseUp += GridMouseUp;
            _grid.MouseLeave += delegate { if (!_editor.Visible) _grid.Cursor = Cursors.Default; };
            _grid.Scroll += delegate { HideEditor(); };
            _grid.ColumnWidthChanged += delegate { HideEditor(); ScheduleFitToGrid(); };
            Deactivate += delegate { HideEditor(); };

            _gridHost = new Panel { Dock = DockStyle.Fill, Margin = new Padding(0) };
            _gridHost.Controls.Add(_grid);
            _editor = new ScrubOrTypeTextBox { Visible = false };
            _editor.ValueRequested += EditorValueRequested;
            _editor.AutoRequested += EditorAutoRequested;
            _editor.ChaseRequested += EditorChaseRequested;
            _gridHost.Controls.Add(_editor);
            _detachedOverlay = new Label
            {
                Dock = DockStyle.Fill,
                TextAlign = ContentAlignment.MiddleCenter,
                BackColor = SystemColors.Window,
                ForeColor = Color.DimGray,
                Visible = false
            };
            _gridHost.Controls.Add(_detachedOverlay);

            _explanation = new Label();
            _explanation.AutoSize = true;
            _explanation.ForeColor = Color.DimGray;
            _explanation.Margin = new Padding(1, 5, 1, 1);
            _explanation.Text = "Dispatched MIDI state. Click a Ch cell to enable/disable. Attribute cells scrub or type; right-click releases force or restores one latest source value. Blue bold is forced; gray italic is historical.";
            layout.Controls.Add(_gridHost, 0, 0);
            layout.Controls.Add(_explanation, 0, 1);
            Controls.Add(layout);
            Shown += delegate
            {
                ApplyHeaderPreferredColumnWidths();
                FitWindowToGrid(true);
                BeginInvoke((MethodInvoker)delegate { _detectManualResize = true; });
            };
            Resize += delegate
            {
                if (_detectManualResize && !_fittingWindow) _autoFitWindow = false;
            };
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _historicalFont.Dispose();
                _forcedFont.Dispose();
                _feedbackTip.Dispose();
            }
            base.Dispose(disposing);
        }

        internal int ChannelRowCount { get { return _grid.Rows.Count; } }
        internal DataGridView GridForTesting { get { return _grid; } }
        internal ScrubOrTypeTextBox EditorForTesting { get { return _editor; } }
        internal int EditorControlCountForTesting { get { return CountControlsOfType<ScrubOrTypeTextBox>(_gridHost); } }

        internal string CellText(int channel, string columnName)
        {
            object value = _grid.Rows[channel].Cells[columnName].Value;
            return value == null ? String.Empty : Convert.ToString(value, CultureInfo.CurrentCulture);
        }

        internal Color CellForeColor(int channel, string columnName)
        {
            return _grid.Rows[channel].Cells[columnName].Style.ForeColor;
        }

        internal FontStyle CellFontStyle(int channel, string columnName)
        {
            Font font = _grid.Rows[channel].Cells[columnName].Style.Font;
            return font == null ? Font.Style : font.Style;
        }

        internal void ActivateEditorForTesting(int channel, string columnName)
        {
            ActivateEditor(channel, _grid.Columns[columnName].Index, new Point(20, 20), MouseButtons.None);
        }

        internal void RequestOverrideForTesting(int channel, ChannelAttribute attribute, int value)
        {
            RaiseOverrideRequested(channel, attribute, value);
        }

        internal void ShowControlError(int channel, ChannelAttribute attribute, string message)
        {
            if (channel < 0 || channel >= 16) return;
            ShowFeedback(_grid.Rows[channel].Cells[ColumnName(attribute)], message);
        }

        internal void UpdateSnapshot(ChannelPlaybackSnapshot snapshot)
        {
            if (snapshot == null || snapshot.Channels == null || snapshot.Channels.Length != 16) return;
            if (IsDetached) return;
            _lastChannels = snapshot.Channels;
            for (int channel = 0; channel < 16; channel++)
            {
                MidiChannelSnapshot state = snapshot.Channels[channel];
                DataGridViewRow row = _grid.Rows[channel];
                row.DefaultCellStyle.BackColor = state.Enabled ? SystemColors.Window : SystemColors.Control;
                row.Cells["Channel"].Value = state.Enabled
                    ? (channel + 1).ToString(CultureInfo.CurrentCulture)
                    : (channel + 1).ToString(CultureInfo.CurrentCulture) + " off";
                row.Cells["Channel"].ToolTipText = state.Enabled
                    ? "Channel enabled. Click to disable it."
                    : "Channel disabled; " + state.MutedFilteredEvents.ToString("N0", CultureInfo.CurrentCulture) +
                        " source channel event(s) filtered at output. Click to enable; no state chase is performed.";
                SetCell(channel, "KeysDown", state.KeysDown.ToString("N0", CultureInfo.CurrentCulture), false, false);
                SetCell(channel, "PeakKeys", state.PeakKeysDown.ToString("N0", CultureInfo.CurrentCulture), false, false);
                SetCell(channel, "Sent", state.SentEvents.ToString("N0", CultureInfo.CurrentCulture), false, false);
                SetCell(channel, "Dropped", state.DroppedEvents.ToString("N0", CultureInfo.CurrentCulture), false, false);
                SetCell(channel, "Suppressed", state.OverrideSuppressedEvents.ToString("N0", CultureInfo.CurrentCulture), false, false);
                SetAttributeCell(channel, "BankMsb", ChannelAttribute.BankMsb, state.BankMsb, state);
                SetAttributeCell(channel, "BankLsb", ChannelAttribute.BankLsb, state.BankLsb, state);
                SetAttributeCell(channel, "Program", ChannelAttribute.Program, state.Program, state);
                SetAttributeCell(channel, "Volume", ChannelAttribute.Volume, state.Volume, state);
                SetAttributeCell(channel, "Expression", ChannelAttribute.Expression, state.Expression, state);
                SetAttributeCell(channel, "Pan", ChannelAttribute.Pan, state.Pan, state);
                SetAttributeCell(channel, "Sustain", ChannelAttribute.Sustain, state.Sustain, state);
                SetAttributeCell(channel, "Bend", ChannelAttribute.PitchBend, state.PitchBend, state);
                SetAttributeCell(channel, "Aftertouch", ChannelAttribute.Aftertouch, state.ChannelPressure, state);
                SetCell(channel, "Position", state.LastDispatchedMicroseconds < 0 ? "—" : FormatClock(state.LastDispatchedMicroseconds), false, state.LastPositionHistorical);
            }
        }

        internal void SetSourceReadouts(ChannelPlaybackSnapshot source)
        {
            _sourceReadouts = source == null ? null : source.Channels;
        }

        internal bool IsDetached { get { return _detachedOverlay.Visible; } }
        internal string DetachedMessage { get { return _detachedOverlay.Text; } }

        internal void DetachForSongReplacement(string message)
        {
            HideEditor(false);
            _lastChannels = null;
            _sourceReadouts = null;
            ClearDisplayedState();
            _grid.Enabled = false;
            _detachedOverlay.Text = message ?? "No MIDI file is loaded.";
            _detachedOverlay.Visible = true;
            _detachedOverlay.BringToFront();
            _explanation.Enabled = false;
            Text = "MIDIBottleneck Player — MIDI Channel Monitor — no file";
        }

        internal void AttachSong(string fileName)
        {
            _detachedOverlay.Visible = false;
            _grid.Enabled = true;
            _explanation.Enabled = true;
            Text = "MIDIBottleneck Player — MIDI Channel Monitor — " + (fileName ?? String.Empty);
        }

        private void ClearDisplayedState()
        {
            for (int row = 0; row < _grid.Rows.Count; row++)
            {
                _grid.Rows[row].DefaultCellStyle.BackColor = SystemColors.Window;
                _grid.Rows[row].Cells["Channel"].Value = (row + 1).ToString(CultureInfo.CurrentCulture);
                _grid.Rows[row].Cells["Channel"].ToolTipText = String.Empty;
                for (int column = 1; column < _grid.Columns.Count; column++)
                {
                    DataGridViewCell cell = _grid.Rows[row].Cells[column];
                    cell.Value = "—";
                    cell.Style.ForeColor = SystemColors.ControlText;
                    cell.Style.Font = Font;
                    cell.ToolTipText = String.Empty;
                }
            }
        }

        private void AddColumn(string name, string caption, int width, string tooltip, ChannelAttribute? attribute)
        {
            DataGridViewTextBoxColumn column = new DataGridViewTextBoxColumn();
            column.Name = name; column.HeaderText = caption; column.Width = width;
            column.SortMode = DataGridViewColumnSortMode.NotSortable;
            column.HeaderCell.ToolTipText = tooltip;
            column.DefaultCellStyle.Alignment = name == "Channel" ? DataGridViewContentAlignment.MiddleCenter : DataGridViewContentAlignment.MiddleRight;
            if (attribute.HasValue) column.Tag = attribute.Value;
            _grid.Columns.Add(column);
        }

        private void SetAttributeCell(int channel, string columnName, ChannelAttribute attribute, int observed, MidiChannelSnapshot state)
        {
            int bit = 1 << (int)attribute;
            bool forced = (state.ForcedAttributeMask & bit) != 0;
            bool pending = (state.PendingForcedAttributeMask & bit) != 0;
            bool historical = !forced && (state.HistoricalAttributeMask & bit) != 0;
            int displayed = forced ? ForcedValue(state, attribute) : observed;
            bool sourceDerived = false;
            if (!forced && IsUnknownAttribute(attribute, displayed) && _sourceReadouts != null)
            {
                int source = ObservedValue(_sourceReadouts[channel], attribute);
                if (!IsUnknownAttribute(attribute, source))
                {
                    displayed = source;
                    historical = true;
                    sourceDerived = true;
                }
            }
            string value = FormatAttribute(attribute, displayed);
            SetCell(channel, columnName, value, forced, historical, pending);
            if (sourceDerived)
                _grid.Rows[channel].Cells[columnName].ToolTipText = value + Environment.NewLine +
                    "Source-file value immediately before this position. It has not been confirmed at MIDI output; queued or dropped events may differ.";
        }

        private static bool IsUnknownAttribute(ChannelAttribute attribute, int value)
        { return attribute == ChannelAttribute.PitchBend ? value == Int32.MinValue : value < 0; }

        private void SetCell(int channel, string columnName, string value, bool forced, bool historical)
        {
            SetCell(channel, columnName, value, forced, historical, false);
        }

        private void SetCell(int channel, string columnName, string value, bool forced, bool historical, bool pending)
        {
            DataGridViewCell cell = _grid.Rows[channel].Cells[columnName];
            if (!String.Equals(cell.Value as string, value, StringComparison.Ordinal)) cell.Value = value;
            Color color = forced ? Color.FromArgb(20, 75, 155) : historical ? Color.DimGray : SystemColors.ControlText;
            Font font = forced ? _forcedFont : historical ? _historicalFont : Font;
            if (cell.Style.ForeColor != color) cell.Style.ForeColor = color;
            if (!Object.ReferenceEquals(cell.Style.Font, font)) cell.Style.Font = font;
            cell.ToolTipText = forced
                ? (pending
                    ? "Forced override is configured but has not yet been applied successfully to the current output session."
                    : "Forced override is active. Conflicting source messages are filtered at the ordered output boundary.")
                : historical ? "Historical last-sent value; it is not enforced and may no longer be effective. Right-click to restore the latest source-file value for this attribute at the current position." : String.Empty;
            if (!String.IsNullOrEmpty(value) && value != "—")
                cell.ToolTipText = String.IsNullOrEmpty(cell.ToolTipText) ? value : value + Environment.NewLine + cell.ToolTipText;
        }

        private void GridMouseMove(object sender, MouseEventArgs e)
        {
            bool leftDown = (e.Button & MouseButtons.Left) != 0 || (Control.MouseButtons & MouseButtons.Left) != 0;
            if (_editor.Visible && _editor.GestureArmedForTesting)
                _editor.ContinuePointerGesture(_grid.PointToScreen(e.Location), (ModifierKeys & Keys.Shift) != 0, leftDown);
            DataGridView.HitTestInfo hit = _grid.HitTest(e.X, e.Y);
            ChannelAttribute attribute;
            _grid.Cursor = hit.RowIndex >= 0 && hit.ColumnIndex >= 0 && TryGetAttribute(hit.ColumnIndex, out attribute)
                ? Cursors.SizeWE : hit.RowIndex >= 0 && hit.ColumnIndex == _grid.Columns["Channel"].Index
                    ? Cursors.Hand : Cursors.Default;
        }

        private void GridMouseUp(object sender, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && _editor.Visible && _editor.GestureArmedForTesting)
                _editor.EndPointerGesture();
        }

        private void GridCellMouseDown(object sender, DataGridViewCellMouseEventArgs e)
        {
            if (e.RowIndex < 0 || e.ColumnIndex < 0) return;
            if (e.ColumnIndex == _grid.Columns["Channel"].Index)
            {
                HideEditor();
                if (e.Button == MouseButtons.Left && _lastChannels != null)
                    RaiseChannelEnabledRequested(e.RowIndex, !_lastChannels[e.RowIndex].Enabled);
                return;
            }
            ChannelAttribute attribute;
            if (!TryGetAttribute(e.ColumnIndex, out attribute)) { HideEditor(); return; }
            if (e.Button == MouseButtons.Right)
            {
                if (IsForced(e.RowIndex, attribute))
                    RaiseOverrideRequested(e.RowIndex, attribute, ChannelOverrideState.AutoValue);
                else if (IsHistorical(e.RowIndex, attribute))
                {
                    if (!_lastChannels[e.RowIndex].Enabled)
                        ShowFeedback(_grid.Rows[e.RowIndex].Cells[e.ColumnIndex], "Enable the channel before restoring its source value.");
                    else
                        RaiseHistoricalChaseRequested(e.RowIndex, attribute);
                }
                HideEditor();
                return;
            }
            if (e.Button == MouseButtons.Left)
                ActivateEditor(e.RowIndex, e.ColumnIndex, Control.MousePosition, MouseButtons.Left);
        }

        private bool TryGetAttribute(int columnIndex, out ChannelAttribute attribute)
        {
            object tag = _grid.Columns[columnIndex].Tag;
            if (tag is ChannelAttribute) { attribute = (ChannelAttribute)tag; return true; }
            attribute = ChannelAttribute.BankMsb; return false;
        }

        private void ActivateEditor(int channel, int columnIndex, Point screenPosition, MouseButtons button)
        {
            ChannelAttribute attribute;
            if (!TryGetAttribute(columnIndex, out attribute)) return;
            int current = CurrentAttributeValue(channel, attribute);
            bool forced = IsForced(channel, attribute);
            bool historical = IsHistorical(channel, attribute);
            Rectangle cell = _grid.GetCellDisplayRectangle(columnIndex, channel, true);
            Point screenTopLeft = _grid.PointToScreen(cell.Location);
            Point hostTopLeft = _gridHost.PointToClient(screenTopLeft);
            _editorChannel = channel;
            _editorAttribute = attribute;
            _editor.Bounds = new Rectangle(hostTopLeft.X + 2, hostTopLeft.Y + 2,
                Math.Max(12, cell.Width - 4), Math.Max(Font.Height + 3, cell.Height - 4));
            _editor.Font = forced ? _forcedFont : Font;
            _editor.ForeColor = forced ? Color.FromArgb(20, 75, 155) : SystemColors.ControlText;
            _editor.Configure(current, ChannelOverrideState.Minimum(attribute), ChannelOverrideState.Maximum(attribute),
                attribute == ChannelAttribute.Program ? 1 : 0,
                attribute == ChannelAttribute.PitchBend ? 16 : 1,
                attribute == ChannelAttribute.PitchBend ? 1 : 8,
                1, attribute == ChannelAttribute.PitchBend ? 1 : 16, forced, historical,
                delegate(int value) { return FormatAttribute(attribute, value); }, AttributeHelp(attribute));
            _editor.Visible = true;
            _editor.BringToFront();
            if (button != MouseButtons.None) _editor.BeginPointerGesture(screenPosition, button);
        }

        private void EditorValueRequested(object sender, ScrubValueEventArgs e)
        {
            if (_editorChannel < 0) return;
            e.Error = RaiseOverrideRequested(_editorChannel, _editorAttribute, e.Value);
            _editor.Font = _forcedFont;
            _editor.ForeColor = Color.FromArgb(20, 75, 155);
        }

        private void EditorAutoRequested(object sender, ScrubValueEventArgs e)
        {
            if (_editorChannel < 0) return;
            e.Error = RaiseOverrideRequested(_editorChannel, _editorAttribute, ChannelOverrideState.AutoValue);
            if (e.Error == null) HideEditor();
        }

        private void EditorChaseRequested(object sender, ScrubValueEventArgs e)
        {
            if (_editorChannel < 0) return;
            if (_lastChannels != null && !_lastChannels[_editorChannel].Enabled)
                e.Error = new InvalidOperationException("Enable this MIDI channel before restoring its source value.");
            else
                e.Error = RaiseHistoricalChaseRequested(_editorChannel, _editorAttribute);
            if (e.Error == null) HideEditor();
        }

        private void HideEditor()
        {
            HideEditor(true);
        }

        private void HideEditor(bool commitTypedValue)
        {
            if (_editor == null) return;
            _editor.FinishHostInteraction(commitTypedValue);
            _editor.Visible = false;
            _editorChannel = -1;
            _grid.Cursor = Cursors.Default;
        }

        private bool IsForced(int channel, ChannelAttribute attribute)
        {
            if (_lastChannels == null || channel < 0 || channel >= _lastChannels.Length) return false;
            return (_lastChannels[channel].ForcedAttributeMask & (1 << (int)attribute)) != 0;
        }

        private bool IsHistorical(int channel, ChannelAttribute attribute)
        {
            if (_lastChannels == null || channel < 0 || channel >= _lastChannels.Length) return false;
            if ((_lastChannels[channel].HistoricalAttributeMask & (1 << (int)attribute)) != 0) return true;
            return _sourceReadouts != null && !IsForced(channel, attribute) &&
                IsUnknownAttribute(attribute, ObservedValue(_lastChannels[channel], attribute)) &&
                !IsUnknownAttribute(attribute, ObservedValue(_sourceReadouts[channel], attribute));
        }

        private static string AttributeHelp(ChannelAttribute attribute)
        {
            switch (attribute)
            {
                case ChannelAttribute.BankMsb:
                case ChannelAttribute.BankLsb: return "0–127. Bank interpretation is synthesizer-specific. Drag one step per eight pixels, or one per sixteen with Shift.";
                case ChannelAttribute.Program: return "Program 1–128 with a General MIDI reference name; actual sounds may differ. Drag one step per eight pixels, or one per sixteen with Shift.";
                case ChannelAttribute.Volume: return "MIDI channel volume, 0–127. Drag one step per eight pixels, or one per sixteen with Shift.";
                case ChannelAttribute.Expression: return "MIDI expression, 0–127. Drag one step per eight pixels, or one per sixteen with Shift.";
                case ChannelAttribute.Pan: return "0 left, 64 center, 127 right. Drag one step per eight pixels, or one per sixteen with Shift.";
                case ChannelAttribute.Sustain: return "Off/On. Note-safety cleanup still sends sustain off before reapplying On.";
                case ChannelAttribute.PitchBend: return "−8192 through +8191. Drag by 16 units per pixel; hold Shift for one unit per pixel.";
                default: return "0–127 channel-wide pressure; drag one step per eight pixels or one per sixteen with Shift. The synthesizer may map or ignore it.";
            }
        }

        private Exception RaiseOverrideRequested(int channel, ChannelAttribute attribute, int value)
        {
            EventHandler<ChannelOverrideRequestEventArgs> handler = OverrideRequested;
            ChannelOverrideRequestEventArgs request = new ChannelOverrideRequestEventArgs(channel, attribute, value);
            if (handler != null) handler(this, request);
            return request.Error;
        }

        private Exception RaiseHistoricalChaseRequested(int channel, ChannelAttribute attribute)
        {
            EventHandler<ChannelChaseRequestEventArgs> handler = HistoricalChaseRequested;
            ChannelChaseRequestEventArgs request = new ChannelChaseRequestEventArgs(channel, attribute,
                delegate(Exception error) { CompleteHistoricalChase(channel, attribute, error); });
            if (handler != null) handler(this, request);
            else request.Error = new InvalidOperationException("No playback session is available.");
            if (request.Error != null)
                request.Complete(request.Error);
            return request.Error;
        }

        private void CompleteHistoricalChase(int channel, ChannelAttribute attribute, Exception error)
        {
            if (IsDisposed) return;
            if (InvokeRequired)
            {
                try { BeginInvoke((MethodInvoker)delegate { CompleteHistoricalChase(channel, attribute, error); }); }
                catch (InvalidOperationException) { }
                return;
            }
            if (error != null) ShowFeedback(_grid.Rows[channel].Cells[ColumnName(attribute)], error.Message);
        }

        private Exception RaiseChannelEnabledRequested(int channel, bool enabled)
        {
            EventHandler<ChannelEnabledRequestEventArgs> handler = ChannelEnabledRequested;
            ChannelEnabledRequestEventArgs request = new ChannelEnabledRequestEventArgs(channel, enabled);
            if (handler != null) handler(this, request);
            if (request.Error != null) ShowFeedback(_grid.Rows[channel].Cells["Channel"], request.Error.Message);
            return request.Error;
        }

        private void ShowFeedback(DataGridViewCell cell, string message)
        {
            if (cell == null || String.IsNullOrEmpty(message)) return;
            cell.ToolTipText = message;
            Rectangle bounds = _grid.GetCellDisplayRectangle(cell.ColumnIndex, cell.RowIndex, true);
            _feedbackTip.Show(message, _grid, bounds.Left, bounds.Bottom, 2500);
        }

        private static string ColumnName(ChannelAttribute attribute)
        {
            switch (attribute)
            {
                case ChannelAttribute.BankMsb: return "BankMsb";
                case ChannelAttribute.BankLsb: return "BankLsb";
                case ChannelAttribute.Program: return "Program";
                case ChannelAttribute.Volume: return "Volume";
                case ChannelAttribute.Expression: return "Expression";
                case ChannelAttribute.Pan: return "Pan";
                case ChannelAttribute.Sustain: return "Sustain";
                case ChannelAttribute.PitchBend: return "Bend";
                default: return "Aftertouch";
            }
        }

        private void ScheduleFitToGrid()
        {
            if (!_autoFitWindow || !IsHandleCreated || IsDisposed || _fittingWindow || _fitScheduled) return;
            _fitScheduled = true;
            BeginInvoke((MethodInvoker)delegate
            {
                _fitScheduled = false;
                if (!IsDisposed && _autoFitWindow) FitWindowToGrid(true);
            });
        }

        private void ApplyHeaderPreferredColumnWidths()
        {
            if (!IsHandleCreated) return;
            _fittingWindow = true;
            _grid.SuspendLayout();
            try
            {
                foreach (DataGridViewColumn column in _grid.Columns)
                {
                    if (String.Equals(column.Name, "Program", StringComparison.Ordinal))
                    {
                        column.Width = 158;
                        continue;
                    }
                    int preferred = column.GetPreferredWidth(DataGridViewAutoSizeColumnMode.ColumnHeader, true);
                    column.Width = Math.Max(34, preferred);
                }
            }
            finally
            {
                _grid.ResumeLayout();
                _fittingWindow = false;
            }
        }

        private void FitWindowToGrid(bool includeHeight)
        {
            if (!_autoFitWindow || !IsHandleCreated) return;
            int columns = 0;
            for (int i = 0; i < _grid.Columns.Count; i++)
                if (_grid.Columns[i].Visible) columns += _grid.Columns[i].Width;
            int gridWidth = columns + 3;
            int rowsHeight = _grid.ColumnHeadersHeight + _grid.Rows.GetRowsHeight(DataGridViewElementStates.Visible) + 3;
            Rectangle working = Screen.FromControl(this).WorkingArea;
            Size nonClient = new Size(Width - ClientSize.Width, Height - ClientSize.Height);
            int maximumClientWidth = Math.Max(1, working.Width - nonClient.Width);
            int maximumClientHeight = Math.Max(1, working.Height - nonClient.Height);
            int clientWidth = Math.Min(Math.Max(420, gridWidth + 12), maximumClientWidth);
            int explanationWidth = Math.Max(1, clientWidth - 14 - _explanation.Margin.Horizontal);
            _explanation.MaximumSize = new Size(explanationWidth, 0);
            _explanation.PerformLayout();
            int horizontalScrollHeight = clientWidth < gridWidth + 12 ? SystemInformation.HorizontalScrollBarHeight : 0;
            int desiredHeight = rowsHeight + horizontalScrollHeight + _explanation.PreferredHeight + _explanation.Margin.Vertical + 12;
            int clientHeight = includeHeight ? Math.Min(Math.Max(300, desiredHeight), maximumClientHeight) : ClientSize.Height;
            _fittingWindow = true;
            try
            {
                ClientSize = new Size(clientWidth, clientHeight);
                int x = Math.Max(working.Left, Math.Min(Left, working.Right - Width));
                int y = Math.Max(working.Top, Math.Min(Top, working.Bottom - Height));
                Location = new Point(x, y);
            }
            finally { _fittingWindow = false; }
        }

        internal bool AutoFitEnabledForTesting { get { return _autoFitWindow; } }
        internal void FitWindowToGridForTesting(bool includeHeight) { FitWindowToGrid(includeHeight); }

        private int CurrentAttributeValue(int channel, ChannelAttribute attribute)
        {
            if (_lastChannels == null || channel < 0 || channel >= _lastChannels.Length)
                return DefaultEditorSeed(attribute);
            MidiChannelSnapshot state = _lastChannels[channel];
            int bit = 1 << (int)attribute;
            int value = (state.ForcedAttributeMask & bit) != 0 ? ForcedValue(state, attribute) : ObservedValue(state, attribute);
            if (IsUnknownAttribute(attribute, value) && _sourceReadouts != null)
                value = ObservedValue(_sourceReadouts[channel], attribute);
            return value == Int32.MinValue || value < 0 && attribute != ChannelAttribute.PitchBend
                ? DefaultEditorSeed(attribute) : value;
        }

        private static int DefaultEditorSeed(ChannelAttribute attribute)
        {
            switch (attribute)
            {
                case ChannelAttribute.Volume: return 100;
                case ChannelAttribute.Expression: return 127;
                case ChannelAttribute.Pan: return 64;
                case ChannelAttribute.PitchBend: return 0;
                default: return 0;
            }
        }

        private static int ObservedValue(MidiChannelSnapshot state, ChannelAttribute attribute)
        {
            switch (attribute)
            {
                case ChannelAttribute.BankMsb: return state.BankMsb;
                case ChannelAttribute.BankLsb: return state.BankLsb;
                case ChannelAttribute.Program: return state.Program;
                case ChannelAttribute.Volume: return state.Volume;
                case ChannelAttribute.Expression: return state.Expression;
                case ChannelAttribute.Pan: return state.Pan;
                case ChannelAttribute.Sustain: return state.Sustain;
                case ChannelAttribute.PitchBend: return state.PitchBend;
                default: return state.ChannelPressure;
            }
        }

        private static int ForcedValue(MidiChannelSnapshot state, ChannelAttribute attribute)
        {
            switch (attribute)
            {
                case ChannelAttribute.BankMsb: return state.ForcedBankMsb;
                case ChannelAttribute.BankLsb: return state.ForcedBankLsb;
                case ChannelAttribute.Program: return state.ForcedProgram;
                case ChannelAttribute.Volume: return state.ForcedVolume;
                case ChannelAttribute.Expression: return state.ForcedExpression;
                case ChannelAttribute.Pan: return state.ForcedPan;
                case ChannelAttribute.Sustain: return state.ForcedSustain;
                case ChannelAttribute.PitchBend: return state.ForcedPitchBend;
                default: return state.ForcedAftertouch;
            }
        }

        private static string FormatAttribute(ChannelAttribute attribute, int value)
        {
            if (value == Int32.MinValue || value < 0 && attribute != ChannelAttribute.PitchBend) return "—";
            if (attribute == ChannelAttribute.Program) return (value + 1).ToString(CultureInfo.CurrentCulture) + " — " + GeneralMidiNames[value];
            if (attribute == ChannelAttribute.Sustain) return value == 0 ? "Off" : "On";
            if (attribute == ChannelAttribute.PitchBend) return value.ToString("+#;-#;0", CultureInfo.CurrentCulture);
            return value.ToString(CultureInfo.CurrentCulture);
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00", CultureInfo.InvariantCulture) + ":" + value.Seconds.ToString("00", CultureInfo.InvariantCulture) + "." + value.Milliseconds.ToString("000", CultureInfo.InvariantCulture);
        }

        private static readonly string[] GeneralMidiNames = new string[]
        {
            "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
            "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
            "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
            "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
            "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
            "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
            "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag Pipe", "Fiddle", "Shanai",
            "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"
        };

        private static int CountControlsOfType<T>(Control parent) where T : Control
        {
            int count = parent is T ? 1 : 0;
            for (int i = 0; i < parent.Controls.Count; i++) count += CountControlsOfType<T>(parent.Controls[i]);
            return count;
        }

        private sealed class BufferedDataGridView : DataGridView { internal BufferedDataGridView() { DoubleBuffered = true; } }
    }
}
