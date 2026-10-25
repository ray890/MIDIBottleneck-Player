// v1 reference side of the Track routing dialog parity check (Milestone 6).
//
// Compiled together with the UNCHANGED v1.0.44 sources (src/*.cs except Program.cs). It
// drives v1's own TrackRoutingForm (created, never shown) through scripted user sessions
// (tier, capacity, percussion, usable channels, row selection, manual destination,
// confirmation, Apply, Use original routing) and records every dialog state after each
// action. v2's midibn-routing-app-tests replays the same actions against the portable
// dialog model (app/routing_dialog_model.h) and requires identical states.
//
// Usage:  routing-ui-reference <fixture-dir> <output-dir> [--full]
//
// One output file per fixture (UTF-8, "\n" line ends, fields separated by TAB, text
// fields escaped as in the other harness tables). Line kinds:
//   session  <n> <key=value ...>      settings and options a session starts from
//   X        <exception type> <msg>   the dialog could not be created (v1 throws)
//   S        <state fields ...>       the dialog state (after construction and after each action)
//   A        <action>                 a user action (tier N | cap N | perc N | usable C 0|1 |
//                                     select R | dest I | confirm 0|1 | apply | original)
//   O        <kind> <explicit> <settings>   the dialog was closed by Apply or Use original routing
//   end
// Row text and preview text are compared through 64-bit FNV-1a digests of the UTF-16 text
// (--full writes the text too, for debugging a difference).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static class RoutingUiReference
    {
        private const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public |
            BindingFlags.Instance | BindingFlags.Static;
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        private static bool _full;
        private static ulong _rng;
        private static StringBuilder _out;

        [STAThread]
        private static int Main(string[] args)
        {
            try
            {
                if (args.Length < 2)
                {
                    Console.Error.WriteLine("usage: routing-ui-reference <fixture-dir> <output-dir> [--full]");
                    return 2;
                }
                _full = args.Length > 2 && args[2] == "--full";
                string fixtures = args[0], outDir = args[1];
                Directory.CreateDirectory(outDir);
                string[] names =
                {
                    "routing-lanes.mid", "routing-many-lanes.mid", "routing-exact-merge.mid", "models-notes.mid",
                    "index-chase-rich.mid", "many-tracks-ties.mid", "f0-all-message-types.mid", "metadata-names.mid",
                    "empty-tracks.mid", "routing-patches-4096.mid", "routing-patches-4097.mid"
                };
                long total = 0;
                foreach (string name in names)
                {
                    _out = new StringBuilder();
                    _rng = 0x2545F4914F6CDD1DUL;
                    Fixture(fixtures, name);
                    string path = Path.Combine(outDir, name.Substring(0, name.LastIndexOf('.')) + ".txt");
                    File.WriteAllText(path, _out.ToString(), new UTF8Encoding(false));
                    total += new FileInfo(path).Length;
                }
                Console.WriteLine("Wrote " + names.Length.ToString(Inv) + " tables to " + outDir + " (" +
                    total.ToString(Inv) + " bytes).");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("routing-ui-reference failure: " + ex);
                return 3;
            }
        }

        // ---------------------------------------------------------------- helpers

        private static void Line(string text) { _out.Append(text).Append('\n'); }
        private static string S(long value) { return value.ToString(Inv); }
        private static string B(bool value) { return value ? "1" : "0"; }

        private static ulong Next()
        {
            _rng ^= _rng >> 12;
            _rng ^= _rng << 25;
            _rng ^= _rng >> 27;
            return _rng * 2685821657736338717UL;
        }
        private static int Pick(int n) { return (int)(Next() % (ulong)n); }

        private static string Esc(string value)
        {
            if (value == null) return "";
            StringBuilder b = new StringBuilder();
            foreach (char c in value)
            {
                if (c == '\\') b.Append("\\\\");
                else if (c == '\t') b.Append("\\t");
                else if (c == '\r') b.Append("\\r");
                else if (c == '\n') b.Append("\\n");
                else if (c < 0x20 || c > 0x7E) b.Append("\\u").Append(((int)c).ToString("x4", Inv));
                else b.Append(c);
            }
            return b.ToString();
        }

        // FNV-1a, 64 bit, over the UTF-16 code units (low byte first).
        private static string Digest(string text)
        {
            ulong hash = 14695981039346656037UL;
            foreach (char c in text)
            {
                hash ^= (ulong)(c & 0xFF);
                hash *= 1099511628211UL;
                hash ^= (ulong)(c >> 8);
                hash *= 1099511628211UL;
            }
            return hash.ToString("x16", Inv);
        }

        private static object F(object target, string field)
        {
            FieldInfo info = target.GetType().GetField(field, Hidden);
            if (info == null) throw new InvalidOperationException("missing field " + field);
            return info.GetValue(target);
        }

        private static string LaneTok(TrackLaneKey lane) { return S(lane.Track) + "." + S(lane.SourceChannel); }

        private static string SettingsText(TrackRoutingSettings s)
        {
            int mask = 0;
            for (int channel = 0; channel < 16; channel++)
                if (s.UsableDestinationChannels[channel]) mask |= 1 << channel;
            List<string> parts = new List<string>();
            foreach (KeyValuePair<TrackLaneKey, int> pair in s.ManualDestinations)
                parts.Add(LaneTok(pair.Key) + "=" + S(pair.Value));
            parts.Sort(StringComparer.Ordinal);
            return "tier=" + S((int)s.MaximumAutomaticTier) + ",cap=" + S(s.TargetCapacity) + ",perc=" +
                S((int)s.PercussionPolicy) + ",usable=" + mask.ToString("X4", Inv) + ",manual=" +
                (parts.Count == 0 ? "-" : String.Join(";", parts.ToArray()));
        }

        private static TrackRoutingSettings MakeSettings(int tier, int capacity, int perc, int mask,
            List<KeyValuePair<TrackLaneKey, int>> manual)
        {
            TrackRoutingSettings s = new TrackRoutingSettings();
            s.MaximumAutomaticTier = (TrackRoutingTier)tier;
            s.TargetCapacity = capacity;
            s.PercussionPolicy = (PercussionRoutingPolicy)perc;
            for (int channel = 0; channel < 16; channel++)
                s.UsableDestinationChannels[channel] = (mask & (1 << channel)) != 0;
            if (manual != null)
                foreach (KeyValuePair<TrackLaneKey, int> pair in manual) s.ManualDestinations[pair.Key] = pair.Value;
            return s;
        }

        // ---------------------------------------------------------------- dialog state

        private static string State(TrackRoutingForm form, TrackRoutingSettings unused)
        {
            DataGridView grid = (DataGridView)F(form, "_lanes");
            ComboBox laneCombo = (ComboBox)F(form, "_laneDestination");
            Label status = (Label)F(form, "_status");
            CheckBox confirm = (CheckBox)F(form, "_confirmLossy");
            Button original = (Button)F(form, "_original");
            ComboBox tier = (ComboBox)F(form, "_tier");
            NumericUpDown capacity = (NumericUpDown)F(form, "_capacity");
            ComboBox perc = (ComboBox)F(form, "_percussion");
            CheckBox[] usable = (CheckBox[])F(form, "_usableChannels");

            StringBuilder rows = new StringBuilder();
            string selected = "-";
            foreach (DataGridViewRow row in grid.Rows)
            {
                Color back = row.DefaultCellStyle.BackColor;
                int style = back == Color.MistyRose ? 1 : back == Color.LightCoral ? 2 : back == Color.LemonChiffon ? 3 : 0;
                TrackLaneKey lane = (TrackLaneKey)row.Tag;
                rows.Append(LaneTok(lane));
                for (int cell = 0; cell < 5; cell++)
                    rows.Append('|').Append(Convert.ToString(row.Cells[cell].Value, CultureInfo.CurrentCulture));
                rows.Append('|').Append(S(style)).Append('|').Append(row.Cells[1].ToolTipText).Append('\n');
            }
            if (grid.SelectedRows.Count != 0) selected = S(grid.SelectedRows[0].Index);
            StringBuilder used = new StringBuilder();
            StringBuilder usableBits = new StringBuilder();
            for (int channel = 0; channel < 16; channel++)
            {
                used.Append(B(form.ChannelCaptionBoldForTesting(channel)));
                usableBits.Append(B(usable[channel].Checked));
            }
            CompiledTrackRoute compiled = form.CompiledRouteForTesting;
            bool valid = compiled != null && compiled.IsValid;
            string preview = form.PreviewTextForTesting;

            StringBuilder line = new StringBuilder("S");
            line.Append('\t').Append(S(grid.Rows.Count));
            line.Append('\t').Append(Digest(rows.ToString()));
            line.Append('\t').Append(Digest(preview));
            line.Append('\t').Append(used);
            line.Append('\t').Append(usableBits);
            line.Append('\t').Append(S(tier.SelectedIndex)).Append(',').Append(S((int)capacity.Value)).Append(',')
                .Append(S(perc.SelectedIndex));
            line.Append('\t').Append(selected);
            line.Append('\t').Append(B(laneCombo.Enabled)).Append(',').Append(S(laneCombo.SelectedIndex));
            line.Append('\t').Append(B(valid)).Append(B(valid && compiled.RequiresExplicitApply))
                .Append(B(form.ApplyEnabledForTesting)).Append(B(status.ForeColor == Color.DarkRed))
                .Append(B(confirm.Checked)).Append(B(original.Enabled));
            line.Append('\t').Append(Esc(status.Text));
            line.Append('\t').Append(Esc(form.SelectedLaneCaptionForTesting));
            if (_full)
            {
                line.Append('\n').Append("R\t").Append(Esc(rows.ToString()));
                line.Append('\n').Append("P\t").Append(Esc(preview));
            }
            return line.ToString();
        }

        // ---------------------------------------------------------------- actions

        private static bool Act(TrackRoutingForm form, string action)
        {
            string[] f = action.Split(' ');
            DataGridView grid = (DataGridView)F(form, "_lanes");
            if (f[0] == "tier") form.SetMaximumTierForTesting((TrackRoutingTier)Int32.Parse(f[1], Inv));
            else if (f[0] == "cap") form.SetTargetCapacityForTesting(Int32.Parse(f[1], Inv));
            else if (f[0] == "perc") ((ComboBox)F(form, "_percussion")).SelectedIndex = Int32.Parse(f[1], Inv);
            else if (f[0] == "usable") form.SetUsableChannelForTesting(Int32.Parse(f[1], Inv), f[2] == "1");
            else if (f[0] == "select")
                form.SelectLaneForTesting((TrackLaneKey)grid.Rows[Int32.Parse(f[1], Inv)].Tag);
            else if (f[0] == "dest")
            {
                int index = Int32.Parse(f[1], Inv);
                form.SetSelectedLaneDestinationForTesting(index == 0 ? -1 : index - 1);
            }
            else if (f[0] == "confirm") form.SetLossyConfirmationForTesting(f[1] == "1");
            else if (f[0] == "apply") form.ConfirmApplyForTesting();
            else if (f[0] == "original")
            {
                // v1's UseOriginalRoutingForTesting uses Button.PerformClick, which does nothing on a form that
                // was never shown; raise the click directly (the same handler a real click runs).
                Button button = (Button)F(form, "_original");
                if (button.Enabled) button.GetType().GetMethod("OnClick", Hidden).Invoke(button, new object[] { EventArgs.Empty });
            }
            else throw new ArgumentException("unknown action " + action);
            return form.SelectedRoute != null || form.UseOriginalRouting;
        }

        private static string Outcome(TrackRoutingForm form)
        {
            string kind = form.UseOriginalRouting ? "original" : form.SelectedRoute != null ? "apply" : "none";
            return "O\t" + kind + "\t" + B(form.ExplicitlyConfirmed) + "\t" +
                (form.SelectedSettings == null ? "-" : SettingsText(form.SelectedSettings));
        }

        // Runs one session: the dialog is created from `initial`, then each action is applied.
        private static void Session(int number, MidiSong song, TrackRoutingSettings initial, bool active,
            string culture, List<string> actions)
        {
            CultureInfo ci = new CultureInfo(culture, false);
            Thread.CurrentThread.CurrentCulture = ci;
            Thread.CurrentThread.CurrentUICulture = ci;
            Line("session\t" + S(number) + "\tinit=" + SettingsText(initial) + "\tactive=" + B(active) +
                "\tculture=" + culture);
            TrackRoutingForm form;
            try { form = new TrackRoutingForm(song, initial, active, 100); }
            catch (Exception ex)
            {
                Line("X\t" + ex.GetType().Name + "\t" + Esc(ex.Message));
                Line("end");
                return;
            }
            using (form)
            {
                Line(State(form, initial));
                foreach (string action in actions)
                {
                    Line("A\t" + action);
                    bool done = Act(form, action);
                    Line(State(form, initial));
                    if (done)
                    {
                        Line(Outcome(form));
                        break;
                    }
                }
            }
            Line("end");
        }

        // Builds a random session's actions by reading the live dialog (row count, lossy state).
        private static void Walk(int number, MidiSong song, TrackRoutingSettings initial, bool active, string culture,
            int steps)
        {
            CultureInfo ci = new CultureInfo(culture, false);
            Thread.CurrentThread.CurrentCulture = ci;
            Thread.CurrentThread.CurrentUICulture = ci;
            Line("session\t" + S(number) + "\tinit=" + SettingsText(initial) + "\tactive=" + B(active) +
                "\tculture=" + culture);
            TrackRoutingForm form;
            try { form = new TrackRoutingForm(song, initial, active, 100); }
            catch (Exception ex)
            {
                Line("X\t" + ex.GetType().Name + "\t" + Esc(ex.Message));
                Line("end");
                return;
            }
            using (form)
            {
                Line(State(form, initial));
                for (int step = 0; step < steps; step++)
                {
                    string action;
                    int roll = Pick(100);
                    int rowCount = form.LaneRowCountForTesting;
                    CompiledTrackRoute compiled = form.CompiledRouteForTesting;
                    if (roll < 10) action = "tier " + S(Pick(3));
                    else if (roll < 20) action = "cap " + S(1 + Pick(16));
                    else if (roll < 26) action = "perc " + S(Pick(2));
                    else if (roll < 40) action = "usable " + S(Pick(16)) + " " + B(Pick(4) != 0);
                    else if (roll < 56 && rowCount != 0) action = "select " + S(Pick(rowCount));
                    else if (roll < 80 && rowCount != 0) action = "dest " + S(Pick(4) == 0 ? 0 : 1 + Pick(16));
                    else if (roll < 90) action = "confirm " + B(Pick(3) != 0);
                    else if (roll < 97) action = "apply";
                    else action = active ? "original" : "apply";
                    Line("A\t" + action);
                    bool done = Act(form, action);
                    Line(State(form, initial));
                    if (done)
                    {
                        Line(Outcome(form));
                        break;
                    }
                }
            }
            Line("end");
        }

        // ---------------------------------------------------------------- the session plan

        private static void Fixture(string dir, string name)
        {
            Thread.CurrentThread.CurrentCulture = new CultureInfo("en-US", false);
            Line("# routing-ui table for " + name + " (v1 TrackRoutingForm, see RoutingUiReference.cs)");
            Line("fixture\t" + name);
            MidiSong song = DirectCompactMidiParser.Load(Path.Combine(dir, name), CancellationToken.None, null);
            List<TrackLaneKey> lanes = new List<TrackLaneKey>();
            try
            {
                foreach (TrackRouteLaneProfile lane in TrackRoutePlanner.Compile(song, new TrackRoutingSettings()).Lanes)
                    lanes.Add(lane.Lane);
            }
            catch (InvalidOperationException) { }
            int number = 0;
            string en = "en-US";

            // Every tier x capacity x percussion policy from the defaults, a few ending in Apply.
            int[] capacities = { 1, 2, 3, 4, 8, 16 };
            int combo = 0;
            for (int tier = 0; tier < 3; tier++)
                foreach (int capacity in capacities)
                    for (int perc = 0; perc < 2; perc++)
                    {
                        List<string> actions = new List<string> { "tier " + S(tier), "cap " + S(capacity), "perc " + S(perc) };
                        if (combo % 3 == 0) actions.Add("apply");
                        else if (combo % 3 == 1) { actions.Add("confirm 1"); actions.Add("apply"); }
                        Session(++number, song, new TrackRoutingSettings(), false, en, actions);
                        combo++;
                    }

            // Usable channel sets under each tier.
            int[] masks = { 0xFFFF, 0xFDFF, 0x0200, 0x0000, 0x0E07, 0x0001, 0x8000, 0x00FF, 0xFF00 };
            foreach (int mask in masks)
                for (int tier = 0; tier < 3; tier += 2)
                {
                    List<string> actions = new List<string> { "tier " + S(tier) };
                    for (int channel = 0; channel < 16; channel++)
                        if ((mask & (1 << channel)) == 0) actions.Add("usable " + S(channel) + " 0");
                    actions.Add("confirm 1");
                    actions.Add("apply");
                    Session(++number, song, new TrackRoutingSettings(), false, en, actions);
                }

            // Manual destinations: every lane to every few channels, then back to automatic.
            if (lanes.Count != 0)
            {
                List<string> actions = new List<string>();
                int limit = Math.Min(lanes.Count, 6);
                for (int row = 0; row < limit; row++)
                {
                    actions.Add("select " + S(row));
                    actions.Add("dest " + S(1 + (row * 5) % 16));
                    actions.Add("dest " + S(1 + (row * 5 + 3) % 16));
                    if (row % 2 == 0) actions.Add("dest 0");
                }
                actions.Add("tier 2");
                actions.Add("confirm 1");
                actions.Add("apply");
                Session(++number, song, new TrackRoutingSettings(), false, en, actions);
            }

            // Initial settings (as MainForm hands the last applied settings back), with and without an active route.
            List<KeyValuePair<TrackLaneKey, int>> manual = new List<KeyValuePair<TrackLaneKey, int>>();
            for (int i = 0; i < lanes.Count && i < 3; i++)
                manual.Add(new KeyValuePair<TrackLaneKey, int>(lanes[i], (i * 7 + 2) % 16));
            Session(++number, song, MakeSettings(1, 4, 0, 0xFFFF, manual), true,
                en, new List<string> { "select 0", "dest 0", "original" });
            Session(++number, song, MakeSettings(2, 3, 1, 0x0E07, manual), true,
                en, new List<string> { "tier 0", "apply", "confirm 1", "apply" });
            Session(++number, song, MakeSettings(0, 0, 0, 0xFFFF, null), false, en, new List<string> { "cap 16", "cap 1", "original" });
            Session(++number, song, MakeSettings(1, 99, 0, 0xFFFF, null), false, en, new List<string> { "cap 16", "tier 1", "confirm 1", "apply" });
            Session(++number, song, MakeSettings(0, 16, 0, 0xFFFF, null), true, en, new List<string> { "original" });

            // The number formatting follows the current culture.
            Session(++number, song, MakeSettings(2, 4, 0, 0xFFFF, null), false, "de-DE", new List<string> { "tier 2", "confirm 1", "apply" });
            Session(++number, song, new TrackRoutingSettings(), false, "de-DE", new List<string> { "tier 1", "cap 2", "confirm 1", "apply" });

            // Random walks.
            for (int walk = 0; walk < 5; walk++)
            {
                TrackRoutingSettings initial = walk % 2 == 0 ? new TrackRoutingSettings()
                    : MakeSettings(Pick(3), 1 + Pick(16), Pick(2), Pick(3) == 0 ? (int)(Next() & 0xFFFF) : 0xFFFF, null);
                Walk(++number, song, initial, walk % 3 == 2, en, 40);
            }
            Line("# end of table");
        }
    }
}
