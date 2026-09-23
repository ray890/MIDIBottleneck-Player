using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class PlaybackEngine : IDisposable
    {
        internal const int DefaultQueueLengthLimit = 2000;
        internal const int DefaultDropBufferCapacity = DefaultQueueLengthLimit;
        private readonly object _sync = new object();
        private readonly EventWaitHandle _wake = new EventWaitHandle(false, EventResetMode.ManualReset);
        private int _wakeGeneration;
        private Thread _thread;
        private MidiSong _song;
        private MidiEventReader _events;
        private IMidiOutput _output;
        private ProcessingMode _mode;
        private int _startEventIndex;
        private PlaybackState _state = PlaybackState.Stopped;
        private long _processingMicroseconds;
        private long _midiBitrate = ServiceDurationCalculator.FivePinDinBitrate;
        private int _serviceDurationMode;
        private int _queueLengthLimit = DefaultQueueLengthLimit;
        private int _simulateSlowdown = 1;
        private int _applyQueueLimitWithoutSlowdown = 1;
        private int _chaseMidiStateOnPlaySeek = 1;
        private int _pendingStateChaseEventIndex = -1;
        private bool _virtualForwardDropActive;
        private int _overflowPolicy;
        private long _transportBaseTicks;
        private long _runStartStamp;
        private int _dispatchSuspended = 1;
        private bool _silenceWhenWorkerExits;
        private Exception _workerExitSilenceFailure;
        private int _workerStopTimeoutMilliseconds = 2000;

        private long _queueLength;
        private long _outstandingEvents;
        private long _maximumQueueLength;
        private long _processedEvents;
        private long _droppedEvents;
        private long _gateFilteredEvents;
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
        private bool _hasPublishedQueue;
        private int _snapshotFilterStart = -1;
        private int _snapshotFilterEnd = -1;
        private long _snapshotEligibleCount;
        private int _snapshotRoutingGeneration = -1;
        private int _snapshotOverrideGeneration = -1;
        private int _usesPreAdmissionFilterAccounting;
        private DropTraceRecorder _dropTrace;
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
            get { return Interlocked.Read(ref _processingMicroseconds); }
            set
            {
                if (value < 0) value = 0;
                Interlocked.Exchange(ref _processingMicroseconds, value);
                SignalWake();
            }
        }

        public ServiceDurationMode ServiceDurationMode
        {
            get { return (ServiceDurationMode)Volatile.Read(ref _serviceDurationMode); }
            set
            {
                Volatile.Write(ref _serviceDurationMode, (int)value);
                SignalWake();
            }
        }

        public long MidiBitrate
        {
            get { return Interlocked.Read(ref _midiBitrate); }
            set
            {
                if (value < 1) value = 1;
                Interlocked.Exchange(ref _midiBitrate, value);
                SignalWake();
            }
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

        public int QueueLengthLimit
        {
            get { return Volatile.Read(ref _queueLengthLimit); }
            set
            {
                if (value < 1) value = 1;
                if (value > 1000000) value = 1000000;
                Volatile.Write(ref _queueLengthLimit, value);
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
                _startEventIndex = FindFirstEventAtOrAfter(song, startMicroseconds);
                _pendingStateChaseEventIndex = ChaseMidiStateOnPlaySeek && _startEventIndex > 0
                    ? _startEventIndex : -1;
                _hasPublishedQueue = false;
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 0);
                _transportBaseTicks = MicrosecondsToTicks(startMicroseconds);
                _runStartStamp = Stopwatch.GetTimestamp();
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
            MidiSong song;
            PlaybackState previousState;

            lock (_sync)
            {
                song = _song;
                if (song == null) return;
                targetMicroseconds = ClampPosition(song, targetMicroseconds);
                previousState = _state;
            }

            StopWorkerForTransition("seeking", ChannelTransitionBoundary.Seek);

            lock (_sync)
            {
                _transportBaseTicks = MicrosecondsToTicks(targetMicroseconds);
                _startEventIndex = FindFirstEventAtOrAfter(song, targetMicroseconds);
                _pendingStateChaseEventIndex = ChaseMidiStateOnPlaySeek && _startEventIndex > 0
                    ? _startEventIndex : -1;
                _hasPublishedQueue = false;
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 0);
                ResetStatisticsLocked();
                _lastDispatchedMicroseconds = targetMicroseconds;
                _effectiveSpeedFrontierMicroseconds = targetMicroseconds;
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
            lock (_sync)
            {
                if (_state != PlaybackState.Playing) return;
                song = _song;
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
            Thread thread;
            IMidiOutput output;
            lock (_sync)
            {
                if (_state == PlaybackState.Playing)
                    _transportBaseTicks += Stopwatch.GetTimestamp() - _runStartStamp;
                _state = PlaybackState.Stopped;
                Volatile.Write(ref _dispatchSuspended, 1);
                thread = _thread;
                output = _output;
                _workerExitSilenceFailure = null;
                _silenceWhenWorkerExits = thread != null;
            }
            SignalWake();

            if (thread == null)
            {
                if (output != null) MidiOutputSafety.ResetAndSilence(output);
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
                _processedEvents = 0;
                _droppedEvents = 0;
                _gateFilteredEvents = 0;
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
                        _output.Send(message);
                        if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                            _channelState.RecordOverrideApplied(channel, attribute, value);
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
            if (queued) SignalWake();
            else
            {
                request.Complete(immediateError);
                if (immediateError != null && !request.HasCompletion) throw immediateError;
            }
            PublishChannelState(true);
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
                request.Complete(error);
                PublishChannelState(true);
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
                    _output.Send(ChannelOverrideState.CreateControllerMessage(request.Channel, 64, 0));
                    _output.Send(ChannelOverrideState.CreateControllerMessage(request.Channel, 120, 0));
                    _output.Send(ChannelOverrideState.CreateControllerMessage(request.Channel, 123, 0));
                }
                _channelRouting.SetEnabled(request.Channel, false);
                if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                    _channelState.SilenceChannelDirect(request.Channel);
                return;
            }
            if (_output == null) throw new InvalidOperationException("No MIDI output session is available.");
            if (!_channelRouting.IsEnabled(request.Channel))
                throw new InvalidOperationException("Enable this MIDI channel before restoring its source value.");
            _output.Send(ChannelOverrideState.CreateMessage(request.Channel, request.Attribute, request.Value));
            if (Volatile.Read(ref _channelMonitoringEnabled) != 0)
                _channelState.RecordManualChaseApplied(request.Channel, request.Attribute, request.Value);
        }

        internal void SetDropTrace(DropTraceRecorder recorder)
        {
            lock (_sync) _dropTrace = recorder;
        }

        public PlaybackSnapshot GetSnapshot()
        {
            lock (_sync)
            {
                long playbackTicks = CurrentTransportTicksLocked();
                // Source arrivals continue while an output call is blocked.
                // This is the application's unlimited backlog, not the synth's queue.
                if (_mode == ProcessingMode.Queue && _hasPublishedQueue &&
                    Volatile.Read(ref _usesPreAdmissionFilterAccounting) == 0 &&
                    (_state == PlaybackState.Playing || _state == PlaybackState.Paused))
                {
                    int due = FindFirstEventAfterTransport(_publishedNextProcess, playbackTicks);
                    _queueLength = Math.Max(0, due - _publishedNextProcess);
                    _outstandingEvents = _queueLength + (_publishedInService ? 1 : 0);
                    _maximumQueueLength = Math.Max(_maximumQueueLength, _outstandingEvents);
                }
                else if (_mode == ProcessingMode.Drop && _hasPublishedQueue &&
                    (_state == PlaybackState.Playing || _state == PlaybackState.Paused))
                {
                    int due = FindFirstEventAfterTransport(_publishedDropNextArrival, playbackTicks);
                    long newlyDue = CountSnapshotEligible(_publishedDropNextArrival, due);
                    _queueLength = Math.Max(0, _publishedDropPending + newlyDue);
                    _outstandingEvents = _queueLength + (_publishedInService ? 1 : 0);
                    _maximumQueueLength = Math.Max(_maximumQueueLength, _outstandingEvents);
                }
                long playbackUs = TicksToMicroseconds(playbackTicks);
                PlaybackSnapshot snapshot = new PlaybackSnapshot();
                snapshot.State = _state;
                snapshot.ProcessingMicroseconds = Interlocked.Read(ref _processingMicroseconds);
                snapshot.ServiceDurationMode = (ServiceDurationMode)Volatile.Read(ref _serviceDurationMode);
                snapshot.MidiBitrate = Interlocked.Read(ref _midiBitrate);
                snapshot.SimulateSlowdown = Volatile.Read(ref _simulateSlowdown) != 0;
                snapshot.QueueLengthLimitEnabled = _mode == ProcessingMode.Drop;
                snapshot.ProcessingMode = _mode;
                snapshot.QueueLengthLimit = Volatile.Read(ref _queueLengthLimit);
                snapshot.OverflowPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                snapshot.QueueLength = _queueLength;
                snapshot.OutstandingEvents = _outstandingEvents;
                snapshot.MaximumQueueLength = _maximumQueueLength;
                snapshot.ProcessedEvents = _processedEvents;
                snapshot.DroppedEvents = _droppedEvents;
                snapshot.GateFilteredEvents = _gateFilteredEvents;
                snapshot.VirtualQueueActive = _virtualForwardDropActive;
                snapshot.VirtualQueueLength = _virtualQueueLength;
                snapshot.VirtualOutstandingEvents = _virtualOutstandingEvents;
                snapshot.VirtualMaximumQueueLength = _virtualMaximumQueueLength;
                snapshot.PlaybackMicroseconds = playbackUs;
                snapshot.IntendedTimelineMicroseconds = _song == null ? 0 : Math.Min(playbackUs, _song.DurationMicroseconds);
                snapshot.LastDispatchedTimelineMicroseconds = _lastDispatchedMicroseconds;
                snapshot.EffectiveSpeedFrontierMicroseconds = _mode == ProcessingMode.PerNoteIntervalGate
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
                    if (_mode == ProcessingMode.Queue)
                        RunQueueMode(waiter);
                    else if (_mode == ProcessingMode.Drop)
                        RunDropMode(waiter);
                    else
                        RunPerNoteIntervalGate(waiter);
                }
                lock (_sync)
                    completed = _state == PlaybackState.Playing && _song != null;
            }
            catch (Exception ex)
            {
                failure = ex;
                try { if (_output != null) _output.Panic(); }
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
                try { MidiOutputSafety.ResetAndSilence(finalSilenceOutput); }
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
                        _transportBaseTicks += Stopwatch.GetTimestamp() - _runStartStamp;
                    _state = failure == null ? PlaybackState.Completed : PlaybackState.Stopped;
                }
                _thread = null;
                _queueLength = 0;
                _outstandingEvents = 0;
                _currentLagMicroseconds = 0;
                abandonedControls = DrainChannelControlRequestsLocked();
            }
            CompleteRetiredChannelControls(abandonedControls, failure ??
                new OperationCanceledException("Playback ended before the channel-control request reached the output boundary."));
            PublishChannelState(true);

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

        private void RunQueueMode(HighResolutionWaiter waiter)
        {
            int nextArrival = _startEventIndex;
            int nextProcess = _startEventIndex;
            int inService = -1;
            long completionTicks = 0;
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
                ApplyPendingStateChase();
                ApplyPendingOverrides();
                long now = CurrentTransportTicks();

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                int currentOverrideGeneration = _channelOverrides.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration)
                {
                    if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                    RetireUnlimitedFilteredBacklog(nextProcess, nextArrival, filtered, ref eligiblePending,
                        ref inService, ref completionTicks, ref lastCompletionTicks, now);
                    routingGeneration = currentRoutingGeneration;
                    overrideGeneration = currentOverrideGeneration;
                }

                int due = FindFirstEventAfterTransport(nextArrival, now);
                AdmitUnlimitedRange(nextArrival, due, filtered, ref eligiblePending);
                nextArrival = due;
                PublishUnlimitedQueue(nextProcess, eligiblePending, inService >= 0);

                if (inService >= 0 && completionTicks <= now)
                {
                    Dispatch(inService, now);
                    inService = -1;
                    lastCompletionTicks = completionTicks;
                    continue;
                }

                if (inService < 0 && eligiblePending > 0)
                {
                    if (IsEffectiveZeroService())
                    {
                        PublishUnlimitedQueue(nextProcess, Math.Max(0, eligiblePending - 1), true);
                        DispatchImmediateFilteredRange(ref nextProcess, nextArrival, now, filtered, ref eligiblePending);
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
                    completionTicks = checked(startTicks + ServiceTicksForEvent(inService));
                    PublishUnlimitedQueue(nextProcess, eligiblePending, true);
                    continue;
                }

                if (nextArrival >= _events.Count && inService < 0 && eligiblePending == 0)
                    return;

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
            int nextArrival = _startEventIndex;
            int inService = -1;
            long completionTicks = Int64.MaxValue;
            int bufferCapacity = Volatile.Read(ref _queueLengthLimit);
            System.Collections.Generic.Queue<int> pending = new System.Collections.Generic.Queue<int>(bufferCapacity);
            int consecutiveDrops = 0;
            long clusterTimestamp = Int64.MinValue;
            int clusterEnd = nextArrival;
            int clusterSize = 0;
            int maximumBufferOccupancy = 0;
            CompleteNoteTracker completeNotes = new CompleteNoteTracker();
            FilteredSourceEvents filtered = new FilteredSourceEvents();
            int routingGeneration = _channelRouting.FilterGeneration;
            int overrideGeneration = _channelOverrides.FilterGeneration;
            if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
            bool traceEnabled;
            lock (_sync) traceEnabled = _dropTrace != null;

            while (IsActive())
            {
                int iterationWakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                ApplyPendingStateChase();
                ApplyPendingOverrides();
                long now = CurrentTransportTicks();
                PublishDropQueue(nextArrival, pending.Count, inService >= 0);

                int currentRoutingGeneration = _channelRouting.FilterGeneration;
                int currentOverrideGeneration = _channelOverrides.FilterGeneration;
                if (currentRoutingGeneration != routingGeneration || currentOverrideGeneration != overrideGeneration)
                {
                    if (HasActiveSourceFilters()) Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
                    RetireDropFilteredBacklog(filtered, pending, ref inService, ref completionTicks, now);
                    routingGeneration = currentRoutingGeneration;
                    overrideGeneration = currentOverrideGeneration;
                }
                long arrivalTicks = nextArrival < _events.Count ? EventTicks(nextArrival) : Int64.MaxValue;

                if (inService >= 0 && completionTicks <= arrivalTicks && completionTicks <= now)
                {
                    long completedAt = completionTicks;
                    Dispatch(inService, now);
                    inService = -1;
                    completionTicks = Int64.MaxValue;
                    if (pending.Count > 0)
                    {
                        inService = pending.Dequeue();
                        // Backlogged service begins at the preceding logical
                        // completion, not at the scheduler thread's wake time.
                        long serviceStart = completedAt;
                        completionTicks = checked(serviceStart + ServiceTicksForEvent(inService));
                        if (traceEnabled)
                            TraceService(inService, serviceStart, completionTicks, EstimateBusyUntil(completionTicks, pending));
                    }
                    UpdateQueue(pending.Count, inService >= 0);
                    continue;
                }

                if (nextArrival < _events.Count && arrivalTicks <= now)
                {
                    if (HasActiveSourceFilters() && MarkNewSourceFiltered(nextArrival, filtered))
                    {
                        nextArrival++;
                        UpdateQueue(pending.Count, inService >= 0);
                        continue;
                    }
                    if (!traceEnabled && !HasActiveSourceFilters() && inService < 0 && pending.Count == 0 && IsEffectiveZeroService())
                    {
                        int dueEnd = nextArrival + 1;
                        int dueLimit = Math.Min(_events.Count, nextArrival + 2048);
                        while (dueEnd < dueLimit && EventTicks(dueEnd) <= now) dueEnd++;
                        PublishDropQueue(nextArrival + 1, 0, true);
                        DispatchImmediateRange(ref nextArrival, dueEnd, now);
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
                    if (noteKind == CompleteNoteEventKind.NoteOff && completeNotes.ShouldSuppressNoteOff(incomingEvent))
                    {
                        consecutiveDrops++;
                        lock (_sync) _droppedEvents++;
                        RecordChannelDrop(nextArrival);
                        if (traceEnabled)
                            TraceDropped(nextArrival, now, "note-off suppressed because its paired note-on was rejected",
                                clusterSize, consecutiveDrops, EstimateBusyUntil(completionTicks, pending),
                                pending.Count + (inService >= 0 ? 1 : 0), maximumBufferOccupancy);
                        nextArrival++;
                        UpdateQueue(pending.Count, inService >= 0);
                        continue;
                    }

                    OverflowPolicy overflowPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                    int outstanding = pending.Count + (inService >= 0 ? 1 : 0);
                    bool safetyAdmission = overflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                        noteKind != CompleteNoteEventKind.NoteOn;
                    if (outstanding < bufferCapacity || safetyAdmission)
                    {
                        int acceptedIndex = nextArrival;
                        consecutiveDrops = 0;
                        if (inService < 0)
                        {
                            inService = acceptedIndex;
                            long serviceStart = arrivalTicks;
                            completionTicks = checked(serviceStart + ServiceTicksForEvent(inService));
                        }
                        else
                        {
                            pending.Enqueue(acceptedIndex);
                        }
                        int occupancy = pending.Count + (inService >= 0 ? 1 : 0);
                        if (occupancy > maximumBufferOccupancy) maximumBufferOccupancy = occupancy;
                        if (noteKind == CompleteNoteEventKind.NoteOn)
                            completeNotes.RecordNoteOn(incomingEvent, true, false);
                        if (traceEnabled)
                        {
                            long busyUntil = EstimateBusyUntil(completionTicks, pending);
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
                            int evicted = pending.Dequeue();
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            RecordChannelDrop(evicted);
                            if (traceEnabled)
                                TraceDropped(evicted, now, "oldest pending event evicted on overflow", ClusterSizeAt(evicted), consecutiveDrops,
                                    EstimateBusyUntil(completionTicks, pending), outstanding - 1, maximumBufferOccupancy);

                            int acceptedIndex = nextArrival;
                            pending.Enqueue(acceptedIndex);
                            if (noteKind == CompleteNoteEventKind.NoteOn)
                                completeNotes.RecordNoteOn(incomingEvent, true, false);
                            if (traceEnabled)
                            {
                                long busyUntil = EstimateBusyUntil(completionTicks, pending);
                                TraceAcceptedAdmission(acceptedIndex, now, clusterSize, outstanding, maximumBufferOccupancy, busyUntil);
                            }
                            UpdateQueue(pending.Count, inService >= 0);
                            consecutiveDrops = 0;
                        }
                        else if (overflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                        {
                            System.Collections.Generic.List<int> cleared = traceEnabled
                                ? new System.Collections.Generic.List<int>(outstanding + 1) : null;
                            int clearedCount = outstanding;
                            bool trackChannels = Volatile.Read(ref _channelMonitoringEnabled) != 0;
                            if (trackChannels && inService >= 0) _channelState.RecordDropped(_events[inService]);
                            if (trackChannels)
                                foreach (int pendingIndex in pending) _channelState.RecordDropped(_events[pendingIndex]);
                            if (traceEnabled && inService >= 0) cleared.Add(inService);
                            if (traceEnabled)
                                while (pending.Count > 0) cleared.Add(pending.Dequeue());
                            else
                                pending.Clear();
                            inService = -1;
                            completionTicks = Int64.MaxValue;

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
                        else if (overflowPolicy == OverflowPolicy.DropIncomingCompleteNotes &&
                            noteKind == CompleteNoteEventKind.NoteOn)
                        {
                            completeNotes.RecordNoteOn(incomingEvent, false, true);
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            RecordChannelDrop(nextArrival);
                            if (traceEnabled)
                                TraceDropped(nextArrival, now, "incoming note-on rejected as a complete-note pair; buffer full (" +
                                    bufferCapacity + " events outstanding)", clusterSize, consecutiveDrops,
                                    EstimateBusyUntil(completionTicks, pending), outstanding, maximumBufferOccupancy);
                        }
                        else
                        {
                            if (noteKind == CompleteNoteEventKind.NoteOn)
                                completeNotes.RecordNoteOn(incomingEvent, false, false);
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            RecordChannelDrop(nextArrival);
                            if (traceEnabled)
                                TraceDropped(nextArrival, now, "newest event dropped; buffer full (" + bufferCapacity + " events outstanding)",
                                    clusterSize, consecutiveDrops, EstimateBusyUntil(completionTicks, pending), outstanding, maximumBufferOccupancy);
                        }
                    }
                    nextArrival++;
                    continue;
                }

                if (nextArrival >= _events.Count && inService < 0 && pending.Count == 0)
                    return;

                long target = Math.Min(arrivalTicks, completionTicks);
                if (inService < 0)
                    SetCurrentLag(0);
                PublishChannelStateBeforeWait(target, now);
                WaitUntil(waiter, target, iterationWakeGeneration);
            }
        }

        private void RunVirtualForwardDropMode(HighResolutionWaiter waiter)
        {
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
                ApplyPendingStateChase();
                ApplyPendingOverrides();
                long nowTicks = CurrentTransportTicks();
                PublishDropQueue(nextArrival, 0, false);

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
                    long service = ConfiguredServiceMicroseconds(incomingEvent);
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
            long interval = Interlocked.Read(ref _processingMicroseconds);
            if (interval <= 0)
                throw new InvalidOperationException("Per-note interval gate requires a nonzero processing interval.");

            PerNoteIntervalGate gate = new PerNoteIntervalGate(interval);
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
                ApplyPendingStateChase();
                ApplyPendingOverrides();
                long nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());

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
                    long nextBoundary = gate.NextBoundaryMicroseconds;
                    long nextSource = nextArrival < _events.Count
                        ? _events[nextArrival].IntendedMicroseconds : Int64.MaxValue;
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
                                DispatchMidiEvent(midiEvent);
                            if (Volatile.Read(ref _dispatchSuspended) != 0 || !IsPlaying()) return;
                        }
                        gate.EndSourceTick();
                        AddGateFiltered(gate.TakeFilteredEventCount());
                    }
                    else
                    {
                        int count = gate.EmitBoundary(nextBoundary, boundaryEvents);
                        AddGateFiltered(gate.TakeFilteredEventCount());
                        for (int index = 0; index < count; index++)
                        {
                            // A native send may have been blocked while a
                            // transport boundary was requested. Do not emit
                            // the rest of the logical batch after it returns.
                            if (Volatile.Read(ref _dispatchSuspended) != 0 || !IsPlaying()) return;
                            DispatchMidiEvent(boundaryEvents[index]);
                        }
                        AdvanceGateEffectiveSpeedFrontier(nextBoundary);
                        examined += Math.Max(1, count);
                    }
                    didWork = true;
                    nowMicroseconds = TicksToMicroseconds(CurrentTransportTicks());
                    if (examined >= 2048 || Stopwatch.GetTimestamp() - batchStarted >= Stopwatch.Frequency / 125)
                        break;
                }

                UpdateQueue(0, false);
                if (nextArrival >= _events.Count && !sourceCompleted)
                {
                    MidiSong song;
                    lock (_sync) song = _song;
                    gate.CompleteSource(song == null ? nowMicroseconds : song.DurationMicroseconds);
                    AddGateFiltered(gate.TakeFilteredEventCount());
                    sourceCompleted = true;
                }
                if (nextArrival >= _events.Count && !gate.HasPendingTransitions) return;

                long targetMicroseconds = gate.NextBoundaryMicroseconds;
                if (nextArrival < _events.Count)
                    targetMicroseconds = Math.Min(targetMicroseconds, _events[nextArrival].IntendedMicroseconds);
                long nowTicks = CurrentTransportTicks();
                long targetTicks = MicrosecondsToTicks(targetMicroseconds);
                PublishChannelStateBeforeWait(targetTicks, nowTicks);
                if (!didWork || targetTicks > nowTicks)
                    WaitUntil(waiter, targetTicks, iterationWakeGeneration);
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
            DispatchMidiEvent(_events[eventIndex]);
        }

        private void DispatchMidiEvent(MidiEventView midiEvent)
        {
            SendScheduledEvent(midiEvent, Volatile.Read(ref _channelMonitoringEnabled) != 0);
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

        private void DispatchImmediateRange(ref int nextIndex, int endExclusive, long transportAtBatchStart)
        {
            const int chunkSize = 2048;
            long stopwatchAtBatchStart = Stopwatch.GetTimestamp();
            if (nextIndex < endExclusive && Volatile.Read(ref _dispatchSuspended) == 0 && IsEffectiveZeroService())
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
                        if ((sent & 63) == 0 && !IsEffectiveZeroService()) break;
                        long intendedMicroseconds;
                        MidiEventView midiEvent = _events.GetSequential(nextIndex, ref cursor, out intendedMicroseconds);
                        PublishDropQueue(nextIndex + 1, 0, true);
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
            FilteredSourceEvents filtered, ref long eligiblePending)
        {
            const int chunkSize = 2048;
            long stopwatchAtBatchStart = Stopwatch.GetTimestamp();
            if (nextIndex >= endExclusive || Volatile.Read(ref _dispatchSuspended) != 0 || !IsEffectiveZeroService()) return;
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
                    if ((examined & 63) == 0 && !IsEffectiveZeroService()) break;
                    int eventIndex = nextIndex++;
                    examined++;
                    if (filtered.Any && filtered.Contains(eventIndex)) continue;
                    long intendedMicroseconds;
                    MidiEventView midiEvent = _events.GetSequential(eventIndex, ref cursor, out intendedMicroseconds);
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

        private void PublishDropQueue(int nextArrival, int pending, bool inService)
        {
            lock (_sync)
            {
                _publishedDropNextArrival = nextArrival;
                _publishedDropPending = Math.Max(0, pending);
                _publishedInService = inService;
                _hasPublishedQueue = true;
                _queueLength = _publishedDropPending;
                _outstandingEvents = _queueLength + (inService ? 1 : 0);
                if (_outstandingEvents > _maximumQueueLength) _maximumQueueLength = _outstandingEvents;
            }
            PublishChannelState(false);
        }

        // A synchronous native Send can block while source time continues.  The
        // worker cannot admit those newly-due events until Send returns, so the
        // snapshot counts that exact candidate range.  Filtered ranges are
        // cached and extended: an unchanged blocked range is never rescanned on
        // every UI refresh.
        private long CountSnapshotEligible(int startInclusive, int endExclusive)
        {
            if (startInclusive >= endExclusive) return 0;
            if (!HasActiveSourceFilters()) return endExclusive - startInclusive;

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
            _output.Send(midiEvent);
            if (trackChannels) _channelState.RecordSuccessful(midiEvent);
            return true;
        }

        private void RetireDropFilteredBacklog(FilteredSourceEvents filtered, Queue<int> pending,
            ref int inService, ref long completionTicks, long now)
        {
            bool changed = false;
            if (inService >= 0 && MarkSourceFiltered(inService, filtered))
            {
                inService = -1;
                completionTicks = Int64.MaxValue;
                changed = true;
            }
            int pendingCount = pending.Count;
            for (int i = 0; i < pendingCount; i++)
            {
                int eventIndex = pending.Dequeue();
                if (MarkSourceFiltered(eventIndex, filtered)) changed = true;
                else pending.Enqueue(eventIndex);
            }
            if (inService < 0 && pending.Count > 0)
            {
                inService = pending.Dequeue();
                completionTicks = checked(now + ServiceTicksForEvent(inService));
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
                return true;
            }
            ChannelAttribute attribute;
            int forcedValue;
            if (_channelOverrides.ShouldSuppress(midiEvent, out attribute, out forcedValue))
            {
                filtered.Add(eventIndex);
                _channelOverrides.RecordSuppressed(midiEvent.Channel);
                Volatile.Write(ref _usesPreAdmissionFilterAccounting, 1);
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
            long now)
        {
            bool changed = false;
            if (inService >= 0 && MarkSourceFiltered(inService, filtered))
            {
                inService = -1;
                completionTicks = 0;
                lastCompletionTicks = now;
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
                        _output.Send(ChannelOverrideState.CreateMessage(channel, attribute, value));
                        if (trackChannels) _channelState.RecordOverrideApplied(channel, attribute, value);
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
                output.Send(message);
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

        private long EstimateBusyUntil(long currentCompletionTicks, System.Collections.Generic.Queue<int> queuedEvents)
        {
            if (currentCompletionTicks == Int64.MaxValue) return 0;
            long busyUntil = currentCompletionTicks;
            foreach (int eventIndex in queuedEvents)
                busyUntil = checked(busyUntil + ServiceTicksForEvent(eventIndex));
            return busyUntil;
        }

        private long ServiceTicksForEvent(int eventIndex)
        {
            if (Volatile.Read(ref _simulateSlowdown) == 0) return 0;
            ServiceDurationMode mode = (ServiceDurationMode)Volatile.Read(ref _serviceDurationMode);
            long microseconds = ServiceDurationCalculator.CalculateMicroseconds(
                _events[eventIndex], mode, Interlocked.Read(ref _processingMicroseconds), Interlocked.Read(ref _midiBitrate));
            return MicrosecondsToTicks(microseconds);
        }

        private long ConfiguredServiceMicroseconds(MidiEventView midiEvent)
        {
            return ServiceDurationCalculator.CalculateMicroseconds(midiEvent,
                (ServiceDurationMode)Volatile.Read(ref _serviceDurationMode),
                Interlocked.Read(ref _processingMicroseconds), Interlocked.Read(ref _midiBitrate));
        }

        private bool IsEffectiveZeroService()
        {
            if (Volatile.Read(ref _simulateSlowdown) == 0) return true;
            return (ServiceDurationMode)Volatile.Read(ref _serviceDurationMode) == ServiceDurationMode.ProcessingTime &&
                Interlocked.Read(ref _processingMicroseconds) == 0;
        }

        private bool WaitWhilePaused()
        {
            while (true)
            {
                PlaybackState state;
                lock (_sync) state = _state;
                if (state == PlaybackState.Playing) return true;
                if (state != PlaybackState.Paused) return false;
                int wakeGeneration = Volatile.Read(ref _wakeGeneration);
                ApplyPendingChannelControls();
                _wake.Reset();
                if (wakeGeneration != Volatile.Read(ref _wakeGeneration)) continue;
                lock (_sync) state = _state;
                if (state == PlaybackState.Paused) _wake.WaitOne();
            }
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

        private long CurrentTransportTicks()
        {
            lock (_sync) return CurrentTransportTicksLocked();
        }

        private long CurrentTransportTicksLocked()
        {
            if (_state == PlaybackState.Playing)
                return _transportBaseTicks + Stopwatch.GetTimestamp() - _runStartStamp;
            return _transportBaseTicks;
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
            _processedEvents = 0;
            _droppedEvents = 0;
            _gateFilteredEvents = 0;
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

    internal enum ChannelControlKind
    {
        SetEnabled,
        Chase
    }

    internal sealed class ChannelControlRequest
    {
        internal readonly ChannelControlKind Kind;
        internal readonly int Channel;
        internal readonly ChannelAttribute Attribute;
        internal readonly int Value;
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
