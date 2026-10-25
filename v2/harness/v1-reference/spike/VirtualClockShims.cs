// Rendezvous spike (bounded; not a harness feature). Compiled with the
// UNCHANGED v1.0.44 sources except HighResolutionWaiter.cs and Program.cs.
//
// C# binds a simple type name to a type in the enclosing namespace before
// types imported by `using` directives, so these MidiBottleneck types replace
// System.Diagnostics.Stopwatch, System.Threading.SpinWait and v1's own
// HighResolutionWaiter for every v1 file without editing it.
// EventWaitHandle is shadowed in EventWaitHandleShadow.cs.
using System;
using System.Threading;

namespace MidiBottleneck
{
    // 1 tick = 1 microsecond. Mutated only by whichever party currently owns
    // the lock-step token (the harness while the worker is parked, the worker
    // while it runs), so plain Interlocked access is sufficient.
    internal static class VirtualTime
    {
        internal static long Ticks;
        internal static long Now { get { return Interlocked.Read(ref Ticks); } }
        internal static void Set(long value) { Interlocked.Exchange(ref Ticks, value); }
        internal static void Advance(long micros) { if (micros > 0) Interlocked.Add(ref Ticks, micros); }
    }

    internal sealed class Stopwatch
    {
        public static readonly long Frequency = 1000000;
        public static readonly bool IsHighResolution = true;
        private long _start;
        private long _stoppedElapsed;
        private bool _running;
        public static long GetTimestamp()
        {
            long now = VirtualTime.Now;
            LockStepScheduler scheduler = LockStepScheduler.Current;
            if (scheduler != null) scheduler.ClockRead(now);
            return now;
        }
        public static Stopwatch StartNew() { Stopwatch s = new Stopwatch(); s.Start(); return s; }
        public void Start() { if (!_running) { _start = GetTimestamp() - _stoppedElapsed; _running = true; } }
        public void Stop() { if (_running) { _stoppedElapsed = GetTimestamp() - _start; _running = false; } }
        public void Restart() { _stoppedElapsed = 0; _running = false; Start(); }
        public void Reset() { _stoppedElapsed = 0; _running = false; }
        public bool IsRunning { get { return _running; } }
        public long ElapsedTicks { get { return _running ? GetTimestamp() - _start : _stoppedElapsed; } }
        public long ElapsedMilliseconds { get { return ElapsedTicks / 1000; } }
        public TimeSpan Elapsed { get { return TimeSpan.FromTicks(ElapsedTicks * 10); } }
    }

    // v1 spins only for the final sub-200 us after a waiter wait. The
    // rendezvous waiter always advances to the full deadline, so this is
    // normally never reached; if it is, each spin is 1 virtual microsecond.
    internal struct SpinWait
    {
        public void SpinOnce()
        {
            Interlocked.Increment(ref LockStepScheduler.SpinCount);
            VirtualTime.Advance(1);
        }
    }

    // Same surface as v1 HighResolutionWaiter. Construction happens first
    // thing on every PlaybackWorker thread, which is how the scheduler learns
    // the identity of the current worker thread.
    internal sealed class HighResolutionWaiter : IDisposable
    {
        private readonly EventWaitHandle _wake;

        public HighResolutionWaiter(EventWaitHandle wake)
        {
            _wake = wake;
            LockStepScheduler.Current.RegisterWorker(Thread.CurrentThread);
        }

        public bool WaitMicroseconds(long microseconds)
        {
            if (microseconds <= 0) return true;
            if (_wake.WaitOne(0)) return false;
            // Keep v1's overflow behaviour for absurd waits (checked coarse*10).
            long coarse = microseconds - 200;
            if (coarse > 0) { long unused = -checked(coarse * 10); }
            return LockStepScheduler.Current.Park(_wake, microseconds);
        }

        public void Dispose() { LockStepScheduler.Current.WorkerWaiterDisposed(); }
    }
}
