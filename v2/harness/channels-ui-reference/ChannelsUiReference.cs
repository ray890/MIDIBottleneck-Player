// v1 reference side of the MIDIBottleneck v2 Channels-window parity harness.
//
// Compiled together with the UNCHANGED v1.0.44 sources (src/*.cs except Program.cs).
// It creates v1's own ChannelMonitorForm and GeneralMidiProgramCatalog, feeds the form
// scripted ChannelPlaybackSnapshot objects, and writes plain-text tables to
// v2/harness/expected/channels-ui/ that v2/tests/channels_app_tests.cpp
// (midibn-channels-app-tests) compares byte for byte.
//
// Usage:  channels-ui-reference.exe <output-dir>               write every table
//         channels-ui-reference.exe --render <png-file> [state]   draw v1's real window (shown briefly)
//
// File format: UTF-8 without byte order mark, "\n" line ends; lines starting with '#'
// are comments; fields are separated by TAB; text fields are escaped (backslash, TAB,
// CR, LF and every character outside printable ASCII as \uXXXX). Long texts (tooltips,
// help) are written once to pool.txt and referenced by number.
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
    internal static partial class ChannelsUiReference
    {
        internal const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public |
            BindingFlags.Instance | BindingFlags.Static;

        internal static string OutDir;
        internal static readonly CultureInfo EnUs = new CultureInfo("en-US", false);
        internal static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        private static readonly string[] AttributeNames =
            { "BankMsb", "BankLsb", "Program", "Volume", "Expression", "Pan", "Sustain", "Bend", "Aftertouch" };

        private static int Main(string[] args)
        {
            try
            {
                Thread.CurrentThread.CurrentCulture = EnUs;
                Thread.CurrentThread.CurrentUICulture = EnUs;
                if (args.Length >= 2 && args[0] == "--render")
                    return Render(args[1], args.Length > 2 ? args[2] : "typical");
                if (args.Length != 1)
                {
                    Console.Error.WriteLine("usage: channels-ui-reference <output-dir> | --render <png-file> [state]");
                    return 2;
                }
                OutDir = args[0];
                Directory.CreateDirectory(OutDir);
                WriteCatalog();
                WriteTexts();
                WriteStates();
                WriteFlows();
                WritePool();
                long total = 0;
                foreach (string file in Directory.GetFiles(OutDir)) total += new FileInfo(file).Length;
                Console.WriteLine("Wrote tables to " + OutDir + " (" + total.ToString(Inv) + " bytes).");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("channels-ui-reference failure: " + ex);
                return 3;
            }
        }

        // ---------------------------------------------------------------- helpers

        internal sealed class Table : IDisposable
        {
            private readonly StringBuilder _text = new StringBuilder();
            private readonly string _path;
            internal Table(string name) { _path = Path.Combine(OutDir, name); }
            internal void Comment(string text) { _text.Append("# ").Append(text).Append('\n'); }
            internal void Row(params string[] fields)
            {
                for (int i = 0; i < fields.Length; i++)
                {
                    if (i > 0) _text.Append('\t');
                    _text.Append(Esc(fields[i]));
                }
                _text.Append('\n');
            }
            public void Dispose() { File.WriteAllText(_path, _text.ToString(), new UTF8Encoding(false)); }
        }

        internal static string Esc(string value)
        {
            if (value == null) return "";
            StringBuilder b = null;
            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                string piece = null;
                if (c == '\\') piece = "\\\\";
                else if (c == '\t') piece = "\\t";
                else if (c == '\r') piece = "\\r";
                else if (c == '\n') piece = "\\n";
                else if (c < 0x20 || c > 0x7E) piece = "\\u" + ((int)c).ToString("x4", Inv);
                if (piece != null && b == null)
                {
                    b = new StringBuilder();
                    b.Append(value, 0, i);
                }
                if (b != null) b.Append(piece != null ? piece : c.ToString());
            }
            return b == null ? value : b.ToString();
        }

        internal static string I(long value) { return value.ToString(Inv); }
        internal static string B(bool value) { return value ? "1" : "0"; }

        private static readonly List<string> PoolList = new List<string>();
        private static readonly Dictionary<string, int> PoolMap = new Dictionary<string, int>(StringComparer.Ordinal);
        private static string Pool(string text)
        {
            if (text == null) text = "";
            int id;
            if (!PoolMap.TryGetValue(text, out id))
            {
                id = PoolList.Count;
                PoolList.Add(text);
                PoolMap[text] = id;
            }
            return "@" + I(id);
        }

        private static void WritePool()
        {
            using (Table t = new Table("pool.txt"))
            {
                t.Comment("Long texts referenced as @<id> by the other tables.");
                for (int i = 0; i < PoolList.Count; i++) t.Row(I(i), PoolList[i]);
            }
        }

        internal sealed class Prng
        {
            private ulong _state;
            internal Prng(ulong seed) { _state = seed == 0 ? 0x9E3779B97F4A7C15UL : seed; }
            internal ulong Next()
            {
                _state ^= _state >> 12;
                _state ^= _state << 25;
                _state ^= _state >> 27;
                return _state * 2685821657736338717UL;
            }
            internal long Below(long n) { return (long)(Next() % (ulong)n); }
            internal long Range(long low, long high) { return low + Below(high - low + 1); }
            internal bool Chance(int percent) { return Below(100) < percent; }
            internal T Pick<T>(params T[] items) { return items[Below(items.Length)]; }
        }

        private static T Field<T>(object target, string name)
        {
            FieldInfo field = target.GetType().GetField(name, Hidden);
            if (field == null) throw new InvalidOperationException("missing field " + target.GetType().Name + "." + name);
            return (T)field.GetValue(target);
        }

        // The method called `name` whose parameters accept `args` (overloads such as HideEditor exist).
        private static MethodInfo FindMethod(Type type, string name, object[] args)
        {
            foreach (MethodInfo method in type.GetMethods(Hidden))
            {
                if (method.Name != name) continue;
                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Length != args.Length) continue;
                bool fits = true;
                for (int i = 0; i < parameters.Length && fits; i++)
                {
                    if (args[i] == null) fits = !parameters[i].ParameterType.IsValueType;
                    else fits = parameters[i].ParameterType.IsInstanceOfType(args[i]);
                }
                if (fits) return method;
            }
            throw new InvalidOperationException("missing method " + type.Name + "." + name);
        }

        private static object Call(object target, string name, params object[] args)
        {
            MethodInfo method = FindMethod(target.GetType(), name, args);
            try { return method.Invoke(target, args); }
            catch (TargetInvocationException ex) { throw ex.InnerException; }
        }

        private static object CallStatic(Type type, string name, params object[] args)
        {
            MethodInfo method = FindMethod(type, name, args);
            try { return method.Invoke(null, args); }
            catch (TargetInvocationException ex) { throw ex.InnerException; }
        }

        // The form is never shown: its Visible chain is made true so that "Visible" flags
        // (the detached overlay, the editor) read as they would on screen.
        private static bool _paletteFixed;

        private static ChannelMonitorForm NewForm(string fileName)
        {
            if (!_paletteFixed)
            {
                // ScrubOrTypeTextBox measures the native combo box colour by drawing it to a bitmap, which
                // gives a transparent colour (an exception) while no window is shown. v1's own fallback
                // is the system colours; setting them first changes no compared value.
                Type type = typeof(ScrubOrTypeTextBox);
                type.GetField("_enabledFieldColor", Hidden).SetValue(null, SystemColors.Window);
                type.GetField("_disabledFieldColor", Hidden).SetValue(null, SystemColors.Control);
                type.GetField("_fieldPaletteMeasured", Hidden).SetValue(null, false);
                type.GetField("_fieldPaletteInitialized", Hidden).SetValue(null, true);
                _paletteFixed = true;
            }
            ChannelMonitorForm form = new ChannelMonitorForm(fileName);
            MethodInfo setState = typeof(Control).GetMethod("SetState", Hidden, null, new Type[] { typeof(int), typeof(bool) }, null);
            if (setState == null) throw new InvalidOperationException("Control.SetState not found");
            setState.Invoke(form, new object[] { 0x2, true });
            return form;
        }

        private static bool VisibleFlag(Control control)
        {
            MethodInfo getState = typeof(Control).GetMethod("GetState", Hidden, null, new Type[] { typeof(int) }, null);
            return (bool)getState.Invoke(control, new object[] { 0x2 });
        }

        // ---------------------------------------------------------------- catalog

        private static void WriteCatalog()
        {
            using (Table t = new Table("catalog.txt"))
            {
                t.Comment("v1 GeneralMidiProgramCatalog: name <program> <text> | drum <channel> <msb> <lsb> <program> <name> <reference> | patch <msb> <lsb> <program> <text>");
                for (int p = -2; p <= 130; p++) t.Row("name", I(p), GeneralMidiProgramCatalog.Name(p));
                int[] msbs = { -1, 0, 1, 2, 63, 64, 119, 120, 121, 125, 126, 127 };
                int[] lsbs = { -1, 0, 1, 127 };
                int[] sparse = { -1, 0, 1, 8, 25, 56, 127, 128 };
                for (int m = 0; m < msbs.Length; m++)
                    for (int l = 0; l < lsbs.Length; l++)
                    {
                        List<int> programs = new List<int>();
                        if (lsbs[l] == 0) for (int p = -1; p <= 128; p++) programs.Add(p);
                        else programs.AddRange(sparse);
                        foreach (int p in programs)
                            t.Row("drum", "9", I(msbs[m]), I(lsbs[l]), I(p),
                                GeneralMidiProgramCatalog.ChannelProgramName(9, msbs[m], lsbs[l], p),
                                Pool(GeneralMidiProgramCatalog.ChannelProgramReference(9, msbs[m], lsbs[l], p)));
                    }
                int[] channels = { 0, 5, 8, 10, 15 };
                int[] melodicBanks = { -1, 0, 120, 127 };
                int[] melodicPrograms = { -1, 0, 25, 127, 128 };
                for (int c = 0; c < channels.Length; c++)
                    for (int m = 0; m < melodicBanks.Length; m++)
                        for (int q = 0; q < melodicPrograms.Length; q++)
                            t.Row("drum", I(channels[c]), I(melodicBanks[m]), "0", I(melodicPrograms[q]),
                                GeneralMidiProgramCatalog.ChannelProgramName(channels[c], melodicBanks[m], 0, melodicPrograms[q]),
                                Pool(GeneralMidiProgramCatalog.ChannelProgramReference(channels[c], melodicBanks[m], 0, melodicPrograms[q])));
                int[] patchPrograms = { -2, -1, 0, 1, 24, 127, 128, 129 };
                int[] patchBanks = { -1, 0, 1, 127 };
                int[] patchLsbs = { -1, 0, 1 };
                for (int m = 0; m < patchBanks.Length; m++)
                    for (int l = 0; l < patchLsbs.Length; l++)
                        for (int q = 0; q < patchPrograms.Length; q++)
                            t.Row("patch", I(patchBanks[m]), I(patchLsbs[l]), I(patchPrograms[q]),
                                GeneralMidiProgramCatalog.DescribePatch(new MidiPatchIdentity(patchBanks[m], patchLsbs[l], patchPrograms[q])));
            }
        }

        // ---------------------------------------------------------------- texts and columns

        private static void WriteTexts()
        {
            using (Table t = new Table("texts.txt"))
            {
                t.Comment("col <index> <name> <caption> <width> <tooltip> <alignment> <attribute|-> | explanation | title | seed | help | clock");
                using (ChannelMonitorForm form = NewForm("probe.mid"))
                {
                    DataGridView grid = form.GridForTesting;
                    t.Row("rows", I(form.ChannelRowCount), I(grid.Columns.Count));
                    for (int i = 0; i < grid.Columns.Count; i++)
                    {
                        DataGridViewColumn c = grid.Columns[i];
                        t.Row("col", I(i), c.Name, c.HeaderText, I(c.Width), Pool(c.HeaderCell.ToolTipText),
                            I((int)c.DefaultCellStyle.Alignment), c.Tag is ChannelAttribute ? I((int)(ChannelAttribute)c.Tag) : "-");
                    }
                    t.Row("explanation", Pool(Field<Label>(form, "_explanation").Text));
                    t.Row("title", "probe.mid", form.Text);
                    ChannelMonitorForm unnamed = NewForm(null);
                    t.Row("title", "(null)", unnamed.Text);
                    unnamed.Dispose();
                    ChannelMonitorForm named = NewForm("aé.mid");
                    t.Row("title", "aé.mid", named.Text);
                    named.Dispose();
                    t.Row("detached0", B(form.IsDetached));
                    form.DetachForSongReplacement(null);
                    t.Row("detach", "(null)", B(form.IsDetached), form.DetachedMessage, form.Text);
                    form.AttachSong("b.mid");
                    t.Row("attach", "b.mid", B(form.IsDetached), form.Text);
                    form.DetachForSongReplacement("Loading new MIDI…");
                    t.Row("detach", "Loading new MIDI…", B(form.IsDetached), form.DetachedMessage, form.Text);
                    form.AttachSong(null);
                    t.Row("attach", "(null)", B(form.IsDetached), form.Text);
                }
                for (int a = 0; a < 9; a++)
                {
                    ChannelAttribute attribute = (ChannelAttribute)a;
                    t.Row("seed", I(a), I((int)CallStatic(typeof(ChannelMonitorForm), "DefaultEditorSeed", attribute)));
                    int[] channels = { 0, 9 };
                    foreach (int ch in channels)
                        t.Row("help", I(a), I(ch), Pool((string)CallStatic(typeof(ChannelMonitorForm), "AttributeHelp", attribute, ch)));
                }
                long[] clocks = { -5, 0, 1, 999, 1000, 59999, 999999, 1000000, 59999999, 60000000, 61001000, 599999999, 600000000,
                    3599999999, 3600000000, 6000000000, 60000000000, 6000000000000, 100000000000000 };
                foreach (long us in clocks)
                    t.Row("clock", I(us), (string)CallStatic(typeof(ChannelMonitorForm), "FormatClock", us));
            }
        }

        // ---------------------------------------------------------------- scripted snapshots

        internal sealed class State
        {
            internal string Name;
            internal MidiChannelSnapshot[] Channels;
            internal MidiChannelSnapshot[] Readouts;   // null: no source readouts
        }

        private static MidiChannelSnapshot RandomChannel(Prng r, int channel)
        {
            MidiChannelSnapshot s = ChannelPlaybackSnapshot.Unknown(channel);
            s.Enabled = !r.Chance(25);
            s.KeysDown = r.Pick(0, 0, 1, 2, 17, 128, 1234, 99999);
            s.PeakKeysDown = s.KeysDown + r.Pick(0, 0, 3, 1000);
            s.SentEvents = r.Pick(0L, 1L, 999L, 1000L, 12345678L, 9876543210123L);
            s.DroppedEvents = r.Pick(0L, 0L, 5L, 1234567L);
            s.OverrideSuppressedEvents = r.Pick(0L, 0L, 12L, 7654321L);
            s.MutedFilteredEvents = r.Pick(0L, 3L, 1000L, 45678901L);
            bool drum = channel == 9;
            s.BankMsb = r.Chance(25) ? -1 : drum ? r.Pick(0, 0, 120, 126, 127, 5, 64) : r.Pick(0, 0, 1, 64, 127);
            s.BankLsb = r.Chance(25) ? -1 : r.Pick(0, 0, 0, 1, 32);
            s.Program = r.Chance(20) ? -1 : drum ? r.Pick(0, 1, 8, 16, 24, 25, 32, 40, 48, 56, 10, 127) : (int)r.Range(0, 127);
            s.Volume = r.Chance(20) ? -1 : r.Chance(5) ? -3 : (int)r.Range(0, 127);
            s.Expression = r.Chance(20) ? -1 : (int)r.Range(0, 127);
            s.Pan = r.Chance(20) ? -1 : (int)r.Range(0, 127);
            s.Sustain = r.Chance(20) ? -1 : r.Pick(0, 1, 127);
            s.PitchBend = r.Chance(20) ? Int32.MinValue : r.Chance(5) ? -9000 : r.Pick(0, 1, -1, 16, -8192, 8191, (int)r.Range(-8192, 8191));
            s.ChannelPressure = r.Chance(20) ? -1 : (int)r.Range(0, 127);
            s.LastDispatchedMicroseconds = r.Pick(-1L, 0L, 1L, 999L, 59999999L, 61234567L, 3599999999L, 36000000000L);
            s.LastPositionHistorical = r.Chance(30);
            s.HistoricalAttributeMask = r.Chance(40) ? (int)r.Range(0, 511) : 0;
            s.ForcedAttributeMask = r.Chance(35) ? (int)r.Range(0, 511) : 0;
            s.PendingForcedAttributeMask = r.Chance(40) ? (int)r.Range(0, 511) : 0;
            s.ForcedBankMsb = (int)r.Range(0, 127);
            if (drum && r.Chance(50)) s.ForcedBankMsb = r.Pick(0, 120, 126, 127);
            s.ForcedBankLsb = r.Pick(0, 0, 0, 1, 32);
            s.ForcedProgram = drum && r.Chance(50) ? r.Pick(0, 8, 16, 25, 48, 56, 3) : (int)r.Range(0, 127);
            s.ForcedVolume = (int)r.Range(0, 127);
            s.ForcedExpression = (int)r.Range(0, 127);
            s.ForcedPan = (int)r.Range(0, 127);
            s.ForcedSustain = r.Pick(0, 1);
            s.ForcedPitchBend = r.Pick(0, 1, -1, 16, -8192, 8191, (int)r.Range(-8192, 8191));
            s.ForcedAftertouch = (int)r.Range(0, 127);
            return s;
        }

        private static MidiChannelSnapshot RandomReadout(Prng r, int channel)
        {
            MidiChannelSnapshot s = ChannelPlaybackSnapshot.Unknown(channel);
            bool drum = channel == 9;
            if (!r.Chance(35)) s.BankMsb = drum ? r.Pick(0, 120, 126, 127, 7) : r.Pick(0, 1, 64);
            if (!r.Chance(35)) s.BankLsb = r.Pick(0, 0, 1, 32);
            if (!r.Chance(35)) s.Program = drum ? r.Pick(0, 8, 25, 48, 56, 2) : (int)r.Range(0, 127);
            if (!r.Chance(35)) s.Volume = (int)r.Range(0, 127);
            if (!r.Chance(35)) s.Expression = (int)r.Range(0, 127);
            if (!r.Chance(35)) s.Pan = (int)r.Range(0, 127);
            if (!r.Chance(35)) s.Sustain = r.Pick(0, 1);
            if (!r.Chance(35)) s.PitchBend = r.Pick(0, 5, -5, -8192, 8191);
            if (!r.Chance(35)) s.ChannelPressure = (int)r.Range(0, 127);
            return s;
        }

        internal static List<State> BuildStates()
        {
            List<State> states = new List<State>();
            State empty = new State { Name = "empty", Channels = ChannelPlaybackSnapshot.Empty().Channels };
            states.Add(empty);
            MidiChannelSnapshot[] fullReadouts = new MidiChannelSnapshot[16];
            Prng rr = new Prng(77);
            for (int i = 0; i < 16; i++)
            {
                fullReadouts[i] = ChannelPlaybackSnapshot.Unknown(i);
                fullReadouts[i].BankMsb = i == 9 ? 120 : 0;
                fullReadouts[i].BankLsb = 0;
                fullReadouts[i].Program = i == 9 ? 8 : i * 5;
                fullReadouts[i].Volume = 100;
                fullReadouts[i].Expression = 127;
                fullReadouts[i].Pan = 64;
                fullReadouts[i].Sustain = i % 2;
                fullReadouts[i].PitchBend = i == 3 ? -300 : 0;
                fullReadouts[i].ChannelPressure = 0;
            }
            states.Add(new State { Name = "empty+readouts", Channels = ChannelPlaybackSnapshot.Empty().Channels, Readouts = fullReadouts });
            Prng r = new Prng(20261014);
            for (int k = 0; k < 90; k++)
            {
                State s = new State { Name = "random" + k.ToString(Inv) };
                s.Channels = new MidiChannelSnapshot[16];
                for (int c = 0; c < 16; c++) s.Channels[c] = RandomChannel(r, c);
                if (r.Chance(50))
                {
                    s.Readouts = new MidiChannelSnapshot[16];
                    for (int c = 0; c < 16; c++) s.Readouts[c] = RandomReadout(r, c);
                }
                states.Add(s);
            }
            return states;
        }

        private static string ColorCode(Color c)
        {
            if (c.IsEmpty) return "N";
            if (c.ToArgb() == Color.FromArgb(20, 75, 155).ToArgb()) return "F";
            if (c.ToArgb() == Color.DimGray.ToArgb()) return "H";
            if (c.ToArgb() == SystemColors.ControlText.ToArgb()) return "N";
            return "?" + c.Name;
        }

        private static string FontCode(FontStyle s)
        {
            if (s == FontStyle.Regular) return "R";
            if (s == FontStyle.Bold) return "B";
            if (s == FontStyle.Italic) return "I";
            return "?" + ((int)s).ToString(Inv);
        }

        private static string RowBack(DataGridViewRow row)
        {
            Color c = row.DefaultCellStyle.BackColor;
            if (c.IsEmpty || c.ToArgb() == SystemColors.Window.ToArgb()) return "W";
            if (c.ToArgb() == SystemColors.Control.ToArgb()) return "C";
            return "?" + c.Name;
        }

        private static void ApplyState(ChannelMonitorForm form, State state)
        {
            form.SetSourceReadouts(state.Readouts == null ? null : new ChannelPlaybackSnapshot(state.Readouts));
            form.UpdateSnapshot(new ChannelPlaybackSnapshot(state.Channels));
        }

        private static void DumpCells(Table t, ChannelMonitorForm form, string label)
        {
            DataGridView grid = form.GridForTesting;
            for (int ch = 0; ch < 16; ch++)
            {
                List<string> fields = new List<string>();
                fields.Add("cells");
                fields.Add(label);
                fields.Add(I(ch));
                fields.Add(RowBack(grid.Rows[ch]));
                for (int col = 0; col < grid.Columns.Count; col++)
                {
                    string name = grid.Columns[col].Name;
                    fields.Add(form.CellText(ch, name));
                    fields.Add(ColorCode(form.CellForeColor(ch, name)) + FontCode(form.CellFontStyle(ch, name)));
                    fields.Add(Pool(grid.Rows[ch].Cells[col].ToolTipText));
                }
                t.Row(fields.ToArray());
            }
        }

        // state <label> <has readouts> | ch <label> <channel> 31 snapshot fields | ro <label> <channel> 9 readout fields
        internal static void DumpInputs(Table t, string label, State state)
        {
            t.Row("state", label, B(state.Readouts != null));
            for (int c = 0; c < 16; c++)
            {
                MidiChannelSnapshot s = state.Channels[c];
                t.Row("ch", label, I(c), I(s.Channel), I(s.KeysDown), I(s.PeakKeysDown), I(s.SentEvents), I(s.DroppedEvents),
                    I(s.OverrideSuppressedEvents), I(s.MutedFilteredEvents), B(s.Enabled), I(s.BankMsb), I(s.BankLsb), I(s.Program),
                    I(s.Volume), I(s.Expression), I(s.Pan), I(s.Sustain), I(s.PitchBend), I(s.ChannelPressure),
                    I(s.LastDispatchedMicroseconds), I(s.HistoricalAttributeMask), B(s.LastPositionHistorical),
                    I(s.ForcedAttributeMask), I(s.PendingForcedAttributeMask), I(s.ForcedBankMsb), I(s.ForcedBankLsb),
                    I(s.ForcedProgram), I(s.ForcedVolume), I(s.ForcedExpression), I(s.ForcedPan), I(s.ForcedSustain),
                    I(s.ForcedPitchBend), I(s.ForcedAftertouch));
            }
            if (state.Readouts != null)
                for (int c = 0; c < 16; c++)
                {
                    MidiChannelSnapshot s = state.Readouts[c];
                    t.Row("ro", label, I(c), I(s.BankMsb), I(s.BankLsb), I(s.Program), I(s.Volume), I(s.Expression), I(s.Pan),
                        I(s.Sustain), I(s.PitchBend), I(s.ChannelPressure));
                }
        }

        private static void WriteStates()
        {
            List<State> states = BuildStates();
            using (Table inputs = new Table("inputs.txt"))
            {
                inputs.Comment("Scripted snapshots: state <label> <has readouts> | ch <label> <channel> <MidiChannelSnapshot fields in declaration order> | ro <label> <channel> <bank msb> <bank lsb> <program> <volume> <expression> <pan> <sustain> <pitch bend> <pressure>");
                for (int s = 0; s < states.Count; s++) DumpInputs(inputs, I(s), states[s]);
            }
            using (Table t = new Table("cells.txt"))
            using (Table q = new Table("queries.txt"))
            using (Table e = new Table("editor-specs.txt"))
            {
                t.Comment("cells <state> <channel> <row background W|C> then per column: text, colour+font (N/F/H + R/B/I), tooltip pool id");
                q.Comment("attr <state> <channel> then per attribute: forced(0/1) historical(0/1) ':' CurrentAttributeValue | format <state> <channel> <attribute> then FormatAttribute(probe values)");
                e.Comment("spec <state> <channel> <attribute> min max offset coarseUnits coarsePixels fineUnits finePixels value forced historical text help editorColour+font");
                ChannelMonitorForm form = NewForm("states.mid");
                // The state before any snapshot: texts only.
                DumpCells(t, form, "initial");
                for (int s = 0; s < states.Count; s++)
                {
                    State state = states[s];
                    ApplyState(form, state);
                    DumpCells(t, form, I(s));
                    for (int ch = 0; ch < 16; ch++)
                    {
                        List<string> fields = new List<string>();
                        fields.Add("attr"); fields.Add(I(s)); fields.Add(I(ch));
                        for (int a = 0; a < 9; a++)
                        {
                            ChannelAttribute attribute = (ChannelAttribute)a;
                            bool forced = (bool)Call(form, "IsForced", ch, attribute);
                            bool historical = (bool)Call(form, "IsHistorical", ch, attribute);
                            int current = (int)Call(form, "CurrentAttributeValue", ch, attribute);
                            fields.Add(B(forced) + B(historical) + ":" + I(current));
                        }
                        q.Row(fields.ToArray());
                    }
                    if (s < 14)
                    {
                        int[] bendProbe = { Int32.MinValue, -8193, -8192, -17, -1, 0, 1, 16, 8191, 8192 };
                        int[] plainProbe = { Int32.MinValue, -5, -1, 0, 1, 24, 63, 64, 100, 127, 128 };
                        int[] channels = { 0, 9 };
                        foreach (int ch in channels)
                            for (int a = 0; a < 9; a++)
                            {
                                ChannelAttribute attribute = (ChannelAttribute)a;
                                int[] probe = attribute == ChannelAttribute.PitchBend ? bendProbe : plainProbe;
                                List<string> fields = new List<string>();
                                fields.Add("format"); fields.Add(I(s)); fields.Add(I(ch)); fields.Add(I(a));
                                foreach (int v in probe) fields.Add((string)Call(form, "FormatAttribute", ch, attribute, v));
                                q.Row(fields.ToArray());
                            }
                    }
                    if (s < 26)
                    {
                        int[] channels = { 0, 9, 15 };
                        foreach (int ch in channels)
                            for (int a = 0; a < 9; a++)
                            {
                                form.ActivateEditorForTesting(ch, AttributeNames[a]);
                                ScrubOrTypeTextBox ed = form.EditorForTesting;
                                ToolTip tip = Field<ToolTip>(ed, "_toolTip");
                                e.Row("spec", I(s), I(ch), I(a),
                                    I(Field<int>(ed, "_minimum")), I(Field<int>(ed, "_maximum")), I(Field<int>(ed, "_displayOffset")),
                                    I(Field<int>(ed, "_coarseUnits")), I(Field<int>(ed, "_coarsePixels")),
                                    I(Field<int>(ed, "_fineUnits")), I(Field<int>(ed, "_finePixels")),
                                    I(ed.CurrentValue), B(ed.IsForced), B(Field<bool>(ed, "_historical")),
                                    ed.Text, Pool(tip.GetToolTip(ed)),
                                    ColorCode(ed.ForeColor) + FontCode(ed.Font.Style));
                            }
                    }
                }
                // Detach clears the display; a later snapshot is ignored until AttachSong.
                form.DetachForSongReplacement("detached for the table");
                DumpCells(t, form, "detached");
                form.UpdateSnapshot(new ChannelPlaybackSnapshot(states[2].Channels));
                DumpCells(t, form, "detached-ignored");
                form.AttachSong("again.mid");
                form.UpdateSnapshot(new ChannelPlaybackSnapshot(states[2].Channels));
                DumpCells(t, form, "reattached");
                form.Dispose();
            }
        }
    }
}
