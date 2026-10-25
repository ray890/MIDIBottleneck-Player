// v1 reference side of the MIDIBottleneck v2 parity harness.
//
// Compiled together with the UNCHANGED v1.0.44 sources (src/*.cs except
// Program.cs) so it runs v1's own code. It never modifies v1 behaviour; the
// tempo map is read from v1's private parser types through reflection.
//
// Modes:
//   dump <midi> [--summary] [--out <trace>]   canonical trace (TRACE_FORMAT.md)
//   flow <midi> [--summary] [--out <trace>]   load-flow trace: progress, decisions, cancellation
//   decimal-vectors <count> <seed> <out>       .NET decimal tempo-clock vectors
//   decimal-ops <seed> <out>                   .NET decimal primitive-operation vectors
//   perf-load <midi>                           one timed load, key=value output
//   models <family> <fixture-dir> <out>        model trace (V1ModelHarness.cs)
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal static class V1ReferenceHarness
    {
        private const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public |
            BindingFlags.Instance | BindingFlags.Static;

        private static int Main(string[] args)
        {
            try
            {
                if (args.Length >= 2 && args[0] == "dump") return Dump(args);
                if (args.Length >= 2 && args[0] == "flow") return Flow(args);
                if (args.Length == 4 && args[0] == "decimal-vectors")
                    return DecimalVectors(Int32.Parse(args[1], CultureInfo.InvariantCulture),
                        Int32.Parse(args[2], CultureInfo.InvariantCulture), args[3]);
                if (args.Length == 3 && args[0] == "decimal-ops")
                    return DecimalOps(Int32.Parse(args[1], CultureInfo.InvariantCulture), args[2]);
                if (args.Length == 2 && args[0] == "perf-load") return PerfLoad(args[1]);
                if (args.Length >= 2 && args[0] == "models") return V1ModelHarness.Run(args);
                Console.Error.WriteLine("usage: dump <midi> [--summary] [--out <trace>] | flow <midi> [--summary] [--out <trace>] | " +
                    "decimal-vectors <count> <seed> <out> | decimal-ops <seed> <out> | perf-load <midi> | models <family> <fixture-dir> <out>");
                return 2;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("harness failure: " + ex);
                return 3;
            }
        }

        // ---------------------------------------------------------------- dump

        private sealed class Trace
        {
            private readonly StringBuilder _text = new StringBuilder();
            internal void Line(string value) { _text.Append(value).Append('\n'); }
            internal void Line(string format, params object[] values)
            { Line(String.Format(CultureInfo.InvariantCulture, format, values)); }
            internal string Text { get { return _text.ToString(); } }
        }

        // Body lines are either written verbatim (full mode) or folded into a
        // SHA-256 digest of exactly those lines (summary mode).
        private sealed class Body
        {
            private readonly Trace _trace;
            private readonly bool _summary;
            private readonly SHA256 _hash;
            internal Body(Trace trace, bool summary)
            {
                _trace = trace;
                _summary = summary;
                _hash = summary ? SHA256.Create() : null;
            }
            internal void Line(string value)
            {
                if (!_summary) { _trace.Line(value); return; }
                byte[] bytes = Encoding.ASCII.GetBytes(value + "\n");
                _hash.TransformBlock(bytes, 0, bytes.Length, null, 0);
            }
            internal void Finish()
            {
                if (!_summary) return;
                _hash.TransformFinalBlock(new byte[0], 0, 0);
                _trace.Line("digest sha256 " + Hex(_hash.Hash));
                _hash.Dispose();
            }
        }

        private static int Dump(string[] args)
        {
            string path = args[1];
            bool summary = false;
            string outPath = null;
            for (int i = 2; i < args.Length; i++)
            {
                if (args[i] == "--summary") summary = true;
                else if (args[i] == "--out" && i + 1 < args.Length) outPath = args[++i];
                else throw new ArgumentException("unknown dump option " + args[i]);
            }

            Trace trace = new Trace();
            trace.Line("# producer v1-reference (MIDIBottleneck v1.0.44 sources) " +
                (IntPtr.Size == 8 ? "x64" : "x86") + " clr " + Environment.Version);
            trace.Line("# input " + Path.GetFileName(path));
            trace.Line("trace-format 1");
            trace.Line("mode " + (summary ? "summary" : "full"));

            trace.Line("[file]");
            long fileSize = new FileInfo(path).Length;
            trace.Line("size {0}", fileSize);
            string container;
            try
            {
                using (MidiFileContent content = MidiFileContent.Open(path))
                    container = content.IsRmid ? "rmid" : "smf";
            }
            catch (Exception) { container = "error"; }
            trace.Line("container " + container);

            WritePreflight(trace, path, fileSize);

            trace.Line("[song]");
            MidiSong song = null;
            try { song = DirectCompactMidiParser.Load(path, CancellationToken.None, null); }
            catch (Exception ex) { trace.Line(ErrorStatus(ex)); }
            if (song != null)
            {
                WriteSong(trace, song);
                WriteTempo(trace, path, summary);
                WriteEvents(trace, song, summary);
                WriteIndexes(trace, song, summary);
            }
            trace.Line("[end]");
            Emit(trace.Text, outPath);
            return 0;
        }

        private static void Emit(string text, string outPath)
        {
            if (outPath == null)
            {
                Stream stdout = Console.OpenStandardOutput();
                byte[] bytes = Encoding.ASCII.GetBytes(text);
                stdout.Write(bytes, 0, bytes.Length);
                stdout.Flush();
            }
            else File.WriteAllBytes(outPath, Encoding.ASCII.GetBytes(text));
        }

        // ------------------------------------------------------------ indexes

        private static readonly string[] AttributeNames =
            { "BankMsb", "BankLsb", "Program", "Volume", "Expression", "Pan", "Sustain", "PitchBend", "Aftertouch" };

        // Reads a private v1 index series (Count + indexer over EventIndex/Value structs).
        private static List<int[]> ReadSeries(object series)
        {
            List<int[]> values = new List<int[]>();
            if (series == null) return values;
            Type type = series.GetType();
            int count = (int)type.GetField("Count", Hidden).GetValue(series);
            Array segments = (Array)type.GetField("Segments", Hidden).GetValue(series);
            FieldInfo eventIndex = null, value = null;
            int remaining = count;
            foreach (object segmentObject in segments)
            {
                Array segment = (Array)segmentObject;
                for (int i = 0; i < segment.Length && remaining > 0; i++, remaining--)
                {
                    object entry = segment.GetValue(i);
                    if (eventIndex == null)
                    {
                        eventIndex = entry.GetType().GetField("EventIndex", Hidden);
                        value = entry.GetType().GetField("Value", Hidden);
                    }
                    values.Add(new int[] { (int)eventIndex.GetValue(entry), (int)value.GetValue(entry) });
                }
            }
            return values;
        }

        private static void WriteSeries(Body body, string header, object series)
        {
            List<int[]> values = ReadSeries(series);
            if (values.Count == 0) return;
            body.Line(header + " " + values.Count.ToString(CultureInfo.InvariantCulture));
            foreach (int[] v in values)
                body.Line("v " + v[0].ToString(CultureInfo.InvariantCulture) + " " + v[1].ToString(CultureInfo.InvariantCulture));
        }

        private static void WriteIndexes(Trace trace, MidiSong song, bool summary)
        {
            IMidiEventStore store = song.EventStore;
            ChannelSourceValueIndex source = song.GetChannelSourceValueIndex();
            MidiStateChaseIndex chase = song.GetMidiStateChaseIndex();

            // Raw source-value histories, slot order channel-major.
            Array changes = (Array)typeof(ChannelSourceValueIndex).GetField("_changes", Hidden).GetValue(source);
            long sourceEntries = 0;
            foreach (object series in changes) sourceEntries += ReadSeries(series).Count;
            trace.Line("[source-index]");
            trace.Line("count {0}", sourceEntries);
            Body body = new Body(trace, summary);
            for (int slot = 0; slot < changes.Length; slot++)
                WriteSeries(body, "slot " + (slot / 9).ToString(CultureInfo.InvariantCulture) + " " + AttributeNames[slot % 9],
                    changes.GetValue(slot));
            body.Finish();

            // Raw whole-state chase histories.
            Type chaseType = typeof(MidiStateChaseIndex);
            Array state = (Array)chaseType.GetField("_state", Hidden).GetValue(chase);
            Array resets = (Array)chaseType.GetField("_resets", Hidden).GetValue(chase);
            Array selectors = (Array)chaseType.GetField("_selectors", Hidden).GetValue(chase);
            Array parameters = (Array)chaseType.GetField("_parameters", Hidden).GetValue(chase);
            trace.Line("[chase-index]");
            trace.Line("entries {0} parameter-series {1}", chase.EntryCount, chase.ParameterSeriesCount);
            body = new Body(trace, summary);
            for (int slot = 0; slot < state.Length; slot++)
                WriteSeries(body, "state " + (slot / 123).ToString(CultureInfo.InvariantCulture) + " " +
                    (slot % 123).ToString(CultureInfo.InvariantCulture), state.GetValue(slot));
            for (int channel = 0; channel < 16; channel++)
                WriteSeries(body, "reset " + channel.ToString(CultureInfo.InvariantCulture), resets.GetValue(channel));
            for (int channel = 0; channel < 16; channel++)
                WriteSeries(body, "selector " + channel.ToString(CultureInfo.InvariantCulture), selectors.GetValue(channel));
            for (int channel = 0; channel < 16; channel++)
            {
                Array channelParameters = (Array)parameters.GetValue(channel);
                if (channelParameters == null) continue;
                foreach (object parameter in channelParameters)
                {
                    int key = (int)parameter.GetType().GetField("Key", Hidden).GetValue(parameter);
                    WriteSeries(body, "parameter " + channel.ToString(CultureInfo.InvariantCulture) + " " +
                        key.ToString(CultureInfo.InvariantCulture), parameter.GetType().GetField("Values", Hidden).GetValue(parameter));
                }
            }
            body.Finish();

            // Query results at every event index. Omitted in summary mode (the
            // large performance fixture), whose raw histories are digested above.
            if (summary) return;
            int count = store.Count;

            trace.Line("[source-query]");
            int[] previous = new int[16 * 9];
            bool[] previousKnown = new bool[16 * 9];
            for (int index = 0; index < count; index++)
            {
                long time = store.GetEvent(index).IntendedMicroseconds;
                StringBuilder line = null;
                for (int slot = 0; slot < 16 * 9; slot++)
                {
                    int value;
                    bool known = source.TryGetLatest(slot / 9, (ChannelAttribute)(slot % 9), time, out value);
                    if (known == previousKnown[slot] && (!known || value == previous[slot])) continue;
                    previousKnown[slot] = known;
                    previous[slot] = value;
                    if (line == null)
                        line = new StringBuilder("q " + index.ToString(CultureInfo.InvariantCulture) + " " +
                            time.ToString(CultureInfo.InvariantCulture));
                    line.Append(' ').Append(slot / 9).Append('.').Append(AttributeNames[slot % 9]).Append('=')
                        .Append(known ? value.ToString(CultureInfo.InvariantCulture) : "-");
                }
                if (line != null) trace.Line(line.ToString());
            }

            trace.Line("[chase-readouts]");
            int[] readout = ReadoutValues(ChannelPlaybackSnapshot.Empty());
            for (int exclusive = 0; exclusive <= count; exclusive++)
            {
                int[] current = ReadoutValues(chase.CreateAttributeReadouts(exclusive));
                StringBuilder line = null;
                for (int i = 0; i < current.Length; i++)
                {
                    if (current[i] == readout[i]) continue;
                    if (line == null) line = new StringBuilder("r " + exclusive.ToString(CultureInfo.InvariantCulture));
                    line.Append(' ').Append(i / 9).Append('.').Append(ReadoutNames[i % 9]).Append('=')
                        .Append(current[i].ToString(CultureInfo.InvariantCulture));
                }
                if (line != null) trace.Line(line.ToString());
                readout = current;
            }

            ChannelRoutingState routing;
            ChannelOverrideState overrides;
            FilteredChaseConfiguration(out routing, out overrides);
            foreach (bool filtered in new bool[] { false, true })
            {
                trace.Line(filtered ? "[chase-messages-filtered]" : "[chase-messages]");
                string last = "-";
                for (int exclusive = 0; exclusive <= count; exclusive++)
                {
                    IList<MidiEvent> list = filtered ? chase.CreateMessages(exclusive, routing, overrides)
                        : chase.CreateMessages(exclusive, null, null);
                    string messages = MessageText(list);
                    if (messages != last)
                    {
                        trace.Line("c " + exclusive.ToString(CultureInfo.InvariantCulture) + " " +
                            list.Count.ToString(CultureInfo.InvariantCulture) + " " + TextDigest(messages));
                        last = messages;
                    }
                }
                trace.Line("final " + last);
            }
        }

        private static string TextDigest(string text)
        {
            using (SHA256 hash = SHA256.Create())
                return "sha256:" + Hex(hash.ComputeHash(Encoding.ASCII.GetBytes(text)));
        }

        private static readonly string[] ReadoutNames =
            { "BankMsb", "BankLsb", "Program", "Volume", "Expression", "Pan", "Sustain", "PitchBend", "ChannelPressure" };

        private static int[] ReadoutValues(ChannelPlaybackSnapshot snapshot)
        {
            int[] values = new int[16 * 9];
            for (int channel = 0; channel < 16; channel++)
            {
                MidiChannelSnapshot s = snapshot.Channels[channel];
                int[] v = { s.BankMsb, s.BankLsb, s.Program, s.Volume, s.Expression, s.Pan, s.Sustain, s.PitchBend, s.ChannelPressure };
                Array.Copy(v, 0, values, channel * 9, 9);
            }
            return values;
        }

        // TRACE_FORMAT.md "filtered chase configuration": disabled channels and
        // forced attributes that exercise every CreateMessages filter path.
        private static void FilteredChaseConfiguration(out ChannelRoutingState routing, out ChannelOverrideState overrides)
        {
            routing = new ChannelRoutingState();
            overrides = new ChannelOverrideState();
            for (int channel = 0; channel < 16; channel++)
            {
                if (channel % 3 == 1) routing.SetEnabled(channel, false);
                switch (channel % 4)
                {
                    case 0:
                        overrides.SetValue(channel, ChannelAttribute.Volume, 100);
                        overrides.SetValue(channel, ChannelAttribute.Program, 5);
                        overrides.SetValue(channel, ChannelAttribute.PitchBend, 0);
                        break;
                    case 1:
                        overrides.SetValue(channel, ChannelAttribute.BankMsb, 1);
                        overrides.SetValue(channel, ChannelAttribute.BankLsb, 2);
                        overrides.SetValue(channel, ChannelAttribute.Pan, 64);
                        overrides.SetValue(channel, ChannelAttribute.Expression, 90);
                        overrides.SetValue(channel, ChannelAttribute.Sustain, 1);
                        overrides.SetValue(channel, ChannelAttribute.Aftertouch, 10);
                        break;
                    case 3:
                        overrides.SetValue(channel, ChannelAttribute.PitchBend, -8192);
                        overrides.SetValue(channel, ChannelAttribute.Aftertouch, 0);
                        overrides.SetValue(channel, ChannelAttribute.Volume, 0);
                        break;
                }
            }
        }

        private static string MessageText(IList<MidiEvent> messages)
        {
            if (messages.Count == 0) return "-";
            StringBuilder text = new StringBuilder();
            foreach (MidiEvent m in messages)
            {
                if (text.Length != 0) text.Append(' ');
                text.Append(m.Kind.ToString()).Append(':');
                for (int b = 0; b < m.DataLength; b++)
                    text.Append(m.GetDataByte(b).ToString("X2", CultureInfo.InvariantCulture));
            }
            return text.ToString();
        }

        // --------------------------------------------------------------- flow

        private enum Decision { Accept, Decline, AcceptThenCancel }

        private sealed class FlowScenario
        {
            internal string Name; internal bool ForcePreflight; internal bool ForceWarning; internal Decision Decision;
            internal FlowScenario(string name, bool preflight, bool warning, Decision decision)
            { Name = name; ForcePreflight = preflight; ForceWarning = warning; Decision = decision; }
        }

        private static readonly FlowScenario[] FlowScenarios =
        {
            new FlowScenario("natural", false, false, Decision.Accept),
            new FlowScenario("preflight", true, false, Decision.Accept),
            new FlowScenario("warning-decline", true, true, Decision.Decline),
            new FlowScenario("warning-accept", true, true, Decision.Accept),
            new FlowScenario("warning-accept-cancel", true, true, Decision.AcceptThenCancel),
        };

        private sealed class FlowResult
        {
            internal int Reports;
            internal bool DecisionRequested;
            internal string Outcome;
        }

        // Runs v1's MidiFileParser.LoadWithPreflight once. cancelAt: -1 never,
        // 0 before the call, K >= 1 inside the K-th progress report.
        private static FlowResult RunFlow(string path, FlowScenario scenario, int cancelAt, Body reports)
        {
            FlowResult result = new FlowResult();
            using (CancellationTokenSource cancellation = new CancellationTokenSource())
            {
                if (cancelAt == 0) cancellation.Cancel();
                Action<MidiLoadProgress> progress = delegate(MidiLoadProgress p)
                {
                    result.Reports++;
                    if (reports != null)
                        reports.Line(String.Format(CultureInfo.InvariantCulture, "p {0} {1} {2} {3} {4} {5} {6}",
                            Quote(p.Stage), p.CompletedTracks, p.TotalTracks, p.OverallCompleted, p.OverallTotal,
                            p.StageCompleted, p.StageTotal));
                    if (result.Reports == cancelAt) cancellation.Cancel();
                };
                Func<MidiLargeFileInspection, bool> decision = delegate(MidiLargeFileInspection inspection)
                {
                    result.DecisionRequested = true;
                    if (scenario.Decision == Decision.AcceptThenCancel) cancellation.Cancel();
                    return scenario.Decision != Decision.Decline;
                };
                try
                {
                    MidiSong song = MidiFileParser.LoadWithPreflight(path, cancellation.Token, progress, decision,
                        scenario.ForcePreflight, scenario.ForceWarning);
                    result.Outcome = song == null ? "declined" : String.Format(CultureInfo.InvariantCulture,
                        "ok events {0} notes {1} duration-us {2}", song.EventStore.Count, song.NoteCount, song.DurationMicroseconds);
                }
                catch (Exception ex) { result.Outcome = ErrorStatus(ex).Replace("status error", "error"); }
            }
            return result;
        }

        private static int Flow(string[] args)
        {
            string path = args[1];
            bool summary = false;
            string outPath = null;
            for (int i = 2; i < args.Length; i++)
            {
                if (args[i] == "--summary") summary = true;
                else if (args[i] == "--out" && i + 1 < args.Length) outPath = args[++i];
                else throw new ArgumentException("unknown flow option " + args[i]);
            }

            Trace trace = new Trace();
            trace.Line("# producer v1-reference (MIDIBottleneck v1.0.44 sources) " +
                (IntPtr.Size == 8 ? "x64" : "x86") + " clr " + Environment.Version);
            trace.Line("# input " + Path.GetFileName(path));
            trace.Line("trace-format 1");
            trace.Line("kind flow");
            trace.Line("mode " + (summary ? "summary" : "full"));
            trace.Line("pointer-size {0}", IntPtr.Size);
            foreach (FlowScenario scenario in FlowScenarios)
            {
                trace.Line("[scenario {0}]", scenario.Name);
                Body body = new Body(trace, summary);
                FlowResult result = RunFlow(path, scenario, -1, body);
                body.Finish();
                trace.Line("reports {0} decision-requested {1}", result.Reports, result.DecisionRequested ? 1 : 0);
                trace.Line("outcome " + result.Outcome);
            }
            if (!summary)
            {
                foreach (FlowScenario scenario in FlowScenarios)
                {
                    if (scenario.Name != "natural" && scenario.Name != "warning-accept") continue;
                    int total = RunFlow(path, scenario, -1, null).Reports;
                    trace.Line("[cancel-sweep {0}]", scenario.Name);
                    for (int k = 0; k <= total + 1; k++)
                    {
                        FlowResult result = RunFlow(path, scenario, k, null);
                        trace.Line("k {0} reports {1} decision-requested {2} outcome {3}", k, result.Reports,
                            result.DecisionRequested ? 1 : 0, result.Outcome);
                    }
                }
            }
            trace.Line("[end]");
            Emit(trace.Text, outPath);
            return 0;
        }

        private static void WritePreflight(Trace trace, string path, long fileSize)
        {
            trace.Line("[preflight]");
            MidiPreflightCounts counts;
            try { counts = MidiLargeFilePreflight.Scan(path, CancellationToken.None, null); }
            catch (Exception ex)
            {
                trace.Line(ErrorStatus(ex));
                WriteRequiresPreflight(trace, fileSize);
                return;
            }
            trace.Line("status ok");
            trace.Line("format {0}", counts.Format);
            trace.Line("tracks {0}", counts.TrackCount);
            trace.Line("division {0}", counts.TicksPerQuarterNote);
            trace.Line("dispatchable-events {0}", counts.DispatchableEventCount);
            trace.Line("note-ons {0}", counts.NoteOnCount);
            trace.Line("final-long-payload-bytes {0}", counts.FinalLongPayloadBytes);
            trace.Line("provisional-payload-bytes {0}", counts.ProvisionalPayloadBytes);
            trace.Line("tempo-changes {0}", counts.TempoChangeCount);
            trace.Line("source-value-entries {0}", counts.SourceValueEntryCount);
            trace.Line("chase-state-entries {0}", counts.ChaseStateEntryCount);
            trace.Line("chase-parameter-value-entries {0}", counts.ChaseParameterValueEntryCount);
            trace.Line("running-status-repairs {0}", counts.RunningStatusRepairCount);
            trace.Line("routing-lanes {0}", counts.RoutingLaneCount);
            trace.Line("routing-patch-state-events {0}", counts.RoutingPatchStateEventCount);
            trace.Line("routing-patch-timeline-upper-bound {0}", counts.RoutingPatchTimelineEntryUpperBound);
            trace.Line("routing-generated-state-upper-bound {0}", counts.RoutingGeneratedStateEventUpperBound);
            for (int track = 0; track < counts.TrackCount; track++)
                trace.Line("track {0} events {1} payload-bytes {2} tempo-changes {3}", track,
                    counts.EventsByTrack[track], counts.PayloadBytesByTrack[track], counts.TempoChangesByTrack[track]);
            trace.Line("source-slots " + Join(counts.SourceEntriesBySlot));
            trace.Line("routing-note-lanes-by-channel " + Join(counts.RoutingNoteLanesByChannel));
            trace.Line("routing-patch-state-by-channel " + Join(counts.RoutingPatchStateEventsByChannel));
            foreach (int pointerSize in new int[] { 4, 8 })
            {
                MidiMemoryProjection p = MidiLargeFilePreflight.Estimate(counts, pointerSize);
                trace.Line("estimate ptr{0} retained {1} peak-low {2} peak-high {3} provisional {4} merge-workspace {5} " +
                    "names {6} routing-retained {7} routing-peak {8} retained-with-routing {9} routing-peak-total {10} " +
                    "warning-threshold {11} architecture-risk {12} requires-warning {13}",
                    pointerSize, p.ProjectedRetainedBytes, p.ProjectedPeakLowBytes, p.ProjectedPeakHighBytes,
                    p.ProvisionalBytes, p.MergeWorkspaceBytes, p.RetainedNameAllowanceBytes,
                    p.RoutingRetainedAdditionalBytes, p.RoutingPeakAdditionalBytes,
                    p.ProjectedRetainedWithRoutingBytes, p.ProjectedRoutingPeakBytes, p.WarningThresholdBytes,
                    p.IsArchitectureRisk ? 1 : 0, MidiLargeFilePreflight.RequiresWarning(p) ? 1 : 0);
            }
            WriteRequiresPreflight(trace, fileSize);
        }

        private static void WriteRequiresPreflight(Trace trace, long fileSize)
        {
            trace.Line("requires-preflight ptr4 {0} ptr8 {1}",
                MidiLargeFilePreflight.RequiresPreflight(fileSize, 4) ? 1 : 0,
                MidiLargeFilePreflight.RequiresPreflight(fileSize, 8) ? 1 : 0);
        }

        private static void WriteSong(Trace trace, MidiSong song)
        {
            CompactMidiEventStore store = (CompactMidiEventStore)song.EventStore;
            trace.Line("status ok");
            trace.Line("format {0}", song.Format);
            trace.Line("tracks {0}", song.TrackCount);
            trace.Line("division {0}", song.TicksPerQuarterNote);
            trace.Line("note-count {0}", song.NoteCount);
            trace.Line("running-status-repairs {0}", song.NonstandardRunningStatusRepairCount);
            trace.Line("duration-us {0}", song.DurationMicroseconds);
            trace.Line("events {0}", store.Count);
            trace.Line("payload-bytes {0}", store.Payloads.Length);
            for (int track = 0; track < song.TrackCount; track++)
            {
                MidiTrackMetadata m = song.MetadataForTrack(track);
                trace.Line("track {0} name {1} name-truncated {2} instrument {3} instrument-truncated {4}", track,
                    Quote(m.TrackName), m.TrackNameTruncated ? 1 : 0, Quote(m.InstrumentName),
                    m.InstrumentNameTruncated ? 1 : 0);
            }
        }

        private static void WriteEvents(Trace trace, MidiSong song, bool summary)
        {
            trace.Line("[events]");
            IMidiEventStore store = song.EventStore;
            trace.Line("count {0}", store.Count);
            Body body = new Body(trace, summary);
            using (SHA256 payloadHash = SHA256.Create())
            {
                StringBuilder line = new StringBuilder(96);
                for (int index = 0; index < store.Count; index++)
                {
                    MidiEventView e = store.GetEvent(index);
                    line.Length = 0;
                    line.Append("e ").Append(index.ToString(CultureInfo.InvariantCulture))
                        .Append(' ').Append(e.AbsoluteTick.ToString(CultureInfo.InvariantCulture))
                        .Append(' ').Append(e.IntendedMicroseconds.ToString(CultureInfo.InvariantCulture))
                        .Append(' ').Append(e.Track.ToString(CultureInfo.InvariantCulture))
                        .Append(' ').Append(e.Kind.ToString())
                        .Append(' ').Append(e.Channel.ToString(CultureInfo.InvariantCulture))
                        .Append(' ').Append(e.Status.ToString("X2", CultureInfo.InvariantCulture))
                        .Append(' ').Append(e.DataLength.ToString(CultureInfo.InvariantCulture))
                        .Append(' ');
                    int length = e.DataLength;
                    if (length == 0) line.Append('-');
                    else if (length <= 3)
                    {
                        for (int b = 0; b < length; b++)
                            line.Append(e.GetDataByte(b).ToString("X2", CultureInfo.InvariantCulture));
                    }
                    else line.Append("sha256:").Append(Hex(payloadHash.ComputeHash(e.Data.ToArray())));
                    body.Line(line.ToString());
                }
            }
            body.Finish();
        }

        // Re-parses each track with v1's private ParseTrack and replays the
        // collected tempo changes through v1's private TempoClock.
        private static void WriteTempo(Trace trace, string path, bool summary)
        {
            Type parser = typeof(DirectCompactMidiParser);
            MethodInfo parseTrack = parser.GetMethod("ParseTrack", Hidden);
            Type parsedTrackType = parser.GetNestedType("ParsedTrack", Hidden);
            Type tempoStoreType = parser.GetNestedType("TempoStore", Hidden);
            Type tempoRecordType = parser.GetNestedType("TempoRecord", Hidden);
            Type tempoClockType = parser.GetNestedType("TempoClock", Hidden);
            FieldInfo temposField = parsedTrackType.GetField("Tempos", Hidden);
            FieldInfo eventsField = parsedTrackType.GetField("Events", Hidden);
            FieldInfo countField = tempoStoreType.GetField("Count", Hidden);
            PropertyInfo itemProperty = tempoStoreType.GetProperty("Item", Hidden);
            FieldInfo tickField = tempoRecordType.GetField("Tick", Hidden);
            FieldInfo tempoField = tempoRecordType.GetField("MicrosecondsPerQuarter", Hidden);
            FieldInfo trackField = tempoRecordType.GetField("Track", Hidden);
            FieldInfo orderField = tempoRecordType.GetField("Order", Hidden);
            if (parseTrack == null || parsedTrackType == null || tempoClockType == null || itemProperty == null)
                throw new InvalidOperationException("v1 private parser types were not found");

            List<long[]> changes = new List<long[]>();
            Array tracks;
            int division;
            using (MidiFileContent content = MidiFileContent.Open(path))
            using (BinaryReader reader = new BinaryReader(content.Stream))
            {
                Stream stream = content.Stream;
                DirectCompactMidiParser.RequireChunk(reader, "MThd");
                uint headerLength = DirectCompactMidiParser.ReadUInt32BigEndian(reader);
                DirectCompactMidiParser.ReadUInt16BigEndian(reader);
                int trackCount = DirectCompactMidiParser.ReadUInt16BigEndian(reader);
                division = DirectCompactMidiParser.ReadUInt16BigEndian(reader);
                if (headerLength > 6) DirectCompactMidiParser.SkipStream(stream, headerLength - 6, CancellationToken.None);
                tracks = Array.CreateInstance(parsedTrackType, trackCount);
                long fileLength = Math.Max(1, stream.Length);
                byte[] buffer = new byte[65536];
                for (int track = 0; track < trackCount; track++)
                {
                    DirectCompactMidiParser.RequireChunk(reader, "MTrk");
                    uint length = DirectCompactMidiParser.ReadUInt32BigEndian(reader);
                    DirectCompactMidiParser.TrackReader trackReader = new DirectCompactMidiParser.TrackReader(
                        stream, length, CancellationToken.None, null, buffer);
                    object parsed = parseTrack.Invoke(null, new object[] { trackReader, track,
                        CancellationToken.None, null, trackCount, 0L, fileLength, length });
                    eventsField.SetValue(parsed, null);
                    tracks.SetValue(parsed, track);
                    object tempos = temposField.GetValue(parsed);
                    int count = (int)countField.GetValue(tempos);
                    for (int i = 0; i < count; i++)
                    {
                        object record = itemProperty.GetValue(tempos, new object[] { i });
                        changes.Add(new long[] { (long)tickField.GetValue(record), (int)tempoField.GetValue(record),
                            (int)trackField.GetValue(record), (int)orderField.GetValue(record) });
                    }
                }
            }
            changes.Sort(delegate(long[] a, long[] b)
            {
                int c = a[0].CompareTo(b[0]);
                if (c != 0) return c;
                c = a[2].CompareTo(b[2]);
                return c != 0 ? c : a[3].CompareTo(b[3]);
            });
            object clock = Activator.CreateInstance(tempoClockType, Hidden, null, new object[] { tracks, division }, null);
            MethodInfo timeAt = tempoClockType.GetMethod("TimeAt", Hidden);

            trace.Line("[tempo]");
            trace.Line("count {0}", changes.Count);
            Body body = new Body(trace, summary);
            foreach (long[] change in changes)
            {
                long time = (long)timeAt.Invoke(clock, new object[] { change[0] });
                body.Line(String.Format(CultureInfo.InvariantCulture, "t {0} {1} {2} {3} {4}",
                    change[0], time, change[1], change[2], change[3]));
            }
            body.Finish();
        }

        private static string ErrorStatus(Exception ex)
        {
            while (ex is TargetInvocationException && ex.InnerException != null) ex = ex.InnerException;
            return "status error " + ex.GetType().Name + " " + Quote(ex.Message);
        }

        private static string Quote(string value)
        {
            StringBuilder result = new StringBuilder(value.Length + 2);
            result.Append('"');
            foreach (char c in value)
            {
                if (c == '"') result.Append("\\\"");
                else if (c == '\\') result.Append("\\\\");
                else if (c >= 0x20 && c <= 0x7E) result.Append(c);
                else result.Append("\\u").Append(((int)c).ToString("X4", CultureInfo.InvariantCulture));
            }
            return result.Append('"').ToString();
        }

        private static string Join(long[] values)
        {
            StringBuilder result = new StringBuilder();
            for (int i = 0; i < values.Length; i++)
            {
                if (i != 0) result.Append(' ');
                result.Append(values[i].ToString(CultureInfo.InvariantCulture));
            }
            return result.ToString();
        }

        private static string Hex(byte[] bytes)
        {
            StringBuilder result = new StringBuilder(bytes.Length * 2);
            foreach (byte b in bytes) result.Append(b.ToString("X2", CultureInfo.InvariantCulture));
            return result.ToString();
        }

        // ------------------------------------------------------ decimal vectors

        // Each line replays v1's TempoClock arithmetic with real .NET decimal:
        //   D <ppqn> <steps> {<delta> <tempo>}*steps <final-delta> <final-tempo> <accumulated> <value> <rounded>
        // where accumulated = sum of (delta*tempo)/ppqn in order, value =
        // accumulated + (final-delta*final-tempo)/ppqn, rounded = Round(value, 0,
        // AwayFromZero), or "OverflowException" when the rounded value does not
        // fit Int64 (v1 would fail the load). Decimal strings are invariant with
        // trailing zeros removed.
        private static int DecimalVectors(int count, int seed, string outPath)
        {
            Random random = new Random(seed);
            int[] commonPpqn = { 1, 2, 3, 6, 7, 9, 24, 48, 96, 120, 192, 240, 384, 480, 960, 1000, 32767, 32766 };
            StringBuilder text = new StringBuilder();
            text.Append("# .NET decimal tempo-clock vectors; seed ").Append(seed).Append(", clr ")
                .Append(Environment.Version).Append('\n');
            for (int vector = 0; vector < count; vector++)
            {
                int ppqn = random.Next(4) == 0 ? 1 + random.Next(32767) : commonPpqn[random.Next(commonPpqn.Length)];
                int steps = random.Next(12);
                decimal accumulated = 0m;
                StringBuilder line = new StringBuilder();
                line.Append("D ").Append(ppqn).Append(' ').Append(steps);
                for (int step = 0; step < steps; step++)
                {
                    long delta = RandomDelta(random);
                    int tempo = RandomTempo(random);
                    accumulated += ((decimal)delta * tempo) / ppqn;
                    line.Append(' ').Append(delta).Append(' ').Append(tempo);
                }
                long finalDelta = RandomDelta(random);
                int finalTempo = RandomTempo(random);
                decimal value = accumulated + ((decimal)finalDelta * finalTempo) / ppqn;
                string rounded;
                try
                {
                    rounded = Decimal.ToInt64(Decimal.Round(value, 0, MidpointRounding.AwayFromZero))
                        .ToString(CultureInfo.InvariantCulture);
                }
                catch (OverflowException) { rounded = "OverflowException"; }
                line.Append(' ').Append(finalDelta).Append(' ').Append(finalTempo)
                    .Append(' ').Append(Normalize(accumulated)).Append(' ').Append(Normalize(value))
                    .Append(' ').Append(rounded);
                text.Append(line).Append('\n');
            }
            File.WriteAllBytes(outPath, Encoding.ASCII.GetBytes(text.ToString()));
            return 0;
        }

        private static long RandomDelta(Random random)
        {
            switch (random.Next(6))
            {
                case 0: return random.Next(4);
                case 1: return random.Next(1000);
                case 2: return random.Next(0x0FFFFFFF);
                case 3: return (long)random.Next(1 << 20) * random.Next(1 << 20);
                case 4: return (long)random.Next(0x0FFFFFFF) * 4096 + random.Next(4096);
                default: return random.Next(100000);
            }
        }

        private static int RandomTempo(Random random)
        {
            switch (random.Next(4))
            {
                case 0: return 1 + random.Next(16);
                case 1: return 500000;
                case 2: return 0xFFFFFF - random.Next(16);
                default: return 1 + random.Next(0xFFFFFF);
            }
        }

        private static string Normalize(decimal value)
        {
            string text = value.ToString(CultureInfo.InvariantCulture);
            if (text.IndexOf('.') >= 0) text = text.TrimEnd('0').TrimEnd('.');
            return text;
        }

        // -------------------------------------------------- decimal primitive ops

        // Exact .NET decimal results for the primitive operations v2 emulates,
        // as raw bits (lo mid hi scale, unsigned hex/decimal):
        //   M <a> <b> = <r>                          (decimal)a * b, a ulong, b uint
        //   V <lo> <mid> <hi> <divisor> = <r>        scale-0 dividend / positive int
        //   A <a: lo mid hi scale> <b: lo mid hi scale> = <r>|OverflowException
        //   R <lo> <mid> <hi> <scale> = <int64>|OverflowException   ToInt64(Round(v, 0, AwayFromZero))
        // where <r> is "<lo> <mid> <hi> <scale>". Targeted boundary values come
        // first, then seeded random values.
        private static int DecimalOps(int seed, string outPath)
        {
            StringBuilder text = new StringBuilder();
            text.Append("# .NET decimal primitive-operation vectors; seed ").Append(seed).Append(", clr ")
                .Append(Environment.Version).Append('\n');
            Random random = new Random(seed);

            ulong[] multiplicands = { 0, 1, 2, 3, 10, 0xFFFFFFFFUL, 0x100000000UL, 0x0FFFFFFFUL, 999999999999UL,
                0x7FFFFFFFFFFFFFFFUL, 0x8000000000000000UL, 0xFFFFFFFFFFFFFFFFUL };
            uint[] multipliers = { 0, 1, 2, 3, 7, 500000, 0xFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF };
            foreach (ulong a in multiplicands)
                foreach (uint b in multipliers)
                    text.Append("M ").Append(a).Append(' ').Append(b).Append(" = ").Append(Bits((decimal)a * b)).Append('\n');

            List<decimal> dividends = new List<decimal>();
            foreach (decimal d in new decimal[] { 0m, 1m, 2m, 3m, 5m, 7m, 9m, 10m, 11m, 99m, 100m, 101m, 2147483647m,
                4294967295m, 4294967296m, 9223372036854775807m, 18446744073709551615m,
                79228162514264337593543950335m, 79228162514264337593543950334m, 39614081257132168796771975167m,
                9999999999999999999999999999m, 10000000000000000000000000000m, 7922816251426433759354395033m,
                7922816251426433759354395034m, 154742504910672534362390527m /* 2^87 - 1 */ })
                dividends.Add(d);
            decimal power = 1m;
            for (int k = 1; k <= 28; k++) { power *= 10m; dividends.Add(power); dividends.Add(power - 1m); dividends.Add(power + 1m); }
            int[] divisors = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 24, 25, 40, 48, 96, 125, 192, 480, 960,
                1000, 1024, 3125, 32766, 32767, 65535, 999999, 1000000, 2147483646, 2147483647 };
            foreach (decimal dividend in dividends)
                foreach (int divisor in divisors)
                {
                    int[] bits = Decimal.GetBits(dividend);
                    text.Append("V ").Append(Unsigned(bits[0])).Append(' ').Append(Unsigned(bits[1])).Append(' ')
                        .Append(Unsigned(bits[2])).Append(' ').Append(divisor).Append(" = ").Append(Bits(dividend / divisor)).Append('\n');
                }

            // Add: mantissas near 2^96 and powers of ten, at many scales, so the
            // overflow scale-down, half-even ties, sticky digits and the
            // exactly-2^96 rounding case are all exercised.
            decimal[] mantissas = { 0m, 1m, 5m, 6m, 15m, 50m, 51m, 18446744073709551615m,
                39614081257132168796771975168m, 79228162514264337593543950335m, 79228162514264337593543950334m,
                79228162514264337593543950325m, 9999999999999999999999999999m, 50000000000000000000000000000m,
                7922816251426433759354395033m, 7922816251426433759354395035m };
            int[] scales = { 0, 1, 2, 9, 18, 27, 28 };
            List<decimal> addends = new List<decimal>();
            foreach (decimal m in mantissas)
                foreach (int s in scales) addends.Add(WithScale(m, s));
            foreach (decimal a in addends)
                foreach (decimal b in addends)
                    AppendAdd(text, a, b);
            for (int i = 0; i < 4000; i++)
                AppendAdd(text, RandomDecimal(random), RandomDecimal(random));

            // Round away from zero and ToInt64 near .5 and the Int64 boundary.
            List<decimal> roundInputs = new List<decimal>();
            foreach (decimal m in mantissas)
                foreach (int s in scales) roundInputs.Add(WithScale(m, s));
            foreach (string literal in new string[] { "0.5", "1.5", "2.5", "0.4999999999999999999999999999",
                "0.5000000000000000000000000001", "9223372036854775807", "9223372036854775807.4",
                "9223372036854775807.5", "9223372036854775806.5", "9223372036854775806.4999999999",
                "9223372036854775808", "9223372036854775807.49999", "4999999999.5", "123456789.0000000001",
                "999999999.4999999999", "999999999.5000000000", "0.0000000000000000000000000005" })
                roundInputs.Add(Decimal.Parse(literal, CultureInfo.InvariantCulture));
            for (int i = 0; i < 2000; i++) roundInputs.Add(RandomDecimal(random));
            foreach (decimal value in roundInputs)
            {
                string rounded;
                try
                {
                    rounded = Decimal.ToInt64(Decimal.Round(value, 0, MidpointRounding.AwayFromZero))
                        .ToString(CultureInfo.InvariantCulture);
                }
                catch (OverflowException) { rounded = "OverflowException"; }
                text.Append("R ").Append(Bits(value)).Append(" = ").Append(rounded).Append('\n');
            }
            File.WriteAllBytes(outPath, Encoding.ASCII.GetBytes(text.ToString()));
            return 0;
        }

        private static void AppendAdd(StringBuilder text, decimal a, decimal b)
        {
            string result;
            try { result = Bits(a + b); }
            catch (OverflowException) { result = "OverflowException"; }
            text.Append("A ").Append(Bits(a)).Append(' ').Append(Bits(b)).Append(" = ").Append(result).Append('\n');
        }

        private static decimal WithScale(decimal mantissa, int scale)
        {
            int[] bits = Decimal.GetBits(mantissa);
            return new decimal(bits[0], bits[1], bits[2], false, (byte)scale);
        }

        private static decimal RandomDecimal(Random random)
        {
            int hiBits = random.Next(5);
            int lo = random.Next() ^ (random.Next(2) << 31);
            int mid = hiBits >= 1 ? random.Next() ^ (random.Next(2) << 31) : 0;
            int hi = hiBits >= 2 ? (hiBits == 4 ? -1 - random.Next(1000) : random.Next() ^ (random.Next(2) << 31)) : 0;
            return new decimal(lo, mid, hi, false, (byte)random.Next(29));
        }

        private static string Unsigned(int value) { return ((uint)value).ToString(CultureInfo.InvariantCulture); }

        private static string Bits(decimal value)
        {
            int[] bits = Decimal.GetBits(value);
            return Unsigned(bits[0]) + " " + Unsigned(bits[1]) + " " + Unsigned(bits[2]) + " " +
                ((bits[3] >> 16) & 0xFF).ToString(CultureInfo.InvariantCulture);
        }

        // ------------------------------------------------------------ perf-load

        private static int PerfLoad(string path)
        {
            Stopwatch timer = Stopwatch.StartNew();
            MidiSong song = DirectCompactMidiParser.Load(path, CancellationToken.None, null);
            timer.Stop();
            Process self = Process.GetCurrentProcess();
            self.Refresh();
            long retained = GC.GetTotalMemory(true);
            self.Refresh();
            Console.WriteLine("implementation=v1-reference");
            Console.WriteLine("architecture=" + (IntPtr.Size == 8 ? "x64" : "x86"));
            Console.WriteLine("events=" + song.EventStore.Count);
            Console.WriteLine("load-ms=" + timer.Elapsed.TotalMilliseconds.ToString("F1", CultureInfo.InvariantCulture));
            Console.WriteLine("peak-working-set-bytes=" + self.PeakWorkingSet64);
            Console.WriteLine("peak-private-bytes=" + self.PeakPagedMemorySize64);
            Console.WriteLine("private-bytes-after-load=" + self.PrivateMemorySize64);
            Console.WriteLine("managed-retained-bytes=" + retained);
            GC.KeepAlive(song);
            return 0;
        }
    }
}
