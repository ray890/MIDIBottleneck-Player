// UiReference, part 3: ScrubOrTypeTextBox (parsing, formatting, scripted gestures),
// the effective-speed / output-rate windows, and the help text.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class UiReference
    {
        private static void WriteControlTables()
        {
            // The text box measures a native combo box's colour on first use; on a console process that
            // measurement can yield a transparent colour the box then rejects. Seed the (colour-only)
            // palette with the documented fallback so the boxes can be constructed; no logic under test uses it.
            Type boxType = typeof(ScrubOrTypeTextBox);
            boxType.GetField("_enabledFieldColor", Hidden).SetValue(null, System.Drawing.SystemColors.Window);
            boxType.GetField("_disabledFieldColor", Hidden).SetValue(null, System.Drawing.SystemColors.Control);
            boxType.GetField("_fieldPaletteInitialized", Hidden).SetValue(null, true);
            WriteSpeedWindows();
            WriteParseTables();
            WriteScrubScripts();
        }

        // ---------------------------------------------------------------- effective speed / output rate

        private static void WriteSpeedWindows()
        {
            using (Table t = new Table("speed-window.txt"))
            {
                t.Comment("EffectivePlaybackSpeed.IsValidWindow, the WindowMicroseconds setter (E = threw), and UserPreferences.TryParseEffectiveSpeedWindow.");
                long[] values = { Int64.MinValue, -25000, -1, 0, 1, 24999, 25000, 25001, 100000, 250000, 500000, 1500000,
                    9999999, 10000000, 10000001, 20000000, Int64.MaxValue };
                for (int i = 0; i < values.Length; i++)
                {
                    EffectivePlaybackSpeed speed = new EffectivePlaybackSpeed();
                    string outcome;
                    try { speed.WindowMicroseconds = values[i]; outcome = "ok"; }
                    catch (ArgumentOutOfRangeException) { outcome = "E"; }
                    t.Row("window", I(values[i]), B(EffectivePlaybackSpeed.IsValidWindow(values[i])), outcome);
                }
                string[] texts = { "250000", " 250000 ", "250000\n", "0", "-0", "+25000", "25000.0", "1e5", "abc", "", "  ", "99999999999999999999",
                    "10000000", "10000001", "24999", U(0x662, 0x665, 0x660, 0x660, 0x660, 0x660) };
                for (int i = 0; i < texts.Length; i++)
                {
                    long parsed;
                    bool ok = UserPreferences.TryParseEffectiveSpeedWindow(texts[i], out parsed);
                    t.Row("parse", texts[i], B(ok), ok ? I(parsed) : "");
                }
            }

            long[] windows = { 0, 25000, 100000, 250000, 1500000, 10000000 };
            using (Table t = new Table("effective-speed.txt"))
            {
                t.Comment("EffectivePlaybackSpeed.Add scripts. 'window w' starts a script (a fresh object with that window);");
                t.Comment("'add clock intended frontier events synchronized sampleTime result(bits|null)'; 'set w' changes the window mid-script.");
                Prng r = new Prng(90210);
                for (int w = 0; w < windows.Length; w++)
                {
                    EffectivePlaybackSpeed speed = new EffectivePlaybackSpeed();
                    speed.WindowMicroseconds = windows[w];
                    t.Row("window", I(windows[w]));
                    long clock = 0, frontier = 0, events = 0, time = 5000000;
                    for (int i = 0; i < 200; i++)
                    {
                        if (i == 100 && w % 2 == 1)
                        {
                            long next = windows[(w + 2) % windows.Length];
                            speed.WindowMicroseconds = next;
                            t.Row("set", I(next));
                        }
                        long dt = r.Range(4000, 90000);
                        if (r.Chance(3)) dt = 0;
                        if (r.Chance(1)) dt = -2000;
                        time += dt;
                        double factor = new double[] { 0.5, 1.0, 1.0, 1.0, 2.0, 0.0, 1.25 }[r.Below(7)];
                        long advance = (long)(Math.Abs(dt) * factor);
                        clock += advance;
                        frontier += (long)(advance * (0.9 + r.Below(21) / 100.0));
                        events += r.Below(300);
                        if (r.Chance(2)) clock -= 100000;
                        if (r.Chance(2)) frontier -= 50000;
                        if (r.Chance(1)) events -= 50;
                        bool sync = r.Chance(30);
                        long intended = frontier + r.Range(-5000, 5000);
                        double? result = speed.Add(clock, intended, frontier, events, sync, time);
                        t.Row("add", I(clock), I(intended), I(frontier), I(events), B(sync), I(time), Hex(result));
                    }
                }
            }
            using (Table t = new Table("output-rate.txt"))
            {
                t.Comment("RollingOutputRate scripts. 'run' starts a fresh object; 'add events time result(bits|null) maximum(bits|null)';");
                t.Comment("'restart' = RestartWindow(); 'reset' = Reset().");
                Prng r = new Prng(31337);
                for (int run = 0; run < 3; run++)
                {
                    RollingOutputRate rate = new RollingOutputRate();
                    t.Row("run", I(run));
                    long events = 0, time = 1000000;
                    for (int i = 0; i < 240; i++)
                    {
                        if (i == 90) { rate.RestartWindow(); t.Row("restart"); }
                        if (i == 170 && run == 2) { rate.Reset(); t.Row("reset"); }
                        long dt = r.Range(5000, 70000);
                        if (r.Chance(3)) dt = 0;
                        if (r.Chance(1)) dt = -3000;
                        time += dt;
                        events += run == 0 ? r.Below(40) : run == 1 ? r.Below(900) : r.Below(3);
                        if (r.Chance(1)) events = Math.Max(0, events - 500);
                        double? result = rate.Add(events, time);
                        t.Row("add", I(events), I(time), Hex(result), Hex(rate.MaximumObserved));
                    }
                }
            }
        }

        // ---------------------------------------------------------------- ScrubOrTypeTextBox: parse and format

        private static readonly MethodInfo TryParseEditableMethod = typeof(ScrubOrTypeTextBox).GetMethod("TryParseEditable", Hidden);
        private static readonly MethodInfo FormatEditableMethod = typeof(ScrubOrTypeTextBox).GetMethod("FormatEditable", Hidden);

        private static ScrubOrTypeTextBox MakeBox(string kind)
        {
            ScrubOrTypeTextBox box = new ScrubOrTypeTextBox();
            if (kind == "numeric") box.ConfigureNumeric(1234, 0, 1000000, 1, 4, 1, 2);
            else if (kind == "channel") box.Configure(5, 0, 127, 1, 1, 4, 1, 8, false, false, null, null);
            else if (kind == "offset0") box.Configure(5, 0, 127, 0, 1, 4, 1, 8, false, false, null, null);
            else if (kind == "toggle") box.Configure(0, 0, 1, 0, 1, 1, 1, 1, false, false, null, null);
            else throw new ArgumentException(kind);
            return box;
        }

        // A string from UTF-16 code units (keeps this source file ASCII).
        internal static string U(params int[] codes)
        {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < codes.Length; i++) b.Append((char)codes[i]);
            return b.ToString();
        }

        private static List<string> ParseInputs()
        {
            List<string> list = new List<string>(new string[] {
                "", " ", "0", "1", "5", "-0", "-1", "+1", "+0", "007", "000000000000000000000012", "42", " 42", "42 ", "\t42\t",
                U(0xA0) + "42",
                "1,234", "1,2,3", "1,,2", ",1", "1,", "1,234,567", "1.234", "1.234.567", "1.", ".5", "1.5", "1,5", "12 34", "1 234",
                "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967296", "99999999999", "0000002147483647",
                "1234567890", "12345678901", "- 5", "+ 5", "--5", "-+5", "5-", "5+", "(5)", "-(5)", "1e3", "1E3", "0x10", "abc", "5a", "a5",
                "On", "Off", "on", "OFF", "oN", " on ", "onn", "1,000,000", "1000000", "1000001", "127", "128", "126", "-128",
                U(0x661, 0x662), U(0xFF11, 0xFF12), "1" + U(0xA0) + "234", "1" + U(0x202F) + "234", "5" + U(0x2212), U(0x2212) + "5",
                "1" + U(0x66C) + "234", U(0x200B) + "5", "5" + U(0x200B), U(0xFEFF) + "5",
                U(0x2003) + " 5 " + U(0x2003), U(0x85) + "5", "5" + U(0x2028), "1_000", U(0x661, 0x662, 0x663), "15" + U(0), U(0) + "15"
            });
            string alphabet = "0123456789012345678901234567890123456789,,..  --++aeOn()" + U(0xA0, 0x2212, 0x66C, 0x202F);
            Prng r = new Prng(55);
            for (int i = 0; i < 1400; i++)
            {
                int length = (int)r.Range(1, 9);
                StringBuilder b = new StringBuilder();
                for (int k = 0; k < length; k++) b.Append(alphabet[(int)r.Below(alphabet.Length)]);
                list.Add(b.ToString());
            }
            for (int i = 0; i < 300; i++)
            {
                // digit groups separated by commas / dots, sometimes signed
                StringBuilder b = new StringBuilder();
                if (r.Chance(15)) b.Append('-');
                int groups = (int)r.Range(1, 4);
                for (int g = 0; g < groups; g++)
                {
                    if (g > 0) b.Append(r.Chance(50) ? ',' : '.');
                    int digits = (int)r.Range(1, 4);
                    for (int d = 0; d < digits; d++) b.Append((char)('0' + r.Below(10)));
                }
                list.Add(b.ToString());
            }
            return list;
        }

        private static void WriteParseTables()
        {
            CultureInfo[] cultures = { EnUs, DeDe };
            List<string> inputs = ParseInputs();
            string[] kinds = { "numeric", "channel", "toggle" };
            int[] formatValues = { -2147483648, -1000, -5, -1, 0, 1, 5, 126, 127, 128, 1000, 123456, 2147483646, 2147483647 };
            for (int c = 0; c < cultures.Length; c++)
            {
                Thread.CurrentThread.CurrentCulture = cultures[c];
                using (Table t = new Table("scrub-parse-" + cultures[c].Name + ".txt"))
                {
                    t.Comment("ScrubOrTypeTextBox.TryParseEditable / FormatEditable / displayed text under " + cultures[c].Name + ".");
                    t.Comment("Rows: 'parse kind text ok value' (value is the out parameter, the box's value on failure);");
                    t.Comment("'format kind value text'; 'flat kind value text' (the box's displayed text after Value = value).");
                    t.Row("culture", cultures[c].Name, cultures[c].NumberFormat.NumberDecimalSeparator, cultures[c].NumberFormat.NumberGroupSeparator);
                    for (int k = 0; k < kinds.Length; k++)
                    {
                        ScrubOrTypeTextBox box = MakeBox(kinds[k]);
                        int count = k == 0 ? (c == 0 ? inputs.Count : 700) : 300;
                        for (int i = 0; i < count; i++)
                        {
                            object[] args = new object[] { inputs[i], 0 };
                            bool ok = (bool)TryParseEditableMethod.Invoke(box, args);
                            t.Row("parse", kinds[k], inputs[i], B(ok), I((int)args[1]));
                        }
                        for (int i = 0; i < formatValues.Length; i++)
                        {
                            string text = (string)FormatEditableMethod.Invoke(box, new object[] { formatValues[i] });
                            t.Row("format", kinds[k], I(formatValues[i]), text);
                        }
                        if (kinds[k] != "toggle")
                        {
                            int[] flat = { 0, 1, 5, 126, 127, 999, 1000, 1234, 12345, 123456, 1000000 };
                            for (int i = 0; i < flat.Length; i++)
                            {
                                box.Value = flat[i] > box.Maximum ? box.Maximum : flat[i];
                                t.Row("flat", kinds[k], I((int)box.Value), box.Text);
                            }
                        }
                        box.Dispose();
                    }
                }
            }
            Thread.CurrentThread.CurrentCulture = EnUs;
        }

        // ---------------------------------------------------------------- ScrubOrTypeTextBox: scripted gestures

        private sealed class ScrubScenario
        {
            internal string Name;
            internal string Kind;          // numeric | channel | toggle
            internal int Value, Minimum, Maximum, CoarseUnits, CoarsePixels, FineUnits, FinePixels, Offset, Increment;
            internal string Mapping;       // none | queue | age | rate0 | rate1 | rate2
            internal bool Forced;
        }

        private static Func<int, long, int> MappingByName(string name)
        {
            if (name == "queue") return MainForm.MapQueueScrubValue;
            if (name == "age") return MainForm.MapQueueAgeScrubValue;
            if (name == "rate0") return delegate(int s, long p) { return MainForm.MapRateScrubValue(ServiceDurationMode.ProcessingTime, s, p); };
            if (name == "rate1") return delegate(int s, long p) { return MainForm.MapRateScrubValue(ServiceDurationMode.MidiBitrate, s, p); };
            if (name == "rate2") return delegate(int s, long p) { return MainForm.MapRateScrubValue(ServiceDurationMode.EventsPerSecond, s, p); };
            return null;
        }

        private static void WriteScrubScripts()
        {
            ScrubScenario[] scenarios = new ScrubScenario[]
            {
                Scenario("queue", "numeric", 50, 1, 1000000, 1, 4, 1, 2, 0, 100, "queue"),
                Scenario("age", "numeric", 1000000, 1, 1800000000, 1, 4, 1, 2, 0, 1000, "age"),
                Scenario("rate-us", "numeric", 0, 0, 1000000, 16, 1, 1, 1, 0, 1, "rate0"),
                Scenario("rate-bitrate", "numeric", 31250, 1, 100000000, 16, 1, 1, 1, 0, 1, "rate1"),
                Scenario("rate-eps", "numeric", 100, 0, 9999999, 16, 1, 1, 1, 0, 1, "rate2"),
                Scenario("plain", "numeric", 10, 1, 128, 1, 4, 1, 2, 0, 1, "none"),
                Scenario("plain-wide", "numeric", 500, 0, 100000, 7, 3, 2, 5, 0, 10, "none"),
                Scenario("channel", "channel", 64, 0, 127, 1, 4, 1, 8, 1, 1, "none"),
                Scenario("channel-fast", "channel", 0, 0, 127, 3, 1, 1, 3, 0, 1, "none"),
                Scenario("toggle", "toggle", 0, 0, 1, 1, 1, 1, 1, 0, 1, "none")
            };
            string[] typed = { "5", "1,234", " 42 ", "999999999999", "-3", "0", "abc", "", "1,2,3", "+7", "1e3", "12 34", "1", "127", "128",
                "126", "100", "10", "On", "Off", "on", "1000000", "1000001", "1800000000", "1800000001", "128", "129", "31250", "99999999", "9999999", "10000000" };
            using (Table t = new Table("scrub-scripts.txt"))
            {
                t.Comment("ScrubOrTypeTextBox scripts on a real control. 'scenario name kind value min max coarseUnits coarsePixels fineUnits finePixels offset increment mapping'");
                t.Comment("starts a script; each op row ends with the control's state: value forced typing error text.");
                t.Comment("ops: scrub px fine | begin (StartScrubbing's state reset) | end (EndGesture) | key up|down | type | commit text explicit(1)|focusloss(0) | arrow up|down | escape | request value");
                for (int s = 0; s < scenarios.Length; s++)
                {
                    ScrubScenario sc = scenarios[s];
                    ScrubOrTypeTextBox box = new ScrubOrTypeTextBox();
                    if (sc.Kind == "numeric")
                    {
                        box.ConfigureNumeric(sc.Value, sc.Minimum, sc.Maximum, sc.CoarseUnits, sc.CoarsePixels, sc.FineUnits, sc.FinePixels);
                        box.SetNumericScrubMapping(MappingByName(sc.Mapping));
                        box.Increment = sc.Increment;
                        box.ThousandsSeparator = true;
                    }
                    else
                    {
                        box.Configure(sc.Value, sc.Minimum, sc.Maximum, sc.Offset, sc.CoarseUnits, sc.CoarsePixels, sc.FineUnits, sc.FinePixels,
                            sc.Forced, false, null, null);
                    }
                    box.ValueRequested += delegate(object o, ScrubValueEventArgs e) { };
                    t.Row("scenario", sc.Name, sc.Kind, I(sc.Value), I(sc.Minimum), I(sc.Maximum), I(sc.CoarseUnits), I(sc.CoarsePixels),
                        I(sc.FineUnits), I(sc.FinePixels), I(sc.Offset), I(sc.Increment), sc.Mapping);
                    Prng r = new Prng((ulong)(1000 + s));
                    for (int i = 0; i < 170; i++)
                    {
                        string op;
                        string[] args;
                        PickOp(r, box, typed, out op, out args);
                        ApplyOp(box, op, args);
                        List<string> row = new List<string>();
                        row.Add(op);
                        row.AddRange(args);
                        row.Add(I(box.CurrentValue));
                        row.Add(B(box.IsForced));
                        row.Add(B(box.IsTyping));
                        row.Add(box.LastError);
                        row.Add(box.Text);
                        t.Row(row.ToArray());
                    }
                    box.Dispose();
                }
            }
        }

        private static ScrubScenario Scenario(string name, string kind, int value, int min, int max, int cu, int cp, int fu, int fp,
            int offset, int increment, string mapping)
        {
            ScrubScenario s = new ScrubScenario();
            s.Name = name; s.Kind = kind; s.Value = value; s.Minimum = min; s.Maximum = max;
            s.CoarseUnits = cu; s.CoarsePixels = cp; s.FineUnits = fu; s.FinePixels = fp; s.Offset = offset;
            s.Increment = increment; s.Mapping = mapping;
            return s;
        }

        private static void PickOp(Prng r, ScrubOrTypeTextBox box, string[] typed, out string op, out string[] args)
        {
            long roll = r.Below(100);
            args = new string[0];
            if (box.IsTyping)
            {
                if (roll < 40) { op = "commit"; args = new string[] { typed[r.Below(typed.Length)], B(r.Chance(60)) }; }
                else if (roll < 70) { op = "arrow"; args = new string[] { r.Chance(50) ? "up" : "down" }; }
                else if (roll < 85) { op = "escape"; }
                else { op = "key"; args = new string[] { r.Chance(50) ? "up" : "down" }; }
                return;
            }
            if (roll < 62)
            {
                op = "scrub";
                long mag = r.Chance(70) ? 3 : r.Chance(60) ? 40 : 3000;
                long px = r.Range(-mag, mag);
                if (r.Chance(8)) px = 0;
                args = new string[] { I(px), B(r.Chance(30)) };
            }
            else if (roll < 72) op = "begin";
            else if (roll < 80) op = "end";
            else if (roll < 86) { op = "key"; args = new string[] { r.Chance(50) ? "up" : "down" }; }
            else if (roll < 94) op = "type";
            else if (roll < 97) { op = "request"; args = new string[] { I(r.Range(-5, 130)) }; }
            else op = "escape";
        }

        private static readonly FieldInfo PixelRemainder = typeof(ScrubOrTypeTextBox).GetField("_pixelRemainder", Hidden);
        private static readonly FieldInfo RemainderFine = typeof(ScrubOrTypeTextBox).GetField("_remainderFine", Hidden);
        private static readonly FieldInfo NumericScrubInitialized = typeof(ScrubOrTypeTextBox).GetField("_numericScrubInitialized", Hidden);
        private static readonly MethodInfo EndGestureMethod = typeof(ScrubOrTypeTextBox).GetMethod("EndGesture", Hidden);
        private static readonly MethodInfo RequestValueMethod = typeof(ScrubOrTypeTextBox).GetMethod("RequestValue", Hidden);
        private static readonly MethodInfo OnKeyDownMethod = typeof(ScrubOrTypeTextBox).GetMethod("OnKeyDown", Hidden);

        private static void ApplyOp(ScrubOrTypeTextBox box, string op, string[] args)
        {
            if (op == "scrub") box.ApplyScrubDeltaForTesting(Int32.Parse(args[0], CultureInfo.InvariantCulture), args[1] == "1");
            else if (op == "begin")
            {
                // StartScrubbing minus the capture and cursor calls.
                PixelRemainder.SetValue(box, 0);
                RemainderFine.SetValue(box, false);
                NumericScrubInitialized.SetValue(box, false);
            }
            else if (op == "end") EndGestureMethod.Invoke(box, new object[] { false });
            else if (op == "key")
                OnKeyDownMethod.Invoke(box, new object[] { new KeyEventArgs(args[0] == "up" ? Keys.Up : Keys.Down) });
            else if (op == "type") box.EnterTypingForTesting();
            else if (op == "commit")
            {
                if (args[1] == "1") box.CommitTextForTesting(args[0]); else box.CommitFocusLossForTesting(args[0]);
            }
            else if (op == "arrow") box.ArrowForTesting(args[0] == "up");
            else if (op == "escape") box.EscapeForTesting();
            else if (op == "request") RequestValueMethod.Invoke(box, new object[] { Int32.Parse(args[0], CultureInfo.InvariantCulture) });
            else throw new InvalidOperationException(op);
        }

        // ---------------------------------------------------------------- help

        private static readonly PlayerHelpTopic[] HelpTopics =
        {
            PlayerHelpTopic.Overview, PlayerHelpTopic.FileAndOutput, PlayerHelpTopic.RateModel, PlayerHelpTopic.QueueAndOverflow,
            PlayerHelpTopic.Playback, PlayerHelpTopic.Analysis, PlayerHelpTopic.Statistics, PlayerHelpTopic.Channels
        };

        private static readonly string[] HelpTitles =
        {
            "Overview", "File and output", "Rate model", "Queue and overflow", "Playback", "Analysis", "Statistics", "Channels"
        };

        private static void WriteHelpTables()
        {
            for (int i = 0; i < HelpTopics.Length; i++)
            {
                string text = (string)CallStatic(typeof(PlayerHelpForm), "TextForTopic", HelpTopics[i]);
                File.WriteAllText(Path.Combine(OutDir, "help-" + HelpTopics[i].ToString() + ".txt"), text, new UTF8Encoding(false));
            }
            // The list-box captions, read from the form's own list.
            using (Table t = new Table("help-titles.txt"))
            {
                t.Comment("PlayerHelpForm list-box captions in topic order (read from the form).");
                using (PlayerHelpForm form = new PlayerHelpForm())
                {
                    FieldInfo topicsField = typeof(PlayerHelpForm).GetField("_topics", Hidden);
                    ListBox list = (ListBox)topicsField.GetValue(form);
                    for (int i = 0; i < list.Items.Count; i++) t.Row(HelpTopics[i].ToString(), list.Items[i].ToString());
                }
            }
        }

        private static int EmitHelp(string path)
        {
            StringBuilder b = new StringBuilder();
            b.Append("// GENERATED by v2/harness/ui-reference (ui-reference.exe --emit-help) from the compiled v1 sources:\n");
            b.Append("// PlayerHelpForm.TextForTopic, byte for byte (UTF-8, \"\\r\\n\" line breaks). Do not edit by hand.\n");
            b.Append("#include \"midibn/app/help_text.h\"\n\nnamespace midibn {\nnamespace app {\nnamespace {\n\n");
            for (int i = 0; i < HelpTopics.Length; i++)
            {
                string text = (string)CallStatic(typeof(PlayerHelpForm), "TextForTopic", HelpTopics[i]);
                b.Append("const char kHelp").Append(HelpTopics[i].ToString()).Append("[] =\n");
                int start = 0;
                bool first = true;
                while (start < text.Length || first)
                {
                    int end = text.IndexOf("\r\n", start, StringComparison.Ordinal);
                    int next;
                    string piece;
                    if (end < 0) { piece = text.Substring(start); next = text.Length; }
                    else { piece = text.Substring(start, end - start) + "\r\n"; next = end + 2; }
                    b.Append("    \"").Append(CppEscape(piece)).Append("\"\n");
                    first = false;
                    start = next;
                    if (end < 0) break;
                }
                b.Append("    ;\n\n");
            }
            b.Append("} // namespace\n\n");
            b.Append("const char* help_topic_title(HelpTopic topic)\n{\n    switch (topic) {\n");
            for (int i = 0; i < HelpTopics.Length; i++)
                b.Append("    case HelpTopic::").Append(HelpTopics[i].ToString()).Append(": return \"").Append(HelpTitles[i]).Append("\";\n");
            b.Append("    }\n    return \"Overview\";\n}\n\n");
            b.Append("std::string help_text(HelpTopic topic)\n{\n    switch (topic) {\n");
            for (int i = 1; i < HelpTopics.Length; i++)
                b.Append("    case HelpTopic::").Append(HelpTopics[i].ToString()).Append(": return std::string(kHelp").Append(HelpTopics[i].ToString()).Append(", sizeof kHelp").Append(HelpTopics[i].ToString()).Append(" - 1);\n");
            b.Append("    case HelpTopic::Overview: break;\n    }\n    return std::string(kHelpOverview, sizeof kHelpOverview - 1);\n}\n\n");
            b.Append("} // namespace app\n} // namespace midibn\n");
            File.WriteAllText(path, b.ToString(), new UTF8Encoding(false));
            Console.WriteLine("Wrote " + path);
            return 0;
        }

        private static string CppEscape(string value)
        {
            StringBuilder b = new StringBuilder();
            byte[] utf8 = new UTF8Encoding(false).GetBytes(value);
            for (int i = 0; i < utf8.Length; i++)
            {
                byte c = utf8[i];
                if (c == (byte)'\\') b.Append("\\\\");
                else if (c == (byte)'"') b.Append("\\\"");
                else if (c == (byte)'\r') b.Append("\\r");
                else if (c == (byte)'\n') b.Append("\\n");
                else if (c == (byte)'\t') b.Append("\\t");
                else if (c < 0x20 || c > 0x7E)
                {
                    b.Append('\\');
                    b.Append((char)('0' + ((c >> 6) & 7)));
                    b.Append((char)('0' + ((c >> 3) & 7)));
                    b.Append((char)('0' + (c & 7)));
                    // keep a following digit out of the escape (MSVC warns C4125 otherwise)
                    if (i + 1 < utf8.Length && utf8[i + 1] >= (byte)'0' && utf8[i + 1] <= (byte)'9') b.Append("\" \"");
                }
                else b.Append((char)c);
            }
            return b.ToString();
        }
    }
}
