using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed partial class PlaybackEngine : IDisposable
    {
        internal const int DefaultQueueLengthLimit = 2000;
        internal const long DefaultQueueAgeLimitMicroseconds = 1000000;
        internal const int DefaultDropBufferCapacity = DefaultQueueLengthLimit;
        internal const int DefaultIntervalPolyphonyLimit = 128;
        private readonly object _sync = new object();
        private readonly EventWaitHandle _wake = new EventWaitHandle(false, EventResetMode.ManualReset);
        private readonly EventWaitHandle _pauseAcknowledged = new EventWaitHandle(false, EventResetMode.ManualReset);
        private int _wakeGeneration;
        private Thread _thread;
        private MidiSong _song;
        private MidiEventReader _events;
        private IMidiOutput _output;
        private ProcessingMode _mode;
        private int _startEventIndex;
        private PlaybackState _state = PlaybackState.Stopped;
        private ServiceDurationSettings _serviceSettings = new ServiceDurationSettings(
            ServiceDurationMode.ProcessingTime, 0, ServiceDurationCalculator.FivePinDinBitrate, 10000);
        private int _queueLengthLimit = DefaultQueueLengthLimit;
        private int _queueAgeLimitEnabled;
        private long _queueAgeLimitMicroseconds = DefaultQueueAgeLimitMicroseconds;
        private int _simulateSlowdown = 1;
        private int _applyQueueLimitWithoutSlowdown;
        private int _chaseMidiStateOnPlaySeek = 1;
        private int _preservePlaybackQueueOnPauseSeek;
        private int _outputPacedTimeline;
        private int _scaleNoteIntervalByTraffic;
        private int _intervalPolyphonyLimitEnabled;
        private int _intervalPolyphonyLimit = DefaultIntervalPolyphonyLimit;
        private int _pendingStateChaseEventIndex = -1;
        private int _pauseRequestGeneration;
        private int _pauseAcknowledgedGeneration;
        private int _preservedSeekGeneration;
        private const int MaximumPendingPreservedSeeks = 256;
        private readonly Queue<PreservedSeekRequest> _preservedSeekRequests =
            new Queue<PreservedSeekRequest>();
        private long _intendedTimelineOverrideMicroseconds = -1;
        private bool _virtualForwardDropActive;
        private int _overflowPolicy;
        private long _transportBaseTicks;
        private long _runStartStamp;
        private int _outputPacedHoldDepth;
        private long _outputPacedHoldStartStamp;
        private long _outputPacedHoldTransportTicks;
        private int _dispatchSuspended = 1;
        private bool _silenceWhenWorkerExits;
        private Exception _workerExitSilenceFailure;
        private int _workerStopTimeoutMilliseconds = 2000;

        private long _queueLength;
        private long _outstandingEvents;
        private long _maximumQueueLength;
        private long _queueAgeMicroseconds;
        private long _maximumQueueAgeMicroseconds;
        private long _processedEvents;
        private long _droppedEvents;
        private long _gateFilteredEvents;
        private long _musicalLimitRejectedAttacks;
        private long _musicalLimitSuppressedReleases;
        private long _outputRateRejectedAttacks;
        private long _outputRateSuppressedReleases;
        private long _outputRateRejectedOtherMessages;
        private long _outputRateSafetySoftExcess;
        private long _outputRateSafetyBypassMessages;
        private OutputRateGate _activeOutputRateGate;
        private long _virtualQueueLength;
        private long _virtualOutstandingEvents;
        private long _virtualMaximumQueueLength;
        private long _lastDispatchedMicroseconds;
        private long _effectiveSpeedFrontierMicroseconds;
        private long _currentLagMicroseconds;
        private long _maximumLagMicroseconds;
        private int _publishedNextProcess;
        private int _publishedDropNextArrival;
        private int _publishedDropPending;
        private bool _publishedInService;
        private int _publishedInServiceEvent = -1;
        private bool _hasPublishedQueue;
        private int _publishedOldestPendingEvent = -1;
        private bool _hasPublishedStructuralDueWork;
        private int _publishedStructuralNextArrival;
        private long _publishedStructuralPending;
        private long _publishedStructuralOldestDueMicroseconds = Int64.MaxValue;
        private bool _publishedStructuralInService;
        private long _publishedStructuralInServiceDueMicroseconds = Int64.MaxValue;
        private long _publishedStructuralFutureDueMicroseconds = Int64.MaxValue;
        private int _publishedStructuralFutureCount;
        private int _snapshotFilterStart = -1;
        private int _snapshotFilterEnd = -1;
        private long _snapshotEligibleCount;
        private int _snapshotFirstEligibleIndex = -1;
        private int _snapshotRoutingGeneration = -1;
        private int _snapshotOverrideGeneration = -1;
        private int _usesPreAdmissionFilterAccounting;
        private DropTraceRecorder _dropTrace;
        private SourceDispatchTrace _sourceDispatchTrace;
        private long _sourceDispatchTraceSession;
        private Action<long> _gateBoundaryDispatchHookForTesting;
        private Action<long, IList<MidiEventView>, int> _gateLogicalOutputTraceForTesting;
        private readonly ChannelStateTracker _channelState = new ChannelStateTracker();
        private readonly ChannelOverrideState _channelOverrides = new ChannelOverrideState();
        private readonly ChannelRoutingState _channelRouting = new ChannelRoutingState();
        private readonly Queue<ChannelControlRequest> _channelControlRequests = new Queue<ChannelControlRequest>();
        private volatile ChannelPlaybackSnapshot _publishedChannelState;
        private int _channelMonitoringEnabled;
        private long _lastChannelPublishStamp;

        public event EventHandler PlaybackEnded;
        public event EventHandler<PlaybackErrorEventArgs> PlaybackFailed;
        internal event EventHandler<ChannelControlErrorEventArgs> ChannelControlFailed;

        public PlaybackState State { get { lock (_sync) return _state; } }

        public long ProcessingMicroseconds
        {
            get { return Volatile.Read(ref _serviceSettings).ProcessingMicroseconds; }
            set
            {
                if (value < 0) value = 0;
                lock (_sync)
                {
                    ServiceDurationSettings previous = _serviceSettings;
                    if (previous.ProcessingMicroseconds == value) return;
                    Volatile.Write(ref _serviceSettings, new ServiceDurationSettings(previous.Mode,
                        value, previous.MidiBitrate, previous.EventsPerSecond));
                }
                SignalWake();
            }
        }

        public ServiceDurationMode ServiceDurationMode
        {
            get { return Volatile.Read(ref _serviceSettings).Mode; }
            set
            {
                lock (_sync)
                {
                    ServiceDurationSettings previous = _serviceSettings;
                    if (previous.Mode == value) return;
                    Volatile.Write(ref _serviceSettings, new ServiceDurationSettings(value,
                        previous.ProcessingMicroseconds, previous.MidiBitrate, previous.EventsPerSecond));
                }
                SignalWake();
            }
        }

        public long MidiBitrate
        {
            get { return Volatile.Read(ref _serviceSettings).MidiBitrate; }
            set
            {
                if (value < 1) value = 1;
                lock (_sync)
                {
                    ServiceDurationSettings previous = _serviceSettings;
                    if (previous.MidiBitrate == value) return;
                    Volatile.Write(ref _serviceSettings, new ServiceDurationSettings(previous.Mode,
                        previous.ProcessingMicroseconds, value, previous.EventsPerSecond));
                }
                SignalWake();
            }
        }

        public long EventsPerSecond
        {
            get { return Volatile.Read(ref _serviceSettings).EventsPerSecond; }
            set
            {
                if (value < 0) value = 0;
                if (value > 9999999) value = 9999999;
                lock (_sync)
                {
                    ServiceDurationSettings previous = _serviceSettings;
                    if (previous.EventsPerSecond == value) return;
                    Volatile.Write(ref _serviceSettings, new ServiceDurationSettings(previous.Mode,
                        previous.ProcessingMicroseconds, previous.MidiBitrate, value));
                }
                SignalWake();
            }
        }

        internal void SetServiceConfiguration(ServiceDurationMode mode, long processingMicroseconds,
            long midiBitrate, long eventsPerSecond)
        {
            lock (_sync)
                Volatile.Write(ref _serviceSettings, new ServiceDurationSettings(mode,
                    Math.Max(0, processingMicroseconds), Math.Max(1, midiBitrate),
                    Math.Max(0, Math.Min(9999999, eventsPerSecond))));
            SignalWake();
        }

        public bool SimulateSlowdown
        {
            get { return Volatile.Read(ref _simulateSlowdown) != 0; }
            set
            {
                Volatile.Write(ref _simulateSlowdown, value ? 1 : 0);
                SignalWake();
            }
        }

        public bool ApplyQueueLimitWithoutSlowdown
        {
            get { return Volatile.Read(ref _applyQueueLimitWithoutSlowdown) != 0; }
            set { Volatile.Write(ref _applyQueueLimitWithoutSlowdown, value ? 1 : 0); }
        }

        public bool ChaseMidiStateOnPlaySeek
        {
            get { return Volatile.Read(ref _chaseMidiStateOnPlaySeek) != 0; }
            set
            {
                Volatile.Write(ref _chaseMidiStateOnPlaySeek, value ? 1 : 0);
                if (!value) Interlocked.Exchange(ref _pendingStateChaseEventIndex, -1);
            }
        }

        public bool PreservePlaybackQueueOnPauseSeek
        {
            get { return Volatile.Read(ref _preservePlaybackQueueOnPauseSeek) != 0; }
            set { Volatile.Write(ref _preservePlaybackQueueOnPauseSeek, value ? 1 : 0); }
        }

        public bool OutputPacedTimeline
        {
            get { return Volatile.Read(ref _outputPacedTimeline) != 0; }
            set { Volatile.Write(ref _outputPacedTimeline, value ? 1 : 0); }
        }

        public bool ScaleNoteIntervalByTraffic
        {
            get { return Volatile.Read(ref _scaleNoteIntervalByTraffic) != 0; }
            set { Volatile.Write(ref _scaleNoteIntervalByTraffic, value ? 1 : 0); }
        }

        public bool IntervalPolyphonyLimitEnabled
        {
            get { return Volatile.Read(ref _intervalPolyphonyLimitEnabled) != 0; }
            set { Volatile.Write(ref _intervalPolyphonyLimitEnabled, value ? 1 : 0); }
        }

        public int IntervalPolyphonyLimit
        {
            get { return Volatile.Read(ref _intervalPolyphonyLimit); }
            set
            {
                if (value < 1) value = 1;
                if (value > DefaultIntervalPolyphonyLimit)
                    value = DefaultIntervalPolyphonyLimit;
                Volatile.Write(ref _intervalPolyphonyLimit, value);
            }
        }

        public int QueueLengthLimit
        {
            get { return Volatile.Read(ref _queueLengthLimit); }
            set
            {
                if (value < 1) value = 1;
                if (value > 1000000) value = 1000000;
                Volatile.Write(ref _queueLengthLimit, value);
                SignalWake();
            }
        }

        public bool QueueAgeLimitEnabled
        {
            get { return Volatile.Read(ref _queueAgeLimitEnabled) != 0; }
            set { Volatile.Write(ref _queueAgeLimitEnabled, value ? 1 : 0); SignalWake(); }
        }

        public long QueueAgeLimitMicroseconds
        {
            get { return Interlocked.Read(ref _queueAgeLimitMicroseconds); }
            set
            {
                if (value < 1) value = 1;
                if (value > 3600000000L) value = 3600000000L;
                Interlocked.Exchange(ref _queueAgeLimitMicroseconds, value);
                SignalWake();
            }
        }

        // Compatibility alias for the production trace/test entry points from
        // the earlier experimental Drop UI.
        public int DropBufferCapacity
        {
            get { return QueueLengthLimit; }
            set { QueueLengthLimit = value; }
        }

        public OverflowPolicy OverflowPolicy
        {
            get { return (OverflowPolicy)Volatile.Read(ref _overflowPolicy); }
            set { Volatile.Write(ref _overflowPolicy, (int)value); }
        }

        public void Start(MidiSong song, IMidiOutput output, ProcessingMode mode)
        {
            Start(song, output, mode, 0);
        }

        public void Start(MidiSong song, IMidiOutput output, ProcessingMode mode, long startMicroseconds)
        {
            Start(song, output, mode, startMicroseconds, false);
        }

        public void Start(MidiSong song, IMidiOutput output, ProcessingMode mode, long startMicroseconds, bool startPaused)
        {
            if (song == null) throw new ArgumentNullException("song");
            if (output == null) throw new ArgumentNullException("output");
            bool virtualForward = mode == ProcessingMode.Drop &&
                Volatile.Read(ref _simulateSlowdown) == 0 &&
                Volatile.Read(ref _applyQueueLimitWithoutSlowdown) != 0;
            OverflowPolicy startPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
            bool ageLimited = mode == ProcessingMode.Drop && Volatile.Read(ref _queueAgeLimitEnabled) != 0;
            if (ageLimited && virtualForward)
                throw new InvalidOperationException("Queue waiting-time limits require a retained delayed queue and cannot be combined with forward-only admission.");
            if (ageLimited && startPolicy != OverflowPolicy.DropOldest &&
                startPolicy != OverflowPolicy.DropOldestCompleteNote &&
                startPolicy != OverflowPolicy.ClearBufferAndCatchUp)
                throw new InvalidOperationException("Choose Drop oldest, Drop oldest complete note, or Clear buffer for a queue waiting-time limit.");
            if (virtualForward && startPolicy != OverflowPolicy.DropNewest &&
                startPolicy != OverflowPolicy.DropIncomingCompleteNotes)
                throw new InvalidOperationException("This overflow policy can remove MIDI that was already sent. Turn on Simulate slowdown, or choose Drop newest or Drop incoming complete notes.");
            Stop();
            IMidiOutputContext outputContext = output as IMidiOutputContext;
            if (outputContext != null) outputContext.SourceFile = song.FilePath;
            startMicroseconds = ClampPosition(song, startMicroseconds);
            lock (_sync)
            {
                _song = song;
                _events = song.GetEventReader();
                _output = output;
                _mode = mode;
                _virtualForwardDropActive = virtualForward;
                SourceDispatchTrace sourceTrace = _sourceDispatchTrace;
                _sourceDispatchTraceSession = sourceTrace == null ? 0 : sourceTrace.BeginSession();
                TraceSourceBoundary("Start", "position=" + startMicroseconds);
                _startEventIndex = FindFirstEventAtOrAfter(song, startMicroseconds);
                _pendingStateChaseEventIndex = ChaseMidiStateOnPlaySeek && _startEventIndex > 0
                    ? _startEventIndex : -1;
                _hasPublishedQueue = false;
                _hasPublishedStructuralDueWork = false;
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 0);
                _transportBaseTicks = MicrosecondsToTicks(startMicroseconds);
                _runStartStamp = Stopwatch.GetTimestamp();
                _outputPacedHoldDepth = 0;
                _outputPacedHoldStartStamp = 0;
                _outputPacedHoldTransportTicks = 0;
                ResetStatisticsLocked();
                if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                {
                    _channelState.ResetAllDirect();
                    ChannelPlaybackSnapshot channelSnapshot = _channelState.CreateSnapshot();
                    _channelOverrides.ApplyToSnapshot(channelSnapshot);
                    _channelRouting.ApplyToSnapshot(channelSnapshot);
                    _publishedChannelState = channelSnapshot;
                }
                _channelOverrides.MarkAllForcedPending();
                _lastDispatchedMicroseconds = startMicroseconds;
                _effectiveSpeedFrontierMicroseconds = startMicroseconds;
                _state = startPaused ? PlaybackState.Paused : PlaybackState.Playing;
                Volatile.Write(ref _dispatchSuspended, startPaused ? 1 : 0);
                _silenceWhenWorkerExits = false;
                _workerExitSilenceFailure = null;
                _thread = new Thread(PlaybackWorker);
                _thread.Name = "MIDI bottleneck scheduler";
                _thread.IsBackground = true;
                _thread.Priority = ThreadPriority.AboveNormal;
                _thread.Start();
            }
        }

        public void Seek(long targetMicroseconds)
        {
            TraceSourceBoundary("SeekRequested", "target=" + targetMicroseconds);
            MidiSong song;
            PlaybackState previousState;

            lock (_sync)
            {
                song = _song;
                if (song == null) return;
                targetMicroseconds = ClampPosition(song, targetMicroseconds);
                previousState = _state;
                if (PreservePlaybackQueueOnPauseSeek &&
                    (previousState == PlaybackState.Playing || previousState == PlaybackState.Paused) &&
                    _thread != null && _thread.IsAlive)
                {
                    if (_preservedSeekRequests.Count >= MaximumPendingPreservedSeeks)
                        throw new InvalidOperationException("Too many playback-preserving Seek requests are pending. Resume or wait for the retained queue to advance before seeking again.");
                    int generation = ++_preservedSeekGeneration;
                    _preservedSeekRequests.Enqueue(new PreservedSeekRequest(generation,
                        targetMicroseconds, FindFirstEventAtOrAfter(song, targetMicroseconds)));
                    _intendedTimelineOverrideMicroseconds = targetMicroseconds;
                    SignalWake();
                    return;
                }
            }

            StopWorkerForTransition("seeking", ChannelTransitionBoundary.Seek);

            lock (_sync)
            {
                _transportBaseTicks = MicrosecondsToTicks(targetMicroseconds);
                _startEventIndex = FindFirstEventAtOrAfter(song, targetMicroseconds);
                _pendingStateChaseEventIndex = ChaseMidiStateOnPlaySeek && _startEventIndex > 0
                    ? _startEventIndex : -1;
                _hasPublishedQueue = false;
                _hasPublishedStructuralDueWork = false;
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 0);
                ResetStatisticsLocked();
                _lastDispatchedMicroseconds = targetMicroseconds;
                _effectiveSpeedFrontierMicroseconds = targetMicroseconds;
                _intendedTimelineOverrideMicroseconds = -1;
                _preservedSeekRequests.Clear();
                _thread = null;

                if (previousState == PlaybackState.Playing || previousState == PlaybackState.Paused)
                {
                    _state = previousState;
                    Volatile.Write(ref _dispatchSuspended, _state == PlaybackState.Playing ? 0 : 1);
                    if (_state == PlaybackState.Playing)
                        _runStartStamp = Stopwatch.GetTimestamp();
                    _thread = new Thread(PlaybackWorker);
                    _thread.Name = "MIDI bottleneck scheduler";
                    _thread.IsBackground = true;
                    _thread.Priority = ThreadPriority.AboveNormal;
                    _thread.Start();
                }
                else
                {
                    _state = PlaybackState.Stopped;
                    Volatile.Write(ref _dispatchSuspended, 1);
                }
            }
            SignalWake();
        }

        public void Pause()
        {
            MidiSong song;
            int preservingGeneration = 0;
            lock (_sync)
            {
                if (_state != PlaybackState.Playing) return;
                song = _song;
                if (PreservePlaybackQueueOnPauseSeek && _thread != null && _thread.IsAlive)
                {
                    FreezeTransportLocked();
                    _state = PlaybackState.Paused;
                    Volatile.Write(ref _dispatchSuspended, 1);
                    preservingGeneration = ++_pauseRequestGeneration;
                    _pauseAcknowledged.Reset();
                }
            }

            if (preservingGeneration != 0)
            {
                SignalWake();
                int timeout = Volatile.Read(ref _workerStopTimeoutMilliseconds);
                if (_pauseAcknowledged.WaitOne(timeout)) return;
                StopWorkerForTransition("acknowledging playback-preserving Pause",
                    ChannelTransitionBoundary.Pause);
                throw new PlaybackWorkerTimeoutException("The MIDI output call did not return within " + timeout +
                    " ms while pausing with queue preservation. Playback was stopped and no replacement scheduler was started.", null);
            }

            // Pause is a native-output boundary, not merely a scheduler flag.
            // Retire the worker first so Reset/Panic cannot race an active Send
            // and provider-buffered messages cannot arrive after the panic.
            StopWorkerForTransition("pausing playback", ChannelTransitionBoundary.Pause);

            lock (_sync)
            {
                if (song == null || _song != song) return;
                long pausedMicroseconds = ClampPosition(song, TicksToMicroseconds(_transportBaseTicks));
                _transportBaseTicks = MicrosecondsToTicks(pausedMicroseconds);
                _startEventIndex = FindFirstEventAtOrAfter(song, pausedMicroseconds);
                _pendingStateChaseEventIndex = ChaseMidiStateOnPlaySeek && _startEventIndex > 0
                    ? _startEventIndex : -1;
                _effectiveSpeedFrontierMicroseconds = pausedMicroseconds;
                _hasPublishedQueue = false;
                _hasPublishedStructuralDueWork = false;
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 0);
                _state = PlaybackState.Paused;
                Volatile.Write(ref _dispatchSuspended, 1);
                _silenceWhenWorkerExits = false;
                _workerExitSilenceFailure = null;
                _thread = new Thread(PlaybackWorker);
                _thread.Name = "MIDI bottleneck scheduler";
                _thread.IsBackground = true;
                _thread.Priority = ThreadPriority.AboveNormal;
                _thread.Start();
            }
            SignalWake();
        }

        public void Resume()
        {
            lock (_sync)
            {
                if (_state != PlaybackState.Paused) return;
                _runStartStamp = Stopwatch.GetTimestamp();
                _state = PlaybackState.Playing;
                Volatile.Write(ref _dispatchSuspended, 0);
            }
            SignalWake();
        }

        public void Stop()
        {
            StopWorkerForTransition("stopping playback", ChannelTransitionBoundary.Stop);
        }

        private enum ChannelTransitionBoundary
        {
            Pause,
            Seek,
            Stop
        }

        private void StopWorkerForTransition(string operation, ChannelTransitionBoundary channelBoundary)
        {
            TraceSourceBoundary(channelBoundary.ToString(), operation);
            Thread thread;
            IMidiOutput output;
            lock (_sync)
            {
                if (_state == PlaybackState.Playing)
                    FreezeTransportLocked();
                _state = PlaybackState.Stopped;
                Volatile.Write(ref _dispatchSuspended, 1);
                thread = _thread;
                output = _output;
                _workerExitSilenceFailure = null;
                _silenceWhenWorkerExits = thread != null;
                _intendedTimelineOverrideMicroseconds = -1;
                _preservedSeekRequests.Clear();
            }
            SignalWake();

            if (thread == null)
            {
                if (output != null)
                {
                    TraceSourceBoundary("ResetAndPanic", operation + "; no-worker");
                    MidiOutputSafety.ResetAndSilence(output);
                }
            }
            else if (thread != Thread.CurrentThread)
            {
                int timeout = Volatile.Read(ref _workerStopTimeoutMilliseconds);
                if (!thread.Join(timeout))
                {
                    lock (_sync)
                    {
                        _queueLength = 0;
                        _outstandingEvents = 0;
                        _currentLagMicroseconds = 0;
                    }
                    throw new PlaybackWorkerTimeoutException("The MIDI output call did not return within " + timeout +
                        " ms while " + operation + ". Playback remains stopped and no replacement scheduler was started. " +
                        "The output is not reset concurrently with its active call; a final reset and all-notes-off panic " +
                        "will run after that call returns.", null);
                }

                Exception exitSilenceFailure;
                lock (_sync)
                {
                    exitSilenceFailure = _workerExitSilenceFailure;
                    _workerExitSilenceFailure = null;
                }
                if (exitSilenceFailure != null)
                    throw new InvalidOperationException("The scheduler stopped, but MIDI reset/all-notes-off failed while " +
                        operation + ".", exitSilenceFailure);
            }

            List<ChannelControlRequest> retiredControls;
            lock (_sync)
            {
                _queueLength = 0;
                _outstandingEvents = 0;
                _currentLagMicroseconds = 0;
                if (channelBoundary == ChannelTransitionBoundary.Stop)
                    _effectiveSpeedFrontierMicroseconds = 0;
                retiredControls = DrainChannelControlRequestsLocked();
            }
            CompleteRetiredChannelControls(retiredControls,
                new OperationCanceledException("The channel-control request was retired by the " + operation + " boundary."));
            _channelOverrides.MarkAllForcedPending();
            if (channelBoundary == ChannelTransitionBoundary.Stop)
            {
                _channelOverrides.ResetStatistics();
                _channelRouting.ResetStatistics();
            }
            if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
            {
                if (channelBoundary == ChannelTransitionBoundary.Pause) _channelState.PauseBoundaryDirect();
                else if (channelBoundary == ChannelTransitionBoundary.Seek) _channelState.SeekBoundaryDirect();
                else _channelState.StopBoundaryDirect();
                PublishChannelState(true);
            }
        }

        internal int WorkerStopTimeoutMilliseconds
        {
            get { return Volatile.Read(ref _workerStopTimeoutMilliseconds); }
            set { Volatile.Write(ref _workerStopTimeoutMilliseconds, Math.Max(1, value)); }
        }

        internal bool HasLiveWorker
        {
            get { lock (_sync) return _thread != null && _thread.IsAlive; }
        }

        public void Unload()
        {
            Stop();
            lock (_sync)
            {
                if (_thread != null && _thread.IsAlive)
                    throw new InvalidOperationException("The MIDI scheduler did not stop in time, so its song cannot yet be unloaded safely.");
                IMidiOutputContext outputContext = _output as IMidiOutputContext;
                if (outputContext != null) outputContext.SourceFile = null;
                _song = null;
                _events = default(MidiEventReader);
                _output = null;
                _startEventIndex = 0;
                _pendingStateChaseEventIndex = -1;
                _intendedTimelineOverrideMicroseconds = -1;
                _preservedSeekRequests.Clear();
                _lastDispatchedMicroseconds = 0;
                _effectiveSpeedFrontierMicroseconds = 0;
                _transportBaseTicks = 0;
                ResetStatisticsLocked();
                _channelOverrides.Clear();
                _channelRouting.Clear();
                _channelControlRequests.Clear();
                if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                {
                    _channelState.ResetAllDirect();
                    ChannelPlaybackSnapshot channelSnapshot = _channelState.CreateSnapshot();
                    _channelOverrides.ApplyToSnapshot(channelSnapshot);
                    _channelRouting.ApplyToSnapshot(channelSnapshot);
                    _publishedChannelState = channelSnapshot;
                }
            }
        }

        internal bool HasAttachedSong
        {
            get { lock (_sync) return _song != null; }
        }

        internal bool HasAttachedOutput
        {
            get { lock (_sync) return _output != null; }
        }

        public void ResetStatistics()
        {
            lock (_sync)
            {
                _maximumQueueLength = _outstandingEvents;
                _maximumQueueAgeMicroseconds = _queueAgeMicroseconds;
                _processedEvents = 0;
                _droppedEvents = 0;
                _gateFilteredEvents = 0;
                _musicalLimitRejectedAttacks = 0;
                _musicalLimitSuppressedReleases = 0;
                _outputRateRejectedAttacks = 0;
                _outputRateSuppressedReleases = 0;
                _outputRateRejectedOtherMessages = 0;
                _outputRateSafetySoftExcess = 0;
                _outputRateSafetyBypassMessages = 0;
                _virtualMaximumQueueLength = _virtualOutstandingEvents;
                _maximumLagMicroseconds = _currentLagMicroseconds;
            }
            _channelOverrides.ResetStatistics();
            _channelRouting.ResetStatistics();
            if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
            {
                _channelState.RequestStatisticsReset();
                ChannelPlaybackSnapshot published = _publishedChannelState;
                if (published != null)
                {
                    ChannelPlaybackSnapshot reset = published.WithStatisticsReset();
                    _channelOverrides.ApplyToSnapshot(reset);
                    _channelRouting.ApplyToSnapshot(reset);
                    _publishedChannelState = reset;
                }
            }
        }

        internal void SetChannelMonitoring(bool enabled)
        {
            Volatile.Write(ref _channelMonitoringEnabled, enabled ? 1 : 0);
            if (enabled)
            {
                _channelState.RequestFullReset();
                ChannelPlaybackSnapshot snapshot = ChannelPlaybackSnapshot.Empty();
                _channelOverrides.ApplyToSnapshot(snapshot);
                _channelRouting.ApplyToSnapshot(snapshot);
                _publishedChannelState = snapshot;
                Interlocked.Exchange(ref _lastChannelPublishStamp, 0);
            }
            else _publishedChannelState = null;
        }

        // Opening the optional monitor can request a fresh, ordered source-state
        // chase. The monitor remains historical until these sends succeed.
        internal bool ChaseStateForNewMonitor(Action<Exception> completion)
        {
            MidiSong song;
            lock (_sync)
            {
                song = _song;
                if (_thread == null || song == null || _output == null ||
                    Volatile.Read(ref _chaseMidiStateOnPlaySeek) == 0) return false;
                _channelControlRequests.Enqueue(new ChannelControlRequest(song, completion));
            }
            SignalWake();
            return true;
        }

        internal ChannelPlaybackSnapshot GetChannelSnapshot()
        {
            return _publishedChannelState;
        }

        internal int GetChannelOverride(int channel, ChannelAttribute attribute)
        {
            return _channelOverrides.GetValue(channel, attribute);
        }

        internal void SetChannelOverride(int channel, ChannelAttribute attribute, int value)
        {
            if (!_channelOverrides.SetValue(channel, attribute, value)) return;
            if (value == ChannelOverrideState.AutoValue)
            {
                if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                    _channelState.MarkAttributeHistoricalDirect(channel, attribute);
                PublishChannelState(true);
                return;
            }

            bool sentDirectly = false;
            try
            {
                lock (_sync)
                {
                    if (_thread == null && _output != null && _channelRouting.IsEnabled(channel))
                    {
                        MidiEvent message = ChannelOverrideState.CreateMessage(channel, attribute, value);
                        SendProtectedEvent(message);
                        RecordGeneratedSourceTrace(message, "ForcedOverride", attribute.ToString());
                        if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                        {
                            _channelState.RecordOverrideApplied(channel, attribute, value);
                            if (_channelOverrides.GetValue(channel, attribute) != value)
                                _channelState.MarkAttributeHistoricalDirect(channel, attribute);
                        }
                        // Consume the pending bit installed by SetValue. No worker
                        // can start while the lifecycle lock is held.
                        int pending = _channelOverrides.TakePendingMask(channel);
                        _channelOverrides.Requeue(channel, pending & ~(1 << (int)attribute));
                        sentDirectly = true;
                    }
                }
            }
            catch
            {
                PublishChannelState(true);
                throw;
            }
            if (!sentDirectly) SignalWake();
            PublishChannelState(true);
        }

        internal bool IsChannelEnabled(int channel) { return _channelRouting.IsEnabled(channel); }

        internal void SetChannelEnabled(int channel, bool enabled)
        {
            SubmitChannelControl(new ChannelControlRequest(ChannelControlKind.SetEnabled, channel,
                ChannelAttribute.BankMsb, enabled ? 1 : 0));
        }

        internal void ChaseLatestSourceChannelAttribute(int channel, ChannelAttribute attribute)
        {
            ChaseLatestSourceChannelAttribute(channel, attribute, null);
        }

        internal void ChaseLatestSourceChannelAttribute(int channel, ChannelAttribute attribute, Action<Exception> completion)
        {
            if (!_channelRouting.IsEnabled(channel))
            {
                InvalidOperationException error = new InvalidOperationException("Enable this MIDI channel before restoring its source value.");
                if (completion != null) { completion(error); return; }
                throw error;
            }
            MidiSong song;
            long positionMicroseconds;
            lock (_sync)
            {
                song = _song;
                positionMicroseconds = song == null ? 0 : TicksToMicroseconds(CurrentTransportTicksLocked());
            }
            int value;
            if (song == null || !song.GetChannelSourceValueIndex().TryGetLatest(channel, attribute,
                ClampPosition(song, positionMicroseconds), out value))
            {
                InvalidOperationException error = new InvalidOperationException(
                    "No source MIDI value for this channel attribute exists at the current position.");
                if (completion != null) { completion(error); return; }
                throw error;
            }
            SubmitChannelControl(new ChannelControlRequest(ChannelControlKind.Chase, channel, attribute, value, completion));
        }

        // Ordered explicit control primitive used by deterministic adapter tests.
        // User-facing historical chase resolves through the immutable source
        // index above and never supplies the gray historical value here.
        internal void SendExplicitChannelAttribute(int channel, ChannelAttribute attribute, int value)
        {
            SubmitChannelControl(new ChannelControlRequest(ChannelControlKind.Chase, channel, attribute, value));
        }

        internal void SendExplicitChannelAttribute(int channel, ChannelAttribute attribute, int value,
            Action<Exception> completion)
        {
            SubmitChannelControl(new ChannelControlRequest(ChannelControlKind.Chase, channel, attribute, value, completion));
        }

        private void SubmitChannelControl(ChannelControlRequest request)
        {
            bool queued;
            Exception immediateError = null;
            lock (_sync)
            {
                queued = _thread != null;
                if (queued) _channelControlRequests.Enqueue(request);
                else
                {
                    try
                    {
                        ExecuteChannelControl(request);
                        if (request.Kind == ChannelControlKind.SetEnabled && request.Value != 0)
                            ApplyPendingOverrides();
                    }
                    catch (Exception ex) { immediateError = ex; }
                }
            }
            if (queued)
            {
                // Nothing is observable until the worker reaches the ordered
                // boundary. Publishing here can race that later publication
                // and overwrite it with the pre-control snapshot.
                SignalWake();
                return;
            }
            // Completion is an acknowledgement of both the direct output
            // operation and its observable monitor state. Publish first so a
            // callback cannot read the pre-control snapshot.
            PublishChannelState(true);
            request.Complete(immediateError);
            if (immediateError != null && !request.HasCompletion) throw immediateError;
        }

        private void ApplyPendingChannelControls()
        {
            while (true)
            {
                ChannelControlRequest request;
                lock (_sync)
                {
                    if (_channelControlRequests.Count == 0) return;
                    request = _channelControlRequests.Dequeue();
                }
                Exception error = null;
                try { ExecuteChannelControl(request); }
                catch (Exception ex)
                {
                    error = ex;
                    EventHandler<ChannelControlErrorEventArgs> handler = ChannelControlFailed;
                    if (handler != null) handler(this, new ChannelControlErrorEventArgs(request.Channel, request.Attribute, ex));
                }
                PublishChannelState(true);
                // The callback may immediately refresh Channels. It must never
                // outrun the snapshot representing this exact control result.
                request.Complete(error);
            }
        }

        private List<ChannelControlRequest> DrainChannelControlRequestsLocked()
        {
            if (_channelControlRequests.Count == 0) return null;
            List<ChannelControlRequest> requests = new List<ChannelControlRequest>(_channelControlRequests.Count);
            while (_channelControlRequests.Count > 0) requests.Add(_channelControlRequests.Dequeue());
            return requests;
        }

        private static void CompleteRetiredChannelControls(List<ChannelControlRequest> requests, Exception error)
        {
            if (requests == null) return;
            for (int i = 0; i < requests.Count; i++) requests[i].Complete(error);
        }

        private void ExecuteChannelControl(ChannelControlRequest request)
        {
            if (request.Kind == ChannelControlKind.MonitorStateChase)
            {
                MidiSong song;
                IMidiOutput output;
                long position;
                lock (_sync)
                {
                    song = _song;
                    output = _output;
                    position = TicksToMicroseconds(CurrentTransportTicksLocked());
                }
                if (!Object.ReferenceEquals(song, request.ExpectedSong) || output == null)
                    throw new InvalidOperationException("The MIDI file or output changed before Channels could refresh.");
                if (Volatile.Read(ref _chaseMidiStateOnPlaySeek) == 0 ||
                    Volatile.Read(ref _channelMonitoringEnabled) == 0) return;
                int exclusive = song.EventStore.LowerBoundByTime(ClampPosition(song, position));
                IList<MidiEvent> messages = song.GetMidiStateChaseIndex().CreateMessages(exclusive,
                    _channelRouting, _channelOverrides);
                for (int i = 0; i < messages.Count; i++)
                {
                    if (!IsActive() || Volatile.Read(ref _channelMonitoringEnabled) == 0)
                        throw new InvalidOperationException("Playback changed before Channels could refresh.");
                    MidiEventView message = MidiEventView.FromEvent(messages[i], -1);
                    SendRateCountedEssentialEvent(output, message);
                    if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                        _channelState.RecordMonitorStateChaseApplied(message);
                }
                return;
            }
            if (request.Kind == ChannelControlKind.SetEnabled)
            {
                bool enabled = request.Value != 0;
                if (enabled)
                {
                    if (_channelRouting.SetEnabled(request.Channel, true))
                        _channelOverrides.MarkChannelForcedPending(request.Channel);
                    return;
                }
                if (!_channelRouting.IsEnabled(request.Channel)) return;
                if (_output != null)
                {
                    SendProtectedEvent(ChannelOverrideState.CreateControllerMessage(request.Channel, 64, 0));
                    SendProtectedEvent(ChannelOverrideState.CreateControllerMessage(request.Channel, 120, 0));
                    SendProtectedEvent(ChannelOverrideState.CreateControllerMessage(request.Channel, 123, 0));
                }
                _channelRouting.SetEnabled(request.Channel, false);
                if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                    _channelState.SilenceChannelDirect(request.Channel);
                return;
            }
            if (_output == null) throw new InvalidOperationException("No MIDI output session is available.");
            if (!_channelRouting.IsEnabled(request.Channel))
                throw new InvalidOperationException("Enable this MIDI channel before restoring its source value.");
            SendRateCountedEssentialEvent(_output,
                ChannelOverrideState.CreateMessage(request.Channel, request.Attribute, request.Value));
            if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                _channelState.RecordManualChaseApplied(request.Channel, request.Attribute, request.Value);
        }

        internal void SetDropTrace(DropTraceRecorder recorder)
        {
            lock (_sync) _dropTrace = recorder;
        }

        internal void SetSourceDispatchTrace(SourceDispatchTrace trace)
        {
            lock (_sync) _sourceDispatchTrace = trace;
        }

        internal void SetGateBoundaryDispatchHookForTesting(Action<long> hook)
        {
            lock (_sync) _gateBoundaryDispatchHookForTesting = hook;
        }

        internal void SetGateLogicalOutputTraceForTesting(
            Action<long, IList<MidiEventView>, int> trace)
        {
            lock (_sync) _gateLogicalOutputTraceForTesting = trace;
        }

        private void TraceGateLogicalOutputForTesting(long logicalMicroseconds,
            IList<MidiEventView> events, int count)
        {
            Action<long, IList<MidiEventView>, int> trace = _gateLogicalOutputTraceForTesting;
            if (trace != null && count > 0) trace(logicalMicroseconds, events, count);
        }

        private void TraceGateLogicalOutputForTesting(long logicalMicroseconds,
            MidiEventView midiEvent)
        {
            Action<long, IList<MidiEventView>, int> trace = _gateLogicalOutputTraceForTesting;
            if (trace == null) return;
            trace(logicalMicroseconds, new MidiEventView[] { midiEvent }, 1);
        }

        public PlaybackSnapshot GetSnapshot()
        {
            lock (_sync)
            {
                long playbackTicks = CurrentTransportTicksLocked();
                // Source arrivals continue while an output call is blocked.
                // This is the application's unlimited backlog, not the synth's queue.
                if (_mode == ProcessingMode.Queue && _hasPublishedQueue &&
                    (_state == PlaybackState.Playing || _state == PlaybackState.Paused))
                {
                    MeasurePublishedDueWork(_publishedNextProcess, 0, -1, Int64.MaxValue,
                        _publishedInService, PublishedInServiceDueMicroseconds(), Int64.MaxValue, 0,
                        playbackTicks);
                }
                else if (_mode == ProcessingMode.Drop && _hasPublishedQueue &&
                    (_state == PlaybackState.Playing || _state == PlaybackState.Paused))
                {
                    MeasurePublishedDueWork(_publishedDropNextArrival,
                        _publishedDropPending, _publishedOldestPendingEvent, Int64.MaxValue,
                        _publishedInService, PublishedInServiceDueMicroseconds(), Int64.MaxValue, 0,
                        playbackTicks);
                }
                else if (_hasPublishedStructuralDueWork &&
                    (_state == PlaybackState.Playing || _state == PlaybackState.Paused))
                {
                    MeasurePublishedDueWork(_publishedStructuralNextArrival,
                        _publishedStructuralPending, -1,
                        _publishedStructuralOldestDueMicroseconds,
                        _publishedStructuralInService,
                        _publishedStructuralInServiceDueMicroseconds,
                        _publishedStructuralFutureDueMicroseconds,
                        _publishedStructuralFutureCount, playbackTicks);
                }
                long playbackUs = TicksToMicroseconds(playbackTicks);
                PlaybackSnapshot snapshot = new PlaybackSnapshot();
                snapshot.State = _state;
                ServiceDurationSettings settings = Volatile.Read(ref _serviceSettings);
                snapshot.ProcessingMicroseconds = settings.ProcessingMicroseconds;
                snapshot.ServiceDurationMode = settings.Mode;
                snapshot.MidiBitrate = settings.MidiBitrate;
                snapshot.EventsPerSecond = settings.EventsPerSecond;
                snapshot.SimulateSlowdown = Volatile.Read(ref _simulateSlowdown) != 0;
                snapshot.QueueLengthLimitEnabled = _mode == ProcessingMode.Drop;
                snapshot.ProcessingMode = _mode;
                snapshot.QueueLengthLimit = Volatile.Read(ref _queueLengthLimit);
                snapshot.QueueAgeLimitEnabled = Volatile.Read(ref _queueAgeLimitEnabled) != 0;
                snapshot.QueueAgeLimitMicroseconds = Interlocked.Read(ref _queueAgeLimitMicroseconds);
                snapshot.QueueAgeMicroseconds = _queueAgeMicroseconds;
                snapshot.MaximumQueueAgeMicroseconds = _maximumQueueAgeMicroseconds;
                snapshot.OverflowPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                snapshot.QueueLength = _queueLength;
                snapshot.OutstandingEvents = _outstandingEvents;
                snapshot.MaximumQueueLength = _maximumQueueLength;
                snapshot.ProcessedEvents = _processedEvents;
                snapshot.DroppedEvents = _droppedEvents;
                snapshot.GateFilteredEvents = _gateFilteredEvents;
                snapshot.MusicalLimitRejectedAttacks = _musicalLimitRejectedAttacks;
                snapshot.MusicalLimitSuppressedReleases = _musicalLimitSuppressedReleases;
                snapshot.OutputRateRejectedAttacks = _outputRateRejectedAttacks;
                snapshot.OutputRateSuppressedReleases = _outputRateSuppressedReleases;
                snapshot.OutputRateRejectedOtherMessages = _outputRateRejectedOtherMessages;
                snapshot.OutputRateSafetySoftExcess = _outputRateSafetySoftExcess;
                snapshot.OutputRateSafetyBypassMessages = _outputRateSafetyBypassMessages;
                snapshot.VirtualQueueActive = _virtualForwardDropActive;
                snapshot.VirtualQueueLength = _virtualQueueLength;
                snapshot.VirtualOutstandingEvents = _virtualOutstandingEvents;
                snapshot.VirtualMaximumQueueLength = _virtualMaximumQueueLength;
                snapshot.PlaybackMicroseconds = playbackUs;
                long intended = _intendedTimelineOverrideMicroseconds >= 0
                    ? _intendedTimelineOverrideMicroseconds : playbackUs;
                snapshot.IntendedTimelineMicroseconds = _song == null ? 0 : Math.Min(intended, _song.DurationMicroseconds);
                snapshot.LastDispatchedTimelineMicroseconds = _lastDispatchedMicroseconds;
                snapshot.EffectiveSpeedFrontierMicroseconds = _mode == ProcessingMode.PerNoteIntervalGate ||
                    _mode == ProcessingMode.DelayedNoteOff ||
                    _mode == ProcessingMode.OutputRateLimit
                    ? _effectiveSpeedFrontierMicroseconds : _lastDispatchedMicroseconds;
                snapshot.CurrentLagMicroseconds = _currentLagMicroseconds;
                snapshot.MaximumLagMicroseconds = _maximumLagMicroseconds;
                return snapshot;
            }
        }

        private void PlaybackWorker()
        {
            Exception failure = null;
            bool completed = false;
            try
            {
                using (HighResolutionWaiter waiter = new HighResolutionWaiter(_wake))
                {
                    if (PreservePlaybackQueueOnPauseSeek &&
                        (_mode == ProcessingMode.Queue || _mode == ProcessingMode.Drop ||
                        _mode == ProcessingMode.PerNoteIntervalGate ||
                        _mode == ProcessingMode.DelayedNoteOff ||
                        _mode == ProcessingMode.NoteAttackLimit ||
                        _mode == ProcessingMode.OutputRateLimit))
                    {
                        if (_mode == ProcessingMode.Queue)
                            RunOrderedUnlimitedQueueMode(waiter);
                        else if (!_virtualForwardDropActive)
                        {
                            if (_mode == ProcessingMode.Drop)
                                RunOrderedFiniteQueueMode(waiter);
                            else if (_mode == ProcessingMode.PerNoteIntervalGate ||
                                _mode == ProcessingMode.DelayedNoteOff)
                                RunOrderedMusicalGateMode(waiter);
                            else if (_mode == ProcessingMode.OutputRateLimit)
                                RunOrderedOutputRateMode(waiter);
                            else
                                RunOrderedNoteAttackMode(waiter);
                        }
                        else
                            RunOrderedVirtualForwardMode(waiter);
                    }
                    else
                    {
                        if (_mode == ProcessingMode.Queue)
                            RunQueueMode(waiter);
                        else if (_mode == ProcessingMode.Drop)
                            RunDropMode(waiter);
                        else if (_mode == ProcessingMode.PerNoteIntervalGate)
                            RunPerNoteIntervalGate(waiter);
                        else if (_mode == ProcessingMode.DelayedNoteOff)
                            RunDelayedNoteOff(waiter);
                        else if (_mode == ProcessingMode.OutputRateLimit)
                            RunOutputRateLimit(waiter);
                        else
                            RunNoteAttackLimit(waiter);
                    }
                }
                lock (_sync)
                    completed = _state == PlaybackState.Playing && _song != null;
            }
            catch (Exception ex)
            {
                failure = ex;
                try
                {
                    if (_output != null)
                    {
                        TraceSourceBoundary("Panic", "worker-failure: " + ex.GetType().Name);
                        _output.Panic();
                    }
                }
                catch { }
                if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                {
                    _channelState.PanicDirect();
                    PublishChannelState(true);
                }
            }

            IMidiOutput finalSilenceOutput = null;
            lock (_sync)
            {
                if (_thread == Thread.CurrentThread && _silenceWhenWorkerExits)
                {
                    finalSilenceOutput = _output;
                    _silenceWhenWorkerExits = false;
                }
            }
            Exception finalSilenceFailure = null;
            if (finalSilenceOutput != null)
            {
                try
                {
                    TraceSourceBoundary("ResetAndPanic", "worker-exit");
                    MidiOutputSafety.ResetAndSilence(finalSilenceOutput);
                }
                catch (Exception ex) { finalSilenceFailure = ex; }
            }

            List<ChannelControlRequest> abandonedControls;
            lock (_sync)
            {
                if (_thread != Thread.CurrentThread) return;
                if (finalSilenceFailure != null) _workerExitSilenceFailure = finalSilenceFailure;
                if (failure != null || completed)
                {
                    if (_state == PlaybackState.Playing)
                        FreezeTransportLocked();
                    _state = failure == null ? PlaybackState.Completed : PlaybackState.Stopped;
                    _preservedSeekRequests.Clear();
                    _intendedTimelineOverrideMicroseconds = -1;
                }
                _thread = null;
                _queueLength = 0;
                _outstandingEvents = 0;
                _currentLagMicroseconds = 0;
                abandonedControls = DrainChannelControlRequestsLocked();
            }
            PublishChannelState(true);
            CompleteRetiredChannelControls(abandonedControls, failure ??
                new OperationCanceledException("Playback ended before the channel-control request reached the output boundary."));

            if (failure != null)
            {
                EventHandler<PlaybackErrorEventArgs> failed = PlaybackFailed;
                if (failed != null) failed(this, new PlaybackErrorEventArgs(failure));
            }
            else if (completed)
            {
                EventHandler ended = PlaybackEnded;
                if (ended != null) ended(this, EventArgs.Empty);
            }
        }

        private bool HasPreservedSeekRequest()
        {
            lock (_sync) return _preservedSeekRequests.Count != 0;
        }

        private void DispatchProtectedAllNotesOff()
        {
            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
            for (int channel = 0; channel < 16; channel++)
            {
                if (!IsActive()) return;
                MidiEvent message = ChannelOverrideState.CreateControllerMessage(channel, 123, 0);
                MidiEventView view = MidiEventView.FromEvent(message, -1);
                SendProtectedEvent(view);
                if (trackChannels) _channelState.RecordSuccessful(view);
            }
            if (trackChannels) PublishChannelState(true);
        }

        private void RunQueueMode(HighResolutionWaiter waiter)
        {
            ServiceDurationSettings serviceSettings = Volatile.Read(ref _serviceSettings);
            ServiceDurationClock serviceClock = serviceSettings.CreateClock();
            ServiceDurationClock beforeCurrentService = serviceClock;
            int nextArrival = _startEventIndex;
            int nextProcess = _startEventIndex;
            int inService = -1;
            long completionTicks = 0;
            long completionStamp = Int64.MaxValue;
            bool serviceHold = false;
            long lastCompletionTicks = 0;
            long eligiblePending = 0;
            FilteredSourceEvents filtered = new FilteredSourceEvents();
            int routingGeneration = _channelRouting.FilterGeneration;
            int overrideGeneration = _channelOverrides.FilterGeneration;
            if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                bool seekPending = HasPreservedSeekRequest();
                if (!seekPending) ApplyPendingStateChase();
                ApplyPendingOverrides();
                long now = CurrentTransportTicks();

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                int currentOverrideGeneration = _channelOverrides.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration)
                {
                    if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                    RetireUnlimitedFilteredBacklog(nextProcess, nextArrival, filtered, ref eligiblePending,
                        ref inService, ref completionTicks, ref lastCompletionTicks, now,
                        ref serviceClock, ref beforeCurrentService);
                    if (inService < 0 && serviceHold)
                    {
                        EndOutputPacedHold(serviceHold);
                        serviceHold = false;
                        completionStamp = Int64.MaxValue;
                    }
                    else if (inService >= 0 && !serviceHold && completionTicks < Int64.MaxValue)
                        StartOutputPacedServiceHold(Math.Max(0, completionTicks - now),
                            ref serviceHold, ref completionStamp);
                    routingGeneration = currentRoutingGeneration;
                    overrideGeneration = currentOverrideGeneration;
                }

                int due = seekPending ? nextArrival : FindFirstEventAfterTransport(nextArrival, now);
                AdmitUnlimitedRange(nextArrival, due, filtered, ref eligiblePending);
                nextArrival = due;
                PublishUnlimitedQueue(nextProcess, eligiblePending, inService >= 0);

                if (inService >= 0 && (OutputPacedTimeline
                    ? completionStamp <= Stopwatch.GetTimestamp()
                    : completionTicks <= now))
                {
                    Dispatch(inService, now);
                    EndOutputPacedHold(serviceHold);
                    serviceHold = false;
                    inService = -1;
                    lastCompletionTicks = completionTicks;
                    completionStamp = Int64.MaxValue;
                    continue;
                }

                if (inService < 0 && eligiblePending > 0)
                {
                    UpdateServiceClock(ref serviceClock, ref serviceSettings);
                    if (Volatile.Read(ref _simulateSlowdown) == 0 || serviceClock.IsImmediate)
                    {
                        PublishUnlimitedQueue(nextProcess, Math.Max(0, eligiblePending - 1), true);
                        DispatchImmediateFilteredRange(ref nextProcess, nextArrival, now, filtered, ref eligiblePending,
                            serviceSettings);
                        long afterDispatch = CurrentTransportTicks();
                        due = FindFirstEventAfterTransport(nextArrival, afterDispatch);
                        AdmitUnlimitedRange(nextArrival, due, filtered, ref eligiblePending);
                        nextArrival = due;
                        PublishUnlimitedQueue(nextProcess, eligiblePending, false);
                        continue;
                    }
                    inService = TakeNextEligible(ref nextProcess, nextArrival, filtered);
                    if (inService < 0) { eligiblePending = 0; continue; }
                    eligiblePending--;
                    long startTicks = Math.Max(EventTicks(inService), lastCompletionTicks);
                    UpdateServiceClock(ref serviceClock, ref serviceSettings);
                    beforeCurrentService = serviceClock;
                    long serviceTicks = MicrosecondsToTicks(
                        serviceClock.NextMicroseconds(_events[inService]));
                    completionTicks = checked(startTicks + serviceTicks);
                    serviceHold = BeginOutputPacedHold();
                    completionStamp = serviceHold
                        ? SaturatingAddTicks(Stopwatch.GetTimestamp(), serviceTicks)
                        : Int64.MaxValue;
                    PublishUnlimitedQueue(nextProcess, eligiblePending, true);
                    continue;
                }

                if ((seekPending || nextArrival >= _events.Count) && inService < 0 && eligiblePending == 0)
                    return;

                if (inService >= 0 && serviceHold)
                {
                    PublishChannelStateBeforeWait(CurrentTransportTicks(), CurrentTransportTicks());
                    WaitUntilStopwatch(waiter, completionStamp, iterationWakeGeneration);
                    continue;
                }
                long target = inService >= 0 ? completionTicks : Int64.MaxValue;
                if (nextArrival < _events.Count)
                    target = Math.Min(target, EventTicks(nextArrival));
                if (inService < 0 && eligiblePending == 0)
                    SetCurrentLag(0);
                PublishChannelStateBeforeWait(target, now);
                WaitUntil(waiter, target, iterationWakeGeneration);
            }
        }

        private void RunDropMode(HighResolutionWaiter waiter)
        {
            if (_virtualForwardDropActive)
            {
                RunVirtualForwardDropMode(waiter);
                return;
            }
            ServiceDurationSettings serviceSettings = Volatile.Read(ref _serviceSettings);
            ServiceDurationClock serviceClock = serviceSettings.CreateClock();
            ServiceDurationClock beforeCurrentService = serviceClock;
            int nextArrival = _startEventIndex;
            int inService = -1;
            long completionTicks = Int64.MaxValue;
            long completionStamp = Int64.MaxValue;
            bool serviceHold = false;
            bool ageLimited = Volatile.Read(ref _queueAgeLimitEnabled) != 0;
            PendingMidiQueue pending = new PendingMidiQueue(ageLimited ? Int32.MaxValue : 1,
                (OverflowPolicy)Volatile.Read(ref _overflowPolicy) == OverflowPolicy.DropOldestCompleteNote ||
                (OverflowPolicy)Volatile.Read(ref _overflowPolicy) == OverflowPolicy.DropBackgroundNotes);
            int consecutiveDrops = 0;
            long clusterTimestamp = Int64.MinValue;
            int clusterEnd = nextArrival;
            int clusterSize = 0;
            int maximumBufferOccupancy = 0;
            CompleteNoteTracker completeNotes = new CompleteNoteTracker();
            FilteredSourceEvents filtered = new FilteredSourceEvents();
            int routingGeneration = _channelRouting.FilterGeneration;
            int overrideGeneration = _channelOverrides.FilterGeneration;
            bool admittingExternalBacklog = false;
            long externalBacklogServiceStart = 0;
            int musicalFairnessCursor = 0;
            if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
            bool traceEnabled;
            lock (_sync) traceEnabled = _dropTrace != null;

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                bool seekPending = HasPreservedSeekRequest();
                if (!seekPending) ApplyPendingStateChase();
                ApplyPendingOverrides();
                long now = CurrentTransportTicks();
                PublishDropQueue(nextArrival, pending, inService >= 0, TicksToMicroseconds(now));

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                int currentOverrideGeneration = _channelOverrides.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration)
                {
                    if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                    RetireDropFilteredBacklog(filtered, pending, completeNotes, ref inService, ref completionTicks, now,
                        ref serviceClock, ref beforeCurrentService);
                    if (inService < 0 && serviceHold)
                    {
                        EndOutputPacedHold(serviceHold);
                        serviceHold = false;
                        completionStamp = Int64.MaxValue;
                    }
                    else if (inService >= 0 && !serviceHold && completionTicks < Int64.MaxValue)
                        StartOutputPacedServiceHold(Math.Max(0, completionTicks - now),
                            ref serviceHold, ref completionStamp);
                    routingGeneration = currentRoutingGeneration;
                    overrideGeneration = currentOverrideGeneration;
                }
                long arrivalTicks = !seekPending && nextArrival < _events.Count
                    ? EventTicks(nextArrival) : Int64.MaxValue;

                if (admittingExternalBacklog && arrivalTicks > now)
                {
                    admittingExternalBacklog = false;
                    if (inService < 0 && pending.Count > 0)
                    {
                        inService = pending.Dequeue();
                        UpdateServiceClock(ref serviceClock, ref serviceSettings);
                        beforeCurrentService = serviceClock;
                        long serviceStart = Volatile.Read(ref _simulateSlowdown) == 0
                            ? now : externalBacklogServiceStart;
                        long serviceTicks = MicrosecondsToTicks(
                            NextPlaybackServiceMicroseconds(ref serviceClock, _events[inService]));
                        completionTicks = checked(serviceStart + serviceTicks);
                        StartOutputPacedServiceHold(serviceTicks,
                            ref serviceHold, ref completionStamp);
                    }
                    UpdateQueue(pending.Count, inService >= 0);
                }

                bool serviceComplete = inService >= 0 && (serviceHold
                    ? arrivalTicks > now && completionStamp <= Stopwatch.GetTimestamp()
                    : completionTicks <= arrivalTicks && completionTicks <= now);
                if (serviceComplete)
                {
                    long completedAt = completionTicks;
                    long providerSendStart;
                    long providerSendEnd;
                    Dispatch(inService, now, out providerSendStart, out providerSendEnd);
                    EndOutputPacedHold(serviceHold);
                    serviceHold = false;
                    completionStamp = Int64.MaxValue;
                    now = CurrentTransportTicks();
                    inService = -1;
                    completionTicks = Int64.MaxValue;
                    if (!seekPending && nextArrival < _events.Count &&
                        EventTicks(nextArrival) > providerSendStart &&
                        EventTicks(nextArrival) <= providerSendEnd)
                    {
                        // A synchronous provider call allowed later source
                        // arrivals to become overdue. Admit that whole eligible
                        // range through the real finite queue before dispatching
                        // anything else, so the displayed pressure and overflow
                        // decision describe the same backlog.
                        admittingExternalBacklog = true;
                        externalBacklogServiceStart = completedAt;
                    }
                    if (!seekPending && ageLimited && pending.Count > 0)
                    {
                        long logicalNowMicroseconds = TicksToMicroseconds(completedAt);
                        long ageLimit = Interlocked.Read(ref _queueAgeLimitMicroseconds);
                        OverflowPolicy completionPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                        lock (_sync)
                        {
                            long observedAge = Math.Max(0, logicalNowMicroseconds -
                                _events[pending.OldestEventIndex].IntendedMicroseconds);
                            if (observedAge > _maximumQueueAgeMicroseconds)
                                _maximumQueueAgeMicroseconds = observedAge;
                        }
                        if (completionPolicy == OverflowPolicy.DropOldest)
                        {
                            while (QueueAgeExceeded(pending, logicalNowMicroseconds, ageLimit))
                            {
                                int evicted = pending.Dequeue();
                                consecutiveDrops++;
                                lock (_sync) _droppedEvents++;
                                RecordChannelDrop(evicted);
                                if (traceEnabled)
                                    TraceDropped(evicted, now,
                                        "oldest pending event exceeded the waiting-time limit before service",
                                        ClusterSizeAt(evicted), consecutiveDrops,
                                        EstimateBusyUntil(completionTicks, pending, serviceClock),
                                        pending.Count, maximumBufferOccupancy);
                            }
                        }
                        else if (completionPolicy == OverflowPolicy.DropOldestCompleteNote)
                        {
                            while (QueueAgeExceeded(pending, logicalNowMicroseconds, ageLimit))
                            {
                                int evictedAttack;
                                int evictedRelease;
                                if (!pending.TryEvictOldestCompleteNoteAtHead(
                                    out evictedAttack, out evictedRelease)) break;
                                if (evictedRelease < 0) completeNotes.MarkUnsentAttackEvicted(evictedAttack);
                                int discarded = evictedRelease >= 0 ? 2 : 1;
                                consecutiveDrops += discarded;
                                lock (_sync) _droppedEvents += discarded;
                                RecordChannelDrop(evictedAttack);
                                if (evictedRelease >= 0) RecordChannelDrop(evictedRelease);
                                if (traceEnabled)
                                {
                                    TraceDropped(evictedAttack, now,
                                        "oldest unsent note exceeded the waiting-time limit before service",
                                        ClusterSizeAt(evictedAttack), consecutiveDrops,
                                        EstimateBusyUntil(completionTicks, pending, serviceClock),
                                        pending.Count, maximumBufferOccupancy);
                                    if (evictedRelease >= 0)
                                        TraceDropped(evictedRelease, now,
                                            "paired pending release evicted with its overdue attack",
                                            ClusterSizeAt(evictedRelease), consecutiveDrops,
                                            EstimateBusyUntil(completionTicks, pending, serviceClock),
                                            pending.Count, maximumBufferOccupancy);
                                }
                            }
                        }
                        else if (completionPolicy == OverflowPolicy.ClearBufferAndCatchUp &&
                            QueueAgeExceeded(pending, logicalNowMicroseconds, ageLimit))
                        {
                            int clearedCount = pending.Count;
                            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
                            foreach (int pendingIndex in pending)
                            {
                                if (trackChannels) _channelState.RecordDropped(_events[pendingIndex]);
                                if (traceEnabled)
                                {
                                    consecutiveDrops++;
                                    TraceDropped(pendingIndex, now,
                                        "waiting-time limit cleared pending work; caught up to realtime",
                                        ClusterSizeAt(pendingIndex), consecutiveDrops, 0, 0,
                                        maximumBufferOccupancy);
                                }
                            }
                            pending.Clear();
                            int catchUp = FindFirstEventAfterTransport(nextArrival, completedAt);
                            int skippedDrops = 0;
                            for (int skipped = nextArrival; skipped < catchUp; skipped++)
                            {
                                if (HasActiveSourceFilters() &&
                                    MarkNewSourceFiltered(skipped, filtered))
                                {
                                    MidiEventView filteredEvent = _events[skipped];
                                    CompleteNoteEventKind filteredKind =
                                        CompleteNoteTracker.Classify(filteredEvent);
                                    if (filteredKind == CompleteNoteEventKind.NoteOn)
                                        completeNotes.RecordFilteredNoteOn(filteredEvent, skipped);
                                    else if (filteredKind == CompleteNoteEventKind.NoteOff)
                                        completeNotes.TakeNoteOff(filteredEvent);
                                    continue;
                                }
                                skippedDrops++;
                                if (trackChannels) _channelState.RecordDropped(_events[skipped]);
                                if (traceEnabled)
                                {
                                    consecutiveDrops++;
                                    TraceDropped(skipped, now,
                                        "waiting-time limit caught up to realtime",
                                        ClusterSizeAt(skipped), consecutiveDrops, 0, 0,
                                        maximumBufferOccupancy);
                                }
                            }
                            clearedCount += skippedDrops;
                            if (!traceEnabled) consecutiveDrops += clearedCount;
                            lock (_sync) _droppedEvents += clearedCount;
                            nextArrival = catchUp;
                            UpdateQueue(0, false);
                            SetCurrentLag(0);
                            try { _output.Panic(); }
                            catch { }
                            _channelOverrides.MarkAllForcedPending();
                            if (trackChannels)
                            {
                                _channelState.PanicDirect();
                                PublishChannelState(true);
                            }
                            continue;
                        }
                    }
                    if (!admittingExternalBacklog && pending.Count > 0)
                    {
                        inService = pending.Dequeue();
                        // Backlogged service begins at the preceding logical
                        // completion, not at the scheduler thread's wake time.
                        long serviceStart = completedAt;
                        UpdateServiceClock(ref serviceClock, ref serviceSettings);
                        beforeCurrentService = serviceClock;
                        long serviceTicks = MicrosecondsToTicks(
                            NextPlaybackServiceMicroseconds(ref serviceClock, _events[inService]));
                        completionTicks = checked(serviceStart + serviceTicks);
                        StartOutputPacedServiceHold(serviceTicks,
                            ref serviceHold, ref completionStamp);
                        if (traceEnabled)
                            TraceService(inService, serviceStart, completionTicks,
                                EstimateBusyUntil(completionTicks, pending, serviceClock));
                    }
                    UpdateQueue(pending.Count, inService >= 0);
                    continue;
                }

                if (nextArrival < _events.Count && arrivalTicks <= now)
                {
                    if (HasActiveSourceFilters() && MarkNewSourceFiltered(nextArrival, filtered))
                    {
                        MidiEventView filteredEvent = _events[nextArrival];
                        CompleteNoteEventKind filteredKind = CompleteNoteTracker.Classify(filteredEvent);
                        if (filteredKind == CompleteNoteEventKind.NoteOn)
                            completeNotes.RecordFilteredNoteOn(filteredEvent, nextArrival);
                        else if (filteredKind == CompleteNoteEventKind.NoteOff)
                            completeNotes.TakeNoteOff(filteredEvent);
                        nextArrival++;
                        UpdateQueue(pending.Count, inService >= 0);
                        continue;
                    }
                    UpdateServiceClock(ref serviceClock, ref serviceSettings);
                    if (!admittingExternalBacklog && !traceEnabled && inService < 0 && pending.Count == 0 &&
                        (Volatile.Read(ref _simulateSlowdown) == 0 || serviceClock.IsImmediate))
                    {
                        int dueEnd = nextArrival + 1;
                        int dueLimit = Math.Min(_events.Count, nextArrival + 2048);
                        while (dueEnd < dueLimit && EventTicks(dueEnd) <= now) dueEnd++;
                        PublishDropQueue(nextArrival + 1, 0, true);
                        if (HasActiveSourceFilters())
                        {
                            long eligibleImmediate = dueEnd - nextArrival;
                            DispatchImmediateFilteredRange(ref nextArrival, dueEnd, now, filtered,
                                ref eligibleImmediate, serviceSettings, true);
                        }
                        else
                        {
                            DispatchImmediateRange(ref nextArrival, dueEnd, now, serviceSettings);
                        }
                        long afterImmediate = CurrentTransportTicks();
                        if (nextArrival < _events.Count && EventTicks(nextArrival) <= afterImmediate &&
                            afterImmediate - now >= Stopwatch.Frequency / 125)
                        {
                            admittingExternalBacklog = true;
                            externalBacklogServiceStart = now;
                        }
                        PublishDropQueue(nextArrival, 0, false);
                        continue;
                    }
                    if (traceEnabled && (arrivalTicks != clusterTimestamp || nextArrival >= clusterEnd))
                    {
                        clusterTimestamp = arrivalTicks;
                        clusterEnd = nextArrival + 1;
                        while (clusterEnd < _events.Count && EventTicks(clusterEnd) == arrivalTicks) clusterEnd++;
                        clusterSize = clusterEnd - nextArrival;
                    }

                    MidiEventView incomingEvent = _events[nextArrival];
                    CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(incomingEvent);
                    OverflowPolicy overflowPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                    pending.SetNoteIndexEnabled(overflowPolicy == OverflowPolicy.DropOldestCompleteNote ||
                        overflowPolicy == OverflowPolicy.DropBackgroundNotes);
                    CompleteNoteTracker.NoteOffMatch noteMatch = noteKind == CompleteNoteEventKind.NoteOff
                        ? completeNotes.TakeNoteOff(incomingEvent)
                        : new CompleteNoteTracker.NoteOffMatch { AttackIndex = -1 };
                    if (noteMatch.Filtered)
                    {
                        _channelRouting.RecordFiltered(incomingEvent.Channel);
                        Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                        nextArrival++;
                        UpdateQueue(pending.Count, inService >= 0);
                        continue;
                    }
                    if (noteMatch.Suppress || ((overflowPolicy == OverflowPolicy.DropOldestCompleteNote ||
                        overflowPolicy == OverflowPolicy.DropBackgroundNotes) &&
                        noteKind == CompleteNoteEventKind.NoteOff && noteMatch.AttackIndex >= 0 && !noteMatch.Accepted))
                    {
                        consecutiveDrops++;
                        lock (_sync) _droppedEvents++;
                        RecordChannelDrop(nextArrival);
                        if (traceEnabled)
                            TraceDropped(nextArrival, now, "note-off suppressed because its paired note-on was rejected",
                                clusterSize, consecutiveDrops, EstimateBusyUntil(completionTicks, pending, serviceClock),
                                pending.Count + (inService >= 0 ? 1 : 0), maximumBufferOccupancy);
                        nextArrival++;
                        UpdateQueue(pending.Count, inService >= 0);
                        continue;
                    }

                    PendingMidiQueue.Entry incomingEntry = new PendingMidiQueue.Entry
                    {
                        EventIndex = nextArrival,
                        PairedAttackIndex = noteMatch.AttackIndex,
                        IsNoteOn = noteKind == CompleteNoteEventKind.NoteOn,
                        IsNoteOff = noteKind == CompleteNoteEventKind.NoteOff
                    };
                    long arrivalMicroseconds = admittingExternalBacklog
                        ? TicksToMicroseconds(now) : incomingEvent.IntendedMicroseconds;
                    long ageLimit = Interlocked.Read(ref _queueAgeLimitMicroseconds);
                    bool ageEvictionStalled = false;
                    if (ageLimited && overflowPolicy == OverflowPolicy.DropOldest)
                    {
                        while (QueueAgeExceeded(pending, arrivalMicroseconds, ageLimit))
                        {
                            int evicted = pending.Dequeue();
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            RecordChannelDrop(evicted);
                            if (traceEnabled)
                                TraceDropped(evicted, now, "oldest pending event exceeded the waiting-time limit",
                                    ClusterSizeAt(evicted), consecutiveDrops,
                                    EstimateBusyUntil(completionTicks, pending, serviceClock),
                                    pending.Count + (inService >= 0 ? 1 : 0), maximumBufferOccupancy);
                        }
                    }
                    else if (ageLimited && overflowPolicy == OverflowPolicy.DropOldestCompleteNote)
                    {
                        while (QueueAgeExceeded(pending, arrivalMicroseconds, ageLimit))
                        {
                            int previousOldest = pending.OldestEventIndex;
                            int evictedAttack;
                            int evictedRelease;
                            if (!pending.TryEvictOldestCompleteNoteAtHead(out evictedAttack, out evictedRelease))
                            {
                                ageEvictionStalled = true;
                                break;
                            }
                            if (evictedRelease < 0) completeNotes.MarkUnsentAttackEvicted(evictedAttack);
                            int discarded = evictedRelease >= 0 ? 2 : 1;
                            consecutiveDrops += discarded;
                            lock (_sync) _droppedEvents += discarded;
                            RecordChannelDrop(evictedAttack);
                            if (evictedRelease >= 0) RecordChannelDrop(evictedRelease);
                            // A later removable note cannot repair a protected
                            // overdue head. Stop rather than discarding unrelated
                            // music or retrying indefinitely; this is the stated
                            // soft-limit case.
                            if (pending.Count > 0 && pending.OldestEventIndex == previousOldest)
                            {
                                ageEvictionStalled = true;
                                break;
                            }
                        }
                    }
                    int outstanding = pending.Count + (inService >= 0 ? 1 : 0);
                    int bufferCapacity = ageLimited ? Int32.MaxValue :
                        Volatile.Read(ref _queueLengthLimit);
                    bool ageExceeded = ageLimited && QueueAgeExceeded(pending, arrivalMicroseconds, ageLimit);
                    bool protectedTraffic = noteKind == CompleteNoteEventKind.NoteOff ||
                        CompleteNoteTracker.IsSafetyControl(incomingEvent);
                    bool safetyAdmission = overflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                        noteKind != CompleteNoteEventKind.NoteOn;
                    if ((!ageLimited && outstanding < bufferCapacity || ageLimited && !ageExceeded) || safetyAdmission)
                    {
                        int acceptedIndex = nextArrival;
                        consecutiveDrops = 0;
                        if (inService < 0 && !admittingExternalBacklog)
                        {
                            inService = acceptedIndex;
                            long serviceStart = arrivalTicks;
                            UpdateServiceClock(ref serviceClock, ref serviceSettings);
                            beforeCurrentService = serviceClock;
                            long serviceTicks = MicrosecondsToTicks(
                                NextPlaybackServiceMicroseconds(ref serviceClock, _events[inService]));
                            completionTicks = checked(serviceStart + serviceTicks);
                            StartOutputPacedServiceHold(serviceTicks,
                                ref serviceHold, ref completionStamp);
                        }
                        else
                        {
                            pending.Enqueue(incomingEntry);
                        }
                        int occupancy = pending.Count + (inService >= 0 ? 1 : 0);
                        if (occupancy > maximumBufferOccupancy) maximumBufferOccupancy = occupancy;
                        if (noteKind == CompleteNoteEventKind.NoteOn)
                            completeNotes.RecordNoteOn(incomingEvent, acceptedIndex, true, false);
                        if (traceEnabled)
                        {
                            long busyUntil = EstimateBusyUntil(completionTicks, pending, serviceClock);
                            TraceAcceptedAdmission(acceptedIndex, now, clusterSize, occupancy, maximumBufferOccupancy, busyUntil);
                            if (acceptedIndex == inService)
                                TraceService(acceptedIndex, arrivalTicks, completionTicks, busyUntil);
                        }
                        UpdateQueue(pending.Count, inService >= 0);
                    }
                    else
                    {
                        if (overflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                        {
                            MidiEventView oldestEvent = _events[pending.OldestEventIndex];
                            bool oldestProtected = CompleteNoteTracker.Classify(oldestEvent) ==
                                CompleteNoteEventKind.NoteOff || CompleteNoteTracker.IsSafetyControl(oldestEvent);
                            if (!oldestProtected)
                            {
                                int evicted = pending.Dequeue();
                                consecutiveDrops++;
                                lock (_sync) _droppedEvents++;
                                RecordChannelDrop(evicted);
                                if (traceEnabled)
                                    TraceDropped(evicted, now, "oldest removable pending event evicted on overflow", ClusterSizeAt(evicted), consecutiveDrops,
                                        EstimateBusyUntil(completionTicks, pending, serviceClock), outstanding - 1, maximumBufferOccupancy);

                                pending.Enqueue(incomingEntry);
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival, true, false);
                                if (traceEnabled)
                                {
                                    long busyUntil = EstimateBusyUntil(completionTicks, pending, serviceClock);
                                    TraceAcceptedAdmission(nextArrival, now, clusterSize, outstanding, maximumBufferOccupancy, busyUntil);
                                }
                                consecutiveDrops = 0;
                            }
                            else if (protectedTraffic)
                            {
                                // A protected head cannot be removed safely.
                                // Admit the arriving release/safety message as
                                // explicit soft-limit traffic.
                                pending.Enqueue(incomingEntry);
                                int occupancy = pending.Count + (inService >= 0 ? 1 : 0);
                                if (occupancy > maximumBufferOccupancy) maximumBufferOccupancy = occupancy;
                                if (traceEnabled)
                                    TraceAcceptedAdmission(nextArrival, now, clusterSize, occupancy,
                                        maximumBufferOccupancy, EstimateBusyUntil(completionTicks, pending, serviceClock));
                                consecutiveDrops = 0;
                            }
                            else
                            {
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival, false, false);
                                consecutiveDrops++;
                                lock (_sync) _droppedEvents++;
                                RecordChannelDrop(nextArrival);
                                if (traceEnabled)
                                    TraceDropped(nextArrival, now, "oldest pending event is protected; incoming event rejected",
                                        clusterSize, consecutiveDrops, EstimateBusyUntil(completionTicks, pending, serviceClock),
                                        outstanding, maximumBufferOccupancy);
                            }
                            UpdateQueue(pending.Count, inService >= 0);
                        }
                        else if (overflowPolicy == OverflowPolicy.DropOldestCompleteNote)
                        {
                            int evictedAttack = -1;
                            int evictedRelease = -1;
                            bool evicted = !ageEvictionStalled &&
                                pending.TryEvictOldestCompleteNote(out evictedAttack, out evictedRelease);
                            if (evicted)
                            {
                                if (evictedRelease < 0) completeNotes.MarkUnsentAttackEvicted(evictedAttack);
                                int discarded = evictedRelease >= 0 ? 2 : 1;
                                consecutiveDrops += discarded;
                                lock (_sync) _droppedEvents += discarded;
                                RecordChannelDrop(evictedAttack);
                                if (evictedRelease >= 0) RecordChannelDrop(evictedRelease);
                                if (traceEnabled)
                                {
                                    TraceDropped(evictedAttack, now, "oldest unsent note occurrence evicted",
                                        ClusterSizeAt(evictedAttack), consecutiveDrops,
                                        EstimateBusyUntil(completionTicks, pending, serviceClock),
                                        pending.Count + (inService >= 0 ? 1 : 0), maximumBufferOccupancy);
                                    if (evictedRelease >= 0)
                                        TraceDropped(evictedRelease, now, "paired pending release evicted with its attack",
                                            ClusterSizeAt(evictedRelease), consecutiveDrops,
                                            EstimateBusyUntil(completionTicks, pending, serviceClock),
                                            pending.Count + (inService >= 0 ? 1 : 0), maximumBufferOccupancy);
                                }
                            }
                            bool ownAttackEvicted = evicted && noteKind == CompleteNoteEventKind.NoteOff &&
                                noteMatch.AttackIndex == evictedAttack;
                            if (ownAttackEvicted || (!evicted && !protectedTraffic))
                            {
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival, false, true);
                                consecutiveDrops++;
                                lock (_sync) _droppedEvents++;
                                RecordChannelDrop(nextArrival);
                                if (traceEnabled)
                                    TraceDropped(nextArrival, now, ownAttackEvicted
                                        ? "release suppressed with its evicted unsent attack"
                                        : "no eligible old unsent note; incoming event rejected",
                                        clusterSize, consecutiveDrops,
                                        EstimateBusyUntil(completionTicks, pending, serviceClock),
                                        pending.Count + (inService >= 0 ? 1 : 0), maximumBufferOccupancy);
                            }
                            else
                            {
                                pending.Enqueue(incomingEntry);
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival, true, false);
                                int occupancy = pending.Count + (inService >= 0 ? 1 : 0);
                                if (occupancy > maximumBufferOccupancy) maximumBufferOccupancy = occupancy;
                                if (traceEnabled)
                                    TraceAcceptedAdmission(nextArrival, now, clusterSize, occupancy,
                                        maximumBufferOccupancy,
                                        EstimateBusyUntil(completionTicks, pending, serviceClock));
                                consecutiveDrops = 0;
                            }
                            UpdateQueue(pending.Count, inService >= 0);
                        }
                    else if (overflowPolicy == OverflowPolicy.DropBackgroundNotes)
                        {
                            int evictedAttack;
                            int evictedRelease;
                            bool evicted = pending.TryEvictLeastImportantCompleteNote(
                                _events, ref musicalFairnessCursor, out evictedAttack,
                                out evictedRelease);
                            if (evicted)
                            {
                                if (evictedRelease < 0)
                                    completeNotes.MarkUnsentAttackEvicted(evictedAttack);
                                int discarded = evictedRelease >= 0 ? 2 : 1;
                                consecutiveDrops += discarded;
                                lock (_sync) _droppedEvents += discarded;
                                RecordChannelDrop(evictedAttack);
                                if (evictedRelease >= 0) RecordChannelDrop(evictedRelease);
                            }
                            bool ownAttackEvicted = evicted &&
                                noteKind == CompleteNoteEventKind.NoteOff &&
                                noteMatch.AttackIndex == evictedAttack;
                            if (ownAttackEvicted || (!evicted &&
                                noteKind == CompleteNoteEventKind.NoteOn))
                            {
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival,
                                        false, true);
                                consecutiveDrops++;
                                lock (_sync) _droppedEvents++;
                                RecordChannelDrop(nextArrival);
                            }
                            else
                            {
                                pending.Enqueue(incomingEntry);
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival,
                                        true, false);
                                int occupancy = pending.Count + (inService >= 0 ? 1 : 0);
                                if (occupancy > maximumBufferOccupancy)
                                    maximumBufferOccupancy = occupancy;
                                consecutiveDrops = 0;
                            }
                            UpdateQueue(pending.Count, inService >= 0);
                        }
                        else if (overflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                        {
                            System.Collections.Generic.List<int> cleared = traceEnabled
                                ? new System.Collections.Generic.List<int>(outstanding + 1) : null;
                            // A live capacity decrease never retracts the event
                            // already in service. Clear only pending/source work;
                            // the in-service event remains ordered at the head.
                            int clearedCount = pending.Count;
                            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
                            if (trackChannels)
                                foreach (int pendingIndex in pending) _channelState.RecordDropped(_events[pendingIndex]);
                            if (traceEnabled)
                                while (pending.Count > 0) cleared.Add(pending.Dequeue());
                            else
                                pending.Clear();

                            int catchUp = nextArrival;
                            if (!traceEnabled)
                            {
                                catchUp = FindFirstEventAfterTransport(nextArrival, now);
                                clearedCount += catchUp - nextArrival;
                            }
                            while (traceEnabled && catchUp < _events.Count && EventTicks(catchUp) <= now)
                            {
                                if (traceEnabled) cleared.Add(catchUp);
                                clearedCount++;
                                catchUp++;
                            }
                            if (trackChannels)
                                for (int skipped = nextArrival; skipped < catchUp; skipped++)
                                    _channelState.RecordDropped(_events[skipped]);

                            if (traceEnabled)
                            {
                                for (int clearedIndex = 0; clearedIndex < cleared.Count; clearedIndex++)
                                {
                                    int eventIndex = cleared[clearedIndex];
                                    consecutiveDrops++;
                                    TraceDropped(eventIndex, now, "buffer cleared; caught up to realtime", ClusterSizeAt(eventIndex),
                                        consecutiveDrops, 0, 0, maximumBufferOccupancy);
                                }
                            }
                            else consecutiveDrops += clearedCount;
                            lock (_sync) _droppedEvents += clearedCount;
                            nextArrival = catchUp;
                            UpdateQueue(0, inService >= 0);
                            SetCurrentLag(0);
                            try { _output.Panic(); }
                            catch { }
                            _channelOverrides.MarkAllForcedPending();
                            if (trackChannels)
                            {
                                _channelState.PanicDirect();
                                PublishChannelState(true);
                            }
                            continue;
                        }
                        else if (overflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                            noteKind == CompleteNoteEventKind.NoteOn)
                        {
                            completeNotes.RecordNoteOn(incomingEvent, nextArrival, false, true);
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            RecordChannelDrop(nextArrival);
                            if (traceEnabled)
                                TraceDropped(nextArrival, now, "incoming note-on rejected as a complete-note pair; buffer full (" +
                                    bufferCapacity + " events outstanding)", clusterSize, consecutiveDrops,
                                    EstimateBusyUntil(completionTicks, pending, serviceClock), outstanding, maximumBufferOccupancy);
                        }
                        else
                        {
                            if (protectedTraffic)
                            {
                                pending.Enqueue(incomingEntry);
                                int occupancy = pending.Count + (inService >= 0 ? 1 : 0);
                                if (occupancy > maximumBufferOccupancy) maximumBufferOccupancy = occupancy;
                                if (traceEnabled)
                                    TraceAcceptedAdmission(nextArrival, now, clusterSize, occupancy,
                                        maximumBufferOccupancy, EstimateBusyUntil(completionTicks, pending, serviceClock));
                                consecutiveDrops = 0;
                                UpdateQueue(pending.Count, inService >= 0);
                            }
                            else
                            {
                                if (noteKind == CompleteNoteEventKind.NoteOn)
                                    completeNotes.RecordNoteOn(incomingEvent, nextArrival, false, false);
                                consecutiveDrops++;
                                lock (_sync) _droppedEvents++;
                                RecordChannelDrop(nextArrival);
                                if (traceEnabled)
                                    TraceDropped(nextArrival, now, "newest event dropped; buffer full (" + bufferCapacity + " events outstanding)",
                                        clusterSize, consecutiveDrops, EstimateBusyUntil(completionTicks, pending, serviceClock), outstanding, maximumBufferOccupancy);
                            }
                        }
                    }
                    nextArrival++;
                    continue;
                }

                if ((seekPending || nextArrival >= _events.Count) && inService < 0 && pending.Count == 0)
                    return;

                if (inService >= 0 && serviceHold && arrivalTicks > now)
                {
                    PublishChannelStateBeforeWait(now, now);
                    WaitUntilStopwatch(waiter, completionStamp, iterationWakeGeneration);
                    continue;
                }
                long target = Math.Min(arrivalTicks, completionTicks);
                if (inService < 0)
                    SetCurrentLag(0);
                PublishChannelStateBeforeWait(target, now);
                WaitUntil(waiter, target, iterationWakeGeneration);
            }
        }

        private void RunVirtualForwardDropMode(HighResolutionWaiter waiter)
        {
            ServiceDurationSettings serviceSettings = Volatile.Read(ref _serviceSettings);
            ServiceDurationClock serviceClock = serviceSettings.CreateClock();
            int nextArrival = _startEventIndex;
            int bufferCapacity = Volatile.Read(ref _queueLengthLimit);
            ForwardDropQueue virtualQueue = new ForwardDropQueue(bufferCapacity);
            CompleteNoteTracker completeNotes = new CompleteNoteTracker();
            FilteredSourceEvents filtered = new FilteredSourceEvents();
            int routingGeneration = _channelRouting.FilterGeneration;
            int overrideGeneration = _channelOverrides.FilterGeneration;
            if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                bool seekPending = HasPreservedSeekRequest();
                if (!seekPending) ApplyPendingStateChase();
                ApplyPendingOverrides();
                long nowTicks = CurrentTransportTicks();
                PublishDropQueue(nextArrival, 0, false);

                if (seekPending) return;

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                int currentOverrideGeneration = _channelOverrides.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration)
                {
                    routingGeneration = currentRoutingGeneration;
                    overrideGeneration = currentOverrideGeneration;
                    if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                }

                if (nextArrival < _events.Count && EventTicks(nextArrival) <= nowTicks)
                {
                    int eventIndex = nextArrival++;
                    MidiEventView incomingEvent = _events[eventIndex];
                    long arrival = incomingEvent.IntendedMicroseconds;
                    virtualQueue.Advance(arrival);
                    PublishVirtualQueue(virtualQueue);

                    if (HasActiveSourceFilters() && MarkNewSourceFiltered(eventIndex, filtered)) continue;

                    CompleteNoteEventKind noteKind = CompleteNoteTracker.Classify(incomingEvent);
                    if (noteKind == CompleteNoteEventKind.NoteOff && completeNotes.ShouldSuppressNoteOff(incomingEvent))
                    {
                        RecordVirtualDrop(eventIndex);
                        continue;
                    }

                    OverflowPolicy policy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                    if (policy != OverflowPolicy.DropNewest && policy != OverflowPolicy.DropIncomingCompleteNotes)
                        throw new InvalidOperationException("The selected overflow policy requires Simulate slowdown because it would retract MIDI that the forward-only model has already sent.");
                    bool safetyAdmission = policy == OverflowPolicy.DropIncomingCompleteNotes &&
                        noteKind != CompleteNoteEventKind.NoteOn;
                    UpdateServiceClock(ref serviceClock, ref serviceSettings);
                    ServiceDurationClock admittedClock = serviceClock;
                    long service = admittedClock.NextMicroseconds(incomingEvent);
                    bool accepted = virtualQueue.TryAdmit(arrival, service, safetyAdmission);
                    if (!accepted)
                    {
                        if (noteKind == CompleteNoteEventKind.NoteOn)
                            completeNotes.RecordNoteOn(incomingEvent, false,
                                policy == OverflowPolicy.DropIncomingCompleteNotes);
                        RecordVirtualDrop(eventIndex);
                        PublishVirtualQueue(virtualQueue);
                        continue;
                    }

                    serviceClock = admittedClock;

                    if (noteKind == CompleteNoteEventKind.NoteOn)
                        completeNotes.RecordNoteOn(incomingEvent, true, false);
                    PublishVirtualQueue(virtualQueue);
                    PublishDropQueue(nextArrival, 0, true);
                    Dispatch(eventIndex, nowTicks);
                    PublishDropQueue(nextArrival, 0, false);
                    continue;
                }

                long nowMicroseconds = TicksToMicroseconds(nowTicks);
                virtualQueue.Advance(nowMicroseconds);
                PublishVirtualQueue(virtualQueue);
                if (nextArrival >= _events.Count) return;
                SetCurrentLag(0);
                long target = EventTicks(nextArrival);
                PublishChannelStateBeforeWait(target, nowTicks);
                WaitUntil(waiter, target, iterationWakeGeneration);
            }
        }

        private void RecordVirtualDrop(int eventIndex)
        {
            lock (_sync) _droppedEvents++;
            RecordChannelDrop(eventIndex);
        }

        private void PublishVirtualQueue(ForwardDropQueue queue)
        {
            lock (_sync)
            {
                _virtualQueueLength = Math.Max(0, queue.Occupancy - 1);
                _virtualOutstandingEvents = queue.Occupancy;
                _virtualMaximumQueueLength = Math.Max(_virtualMaximumQueueLength, queue.Occupancy);
            }
        }

        private void RunPerNoteIntervalGate(HighResolutionWaiter waiter)
        {
            long interval = ProcessingMicroseconds;
            int intervalLimit = IntervalPolyphonyLimitEnabled
                ? IntervalPolyphonyLimit : DefaultIntervalPolyphonyLimit;
            PerNoteIntervalGate gate = new PerNoteIntervalGate(interval,
                IntervalPolyphonyLimitEnabled, intervalLimit);
            AdaptiveNoteIntervalClock adaptive = new AdaptiveNoteIntervalClock(interval,
                ScaleNoteIntervalByTraffic);
            MidiEventView[] boundaryEvents = new MidiEventView[128];
            int nextArrival = _startEventIndex;
            int routingGeneration = _channelRouting.FilterGeneration;
            int[] disableGenerations = new int[16];
            for (int channel = 0; channel < 16; channel++)
                disableGenerations[channel] = _channelRouting.DisableGeneration(channel);
            FilteredSourceEvents sourceFilters = new FilteredSourceEvents();
            bool sourceCompleted = false;

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                bool seekPending = HasPreservedSeekRequest();
                if (!seekPending) ApplyPendingStateChase();
                ApplyPendingOverrides();
                long nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());

                if (seekPending && !sourceCompleted)
                {
                    gate.CompleteSource(nowMicroseconds);
                    AddGateFiltered(gate.TakeFilteredEventCount());
                    sourceCompleted = true;
                }

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration)
                {
                    for (int channel = 0; channel < 16; channel++)
                    {
                        int disabledAt = _channelRouting.DisableGeneration(channel);
                        if (disabledAt == disableGenerations[channel]) continue;
                        disableGenerations[channel] = disabledAt;
                        int retired = gate.RetireChannel(channel, nowMicroseconds);
                        for (int index = 0; index < retired; index++)
                            _channelRouting.RecordFiltered(channel);
                    }
                    routingGeneration = currentRoutingGeneration;
                    PublishChannelState(true);
                }

                bool didWork = false;
                int examined = 0;
                long batchStarted = Stopwatch.GetTimestamp();
                while (IsPlaying() && Volatile.Read(ref _dispatchSuspended) == 0)
                {
                    long localBoundary = gate.NextBoundaryMicroseconds;
                    long nextSource = !seekPending && nextArrival < _events.Count
                        ? _events[nextArrival].IntendedMicroseconds : Int64.MaxValue;
                    adaptive.CompleteIfClosed(localBoundary, nextSource);
                    long nextBoundary = adaptive.EffectiveBoundary(localBoundary);
                    long nextAction = Math.Min(nextBoundary, nextSource);
                    if (nextAction > nowMicroseconds) break;

                    // Source events exactly on a boundary are admitted before
                    // that boundary is resolved. All events at one absolute
                    // MIDI tick are kept together so simultaneous same-pitch
                    // layers make one deterministic attack decision.
                    if (nextSource <= nextBoundary)
                    {
                        long sourceTick = _events[nextArrival].AbsoluteTick;
                        gate.BeginSourceTick(sourceTick);
                        while (nextArrival < _events.Count && _events[nextArrival].AbsoluteTick == sourceTick)
                        {
                            int eventIndex = nextArrival++;
                            examined++;
                            if (HasActiveSourceFilters() && MarkNewSourceFiltered(eventIndex, sourceFilters))
                                continue;
                            MidiEventView midiEvent = _events[eventIndex];
                            PerNoteGateAdmission admission = gate.Admit(midiEvent);
                            if (admission == PerNoteGateAdmission.NotNote)
                            {
                                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                                    true, midiEvent.IntendedMicroseconds,
                                    gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                                TraceGateLogicalOutputForTesting(
                                    midiEvent.IntendedMicroseconds, midiEvent);
                                DispatchMidiEvent(midiEvent);
                                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                                    false, Int64.MaxValue,
                                    gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                            }
                            if (ShouldRetireWorkerAtDispatchBoundary()) return;
                        }
                        gate.EndSourceTick();
                        AddGateFiltered(gate.TakeFilteredEventCount());
                    }
                    else
                    {
                        Action<long> dispatchHook = _gateBoundaryDispatchHookForTesting;
                        if (dispatchHook != null) dispatchHook(nextBoundary);
                        int count = gate.EmitBoundary(nextBoundary, boundaryEvents);
                        TraceGateLogicalOutputForTesting(nextBoundary, boundaryEvents, count);
                        adaptive.RecordBoundaryPass(nextBoundary, count);
                        AddGateFiltered(gate.TakeFilteredEventCount());
                        for (int index = 0; index < count; index++)
                        {
                            // A native send may have been blocked while a
                            // transport boundary was requested. Do not emit
                            // the rest of the logical batch after it returns.
                            if (ShouldRetireWorkerAtDispatchBoundary()) return;
                            PublishStructuralDueWork(nextArrival, count - index - 1,
                                nextBoundary, true, nextBoundary,
                                    gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                            DispatchMidiEvent(boundaryEvents[index]);
                            PublishStructuralDueWork(nextArrival, count - index - 1,
                                nextBoundary, false, Int64.MaxValue,
                                gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                        }
                        AdvanceGateEffectiveSpeedFrontier(nextBoundary);
                        examined += Math.Max(1, count);
                    }
                    didWork = true;
                    nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());
                    if (examined >= 2048 || Stopwatch.GetTimestamp() - batchStarted >= Stopwatch.Frequency / 125)
                        break;
                }

                long nextSourceAfterWork = !seekPending && nextArrival < _events.Count
                    ? _events[nextArrival].IntendedMicroseconds : Int64.MaxValue;
                adaptive.CompleteIfClosed(gate.NextBoundaryMicroseconds, nextSourceAfterWork);
                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                    false, Int64.MaxValue, gate.NextBoundaryMicroseconds,
                    gate.NextBoundaryOutputCount);
                if (!seekPending && nextArrival >= _events.Count && !sourceCompleted)
                {
                    MidiSong song;
                    lock (_sync) song = _song;
                    gate.CompleteSource(song == null ? nowMicroseconds : song.DurationMicroseconds);
                    AddGateFiltered(gate.TakeFilteredEventCount());
                    sourceCompleted = true;
                }
                if ((seekPending || nextArrival >= _events.Count) && !gate.HasPendingTransitions) return;

                long targetMicroseconds = adaptive.EffectiveBoundary(gate.NextBoundaryMicroseconds);
                if (!seekPending && nextArrival < _events.Count)
                    targetMicroseconds = Math.Min(targetMicroseconds, _events[nextArrival].IntendedMicroseconds);
                long nowTicks = CurrentTransportTicks();
                long targetTicks = MicrosecondsToTicks(targetMicroseconds);
                PublishChannelStateBeforeWait(targetTicks, nowTicks);
                if (!didWork || targetTicks > nowTicks)
                    WaitUntil(waiter, targetTicks, iterationWakeGeneration);
            }
        }

        private void RunDelayedNoteOff(HighResolutionWaiter waiter)
        {
            long interval = ProcessingMicroseconds;
            int intervalLimit = IntervalPolyphonyLimitEnabled
                ? IntervalPolyphonyLimit : DefaultIntervalPolyphonyLimit;
            DelayedNoteOffGate gate = new DelayedNoteOffGate(interval,
                IntervalPolyphonyLimitEnabled, intervalLimit);
            AdaptiveNoteIntervalClock adaptive = new AdaptiveNoteIntervalClock(interval,
                ScaleNoteIntervalByTraffic);
            List<MidiEventView> boundaryEvents = new List<MidiEventView>(128);
            int nextArrival = _startEventIndex;
            int routingGeneration = _channelRouting.FilterGeneration;
            int[] disableGenerations = new int[16];
            for (int channel = 0; channel < 16; channel++)
                disableGenerations[channel] = _channelRouting.DisableGeneration(channel);
            FilteredSourceEvents sourceFilters = new FilteredSourceEvents();
            bool sourceCompleted = false;

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                bool seekPending = HasPreservedSeekRequest();
                if (!seekPending) ApplyPendingStateChase();
                ApplyPendingOverrides();
                long nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());

                if (seekPending && !sourceCompleted)
                {
                    gate.CompleteSource(nowMicroseconds);
                    sourceCompleted = true;
                }

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration)
                {
                    for (int channel = 0; channel < 16; channel++)
                    {
                        int disabledAt = _channelRouting.DisableGeneration(channel);
                        if (disabledAt == disableGenerations[channel]) continue;
                        disableGenerations[channel] = disabledAt;
                        int retired = gate.RetireChannel(channel, nowMicroseconds);
                        for (int index = 0; index < retired; index++)
                            _channelRouting.RecordFiltered(channel);
                    }
                    routingGeneration = currentRoutingGeneration;
                    PublishChannelState(true);
                }

                bool didWork = false;
                int examined = 0;
                long batchStarted = Stopwatch.GetTimestamp();
                while (IsPlaying() && Volatile.Read(ref _dispatchSuspended) == 0)
                {
                    long localBoundary = gate.NextBoundaryMicroseconds;
                    long nextSource = !seekPending && nextArrival < _events.Count
                        ? _events[nextArrival].IntendedMicroseconds : Int64.MaxValue;
                    adaptive.CompleteIfClosed(localBoundary, nextSource);
                    long nextBoundary = adaptive.EffectiveBoundary(localBoundary);
                    long nextAction = Math.Min(nextBoundary, nextSource);
                    if (nextAction > nowMicroseconds) break;

                    // D-boundary releases win exact-time ties. Recovery
                    // candidates at the exact R boundary are admitted first so
                    // all live candidates can share the Per-note priority rule.
                    bool sourcePrecedesRecovery = nextArrival < _events.Count &&
                        gate.SourcePrecedesBoundary(_events[nextArrival], nextBoundary);
                    if (nextBoundary < nextSource ||
                        nextBoundary == nextSource && !sourcePrecedesRecovery)
                    {
                        Action<long> dispatchHook = _gateBoundaryDispatchHookForTesting;
                        if (dispatchHook != null) dispatchHook(nextBoundary);
                        int count = gate.EmitBoundary(nextBoundary, boundaryEvents);
                        TraceGateLogicalOutputForTesting(nextBoundary, boundaryEvents, count);
                        adaptive.RecordBoundaryPass(nextBoundary, count);
                        AddGateFiltered(gate.TakeFilteredEventCount());
                        for (int index = 0; index < count; index++)
                        {
                            if (ShouldRetireWorkerAtDispatchBoundary()) return;
                            PublishStructuralDueWork(nextArrival, count - index - 1,
                                nextBoundary, true, nextBoundary,
                                gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                            DispatchMidiEvent(boundaryEvents[index]);
                            PublishStructuralDueWork(nextArrival, count - index - 1,
                                nextBoundary, false, Int64.MaxValue,
                                gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                        }
                        AdvanceGateEffectiveSpeedFrontier(nextBoundary);
                        examined += Math.Max(1, count);
                    }
                    else
                    {
                        int eventIndex = nextArrival++;
                        examined++;
                        if (!HasActiveSourceFilters() || !MarkNewSourceFiltered(eventIndex, sourceFilters))
                        {
                            MidiEventView midiEvent = _events[eventIndex];
                            DelayedNoteOffAdmission admission = gate.Admit(midiEvent);
                            if (admission == DelayedNoteOffAdmission.NotNote)
                            {
                                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                                    true, midiEvent.IntendedMicroseconds,
                                    gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                                TraceGateLogicalOutputForTesting(
                                    midiEvent.IntendedMicroseconds, midiEvent);
                                DispatchMidiEvent(midiEvent);
                                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                                    false, Int64.MaxValue,
                                    gate.NextBoundaryMicroseconds, gate.NextBoundaryOutputCount);
                                if (DelayedNoteOffGate.IsNoteTerminatingControl(midiEvent))
                                    AddGateFiltered(gate.RetireChannel(midiEvent.Channel,
                                        midiEvent.IntendedMicroseconds));
                            }
                        }
                        if (ShouldRetireWorkerAtDispatchBoundary()) return;
                        AddGateFiltered(gate.TakeFilteredEventCount());
                    }
                    didWork = true;
                    nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());
                    if (examined >= 2048 || Stopwatch.GetTimestamp() - batchStarted >= Stopwatch.Frequency / 125)
                        break;
                }

                long nextSourceAfterWork = !seekPending && nextArrival < _events.Count
                    ? _events[nextArrival].IntendedMicroseconds : Int64.MaxValue;
                adaptive.CompleteIfClosed(gate.NextBoundaryMicroseconds, nextSourceAfterWork);
                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                    false, Int64.MaxValue, gate.NextBoundaryMicroseconds,
                    gate.NextBoundaryOutputCount);
                if (!seekPending && nextArrival >= _events.Count && !sourceCompleted)
                {
                    MidiSong song;
                    lock (_sync) song = _song;
                    gate.CompleteSource(song == null ? nowMicroseconds : song.DurationMicroseconds);
                    AddGateFiltered(gate.TakeFilteredEventCount());
                    sourceCompleted = true;
                }
                if ((seekPending || nextArrival >= _events.Count) && !gate.HasPendingTransitions) return;

                long targetMicroseconds = adaptive.EffectiveBoundary(gate.NextBoundaryMicroseconds);
                if (!seekPending && nextArrival < _events.Count)
                    targetMicroseconds = Math.Min(targetMicroseconds, _events[nextArrival].IntendedMicroseconds);
                long nowTicks = CurrentTransportTicks();
                long targetTicks = MicrosecondsToTicks(targetMicroseconds);
                PublishChannelStateBeforeWait(targetTicks, nowTicks);
                if (!didWork || targetTicks > nowTicks)
                    WaitUntil(waiter, targetTicks, iterationWakeGeneration);
            }
        }

        private void RunNoteAttackLimit(HighResolutionWaiter waiter)
        {
            MusicalAttackRateLimiter limiter = new MusicalAttackRateLimiter(EventsPerSecond);
            List<MidiEventView> emissions = new List<MidiEventView>(128);
            int nextArrival = _startEventIndex;
            int routingGeneration = _channelRouting.FilterGeneration;
            int[] disableGenerations = new int[16];
            for (int channel = 0; channel < 16; channel++)
                disableGenerations[channel] = _channelRouting.DisableGeneration(channel);
            FilteredSourceEvents sourceFilters = new FilteredSourceEvents();

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                bool seekPending = HasPreservedSeekRequest();
                if (!seekPending) ApplyPendingStateChase();
                ApplyPendingOverrides();
                long nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());

                if (seekPending) return;

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration)
                {
                    for (int channel = 0; channel < 16; channel++)
                    {
                        int disabledAt = _channelRouting.DisableGeneration(channel);
                        if (disabledAt == disableGenerations[channel]) continue;
                        disableGenerations[channel] = disabledAt;
                        limiter.RetireChannel(channel);
                    }
                    routingGeneration = currentRoutingGeneration;
                    PublishChannelState(true);
                }

                bool didWork = false;
                int examined = 0;
                long batchStarted = Stopwatch.GetTimestamp();
                while (nextArrival < _events.Count && IsPlaying() &&
                    Volatile.Read(ref _dispatchSuspended) == 0 &&
                    _events[nextArrival].IntendedMicroseconds <= nowMicroseconds)
                {
                    long sourceTick = _events[nextArrival].AbsoluteTick;
                    long sourceTime = _events[nextArrival].IntendedMicroseconds;
                    limiter.BeginSourceTick(sourceTick);
                    while (nextArrival < _events.Count && _events[nextArrival].AbsoluteTick == sourceTick)
                    {
                        int eventIndex = nextArrival++;
                        examined++;
                        if (HasActiveSourceFilters() && MarkNewSourceFiltered(eventIndex, sourceFilters))
                            continue;
                        limiter.Admit(_events[eventIndex]);
                    }
                    int count = limiter.EndSourceTick(emissions);
                    AddMusicalLimitExclusions(limiter.TakeRejectedAttacks(), limiter.TakeSuppressedReleases());
                    for (int index = 0; index < count; index++)
                    {
                        if (ShouldRetireWorkerAtDispatchBoundary()) return;
                        PublishStructuralDueWork(nextArrival, count - index - 1,
                            sourceTime, true, sourceTime, Int64.MaxValue, 0);
                        DispatchMidiEvent(emissions[index]);
                        PublishStructuralDueWork(nextArrival, count - index - 1,
                            sourceTime, false, Int64.MaxValue, Int64.MaxValue, 0);
                    }
                    AdvanceGateEffectiveSpeedFrontier(sourceTime);
                    didWork = true;
                    nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());
                    if (examined >= 2048 || Stopwatch.GetTimestamp() - batchStarted >= Stopwatch.Frequency / 125)
                        break;
                }
                PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                    false, Int64.MaxValue, Int64.MaxValue, 0);
                if (nextArrival >= _events.Count)
                {
                    limiter.CompleteSource();
                    return;
                }
                long nowTicks = CurrentTransportTicks();
                long targetTicks = MicrosecondsToTicks(_events[nextArrival].IntendedMicroseconds);
                PublishChannelStateBeforeWait(targetTicks, nowTicks);
                if (!didWork || targetTicks > nowTicks)
                    WaitUntil(waiter, targetTicks, iterationWakeGeneration);
            }
        }

        private void RunOutputRateLimit(HighResolutionWaiter waiter)
        {
            OutputRateGate gate = new OutputRateGate(EventsPerSecond, 1000000L);
            List<MidiEventView> emissions = new List<MidiEventView>(128);
            int nextArrival = _startEventIndex;
            int routingGeneration = _channelRouting.FilterGeneration;
            int[] disableGenerations = new int[16];
            for (int channel = 0; channel < 16; channel++)
                disableGenerations[channel] = _channelRouting.DisableGeneration(channel);
            FilteredSourceEvents sourceFilters = new FilteredSourceEvents();
            _activeOutputRateGate = gate;
            try
            {
                while (IsActive())
                {
                    int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                    ApplyPendingChannelControls();
                    if (!WaitWhilePaused()) return;
                    bool seekPending = HasPreservedSeekRequest();
                    if (!seekPending) ApplyPendingStateChase();
                    ApplyPendingOverrides();
                    long nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());

                    if (seekPending) return;

                    int currentRoutingGeneration = _channelRouting.FilterGeneration;
                    if (currentRoutingGeneration != routingGeneration)
                    {
                        for (int channel = 0; channel < 16; channel++)
                        {
                            int disabledAt = _channelRouting.DisableGeneration(channel);
                            if (disabledAt == disableGenerations[channel]) continue;
                            disableGenerations[channel] = disabledAt;
                            gate.RetireChannel(channel);
                        }
                        routingGeneration = currentRoutingGeneration;
                        PublishChannelState(true);
                    }

                    bool didWork = false;
                    int examined = 0;
                    long batchStarted = Stopwatch.GetTimestamp();
                    while (nextArrival < _events.Count && IsPlaying() &&
                        Volatile.Read(ref _dispatchSuspended) == 0 &&
                        _events[nextArrival].IntendedMicroseconds <= nowMicroseconds)
                    {
                        long sourceTick = _events[nextArrival].AbsoluteTick;
                        long sourceTime = _events[nextArrival].IntendedMicroseconds;
                        gate.BeginSourceTick(sourceTick);
                        while (nextArrival < _events.Count &&
                            _events[nextArrival].AbsoluteTick == sourceTick)
                        {
                            int eventIndex = nextArrival++;
                            examined++;
                            if (HasActiveSourceFilters() &&
                                MarkNewSourceFiltered(eventIndex, sourceFilters)) continue;
                            gate.Admit(_events[eventIndex]);
                        }
                        int count = gate.EndSourceTick(emissions,
                            TicksToMicroseconds(Stopwatch.GetTimestamp()));
                        AddOutputRateExclusions(gate.TakeRejectedAttacks(),
                            gate.TakeSuppressedReleases(), gate.TakeRejectedOtherMessages());
                        for (int index = 0; index < count; index++)
                        {
                            if (ShouldRetireWorkerAtDispatchBoundary()) return;
                            PublishStructuralDueWork(nextArrival, count - index - 1,
                                sourceTime, true, sourceTime, Int64.MaxValue, 0);
                            DispatchMidiEvent(emissions[index]);
                            PublishStructuralDueWork(nextArrival, count - index - 1,
                                sourceTime, false, Int64.MaxValue, Int64.MaxValue, 0);
                        }
                        AdvanceGateEffectiveSpeedFrontier(sourceTime);
                        didWork = true;
                        nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());
                        if (examined >= 2048 ||
                            Stopwatch.GetTimestamp() - batchStarted >= Stopwatch.Frequency / 125)
                            break;
                    }
                    PublishStructuralDueWork(nextArrival, 0, Int64.MaxValue,
                        false, Int64.MaxValue, Int64.MaxValue, 0);
                    if (nextArrival >= _events.Count)
                    {
                        gate.CompleteSource();
                        AddOutputRateExclusions(gate.TakeRejectedAttacks(),
                            gate.TakeSuppressedReleases(), gate.TakeRejectedOtherMessages());
                        return;
                    }
                    long nowTicks = CurrentTransportTicks();
                    long targetTicks = MicrosecondsToTicks(_events[nextArrival].IntendedMicroseconds);
                    PublishChannelStateBeforeWait(targetTicks, nowTicks);
                    if (!didWork || targetTicks > nowTicks)
                        WaitUntil(waiter, targetTicks, iterationWakeGeneration);
                }
            }
            finally
            {
                if (Object.ReferenceEquals(_activeOutputRateGate, gate))
                    _activeOutputRateGate = null;
            }
        }

        private void AddMusicalLimitExclusions(long rejectedAttacks, long suppressedReleases)
        {
            if (rejectedAttacks == 0 && suppressedReleases == 0) return;
            lock (_sync)
            {
                _musicalLimitRejectedAttacks += rejectedAttacks;
                _musicalLimitSuppressedReleases += suppressedReleases;
            }
        }

        private void AddOutputRateExclusions(long rejectedAttacks, long suppressedReleases,
            long rejectedOtherMessages)
        {
            if (rejectedAttacks == 0 && suppressedReleases == 0 && rejectedOtherMessages == 0) return;
            lock (_sync)
            {
                _outputRateRejectedAttacks += rejectedAttacks;
                _outputRateSuppressedReleases += suppressedReleases;
                _outputRateRejectedOtherMessages += rejectedOtherMessages;
            }
        }

        private void AddGateFiltered(long count)
        {
            if (count == 0) return;
            lock (_sync) _gateFilteredEvents += count;
        }

        private void AdvanceGateEffectiveSpeedFrontier(long sourceMicroseconds)
        {
            lock (_sync)
            {
                long duration = _song == null ? sourceMicroseconds : _song.DurationMicroseconds;
                long resolved = Math.Min(sourceMicroseconds, duration);
                if (resolved > _effectiveSpeedFrontierMicroseconds)
                    _effectiveSpeedFrontierMicroseconds = resolved;
            }
        }

        private void Dispatch(int eventIndex, long actualTransportTicks)
        {
            long providerSendStart;
            long providerSendEnd;
            Dispatch(eventIndex, actualTransportTicks,
                out providerSendStart, out providerSendEnd);
        }

        private void Dispatch(int eventIndex, long actualTransportTicks,
            out long providerSendStart, out long providerSendEnd)
        {
            lock (_sync) _publishedInServiceEvent = eventIndex;
            DispatchMidiEvent(_events[eventIndex], out providerSendStart, out providerSendEnd);
        }

        private void DispatchMidiEvent(MidiEventView midiEvent)
        {
            long providerSendStart;
            long providerSendEnd;
            DispatchMidiEvent(midiEvent, out providerSendStart, out providerSendEnd);
        }

        private void DispatchMidiEvent(MidiEventView midiEvent,
            out long providerSendStart, out long providerSendEnd)
        {
            SendScheduledEvent(midiEvent, Volatile.Read(ref _channelMonitoringEnabled) != 0,
                out providerSendStart, out providerSendEnd);
            long actualUs = TicksToMicroseconds(CurrentTransportTicks());
            long lag = Math.Max(0, actualUs - midiEvent.IntendedMicroseconds);
            lock (_sync)
            {
                _processedEvents++;
                _lastDispatchedMicroseconds = midiEvent.IntendedMicroseconds;
                _currentLagMicroseconds = lag;
                if (lag > _maximumLagMicroseconds) _maximumLagMicroseconds = lag;
            }
            PublishChannelState(false);
        }

        private void DispatchImmediateRange(ref int nextIndex, int endExclusive, long transportAtBatchStart,
            ServiceDurationSettings serviceSettings)
        {
            const int chunkSize = 2048;
            long stopwatchAtBatchStart = Stopwatch.GetTimestamp();
            if (nextIndex < endExclusive && Volatile.Read(ref _dispatchSuspended) == 0)
            {
                int chunkEnd = Math.Min(endExclusive, nextIndex + chunkSize);
                int sent = 0;
                long lastTimeline = 0;
                long currentLag = 0;
                long maximumLag = 0;
                bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
                CompactSequentialCursor cursor = new CompactSequentialCursor();
                try
                {
                    while (nextIndex < chunkEnd)
                    {
                        // A native call can remain blocked while Stop, Seek, or
                        // Pause is requested.  Observe that request before every
                        // subsequent send, without taking the scheduler lock on
                        // the successful hot path.  Rate-model changes retain
                        // the established 64-event check cadence.
                        if (Volatile.Read(ref _dispatchSuspended) != 0) break;
                        if (!Object.ReferenceEquals(Volatile.Read(ref _serviceSettings), serviceSettings)) break;
                        long intendedMicroseconds;
                        MidiEventView midiEvent = _events.GetSequential(nextIndex, ref cursor, out intendedMicroseconds);
                        PublishImmediateInService(nextIndex + 1, nextIndex);
                        SendScheduledEvent(midiEvent, trackChannels);
                        long elapsedTicks = Stopwatch.GetTimestamp() - stopwatchAtBatchStart;
                        long actualTicks = transportAtBatchStart + elapsedTicks;
                        currentLag = Math.Max(0, TicksToMicroseconds(actualTicks) - intendedMicroseconds);
                        if (currentLag > maximumLag) maximumLag = currentLag;
                        lastTimeline = intendedMicroseconds;
                        sent++;
                        nextIndex++;
                        // Return to admission/publication after at most 2048
                        // sends or 8 ms of completed output work. Never wait for
                        // an entire multi-million-event range to drain.
                        if (elapsedTicks >= Stopwatch.Frequency / 125) break;
                    }
                }
                finally
                {
                    if (sent > 0)
                    {
                        lock (_sync)
                        {
                            _processedEvents += sent;
                            _lastDispatchedMicroseconds = lastTimeline;
                            _currentLagMicroseconds = currentLag;
                            if (maximumLag > _maximumLagMicroseconds) _maximumLagMicroseconds = maximumLag;
                        }
                    }
                    PublishChannelState(false);
                }
            }
        }

        private void DispatchImmediateFilteredRange(ref int nextIndex, int endExclusive, long transportAtBatchStart,
            FilteredSourceEvents filtered, ref long eligiblePending, ServiceDurationSettings serviceSettings,
            bool classifySourceAtDispatch = false)
        {
            const int chunkSize = 2048;
            long stopwatchAtBatchStart = Stopwatch.GetTimestamp();
            if (nextIndex >= endExclusive || Volatile.Read(ref _dispatchSuspended) != 0) return;
            int examined = 0;
            int sent = 0;
            long lastTimeline = 0;
            long currentLag = 0;
            long maximumLag = 0;
            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
            CompactSequentialCursor cursor = new CompactSequentialCursor();
            try
            {
                while (nextIndex < endExclusive && examined < chunkSize)
                {
                    if (Volatile.Read(ref _dispatchSuspended) != 0) break;
                    if (!Object.ReferenceEquals(Volatile.Read(ref _serviceSettings), serviceSettings)) break;
                    int eventIndex = nextIndex++;
                    examined++;
                    if (classifySourceAtDispatch)
                    {
                        if (MarkSourceFiltered(eventIndex, filtered)) continue;
                    }
                    else if (filtered.Any && filtered.Contains(eventIndex)) continue;
                    long intendedMicroseconds;
                    MidiEventView midiEvent = _events.GetSequential(eventIndex, ref cursor, out intendedMicroseconds);
                    PublishImmediateInService(nextIndex, eventIndex);
                    SendScheduledEvent(midiEvent, trackChannels);
                    if (eligiblePending > 0) eligiblePending--;
                    long elapsedTicks = Stopwatch.GetTimestamp() - stopwatchAtBatchStart;
                    long actualTicks = transportAtBatchStart + elapsedTicks;
                    currentLag = Math.Max(0, TicksToMicroseconds(actualTicks) - intendedMicroseconds);
                    if (currentLag > maximumLag) maximumLag = currentLag;
                    lastTimeline = intendedMicroseconds;
                    sent++;
                    if (elapsedTicks >= Stopwatch.Frequency / 125) break;
                }
            }
            finally
            {
                if (sent > 0)
                {
                    lock (_sync)
                    {
                        _processedEvents += sent;
                        _lastDispatchedMicroseconds = lastTimeline;
                        _currentLagMicroseconds = currentLag;
                        if (maximumLag > _maximumLagMicroseconds) _maximumLagMicroseconds = maximumLag;
                    }
                }
                PublishChannelState(false);
            }
        }

        private void UpdateQueue(long queue, bool inService)
        {
            lock (_sync)
            {
                _queueLength = queue;
                _outstandingEvents = queue + (inService ? 1 : 0);
                if (_outstandingEvents > _maximumQueueLength) _maximumQueueLength = _outstandingEvents;
            }
            PublishChannelState(false);
        }

        private void PublishStructuralDueWork(int nextArrival, long pending,
            long oldestPendingDueMicroseconds, bool inService,
            long inServiceDueMicroseconds, long futureDueMicroseconds,
            int futureDueCount)
        {
            lock (_sync)
            {
                _hasPublishedStructuralDueWork = true;
                _publishedStructuralNextArrival = nextArrival;
                _publishedStructuralPending = Math.Max(0, pending);
                _publishedStructuralOldestDueMicroseconds = pending > 0
                    ? oldestPendingDueMicroseconds : Int64.MaxValue;
                _publishedStructuralInService = inService;
                _publishedStructuralInServiceDueMicroseconds = inService
                    ? inServiceDueMicroseconds : Int64.MaxValue;
                _publishedStructuralFutureDueMicroseconds = futureDueMicroseconds;
                _publishedStructuralFutureCount = Math.Max(0, futureDueCount);
                _queueLength = _publishedStructuralPending;
                _outstandingEvents = _queueLength + (inService ? 1 : 0);
                if (_outstandingEvents > _maximumQueueLength)
                    _maximumQueueLength = _outstandingEvents;
            }
            PublishChannelState(false);
        }

        private void PublishImmediateInService(int nextSource, int eventIndex)
        {
            lock (_sync)
            {
                _publishedInService = true;
                _publishedInServiceEvent = eventIndex;
                _hasPublishedQueue = true;
                if (_mode == ProcessingMode.Queue)
                    _publishedNextProcess = nextSource;
                else
                {
                    _publishedDropNextArrival = nextSource;
                    _publishedDropPending = 0;
                    _publishedOldestPendingEvent = -1;
                }
                _queueLength = 0;
                _outstandingEvents = 1;
                if (_maximumQueueLength < 1) _maximumQueueLength = 1;
            }
            PublishChannelState(false);
        }

        private void PublishDropQueue(int nextArrival, int pending, bool inService)
        {
            lock (_sync)
            {
                _publishedDropNextArrival = nextArrival;
                _publishedDropPending = Math.Max(0, pending);
                _publishedInService = inService;
                if (!inService) _publishedInServiceEvent = -1;
                _publishedOldestPendingEvent = -1;
                _hasPublishedQueue = true;
                _queueLength = _publishedDropPending;
                _outstandingEvents = _queueLength + (inService ? 1 : 0);
                if (_outstandingEvents > _maximumQueueLength) _maximumQueueLength = _outstandingEvents;
            }
            PublishChannelState(false);
        }

        private void PublishDropQueue(int nextArrival, PendingMidiQueue pending, bool inService,
            long logicalNowMicroseconds)
        {
            int oldest = pending.Count == 0 ? -1 : pending.OldestEventIndex;
            lock (_sync)
            {
                _publishedDropNextArrival = nextArrival;
                _publishedDropPending = pending.Count;
                _publishedInService = inService;
                if (!inService) _publishedInServiceEvent = -1;
                _publishedOldestPendingEvent = oldest;
                _hasPublishedQueue = true;
                _queueLength = pending.Count;
                _outstandingEvents = _queueLength + (inService ? 1 : 0);
                if (_outstandingEvents > _maximumQueueLength) _maximumQueueLength = _outstandingEvents;
                _queueAgeMicroseconds = oldest < 0 ? 0 : Math.Max(0,
                    logicalNowMicroseconds - _events[oldest].IntendedMicroseconds);
                if (_queueAgeMicroseconds > _maximumQueueAgeMicroseconds)
                    _maximumQueueAgeMicroseconds = _queueAgeMicroseconds;
            }
            PublishChannelState(false);
        }

        private bool QueueAgeExceeded(PendingMidiQueue pending, long logicalNowMicroseconds, long limit)
        {
            if (pending.Count == 0) return false;
            return logicalNowMicroseconds - _events[pending.OldestEventIndex].IntendedMicroseconds > limit;
        }

        // One source-of-truth measurement for work that is already eligible
        // but sits outside a mode's structural queue. The mode publisher
        // supplies admitted/in-service transitions; this method adds only
        // newly due source work and a structural deadline once it is due.
        private void MeasurePublishedDueWork(int nextSource, long admittedPending,
            int admittedOldestEvent, long admittedOldestDueMicroseconds,
            bool inService, long inServiceDueMicroseconds,
            long structuralDueMicroseconds, int structuralDueCount,
            long playbackTicks)
        {
            int dueEnd = _preservedSeekRequests.Count == 0
                ? FindFirstEventAfterTransport(nextSource, playbackTicks)
                : nextSource;
            long newlyDue = CountSnapshotEligible(nextSource, dueEnd);
            long playbackMicroseconds = TicksToMicroseconds(playbackTicks);
            long dueStructural = structuralDueCount > 0 &&
                structuralDueMicroseconds <= playbackMicroseconds
                ? structuralDueCount : 0;
            _queueLength = Math.Max(0, admittedPending + newlyDue + dueStructural);
            _outstandingEvents = _queueLength + (inService ? 1 : 0);
            if (_outstandingEvents > _maximumQueueLength)
                _maximumQueueLength = _outstandingEvents;

            long oldest = Int64.MaxValue;
            if (inService && inServiceDueMicroseconds != Int64.MaxValue)
                oldest = Math.Min(oldest, inServiceDueMicroseconds);
            if (admittedPending > 0)
            {
                if (admittedOldestEvent >= 0)
                    oldest = Math.Min(oldest,
                        _events[admittedOldestEvent].IntendedMicroseconds);
                if (admittedOldestDueMicroseconds != Int64.MaxValue)
                    oldest = Math.Min(oldest, admittedOldestDueMicroseconds);
            }
            if (_snapshotFirstEligibleIndex >= 0)
                oldest = Math.Min(oldest,
                    _events[_snapshotFirstEligibleIndex].IntendedMicroseconds);
            if (dueStructural > 0)
                oldest = Math.Min(oldest, structuralDueMicroseconds);
            _queueAgeMicroseconds = oldest == Int64.MaxValue ? 0 :
                Math.Max(0, playbackMicroseconds - oldest);
            if (_queueAgeMicroseconds > _maximumQueueAgeMicroseconds)
                _maximumQueueAgeMicroseconds = _queueAgeMicroseconds;
        }

        private long PublishedInServiceDueMicroseconds()
        {
            return _publishedInServiceEvent >= 0 && _publishedInServiceEvent < _events.Count
                ? _events[_publishedInServiceEvent].IntendedMicroseconds : Int64.MaxValue;
        }

        // A synchronous native Send can block while source time continues.  The
        // worker cannot admit those newly-due events until Send returns, so the
        // snapshot counts that exact candidate range.  Filtered ranges are
        // cached and extended: an unchanged blocked range is never rescanned on
        // every UI refresh.
        private long CountSnapshotEligible(int startInclusive, int endExclusive)
        {
            if (startInclusive >= endExclusive)
            {
                _snapshotFirstEligibleIndex = -1;
                return 0;
            }
            if (!HasActiveSourceFilters())
            {
                _snapshotFirstEligibleIndex = startInclusive;
                return endExclusive - startInclusive;
            }

            int routingGeneration = _channelRouting.FilterGeneration;
            int overrideGeneration = _channelOverrides.FilterGeneration;
            int scanStart;
            long eligible;
            if (_snapshotFilterStart == startInclusive && _snapshotFilterEnd >= startInclusive &&
                _snapshotFilterEnd <= endExclusive && _snapshotRoutingGeneration == routingGeneration &&
                _snapshotOverrideGeneration == overrideGeneration)
            {
                scanStart = _snapshotFilterEnd;
                eligible = _snapshotEligibleCount;
            }
            else
            {
                scanStart = startInclusive;
                eligible = 0;
                _snapshotFirstEligibleIndex = -1;
                _snapshotFilterStart = startInclusive;
                _snapshotRoutingGeneration = routingGeneration;
                _snapshotOverrideGeneration = overrideGeneration;
            }

            for (int index = scanStart; index < endExclusive; index++)
            {
                MidiEventView midiEvent = _events[index];
                if (_channelRouting.ShouldFilter(midiEvent)) continue;
                ChannelAttribute attribute;
                int forcedValue;
                if (_channelOverrides.ShouldSuppress(midiEvent, out attribute, out forcedValue)) continue;
                if (_snapshotFirstEligibleIndex < 0) _snapshotFirstEligibleIndex = index;
                eligible++;
            }
            _snapshotFilterEnd = endExclusive;
            _snapshotEligibleCount = eligible;
            return eligible;
        }

        private void RecordChannelDrop(int eventIndex)
        {
            if (Volatile.Read(ref _channelMonitoringEnabled) == 0) return;
            _channelState.RecordDropped(_events[eventIndex]);
            PublishChannelState(false);
        }

        private void PublishChannelState(bool force)
        {
            if (Volatile.Read(ref _channelMonitoringEnabled) == 0) return;
            long now = Stopwatch.GetTimestamp();
            long previous = Interlocked.Read(ref _lastChannelPublishStamp);
            if (!force && previous != 0 && now - previous < Stopwatch.Frequency / 60) return;
            Interlocked.Exchange(ref _lastChannelPublishStamp, now);
            ChannelPlaybackSnapshot snapshot = _channelState.CreateSnapshot();
            _channelOverrides.ApplyToSnapshot(snapshot);
            _channelRouting.ApplyToSnapshot(snapshot);
            _publishedChannelState = snapshot;
        }

        private bool SendScheduledEvent(MidiEventView midiEvent, bool trackChannels)
        {
            long providerSendStart;
            long providerSendEnd;
            return SendScheduledEvent(midiEvent, trackChannels,
                out providerSendStart, out providerSendEnd);
        }

        private bool SendScheduledEvent(MidiEventView midiEvent, bool trackChannels,
            out long providerSendStart, out long providerSendEnd)
        {
            providerSendStart = CurrentTransportTicks();
            bool pacedHold = BeginOutputPacedHold();
            try { _output.Send(midiEvent); }
            finally { EndOutputPacedHold(pacedHold); }
            providerSendEnd = CurrentTransportTicks();
            RecordOutputRateDispatch(midiEvent,
                TicksToMicroseconds(Stopwatch.GetTimestamp()), false);
            SourceDispatchTrace trace = _sourceDispatchTrace;
            if (trace != null && midiEvent.EventIndex >= 0)
                trace.RecordSource(midiEvent.EventIndex, midiEvent, DescribeProcessingPath(_mode),
                    _sourceDispatchTraceSession, _channelOverrides.FilterGeneration,
                    false, true, "Sent", null,
                    ChannelOverrideState.AutoValue);
            if (trackChannels) _channelState.RecordSuccessful(midiEvent);
            return true;
        }

        private void SendProtectedEvent(MidiEventView midiEvent)
        {
            SendProtectedEvent(_output, midiEvent);
        }

        private void SendProtectedEvent(IMidiOutput output, MidiEventView midiEvent)
        {
            bool pacedHold = BeginOutputPacedHold();
            try { output.Send(midiEvent); }
            finally { EndOutputPacedHold(pacedHold); }
            if (_activeOutputRateGate != null)
                lock (_sync) _outputRateSafetyBypassMessages++;
        }

        private void SendRateCountedEssentialEvent(IMidiOutput output, MidiEventView midiEvent)
        {
            bool pacedHold = BeginOutputPacedHold();
            try { output.Send(midiEvent); }
            finally { EndOutputPacedHold(pacedHold); }
            OutputRateGate gate = _activeOutputRateGate;
            if (gate != null && gate.RecordExceptionalDispatch(
                TicksToMicroseconds(Stopwatch.GetTimestamp())))
                lock (_sync) _outputRateSafetySoftExcess++;
        }

        private void SendRateCountedEssentialEvent(MidiEventView midiEvent)
        {
            SendRateCountedEssentialEvent(_output, midiEvent);
        }

        private void RecordOutputRateDispatch(MidiEventView midiEvent,
            long dispatchTimestamp, bool protectedTraffic)
        {
            OutputRateGate gate = _activeOutputRateGate;
            if (gate == null || !gate.RecordDispatch(dispatchTimestamp, protectedTraffic)) return;
            lock (_sync) _outputRateSafetySoftExcess++;
        }

        private void RetireDropFilteredBacklog(FilteredSourceEvents filtered, PendingMidiQueue pending,
            CompleteNoteTracker completeNotes,
            ref int inService, ref long completionTicks, long now,
            ref ServiceDurationClock serviceClock, ref ServiceDurationClock beforeCurrentService)
        {
            bool changed = false;
            if (inService >= 0 && MarkSourceFiltered(inService, filtered))
            {
                if (CompleteNoteTracker.Classify(_events[inService]) == CompleteNoteEventKind.NoteOn)
                    completeNotes.MarkUnsentAttackFiltered(inService);
                inService = -1;
                completionTicks = Int64.MaxValue;
                serviceClock = beforeCurrentService;
                changed = true;
            }
            int pendingCount = pending.Count;
            for (int i = 0; i < pendingCount; i++)
            {
                PendingMidiQueue.Entry entry = pending.DequeueEntry();
                int eventIndex = entry.EventIndex;
                if (MarkSourceFiltered(eventIndex, filtered))
                {
                    if (CompleteNoteTracker.Classify(_events[eventIndex]) == CompleteNoteEventKind.NoteOn)
                        completeNotes.MarkUnsentAttackFiltered(eventIndex);
                    changed = true;
                }
                else pending.Enqueue(entry);
            }
            if (inService < 0 && pending.Count > 0)
            {
                inService = pending.Dequeue();
                beforeCurrentService = serviceClock;
                completionTicks = checked(now +
                    MicrosecondsToTicks(NextPlaybackServiceMicroseconds(ref serviceClock,
                        _events[inService])));
            }
            if (changed)
            {
                UpdateQueue(pending.Count, inService >= 0);
                PublishChannelState(true);
            }
        }

        private bool HasActiveSourceFilters()
        {
            return _channelRouting.AnyDisabled || _channelOverrides.AnyOverrides;
        }

        private bool MarkSourceFiltered(int eventIndex, FilteredSourceEvents filtered)
        {
            if (filtered.Contains(eventIndex)) return true;
            return MarkNewSourceFiltered(eventIndex, filtered);
        }

        private bool MarkNewSourceFiltered(int eventIndex, FilteredSourceEvents filtered)
        {
            MidiEventView midiEvent = _events[eventIndex];
            if (_channelRouting.ShouldFilter(midiEvent))
            {
                filtered.Add(eventIndex);
                _channelRouting.RecordFiltered(midiEvent.Channel);
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                SourceDispatchTrace trace = _sourceDispatchTrace;
                if (trace != null)
                    trace.RecordSource(eventIndex, midiEvent, DescribeProcessingPath(_mode),
                        _sourceDispatchTraceSession, _channelOverrides.FilterGeneration,
                        true, false, "ChannelDisabled", null,
                        ChannelOverrideState.AutoValue);
                return true;
            }
            ChannelAttribute attribute;
            int forcedValue;
            if (_channelOverrides.ShouldSuppress(midiEvent, out attribute, out forcedValue))
            {
                filtered.Add(eventIndex);
                _channelOverrides.RecordSuppressed(midiEvent.Channel);
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                SourceDispatchTrace trace = _sourceDispatchTrace;
                if (trace != null)
                    trace.RecordSource(eventIndex, midiEvent, DescribeProcessingPath(_mode),
                        _sourceDispatchTraceSession, _channelOverrides.FilterGeneration,
                        true, false, "ForcedOverride",
                        attribute.ToString(), forcedValue);
                return true;
            }
            return false;
        }

        private void AdmitUnlimitedRange(int startInclusive, int endExclusive, FilteredSourceEvents filtered,
            ref long eligiblePending)
        {
            if (startInclusive >= endExclusive) return;
            if (!HasActiveSourceFilters())
            {
                eligiblePending += endExclusive - startInclusive;
                return;
            }
            for (int index = startInclusive; index < endExclusive; index++)
                if (!MarkNewSourceFiltered(index, filtered)) eligiblePending++;
            PublishChannelState(false);
        }

        private void RetireUnlimitedFilteredBacklog(int nextProcess, int nextArrival, FilteredSourceEvents filtered,
            ref long eligiblePending, ref int inService, ref long completionTicks, ref long lastCompletionTicks,
            long now, ref ServiceDurationClock serviceClock, ref ServiceDurationClock beforeCurrentService)
        {
            bool changed = false;
            if (inService >= 0 && MarkSourceFiltered(inService, filtered))
            {
                inService = -1;
                completionTicks = 0;
                lastCompletionTicks = now;
                serviceClock = beforeCurrentService;
                changed = true;
            }
            for (int index = nextProcess; index < nextArrival; index++)
            {
                if (filtered.Contains(index)) continue;
                if (MarkSourceFiltered(index, filtered))
                {
                    if (eligiblePending > 0) eligiblePending--;
                    changed = true;
                }
            }
            if (changed) PublishChannelState(true);
        }

        private int TakeNextEligible(ref int nextProcess, int nextArrival, FilteredSourceEvents filtered)
        {
            while (nextProcess < nextArrival)
            {
                int candidate = nextProcess++;
                if (!filtered.Contains(candidate)) return candidate;
            }
            return -1;
        }

        private void ApplyPendingOverrides()
        {
            if (!_channelOverrides.AnyOverrides || _output == null) return;
            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
            bool applied = false;
            for (int channel = 0; channel < 16; channel++)
            {
                int mask = _channelOverrides.TakePendingMask(channel);
                if (!_channelRouting.IsEnabled(channel))
                {
                    _channelOverrides.Requeue(channel, mask);
                    continue;
                }
                while (mask != 0)
                {
                    int bit = mask & -mask;
                    int attributeIndex = 0;
                    int scan = bit;
                    while ((scan >>= 1) != 0) attributeIndex++;
                    ChannelAttribute attribute = (ChannelAttribute)attributeIndex;
                    int value = _channelOverrides.GetValue(channel, attribute);
                    mask &= ~bit;
                    if (value == ChannelOverrideState.AutoValue) continue;
                    try
                    {
                        MidiEvent forcedMessage = ChannelOverrideState.CreateMessage(channel, attribute, value);
                        SendRateCountedEssentialEvent(forcedMessage);
                        RecordGeneratedSourceTrace(forcedMessage, "ForcedOverride", attribute.ToString());
                        if (trackChannels)
                        {
                            _channelState.RecordOverrideApplied(channel, attribute, value);
                            // The UI may clear or replace this override after
                            // Send returns but before its observation is
                            // published. Preserve the wire value as historical
                            // unless it is still the requested forced value.
                            if (_channelOverrides.GetValue(channel, attribute) != value)
                                _channelState.MarkAttributeHistoricalDirect(channel, attribute);
                        }
                        applied = true;
                    }
                    catch
                    {
                        _channelOverrides.Requeue(channel, mask | bit);
                        throw;
                    }
                }
            }
            if (applied) PublishChannelState(true);
        }

        private void ApplyPendingStateChase()
        {
            int eventIndexExclusive = Interlocked.Exchange(ref _pendingStateChaseEventIndex, -1);
            if (eventIndexExclusive < 0 || Volatile.Read(ref _chaseMidiStateOnPlaySeek) == 0) return;
            MidiSong song;
            IMidiOutput output;
            lock (_sync) { song = _song; output = _output; }
            if (song == null || output == null) return;
            IList<MidiEvent> messages = song.GetMidiStateChaseIndex().CreateMessages(eventIndexExclusive,
                _channelRouting, _channelOverrides);
            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
            for (int i = 0; i < messages.Count; i++)
            {
                if (!IsActive()) return;
                MidiEventView message = MidiEventView.FromEvent(messages[i], -1);
                SendRateCountedEssentialEvent(output, message);
                RecordGeneratedSourceTrace(message.ToCompatibilityEvent(), "StateChase", "PlaySeek");
                if (trackChannels) _channelState.RecordSuccessful(message);
            }
            if (messages.Count != 0) PublishChannelState(true);
        }

        private void PublishChannelStateBeforeWait(long targetTicks, long nowTicks)
        {
            if (Volatile.Read(ref _channelMonitoringEnabled) == 0) return;
            // A short burst can finish within the normal publication interval
            // and then wait for a sparse future event. Publish that completed
            // burst before sleeping so the monitor does not remain stale for
            // the entire sparse gap. High-rate service loops still use the
            // ordinary bounded publication cadence.
            if (targetTicks > nowTicks + Stopwatch.Frequency / 60)
                PublishChannelState(true);
            else
                PublishChannelState(false);
        }

        private void PublishUnlimitedQueue(int nextProcess, long eligiblePending, bool inService)
        {
            lock (_sync)
            {
                _publishedNextProcess = nextProcess;
                _publishedInService = inService;
                if (!inService) _publishedInServiceEvent = -1;
                _hasPublishedQueue = true;
                _queueLength = Math.Max(0, eligiblePending);
                _outstandingEvents = _queueLength + (inService ? 1 : 0);
                _maximumQueueLength = Math.Max(_maximumQueueLength, _outstandingEvents);
            }
        }

        private void SetCurrentLag(long lag)
        {
            lock (_sync) _currentLagMicroseconds = lag;
        }

        private string DescribeProcessingPath(ProcessingMode mode)
        {
            if (mode != ProcessingMode.Drop) return mode.ToString();
            string path = "Drop/" + ((OverflowPolicy)Volatile.Read(ref _overflowPolicy)).ToString();
            if (_virtualForwardDropActive) path += "/VirtualForward";
            return path;
        }

        private void TraceSourceBoundary(string boundary, string detail)
        {
            SourceDispatchTrace trace = _sourceDispatchTrace;
            if (trace != null)
                trace.RecordBoundary(boundary, detail, DescribeProcessingPath(_mode),
                    _sourceDispatchTraceSession, _channelOverrides.FilterGeneration);
        }

        private void RecordGeneratedSourceTrace(MidiEvent midiEvent, string origin, string detail)
        {
            SourceDispatchTrace trace = _sourceDispatchTrace;
            if (trace == null || midiEvent == null) return;
            trace.RecordGenerated(MidiEventView.FromEvent(midiEvent, -1), origin, detail,
                DescribeProcessingPath(_mode), _sourceDispatchTraceSession,
                _channelOverrides.FilterGeneration);
        }

        private void TraceAcceptedAdmission(int eventIndex, long decisionTicks, int clusterSize, int bufferOccupancy, int maximumBufferOccupancy, long busyUntilTicks)
        {
            DropTraceRecorder trace;
            lock (_sync) trace = _dropTrace;
            if (trace != null)
                trace.RecordAcceptedAdmission(eventIndex, _events[eventIndex], TicksToMicroseconds(decisionTicks), clusterSize, bufferOccupancy, maximumBufferOccupancy, TicksToMicroseconds(busyUntilTicks));
        }

        private void TraceService(int eventIndex, long serviceStartTicks, long serviceEndTicks, long busyUntilTicks)
        {
            DropTraceRecorder trace;
            lock (_sync) trace = _dropTrace;
            if (trace != null)
                trace.RecordService(eventIndex, TicksToMicroseconds(serviceStartTicks), TicksToMicroseconds(serviceEndTicks), TicksToMicroseconds(busyUntilTicks));
        }

        private void TraceDropped(int eventIndex, long decisionTicks, string reason, int clusterSize, int consecutiveDrops, long busyUntilTicks, int bufferOccupancy, int maximumBufferOccupancy)
        {
            DropTraceRecorder trace;
            lock (_sync) trace = _dropTrace;
            if (trace != null)
                trace.RecordDropped(eventIndex, _events[eventIndex], TicksToMicroseconds(decisionTicks), reason, clusterSize, consecutiveDrops, TicksToMicroseconds(busyUntilTicks), bufferOccupancy, maximumBufferOccupancy);
        }

        private int ClusterSizeAt(int eventIndex)
        {
            long timestamp = _events[eventIndex].IntendedMicroseconds;
            int first = eventIndex;
            int last = eventIndex + 1;
            while (first > 0 && _events[first - 1].IntendedMicroseconds == timestamp) first--;
            while (last < _events.Count && _events[last].IntendedMicroseconds == timestamp) last++;
            return last - first;
        }

        private long EstimateBusyUntil(long currentCompletionTicks,
            System.Collections.Generic.IEnumerable<int> queuedEvents, ServiceDurationClock serviceClock)
        {
            if (currentCompletionTicks == Int64.MaxValue) return 0;
            if (Volatile.Read(ref _simulateSlowdown) == 0) return currentCompletionTicks;
            long busyUntil = currentCompletionTicks;
            foreach (int eventIndex in queuedEvents)
                busyUntil = checked(busyUntil +
                    MicrosecondsToTicks(serviceClock.NextMicroseconds(_events[eventIndex])));
            return busyUntil;
        }

        private ServiceDurationClock CreateServiceClock()
        {
            return Volatile.Read(ref _serviceSettings).CreateClock();
        }

        private long NextPlaybackServiceMicroseconds(ref ServiceDurationClock clock, MidiEventView midiEvent)
        {
            // Source filtering, tracing, and finite-queue bookkeeping must not
            // reactivate the remembered service model while Rate is None.
            return Volatile.Read(ref _simulateSlowdown) == 0 ? 0 : clock.NextMicroseconds(midiEvent);
        }

        private void UpdateServiceClock(ref ServiceDurationClock clock, ref ServiceDurationSettings settings)
        {
            ServiceDurationSettings current = Volatile.Read(ref _serviceSettings);
            if (Object.ReferenceEquals(current, settings)) return;
            settings = current;
            // A new model starts with a fresh fractional phase at the next
            // service start. Existing in-service and virtual entries retain
            // their assigned completion times.
            clock = current.CreateClock();
        }

        private bool IsEffectiveZeroService()
        {
            if (Volatile.Read(ref _simulateSlowdown) == 0) return true;
            return Volatile.Read(ref _serviceSettings).IsImmediate;
        }

        private bool WaitWhilePaused()
        {
            while (true)
            {
                PlaybackState state;
                lock (_sync) state = _state;
                if (state == PlaybackState.Playing) return true;
                if (state != PlaybackState.Paused) return false;
                AcknowledgePreservedPause();
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                _wake.Reset();
                if (wakeGeneration != Volatile.Read(ref _wakeGeneration)) continue;
                lock (_sync) state = _state;
                if (state == PlaybackState.Paused) _wake.WaitOne();
            }
        }

        private void AcknowledgePreservedPause()
        {
            bool signal = false;
            lock (_sync)
            {
                if (_state == PlaybackState.Paused && PreservePlaybackQueueOnPauseSeek &&
                    _pauseAcknowledgedGeneration != _pauseRequestGeneration)
                {
                    _pauseAcknowledgedGeneration = _pauseRequestGeneration;
                    signal = true;
                }
            }
            if (signal) _pauseAcknowledged.Set();
        }

        private void SignalWake()
        {
            Interlocked.Increment(ref _wakeGeneration);
            _wake.Set();
        }

        private void WaitUntil(HighResolutionWaiter waiter, long targetTicks, int iterationWakeGeneration)
        {
            while (IsActive())
            {
                _wake.Reset();
                if (iterationWakeGeneration != Volatile.Read(ref _wakeGeneration)) return;
                if (!IsPlaying()) return;
                long remainingTicks = targetTicks - CurrentTransportTicks();
                if (remainingTicks <= 0) return;
                long remainingUs = Math.Max(1, TicksToMicroseconds(remainingTicks));
                if (!waiter.WaitMicroseconds(remainingUs)) return;

                // The waitable timer deliberately wakes about 200 us early.
                SpinWait spin = new SpinWait();
                while (targetTicks > CurrentTransportTicks() && IsPlaying())
                {
                    if (_wake.WaitOne(0)) return;
                    spin.SpinOnce();
                }
                return;
            }
        }

        private bool IsActive()
        {
            lock (_sync) return _state == PlaybackState.Playing || _state == PlaybackState.Paused;
        }

        private bool IsPlaying()
        {
            lock (_sync) return _state == PlaybackState.Playing;
        }

        private bool ShouldRetireWorkerAtDispatchBoundary()
        {
            if (Volatile.Read(ref _dispatchSuspended) == 0 && IsPlaying()) return false;
            lock (_sync)
                return !(_state == PlaybackState.Paused && PreservePlaybackQueueOnPauseSeek);
        }

        private long CurrentTransportTicks()
        {
            lock (_sync) return CurrentTransportTicksLocked();
        }

        private long CurrentTransportTicksLocked()
        {
            if (_state == PlaybackState.Playing)
            {
                if (_outputPacedHoldDepth > 0)
                    return _outputPacedHoldTransportTicks;
                return _transportBaseTicks + Stopwatch.GetTimestamp() - _runStartStamp;
            }
            return _transportBaseTicks;
        }

        private void FreezeTransportLocked()
        {
            _transportBaseTicks = CurrentTransportTicksLocked();
            _runStartStamp = Stopwatch.GetTimestamp();
            _outputPacedHoldDepth = 0;
            _outputPacedHoldStartStamp = 0;
            _outputPacedHoldTransportTicks = 0;
        }

        private bool BeginOutputPacedHold()
        {
            if (Volatile.Read(ref _outputPacedTimeline) == 0) return false;
            lock (_sync)
            {
                if (_state != PlaybackState.Playing) return false;
                if (_outputPacedHoldDepth == 0)
                {
                    long now = Stopwatch.GetTimestamp();
                    _outputPacedHoldTransportTicks = _transportBaseTicks + now - _runStartStamp;
                    _outputPacedHoldStartStamp = now;
                }
                _outputPacedHoldDepth++;
                return true;
            }
        }

        private void EndOutputPacedHold(bool entered)
        {
            if (!entered) return;
            lock (_sync)
            {
                if (_outputPacedHoldDepth <= 0) return;
                _outputPacedHoldDepth--;
                if (_outputPacedHoldDepth != 0) return;
                if (_state == PlaybackState.Playing)
                    _runStartStamp = SaturatingAddTicks(_runStartStamp,
                        Math.Max(0, Stopwatch.GetTimestamp() - _outputPacedHoldStartStamp));
                _outputPacedHoldStartStamp = 0;
                _outputPacedHoldTransportTicks = 0;
            }
        }

        private void StartOutputPacedServiceHold(long serviceTicks,
            ref bool entered, ref long completionStamp)
        {
            entered = BeginOutputPacedHold();
            completionStamp = entered
                ? SaturatingAddTicks(Stopwatch.GetTimestamp(), Math.Max(0, serviceTicks))
                : Int64.MaxValue;
        }

        private long EventTicks(int eventIndex)
        {
            return MicrosecondsToTicks(_events[eventIndex].IntendedMicroseconds);
        }

        internal static long MicrosecondsToTicks(long microseconds)
        {
            const long scale = 1000000L;
            long frequency = Stopwatch.Frequency;
            if (frequency <= Int64.MaxValue / scale)
                return checked((microseconds / scale) * frequency +
                    ((microseconds % scale) * frequency) / scale);
            return (long)(((decimal)microseconds * frequency) / scale);
        }

        internal static long TicksToMicroseconds(long ticks)
        {
            const long scale = 1000000L;
            long frequency = Stopwatch.Frequency;
            if (frequency <= Int64.MaxValue / scale)
                return checked((ticks / frequency) * scale +
                    ((ticks % frequency) * scale) / frequency);
            return (long)(((decimal)ticks * scale) / frequency);
        }

        private void ResetStatisticsLocked()
        {
            _queueLength = 0;
            _outstandingEvents = 0;
            _maximumQueueLength = 0;
            _queueAgeMicroseconds = 0;
            _maximumQueueAgeMicroseconds = 0;
            _processedEvents = 0;
            _droppedEvents = 0;
            _gateFilteredEvents = 0;
            _musicalLimitRejectedAttacks = 0;
            _musicalLimitSuppressedReleases = 0;
            _outputRateRejectedAttacks = 0;
            _outputRateSuppressedReleases = 0;
            _outputRateRejectedOtherMessages = 0;
            _outputRateSafetySoftExcess = 0;
            _outputRateSafetyBypassMessages = 0;
            _virtualQueueLength = 0;
            _virtualOutstandingEvents = 0;
            _virtualMaximumQueueLength = 0;
            _lastDispatchedMicroseconds = 0;
            _effectiveSpeedFrontierMicroseconds = 0;
            _currentLagMicroseconds = 0;
            _maximumLagMicroseconds = 0;
            _snapshotFilterStart = -1;
            _snapshotFilterEnd = -1;
            _snapshotEligibleCount = 0;
            _snapshotFirstEligibleIndex = -1;
            _publishedOldestPendingEvent = -1;
        }

        private static long ClampPosition(MidiSong song, long microseconds)
        {
            if (microseconds < 0) return 0;
            if (microseconds > song.DurationMicroseconds) return song.DurationMicroseconds;
            return microseconds;
        }

        private static int FindFirstEventAtOrAfter(MidiSong song, long microseconds)
        {
            return song.EventStore.LowerBoundByTime(microseconds);
        }

        private int FindFirstEventAfterTransport(int startIndex, long transportTicks)
        {
            int low = Math.Max(0, startIndex);
            int high = _events.Count;
            while (low < high)
            {
                int middle = low + ((high - low) / 2);
                if (EventTicks(middle) <= transportTicks) low = middle + 1;
                else high = middle;
            }
            return low;
        }

        public void Dispose()
        {
            Unload();
            _wake.Dispose();
            _pauseAcknowledged.Dispose();
        }

        private sealed class PreservedSeekRequest
        {
            internal readonly int Generation;
            internal readonly long TargetMicroseconds;
            internal readonly int TargetEventIndex;

            internal PreservedSeekRequest(int generation, long targetMicroseconds, int targetEventIndex)
            {
                Generation = generation;
                TargetMicroseconds = targetMicroseconds;
                TargetEventIndex = targetEventIndex;
            }
        }

        // Sparse segmented bits retain exact decisions made at admission or a
        // live control boundary without allocating per-event objects or a
        // whole-song bitmap merely because one channel was briefly muted.
        private sealed class FilteredSourceEvents
        {
            private const int SegmentShift = 15;
            private const int SegmentEventCount = 1 << SegmentShift;
            private const int WordsPerSegment = SegmentEventCount / 32;
            private readonly Dictionary<int, uint[]> _segments = new Dictionary<int, uint[]>();

            internal bool Any { get { return _segments.Count != 0; } }

            internal bool Contains(int eventIndex)
            {
                uint[] words;
                if (!_segments.TryGetValue(eventIndex >> SegmentShift, out words)) return false;
                int within = eventIndex & (SegmentEventCount - 1);
                return (words[within >> 5] & (1u << (within & 31))) != 0;
            }

            internal void Add(int eventIndex)
            {
                int segment = eventIndex >> SegmentShift;
                uint[] words;
                if (!_segments.TryGetValue(segment, out words))
                {
                    words = new uint[WordsPerSegment];
                    _segments.Add(segment, words);
                }
                int within = eventIndex & (SegmentEventCount - 1);
                words[within >> 5] |= 1u << (within & 31);
            }
        }
    }

    internal sealed class SourceDispatchTraceEntry
    {
        internal long Sequence;
        internal long StopwatchTicks;
        internal long Session;
        internal int SourceEventIndex;
        internal string Payload;
        internal string ProcessingPath;
        internal int OverrideGeneration;
        internal bool SuppressionDecision;
        internal bool FinalSendDecision;
        internal string Outcome;
        internal string Origin;
        internal string ForcedAttribute;
        internal int ForcedValue;
        internal string Boundary;
        internal string Detail;
    }

    // Optional bounded-test/diagnostic recorder for the source-event terminal
    // invariant. Reset/Panic and transport boundaries append entries; none of
    // them clear earlier evidence.
    internal sealed class SourceDispatchTrace
    {
        private readonly object _sync = new object();
        private readonly List<SourceDispatchTraceEntry> _entries =
            new List<SourceDispatchTraceEntry>();
        private long _sequence;
        private long _nextSession;

        internal long BeginSession()
        {
            lock (_sync) return ++_nextSession;
        }

        internal void RecordSource(int eventIndex, MidiEventView midiEvent, string path,
            long session, int overrideGeneration, bool suppressed, bool sent, string outcome,
            string forcedAttribute, int forcedValue)
        {
            Add(new SourceDispatchTraceEntry
            {
                SourceEventIndex = eventIndex,
                Session = session,
                Payload = PayloadText(midiEvent),
                ProcessingPath = path,
                OverrideGeneration = overrideGeneration,
                SuppressionDecision = suppressed,
                FinalSendDecision = sent,
                Outcome = outcome,
                Origin = "Source",
                ForcedAttribute = forcedAttribute,
                ForcedValue = forcedValue
            });
        }

        internal void RecordGenerated(MidiEventView midiEvent, string origin, string detail,
            string path, long session, int overrideGeneration)
        {
            Add(new SourceDispatchTraceEntry
            {
                SourceEventIndex = -1,
                Session = session,
                Payload = PayloadText(midiEvent),
                ProcessingPath = path,
                OverrideGeneration = overrideGeneration,
                FinalSendDecision = true,
                Outcome = "GeneratedSend",
                Origin = origin,
                ForcedValue = ChannelOverrideState.AutoValue,
                Detail = detail
            });
        }

        internal void RecordBoundary(string boundary, string detail, string path,
            long session, int overrideGeneration)
        {
            Add(new SourceDispatchTraceEntry
            {
                SourceEventIndex = -1,
                Session = session,
                ProcessingPath = path,
                OverrideGeneration = overrideGeneration,
                Outcome = "Boundary",
                Origin = "Lifecycle",
                ForcedValue = ChannelOverrideState.AutoValue,
                Boundary = boundary,
                Detail = detail
            });
        }

        internal SourceDispatchTraceEntry[] Snapshot()
        {
            lock (_sync) return _entries.ToArray();
        }

        internal void SaveJsonLines(string path)
        {
            SourceDispatchTraceEntry[] entries = Snapshot();
            using (System.IO.StreamWriter writer = new System.IO.StreamWriter(path, false,
                new System.Text.UTF8Encoding(false)))
            {
                for (int index = 0; index < entries.Length; index++)
                {
                    SourceDispatchTraceEntry entry = entries[index];
                    writer.Write("{\"sequence\":"); writer.Write(entry.Sequence);
                    writer.Write(",\"stopwatch_ticks\":"); writer.Write(entry.StopwatchTicks);
                    writer.Write(",\"session\":"); writer.Write(entry.Session);
                    writer.Write(",\"source_event_index\":"); writer.Write(entry.SourceEventIndex);
                    WriteJsonString(writer, "payload", entry.Payload);
                    WriteJsonString(writer, "processing_path", entry.ProcessingPath);
                    writer.Write(",\"override_generation\":"); writer.Write(entry.OverrideGeneration);
                    writer.Write(",\"suppression_decision\":"); writer.Write(entry.SuppressionDecision ? "true" : "false");
                    writer.Write(",\"final_send_decision\":"); writer.Write(entry.FinalSendDecision ? "true" : "false");
                    WriteJsonString(writer, "outcome", entry.Outcome);
                    WriteJsonString(writer, "origin", entry.Origin);
                    WriteJsonString(writer, "forced_attribute", entry.ForcedAttribute);
                    if (entry.ForcedValue == ChannelOverrideState.AutoValue)
                        writer.Write(",\"forced_value\":null");
                    else
                    {
                        writer.Write(",\"forced_value\":");
                        writer.Write(entry.ForcedValue);
                    }
                    WriteJsonString(writer, "boundary", entry.Boundary);
                    WriteJsonString(writer, "detail", entry.Detail);
                    writer.WriteLine("}");
                }
            }
        }

        private void Add(SourceDispatchTraceEntry entry)
        {
            lock (_sync)
            {
                entry.Sequence = ++_sequence;
                entry.StopwatchTicks = Stopwatch.GetTimestamp();
                _entries.Add(entry);
            }
        }

        private static string PayloadText(MidiEventView midiEvent)
        {
            return midiEvent.IsValid ? BitConverter.ToString(midiEvent.Data.ToArray()) : null;
        }

        private static void WriteJsonString(System.IO.TextWriter writer, string name, string value)
        {
            writer.Write(",\""); writer.Write(name); writer.Write("\":");
            if (value == null) { writer.Write("null"); return; }
            writer.Write('"');
            for (int index = 0; index < value.Length; index++)
            {
                char character = value[index];
                if (character == '"' || character == '\\')
                {
                    writer.Write('\\'); writer.Write(character);
                }
                else if (character == '\r') writer.Write("\\r");
                else if (character == '\n') writer.Write("\\n");
                else if (character == '\t') writer.Write("\\t");
                else if (character < 0x20)
                    writer.Write("\\u" + ((int)character).ToString("X4",
                        System.Globalization.CultureInfo.InvariantCulture));
                else writer.Write(character);
            }
            writer.Write('"');
        }
    }

    internal enum ChannelControlKind
    {
        SetEnabled,
        Chase,
        MonitorStateChase
    }

    internal sealed class ChannelControlRequest
    {
        internal readonly ChannelControlKind Kind;
        internal readonly int Channel;
        internal readonly ChannelAttribute Attribute;
        internal readonly int Value;
        internal readonly MidiSong ExpectedSong;
        private readonly Action<Exception> _completion;
        private int _completed;

        internal ChannelControlRequest(ChannelControlKind kind, int channel, ChannelAttribute attribute, int value)
            : this(kind, channel, attribute, value, null)
        {
        }

        internal ChannelControlRequest(ChannelControlKind kind, int channel, ChannelAttribute attribute, int value,
            Action<Exception> completion)
        {
            Kind = kind; Channel = channel; Attribute = attribute; Value = value; _completion = completion;
        }

        internal ChannelControlRequest(MidiSong expectedSong, Action<Exception> completion)
        {
            Kind = ChannelControlKind.MonitorStateChase;
            ExpectedSong = expectedSong;
            Channel = -1;
            _completion = completion;
        }

        internal bool HasCompletion { get { return _completion != null; } }

        internal void Complete(Exception error)
        {
            if (Interlocked.Exchange(ref _completed, 1) != 0) return;
            Action<Exception> completion = _completion;
            if (completion == null) return;
            try { completion(error); }
            catch { }
        }
    }

    internal sealed class ChannelControlErrorEventArgs : EventArgs
    {
        internal readonly int Channel;
        internal readonly ChannelAttribute Attribute;
        internal readonly Exception Error;

        internal ChannelControlErrorEventArgs(int channel, ChannelAttribute attribute, Exception error)
        {
            Channel = channel; Attribute = attribute; Error = error;
        }
    }

    internal sealed class PlaybackErrorEventArgs : EventArgs
    {
        public readonly Exception Error;
        public PlaybackErrorEventArgs(Exception error) { Error = error; }
    }

    internal sealed class PlaybackWorkerTimeoutException : TimeoutException
    {
        public PlaybackWorkerTimeoutException(string message, Exception innerException)
            : base(message, innerException) { }
    }
}
