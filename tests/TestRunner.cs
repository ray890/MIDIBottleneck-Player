using System;
using System.Collections.Generic;
using System.IO;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.Threading;
using System.Runtime.InteropServices;
using System.Reflection;
using System.Windows.Forms;

namespace MidiBottleneck.Tests
{
    internal static class TestRunner
    {
        private static int _passed;

        [DllImport("user32.dll")]
        private static extern IntPtr GetSystemMenu(IntPtr window, bool revert);

        [DllImport("user32.dll")]
        private static extern uint GetMenuState(IntPtr menu, uint identifier, uint flags);

        [DllImport("user32.dll")]
        private static extern int GetMenuItemCount(IntPtr menu);

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
                if (arguments.Length == 3 && arguments[0] == "--report-state-prefix")
                {
                    ReportMidiStatePrefix(arguments[1], Int64.Parse(arguments[2], CultureInfo.InvariantCulture));
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
                if (arguments.Length == 2 && arguments[0] == "--render-ui-none")
                {
                    RenderMainWindow(arguments[1], false, false, false, true);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-ui-min-none")
                {
                    RenderMainWindow(arguments[1], false, true, false, true);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-channel-monitor")
                {
                    RenderChannelMonitor(arguments[1], false, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-channel-monitor-stale")
                {
                    RenderChannelMonitor(arguments[1], true, false);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-channel-monitor-forced")
                {
                    RenderChannelMonitor(arguments[1], false, true);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-build20")
                {
                    RenderBuild20Set(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-build21")
                {
                    RenderBuild21Set(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-build23")
                {
                    RenderBuild23Set(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-build22-new")
                {
                    RenderBuild22NewSet(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-analysis-auto")
                {
                    RenderAutomaticAnalysisWindow(arguments[1]);
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
                    if (String.Equals(arguments[1], "synthetic", StringComparison.OrdinalIgnoreCase))
                    {
                        string syntheticPath = CreateDenseMidiFile(5000000);
                        try { RenderLoadingMainWindow(syntheticPath, arguments[2], false); }
                        finally { DeleteFileWhenAvailable(syntheticPath); }
                    }
                    else RenderLoadingMainWindow(arguments[1], arguments[2], false);
                    return 0;
                }
                if (arguments.Length == 3 && arguments[0] == "--render-ui-loading-min")
                {
                    if (String.Equals(arguments[1], "synthetic", StringComparison.OrdinalIgnoreCase))
                    {
                        string syntheticPath = CreateDenseMidiFile(5000000);
                        try { RenderLoadingMainWindow(syntheticPath, arguments[2], true); }
                        finally { DeleteFileWhenAvailable(syntheticPath); }
                    }
                    else RenderLoadingMainWindow(arguments[1], arguments[2], true);
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
                if (arguments.Length == 3 && arguments[0] == "--render-analysis-synthetic")
                {
                    string syntheticPath = CreateDenseMidiFile(20000);
                    try
                    {
                        if (String.Equals(arguments[1], "minimum", StringComparison.OrdinalIgnoreCase))
                            RenderAnalysisWindow(syntheticPath, arguments[2], false, true, true, false, true, null, false);
                        else if (String.Equals(arguments[1], "busy", StringComparison.OrdinalIgnoreCase))
                            RenderBusyAnalysisWindow(syntheticPath, arguments[2]);
                        else RenderAutomaticAnalysisWindowFromFile(syntheticPath, arguments[2]);
                    }
                    finally { DeleteFileWhenAvailable(syntheticPath); }
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--render-corrective-desktop")
                {
                    RenderCorrectiveDesktop(arguments[1]);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-corrective")
                {
                    RunFocused("WinMM adapter successful-send work and errors", TestWinMmAdapter);
                    RunFocused("KDMAPI short-send hot path", TestKdmApiShortHotPath);
                    RunFocused("KDMAPI prepared SysEx reclamation", TestKdmApiOutput);
                    RunFocused("integer stopwatch conversion matches exact scheduler math", TestStopwatchConversion);
                    RunFocused("live queue snapshots during slow and blocked output", TestQueueFreshness);
                    RunFocused("file workload scan reuse with exact projections", TestAnalysisWorkloadReuse);
                    RunFocused("loading cadence and corrected layout", TestCorrectiveLoading);
                    RunFocused("blocking output worker lifecycle", TestBlockingOutputWorkerLifecycle);
                    RunFocused("immediate dispatch ordering and checkpoint cadence", TestImmediateDispatchCadence);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-timing-lifecycle")
                {
                    RunFocused("blocking output worker lifecycle", TestBlockingOutputWorkerLifecycle);
                    RunFocused("active seek regression", TestActiveSeek);
                    RunFocused("paused seek regression", TestPausedSeek);
                    RunFocused("reset then silence contract", TestOutputResetSilenceContract);
                    RunFocused("output restart semantics", TestOutputRestartSemantics);
                    RunFocused("immediate dispatch ordering and checkpoint cadence", TestImmediateDispatchCadence);
                    RunFocused("loading presentation heartbeat cadence", TestCorrectiveLoading);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-playback-overflow-axis")
                {
                    RunFocused("blocking Pause/Stop output lifecycle", TestBlockingOutputWorkerLifecycle);
                    RunFocused("Drop incoming complete notes scheduler and Analysis equivalence", TestCompleteNoteOverflow);
                    RunFocused("Clear-buffer exact binary catch-up", TestClearCatchUpOptimization);
                    RunFocused("adaptive aligned Analysis time-axis ticks", TestAdaptiveTimelineTicks);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-null-output")
                {
                    RunFocused("None output no-op and allocation-free contract", TestNullMidiOutputContract);
                    RunFocused("None output selector and native device mapping", TestNullOutputSelection);
                    RunFocused("None output playback and restart boundary", TestNullOutputPlayback);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-live-statistics-loading")
                {
                    RunFocused("observed maximum output-rate retention and reset", TestObservedMaximumOutputRate);
                    RunFocused("loading elapsed/private-memory presentation", TestBackgroundMidiLoading);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-event-store")
                {
                    RunFocused("indexed read-only event-store boundary", TestMidiEventStoreBoundary);
                    RunFocused("packed short-message storage", TestPackedMidiEventData);
                    RunFocused("tempo/running-status/SysEx parser regression", TestMidiParser);
                    RunFocused("immediate dispatch payload/order regression", TestImmediateDispatchCadence);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-analysis-channels")
                {
                    RunFocused("background loading telemetry and allocation", TestBackgroundMidiLoading);
                    RunFocused("asynchronous Analysis cancellation and Auto label", TestAsynchronousAnalysis);
                    RunFocused("Analysis shell detach and rebind across file replacement", TestAnalysisWindowPersistence);
                    RunFocused("dispatched MIDI channel-state lifecycle", TestChannelStateMonitor);
                    RunFocused("bounded channel overrides", TestChannelOverrides);
                    RunFocused("read-only 16-channel monitor UI", TestChannelMonitorInterface);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build20")
                {
                    RunFocused("scrub-or-type channel override editor", TestBuild20ScrubEditor);
                    RunFocused("managed icon on application-owned forms", TestBuild20FormIcons);
                    RunFocused("persistent detached Analysis and channel-monitor shells", TestAnalysisWindowPersistence);
                    RunFocused("Analysis report wrapping and splitter cursor", TestBuild19AnalysisUsability);
                    RunFocused("measured compact-width layout and statistic captions", TestBuild20CompactLayout);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build21")
                {
                    RunFocused("first grid gesture and refined scrub typing", TestBuild21ScrubHandoff);
                    RunFocused("historical chase and channel output filtering", TestBuild21ChannelControls);
                    RunFocused("channel monitor measured fitting", TestBuild21ChannelMonitorFit);
                    RunFocused("normalized Analysis geometry", TestBuild21AnalysisGeometry);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build22")
                {
                    RunFocused("unchanged typing and refined Channel Monitor fitting", TestBuild21ScrubHandoff);
                    RunFocused("active-worker historical chase acknowledgement", TestBuild22HistoricalChaseAcknowledgement);
                    RunFocused("channel monitor measured fitting", TestBuild21ChannelMonitorFit);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build23-local")
                {
                    RunFocused("real Channel Monitor historical-chase route", TestBuild23HistoricalChaseUiRoute);
                    RunFocused("single-source product metadata", TestBuild23ProductMetadata);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build21-source-chase")
                {
                    RunFocused("latest source-value chase through realized Channel Monitor", TestBuild21SourceValueChase);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build22-new")
                {
                    RunFocused("main-window MIDI file drag and drop", TestBuild22MidiFileDrop);
                    RunFocused("Analysis predicted output completion", TestAnalysisPredictedCompletion);
                    RunFocused("Always-on-top native system-menu command", TestBuild22AlwaysOnTopMenu);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--test-build23-filtering")
                {
                    RunFocused("pre-admission channel and override filtering", TestBuild23PreAdmissionFiltering);
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--benchmark-winmm-adapter")
                {
                    BenchmarkWinMmAdapter();
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--benchmark-short-adapters")
                {
                    BenchmarkShortAdapters();
                    return 0;
                }
                if (arguments.Length == 2 && arguments[0] == "--benchmark-event-store")
                {
                    BenchmarkEventStore(Int32.Parse(arguments[1], CultureInfo.InvariantCulture));
                    return 0;
                }
                if (arguments.Length == 1 && arguments[0] == "--benchmark-channel-monitor")
                {
                    BenchmarkChannelMonitorOverhead();
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
                Run("indexed read-only event-store boundary", TestMidiEventStoreBoundary);
                Run("packed short-message storage", TestPackedMidiEventData);
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
                Run("Drop incoming complete notes scheduler and Analysis equivalence", TestCompleteNoteOverflow);
                Run("clear buffer and catch up production behavior", TestClearBufferCatchUpPlaybackEngine);
                Run("clear buffer uses exact binary catch-up", TestClearCatchUpOptimization);
                Run("zero processing time", TestZeroServiceTime);
                Run("slowdown-enabled zero service uses immediate scheduler path", TestEffectiveZeroServicePath);
                Run("MIDI bitrate byte-duration calculation", TestMidiBitrateCalculation);
                Run("MIDI bitrate production service intervals", TestMidiBitratePlaybackIntervals);
                Run("parsed MIDI Queue playback under MIDI bitrate", TestParsedMidiBitrateQueuePlayback);
                Run("service-mode values remain independent", TestServiceModeValuePreservation);
                Run("active seek clears queued work and stale dispatches", TestActiveSeek);
                Run("paused seek remains paused at a clean position", TestPausedSeek);
                Run("blocking output cannot overlap seek or stop workers", TestBlockingOutputWorkerLifecycle);
                Run("immediate dispatch preserves payload/order without checkpoint stalls", TestImmediateDispatchCadence);
                Run("SMF SysEx fragments are framed for strict winmm drivers", TestSystemExclusiveAssembly);
                Run("OmniMIDI rejected packet regression structures and MIDIHDR fields", TestOmniMidiPacketStructures);
                Run("WinMM adapter successful-send work and errors", TestWinMmAdapter);
                Run("KDMAPI short-send hot path", TestKdmApiShortHotPath);
                Run("integer stopwatch conversion matches exact scheduler math", TestStopwatchConversion);
                Run("live queue snapshots during slow and blocked output", TestQueueFreshness);
                Run("file workload scan reuse with exact projections", TestAnalysisWorkloadReuse);
                Run("loading cadence and corrected layout", TestCorrectiveLoading);
                Run("KDMAPI short, SysEx, reset, and stream lifecycle", TestKdmApiOutput);
                Run("KDMAPI initialization failure cleanup", TestKdmApiInitializationCleanup);
                Run("controlled KDMAPI provider selection and architecture validation", TestKdmApiProviderSelection);
                Run("Stop and Seek reset then silence MIDI output", TestOutputResetSilenceContract);
                Run("Reset stats does not disrupt playback", TestResetStatisticsDuringPlayback);
                Run("processing slider low-range mapping and track clicks", TestProcessingSliderMapping);
                Run("effective playback speed rolling estimate", TestEffectivePlaybackSpeed);
                Run("current MIDI output-rate rolling estimate", TestRollingOutputRate);
                Run("observed maximum output-rate retention and reset", TestObservedMaximumOutputRate);
                Run("live Rate model applies at next service", TestLiveRateModelChange);
                Run("live overflow policy applies at next overflow", TestLiveOverflowPolicyChange);
                Run("output restart preserves paused source position and clears backlog", TestOutputRestartSemantics);
                Run("None output no-op and allocation-free contract", TestNullMidiOutputContract);
                Run("None output selector and native device mapping", TestNullOutputSelection);
                Run("None output playback and restart boundary", TestNullOutputPlayback);
                Run("dispatched MIDI channel-state lifecycle", TestChannelStateMonitor);
                Run("bounded channel overrides", TestChannelOverrides);
                Run("read-only 16-channel monitor UI", TestChannelMonitorInterface);
                Run("scrub-or-type channel override editor", TestBuild20ScrubEditor);
                Run("managed icon on application-owned forms", TestBuild20FormIcons);
                Run("first grid gesture and refined scrub typing", TestBuild21ScrubHandoff);
                Run("historical chase and channel output filtering", TestBuild21ChannelControls);
                Run("active-worker historical chase acknowledgement", TestBuild22HistoricalChaseAcknowledgement);
                Run("real Channel Monitor historical-chase route", TestBuild23HistoricalChaseUiRoute);
                Run("latest source-value chase through realized Channel Monitor", TestBuild21SourceValueChase);
                Run("main-window MIDI file drag and drop", TestBuild22MidiFileDrop);
                Run("Analysis predicted output completion", TestAnalysisPredictedCompletion);
                Run("Always-on-top native system-menu command", TestBuild22AlwaysOnTopMenu);
                Run("single-source product metadata", TestBuild23ProductMetadata);
                Run("pre-admission channel and override filtering", TestBuild23PreAdmissionFiltering);
                Run("channel monitor measured fitting", TestBuild21ChannelMonitorFit);
                Run("normalized Analysis geometry", TestBuild21AnalysisGeometry);
                Run("dense 200,000-event MIDI parsing", TestDenseMidiParser);
                Run("cancellable parser progress and cancellation", TestCancellableMidiParser);
                Run("contiguous event-storage limit fails clearly before allocation", TestContiguousEventStorageLimit);
                Run("background loading, stale-result rejection, and unload", TestBackgroundMidiLoading);
                Run("workload analysis and graph data", TestWorkloadAnalysis);
                Run("asynchronous Analysis refresh and resolution policy", TestAsynchronousAnalysis);
                Run("Analysis shell detach and rebind across file replacement", TestAnalysisWindowPersistence);
                Run("Analysis report wrapping and splitter cursor", TestBuild19AnalysisUsability);
                Run("configured whole-file Analysis window", TestAnalysisWindowConstruction);
                Run("selected-model Analysis interaction and seek", TestAnalysisInteraction);
                Run("Analysis graph geometry, overlay, and coalesced message-pump updates", TestAnalysisRenderingRefinements);
                Run("adaptive aligned Analysis time-axis ticks", TestAdaptiveTimelineTicks);
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
                Run("measured compact-width layout and statistic captions", TestBuild20CompactLayout);
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

        private static void RenderCorrectiveDesktop(string directory)
        {
            Directory.CreateDirectory(directory);
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); PumpFor(100);
                CaptureCorrectiveState(form, directory, "initial-standard");
                List<Control> controls = new List<Control>(); CollectControls(form, controls);
                FindCheckBox(controls, "Simulate slowdown").Checked = true;
                form.Size = new Size(500, 500); PumpFor(100);
                CaptureCorrectiveState(form, directory, "compact-enabled");
                FindCheckBox(controls, "Queue limit:").Checked = true; PumpFor(100);
                CaptureCorrectiveState(form, directory, "compact-pressure");
                FindComboContaining(controls, "MIDI serial bitrate").SelectedIndex = 1; PumpFor(100);
                CaptureCorrectiveState(form, directory, "compact-bitrate");
                form.Size = new Size(806, 544); PumpFor(100);
                CaptureCorrectiveState(form, directory, "restored-standard");
                var flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
                typeof(MainForm).GetField("_loadingSong", flags).SetValue(form, true);
                typeof(MainForm).GetMethod("SetLoadingPresentation", flags).Invoke(form, new object[] { true });
                Label loadingFile = (Label)typeof(MainForm).GetField("_fileLabel", flags).GetValue(form);
                loadingFile.Text = "Loading a very long black-MIDI filename used for compact allocation verification.mid…" +
                    Environment.NewLine + MainForm.FormatLoadingTelemetry(3723, 12L * 1024 * 1024 * 1024, false);
                typeof(MainForm).GetField("_loadProgress", flags).SetValue(form,
                    new MidiLoadProgress("Assigning event timestamps — long current-stage description", 16, 22, 750, 1000, 500, 1000));
                PumpFor(150);
                CaptureCorrectiveState(form, directory, "loading-standard");
                form.Size = new Size(500, 500); PumpFor(150);
                loadingFile.Text = "Loading a very long black-MIDI filename used for compact allocation verification.mid…" +
                    Environment.NewLine + MainForm.FormatLoadingTelemetry(3723, 12L * 1024 * 1024 * 1024, true);
                PumpFor(60);
                CaptureCorrectiveState(form, directory, "loading-compact");
                typeof(MainForm).GetField("_loadingSong", flags).SetValue(form, false);
                form.Close();
            }
        }

        private static void CaptureCorrectiveState(MainForm form, string directory, string name)
        {
            form.Activate(); form.BringToFront(); PumpFor(120);
            using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
            {
                bool screen = true;
                try { using (Graphics graphics = Graphics.FromImage(bitmap)) graphics.CopyFromScreen(form.Location, Point.Empty, form.Size); }
                catch (System.ComponentModel.Win32Exception) { screen = false; CaptureForm(form, bitmap); }
                bitmap.Save(Path.Combine(directory, name + ".png"));
                Console.WriteLine(name + ": " + form.Width + "×" + form.Height + "; desktop pixels=" + screen);
            }
        }

        private static void TestWinMmAdapter()
        {
            MidiEvent midiEvent = BuildSong(new long[] { 0 }).Events[0];
            int calls = 0;
            uint result = 0;
            using (WindowsMidiOutput output = new WindowsMidiOutput(delegate(IntPtr handle, uint message) { calls++; return result; }))
            {
                for (int i = 0; i < 1000; i++) output.Send(midiEvent);
                int before = GC.CollectionCount(0);
                for (int i = 0; i < 500000; i++) output.Send(midiEvent);
                Equal(before, GC.CollectionCount(0), "successful production adapter sends allocate no diagnostic strings");
                Equal(501000, calls, "adapter preserves all short messages");
                result = 3;
                bool failed = false;
                try { output.Send(midiEvent); }
                catch (System.ComponentModel.Win32Exception ex)
                {
                    failed = ex.NativeErrorCode == 3 && ex.Message.Contains("Deterministic WinMM boundary") && ex.Message.Contains("handle 0x1");
                }
                Equal(true, failed, "nonzero standard result retains detailed cached identity");
            }
        }

        private sealed class SlowGateOutput : IMidiOutput
        {
            public readonly ManualResetEvent Entered = new ManualResetEvent(false);
            public readonly ManualResetEvent Release = new ManualResetEvent(false);
            public int Count;
            public void Send(MidiEvent midiEvent)
            {
                Entered.Set();
                Release.WaitOne(1000);
                Thread.Sleep(1);
                Interlocked.Increment(ref Count);
            }
            public void Panic() { }
            public void Reset() { }
        }

        private static void TestQueueFreshness()
        {
            foreach (ProcessingMode mode in new ProcessingMode[] { ProcessingMode.Queue, ProcessingMode.Drop })
            foreach (long service in new long[] { 0, 100 })
            {
                long[] times = new long[1000];
                for (int i = 0; i < times.Length; i++) times[i] = i * 1000;
                SlowGateOutput output = new SlowGateOutput();
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.ProcessingMicroseconds = service;
                    engine.SimulateSlowdown = true;
                    engine.QueueLengthLimit = 16;
                    engine.Start(BuildSong(times), output, mode);
                    try
                    {
                        Equal(true, output.Entered.WaitOne(1000), "output entered blocked send");
                        Thread.Sleep(100);
                        PlaybackSnapshot blocked = engine.GetSnapshot();
                        Equal(0L, blocked.ProcessedEvents, "blocked send is not counted as sent");
                        Equal(blocked.QueueLength + 1, blocked.OutstandingEvents, "in-service slot distinguished from pending");
                        if (mode == ProcessingMode.Queue && blocked.QueueLength < 50)
                            throw new Exception("unlimited source arrivals froze during output blocking");
                        if (mode == ProcessingMode.Drop && blocked.OutstandingEvents > 16)
                            throw new Exception("snapshot invented finite admissions beyond capacity");
                        output.Release.Set();
                        Stopwatch publication = Stopwatch.StartNew();
                        while (engine.GetSnapshot().ProcessedEvents < 10 && publication.ElapsedMilliseconds < 800) Thread.Sleep(5);
                        PlaybackSnapshot active = engine.GetSnapshot();
                        if (active.ProcessedEvents < 10) throw new Exception("slow output statistics wait for the whole range: " + mode + ", service=" + service);
                        if (active.CurrentLagMicroseconds < 50000) throw new Exception("dispatch lag excludes output blocking");
                        engine.Pause();
                        Thread.Sleep(20);
                        long paused = engine.GetSnapshot().ProcessedEvents;
                        Thread.Sleep(20);
                        Equal(paused, engine.GetSnapshot().ProcessedEvents, "paused slow output settles without continuing dispatch");
                        engine.Seek(500000);
                        Equal(0L, engine.GetSnapshot().ProcessedEvents, "paused seek resets completed count");
                        Equal(PlaybackState.Paused, engine.State, "paused seek retains state");
                        engine.Resume(); Thread.Sleep(30); engine.Stop();
                        Equal(0L, engine.GetSnapshot().OutstandingEvents, "stop clears queue snapshot");
                    }
                    finally { output.Release.Set(); }
                }
            }
        }

        private static void TestAnalysisWorkloadReuse()
        {
            long[] times = new long[20000];
            for (int i = 0; i < times.Length; i++) times[i] = (i / 20) * 1000;
            MidiSong song = BuildSong(times);
            long before = WorkloadAnalyzer.WorkloadScanCount;
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            WorkloadAnalyzer.Analyze(song, 10000, configuration);
            Equal(before + 1, WorkloadAnalyzer.WorkloadScanCount, "initial workload scan");
            for (int mode = 0; mode < 2; mode++)
            for (int policy = 0; policy < 4; policy++)
            {
                configuration = DefaultAnalysisConfiguration();
                configuration.ServiceDurationMode = (ServiceDurationMode)mode;
                configuration.ProcessingMicroseconds = 979;
                configuration.MidiBitrate = 31251;
                configuration.QueueLengthLimitEnabled = policy != 0;
                configuration.QueueLengthLimit = 16;
                configuration.OverflowPolicy = (OverflowPolicy)policy;
                long scans = WorkloadAnalyzer.WorkloadScanCount;
                WorkloadAnalysis reused = WorkloadAnalyzer.Analyze(song, 10000, configuration);
                Equal(scans, WorkloadAnalyzer.WorkloadScanCount, "rate change reuses cluster/message workload");
                WorkloadAnalysis exact = WorkloadAnalyzer.AnalyzeUncached(song, 10000, configuration, CancellationToken.None, null);
                Equal(exact.TotalBytes, reused.TotalBytes, "exact reused bytes");
                Equal(exact.PredictedMaximumOccupancy, reused.PredictedMaximumOccupancy, "exact reused projection");
                Equal(exact.PredictedDroppedEvents, reused.PredictedDroppedEvents, "exact reused drops");
                Equal(exact.PredictedOutputCompletionMicroseconds, reused.PredictedOutputCompletionMicroseconds,
                    "exact reused output completion");
                for (int i = 0; i < exact.Buckets.Length; i++)
                {
                    Equal(exact.Buckets[i].ServiceDemandMicroseconds, reused.Buckets[i].ServiceDemandMicroseconds, "exact per-event rounded demand");
                    Equal(exact.Buckets[i].PredictedPeakOccupancy, reused.Buckets[i].PredictedPeakOccupancy, "exact bucket pressure");
                }
            }
        }

        private static void TestCorrectiveLoading()
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); Application.DoEvents();
                Equal(form.RealizedRequiredWindowHeight, form.Height, "initial standard measured minimum");
                List<Control> controls = new List<Control>(); CollectControls(form, controls);
                Equal(false, FindCheckBox(controls, "Simulate slowdown").Checked, "initial slowdown unchecked");
                PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                Equal(false, engine.SimulateSlowdown, "initial engine slowdown disabled");
                var loading = typeof(MainForm).GetField("_loadingSong", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
                var progress = typeof(MainForm).GetField("_loadProgress", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
                BufferedStatusLabel status = (BufferedStatusLabel)typeof(MainForm).GetField("_loadingStatusLabel",
                    System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                Equal(true, status.UsesAtomicPainting, "loading text uses one opaque double-buffered paint");
                int statusPaints = status.PaintCount;
                TableLayoutPanel fileTable = (TableLayoutPanel)typeof(MainForm).GetField("_fileOutputTable",
                    System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).GetValue(form);
                int loadingLayouts = 0;
                fileTable.Layout += delegate { loadingLayouts++; };
                loading.SetValue(form, true);
                typeof(MainForm).GetMethod("SetLoadingPresentation", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).Invoke(form, new object[] { true });
                List<long> samples = new List<long>();
                form.LoadingVisualSampled += delegate(long stamp) { samples.Add(stamp); };
                Stopwatch timer = Stopwatch.StartNew();
                using (System.Windows.Forms.Timer producer = new System.Windows.Forms.Timer())
                {
                    producer.Interval = 10;
                    producer.Tick += delegate { progress.SetValue(form, new MidiLoadProgress("Parsing long track", (int)(timer.ElapsedMilliseconds % 1000), 1000)); };
                    producer.Start(); PumpFor(1600); producer.Stop();
                }
                loading.SetValue(form, false);
                if (samples.Count < 2) throw new Exception("no progress samples");
                double seconds = (samples[samples.Count - 1] - samples[0]) / (double)Stopwatch.Frequency;
                double rate = (samples.Count - 1) / seconds;
                double minimumMs = Double.MaxValue, maximumMs = 0;
                for (int i = 1; i < samples.Count; i++)
                {
                    double milliseconds = (samples[i] - samples[i - 1]) * 1000.0 / Stopwatch.Frequency;
                    minimumMs = Math.Min(minimumMs, milliseconds); maximumMs = Math.Max(maximumMs, milliseconds);
                }
                Console.WriteLine("      Real message-loop loading cadence: " + rate.ToString("F1") + " Hz, " + samples.Count +
                    " samples; mean " + (1000 / rate).ToString("F1") + " ms, range " + minimumMs.ToString("F1") + "–" + maximumMs.ToString("F1") + " ms");
                if (rate < 35 || rate > 75) throw new Exception("loading visual cadence outside shared-heartbeat range 35–75 Hz: " + rate);
                if (status.PaintCount - statusPaints < 10) throw new Exception("changing loading text was not painted through the buffered surface");
                if (loadingLayouts > 2) throw new Exception("changing loading text repeatedly reflowed the file/output table: " + loadingLayouts);
                form.Close();
            }
        }

        private static void TestKdmApiShortHotPath()
        {
            MidiEvent midiEvent = BuildSong(new long[] { 0 }).Events[0];
            BenchmarkKdmApiNative native = new BenchmarkKdmApiNative();
            using (KdmApiMidiOutput output = new KdmApiMidiOutput(native))
            {
                output.Open();
                for (int i = 0; i < 10000; i++) output.Send(midiEvent);
                int collections = GC.CollectionCount(0);
                for (int i = 0; i < 500000; i++) output.Send(midiEvent);
                Equal(collections, GC.CollectionCount(0), "KDMAPI short sends avoid empty-long-buffer allocations");
                Equal(510000L, native.ShortCount, "KDMAPI hot path preserves every short message");
            }
        }

        private static void TestBlockingOutputWorkerLifecycle()
        {
            MidiSong song = BuildSong(new long[] { 0, 500000, 1000000 });

            BlockingLifecycleOutput pauseOutput = new BlockingLifecycleOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.WorkerStopTimeoutMilliseconds = 500;
                engine.SimulateSlowdown = false;
                engine.Start(song, pauseOutput, ProcessingMode.Queue);
                try
                {
                    Equal(true, pauseOutput.Entered.WaitOne(1000), "pause worker entered blocking output");
                    Exception pauseFailure = null;
                    Thread pause = new Thread(new ThreadStart(delegate
                    {
                        try { engine.Pause(); }
                        catch (Exception ex) { pauseFailure = ex; }
                    }));
                    pause.Start();
                    Thread.Sleep(40);
                    Equal(true, pause.IsAlive, "Pause waits for the active output call");
                    Equal(0, pauseOutput.LastResetSequence, "Pause does not reset during an active send");
                    Equal(0, pauseOutput.LastPanicSequence, "Pause does not panic during an active send");
                    pauseOutput.Release.Set();
                    Equal(true, pause.Join(1500), "Pause completes after output returns");
                    if (pauseFailure != null) throw new Exception("ordinary blocking Pause failed", pauseFailure);
                    Equal(PlaybackState.Paused, engine.State, "Pause recreates a clean paused worker");
                    long pausedPosition = engine.GetSnapshot().IntendedTimelineMicroseconds;
                    Thread.Sleep(40);
                    Equal(pausedPosition, engine.GetSnapshot().IntendedTimelineMicroseconds, "paused source position remains stable");
                    Equal(1, pauseOutput.SendBeginCount, "paused replacement emits no stale event");
                    if (pauseOutput.LastResetSequence <= pauseOutput.FirstSendEndSequence ||
                        pauseOutput.LastPanicSequence <= pauseOutput.LastResetSequence)
                        throw new Exception("Pause did not reset then panic after the blocked send returned");
                    engine.Resume();
                    WaitFor(delegate { return pauseOutput.HasBegunNote(61); }, 1500, "post-Pause Resume event");
                    Equal(1, pauseOutput.MaximumConcurrentSends, "Pause/Resume workers never overlap");
                    int resumedSend = pauseOutput.FirstBeginSequenceForNote(61);
                    if (pauseOutput.LastPanicBefore(resumedSend) <= 0)
                        throw new Exception("Resume began before the retired worker's silence boundary");
                    engine.Stop();
                }
                finally { pauseOutput.Release.Set(); }
            }

            BlockingLifecycleOutput seekOutput = new BlockingLifecycleOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.WorkerStopTimeoutMilliseconds = 500;
                engine.SimulateSlowdown = false;
                engine.Start(song, seekOutput, ProcessingMode.Queue);
                try
                {
                    Equal(true, seekOutput.Entered.WaitOne(1000), "seek worker entered blocking output");
                    Exception seekFailure = null;
                    Thread seek = new Thread(new ThreadStart(delegate
                    {
                        try { engine.Seek(500000); }
                        catch (Exception ex) { seekFailure = ex; }
                    }));
                    seek.Start();
                    Thread.Sleep(40);
                    Equal(true, seek.IsAlive, "seek waits for the old worker instead of starting a replacement");
                    Equal(1, seekOutput.SendBeginCount, "no replacement send while old output call is blocked");
                    seekOutput.Release.Set();
                    Equal(true, seek.Join(1500), "seek completes after the output call returns");
                    if (seekFailure != null) throw new Exception("ordinary blocking seek failed", seekFailure);
                    WaitFor(delegate { return seekOutput.HasBegunNote(61); }, 1000, "post-seek target event");
                    Equal(1, seekOutput.MaximumConcurrentSends, "old and new workers never overlap");
                    int newWorkerSend = seekOutput.FirstBeginSequenceForNote(61);
                    if (newWorkerSend <= 0 || seekOutput.LastPanicBefore(newWorkerSend) <= 0)
                        throw new Exception("old worker reset/panic did not finish before post-seek output");
                    engine.Stop();
                }
                finally { seekOutput.Release.Set(); }
            }

            BlockingLifecycleOutput timeoutOutput = new BlockingLifecycleOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.WorkerStopTimeoutMilliseconds = 75;
                engine.SimulateSlowdown = false;
                engine.Start(song, timeoutOutput, ProcessingMode.Queue);
                try
                {
                    Equal(true, timeoutOutput.Entered.WaitOne(1000), "timeout worker entered blocking output");
                    bool timedOut = false;
                    try { engine.Seek(500000); }
                    catch (PlaybackWorkerTimeoutException ex)
                    {
                        timedOut = ex.Message.IndexOf("no replacement scheduler", StringComparison.OrdinalIgnoreCase) >= 0;
                    }
                    Equal(true, timedOut, "timed-out seek reports that no replacement was started");
                    Equal(PlaybackState.Stopped, engine.State, "timed-out seek remains stopped");
                    Equal(true, engine.HasLiveWorker, "blocked worker identity remains attached");
                    Equal(1, timeoutOutput.SendBeginCount, "timed-out seek starts no new output worker");
                    Equal(0, timeoutOutput.LastPanicSequence, "timed-out seek does not race panic against an active native call");
                    timeoutOutput.Release.Set();
                    WaitFor(delegate { return !engine.HasLiveWorker; }, 1000, "blocked worker final cleanup");
                    Equal(1, timeoutOutput.SendBeginCount, "old immediate range sends no stale events after unblock");
                    if (timeoutOutput.LastPanicSequence <= timeoutOutput.FirstSendEndSequence)
                        throw new Exception("no final post-return panic followed the stale native-call boundary");
                }
                finally { timeoutOutput.Release.Set(); }
            }

            BlockingLifecycleOutput pauseTimeoutOutput = new BlockingLifecycleOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.WorkerStopTimeoutMilliseconds = 75;
                engine.SimulateSlowdown = false;
                engine.Start(song, pauseTimeoutOutput, ProcessingMode.Queue);
                try
                {
                    Equal(true, pauseTimeoutOutput.Entered.WaitOne(1000), "Pause-timeout worker entered blocking output");
                    bool timedOut = false;
                    try { engine.Pause(); }
                    catch (PlaybackWorkerTimeoutException) { timedOut = true; }
                    Equal(true, timedOut, "timed-out Pause is explicit");
                    Equal(PlaybackState.Stopped, engine.State, "timed-out Pause remains safely stopped");
                    Equal(true, engine.HasLiveWorker, "timed-out Pause retains blocked worker identity");
                    Equal(0, pauseTimeoutOutput.LastPanicSequence, "timed-out Pause does not race panic");
                    pauseTimeoutOutput.Release.Set();
                    WaitFor(delegate { return !engine.HasLiveWorker; }, 1000, "Pause-timeout post-return cleanup");
                    Equal(1, pauseTimeoutOutput.SendBeginCount, "timed-out Pause emits no stale tail");
                    if (pauseTimeoutOutput.LastPanicSequence <= pauseTimeoutOutput.FirstSendEndSequence)
                        throw new Exception("timed-out Pause did not silence after output returned");
                }
                finally { pauseTimeoutOutput.Release.Set(); }
            }

            BlockingLifecycleOutput stopOutput = new BlockingLifecycleOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.WorkerStopTimeoutMilliseconds = 75;
                engine.SimulateSlowdown = false;
                engine.Start(song, stopOutput, ProcessingMode.Queue);
                try
                {
                    Equal(true, stopOutput.Entered.WaitOne(1000), "stop worker entered blocking output");
                    bool timedOut = false;
                    try { engine.Stop(); }
                    catch (PlaybackWorkerTimeoutException) { timedOut = true; }
                    Equal(true, timedOut, "timed-out Stop is explicit");
                    Equal(PlaybackState.Stopped, engine.State, "timed-out Stop state");
                    Equal(1, stopOutput.SendBeginCount, "timed-out Stop starts no replacement");
                    Equal(0, stopOutput.LastPanicSequence, "timed-out Stop does not race panic against an active native call");
                    stopOutput.Release.Set();
                    WaitFor(delegate { return !engine.HasLiveWorker; }, 1000, "Stop post-return cleanup");
                    if (stopOutput.LastPanicSequence <= stopOutput.FirstSendEndSequence)
                        throw new Exception("Stop did not silence again after the blocked send returned");
                }
                finally { stopOutput.Release.Set(); }
            }
        }

        private static void TestImmediateDispatchCadence()
        {
            const int count = 65536;
            const int checkpoint = 2048;
            MidiSong song = BuildSong(new long[count]);
            CadenceProbeOutput output = new CadenceProbeOutput(checkpoint, count);
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = false;
                engine.Start(song, output, ProcessingMode.Queue);
                Stopwatch timeout = Stopwatch.StartNew();
                while (engine.State == PlaybackState.Playing && timeout.ElapsedMilliseconds < 4000)
                {
                    // Model the main presentation thread sampling queue and
                    // timeline state while the scheduler crosses checkpoints.
                    engine.GetSnapshot();
                    Thread.Sleep(0);
                }
                if (engine.State == PlaybackState.Playing) throw new Exception("immediate cadence run timed out");
                Equal(count, output.Count, "immediate path preserves every payload");
                Equal(false, output.PayloadMismatch, "immediate path preserves established payload/order");
            }
            double[] intervals = output.CheckpointIntervalsMilliseconds();
            Array.Sort(intervals);
            double median = intervals.Length == 0 ? 0 : intervals[intervals.Length / 2];
            double maximum = intervals.Length == 0 ? 0 : intervals[intervals.Length - 1];
            Console.WriteLine("      Immediate checkpoint cadence: " + intervals.Length + " blocks; median " +
                median.ToString("F3", CultureInfo.InvariantCulture) + " ms, maximum " +
                maximum.ToString("F3", CultureInfo.InvariantCulture) + " ms");
            if (maximum > Math.Max(25.0, median * 20.0 + 2.0))
                throw new Exception("periodic immediate-dispatch checkpoint stall: median=" + median + ", max=" + maximum);
        }

        private static void TestStopwatchConversion()
        {
            long[] microseconds = new long[] { -1000001, -1, 0, 1, 999999, 1000000, 1234567890123 };
            for (int i = 0; i < microseconds.Length; i++)
            {
                long expected = (long)(((decimal)microseconds[i] * Stopwatch.Frequency) / 1000000m);
                Equal(expected, PlaybackEngine.MicrosecondsToTicks(microseconds[i]), "microseconds-to-ticks exact " + microseconds[i]);
            }
            long[] ticks = new long[] { -Stopwatch.Frequency - 1, -1, 0, 1, Stopwatch.Frequency - 1,
                Stopwatch.Frequency, checked(Stopwatch.Frequency * 1234567L + Stopwatch.Frequency / 3) };
            for (int i = 0; i < ticks.Length; i++)
            {
                long expected = (long)(((decimal)ticks[i] * 1000000m) / Stopwatch.Frequency);
                Equal(expected, PlaybackEngine.TicksToMicroseconds(ticks[i]), "ticks-to-microseconds exact " + ticks[i]);
            }
        }

        private static void BenchmarkWinMmAdapter()
        {
            const int count = 500000;
            MidiEvent midiEvent = BuildSong(new long[] { 0 }).Events[0];
            long calls = 0;
            using (WindowsMidiOutput output = new WindowsMidiOutput(delegate(IntPtr handle, uint message) { calls++; return 0; }))
            {
                for (int i = 0; i < 10000; i++) output.Send(midiEvent);
                for (int run = 0; run < 3; run++)
                {
                    int collections = GC.CollectionCount(0);
                    Stopwatch timer = Stopwatch.StartNew();
                    for (int i = 0; i < count; i++) output.Send(midiEvent);
                    timer.Stop();
                    Console.WriteLine("WinMM adapter: " + count + " sends in " + timer.Elapsed.TotalMilliseconds.ToString("F2") +
                        " ms; Gen0 collections=" + (GC.CollectionCount(0) - collections));
                }
                Equal(1510000L, calls, "all adapter calls preserved");
            }
        }

        private static void BenchmarkShortAdapters()
        {
            const int count = 500000;
            MidiEvent midiEvent = BuildSong(new long[] { 0 }).Events[0];
            BenchmarkKdmApiNative native = new BenchmarkKdmApiNative();
            using (WindowsMidiOutput winmm = new WindowsMidiOutput(delegate(IntPtr handle, uint message) { return 0; }))
            using (KdmApiMidiOutput kdmapi = new KdmApiMidiOutput(native))
            {
                kdmapi.Open();
                for (int i = 0; i < 20000; i++) { winmm.Send(midiEvent); kdmapi.Send(midiEvent); }
                for (int run = 0; run < 5; run++)
                {
                    MeasureAdapter("WinMM", winmm, midiEvent, count);
                    MeasureAdapter("KDMAPI", kdmapi, midiEvent, count);
                }
            }
            Equal(2520000L, native.ShortCount, "KDMAPI adapter benchmark preserves every short message");
        }

        private static void MeasureAdapter(string name, IMidiOutput output, MidiEvent midiEvent, int count)
        {
            int collections = GC.CollectionCount(0);
            Stopwatch timer = Stopwatch.StartNew();
            for (int i = 0; i < count; i++) output.Send(midiEvent);
            timer.Stop();
            Console.WriteLine(name + " adapter: " + count + " sends in " +
                timer.Elapsed.TotalMilliseconds.ToString("F2", CultureInfo.InvariantCulture) +
                " ms; Gen0 collections=" + (GC.CollectionCount(0) - collections));
        }

        private static void TestMidiEventStoreBoundary()
        {
            MidiSong legacy = BuildSong(new long[] { 0, 1000, 1000, 5000 });
            MidiEventReader legacyReader = legacy.GetEventReader();
            Equal(4, legacyReader.Count, "legacy reader count");
            Equal(true, Object.ReferenceEquals(legacy.Events[2], legacyReader[2]), "legacy identity remains stable");
            Equal(0, legacyReader.LowerBoundByTime(0), "lower bound at start");
            Equal(1, legacyReader.LowerBoundByTime(1), "lower bound between events");
            Equal(1, legacyReader.LowerBoundByTime(1000), "lower bound at duplicate timestamp");
            Equal(4, legacyReader.LowerBoundByTime(6000), "lower bound after end");

            MidiEvent[] immutableEvents = legacy.Events.ToArray();
            MidiSong indexed = new MidiSong
            {
                DurationMicroseconds = legacy.DurationMicroseconds,
                TrackCount = legacy.TrackCount,
                TicksPerQuarterNote = legacy.TicksPerQuarterNote
            };
            indexed.SetEventStore(new ArrayMidiEventStore(immutableEvents));
            Equal(true, indexed.Events == null, "non-list backend does not expose mutable legacy list");
            MidiEventReader indexedReader = indexed.GetEventReader();
            Equal(4, indexedReader.Count, "indexed backend count");
            Equal(1, indexedReader.LowerBoundByTime(1000), "indexed backend lower bound");
            Equal(true, Object.ReferenceEquals(immutableEvents[3], indexedReader[3]), "indexed backend identity remains stable");

            WorkloadAnalysis analysis = WorkloadAnalyzer.AnalyzeUncached(indexed, 1000,
                DefaultAnalysisConfiguration(), CancellationToken.None, null);
            Equal(4L, analysis.TotalEvents, "Analysis consumes indexed backend");
            using (PlaybackEngine engine = new PlaybackEngine())
            using (NullMidiOutput output = new NullMidiOutput())
            {
                engine.SimulateSlowdown = false;
                engine.Start(indexed, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 3000,
                    "indexed backend playback completion");
                Equal(4L, engine.GetSnapshot().ProcessedEvents, "playback consumes indexed backend");
            }

            // Reassigning the compatibility list invalidates the prior wrapper
            // without requiring test builders to know about the new boundary.
            legacy.Events = new List<MidiEvent> { immutableEvents[0] };
            Equal(1, legacy.EventStore.Count, "legacy list replacement refreshes backend");
        }

        private static void TestPackedMidiEventData()
        {
            MidiEvent two = new MidiEvent { Data = new byte[] { 0xC2, 0x7F } };
            MidiEvent three = new MidiEvent { Data = new byte[] { 0x91, 0x40, 0x55 } };
            Equal(false, two.Data.UsesHeapPayload, "two-byte message is inline");
            Equal(false, three.Data.UsesHeapPayload, "three-byte message is inline");
            Equal(2, two.Data.Length, "two-byte length");
            Equal((uint)0x00554091, three.Data.PackedShortMessage, "packed short value");
            Equal((byte)0x40, three.Data[1], "packed indexed byte");
            byte[] reconstructed = three.Data.ToArray();
            ByteSequence(new byte[] { 0x91, 0x40, 0x55 }, reconstructed, "explicit short reconstruction");

            byte[] sysex = new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
            MidiEvent longEvent = new MidiEvent
            {
                Kind = MidiEventKind.SystemExclusive,
                Status = 0xF0,
                Data = sysex
            };
            Equal(true, longEvent.Data.UsesHeapPayload, "long message retains side payload");
            byte[] packet = new SystemExclusiveAssembler().Accept(longEvent);
            ByteSequence(sysex, packet, "long payload remains byte exact");
            Equal(false, Object.ReferenceEquals(sysex, packet), "assembled packet owns its output buffer");

            byte[] largeSysEx = new byte[300];
            largeSysEx[0] = 0xF0;
            largeSysEx[299] = 0xF7;
            MidiEvent largeEvent = new MidiEvent
            {
                Kind = MidiEventKind.SystemExclusive,
                Status = 0xF0,
                Data = largeSysEx
            };
            Equal(300, largeEvent.DataLength, "long payload length is not limited to packed short length");
            ByteSequence(largeSysEx, new SystemExclusiveAssembler().Accept(largeEvent), "large long payload remains exact");

            using (NullMidiOutput output = new NullMidiOutput())
            {
                output.Send(two);
                output.Send(three);
                output.Send(longEvent);
            }
        }

        private static void BenchmarkEventStore(int eventCount)
        {
            if (eventCount < 1 || eventCount > 5000000) throw new ArgumentOutOfRangeException("eventCount");
            string path = CreateDenseMidiFile(eventCount);
            try
            {
                GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                long managedBefore = GC.GetTotalMemory(true);
                long privateBefore;
                using (Process process = Process.GetCurrentProcess()) { process.Refresh(); privateBefore = process.PrivateMemorySize64; }
                Stopwatch load = Stopwatch.StartNew();
                MidiSong song = MidiFileParser.Load(path);
                load.Stop();
                GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                long managedAfter = GC.GetTotalMemory(true);
                long privateAfter;
                using (Process process = Process.GetCurrentProcess()) { process.Refresh(); privateAfter = process.PrivateMemorySize64; }

                int shortPayloadArrays = 0;
                for (int i = 0; i < song.Events.Count; i++)
                    if (song.Events[i].Data.UsesHeapPayload && song.Events[i].Data.Length <= 3) shortPayloadArrays++;

                AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
                configuration.SimulateSlowdown = false;
                Stopwatch analysisTimer = Stopwatch.StartNew();
                WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, configuration);
                analysisTimer.Stop();

                Stopwatch playback = Stopwatch.StartNew();
                PlaybackSnapshot snapshot;
                using (NullMidiOutput output = new NullMidiOutput())
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.SimulateSlowdown = false;
                    engine.Start(song, output, ProcessingMode.Queue);
                    WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 30000, "event-store benchmark playback");
                    snapshot = engine.GetSnapshot();
                }
                playback.Stop();

                Console.WriteLine("Event-store benchmark: architecture={0}, events={1:N0}, backend={2}",
                    IntPtr.Size == 8 ? "x64" : "x86", song.Events.Count, song.Events.GetType().Name);
                Console.WriteLine("  load={0:F1} ms; managed delta={1:N0} bytes; private delta={2:N0} bytes; short payload arrays={3:N0}",
                    load.Elapsed.TotalMilliseconds, managedAfter - managedBefore, privateAfter - privateBefore, shortPayloadArrays);
                Console.WriteLine("  analysis={0:F1} ms; analyzed events={1:N0}", analysisTimer.Elapsed.TotalMilliseconds, analysis.TotalEvents);
                Console.WriteLine("  immediate None playback={0:F1} ms; processed={1:N0}; max lag={2:N0} us",
                    playback.Elapsed.TotalMilliseconds, snapshot.ProcessedEvents, snapshot.MaximumLagMicroseconds);
            }
            finally { DeleteFileWhenAvailable(path); }
        }

        private static void BenchmarkChannelMonitorOverhead()
        {
            MidiSong warmup = BuildSong(new long[10000]);
            MidiSong song = BuildSong(new long[1000000]);
            MeasureChannelMonitorRun(warmup, false);
            MeasureChannelMonitorRun(warmup, true);
            MeasureChannelOverrideRun(warmup);
            double[] closed = new double[5];
            double[] open = new double[5];
            double[] overridden = new double[5];
            for (int i = 0; i < closed.Length; i++)
            {
                closed[i] = MeasureChannelMonitorRun(song, false);
                open[i] = MeasureChannelMonitorRun(song, true);
                overridden[i] = MeasureChannelOverrideRun(song);
            }
            Array.Sort(closed);
            Array.Sort(open);
            Array.Sort(overridden);
            Console.WriteLine("Channel-monitor None benchmark: architecture={0}, events={1:N0}",
                IntPtr.Size == 8 ? "x64" : "x86", song.Events.Count);
            Console.WriteLine("  monitor closed: median={0:F1} ms; range={1:F1}–{2:F1} ms", closed[2], closed[0], closed[4]);
            Console.WriteLine("  monitor open:   median={0:F1} ms; range={1:F1}–{2:F1} ms", open[2], open[0], open[4]);
            Console.WriteLine("  open overhead:  {0:F1}%", (open[2] / closed[2] - 1.0) * 100.0);
            Console.WriteLine("  closed + one active override: median={0:F1} ms; range={1:F1}–{2:F1} ms; overhead={3:F1}%",
                overridden[2], overridden[0], overridden[4], (overridden[2] / closed[2] - 1.0) * 100.0);
        }

        private static double MeasureChannelMonitorRun(MidiSong song, bool enabled)
        {
            using (NullMidiOutput output = new NullMidiOutput())
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SetChannelMonitoring(enabled);
                engine.SimulateSlowdown = false;
                Stopwatch timer = Stopwatch.StartNew();
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 30000,
                    "channel-monitor benchmark playback");
                timer.Stop();
                if (enabled)
                    Equal((long)song.Events.Count, engine.GetChannelSnapshot().Channels[0].SentEvents,
                        "channel-monitor benchmark sent count");
                return timer.Elapsed.TotalMilliseconds;
            }
        }

        private static double MeasureChannelOverrideRun(MidiSong song)
        {
            using (NullMidiOutput output = new NullMidiOutput())
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SetChannelOverride(0, ChannelAttribute.Program, 12);
                engine.SimulateSlowdown = false;
                Stopwatch timer = Stopwatch.StartNew();
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 30000,
                    "active channel-override benchmark playback");
                timer.Stop();
                Equal((long)song.Events.Count, engine.GetSnapshot().ProcessedEvents,
                    "active override benchmark preserves event processing");
                return timer.Elapsed.TotalMilliseconds;
            }
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

        private static void RenderMainWindow(string outputPath, bool bitrateMode, bool minimumSize, bool finite,
            bool noOutput = false)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show();
                Application.DoEvents();
                if (noOutput)
                {
                    List<Control> outputControls = new List<Control>();
                    CollectControls(form, outputControls);
                    ComboBox output = FindMidiOutputCombo(outputControls);
                    if (output == null) throw new Exception("MIDI output selector was not found for None rendering");
                    for (int i = 0; i < output.Items.Count; i++)
                        if (MainForm.IsNoOutputSelection(output.Items[i])) output.SelectedIndex = i;
                    Application.DoEvents();
                }
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

        private static void RenderChannelMonitor(string outputPath, bool stale, bool forced)
        {
            Application.EnableVisualStyles();
            ChannelStateTracker tracker = new ChannelStateTracker();
            tracker.RecordSuccessful(ChannelMessage(77283000, 0xB0, 0, 0));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0xB0, 32, 0));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0xC0, 40));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0xB0, 7, 104));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0xB0, 11, 127));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0xB0, 10, 64));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0x90, 60, 100));
            tracker.RecordSuccessful(ChannelMessage(77283000, 0x90, 64, 100));
            tracker.RecordDropped(ChannelMessage(77283000, 0x91, 67, 100));
            if (stale) tracker.PauseBoundaryDirect();
            ChannelPlaybackSnapshot snapshot = tracker.CreateSnapshot();
            if (forced)
            {
                ChannelOverrideState overrides = new ChannelOverrideState();
                overrides.SetValue(0, ChannelAttribute.Program, 40);
                overrides.SetValue(0, ChannelAttribute.Volume, 110);
                overrides.TakePendingMask(0);
                tracker.RecordOverrideApplied(0, ChannelAttribute.Program, 40);
                tracker.RecordOverrideApplied(0, ChannelAttribute.Volume, 110);
                snapshot = tracker.CreateSnapshot();
                overrides.ApplyToSnapshot(snapshot);
            }
            using (ChannelMonitorForm form = new ChannelMonitorForm("example.mid"))
            {
                form.Show(); Application.DoEvents();
                form.UpdateSnapshot(snapshot);
                Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered channel monitor: " + Path.GetFullPath(outputPath));
        }

        private static void RenderAutomaticAnalysisWindow(string outputPath)
        {
            Application.EnableVisualStyles();
            MidiSong song = BuildSong(new long[] { 0, 30000000, 60000000, 120000000, 180000000, 240000000, 300000000 });
            song.FilePath = "whole-file-workload-example.mid";
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 1000000, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered automatic-resolution Analysis: " + Path.GetFullPath(outputPath));
        }

        private static void RenderAutomaticAnalysisWindowFromFile(string midiPath, string outputPath)
        {
            Application.EnableVisualStyles();
            MidiSong song = MidiFileParser.Load(midiPath);
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(outputPath);
                }
                form.Close();
            }
            Console.WriteLine("Rendered default Analysis command bar: " + Path.GetFullPath(outputPath));
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
                else
                {
                    // Allow a deliberate below-minimum visual experiment
                    // without changing the product constraint first.
                    form.ClientSize = new Size(639, form.ClientSize.Height);
                    Application.DoEvents();
                    form.MaximumSize = Size.Empty;
                    form.MinimumSize = Size.Empty;
                }
                form.ClientSize = new Size(clientWidth, 570);
                Application.DoEvents();
                AssertProcessingClusters(form, "rendered compact client width " + clientWidth);
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

        private static void RenderLoadingMainWindow(string midiPath, string outputPath, bool compact)
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.SuppressLoadErrorDialogs = true;
                form.Show(); Application.DoEvents();
                if (compact) { form.Size = form.MinimumSize; Application.DoEvents(); }
                form.BeginMidiLoad(midiPath);
                PumpUntil(delegate
                {
                    return !form.IsLoadingSong || form.LoadingOverallPermille >= 5;
                }, 10000, "loading-progress visual state");
                if (!form.IsLoadingSong)
                    throw new Exception("the selected MIDI completed before its loading-progress state could be captured");
                if (compact)
                {
                    var flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
                    typeof(MainForm).GetField("_loadingFileName", flags).SetValue(form,
                        "An exceptionally long black-MIDI workload filename for compact loading.mid");
                    typeof(MainForm).GetMethod("UpdateLoadingFileTelemetry", flags).Invoke(form, new object[] { true });
                    Application.DoEvents();
                }
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

        private static void TestCompleteNoteOverflow()
        {
            MidiSong song = BuildCompleteNotePolicySong();
            FakeMidiOutput output = new FakeMidiOutput();
            PlaybackSnapshot snapshot;
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 1000;
                engine.QueueLengthLimit = 1;
                engine.OverflowPolicy = OverflowPolicy.DropIncomingCompleteNotes;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "complete-note policy playback");
                snapshot = engine.GetSnapshot();
                Sequence(new int[] { 0, 2, 4, 7 }, output.SentEventIndices(), "complete-note retained events");
                Equal(4L, snapshot.ProcessedEvents, "complete-note processed count");
                Equal(4L, snapshot.DroppedEvents, "rejected note-ons and paired note-offs both count as dropped");
                if (snapshot.MaximumQueueLength < 2)
                    throw new Exception("soft-limit safety admissions were not represented in queue occupancy");
            }

            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.ProcessingMicroseconds = 1000;
            configuration.QueueLengthLimitEnabled = true;
            configuration.QueueLengthLimit = 1;
            configuration.OverflowPolicy = OverflowPolicy.DropIncomingCompleteNotes;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 1000, configuration);
            Equal(snapshot.DroppedEvents, analysis.PredictedDroppedEvents, "Analysis complete-note drops match production");
            Equal((int)snapshot.MaximumQueueLength, analysis.PredictedMaximumOccupancy,
                "Analysis complete-note occupancy matches production including protected and in-service slots");
            Equal(3L, snapshot.MaximumQueueLength,
                "protected events consume ordinary slots and may raise the complete-note soft ceiling above capacity");

            // A seek that begins after the NoteOn sees an unmatched NoteOff.
            // It is conservatively retained rather than risking a stuck voice.
            MidiSong seekSong = new MidiSong { FilePath = "note-seek.mid", Format = 0, TrackCount = 1,
                TicksPerQuarterNote = 480, Events = new List<MidiEvent>(), DurationMicroseconds = 2000 };
            seekSong.Events.Add(ChannelEvent(0, 0x90, 64, 100, 0));
            seekSong.Events.Add(ChannelEvent(1000, 0x80, 64, 0, 1));
            FakeMidiOutput seekOutput = new FakeMidiOutput();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 1000;
                engine.QueueLengthLimit = 1;
                engine.OverflowPolicy = OverflowPolicy.DropIncomingCompleteNotes;
                engine.Start(seekSong, seekOutput, ProcessingMode.Drop, 500);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "mid-note seek complete-note policy");
                Sequence(new int[] { 1 }, seekOutput.SentEventIndices(), "unmatched NoteOff after seek retained");
            }

            using (StatisticsView statistics = new StatisticsView())
            {
                statistics.SetQueuePressure(true, 3, 1, true);
                Near(3.0, statistics.QueuePressureRatio, 0.0001, "pressure meter exposes soft-limit excess above 100 percent");
                using (Bitmap bitmap = new Bitmap(700, 104))
                {
                    statistics.Size = bitmap.Size;
                    statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
                }
            }
        }

        private static void TestClearCatchUpOptimization()
        {
            const int count = 200000;
            MidiSong song = BuildSong(new long[count]);
            Stopwatch linear = Stopwatch.StartNew();
            int linearIndex = 0;
            while (linearIndex < song.Events.Count && song.Events[linearIndex].IntendedMicroseconds <= 0) linearIndex++;
            linear.Stop();
            Equal(count, linearIndex, "linear reference covers every overdue event");

            FakeMidiOutput output = new FakeMidiOutput();
            Stopwatch optimized = Stopwatch.StartNew();
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 1000000;
                engine.QueueLengthLimit = 1;
                engine.OverflowPolicy = OverflowPolicy.ClearBufferAndCatchUp;
                engine.Start(song, output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "binary clear-buffer catch-up");
                PlaybackSnapshot result = engine.GetSnapshot();
                Equal((long)count, result.DroppedEvents, "binary catch-up exact skipped count");
                Equal(0L, result.ProcessedEvents, "binary catch-up dispatches no stale overdue event");
                Equal(0L, result.OutstandingEvents, "binary catch-up clears outstanding work");
            }
            optimized.Stop();
            Console.WriteLine("      Clear catch-up 200,000 overdue events: linear reference " +
                linear.Elapsed.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture) + " ms; production binary path " +
                optimized.Elapsed.TotalMilliseconds.ToString("F3", CultureInfo.InvariantCulture) + " ms including worker/panic");
            if (optimized.ElapsedMilliseconds > 1000) throw new Exception("binary clear-buffer catch-up exceeded bounded runtime");
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
            byte[] longPayload = new byte[100];
            longPayload[0] = 0xF0;
            longPayload[99] = 0xF7;
            song.Events[2].Data = longPayload;
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
                Equal(reset.OutstandingEvents, reset.MaximumQueueLength,
                    "statistics reset maximum queue baseline includes pending and in-service work");
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

        private static void TestObservedMaximumOutputRate()
        {
            RollingOutputRate rate = new RollingOutputRate();
            Equal(null, rate.MaximumObserved, "observed peak begins unavailable");
            Equal(null, rate.Add(0, 0), "observed peak initial sample");
            Near(1000, rate.Add(100, 100000).Value, 0.01, "first live rolling output rate");
            Near(1000, rate.MaximumObserved.Value, 0.01, "first rolling rate becomes peak");
            Near(2000, rate.Add(400, 200000).Value, 0.01, "higher rolling output rate");
            Near(2000, rate.MaximumObserved.Value, 0.01, "observed peak retains higher rate");

            rate.RestartWindow();
            Equal(null, rate.Add(400, 300000), "Pause/Resume window restart needs a fresh sample");
            Near(500, rate.Add(450, 400000).Value, 0.01, "post-resume lower live rate");
            Near(2000, rate.MaximumObserved.Value, 0.01, "Pause/Resume preserves observed peak");
            rate.RestartWindow();
            Near(2000, rate.MaximumObserved.Value, 0.01, "Stop/completion retain final observed peak");

            PlaybackSnapshot slowdownOff = new PlaybackSnapshot
            {
                SimulateSlowdown = false,
                ServiceDurationMode = ServiceDurationMode.ProcessingTime,
                ProcessingMicroseconds = 1234
            };
            Equal("—", MainForm.FormatMaximumRate(slowdownOff, null, false),
                "immediate slowdown-off rate is unavailable before samples");
            if (MainForm.FormatMaximumRate(slowdownOff, 1234567.8, false).IndexOf("1,234,567.8", StringComparison.Ordinal) < 0)
                throw new Exception("slowdown-off maximum rate did not use the observed rolling peak");

            PlaybackSnapshot enabledZero = new PlaybackSnapshot
            {
                SimulateSlowdown = true,
                ServiceDurationMode = ServiceDurationMode.ProcessingTime,
                ProcessingMicroseconds = 0
            };
            if (MainForm.FormatMaximumRate(enabledZero, 7654321, true).IndexOf("7,654,321", StringComparison.Ordinal) < 0)
                throw new Exception("enabled zero-service maximum rate did not use the observed rolling peak");

            PlaybackSnapshot nonzero = new PlaybackSnapshot
            {
                SimulateSlowdown = true,
                ServiceDurationMode = ServiceDurationMode.ProcessingTime,
                ProcessingMicroseconds = 1000
            };
            Equal("1,000.0 events/sec", MainForm.FormatMaximumRate(nonzero, 9999999, false),
                "nonzero processing retains theoretical maximum");
            PlaybackSnapshot bitrate = new PlaybackSnapshot
            {
                SimulateSlowdown = true,
                ServiceDurationMode = ServiceDurationMode.MidiBitrate,
                ProcessingMicroseconds = 0,
                MidiBitrate = 31250
            };
            Equal("31,250 bit/s", MainForm.FormatMaximumRate(bitrate, 9999999, false),
                "serial bitrate retains configured theoretical value");

            rate.Reset();
            Equal(null, rate.MaximumObserved, "statistics reset clears observed peak");
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

        private static void TestNullMidiOutputContract()
        {
            MidiEvent shortMessage = ChannelEvent(new byte[] { 0x90, 60, 100 });
            MidiEvent systemExclusive = SysExEvent(0xF0,
                new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 });
            using (NullMidiOutput output = new NullMidiOutput())
            {
                output.Open();
                output.SourceFile = "diagnostic.mid";
                output.Send(shortMessage);
                output.Send(systemExclusive);
                output.Send(null);
                output.Reset();
                output.Panic();

                for (int i = 0; i < 10000; i++) output.Send(shortMessage);
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
                int collections = GC.CollectionCount(0);
                for (int i = 0; i < 1000000; i++) output.Send(shortMessage);
                Equal(collections, GC.CollectionCount(0),
                    "None output successful-send hot path performs no managed allocations");

                output.Close();
                Equal(null, output.SourceFile, "None output close clears source context");
            }
        }

        private static void TestNullOutputSelection()
        {
            List<MidiOutputDeviceInfo> devices = new List<MidiOutputDeviceInfo>();
            devices.Add(new MidiOutputDeviceInfo { DeviceId = 7, Name = "First native device" });
            devices.Add(new MidiOutputDeviceInfo { DeviceId = 42, Name = "Second native device" });
            List<object> selections = MainForm.CreateOutputSelections(devices);
            Equal(3, selections.Count, "None plus native selector item count");
            Equal(true, MainForm.IsNoOutputSelection(selections[0]), "None is a distinct selector item");
            Equal(7u, ((MidiOutputDeviceInfo)selections[1]).DeviceId,
                "synthetic None entry does not offset first native device id");
            Equal(42u, ((MidiOutputDeviceInfo)selections[2]).DeviceId,
                "synthetic None entry does not offset second native device id");
            Equal(1, MainForm.DefaultOutputSelectionIndex(devices),
                "first real device remains the default when available");

            List<object> noDevices = MainForm.CreateOutputSelections(new List<MidiOutputDeviceInfo>());
            Equal(1, noDevices.Count, "None remains available with no native devices");
            Equal(true, MainForm.IsNoOutputSelection(noDevices[0]), "no-device selector contains None");
            Equal(0, MainForm.DefaultOutputSelectionIndex(new List<MidiOutputDeviceInfo>()),
                "None is selected by default with no native devices");

            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show();
                Application.DoEvents();
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                ComboBox output = FindMidiOutputCombo(controls);
                CheckBox kdmApi = FindCheckBox(controls, "KDMAPI");
                if (output == null || kdmApi == null) throw new Exception("output controls were not found");
                int noneIndex = -1;
                for (int i = 0; i < output.Items.Count; i++)
                    if (MainForm.IsNoOutputSelection(output.Items[i])) noneIndex = i;
                if (noneIndex < 0) throw new Exception("None output was not present in the realized selector");
                output.SelectedIndex = noneIndex;
                Application.DoEvents();
                Equal(true, output.Enabled, "None remains an actionable output selection");
                AssertComboFullyVisible(output, NullMidiOutput.DisplayName, "default None output selector");
                form.Size = new Size(500, 500);
                Application.DoEvents();
                AssertComboFullyVisible(output, NullMidiOutput.DisplayName, "compact None output selector");
                form.Size = new Size(790, 660);
                Application.DoEvents();
                AssertComboFullyVisible(output, NullMidiOutput.DisplayName, "restored None output selector");
                kdmApi.Checked = true;
                Application.DoEvents();
                Equal(false, output.Enabled, "KDMAPI is a separate active output and disables the WinMM/None list");
                kdmApi.Checked = false;
                Application.DoEvents();
                Equal(true, output.Enabled, "None selection returns after leaving KDMAPI mode");
                Equal(true, MainForm.IsNoOutputSelection(output.SelectedItem),
                    "KDMAPI toggling does not silently replace the selected None sink");
                form.Close();
            }
        }

        private static void TestNullOutputPlayback()
        {
            MidiSong song = BuildSong(new long[] { 0, 20000, 40000, 60000, 80000 });
            song.Events[1].Kind = MidiEventKind.SystemExclusive;
            song.Events[1].Channel = -1;
            song.Events[1].Status = 0xF0;
            song.Events[1].Data = new byte[] { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
            using (NullMidiOutput output = new NullMidiOutput())
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                output.Open();
                engine.SimulateSlowdown = false;
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000,
                    "None output immediate playback completion");
                PlaybackSnapshot completed = engine.GetSnapshot();
                Equal(5L, completed.ProcessedEvents, "None output advances processed-event statistics");
                Equal(0L, completed.DroppedEvents, "None output does not alter simulator drops");
                Equal(80000L, completed.LastDispatchedTimelineMicroseconds,
                    "None output advances the MIDI output frontier");
                engine.Unload();
                Equal(null, output.SourceFile, "None output detaches song context on unload");
            }

            MidiSong restartSong = BuildSong(new long[] { 0, 50000, 100000, 150000, 200000 });
            FakeMidiOutput realBoundary = new FakeMidiOutput();
            using (NullMidiOutput none = new NullMidiOutput())
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 100000;
                engine.Start(restartSong, realBoundary, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().PlaybackMicroseconds >= 60000; }, 1000,
                    "source position before real-to-None restart");
                engine.Pause();
                long restartPosition = engine.GetSnapshot().IntendedTimelineMicroseconds;
                engine.Stop();
                none.Open();
                engine.Start(restartSong, none, ProcessingMode.Queue, restartPosition, true);
                PlaybackSnapshot restarted = engine.GetSnapshot();
                Equal(PlaybackState.Paused, restarted.State, "real-to-None restart preserves paused state");
                Near(restartPosition, restarted.IntendedTimelineMicroseconds, 2,
                    "real-to-None restart preserves source position");
                Equal(0L, restarted.ProcessedEvents, "real-to-None restart clears statistics");
                Equal(0L, restarted.OutstandingEvents, "real-to-None restart clears backlog");
                engine.Resume();
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000,
                    "real-to-None restarted playback completion");
                if (realBoundary.ResetCount < 1 || realBoundary.PanicCount < 1)
                    throw new Exception("real output was not reset and silenced at the restart boundary");
            }
        }

        private static void TestChannelStateMonitor()
        {
            ChannelStateTracker tracker = new ChannelStateTracker();
            tracker.RecordSuccessful(ChannelMessage(0, 0x90, 60, 100));
            tracker.RecordSuccessful(ChannelMessage(1000, 0x90, 60, 90));
            MidiChannelSnapshot channel = tracker.CreateSnapshot().Channels[0];
            Equal(1, channel.KeysDown, "overlapping occurrences count one distinct key down");
            Equal(1, channel.PeakKeysDown, "peak keys uses distinct MIDI keys");
            tracker.RecordSuccessful(ChannelMessage(2000, 0x80, 60, 0));
            Equal(1, tracker.CreateSnapshot().Channels[0].KeysDown,
                "first NoteOff retains the overlapping same-key occurrence");
            tracker.RecordSuccessful(ChannelMessage(3000, 0x90, 60, 0));
            tracker.RecordSuccessful(ChannelMessage(4000, 0x80, 60, 0));
            Equal(0, tracker.CreateSnapshot().Channels[0].KeysDown,
                "velocity-zero and unmatched NoteOff never create a negative key count");

            tracker.RecordSuccessful(ChannelMessage(5000, 0xB0, 0, 5));
            tracker.RecordSuccessful(ChannelMessage(6000, 0xB0, 32, 7));
            tracker.RecordSuccessful(ChannelMessage(7000, 0xC0, 41));
            tracker.RecordSuccessful(ChannelMessage(8000, 0xB0, 7, 100));
            tracker.RecordSuccessful(ChannelMessage(9000, 0xB0, 11, 80));
            tracker.RecordSuccessful(ChannelMessage(10000, 0xB0, 10, 64));
            tracker.RecordSuccessful(ChannelMessage(11000, 0xB0, 64, 127));
            tracker.RecordSuccessful(ChannelMessage(12000, 0xE0, 0, 64));
            tracker.RecordSuccessful(ChannelMessage(13000, 0xD0, 55));
            tracker.RecordDropped(ChannelMessage(14000, 0x91, 64, 100));
            ChannelPlaybackSnapshot controllerSnapshot = tracker.CreateSnapshot();
            channel = controllerSnapshot.Channels[0];
            Equal(5, channel.BankMsb, "bank MSB tracks successful dispatch");
            Equal(7, channel.BankLsb, "bank LSB tracks successful dispatch");
            Equal(41, channel.Program, "program tracks successful dispatch");
            Equal(100, channel.Volume, "CC7 tracks successful dispatch");
            Equal(80, channel.Expression, "CC11 tracks successful dispatch");
            Equal(64, channel.Pan, "CC10 tracks successful dispatch");
            Equal(1, channel.Sustain, "CC64 tracks successful dispatch");
            Equal(0, channel.PitchBend, "pitch bend is centered at zero");
            Equal(55, channel.ChannelPressure, "channel pressure tracks successful dispatch");
            Equal(1L, controllerSnapshot.Channels[1].DroppedEvents, "dropped channel event is counted separately");

            tracker.RecordSuccessful(ChannelMessage(15000, 0x90, 62, 100));
            tracker.RequestStatisticsReset();
            channel = tracker.CreateSnapshot().Channels[0];
            Equal(1, channel.KeysDown, "Reset stats retains live key state");
            Equal(1, channel.PeakKeysDown, "Reset stats rebases peak to current keys");
            Equal(0L, channel.SentEvents, "Reset stats clears sent channel count");
            Equal(41, channel.Program, "Reset stats retains known program");
            tracker.PanicDirect();
            channel = tracker.CreateSnapshot().Channels[0];
            Equal(0, channel.KeysDown, "panic clears keys");
            Equal(0, channel.Sustain, "panic clears sustain");
            Equal(41, channel.Program, "panic retains unaffected known program");
            tracker.RecordDropped(ChannelMessage(15500, 0x91, 65, 100));
            tracker.PauseBoundaryDirect();
            channel = tracker.CreateSnapshot().Channels[0];
            Equal(41, channel.Program, "Pause retains the last observed program");
            Equal(100, channel.Volume, "Pause retains the last observed controller state");
            Equal(true, (channel.HistoricalAttributeMask & (1 << (int)ChannelAttribute.Program)) != 0,
                "Pause marks retained program state historical after provider reset");
            tracker.SeekBoundaryDirect();
            channel = tracker.CreateSnapshot().Channels[0];
            Equal(0L, channel.SentEvents, "Seek clears sent channel count");
            Equal(0, channel.PeakKeysDown, "Seek clears peak keys");
            Equal(41, channel.Program, "Seek retains the last observed program");
            Equal(1L, tracker.CreateSnapshot().Channels[1].DroppedEvents, "Seek preserves dropped channel count");
            tracker.StopBoundaryDirect();
            channel = tracker.CreateSnapshot().Channels[0];
            Equal(-1, channel.Program, "Stop clears observed program state");
            Equal(-1, channel.Volume, "Stop clears observed controller state");
            Equal(0L, channel.SentEvents, "Stop clears sent channel count");

            MidiSong liveSong = new MidiSong
            {
                FilePath = "channel-monitor.mid",
                Format = 0,
                TrackCount = 1,
                TicksPerQuarterNote = 480,
                Events = new List<MidiEvent>(),
                DurationMicroseconds = 1000000
            };
            liveSong.Events.Add(ChannelMessage(0, 0xC0, 9));
            liveSong.Events.Add(ChannelMessage(0, 0x90, 60, 100));
            liveSong.Events.Add(ChannelMessage(1000000, 0x80, 60, 0));
            using (NullMidiOutput output = new NullMidiOutput())
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = false;
                output.Open();
                engine.Start(liveSong, output, ProcessingMode.Queue);
                WaitFor(delegate
                {
                    ChannelPlaybackSnapshot value = engine.GetChannelSnapshot();
                    return value != null && value.Channels[0].SentEvents >= 2;
                }, 1000,
                    "channel state after successful None dispatch");
                ChannelPlaybackSnapshot running = engine.GetChannelSnapshot();
                Equal(1, running.Channels[0].KeysDown, "engine publishes dispatched key state");
                Equal(2L, running.Channels[0].SentEvents, "engine publishes successful channel-event count");
                engine.ResetStatistics();
                ChannelPlaybackSnapshot reset = engine.GetChannelSnapshot();
                Equal(1, reset.Channels[0].KeysDown, "engine Reset stats retains keys down");
                Equal(0L, reset.Channels[0].SentEvents, "engine Reset stats clears channel count");
                engine.Pause();
                ChannelPlaybackSnapshot paused = engine.GetChannelSnapshot();
                Equal(0, paused.Channels[0].KeysDown, "Pause provider reset clears channel keys");
                Equal(9, paused.Channels[0].Program, "Pause retains the last observed channel program");
                Equal(true, (paused.Channels[0].HistoricalAttributeMask & (1 << (int)ChannelAttribute.Program)) != 0,
                    "Pause identifies retained attributes as historical");
                engine.Stop();
                Equal(-1, engine.GetChannelSnapshot().Channels[0].Program, "Stop clears monitor observations");
            }

            OverflowPolicy[] policies = new OverflowPolicy[]
            {
                OverflowPolicy.DropNewest,
                OverflowPolicy.DropOldest,
                OverflowPolicy.ClearBufferAndCatchUp,
                OverflowPolicy.DropIncomingCompleteNotes
            };
            for (int policyIndex = 0; policyIndex < policies.Length; policyIndex++)
            {
                MidiSong overflowSong = BuildCompleteNotePolicySong();
                using (NullMidiOutput output = new NullMidiOutput())
                using (PlaybackEngine engine = new PlaybackEngine())
                {
                    engine.SetChannelMonitoring(true);
                    engine.SimulateSlowdown = true;
                    engine.ProcessingMicroseconds = 100000;
                    engine.QueueLengthLimit = 1;
                    engine.OverflowPolicy = policies[policyIndex];
                    output.Open();
                    engine.Start(overflowSong, output, ProcessingMode.Drop);
                    WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 3000,
                        policies[policyIndex] + " channel-drop completion");
                    PlaybackSnapshot playback = engine.GetSnapshot();
                    ChannelPlaybackSnapshot tracked = engine.GetChannelSnapshot();
                    long trackedDropped = 0;
                    for (int channelIndex = 0; channelIndex < 16; channelIndex++)
                        trackedDropped += tracked.Channels[channelIndex].DroppedEvents;
                    Equal(playback.DroppedEvents, trackedDropped,
                        policies[policyIndex] + " channel drops equal scheduler drops");
                }
            }

            using (PlaybackEngine failingEngine = new PlaybackEngine())
            {
                failingEngine.SetChannelMonitoring(true);
                failingEngine.SimulateSlowdown = false;
                failingEngine.Start(BuildSong(new long[] { 0 }), new ThrowingMidiOutput(), ProcessingMode.Queue);
                WaitFor(delegate { return failingEngine.State == PlaybackState.Stopped; }, 1000,
                    "channel output failure");
                Equal(0L, failingEngine.GetChannelSnapshot().Channels[0].SentEvents,
                    "failed output call is not published as dispatched");
            }
        }

        private static void TestChannelMonitorInterface()
        {
            Application.EnableVisualStyles();
            using (ChannelMonitorForm monitor = new ChannelMonitorForm("synthetic.mid"))
            {
                monitor.Show(); Application.DoEvents();
                Equal(16, monitor.ChannelRowCount, "channel monitor contains exactly 16 rows");
                Equal("—", monitor.CellText(0, "Program"), "unknown channel value uses an em dash");
                ChannelStateTracker tracker = new ChannelStateTracker();
                tracker.RecordSuccessful(ChannelMessage(1234567, 0xC0, 4));
                tracker.RecordSuccessful(ChannelMessage(1234567, 0x90, 60, 100));
                monitor.UpdateSnapshot(tracker.CreateSnapshot());
                Equal("5 — Electric Piano 1", monitor.CellText(0, "Program"), "channel monitor displays program with GM reference name");
                Equal("1", monitor.CellText(0, "KeysDown"), "channel monitor displays live key count");
                Equal("00:01.234", monitor.CellText(0, "Position"), "channel monitor displays output position");
                tracker.PauseBoundaryDirect();
                monitor.UpdateSnapshot(tracker.CreateSnapshot());
                Equal(Color.DimGray, monitor.CellForeColor(0, "Program"), "retained post-reset state is visibly subdued");
                int requestedValue = Int32.MinValue;
                monitor.OverrideRequested += delegate(object sender, ChannelOverrideRequestEventArgs request)
                {
                    if (request.Channel == 0 && request.Attribute == ChannelAttribute.Volume) requestedValue = request.Value;
                };
                monitor.RequestOverrideForTesting(0, ChannelAttribute.Volume, 88);
                Equal(88, requestedValue, "channel monitor exposes a bounded override request interaction");
                monitor.Close();
            }

            string path = Path.Combine(Path.GetTempPath(), "midi-channel-monitor-" + Guid.NewGuid().ToString("N") + ".mid");
            File.WriteAllBytes(path, BuildTestMidi());
            try
            {
                using (MainForm form = new MainForm())
                {
                    form.SuppressLoadErrorDialogs = true;
                    form.Show(); Application.DoEvents();
                    form.BeginMidiLoad(path);
                    PumpUntil(delegate { return !form.IsLoadingSong; }, 5000, "channel monitor source load");
                    form.ShowChannelMonitorForTesting();
                    Application.DoEvents();
                    ChannelMonitorForm monitor = form.ChannelMonitorForTesting;
                    if (monitor == null || monitor.IsDisposed) throw new Exception("Channels command did not open the monitor");
                    Equal(null, monitor.Owner, "channel monitor is unowned and modeless");
                    Equal(16, monitor.ChannelRowCount, "main form channel monitor has 16 rows");
                    form.UnloadCurrentSong();
                    Application.DoEvents();
                    Equal(true, monitor.IsDisposed, "song unload closes its channel monitor");
                    Equal(null, form.ChannelMonitorForTesting, "song unload releases channel monitor reference");
                    form.Close();
                }
            }
            finally { File.Delete(path); }
        }

        private static void TestBuild20ScrubEditor()
        {
            Application.EnableVisualStyles();
            List<int> requested = new List<int>();
            using (ScrubOrTypeTextBox editor = new ScrubOrTypeTextBox())
            {
                editor.ValueRequested += delegate(object sender, ScrubValueEventArgs e) { requested.Add(e.Value); };
                editor.AutoRequested += delegate(object sender, ScrubValueEventArgs e) { requested.Add(e.Value); };
                editor.Configure(40, 0, 127, 1, 1, 8, 1, 16, false, false,
                    delegate(int value) { return (value + 1).ToString() + " — Violin"; }, "Program help");
                Equal(0, requested.Count, "activation/configuration sends no override request");
                if (editor.Text.IndexOf("41", StringComparison.Ordinal) < 0 || editor.Text.IndexOf("Violin", StringComparison.Ordinal) < 0)
                    throw new Exception("flat Program display omitted its 1-based value or GM name");

                editor.BeginPointerGesture(new Point(100, 100), MouseButtons.Left);
                editor.ContinuePointerGestureForTesting(new Point(103, 100), false, true);
                Equal(false, editor.IsScrubbing, "three pixels remains a click gesture");
                Equal(0, requested.Count, "click threshold sends no request");
                editor.EndPointerGesture();
                Equal(true, editor.IsTyping, "mouse-up without a drag enters typing mode");
                editor.EscapeForTesting();
                Equal(false, editor.IsTyping, "Escape exits typing mode");

                editor.BeginPointerGesture(new Point(100, 100), MouseButtons.Left);
                editor.ContinuePointerGestureForTesting(new Point(104, 100), false, true);
                Equal(true, editor.IsScrubbing, "movement beyond three pixels begins scrubbing");
                editor.ApplyScrubDeltaForTesting(16, false);
                Equal(42, requested[requested.Count - 1], "ordinary attributes scrub one step per eight pixels");
                editor.EndPointerGesture();
                Equal(false, editor.IsScrubbing, "mouse-up ends scrubbing");
                Equal(false, editor.CursorIsHidden, "mouse-up restores the hidden cursor");
                Equal(false, editor.Capture, "mouse-up releases capture");

                editor.EnterTypingForTesting();
                Equal(true, editor.CommitTextForTesting("128"), "Program accepts user-facing value 128");
                Equal(127, requested[requested.Count - 1], "Program converts 128 to engine value 127");
                editor.EnterTypingForTesting();
                Equal(false, editor.CommitTextForTesting("129"), "out-of-range Program input is rejected");
                Equal(127, editor.CurrentValue, "invalid input restores the last valid value");
                editor.EnterTypingForTesting();
                editor.Text = "12";
                typeof(Control).GetMethod("OnLostFocus", BindingFlags.Instance | BindingFlags.NonPublic, null,
                    new Type[] { typeof(EventArgs) }, null).Invoke(editor, new object[] { EventArgs.Empty });
                Equal(11, requested[requested.Count - 1], "focus loss commits typed Program using the 1-based UI boundary");

                editor.Configure(0, 0, 1, 0, 1, 1, 1, 1, false, false, delegate(int value) { return value == 0 ? "Off" : "On"; }, "Sustain");
                editor.ApplyScrubDeltaForTesting(50, false);
                Equal(1, requested[requested.Count - 1], "Sustain scrubbing makes one deliberate bounded transition");
                editor.ApplyScrubDeltaForTesting(50, false);
                Equal(1, requested[requested.Count - 1], "redundant Sustain movement does not resend On");

                editor.Configure(0, -8192, 8191, 0, 16, 1, 1, 1, false, false, null, "Pitch bend");
                editor.ApplyScrubDeltaForTesting(3, false);
                Equal(48, requested[requested.Count - 1], "pitch bend coarse scrub uses 16 units per pixel");
                editor.ApplyScrubDeltaForTesting(3, true);
                Equal(51, requested[requested.Count - 1], "Shift/fine pitch bend scrub uses one unit per pixel");
                editor.ApplyScrubDeltaForTesting(100000, false);
                Equal(8191, requested[requested.Count - 1], "scrub clamps at maximum");
                editor.ApplyScrubDeltaForTesting(-100000, false);
                Equal(-8192, requested[requested.Count - 1], "scrub clamps at minimum");
                editor.Configure(0, -8192, 8191, 0, 16, 1, 1, 1, false, false, null, "Pitch bend");
                editor.ApplyScrubDeltaForTesting(10, false);
                editor.ApplyScrubDeltaForTesting(10, false);
                Equal(320, requested[requested.Count - 1], "successive relative scrub segments accumulate without a screen-edge limit");
                editor.Configure(64, 0, 127, 0, 1, 8, 1, 16, true, false, null, "Pan");
                editor.RequestAutoForTesting();
                Equal(ChannelOverrideState.AutoValue, requested[requested.Count - 1], "right-click Auto requests the Auto sentinel");
                editor.EscapeForTesting();
                Equal(false, editor.CursorIsHidden, "Escape never leaves the cursor hidden");
                Equal(false, editor.Capture, "Escape never leaves capture active");
            }

            using (ChannelMonitorForm monitor = new ChannelMonitorForm("build20.mid"))
            {
                monitor.Show(); Application.DoEvents();
                List<int> monitorRequests = new List<int>();
                monitor.OverrideRequested += delegate(object sender, ChannelOverrideRequestEventArgs e) { monitorRequests.Add(e.Value); };
                ChannelStateTracker tracker = new ChannelStateTracker();
                tracker.RecordOverrideApplied(0, ChannelAttribute.Volume, 88);
                ChannelPlaybackSnapshot snapshot = tracker.CreateSnapshot();
                snapshot.Channels[0].ForcedAttributeMask |= 1 << (int)ChannelAttribute.Volume;
                snapshot.Channels[0].ForcedVolume = 88;
                monitor.UpdateSnapshot(snapshot);
                Equal("88", monitor.CellText(0, "Volume"), "forced cell remains concise");
                Equal(Color.FromArgb(20, 75, 155), monitor.CellForeColor(0, "Volume"), "forced cell is blue");
                if ((monitor.CellFontStyle(0, "Volume") & FontStyle.Bold) == 0)
                    throw new Exception("forced cell is not bold");
                Equal(1, monitor.EditorControlCountForTesting, "monitor hosts one reusable scrub/type editor");
                Equal(null, monitor.GridForTesting.ContextMenuStrip, "monitor has no context menu");
                monitor.ActivateEditorForTesting(0, "Program");
                Equal(0, monitorRequests.Count, "editor activation itself is side-effect free");
                int volumeColumn = monitor.GridForTesting.Columns["Volume"].Index;
                MethodInfo cellMouseDown = typeof(DataGridView).GetMethod("OnCellMouseDown", BindingFlags.Instance | BindingFlags.NonPublic,
                    null, new Type[] { typeof(DataGridViewCellMouseEventArgs) }, null);
                cellMouseDown.Invoke(monitor.GridForTesting, new object[] { new DataGridViewCellMouseEventArgs(volumeColumn, 0, 4, 4,
                    new MouseEventArgs(MouseButtons.Right, 1, 4, 4, 0)) });
                Application.DoEvents();
                Equal(ChannelOverrideState.AutoValue, monitorRequests[monitorRequests.Count - 1], "right-clicking a forced cell returns it to Auto through the real grid path");
                Equal(null, monitor.GridForTesting.ContextMenuStrip, "right-click installs no context menu");
                monitor.DetachForSongReplacement("Loading new MIDI…");
                Equal(false, monitor.EditorForTesting.Visible, "detaching hides the reusable editor");
                monitor.Close();
            }
            Equal(null, typeof(ChannelMonitorForm).GetNestedType("ChannelOverrideDialog", BindingFlags.NonPublic | BindingFlags.Public),
                "obsolete popup editor type was removed");
        }

        private static void TestBuild21ScrubHandoff()
        {
            Application.EnableVisualStyles();
            using (ChannelMonitorForm monitor = new ChannelMonitorForm("gesture.mid"))
            {
                List<int> requested = new List<int>();
                monitor.OverrideRequested += delegate(object sender, ChannelOverrideRequestEventArgs e) { requested.Add(e.Value); };
                monitor.UpdateSnapshot(ChannelPlaybackSnapshot.Empty());
                monitor.Show(); Application.DoEvents();
                DataGridView grid = monitor.GridForTesting;
                int column = grid.Columns["Volume"].Index;
                Rectangle cell = grid.GetCellDisplayRectangle(column, 0, true);
                int x = cell.Left + Math.Min(12, Math.Max(4, cell.Width / 4));
                int y = cell.Top + cell.Height / 2;
                Cursor.Position = grid.PointToScreen(new Point(x, y));
                SendMessage(grid.Handle, 0x0201, new IntPtr(1), MouseCoordinates(x, y));
                Application.DoEvents();
                Equal(true, monitor.EditorForTesting.GestureArmedForTesting, "first grid MouseDown arms overlay gesture");
                Equal(true, monitor.EditorForTesting.ReadOnly, "flat editor remains read-only");
                Equal(0, monitor.EditorForTesting.SelectionLength, "flat editor exposes no selection highlight");
                MethodInfo gridMouseMove = typeof(Control).GetMethod("OnMouseMove", BindingFlags.Instance | BindingFlags.NonPublic);
                MethodInfo gridMouseUp = typeof(Control).GetMethod("OnMouseUp", BindingFlags.Instance | BindingFlags.NonPublic);
                gridMouseMove.Invoke(grid, new object[] { new MouseEventArgs(MouseButtons.Left, 0, x + 4, y, 0) });
                Application.DoEvents();
                Equal(true, monitor.EditorForTesting.IsScrubbing, "first grid-owned drag crosses threshold immediately");
                Equal(0, requested.Count, "crossing threshold alone sends no override");
                gridMouseMove.Invoke(grid, new object[] { new MouseEventArgs(MouseButtons.Left, 0, x + 12, y, 0) });
                Application.DoEvents();
                Equal(101, requested[requested.Count - 1], "unknown Volume seed 100 accumulates one step per eight pixels");
                gridMouseUp.Invoke(grid, new object[] { new MouseEventArgs(MouseButtons.Left, 1, x + 12, y, 0) });
                Application.DoEvents();
                Equal(false, monitor.EditorForTesting.IsScrubbing, "grid-owned MouseUp ends first scrub");
                Equal(false, monitor.EditorForTesting.GestureArmedForTesting, "MouseUp clears armed state");
                Equal(false, monitor.EditorForTesting.CursorIsHidden, "MouseUp restores cursor");
                Equal(false, monitor.EditorForTesting.Capture, "MouseUp releases capture");
                int afterRelease = requested.Count;
                gridMouseMove.Invoke(grid, new object[] { new MouseEventArgs(MouseButtons.None, 0, x + 30, y, 0) });
                Application.DoEvents();
                Equal(afterRelease, requested.Count, "hover after release cannot continue scrub");

                int panColumn = grid.Columns["Pan"].Index;
                Rectangle panCell = grid.GetCellDisplayRectangle(panColumn, 0, true);
                int px = panCell.Left + 5, py = panCell.Top + panCell.Height / 2;
                SendMessage(grid.Handle, 0x0201, new IntPtr(1), MouseCoordinates(px, py));
                SendMessage(grid.Handle, 0x0202, IntPtr.Zero, MouseCoordinates(px, py));
                Application.DoEvents();
                Equal(true, monitor.EditorForTesting.IsTyping, "first simple grid click enters typing");
                int requestsBeforeArrows = requested.Count;
                monitor.EditorForTesting.ArrowForTesting(true);
                Equal("65", monitor.EditorForTesting.Text, "typing Up Arrow changes pending neutral Pan seed");
                monitor.EditorForTesting.ArrowForTesting(false);
                Equal("64", monitor.EditorForTesting.Text, "typing Down Arrow changes pending text");
                Equal(requestsBeforeArrows, requested.Count, "typing arrows do not send before commit");
                grid.Focus(); Application.DoEvents();
                Equal(requestsBeforeArrows, requested.Count, "unchanged click/type focus loss sends no override");
                Equal(false, monitor.EditorForTesting.IsTyping, "unchanged focus loss exits typing mode");

                monitor.ActivateEditorForTesting(0, "Pan");
                monitor.EditorForTesting.EnterTypingForTesting();
                monitor.EditorForTesting.Text = "65";
                grid.Focus(); Application.DoEvents();
                Equal(65, requested[requested.Count - 1], "changed focus loss commits the edited value");

                monitor.ActivateEditorForTesting(1, "Pan");
                monitor.EditorForTesting.EnterTypingForTesting();
                int beforeExplicitEnter = requested.Count;
                typeof(Control).GetMethod("OnKeyDown", BindingFlags.Instance | BindingFlags.NonPublic,
                    null, new Type[] { typeof(KeyEventArgs) }, null).Invoke(monitor.EditorForTesting,
                    new object[] { new KeyEventArgs(Keys.Enter) });
                Equal(beforeExplicitEnter + 1, requested.Count, "Enter explicitly commits an unchanged neutral seed");
                Equal(64, requested[requested.Count - 1], "explicit Enter commits the displayed Pan seed");

                monitor.ActivateEditorForTesting(2, "Pan");
                monitor.EditorForTesting.EnterTypingForTesting();
                int beforeArrowCommit = requested.Count;
                monitor.EditorForTesting.ArrowForTesting(true);
                grid.Focus(); Application.DoEvents();
                Equal(beforeArrowCommit + 1, requested.Count, "Up Arrow followed by focus loss commits a deliberate edit");
                Equal(65, requested[requested.Count - 1], "Up Arrow commits the incremented pending value");

                monitor.ActivateEditorForTesting(3, "Pan");
                monitor.EditorForTesting.EnterTypingForTesting();
                monitor.EditorForTesting.Text = "70";
                int beforeEscape = requested.Count;
                monitor.EditorForTesting.EscapeForTesting();
                Equal(beforeEscape, requested.Count, "Escape abandons typed changes without sending");
                Equal(64, monitor.EditorForTesting.CurrentValue, "Escape restores committed editor seed");

                int requestsBeforeSeeds = requested.Count;
                monitor.ActivateEditorForTesting(1, "Bend");
                Equal("—", monitor.CellText(1, "Bend"), "unknown Pitch bend cell remains visually unknown");
                Equal(0, monitor.EditorForTesting.CurrentValue, "unknown Pitch bend editor starts centered");
                Equal(requestsBeforeSeeds, requested.Count, "activating unknown Pitch bend sends nothing");
                string[] seedColumns = { "BankMsb", "BankLsb", "Program", "Volume", "Expression", "Pan", "Sustain", "Bend", "Aftertouch" };
                int[] seeds = { 0, 0, 0, 100, 127, 64, 0, 0, 0 };
                for (int seed = 0; seed < seedColumns.Length; seed++)
                {
                    monitor.ActivateEditorForTesting(2, seedColumns[seed]);
                    Equal(seeds[seed], monitor.EditorForTesting.CurrentValue, seedColumns[seed] + " unknown editor seed");
                    Equal("—", monitor.CellText(2, seedColumns[seed]), seedColumns[seed] + " cell remains unknown until a request");
                }
                Equal(requestsBeforeSeeds, requested.Count, "opening neutral seeds sends no requests");
                ChannelStateTracker tracker = new ChannelStateTracker();
                tracker.RecordSuccessful(ChannelMessage(0, 0xB0, 7, 80));
                tracker.MarkAttributeHistoricalDirect(0, ChannelAttribute.Volume);
                monitor.UpdateSnapshot(tracker.CreateSnapshot());
                int chasedChannel = -1;
                ChannelAttribute chasedAttribute = ChannelAttribute.BankMsb;
                monitor.HistoricalChaseRequested += delegate(object sender, ChannelChaseRequestEventArgs e)
                {
                    chasedChannel = e.Channel;
                    chasedAttribute = e.Attribute;
                };
                MethodInfo cellMouseDown = typeof(DataGridView).GetMethod("OnCellMouseDown", BindingFlags.Instance | BindingFlags.NonPublic,
                    null, new Type[] { typeof(DataGridViewCellMouseEventArgs) }, null);
                cellMouseDown.Invoke(grid, new object[] { new DataGridViewCellMouseEventArgs(column, 0, 4, 4,
                    new MouseEventArgs(MouseButtons.Right, 1, 4, 4, 0)) });
                Equal(0, chasedChannel, "right-clicking historical cell requests source chase for the correct channel");
                Equal(ChannelAttribute.Volume, chasedAttribute, "right-clicking historical cell requests source chase for the correct attribute");
                bool? enabledRequest = null;
                monitor.ChannelEnabledRequested += delegate(object sender, ChannelEnabledRequestEventArgs e) { enabledRequest = e.Enabled; };
                int channelColumn = grid.Columns["Channel"].Index;
                cellMouseDown.Invoke(grid, new object[] { new DataGridViewCellMouseEventArgs(channelColumn, 0, 4, 4,
                    new MouseEventArgs(MouseButtons.Left, 1, 4, 4, 0)) });
                Equal(false, enabledRequest.Value, "clicking Channel cell requests disable");
                monitor.Close();
            }

            using (ScrubOrTypeTextBox editor = new ScrubOrTypeTextBox())
            {
                List<int> requested = new List<int>();
                editor.ValueRequested += delegate(object sender, ScrubValueEventArgs e) { requested.Add(e.Value); };
                editor.Configure(64, 0, 127, 0, 1, 8, 1, 16, false, false, null, "Pan");
                editor.ApplyScrubDeltaForTesting(7, false);
                Equal(0, requested.Count, "sub-step movement accumulates without sending");
                editor.ApplyScrubDeltaForTesting(1, false);
                Equal(65, requested[0], "eight accumulated pixels make one ordinary step");
                editor.ApplyScrubDeltaForTesting(-4, false);
                editor.ApplyScrubDeltaForTesting(-4, false);
                Equal(64, requested[requested.Count - 1], "reverse movement naturally unwinds remainder");
                editor.ApplyScrubDeltaForTesting(15, true);
                Equal(64, requested[requested.Count - 1], "Shift uses one step per sixteen pixels");
                editor.ApplyScrubDeltaForTesting(1, true);
                Equal(65, requested[requested.Count - 1], "Shift remainder completes at sixteen pixels");
                editor.Configure(0, -8192, 8191, 0, 16, 1, 1, 1, false, false, null, "Bend");
                editor.ApplyScrubDeltaForTesting(2, false);
                Equal(32, requested[requested.Count - 1], "Pitch bend uses 16 units per pixel");
                editor.ApplyScrubDeltaForTesting(2, true);
                Equal(34, requested[requested.Count - 1], "Shift Pitch bend uses one unit per pixel");
            }
        }

        private static void TestBuild21ChannelControls()
        {
            MidiSong empty = NewChannelSong("controls-empty.mid", 0);
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.Start(empty, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000, "control empty completion");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                int afterForce = output.SentPayloads().Count;
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                Equal(afterForce, output.SentPayloads().Count, "releasing force emits no MIDI message");
                MidiChannelSnapshot released = engine.GetChannelSnapshot().Channels[0];
                Equal(0, released.ForcedAttributeMask & (1 << (int)ChannelAttribute.Volume), "released attribute is Auto");
                Equal(true, (released.HistoricalAttributeMask & (1 << (int)ChannelAttribute.Volume)) != 0,
                    "released forced value becomes historical");
                Equal(80, released.Volume, "historical release retains last successful value");
                engine.SendExplicitChannelAttribute(0, ChannelAttribute.Volume, 80);
                Equal(afterForce + 1, output.SentPayloads().Count, "historical chase emits exactly one message");
                MidiChannelSnapshot chased = engine.GetChannelSnapshot().Channels[0];
                Equal(0, chased.ForcedAttributeMask & (1 << (int)ChannelAttribute.Volume), "one-value chase creates no override");
                Equal(0, chased.HistoricalAttributeMask & (1 << (int)ChannelAttribute.Volume), "successful chase clears historical state");

                foreach (ChannelAttribute attribute in (ChannelAttribute[])Enum.GetValues(typeof(ChannelAttribute)))
                {
                    int value = attribute == ChannelAttribute.PitchBend ? -321 : attribute == ChannelAttribute.Sustain ? 1 : 33;
                    engine.SetChannelOverride(2, attribute, value);
                    engine.SetChannelOverride(2, attribute, ChannelOverrideState.AutoValue);
                    int before = output.SentPayloads().Count;
                    engine.SendExplicitChannelAttribute(2, attribute, value);
                    Equal(before + 1, output.SentPayloads().Count, attribute + " chase sends one encoded message");
                    ChannelAttribute classified; int classifiedValue;
                    MidiEvent sent = output.SentEvents()[output.SentEvents().Count - 1];
                    Equal(true, ChannelOverrideState.TryClassify(sent, out classified, out classifiedValue), attribute + " chase classifies");
                    Equal(attribute, classified, attribute + " chase attribute");
                    Equal(value, classifiedValue, attribute + " chase value");
                }
                engine.Unload();
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                // Keep the conflicting source value well beyond the control boundary.
                // A short 300 ms gap made this assertion depend on full-suite thread
                // scheduling: correctly pre-admitted work could be filtered before
                // the test released the force and, by design, is never replayed.
                MidiSong laterSource = NewChannelSong("released-force.mid", 1100000,
                    ChannelMessage(0, 0x90, 60, 1), ChannelMessage(1000000, 0xB0, 7, 20));
                engine.Start(laterSource, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().ProcessedEvents >= 1; }, 1000, "released-force initial dispatch");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                WaitFor(delegate { return ContainsMessage(output.SentPayloads(), 0xB0, 7, 80); }, 1000, "live force application");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2500, "released-force source completion");
                Equal(true, ContainsMessage(output.SentPayloads(), 0xB0, 7, 20), "source conflict passes after force release");
                MidiChannelSnapshot state = engine.GetChannelSnapshot().Channels[0];
                Equal(0L, state.OverrideSuppressedEvents, "released force no longer increments override filtering");
                Equal(0, state.HistoricalAttributeMask & (1 << (int)ChannelAttribute.Volume),
                    "successful later source value clears historical state");
            }

            MidiEvent system = new MidiEvent { Channel = -1, Status = 0xF8, Kind = MidiEventKind.SystemMessage, Data = new byte[] { 0xF8 }, IntendedMicroseconds = 0 };
            MidiSong filtered = NewChannelSong("muted.mid", 0,
                ChannelMessage(0, 0x90, 60, 100), ChannelMessage(0, 0xB0, 7, 50),
                ChannelMessage(0, 0x91, 61, 100), system);
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SetChannelEnabled(0, false);
                Equal(false, engine.IsChannelEnabled(0), "channel can be disabled before output opens");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 90);
                engine.Start(filtered, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000, "muted playback completion");
                List<byte[]> sent = output.SentPayloads();
                Equal(false, ContainsMessage(sent, 0x90, 60, 100), "disabled channel source note is filtered");
                Equal(false, ContainsMessage(sent, 0xB0, 7, 50), "disabled channel source controller is filtered");
                Equal(true, ContainsMessage(sent, 0x91, 61, 100), "other channel remains enabled");
                Equal(true, ContainsMessage(sent, 0xF8), "system message is unaffected by channel filter");
                Equal(2L, engine.GetSnapshot().ProcessedEvents, "muted events do not enter scheduler processing accounting");
                Equal(0L, engine.GetSnapshot().DroppedEvents, "muted events are not queue drops");
                MidiChannelSnapshot state = engine.GetChannelSnapshot().Channels[0];
                Equal(false, state.Enabled, "snapshot exposes disabled channel");
                Equal(2L, state.MutedFilteredEvents, "muted-filtered count is separate");
                Equal(0L, state.OverrideSuppressedEvents, "disabled filtering takes precedence over override filtering");
                int beforeEnable = sent.Count;
                engine.SetChannelEnabled(0, true);
                Equal(true, engine.IsChannelEnabled(0), "channel re-enables independently");
                Equal(true, ContainsMessage(output.SentPayloads(), 0xB0, 7, 90), "re-enable reapplies configured forced value");
                if (output.SentPayloads().Count != beforeEnable + 1)
                    throw new Exception("re-enable replayed unrelated source state");
                engine.Unload();
                Equal(true, engine.IsChannelEnabled(0), "unload restores every channel enabled");
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                MidiSong held = NewChannelSong("held-muted.mid", 1000000,
                    ChannelMessage(0, 0x90, 64, 100), ChannelMessage(1000000, 0x80, 64, 0));
                engine.Start(held, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().ProcessedEvents >= 1; }, 1000, "held note dispatch");
                engine.SetChannelEnabled(0, false);
                WaitFor(delegate { return !engine.IsChannelEnabled(0); }, 1000, "ordered disable boundary");
                List<byte[]> sent = output.SentPayloads();
                int sustain = IndexOfMessage(sent, 0xB0, 64, 0);
                int soundOff = IndexOfMessage(sent, 0xB0, 120, 0);
                int notesOff = IndexOfMessage(sent, 0xB0, 123, 0);
                if (!(sustain >= 0 && soundOff > sustain && notesOff > soundOff))
                    throw new Exception("channel disable safety messages were not ordered sustain-off, all-sound-off, all-notes-off");
                Equal(0, engine.GetChannelSnapshot().Channels[0].KeysDown, "disable safety clears tracked held keys");
                engine.Stop();
                Equal(false, engine.IsChannelEnabled(0), "disabled channel persists across Stop");
                engine.Unload();
            }

            using (PlaybackEngine noneEngine = new PlaybackEngine())
            using (NullMidiOutput none = new NullMidiOutput())
            {
                none.Open();
                noneEngine.SetChannelMonitoring(true);
                noneEngine.Start(empty, none, ProcessingMode.Queue);
                WaitFor(delegate { return noneEngine.State == PlaybackState.Completed; }, 1000, "None control completion");
                noneEngine.SetChannelOverride(3, ChannelAttribute.Pan, 64);
                noneEngine.SetChannelOverride(3, ChannelAttribute.Pan, ChannelOverrideState.AutoValue);
                noneEngine.SendExplicitChannelAttribute(3, ChannelAttribute.Pan, 64);
                Equal(0, noneEngine.GetChannelSnapshot().Channels[3].HistoricalAttributeMask & (1 << (int)ChannelAttribute.Pan),
                    "None accepts logical one-value chase");
            }

            using (PlaybackEngine failing = new PlaybackEngine())
            {
                ToggleFailureOutput output = new ToggleFailureOutput();
                failing.SetChannelMonitoring(true);
                failing.Start(empty, output, ProcessingMode.Queue);
                WaitFor(delegate { return failing.State == PlaybackState.Completed; }, 1000, "toggle output completion");
                failing.SetChannelOverride(0, ChannelAttribute.Pan, 70);
                failing.SetChannelOverride(0, ChannelAttribute.Pan, ChannelOverrideState.AutoValue);
                output.Throw = true;
                bool threw = false;
                try { failing.SendExplicitChannelAttribute(0, ChannelAttribute.Pan, 70); }
                catch (InvalidOperationException) { threw = true; }
                Equal(true, threw, "failed direct chase is reported");
                Equal(true, (failing.GetChannelSnapshot().Channels[0].HistoricalAttributeMask & (1 << (int)ChannelAttribute.Pan)) != 0,
                    "failed chase preserves historical state");
                try { failing.SetChannelEnabled(0, false); }
                catch (InvalidOperationException) { }
                Equal(true, failing.IsChannelEnabled(0), "failed disable safety leaves channel enabled");
            }
        }

        private static void TestBuild22HistoricalChaseAcknowledgement()
        {
            MidiSong activeSong = NewChannelSong("active-chase.mid", 3000000,
                ChannelMessage(0, 0xB0, 7, 80), ChannelMessage(1000, 0x90, 60, 100),
                ChannelMessage(2500000, 0x90, 61, 100));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                AcknowledgingMidiOutput output = new AcknowledgingMidiOutput(2);
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = false;
                engine.Start(activeSong, output, ProcessingMode.Queue);
                if (!output.Entered.WaitOne(1500)) throw new Exception("active chase did not reach the blocked source send");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                Equal(true, (engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Volume)) != 0, "active chase begins from a historical value");
                ManualResetEvent completed = new ManualResetEvent(false);
                Exception completionError = null;
                engine.SendExplicitChannelAttribute(0, ChannelAttribute.Volume, 80, delegate(Exception error)
                {
                    completionError = error;
                    completed.Set();
                });
                Equal(false, completed.WaitOne(60), "queued chase is not acknowledged while an earlier output call is blocked");
                Equal(true, (engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Volume)) != 0, "historical state remains until output confirms the chase");
                output.Release.Set();
                if (!completed.WaitOne(1500)) throw new Exception("active chase acknowledgement did not complete");
                Equal(null, completionError, "active chase completion reports success");
                Equal(2, output.CountPayload(0xB0, 7, 80), "active chase sends exactly one additional CC7 value");
                Equal(0, engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Volume), "confirmed chase clears historical state");
                engine.Stop();
                completed.Dispose();
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                AcknowledgingMidiOutput output = new AcknowledgingMidiOutput(0);
                engine.SetChannelMonitoring(true);
                engine.Start(NewChannelSong("paused-chase.mid", 2000000,
                    ChannelMessage(1500000, 0x90, 64, 100)), output, ProcessingMode.Queue, 0, true);
                ManualResetEvent completed = new ManualResetEvent(false);
                Exception completionError = null;
                engine.SendExplicitChannelAttribute(2, ChannelAttribute.Program, 24, delegate(Exception error)
                {
                    completionError = error; completed.Set();
                });
                if (!completed.WaitOne(1500)) throw new Exception("paused-worker chase did not complete");
                Equal(null, completionError, "paused-worker chase succeeds without resuming playback");
                Equal(1, output.CountPayload(0xC2, 24), "paused-worker chase sends the exact Program payload");
                Equal(PlaybackState.Paused, engine.State, "manual chase does not resume a paused worker");
                engine.Stop();
                completed.Dispose();
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                AcknowledgingMidiOutput output = new AcknowledgingMidiOutput(2);
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = false;
                engine.Start(activeSong, output, ProcessingMode.Queue);
                if (!output.Entered.WaitOne(1500)) throw new Exception("failed chase did not reach blocked source send");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                output.FailMatchingControl = true;
                ManualResetEvent completed = new ManualResetEvent(false);
                Exception completionError = null;
                engine.SendExplicitChannelAttribute(0, ChannelAttribute.Volume, 80, delegate(Exception error)
                {
                    completionError = error; completed.Set();
                });
                output.Release.Set();
                if (!completed.WaitOne(1500)) throw new Exception("failed active chase acknowledgement did not complete");
                if (completionError == null) throw new Exception("failed active chase was reported as successful");
                Equal(true, (engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Volume)) != 0, "failed chase preserves historical state");
                engine.Stop();
                completed.Dispose();
            }
        }

        private static void TestBuild23HistoricalChaseUiRoute()
        {
            Application.EnableVisualStyles();
            MidiSong song = NewChannelSong("ui-chase.mid", 4000000,
                ChannelMessage(0, 0xB0, 7, 80),
                ChannelMessage(1000, 0x90, 60, 100),
                ChannelMessage(3500000, 0x90, 61, 100));
            AcknowledgingMidiOutput output = new AcknowledgingMidiOutput(2);
            using (MainForm main = new MainForm())
            {
                BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                typeof(MainForm).GetField("_song", flags).SetValue(main, song);
                PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", flags).GetValue(main);
                main.Show();
                main.ShowChannelMonitorForTesting();
                ChannelMonitorForm monitor = main.ChannelMonitorForTesting;
                PumpFor(60);
                engine.SimulateSlowdown = false;
                engine.Start(song, output, ProcessingMode.Queue);
                if (!output.Entered.WaitOne(1500)) throw new Exception("UI chase fixture did not reach the blocked source send");

                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                PumpUntil(delegate
                {
                    ChannelPlaybackSnapshot snapshot = engine.GetChannelSnapshot();
                    return snapshot != null && (snapshot.Channels[0].HistoricalAttributeMask &
                        (1 << (int)ChannelAttribute.Volume)) != 0;
                }, 1000, "historical Volume appearance");
                PumpUntil(delegate { return (monitor.CellFontStyle(0, "Volume") & FontStyle.Italic) != 0; },
                    1000, "historical Volume cell paint");

                DataGridView grid = monitor.GridForTesting;
                Rectangle cell = grid.GetCellDisplayRectangle(grid.Columns["Volume"].Index, 0, true);
                int x = cell.Left + Math.Max(2, cell.Width / 2);
                int y = cell.Top + Math.Max(2, cell.Height / 2);
                SendMessage(grid.Handle, 0x0204, new IntPtr(2), MouseCoordinates(x, y));
                SendMessage(grid.Handle, 0x0205, IntPtr.Zero, MouseCoordinates(x, y));
                PumpFor(80);
                Equal(1, output.CountPayload(0xB0, 7, 80), "right-click remains pending behind the blocked output call");
                Equal(true, (engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Volume)) != 0, "UI chase remains historical until ordered output succeeds");

                output.Release.Set();
                PumpUntil(delegate { return output.CountPayload(0xB0, 7, 80) >= 2; }, 1500,
                    "right-click chase output delivery");
                PumpFor(40);
                int acceptedVolumeMessages = output.CountPayload(0xB0, 7, 80);
                int historicalMask = engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask;
                Console.WriteLine("      UI chase diagnostic: CC7=80 count {0}, historical mask 0x{1:X}",
                    acceptedVolumeMessages, historicalMask);
                Equal(2, acceptedVolumeMessages, "right-click sends exactly one additional CC7 value");
                Equal(0, historicalMask & (1 << (int)ChannelAttribute.Volume),
                    "right-click chase acknowledgement clears historical state");
                Equal(ChannelOverrideState.AutoValue, engine.GetChannelOverride(0, ChannelAttribute.Volume),
                    "one-value chase creates no force");
                engine.Stop();
                main.Close();
            }

            using (MainForm main = new MainForm())
            {
                MidiSong failureSong = NewChannelSong("ui-chase-failure.mid", 3000000,
                    ChannelMessage(0, 0xB0, 7, 75), ChannelMessage(2500000, 0x90, 64, 100));
                ToggleFailureOutput failingOutput = new ToggleFailureOutput();
                BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                typeof(MainForm).GetField("_song", flags).SetValue(main, failureSong);
                PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", flags).GetValue(main);
                main.Show(); main.ShowChannelMonitorForTesting(); PumpFor(40);
                engine.Start(failureSong, failingOutput, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().ProcessedEvents >= 1; }, 1000, "UI failed-chase initial state");
                engine.Pause(); PumpFor(50);
                PumpUntil(delegate { return (main.ChannelMonitorForTesting.CellFontStyle(0, "Volume") & FontStyle.Italic) != 0; },
                    1000, "failed-chase historical cell paint");
                failingOutput.Throw = true;
                RightClickCell(main.ChannelMonitorForTesting.GridForTesting, 0, "Volume");
                PumpFor(200);
                Equal(true, (engine.GetChannelSnapshot().Channels[0].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Volume)) != 0,
                    "full UI failed chase retains historical state");
                Equal(ChannelOverrideState.AutoValue, engine.GetChannelOverride(0, ChannelAttribute.Volume),
                    "failed UI chase creates no override");
                failingOutput.Throw = false;
                engine.Stop(); main.Close();
            }

            // The production adapters receive the same packed message at their
            // deterministic native boundaries. No installed provider is opened.
            List<uint> winmmMessages = new List<uint>();
            using (WindowsMidiOutput winmm = new WindowsMidiOutput(delegate(IntPtr handle, uint message)
            {
                winmmMessages.Add(message); return 0;
            }))
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SetChannelMonitoring(true);
                engine.Start(BuildSong(new long[0]), winmm, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000, "WinMM-shaped chase fixture");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                engine.SendExplicitChannelAttribute(0, ChannelAttribute.Volume, 80);
                Equal(2, CountPackedMessage(winmmMessages, 0x005007B0u), "WinMM-shaped boundary receives forced value and one chase");
            }

            FakeKdmApiNative native = new FakeKdmApiNative();
            using (KdmApiMidiOutput kdmapi = new KdmApiMidiOutput(native))
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                kdmapi.Open();
                engine.SetChannelMonitoring(true);
                engine.Start(BuildSong(new long[0]), kdmapi, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000, "KDMAPI-shaped chase fixture");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                engine.SendExplicitChannelAttribute(0, ChannelAttribute.Volume, 80);
                Equal(2, CountPackedMessage(native.ShortMessages, 0x005007B0u), "KDMAPI-shaped boundary receives forced value and one chase");
            }

            // Closing the modeless shell cannot strand the ordered request.
            AcknowledgingMidiOutput closingOutput = new AcknowledgingMidiOutput(2);
            using (MainForm main = new MainForm())
            {
                BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                typeof(MainForm).GetField("_song", flags).SetValue(main, song);
                PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", flags).GetValue(main);
                main.Show(); main.ShowChannelMonitorForTesting(); PumpFor(40);
                engine.Start(song, closingOutput, ProcessingMode.Queue);
                if (!closingOutput.Entered.WaitOne(1500)) throw new Exception("closing-monitor chase fixture did not block");
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                PumpUntil(delegate { return (main.ChannelMonitorForTesting.CellFontStyle(0, "Volume") & FontStyle.Italic) != 0; },
                    1000, "closing-monitor historical cell paint");
                RightClickCell(main.ChannelMonitorForTesting.GridForTesting, 0, "Volume");
                main.ChannelMonitorForTesting.Close(); PumpFor(20);
                closingOutput.Release.Set();
                WaitFor(delegate { return closingOutput.CountPayload(0xB0, 7, 80) == 2; }, 1500,
                    "closed-monitor ordered chase completion");
                engine.Stop(); main.Close();
            }
        }

        private static void TestBuild21SourceValueChase()
        {
            Application.EnableVisualStyles();
            MidiSong song = NewChannelSong("source-value-chase.mid", 2000000,
                ChannelMessage(0, 0xC1, 2),
                ChannelMessage(0, 0x91, 60, 100),
                ChannelMessage(1500000, 0xC1, 8));

            int indexedValue;
            Equal(true, song.GetChannelSourceValueIndex().TryGetLatest(1, ChannelAttribute.Program, 1000000, out indexedValue),
                "source index finds Program before transport");
            Equal(2, indexedValue, "source index returns wire Program 2 rather than a later or forced value");

            AcknowledgingMidiOutput output = new AcknowledgingMidiOutput(4);
            using (MainForm main = new MainForm())
            {
                BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                typeof(MainForm).GetField("_song", flags).SetValue(main, song);
                PlaybackEngine engine = (PlaybackEngine)typeof(MainForm).GetField("_engine", flags).GetValue(main);
                main.Show();
                main.ShowChannelMonitorForTesting();
                ChannelMonitorForm monitor = main.ChannelMonitorForTesting;
                PumpFor(50);
                engine.SimulateSlowdown = false;
                engine.Start(song, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().ProcessedEvents >= 2; }, 1000,
                    "source-value fixture initial Program and note");

                engine.SetChannelOverride(1, ChannelAttribute.Program, 5);
                PumpUntil(delegate { return output.CountPayload(0xC1, 5) == 1; }, 1000,
                    "forced displayed Program 6 reaches output");
                engine.SetChannelOverride(1, ChannelAttribute.Program, ChannelOverrideState.AutoValue);
                PumpUntil(delegate
                {
                    MidiChannelSnapshot state = engine.GetChannelSnapshot().Channels[1];
                    return state.Program == 5 && (state.HistoricalAttributeMask &
                        (1 << (int)ChannelAttribute.Program)) != 0;
                }, 1000, "former forced Program remains gray historical");
                monitor.UpdateSnapshot(engine.GetChannelSnapshot());
                Equal("6 — Electric Piano 2", monitor.CellText(1, "Program"),
                    "historical display retains former forced Program 6");

                RightClickCell(monitor.GridForTesting, 1, "Program");
                if (!output.Entered.WaitOne(1000)) throw new Exception("source-value chase did not reach ordered output boundary");
                PumpFor(50);
                Equal(1, output.CountPayload(0xC1, 2), "source Program remains the only C1 02 while chase send is blocked");
                Equal(true, (engine.GetChannelSnapshot().Channels[1].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Program)) != 0,
                    "cell remains historical until source-value send is acknowledged");

                output.Release.Set();
                PumpUntil(delegate { return output.CountPayload(0xC1, 2) == 2; }, 1500,
                    "exact C1 02 source-value chase payload");
                PumpUntil(delegate
                {
                    MidiChannelSnapshot state = engine.GetChannelSnapshot().Channels[1];
                    return state.Program == 2 && (state.HistoricalAttributeMask &
                        (1 << (int)ChannelAttribute.Program)) == 0;
                }, 1000, "acknowledged source Program becomes current");
                Equal(1, output.CountPayload(0xC1, 5), "chase never resends historical C1 05");
                Equal(ChannelOverrideState.AutoValue, engine.GetChannelOverride(1, ChannelAttribute.Program),
                    "source-value chase creates no force");

                PumpUntil(delegate { return output.CountPayload(0xC1, 8) == 1; }, 2500,
                    "later source Program supersedes restored value");
                PumpUntil(delegate { return engine.GetChannelSnapshot().Channels[1].Program == 8; }, 1000,
                    "monitor accepts later source Program");
                engine.Stop(); main.Close();
            }

            MidiSong pausedSong = NewChannelSong("paused-source-chase.mid", 2000000,
                ChannelMessage(0, 0xC2, 24), ChannelMessage(1500000, 0x92, 64, 100));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput pausedOutput = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.Start(pausedSong, pausedOutput, ProcessingMode.Queue, 1000000, true);
                engine.ChaseLatestSourceChannelAttribute(2, ChannelAttribute.Program);
                WaitFor(delegate { return ContainsMessage(pausedOutput.SentPayloads(), 0xC2, 24); }, 1000,
                    "paused source-value chase acknowledgement");
                Equal(24, engine.GetChannelSnapshot().Channels[2].Program,
                    "paused chase records the acknowledged latest source Program");
                Equal(PlaybackState.Paused, engine.State, "source-value chase does not resume paused playback");
                engine.SetChannelEnabled(2, false);
                ManualResetEvent rejectedDone = new ManualResetEvent(false);
                Exception rejectedError = null;
                engine.ChaseLatestSourceChannelAttribute(2, ChannelAttribute.Program, delegate(Exception error)
                {
                    rejectedError = error; rejectedDone.Set();
                });
                if (!rejectedDone.WaitOne(1000)) throw new Exception("disabled-channel source chase did not complete");
                Equal(true, rejectedError is InvalidOperationException,
                    "source-value chase is rejected at the ordered disabled-channel boundary");
                rejectedDone.Dispose();
                engine.Stop();
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            using (NullMidiOutput none = new NullMidiOutput())
            {
                none.Open();
                engine.SetChannelMonitoring(true);
                engine.Start(pausedSong, none, ProcessingMode.Queue, 1000000, true);
                engine.ChaseLatestSourceChannelAttribute(2, ChannelAttribute.Program);
                // The paused worker still owns ordered control execution. Allow for
                // a busy x86 full-suite host without weakening the state assertion.
                WaitFor(delegate { return engine.GetChannelSnapshot().Channels[2].Program == 24; }, 5000,
                    "None source-value chase acknowledgement");
                Equal(24, engine.GetChannelSnapshot().Channels[2].Program,
                    "None acknowledges the logical source-value chase");
                engine.Stop();
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                ToggleFailureOutput failure = new ToggleFailureOutput();
                engine.SetChannelMonitoring(true);
                MidiSong failureSong = NewChannelSong("failed-source-chase.mid", 0,
                    ChannelMessage(0, 0xC2, 24));
                engine.Start(failureSong, failure, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000,
                    "failure fixture source completion");
                engine.SetChannelOverride(2, ChannelAttribute.Program, 5);
                engine.SetChannelOverride(2, ChannelAttribute.Program, ChannelOverrideState.AutoValue);
                failure.Throw = true;
                bool failed = false;
                try { engine.ChaseLatestSourceChannelAttribute(2, ChannelAttribute.Program); }
                catch (InvalidOperationException) { failed = true; }
                Equal(true, failed, "failed source-value chase is reported");
                Equal(true, (engine.GetChannelSnapshot().Channels[2].HistoricalAttributeMask &
                    (1 << (int)ChannelAttribute.Program)) != 0,
                    "failed source-value chase remains historical and retryable");
                engine.Stop();
            }
        }

        private static void TestBuild23ProductMetadata()
        {
            Assembly assembly = Assembly.GetExecutingAssembly();
            AssemblyFileVersionAttribute file = (AssemblyFileVersionAttribute)Attribute.GetCustomAttribute(
                assembly, typeof(AssemblyFileVersionAttribute));
            AssemblyInformationalVersionAttribute information = (AssemblyInformationalVersionAttribute)Attribute.GetCustomAttribute(
                assembly, typeof(AssemblyInformationalVersionAttribute));
            Equal(file.Version, ProductIdentity.Version, "runtime product version comes from assembly metadata");
            Equal(information.InformationalVersion, ProductIdentity.InformationalVersion,
                "runtime informational identity comes from assembly metadata");
            using (AboutProductDialog about = new AboutProductDialog())
            {
                about.Show(); Application.DoEvents();
                string expected = information.InformationalVersion + " • version " + file.Version;
                bool found = false;
                foreach (Control control in about.Controls)
                    if (String.Equals(control.Text, expected, StringComparison.Ordinal)) found = true;
                Equal(true, found, "realized About text matches executing assembly metadata");
                about.Close();
            }
        }

        private static void TestBuild23PreAdmissionFiltering()
        {
            List<MidiEvent> immediateEvents = new List<MidiEvent>();
            for (int i = 0; i < 1000; i++) immediateEvents.Add(ChannelMessage(0, 0x90, (byte)(i & 0x7F), 100));
            for (int i = 0; i < 500; i++) immediateEvents.Add(ChannelMessage(0, 0xB1, 7, 20));
            for (int i = 0; i < 500; i++) immediateEvents.Add(ChannelMessage(0, 0xB1, 7, 80));
            immediateEvents.Add(new MidiEvent { Channel = -1, Status = 0xF8, Kind = MidiEventKind.SystemMessage,
                Data = new byte[] { 0xF8 }, IntendedMicroseconds = 0 });
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = false;
                engine.SetChannelEnabled(0, false);
                engine.SetChannelOverride(1, ChannelAttribute.Volume, 80);
                engine.Start(NewChannelSong("pre-admission-immediate.mid", 0, immediateEvents.ToArray()), output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "pre-admission immediate completion");
                PlaybackSnapshot result = engine.GetSnapshot();
                Equal(501L, result.ProcessedEvents, "only eligible source events enter immediate dispatch statistics");
                Equal(0L, result.DroppedEvents, "source filtering is not queue overflow");
                Equal(501L, result.MaximumQueueLength, "unlimited queue pressure excludes pre-admission filtered events");
                MidiChannelSnapshot muted = engine.GetChannelSnapshot().Channels[0];
                MidiChannelSnapshot overridden = engine.GetChannelSnapshot().Channels[1];
                Equal(1000L, muted.MutedFilteredEvents, "muted source count is retained separately");
                Equal(500L, overridden.OverrideSuppressedEvents, "override-filtered source count is retained separately");
                Equal(true, ContainsMessage(output.SentPayloads(), 0xF8), "channel-free system event remains eligible");
            }

            List<MidiEvent> finiteEvents = new List<MidiEvent>();
            for (int i = 0; i < 100; i++) finiteEvents.Add(ChannelMessage(0, 0x90, (byte)(i & 0x7F), 100));
            finiteEvents.Add(ChannelMessage(0, 0x91, 60, 100));
            finiteEvents.Add(ChannelMessage(0, 0x91, 61, 100));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 1000;
                engine.QueueLengthLimit = 2;
                engine.OverflowPolicy = OverflowPolicy.DropNewest;
                engine.SetChannelEnabled(0, false);
                engine.Start(NewChannelSong("pre-admission-finite.mid", 0, finiteEvents.ToArray()), output, ProcessingMode.Drop);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2000, "pre-admission finite completion");
                WaitFor(delegate { return engine.GetChannelSnapshot().Channels[0].MutedFilteredEvents == 100; }, 1000,
                    "pre-admission finite channel snapshot publication");
                PlaybackSnapshot result = engine.GetSnapshot();
                Equal(2L, result.ProcessedEvents, "finite queue services only two eligible events");
                Equal(0L, result.DroppedEvents, "filtered arrivals cannot cause finite-queue overflow");
                Equal(2L, result.MaximumQueueLength, "finite queue occupancy excludes muted arrivals");
                Equal(100L, engine.GetChannelSnapshot().Channels[0].MutedFilteredEvents, "finite muted count");
            }

            List<MidiEvent> backlogEvents = new List<MidiEvent>();
            for (int i = 0; i < 100; i++)
                backlogEvents.Add(ChannelMessage(0, (byte)((i & 1) == 0 ? 0x90 : 0x91), (byte)(i & 0x7F), 100));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 5000;
                engine.Start(NewChannelSong("live-muted-backlog.mid", 0, backlogEvents.ToArray()), output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().OutstandingEvents >= 80; }, 1000, "live mute backlog admission");
                engine.SetChannelEnabled(0, false);
                WaitFor(delegate { return !engine.IsChannelEnabled(0); }, 1000, "live mute ordered boundary");
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2500, "live mute backlog completion");
                PlaybackSnapshot result = engine.GetSnapshot();
                MidiChannelSnapshot channel = engine.GetChannelSnapshot().Channels[0];
                Equal(100L, result.ProcessedEvents + channel.MutedFilteredEvents,
                    "live boundary accounts every source event exactly once as sent or muted");
                Equal(0L, result.DroppedEvents, "live retirement is not a queue drop");
                Equal(0L, result.OutstandingEvents, "live-retired backlog leaves no stale queue occupancy");
            }

            List<MidiEvent> overrideBacklog = new List<MidiEvent>();
            for (int i = 0; i < 60; i++) overrideBacklog.Add(ChannelMessage(0, 0xB2, 7, 20));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = true;
                engine.ProcessingMicroseconds = 5000;
                engine.Start(NewChannelSong("live-override-backlog.mid", 0, overrideBacklog.ToArray()), output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().OutstandingEvents >= 50; }, 1000, "live override backlog admission");
                engine.SetChannelOverride(2, ChannelAttribute.Volume, 80);
                WaitFor(delegate { return ContainsMessage(output.SentPayloads(), 0xB2, 7, 80); }, 1000, "ordered override injection");
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 2500, "live override backlog completion");
                PlaybackSnapshot result = engine.GetSnapshot();
                MidiChannelSnapshot channel = engine.GetChannelSnapshot().Channels[2];
                Equal(60L, result.ProcessedEvents + channel.OverrideSuppressedEvents,
                    "live override boundary accounts every source event exactly once");
                Equal(0L, result.DroppedEvents, "live override retirement is not queue overflow");
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            {
                engine.SetChannelMonitoring(true);
                engine.SetChannelEnabled(0, false);
                engine.Start(NewChannelSong("filtered-failure.mid", 0, ChannelMessage(0, 0x90, 60, 100)),
                    new ThrowingMidiOutput(), ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000,
                    "fully filtered source never enters failing output");
                WaitFor(delegate { return engine.GetChannelSnapshot().Channels[0].MutedFilteredEvents == 1; }, 1000,
                    "fully filtered channel snapshot publication");
                Equal(0L, engine.GetSnapshot().ProcessedEvents, "fully filtered source is not reported sent");
                Equal(1L, engine.GetChannelSnapshot().Channels[0].MutedFilteredEvents, "fully filtered failure count");
            }

            using (PlaybackEngine engine = new PlaybackEngine())
            using (NullMidiOutput none = new NullMidiOutput())
            {
                none.Open(); engine.SetChannelMonitoring(true); engine.SetChannelEnabled(0, false);
                engine.Start(NewChannelSong("filtered-none.mid", 0, ChannelMessage(0, 0x90, 60, 100),
                    ChannelMessage(0, 0x91, 61, 100)), none, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1000, "None filtered completion");
                Equal(1L, engine.GetSnapshot().ProcessedEvents, "None follows identical pre-admission filter semantics");
            }
        }

        private static void RightClickCell(DataGridView grid, int rowIndex, string columnName)
        {
            Rectangle cell = grid.GetCellDisplayRectangle(grid.Columns[columnName].Index, rowIndex, true);
            int x = cell.Left + Math.Max(2, cell.Width / 2);
            int y = cell.Top + Math.Max(2, cell.Height / 2);
            SendMessage(grid.Handle, 0x0204, new IntPtr(2), MouseCoordinates(x, y));
            SendMessage(grid.Handle, 0x0205, IntPtr.Zero, MouseCoordinates(x, y));
        }

        private static int CountPackedMessage(IList<uint> messages, uint expected)
        {
            int count = 0;
            for (int i = 0; i < messages.Count; i++) if (messages[i] == expected) count++;
            return count;
        }

        private static void TestBuild21ChannelMonitorFit()
        {
            Application.EnableVisualStyles();
            using (ChannelMonitorForm monitor = new ChannelMonitorForm("fit.mid"))
            {
                ChannelStateTracker tracker = new ChannelStateTracker();
                tracker.RecordSuccessful(ChannelMessage(0, 0xC0, 24));
                monitor.UpdateSnapshot(tracker.CreateSnapshot());
                monitor.Show(); Application.DoEvents(); Application.DoEvents();
                DataGridView grid = monitor.GridForTesting;
                Console.WriteLine("      Channel Monitor fit: outer {0}x{1}; client {2}x{3}",
                    monitor.Width, monitor.Height, monitor.ClientSize.Width, monitor.ClientSize.Height);
                Rectangle last = grid.GetCellDisplayRectangle(0, 15, true);
                if (last.Height <= 0 || last.Bottom > grid.ClientSize.Height)
                    throw new Exception("initial monitor does not show all 16 rows");
                int blank = grid.ClientSize.Height - last.Bottom;
                if (blank > SystemInformation.HorizontalScrollBarHeight + 8)
                    throw new Exception("monitor leaves unnecessary blank row area: " + blank + " px");
                Rectangle working = Screen.FromControl(monitor).WorkingArea;
                if (monitor.Width > working.Width || monitor.Height > working.Height)
                    throw new Exception("initial monitor size exceeds working area");
                if (String.IsNullOrEmpty(grid.Rows[0].Cells["Program"].ToolTipText) ||
                    grid.Rows[0].Cells["Program"].ToolTipText.IndexOf("Acoustic Guitar", StringComparison.Ordinal) < 0)
                    throw new Exception("ellipsized Program value is not available through tooltip");
                int before = monitor.Width;
                grid.Columns["Program"].Width += 20;
                Application.DoEvents(); Application.DoEvents();
                if (monitor.Width <= before && before < working.Width - 1)
                    throw new Exception("column resize did not auto-fit monitor width");
                if (monitor.Width > working.Width)
                    throw new Exception("column-driven auto-fit exceeded the working area");
                // Shrink rather than grow so this remains a real resize even on
                // a 1024-pixel hosted desktop where the initial fit is clamped.
                monitor.Width = Math.Max(monitor.MinimumSize.Width, monitor.Width - 10);
                Application.DoEvents();
                Equal(false, monitor.AutoFitEnabledForTesting, "manual form resize disables session auto-fit");
                int manual = monitor.Width;
                grid.Columns["Program"].Width += 20; Application.DoEvents(); Application.DoEvents();
                Equal(manual, monitor.Width, "column changes stop resizing form after manual resize");
                monitor.Close();
            }
        }

        private static void TestBuild21AnalysisGeometry()
        {
            Application.EnableVisualStyles();
            MidiSong song = BuildSong(new long[] { 0, 100000, 200000 });
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, DefaultAnalysisConfiguration());
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                AssertAnalysisGaps(form, "default");
                form.Size = form.MinimumSize; Application.DoEvents();
                AssertAnalysisGaps(form, "minimum");
                form.DetachForSongReplacement("Loading new MIDI…"); Application.DoEvents();
                AssertAnalysisGaps(form, "detached");
                Rectangle splitter = form.AnalysisSplit.SplitterRectangle;
                if (splitter.Width < 4 || splitter.Width > 8) throw new Exception("Analysis splitter hit target is not restrained");
                form.Close();
            }
        }

        private static void AssertAnalysisGaps(DiagnosticsForm form, string state)
        {
            Control header = form.AnalysisHeader;
            Control summary = form.AnalysisSummary;
            Control graph = form.Graph;
            Control seek = form.AnalysisSeekButton;
            SplitContainer split = form.AnalysisSplit;
            Point headerBottom = form.PointToClient(header.PointToScreen(new Point(0, header.Height)));
            Point splitTop = form.PointToClient(split.PointToScreen(Point.Empty));
            Point summaryLeftBottom = form.PointToClient(summary.PointToScreen(new Point(0, summary.Height)));
            Point graphRight = form.PointToClient(graph.PointToScreen(new Point(graph.Width, 0)));
            Point seekRight = form.PointToClient(seek.PointToScreen(new Point(seek.Width, 0)));
            if (Math.Abs((splitTop.Y - headerBottom.Y) - 4) > 1) throw new Exception(state + " Analysis header gap is " + (splitTop.Y - headerBottom.Y));
            if (Math.Abs(summaryLeftBottom.X - 4) > 1) throw new Exception(state + " Analysis report left gap is " + summaryLeftBottom.X);
            if (Math.Abs((form.ClientSize.Height - summaryLeftBottom.Y) - 4) > 1) throw new Exception(state + " Analysis report bottom gap is " + (form.ClientSize.Height - summaryLeftBottom.Y));
            if (Math.Abs((form.ClientSize.Width - graphRight.X) - 4) > 1) throw new Exception(state + " Analysis graph right gap is " + (form.ClientSize.Width - graphRight.X));
            if (Math.Abs((form.ClientSize.Width - seekRight.X) - 4) > 1) throw new Exception(state + " Analysis seek right gap is " + (form.ClientSize.Width - seekRight.X));
            Rectangle divider = split.SplitterRectangle;
            Rectangle report = summary.Bounds;
            Rectangle graphBounds = graph.Bounds;
            if (divider.Left - report.Right > 1 || graphBounds.Left - divider.Right > 1)
                throw new Exception(state + " Analysis splitter has excess surrounding whitespace");
        }

        private static IntPtr MouseCoordinates(int x, int y)
        {
            return new IntPtr((y << 16) | (x & 0xFFFF));
        }

        private static int IndexOfMessage(List<byte[]> messages, params byte[] expected)
        {
            for (int index = 0; index < messages.Count; index++)
            {
                byte[] candidate = messages[index];
                if (candidate.Length != expected.Length) continue;
                bool match = true;
                for (int b = 0; b < expected.Length; b++) if (candidate[b] != expected[b]) { match = false; break; }
                if (match) return index;
            }
            return -1;
        }

        private static void TestBuild19AnalysisUsability()
        {
            Application.EnableVisualStyles();
            MidiSong song = BuildSong(new long[] { 0, 100000, 200000 });
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, DefaultAnalysisConfiguration());
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                Equal(true, form.SummaryWordWrap, "Analysis report wraps long lines");
                Equal(RichTextBoxScrollBars.Vertical, form.SummaryScrollBars, "Analysis report uses only a vertical scrollbar");
                SplitContainer split = form.AnalysisSplit;
                MethodInfo mouseMove = typeof(Control).GetMethod("OnMouseMove", BindingFlags.Instance | BindingFlags.NonPublic);
                mouseMove.Invoke(split, new object[] { new MouseEventArgs(MouseButtons.None, 0, 1, 10, 0) });
                Equal(Cursors.Default, split.Cursor, "Analysis outer border does not advertise splitter dragging");
                Rectangle splitter = split.SplitterRectangle;
                mouseMove.Invoke(split, new object[] { new MouseEventArgs(MouseButtons.None, 0, splitter.Left + splitter.Width / 2, splitter.Top + 10, 0) });
                Equal(Cursors.VSplit, split.Cursor, "actual Analysis splitter uses the resize cursor");
                form.DetachForSongReplacement("MIDI loading cancelled. No MIDI file is loaded.");
                Equal(false, form.SummaryEnabled, "detached Analysis report is disabled");
                form.AttachSong(song, DefaultAnalysisConfiguration());
                Equal(true, form.SummaryEnabled, "successful reattachment enables the Analysis report");
                form.Close();
            }
        }

        private static void TestBuild20FormIcons()
        {
            Application.EnableVisualStyles();
            MidiSong song = BuildSong(new long[] { 0, 100000 });
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, DefaultAnalysisConfiguration());
            using (MainForm main = new MainForm())
            using (DiagnosticsForm diagnostics = new DiagnosticsForm(song, analysis))
            using (ChannelMonitorForm monitor = new ChannelMonitorForm("icons.mid"))
            using (AboutProductDialog about = new AboutProductDialog())
            {
                Form[] forms = new Form[] { main, diagnostics, monitor, about };
                for (int i = 0; i < forms.Length; i++)
                    if (forms[i].Icon == null || forms[i].Icon.Handle == IntPtr.Zero)
                        throw new Exception(forms[i].GetType().Name + " has no managed Form.Icon");
            }
            if (ProductIcon.Value == null || ProductIcon.Value.Handle == IntPtr.Zero)
                throw new Exception("shared managed icon became invalid after forms were disposed");
        }

        private static void TestBuild20CompactLayout()
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); Application.DoEvents();
                form.ClientSize = new Size(416, form.ClientSize.Height); Application.DoEvents();
                if (Math.Abs(form.ClientSize.Width - 416) > 1) throw new Exception("compact client minimum is not 416 pixels at 96 DPI");
                Type type = typeof(MainForm); BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                NumericUpDown queue = (NumericUpDown)type.GetField("_queueLimitValue", flags).GetValue(form);
                NumericUpDown value = (NumericUpDown)type.GetField("_processingValue", flags).GetValue(form);
                Label eventsUnit = (Label)type.GetField("_eventsLabel", flags).GetValue(form);
                Equal(77, queue.Width, "compact Queue measured numeric width");
                queue.Value = queue.Maximum;
                AssertNumericFullyVisible(queue, "compact Queue maximum");
                if (eventsUnit.Visible && eventsUnit.Right > eventsUnit.Parent.ClientSize.Width)
                    throw new Exception("compact events unit is visible while clipped");
                ComboBox mode = (ComboBox)type.GetField("_serviceModeCombo", flags).GetValue(form);
                mode.SelectedIndex = 1; Application.DoEvents();
                Equal(89, value.Width, "compact bitrate measured numeric width");
                value.Value = value.Maximum;
                AssertNumericFullyVisible(value, "compact bitrate maximum");
                mode.SelectedIndex = 0; Application.DoEvents();
                Equal(77, value.Width, "compact processing-time measured numeric width");
                value.Value = value.Maximum;
                AssertNumericFullyVisible(value, "compact processing-time maximum");
                form.ClientSize = new Size(640, form.ClientSize.Height); Application.DoEvents();
                Equal(true, eventsUnit.Visible, "events unit returns when its complete cluster fits");
                Equal(100, queue.Width, "standard Queue width restored");
                Equal(100, value.Width, "standard processing-time width restored");
                form.Close();
            }
            using (StatisticsView statistics = new StatisticsView())
            {
                statistics.Compact = true;
                statistics.Size = new Size(404, statistics.Height);
                statistics.SetValues(new string[] { "01:23:45.678 / 01:23:44.999", "1,097,842 / 1,500,000", "9,500,000 events/s", "8,750,000 events/s", "10,385,604 / 38", "100.2%", "12,345.678 ms", "1,234.567 ms" });
                using (Bitmap bitmap = new Bitmap(statistics.Width, statistics.Height)) statistics.DrawToBitmap(bitmap, statistics.ClientRectangle);
                string first = statistics.SelectedCaptionAt(0);
                if (first != "Timeline/output:" && first != "Timeline:" && first != "Time:") throw new Exception("invalid measured Timeline caption");
                if (statistics.ValueWasTruncated(1)) throw new Exception("Queue readout was truncated instead of abbreviating its caption");
            }
        }

        private static void RenderBuild20Set(string outputDirectory)
        {
            Directory.CreateDirectory(outputDirectory);
            RenderMainWindow(Path.Combine(outputDirectory, "main-standard.png"), false, false, false);
            RenderMainWindow(Path.Combine(outputDirectory, "main-compact.png"), false, true, false);
            RenderChannelMonitor(Path.Combine(outputDirectory, "channel-monitor-forced.png"), false, true);

            Application.EnableVisualStyles();
            using (MainForm compact = new MainForm())
            {
                compact.Show(); Application.DoEvents();
                compact.ClientSize = new Size(416, compact.ClientSize.Height); Application.DoEvents();
                BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                NumericUpDown queue = (NumericUpDown)typeof(MainForm).GetField("_queueLimitValue", flags).GetValue(compact);
                NumericUpDown value = (NumericUpDown)typeof(MainForm).GetField("_processingValue", flags).GetValue(compact);
                ComboBox mode = (ComboBox)typeof(MainForm).GetField("_serviceModeCombo", flags).GetValue(compact);
                queue.Value = queue.Maximum;
                mode.SelectedIndex = 1; Application.DoEvents();
                value.Value = value.Maximum;
                StatisticsView statistics = FindStatisticsView(compact);
                statistics.SetValues(new string[] { "01:23:45.678 / 01:23:44.999", "1,097,842 / 1,500,000", "9,500,000 events/s", "8,750,000 events/s", "10,385,604 / 38", "100.2%", "12,345.678 ms", "1,234.567 ms" });
                using (Bitmap bitmap = new Bitmap(compact.Width, compact.Height))
                {
                    CaptureForm(compact, bitmap); bitmap.Save(Path.Combine(outputDirectory, "main-compact-maximum-values.png"));
                }
                compact.Close();
            }

            using (ChannelMonitorForm monitor = new ChannelMonitorForm("scrub-editor.mid"))
            {
                monitor.Show(); Application.DoEvents();
                ChannelStateTracker tracker = new ChannelStateTracker();
                tracker.RecordOverrideApplied(0, ChannelAttribute.Program, 40);
                monitor.UpdateSnapshot(tracker.CreateSnapshot());
                monitor.ActivateEditorForTesting(0, "Program"); Application.DoEvents();
                monitor.EditorForTesting.EnterTypingForTesting(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(monitor.Width, monitor.Height))
                {
                    CaptureForm(monitor, bitmap); bitmap.Save(Path.Combine(outputDirectory, "channel-scrub-editor.png"));
                }
                monitor.Close();
            }
            using (AboutProductDialog about = new AboutProductDialog())
            {
                about.Show(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(about.Width, about.Height))
                {
                    CaptureForm(about, bitmap); bitmap.Save(Path.Combine(outputDirectory, "about.png"));
                }
                about.Close();
            }
            string synthetic = CreateDenseMidiFile(20000);
            try { RenderAutomaticAnalysisWindowFromFile(synthetic, Path.Combine(outputDirectory, "analysis.png")); }
            finally { DeleteFileWhenAvailable(synthetic); }
        }

        private static void RenderBuild21Set(string outputDirectory)
        {
            Directory.CreateDirectory(outputDirectory);
            Application.EnableVisualStyles();
            ChannelStateTracker tracker = new ChannelStateTracker();
            tracker.RecordSuccessful(ChannelMessage(12345000, 0xC0, 24));
            tracker.RecordSuccessful(ChannelMessage(12345000, 0xB0, 7, 100));
            tracker.RecordSuccessful(ChannelMessage(12345000, 0xB0, 10, 64));
            tracker.MarkAttributeHistoricalDirect(0, ChannelAttribute.Program);
            tracker.RecordOverrideApplied(0, ChannelAttribute.Volume, 110);
            ChannelPlaybackSnapshot snapshot = tracker.CreateSnapshot();
            ChannelOverrideState overrides = new ChannelOverrideState();
            overrides.SetValue(0, ChannelAttribute.Volume, 110);
            overrides.TakePendingMask(0);
            overrides.ApplyToSnapshot(snapshot);
            ChannelRoutingState routing = new ChannelRoutingState();
            routing.SetEnabled(1, false);
            routing.RecordFiltered(1); routing.RecordFiltered(1); routing.RecordFiltered(1);
            routing.ApplyToSnapshot(snapshot);

            using (ChannelMonitorForm monitor = new ChannelMonitorForm("build21-channel-states.mid"))
            {
                monitor.UpdateSnapshot(snapshot);
                monitor.Show(); Application.DoEvents(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(monitor.Width, monitor.Height))
                {
                    CaptureForm(monitor, bitmap); bitmap.Save(Path.Combine(outputDirectory, "channel-monitor-states.png"));
                }
                monitor.ActivateEditorForTesting(2, "Bend"); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(monitor.Width, monitor.Height))
                {
                    CaptureForm(monitor, bitmap); bitmap.Save(Path.Combine(outputDirectory, "channel-scrub-flat.png"));
                }
                monitor.EditorForTesting.EnterTypingForTesting(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(monitor.Width, monitor.Height))
                {
                    CaptureForm(monitor, bitmap); bitmap.Save(Path.Combine(outputDirectory, "channel-scrub-typing.png"));
                }
                monitor.Close();
            }

            MidiSong song = BuildSong(new long[] { 0, 100000, 200000, 400000 });
            song.FilePath = "build21-analysis.mid";
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, DefaultAnalysisConfiguration());
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Show(); Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap); bitmap.Save(Path.Combine(outputDirectory, "analysis-default.png"));
                }
                form.Size = form.MinimumSize; Application.DoEvents();
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap); bitmap.Save(Path.Combine(outputDirectory, "analysis-narrow.png"));
                }
                form.Close();
            }
        }

        private static void RenderBuild23Set(string outputDirectory)
        {
            Directory.CreateDirectory(outputDirectory);
            Application.EnableVisualStyles();
            ChannelStateTracker tracker = new ChannelStateTracker();
            tracker.RecordOverrideApplied(1, ChannelAttribute.Program, 5);
            tracker.MarkAttributeHistoricalDirect(1, ChannelAttribute.Program);
            using (ChannelMonitorForm historical = new ChannelMonitorForm("build21-source-value-chase.mid"))
            {
                historical.UpdateSnapshot(tracker.CreateSnapshot());
                historical.Show(); PumpFor(80);
                using (Bitmap bitmap = new Bitmap(historical.Width, historical.Height))
                {
                    CaptureForm(historical, bitmap);
                    bitmap.Save(Path.Combine(outputDirectory, "program-6-historical-before-source-chase.png"));
                }
                historical.Close();
            }
            tracker.RecordManualChaseApplied(1, ChannelAttribute.Program, 2);
            using (ChannelMonitorForm chased = new ChannelMonitorForm("build21-source-value-chase.mid"))
            {
                chased.UpdateSnapshot(tracker.CreateSnapshot());
                chased.Show(); PumpFor(80);
                using (Bitmap bitmap = new Bitmap(chased.Width, chased.Height))
                {
                    CaptureForm(chased, bitmap);
                    bitmap.Save(Path.Combine(outputDirectory, "program-3-current-after-source-chase.png"));
                }
                chased.Close();
            }
            using (AboutProductDialog about = new AboutProductDialog())
            {
                about.Show(); PumpFor(80);
                using (Bitmap bitmap = new Bitmap(about.Width, about.Height))
                {
                    CaptureForm(about, bitmap);
                    bitmap.Save(Path.Combine(outputDirectory, "about-build21.png"));
                }
                about.Close();
            }
        }

        private static void RenderBuild22NewSet(string outputDirectory)
        {
            Directory.CreateDirectory(outputDirectory);
            Application.EnableVisualStyles();
            MidiSong song = BuildSong(new long[] { 0, 0, 0, 100000 });
            song.FilePath = "build22-analysis-completion.mid";
            song.DurationMicroseconds = 100000;
            AnalysisConfiguration configuration = DefaultAnalysisConfiguration();
            configuration.ProcessingMicroseconds = 100000;
            WorkloadAnalysis analysis = WorkloadAnalyzer.Analyze(song, 100000, configuration);
            using (DiagnosticsForm form = new DiagnosticsForm(song, analysis))
            {
                form.Size = new Size(1180, 760);
                form.Show(); PumpFor(80);
                List<Control> controls = new List<Control>();
                CollectControls(form, controls);
                RichTextBox report = FindControl<RichTextBox>(controls);
                int queueProjection = report.Text.IndexOf("QUEUE PROJECTION", StringComparison.Ordinal);
                if (queueProjection >= 0)
                {
                    report.SelectionStart = queueProjection;
                    report.SelectionLength = 0;
                    report.ScrollToCaret();
                    PumpFor(30);
                }
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    CaptureForm(form, bitmap);
                    bitmap.Save(Path.Combine(outputDirectory, "analysis-queue-completion.png"));
                }
                form.Close();
            }
            using (AboutProductDialog about = new AboutProductDialog())
            {
                about.Show(); PumpFor(40);
                using (Bitmap bitmap = new Bitmap(about.Width, about.Height))
                {
                    CaptureForm(about, bitmap);
                    bitmap.Save(Path.Combine(outputDirectory, "about-build22.png"));
                }
                about.Close();
            }
        }

        private static void TestChannelOverrides()
        {
            foreach (ChannelAttribute attribute in (ChannelAttribute[])Enum.GetValues(typeof(ChannelAttribute)))
            {
                int value = attribute == ChannelAttribute.PitchBend ? -1234 : attribute == ChannelAttribute.Sustain ? 1 : 37;
                MidiEvent message = ChannelOverrideState.CreateMessage(5, attribute, value);
                ChannelAttribute classified;
                int classifiedValue;
                Equal(true, ChannelOverrideState.TryClassify(message, out classified, out classifiedValue), attribute + " override classification");
                Equal(attribute, classified, attribute + " attribute round trip");
                Equal(value, classifiedValue, attribute + " value round trip");
            }
            ChannelOverrideState classification = new ChannelOverrideState();
            classification.SetValue(0, ChannelAttribute.Sustain, 1);
            ChannelAttribute ignoredAttribute;
            int ignoredValue;
            Equal(false, classification.ShouldSuppress(ChannelMessage(0, 0xB0, 123, 0), out ignoredAttribute, out ignoredValue),
                "provider-safety all-notes-off is never override-filtered");
            Equal(true, classification.ShouldSuppress(ChannelMessage(0, 0xB0, 121, 0), out ignoredAttribute, out ignoredValue),
                "source Reset All Controllers is filtered when it conflicts with a forced attribute");
            MidiEvent ordinaryNote = ChannelMessage(0, 0x90, 60, 1);
            int collections = GC.CollectionCount(0);
            for (int repeat = 0; repeat < 500000; repeat++)
                classification.ShouldSuppress(ordinaryNote, out ignoredAttribute, out ignoredValue);
            Equal(collections, GC.CollectionCount(0), "active-override hot-path classification allocates nothing");

            MidiSong filterSong = NewChannelSong("channel-overrides.mid", 0,
                ChannelMessage(0, 0xB0, 7, 20), ChannelMessage(0, 0xB0, 7, 80), ChannelMessage(0, 0xC0, 5));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = false;
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 80);
                engine.SetChannelOverride(0, ChannelAttribute.Program, 10);
                engine.Start(filterSong, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.State == PlaybackState.Completed; }, 1500, "override filtering completion");
                List<byte[]> sent = output.SentPayloads();
                Equal(3, sent.Count, "forced injections plus matching source value reach output");
                Equal(true, ContainsMessage(sent, 0xB0, 7, 80), "volume override reaches output");
                Equal(false, ContainsMessage(sent, 0xB0, 7, 20), "conflicting controller is filtered");
                Equal(true, ContainsMessage(sent, 0xC0, 10), "program override reaches output");
                Equal(false, ContainsMessage(sent, 0xC0, 5), "conflicting program is filtered");
                Equal(1L, engine.GetSnapshot().ProcessedEvents, "suppressed source events do not enter scheduler processing accounting");
                Equal(0L, engine.GetSnapshot().DroppedEvents, "suppression is not queue overflow");
                MidiChannelSnapshot tracked = engine.GetChannelSnapshot().Channels[0];
                Equal(1L, tracked.SentEvents, "only matching source output increments monitor Sent");
                Equal(2L, tracked.OverrideSuppressedEvents, "override-filtered events use separate accounting");
                Equal(80, tracked.ForcedVolume, "forced value is published separately");
                Equal(0, tracked.PendingForcedAttributeMask, "successful injections are no longer pending");

                int beforeAuto = output.SentPayloads().Count;
                engine.SetChannelOverride(0, ChannelAttribute.Volume, ChannelOverrideState.AutoValue);
                Equal(beforeAuto, output.SentPayloads().Count, "Auto sends no reconstructed source value");
                engine.SetChannelMonitoring(false);
                engine.SetChannelOverride(0, ChannelAttribute.Program, 11);
                engine.SetChannelMonitoring(true);
                Equal(11, engine.GetChannelSnapshot().Channels[0].ForcedProgram, "override survives monitor close/reopen");
                engine.Unload();
                Equal(ChannelOverrideState.AutoValue, engine.GetChannelOverride(0, ChannelAttribute.Program), "unload clears overrides");
            }

            MidiSong boundarySong = NewChannelSong("override-boundaries.mid", 30000000,
                ChannelMessage(0, 0x90, 60, 100), ChannelMessage(30000000, 0x80, 60, 0));
            using (PlaybackEngine engine = new PlaybackEngine())
            {
                FakeMidiOutput output = new FakeMidiOutput();
                engine.SetChannelMonitoring(true);
                engine.SimulateSlowdown = false;
                engine.SetChannelOverride(0, ChannelAttribute.Volume, 90);
                engine.SetChannelOverride(0, ChannelAttribute.Sustain, 1);
                engine.Start(boundarySong, output, ProcessingMode.Queue);
                WaitFor(delegate { return engine.GetSnapshot().ProcessedEvents >= 1; }, 1000, "override boundary initial dispatch");
                engine.SetChannelOverride(0, ChannelAttribute.Pan, 33);
                // Keep the next source event far beyond this deadline so a lost
                // wake cannot pass when that event eventually wakes the worker.
                // The five-second deadline also tolerates full-suite x86 GC and
                // host scheduling; an isolated request normally completes at once.
                WaitFor(delegate { return ContainsMessage(output.SentPayloads(), 0xB0, 10, 33); }, 5000,
                    "live override injection through ordered worker");
                engine.Pause();
                Equal(0, output.SentPayloads().Count, "Pause safety reset does not reapply sustain while paused");
                MidiChannelSnapshot paused = engine.GetChannelSnapshot().Channels[0];
                Equal(0, paused.KeysDown, "Pause clears held keys");
                Equal(1L, paused.SentEvents, "Pause preserves sent count");
                Equal(1, paused.PeakKeysDown, "Pause preserves peak keys");
                engine.Resume();
                WaitFor(delegate { return output.SentPayloads().Count >= 2; }, 1000, "Pause/Resume reapplication");
                Equal(true, ContainsMessage(output.SentPayloads(), 0xB0, 7, 90), "Resume reapplies volume");
                Equal(true, ContainsMessage(output.SentPayloads(), 0xB0, 64, 127), "Resume reapplies sustain after safety boundary");

                engine.Seek(500000);
                WaitFor(delegate { return output.SentPayloads().Count >= 2; }, 1000, "Seek override reapplication");
                MidiChannelSnapshot sought = engine.GetChannelSnapshot().Channels[0];
                Equal(0L, sought.SentEvents, "Seek resets channel Sent");
                Equal(0, sought.PeakKeysDown, "Seek resets peak keys");
                Equal(true, ContainsMessage(output.SentPayloads(), 0xB0, 7, 90), "Seek reapplies forced state");

                engine.Stop();
                Equal(-1, engine.GetChannelSnapshot().Channels[0].Program, "Stop clears monitor observations");
                FakeMidiOutput replacement = new FakeMidiOutput();
                engine.Start(boundarySong, replacement, ProcessingMode.Queue, 500000);
                WaitFor(delegate { return replacement.SentPayloads().Count >= 2; }, 1000, "Stop to Play reapplication");
                Equal(true, ContainsMessage(replacement.SentPayloads(), 0xB0, 7, 90), "Stop to Play retains overrides");
                engine.Unload();
            }

            using (PlaybackEngine noneEngine = new PlaybackEngine())
            using (NullMidiOutput none = new NullMidiOutput())
            {
                noneEngine.SetChannelMonitoring(true);
                none.Open();
                noneEngine.Start(NewChannelSong("empty.mid", 0), none, ProcessingMode.Queue);
                WaitFor(delegate { return noneEngine.State == PlaybackState.Completed; }, 1000, "None empty completion");
                noneEngine.SetChannelOverride(2, ChannelAttribute.Aftertouch, 77);
                MidiChannelSnapshot state = noneEngine.GetChannelSnapshot().Channels[2];
                Equal(77, state.ForcedAftertouch, "None accepts logical override injection");
                Equal(0, state.PendingForcedAttributeMask, "None completes logical injection");
            }

            using (PlaybackEngine failing = new PlaybackEngine())
            {
                failing.SetChannelMonitoring(true);
                failing.Start(NewChannelSong("empty.mid", 0), new ThrowingMidiOutput(), ProcessingMode.Queue);
                WaitFor(delegate { return failing.State == PlaybackState.Completed; }, 1000, "empty failing-output completion");
                bool threw = false;
                try { failing.SetChannelOverride(0, ChannelAttribute.Pan, 64); }
                catch (InvalidOperationException) { threw = true; }
                Equal(true, threw, "failed override injection is reported");
                MidiChannelSnapshot failed = failing.GetChannelSnapshot().Channels[0];
                Equal(64, failed.ForcedPan, "failed injection retains configuration");
                Equal(true, (failed.PendingForcedAttributeMask & (1 << (int)ChannelAttribute.Pan)) != 0,
                    "failed injection remains visibly pending");
            }
        }

        private static MidiSong NewChannelSong(string path, long duration, params MidiEvent[] events)
        {
            return new MidiSong
            {
                FilePath = path, Format = 0, TrackCount = 1, TicksPerQuarterNote = 480,
                Events = new List<MidiEvent>(events), DurationMicroseconds = duration
            };
        }

        private static bool ContainsMessage(List<byte[]> messages, params byte[] expected)
        {
            for (int index = 0; index < messages.Count; index++)
            {
                byte[] actual = messages[index];
                if (actual.Length != expected.Length) continue;
                bool equal = true;
                for (int offset = 0; offset < actual.Length; offset++)
                    if (actual[offset] != expected[offset]) { equal = false; break; }
                if (equal) return true;
            }
            return false;
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

        private static MidiEvent ChannelMessage(long time, params byte[] data)
        {
            int command = data == null || data.Length == 0 ? 0 : data[0] & 0xF0;
            MidiEventKind kind = command == 0x80 ? MidiEventKind.NoteOff :
                command == 0x90 ? MidiEventKind.NoteOn :
                command == 0xB0 ? MidiEventKind.ControlChange :
                command == 0xC0 ? MidiEventKind.ProgramChange :
                command == 0xE0 ? MidiEventKind.PitchBend : MidiEventKind.SystemMessage;
            return new MidiEvent
            {
                IntendedMicroseconds = time,
                Kind = kind,
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
                    if (form.LoadingFileAreaWidth < 240 || form.LoadingStatusAreaWidth < 210)
                        throw new Exception("standard loading header does not preserve useful filename/status widths: " +
                            form.LoadingFileAreaWidth + "/" + form.LoadingStatusAreaWidth);
                    if (form.LoadingFileText.IndexOf(Environment.NewLine, StringComparison.Ordinal) < 0 ||
                        form.LoadingFileText.IndexOf("committed", StringComparison.OrdinalIgnoreCase) < 0)
                        throw new Exception("loading filename area does not contain its fixed second-line elapsed/committed-memory telemetry");
                    string largeTelemetry = MainForm.FormatLoadingTelemetry(3723, 12L * 1024 * 1024 * 1024, false);
                    if (largeTelemetry.IndexOf("1:02:03", StringComparison.Ordinal) < 0 ||
                        largeTelemetry.IndexOf("elapsed", StringComparison.OrdinalIgnoreCase) < 0 ||
                        largeTelemetry.IndexOf("12.0 GiB committed", StringComparison.OrdinalIgnoreCase) < 0)
                        throw new Exception("large loading telemetry is not compact and honest: " + largeTelemetry);
                    string compactTelemetry = MainForm.FormatLoadingTelemetry(65, 1536L * 1024 * 1024, true);
                    if (compactTelemetry.IndexOf("elapsed", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        compactTelemetry.IndexOf("committed", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        compactTelemetry.IndexOf("1:05", StringComparison.Ordinal) < 0 ||
                        compactTelemetry.IndexOf("1.50 GiB", StringComparison.Ordinal) < 0)
                        throw new Exception("compact loading telemetry is not abbreviated correctly: " + compactTelemetry);
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
                    if (form.LoadingFileText.IndexOf("Private", StringComparison.Ordinal) >= 0)
                        throw new Exception("loading telemetry remained visible after successful completion");
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
                    if (form.LoadingFileText.IndexOf("Private", StringComparison.Ordinal) >= 0)
                        throw new Exception("loading telemetry remained visible after failure");
                    Equal(processingTop, idleProcessing.Top, "failed loading does not move Processing model");

                    dense = CreateDenseMidiFile(300000);
                    form.BeginMidiLoad(dense);
                    Equal(processingTop, idleProcessing.Top, "progressing load does not move Processing model");
                    int firstTelemetryUpdate = form.LoadingTelemetryUpdateCount;
                    var privateFlags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
                    typeof(MainForm).GetField("_loadStartedTimestamp", privateFlags).SetValue(form,
                        Stopwatch.GetTimestamp() - (long)(2.2 * Stopwatch.Frequency));
                    var updateTelemetry = typeof(MainForm).GetMethod("UpdateLoadingFileTelemetry", privateFlags);
                    updateTelemetry.Invoke(form, new object[] { false });
                    int secondTelemetryUpdate = form.LoadingTelemetryUpdateCount;
                    if (secondTelemetryUpdate <= firstTelemetryUpdate)
                        throw new Exception("loading elapsed/private-memory telemetry did not sample after an elapsed-second change");
                    for (int telemetryCall = 0; telemetryCall < 25; telemetryCall++)
                        updateTelemetry.Invoke(form, new object[] { false });
                    Equal(secondTelemetryUpdate, form.LoadingTelemetryUpdateCount,
                        "loading telemetry does not query/reassign at the 16 ms presentation cadence");
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
                    if (form.LoadingFileText.IndexOf("Private", StringComparison.Ordinal) >= 0 ||
                        form.LoadingFileText.IndexOf(Environment.NewLine, StringComparison.Ordinal) >= 0)
                        throw new Exception("loading telemetry did not disappear immediately after cancellation");
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

                using (MainForm compactLoading = new MainForm())
                {
                    compactLoading.Show(); Application.DoEvents();
                    compactLoading.Size = compactLoading.MinimumSize;
                    Application.DoEvents();
                    List<Control> compactControls = new List<Control>();
                    CollectControls(compactLoading, compactControls);
                    GroupBox compactProcessing = FindGroupBox(compactControls, "Processing model");
                    int compactProcessingTop = compactProcessing.Top;
                    var compactFlags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
                    BufferedStatusLabel compactFile = (BufferedStatusLabel)typeof(MainForm).GetField("_fileLabel", compactFlags).GetValue(compactLoading);
                    BufferedStatusLabel compactStatus = (BufferedStatusLabel)typeof(MainForm).GetField("_loadingStatusLabel", compactFlags).GetValue(compactLoading);
                    compactFile.Text = "Loading an intentionally very long black-MIDI filename.mid…" + Environment.NewLine + "12:34  •  12.4 GiB";
                    compactStatus.Text = "Assigning event timestamps…" + Environment.NewLine + "87.5% stage  •  73.2% overall";
                    typeof(MainForm).GetMethod("SetLoadingPresentation",
                        compactFlags)
                        .Invoke(compactLoading, new object[] { true });
                    Application.DoEvents();
                    if (compactLoading.LoadingFileAreaWidth < 145)
                        throw new Exception("compact filename/telemetry area is not usefully visible: " + compactLoading.LoadingFileAreaWidth);
                    if (compactLoading.LoadingStatusAreaWidth < 165)
                        throw new Exception("compact parser stage/percentage area is not usefully visible: " + compactLoading.LoadingStatusAreaWidth);
                    if (compactLoading.LoadingProgressWidth > 125)
                        throw new Exception("compact loading bars retain an unnecessarily wide footprint");
                    Equal(compactProcessingTop, compactProcessing.Top,
                        "compact loading visibility does not move lower groups");
                    typeof(MainForm).GetMethod("SetLoadingPresentation",
                        System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)
                        .Invoke(compactLoading, new object[] { false });
                    Application.DoEvents();
                    Equal(compactProcessingTop, compactProcessing.Top,
                        "compact loading restoration does not move lower groups");
                    compactLoading.Close();
                }
            }
            finally
            {
                DeleteFileWhenAvailable(first);
                DeleteFileWhenAvailable(second);
                if (dense != null) DeleteFileWhenAvailable(dense);
            }
        }

        private static void TestAnalysisWindowPersistence()
        {
            string first = Path.Combine(Path.GetTempPath(), "analysis-shell-first-" + Guid.NewGuid().ToString("N") + ".mid");
            string second = Path.Combine(Path.GetTempPath(), "analysis-shell-second-" + Guid.NewGuid().ToString("N") + ".mid");
            File.WriteAllBytes(first, BuildTestMidi());
            File.WriteAllBytes(second, BuildTestMidi());
            try
            {
                Application.EnableVisualStyles();
                using (MainForm main = new MainForm())
                {
                    main.SuppressLoadErrorDialogs = true;
                    main.Show(); Application.DoEvents();
                    main.BeginMidiLoad(first);
                    PumpUntil(delegate { return !main.IsLoadingSong; }, 5000, "first Analysis-shell source load");
                    main.ShowAnalysisForTesting();
                    Application.DoEvents();
                    DiagnosticsForm[] windows = main.AnalysisWindowsForTesting;
                    Equal(1, windows.Length, "one Analysis shell opened");
                    DiagnosticsForm shell = windows[0];
                    main.ShowChannelMonitorForTesting();
                    Application.DoEvents();
                    ChannelMonitorForm monitor = main.ChannelMonitorForTesting;
                    if (monitor == null || monitor.IsDisposed) throw new Exception("channel monitor shell did not open");
                    shell.Location = new Point(80, 90);
                    shell.Size = new Size(1000, 620);
                    Point preservedLocation = shell.Location;
                    Size preservedSize = shell.Size;

                    main.BeginMidiLoad(second);
                    Equal(false, shell.IsDisposed, "replacement keeps the Analysis window shell open");
                    Equal(false, shell.HasAttachedSong, "replacement start detaches the old song immediately");
                    Equal(null, shell.CurrentAnalysis, "detached shell retains no old graph result");
                    Equal(null, shell.Graph.Analysis, "detached graph is not interactively usable");
                    Equal(false, monitor.IsDisposed, "replacement keeps the channel monitor shell open");
                    Equal(true, monitor.IsDetached, "replacement start detaches the channel monitor");
                    Equal("Loading new MIDI…", monitor.DetachedMessage, "channel monitor shows the loading state");
                    main.CancelMidiLoad();
                    PumpFor(300);
                    Equal(false, shell.HasAttachedSong, "cancelled replacement leaves an honest detached shell");
                    Equal(false, shell.CalculationPending, "cancelled replacement leaves no stale Analysis work pending");
                    Equal("MIDI loading cancelled. No MIDI file is loaded.", shell.SummaryText,
                        "cancelled replacement updates detached Analysis state");
                    Equal("MIDI loading cancelled. No MIDI file is loaded.", monitor.DetachedMessage,
                        "cancelled replacement updates detached channel-monitor state");

                    main.BeginMidiLoad(second);
                    PumpUntil(delegate { return !main.IsLoadingSong; }, 5000, "replacement Analysis-shell source load");
                    Equal(true, shell.HasAttachedSong, "successful replacement rebinds the same Analysis shell");
                    Equal(Path.GetFullPath(second), shell.SourceSong.FilePath, "rebound shell references only the new song");
                    Equal(preservedLocation, shell.Location, "Analysis shell position survives replacement");
                    Equal(preservedSize, shell.Size, "Analysis shell size survives replacement");
                    Equal(false, monitor.IsDetached, "successful replacement rebinds the existing channel monitor");
                    Equal(false, monitor.IsDisposed, "successful replacement reuses the channel monitor window");
                    PumpUntil(delegate { return !shell.CalculationPending; }, 5000, "rebound Analysis calculation");
                    if (shell.CurrentAnalysis == null) throw new Exception("rebound Analysis shell did not produce a new result");
                    main.BeginMidiLoad(second + ".missing");
                    PumpUntil(delegate { return !main.IsLoadingSong; }, 5000, "failed replacement Analysis shell");
                    Equal(false, shell.HasAttachedSong, "failed replacement leaves the Analysis shell detached");
                    Equal(null, shell.CurrentAnalysis, "failed replacement cannot restore stale old-song Analysis");
                    Equal("MIDI loading failed. No MIDI file is loaded.", shell.SummaryText,
                        "failed replacement updates detached Analysis state");
                    Equal("MIDI loading failed. No MIDI file is loaded.", monitor.DetachedMessage,
                        "failed replacement updates detached channel-monitor state");
                    main.Close();
                    Equal(true, shell.IsDisposed, "main form close safely closes preserved Analysis shell");
                    Equal(true, monitor.IsDisposed, "main form close safely closes preserved channel monitor shell");
                }
            }
            finally
            {
                DeleteFileWhenAvailable(first);
                DeleteFileWhenAvailable(second);
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
                Equal(true, form.AutomaticResolutionSelected, "initial Analysis resolution remains explicit Auto mode");
                if (!form.ResolutionSelectionText.StartsWith("Auto (", StringComparison.Ordinal))
                    throw new Exception("accepted automatic Analysis resolution is not shown in the selector: " + form.ResolutionSelectionText);
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
            MidiSong autoSong = BuildSong(new long[] { 0, 300000000 });
            using (DiagnosticsForm automatic = new DiagnosticsForm(autoSong))
            {
                automatic.Show(); Application.DoEvents();
                automatic.RequestAnalysis(first);
                PumpUntil(delegate { return !automatic.CalculationPending; }, 5000, "initial long-range Auto Analysis");
                string initialAuto = automatic.ResolutionSelectionText;
                Equal(true, automatic.AutomaticResolutionSelected, "long-range selector is Auto before zoom");
                int center = automatic.Graph.GraphArea.Left + automatic.Graph.GraphArea.Width / 2;
                for (int zoom = 0; zoom < 5; zoom++) automatic.Graph.ZoomAtClientX(center, 120);
                PumpUntil(delegate
                {
                    return !automatic.CalculationPending &&
                        !String.Equals(initialAuto, automatic.ResolutionSelectionText, StringComparison.Ordinal);
                }, 5000, "accepted zoomed Auto resolution label");
                Equal(true, automatic.AutomaticResolutionSelected, "dynamic Auto label does not change resolution mode");
                if (!automatic.ResolutionSelectionText.StartsWith("Auto (", StringComparison.Ordinal))
                    throw new Exception("zoomed automatic resolution lost its informative label");
                automatic.Close();
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
                int invocationCountAtCancel = Volatile.Read(ref calculationNumber);
                string acceptedLabelAtCancel = slow.ResolutionSelectionText;
                slow.CancelAnalysisCalculation();
                Equal(false, slow.CalculationStatusVisible, "cancel hides delayed Analysis status");
                Equal(false, slow.CalculationPending, "cancel retires Analysis request");
                // Exercise the real message pump beyond the 180 ms resolution
                // debounce and the header-height change caused by hiding the
                // busy controls. Layout alone must not recreate the request.
                PumpFor(650);
                Equal(invocationCountAtCancel, Volatile.Read(ref calculationNumber),
                    "cancelled Analysis is not restarted by responsive-header layout");
                Equal(false, slow.CalculationPending, "cancel remains retired after queued layout messages drain");
                Equal(acceptedLabelAtCancel, slow.ResolutionSelectionText,
                    "cancel retains the last accepted automatic-resolution label");
                List<Control> slowControls = new List<Control>();
                CollectControls(slow, slowControls);
                ComboBox slowResolution = FindComboContaining(slowControls, "Auto");
                slowResolution.SelectedItem = "25 ms";
                PumpUntil(delegate { return Volatile.Read(ref calculationNumber) > invocationCountAtCancel; }, 1500,
                    "intentional resolution change after cancellation");
                PumpUntil(delegate { return !slow.CalculationPending; }, 3000,
                    "intentional Analysis completion after cancellation");
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
                Equal(false, custom.AutomaticResolutionSelected, "Custom resolution remains distinct from Auto mode");
                if (!custom.ResolutionSelectionText.StartsWith("Custom: ", StringComparison.Ordinal))
                    throw new Exception("Custom Analysis interval is not shown in the selector");
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

        private static void TestAnalysisPredictedCompletion()
        {
            AnalysisConfiguration immediate = DefaultAnalysisConfiguration();
            immediate.SimulateSlowdown = false;
            MidiSong empty = BuildSong(new long[0]);
            Equal(0L, WorkloadAnalyzer.Analyze(empty, 100000, immediate).PredictedOutputCompletionMicroseconds,
                "empty song has no predicted output completion work");

            MidiSong sparse = BuildSong(new long[] { 0, 50000 });
            sparse.DurationMicroseconds = 200000;
            WorkloadAnalysis immediateResult = WorkloadAnalyzer.Analyze(sparse, 100000, immediate);
            Equal(50000L, immediateResult.PredictedOutputCompletionMicroseconds,
                "instantaneous service completes at the last dispatchable event rather than formal source end");
            AnalysisConfiguration zeroProcessing = DefaultAnalysisConfiguration();
            zeroProcessing.ProcessingMicroseconds = 0;
            Equal(50000L, WorkloadAnalyzer.Analyze(sparse, 100000, zeroProcessing).PredictedOutputCompletionMicroseconds,
                "slowdown-enabled zero processing time remains instantaneous");

            AnalysisConfiguration unlimited = DefaultAnalysisConfiguration();
            unlimited.ProcessingMicroseconds = 100000;
            MidiSong burst = BuildSong(new long[] { 0, 0, 0 });
            WorkloadAnalysis unlimitedResult = WorkloadAnalyzer.Analyze(burst, 100000, unlimited);
            Equal(300000L, unlimitedResult.PredictedOutputCompletionMicroseconds,
                "unlimited projection drains all accepted work after the final arrival");

            AnalysisConfiguration bitrate = DefaultAnalysisConfiguration();
            bitrate.ServiceDurationMode = ServiceDurationMode.MidiBitrate;
            bitrate.MidiBitrate = 31250;
            WorkloadAnalysis bitrateResult = WorkloadAnalyzer.Analyze(burst, 100000, bitrate);
            Equal(2880L, bitrateResult.PredictedOutputCompletionMicroseconds,
                "bitrate projection uses exact rounded per-message serial service");

            AnalysisConfiguration finite = DefaultAnalysisConfiguration();
            finite.ProcessingMicroseconds = 100000;
            finite.QueueLengthLimitEnabled = true;
            finite.QueueLengthLimit = 2;
            finite.OverflowPolicy = OverflowPolicy.DropNewest;
            MidiSong finiteSong = BuildSong(new long[] { 0, 0, 100000, 100000, 100000 });
            WorkloadAnalysis finiteResult = WorkloadAnalyzer.Analyze(finiteSong, 100000, finite);
            Equal(300000L, finiteResult.PredictedOutputCompletionMicroseconds,
                "finite projection drains only accepted events");
            Equal(2L, finiteResult.PredictedDroppedEvents, "finite completion excludes dropped events from service");

            AnalysisConfiguration dropOldest = DefaultAnalysisConfiguration();
            dropOldest.ProcessingMicroseconds = 100000;
            dropOldest.QueueLengthLimitEnabled = true;
            dropOldest.QueueLengthLimit = 2;
            dropOldest.OverflowPolicy = OverflowPolicy.DropOldest;
            WorkloadAnalysis dropOldestResult = WorkloadAnalyzer.Analyze(BuildSong(new long[] { 0, 0, 0, 0, 0 }), 100000, dropOldest);
            Equal(200000L, dropOldestResult.PredictedOutputCompletionMicroseconds,
                "drop-oldest completion includes the in-service and final retained pending event");
            Equal(3L, dropOldestResult.PredictedDroppedEvents, "drop-oldest replacement count");

            AnalysisConfiguration clear = DefaultAnalysisConfiguration();
            clear.ProcessingMicroseconds = 100000;
            clear.QueueLengthLimitEnabled = true;
            clear.QueueLengthLimit = 1;
            clear.OverflowPolicy = OverflowPolicy.ClearBufferAndCatchUp;
            MidiSong clearSong = BuildSong(new long[] { 0, 0, 200000 });
            WorkloadAnalysis clearResult = WorkloadAnalyzer.Analyze(clearSong, 100000, clear);
            Equal(300000L, clearResult.PredictedOutputCompletionMicroseconds,
                "clear-buffer completion excludes cleared work and includes later accepted work");
            Equal(1, clearResult.PredictedBufferClears, "clear-buffer projection records its clear boundary");

            string summary = (string)typeof(DiagnosticsForm).GetMethod("BuildSummary",
                BindingFlags.Static | BindingFlags.NonPublic).Invoke(null, new object[] { finiteSong, finiteResult });
            if (!summary.Contains("Source end") || !summary.Contains("Predicted completion") ||
                !summary.Contains("Predicted overrun") || !summary.Contains("not synthesizer or audible-tail completion"))
                throw new Exception("Analysis report does not distinguish source end, modeled completion, overrun, and native limitations");
        }

        private static void TestBuild22MidiFileDrop()
        {
            string first = Path.Combine(Path.GetTempPath(), "midi-drop-first-" + Guid.NewGuid().ToString("N") + ".mid");
            string second = Path.Combine(Path.GetTempPath(), "midi-drop-second-" + Guid.NewGuid().ToString("N") + ".MIDI");
            string invalid = Path.Combine(Path.GetTempPath(), "midi-drop-invalid-" + Guid.NewGuid().ToString("N") + ".txt");
            string dense = null;
            File.WriteAllBytes(first, BuildTestMidi());
            File.WriteAllBytes(second, BuildTestMidi());
            File.WriteAllText(invalid, "not MIDI");
            try
            {
                Application.EnableVisualStyles();
                using (MainForm form = new MainForm())
                {
                    form.SuppressLoadErrorDialogs = true;
                    form.Show(); PumpFor(40);
                    BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
                    Control[] targets = new Control[]
                    {
                        (Control)typeof(MainForm).GetField("_fileLabel", flags).GetValue(form),
                        (Control)typeof(MainForm).GetField("_outputCombo", flags).GetValue(form),
                        (Control)typeof(MainForm).GetField("_processingValue", flags).GetValue(form),
                        (Control)typeof(MainForm).GetField("_timelineView", flags).GetValue(form),
                        (Control)typeof(MainForm).GetField("_statisticsView", flags).GetValue(form)
                    };
                    for (int index = 0; index < targets.Length; index++)
                    {
                        Equal(true, targets[index].AllowDrop, "main child drop target " + index + " is enabled");
                        Equal(DragDropEffects.Copy, RaiseFileDrag(targets[index], new string[] { first }, false),
                            "valid file advertises Copy over child target " + index);
                    }
                    Equal(DragDropEffects.None, RaiseFileDrag(targets[0], new string[] { first, second }, false),
                        "multiple files are rejected");
                    Equal(DragDropEffects.None, RaiseFileDrag(targets[0], new string[] { invalid }, false),
                        "unsupported file is rejected");
                    Equal(DragDropEffects.None, RaiseTextDrag(targets[0], "file:///" + first),
                        "text and URL drops are rejected");

                    RaiseFileDrag(targets[4], new string[] { first }, true);
                    PumpUntil(delegate { return !form.IsLoadingSong; }, 5000, "first dropped MIDI load");
                    Equal(Path.GetFullPath(first), Path.GetFullPath(form.CurrentSong.FilePath), "dropped MIDI uses normal load lifecycle");

                    RaiseFileDrag(targets[1], new string[] { second }, true);
                    PumpUntil(delegate { return !form.IsLoadingSong; }, 5000, "loaded-song drop replacement");
                    Equal(Path.GetFullPath(second), Path.GetFullPath(form.CurrentSong.FilePath), "drop replaces loaded song safely");
                    RaiseFileDrag(targets[0], new string[] { invalid }, true);
                    Equal(Path.GetFullPath(second), Path.GetFullPath(form.CurrentSong.FilePath), "invalid drop leaves current song intact");

                    dense = CreateDenseMidiFile(300000);
                    RaiseFileDrag(targets[2], new string[] { dense }, true);
                    Equal(true, form.IsLoadingSong, "dense dropped MIDI begins asynchronous load");
                    RaiseFileDrag(targets[3], new string[] { first }, true);
                    PumpUntil(delegate { return !form.IsLoadingSong; }, 8000, "drop replacement during active load");
                    Equal(Path.GetFullPath(first), Path.GetFullPath(form.CurrentSong.FilePath),
                        "new drop retires active load generation and wins");

                    form.ClientSize = new Size(416, 389); PumpFor(40);
                    for (int index = 0; index < targets.Length; index++)
                        Equal(DragDropEffects.Copy, RaiseFileDrag(targets[index], new string[] { first }, false),
                            "compact child target accepts MIDI " + index);
                    form.Close();
                }
            }
            finally
            {
                DeleteFileWhenAvailable(first);
                DeleteFileWhenAvailable(second);
                DeleteFileWhenAvailable(invalid);
                DeleteFileWhenAvailable(dense);
            }
        }

        private static DragDropEffects RaiseFileDrag(Control target, string[] files, bool drop)
        {
            DataObject data = new DataObject();
            data.SetData(DataFormats.FileDrop, files);
            DragEventArgs arguments = new DragEventArgs(data, 0, 0, 0, DragDropEffects.Copy, DragDropEffects.None);
            MethodInfo method = typeof(Control).GetMethod(drop ? "OnDragDrop" : "OnDragEnter", BindingFlags.Instance | BindingFlags.NonPublic);
            method.Invoke(target, new object[] { arguments });
            if (!drop) Application.DoEvents();
            return arguments.Effect;
        }

        private static DragDropEffects RaiseTextDrag(Control target, string text)
        {
            DataObject data = new DataObject();
            data.SetData(DataFormats.Text, text);
            DragEventArgs arguments = new DragEventArgs(data, 0, 0, 0, DragDropEffects.Copy, DragDropEffects.None);
            typeof(Control).GetMethod("OnDragEnter", BindingFlags.Instance | BindingFlags.NonPublic)
                .Invoke(target, new object[] { arguments });
            return arguments.Effect;
        }

        private static void TestBuild22AlwaysOnTopMenu()
        {
            Application.EnableVisualStyles();
            using (MainForm form = new MainForm())
            {
                form.Show(); PumpFor(40);
                Equal(false, form.AlwaysOnTopForTesting, "Always on top defaults off");
                IntPtr menu = GetSystemMenu(form.Handle, false);
                int initialCount = GetMenuItemCount(menu);
                uint initialState = GetMenuState(menu, (uint)MainForm.AlwaysOnTopSystemCommandForTesting, 0);
                Equal(0U, initialState & 0x0008U, "Always on top menu starts unchecked");

                SendMessage(form.Handle, 0x0112, (IntPtr)MainForm.AlwaysOnTopSystemCommandForTesting, IntPtr.Zero);
                Application.DoEvents();
                Equal(true, form.AlwaysOnTopForTesting, "system-menu command enables TopMost");
                uint checkedState = GetMenuState(menu, (uint)MainForm.AlwaysOnTopSystemCommandForTesting, 0);
                Equal(0x0008U, checkedState & 0x0008U, "Always on top menu check follows enabled state");

                typeof(Control).GetMethod("RecreateHandle", BindingFlags.Instance | BindingFlags.NonPublic)
                    .Invoke(form, null);
                Application.DoEvents();
                menu = GetSystemMenu(form.Handle, false);
                Equal(initialCount, GetMenuItemCount(menu), "handle recreation does not duplicate system-menu entries");
                checkedState = GetMenuState(menu, (uint)MainForm.AlwaysOnTopSystemCommandForTesting, 0);
                Equal(0x0008U, checkedState & 0x0008U, "handle recreation preserves Always-on-top check");

                SendMessage(form.Handle, 0x0112, (IntPtr)MainForm.AlwaysOnTopSystemCommandForTesting, IntPtr.Zero);
                Application.DoEvents();
                Equal(false, form.AlwaysOnTopForTesting, "second system-menu command disables TopMost");
                form.Close();
            }
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
                Equal(1, form.HeaderRowCount, "default-width global Analysis command bar uses one line");
                Equal(true, form.HeaderControlsFit, "default-width Analysis command bar fits above the full split");
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
                if (!summary.Text.Contains("Graph resolution")) throw new Exception("Analysis aggregation interval is not identified");
                if (!summary.Text.Contains("Predicted drops")) throw new Exception("Analysis prediction summary is absent");
                if (!summary.Text.Contains("Maximum rate")) throw new Exception("Analysis maximum rate is absent");
                if (!summary.Text.Contains("SMF format") || !summary.Text.Contains("PPQN") || !summary.Text.Contains("Musical note-ons"))
                    throw new Exception("Analysis MIDI-file metadata is incomplete");
                form.Size = new Size(1500, 660); Application.DoEvents();
                Equal(1, form.HeaderRowCount, "wide Analysis ordinary controls use one line");
                Equal(true, form.HeaderControlsFit, "wide Analysis ordinary controls fit");
                form.Size = form.MinimumSize; Application.DoEvents();
                if (form.HeaderRowCount < 1 || form.HeaderRowCount > 2)
                    throw new Exception("minimum Analysis header has an unexpected row count: " + form.HeaderRowCount);
                Equal(true, form.HeaderControlsFit, "minimum Analysis header fits or wraps naturally without clipping");
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

        private static void TestAdaptiveTimelineTicks()
        {
            WorkloadAnalysis analysis = new WorkloadAnalysis
            {
                DurationMicroseconds = 2L * 60 * 60 * 1000000 + 17L * 60 * 1000000 + 23000000,
                BucketMicroseconds = 1000000,
                Buckets = new WorkloadBucket[] { new WorkloadBucket() },
                Configuration = DefaultAnalysisConfiguration(),
                MessageTypes = new List<MessageTypeWorkload>()
            };
            using (WorkloadGraph graph = new WorkloadGraph())
            {
                graph.Size = new Size(900, 500);
                graph.Analysis = analysis;
                AssertTimelineTicks(graph, "whole-file hour axis");
                bool sawHours = false;
                foreach (TimelineAxisTick tick in graph.TimelineTicks)
                    if (tick.Label.Split(':').Length == 3) sawHours = true;
                Equal(true, sawHours, "hour-long axis uses hh:mm:ss labels");

                int center = graph.GraphArea.Left + graph.GraphArea.Width / 2;
                for (int i = 0; i < 9; i++) graph.ZoomAtClientX(center, 120);
                graph.PanByPixels(37);
                AssertTimelineTicks(graph, "non-round zoomed viewport");

                WorkloadAnalysis close = new WorkloadAnalysis
                {
                    DurationMicroseconds = 200000,
                    BucketMicroseconds = 1000,
                    Buckets = new WorkloadBucket[] { new WorkloadBucket() },
                    Configuration = DefaultAnalysisConfiguration(),
                    MessageTypes = new List<MessageTypeWorkload>()
                };
                graph.Analysis = close;
                graph.ResetZoom();
                AssertTimelineTicks(graph, "millisecond close zoom");
                bool sawFraction = false;
                foreach (TimelineAxisTick tick in graph.TimelineTicks)
                    if (tick.Label.IndexOf('.') >= 0) sawFraction = true;
                Equal(true, sawFraction, "close zoom uses fractional-second labels");

                graph.Size = new Size(420, 360);
                AssertTimelineTicks(graph, "narrow collision avoidance");
                List<long> before = new List<long>();
                foreach (TimelineAxisTick tick in graph.TimelineTicks) before.Add(tick.TimeMicroseconds);
                graph.SetPlaybackPositions(100000, 90000);
                List<long> after = new List<long>();
                foreach (TimelineAxisTick tick in graph.TimelineTicks) after.Add(tick.TimeMicroseconds);
                Sequence(before, after, "live markers do not move static axis ticks");
            }
        }

        private static void AssertTimelineTicks(WorkloadGraph graph, string name)
        {
            System.Collections.Generic.IList<TimelineAxisTick> ticks = graph.TimelineTicks;
            if (ticks.Count < 1) throw new Exception(name + " produced no readable tick");
            long interval = WorkloadGraph.ChooseTimelineTickInterval(
                graph.ViewEndMicroseconds - graph.ViewStartMicroseconds, graph.GraphArea.Width);
            int previousRight = Int32.MinValue;
            for (int i = 0; i < ticks.Count; i++)
            {
                Equal(0L, ticks[i].TimeMicroseconds % interval, name + " aligned tick " + i);
                if (ticks[i].LabelBounds.Left < previousRight + 10)
                    throw new Exception(name + " labels collide at tick " + i);
                if (ticks[i].LabelBounds.Left < 2 || ticks[i].LabelBounds.Right > graph.ClientSize.Width - 2)
                    throw new Exception(name + " tick label is outside the balanced plot allowance");
                previousRight = ticks[i].LabelBounds.Right;
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

        private static void ReportMidiStatePrefix(string path, long endMicroseconds)
        {
            MidiSong song = MidiFileParser.Load(path);
            Console.WriteLine("File: " + path);
            Console.WriteLine("Format={0}, tracks={1}, PPQN={2}, events={3:N0}", song.Format, song.TrackCount,
                song.TicksPerQuarterNote, song.Events.Count);
            for (int i = 0; i < song.Events.Count; i++)
            {
                MidiEvent midiEvent = song.Events[i];
                if (midiEvent.IntendedMicroseconds > endMicroseconds) break;
                if (midiEvent.Kind == MidiEventKind.NoteOn || midiEvent.Kind == MidiEventKind.NoteOff ||
                    midiEvent.Kind == MidiEventKind.PolyphonicAftertouch) continue;
                string bytes = midiEvent.Data.ToHexString().Replace('-', ' ');
                if (bytes.Length > 160) bytes = bytes.Substring(0, 160) + " … (" + midiEvent.Data.Length + " bytes)";
                Console.WriteLine("{0,10} us tick={1,-9} track={2,-4} event={3,-9} {4,-18} {5}",
                    midiEvent.IntendedMicroseconds, midiEvent.AbsoluteTick, midiEvent.Track, midiEvent.EventIndex,
                    midiEvent.Kind, bytes);
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
                Console.WriteLine("Opening WinMM device " + omni.DeviceId + ": " + omni.Name + "; module " + WindowsMidiOutput.GetLoadedModulePath());
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
                Equal(ProductIdentity.Name, form.Text, "window title");
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
                Equal(432, form.MinimumSize.Width, "compact-layout minimum width at 96 DPI");
                Equal(form.RealizedRequiredWindowHeight, form.MinimumSize.Height,
                    "compact minimum height follows realized content");
                int compactMinimumHeight = form.MinimumSize.Height;
                if (compactMinimumHeight >= 470)
                    throw new Exception("compact minimum height was not reduced from its obsolete fixed value: " + compactMinimumHeight);
                Equal(compactMinimumHeight, form.MaximumSize.Height, "compact height cap");
                Equal(2, statistics.ColumnCount, "compact statistics remain two columns");
                string[] expectedCompactCaptions = new string[] { "Timeline/output:", "Queue now/max:", "Max rate:",
                    "Output rate:", "Sent/dropped:", "Effective speed:", "Max lag:", "Current lag:" };
                for (int captionIndex = 0; captionIndex < expectedCompactCaptions.Length; captionIndex++)
                    Equal(expectedCompactCaptions[captionIndex], StatisticsView.CompactCaptionAt(captionIndex),
                        "compact statistic caption " + captionIndex);
                int[] compactWidths = new int[] { 620, 580, 540, 500, 465, 440, 432 };
                for (int widthIndex = 0; widthIndex < compactWidths.Length; widthIndex++)
                {
                    form.Size = new Size(compactWidths[widthIndex], compactMinimumHeight);
                    Application.DoEvents();
                    AssertProcessingClusters(form, "active compact resize at " + compactWidths[widthIndex] + " pixels");
                }
                form.Size = form.MinimumSize;
                Application.DoEvents();
                int compactNonClientWidth = form.Width - form.ClientSize.Width;
                int compactNonClientHeight = form.Height - form.ClientSize.Height;
                Console.WriteLine("      Compact metrics: outer " + form.Width + "x" + form.Height +
                    "; client " + form.ClientSize.Width + "x" + form.ClientSize.Height +
                    "; non-client " + compactNonClientWidth + "x" + compactNonClientHeight);
                if (form.ClientSize.Width < 415 || form.ClientSize.Width > 417)
                    throw new Exception("accepted compact client width is outside the verified usable experiment: " + form.ClientSize.Width);
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
                // This is an isolated wall-clock layout benchmark near the end
                // of a large single-process suite. Collect prior test fixtures
                // before starting the clock so an unrelated generation-2 pause
                // is not reported as a responsive-layout regression.
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
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
                // Per-crossing latency is the user-visible contract.  The total
                // guard still catches cumulative layout regressions, while
                // allowing normal variation in twenty realized native-control
                // resize/paint cycles on a loaded desktop.
                // Focused runs remain around 190 ms / 2.7 s on this host.  Keep
                // a modest loaded-desktop allowance for the same realized path
                // near the end of the all-in-one x86 suite without discarding
                // either the per-crossing or cumulative regression guard.
                if (slowestTransition >= 300 || responsiveTimer.ElapsedMilliseconds > 3500)
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
                Equal(false, slowdown.Checked, "simulate slowdown default");
                slowdown.Checked = true;
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
                {
                    string candidate = combo.Items[item].ToString();
                    if (String.Equals(candidate, itemText, StringComparison.Ordinal) ||
                        (String.Equals(itemText, "Auto", StringComparison.Ordinal) &&
                            candidate.StartsWith("Auto", StringComparison.Ordinal))) return combo;
                }
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
                    if (combo.Items[item] is MidiOutputDeviceInfo || MainForm.IsNoOutputSelection(combo.Items[item])) return combo;
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
            int textWidth = TextRenderer.MeasureText(maximum, numeric.Font, Size.Empty,
                TextFormatFlags.NoPadding | TextFormatFlags.SingleLine).Width;
            if (numeric.ClientSize.Width < textWidth + SystemInformation.VerticalScrollBarWidth + 5)
                throw new Exception(name + " is too narrow for its maximum value and spinner buttons: client " +
                    numeric.ClientSize.Width + ", text " + textWidth + ", spinner " + SystemInformation.VerticalScrollBarWidth);
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
                midiEvent.EventIndex = i;
                song.Events.Add(midiEvent);
            }
            song.NoteCount = eventTimes.Length;
            song.DurationMicroseconds = eventTimes.Length == 0 ? 0 : eventTimes[eventTimes.Length - 1];
            return song;
        }

        private static MidiSong BuildCompleteNotePolicySong()
        {
            MidiSong song = new MidiSong();
            song.FilePath = "complete-note-overflow.mid";
            song.Format = 0;
            song.TrackCount = 1;
            song.TicksPerQuarterNote = 480;
            song.Events = new List<MidiEvent>();
            song.Events.Add(ChannelEvent(0, 0x90, 60, 100, 0)); // retained NoteOn
            song.Events.Add(ChannelEvent(0, 0x90, 60, 90, 1));  // overlapping NoteOn rejected
            song.Events.Add(ChannelEvent(0, 0x90, 60, 0, 2));   // FIFO Off for retained occurrence
            song.Events.Add(ChannelEvent(0, 0x80, 60, 0, 3));   // Off paired with rejected occurrence
            song.Events.Add(ChannelEvent(0, 0xB0, 7, 100, 4));  // protected non-note message
            song.Events.Add(ChannelEvent(0, 0x90, 61, 80, 5));  // rejected NoteOn
            song.Events.Add(ChannelEvent(50000, 0x80, 61, 0, 6)); // much-later paired Off suppressed
            song.Events.Add(ChannelEvent(50000, 0x80, 62, 0, 7)); // unmatched Off retained
            song.NoteCount = 3;
            song.DurationMicroseconds = 50000;
            return song;
        }

        private static MidiEvent ChannelEvent(long time, byte status, byte data1, byte data2, int index)
        {
            int command = status & 0xF0;
            MidiEventKind kind = command == 0x80 ? MidiEventKind.NoteOff :
                command == 0x90 ? MidiEventKind.NoteOn :
                command == 0xB0 ? MidiEventKind.ControlChange : MidiEventKind.SystemMessage;
            return new MidiEvent
            {
                AbsoluteTick = index,
                IntendedMicroseconds = time,
                Track = 0,
                Order = index,
                Kind = kind,
                Channel = status & 0x0F,
                Status = status,
                Data = new byte[] { status, data1, data2 },
                EventIndex = index
            };
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

        private sealed class ThrowingMidiOutput : IMidiOutput
        {
            public void Send(MidiEvent midiEvent) { throw new InvalidOperationException("Synthetic output failure"); }
            public void Panic() { }
            public void Reset() { }
        }

        private sealed class ToggleFailureOutput : IMidiOutput
        {
            internal bool Throw;
            public void Send(MidiEvent midiEvent)
            {
                if (Throw) throw new InvalidOperationException("Synthetic output failure");
            }
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

        private sealed class BlockingLifecycleOutput : IMidiOutput
        {
            internal readonly ManualResetEvent Entered = new ManualResetEvent(false);
            internal readonly ManualResetEvent Release = new ManualResetEvent(false);
            private readonly object _sync = new object();
            private readonly List<int> _beginNotes = new List<int>();
            private readonly List<int> _beginSequences = new List<int>();
            private readonly List<int> _panicSequences = new List<int>();
            private int _sequence;
            private int _blockFirst = 1;
            private int _activeSends;
            internal int MaximumConcurrentSends;
            internal int FirstSendEndSequence;
            internal int LastResetSequence;
            internal int LastPanicSequence;

            internal int SendBeginCount { get { lock (_sync) return _beginNotes.Count; } }

            public void Send(MidiEvent midiEvent)
            {
                int active = Interlocked.Increment(ref _activeSends);
                int observed;
                do
                {
                    observed = MaximumConcurrentSends;
                    if (active <= observed) break;
                }
                while (Interlocked.CompareExchange(ref MaximumConcurrentSends, active, observed) != observed);
                try
                {
                    int note = midiEvent.Data.Length < 2 ? -1 : midiEvent.Data[1];
                    lock (_sync)
                    {
                        _beginNotes.Add(note);
                        _beginSequences.Add(++_sequence);
                    }
                    if (Interlocked.Exchange(ref _blockFirst, 0) != 0)
                    {
                        Entered.Set();
                        Release.WaitOne();
                    }
                    lock (_sync)
                    {
                        int ended = ++_sequence;
                        if (FirstSendEndSequence == 0) FirstSendEndSequence = ended;
                    }
                }
                finally { Interlocked.Decrement(ref _activeSends); }
            }

            public void Reset() { lock (_sync) { LastResetSequence = ++_sequence; } }
            public void Panic()
            {
                lock (_sync)
                {
                    LastPanicSequence = ++_sequence;
                    _panicSequences.Add(LastPanicSequence);
                }
            }

            internal bool HasBegunNote(int note)
            {
                lock (_sync) return _beginNotes.Contains(note);
            }

            internal int FirstBeginSequenceForNote(int note)
            {
                lock (_sync)
                {
                    for (int i = 0; i < _beginNotes.Count; i++)
                        if (_beginNotes[i] == note) return _beginSequences[i];
                    return 0;
                }
            }

            internal int LastPanicBefore(int sequence)
            {
                lock (_sync)
                {
                    int result = 0;
                    for (int i = 0; i < _panicSequences.Count; i++)
                        if (_panicSequences[i] < sequence) result = _panicSequences[i];
                    return result;
                }
            }
        }

        private sealed class CadenceProbeOutput : IMidiOutput
        {
            private readonly int _checkpoint;
            private readonly int _expectedCount;
            private readonly long[] _checkpointStamps;
            internal int Count;
            internal bool PayloadMismatch;

            internal CadenceProbeOutput(int checkpoint, int expectedCount)
            {
                _checkpoint = checkpoint;
                _expectedCount = expectedCount;
                _checkpointStamps = new long[(expectedCount + checkpoint - 1) / checkpoint];
            }

            public void Send(MidiEvent midiEvent)
            {
                int index = Count;
                if (index % _checkpoint == 0) _checkpointStamps[index / _checkpoint] = Stopwatch.GetTimestamp();
                if (index >= _expectedCount || midiEvent == null || midiEvent.Order != index ||
                    midiEvent.EventIndex != index || midiEvent.Status != 0x90 ||
                    midiEvent.Data.Length != 3 || midiEvent.Data[0] != 0x90 ||
                    midiEvent.Data[1] != (byte)(60 + (index % 12)) || midiEvent.Data[2] != 1)
                    PayloadMismatch = true;
                Thread.SpinWait(40);
                Count = index + 1;
            }

            internal double[] CheckpointIntervalsMilliseconds()
            {
                double[] result = new double[Math.Max(0, _checkpointStamps.Length - 1)];
                for (int i = 1; i < _checkpointStamps.Length; i++)
                    result[i - 1] = (_checkpointStamps[i] - _checkpointStamps[i - 1]) * 1000.0 / Stopwatch.Frequency;
                return result;
            }

            public void Panic() { }
            public void Reset() { }
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

        private sealed class ArrayMidiEventStore : IMidiEventStore
        {
            private readonly MidiEvent[] _events;
            internal ArrayMidiEventStore(MidiEvent[] events) { _events = events; }
            public int Count { get { return _events.Length; } }
            public MidiEvent GetEvent(int index) { return _events[index]; }
            public int LowerBoundByTime(long microseconds)
            {
                int low = 0;
                int high = _events.Length;
                while (low < high)
                {
                    int middle = low + ((high - low) >> 1);
                    if (_events[middle].IntendedMicroseconds < microseconds) low = middle + 1;
                    else high = middle;
                }
                return low;
            }
        }

        private sealed class AcknowledgingMidiOutput : IMidiOutput
        {
            internal readonly ManualResetEvent Entered = new ManualResetEvent(false);
            internal readonly ManualResetEvent Release = new ManualResetEvent(false);
            internal volatile bool FailMatchingControl;
            private readonly int _blockSendNumber;
            private readonly object _sync = new object();
            private readonly List<byte[]> _payloads = new List<byte[]>();
            private int _sendNumber;

            internal AcknowledgingMidiOutput(int blockSendNumber) { _blockSendNumber = blockSendNumber; }

            public void Send(MidiEvent midiEvent)
            {
                int sendNumber = Interlocked.Increment(ref _sendNumber);
                if (sendNumber == _blockSendNumber)
                {
                    Entered.Set();
                    Release.WaitOne();
                }
                byte[] payload = midiEvent.Data.ToArray();
                if (FailMatchingControl && payload.Length == 3 && payload[0] == 0xB0 && payload[1] == 7 && payload[2] == 80)
                    throw new InvalidOperationException("Synthetic channel-control failure.");
                lock (_sync) _payloads.Add((byte[])payload.Clone());
            }

            public void Reset() { }
            public void Panic() { }

            internal int CountPayload(params byte[] expected)
            {
                int count = 0;
                lock (_sync)
                {
                    for (int i = 0; i < _payloads.Count; i++)
                    {
                        byte[] payload = _payloads[i];
                        if (payload.Length != expected.Length) continue;
                        bool matches = true;
                        for (int b = 0; b < payload.Length; b++)
                            if (payload[b] != expected[b]) { matches = false; break; }
                        if (matches) count++;
                    }
                }
                return count;
            }
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
                        notes.Add(_sentEvents[i].Data.Length > 1 ? _sentEvents[i].Data[1] : -1);
                    return notes;
                }
            }

            public List<int> SentEventIndices()
            {
                lock (_sync)
                {
                    List<int> indices = new List<int>(_sentEvents.Count);
                    for (int i = 0; i < _sentEvents.Count; i++) indices.Add(_sentEvents[i].EventIndex);
                    return indices;
                }
            }

            public List<byte[]> SentPayloads()
            {
                lock (_sync)
                {
                    List<byte[]> payloads = new List<byte[]>(_sentEvents.Count);
                    for (int i = 0; i < _sentEvents.Count; i++) payloads.Add(_sentEvents[i].Data.ToArray());
                    return payloads;
                }
            }

            public List<MidiEvent> SentEvents()
            {
                lock (_sync) return new List<MidiEvent>(_sentEvents);
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

        private sealed class BenchmarkKdmApiNative : IKdmApiNative
        {
            public long ShortCount;
            public bool IsAvailable() { return true; }
            public bool InitializeStream() { return true; }
            public bool TerminateStream() { return true; }
            public void ResetStream() { }
            public void SendShort(uint message) { ShortCount++; }
            public uint PrepareLong(IntPtr header, uint headerSize) { return 0; }
            public uint SendLong(IntPtr header, uint headerSize) { return 0; }
            public uint UnprepareLong(IntPtr header, uint headerSize) { return 0; }
            public string Version { get { return "benchmark"; } }
            public string ProviderPath { get { return "deterministic KDMAPI boundary"; } }
            public bool SupportsLongMessages { get { return true; } }
            public string LongMessageStatus { get { return "benchmark"; } }
            public void Dispose() { }
        }
    }
}
