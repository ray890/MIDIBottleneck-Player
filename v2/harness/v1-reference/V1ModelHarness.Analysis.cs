// v1 Analysis and track-routing traces (Milestone 4). Drives v1's own
// WorkloadAnalyzer, TrackRoutePlanner, TrackRoutedEventStore and
// TrackRoutingForm (created, never shown), unchanged.
//
//   models analysis <fixture-dir> <out-trace>
//   models routing  <fixture-dir> <out-trace>
//
// Ops (TRACE_FORMAT.md, "Analysis and routing traces"):
//   > bucket-count <duration> <bucket>     WorkloadAnalyzer.CalculateBucketCount
//   > default-resolution <duration>        WorkloadAnalyzer.DefaultResolution
//   > analyze <bucket> <config>            WorkloadAnalyzer.Analyze (per-song base-workload cache)
//   > uncached <bucket> <config>           WorkloadAnalyzer.AnalyzeUncached
//   > project <gate> <bucket> <config>     one gate projection with its emission observer
//   > cache-clear                          drops the song's base-workload cache
//   > route <settings>                     TrackRoutingForm over TrackRoutePlanner.Compile
//   > routed-view                          TrackRouteApplication.CreateRoutedSong (last route)
//   > routed-analyze <bucket> <config>     Analyze over the routed song
// Doubles are written as their IEEE-754 bit patterns (16 hex digits).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class V1ModelHarness
    {
        // ------------------------------------------------------------ text

        private static string Hx(double value)
        {
            return BitConverter.DoubleToInt64Bits(value).ToString("x16", Inv);
        }

        // ASCII-safe text: anything outside 0x20-0x7E, and the backslash, as \uXXXX.
        private static string Esc(string text)
        {
            if (text == null) return "<null>";
            StringBuilder b = new StringBuilder(text.Length);
            foreach (char c in text)
            {
                if (c < 0x20 || c > 0x7E || c == '\\') b.Append("\\u").Append(((int)c).ToString("X4", Inv));
                else b.Append(c);
            }
            return b.ToString();
        }

        private sealed class Fnv
        {
            internal ulong Hash = 14695981039346656037UL;
            internal void Add(string text)
            {
                foreach (char c in text) { Hash ^= (byte)c; Hash *= 1099511628211UL; }
            }
            internal string Text { get { return Hash.ToString("x16", Inv); } }
        }

        // ------------------------------------------------------------ configuration

        private static readonly long[] ProcValues = { 0, 1, 7, 250, 1000, 5000, 40000 };
        private static readonly long[] BitValues = { 1, 300, 3125, 31250, 1000000 };
        private static readonly long[] EpsValues = { 0, 1, 3, 500, 1000, 20000, 9999999 };
        private static readonly int[] QueueLimits = { 1, 2, 3, 8, 32, 500, 100000 };
        private static readonly long[] AgeLimits = { 1, 900, 5000, 60000 };
        private static readonly long[] GateIntervals = { 1, 250, 1000, 20000, 250000 };
        private static readonly int[] PolyLimits = { -3, 0, 1, 2, 5, 128, 300 };
        private static readonly long[] RateValues = { 0, 1, 5, 30, 1000, 9999999 };

        private static string RandomOverride()
        {
            int pick = Pick(10);
            if (pick < 6) return "-";
            if (pick == 6) return "none";
            if (pick == 7) return "3.4=100;3.5=20;5.6=1;5.7=-8192;5.8=9";   // conflicts with CC121 resets
            List<string> parts = new List<string>();
            int count = 1 + Pick(4);
            for (int i = 0; i < count; i++)
            {
                int channel = Pick(16);
                int attribute = Pick(9);
                int value = attribute == 7 ? Pick(16384) - 8192 : attribute == 6 ? Pick(2) : Pick(128);
                string key = S(channel) + "." + S(attribute) + "=";
                bool duplicate = false;
                foreach (string part in parts) if (part.StartsWith(key, StringComparison.Ordinal)) duplicate = true;
                if (!duplicate) parts.Add(key + S(value));
            }
            parts.Sort(CompareOverrideParts);
            return String.Join(";", parts.ToArray());
        }

        private static int CompareOverrideParts(string left, string right)
        {
            string[] l = left.Split('.', '='), r = right.Split('.', '=');
            int c = Int32.Parse(l[0], Inv).CompareTo(Int32.Parse(r[0], Inv));
            return c != 0 ? c : Int32.Parse(l[1], Inv).CompareTo(Int32.Parse(r[1], Inv));
        }

        private static string RandomConfiguration()
        {
            int kind = Pick(12);
            bool sim = kind <= 5 || kind == 11 && Pick(2) == 0;
            int mode = Pick(3);
            long proc = ProcValues[Pick(ProcValues.Length)];
            long bit = BitValues[Pick(BitValues.Length)];
            long eps = EpsValues[Pick(EpsValues.Length)];
            bool ql = kind >= 2 && kind <= 6 || kind == 11 && Pick(2) == 0;
            int qlLimit = QueueLimits[Pick(QueueLimits.Length)];
            bool qa = Pick(3) == 0;
            long qaLimit = AgeLimits[Pick(AgeLimits.Length)];
            int policy = Pick(6);
            bool pn = kind == 7, dn = kind == 8, na = kind == 9, or = kind == 10;
            if (pn || dn) proc = GateIntervals[Pick(GateIntervals.Length)];
            if (na || or) eps = Pick(12) == 0 ? (Pick(2) == 0 ? -1 : 10000000) : RateValues[Pick(RateValues.Length)];
            bool scale = Pick(2) == 0;
            bool ip = Pick(2) == 0;
            int ipLimit = PolyLimits[Pick(PolyLimits.Length)];
            bool paced = Pick(2) == 0;
            bool aq = kind == 6 ? true : Pick(4) != 0;
            if (kind == 6) { sim = false; ql = true; policy = Pick(4) == 0 ? Pick(6) : (Pick(2) == 0 ? 0 : 3); }
            return Cfg(sim, mode, proc, bit, eps, ql, qlLimit, qa, qaLimit, policy, pn, dn, scale, ip, ipLimit, na, or,
                paced, aq, RandomOverride());
        }

        private static string Cfg(bool sim, int mode, long proc, long bit, long eps, bool ql, int qlLimit, bool qa,
            long qaLimit, int policy, bool pn, bool dn, bool scale, bool ip, int ipLimit, bool na, bool or, bool paced,
            bool aq, string ovr)
        {
            return "sim=" + B(sim) + ",mode=" + S(mode) + ",proc=" + S(proc) + ",bit=" + S(bit) + ",eps=" + S(eps) +
                ",ql=" + B(ql) + ":" + S(qlLimit) + ",qa=" + B(qa) + ":" + S(qaLimit) + ",pol=" + S(policy) +
                ",pn=" + B(pn) + ",dn=" + B(dn) + ",scale=" + B(scale) + ",ip=" + B(ip) + ":" + S(ipLimit) +
                ",na=" + B(na) + ",or=" + B(or) + ",paced=" + B(paced) + ",aq=" + B(aq) + ",ovr=" + ovr;
        }

        private static AnalysisConfiguration ParseCfg(string text)
        {
            if (text == "null") return null;
            AnalysisConfiguration c = new AnalysisConfiguration();
            foreach (string part in text.Split(','))
            {
                int eq = part.IndexOf('=');
                string key = part.Substring(0, eq), value = part.Substring(eq + 1);
                string[] pair = value.Split(':');
                switch (key)
                {
                    case "sim": c.SimulateSlowdown = value == "1"; break;
                    case "mode": c.ServiceDurationMode = (ServiceDurationMode)Int32.Parse(value, Inv); break;
                    case "proc": c.ProcessingMicroseconds = Int64.Parse(value, Inv); break;
                    case "bit": c.MidiBitrate = Int64.Parse(value, Inv); break;
                    case "eps": c.EventsPerSecond = Int64.Parse(value, Inv); break;
                    case "ql": c.QueueLengthLimitEnabled = pair[0] == "1"; c.QueueLengthLimit = Int32.Parse(pair[1], Inv); break;
                    case "qa": c.QueueAgeLimitEnabled = pair[0] == "1"; c.QueueAgeLimitMicroseconds = Int64.Parse(pair[1], Inv); break;
                    case "pol": c.OverflowPolicy = (OverflowPolicy)Int32.Parse(value, Inv); break;
                    case "pn": c.PerNoteIntervalGateEnabled = value == "1"; break;
                    case "dn": c.DelayedNoteOffEnabled = value == "1"; break;
                    case "scale": c.ScaleNoteIntervalByTraffic = value == "1"; break;
                    case "ip": c.IntervalPolyphonyLimitEnabled = pair[0] == "1"; c.IntervalPolyphonyLimit = Int32.Parse(pair[1], Inv); break;
                    case "na": c.NoteAttackLimitEnabled = value == "1"; break;
                    case "or": c.OutputRateLimitEnabled = value == "1"; break;
                    case "paced": c.OutputPacedTimeline = value == "1"; break;
                    case "aq": c.ApplyQueueLimitWithoutSlowdown = value == "1"; break;
                    case "ovr": c.OverrideProjection = ParseOverride(value); break;
                    default: throw new ArgumentException("unknown configuration key " + key);
                }
            }
            return c;
        }

        private static AnalysisOverrideSnapshot ParseOverride(string text)
        {
            if (text == "-") return null;
            int[] values = new int[16 * ChannelOverrideState.AttributeCount];
            for (int i = 0; i < values.Length; i++) values[i] = ChannelOverrideState.AutoValue;
            if (text != "none")
                foreach (string part in text.Split(';'))
                {
                    string[] f = part.Split('.', '=');
                    values[Int32.Parse(f[0], Inv) * ChannelOverrideState.AttributeCount + Int32.Parse(f[1], Inv)] =
                        Int32.Parse(f[2], Inv);
                }
            return new AnalysisOverrideSnapshot(values);
        }

        // ------------------------------------------------------------ analysis output

        private static string BucketText(WorkloadBucket b)
        {
            return S(b.EventCount) + "," + S(b.ByteCount) + "," + S(b.LargestCluster) + "," +
                S(b.ServiceDemandMicroseconds) + "," + S(b.PredictedPeakOccupancy) + "," +
                S(b.PredictedPeakQueueAgeMicroseconds) + "," + S(b.PredictedDroppedEvents) + "," +
                S(b.PredictedBufferClears);
        }

        private static string AnalysisText(WorkloadAnalysis a, string ready)
        {
            List<string> lines = new List<string>();
            lines.Add("state " + S((int)a.ProjectionState) + " gate " + B(a.HasGateProjection) + " queue " +
                B(a.HasQueueProjection) + " failure " + (a.ProjectionFailureReason == null ? "-" : Esc(a.ProjectionFailureReason)) +
                (ready == null ? "" : " ready " + ready));
            lines.Add("totals events " + S(a.TotalEvents) + " bytes " + S(a.TotalBytes) + " unique " + S(a.UniqueTimestamps) +
                " largest " + S(a.LargestTimestampCluster) + " c2 " + S(a.EventsInClustersAtLeast2) + " c10 " +
                S(a.EventsInClustersAtLeast10) + " c50 " + S(a.EventsInClustersAtLeast50) + " c100 " +
                S(a.EventsInClustersAtLeast100) + " bucket " + S(a.BucketMicroseconds) + " duration " + S(a.DurationMicroseconds));
            lines.Add("rates avg " + Hx(a.AverageEventsPerSecond) + " peak-ev " + Hx(a.PeakEventsPerSecond) + " peak-bytes " +
                Hx(a.PeakBytesPerSecond) + " cap-ev " + Hx(a.EventServiceCapacityPerSecond) + " cap-bytes " +
                Hx(a.ByteServiceCapacityPerSecond) + " gate-peak " + Hx(a.GatePeakEventsPerSecond));
            lines.Add("predicted occ " + S(a.PredictedMaximumOccupancy) + " age " + S(a.PredictedPeakQueueAgeMicroseconds) +
                " dropped " + S(a.PredictedDroppedEvents) + " clears " + S(a.PredictedBufferClears) + " completion " +
                S(a.PredictedOutputCompletionMicroseconds));
            lines.Add("gate events " + S(a.GateOutputEvents) + " notes " + S(a.GateOutputNoteTransitions) + " other " +
                S(a.GateOutputNonNoteEvents) + " filtered " + S(a.GateFilteredNoteEvents) + " musical " +
                S(a.MusicalLimitRejectedAttacks) + " " + S(a.MusicalLimitSuppressedReleases) + " outrate " +
                S(a.OutputRateRejectedAttacks) + " " + S(a.OutputRateSuppressedReleases) + " " +
                S(a.OutputRateRejectedOtherMessages) + " " + S(a.OutputRateSafetySoftExcess) + " override " +
                S(a.OverrideProjectedForcedMessages) + " " + S(a.OverrideFilteredSourceMessages) + " completion " +
                S(a.GateOutputCompletionMicroseconds));
            StringBuilder types = new StringBuilder("types");
            if (a.MessageTypes == null) types.Append(" null");
            else foreach (MessageTypeWorkload t in a.MessageTypes)
                    types.Append(' ').Append(S((int)t.Kind)).Append(':').Append(S(t.EventCount)).Append(':').Append(S(t.ByteCount));
            lines.Add(types.ToString());
            Fnv digest = new Fnv();
            StringBuilder sample = new StringBuilder();
            int shown = 0;
            for (int i = 0; i < a.Buckets.Length; i++)
            {
                string text = BucketText(a.Buckets[i]);
                digest.Add(text);
                digest.Add(";");
                if (shown < 6 && text != "0,0,0,0,0,0,0,0") { sample.Append(' ').Append(S(i)).Append('=').Append(text); shown++; }
            }
            lines.Add("buckets " + S(a.Buckets.Length) + " " + digest.Text + sample);
            if (a.GateOutputBuckets == null) lines.Add("gate-buckets null");
            else
            {
                Fnv gate = new Fnv();
                StringBuilder gateSample = new StringBuilder();
                int gateShown = 0;
                for (int i = 0; i < a.GateOutputBuckets.Length; i++)
                {
                    gate.Add(S(a.GateOutputBuckets[i]));
                    gate.Add(",");
                    if (gateShown < 6 && a.GateOutputBuckets[i] != 0)
                    { gateSample.Append(' ').Append(S(i)).Append('=').Append(S(a.GateOutputBuckets[i])); gateShown++; }
                }
                lines.Add("gate-buckets " + S(a.GateOutputBuckets.Length) + " " + gate.Text + gateSample);
            }
            return String.Join("\n= ", lines.ToArray());
        }

        private static List<string> _progress;

        private static Action<WorkloadAnalysisProgress> ProgressRecorder()
        {
            _progress = new List<string>();
            return p => _progress.Add(p.Stage + "|" + S(p.OverallPermille) + "|" + S(p.StagePermille));
        }

        private static string ProgressText()
        {
            Fnv digest = new Fnv();
            StringBuilder runs = new StringBuilder();
            string last = null;
            int run = 0;
            foreach (string entry in _progress)
            {
                digest.Add(entry);
                digest.Add(";");
                string stage = entry.Substring(0, entry.IndexOf('|'));
                if (stage == last) { run++; continue; }
                if (last != null) runs.Append(last).Append('*').Append(S(run)).Append('/');
                last = stage;
                run = 1;
            }
            if (last != null) runs.Append(last).Append('*').Append(S(run));
            return "progress " + S(_progress.Count) + " " + digest.Text + " " + runs;
        }

        private static string AnalyzeOp(MidiSong song, long bucket, string cfg)
        {
            AnalysisConfiguration configuration = ParseCfg(cfg);
            string ready = null;
            WorkloadAnalysis result = WorkloadAnalyzer.Analyze(song, bucket, configuration, CancellationToken.None,
                ProgressRecorder(), w => ready = S(w.BucketMicroseconds));
            return AnalysisText(result, ready);
        }

        private static void AnalyzeCall(string op, MidiSong song, long bucket, string cfg)
        {
            _progress = new List<string>();
            Op(op + " " + S(bucket) + " " + cfg, () => AnalyzeOp(song, bucket, cfg), ProgressText);
        }

        private static string UncachedOp(long bucket, string cfg)
        {
            AnalysisConfiguration configuration = ParseCfg(cfg);
            WorkloadAnalysis result = WorkloadAnalyzer.AnalyzeUncached(_song, bucket, configuration,
                CancellationToken.None, ProgressRecorder());
            return AnalysisText(result, null);
        }

        private static string ProjectOp(string gate, long bucket, string cfg)
        {
            AnalysisConfiguration configuration = ParseCfg(cfg);
            Action<WorkloadAnalysisProgress> progress = ProgressRecorder();
            WorkloadAnalysis result = WorkloadAnalyzer.AnalyzeUncached(_song, bucket, null, CancellationToken.None, null)
                .CopyWorkload(configuration, CancellationToken.None);
            Fnv digest = new Fnv();
            StringBuilder sample = new StringBuilder();
            int count = 0;
            Action<MidiEventView, long> observer = (e, time) =>
            {
                string text = Ev(e) + "@" + S(time);
                digest.Add(text);
                digest.Add(";");
                if (count < 4) sample.Append(' ').Append(text);
                count++;
            };
            if (gate == "pernote") WorkloadAnalyzer.ProjectPerNoteGate(_song, result, configuration, CancellationToken.None, progress, observer);
            else if (gate == "delayed") WorkloadAnalyzer.ProjectDelayedNoteOff(_song, result, configuration, CancellationToken.None, progress, observer);
            else if (gate == "attack") WorkloadAnalyzer.ProjectNoteAttackLimit(_song, result, configuration, CancellationToken.None, progress, observer);
            else WorkloadAnalyzer.ProjectOutputRateLimit(_song, result, configuration, CancellationToken.None, progress, observer);
            return "emitted " + S(count) + " " + digest.Text + sample + "\n= " + AnalysisText(result, null);
        }

        private static void CacheClear(MidiSong song)
        {
            Line("> cache-clear");
            FieldInfo field = typeof(WorkloadAnalyzer).GetField("Workloads", Hidden);
            object table = field.GetValue(null);
            table.GetType().GetMethod("Remove").Invoke(table, new object[] { song });
        }

        // ------------------------------------------------------------ analysis family

        private static void AnalysisFamily()
        {
            Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
            Thread.CurrentThread.CurrentUICulture = CultureInfo.InvariantCulture;
            _rng = 0x2F6B7D31;
            Section("bucket-count");
            long[][] counts =
            {
                new long[] { 0, 1 }, new long[] { 1999998, 1 }, new long[] { 1999999, 1 }, new long[] { 2000000, 1 },
                new long[] { 5, 0 }, new long[] { 5, -1 }, new long[] { -5, 1 }, new long[] { -1, 2 },
                new long[] { Int64.MaxValue, 1 }, new long[] { Int64.MaxValue, 2 }, new long[] { Int64.MinValue, 1 },
                new long[] { 100, 3 }, new long[] { 399999999, 200 }, new long[] { 400000000, 200 },
                new long[] { 3999999999999, 2000000 }, new long[] { 4000000000000, 2000000 }
            };
            foreach (long[] c in counts)
            {
                long duration = c[0], bucket = c[1];
                Op("bucket-count " + S(duration) + " " + S(bucket),
                    () => S(WorkloadAnalyzer.CalculateBucketCount(duration, bucket)), null);
            }
            long[] durations = { 0, 499999999, 500000000, 500000001, 1000000000000, Int64.MaxValue - 4998, Int64.MaxValue, -10 };
            MethodInfo defaultResolution = typeof(WorkloadAnalyzer).GetMethod("DefaultResolution", Hidden);
            foreach (long duration in durations)
            {
                long d = duration;
                Op("default-resolution " + S(d), () =>
                {
                    MidiSong probe = new MidiSong { DurationMicroseconds = d, Events = new List<MidiEvent>() };
                    return S((long)defaultResolution.Invoke(null, new object[] { probe }));
                }, null);
            }

            string[] fixtures = { "models-notes.mid", "models-dense.mid", "index-chase-rich.mid", "many-tracks-ties.mid",
                "tempo-changes.mid", "empty-tracks.mid", "sysex-variants.mid", "routing-lanes.mid", "f1-v1-test-midi.mid" };
            foreach (string fixture in fixtures)
            {
                Section("fixture " + fixture);
                LoadFixture(fixture);
                AnalysisRuns(fixture);
            }
        }

        private static long PickBucket(long duration)
        {
            int pick = Pick(6);
            if (pick == 0) return WorkloadAnalyzerDefault(duration);
            if (pick == 1) return Math.Max(7, duration / 20000 + 1);
            if (pick == 2) return 1000;
            if (pick == 3) return 100000;
            if (pick == 4) return 1000000000000;
            return 25000;
        }

        private static long WorkloadAnalyzerDefault(long duration)
        {
            MethodInfo defaultResolution = typeof(WorkloadAnalyzer).GetMethod("DefaultResolution", Hidden);
            return (long)defaultResolution.Invoke(null, new object[] { _song });
        }

        private static void AnalysisRuns(string fixture)
        {
            long duration = _song.DurationMicroseconds;
            long dflt = WorkloadAnalyzerDefault(duration);
            string ovrMix = "0.3=90;0.2=5;1.7=0;9.6=1;2.0=1;2.1=3;4.4=127;4.5=64";
            string[] fixedConfigs =
            {
                "null",
                Cfg(true, 0, 250, 31250, 1000, false, 32, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, 250, 31250, 1000, false, 32, false, 5000, 0, false, false, false, false, 128, false, false, true, true, "-"),
                Cfg(true, 1, 0, 31250, 1000, false, 32, false, 5000, 0, false, false, false, false, 128, false, false, true, true, "-"),
                Cfg(true, 2, 0, 31250, 1000, false, 32, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 2, 0, 31250, 0, false, 32, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, 0, 31250, 1000, true, 8, false, 5000, 1, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, 1000, 31250, 1000, true, 3, true, 900, 1, false, false, false, false, 128, false, false, true, true, "-"),
                Cfg(true, 0, 1000, 31250, 1000, true, 3, true, 900, 2, false, false, false, false, 128, false, false, true, true, "-"),
                Cfg(true, 1, 0, 3125, 1000, true, 4, true, 5000, 4, false, false, false, false, 128, false, false, true, true, "-"),
                Cfg(true, 1, 0, 3125, 1000, true, 4, false, 5000, 5, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 2, 0, 31250, 500, true, 2, false, 5000, 3, false, false, false, false, 128, false, false, false, true, ovrMix),
                Cfg(true, 0, 5000, 31250, 1000, true, 1, false, 5000, 2, false, false, false, false, 128, false, false, true, true, "none"),
                Cfg(false, 0, 1000, 31250, 1000, true, 4, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(false, 2, 0, 31250, 300, true, 4, false, 5000, 3, false, false, false, false, 128, false, false, false, true, ovrMix),
                Cfg(false, 0, 1000, 31250, 1000, true, 4, false, 5000, 1, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(false, 0, 1000, 31250, 1000, true, 4, false, 5000, 1, false, false, false, false, 128, false, false, false, false, "-"),
                Cfg(false, 1, 0, 31250, 1000, false, 4, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, 1000, 31250, 1000, false, 32, false, 5000, 0, true, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, 20000, 31250, 1000, false, 32, false, 5000, 0, true, false, true, true, 2, false, false, false, true, ovrMix),
                Cfg(true, 0, 1000, 31250, 1000, false, 32, false, 5000, 0, false, true, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, 20000, 31250, 1000, false, 32, false, 5000, 0, false, true, true, true, 1, false, false, false, true, ovrMix),
                Cfg(true, 0, 1000, 31250, 30, false, 32, false, 5000, 0, false, false, false, false, 128, true, false, false, true, "-"),
                Cfg(true, 0, 1000, 31250, 30, false, 32, false, 5000, 0, false, false, false, false, 128, false, true, false, true, ovrMix),
                Cfg(true, 0, 1000, 31250, -1, false, 32, false, 5000, 0, false, false, false, false, 128, true, false, false, true, "-"),
                Cfg(true, 0, 1000, 31250, 10000000, false, 32, false, 5000, 0, false, false, false, false, 128, false, true, false, true, "-"),
                Cfg(true, 0, -5, 31250, 1000, false, 32, false, 5000, 0, true, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, Int64.MaxValue / 2, 31250, 1000, true, 4, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "-"),
                Cfg(true, 0, Int64.MaxValue / 2, 31250, 1000, false, 4, false, 5000, 0, false, false, false, false, 128, false, false, false, true, "none"),
            };
            foreach (string cfg in fixedConfigs)
            {
                AnalyzeCall("analyze", _song, Pick(2) == 0 ? dflt : PickBucket(duration), cfg);
            }
            for (int i = 0; i < 36; i++)
            {
                if (i == 18) CacheClear(_song);
                AnalyzeCall("analyze", _song, PickBucket(duration), RandomConfiguration());
            }
            for (int i = 0; i < 8; i++)
            {
                long bucket = PickBucket(duration);
                string cfg = i == 0 ? "null" : RandomConfiguration();
                _progress = new List<string>();
                Op("uncached " + S(bucket) + " " + cfg, () => UncachedOp(bucket, cfg), ProgressText);
            }
            string[] gates = { "pernote", "delayed", "attack", "outputrate" };
            foreach (string gate in gates)
            {
                for (int i = 0; i < 3; i++)
                {
                    long bucket = PickBucket(duration);
                    bool pn = gate == "pernote", dn = gate == "delayed";
                    string cfg = Cfg(true, 0, pn || dn ? GateIntervals[Pick(GateIntervals.Length)] : 1000, 31250,
                        RateValues[1 + Pick(RateValues.Length - 1)], false, 32, false, 5000, 0, pn, dn, Pick(2) == 0,
                        Pick(2) == 0, PolyLimits[Pick(PolyLimits.Length)], gate == "attack", gate == "outputrate", false, true,
                        i == 2 ? "1.3=50;9.2=0" : "-");
                    _progress = new List<string>();
                    Op("project " + gate + " " + S(bucket) + " " + cfg, () => ProjectOp(gate, bucket, cfg), ProgressText);
                }
            }
            if (fixture == "tempo-changes.mid")
            {
                // The 2,000,000-bucket limit (14.5 s song): a base of about 1.04
                // million buckets grows to the limit when Per-note boundaries pass
                // the end; a base of 1.8 million throws while growing past it.
                long grow = 14;
                long limit = 8;
                string early = Cfg(true, 0, 250000, 31250, 1000, false, 32, false, 5000, 0, true, false, false, false, 128,
                    false, false, false, true, "-");
                string late = Cfg(true, 0, 2000000, 31250, 1000, false, 32, false, 5000, 0, true, false, false, false, 128,
                    false, false, false, true, "-");
                AnalyzeCall("analyze", _song, grow, early);
                CacheClear(_song);
                AnalyzeCall("analyze", _song, limit, late);
                CacheClear(_song);
                AnalyzeCall("analyze", _song, 7, "null");
            }
        }
    }
}
