// v1 reference side of the MIDIBottleneck v2 Analysis-window parity harness (Milestone 6).
//
// Compiled together with the UNCHANGED v1.0.44 sources (src/*.cs except Program.cs), so it
// runs v1's own DiagnosticsForm / WorkloadGraph code: internal statics directly, private
// statics and instance members through reflection on real, handle-less controls. It writes
// plain-text tables to v2/harness/expected/analysis-ui/ that
// v2/tests/analysis_app_tests.cpp (midibn-analysis-app-tests) compares value by value.
//
// Usage:  analysis-ui-reference.exe <fixture-dir> <output-dir>
//
// File format (same as v2/harness/ui-reference): UTF-8 without byte order mark, "\n" line
// ends; lines starting with '#' are comments; fields are separated by TAB; text fields are
// escaped (backslash, TAB, CR, LF and every character outside printable ASCII as \uXXXX).
// Doubles are written as 16 hex digits of their IEEE-754 bit pattern.
//
// Tables (see the comment at the top of each Write* method):
//   summary.txt        BuildSummary text: fixture x resolution x projection state x configuration
//   inspection.txt     the inspection text box at sample times
//   formats.txt        FormatBytes / FormatResolution / FormatClock / ... on grids (en-US and de-DE)
//   ticks.txt          ChooseTimelineTickInterval, FormatAxisClock, AbbreviatePlaybackCounts
//   resolution.txt     ChooseAutoResolution grid and custom-resolution validation
//   cache-key.txt      AnalysisCacheKey on the summary configurations
//   failure.txt        ConciseAnalysisFailure
//   graph-script.txt   scripted WorkloadGraph operations with the state after each (viewport,
//                      plot rectangle, hover/pin, events raised, ticks), and the text widths
//                      v1's TextRenderer measured for the labels (the C++ test feeds them back)
//   overlay.txt        the Playback statistics overlay: bounds and line fitting
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class AnalysisUiReference
    {
        internal const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public |
            BindingFlags.Instance | BindingFlags.Static;

        internal static string OutDir;
        internal static string FixtureDir;
        internal static readonly CultureInfo EnUs = new CultureInfo("en-US", false);
        internal static readonly CultureInfo DeDe = new CultureInfo("de-DE", false);
        internal static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        [STAThread]
        private static int Main(string[] args)
        {
            try
            {
                if (args.Length != 2)
                {
                    Console.Error.WriteLine("usage: analysis-ui-reference <fixture-dir> <output-dir>");
                    return 2;
                }
                FixtureDir = args[0];
                OutDir = args[1];
                Directory.CreateDirectory(OutDir);
                Thread.CurrentThread.CurrentCulture = EnUs;
                Thread.CurrentThread.CurrentUICulture = EnUs;
                WriteSummaryTable();
                WriteInspectionTable();
                WriteFormatTables();
                WriteTickTables();
                WriteResolutionTables();
                WriteCacheKeyTable();
                WriteFailureTable();
                WriteOverlayTables();
                WriteGraphScript();
                long total = 0;
                foreach (string file in Directory.GetFiles(OutDir)) total += new FileInfo(file).Length;
                Console.WriteLine("Wrote tables to " + OutDir + " (" + total.ToString(Inv) + " bytes).");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("analysis-ui-reference failure: " + ex);
                return 3;
            }
        }

        // ---------------------------------------------------------------- helpers

        internal sealed class Table : IDisposable
        {
            private readonly StringBuilder _text = new StringBuilder();
            private readonly string _path;
            internal Table(string name) { _path = Path.Combine(OutDir, name); }
            internal void Comment(string text) { _text.Append("# ").Append(text).Append('\n'); }
            internal void Row(params string[] fields)
            {
                for (int i = 0; i < fields.Length; i++)
                {
                    if (i > 0) _text.Append('\t');
                    _text.Append(Esc(fields[i]));
                }
                _text.Append('\n');
            }
            public void Dispose() { File.WriteAllText(_path, _text.ToString(), new UTF8Encoding(false)); }
        }

        internal static string Esc(string value)
        {
            if (value == null) return "";
            StringBuilder b = null;
            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                string piece = null;
                if (c == '\\') piece = "\\\\";
                else if (c == '\t') piece = "\\t";
                else if (c == '\r') piece = "\\r";
                else if (c == '\n') piece = "\\n";
                else if (c < 0x20 || c > 0x7E) piece = "\\u" + ((int)c).ToString("x4", Inv);
                if (piece != null && b == null) { b = new StringBuilder(); b.Append(value, 0, i); }
                if (b != null) b.Append(piece != null ? piece : c.ToString());
            }
            return b == null ? value : b.ToString();
        }

        internal static string I(long value) { return value.ToString(Inv); }
        internal static string B(bool value) { return value ? "1" : "0"; }
        internal static string Hex(double value) { return BitConverter.DoubleToInt64Bits(value).ToString("x16", Inv); }

        internal sealed class Prng
        {
            private ulong _state;
            internal Prng(ulong seed) { _state = seed == 0 ? 0x9E3779B97F4A7C15UL : seed; }
            internal ulong Next()
            {
                _state ^= _state >> 12;
                _state ^= _state << 25;
                _state ^= _state >> 27;
                return _state * 2685821657736338717UL;
            }
            internal long Below(long n) { return (long)(Next() % (ulong)n); }
            internal long Range(long low, long high) { return low + Below(high - low + 1); }
        }

        internal static object CallStatic(Type type, string name, params object[] args)
        {
            MethodInfo method = type.GetMethod(name, Hidden);
            if (method == null) throw new InvalidOperationException("missing method " + type.Name + "." + name);
            try { return method.Invoke(null, args); }
            catch (TargetInvocationException ex) { throw ex.InnerException; }
        }

        internal static object CallInstance(object target, string name, params object[] args)
        {
            MethodInfo method = target.GetType().GetMethod(name, Hidden);
            if (method == null) throw new InvalidOperationException("missing method " + target.GetType().Name + "." + name);
            try { return method.Invoke(target, args); }
            catch (TargetInvocationException ex) { throw ex.InnerException; }
        }

        internal static object Field(object target, string name)
        {
            FieldInfo info = target.GetType().GetField(name, Hidden);
            if (info == null) throw new MissingFieldException(target.GetType().Name, name);
            return info.GetValue(target);
        }

        internal static void SetField(object target, string name, object value)
        {
            FieldInfo info = target.GetType().GetField(name, Hidden);
            if (info == null) throw new MissingFieldException(target.GetType().Name, name);
            info.SetValue(target, value);
        }

        // ---------------------------------------------------------------- configurations

        // "sim=1,mode=0,proc=250,..." (the V1ModelHarness configuration text).
        internal static string Cfg(bool sim, int mode, long proc, long bit, long eps, bool ql, int qlLimit, bool qa,
            long qaLimit, int policy, bool pn, bool dn, bool scale, bool ip, int ipLimit, bool na, bool or, bool paced,
            bool aq, string ovr)
        {
            return "sim=" + B(sim) + ",mode=" + I(mode) + ",proc=" + I(proc) + ",bit=" + I(bit) + ",eps=" + I(eps) +
                ",ql=" + B(ql) + ":" + I(qlLimit) + ",qa=" + B(qa) + ":" + I(qaLimit) + ",pol=" + I(policy) +
                ",pn=" + B(pn) + ",dn=" + B(dn) + ",scale=" + B(scale) + ",ip=" + B(ip) + ":" + I(ipLimit) +
                ",na=" + B(na) + ",or=" + B(or) + ",paced=" + B(paced) + ",aq=" + B(aq) + ",ovr=" + ovr;
        }

        internal static AnalysisConfiguration ParseCfg(string text)
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

        // Shorthands: a configuration with the given feature on.
        private static string QueueCfg(int mode, long proc, long bit, long eps, bool ql, int limit, int policy, bool qa,
            long age, bool sim, bool aq, bool paced, string ovr)
        {
            return Cfg(sim, mode, proc, bit, eps, ql, limit, qa, age, policy, false, false, false, true, 128, false, false,
                paced, aq, ovr);
        }

        private static string GateCfg(string gate, long proc, long eps, bool scale, bool ip, int ipLimit, string ovr)
        {
            return Cfg(false, 0, proc, 31250, eps, false, 2000, false, 1000000, 0, gate == "pn", gate == "dn", scale, ip,
                ipLimit, gate == "na", gate == "or", false, true, ovr);
        }

        // The configurations of the summary table (and of the cache-key table).
        internal static List<string> SummaryConfigurations()
        {
            List<string> list = new List<string>();
            list.Add(QueueCfg(0, 100, 31250, 10000, false, 2000, 0, false, 1000000, false, true, false, "-"));   // defaults: Rate model None
            list.Add(QueueCfg(0, 0, 31250, 10000, false, 2000, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(0, 100, 31250, 10000, false, 2000, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, false, 2000, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(0, 5000, 31250, 10000, false, 2000, 0, false, 1000000, true, true, false, "-"));
            for (int policy = 0; policy < 6; policy++)
                list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, policy, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 1, true, 5000, true, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 2, true, 900, true, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 4, true, 60000, true, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 100000, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(1, 100, 31250, 10000, false, 2000, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(1, 100, 31250, 10000, true, 8, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(1, 100, 300, 10000, true, 3, 3, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(2, 100, 31250, 0, false, 2000, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(2, 100, 31250, 1000, true, 32, 0, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(2, 100, 31250, 20000, true, 32, 2, false, 1000000, true, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 0, false, 1000000, false, true, false, "-"));   // virtual forward
            list.Add(QueueCfg(1, 250, 31250, 10000, true, 8, 0, false, 1000000, false, true, false, "-"));
            list.Add(QueueCfg(2, 250, 31250, 2000, true, 8, 0, false, 1000000, false, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 0, false, 1000000, false, false, false, "-"));  // limit off without slowdown
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 0, true, 5000, false, true, false, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 0, false, 1000000, true, true, true, "-"));    // output-paced
            list.Add(GateCfg("pn", 1000, 0, false, false, 128, "-"));
            list.Add(GateCfg("pn", 1000, 0, true, true, 5, "-"));
            list.Add(GateCfg("pn", 250000, 0, true, false, 128, "-"));
            list.Add(GateCfg("dn", 1000, 0, false, false, 128, "-"));
            list.Add(GateCfg("dn", 250000, 0, true, true, 2, "-"));
            list.Add(GateCfg("dn", 20000, 0, false, false, 128, "-"));
            list.Add(GateCfg("na", 100, 30, false, false, 128, "-"));
            list.Add(GateCfg("na", 100, 0, false, false, 128, "-"));
            list.Add(GateCfg("or", 100, 5, false, false, 128, "-"));
            list.Add(GateCfg("or", 100, 0, false, false, 128, "-"));
            list.Add(GateCfg("or", 100, 1000, false, false, 128, "-"));
            list.Add(QueueCfg(0, 250, 31250, 10000, true, 8, 0, false, 1000000, true, true, false, "none"));
            list.Add(QueueCfg(0, 250, 31250, 10000, false, 2000, 0, false, 1000000, true, true, false, "3.4=100;3.5=20;5.6=1"));
            list.Add(GateCfg("pn", 1000, 0, false, false, 128, "3.4=100;3.5=20"));
            list.Add(GateCfg("or", 100, 1000, false, false, 128, "none"));
            list.Add(GateCfg("na", 100, 30, false, false, 128, "0.3=64;1.3=0"));
            list.Add(GateCfg("dn", 20000, 0, false, false, 128, "3.4=100;3.5=20;5.6=1;5.7=-8192;5.8=9"));
            Prng r = new Prng(20261014);
            for (int i = 0; i < 6; i++)
            {
                int kind = (int)r.Below(4);
                string ovr = r.Below(3) == 0 ? "none" : "-";
                if (kind == 0)
                    list.Add(QueueCfg((int)r.Below(3), new long[] { 0, 7, 1000 }[r.Below(3)], new long[] { 300, 3125 }[r.Below(2)],
                        new long[] { 0, 3, 500 }[r.Below(3)], r.Below(2) == 0, new int[] { 1, 2, 500 }[r.Below(3)], (int)r.Below(6),
                        r.Below(2) == 0, new long[] { 1, 900 }[r.Below(2)], r.Below(3) != 0, r.Below(2) == 0, r.Below(2) == 0, ovr));
                else
                    list.Add(GateCfg(new string[] { "pn", "dn", "na", "or" }[r.Below(4)], new long[] { 1, 1000 }[r.Below(2)],
                        new long[] { 1, 5, 9999999 }[r.Below(3)], r.Below(2) == 0, r.Below(2) == 0, new int[] { -3, 0, 1, 300 }[r.Below(4)], ovr));
            }
            return list;
        }

        // ---------------------------------------------------------------- fixtures and analyses

        internal static MidiSong LoadFixture(string name)
        {
            return DirectCompactMidiParser.Load(Path.Combine(FixtureDir, name + ".mid"), CancellationToken.None, null);
        }

        internal static WorkloadAnalysis AnalyzeComplete(MidiSong song, long bucket, string cfg)
        {
            return WorkloadAnalyzer.Analyze(song, bucket, ParseCfg(cfg), CancellationToken.None, null, null);
        }

        // The window's states: "complete", "pending" (the preview shown while the projection runs),
        // "cancelled" (Cancel pressed during a preview) and "failed:<reason>".
        internal static WorkloadAnalysis AnalyzeVariant(MidiSong song, long bucket, string cfg, string variant)
        {
            if (variant == "complete") return AnalyzeComplete(song, bucket, cfg);
            WorkloadAnalysis baseWorkload = WorkloadAnalyzer.Analyze(song, bucket, null, CancellationToken.None, null, null);
            WorkloadAnalysis pending = new WorkloadBaseAnalysis(baseWorkload).CreatePresentationAnalysis(
                ParseCfg(cfg), CancellationToken.None);
            if (variant == "pending") return pending;
            if (variant == "cancelled") return pending.WithProjectionState(AnalysisProjectionState.Cancelled);
            if (variant.StartsWith("failed:", StringComparison.Ordinal)) return pending.WithProjectionFailure(variant.Substring(7));
            throw new ArgumentException(variant);
        }

        private static string FileName(string fixture) { return fixture + ".mid"; }

        private static long DefaultResolution(MidiSong song)
        {
            return (long)CallStatic(typeof(WorkloadAnalyzer), "DefaultResolution", song);
        }

        // ---------------------------------------------------------------- summary.txt

        // fixture, bucket (microseconds), variant, configuration, BuildSummary text.
        // The song's file name is "<fixture>.mid" (the C++ side loads the same file).
        private static void WriteSummaryTable()
        {
            MethodInfo build = typeof(DiagnosticsForm).GetMethod("BuildSummary", Hidden);
            List<string> all = SummaryConfigurations();
            using (Table t = new Table("summary.txt"))
            {
                t.Comment("fixture\tbucket\tvariant\tcfg\tBuildSummary text (DiagnosticsForm.BuildSummary, en-US)");
                string[] full = { "models-notes", "f0-all-message-types" };
                string[] few = { "models-dense", "tempo-changes", "f1-v1-test-midi", "empty-tracks" };
                int[] fewConfigs = { 0, 3, 6, 11, 16, 22, 29, 33, 36, 40, 44, 46 };
                foreach (string fixture in full)
                {
                    MidiSong song = LoadFixture(fixture);
                    long bucket = DefaultResolution(song);
                    foreach (string cfg in all) SummaryRow(t, build, song, fixture, bucket, cfg, "complete");
                    if (fixture == "models-notes")
                    {
                        SummaryRow(t, build, song, fixture, 25000, all[3], "complete");
                        SummaryRow(t, build, song, fixture, 1000000, all[8], "complete");
                        int[] states = { 0, 3, 7, 21, 28, 31, 34, 37, 40 };
                        foreach (int index in states)
                        {
                            SummaryRow(t, build, song, fixture, bucket, all[index], "pending");
                            SummaryRow(t, build, song, fixture, bucket, all[index], "cancelled");
                            SummaryRow(t, build, song, fixture, bucket, all[index], "failed:Operation timed out.");
                        }
                    }
                }
                foreach (string fixture in few)
                {
                    MidiSong song;
                    try { song = LoadFixture(fixture); } catch (Exception) { continue; }
                    long bucket = DefaultResolution(song);
                    foreach (int index in fewConfigs)
                        if (index < all.Count) SummaryRow(t, build, song, fixture, bucket, all[index], "complete");
                }
            }
        }

        private static void SummaryRow(Table t, MethodInfo build, MidiSong song, string fixture, long bucket, string cfg,
            string variant)
        {
            song.FilePath = Path.Combine(FixtureDir, FileName(fixture));
            WorkloadAnalysis analysis = AnalyzeVariant(song, bucket, cfg, variant);
            string text = (string)build.Invoke(null, new object[] { song, analysis });
            t.Row(fixture, I(bucket), variant, cfg, text);
        }

        // ---------------------------------------------------------------- inspection.txt

        // fixture, bucket, variant, cfg, show-event-counts, pinned, time, inspection text.
        private static void WriteInspectionTable()
        {
            List<string> all = SummaryConfigurations();
            int[] picks = { 0, 6, 12, 13, 17, 24, 28, 31, 33, 35, 42 };
            using (Table t = new Table("inspection.txt"))
            {
                t.Comment("fixture\tbucket\tvariant\tcfg\tshowEventCounts\tpinned\ttime\ttext (DiagnosticsForm.UpdateInspection)");
                foreach (string fixture in new[] { "models-notes", "f0-all-message-types" })
                {
                    MidiSong song = LoadFixture(fixture);
                    song.FilePath = Path.Combine(FixtureDir, FileName(fixture));
                    long bucket = DefaultResolution(song);
                    long duration = song.DurationMicroseconds;
                    long[] times = { -1, 0, 1, 12345, 500000, duration / 3, duration / 2, Math.Max(0, duration - 1), duration, duration + 1000, 1000000000000L };
                    foreach (int index in picks)
                    {
                        string variant = index == 42 ? "pending" : "complete";
                        WorkloadAnalysis analysis = AnalyzeVariant(song, bucket, all[index], variant);
                        using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
                        {
                            for (int counts = 0; counts < 2; counts++)
                            {
                                CallInstance(form, "SetShowEventCounts", counts == 1);
                                for (int i = 0; i < times.Length; i++)
                                {
                                    bool pinned = (i & 1) == 1;
                                    CallInstance(form, "UpdateInspection", times[i], pinned);
                                    t.Row(fixture, I(bucket), variant, all[index], B(counts == 1), B(pinned), I(times[i]),
                                        form.InspectionTextForTesting);
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------- formats.txt

        private static void WriteFormatTables()
        {
            Type form = typeof(DiagnosticsForm);
            Type graph = typeof(WorkloadGraph);
            long[] bytes = { Int64.MinValue, -1, 0, 1, 99, 1023, 1024, 1025, 1536, 10239, 10240, 102400, 1048575, 1048576, 1048577,
                1572864, 5242880, 123456789, 1073741823, 1073741824, 5000000000L, 1099511627776L, Int64.MaxValue };
            long[] micros = { 0, 1, 999, 1000, 1001, 1500, 10000, 12345, 25000, 100000, 123456, 250000, 999999, 1000000,
                1000001, 1500000, 2500000, 60000000, 86400000000L, 123456789012L, 1000, 3000, 7000 };
            long[] clocks = { Int64.MinValue, -1000, -1, 0, 1, 999, 1000, 59999, 59999999, 60000000, 61000000, 61999999, 3599999999L,
                3600000000L, 3661123456L, 5999999999L, 6000000000L, 36000000000L, 359999999999L, 360000000000L, 86399999999L,
                1000000000000L, 12345678901234L };
            using (Table t = new Table("formats.txt"))
            {
                t.Comment("function\tculture\targument\tresult");
                foreach (CultureInfo culture in new[] { EnUs, DeDe })
                {
                    Thread.CurrentThread.CurrentCulture = culture;
                    foreach (long v in bytes) t.Row("bytes", culture.Name, I(v), (string)CallStatic(form, "FormatBytes", v));
                    foreach (long v in micros)
                    {
                        t.Row("resolution", culture.Name, I(v), (string)CallStatic(form, "FormatResolution", v));
                        t.Row("graph-resolution", culture.Name, I(v), (string)CallStatic(graph, "FormatResolution", v));
                    }
                }
                Thread.CurrentThread.CurrentCulture = EnUs;
                foreach (long v in clocks)
                {
                    t.Row("clock", "-", I(v), (string)CallStatic(form, "FormatClock", v));
                    t.Row("boundary", "-", I(v), (string)CallStatic(form, "FormatIntervalBoundary", v));
                    t.Row("graph-clock", "-", I(v), (string)CallStatic(graph, "FormatClock", v));
                    t.Row("graph-clock-detailed", "-", I(v), (string)CallStatic(graph, "FormatClockDetailed", v));
                    t.Row("graph-boundary", "-", I(v), (string)CallStatic(graph, "FormatIntervalBoundary", v));
                }
                for (int policy = 0; policy < 7; policy++)
                    t.Row("overflow", "-", I(policy), (string)CallStatic(form, "FormatOverflowPolicy", (OverflowPolicy)policy));
            }
        }

        // ---------------------------------------------------------------- ticks.txt

        private static readonly long[] TickSpans = { Int64.MinValue, -5, 0, 1, 2, 999, 1000, 4000, 9999, 10000, 33333, 100000,
            250000, 999999, 1000000, 1500000, 7000000, 20000000, 59999999, 60000000, 100000000, 123456789, 600000000,
            1234567890, 3599999999L, 3600000000L, 7200000001L, 20000000000L, 86400000000L, 360000000000L, 3600000000L * 100,
            3600000000L * 1000, 3600000000L * 100000, 9000000000000000000L };
        private static readonly int[] TickWidths = { -10, 0, 1, 75, 76, 77, 100, 152, 300, 430, 640, 900, 1200, 1920, 3840 };

        private static void WriteTickTables()
        {
            Type graph = typeof(WorkloadGraph);
            long[] intervals = { 1000, 2000, 5000, 10000, 50000, 100000, 500000, 999999, 1000000, 2000000, 15000000, 60000000,
                300000000, 3600000000L, 7200000000L, 36000000000L, 108000000000L };
            long[] times = { Int64.MinValue, -1, 0, 1, 999, 1000, 59999999, 60000000, 61500000, 3599999999L, 3600000000L,
                3661123456L, 7322999999L, 36000000000L, 359999999999L, 360000000000L, 1234567890123L };
            using (Table t = new Table("ticks.txt"))
            {
                t.Comment("interval\tspan\twidth\tresult   |   axis\ttime\tinterval\tlabel   |   abbreviate\ttext\tresult");
                foreach (long span in TickSpans)
                    foreach (int width in TickWidths)
                        t.Row("interval", I(span), I(width), I((long)CallStatic(graph, "ChooseTimelineTickInterval", span, width)));
                foreach (long time in times)
                    foreach (long interval in intervals)
                        t.Row("axis", I(time), I(interval), (string)CallStatic(graph, "FormatAxisClock", time, interval));
                string[] texts = { "", "0", "12", "1,234", "9,999", "10,000", "10,001", "12,345", "99,999", "100,000", "999,999",
                    "1,000,000", "1,234,567", "12,345,678", "999,999,999", "1,000,000,000", "1,234,567,890", "12,345,678,901",
                    "5,000 / 10,000", "10,000 / 1,000,000", "120,000 events/sec", "1,234,567 / 12,345,678 / 3", "1,2", "1,23,4",
                    "abc 12,345 def", "00:12.345 / 00:11.000", "9,999,999 / 10,000,000", "1,000,000,000,000", "12345", "12345678",
                    "1,234.5", "12,345.6 events/sec", "250.0%", "0.000 ms / 12.345 ms", "99,999,999,999,999,999,999", "10,000,0" };
                foreach (string text in texts)
                    t.Row("abbreviate", text, (string)CallStatic(graph, "AbbreviatePlaybackCounts", text));
            }
        }

        // ---------------------------------------------------------------- resolution.txt

        private static void WriteResolutionTables()
        {
            Type form = typeof(DiagnosticsForm);
            long[] spans = { 1, 1000, 100000, 1000000, 3000000, 10000000, 30000000, 60000000, 120000000, 180000000, 240000000,
                300000000, 600000000, 1800000000, 3600000000L, 7200000000L, 86400000000L };
            int[] widths = { 100, 150, 600, 900, 1200, 1500, 2000, 3840 };
            long[] currents = { 0, 10000, 25000, 50000, 100000, 250000, 500000, 1000000, 123456 };
            decimal[] decimals = { 0m, -1m, 0.0004m, 0.0005m, 0.001m, 0.0015m, 0.0025m, 1m, 10m, 12.3456789m, 100m, 1000m,
                86400000m, 86400000.001m, 1000000000000m, 9223372036854775m, 9223372036854776m, 9223372036854775.807m,
                9223372036854775.808m, 79228162514264337593543950335m, 79228162514264337593543950m,
                79228162514264337593543950.335m, 0.0000000000000000000000000001m };
            long[] durations = { 0, 1, 1000, 250000, 5000000, 60000000, 3600000000L, 86400000000L, 200000000000000L };
            using (Table t = new Table("resolution.txt"))
            {
                t.Comment("auto\tspan\twidth\tcurrent\tresult   |   custom\tduration\tmilliseconds\tok\tmicroseconds\terror");
                foreach (long span in spans)
                    foreach (int width in widths)
                        foreach (long current in currents)
                            t.Row("auto", I(span), I(width), I(current), I((long)CallStatic(form, "ChooseAutoResolution", span, width, current)));
                foreach (long duration in durations)
                    foreach (decimal ms in decimals)
                    {
                        MidiSong song = new MidiSong { DurationMicroseconds = duration };
                        object[] args = { song, ms, 0L, null };
                        MethodInfo method = form.GetMethod("TryValidateCustomResolution", Hidden);
                        bool ok = (bool)method.Invoke(null, args);
                        t.Row("custom", I(duration), ms.ToString(Inv), B(ok), I((long)args[2]), (string)args[3]);
                    }
                MidiSong none = null;
                object[] nullArgs = { none, 5m, 0L, null };
                bool nullOk = (bool)form.GetMethod("TryValidateCustomResolution", Hidden).Invoke(null, nullArgs);
                t.Row("custom", "nosong", "5", B(nullOk), I((long)nullArgs[2]), (string)nullArgs[3]);
                long[] defaults = { 0, 1, 1000000, 5000000, 20000000, 60000000, 300000000, 3600000000L, 86400000000L, 200000000000000L };
                foreach (long duration in defaults)
                {
                    MidiSong song = new MidiSong { DurationMicroseconds = duration };
                    long bucketCount = -1;
                    string error = "";
                    long resolution = DefaultResolution(song);
                    try { bucketCount = WorkloadAnalyzer.CalculateBucketCount(duration, resolution); } catch (Exception ex) { error = ex.GetType().Name + ": " + ex.Message; }
                    t.Row("default", I(duration), I(resolution), I(bucketCount), error);
                }
            }
        }

        // ---------------------------------------------------------------- cache-key.txt

        private static void WriteCacheKeyTable()
        {
            using (Table t = new Table("cache-key.txt"))
            {
                t.Comment("cfg\tresolution\tAnalysisCacheKey");
                foreach (string cfg in SummaryConfigurations())
                    foreach (long resolution in new long[] { 10000, 100000, 123456 })
                    {
                        AnalysisConfiguration c = ParseCfg(cfg);
                        t.Row(cfg, I(resolution), (string)CallStatic(typeof(DiagnosticsForm), "AnalysisCacheKey", c, resolution));
                    }
                // Fields the key leaves out (queue age without length, processing time while no slowdown...) must give the same key.
                AnalysisConfiguration d = new AnalysisConfiguration();
                t.Row("default", "100000", (string)CallStatic(typeof(DiagnosticsForm), "AnalysisCacheKey", d, 100000L));
            }
        }

        // ---------------------------------------------------------------- failure.txt

        private static void WriteFailureTable()
        {
            string longText = new string('x', 240);
            string[] messages = { "Plain failure.", "  padded  ", "two\r\nlines", "tab\there", "cr\rcr", "lf\nlf", "", "   ", "\r\n",
                longText, longText + "y", new string('z', 237), new string('z', 238), new string('z', 300), "café — dash",
                "multi\r\nline " + new string('q', 400), new string('w', 236) + " " + new string('v', 20) };
            using (Table t = new Table("failure.txt"))
            {
                t.Comment("exception type\tmessage\tConciseAnalysisFailure; the type name is shown when the message is blank");
                Exception[] makers = { new OverflowException(""), new InvalidOperationException(""), new OperationCanceledException("") };
                foreach (string message in messages)
                {
                    Exception[] exceptions = { new OverflowException(message), new InvalidOperationException(message), new Exception(message),
                        new ArgumentOutOfRangeException(null, message) };
                    foreach (Exception ex in exceptions)
                        t.Row(ex.GetType().Name, message, (string)CallStatic(typeof(DiagnosticsForm), "ConciseAnalysisFailure", ex));
                }
                // The message text of a type that defines its own default message.
                foreach (Exception ex in new Exception[] { new OverflowException(), new InvalidOperationException(), new OperationCanceledException() })
                    t.Row(ex.GetType().Name, ex.Message, (string)CallStatic(typeof(DiagnosticsForm), "ConciseAnalysisFailure", ex));
                t.Row("null", "", (string)CallStatic(typeof(DiagnosticsForm), "ConciseAnalysisFailure", (object)null));
            }
        }
    }
}
