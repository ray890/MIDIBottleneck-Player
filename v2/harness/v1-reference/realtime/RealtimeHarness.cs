// Real-time property-check driver for v1's UNCHANGED PlaybackEngine
// (Milestone 5 exit test; v2/docs/ENGINE_PROPERTY_CHECKS.md).
//
// Unlike the lock-step spike this program shadows nothing: the engine runs
// with the real System.Diagnostics.Stopwatch, v1's own HighResolutionWaiter,
// real EventWaitHandles and real threads, against a recording output that
// sends nowhere (no device is opened, no provider DLL is loaded). Scenarios
// use the committed grammar (v2/harness/ENGINE_SCENARIOS.md; ScenarioFile.cs
// and ScenarioBinder.cs of the spike are compiled in unchanged), but every
// time is REAL elapsed microseconds since the run began.
//
// Usage:
//   v1-realtime.exe list <scenarioFile>
//   v1-realtime.exe events <fixture.mid> <outFile>        (the song's event list)
//   v1-realtime.exe run <scenarioFile> <scenario> <fixture.mid> <outLog>
//
// Log format: v2/docs/ENGINE_PROPERTY_CHECKS.md, section "Log format". The v2
// program (v2/tools/engine_realtime_main.cpp) writes the same format.
//
// Compiled with C# 5 (the .NET Framework csc.exe).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    // ---- stand-ins for the spike types ScenarioBinder.cs binds to -----------

    internal sealed class SpikeCommand
    {
        internal readonly long Offset;
        internal readonly string Name;
        internal readonly Action<PlaybackEngine> Apply;
        internal readonly Func<PlaybackEngine, bool> StartsWorker;
        internal SpikeCommand(long offset, string name, bool startsWorker, Action<PlaybackEngine> apply)
            : this(offset, name, delegate(PlaybackEngine e) { return startsWorker; }, apply) { }
        internal SpikeCommand(long offset, string name, Func<PlaybackEngine, bool> startsWorker, Action<PlaybackEngine> apply)
        { Offset = offset; Name = name; StartsWorker = startsWorker; Apply = apply; }
    }

    internal sealed class SpikeScenario
    {
        internal string Name;
        internal Action<PlaybackEngine> Configure;
        internal List<SpikeCommand> Commands = new List<SpikeCommand>();
        internal bool StopAfterEnd = true;
        internal int SnapshotMode;
        internal TrackRoutingSettings Route;
    }

    // ScenarioBinder reads the song and output the "start" command plays
    // through these two members (the name is the spike's; this program has no
    // virtual time).
    internal static class RendezvousSpike
    {
        internal static MidiSong CurrentSong;
        internal static IMidiOutput CurrentOutput;
    }

    // ---- the recording no-device output -----------------------------------

    internal sealed class RealtimeRecorder
    {
        private readonly StringBuilder _log = new StringBuilder(1 << 20);
        private long _baseStamp = System.Diagnostics.Stopwatch.GetTimestamp();
        internal void Rebase() { _baseStamp = System.Diagnostics.Stopwatch.GetTimestamp(); }
        internal long Now()
        {
            long ticks = System.Diagnostics.Stopwatch.GetTimestamp() - _baseStamp;
            long frequency = System.Diagnostics.Stopwatch.Frequency;
            return (ticks / frequency) * 1000000L + ((ticks % frequency) * 1000000L) / frequency;
        }
        internal void Line(string text) { lock (_log) _log.Append(text).Append('\n'); }
        internal void Line(long t, string text) { lock (_log) _log.Append(t).Append(' ').Append(text).Append('\n'); }
        internal string Text() { lock (_log) return _log.ToString(); }
    }

    internal sealed class RealtimeRecordingOutput : IMidiOutput, IMidiOutputContext, IDisposable
    {
        private readonly RealtimeRecorder _recorder;
        public string SourceFile { get; set; }
        internal long Sends;
        internal RealtimeRecordingOutput(RealtimeRecorder recorder) { _recorder = recorder; }

        public void Send(MidiEventView midiEvent)
        {
            long t = _recorder.Now();
            MidiEventData data = midiEvent.Data;
            int length = data.Length;
            StringBuilder line = new StringBuilder(48);
            line.Append("SEND ").Append(midiEvent.Kind).Append(" src=").Append(midiEvent.EventIndex).Append(' ');
            if (length == 0) line.Append('-');
            for (int i = 0; i < length; i++) line.Append(data[i].ToString("X2"));
            _recorder.Line(t, line.ToString());
            Interlocked.Increment(ref Sends);
        }

        public void Panic() { _recorder.Line(_recorder.Now(), "PANIC"); }
        public void Reset() { _recorder.Line(_recorder.Now(), "RESET"); }
        public void Dispose() { }
    }

    internal static class RealtimeHarness
    {
        private static int Main(string[] args)
        {
            try
            {
                if (args.Length >= 2 && args[0] == "list") return List(args[1]);
                if (args.Length >= 3 && args[0] == "events") return DumpEvents(args[1], args[2]);
                if (args.Length >= 5 && args[0] == "run") return Run(args[1], args[2], args[3], args[4], args.Length >= 6 && args[5] == "warm");
            }
            catch (Exception ex)
            {
                Console.WriteLine("v1-realtime ERROR: " + ex.GetType().Name + ": " + ex.Message);
                return 2;
            }
            Console.WriteLine("usage: v1-realtime.exe list <scenarioFile>");
            Console.WriteLine("       v1-realtime.exe events <fixture.mid> <outFile>");
            Console.WriteLine("       v1-realtime.exe run <scenarioFile> <scenario> <fixture.mid> <outLog>");
            return 2;
        }

        private static List<ScenarioDefinition> LoadScenarios(string path)
        {
            List<ScenarioDefinition> definitions = ScenarioFile.Load(path);
            foreach (ScenarioDefinition d in definitions)
            {
                if (d.Route != null) throw new FormatException(path + " line " + d.Route.Line + ": 'route' is not supported by the real-time programs");
                ScenarioBinder.Build(d, 1000);
            }
            return definitions;
        }

        private static int List(string scenarioFile)
        {
            foreach (ScenarioDefinition d in LoadScenarios(scenarioFile)) Console.WriteLine(d.Name);
            return 0;
        }

        // events: one line per source event, "index time_us kind HEX", plus a header.
        private static int DumpEvents(string fixture, string outFile)
        {
            MidiSong song = DirectCompactMidiParser.Load(fixture, CancellationToken.None, null);
            MidiEventReader events = song.GetEventReader();
            StringBuilder text = new StringBuilder();
            text.Append("SONG count=").Append(events.Count).Append(" duration=").Append(song.DurationMicroseconds).Append('\n');
            for (int i = 0; i < events.Count; i++)
            {
                MidiEventView e = events[i];
                MidiEventData data = e.Data;
                text.Append(i).Append(' ').Append(e.IntendedMicroseconds).Append(' ').Append(e.Kind).Append(' ');
                if (data.Length == 0) text.Append('-');
                for (int b = 0; b < data.Length; b++) text.Append(data[b].ToString("X2"));
                text.Append('\n');
            }
            File.WriteAllText(outFile, text.ToString(), new UTF8Encoding(false));
            Console.WriteLine("events " + events.Count + " duration_us " + song.DurationMicroseconds);
            return 0;
        }

        private static string Snapshot(PlaybackEngine engine)
        {
            PlaybackSnapshot s = engine.GetSnapshot();
            return "state=" + s.State + " processed=" + s.ProcessedEvents + " dropped=" + s.DroppedEvents +
                " queue=" + s.QueueLength + " outstanding=" + s.OutstandingEvents + " maxQueue=" + s.MaximumQueueLength +
                " gateFiltered=" + s.GateFilteredEvents + " musicalRejectedAttacks=" + s.MusicalLimitRejectedAttacks +
                " musicalSuppressedReleases=" + s.MusicalLimitSuppressedReleases +
                " rateRejectedAttacks=" + s.OutputRateRejectedAttacks + " rateSuppressedReleases=" + s.OutputRateSuppressedReleases +
                " rateRejectedOther=" + s.OutputRateRejectedOtherMessages + " rateSafetySoftExcess=" + s.OutputRateSafetySoftExcess +
                " rateSafetyBypass=" + s.OutputRateSafetyBypassMessages +
                " virtualQueue=" + s.VirtualQueueLength + " virtualMaxQueue=" + s.VirtualMaximumQueueLength +
                " position=" + s.PlaybackMicroseconds + " intended=" + s.IntendedTimelineMicroseconds +
                " lastDispatched=" + s.LastDispatchedTimelineMicroseconds + " frontier=" + s.EffectiveSpeedFrontierMicroseconds +
                " lag=" + s.CurrentLagMicroseconds + " maxLag=" + s.MaximumLagMicroseconds;
        }

        private static long _lastCommandAt;

        private static int Run(string scenarioFile, string scenarioName, string fixture, string outLog, bool warm)
        {
            ScenarioDefinition definition = null;
            foreach (ScenarioDefinition d in LoadScenarios(scenarioFile))
                if (d.Name == scenarioName) definition = d;
            if (definition == null) throw new ArgumentException("no scenario '" + scenarioName + "' in " + scenarioFile);

            MidiSong song = DirectCompactMidiParser.Load(fixture, CancellationToken.None, null);
            long duration = Math.Max(1000, song.DurationMicroseconds);
            SpikeScenario scenario = ScenarioBinder.Build(definition, duration);
            if (warm) WarmUp(fixture, scenario);


            RealtimeRecorder recorder = new RealtimeRecorder();
            RealtimeRecordingOutput output = new RealtimeRecordingOutput(recorder);
            RendezvousSpike.CurrentSong = song;
            RendezvousSpike.CurrentOutput = output;
            string arch = IntPtr.Size == 8 ? "x64" : "x86";
            recorder.Line("RUN program=v1 arch=" + arch + " scenario=" + scenarioName + " fixture=" +
                Path.GetFileNameWithoutExtension(fixture) + " duration=" + song.DurationMicroseconds +
                " events=" + song.GetEventReader().Count + " timerFrequency=" + System.Diagnostics.Stopwatch.Frequency);
            recorder.Line("DESCRIBE " + definition.Description);
            foreach (ScenarioStep step in definition.Setup)
                recorder.Line("PLAN-SETUP " + step.Verb + (step.Args.Length == 0 ? "" : " " + String.Join(" ", step.Args)));
            foreach (SpikeCommand planned in scenario.Commands)
                recorder.Line("PLAN-AT " + planned.Offset + " " + planned.Name);

            // Watchdog: a run that outlives its cap by a wide margin writes what it has and exits (never a pass).
            long capUs = 40 * duration + 30000000L;
            Thread watchdog = new Thread(delegate()
            {
                while (true)
                {
                    Thread.Sleep(100);
                    if (recorder.Now() > capUs + 30000000L)
                    {
                        recorder.Line(recorder.Now(), "WATCHDOG run exceeded its cap by 30 s");
                        File.WriteAllText(outLog, recorder.Text(), new UTF8Encoding(false));
                        Environment.Exit(3);
                    }
                }
            });
            watchdog.IsBackground = true;
            watchdog.Start();

            PlaybackEngine engine = new PlaybackEngine();
            engine.PlaybackEnded += delegate { recorder.Line(recorder.Now(), "EVENT PlaybackEnded"); };
            engine.PlaybackFailed += delegate(object sender, PlaybackErrorEventArgs e)
            { recorder.Line(recorder.Now(), "EVENT PlaybackFailed " + e.Error.GetType().Name + ": " + Flat(e.Error.Message)); };
            engine.ChannelControlFailed += delegate(object sender, ChannelControlErrorEventArgs e)
            { recorder.Line(recorder.Now(), "EVENT ChannelControlFailed ch=" + e.Channel + " " + e.Attribute + " " + e.Error.GetType().Name + ": " + Flat(e.Error.Message)); };

            if (scenario.Configure != null) scenario.Configure(engine);
            recorder.Rebase();
            recorder.Line(recorder.Now(), "BEGIN");
            long lastSample = 0;
            int next = 0;
            bool endedInTime = true;
            _lastCommandAt = recorder.Now();

            while (true)
            {
                long now = recorder.Now();
                if (now - lastSample >= 20000)
                {
                    lastSample = now;
                    recorder.Line(now, "SNAP " + Snapshot(engine));
                }
                if (next < scenario.Commands.Count)
                {
                    SpikeCommand command = scenario.Commands[next];
                    if (now >= command.Offset)
                    {
                        next++;
                        Apply(recorder, engine, command);
                        recorder.Line(recorder.Now(), "SNAP " + Snapshot(engine));
                        lastSample = recorder.Now();
                        continue;
                    }
                    long remaining = command.Offset - now;
                    if (remaining > 20000) Thread.Sleep(1); else Thread.Yield();
                    continue;
                }
                // All commands done: wait for the worker to finish (or the run to stall).
                PlaybackState state = engine.State;
                bool alive = engine.HasLiveWorker;
                if (!alive && state != PlaybackState.Playing) break;
                if (state == PlaybackState.Paused && now - _lastCommandAt > 200000)
                {
                    // Left paused at the end of the script: end the run (as the spike's Stop(paused at end)).
                    Apply(recorder, engine, new SpikeCommand(now, "Stop(paused at end)", false,
                        delegate(PlaybackEngine e) { e.Stop(); }));
                    break;
                }
                if (now > capUs)
                {
                    recorder.Line(now, "TIMEOUT run exceeded the cap");
                    endedInTime = false;
                    break;
                }
                Thread.Sleep(1);
            }
            if (scenario.StopAfterEnd || !endedInTime)
                Apply(recorder, engine, new SpikeCommand(recorder.Now(), "Stop(after end)", false,
                    delegate(PlaybackEngine e) { e.Stop(); }));
            recorder.Line(recorder.Now(), "FINAL " + Snapshot(engine) + " sends=" + output.Sends);
            try { engine.Dispose(); }
            catch (Exception ex) { recorder.Line(recorder.Now(), "DISPOSE-ERROR " + ex.GetType().Name + ": " + Flat(ex.Message)); }
            recorder.Line(recorder.Now(), "END");
            File.WriteAllText(outLog, recorder.Text(), new UTF8Encoding(false));
            Console.WriteLine("v1 " + arch + " " + scenarioName + " " + Path.GetFileNameWithoutExtension(fixture) + " sends=" + output.Sends);
            return endedInTime ? 0 : 1;
        }

        // Optional JIT warm-up (argument "warm"): plays the 69 ms f0-all-message-types
        // fixture once in the scenario's first processing mode, with the scenario's
        // settings, on a throwaway engine and v1's own None output, so that the run
        // under test does not pay first-call JIT compilation inside the engine. The
        // measured run starts afterwards on a fresh engine; nothing here is logged.
        private static void WarmUp(string fixture, SpikeScenario scenario)
        {
            try
            {
                string warmFile = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(fixture)), "f0-all-message-types.mid");
                if (!File.Exists(warmFile)) return;
                ProcessingMode mode = ProcessingMode.Queue;
                foreach (SpikeCommand command in scenario.Commands)
                    if (command.Name.StartsWith("Start(", StringComparison.Ordinal))
                    {
                        string text = command.Name.Substring(6, command.Name.IndexOf(',') - 6);
                        mode = (ProcessingMode)Enum.Parse(typeof(ProcessingMode), text);
                        break;
                    }
                MidiSong warmSong = DirectCompactMidiParser.Load(warmFile, CancellationToken.None, null);
                NullMidiOutput none = new NullMidiOutput();
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    if (scenario.Configure != null) scenario.Configure(engine);
                    engine.Start(warmSong, none, mode, 0, false);
                    engine.Pause();
                    engine.Resume();
                    engine.Seek(10000);
                    System.Diagnostics.Stopwatch limit = System.Diagnostics.Stopwatch.StartNew();
                    while (engine.State == PlaybackState.Playing && limit.ElapsedMilliseconds < 800) Thread.Sleep(2);
                    engine.Stop();
                }
            }
            catch (Exception)
            {
                // Warm-up is best effort (an illegal option combination for the mode, for example).
            }
        }

        private static void Apply(RealtimeRecorder recorder, PlaybackEngine engine, SpikeCommand command)
        {
            recorder.Line(recorder.Now(), "SNAP " + Snapshot(engine));
            recorder.Line(recorder.Now(), "CMD " + command.Name);
            try { command.Apply(engine); }
            catch (Exception ex) { recorder.Line(recorder.Now(), "CMD-EXCEPTION " + command.Name + " " + ex.GetType().Name + ": " + Flat(ex.Message)); }
            _lastCommandAt = recorder.Now();
            recorder.Line(_lastCommandAt, "CMD-DONE");
        }

        private static string Flat(string text)
        {
            return text == null ? "" : text.Replace("\r", " ").Replace("\n", " ");
        }
    }
}
