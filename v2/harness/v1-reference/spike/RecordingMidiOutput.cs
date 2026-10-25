// Recording IMidiOutput for the rendezvous spike. Every Send/Reset/Panic is
// recorded with the virtual timestamp and the calling party (W = v1 worker
// thread, H = harness thread). Each Send then advances virtual time by a
// scripted service cost, standing in for the time a real driver call takes.
using System;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal sealed class RecordingMidiOutput : IMidiOutput, IMidiOutputContext, IDisposable
    {
        // 31,250 bit/s with 10 bits per byte on the wire = 320 us per byte.
        internal const long MicrosecondsPerByte = 320;

        private readonly StringBuilder _log;
        private readonly Thread _harnessThread;
        private readonly long _fixedShortCost;
        private readonly bool _costFromBytes;
        internal long Sends;
        internal long Resets;
        internal long Panics;

        // costFromBytes: cost = bytes * 320 us; otherwise fixedShortCost per
        // short message and bytes * 320 us for SysEx.
        internal RecordingMidiOutput(StringBuilder log, Thread harnessThread, bool costFromBytes, long fixedShortCost)
        {
            _log = log;
            _harnessThread = harnessThread;
            _costFromBytes = costFromBytes;
            _fixedShortCost = fixedShortCost;
        }

        public string SourceFile { get; set; }

        private char Who { get { return Thread.CurrentThread == _harnessThread ? 'H' : 'W'; } }

        public void Send(MidiEventView midiEvent)
        {
            MidiEventData data = midiEvent.Data;
            int length = data.Length;
            long cost;
            if (_costFromBytes || length > 3 || midiEvent.Kind == MidiEventKind.SystemExclusive)
                cost = Math.Max(1, length) * MicrosecondsPerByte;
            else cost = _fixedShortCost;
            lock (_log)
            {
                _log.Append(VirtualTime.Now).Append(' ').Append(Who).Append(" SEND ")
                    .Append(midiEvent.Kind).Append(" src=").Append(midiEvent.EventIndex).Append(' ');
                for (int i = 0; i < length; i++) _log.Append(data[i].ToString("X2"));
                _log.Append(" cost=").Append(cost).Append('\n');
            }
            Sends++;
            VirtualTime.Advance(cost);
            LockStepScheduler scheduler = LockStepScheduler.Current;
            if (scheduler != null && YieldAtSendBoundary) scheduler.SendBoundary();
        }

        internal bool YieldAtSendBoundary = true;

        public void Panic()
        {
            Panics++;
            lock (_log) _log.Append(VirtualTime.Now).Append(' ').Append(Who).Append(" PANIC\n");
        }

        public void Reset()
        {
            Resets++;
            lock (_log) _log.Append(VirtualTime.Now).Append(' ').Append(Who).Append(" RESET\n");
        }

        public void Dispose() { }
    }
}
