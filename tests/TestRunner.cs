using System;
using System.Collections.Generic;
using System.IO;
using System.Diagnostics;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck.Tests
{
    internal static class TestRunner
    {
        private static int _passed;

        [STAThread]
        private static int Main(string[] arguments)
        {
            try
            {
                if (arguments.Length == 2 && arguments[0] == "--midi-device")
                {
                    TestSingleMidiDevice(UInt32.Parse(arguments[1]));
                    Console.WriteLine("PASS: single MIDI device " + arguments[1]);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--midi-sequence")
                {
                    TestMidiOutputSequence();
                    Console.WriteLine("PASS: repeated MIDI device sequence");
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--kdmapi-integration")
                {
                    TestKdmApiIntegration();
                    Console.WriteLine("PASS: KDMAPI integration");
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--analyze-drop")
                {
                    AnalyzeDropFile(arguments[1], Int64.Parse(arguments[2]));
                    return 0;
                }
                if ((arguments.Length == 6 || arguments.Length == 7) && arguments[0] == "--production-drop-trace")
                {
                    int capacity = arguments.Length == 7 ? Int32.Parse(arguments[6]) : PlaybackEngine.DefaultDropBufferCapacity;
                    RunProductionDropTrace(arguments[1], Int64.Parse(arguments[2]), Int64.Parse(arguments[3]), Int64.Parse(arguments[4]), arguments[5], capacity);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui")
                {
                    RenderMainWindow(arguments[1], false, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-bitrate")
                {
                    RenderMainWindow(arguments[1], true, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-min")
                {
                    RenderMainWindow(arguments[1], false, true);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis")
                {
                    RenderAnalysisWindow(arguments[1], arguments[2]);
                    return 0;
                }
                Run("tempo map, multiple tracks, running status, and SysEx", TestMidiParser);
                Run("FIFO queue accumulation", TestQueueSimulation);
                Run("queue mode playback-engine ordering regression", TestQueuePlaybackEngine);
                Run("independent slowdown and queue-limit combinations", TestIndependentPolicyCombinations);
                Run("2,000-event default queue limit", TestDefaultQueueLimit);
                Run("bounded Drop reference decisions", TestDropSimulation);
                Run("drop mode preserves sparse events in playback engine", TestSparseDropPlaybackEngine);
                Run("drop mode rejects only bounded-buffer overflow", TestDenseDropPlaybackEngine);
                Run("production Drop trace schema and overflow reasons", TestDropTraceOutput);
                Run("experimental Drop capacities", TestDropCapacities);
                Run("overflow policy behavior", TestOverflowPolicies);
                Run("clear buffer and catch up production behavior", TestClearBufferCatchUpPlaybackEngine);
                Run("zero processing time", TestZeroServiceTime);
                Run("MIDI bitrate byte-duration calculation", TestMidiBitrateCalculation);
                Run("MIDI bitrate production service intervals", TestMidiBitratePlaybackIntervals);
                Run("parsed MIDI Queue playback under MIDI bitrate", TestParsedMidiBitrateQueuePlayback);
                Run("service-mode values remain independent", TestServiceModeValuePreservation);
                Run("active seek clears queued work and stale dispatches", TestActiveSeek);
                Run("paused seek remains paused at a clean position", TestPausedSeek);
                Run("SMF SysEx fragments are framed for strict winmm drivers", TestSystemExclusiveAssembly);
                Run("KDMAPI short, SysEx, reset, and stream lifecycle", TestKdmApiOutput);
                Run("dense 200,000-event MIDI parsing", TestDenseMidiParser);
                Run("workload analysis and graph data", TestWorkloadAnalysis);
                Run("configured whole-file Analysis window", TestAnalysisWindowConstruction);
                Run("consistent simulated-lag formatting", TestLagFormatting);
                Run("Windows MIDI device enumeration", TestMidiDeviceEnumeration);
                if (Array.IndexOf(arguments, "--midi-integration") >= 0)
                    Run("MIDI SysEx output and repeated device switching", TestMidiOutputSwitching);
                Run("WinForms interface construction", TestInterfaceConstruction);
                Run("playback timeline rendering path", TestPlaybackTimelineRendering);
                Console.WriteLine("PASS: " + _passed + " tests");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("FAIL: " + ex.Message);
                return 1;
            }
        }

        private static void Run(string name, Action test)
        {
            test();
            _passed++;
            Console.WriteLine("  OK  " + name);
        }

        private static void RenderMainWindow(string outputPath, bool bitrateMode, bool minimumSize)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show();
                Application.DoEvents();
                if (minimumSize)
                {
                    form.Size = form.MinimumSize;
                    Application.DoEvents();
                }
                if (bitrateMode)
                {
                    List<Control> controls = new List<Control>();
                    CollectControls(form, controls);
                    ComboBox serviceMode = FindComboContaining(controls, "MIDI bitrate / byte transmission");
                    if (serviceMode == null) throw new Exception("MIDI bitrate mode was not found for UI rendering");
                    serviceMode.SelectedIndex = 1;
                    Application.DoEvents();
                }
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered UI: " + Path.GetFullPath(outputPath));
        }

        private static void RenderAnalysisWindow(string midiPath, string outputPath)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.QueueLengthLimitEnabled = true;
            configuration.QueueLengthLimit = 64;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show();
                Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered analysis: " + Path.GetFullPath(outputPath));
        }

        private static void TestMidiParser()
        {
            string path = Path.Combine(Path.GetTempPath(), "midi-bottleneck-parser-test-" + Guid.NewGuid().ToString("N") + ".mid");
            try
            {
                File.WriteAllBytes(path, BuildTestMidi());
                MidiSong song = MidiFileParser.Load(path);
                Equal(1, song.Format, "format");
                Equal(2, song.TrackCount, "track count");
                Equal(480, song.TicksPerQuarterNote, "PPQN");
                Equal(6, song.Events.Count, "dispatchable event count");
                Equal(0L, song.Events[0].IntendedMicroseconds, "program time");
                Equal(0L, song.Events[1].IntendedMicroseconds, "first note time");
                Equal(250000L, song.Events[2].IntendedMicroseconds, "running-status note time");
                Equal(500000L, song.Events[3].IntendedMicroseconds, "pre-change note-off time");
                Equal(1500000L, song.Events[4].IntendedMicroseconds, "SysEx time after tempo change");
                Equal(1500000L, song.Events[5].IntendedMicroseconds, "last note-off time");
                Equal(MidiEventKind.NoteOn, song.Events[2].Kind, "running-status kind");
                Equal((byte)0x90, song.Events[2].Data[0], "expanded running status");
                Equal((byte)62, song.Events[2].Data[1], "running-status note");
                Equal(MidiEventKind.SystemExclusive, song.Events[4].Kind, "SysEx kind");
                Equal((byte)0xF0, song.Events[4].Data[0], "SysEx prefix");
                Equal(1500000L, song.DurationMicroseconds, "duration");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestQueueSimulation()
        {
            long[] arrivals = new long[] { 0, 200, 400, 600, 800 };
            SimulationResult result = BottleneckSimulator.Run(arrivals, 500, ProcessingMode.Queue);
            Sequence(new long[] { 500, 1000, 1500, 2000, 2500 }, result.DispatchMicroseconds, "queue dispatches");
            Equal(0, result.DroppedEvents, "queue drops");
            Equal(3, result.MaximumQueueLength, "maximum queue");
        }

        private static void TestQueuePlaybackEngine()
        {
            MidiSong song = BuildSong(new long[] { 0, 1000, 2000, 30000 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 5000;
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "queue playback completion");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(4L, snapshot.ProcessedEvents, "queue processed");
                Equal(0L, snapshot.DroppedEvents, "queue dropped");
                Sequence(new long[] { 0, 1000, 2000, 30000 }, output.SentTimes(), "queue output order");
            }
        }

        private static void TestIndependentPolicyCombinations()
        {
            long[] arrivals = new long[] { 0, 0, 0 };
            SimulationResult slowdownUnlimited = BottleneckSimulator.Run(arrivals, 1000, true, false, 2, OverflowPolicy.DropNewest);
            Sequence(new long[] { 1000, 2000, 3000 }, slowdownUnlimited.DispatchMicroseconds, "slowdown on unlimited");
            Equal(0, slowdownUnlimited.DroppedEvents, "slowdown on unlimited drops");

            SimulationResult slowdownLimited = BottleneckSimulator.Run(arrivals, 1000, true, true, 2, OverflowPolicy.DropNewest);
            Sequence(new long[] { 1000, 2000 }, slowdownLimited.DispatchMicroseconds, "slowdown on limited");
            Equal(1, slowdownLimited.DroppedEvents, "slowdown on limited drops");

            SimulationResult immediateLimited = BottleneckSimulator.Run(arrivals, 1000, false, true, 1, OverflowPolicy.DropNewest);
            Sequence(new long[] { 0, 0, 0 }, immediateLimited.DispatchMicroseconds, "slowdown off limited");
            Equal(0, immediateLimited.DroppedEvents, "slowdown off limited drops");

            SimulationResult immediateUnlimited = BottleneckSimulator.Run(arrivals, 1000, false, false, 1, OverflowPolicy.DropNewest);
            Sequence(new long[] { 0, 0, 0 }, immediateUnlimited.DispatchMicroseconds, "slowdown off unlimited");
            Equal(0, immediateUnlimited.DroppedEvents, "slowdown off unlimited drops");

            MidiSong song = BuildSong(arrivals);
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = false;
                engine.ProcessingMicroseconds = 10000;
                engine.QueueLengthLimit = 1;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "slowdown-off production completion");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(3L, snapshot.ProcessedEvents, "slowdown-off production processed");
                Equal(0L, snapshot.DroppedEvents, "slowdown-off production drops");
            }
        }

        private static void TestDefaultQueueLimit()
        {
            Equal(2000, PlaybackEngine.DefaultQueueLengthLimit, "default queue limit constant");
            using (PlaybackEngine engine = new PlaybackEngine())
                Equal(2000, engine.QueueLengthLimit, "default queue limit engine");
        }

        private static void TestDropSimulation()
        {
            long[] arrivals = new long[70];
            for (int i = 0; i < 69; i++) arrivals[i] = 0;
            arrivals[69] = 40000;
            SimulationResult result = BottleneckSimulator.Run(arrivals, 500, ProcessingMode.Drop, 64);
            Equal(65, result.DispatchMicroseconds.Count, "bounded drop dispatch count");
            Equal(5, result.DroppedEvents, "bounded drop count");
            Equal(500L, result.DispatchMicroseconds[0], "first bounded dispatch");
            Equal(32000L, result.DispatchMicroseconds[63], "last buffered dispatch");
            Equal(40500L, result.DispatchMicroseconds[64], "post-drain dispatch");
        }

        private static void TestSparseDropPlaybackEngine()
        {
            MidiSong song = BuildSong(new long[] { 0, 50000, 100000 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 5000;
                engine.QueueLengthLimit = 64;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "sparse drop playback completion");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(3L, snapshot.ProcessedEvents, "sparse processed");
                Equal(0L, snapshot.DroppedEvents, "sparse dropped");
                Sequence(new long[] { 0, 50000, 100000 }, output.SentTimes(), "sparse output");
            }
        }

        private static void TestDenseDropPlaybackEngine()
        {
            long[] arrivals = new long[70];
            for (int i = 0; i < 69; i++) arrivals[i] = 0;
            arrivals[69] = 400000;
            MidiSong song = BuildSong(arrivals);
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 5000;
                engine.QueueLengthLimit = 64;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "dense drop playback completion");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(65L, snapshot.ProcessedEvents, "dense processed");
                Equal(5L, snapshot.DroppedEvents, "dense dropped");
                List<long> sent = output.SentTimes();
                Equal(65, sent.Count, "dense output count");
                Equal(0L, sent[0], "dense first source time");
                Equal(0L, sent[63], "dense last buffered source time");
                Equal(400000L, sent[64], "dense post-drain source time");
            }
        }

        private static void TestDropTraceOutput()
        {
            long[] arrivals = new long[66];
            MidiSong song = BuildSong(arrivals);
            FakeMidiOutput output = new FakeMidiOutput();
            DropTraceRecorder trace = new DropTraceRecorder(0, 0, 1000);
            string path = Path.Combine(Path.GetTempPath(), "midi-drop-trace-test-" + Guid.NewGuid().ToString("N") + ".csv");
            try
            {
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.ProcessingMicroseconds = 1000;
                    engine.QueueLengthLimit = 64;
                    engine.SetDropTrace(trace);
                    engine.Start(song, output, ProcessingMode.Drop);
                    WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "Drop trace playback completion");
                }
                trace.Save(path);
                string csv = File.ReadAllText(path);
                if (!csv.Contains("service_start_us,service_end_us")) throw new Exception("Drop trace lacks service interval columns");
                if (!csv.Contains("same_timestamp_cluster_size,consecutive_drops,buffer_occupancy,max_buffer_occupancy,busy_until_us")) throw new Exception("Drop trace lacks cluster/drop/occupancy columns");
                if (!csv.Contains("newest event dropped; buffer full (64 events outstanding)")) throw new Exception("Drop trace lacks overflow reason");
                Equal(66, trace.RowCount, "Drop trace row count");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestDropCapacities()
        {
            long[] arrivals = new long[300];
            int[] capacities = new int[] { 8, 16, 32, 64, 128, 256 };
            for (int i = 0; i < capacities.Length; i++)
            {
                SimulationResult result = BottleneckSimulator.Run(arrivals, 1000, ProcessingMode.Drop, capacities[i]);
                Equal(capacities[i], result.DispatchMicroseconds.Count, "capacity " + capacities[i] + " accepted");
                Equal(300 - capacities[i], result.DroppedEvents, "capacity " + capacities[i] + " dropped");
            }

            MidiSong song = BuildSong(new long[10]);
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.DropBufferCapacity = 8;
                engine.ProcessingMicroseconds = 1000;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "capacity-8 production completion");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(8L, snapshot.ProcessedEvents, "capacity-8 production accepted");
                Equal(2L, snapshot.DroppedEvents, "capacity-8 production dropped");
            }
        }

        private static void TestOverflowPolicies()
        {
            long[] arrivals = new long[] { 0, 0, 0, 10000 };
            SimulationResult newest = BottleneckSimulator.Run(arrivals, 1000, true, true, 2, OverflowPolicy.DropNewest);
            Sequence(new int[] { 0, 1, 3 }, newest.DispatchedEventIndices, "drop newest retained indices");
            Equal(1, newest.DroppedEvents, "drop newest count");

            SimulationResult oldest = BottleneckSimulator.Run(arrivals, 1000, true, true, 2, OverflowPolicy.DropOldest);
            Sequence(new int[] { 0, 2, 3 }, oldest.DispatchedEventIndices, "drop oldest retained indices");
            Equal(1, oldest.DroppedEvents, "drop oldest count");

            SimulationResult clear = BottleneckSimulator.Run(arrivals, 1000, true, true, 2, OverflowPolicy.ClearBufferAndCatchUp);
            Sequence(new int[] { 3 }, clear.DispatchedEventIndices, "clear/catch-up retained indices");
            Equal(3, clear.DroppedEvents, "clear/catch-up drop count");
            Equal(1, clear.BufferClearCount, "clear/catch-up count");

            MidiSong song = BuildSong(arrivals);
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 1000;
                engine.QueueLengthLimit = 2;
                engine.OverflowPolicy = OverflowPolicy.DropOldest;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "drop-oldest production completion");
                Sequence(new int[] { 60, 62, 63 }, output.SentNoteNumbers(), "drop-oldest production retained notes");
            }
        }

        private static void TestClearBufferCatchUpPlaybackEngine()
        {
            MidiSong song = BuildSong(new long[] { 0, 0, 0, 100000 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 10000;
                engine.QueueLengthLimit = 2;
                engine.OverflowPolicy = OverflowPolicy.ClearBufferAndCatchUp;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "clear/catch-up playback completion");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(1L, snapshot.ProcessedEvents, "clear/catch-up processed");
                Equal(3L, snapshot.DroppedEvents, "clear/catch-up dropped");
                Sequence(new long[] { 100000 }, output.SentTimes(), "clear/catch-up output");
                if (output.PanicCount < 1) throw new Exception("clear/catch-up did not terminate sounding notes");
            }
        }

        private static void TestMidiBitrateCalculation()
        {
            Equal(31250L, ServiceDurationCalculator.FivePinDinBitrate, "DIN preset");
            Equal(640L, ServiceDurationCalculator.CalculateBitrateMicroseconds(2, 31250), "DIN two-byte duration");
            Equal(960L, ServiceDurationCalculator.CalculateBitrateMicroseconds(3, 31250), "DIN three-byte duration");
            Equal(3200L, ServiceDurationCalculator.CalculateBitrateMicroseconds(10, 31250), "DIN ten-byte duration");
            Equal(32000L, ServiceDurationCalculator.CalculateBitrateMicroseconds(100, 31250), "DIN SysEx duration");
            Equal(600L, ServiceDurationCalculator.CalculateBitrateMicroseconds(3, 50000), "custom bitrate duration");
            Equal(261L, ServiceDurationCalculator.CalculateBitrateMicroseconds(1, 38400), "fractional duration rounds upward");
            Equal(0L, ServiceDurationCalculator.CalculateBitrateMicroseconds(0, 31250), "zero-byte duration");
        }

        private static void TestMidiBitratePlaybackIntervals()
        {
            MidiSong song = BuildSong(new long[] { 0, 0, 0 });
            song.Events[0].Data = new byte[] { 0xC0, 0x01 };
            song.Events[1].Data = new byte[] { 0x90, 60, 1 };
            song.Events[2].Kind = MidiEventKind.SystemExclusive;
            song.Events[2].Status = 0xF0;
            song.Events[2].Channel = -1;
            song.Events[2].Data = new byte[100];
            song.Events[2].Data[0] = 0xF0;
            song.Events[2].Data[99] = 0xF7;
            string path = Path.Combine(Path.GetTempPath(), "midi-bitrate-trace-" + Guid.NewGuid().ToString("N") + ".csv");
            try
            {
                DropTraceRecorder trace = new DropTraceRecorder(0, 0, 100);
                FakeMidiOutput output = new FakeMidiOutput();
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
                    engine.MidiBitrate = ServiceDurationCalculator.FivePinDinBitrate;
                    engine.SetDropTrace(trace);
                    engine.Start(song, output, ProcessingMode.Drop);
                    WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "bitrate playback completion");
                }
                trace.Save(path);
                string[] lines = File.ReadAllLines(path);
                Equal(4, lines.Length, "bitrate trace line count");
                string[] first = lines[1].Split(',');
                string[] second = lines[2].Split(',');
                string[] third = lines[3].Split(',');
                Equal(640L, Int64.Parse(first[4]) - Int64.Parse(first[3]), "production two-byte service");
                Equal(960L, Int64.Parse(second[4]) - Int64.Parse(second[3]), "production three-byte service");
                Equal(32000L, Int64.Parse(third[4]) - Int64.Parse(third[3]), "production SysEx service");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestServiceModeValuePreservation()
        {
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 979;
                engine.MidiBitrate = 50000;
                engine.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
                engine.ServiceDurationMode = ServiceDurationMode.ProcessingTime;
                Equal(979L, engine.ProcessingMicroseconds, "preserved processing time");
                Equal(50000L, engine.MidiBitrate, "preserved MIDI bitrate");
            }
        }

        private static void TestParsedMidiBitrateQueuePlayback()
        {
            string path = Path.Combine(Path.GetTempPath(), "midi-bitrate-queue-" + Guid.NewGuid().ToString("N") + ".mid");
            try
            {
                File.WriteAllBytes(path, BuildTestMidi());
                MidiSong song = MidiFileParser.Load(path);
                FakeMidiOutput output = new FakeMidiOutput();
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
                    engine.MidiBitrate = ServiceDurationCalculator.FivePinDinBitrate;
                    engine.Start(song, output, ProcessingMode.Queue);
                    WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 4000, "parsed bitrate Queue completion");
                    PlaybackSnapshot snapshot = engine.GetSnapshot();
                    Equal((long)song.Events.Count, snapshot.ProcessedEvents, "parsed bitrate Queue processed events");
                    Equal(0L, snapshot.DroppedEvents, "parsed bitrate Queue drops");
                    Equal(song.Events.Count, output.SentTimes().Count, "parsed bitrate Queue output count");
                }
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestZeroServiceTime()
        {
            long[] arrivals = new long[] { 0, 0, 10 };
            SimulationResult queue = BottleneckSimulator.Run(arrivals, 0, ProcessingMode.Queue);
            SimulationResult drop = BottleneckSimulator.Run(arrivals, 0, ProcessingMode.Drop);
            Sequence(arrivals, queue.DispatchMicroseconds, "zero-time queue");
            Sequence(arrivals, drop.DispatchMicroseconds, "zero-time drop");
            Equal(0, drop.DroppedEvents, "zero-time drops");
        }

        private static void TestActiveSeek()
        {
            MidiSong song = BuildSong(new long[] { 0, 10000, 20000, 100000, 110000 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 20000;
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().QueueLength >= 1; }, 1000, "queue buildup before seek");
                engine.Seek(100000);
                PlaybackSnapshot afterSeek = engine.GetSnapshot();
                Equal(0L, afterSeek.CurrentLagMicroseconds, "seek current lag");
                Equal(0L, afterSeek.MaximumLagMicroseconds, "seek maximum lag reset");
                Equal(0L, afterSeek.DroppedEvents, "seek dropped reset");
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "post-seek playback completion");
                Sequence(new long[] { 100000, 110000 }, output.SentTimes(), "post-seek output");
                if (output.ResetCount < 1) throw new Exception("seek did not reset the MIDI output");
            }
        }

        private static void TestPausedSeek()
        {
            MidiSong song = BuildSong(new long[] { 0, 50000, 100000, 150000 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 1000;
                engine.Start(song, output, ProcessingMode.Queue);
                engine.Pause();
                engine.Seek(100000);
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(PlaybackState.Paused, snapshot.State, "paused seek state");
                Near(100000L, snapshot.PlaybackMicroseconds, 2L, "paused seek position");
                Equal(0L, snapshot.QueueLength, "paused seek queue");
                Equal(0L, snapshot.CurrentLagMicroseconds, "paused seek lag");
                engine.Resume();
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "paused-seek resume completion");
                Sequence(new long[] { 100000, 150000 }, output.SentTimes(), "paused-seek output");
            }
        }

        private static void TestSystemExclusiveAssembly()
        {
            SystemExclusiveAssembler assembler = new SystemExclusiveAssembler();
            byte[] first = assembler.Accept(SysExEvent(0xF0, new byte[] { 0xF0, 0x7D, 0x01 }));
            if (first != null) throw new Exception("incomplete SysEx was emitted");
            byte[] complete = assembler.Accept(SysExEvent(0xF7, new byte[] { 0x02, 0xF7 }));
            ByteSequence(new byte[] { 0xF0, 0x7D, 0x01, 0x02, 0xF7 }, complete, "assembled SysEx");

            byte[] direct = assembler.Accept(SysExEvent(0xF0, new byte[] { 0xF0, 0x7E, 0x7F, 0xF7 }));
            ByteSequence(new byte[] { 0xF0, 0x7E, 0x7F, 0xF7 }, direct, "complete SysEx");
            byte[] unsafeEscape = assembler.Accept(SysExEvent(0xF7, new byte[] { 0x01, 0x02 }));
            if (unsafeEscape != null) throw new Exception("unframed F7 escape was emitted as SysEx");
        }

        private static void TestKdmApiOutput()
        {
            FakeKdmApiNative native = new FakeKdmApiNative();
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(native))
            {
                output.Open();
                MidiEvent shortEvent = BuildSong(new long[] { 0 }).Events[0];
                shortEvent.Data = new byte[] { 0x90, 0x3C, 0x7F };
                output.Send(shortEvent);
                output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7D, 0x01, 0xF7 }));
                Equal(1, native.InitializeCount, "KDMAPI initialize count");
                Equal((uint)0x007F3C90, native.ShortMessages[0], "KDMAPI packed short message");
                Equal(1, native.PrepareLongCount, "KDMAPI prepare long count");
                Equal(1, native.SendLongCount, "KDMAPI send long count");
                output.Reset();
                Equal(1, native.UnprepareLongCount, "KDMAPI unprepare after reset");
            }
            Equal(1, native.TerminateCount, "KDMAPI terminate count");
            if (native.ResetCount < 2) throw new Exception("KDMAPI stream was not reset before disposal");
        }

        private static void TestKdmApiIntegration()
        {
            using (KdmApiMidiOutput output = new KdmApiMidiOutput())
            {
                Console.WriteLine("      Loading and initializing OmniMIDI KDMAPI");
                output.Open();
                Console.WriteLine("      Sending framed System Exclusive packet");
                output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7D, 0x00, 0xF7 }));
                MidiEvent allNotesOff = BuildSong(new long[] { 0 }).Events[0];
                allNotesOff.Kind = MidiEventKind.ControlChange;
                allNotesOff.Data = new byte[] { 0xB0, 123, 0 };
                Console.WriteLine("      Sending short message");
                output.Send(allNotesOff);
                Console.WriteLine("      Resetting KDMAPI stream");
                output.Reset();
                Console.WriteLine("      KDMAPI stream reset complete");
            }
            Console.WriteLine("      KDMAPI stream terminated");
        }

        private static void TestDenseMidiParser()
        {
            const int eventCount = 200000;
            string path = Path.Combine(Path.GetTempPath(), "midi-bottleneck-dense-test-" + Guid.NewGuid().ToString("N") + ".mid");
            try
            {
                List<byte> track = new List<byte>(eventCount * 3 + 8);
                Add(track, 0x00, 0x90, 0x3C, 0x01);
                for (int i = 1; i < eventCount; i++)
                    Add(track, 0x00, 0x3C, 0x01);
                Add(track, 0x00, 0xFF, 0x2F, 0x00);

                List<byte> bytes = new List<byte>(track.Count + 22);
                AddAscii(bytes, "MThd");
                AddUInt32(bytes, 6);
                AddUInt16(bytes, 0);
                AddUInt16(bytes, 1);
                AddUInt16(bytes, 480);
                AddTrack(bytes, track);
                File.WriteAllBytes(path, bytes.ToArray());

                MidiSong song = MidiFileParser.Load(path);
                Equal(eventCount, song.Events.Count, "dense event count");
                Equal(0L, song.Events[eventCount - 1].IntendedMicroseconds, "dense last timestamp");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestWorkloadAnalysis()
        {
            MidiSong song = BuildSong(new long[] { 0, 0, 100000, 100000, 100000 });
            song.Events[0].Kind = MidiEventKind.ProgramChange;
            song.Events[0].Data = new byte[] { 0xC0, 0x01 };
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.ProcessingMicroseconds = 100000;
            configuration.QueueLengthLimitEnabled = true;
            configuration.QueueLengthLimit = 2;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, configuration);
            Equal(5L, analysis.TotalEvents, "analysis events");
            Equal(14L, analysis.TotalBytes, "analysis bytes");
            Equal(2L, analysis.UniqueTimestamps, "analysis timestamps");
            Equal(3, analysis.LargestTimestampCluster, "analysis largest cluster");
            Equal(5L, analysis.EventsInClustersAtLeast2, "analysis clustered events");
            Equal(0L, analysis.EventsInClustersAtLeast10, "analysis large clustered events");
            Near(50L, (long)Math.Round(analysis.AverageEventsPerSecond), 0, "analysis average rate");
            Near(30L, (long)Math.Round(analysis.PeakEventsPerSecond), 0, "analysis peak rate");
            Near(90L, (long)Math.Round(analysis.PeakBytesPerSecond), 0, "analysis peak byte rate");
            Equal(2, analysis.MessageTypes.Count, "analysis message types");
            Equal(2, analysis.PredictedMaximumOccupancy, "analysis predicted peak occupancy");
            Equal(2L, analysis.PredictedDroppedEvents, "analysis predicted drops");
            Equal(10L, (long)Math.Round(analysis.EventServiceCapacityPerSecond), "analysis service capacity line");
            Equal(200000L, analysis.Buckets[0].ServiceDemandMicroseconds, "analysis first-bucket service demand");

            AnalysisConfiguration bitrateConfiguration = DefaultAnalysisConfiguration();
            bitrateConfiguration.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
            bitrateConfiguration.MidiBitrate = 31250;
            WorkloadAnalysis bitrateAnalysis = WorkloadAnalyzer.Analyze(song, 100000, bitrateConfiguration);
            Equal(3125L, (long)Math.Round(bitrateAnalysis.ByteServiceCapacityPerSecond), "analysis byte service capacity");
            Equal(4480L, bitrateAnalysis.Buckets[0].ServiceDemandMicroseconds + bitrateAnalysis.Buckets[1].ServiceDemandMicroseconds,
                "analysis byte-proportional service demand");
        }

        private static void TestAnalysisWindowConstruction()
        {
            MidiSong song = BuildSong(new long[] { 0, 0, 100000, 200000, 200000 });
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.QueueLengthLimitEnabled = true;
            configuration.QueueLengthLimit = 2;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                IntPtr handle = form.Handle;
                if (handle == IntPtr.Zero) throw new Exception("Analysis window handle is zero");
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                WorkloadGraph graph = FindControl<WorkloadGraph>(controls);
                TextBox summary = FindControl<TextBox>(controls);
                if (graph == null || summary == null) throw new Exception("Analysis controls were not constructed");
                if (!summary.Text.Contains("Scope: whole file")) throw new Exception("Analysis scope is not identified");
                if (!summary.Text.Contains("Aggregation: fixed 100 ms buckets")) throw new Exception("Analysis aggregation interval is not identified");
                if (!summary.Text.Contains("Predicted drops:")) throw new Exception("Analysis prediction summary is absent");
                if (!summary.Text.Contains("Maximum rate:")) throw new Exception("Analysis maximum rate is absent");
                AnalysisConfiguration fasterConfiguration = DefaultAnalysisConfiguration();
                fasterConfiguration.ProcessingMicroseconds = 10;
                WorkloadAnalysis faster = WorkloadAnalyzer.Analyze(song, 100000, fasterConfiguration);
                form.UpdateAnalysis(faster);
                if (!summary.Text.Contains("100,000.0 events/sec")) throw new Exception("Analysis did not refresh its maximum rate");
                Equal(faster, graph.Analysis, "Analysis graph live replacement");
                graph.Size = new Size(700, 520);
                using (Bitmap bitmap = new Bitmap(700, 520))
                {
                    graph.DrawToBitmap(bitmap, graph.ClientRectangle);
                }
            }
        }

        private static void TestLagFormatting()
        {
            Equal("0.000 ms", MainForm.FormatLagMilliseconds(0), "zero lag format");
            Equal("43.137 ms", MainForm.FormatLagMilliseconds(43137), "nonzero lag format");
        }

        private static AnalysisConfiguration DefaultAnalysisConfiguration()
        {
            AnalysisConfiguration configuration = new AnalysisConfiguration();
            configuration.SimulateSlowdown = true;
            configuration.ServiceDurationMode = ServiceDurationMode.ProcessingTime;
            configuration.ProcessingMicroseconds = 100;
            configuration.MidiBitrate = ServiceDurationCalculator.FivePinDinBitrate;
            configuration.QueueLengthLimit = PlaybackEngine.DefaultQueueLengthLimit;
            configuration.OverflowPolicy = OverflowPolicy.DropNewest;
            return configuration;
        }

        private static void TestMidiDeviceEnumeration()
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            if (devices == null) throw new Exception("device enumeration returned null");
            Console.WriteLine("      Found " + devices.Count + " Windows MIDI output device(s)");
            for (int i = 0; i < devices.Count; i++)
                Console.WriteLine("        " + devices[i].DeviceId + ": " + devices[i].Name);
        }

        private static void TestMidiOutputSwitching()
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            if (devices.Count == 0) return;
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < devices.Count; i++)
                    RunMidiProbeProcess("--midi-device " + devices[i].DeviceId, devices[i].Name, 15000);
            RunMidiProbeProcess("--midi-sequence", "repeated switching sequence", 30000);
        }

        private static void TestMidiOutputSequence()
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            MidiEvent harmlessSysEx = SysExEvent(0xF0, new byte[] { 0xF0, 0x7D, 0x00, 0xF7 });
            using (WindowsMidiOutput output = new WindowsMidiOutput())
            {
                for (int pass = 0; pass < 2; pass++)
                {
                    for (int i = 0; i < devices.Count; i++)
                    {
                        Console.WriteLine("      Pass " + (pass + 1) + ", opening " + devices[i].Name);
                        output.Open(devices[i].DeviceId);
                        Console.WriteLine("        opened; sending framed SysEx");
                        output.Send(harmlessSysEx);
                        Console.WriteLine("        sent; resetting output");
                        output.Reset();
                        Console.WriteLine("        reset complete");
                    }
                }
            }
        }

        private static void RunMidiProbeProcess(string arguments, string description, int timeoutMilliseconds)
        {
            ProcessStartInfo start = new ProcessStartInfo();
            start.FileName = Process.GetCurrentProcess().MainModule.FileName;
            start.Arguments = arguments;
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.RedirectStandardOutput = true;
            start.RedirectStandardError = true;
            using (Process process = Process.Start(start))
            {
                if (!process.WaitForExit(timeoutMilliseconds))
                {
                    process.Kill();
                    process.WaitForExit();
                    throw new Exception("MIDI probe timed out for " + description);
                }
                string output = process.StandardOutput.ReadToEnd();
                string error = process.StandardError.ReadToEnd();
                Console.Write(output);
                if (process.ExitCode != 0)
                    throw new Exception("MIDI probe failed for " + description + ": " + error);
            }
        }

        private static void TestSingleMidiDevice(uint deviceId)
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            MidiOutputDeviceInfo device = null;
            for (int i = 0; i < devices.Count; i++)
                if (devices[i].DeviceId == deviceId) device = devices[i];
            if (device == null) throw new Exception("MIDI device " + deviceId + " was not found");
            Console.WriteLine("Opening " + device.Name);
            using (WindowsMidiOutput output = new WindowsMidiOutput())
            {
                output.Open(device.DeviceId);
                Console.WriteLine("Opened; sending framed SysEx");
                output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7D, 0x00, 0xF7 }));
                Console.WriteLine("Sent; resetting");
                output.Reset();
                Console.WriteLine("Reset complete");
            }
            Console.WriteLine("Close complete");
        }

        private static void AnalyzeDropFile(string path, long serviceMicroseconds)
        {
            MidiSong song = MidiFileParser.Load(path);
            Console.WriteLine("File: " + song.FilePath);
            Console.WriteLine("Events: " + song.Events.Count + ", duration us: " + song.DurationMicroseconds + ", service us: " + serviceMicroseconds);
            long[] centers = new long[] { 30000000, 77000000, 96000000 };
            for (int centerIndex = -1; centerIndex < centers.Length; centerIndex++)
            {
                long from = centerIndex < 0 ? 0 : centers[centerIndex] - 1000000;
                long to = centerIndex < 0 ? Int64.MaxValue : centers[centerIndex] + 1000000;
                long busyUntil = -1;
                int accepted = 0;
                int dropped = 0;
                int consecutive = 0;
                int maxConsecutive = 0;
                int maxCluster = 0;
                int uniqueTimestamps = 0;
                long lastTimestamp = -1;
                int cluster = 0;
                for (int i = 0; i < song.Events.Count; i++)
                {
                    long arrival = song.Events[i].IntendedMicroseconds;
                    if (arrival != lastTimestamp)
                    {
                        if (cluster > maxCluster) maxCluster = cluster;
                        cluster = 1;
                        uniqueTimestamps++;
                        lastTimestamp = arrival;
                    }
                    else cluster++;

                    bool keep = serviceMicroseconds == 0 || arrival >= busyUntil;
                    if (keep)
                    {
                        busyUntil = arrival + serviceMicroseconds;
                        consecutive = 0;
                        if (arrival >= from && arrival <= to) accepted++;
                    }
                    else
                    {
                        consecutive++;
                        if (arrival >= from && arrival <= to)
                        {
                            dropped++;
                            if (consecutive > maxConsecutive) maxConsecutive = consecutive;
                        }
                    }
                }
                if (cluster > maxCluster) maxCluster = cluster;
                string name = centerIndex < 0 ? "whole file" : ((centers[centerIndex] / 1000000) + "s ±1s");
                double rate = accepted + dropped == 0 ? 0 : (100.0 * accepted / (accepted + dropped));
                Console.WriteLine(name + ": accepted=" + accepted + ", dropped=" + dropped + ", accepted%=" + rate.ToString("F2") + ", max consecutive drops=" + maxConsecutive + ", unique timestamps=" + uniqueTimestamps + ", max cluster=" + maxCluster);
            }
            Console.WriteLine("Alternative whole-file admission ratios:");
            Console.WriteLine("  atomic same-timestamp batches: " + AnalyzeAtomicBatches(song, serviceMicroseconds).ToString("F2") + "%");
            int[] capacities = new int[] { 8, 32, 64, 128 };
            for (int i = 0; i < capacities.Length; i++)
            {
                Console.WriteLine("  finite FIFO capacity " + capacities[i] + ": " + AnalyzeFiniteQueue(song, serviceMicroseconds, capacities[i]).ToString("F2") + "%");
                Console.WriteLine("  token bucket capacity " + capacities[i] + ": " + AnalyzeTokenBucket(song, serviceMicroseconds, capacities[i]).ToString("F2") + "%");
            }
        }

        private static void RunProductionDropTrace(string path, long serviceMicroseconds, long fromMicroseconds, long toMicroseconds, string outputPath, int capacity)
        {
            MidiSong song = MidiFileParser.Load(path);
            DropTraceRecorder trace = new DropTraceRecorder(fromMicroseconds, toMicroseconds, 50000);
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = serviceMicroseconds;
                engine.DropBufferCapacity = capacity;
                engine.SetDropTrace(trace);
                engine.Start(song, output, ProcessingMode.Drop, fromMicroseconds);
                long stopAt = Math.Min(song.DurationMicroseconds, toMicroseconds + 200000);
                WaitFor(delegate
                {
                    PlaybackSnapshot snapshot = engine.GetSnapshot();
                    return snapshot.State == PlaybackState.Completed || snapshot.IntendedTimelineMicroseconds >= stopAt;
                }, (int)Math.Max(5000, ((stopAt - fromMicroseconds) / 1000) + 5000), "production drop trace window");
                engine.Stop();
            }
            trace.Save(outputPath);
            Console.WriteLine("Production Drop trace rows: " + trace.RowCount);
            Console.WriteLine("Experimental Drop capacity: " + capacity);
            Console.WriteLine("Trace: " + Path.GetFullPath(outputPath));
        }

        private static double AnalyzeAtomicBatches(MidiSong song, long serviceMicroseconds)
        {
            long busyUntil = -1;
            int accepted = 0;
            int index = 0;
            while (index < song.Events.Count)
            {
                int end = index + 1;
                long timestamp = song.Events[index].IntendedMicroseconds;
                while (end < song.Events.Count && song.Events[end].IntendedMicroseconds == timestamp) end++;
                int count = end - index;
                if (timestamp >= busyUntil)
                {
                    accepted += count;
                    busyUntil = timestamp + count * serviceMicroseconds;
                }
                index = end;
            }
            return 100.0 * accepted / song.Events.Count;
        }

        private static double AnalyzeFiniteQueue(MidiSong song, long serviceMicroseconds, int capacity)
        {
            Queue<long> completions = new Queue<long>();
            long lastCompletion = 0;
            int accepted = 0;
            for (int i = 0; i < song.Events.Count; i++)
            {
                long arrival = song.Events[i].IntendedMicroseconds;
                while (completions.Count > 0 && completions.Peek() <= arrival) completions.Dequeue();
                if (completions.Count >= capacity) continue;
                lastCompletion = Math.Max(arrival, lastCompletion) + serviceMicroseconds;
                completions.Enqueue(lastCompletion);
                accepted++;
            }
            return 100.0 * accepted / song.Events.Count;
        }

        private static double AnalyzeTokenBucket(MidiSong song, long serviceMicroseconds, int capacity)
        {
            decimal tokens = capacity;
            long previous = song.Events.Count == 0 ? 0 : song.Events[0].IntendedMicroseconds;
            int accepted = 0;
            for (int i = 0; i < song.Events.Count; i++)
            {
                long arrival = song.Events[i].IntendedMicroseconds;
                if (serviceMicroseconds > 0)
                    tokens = Math.Min(capacity, tokens + ((decimal)(arrival - previous) / serviceMicroseconds));
                previous = arrival;
                if (tokens >= 1m)
                {
                    tokens -= 1m;
                    accepted++;
                }
            }
            return 100.0 * accepted / song.Events.Count;
        }

        private static void TestInterfaceConstruction()
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                IntPtr handle = form.Handle;
                if (!form.IsHandleCreated) throw new Exception("main window handle was not created");
                if (handle == IntPtr.Zero) throw new Exception("main window handle is zero");
                if (form.Controls.Count == 0) throw new Exception("main window has no controls");
                Equal("MIDI Event Bottleneck Simulator", form.Text, "window title");
                StatisticsView statistics = FindStatisticsView(form);
                if (statistics == null) throw new Exception("statistics view was not found");
                Equal(true, statistics.UsesDoubleBuffer, "statistics double buffering");
                Rectangle bounds = statistics.Bounds;
                int layoutEvents = 0;
                statistics.Layout += delegate { layoutEvents++; };
                string[] values = new string[10];
                for (int i = 0; i < values.Length; i++) values[i] = "9 µs";
                Equal(true, statistics.SetValues(values), "statistics first update");
                Equal(false, statistics.SetValues(values), "statistics unchanged update");
                for (int update = 0; update < 1000; update++)
                {
                    for (int i = 0; i < values.Length; i++) values[i] = (update + i).ToString() + ".000 ms";
                    statistics.SetValues(values);
                }
                Equal(bounds, statistics.Bounds, "statistics bounds after rapid updates");
                using (Bitmap bitmap = new Bitmap(Math.Max(1, statistics.Width), Math.Max(1, statistics.Height)))
                {
                    Stopwatch paintTimer = Stopwatch.StartNew();
                    for (int frame = 0; frame < 120; frame++)
                    {
                        for (int i = 0; i < values.Length; i++) values[i] = (frame * 1000 + i).ToString("N0");
                        statistics.SetValues(values);
                        statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
                    }
                    paintTimer.Stop();
                    if (paintTimer.ElapsedMilliseconds > 3000)
                        throw new Exception("statistics rendering benchmark took " + paintTimer.ElapsedMilliseconds + " ms for 120 frames");
                    Console.WriteLine("      Statistics render benchmark: 120 frames in " + paintTimer.ElapsedMilliseconds + " ms");
                }
                Equal(0, layoutEvents, "statistics layout events during updates");

                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                CheckBox slowdown = FindCheckBox(controls, "Simulate slowdown");
                CheckBox queueLimit = FindCheckBox(controls, "Queue length limit:");
                CheckBox kdmApi = FindCheckBox(controls, "KDMAPI");
                ComboBox serviceMode = FindComboContaining(controls, "MIDI bitrate / byte transmission");
                ComboBox overflow = FindComboContaining(controls, "Clear buffer and jump to realtime");
                ComboBox midiOutput = FindMidiOutputCombo(controls);
                Button dinPreset = FindButton(controls, "31,250 DIN");
                NumericUpDown queueLimitValue = FindNumericWithValue(controls, 2000m);
                if (slowdown == null || queueLimit == null || kdmApi == null || serviceMode == null || overflow == null || dinPreset == null || queueLimitValue == null)
                    throw new Exception("independent processing policy controls were not found");
                Equal(new Size(740, 670), form.MinimumSize, "compact minimum window size");
                if (FindButton(controls, "Play") == null) throw new Exception("merged Play button was not found");
                if (FindButton(controls, "Pause") != null) throw new Exception("separate Pause button still exists");
                if (FindButton(controls, "−10 sec") != null || FindButton(controls, "+10 sec") != null)
                    throw new Exception("redundant ten-second seek buttons still exist");
                if (FindButton(controls, "Reset statistics") != null) throw new Exception("redundant statistics reset button still exists");
                if (!ContainsLabel(controls, "Rate model:")) throw new Exception("rate model label was not found");
                if (midiOutput != null)
                {
                    kdmApi.Checked = true;
                    Equal(false, midiOutput.Enabled, "KDMAPI disables Windows output selection");
                    kdmApi.Checked = false;
                    Equal(true, midiOutput.Enabled, "Windows output selection restored");
                }
                Equal(true, slowdown.Checked, "simulate slowdown default");
                Equal(false, queueLimit.Checked, "queue limit default disabled");
                Equal(false, queueLimitValue.Enabled, "queue limit value disabled while unlimited");
                Equal(2000m, queueLimitValue.Value, "queue limit UI default");
                queueLimit.Checked = true;
                Equal(true, queueLimitValue.Enabled, "queue limit value enabled");
                queueLimitValue.Value = 1234;
                Equal(1234m, queueLimitValue.Value, "custom queue limit");
                overflow.SelectedIndex = 2;
                Equal("Clear buffer and jump to realtime", overflow.SelectedItem.ToString(), "overflow UI selection");

                Equal(0, serviceMode.SelectedIndex, "processing-time service default");
                serviceMode.SelectedIndex = 1;
                Application.DoEvents();
                dinPreset.PerformClick();
                NumericUpDown serviceValue = FindNumericWithMaximum(controls, 100000000m);
                if (serviceValue == null) throw new Exception("bitrate numeric field was not found");
                Equal(31250m, serviceValue.Value, "5-pin DIN UI preset");
                serviceValue.Value = 50000m;
                serviceMode.SelectedIndex = 0;
                Application.DoEvents();
                Equal(100m, serviceValue.Value, "processing-time value restored");
                serviceMode.SelectedIndex = 1;
                Application.DoEvents();
                Equal(50000m, serviceValue.Value, "bitrate value restored");
                slowdown.Checked = false;
                Equal(false, serviceValue.Enabled, "service value disabled without slowdown");
                form.Close();
            }
        }

        private static void TestPlaybackTimelineRendering()
        {
            using (PlaybackTimelineView timeline = new PlaybackTimelineView())
            {
                timeline.Size = new Size(800, 42);
                IntPtr handle = timeline.Handle;
                Equal(true, timeline.UsesDoubleBuffer, "timeline double buffering");
                int layouts = 0;
                timeline.Layout += delegate { layouts++; };
                using (Bitmap bitmap = new Bitmap(800, 42))
                {
                    Stopwatch timer = Stopwatch.StartNew();
                    for (int frame = 0; frame < 600; frame++)
                    {
                        timeline.SetTimeline(frame * 16000L, 360000000L);
                        timeline.DrawToBitmap(bitmap, timeline.ClientRectangle);
                    }
                    timer.Stop();
                    Console.WriteLine("      Timeline render benchmark: 600 frames in " + timer.ElapsedMilliseconds + " ms");
                    if (timer.ElapsedMilliseconds > 3000)
                        throw new Exception("timeline rendering benchmark took " + timer.ElapsedMilliseconds + " ms");
                }
                Equal(true, timeline.SetTimeline(360000000L, 360000000L), "timeline reaches source end");
                Equal(false, timeline.SetTimeline(360000000L, 360000000L), "timeline does no work while source end is unchanged");
                Equal(0, layouts, "timeline layout events during updates");
            }
        }

        private static StatisticsView FindStatisticsView(Control parent)
        {
            foreach (Control child in parent.Controls)
            {
                StatisticsView statistics = child as StatisticsView;
                if (statistics != null) return statistics;
                statistics = FindStatisticsView(child);
                if (statistics != null) return statistics;
            }
            return null;
        }

        private static void CollectControls(Control parent, List<Control> result)
        {
            foreach (Control child in parent.Controls)
            {
                result.Add(child);
                CollectControls(child, result);
            }
        }

        private static CheckBox FindCheckBox(List<Control> controls, string text)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                CheckBox checkBox = controls[i] as CheckBox;
                if (checkBox != null && checkBox.Text == text) return checkBox;
            }
            return null;
        }

        private static ComboBox FindComboContaining(List<Control> controls, string itemText)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                ComboBox combo = controls[i] as ComboBox;
                if (combo == null) continue;
                for (int item = 0; item < combo.Items.Count; item++)
                    if (String.Equals(combo.Items[item].ToString(), itemText, StringComparison.Ordinal)) return combo;
            }
            return null;
        }

        private static ComboBox FindMidiOutputCombo(List<Control> controls)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                ComboBox combo = controls[i] as ComboBox;
                if (combo == null) continue;
                for (int item = 0; item < combo.Items.Count; item++)
                    if (combo.Items[item] is MidiOutputDeviceInfo) return combo;
            }
            return null;
        }

        private static bool ContainsLabel(List<Control> controls, string text)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                Label label = controls[i] as Label;
                if (label != null && label.Text == text) return true;
            }
            return false;
        }

        private static T FindControl<T>(List<Control> controls) where T : Control
        {
            for (int i = 0; i < controls.Count; i++)
            {
                T control = controls[i] as T;
                if (control != null) return control;
            }
            return null;
        }

        private static Button FindButton(List<Control> controls, string text)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                Button button = controls[i] as Button;
                if (button != null && button.Text == text) return button;
            }
            return null;
        }

        private static NumericUpDown FindNumericWithMaximum(List<Control> controls, decimal maximum)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                NumericUpDown numeric = controls[i] as NumericUpDown;
                if (numeric != null && numeric.Maximum == maximum) return numeric;
            }
            return null;
        }

        private static NumericUpDown FindNumericWithValue(List<Control> controls, decimal value)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                NumericUpDown numeric = controls[i] as NumericUpDown;
                if (numeric != null && numeric.Value == value) return numeric;
            }
            return null;
        }

        private static byte[] BuildTestMidi()
        {
            List<byte> bytes = new List<byte>();
            AddAscii(bytes, "MThd");
            AddUInt32(bytes, 6);
            AddUInt16(bytes, 1);
            AddUInt16(bytes, 2);
            AddUInt16(bytes, 480);

            List<byte> tempoTrack = new List<byte>();
            Add(tempoTrack, 0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20);
            Add(tempoTrack, 0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40);
            Add(tempoTrack, 0x00, 0xFF, 0x2F, 0x00);
            AddTrack(bytes, tempoTrack);

            List<byte> noteTrack = new List<byte>();
            Add(noteTrack, 0x00, 0xC0, 0x05);
            Add(noteTrack, 0x00, 0x90, 0x3C, 0x64);
            Add(noteTrack, 0x81, 0x70, 0x3E, 0x64);
            Add(noteTrack, 0x81, 0x70, 0x80, 0x3C, 0x00);
            Add(noteTrack, 0x83, 0x60, 0xF0, 0x03, 0x7D, 0x01, 0xF7);
            Add(noteTrack, 0x00, 0x80, 0x3E, 0x00);
            Add(noteTrack, 0x00, 0xFF, 0x2F, 0x00);
            AddTrack(bytes, noteTrack);
            return bytes.ToArray();
        }

        private static MidiSong BuildSong(long[] eventTimes)
        {
            MidiSong song = new MidiSong();
            song.FilePath = "synthetic.mid";
            song.Format = 0;
            song.TrackCount = 1;
            song.TicksPerQuarterNote = 480;
            song.Events = new List<MidiEvent>();
            for (int i = 0; i < eventTimes.Length; i++)
            {
                MidiEvent midiEvent = new MidiEvent();
                midiEvent.AbsoluteTick = i;
                midiEvent.IntendedMicroseconds = eventTimes[i];
                midiEvent.Track = 0;
                midiEvent.Order = i;
                midiEvent.Kind = MidiEventKind.NoteOn;
                midiEvent.Channel = 0;
                midiEvent.Status = 0x90;
                midiEvent.Data = new byte[] { 0x90, (byte)(60 + (i % 12)), 1 };
                song.Events.Add(midiEvent);
            }
            song.DurationMicroseconds = eventTimes.Length == 0 ? 0 : eventTimes[eventTimes.Length - 1];
            return song;
        }

        private static MidiEvent SysExEvent(byte status, byte[] data)
        {
            MidiEvent midiEvent = new MidiEvent();
            midiEvent.Kind = MidiEventKind.SystemExclusive;
            midiEvent.Status = status;
            midiEvent.Channel = -1;
            midiEvent.Data = data;
            return midiEvent;
        }

        private static void WaitFor(Func<bool> condition, int timeoutMilliseconds, string name)
        {
            Stopwatch timer = Stopwatch.StartNew();
            while (!condition())
            {
                if (timer.ElapsedMilliseconds > timeoutMilliseconds)
                    throw new Exception(name + " timed out");
                Thread.Sleep(1);
            }
        }

        private static void AddTrack(List<byte> target, List<byte> track)
        {
            AddAscii(target, "MTrk");
            AddUInt32(target, (uint)track.Count);
            target.AddRange(track);
        }

        private static void Add(List<byte> target, params byte[] values)
        {
            target.AddRange(values);
        }

        private static void AddAscii(List<byte> target, string value)
        {
            for (int i = 0; i < value.Length; i++) target.Add((byte)value[i]);
        }

        private static void AddUInt16(List<byte> target, ushort value)
        {
            target.Add((byte)(value >> 8));
            target.Add((byte)value);
        }

        private static void AddUInt32(List<byte> target, uint value)
        {
            target.Add((byte)(value >> 24));
            target.Add((byte)(value >> 16));
            target.Add((byte)(value >> 8));
            target.Add((byte)value);
        }

        private static void Sequence<T>(IList<T> expected, IList<T> actual, string name)
        {
            Equal(expected.Count, actual.Count, name + " count");
            for (int i = 0; i < expected.Count; i++)
                Equal(expected[i], actual[i], name + "[" + i + "]");
        }

        private static void ByteSequence(IList<byte> expected, IList<byte> actual, string name)
        {
            if (actual == null) throw new Exception(name + " is null");
            Equal(expected.Count, actual.Count, name + " count");
            for (int i = 0; i < expected.Count; i++)
                Equal(expected[i], actual[i], name + "[" + i + "]");
        }

        private static void Equal<T>(T expected, T actual, string name)
        {
            if (!EqualityComparer<T>.Default.Equals(expected, actual))
                throw new Exception(name + ": expected " + expected + ", got " + actual);
        }

        private static void Near(long expected, long actual, long tolerance, string name)
        {
            if (Math.Abs(expected - actual) > tolerance)
                throw new Exception(name + ": expected " + expected + " ± " + tolerance + ", got " + actual);
        }

        private sealed class FakeMidiOutput : IMidiOutput
        {
            private readonly object _sync = new object();
            private readonly List<long> _sent = new List<long>();
            private readonly List<MidiEvent> _sentEvents = new List<MidiEvent>();
            public int ResetCount;
            public int PanicCount;

            public void Send(MidiEvent midiEvent)
            {
                lock (_sync)
                {
                    _sent.Add(midiEvent.IntendedMicroseconds);
                    _sentEvents.Add(midiEvent);
                }
            }

            public void Panic() { lock (_sync) PanicCount++; }

            public void Reset()
            {
                lock (_sync)
                {
                    ResetCount++;
                    _sent.Clear();
                    _sentEvents.Clear();
                }
            }

            public List<long> SentTimes()
            {
                lock (_sync) return new List<long>(_sent);
            }

            public List<int> SentNoteNumbers()
            {
                lock (_sync)
                {
                    List<int> notes = new List<int>();
                    for (int i = 0; i < _sentEvents.Count; i++)
                        notes.Add(_sentEvents[i].Data != null && _sentEvents[i].Data.Length > 1 ? _sentEvents[i].Data[1] : -1);
                    return notes;
                }
            }
        }

        private sealed class FakeKdmApiNative : IKdmApiNative
        {
            public int InitializeCount;
            public int TerminateCount;
            public int ResetCount;
            public int PrepareLongCount;
            public int SendLongCount;
            public int UnprepareLongCount;
            public readonly List<uint> ShortMessages = new List<uint>();

            public bool IsAvailable() { return true; }
            public bool InitializeStream() { InitializeCount++; return true; }
            public bool TerminateStream() { TerminateCount++; return true; }
            public void ResetStream() { ResetCount++; }
            public void SendShort(uint message) { ShortMessages.Add(message); }
            public uint PrepareLong(IntPtr header, uint headerSize) { PrepareLongCount++; return 0; }
            public uint SendLong(IntPtr header, uint headerSize) { SendLongCount++; return 0; }
            public uint UnprepareLong(IntPtr header, uint headerSize) { UnprepareLongCount++; return 0; }
            public void Dispose() { }
        }
    }
}
