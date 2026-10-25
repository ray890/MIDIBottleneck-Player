// WorkloadGraph reference tables: a scripted run of v1's own WorkloadGraph control (viewport,
// zoom, pan, follow, hover, pin, seek events, plot rectangle, timeline ticks) and the
// Playback statistics overlay (bounds, line fitting).
//
// The graph measures text with TextRenderer, which the portable layer cannot do. The script
// therefore also writes every label width v1 measured ("measure" rows: mode d = default flags,
// n = NoPadding|SingleLine, label, width, height); the C++ test feeds those widths back through
// its text-measurement callback, so the tick filtering and the plot rectangle are compared
// exactly. The label candidates are recomputed here with the same arithmetic v1 uses
// (BuildTimelineTicks / GraphArea); a missing width makes the C++ test fail rather than guess.
//
// graph-script.txt rows:
//   op <TAB> text                                  the operation (see GraphRig.Exec)
//   state <TAB> text                               the state after it
//   measure <TAB> d|n <TAB> scale <TAB> label <TAB> w <TAB> h  (at the end of the file, sorted)
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class AnalysisUiReference
    {
        private sealed class GraphRig : IDisposable
        {
            internal readonly WorkloadGraph Graph = new WorkloadGraph();
            internal readonly SortedDictionary<string, string> Measures = new SortedDictionary<string, string>(StringComparer.Ordinal);
            private readonly List<string> _events = new List<string>();
            private readonly Dictionary<string, WorkloadAnalysis> _analyses = new Dictionary<string, WorkloadAnalysis>();
            private readonly Dictionary<string, MidiSong> _songs = new Dictionary<string, MidiSong>();
            private Point _pointer;

            internal GraphRig()
            {
                IntPtr handle = Graph.Handle;   // the dynamic-update timer handler needs a handle
                Graph.ViewportChanged += delegate { _events.Add("V"); };
                Graph.InspectionChanged += delegate(object s, WorkloadSelectionEventArgs e)
                    { _events.Add("I:" + I(e.TimeMicroseconds) + ":" + B(e.Pinned)); };
                Graph.SeekRequested += delegate(object s, WorkloadSelectionEventArgs e)
                    { _events.Add("S:" + I(e.TimeMicroseconds) + ":" + B(e.Pinned)); };
            }

            public void Dispose() { Graph.Dispose(); }

            private Font LabelFont { get { return (Font)Field(Graph, "_labelFont"); } }

            private void Mouse(string method, MouseButtons button, int clicks, int x, int y, int delta)
            {
                MouseEventArgs args = new MouseEventArgs(button, clicks, x, y, delta);
                MethodInfo m = typeof(WorkloadGraph).GetMethod(method, Hidden, null, new[] { typeof(MouseEventArgs) }, null);
                if (m == null) throw new InvalidOperationException("missing " + method);
                m.Invoke(Graph, new object[] { args });
            }

            // One operation; returns the state text.
            internal string Exec(string op)
            {
                string[] f = op.Split(' ');
                _events.Clear();
                string extra = "";
                switch (f[0])
                {
                    case "size": Graph.ClientSize = new Size(Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv)); break;
                    case "scale": Graph.ApplicationScalePercent = Int32.Parse(f[1], Inv); break;
                    case "analysis":
                    {
                        string key = f[1] + " " + f[2] + " " + f[3];
                        WorkloadAnalysis analysis;
                        if (!_analyses.TryGetValue(key, out analysis))
                        {
                            MidiSong song;
                            if (!_songs.TryGetValue(f[1], out song)) _songs[f[1]] = song = LoadFixture(f[1]);
                            analysis = AnalyzeComplete(song, Int64.Parse(f[2], Inv), f[3]);
                            _analyses[key] = analysis;
                        }
                        Graph.Analysis = analysis;
                        break;
                    }
                    case "preview":
                    {
                        // the workload-only graph shown while the projection runs
                        MidiSong song;
                        if (!_songs.TryGetValue(f[1], out song)) _songs[f[1]] = song = LoadFixture(f[1]);
                        Graph.Analysis = AnalyzeVariant(song, Int64.Parse(f[2], Inv), f[3], "pending");
                        break;
                    }
                    case "detach": Graph.DetachAnalysis("detached"); break;
                    case "reset": Graph.ResetZoom(); break;
                    case "zoom": Graph.ZoomAtClientX(Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv)); break;
                    case "pan": Graph.PanByPixels(Int32.Parse(f[1], Inv)); break;
                    case "follow": Graph.FollowTarget = (AnalysisFollowTarget)Int32.Parse(f[1], Inv); break;
                    case "pos": Graph.SetPlaybackPositions(Int64.Parse(f[1], Inv), Int64.Parse(f[2], Inv)); break;
                    case "events": Graph.ShowEventCounts = f[1] == "1"; break;
                    case "move":
                        _pointer = new Point(Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv));
                        Mouse("OnMouseMove", MouseButtons.None, 0, _pointer.X, _pointer.Y, 0);
                        break;
                    case "dragmove":
                        _pointer = new Point(Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv));
                        Mouse("OnMouseMove", MouseButtons.Left, 0, _pointer.X, _pointer.Y, 0);
                        break;
                    case "tick":
                        Thread.Sleep(45);   // beyond the 30 Hz inspection throttle
                        CallInstance(Graph, "ProcessDynamicUpdates", null, EventArgs.Empty);
                        break;
                    case "leave": CallInstance(Graph, "OnMouseLeave", EventArgs.Empty); break;
                    case "down": Mouse("OnMouseDown", MouseButtons.Left, 1, Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv), 0); break;
                    case "up": Mouse("OnMouseUp", MouseButtons.Left, 1, Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv), 0); break;
                    case "dbl": Mouse("OnMouseDoubleClick", MouseButtons.Left, 2, Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv), 0); break;
                    case "wheel": Mouse("OnMouseWheel", MouseButtons.None, 0, Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv), Int32.Parse(f[3], Inv)); break;
                    case "timeat": extra = " timeat=" + I(Graph.TimeAtClientX(Int32.Parse(f[1], Inv))); break;
                    case "bucketat": extra = " bucketat=" + BucketText(Graph.BucketAt(Int64.Parse(f[1], Inv))); break;
                    case "range":
                    {
                        object[] args = { Int32.Parse(f[1], Inv), Graph.GraphArea, 0, 0 };
                        CallInstance(Graph, "BucketRangeAtPixel", args);
                        extra = " range=" + I((int)args[2]) + "," + I((int)args[3]);
                        break;
                    }
                    case "pin":
                    {
                        long time;
                        bool ok = Graph.TryGetPinTime(new Point(Int32.Parse(f[1], Inv), Int32.Parse(f[2], Inv)), out time);
                        extra = " pintime=" + (ok ? I(time) : "-");
                        break;
                    }
                    case "noop": break;
                    default: throw new ArgumentException("script op " + f[0]);
                }
                return State(extra);
            }

            private static string BucketText(WorkloadBucket b)
            {
                if (b == null) return "-";
                return I(b.EventCount) + "," + I(b.ByteCount) + "," + I(b.LargestCluster) + "," + I(b.PredictedPeakOccupancy) + "," +
                    I(b.PredictedPeakQueueAgeMicroseconds) + "," + I(b.PredictedDroppedEvents) + "," + I(b.PredictedBufferClears);
            }

            private static string Opt(long? value) { return value.HasValue ? I(value.Value) : "-"; }

            private void Measure(string mode, string label)
            {
                string key = mode + "\t" + I((int)Field(Graph, "_applicationScalePercent")) + "\t" + label;
                if (Measures.ContainsKey(key)) return;
                Size size = mode == "d"
                    ? TextRenderer.MeasureText(label, LabelFont)
                    : TextRenderer.MeasureText(label, LabelFont, Size.Empty, TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
                Measures[key] = I(size.Width) + "\t" + I(size.Height);
            }

            private string State(string extra)
            {
                StringBuilder s = new StringBuilder();
                long start = Graph.ViewStartMicroseconds, end = Graph.ViewEndMicroseconds;
                s.Append("view=").Append(I(start)).Append("..").Append(I(end));
                // The label widths GraphArea / BuildTimelineTicks ask for (copies of v1's expressions).
                long span = Math.Max(1, end - start);
                int scalePercent = (int)Field(Graph, "_applicationScalePercent");
                int hundred = Math.Max(1, (100 * scalePercent + 50) / 100);
                long gridInterval = WorkloadGraph.ChooseTimelineTickInterval(span, Math.Max(hundred, Graph.ClientSize.Width));
                Measure("d", WorkloadGraph.FormatAxisClock(Math.Max(start, end), gridInterval));
                Rectangle area = Graph.GraphArea;
                s.Append(" area=").Append(I(area.X)).Append(',').Append(I(area.Y)).Append(',').Append(I(area.Width)).Append(',').Append(I(area.Height));
                if (end > start && area.Width > 0)
                {
                    long interval = WorkloadGraph.ChooseTimelineTickInterval(end - start, area.Width);
                    long remainder = start % interval;
                    long first = remainder == 0 ? start : start + (interval - remainder);
                    int guard = 0;
                    for (long time = first; time <= end && guard < 500; time += interval, guard++)
                        Measure("n", WorkloadGraph.FormatAxisClock(time, interval));
                }
                s.Append(" hover=").Append(Opt(Graph.HoverTimeMicroseconds));
                s.Append(" pin=").Append(Opt(Graph.PinnedTimeMicroseconds));
                s.Append(" pos=").Append(Opt(Graph.PlaybackTimelinePosition)).Append(',').Append(Opt(Graph.MidiOutputPosition));
                s.Append(" follow=").Append(I((int)Graph.FollowTarget));
                s.Append(" bytes=").Append(B(Graph.UsesByteRate));
                s.Append(" title=").Append(Graph.EventPanelTitleForTesting);
                s.Append(" peak=").Append(Hex(Graph.EventPanelPeakForTesting));
                s.Append(" ev=").Append(String.Join(",", _events.ToArray()));
                s.Append(" ticks=");
                foreach (TimelineAxisTick tick in Graph.TimelineTicks)
                    s.Append(I(tick.TimeMicroseconds)).Append(':').Append(I(tick.X)).Append(':').Append(tick.Label).Append(':')
                        .Append(I(tick.LabelBounds.Left)).Append(':').Append(I(tick.LabelBounds.Top)).Append(':')
                        .Append(I(tick.LabelBounds.Width)).Append(':').Append(I(tick.LabelBounds.Height)).Append(';');
                s.Append(extra);
                return s.ToString();
            }
        }

        // ---------------------------------------------------------------- graph-script.txt

        private static List<string> BuildGraphScript()
        {
            List<string> ops = new List<string>();
            string queue = QueueCfg(0, 250, 31250, 10000, true, 8, 0, false, 1000000, true, true, false, "-");
            string bytes = QueueCfg(1, 100, 31250, 10000, true, 8, 0, true, 5000, true, true, false, "-");
            string gate = GateCfg("pn", 1000, 0, true, true, 5, "-");
            string plain = QueueCfg(0, 100, 31250, 10000, false, 2000, 0, false, 1000000, false, true, false, "-");
            string dense = GateCfg("or", 100, 1000, false, false, 128, "-");
            // Part 1: one analysis, every navigation operation.
            ops.Add("size 900 520");
            ops.Add("noop");
            ops.Add("analysis models-notes 100000 " + queue);
            ops.Add("reset");
            string[] nav = { "zoom 450 120", "zoom 450 120", "zoom 200 -120", "zoom 1000 240", "zoom 450 0", "zoom 60 360", "zoom 700 -240",
                "pan 50", "pan -300", "pan 100000", "pan -100000", "reset", "zoom 100 1200", "zoom 5 -1200", "zoom 899 120",
                "follow 1", "pos 500000 600000", "pos 5000000 4900000", "pos 100 100", "follow 2", "pos 2000000 3000000", "pos 60000000 0",
                "follow 0", "pos 0 0", "events 1", "events 0", "range 0", "range 10", "range 300", "range 649", "range 5000",
                "timeat 0", "timeat 50", "timeat 400", "timeat 1000", "bucketat -5", "bucketat 0", "bucketat 150000", "bucketat 99999999999",
                "pin 10 10", "pin 100 100", "pin 100 300", "pin 40 100", "pin 44 100", "pin 840 100", "pin 849 100", "pin 860 100" };
            foreach (string op in nav) ops.Add(op);
            // Scales and sizes.
            string[] scales = { "scale 50", "scale 75", "scale 100", "scale 125", "scale 150", "scale 200", "scale 100" };
            foreach (string s in scales) { ops.Add(s); ops.Add("noop"); ops.Add("zoom 300 240"); ops.Add("reset"); }
            string[] sizes = { "size 640 400", "size 300 300", "size 1300 700", "size 820 520", "size 1920 900", "size 450 330", "size 900 520" };
            foreach (string s in sizes) { ops.Add(s); ops.Add("zoom 300 240"); ops.Add("reset"); }
            // Part 2: the pointer.
            ops.Add("size 900 520");
            ops.Add("reset");
            string[] mouse = { "move 400 100", "tick", "move 450 120", "tick", "move 455 120", "move 20 20", "tick", "down 450 120", "up 450 120",
                "move 600 100", "tick", "leave", "down 500 100", "dragmove 520 100", "dragmove 560 100", "dragmove 620 100", "up 620 100",
                "zoom 450 360", "zoom 450 360", "down 500 100", "dragmove 502 100", "dragmove 503 100", "up 503 100", "down 500 100", "dragmove 480 100",
                "dragmove 300 100", "dragmove 700 100", "up 700 100", "dbl 450 100", "down 10 100", "dragmove 100 100", "up 100 100",
                "down 450 20", "up 450 20", "down 860 100", "up 860 100", "wheel 450 100 120", "wheel 450 100 -120", "wheel 450 100 240",
                "wheel 450 500 120", "wheel 10 10 120", "wheel 450 100 0", "move 460 100", "tick", "leave", "leave", "move 500 400", "tick" };
            foreach (string op in mouse) ops.Add(op);
            // Part 3: other analyses (preview, gate projection, byte-rate model, plain), replacement and detach.
            string[] others = {
                "analysis models-notes 100000 " + gate, "reset", "events 1", "pos 1000000 900000",
                "analysis models-notes 100000 " + bytes, "events 0", "zoom 450 240",
                "analysis models-dense 50000 " + dense, "zoom 450 360", "pan 200",
                "analysis models-notes 25000 " + plain,
                "preview models-notes 100000 " + queue, "preview models-notes 100000 " + gate,
                "analysis f0-all-message-types 100000 " + queue, "reset", "analysis f0-all-message-types 50000 " + gate, "detach", "noop",
                "analysis models-notes 100000 " + queue, "zoom 450 240", "analysis tempo-changes 100000 " + queue, "detach", "zoom 450 120", "pan 20" };
            foreach (string op in others) ops.Add(op);
            // Part 4: seeded random walk on two analyses.
            Prng r = new Prng(20261014);
            string[] fixtures = { "models-notes", "models-dense" };
            ops.Add("analysis models-notes 100000 " + queue);
            string[] simple = { "reset", "noop", "follow 1", "follow 2", "follow 0", "events 1", "events 0" };
            for (int i = 0; i < 260; i++)
            {
                long kind = r.Below(12);
                if (kind <= 2) ops.Add("zoom " + I(r.Range(-20, 920)) + " " + I(r.Range(-5, 5) * 120));
                else if (kind <= 4) ops.Add("pan " + I(r.Range(-400, 400)));
                else if (kind == 5) ops.Add("pos " + I(r.Range(0, 9000000)) + " " + I(r.Range(0, 9000000)));
                else if (kind == 6) ops.Add(simple[r.Below(simple.Length)]);
                else if (kind == 7) ops.Add("size " + I(r.Range(300, 1500)) + " " + I(r.Range(300, 800)));
                else if (kind == 8)
                {
                    string fixture = fixtures[r.Below(2)];
                    ops.Add("analysis " + fixture + " " + (fixture == "models-notes" ? "100000 " : "50000 ") + (r.Below(2) == 0 ? queue : gate));
                }
                else if (kind == 9) { ops.Add("move " + I(r.Range(0, 900)) + " " + I(r.Range(0, 520))); ops.Add("tick"); }
                else if (kind == 10) { long x = r.Range(0, 900), y = r.Range(0, 520); ops.Add("down " + I(x) + " " + I(y)); ops.Add("up " + I(x + r.Range(0, 3)) + " " + I(y)); }
                else ops.Add("scale " + I(new[] { 50, 75, 100, 125, 150, 200 }[r.Below(6)]));
            }
            return ops;
        }

        private static void WriteGraphScript()
        {
            List<string> ops = BuildGraphScript();
            using (GraphRig rig = new GraphRig())
            using (Table t = new Table("graph-script.txt"))
            {
                t.Comment("op/state pairs from a WorkloadGraph run; measure rows (label widths v1 measured) follow");
                foreach (string op in ops)
                {
                    string state;
                    try { state = rig.Exec(op); }
                    catch (Exception ex) { state = "exception " + ex.GetType().Name + ": " + ex.Message; }
                    t.Row("op", op);
                    t.Row("state", state);
                }
                foreach (KeyValuePair<string, string> pair in rig.Measures)
                {
                    string[] parts = pair.Key.Split('\t');
                    t.Row("measure", parts[0], parts[1], parts[2], pair.Value.Split('\t')[0], pair.Value.Split('\t')[1]);
                }
            }
        }

        // ---------------------------------------------------------------- overlay.txt

        private static void WriteOverlayTables()
        {
            string[] captions = { "Playback: ", "Timeline / output: ", "Queue now / maximum: ", "Events sent / dropped: ", "Output rate: ",
                "Effective speed: ", "Lag current / maximum: " };
            string[] shorts = { "State: ", "Time: ", "Queue: ", "Events: ", "Rate: ", "Speed: ", "Lag: " };
            string[] values = { "Playing", "Paused", "01:23.456 / 01:23.400", "12,345 / 100,000", "1,234,567 / 12,345", "123.4 events/sec",
                "—", "99.9%", "0.250 ms / 12.500 ms", "10,000,000 / 200,000,000", "5,000 / 10,000 actual backlog", "12,345 events/sec",
                "1,000,000,000 / 2,500,000,000", "", "250.0%" };
            int[] widths = { 60, 100, 150, 220, 296 };
            using (GraphRig rig = new GraphRig())
            using (Table t = new Table("overlay.txt"))
            {
                t.Comment("bounds\tscale\tareaW\tareaH\tx\ty\tw\th   |   fit\tcaption\tshort\tvalue\twidth\tresult   |   measure\ttext\tw");
                Graphics g = Graphics.FromImage(new Bitmap(4, 4));
                int[] scales = { 50, 100, 150, 200 };
                foreach (int scale in scales)
                {
                    rig.Graph.ApplicationScalePercent = scale;
                    foreach (int areaWidth in new[] { 20, 100, 200, 300, 330, 400, 640, 1000 })
                    {
                        Rectangle r = rig.Graph.PlaybackStatisticsBounds(new Rectangle(44, 42, areaWidth, 300));
                        t.Row("bounds", I(scale), I(areaWidth), "300", I(r.X), I(r.Y), I(r.Width), I(r.Height));
                    }
                    Font font = (Font)Field(rig.Graph, "_labelFont");
                    SortedDictionary<string, int> measured = new SortedDictionary<string, int>(StringComparer.Ordinal);
                    for (int i = 0; i < captions.Length; i++)
                        foreach (string value in values)
                            foreach (int width in widths)
                            {
                                int w = width * scale / 100;
                                // candidates, with the same string operations FitPlaybackLine applies
                                string compact = (value ?? "").Replace(" / ", "/").Replace(" events/sec", " ev/s");
                                string abbreviated = (string)CallStatic(typeof(WorkloadGraph), "AbbreviatePlaybackCounts", compact);
                                string[] candidates = { captions[i] + value, captions[i] + compact, captions[i] + abbreviated, shorts[i] + abbreviated };
                                foreach (string candidate in candidates)
                                    if (!measured.ContainsKey(candidate))
                                        measured[candidate] = TextRenderer.MeasureText(g, candidate, font, Size.Empty,
                                            TextFormatFlags.NoPadding | TextFormatFlags.SingleLine).Width;
                                string fit = rig.Graph.FitPlaybackLineForTesting(g, captions[i], shorts[i], value, w);
                                t.Row("fit", I(scale), captions[i], shorts[i], value, I(w), fit);
                            }
                    foreach (KeyValuePair<string, int> pair in measured) t.Row("measure", I(scale), pair.Key, I(pair.Value));
                }
                g.Dispose();
            }
        }
    }
}
