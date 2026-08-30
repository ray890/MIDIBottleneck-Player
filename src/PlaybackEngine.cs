using System;
using System.Diagnostics;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class PlaybackEngine : IDisposable
    {
        private readonly object _sync = new object();
        private readonly EventWaitHandle _wake = new EventWaitHandle(false, EventResetMode.ManualReset);
        private Thread _thread;
        private MidiSong _song;
        private WindowsMidiOutput _output;
        private ProcessingMode _mode;
        private PlaybackState _state = PlaybackState.Stopped;
        private long _processingMicroseconds;
        private long _transportBaseTicks;
        private long _runStartStamp;

        private long _queueLength;
        private long _maximumQueueLength;
        private long _processedEvents;
        private long _droppedEvents;
        private long _lastDispatchedMicroseconds;
        private long _currentLagMicroseconds;
        private long _maximumLagMicroseconds;

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

        public void Start(MidiSong song, WindowsMidiOutput output, ProcessingMode mode)
        {
            if (song == null) throw new ArgumentNullException("song");
            if (output == null) throw new ArgumentNullException("output");
            Stop();
            lock (_sync)
            {
                _song = song;
                _output = output;
                _mode = mode;
                _transportBaseTicks = 0;
                _runStartStamp = Stopwatch.GetTimestamp();
                ResetStatisticsLocked();
                _state = PlaybackState.Playing;
                _thread = new Thread(PlaybackWorker);
                _thread.Name = "MIDI bottleneck scheduler";
                _thread.IsBackground = true;
                _thread.Priority = ThreadPriority.AboveNormal;
                _thread.Start();
            }
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
            WindowsMidiOutput output;
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

        public PlaybackSnapshot GetSnapshot()
        {
            lock (_sync)
            {
                long playbackTicks = CurrentTransportTicksLocked();
                long playbackUs = TicksToMicroseconds(playbackTicks);
                PlaybackSnapshot snapshot = new PlaybackSnapshot();
                snapshot.State = _state;
                snapshot.ProcessingMicroseconds = Interlocked.Read(ref _processingMicroseconds);
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
            int nextArrival = 0;
            int nextProcess = 0;
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
                    completionTicks = checked(startTicks + MicrosecondsToTicks(Interlocked.Read(ref _processingMicroseconds)));
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
            int nextArrival = 0;
            int inService = -1;
            long completionTicks = Int64.MaxValue;

            while (IsActive())
            {
                if (!WaitWhilePaused()) return;
                long now = CurrentTransportTicks();
                long arrivalTicks = nextArrival < _song.Events.Count ? EventTicks(nextArrival) : Int64.MaxValue;

                if (inService >= 0 && completionTicks <= arrivalTicks && completionTicks <= now)
                {
                    Dispatch(inService, now);
                    inService = -1;
                    completionTicks = Int64.MaxValue;
                    continue;
                }

                if (nextArrival < _song.Events.Count && arrivalTicks <= now)
                {
                    if (inService < 0)
                    {
                        inService = nextArrival;
                        completionTicks = checked(arrivalTicks + MicrosecondsToTicks(Interlocked.Read(ref _processingMicroseconds)));
                    }
                    else
                    {
                        lock (_sync) _droppedEvents++;
                    }
                    nextArrival++;
                    continue;
                }

                if (nextArrival >= _song.Events.Count && inService < 0)
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
