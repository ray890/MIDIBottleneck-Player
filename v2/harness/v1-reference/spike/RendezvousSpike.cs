// Bounded spike driver: runs v1's UNCHANGED PlaybackEngine under the
// lock-step rendezvous scheduler over fixture songs and scripted Play/Stop
// scenarios, and writes the recorded output plus the scheduling trace.
//
// The scenarios are data: v2/harness/engine-scenarios.txt, specified in
// v2/harness/ENGINE_SCENARIOS.md (parsed by ScenarioFile.cs, bound to v1's
// public API by ScenarioBinder.cs).
//
// Usage: rendezvous-spike.exe [--scenarios <file>] [--only <name|prefix*>[,...]]
//                             <fixturesDir> <outDir> [fixture.mid ...]
//        rendezvous-spike.exe --list [--scenarios <file>]
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class SpikeCommand
    {
        internal readonly long Offset;           // virtual microseconds after scenario base
        internal readonly string Name;
        internal readonly Action<PlaybackEngine> Apply;
        // Evaluated just before Apply: does this command create a new worker
        // thread? (Start always does; a non-preserving Pause or Seek does when
        // the engine is active; a preserving one, Resume and Stop never do.)
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
        // 0: no snapshot lines (the Milestone 3 scenarios, so their recorded
        // files stay byte-identical). 1: the engine snapshot (SNAP line) after
        // every command once the worker is quiescent. 2: SNAP plus the SNAPX
        // line with the gate and queue counters.
        internal int SnapshotMode;
        internal TrackRoutingSettings Route;   // null = play the loaded song unrouted
    }

    internal static class RendezvousSpike
    {
        private const long BaseTime = 10000000;   // non-zero virtual start
        private const int HangMilliseconds = 20000;
        private static readonly FieldInfo WakeField =
            typeof(PlaybackEngine).GetField("_wake", BindingFlags.NonPublic | BindingFlags.Instance);

        private static int Main(string[] args)
        {
            if (args.Length >= 1 && args[0] == "--selftest")
            {
                StartWatchdog();
                return SpikeSelfTest.Run(args.Length >= 2 && args[1] == "--hang");
            }
            string scenarioPath = null;
            string only = null;
            bool list = false;
            List<string> positional = new List<string>();
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--scenarios" && i + 1 < args.Length) scenarioPath = args[++i];
                else if (args[i] == "--only" && i + 1 < args.Length) only = args[++i];
                else if (args[i] == "--list") list = true;
                else positional.Add(args[i]);
            }
            if (!list && positional.Count < 2)
            {
                Console.WriteLine("usage: rendezvous-spike.exe [--scenarios <file>] [--only <name|prefix*>[,...]] <fixturesDir> <outDir> [fixture ...]");
                Console.WriteLine("       rendezvous-spike.exe --list [--scenarios <file>]");
                Console.WriteLine("       rendezvous-spike.exe --selftest [--hang]");
                return 2;
            }
            StartWatchdog();
            List<ScenarioDefinition> definitions;
            try
            {
                if (scenarioPath == null) scenarioPath = ScenarioFile.FindDefault();
                definitions = ScenarioFile.Load(scenarioPath);
                ScenarioBinder.Validate(definitions);
            }
            catch (Exception ex)
            {
                Console.WriteLine("SCENARIO FILE ERROR: " + ex.Message);
                return 2;
            }
            if (only != null) definitions = FilterScenarios(definitions, only);
            if (list)
            {
                foreach (ScenarioDefinition definition in definitions) Console.WriteLine(definition.Name);
                Console.WriteLine(definitions.Count + " scenarios in " + scenarioPath);
                return 0;
            }
            if (definitions.Count == 0) { Console.WriteLine("no scenario matches --only"); return 2; }
            string fixtures = positional[0];
            string outDir = positional[1];
            Directory.CreateDirectory(outDir);
            List<string> names = new List<string>();
            for (int i = 2; i < positional.Count; i++) names.Add(positional[i]);
            if (names.Count == 0)
                names.AddRange(new string[] { "f1-v1-test-midi.mid", "tempo-changes.mid", "models-notes.mid",
                    "models-dense.mid", "many-tracks-ties.mid", "sysex-variants.mid" });

            int failures = 0;
            foreach (string name in names)
            {
                SetLabel("load " + name);
                MidiSong song = DirectCompactMidiParser.Load(Path.Combine(fixtures, name), CancellationToken.None, null);
                long duration = Math.Max(1000, song.DurationMicroseconds);
                foreach (ScenarioDefinition definition in definitions)
                {
                    SpikeScenario scenario = ScenarioBinder.Build(definition, duration);
                    string result;
                    if (!RunScenario(song, name, scenario, outDir, out result)) failures++;
                    Console.WriteLine(result);
                }
            }
            Console.WriteLine(failures == 0 ? "SPIKE: ALL SCENARIOS TERMINATED" : "SPIKE: " + failures + " SCENARIO(S) FAILED");
            return failures == 0 ? 0 : 1;
        }

        // --only: comma-separated scenario names; a trailing '*' matches a prefix.
        private static List<ScenarioDefinition> FilterScenarios(List<ScenarioDefinition> all, string only)
        {
            string[] patterns = only.Split(new char[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
            List<ScenarioDefinition> kept = new List<ScenarioDefinition>();
            foreach (ScenarioDefinition definition in all)
            {
                foreach (string pattern in patterns)
                {
                    bool match = pattern.EndsWith("*", StringComparison.Ordinal)
                        ? definition.Name.StartsWith(pattern.Substring(0, pattern.Length - 1), StringComparison.Ordinal)
                        : definition.Name == pattern;
                    if (match) { kept.Add(definition); break; }
                }
            }
            return kept;
        }

        // In-process watchdog. Each scheduler wait already has a 20 s real-time
        // limit and run-spike.ps1 kills a process after its own timeout; this
        // is the last resort inside the process. If one scenario (or the self
        // test) is still running after SPIKE_WATCHDOG_SECONDS real seconds
        // (default 300), it prints which one and exits with code 3 instead of
        // hanging. A blocked scenario is never reported as a pass.
        internal static volatile string CurrentLabel = "(starting)";
        internal static long LabelSinceTicks = System.Diagnostics.Stopwatch.GetTimestamp();

        internal static void SetLabel(string label)
        {
            CurrentLabel = label;
            Interlocked.Exchange(ref LabelSinceTicks, System.Diagnostics.Stopwatch.GetTimestamp());
        }

        private static void StartWatchdog()
        {
            double limit = 300;
            string text = Environment.GetEnvironmentVariable("SPIKE_WATCHDOG_SECONDS");
            double parsed;
            if (text != null && Double.TryParse(text, System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture, out parsed)) limit = parsed;
            Thread watchdog = new Thread(delegate()
            {
                while (true)
                {
                    Thread.Sleep(100);
                    double seconds = (System.Diagnostics.Stopwatch.GetTimestamp() - Interlocked.Read(ref LabelSinceTicks)) /
                        (double)System.Diagnostics.Stopwatch.Frequency;
                    if (seconds > limit)
                    {
                        Console.WriteLine("WATCHDOG: '" + CurrentLabel + "' ran longer than " + limit + " s real time; failing the run.");
                        Console.Out.Flush();
                        Environment.Exit(3);
                    }
                }
            });
            watchdog.IsBackground = true;
            watchdog.Name = "spike watchdog";
            watchdog.Start();
        }

        [ThreadStatic] internal static MidiSong CurrentSong;
        [ThreadStatic] internal static RecordingMidiOutput CurrentOutput;

        private static bool RunScenario(MidiSong song, string fixture, SpikeScenario scenario, string outDir, out string result)
        {
            System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
            SetLabel(scenario.Name + " " + fixture);
            VirtualTime.Set(BaseTime);
            Interlocked.Exchange(ref LockStepScheduler.SpinCount, 0);
            LockStepScheduler scheduler = new LockStepScheduler(HangMilliseconds);
            LockStepScheduler.Current = scheduler;
            StringBuilder log = new StringBuilder();
            RecordingMidiOutput output = new RecordingMidiOutput(log, Thread.CurrentThread, true, 0);
            CurrentSong = song;
            CurrentOutput = output;
            string error = null;
            if (scenario.Route != null)
            {
                // MainForm.ApplyTrackRoute: compile the route over the loaded
                // song and play the derived view. An invalid plan cannot be
                // applied (the dialog's Apply stays disabled), so it is logged
                // and the unrouted song plays.
                CompiledTrackRoute route = TrackRoutePlanner.Compile(song, scenario.Route);
                TrackRouteStatistics rs = route.Statistics;
                if (route.IsValid) CurrentSong = TrackRouteApplication.CreateRoutedSong(song, route);
                log.Append(VirtualTime.Now).Append(" H ROUTE valid=").Append(route.IsValid ? 1 : 0)
                    .Append(" explicit=").Append(route.RequiresExplicitApply ? 1 : 0)
                    .Append(" tier=").Append((int)route.HighestTier)
                    .Append(" errors=").Append(route.Errors.Count)
                    .Append(" lanes=").Append(rs.RoutedLaneCount)
                    .Append(" destinations=").Append(rs.DestinationCount)
                    .Append(" merged=").Append(rs.MergedLaneCount)
                    .Append(" retimbred=").Append(rs.RetimbredLaneCount)
                    .Append(" omitted=").Append(rs.OmittedLaneCount)
                    .Append(" omittedStarts=").Append(rs.OmittedNoteStartCount)
                    .Append(" generated=").Append(rs.GeneratedDestinationStateCount)
                    .Append(" included=").Append(rs.IncludedSourceEventCount)
                    .Append(" output=").Append(rs.RoutedOutputEventCount)
                    .Append(" applied=").Append(route.IsValid ? 1 : 0).Append('\n');
            }
            long cap = BaseTime + 40 * Math.Max(1000, song.DurationMicroseconds) + 600000000L;

            PlaybackEngine engine = new PlaybackEngine();
            EventWaitHandle wake = (EventWaitHandle)WakeField.GetValue(engine);
            engine.PlaybackEnded += delegate { lock (log) log.Append(VirtualTime.Now).Append(" W EVENT PlaybackEnded\n"); };
            engine.PlaybackFailed += delegate(object sender, PlaybackErrorEventArgs e)
            {
                lock (log) log.Append(VirtualTime.Now).Append(" W EVENT PlaybackFailed ").Append(e.Error.GetType().Name)
                    .Append(": ").Append(e.Error.Message).Append('\n');
            };
            engine.ChannelControlFailed += delegate(object sender, ChannelControlErrorEventArgs e)
            {
                lock (log) log.Append(VirtualTime.Now).Append(" W EVENT ChannelControlFailed ch=").Append(e.Channel)
                    .Append(' ').Append(e.Attribute).Append(' ').Append(e.Error.GetType().Name)
                    .Append(": ").Append(e.Error.Message).Append('\n');
            };
            try
            {
                if (scenario.Configure != null) scenario.Configure(engine);
                if (LockStepScheduler.FreeRunning) DriveFree(engine, scheduler, scenario, log);
                else Drive(engine, wake, scheduler, scenario, log, cap);
            }
            catch (Exception ex)
            {
                error = ex.GetType().Name + ": " + ex.Message;
                lock (log) log.Append("HARNESS-ERROR ").Append(error).Append('\n');
            }

            if (error == null)
            {
                PlaybackSnapshot snap = engine.GetSnapshot();
                log.Append("FINAL t=").Append(VirtualTime.Now).Append(" state=").Append(snap.State)
                    .Append(" processed=").Append(snap.ProcessedEvents).Append(" dropped=").Append(snap.DroppedEvents)
                    .Append(" maxQueue=").Append(snap.MaximumQueueLength).Append(" maxLagUs=").Append(snap.MaximumLagMicroseconds)
                    .Append(" position=").Append(snap.PlaybackMicroseconds).Append('\n');
                try { engine.Dispose(); }
                catch (Exception ex) { error = "Dispose: " + ex.GetType().Name + ": " + ex.Message; }
                log.Append("DISPOSED t=").Append(VirtualTime.Now).Append('\n');
            }
            if (scheduler.ProtocolError != null && error == null) error = "protocol: " + scheduler.ProtocolError;

            string stem = Path.Combine(outDir, scenario.Name + "__" + Path.GetFileNameWithoutExtension(fixture));
            byte[] outBytes = Encoding.UTF8.GetBytes(log.ToString());
            byte[] schedBytes = Encoding.UTF8.GetBytes(scheduler.Trace.ToString());
            File.WriteAllBytes(stem + ".out.txt", outBytes);
            File.WriteAllBytes(stem + ".sched.txt", schedBytes);
            result = string.Format("{0,-30} {1,-22} out={2} sched={3} sends={4} parks={5} go={6} wake={7} spins={8} yields={12} waits={13} ctl={14}/{15} virtEnd={9} realMs={10}{11}",
                scenario.Name, fixture, Sha(outBytes).Substring(0, 16), Sha(schedBytes).Substring(0, 16), output.Sends,
                scheduler.Parks, scheduler.GoResumes, scheduler.WakeResumes, Interlocked.Read(ref LockStepScheduler.SpinCount),
                VirtualTime.Now - BaseTime, real.ElapsedMilliseconds, error == null ? "" : " ERROR " + error, scheduler.SendYields,
                Interlocked.Read(ref scheduler.EventWaits), Interlocked.Read(ref scheduler.ControlWaits), Interlocked.Read(ref scheduler.ControlTimeouts));
            return error == null;
        }

        private static void Drive(PlaybackEngine engine, EventWaitHandle wake, LockStepScheduler scheduler,
            SpikeScenario scenario, StringBuilder log, long cap)
        {
            List<SpikeCommand> commands = new List<SpikeCommand>(scenario.Commands);
            bool finalStopIssued = !scenario.StopAfterEnd;
            int next = 0;
            FieldInfo generationField = typeof(PlaybackEngine).GetField("_wakeGeneration", BindingFlags.NonPublic | BindingFlags.Instance);
            FieldInfo stateField = typeof(PlaybackEngine).GetField("_state", BindingFlags.NonPublic | BindingFlags.Instance);
            scheduler.EngineWakeGeneration = delegate { return (int)generationField.GetValue(engine); };
            scheduler.EnginePlaying = delegate { return (PlaybackState)stateField.GetValue(engine) == PlaybackState.Playing; };
            long seenSeq = scheduler.ParkSeq;
            string pendingNote = null;
            bool snapshotPending = false;
            while (true)
            {
                long deadline, parkSeq;
                bool parked = scheduler.WaitQuiescent(seenSeq, out deadline, out parkSeq);
                if (pendingNote != null)
                {
                    // Logged only once the worker is quiescent again, so it
                    // cannot interleave nondeterministically with worker records.
                    lock (log) log.Append(pendingNote).Append('\n');
                    pendingNote = null;
                }
                if (snapshotPending)
                {
                    snapshotPending = false;
                    PlaybackSnapshot snap = engine.GetSnapshot();
                    lock (log)
                        log.Append(VirtualTime.Now).Append(" H SNAP state=").Append(snap.State)
                            .Append(" processed=").Append(snap.ProcessedEvents)
                            .Append(" dropped=").Append(snap.DroppedEvents)
                            .Append(" queue=").Append(snap.QueueLength)
                            .Append(" position=").Append(snap.PlaybackMicroseconds)
                            .Append(" intended=").Append(snap.IntendedTimelineMicroseconds)
                            .Append(" worker=").Append(parked ? (deadline == Int64.MaxValue ? "waiting-no-deadline" : "parked") : "none")
                            .Append('\n');
                    if (scenario.SnapshotMode == 2)
                        lock (log)
                            log.Append(VirtualTime.Now).Append(" H SNAPX gateFiltered=").Append(snap.GateFilteredEvents)
                                .Append(" musicalRejectedAttacks=").Append(snap.MusicalLimitRejectedAttacks)
                                .Append(" musicalSuppressedReleases=").Append(snap.MusicalLimitSuppressedReleases)
                                .Append(" rateRejectedAttacks=").Append(snap.OutputRateRejectedAttacks)
                                .Append(" rateSuppressedReleases=").Append(snap.OutputRateSuppressedReleases)
                                .Append(" rateRejectedOther=").Append(snap.OutputRateRejectedOtherMessages)
                                .Append(" rateSafetySoftExcess=").Append(snap.OutputRateSafetySoftExcess)
                                .Append(" rateSafetyBypass=").Append(snap.OutputRateSafetyBypassMessages)
                                .Append(" virtualQueue=").Append(snap.VirtualQueueLength)
                                .Append(" maxQueue=").Append(snap.MaximumQueueLength)
                                .Append(" lastDispatched=").Append(snap.LastDispatchedTimelineMicroseconds)
                                .Append(" frontier=").Append(snap.EffectiveSpeedFrontierMicroseconds)
                                .Append(" lag=").Append(snap.CurrentLagMicroseconds)
                                .Append(" maxLag=").Append(snap.MaximumLagMicroseconds)
                                .Append('\n');
                }
                SpikeCommand command = null;
                if (next < commands.Count && (!parked || BaseTime + commands[next].Offset <= deadline))
                    command = commands[next++];
                else if (next >= commands.Count && parked && deadline >= cap)
                {
                    // A worker waiting with no deadline is paused with nothing
                    // left to script: end the run at the current virtual time.
                    bool noDeadline = deadline == Int64.MaxValue;
                    command = new SpikeCommand(noDeadline ? VirtualTime.Now - BaseTime : deadline - BaseTime,
                        noDeadline ? "Stop(paused at end)" : "Stop(CAP)", false, delegate(PlaybackEngine e) { e.Stop(); });
                }
                else if (next >= commands.Count && !parked)
                {
                    if (finalStopIssued) return;
                    finalStopIssued = true;
                    command = new SpikeCommand(VirtualTime.Now - BaseTime, "Stop(after end)", false, delegate(PlaybackEngine e) { e.Stop(); });
                }

                if (command == null)
                {
                    // Nothing due before the worker's deadline: advance and release.
                    Interlocked.Exchange(ref scheduler.NextCommandTime, NextDue(commands, next, cap));
                    scheduler.ReleaseToDeadline(deadline);
                    seenSeq = parkSeq;
                    continue;
                }

                long at = BaseTime + command.Offset;
                if (VirtualTime.Now < at) VirtualTime.Set(at);
                int generation = scheduler.WorkerGeneration;
                long seqBefore = scheduler.ParkSeq;
                int wakeGenerationBefore = scheduler.EngineWakeGeneration();
                bool startsWorker = command.StartsWorker(engine);
                Interlocked.Exchange(ref scheduler.CommandSeenSeq, seqBefore);
                lock (log) log.Append(VirtualTime.Now).Append(" H CMD ").Append(command.Name).Append('\n');
                scheduler.Note("CMD " + command.Name + " t=" + VirtualTime.Now);
                // Published before Apply: a worker released by this command may yield
                // at a send boundary once the following command is due.
                Interlocked.Exchange(ref scheduler.NextCommandTime, NextDue(commands, next, cap));
                scheduler.BeginCommand();
                try { command.Apply(engine); }
                catch (Exception ex)
                {
                    pendingNote = "CMD-EXCEPTION " + command.Name + " " + ex.GetType().Name + ": " + ex.Message;
                }
                finally { scheduler.EndCommand(); }
                if (startsWorker && pendingNote == null) scheduler.WaitForWorkerGeneration(generation + 1);
                bool released = startsWorker || !parked || scheduler.CommandReleasedWorker(parkSeq, wakeGenerationBefore);
                // seqBefore - 1: the worker is still parked at the same rendezvous.
                // A control wait inside the command (preserving Pause) may already
                // have released and seen later parks; never wait for those again.
                seenSeq = released ? Math.Max(seqBefore, Interlocked.Read(ref scheduler.CommandSeenSeq)) : seqBefore - 1;
                snapshotPending = scenario.SnapshotMode != 0;
            }
        }

        // Negative control only: commands race the worker on the virtual clock.
        private static void DriveFree(PlaybackEngine engine, LockStepScheduler scheduler, SpikeScenario scenario, StringBuilder log)
        {
            foreach (SpikeCommand command in scenario.Commands)
            {
                long at = BaseTime + command.Offset;
                System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
                while (VirtualTime.Now < at && engine.HasLiveWorker && engine.State != PlaybackState.Paused && real.ElapsedMilliseconds < HangMilliseconds)
                    Thread.Yield();
                if (VirtualTime.Now < at) VirtualTime.Set(at);
                lock (log) log.Append(VirtualTime.Now).Append(" H CMD ").Append(command.Name).Append('\n');
                try { command.Apply(engine); }
                catch (Exception ex) { lock (log) log.Append("CMD-EXCEPTION ").Append(ex.GetType().Name).Append('\n'); }
            }
            System.Diagnostics.Stopwatch wait = System.Diagnostics.Stopwatch.StartNew();
            while (engine.HasLiveWorker && engine.State != PlaybackState.Paused && wait.ElapsedMilliseconds < HangMilliseconds) Thread.Sleep(1);
            Thread.Sleep(20);
            lock (log) log.Append(VirtualTime.Now).Append(" H CMD Stop(after end)\n");
            engine.Stop();
        }

        private static long NextDue(List<SpikeCommand> commands, int next, long cap)
        {
            return next < commands.Count ? BaseTime + commands[next].Offset : cap;
        }

        private static string Sha(byte[] bytes)
        {
            using (SHA256 sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "");
        }
    }
}
