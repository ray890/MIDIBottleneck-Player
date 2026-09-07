using System;
using System.Collections.Generic;
using System.IO;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.Threading;
using System.Runtime.InteropServices;
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
                if (arguments.Length == 3 && arguments[0] == "--midi-file-sysex")
                {
                    ProbeFileSystemExclusive(UInt32.Parse(arguments[1]), arguments[2], false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--kdmapi-file-sysex")
                {
                    ProbeFileSystemExclusive(0, arguments[1], true);
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
                if (arguments.Length == 2 && arguments[0] == "--kdmapi-provider-probe")
                {
                    ProbeKdmApiProvider(arguments[1], true);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--kdmapi-provider-short-probe")
                {
                    ProbeKdmApiProvider(arguments[1], false);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--winmm-short-probe")
                {
                    ProbeWinMmProvider(false);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--winmm-sysex-probe")
                {
                    ProbeWinMmProvider(true);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--winmm-raw-probe")
                {
                    ProbeRawWinMmReturns();
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--kdmapi-header-probe")
                {
                    ProbeKdmApiHeaderContract();
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--sysex-report")
                {
                    ReportSystemExclusivePackets(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--analyze-drop")
                {
                    AnalyzeDropFile(arguments[1], Int64.Parse(arguments[2]));
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--benchmark-bounded")
                {
                    BenchmarkBoundedFile(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--benchmark-output-window")
                {
                    BenchmarkOutputWindow(arguments[1], arguments[2]);
                    return 0;
                }
                if (arguments.Length == 5 && arguments[0] == "--benchmark-output-range")
                {
                    BenchmarkOutputWindow(arguments[1], arguments[2], Int64.Parse(arguments[3], CultureInfo.InvariantCulture),
                        Int64.Parse(arguments[4], CultureInfo.InvariantCulture));
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--benchmark-load-memory")
                {
                    BenchmarkSequentialLoadMemory(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--benchmark-analysis")
                {
                    BenchmarkAnalysis(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--benchmark-output-window-ui")
                {
                    BenchmarkOutputWindowWithUi(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--benchmark-ui-load")
                {
                    BenchmarkAsyncUiLoad(arguments[1]);
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
                    RenderMainWindow(arguments[1], false, false, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-default-min")
                {
                    RenderDefaultMinimumMainWindow(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-bitrate")
                {
                    RenderMainWindow(arguments[1], true, false, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-min")
                {
                    RenderMainWindow(arguments[1], false, true, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-min-bitrate")
                {
                    RenderMainWindow(arguments[1], true, true, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-min-finite")
                {
                    RenderMainWindow(arguments[1], false, true, true);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-finite")
                {
                    RenderMainWindow(arguments[1], false, false, true);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-ui-clientwidth")
                {
                    RenderMainWindowAtWidth(arguments[1], Int32.Parse(arguments[2]));
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-restored")
                {
                    RenderRestoredMainWindow(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-ui-loading")
                {
                    RenderLoadingMainWindow(arguments[1], arguments[2]);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis")
                {
                    RenderAnalysisWindow(arguments[1], arguments[2], false, true, true, false, false, null, false);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis-bitrate")
                {
                    RenderAnalysisWindow(arguments[1], arguments[2], true, false, true, false, false, null, false);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis-overlay")
                {
                    RenderAnalysisWindow(arguments[1], arguments[2], false, true, true, true, false, null, false);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis-min")
                {
                    RenderAnalysisWindow(arguments[1], arguments[2], false, true, true, false, true, null, false);
                    return 0;
                }
                if (arguments.Length == 4 && arguments[0] == "--render-analysis-resolution")
                {
                    RenderAnalysisWindow(arguments[1], arguments[2], false, true, true, false, false, arguments[3], false);
                    return 0;
                }
                if (arguments.Length == 4 && arguments[0] == "--render-analysis-custom")
                {
                    RenderCustomAnalysisWindow(arguments[1], arguments[2], Decimal.Parse(arguments[3], CultureInfo.InvariantCulture));
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis-recalculating")
                {
                    RenderBusyAnalysisWindow(arguments[1], arguments[2]);
                    return 0;
                }
                if (arguments.Length == 4 && arguments[0] == "--render-analysis-splitter")
                {
                    RenderAnalysisSplitter(arguments[1], arguments[2], String.Equals(arguments[3], "after", StringComparison.OrdinalIgnoreCase));
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-analysis-edge-pin")
                {
                    RenderAnalysisEdgePin(arguments[1], arguments[2]);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-finishing-ui")
                {
                    RunFocused("contiguous event-storage limit fails clearly before allocation", TestContiguousEventStorageLimit);
                    RunFocused("controlled KDMAPI provider selection and architecture validation", TestKdmApiProviderSelection);
                    RunFocused("background loading, stale-result rejection, and unload", TestBackgroundMidiLoading);
                    RunFocused("asynchronous Analysis refresh and resolution policy", TestAsynchronousAnalysis);
                    RunFocused("configured whole-file Analysis window", TestAnalysisWindowConstruction);
                    RunFocused("selected-model Analysis interaction and seek", TestAnalysisInteraction);
                    RunFocused("Analysis graph geometry, overlay, and coalesced message-pump updates", TestAnalysisRenderingRefinements);
                    RunFocused("WinForms interface construction", TestInterfaceConstruction);
                    RunFocused("finishing layout, loading-footprint, splitter, and edge-pin contracts", TestFinishingReleaseContracts);
                    Console.WriteLine("PASS: " + _passed + " focused finishing tests");
                    return 0;
                }
                Run("tempo map, multiple tracks, running status, and SysEx", TestMidiParser);
                Run("explicit process architecture and packed MIDIHDR ABI", TestProcessArchitecture);
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
                Run("slowdown-enabled zero service uses immediate scheduler path", TestEffectiveZeroServicePath);
                Run("MIDI bitrate byte-duration calculation", TestMidiBitrateCalculation);
                Run("MIDI bitrate production service intervals", TestMidiBitratePlaybackIntervals);
                Run("parsed MIDI Queue playback under MIDI bitrate", TestParsedMidiBitrateQueuePlayback);
                Run("service-mode values remain independent", TestServiceModeValuePreservation);
                Run("active seek clears queued work and stale dispatches", TestActiveSeek);
                Run("paused seek remains paused at a clean position", TestPausedSeek);
                Run("SMF SysEx fragments are framed for strict winmm drivers", TestSystemExclusiveAssembly);
                Run("OmniMIDI rejected packet regression structures and MIDIHDR fields", TestOmniMidiPacketStructures);
                Run("KDMAPI short, SysEx, reset, and stream lifecycle", TestKdmApiOutput);
                Run("KDMAPI initialization failure cleanup", TestKdmApiInitializationCleanup);
                Run("controlled KDMAPI provider selection and architecture validation", TestKdmApiProviderSelection);
                Run("Stop and Seek reset then silence MIDI output", TestOutputResetSilenceContract);
                Run("Reset stats does not disrupt playback", TestResetStatisticsDuringPlayback);
                Run("processing slider low-range mapping and track clicks", TestProcessingSliderMapping);
                Run("effective playback speed rolling estimate", TestEffectivePlaybackSpeed);
                Run("current MIDI output-rate rolling estimate", TestRollingOutputRate);
                Run("live Rate model applies at next service", TestLiveRateModelChange);
                Run("live overflow policy applies at next overflow", TestLiveOverflowPolicyChange);
                Run("output restart preserves paused source position and clears backlog", TestOutputRestartSemantics);
                Run("dense 200,000-event MIDI parsing", TestDenseMidiParser);
                Run("cancellable parser progress and cancellation", TestCancellableMidiParser);
                Run("contiguous event-storage limit fails clearly before allocation", TestContiguousEventStorageLimit);
                Run("background loading, stale-result rejection, and unload", TestBackgroundMidiLoading);
                Run("workload analysis and graph data", TestWorkloadAnalysis);
                Run("asynchronous Analysis refresh and resolution policy", TestAsynchronousAnalysis);
                Run("configured whole-file Analysis window", TestAnalysisWindowConstruction);
                Run("selected-model Analysis interaction and seek", TestAnalysisInteraction);
                Run("Analysis graph geometry, overlay, and coalesced message-pump updates", TestAnalysisRenderingRefinements);
                Run("Analysis paused seek resumes and stopped seek stays stopped", TestAnalysisSeekPlaybackPolicy);
                Run("consistent simulated-lag formatting", TestLagFormatting);
                Run("conditional queue-pressure rendering semantics", TestQueuePressureView);
                Run("statistics return width after long values", TestStatisticsWidthRelease);
                Run("corrected 02:03–02:06 benchmark extraction", TestBenchmarkWindowExtraction);
                Run("zero-service extreme burst scheduler responsiveness", TestImmediateBurstPerformance);
                Run("simulated-slowdown burst keeps WinForms responsive", TestSlowdownBurstUiResponsiveness);
                Run("Windows MIDI device enumeration", TestMidiDeviceEnumeration);
                if (Array.IndexOf(arguments, "--midi-integration") >= 0)
                    Run("MIDI SysEx output and repeated device switching", TestMidiOutputSwitching);
                Run("WinForms interface construction", TestInterfaceConstruction);
                Run("finishing layout, loading-footprint, splitter, and edge-pin contracts", TestFinishingReleaseContracts);
                Run("playback timeline rendering path", TestPlaybackTimelineRendering);
                Console.WriteLine("PASS: " + _passed + " tests");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("FAIL: " + ex.ToString());
                return 1;
            }
        }

        private static void Run(string name, Action test)
        {
            test();
            _passed++;
            Console.WriteLine("  OK  " + name);
        }

        private static void RunFocused(string name, Action test)
        {
            Run(name, test);
        }

        private static void ReportSystemExclusivePackets(string midiPath)
        {
            MidiSong song = MidiFileParser.Load(midiPath);
            SystemExclusiveAssembler assembler = new SystemExclusiveAssembler();
            List<string> fragments = new List<string>();
            int packetNumber = 0;
            for (int eventIndex = 0; eventIndex < song.Events.Count; eventIndex++)
            {
                MidiEvent midiEvent = song.Events[eventIndex];
                if (midiEvent.Kind != MidiEventKind.SystemExclusive) continue;
                if (midiEvent.Status == 0xF0) fragments.Clear();
                fragments.Add(String.Format("#{0} tick={1} time={2}us status={3:X2} data={4}",
                    eventIndex, midiEvent.AbsoluteTick, midiEvent.IntendedMicroseconds, midiEvent.Status, midiEvent.Data.Length));
                byte[] packet = assembler.Accept(midiEvent);
                if (packet == null) continue;
                packetNumber++;
                Console.WriteLine("Packet {0}: {1} bytes, framed={2}, fragments=[{3}]", packetNumber, packet.Length,
                    SystemExclusiveAssembler.IsComplete(packet), String.Join("; ", fragments.ToArray()));
                Console.WriteLine("  leading={0}", FormatHexEdge(packet, 0, Math.Min(16, packet.Length)));
                Console.WriteLine("  trailing={0}", FormatHexEdge(packet, Math.Max(0, packet.Length - 16), Math.Min(16, packet.Length)));
                fragments.Clear();
            }
            Console.WriteLine("Total complete packets: {0}", packetNumber);
        }

        private static string FormatHexEdge(byte[] bytes, int start, int count)
        {
            string[] values = new string[count];
            for (int i = 0; i < count; i++) values[i] = bytes[start + i].ToString("X2");
            return String.Join(" ", values);
        }

        private static void RenderMainWindow(string outputPath, bool bitrateMode, bool minimumSize, bool finite)
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
                    form.Size = form.MinimumSize;
                    Application.DoEvents();
                }
                if (bitrateMode)
                {
                    List<Control> controls = new List<Control>();
                    CollectControls(form, controls);
                    ComboBox serviceMode = FindComboContaining(controls, "MIDI serial bitrate");
                    if (serviceMode == null) throw new Exception("MIDI bitrate mode was not found for UI rendering");
                    serviceMode.SelectedIndex = 1;
                    if (minimumSize)
                    {
                        NumericUpDown bitrate = FindNumericWithMaximum(controls, 100000000m);
                        if (bitrate != null) bitrate.Value = bitrate.Maximum;
                    }
                    Application.DoEvents();
                }
                if (finite)
                {
                    List<Control> controls = new List<Control>();
                    CollectControls(form, controls);
                    CheckBox queueLimit = FindCheckBox(controls, minimumSize ? "Queue limit:" : "Queue length limit:");
                    if (queueLimit == null) throw new Exception("Queue limit was not found for UI rendering");
                    queueLimit.Checked = true;
                    Application.DoEvents();
                }
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered UI: " + Path.GetFullPath(outputPath));
        }

        private static void RenderDefaultMinimumMainWindow(string outputPath)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); Application.DoEvents();
                form.ClientSize = new Size(640, form.ClientSize.Height); Application.DoEvents();
                form.Size = new Size(form.Width, form.MinimumSize.Height); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                Console.WriteLine("Rendered default minimum " + form.Width + "x" + form.Height + ": " + Path.GetFullPath(outputPath));
                form.Close();
            }
        }

        private static void RenderMainWindowAtWidth(string outputPath, int clientWidth)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show();
                Application.DoEvents();
                if (clientWidth >= 640)
                {
                    form.ClientSize = new Size(639, 525);
                    Application.DoEvents();
                }
                form.ClientSize = new Size(clientWidth, 570);
                Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered UI at " + clientWidth + " client pixels: " + Path.GetFullPath(outputPath));
        }

        private static void RenderRestoredMainWindow(string outputPath)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); Application.DoEvents();
                form.Size = new Size(500, 510); Application.DoEvents();
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                ComboBox serviceMode = FindComboContaining(controls, "MIDI serial bitrate");
                serviceMode.SelectedIndex = 1; Application.DoEvents();
                form.Size = new Size(806, 590); Application.DoEvents();
                form.Refresh(); PumpFor(150);
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered compact-to-default restoration: " + Path.GetFullPath(outputPath));
        }

        private static void RenderLoadingMainWindow(string midiPath, string outputPath)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.SuppressLoadErrorDialogs = true;
                form.Show(); Application.DoEvents();
                form.BeginMidiLoad(midiPath);
                PumpUntil(delegate
                {
                    return !form.IsLoadingSong || form.LoadingOverallPermille >= 5;
                }, 10000, "loading-progress visual state");
                if (!form.IsLoadingSong)
                    throw new Exception("the selected MIDI completed before its loading-progress state could be captured");
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.CancelMidiLoad();
                form.Close();
            }
            Console.WriteLine("Rendered two-level loading progress: " + Path.GetFullPath(outputPath));
        }

        private static void RenderCustomAnalysisWindow(string midiPath, string outputPath, decimal milliseconds)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                string error;
                if (!form.SetCustomResolutionMilliseconds(milliseconds, out error)) throw new Exception(error);
                long expectedResolution = Decimal.ToInt64(Decimal.Round(milliseconds * 1000m, 0, MidpointRounding.AwayFromZero));
                PumpUntil(delegate
                {
                    return !form.CalculationPending && form.ActiveResolutionMicroseconds == expectedResolution;
                }, 30000, "custom-resolution visual Analysis");
                form.Refresh();
                form.Graph.Focus();
                Cursor.Position = form.PointToScreen(new Point(form.ClientSize.Width - 4, form.ClientSize.Height - 4));
                PumpFor(300);
                form.Activate();
                form.BringToFront();
                PumpFor(100);
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered custom Analysis resolution: " + Path.GetFullPath(outputPath));
        }

        private static void RenderBusyAnalysisWindow(string midiPath, string outputPath)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            WorkloadAnalysis completed = WorkloadAnalyzer.Analyze(song, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, completed,
                delegate(MidiSong source, long bucket, AnalysisConfiguration requested, CancellationToken token,
                    Action<WorkloadAnalysisProgress> progress)
                {
                    for (int step = 0; step <= 30; step++)
                    {
                        if (progress != null) progress(new WorkloadAnalysisProgress("Projecting queue pressure", step, 30, 0, 1000));
                        if (token.WaitHandle.WaitOne(100)) token.ThrowIfCancellationRequested();
                    }
                    return WorkloadAnalyzer.Analyze(source, bucket, requested, token, progress);
                }))
            {
                form.Show(); Application.DoEvents();
                AnalysisConfiguration changed = DefaultAnalysisConfiguration();
                changed.ProcessingMicroseconds = 333;
                form.RequestAnalysis(changed);
                PumpUntil(delegate { return form.CalculationStatusVisible && form.CalculationProgressPermille > 0; },
                    2500, "delayed Analysis progress visual state");
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.CancelAnalysisCalculation();
                form.Close();
            }
            Console.WriteLine("Rendered delayed Analysis progress: " + Path.GetFullPath(outputPath));
        }

        private static void RenderAnalysisWindow(string midiPath, string outputPath, bool bitrate, bool finite, bool pinned, bool overlay,
            bool minimum, string resolution, bool recalculating)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.ServiceDurationMode = bitrate ? ServiceDurationMode.MidiBitrate : ServiceDurationMode.ProcessingTime;
            configuration.QueueLengthLimitEnabled = finite;
            configuration.QueueLengthLimit = 64;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show();
                Application.DoEvents();
                if (minimum)
                {
                    form.Size = form.MinimumSize;
                    Application.DoEvents();
                }
                if (!String.IsNullOrEmpty(resolution))
                {
                    List<Control> resolutionControls = new List<Control>();
                    CollectControls(form, resolutionControls);
                    ComboBox resolutionCombo = FindComboContaining(resolutionControls, "Auto");
                    if (resolutionCombo == null || !resolutionCombo.Items.Contains(resolution)) throw new Exception("Analysis resolution control was not found");
                    resolutionCombo.SelectedItem = resolution;
                    PumpUntil(delegate { return !form.CalculationPending; }, 10000, "Analysis resolution render");
                }
                if (recalculating)
                {
                    AnalysisConfiguration changed = DefaultAnalysisConfiguration();
                    changed.ProcessingMicroseconds = 333;
                    form.RequestAnalysis(changed);
                    PumpUntil(delegate { return form.CalculationStatusVisible || !form.CalculationPending; }, 5000, "Analysis recalculation indicator render");
                }
                if (pinned)
                {
                    List<Control> controls = new List<Control>();
                    CollectControls(form, controls);
                    WorkloadGraph graph = FindControl<WorkloadGraph>(controls);
                    if (graph != null) graph.InspectAtClientX(graph.GraphArea.Left + graph.GraphArea.Width * 2 / 3, true);
                    PlaybackSnapshot snapshot = new PlaybackSnapshot();
                    snapshot.State = PlaybackState.Playing;
                    snapshot.IntendedTimelineMicroseconds = song.DurationMicroseconds * 3 / 5;
                    snapshot.LastDispatchedTimelineMicroseconds = song.DurationMicroseconds * 2 / 5;
                    form.UpdatePlaybackSnapshot(snapshot, new PlaybackOverlayData
                    {
                        State = "Playing", TimelineAndOutput = "03:36.000 / 02:24.000", Queue = "1,463 / 2,000",
                        Events = "82,504 / 417", OutputRate = "2,834.0 events/sec", EffectiveSpeed = "72.4%",
                        Lag = "700.000 ms / 1,245.000 ms"
                    });
                    if (overlay)
                    {
                        CheckBox playbackStatistics = FindCheckBox(controls, "Playback statistics");
                        if (playbackStatistics != null) playbackStatistics.Checked = true;
                    }
                    Application.DoEvents();
                }
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    if (!recalculating) PumpFor(700);
                    else Application.DoEvents();
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered analysis: " + Path.GetFullPath(outputPath));
        }

        private static void RenderAnalysisSplitter(string midiPath, string outputPath, bool moved)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                if (moved)
                {
                    form.AnalysisSplit.SplitterDistance = Math.Min(form.AnalysisSplit.Width - form.AnalysisSplit.Panel2MinSize -
                        form.AnalysisSplit.SplitterWidth, form.AnalysisSplit.SplitterDistance + 90);
                    Application.DoEvents();
                }
                PumpFor(300);
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered Analysis splitter " + (moved ? "after drag: " : "before interaction: ") + Path.GetFullPath(outputPath));
        }

        private static void RenderAnalysisEdgePin(string midiPath, string outputPath)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, DefaultAnalysisConfiguration());
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                WorkloadGraph graph = form.Graph;
                Rectangle area = graph.GraphArea;
                int x = area.Left - 3;
                int y = area.Top + area.Height / 2;
                IntPtr coordinates = new IntPtr(((y & 0xFFFF) << 16) | (x & 0xFFFF));
                SendMessage(graph.Handle, 0x0201, new IntPtr(1), coordinates);
                SendMessage(graph.Handle, 0x0202, IntPtr.Zero, coordinates);
                Application.DoEvents();
                Equal((long?)0L, graph.PinnedTimeMicroseconds, "rendered tolerated edge pin");
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered Analysis zero-time edge pin: " + Path.GetFullPath(outputPath));
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
                Equal(2L, song.NoteCount, "musical note-on count");
                Equal((long)BuildTestMidi().Length, song.FileSizeBytes, "source file size");
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
                Equal("6 events  •  2 tracks  •  2 notes  •  00:01.500", MainForm.FormatFileInformation(song, false), "normal file information");
                Equal("6 events  •  2 tracks" + Environment.NewLine + "2 notes  •  00:01.500", MainForm.FormatFileInformation(song, true), "compact file information");
            }
            finally
            {
                if (File.Exists(path)) File.Delete(path);
            }
        }

        private static void TestProcessArchitecture()
        {
#if ARCH_X86
            Equal(4, IntPtr.Size, "x86 process pointer size");
            Equal(64, Marshal.SizeOf(typeof(NativeMidiHeader)), "x86 packed MIDIHDR size");
#elif ARCH_X64
            Equal(8, IntPtr.Size, "x64 process pointer size");
            Equal(112, Marshal.SizeOf(typeof(NativeMidiHeader)), "x64 packed MIDIHDR size");
#else
            throw new Exception("test executable was not compiled for an explicit architecture");
#endif
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

        private static void TestEffectiveZeroServicePath()
        {
            const int benchmarkCount = 250000;
            MidiSong benchmarkSong = BuildSong(new long[benchmarkCount]);
            MeasureEngineRun(BuildSong(new long[1000]), false, ServiceDurationMode.ProcessingTime, 0, ProcessingMode.Queue, 0);
            long disabledMilliseconds = MeasureEngineRun(benchmarkSong, false, ServiceDurationMode.ProcessingTime, 0,
                ProcessingMode.Queue, benchmarkCount);
            long enabledZeroMilliseconds = MeasureEngineRun(benchmarkSong, true, ServiceDurationMode.ProcessingTime, 0,
                ProcessingMode.Queue, benchmarkCount);
            if (disabledMilliseconds > 2500 || enabledZeroMilliseconds > 2500)
                throw new Exception("effective-zero benchmark was unexpectedly slow: off=" + disabledMilliseconds +
                    " ms, on/zero=" + enabledZeroMilliseconds + " ms");
            if (enabledZeroMilliseconds > disabledMilliseconds * 3 + 150)
                throw new Exception("slowdown-on zero service did not approach the slowdown-off fast path: off=" +
                    disabledMilliseconds + " ms, on/zero=" + enabledZeroMilliseconds + " ms");
            Console.WriteLine("      Effective-zero benchmark (" + benchmarkCount.ToString("N0") +
                " events): slowdown off " + disabledMilliseconds + " ms; slowdown on at 0 µs " +
                enabledZeroMilliseconds + " ms");

            MidiSong finiteSong = BuildSong(new long[10000]);
            CountingMidiOutput finiteOutput = new CountingMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = true;
                engine.ServiceDurationMode = ServiceDurationMode.ProcessingTime;
                engine.ProcessingMicroseconds = 0;
                engine.QueueLengthLimit = 1;
                engine.Start(finiteSong, finiteOutput, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State != PlaybackState.Playing; }, 3000, "finite effective-zero run");
                PlaybackSnapshot snapshot = engine.GetSnapshot();
                Equal(true, snapshot.SimulateSlowdown, "zero fast path preserves logical slowdown state");
                Equal(ServiceDurationMode.ProcessingTime, snapshot.ServiceDurationMode, "zero fast path preserves Rate model");
                Equal(0L, snapshot.ProcessingMicroseconds, "zero fast path preserves configured processing time");
                Equal(10000L, snapshot.ProcessedEvents, "instantaneous finite queue sends every arrival");
                Equal(0L, snapshot.DroppedEvents, "instantaneous finite queue does not invent overflow");
            }

            long nonzeroMilliseconds = MeasureEngineRun(BuildSong(new long[1500]), true,
                ServiceDurationMode.ProcessingTime, 100, ProcessingMode.Queue, 1500);
            if (nonzeroMilliseconds < 75)
                throw new Exception("small nonzero processing time incorrectly used the zero-service path (" + nonzeroMilliseconds + " ms)");
            long bitrateMilliseconds = MeasureEngineRun(BuildSong(new long[30]), true,
                ServiceDurationMode.MidiBitrate, 0, ProcessingMode.Queue, 30);
            if (bitrateMilliseconds < 15)
                throw new Exception("MIDI serial bitrate incorrectly used the processing-time zero path (" + bitrateMilliseconds + " ms)");

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                CallbackMidiOutput output = new CallbackMidiOutput(delegate(long sent)
                {
                    if (sent == 500) engine.ProcessingMicroseconds = 200;
                });
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 0;
                Stopwatch elapsed = Stopwatch.StartNew();
                engine.Start(BuildSong(new long[5000]), output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State != PlaybackState.Playing; }, 4000, "live zero-to-nonzero change");
                elapsed.Stop();
                Equal(5000L, output.Count, "zero-to-nonzero output count");
                if (elapsed.ElapsedMilliseconds < 400)
                    throw new Exception("live zero-to-nonzero change was not observed within a bounded batch");
            }
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                CallbackMidiOutput output = new CallbackMidiOutput(delegate(long sent)
                {
                    if (sent == 500) engine.ProcessingMicroseconds = 0;
                });
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 200;
                Stopwatch elapsed = Stopwatch.StartNew();
                engine.Start(BuildSong(new long[5000]), output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State != PlaybackState.Playing; }, 3000, "live nonzero-to-zero change");
                elapsed.Stop();
                Equal(5000L, output.Count, "nonzero-to-zero output count");
                if (elapsed.ElapsedMilliseconds > 1000)
                    throw new Exception("live nonzero-to-zero change did not enter the fast path promptly");
            }

            long[] controlledTimes = new long[100000];
            for (int i = controlledTimes.Length / 2; i < controlledTimes.Length; i++) controlledTimes[i] = 1000000;
            MidiSong controlledSong = BuildSong(controlledTimes);
            PacedMidiOutput controlledOutput = new PacedMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 0;
                engine.Start(controlledSong, controlledOutput, ProcessingMode.Queue);
                WaitFor(delegate { return controlledOutput.Count >= 500; }, 2000, "effective-zero control burst start");
                Stopwatch pauseTimer = Stopwatch.StartNew();
                engine.Pause();
                pauseTimer.Stop();
                if (pauseTimer.ElapsedMilliseconds > 500) throw new Exception("Pause was not responsive during zero-duration burst");
                PumpFor(30);
                long pausedCount = controlledOutput.Count;
                PumpFor(30);
                Equal(pausedCount, controlledOutput.Count, "paused zero-duration burst remains stopped");
                Stopwatch seekTimer = Stopwatch.StartNew();
                engine.Seek(1000000);
                seekTimer.Stop();
                if (seekTimer.ElapsedMilliseconds > 1000) throw new Exception("Seek was not responsive during zero-duration burst");
                Equal(PlaybackState.Paused, engine.State, "seek preserves paused state during burst");
                Equal(0L, engine.GetSnapshot().OutstandingEvents, "seek clears zero-duration burst backlog");
                if (controlledOutput.ResetCount == 0 || controlledOutput.PanicCount == 0)
                    throw new Exception("seek did not reset and silence the zero-duration burst output");
                engine.Resume();
                WaitFor(delegate { return controlledOutput.Count > pausedCount; }, 2000, "resume after zero-duration seek");
                Stopwatch stopTimer = Stopwatch.StartNew();
                engine.Stop();
                stopTimer.Stop();
                if (stopTimer.ElapsedMilliseconds > 1000) throw new Exception("Stop was not responsive during zero-duration burst");
                Equal(PlaybackState.Stopped, engine.State, "Stop state after zero-duration burst");
            }
        }

        private static long MeasureEngineRun(MidiSong song, bool slowdown, ServiceDurationMode mode,
            long processingMicroseconds, ProcessingMode processingMode, long expectedCount)
        {
            CountingMidiOutput output = new CountingMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = slowdown;
                engine.ServiceDurationMode = mode;
                engine.ProcessingMicroseconds = processingMicroseconds;
                engine.MidiBitrate = ServiceDurationCalculator.FivePinDinBitrate;
                Stopwatch elapsed = Stopwatch.StartNew();
                engine.Start(song, output, processingMode);
                WaitFor(delegate { return engine.State != PlaybackState.Playing; }, 5000, "deterministic scheduler measurement");
                elapsed.Stop();
                if (expectedCount > 0) Equal(expectedCount, output.Count, "deterministic scheduler output count");
                return elapsed.ElapsedMilliseconds;
            }
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

        private static void TestOmniMidiPacketStructures()
        {
            MidiEvent gm = SysExEvent(0xF0, new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 });
            gm.EventIndex = 1;
            MidiEvent circus = SysExEvent(0xF0, new byte[] { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 });
            circus.EventIndex = 0;
            circus.AbsoluteTick = 0;
            circus.IntendedMicroseconds = 0;
            MidiEvent lbsfs = SysExEvent(0xF0, new byte[] { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 });
            lbsfs.EventIndex = 54;
            lbsfs.AbsoluteTick = 960;
            lbsfs.IntendedMicroseconds = 571426;

            SystemExclusiveAssembler assembler = new SystemExclusiveAssembler();
            Equal(6, assembler.AcceptPacket(gm).Bytes.Length, "GM System On packet length");
            SystemExclusivePacket circusPacket = assembler.AcceptPacket(circus);
            Equal(11, circusPacket.Bytes.Length, "Circus Galop packet length");
            Equal(true, SystemExclusiveAssembler.IsComplete(circusPacket.Bytes), "Circus Galop framing");
            SystemExclusivePacket lbsfsPacket = assembler.AcceptPacket(lbsfs);
            Equal(9, lbsfsPacket.Bytes.Length, "lbsfs packet length");
            Equal(54, lbsfsPacket.Fragments[0].EventIndex, "lbsfs source event index");

            FakeKdmApiNative native = new FakeKdmApiNative();
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(native))
            {
                output.Open();
                output.Send(circus);
                Equal((uint)11, native.LastHeader.BufferLength, "MIDIHDR buffer length");
                Equal((uint)0, native.LastHeader.BytesRecorded, "output MIDIHDR recorded bytes remain zero");
                Equal((uint)0, native.LastInitialFlags, "MIDIHDR initial flags");
                Equal(IntPtr.Size == 8 ? 112 : 64, native.LastHeaderSize, "packed native MIDIHDR size");
            }
            IntPtr testData = Marshal.AllocHGlobal(6);
            try
            {
                NativeMidiHeader outputHeader = NativeMidiHeader.CreateOutput(testData, 6);
                Equal((uint)6, outputHeader.BufferLength, "shared WinMM/KDMAPI output length");
                Equal((uint)0, outputHeader.BytesRecorded, "shared output header input-byte field");
                Equal((uint)0, outputHeader.Flags, "shared output header flags");
                Equal(IntPtr.Zero, outputHeader.Reserved7, "shared output header reserved fields");
                Equal(IntPtr.Size == 8 ? 112 : 64, Marshal.SizeOf(typeof(NativeMidiHeader)), "shared packed native MIDIHDR layout");
                Equal(new IntPtr(IntPtr.Size == 8 ? 28 : 20), Marshal.OffsetOf(typeof(NativeMidiHeader), "Next"), "packed MIDIHDR lpNext offset");
            }
            finally { Marshal.FreeHGlobal(testData); }
            FakeKdmApiNative rejecting = new FakeKdmApiNative();
            rejecting.PrepareLongResult = 11;
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(rejecting))
            {
                output.SourceFile = "lbsfs 211k.mid";
                output.Open();
                try
                {
                    output.Send(lbsfs);
                    throw new Exception("rejected SysEx packet did not raise an error");
                }
                catch (System.ComponentModel.Win32Exception ex)
                {
                    if (!ex.Message.Contains("lbsfs 211k.mid") || !ex.Message.Contains("event #54") ||
                        !ex.Message.Contains("Packet: 9 bytes") || !ex.Message.Contains("MIDIHDR: size="))
                        throw new Exception("SysEx native error lacks packet/header diagnostics: " + ex.Message);
                }
            }
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
                int beforePanic = native.ShortMessages.Count;
                output.Panic();
                Equal(beforePanic + 48, native.ShortMessages.Count, "KDMAPI channel panic message count");
                Equal((uint)(0xB0 | (120 << 8)), native.ShortMessages[beforePanic], "KDMAPI CC120 after reset");
                Equal((uint)(0xB0 | (123 << 8)), native.ShortMessages[beforePanic + 1], "KDMAPI CC123 after reset");
                Equal((uint)(0xB0 | (64 << 8)), native.ShortMessages[beforePanic + 2], "KDMAPI sustain off after reset");
            }
            Equal(1, native.TerminateCount, "KDMAPI terminate count");
            if (native.ResetCount < 2) throw new Exception("KDMAPI stream was not reset before disposal");

            FakeKdmApiNative shortOnly = new FakeKdmApiNative();
            shortOnly.SupportsLongMessages = false;
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(shortOnly))
            {
                output.Open();
                output.Send(ChannelEvent(new byte[] { 0x90, 60, 1 }));
                bool explained = false;
                try { output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7D, 0x01, 0xF7 })); }
                catch (NotSupportedException ex)
                {
                    explained = ex.Message.IndexOf("short MIDI messages", StringComparison.OrdinalIgnoreCase) >= 0 &&
                        ex.Message.IndexOf("PrepareLongData", StringComparison.Ordinal) >= 0 &&
                        ex.Message.IndexOf(shortOnly.ProviderPath, StringComparison.Ordinal) >= 0;
                }
                Equal(true, explained, "short-only KDMAPI provider reports its SysEx limitation without dropping data");
            }
        }

        private static void TestKdmApiInitializationCleanup()
        {
            FakeKdmApiNative native = new FakeKdmApiNative();
            native.InitializeResult = false;
            bool failed = false;
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(native, true))
            {
                try { output.Open(); }
                catch (InvalidOperationException ex)
                {
                    failed = ex.Message.IndexOf("rejected stream initialization", StringComparison.OrdinalIgnoreCase) >= 0;
                }
            }
            Equal(true, failed, "phase-specific KDMAPI initialization error");
            Equal(1, native.DisposeCount, "KDMAPI native module cleanup after initialization failure");
            Equal(0, native.TerminateCount, "uninitialized KDMAPI stream not terminated");

            FakeKdmApiNative terminationFailure = new FakeKdmApiNative();
            terminationFailure.TerminateResult = false;
            bool closeFailed = false;
            KdmApiMidiOutput failingClose = new KdmApiMidiOutput(terminationFailure, true);
            failingClose.Open();
            try { failingClose.Close(); }
            catch (InvalidOperationException ex)
            {
                closeFailed = ex.Message.IndexOf("stream termination", StringComparison.OrdinalIgnoreCase) >= 0;
            }
            Equal(true, closeFailed, "phase-specific KDMAPI termination error");
            Equal(1, terminationFailure.DisposeCount, "KDMAPI module released after termination rejection");
        }

        private static void TestOutputResetSilenceContract()
        {
            MidiSong song = BuildSong(new long[] { 0, 500000, 1000000 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().ProcessedEvents >= 1; }, 1000, "first event before seek silence");
                engine.Seek(500000);
                if (output.ResetCount < 1 || output.PanicCount < 1) throw new Exception("Seek did not reset and panic the output");
                if (output.LastPanicSequence <= output.LastResetSequence) throw new Exception("Seek panic was not sent after reset");
                int resetBeforeStop = output.ResetCount;
                int panicBeforeStop = output.PanicCount;
                engine.Stop();
                if (output.ResetCount <= resetBeforeStop || output.PanicCount <= panicBeforeStop)
                    throw new Exception("Stop did not reset and panic the output");
                if (output.LastPanicSequence <= output.LastResetSequence) throw new Exception("Stop panic was not sent after reset");
                Equal(0L, engine.GetSnapshot().OutstandingEvents, "Stop discarded outstanding events");
            }
        }

        private static void TestResetStatisticsDuringPlayback()
        {
            long[] arrivals = new long[40];
            for (int i = 0; i < arrivals.Length; i++) arrivals[i] = i * 1000;
            MidiSong song = BuildSong(arrivals);
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 50000;
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().QueueLength >= 5; }, 1000, "queue before statistics reset");
                int outputResets = output.ResetCount;
                long playbackBefore = engine.GetSnapshot().PlaybackMicroseconds;
                engine.ResetStatistics();
                PlaybackSnapshot reset = engine.GetSnapshot();
                Equal(PlaybackState.Playing, reset.State, "statistics reset playback state");
                Equal(outputResets, output.ResetCount, "statistics reset does not touch MIDI output");
                if (reset.PlaybackMicroseconds < playbackBefore) throw new Exception("statistics reset moved playback backwards");
                if (reset.MaximumQueueLength < reset.QueueLength) throw new Exception("statistics reset maximum queue baseline is below current queue");
                if (reset.MaximumLagMicroseconds < reset.CurrentLagMicroseconds) throw new Exception("statistics reset maximum lag baseline is below current lag");
            }
        }

        private static void TestProcessingSliderMapping()
        {
            Equal(0L, MainForm.SliderToMicroseconds(0), "slider zero");
            Equal(5000L, MainForm.SliderToMicroseconds(ProcessingTrackBar.LowRangeEnd), "slider low-range boundary");
            Equal(1000000L, MainForm.SliderToMicroseconds(ProcessingTrackBar.ScaleMaximum), "slider maximum");
            long previous = -1;
            for (int slider = 0; slider <= ProcessingTrackBar.ScaleMaximum; slider++)
            {
                long value = MainForm.SliderToMicroseconds(slider);
                if (value < previous) throw new Exception("processing slider mapping is not monotonic");
                previous = value;
            }
            long[] calibration = new long[] { 0, 500, 960, 979, 1399, 2000, 3000, 5000, 10000, 1000000 };
            for (int i = 0; i < calibration.Length; i++)
            {
                int slider = MainForm.MicrosecondsToSlider(calibration[i]);
                Near(calibration[i], MainForm.SliderToMicroseconds(slider), calibration[i] <= 5000 ? 1 : Math.Max(2, calibration[i] / 500), "slider round trip " + calibration[i]);
            }
            Equal(0, ProcessingTrackBar.ValueFromTrackPoint(0, 500, 0, 10000), "track click left edge");
            Near(5000, ProcessingTrackBar.ValueFromTrackPoint(250, 500, 0, 10000), 30, "track click midpoint");
            Equal(10000, ProcessingTrackBar.ValueFromTrackPoint(500, 500, 0, 10000), "track click right edge");
            using (ProcessingTrackBar track = new ProcessingTrackBar())
            {
                track.Minimum = 0;
                track.Maximum = 10000;
                track.Size = new Size(500, 45);
                track.ApplyTrackClick(250);
                Near(5000, track.Value, 30, "actual track click changes value");
            }
        }

        private static void TestEffectivePlaybackSpeed()
        {
            EffectivePlaybackSpeed speed = new EffectivePlaybackSpeed();
            double? result = null;
            for (int i = 0; i <= 5; i++)
                result = speed.Add(i * 50000, i * 50000, i * 50000, i, true, i * 50000);
            Near(100, result.Value, 0.01, "realtime regression speed");
            speed.Reset();
            for (int i = 0; i <= 5; i++)
                result = speed.Add(i * 50000, i * 50000, i * 25000, i, false, i * 50000);
            Near(50, result.Value, 0.01, "falling-behind speed");
            speed.Reset();
            for (int i = 0; i <= 5; i++)
                result = speed.Add(i * 50000, i * 50000, i * 75000, i, false, i * 50000);
            Near(150, result.Value, 0.01, "catch-up speed");
            speed.Reset();
            for (int i = 0; i <= 5; i++)
                result = speed.Add(i * 50000, i * 50000, 0, 0, true, i * 50000);
            Near(100, result.Value, 0.01, "synchronized sparse passage remains realtime");
            speed.Reset();
            for (int i = 0; i <= 5; i++)
                result = speed.Add(i * 50000, i * 50000, 0, i, false, i * 50000);
            Equal(null, result, "repeated output timestamps are insufficient movement");
            Equal("—", EffectivePlaybackSpeed.Format(null), "insufficient format");

            speed.WindowMicroseconds = 0;
            Equal(null, speed.Add(0, 0, 0, 0, false, 0), "instantaneous initial sample");
            Equal(null, speed.Add(100000, 100000, 0, 1, false, 100000), "instantaneous repeated timestamp");
            Near(100, speed.Add(200000, 200000, 100000, 2, false, 200000).Value, 0.01, "instantaneous shortest distinct samples");
            long[] presets = new long[] { 0, 100000, 250000, 500000, 1500000 };
            for (int i = 0; i < presets.Length; i++) { speed.WindowMicroseconds = presets[i]; Equal(presets[i], speed.WindowMicroseconds, "speed preset"); }
            Equal(true, EffectivePlaybackSpeed.IsValidWindow(333000), "valid custom interval");
            Equal(false, EffectivePlaybackSpeed.IsValidWindow(1000), "invalid custom interval");
            long parsed;
            Equal(true, UserPreferences.TryParseEffectiveSpeedWindow("333000", out parsed), "stored custom interval validation");
            Equal(333000L, parsed, "stored custom interval value");
            speed.Reset();
            Equal(null, speed.Add(2000000, 2000000, 2000000, 20, false, 2000000), "speed reset discards old window");
        }

        private static void TestRollingOutputRate()
        {
            RollingOutputRate rate = new RollingOutputRate();
            Equal(null, rate.Add(0, 0), "output-rate initial sample");
            rate.Add(10, 100000);
            Near(100, rate.Add(20, 200000).Value, 0.01, "successfully sent events per second");
            rate.Reset();
            Equal(null, rate.Add(20, 300000), "output-rate reset clears history");
            Equal("—", RollingOutputRate.Format(null), "output-rate insufficient format");
        }

        private static void TestLiveRateModelChange()
        {
            MidiSong song = BuildSong(new long[] { 0, 0 });
            FakeMidiOutput output = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 300000;
                Stopwatch elapsed = Stopwatch.StartNew();
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().QueueLength >= 1; }, 1000, "queued event before live Rate model change");
                engine.MidiBitrate = 100000000;
                engine.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000, "completion after live Rate model change");
                elapsed.Stop();
                if (elapsed.ElapsedMilliseconds < 240 || elapsed.ElapsedMilliseconds > 520)
                    throw new Exception("live Rate model did not preserve current service then accelerate the next one: " + elapsed.ElapsedMilliseconds + " ms");
            }
        }

        private static void TestLiveOverflowPolicyChange()
        {
            MidiSong song = BuildSong(new long[] { 0, 0, 200000, 200000 });
            FakeMidiOutput output = new FakeMidiOutput();
            string tracePath = Path.Combine(Path.GetTempPath(), "midi-live-overflow-" + Guid.NewGuid().ToString("N") + ".csv");
            try
            {
                DropTraceRecorder trace = new DropTraceRecorder(0, 300000, 100);
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.QueueLengthLimit = 2;
                    engine.ProcessingMicroseconds = 1000000;
                    engine.OverflowPolicy = OverflowPolicy.DropNewest;
                    engine.SetDropTrace(trace);
                    engine.Start(song, output, ProcessingMode.Drop);
                    WaitFor(delegate { return engine.GetSnapshot().OutstandingEvents == 2; }, 1000, "full buffer before live overflow policy change");
                    engine.OverflowPolicy = OverflowPolicy.ClearBufferAndCatchUp;
                    WaitFor(delegate { return engine.GetSnapshot().DroppedEvents >= 3; }, 1000, "clear-buffer overflow after live policy change");
                    engine.Stop();
                }
                trace.Save(tracePath);
                string traceText = File.ReadAllText(tracePath);
                if (traceText.IndexOf("buffer cleared; caught up to realtime", StringComparison.Ordinal) < 0)
                    throw new Exception("new overflow policy was not read at the later overflow");
            }
            finally { if (File.Exists(tracePath)) File.Delete(tracePath); }
        }

        private static void TestOutputRestartSemantics()
        {
            MidiSong song = BuildSong(new long[] { 0, 50000, 100000, 150000, 200000 });
            FakeMidiOutput first = new FakeMidiOutput();
            FakeMidiOutput second = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.ProcessingMicroseconds = 100000;
                engine.Start(song, first, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().PlaybackMicroseconds >= 60000; }, 1000, "source position before output restart");
                engine.Pause();
                long sourcePosition = engine.GetSnapshot().IntendedTimelineMicroseconds;
                engine.Stop();
                engine.Start(song, second, ProcessingMode.Queue, sourcePosition, true);
                PlaybackSnapshot restarted = engine.GetSnapshot();
                Equal(PlaybackState.Paused, restarted.State, "paused output restart state");
                Near(sourcePosition, restarted.IntendedTimelineMicroseconds, 2, "output restart source position");
                Equal(0L, restarted.QueueLength, "output restart queue cleared");
                Equal(0L, restarted.ProcessedEvents, "output restart statistics cleared");
                if (first.ResetCount < 1) throw new Exception("old output was not reset during restart");
                engine.Resume();
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "output restart completion");
                if (second.SentTimes().Count == 0) throw new Exception("replacement output received no events");
                for (int i = 0; i < second.SentTimes().Count; i++)
                    if (second.SentTimes()[i] < sourcePosition) throw new Exception("stale pre-restart event reached replacement output");
            }
        }

        private static void TestKdmApiIntegration()
        {
            using (KdmApiMidiOutput output = new KdmApiMidiOutput())
            {
                Console.WriteLine("      Loading and initializing OmniMIDI KDMAPI");
                output.Open();
                Console.WriteLine("      Sending framed System Exclusive packet");
                output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 }));
                MidiEvent allNotesOff = BuildSong(new long[] { 0 }).Events[0];
                allNotesOff.Kind = MidiEventKind.ControlChange;
                allNotesOff.Data = new byte[] { 0xB0, 123, 0 };
                Console.WriteLine("      Sending short message");
                output.Send(allNotesOff);
                Console.WriteLine("      Resetting KDMAPI stream");
                MidiOutputSafety.ResetAndSilence(output);
                Console.WriteLine("      KDMAPI stream reset complete");
            }
            Console.WriteLine("      KDMAPI stream terminated");
        }

        private static void ProbeKdmApiProvider(string providerPath, bool includeSystemExclusive)
        {
            Console.WriteLine("Requested KDMAPI provider: " + Path.GetFullPath(providerPath));
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(providerPath))
            {
                Console.WriteLine("Loaded KDMAPI provider: " + output.ProviderPath);
                Console.WriteLine("Initializing KDMAPI stream");
                output.Open();
                Console.WriteLine("Initialized; sending short Note On/Off");
                output.Send(ChannelEvent(new byte[] { 0x90, 60, 1 }));
                output.Send(ChannelEvent(new byte[] { 0x80, 60, 0 }));
                Console.WriteLine("Short messages accepted");
                if (includeSystemExclusive)
                {
                    Console.WriteLine("Sending framed SysEx");
                    output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 }));
                    Console.WriteLine("SysEx accepted");
                }
                Console.WriteLine("Resetting and silencing");
                MidiOutputSafety.ResetAndSilence(output);
                Console.WriteLine("Reset/panic accepted");
            }
            Console.WriteLine("KDMAPI provider terminated cleanly");
        }

        private static void ProbeWinMmProvider(bool includeSystemExclusive)
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            Console.WriteLine("Loaded winmm module: " + WindowsMidiOutput.GetLoadedModulePath());
            Console.WriteLine("Enumerated devices: " + devices.Count);
            for (int i = 0; i < devices.Count; i++)
                Console.WriteLine("  " + devices[i].DeviceId + ": " + devices[i].Name);
            if (devices.Count == 0) throw new InvalidOperationException("The selected WinMM provider exposed no MIDI outputs.");
            MidiOutputDeviceInfo device = devices[0];
            using (WindowsMidiOutput output = new WindowsMidiOutput())
            {
                Console.WriteLine("Opening device " + device.DeviceId + ": " + device.Name);
                output.Open(device.DeviceId);
                Console.WriteLine("Opened; sending short Note On");
                output.Send(ChannelEvent(new byte[] { 0x90, 60, 1 }));
                Console.WriteLine("Note On accepted; sending short Note Off");
                output.Send(ChannelEvent(new byte[] { 0x80, 60, 0 }));
                Console.WriteLine("Short messages accepted");
                if (includeSystemExclusive)
                {
                    Console.WriteLine("Sending framed SysEx");
                    output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 }));
                    Console.WriteLine("SysEx accepted");
                }
                Console.WriteLine("Resetting and sending panic");
                MidiOutputSafety.ResetAndSilence(output);
                Console.WriteLine("Reset/panic accepted");
            }
            Console.WriteLine("WinMM provider closed cleanly");
        }

        private static void ProbeRawWinMmReturns()
        {
            List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
            Console.WriteLine("Loaded winmm module: " + WindowsMidiOutput.GetLoadedModulePath());
            if (devices.Count == 0) throw new InvalidOperationException("No WinMM device was exposed.");
            IntPtr handle;
            uint open = RawMidiOutOpen(out handle, devices[0].DeviceId, IntPtr.Zero, IntPtr.Zero, 0);
            Console.WriteLine("midiOutOpen => " + open + ", handle=0x" + handle.ToInt64().ToString("X"));
            if (open != 0) return;
            try
            {
                uint[] messages = new uint[] { 0x0040B0, 0x00013C90, 0x00003C80, 0x0040B0 };
                string[] names = new string[] { "sustain-off", "note-on", "note-off", "sustain-off" };
                for (int i = 0; i < messages.Length; i++)
                {
                    uint result = RawMidiOutShortMsg(handle, messages[i]);
                    Console.WriteLine("immediate " + names[i] + " => " + result);
                }
                Thread.Sleep(1000);
                for (int i = 0; i < messages.Length; i++)
                {
                    uint result = RawMidiOutShortMsg(handle, messages[i]);
                    Console.WriteLine("after 1000 ms " + names[i] + " => " + result);
                }
                Console.WriteLine("midiOutReset => " + RawMidiOutReset(handle));
            }
            finally { Console.WriteLine("midiOutClose => " + RawMidiOutClose(handle)); }
        }

        [DllImport("winmm.dll", EntryPoint = "midiOutOpen")]
        private static extern uint RawMidiOutOpen(out IntPtr handle, uint deviceId, IntPtr callback, IntPtr instance, uint flags);
        [DllImport("winmm.dll", EntryPoint = "midiOutShortMsg")]
        private static extern uint RawMidiOutShortMsg(IntPtr handle, uint message);
        [DllImport("winmm.dll", EntryPoint = "midiOutReset")]
        private static extern uint RawMidiOutReset(IntPtr handle);
        [DllImport("winmm.dll", EntryPoint = "midiOutClose")]
        private static extern uint RawMidiOutClose(IntPtr handle);

        private static MidiEvent ChannelEvent(byte[] data)
        {
            return new MidiEvent
            {
                Kind = data != null && data.Length > 0 && (data[0] & 0xF0) == 0x80
                    ? MidiEventKind.NoteOff : MidiEventKind.NoteOn,
                Channel = data == null || data.Length == 0 ? -1 : data[0] & 0x0F,
                Status = data == null || data.Length == 0 ? (byte)0 : data[0],
                Data = data
            };
        }

        private static void TestKdmApiProviderSelection()
        {
            Equal(true, WindowsMidiOutput.IsPreparedLongMessageUnsafeDescription("WinMM to KDMAPI or syndrv"),
                "known Snappy WinMM long-message boundary is identified");
            Equal(false, WindowsMidiOutput.IsPreparedLongMessageUnsafeDescription("OmniMIDI - WinMM/KDMAPI target library"),
                "OmniMIDI prepared-long-message path remains enabled");
            Equal(true, WindowsMidiOutput.IsCumulativeByteCountContract(new uint[] { 0, 3, 6, 9 }),
                "Snappy cumulative accepted-byte contract is recognized exactly");
            Equal(false, WindowsMidiOutput.IsCumulativeByteCountContract(new uint[] { 0, 3, 0, 0 }),
                "transient startup recovery is not classified as cumulative results");
            Equal(false, WindowsMidiOutput.IsCumulativeByteCountContract(new uint[] { 0, 3, 6, 8 }),
                "arbitrary nonzero WinMM errors are not accepted");
            string root = Path.Combine(Path.GetTempPath(), "kdm-provider-selection-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                string app = Path.Combine(root, "app");
                string system = Path.Combine(root, "system");
                Directory.CreateDirectory(app);
                Directory.CreateDirectory(system);
                string local = Path.Combine(app, "OmniMIDI.dll");
                string installed = Path.Combine(system, "OmniMIDI.dll");
                string explicitProvider = Path.Combine(root, "custom-provider.dll");
                File.WriteAllBytes(local, new byte[] { 1 });
                File.WriteAllBytes(installed, new byte[] { 2 });
                File.WriteAllBytes(explicitProvider, new byte[] { 3 });
                Equal(Path.GetFullPath(explicitProvider),
                    DynamicKdmApiNative.ResolveProviderPath(explicitProvider, app, system),
                    "explicit KDMAPI provider wins");
                Equal(Path.GetFullPath(local), DynamicKdmApiNative.ResolveProviderPath(null, app, system),
                    "application-local KDMAPI provider wins over installed provider");
                File.Delete(local);
                Equal(Path.GetFullPath(installed), DynamicKdmApiNative.ResolveProviderPath(null, app, system),
                    "installed KDMAPI provider is fallback");

                ushort currentMachine = DynamicKdmApiNative.ReadPeMachine(Process.GetCurrentProcess().MainModule.FileName);
                Equal(IntPtr.Size == 8 ? (ushort)0x8664 : (ushort)0x014C, currentMachine,
                    "provider PE reader matches process architecture");
                string mismatch = Path.Combine(root, "wrong-architecture.dll");
                byte[] image = new byte[128];
                image[0] = 0x4D; image[1] = 0x5A;
                image[0x3C] = 0x40;
                image[0x40] = 0x50; image[0x41] = 0x45;
                ushort wrongMachine = IntPtr.Size == 8 ? (ushort)0x014C : (ushort)0x8664;
                image[0x44] = (byte)wrongMachine; image[0x45] = (byte)(wrongMachine >> 8);
                File.WriteAllBytes(mismatch, image);
                bool rejected = false;
                try { DynamicKdmApiNative.ValidateProviderArchitecture(mismatch); }
                catch (BadImageFormatException ex)
                {
                    rejected = ex.Message.IndexOf(IntPtr.Size == 8 ? "x64" : "x86", StringComparison.OrdinalIgnoreCase) >= 0 &&
                        ex.Message.IndexOf(mismatch, StringComparison.OrdinalIgnoreCase) >= 0;
                }
                Equal(true, rejected, "wrong-architecture local provider is rejected clearly");
            }
            finally
            {
                if (Directory.Exists(root)) Directory.Delete(root, true);
            }
        }

        private static void ProbeKdmApiHeaderContract()
        {
            using (DynamicKdmApiNative native = new DynamicKdmApiNative())
            {
                Console.WriteLine("KDMAPI version " + native.Version + ", available=" + native.IsAvailable());
                Console.WriteLine("Initialize=" + native.InitializeStream());
                IntPtr data = Marshal.AllocHGlobal(6);
                IntPtr headerPointer = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(NativeMidiHeader)));
                try
                {
                    Marshal.Copy(new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 }, 0, data, 6);
                    uint[] sizes = new uint[] { 64, 112, 120, 128 };
                    for (int i = 0; i < sizes.Length; i++)
                    {
                        NativeMidiHeader header = NativeMidiHeader.CreateOutput(data, 6);
                        Marshal.StructureToPtr(header, headerPointer, false);
                        uint result = native.PrepareLong(headerPointer, sizes[i]);
                        Console.WriteLine("size=" + sizes[i] + ", recorded=0 => " + result);
                        if (result == 0) Console.WriteLine("  unprepare => " + native.UnprepareLong(headerPointer, sizes[i]));
                    }
                    NativeMidiHeader recorded = NativeMidiHeader.CreateOutput(data, 6);
                    recorded.BytesRecorded = 6;
                    Marshal.StructureToPtr(recorded, headerPointer, false);
                    uint packedSize = (uint)Marshal.SizeOf(typeof(NativeMidiHeader));
                    uint recordedResult = native.PrepareLong(headerPointer, packedSize);
                    Console.WriteLine("size=" + packedSize + ", recorded=6 => " + recordedResult);
                    if (recordedResult == 0) Console.WriteLine("  unprepare => " + native.UnprepareLong(headerPointer, packedSize));
                }
                finally
                {
                    Marshal.FreeHGlobal(headerPointer);
                    Marshal.FreeHGlobal(data);
                    native.ResetStream();
                    Console.WriteLine("Terminate=" + native.TerminateStream());
                }
            }
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

        private static void TestCancellableMidiParser()
        {
            string path = CreateDenseMidiFile(50000);
            try
            {
                CancellationTokenSource cancellation = new CancellationTokenSource();
                cancellation.Cancel();
                bool cancelled = false;
                try { MidiFileParser.Load(path, cancellation.Token, null); }
                catch (OperationCanceledException) { cancelled = true; }
                Equal(true, cancelled, "parser honors cancellation");

                CancellationTokenSource duringParsing = new CancellationTokenSource();
                cancelled = false;
                try
                {
                    MidiFileParser.Load(path, duringParsing.Token, delegate(MidiLoadProgress progress)
                    {
                        if (progress.Stage.StartsWith("Parsing track", StringComparison.Ordinal)) duringParsing.Cancel();
                    });
                }
                catch (OperationCanceledException) { cancelled = true; }
                Equal(true, cancelled, "parser checks cancellation inside track parsing");

                List<string> stages = new List<string>();
                List<MidiLoadProgress> reports = new List<MidiLoadProgress>();
                MidiSong song = MidiFileParser.Load(path, CancellationToken.None, delegate(MidiLoadProgress progress)
                {
                    stages.Add(progress.Stage);
                    reports.Add(progress);
                });
                Equal(50000, song.Events.Count, "cancellable parser successful result");
                bool parsedStage = false;
                for (int i = 0; i < stages.Count; i++) if (stages[i].StartsWith("Parsing track", StringComparison.Ordinal)) parsedStage = true;
                if (!parsedStage || !stages.Contains("Merging tracks") || !stages.Contains("Assigning playback timestamps") || !stages.Contains("Ready"))
                    throw new Exception("parser did not report its meaningful stages");
                int previousOverall = -1;
                bool sawIntermediateOverall = false;
                for (int i = 0; i < reports.Count; i++)
                {
                    MidiLoadProgress report = reports[i];
                    if (report.OverallPermille < previousOverall)
                        throw new Exception("parser overall progress moved backwards at " + report.Stage);
                    if (report.OverallPermille < 0 || report.OverallPermille > 1000 ||
                        report.StagePermille < 0 || report.StagePermille > 1000)
                        throw new Exception("parser progress escaped the documented range");
                    if (report.OverallPermille > 0 && report.OverallPermille < 1000) sawIntermediateOverall = true;
                    previousOverall = report.OverallPermille;
                }
                Equal(true, sawIntermediateOverall, "parser reports honest intermediate overall progress");
                Equal(1000, reports[reports.Count - 1].OverallPermille, "parser overall progress completes");
                Equal(1000, reports[reports.Count - 1].StagePermille, "parser final stage progress completes");
            }
            finally { if (File.Exists(path)) File.Delete(path); }
        }

        private static void TestContiguousEventStorageLimit()
        {
            long maximum = MidiFileParser.MaximumContiguousEventCount;
            if (maximum <= 0 || maximum > Int32.MaxValue)
                throw new Exception("invalid computed contiguous event-storage limit: " + maximum);
            MidiFileParser.ValidateContiguousEventCount(maximum);
            bool rejected = false;
            try { MidiFileParser.ValidateContiguousEventCount(maximum + 1); }
            catch (InvalidDataException ex)
            {
                rejected = ex.Message.IndexOf("contiguous event-storage limit", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    ex.Message.IndexOf("structural", StringComparison.OrdinalIgnoreCase) >= 0;
            }
            Equal(true, rejected, "impossible contiguous event allocation is rejected clearly");
            long legacyX64 = Math.Min(Int32.MaxValue, (0x7FEFFFFFL - 64L) / 8L);
            long legacyX86 = Math.Min(Int32.MaxValue, (0x7FEFFFFFL - 64L) / 4L);
            Equal(legacyX64, MidiFileParser.CalculateMaximumContiguousEventCount(8, false), "legacy x64 array byte limit");
            Equal(legacyX86, MidiFileParser.CalculateMaximumContiguousEventCount(4, true), "x86 remains on legacy array byte limit");
            Equal(0x7FEFFFFFL, MidiFileParser.CalculateMaximumContiguousEventCount(8, true), "configured x64 VLO element limit");
            long expected = IntPtr.Size == 8 && MidiFileParser.VeryLargeArraysConfigured() ? 0x7FEFFFFFL :
                Math.Min(Int32.MaxValue, (0x7FEFFFFFL - 64L) / IntPtr.Size);
            Equal(expected, maximum, "contiguous event limit follows architecture and runtime configuration");

            bool observedRejected = false;
            try { MidiFileParser.ValidateObservedContiguousEventCount(maximum + 1, 3, 9); }
            catch (InvalidDataException ex)
            {
                observedRejected = ex.Message.IndexOf("at least", StringComparison.OrdinalIgnoreCase) >= 0 &&
                    ex.Message.IndexOf("complete file total is not yet known", StringComparison.OrdinalIgnoreCase) >= 0;
            }
            Equal(true, observedRejected, "partial running count is not reported as a complete total");
        }

        private static void TestBackgroundMidiLoading()
        {
            string first = Path.Combine(Path.GetTempPath(), "midi-load-first-" + Guid.NewGuid().ToString("N") + ".mid");
            string second = Path.Combine(Path.GetTempPath(), "midi-load-second-" + Guid.NewGuid().ToString("N") + ".mid");
            string dense = null;
            File.WriteAllBytes(first, BuildTestMidi());
            File.WriteAllBytes(second, BuildTestMidi());
            try
            {
                Application.EnableVisualStyles();
                using (MainForm form = new MainForm())
                {
                    form.SuppressLoadErrorDialogs = true;
                    form.Show();
                    Application.DoEvents();
                    int idleHeight = form.Height;
                    List<Control> idleControls = new List<Control>();
                    CollectControls(form, idleControls);
                    GroupBox idleProcessing = FindGroupBox(idleControls, "Processing model");
                    GroupBox idlePlayback = FindGroupBox(idleControls, "Playback");
                    GroupBox idleStatistics = FindGroupBox(idleControls, "Statistics");
                    int processingTop = idleProcessing.Top;
                    int playbackTop = idlePlayback.Top;
                    int statisticsTop = idleStatistics.Top;
                    bool transientVerticalMovement = false;
                    form.Layout += delegate
                    {
                        if (idleProcessing.Top != processingTop || idlePlayback.Top != playbackTop || idleStatistics.Top != statisticsTop)
                            transientVerticalMovement = true;
                    };
                    form.BeginMidiLoad(first);
                    Equal("Cancel", form.OpenCommandText, "Open command becomes Cancel while loading");
                    Equal(true, form.LoadingActivityVisible, "loading activity is visible");
                    Equal(idleHeight, form.Height, "loading keeps the main window height stable");
                    Equal(processingTop, idleProcessing.Top, "loading does not move Processing model");
                    Equal(playbackTop, idlePlayback.Top, "loading does not move Playback");
                    Equal(statisticsTop, idleStatistics.Top, "loading does not move Statistics");
                    Equal(false, transientVerticalMovement, "loading start has no transient lower-group movement");
                    List<Control> loadingControls = new List<Control>();
                    CollectControls(form, loadingControls);
                    List<ProgressBar> loadingBars = new List<ProgressBar>();
                    for (int controlIndex = 0; controlIndex < loadingControls.Count; controlIndex++)
                    {
                        ProgressBar bar = loadingControls[controlIndex] as ProgressBar;
                        if (bar != null) loadingBars.Add(bar);
                    }
                    Equal(2, loadingBars.Count, "loading uses overall and stage progress bars");
                    if (loadingBars[0].Style != ProgressBarStyle.Continuous || loadingBars[1].Style != ProgressBarStyle.Continuous)
                        throw new Exception("loading progress remains indeterminate instead of reporting parser work");
                    if (Math.Max(loadingBars[0].Height, loadingBars[1].Height) <= Math.Min(loadingBars[0].Height, loadingBars[1].Height))
                        throw new Exception("stage progress bar is not the intended restrained half-height indicator");
                    Button cancelButton = FindButton(loadingControls, "Cancel");
                    CheckBox kdmApi = FindCheckBox(loadingControls, "KDMAPI");
                    Label loadingFile = null;
                    for (int controlIndex = 0; controlIndex < loadingControls.Count; controlIndex++)
                    {
                        Label label = loadingControls[controlIndex] as Label;
                        if (label != null && label.Text.StartsWith("Loading ", StringComparison.Ordinal)) loadingFile = label;
                    }
                    if (cancelButton == null || loadingFile == null || kdmApi == null) throw new Exception("loading controls were not found");
                    int cancelTop = form.PointToClient(cancelButton.PointToScreen(Point.Empty)).Y;
                    int fileTop = form.PointToClient(loadingFile.PointToScreen(Point.Empty)).Y;
                    int barsTop = Int32.MaxValue;
                    int barsBottom = Int32.MinValue;
                    for (int barIndex = 0; barIndex < loadingBars.Count; barIndex++)
                    {
                        Point barPoint = form.PointToClient(loadingBars[barIndex].PointToScreen(Point.Empty));
                        barsTop = Math.Min(barsTop, barPoint.Y);
                        barsBottom = Math.Max(barsBottom, barPoint.Y + loadingBars[barIndex].Height);
                    }
                    int kdmTop = form.PointToClient(kdmApi.PointToScreen(Point.Empty)).Y;
                    int kdmCenter = kdmTop + kdmApi.Height / 2;
                    int barsCenter = (barsTop + barsBottom) / 2;
                    if (Math.Abs(cancelTop - fileTop) > 8 || Math.Abs(kdmCenter - barsCenter) > 5)
                        throw new Exception("loading filename/status and output-row progress bars are not in their stable row footprints");
                    int kdmRight = form.PointToClient(kdmApi.PointToScreen(Point.Empty)).X + kdmApi.Width;
                    int barsLeft = Int32.MaxValue;
                    for (int barIndex = 0; barIndex < loadingBars.Count; barIndex++)
                        barsLeft = Math.Min(barsLeft, form.PointToClient(loadingBars[barIndex].PointToScreen(Point.Empty)).X);
                    if (barsLeft < kdmRight) throw new Exception("loading bars are not positioned beside the KDMAPI checkbox");
                    form.BeginMidiLoad(second);
                    PumpUntil(delegate { return !form.IsLoadingSong; }, 5000, "background MIDI load");
                    if (form.CurrentSong == null || !String.Equals(Path.GetFullPath(second), form.CurrentSong.FilePath, StringComparison.OrdinalIgnoreCase))
                        throw new Exception("a stale loading result replaced the newest selection");
                    Equal("Open MIDI...", form.OpenCommandText, "Open command restored after loading");
                    Equal(idleHeight, form.Height, "loading completion keeps the main window height stable");
                    Equal(processingTop, idleProcessing.Top, "loading completion does not move Processing model");
                    Equal(playbackTop, idlePlayback.Top, "loading completion does not move Playback");
                    Equal(statisticsTop, idleStatistics.Top, "loading completion does not move Statistics");

                    PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                    FakeMidiOutput attachedOutput = new FakeMidiOutput();
                    engine.Start(form.CurrentSong, attachedOutput, ProcessingMode.Queue, 0, true);
                    Equal(form.CurrentSong.FilePath, attachedOutput.SourceFile, "engine supplies one song-level source path to output diagnostics");
                    Equal(true, form.EngineHasAttachedSong, "engine attached current song before replacement");
                    form.UnloadCurrentSong();
                    Equal(false, form.EngineHasAttachedSong, "unload releases engine song");
                    Equal(false, form.EngineHasAttachedOutput, "unload releases engine output");
                    Equal(null, attachedOutput.SourceFile, "unload clears output source context");

                    form.BeginMidiLoad(first + ".missing");
                    PumpUntil(delegate { return !form.IsLoadingSong; }, 5000, "background load error");
                    if (form.LastLoadError == null || form.CurrentSong != null) throw new Exception("background load error did not leave a clean unloaded state");
                    Equal(processingTop, idleProcessing.Top, "failed loading does not move Processing model");

                    dense = CreateDenseMidiFile(300000);
                    form.BeginMidiLoad(dense);
                    Equal(processingTop, idleProcessing.Top, "progressing load does not move Processing model");
                    int lastOverall = -1;
                    bool sawStageMovement = false;
                    Stopwatch loadTimer = Stopwatch.StartNew();
                    while (form.IsLoadingSong && loadTimer.ElapsedMilliseconds < 10000)
                    {
                        Application.DoEvents();
                        int overall = form.LoadingOverallPermille;
                        int stage = form.LoadingStagePermille;
                        if (overall < lastOverall) throw new Exception("UI loading progress moved backwards");
                        if (stage > 0 && stage < 1000) sawStageMovement = true;
                        lastOverall = overall;
                        Thread.Sleep(1);
                    }
                    if (form.IsLoadingSong) throw new Exception("dense background progress load timed out");
                    Equal(true, sawStageMovement, "UI displays current-stage progress movement");
                    Equal(false, form.LoadingActivityVisible, "both loading bars hide after completion");
                    Equal(statisticsTop, idleStatistics.Top, "progress completion does not move Statistics");
                    form.UnloadCurrentSong();

                    form.BeginMidiLoad(dense);
                    form.CancelMidiLoad();
                    Equal(false, form.IsLoadingSong, "loading cancellation returns UI to idle");
                    Equal(null, form.CurrentSong, "cancelled load does not publish a partial song");
                    Equal(playbackTop, idlePlayback.Top, "loading cancellation does not move Playback");
                    form.Close();
                }

                using (MainForm closing = new MainForm())
                {
                    closing.Show(); Application.DoEvents();
                    closing.BeginMidiLoad(dense);
                    closing.Close();
                    Equal(true, closing.IsDisposed, "window closes while a load is pending");
                }
            }
            finally
            {
                DeleteFileWhenAvailable(first);
                DeleteFileWhenAvailable(second);
                if (dense != null) DeleteFileWhenAvailable(dense);
            }
        }

        private static void TestAsynchronousAnalysis()
        {
            MidiSong song = BuildSong(new long[] { 0, 0, 100000, 200000, 300000, 400000 });
            AnalysisConfiguration first = DefaultAnalysisConfiguration();
            AnalysisConfiguration second = DefaultAnalysisConfiguration();
            second.ProcessingMicroseconds = 250;
            WorkloadAnalysis sharedCompleted = null;
            using (DiagnosticsForm form = new DiagnosticsForm(song))
            {
                form.Show(); Application.DoEvents();
                form.RequestAnalysis(first);
                PumpUntil(delegate { return !form.CalculationPending; }, 5000, "initial asynchronous Analysis");
                WorkloadAnalysis completed = form.Graph.Analysis;
                if (completed == null) throw new Exception("asynchronous Analysis did not publish a completed result");
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                ComboBox resolution = FindComboContaining(controls, "Auto");
                if (resolution == null) throw new Exception("Analysis Resolution selector was not found");
                resolution.SelectedItem = "25 ms";
                PumpUntil(delegate { return form.ActiveResolutionMicroseconds == 25000 && !form.CalculationPending; }, 5000, "25 ms Analysis resolution");
                WorkloadAnalysis cached25 = form.Graph.Analysis;
                resolution.SelectedItem = "100 ms";
                PumpUntil(delegate { return form.ActiveResolutionMicroseconds == 100000 && !form.CalculationPending; }, 5000, "100 ms Analysis resolution");
                resolution.SelectedItem = "25 ms";
                PumpUntil(delegate { return Object.ReferenceEquals(cached25, form.Graph.Analysis); }, 5000, "Analysis resolution cache");
                completed = form.Graph.Analysis;
                form.RequestAnalysis(second);
                form.CancelAnalysisCalculation();
                Equal(completed, form.Graph.Analysis, "cancelled recalculation retains last completed graph");

                form.RequestAnalysis(first);
                form.RequestAnalysis(second);
                PumpUntil(delegate { return !form.CalculationPending; }, 5000, "stale Analysis replacement");
                Equal(250L, form.Graph.Analysis.Configuration.ProcessingMicroseconds, "stale Analysis result rejected");
                sharedCompleted = form.Graph.Analysis;
                form.Close();
            }
            using (DiagnosticsForm peer = new DiagnosticsForm(song))
            {
                peer.Show(); Application.DoEvents();
                List<Control> peerControls = new List<Control>();
                CollectControls(peer, peerControls);
                ComboBox peerResolution = FindComboContaining(peerControls, "Auto");
                peerResolution.SelectedItem = "25 ms";
                peer.RequestAnalysis(second);
                Application.DoEvents();
                Equal(false, peer.CalculationPending, "equivalent completed Analysis is shared across windows");
                Equal(true, Object.ReferenceEquals(sharedCompleted, peer.CurrentAnalysis), "shared Analysis reuses the completed immutable result");
                peer.Close();
            }
            using (DiagnosticsForm quick = new DiagnosticsForm(song, null,
                delegate(MidiSong source, long bucket, AnalysisConfiguration configuration, CancellationToken token,
                    Action<WorkloadAnalysisProgress> progress)
                {
                    if (token.WaitHandle.WaitOne(650)) token.ThrowIfCancellationRequested();
                    return WorkloadAnalyzer.Analyze(source, bucket, configuration, token, progress);
                }))
            {
                quick.Show(); Application.DoEvents();
                quick.RequestAnalysis(first);
                PumpUntil(delegate { return !quick.AnalysisBusy; }, 1500, "quick Analysis completion");
                Equal(false, quick.CalculationStatusVisible, "sub-second Analysis never flashes delayed status");
                quick.Close();
            }
            int calculationNumber = 0;
            using (DiagnosticsForm slow = new DiagnosticsForm(song, null,
                delegate(MidiSong source, long bucket, AnalysisConfiguration configuration, CancellationToken token,
                    Action<WorkloadAnalysisProgress> progress)
                {
                    int calculation = Interlocked.Increment(ref calculationNumber);
                    for (int step = 0; step <= 15; step++)
                    {
                        if (progress != null) progress(new WorkloadAnalysisProgress(
                            "Synthetic analysis " + calculation, step, 15, 0, 1000));
                        if (token.WaitHandle.WaitOne(100)) token.ThrowIfCancellationRequested();
                    }
                    return WorkloadAnalyzer.Analyze(source, bucket, configuration, token, progress);
                }))
            {
                slow.Show(); Application.DoEvents();
                slow.RequestAnalysis(first);
                PumpUntil(delegate { return slow.CalculationStatusVisible; }, 1600, "one-second delayed Analysis status");
                Equal(true, slow.CalculationProgressVisible, "delayed Analysis progress is visible");
                if (slow.CalculationProgressPermille <= 0) throw new Exception("delayed Analysis did not publish real progress");
                slow.Size = new Size(1600, 660); Application.DoEvents();
                Equal(1, slow.HeaderRowCount, "wide Analysis header uses one responsive line with busy controls");
                Equal(true, slow.HeaderControlsFit, "wide busy Analysis header controls fit");
                slow.Size = slow.MinimumSize; Application.DoEvents();
                if (slow.HeaderRowCount < 2) throw new Exception("minimum-width Analysis header did not wrap naturally");
                Equal(true, slow.HeaderControlsFit, "wrapped busy Analysis header controls fit");
                int firstProgress = slow.CalculationProgressPermille;
                slow.RequestAnalysis(second);
                Application.DoEvents();
                Equal(true, slow.CalculationStatusVisible, "superseding request keeps one continuous busy indicator");
                Equal(true, slow.AnalysisBusy, "superseding request remains in the same busy period");
                PumpFor(250);
                if (slow.CalculationProgressPermille < firstProgress)
                    throw new Exception("Analysis progress moved backwards across a superseding request");
                PumpUntil(delegate { return !slow.AnalysisBusy; }, 3000, "superseding Analysis completion");
                Equal(false, slow.CalculationStatusVisible, "accepted Analysis result hides busy status");
                Equal(250L, slow.CurrentAnalysis.Configuration.ProcessingMicroseconds, "latest Analysis generation wins");

                slow.RequestAnalysis(first);
                PumpUntil(delegate { return slow.CalculationStatusVisible; }, 1600, "delayed Analysis status before cancel");
                slow.CancelAnalysisCalculation();
                Equal(false, slow.CalculationStatusVisible, "cancel hides delayed Analysis status");
                Equal(false, slow.CalculationPending, "cancel retires Analysis request");
                slow.Close();
            }
            string customError;
            MidiSong customSong = BuildSong(new long[] { 0, 3000000 });
            using (DiagnosticsForm custom = new DiagnosticsForm(customSong))
            {
                custom.Show(); Application.DoEvents();
                custom.RequestAnalysis(first);
                PumpUntil(delegate { return !custom.AnalysisBusy; }, 5000, "initial custom-resolution Analysis");
                Equal(false, custom.SetCustomResolutionMilliseconds(0m, out customError), "custom Analysis resolution rejects zero");
                if (String.IsNullOrEmpty(customError)) throw new Exception("custom zero-resolution error was empty");
                Equal(false, custom.SetCustomResolutionMilliseconds(0.001m, out customError), "custom Analysis resolution rejects unsafe bucket count");
                if (customError.IndexOf("bucket", StringComparison.OrdinalIgnoreCase) < 0)
                    throw new Exception("unsafe custom-resolution error does not explain bucket allocation");
                Equal(true, custom.SetCustomResolutionMilliseconds(12.5m, out customError), "safe custom Analysis resolution accepted");
                PumpUntil(delegate { return !custom.AnalysisBusy; }, 5000, "custom Analysis resolution");
                Equal(12500L, custom.ActiveResolutionMicroseconds, "custom Analysis resolution applied exactly");
                custom.Close();
            }
            Equal(10000L, DiagnosticsForm.ChooseAutoResolution(3000000, 500, 0), "Auto selects fine resolution for short visible span");
            Equal(1000000L, DiagnosticsForm.ChooseAutoResolution(300000000, 500, 0), "Auto selects coarse resolution for long visible span");
            Equal(100000L, DiagnosticsForm.ChooseAutoResolution(40000000, 500, 100000), "Auto hysteresis preserves a stable current resolution");
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
                form.Show();
                Application.DoEvents();
                Equal(null, form.Owner, "Analysis window remains unowned");
                IntPtr handle = form.Handle;
                if (handle == IntPtr.Zero) throw new Exception("Analysis window handle is zero");
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                WorkloadGraph graph = FindControl<WorkloadGraph>(controls);
                RichTextBox summary = FindControl<RichTextBox>(controls);
                if (graph == null || summary == null) throw new Exception("Analysis controls were not constructed");
                SplitContainer split = form.AnalysisSplit;
                if (split == null || split.SplitterWidth < 4 || split.BackColor == split.Panel1.BackColor)
                    throw new Exception("Analysis splitter is not visibly distinct before interaction");
                if (split.Panel1.Padding.Left > 7 || split.Panel2.Padding.Right > 7)
                    throw new Exception("Analysis splitter retains excessive horizontal padding");
                if (!summary.Text.Contains("Scope                 Whole file")) throw new Exception("Analysis scope is not identified");
                if (!summary.Text.Contains("Graph resolution      100 ms")) throw new Exception("Analysis aggregation interval is not identified");
                if (!summary.Text.Contains("Predicted drops")) throw new Exception("Analysis prediction summary is absent");
                if (!summary.Text.Contains("Maximum rate")) throw new Exception("Analysis maximum rate is absent");
                if (!summary.Text.Contains("SMF format") || !summary.Text.Contains("PPQN") || !summary.Text.Contains("Musical note-ons"))
                    throw new Exception("Analysis MIDI-file metadata is incomplete");
                form.Size = new Size(1500, 660); Application.DoEvents();
                Equal(1, form.HeaderRowCount, "wide Analysis ordinary controls use one line");
                Equal(true, form.HeaderControlsFit, "wide Analysis ordinary controls fit");
                form.Size = form.MinimumSize; Application.DoEvents();
                if (form.HeaderRowCount < 2) throw new Exception("minimum Analysis header did not wrap");
                Equal(true, form.HeaderControlsFit, "minimum Analysis header wraps without clipping");
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

        private static void TestAnalysisInteraction()
        {
            MidiSong song = BuildSong(new long[] { 0, 25000, 50000, 75000, 100000 });
            song.FilePath = "analysis-interaction.mid";
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.QueueLengthLimitEnabled = true;
            configuration.QueueLengthLimit = 2;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 25000, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show();
                Application.DoEvents();
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                WorkloadGraph graph = FindControl<WorkloadGraph>(controls);
                if (graph == null) throw new Exception("interactive Analysis graph was not found");
                graph.Size = new Size(600, 400);
                Equal(0L, graph.TimeAtClientX(graph.GraphArea.Left), "Analysis x-to-time beginning");
                Near(song.DurationMicroseconds, graph.TimeAtClientX(graph.GraphArea.Right), 1, "Analysis x-to-time end");
                long edgeTime;
                int edgeY = graph.GraphArea.Top + graph.GraphArea.Height / 2;
                Equal(true, graph.TryGetPinTime(new Point(graph.GraphArea.Left - 3, edgeY), out edgeTime), "left pin tolerance accepted");
                Equal(0L, edgeTime, "left pin tolerance clamps to whole-file start");
                Equal(true, graph.TryGetPinTime(new Point(graph.GraphArea.Right + 3, edgeY), out edgeTime), "right pin tolerance accepted");
                Near(song.DurationMicroseconds, edgeTime, 1, "right pin tolerance clamps to whole-file end");
                Equal(false, graph.TryGetPinTime(new Point(graph.GraphArea.Left - 7, edgeY), out edgeTime), "outside left pin tolerance rejected");
                Equal(false, graph.TryGetPinTime(new Point(graph.GraphArea.Right + 7, edgeY), out edgeTime), "outside right pin tolerance rejected");
                int pinX = graph.GraphArea.Left - 3;
                IntPtr pinCoordinates = new IntPtr(((edgeY & 0xFFFF) << 16) | (pinX & 0xFFFF));
                SendMessage(graph.Handle, 0x0201, new IntPtr(1), pinCoordinates);
                SendMessage(graph.Handle, 0x0202, IntPtr.Zero, pinCoordinates);
                Equal((long?)0L, graph.PinnedTimeMicroseconds, "tolerated left-edge click creates an exact zero pin");
                graph.InspectAtClientX(graph.GraphArea.Left + graph.GraphArea.Width / 2, true);
                if (!graph.PinnedTimeMicroseconds.HasValue) throw new Exception("Analysis click did not pin inspection");
                long pin = graph.PinnedTimeMicroseconds.Value;
                graph.InspectAtClientX(graph.GraphArea.Left + graph.GraphArea.Width / 3, false);
                Equal(pin, graph.PinnedTimeMicroseconds.Value, "Analysis hover preserves pin");
                Equal(true, form.SeekToPinEnabled, "Seek to pin remains enabled after hover");

                int centerX = graph.GraphArea.Left + graph.GraphArea.Width / 2;
                long centerBefore = graph.TimeAtClientX(centerX);
                graph.ZoomAtClientX(centerX, 120);
                Near(centerBefore, graph.TimeAtClientX(centerX), 2, "zoom remains centered on cursor timestamp");
                if (graph.ViewEndMicroseconds - graph.ViewStartMicroseconds >= song.DurationMicroseconds)
                    throw new Exception("Analysis wheel zoom did not reduce viewport");
                Equal(true, graph.TryGetPinTime(new Point(graph.GraphArea.Left - 3, edgeY), out edgeTime), "zoomed left pin tolerance accepted");
                Equal(graph.ViewStartMicroseconds, edgeTime, "zoomed left pin clamps to viewport start");
                Equal(true, graph.TryGetPinTime(new Point(graph.GraphArea.Right + 3, edgeY), out edgeTime), "zoomed right pin tolerance accepted");
                Near(graph.ViewEndMicroseconds, edgeTime, 1, "zoomed right pin clamps to viewport end");
                graph.PanByPixels(100000);
                Equal(0L, graph.ViewStartMicroseconds, "Analysis pan clamps at beginning");
                graph.PanByPixels(-100000);
                Near(song.DurationMicroseconds, graph.ViewEndMicroseconds, 1, "Analysis pan clamps at end");
                graph.ResetZoom();
                Equal(0L, graph.ViewStartMicroseconds, "Analysis reset zoom start");
                Near(song.DurationMicroseconds, graph.ViewEndMicroseconds, 1, "Analysis reset zoom end");

                graph.ZoomAtClientX(centerX, 240);
                graph.FollowTarget = AnalysisFollowTarget.PlaybackTimeline;
                graph.SetPlaybackPositions(song.DurationMicroseconds, song.DurationMicroseconds / 4);
                if (graph.ViewEndMicroseconds < song.DurationMicroseconds)
                    throw new Exception("Analysis playback follow did not reveal end marker");
                graph.FollowTarget = AnalysisFollowTarget.MidiOutput;
                graph.SetPlaybackPositions(song.DurationMicroseconds, 0);
                Equal(0L, graph.ViewStartMicroseconds, "Analysis MIDI-output follow reveals beginning marker");

                bool seekRaised = false;
                form.SeekRequested += delegate { seekRaised = true; };
                Button seek = FindButton(controls, "Seek to pin");
                if (seek == null || !seek.Enabled) throw new Exception("Analysis seek action was not enabled for pinned selection");
                seek.PerformClick();
                Equal(true, seekRaised, "Analysis deliberate seek action");

                AnalysisConfiguration bitrate = DefaultAnalysisConfiguration();
                bitrate.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
                form.UpdateAnalysis(WorkloadAnalyzer.Analyze(song, 25000, bitrate));
                Equal(true, graph.UsesByteRate, "Analysis selected MIDI byte-rate graph");
                graph.PlaybackStatisticsVisible = true;
                graph.SetPlaybackOverlay(PlaybackState.Playing, new PlaybackOverlayData
                {
                    State = "Playing", TimelineAndOutput = "00:00.100 / 00:00.075", Queue = "2 / 5",
                    Events = "4 / 1", OutputRate = "40.0 events/sec", EffectiveSpeed = "75.0%", Lag = "25.000 ms / 30.000 ms"
                });
                Equal(true, graph.PlaybackStatisticsDrawn, "Analysis playback-statistics overlay visible while playing");
                graph.SetPlaybackOverlay(PlaybackState.Stopped, null);
                Equal(false, graph.PlaybackStatisticsDrawn, "Analysis playback-statistics overlay hidden while stopped");
                graph.FollowTarget = AnalysisFollowTarget.Off;
                graph.ResetZoom();
                using (Bitmap dynamicBitmap = new Bitmap(graph.Width, graph.Height))
                {
                    graph.DrawToBitmap(dynamicBitmap, graph.ClientRectangle);
                    int staticBuilds = graph.StaticLayerBuildCount;
                    for (int movement = 0; movement < 120; movement++)
                    {
                        graph.InspectAtClientX(graph.GraphArea.Left + movement % Math.Max(1, graph.GraphArea.Width), false);
                        graph.SetPlaybackPositions(movement * 500L, movement * 400L);
                        graph.DrawToBitmap(dynamicBitmap, graph.ClientRectangle);
                    }
                    Equal(staticBuilds, graph.StaticLayerBuildCount, "mouse inspection and live markers reuse static graph layer");
                    Equal((long?)59500L, graph.PlaybackTimelinePosition, "playback marker continues through inspection movement");
                    Equal((long?)47600L, graph.MidiOutputPosition, "output marker continues through inspection movement");
                }
                form.Close();
            }
        }

        [DllImport("user32.dll")]
        private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool PrintWindow(IntPtr window, IntPtr targetDeviceContext, uint flags);

        private static void CaptureForm(Form form, Bitmap bitmap)
        {
            bool captured = false;
            using (Graphics graphics = Graphics.FromImage(bitmap))
            {
                IntPtr deviceContext = graphics.GetHdc();
                try { captured = PrintWindow(form.Handle, deviceContext, 0); }
                finally { graphics.ReleaseHdc(deviceContext); }
            }
            if (!captured) form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size));
        }

        private static void TestAnalysisRenderingRefinements()
        {
            MidiSong song = BuildSong(new long[] { 0, 10000, 20000, 30000, 40000, 50000, 100000 });
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.QueueLengthLimitEnabled = true;
            configuration.QueueLengthLimit = 5;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 10000, configuration);
            using (Form host = new Form())
            using (WorkloadGraph graph = new WorkloadGraph())
            using (System.Windows.Forms.Timer playback = new System.Windows.Forms.Timer())
            {
                host.ClientSize = new Size(620, 440);
                graph.Dock = DockStyle.Fill;
                graph.Analysis = analysis;
                graph.PlaybackStatisticsVisible = true;
                graph.SetPlaybackOverlay(PlaybackState.Playing, new PlaybackOverlayData
                {
                    State = "Playing", TimelineAndOutput = "00:00.050 / 00:00.040", Queue = "2 / 5",
                    Events = "5 / 0", OutputRate = "100.0 events/sec", EffectiveSpeed = "100.0%", Lag = "1.000 ms / 2.000 ms"
                });
                host.Controls.Add(graph);
                host.Show(); Application.DoEvents();
                Rectangle area = graph.GraphArea;
                if (Math.Abs(area.Left - (graph.ClientSize.Width - area.Right)) > 1)
                    throw new Exception("Analysis plot is not horizontally centered");
                if (area.Top < 8 || graph.ClientSize.Height - area.Bottom < 50)
                    throw new Exception("Analysis plot outer insets are unbalanced");
                Rectangle overlay = graph.PlaybackStatisticsBounds(area);
                if (overlay.Left != area.Left + 7 || overlay.Top != area.Top + 7)
                    throw new Exception("playback statistics overlay is not anchored at plot top-left");

                long marker = 0;
                playback.Interval = 10;
                playback.Tick += delegate { marker += 1000; graph.SetPlaybackPositions(marker, Math.Max(0, marker - 500)); };
                int paintsBefore = graph.DynamicPaintCount;
                playback.Start();
                Stopwatch movement = Stopwatch.StartNew();
                while (movement.ElapsedMilliseconds < 450)
                {
                    int x = area.Left + (int)(movement.ElapsedMilliseconds % Math.Max(1, area.Width));
                    int y = area.Top + area.Height / 2;
                    SendMessage(graph.Handle, 0x0200, IntPtr.Zero, new IntPtr((y << 16) | (x & 0xFFFF)));
                    Application.DoEvents();
                }
                playback.Stop();
                if (graph.DynamicPaintCount - paintsBefore < 5) throw new Exception("continuous mouse inspection starved graph painting");
                if (!graph.PlaybackTimelinePosition.HasValue || graph.PlaybackTimelinePosition.Value < 10000)
                    throw new Exception("playback marker stalled during continuous mouse inspection");

                host.ClientSize = new Size(900, 600); Application.DoEvents();
                Rectangle largeArea = graph.GraphArea;
                if (Math.Abs(largeArea.Left - (graph.ClientSize.Width - largeArea.Right)) > 1)
                    throw new Exception("large Analysis plot is not horizontally centered");
                host.Close();
            }
        }

        private static void TestAnalysisSeekPlaybackPolicy()
        {
            Application.EnableVisualStyles();
            MidiSong song = BuildSong(new long[] { 0, 500000, 1000000, 1500000 });
            using (MainForm form = new MainForm())
            {
                Type type = typeof(MainForm);
                PlaybackEngine engine = (PlaybackEngine)type.GetField("_engine", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                type.GetField("_song", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).SetValue(form, song);
                type.GetField("_engineSong", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).SetValue(form, song);
                engine.SimulateSlowdown = false;
                FakeMidiOutput output = new FakeMidiOutput();
                engine.Start(song, output, ProcessingMode.Queue, 0, true);
                Equal(PlaybackState.Paused, engine.State, "test playback begins paused");
                type.GetMethod("PerformAnalysisSeek", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).Invoke(form, new object[] { 750000L });
                Equal(PlaybackState.Playing, engine.State, "Analysis seek resumes paused playback");
                engine.Stop();
                type.GetMethod("PerformAnalysisSeek", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).Invoke(form, new object[] { 250000L });
                Equal(PlaybackState.Stopped, engine.State, "Analysis seek leaves stopped playback stopped");
            }
        }

        private static void TestLagFormatting()
        {
            Equal("0.000 ms", MainForm.FormatLagMilliseconds(0), "zero lag format");
            Equal("43.137 ms", MainForm.FormatLagMilliseconds(43137), "nonzero lag format");
        }

        private static void TestQueuePressureView()
        {
            using (StatisticsView statistics = new StatisticsView())
            {
                statistics.Size = new Size(700, 104);
                statistics.SetQueuePressure(false, 0, 2000, false);
                Equal(false, statistics.QueuePressureVisible, "unlimited queue hides pressure meter");
                statistics.SetQueuePressure(true, 1463, 2000, false);
                Equal(true, statistics.QueuePressureVisible, "finite queue shows pressure meter");
                Near(0.7315, statistics.QueuePressureRatio, 0.0001, "queue pressure ratio includes outstanding slot count");
                using (Bitmap bitmap = new Bitmap(statistics.Width, statistics.Height))
                    statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
            }
        }

        private static void TestStatisticsWidthRelease()
        {
            using (StatisticsView statistics = new StatisticsView())
            using (Bitmap bitmap = new Bitmap(900, 130))
            {
                statistics.Size = bitmap.Size;
                string[] longValues = new string[]
                {
                    "99:59.999 / 99:59.999", "1,097,842 / 1,500,000", "1,000,000.0 events/sec", "999,999.9 events/sec",
                    "10,385,604 / 10,000,000", "1,234.5%", "999,999.999 ms", "999,999.999 ms"
                };
                statistics.SetValues(longValues);
                statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
                int longQueue = statistics.ValueAllocationWidth(1);
                string[] shortValues = new string[] { "0 / 0", "0 / 0", "—", "—", "0 / 0", "—", "0.000 ms", "0.000 ms" };
                statistics.SetValues(shortValues);
                statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
                if (statistics.ValueAllocationWidth(1) >= longQueue)
                    throw new Exception("a former long queue value retained stale horizontal width");
            }
        }

        private static void TestBenchmarkWindowExtraction()
        {
            MidiSong source = BuildSong(new long[] { 122999999, 123000000, 123050000, 124000000, 125999999, 126000000 });
            MidiSong sample = ExtractBenchmarkWindow(source, 123000000, 126000000);
            Equal(4, sample.Events.Count, "benchmark window includes every event without a cap");
            Sequence(new long[] { 0, 50000, 1000000, 2999999 }, new List<long>
            {
                sample.Events[0].IntendedMicroseconds, sample.Events[1].IntendedMicroseconds,
                sample.Events[2].IntendedMicroseconds, sample.Events[3].IntendedMicroseconds
            }, "benchmark preserves relative timestamps");
            Equal(3000000L, sample.DurationMicroseconds, "benchmark covers full three-second interval");
        }

        private static void TestImmediateBurstPerformance()
        {
            const int count = 300000;
            MidiSong song = BuildSong(new long[count]);
            CountingMidiOutput output = new CountingMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = false;
                Stopwatch elapsed = Stopwatch.StartNew();
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State != PlaybackState.Playing; }, 4000, "immediate burst");
                elapsed.Stop();
                Equal((long)count, output.Count, "immediate burst sends every event");
                if (elapsed.ElapsedMilliseconds > 2000) throw new Exception("immediate burst took " + elapsed.ElapsedMilliseconds + " ms");
            }
        }

        private static void TestSlowdownBurstUiResponsiveness()
        {
            const int count = 60000;
            MidiSong song = BuildSong(new long[count]);
            CountingMidiOutput output = new CountingMidiOutput();
            using (Form form = new Form())
            using (System.Windows.Forms.Timer pulse = new System.Windows.Forms.Timer())
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                int pulses = 0;
                pulse.Interval = 16;
                pulse.Tick += delegate { pulses++; };
                form.Show(); Application.DoEvents();
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 10;
                pulse.Start();
                engine.Start(song, output, ProcessingMode.Queue);
                Stopwatch elapsed = Stopwatch.StartNew();
                while (engine.State == PlaybackState.Playing && elapsed.ElapsedMilliseconds < 5000)
                {
                    Application.DoEvents();
                    Thread.Sleep(1);
                }
                pulse.Stop();
                if (engine.State == PlaybackState.Playing) throw new Exception("simulated-slowdown responsiveness run timed out");
                if (pulses < 10) throw new Exception("scheduler starved the WinForms message pump (only " + pulses + " timer pulses)");
                Equal((long)count, output.Count, "simulated-slowdown burst output count");
                form.Close();
            }
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
                output.Send(SysExEvent(0xF0, new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 }));
                Console.WriteLine("Sent; resetting");
                MidiOutputSafety.ResetAndSilence(output);
                Console.WriteLine("Reset complete");
            }
            Console.WriteLine("Close complete");
        }

        private static void ProbeFileSystemExclusive(uint deviceId, string path, bool kdmApi)
        {
            MidiSong song = MidiFileParser.Load(path);
            IMidiOutput output;
            IDisposable disposable;
            if (kdmApi)
            {
                KdmApiMidiOutput kdm = new KdmApiMidiOutput();
                kdm.Open();
                output = kdm;
                disposable = kdm;
            }
            else
            {
                WindowsMidiOutput winmm = new WindowsMidiOutput();
                winmm.Open(deviceId);
                output = winmm;
                disposable = winmm;
            }
            IMidiOutputContext context = output as IMidiOutputContext;
            if (context != null) context.SourceFile = song.FilePath;
            int fragments = 0;
            try
            {
                for (int i = 0; i < song.Events.Count; i++)
                {
                    if (song.Events[i].Kind != MidiEventKind.SystemExclusive) continue;
                    output.Send(song.Events[i]);
                    fragments++;
                }
                MidiOutputSafety.ResetAndSilence(output);
            }
            finally { disposable.Dispose(); }
            Console.WriteLine((kdmApi ? "KDMAPI" : "WinMM") + " accepted " + fragments +
                " SysEx fragment(s) from " + Path.GetFileName(path));
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

        private static void BenchmarkBoundedFile(string path)
        {
            Stopwatch parse = Stopwatch.StartNew();
            MidiSong song = MidiFileParser.Load(path);
            parse.Stop();
            for (int i = 0; i < song.Events.Count; i++) song.Events[i].IntendedMicroseconds = 0;
            song.DurationMicroseconds = 0;
            Console.WriteLine("Parsed " + song.Events.Count.ToString("N0") + " events in " + parse.ElapsedMilliseconds + " ms");
            BenchmarkBoundedPass(song, false, "zero-service bounded throughput");
            BenchmarkBoundedPass(song, true, "1-us bounded overflow throughput");
        }

        private static void BenchmarkOutputWindow(string path, string backendName)
        {
            BenchmarkOutputWindow(path, backendName, 123000000, 126000000);
        }

        private static void BenchmarkOutputWindow(string path, string backendName, long windowStart, long windowEnd)
        {
            if (windowStart < 0 || windowEnd <= windowStart) throw new ArgumentOutOfRangeException("windowEnd", "Benchmark end must be after a nonnegative start.");
            Stopwatch load = Stopwatch.StartNew();
            MidiSong source = MidiFileParser.Load(path);
            load.Stop();
            Console.WriteLine("Loaded {0:N0} source events in {1:N0} ms", source.Events.Count, load.ElapsedMilliseconds);
            MidiSong sample = ExtractBenchmarkWindow(source, windowStart, windowEnd);
            long bucketCount = (windowEnd - windowStart + 99999) / 100000;
            if (bucketCount <= 0 || bucketCount > 1000000) throw new ArgumentOutOfRangeException("windowEnd", "Benchmark range creates an unsafe distribution size.");
            int[] distribution = new int[(int)bucketCount];
            for (int i = 0; i < sample.Events.Count; i++)
            {
                int bucket = (int)Math.Min(distribution.Length - 1, Math.Max(0, sample.Events[i].IntendedMicroseconds / 100000));
                distribution[bucket]++;
            }
            Console.WriteLine("Window {0}–{1}: {2:N0} events (uncapped)", FormatBenchmarkTime(windowStart),
                FormatBenchmarkTime(windowEnd), sample.Events.Count);
            for (int i = 0; i < distribution.Length; i++)
                Console.WriteLine("  {0}–{1}: {2:N0}", FormatBenchmarkTime(windowStart + i * 100000L),
                    FormatBenchmarkTime(Math.Min(windowEnd, windowStart + (i + 1) * 100000L)), distribution[i]);

            IMidiOutput realOutput = null;
            IDisposable disposable = null;
            if (String.Equals(backendName, "null", StringComparison.OrdinalIgnoreCase))
                realOutput = new CountingMidiOutput();
            else if (String.Equals(backendName, "kdmapi", StringComparison.OrdinalIgnoreCase))
            {
                KdmApiMidiOutput kdm = new KdmApiMidiOutput();
                kdm.Open();
                realOutput = kdm;
                disposable = kdm;
            }
            else if (String.Equals(backendName, "winmm", StringComparison.OrdinalIgnoreCase))
            {
                List<MidiOutputDeviceInfo> devices = WindowsMidiOutput.GetDevices();
                MidiOutputDeviceInfo omni = null;
                for (int i = 0; i < devices.Count; i++)
                    if (devices[i].Name.IndexOf("OmniMIDI", StringComparison.OrdinalIgnoreCase) >= 0) { omni = devices[i]; break; }
                if (omni == null) throw new Exception("OmniMIDI WinMM output was not found for the bounded benchmark.");
                WindowsMidiOutput winmm = new WindowsMidiOutput();
                winmm.Open(omni.DeviceId);
                realOutput = winmm;
                disposable = winmm;
            }
            else throw new ArgumentException("Backend must be null, kdmapi, or winmm.");

            List<double> totals = new List<double>();
            List<double> outputs = new List<double>();
            List<double> engines = new List<double>();
            List<double> processorCpu = new List<double>();
            List<long> maximumLags = new List<long>();
            try
            {
                Stopwatch warmup = Stopwatch.StartNew();
                MidiOutputSafety.ResetAndSilence(realOutput);
                warmup.Stop();
                Console.WriteLine("Output warm-up reset/panic: {0:N1} ms", warmup.Elapsed.TotalMilliseconds);
                for (int run = 0; run < 3; run++)
                {
                    TimedMidiOutput timed = new TimedMidiOutput(realOutput);
                    long[] lagByBucket = new long[distribution.Length];
                    using (PlaybackEngine engine = new PlaybackEngine())
                    {
                        engine.SimulateSlowdown = false;
                        TimeSpan cpuStart = Process.GetCurrentProcess().TotalProcessorTime;
                        Stopwatch elapsed = Stopwatch.StartNew();
                        engine.Start(sample, timed, ProcessingMode.Queue);
                        while (engine.State == PlaybackState.Playing && elapsed.ElapsedMilliseconds < 15000)
                        {
                            PlaybackSnapshot live = engine.GetSnapshot();
                            int bucket = (int)Math.Min(lagByBucket.Length - 1, Math.Max(0, live.IntendedTimelineMicroseconds / 100000));
                            lagByBucket[bucket] = Math.Max(lagByBucket[bucket], live.CurrentLagMicroseconds);
                            Thread.Sleep(2);
                        }
                        if (engine.State == PlaybackState.Playing) throw new Exception(backendName + " output benchmark exceeded 15 seconds");
                        elapsed.Stop();
                        PlaybackSnapshot result = engine.GetSnapshot();
                        double outputMilliseconds = timed.SendTicks * 1000.0 / Stopwatch.Frequency;
                        double engineMilliseconds = Math.Max(0, elapsed.Elapsed.TotalMilliseconds - outputMilliseconds);
                        double cpuMilliseconds = Math.Max(0, (Process.GetCurrentProcess().TotalProcessorTime - cpuStart).TotalMilliseconds - outputMilliseconds);
                        totals.Add(elapsed.Elapsed.TotalMilliseconds);
                        outputs.Add(outputMilliseconds);
                        engines.Add(engineMilliseconds);
                        processorCpu.Add(cpuMilliseconds);
                        maximumLags.Add(result.MaximumLagMicroseconds);
                        Console.WriteLine("Run {0}: total {1:N1} ms; max dispatch lag {2:N3} ms; output calls {3:N1} ms; scheduled wall outside output {4:N1} ms; non-output process CPU {5:N1} ms",
                            run + 1, elapsed.Elapsed.TotalMilliseconds, result.MaximumLagMicroseconds / 1000.0, outputMilliseconds, engineMilliseconds, cpuMilliseconds);
                        Console.Write("  Lag samples by 100 ms bucket (µs):");
                        for (int i = 0; i < lagByBucket.Length; i++) Console.Write(" " + lagByBucket[i].ToString());
                        Console.WriteLine();
                    }
                }
                totals.Sort(); outputs.Sort(); engines.Sort(); processorCpu.Sort(); maximumLags.Sort();
                Console.WriteLine("{0} median (range): total {1:N1} ms ({2:N1}–{3:N1}); output {4:N1} ms ({5:N1}–{6:N1}); scheduled wall outside output {7:N1} ms ({8:N1}–{9:N1}); non-output CPU {10:N1} ms ({11:N1}–{12:N1}); max lag {13:N3} ms ({14:N3}–{15:N3})",
                    backendName, totals[1], totals[0], totals[2], outputs[1], outputs[0], outputs[2], engines[1], engines[0], engines[2],
                    processorCpu[1], processorCpu[0], processorCpu[2], maximumLags[1] / 1000.0, maximumLags[0] / 1000.0, maximumLags[2] / 1000.0);
                if (String.Equals(backendName, "null", StringComparison.OrdinalIgnoreCase))
                    BenchmarkUninstrumentedNull(sample);
            }
            finally
            {
                if (disposable != null) disposable.Dispose();
            }
        }

        private static string FormatBenchmarkTime(long microseconds)
        {
            TimeSpan time = TimeSpan.FromTicks(Math.Max(0, microseconds) * 10L);
            return String.Format(CultureInfo.InvariantCulture, "{0:00}:{1:00}.{2:000}", (int)time.TotalMinutes, time.Seconds, time.Milliseconds);
        }

        private static void BenchmarkUninstrumentedNull(MidiSong sample)
        {
            List<double> totals = new List<double>();
            List<long> lags = new List<long>();
            for (int run = 0; run < 3; run++)
            {
                CountingMidiOutput output = new CountingMidiOutput();
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.SimulateSlowdown = false;
                    Stopwatch elapsed = Stopwatch.StartNew();
                    engine.Start(sample, output, ProcessingMode.Queue);
                    WaitFor(delegate { return engine.State != PlaybackState.Playing; }, 15000, "uninstrumented null benchmark");
                    elapsed.Stop();
                    PlaybackSnapshot snapshot = engine.GetSnapshot();
                    totals.Add(elapsed.Elapsed.TotalMilliseconds);
                    lags.Add(snapshot.MaximumLagMicroseconds);
                }
            }
            totals.Sort(); lags.Sort();
            Console.WriteLine("Uninstrumented null, UI suppressed: total {0:N1} ms ({1:N1}–{2:N1}); max lag {3:N3} ms ({4:N3}–{5:N3})",
                totals[1], totals[0], totals[2], lags[1] / 1000.0, lags[0] / 1000.0, lags[2] / 1000.0);
        }

        private static void BenchmarkSequentialLoadMemory(string path)
        {
            long baseline = GC.GetTotalMemory(true);
            Stopwatch firstTimer = Stopwatch.StartNew();
            MidiSong first = MidiFileParser.Load(path);
            firstTimer.Stop();
            long firstLive = GC.GetTotalMemory(false);
            WeakReference firstReference = new WeakReference(first);
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.Start(first, new CountingMidiOutput(), ProcessingMode.Queue, 0, true);
                engine.Unload();
                first = null;
                GC.GetTotalMemory(true);
                Console.WriteLine("After explicit engine unload: attachedSong={0}, attachedOutput={1}, oldSongAlive={2}",
                    engine.HasAttachedSong, engine.HasAttachedOutput, firstReference.IsAlive);
            }
            long afterUnload = GC.GetTotalMemory(true);
            Stopwatch secondTimer = Stopwatch.StartNew();
            MidiSong second = MidiFileParser.Load(path);
            secondTimer.Stop();
            long secondLive = GC.GetTotalMemory(false);
            WeakReference secondReference = new WeakReference(second);
            second = null;
            long afterSecondUnload = GC.GetTotalMemory(true);
            Console.WriteLine("Managed live memory: baseline {0:N0}; first loaded {1:N0}; after unload {2:N0}; second loaded {3:N0}; after second unload {4:N0} bytes",
                baseline, firstLive, afterUnload, secondLive, afterSecondUnload);
            Console.WriteLine("Load times: first {0:N0} ms; second {1:N0} ms; secondSongAliveAfterGC={2}",
                firstTimer.ElapsedMilliseconds, secondTimer.ElapsedMilliseconds, secondReference.IsAlive);
        }

        private static void BenchmarkOutputWindowWithUi(string path)
        {
            MidiSong source = MidiFileParser.Load(path);
            MidiSong sample = ExtractBenchmarkWindow(source, 123000000, 126000000);
            List<double> totals = new List<double>();
            List<long> lags = new List<long>();
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); Application.DoEvents();
                PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                typeof(MainForm).GetField("_song", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).SetValue(form, sample);
                typeof(MainForm).GetField("_engineSong", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).SetValue(form, sample);
                for (int run = 0; run < 3; run++)
                {
                    CountingMidiOutput output = new CountingMidiOutput();
                    engine.SimulateSlowdown = false;
                    Stopwatch elapsed = Stopwatch.StartNew();
                    engine.Start(sample, output, ProcessingMode.Queue);
                    while (engine.State == PlaybackState.Playing && elapsed.ElapsedMilliseconds < 15000)
                    {
                        Application.DoEvents();
                        Thread.Sleep(1);
                    }
                    if (engine.State == PlaybackState.Playing) throw new Exception("normal-UI benchmark exceeded 15 seconds");
                    elapsed.Stop();
                    PlaybackSnapshot snapshot = engine.GetSnapshot();
                    totals.Add(elapsed.Elapsed.TotalMilliseconds);
                    lags.Add(snapshot.MaximumLagMicroseconds);
                    Console.WriteLine("Normal UI run {0}: total {1:N1} ms; max dispatch lag {2:N3} ms; sent {3:N0}",
                        run + 1, elapsed.Elapsed.TotalMilliseconds, snapshot.MaximumLagMicroseconds / 1000.0, output.Count);
                }
                form.Close();
            }
            totals.Sort(); lags.Sort();
            Console.WriteLine("Normal UI median (range): total {0:N1} ms ({1:N1}–{2:N1}); max lag {3:N3} ms ({4:N3}–{5:N3})",
                totals[1], totals[0], totals[2], lags[1] / 1000.0, lags[0] / 1000.0, lags[2] / 1000.0);
        }

        private static void BenchmarkAsyncUiLoad(string path)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            using (System.Windows.Forms.Timer pulse = new System.Windows.Forms.Timer())
            {
                int pulses = 0;
                long last = 0;
                long maximumGap = 0;
                Stopwatch elapsed = Stopwatch.StartNew();
                pulse.Interval = 16;
                pulse.Tick += delegate
                {
                    long now = elapsed.ElapsedMilliseconds;
                    if (last > 0) maximumGap = Math.Max(maximumGap, now - last);
                    last = now;
                    pulses++;
                };
                form.Show(); Application.DoEvents();
                pulse.Start();
                form.BeginMidiLoad(path);
                PumpUntil(delegate { return !form.IsLoadingSong; }, 30000, "asynchronous large-file UI load");
                elapsed.Stop(); pulse.Stop();
                if (form.CurrentSong == null) throw new Exception("asynchronous large-file load failed");
                Console.WriteLine("Async UI load: {0:N0} events in {1:N0} ms; UI pulses={2:N0}; maximum timer gap={3:N0} ms",
                    form.CurrentSong.Events.Count, elapsed.ElapsedMilliseconds, pulses, maximumGap);
                form.UnloadCurrentSong();
                form.Close();
            }
        }

        private static void BenchmarkAnalysis(string path)
        {
            MidiSong song = MidiFileParser.Load(path);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            long[] resolutions = new long[] { 1000000, 100000, 10000 };
            for (int i = 0; i < resolutions.Length; i++)
            {
                Stopwatch timer = Stopwatch.StartNew();
                WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, resolutions[i], configuration);
                timer.Stop();
                Console.WriteLine("Analysis {0:N0} µs: {1:N0} buckets, {2:N0} events, {3:N0} ms",
                    resolutions[i], analysis.Buckets.Length, analysis.TotalEvents, timer.ElapsedMilliseconds);
            }
        }

        private static MidiSong ExtractBenchmarkWindow(MidiSong source, long windowStart, long windowEnd)
        {
            int first = 0;
            int last = source.Events.Count;
            while (first < last)
            {
                int middle = first + (last - first) / 2;
                if (source.Events[middle].IntendedMicroseconds < windowStart) first = middle + 1; else last = middle;
            }
            MidiSong sample = new MidiSong();
            sample.FilePath = source.FilePath;
            sample.Format = source.Format;
            sample.TrackCount = source.TrackCount;
            sample.TicksPerQuarterNote = source.TicksPerQuarterNote;
            sample.Events = new List<MidiEvent>();
            for (int index = first; index < source.Events.Count; index++)
            {
                MidiEvent original = source.Events[index];
                if (original.IntendedMicroseconds >= windowEnd) break;
                sample.Events.Add(new MidiEvent
                {
                    AbsoluteTick = original.AbsoluteTick, IntendedMicroseconds = original.IntendedMicroseconds - windowStart, Track = original.Track,
                    Order = original.Order, Kind = original.Kind, Channel = original.Channel, Status = original.Status,
                    Data = original.Data, EventIndex = original.EventIndex
                });
            }
            sample.DurationMicroseconds = Math.Max(0, windowEnd - windowStart);
            return sample;
        }

        private static void BenchmarkBoundedPass(MidiSong song, bool slowdown, string label)
        {
            CountingMidiOutput output = new CountingMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = slowdown;
                engine.ProcessingMicroseconds = 1;
                engine.QueueLengthLimit = 2000;
                engine.OverflowPolicy = OverflowPolicy.DropNewest;
                Stopwatch elapsed = Stopwatch.StartNew();
                engine.Start(song, output, ProcessingMode.Drop);
                while (engine.State == PlaybackState.Playing) Thread.Sleep(1);
                elapsed.Stop();
                PlaybackSnapshot result = engine.GetSnapshot();
                Console.WriteLine(label + ": " + elapsed.ElapsedMilliseconds + " ms, sent=" +
                    result.ProcessedEvents.ToString("N0") + ", dropped=" + result.DroppedEvents.ToString("N0"));
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
                form.Show();
                Application.DoEvents();
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
                string[] values = new string[8];
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
                ComboBox serviceMode = FindComboContaining(controls, "MIDI serial bitrate");
                ComboBox overflow = FindComboContaining(controls, "Clear buffer and jump to realtime");
                ComboBox midiOutput = FindMidiOutputCombo(controls);
                Button dinPreset = FindButton(controls, "5-pin DIN");
                NumericUpDown queueLimitValue = FindNumericWithValue(controls, 2000m);
                if (slowdown == null || queueLimit == null || kdmApi == null || serviceMode == null || overflow == null || dinPreset == null || queueLimitValue == null)
                    throw new Exception("independent processing policy controls were not found");
                Equal(560, form.MinimumSize.Width, "normal-layout minimum width");
                Equal(form.RealizedRequiredWindowHeight, form.MinimumSize.Height,
                    "normal-layout minimum height follows realized content");
                if (form.MinimumSize.Height >= 565)
                    throw new Exception("normal minimum height was not reduced from its obsolete fixed value: " + form.MinimumSize.Height);
                int defaultMinimumHeight = form.MinimumSize.Height;
                GroupBox processingGroup = FindGroupBox(controls, "Processing model");
                int unlimitedProcessingHeight = processingGroup == null ? 0 : processingGroup.Height;
                queueLimit.Checked = true;
                Application.DoEvents();
                if (processingGroup != null) Equal(unlimitedProcessingHeight, processingGroup.Height, "queue toggle preserves processing group height");
                queueLimit.Checked = false;
                Application.DoEvents();
                if (processingGroup != null) Equal(unlimitedProcessingHeight, processingGroup.Height, "queue untoggle restores processing group height");

                form.Size = new Size(620, 590);
                Application.DoEvents();
                Equal(true, statistics.Compact, "compact layout breakpoint");
                Equal(500, form.MinimumSize.Width, "compact-layout minimum width");
                Equal(form.RealizedRequiredWindowHeight, form.MinimumSize.Height,
                    "compact minimum height follows realized content");
                int compactMinimumHeight = form.MinimumSize.Height;
                if (compactMinimumHeight >= 470)
                    throw new Exception("compact minimum height was not reduced from its obsolete fixed value: " + compactMinimumHeight);
                Equal(compactMinimumHeight, form.MaximumSize.Height, "compact height cap");
                Equal(2, statistics.ColumnCount, "compact statistics remain two columns");
                int[] compactWidths = new int[] { 620, 580, 540, 500 };
                for (int widthIndex = 0; widthIndex < compactWidths.Length; widthIndex++)
                {
                    form.Size = new Size(compactWidths[widthIndex], compactMinimumHeight);
                    Application.DoEvents();
                    AssertProcessingClusters(form, "active compact resize at " + compactWidths[widthIndex] + " pixels");
                }
                form.Size = form.MinimumSize;
                Application.DoEvents();
                AssertProcessingClusters(form, "compact processing-time minimum");
                if (statistics.Bottom > form.ClientSize.Height) throw new Exception("compact statistics are clipped");
                GroupBox compactStatisticsGroup = FindGroupBox(controls, "Statistics");
                if (compactStatisticsGroup == null || statistics.Bottom > compactStatisticsGroup.ClientSize.Height)
                    throw new Exception("compact Statistics group clips its custom-painted surface");
                Button compactAnalysis = FindButton(controls, "Analysis...");
                Button compactReset = FindButton(controls, "Reset stats");
                if (compactAnalysis == null || compactReset == null || compactAnalysis.Right > compactReset.Left)
                    throw new Exception("compact playback controls overlap");
                if (compactAnalysis.Right > form.ClientSize.Width || compactReset.Right > form.ClientSize.Width)
                    throw new Exception("compact playback controls are clipped");
                NumericUpDown compactQueue = FindNumericWithMaximum(controls, 1000000m);
                if (compactQueue == null) throw new Exception("compact queue-limit numeric field was not found");
                compactQueue.Value = compactQueue.Maximum;
                AssertNumericFullyVisible(compactQueue, "compact maximum queue limit");
                compactQueue.Value = 2000m;
                AssertComboFullyVisible(serviceMode, "Processing time per event", "compact Rate model");
                AssertComboFullyVisible(overflow, Convert.ToString(overflow.SelectedItem), "compact overflow policy");
                serviceMode.SelectedIndex = 1; Application.DoEvents();
                NumericUpDown compactBitrate = FindNumericWithMaximum(controls, 100000000m);
                if (compactBitrate == null) throw new Exception("compact bitrate numeric field was not found");
                compactBitrate.Value = compactBitrate.Maximum;
                AssertNumericFullyVisible(compactBitrate, "compact maximum MIDI bitrate");
                AssertProcessingClusters(form, "compact bitrate minimum");
                Equal(false, dinPreset.Visible, "5-pin DIN preset hidden only in compact layout");
                int compactStatisticsHeight = statistics.Height;
                queueLimit.Checked = true; Application.DoEvents();
                Equal(compactStatisticsHeight, statistics.Height, "queue-pressure meter does not grow compact layout");
                Equal(true, statistics.QueuePressureVisible, "finite queue pressure visible at compact minimum");
                if (statistics.Bottom > form.ClientSize.Height) throw new Exception("finite compact statistics are clipped");
                queueLimit.Checked = false; Application.DoEvents();
                form.Size = new Size(790, 660);
                Application.DoEvents();
                Equal(false, statistics.Compact, "normal layout restored across breakpoint");
                Equal(104, statistics.Height, "normal statistics height restored across breakpoint");
                GroupBox restoredStatisticsGroup = FindGroupBox(controls, "Statistics");
                if (restoredStatisticsGroup == null || statistics.Bottom > restoredStatisticsGroup.ClientSize.Height)
                    throw new Exception("restored Statistics group clips its custom-painted surface");
                Point restoredStatistics = form.PointToClient(statistics.PointToScreen(Point.Empty));
                if (restoredStatistics.Y + statistics.Height > form.ClientSize.Height)
                    throw new Exception("restored normal statistics are clipped: top=" + restoredStatistics.Y +
                        ", height=" + statistics.Height + ", client=" + form.ClientSize.Height);
                Equal(560, form.MinimumSize.Width, "normal minimum width restored");
                Equal(form.RealizedRequiredWindowHeight, form.MinimumSize.Height, "normal content-derived minimum restored");
                Equal(defaultMinimumHeight, form.MinimumSize.Height, "normal minimum height restoration is stable");
                Equal(Size.Empty, form.MaximumSize, "normal layout removes compact height cap");
                Equal(true, dinPreset.Visible, "5-pin DIN preset restored on return to default bitrate layout");
                Equal(100000000m, compactBitrate.Value, "responsive transition preserves configured bitrate");
                AssertProcessingClusters(form, "restored default bitrate");
                int[] activeWidths = new int[] { 640, 660, 700, 740, 790 };
                for (int widthIndex = 0; widthIndex < activeWidths.Length; widthIndex++)
                {
                    form.ClientSize = new Size(activeWidths[widthIndex], 570);
                    Application.DoEvents();
                    AssertProcessingClusters(form, "active resize at " + activeWidths[widthIndex] + " client pixels");
                }
                serviceMode.SelectedIndex = 0; Application.DoEvents();
                form.ClientSize = new Size(639, 570); Application.DoEvents();
                Equal(true, statistics.Compact, "one pixel below responsive breakpoint");
                form.ClientSize = new Size(640, 570); Application.DoEvents();
                Equal(false, statistics.Compact, "responsive breakpoint boundary");
                for (int transition = 0; transition < 4; transition++)
                {
                    form.Size = new Size(620, 590); Application.DoEvents();
                    Equal(true, statistics.Compact, "repeated compact transition");
                    form.Size = new Size(790, 660); Application.DoEvents();
                    Equal(false, statistics.Compact, "repeated normal transition");
                    if (processingGroup != null) Equal(unlimitedProcessingHeight, processingGroup.Height, "processing height stable across breakpoints");
                }
                PlaybackEngine responsiveEngine = (PlaybackEngine)typeof(MainForm).GetField("_engine", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                long processingBeforeResize = responsiveEngine.ProcessingMicroseconds;
                ServiceDurationMode modeBeforeResize = responsiveEngine.ServiceDurationMode;
                Stopwatch responsiveTimer = Stopwatch.StartNew();
                long slowestTransition = 0;
                for (int transition = 0; transition < 10; transition++)
                {
                    Stopwatch crossing = Stopwatch.StartNew();
                    form.ClientSize = new Size(639, 570); Application.DoEvents();
                    crossing.Stop();
                    slowestTransition = Math.Max(slowestTransition, crossing.ElapsedMilliseconds);
                    crossing.Restart();
                    form.ClientSize = new Size(640, 570); Application.DoEvents();
                    crossing.Stop();
                    slowestTransition = Math.Max(slowestTransition, crossing.ElapsedMilliseconds);
                }
                responsiveTimer.Stop();
                if (slowestTransition >= 250 || responsiveTimer.ElapsedMilliseconds > 2200)
                    throw new Exception("responsive breakpoint transitions took " + responsiveTimer.ElapsedMilliseconds +
                        " ms total; slowest crossing " + slowestTransition + " ms");
                Equal(processingBeforeResize, responsiveEngine.ProcessingMicroseconds, "resize does not reapply processing model");
                Equal(modeBeforeResize, responsiveEngine.ServiceDurationMode, "resize does not mutate Rate model");
                Console.WriteLine("      Responsive transitions: 20 realized crossings in " + responsiveTimer.ElapsedMilliseconds +
                    " ms; slowest " + slowestTransition + " ms");
                if (FindButton(controls, "Play") == null) throw new Exception("merged Play button was not found");
                if (FindButton(controls, "Pause") != null) throw new Exception("separate Pause button still exists");
                if (FindButton(controls, "−10 sec") != null || FindButton(controls, "+10 sec") != null)
                    throw new Exception("redundant ten-second seek buttons still exist");
                if (FindButton(controls, "Reset stats") == null) throw new Exception("Reset stats button was not restored");
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

                Equal("Timeline / output:", StatisticsView.CaptionAt(0), "statistics first caption");
                Equal("Queue now / maximum:", StatisticsView.CaptionAt(1), "statistics second caption");
                Equal("Maximum rate:", StatisticsView.CaptionAt(2), "statistics third caption");
                Equal("Output rate:", StatisticsView.CaptionAt(3), "statistics fourth caption");
                Equal("Events sent / dropped:", StatisticsView.CaptionAt(4), "statistics fifth caption");
                Equal("Effective speed:", StatisticsView.CaptionAt(5), "statistics sixth caption");
                Equal("Maximum lag:", StatisticsView.CaptionAt(6), "statistics seventh caption");
                Equal("Current lag:", StatisticsView.CaptionAt(7), "statistics eighth caption");
                statistics.Size = new Size(760, statistics.Height);
                values[1] = "1,097,842 / 1,500,000";
                statistics.SetValues(values);
                using (Bitmap queueBitmap = new Bitmap(statistics.Width, statistics.Height))
                    statistics.DrawToBitmap(queueBitmap, statistics.ClientRectangle);
                Equal(false, statistics.ValueWasTruncated(1), "large queue value uses available cell width");

                Equal(0, serviceMode.SelectedIndex, "processing-time Rate model restored for value-preservation test");
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

        private static void TestFinishingReleaseContracts()
        {
            string dense = CreateDenseMidiFile(75000);
            try
            {
                Application.EnableVisualStyles();
                using (MainForm form = new MainForm())
                {
                    form.SuppressLoadErrorDialogs = true;
                    form.Show(); Application.DoEvents();
                    List<Control> controls = new List<Control>();
                    CollectControls(form, controls);
                    GroupBox processing = FindGroupBox(controls, "Processing model");
                    GroupBox playback = FindGroupBox(controls, "Playback");
                    GroupBox statisticsGroup = FindGroupBox(controls, "Statistics");
                    StatisticsView statistics = FindStatisticsView(form);
                    int height = form.Height;
                    int processingTop = processing.Top;
                    int playbackTop = playback.Top;
                    int statisticsTop = statisticsGroup.Top;
                    form.BeginMidiLoad(dense);
                    Equal(height, form.Height, "finishing load keeps window height");
                    Equal(processingTop, processing.Top, "finishing load keeps Processing model position");
                    Equal(playbackTop, playback.Top, "finishing load keeps Playback position");
                    Equal(statisticsTop, statisticsGroup.Top, "finishing load keeps Statistics position");
                    Equal(true, form.LoadingActivityVisible, "finishing load shows both in-row progress bars");
                    form.CancelMidiLoad();
                    PumpFor(100);

                    form.Size = new Size(500, 470); Application.DoEvents();
                    form.Size = new Size(806, 590); Application.DoEvents();
                    if (statistics.Bottom > statisticsGroup.ClientSize.Height)
                        throw new Exception("restored Statistics surface is clipped by its group");
                    Point viewPoint = form.PointToClient(statistics.PointToScreen(Point.Empty));
                    if (viewPoint.Y + statistics.Height > form.ClientSize.Height)
                        throw new Exception("restored Statistics surface is clipped by the form");
                    using (Bitmap bitmap = new Bitmap(statistics.Width, statistics.Height))
                        statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
                    form.Close();
                }

                MidiSong song = BuildSong(new long[] { 0, 100000, 200000 });
                WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, DefaultAnalysisConfiguration());
                using (DiagnosticsForm analysisForm = new DiagnosticsForm(song, analysis))
                {
                    analysisForm.Show(); Application.DoEvents();
                    SplitContainer split = analysisForm.AnalysisSplit;
                    if (split.BackColor == split.Panel1.BackColor || split.SplitterWidth < 4)
                        throw new Exception("untouched Analysis splitter is not visibly distinct");
                    WorkloadGraph graph = analysisForm.Graph;
                    Rectangle area = graph.GraphArea;
                    long edgeTime;
                    Equal(true, graph.TryGetPinTime(new Point(area.Left - 3, area.Top + area.Height / 2), out edgeTime),
                        "finishing left-edge pin tolerance");
                    Equal(0L, edgeTime, "finishing left-edge pin exact zero");
                    Equal(false, graph.TryGetPinTime(new Point(area.Left - 7, area.Top + area.Height / 2), out edgeTime),
                        "finishing outside-edge click rejection");
                    analysisForm.Close();
                }
            }
            finally { if (File.Exists(dense)) File.Delete(dense); }
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

        private static GroupBox FindGroupBox(List<Control> controls, string text)
        {
            for (int i = 0; i < controls.Count; i++)
            {
                GroupBox group = controls[i] as GroupBox;
                if (group != null && group.Text == text) return group;
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

        private static void AssertNumericFullyVisible(NumericUpDown numeric, string name)
        {
            if (numeric.Parent == null || numeric.Left < 0 || numeric.Top < 0 ||
                numeric.Right > numeric.Parent.ClientSize.Width || numeric.Bottom > numeric.Parent.ClientSize.Height)
                throw new Exception(name + " outer bounds are clipped");
            for (int index = 0; index < numeric.Controls.Count; index++)
            {
                Control child = numeric.Controls[index];
                if (!child.Visible) continue;
                // The native UpDownBase hosts its edit/spinner children one
                // border pixel outside the client vertically. Horizontal
                // clipping is the failure that hides the spinner arrows.
                if (child.Left < 0 || child.Top < -1 || child.Right > numeric.ClientSize.Width ||
                    child.Bottom > numeric.ClientSize.Height + 1)
                    throw new Exception(name + " internal " + child.GetType().Name + " area is clipped: " +
                        child.Bounds + " within " + numeric.ClientRectangle);
            }
            string maximum = numeric.Maximum.ToString("N0");
            int textWidth = TextRenderer.MeasureText(maximum, numeric.Font).Width;
            if (numeric.ClientSize.Width < textWidth + SystemInformation.VerticalScrollBarWidth + 6)
                throw new Exception(name + " is too narrow for its maximum value and spinner buttons");
        }

        private static void AssertComboFullyVisible(ComboBox combo, string longestText, string name)
        {
            if (combo.Parent == null || combo.Left < 0 || combo.Top < 0 || combo.Right > combo.Parent.ClientSize.Width ||
                combo.Bottom > combo.Parent.ClientSize.Height)
                throw new Exception(name + " bounds or dropdown arrow are clipped");
            int required = TextRenderer.MeasureText(longestText, combo.Font, Size.Empty,
                TextFormatFlags.NoPadding | TextFormatFlags.SingleLine).Width + SystemInformation.VerticalScrollBarWidth + 3;
            if (combo.ClientSize.Width < required)
                throw new Exception(name + " is too narrow for '" + longestText + "' and its dropdown arrow");
        }

        private static void AssertProcessingClusters(MainForm form, string state)
        {
            const System.Reflection.BindingFlags flags = System.Reflection.BindingFlags.Instance |
                System.Reflection.BindingFlags.NonPublic;
            Type type = typeof(MainForm);
            FlowLayoutPanel queue = (FlowLayoutPanel)type.GetField("_queueCluster", flags).GetValue(form);
            FlowLayoutPanel overflow = (FlowLayoutPanel)type.GetField("_overflowCluster", flags).GetValue(form);
            FlowLayoutPanel rate = (FlowLayoutPanel)type.GetField("_rateCluster", flags).GetValue(form);
            FlowLayoutPanel service = (FlowLayoutPanel)type.GetField("_serviceCluster", flags).GetValue(form);
            Label overflowLabel = (Label)type.GetField("_overflowLabel", flags).GetValue(form);
            Label serviceLabel = (Label)type.GetField("_serviceValueLabel", flags).GetValue(form);
            Label unit = (Label)type.GetField("_serviceUnitLabel", flags).GetValue(form);
            NumericUpDown queueValue = (NumericUpDown)type.GetField("_queueLimitValue", flags).GetValue(form);
            NumericUpDown processingValue = (NumericUpDown)type.GetField("_processingValue", flags).GetValue(form);
            ComboBox rateCombo = (ComboBox)type.GetField("_serviceModeCombo", flags).GetValue(form);
            ComboBox overflowCombo = (ComboBox)type.GetField("_overflowPolicyCombo", flags).GetValue(form);
            FlowLayoutPanel[] clusters = new FlowLayoutPanel[] { queue, overflow, rate, service };
            for (int index = 0; index < clusters.Length; index++)
            {
                FlowLayoutPanel cluster = clusters[index];
                if (cluster.Left < 0 || cluster.Top < 0 || cluster.Right > cluster.Parent.ClientSize.Width ||
                    cluster.Bottom > cluster.Parent.ClientSize.Height)
                    throw new Exception(state + " clips processing cluster " + index + ": " + cluster.Bounds +
                        " within " + cluster.Parent.ClientRectangle);
            }
            int overflowX = form.PointToClient(overflowLabel.PointToScreen(Point.Empty)).X;
            int serviceX = form.PointToClient(serviceLabel.PointToScreen(Point.Empty)).X;
            if (Math.Abs(overflowX - serviceX) > 1)
                throw new Exception(state + " misaligns right-side captions by " + Math.Abs(overflowX - serviceX) + " pixels");
            int unitGap = unit.Left - processingValue.Right;
            if (unitGap < 0 || unitGap > 8)
                throw new Exception(state + " lets the service unit drift " + unitGap + " pixels from its numeric field");
            AssertNumericFullyVisible(queueValue, state + " queue numeric");
            AssertNumericFullyVisible(processingValue, state + " rate numeric");
            AssertComboFullyVisible(rateCombo, "Processing time per event", state + " Rate model");
            if (overflowCombo.Right > overflow.ClientSize.Width)
                throw new Exception(state + " clips the overflow dropdown arrow");
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
            song.NoteCount = eventTimes.Length;
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

        private static void PumpUntil(Func<bool> condition, int timeoutMilliseconds, string name)
        {
            Stopwatch timer = Stopwatch.StartNew();
            while (!condition())
            {
                if (timer.ElapsedMilliseconds > timeoutMilliseconds) throw new Exception(name + " timed out");
                Application.DoEvents();
                Thread.Sleep(1);
            }
            Application.DoEvents();
        }

        private static void PumpFor(int milliseconds)
        {
            Stopwatch timer = Stopwatch.StartNew();
            while (timer.ElapsedMilliseconds < milliseconds)
            {
                Application.DoEvents();
                Thread.Sleep(1);
            }
        }

        private static void DeleteFileWhenAvailable(string path)
        {
            if (String.IsNullOrEmpty(path)) return;
            Stopwatch wait = Stopwatch.StartNew();
            while (File.Exists(path))
            {
                try { File.Delete(path); return; }
                catch (IOException)
                {
                    if (wait.ElapsedMilliseconds >= 3000) throw;
                    Application.DoEvents();
                    Thread.Sleep(10);
                }
            }
        }

        private static string CreateDenseMidiFile(int eventCount)
        {
            string path = Path.Combine(Path.GetTempPath(), "midi-bottleneck-dense-" + Guid.NewGuid().ToString("N") + ".mid");
            List<byte> track = new List<byte>(eventCount * 3 + 8);
            Add(track, 0x00, 0x90, 0x3C, 0x01);
            for (int i = 1; i < eventCount; i++) Add(track, 0x00, 0x3C, 0x01);
            Add(track, 0x00, 0xFF, 0x2F, 0x00);
            List<byte> bytes = new List<byte>(track.Count + 22);
            AddAscii(bytes, "MThd");
            AddUInt32(bytes, 6);
            AddUInt16(bytes, 0);
            AddUInt16(bytes, 1);
            AddUInt16(bytes, 480);
            AddTrack(bytes, track);
            File.WriteAllBytes(path, bytes.ToArray());
            return path;
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

        private static void Near(double expected, double actual, double tolerance, string name)
        {
            if (Math.Abs(expected - actual) > tolerance)
                throw new Exception(name + ": expected " + expected + " ± " + tolerance + ", got " + actual);
        }

        private sealed class CountingMidiOutput : IMidiOutput
        {
            public long Count;
            public void Send(MidiEvent midiEvent) { Count++; }
            public void Panic() { }
            public void Reset() { }
        }

        private sealed class CallbackMidiOutput : IMidiOutput
        {
            private readonly Action<long> _callback;
            private long _count;
            internal CallbackMidiOutput(Action<long> callback) { _callback = callback; }
            internal long Count { get { return Interlocked.Read(ref _count); } }
            public void Send(MidiEvent midiEvent)
            {
                long count = Interlocked.Increment(ref _count);
                if (_callback != null) _callback(count);
            }
            public void Panic() { }
            public void Reset() { }
        }

        private sealed class PacedMidiOutput : IMidiOutput
        {
            private long _count;
            internal int ResetCount;
            internal int PanicCount;
            internal long Count { get { return Interlocked.Read(ref _count); } }
            public void Send(MidiEvent midiEvent)
            {
                Thread.SpinWait(1200);
                Interlocked.Increment(ref _count);
            }
            public void Panic() { Interlocked.Increment(ref PanicCount); }
            public void Reset() { Interlocked.Increment(ref ResetCount); }
        }

        private sealed class TimedMidiOutput : IMidiOutput
        {
            private readonly IMidiOutput _inner;
            public long SendTicks;
            public TimedMidiOutput(IMidiOutput inner) { _inner = inner; }
            public void Send(MidiEvent midiEvent)
            {
                long start = Stopwatch.GetTimestamp();
                try { _inner.Send(midiEvent); }
                finally { SendTicks += Stopwatch.GetTimestamp() - start; }
            }
            public void Panic() { _inner.Panic(); }
            public void Reset() { _inner.Reset(); }
        }

        private sealed class FakeMidiOutput : IMidiOutput, IMidiOutputContext
        {
            public string SourceFile { get; set; }
            private readonly object _sync = new object();
            private readonly List<long> _sent = new List<long>();
            private readonly List<MidiEvent> _sentEvents = new List<MidiEvent>();
            public int ResetCount;
            public int PanicCount;
            public int LastResetSequence;
            public int LastPanicSequence;
            private int _safetySequence;

            public void Send(MidiEvent midiEvent)
            {
                lock (_sync)
                {
                    _sent.Add(midiEvent.IntendedMicroseconds);
                    _sentEvents.Add(midiEvent);
                }
            }

            public void Panic() { lock (_sync) { PanicCount++; LastPanicSequence = ++_safetySequence; } }

            public void Reset()
            {
                lock (_sync)
                {
                    ResetCount++;
                    LastResetSequence = ++_safetySequence;
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

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        private struct CapturedMidiHeader
        {
            public IntPtr Data;
            public uint BufferLength;
            public uint BytesRecorded;
            public IntPtr User;
            public uint Flags;
            public IntPtr Next;
            public IntPtr Reserved;
            public uint Offset;
            public IntPtr Reserved0;
            public IntPtr Reserved1;
            public IntPtr Reserved2;
            public IntPtr Reserved3;
            public IntPtr Reserved4;
            public IntPtr Reserved5;
            public IntPtr Reserved6;
            public IntPtr Reserved7;
        }

        private sealed class FakeKdmApiNative : IKdmApiNative
        {
            public int InitializeCount;
            public int TerminateCount;
            public int ResetCount;
            public int PrepareLongCount;
            public int SendLongCount;
            public int UnprepareLongCount;
            public int DisposeCount;
            public bool InitializeResult = true;
            public bool TerminateResult = true;
            public uint PrepareLongResult;
            public int LastHeaderSize;
            public CapturedMidiHeader LastHeader;
            public uint LastInitialFlags;
            public readonly List<uint> ShortMessages = new List<uint>();
            public string Version { get { return "test"; } }
            public string ProviderPath { get { return "test-provider"; } }
            public bool SupportsLongMessages { get; set; }
            public string LongMessageStatus { get { return SupportsLongMessages ? "prepared long-message exports available" : "test long-message exports missing"; } }

            public FakeKdmApiNative()
            {
                SupportsLongMessages = true;
            }

            public bool IsAvailable() { return true; }
            public bool InitializeStream() { InitializeCount++; return InitializeResult; }
            public bool TerminateStream() { TerminateCount++; return TerminateResult; }
            public void ResetStream() { ResetCount++; }
            public void SendShort(uint message) { ShortMessages.Add(message); }
            public uint PrepareLong(IntPtr header, uint headerSize)
            {
                PrepareLongCount++;
                LastHeaderSize = (int)headerSize;
                LastHeader = (CapturedMidiHeader)Marshal.PtrToStructure(header, typeof(CapturedMidiHeader));
                LastInitialFlags = LastHeader.Flags;
                return PrepareLongResult;
            }
            public uint SendLong(IntPtr header, uint headerSize) { SendLongCount++; return 0; }
            public uint UnprepareLong(IntPtr header, uint headerSize) { UnprepareLongCount++; return 0; }
            public void Dispose() { DisposeCount++; }
        }
    }
}
