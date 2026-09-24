using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class MainForm : Form
    {
        private const int WmSysCommand = 0x0112;
        private const int SystemMenuAbout = 0x1F20;
        private const int SystemMenuAlwaysOnTop = 0x1F30;
        private const int SystemMenuPerNoteIntervalGate = 0x1F40;
        private const int SystemMenuApplyQueueLimitWithoutSlowdown = 0x1F50;
        private const int SystemMenuChaseMidiState = 0x1F60;
        private const uint MfString = 0x0000;
        private const uint MfSeparator = 0x0800;
        private const uint MfChecked = 0x0008;
        private const uint MfUnchecked = 0x0000;
        private const uint MfEnabled = 0x0000;
        private const uint MfGrayed = 0x0001;

        [DllImport("user32.dll")]
        private static extern IntPtr GetSystemMenu(IntPtr window, bool revert);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool AppendMenu(IntPtr menu, uint flags, UIntPtr identifier, string text);

        [DllImport("user32.dll")]
        private static extern uint CheckMenuItem(IntPtr menu, uint identifier, uint flags);

        [DllImport("user32.dll")]
        private static extern uint EnableMenuItem(IntPtr menu, uint identifier, uint flags);

        [DllImport("user32.dll")]
        private static extern bool DrawMenuBar(IntPtr window);
        private readonly PlaybackEngine _engine = new PlaybackEngine();
        private readonly WindowsMidiOutput _output = new WindowsMidiOutput();
        private readonly KdmApiMidiOutput _kdmApiOutput = new KdmApiMidiOutput();
        private readonly NullMidiOutput _nullOutput = new NullMidiOutput();
        private readonly HashSet<Control> _midiDropTargets = new HashSet<Control>();
        private readonly List<DiagnosticsForm> _analysisWindows = new List<DiagnosticsForm>();
        private ChannelMonitorForm _channelMonitor;
        private MidiSong _sourceReadoutSong;
        private int _sourceReadoutEventIndex = -1;
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
        private bool _loadingSong;
        private CancellationTokenSource _loadCancellation;
        private int _loadGeneration;
        private volatile MidiLoadProgress _loadProgress;
        private string _lastLoadStatus;
        private string _lastLoadTooltipStage;
        private string _loadingFileName;
        private long _loadStartedTimestamp;
        private long _loadDecisionPauseStartedTimestamp;
        private long _loadDecisionPausedTicks;
        private long _lastLoadTelemetrySecond = -1;
        private string _lastLoadTelemetryText;
        private int _loadingTelemetryUpdateCount;
        private int _lastDefaultHeight = 565;
        private int _lastCompactHeight;
        private int _lastDefaultRequiredHeight;
        private bool _suppressLoadErrorDialogs;
        private Exception _lastLoadError;
        private Func<MidiLargeFileInspection, bool> _largeFileWarningHandlerForTests;
        private bool _forceLargeFilePreflightForTests;
        private bool _forceLargeFileWarningForTests;
        private MidiLargeFileInspection _lastLargeFileInspection;
        private bool _perNoteIntervalGateEnabled;
        private bool _applyQueueLimitWithoutSlowdown;
        private bool _chaseMidiStateOnPlaySeek = true;
        private ServiceDurationMode _serviceModeBeforePerNoteGate = ServiceDurationMode.ProcessingTime;

        private Label _fileLabel;
        private Label _fileInfoLabel;
        private Button _openButton;
        private ProgressBar _loadActivity;
        private ProgressBar _loadStageActivity;
        private Label _loadingStatusLabel;
        private TableLayoutPanel _loadingPanel;
        private TableLayoutPanel _fileHeaderTable;
        private TableLayoutPanel _fileOutputTable;
        private GroupBox _fileOutputGroup;
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
        private Label _serviceModeLabel;
        private Label _overflowLabel;
        private Label _eventsLabel;
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
        private GroupBox _statisticsGroup;
        private GroupBox _playbackGroup;
        private TableLayoutPanel _rootLayout;
        private TableLayoutPanel _processingTable;
        private FlowLayoutPanel _queueCluster;
        private FlowLayoutPanel _overflowCluster;
        private FlowLayoutPanel _rateCluster;
        private FlowLayoutPanel _serviceCluster;
        private TableLayoutPanel _playbackButtonLayout;
        private System.Windows.Forms.Timer _uiTimer;
        private System.Windows.Forms.Timer _analysisRefreshTimer;
        private PlaybackState _lastStatisticsState = PlaybackState.Stopped;

        public MainForm()
        {
            ProductIcon.Apply(this);
            Text = ProductIdentity.Name;
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(560, 565);
            ClientSize = new Size(790, 526);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

            _effectiveSpeed.WindowMicroseconds = UserPreferences.LoadEffectiveSpeedWindow();
            _engine.SimulateSlowdown = false;
            _engine.ApplyQueueLimitWithoutSlowdown = false;
            _engine.ChaseMidiStateOnPlaySeek = true;

            BuildInterface();
            ConfigureMidiDrop(this);
            LoadOutputDevices();
            SetProcessingMicroseconds(100);
            UpdateTransportControls();

            _engine.PlaybackEnded += EnginePlaybackEnded;
            _engine.PlaybackFailed += EnginePlaybackFailed;
            _engine.ChannelControlFailed += EngineChannelControlFailed;
            _uiTimer = new System.Windows.Forms.Timer();
            _uiTimer.Interval = 16;
            _uiTimer.Tick += delegate { RefreshStatistics(); };
            _uiTimer.Start();
            _analysisRefreshTimer = new System.Windows.Forms.Timer();
            _analysisRefreshTimer.Interval = 200;
            _analysisRefreshTimer.Tick += RefreshOpenAnalyses;
            ClientSizeChanged += delegate { UpdateResponsiveLayout(); UpdateCompactUnitVisibility(); };
            UpdateResponsiveLayout();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            IntPtr menu = GetSystemMenu(Handle, false);
            if (menu != IntPtr.Zero)
            {
                AppendMenu(menu, MfSeparator, UIntPtr.Zero, null);
                AppendMenu(menu, MfString, (UIntPtr)SystemMenuAbout, "About " + ProductIdentity.Name + "…");
                AppendMenu(menu, MfSeparator, UIntPtr.Zero, null);
                AppendMenu(menu, MfString, (UIntPtr)SystemMenuChaseMidiState,
                    "Chase MIDI state on Play/Seek");
                AppendMenu(menu, MfString, (UIntPtr)SystemMenuAlwaysOnTop, "Always on top");
                AppendMenu(menu, MfString, (UIntPtr)SystemMenuPerNoteIntervalGate, "Per-note interval gate");
                AppendMenu(menu, MfString, (UIntPtr)SystemMenuApplyQueueLimitWithoutSlowdown,
                    "Apply queue limit without slowdown");
                UpdateAlwaysOnTopMenuCheck();
                UpdatePerNoteIntervalGateMenuCheck();
                UpdateForwardQueueMenuState();
                UpdateStateChaseMenuState();
            }
        }

        protected override void WndProc(ref Message message)
        {
            if (message.Msg == WmSysCommand)
            {
                int command = message.WParam.ToInt32() & 0xFFF0;
                if (command == SystemMenuAbout) { ShowAboutDialog(); return; }
                if (command == SystemMenuAlwaysOnTop) { ToggleAlwaysOnTop(); return; }
                if (command == SystemMenuPerNoteIntervalGate) { TogglePerNoteIntervalGate(); return; }
                if (command == SystemMenuApplyQueueLimitWithoutSlowdown) { ToggleForwardQueueLimit(); return; }
                if (command == SystemMenuChaseMidiState) { ToggleStateChase(); return; }
            }
            base.WndProc(ref message);
        }

        private void ShowAboutDialog()
        {
            using (AboutProductDialog dialog = new AboutProductDialog()) dialog.ShowDialog(this);
        }

        private void ToggleAlwaysOnTop()
        {
            TopMost = !TopMost;
            UpdateAlwaysOnTopMenuCheck();
        }

        private void UpdateAlwaysOnTopMenuCheck()
        {
            if (!IsHandleCreated) return;
            IntPtr menu = GetSystemMenu(Handle, false);
            if (menu == IntPtr.Zero) return;
            CheckMenuItem(menu, (uint)SystemMenuAlwaysOnTop, TopMost ? MfChecked : MfUnchecked);
            DrawMenuBar(Handle);
        }

        private void UpdatePerNoteIntervalGateMenuCheck()
        {
            if (!IsHandleCreated) return;
            IntPtr menu = GetSystemMenu(Handle, false);
            if (menu == IntPtr.Zero) return;
            CheckMenuItem(menu, (uint)SystemMenuPerNoteIntervalGate,
                _perNoteIntervalGateEnabled ? MfChecked : MfUnchecked);
            DrawMenuBar(Handle);
        }

        private void ToggleForwardQueueLimit()
        {
            PlaybackState state = _engine.State;
            if (state == PlaybackState.Playing || state == PlaybackState.Paused) return;
            _applyQueueLimitWithoutSlowdown = !_applyQueueLimitWithoutSlowdown;
            _engine.ApplyQueueLimitWithoutSlowdown = _applyQueueLimitWithoutSlowdown;
            UpdateForwardQueueMenuState();
            UpdatePolicyControlState();
            UpdateTransportControls();
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void UpdateForwardQueueMenuState()
        {
            if (!IsHandleCreated) return;
            IntPtr menu = GetSystemMenu(Handle, false);
            if (menu == IntPtr.Zero) return;
            CheckMenuItem(menu, (uint)SystemMenuApplyQueueLimitWithoutSlowdown,
                _applyQueueLimitWithoutSlowdown ? MfChecked : MfUnchecked);
            PlaybackState state = _engine.State;
            bool active = state == PlaybackState.Playing || state == PlaybackState.Paused;
            EnableMenuItem(menu, (uint)SystemMenuApplyQueueLimitWithoutSlowdown,
                active ? MfGrayed : MfEnabled);
            DrawMenuBar(Handle);
        }

        private void ToggleStateChase()
        {
            PlaybackState state = _engine.State;
            if (state == PlaybackState.Playing || state == PlaybackState.Paused) return;
            _chaseMidiStateOnPlaySeek = !_chaseMidiStateOnPlaySeek;
            _engine.ChaseMidiStateOnPlaySeek = _chaseMidiStateOnPlaySeek;
            _sourceReadoutEventIndex = -1;
            RefreshStatistics();
            UpdateStateChaseMenuState();
        }

        private void UpdateStateChaseMenuState()
        {
            if (!IsHandleCreated) return;
            IntPtr menu = GetSystemMenu(Handle, false);
            if (menu == IntPtr.Zero) return;
            CheckMenuItem(menu, (uint)SystemMenuChaseMidiState,
                _chaseMidiStateOnPlaySeek ? MfChecked : MfUnchecked);
            PlaybackState state = _engine.State;
            bool active = state == PlaybackState.Playing || state == PlaybackState.Paused;
            EnableMenuItem(menu, (uint)SystemMenuChaseMidiState, active ? MfGrayed : MfEnabled);
            DrawMenuBar(Handle);
        }

        private void TogglePerNoteIntervalGate()
        {
            if (!_perNoteIntervalGateEnabled && _engine.ProcessingMicroseconds <= 0)
            {
                MessageBox.Show(this,
                    "Set Processing time per event to a value greater than zero before enabling the per-note interval gate.",
                    "Per-note interval gate", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            PlaybackState previousState = _engine.State;
            bool wasActive = previousState == PlaybackState.Playing || previousState == PlaybackState.Paused;
            if (!_perNoteIntervalGateEnabled)
            {
                _serviceModeBeforePerNoteGate = _engine.ServiceDurationMode;
                _perNoteIntervalGateEnabled = true;
                _engine.ServiceDurationMode = ServiceDurationMode.ProcessingTime;
                _updatingProcessingControls = true;
                try { _serviceModeCombo.SelectedIndex = 0; }
                finally { _updatingProcessingControls = false; }
                ConfigureServiceControls();
            }
            else
            {
                _perNoteIntervalGateEnabled = false;
                _engine.ServiceDurationMode = _serviceModeBeforePerNoteGate;
                _updatingProcessingControls = true;
                try { _serviceModeCombo.SelectedIndex = (int)_engine.ServiceDurationMode; }
                finally { _updatingProcessingControls = false; }
                ConfigureServiceControls();
            }

            if (wasActive) TryRestartForProcessingModeChange(previousState);
            UpdatePerNoteIntervalGateMenuCheck();
            UpdateTransportControls();
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void RestartForProcessingModeChange(PlaybackState previousState)
        {
            if (_song == null || _activeOutput == null) return;
            PlaybackSnapshot before = _engine.GetSnapshot();
            long restartPosition = before.IntendedTimelineMicroseconds;
            _engine.Stop();
            _selectedPositionMicroseconds = restartPosition;
            _engine.Start(_song, _activeOutput, SelectedProcessingMode(), restartPosition,
                previousState == PlaybackState.Paused);
            _engineSong = _song;
            ResetLiveMeasurements();
        }

        private bool TryRestartForProcessingModeChange(PlaybackState previousState)
        {
            try
            {
                RestartForProcessingModeChange(previousState);
                return true;
            }
            catch (Exception ex)
            {
                _outputError = ex.Message;
                UpdateTransportControls();
                RefreshStatistics();
                MessageBox.Show(this, ex.Message, "Unable to change processing mode",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        private void ConfigureMidiDrop(Control control)
        {
            if (control == null || !_midiDropTargets.Add(control)) return;
            control.AllowDrop = true;
            control.DragEnter += MidiFileDragEnter;
            control.DragDrop += MidiFileDragDrop;
            control.ControlAdded += MidiDropControlAdded;
            for (int index = 0; index < control.Controls.Count; index++)
                ConfigureMidiDrop(control.Controls[index]);
        }

        private void MidiDropControlAdded(object sender, ControlEventArgs e)
        {
            ConfigureMidiDrop(e.Control);
        }

        private void MidiFileDragEnter(object sender, DragEventArgs e)
        {
            string path;
            e.Effect = TryGetDroppedMidiPath(e.Data, out path) ? DragDropEffects.Copy : DragDropEffects.None;
        }

        private void MidiFileDragDrop(object sender, DragEventArgs e)
        {
            string path;
            if (!TryGetDroppedMidiPath(e.Data, out path)) return;
            BeginMidiLoad(path);
        }

        internal static bool TryGetDroppedMidiPath(IDataObject data, out string path)
        {
            path = null;
            if (data == null || !data.GetDataPresent(DataFormats.FileDrop, false)) return false;
            try
            {
                string[] files = data.GetData(DataFormats.FileDrop, false) as string[];
                if (files == null || files.Length != 1 || String.IsNullOrWhiteSpace(files[0]) || !File.Exists(files[0])) return false;
                string extension = Path.GetExtension(files[0]);
                if (!String.Equals(extension, ".mid", StringComparison.OrdinalIgnoreCase) &&
                    !String.Equals(extension, ".midi", StringComparison.OrdinalIgnoreCase)) return false;
                path = Path.GetFullPath(files[0]);
                return true;
            }
            catch (Exception exception)
            {
                if (exception is OutOfMemoryException || exception is StackOverflowException || exception is ThreadAbortException) throw;
                return false;
            }
        }

        internal bool AlwaysOnTopForTesting { get { return TopMost; } }
        internal static int AlwaysOnTopSystemCommandForTesting { get { return SystemMenuAlwaysOnTop; } }
        internal bool PerNoteIntervalGateForTesting { get { return _perNoteIntervalGateEnabled; } }
        internal static int PerNoteIntervalGateSystemCommandForTesting { get { return SystemMenuPerNoteIntervalGate; } }
        internal bool ApplyQueueLimitWithoutSlowdownForTesting { get { return _applyQueueLimitWithoutSlowdown; } }
        internal static int ApplyQueueLimitWithoutSlowdownSystemCommandForTesting
        { get { return SystemMenuApplyQueueLimitWithoutSlowdown; } }
        internal bool ChaseMidiStateOnPlaySeekForTesting { get { return _chaseMidiStateOnPlaySeek; } }
        internal static int ChaseMidiStateSystemCommandForTesting { get { return SystemMenuChaseMidiState; } }
        internal bool PerNoteGateControlsLockedForTesting
        {
            get
            {
                return !_serviceModeCombo.Enabled && !_simulateSlowdownCheck.Enabled &&
                    !_queueLimitCheck.Enabled && !_queueLimitValue.Enabled &&
                    !_overflowPolicyCombo.Enabled && _processingValue.Enabled && _processingSlider.Enabled;
            }
        }

        private void BuildInterface()
        {
            _rootLayout = new TableLayoutPanel();
            _rootLayout.Dock = DockStyle.Fill;
            _rootLayout.Padding = new Padding(5);
            _rootLayout.ColumnCount = 1;
            _rootLayout.RowCount = 4;
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(_rootLayout);

            _rootLayout.Controls.Add(BuildFileAndOutputGroup(), 0, 0);
            _rootLayout.Controls.Add(BuildProcessingGroup(), 0, 1);
            _rootLayout.Controls.Add(BuildPlaybackGroup(), 0, 2);
            _rootLayout.Controls.Add(BuildStatisticsGroup(), 0, 3);
        }

        private Control BuildFileAndOutputGroup()
        {
            GroupBox group = NewGroup("File and output");
            _fileOutputGroup = group;
            TableLayoutPanel table = NewTable(4);
            _fileOutputTable = table;
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            table.RowCount = 2;
            // Both the normal filename and the two-line loading presentation live
            // in this fixed top-row footprint.  Visibility changes therefore do
            // not alter the preferred height of this group.
            table.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            _openButton = new Button();
            _openButton.Text = "Open MIDI...";
            _openButton.AutoSize = true;
            _openButton.Click += OpenMidiClicked;
            table.Controls.Add(_openButton, 0, 0);

            _fileHeaderTable = new TableLayoutPanel();
            _fileHeaderTable.Dock = DockStyle.Fill;
            _fileHeaderTable.Margin = new Padding(0);
            _fileHeaderTable.Padding = new Padding(0);
            _fileHeaderTable.ColumnCount = 2;
            _fileHeaderTable.RowCount = 1;
            _fileHeaderTable.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
            _fileHeaderTable.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
            _fileHeaderTable.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            _fileHeaderTable.SizeChanged += delegate { if (_loadingSong) ConfigureFileHeaderAllocation(true); };
            table.Controls.Add(_fileHeaderTable, 1, 0);
            table.SetColumnSpan(_fileHeaderTable, 3);

            _fileLabel = new BufferedStatusLabel();
            _fileLabel.Text = "No file loaded";
            _fileLabel.AutoEllipsis = true;
            _fileLabel.Dock = DockStyle.Fill;
            _fileLabel.TextAlign = ContentAlignment.MiddleLeft;
            _fileLabel.Margin = new Padding(3, 0, 2, 0);
            _fileHeaderTable.Controls.Add(_fileLabel, 0, 0);

            _fileInfoLabel = new Label();
            _fileInfoLabel.Text = "";
            _fileInfoLabel.AutoSize = true;
            _fileInfoLabel.TextAlign = ContentAlignment.MiddleRight;
            _fileInfoLabel.Anchor = AnchorStyles.Right;
            _fileInfoLabel.Dock = DockStyle.Fill;
            _fileHeaderTable.Controls.Add(_fileInfoLabel, 1, 0);

            _loadingStatusLabel = new BufferedStatusLabel();
            _loadingStatusLabel.AutoEllipsis = true;
            _loadingStatusLabel.Dock = DockStyle.Fill;
            _loadingStatusLabel.Margin = new Padding(2, 0, 0, 0);
            _loadingStatusLabel.TextAlign = ContentAlignment.MiddleLeft;
            _loadingStatusLabel.Visible = false;
            _fileHeaderTable.Controls.Add(_loadingStatusLabel, 1, 0);

            _loadActivity = new ProgressBar();
            _loadActivity.Style = ProgressBarStyle.Continuous;
            _loadActivity.Maximum = 1000;
            _loadActivity.Dock = DockStyle.Fill;
            _loadActivity.Margin = new Padding(0, 1, 0, 1);

            _loadStageActivity = new ProgressBar();
            _loadStageActivity.Style = ProgressBarStyle.Continuous;
            _loadStageActivity.Maximum = 1000;
            _loadStageActivity.Dock = DockStyle.Fill;
            _loadStageActivity.Margin = new Padding(0);

            _loadingPanel = new TableLayoutPanel();
            _loadingPanel.AutoSize = false;
            _loadingPanel.Size = new Size(180, 23);
            _loadingPanel.ColumnCount = 1;
            _loadingPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            _loadingPanel.RowCount = 2;
            _loadingPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 15));
            _loadingPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 6));
            _loadingPanel.Anchor = AnchorStyles.Right;
            _loadingPanel.Margin = new Padding(4, 0, 3, 0);
            _loadingPanel.Visible = false;
            _loadingPanel.Controls.Add(_loadActivity, 0, 0);
            _loadingPanel.Controls.Add(_loadStageActivity, 0, 1);

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
            _toolTip.SetToolTip(_kdmApiCheck, "Send directly through the application-local or installed KDMAPI provider. While checked, KDMAPI is the active output and the WinMM/None list is inactive. Changing output during playback restarts at the current source position and clears backlog/statistics.");
            _toolTip.SetToolTip(_outputCombo, "Choose a Windows MIDI device or None for scheduler-only diagnostics. None processes events and simulator statistics normally but makes no native MIDI calls. Changing output during playback restarts at the current source position and clears backlog/statistics.");
            table.Controls.Add(_kdmApiCheck, 2, 1);

            _stateLabel = new Label();
            _stateLabel.AutoSize = true;
            _stateLabel.Anchor = AnchorStyles.Right;
            _stateLabel.Margin = new Padding(12, 3, 3, 3);
            table.Controls.Add(_stateLabel, 3, 1);
            table.Controls.Add(_loadingPanel, 3, 1);

            group.Controls.Add(table);
            return group;
        }

        private Control BuildProcessingGroup()
        {
            GroupBox group = NewGroup("Processing model");
            TableLayoutPanel table = NewTable(2);
            _processingTable = table;
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 48));
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 52));
            table.RowCount = 4;
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            _queueCluster = NewInlineCluster();
            _overflowCluster = NewInlineCluster();
            _rateCluster = NewInlineCluster();
            _serviceCluster = NewInlineCluster();
            _overflowCluster.Margin = new Padding(4, 0, 0, 0);
            _serviceCluster.Margin = new Padding(4, 0, 0, 0);

            _simulateSlowdownCheck = new CheckBox();
            _simulateSlowdownCheck.Text = "Simulate slowdown";
            _simulateSlowdownCheck.AutoSize = true;
            _simulateSlowdownCheck.Checked = false;
            _simulateSlowdownCheck.CheckedChanged += SimulateSlowdownChanged;
            _toolTip.SetToolTip(_simulateSlowdownCheck, "A live change applies to the next event that begins service.");
            table.Controls.Add(_simulateSlowdownCheck, 0, 1);
            table.SetColumnSpan(_simulateSlowdownCheck, 2);

            _queueLimitCheck = new CheckBox();
            _queueLimitCheck.Text = "Queue length limit:";
            _queueLimitCheck.AutoSize = true;
            _queueLimitCheck.CheckedChanged += QueueLimitChanged;
            _toolTip.SetToolTip(_queueLimitCheck, "Queue structure is locked while playback is active.");
            _queueCluster.Controls.Add(_queueLimitCheck);

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
            _queueCluster.Controls.Add(_queueLimitValue);

            _eventsLabel = new Label();
            _eventsLabel.Text = "events";
            _eventsLabel.AutoSize = true;
            _eventsLabel.Anchor = AnchorStyles.Left;
            _queueCluster.Controls.Add(_eventsLabel);

            _overflowLabel = new Label();
            _overflowLabel.Text = "Overflow:";
            _overflowLabel.AutoSize = true;
            _overflowLabel.Anchor = AnchorStyles.Right;
            _overflowLabel.Margin = new Padding(8, 3, 3, 3);
            _overflowCluster.Controls.Add(_overflowLabel);

            _overflowPolicyCombo = new ComboBox();
            _overflowPolicyCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _overflowPolicyCombo.Items.Add("Drop newest");
            _overflowPolicyCombo.Items.Add("Drop oldest");
            _overflowPolicyCombo.Items.Add("Clear buffer and jump to realtime");
            _overflowPolicyCombo.Items.Add("Drop incoming complete notes");
            _overflowPolicyCombo.Items.Add("Drop oldest complete note");
            _overflowPolicyCombo.SelectedIndex = 0;
            _overflowPolicyCombo.Width = 245;
            _overflowPolicyCombo.DropDownWidth = 245;
            _overflowPolicyCombo.SelectedIndexChanged += OverflowPolicyChanged;
            _toolTip.SetToolTip(_overflowPolicyCombo, "A change made during playback applies at the next overflow.");
            _overflowCluster.Controls.Add(_overflowPolicyCombo);

            _serviceModeLabel = new Label();
            _serviceModeLabel.Text = "Rate model:";
            _serviceModeLabel.AutoSize = true;
            _serviceModeLabel.Anchor = AnchorStyles.Left;

            _serviceModeCombo = new ComboBox();
            _serviceModeCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _serviceModeCombo.Items.Add("Processing time per event");
            _serviceModeCombo.Items.Add("MIDI serial bitrate");
            _serviceModeCombo.Items.Add("Events per second");
            _serviceModeCombo.SelectedIndex = 0;
            _serviceModeCombo.Width = 210;
            _serviceModeCombo.Anchor = AnchorStyles.Left;
            _serviceModeCombo.SelectedIndexChanged += ServiceModeChanged;
            _toolTip.SetToolTip(_serviceModeCombo, "A live change applies when the next event begins service.");

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
            _processingValue.Anchor = AnchorStyles.Left;
            _processingValue.ThousandsSeparator = true;
            _processingValue.ValueChanged += ProcessingValueChanged;
            _toolTip.SetToolTip(_processingValue, "A live edit applies when the next event begins service.");

            _serviceUnitLabel = new Label();
            _serviceUnitLabel.Text = "µs";
            _serviceUnitLabel.AutoSize = true;
            _serviceUnitLabel.Anchor = AnchorStyles.Left;
            _rateCluster.Controls.Add(_serviceModeLabel);
            _rateCluster.Controls.Add(_serviceModeCombo);
            _serviceCluster.Controls.Add(_serviceValueLabel);
            _serviceCluster.Controls.Add(_processingValue);
            _serviceCluster.Controls.Add(_serviceUnitLabel);
            _serviceCluster.Controls.Add(_dinPresetButton);
            table.Controls.Add(_queueCluster, 0, 0);
            table.Controls.Add(_overflowCluster, 1, 0);
            table.Controls.Add(_rateCluster, 0, 2);
            table.Controls.Add(_serviceCluster, 1, 2);

            _processingSlider = new ProcessingTrackBar();
            _processingSlider.Minimum = 0;
            _processingSlider.Maximum = ProcessingTrackBar.ScaleMaximum;
            _processingSlider.TickFrequency = 1000;
            _processingSlider.Dock = DockStyle.Fill;
            _processingSlider.AutoSize = true;
            _processingSlider.ValueChanged += ProcessingSliderChanged;
            _toolTip.SetToolTip(_processingSlider, "Click or drag to set the rate. A live edit applies when the next event begins service.");
            table.Controls.Add(_processingSlider, 0, 3);
            table.SetColumnSpan(_processingSlider, 2);

            group.Controls.Add(table);
            UpdatePolicyControlState();
            return group;
        }

        private static FlowLayoutPanel NewInlineCluster()
        {
            FlowLayoutPanel cluster = new FlowLayoutPanel();
            cluster.AutoSize = true;
            cluster.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            cluster.WrapContents = false;
            cluster.FlowDirection = FlowDirection.LeftToRight;
            cluster.Anchor = AnchorStyles.Left;
            cluster.Margin = new Padding(0);
            return cluster;
        }

        private Control BuildPlaybackGroup()
        {
            GroupBox group = NewGroup("Playback");
            _playbackGroup = group;
            TableLayoutPanel layout = NewTable(1);
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            _timelineView = new PlaybackTimelineView();
            _timelineView.Dock = DockStyle.Top;
            _timelineView.Enabled = false;
            _timelineView.SeekRequested += delegate(object sender, TimelineSeekEventArgs e) { PerformSeek(e.PositionMicroseconds); };
            layout.Controls.Add(_timelineView, 0, 0);
            _playbackButtonLayout = new TableLayoutPanel();
            _playbackButtonLayout.Dock = DockStyle.Top;
            _playbackButtonLayout.AutoSize = true;
            _playbackButtonLayout.ColumnCount = 7;
            for (int column = 0; column < 5; column++)
                _playbackButtonLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            _playbackButtonLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            _playbackButtonLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));

            _playButton = NewButton("Play", PlayClicked);
            _stopButton = NewButton("Stop", StopClicked);
            _seekBack5Button = NewButton("−5 sec", delegate { SeekBy(-5000000); });
            _seekForward5Button = NewButton("+5 sec", delegate { SeekBy(5000000); });
            _analysisButton = NewButton("Analysis...", ShowAnalysisClicked);
            _resetStatsButton = NewButton("Reset stats", ResetStatsClicked);
            _toolTip.SetToolTip(_resetStatsButton, "Reset counters and maximum baselines without seeking, clearing backlog, or resetting MIDI output.");

            _playbackButtonLayout.Controls.Add(_playButton, 0, 0);
            _playbackButtonLayout.Controls.Add(_stopButton, 1, 0);
            _playbackButtonLayout.Controls.Add(_seekBack5Button, 2, 0);
            _playbackButtonLayout.Controls.Add(_seekForward5Button, 3, 0);
            _playbackButtonLayout.Controls.Add(_analysisButton, 4, 0);
            _playbackButtonLayout.Controls.Add(_resetStatsButton, 6, 0);
            layout.Controls.Add(_playbackButtonLayout, 0, 1);
            group.Controls.Add(layout);
            return group;
        }

        private Control BuildStatisticsGroup()
        {
            GroupBox group = NewGroup("Statistics");
            _statisticsGroup = group;
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
            group.Padding = new Padding(6, 5, 6, 4);
            group.Margin = new Padding(2, 1, 2, 3);
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
            List<object> selections = CreateOutputSelections(devices);
            for (int i = 0; i < selections.Count; i++) _outputCombo.Items.Add(selections[i]);
            _outputCombo.SelectedIndex = DefaultOutputSelectionIndex(devices);
        }

        internal static List<object> CreateOutputSelections(IList<MidiOutputDeviceInfo> devices)
        {
            List<object> selections = new List<object>((devices == null ? 0 : devices.Count) + 1);
            selections.Add(NoMidiOutputSelection.Instance);
            if (devices != null)
                for (int i = 0; i < devices.Count; i++) selections.Add(devices[i]);
            return selections;
        }

        internal static int DefaultOutputSelectionIndex(IList<MidiOutputDeviceInfo> devices)
        {
            // Preserve the former default of the first real Windows device.
            // If there is none, the diagnostic sink remains fully usable.
            return devices != null && devices.Count > 0 ? 1 : 0;
        }

        internal static bool IsNoOutputSelection(object selection)
        {
            return selection is NoMidiOutputSelection;
        }

        private void OpenMidiClicked(object sender, EventArgs e)
        {
            if (_loadingSong)
            {
                CancelMidiLoad();
                return;
            }
            using (OpenFileDialog dialog = new OpenFileDialog())
            {
                dialog.Title = "Open Standard MIDI File";
                dialog.Filter = "MIDI files (*.mid;*.midi)|*.mid;*.midi|All files (*.*)|*.*";
                dialog.CheckFileExists = true;
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                BeginMidiLoad(dialog.FileName);
            }
        }

        internal void BeginMidiLoad(string path)
        {
            if (String.IsNullOrWhiteSpace(path)) throw new ArgumentException("A MIDI file path is required.", "path");
            CancelMidiLoad();
            UnloadCurrentSong(true);
            CancellationTokenSource cancellation = new CancellationTokenSource();
            _loadCancellation = cancellation;
            int generation = ++_loadGeneration;
            _loadingSong = true;
            _lastLoadError = null;
            _loadProgress = new MidiLoadProgress("Starting", 0, 0);
            _loadingFileName = Path.GetFileName(path);
            _loadStartedTimestamp = Stopwatch.GetTimestamp();
            _loadDecisionPauseStartedTimestamp = 0;
            _loadDecisionPausedTicks = 0;
            _lastLoadTelemetrySecond = -1;
            _lastLoadTelemetryText = null;
            _loadingTelemetryUpdateCount = 0;
            _lastLoadStatus = null;
            _lastLoadTooltipStage = null;
            _openButton.Text = "Cancel";
            _loadActivity.Value = 0;
            _loadStageActivity.Value = 0;
            _loadingStatusLabel.Text = "Starting…" + Environment.NewLine + "0.0% overall";
            UpdateLoadingFileTelemetry(true);
            _toolTip.SetToolTip(_fileLabel, path + Environment.NewLine +
                "The second line shows elapsed loading time and memory committed exclusively to this MIDIBottleneck Player process. " +
                "Private commit may be resident or paged out; it is not managed-heap size or working set.");
            _fileInfoLabel.Text = String.Empty;
            SetLoadingPresentation(true);
            UpdateTransportControls();

            Task.Factory.StartNew(delegate
            {
                return MidiFileParser.LoadWithPreflight(path, cancellation.Token, delegate(MidiLoadProgress progress)
                {
                    _loadProgress = progress;
                }, delegate(MidiLargeFileInspection inspection)
                {
                    return RequestLargeFileDecision(inspection, generation, cancellation);
                }, _forceLargeFilePreflightForTests, _forceLargeFileWarningForTests);
            }, cancellation.Token, TaskCreationOptions.LongRunning, TaskScheduler.Default).ContinueWith(delegate(Task<MidiSong> task)
            {
                if (IsDisposed || !IsHandleCreated) return;
                try
                {
                    BeginInvoke((MethodInvoker)delegate
                    {
                        if (IsDisposed || generation != _loadGeneration || cancellation != _loadCancellation) return;
                        _loadCancellation = null;
                        cancellation.Dispose();
                        _loadingSong = false;
                        _loadProgress = null;
                        _loadingFileName = null;
                        ResetLoadingTelemetryState();
                        _lastLoadTooltipStage = null;
                        SetLoadingPresentation(false);
                        _openButton.Text = "Open MIDI...";
                        if (task.IsCanceled || (!task.IsFaulted && task.Result == null))
                        {
                            _fileLabel.Text = "No file loaded";
                            _toolTip.SetToolTip(_fileLabel, String.Empty);
                            SetDetachedWindowsMessage("MIDI loading cancelled. No MIDI file is loaded.");
                        }
                        else if (task.IsFaulted)
                        {
                            _fileLabel.Text = "No file loaded";
                            _toolTip.SetToolTip(_fileLabel, String.Empty);
                            _lastLoadError = task.Exception.GetBaseException();
                            if (!_suppressLoadErrorDialogs)
                                MessageBox.Show(this, _lastLoadError.Message, "Unable to load MIDI file", MessageBoxButtons.OK, MessageBoxIcon.Error);
                            SetDetachedWindowsMessage("MIDI loading failed. No MIDI file is loaded.");
                        }
                        else
                        {
                            _song = task.Result;
                            _selectedPositionMicroseconds = 0;
                            _fileLabel.Text = Path.GetFileName(_song.FilePath);
                            _toolTip.SetToolTip(_fileLabel, _song.FilePath);
                            UpdateFileInformation();
                            AnalysisConfiguration configuration = CurrentAnalysisConfiguration();
                            DiagnosticsForm[] windows = _analysisWindows.ToArray();
                            for (int index = 0; index < windows.Length; index++)
                            {
                                DiagnosticsForm window = windows[index];
                                if (window != null && !window.IsDisposed) window.AttachSong(_song, configuration);
                            }
                            if (_channelMonitor != null && !_channelMonitor.IsDisposed)
                            {
                                _engine.SetChannelMonitoring(true);
                                _channelMonitor.AttachSong(Path.GetFileName(_song.FilePath));
                                _channelMonitor.UpdateSnapshot(_engine.GetChannelSnapshot());
                            }
                        }
                        UpdateSeekDisplay();
                        UpdateTransportControls();
                        RefreshStatistics();
                    });
                }
                catch (InvalidOperationException) { }
            });
        }

        private bool RequestLargeFileDecision(MidiLargeFileInspection inspection, int generation,
            CancellationTokenSource cancellation)
        {
            if (inspection == null || cancellation.IsCancellationRequested) return false;
            bool accepted = false;
            try
            {
                MethodInvoker show = delegate
                {
                    if (IsDisposed || generation != _loadGeneration || cancellation != _loadCancellation ||
                        cancellation.IsCancellationRequested) return;
                    BeginLoadingDecisionPause();
                    try
                    {
                        _lastLargeFileInspection = inspection;
                        if (_largeFileWarningHandlerForTests != null)
                            accepted = _largeFileWarningHandlerForTests(inspection);
                        else
                        {
                            using (LargeMidiWarningDialog dialog = new LargeMidiWarningDialog(inspection))
                                accepted = dialog.ShowDialog(this) == DialogResult.OK;
                        }
                    }
                    finally { EndLoadingDecisionPause(); }
                };
                if (InvokeRequired) Invoke(show); else show();
            }
            catch (InvalidOperationException) { return false; }
            return accepted;
        }

        internal void CancelMidiLoad()
        {
            CancellationTokenSource cancellation = _loadCancellation;
            if (cancellation == null) return;
            ++_loadGeneration;
            _loadCancellation = null;
            cancellation.Cancel();
            cancellation.Dispose();
            _loadingSong = false;
            _loadProgress = null;
            _loadingFileName = null;
            ResetLoadingTelemetryState();
            _lastLoadTooltipStage = null;
            SetLoadingPresentation(false);
            if (_openButton != null) _openButton.Text = "Open MIDI...";
            if (_fileLabel != null)
            {
                _fileLabel.Text = "No file loaded";
                _toolTip.SetToolTip(_fileLabel, String.Empty);
            }
            if (_fileInfoLabel != null)
            {
                _fileInfoLabel.Text = String.Empty;
                _fileInfoLabel.Visible = true;
            }
            SetDetachedWindowsMessage("MIDI loading cancelled. No MIDI file is loaded.");
            UpdateTransportControls();
        }

        private void SetDetachedWindowsMessage(string message)
        {
            DiagnosticsForm[] windows = _analysisWindows.ToArray();
            for (int index = 0; index < windows.Length; index++)
                if (windows[index] != null && !windows[index].IsDisposed)
                    windows[index].DetachForSongReplacement(message);
            if (_channelMonitor != null && !_channelMonitor.IsDisposed)
                _channelMonitor.DetachForSongReplacement(message);
        }

        private void SetLoadingPresentation(bool loading)
        {
            if (_fileOutputTable == null) return;
            if (_fileOutputGroup != null) _fileOutputGroup.SuspendLayout();
            _fileOutputTable.SuspendLayout();
            try
            {
                _fileInfoLabel.Visible = !loading;
                _loadingStatusLabel.Visible = loading;
                _stateLabel.Visible = !loading;
                _loadingPanel.Visible = loading;
                ConfigureFileHeaderAllocation(loading);
            }
            finally
            {
                _fileOutputTable.ResumeLayout(true);
                if (_fileOutputGroup != null) _fileOutputGroup.ResumeLayout(true);
            }
        }

        private void ConfigureFileHeaderAllocation(bool loading)
        {
            if (_fileHeaderTable == null) return;
            if (!loading)
            {
                _fileHeaderTable.ColumnStyles[0].SizeType = SizeType.Percent;
                _fileHeaderTable.ColumnStyles[1].SizeType = SizeType.Percent;
                float filePercent = _compactLayout ? 45F : 48F;
                _fileHeaderTable.ColumnStyles[0].Width = filePercent;
                _fileHeaderTable.ColumnStyles[1].Width = 100F - filePercent;
                return;
            }

            // The parser side needs enough space to communicate both its stage
            // and percentages, but it must not starve the filename/telemetry.
            // Make the status column a measured absolute width and leave the
            // rest to the flexible filename column instead of encoding a
            // product rule as a dominant fixed percentage.
            int available = Math.Max(0, _fileHeaderTable.ClientSize.Width);
            int fileMinimum = _compactLayout ? 145 : 240;
            int statusMinimum = _compactLayout ? 165 : 215;
            int statusIdeal = statusMinimum;
            if (_loadingStatusLabel != null && !String.IsNullOrEmpty(_loadingStatusLabel.Text))
            {
                string[] lines = _loadingStatusLabel.Text.Split(new string[] { Environment.NewLine }, StringSplitOptions.None);
                for (int index = 0; index < lines.Length; index++)
                    statusIdeal = Math.Max(statusIdeal, TextRenderer.MeasureText(lines[index], _loadingStatusLabel.Font).Width + 12);
            }
            statusIdeal = Math.Min(statusIdeal, _compactLayout ? 215 : 300);
            int statusWidth = Math.Min(statusIdeal, Math.Max(statusMinimum, available - fileMinimum));
            if (available < fileMinimum + statusMinimum)
                statusWidth = Math.Max(1, available / 2);
            _fileHeaderTable.ColumnStyles[0].SizeType = SizeType.Percent;
            _fileHeaderTable.ColumnStyles[0].Width = 100F;
            _fileHeaderTable.ColumnStyles[1].SizeType = SizeType.Absolute;
            _fileHeaderTable.ColumnStyles[1].Width = statusWidth;
        }


        internal void UnloadCurrentSong()
        {
            UnloadCurrentSong(false);
        }

        private void UnloadCurrentSong(bool preserveAnalysisShells)
        {
            if (preserveAnalysisShells)
            {
                if (_channelMonitor != null && !_channelMonitor.IsDisposed)
                    _channelMonitor.DetachForSongReplacement("Loading new MIDI…");
                _engine.SetChannelMonitoring(false);
            }
            else CloseChannelMonitor();
            DiagnosticsForm[] windows = _analysisWindows.ToArray();
            for (int i = 0; i < windows.Length; i++)
            {
                if (windows[i] == null || windows[i].IsDisposed) continue;
                if (preserveAnalysisShells) windows[i].DetachForSongReplacement("Loading new MIDI…");
                else windows[i].Close();
            }
            if (!preserveAnalysisShells) _analysisWindows.Clear();
            _engine.Unload();
            _engineSong = null;
            _song = null;
            _selectedPositionMicroseconds = 0;
            ResetLiveMeasurements();
        }

        internal bool IsLoadingSong { get { return _loadingSong; } }
        internal event Action<long> LoadingVisualSampled;
        internal bool EngineHasAttachedSong { get { return _engine.HasAttachedSong; } }
        internal bool EngineHasAttachedOutput { get { return _engine.HasAttachedOutput; } }
        internal MidiSong CurrentSong { get { return _song; } }
        internal bool SuppressLoadErrorDialogs { get { return _suppressLoadErrorDialogs; } set { _suppressLoadErrorDialogs = value; } }
        internal Exception LastLoadError { get { return _lastLoadError; } }
        internal bool LoadingActivityVisible { get { return _loadingPanel != null && _loadingPanel.Visible; } }
        internal int LoadingOverallPermille { get { return _loadActivity == null ? 0 : _loadActivity.Value; } }
        internal int LoadingStagePermille { get { return _loadStageActivity == null ? 0 : _loadStageActivity.Value; } }
        internal string LoadingStageText { get { return _loadingStatusLabel == null ? String.Empty : _loadingStatusLabel.Text; } }
        internal string LoadingFileText { get { return _fileLabel == null ? String.Empty : _fileLabel.Text; } }
        internal int LoadingFileAreaWidth { get { return _fileLabel == null ? 0 : _fileLabel.Width; } }
        internal int LoadingStatusAreaWidth { get { return _loadingStatusLabel == null ? 0 : _loadingStatusLabel.Width; } }
        internal int LoadingProgressWidth { get { return _loadingPanel == null ? 0 : _loadingPanel.Width; } }
        internal int LoadingTelemetryUpdateCount { get { return _loadingTelemetryUpdateCount; } }
        internal string OpenCommandText { get { return _openButton == null ? String.Empty : _openButton.Text; } }
        internal Func<MidiLargeFileInspection, bool> LargeFileWarningHandlerForTests
        { get { return _largeFileWarningHandlerForTests; } set { _largeFileWarningHandlerForTests = value; } }
        internal bool ForceLargeFilePreflightForTests
        { get { return _forceLargeFilePreflightForTests; } set { _forceLargeFilePreflightForTests = value; } }
        internal bool ForceLargeFileWarningForTests
        { get { return _forceLargeFileWarningForTests; } set { _forceLargeFileWarningForTests = value; } }
        internal MidiLargeFileInspection LastLargeFileInspection { get { return _lastLargeFileInspection; } }

        private void PlayClicked(object sender, EventArgs e)
        {
            if (_engine.State == PlaybackState.Playing)
            {
                try { _engine.Pause(); }
                catch (PlaybackWorkerTimeoutException ex)
                {
                    _outputError = ex.Message;
                    RestartLiveMeasurementWindows();
                    UpdateTransportControls();
                    RefreshStatistics();
                    MessageBox.Show(this, ex.Message, "Unable to pause MIDI output safely", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                RestartLiveMeasurementWindows();
                UpdateTransportControls();
                return;
            }
            if (_engine.State == PlaybackState.Paused)
            {
                _engine.Resume();
                RestartLiveMeasurementWindows();
                UpdateTransportControls();
                return;
            }
            if (_song == null)
            {
                MessageBox.Show(this, "Open a MIDI file first.", "No MIDI file", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (IsUnsupportedForwardQueueSelection())
            {
                MessageBox.Show(this,
                    "This policy needs Simulate slowdown: a forward-only queue cannot remove MIDI that was already sent. Turn on Simulate slowdown, choose Drop newest or Drop incoming complete notes, or turn off Apply queue limit without slowdown in the window menu.",
                    "Queue policy needs simulated slowdown", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (!_kdmApiCheck.Checked && !(_outputCombo.SelectedItem is MidiOutputDeviceInfo) &&
                !IsNoOutputSelection(_outputCombo.SelectedItem))
            {
                MessageBox.Show(this, "No Windows MIDI output device is available.", "No MIDI output", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                if (_selectedPositionMicroseconds >= _song.DurationMicroseconds)
                    _selectedPositionMicroseconds = 0;
                IMidiOutput selectedOutput = OpenSelectedOutput();
                _engine.Start(_song, selectedOutput, SelectedProcessingMode(), _selectedPositionMicroseconds);
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
                if (_activeOutput == _output) _output.Dispose();
                else if (_activeOutput == _nullOutput) _nullOutput.Close();
                _kdmApiOutput.Open();
                return _kdmApiOutput;
            }
            if (IsNoOutputSelection(_outputCombo.SelectedItem))
            {
                if (_activeOutput == _kdmApiOutput) _kdmApiOutput.Close();
                else if (_activeOutput == _output) _output.Dispose();
                _nullOutput.Open();
                return _nullOutput;
            }
            MidiOutputDeviceInfo device = _outputCombo.SelectedItem as MidiOutputDeviceInfo;
            if (device == null) throw new InvalidOperationException("No Windows MIDI output device is selected.");
            if (_activeOutput == _kdmApiOutput) _kdmApiOutput.Close();
            else if (_activeOutput == _nullOutput) _nullOutput.Close();
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
                else if (_activeOutput == _nullOutput) _nullOutput.Close();
                _activeOutput = null;
                _selectedPositionMicroseconds = restartPosition;
                ResetLiveMeasurements();

                IMidiOutput replacement = OpenSelectedOutput();
                _engine.Start(_song, replacement, SelectedProcessingMode(),
                    restartPosition, previousState == PlaybackState.Paused);
                _activeOutput = replacement;
                _engineSong = _song;
                _outputError = null;
                ResetLiveMeasurements();
            }
            catch (Exception ex)
            {
                // A timed-out in-process native send still owns the selected
                // backend until its worker returns and performs the final
                // reset/panic.  Do not close that backend underneath it.
                if (!_engine.HasLiveWorker)
                {
                    try { _output.Dispose(); } catch { }
                    try { _kdmApiOutput.Dispose(); } catch { }
                    try { _nullOutput.Dispose(); } catch { }
                    _activeOutput = null;
                }
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
            try { _engine.Stop(); }
            catch (PlaybackWorkerTimeoutException ex)
            {
                _outputError = ex.Message;
                RestartLiveMeasurementWindows();
                UpdateTransportControls();
                RefreshStatistics();
                MessageBox.Show(this, ex.Message, "Unable to stop MIDI output safely", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            _selectedPositionMicroseconds = 0;
            RestartLiveMeasurementWindows();
            UpdateSeekDisplay();
            UpdateTransportControls();
            RefreshStatistics();
        }

        private ProcessingMode SelectedProcessingMode()
        {
            if (_perNoteIntervalGateEnabled) return ProcessingMode.PerNoteIntervalGate;
            return _queueLimitCheck.Checked ? ProcessingMode.Drop : ProcessingMode.Queue;
        }

        private void ProcessingValueChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
            {
                ApplyMidiBitrate(Decimal.ToInt64(_processingValue.Value), true);
                return;
            }
            if (_engine.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
            {
                ApplyEventsPerSecond(Decimal.ToInt64(_processingValue.Value), true);
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
            if (_engine.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
            {
                ApplyEventsPerSecond(SliderToEventRate(_processingSlider.Value), false);
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
            if (_perNoteIntervalGateEnabled && microseconds == 0) microseconds = 1;
            long previousMicroseconds = _engine.ProcessingMicroseconds;
            PlaybackState previousState = _engine.State;
            _engine.ProcessingMicroseconds = microseconds;
            _updatingProcessingControls = true;
            try
            {
                if (!fromNumeric || _processingValue.Value != microseconds)
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
            if (_perNoteIntervalGateEnabled && previousMicroseconds != microseconds &&
                (previousState == PlaybackState.Playing || previousState == PlaybackState.Paused))
                TryRestartForProcessingModeChange(previousState);
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void UpdateProcessingSummary(long microseconds)
        {
            _toolTip.SetToolTip(_processingSlider,
                _perNoteIntervalGateEnabled
                    ? "Sets the nonzero interval for the per-note gate. A live edit safely silences and restarts the scheduler at the same position."
                    : "Click or drag to set the rate. Fine adjustment to 5,000 µs; logarithmic above. A live edit applies when the next event begins service.");
        }

        private void ServiceModeChanged(object sender, EventArgs e)
        {
            if (_updatingProcessingControls) return;
            PlaybackState previousState = _engine.State;
            ServiceDurationMode previousMode = _engine.ServiceDurationMode;
            ServiceDurationMode mode = (ServiceDurationMode)Math.Max(0, _serviceModeCombo.SelectedIndex);
            long processing = _engine.ProcessingMicroseconds;
            long eventRate = _engine.EventsPerSecond;
            if (mode == ServiceDurationMode.EventsPerSecond && previousMode == ServiceDurationMode.ProcessingTime)
                eventRate = ProcessingToEventRate(processing);
            else if (mode == ServiceDurationMode.ProcessingTime && previousMode == ServiceDurationMode.EventsPerSecond)
                processing = EventRateToProcessing(eventRate);
            _engine.SetServiceConfiguration(mode, processing, _engine.MidiBitrate, eventRate);
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
                    _processingValue.Width = _compactLayout ? 89 : 110;
                    _processingValue.DecimalPlaces = 0;
                    _processingValue.Minimum = 1;
                    _processingValue.Maximum = 100000000;
                    _processingValue.Increment = 100;
                    _processingValue.Value = Math.Min(_processingValue.Maximum, _engine.MidiBitrate);
                    _serviceUnitLabel.Text = "bit/s";
                    _dinPresetButton.Visible = !_compactLayout;
                    _processingSlider.Value = BitrateToSlider(_engine.MidiBitrate);
                }
                else if (_engine.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
                {
                    _serviceValueLabel.Text = "Events/sec:";
                    _processingValue.Width = _compactLayout ? 77 : 100;
                    _processingValue.DecimalPlaces = 0;
                    _processingValue.Minimum = 0;
                    _processingValue.Maximum = 1000000;
                    _processingValue.Increment = 1;
                    _processingValue.Value = _engine.EventsPerSecond;
                    _serviceUnitLabel.Text = String.Empty;
                    _dinPresetButton.Visible = false;
                    _processingSlider.Value = EventRateToSlider(_engine.EventsPerSecond);
                }
                else
                {
                    _serviceValueLabel.Text = _compactLayout ? "Time/event:" : "Processing time per event:";
                    _processingValue.Width = _compactLayout ? 77 : 100;
                    _processingValue.Minimum = _perNoteIntervalGateEnabled ? 1 : 0;
                    _serviceUnitLabel.Text = "µs";
                    _dinPresetButton.Visible = false;
                }
            }
            finally { _updatingProcessingControls = false; }
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                UpdateBitrateSummary(_engine.MidiBitrate);
            else if (_engine.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
                UpdateEventRateSummary(_engine.EventsPerSecond);
            else
                SetProcessingMicroseconds(_engine.ProcessingMicroseconds);
            UpdatePolicyControlState();
        }

        private void ApplyMidiBitrate(long bitrate, bool fromNumeric)
        {
            if (bitrate < 1) bitrate = 1;
            if (bitrate > 100000000) bitrate = 100000000;
            long previous = _engine.MidiBitrate;
            PlaybackState previousState = _engine.State;
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
            _toolTip.SetToolTip(_processingSlider,
                "Click or drag to set the bitrate. Uses 10 transmitted bits per MIDI byte. A live edit applies when the next event begins service.");
        }

        private void ApplyEventsPerSecond(long rate, bool fromNumeric)
        {
            rate = Math.Max(0, Math.Min(1000000, rate));
            long previous = _engine.EventsPerSecond;
            PlaybackState previousState = _engine.State;
            _engine.EventsPerSecond = rate;
            _updatingProcessingControls = true;
            try
            {
                if (!fromNumeric) _processingValue.Value = rate;
                int slider = EventRateToSlider(rate);
                if (_processingSlider.Value != slider) _processingSlider.Value = slider;
            }
            finally { _updatingProcessingControls = false; }
            UpdateEventRateSummary(rate);
            RefreshStatistics();
            ScheduleAnalysisRefresh();
        }

        private void UpdateEventRateSummary(long rate)
        {
            _toolTip.SetToolTip(_processingSlider, rate == 0
                ? "Unlimited: modeled service is immediate. Move right to choose 1 through 1,000,000 events per second."
                : "Click or drag from lower to higher event rates. Fractional microseconds are distributed exactly across events. A live edit applies when the next event begins service.");
        }

        private void SimulateSlowdownChanged(object sender, EventArgs e)
        {
            PlaybackState previousState = _engine.State;
            bool restartForwardModel = _queueLimitCheck.Checked && _applyQueueLimitWithoutSlowdown &&
                (previousState == PlaybackState.Playing || previousState == PlaybackState.Paused);
            _engine.SimulateSlowdown = _simulateSlowdownCheck.Checked;
            UpdatePolicyControlState();
            if (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                UpdateBitrateSummary(_engine.MidiBitrate);
            else if (_engine.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
                UpdateEventRateSummary(_engine.EventsPerSecond);
            else
                UpdateProcessingSummary(_engine.ProcessingMicroseconds);
            RefreshStatistics();
            ScheduleAnalysisRefresh();
            if (restartForwardModel) TryRestartForProcessingModeChange(previousState);
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
            OverflowPolicy policy = (OverflowPolicy)Math.Max(0, _overflowPolicyCombo.SelectedIndex);
            _engine.OverflowPolicy = policy;
            _toolTip.SetToolTip(_overflowPolicyCombo, policy == OverflowPolicy.DropIncomingCompleteNotes
                ? "When full, reject an incoming Note On and later suppress its paired Note Off. Required Note Off and non-note messages are retained, so the configured capacity is a soft safety limit. A live change applies at the next overflow."
                : policy == OverflowPolicy.DropOldestCompleteNote
                    ? "When full, remove the oldest queued Note On that has not begun service, including its queued release. A later matching release is suppressed. Required releases may exceed the limit; if no old note is eligible, other incoming MIDI is rejected. Requires Simulate slowdown."
                : policy == OverflowPolicy.DropOldest || policy == OverflowPolicy.ClearBufferAndCatchUp
                    ? "This policy requires Simulate slowdown. A forward-only queue cannot retract MIDI that has already been sent."
                    : "A change made during playback applies at the next overflow.");
            UpdateTransportControls();
            ScheduleAnalysisRefresh();
        }

        private bool IsUnsupportedForwardQueueSelection()
        {
            if (_perNoteIntervalGateEnabled || !_queueLimitCheck.Checked || _simulateSlowdownCheck.Checked ||
                !_applyQueueLimitWithoutSlowdown) return false;
            OverflowPolicy policy = (OverflowPolicy)Math.Max(0, _overflowPolicyCombo.SelectedIndex);
            return policy == OverflowPolicy.DropOldest ||
                policy == OverflowPolicy.DropOldestCompleteNote ||
                policy == OverflowPolicy.ClearBufferAndCatchUp;
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
            bool gate = _perNoteIntervalGateEnabled;
            bool forwardModel = !gate && limited && !slowdown && _applyQueueLimitWithoutSlowdown;
            _simulateSlowdownCheck.Enabled = !gate;
            _queueLimitCheck.Enabled = !gate && !active;
            _queueLimitValue.Enabled = !gate && limited && !active;
            _overflowPolicyCombo.Enabled = !gate && limited && !(active && forwardModel);
            _serviceModeCombo.Enabled = !gate && (slowdown || forwardModel);
            _processingValue.Enabled = gate || slowdown || forwardModel;
            _processingSlider.Enabled = gate || slowdown || forwardModel;
            _dinPresetButton.Enabled = !gate && (slowdown || forwardModel) && _engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate;
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

        internal static int EventRateToSlider(long rate)
        {
            if (rate <= 0) return 0;
            double normalized = Math.Log10(Math.Max(1, Math.Min(1000000, rate))) / 6.0;
            return 1 + Math.Max(0, Math.Min(ProcessingTrackBar.ScaleMaximum - 1,
                (int)Math.Round(normalized * (ProcessingTrackBar.ScaleMaximum - 1))));
        }

        internal static long SliderToEventRate(int slider)
        {
            if (slider <= 0) return 0;
            double normalized = (Math.Min(ProcessingTrackBar.ScaleMaximum, slider) - 1) /
                (double)(ProcessingTrackBar.ScaleMaximum - 1);
            return Math.Max(1, Math.Min(1000000, (long)Math.Round(Math.Pow(10.0, normalized * 6.0))));
        }

        internal static long ProcessingToEventRate(long microseconds)
        {
            if (microseconds <= 0) return 0;
            return Math.Max(1, Math.Min(1000000,
                (long)Math.Round(1000000.0 / microseconds, MidpointRounding.AwayFromZero)));
        }

        internal static long EventRateToProcessing(long rate)
        {
            if (rate <= 0) return 0;
            return Math.Max(1, Math.Min(1000000,
                (long)Math.Round(1000000.0 / rate, MidpointRounding.AwayFromZero)));
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
            UpdateLoadStatus();
            PlaybackSnapshot snapshot = _engine.GetSnapshot();
            if (!_timelineView.IsDragging && snapshot.State != PlaybackState.Stopped)
            {
                _selectedPositionMicroseconds = snapshot.IntendedTimelineMicroseconds;
                UpdateSeekDisplay();
            }
            long sampleTime = StopwatchTicksToMicroseconds(Stopwatch.GetTimestamp());
            bool synchronized = snapshot.ProcessingMode != ProcessingMode.PerNoteIntervalGate &&
                snapshot.OutstandingEvents == 0 && snapshot.CurrentLagMicroseconds == 0 &&
                snapshot.LastDispatchedTimelineMicroseconds <= snapshot.IntendedTimelineMicroseconds;
            double? speed = snapshot.State == PlaybackState.Playing
                ? _effectiveSpeed.Add(snapshot.PlaybackMicroseconds, snapshot.IntendedTimelineMicroseconds,
                    snapshot.EffectiveSpeedFrontierMicroseconds, snapshot.ProcessedEvents, synchronized, sampleTime)
                : (double?)null;
            double? outputRate = snapshot.State == PlaybackState.Playing
                ? _outputRate.Add(snapshot.ProcessedEvents, sampleTime) : (double?)null;
            string configuredRate = _perNoteIntervalGateEnabled
                ? FormatPerNoteFrameRate(_engine.ProcessingMicroseconds, _compactLayout)
                : FormatMaximumRate(snapshot, _outputRate.MaximumObserved, _compactLayout);
            _statisticsView.PerNoteIntervalGate = _perNoteIntervalGateEnabled;
            if (snapshot.DroppedEvents > _lastDroppedEvents)
                _overflowVisibleUntilMicroseconds = sampleTime + 800000;
            _lastDroppedEvents = snapshot.DroppedEvents;
            _statisticsView.SetValues(new string[]
            {
                FormatTime(snapshot.IntendedTimelineMicroseconds) + " / " + FormatTime(snapshot.LastDispatchedTimelineMicroseconds),
                snapshot.OutstandingEvents.ToString("N0", CultureInfo.CurrentCulture) + " / " + snapshot.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture) +
                    (snapshot.VirtualQueueActive ? (_compactLayout ? " actual" : " actual backlog") : String.Empty),
                configuredRate,
                _compactLayout && outputRate.HasValue
                    ? outputRate.Value.ToString("N0", CultureInfo.CurrentCulture) + " events/s"
                    : RollingOutputRate.Format(outputRate),
                FormatEventCounts(snapshot, _compactLayout),
                EffectivePlaybackSpeed.Format(speed),
                FormatLagMilliseconds(snapshot.MaximumLagMicroseconds),
                FormatLagMilliseconds(snapshot.CurrentLagMicroseconds)
            });
            _statisticsView.SetQueuePressure(_queueLimitCheck.Checked && !_perNoteIntervalGateEnabled,
                snapshot.VirtualQueueActive ? snapshot.VirtualOutstandingEvents : snapshot.OutstandingEvents,
                Decimal.ToInt64(_queueLimitValue.Value), sampleTime < _overflowVisibleUntilMicroseconds,
                snapshot.VirtualQueueActive);
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
                    analysisWindows[i].UpdatePlaybackSnapshot(snapshot, new PlaybackOverlayData
                    {
                        State = snapshot.State.ToString(),
                        TimelineAndOutput = FormatTime(snapshot.IntendedTimelineMicroseconds) + " / " + FormatTime(snapshot.LastDispatchedTimelineMicroseconds),
                        Queue = snapshot.OutstandingEvents.ToString("N0", CultureInfo.CurrentCulture) + " / " + snapshot.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture),
                        Events = FormatEventCounts(snapshot, false),
                        OutputRate = RollingOutputRate.Format(outputRate),
                        EffectiveSpeed = EffectivePlaybackSpeed.Format(speed),
                        Lag = FormatLagMilliseconds(snapshot.CurrentLagMicroseconds) + " / " + FormatLagMilliseconds(snapshot.MaximumLagMicroseconds)
                    });
            if (_channelMonitor != null && !_channelMonitor.IsDisposed)
            {
                UpdateChannelSourceReadouts(snapshot.State == PlaybackState.Stopped
                    ? _selectedPositionMicroseconds : snapshot.IntendedTimelineMicroseconds);
                _channelMonitor.UpdateSnapshot(_engine.GetChannelSnapshot());
            }
        }

        private static long StopwatchTicksToMicroseconds(long ticks)
        {
            return (long)(((decimal)ticks * 1000000m) / Stopwatch.Frequency);
        }

        private void UpdateTransportControls()
        {
            PlaybackState state = _engine.State;
            bool active = state == PlaybackState.Playing || state == PlaybackState.Paused;
            _playButton.Enabled = _song != null && !_loadingSong && (active || !IsUnsupportedForwardQueueSelection());
            _playButton.Text = state == PlaybackState.Playing ? "Pause" : state == PlaybackState.Paused ? "Resume" : "Play";
            _stopButton.Enabled = active || state == PlaybackState.Completed;
            _outputCombo.Enabled = !_kdmApiCheck.Checked && _outputCombo.Items.Count > 0;
            UpdatePolicyControlState();
            bool canSeek = _song != null && !_loadingSong;
            _timelineView.Enabled = canSeek;
            _seekBack5Button.Enabled = canSeek;
            _seekForward5Button.Enabled = canSeek;
            _analysisButton.Enabled = canSeek;
            _stateLabel.Text = state.ToString();
            _toolTip.SetToolTip(_playButton, IsUnsupportedForwardQueueSelection()
                ? "This overflow policy requires Simulate slowdown because sent MIDI cannot be removed from a forward-only queue."
                : String.Empty);
            UpdateForwardQueueMenuState();
            UpdateStateChaseMenuState();
        }

        private void UpdateLoadStatus()
        {
            if (!_loadingSong) return;
            MidiLoadProgress progress = _loadProgress;
            if (progress == null) return;
            long now = Stopwatch.GetTimestamp();
            // Loading consumes only the newest immutable parser snapshot on
            // the existing presentation heartbeat.  Missed UI frames are not
            // queued or replayed, and unchanged visual values remain untouched.
            Action<long> sampled = LoadingVisualSampled;
            if (sampled != null) sampled(now);

            // Bar sampling is independent of descriptive text changes.  The
            // parser publishes only its latest immutable progress snapshot, so
            // this cannot queue redundant UI work.
            if (_loadActivity.Value != progress.OverallPermille) _loadActivity.Value = progress.OverallPermille;
            if (_loadStageActivity.Value != progress.StagePermille) _loadStageActivity.Value = progress.StagePermille;
            UpdateLoadingFileTelemetry(false);
            string detail = progress.Stage + Environment.NewLine +
                (progress.OverallPermille / 10.0).ToString("N1", CultureInfo.CurrentCulture) + "% overall  •  " +
                (progress.StagePermille / 10.0).ToString("N1", CultureInfo.CurrentCulture) + "% stage";
            if (!String.Equals(detail, _lastLoadStatus, StringComparison.Ordinal))
            {
                _lastLoadStatus = detail;
                _loadingStatusLabel.Text = detail;
            }
            // Stage changes are infrequent.  Keep a useful full-description
            // tooltip, but do not rebuild native tooltip text for every changing
            // percentage frame.
            if (!String.Equals(_lastLoadTooltipStage, progress.Stage, StringComparison.Ordinal))
            {
                _lastLoadTooltipStage = progress.Stage;
                string hover = progress.Stage + Environment.NewLine +
                    "The upper bar shows overall parser work; the lower bar shows the current stage. Click Cancel to stop loading.";
                _toolTip.SetToolTip(_loadingPanel, hover);
                _toolTip.SetToolTip(_loadingStatusLabel, hover);
            }
        }

        private void UpdateLoadingFileTelemetry(bool force)
        {
            if (!_loadingSong || String.IsNullOrEmpty(_loadingFileName)) return;
            long now = Stopwatch.GetTimestamp();
            long elapsedSeconds = LoadingElapsedSeconds(now);
            if (!force && elapsedSeconds == _lastLoadTelemetrySecond) return;
            long privateBytes;
            using (Process process = Process.GetCurrentProcess())
            {
                process.Refresh();
                privateBytes = process.PrivateMemorySize64;
            }
            string telemetry = FormatLoadingTelemetry(elapsedSeconds, privateBytes, _compactLayout);
            if (!String.Equals(telemetry, _lastLoadTelemetryText, StringComparison.Ordinal))
            {
                _lastLoadTelemetryText = telemetry;
                _fileLabel.Text = "Loading " + _loadingFileName + "…" + Environment.NewLine + telemetry;
                _loadingTelemetryUpdateCount++;
            }
            _lastLoadTelemetrySecond = elapsedSeconds;
        }

        private void ResetLoadingTelemetryState()
        {
            _loadStartedTimestamp = 0;
            _loadDecisionPauseStartedTimestamp = 0;
            _loadDecisionPausedTicks = 0;
            _lastLoadTelemetrySecond = -1;
            _lastLoadTelemetryText = null;
        }

        private void BeginLoadingDecisionPause()
        {
            if (_loadStartedTimestamp == 0 || _loadDecisionPauseStartedTimestamp != 0) return;
            _loadDecisionPauseStartedTimestamp = Stopwatch.GetTimestamp();
            UpdateLoadingFileTelemetry(true);
        }

        private void EndLoadingDecisionPause()
        {
            if (_loadDecisionPauseStartedTimestamp == 0) return;
            long now = Stopwatch.GetTimestamp();
            if (_loadStartedTimestamp != 0)
                _loadDecisionPausedTicks += Math.Max(0, now - _loadDecisionPauseStartedTimestamp);
            _loadDecisionPauseStartedTimestamp = 0;
            UpdateLoadingFileTelemetry(true);
        }

        private long LoadingElapsedSeconds(long now)
        {
            return ComputeLoadingElapsedSeconds(_loadStartedTimestamp, _loadDecisionPauseStartedTimestamp,
                _loadDecisionPausedTicks, now, Stopwatch.Frequency);
        }

        internal static long ComputeLoadingElapsedSeconds(long started, long pauseStarted,
            long pausedTicks, long now, long frequency)
        {
            if (started == 0 || frequency <= 0) return 0;
            long effectiveNow = pauseStarted == 0 ? now : pauseStarted;
            long elapsedTicks = Math.Max(0, effectiveNow - started - Math.Max(0, pausedTicks));
            return Math.Max(0, (long)(elapsedTicks / (double)frequency));
        }

        internal static string FormatLoadingTelemetry(long elapsedSeconds, long privateBytes, bool compact)
        {
            elapsedSeconds = Math.Max(0, elapsedSeconds);
            privateBytes = Math.Max(0, privateBytes);
            TimeSpan elapsed = TimeSpan.FromSeconds(elapsedSeconds);
            string time = elapsed.TotalHours >= 1
                ? String.Format(CultureInfo.CurrentCulture, "{0}:{1:00}:{2:00}", (long)elapsed.TotalHours, elapsed.Minutes, elapsed.Seconds)
                : String.Format(CultureInfo.CurrentCulture, "{0}:{1:00}", (long)elapsed.TotalMinutes, elapsed.Seconds);
            double mebibytes = privateBytes / 1048576.0;
            string memory = mebibytes >= 1024
                ? (mebibytes / 1024.0).ToString(mebibytes >= 10240 ? "N1" : "N2", CultureInfo.CurrentCulture) + " GiB"
                : mebibytes.ToString("N0", CultureInfo.CurrentCulture) + " MiB";
            return compact ? time + "  •  " + memory : time + " elapsed  •  " + memory + " committed";
        }

        private void SeekBy(long deltaMicroseconds)
        {
            PerformSeek(_selectedPositionMicroseconds + deltaMicroseconds);
        }

        private void ShowAnalysisClicked(object sender, EventArgs e)
        {
            if (_song == null) return;
            AnalysisConfiguration configuration = CurrentAnalysisConfiguration();
            DiagnosticsForm diagnostics = new DiagnosticsForm(_song);
            _analysisWindows.Add(diagnostics);
            diagnostics.FormClosed += delegate { _analysisWindows.Remove(diagnostics); };
            diagnostics.SeekRequested += delegate(object source, WorkloadSelectionEventArgs seek)
            {
                PerformAnalysisSeek(seek.TimeMicroseconds);
            };
            diagnostics.ChannelsRequested += delegate { ShowChannelMonitor(); };
            diagnostics.StartPosition = FormStartPosition.Manual;
            Rectangle working = Screen.FromControl(this).WorkingArea;
            Point proposed = new Point(Right + 12, Top);
            if (proposed.X + diagnostics.Width > working.Right)
                proposed.X = Math.Max(working.Left, Left + 36);
            proposed.Y = Math.Max(working.Top, Math.Min(working.Bottom - diagnostics.Height, proposed.Y));
            diagnostics.Location = proposed;
            diagnostics.Show();
            diagnostics.RequestAnalysis(configuration);
        }

        internal void ShowAnalysisForTesting()
        {
            ShowAnalysisClicked(this, EventArgs.Empty);
        }

        internal DiagnosticsForm[] AnalysisWindowsForTesting
        {
            get { return _analysisWindows.ToArray(); }
        }

        private void ShowChannelMonitor()
        {
            if (_song == null && (_channelMonitor == null || _channelMonitor.IsDisposed)) return;
            if (_channelMonitor != null && !_channelMonitor.IsDisposed)
            {
                if (_channelMonitor.WindowState == FormWindowState.Minimized)
                    _channelMonitor.WindowState = FormWindowState.Normal;
                _channelMonitor.Activate();
                return;
            }

            _engine.SetChannelMonitoring(true);
            ChannelMonitorForm monitor = new ChannelMonitorForm(_song == null ? null : Path.GetFileName(_song.FilePath));
            _channelMonitor = monitor;
            _sourceReadoutEventIndex = -1;
            monitor.OverrideRequested += delegate(object sender, ChannelOverrideRequestEventArgs request)
            {
                try
                {
                    _engine.SetChannelOverride(request.Channel, request.Attribute, request.Value);
                    monitor.UpdateSnapshot(_engine.GetChannelSnapshot());
                }
                catch (Exception ex)
                {
                    monitor.UpdateSnapshot(_engine.GetChannelSnapshot());
                    request.Error = ex;
                }
            };
            monitor.HistoricalChaseRequested += delegate(object sender, ChannelChaseRequestEventArgs request)
            {
                try
                {
                    _engine.ChaseLatestSourceChannelAttribute(request.Channel, request.Attribute, delegate(Exception error)
                    {
                        // Retire the logical request even if its originating
                        // monitor closed while the ordered send was pending.
                        request.Complete(error);
                        if (monitor.IsDisposed || !monitor.IsHandleCreated) return;
                        try
                        {
                            monitor.BeginInvoke((MethodInvoker)delegate
                            {
                                if (monitor.IsDisposed) return;
                                monitor.UpdateSnapshot(_engine.GetChannelSnapshot());
                            });
                        }
                        catch (InvalidOperationException) { }
                    });
                }
                catch (Exception ex) { request.Error = ex; request.Complete(ex); }
            };
            monitor.ChannelEnabledRequested += delegate(object sender, ChannelEnabledRequestEventArgs request)
            {
                try
                {
                    _engine.SetChannelEnabled(request.Channel, request.Enabled);
                    monitor.UpdateSnapshot(_engine.GetChannelSnapshot());
                }
                catch (Exception ex) { request.Error = ex; }
            };
            monitor.FormClosed += delegate
            {
                if (Object.ReferenceEquals(_channelMonitor, monitor))
                {
                    _channelMonitor = null;
                    _engine.SetChannelMonitoring(false);
                }
            };
            monitor.StartPosition = FormStartPosition.Manual;
            Rectangle working = Screen.FromControl(this).WorkingArea;
            Point proposed = new Point(Math.Max(working.Left, Left + 28), Math.Max(working.Top, Top + 28));
            proposed.X = Math.Min(working.Right - monitor.Width, proposed.X);
            proposed.Y = Math.Min(working.Bottom - monitor.Height, proposed.Y);
            monitor.Location = proposed;
            monitor.Show();
            if (_song == null) monitor.DetachForSongReplacement("No MIDI file is loaded.");
            else
            {
                UpdateChannelSourceReadouts(_engine.State == PlaybackState.Stopped
                    ? _selectedPositionMicroseconds : _engine.GetSnapshot().IntendedTimelineMicroseconds);
                monitor.UpdateSnapshot(_engine.GetChannelSnapshot());
            }
        }

        private void UpdateChannelSourceReadouts(long positionMicroseconds)
        {
            ChannelMonitorForm monitor = _channelMonitor;
            if (monitor == null || monitor.IsDisposed || _song == null) return;
            if (!_chaseMidiStateOnPlaySeek)
            {
                monitor.SetSourceReadouts(null);
                _sourceReadoutEventIndex = -1;
                return;
            }
            int exclusive = _song.EventStore.LowerBoundByTime(positionMicroseconds);
            if (Object.ReferenceEquals(_song, _sourceReadoutSong) && exclusive == _sourceReadoutEventIndex) return;
            monitor.SetSourceReadouts(_song.GetMidiStateChaseIndex().CreateAttributeReadouts(exclusive));
            _sourceReadoutSong = _song;
            _sourceReadoutEventIndex = exclusive;
        }

        private void CloseChannelMonitor()
        {
            ChannelMonitorForm monitor = _channelMonitor;
            _channelMonitor = null;
            if (monitor != null && !monitor.IsDisposed) monitor.Close();
            _engine.SetChannelMonitoring(false);
        }

        internal void ShowChannelMonitorForTesting()
        {
            ShowChannelMonitor();
        }

        internal ChannelMonitorForm ChannelMonitorForTesting { get { return _channelMonitor; } }

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
            DiagnosticsForm[] windows = _analysisWindows.ToArray();
            for (int i = 0; i < windows.Length; i++)
            {
                DiagnosticsForm window = windows[i];
                if (window == null || window.IsDisposed) continue;
                window.RequestAnalysis(configuration);
            }
        }

        private AnalysisConfiguration CurrentAnalysisConfiguration()
        {
            AnalysisConfiguration configuration = new AnalysisConfiguration();
            configuration.SimulateSlowdown = _simulateSlowdownCheck.Checked;
            configuration.ServiceDurationMode = _engine.ServiceDurationMode;
            configuration.ProcessingMicroseconds = _engine.ProcessingMicroseconds;
            configuration.MidiBitrate = _engine.MidiBitrate;
            configuration.EventsPerSecond = _engine.EventsPerSecond;
            configuration.QueueLengthLimitEnabled = _queueLimitCheck.Checked;
            configuration.QueueLengthLimit = Decimal.ToInt32(_queueLimitValue.Value);
            configuration.OverflowPolicy = (OverflowPolicy)Math.Max(0, _overflowPolicyCombo.SelectedIndex);
            configuration.PerNoteIntervalGateEnabled = _perNoteIntervalGateEnabled;
            configuration.ApplyQueueLimitWithoutSlowdown = _applyQueueLimitWithoutSlowdown;
            return configuration;
        }

        private void PerformSeek(long targetMicroseconds)
        {
            if (_song == null) return;
            targetMicroseconds = Math.Max(0, Math.Min(_song.DurationMicroseconds, targetMicroseconds));
            try
            {
                if (_engineSong == _song) _engine.Seek(targetMicroseconds);
            }
            catch (PlaybackWorkerTimeoutException ex)
            {
                _outputError = ex.Message;
                PlaybackSnapshot stopped = _engine.GetSnapshot();
                _selectedPositionMicroseconds = stopped.IntendedTimelineMicroseconds;
                RestartLiveMeasurementWindows();
                UpdateSeekDisplay();
                RefreshStatistics();
                UpdateTransportControls();
                MessageBox.Show(this, ex.Message, "Unable to seek safely", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            _selectedPositionMicroseconds = targetMicroseconds;
            ResetLiveMeasurements();
            UpdateSeekDisplay();
            RefreshStatistics();
            UpdateTransportControls();
        }

        private void PerformAnalysisSeek(long targetMicroseconds)
        {
            PlaybackState before = _engine.State;
            PerformSeek(targetMicroseconds);
            if (before == PlaybackState.Paused && _engine.State == PlaybackState.Paused)
            {
                _engine.Resume();
                RestartLiveMeasurementWindows();
                UpdateTransportControls();
            }
        }

        private void UpdateSeekDisplay()
        {
            long duration = _song == null ? 0 : _song.DurationMicroseconds;
            _timelineView.SetTimeline(_selectedPositionMicroseconds, duration);
        }

        private void UpdateFileInformation()
        {
            _fileInfoLabel.Text = FormatFileInformation(_song, _compactLayout);
            _toolTip.SetToolTip(_fileInfoLabel, _song == null ? String.Empty : FormatFileInformation(_song, false));
        }

        internal static string FormatFileInformation(MidiSong song, bool compact)
        {
            if (song == null) return String.Empty;
            string first = String.Format(CultureInfo.CurrentCulture, "{0:N0} events  •  {1} tracks", song.EventStore.Count, song.TrackCount);
            string second = String.Format(CultureInfo.CurrentCulture, "{0:N0} notes  •  {1}", song.NoteCount, FormatTime(song.DurationMicroseconds));
            return compact ? first + Environment.NewLine + second : first + "  •  " + second;
        }

        private void UpdateResponsiveLayout()
        {
            if (_rootLayout == null || _statisticsView == null) return;
            bool compact = ClientSize.Width < 640;
            if (_responsiveLayoutInitialized && compact == _compactLayout) return;
            _responsiveLayoutInitialized = true;
            if (compact && !_compactLayout) _lastDefaultHeight = Math.Max(MinimumSize.Height, Height);
            _compactLayout = compact;
            List<Control> suspended = new List<Control>();
            SuspendLayout();
            SuspendLayoutTree(_rootLayout, suspended);
            try
            {
                if (compact)
                {
                    if (_lastCompactHeight > 0)
                    {
                        MinimumSize = new Size(CalculateCompactMinimumWindowWidth(), _lastCompactHeight);
                        MaximumSize = new Size(10000, _lastCompactHeight);
                    }
                    else
                    {
                        // The first compact layout has not been measured yet.
                        MaximumSize = Size.Empty;
                        MinimumSize = new Size(CalculateCompactMinimumWindowWidth(), 100);
                    }
                    // After the first realized compact layout its content height
                    // is stable for this DPI/font.  Apply it while layout is
                    // suspended so the breakpoint needs one final layout pass,
                    // rather than one pass before and another after the height
                    // constraint changes.
                    if (_lastCompactHeight > 0 && Height != _lastCompactHeight)
                        Height = _lastCompactHeight;
                }
                else
                {
                    MaximumSize = Size.Empty;
                    MinimumSize = new Size(560, _lastDefaultRequiredHeight > 0 ? _lastDefaultRequiredHeight : 100);
                    int restoredHeight = Math.Max(_lastDefaultHeight, MinimumSize.Height);
                    if (Height < restoredHeight) Height = restoredHeight;
                }
                _rootLayout.Padding = compact ? new Padding(2) : new Padding(5);
                _loadingPanel.Width = compact ? 118 : 180;
                _loadingPanel.Margin = compact ? new Padding(2, 0, 1, 0) : new Padding(4, 0, 3, 0);
                _kdmApiCheck.Margin = compact ? new Padding(4, 3, 2, 3) : new Padding(10, 3, 3, 3);
                ConfigureFileHeaderAllocation(_loadingSong);
                _processingTable.ColumnStyles[0].Width = compact ? 51 : 48;
                _processingTable.ColumnStyles[1].Width = compact ? 49 : 52;
                _statisticsView.Compact = compact;
                _queueLimitCheck.Text = compact ? "Queue limit:" : "Queue length limit:";
                _serviceModeLabel.Text = compact ? "Rate:" : "Rate model:";
                _serviceValueLabel.Text = _engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate
                    ? (compact ? "Bitrate:" : "MIDI bitrate:")
                    : _engine.ServiceDurationMode == ServiceDurationMode.EventsPerSecond
                        ? "Events/sec:"
                        : (compact ? "Time/event:" : "Processing time per event:");
                _serviceModeCombo.Width = compact ? 170 : 210;
                _overflowPolicyCombo.Width = compact ? 132 : 235;
                _overflowCluster.Margin = compact ? new Padding(0) : new Padding(4, 0, 0, 0);
                _serviceCluster.Margin = compact ? new Padding(0) : new Padding(4, 0, 0, 0);
                _overflowLabel.Margin = compact ? new Padding(0, 5, 3, 2) : new Padding(0, 6, 4, 3);
                _queueLimitValue.Width = compact ? 77 : 100;
                _processingValue.Width = compact
                    ? (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate ? 89 : 77)
                    : (_engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate ? 110 : 100);
                _queueLimitCheck.Margin = compact ? new Padding(0, 4, 3, 2) : new Padding(0, 5, 4, 3);
                _queueLimitValue.Margin = compact ? new Padding(0, 1, 3, 1) : new Padding(0, 2, 4, 2);
                _simulateSlowdownCheck.Margin = compact ? new Padding(0, 0, 2, 0) : new Padding(0, 2, 3, 2);
                _eventsLabel.Margin = compact ? new Padding(0, 5, 1, 2) : new Padding(0, 6, 2, 3);
                _serviceModeLabel.Margin = compact ? new Padding(0, 5, 1, 2) : new Padding(0, 6, 4, 3);
                _serviceModeCombo.Margin = compact ? new Padding(0, 1, 0, 1) : new Padding(0, 2, 3, 2);
                _serviceValueLabel.Margin = compact ? new Padding(0, 5, 3, 2) : new Padding(0, 6, 4, 3);
                _processingValue.Margin = compact ? new Padding(0, 1, 3, 1) : new Padding(0, 2, 3, 2);
                _serviceUnitLabel.Margin = compact ? new Padding(0, 5, 2, 2) : new Padding(0, 6, 3, 3);
                _dinPresetButton.Margin = compact ? new Padding(1, 0, 1, 0) : new Padding(3, 0, 3, 0);
                _processingTable.Padding = compact ? new Padding(0) : new Padding(1);
                // The one-pixel compact inset keeps the native TrackBar paint
                // from touching the rate-model row above it.
                _rateCluster.MinimumSize = compact ? new Size(0, _serviceModeCombo.PreferredHeight + 4) : Size.Empty;
                _processingSlider.Margin = compact ? new Padding(0, 2, 0, 0) : new Padding(3);
                _processingSlider.AutoSize = false;
                _processingSlider.Height = compact ? 32 : 36;
                _dinPresetButton.Visible = !_compactLayout && _engine.ServiceDurationMode == ServiceDurationMode.MidiBitrate;
                UpdateCompactUnitVisibility();
                _timelineView.MinimumSize = new Size(300, compact ? 20 : 38);
                _timelineView.Height = compact ? 20 : 38;
                _timelineView.Margin = compact ? new Padding(1, 0, 1, 0) : new Padding(3);
                _playbackButtonLayout.Margin = compact ? new Padding(1, 0, 1, 0) : new Padding(3);
                Button[] playbackButtons = new Button[] { _playButton, _stopButton, _seekBack5Button, _seekForward5Button, _analysisButton, _resetStatsButton };
                for (int i = 0; i < playbackButtons.Length; i++)
                    playbackButtons[i].Margin = compact ? new Padding(1, 1, 1, 1) : new Padding(3);
                _playButton.AutoSize = _stopButton.AutoSize = _seekBack5Button.AutoSize = _seekForward5Button.AutoSize =
                    _analysisButton.AutoSize = _resetStatsButton.AutoSize = !compact;
                if (compact)
                {
                    _playButton.Width = 54; _stopButton.Width = 54; _seekBack5Button.Width = 59;
                    _seekForward5Button.Width = 59; _analysisButton.Width = 76; _resetStatsButton.Width = 76;
                }
                Control.ControlCollection children = _rootLayout.Controls;
                for (int i = 0; i < children.Count; i++)
                {
                    GroupBox group = children[i] as GroupBox;
                    if (group == null) continue;
                    group.Padding = compact ? new Padding(3, 3, 3, 2) : new Padding(6, 5, 6, 4);
                    group.Margin = compact ? new Padding(1, 0, 1, 1) : new Padding(2, 1, 2, 3);
                }
                if (compact)
                {
                    // GroupBox reserves its caption independently.  These
                    // smaller content insets remove blank space without moving
                    // either custom-painted surface into the caption/border.
                    _playbackGroup.Padding = new Padding(3, 0, 3, 1);
                    _statisticsGroup.Padding = new Padding(3, 0, 3, 0);
                }
                if (_loadingSong) UpdateLoadingFileTelemetry(true);
                else UpdateFileInformation();
            }
            finally
            {
                for (int i = suspended.Count - 1; i >= 0; i--) suspended[i].ResumeLayout(false);
                ResumeLayout(false);
                // A width crossing can first be laid out at the old compact
                // height because MaximumSize is removed during that same
                // resize message.  Complete one layout and paint of the
                // custom surface after every affected container is resumed;
                // otherwise the lower rows retain the old clipped backing
                // pixels until a statistic happens to change.
                _rootLayout.PerformLayout();
                PerformLayout();
                ApplyMeasuredWindowConstraints(true);
                // Queue one double-buffered repaint after the layout settles.  A
                // synchronous Refresh here made every responsive breakpoint
                // crossing wait for an immediate paint, even though the next
                // message-loop turn will paint the same final state.
                _statisticsView.Invalidate();
            }
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            // The non-client metrics and AutoSize descendants are authoritative
            // only after a real handle has completed its first layout.
            _statisticsGroup.PerformLayout();
            _rootLayout.PerformLayout();
            PerformLayout();
            ApplyMeasuredWindowConstraints(false);
            if (!_compactLayout)
            {
                Height = RealizedRequiredWindowHeight;
                _lastDefaultHeight = Height;
            }
        }

        private void ApplyMeasuredWindowConstraints(bool restoreDefaultHeight)
        {
            int requiredHeight = CalculateRequiredWindowHeight();
            if (_compactLayout)
            {
                _lastCompactHeight = requiredHeight;
                Size compactMinimum = new Size(CalculateCompactMinimumWindowWidth(), requiredHeight);
                Size compactMaximum = new Size(10000, requiredHeight);
                if (MinimumSize != compactMinimum) MinimumSize = compactMinimum;
                if (MaximumSize != compactMaximum) MaximumSize = compactMaximum;
                if (Height != requiredHeight) Height = requiredHeight;
            }
            else
            {
                _lastDefaultRequiredHeight = requiredHeight;
                if (MaximumSize != Size.Empty) MaximumSize = Size.Empty;
                Size defaultMinimum = new Size(560, requiredHeight);
                if (MinimumSize != defaultMinimum) MinimumSize = defaultMinimum;
                if (restoreDefaultHeight && Height < Math.Max(requiredHeight, _lastDefaultHeight))
                    Height = Math.Max(requiredHeight, _lastDefaultHeight);
            }
        }

        private int CalculateRequiredWindowHeight()
        {
            int contentBottom = _rootLayout.Padding.Top;
            for (int i = 0; i < _rootLayout.Controls.Count; i++)
            {
                Control child = _rootLayout.Controls[i];
                if (child.Visible && child.Bottom > contentBottom) contentBottom = child.Bottom;
            }
            int requiredClientHeight = contentBottom + _rootLayout.Padding.Bottom;
            int nonClientHeight = Math.Max(0, Height - ClientSize.Height);
            return Math.Max(1, requiredClientHeight + nonClientHeight);
        }

        private int CalculateCompactMinimumWindowWidth()
        {
            float dpi = 96F;
            if (IsHandleCreated)
            {
                using (Graphics graphics = CreateGraphics()) dpi = graphics.DpiX;
            }
            int requiredClientWidth = (int)Math.Ceiling(416F * dpi / 96F);
            int nonClientWidth = Math.Max(0, Width - ClientSize.Width);
            return requiredClientWidth + nonClientWidth;
        }

        private void UpdateCompactUnitVisibility()
        {
            if (_eventsLabel == null || _processingTable == null || _queueCluster == null) return;
            if (!_compactLayout)
            {
                _eventsLabel.Visible = true;
                return;
            }
            int available = _processingTable.GetColumnWidths().Length == 0
                ? _processingTable.ClientSize.Width / 2
                : _processingTable.GetColumnWidths()[0];
            int required = 0;
            for (int i = 0; i < _queueCluster.Controls.Count; i++)
            {
                Control child = _queueCluster.Controls[i];
                Size preferred = child.GetPreferredSize(Size.Empty);
                int width = Object.ReferenceEquals(child, _queueLimitValue) ? child.Width : preferred.Width;
                required += width + child.Margin.Horizontal;
            }
            _eventsLabel.Visible = available >= required;
        }

        internal int RealizedRequiredWindowHeight { get { return CalculateRequiredWindowHeight(); } }

        private static void SuspendLayoutTree(Control control, List<Control> suspended)
        {
            control.SuspendLayout();
            suspended.Add(control);
            for (int i = 0; i < control.Controls.Count; i++)
            {
                Control child = control.Controls[i];
                // Suspend only actual layout containers. Suspending every
                // descendant also freezes UpDownBase's private edit/spinner
                // children and makes each responsive transition needlessly
                // expensive on a realized form.
                if (child is TableLayoutPanel || child is FlowLayoutPanel || child is GroupBox)
                    SuspendLayoutTree(child, suspended);
            }
        }

        private void ResetLiveMeasurements()
        {
            _effectiveSpeed.Reset();
            _outputRate.Reset();
            _lastDroppedEvents = 0;
            _overflowVisibleUntilMicroseconds = 0;
        }

        private void RestartLiveMeasurementWindows()
        {
            _effectiveSpeed.Reset();
            _outputRate.RestartWindow();
            _lastDroppedEvents = 0;
            _overflowVisibleUntilMicroseconds = 0;
        }

        internal static string FormatObservedMaximumRate(double? rate, bool compact)
        {
            if (!rate.HasValue || Double.IsNaN(rate.Value) || Double.IsInfinity(rate.Value)) return "—";
            return rate.Value.ToString(compact ? "N0" : "N1", CultureInfo.CurrentCulture) +
                (compact ? " events/s" : " events/sec");
        }

        internal static string FormatMaximumRate(PlaybackSnapshot snapshot, double? observedRate, bool compact)
        {
            if (snapshot == null) return "—";
            if (snapshot.ProcessingMode == ProcessingMode.PerNoteIntervalGate)
                return FormatPerNoteFrameRate(snapshot.ProcessingMicroseconds, compact);
            bool immediate = !snapshot.SimulateSlowdown && !snapshot.VirtualQueueActive ||
                (snapshot.ServiceDurationMode == ServiceDurationMode.ProcessingTime && snapshot.ProcessingMicroseconds == 0) ||
                (snapshot.ServiceDurationMode == ServiceDurationMode.EventsPerSecond && snapshot.EventsPerSecond == 0);
            if (immediate) return FormatObservedMaximumRate(observedRate, compact);
            if (snapshot.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                return snapshot.MidiBitrate.ToString("N0", CultureInfo.CurrentCulture) + " bit/s";
            if (snapshot.ServiceDurationMode == ServiceDurationMode.EventsPerSecond)
                return snapshot.EventsPerSecond.ToString("N0", CultureInfo.CurrentCulture) +
                    (compact ? " events/s" : " events/sec");
            return (1000000.0 / Math.Max(1, snapshot.ProcessingMicroseconds)).ToString(compact ? "N0" : "N1",
                CultureInfo.CurrentCulture) + (compact ? " events/s" : " events/sec");
        }

        internal static string FormatPerNoteFrameRate(long intervalMicroseconds, bool compact)
        {
            return (1000000.0 / Math.Max(1, intervalMicroseconds)).ToString(
                compact ? "N0" : "N1", CultureInfo.CurrentCulture) +
                (compact ? " frames/s" : " frames/sec");
        }

        internal static string FormatEventCounts(PlaybackSnapshot snapshot, bool compact)
        {
            if (snapshot == null) return "—";
            string value = snapshot.ProcessedEvents.ToString("N0", CultureInfo.CurrentCulture) + " / " +
                snapshot.DroppedEvents.ToString("N0", CultureInfo.CurrentCulture);
            if (snapshot.ProcessingMode == ProcessingMode.PerNoteIntervalGate && snapshot.GateFilteredEvents != 0)
                value += compact
                    ? " (+" + snapshot.GateFilteredEvents.ToString("N0", CultureInfo.CurrentCulture) + " gate)"
                    : " (+" + snapshot.GateFilteredEvents.ToString("N0", CultureInfo.CurrentCulture) + " gate-filtered)";
            return value;
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
                ProductIcon.Apply(dialog);
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

        private void EngineChannelControlFailed(object sender, ChannelControlErrorEventArgs e)
        {
            if (!IsHandleCreated) return;
            BeginInvoke((MethodInvoker)delegate
            {
                if (_channelMonitor != null && !_channelMonitor.IsDisposed)
                {
                    _channelMonitor.UpdateSnapshot(_engine.GetChannelSnapshot());
                    _channelMonitor.ShowControlError(e.Channel, e.Attribute, e.Error.Message);
                }
            });
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            CancelMidiLoad();
            _uiTimer.Stop();
            _analysisRefreshTimer.Stop();
            DiagnosticsForm[] analysisWindows = _analysisWindows.ToArray();
            for (int i = 0; i < analysisWindows.Length; i++)
                if (analysisWindows[i] != null && !analysisWindows[i].IsDisposed) analysisWindows[i].Close();
            CloseChannelMonitor();
            try { _engine.Dispose(); }
            catch (PlaybackWorkerTimeoutException ex)
            {
                // The in-process driver call still owns the scheduler/output.
                // Keep the form alive rather than disposing that backend under
                // the blocked worker.  Closing can be retried after it returns.
                e.Cancel = true;
                _uiTimer.Start();
                MessageBox.Show(this, ex.Message, "MIDI output is still stopping", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                base.OnFormClosing(e);
                return;
            }
            _output.Dispose();
            _kdmApiOutput.Dispose();
            _nullOutput.Dispose();
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
