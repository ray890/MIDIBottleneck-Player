using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class ChannelMonitorForm : Form
    {
        private readonly BufferedDataGridView _grid;
        private readonly Label _explanation;

        internal ChannelMonitorForm(string fileName)
        {
            Text = "MIDI Channel Monitor — " + (fileName ?? String.Empty);
            StartPosition = FormStartPosition.Manual;
            ClientSize = new Size(1160, 520);
            MinimumSize = new Size(760, 400);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

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
            _grid.SelectionMode = DataGridViewSelectionMode.FullRowSelect;
            _grid.RowHeadersVisible = false;
            _grid.AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.None;
            _grid.BackgroundColor = SystemColors.Window;
            AddColumn("Channel", "Ch", 42, "MIDI channel 1–16.");
            AddColumn("KeysDown", "Keys down", 72, "Distinct MIDI keys currently held according to successfully dispatched Note On/Off messages. This is not synthesizer polyphony.");
            AddColumn("PeakKeys", "Peak keys", 68, "Highest Keys down value since Reset stats.");
            AddColumn("Sent", "Sent", 76, "Successfully dispatched channel messages. With None selected, these are logically consumed messages.");
            AddColumn("Dropped", "Dropped", 68, "Channel messages rejected by the selected finite-queue overflow policy.");
            AddColumn("Bank", "Bank", 70, "Latest successfully dispatched Bank Select MSB/LSB, when known.");
            AddColumn("Program", "Program", 66, "Latest successfully dispatched MIDI Program Change, displayed as 1–128.");
            AddColumn("Volume", "Volume", 60, "Latest successfully dispatched Channel Volume (CC7).");
            AddColumn("Expression", "Expr", 54, "Latest successfully dispatched Expression (CC11).");
            AddColumn("Pan", "Pan", 48, "Latest successfully dispatched Pan (CC10).");
            AddColumn("Sustain", "Sustain", 58, "Latest successfully dispatched Sustain Pedal (CC64) state.");
            AddColumn("Bend", "Pitch bend", 76, "Latest successfully dispatched pitch bend, centered at 0.");
            AddColumn("Pressure", "Pressure", 64, "Latest successfully dispatched channel pressure.");
            AddColumn("Position", "MIDI output position", 116, "Source timestamp of the most recent successfully dispatched channel message.");
            for (int channel = 0; channel < 16; channel++)
            {
                int row = _grid.Rows.Add();
                _grid.Rows[row].Cells[0].Value = (channel + 1).ToString(CultureInfo.CurrentCulture);
                for (int column = 1; column < _grid.Columns.Count; column++)
                    _grid.Rows[row].Cells[column].Value = "—";
            }

            _explanation = new Label();
            _explanation.AutoSize = true;
            _explanation.ForeColor = Color.DimGray;
            _explanation.Margin = new Padding(1, 5, 1, 1);
            _explanation.Text = "Read-only dispatched MIDI state from when this window was opened. Open it before Play for full-session counts. Keys down is not synthesizer voice count; system messages are not assigned to a row.";
            layout.Controls.Add(_grid, 0, 0);
            layout.Controls.Add(_explanation, 0, 1);
            Controls.Add(layout);
        }

        internal int ChannelRowCount { get { return _grid.Rows.Count; } }

        internal string CellText(int channel, string columnName)
        {
            object value = _grid.Rows[channel].Cells[columnName].Value;
            return value == null ? String.Empty : Convert.ToString(value, CultureInfo.CurrentCulture);
        }

        internal void UpdateSnapshot(ChannelPlaybackSnapshot snapshot)
        {
            if (snapshot == null || snapshot.Channels == null || snapshot.Channels.Length != 16) return;
            for (int channel = 0; channel < 16; channel++)
            {
                MidiChannelSnapshot state = snapshot.Channels[channel];
                SetCell(channel, "KeysDown", state.KeysDown.ToString("N0", CultureInfo.CurrentCulture));
                SetCell(channel, "PeakKeys", state.PeakKeysDown.ToString("N0", CultureInfo.CurrentCulture));
                SetCell(channel, "Sent", state.SentEvents.ToString("N0", CultureInfo.CurrentCulture));
                SetCell(channel, "Dropped", state.DroppedEvents.ToString("N0", CultureInfo.CurrentCulture));
                SetCell(channel, "Bank", FormatBank(state.BankMsb, state.BankLsb));
                SetCell(channel, "Program", state.Program < 0 ? "—" : (state.Program + 1).ToString(CultureInfo.CurrentCulture));
                SetCell(channel, "Volume", FormatKnown(state.Volume));
                SetCell(channel, "Expression", FormatKnown(state.Expression));
                SetCell(channel, "Pan", FormatKnown(state.Pan));
                SetCell(channel, "Sustain", state.Sustain < 0 ? "—" : state.Sustain == 0 ? "Off" : "On");
                SetCell(channel, "Bend", state.PitchBend == Int32.MinValue ? "—" : state.PitchBend.ToString("+#;-#;0", CultureInfo.CurrentCulture));
                SetCell(channel, "Pressure", FormatKnown(state.ChannelPressure));
                SetCell(channel, "Position", state.LastDispatchedMicroseconds < 0 ? "—" : FormatClock(state.LastDispatchedMicroseconds));
            }
        }

        private void AddColumn(string name, string caption, int width, string tooltip)
        {
            DataGridViewTextBoxColumn column = new DataGridViewTextBoxColumn();
            column.Name = name;
            column.HeaderText = caption;
            column.Width = width;
            column.SortMode = DataGridViewColumnSortMode.NotSortable;
            column.HeaderCell.ToolTipText = tooltip;
            column.DefaultCellStyle.Alignment = name == "Channel" ? DataGridViewContentAlignment.MiddleCenter : DataGridViewContentAlignment.MiddleRight;
            _grid.Columns.Add(column);
        }

        private void SetCell(int channel, string columnName, string value)
        {
            DataGridViewCell cell = _grid.Rows[channel].Cells[columnName];
            string previous = cell.Value as string;
            if (!String.Equals(previous, value, StringComparison.Ordinal)) cell.Value = value;
        }

        private static string FormatKnown(int value)
        {
            return value < 0 ? "—" : value.ToString(CultureInfo.CurrentCulture);
        }

        private static string FormatBank(int msb, int lsb)
        {
            if (msb < 0 && lsb < 0) return "—";
            return (msb < 0 ? "—" : msb.ToString(CultureInfo.CurrentCulture)) + "/" +
                (lsb < 0 ? "—" : lsb.ToString(CultureInfo.CurrentCulture));
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00", CultureInfo.InvariantCulture) + ":" +
                value.Seconds.ToString("00", CultureInfo.InvariantCulture) + "." + value.Milliseconds.ToString("000", CultureInfo.InvariantCulture);
        }

        private sealed class BufferedDataGridView : DataGridView
        {
            internal BufferedDataGridView()
            {
                DoubleBuffered = true;
            }
        }
    }
}
