using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal delegate WorkloadAnalysis WorkloadAnalysisRunner(MidiSong song, long bucketMicroseconds,
        AnalysisConfiguration configuration, CancellationToken cancellationToken,
        Action<WorkloadAnalysisProgress> progress, Action<WorkloadBaseAnalysis> workloadReady);

    internal sealed class DiagnosticsForm : Form
    {
        private readonly RichTextBox _summary;
        private readonly SplitContainer _split;
        private readonly WorkloadGraph _graph;
        private readonly TextBox _inspection;
        private readonly Button _seekButton;
        private readonly ComboBox _followCombo;
        private readonly CheckBox _playbackStatisticsCheck;
        private readonly Font _summaryHeadingFont;
        private readonly ComboBox _resolutionCombo;
        private readonly ToolTip _toolTip;
        private readonly FlowLayoutPanel _headerLayout;
        private readonly Label _calculationStatus;
        private readonly ProgressBar _calculationProgress;
        private readonly Button _cancelCalculationButton;
        private readonly System.Windows.Forms.Timer _calculationDelayTimer;
        private readonly System.Windows.Forms.Timer _calculationProgressTimer;
        private readonly System.Windows.Forms.Timer _resolutionDebounceTimer;
        private readonly Dictionary<string, WorkloadAnalysis> _analysisCache = new Dictionary<string, WorkloadAnalysis>();
        private readonly Queue<string> _analysisCacheOrder = new Queue<string>();
        private static readonly ConditionalWeakTable<MidiSong, SharedSongAnalysisCache> SharedAnalysisCaches =
            new ConditionalWeakTable<MidiSong, SharedSongAnalysisCache>();
        private CancellationTokenSource _analysisCancellation;
        private AnalysisConfiguration _pendingConfiguration;
        private int _analysisGeneration;
        private WorkloadAnalysis _analysis;
        private readonly WorkloadAnalysisRunner _analyzer;
        private readonly bool _usesDefaultAnalyzer;
        private volatile AnalysisProgressUpdate _latestAnalysisProgress;
        private bool _analysisBusy;
        private bool _busyIndicatorVisible;
        private int _displayedProgressPermille;
        private bool _updatingAnalysis;
        private bool _hasCompletedAnalysis;
        private int _previewPublicationCount;
        private bool _suppressResolutionSelection;
        private ResolutionSelectionMode _resolutionMode = ResolutionSelectionMode.Auto;
        private string _autoResolutionLabel = "Auto";
        private string _lastValidResolution = "Auto";
        private string _customResolutionLabel;
        private long _customResolutionMicroseconds;

        private enum ResolutionSelectionMode
        {
            Auto,
            Fixed,
            Custom
        }

        private sealed class AnalysisProgressUpdate
        {
            internal int Generation;
            internal WorkloadAnalysisProgress Progress;
        }

        private sealed class SharedSongAnalysisCache
        {
            internal readonly Dictionary<string, WeakReference> Results = new Dictionary<string, WeakReference>();
            internal readonly Queue<string> Order = new Queue<string>();
        }

        internal event EventHandler<WorkloadSelectionEventArgs> SeekRequested;
        internal event EventHandler ChannelsRequested;

        internal MidiSong SourceSong { get; private set; }

        public DiagnosticsForm(MidiSong song)
            : this(song, null)
        {
        }

        public DiagnosticsForm(MidiSong song, WorkloadAnalysis analysis)
            : this(song, analysis,
                (WorkloadAnalysisRunner)null)
        {
        }

        internal DiagnosticsForm(MidiSong song, WorkloadAnalysis analysis,
            Func<MidiSong, long, AnalysisConfiguration, CancellationToken, WorkloadAnalysis> analyzer)
            : this(song, analysis, analyzer == null ? null :
                new WorkloadAnalysisRunner(
                    delegate(MidiSong source, long bucket, AnalysisConfiguration configuration, CancellationToken token,
                        Action<WorkloadAnalysisProgress> progress, Action<WorkloadBaseAnalysis> workloadReady)
                    { return analyzer(source, bucket, configuration, token); }))
        {
        }

        internal DiagnosticsForm(MidiSong song, WorkloadAnalysis analysis,
            Func<MidiSong, long, AnalysisConfiguration, CancellationToken, Action<WorkloadAnalysisProgress>, WorkloadAnalysis> analyzer)
            : this(song, analysis, analyzer == null ? null :
                new WorkloadAnalysisRunner(
                    delegate(MidiSong source, long bucket, AnalysisConfiguration configuration, CancellationToken token,
                        Action<WorkloadAnalysisProgress> progress, Action<WorkloadBaseAnalysis> workloadReady)
                    { return analyzer(source, bucket, configuration, token, progress); }))
        {
        }

        internal DiagnosticsForm(MidiSong song, WorkloadAnalysis analysis, WorkloadAnalysisRunner analyzer)
        {
            ProductIcon.Apply(this);
            _usesDefaultAnalyzer = analyzer == null;
            _analyzer = analyzer == null
                ? new WorkloadAnalysisRunner(
                    delegate(MidiSong source, long bucket, AnalysisConfiguration configuration, CancellationToken token,
                        Action<WorkloadAnalysisProgress> progress, Action<WorkloadBaseAnalysis> workloadReady)
                    { return WorkloadAnalyzer.Analyze(source, bucket, configuration, token, progress, workloadReady); })
                : analyzer;
            SourceSong = song;
            Text = "MIDIBottleneck Player — Analysis — " + System.IO.Path.GetFileName(song.FilePath);
            StartPosition = FormStartPosition.Manual;
            ClientSize = new Size(1080, 660);
            MinimumSize = new Size(820, 520);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            _summaryHeadingFont = new Font("Consolas", 9F, FontStyle.Bold, GraphicsUnit.Point);

            _split = new SplitContainer();
            SplitContainer split = _split;
            split.Dock = DockStyle.Fill;
            split.FixedPanel = FixedPanel.Panel1;
            split.SplitterDistance = 390;
            split.SplitterWidth = 6;
            split.BackColor = SystemColors.ControlDark;
            split.Margin = new Padding(0, 4, 0, 4);
            split.Cursor = Cursors.Default;
            split.MouseMove += delegate(object sender, MouseEventArgs e)
            {
                split.Cursor = split.SplitterRectangle.Contains(e.Location) ? Cursors.VSplit : Cursors.Default;
            };
            split.MouseLeave += delegate { split.Cursor = Cursors.Default; };
            split.Paint += delegate(object sender, PaintEventArgs e)
            {
                Rectangle divider = split.SplitterRectangle;
                if (divider.Width <= 0 || divider.Height <= 0) return;
                using (Pen light = new Pen(SystemColors.ControlLightLight))
                using (Pen dark = new Pen(SystemColors.ControlDarkDark))
                {
                    int center = divider.Left + divider.Width / 2;
                    int top = divider.Top + Math.Max(4, (divider.Height - 24) / 2);
                    for (int y = top; y < top + 24; y += 6)
                    {
                        e.Graphics.DrawLine(dark, center - 1, y, center - 1, y + 1);
                        e.Graphics.DrawLine(light, center, y + 1, center, y + 2);
                    }
                }
            };
            split.SplitterMoved += delegate { split.Invalidate(split.SplitterRectangle); };
            split.Panel1.BackColor = SystemColors.Control;
            split.Panel2.BackColor = SystemColors.Control;
            split.Panel1.Padding = new Padding(0);
            split.Panel2.Padding = new Padding(0, 0, 2, 0);

            _summary = new RichTextBox();
            _summary.Dock = DockStyle.Fill;
            _summary.ReadOnly = true;
            _summary.ScrollBars = RichTextBoxScrollBars.Vertical;
            _summary.WordWrap = true;
            _summary.BorderStyle = BorderStyle.FixedSingle;
            _summary.BackColor = SystemColors.Window;
            _summary.Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point);
            split.Panel1.Controls.Add(_summary);

            TableLayoutPanel rootLayout = new TableLayoutPanel();
            rootLayout.Dock = DockStyle.Fill;
            rootLayout.Margin = new Padding(0);
            // SplitContainer contributes a two-pixel realized border. Pair it
            // with two DIP root insets so report/graph edges land at four.
            rootLayout.Padding = new Padding(2, 3, 2, 2);
            rootLayout.ColumnCount = 1;
            rootLayout.RowCount = 2;
            rootLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            rootLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            rootLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

            TableLayoutPanel graphLayout = new TableLayoutPanel();
            graphLayout.Dock = DockStyle.Fill;
            graphLayout.Margin = new Padding(0);
            graphLayout.Padding = new Padding(0);
            graphLayout.ColumnCount = 2;
            graphLayout.RowCount = 2;
            graphLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            graphLayout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            graphLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            graphLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 92));

            _headerLayout = new FlowLayoutPanel();
            _headerLayout.AutoSize = true;
            _headerLayout.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            _headerLayout.Dock = DockStyle.Fill;
            _headerLayout.WrapContents = true;
            _headerLayout.Margin = new Padding(0);
            rootLayout.Controls.Add(_headerLayout, 0, 0);

            Button resetZoom = new Button();
            resetZoom.Text = "Reset zoom";
            resetZoom.AutoSize = true;
            resetZoom.Margin = new Padding(2, 3, 6, 3);
            resetZoom.Click += delegate { _graph.ResetZoom(); };
            Label followLabel = new Label();
            followLabel.Text = "Follow:";
            followLabel.AutoSize = true;
            followLabel.Anchor = AnchorStyles.Left;
            followLabel.Margin = new Padding(3, 8, 3, 3);
            _followCombo = new ComboBox();
            _followCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _followCombo.Items.Add("Off");
            _followCombo.Items.Add("Playback timeline");
            _followCombo.Items.Add("MIDI output");
            _followCombo.SelectedIndex = 0;
            _followCombo.Width = 150;
            _followCombo.SelectedIndexChanged += delegate { _graph.FollowTarget = (AnalysisFollowTarget)_followCombo.SelectedIndex; };
            _followCombo.Margin = new Padding(0, 4, 7, 3);
            Label resolutionLabel = new Label();
            resolutionLabel.Text = "Resolution:";
            resolutionLabel.AutoSize = true;
            resolutionLabel.Margin = new Padding(3, 8, 3, 3);
            _resolutionCombo = new ComboBox();
            _toolTip = new ToolTip();
            _resolutionCombo.DropDownStyle = ComboBoxStyle.DropDownList;
            _resolutionCombo.Items.AddRange(new object[] { _autoResolutionLabel, "10 ms", "25 ms", "50 ms", "100 ms", "250 ms", "500 ms", "1 s", "Custom…" });
            _resolutionCombo.SelectedIndex = 0;
            _resolutionCombo.Width = 145;
            _resolutionCombo.Margin = new Padding(0, 4, 7, 3);
            _resolutionCombo.SelectedIndexChanged += ResolutionSelectionChanged;
            _playbackStatisticsCheck = new CheckBox();
            _playbackStatisticsCheck.Text = "Playback statistics";
            _playbackStatisticsCheck.AutoSize = true;
            _playbackStatisticsCheck.Margin = new Padding(3, 7, 8, 3);
            _playbackStatisticsCheck.CheckedChanged += delegate { _graph.PlaybackStatisticsVisible = _playbackStatisticsCheck.Checked; };
            Button channelsButton = new Button();
            channelsButton.Text = "Channels…";
            channelsButton.AutoSize = true;
            channelsButton.Margin = new Padding(3, 3, 8, 3);
            channelsButton.Click += delegate
            {
                EventHandler handler = ChannelsRequested;
                if (handler != null) handler(this, EventArgs.Empty);
            };
            _calculationStatus = new Label();
            _calculationStatus.Text = "Recalculating analysis…";
            _calculationStatus.AutoSize = true;
            _calculationStatus.ForeColor = Color.DimGray;
            _calculationStatus.Margin = new Padding(3, 8, 3, 3);
            _calculationStatus.Visible = false;
            _calculationProgress = new ProgressBar();
            _calculationProgress.Style = ProgressBarStyle.Continuous;
            _calculationProgress.Maximum = 1000;
            _calculationProgress.Width = 105;
            _calculationProgress.Height = 14;
            _calculationProgress.Margin = new Padding(3, 8, 3, 3);
            _calculationProgress.Visible = false;
            _cancelCalculationButton = new Button();
            _cancelCalculationButton.Text = "Cancel";
            _cancelCalculationButton.AutoSize = true;
            _cancelCalculationButton.Visible = false;
            _cancelCalculationButton.Click += delegate { CancelAnalysisCalculation(); };
            _cancelCalculationButton.Margin = new Padding(3);
            _headerLayout.Controls.Add(resetZoom);
            _headerLayout.Controls.Add(followLabel);
            _headerLayout.Controls.Add(_followCombo);
            _headerLayout.Controls.Add(resolutionLabel);
            _headerLayout.Controls.Add(_resolutionCombo);
            _headerLayout.Controls.Add(_playbackStatisticsCheck);
            _headerLayout.Controls.Add(channelsButton);
            _headerLayout.Controls.Add(_calculationStatus);
            _headerLayout.Controls.Add(_calculationProgress);
            _headerLayout.Controls.Add(_cancelCalculationButton);

            _graph = new WorkloadGraph();
            _graph.Dock = DockStyle.Fill;
            _graph.Margin = new Padding(0);
            _graph.InspectionChanged += GraphInspectionChanged;
            _graph.SeekRequested += GraphSeekRequested;
            _graph.ViewportChanged += delegate { ScheduleResolutionRefresh(); };
            graphLayout.Controls.Add(_graph, 0, 0);
            graphLayout.SetColumnSpan(_graph, 2);

            _inspection = new TextBox();
            _inspection.Dock = DockStyle.Fill;
            _inspection.Multiline = true;
            _inspection.ReadOnly = true;
            _inspection.BackColor = SystemColors.Window;
            _inspection.Font = new Font("Consolas", 8.5F, FontStyle.Regular, GraphicsUnit.Point);
            _inspection.Text = "Hover over the graph to inspect a time. Click to pin; double-click to seek.";
            _inspection.Margin = new Padding(0);
            graphLayout.Controls.Add(_inspection, 0, 1);

            _seekButton = new Button();
            _seekButton.Text = "Seek to pin";
            _seekButton.AutoSize = true;
            _seekButton.Enabled = false;
            _seekButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            _seekButton.Margin = new Padding(3, 3, 0, 0);
            _seekButton.Click += delegate
            {
                if (_graph.PinnedTimeMicroseconds.HasValue)
                    RaiseSeekRequested(_graph.PinnedTimeMicroseconds.Value);
            };
            graphLayout.Controls.Add(_seekButton, 1, 1);
            split.Panel2.Controls.Add(graphLayout);
            rootLayout.Controls.Add(split, 0, 1);
            Controls.Add(rootLayout);
            _calculationDelayTimer = new System.Windows.Forms.Timer();
            _calculationDelayTimer.Interval = 1000;
            _calculationDelayTimer.Tick += delegate
            {
                _calculationDelayTimer.Stop();
                if (_analysisBusy) ShowBusyIndicator();
            };
            _calculationProgressTimer = new System.Windows.Forms.Timer();
            _calculationProgressTimer.Interval = 100;
            _calculationProgressTimer.Tick += delegate { RefreshAnalysisProgress(); };
            _resolutionDebounceTimer = new System.Windows.Forms.Timer();
            _resolutionDebounceTimer.Interval = 180;
            _resolutionDebounceTimer.Tick += delegate
            {
                _resolutionDebounceTimer.Stop();
                if (_pendingConfiguration != null) RequestAnalysis(_pendingConfiguration);
            };
            if (analysis != null) UpdateAnalysis(analysis);
            else
            {
                _summary.Text = "Analysis will appear here when calculation completes.";
                _graph.EmptyMessage = "Calculating workload analysis…";
            }
            PerformLayout();
            split.Panel1MinSize = 330;
            split.Panel2MinSize = 360;
            split.SplitterDistance = 390;
            Shown += delegate
            {
                _summary.SelectionStart = 0;
                _summary.SelectionLength = 0;
            };
        }

        internal void UpdateAnalysis(WorkloadAnalysis analysis)
        {
            UpdateAnalysisDisplay(analysis, true);
        }

        private void UpdateAnalysisDisplay(WorkloadAnalysis analysis, bool completed)
        {
            if (analysis == null) throw new ArgumentNullException("analysis");
            _updatingAnalysis = true;
            try
            {
            _analysis = analysis;
            _hasCompletedAnalysis = completed;
            if (_pendingConfiguration == null) _pendingConfiguration = CloneConfiguration(analysis.Configuration);
            int selectionStart = _summary == null ? 0 : _summary.SelectionStart;
            if (_summary != null)
            {
                ApplySummary(BuildSummary(SourceSong, analysis));
                _summary.SelectionStart = Math.Min(selectionStart, _summary.TextLength);
                _summary.SelectionLength = 0;
            }
            if (_graph != null) { _graph.EmptyMessage = String.Empty; _graph.Analysis = analysis; }
            if (_resolutionMode == ResolutionSelectionMode.Auto)
                UpdateAutoResolutionLabel(analysis.BucketMicroseconds);
            if (_graph != null && _graph.InspectedTimeMicroseconds.HasValue)
                UpdateInspection(_graph.InspectedTimeMicroseconds.Value, _graph.PinnedTimeMicroseconds.HasValue);
            }
            finally { _updatingAnalysis = false; }
        }

        internal void UpdatePlaybackSnapshot(PlaybackSnapshot snapshot)
        {
            UpdatePlaybackSnapshot(snapshot, null);
        }

        internal void UpdatePlaybackSnapshot(PlaybackSnapshot snapshot, PlaybackOverlayData overlay)
        {
            if (snapshot == null || _graph == null) return;
            _graph.SetPlaybackPositions(snapshot.IntendedTimelineMicroseconds, snapshot.LastDispatchedTimelineMicroseconds);
            _graph.SetPlaybackOverlay(snapshot.State, overlay);
        }

        internal WorkloadGraph Graph { get { return _graph; } }
        internal SplitContainer AnalysisSplit { get { return _split; } }
        internal Control AnalysisHeader { get { return _headerLayout; } }
        internal Control AnalysisSummary { get { return _summary; } }
        internal Control AnalysisSeekButton { get { return _seekButton; } }
        internal bool SummaryWordWrap { get { return _summary.WordWrap; } }
        internal RichTextBoxScrollBars SummaryScrollBars { get { return _summary.ScrollBars; } }
        internal bool SummaryEnabled { get { return _summary.Enabled; } }
        internal string SummaryText { get { return _summary.Text; } }
        internal bool SeekToPinEnabled { get { return _seekButton.Enabled; } }
        internal bool CalculationStatusVisible { get { return _calculationStatus.Visible; } }
        internal bool CalculationProgressVisible { get { return _calculationProgress.Visible; } }
        internal int CalculationProgressPermille { get { return _calculationProgress.Value; } }
        internal string CalculationStatusText { get { return _calculationStatus.Text; } }
        internal bool AnalysisBusy { get { return _analysisBusy; } }
        internal bool CalculationPending { get { return _analysisCancellation != null; } }
        internal long ActiveResolutionMicroseconds { get { return _analysis == null ? 0 : _analysis.BucketMicroseconds; } }
        internal string ResolutionSelectionText { get { return Convert.ToString(_resolutionCombo.SelectedItem, CultureInfo.CurrentCulture); } }
        internal bool AutomaticResolutionSelected { get { return _resolutionMode == ResolutionSelectionMode.Auto; } }
        internal WorkloadAnalysis CurrentAnalysis { get { return _analysis; } }
        internal bool HasCompletedAnalysis { get { return _hasCompletedAnalysis; } }
        internal int PreviewPublicationCount { get { return _previewPublicationCount; } }
        internal bool HasAttachedSong { get { return SourceSong != null; } }
        internal int HeaderRowCount
        {
            get
            {
                List<Rectangle> bounds = new List<Rectangle>();
                for (int index = 0; index < _headerLayout.Controls.Count; index++)
                {
                    Control control = _headerLayout.Controls[index];
                    if (control.Visible) bounds.Add(control.Bounds);
                }
                bounds.Sort(delegate(Rectangle left, Rectangle right) { return left.Top.CompareTo(right.Top); });
                int rows = 0;
                int rowBottom = Int32.MinValue;
                for (int index = 0; index < bounds.Count; index++)
                {
                    if (bounds[index].Top >= rowBottom)
                    {
                        rows++;
                        rowBottom = bounds[index].Bottom;
                    }
                    else if (bounds[index].Bottom > rowBottom) rowBottom = bounds[index].Bottom;
                }
                return rows;
            }
        }
        internal bool HeaderControlsFit
        {
            get
            {
                for (int index = 0; index < _headerLayout.Controls.Count; index++)
                {
                    Control control = _headerLayout.Controls[index];
                    if (!control.Visible) continue;
                    if (control.Left < 0 || control.Top < 0 || control.Right > _headerLayout.ClientSize.Width ||
                        control.Bottom > _headerLayout.ClientSize.Height) return false;
                }
                return true;
            }
        }

        internal void RequestAnalysis(AnalysisConfiguration configuration)
        {
            if (IsDisposed || SourceSong == null) return;
            _resolutionDebounceTimer.Stop();
            _pendingConfiguration = CloneConfiguration(configuration);
            long resolution = SelectedResolutionMicroseconds();
            string cacheKey = AnalysisCacheKey(_pendingConfiguration, resolution);
            WorkloadAnalysis cached;
            if (_analysisCache.TryGetValue(cacheKey, out cached))
            {
                CancelCurrentAnalysisWork();
                UpdateAnalysis(cached.WithConfiguration(CloneConfiguration(_pendingConfiguration)));
                EndBusyPeriod();
                return;
            }
            if (_usesDefaultAnalyzer && TryGetSharedAnalysis(SourceSong, cacheKey, out cached))
            {
                CancelCurrentAnalysisWork();
                RememberLocalAnalysis(cacheKey, cached);
                UpdateAnalysis(cached.WithConfiguration(CloneConfiguration(_pendingConfiguration)));
                EndBusyPeriod();
                return;
            }

            CancellationTokenSource previous = _analysisCancellation;
            _analysisCancellation = null;
            if (previous != null)
            {
                previous.Cancel();
                previous.Dispose();
            }
            CancellationTokenSource cancellation = new CancellationTokenSource();
            _analysisCancellation = cancellation;
            int generation = ++_analysisGeneration;
            BeginBusyPeriod();
            MidiSong song = SourceSong;
            AnalysisConfiguration requested = CloneConfiguration(_pendingConfiguration);
            bool allowPreview = !_hasCompletedAnalysis;

            Task.Factory.StartNew(delegate
            {
                return _analyzer(song, resolution, requested, cancellation.Token, delegate(WorkloadAnalysisProgress value)
                {
                    _latestAnalysisProgress = new AnalysisProgressUpdate { Generation = generation, Progress = value };
                }, allowPreview ? (Action<WorkloadBaseAnalysis>)delegate(WorkloadBaseAnalysis workload)
                {
                    WorkloadAnalysis preview = workload.CreatePresentationAnalysis(requested, cancellation.Token);
                    PublishWorkloadPreview(generation, cancellation, song, preview);
                } : null);
            }, cancellation.Token, TaskCreationOptions.LongRunning, TaskScheduler.Default).ContinueWith(delegate(Task<WorkloadAnalysis> task)
            {
                if (IsDisposed || !IsHandleCreated) return;
                try
                {
                    BeginInvoke((MethodInvoker)delegate
                    {
                        if (IsDisposed || generation != _analysisGeneration || cancellation != _analysisCancellation) return;
                        _analysisCancellation = null;
                        cancellation.Dispose();
                        if (task.IsCanceled)
                        {
                            if (!_resolutionDebounceTimer.Enabled) EndBusyPeriod();
                            return;
                        }
                        if (task.IsFaulted)
                        {
                            string reason = ConciseAnalysisFailure(task.Exception.GetBaseException());
                            bool retainedPreview = !_hasCompletedAnalysis && _analysis != null &&
                                _analysis.ProjectionState == AnalysisProjectionState.Pending;
                            if (retainedPreview)
                                UpdateAnalysisDisplay(_analysis.WithProjectionFailure(reason), false);
                            else if (_analysis == null)
                            {
                                string message = "Analysis failed: " + reason;
                                _summary.Text = message;
                                _graph.EmptyMessage = message;
                            }
                            EndBusyPeriod();
                            _calculationStatus.Text = (retainedPreview ? "Queue projection failed — " : "Analysis failed — ") + reason;
                            _calculationStatus.Visible = true;
                            return;
                        }
                        RememberLocalAnalysis(cacheKey, task.Result);
                        if (_usesDefaultAnalyzer) RememberSharedAnalysis(song, cacheKey, task.Result);
                        UpdateAnalysis(task.Result);
                        if (!_resolutionDebounceTimer.Enabled) EndBusyPeriod();
                    });
                }
                catch (InvalidOperationException) { }
            });
        }

        private void PublishWorkloadPreview(int generation, CancellationTokenSource cancellation,
            MidiSong song, WorkloadAnalysis preview)
        {
            if (IsDisposed || !IsHandleCreated) return;
            try
            {
                BeginInvoke((MethodInvoker)delegate
                {
                    if (IsDisposed || generation != _analysisGeneration || cancellation != _analysisCancellation ||
                        cancellation.IsCancellationRequested || song != SourceSong || _hasCompletedAnalysis) return;
                    _previewPublicationCount++;
                    UpdateAnalysisDisplay(preview, false);
                });
            }
            catch (InvalidOperationException) { }
        }

        internal void DetachForSongReplacement(string message)
        {
            _resolutionDebounceTimer.Stop();
            CancelCurrentAnalysisWork();
            EndBusyPeriod();
            SourceSong = null;
            _analysis = null;
            _hasCompletedAnalysis = false;
            _pendingConfiguration = null;
            _analysisCache.Clear();
            _analysisCacheOrder.Clear();
            _latestAnalysisProgress = null;
            _seekButton.Enabled = false;
            _split.Enabled = false;
            _headerLayout.Enabled = false;
            Text = "MIDIBottleneck Player — Analysis — no file";
            _summary.Text = message ?? "No MIDI file is attached.";
            _graph.DetachAnalysis(message ?? "No MIDI file is attached.");
        }

        internal void AttachSong(MidiSong song, AnalysisConfiguration configuration)
        {
            if (song == null) throw new ArgumentNullException("song");
            _resolutionDebounceTimer.Stop();
            CancelCurrentAnalysisWork();
            EndBusyPeriod();
            SourceSong = song;
            _analysis = null;
            _hasCompletedAnalysis = false;
            _pendingConfiguration = null;
            _analysisCache.Clear();
            _analysisCacheOrder.Clear();
            _latestAnalysisProgress = null;
            _seekButton.Enabled = false;
            _split.Enabled = true;
            _headerLayout.Enabled = true;
            Text = "MIDIBottleneck Player — Analysis — " + System.IO.Path.GetFileName(song.FilePath);
            _summary.Text = "Analysis will appear here when calculation completes.";
            _graph.DetachAnalysis("Calculating workload analysis…");
            RequestAnalysis(configuration);
        }

        private void ScheduleResolutionRefresh()
        {
            if (_pendingConfiguration == null || IsDisposed || _updatingAnalysis) return;
            long proposedResolution = SelectedResolutionMicroseconds();
            if (_analysis != null && proposedResolution == _analysis.BucketMicroseconds &&
                String.Equals(AnalysisCacheKey(_pendingConfiguration, proposedResolution),
                    AnalysisCacheKey(_analysis.Configuration ?? new AnalysisConfiguration(), _analysis.BucketMicroseconds),
                    StringComparison.Ordinal))
                return;
            BeginBusyPeriod();
            _resolutionDebounceTimer.Stop();
            _resolutionDebounceTimer.Start();
        }

        private void ResolutionSelectionChanged(object sender, EventArgs e)
        {
            if (_suppressResolutionSelection) return;
            string selected = Convert.ToString(_resolutionCombo.SelectedItem, CultureInfo.CurrentCulture);
            if (String.Equals(selected, "Custom…", StringComparison.Ordinal))
            {
                PromptForCustomResolution();
                return;
            }
            if (String.Equals(selected, _autoResolutionLabel, StringComparison.Ordinal))
                _resolutionMode = ResolutionSelectionMode.Auto;
            else if (!String.IsNullOrEmpty(_customResolutionLabel) && String.Equals(selected, _customResolutionLabel, StringComparison.Ordinal))
                _resolutionMode = ResolutionSelectionMode.Custom;
            else
                _resolutionMode = ResolutionSelectionMode.Fixed;
            if (!String.IsNullOrEmpty(selected)) _lastValidResolution = selected;
            ScheduleResolutionRefresh();
        }

        private void PromptForCustomResolution()
        {
            string error;
            using (Form dialog = new Form())
            using (NumericUpDown value = new NumericUpDown())
            using (Button ok = new Button())
            using (Button cancel = new Button())
            {
                dialog.Text = "Custom Analysis resolution";
                ProductIcon.Apply(dialog);
                dialog.FormBorderStyle = FormBorderStyle.FixedDialog;
                dialog.StartPosition = FormStartPosition.CenterParent;
                dialog.ClientSize = new Size(390, 120);
                dialog.MinimizeBox = false;
                dialog.MaximizeBox = false;
                Label label = new Label { Text = "Aggregation interval:", AutoSize = true, Location = new Point(12, 20) };
                value.DecimalPlaces = 3;
                value.Minimum = 0.001m;
                value.Maximum = 86400000m;
                value.Increment = 1m;
                value.Value = _customResolutionMicroseconds > 0 ? _customResolutionMicroseconds / 1000m : 100m;
                value.Location = new Point(145, 17);
                value.Width = 135;
                Label unit = new Label { Text = "ms", AutoSize = true, Location = new Point(286, 20) };
                Label note = new Label
                {
                    Text = "Up to 2,000,000 buckets (about 128 MiB before drawing caches).",
                    AutoSize = true,
                    ForeColor = Color.DimGray,
                    Location = new Point(12, 50)
                };
                ok.Text = "OK"; ok.DialogResult = DialogResult.OK; ok.Location = new Point(218, 82);
                cancel.Text = "Cancel"; cancel.DialogResult = DialogResult.Cancel; cancel.Location = new Point(299, 82);
                dialog.Controls.Add(label); dialog.Controls.Add(value); dialog.Controls.Add(unit); dialog.Controls.Add(note);
                dialog.Controls.Add(ok); dialog.Controls.Add(cancel);
                dialog.AcceptButton = ok; dialog.CancelButton = cancel;
                while (dialog.ShowDialog(this) == DialogResult.OK)
                {
                    long resolution;
                    if (TryValidateCustomResolution(SourceSong, value.Value, out resolution, out error))
                    {
                        ApplyCustomResolution(resolution);
                        return;
                    }
                    MessageBox.Show(dialog, error, "Invalid Analysis resolution", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
            }
            SelectResolutionText(_lastValidResolution);
        }

        internal bool SetCustomResolutionMilliseconds(decimal milliseconds, out string error)
        {
            long resolution;
            if (!TryValidateCustomResolution(SourceSong, milliseconds, out resolution, out error)) return false;
            ApplyCustomResolution(resolution);
            return true;
        }

        internal static bool TryValidateCustomResolution(MidiSong song, decimal milliseconds, out long microseconds, out string error)
        {
            microseconds = 0;
            error = null;
            if (song == null) { error = "No MIDI file is available."; return false; }
            if (milliseconds <= 0 || milliseconds > Decimal.MaxValue / 1000m)
            {
                error = "Enter a positive aggregation interval in milliseconds.";
                return false;
            }
            decimal micros = Decimal.Round(milliseconds * 1000m, 0, MidpointRounding.AwayFromZero);
            if (micros < 1 || micros > Int64.MaxValue)
            {
                error = "The aggregation interval is outside the supported range.";
                return false;
            }
            microseconds = Decimal.ToInt64(micros);
            try { WorkloadAnalyzer.CalculateBucketCount(song.DurationMicroseconds, microseconds); }
            catch (Exception ex) { error = ex.Message; microseconds = 0; return false; }
            return true;
        }

        private void ApplyCustomResolution(long resolution)
        {
            _customResolutionMicroseconds = resolution;
            string label = "Custom: " + FormatResolution(resolution);
            _suppressResolutionSelection = true;
            try
            {
                if (_customResolutionLabel == null)
                    _resolutionCombo.Items.Insert(_resolutionCombo.Items.Count - 1, label);
                else
                    _resolutionCombo.Items[_resolutionCombo.Items.IndexOf(_customResolutionLabel)] = label;
                _customResolutionLabel = label;
                _resolutionCombo.SelectedItem = label;
                _resolutionMode = ResolutionSelectionMode.Custom;
                _lastValidResolution = label;
            }
            finally { _suppressResolutionSelection = false; }
            ScheduleResolutionRefresh();
        }

        private void SelectResolutionText(string text)
        {
            _suppressResolutionSelection = true;
            try { _resolutionCombo.SelectedItem = text; }
            finally { _suppressResolutionSelection = false; }
        }

        internal void CancelAnalysisCalculation()
        {
            _resolutionDebounceTimer.Stop();
            CancelCurrentAnalysisWork();
            // Cancel retires the desired request as well as the worker.  Header
            // controls disappearing can resize the graph; that layout-only
            // resize must not recreate the cancelled configuration.
            _pendingConfiguration = _hasCompletedAnalysis && _analysis != null
                ? CloneConfiguration(_analysis.Configuration) : null;
            if (!_hasCompletedAnalysis && _analysis != null &&
                _analysis.ProjectionState == AnalysisProjectionState.Pending)
                UpdateAnalysisDisplay(_analysis.WithProjectionState(AnalysisProjectionState.Cancelled), false);
            else if (_analysis == null)
            {
                _summary.Text = "Analysis calculation cancelled.";
                _graph.EmptyMessage = "Analysis calculation cancelled.";
            }
            EndBusyPeriod();
        }

        private void CancelCurrentAnalysisWork()
        {
            ++_analysisGeneration;
            CancellationTokenSource cancellation = _analysisCancellation;
            _analysisCancellation = null;
            if (cancellation != null)
            {
                cancellation.Cancel();
                cancellation.Dispose();
            }
        }

        private void BeginBusyPeriod()
        {
            if (_analysisBusy) return;
            _analysisBusy = true;
            _busyIndicatorVisible = false;
            _displayedProgressPermille = 0;
            _latestAnalysisProgress = null;
            _calculationStatus.Text = "Recalculating analysis…";
            _calculationStatus.Visible = false;
            _calculationProgress.Value = 0;
            _calculationProgress.Visible = false;
            _cancelCalculationButton.Visible = false;
            _calculationDelayTimer.Stop();
            _calculationDelayTimer.Start();
            _calculationProgressTimer.Start();
        }

        private void ShowBusyIndicator()
        {
            if (!_analysisBusy) return;
            _busyIndicatorVisible = true;
            _calculationStatus.Visible = true;
            _calculationProgress.Visible = true;
            _cancelCalculationButton.Visible = true;
            RefreshAnalysisProgress();
        }

        private void RefreshAnalysisProgress()
        {
            if (!_analysisBusy) return;
            AnalysisProgressUpdate latest = _latestAnalysisProgress;
            if (latest == null || latest.Generation != _analysisGeneration || latest.Progress == null) return;
            _displayedProgressPermille = Math.Max(_displayedProgressPermille, latest.Progress.OverallPermille);
            if (_busyIndicatorVisible)
            {
                _calculationProgress.Value = Math.Min(1000, _displayedProgressPermille);
                _calculationStatus.Text = latest.Progress.Stage + " — " +
                    (_displayedProgressPermille / 10.0).ToString("N1", CultureInfo.CurrentCulture) + "%";
            }
        }

        private void EndBusyPeriod()
        {
            _analysisBusy = false;
            _busyIndicatorVisible = false;
            _calculationDelayTimer.Stop();
            _calculationProgressTimer.Stop();
            _calculationStatus.Visible = false;
            _calculationProgress.Visible = false;
            _cancelCalculationButton.Visible = false;
            _latestAnalysisProgress = null;
        }

        private static string ConciseAnalysisFailure(Exception exception)
        {
            string message = exception == null ? "Unknown error." : exception.Message;
            if (String.IsNullOrWhiteSpace(message)) message = exception == null ? "Unknown error." : exception.GetType().Name;
            message = message.Replace('\r', ' ').Replace('\n', ' ').Trim();
            return message.Length <= 240 ? message : message.Substring(0, 237) + "…";
        }

        private long SelectedResolutionMicroseconds()
        {
            if (_resolutionMode == ResolutionSelectionMode.Custom)
                return _customResolutionMicroseconds;
            string selected = Convert.ToString(_resolutionCombo.SelectedItem, CultureInfo.CurrentCulture);
            if (_resolutionMode == ResolutionSelectionMode.Fixed)
                return ResolutionFromText(selected);
            long span = _graph == null || _graph.ViewEndMicroseconds <= _graph.ViewStartMicroseconds
                ? Math.Max(1, SourceSong.DurationMicroseconds)
                : _graph.ViewEndMicroseconds - _graph.ViewStartMicroseconds;
            int plotWidth = _graph == null ? 600 : Math.Max(100, _graph.GraphArea.Width);
            return ChooseAutoResolution(span, plotWidth, _analysis == null ? 0 : _analysis.BucketMicroseconds);
        }

        private void UpdateAutoResolutionLabel(long acceptedResolution)
        {
            string label = "Auto (" + FormatResolution(acceptedResolution) + ")";
            if (String.Equals(label, _autoResolutionLabel, StringComparison.Ordinal)) return;
            _suppressResolutionSelection = true;
            try
            {
                int index = _resolutionCombo.Items.IndexOf(_autoResolutionLabel);
                if (index < 0) index = 0;
                _resolutionCombo.Items[index] = label;
                _autoResolutionLabel = label;
                _resolutionCombo.SelectedIndex = index;
                _lastValidResolution = label;
                _toolTip.SetToolTip(_resolutionCombo, "Automatic graph resolution; the displayed graph currently uses " +
                    FormatResolution(acceptedResolution) + " buckets.");
            }
            finally { _suppressResolutionSelection = false; }
        }

        private static long ResolutionFromText(string text)
        {
            switch (text)
            {
                case "10 ms": return 10000;
                case "25 ms": return 25000;
                case "50 ms": return 50000;
                case "100 ms": return 100000;
                case "250 ms": return 250000;
                case "500 ms": return 500000;
                case "1 s": return 1000000;
                default: return 0;
            }
        }

        internal static long ChooseAutoResolution(long span, int plotWidth, long current)
        {
            long[] choices = ResolutionChoices();
            double target = span / (Math.Max(1.0, plotWidth / 1.5));
            if (current > 0 && target >= current * 0.72 && target <= current * 1.85)
                return current;
            long best = choices[0];
            double bestDistance = Double.MaxValue;
            for (int i = 0; i < choices.Length; i++)
            {
                double distance = Math.Abs(Math.Log(Math.Max(1, target) / choices[i]));
                if (distance < bestDistance) { bestDistance = distance; best = choices[i]; }
            }
            return best;
        }

        private static long[] ResolutionChoices()
        {
            return new long[] { 10000, 25000, 50000, 100000, 250000, 500000, 1000000 };
        }

        private static AnalysisConfiguration CloneConfiguration(AnalysisConfiguration source)
        {
            if (source == null) return new AnalysisConfiguration();
            return new AnalysisConfiguration
            {
                SimulateSlowdown = source.SimulateSlowdown,
                ServiceDurationMode = source.ServiceDurationMode,
                ProcessingMicroseconds = source.ProcessingMicroseconds,
                MidiBitrate = source.MidiBitrate,
                QueueLengthLimitEnabled = source.QueueLengthLimitEnabled,
                QueueLengthLimit = source.QueueLengthLimit,
                OverflowPolicy = source.OverflowPolicy,
                PerNoteIntervalGateEnabled = source.PerNoteIntervalGateEnabled,
                ApplyQueueLimitWithoutSlowdown = source.ApplyQueueLimitWithoutSlowdown
            };
        }

        private static string AnalysisCacheKey(AnalysisConfiguration value, long resolution)
        {
            return resolution.ToString(CultureInfo.InvariantCulture) + "|" +
                value.SimulateSlowdown + "|" + value.ApplyQueueLimitWithoutSlowdown + "|" + (int)value.ServiceDurationMode + "|" +
                ((value.SimulateSlowdown || value.ApplyQueueLimitWithoutSlowdown && value.QueueLengthLimitEnabled) && value.ServiceDurationMode == ServiceDurationMode.ProcessingTime ? value.ProcessingMicroseconds : 0).ToString(CultureInfo.InvariantCulture) + "|" +
                ((value.SimulateSlowdown || value.ApplyQueueLimitWithoutSlowdown && value.QueueLengthLimitEnabled) && value.ServiceDurationMode == ServiceDurationMode.MidiBitrate ? value.MidiBitrate : 0).ToString(CultureInfo.InvariantCulture) + "|" +
                value.QueueLengthLimitEnabled + "|" + (value.QueueLengthLimitEnabled ? value.QueueLengthLimit : 0).ToString(CultureInfo.InvariantCulture) + "|" +
                (value.QueueLengthLimitEnabled ? (int)value.OverflowPolicy : 0);
        }

        private void RememberLocalAnalysis(string key, WorkloadAnalysis analysis)
        {
            if (!_analysisCache.ContainsKey(key))
            {
                while (_analysisCache.Count >= 12 && _analysisCacheOrder.Count > 0)
                    _analysisCache.Remove(_analysisCacheOrder.Dequeue());
                _analysisCacheOrder.Enqueue(key);
            }
            _analysisCache[key] = analysis;
        }

        private static bool TryGetSharedAnalysis(MidiSong song, string key, out WorkloadAnalysis analysis)
        {
            analysis = null;
            SharedSongAnalysisCache cache = SharedAnalysisCaches.GetOrCreateValue(song);
            lock (cache)
            {
                WeakReference reference;
                if (!cache.Results.TryGetValue(key, out reference)) return false;
                analysis = reference.Target as WorkloadAnalysis;
                if (analysis != null) return true;
                cache.Results.Remove(key);
                return false;
            }
        }

        private static void RememberSharedAnalysis(MidiSong song, string key, WorkloadAnalysis analysis)
        {
            SharedSongAnalysisCache cache = SharedAnalysisCaches.GetOrCreateValue(song);
            lock (cache)
            {
                if (!cache.Results.ContainsKey(key))
                {
                    while (cache.Results.Count >= 12 && cache.Order.Count > 0)
                        cache.Results.Remove(cache.Order.Dequeue());
                    cache.Order.Enqueue(key);
                }
                cache.Results[key] = new WeakReference(analysis);
            }
        }

        private void GraphInspectionChanged(object sender, WorkloadSelectionEventArgs e)
        {
            UpdateInspection(e.TimeMicroseconds, e.Pinned);
        }

        private void GraphSeekRequested(object sender, WorkloadSelectionEventArgs e)
        {
            UpdateInspection(e.TimeMicroseconds, true);
            RaiseSeekRequested(e.TimeMicroseconds);
        }

        private void RaiseSeekRequested(long timeMicroseconds)
        {
            EventHandler<WorkloadSelectionEventArgs> handler = SeekRequested;
            if (handler != null) handler(this, new WorkloadSelectionEventArgs(timeMicroseconds, true));
        }

        private void UpdateInspection(long timeMicroseconds, bool pinned)
        {
            if (_analysis == null || _inspection == null) return;
            WorkloadBucket bucket = _graph.BucketAt(timeMicroseconds);
            int bucketIndex = (int)Math.Max(0, Math.Min(_analysis.Buckets.Length - 1,
                timeMicroseconds / Math.Max(1, _analysis.BucketMicroseconds)));
            long start = bucketIndex * _analysis.BucketMicroseconds;
            long end = Math.Min(_analysis.DurationMicroseconds, start + _analysis.BucketMicroseconds);
            double seconds = _analysis.BucketMicroseconds / 1000000.0;
            StringBuilder text = new StringBuilder();
            text.Append(pinned ? "Pinned: " : "Cursor: ").AppendLine(FormatClock(timeMicroseconds));
            text.Append("Bucket: ").Append(FormatClock(start)).Append("–").AppendLine(FormatClock(end));
            if (bucket != null)
            {
                text.Append("Events/sec: ").Append((bucket.EventCount / seconds).ToString("N1", CultureInfo.CurrentCulture));
                text.Append("   MIDI bytes/sec: ").AppendLine((bucket.ByteCount / seconds).ToString("N1", CultureInfo.CurrentCulture));
                text.Append("Largest simultaneous cluster: ").Append(bucket.LargestCluster.ToString("N0", CultureInfo.CurrentCulture));
                if (_analysis.HasQueueProjection && _analysis.Configuration != null &&
                    _analysis.Configuration.QueueLengthLimitEnabled)
                {
                    text.Append("   Predicted occupancy: ").Append(bucket.PredictedPeakOccupancy.ToString("N0", CultureInfo.CurrentCulture));
                    text.Append(" / ").Append(_analysis.Configuration.QueueLengthLimit.ToString("N0", CultureInfo.CurrentCulture));
                    text.Append("   Predicted drops: ").Append(bucket.PredictedDroppedEvents.ToString("N0", CultureInfo.CurrentCulture));
                    if (bucket.PredictedBufferClears > 0) text.Append("   Predicted buffer clear");
                }
            }
            _inspection.Text = text.ToString();
            _seekButton.Enabled = _graph.PinnedTimeMicroseconds.HasValue;
        }

        private void ApplySummary(string text)
        {
            _summary.Text = text;
            string[] headings = new string[] { "MIDI FILE", "WORKLOAD", "PROCESSING MODEL", "QUEUE PROJECTION", "EVENT CLUSTERS", "MESSAGE TYPES" };
            for (int i = 0; i < headings.Length; i++)
            {
                int start = _summary.Text.IndexOf(headings[i], StringComparison.Ordinal);
                if (start < 0) continue;
                _summary.Select(start, headings[i].Length);
                _summary.SelectionFont = _summaryHeadingFont;
                _summary.SelectionColor = Color.FromArgb(42, 92, 150);
            }
            _summary.Select(0, 0);
        }

        private static string BuildSummary(MidiSong song, WorkloadAnalysis analysis)
        {
            StringBuilder text = new StringBuilder();
            text.AppendLine("MIDI FILE");
            text.AppendLine("Filename              " + System.IO.Path.GetFileName(song.FilePath));
            text.AppendLine("SMF format            " + song.Format.ToString(CultureInfo.CurrentCulture));
            text.AppendLine("Tracks                " + song.TrackCount.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("PPQN                  " + song.TicksPerQuarterNote.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Duration              " + FormatClock(song.DurationMicroseconds));
            text.AppendLine("Musical note-ons      " + song.NoteCount.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Dispatchable events   " + analysis.TotalEvents.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("File size             " + FormatBytes(song.FileSizeBytes));
            text.AppendLine();
            text.AppendLine("WORKLOAD");
            text.AppendLine("Scope                 Whole file, " + FormatClock(0) + " – " + FormatClock(analysis.DurationMicroseconds));
            text.AppendLine("Graph resolution      " + FormatResolution(analysis.BucketMicroseconds));
            text.AppendLine("MIDI message bytes    " + analysis.TotalBytes.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Unique timestamps     " + analysis.UniqueTimestamps.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Average events/sec    " + analysis.AverageEventsPerSecond.ToString("N1", CultureInfo.CurrentCulture));
            text.AppendLine("Peak events/sec       " + analysis.PeakEventsPerSecond.ToString("N1", CultureInfo.CurrentCulture));
            text.AppendLine("Peak bytes/sec        " + analysis.PeakBytesPerSecond.ToString("N1", CultureInfo.CurrentCulture));
            if (!analysis.HasQueueProjection)
            {
                text.AppendLine();
                text.AppendLine("QUEUE PROJECTION");
                if (analysis.ProjectionState == AnalysisProjectionState.Pending)
                    text.AppendLine("Status                Queue projection pending");
                else if (analysis.ProjectionState == AnalysisProjectionState.Cancelled)
                    text.AppendLine("Status                Workload only — projection cancelled");
                else
                {
                    text.AppendLine("Status                Workload only — queue projection failed");
                    if (!String.IsNullOrEmpty(analysis.ProjectionFailureReason))
                        text.AppendLine("Failure               " + analysis.ProjectionFailureReason);
                }
            }
            if (analysis.HasQueueProjection && analysis.Configuration != null)
            {
                AnalysisConfiguration configuration = analysis.Configuration;
                text.AppendLine();
                text.AppendLine("PROCESSING MODEL");
                bool virtualForward = !configuration.SimulateSlowdown && configuration.QueueLengthLimitEnabled &&
                    configuration.ApplyQueueLimitWithoutSlowdown;
                text.AppendLine("Simulate slowdown     " + (configuration.SimulateSlowdown ? "On" : "Off (accepted output is immediate)"));
                if (virtualForward)
                    text.AppendLine("Forward queue limit   On — modeled pressure can reject new MIDI, but never delays or retracts sent MIDI");
                if (configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                {
                    text.AppendLine("Rate model            MIDI serial bitrate at " + configuration.MidiBitrate.ToString("N0", CultureInfo.CurrentCulture) + " bit/s");
                    text.AppendLine("Maximum rate          " + analysis.ByteServiceCapacityPerSecond.ToString("N1", CultureInfo.CurrentCulture) + " bytes/sec");
                }
                else
                {
                    text.AppendLine("Rate model            Processing time, " + configuration.ProcessingMicroseconds.ToString("N0", CultureInfo.CurrentCulture) + " µs/event");
                    text.AppendLine("Maximum rate          " +
                        ((!configuration.SimulateSlowdown && !virtualForward) || configuration.ProcessingMicroseconds == 0
                            ? "Immediate (simulated)"
                            : analysis.EventServiceCapacityPerSecond.ToString("N1", CultureInfo.CurrentCulture) + " events/sec"));
                }
                text.AppendLine();
                text.AppendLine("QUEUE PROJECTION");
                text.AppendLine("Queue length limit    " + (configuration.QueueLengthLimitEnabled ? configuration.QueueLengthLimit.ToString("N0", CultureInfo.CurrentCulture) + " event slots" : "Unlimited"));
                if (configuration.PerNoteIntervalGateEnabled)
                {
                    text.AppendLine("Per-note interval gate Live playback only");
                    text.AppendLine("Gate projection       Not included in this static source/configuration Analysis");
                }
                if (configuration.QueueLengthLimitEnabled)
                    text.AppendLine("Overflow policy       " + FormatOverflowPolicy(configuration.OverflowPolicy));
                text.AppendLine("Predicted peak        " + analysis.PredictedMaximumOccupancy.ToString("N0", CultureInfo.CurrentCulture) + " outstanding events");
                text.AppendLine("Predicted drops       " + analysis.PredictedDroppedEvents.ToString("N0", CultureInfo.CurrentCulture));
                if (analysis.PredictedBufferClears > 0)
                    text.AppendLine("Predicted clears      " + analysis.PredictedBufferClears.ToString("N0", CultureInfo.CurrentCulture));
                text.AppendLine("Source end            " + FormatClock(song.DurationMicroseconds));
                text.AppendLine("Predicted completion  " + FormatClock(analysis.PredictedOutputCompletionMicroseconds));
                long predictedOverrun = Math.Max(0, analysis.PredictedOutputCompletionMicroseconds - song.DurationMicroseconds);
                if (predictedOverrun > 0)
                    text.AppendLine("Predicted overrun     " + FormatClock(predictedOverrun) + " after source end");
                text.AppendLine(virtualForward
                    ? "Completion is the last accepted source event; virtual queue work does not delay accepted output."
                    : "Completion is modeled accepted-event service, not synthesizer or audible-tail completion.");
                if (virtualForward)
                    text.AppendLine("Virtual pressure does not predict delay inside a MIDI driver or synthesizer.");
                text.AppendLine("Projection is simulator output, not a hardware measurement.");
                text.AppendLine("Live channel mutes/overrides are not applied to this static source-file projection.");
            }
            text.AppendLine();
            text.AppendLine("EVENT CLUSTERS");
            text.AppendLine("Largest simultaneous  " + analysis.LargestTimestampCluster.ToString("N0", CultureInfo.CurrentCulture));
            AppendCluster(text, ">= 2", analysis.EventsInClustersAtLeast2, analysis.TotalEvents);
            AppendCluster(text, ">= 10", analysis.EventsInClustersAtLeast10, analysis.TotalEvents);
            AppendCluster(text, ">= 50", analysis.EventsInClustersAtLeast50, analysis.TotalEvents);
            AppendCluster(text, ">= 100", analysis.EventsInClustersAtLeast100, analysis.TotalEvents);
            text.AppendLine();
            text.AppendLine("MESSAGE TYPES");
            text.AppendLine("Type                     Events       Bytes");
            for (int i = 0; i < analysis.MessageTypes.Count; i++)
            {
                MessageTypeWorkload type = analysis.MessageTypes[i];
                text.Append(type.Kind.ToString().PadRight(24));
                text.Append(type.EventCount.ToString("N0", CultureInfo.CurrentCulture).PadLeft(10));
                text.Append(type.ByteCount.ToString("N0", CultureInfo.CurrentCulture).PadLeft(12));
                text.AppendLine();
            }
            return text.ToString();
        }

        private static string FormatBytes(long bytes)
        {
            if (bytes <= 0) return "—";
            if (bytes >= 1024L * 1024L) return (bytes / (1024.0 * 1024.0)).ToString("N2", CultureInfo.CurrentCulture) + " MiB";
            if (bytes >= 1024L) return (bytes / 1024.0).ToString("N1", CultureInfo.CurrentCulture) + " KiB";
            return bytes.ToString("N0", CultureInfo.CurrentCulture) + " bytes";
        }

        private static string FormatResolution(long microseconds)
        {
            return microseconds >= 1000000
                ? (microseconds / 1000000.0).ToString("0.###", CultureInfo.CurrentCulture) + " s"
                : (microseconds / 1000.0).ToString("0.###", CultureInfo.CurrentCulture) + " ms";
        }

        private static void AppendCluster(StringBuilder text, string threshold, long count, long total)
        {
            double percent = total == 0 ? 0 : 100.0 * count / total;
            text.AppendLine(threshold.PadRight(8) + count.ToString("N0", CultureInfo.CurrentCulture).PadLeft(12) + "  (" + percent.ToString("N2", CultureInfo.CurrentCulture) + "%)");
        }

        private static string FormatOverflowPolicy(OverflowPolicy policy)
        {
            if (policy == OverflowPolicy.DropOldest) return "Drop oldest pending event";
            if (policy == OverflowPolicy.ClearBufferAndCatchUp) return "Clear buffer and jump to realtime";
            if (policy == OverflowPolicy.DropIncomingCompleteNotes) return "Drop incoming complete notes";
            return "Drop newest";
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00", CultureInfo.InvariantCulture) + ":" +
                value.Seconds.ToString("00", CultureInfo.InvariantCulture) + "." + value.Milliseconds.ToString("000", CultureInfo.InvariantCulture);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                CancelAnalysisCalculation();
                _calculationDelayTimer.Dispose();
                _calculationProgressTimer.Dispose();
                _resolutionDebounceTimer.Dispose();
                _toolTip.Dispose();
                _summaryHeadingFont.Dispose();
                SourceSong = null;
                _analysis = null;
                _analysisCache.Clear();
                _analysisCacheOrder.Clear();
            }
            base.Dispose(disposing);
        }
    }
}
