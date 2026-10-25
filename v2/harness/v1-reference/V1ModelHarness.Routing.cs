// v1 track-routing traces (Milestone 4); see V1ModelHarness.Analysis.cs for
// the op list. The planner runs inside v1's own TrackRoutingForm (created,
// never shown), which also supplies the lane rows, the preview text and the
// Apply state. Control visibility is not read: a form that is never shown
// reports every control as invisible.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class V1ModelHarness
    {
        private static CompiledTrackRoute _route;

        // tier=N,cap=N,perc=N,usable=XXXX,manual=t.c=d;...|-
        private static string SettingsText(int tier, int capacity, int percussion, int usableMask, string manual)
        {
            return "tier=" + S(tier) + ",cap=" + S(capacity) + ",perc=" + S(percussion) + ",usable=" +
                usableMask.ToString("X4", Inv) + ",manual=" + manual;
        }

        private static TrackRoutingSettings ParseSettings(string text)
        {
            TrackRoutingSettings settings = new TrackRoutingSettings();
            foreach (string part in text.Split(','))
            {
                int eq = part.IndexOf('=');
                string key = part.Substring(0, eq), value = part.Substring(eq + 1);
                if (key == "tier") settings.MaximumAutomaticTier = (TrackRoutingTier)Int32.Parse(value, Inv);
                else if (key == "cap") settings.TargetCapacity = Int32.Parse(value, Inv);
                else if (key == "perc") settings.PercussionPolicy = (PercussionRoutingPolicy)Int32.Parse(value, Inv);
                else if (key == "usable")
                {
                    int mask = Int32.Parse(value, NumberStyles.HexNumber, Inv);
                    for (int channel = 0; channel < 16; channel++)
                        settings.UsableDestinationChannels[channel] = (mask & (1 << channel)) != 0;
                }
                else if (key == "manual")
                {
                    if (value == "-") continue;
                    foreach (string entry in value.Split(';'))
                    {
                        string[] f = entry.Split('.', '=');
                        settings.ManualDestinations[new TrackLaneKey(Int32.Parse(f[0], Inv), Int32.Parse(f[1], Inv))] =
                            Int32.Parse(f[2], Inv);
                    }
                }
                else throw new ArgumentException("unknown settings key " + key);
            }
            return settings;
        }

        private static string LaneTok(TrackLaneKey lane) { return S(lane.Track) + "." + S(lane.SourceChannel); }
        private static string PatchTok(MidiPatchIdentity p) { return S(p.BankMsb) + "/" + S(p.BankLsb) + "/" + S(p.Program); }

        private static string RouteText(CompiledTrackRoute route)
        {
            List<string> lines = new List<string>();
            lines.Add("valid " + B(route.IsValid) + " explicit " + B(route.RequiresExplicitApply) + " highest " +
                S((int)route.HighestTier) + " lanes " + S(route.Lanes.Count) + " assignments " + S(route.Assignments.Count) +
                " destinations " + S(route.Destinations.Count) + " errors " + S(route.Errors.Count));
            foreach (string error in route.Errors) lines.Add("error " + Esc(error));
            foreach (TrackRouteLaneProfile lane in route.Lanes)
            {
                StringBuilder timeline = new StringBuilder();
                for (int i = 0; i < lane.PatchTimeline.Count; i++)
                {
                    if (i == 8) { timeline.Append(" ..."); break; }
                    timeline.Append(' ').Append(S(lane.PatchTimeline[i].EventIndex)).Append(':').Append(PatchTok(lane.PatchTimeline[i].Patch));
                }
                Fnv timelineDigest = new Fnv();
                foreach (TrackRoutePatchChange change in lane.PatchTimeline)
                    timelineDigest.Add(S(change.EventIndex) + ":" + PatchTok(change.Patch) + ";");
                Fnv intervals = new Fnv();
                foreach (TrackRouteSoundInterval interval in lane.SoundingIntervals)
                    intervals.Add(S(interval.StartEventIndex) + ":" + S(interval.EndEventIndex) + ":" + S(interval.StartMicroseconds) +
                        ":" + S(interval.EndMicroseconds) + ":" + S(interval.PrefixMaximumEndMicroseconds) + ":" + S(interval.Key) + ";");
                lines.Add("lane " + LaneTok(lane.Lane) + " name " + Esc(lane.TrackName) + " inst " + Esc(lane.InstrumentName) +
                    " display " + Esc(lane.DisplayName) + " ev " + S(lane.EventCount) + " at " + S(lane.AttackCount) +
                    " first " + S(lane.FirstAttackMicroseconds) + " last " + S(lane.LastReleaseMicroseconds) +
                    " dyn " + B(lane.HasDynamicState) + " sus " + B(lane.HasSustain) + " sys " + B(lane.TrackContainsSystemTraffic) +
                    " sig " + lane.DynamicStateSignature.ToString("x16", Inv) + " rep " + PatchTok(lane.RepresentativePatch) +
                    " dom " + lane.DominanceScore.ToString(Inv) + " hist " + S(lane.PatchHistory.Count) +
                    " intervals " + S(lane.SoundingIntervals.Count) + " " + intervals.Text +
                    " timeline " + S(lane.PatchTimeline.Count) + " " + timelineDigest.Text + timeline);
            }
            foreach (TrackRouteAssignment a in route.Assignments)
                lines.Add("assign " + LaneTok(a.Profile.Lane) + " dest " + S(a.DestinationChannel) + " tier " + S((int)a.Tier) +
                    " flags " + B(a.Manual) + B(a.RepresentativePatch) + B(a.StateConflict) + B(a.SameKeyOverlap) +
                    B(a.TimeHandoff) + B(a.PercussionException) + B(a.Retimbred) + B(a.Omitted) +
                    " | " + Esc(String.Join("; ", a.Warnings.ToArray())));
            foreach (TrackRouteDestinationPreview d in route.Destinations)
            {
                StringBuilder members = new StringBuilder();
                foreach (TrackRouteAssignment a in d.Assignments) members.Append(' ').Append(LaneTok(a.Profile.Lane));
                lines.Add("dest " + S(d.DestinationChannel) + " rep " + PatchTok(d.RepresentativePatch) + " lane " +
                    LaneTok(d.RepresentativeLane) + " members" + members);
            }
            TrackRouteStatistics s = route.Statistics;
            lines.Add("stats routed " + S(s.RoutedLaneCount) + " dests " + S(s.DestinationCount) + " merged " +
                S(s.MergedLaneCount) + " retimbred " + S(s.RetimbredLaneCount) + " omitted " + S(s.OmittedLaneCount) +
                " omitted-starts " + S(s.OmittedNoteStartCount) + " generated " + S(s.GeneratedDestinationStateCount) +
                " included " + S(s.IncludedSourceEventCount) + " output " + S(s.RoutedOutputEventCount));
            sbyte[] eventDestinations = (sbyte[])F(route, "_eventDestinations");
            Fnv destinationDigest = new Fnv();
            int[] histogram = new int[18];
            foreach (sbyte d in eventDestinations)
            {
                destinationDigest.Add(S(d) + ",");
                histogram[d + 2]++;
            }
            StringBuilder hist = new StringBuilder();
            for (int i = 0; i < histogram.Length; i++)
                if (histogram[i] != 0) hist.Append(' ').Append(S(i - 2)).Append(':').Append(S(histogram[i]));
            lines.Add("event-destinations " + S(eventDestinations.Length) + " " + destinationDigest.Text + hist);
            Dictionary<int, List<MidiEvent>> generated = (Dictionary<int, List<MidiEvent>>)F(route, "_generatedPatchEventsBefore");
            List<int> keys = new List<int>(generated.Keys);
            keys.Sort();
            Fnv generatedDigest = new Fnv();
            StringBuilder generatedSample = new StringBuilder();
            int total = 0;
            foreach (int key in keys)
            {
                StringBuilder entry = new StringBuilder(S(key) + "<");
                foreach (MidiEvent e in generated[key]) entry.Append(Ev(MidiEventView.FromEvent(e, -1))).Append(' ');
                entry.Append('>');
                generatedDigest.Add(entry.ToString());
                if (total < 6) generatedSample.Append(' ').Append(entry);
                total += generated[key].Count;
            }
            lines.Add("generated " + S(keys.Count) + " " + S(total) + " " + generatedDigest.Text + generatedSample);
            return String.Join("\n= ", lines.ToArray());
        }

        private static string FormText(TrackRoutingForm form, TrackRoutingSettings settings)
        {
            List<string> lines = new List<string>();
            DataGridView grid = (DataGridView)F(form, "_lanes");
            foreach (DataGridViewRow row in grid.Rows)
            {
                Color back = row.DefaultCellStyle.BackColor;
                int style = back == Color.MistyRose ? 1 : back == Color.LightCoral ? 2 : back == Color.LemonChiffon ? 3 : 0;
                StringBuilder text = new StringBuilder("row " + LaneTok((TrackLaneKey)row.Tag));
                for (int cell = 0; cell < 5; cell++)
                    text.Append(" | ").Append(Esc(Convert.ToString(row.Cells[cell].Value, CultureInfo.CurrentCulture)));
                text.Append(" | ").Append(S(style));
                lines.Add(text.ToString());
            }
            string preview = form.PreviewTextForTesting;
            string[] previewLines = preview.Split(new[] { "\r\n" }, StringSplitOptions.None);
            for (int i = 0; i < previewLines.Length; i++) lines.Add("preview " + Esc(previewLines[i]));
            StringBuilder used = new StringBuilder("used ");
            for (int channel = 0; channel < 16; channel++) used.Append(B(form.ChannelCaptionBoldForTesting(channel)));
            lines.Add(used.ToString());
            lines.Add(ApplyStateText("apply", form));
            if (form.LossyConfirmationRequiredForTesting)
            {
                form.SetLossyConfirmationForTesting(true);
                lines.Add(ApplyStateText("confirm", form));
            }
            form.ConfirmApplyForTesting();
            lines.Add("applied " + B(form.SelectedRoute != null) + " " + B(form.ExplicitlyConfirmed) + " " +
                B(form.SelectedSettings != null));
            return String.Join("\n= ", lines.ToArray());
        }

        private static string ApplyStateText(string name, TrackRoutingForm form)
        {
            Label status = (Label)F(form, "_status");
            CompiledTrackRoute compiled = form.CompiledRouteForTesting;
            bool valid = compiled != null && compiled.IsValid;
            return name + " " + B(valid) + " " + B(valid && compiled.RequiresExplicitApply) + " " +
                B(form.ApplyEnabledForTesting) + " " + B(status.ForeColor == Color.DarkRed) + " " + Esc(status.Text);
        }

        private static string RouteOp(string settingsText)
        {
            TrackRoutingSettings settings = ParseSettings(settingsText);
            _route = null;
            using (TrackRoutingForm form = new TrackRoutingForm(_song, settings, false, 100))
            {
                _route = form.CompiledRouteForTesting;
                return RouteText(_route) + "\n= " + FormText(form, settings);
            }
        }

        private static MidiSong _routedSong;

        private static string RoutedViewOp()
        {
            _routedSong = null;
            MidiSong routed = TrackRouteApplication.CreateRoutedSong(_song, _route);
            _routedSong = routed;
            MidiEventReader view = routed.GetEventReader();
            Fnv digest = new Fnv();
            StringBuilder sample = new StringBuilder();
            for (int i = 0; i < view.Count; i++)
            {
                string text = Ev(view[i]) + "/" + S(view[i].SourceEventIndex);
                digest.Add(text);
                digest.Add(";");
                if (i < 4) sample.Append(' ').Append(text);
            }
            return "view " + S(view.Count) + " " + digest.Text + " lower " +
                S(routed.EventStore.LowerBoundByTime(routed.DurationMicroseconds / 2)) + sample;
        }

        // ------------------------------------------------------------ routing-large

        // The 1,000,000 simultaneously-held-notes limit (MaximumActiveAttacks): a file at the
        // limit (accepted) and one over it (rejected), generated on demand under fixtures\large.
        private static void RoutingLargeFamily()
        {
            Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
            Thread.CurrentThread.CurrentUICulture = CultureInfo.InvariantCulture;
            foreach (string fixture in new string[] { "routing-held-1000000.mid", "routing-held-1000001.mid" })
            {
                Section("fixture " + fixture);
                LoadFixture(fixture);
                foreach (string text in new string[] { SettingsText(0, 16, 0, 0xFFFF, "-"), SettingsText(2, 4, 0, 0xFFFF, "-") })
                {
                    string captured = text;
                    Op("route " + captured, () => RouteOp(captured), null);
                    if (_route != null && _route.IsValid) Op("routed-view", RoutedViewOp, null);
                }
            }
        }

        // ------------------------------------------------------------ routing family

        private static List<TrackLaneKey> ProbeLanes()
        {
            try
            {
                List<TrackLaneKey> lanes = new List<TrackLaneKey>();
                foreach (TrackRouteLaneProfile lane in TrackRoutePlanner.Compile(_song, new TrackRoutingSettings()).Lanes)
                    lanes.Add(lane.Lane);
                return lanes;
            }
            catch (InvalidOperationException) { return new List<TrackLaneKey>(); }
        }

        private static string ManualText(List<TrackLaneKey> lanes, int count, bool invalid)
        {
            if (lanes.Count == 0 || count == 0) return "-";
            SortedDictionary<TrackLaneKey, int> manual = new SortedDictionary<TrackLaneKey, int>();
            for (int i = 0; i < count; i++)
            {
                TrackLaneKey lane = lanes[Pick(lanes.Count)];
                // v1's dialog throws when its first row has destination 16 (unreachable
                // from its own selector), so that row only gets -1.
                int destination = invalid && i == 0 ? (lane.Equals(lanes[0]) || Pick(2) == 0 ? -1 : 16) : Pick(16);
                manual[lane] = destination;
            }
            List<string> parts = new List<string>();
            foreach (KeyValuePair<TrackLaneKey, int> pair in manual)
                parts.Add(LaneTok(pair.Key) + "=" + S(pair.Value));
            return String.Join(";", parts.ToArray());
        }

        private static void RoutingFamily()
        {
            Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
            Thread.CurrentThread.CurrentUICulture = CultureInfo.InvariantCulture;
            _rng = 0x51ED270B;
            string[] fixtures = { "routing-lanes.mid", "routing-many-lanes.mid", "routing-exact-merge.mid", "models-notes.mid", "index-chase-rich.mid",
                "many-tracks-ties.mid", "f0-all-message-types.mid", "metadata-names.mid", "empty-tracks.mid",
                "routing-patches-4096.mid", "routing-patches-4097.mid" };
            foreach (string fixture in fixtures)
            {
                Section("fixture " + fixture);
                LoadFixture(fixture);
                // A route compiled for the previous song must be refused.
                if (_route != null) Op("routed-view", RoutedViewOp, null);
                List<TrackLaneKey> lanes = ProbeLanes();
                List<string> settings = new List<string>
                {
                    SettingsText(0, 16, 0, 0xFFFF, "-"),
                    SettingsText(1, 16, 0, 0xFFFF, "-"),
                    SettingsText(2, 16, 0, 0xFFFF, "-"),
                    SettingsText(0, 4, 0, 0xFFFF, "-"),
                    SettingsText(1, 4, 0, 0xFFFF, "-"),
                    SettingsText(2, 4, 0, 0xFFFF, "-"),
                    SettingsText(2, 1, 1, 0xFFFF, "-"),
                    SettingsText(1, 8, 1, 0xFFFF, "-"),
                    SettingsText(2, 16, 0, 0xFDFF, "-"),
                    SettingsText(0, 16, 0, 0x0200, "-"),
                    SettingsText(0, 16, 1, 0x0000, "-"),
                    SettingsText(2, 3, 0, 0x0E07, "-"),
                    SettingsText(0, 0, 0, 0xFFFF, "-"),
                    SettingsText(2, 99, 0, 0xFFFF, "-"),
                    SettingsText(0, 16, 0, 0xFFFF, ManualText(lanes, 2, false)),
                    SettingsText(1, 2, 0, 0xFFFF, ManualText(lanes, 4, false)),
                    SettingsText(2, 4, 1, 0xFFFF, ManualText(lanes, 3, true)),
                };
                if (lanes.Count != 0)
                {
                    settings.Add(SettingsText(0, 16, 0, 0xFFFF, LaneTok(lanes[0]) + "=9"));
                    settings.Add(SettingsText(0, 16, 0, 0xFDFF, LaneTok(lanes[lanes.Count - 1]) + "=9"));
                }
                for (int i = 0; i < 18; i++)
                {
                    int mask = Pick(3) == 0 ? (int)(Next() & 0xFFFF) : 0xFFFF;
                    settings.Add(SettingsText(Pick(3), 1 + Pick(16), Pick(2), mask,
                        Pick(3) == 0 ? ManualText(lanes, 1 + Pick(3), Pick(5) == 0) : "-"));
                }
                int analyzed = 0;
                foreach (string text in settings)
                {
                    string captured = text;
                    Op("route " + captured, () => RouteOp(captured), null);
                    if (_route == null || !_route.IsValid && Pick(4) != 0) continue;
                    Op("routed-view", RoutedViewOp, null);
                    if (_routedSong != null && analyzed < 4)
                    {
                        analyzed++;
                        long bucket = Pick(2) == 0 ? 25000 : 100000;
                        AnalyzeCall("routed-analyze", _routedSong, bucket, analyzed == 1 ? "null" : RandomConfiguration());
                        AnalyzeCall("routed-analyze", _routedSong, bucket, Cfg(true, 0, 1000, 31250, 1000, false, 32, false,
                            5000, 0, false, true, false, false, 128, false, false, false, true, "1.3=10"));
                    }
                }
            }
        }
    }
}
