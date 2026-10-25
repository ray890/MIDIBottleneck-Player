// Recorded property checks for the real-time engine runs (Milestone 5 exit
// test). Reads the logs written by v1-realtime.exe and midibn-engine-realtime.exe
// (the same format; v2/docs/ENGINE_PROPERTY_CHECKS.md, "Log format"), checks every
// property of that document on each log, and compares the logs of one
// configuration across repeats, programs and architectures.
//
// Compiled by tools/engine-properties.ps1 with the .NET Framework csc.exe
// (C# 5; no third-party code).
//
// Usage:
//   engine-property-checker.exe <eventsDir> <logsDir> <resultFile>
// <logsDir> holds <program>-<arch>-<scenario>-<fixture>-r<rep>.log files;
// <eventsDir> holds <fixture>.txt (the song event lists the programs dump).
// The result file is a line-oriented report the script prints:
//   PROP <id> <group> <checks> <failures>          one per property and group
//   FAIL <id> <group> <log> <detail>               first failures
//   INFO <topic> <text>                            observations (not pass/fail)
//   NOTE <text>                                    other findings
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace EngineProperties
{
    internal enum K { Send, Reset, Panic, Snap, Cmd, CmdDone, CmdEx, Event, Final, End, Begin, Other }

    internal sealed class Rec
    {
        internal K Kind;
        internal long T;
        internal int Index;
        // Send
        internal string SendKind;
        internal int Src;
        internal string Hex;
        internal bool Source;       // a source event (as opposed to a generated message)
        // Snap / Final
        internal string State;
        internal Dictionary<string, long> V;
        // Cmd / Event / Other
        internal string Text;

        internal long Get(string key) { long v; return V != null && V.TryGetValue(key, out v) ? v : 0; }
    }

    internal sealed class SongInfo
    {
        internal string Name;
        internal int Count;
        internal long Duration;
        internal long[] Time;
        internal string[] Kind;
        internal string[] Hex;

        internal static SongInfo Load(string path)
        {
            string[] lines = File.ReadAllLines(path);
            SongInfo song = new SongInfo();
            song.Name = Path.GetFileNameWithoutExtension(path);
            string[] header = lines[0].Split(' ');
            song.Count = Int32.Parse(header[1].Substring(6), CultureInfo.InvariantCulture);
            song.Duration = Int64.Parse(header[2].Substring(9), CultureInfo.InvariantCulture);
            song.Time = new long[song.Count];
            song.Kind = new string[song.Count];
            song.Hex = new string[song.Count];
            for (int i = 0; i < song.Count; i++)
            {
                string[] f = lines[i + 1].Split(' ');
                song.Time[i] = Int64.Parse(f[1], CultureInfo.InvariantCulture);
                song.Kind[i] = f[2];
                song.Hex[i] = f[3];
            }
            return song;
        }

        // First index whose time is >= t (v1 LowerBoundByTime).
        internal int LowerBound(long t)
        {
            int lo = 0, hi = Count;
            while (lo < hi)
            {
                int mid = lo + (hi - lo) / 2;
                if (Time[mid] < t) lo = mid + 1; else hi = mid;
            }
            return lo;
        }

        internal int ClampedLowerBound(long t)
        {
            if (t < 0) t = 0;
            if (t > Duration) t = Duration;
            return LowerBound(t);
        }

        internal int Status(int i) { return Convert.ToInt32(Hex[i].Substring(0, 2), 16); }
        internal int Byte(int i, int n)
        {
            return Hex[i].Length >= 2 * n + 2 ? Convert.ToInt32(Hex[i].Substring(2 * n, 2), 16) : 0;
        }
        // CompleteNoteTracker.Classify: 1 NoteOn, 2 NoteOff, 0 other.
        internal int NoteKind(int i)
        {
            if (Hex[i].Length < 6) return 0;
            int command = Status(i) & 0xF0;
            if (command == 0x90) return Byte(i, 2) == 0 ? 2 : 1;
            return command == 0x80 ? 2 : 0;
        }
        // CompleteNoteTracker.IsSafetyControl.
        internal bool IsSafety(int i)
        {
            if (Hex[i].Length < 6 || (Status(i) & 0xF0) != 0xB0) return false;
            int controller = Byte(i, 1), value = Byte(i, 2);
            return (controller == 64 && value < 64) || (controller >= 120 && controller <= 127);
        }
    }

    internal sealed class Cmd
    {
        internal string Name;
        internal string Verb;          // Start, Stop, Pause, Resume, Seek, Set, Service, Override, Channel, Snapshot, Other
        internal int Rec = -1;         // index of the CMD record
        internal int Done = -1;        // index of the CMD-DONE record
        internal Rec Pre;              // the snapshot logged just before
        internal Rec Post;             // the first snapshot after CMD-DONE
        internal string Mode;          // Start
        internal long Pos;             // Start position / Seek target
        internal bool Paused;          // Start ... paused
        internal int Channel;          // Override / Channel
        internal string Attribute;     // Override
        internal string Value;         // Override value text, or "1"/"0" for Channel
        internal bool Acting;          // Pause/Resume that changed the state
        internal bool Preserved;       // ordered (queue-preserving) Pause or Seek
        internal bool Ordered;         // the ordered path was selected when it ran
        internal bool Chase = true;
        internal string Policy = "DropNewest";
        internal bool AgeLimit;
        internal bool ForwardOnly;
        internal long Limit = 2000;
    }

    internal sealed class Seg
    {
        internal string Kind;          // Start, Seek, Resume, SeekOrdered
        internal Cmd Command;
        internal int StartRec;         // first record that belongs to it
        internal int EndRec;           // exclusive
        internal long StartPos;
        internal int StartIdx;
        internal bool StatsReset;
        internal bool Ordered;
        internal string Mode;
        internal bool Chase;
        internal string Policy;
        internal bool AgeLimit;
        internal bool ForwardOnly;
        internal long Limit;
        internal bool Ended;           // PlaybackEnded inside
        internal List<Rec> Src = new List<Rec>();
        internal List<Rec> Gen = new List<Rec>();
        internal List<Rec> Chased = new List<Rec>();   // generated messages before the first source send
    }

    internal sealed class Log
    {
        internal string File, Program, Arch, Scenario, Fixture;
        internal int Rep;
        internal string Group { get { return Program + "-" + Arch; } }
        internal string Key { get { return Scenario + "/" + Fixture + "/" + Group + "/r" + Rep; } }
        internal Dictionary<string, string> Tags = new Dictionary<string, string>();
        internal List<string[]> Setup = new List<string[]>();
        internal List<Rec> R = new List<Rec>();
        internal List<Cmd> Cmds = new List<Cmd>();
        internal List<Seg> Segs = new List<Seg>();
        internal SongInfo Song;
        internal bool HasOverrides;    // any Override / Channel command or setup line
        internal int Ends;
        internal Dictionary<string, string> Settings = new Dictionary<string, string>();
        internal string Class { get { string v; return Tags.TryGetValue("class", out v) ? v : "prop"; } }
    }

    internal sealed class PropertyResult
    {
        internal long Checks;
        internal long Failures;
        internal List<string> Details = new List<string>();
    }

    internal static class Program
    {
        // property id -> text (printed by the script)
        internal static readonly string[][] Properties = new string[][]
        {
            new[] { "T01", "terminates: END reached, no timeout or watchdog, every command returned without an exception" },
            new[] { "T02", "no PlaybackFailed or ChannelControlFailed event" },
            new[] { "T03", "PlaybackEnded fires exactly as often as the scenario ends playback; Completed before the final Stop, Stopped after" },
            new[] { "S01", "Stop, Start, classic Pause and classic Seek each end in one RESET followed at once by one PANIC (none for the first Start or a queue-preserving Pause/Seek)" },
            new[] { "S02", "nothing reaches the output between a Stop and the next Start" },
            new[] { "S03", "Paused: nothing is sent and processed/position stay frozen" },
            new[] { "S04", "Resume after a classic Pause restarts at the first event at or after the frozen position; an ordered Pause loses and repeats nothing" },
            new[] { "S05", "Seek restarts at the first event at or after the target (classic: after RESET+PANIC; ordered: retained queue, then 16 x CC123 marker)" },
            new[] { "S06", "chase messages after Start/Seek/Resume at a position are sound state restores (none at position 0 or with the chase off)" },
            new[] { "S07", "source events are sent in song order, once each, with no gaps in Queue modes" },
            new[] { "S08", "every sent source event carries exactly the song's bytes (SysEx included)" },
            new[] { "C01", "processed equals the number of source events sent (at every quiet point); never more while playing" },
            new[] { "C02", "count conservation: processed + dropped + gate counters = events from the start position (per v1's own counters)" },
            new[] { "C03", "snapshot counters never decrease between restarts" },
            new[] { "P01", "Drop newest events / Drop oldest events: NoteOff and safety controls are never dropped" },
            new[] { "P02", "complete-note policies: a NoteOff is dropped exactly when its NoteOn was; safety controls and unmatched NoteOffs never" },
            new[] { "P03", "PANIC during play only from Clear buffer (at least one if it dropped), never from the other policies" },
            new[] { "Q01", "a finite queue drops only when its limit was reached; an ample limit drops nothing" },
            new[] { "Q02", "snapshot sanity: counters non-negative, position/intended/lag within range, forward-only virtual queue within its limit" },
            new[] { "O01", "override: forced message sent once, then conflicting source events are suppressed" },
            new[] { "O02", "channel disable: CC64/CC120/CC123 silence, then nothing from that channel until enabled" },
            new[] { "M01", "no event is dispatched before its due time" },
            new[] { "L01", "dispatch lateness (median and 99th percentile behind the due time): v2 no worse than v1 (equal or better; 25 % + 0.2/1 ms noise margin)" },
            new[] { "J01", "dispatch jitter (99th percentile minus median lateness): v2 no worse than v1 (25 % + 1 ms noise margin)" },
            new[] { "K01", "command-to-effect latency (override, channel disable; median and worst): v2 no worse than v1 (25 % + 0.5/5 ms noise margin)" },
            new[] { "X01", "exact class: the sent sequence is identical across repeats, programs and architectures" },
            new[] { "X02", "exact class: the final counters are identical across repeats, programs and architectures" },
            new[] { "X03", "the chase block for a start position is identical wherever it occurs" },
        };

        static readonly Dictionary<string, Dictionary<string, PropertyResult>> Results =
            new Dictionary<string, Dictionary<string, PropertyResult>>();
        static readonly List<string> Info = new List<string>();
        static readonly List<string> Notes = new List<string>();

        static void Check(string id, string group, string log, bool ok, string detail)
        {
            Dictionary<string, PropertyResult> byGroup;
            if (!Results.TryGetValue(id, out byGroup)) Results[id] = byGroup = new Dictionary<string, PropertyResult>();
            PropertyResult r;
            if (!byGroup.TryGetValue(group, out r)) byGroup[group] = r = new PropertyResult();
            r.Checks++;
            if (!ok)
            {
                r.Failures++;
                if (r.Details.Count < 6) r.Details.Add(log + " :: " + detail);
            }
        }

        static void Note(string text) { if (!Notes.Contains(text)) Notes.Add(text); }

        static readonly Dictionary<string, long[]> Uncounted = new Dictionary<string, long[]>();

        static readonly Dictionary<string, List<long>> Latencies = new Dictionary<string, List<long>>();

        // command-to-effect latency (information only)
        static void ControlLatency(string group, string what, long microseconds)
        {
            string key = group + " " + what;
            List<long> list;
            if (!Latencies.TryGetValue(key, out list)) Latencies[key] = list = new List<long>();
            list.Add(microseconds);
        }

        static int Main(string[] args)
        {
            if (args.Length < 3) { Console.WriteLine("usage: engine-property-checker <eventsDir> <logsDir> <resultFile>"); return 2; }
            string eventsDir = args[0], logsDir = args[1], resultFile = args[2];
            Dictionary<string, SongInfo> songs = new Dictionary<string, SongInfo>();
            foreach (string f in Directory.GetFiles(eventsDir, "*.txt"))
            {
                // <fixture>.v1.txt and <fixture>.v2.txt: the song as each program reads it (they differ only by DD-001)
                string stem = Path.GetFileNameWithoutExtension(f);
                int dot = stem.LastIndexOf('.');
                if (dot < 0) continue;
                SongInfo s = SongInfo.Load(f);
                s.Name = stem.Substring(0, dot);
                songs[stem.Substring(dot + 1) + ":" + s.Name] = s;
            }
            List<Log> logs = new List<Log>();
            string[] files = Directory.GetFiles(logsDir, "*.log");
            Array.Sort(files, StringComparer.Ordinal);
            foreach (string f in files)
            {
                Log log = ParseLog(f);
                if (log == null) continue;
                if (!songs.TryGetValue(log.Program + ":" + log.Fixture, out log.Song)) { Note("no event list for fixture " + log.Fixture + " (" + log.Program + ")"); continue; }
                logs.Add(log);
            }
            foreach (Log log in logs)
            {
                Interpret(log);
                RunChecks(log);
            }
            CrossChecks(logs);
            Timing(logs);
            Spread(logs);
            Write(resultFile, logs);
            return 0;
        }

        // ---------------------------------------------------------------- parsing

        static Log ParseLog(string path)
        {
            string[] lines = File.ReadAllLines(path);
            if (lines.Length == 0) return null;
            Log log = new Log();
            log.File = Path.GetFileName(path);
            string name = Path.GetFileNameWithoutExtension(path);
            int rIndex = name.LastIndexOf("-r", StringComparison.Ordinal);
            log.Rep = Int32.Parse(name.Substring(rIndex + 2), CultureInfo.InvariantCulture);
            for (int i = 0; i < lines.Length; i++)
            {
                string line = lines[i];
                if (line.Length == 0) continue;
                if (line.StartsWith("RUN ", StringComparison.Ordinal))
                {
                    foreach (string token in line.Substring(4).Split(' '))
                    {
                        int eq = token.IndexOf('=');
                        if (eq < 0) continue;
                        string key = token.Substring(0, eq), value = token.Substring(eq + 1);
                        if (key == "program") log.Program = value;
                        else if (key == "arch") log.Arch = value;
                        else if (key == "scenario") log.Scenario = value;
                        else if (key == "fixture") log.Fixture = value;
                    }
                    continue;
                }
                if (line.StartsWith("DESCRIBE ", StringComparison.Ordinal))
                {
                    string text = line.Substring(9);
                    int bar = text.IndexOf('|');
                    string tags = bar < 0 ? text : text.Substring(0, bar);
                    foreach (string token in tags.Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        int eq = token.IndexOf('=');
                        if (eq > 0) log.Tags[token.Substring(0, eq)] = token.Substring(eq + 1);
                    }
                    continue;
                }
                if (line.StartsWith("PLAN-SETUP ", StringComparison.Ordinal))
                {
                    log.Setup.Add(line.Substring(11).Split(' '));
                    continue;
                }
                if (line.StartsWith("PLAN-AT ", StringComparison.Ordinal)) continue;
                int space = line.IndexOf(' ');
                long t;
                if (space < 0 || !Int64.TryParse(line.Substring(0, space), NumberStyles.None, CultureInfo.InvariantCulture, out t))
                    continue;
                string rest = line.Substring(space + 1);
                Rec rec = new Rec();
                rec.T = t;
                rec.Index = log.R.Count;
                if (rest.StartsWith("SEND ", StringComparison.Ordinal))
                {
                    string[] f = rest.Split(' ');
                    rec.Kind = K.Send;
                    rec.SendKind = f[1];
                    rec.Src = Int32.Parse(f[2].Substring(4), CultureInfo.InvariantCulture);
                    rec.Hex = f[3];
                }
                else if (rest == "RESET") rec.Kind = K.Reset;
                else if (rest == "PANIC") rec.Kind = K.Panic;
                else if (rest.StartsWith("SNAP ", StringComparison.Ordinal) || rest.StartsWith("FINAL ", StringComparison.Ordinal))
                {
                    rec.Kind = rest[0] == 'S' ? K.Snap : K.Final;
                    rec.V = new Dictionary<string, long>();
                    foreach (string token in rest.Substring(rest.IndexOf(' ') + 1).Split(' '))
                    {
                        int eq = token.IndexOf('=');
                        if (eq < 0) continue;
                        string key = token.Substring(0, eq), value = token.Substring(eq + 1);
                        if (key == "state") rec.State = value;
                        else rec.V[key] = Int64.Parse(value, CultureInfo.InvariantCulture);
                    }
                }
                else if (rest.StartsWith("CMD-DONE", StringComparison.Ordinal)) rec.Kind = K.CmdDone;
                else if (rest.StartsWith("CMD-EXCEPTION ", StringComparison.Ordinal)) { rec.Kind = K.CmdEx; rec.Text = rest; }
                else if (rest.StartsWith("CMD ", StringComparison.Ordinal)) { rec.Kind = K.Cmd; rec.Text = rest.Substring(4); }
                else if (rest.StartsWith("EVENT ", StringComparison.Ordinal)) { rec.Kind = K.Event; rec.Text = rest.Substring(6); }
                else if (rest == "END") rec.Kind = K.End;
                else if (rest == "BEGIN") rec.Kind = K.Begin;
                else { rec.Kind = K.Other; rec.Text = rest; }
                log.R.Add(rec);
            }
            string ends;
            log.Ends = log.Tags.TryGetValue("ends", out ends) ? Int32.Parse(ends, CultureInfo.InvariantCulture) : 0;
            return log;
        }

        // ---------------------------------------------------------------- interpretation

        static string Kebab(string pascal)
        {
            StringBuilder text = new StringBuilder();
            for (int i = 0; i < pascal.Length; i++)
            {
                char c = pascal[i];
                if (Char.IsUpper(c)) { if (i > 0) text.Append('-'); text.Append(Char.ToLowerInvariant(c)); }
                else text.Append(c);
            }
            return text.ToString();
        }

        static void ApplySetting(Dictionary<string, string> settings, string key, string value) { settings[key] = value; }

        static string BoolText(string v) { return v == "1" ? "on" : v == "0" ? "off" : v; }

        static void Interpret(Log log)
        {
            SongInfo song = log.Song;
            Dictionary<string, string> settings = log.Settings;
            foreach (string[] line in log.Setup)
            {
                if (line[0] == "set" && line.Length >= 3) settings[line[1]] = line[2];
                else if (line[0] == "override" || line[0] == "channel") log.HasOverrides = true;
            }
            Func<string, string, string> get = delegate(string key, string def)
            { string v; return settings.TryGetValue(key, out v) ? v : def; };

            // commands
            Cmd current = null;
            Rec lastSnap = null;
            for (int i = 0; i < log.R.Count; i++)
            {
                Rec r = log.R[i];
                if (r.Kind == K.Snap)
                {
                    if (current != null && current.Done >= 0 && current.Post == null) current.Post = r;
                    lastSnap = r;
                }
                else if (r.Kind == K.Cmd)
                {
                    current = new Cmd();
                    current.Name = r.Text;
                    current.Rec = i;
                    current.Pre = lastSnap;
                    ParseCmd(current, settings);
                    current.Ordered = get("preserve-playback-queue-on-pause-seek", "off") == "on";
                    current.Chase = get("chase-midi-state-on-play-seek", "on") != "off";
                    current.Policy = get("overflow-policy", "drop-newest");
                    current.AgeLimit = get("queue-age-limit-enabled", "off") == "on";
                    current.ForwardOnly = get("simulate-slowdown", "on") == "off" &&
                        get("apply-queue-limit-without-slowdown", "off") == "on";
                    current.Limit = Int64.Parse(get("queue-length-limit", "2000"), CultureInfo.InvariantCulture);
                    log.Cmds.Add(current);
                    if (current.Verb == "Override" || current.Verb == "Channel") log.HasOverrides = true;
                }
                else if (r.Kind == K.CmdDone && current != null && current.Done < 0)
                {
                    current.Done = i;
                }
            }
            // The setting changes applied by Set/SetService commands, in order, are folded into later commands' view.
            Dictionary<string, string> live = new Dictionary<string, string>(settings);
            foreach (Cmd c in log.Cmds)
            {
                if (c.Verb == "Set")
                {
                    // Set(Member=value)
                    string body = c.Name.Substring(4, c.Name.Length - 5);
                    int eq = body.IndexOf('=');
                    string key = Kebab(body.Substring(0, eq));
                    string value = body.Substring(eq + 1);
                    live[key] = BoolText(value);
                    if (key == "overflow-policy") live[key] = Kebab(value);
                    continue;
                }
                c.Ordered = (live.ContainsKey("preserve-playback-queue-on-pause-seek") ? live["preserve-playback-queue-on-pause-seek"] : "off") == "on";
                c.Chase = (live.ContainsKey("chase-midi-state-on-play-seek") ? live["chase-midi-state-on-play-seek"] : "on") != "off";
                c.Policy = live.ContainsKey("overflow-policy") ? live["overflow-policy"] : "drop-newest";
                c.AgeLimit = (live.ContainsKey("queue-age-limit-enabled") ? live["queue-age-limit-enabled"] : "off") == "on";
                c.ForwardOnly = (live.ContainsKey("simulate-slowdown") ? live["simulate-slowdown"] : "on") == "off" &&
                    (live.ContainsKey("apply-queue-limit-without-slowdown") ? live["apply-queue-limit-without-slowdown"] : "off") == "on";
                c.Limit = Int64.Parse(live.ContainsKey("queue-length-limit") ? live["queue-length-limit"] : "2000", CultureInfo.InvariantCulture);
            }

            // segments
            bool outputSeen = false;
            Seg open = null;
            long pausedPosition = -1;
            bool classicPaused = false;
            Action<int> closeAt = delegate(int rec)
            {
                if (open != null) { open.EndRec = rec; log.Segs.Add(open); open = null; }
            };
            foreach (Cmd c in log.Cmds)
            {
                PlaybackStateSnapshot(c);
                int panic = PanicInSpan(log, c);
                bool stopLike = c.Verb == "Stop" || c.Verb == "Start";
                string pre = c.Pre == null ? "Stopped" : c.Pre.State;
                if (c.Verb == "Start")
                {
                    closeAt(panic >= 0 ? panic + 1 : c.Rec);
                    Seg seg = new Seg();
                    seg.Kind = "Start";
                    seg.Command = c;
                    seg.StartRec = panic >= 0 ? panic + 1 : c.Rec;
                    seg.StartPos = Math.Max(0, Math.Min(c.Pos, song.Duration));
                    seg.StartIdx = song.ClampedLowerBound(c.Pos);
                    seg.StatsReset = true;
                    seg.Ordered = c.Ordered;
                    seg.Mode = c.Mode;
                    seg.Chase = c.Chase;
                    seg.Policy = c.Policy;
                    seg.AgeLimit = c.AgeLimit;
                    seg.ForwardOnly = c.ForwardOnly;
                    seg.Limit = c.Limit;
                    open = seg;
                    outputSeen = true;
                    classicPaused = false;
                    pausedPosition = -1;
                }
                else if (c.Verb == "Stop")
                {
                    closeAt(panic >= 0 ? panic : c.Rec);
                    classicPaused = false;
                }
                else if (c.Verb == "Pause")
                {
                    if (pre == "Playing")
                    {
                        c.Acting = true;
                        if (c.Ordered) c.Preserved = true;
                        else
                        {
                            closeAt(panic >= 0 ? panic : c.Rec);
                            classicPaused = true;
                            pausedPosition = c.Post != null ? c.Post.Get("position") : 0;
                        }
                    }
                }
                else if (c.Verb == "Resume")
                {
                    if (pre == "Paused")
                    {
                        c.Acting = true;
                        if (classicPaused)
                        {
                            Seg prev = log.Segs.Count > 0 ? log.Segs[log.Segs.Count - 1] : null;
                            Seg seg = new Seg();
                            seg.Kind = "Resume";
                            seg.Command = c;
                            seg.StartRec = c.Rec;
                            seg.StartPos = Math.Max(0, Math.Min(pausedPosition, song.Duration));
                            seg.StartIdx = song.ClampedLowerBound(pausedPosition);
                            seg.StatsReset = false;
                            seg.Ordered = prev != null && prev.Ordered;
                            seg.Mode = prev != null ? prev.Mode : "Queue";
                            seg.Chase = prev != null ? prev.Chase : true;
                            seg.Policy = prev != null ? prev.Policy : "drop-newest";
                            seg.AgeLimit = prev != null && prev.AgeLimit;
                            seg.ForwardOnly = prev != null && prev.ForwardOnly;
                            seg.Limit = prev != null ? prev.Limit : 2000;
                            open = seg;
                            classicPaused = false;
                        }
                    }
                }
                else if (c.Verb == "Seek")
                {
                    bool active = pre == "Playing" || pre == "Paused";
                    c.Acting = true;
                    Seg previous = open ?? (log.Segs.Count > 0 ? log.Segs[log.Segs.Count - 1] : null);
                    if (c.Ordered && active)
                    {
                        c.Preserved = true;
                        // the segment of the new generation begins after the 16-message marker
                        int markerLast;
                        int marker = FindMarker(log, c.Rec, out markerLast);
                        Seg seg = new Seg();
                        seg.Kind = "SeekOrdered";
                        seg.Command = c;
                        seg.StartPos = Math.Max(0, Math.Min(c.Pos, song.Duration));
                        seg.StartIdx = song.ClampedLowerBound(c.Pos);
                        seg.StatsReset = false;
                        seg.Ordered = true;
                        seg.Mode = previous != null ? previous.Mode : "Queue";
                        seg.Chase = c.Chase;
                        seg.Policy = c.Policy;
                        seg.AgeLimit = c.AgeLimit;
                        seg.ForwardOnly = c.ForwardOnly;
                        seg.Limit = c.Limit;
                        if (marker >= 0)
                        {
                            closeAt(marker);
                            seg.StartRec = markerLast + 1;
                            open = seg;
                        }
                        else
                        {
                            // The marker never appeared (the run ended first): no new generation to check.
                            seg.StartRec = log.R.Count;
                            c.Preserved = true;
                            seg.Kind = "SeekOrdered-nomarker";
                            open = seg;
                        }
                    }
                    else
                    {
                        closeAt(panic >= 0 ? panic + 1 : c.Rec);
                        Seg seg = new Seg();
                        seg.Kind = "Seek";
                        seg.Command = c;
                        seg.StartRec = panic >= 0 ? panic + 1 : c.Rec;
                        seg.StartPos = Math.Max(0, Math.Min(c.Pos, song.Duration));
                        seg.StartIdx = song.ClampedLowerBound(c.Pos);
                        seg.StatsReset = true;
                        seg.Ordered = c.Ordered;
                        seg.Mode = previous != null ? previous.Mode : "Queue";
                        seg.Chase = c.Chase;
                        seg.Policy = c.Policy;
                        seg.AgeLimit = c.AgeLimit;
                        seg.ForwardOnly = c.ForwardOnly;
                        seg.Limit = c.Limit;
                        open = seg;
                        classicPaused = false;
                    }
                }
                if (stopLike) { }
            }
            closeAt(log.R.Count);
            if (!outputSeen) { }

            // classify sends and attach them to segments
            Rec event0 = null;
            for (int i = 0; i < log.R.Count; i++)
            {
                Rec r = log.R[i];
                if (r.Kind != K.Send) continue;
                r.Source = r.Src >= 1 || (r.Src == 0 && song.Count > 0 && r.SendKind == song.Kind[0] && r.Hex == song.Hex[0]);
                if (event0 == null) event0 = r;
            }
            foreach (Seg seg in log.Segs)
            {
                for (int i = seg.StartRec; i < seg.EndRec && i < log.R.Count; i++)
                {
                    Rec r = log.R[i];
                    if (r.Kind == K.Event && r.Text == "PlaybackEnded") seg.Ended = true;
                    if (r.Kind != K.Send) continue;
                    if (r.Source) seg.Src.Add(r); else seg.Gen.Add(r);
                }
                // the chase block: generated messages before the first source send
                int first = seg.Src.Count > 0 ? seg.Src[0].Index : seg.EndRec;
                foreach (Rec g in seg.Gen) if (g.Index < first) seg.Chased.Add(g);
            }
        }

        static void PlaybackStateSnapshot(Cmd c) { }

        // The kind of the previous / next record that reached the output (events and snapshots logged from other threads may fall between).
        static K PrevOutput(Log log, int i)
        {
            for (int j = i - 1; j >= 0; j--) { K k = log.R[j].Kind; if (k == K.Send || k == K.Reset || k == K.Panic) return k; }
            return K.Other;
        }

        static K NextOutput(Log log, int i)
        {
            for (int j = i + 1; j < log.R.Count; j++) { K k = log.R[j].Kind; if (k == K.Send || k == K.Reset || k == K.Panic) return k; }
            return K.Other;
        }

        static int PanicInSpan(Log log, Cmd c)
        {
            int end = c.Done >= 0 ? c.Done : log.R.Count - 1;
            for (int i = c.Rec + 1; i < end; i++)
                if (log.R[i].Kind == K.Panic && PrevOutput(log, i) == K.Reset) return i;
            return -1;
        }

        // The first run of 16 consecutive generated CC123 sends B0..BF 7B 00 after the command.
        static int FindMarker(Log log, int from, Cmd c)
        {
            int last;
            return FindMarker(log, from, out last);
        }

        // Sends only count: snapshot lines of the control thread may fall between them.
        static int FindMarker(Log log, int from, out int lastIndex)
        {
            lastIndex = -1;
            for (int i = from; i < log.R.Count; i++)
            {
                if (log.R[i].Kind != K.Send || log.R[i].Src != -1 || log.R[i].Hex != "B07B00") continue;
                int n = 0, j = i;
                while (j < log.R.Count && n < 16)
                {
                    Rec r = log.R[j];
                    if (r.Kind == K.Send)
                    {
                        if (r.Src != -1 || r.Hex != String.Format("B{0:X}7B00", n)) break;
                        n++;
                    }
                    else if (r.Kind == K.Reset || r.Kind == K.Panic || r.Kind == K.Cmd) break;
                    j++;
                }
                if (n == 16) { lastIndex = j - 1; return i; }
            }
            return -1;
        }

        static void ParseCmd(Cmd c, Dictionary<string, string> settings)
        {
            string name = c.Name;
            if (name.StartsWith("Start(", StringComparison.Ordinal))
            {
                c.Verb = "Start";
                string body = name.Substring(6, name.Length - 7);
                string[] f = body.Split(',');
                c.Mode = f[0];
                c.Pos = Int64.Parse(f[1].Substring(4), CultureInfo.InvariantCulture);
                c.Paused = f.Length > 2 && f[2] == "paused";
            }
            else if (name == "Stop" || name.StartsWith("Stop(", StringComparison.Ordinal)) c.Verb = "Stop";
            else if (name == "Pause") c.Verb = "Pause";
            else if (name == "Resume") c.Verb = "Resume";
            else if (name.StartsWith("Seek(", StringComparison.Ordinal))
            {
                c.Verb = "Seek";
                c.Pos = Int64.Parse(name.Substring(5, name.Length - 6), CultureInfo.InvariantCulture);
            }
            else if (name.StartsWith("SetService(", StringComparison.Ordinal)) c.Verb = "Service";
            else if (name.StartsWith("Set(", StringComparison.Ordinal)) c.Verb = "Set";
            else if (name.StartsWith("Override(", StringComparison.Ordinal))
            {
                c.Verb = "Override";
                // Override(ch=7,Program=42)
                string body = name.Substring(9, name.Length - 10);
                string[] f = body.Split(',');
                c.Channel = Int32.Parse(f[0].Substring(3), CultureInfo.InvariantCulture);
                int eq = f[1].IndexOf('=');
                c.Attribute = f[1].Substring(0, eq);
                c.Value = f[1].Substring(eq + 1);
            }
            else if (name.StartsWith("Channel(", StringComparison.Ordinal))
            {
                c.Verb = "Channel";
                string body = name.Substring(8, name.Length - 9);
                string[] f = body.Split(',');
                c.Channel = Int32.Parse(f[0].Substring(3), CultureInfo.InvariantCulture);
                c.Value = f[1].Substring(8);
            }
            else if (name == "Snapshot") c.Verb = "Snapshot";
            else c.Verb = "Other";
        }

        // ---------------------------------------------------------------- checks

        static string Fmt(Rec r) { return r == null ? "-" : r.T + "us line#" + r.Index; }

        static void RunChecks(Log log)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            Rec final = null, end = null;
            foreach (Rec r in log.R)
            {
                if (r.Kind == K.Final) final = r;
                if (r.Kind == K.End) end = r;
            }

            // ---- T01 T02 T03
            bool bad = false;
            string why = "";
            if (end == null) { bad = true; why = "no END"; }
            if (final == null) { bad = true; why += " no FINAL"; }
            foreach (Rec r in log.R)
            {
                if (r.Kind == K.CmdEx) { bad = true; why += " " + r.Text; }
                if (r.Kind == K.Other && (r.Text.StartsWith("TIMEOUT") || r.Text.StartsWith("WATCHDOG") || r.Text.StartsWith("DISPOSE-ERROR")))
                { bad = true; why += " " + r.Text; }
            }
            foreach (Cmd c in log.Cmds) if (c.Done < 0) { bad = true; why += " no CMD-DONE for " + c.Name; }
            Check("T01", g, k, !bad, why);
            bool failed = false;
            string failText = "";
            int ended = 0;
            foreach (Rec r in log.R)
            {
                if (r.Kind != K.Event) continue;
                if (r.Text.StartsWith("PlaybackFailed") || r.Text.StartsWith("ChannelControlFailed")) { failed = true; failText = r.Text; }
                if (r.Text == "PlaybackEnded") ended++;
            }
            Check("T02", g, k, !failed, failText);
            string t03 = "";
            bool t03ok = ended == log.Ends;
            if (!t03ok) t03 = "PlaybackEnded x" + ended + ", expected " + log.Ends;
            if (final != null && final.State != "Stopped") { t03ok = false; t03 += " final state " + final.State; }
            Cmd finalStop = null;
            for (int i = log.Cmds.Count - 1; i >= 0; i--) if (log.Cmds[i].Name.StartsWith("Stop(", StringComparison.Ordinal)) { finalStop = log.Cmds[i]; break; }
            if (log.Ends >= 1 && finalStop != null && finalStop.Pre != null && finalStop.Pre.State != "Completed")
            { t03ok = false; t03 += " state before the final Stop was " + finalStop.Pre.State; }
            Check("T03", g, k, t03ok, t03);

            // ---- S01 stop/start silence
            bool firstStart = true;
            foreach (Cmd c in log.Cmds)
            {
                if (c.Done < 0) continue;
                bool expectPair;
                string pre = c.Pre == null ? "Stopped" : c.Pre.State;
                if (c.Verb == "Stop") expectPair = true;
                else if (c.Verb == "Start") { expectPair = !firstStart; firstStart = false; }
                else if (c.Verb == "Pause") { if (!c.Acting) continue; expectPair = !c.Preserved; }
                else if (c.Verb == "Seek") expectPair = !c.Preserved;
                else continue;
                if (c.Verb == "Stop" && firstStart) { expectPair = false; }
                int resets = 0, panics = 0;
                bool adjacent = true;
                for (int i = c.Rec + 1; i < c.Done; i++)
                {
                    Rec r = log.R[i];
                    if (r.Kind == K.Reset) { resets++; if (NextOutput(log, i) != K.Panic) adjacent = false; }
                    if (r.Kind == K.Panic && PrevOutput(log, i) != K.Reset) { /* a Clear buffer PANIC during play is counted by P03 */ }
                    if (r.Kind == K.Panic && PrevOutput(log, i) == K.Reset) panics++;
                }
                bool ok = expectPair ? (resets == 1 && panics == 1 && adjacent) : (resets == 0 && panics == 0);
                Check("S01", g, k, ok, c.Name + ": RESET x" + resets + " PANIC x" + panics + (adjacent ? "" : " not adjacent") + ", expected " + (expectPair ? "one pair" : "none"));
            }
            if (final != null && end != null)
            {
                int resets = 0, panics = 0;
                bool order = true;
                for (int i = final.Index + 1; i < end.Index; i++)
                {
                    if (log.R[i].Kind == K.Reset) { resets++; if (NextOutput(log, i) != K.Panic) order = false; }
                    else if (log.R[i].Kind == K.Panic) panics++;
                    else if (log.Program == "v1" && log.R[i].Kind == K.Event && log.R[i].Text == "PlaybackEnded")
                        Note("v1 raises PlaybackEnded on its playback thread, which can log it just after the harness's final Stop and before Dispose's RESET/PANIC (a race in v1's own event delivery; not a send). The v1 Dispose-silence check ignores that one event record; v2 is still held to the strict order (any record between its FINAL and END other than RESET/PANIC fails).");
                    else order = false;
                }
                Check("S01", g, k, resets == 1 && panics == 1 && order, "Dispose:RESET x" + resets + " PANIC x" + panics);
            }

            // ---- S02 quiet after Stop
            for (int ci = 0; ci < log.Cmds.Count; ci++)
            {
                Cmd c = log.Cmds[ci];
                if (c.Verb != "Stop" || c.Done < 0) continue;
                int stop = final != null ? final.Index : log.R.Count;
                for (int cj = ci + 1; cj < log.Cmds.Count; cj++)
                    if (log.Cmds[cj].Verb == "Start") { stop = log.Cmds[cj].Rec; break; }
                int out_ = 0;
                string first = "";
                for (int i = c.Done + 1; i < stop; i++)
                {
                    Rec r = log.R[i];
                    if (r.Kind == K.Send || r.Kind == K.Reset || r.Kind == K.Panic) { out_++; if (first.Length == 0) first = r.T + "us " + r.Kind; }
                }
                Check("S02", g, k, out_ == 0, c.Name + ": " + out_ + " output record(s) after it returned, first " + first);
            }

            // ---- S03 paused: frozen and silent
            for (int ci = 0; ci < log.Cmds.Count; ci++)
            {
                Cmd c = log.Cmds[ci];
                bool startPaused = c.Verb == "Start" && c.Paused;
                if (!((c.Verb == "Pause" && c.Acting) || startPaused) || c.Done < 0) continue;
                int stop = log.R.Count;
                for (int cj = ci + 1; cj < log.Cmds.Count; cj++)
                {
                    string v = log.Cmds[cj].Verb;
                    if (v == "Resume" || v == "Stop" || v == "Start" || v == "Seek") { stop = log.Cmds[cj].Rec; break; }
                }
                if (c.Post == null) continue;
                Rec first = c.Post;
                int outputs = 0;
                bool frozen = true;
                string detail = "";
                for (int i = c.Done + 1; i < stop; i++)
                {
                    Rec r = log.R[i];
                    if (r.Kind == K.Send || r.Kind == K.Reset || r.Kind == K.Panic) outputs++;
                    if (r.Kind == K.Snap && r.State == "Paused")
                    {
                        if (r.Get("processed") != first.Get("processed") || r.Get("position") != first.Get("position"))
                        { frozen = false; detail = "snapshot at " + r.T + "us changed processed/position"; }
                    }
                    if (r.Kind == K.Snap && r.State != "Paused" && i < stop) { frozen = false; detail = "state " + r.State + " at " + r.T + "us while paused"; }
                }
                // sends inside the pause span itself (the classic worker stops there) are covered by S01
                Check("S03", g, k, outputs == 0 && frozen && first.State == "Paused",
                    c.Name + ": " + outputs + " output record(s) while paused. " + detail + (first.State == "Paused" ? "" : " first snapshot state " + first.State));
            }

            // ---- segments: S04 S05 S06 S07 S08 and the scenario-level C01 C02 C03 P* Q* M01
            CheckSegments(log, final);
            CheckSource(log);
            CheckSnapshots(log, final);
            CheckDrops(log, final);
            CheckControls(log);
            CheckTiming(log);
        }

        static bool IsQueueMode(Seg s) { return s.Mode == "Queue"; }

        static void CheckSegments(Log log, Rec final)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            Seg lastStatsSeg = null;
            foreach (Seg seg in log.Segs)
            {
                List<Rec> src = seg.Src;
                bool filtered = log.HasOverrides;
                // S07: order (per mode) and, for Queue, coverage
                bool strict = seg.Mode == "Queue" || seg.Mode == "Drop" || seg.Mode == "NoteAttackLimit" || seg.Mode == "OutputRateLimit";
                string detail = "";
                bool ok = true;
                int previous = -1;
                for (int i = 0; i < src.Count && ok; i++)
                {
                    int idx = src[i].Src;
                    bool note = idx < song.Count && song.NoteKind(idx) != 0;
                    if (idx < seg.StartIdx || idx >= song.Count) { ok = false; detail = "source index " + idx + " outside [" + seg.StartIdx + "," + song.Count + ")"; break; }
                    if (strict || !note)
                    {
                        if (idx <= previous) { ok = false; detail = "index " + idx + " after " + previous; break; }
                        previous = idx;
                    }
                }
                if (ok && IsQueueMode(seg) && !filtered && seg.Kind != "SeekOrdered-nomarker")
                {
                    // contiguous from the start index; to the end when the segment played out
                    int expect = seg.StartIdx;
                    for (int i = 0; i < src.Count; i++)
                    {
                        if (src[i].Src != expect) { ok = false; detail = "expected index " + expect + " got " + src[i].Src + " at " + src[i].T + "us"; break; }
                        expect++;
                    }
                    if (ok && seg.Ended && expect != song.Count) { ok = false; detail = "played out but stopped at index " + expect + " of " + song.Count; }
                }
                if (seg.Kind != "SeekOrdered-nomarker")
                    Check("S07", g, k, ok, "segment " + seg.Kind + " " + seg.Command.Name + ": " + detail);

                // S04/S05: first source index
                if (!filtered && IsQueueMode(seg) && src.Count > 0 && seg.Kind != "Start" && seg.Kind != "SeekOrdered-nomarker")
                {
                    string id = seg.Kind == "Resume" ? "S04" : "S05";
                    Check(id, g, k, src[0].Src == seg.StartIdx,
                        seg.Kind + ": first source index " + src[0].Src + ", expected " + seg.StartIdx + " (position " + seg.StartPos + ")");
                }
                if (seg.StatsReset) lastStatsSeg = seg;
            }

            // ordered Seek: the marker block exists and is followed by the new generation
            foreach (Cmd c in log.Cmds)
            {
                if (c.Verb != "Seek" || !c.Preserved) continue;
                int marker = FindMarker(log, c.Rec, c);
                Check("S05", g, k, marker >= 0, c.Name + ": no 16-message CC123 marker block after the ordered Seek");
            }

            // S04 for an ordered Pause: nothing lost or repeated overall (Queue, no seek, no filter)
            bool orderedPause = false, anySeek = false;
            foreach (Cmd c in log.Cmds) { if (c.Verb == "Pause" && c.Preserved) orderedPause = true; if (c.Verb == "Seek") anySeek = true; }
            if (orderedPause && !anySeek && !log.HasOverrides && log.Segs.Count == 1 && IsQueueMode(log.Segs[0]))
            {
                Seg seg = log.Segs[0];
                bool ok = true;
                string detail = "";
                int expect = seg.StartIdx;
                foreach (Rec r in seg.Src)
                    if (r.Src != expect++) { ok = false; detail = "index " + r.Src + " where " + (expect - 1) + " was expected (lost or repeated around the Pause)"; break; }
                if (ok && seg.Ended && expect != song.Count) { ok = false; detail = "stopped at " + expect + " of " + song.Count; }
                Check("S04", g, k, ok, "ordered Pause/Resume: " + detail);
            }

            // S06: chase blocks
            foreach (Seg seg in log.Segs)
            {
                if (seg.Kind == "SeekOrdered-nomarker") continue;
                bool expectChase = seg.Chase && seg.StartIdx > 0 && !(seg.Kind == "Resume" && false);
                List<Rec> block = ChaseBlock(log, seg);
                bool ok = true;
                string detail = "";
                if (!expectChase)
                {
                    // overrides pending at the start would also be sent here; none of the scenarios set any at the start
                    if (block.Count != 0 && !log.HasOverrides) { ok = false; detail = block.Count + " generated message(s) before the first source event where none are expected"; }
                }
                else
                {
                    string oracle = ChaseOracle(song, seg.StartIdx, block);
                    if (oracle != null) { ok = false; detail = oracle; }
                    if (ok && block.Count == 0 && HasChaseableState(song, seg.StartIdx) && seg.Src.Count + seg.Gen.Count > 0)
                    { ok = false; detail = "no chase message although state exists before index " + seg.StartIdx; }
                }
                // a segment that never reached its first source send (stopped, paused at the end) proves nothing
                Check("S06", g, k, ok, seg.Kind + " " + seg.Command.Name + " at index " + seg.StartIdx + ": " + detail);
            }
        }

        // the generated messages sent at the start of a segment, before its first source send (the ordered-Seek marker excluded)
        static List<Rec> ChaseBlock(Log log, Seg seg)
        {
            List<Rec> block = new List<Rec>();
            foreach (Rec r in seg.Chased) block.Add(r);
            return block;
        }

        static bool HasChaseableState(SongInfo song, int index)
        {
            for (int i = 0; i < index && i < song.Count; i++)
            {
                int command = song.Status(i) & 0xF0;
                if (song.Kind[i] == "ControlChange" || song.Kind[i] == "ProgramChange" || song.Kind[i] == "PitchBend" ||
                    song.Kind[i] == "ChannelAftertouch") return true;
                if (command == 0) { }
            }
            return false;
        }

        // Soundness of the chase messages: each plain restore equals the latest source value before the index.
        static string ChaseOracle(SongInfo song, int index, List<Rec> block)
        {
            Dictionary<string, string> latest = new Dictionary<string, string>();
            for (int i = 0; i < index && i < song.Count; i++)
            {
                string kind = song.Kind[i];
                int status = song.Status(i);
                int ch = status & 0x0F;
                if (kind == "ControlChange") latest["cc" + ch + ":" + song.Byte(i, 1)] = song.Hex[i].Substring(4, 2);
                else if (kind == "ProgramChange") latest["pc" + ch] = song.Hex[i].Substring(2, 2);
                else if (kind == "PitchBend") latest["pb" + ch] = song.Hex[i].Substring(2, 4);
                else if (kind == "ChannelAftertouch") latest["ca" + ch] = song.Hex[i].Substring(2, 2);
            }
            foreach (Rec r in block)
            {
                string hex = r.Hex;
                if (hex.Length < 4) return "malformed chase message " + hex;
                int status = Convert.ToInt32(hex.Substring(0, 2), 16);
                int ch = status & 0x0F;
                string key = null, value = null;
                switch (status & 0xF0)
                {
                    case 0xB0:
                        {
                            int controller = Convert.ToInt32(hex.Substring(2, 2), 16);
                            if (controller == 6 || controller == 38 || (controller >= 96 && controller <= 101)) continue;   // parameter / selector sequences
                            key = "cc" + ch + ":" + controller; value = hex.Substring(4, 2);
                            break;
                        }
                    case 0xC0: key = "pc" + ch; value = hex.Substring(2, 2); break;
                    case 0xE0: key = "pb" + ch; value = hex.Substring(2, 4); break;
                    case 0xD0: key = "ca" + ch; value = hex.Substring(2, 2); break;
                    default: return "chase message " + hex + " is not a controller/program/bend/pressure restore";
                }
                string expected;
                if (!latest.TryGetValue(key, out expected)) return "chase message " + hex + " restores state the song never set before index " + index;
                if (expected != value) return "chase message " + hex + " restores " + value + " but the latest source value is " + expected;
                if (r.Src != -1) return "chase message " + hex + " has source index " + r.Src;
            }
            return null;
        }

        static void CheckSource(Log log)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            // S08: bytes intact
            // The Per-note interval gate and Delayed NoteOff build their note messages themselves (they re-time and re-pair
            // notes, so the index of an emitted note message need not name its bytes); every other message is checked.
            bool noteRewriting = false;
            foreach (Seg s in log.Segs) if (s.Mode == "PerNoteIntervalGate" || s.Mode == "DelayedNoteOff") noteRewriting = true;
            foreach (Rec r in log.R)
            {
                if (r.Kind != K.Send || !r.Source) continue;
                if (noteRewriting && (r.SendKind == "NoteOn" || r.SendKind == "NoteOff")) continue;
                // A gate re-emits a velocity-0 NoteOn as the NoteOff it means (90 kk 00 -> 80 kk 00); everything else is untouched.
                bool kindOk = r.Src < song.Count && (r.SendKind == song.Kind[r.Src] ||
                    ((r.SendKind == "NoteOn" || r.SendKind == "NoteOff") && (song.Kind[r.Src] == "NoteOn" || song.Kind[r.Src] == "NoteOff") && r.Hex.EndsWith("00")));
                bool ok = r.Src < song.Count && Canon(r.Hex) == Canon(song.Hex[r.Src]) && kindOk;
                Check("S08", g, k, ok, "source " + r.Src + " sent as " + r.SendKind + " " + r.Hex + (r.Src < song.Count ? ", song has " + song.Kind[r.Src] + " " + song.Hex[r.Src] : ""));
            }
            // generated messages must be recognisable (index 0 or -1)
            foreach (Rec r in log.R)
            {
                if (r.Kind != K.Send || r.Source) continue;
                Check("S08", g, k, r.Src <= 0, "generated message " + r.Hex + " has source index " + r.Src);
            }
        }

        static string Canon(string hex)
        {
            return hex.Length == 6 && hex[0] == '9' && hex.EndsWith("00") ? "8" + hex.Substring(1) : hex;
        }

        static bool IsStatsResetSeg(Seg s) { return s.StatsReset; }

        static void CheckSnapshots(Log log, Rec final)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            // statistics segments: Start and classic Seek restart the counters
            List<Seg> statSegs = new List<Seg>();
            foreach (Seg s in log.Segs) if (s.StatsReset) statSegs.Add(s);
            // C01 + C03
            string[] monotone = { "processed", "dropped", "gateFiltered", "musicalRejectedAttacks", "musicalSuppressedReleases",
                "rateRejectedAttacks", "rateSuppressedReleases", "rateRejectedOther", "rateSafetySoftExcess", "rateSafetyBypass" };
            for (int si = 0; si < statSegs.Count; si++)
            {
                Seg seg = statSegs[si];
                int from = seg.StartRec;
                int to = si + 1 < statSegs.Count ? statSegs[si + 1].StartRec : log.R.Count;
                // sends counted from the start of this statistics segment (across Resume sub-segments)
                int sources = 0, sourceIdx = from;
                Rec prev = null;
                for (int i = from; i < to; i++)
                {
                    Rec r = log.R[i];
                    if (r.Kind == K.Send && r.Source) sources++;
                    if (r.Kind != K.Snap && r.Kind != K.Final) continue;
                    // a snapshot logged between the old worker's exit and the reset of the next segment is not ours
                    long processed = r.Get("processed");
                    bool quiet = r.State != "Playing";
                    if (quiet) Check("C01", g, k, processed == sources, "processed=" + processed + " but " + sources + " source events sent (" + r.State + " at " + r.T + "us)");
                    else Check("C01", g, k, processed <= sources, "processed=" + processed + " exceeds " + sources + " source events sent (at " + r.T + "us)");
                    if (prev != null)
                        foreach (string key in monotone)
                            if (r.Get(key) < prev.Get(key))
                                Check("C03", g, k, false, key + " fell from " + prev.Get(key) + " to " + r.Get(key) + " at " + r.T + "us");
                    Check("C03", g, k, true, "");
                    prev = r;
                    sourceIdx = i;
                }
            }
            // Q02: sanity
            foreach (Rec r in log.R)
            {
                if (r.Kind != K.Snap && r.Kind != K.Final) continue;
                bool ok = r.Get("queue") >= 0 && r.Get("outstanding") >= 0 && r.Get("processed") >= 0 && r.Get("dropped") >= 0 &&
                    r.Get("position") >= 0 && r.Get("intended") >= 0 && r.Get("intended") <= song.Duration &&
                    r.Get("lag") >= 0 && r.Get("maxLag") >= 0 && r.Get("processed") <= song.Count &&
                    (r.State == "Stopped" || r.State == "Playing" || r.State == "Paused" || r.State == "Completed");
                Check("Q02", g, k, ok, "snapshot at " + r.T + "us out of range: " + Describe(r));
            }
        }

        static string Describe(Rec r)
        {
            StringBuilder text = new StringBuilder(r.State);
            foreach (KeyValuePair<string, long> kv in r.V) text.Append(' ').Append(kv.Key).Append('=').Append(kv.Value);
            return text.ToString();
        }

        // Protected-traffic and count rules for the Drop modes and the gates.
        static void CheckDrops(Log log, Rec final)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            Seg only = null;
            int starts = 0;
            foreach (Seg s in log.Segs) if (s.Kind == "Start") { starts++; only = s; }
            bool simple = log.Segs.Count == 1 && starts == 1;   // one Start, no Pause/Seek/Resume restarts
            // the last statistics segment run to the end
            Seg last = null;
            foreach (Seg s in log.Segs) if (s.StatsReset) last = s;
            bool hasOrderedSeek = false;
            foreach (Cmd c in log.Cmds)
                if (c.Verb == "Seek" && c.Preserved) hasOrderedSeek = true;

            // C02: conservation, in the statistics segment that played out
            if (last != null && last.Ended && !hasOrderedSeek && !log.HasOverrides && final != null)
            {
                long expectedTotal = song.Count - last.StartIdx;
                // events a classic Pause discards (due but not yet sent) are neither processed nor dropped
                long discarded = 0;
                bool counting = false;
                int lastSent = last.StartIdx - 1;
                foreach (Seg s in log.Segs)
                {
                    if (s == last) counting = true;
                    if (!counting) continue;
                    if (s.Kind == "Resume") discarded += Math.Max(0, s.StartIdx - (lastSent + 1));
                    foreach (Rec r in s.Src) lastSent = r.Src;
                }
                long processed = final.Get("processed"), dropped = final.Get("dropped");
                long accounted;
                string mode = last.Mode;
                long extra = 0;
                string name = "";
                if (mode == "Queue") { accounted = processed + dropped; name = "processed+dropped"; }
                else if (mode == "Drop")
                {
                    accounted = processed + dropped;
                    name = "processed+dropped";
                    if (last.Ordered && IsPairPolicy(last.Policy) && !last.ForwardOnly)
                    {
                        extra = UncountedReleases(log, last);
                        name += "+uncounted suppressed releases(" + extra + ")";
                    }
                }
                else if (mode == "PerNoteIntervalGate" || mode == "DelayedNoteOff") { accounted = processed + final.Get("gateFiltered") + dropped; name = "processed+gateFiltered"; }
                else if (mode == "NoteAttackLimit") { accounted = processed + final.Get("musicalRejectedAttacks") + final.Get("musicalSuppressedReleases") + dropped; name = "processed+musicalRejected+musicalSuppressed"; }
                else { accounted = processed + final.Get("rateRejectedAttacks") + final.Get("rateSuppressedReleases") + final.Get("rateRejectedOther") + dropped; name = "processed+rateRejected+rateSuppressed+rateRejectedOther"; }
                long total = accounted + discarded;
                if (mode == "Drop" && last.Ordered && IsPairPolicy(last.Policy) && !last.ForwardOnly)
                {
                    // v1's ordered finite path does not count a NoteOff suppressed because its NoteOn was dropped
                    // (PlaybackEngine.Ordered.cs:898-903): the shortfall must be explained by such releases alone.
                    long missing = expectedTotal - total;
                    Check("C02", g, k, missing >= 0 && missing <= extra,
                        mode + ": " + name + " = " + total + " of " + expectedTotal + " (shortfall " + missing + ", at most " + extra + " suppressed releases)");
                    if (missing > 0)
                    {
                        string uk = log.Scenario + " (" + last.Policy + ")";
                        long[] range;
                        if (!Uncounted.TryGetValue(uk, out range)) Uncounted[uk] = range = new long[] { Int64.MaxValue, 0, 0 };
                        range[0] = Math.Min(range[0], missing); range[1] = Math.Max(range[1], missing); range[2]++;
                    }
                }
                else
                    Check("C02", g, k, total == expectedTotal,
                        mode + ": " + name + " = " + accounted + (discarded > 0 ? " + " + discarded + " discarded by Pause" : "") + ", song has " + expectedTotal + " events from index " + last.StartIdx);
            }

            if (!simple || only == null || only.Mode != "Drop") return;
            string policy = only.Policy;
            bool live = false;
            foreach (Cmd c in log.Cmds) if (c.Verb == "Set" && (c.Name.StartsWith("Set(OverflowPolicy") || c.Name.StartsWith("Set(QueueLengthLimit"))) live = true;
            List<int> sent = new List<int>();
            foreach (Rec r in only.Src) sent.Add(r.Src);
            HashSet<int> sentSet = new HashSet<int>(sent);
            int through = only.Ended ? song.Count - 1 : (sent.Count > 0 ? sent[sent.Count - 1] : -1);

            // P01
            if (!only.Ordered && !only.ForwardOnly && !only.AgeLimit && !live && !log.HasOverrides && (policy == "drop-newest" || policy == "drop-oldest") && only.Ended)
            {
                int missing = 0;
                string first = "";
                for (int i = only.StartIdx; i <= through; i++)
                {
                    bool protectedEvent = song.NoteKind(i) == 2 || song.IsSafety(i);
                    if (protectedEvent && !sentSet.Contains(i)) { missing++; if (first.Length == 0) first = "event " + i + " " + song.Kind[i] + " " + song.Hex[i]; }
                }
                Check("P01", g, k, missing == 0, policy + ": " + missing + " protected event(s) dropped, first " + first);
            }

            // P02
            if (IsPairPolicy(policy) && !only.AgeLimit && !live && only.Ended)
            {
                string detail = PairRule(song, only.StartIdx, through, sentSet, policy, only.ForwardOnly);
                Check("P02", g, k, detail == null, policy + ": " + detail);
            }

            // P03
            {
                int midPlayPanics = 0;
                for (int i = only.StartRec; i < log.R.Count; i++)
                    if (log.R[i].Kind == K.Panic && PrevOutput(log, i) != K.Reset) midPlayPanics++;
                bool clear = policy == "clear-buffer-and-catch-up" && !only.ForwardOnly;
                long dropped = final == null ? 0 : final.Get("dropped");
                bool ok = clear ? (dropped > 0 ? midPlayPanics >= 1 : midPlayPanics == 0) : midPlayPanics == 0;
                if (!live) Check("P03", g, k, ok, policy + ": " + midPlayPanics + " mid-play PANIC(s), dropped=" + dropped);
            }

            // Q01
            if (!only.ForwardOnly && !live && !only.Ordered && final != null)
            {
                long dropped = final.Get("dropped");
                long maxQueue = final.Get("maxQueue");
                bool ok = dropped == 0 || only.AgeLimit || maxQueue >= only.Limit;
                Check("Q01", g, k, ok, "dropped=" + dropped + " but the queue only reached " + maxQueue + " of limit " + only.Limit);
                if (only.Limit >= song.Count && !only.AgeLimit)
                    Check("Q01", g, k, dropped == 0, "an ample limit (" + only.Limit + ") still dropped " + dropped);
            }
            if (only.ForwardOnly && final != null && policy == "drop-newest")
                Check("Q02", g, k, final.Get("virtualMaxQueue") <= only.Limit, "forward-only virtual queue reached " + final.Get("virtualMaxQueue") + " of limit " + only.Limit);
        }

        static bool IsPairPolicy(string policy)
        {
            return policy == "drop-incoming-complete-notes" || policy == "drop-oldest-complete-note" || policy == "drop-background-notes";
        }

        // FIFO pairing per channel/key over the source stream, as CompleteNoteTracker pairs occurrences.
        static string PairRule(SongInfo song, int from, int through, HashSet<int> sent, string policy, bool forward)
        {
            Dictionary<int, Queue<int>> open = new Dictionary<int, Queue<int>>();
            int unsafeDrops = 0;
            for (int i = from; i <= through && i < song.Count; i++)
            {
                int kind = song.NoteKind(i);
                int slot = (song.Status(i) & 0x0F) * 128 + (song.Byte(i, 1) & 0x7F);
                if (kind == 1)
                {
                    Queue<int> q;
                    if (!open.TryGetValue(slot, out q)) open[slot] = q = new Queue<int>();
                    q.Enqueue(i);
                }
                else if (kind == 2)
                {
                    Queue<int> q;
                    int on = -1;
                    if (open.TryGetValue(slot, out q) && q.Count > 0) on = q.Dequeue();
                    bool offSent = sent.Contains(i);
                    if (on < 0)
                    {
                        if (!offSent) return "unmatched NoteOff " + i + " was dropped";
                    }
                    else
                    {
                        bool onSent = sent.Contains(on);
                        if (onSent && !offSent) return "NoteOff " + i + " dropped although its NoteOn " + on + " was sent";
                        if (!onSent && offSent) return "NoteOff " + i + " sent although its NoteOn " + on + " was dropped";
                    }
                }
                else if (song.IsSafety(i) && !sent.Contains(i)) unsafeDrops++;
            }
            if (unsafeDrops > 0) return unsafeDrops + " safety control(s) dropped";
            return null;
        }

        // NoteOffs left out because their NoteOn was dropped: the ordered finite path does not count them (Q: PlaybackEngine.Ordered.cs:898-903).
        static long UncountedReleases(Log log, Seg seg)
        {
            SongInfo song = log.Song;
            HashSet<int> sent = new HashSet<int>();
            foreach (Rec r in seg.Src) sent.Add(r.Src);
            Dictionary<int, Queue<int>> open = new Dictionary<int, Queue<int>>();
            long count = 0;
            for (int i = seg.StartIdx; i < song.Count; i++)
            {
                int kind = song.NoteKind(i);
                int slot = (song.Status(i) & 0x0F) * 128 + (song.Byte(i, 1) & 0x7F);
                if (kind == 1)
                {
                    Queue<int> q;
                    if (!open.TryGetValue(slot, out q)) open[slot] = q = new Queue<int>();
                    q.Enqueue(i);
                }
                else if (kind == 2)
                {
                    Queue<int> q;
                    if (open.TryGetValue(slot, out q) && q.Count > 0)
                    {
                        int on = q.Dequeue();
                        if (!sent.Contains(on) && !sent.Contains(i)) count++;
                    }
                }
            }
            return count;
        }

        // ---- channel controls
        static void CheckControls(Log log)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            foreach (Cmd c in log.Cmds)
            {
                if (c.Done < 0) continue;
                if (c.Verb == "Override" && c.Value != "Auto")
                {
                    byte[] expected = OverrideBytes(c.Channel, c.Attribute, Int32.Parse(c.Value, CultureInfo.InvariantCulture));
                    string expectedHex = HexOf(expected);
                    // the forced message: a generated send with exactly these bytes, after the command, once, before the next boundary
                    int boundary = log.R.Count;
                    foreach (Cmd d in log.Cmds)
                        if (d.Rec > c.Rec && (d.Verb == "Start" || d.Verb == "Stop" || d.Verb == "Seek" || (d.Verb == "Pause" && d.Acting && !d.Preserved)))
                        { boundary = d.Rec; break; }
                    int found = 0;
                    int foundAt = -1;
                    for (int i = c.Rec; i < boundary; i++)
                        if (log.R[i].Kind == K.Send && !log.R[i].Source && log.R[i].Hex == expectedHex) { found++; if (foundAt < 0) foundAt = i; }
                    string state = c.Pre == null ? "Stopped" : c.Pre.State;
                    if (state == "Playing")
                    {
                        Check("O01", g, k, found == 1, c.Name + ": forced message " + expectedHex + " sent " + found + " time(s), expected once");
                        if (foundAt >= 0) ControlLatency(log.Group, "override", log.R[foundAt].T - log.R[c.Rec].T);
                        // suppression: later source events of this attribute must carry the forced value
                        long position = c.Post != null ? c.Post.Get("position") : 0;
                        int opportunities = 0;
                        int violations = 0;
                        string first = "";
                        int clearedAt = ClearedAt(log, c);
                        foreach (Rec r in log.R)
                        {
                            if (r.Kind != K.Send || !r.Source || r.Index <= c.Done) continue;
                            if (clearedAt >= 0 && r.Index > clearedAt) break;
                            if (r.Index > boundary) break;
                            int ch, value;
                            if (!Classify(r.Hex, c.Attribute, out ch, out value) || ch != c.Channel) continue;
                            if (song.Time[r.Src] <= position + 1000) continue;
                            if (value != Int32.Parse(c.Value, CultureInfo.InvariantCulture)) { violations++; if (first.Length == 0) first = "event " + r.Src + " " + r.Hex + " at " + r.T + "us"; }
                        }
                        // opportunities: source events later than the override (and before it is changed) that would have been suppressed
                        int startIdx = song.LowerBound(position + 1000);
                        int stopIdx = song.Count;
                        foreach (Cmd d in log.Cmds)
                            if (d.Rec > c.Rec && d.Verb == "Override" && d.Channel == c.Channel && d.Attribute == c.Attribute && d.Pre != null)
                            { stopIdx = song.LowerBound(d.Pre.Get("position")); break; }
                        for (int i = startIdx; i < stopIdx; i++)
                        {
                            int ch, value;
                            if (Classify(song.Hex[i], c.Attribute, out ch, out value) && ch == c.Channel && value != Int32.Parse(c.Value, CultureInfo.InvariantCulture)) opportunities++;
                        }
                        Check("O01", g, k, violations == 0, c.Name + ": " + violations + " conflicting source event(s) still sent, first " + first);
                        Check("O01", g, k, opportunities > 0, c.Name + ": vacuous - no later source event of this attribute exists (" + song.Name + ")");
                    }
                }
                if (c.Verb == "Channel" && c.Value == "0")
                {
                    string state = c.Pre == null ? "Stopped" : c.Pre.State;
                    if (state != "Playing") continue;
                    string[] trio = { String.Format("B{0:X}4000", c.Channel), String.Format("B{0:X}7800", c.Channel), String.Format("B{0:X}7B00", c.Channel) };
                    // the three messages are consecutive sends; snapshot lines of the control thread may fall between them
                    int at = -1, trioEnd = -1;
                    for (int i = c.Rec; i < log.R.Count && at < 0; i++)
                    {
                        if (log.R[i].Kind != K.Send || log.R[i].Source || log.R[i].Hex != trio[0]) continue;
                        int n = 1, j = i + 1;
                        while (j < log.R.Count && n < 3)
                        {
                            if (log.R[j].Kind == K.Send) { if (log.R[j].Hex != trio[n]) break; n++; }
                            else if (log.R[j].Kind == K.Reset || log.R[j].Kind == K.Panic || log.R[j].Kind == K.Cmd) break;
                            j++;
                        }
                        if (n == 3) { at = i; trioEnd = j - 1; }
                    }
                    Check("O02", g, k, at >= 0, c.Name + ": silence messages " + String.Join(",", trio) + " not found after the command");
                    if (at >= 0) ControlLatency(log.Group, "channel disable", log.R[at].T - log.R[c.Rec].T);
                    if (at < 0) continue;
                    // until it is enabled again: no source event on that channel
                    int enabledAt = log.R.Count;
                    foreach (Cmd d in log.Cmds)
                        if (d.Rec > c.Rec && d.Verb == "Channel" && d.Channel == c.Channel && d.Value == "1") { enabledAt = d.Rec; break; }
                    int stragglers = 0;
                    string first = "";
                    for (int i = trioEnd + 1; i < enabledAt; i++)
                    {
                        Rec r = log.R[i];
                        if (r.Kind == K.Send && r.Source && (Convert.ToInt32(r.Hex.Substring(0, 2), 16) & 0xF0) < 0xF0 && (Convert.ToInt32(r.Hex.Substring(0, 2), 16) & 0x0F) == c.Channel)
                        { stragglers++; if (first.Length == 0) first = "event " + r.Src + " " + r.Hex; }
                    }
                    Check("O02", g, k, stragglers == 0, c.Name + ": " + stragglers + " source event(s) of the disabled channel sent after the silence, first " + first);
                    // opportunity: later events exist on that channel
                    int later = 0;
                    long position = c.Post != null ? c.Post.Get("position") : 0;
                    for (int i = song.LowerBound(position); i < song.Count; i++)
                        if (song.Kind[i] != "SystemExclusive" && song.Kind[i] != "SystemMessage" && (song.Status(i) & 0x0F) == c.Channel) later++;
                    Check("O02", g, k, later > 0, c.Name + ": vacuous - the channel has no later events");
                }
            }
        }

        static int ClearedAt(Log log, Cmd c)
        {
            foreach (Cmd d in log.Cmds)
                if (d.Rec > c.Rec && d.Verb == "Override" && d.Channel == c.Channel && d.Attribute == c.Attribute) return d.Rec;
            return -1;
        }

        static string HexOf(byte[] bytes)
        {
            StringBuilder text = new StringBuilder();
            foreach (byte b in bytes) text.Append(b.ToString("X2"));
            return text.ToString();
        }

        // ChannelOverrideState.CreateMessage
        static byte[] OverrideBytes(int channel, string attribute, int value)
        {
            switch (attribute)
            {
                case "BankMsb": return new byte[] { (byte)(0xB0 | channel), 0, (byte)value };
                case "BankLsb": return new byte[] { (byte)(0xB0 | channel), 32, (byte)value };
                case "Program": return new byte[] { (byte)(0xC0 | channel), (byte)value };
                case "Volume": return new byte[] { (byte)(0xB0 | channel), 7, (byte)value };
                case "Expression": return new byte[] { (byte)(0xB0 | channel), 11, (byte)value };
                case "Pan": return new byte[] { (byte)(0xB0 | channel), 10, (byte)value };
                case "Sustain": return new byte[] { (byte)(0xB0 | channel), 64, (byte)(value == 0 ? 0 : 127) };
                case "PitchBend": { int raw = value + 8192; return new byte[] { (byte)(0xE0 | channel), (byte)(raw & 0x7F), (byte)((raw >> 7) & 0x7F) }; }
                default: return new byte[] { (byte)(0xD0 | channel), (byte)value };
            }
        }

        // ChannelOverrideState.TryClassify for one attribute: does this message set it, and to what value?
        static bool Classify(string hex, string attribute, out int channel, out int value)
        {
            channel = 0; value = 0;
            if (hex.Length < 4) return false;
            int status = Convert.ToInt32(hex.Substring(0, 2), 16);
            int first = Convert.ToInt32(hex.Substring(2, 2), 16);
            int second = hex.Length >= 6 ? Convert.ToInt32(hex.Substring(4, 2), 16) : 0;
            channel = status & 0x0F;
            switch (attribute)
            {
                case "Program": if ((status & 0xF0) != 0xC0) return false; value = first; return true;
                case "Aftertouch": if ((status & 0xF0) != 0xD0) return false; value = first; return true;
                case "PitchBend": if ((status & 0xF0) != 0xE0) return false; value = ((second << 7) | first) - 8192; return true;
            }
            if ((status & 0xF0) != 0xB0) return false;
            int controller = attribute == "BankMsb" ? 0 : attribute == "BankLsb" ? 32 : attribute == "Volume" ? 7 : attribute == "Expression" ? 11 : attribute == "Pan" ? 10 : 64;
            if (first != controller) return false;
            value = attribute == "Sustain" ? (second >= 64 ? 1 : 0) : second;
            return true;
        }

        // ---- timing
        static void CheckTiming(Log log)
        {
            string g = log.Group, k = log.Key;
            SongInfo song = log.Song;
            foreach (Seg seg in log.Segs)
            {
                if (seg.Kind != "Start" || seg.Command.Paused) continue;
                // only a segment that no later command interrupts has a single time origin
                bool interrupted = false;
                foreach (Cmd c in log.Cmds)
                    if (c.Rec > seg.Command.Rec && (c.Verb == "Pause" || c.Verb == "Resume" || c.Verb == "Seek" || c.Verb == "Start")) interrupted = true;
                if (interrupted) continue;
                long origin = log.R[seg.Command.Rec].T;
                foreach (Rec r in seg.Src)
                {
                    long due = origin + (song.Time[r.Src] - seg.StartPos);
                    Check("M01", g, k, r.T >= due - 2, "event " + r.Src + " sent at " + r.T + "us, " + (due - r.T) + "us before its due time " + due + "us");
                }
            }
        }

        // ---------------------------------------------------------------- cross-run checks

        static string SequenceOf(Log log)
        {
            StringBuilder text = new StringBuilder();
            foreach (Rec r in log.R)
            {
                if (r.Kind == K.Send)
                {
                    // DD-001: v1 reads a SysEx event of 1-3 stored bytes back as zeros, v2 keeps the bytes (an improvement, DD-001 in v2/docs/DIFFERENCES_FROM_V1.md);
                    // the comparison keeps the length and the kind only.
                    string hex = r.SendKind == "SystemExclusive" && r.Hex.Length <= 6 ? "short-sysex:" + (r.Hex.Length / 2) : r.Hex;
                    text.Append("S ").Append(r.SendKind).Append(' ').Append(r.Src).Append(' ').Append(hex).Append('\n');
                }
                else if (r.Kind == K.Reset) text.Append("R\n");
                else if (r.Kind == K.Panic) text.Append("P\n");
            }
            return text.ToString();
        }

        static void CrossChecks(List<Log> logs)
        {
            Dictionary<string, List<Log>> byConfig = new Dictionary<string, List<Log>>();
            foreach (Log log in logs)
            {
                string key = log.Scenario + "/" + log.Fixture;
                List<Log> list;
                if (!byConfig.TryGetValue(key, out list)) byConfig[key] = list = new List<Log>();
                list.Add(log);
            }
            string[] counters = { "processed", "dropped", "gateFiltered", "musicalRejectedAttacks", "musicalSuppressedReleases",
                "rateRejectedAttacks", "rateSuppressedReleases", "rateRejectedOther" };
            foreach (KeyValuePair<string, List<Log>> kv in byConfig)
            {
                List<Log> list = kv.Value;
                if (list[0].Class != "exact") continue;
                Log reference = list[0];
                string refSeq = SequenceOf(reference);
                Rec refFinal = FinalOf(reference);
                foreach (Log other in list)
                {
                    if (other == reference) continue;
                    string seq = SequenceOf(other);
                    bool same = seq == refSeq;
                    string detail = "";
                    if (!same) detail = "differs from " + reference.Key + ": " + FirstDifference(refSeq, seq);
                    Check("X01", other.Group, other.Key, same, detail);
                    Rec fin = FinalOf(other);
                    bool eq = true;
                    string cd = "";
                    foreach (string c in counters)
                        if (refFinal != null && fin != null && refFinal.Get(c) != fin.Get(c)) { eq = false; cd += " " + c + " " + refFinal.Get(c) + " vs " + fin.Get(c); }
                    Check("X02", other.Group, other.Key, eq, "final counters differ from " + reference.Key + ":" + cd);
                }
                // the reference itself participates as "cross" so a single-log config is not vacuous
                Check("X01", reference.Group, reference.Key, true, "");
                Check("X02", reference.Group, reference.Key, true, "");
            }

            // X03: the chase block of a start position, wherever it occurs (same fixture, position, chase on)
            Dictionary<string, KeyValuePair<string, string>> blocks = new Dictionary<string, KeyValuePair<string, string>>();
            foreach (Log log in logs)
            {
                foreach (Seg seg in log.Segs)
                {
                    if (seg.Kind == "SeekOrdered-nomarker" || !seg.Chase || seg.StartIdx == 0) continue;
                    if (log.HasOverrides) continue;
                    List<Rec> block = ChaseBlock(log, seg);
                    if (block.Count == 0 && seg.Src.Count == 0) continue;   // never reached
                    StringBuilder text = new StringBuilder();
                    foreach (Rec r in block) text.Append(r.Hex).Append(';');
                    string key = log.Fixture + "@" + seg.StartIdx;
                    KeyValuePair<string, string> prev;
                    if (!blocks.TryGetValue(key, out prev)) blocks[key] = new KeyValuePair<string, string>(log.Key, text.ToString());
                    else Check("X03", log.Group, log.Key, prev.Value == text.ToString(),
                        "chase block for " + key + " differs from " + prev.Key + ": " + FirstDifference(prev.Value.Replace(';', '\n'), text.ToString().Replace(';', '\n')));
                }
            }
        }

        static Rec FinalOf(Log log)
        {
            foreach (Rec r in log.R) if (r.Kind == K.Final) return r;
            return null;
        }

        static string FirstDifference(string a, string b)
        {
            string[] x = a.Split('\n'), y = b.Split('\n');
            int n = Math.Min(x.Length, y.Length);
            for (int i = 0; i < n; i++)
                if (x[i] != y[i]) return "line " + i + " '" + x[i] + "' vs '" + y[i] + "' (lengths " + x.Length + "/" + y.Length + ")";
            return "lengths " + x.Length + " vs " + y.Length;
        }

        // ---------------------------------------------------------------- information (not pass/fail)

        static void Timing(List<Log> logs)
        {
            // lag of each source dispatch behind its due time, single uninterrupted Start segments, by program/arch and scenario class
            Dictionary<string, List<long>> lags = new Dictionary<string, List<long>>();
            Dictionary<string, long> maxLag = new Dictionary<string, long>();
            foreach (Log log in logs)
            {
                foreach (Seg seg in log.Segs)
                {
                    if (seg.Kind != "Start" || seg.Command.Paused || seg.Mode != "Queue" || log.HasOverrides) continue;
                    bool interrupted = false;
                    foreach (Cmd c in log.Cmds)
                        if (c.Rec > seg.Command.Rec && (c.Verb == "Pause" || c.Verb == "Resume" || c.Verb == "Seek" || c.Verb == "Start")) interrupted = true;
                    if (interrupted) continue;
                    // only the Queue scenarios with no service time (immediate dispatch): the lag is then pure scheduling
                    if (log.Settings.ContainsKey("simulate-slowdown") && log.Settings["simulate-slowdown"] == "on" && HasService(log)) continue;
                    long origin = log.R[seg.Command.Rec].T;
                    string key = log.Group + " " + log.Scenario + "/" + log.Fixture;
                    List<long> list;
                    if (!lags.TryGetValue(key, out list)) lags[key] = list = new List<long>();
                    foreach (Rec r in seg.Src) list.Add(r.T - (origin + (log.Song.Time[r.Src] - seg.StartPos)));
                }
            }
            List<string> latencyKeys = new List<string>(Latencies.Keys);
            latencyKeys.Sort(StringComparer.Ordinal);
            foreach (string key in latencyKeys)
            {
                List<long> list = Latencies[key];
                list.Sort();
                Info.Add("CONTROL-LATENCY " + key + " n=" + list.Count + " p50=" + list[list.Count / 2] + " max=" + list[list.Count - 1] + " (us from the command to its effect on the output)");
            }
            List<string> keys = new List<string>(lags.Keys);
            keys.Sort(StringComparer.Ordinal);
            foreach (string key in keys)
            {
                List<long> list = lags[key];
                list.Sort();
                if (list.Count == 0) continue;
                Info.Add("LAG " + key + " n=" + list.Count + " p50=" + list[list.Count / 2] + " p90=" + list[(int)(list.Count * 0.9)] +
                    " p99=" + list[(int)(list.Count * 0.99)] + " max=" + list[list.Count - 1] + " (us behind due time, all repeats pooled)");
            }
            QualityChecks(lags);
        }

        // ---- quality properties: v2 must be no worse than v1 (it may be much better)
        //
        // Timing quality is compared with v1's measurement of the same configuration on the same architecture in the
        // same run, not with a fixed bound: "equal or better" with a noise margin, because the PC is not quiet and the
        // lanes run in parallel. A value that is lower than v1's always passes. Ordering, count and correctness
        // properties are not touched by this: they stay exact.
        const double QualityRatio = 1.25;           // v2 may be up to 25 % above v1 ...
        const long LatenessMedianMargin = 200;      // ... plus this many microseconds (dispatch lateness, median)
        const long LatenessTailMargin = 1000;       // (99th percentile)
        const long JitterMargin = 1000;
        const long CommandMedianMargin = 500;       // command-to-effect latency, median
        const long CommandWorstMargin = 5000;       // worst case
        const int MinimumSamplesMedian = 20, MinimumSamplesTail = 100;

        static long Percentile(List<long> sorted, double fraction)
        {
            int index = (int)(sorted.Count * fraction);
            return sorted[Math.Min(index, sorted.Count - 1)];
        }

        static bool NoWorse(long v2, long v1, long margin) { return v2 <= (long)(v1 * QualityRatio) + margin; }

        // Records one comparison: the v2 lane gets the verdict, the v1 lane a passing check (it is the reference), so that
        // a property that never ran in either lane is reported NOT-EXERCISED.
        static void QualityCheck(string id, string arch, string config, string what, long v2, long v1, long margin)
        {
            bool ok = NoWorse(v2, v1, margin);
            Check(id, "v2-" + arch, config, ok, what + ": v2 " + v2 + "us is worse than v1 " + v1 + "us (allowed: v1 x " + QualityRatio + " + " + margin + "us)");
            Check(id, "v1-" + arch, config, true, "");
        }

        static void QualityChecks(Dictionary<string, List<long>> lags)
        {
            foreach (string arch in new string[] { "x64", "x86" })
            {
                foreach (string key in new List<string>(lags.Keys))
                {
                    // key = "<group> <scenario>/<fixture>"
                    int space = key.IndexOf(' ');
                    if (space < 0 || key.Substring(0, space) != "v2-" + arch) continue;
                    string config = key.Substring(space + 1);
                    List<long> v2, v1;
                    if (!lags.TryGetValue("v1-" + arch + " " + config, out v1)) continue;
                    v2 = lags[key];
                    if (v1.Count < MinimumSamplesMedian || v2.Count < MinimumSamplesMedian) continue;   // both lists are sorted by Timing
                    QualityCheck("L01", arch, config, "median lateness", Percentile(v2, 0.5), Percentile(v1, 0.5), LatenessMedianMargin);
                    if (v1.Count >= MinimumSamplesTail && v2.Count >= MinimumSamplesTail)
                    {
                        QualityCheck("L01", arch, config, "99th percentile lateness", Percentile(v2, 0.99), Percentile(v1, 0.99), LatenessTailMargin);
                        QualityCheck("J01", arch, config, "jitter (p99 - median)", Percentile(v2, 0.99) - Percentile(v2, 0.5),
                            Percentile(v1, 0.99) - Percentile(v1, 0.5), JitterMargin);
                    }
                }
                foreach (string what in new string[] { "override", "channel disable" })
                {
                    List<long> v2, v1;
                    if (!Latencies.TryGetValue("v2-" + arch + " " + what, out v2) || !Latencies.TryGetValue("v1-" + arch + " " + what, out v1)) continue;
                    if (v1.Count < 5 || v2.Count < 5) continue;
                    QualityCheck("K01", arch, what, "median command latency", Percentile(v2, 0.5), Percentile(v1, 0.5), CommandMedianMargin);
                    QualityCheck("K01", arch, what, "worst command latency", v2[v2.Count - 1], v1[v1.Count - 1], CommandWorstMargin);
                }
            }
        }

        static bool HasService(Log log)
        {
            foreach (string[] line in log.Setup)
                if (line[0] == "service" && line.Length >= 3 && line[2] != "0") return true;
            return false;
        }

        static void Spread(List<Log> logs)
        {
            Dictionary<string, List<Log>> byConfig = new Dictionary<string, List<Log>>();
            foreach (Log log in logs)
            {
                string key = log.Scenario + "/" + log.Fixture;
                List<Log> list;
                if (!byConfig.TryGetValue(key, out list)) byConfig[key] = list = new List<Log>();
                list.Add(log);
            }
            List<string> keys = new List<string>(byConfig.Keys);
            keys.Sort(StringComparer.Ordinal);
            foreach (string key in keys)
            {
                List<Log> list = byConfig[key];
                if (list[0].Class == "exact") continue;
                StringBuilder text = new StringBuilder("SPREAD " + key + " (processed/dropped per program-arch: min..max over repeats)");
                Dictionary<string, long[]> ranges = new Dictionary<string, long[]>();
                foreach (Log log in list)
                {
                    Rec fin = FinalOf(log);
                    if (fin == null) continue;
                    long[] r;
                    if (!ranges.TryGetValue(log.Group, out r)) ranges[log.Group] = r = new long[] { Int64.MaxValue, Int64.MinValue, Int64.MaxValue, Int64.MinValue };
                    r[0] = Math.Min(r[0], fin.Get("processed")); r[1] = Math.Max(r[1], fin.Get("processed"));
                    r[2] = Math.Min(r[2], fin.Get("dropped")); r[3] = Math.Max(r[3], fin.Get("dropped"));
                }
                List<string> groups = new List<string>(ranges.Keys);
                groups.Sort(StringComparer.Ordinal);
                foreach (string group in groups)
                {
                    long[] r = ranges[group];
                    text.Append(" | ").Append(group).Append(' ').Append(r[0] == r[1] ? r[0].ToString() : r[0] + ".." + r[1]).Append('/').Append(r[2] == r[3] ? r[3].ToString() : r[2] + ".." + r[3]);
                }
                long lo = Int64.MaxValue, hi = Int64.MinValue, dlo = Int64.MaxValue, dhi = Int64.MinValue;
                foreach (long[] r in ranges.Values) { lo = Math.Min(lo, r[0]); hi = Math.Max(hi, r[1]); dlo = Math.Min(dlo, r[2]); dhi = Math.Max(dhi, r[3]); }
                text.Append(lo == hi && dlo == dhi ? " => STABLE: identical in all " + list.Count + " runs of the four lanes" : "");
                Info.Add(text.ToString());
            }
        }

        // ---------------------------------------------------------------- report

        static void Write(string path, List<Log> logs)
        {
            StringBuilder text = new StringBuilder();
            text.Append("LOGS ").Append(logs.Count).Append('\n');
            foreach (string[] property in Properties)
            {
                Dictionary<string, PropertyResult> byGroup;
                Results.TryGetValue(property[0], out byGroup);
                foreach (string group in new string[] { "v1-x64", "v1-x86", "v2-x64", "v2-x86" })
                {
                    PropertyResult r = null;
                    if (byGroup != null) byGroup.TryGetValue(group, out r);
                    text.Append("PROP ").Append(property[0]).Append(' ').Append(group).Append(' ')
                        .Append(r == null ? 0 : r.Checks).Append(' ').Append(r == null ? 0 : r.Failures).Append('\n');
                    if (r != null)
                        foreach (string d in r.Details) text.Append("FAIL ").Append(property[0]).Append(' ').Append(group).Append(' ').Append(d.Replace('\n', ' ')).Append('\n');
                }
            }
            foreach (string i in Info) text.Append("INFO ").Append(i).Append('\n');
            foreach (KeyValuePair<string, long[]> u in Uncounted)
                text.Append("NOTE v1 ordered finite Drop path, ").Append(u.Key).Append(": DroppedEvents omits ").Append(u.Value[0]).Append("..").Append(u.Value[1])
                    .Append(" NoteOff(s) per run that were suppressed with their dropped NoteOn (").Append(u.Value[2]).Append(" runs; PlaybackEngine.Ordered.cs:898-903 versus the classic path's PlaybackEngine.cs:1685)\n");
            foreach (string n in Notes) text.Append("NOTE ").Append(n).Append('\n');
            File.WriteAllText(path, text.ToString(), new UTF8Encoding(false));
        }
    }
}
