using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Text;
using System.Windows.Forms;

namespace MidiBottleneck
{
    // Complete session-only editor for the deterministic routing planner.
    // Nothing is published to MainForm until a fully valid preview is accepted.
    internal sealed class TrackRoutingForm : Form
    {
        private sealed class DestinationItem
        {
            internal readonly int Channel;
            internal readonly string Text;
            internal DestinationItem(int channel, string text)
            { Channel = channel; Text = text; }
            public override string ToString() { return Text; }
        }

        private readonly MidiSong _source;
        private readonly int _scalePercent;
        private readonly bool _hadActiveRoute;
        private TrackRoutingSettings _settings;
        private CompiledTrackRoute _compiled;
        private bool _updating;

        private ComboBox _tier;
        private NumericUpDown _capacity;
        private ComboBox _percussion;
        private CheckBox[] _usableChannels;
        private DataGridView _lanes;
        private Label _selectedLaneCaption;
        private ComboBox _laneDestination;
        private TextBox _preview;
        private Label _status;
        private CheckBox _confirmLossy;
        private Button _apply;
        private Button _original;
        private Font _usedChannelFont;
        private ToolTip _toolTips;

        internal TrackRoutingForm(MidiSong source, TrackRoutingSettings initialSettings,
            bool hasActiveRoute, int scalePercent)
        {
            if (source == null) throw new ArgumentNullException("source");
            _source = source;
            _settings = (initialSettings ?? new TrackRoutingSettings()).Clone();
            _hadActiveRoute = hasActiveRoute;
            _scalePercent = Math.Max(50, Math.Min(200, scalePercent));

            ProductIcon.Apply(this);
            Text = "Track routing — " + ProductIdentity.Name;
            StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.Sizable;
            MinimizeBox = false;
            ShowInTaskbar = false;
            AutoScaleMode = AutoScaleMode.Font;
            Font = UiScaleFont.CreateUi(9F, FontStyle.Regular, _scalePercent);
            AccessibleName = "Track routing planner";
            BuildInterface();
            RefreshPlan(null);
        }

        internal CompiledTrackRoute SelectedRoute { get; private set; }
        internal TrackRoutingSettings SelectedSettings { get; private set; }
        internal bool ExplicitlyConfirmed { get; private set; }
        internal bool UseOriginalRouting { get; private set; }

        internal CompiledTrackRoute CompiledRouteForTesting { get { return _compiled; } }
        internal bool ApplyEnabledForTesting { get { return _apply.Enabled; } }
        internal string PreviewTextForTesting { get { return _preview.Text; } }
        internal bool LossyConfirmationRequiredForTesting
        { get { return _compiled != null && _compiled.RequiresExplicitApply; } }
        internal int LaneRowCountForTesting { get { return _lanes.Rows.Count; } }
        internal string SelectedLaneCaptionForTesting { get { return _selectedLaneCaption.Text; } }
        internal bool ChannelCaptionBoldForTesting(int channel)
        { return _usableChannels[channel].Font.Bold; }
        internal string PatchToolTipForTesting(TrackLaneKey lane)
        {
            for (int index = 0; index < _lanes.Rows.Count; index++)
                if (_lanes.Rows[index].Tag is TrackLaneKey &&
                    ((TrackLaneKey)_lanes.Rows[index].Tag).Equals(lane))
                    return _lanes.Rows[index].Cells[1].ToolTipText;
            return String.Empty;
        }
        internal string LaneDestinationForTesting(TrackLaneKey lane)
        { return LaneCellTextForTesting(lane, 3); }
        internal string LaneStatusForTesting(TrackLaneKey lane)
        { return LaneCellTextForTesting(lane, 4); }
        internal bool LaneIsOmissionProminentForTesting(TrackLaneKey lane)
        {
            for (int index = 0; index < _lanes.Rows.Count; index++)
                if (_lanes.Rows[index].Tag is TrackLaneKey &&
                    ((TrackLaneKey)_lanes.Rows[index].Tag).Equals(lane))
                    return _lanes.Rows[index].DefaultCellStyle.BackColor == Color.LightCoral;
            return false;
        }
        private string LaneCellTextForTesting(TrackLaneKey lane, int cell)
        {
            for (int index = 0; index < _lanes.Rows.Count; index++)
                if (_lanes.Rows[index].Tag is TrackLaneKey &&
                    ((TrackLaneKey)_lanes.Rows[index].Tag).Equals(lane))
                    return Convert.ToString(_lanes.Rows[index].Cells[cell].Value,
                        CultureInfo.CurrentCulture);
            return String.Empty;
        }
        internal bool CoreControlsFitForTesting
        {
            get
            {
                return _lanes.Width > 0 && _lanes.Height > 0 &&
                    _preview.Width > 0 && _preview.Height > 0 &&
                    _apply.Width > 0 && _apply.Height > 0 &&
                    _lanes.Parent.ClientRectangle.Contains(_lanes.Bounds) &&
                    _preview.Parent.ClientRectangle.Contains(_preview.Bounds);
            }
        }

        private int ScaleMetric(int value)
        {
            return Math.Max(1, (int)Math.Round(value * _scalePercent / 100.0));
        }

        private void BuildInterface()
        {
            _usedChannelFont = new Font(Font, FontStyle.Bold);
            _toolTips = new ToolTip();
            Rectangle work = Screen.PrimaryScreen == null
                ? new Rectangle(0, 0, ScaleMetric(1200), ScaleMetric(800))
                : Screen.PrimaryScreen.WorkingArea;
            ClientSize = new Size(
                Math.Min(ScaleMetric(1050), Math.Max(ScaleMetric(720), work.Width - ScaleMetric(80))),
                Math.Min(ScaleMetric(690), Math.Max(ScaleMetric(520), work.Height - ScaleMetric(80))));
            MinimumSize = new Size(
                Math.Min(ScaleMetric(760), Width), Math.Min(ScaleMetric(540), Height));

            TableLayoutPanel root = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                Padding = new Padding(ScaleMetric(10)),
                ColumnCount = 1,
                RowCount = 4
            };
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(root);

            Label introduction = new Label
            {
                AutoSize = true,
                MaximumSize = new Size(ClientSize.Width - ScaleMetric(40), 0),
                Text = "Build a session-only suggested route for each MIDI track and channel combination. " +
                    "One MIDI track can appear more than once when it uses multiple channels. " +
                    "Review every destination and warning, correct rows when needed, then Apply. " +
                    "The original MIDI data is never changed."
            };
            root.Controls.Add(introduction, 0, 0);

            TableLayoutPanel options = new TableLayoutPanel
            {
                AutoSize = true,
                Dock = DockStyle.Top,
                ColumnCount = 6,
                RowCount = 2,
                Margin = new Padding(0, ScaleMetric(8), 0, ScaleMetric(8))
            };
            options.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 34F));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 18F));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 48F));
            root.Controls.Add(options, 0, 1);

            options.Controls.Add(NewCaption("Automatic tier:"), 0, 0);
            _tier = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList,
                Dock = DockStyle.Fill,
                AccessibleName = "Maximum automatic routing tier"
            };
            _tier.Items.AddRange(new object[]
            {
                "Exact-safe only", "GM-family (confirmation required)",
                "Aggressive (confirmation required)"
            });
            _tier.SelectedIndex = (int)_settings.MaximumAutomaticTier;
            _tier.SelectedIndexChanged += delegate
            {
                if (_updating) return;
                _settings.MaximumAutomaticTier = (TrackRoutingTier)_tier.SelectedIndex;
                RefreshPlan(SelectedLane());
            };
            options.Controls.Add(_tier, 1, 0);

            options.Controls.Add(NewCaption("Melodic destinations:"), 2, 0);
            _capacity = new NumericUpDown
            {
                Minimum = 1,
                Maximum = 16,
                Value = Math.Max(1, Math.Min(16, _settings.TargetCapacity)),
                Dock = DockStyle.Fill,
                AccessibleName = "Maximum melodic destination channels"
            };
            _capacity.ValueChanged += delegate
            {
                if (_updating) return;
                _settings.TargetCapacity = (int)_capacity.Value;
                RefreshPlan(SelectedLane());
            };
            options.Controls.Add(_capacity, 3, 0);

            options.Controls.Add(NewCaption("Percussion:"), 4, 0);
            _percussion = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList,
                Dock = DockStyle.Fill,
                AccessibleName = "Percussion routing policy"
            };
            _percussion.Items.AddRange(new object[]
            { "Reserve destination channel 10", "Allow any selected destination" });
            _percussion.SelectedIndex = (int)_settings.PercussionPolicy;
            _percussion.SelectedIndexChanged += delegate
            {
                if (_updating) return;
                _settings.PercussionPolicy = (PercussionRoutingPolicy)_percussion.SelectedIndex;
                RefreshPlan(SelectedLane());
            };
            options.Controls.Add(_percussion, 5, 0);

            Label availableLabel = NewCaption("Usable output channels:");
            options.Controls.Add(availableLabel, 0, 1);
            FlowLayoutPanel channels = new FlowLayoutPanel
            {
                AutoSize = true,
                Dock = DockStyle.Fill,
                WrapContents = true,
                Margin = new Padding(0, ScaleMetric(4), 0, 0)
            };
            options.SetColumnSpan(channels, 5);
            options.Controls.Add(channels, 1, 1);
            _usableChannels = new CheckBox[16];
            for (int channel = 0; channel < 16; channel++)
            {
                int captured = channel;
                CheckBox check = new CheckBox
                {
                    AutoSize = true,
                    Text = (channel + 1).ToString(CultureInfo.CurrentCulture),
                    Checked = _settings.UsableDestinationChannels[channel],
                    AccessibleName = "Use output channel " + (channel + 1).ToString(CultureInfo.CurrentCulture),
                    Margin = new Padding(0, 0, ScaleMetric(8), 0)
                };
                check.CheckedChanged += delegate
                {
                    if (_updating) return;
                    _settings.UsableDestinationChannels[captured] = check.Checked;
                    RefreshPlan(SelectedLane());
                };
                _usableChannels[channel] = check;
                channels.Controls.Add(check);
            }

            SplitContainer split = new SplitContainer
            {
                Dock = DockStyle.Fill,
                Orientation = Orientation.Vertical,
                Size = new Size(Math.Max(ScaleMetric(720), ClientSize.Width - ScaleMetric(20)),
                    Math.Max(ScaleMetric(360), ClientSize.Height - ScaleMetric(180)))
            };
            split.Panel1MinSize = ScaleMetric(420);
            split.Panel2MinSize = ScaleMetric(220);
            split.SplitterDistance = Math.Max(split.Panel1MinSize,
                Math.Min(split.Width - split.Panel2MinSize,
                    Math.Max(ScaleMetric(560), split.Width * 3 / 5)));
            root.Controls.Add(split, 0, 2);

            TableLayoutPanel lanePanel = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 2,
                RowCount = 3
            };
            lanePanel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            lanePanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            lanePanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            lanePanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            lanePanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            split.Panel1.Controls.Add(lanePanel);
            Label lanesHeading = NewHeading("MIDI tracks and channels with proposed destinations");
            lanePanel.SetColumnSpan(lanesHeading, 2);
            lanePanel.Controls.Add(lanesHeading, 0, 0);

            _lanes = new DataGridView
            {
                Dock = DockStyle.Fill,
                ReadOnly = true,
                AllowUserToAddRows = false,
                AllowUserToDeleteRows = false,
                AllowUserToResizeRows = false,
                AutoGenerateColumns = false,
                AutoSizeRowsMode = DataGridViewAutoSizeRowsMode.AllCells,
                RowHeadersVisible = false,
                MultiSelect = false,
                SelectionMode = DataGridViewSelectionMode.FullRowSelect,
                BackgroundColor = SystemColors.Window,
                BorderStyle = BorderStyle.Fixed3D,
                AccessibleName = "MIDI track and channel routing preview"
            };
            AddColumn("MIDI track / channel", ScaleMetric(155), DataGridViewAutoSizeColumnMode.None);
            AddColumn("Patch", ScaleMetric(155), DataGridViewAutoSizeColumnMode.None);
            AddColumn("Events / notes", ScaleMetric(88), DataGridViewAutoSizeColumnMode.None);
            AddColumn("Destination", ScaleMetric(82), DataGridViewAutoSizeColumnMode.None);
            AddColumn("Status", ScaleMetric(170), DataGridViewAutoSizeColumnMode.Fill);
            _lanes.SelectionChanged += delegate { UpdateSelectedLaneEditor(); };
            lanePanel.SetColumnSpan(_lanes, 2);
            lanePanel.Controls.Add(_lanes, 0, 1);

            _selectedLaneCaption = NewCaption("Selected MIDI track/channel output:");
            lanePanel.Controls.Add(_selectedLaneCaption, 0, 2);
            _laneDestination = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList,
                Dock = DockStyle.Fill,
                AccessibleName = "Selected MIDI track and channel output correction"
            };
            _laneDestination.Items.Add(new DestinationItem(-1, "Automatic suggestion"));
            for (int channel = 0; channel < 16; channel++)
                _laneDestination.Items.Add(new DestinationItem(channel,
                    "Output channel " + (channel + 1).ToString(CultureInfo.CurrentCulture)));
            _laneDestination.SelectedIndexChanged += LaneDestinationChanged;
            string correctionHelp = "Select a table row to choose the MIDI track and channel being edited. " +
                "Automatic suggestion restores planned routing; choosing an output channel " +
                "manually corrects that row's destination.";
            _toolTips.SetToolTip(_selectedLaneCaption, correctionHelp);
            _toolTips.SetToolTip(_laneDestination, correctionHelp);
            lanePanel.Controls.Add(_laneDestination, 1, 2);

            TableLayoutPanel previewPanel = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 1,
                RowCount = 2
            };
            previewPanel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            previewPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            split.Panel2.Controls.Add(previewPanel);
            previewPanel.Controls.Add(NewHeading("Complete plan preview"), 0, 0);
            _preview = new TextBox
            {
                Dock = DockStyle.Fill,
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Both,
                WordWrap = false,
                Font = UiScaleFont.CreateMonospace(8.5F, FontStyle.Regular, _scalePercent),
                BackColor = SystemColors.Window,
                AccessibleName = "Complete route plan and warnings"
            };
            previewPanel.Controls.Add(_preview, 0, 1);

            TableLayoutPanel bottom = new TableLayoutPanel
            {
                AutoSize = true,
                Dock = DockStyle.Fill,
                ColumnCount = 5,
                RowCount = 2,
                Margin = new Padding(0, ScaleMetric(8), 0, 0)
            };
            bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            for (int index = 1; index < 5; index++)
                bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            root.Controls.Add(bottom, 0, 3);

            _status = new Label { AutoSize = true, Anchor = AnchorStyles.Left };
            bottom.Controls.Add(_status, 0, 0);
            bottom.SetColumnSpan(_status, 5);
            _confirmLossy = new CheckBox
            {
                AutoSize = true,
                Text = "I understand the listed retimbrings, conflicts, handoffs, or manual-route warnings.",
                AccessibleName = "Confirm lossy routing warnings",
                Margin = new Padding(0, ScaleMetric(5), ScaleMetric(8), 0)
            };
            _confirmLossy.CheckedChanged += delegate { UpdateApplyState(); };
            bottom.Controls.Add(_confirmLossy, 0, 1);

            _original = new Button
            {
                AutoSize = true,
                Text = "Use original routing",
                Enabled = _hadActiveRoute,
                AccessibleName = "Clear the active track route"
            };
            _original.Click += delegate
            {
                UseOriginalRouting = true;
                SelectedRoute = null;
                SelectedSettings = null;
                ExplicitlyConfirmed = false;
                DialogResult = DialogResult.OK;
            };
            bottom.Controls.Add(_original, 2, 1);

            _apply = new Button { AutoSize = true, Text = "Apply", AccessibleName = "Apply track route" };
            _apply.Click += ApplyClicked;
            bottom.Controls.Add(_apply, 3, 1);
            Button cancel = new Button { AutoSize = true, Text = "Cancel", DialogResult = DialogResult.Cancel };
            bottom.Controls.Add(cancel, 4, 1);
            AcceptButton = _apply;
            CancelButton = cancel;
        }

        private Label NewCaption(string text)
        {
            return new Label
            {
                AutoSize = true,
                Text = text,
                Anchor = AnchorStyles.Left,
                Margin = new Padding(0, ScaleMetric(5), ScaleMetric(5), ScaleMetric(3))
            };
        }

        private Label NewHeading(string text)
        {
            return new Label
            {
                AutoSize = true,
                Text = text,
                Font = new Font(Font, FontStyle.Bold),
                Margin = new Padding(0, 0, 0, ScaleMetric(4))
            };
        }

        private void AddColumn(string heading, int width, DataGridViewAutoSizeColumnMode mode)
        {
            _lanes.Columns.Add(new DataGridViewTextBoxColumn
            {
                HeaderText = heading,
                Width = width,
                AutoSizeMode = mode,
                SortMode = DataGridViewColumnSortMode.NotSortable
            });
        }

        private TrackLaneKey? SelectedLane()
        {
            if (_lanes == null || _lanes.SelectedRows.Count == 0) return null;
            object tag = _lanes.SelectedRows[0].Tag;
            return tag is TrackLaneKey ? (TrackLaneKey?)tag : null;
        }

        private void RefreshPlan(TrackLaneKey? restoreLane)
        {
            _confirmLossy.Checked = false;
            _compiled = TrackRoutePlanner.Compile(_source, _settings);
            UpdateUsedChannelEmphasis();
            RebuildLaneGrid(restoreLane);
            _preview.Text = BuildPreviewText();
            UpdateApplyState();
        }

        private void RebuildLaneGrid(TrackLaneKey? restoreLane)
        {
            _updating = true;
            try
            {
                _lanes.Rows.Clear();
                int selected = -1;
                for (int index = 0; index < _compiled.Lanes.Count; index++)
                {
                    TrackRouteLaneProfile lane = _compiled.Lanes[index];
                    TrackRouteAssignment assignment = FindAssignment(lane.Lane);
                    string destination = assignment == null ? "Unresolved" : assignment.Omitted
                        ? "Omitted"
                        : "Channel " + (assignment.DestinationChannel + 1).ToString(CultureInfo.CurrentCulture);
                    string status = assignment == null ? "Hard error — no complete route" :
                        assignment.Omitted
                            ? "Aggressive — omitted " + assignment.Profile.AttackCount.ToString(
                                "N0", CultureInfo.CurrentCulture) +
                                (assignment.Profile.AttackCount == 1 ? " note start" : " note starts")
                            : (assignment.Manual ? "Manual" : TierName(assignment.Tier));
                    if (assignment != null && assignment.Warnings.Count != 0)
                        status += " — " + String.Join("; ", new List<string>(assignment.Warnings).ToArray());
                    int rowIndex = _lanes.Rows.Add(
                        lane.DisplayName + " · channel " +
                            (lane.Lane.SourceChannel + 1).ToString(CultureInfo.CurrentCulture),
                        lane.RepresentativePatch.ToString(),
                        lane.EventCount.ToString("N0", CultureInfo.CurrentCulture) + " / " +
                            lane.AttackCount.ToString("N0", CultureInfo.CurrentCulture),
                        destination, status);
                    DataGridViewRow row = _lanes.Rows[rowIndex];
                    row.Tag = lane.Lane;
                    row.Cells[1].ToolTipText =
                        GeneralMidiProgramCatalog.DescribePatch(lane.RepresentativePatch);
                    if (assignment == null)
                        row.DefaultCellStyle.BackColor = Color.MistyRose;
                    else if (assignment.Omitted)
                        row.DefaultCellStyle.BackColor = Color.LightCoral;
                    else if (assignment.Warnings.Count != 0 || assignment.Manual)
                        row.DefaultCellStyle.BackColor = Color.LemonChiffon;
                    if (restoreLane.HasValue && lane.Lane.Equals(restoreLane.Value)) selected = rowIndex;
                }
                if (_lanes.Rows.Count != 0)
                {
                    if (selected < 0) selected = 0;
                    _lanes.ClearSelection();
                    _lanes.Rows[selected].Selected = true;
                    _lanes.CurrentCell = _lanes.Rows[selected].Cells[0];
                }
            }
            finally { _updating = false; }
            UpdateSelectedLaneEditor();
        }

        private void UpdateUsedChannelEmphasis()
        {
            bool[] used = new bool[16];
            if (_compiled != null)
                for (int index = 0; index < _compiled.Destinations.Count; index++)
                {
                    int channel = _compiled.Destinations[index].DestinationChannel;
                    if (channel >= 0 && channel < used.Length) used[channel] = true;
                }
            for (int channel = 0; channel < _usableChannels.Length; channel++)
                _usableChannels[channel].Font = used[channel] ? _usedChannelFont : Font;
        }

        private TrackRouteAssignment FindAssignment(TrackLaneKey lane)
        {
            for (int index = 0; index < _compiled.Assignments.Count; index++)
                if (_compiled.Assignments[index].Profile.Lane.Equals(lane))
                    return _compiled.Assignments[index];
            return null;
        }

        private void UpdateSelectedLaneEditor()
        {
            if (_updating) return;
            TrackLaneKey? selected = SelectedLane();
            _updating = true;
            try
            {
                _laneDestination.Enabled = selected.HasValue;
                if (!selected.HasValue) _laneDestination.SelectedIndex = -1;
                else
                {
                    int manual;
                    _laneDestination.SelectedIndex =
                        _settings.ManualDestinations.TryGetValue(selected.Value, out manual)
                        ? manual + 1 : 0;
                }
            }
            finally { _updating = false; }
        }

        private void LaneDestinationChanged(object sender, EventArgs e)
        {
            if (_updating) return;
            TrackLaneKey? lane = SelectedLane();
            DestinationItem item = _laneDestination.SelectedItem as DestinationItem;
            if (!lane.HasValue || item == null) return;
            if (item.Channel < 0) _settings.ManualDestinations.Remove(lane.Value);
            else _settings.ManualDestinations[lane.Value] = item.Channel;
            RefreshPlan(lane);
        }

        private string BuildPreviewText()
        {
            StringBuilder text = new StringBuilder();
            TrackRouteStatistics statistics = _compiled.Statistics;
            text.AppendLine(_compiled.IsValid ? "VALID COMPLETE PLAN" : "PLAN HAS HARD ERRORS");
            text.AppendLine("Automatic tier: " + TierName(_settings.MaximumAutomaticTier));
            text.AppendLine("Routed: " + statistics.RoutedLaneCount.ToString(
                "N0", CultureInfo.CurrentCulture) + " MIDI track/channel entries to " +
                statistics.DestinationCount.ToString("N0", CultureInfo.CurrentCulture) +
                (statistics.DestinationCount == 1 ? " output channel" : " output channels"));
            text.AppendLine("Merged: " + statistics.MergedLaneCount.ToString(
                "N0", CultureInfo.CurrentCulture) +
                (statistics.MergedLaneCount == 1
                    ? " additional entry shares an output channel"
                    : " additional entries share output channels"));
            text.AppendLine("Retimbred: " + statistics.RetimbredLaneCount.ToString(
                "N0", CultureInfo.CurrentCulture) + " entries");
            text.AppendLine("Omitted: " + statistics.OmittedLaneCount.ToString(
                "N0", CultureInfo.CurrentCulture) + " entries / " +
                statistics.OmittedNoteStartCount.ToString("N0", CultureInfo.CurrentCulture) +
                " note starts");
            text.AppendLine("Destination Bank/Program messages generated: " +
                statistics.GeneratedDestinationStateCount.ToString(
                    "N0", CultureInfo.CurrentCulture));
            text.AppendLine("Routed output view: " +
                statistics.IncludedSourceEventCount.ToString("N0", CultureInfo.CurrentCulture) +
                " source messages + " +
                statistics.GeneratedDestinationStateCount.ToString(
                    "N0", CultureInfo.CurrentCulture) + " generated = " +
                statistics.RoutedOutputEventCount.ToString(
                    "N0", CultureInfo.CurrentCulture) + " total");
            text.AppendLine("Apply confirmation: " +
                (_compiled.RequiresExplicitApply ? "required" : "not required"));
            if (_compiled.Errors.Count != 0)
            {
                text.AppendLine();
                text.AppendLine("HARD ERRORS");
                for (int index = 0; index < _compiled.Errors.Count; index++)
                    text.AppendLine("• " + _compiled.Errors[index]);
            }
            for (int destinationIndex = 0;
                destinationIndex < _compiled.Destinations.Count; destinationIndex++)
            {
                TrackRouteDestinationPreview destination = _compiled.Destinations[destinationIndex];
                text.AppendLine();
                text.AppendLine("OUTPUT CHANNEL " +
                    (destination.DestinationChannel + 1).ToString(CultureInfo.CurrentCulture));
                text.AppendLine("Representative: " + destination.RepresentativePatch + " from " +
                    destination.RepresentativeLane);
                for (int assignmentIndex = 0;
                    assignmentIndex < destination.Assignments.Count; assignmentIndex++)
                {
                    TrackRouteAssignment assignment = destination.Assignments[assignmentIndex];
                    text.Append("  • ").Append(assignment.Profile.DisplayName)
                        .Append(" · source ch ")
                        .Append((assignment.Profile.Lane.SourceChannel + 1)
                            .ToString(CultureInfo.CurrentCulture))
                        .Append(" — ").Append(assignment.Manual ? "manual" : TierName(assignment.Tier));
                    if (assignment.RepresentativePatch) text.Append("; representative");
                    text.AppendLine();
                    for (int warning = 0; warning < assignment.Warnings.Count; warning++)
                        text.AppendLine("      WARNING: " + assignment.Warnings[warning]);
                }
            }
            bool wroteOmissions = false;
            for (int assignmentIndex = 0;
                assignmentIndex < _compiled.Assignments.Count; assignmentIndex++)
            {
                TrackRouteAssignment assignment = _compiled.Assignments[assignmentIndex];
                if (!assignment.Omitted) continue;
                if (!wroteOmissions)
                {
                    text.AppendLine();
                    text.AppendLine("OMITTED MIDI TRACKS / CHANNELS — LOSSY CONFIRMATION REQUIRED");
                    wroteOmissions = true;
                }
                text.Append("  • ").Append(assignment.Profile.DisplayName)
                    .Append(" · source ch ")
                    .Append((assignment.Profile.Lane.SourceChannel + 1)
                        .ToString(CultureInfo.CurrentCulture))
                    .Append(" — ").Append(assignment.Profile.AttackCount.ToString(
                        "N0", CultureInfo.CurrentCulture)).AppendLine(
                            assignment.Profile.AttackCount == 1
                                ? " note start omitted" : " note starts omitted");
                for (int warning = 0; warning < assignment.Warnings.Count; warning++)
                    text.AppendLine("      WARNING: " + assignment.Warnings[warning]);
            }
            return text.ToString();
        }

        private void UpdateApplyState()
        {
            bool valid = _compiled != null && _compiled.IsValid;
            bool confirmed = !valid || !_compiled.RequiresExplicitApply || _confirmLossy.Checked;
            _confirmLossy.Visible = valid && _compiled.RequiresExplicitApply;
            _confirmLossy.Enabled = _confirmLossy.Visible;
            _apply.Enabled = valid && confirmed;
            _status.ForeColor = valid ? SystemColors.ControlText : Color.DarkRed;
            _status.Text = !valid
                ? "Resolve every hard error before Apply. The current route remains unchanged."
                : _compiled.RequiresExplicitApply
                    ? "Review all highlighted warnings and confirm before Apply."
                    : "Exact-safe complete plan. Apply changes only this stopped session.";
        }

        private void ApplyClicked(object sender, EventArgs e)
        {
            if (!_apply.Enabled || _compiled == null || !_compiled.IsValid) return;
            SelectedRoute = _compiled;
            SelectedSettings = _settings.Clone();
            ExplicitlyConfirmed = !_compiled.RequiresExplicitApply || _confirmLossy.Checked;
            UseOriginalRouting = false;
            DialogResult = DialogResult.OK;
        }

        private static string TierName(TrackRoutingTier tier)
        {
            return tier == TrackRoutingTier.ExactSafe ? "Exact-safe" :
                tier == TrackRoutingTier.GmFamily ? "GM-family" : "Aggressive";
        }

        internal void SetTargetCapacityForTesting(int value)
        {
            _capacity.Value = Math.Max((int)_capacity.Minimum,
                Math.Min((int)_capacity.Maximum, value));
        }

        internal void SetMaximumTierForTesting(TrackRoutingTier tier)
        {
            _tier.SelectedIndex = (int)tier;
        }

        internal void SetUsableChannelForTesting(int channel, bool usable)
        {
            _usableChannels[channel].Checked = usable;
        }

        internal void SelectLaneForTesting(TrackLaneKey lane)
        {
            for (int index = 0; index < _lanes.Rows.Count; index++)
                if (_lanes.Rows[index].Tag is TrackLaneKey &&
                    ((TrackLaneKey)_lanes.Rows[index].Tag).Equals(lane))
                {
                    _lanes.ClearSelection();
                    _lanes.Rows[index].Selected = true;
                    _lanes.CurrentCell = _lanes.Rows[index].Cells[0];
                    return;
                }
            throw new InvalidOperationException(
                "The requested MIDI track and channel row is not visible.");
        }

        internal void SetSelectedLaneDestinationForTesting(int destinationChannel)
        {
            _laneDestination.SelectedIndex = destinationChannel < 0 ? 0 : destinationChannel + 1;
        }

        internal void SetLossyConfirmationForTesting(bool value)
        {
            _confirmLossy.Checked = value;
        }

        internal void ConfirmApplyForTesting()
        {
            ApplyClicked(_apply, EventArgs.Empty);
        }

        internal void UseOriginalRoutingForTesting()
        {
            if (!_original.Enabled) return;
            _original.PerformClick();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (_toolTips != null) _toolTips.Dispose();
                if (_usedChannelFont != null) _usedChannelFont.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}
