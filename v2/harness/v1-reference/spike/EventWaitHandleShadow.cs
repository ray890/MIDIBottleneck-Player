// Shadow System.Threading.EventWaitHandle for the rendezvous spike (compiled
// into the spike only; v1 sources are not edited).
//
// C# binds a simple type name to a type in the enclosing namespace before
// types imported by `using System.Threading;`, so this MidiBottleneck type
// replaces EventWaitHandle in every v1 file. v1 uses it only for two fields of
// PlaybackEngine (grep src/ for EventWaitHandle):
//   _wake              ManualReset. Set by SignalWake; Reset by the worker;
//                      polled with WaitOne(0); the paused worker blocks in an
//                      untimed WaitOne() (PlaybackEngine.cs WaitWhilePaused).
//   _pauseAcknowledged ManualReset. Set by the worker when a playback-
//                      preserving Pause is acknowledged; the control thread
//                      waits on it with WaitOne(timeout) (Pause, 2000 ms).
//
// State (signalled/clear, AutoReset/ManualReset) lives in a real kernel
// event, so Set/Reset/WaitOne(0) have exactly the real semantics. Only the
// two BLOCKING waits are routed to the lock-step scheduler:
//   worker thread, untimed:  a park with no deadline, released only by Set.
//   worker thread, timed:    a park with a virtual deadline (v1 does not do
//                            this; supported for completeness).
//   any other thread, timed: the control wait, which lends the token to the
//                            worker and models the timeout in virtual time.
// Not routed (plain kernel behaviour): the negative-control mode
// (SPIKE_MODE=free) and any wait made when no scheduler is installed.
using System;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class EventWaitHandle : IDisposable
    {
        private readonly System.Threading.EventWaitHandle _real;

        public EventWaitHandle(bool initialState, EventResetMode mode)
        {
            _real = new System.Threading.EventWaitHandle(initialState, mode);
        }

        // The kernel event, for the scheduler's WaitAny rendezvous.
        internal WaitHandle Handle { get { return _real; } }

        // Non-blocking test; consumes the signal for AutoReset, like WaitOne(0).
        internal bool PollSignaled() { return _real.WaitOne(0); }

        public bool Set() { return _real.Set(); }

        public bool Reset() { return _real.Reset(); }

        public bool WaitOne() { return WaitOne(Timeout.Infinite); }

        public bool WaitOne(int millisecondsTimeout)
        {
            if (millisecondsTimeout < -1) throw new ArgumentOutOfRangeException("millisecondsTimeout");
            if (millisecondsTimeout == 0) return _real.WaitOne(0);
            LockStepScheduler scheduler = LockStepScheduler.Current;
            if (scheduler == null || LockStepScheduler.FreeRunning)
                return _real.WaitOne(millisecondsTimeout);
            if (scheduler.IsWorkerThread)
                return scheduler.WaitFromWorker(this, millisecondsTimeout);
            if (millisecondsTimeout < 0)
                throw new InvalidOperationException("Untimed EventWaitHandle wait on a non-worker thread would block the lock-step schedule.");
            return scheduler.WaitFromControl(this, millisecondsTimeout);
        }

        public void Dispose() { _real.Dispose(); }
    }
}
