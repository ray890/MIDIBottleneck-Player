using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MidiBottleneck
{
    internal sealed class HighResolutionWaiter : IDisposable
    {
        private const uint CreateWaitableTimerHighResolution = 0x00000002;
        private const uint TimerAllAccess = 0x001F0003;
        private const uint Infinite = 0xFFFFFFFF;
        private readonly IntPtr _timer;
        private readonly System.Threading.EventWaitHandle _wake;

        public HighResolutionWaiter(System.Threading.EventWaitHandle wake)
        {
            _wake = wake;
            try
            {
                _timer = CreateWaitableTimerEx(IntPtr.Zero, null, CreateWaitableTimerHighResolution, TimerAllAccess);
            }
            catch (EntryPointNotFoundException)
            {
                _timer = IntPtr.Zero;
            }
            if (_timer == IntPtr.Zero)
                _timer = CreateWaitableTimer(IntPtr.Zero, false, null);
            if (_timer == IntPtr.Zero)
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Unable to create the playback timer.");
        }

        public bool WaitMicroseconds(long microseconds)
        {
            if (microseconds <= 0) return true;
            if (_wake.WaitOne(0)) return false;

            // Let the kernel timer handle the coarse portion, then yield/spin only
            // for the final 200 microseconds. This avoids a continuously busy core.
            long coarse = microseconds - 200;
            if (coarse > 0)
            {
                long dueTime100Nanoseconds = -checked(coarse * 10);
                if (!SetWaitableTimer(_timer, ref dueTime100Nanoseconds, 0, IntPtr.Zero, IntPtr.Zero, false))
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Unable to arm the playback timer.");
                IntPtr[] handles = new IntPtr[] { _timer, _wake.SafeWaitHandle.DangerousGetHandle() };
                uint wait = WaitForMultipleObjects(2, handles, false, Infinite);
                if (wait == 0xFFFFFFFF)
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "The playback timer wait failed.");
                if (wait == 1) return false;
            }
            return !_wake.WaitOne(0);
        }

        public void Dispose()
        {
            CancelWaitableTimer(_timer);
            CloseHandle(_timer);
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateWaitableTimerEx(IntPtr attributes, string name, uint flags, uint desiredAccess);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateWaitableTimer(IntPtr attributes, bool manualReset, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetWaitableTimer(IntPtr timer, ref long dueTime, int period, IntPtr completionRoutine, IntPtr argument, bool resume);
        [DllImport("kernel32.dll")]
        private static extern bool CancelWaitableTimer(IntPtr timer);
        [DllImport("kernel32.dll")]
        private static extern uint WaitForMultipleObjects(uint count, IntPtr[] handles, bool waitAll, uint milliseconds);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
