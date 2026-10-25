// Milestone 2 Step 4 feasibility proof (not a harness feature). Compiled with
// the UNCHANGED v1.0.44 sources except HighResolutionWaiter.cs (replaced
// below) and Program.cs. See v2/docs/ENGINE_HARNESS_FEASIBILITY.md.
//
// C# resolves a simple type name to a type in the enclosing namespace before
// types imported by `using` directives, so these MidiBottleneck types replace
// System.Diagnostics.Stopwatch and System.Threading.SpinWait for every v1 file.
using System;
using System.Reflection;
using System.Threading;

namespace MidiBottleneck
{
    internal static class VirtualTime
    {
        internal static long Ticks;  // 1 tick = 1 microsecond
    }

    internal sealed class Stopwatch
    {
        public static readonly long Frequency = 1000000;
        public static readonly bool IsHighResolution = true;
        private long _start;
        public static long GetTimestamp() { return Interlocked.Read(ref VirtualTime.Ticks); }
        public static Stopwatch StartNew() { Stopwatch s = new Stopwatch(); s._start = GetTimestamp(); return s; }
        public long ElapsedTicks { get { return GetTimestamp() - _start; } }
        public long ElapsedMilliseconds { get { return ElapsedTicks / 1000; } }
        public TimeSpan Elapsed { get { return TimeSpan.FromTicks(ElapsedTicks * 10); } }
    }

    // The engine spins after the coarse wait; each spin advances virtual time.
    internal struct SpinWait
    {
        public void SpinOnce() { Interlocked.Increment(ref VirtualTime.Ticks); }
    }

    // Same surface as v1 HighResolutionWaiter; waiting advances virtual time.
    internal sealed class HighResolutionWaiter : IDisposable
    {
        private readonly EventWaitHandle _wake;
        public HighResolutionWaiter(EventWaitHandle wake) { _wake = wake; }
        public bool WaitMicroseconds(long microseconds)
        {
            if (microseconds <= 0) return true;
            if (_wake.WaitOne(0)) return false;
            Interlocked.Add(ref VirtualTime.Ticks, microseconds);
            return !_wake.WaitOne(0);
        }
        public void Dispose() { }
    }

    internal static class VirtualTimePoc
    {
        private static int Main()
        {
            VirtualTime.Ticks = 5000000;
            long ticks = PlaybackEngine.MicrosecondsToTicks(1234);
            long micros = PlaybackEngine.TicksToMicroseconds(987654);
            Type waiterType = typeof(PlaybackEngine).GetMethod("WaitUntil", BindingFlags.NonPublic | BindingFlags.Instance)
                .GetParameters()[0].ParameterType;
            Console.WriteLine("v1 MicrosecondsToTicks(1234) = " + ticks + " (virtual 1 MHz clock: expected 1234)");
            Console.WriteLine("v1 TicksToMicroseconds(987654) = " + micros + " (expected 987654)");
            Console.WriteLine("v1 Stopwatch.GetTimestamp() as seen by v1 code = " + Stopwatch.GetTimestamp());
            Console.WriteLine("v1 WaitUntil waiter type = " + waiterType.FullName + " from " +
                (waiterType.Assembly == typeof(VirtualTimePoc).Assembly ? "this harness assembly" : "elsewhere"));
            bool ok = ticks == 1234 && micros == 987654;
            Console.WriteLine(ok ? "POC: PASS" : "POC: FAIL");
            return ok ? 0 : 1;
        }
    }
}
