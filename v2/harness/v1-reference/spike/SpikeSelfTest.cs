// Self test of the shadow EventWaitHandle and the lock-step control wait,
// independent of v1's engine. Run: rendezvous-spike.exe --selftest
// (--selftest --hang additionally blocks forever so the in-process watchdog
// can be seen failing the run; use SPIKE_WATCHDOG_SECONDS=2).
using System;
using System.Threading;

namespace MidiBottleneck
{
    internal static class SpikeSelfTest
    {
        private const long Base = 10000000;
        private static int _failures;

        private static void Check(string name, bool ok, string detail)
        {
            Console.WriteLine((ok ? "PASS " : "FAIL ") + name + (detail == null ? "" : "  " + detail));
            if (!ok) _failures++;
        }

        internal static int Run(bool hang)
        {
            RendezvousSpike.SetLabel("selftest");
            if (hang)
            {
                RendezvousSpike.SetLabel("selftest-hang");
                Console.WriteLine("selftest --hang: blocking forever; the watchdog must end this run");
                Thread.Sleep(Timeout.Infinite);
            }

            // Reset semantics come from the kernel event: AutoReset consumes.
            EventWaitHandle auto = new EventWaitHandle(false, EventResetMode.AutoReset);
            EventWaitHandle manual = new EventWaitHandle(false, EventResetMode.ManualReset);
            auto.Set(); manual.Set();
            bool a1 = auto.WaitOne(0), a2 = auto.WaitOne(0), m1 = manual.WaitOne(0), m2 = manual.WaitOne(0);
            Check("AutoReset is consumed by one wait, ManualReset is not", a1 && !a2 && m1 && m2, null);
            manual.Reset();
            Check("Reset clears a ManualReset event", !manual.WaitOne(0), null);
            Check("a signalled event satisfies an untimed WaitOne() without the scheduler",
                SignalledUntimedWaitOnPlainThread(), null);

            // Control wait against a parked fake worker.
            VirtualTime.Set(Base);
            LockStepScheduler scheduler = new LockStepScheduler(10000);
            LockStepScheduler.Current = scheduler;
            int wakeGeneration = 0;
            scheduler.EngineWakeGeneration = delegate { return Volatile.Read(ref wakeGeneration); };
            EventWaitHandle wake = new EventWaitHandle(false, EventResetMode.ManualReset);
            EventWaitHandle ack = new EventWaitHandle(false, EventResetMode.ManualReset);
            EventWaitHandle pause = new EventWaitHandle(false, EventResetMode.ManualReset);

            // The fake worker: waits 10 virtual seconds (a timed park, like
            // the waitable timer), acknowledges, then blocks untimed like the
            // paused v1 worker, then exits when the control side sets `pause`.
            Thread worker = new Thread(delegate()
            {
                using (HighResolutionWaiter waiter = new HighResolutionWaiter(wake))
                {
                    waiter.WaitMicroseconds(10000000);
                    ack.Set();
                    pause.WaitOne();
                }
            });
            worker.IsBackground = true;
            worker.Start();

            scheduler.WaitForWorkerGeneration(1);
            long deadline, seq;
            bool parked = scheduler.WaitQuiescent(0, out deadline, out seq);
            Check("worker parks with its deadline 10 s ahead", parked && deadline == Base + 10000000, "deadline=" + (deadline - Base));

            Interlocked.Exchange(ref scheduler.CommandSeenSeq, seq);
            bool first = ack.WaitOne(2000);
            Check("control wait 2000 ms times out in virtual time when the worker's park is later",
                !first && VirtualTime.Now == Base + 2000000, "returned=" + first + " now=" + (VirtualTime.Now - Base));
            long d2, s2;
            Check("worker is still parked after the timeout", scheduler.WaitQuiescent(seq - 1, out d2, out s2) && s2 == seq, null);

            bool second = ack.WaitOne(20000);
            Check("control wait 20000 ms lends the token: worker released at its deadline, sets the handle, wait returns true",
                second && VirtualTime.Now == Base + 10000000, "returned=" + second + " now=" + (VirtualTime.Now - Base));

            // The worker is now (or about to be) blocked in the untimed wait.
            long d3, s3;
            Check("worker then waits with no deadline",
                scheduler.WaitQuiescent(Interlocked.Read(ref scheduler.CommandSeenSeq), out d3, out s3) && d3 == Int64.MaxValue, "d3=" + d3 + " s3=" + s3 + " seen=" + scheduler.CommandSeenSeq);
            Thread.Sleep(50);
            Check("an untimed wait is released by nothing but Set", worker.IsAlive && VirtualTime.Now == Base + 10000000, "alive=" + worker.IsAlive + " now=" + (VirtualTime.Now - Base));
            pause.Set();
            Check("Set releases the untimed wait and the worker exits", worker.Join(5000), null);

            // Control wait with no live worker: pure virtual-time timeout.
            ack.Reset();
            long before = VirtualTime.Now;
            bool none = ack.WaitOne(2000);
            Check("control wait with a finished worker times out after 2000 virtual ms",
                !none && VirtualTime.Now == before + 2000000, "now-before=" + (VirtualTime.Now - before));

            Console.WriteLine(_failures == 0 ? "SELFTEST: PASS" : "SELFTEST: " + _failures + " FAILED");
            return _failures == 0 ? 0 : 1;
        }

        private static bool SignalledUntimedWaitOnPlainThread()
        {
            LockStepScheduler saved = LockStepScheduler.Current;
            LockStepScheduler.Current = null;
            try
            {
                EventWaitHandle e = new EventWaitHandle(true, EventResetMode.ManualReset);
                return e.WaitOne();
            }
            finally { LockStepScheduler.Current = saved; }
        }
    }
}
