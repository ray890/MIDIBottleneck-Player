using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Diagnostics;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class MainForm : Form
    {
        private readonly PlaybackEngine _engine = new PlaybackEngine();
        private readonly WindowsMidiOutput _output = new WindowsMidiOutput();
        private readonly KdmApiMidiOutput _kdmApiOutput = new KdmApiMidiOutput();
        private readonly List<DiagnosticsForm> _analysisWindows = new List<DiagnosticsForm>();
        private readonly ToolTip _toolTip = new ToolTip();
        private IMidiOutput _activeOutput;
        private MidiSong _song;
        private MidiSong _engineSong;
        private bool _updatingProcessingControls;
        private bool _switchingOutput;
        private long _selectedPositionMicroseconds;
        private readonly EffectivePlaybackSpeed _effectiveSpeed = new EffectivePlaybackSpeed();
        private readonly RollingOutputRate _outputRate = new RollingOutputRate();
        private ContextMenuStrip _speedWindowMenu;
        private long _lastDroppedEvents;
        private long _overflowVisibleUntilMicroseconds;
        private string _outputError;
        private bool _compactLayout;
        private bool _responsiveLayoutInitialized;

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
        private Button _resetStatsButton;
        private StatisticsView _statisticsView;
        private TableLayoutPanel _rootLayout;
        private Label _footerLabel;
        private Timer _uiTimer;
        private Timer _analysisRefreshTimer;
        private PlaybackState _lastStatisticsState = PlaybackState.Stopped;

        public MainForm()
        {
            Text = "MIDI Event Bottleneck Simulator";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(600, 660);
            ClientSize = new Size(790, 630);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

            _effectiveSpeed.WindowMicroseconds = UserPreferences.LoadEffectiveSpeedWindow();

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
            ClientSizeChanged += delegate { UpdateResponsiveLayout(); };
            UpdateResponsiveLayout();
        }

        private void BuildInterface()
        {
            _rootLayout = new TableLayoutPanel();
            _rootLayout.Dock = DockStyle.Fill;
            _rootLayout.Padding = new Padding(9);
            _rootLayout.ColumnCount = 1;
            _rootLayout.RowCount = 5;
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            Controls.Add(_rootLayout);

            _rootLayout.Controls.Add(BuildFileAndOutputGroup(), 0, 0);
            _rootLayout.Controls.Add(BuildProcessingGroup(), 0, 1);
            _rootLayout.Controls.Add(BuildPlaybackGroup(), 0, 2);
            _rootLayout.Controls.Add(BuildStatisticsGroup(), 0, 3);

            _footerLabel = new Label();
            _footerLabel.Dock = DockStyle.Fill;
            _footerLabel.Padding = new Padding(3, 3, 3, 0);
            _footerLabel.ForeColor = Color.DimGray;
            _footerLabel.Text = "Tempo-mapped MIDI events enter a single-server processor; live rate changes apply to the next event starting service.";
            _footerLabel.ToolTipText(_footerLabel.Text);
            _rootLayout.Controls.Add(_footerLabel, 0, 4);
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
            _outputCombo.SelectedIndexChanged += OutputSettingChanged;
            table.Controls.Add(_outputCombo, 1, 1);

            _kdmApiCheck = new CheckBox();
            _kdmApiCheck.Text = "KDMAPI";
            _kdmApiCheck.AutoSize = true;
            _kdmApiCheck.Anchor = AnchorStyles.Left;
            _kdmApiCheck.Margin = new Padding(10, 3, 3, 3);
            _kdmApiCheck.CheckedChanged += OutputSettingChanged;
            _toolTip.SetToolTip(_kdmApiCheck, "Send directly to OmniMIDI. Changing output during playback restarts at the current source position and clears backlog/statistics.");
            _toolTip.SetToolTip(_outputCombo, "Changing output during playback restarts at the current source position and clears backlog/statistics.");
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
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            _simulateSlowdownCheck = new CheckBox();
            _simulateSlowdownCheck.Text = "Simulate slowdown";
            _simulateSlowdownCheck.AutoSize = true;
            _simulateSlowdownCheck.Checked = true;
            _simulateSlowdownCheck.CheckedChanged += SimulateSlowdownChanged;
            _toolTip.SetToolTip(_simulateSlowdownCheck, "A live change applies to the next event that begins service.");
            table.Controls.Add(_simulateSlowdownCheck, 0, 1);
            table.SetColumnSpan(_simulateSlowdownCheck, 8);

            _queueLimitCheck = new CheckBox();
            _queueLimitCheck.Text = "Queue length limit:";
            _queueLimitCheck.AutoSize = true;
            _queueLimitCheck.CheckedChanged += QueueLimitChanged;
            _toolTip.SetToolTip(_queueLimitCheck, "Queue structure is locked while playback is active.");
            table.Controls.Add(_queueLimitCheck, 0, 0);

            _queueLimitValue = new NumericUpDown();
            _queueLimitValue.Minimum = 1;
            _queueLimitValue.Maximum = 1000000;
            _queueLimitValue.Value = PlaybackEngine.DefaultQueueLengthLimit;
            _queueLimitValue.Increment = 100;
            _queueLimitValue.ThousandsSeparator = true;
            _queueLimitValue.Width = 100;
            _toolTip.SetToolTip(_queueLimitValue, "Queue structure is locked while playback is active.");
            _queueLimitValue.ValueChanged += delegate
            {
                _engine.QueueLengthLimit = Decimal.ToInt32(_queueLimitValue.Value);
                ScheduleAnalysisRefresh();
            };
            table.Controls.Add(_queueLimitValue, 1, 0);

            Label eventsLabel = new Label();
            eventsLabel.Text = "events";
            eventsLabel.AutoSize = true;
            eventsLabel.Anchor = AnchorStyles.Left;
            table.Controls.Add(eventsLabel, 2, 0);

            Label overflowLabel = new Label();
            overflowLabel.Text = "Overflow:";
            overflowLabel.AutoSize = true;
            overflowLabel.Anchor = AnchorStyles.Right;
            overflowLabel.Margin = new Padding(18, 3, 3, 3);
            table.Controls.Add(overflowLabel, 3, 0);

            _overflowPolicyCombo = new ComboBox();
            _overflowPolicyCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _overflowPolicyCombo.Items.Add("Drop newest");
            _overflowPolicyCombo.Items.Add("Drop oldest");
            _overflowPolicyCombo.Items.Add("Clear buffer and jump to realtime");
            _overflowPolicyCombo.SelectedIndex = 0;
            _overflowPolicyCombo.Width = 245;
            _overflowPolicyCombo.SelectedIndexChanged += OverflowPolicyChanged;
            _toolTip.SetToolTip(_overflowPolicyCombo, "A change made during playback applies at the next overflow.");
            table.Controls.Add(_overflowPolicyCombo, 4, 0);
            table.SetColumnSpan(_overflowPolicyCombo, 4);

            Label serviceModeLabel = new Label();
            serviceModeLabel.Text = "Rate model:";
            serviceModeLabel.AutoSize = true;
            serviceModeLabel.Anchor = AnchorStyles.Left;

            _serviceModeCombo = new ComboBox();
            _serviceModeCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _serviceModeCombo.Items.Add("Processing time per event");
            _serviceModeCombo.Items.Add("MIDI serial bitrate");
            _serviceModeCombo.SelectedIndex = 0;
            _serviceModeCombo.Width = 210;
            _serviceModeCombo.SelectedIndexChanged += ServiceModeChanged;
            _toolTip.SetToolTip(_serviceModeCombo, "A change made during playback applies to the next event that begins service.");

            _serviceValueLabel = new Label();
            _serviceValueLabel.Text = "Processing time per event:";
            _serviceValueLabel.AutoSize = true;
            _serviceValueLabel.Anchor = AnchorStyles.Left;
            _serviceValueLabel.Margin = new Padding(12, 3, 3, 3);

            _dinPresetButton = new Button();
            _dinPresetButton.Text = "5-pin DIN";
            _dinPresetButton.AutoSize = true;
            _dinPresetButton.Visible = false;
            _dinPresetButton.Anchor = AnchorStyles.Left;
            _dinPresetButton.Click += delegate { ApplyMidiBitrate(ServiceDurationCalculator.FivePinDinBitrate, false); };
            _toolTip.SetToolTip(_dinPresetButton, "Set the standard MIDI DIN rate of 31,250 bit/s.");

            _processingValue = new NumericUpDown();
            _processingValue.Minimum = 0;
            _processingValue.Maximum = 1000000;
            _processingValue.Width = 100;
            _processingValue.ThousandsSeparator = true;
            _processingValue.ValueChanged += ProcessingValueChanged;
            _toolTip.SetToolTip(_processingValue, "A live edit applies to the next event that begins service.");

            _serviceUnitLabel = new Label();
            _serviceUnitLabel.Text = "µs";
            _serviceUnitLabel.AutoSize = true;
            _serviceUnitLabel.Anchor = AnchorStyles.Left;
            FlowLayoutPanel serviceRow = new FlowLayoutPanel();
            serviceRow.AutoSize = true;
            serviceRow.Dock = DockStyle.Fill;
            serviceRow.WrapContents = false;
            serviceRow.Margin = new Padding(0);
            serviceRow.Controls.Add(serviceModeLabel);
            serviceRow.Controls.Add(_serviceModeCombo);
            serviceRow.Controls.Add(_serviceValueLabel);
            serviceRow.Controls.Add(_processingValue);
            serviceRow.Controls.Add(_serviceUnitLabel);
            serviceRow.Controls.Add(_dinPresetButton);
            table.Controls.Add(serviceRow, 0, 2);
            table.SetColumnSpan(serviceRow, 8);

            _processingSlider = new ProcessingTrackBar();
            _processingSlider.Minimum = 0;
            _processingSlider.Maximum = ProcessingTrackBar.ScaleMaximum;
            _processingSlider.TickFrequency = 1000;
            _processingSlider.Dock = DockStyle.Fill;
            _processingSlider.AutoSize = true;
            _processingSlider.ValueChanged += ProcessingSliderChanged;
            _toolTip.SetToolTip(_processingSlider, "Click or drag to set the rate. A live edit applies to the next event that begins service.");
            table.Controls.Add(_processingSlider, 0, 3);
            table.SetColumnSpan(_processingSlider, 8);

            _processingSummary = new Label();
            _processingSummary.AutoSize = false;
            _processingSummary.AutoEllipsis = true;
            _processingSummary.Dock = DockStyle.Fill;
            _processingSummary.Height = 18;
            _processingSummary.Margin = new Padding(3, 0, 3, 0);
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
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));

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
            _resetStatsButton = NewButton("Reset stats", ResetStatsClicked);
            _resetStatsButton.Anchor = AnchorStyles.Right;
            _toolTip.SetToolTip(_resetStatsButton, "Reset counters and maximum baselines without seeking, clearing backlog, or resetting MIDI output.");

            panel.Controls.Add(_playButton);
            panel.Controls.Add(_stopButton);
            panel.Controls.Add(_seekBack5Button);
            panel.Controls.Add(_seekForward5Button);
            panel.Controls.Add(_analysisButton);
            layout.Controls.Add(panel, 0, 1);
            layout.Controls.Add(_resetStatsButton, 1, 1);
            group.Controls.Add(layout);
            return group;
        }

        private Control BuildStatisticsGroup()
        {
            GroupBox group = NewGroup("Statistics");
            _statisticsView = new StatisticsView();
            _statisticsView.Dock = DockStyle.Top;
            _statisticsView.SpeedMeasurementDescription = EffectivePlaybackSpeed.DescribeWindow(_effectiveSpeed.WindowMicroseconds);
            _statisticsView.EffectiveSpeedContextRequested += ShowEffectiveSpeedMenu;
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
            group.Padding = new Padding(8);
            group.Margin = new Padding(2, 2, 2, 6);
            return group;
        }

        private static TableLayoutPanel NewTable(int columns)
        {
            TableLayoutPanel table = new TableLayoutPanel();
            table.Dock = DockStyle.Top;
            table.AutoSize = true;
            table.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            table.ColumnCount = columns;
            table.Padding = new Padding(1);
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
                ResetLiveMeasurements();
                UpdateTransportControls();
                return;
            }
            if (_engine.State == PlaybackState.Paused)
            {
                _engine.Resume();
                ResetLiveMeasurements();
                UpdateTransportControls();
                return;
            }
            if (_song == null)
            {
                MessageBox.Show(this, "Open a MIDI file first.", "No MIDI file", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (!_kdmApiCheck.Checked && !(_outputCombo.SelectedItem is MidiOutputDeviceInfo))
            {
                MessageBox.Show(this, "No Windows MIDI output device is available.", "No MIDI output", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                if (_selectedPositionMicroseconds >= _song.DurationMicroseconds)
                    _selectedPositionMicroseconds = 0;
                IMidiOutput selectedOutput = OpenSelectedOutput();
                _engine.Start(_song, selectedOutput, _queueLimitCheck.Checked ? ProcessingMode.Drop : ProcessingMode.Queue, _selectedPositionMicroseconds);
                _activeOutput = selectedOutput;
                _engineSong = _song;
                _outputError = null;
                ResetLiveMeasurements();
                UpdateTransportControls();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Unable to start playback", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private IMidiOutput OpenSelectedOutput()
        {
            if (_kdmApiCheck.Checked)
            {
                if (_activeOutput != null && _activeOutput != _kdmApiOutput) _output.Dispose();
                _kdmApiOutput.Open();
                return _kdmApiOutput;
            }
            MidiOutputDeviceInfo device = _outputCombo.SelectedItem as MidiOutputDeviceInfo;
            if (device == null) throw new InvalidOperationException("No Windows MIDI output device is selected.");
            if (_activeOutput == _kdmApiOutput) _kdmApiOutput.Close();
            _output.Open(device.DeviceId);
            return _output;
        }

        private void OutputSettingChanged(object sender, EventArgs e)
        {
            UpdateTransportControls();
            if (_switchingOutput) return;
            PlaybackState previousState = _engine.State;
            if (previousState != PlaybackState.Playing && previousState != PlaybackState.Paused) return;

            PlaybackSnapshot before = _engine.GetSnapshot();
            long restartPosition = before.IntendedTimelineMicroseconds;
            _switchingOutput = true;
            try
            {
                _engine.Stop();
                if (_activeOutput == _kdmApiOutput) _kdmApiOutput.Close();
                else if (_activeOutput == _output) _output.Dispose();
                _activeOutput = null;
                _selectedPositionMicroseconds = restartPosition;
                ResetLiveMeasurements();

                IMidiOutput replacement = OpenSelectedOutput();
                _engine.Start(_song, replacement, _queueLimitCheck.Checked ? ProcessingMode.Drop : ProcessingMode.Queue,
                    restartPosition, previousState == PlaybackState.Paused);
                _activeOutput = replacement;
                _engineSong = _song;
                _outputError = null;
                ResetLiveMeasurements();
            }
            catch (Exception ex)
            {
                try { _output.Dispose(); } catch { }
                try { _kdmApiOutput.Dispose(); } catch { }
                _activeOutput = null;
                _outputError = ex.Message;
                MessageBox.Show(this, ex.Message, "Unable to change MIDI output", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                _switchingOutput = false;
                UpdateSeekDisplay();
                UpdateTransportControls();
                RefreshStatistics();
            }
        }

        private void StopClicked(object sender, EventArgs e)
        {
            _engine.Stop();
            _selectedPositionMicroseconds = 0;
            ResetLiveMeasurements();
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
                _processingSummary.Text = prefix + "0 µs/event — unlimited simulated service rate (host and MIDI output limits still apply).";
            else
                _processingSummary.Text = prefix + FormatDuration(microseconds) + "/event — theoretical maximum " + (1000000.0 / microseconds).ToString("N1", CultureInfo.CurrentCulture) + " events/sec. Fine 0–5,000 µs range, then logarithmic to 1 second.";
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
                    _serviceValueLabel.Text = _compactLayout ? "Bitrate:" : "MIDI bitrate:";
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
                    _serviceValueLabel.Text = _compactLayout ? "Time/event:" : "Processing time per event:";
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
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void OverflowPolicyChanged(object sender, EventArgs e)
        {
            _engine.OverflowPolicy = (OverflowPolicy)Math.Max(0, _overflowPolicyCombo.SelectedIndex);
            ScheduleAnalysisRefresh();
        }

        private void ResetStatsClicked(object sender, EventArgs e)
        {
            _engine.ResetStatistics();
            ResetLiveMeasurements();
            RefreshStatistics();
        }

        private void UpdatePolicyControlState()
        {
            bool active = _engine.State == PlaybackState.Playing || _engine.State == PlaybackState.Paused;
            bool slowdown = _simulateSlowdownCheck.Checked;
            bool limited = _queueLimitCheck.Checked;
            _queueLimitValue.Enabled = limited && !active;
            _overflowPolicyCombo.Enabled = limited;
            _serviceModeCombo.Enabled = slowdown;
            _processingValue.Enabled = slowdown;
            _processingSlider.Enabled = slowdown;
            _dinPresetButton.Enabled = slowdown && _engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate;
        }

        private static int BitrateToSlider(long bitrate)
        {
            double normalized = (Math.Log10(Math.Max(100, Math.Min(100000000, bitrate))) - 2.0) / 6.0;
            return Math.Max(0, Math.Min(ProcessingTrackBar.ScaleMaximum, (int)Math.Round(normalized * ProcessingTrackBar.ScaleMaximum)));
        }

        private static long SliderToBitrate(int slider)
        {
            double exponent = 2.0 + (Math.Max(0, Math.Min(ProcessingTrackBar.ScaleMaximum, slider)) / (double)ProcessingTrackBar.ScaleMaximum) * 6.0;
            return (long)Math.Round(Math.Pow(10.0, exponent));
        }

        internal static int MicrosecondsToSlider(long microseconds)
        {
            return ProcessingTrackBar.MicrosecondsToSlider(microseconds);
        }

        internal static long SliderToMicroseconds(int slider)
        {
            return ProcessingTrackBar.SliderToMicroseconds(slider);
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
                : (processing == 0 ? "Unlimited (simulated)" : (1000000.0 / processing).ToString(_compactLayout ? "N0" : "N1", CultureInfo.CurrentCulture) + (_compactLayout ? " events/s" : " events/sec"));
            long sampleTime = StopwatchTicksToMicroseconds(Stopwatch.GetTimestamp());
            bool synchronized = snapshot.OutstandingEvents == 0 && snapshot.CurrentLagMicroseconds == 0 &&
                snapshot.LastDispatchedTimelineMicroseconds <= snapshot.IntendedTimelineMicroseconds;
            double? speed = snapshot.State == PlaybackState.Playing
                ? _effectiveSpeed.Add(snapshot.PlaybackMicroseconds, snapshot.IntendedTimelineMicroseconds,
                    snapshot.LastDispatchedTimelineMicroseconds, snapshot.ProcessedEvents, synchronized, sampleTime)
                : (double?)null;
            double? outputRate = snapshot.State == PlaybackState.Playing
                ? _outputRate.Add(snapshot.ProcessedEvents, sampleTime) : (double?)null;
            if (snapshot.DroppedEvents > _lastDroppedEvents)
                _overflowVisibleUntilMicroseconds = sampleTime + 800000;
            _lastDroppedEvents = snapshot.DroppedEvents;
            _statisticsView.SetValues(new string[]
            {
                configuredRate,
                snapshot.QueueLength.ToString("N0", CultureInfo.CurrentCulture) + " / " + snapshot.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture),
                snapshot.ProcessedEvents.ToString("N0", CultureInfo.CurrentCulture) + " / " + snapshot.DroppedEvents.ToString("N0", CultureInfo.CurrentCulture),
                EffectivePlaybackSpeed.Format(speed),
                FormatLagMilliseconds(snapshot.CurrentLagMicroseconds),
                FormatLagMilliseconds(snapshot.MaximumLagMicroseconds),
                FormatTime(snapshot.IntendedTimelineMicroseconds) + " / " + FormatTime(snapshot.LastDispatchedTimelineMicroseconds),
                _compactLayout && outputRate.HasValue
                    ? outputRate.Value.ToString("N0", CultureInfo.CurrentCulture) + " events/s"
                    : RollingOutputRate.Format(outputRate)
            });
            _statisticsView.SetQueuePressure(_queueLimitCheck.Checked, snapshot.OutstandingEvents,
                Decimal.ToInt64(_queueLimitValue.Value), sampleTime < _overflowVisibleUntilMicroseconds);
            string stateText = snapshot.State.ToString();
            if (!String.Equals(_stateLabel.Text, stateText, StringComparison.Ordinal))
                _stateLabel.Text = stateText;
            if (snapshot.State != _lastStatisticsState)
            {
                _lastStatisticsState = snapshot.State;
                UpdateTransportControls();
            }
            DiagnosticsForm[] analysisWindows = _analysisWindows.ToArray();
            for (int i = 0; i < analysisWindows.Length; i++)
                if (analysisWindows[i] != null && !analysisWindows[i].IsDisposed)
                    analysisWindows[i].UpdatePlaybackSnapshot(snapshot);
        }

        private static long StopwatchTicksToMicroseconds(long ticks)
        {
            return (long)(((decimal)ticks * 1000000m) / Stopwatch.Frequency);
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
                diagnostics.SeekRequested += delegate(object source, WorkloadSelectionEventArgs seek)
                {
                    PerformSeek(seek.TimeMicroseconds);
                };
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
            ResetLiveMeasurements();
            UpdateSeekDisplay();
            RefreshStatistics();
            UpdateTransportControls();
        }

        private void UpdateSeekDisplay()
        {
            long duration = _song == null ? 0 : _song.DurationMicroseconds;
            _timelineView.SetTimeline(_selectedPositionMicroseconds, duration);
        }

        private void UpdateResponsiveLayout()
        {
            if (_rootLayout == null || _statisticsView == null) return;
            bool compact = ClientSize.Width < 650;
            if (_responsiveLayoutInitialized && compact == _compactLayout) return;
            _responsiveLayoutInitialized = true;
            _compactLayout = compact;
            _rootLayout.SuspendLayout();
            try
            {
                MinimumSize = compact ? new Size(560, 600) : new Size(600, 660);
                _rootLayout.Padding = compact ? new Padding(5) : new Padding(9);
                _footerLabel.Visible = !compact;
                _processingSummary.Visible = !compact;
                _fileInfoLabel.Visible = !compact;
                _statisticsView.Compact = compact;
                _serviceModeCombo.Width = compact ? 170 : 210;
                _overflowPolicyCombo.Width = compact ? 180 : 245;
                _processingValue.Width = compact ? 84 : 100;
                Control.ControlCollection children = _rootLayout.Controls;
                for (int i = 0; i < children.Count; i++)
                {
                    GroupBox group = children[i] as GroupBox;
                    if (group == null) continue;
                    group.Padding = compact ? new Padding(5) : new Padding(8);
                    group.Margin = compact ? new Padding(1, 1, 1, 4) : new Padding(2, 2, 2, 6);
                }
                ConfigureServiceControls();
            }
            finally
            {
                _rootLayout.ResumeLayout(true);
            }
        }

        private void ResetLiveMeasurements()
        {
            _effectiveSpeed.Reset();
            _outputRate.Reset();
            _lastDroppedEvents = 0;
            _overflowVisibleUntilMicroseconds = 0;
        }

        private void ShowEffectiveSpeedMenu(object sender, MouseEventArgs e)
        {
            if (_speedWindowMenu != null) _speedWindowMenu.Dispose();
            _speedWindowMenu = new ContextMenuStrip();
            AddSpeedWindowItem("Instantaneous", EffectivePlaybackSpeed.InstantaneousWindowMicroseconds);
            AddSpeedWindowItem("100 ms", 100000);
            AddSpeedWindowItem("250 ms", 250000);
            AddSpeedWindowItem("500 ms", 500000);
            AddSpeedWindowItem("1.5 s", 1500000);
            _speedWindowMenu.Items.Add(new ToolStripSeparator());
            ToolStripMenuItem custom = new ToolStripMenuItem("Custom...");
            custom.Click += delegate { PromptForCustomSpeedWindow(); };
            _speedWindowMenu.Items.Add(custom);
            _speedWindowMenu.Show(_statisticsView, e.Location);
        }

        private void AddSpeedWindowItem(string text, long microseconds)
        {
            ToolStripMenuItem item = new ToolStripMenuItem(text);
            item.Checked = _effectiveSpeed.WindowMicroseconds == microseconds;
            item.Click += delegate { SetEffectiveSpeedWindow(microseconds); };
            _speedWindowMenu.Items.Add(item);
        }

        private void SetEffectiveSpeedWindow(long microseconds)
        {
            _effectiveSpeed.WindowMicroseconds = microseconds;
            _statisticsView.SpeedMeasurementDescription = EffectivePlaybackSpeed.DescribeWindow(microseconds);
            UserPreferences.SaveEffectiveSpeedWindow(microseconds);
            RefreshStatistics();
        }

        private void PromptForCustomSpeedWindow()
        {
            using (Form dialog = new Form())
            using (NumericUpDown value = new NumericUpDown())
            using (Button ok = new Button())
            using (Button cancel = new Button())
            {
                dialog.Text = "Effective playback speed window";
                dialog.FormBorderStyle = FormBorderStyle.FixedDialog;
                dialog.StartPosition = FormStartPosition.CenterParent;
                dialog.ClientSize = new Size(330, 105);
                dialog.MinimizeBox = false;
                dialog.MaximizeBox = false;
                Label label = new Label { Text = "Measurement window:", AutoSize = true, Location = new Point(12, 18) };
                value.DecimalPlaces = 3;
                value.Minimum = 25;
                value.Maximum = 10000;
                value.Increment = 25;
                value.Value = Math.Max(value.Minimum, Math.Min(value.Maximum,
                    (_effectiveSpeed.WindowMicroseconds == 0 ? 250000 : _effectiveSpeed.WindowMicroseconds) / 1000m));
                value.Location = new Point(145, 15);
                value.Width = 105;
                Label unit = new Label { Text = "ms", AutoSize = true, Location = new Point(256, 18) };
                ok.Text = "OK"; ok.DialogResult = DialogResult.OK; ok.Location = new Point(164, 64);
                cancel.Text = "Cancel"; cancel.DialogResult = DialogResult.Cancel; cancel.Location = new Point(245, 64);
                dialog.Controls.Add(label); dialog.Controls.Add(value); dialog.Controls.Add(unit); dialog.Controls.Add(ok); dialog.Controls.Add(cancel);
                dialog.AcceptButton = ok; dialog.CancelButton = cancel;
                if (dialog.ShowDialog(this) == DialogResult.OK)
                    SetEffectiveSpeedWindow((long)Math.Round(value.Value * 1000m));
            }
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
            if (_speedWindowMenu != null) _speedWindowMenu.Dispose();
            _toolTip.Dispose();
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
