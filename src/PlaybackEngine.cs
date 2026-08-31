using System;
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

        private long _queueLength;
        private long _maximumQueueLength;
        private long _processedEvents;
        private long _droppedEvents;
        private long _lastDispatchedMicroseconds;
        private long _currentLagMicroseconds;
        private long _maximumLagMicroseconds;
        private DropTraceRecorder _dropTrace;

        public event EventHandler PlaybackEnded;
        public event EventHandler<PlaybackErrorEventArgs> PlaybackFailed;

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
            if (song == null) throw new ArgumentNullException("song");
            if (output == null) throw new ArgumentNullException("output");
            Stop();
            startMicroseconds = ClampPosition(song, startMicroseconds);
            lock (_sync)
            {
                _song = song;
                _output = output;
                _mode = mode;
                _startEventIndex = FindFirstEventAtOrAfter(song, startMicroseconds);
                _transportBaseTicks = MicrosecondsToTicks(startMicroseconds);
                _runStartStamp = Stopwatch.GetTimestamp();
                ResetStatisticsLocked();
                _lastDispatchedMicroseconds = startMicroseconds;
                _state = PlaybackState.Playing;
                _thread = new Thread(PlaybackWorker);
                _thread.Name = "MIDI bottleneck scheduler";
                _thread.IsBackground = true;
                _thread.Priority = ThreadPriority.AboveNormal;
                _thread.Start();
            }
        }

        public void Seek(long targetMicroseconds)
        {
            Thread previousThread;
            IMidiOutput output;
            MidiSong song;
            PlaybackState previousState;

            lock (_sync)
            {
                song = _song;
                if (song == null) return;
                targetMicroseconds = ClampPosition(song, targetMicroseconds);
                previousState = _state;
                if (_state == PlaybackState.Playing)
                    _transportBaseTicks += Stopwatch.GetTimestamp() - _runStartStamp;
                _state = PlaybackState.Stopped;
                previousThread = _thread;
                output = _output;
            }

            _wake.Set();
            if (previousThread != null && previousThread != Thread.CurrentThread)
                previousThread.Join(2000);
            if (output != null)
                output.Reset();

            lock (_sync)
            {
                _transportBaseTicks = MicrosecondsToTicks(targetMicroseconds);
                _startEventIndex = FindFirstEventAtOrAfter(song, targetMicroseconds);
                ResetStatisticsLocked();
                _lastDispatchedMicroseconds = targetMicroseconds;
                _thread = null;

                if (previousState == PlaybackState.Playing || previousState == PlaybackState.Paused)
                {
                    _state = previousState;
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
                }
            }
            _wake.Set();
        }

        public void Pause()
        {
            lock (_sync)
            {
                if (_state != PlaybackState.Playing) return;
                _transportBaseTicks += Stopwatch.GetTimestamp() - _runStartStamp;
                _state = PlaybackState.Paused;
            }
            _output.Panic();
            _wake.Set();
        }

        public void Resume()
        {
            lock (_sync)
            {
                if (_state != PlaybackState.Paused) return;
                _runStartStamp = Stopwatch.GetTimestamp();
                _state = PlaybackState.Playing;
            }
            _wake.Set();
        }

        public void Stop()
        {
            Thread thread;
            IMidiOutput output;
            lock (_sync)
            {
                if (_state == PlaybackState.Playing)
                    _transportBaseTicks += Stopwatch.GetTimestamp() - _runStartStamp;
                _state = PlaybackState.Stopped;
                thread = _thread;
                output = _output;
            }
            _wake.Set();
            if (thread != null && thread != Thread.CurrentThread)
                thread.Join(2000);
            if (output != null)
                output.Reset();
            lock (_sync)
            {
                if (_thread == thread) _thread = null;
                _queueLength = 0;
                _currentLagMicroseconds = 0;
            }
        }

        public void ResetStatistics()
        {
            lock (_sync)
            {
                _maximumQueueLength = _queueLength;
                _processedEvents = 0;
                _droppedEvents = 0;
                _maximumLagMicroseconds = _currentLagMicroseconds;
            }
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
            }

            lock (_sync)
            {
                if (failure != null || completed)
                {
                    if (_state == PlaybackState.Playing)
                        _transportBaseTicks += Stopwatch.GetTimestamp() - _runStartStamp;
                    _state = failure == null ? PlaybackState.Completed : PlaybackState.Stopped;
                }
                _thread = null;
                _queueLength = 0;
                _currentLagMicroseconds = 0;
            }

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
                if (!WaitWhilePaused()) return;
                long now = CurrentTransportTicks();

                while (nextArrival < _song.Events.Count && EventTicks(nextArrival) <= now)
                    nextArrival++;
                UpdateQueue(nextArrival - nextProcess);

                if (inService >= 0 && completionTicks <= now)
                {
                    Dispatch(inService, now);
                    inService = -1;
                    lastCompletionTicks = completionTicks;
                    continue;
                }

                if (inService < 0 && nextProcess < nextArrival)
                {
                    inService = nextProcess++;
                    long startTicks = Math.Max(EventTicks(inService), lastCompletionTicks);
                    completionTicks = checked(startTicks + ServiceTicksForEvent(inService));
                    UpdateQueue(nextArrival - nextProcess);
                    continue;
                }

                if (nextArrival >= _song.Events.Count && inService < 0 && nextProcess >= nextArrival)
                    return;

                long target = inService >= 0 ? completionTicks : Int64.MaxValue;
                if (nextArrival < _song.Events.Count)
                    target = Math.Min(target, EventTicks(nextArrival));
                if (inService < 0 && nextProcess >= nextArrival)
                    SetCurrentLag(0);
                WaitUntil(waiter, target);
            }
        }

        private void RunDropMode(HighResolutionWaiter waiter)
        {
            int nextArrival = _startEventIndex;
            int inService = -1;
            long completionTicks = Int64.MaxValue;
            int bufferCapacity = Volatile.Read(ref _queueLengthLimit);
            OverflowPolicy overflowPolicy = (OverflowPolicy)Volatile.Read(ref _overflowPolicy);
            System.Collections.Generic.Queue<int> pending = new System.Collections.Generic.Queue<int>(bufferCapacity);
            int consecutiveDrops = 0;
            long clusterTimestamp = Int64.MinValue;
            int clusterEnd = nextArrival;
            int clusterSize = 0;
            int maximumBufferOccupancy = 0;

            while (IsActive())
            {
                if (!WaitWhilePaused()) return;
                long now = CurrentTransportTicks();
                long arrivalTicks = nextArrival < _song.Events.Count ? EventTicks(nextArrival) : Int64.MaxValue;

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
                        TraceService(inService, serviceStart, completionTicks, EstimateBusyUntil(completionTicks, pending));
                    }
                    UpdateQueue(pending.Count);
                    continue;
                }

                if (nextArrival < _song.Events.Count && arrivalTicks <= now)
                {
                    if (arrivalTicks != clusterTimestamp || nextArrival >= clusterEnd)
                    {
                        clusterTimestamp = arrivalTicks;
                        clusterEnd = nextArrival + 1;
                        while (clusterEnd < _song.Events.Count && EventTicks(clusterEnd) == arrivalTicks) clusterEnd++;
                        clusterSize = clusterEnd - nextArrival;
                    }

                    int outstanding = pending.Count + (inService >= 0 ? 1 : 0);
                    if (outstanding < bufferCapacity)
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
                        long busyUntil = EstimateBusyUntil(completionTicks, pending);
                        TraceAcceptedAdmission(acceptedIndex, now, clusterSize, occupancy, maximumBufferOccupancy, busyUntil);
                        if (acceptedIndex == inService)
                            TraceService(acceptedIndex, arrivalTicks, completionTicks, busyUntil);
                        UpdateQueue(pending.Count);
                    }
                    else
                    {
                        if (overflowPolicy == OverflowPolicy.DropOldest && pending.Count > 0)
                        {
                            int evicted = pending.Dequeue();
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            TraceDropped(evicted, now, "oldest pending event evicted on overflow", ClusterSizeAt(evicted), consecutiveDrops,
                                EstimateBusyUntil(completionTicks, pending), outstanding - 1, maximumBufferOccupancy);

                            int acceptedIndex = nextArrival;
                            pending.Enqueue(acceptedIndex);
                            long busyUntil = EstimateBusyUntil(completionTicks, pending);
                            TraceAcceptedAdmission(acceptedIndex, now, clusterSize, outstanding, maximumBufferOccupancy, busyUntil);
                            UpdateQueue(pending.Count);
                            consecutiveDrops = 0;
                        }
                        else if (overflowPolicy == OverflowPolicy.ClearBufferAndCatchUp)
                        {
                            System.Collections.Generic.List<int> cleared = new System.Collections.Generic.List<int>(outstanding + 1);
                            if (inService >= 0) cleared.Add(inService);
                            while (pending.Count > 0) cleared.Add(pending.Dequeue());
                            inService = -1;
                            completionTicks = Int64.MaxValue;

                            int catchUp = nextArrival;
                            while (catchUp < _song.Events.Count && EventTicks(catchUp) <= now)
                            {
                                cleared.Add(catchUp);
                                catchUp++;
                            }

                            for (int clearedIndex = 0; clearedIndex < cleared.Count; clearedIndex++)
                            {
                                int eventIndex = cleared[clearedIndex];
                                consecutiveDrops++;
                                TraceDropped(eventIndex, now, "buffer cleared; caught up to realtime", ClusterSizeAt(eventIndex),
                                    consecutiveDrops, 0, 0, maximumBufferOccupancy);
                            }
                            lock (_sync) _droppedEvents += cleared.Count;
                            nextArrival = catchUp;
                            UpdateQueue(0);
                            SetCurrentLag(0);
                            try { _output.Panic(); }
                            catch { }
                            continue;
                        }
                        else
                        {
                            consecutiveDrops++;
                            lock (_sync) _droppedEvents++;
                            TraceDropped(nextArrival, now, "newest event dropped; buffer full (" + bufferCapacity + " events outstanding)",
                                clusterSize, consecutiveDrops, EstimateBusyUntil(completionTicks, pending), outstanding, maximumBufferOccupancy);
                        }
                    }
                    nextArrival++;
                    continue;
                }

                if (nextArrival >= _song.Events.Count && inService < 0 && pending.Count == 0)
                    return;

                long target = Math.Min(arrivalTicks, completionTicks);
                if (inService < 0)
                    SetCurrentLag(0);
                WaitUntil(waiter, target);
            }
        }

        private void Dispatch(int eventIndex, long actualTransportTicks)
        {
            MidiEvent midiEvent = _song.Events[eventIndex];
            _output.Send(midiEvent);
            long actualUs = TicksToMicroseconds(actualTransportTicks);
            long lag = Math.Max(0, actualUs - midiEvent.IntendedMicroseconds);
            lock (_sync)
            {
                _processedEvents++;
                _lastDispatchedMicroseconds = midiEvent.IntendedMicroseconds;
                _currentLagMicroseconds = lag;
                if (lag > _maximumLagMicroseconds) _maximumLagMicroseconds = lag;
            }
        }

        private void UpdateQueue(long queue)
        {
            lock (_sync)
            {
                _queueLength = queue;
                if (queue > _maximumQueueLength) _maximumQueueLength = queue;
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
                trace.RecordAcceptedAdmission(eventIndex, _song.Events[eventIndex], TicksToMicroseconds(decisionTicks), clusterSize, bufferOccupancy, maximumBufferOccupancy, TicksToMicroseconds(busyUntilTicks));
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
                trace.RecordDropped(eventIndex, _song.Events[eventIndex], TicksToMicroseconds(decisionTicks), reason, clusterSize, consecutiveDrops, TicksToMicroseconds(busyUntilTicks), bufferOccupancy, maximumBufferOccupancy);
        }

        private int ClusterSizeAt(int eventIndex)
        {
            long timestamp = _song.Events[eventIndex].IntendedMicroseconds;
            int first = eventIndex;
            int last = eventIndex + 1;
            while (first > 0 && _song.Events[first - 1].IntendedMicroseconds == timestamp) first--;
            while (last < _song.Events.Count && _song.Events[last].IntendedMicroseconds == timestamp) last++;
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
                _song.Events[eventIndex], mode, Interlocked.Read(ref _processingMicroseconds), Interlocked.Read(ref _midiBitrate));
            return MicrosecondsToTicks(microseconds);
        }

        private bool WaitWhilePaused()
        {
            while (true)
            {
                PlaybackState state;
                lock (_sync) state = _state;
                if (state == PlaybackState.Playing) return true;
                if (state != PlaybackState.Paused) return false;
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
            return MicrosecondsToTicks(_song.Events[eventIndex].IntendedMicroseconds);
        }

        private static long MicrosecondsToTicks(long microseconds)
        {
            return (long)(((decimal)microseconds * Stopwatch.Frequency) / 1000000m);
        }

        private static long TicksToMicroseconds(long ticks)
        {
            return (long)(((decimal)ticks * 1000000m) / Stopwatch.Frequency);
        }

        private void ResetStatisticsLocked()
        {
            _queueLength = 0;
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
            int low = 0;
            int high = song.Events.Count;
            while (low < high)
            {
                int middle = low + ((high - low) / 2);
                if (song.Events[middle].IntendedMicroseconds < microseconds)
                    low = middle + 1;
                else
                    high = middle;
            }
            return low;
        }

        public void Dispose()
        {
            Stop();
            _wake.Dispose();
        }
    }

    internal sealed class PlaybackErrorEventArgs : EventArgs
    {
        public readonly Exception Error;
        public PlaybackErrorEventArgs(Exception error) { Error = error; }
    }
}
