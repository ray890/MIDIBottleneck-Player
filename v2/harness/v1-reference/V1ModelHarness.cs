// v1 model traces (Milestone 3). Drives v1's own model classes, unchanged,
// with scripted inputs over fixture event streams, and records every call,
// its result, any exception, and the model state after the call.
//
//   models <family> <fixture-dir> <out-trace>
//
// Line kinds (TRACE_FORMAT.md, "Model traces"):
//   > op      a call, with every input spelled out (v2 replays these lines)
//   = text    the call's result
//   ! text    the .NET exception the call threw: type, parameter, message
//   ~ text    model state after the call (private fields read by reflection)
// The driver chooses ops with a seeded generator and from v1's results; v2
// never re-derives that choice, it replays the recorded ops.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal static partial class V1ModelHarness
    {
        private const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public |
            BindingFlags.Instance | BindingFlags.Static;
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        private static StringBuilder _out;
        private static string _fixtureDir;
        private static MidiSong _song;
        private static MidiEventReader _events;
        private static uint _rng;

        internal static int Run(string[] args)
        {
            if (args.Length != 4) throw new ArgumentException("usage: models <family> <fixture-dir> <out-trace>");
            string family = args[1];
            _fixtureDir = args[2];
            _out = new StringBuilder();
            Line("# producer v1-reference (MIDIBottleneck v1.0.44 sources) " +
                (IntPtr.Size == 8 ? "x64" : "x86") + " clr " + Environment.Version);
            Line("model-trace 1");
            Line("family " + family);
            if (family == "rate") RateFamily();
            else if (family == "queue") QueueFamily();
            else if (family == "ordered") OrderedFamily();
            else if (family == "analysis") AnalysisFamily();
            else if (family == "routing") RoutingFamily();
            else if (family == "routing-large") RoutingLargeFamily();
            else if (!GateFamily(family)) throw new ArgumentException("unknown model family " + family);
            Line("[end]");
            File.WriteAllBytes(args[3], Encoding.ASCII.GetBytes(_out.ToString()));
            return 0;
        }

        // ------------------------------------------------------------ output

        private static void Line(string text) { _out.Append(text).Append('\n'); }
        private static string S(long value) { return value.ToString(Inv); }
        private static string B(bool value) { return value ? "1" : "0"; }

        private static void Section(string name) { Line("[" + name + "]"); }

        // Records one call: the op line, its result or exception, and the state.
        private static void Op(string text, Func<string> action, Func<string> state)
        {
            Line("> " + text);
            try
            {
                string result = action();
                if (result != null) Line("= " + result);
            }
            catch (Exception ex) { Line(ExceptionLine(ex)); }
            if (state != null) Line("~ " + state());
        }

        private static string ExceptionLine(Exception ex)
        {
            string param = "-";
            ArgumentException argument = ex as ArgumentException;
            if (argument != null && !String.IsNullOrEmpty(argument.ParamName)) param = argument.ParamName;
            string message = ex.Message;
            int cut = message.IndexOfAny(new[] { '\r', '\n' });
            if (cut >= 0) message = message.Substring(0, cut);
            return "! " + ex.GetType().Name + " " + param + " " + message;
        }

        // FNV-1a 64 of ASCII text, as 16 lower-case hex digits.
        private static string Digest(string text)
        {
            ulong hash = 14695981039346656037UL;
            foreach (char c in text)
            {
                hash ^= (byte)c;
                hash *= 1099511628211UL;
            }
            return hash.ToString("x16", Inv);
        }

        private static uint Next()
        {
            uint x = _rng;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            _rng = x;
            return x;
        }

        private static int Pick(int count) { return (int)(Next() % (uint)count); }

        private static object F(object target, string field)
        {
            FieldInfo info = target.GetType().GetField(field, Hidden);
            if (info == null) throw new MissingFieldException(target.GetType().Name, field);
            return info.GetValue(target);
        }

        private static long L(object target, string field) { return Convert.ToInt64(F(target, field), Inv); }
        private static bool Z(object target, string field) { return (bool)F(target, field); }

        // ------------------------------------------------------------ events

        private static void LoadFixture(string name)
        {
            Line("> fixture " + name);
            _song = DirectCompactMidiParser.Load(Path.Combine(_fixtureDir, name), CancellationToken.None, null);
            _events = _song.GetEventReader();
            Line("= events " + S(_events.Count) + " duration " + S(_song.DurationMicroseconds));
        }

        // Event token for op lines: e<index>[@<routed channel>][#<logical index>], or x (default view).
        private static string Tok(MidiEventView e, int sourceIndex, int routed, int logical)
        {
            if (!e.IsValid) return "x";
            string text = "e" + S(sourceIndex);
            if (routed >= 0) text += "@" + S(routed);
            if (logical != Int32.MinValue) text += "#" + S(logical);
            return text;
        }
        private static string Tok(int index) { return "e" + S(index); }

        internal static string Ev(MidiEventView e)
        {
            if (!e.IsValid) return "invalid";
            StringBuilder text = new StringBuilder();
            text.Append(S(e.EventIndex)).Append(':').Append(S(e.AbsoluteTick)).Append(':')
                .Append(S(e.IntendedMicroseconds)).Append(':').Append(S(e.Track)).Append(':')
                .Append(S(e.Channel)).Append(':').Append(e.Status.ToString("X2", Inv)).Append(':')
                .Append(S(e.DataLength));
            // Only channel-message bytes: no model reads system/SysEx bytes.
            if (e.Status < 0xF0)
            {
                text.Append(':');
                for (int i = 0; i < Math.Min(3, e.DataLength); i++) text.Append(e.GetDataByte(i).ToString("X2", Inv));
            }
            return text.ToString();
        }

        private static string EvList(IList<MidiEventView> events, int count)
        {
            StringBuilder text = new StringBuilder("[");
            for (int i = 0; i < count; i++)
            {
                if (i > 0) text.Append(',');
                text.Append(Ev(events[i]));
            }
            return text.Append(']').ToString();
        }

        private static long Sat(long value, long delta)
        {
            if (delta > 0 && value > Int64.MaxValue - delta) return Int64.MaxValue;
            if (delta < 0 && value < Int64.MinValue - delta) return Int64.MinValue;
            return value + delta;
        }

        // ============================================================== rate

        private static ServiceDurationClock _clock;
        private static ProgressiveRatePacer _pacer;
        private static AdaptiveNoteIntervalClock _adaptive;

        private static string ClockState()
        {
            object boxed = _clock;
            return "rem " + S(L(boxed, "_remainder"));
        }

        internal static string PacerState(ProgressiveRatePacer pacer)
        {
            if (pacer == null) return "none";
            object queue = F(pacer, "_history");
            int queued = (int)queue.GetType().GetProperty("Count").GetValue(queue, null);
            string tail = "-";
            if (Z(pacer, "_hasTail"))
            {
                object stamp = F(pacer, "_tail");
                tail = S(L(stamp, "Timestamp")) + ":" + S(L(stamp, "Count"));
            }
            return "i" + B(Z(pacer, "_initialized")) + " x" + B(Z(pacer, "_exhausted")) + " w" + S(L(pacer, "_nextWhole")) +
                " r" + S(L(pacer, "_nextRemainder")) + " h" + S(L(pacer, "_historyCount")) + " v" + S(L(pacer, "_reservedCount")) +
                " q" + S(queued) + " t" + tail;
        }

        internal static string AdaptiveState(AdaptiveNoteIntervalClock adaptive)
        {
            return "next " + S(adaptive.NextAllowedBoundaryMicroseconds) + " open " + S(adaptive.OpenBoundaryMicroseconds) +
                " count " + S(L(adaptive, "_openOutputCount"));
        }

        private static readonly string[] ModeNames = { "proc", "bitrate", "eps" };

        private static void RateFamily()
        {
            _rng = 0x5EED0001;
            Section("service-durations");
            long[] processing = { -5, 0, 1, 333, 1000000, Int64.MaxValue };
            long[] bitrates = { -1, 0, 1, 31250, 9999999, Int64.MaxValue };
            long[] rates = { -1, 0, 1, 3, 7, 3000, 9999999, 10000000, Int64.MaxValue };
            List<long[]> configs = new List<long[]>();
            foreach (long p in processing) configs.Add(new long[] { 0, p, 31250, 3000 });
            foreach (long b in bitrates) configs.Add(new long[] { 1, 0, b, 0 });
            foreach (long r in rates) configs.Add(new long[] { 2, 7, 31250, r });
            long[] counts = { 0, -3, 1, 7, 1000, 9223372036855, Int64.MaxValue };
            foreach (string fixture in new[] { "models-notes.mid", "f0-all-message-types.mid", "long-sysex-segment-crossing.mid" })
            {
                LoadFixture(fixture);
                int cursor = 0;
                foreach (long[] c in configs)
                {
                    ServiceDurationMode mode = (ServiceDurationMode)c[0];
                    string args = ModeNames[c[0]] + " " + S(c[1]) + " " + S(c[2]) + " " + S(c[3]);
                    Op("settings.immediate " + args, () =>
                        B(new ServiceDurationSettings(mode, c[1], c[2], c[3]).IsImmediate), null);
                    Op("new clock " + args, () => { _clock = new ServiceDurationClock(mode, c[1], c[2], c[3]); return null; }, ClockState);
                    Op("clock.immediate", () => B(_clock.IsImmediate), null);
                    // Live setting change: the next clock continues the same event stream.
                    for (int i = 0; i < 40 && _events.Count > 0; i++)
                    {
                        int index = cursor++ % _events.Count;
                        Op("clock.next " + Tok(index), () => S(_clock.NextMicroseconds(_events[index])), ClockState);
                        if (i % 13 == 0)
                        {
                            long count = counts[Pick(counts.Length)];
                            Op("clock.total " + S(count), () => S(_clock.TotalForNextEvents(count)), ClockState);
                            // A look-ahead copy never changes the generation's clock.
                            Op("clock.peek " + Tok(index), () => { ServiceDurationClock copy = _clock; return S(copy.NextMicroseconds(_events[index])); }, ClockState);
                        }
                    }
                    if (_events.Count > 0)
                        Op("svc.calc " + Tok(0) + " " + args, () => S(ServiceDurationCalculator.CalculateMicroseconds(_events[0], mode, c[1], c[2])), null);
                    Op("svc.calc x " + args, () => S(ServiceDurationCalculator.CalculateMicroseconds(default(MidiEventView), mode, c[1], c[2])), null);
                }
            }
            int[] byteCounts = { -1, 0, 1, 2, 3, 4, 1000, 1200000, Int32.MaxValue };
            long[] bitrateValues = { -1, 0, 1, 3, 31250, 1000000, 9999999, Int64.MaxValue / 2, Int64.MaxValue };
            foreach (int n in byteCounts)
                foreach (long b in bitrateValues)
                    Op("svc.bitrate " + S(n) + " " + S(b), () => S(ServiceDurationCalculator.CalculateBitrateMicroseconds(n, b)), null);

            PacerSection();
            AdaptiveSection();
        }

        private static void PacerSection()
        {
            Section("progressive-pacer");
            _rng = 0x5EED0002;
            long[] rates = { 0, 1, 2, 19, 20, 21, 39, 40, 41, 1000, 3000, 9999999 };
            long[] units = { 1000000, 10000000, 1, 7 };
            long[] bursts = { 1, 2, 64 };
            List<long[]> configs = new List<long[]>();
            foreach (long r in rates) foreach (long u in units) foreach (long b in bursts) configs.Add(new long[] { r, u, b });
            configs.Add(new long[] { -1, 1000000, 1 });
            configs.Add(new long[] { 10000000, 1000000, 1 });
            configs.Add(new long[] { 1000, 0, 1 });
            configs.Add(new long[] { 1000, 1000000, 0 });
            configs.Add(new long[] { 1000, 1000000, Int64.MaxValue });
            configs.Add(new long[] { 3, Int64.MaxValue, 1 });
            configs.Add(new long[] { 9999999, Int64.MaxValue, 5 });
            long[] starts = { 0, 5, 999999, -40, Int64.MaxValue - 3, Int64.MaxValue - 2000000 };
            int configIndex = 0;
            foreach (long[] c in configs)
            {
                long rate = c[0], unit = c[1], burst = c[2];
                Op("new pacer " + S(rate) + " " + S(unit) + " " + S(burst), () =>
                {
                    _pacer = null;
                    _pacer = new ProgressiveRatePacer(rate, unit, burst);
                    return null;
                }, () => PacerState(_pacer));
                if (_pacer == null) continue;
                long ts = starts[configIndex++ % starts.Length];
                long step = rate == 0 ? 1 : Math.Max(1, unit / rate);
                for (int i = 0; i < 70; i++)
                {
                    int kind = Pick(20);
                    long delta;
                    int d = Pick(10);
                    if (d < 3) delta = 0;
                    else if (d < 5) delta = step;
                    else if (d < 6) delta = 1;
                    else if (d < 7) delta = unit / 2;
                    else if (d < 8) delta = unit + Pick(3) - 1;
                    else if (d < 9) delta = (long)Pick(1000);
                    else delta = Pick(8) == 0 ? -(long)Pick(1000) - 1 : 2 * unit;
                    ts = Sat(ts, delta);
                    long t = ts;
                    long max = new long[] { 0, 1, 2, 5, 64, Int64.MaxValue }[Pick(6)];
                    if (kind < 5) Op("pacer.consume " + S(t), () => B(_pacer.TryConsume(t)), () => PacerState(_pacer));
                    else if (kind < 8) Op("pacer.avail " + S(t) + " " + S(max), () => S(_pacer.AvailableSlots(t, max)), () => PacerState(_pacer));
                    else if (kind < 11) Op("pacer.reserve " + S(t), () => B(_pacer.TryReserve(t)), () => PacerState(_pacer));
                    else if (kind < 12) Op("pacer.ahead " + S(t), () => B(_pacer.TryReserveAhead(t)), () => PacerState(_pacer));
                    else if (kind < 14) Op("pacer.rolling " + S(t) + " " + S(max), () => S(_pacer.RollingAvailableSlots(t, max)), () => PacerState(_pacer));
                    else if (kind < 17) Op("pacer.complete " + S(t), () => { _pacer.CompleteReservation(t); return null; }, () => PacerState(_pacer));
                    else if (kind < 18) Op("pacer.cancel", () => { _pacer.CancelReservation(); return null; }, () => PacerState(_pacer));
                    else
                    {
                        // A simultaneous same-time batch: consume until refused.
                        for (int k = 0; k < 12; k++)
                        {
                            bool consumed = false;
                            Op("pacer.consume " + S(t), () => { consumed = _pacer.TryConsume(t); return B(consumed); }, () => PacerState(_pacer));
                            if (!consumed) break;
                        }
                    }
                }
            }
        }

        private static void AdaptiveSection()
        {
            Section("adaptive-interval");
            _rng = 0x5EED0003;
            long[] intervals = { -1, 0, 1, 1000, 5208, Int64.MaxValue / 3, Int64.MaxValue };
            int[] emitted = { 0, 1, 2, 3, -4, 128, Int32.MaxValue };
            foreach (long interval in intervals)
                foreach (bool enabled in new[] { false, true })
                {
                    Op("new adaptive " + S(interval) + " " + B(enabled), () =>
                    {
                        _adaptive = null;
                        _adaptive = new AdaptiveNoteIntervalClock(interval, enabled);
                        return null;
                    }, () => _adaptive == null ? "none" : AdaptiveState(_adaptive));
                    if (_adaptive == null) continue;
                    long boundary = 0;
                    for (int i = 0; i < 60; i++)
                    {
                        int kind = Pick(12);
                        boundary = Sat(boundary, Pick(4) == 0 ? 0 : Pick(3) * 1000 + Pick(7));
                        long b = Pick(25) == 0 ? Int64.MaxValue : boundary;
                        long s = Pick(3) == 0 ? b : Sat(b, Pick(3000) - 500);
                        int n = emitted[Pick(emitted.Length)];
                        if (kind < 3) Op("adaptive.effective " + S(b), () => S(_adaptive.EffectiveBoundary(b)), () => AdaptiveState(_adaptive));
                        else if (kind < 7)
                        {
                            long pass = Pick(5) == 0 ? Sat(b, 1) : _adaptive.OpenBoundaryMicroseconds != Int64.MaxValue ? _adaptive.OpenBoundaryMicroseconds : b;
                            Op("adaptive.pass " + S(pass) + " " + S(n), () => { _adaptive.RecordBoundaryPass(pass, n); return null; }, () => AdaptiveState(_adaptive));
                        }
                        else if (kind < 11) Op("adaptive.complete " + S(b) + " " + S(s), () => B(_adaptive.CompleteIfClosed(b, s)), () => AdaptiveState(_adaptive));
                        else Op("adaptive.clear", () => { _adaptive.Clear(); return null; }, () => AdaptiveState(_adaptive));
                    }
                }
            // Exact boundary values: the batch closes only when both the next
            // local boundary and the next source time are strictly later.
            Op("new adaptive 1000 1", () => { _adaptive = new AdaptiveNoteIntervalClock(1000, true); return null; }, () => AdaptiveState(_adaptive));
            long[][] closes = { new long[] { 5001, 5000 }, new long[] { 5000, 5001 }, new long[] { 5000, 5000 }, new long[] { 4999, 9000 },
                new long[] { 9000, 4999 }, new long[] { 5001, 5001 } };
            foreach (long[] close in closes)
            {
                long local = close[0], next = close[1];
                Op("adaptive.pass 5000 2", () => { _adaptive.RecordBoundaryPass(5000, 2); return null; }, () => AdaptiveState(_adaptive));
                Op("adaptive.effective 5000", () => S(_adaptive.EffectiveBoundary(5000)), () => AdaptiveState(_adaptive));
                Op("adaptive.effective 4999", () => S(_adaptive.EffectiveBoundary(4999)), () => AdaptiveState(_adaptive));
                Op("adaptive.complete " + S(local) + " " + S(next), () => B(_adaptive.CompleteIfClosed(local, next)), () => AdaptiveState(_adaptive));
                Op("adaptive.effective 6999", () => S(_adaptive.EffectiveBoundary(6999)), () => AdaptiveState(_adaptive));
                Op("adaptive.effective 7000", () => S(_adaptive.EffectiveBoundary(7000)), () => AdaptiveState(_adaptive));
                Op("adaptive.clear", () => { _adaptive.Clear(); return null; }, () => AdaptiveState(_adaptive));
            }
            long[] values = { -1, 0, 1, 2, 3037000499, 3037000500, Int64.MaxValue };
            foreach (long a in values)
                foreach (long b in values)
                {
                    Op("adaptive.smul " + S(a) + " " + S(b), () => S(AdaptiveNoteIntervalClock.SaturatingMultiply(a, b)), null);
                    Op("adaptive.sadd " + S(a) + " " + S(b), () => S(AdaptiveNoteIntervalClock.SaturatingAdd(a, b)), null);
                }
        }

        // ============================================================= queue

        private static PendingMidiQueue _pending;
        private static ForwardDropQueue _fdq;
        private static CompleteNoteTracker _tracker;

        private static string PendingState()
        {
            if (_pending == null) return "none";
            StringBuilder items = new StringBuilder();
            foreach (int index in _pending) items.Append(S(index)).Append(',');
            return "n" + S(_pending.Count) + " o" + S(_pending.OldestEventIndex) + " k" + S(_pending.PeakBackingCount) +
                " e" + S(_pending.EligibleBackingCount) + " s" + S(_pending.AllocatedSegmentCount) +
                " f" + S(L(_pending, "_free")) + " x" + S(L(_pending, "_nextFresh")) + " h" + S(L(_pending, "_head")) +
                " t" + S(L(_pending, "_tail")) + " eh" + S(L(_pending, "_eligibleHead")) + " et" + S(L(_pending, "_eligibleTail")) +
                " ho" + S(L(_pending, "_simpleHeadOffset")) + " to" + S(L(_pending, "_simpleTailOffset")) +
                " d" + Digest(items.ToString());
        }

        private static string FdqState()
        {
            if (_fdq == null) return "none";
            Queue<long> pending = (Queue<long>)F(_fdq, "_pendingService");
            StringBuilder items = new StringBuilder();
            foreach (long service in pending) items.Append(S(service)).Append(',');
            return "o" + S(_fdq.Occupancy) + " m" + S(_fdq.MaximumOccupancy) + " b" + B(Z(_fdq, "_busy")) +
                " c" + S(L(_fdq, "_completionMicroseconds")) + " p" + S(pending.Count) + " d" + Digest(items.ToString());
        }

        internal static string TrackerText(CompleteNoteTracker tracker)
        {
            StringBuilder text = new StringBuilder();
            Array slots = (Array)F(tracker, "_occurrences");
            int total = 0;
            for (int slot = 0; slot < slots.Length; slot++)
            {
                object queue = slots.GetValue(slot);
                if (queue == null) continue;
                System.Collections.IEnumerable entries = (System.Collections.IEnumerable)queue;
                bool any = false;
                foreach (object occurrence in entries)
                {
                    if (!any) { text.Append(S(slot)).Append(':'); any = true; }
                    text.Append(S(L(occurrence, "AttackIndex"))).Append('/').Append(S(L(occurrence, "State"))).Append(',');
                    total++;
                }
                if (any) text.Append(';');
            }
            text.Append("|q").Append(S(total));
            foreach (string set in new[] { "_unmatchedAccepted", "_evictedUnmatched", "_filteredUnmatched" })
            {
                List<int> values = new List<int>((HashSet<int>)F(tracker, set));
                values.Sort();
                text.Append('|');
                foreach (int value in values) text.Append(S(value)).Append(',');
            }
            return text.ToString();
        }

        private static string TrackerState()
        {
            HashSet<int> accepted = (HashSet<int>)F(_tracker, "_unmatchedAccepted");
            return "a" + S(accepted.Count) + " d" + Digest(TrackerText(_tracker));
        }

        private static string Entry(PendingMidiQueue.Entry entry)
        {
            return S(entry.EventIndex) + " " + S(entry.PairedAttackIndex) + " " + B(entry.IsNoteOn) + " " + B(entry.IsNoteOff);
        }

        private static void QueueFamily()
        {
            Section("pending-queue");
            _rng = 0x5EED0101;
            Op("new pending 0 0", () => { _pending = new PendingMidiQueue(0); return null; }, null);
            string[] policies = { "newest", "oldest", "oldest-note", "least-note", "head-age" };
            int configIndex = 0;
            foreach (string fixture in new[] { "models-notes.mid", "models-dense.mid" })
            {
                LoadFixture(fixture);
                foreach (int capacity in new[] { 1, 2, 8, 64 })
                    foreach (string policy in policies)
                        PendingRun(capacity, policy, configIndex++);
            }
            PendingEdges();

            Section("forward-drop-queue");
            _rng = 0x5EED0102;
            Op("new fdq 0", () => { _fdq = new ForwardDropQueue(0); return null; }, null);
            LoadFixture("models-dense.mid");
            foreach (int capacity in new[] { 1, 2, 3, 16, 1000000, Int32.MaxValue })
                foreach (long service in new long[] { 0, 100, 5208, 50000 })
                {
                    Op("new fdq " + S(capacity), () => { _fdq = new ForwardDropQueue(capacity); return null; }, FdqState);
                    for (int i = 0; i < _events.Count && i < 500; i += 1 + Pick(3))
                    {
                        MidiEventView e = _events[i];
                        long arrival = e.IntendedMicroseconds;
                        bool safety = CompleteNoteTracker.IsSafetyControl(e) || Pick(40) == 0;
                        long cost = Pick(30) == 0 ? -7 : service + Pick(3);
                        if (Pick(10) == 0) Op("fdq.advance " + S(arrival), () => { _fdq.Advance(arrival); return null; }, FdqState);
                        Op("fdq.admit " + S(arrival) + " " + S(cost) + " " + B(safety), () => B(_fdq.TryAdmit(arrival, cost, safety)), FdqState);
                    }
                }
            Op("new fdq 4", () => { _fdq = new ForwardDropQueue(4); return null; }, FdqState);
            Op("fdq.admit " + S(Int64.MaxValue - 10) + " 5 0", () => B(_fdq.TryAdmit(Int64.MaxValue - 10, 5, false)), FdqState);
            Op("fdq.admit " + S(Int64.MaxValue - 10) + " 20 0", () => B(_fdq.TryAdmit(Int64.MaxValue - 10, 20, false)), FdqState);
            Op("fdq.admit " + S(Int64.MaxValue - 5) + " 1 0", () => B(_fdq.TryAdmit(Int64.MaxValue - 5, 1, false)), FdqState);
            Op("fdq.advance " + S(Int64.MaxValue), () => { _fdq.Advance(Int64.MaxValue); return null; }, FdqState);
            Op("fdq.admit 0 " + S(Int64.MaxValue) + " 0", () => B(_fdq.TryAdmit(0, Int64.MaxValue, false)), FdqState);
            Op("new fdq 4", () => { _fdq = new ForwardDropQueue(4); return null; }, FdqState);
            Op("fdq.admit 10 " + S(Int64.MaxValue) + " 0", () => B(_fdq.TryAdmit(10, Int64.MaxValue, false)), FdqState);

            Section("complete-note-tracker");
            _rng = 0x5EED0103;
            foreach (string fixture in new[] { "f0-all-message-types.mid", "index-chase-rich.mid", "models-notes.mid" })
            {
                LoadFixture(fixture);
                Op("new tracker", () => { _tracker = new CompleteNoteTracker(); return null; }, TrackerState);
                for (int i = 0; i < _events.Count && i < 1200; i++)
                {
                    int index = i;
                    MidiEventView e = _events[i];
                    Op("tracker.classify " + Tok(index), () => S((int)CompleteNoteTracker.Classify(_events[index])), null);
                    Op("tracker.safety " + Tok(index), () => B(CompleteNoteTracker.IsSafetyControl(_events[index])), null);
                    CompleteNoteEventKind kind = CompleteNoteTracker.Classify(e);
                    int choice = Pick(10);
                    if (kind == CompleteNoteEventKind.NoteOn)
                    {
                        int attack = choice == 0 ? -1 : index;
                        bool accepted = choice < 6, policy = choice >= 8;
                        if (choice == 9) Op("tracker.filtered " + Tok(index) + " " + S(attack), () => { _tracker.RecordFilteredNoteOn(_events[index], attack); return null; }, TrackerState);
                        else Op("tracker.on " + Tok(index) + " " + S(attack) + " " + B(accepted) + " " + B(policy), () => { _tracker.RecordNoteOn(_events[index], attack, accepted, policy); return null; }, TrackerState);
                        if (Pick(6) == 0) Op("tracker.evicted " + S(attack), () => { _tracker.MarkUnsentAttackEvicted(attack); return null; }, TrackerState);
                        if (Pick(9) == 0) Op("tracker.mfiltered " + S(attack), () => { _tracker.MarkUnsentAttackFiltered(attack); return null; }, TrackerState);
                    }
                    else if (kind == CompleteNoteEventKind.NoteOff)
                    {
                        if (choice < 8) Op("tracker.off " + Tok(index), () => NoteOffMatchText(_tracker.TakeNoteOff(_events[index])), TrackerState);
                        else Op("tracker.suppress " + Tok(index), () => B(_tracker.ShouldSuppressNoteOff(_events[index])), TrackerState);
                    }
                    else if (choice == 0)
                        Op("tracker.off " + Tok(index), () => NoteOffMatchText(_tracker.TakeNoteOff(_events[index])), TrackerState);
                }
            }

            SelectorSection();
        }

        private static string NoteOffMatchText(CompleteNoteTracker.NoteOffMatch match)
        {
            return S(match.AttackIndex) + " " + B(match.Accepted) + " " + B(match.Suppress) + " " + B(match.Filtered);
        }

        // A queue simulation like v1 Analysis: one service at a time, a
        // generation-owned service clock, a complete-note tracker for pairing,
        // and the overflow policy applied when the queue is full.
        private static void PendingRun(int capacity, string policy, int configIndex)
        {
            bool noteIndex = policy != "newest" && policy != "oldest";
            Op("new pending " + S(capacity) + " " + B(noteIndex), () => { _pending = new PendingMidiQueue(capacity, noteIndex); return null; }, PendingState);
            Op("new tracker", () => { _tracker = new CompleteNoteTracker(); return null; }, TrackerState);
            long eps = new long[] { 2000, 9000, 40000 }[configIndex % 3];
            Op("new clock eps 0 31250 " + S(eps), () => { _clock = new ServiceDurationClock(ServiceDurationMode.EventsPerSecond, 0, 31250, eps); return null; }, ClockState);
            int fairness = 0;
            long completion = 0;
            bool busy = false;
            int limit = Math.Min(_events.Count, 900);
            for (int i = 0; i < limit; i++)
            {
                int index = i;
                MidiEventView incoming = _events[index];
                long arrival = incoming.IntendedMicroseconds;
                while (busy && completion <= arrival)
                {
                    if (_pending.Count == 0) { busy = false; break; }
                    int served = -1;
                    Op("pending.deq", () => { PendingMidiQueue.Entry entry = _pending.DequeueEntry(); served = entry.EventIndex; return Entry(entry); }, PendingState);
                    int s = served;
                    long service = 0;
                    Op("clock.next " + Tok(s), () => { service = _clock.NextMicroseconds(_events[s]); return S(service); }, ClockState);
                    completion = Sat(completion, service);
                }
                // Live setting changes: the note index is toggled at a control boundary.
                if (i == limit / 3 || i == 2 * limit / 3)
                {
                    bool enable = i == limit / 3 ? !noteIndex : noteIndex;
                    Op("pending.index " + B(enable), () => { _pending.SetNoteIndexEnabled(enable); return null; }, PendingState);
                }
                bool indexed = (bool)F(_pending, "_noteIndexEnabled");
                CompleteNoteEventKind kind = CompleteNoteTracker.Classify(incoming);
                CompleteNoteTracker.NoteOffMatch match = default(CompleteNoteTracker.NoteOffMatch);
                match.AttackIndex = -1;
                if (kind == CompleteNoteEventKind.NoteOff)
                {
                    CompleteNoteTracker.NoteOffMatch taken = default(CompleteNoteTracker.NoteOffMatch);
                    Op("tracker.off " + Tok(index), () => { taken = _tracker.TakeNoteOff(_events[index]); return NoteOffMatchText(taken); }, TrackerState);
                    match = taken;
                    if (match.Suppress) continue;
                }
                if (!busy)
                {
                    busy = true;
                    long service = 0;
                    Op("clock.next " + Tok(index), () => { service = _clock.NextMicroseconds(_events[index]); return S(service); }, ClockState);
                    completion = Sat(arrival, service);
                    if (kind == CompleteNoteEventKind.NoteOn)
                        Op("tracker.on " + Tok(index) + " " + S(index) + " 1 0", () => { _tracker.RecordNoteOn(_events[index], index, true, false); return null; }, TrackerState);
                    continue;
                }
                if (_pending.Count >= capacity)
                {
                    bool made = false;
                    if (policy == "oldest")
                        Op("pending.deq", () => Entry(_pending.DequeueEntry()), PendingState);
                    else if (policy != "newest" && indexed)
                    {
                        int attack = -1, release = -1;
                        bool evicted = false;
                        if (policy == "oldest-note")
                            Op("pending.evict-oldest", () => { evicted = _pending.TryEvictOldestCompleteNote(out attack, out release); return B(evicted) + " " + S(attack) + " " + S(release); }, PendingState);
                        else if (policy == "head-age")
                            Op("pending.evict-head", () => { evicted = _pending.TryEvictOldestCompleteNoteAtHead(out attack, out release); return B(evicted) + " " + S(attack) + " " + S(release); }, PendingState);
                        else
                        {
                            int before = fairness;
                            Op("pending.evict-least " + S(before), () => { evicted = _pending.TryEvictLeastImportantCompleteNote(_events, ref fairness, out attack, out release); return B(evicted) + " " + S(attack) + " " + S(release) + " " + S(fairness); }, PendingState);
                        }
                        if (evicted && release < 0)
                        {
                            int a = attack;
                            Op("tracker.evicted " + S(a), () => { _tracker.MarkUnsentAttackEvicted(a); return null; }, TrackerState);
                        }
                        made = evicted;
                    }
                    else if (policy != "newest" && Pick(5) == 0)
                        Op("pending.evict-oldest", () => { int a, r; bool ok = _pending.TryEvictOldestCompleteNote(out a, out r); return B(ok) + " " + S(a) + " " + S(r); }, PendingState);
                    if (_pending.Count >= capacity && !made && policy != "oldest")
                    {
                        if (kind == CompleteNoteEventKind.NoteOn)
                            Op("tracker.on " + Tok(index) + " " + S(index) + " 0 1", () => { _tracker.RecordNoteOn(_events[index], index, false, true); return null; }, TrackerState);
                        continue;
                    }
                }
                PendingMidiQueue.Entry newEntry = new PendingMidiQueue.Entry
                {
                    EventIndex = index,
                    PairedAttackIndex = kind == CompleteNoteEventKind.NoteOff ? match.AttackIndex : -1,
                    IsNoteOn = kind == CompleteNoteEventKind.NoteOn,
                    IsNoteOff = kind == CompleteNoteEventKind.NoteOff
                };
                Op("pending.enq " + Entry(newEntry), () => { _pending.Enqueue(newEntry); return null; }, PendingState);
                if (kind == CompleteNoteEventKind.NoteOn)
                    Op("tracker.on " + Tok(index) + " " + S(index) + " 1 0", () => { _tracker.RecordNoteOn(_events[index], index, true, false); return null; }, TrackerState);
                if (configIndex % 7 == 3 && i == limit / 2)
                    Op("pending.clear", () => { _pending.Clear(); return null; }, PendingState);
            }
            while (_pending.Count > 0)
                Op("pending.deq", () => Entry(_pending.DequeueEntry()), PendingState);
        }

        private static void PendingEdges()
        {
            Section("pending-queue-edges");
            LoadFixture("models-notes.mid");
            Op("new pending 3 0", () => { _pending = new PendingMidiQueue(3); return null; }, PendingState);
            Op("pending.deq", () => Entry(_pending.DequeueEntry()), PendingState);
            Op("pending.evict-oldest", () => { int a, r; bool ok = _pending.TryEvictOldestCompleteNote(out a, out r); return B(ok) + " " + S(a) + " " + S(r); }, PendingState);
            Op("pending.evict-head", () => { int a, r; bool ok = _pending.TryEvictOldestCompleteNoteAtHead(out a, out r); return B(ok) + " " + S(a) + " " + S(r); }, PendingState);
            Op("pending.evict-least 0", () => { int a, r, c = 0; bool ok = _pending.TryEvictLeastImportantCompleteNote(_events, ref c, out a, out r); return B(ok) + " " + S(a) + " " + S(r) + " " + S(c); }, PendingState);
            // Cross a 256-entry segment boundary in both representations.
            for (int i = 0; i < 600; i++)
            {
                PendingMidiQueue.Entry entry = new PendingMidiQueue.Entry { EventIndex = i, PairedAttackIndex = -1, IsNoteOn = i % 3 == 0 };
                Op("pending.enq " + Entry(entry), () => { _pending.Enqueue(entry); return null; }, PendingState);
                if (i % 5 == 4) Op("pending.deq", () => Entry(_pending.DequeueEntry()), PendingState);
                if (i == 300) Op("pending.index 1", () => { _pending.SetNoteIndexEnabled(true); return null; }, PendingState);
                if (i == 450) Op("pending.index 0", () => { _pending.SetNoteIndexEnabled(false); return null; }, PendingState);
                if (i == 500) Op("pending.index 1", () => { _pending.SetNoteIndexEnabled(true); return null; }, PendingState);
            }
            PendingMidiQueue.Entry duplicate = new PendingMidiQueue.Entry { EventIndex = 597, PairedAttackIndex = -1, IsNoteOn = true };
            Op("pending.enq " + Entry(duplicate), () => { _pending.Enqueue(duplicate); return null; }, PendingState);
            PendingMidiQueue.Entry paired = new PendingMidiQueue.Entry { EventIndex = 900, PairedAttackIndex = 597, IsNoteOff = true };
            Op("pending.enq " + Entry(paired), () => { _pending.Enqueue(paired); return null; }, PendingState);
            Op("pending.evict-oldest", () => { int a, r; bool ok = _pending.TryEvictOldestCompleteNote(out a, out r); return B(ok) + " " + S(a) + " " + S(r); }, PendingState);
            Op("pending.index 0", () => { _pending.SetNoteIndexEnabled(false); return null; }, PendingState);
            Op("pending.clear", () => { _pending.Clear(); return null; }, PendingState);
            Op("pending.index 1", () => { _pending.SetNoteIndexEnabled(true); return null; }, PendingState);
            Op("pending.deq", () => Entry(_pending.DequeueEntry()), PendingState);
        }

        private static void SelectorSection()
        {
            Section("musical-priority");
            _rng = 0x5EED0104;
            MusicalNotePrioritySelector selector = new MusicalNotePrioritySelector();
            int[] tracks = { 0, 1, 2, -1, 5, 40, Int32.MaxValue, Int32.MinValue, 5409 };
            int[] channels = { 0, 1, 9, 15, -1, 3 };
            long[] times = { 0, 0, 1000, 1000, 5208 };
            long[] durations = { 0, 100, 100, Int64.MaxValue, 7 };
            int[] cursors = { 0, 1, 2, Int32.MaxValue, 1000, 397 };
            for (int round = 0; round < 400; round++)
            {
                int count = 1 + Pick(12);
                MusicalNotePriorityCandidate[] candidates = new MusicalNotePriorityCandidate[count];
                StringBuilder text = new StringBuilder();
                for (int i = 0; i < count; i++)
                {
                    candidates[i] = new MusicalNotePriorityCandidate
                    {
                        Pitch = 40 + Pick(30),
                        Velocity = Pick(3) == 0 ? 64 : Pick(128),
                        Track = tracks[Pick(tracks.Length)],
                        Channel = channels[Pick(channels.Length)],
                        SourceOrder = Pick(4) == 0 ? 7 : Pick(100000),
                        IntendedMicroseconds = times[Pick(times.Length)],
                        DurationMicroseconds = durations[Pick(durations.Length)]
                    };
                    MusicalNotePriorityCandidate c = candidates[i];
                    if (i > 0) text.Append(';');
                    text.Append(S(c.Pitch)).Append(',').Append(S(c.Velocity)).Append(',').Append(S(c.Track)).Append(',')
                        .Append(S(c.Channel)).Append(',').Append(S(c.SourceOrder)).Append(',')
                        .Append(S(c.IntendedMicroseconds)).Append(',').Append(S(c.DurationMicroseconds));
                }
                int cursor = cursors[Pick(cursors.Length)];
                int n = round % 50 == 7 ? count + 1 : round % 50 == 9 ? 0 : count;
                Op("select " + S(cursor) + " " + S(count) + " " + S(n) + " " + text, () =>
                {
                    int victim = selector.SelectLeastImportant(candidates, n, cursor);
                    return S(victim) + " " + S(selector.NextFairnessCursor(candidates[victim]));
                }, null);
            }
        }

        // =========================================================== ordered

        private static OrderedFiniteAdmissionCore _ordered;

        internal static string ItemText(OutputWorkItem item)
        {
            return S(item.GenerationId) + "/" + S((int)item.Kind) + "/" + S(item.SourceEventIndex) + "/" +
                S(item.LogicalOutputMicroseconds) + "/" + S((int)item.Protection) + "/" + S(item.OccurrenceId) + "/" +
                (item.HasMidiEvent ? Ev(item.MidiEvent) : "-");
        }

        private static string ItemsText(IList<OutputWorkItem> items)
        {
            StringBuilder text = new StringBuilder("[");
            for (int i = 0; i < items.Count; i++)
            {
                if (i > 0) text.Append(',');
                text.Append(ItemText(items[i]));
            }
            return text.Append(']').ToString();
        }

        private static string OrderedState()
        {
            if (_ordered == null) return "none";
            object queue = F(_ordered, "_generations");
            LinkedList<PlaybackGeneration> generations = (LinkedList<PlaybackGeneration>)F(queue, "_generations");
            StringBuilder text = new StringBuilder();
            foreach (PlaybackGeneration g in generations)
            {
                text.Append(S(g.Id)).Append(',').Append(B(g.IsAdmissionClosed)).Append(',').Append(S(g.SourceEventCursor)).Append(',')
                    .Append(S(g.SourceClockAnchorTicks)).Append(',').Append(S(g.FrozenElapsedTicks)).Append(',')
                    .Append(S(g.TargetSourceMicroseconds)).Append(',').Append(S(g.ChasePrefixCount)).Append(',')
                    .Append(S(L(g.ModeState, "MusicalFairnessCursor"))).Append(':');
                foreach (OutputWorkItem item in (LinkedList<OutputWorkItem>)F(g, "_admitted")) text.Append(ItemText(item)).Append(';');
                text.Append(':').Append(TrackerText((CompleteNoteTracker)F(g.ModeState, "CompleteNotes"))).Append('\n');
            }
            return "live" + S(_ordered.LiveGenerationCount) + " pend" + S(_ordered.PendingCount) + " out" + S(_ordered.OutstandingCount) +
                " next" + S(L(_ordered, "_nextGenerationId")) + " svc" + (_ordered.HasInService ? ItemText((OutputWorkItem)F(_ordered, "_inService")) : "-") +
                " d" + Digest(text.ToString());
        }

        private static Func<MidiEventView, OrderedFiniteCapturedDecision> Decision(string spec)
        {
            if (spec == "none") return null;
            if (spec == "badid") return e => new OrderedFiniteCapturedDecision(e.WithLogicalEventIndex(e.EventIndex + 1), false);
            if (spec == "invalid") return e => new OrderedFiniteCapturedDecision(default(MidiEventView), false);
            string[] parts = spec.Split(':');
            int mask = Convert.ToInt32(parts[1], 16);
            if (parts[0] == "mask")
                return e => new OrderedFiniteCapturedDecision(e, e.Channel >= 0 && ((mask >> e.Channel) & 1) != 0);
            int shift = Int32.Parse(parts[2], Inv);
            return e =>
            {
                bool filtered = e.Channel >= 0 && ((mask >> e.Channel) & 1) != 0;
                if (filtered || e.Channel < 0) return new OrderedFiniteCapturedDecision(e, filtered);
                return new OrderedFiniteCapturedDecision(e.WithOutputChannel((e.Channel + shift) % 16), false);
            };
        }

        // The captured token for an op line, mirroring Decision(spec).
        private static string CapturedTok(string spec, int index, out bool filtered)
        {
            MidiEventView e = _events[index];
            filtered = false;
            if (spec == "none") return Tok(index);
            string[] parts = spec.Split(':');
            int mask = Convert.ToInt32(parts[1], 16);
            filtered = e.Channel >= 0 && ((mask >> e.Channel) & 1) != 0;
            if (parts[0] == "mask" || filtered || e.Channel < 0) return Tok(index);
            return Tok(index) + "@" + S((e.Channel + Int32.Parse(parts[2], Inv)) % 16);
        }

        private static MidiEventView Parse(string token)
        {
            if (token == "x") return default(MidiEventView);
            int at = token.IndexOf('@'), hash = token.IndexOf('#');
            int end = at >= 0 ? at : hash >= 0 ? hash : token.Length;
            MidiEventView e = _events[Int32.Parse(token.Substring(1, end - 1), Inv)];
            if (at >= 0) e = e.WithOutputChannel(Int32.Parse(token.Substring(at + 1, (hash >= 0 ? hash : token.Length) - at - 1), Inv));
            if (hash >= 0) e = e.WithLogicalEventIndex(Int32.Parse(token.Substring(hash + 1), Inv));
            return e;
        }

        private static string ChaseTok(int[] chase)
        {
            if (chase == null) return "-";
            StringBuilder text = new StringBuilder("[");
            for (int i = 0; i < chase.Length; i++) { if (i > 0) text.Append(','); text.Append(Tok(chase[i])); }
            return text.Append(']').ToString();
        }

        private static List<MidiEventView> ChaseList(int[] chase)
        {
            if (chase == null) return null;
            List<MidiEventView> list = new List<MidiEventView>();
            foreach (int index in chase) list.Add(_events[index]);
            return list;
        }

        private static string AdmissionText(OrderedFiniteAdmissionResult result)
        {
            return S((int)result.Disposition) + " " + B(result.PairedReleaseSuppressed) + " " + ItemsText(result.Evicted);
        }

        private static string PressureText(OrderedFinitePressureSnapshot p)
        {
            return S(p.AdmittedOutstandingCount) + " " + S(p.ExternalDueCount) + " " + S(p.TotalDueCount) + " " +
                S(p.OldestDueMicroseconds) + " " + S(p.CurrentAgeMicroseconds);
        }

        private static string RepairText(OrderedFiniteAgeRepairResult r)
        {
            return ItemsText(r.Evicted) + " " + ClearText(r.ClearResult) + " " + B(r.StalledByInServiceOrPreservationBarrier) +
                " " + S(r.AgeAfterMicroseconds);
        }

        private static string ClearText(OrderedFiniteClearResult result)
        {
            if (result == null) return "-";
            return ItemsText(result.ClearedWork) + " " + S(result.FirstSkippedSourceEventIndex) + " " + S(result.SourceEventCursorAfter) +
                " " + S(result.DroppedSkippedSourceEvents) + " " + S(result.FilteredSkippedSourceEvents) + " " + S(result.DroppedEvents) +
                " " + B(result.RequiresSafetyReset);
        }

        private static void OrderedFamily()
        {
            Section("ordered-finite-admission");
            _rng = 0x5EED0201;
            OverflowPolicy[] policies = { OverflowPolicy.DropNewest, OverflowPolicy.DropOldest, OverflowPolicy.ClearBufferAndCatchUp,
                OverflowPolicy.DropIncomingCompleteNotes, OverflowPolicy.DropOldestCompleteNote, OverflowPolicy.DropBackgroundNotes };
            string[] decisions = { "none", "mask:0204", "route:0001:3" };
            int[] capacities = { 1, 3, 16 };
            int configIndex = 0;
            foreach (string fixture in new[] { "models-notes.mid", "models-dense.mid", "index-chase-rich.mid" })
            {
                LoadFixture(fixture);
                foreach (OverflowPolicy policy in policies)
                    foreach (int capacity in capacities)
                    {
                        OverflowPolicy second = policies[(Array.IndexOf(policies, policy) + 1 + configIndex) % policies.Length];
                        OrderedRun(1 + configIndex % 4, capacity, policy, second, decisions[configIndex % decisions.Length], configIndex);
                        configIndex++;
                    }
            }
            OrderedEdges();
        }

        private static void OrderedRun(int maxGenerations, int capacity, OverflowPolicy first, OverflowPolicy second,
            string decisionSpec, int configIndex)
        {
            Func<MidiEventView, OrderedFiniteCapturedDecision> decision = Decision(decisionSpec);
            Op("new ordered " + S(maxGenerations), () => { _ordered = new OrderedFiniteAdmissionCore(_events, maxGenerations); return null; }, OrderedState);
            int[] chase = configIndex % 2 == 0 ? null : ChaseIndices(0, 3);
            Op("ordered.start 0 0 " + ChaseTok(chase) + " 1000 0", () => { _ordered.Start(0, 0, ChaseList(chase), 1000, 0); return null; }, OrderedState);
            long now = 0;
            OverflowPolicy[] repairPolicies = { OverflowPolicy.DropOldest, OverflowPolicy.DropOldestCompleteNote, OverflowPolicy.ClearBufferAndCatchUp };
            const int steps = 320;
            for (int step = 0; step < steps; step++)
            {
                // Live setting changes: capacity and policy switch mid-stream.
                int cap = (step / 40) % 2 == 0 ? capacity : Math.Max(1, capacity * 2 - 1);
                OverflowPolicy policy = step < steps / 2 ? first : second;
                PlaybackGeneration tail = _ordered.Tail;
                int kind = Pick(32);
                if (kind < 16 && tail != null && !tail.IsAdmissionClosed && tail.SourceEventCursor < _events.Count)
                {
                    int index = tail.SourceEventCursor;
                    now = Math.Max(now, _events[index].IntendedMicroseconds);
                    if (kind == 0)
                    {
                        Op("ordered.admit " + S(cap) + " " + S((int)policy), () => AdmissionText(_ordered.AdmitNextSource(cap, policy)), OrderedState);
                        continue;
                    }
                    bool filtered;
                    string token = CapturedTok(decisionSpec, index, out filtered);
                    MidiEventView captured = Parse(token);
                    Op("ordered.admitx " + token + " " + B(filtered) + " " + S(cap) + " " + S((int)policy),
                        () => AdmissionText(_ordered.AdmitNextSource(captured, filtered, cap, policy)), OrderedState);
                }
                else if (kind < 20)
                {
                    OutputWorkItem item = default(OutputWorkItem);
                    Op("ordered.begin", () => { bool ok = _ordered.TryBeginDispatch(out item); return B(ok) + " " + (ok ? ItemText(item) : "-"); }, OrderedState);
                }
                else if (kind < 26)
                {
                    if (_ordered.HasInService || Pick(10) == 0)
                        Op("ordered.complete", () => { _ordered.CompleteDispatch(); return null; }, OrderedState);
                }
                else if (kind == 26)
                {
                    long at = Sat(now, Pick(8000));
                    Op("ordered.pressure " + S(at) + " " + decisionSpec, () =>
                    {
                        OrderedFinitePressureSnapshot p = _ordered.MeasureDuePressure(at, decision);
                        return S(p.AdmittedOutstandingCount) + " " + S(p.ExternalDueCount) + " " + S(p.TotalDueCount) + " " +
                            S(p.OldestDueMicroseconds) + " " + S(p.CurrentAgeMicroseconds);
                    }, OrderedState);
                }
                else if (kind == 27)
                {
                    if (_ordered.HasInService && Pick(3) != 0)
                        Op("ordered.complete", () => { _ordered.CompleteDispatch(); return null; }, OrderedState);
                    long at = Sat(now, Pick(8000));
                    long limit = new long[] { 0, 1, 5000, 100000 }[Pick(4)];
                    OverflowPolicy repair = Pick(12) == 0 ? OverflowPolicy.DropNewest : repairPolicies[Pick(3)];
                    Op("ordered.repair " + S(at) + " " + S(limit) + " " + S((int)repair) + " " + decisionSpec, () =>
                    {
                        OrderedFiniteAgeRepairResult r = _ordered.RepairWaitingTimePressure(at, limit, repair, decision);
                        return ItemsText(r.Evicted) + " " + ClearText(r.ClearResult) + " " + B(r.StalledByInServiceOrPreservationBarrier) +
                            " " + S(r.AgeAfterMicroseconds);
                    }, OrderedState);
                }
                else if (kind == 28)
                {
                    int cursor = Pick(_events.Count + 1);
                    long target = cursor < _events.Count ? _events[cursor].IntendedMicroseconds : _song.DurationMicroseconds;
                    int[] seekChase = Pick(2) == 0 ? null : ChaseIndices(cursor, Pick(4));
                    long anchor = 5000 + step;
                    Op("ordered.seek " + S(target) + " " + S(cursor) + " " + ChaseTok(seekChase) + " " + S(anchor) + " 7",
                        () => B(_ordered.TryAppendSeek(target, cursor, ChaseList(seekChase), anchor, 7)), OrderedState);
                }
                else if (kind == 29)
                {
                    long through = Pick(6) == 0 ? Int64.MaxValue : Sat(now, Pick(20000));
                    Op("ordered.clear " + S(through) + " " + decisionSpec, () => ClearText(_ordered.ClearOpenTailAndCatchUp(through, decision)), OrderedState);
                }
                else if (kind == 30)
                {
                    long delta = new long[] { 0, -5, 17, Int64.MaxValue }[Pick(4)];
                    Op("ordered.shift " + S(delta), () => { _ordered.ShiftSourceClockAnchors(delta); return null; }, OrderedState);
                }
                else
                {
                    OutputWorkItem item = default(OutputWorkItem);
                    Op("ordered.peek", () => { bool ok = _ordered.TryPeekDispatch(out item); return B(ok) + " " + (ok ? ItemText(item) : "-"); }, OrderedState);
                    Op("ordered.atcap " + S(cap), () => B(_ordered.IsAtOrAboveCapacity(cap)), null);
                }
            }
        }

        // Up to `count` controller/program events at or after `from`, as a chase prefix.
        private static int[] ChaseIndices(int from, int count)
        {
            List<int> chase = new List<int>();
            for (int i = from; i < _events.Count && chase.Count < count; i++)
                if (_events[i].Kind == MidiEventKind.ControlChange || _events[i].Kind == MidiEventKind.ProgramChange) chase.Add(i);
            return chase.ToArray();
        }

        private static void OrderedEdges()
        {
            Section("ordered-finite-admission-edges");
            LoadFixture("models-notes.mid");
            Op("new ordered 0", () => { _ordered = new OrderedFiniteAdmissionCore(_events, 0); return null; }, null);
            Op("new ordered 2", () => { _ordered = new OrderedFiniteAdmissionCore(_events, 2); return null; }, OrderedState);
            Op("ordered.admit 1 0", () => AdmissionText(_ordered.AdmitNextSource(1, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.clear 0 none", () => ClearText(_ordered.ClearOpenTailAndCatchUp(0, null)), OrderedState);
            Op("ordered.seek 0 0 - 0 0", () => B(_ordered.TryAppendSeek(0, 0, null, 0, 0)), OrderedState);
            Op("ordered.start -1 0 - 0 0", () => { _ordered.Start(-1, 0, null, 0, 0); return null; }, OrderedState);
            Op("ordered.start -1 0 [e0] 0 0", () => { _ordered.Start(-1, 0, ChaseList(new[] { 0 }), 0, 0); return null; }, OrderedState);
            Op("ordered.start 0 " + S(_events.Count + 1) + " - 0 0", () => { _ordered.Start(0, _events.Count + 1, null, 0, 0); return null; }, OrderedState);
            Op("ordered.start 0 " + S(_events.Count - 2) + " - 0 0", () => { _ordered.Start(0, _events.Count - 2, null, 0, 0); return null; }, OrderedState);
            Op("ordered.start 0 0 - 0 0", () => { _ordered.Start(0, 0, null, 0, 0); return null; }, OrderedState);
            Op("ordered.atcap 0", () => B(_ordered.IsAtOrAboveCapacity(0)), null);
            int last = _events.Count - 2;
            Op("ordered.admitx " + Tok(last) + " 0 0 0", () => AdmissionText(_ordered.AdmitNextSource(_events[last], false, 0, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.admitx " + Tok(last) + " 0 1 99", () => AdmissionText(_ordered.AdmitNextSource(_events[last], false, 1, (OverflowPolicy)99)), OrderedState);
            Op("ordered.admitx " + Tok(last + 1) + " 0 1 0", () => AdmissionText(_ordered.AdmitNextSource(_events[last + 1], false, 1, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.admitx x 0 1 0", () => AdmissionText(_ordered.AdmitNextSource(default(MidiEventView), false, 1, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.admitx " + Tok(last) + "#" + S(last) + " 0 1 0", () => AdmissionText(_ordered.AdmitNextSource(_events[last].WithLogicalEventIndex(last), false, 1, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.admit 1 0", () => AdmissionText(_ordered.AdmitNextSource(1, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.admit 1 0", () => AdmissionText(_ordered.AdmitNextSource(1, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.complete", () => { _ordered.CompleteDispatch(); return null; }, OrderedState);
            Op("ordered.pressure -1 none", () => PressureText(_ordered.MeasureDuePressure(-1, null)), OrderedState);
            Op("ordered.pressure " + S(Int64.MaxValue) + " none", () => PressureText(_ordered.MeasureDuePressure(Int64.MaxValue, null)), OrderedState);
            Op("ordered.repair -1 0 1 none", () => RepairText(_ordered.RepairWaitingTimePressure(-1, 0, OverflowPolicy.DropOldest, null)), OrderedState);
            Op("ordered.repair 0 -1 1 none", () => RepairText(_ordered.RepairWaitingTimePressure(0, -1, OverflowPolicy.DropOldest, null)), OrderedState);
            Op("ordered.repair 0 0 5 none", () => RepairText(_ordered.RepairWaitingTimePressure(0, 0, OverflowPolicy.DropBackgroundNotes, null)), OrderedState);
            Op("ordered.clear -1 none", () => ClearText(_ordered.ClearOpenTailAndCatchUp(-1, null)), OrderedState);
            Op("new ordered 3", () => { _ordered = new OrderedFiniteAdmissionCore(_events, 3); return null; }, OrderedState);
            Op("ordered.start 0 0 - 0 0", () => { _ordered.Start(0, 0, null, 0, 0); return null; }, OrderedState);
            for (int i = 0; i < 6; i++) Op("ordered.admit 100 0", () => AdmissionText(_ordered.AdmitNextSource(100, OverflowPolicy.DropNewest)), OrderedState);
            Op("ordered.pressure 1000000 badid", () => PressureText(_ordered.MeasureDuePressure(1000000, Decision("badid"))), OrderedState);
            Op("ordered.pressure 1000000 invalid", () => PressureText(_ordered.MeasureDuePressure(1000000, Decision("invalid"))), OrderedState);
            Op("ordered.clear 1000000 badid", () => ClearText(_ordered.ClearOpenTailAndCatchUp(1000000, Decision("badid"))), OrderedState);
            Op("ordered.seek 0 3 - 0 0", () => B(_ordered.TryAppendSeek(0, 3, null, 0, 0)), OrderedState);
            Op("ordered.seek 5 9 - 0 0", () => B(_ordered.TryAppendSeek(5, 9, null, 0, 0)), OrderedState);
            Op("ordered.seek 5 9 - 0 0", () => B(_ordered.TryAppendSeek(5, 9, null, 0, 0)), OrderedState);
            Op("ordered.clear 0 none", () => ClearText(_ordered.ClearOpenTailAndCatchUp(0, null)), OrderedState);
            Op("ordered.repair 9000000 0 2 none", () =>
            {
                OrderedFiniteAgeRepairResult r = _ordered.RepairWaitingTimePressure(9000000, 0, OverflowPolicy.ClearBufferAndCatchUp, null);
                return ItemsText(r.Evicted) + " " + ClearText(r.ClearResult) + " " + B(r.StalledByInServiceOrPreservationBarrier) + " " + S(r.AgeAfterMicroseconds);
            }, OrderedState);
            Op("ordered.shift " + S(Int64.MaxValue), () => { _ordered.ShiftSourceClockAnchors(Int64.MaxValue); return null; }, OrderedState);
        }
    }
}
