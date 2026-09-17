using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class ChannelOverrideRequestEventArgs : EventArgs
    {
        internal readonly int Channel;
        internal readonly ChannelAttribute Attribute;
        internal readonly int Value;

        internal ChannelOverrideRequestEventArgs(int channel, ChannelAttribute attribute, int value)
        {
            Channel = channel;
            Attribute = attribute;
            Value = value;
        }
    }

    internal sealed class ChannelMonitorForm : Form
    {
        private readonly BufferedDataGridView _grid;
        private readonly Font _forcedFont;
        private readonly Font _historicalFont;

        internal event EventHandler<ChannelOverrideRequestEventArgs> OverrideRequested;

        internal ChannelMonitorForm(string fileName)
        {
            Text = "MIDI Channel Monitor — " + (fileName ?? String.Empty);
            StartPosition = FormStartPosition.Manual;
            ClientSize = new Size(1350, 520);
            MinimumSize = new Size(780, 400);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            _forcedFont = new Font(Font, FontStyle.Bold);
            _historicalFont = new Font(Font, FontStyle.Italic);

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
            _grid.MultiSelect = false;
            _grid.SelectionMode = DataGridViewSelectionMode.CellSelect;
            _grid.RowHeadersVisible = false;
            _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.None;
            _grid.BackgroundColor = SystemColors.Window;
            AddColumn("Channel", "Ch", 42, "MIDI channel 1–16.", null);
            AddColumn("KeysDown", "Keys down", 72, "Distinct MIDI keys currently held according to successfully dispatched Note On/Off messages. This is not synthesizer polyphony.", null);
            AddColumn("PeakKeys", "Peak keys", 68, "Highest Keys down value since Reset stats.", null);
            AddColumn("Sent", "Sent", 76, "Successfully dispatched source channel messages. Override injections are not source events.", null);
            AddColumn("Dropped", "Dropped", 68, "Channel messages rejected by the selected finite-queue overflow policy.", null);
            AddColumn("Suppressed", "Override filtered", 116, "Source messages suppressed only at the ordered output boundary because they conflicted with a forced channel value. These are not queue-overflow drops.", null);
            AddColumn("BankMsb", "Bank MSB", 72, "Latest successfully dispatched Bank Select MSB (CC0). Right-click or double-click to force a value or return to Auto.", ChannelAttribute.BankMsb);
            AddColumn("BankLsb", "Bank LSB", 72, "Latest successfully dispatched Bank Select LSB (CC32). Right-click or double-click to force a value or return to Auto.", ChannelAttribute.BankLsb);
            AddColumn("Program", "Program", 190, "Latest Program Change, displayed as 1–128 with a General MIDI reference name. The actual sound can differ with non-GM banks, soundfonts, and synthesizers.", ChannelAttribute.Program);
            AddColumn("Volume", "Volume", 60, "Latest Channel Volume (CC7).", ChannelAttribute.Volume);
            AddColumn("Expression", "Expr", 54, "Latest Expression (CC11).", ChannelAttribute.Expression);
            AddColumn("Pan", "Pan", 48, "Latest Pan (CC10).", ChannelAttribute.Pan);
            AddColumn("Sustain", "Sustain", 68, "Latest Sustain Pedal (CC64) state. Panic safety always sends sustain off before a forced On is reapplied at the next playback boundary.", ChannelAttribute.Sustain);
            AddColumn("Bend", "Pitch bend", 76, "Latest pitch bend, centered at 0.", ChannelAttribute.PitchBend);
            AddColumn("Aftertouch", "Aftertouch", 72, "Latest MIDI channel pressure/channel aftertouch.", ChannelAttribute.Aftertouch);
            AddColumn("Position", "MIDI output position", 116, "Source timestamp of the most recent successfully dispatched channel message.", null);
            for (int channel = 0; channel < 16; channel++)
            {
                int row = _grid.Rows.Add();
                _grid.Rows[row].Cells[0].Value = (channel + 1).ToString(CultureInfo.CurrentCulture);
                for (int column = 1; column < _grid.Columns.Count; column++) _grid.Rows[row].Cells[column].Value = "—";
            }
            _grid.CellMouseClick += GridCellMouseClick;
            _grid.CellDoubleClick += GridCellDoubleClick;

            Label explanation = new Label();
            explanation.AutoSize = true;
            explanation.ForeColor = Color.DimGray;
            explanation.Margin = new Padding(1, 5, 1, 1);
            explanation.Text = "Dispatched MIDI state; Keys down is not synth voice count. Right-click an attribute to use Auto or force it. Blue bold values are forced; gray italic values were last observed before an output reset and may no longer be active.";
            layout.Controls.Add(_grid, 0, 0);
            layout.Controls.Add(explanation, 0, 1);
            Controls.Add(layout);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) { _forcedFont.Dispose(); _historicalFont.Dispose(); }
            base.Dispose(disposing);
        }

        internal int ChannelRowCount { get { return _grid.Rows.Count; } }

        internal string CellText(int channel, string columnName)
        {
            object value = _grid.Rows[channel].Cells[columnName].Value;
            return value == null ? String.Empty : Convert.ToString(value, CultureInfo.CurrentCulture);
        }

        internal Color CellForeColor(int channel, string columnName)
        {
            return _grid.Rows[channel].Cells[columnName].Style.ForeColor;
        }

        internal void RequestOverrideForTesting(int channel, ChannelAttribute attribute, int value)
        {
            RaiseOverrideRequested(channel, attribute, value);
        }

        internal void UpdateSnapshot(ChannelPlaybackSnapshot snapshot)
        {
            if (snapshot == null || snapshot.Channels == null || snapshot.Channels.Length != 16) return;
            for (int channel = 0; channel < 16; channel++)
            {
                MidiChannelSnapshot state = snapshot.Channels[channel];
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
            string value = FormatAttribute(attribute, displayed);
            if (forced) value = "F: " + value + (pending ? " (pending)" : String.Empty);
            SetCell(channel, columnName, value, forced, historical);
        }

        private void SetCell(int channel, string columnName, string value, bool forced, bool historical)
        {
            DataGridViewCell cell = _grid.Rows[channel].Cells[columnName];
            if (!String.Equals(cell.Value as string, value, StringComparison.Ordinal)) cell.Value = value;
            Color color = forced ? Color.FromArgb(20, 75, 155) : historical ? Color.DimGray : SystemColors.ControlText;
            Font font = forced ? _forcedFont : historical ? _historicalFont : Font;
            if (cell.Style.ForeColor != color) cell.Style.ForeColor = color;
            if (!Object.ReferenceEquals(cell.Style.Font, font)) cell.Style.Font = font;
            cell.ToolTipText = forced
                ? (value.IndexOf("pending", StringComparison.Ordinal) >= 0
                    ? "Forced override is configured but has not yet been applied successfully to the current output session."
                    : "Forced override is active. Conflicting source messages are filtered at the ordered output boundary.")
                : historical ? "Last observed before an output reset; the provider's current effective value may differ." : String.Empty;
        }

        private void GridCellMouseClick(object sender, DataGridViewCellMouseEventArgs e)
        {
            if (e.Button != MouseButtons.Right || e.RowIndex < 0 || e.ColumnIndex < 0) return;
            ChannelAttribute attribute;
            if (!TryGetAttribute(e.ColumnIndex, out attribute)) return;
            _grid.CurrentCell = _grid.Rows[e.RowIndex].Cells[e.ColumnIndex];
            ContextMenuStrip menu = new ContextMenuStrip();
            ToolStripMenuItem force = new ToolStripMenuItem("Force value…");
            force.Click += delegate { PromptForOverride(e.RowIndex, attribute); };
            ToolStripMenuItem automatic = new ToolStripMenuItem("Use Auto");
            automatic.Click += delegate { RaiseOverrideRequested(e.RowIndex, attribute, ChannelOverrideState.AutoValue); };
            menu.Items.Add(force); menu.Items.Add(automatic);
            menu.Closed += delegate { menu.Dispose(); };
            menu.Show(_grid, _grid.PointToClient(Cursor.Position));
        }

        private void GridCellDoubleClick(object sender, DataGridViewCellEventArgs e)
        {
            if (e.RowIndex < 0 || e.ColumnIndex < 0) return;
            ChannelAttribute attribute;
            if (TryGetAttribute(e.ColumnIndex, out attribute)) PromptForOverride(e.RowIndex, attribute);
        }

        private bool TryGetAttribute(int columnIndex, out ChannelAttribute attribute)
        {
            object tag = _grid.Columns[columnIndex].Tag;
            if (tag is ChannelAttribute) { attribute = (ChannelAttribute)tag; return true; }
            attribute = ChannelAttribute.BankMsb; return false;
        }

        private void PromptForOverride(int channel, ChannelAttribute attribute)
        {
            string display = _grid.CurrentCell == null ? String.Empty : Convert.ToString(_grid.CurrentCell.Value, CultureInfo.CurrentCulture);
            using (ChannelOverrideDialog dialog = new ChannelOverrideDialog(channel, attribute, display))
                if (dialog.ShowDialog(this) == DialogResult.OK) RaiseOverrideRequested(channel, attribute, dialog.OverrideValue);
        }

        private void RaiseOverrideRequested(int channel, ChannelAttribute attribute, int value)
        {
            EventHandler<ChannelOverrideRequestEventArgs> handler = OverrideRequested;
            if (handler != null) handler(this, new ChannelOverrideRequestEventArgs(channel, attribute, value));
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

        private sealed class BufferedDataGridView : DataGridView { internal BufferedDataGridView() { DoubleBuffered = true; } }

        private sealed class ChannelOverrideDialog : Form
        {
            private readonly NumericUpDown _value;
            internal int OverrideValue { get { return Decimal.ToInt32(_value.Value); } }

            internal ChannelOverrideDialog(int channel, ChannelAttribute attribute, string display)
            {
                Text = "Force channel " + (channel + 1).ToString(CultureInfo.CurrentCulture) + " " + FriendlyName(attribute);
                FormBorderStyle = FormBorderStyle.FixedDialog; StartPosition = FormStartPosition.CenterParent;
                MinimizeBox = false; MaximizeBox = false; ShowInTaskbar = false; ClientSize = new Size(330, 92);
                Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
                Label label = new Label { Text = FriendlyName(attribute) + ":", AutoSize = true, Location = new Point(10, 15) };
                _value = new NumericUpDown { Minimum = ChannelOverrideState.Minimum(attribute), Maximum = ChannelOverrideState.Maximum(attribute), Width = 130, Location = new Point(145, 11) };
                int suggested = SuggestedValue(attribute, display);
                _value.Value = Math.Max(_value.Minimum, Math.Min(_value.Maximum, suggested));
                Button okay = new Button { Text = "Force", DialogResult = DialogResult.OK, AutoSize = true, Location = new Point(158, 51) };
                Button cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true, Location = new Point(238, 51) };
                Controls.Add(label); Controls.Add(_value); Controls.Add(okay); Controls.Add(cancel); AcceptButton = okay; CancelButton = cancel;
            }

            private static string FriendlyName(ChannelAttribute attribute)
            {
                switch (attribute)
                {
                    case ChannelAttribute.BankMsb: return "Bank Select MSB";
                    case ChannelAttribute.BankLsb: return "Bank Select LSB";
                    case ChannelAttribute.Program: return "Program (1–128)";
                    case ChannelAttribute.Expression: return "Expression";
                    case ChannelAttribute.PitchBend: return "Pitch bend";
                    case ChannelAttribute.Aftertouch: return "Channel aftertouch";
                    default: return attribute.ToString();
                }
            }

            private static int SuggestedValue(ChannelAttribute attribute, string display)
            {
                if (attribute == ChannelAttribute.Sustain) return display.IndexOf("On", StringComparison.OrdinalIgnoreCase) >= 0 ? 1 : 0;
                int result; string digits = String.Empty;
                for (int i = 0; i < display.Length; i++)
                {
                    char c = display[i];
                    if (Char.IsDigit(c) || c == '-' && digits.Length == 0) digits += c;
                    else if (digits.Length > 0) break;
                }
                if (!Int32.TryParse(digits, NumberStyles.Integer, CultureInfo.CurrentCulture, out result)) result = 0;
                if (attribute == ChannelAttribute.Program && result > 0) result--;
                return result;
            }
        }
    }
}
