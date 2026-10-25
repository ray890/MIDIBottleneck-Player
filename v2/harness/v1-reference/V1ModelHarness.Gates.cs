// v1 model traces for the gate families (Milestone 3 groups 3-6):
//   pernote     PerNoteIntervalGate (+ AdaptiveNoteIntervalClock), Analysis-style loop
//   delayed     DelayedNoteOffGate (+ AdaptiveNoteIntervalClock), Analysis-style loop
//   attack      MusicalAttackRateLimiter (Note start rate gate)
//   outputrate  OutputRateGate in Analysis mode (ideal source time) and Playback
//               mode (scripted decision and dispatch times)
// Every family adds live setting changes (a new gate mid-stream), channel
// retirement, zero/minimum/maximum and invalid settings.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Text;

namespace MidiBottleneck
{
    internal static partial class V1ModelHarness
    {
        private static bool GateFamily(string family)
        {
            if (family == "pernote") PerNoteFamily();
            else if (family == "delayed") DelayedFamily();
            else if (family == "attack") AttackFamily();
            else if (family == "outputrate") OutputRateFamily();
            else return false;
            return true;
        }

        // ---------------------------------------------------- state helpers

        private static readonly Dictionary<string, FieldInfo> FieldCache = new Dictionary<string, FieldInfo>();

        private static object FV(object target, string field)
        {
            string key = target.GetType().FullName + "|" + field;
            FieldInfo info;
            if (!FieldCache.TryGetValue(key, out info))
            {
                info = target.GetType().GetField(field, Hidden);
                if (info == null) throw new MissingFieldException(target.GetType().Name, field);
                FieldCache[key] = info;
            }
            return info.GetValue(target);
        }

        private static void AppendValue(StringBuilder text, object value)
        {
            if (value is bool) text.Append((bool)value ? '1' : '0');
            else if (value is MidiEventView) text.Append(Ev((MidiEventView)value));
            else text.Append(Convert.ToInt64(value, Inv).ToString(Inv));
        }

        // The named fields of one value, comma-separated.
        private static void Fields(StringBuilder text, object target, params string[] names)
        {
            for (int i = 0; i < names.Length; i++)
            {
                if (i > 0) text.Append(',');
                AppendValue(text, FV(target, names[i]));
            }
        }

        private static object Node(object owner, string chunksField, int node)
        {
            IList chunks = (IList)FV(owner, chunksField);
            return ((Array)chunks[node / 4096]).GetValue(node % 4096);
        }

        private static string EmissionsText(IList<MidiEventView> emissions, int count)
        {
            StringBuilder text = new StringBuilder(S(count)).Append(" [");
            for (int i = 0; i < count; i++)
            {
                if (i > 0) text.Append(',');
                text.Append(Ev(emissions[i]));
            }
            return text.Append(']').ToString();
        }

        // ======================================================== pernote

        private static PerNoteIntervalGate _perNote;

        private static readonly string[] PerNoteLongArrays = { "_minimumReleaseBoundary", "_nextAttackBoundary",
            "_attackBoundary", "_preparatoryReleaseBoundary", "_naturalReleaseBoundary", "_candidateBoundary", "_releaseCarryOrder" };
        private static readonly string[] PerNoteIntArrays = { "_outputChannel", "_selectedSupportCount", "_candidateHead",
            "_candidateRepresentative", "_candidateMembers", "_tickHead", "_tickTail", "_tickRepresentative", "_tickMembers" };
        private static readonly string[] PerNoteOccurrenceFields = { "InUse", "Ended", "RemovedByRouting", "Disposition",
            "Track", "Channel", "Pitch", "NextActive", "NextGroup", "NextFree", "NoteOn", "NoteOff" };

        internal static string PerNoteState()
        {
            PerNoteIntervalGate g = _perNote;
            if (g == null) return "none";
            StringBuilder text = new StringBuilder();
            bool[] down = (bool[])FV(g, "_outputDown");
            MidiEventView[] pending = (MidiEventView[])FV(g, "_pendingAttack");
            MidiEventView[] natural = (MidiEventView[])FV(g, "_naturalRelease");
            long[][] longs = new long[PerNoteLongArrays.Length][];
            for (int i = 0; i < longs.Length; i++) longs[i] = (long[])FV(g, PerNoteLongArrays[i]);
            int[][] ints = new int[PerNoteIntArrays.Length][];
            for (int i = 0; i < ints.Length; i++) ints[i] = (int[])FV(g, PerNoteIntArrays[i]);
            for (int p = 0; p < 128; p++)
            {
                text.Append(S(p)).Append(':').Append(down[p] ? '1' : '0');
                foreach (long[] values in longs) text.Append(',').Append(S(values[p]));
                foreach (int[] values in ints) text.Append(',').Append(S(values[p]));
                text.Append(',').Append(Ev(pending[p])).Append(',').Append(Ev(natural[p])).Append(';');
                foreach (int start in new[] { ints[5][p], ints[2][p] })    // tick group, candidate group
                    for (int node = start; node >= 0; node = Convert.ToInt32(FV(Node(g, "_occurrenceChunks", node), "NextGroup"), Inv))
                    {
                        text.Append('g').Append(S(node)).Append(':');
                        Fields(text, Node(g, "_occurrenceChunks", node), PerNoteOccurrenceFields);
                        text.Append(';');
                    }
            }
            int[] activeHead = (int[])FV(g, "_activeHead");
            int[] activeTail = (int[])FV(g, "_activeTail");
            for (int key = 0; key < activeHead.Length; key++)
            {
                if (activeHead[key] < 0 && activeTail[key] < 0) continue;
                text.Append('k').Append(S(key)).Append('/').Append(S(activeTail[key])).Append(':');
                for (int node = activeHead[key]; node >= 0; node = Convert.ToInt32(FV(Node(g, "_occurrenceChunks", node), "NextActive"), Inv))
                {
                    text.Append(S(node)).Append(':');
                    Fields(text, Node(g, "_occurrenceChunks", node), PerNoteOccurrenceFields);
                    text.Append(';');
                }
            }
            return "nb" + S(g.NextBoundaryMicroseconds) + " oc" + S(g.NextBoundaryOutputCount) + " f" + S(L(g, "_filteredEvents")) +
                " st" + S(L(g, "_sourceTick")) + " fr" + S(L(g, "_freeOccurrence")) + " nu" + S(L(g, "_nextUnusedOccurrence")) +
                " sc" + B(Z(g, "_sourceComplete")) + " rc" + S(L(g, "_nextReleaseCarryOrder")) + " fc" + S(L(g, "_musicalFairnessCursor")) +
                " seg" + S(g.OccurrenceSegmentCountForTesting) + " d" + Digest(text.ToString());
        }

        private static string GateAndAdaptiveState(Func<string> gate)
        {
            return gate() + " | " + (_adaptive == null ? "none" : AdaptiveState(_adaptive));
        }

        private struct GateConfig
        {
            internal long Interval;
            internal bool LimitEnabled;
            internal int Limit;
            internal bool Adaptive;
            internal GateConfig(long interval, bool limitEnabled, int limit, bool adaptive)
            {
                Interval = interval; LimitEnabled = limitEnabled; Limit = limit; Adaptive = adaptive;
            }
            internal string Text { get { return S(Interval) + " " + B(LimitEnabled) + " " + S(Limit); } }
        }

        private static void PerNoteFamily()
        {
            Section("per-note-interval-gate");
            _rng = 0x5EED0301;
            GateConfig[] configs =
            {
                new GateConfig(0, false, 128, false), new GateConfig(1000, false, 128, false),
                new GateConfig(5208, true, 3, true), new GateConfig(20000, true, 1, false),
                new GateConfig(250000, true, 128, true), new GateConfig(5208, false, 128, true),
                new GateConfig(1, true, 2, false), new GateConfig(Int64.MaxValue / 2, false, 128, false),
                new GateConfig(2604, true, 5, true)
            };
            string[] fixtures = { "models-notes.mid", "models-dense.mid", "f1-v1-test-midi.mid", "many-tracks-ties.mid", "index-chase-rich.mid" };
            int configIndex = 0;
            foreach (string fixture in fixtures)
            {
                LoadFixture(fixture);
                for (int run = 0; run < 3; run++)
                {
                    GateConfig first = configs[configIndex % configs.Length];
                    GateConfig second = configs[(configIndex + 4) % configs.Length];
                    PerNoteRun(first, second, 700, configIndex);
                    configIndex++;
                }
            }
            Section("per-note-interval-gate-edges");
            LoadFixture("models-notes.mid");
            foreach (string args in new[] { "-1 0 128", "5 1 0", "5 1 129", "5 0 0" })
            {
                string[] p = args.Split(' ');
                Op("new pernote " + args, () => { _perNote = null; _perNote = new PerNoteIntervalGate(Int64.Parse(p[0], Inv), p[1] == "1", Int32.Parse(p[2], Inv)); return null; }, () => GateAndAdaptiveState(PerNoteState));
            }
            Op("new pernote 5208 1 4", () => { _perNote = new PerNoteIntervalGate(5208, true, 4); return null; }, () => GateAndAdaptiveState(PerNoteState));
            Op("pernote.emit 0 127", () => EmissionsText(new MidiEventView[127], _perNote.EmitBoundary(0, new MidiEventView[127])), () => GateAndAdaptiveState(PerNoteState));
            for (int i = 0; i < 40; i++)
            {
                int index = i;
                Op("pernote.classify " + Tok(index), () => { int pitch; bool on; bool ok = PerNoteIntervalGate.TryClassify(_events[index], out pitch, out on); return B(ok) + " " + S(pitch) + " " + B(on); }, null);
                Op("pernote.better " + Tok(index) + " " + Tok(index + 1), () => B(PerNoteIntervalGate.IsBetterCandidate(_events[index], _events[index + 1])), null);
            }
        }

        private static void PerNoteRun(GateConfig config, GateConfig liveChange, int eventLimit, int configIndex)
        {
            Func<string> state = () => GateAndAdaptiveState(PerNoteState);
            Op("new pernote " + config.Text, () => { _perNote = null; _perNote = new PerNoteIntervalGate(config.Interval, config.LimitEnabled, config.Limit); return null; }, state);
            Op("new adaptive " + S(config.Interval) + " " + B(config.Adaptive), () => { _adaptive = new AdaptiveNoteIntervalClock(config.Interval, config.Adaptive); return null; }, state);
            int count = Math.Min(_events.Count, eventLimit);
            int next = 0;
            bool completed = false;
            bool changed = false;
            MidiEventView[] emissions = new MidiEventView[128];
            int actions = 0;
            while ((next < count || !completed || _perNote.HasPendingTransitions) && actions++ < 4000)
            {
                long localBoundary = _perNote.NextBoundaryMicroseconds;
                long source = next < count ? _events[next].IntendedMicroseconds : Int64.MaxValue;
                Op("adaptive.complete " + S(localBoundary) + " " + S(source), () => B(_adaptive.CompleteIfClosed(localBoundary, source)), state);
                long boundary = 0;
                Op("adaptive.effective " + S(localBoundary), () => { boundary = _adaptive.EffectiveBoundary(localBoundary); return S(boundary); }, null);
                if (next < count && source <= boundary)
                {
                    long tick = _events[next].AbsoluteTick;
                    Op("pernote.begin " + S(tick), () => { _perNote.BeginSourceTick(tick); return null; }, state);
                    while (next < count && _events[next].AbsoluteTick == tick)
                    {
                        int index = next++;
                        Op("pernote.admit " + Tok(index), () => S((int)_perNote.Admit(_events[index])), state);
                    }
                    Op("pernote.end", () => { _perNote.EndSourceTick(); return null; }, state);
                    Op("pernote.take", () => S(_perNote.TakeFilteredEventCount()), state);
                }
                else if (boundary != Int64.MaxValue)
                {
                    long b = boundary;
                    int emitted = 0;
                    Op("pernote.emit " + S(b) + " 128", () => { emitted = _perNote.EmitBoundary(b, emissions); return EmissionsText(emissions, emitted); }, state);
                    Op("adaptive.pass " + S(b) + " " + S(emitted), () => { _adaptive.RecordBoundaryPass(b, emitted); return null; }, state);
                    Op("pernote.take", () => S(_perNote.TakeFilteredEventCount()), state);
                }
                if (next == count && !completed)
                {
                    long end = _song.DurationMicroseconds;
                    Op("pernote.complete " + S(end), () => { _perNote.CompleteSource(end); return null; }, state);
                    Op("pernote.take", () => S(_perNote.TakeFilteredEventCount()), state);
                    completed = true;
                }
                // Channel disable: the ordered control retires that channel's support.
                if (next > 0 && Pick(90) == 0)
                {
                    int channel = Pick(18) - 1;
                    long now = _events[Math.Min(next, count) - 1].IntendedMicroseconds + Pick(3000);
                    Op("pernote.retire " + S(channel) + " " + S(now), () => S(_perNote.RetireChannel(channel, now)), state);
                }
                if (Pick(60) == 0)
                {
                    int pitch = Pick(130) - 1;
                    int track = Pick(2), queryChannel = Pick(3);
                    Op("pernote.query " + S(pitch) + " " + S(track) + " " + S(queryChannel), () => B(_perNote.IsActive(pitch)) + " " +
                        S(_perNote.OwnerChannel(pitch)) + " " + B(_perNote.HasActiveOccurrenceForTesting(track, queryChannel, pitch)), null);
                }
                // Live setting change: a new gate continues the same source stream.
                if (!changed && next >= count / 2 && !completed)
                {
                    changed = true;
                    Op("new pernote " + liveChange.Text, () => { _perNote = null; _perNote = new PerNoteIntervalGate(liveChange.Interval, liveChange.LimitEnabled, liveChange.Limit); return null; }, state);
                    Op("new adaptive " + S(liveChange.Interval) + " " + B(liveChange.Adaptive), () => { _adaptive = new AdaptiveNoteIntervalClock(liveChange.Interval, liveChange.Adaptive); return null; }, state);
                }
            }
        }

        // ======================================================== delayed

        private static DelayedNoteOffGate _delayed;

        private static readonly string[] DelayedLongArrays = { "_attackDue", "_attackEmission", "_attackOrder", "_releaseDue",
            "_releaseOrder", "_recoveryDue", "_recoveryOrder" };
        private static readonly string[] DelayedOccurrenceFields = { "InUse", "KeyReleased", "SustainHeld", "Disposition",
            "Track", "Channel", "Pitch", "PreviousActive", "NextActive", "NextFree", "Order", "NoteOn", "NoteOff" };

        internal static string DelayedState()
        {
            DelayedNoteOffGate g = _delayed;
            if (g == null) return "none";
            StringBuilder text = new StringBuilder();
            byte[] state = (byte[])FV(g, "_state");
            int[] owner = (int[])FV(g, "_ownerOccurrence");
            int[] candidate = (int[])FV(g, "_candidateOccurrence");
            bool[] pending = (bool[])FV(g, "_releasePending");
            bool[] retrigger = (bool[])FV(g, "_releaseForRetrigger");
            MidiEventView[] attack = (MidiEventView[])FV(g, "_attack");
            MidiEventView[] release = (MidiEventView[])FV(g, "_release");
            long[][] longs = new long[DelayedLongArrays.Length][];
            for (int i = 0; i < longs.Length; i++) longs[i] = (long[])FV(g, DelayedLongArrays[i]);
            for (int p = 0; p < 128; p++)
            {
                text.Append(S(p)).Append(':').Append(S(state[p])).Append(',').Append(S(owner[p])).Append(',').Append(S(candidate[p]))
                    .Append(',').Append(pending[p] ? '1' : '0').Append(',').Append(retrigger[p] ? '1' : '0');
                foreach (long[] values in longs) text.Append(',').Append(S(values[p]));
                text.Append(',').Append(Ev(attack[p])).Append(',').Append(Ev(release[p])).Append(';');
            }
            foreach (bool down in (bool[])FV(g, "_sustainDown")) text.Append(down ? '1' : '0');
            int[] activeHead = (int[])FV(g, "_activeHead");
            int[] activeTail = (int[])FV(g, "_activeTail");
            for (int slot = 0; slot < activeHead.Length; slot++)
            {
                if (activeHead[slot] < 0 && activeTail[slot] < 0) continue;
                text.Append('k').Append(S(slot)).Append('/').Append(S(activeTail[slot])).Append(':');
                for (int node = activeHead[slot]; node >= 0; node = Convert.ToInt32(FV(Node(g, "_occurrenceChunks", node), "NextActive"), Inv))
                {
                    text.Append(S(node)).Append(':');
                    Fields(text, Node(g, "_occurrenceChunks", node), DelayedOccurrenceFields);
                    text.Append(';');
                }
            }
            return "nb" + S(g.NextBoundaryMicroseconds) + " oc" + S(g.NextBoundaryOutputCount) + " f" + S(L(g, "_filteredEvents")) +
                " id" + S(g.InsideDurationFilteredForTesting) + " rl" + S(g.RecoveryLostFilteredForTesting) +
                " re" + S(g.RecoveryEndedFilteredForTesting) + " so" + S(L(g, "_sourceOrder")) + " sc" + B(Z(g, "_sourceComplete")) +
                " live" + S(g.LiveOccurrenceCountForTesting) + " fr" + S(L(g, "_freeOccurrence")) + " nu" + S(L(g, "_nextUnusedOccurrence")) +
                " fc" + S(L(g, "_musicalFairnessCursor")) + " seg" + S(g.OccurrenceSegmentCountForTesting) + " d" + Digest(text.ToString());
        }

        private static void DelayedFamily()
        {
            Section("delayed-note-off-gate");
            _rng = 0x5EED0401;
            GateConfig[] configs =
            {
                new GateConfig(0, false, 128, false), new GateConfig(1, false, 128, false),
                new GateConfig(4000, false, 128, true), new GateConfig(20000, true, 3, false),
                new GateConfig(160000, true, 1, true), new GateConfig(200000, false, 128, false),
                new GateConfig(20000, true, 128, true), new GateConfig(Int64.MaxValue / 3, false, 128, false),
                new GateConfig(52080, true, 6, false)
            };
            string[] fixtures = { "models-notes.mid", "models-dense.mid", "f1-v1-test-midi.mid", "many-tracks-ties.mid", "index-chase-rich.mid" };
            int configIndex = 0;
            foreach (string fixture in fixtures)
            {
                LoadFixture(fixture);
                for (int run = 0; run < 3; run++)
                {
                    DelayedRun(configs[configIndex % configs.Length], configs[(configIndex + 5) % configs.Length], 700);
                    configIndex++;
                }
            }
            Section("delayed-note-off-gate-edges");
            LoadFixture("index-chase-rich.mid");
            foreach (string args in new[] { "-1 0 128", "5 1 0", "5 1 129" })
            {
                string[] p = args.Split(' ');
                Op("new delayed " + args, () => { _delayed = null; _delayed = new DelayedNoteOffGate(Int64.Parse(p[0], Inv), p[1] == "1", Int32.Parse(p[2], Inv)); return null; }, () => GateAndAdaptiveState(DelayedState));
            }
            for (int i = 0; i < _events.Count && i < 300; i++)
            {
                int index = i;
                Op("delayed.terminating " + Tok(index), () => B(DelayedNoteOffGate.IsNoteTerminatingControl(_events[index])), null);
            }
        }

        private static void DelayedRun(GateConfig config, GateConfig liveChange, int eventLimit)
        {
            Func<string> state = () => GateAndAdaptiveState(DelayedState);
            Op("new delayed " + config.Text, () => { _delayed = null; _delayed = new DelayedNoteOffGate(config.Interval, config.LimitEnabled, config.Limit); return null; }, state);
            Op("new adaptive " + S(config.Interval) + " " + B(config.Adaptive), () => { _adaptive = new AdaptiveNoteIntervalClock(config.Interval, config.Adaptive); return null; }, state);
            int count = Math.Min(_events.Count, eventLimit);
            int next = 0;
            bool completed = false;
            bool changed = false;
            List<MidiEventView> emissions = new List<MidiEventView>(128);
            int actions = 0;
            while ((next < count || !completed || _delayed.HasPendingTransitions) && actions++ < 5000)
            {
                long localBoundary = _delayed.NextBoundaryMicroseconds;
                long source = next < count ? _events[next].IntendedMicroseconds : Int64.MaxValue;
                Op("adaptive.complete " + S(localBoundary) + " " + S(source), () => B(_adaptive.CompleteIfClosed(localBoundary, source)), state);
                long boundary = 0;
                Op("adaptive.effective " + S(localBoundary), () => { boundary = _adaptive.EffectiveBoundary(localBoundary); return S(boundary); }, null);
                bool precedes = false;
                if (next < count)
                {
                    int index = next;
                    long b0 = boundary;
                    Op("delayed.precedes " + Tok(index) + " " + S(b0), () => { precedes = _delayed.SourcePrecedesBoundary(_events[index], b0); return B(precedes); }, null);
                    if (Pick(20) == 0) Op("delayed.precedes1 " + Tok(index), () => B(_delayed.SourcePrecedesBoundary(_events[index])), null);
                }
                if (boundary < source || boundary == source && !precedes)
                {
                    long b = boundary;
                    int emitted = 0;
                    Op("delayed.emit " + S(b), () => { emitted = _delayed.EmitBoundary(b, emissions); return EmissionsText(emissions, emitted); }, state);
                    Op("adaptive.pass " + S(b) + " " + S(emitted), () => { _adaptive.RecordBoundaryPass(b, emitted); return null; }, state);
                    Op("delayed.take", () => S(_delayed.TakeFilteredEventCount()), state);
                }
                else if (next < count)
                {
                    int index = next++;
                    DelayedNoteOffAdmission admission = DelayedNoteOffAdmission.Accepted;
                    Op("delayed.admit " + Tok(index), () => { admission = _delayed.Admit(_events[index]); return S((int)admission); }, state);
                    if (admission == DelayedNoteOffAdmission.NotNote && DelayedNoteOffGate.IsNoteTerminatingControl(_events[index]))
                    {
                        int channel = _events[index].Channel;
                        long now = _events[index].IntendedMicroseconds;
                        Op("delayed.retire " + S(channel) + " " + S(now), () => S(_delayed.RetireChannel(channel, now)), state);
                    }
                    Op("delayed.take", () => S(_delayed.TakeFilteredEventCount()), state);
                }
                if (next == count && !completed)
                {
                    long end = _song.DurationMicroseconds;
                    Op("delayed.complete " + S(end), () => { _delayed.CompleteSource(end); return null; }, state);
                    Op("delayed.take", () => S(_delayed.TakeFilteredEventCount()), state);
                    completed = true;
                }
                if (next > 0 && Pick(120) == 0)
                {
                    int channel = Pick(18) - 1;
                    long now = _events[Math.Min(next, count) - 1].IntendedMicroseconds;
                    Op("delayed.retire " + S(channel) + " " + S(now), () => S(_delayed.RetireChannel(channel, now)), state);
                }
                if (!changed && next >= count / 2 && !completed)
                {
                    changed = true;
                    Op("new delayed " + liveChange.Text, () => { _delayed = null; _delayed = new DelayedNoteOffGate(liveChange.Interval, liveChange.LimitEnabled, liveChange.Limit); return null; }, state);
                    Op("new adaptive " + S(liveChange.Interval) + " " + B(liveChange.Adaptive), () => { _adaptive = new AdaptiveNoteIntervalClock(liveChange.Interval, liveChange.Adaptive); return null; }, state);
                }
            }
        }

        // ========================================================= attack

        private static MusicalAttackRateLimiter _attack;

        private static string SlotListText(object gate, string[] fields)
        {
            StringBuilder text = new StringBuilder();
            int[] head = (int[])FV(gate, "_head");
            int[] tail = (int[])FV(gate, "_tail");
            for (int slot = 0; slot < head.Length; slot++)
            {
                if (head[slot] < 0 && tail[slot] < 0) continue;
                text.Append(S(slot)).Append('/').Append(S(tail[slot])).Append(':');
                for (int node = head[slot]; node >= 0; node = Convert.ToInt32(FV(Node(gate, "_chunks", node), "Next"), Inv))
                {
                    text.Append(S(node)).Append(':');
                    Fields(text, Node(gate, "_chunks", node), fields);
                    text.Append(';');
                }
            }
            return text.ToString();
        }

        private static readonly string[] AttackOccurrenceFields = { "InUse", "Accepted", "Channel", "Pitch", "Previous", "Next", "NextFree" };

        internal static string AttackState()
        {
            MusicalAttackRateLimiter g = _attack;
            if (g == null) return "none";
            return "live" + S(g.LiveOccurrenceCountForTesting) + " fr" + S(L(g, "_free")) + " nu" + S(L(g, "_nextUnused")) +
                " seg" + S(g.OccurrenceSegmentCountForTesting) + " fc" + S(L(g, "_fairnessCursor")) + " rj" + S(L(g, "_rejectedAttacks")) +
                " sp" + S(L(g, "_suppressedReleases")) + " te" + S(((IList)FV(g, "_tickEvents")).Count) +
                " p[" + PacerState((ProgressiveRatePacer)FV(g, "_pacer")) + "] d" + Digest(SlotListText(g, AttackOccurrenceFields));
        }

        private static void AttackFamily()
        {
            Section("note-start-rate-gate");
            _rng = 0x5EED0501;
            long[] rates = { 0, 1, 7, 20, 21, 100, 1000, 9999999, 40, 3 };
            string[] fixtures = { "models-notes.mid", "models-dense.mid", "many-tracks-ties.mid", "f0-all-message-types.mid" };
            int configIndex = 0;
            foreach (string fixture in fixtures)
            {
                LoadFixture(fixture);
                for (int run = 0; run < 3; run++)
                {
                    AttackRun(rates[configIndex % rates.Length], rates[(configIndex + 3) % rates.Length], 900);
                    configIndex++;
                }
            }
            Section("note-start-rate-gate-edges");
            LoadFixture("models-notes.mid");
            foreach (long rate in new long[] { -1, 10000000 })
                Op("new attack " + S(rate), () => { _attack = null; _attack = new MusicalAttackRateLimiter(rate); return null; }, AttackState);
            Op("new attack 5", () => { _attack = new MusicalAttackRateLimiter(5); return null; }, AttackState);
            Op("attack.admit " + Tok(0), () => { _attack.Admit(_events[0]); return null; }, AttackState);
            Op("attack.begin 0", () => { _attack.BeginSourceTick(0); return null; }, AttackState);
            Op("attack.end", () => { List<MidiEventView> e = new List<MidiEventView>(); return EmissionsText(e, _attack.EndSourceTick(e)); }, AttackState);
            Op("attack.end", () => { List<MidiEventView> e = new List<MidiEventView>(); return EmissionsText(e, _attack.EndSourceTick(e)); }, AttackState);
            Op("attack.avail 0 5", () => S(_attack.AvailableStartsForTesting(0, 5)), AttackState);
        }

        private static void AttackRun(long rate, long liveChange, int eventLimit)
        {
            Op("new attack " + S(rate), () => { _attack = null; _attack = new MusicalAttackRateLimiter(rate); return null; }, AttackState);
            int count = Math.Min(_events.Count, eventLimit);
            int next = 0;
            bool changed = false;
            List<MidiEventView> emissions = new List<MidiEventView>(128);
            while (next < count)
            {
                long tick = _events[next].AbsoluteTick;
                Op("attack.begin " + S(tick), () => { _attack.BeginSourceTick(tick); return null; }, AttackState);
                while (next < count && _events[next].AbsoluteTick == tick)
                {
                    int index = next++;
                    Op("attack.admit " + Tok(index), () => { _attack.Admit(_events[index]); return null; }, AttackState);
                }
                Op("attack.end", () => EmissionsText(emissions, _attack.EndSourceTick(emissions)), AttackState);
                Op("attack.take-rejected", () => S(_attack.TakeRejectedAttacks()), AttackState);
                Op("attack.take-suppressed", () => S(_attack.TakeSuppressedReleases()), AttackState);
                if (Pick(70) == 0)
                {
                    int channel = Pick(18) - 1;
                    Op("attack.retire " + S(channel), () => { _attack.RetireChannel(channel); return null; }, AttackState);
                }
                if (Pick(90) == 0)
                {
                    long at = _events[next - 1].IntendedMicroseconds + Pick(2000);
                    long max = Pick(5);
                    Op("attack.avail " + S(at) + " " + S(max), () => S(_attack.AvailableStartsForTesting(at, max)), AttackState);
                }
                if (!changed && next >= count / 2)
                {
                    changed = true;
                    Op("new attack " + S(liveChange), () => { _attack = null; _attack = new MusicalAttackRateLimiter(liveChange); return null; }, AttackState);
                }
            }
            Op("attack.complete", () => { _attack.CompleteSource(); return null; }, AttackState);
        }

        // ===================================================== outputrate

        private static OutputRateGate _outputRate;
        private static readonly string[] OutputOccurrenceFields = { "InUse", "Accepted", "CandidateIndex", "Channel", "Pitch", "Previous", "Next", "NextFree" };

        internal static string OutputRateState()
        {
            OutputRateGate g = _outputRate;
            if (g == null) return "none";
            StringBuilder pending = new StringBuilder();
            foreach (bool value in (Queue<bool>)FV(g, "_pendingRequiredRelease")) pending.Append(value ? '1' : '0');
            return "live" + S(g.LiveOccurrenceCountForTesting) + " fr" + S(L(g, "_free")) + " nu" + S(L(g, "_nextUnused")) +
                " fc" + S(L(g, "_fairnessCursor")) + " rj" + S(L(g, "_rejectedAttacks")) + " sp" + S(L(g, "_suppressedReleases")) +
                " ro" + S(L(g, "_rejectedOtherMessages")) + " te" + S(((IList)FV(g, "_tickEvents")).Count) +
                " q" + pending + " p[" + PacerState((ProgressiveRatePacer)FV(g, "_pacer")) + "] d" +
                Digest(SlotListText(g, OutputOccurrenceFields));
        }

        private static void OutputRateFamily()
        {
            Section("output-rate-gate-analysis");
            _rng = 0x5EED0601;
            long[] rates = { 0, 1, 5, 20, 21, 100, 1000, 3125, 9999999, 41 };
            string[] fixtures = { "models-notes.mid", "models-dense.mid", "many-tracks-ties.mid", "f0-all-message-types.mid" };
            int configIndex = 0;
            foreach (string fixture in fixtures)
            {
                LoadFixture(fixture);
                for (int run = 0; run < 2; run++)
                {
                    OutputRateRun(rates[configIndex % rates.Length], rates[(configIndex + 4) % rates.Length], 1000000, false, 800);
                    configIndex++;
                }
            }
            Section("output-rate-gate-playback");
            _rng = 0x5EED0602;
            configIndex = 0;
            foreach (string fixture in fixtures)
            {
                LoadFixture(fixture);
                for (int run = 0; run < 2; run++)
                {
                    long units = run == 0 ? 1000000 : 10000000;
                    OutputRateRun(rates[(configIndex + 2) % rates.Length], rates[(configIndex + 7) % rates.Length], units, true, 800);
                    configIndex++;
                }
            }
            Section("output-rate-gate-edges");
            LoadFixture("models-notes.mid");
            foreach (string args in new[] { "-1 1000000", "10000000 1000000", "5 0", "5 -1" })
            {
                string[] p = args.Split(' ');
                Op("new outrate " + args, () => { _outputRate = null; _outputRate = new OutputRateGate(Int64.Parse(p[0], Inv), Int64.Parse(p[1], Inv)); return null; }, OutputRateState);
            }
            Op("new outrate 2 1000000", () => { _outputRate = new OutputRateGate(2, 1000000); return null; }, OutputRateState);
            Op("outrate.admit " + Tok(0), () => { _outputRate.Admit(_events[0]); return null; }, OutputRateState);
            Op("outrate.begin 0", () => { _outputRate.BeginSourceTick(0); return null; }, OutputRateState);
            for (int i = 0; i < 4; i++) Op("outrate.dispatch 5 1", () => B(_outputRate.RecordDispatch(5, true)), OutputRateState);
            Op("outrate.exceptional 5", () => B(_outputRate.RecordExceptionalDispatch(5)), OutputRateState);
            Op("outrate.avail 5", () => S(_outputRate.AvailableEvents(5)), OutputRateState);
            Op("outrate.protected " + Tok(0), () => B(OutputRateGate.IsProtectedTraffic(_events[0])), null);
        }

        private static void OutputRateRun(long rate, long liveChange, long units, bool playback, int eventLimit)
        {
            Op("new outrate " + S(rate) + " " + S(units), () => { _outputRate = null; _outputRate = new OutputRateGate(rate, units); return null; }, OutputRateState);
            long scale = units / 1000000;
            int count = Math.Min(_events.Count, eventLimit);
            int next = 0;
            bool changed = false;
            long clock = 0;
            List<MidiEventView> emissions = new List<MidiEventView>(128);
            while (next < count)
            {
                long tick = _events[next].AbsoluteTick;
                long time = _events[next].IntendedMicroseconds * scale;
                Op("outrate.begin " + S(tick), () => { _outputRate.BeginSourceTick(tick); return null; }, OutputRateState);
                while (next < count && _events[next].AbsoluteTick == tick)
                {
                    int index = next++;
                    Op("outrate.admit " + Tok(index), () => { _outputRate.Admit(_events[index]); return null; }, OutputRateState);
                }
                // Playback decides at the scripted wall time (wake latency after the
                // source time); Analysis at the ideal source time.
                if (playback) clock = Math.Max(clock, time + Pick(4) * 250 * scale);
                long decision = playback ? clock : time;
                int emitted = 0;
                Op("outrate.end " + S(decision), () => { emitted = _outputRate.EndSourceTick(emissions, decision); return EmissionsText(emissions, emitted); }, OutputRateState);
                Op("outrate.take", () => S(_outputRate.TakeRejectedAttacks()) + " " + S(_outputRate.TakeSuppressedReleases()) + " " +
                    S(_outputRate.TakeRejectedOtherMessages()), OutputRateState);
                for (int i = 0; i < emitted; i++)
                {
                    // Scripted dispatch time: each send costs 320 us, or its bytes at 31,250 bit/s.
                    if (playback) clock += (Pick(2) == 0 ? 320 : ServiceDurationCalculator.CalculateBitrateMicroseconds(emissions[i].DataLength, 31250)) * scale;
                    long at = playback ? clock : time;
                    bool required = playback && Pick(10) == 0;
                    Op("outrate.dispatch " + S(at) + " " + B(required), () => B(_outputRate.RecordDispatch(at, required)), OutputRateState);
                }
                if (playback && Pick(25) == 0)
                {
                    clock += 320 * scale;
                    long at = clock;
                    Op("outrate.exceptional " + S(at), () => B(_outputRate.RecordExceptionalDispatch(at)), OutputRateState);
                }
                if (Pick(60) == 0)
                {
                    int channel = Pick(18) - 1;
                    Op("outrate.retire " + S(channel), () => { _outputRate.RetireChannel(channel); return null; }, OutputRateState);
                }
                if (Pick(80) == 0)
                {
                    long at = playback ? clock : time;
                    long max = Pick(4) == 0 ? Int64.MaxValue : Pick(6);
                    Op("outrate.avail " + S(at), () => S(_outputRate.AvailableEvents(at)), OutputRateState);
                    Op("outrate.avail2 " + S(at) + " " + S(max), () => S(_outputRate.AvailableEvents(at, max)), OutputRateState);
                }
                if (!changed && next >= count / 2)
                {
                    changed = true;
                    Op("new outrate " + S(liveChange) + " " + S(units), () => { _outputRate = null; _outputRate = new OutputRateGate(liveChange, units); return null; }, OutputRateState);
                }
            }
            Op("outrate.complete", () => { _outputRate.CompleteSource(); return null; }, OutputRateState);
        }
    }
}
