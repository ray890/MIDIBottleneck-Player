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
        private Thread _thread;
        private MidiSong _song;
        private MidiEventReader _events;
        private List<MidiEvent> _eventList;
        private IMidiOutput _output;
        private ProcessingMode _mode;
        private int _startEventIndex;
        private PlaybackState _state = PlaybackState.Stopped;
        private long _processingMicroseconds;
        private long _midiBitrate = ServiceDurationCalculator.FivePinDinBitrate;
        private int _serviceDurationMode;
        private int _queueLengthLimit = DefaultQueueLengthLimit;
        private int _simulateSlowdown = 1;
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
        private long _lastDispatchedMicroseconds;
        private long _currentLagMicroseconds;
        private long _maximumLagMicroseconds;
        private int _publishedNextProcess;
        private bool _publishedInService;
        private bool _hasPublishedQueue;
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
                _wake.Set();
            }
        }

        public ServiceDurationMode ServiceDurationMode
        {
            get { return (ServiceDurationMode)Volatile.Read(ref _serviceDurationMode); }
            set
            {
                Volatile.Write(ref _serviceDurationMode, (int)value);
                _wake.Set();
            }
        }

        public long MidiBitrate
        {
            get { return Interlocked.Read(ref _midiBitrate); }
            set
            {
                if (value < 1) value = 1;
                Interlocked.Exchange(ref _midiBitrate, value);
                _wake.Set();
            }
        }

        public bool SimulateSlowdown
        {
            get { return Volatile.Read(ref _simulateSlowdown) != 0; }
            set
            {
                Volatile.Write(ref _simulateSlowdown, value ? 1 : 0);
                _wake.Set();
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
            Stop();
            IMidiOutputContext outputContext = output as IMidiOutputContext;
            if (outputContext != null) outputContext.SourceFile = song.FilePath;
            startMicroseconds = ClampPosition(song, startMicroseconds);
            lock (_sync)
            {
                _song = song;
                _events = song.GetEventReader();
                ListMidiEventStore listStore = song.EventStore as ListMidiEventStore;
                _eventList = listStore == null ? null : listStore.Source;
                _output = output;
                _mode = mode;
                _startEventIndex = FindFirstEventAtOrAfter(song, startMicroseconds);
                _hasPublishedQueue = false;
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
                _hasPublishedQueue = false;
                ResetStatisticsLocked();
                _lastDispatchedMicroseconds = targetMicroseconds;
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
            _wake.Set();
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
                _hasPublishedQueue = false;
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
            _wake.Set();
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
            _wake.Set();
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
            _wake.Set();

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
                _eventList = null;
                _output = null;
                _startEventIndex = 0;
                _lastDispatchedMicroseconds = 0;
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
            if (!sentDirectly) _wake.Set();
            PublishChannelState(true);
        }

        internal bool IsChannelEnabled(int channel) { return _channelRouting.IsEnabled(channel); }

        internal void SetChannelEnabled(int channel, bool enabled)
        {
            SubmitChannelControl(new ChannelControlRequest(ChannelControlKind.SetEnabled, channel,
                ChannelAttribute.BankMsb, enabled ? 1 : 0));
        }

        internal void ChaseChannelAttribute(int channel, ChannelAttribute attribute, int value)
        {
            ChaseChannelAttribute(channel, attribute, value, null);
        }

        internal void ChaseChannelAttribute(int channel, ChannelAttribute attribute, int value, Action<Exception> completion)
        {
            if (!_channelRouting.IsEnabled(channel))
            {
                InvalidOperationException error = new InvalidOperationException("Enable this MIDI channel before sending a historical value.");
                if (completion != null) { completion(error); return; }
                throw error;
            }
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
            if (queued) _wake.Set();
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
                throw new InvalidOperationException("Enable this MIDI channel before sending a historical value.");
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
                    (_state == PlaybackState.Playing || _state == PlaybackState.Paused))
                {
                    int due = FindFirstEventAfterTransport(_publishedNextProcess, playbackTicks);
                    _queueLength = Math.Max(0, due - _publishedNextProcess);
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
                snapshot.QueueLengthLimit = Volatile.Read(ref _queueLengthLimit);
                snapshot.OverflowPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
                snapshot.QueueLength = _queueLength;
                snapshot.OutstandingEvents = _outstandingEvents;
                snapshot.MaximumQueueLength = _maximumQueueLength;
                snapshot.ProcessedEvents = _processedEvents;
                snapshot.DroppedEvents = _droppedEvents;
                snapshot.PlaybackMicroseconds = playbackUs;
                snapshot.IntendedTimelineMicroseconds = _song == null ? 0 : Math.Min(playbackUs, _song.DurationMicroseconds);
                snapshot.LastDispatchedTimelineMicroseconds = _lastDispatchedMicroseconds;
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
                    else
                        RunDropMode(waiter);
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

            while (IsActive())
            {
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                ApplyPendingOverrides();
                long now = CurrentTransportTicks();

                nextArrival = FindFirstEventAfterTransport(nextArrival, now);
                PublishUnlimitedQueue(nextProcess, nextArrival, inService >= 0);

                if (inService >= 0 && completionTicks <= now)
                {
                    Dispatch(inService, now);
                    inService = -1;
                    lastCompletionTicks = completionTicks;
                    continue;
                }

                if (inService < 0 && nextProcess < nextArrival)
                {
                    if (IsEffectiveZeroService())
                    {
                        PublishUnlimitedQueue(nextProcess + 1, nextArrival, true);
                        DispatchImmediateRange(ref nextProcess, nextArrival, now);
                        nextArrival = FindFirstEventAfterTransport(nextArrival, CurrentTransportTicks());
                        PublishUnlimitedQueue(nextProcess, nextArrival, false);
                        continue;
                    }
                    inService = nextProcess++;
                    long startTicks = Math.Max(EventTicks(inService), lastCompletionTicks);
                    completionTicks = checked(startTicks + ServiceTicksForEvent(inService));
                    PublishUnlimitedQueue(nextProcess, nextArrival, true);
                    continue;
                }

                if (nextArrival >= _events.Count && inService < 0 && nextProcess >= nextArrival)
                    return;

                long target = inService >= 0 ? completionTicks : Int64.MaxValue;
                if (nextArrival < _events.Count)
                    target = Math.Min(target, EventTicks(nextArrival));
                if (inService < 0 && nextProcess >= nextArrival)
                    SetCurrentLag(0);
                PublishChannelStateBeforeWait(target, now);
                WaitUntil(waiter, target);
            }
        }

        private void RunDropMode(HighResolutionWaiter waiter)
        {
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
            bool traceEnabled;
            lock (_sync) traceEnabled = _dropTrace != null;

            while (IsActive())
            {
                ApplyPendingChannelControls();
                if (!WaitWhilePaused()) return;
                ApplyPendingOverrides();
                long now = CurrentTransportTicks();
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
                    if (!traceEnabled && inService < 0 && pending.Count == 0 && IsEffectiveZeroService())
                    {
                        int dueEnd = nextArrival + 1;
                        int dueLimit = Math.Min(_events.Count, nextArrival + 2048);
                        while (dueEnd < dueLimit && EventTicks(dueEnd) <= now) dueEnd++;
                        UpdateQueue(0, true);
                        DispatchImmediateRange(ref nextArrival, dueEnd, now);
                        UpdateQueue(0, false);
                        continue;
                    }
                    if (traceEnabled && (arrivalTicks != clusterTimestamp || nextArrival >= clusterEnd))
                    {
                        clusterTimestamp = arrivalTicks;
                        clusterEnd = nextArrival + 1;
                        while (clusterEnd < _events.Count && EventTicks(clusterEnd) == arrivalTicks) clusterEnd++;
                        clusterSize = clusterEnd - nextArrival;
                    }

                    MidiEvent incomingEvent = _events[nextArrival];
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
                WaitUntil(waiter, target);
            }
        }

        private void Dispatch(int eventIndex, long actualTransportTicks)
        {
            MidiEvent midiEvent = _events[eventIndex];
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
            if (_eventList != null)
            {
                DispatchImmediateListRange(ref nextIndex, endExclusive, transportAtBatchStart, _eventList);
                return;
            }

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
                        MidiEvent midiEvent = _events[nextIndex];
                        SendScheduledEvent(midiEvent, trackChannels);
                        long elapsedTicks = Stopwatch.GetTimestamp() - stopwatchAtBatchStart;
                        long actualTicks = transportAtBatchStart + elapsedTicks;
                        currentLag = Math.Max(0, TicksToMicroseconds(actualTicks) - midiEvent.IntendedMicroseconds);
                        if (currentLag > maximumLag) maximumLag = currentLag;
                        lastTimeline = midiEvent.IntendedMicroseconds;
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

        // The reference backend retains a direct List access loop. This avoids
        // charging today's hottest path for the abstraction used by future
        // compact stores.
        private void DispatchImmediateListRange(ref int nextIndex, int endExclusive, long transportAtBatchStart,
            List<MidiEvent> events)
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
                try
                {
                    while (nextIndex < chunkEnd)
                    {
                        if (Volatile.Read(ref _dispatchSuspended) != 0) break;
                        if ((sent & 63) == 0 && !IsEffectiveZeroService()) break;
                        MidiEvent midiEvent = events[nextIndex];
                        SendScheduledEvent(midiEvent, trackChannels);
                        long elapsedTicks = Stopwatch.GetTimestamp() - stopwatchAtBatchStart;
                        long actualTicks = transportAtBatchStart + elapsedTicks;
                        currentLag = Math.Max(0, TicksToMicroseconds(actualTicks) - midiEvent.IntendedMicroseconds);
                        if (currentLag > maximumLag) maximumLag = currentLag;
                        lastTimeline = midiEvent.IntendedMicroseconds;
                        sent++;
                        nextIndex++;
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

        private bool SendScheduledEvent(MidiEvent midiEvent, bool trackChannels)
        {
            if (_channelRouting.ShouldFilter(midiEvent))
            {
                _channelRouting.RecordFiltered(midiEvent.Channel);
                return false;
            }
            ChannelAttribute attribute;
            int forcedValue;
            if (_channelOverrides.ShouldSuppress(midiEvent, out attribute, out forcedValue))
            {
                _channelOverrides.RecordSuppressed(midiEvent.Channel);
                return false;
            }
            _output.Send(midiEvent);
            if (trackChannels) _channelState.RecordSuccessful(midiEvent);
            return true;
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

        private void PublishUnlimitedQueue(int nextProcess, int nextArrival, bool inService)
        {
            lock (_sync)
            {
                _publishedNextProcess = nextProcess;
                _publishedInService = inService;
                _hasPublishedQueue = true;
                _queueLength = Math.Max(0, nextArrival - nextProcess);
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
                ApplyPendingChannelControls();
                _wake.Reset();
                lock (_sync) state = _state;
                if (state == PlaybackState.Paused) _wake.WaitOne();
            }
        }

        private void WaitUntil(HighResolutionWaiter waiter, long targetTicks)
        {
            while (IsActive())
            {
                _wake.Reset();
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
            _lastDispatchedMicroseconds = 0;
            _currentLagMicroseconds = 0;
            _maximumLagMicroseconds = 0;
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
