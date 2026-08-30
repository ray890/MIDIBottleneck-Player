using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class MainForm : Form
    {
        private readonly PlaybackEngine _engine = new PlaybackEngine();
        private readonly WindowsMidiOutput _output = new WindowsMidiOutput();
        private MidiSong _song;
        private MidiSong _engineSong;
        private bool _updatingProcessingControls;
        private bool _draggingSeek;
        private long _selectedPositionMicroseconds;

        private Label _fileLabel;
        private Label _fileInfoLabel;
        private ComboBox _outputCombo;
        private RadioButton _queueRadio;
        private RadioButton _dropRadio;
        private NumericUpDown _processingValue;
        private ComboBox _processingUnit;
        private TrackBar _processingSlider;
        private Label _processingSummary;
        private Button _playButton;
        private Button _pauseButton;
        private Button _stopButton;
        private Label _stateLabel;
        private TrackBar _seekBar;
        private Label _seekPositionLabel;
        private Button _seekBack10Button;
        private Button _seekBack5Button;
        private Button _seekForward5Button;
        private Button _seekForward10Button;
        private Label _rateValue;
        private Label _queueValue;
        private Label _maxQueueValue;
        private Label _processedValue;
        private Label _droppedValue;
        private Label _playbackValue;
        private Label _intendedValue;
        private Label _dispatchedValue;
        private Label _lagValue;
        private Label _maxLagValue;
        private Timer _uiTimer;

        public MainForm()
        {
            Text = "MIDI Event Bottleneck Simulator";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(760, 650);
            ClientSize = new Size(850, 720);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

            BuildInterface();
            LoadOutputDevices();
            SetProcessingMicroseconds(100);
            UpdateTransportControls();

            _engine.PlaybackEnded += EnginePlaybackEnded;
            _engine.PlaybackFailed += EnginePlaybackFailed;
            _uiTimer = new Timer();
            _uiTimer.Interval = 100;
            _uiTimer.Tick += delegate { RefreshStatistics(); };
            _uiTimer.Start();
        }

        private void BuildInterface()
        {
            TableLayoutPanel root = new TableLayoutPanel();
            root.Dock = DockStyle.Fill;
            root.Padding = new Padding(12);
            root.ColumnCount = 1;
            root.RowCount = 5;
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            Controls.Add(root);

            root.Controls.Add(BuildFileAndOutputGroup(), 0, 0);
            root.Controls.Add(BuildProcessingGroup(), 0, 1);
            root.Controls.Add(BuildPlaybackGroup(), 0, 2);
            root.Controls.Add(BuildStatisticsGroup(), 0, 3);

            Label note = new Label();
            note.Dock = DockStyle.Fill;
            note.Padding = new Padding(3, 9, 3, 0);
            note.ForeColor = Color.DimGray;
            note.Text = "Timing model: source timestamps are resolved through the file's complete tempo map, then each dispatchable MIDI event enters a single-server processor. Processing time changes apply to the next event that begins service.";
            root.Controls.Add(note, 0, 4);
        }

        private Control BuildFileAndOutputGroup()
        {
            GroupBox group = NewGroup("File and output");
            TableLayoutPanel table = NewTable(3);
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            Button open = new Button();
            open.Text = "Open MIDI...";
            open.AutoSize = true;
            open.Click += OpenMidiClicked;
            table.Controls.Add(open, 0, 0);

            _fileLabel = new Label();
            _fileLabel.Text = "No file loaded";
            _fileLabel.AutoEllipsis = true;
            _fileLabel.Dock = DockStyle.Fill;
            _fileLabel.TextAlign = ContentAlignment.MiddleLeft;
            table.Controls.Add(_fileLabel, 1, 0);

            _fileInfoLabel = new Label();
            _fileInfoLabel.Text = "";
            _fileInfoLabel.AutoSize = true;
            _fileInfoLabel.TextAlign = ContentAlignment.MiddleRight;
            table.Controls.Add(_fileInfoLabel, 2, 0);

            Label outputLabel = new Label();
            outputLabel.Text = "MIDI output:";
            outputLabel.AutoSize = true;
            outputLabel.Anchor = AnchorStyles.Left;
            table.Controls.Add(outputLabel, 0, 1);

            _outputCombo = new ComboBox();
            _outputCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _outputCombo.Dock = DockStyle.Fill;
            table.Controls.Add(_outputCombo, 1, 1);
            table.SetColumnSpan(_outputCombo, 2);

            group.Controls.Add(table);
            return group;
        }

        private Control BuildProcessingGroup()
        {
            GroupBox group = NewGroup("Processing model");
            TableLayoutPanel table = NewTable(4);
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            _queueRadio = new RadioButton();
            _queueRadio.Text = "Queue events / simulate slowdown";
            _queueRadio.AutoSize = true;
            _queueRadio.Checked = true;
            table.Controls.Add(_queueRadio, 0, 0);
            table.SetColumnSpan(_queueRadio, 2);

            _dropRadio = new RadioButton();
            _dropRadio.Text = "Drop events when busy";
            _dropRadio.AutoSize = true;
            table.Controls.Add(_dropRadio, 2, 0);
            table.SetColumnSpan(_dropRadio, 2);

            Label valueLabel = new Label();
            valueLabel.Text = "Processing time per event:";
            valueLabel.AutoSize = true;
            valueLabel.Anchor = AnchorStyles.Left;
            table.Controls.Add(valueLabel, 0, 1);

            _processingValue = new NumericUpDown();
            _processingValue.Minimum = 0;
            _processingValue.Maximum = 1000000;
            _processingValue.Width = 130;
            _processingValue.ThousandsSeparator = true;
            _processingValue.ValueChanged += ProcessingValueChanged;
            table.Controls.Add(_processingValue, 2, 1);

            _processingUnit = new ComboBox();
            _processingUnit.DropDownStyle = ComboBoxStyle.DropDownList;
            _processingUnit.Items.Add("µs");
            _processingUnit.Items.Add("ms");
            _processingUnit.SelectedIndex = 0;
            _processingUnit.Width = 70;
            _processingUnit.SelectedIndexChanged += ProcessingUnitChanged;
            table.Controls.Add(_processingUnit, 3, 1);

            _processingSlider = new TrackBar();
            _processingSlider.Minimum = 0;
            _processingSlider.Maximum = 1000;
            _processingSlider.TickFrequency = 100;
            _processingSlider.Dock = DockStyle.Fill;
            _processingSlider.AutoSize = true;
            _processingSlider.ValueChanged += ProcessingSliderChanged;
            table.Controls.Add(_processingSlider, 0, 2);
            table.SetColumnSpan(_processingSlider, 4);

            _processingSummary = new Label();
            _processingSummary.AutoSize = true;
            _processingSummary.ForeColor = Color.DimGray;
            table.Controls.Add(_processingSummary, 0, 3);
            table.SetColumnSpan(_processingSummary, 4);

            group.Controls.Add(table);
            return group;
        }

        private Control BuildPlaybackGroup()
        {
            GroupBox group = NewGroup("Playback");
            TableLayoutPanel layout = NewTable(2);
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 150));

            _seekBar = new TrackBar();
            _seekBar.Minimum = 0;
            _seekBar.Maximum = 1000000;
            _seekBar.TickStyle = TickStyle.None;
            _seekBar.Dock = DockStyle.Fill;
            _seekBar.Enabled = false;
            _seekBar.MouseDown += delegate { _draggingSeek = true; };
            _seekBar.MouseUp += delegate { _draggingSeek = false; PerformSeek(SeekBarToMicroseconds(_seekBar.Value)); };
            _seekBar.KeyUp += delegate { PerformSeek(SeekBarToMicroseconds(_seekBar.Value)); };
            _seekBar.ValueChanged += SeekBarValueChanged;
            layout.Controls.Add(_seekBar, 0, 0);

            _seekPositionLabel = new Label();
            _seekPositionLabel.Text = "00:00.000 / 00:00.000";
            _seekPositionLabel.Dock = DockStyle.Fill;
            _seekPositionLabel.TextAlign = ContentAlignment.MiddleRight;
            _seekPositionLabel.Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point);
            layout.Controls.Add(_seekPositionLabel, 1, 0);

            FlowLayoutPanel panel = new FlowLayoutPanel();
            panel.Dock = DockStyle.Fill;
            panel.AutoSize = true;
            panel.WrapContents = false;

            _playButton = NewButton("Play", PlayClicked);
            _pauseButton = NewButton("Pause", PauseClicked);
            _stopButton = NewButton("Stop", StopClicked);
            _seekBack10Button = NewButton("−10 sec", delegate { SeekBy(-10000000); });
            _seekBack5Button = NewButton("−5 sec", delegate { SeekBy(-5000000); });
            _seekForward5Button = NewButton("+5 sec", delegate { SeekBy(5000000); });
            _seekForward10Button = NewButton("+10 sec", delegate { SeekBy(10000000); });
            Button reset = NewButton("Reset statistics", delegate { _engine.ResetStatistics(); RefreshStatistics(); });
            reset.Margin = new Padding(18, 3, 3, 3);
            _stateLabel = new Label();
            _stateLabel.AutoSize = true;
            _stateLabel.Margin = new Padding(18, 8, 3, 3);

            panel.Controls.Add(_playButton);
            panel.Controls.Add(_pauseButton);
            panel.Controls.Add(_stopButton);
            panel.Controls.Add(_seekBack10Button);
            panel.Controls.Add(_seekBack5Button);
            panel.Controls.Add(_seekForward5Button);
            panel.Controls.Add(_seekForward10Button);
            panel.Controls.Add(reset);
            panel.Controls.Add(_stateLabel);
            layout.Controls.Add(panel, 0, 1);
            layout.SetColumnSpan(panel, 2);
            group.Controls.Add(layout);
            return group;
        }

        private Control BuildStatisticsGroup()
        {
            GroupBox group = NewGroup("Statistics");
            TableLayoutPanel table = NewTable(4);
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));

            _rateValue = AddStatistic(table, 0, 0, "Theoretical maximum rate:");
            _queueValue = AddStatistic(table, 2, 0, "Current queue length:");
            _maxQueueValue = AddStatistic(table, 0, 1, "Maximum queue length:");
            _processedValue = AddStatistic(table, 2, 1, "Events processed:");
            _droppedValue = AddStatistic(table, 0, 2, "Events dropped:");
            _playbackValue = AddStatistic(table, 2, 2, "Playback clock:");
            _intendedValue = AddStatistic(table, 0, 3, "Intended timeline position:");
            _dispatchedValue = AddStatistic(table, 2, 3, "Last dispatched source position:");
            _lagValue = AddStatistic(table, 0, 4, "Current simulated lag:");
            _maxLagValue = AddStatistic(table, 2, 4, "Maximum simulated lag:");

            group.Controls.Add(table);
            return group;
        }

        private static Label AddStatistic(TableLayoutPanel table, int column, int row, string caption)
        {
            Label name = new Label();
            name.Text = caption;
            name.AutoSize = true;
            name.Anchor = AnchorStyles.Left;
            name.Margin = new Padding(3, 5, 8, 5);
            table.Controls.Add(name, column, row);
            Label value = new Label();
            value.Text = "0";
            value.AutoSize = false;
            value.MinimumSize = new Size(145, 24);
            value.Dock = DockStyle.Fill;
            value.TextAlign = ContentAlignment.MiddleRight;
            value.Font = new Font("Consolas", 9F, FontStyle.Bold, GraphicsUnit.Point);
            value.Tag = "StatisticValue";
            value.Margin = new Padding(3, 5, 18, 5);
            table.Controls.Add(value, column + 1, row);
            return value;
        }

        private static GroupBox NewGroup(string text)
        {
            GroupBox group = new GroupBox();
            group.Text = text;
            group.Dock = DockStyle.Top;
            group.AutoSize = true;
            group.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            group.Padding = new Padding(10);
            group.Margin = new Padding(3, 3, 3, 9);
            return group;
        }

        private static TableLayoutPanel NewTable(int columns)
        {
            TableLayoutPanel table = new TableLayoutPanel();
            table.Dock = DockStyle.Top;
            table.AutoSize = true;
            table.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            table.ColumnCount = columns;
            table.Padding = new Padding(2);
            return table;
        }

        private static Button NewButton(string text, EventHandler click)
        {
            Button button = new Button();
            button.Text = text;
            button.AutoSize = true;
            button.Click += click;
            return button;
        }

        private void LoadOutputDevices()
        {
            _outputCombo.Items.Clear();
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            foreach (MidiOutputDeviceInfo device in devices)
                _outputCombo.Items.Add(device);
            if (_outputCombo.Items.Count > 0)
                _outputCombo.SelectedIndex = 0;
            else
            {
                _outputCombo.Items.Add("No Windows MIDI output devices found");
                _outputCombo.SelectedIndex = 0;
                _outputCombo.Enabled = false;
            }
        }

        private void OpenMidiClicked(object sender, EventArgs e)
        {
            using (OpenFileDialog dialog = new OpenFileDialog())
            {
                dialog.Title = "Open Standard MIDI File";
                dialog.Filter = "MIDI files (*.mid;*.midi)|*.mid;*.midi|All files (*.*)|*.*";
                dialog.CheckFileExists = true;
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    _engine.Stop();
                    _engineSong = null;
                    _selectedPositionMicroseconds = 0;
                    Cursor = Cursors.WaitCursor;
                    _song = MidiFileParser.Load(dialog.FileName);
                    _fileLabel.Text = Path.GetFileName(_song.FilePath);
                    _fileLabel.ToolTipText(_song.FilePath);
                    _fileInfoLabel.Text = String.Format(CultureInfo.CurrentCulture, "{0:N0} events  •  {1} tracks  •  PPQN {2}  •  {3}", _song.Events.Count, _song.TrackCount, _song.TicksPerQuarterNote, FormatTime(_song.DurationMicroseconds));
                    UpdateSeekDisplay();
                }
                catch (Exception ex)
                {
                    _song = null;
                    MessageBox.Show(this, ex.Message, "Unable to load MIDI file", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
                finally
                {
                    Cursor = Cursors.Default;
                    UpdateSeekDisplay();
                    UpdateTransportControls();
                }
            }
        }

        private void PlayClicked(object sender, EventArgs e)
        {
            if (_engine.State == PlaybackState.Paused)
            {
                _engine.Resume();
                UpdateTransportControls();
                return;
            }
            if (_song == null)
            {
                MessageBox.Show(this, "Open a MIDI file first.", "No MIDI file", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            MidiOutputDeviceInfo device = _outputCombo.SelectedItem as MidiOutputDeviceInfo;
            if (device == null)
            {
                MessageBox.Show(this, "No Windows MIDI output device is available.", "No MIDI output", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                if (_selectedPositionMicroseconds >= _song.DurationMicroseconds)
                    _selectedPositionMicroseconds = 0;
                _output.Open(device.DeviceId);
                _engine.Start(_song, _output, _queueRadio.Checked ? ProcessingMode.Queue : ProcessingMode.Drop, _selectedPositionMicroseconds);
                _engineSong = _song;
                UpdateTransportControls();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Unable to start playback", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void PauseClicked(object sender, EventArgs e)
        {
            _engine.Pause();
            UpdateTransportControls();
        }

        private void StopClicked(object sender, EventArgs e)
        {
            _engine.Stop();
            _selectedPositionMicroseconds = 0;
            UpdateSeekDisplay();
            UpdateTransportControls();
            RefreshStatistics();
        }

        private void ProcessingValueChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            decimal multiplier = _processingUnit.SelectedIndex == 1 ? 1000m : 1m;
            long microseconds = Decimal.ToInt64(Decimal.Round(_processingValue.Value * multiplier, 0, MidpointRounding.AwayFromZero));
            ApplyProcessingMicroseconds(microseconds, true);
        }

        private void ProcessingUnitChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            SetProcessingMicroseconds(_engine.ProcessingMicroseconds);
        }

        private void ProcessingSliderChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            long microseconds = SliderToMicroseconds(_processingSlider.Value);
            ApplyProcessingMicroseconds(microseconds, false);
        }

        private void SetProcessingMicroseconds(long microseconds)
        {
            ApplyProcessingMicroseconds(microseconds, false);
        }

        private void ApplyProcessingMicroseconds(long microseconds, bool fromNumeric)
        {
            if (microseconds < 0) microseconds = 0;
            if (microseconds > 1000000) microseconds = 1000000;
            _engine.ProcessingMicroseconds = microseconds;
            _updatingProcessingControls = true;
            try
            {
                if (!fromNumeric)
                {
                    if (_processingUnit.SelectedIndex == 1)
                    {
                        _processingValue.DecimalPlaces = 3;
                        _processingValue.Increment = 0.001m;
                        _processingValue.Maximum = 1000m;
                        _processingValue.Value = microseconds / 1000m;
                    }
                    else
                    {
                        _processingValue.DecimalPlaces = 0;
                        _processingValue.Increment = 1m;
                        _processingValue.Maximum = 1000000m;
                        _processingValue.Value = microseconds;
                    }
                }
                int slider = MicrosecondsToSlider(microseconds);
                if (_processingSlider.Value != slider) _processingSlider.Value = slider;
            }
            finally { _updatingProcessingControls = false; }
            UpdateProcessingSummary(microseconds);
            RefreshStatistics();
        }

        private void UpdateProcessingSummary(long microseconds)
        {
            if (microseconds == 0)
                _processingSummary.Text = "0 µs/event — unlimited simulated service rate (host and MIDI output limits still apply). Slider is logarithmic from 1 µs to 1 s.";
            else
                _processingSummary.Text = FormatDuration(microseconds) + "/event — theoretical maximum " + (1000000.0 / microseconds).ToString("N1", CultureInfo.CurrentCulture) + " events/sec. Slider is logarithmic from 1 µs to 1 s.";
        }

        private static int MicrosecondsToSlider(long microseconds)
        {
            if (microseconds <= 0) return 0;
            double normalized = Math.Log10(microseconds) / 6.0;
            return Math.Max(1, Math.Min(1000, 1 + (int)Math.Round(normalized * 999.0)));
        }

        private static long SliderToMicroseconds(int slider)
        {
            if (slider <= 0) return 0;
            double exponent = ((slider - 1) / 999.0) * 6.0;
            return (long)Math.Round(Math.Pow(10.0, exponent));
        }

        private void RefreshStatistics()
        {
            PlaybackSnapshot snapshot = _engine.GetSnapshot();
            if (!_draggingSeek && snapshot.State != PlaybackState.Stopped)
            {
                _selectedPositionMicroseconds = snapshot.IntendedTimelineMicroseconds;
                UpdateSeekDisplay();
            }
            long processing = snapshot.ProcessingMicroseconds;
            _rateValue.Text = processing == 0 ? "Unlimited (simulated)" : (1000000.0 / processing).ToString("N1", CultureInfo.CurrentCulture) + " events/sec";
            _queueValue.Text = snapshot.QueueLength.ToString("N0", CultureInfo.CurrentCulture);
            _maxQueueValue.Text = snapshot.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture);
            _processedValue.Text = snapshot.ProcessedEvents.ToString("N0", CultureInfo.CurrentCulture);
            _droppedValue.Text = snapshot.DroppedEvents.ToString("N0", CultureInfo.CurrentCulture);
            _playbackValue.Text = FormatTime(snapshot.PlaybackMicroseconds);
            _intendedValue.Text = FormatTime(snapshot.IntendedTimelineMicroseconds);
            _dispatchedValue.Text = FormatTime(snapshot.LastDispatchedTimelineMicroseconds);
            _lagValue.Text = FormatDuration(snapshot.CurrentLagMicroseconds);
            _maxLagValue.Text = FormatDuration(snapshot.MaximumLagMicroseconds);
            _stateLabel.Text = snapshot.State.ToString();
            if (_engine.State == PlaybackState.Completed) UpdateTransportControls();
        }

        private void UpdateTransportControls()
        {
            PlaybackState state = _engine.State;
            bool active = state == PlaybackState.Playing || state == PlaybackState.Paused;
            _playButton.Enabled = _song != null && state != PlaybackState.Playing;
            _playButton.Text = state == PlaybackState.Paused ? "Resume" : "Play";
            _pauseButton.Enabled = state == PlaybackState.Playing;
            _stopButton.Enabled = active || state == PlaybackState.Completed;
            _outputCombo.Enabled = !active && _outputCombo.Items.Count > 0 && _outputCombo.SelectedItem is MidiOutputDeviceInfo;
            _queueRadio.Enabled = !active;
            _dropRadio.Enabled = !active;
            bool canSeek = _song != null;
            _seekBar.Enabled = canSeek;
            _seekBack10Button.Enabled = canSeek;
            _seekBack5Button.Enabled = canSeek;
            _seekForward5Button.Enabled = canSeek;
            _seekForward10Button.Enabled = canSeek;
            _stateLabel.Text = state.ToString();
        }

        private void SeekBarValueChanged(object sender, EventArgs e)
        {
            if (!_draggingSeek) return;
            long preview = SeekBarToMicroseconds(_seekBar.Value);
            _seekPositionLabel.Text = FormatTime(preview) + " / " + FormatTime(_song == null ? 0 : _song.DurationMicroseconds);
        }

        private void SeekBy(long deltaMicroseconds)
        {
            PerformSeek(_selectedPositionMicroseconds + deltaMicroseconds);
        }

        private void PerformSeek(long targetMicroseconds)
        {
            if (_song == null) return;
            targetMicroseconds = Math.Max(0, Math.Min(_song.DurationMicroseconds, targetMicroseconds));
            _selectedPositionMicroseconds = targetMicroseconds;
            if (_engineSong == _song)
                _engine.Seek(targetMicroseconds);
            UpdateSeekDisplay();
            RefreshStatistics();
            UpdateTransportControls();
        }

        private void UpdateSeekDisplay()
        {
            long duration = _song == null ? 0 : _song.DurationMicroseconds;
            if (!_draggingSeek)
            {
                int position = duration <= 0 ? 0 : (int)Math.Min(1000000, ((decimal)_selectedPositionMicroseconds * 1000000m) / duration);
                if (_seekBar.Value != position) _seekBar.Value = position;
            }
            _seekPositionLabel.Text = FormatTime(_selectedPositionMicroseconds) + " / " + FormatTime(duration);
        }

        private long SeekBarToMicroseconds(int value)
        {
            if (_song == null || _song.DurationMicroseconds <= 0) return 0;
            return (long)(((decimal)value * _song.DurationMicroseconds) / 1000000m);
        }

        private void EnginePlaybackEnded(object sender, EventArgs e)
        {
            if (IsHandleCreated) BeginInvoke((MethodInvoker)delegate { UpdateTransportControls(); RefreshStatistics(); });
        }

        private void EnginePlaybackFailed(object sender, PlaybackErrorEventArgs e)
        {
            if (IsHandleCreated)
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    UpdateTransportControls();
                    MessageBox.Show(this, e.Error.Message, "Playback error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                });
            }
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            _uiTimer.Stop();
            _engine.Dispose();
            _output.Dispose();
            base.OnFormClosing(e);
        }

        private static string FormatTime(long microseconds)
        {
            if (microseconds < 0) microseconds = 0;
            TimeSpan value = TimeSpan.FromTicks(microseconds * 10);
            int totalMinutes = (int)value.TotalMinutes;
            return String.Format(CultureInfo.CurrentCulture, "{0:00}:{1:00}.{2:000}", totalMinutes, value.Seconds, value.Milliseconds);
        }

        private static string FormatDuration(long microseconds)
        {
            if (microseconds < 1000) return microseconds.ToString("N0", CultureInfo.CurrentCulture) + " µs";
            return (microseconds / 1000.0).ToString("N3", CultureInfo.CurrentCulture) + " ms";
        }
    }

    internal static class LabelExtensions
    {
        public static void ToolTipText(this Label label, string text)
        {
            ToolTip tip = new ToolTip();
            tip.SetToolTip(label, text);
        }
    }
}
