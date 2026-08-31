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
        private readonly KdmApiMidiOutput _kdmApiOutput = new KdmApiMidiOutput();
        private readonly List<DiagnosticsForm> _analysisWindows = new List<DiagnosticsForm>();
        private IMidiOutput _activeOutput;
        private MidiSong _song;
        private MidiSong _engineSong;
        private bool _updatingProcessingControls;
        private long _selectedPositionMicroseconds;

        private Label _fileLabel;
        private Label _fileInfoLabel;
        private ComboBox _outputCombo;
        private CheckBox _kdmApiCheck;
        private CheckBox _simulateSlowdownCheck;
        private CheckBox _queueLimitCheck;
        private ComboBox _serviceModeCombo;
        private ComboBox _overflowPolicyCombo;
        private Label _serviceValueLabel;
        private NumericUpDown _processingValue;
        private Label _serviceUnitLabel;
        private TrackBar _processingSlider;
        private Label _processingSummary;
        private Button _dinPresetButton;
        private NumericUpDown _queueLimitValue;
        private Button _playButton;
        private Button _stopButton;
        private Label _stateLabel;
        private PlaybackTimelineView _timelineView;
        private Button _seekBack5Button;
        private Button _seekForward5Button;
        private Button _analysisButton;
        private StatisticsView _statisticsView;
        private Timer _uiTimer;
        private Timer _analysisRefreshTimer;
        private PlaybackState _lastStatisticsState = PlaybackState.Stopped;

        public MainForm()
        {
            Text = "MIDI Event Bottleneck Simulator";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(740, 670);
            ClientSize = new Size(790, 650);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

            BuildInterface();
            LoadOutputDevices();
            SetProcessingMicroseconds(100);
            UpdateTransportControls();

            _engine.PlaybackEnded += EnginePlaybackEnded;
            _engine.PlaybackFailed += EnginePlaybackFailed;
            _uiTimer = new Timer();
            _uiTimer.Interval = 16;
            _uiTimer.Tick += delegate { RefreshStatistics(); };
            _uiTimer.Start();
            _analysisRefreshTimer = new Timer();
            _analysisRefreshTimer.Interval = 200;
            _analysisRefreshTimer.Tick += RefreshOpenAnalyses;
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
            TableLayoutPanel table = NewTable(4);
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
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

            _kdmApiCheck = new CheckBox();
            _kdmApiCheck.Text = "KDMAPI";
            _kdmApiCheck.AutoSize = true;
            _kdmApiCheck.Anchor = AnchorStyles.Left;
            _kdmApiCheck.Margin = new Padding(10, 3, 3, 3);
            _kdmApiCheck.CheckedChanged += delegate { UpdateTransportControls(); };
            ToolTip kdmApiTip = new ToolTip();
            kdmApiTip.SetToolTip(_kdmApiCheck, "Send directly to OmniMIDI. The selection applies the next time playback starts.");
            table.Controls.Add(_kdmApiCheck, 2, 1);

            _stateLabel = new Label();
            _stateLabel.AutoSize = true;
            _stateLabel.Anchor = AnchorStyles.Right;
            _stateLabel.Margin = new Padding(12, 3, 3, 3);
            table.Controls.Add(_stateLabel, 3, 1);

            group.Controls.Add(table);
            return group;
        }

        private Control BuildProcessingGroup()
        {
            GroupBox group = NewGroup("Processing model");
            TableLayoutPanel table = NewTable(8);
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            _simulateSlowdownCheck = new CheckBox();
            _simulateSlowdownCheck.Text = "Simulate slowdown";
            _simulateSlowdownCheck.AutoSize = true;
            _simulateSlowdownCheck.Checked = true;
            _simulateSlowdownCheck.CheckedChanged += SimulateSlowdownChanged;
            table.Controls.Add(_simulateSlowdownCheck, 0, 0);
            table.SetColumnSpan(_simulateSlowdownCheck, 8);

            _queueLimitCheck = new CheckBox();
            _queueLimitCheck.Text = "Queue length limit:";
            _queueLimitCheck.AutoSize = true;
            _queueLimitCheck.CheckedChanged += QueueLimitChanged;
            table.Controls.Add(_queueLimitCheck, 0, 1);

            _queueLimitValue = new NumericUpDown();
            _queueLimitValue.Minimum = 1;
            _queueLimitValue.Maximum = 1000000;
            _queueLimitValue.Value = PlaybackEngine.DefaultQueueLengthLimit;
            _queueLimitValue.Increment = 100;
            _queueLimitValue.ThousandsSeparator = true;
            _queueLimitValue.Width = 100;
            _queueLimitValue.ValueChanged += delegate
            {
                _engine.QueueLengthLimit = Decimal.ToInt32(_queueLimitValue.Value);
                ScheduleAnalysisRefresh();
            };
            table.Controls.Add(_queueLimitValue, 1, 1);

            Label eventsLabel = new Label();
            eventsLabel.Text = "events";
            eventsLabel.AutoSize = true;
            eventsLabel.Anchor = AnchorStyles.Left;
            table.Controls.Add(eventsLabel, 2, 1);

            Label overflowLabel = new Label();
            overflowLabel.Text = "Overflow:";
            overflowLabel.AutoSize = true;
            overflowLabel.Anchor = AnchorStyles.Right;
            overflowLabel.Margin = new Padding(18, 3, 3, 3);
            table.Controls.Add(overflowLabel, 3, 1);

            _overflowPolicyCombo = new ComboBox();
            _overflowPolicyCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _overflowPolicyCombo.Items.Add("Drop newest");
            _overflowPolicyCombo.Items.Add("Drop oldest");
            _overflowPolicyCombo.Items.Add("Clear buffer and jump to realtime");
            _overflowPolicyCombo.SelectedIndex = 0;
            _overflowPolicyCombo.Width = 245;
            _overflowPolicyCombo.SelectedIndexChanged += OverflowPolicyChanged;
            table.Controls.Add(_overflowPolicyCombo, 4, 1);
            table.SetColumnSpan(_overflowPolicyCombo, 4);

            Label serviceModeLabel = new Label();
            serviceModeLabel.Text = "Rate model:";
            serviceModeLabel.AutoSize = true;
            serviceModeLabel.Anchor = AnchorStyles.Left;
            table.Controls.Add(serviceModeLabel, 0, 2);

            _serviceModeCombo = new ComboBox();
            _serviceModeCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _serviceModeCombo.Items.Add("Processing time per event");
            _serviceModeCombo.Items.Add("MIDI bitrate / byte transmission");
            _serviceModeCombo.SelectedIndex = 0;
            _serviceModeCombo.Width = 210;
            _serviceModeCombo.SelectedIndexChanged += ServiceModeChanged;
            table.Controls.Add(_serviceModeCombo, 1, 2);
            table.SetColumnSpan(_serviceModeCombo, 2);

            _serviceValueLabel = new Label();
            _serviceValueLabel.Text = "Processing time per event:";
            _serviceValueLabel.AutoSize = true;
            _serviceValueLabel.Anchor = AnchorStyles.Left;
            _serviceValueLabel.Margin = new Padding(16, 3, 3, 3);
            table.Controls.Add(_serviceValueLabel, 3, 2);

            _dinPresetButton = new Button();
            _dinPresetButton.Text = "31,250 DIN";
            _dinPresetButton.AutoSize = true;
            _dinPresetButton.Visible = false;
            _dinPresetButton.Anchor = AnchorStyles.Left;
            _dinPresetButton.Click += delegate { ApplyMidiBitrate(ServiceDurationCalculator.FivePinDinBitrate, false); };
            table.Controls.Add(_dinPresetButton, 6, 2);

            _processingValue = new NumericUpDown();
            _processingValue.Minimum = 0;
            _processingValue.Maximum = 1000000;
            _processingValue.Width = 100;
            _processingValue.ThousandsSeparator = true;
            _processingValue.ValueChanged += ProcessingValueChanged;
            table.Controls.Add(_processingValue, 4, 2);

            _serviceUnitLabel = new Label();
            _serviceUnitLabel.Text = "µs";
            _serviceUnitLabel.AutoSize = true;
            _serviceUnitLabel.Anchor = AnchorStyles.Left;
            table.Controls.Add(_serviceUnitLabel, 5, 2);

            _processingSlider = new TrackBar();
            _processingSlider.Minimum = 0;
            _processingSlider.Maximum = 1000;
            _processingSlider.TickFrequency = 100;
            _processingSlider.Dock = DockStyle.Fill;
            _processingSlider.AutoSize = true;
            _processingSlider.ValueChanged += ProcessingSliderChanged;
            table.Controls.Add(_processingSlider, 0, 3);
            table.SetColumnSpan(_processingSlider, 8);

            _processingSummary = new Label();
            _processingSummary.AutoSize = true;
            _processingSummary.ForeColor = Color.DimGray;
            table.Controls.Add(_processingSummary, 0, 4);
            table.SetColumnSpan(_processingSummary, 8);

            group.Controls.Add(table);
            UpdatePolicyControlState();
            return group;
        }

        private Control BuildPlaybackGroup()
        {
            GroupBox group = NewGroup("Playback");
            TableLayoutPanel layout = NewTable(2);
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 150));

            _timelineView = new PlaybackTimelineView();
            _timelineView.Dock = DockStyle.Top;
            _timelineView.Enabled = false;
            _timelineView.SeekRequested += delegate(object sender, TimelineSeekEventArgs e) { PerformSeek(e.PositionMicroseconds); };
            layout.Controls.Add(_timelineView, 0, 0);
            layout.SetColumnSpan(_timelineView, 2);

            FlowLayoutPanel panel = new FlowLayoutPanel();
            panel.Dock = DockStyle.Fill;
            panel.AutoSize = true;
            panel.WrapContents = false;

            _playButton = NewButton("Play", PlayClicked);
            _stopButton = NewButton("Stop", StopClicked);
            _seekBack5Button = NewButton("−5 sec", delegate { SeekBy(-5000000); });
            _seekForward5Button = NewButton("+5 sec", delegate { SeekBy(5000000); });
            _analysisButton = NewButton("Analysis...", ShowAnalysisClicked);
            _analysisButton.Margin = new Padding(18, 3, 3, 3);

            panel.Controls.Add(_playButton);
            panel.Controls.Add(_stopButton);
            panel.Controls.Add(_seekBack5Button);
            panel.Controls.Add(_seekForward5Button);
            panel.Controls.Add(_analysisButton);
            layout.Controls.Add(panel, 0, 1);
            layout.SetColumnSpan(panel, 2);
            group.Controls.Add(layout);
            return group;
        }

        private Control BuildStatisticsGroup()
        {
            GroupBox group = NewGroup("Statistics");
            _statisticsView = new StatisticsView();
            _statisticsView.Dock = DockStyle.Top;
            group.Controls.Add(_statisticsView);
            return group;
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
            if (_engine.State == PlaybackState.Playing)
            {
                _engine.Pause();
                UpdateTransportControls();
                return;
            }
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
            if (!_kdmApiCheck.Checked && device == null)
            {
                MessageBox.Show(this, "No Windows MIDI output device is available.", "No MIDI output", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                if (_selectedPositionMicroseconds >= _song.DurationMicroseconds)
                    _selectedPositionMicroseconds = 0;
                IMidiOutput selectedOutput;
                if (_kdmApiCheck.Checked)
                {
                    if (_activeOutput != null && _activeOutput != _kdmApiOutput)
                        _output.Dispose();
                    _kdmApiOutput.Open();
                    selectedOutput = _kdmApiOutput;
                }
                else
                {
                    if (_activeOutput == _kdmApiOutput)
                        _kdmApiOutput.Dispose();
                    _output.Open(device.DeviceId);
                    selectedOutput = _output;
                }
                _engine.Start(_song, selectedOutput, _queueLimitCheck.Checked ? ProcessingMode.Drop : ProcessingMode.Queue, _selectedPositionMicroseconds);
                _activeOutput = selectedOutput;
                _engineSong = _song;
                UpdateTransportControls();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Unable to start playback", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
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
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
            {
                ApplyMidiBitrate(Decimal.ToInt64(_processingValue.Value), true);
                return;
            }
            long microseconds = Decimal.ToInt64(_processingValue.Value);
            ApplyProcessingMicroseconds(microseconds, true);
        }

        private void ProcessingSliderChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
            {
                ApplyMidiBitrate(SliderToBitrate(_processingSlider.Value), false);
                return;
            }
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
                    _processingValue.DecimalPlaces = 0;
                    _processingValue.Increment = 1m;
                    _processingValue.Maximum = 1000000m;
                    _processingValue.Value = microseconds;
                }
                int slider = MicrosecondsToSlider(microseconds);
                if (_processingSlider.Value != slider) _processingSlider.Value = slider;
            }
            finally { _updatingProcessingControls = false; }
            UpdateProcessingSummary(microseconds);
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void UpdateProcessingSummary(long microseconds)
        {
            string prefix = _engine.SimulateSlowdown ? String.Empty : "Slowdown disabled — configured value retained. ";
            if (microseconds == 0)
                _processingSummary.Text = prefix + "0 µs/event — unlimited simulated service rate (host and MIDI output limits still apply). Slider is logarithmic from 1 µs to 1 s.";
            else
                _processingSummary.Text = prefix + FormatDuration(microseconds) + "/event — theoretical maximum " + (1000000.0 / microseconds).ToString("N1", CultureInfo.CurrentCulture) + " events/sec. Slider is logarithmic from 1 µs to 1 s.";
        }

        private void ServiceModeChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            ServiceDurationMode mode = _serviceModeCombo.SelectedIndex == 1 ? ServiceDurationMode.MidiBitrate : ServiceDurationMode.ProcessingTime;
            _engine.ServiceDurationMode = mode;
            ConfigureServiceControls();
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void ConfigureServiceControls()
        {
            _updatingProcessingControls = true;
            try
            {
                if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                {
                    _serviceValueLabel.Text = "MIDI bitrate:";
                    _processingValue.DecimalPlaces = 0;
                    _processingValue.Minimum = 1;
                    _processingValue.Maximum = 100000000;
                    _processingValue.Increment = 100;
                    _processingValue.Value = Math.Min(_processingValue.Maximum, _engine.MidiBitrate);
                    _serviceUnitLabel.Text = "bit/s";
                    _dinPresetButton.Visible = true;
                    _processingSlider.Value = BitrateToSlider(_engine.MidiBitrate);
                }
                else
                {
                    _serviceValueLabel.Text = "Processing time per event:";
                    _processingValue.Minimum = 0;
                    _serviceUnitLabel.Text = "µs";
                    _dinPresetButton.Visible = false;
                }
            }
            finally { _updatingProcessingControls = false; }
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                UpdateBitrateSummary(_engine.MidiBitrate);
            else
                SetProcessingMicroseconds(_engine.ProcessingMicroseconds);
            UpdatePolicyControlState();
        }

        private void ApplyMidiBitrate(long bitrate, bool fromNumeric)
        {
            if (bitrate < 1) bitrate = 1;
            if (bitrate > 100000000) bitrate = 100000000;
            _engine.MidiBitrate = bitrate;
            _updatingProcessingControls = true;
            try
            {
                if (!fromNumeric) _processingValue.Value = bitrate;
                int slider = BitrateToSlider(bitrate);
                if (_processingSlider.Value != slider) _processingSlider.Value = slider;
            }
            finally { _updatingProcessingControls = false; }
            UpdateBitrateSummary(bitrate);
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void UpdateBitrateSummary(long bitrate)
        {
            long twoBytes = ServiceDurationCalculator.CalculateBitrateMicroseconds(2, bitrate);
            long threeBytes = ServiceDurationCalculator.CalculateBitrateMicroseconds(3, bitrate);
            string prefix = _engine.SimulateSlowdown ? String.Empty : "Slowdown disabled — configured value retained. ";
            _processingSummary.Text = prefix + bitrate.ToString("N0", CultureInfo.CurrentCulture) + " bit/s — 2-byte message " + FormatDuration(twoBytes) + ", 3-byte message " + FormatDuration(threeBytes) + ". Ten serial bits are charged per MIDI byte.";
        }

        private void SimulateSlowdownChanged(object sender, EventArgs e)
        {
            _engine.SimulateSlowdown = _simulateSlowdownCheck.Checked;
            UpdatePolicyControlState();
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                UpdateBitrateSummary(_engine.MidiBitrate);
            else
                UpdateProcessingSummary(_engine.ProcessingMicroseconds);
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void QueueLimitChanged(object sender, EventArgs e)
        {
            UpdatePolicyControlState();
            UpdateTransportControls();
            ScheduleAnalysisRefresh();
        }

        private void OverflowPolicyChanged(object sender, EventArgs e)
        {
            _engine.OverflowPolicy = (OverflowPolicy)Math.Max(0, _overflowPolicyCombo.SelectedIndex);
            ScheduleAnalysisRefresh();
        }

        private void UpdatePolicyControlState()
        {
            bool active = _engine.State == PlaybackState.Playing || _engine.State == PlaybackState.Paused;
            bool slowdown = _simulateSlowdownCheck.Checked;
            bool limited = _queueLimitCheck.Checked;
            _queueLimitValue.Enabled = limited && !active;
            _overflowPolicyCombo.Enabled = limited && !active;
            _serviceModeCombo.Enabled = slowdown && !active;
            _processingValue.Enabled = slowdown;
            _processingSlider.Enabled = slowdown;
            _dinPresetButton.Enabled = slowdown && _engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate;
        }

        private static int BitrateToSlider(long bitrate)
        {
            double normalized = (Math.Log10(Math.Max(100, Math.Min(100000000, bitrate))) - 2.0) / 6.0;
            return Math.Max(0, Math.Min(1000, (int)Math.Round(normalized * 1000.0)));
        }

        private static long SliderToBitrate(int slider)
        {
            double exponent = 2.0 + (Math.Max(0, Math.Min(1000, slider)) / 1000.0) * 6.0;
            return (long)Math.Round(Math.Pow(10.0, exponent));
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
            if (!_timelineView.IsDragging && snapshot.State != PlaybackState.Stopped)
            {
                _selectedPositionMicroseconds = snapshot.IntendedTimelineMicroseconds;
                UpdateSeekDisplay();
            }
            long processing = snapshot.ProcessingMicroseconds;
            string configuredRate = !snapshot.SimulateSlowdown
                ? "Immediate (simulated)"
                : snapshot.ServiceDurationMode == ServiceDurationMode.MidiBitrate
                ? snapshot.MidiBitrate.ToString("N0", CultureInfo.CurrentCulture) + " bit/s"
                : (processing == 0 ? "Unlimited (simulated)" : (1000000.0 / processing).ToString("N1", CultureInfo.CurrentCulture) + " events/sec");
            _statisticsView.SetValues(new string[]
            {
                configuredRate,
                snapshot.QueueLength.ToString("N0", CultureInfo.CurrentCulture),
                snapshot.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture),
                snapshot.ProcessedEvents.ToString("N0", CultureInfo.CurrentCulture),
                snapshot.DroppedEvents.ToString("N0", CultureInfo.CurrentCulture),
                FormatTime(snapshot.PlaybackMicroseconds),
                FormatTime(snapshot.IntendedTimelineMicroseconds),
                FormatTime(snapshot.LastDispatchedTimelineMicroseconds),
                FormatLagMilliseconds(snapshot.CurrentLagMicroseconds),
                FormatLagMilliseconds(snapshot.MaximumLagMicroseconds)
            });
            string stateText = snapshot.State.ToString();
            if (!String.Equals(_stateLabel.Text, stateText, StringComparison.Ordinal))
                _stateLabel.Text = stateText;
            if (snapshot.State != _lastStatisticsState)
            {
                _lastStatisticsState = snapshot.State;
                UpdateTransportControls();
            }
        }

        private void UpdateTransportControls()
        {
            PlaybackState state = _engine.State;
            bool active = state == PlaybackState.Playing || state == PlaybackState.Paused;
            _playButton.Enabled = _song != null;
            _playButton.Text = state == PlaybackState.Playing ? "Pause" : state == PlaybackState.Paused ? "Resume" : "Play";
            _stopButton.Enabled = active || state == PlaybackState.Completed;
            _outputCombo.Enabled = !_kdmApiCheck.Checked && _outputCombo.Items.Count > 0 && _outputCombo.SelectedItem is MidiOutputDeviceInfo;
            _queueLimitCheck.Enabled = !active;
            UpdatePolicyControlState();
            bool canSeek = _song != null;
            _timelineView.Enabled = canSeek;
            _seekBack5Button.Enabled = canSeek;
            _seekForward5Button.Enabled = canSeek;
            _analysisButton.Enabled = canSeek;
            _stateLabel.Text = state.ToString();
        }

        private void SeekBy(long deltaMicroseconds)
        {
            PerformSeek(_selectedPositionMicroseconds + deltaMicroseconds);
        }

        private void ShowAnalysisClicked(object sender, EventArgs e)
        {
            if (_song == null) return;
            try
            {
                Cursor = Cursors.WaitCursor;
                AnalysisConfiguration configuration = CurrentAnalysisConfiguration();
                WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(_song, configuration);
                DiagnosticsForm diagnostics = new DiagnosticsForm(_song, analysis);
                _analysisWindows.Add(diagnostics);
                diagnostics.FormClosed += delegate { _analysisWindows.Remove(diagnostics); };
                diagnostics.Show(this);
            }
            finally
            {
                Cursor = Cursors.Default;
            }
        }

        private void ScheduleAnalysisRefresh()
        {
            if (_analysisWindows.Count == 0 || _analysisRefreshTimer == null) return;
            _analysisRefreshTimer.Stop();
            _analysisRefreshTimer.Start();
        }

        private void RefreshOpenAnalyses(object sender, EventArgs e)
        {
            _analysisRefreshTimer.Stop();
            if (_analysisWindows.Count == 0) return;
            AnalysisConfiguration configuration = CurrentAnalysisConfiguration();
            Dictionary<MidiSong, WorkloadAnalysis> refreshed = new Dictionary<MidiSong, WorkloadAnalysis>();
            DiagnosticsForm[] windows = _analysisWindows.ToArray();
            for (int i = 0; i < windows.Length; i++)
            {
                DiagnosticsForm window = windows[i];
                if (window == null || window.IsDisposed) continue;
                WorkloadAnalysis analysis;
                if (!refreshed.TryGetValue(window.SourceSong, out analysis))
                {
                    analysis = WorkloadAnalyzer.Analyze(window.SourceSong, configuration);
                    refreshed.Add(window.SourceSong, analysis);
                }
                window.UpdateAnalysis(analysis);
            }
        }

        private AnalysisConfiguration CurrentAnalysisConfiguration()
        {
            AnalysisConfiguration configuration = new AnalysisConfiguration();
            configuration.SimulateSlowdown = _simulateSlowdownCheck.Checked;
            configuration.ServiceDurationMode = _engine.ServiceDurationMode;
            configuration.ProcessingMicroseconds = _engine.ProcessingMicroseconds;
            configuration.MidiBitrate = _engine.MidiBitrate;
            configuration.QueueLengthLimitEnabled = _queueLimitCheck.Checked;
            configuration.QueueLengthLimit = Decimal.ToInt32(_queueLimitValue.Value);
            configuration.OverflowPolicy = (OverflowPolicy)Math.Max(0, _overflowPolicyCombo.SelectedIndex);
            return configuration;
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
            _timelineView.SetTimeline(_selectedPositionMicroseconds, duration);
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
            _analysisRefreshTimer.Stop();
            _engine.Dispose();
            _output.Dispose();
            _kdmApiOutput.Dispose();
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

        internal static string FormatLagMilliseconds(long microseconds)
        {
            if (microseconds < 0) microseconds = 0;
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
