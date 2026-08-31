using System;
using System.Drawing;
using System.Globalization;
using System.Text;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class DiagnosticsForm : Form
    {
        private readonly TextBox _summary;
        private readonly WorkloadGraph _graph;

        internal MidiSong SourceSong { get; private set; }

        public DiagnosticsForm(MidiSong song, WorkloadAnalysis analysis)
        {
            SourceSong = song;
            Text = "MIDI Workload Analysis — " + System.IO.Path.GetFileName(song.FilePath);
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(1080, 660);
            MinimumSize = new Size(820, 520);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

            SplitContainer split = new SplitContainer();
            split.Dock = DockStyle.Fill;
            split.FixedPanel = FixedPanel.Panel1;
            split.SplitterDistance = 400;
            split.Panel1.Padding = new Padding(10);
            split.Panel2.Padding = new Padding(6);

            _summary = new TextBox();
            _summary.Dock = DockStyle.Fill;
            _summary.Multiline = true;
            _summary.ReadOnly = true;
            _summary.ScrollBars = ScrollBars.Vertical;
            _summary.WordWrap = false;
            _summary.Font = new Font("Consolas", 9F, FontStyle.Regular, GraphicsUnit.Point);
            split.Panel1.Controls.Add(_summary);

            _graph = new WorkloadGraph();
            _graph.Dock = DockStyle.Fill;
            split.Panel2.Controls.Add(_graph);
            Controls.Add(split);
            UpdateAnalysis(analysis);
            PerformLayout();
            split.Panel1MinSize = 350;
            split.SplitterDistance = 400;
            Shown += delegate
            {
                _summary.SelectionStart = 0;
                _summary.SelectionLength = 0;
            };
        }

        internal void UpdateAnalysis(WorkloadAnalysis analysis)
        {
            if (analysis == null) throw new ArgumentNullException("analysis");
            int selectionStart = _summary == null ? 0 : _summary.SelectionStart;
            if (_summary != null)
            {
                _summary.Text = BuildSummary(analysis);
                _summary.SelectionStart = Math.Min(selectionStart, _summary.TextLength);
                _summary.SelectionLength = 0;
            }
            if (_graph != null) _graph.Analysis = analysis;
        }

        private static string BuildSummary(WorkloadAnalysis analysis)
        {
            StringBuilder text = new StringBuilder();
            text.AppendLine("WORKLOAD SUMMARY");
            text.AppendLine("Scope: whole file  " + FormatClock(0) + " – " + FormatClock(analysis.DurationMicroseconds));
            text.AppendLine("Aggregation: fixed " + (analysis.BucketMicroseconds / 1000.0).ToString("N0", CultureInfo.CurrentCulture) + " ms buckets");
            text.AppendLine();
            text.AppendLine("Dispatchable events: " + analysis.TotalEvents.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("MIDI message bytes:  " + analysis.TotalBytes.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Unique timestamps:   " + analysis.UniqueTimestamps.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Largest cluster:     " + analysis.LargestTimestampCluster.ToString("N0", CultureInfo.CurrentCulture));
            text.AppendLine("Average events/sec:  " + analysis.AverageEventsPerSecond.ToString("N1", CultureInfo.CurrentCulture));
            text.AppendLine("Peak events/sec:     " + analysis.PeakEventsPerSecond.ToString("N1", CultureInfo.CurrentCulture));
            text.AppendLine("Peak bytes/sec:      " + analysis.PeakBytesPerSecond.ToString("N1", CultureInfo.CurrentCulture));
            if (analysis.Configuration != null)
            {
                AnalysisConfiguration configuration = analysis.Configuration;
                text.AppendLine();
                text.AppendLine("SELECTED SIMULATOR CONFIGURATION");
                text.AppendLine("Simulate slowdown:  " + (configuration.SimulateSlowdown ? "On" : "Off (zero service time)"));
                if (configuration.ServiceDurationMode == ServiceDurationMode.MidiBitrate)
                {
                    text.AppendLine("Service duration:   MIDI bytes at " + configuration.MidiBitrate.ToString("N0", CultureInfo.CurrentCulture) + " bit/s");
                    text.AppendLine("Maximum rate:       " + analysis.ByteServiceCapacityPerSecond.ToString("N1", CultureInfo.CurrentCulture) + " bytes/sec");
                }
                else
                {
                    text.AppendLine("Service duration:   " + configuration.ProcessingMicroseconds.ToString("N0", CultureInfo.CurrentCulture) + " µs/event");
                    text.AppendLine("Maximum rate:       " + (configuration.SimulateSlowdown ? analysis.EventServiceCapacityPerSecond.ToString("N1", CultureInfo.CurrentCulture) + " events/sec" : "Immediate (simulated)"));
                }
                text.AppendLine("Queue length limit: " + (configuration.QueueLengthLimitEnabled ? configuration.QueueLengthLimit.ToString("N0", CultureInfo.CurrentCulture) + " event slots" : "Unlimited"));
                if (configuration.QueueLengthLimitEnabled)
                    text.AppendLine("Overflow policy:    " + FormatOverflowPolicy(configuration.OverflowPolicy));
                text.AppendLine("Predicted peak:     " + analysis.PredictedMaximumOccupancy.ToString("N0", CultureInfo.CurrentCulture) + " outstanding events");
                text.AppendLine("Predicted drops:    " + analysis.PredictedDroppedEvents.ToString("N0", CultureInfo.CurrentCulture));
                if (analysis.PredictedBufferClears > 0)
                    text.AppendLine("Predicted clears:   " + analysis.PredictedBufferClears.ToString("N0", CultureInfo.CurrentCulture));
                text.AppendLine("Prediction is a deterministic simulator projection, not a hardware measurement.");
            }
            text.AppendLine();
            text.AppendLine("EVENTS PARTICIPATING IN CLUSTERS");
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

        private static void AppendCluster(StringBuilder text, string threshold, long count, long total)
        {
            double percent = total == 0 ? 0 : 100.0 * count / total;
            text.AppendLine(threshold.PadRight(8) + count.ToString("N0", CultureInfo.CurrentCulture).PadLeft(12) + "  (" + percent.ToString("N2", CultureInfo.CurrentCulture) + "%)");
        }

        private static string FormatOverflowPolicy(OverflowPolicy policy)
        {
            if (policy == OverflowPolicy.DropOldest) return "Drop oldest pending event";
            if (policy == OverflowPolicy.ClearBufferAndCatchUp) return "Clear buffer and jump to realtime";
            return "Drop newest";
        }

        private static string FormatClock(long microseconds)
        {
            TimeSpan value = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10);
            return ((int)value.TotalMinutes).ToString("00", CultureInfo.InvariantCulture) + ":" +
                value.Seconds.ToString("00", CultureInfo.InvariantCulture) + "." + value.Milliseconds.ToString("000", CultureInfo.InvariantCulture);
        }
    }
}
