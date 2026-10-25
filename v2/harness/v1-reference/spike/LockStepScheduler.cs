// Lock-step rendezvous between v1's PlaybackWorker thread and the harness.
// Exactly one of {harness thread, worker thread} runs engine code at a time:
//  - The worker runs until it calls HighResolutionWaiter.WaitMicroseconds,
//    which parks it (Park) and hands the token to the harness.
//  - The harness then either applies a scripted command (virtual time moved
//    to the command time) or advances virtual time to the parked deadline and
//    releases the worker with _go.
//  - The parked worker waits on {_go, engine._wake}. A command that calls
//    v1's SignalWake (Stop, Start, setters) therefore releases the worker the
//    same way the real waitable-timer wait would be interrupted; the harness
//    then waits until the worker parks again or its thread has exited.
//  - An overloaded worker may never wait (each Send advances virtual time),
//    so RecordingMidiOutput also yields after a completed Send once the next
//    scripted command is due (SendBoundary), released by v1's wake generation.
using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class LockStepScheduler
    {
        internal static LockStepScheduler Current;
        internal static long SpinCount;

        private readonly object _gate = new object();
        private readonly AutoResetEvent _go = new AutoResetEvent(false);
        private readonly int _hangMilliseconds;
        private Thread _worker;
        private int _workerGeneration;
        private bool _parked;
        private long _parkDeadline;
        private long _parkSeq;
        private int _parkGeneration;
        private readonly StringBuilder _trace = new StringBuilder();
        internal long Parks;
        internal long GoResumes;
        internal long WakeResumes;
        internal string ProtocolError;

        internal LockStepScheduler(int hangMilliseconds) { _hangMilliseconds = hangMilliseconds; }

        internal StringBuilder Trace { get { return _trace; } }

        // ---- worker side -------------------------------------------------

        // Start gate. A harness command that creates a worker and then keeps
        // running engine code (v1's Pause and Seek end with SignalWake after
        // starting the replacement thread) must not overlap that worker, or
        // the worker's first steps would depend on real-time timing. While a
        // command is active a newly started worker waits here, before it has
        // done anything, until the command has returned.
        private bool _commandActive;

        internal void BeginCommand() { lock (_gate) _commandActive = true; }

        internal void EndCommand()
        {
            lock (_gate)
            {
                _commandActive = false;
                Monitor.PulseAll(_gate);
            }
        }

        internal void RegisterWorker(Thread thread)
        {
            System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
            lock (_gate)
            {
                while (_commandActive && !FreeRunning)
                {
                    if (real.ElapsedMilliseconds > _hangMilliseconds)
                    {
                        ProtocolError = "new worker held longer than " + _hangMilliseconds + " ms while a command was active";
                        throw new TimeoutException("LockStepScheduler: " + ProtocolError);
                    }
                    Monitor.Wait(_gate, 5);
                }
                _worker = thread;
                _workerGeneration++;
                _trace.Append("WORKER-START gen=").Append(_workerGeneration)
                    .Append(" t=").Append(VirtualTime.Now).Append('\n');
                Monitor.PulseAll(_gate);
            }
        }

        // Clock-polling livelock guard. A worker that reads the virtual clock
        // over and over while the virtual time cannot move (v1 busy-polls
        // Stopwatch in a few loops that have no wait, for example the ordered
        // path while a live channel control or an Output-paced service hold is
        // pending and the next source event is already due) would run forever
        // under a virtual clock. After MaximumIdleReads consecutive reads at
        // one virtual time the worker is failed with a TimeoutException, which
        // v1 reports as PlaybackFailed, instead of hanging until the 20 s real
        // time limit. No scenario in the committed file reaches it.
        internal const long MaximumIdleReads = 5000000;
        private long _idleReadTime = -1;
        private long _idleReads;

        internal void ClockRead(long now)
        {
            if (FreeRunning || Thread.CurrentThread != Volatile.Read(ref _worker)) return;
            if (now != _idleReadTime) { _idleReadTime = now; _idleReads = 0; return; }
            if (++_idleReads < MaximumIdleReads) return;
            _idleReads = 0;
            ProtocolError = "worker polled the clock " + MaximumIdleReads + " times at virtual time " + now + " (busy-poll livelock)";
            throw new TimeoutException("LockStepScheduler: " + ProtocolError);
        }

        internal void WorkerWaiterDisposed()
        {
            lock (_gate)
                _trace.Append("WORKER-WAITER-DISPOSE t=").Append(VirtualTime.Now).Append('\n');
        }

        // Second rendezvous kind: after each completed output Send, if a
        // scripted command is due (virtual now >= NextCommandTime), the worker
        // yields with deadline = now so the harness can apply the command
        // "during" the worker's dispatch run, where a real UI command could
        // land. v1's _wake may legitimately be left set for a whole dispatch
        // run (Start signals it and an overloaded worker never reaches
        // WaitUntil's Reset), so a yield cannot use _wake as its release
        // signal. It uses v1's _wakeGeneration instead: SignalWake (the only
        // _wake.Set in v1) always increments it first, so a command that
        // would interrupt a real wait releases the yield too.
        internal void SendBoundary()
        {
            if (FreeRunning || Thread.CurrentThread != Volatile.Read(ref _worker)) return;
            if (VirtualTime.Now < Interlocked.Read(ref NextCommandTime)) return;
            if (EngineWakeGeneration == null || EnginePlaying == null || !EnginePlaying()) return;
            SendYields++;
            Park(null, 0, "YIELD");
        }

        internal long NextCommandTime = Int64.MaxValue;
        internal Func<int> EngineWakeGeneration;
        // Yields only while v1 is Playing: Stop/Pause/Seek set Stopped before
        // SignalWake and then Join, so a worker draining after them must not
        // block. The state is written only by the harness command (before the
        // wake) and read by the worker, so the decision is deterministic.
        internal Func<bool> EnginePlaying;
        internal long SendYields;

        // Negative control (SPIKE_MODE=free): no rendezvous. The waiter just
        // advances virtual time like the Milestone 2 PoC and the harness applies
        // commands concurrently by polling the clock.
        internal static readonly bool FreeRunning =
            Environment.GetEnvironmentVariable("SPIKE_MODE") == "free";

        internal bool Park(EventWaitHandle wake, long microseconds)
        {
            if (FreeRunning)
            {
                Interlocked.Increment(ref Parks);
                VirtualTime.Advance(microseconds);
                return !wake.WaitOne(0);
            }
            return Park(wake, microseconds, "PARK");
        }

        private bool Park(EventWaitHandle wake, long microseconds, string kind)
        {
            int outcome = ParkCore(wake, microseconds, kind);
            if (outcome == 1) return false;
            return wake == null || !wake.PollSignaled();
        }

        // Worker side of the shadow EventWaitHandle: v1's blocking wait on an
        // event. Untimed (milliseconds < 0) is a park with NO deadline that
        // only Set releases; timed is a park with a virtual deadline. Returns
        // true when the event was signalled, false on timeout.
        internal bool WaitFromWorker(EventWaitHandle handle, int milliseconds)
        {
            if (handle.PollSignaled()) return true;
            bool untimed = milliseconds < 0;
            long micro = untimed ? Int64.MaxValue : milliseconds * 1000L;
            Interlocked.Increment(ref EventWaits);
            return ParkCore(handle, micro, untimed ? "WAIT" : "WAITT") == 1;
        }

        internal long EventWaits;

        // Returns 1 when released by the wake handle, 0 when released by the
        // harness (_go) at the deadline.
        private int ParkCore(EventWaitHandle wake, long microseconds, string kind)
        {
            long now = VirtualTime.Now;
            _idleReadTime = -1;
            long deadline = now > Int64.MaxValue - microseconds ? Int64.MaxValue : now + microseconds;
            int generation = EngineWakeGeneration == null ? 0 : EngineWakeGeneration();
            lock (_gate)
            {
                _parked = true;
                _parkDeadline = deadline;
                _parkGeneration = generation;
                _parkSeq++;
                Parks++;
                _trace.Append(kind).Append(" t=").Append(now).Append(" until=");
                if (microseconds == Int64.MaxValue) _trace.Append("none");
                else _trace.Append(deadline);
                _trace.Append('\n');
                Monitor.PulseAll(_gate);
            }
            bool woken;
            if (wake != null)
            {
                // Timed park: v1 entered it with _wake clear (WaitMicroseconds
                // returns early otherwise), exactly like the real timer wait.
                int index = WaitHandle.WaitAny(new WaitHandle[] { _go, wake.Handle }, _hangMilliseconds);
                if (index == WaitHandle.WaitTimeout) Hang();
                woken = index == 1;
            }
            else
            {
                // Yield: poll v1's wake generation. Real-time polling cannot
                // change the outcome: the harness thread is either blocked in
                // Join or done with the command when the change is seen.
                System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
                while (true)
                {
                    if (_go.WaitOne(1)) { woken = false; break; }
                    if (EngineWakeGeneration() != generation) { woken = true; break; }
                    if (real.ElapsedMilliseconds > _hangMilliseconds) Hang();
                }
            }
            lock (_gate)
            {
                _parked = false;
                Monitor.PulseAll(_gate);
                if (woken)
                {
                    WakeResumes++;
                    _trace.Append("RESUME-WAKE t=").Append(VirtualTime.Now).Append('\n');
                    if (_go.WaitOne(0)) ProtocolError = "go and wake were both pending";
                    return 1;
                }
                GoResumes++;
                _trace.Append("RESUME-GO t=").Append(VirtualTime.Now).Append('\n');
            }
            return 0;
        }

        private void Hang()
        {
            lock (_gate)
            {
                _parked = false;
                ProtocolError = "worker parked longer than " + _hangMilliseconds + " ms real time";
            }
            throw new TimeoutException("LockStepScheduler: " + ProtocolError);
        }

        // ---- harness side ------------------------------------------------

        internal int WorkerGeneration { get { lock (_gate) return _workerGeneration; } }
        internal long ParkSeq { get { lock (_gate) return _parkSeq; } }

        internal void WaitForWorkerGeneration(int generation)
        {
            System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
            lock (_gate)
            {
                while (_workerGeneration < generation)
                {
                    if (real.ElapsedMilliseconds > _hangMilliseconds)
                        throw new TimeoutException("LockStepScheduler: worker never started");
                    Monitor.Wait(_gate, 5);
                }
            }
        }

        // Waits until the worker is parked in a park newer than seenSeq, or
        // the worker thread has exited. Returns true (with the deadline) when
        // parked; false when no worker thread is alive.
        internal bool WaitQuiescent(long seenSeq, out long deadline, out long parkSeq)
        {
            System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
            Thread worker;
            lock (_gate)
            {
                while (true)
                {
                    if (_parked && _parkSeq > seenSeq)
                    {
                        deadline = _parkDeadline;
                        parkSeq = _parkSeq;
                        return true;
                    }
                    worker = _worker;
                    if (worker == null || !worker.IsAlive) break;
                    if (real.ElapsedMilliseconds > _hangMilliseconds)
                        throw new TimeoutException("LockStepScheduler: worker neither parked nor exited (hang)");
                    Monitor.Wait(_gate, 2);
                }
            }
            if (worker != null && !worker.Join(_hangMilliseconds))
                throw new TimeoutException("LockStepScheduler: worker did not exit");
            deadline = 0;
            parkSeq = ParkSeq;
            return false;
        }

        // After a harness command ran while the worker was parked at parkSeq:
        // did the command release the worker (v1 SignalWake: the wake
        // generation moved) or has the worker left that park / exited?
        internal bool CommandReleasedWorker(long parkSeq, int wakeGenerationBefore)
        {
            if (EngineWakeGeneration() != wakeGenerationBefore) return true;
            lock (_gate) return !(_parked && _parkSeq == parkSeq);
        }

        internal bool IsWorkerThread
        {
            get { return Thread.CurrentThread == Volatile.Read(ref _worker); }
        }

        // The newest park the running harness command already accounts for
        // (the park it started at, or one it released itself inside a control
        // wait). The driver sets it to the park sequence before Apply and
        // reads it back afterwards.
        internal long CommandSeenSeq;

        // Control-thread side of the shadow EventWaitHandle: a wait with a
        // timeout in virtual milliseconds (v1: _pauseAcknowledged.WaitOne in
        // Pause, called from inside a harness command, with the worker parked
        // or about to leave a park). The control thread does not block the
        // schedule; it lends the token to the worker:
        //  - the worker still leaving a park that the command's wake signal
        //    releases, or running toward its next park: wait for it (real
        //    time only, no virtual time passes);
        //  - the worker parked for good (a park newer than the command's, or
        //    one nothing has signalled) with the handle still clear: if that
        //    park's deadline is within the control deadline, advance virtual
        //    time to it and release the worker; otherwise (a later deadline,
        //    or none) virtual time moves to the control deadline and the
        //    wait times out;
        //  - the handle set, at any point: return true.
        // The handle is re-checked after the worker is seen parked, so a Set
        // made just before the park is never missed.
        internal bool WaitFromControl(EventWaitHandle handle, int milliseconds)
        {
            long controlDeadline = milliseconds < 0 ? Int64.MaxValue : SaturatingAdd(VirtualTime.Now, milliseconds * 1000L);
            System.Diagnostics.Stopwatch real = System.Diagnostics.Stopwatch.StartNew();
            Interlocked.Increment(ref ControlWaits);
            long releasedSeq = -1;   // a park this wait already released (the worker may not have woken yet)
            while (true)
            {
                bool parked;
                long deadline, seq;
                int generation;
                bool alive;
                lock (_gate)
                {
                    parked = _parked;
                    deadline = _parkDeadline;
                    seq = _parkSeq;
                    generation = _parkGeneration;
                    Thread w = _worker;
                    alive = w != null && w.IsAlive;
                }
                bool stuck = parked && seq != releasedSeq &&
                    (seq > Interlocked.Read(ref CommandSeenSeq) || EngineWakeGeneration() == generation);
                if (stuck)
                {
                    if (handle.PollSignaled()) return true;
                    if (controlDeadline == Int64.MaxValue && deadline == Int64.MaxValue)
                    {
                        ProtocolError = "control wait and worker park both have no deadline";
                        throw new InvalidOperationException("LockStepScheduler: " + ProtocolError);
                    }
                    if (deadline != Int64.MaxValue && deadline <= controlDeadline)
                    {
                        Note("CONTROL-WAIT-RELEASE t=" + VirtualTime.Now + " until=" + deadline);
                        Interlocked.Exchange(ref CommandSeenSeq, seq);
                        releasedSeq = seq;
                        ReleaseToDeadline(deadline);
                        real.Restart();
                        continue;
                    }
                    return ControlTimeout(controlDeadline);
                }
                if (!parked && !alive)
                {
                    if (handle.PollSignaled()) return true;
                    return ControlTimeout(controlDeadline);
                }
                if (handle.PollSignaled()) return true;
                if (real.ElapsedMilliseconds > _hangMilliseconds)
                {
                    ProtocolError = "control wait: worker neither signalled nor parked within " + _hangMilliseconds + " ms real time";
                    throw new TimeoutException("LockStepScheduler: " + ProtocolError);
                }
                lock (_gate) Monitor.Wait(_gate, 1);
            }
        }

        internal long ControlWaits;
        internal long ControlTimeouts;

        private bool ControlTimeout(long controlDeadline)
        {
            if (controlDeadline != Int64.MaxValue && VirtualTime.Now < controlDeadline) VirtualTime.Set(controlDeadline);
            Interlocked.Increment(ref ControlTimeouts);
            Note("CONTROL-WAIT-TIMEOUT t=" + VirtualTime.Now);
            return false;
        }

        private static long SaturatingAdd(long a, long b)
        {
            return a > Int64.MaxValue - b ? Int64.MaxValue : a + b;
        }

        internal void ReleaseToDeadline(long deadline)
        {
            if (VirtualTime.Now < deadline) VirtualTime.Set(deadline);
            _go.Set();
        }

        internal void Note(string line)
        {
            lock (_gate) _trace.Append(line).Append('\n');
        }
    }
}
