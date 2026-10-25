// Real-device measurement driver for v1's UNCHANGED engine and outputs (v1.0.44;
// user prompt 007, Step 2). It plays a generated fixture through v1's real
// PlaybackEngine into v1's real WindowsMidiOutput or KdmApiMidiOutput (both end
// in OmniMIDI) and records when sends really happen. It is the v1 twin of
// v2/tools/device_measure_main.cpp and writes the same files:
//   <out>\<label>.r<N>.run.txt      key=value lines
//   <out>\<label>.r<N>.samples.bin  sampled sends (format: v2/tools/device_analysis.h)
// The statistics are produced by `midibn-device-measure analyze` for both programs.
//
// NOTHING is shadowed: Stopwatch, SpinWait, the waitable timer, EventWaitHandles
// and threads are the framework's, and v1's own HighResolutionWaiter runs.
//
// This program OPENS REAL DEVICES. Never run it directly: use
// v2\tools\device\run-device-measurements.ps1, which mutes the playback
// endpoints first and restores them afterwards.
//
// Usage:
//   v1-device.exe run --output winmm|kdmapi|null --fixture <f.mid> --out <dir> --label <name>
//                     [--repeats 3] [--instr none|count|sample|tsall] [--sample-every 128]
//                     [--device-name OmniMIDI] [--warm 1] [--settle-ms 1000]
//   v1-device.exe raw --output winmm|kdmapi|null --layer api|func|delegate|dllimport|output --seconds 2 [--repeats 3]
//   v1-device.exe devices
//
// Compiled with C# 5 (the .NET Framework csc.exe).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal enum Instr { None, Count, Sample, TsAll }

    // The latest value set on each channel (-1 = never set): volume (CC7), expression (CC11), sustain pedal (CC64),
    // program and bank. Used for what the file has set before a position and for what the state chase sent.
    internal sealed class ChannelValues
    {
        internal readonly short[] Cc7 = new short[16], Cc11 = new short[16], Cc64 = new short[16], Program = new short[16], BankMsb = new short[16], BankLsb = new short[16];
        internal int ResetChannels;   // channels that had a Reset All Controllers (CC121)
        internal ChannelValues()
        {
            for (int c = 0; c < 16; c++) { Cc7[c] = -1; Cc11[c] = -1; Cc64[c] = -1; Program[c] = -1; BankMsb[c] = -1; BankLsb[c] = -1; }
        }
        internal ChannelValues Clone()
        {
            ChannelValues v = new ChannelValues();
            Array.Copy(Cc7, v.Cc7, 16); Array.Copy(Cc11, v.Cc11, 16); Array.Copy(Cc64, v.Cc64, 16);
            Array.Copy(Program, v.Program, 16); Array.Copy(BankMsb, v.BankMsb, 16); Array.Copy(BankLsb, v.BankLsb, 16);
            v.ResetChannels = ResetChannels;
            return v;
        }
        internal void Note(int status, int d1, int d2)
        {
            int ch = status & 15;
            if ((status & 0xF0) == 0xC0) Program[ch] = (short)d1;
            else if ((status & 0xF0) == 0xB0)
            {
                if (d1 == 7) Cc7[ch] = (short)d2;
                else if (d1 == 11) Cc11[ch] = (short)d2;
                else if (d1 == 64) Cc64[ch] = (short)d2;
                else if (d1 == 0) BankMsb[ch] = (short)d2;
                else if (d1 == 32) BankLsb[ch] = (short)d2;
                else if (d1 == 121) ResetChannels |= 1 << ch;
            }
        }
    }

    // Wraps v1's real output. Per send: one counter increment and one compare (Count), plus two
    // timer reads on every `every`-th send (Sample), or on every send (TsAll, the naive approach,
    // kept only to measure what it costs). Runs on the engine's single sending thread; read after
    // the worker has stopped.
    internal sealed class MeasuringOutput : IMidiOutput, IMidiOutputContext, IDisposable
    {
        private readonly IMidiOutput _inner;
        private readonly Instr _mode;
        private readonly long _mask;
        private readonly long _last;
        private long _count;
        internal long SumDuration;
        internal long IndexMismatches;
        internal long ChaseSends;
        internal readonly ChannelValues ChaseSeen = new ChannelValues();
        internal long DoneStamp;
        private int _done;
        private readonly long _indexBase;
        private readonly long _startUs;
        internal readonly long[] K;
        internal readonly long[] Before;
        internal readonly long[] Intended;
        internal readonly int[] Duration;
        internal int N;

        internal MeasuringOutput(IMidiOutput inner, Instr mode, int every, long expected, long indexBase, long startUs)
        {
            _indexBase = indexBase;
            _startUs = startUs;
            _inner = inner;
            _mode = mode;
            _last = expected - 1;
            _mask = mode == Instr.Sample ? every - 1 : (mode == Instr.TsAll ? 127 : Int64.MaxValue);
            long stride = mode == Instr.Sample ? every : 128;
            int capacity = (int)(expected / stride + 8);
            K = new long[capacity]; Before = new long[capacity]; Intended = new long[capacity]; Duration = new int[capacity];
        }

        internal long Count { get { return _count; } }
        internal bool Done { get { return Thread.VolatileRead(ref _done) != 0; } }
        internal long WindowSends { get { return Math.Min(_count, _last + 1); } }
        internal long ExtraSends { get { return Math.Max(0L, _count - (_last + 1)); } }

        public string SourceFile
        {
            get { IMidiOutputContext c = _inner as IMidiOutputContext; return c == null ? null : c.SourceFile; }
            set { IMidiOutputContext c = _inner as IMidiOutputContext; if (c != null) c.SourceFile = value; }
        }

        public void Send(MidiEventView midiEvent)
        {
            // A state-chase message (play from a position) has no event index (-1): forwarded, not counted as a scheduled event.
            if (midiEvent.EventIndex < 0)
            {
                ChaseSends++;
                MidiEventData d = midiEvent.Data;
                if (d.Length >= 2) ChaseSeen.Note(midiEvent.Status, d[1], d.Length > 2 ? d[2] : 0);
                _inner.Send(midiEvent);
                return;
            }
            long c = _count++;
            if (((c & _mask) == 0 && c <= _last) || c == _last)
            {
                long b = Stopwatch.GetTimestamp();
                _inner.Send(midiEvent);
                long a = Stopwatch.GetTimestamp();
                if (_mode == Instr.TsAll) SumDuration += a - b;
                int i = N++;
                // The intended time is counted from the start position (a passage starts the engine clock there).
                K[i] = c; Before[i] = b; Duration[i] = (int)(a - b); Intended[i] = midiEvent.IntendedMicroseconds - _startUs;
                if (midiEvent.EventIndex != _indexBase + c) IndexMismatches++;
                if (c == _last) { DoneStamp = a; Thread.VolatileWrite(ref _done, 1); }
            }
            else if (_mode == Instr.TsAll)
            {
                long b = Stopwatch.GetTimestamp();
                _inner.Send(midiEvent);
                SumDuration += Stopwatch.GetTimestamp() - b;
            }
            else _inner.Send(midiEvent);
        }

        public void Panic() { _inner.Panic(); }
        public void Reset() { _inner.Reset(); }
        public void Dispose() { }
    }

    internal static class DeviceMeasure
    {
        [DllImport("kernel32.dll")]
        private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
        [DllImport("kernel32.dll")]
        private static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll")]
        private static extern IntPtr GetCurrentProcess();
        [DllImport("winmm.dll")]
        private static extern uint midiOutOpen(out IntPtr handle, uint deviceId, IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll")]
        private static extern uint midiOutClose(IntPtr handle);
        [DllImport("winmm.dll")]
        private static extern uint midiOutReset(IntPtr handle);
        [DllImport("winmm.dll")]
        private static extern uint midiOutShortMsg(IntPtr handle, uint message);
        // The cheapest managed-to-native path to the KDMAPI export, for comparison with v1's delegate.
        [DllImport("OmniMIDI.dll", EntryPoint = "SendDirectData")]
        private static extern void KdmSendDirectDataImport(uint message);

        [DllImport("ntdll.dll")]
        private static extern int NtQueryTimerResolution(out uint minimum, out uint maximum, out uint current);

        // The system timer resolution in effect now, in microseconds (15625 = default 15.6 ms tick; 1000 = something
        // called timeBeginPeriod(1), e.g. the OmniMIDI driver). It decides how long Sleep(1) really sleeps.
        private static double TimerResolutionUs()
        {
            uint min, max, cur;
            return NtQueryTimerResolution(out min, out max, out cur) == 0 ? cur / 10.0 : -1.0;
        }

        private static string Inv(double v, int digits) { return v.ToString("F" + digits, CultureInfo.InvariantCulture); }

        private static Dictionary<string, string> Parse(string[] args, int from)
        {
            Dictionary<string, string> d = new Dictionary<string, string>();
            for (int i = from; i + 1 < args.Length; i += 2) d[args[i].TrimStart('-')] = args[i + 1];
            return d;
        }
        private static string Get(Dictionary<string, string> d, string key, string fallback)
        { string v; return d.TryGetValue(key, out v) ? v : fallback; }

        private static int Main(string[] args)
        {
            try
            {
                if (args.Length >= 1 && args[0] == "devices") return Devices();
                // The output kinds this driver can play through (the run script asks, and skips a path whose kind is missing).
                if (args.Length >= 1 && args[0] == "outputs") { Console.WriteLine("outputs=null,winmm,kdmapi"); return 0; }
                if (args.Length >= 1 && args[0] == "run") return Run(Parse(args, 1));
                if (args.Length >= 1 && args[0] == "raw") return Raw(Parse(args, 1));
            }
            catch (Exception ex)
            {
                Console.WriteLine("v1-device ERROR: " + ex.GetType().Name + ": " + ex.Message);
                return 2;
            }
            Console.WriteLine("usage: v1-device.exe devices | run ... | raw ...  (see the header of DeviceMeasure.cs)");
            return 2;
        }

        private static int Devices()
        {
            Console.WriteLine("winmm_module=" + WindowsMidiOutput.GetLoadedModulePath());
            foreach (MidiOutputDeviceInfo d in WindowsMidiOutput.GetDevices()) Console.WriteLine("device " + d.DeviceId + " name=" + d.Name);
            return 0;
        }

        // ---- calibration ---------------------------------------------------

        private static double CalibrateTimerNs()
        {
            const int reads = 4000000;
            long a = Stopwatch.GetTimestamp();
            long sink = 0;
            for (int i = 0; i < reads; i++) sink = Stopwatch.GetTimestamp();
            long b = Stopwatch.GetTimestamp();
            GC.KeepAlive(sink.ToString());
            return (double)(b - a) * 1e9 / Stopwatch.Frequency / reads;
        }

        private static double CalibrateLockNs()
        {
            object gate = new object();
            const int n = 5000000;
            int sink = 0;
            long a = Stopwatch.GetTimestamp();
            for (int i = 0; i < n; i++) { lock (gate) { sink++; } }
            long b = Stopwatch.GetTimestamp();
            GC.KeepAlive(sink.ToString());
            return (double)(b - a) * 1e9 / Stopwatch.Frequency / n;
        }

        // ---- CPU load ------------------------------------------------------

        private struct Cpu
        {
            internal long Idle, Kernel, User, Self;
            internal static Cpu Take()
            {
                Cpu c = new Cpu();
                long creation, exit, sk, su;
                GetSystemTimes(out c.Idle, out c.Kernel, out c.User);
                GetProcessTimes(GetCurrentProcess(), out creation, out exit, out sk, out su);
                c.Self = sk + su;
                return c;
            }
        }

        private static void CpuLines(List<string> info, Cpu a, Cpu b)
        {
            double total = (double)((b.Kernel - a.Kernel) + (b.User - a.User));
            double idle = (double)(b.Idle - a.Idle);
            double self = (double)(b.Self - a.Self);
            info.Add("logical_cpus=" + Environment.ProcessorCount);
            if (total > 0)
            {
                info.Add("cpu_total_busy_pct=" + Inv(100.0 * (total - idle) / total, 1));
                info.Add("cpu_this_process_pct=" + Inv(100.0 * self / total, 1));
                info.Add("cpu_other_processes_pct=" + Inv(100.0 * Math.Max(0.0, total - idle - self) / total, 1));
            }
        }

        // ---- outputs -------------------------------------------------------

        private sealed class Opened
        {
            internal IMidiOutput Output;
            internal IDisposable Disposable;
            internal string Identity;
        }

        private static Opened OpenOutput(string kind, string deviceName)
        {
            Opened o = new Opened();
            if (kind == "null")
            {
                NullMidiOutput n = new NullMidiOutput();
                o.Output = n; o.Disposable = n; o.Identity = "null";
                return o;
            }
            if (kind == "winmm")
            {
                WindowsMidiOutput w = new WindowsMidiOutput();
                long id = -1;
                foreach (MidiOutputDeviceInfo d in WindowsMidiOutput.GetDevices()) if (d.Name == deviceName) { id = d.DeviceId; break; }
                if (id < 0) throw new InvalidOperationException("WinMM device '" + deviceName + "' not found");
                w.Open((uint)id);
                o.Output = w; o.Disposable = w;
                o.Identity = "winmm device=" + id + " module=" + WindowsMidiOutput.GetLoadedModulePath();
                return o;
            }
            if (kind == "kdmapi")
            {
                KdmApiMidiOutput k = new KdmApiMidiOutput();
                k.Open();
                o.Output = k; o.Disposable = k;
                o.Identity = "kdmapi provider=" + k.ProviderPath;
                return o;
            }
            throw new InvalidOperationException("unknown output '" + kind + "'");
        }

        // ---- one measured playback ------------------------------------------

        private static void SetAppEngineSettings(PlaybackEngine engine)
        {
            // The settings MainForm applies at start-up (MainForm.cs lines 275-283, 306).
            engine.SimulateSlowdown = false;
            engine.ApplyQueueLimitWithoutSlowdown = false;
            engine.ChaseMidiStateOnPlaySeek = true;
            engine.PreservePlaybackQueueOnPauseSeek = false;
            engine.OutputPacedTimeline = false;
            engine.ScaleNoteIntervalByTraffic = false;
            engine.IntervalPolyphonyLimitEnabled = false;
            engine.IntervalPolyphonyLimit = PlaybackEngine.DefaultIntervalPolyphonyLimit;
            engine.ProcessingMicroseconds = 100;
        }

        // A part of the file to play: source seconds it starts and ends at, and the optional "mark" (where the heavy
        // passage itself starts, after a lead-in).
        private sealed class Window
        {
            internal double StartS;
            internal double EndS = -1;    // < 0: to the end of the file
            internal double MarkS = -1;   // < 0: none
        }

        private sealed class PlayParams
        {
            internal bool Passage;
            internal long StartUs;
            internal long EndUs;
            internal long MarkUs = -1;
            internal int StartIndex;
            internal long WindowEvents;
            internal long PassageEvents;   // events from the mark (the heavy part) to the end of the window
            internal bool Chase = true;   // "Chase MIDI state on Play/Seek" (the player default: on)
            internal bool HaveHeld;
            internal long HeldStartVoices, HeldMarkVoices;
            internal ChannelValues ValuesStart;
            internal int HeldStartDistinct, HeldMarkDistinct, PedalStart, PedalMark;
            internal double LimitSeconds;
            internal double WindowSeconds;
        }

        // What the file itself says is sounding at an event index: notes started and not yet ended (stacked notes count once
        // each), distinct (channel, note) pairs, and channels with the sustain pedal down. Sustain tails and release times are
        // not modelled. A direct start at a passage misses the note starts before it, so these are the notes that stay silent
        // there but sound when playback starts earlier (lead-in).
        private struct Held
        {
            internal long Voices;
            internal int Distinct;
            internal int PedalChannels;
            internal ChannelValues Values;
        }

        private static Dictionary<int, Held> ScanHeld(MidiSong song, List<int> targetList)
        {
            Dictionary<int, Held> result = new Dictionary<int, Held>();
            SortedDictionary<int, bool> unique = new SortedDictionary<int, bool>();
            foreach (int t0 in targetList) unique[t0] = true;
            if (unique.Count == 0) return result;
            List<int> targets = new List<int>(unique.Keys);
            int[] counts = new int[16 * 128];
            bool[] pedal = new bool[16];
            long voices = 0;
            int distinct = 0;
            int ti = 0;
            int last = targets[targets.Count - 1];
            ChannelValues state = new ChannelValues();
            MidiEventReader reader = song.GetEventReader();
            for (int i = 0; i <= last; i++)
            {
                while (ti < targets.Count && targets[ti] == i)
                {
                    Held h = new Held();
                    h.Voices = voices; h.Distinct = distinct; h.Values = state.Clone();
                    for (int c = 0; c < 16; c++) if (pedal[c]) h.PedalChannels++;
                    result[i] = h;
                    ti++;
                }
                if (i == last) break;
                MidiEventView e = reader[i];
                MidiEventKind kind = e.Kind;
                if (kind != MidiEventKind.NoteOn && kind != MidiEventKind.NoteOff && kind != MidiEventKind.ControlChange && kind != MidiEventKind.ProgramChange) continue;
                int length = e.DataLength;
                if (length < 2) continue;
                MidiEventData data = e.Data;
                int ch = e.Channel & 15;
                if (length >= 3 && kind == MidiEventKind.NoteOn && data[2] != 0)
                {
                    int slot = ch * 128 + (data[1] & 127);
                    if (counts[slot]++ == 0) distinct++;
                    voices++;
                }
                else if (length >= 3 && (kind == MidiEventKind.NoteOn || kind == MidiEventKind.NoteOff))
                {
                    int slot = ch * 128 + (data[1] & 127);
                    if (counts[slot] > 0) { if (--counts[slot] == 0) distinct--; voices--; }
                }
                else if (kind == MidiEventKind.ControlChange && length >= 3) { if (data[1] == 64) pedal[ch] = data[2] >= 64; state.Note(0xB0 | ch, data[1], data[2]); }
                else if (kind == MidiEventKind.ProgramChange) state.Note(0xC0 | ch, data[1], 0);
            }
            return result;
        }

        // One look at the engine's snapshot, taken about every 16 ms like the player's statistics timer.
        private struct SnapRow
        {
            internal double WallMs;
            internal long PlaybackUs, TimelineUs, FrontierUs, Processed, Dropped, CurrentLagUs, MaxLagUs;
            internal bool HasSpeed;
            internal double Speed;
        }

        private static List<string> SplitList(string text, char separator)
        {
            List<string> parts = new List<string>();
            foreach (string p in text.Split(separator)) if (p.Length > 0) parts.Add(p);
            return parts;
        }

        // "120-126" or "118-126:123" (window start-end, optional :mark), several separated by commas.
        private static List<Window> ParseWindows(string text)
        {
            List<Window> windows = new List<Window>();
            foreach (string part in SplitList(text, ','))
            {
                Window w = new Window();
                string range = part;
                int colon = part.IndexOf(':');
                if (colon >= 0) { w.MarkS = Double.Parse(part.Substring(colon + 1), CultureInfo.InvariantCulture); range = part.Substring(0, colon); }
                int dash = range.IndexOf('-', 1);
                if (dash < 0) throw new ArgumentException("bad passage '" + part + "' (use start-end in seconds)");
                w.StartS = Double.Parse(range.Substring(0, dash), CultureInfo.InvariantCulture);
                string endText = range.Substring(dash + 1);
                w.EndS = endText.Length == 0 ? -1.0 : Double.Parse(endText, CultureInfo.InvariantCulture);
                if (w.StartS < 0 || (w.EndS >= 0 && w.EndS <= w.StartS)) throw new ArgumentException("bad passage '" + part + "'");
                windows.Add(w);
            }
            return windows;
        }

        private static double PercentileOf(List<double> values, double p)
        {
            if (values.Count == 0) return 0.0;
            double[] v = values.ToArray();
            Array.Sort(v);
            double rank = p / 100.0 * (v.Length - 1);
            int lo = (int)rank;
            int hi = Math.Min(v.Length - 1, lo + 1);
            double frac = rank - lo;
            return v[lo] * (1.0 - frac) + v[hi] * frac;
        }

        private static string Mb(long bytes) { return Inv(bytes / 1048576.0, 1); }

        private static string Find(List<string> info, string key)
        {
            foreach (string l in info) if (l.StartsWith(key + "=", StringComparison.Ordinal)) return l.Substring(key.Length + 1);
            return "";
        }

        private static int Run(Dictionary<string, string> opt)
        {
            string fixture = Get(opt, "fixture", "");
            string outDir = Get(opt, "out", ".");
            string label = Get(opt, "label", "run");
            int repeats = Int32.Parse(Get(opt, "repeats", "3"), CultureInfo.InvariantCulture);
            List<string> instrNames = SplitList(Get(opt, "instr", "sample"), ',');
            int every = Int32.Parse(Get(opt, "sample-every", "128"), CultureInfo.InvariantCulture);
            int settleMs = Int32.Parse(Get(opt, "settle-ms", "1000"), CultureInfo.InvariantCulture);
            int pathSettleMs = Int32.Parse(Get(opt, "path-settle-ms", "3000"), CultureInfo.InvariantCulture);
            bool warm = Get(opt, "warm", "1") != "0";
            bool loadOnly = Get(opt, "load-only", "0") != "0";
            if (every <= 0 || (every & (every - 1)) != 0) throw new ArgumentException("--sample-every must be a power of two");

            // Outputs: one (--output) or several (--outputs a,b,c) played in sequence in this process, so a big file is loaded once.
            string outputsText = Get(opt, "outputs", Get(opt, "output", ""));
            List<string> kinds = SplitList(outputsText, ',');
            if (kinds.Count == 0 && !loadOnly) throw new ArgumentException("--output or --outputs is needed");
            List<string> tags = opt.ContainsKey("path-tags") ? SplitList(opt["path-tags"], ',') : kinds;
            if (tags.Count != kinds.Count) throw new ArgumentException("--path-tags must have as many entries as --outputs");
            List<double> capExtras = new List<double>();
            foreach (string c in SplitList(Get(opt, "cap-extras", ""), ',')) capExtras.Add(Double.Parse(c, CultureInfo.InvariantCulture));
            while (capExtras.Count < kinds.Count) capExtras.Add(120.0);

            // Passages: --passages s-e[:mark],...  or  --start S [--seconds D] [--mark M].
            List<Window> windows = new List<Window>();
            if (opt.ContainsKey("passages")) windows = ParseWindows(opt["passages"]);
            else if (opt.ContainsKey("start") || opt.ContainsKey("seconds"))
            {
                Window w = new Window();
                w.StartS = Double.Parse(Get(opt, "start", "0"), CultureInfo.InvariantCulture);
                w.EndS = opt.ContainsKey("seconds") ? w.StartS + Double.Parse(opt["seconds"], CultureInfo.InvariantCulture) : -1.0;
                w.MarkS = opt.ContainsKey("mark") ? Double.Parse(opt["mark"], CultureInfo.InvariantCulture) : -1.0;
                windows.Add(w);
            }
            bool passageMode = windows.Count > 0;

            string arch = IntPtr.Size == 8 ? "x64" : "x86";
            double timerNs = loadOnly ? 0.0 : CalibrateTimerNs();
            double lockNs = loadOnly ? 0.0 : CalibrateLockNs();
            Process self = Process.GetCurrentProcess();
            self.Refresh();
            long privateBefore = self.PrivateMemorySize64;
            long loadStart = Stopwatch.GetTimestamp();
            MidiSong song = DirectCompactMidiParser.Load(fixture, CancellationToken.None, null);
            double loadSeconds = (double)(Stopwatch.GetTimestamp() - loadStart) / Stopwatch.Frequency;
            self.Refresh();
            long privateAfterLoad = self.PrivateMemorySize64;
            long peakPrivateAfterLoad = self.PeakPagedMemorySize64;
            long peakWorkingSetAfterLoad = self.PeakWorkingSet64;
            long events = song.GetEventReader().Count;
            long durationUs = song.DurationMicroseconds;
            List<string> loadLines = new List<string>();
            loadLines.Add("program=v1");
            loadLines.Add("arch=" + arch);
            loadLines.Add("fixture=" + fixture);
            loadLines.Add("events=" + events);
            loadLines.Add("duration_ms=" + Inv(durationUs / 1000.0, 1));
            loadLines.Add("load_s=" + Inv(loadSeconds, 2));
            loadLines.Add("chase_prepare_s=0.00");   // v1 builds the state-chase index inside the load (DirectCompactMidiParser)
            loadLines.Add("ready_to_play_s=" + Inv(loadSeconds, 2));
            loadLines.Add("private_mb_before_load=" + Mb(privateBefore));
            loadLines.Add("private_mb_after_load=" + Mb(privateAfterLoad));
            loadLines.Add("peak_private_mb_after_load=" + Mb(peakPrivateAfterLoad));
            loadLines.Add("peak_working_set_mb_after_load=" + Mb(peakWorkingSetAfterLoad));
            File.WriteAllText(Path.Combine(outDir, label + ".load.txt"), String.Join("\n", loadLines.ToArray()) + "\n", new UTF8Encoding(false));
            if (loadOnly)
            {
                Console.WriteLine("device-measure v1 " + arch + " load-only events=" + events + " load_s=" + Inv(loadSeconds, 2) +
                    " peak_private_mb=" + Mb(peakPrivateAfterLoad) + " peak_working_set_mb=" + Mb(peakWorkingSetAfterLoad));
                return 0;
            }

            // "Chase MIDI state on Play/Seek": the player defaults it to on (MainForm); --chase 0 turns it off.
            bool chaseOn = Get(opt, "chase", "1") != "0";
            // Notes the file has sounding at each window start and mark (one pass over the events up to the last of them).
            Dictionary<int, Held> heldAt = new Dictionary<int, Held>();
            if (passageMode && Get(opt, "held-scan", "1") != "0")
            {
                List<int> targets = new List<int>();
                foreach (Window hw in windows)
                {
                    targets.Add(song.EventStore.LowerBoundByTime(Math.Min(durationUs, (long)(hw.StartS * 1e6 + 0.5))));
                    if (hw.MarkS >= 0) targets.Add(song.EventStore.LowerBoundByTime(Math.Min(durationUs, (long)(hw.MarkS * 1e6 + 0.5))));
                }
                long scanStart = Stopwatch.GetTimestamp();
                heldAt = ScanHeld(song, targets);
                Console.WriteLine("held-note scan: " + heldAt.Count + " positions in " + Inv((double)(Stopwatch.GetTimestamp() - scanStart) / Stopwatch.Frequency, 1) + " s");
            }

            Func<Window, PlayParams> makeParams = delegate(Window w)
            {
                PlayParams p = new PlayParams();
                p.StartUs = 0;
                p.EndUs = durationUs;
                p.WindowEvents = events;
                p.WindowSeconds = durationUs / 1e6;
                if (w != null)
                {
                    p.Passage = true;
                    p.StartUs = Math.Min(durationUs, (long)(w.StartS * 1e6 + 0.5));
                    p.EndUs = w.EndS < 0 ? durationUs : Math.Min(durationUs, (long)(w.EndS * 1e6 + 0.5));
                    p.MarkUs = w.MarkS < 0 ? -1 : (long)(w.MarkS * 1e6 + 0.5);
                    p.StartIndex = song.EventStore.LowerBoundByTime(p.StartUs);
                    // A window that runs to the end of the file holds every remaining event.
                    long endIndex = w.EndS < 0 ? events : song.EventStore.LowerBoundByTime(p.EndUs);
                    p.WindowEvents = Math.Max(0L, endIndex - p.StartIndex);
                    int markIndex = p.MarkUs >= 0 ? song.EventStore.LowerBoundByTime(Math.Min(durationUs, p.MarkUs)) : p.StartIndex;
                    p.PassageEvents = Math.Max(0L, endIndex - markIndex);
                    p.Chase = chaseOn;
                    Held hs, hm;
                    if (heldAt.TryGetValue(p.StartIndex, out hs))
                    {
                        if (!heldAt.TryGetValue(markIndex, out hm)) hm = hs;
                        p.HaveHeld = true;
                        p.ValuesStart = hs.Values; p.HeldStartVoices = hs.Voices; p.HeldStartDistinct = hs.Distinct; p.PedalStart = hs.PedalChannels;
                        p.HeldMarkVoices = hm.Voices; p.HeldMarkDistinct = hm.Distinct; p.PedalMark = hm.PedalChannels;
                    }
                    p.WindowSeconds = (p.EndUs - p.StartUs) / 1e6;
                }
                return p;
            };

            // The label of one run: the template with {path} and {pass}, or the plain label (plus -path and -pN when several).
            Func<int, int, string> runLabel = delegate(int outputIndex, int passageIndex)
            {
                string templ = Get(opt, "label-template", "");
                string passText = passageIndex > 0 ? passageIndex.ToString(CultureInfo.InvariantCulture) : "all";
                if (templ.Length > 0) return templ.Replace("{path}", tags[outputIndex]).Replace("{pass}", passText);
                string l = label;
                if (kinds.Count > 1) l += "-" + tags[outputIndex];
                if (passageIndex > 0) l += "-p" + passText;
                return l;
            };

            int passageCount = passageMode ? windows.Count : 1;
            for (int oi = 0; oi < kinds.Count; oi++)
            {
                string kind = kinds[oi];
                Opened opened;
                try { opened = OpenOutput(kind, Get(opt, "device-name", "OmniMIDI")); }
                catch (Exception ex)
                {
                    Console.WriteLine("output " + tags[oi] + " (" + kind + ") could not be opened: " + ex.Message);
                    File.WriteAllText(Path.Combine(outDir, runLabel(oi, 0) + ".error.txt"), "open failed: " + ex.Message + "\n", new UTF8Encoding(false));
                    continue;
                }
                Console.WriteLine("device-measure v1 " + arch + " output=" + kind + " path=" + tags[oi] + " fixture=" + fixture + " events=" + events +
                    " load_s=" + Inv(loadSeconds, 1) + " identity=[" + opened.Identity + "] winmm_module=" + WindowsMidiOutput.GetLoadedModulePath());

                if (warm)
                {
                    List<string> ignore = new List<string>();
                    List<SnapRow> ignoreSnaps = new List<SnapRow>();
                    long t0w, epochw; double msw;
                    PlayParams wp = makeParams(passageMode ? windows[0] : null);
                    Play(song, opened.Output, wp, true, ignore, null, out t0w, out msw, out epochw, ignoreSnaps);
                    Thread.Sleep(settleMs);
                }

                for (int pi = 0; pi < passageCount; pi++)
                {
                    Window window = passageMode ? windows[pi] : null;
                    PlayParams p = makeParams(window);
                    if (p.Passage) p.LimitSeconds = p.WindowSeconds + capExtras[oi];
                    string runName = runLabel(oi, passageMode ? pi + 1 : 0);
                    foreach (string instrName in instrNames)
                    {
                    Instr mode = instrName == "none" ? Instr.None : instrName == "count" ? Instr.Count : instrName == "tsall" ? Instr.TsAll : Instr.Sample;
                    if (passageMode && mode == Instr.None) mode = Instr.Count;   // the passage needs the decorator to know when it is over
                    string instrRunName = instrNames.Count > 1 ? runName + "-i" + instrName : runName;
                    for (int r = 1; r <= repeats; r++)
                    {
                        List<string> info = new List<string>();
                        info.Add("program=v1");
                        info.Add("arch=" + arch);
                        info.Add("output=" + kind);
                        info.Add("path=" + tags[oi]);
                        info.Add("fixture=" + fixture);
                        info.Add("repeat=" + r);
                        info.Add("instr=" + instrName);
                        info.Add("events=" + events);
                        info.Add("fixture_duration_ms=" + Inv(durationUs / 1000.0, 1));
                        info.Add("calib_qpc_ns=" + Inv(timerNs, 2));
                        info.Add("calib_lock_ns=" + Inv(lockNs, 2));
                        info.Add("load_s=" + Inv(loadSeconds, 1));
                        info.Add("peak_private_mb_after_load=" + Mb(peakPrivateAfterLoad));
                        info.Add("peak_working_set_mb_after_load=" + Mb(peakWorkingSetAfterLoad));
                        info.Add("timer_resolution_us_before_play=" + Inv(TimerResolutionUs(), 1));
                        info.Add("identity=" + opened.Identity);
                        info.Add("winmm_module=" + WindowsMidiOutput.GetLoadedModulePath());
                        if (p.Passage)
                        {
                            info.Add("passage_index=" + (pi + 1));
                            info.Add("window_start_s=" + Inv(p.StartUs / 1e6, 3));
                            info.Add("window_end_s=" + Inv(p.EndUs / 1e6, 3));
                            info.Add("mark_s=" + (p.MarkUs < 0 ? "none" : Inv(p.MarkUs / 1e6, 3)));
                            info.Add("window_events=" + p.WindowEvents);
                            info.Add("passage_events=" + p.PassageEvents);
                            info.Add("rate_model=None");
                            info.Add("processing_mode=Queue (no rate model, no queue limit, simulate slowdown off)");
                            info.Add("instrumentation=" + instrName + (instrName == "count" ? " (a counter per send; no timer reads except the first and last send)" : instrName == "sample" ? " (two timer reads on every " + every + "th send)" : ""));
                            info.Add("chase_midi_state_on_play_seek=" + (p.Chase ? "on" : "off"));
                            if (p.HaveHeld)
                            {
                                info.Add("held_at_start_voices=" + p.HeldStartVoices);
                                info.Add("held_at_start_distinct=" + p.HeldStartDistinct);
                                info.Add("pedal_down_channels_at_start=" + p.PedalStart);
                                info.Add("held_at_mark_voices=" + p.HeldMarkVoices);
                                info.Add("held_at_mark_distinct=" + p.HeldMarkDistinct);
                                info.Add("pedal_down_channels_at_mark=" + p.PedalMark);
                            }
                            info.Add("wall_cap_s=" + Inv(p.LimitSeconds, 1));
                        }
                        MeasuringOutput measuring = null;
                        IMidiOutput output = opened.Output;
                        if (mode != Instr.None)
                        {
                            measuring = new MeasuringOutput(opened.Output, mode, every, p.Passage ? p.WindowEvents : events,
                                p.Passage ? p.StartIndex : 0, p.Passage ? p.StartUs : 0);
                            output = measuring;
                        }
                        long t0, epoch; double endedMs;
                        List<SnapRow> snaps = new List<SnapRow>();
                        Play(song, output, p, false, info, measuring, out t0, out endedMs, out epoch, snaps);
                        info.Add("engine_epoch_after_t0_us=" + Inv((double)epoch * 1e6 / Stopwatch.Frequency, 1));
                        long sentInWindow = measuring != null ? measuring.WindowSends : events;
                        info.Add("throughput_over_ended_meps=" + Inv(endedMs > 0 ? sentInWindow / endedMs / 1000.0 : 0.0, 4));
                        string basePath = Path.Combine(outDir, instrRunName + ".r" + r);
                        self.Refresh();
                        info.Add("peak_private_mb_after_run=" + Mb(self.PeakPagedMemorySize64));
                        info.Add("peak_working_set_mb_after_run=" + Mb(self.PeakWorkingSet64));
                        if (measuring != null)
                        {
                            info.Add("sends=" + measuring.WindowSends);
                            info.Add("event_index_mismatches=" + measuring.IndexMismatches);
                            if (p.Passage)
                            {
                                info.Add("capped=" + (Find(info, "end_reason") == "CAPPED" ? "yes" : "no"));
                                info.Add("window_sends=" + measuring.WindowSends);
                                info.Add("window_complete=" + (measuring.WindowSends >= p.WindowEvents ? "yes" : "no"));
                                info.Add("extra_sends_after_window=" + measuring.ExtraSends);
                                info.Add("chase_sends=" + measuring.ChaseSends);
                                if (p.HaveHeld && p.Chase && p.StartIndex > 0 && p.ValuesStart != null)
                                {
                                    // Did the chase restore what the file had set before the start position? (per channel: the latest
                                    // CC7 volume, CC11 expression, CC64 sustain, program and bank). A channel that had a Reset All
                                    // Controllers (CC121) before the position may legitimately differ for CC11 and CC64.
                                    ChannelValues want = p.ValuesStart, got = measuring.ChaseSeen;
                                    info.Add(RestoreLine("cc7_volume", want.Cc7, got.Cc7, want.ResetChannels));
                                    info.Add(RestoreLine("cc11_expression", want.Cc11, got.Cc11, want.ResetChannels));
                                    info.Add(RestoreLine("cc64_sustain", want.Cc64, got.Cc64, want.ResetChannels));
                                    info.Add(RestoreLine("program", want.Program, got.Program, want.ResetChannels));
                                    info.Add(RestoreLine("bank_msb", want.BankMsb, got.BankMsb, want.ResetChannels));
                                    info.Add(RestoreLine("bank_lsb", want.BankLsb, got.BankLsb, want.ResetChannels));
                                }
                                long scheduled = measuring.WindowSends + measuring.ExtraSends;
                                info.Add("chase_check=" + (Find(info, "processed_events_final") == scheduled.ToString(CultureInfo.InvariantCulture)
                                    ? "ok" : "processed_events_final differs from the scheduled sends counted"));
                            }
                            if (mode == Instr.TsAll)
                                info.Add("tsall_mean_send_ns=" + Inv((double)measuring.SumDuration * 1e9 / Stopwatch.Frequency / Math.Max(1L, measuring.Count) - timerNs, 1));
                            if (mode != Instr.Count) WriteSamples(basePath + ".samples.bin", measuring, mode, every, t0, timerNs, p.Passage ? p.WindowEvents : events, epoch);
                            else if (measuring.N >= 2)
                            {
                                double spanUs = (double)(measuring.Before[measuring.N - 1] - measuring.Before[0]) * 1e6 / Stopwatch.Frequency;
                                info.Add("throughput_first_to_last_send_meps=" + Inv(spanUs > 0 ? (double)(measuring.K[measuring.N - 1] - measuring.K[0]) / spanUs : 0.0, 4));
                            }
                        }
                        if (p.Passage && snaps.Count > 0)
                        {
                            // The player's own readouts, sampled every 16 ms: Maximum lag / Current lag / Effective speed.
                            // max_lag_* is the engine's statistic (largest lag of any event sent since the run started, reset at Start).
                            info.Add("max_lag_at_last_look_us=" + snaps[snaps.Count - 1].MaxLagUs);
                            if (p.MarkUs >= 0)
                            {
                                foreach (SnapRow row in snaps)
                                    if (row.TimelineUs >= p.MarkUs)
                                    {
                                        info.Add("max_lag_at_mark_us=" + row.MaxLagUs);
                                        info.Add("current_lag_at_mark_us=" + row.CurrentLagUs);
                                        break;
                                    }
                            }
                            SpeedStats(info, "eff_speed", snaps, 0);
                            if (p.MarkUs >= 0) SpeedStats(info, "eff_speed_passage", snaps, p.MarkUs);
                            StringBuilder csv = new StringBuilder("wall_ms,playback_us,timeline_us,frontier_us,processed,dropped,current_lag_us,max_lag_us,speed_pct\n");
                            foreach (SnapRow row in snaps)
                                csv.Append(Inv(row.WallMs, 1)).Append(',').Append(row.PlaybackUs).Append(',').Append(row.TimelineUs).Append(',')
                                    .Append(row.FrontierUs).Append(',').Append(row.Processed).Append(',').Append(row.Dropped).Append(',')
                                    .Append(row.CurrentLagUs).Append(',').Append(row.MaxLagUs).Append(',').Append(row.HasSpeed ? Inv(row.Speed, 1) : "").Append('\n');
                            File.WriteAllText(basePath + ".snapshots.csv", csv.ToString(), new UTF8Encoding(false));
                        }
                        File.WriteAllText(basePath + ".run.txt", String.Join("\n", info.ToArray()) + "\n", new UTF8Encoding(false));
                        Console.WriteLine("repeat " + r + " " + instrRunName + ": ended_ms=" + Inv(endedMs, 1) + " throughput_over_ended_meps=" + Inv(endedMs > 0 ? sentInWindow / endedMs / 1000.0 : 0.0, 3));
                        Thread.Sleep(settleMs);
                    }
                    }
                }
                // The driver can hang in reset or close after a flood; the watchdog then ends the process (the script runs the
                // outputs that were not reached again, in a new process).
                Opened openedForCleanup = opened;
                CleanupWithWatchdog(Path.Combine(outDir, runLabel(oi, 0) + ".cleanup.txt"),
                    delegate { try { openedForCleanup.Output.Reset(); } catch (Exception ex) { Console.WriteLine("reset failed: " + ex.Message); } },
                    delegate { openedForCleanup.Disposable.Dispose(); });
                if (oi + 1 < kinds.Count) Thread.Sleep(pathSettleMs);
            }
            return 0;
        }

        private static string RestoreLine(string name, short[] want, short[] got, int resetChannels)
        {
            int wanted = 0, restored = 0, resetExempt = 0;
            for (int c = 0; c < 16; c++)
            {
                if (want[c] < 0) continue;
                wanted++;
                if (got[c] == want[c]) restored++;
                else if (((resetChannels >> c) & 1) != 0) resetExempt++;
            }
            return "chase_restored_" + name + "=" + restored + "_of_" + wanted +
                (resetExempt > 0 ? "_(" + resetExempt + "_differ_after_a_reset_all_controllers)" : "");
        }

        private static void SpeedStats(List<string> info, string prefix, List<SnapRow> snaps, long fromTimeline)
        {
            List<double> v = new List<double>();
            bool haveFirst = false;
            SnapRow first = new SnapRow(), last = new SnapRow();
            foreach (SnapRow row in snaps)
            {
                if (row.TimelineUs < fromTimeline) continue;
                if (!haveFirst) { first = row; haveFirst = true; }
                last = row;
                if (row.HasSpeed) v.Add(row.Speed);
            }
            if (v.Count > 0)
            {
                double sum = 0, min = Double.MaxValue;
                foreach (double x in v) { sum += x; if (x < min) min = x; }
                info.Add(prefix + "_min_pct=" + Inv(min, 1));
                info.Add(prefix + "_p5_pct=" + Inv(PercentileOf(v, 5), 1));
                info.Add(prefix + "_p50_pct=" + Inv(PercentileOf(v, 50), 1));
                info.Add(prefix + "_mean_pct=" + Inv(sum / v.Count, 1));
                info.Add(prefix + "_last_pct=" + Inv(v[v.Count - 1], 1));
            }
            if (haveFirst && last.PlaybackUs > first.PlaybackUs)
                info.Add(prefix + "_overall_pct=" + Inv(100.0 * (last.FrontierUs - first.FrontierUs) / (last.PlaybackUs - first.PlaybackUs), 1));
        }

        [DllImport("kernel32.dll")]
        private static extern bool TerminateProcess(IntPtr process, uint exitCode);

        // A device driver can hang in reset or close after a flood (system WinMM does, see DEVICE_MEASUREMENTS.md).
        // All results are already on disk by then, so the cleanup runs under a 20 second watchdog that ends the
        // process and says so in <label>.cleanup.txt.
        private static void CleanupWithWatchdog(string logPath, Action reset, Action close)
        {
            StringBuilder text = new StringBuilder();
            object gate = new object();
            ManualResetEvent done = new ManualResetEvent(false);
            Action<string> add = delegate(string line)
            {
                lock (gate) { text.Append(line).Append("\n"); File.WriteAllText(logPath, text.ToString()); }
            };
            Thread watchdog = new Thread(delegate()
            {
                if (done.WaitOne(20000)) return;
                add("cleanup-timeout: reset or close did not return within 20 s; process ended by the watchdog");
                TerminateProcess(GetCurrentProcess(), 0);
            });
            watchdog.IsBackground = true;
            watchdog.Start();
            add("reset-start");
            reset();
            add("reset-done");
            add("close-start");
            close();
            add("close-done");
            done.Set();
        }

        private static void Play(MidiSong song, IMidiOutput output, PlayParams p, bool warm, List<string> info, MeasuringOutput measuring,
            out long t0, out double endedMs, out long epochRelTicks, List<SnapRow> snaps)
        {
            epochRelTicks = 0;
            long endedAt = 0;
            string failure = null;
            long durationUs = song.DurationMicroseconds;
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                SetAppEngineSettings(engine);
                engine.ChaseMidiStateOnPlaySeek = p.Chase;
                EffectivePlaybackSpeed speedWindow = new EffectivePlaybackSpeed();   // the player's default 250 ms window
                engine.PlaybackEnded += delegate { Interlocked.Exchange(ref endedAt, Stopwatch.GetTimestamp()); };
                engine.PlaybackFailed += delegate(object sender, PlaybackErrorEventArgs e)
                { failure = e.Error.GetType().Name + ": " + e.Error.Message.Replace("\r", " ").Replace("\n", " "); Interlocked.Exchange(ref endedAt, Stopwatch.GetTimestamp()); };
                Cpu cpu0 = Cpu.Take();
                t0 = Stopwatch.GetTimestamp();
                string startError = null;
                // Start(song, output, mode, startMicroseconds, startPaused) is what the player calls on Play from a position;
                // the engine then sends the state chase (ChaseMidiStateOnPlaySeek, set by SetAppEngineSettings).
                try { engine.Start(song, output, ProcessingMode.Queue, p.StartUs, false); }
                catch (Exception ex) { startError = ex.GetType().Name + ": " + ex.Message; }
                long startReturned = Stopwatch.GetTimestamp();
                info.Add("start_call_us=" + Inv((double)(startReturned - t0) * 1e6 / Stopwatch.Frequency, 1));
                endedMs = 0;
                if (startError != null) { info.Add("start_error=" + startError); return; }
                double limitSeconds = p.Passage ? p.LimitSeconds : durationUs / 1e6 * 30.0 + 60.0;
                string reason = "completed";
                // The engine's own time zero is a little after t0 (start-up work inside Start). Find it from two
                // snapshots a few milliseconds in: epoch = (timer at the snapshot) - (transport position it reports,
                // counted from the start position). The snapshot that took the least time is used.
                long bestSpan = Int64.MaxValue;
                epochRelTicks = 0;
                int snapshotsTaken = 0;
                long nextSample = t0 + Stopwatch.Frequency * 16 / 1000;
                while (true)
                {
                    long now = Stopwatch.GetTimestamp();
                    double elapsed = (double)(now - t0) / Stopwatch.Frequency;
                    if (snapshotsTaken < 2 && elapsed > (snapshotsTaken == 0 ? 0.003 : 0.006) && Interlocked.Read(ref endedAt) == 0)
                    {
                        long sa = Stopwatch.GetTimestamp();
                        PlaybackSnapshot s = engine.GetSnapshot();
                        long sb = Stopwatch.GetTimestamp();
                        snapshotsTaken++;
                        if (sb - sa < bestSpan)
                        {
                            bestSpan = sb - sa;
                            epochRelTicks = (sa + sb) / 2 - t0 - (s.PlaybackMicroseconds - p.StartUs) * Stopwatch.Frequency / 1000000;
                        }
                    }
                    else if (p.Passage && !warm && now >= nextSample && Interlocked.Read(ref endedAt) == 0)
                    {
                        nextSample = now + Stopwatch.Frequency * 16 / 1000;
                        PlaybackSnapshot s = engine.GetSnapshot();
                        // The same "synchronized" test the player's statistics refresh uses (MainForm.RefreshStatistics).
                        bool synchronized = s.ProcessingMode != ProcessingMode.PerNoteIntervalGate &&
                            s.ProcessingMode != ProcessingMode.DelayedNoteOff && s.ProcessingMode != ProcessingMode.NoteAttackLimit &&
                            s.ProcessingMode != ProcessingMode.OutputRateLimit && s.OutstandingEvents == 0 && s.CurrentLagMicroseconds == 0 &&
                            s.LastDispatchedTimelineMicroseconds <= s.IntendedTimelineMicroseconds;
                        double? speed = s.State == PlaybackState.Playing
                            ? speedWindow.Add(s.PlaybackMicroseconds, s.IntendedTimelineMicroseconds, s.EffectiveSpeedFrontierMicroseconds,
                                s.ProcessedEvents, synchronized, (now - t0) * 1000000 / Stopwatch.Frequency)
                            : (double?)null;
                        SnapRow row = new SnapRow();
                        row.WallMs = (double)(now - t0) * 1e3 / Stopwatch.Frequency;
                        row.PlaybackUs = s.PlaybackMicroseconds;
                        row.TimelineUs = s.IntendedTimelineMicroseconds;
                        row.FrontierUs = s.EffectiveSpeedFrontierMicroseconds;
                        row.Processed = s.ProcessedEvents;
                        row.Dropped = MainForm.TotalDroppedEvents(s);
                        row.CurrentLagUs = s.CurrentLagMicroseconds;
                        row.MaxLagUs = s.MaximumLagMicroseconds;
                        row.HasSpeed = speed.HasValue;
                        row.Speed = speed.HasValue ? speed.Value : 0.0;
                        snaps.Add(row);
                    }
                    if (p.Passage && !warm && measuring != null && measuring.Done) { reason = "completed"; break; }
                    if (Interlocked.Read(ref endedAt) != 0 && !engine.HasLiveWorker) break;
                    if (warm && elapsed > 0.4) { reason = "warm-stopped"; break; }
                    if (elapsed > limitSeconds) { reason = p.Passage ? "CAPPED" : "TIMEOUT"; break; }
                    Thread.Sleep(1);
                }
                long finished = Stopwatch.GetTimestamp();
                Cpu cpu1 = Cpu.Take();
                PlaybackSnapshot snap = engine.GetSnapshot();
                engine.Stop();
                PlaybackSnapshot after = engine.GetSnapshot();
                long ended = Interlocked.Read(ref endedAt);
                long endStamp = ended != 0 ? ended : finished;
                if (p.Passage && measuring != null && measuring.Done) endStamp = measuring.DoneStamp;
                endedMs = (double)(endStamp - t0) * 1e3 / Stopwatch.Frequency;
                info.Add("end_reason=" + reason);
                info.Add("ended_ms=" + Inv(endedMs, 2));
                info.Add("processed_events=" + snap.ProcessedEvents);
                info.Add("dropped_events=" + snap.DroppedEvents);
                info.Add("gate_filtered_events=" + snap.GateFilteredEvents);
                info.Add("engine_max_lag_us=" + snap.MaximumLagMicroseconds);
                info.Add("engine_current_lag_us=" + snap.CurrentLagMicroseconds);
                info.Add("processed_events_final=" + after.ProcessedEvents);
                info.Add("playback_failed=" + (failure ?? "no"));
                CpuLines(info, cpu0, cpu1);
            }
        }

        private static void WriteSamples(string path, MeasuringOutput m, Instr mode, int every, long t0, double timerNs, long events, long epoch)
        {
            using (FileStream stream = new FileStream(path, FileMode.Create, FileAccess.Write))
            using (BinaryWriter w = new BinaryWriter(stream))
            {
                w.Write(Encoding.ASCII.GetBytes("MBDEVS01"));
                w.Write((uint)1);
                w.Write((uint)(mode == Instr.Sample ? every : 128));
                w.Write((long)Stopwatch.Frequency);
                w.Write((long)m.WindowSends);
                w.Write((long)m.N);
                w.Write(timerNs);
                w.Write(events);
                w.Write(epoch); w.Write((long)0); w.Write((long)0);
                for (int i = 0; i < m.N; i++)
                {
                    w.Write(m.K[i]);
                    w.Write(m.Intended[i]);
                    w.Write(m.Before[i] - t0);
                    w.Write(m.Duration[i]);
                    w.Write((int)0);
                }
            }
        }

        // ---- raw ceilings (no engine) -----------------------------------------

        private static uint[] MessageTable()
        {
            uint[] m = new uint[4096];
            for (int i = 0; i < m.Length; i++)
                m[i] = (uint)(0xB0 | (i & 15)) | ((uint)(20 + (i / 16) % 12) << 8) | ((uint)((i * 7) & 127) << 16);
            return m;
        }

        private static double TightLoop(double seconds, Action<uint> send)
        {
            uint[] msgs = MessageTable();
            long freq = Stopwatch.Frequency;
            long start = Stopwatch.GetTimestamp();
            long end = start + (long)(seconds * freq);
            long n = 0;
            long now = start;
            while (now < end)
            {
                for (int i = 0; i < 65536; i++) send(msgs[i & 4095]);
                n += 65536;
                now = Stopwatch.GetTimestamp();
            }
            return (double)n / ((double)(now - start) / freq) / 1e6;
        }

        private static int Raw(Dictionary<string, string> opt)
        {
            string kind = Get(opt, "output", "winmm");
            string layer = Get(opt, "layer", "output");
            double seconds = Double.Parse(Get(opt, "seconds", "2"), CultureInfo.InvariantCulture);
            int repeats = Int32.Parse(Get(opt, "repeats", "3"), CultureInfo.InvariantCulture);
            string deviceName = Get(opt, "device-name", "OmniMIDI");
            string arch = IntPtr.Size == 8 ? "x64" : "x86";
            Thread.CurrentThread.Priority = ThreadPriority.AboveNormal;
            List<double> rates = new List<double>();
            string identity;
            Action<uint> send;
            Action cleanup;
            if (kind == "winmm" && (layer == "api" || layer == "func"))
            {
                long id = -1;
                foreach (MidiOutputDeviceInfo d in WindowsMidiOutput.GetDevices()) if (d.Name == deviceName) { id = d.DeviceId; break; }
                if (id < 0) throw new InvalidOperationException("WinMM device not found");
                IntPtr h;
                if (midiOutOpen(out h, (uint)id, IntPtr.Zero, IntPtr.Zero, 0) != 0) throw new InvalidOperationException("midiOutOpen failed");
                if (layer == "api")
                {
                    identity = "DllImport midiOutShortMsg direct, module=" + WindowsMidiOutput.GetLoadedModulePath();
                    send = delegate(uint m) { midiOutShortMsg(h, m); };
                }
                else
                {
                    // What WindowsMidiOutput does: a Func<IntPtr,uint,uint> delegate bound to the DllImport.
                    Func<IntPtr, uint, uint> sendShort = midiOutShortMsg;
                    identity = "Func delegate over DllImport (v1's _sendShort), module=" + WindowsMidiOutput.GetLoadedModulePath();
                    send = delegate(uint m) { sendShort(h, m); };
                }
                cleanup = delegate { midiOutReset(h); midiOutClose(h); };
            }
            else if (kind == "kdmapi" && (layer == "delegate" || layer == "dllimport"))
            {
                DynamicKdmApiNative native = new DynamicKdmApiNative();
                if (!native.IsAvailable() || !native.InitializeStream()) throw new InvalidOperationException("KDMAPI initialize failed");
                if (layer == "delegate")
                {
                    identity = "IKdmApiNative.SendShort -> Marshal.GetDelegateForFunctionPointer delegate (v1), provider=" + native.ProviderPath;
                    IKdmApiNative iface = native;
                    send = delegate(uint m) { iface.SendShort(m); };
                }
                else
                {
                    identity = "DllImport SendDirectData direct, provider=" + native.ProviderPath;
                    send = delegate(uint m) { KdmSendDirectDataImport(m); };
                }
                cleanup = delegate { native.ResetStream(); native.TerminateStream(); native.Dispose(); };
            }
            else if (layer == "output")
            {
                // v1's IMidiOutput.Send(MidiEventView) as the engine calls it: 4096 views taken from a generated song.
                string fixture = Get(opt, "fixture", "");
                MidiSong song = DirectCompactMidiParser.Load(fixture, CancellationToken.None, null);
                MidiEventReader reader = song.GetEventReader();
                MidiEventView[] views = new MidiEventView[4096];
                for (int i = 0; i < views.Length; i++) views[i] = reader[i];
                Opened o = OpenOutput(kind, deviceName);
                IMidiOutput output = o.Output;
                identity = "IMidiOutput.Send(MidiEventView) " + o.Identity;
                int index = 0;
                send = delegate(uint unused) { output.Send(views[index++ & 4095]); };
                cleanup = delegate { try { output.Reset(); } catch (Exception) { } o.Disposable.Dispose(); };
            }
            else throw new InvalidOperationException("unsupported --output/--layer combination");

            TightLoop(0.3, send);
            for (int r = 0; r < repeats; r++) { rates.Add(TightLoop(seconds, send)); Thread.Sleep(1000); }
            StringBuilder line = new StringBuilder();
            line.Append("RAW program=v1 arch=").Append(arch).Append(" output=").Append(kind).Append(" layer=").Append(layer)
                .Append(" seconds=").Append(Inv(seconds, 1)).Append(" identity=[").Append(identity).Append("] meps=");
            for (int i = 0; i < rates.Count; i++) line.Append(i > 0 ? "," : "").Append(Inv(rates[i], 3));
            Console.WriteLine(line.ToString());
            CleanupWithWatchdog(Get(opt, "cleanup-log", "raw.cleanup.txt"), delegate { }, cleanup);
            return 0;
        }
    }
}
