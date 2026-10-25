// v1 reference side of the MIDIBottleneck v2 UI-layer parity harness.
//
// Compiled together with the UNCHANGED v1.0.44 sources (src/*.cs except Program.cs)
// so it runs v1's own code (internal statics directly, private members through
// reflection). It writes plain-text tables to v2/harness/expected/ui/ that
// v2/tests/ui_tests.cpp (midibn-ui-tests) compares value by value.
//
// Usage:  ui-reference.exe <output-dir>              write every table
//         ui-reference.exe --emit-help <cpp-file>    write v2/core/src/app/help_text.cpp
//
// File format: UTF-8 without byte order mark, "\n" line ends; lines starting
// with '#' are comments; fields are separated by TAB; text fields are escaped
// (backslash, TAB, CR, LF and every character outside printable ASCII as \uXXXX).
// Tables whose inputs are a documented integer grid list only the outputs, one
// per line, in the loop order documented at the top of the file.
//
// This file: entry point, helpers, number formatting (N<d>, N0, 0.### ...), and
// the display-text tables. UiReference.Mappings.cs: slider/scrub/timeline/scale
// tables. UiReference.Controls.cs: ScrubOrTypeTextBox scripts, speed windows, help.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;

namespace MidiBottleneck
{
    internal static partial class UiReference
    {
        internal const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public |
            BindingFlags.Instance | BindingFlags.Static;

        internal static string OutDir;
        internal static readonly CultureInfo EnUs = new CultureInfo("en-US", false);
        internal static readonly CultureInfo DeDe = new CultureInfo("de-DE", false);

        private static int Main(string[] args)
        {
            try
            {
                if (args.Length == 2 && args[0] == "--emit-help") return EmitHelp(args[1]);
                if (args.Length != 1)
                {
                    Console.Error.WriteLine("usage: ui-reference <output-dir> | --emit-help <cpp-file>");
                    return 2;
                }
                OutDir = args[0];
                Directory.CreateDirectory(OutDir);
                Thread.CurrentThread.CurrentCulture = EnUs;
                Thread.CurrentThread.CurrentUICulture = EnUs;
                WriteNumberFormats();
                WriteDisplayTables();
                WriteMappingTables();
                WriteControlTables();
                WriteHelpTables();
                long total = 0;
                foreach (string file in Directory.GetFiles(OutDir))
                {
                    total += new FileInfo(file).Length;
                }
                Console.WriteLine("Wrote tables to " + OutDir + " (" + total.ToString(CultureInfo.InvariantCulture) + " bytes).");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("ui-reference failure: " + ex);
                return 3;
            }
        }

        // ---------------------------------------------------------------- helpers

        internal sealed class Table : IDisposable
        {
            private readonly StringBuilder _text = new StringBuilder();
            private readonly string _path;

            internal Table(string name) { _path = Path.Combine(OutDir, name); }

            internal void Comment(string text)
            {
                _text.Append("# ").Append(text).Append('\n');
            }

            internal void Row(params string[] fields)
            {
                for (int i = 0; i < fields.Length; i++)
                {
                    if (i > 0) _text.Append('\t');
                    _text.Append(Esc(fields[i]));
                }
                _text.Append('\n');
            }

            internal void Raw(string value) { _text.Append(value).Append('\n'); }

            public void Dispose()
            {
                File.WriteAllText(_path, _text.ToString(), new UTF8Encoding(false));
            }
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
                else if (c < 0x20 || c > 0x7E) piece = "\\u" + ((int)c).ToString("x4", CultureInfo.InvariantCulture);
                if (piece != null && b == null)
                {
                    b = new StringBuilder();
                    b.Append(value, 0, i);
                }
                if (b != null) b.Append(piece != null ? piece : c.ToString());
            }
            return b == null ? value : b.ToString();
        }

        internal static string I(long value) { return value.ToString(CultureInfo.InvariantCulture); }
        internal static string B(bool value) { return value ? "1" : "0"; }
        internal static string Hex(double value)
        {
            return BitConverter.DoubleToInt64Bits(value).ToString("x16", CultureInfo.InvariantCulture);
        }
        internal static string Hex(double? value) { return value.HasValue ? Hex(value.Value) : "null"; }
        internal static double FromHex(string text)
        {
            return BitConverter.Int64BitsToDouble(Int64.Parse(text, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture));
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
            // 0 <= result < n
            internal long Below(long n) { return (long)(Next() % (ulong)n); }
            internal long Range(long low, long high) { return low + Below(high - low + 1); }
            internal bool Chance(int percent) { return Below(100) < percent; }
        }

        internal static object CallStatic(Type type, string name, params object[] args)
        {
            MethodInfo method = type.GetMethod(name, Hidden);
            if (method == null) throw new InvalidOperationException("missing method " + type.Name + "." + name);
            try { return method.Invoke(null, args); }
            catch (TargetInvocationException ex) { throw ex.InnerException; }
        }

        // ---------------------------------------------------------------- number formatting

        private static readonly int[] NDecimals = { 0, 1, 2, 3, 4 };
        private static readonly int[] TrimmedPlaces = { 1, 3 };

        private static readonly double[] SpecialDoubles =
        {
            0.0, 0.05, 0.005, 0.0005, 0.00005, 0.5, 1.5, 2.5, 3.5, 0.25, 0.125, 0.375, 1.005, 2.675, 0.045, 0.015, 8.345,
            0.9995, 0.99999, 0.999999, 9.995, 99.95, 999.5, 999.95, 9999.5, 99999.5, 999999.5, 9999999.5,
            9999999.95, 9999999.995, 1e15, 1e15 + 1, 1e15 - 1, 999999999999999.0, 999999999999999.5, 99999999999999.9,
            99999999999999.95, 9999999999999.995, 123456789012345.0, 1234567890123456.0, 12345678901234567.0,
            123456789012345678.0, 1234567890123.125, 1234567890123.375, 4503599627370.125, 4503599627370.5,
            0.1 + 0.2, 1.0 / 3.0, 2.0 / 3.0, 1e-5, 1e-10, 1e-15, 1e-20, 0.000123456, 0.0001234565, 0.0009995,
            100.0, 1000.0, 1234.5, 12345.678, 123456.789, 1234567.891, 999.9999, 99.99999, 9.999999, 0.99999999,
            1.0, 2.0, 10.0, 99.0, 100.5, 249.5, 250.5, 2.5e6, 1.5e9, 4294967295.5, 2147483647.5, 9007199254740992.0,
            9007199254740993.0, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22, 1e23, 1e25, 1e30, 3.141592653589793,
            2.718281828459045, 1234.56789012345678, 0.30000000000000004, 0.1, 0.7, 1.1, 1.15, 1.25, 1.35, 1.45,
            5e-324, 2.2250738585072014e-308, 1e-300, 1.7976931348623157e308, 4.35, 4.45, 0.35, 0.45, 0.55, 0.65, 0.95
        };

        // The format_n / format_trimmed input set: SpecialDoubles with both signs, then
        // generated values in documented classes. Fully determined by the seed.
        private static List<double> BuildFormatInputs()
        {
            List<double> list = new List<double>();
            for (int i = 0; i < SpecialDoubles.Length; i++)
            {
                list.Add(SpecialDoubles[i]);
                if (SpecialDoubles[i] != 0.0 && Math.Abs(SpecialDoubles[i]) < 1e29) list.Add(-SpecialDoubles[i]);
            }
            Prng r = new Prng(20261014);
            double[] p10 = new double[23];
            p10[0] = 1.0;
            for (int i = 1; i < p10.Length; i++) p10[i] = p10[i - 1] * 10.0;
            // k / 10^p : decimal-looking inputs of 1..17 digits (the classic 0.05, 1.005, 2.675 family)
            for (int i = 0; i < 300; i++)
            {
                int digits = (int)r.Range(1, 17);
                long limit = 1;
                for (int d = 0; d < digits; d++) limit *= 10;
                long k = r.Range(1, limit - 1);
                int p = (int)r.Range(0, 12);
                double v = (double)k / p10[p];
                list.Add(r.Chance(25) ? -v : v);
            }
            // halves: (k + 0.5) / 10^p, aimed at the rounding position of N0..N6
            for (int i = 0; i < 180; i++)
            {
                long k = r.Range(0, 99999999);
                int p = (int)r.Range(0, 7);
                double v = (k + 0.5) / p10[p];
                list.Add(r.Chance(25) ? -v : v);
                double w = (double)(k * 10 + 5) / p10[p + 1];
                list.Add(r.Chance(25) ? -w : w);
            }
            // m / 2^j : exact binary fractions; many are exact ties or 16-digit values at the 15-digit edge
            for (int i = 0; i < 200; i++)
            {
                long m = (long)(r.Next() >> 11);
                m |= 1L << 50;
                int j = (int)r.Range(1, 30);
                double v = (double)m / (double)(1L << j);
                list.Add(r.Chance(25) ? -v : v);
            }
            // 15-16 digit boundaries: 9999..9 / 10^p and 1000..0 + 5 / 10^p
            for (int i = 0; i < 100; i++)
            {
                int digits = (int)r.Range(13, 17);
                long nines = 1;
                for (int d = 0; d < digits; d++) nines *= 10;
                long a = nines - 1 - r.Range(0, 20);
                long b = nines / 10 + r.Range(0, 20) * 5;
                int p = (int)r.Range(0, 9);
                list.Add((double)a / p10[p]);
                list.Add(-(double)b / p10[p]);
            }
            // random bit patterns with exponents 2^-70 .. 2^90 (finite, both signs)
            for (int i = 0; i < 200; i++)
            {
                ulong bits = r.Next();
                ulong exponent = (ulong)(1023 + r.Range(-70, 90));
                bits = (bits & 0x800FFFFFFFFFFFFFUL) | (exponent << 52);
                list.Add(BitConverter.Int64BitsToDouble((long)bits));
            }
            // large: k * 10^q
            for (int i = 0; i < 100; i++)
            {
                long k = r.Range(1, 99999999999L);
                int q = (int)r.Range(0, 20);
                double v = (double)k * p10[q];
                list.Add(r.Chance(25) ? -v : v);
            }
            // tiny: k / 10^p with large p
            for (int i = 0; i < 100; i++)
            {
                long k = r.Range(1, 9999999);
                int p = (int)r.Range(7, 22);
                double v = (double)k / p10[p];
                list.Add(r.Chance(25) ? -v : v);
            }
            // The quantities the player really shows: rates, ms, GiB, percent.
            for (int i = 0; i < 120; i++)
            {
                long n = r.Range(0, 99999999);
                list.Add(n / 1000.0);
                list.Add(n / 1048576.0);
                list.Add(1000000.0 / (n + 1));
                list.Add(n / 10.0);
            }
            return list;
        }

        private static string[] NCells(double value, CultureInfo culture, int[] decimals, int[] trimmed)
        {
            Thread.CurrentThread.CurrentCulture = culture;
            List<string> cells = new List<string>();
            for (int i = 0; i < decimals.Length; i++)
                cells.Add(value.ToString("N" + decimals[i].ToString(CultureInfo.InvariantCulture), CultureInfo.CurrentCulture));
            for (int i = 0; i < trimmed.Length; i++)
                cells.Add(value.ToString("0." + new string('#', trimmed[i]), CultureInfo.CurrentCulture));
            return cells.ToArray();
        }

        private static void WriteNumberFormats()
        {
            List<double> inputs = BuildFormatInputs();
            CultureInfo[] cultures = { EnUs, DeDe };
            for (int c = 0; c < cultures.Length; c++)
            {
                CultureInfo culture = cultures[c];
                bool full = c == 0;
                int[] decimals = full ? NDecimals : new int[] { 0, 1, 3 };
                int[] trimmed = full ? TrimmedPlaces : new int[] { 3 };
                int count = full ? inputs.Count : 700;
                using (Table table = new Table("format-n-" + culture.Name + ".txt"))
                {
                    table.Comment("double.ToString(\"N<d>\") and the custom format \"0.<#...>\" under " + culture.Name + ".");
                    table.Comment("Row: HEX(double bits) then one result per column; columns as listed in the 'columns' row.");
                    table.Row("culture", culture.Name, culture.NumberFormat.NumberDecimalSeparator, culture.NumberFormat.NumberGroupSeparator);
                    List<string> columns = new List<string>();
                    columns.Add("columns");
                    for (int i = 0; i < decimals.Length; i++) columns.Add("N" + I(decimals[i]));
                    for (int i = 0; i < trimmed.Length; i++) columns.Add("T" + I(trimmed[i]));
                    table.Row(columns.ToArray());
                    for (int i = 0; i < count; i++)
                    {
                        List<string> row = new List<string>();
                        row.Add(Hex(inputs[i]));
                        row.AddRange(NCells(inputs[i], culture, decimals, trimmed));
                        table.Row(row.ToArray());
                    }
                }
            }
            Thread.CurrentThread.CurrentCulture = EnUs;

            // long.ToString("N0") and int.ToString() under both cultures
            using (Table table = new Table("format-n0.txt"))
            {
                table.Comment("long.ToString(\"N0\") under en-US and de-DE. Row: value, en-US result, de-DE result.");
                List<long> values = new List<long>();
                long[] fixedValues = { 0, 1, -1, 9, 10, 99, 100, 999, 1000, -999, -1000, 1234, 12345, 123456, 1234567,
                    999999, 1000000, -1234567, 2147483647, -2147483648L, 2147483648L, 4294967295L, 4294967296L,
                    99999999999L, 1000000000000L, 9223372036854775807L, -9223372036854775807L - 1, 1073741824L, 1048576L };
                values.AddRange(fixedValues);
                Prng r = new Prng(777);
                for (int i = 0; i < 400; i++)
                {
                    int digits = (int)r.Range(1, 19);
                    long limit = 1;
                    for (int d = 0; d < digits - 1; d++) limit *= 10;
                    long v = limit + r.Below(limit * 9 > 0 ? limit * 9 : long.MaxValue / 2);
                    values.Add(r.Chance(30) ? -v : v);
                }
                for (int i = 0; i < values.Count; i++)
                {
                    Thread.CurrentThread.CurrentCulture = EnUs;
                    string en = values[i].ToString("N0", CultureInfo.CurrentCulture);
                    Thread.CurrentThread.CurrentCulture = DeDe;
                    string de = values[i].ToString("N0", CultureInfo.CurrentCulture);
                    table.Row(I(values[i]), en, de);
                }
                Thread.CurrentThread.CurrentCulture = EnUs;
            }
        }

        // ---------------------------------------------------------------- display tables

        private static PlaybackSnapshot RandomSnapshot(Prng r)
        {
            PlaybackSnapshot s = new PlaybackSnapshot();
            s.State = (PlaybackState)r.Range(0, 3);
            s.ProcessedEvents = RandomCount(r);
            s.DroppedEvents = RandomCount(r) / (r.Chance(50) ? 1 : 1000);
            s.GateFilteredEvents = r.Chance(40) ? RandomCount(r) / 100 : 0;
            s.MusicalLimitRejectedAttacks = r.Chance(30) ? RandomCount(r) / 100 : 0;
            s.MusicalLimitSuppressedReleases = r.Chance(30) ? RandomCount(r) / 100 : 0;
            s.OutputRateRejectedAttacks = r.Chance(30) ? RandomCount(r) / 100 : 0;
            s.OutputRateSuppressedReleases = r.Chance(30) ? RandomCount(r) / 100 : 0;
            s.OutputRateRejectedOtherMessages = r.Chance(30) ? RandomCount(r) / 1000 : 0;
            s.OutputRateSafetySoftExcess = r.Chance(40) ? RandomCount(r) / 1000 : 0;
            if (r.Chance(5)) s.DroppedEvents = Int64.MaxValue - r.Range(0, 5);
            if (r.Chance(5)) s.GateFilteredEvents = Int64.MaxValue - r.Range(0, 5);
            if (r.Chance(5)) s.MusicalLimitRejectedAttacks = -r.Range(1, 9);
            s.VirtualQueueActive = r.Chance(40);
            s.SimulateSlowdown = r.Chance(50);
            s.ServiceDurationMode = (ServiceDurationMode)r.Range(0, 2);
            s.ProcessingMicroseconds = r.Chance(20) ? 0 : RandomCount(r) % 2000000 - (r.Chance(5) ? 100 : 0);
            s.MidiBitrate = r.Chance(30) ? 31250 : RandomCount(r) % 100000001;
            s.EventsPerSecond = r.Chance(20) ? 0 : RandomCount(r) % 10000000;
            s.ProcessingMode = (ProcessingMode)r.Range(0, 5);
            s.OutstandingEvents = RandomCount(r) / 100;
            s.MaximumQueueLength = RandomCount(r) / 100;
            s.IntendedTimelineMicroseconds = RandomCount(r) * 1000;
            s.LastDispatchedTimelineMicroseconds = RandomCount(r) * 1000;
            s.CurrentLagMicroseconds = RandomCount(r) % 5000000 - (r.Chance(5) ? 1000 : 0);
            s.MaximumLagMicroseconds = RandomCount(r) % 50000000;
            return s;
        }

        private static long RandomCount(Prng r)
        {
            int digits = (int)r.Range(1, 12);
            long limit = 1;
            for (int d = 0; d < digits; d++) limit *= 10;
            return r.Below(limit);
        }

        // The snapshot fields the display functions read, as comma-separated integers in this order:
        // State, ProcessedEvents, DroppedEvents, GateFilteredEvents, MusicalLimitRejectedAttacks,
        // MusicalLimitSuppressedReleases, OutputRateRejectedAttacks, OutputRateSuppressedReleases,
        // OutputRateRejectedOtherMessages, OutputRateSafetySoftExcess, VirtualQueueActive, SimulateSlowdown,
        // ServiceDurationMode, ProcessingMicroseconds, MidiBitrate, EventsPerSecond, ProcessingMode,
        // OutstandingEvents, MaximumQueueLength, IntendedTimelineMicroseconds, LastDispatchedTimelineMicroseconds,
        // CurrentLagMicroseconds, MaximumLagMicroseconds.
        private static string SnapText(PlaybackSnapshot s)
        {
            return I((int)s.State) + "," + I(s.ProcessedEvents) + "," + I(s.DroppedEvents) + "," + I(s.GateFilteredEvents) + "," +
                I(s.MusicalLimitRejectedAttacks) + "," + I(s.MusicalLimitSuppressedReleases) + "," +
                I(s.OutputRateRejectedAttacks) + "," + I(s.OutputRateSuppressedReleases) + "," +
                I(s.OutputRateRejectedOtherMessages) + "," + I(s.OutputRateSafetySoftExcess) + "," +
                B(s.VirtualQueueActive) + "," + B(s.SimulateSlowdown) + "," + I((int)s.ServiceDurationMode) + "," +
                I(s.ProcessingMicroseconds) + "," + I(s.MidiBitrate) + "," + I(s.EventsPerSecond) + "," +
                I((int)s.ProcessingMode) + "," + I(s.OutstandingEvents) + "," + I(s.MaximumQueueLength) + "," +
                I(s.IntendedTimelineMicroseconds) + "," + I(s.LastDispatchedTimelineMicroseconds) + "," +
                I(s.CurrentLagMicroseconds) + "," + I(s.MaximumLagMicroseconds);
        }

        private static int _snapshotCount;

        // Writes a 'snap' row for the snapshot and returns its id for the rows that use it.
        private static string SnapRef(Table table, PlaybackSnapshot s)
        {
            string id = "s" + I(_snapshotCount++);
            table.Row("snap", id, SnapText(s));
            return id;
        }

        private static double? RandomOptionalRate(Prng r)
        {
            int kind = (int)r.Range(0, 9);
            if (kind == 0) return null;
            if (kind == 1) return Double.NaN;
            if (kind == 2) return Double.PositiveInfinity;
            if (kind == 3) return Double.NegativeInfinity;
            if (kind == 4) return 0.0;
            double mag = Math.Pow(10.0, r.Range(-2, 8));
            double v = (r.Below(1000000) / 1000000.0) * mag * 10.0;
            if (r.Chance(10)) v = Math.Round(v) + 0.5;
            return r.Chance(5) ? -v : v;
        }

        private static long MaybeBig(Prng r)
        {
            long v = RandomCount(r);
            if (r.Chance(8)) v = -v;
            return v;
        }

        private static readonly Type MainFormType = typeof(MainForm);
        private static readonly Type StatisticsViewType = typeof(StatisticsView);

        private static void WriteDisplayTables()
        {
            CultureInfo[] cultures = { EnUs, DeDe };
            for (int c = 0; c < cultures.Length; c++)
            {
                Thread.CurrentThread.CurrentCulture = cultures[c];
                using (Table table = new Table("display-" + cultures[c].Name + ".txt"))
                {
                    table.Comment("Display strings under " + cultures[c].Name + ". Row: function, inputs..., result (last field(s)).");
                    table.Comment("snap = PlaybackSnapshot fields as Name=Value;... ; bits = double bit pattern in hex or 'null'.");
                    table.Row("culture", cultures[c].Name, cultures[c].NumberFormat.NumberDecimalSeparator, cultures[c].NumberFormat.NumberGroupSeparator);
                    DisplayRows(table, c == 0);
                }
            }
            Thread.CurrentThread.CurrentCulture = EnUs;
        }

        private static void DisplayRows(Table table, bool primary)
        {
            _snapshotCount = 0;
            Prng r = new Prng(4242);
            int n = primary ? 45 : 20;
            // --- times and durations
            long[] fixedMicro = { -5, 0, 1, 999, 1000, 1001, 1234, 59999999, 60000000, 61000001, 599999999, 3599999999L,
                3600000000L, 3600000001L, 86399999999L, 1234567890123L, 9999999999999L, 92233720368547758L, 92233720368547759L,
                Int64.MaxValue / 10 + 1 };
            List<long> micro = new List<long>(fixedMicro);
            for (int i = 0; i < n; i++) micro.Add(r.Chance(10) ? -RandomCount(r) : RandomCount(r) * (r.Chance(50) ? 1 : 1000));
            for (int i = 0; i < micro.Count; i++)
            {
                long us = micro[i];
                if (primary) table.Row("time", I(us), (string)CallStatic(MainFormType, "FormatTime", us));
                table.Row("duration", I(us), (string)CallStatic(MainFormType, "FormatDuration", us));
                table.Row("lag", I(us), MainForm.FormatLagMilliseconds(us));
                table.Row("delayednoteoff", I(us), B(false), MainForm.FormatDelayedNoteOffDuration(us, false));
                table.Row("delayednoteoff", I(us), B(true), MainForm.FormatDelayedNoteOffDuration(us, true));
                table.Row("pernote", I(us), B(false), MainForm.FormatPerNoteFrameRate(us, false));
                table.Row("pernote", I(us), B(true), MainForm.FormatPerNoteFrameRate(us, true));
                table.Row("window", I(us), EffectivePlaybackSpeed.DescribeWindow(us));
                table.Row("bytes", I(us), LargeMidiWarningDialog.FormatBytes(us));
                table.Row("pressurenumber", I(us), (string)CallStatic(StatisticsViewType, "FormatCompactPressureNumber", us));
            }
            long[] bytes = { 0, 1, 1048575, 1048576, 1572864, 1073741823L, 1073741824L, 1610612736L, 10737418239L, 10737418240L,
                10737418241L, 1099511627776L, 8589934592L, Int64.MaxValue, -5 };
            for (int i = 0; i < bytes.Length; i++)
            {
                table.Row("bytes", I(bytes[i]), LargeMidiWarningDialog.FormatBytes(bytes[i]));
                table.Row("telemetry", I(65 + i * 400), I(bytes[i]), B(false), MainForm.FormatLoadingTelemetry(65 + i * 400, bytes[i], false));
                table.Row("telemetry", I(3599 + i), I(bytes[i]), B(true), MainForm.FormatLoadingTelemetry(3599 + i, bytes[i], true));
            }
            long[] pressureNumbers = { 0, 1, 999, 1000, 1049, 1050, 1099, 1500, 999499, 999500, 999949, 999950, 1000000, 1049999,
                1050000, 12345678, 999999999, 1000000000, 2500000000L, 1800000000, -5, -1000, -1500000, -2000000000L };
            for (int i = 0; i < pressureNumbers.Length; i++)
                table.Row("pressurenumber", I(pressureNumbers[i]), (string)CallStatic(StatisticsViewType, "FormatCompactPressureNumber", pressureNumbers[i]));

            // --- loading telemetry
            for (int i = 0; i < n; i++)
            {
                long seconds = r.Chance(30) ? r.Range(0, 7200) : RandomCount(r) % 4000000;
                long privateBytes = r.Chance(20) ? r.Range(0, 2000000000L) : RandomCount(r) * 1000;
                bool compact = r.Chance(50);
                table.Row("telemetry", I(seconds), I(privateBytes), B(compact), MainForm.FormatLoadingTelemetry(seconds, privateBytes, compact));
            }
            long[] frequencies = { 10000000, 1000000000, 3579545, 2539062, 0, -1 };
            for (int i = 0; i < n; i++)
            {
                long freq = frequencies[r.Below(frequencies.Length)];
                long started = r.Chance(10) ? 0 : RandomCount(r) % 1000000000000L;
                long pauseStarted = r.Chance(60) ? 0 : started + r.Range(-1000, 100000000);
                long pausedTicks = r.Chance(50) ? 0 : r.Range(-1000, 200000000);
                long now = started + r.Range(-1000, 5000000000L);
                table.Row("loadingelapsed", I(started), I(pauseStarted), I(pausedTicks), I(now), I(freq),
                    I(MainForm.ComputeLoadingElapsedSeconds(started, pauseStarted, pausedTicks, now, freq)));
            }

            // --- load status (the expression inside UpdateLoadStatus, copied verbatim)
            string[] stages = { "Reading file", "Merging tracks", "Building indexes", "Counting events" };
            for (int i = 0; i < n; i++)
            {
                string stage = stages[r.Below(stages.Length)];
                int overall = (int)r.Range(0, 1000);
                int stagePermille = (int)r.Range(0, 1000);
                string detail = stage + Environment.NewLine +
                    (overall / 10.0).ToString("N1", CultureInfo.CurrentCulture) + "% overall  \u2022  " +
                    (stagePermille / 10.0).ToString("N1", CultureInfo.CurrentCulture) + "% stage";
                table.Row("loadstatus", stage, I(overall), I(stagePermille), detail);
            }

            // --- file information
            for (int i = 0; i < n; i++)
            {
                MidiSong song = new MidiSong();
                song.TrackCount = (int)(r.Chance(30) ? r.Range(0, 20) : r.Range(0, 100000));
                song.NoteCount = RandomCount(r);
                song.DurationMicroseconds = r.Chance(10) ? 0 : RandomCount(r) * (r.Chance(50) ? 1 : 1000);
                song.NonstandardRunningStatusRepairCount = r.Chance(50) ? 0 : (r.Chance(30) ? 1 : RandomCount(r));
                int events = (int)(r.Chance(10) ? 0 : r.Below(2147483647L) / (r.Chance(50) ? 1 : 1000));
                song.SetEventStore(new FakeEventStore(events));
                bool compact = r.Chance(50);
                table.Row("fileinfo", I(events), I(song.TrackCount), I(song.NoteCount), I(song.DurationMicroseconds),
                    I(song.NonstandardRunningStatusRepairCount), B(compact), MainForm.FormatFileInformation(song, compact));
            }
            long[] repairs = { 1, 2, 3, 1000, 1234567 };
            for (int i = 0; i < repairs.Length; i++)
            {
                // the String.Format in the load-completed handler, copied verbatim
                string message = String.Format(CultureInfo.CurrentCulture,
                    "This file was loaded with {0:N0} nonstandard running-status repair{1}. The repair was limited to an unambiguous channel message immediately after MIDI metadata.",
                    repairs[i], repairs[i] == 1 ? String.Empty : "s");
                table.Row("repairmessage", I(repairs[i]), message);
            }

            // --- large-file warning
            for (int i = 0; i < (primary ? 40 : 12); i++)
            {
                MidiMemoryProjection p = new MidiMemoryProjection();
                p.ProjectedRetainedBytes = RandomCount(r) * (r.Chance(50) ? 1 : 100);
                p.ProjectedPeakLowBytes = RandomCount(r) * (r.Chance(50) ? 1 : 100);
                p.ProjectedPeakHighBytes = RandomCount(r) * (r.Chance(50) ? 1 : 100);
                p.ProjectedRetainedWithRoutingBytes = RandomCount(r) * (r.Chance(50) ? 1 : 100);
                p.ProjectedRoutingPeakBytes = RandomCount(r) * (r.Chance(50) ? 1 : 100);
                p.IsArchitectureRisk = r.Chance(40);
                MidiPreflightCounts counts = new MidiPreflightCounts();
                counts.DispatchableEventCount = RandomCount(r);
                string name = r.Chance(30) ? "Black \u2013 Mix (v2).mid" : "song" + I(i) + ".mid";
                MidiLargeFileInspection inspection = new MidiLargeFileInspection(@"C:\Music\" + name, counts, p);
                table.Row("largemessage", name, I(counts.DispatchableEventCount), I(p.ProjectedRetainedBytes),
                    I(p.ProjectedPeakLowBytes), I(p.ProjectedPeakHighBytes), I(p.ProjectedRetainedWithRoutingBytes),
                    I(p.ProjectedRoutingPeakBytes), B(p.IsArchitectureRisk), LargeMidiWarningDialog.CreateMessage(inspection));
            }

            // --- optional rates
            for (int i = 0; i < n; i++)
            {
                double? rate = RandomOptionalRate(r);
                table.Row("observed", Hex(rate), B(false), MainForm.FormatObservedMaximumRate(rate, false));
                table.Row("observed", Hex(rate), B(true), MainForm.FormatObservedMaximumRate(rate, true));
                table.Row("speed", Hex(rate), EffectivePlaybackSpeed.Format(rate));
                table.Row("outputrate", Hex(rate), RollingOutputRate.Format(rate));
            }

            // --- snapshot driven strings
            for (int i = 0; i < n; i++)
            {
                PlaybackSnapshot s = RandomSnapshot(r);
                double? observed = RandomOptionalRate(r);
                bool compact = r.Chance(50);
                string snap = SnapRef(table, s);
                table.Row("dropped", snap, I(MainForm.TotalDroppedEvents(s)));
                table.Row("counts", snap, B(compact), MainForm.FormatEventCounts(s, compact));
                table.Row("maxrate", snap, Hex(observed), B(compact), MainForm.FormatMaximumRate(s, observed, compact));
            }

            // --- the eight statistics cells, exactly as RefreshStatistics builds them
            for (int i = 0; i < n; i++)
            {
                PlaybackSnapshot s = RandomSnapshot(r);
                bool perNote = r.Chance(20), delayed = r.Chance(20), noteAttack = r.Chance(20), outputRateGate = r.Chance(20);
                long engineProcessing = r.Chance(20) ? 0 : RandomCount(r) % 2000000;
                long engineEps = r.Chance(20) ? 0 : RandomCount(r) % 10000000;
                double? speed = RandomOptionalRate(r);
                double? outputRate = RandomOptionalRate(r);
                double? observed = RandomOptionalRate(r);
                bool compact = r.Chance(50);
                string[] cells = StatisticsCells(s, perNote, delayed, noteAttack, outputRateGate, engineProcessing,
                    engineEps, speed, outputRate, observed, compact);
                List<string> row = new List<string>();
                row.Add("cells");
                string cellsSnap = SnapRef(table, s);
                row.Add(cellsSnap);
                row.Add(B(perNote) + B(delayed) + B(noteAttack) + B(outputRateGate));
                row.Add(I(engineProcessing));
                row.Add(I(engineEps));
                row.Add(Hex(speed));
                row.Add(Hex(outputRate));
                row.Add(Hex(observed));
                row.Add(B(compact));
                row.AddRange(cells);
                table.Row(row.ToArray());
                // Analysis overlay strings
                table.Row("analysis", cellsSnap,
                    s.OutstandingEvents.ToString("N0", CultureInfo.CurrentCulture) + " / " + s.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture),
                    MainForm.FormatLagMilliseconds(s.CurrentLagMicroseconds) + " / " + MainForm.FormatLagMilliseconds(s.MaximumLagMicroseconds));
            }

            // --- queue pressure
            using (StatisticsView view = new StatisticsView())
            {
                MethodInfo setPressure = StatisticsViewType.GetMethod("SetQueuePressure", Hidden, null,
                    new Type[] { typeof(bool), typeof(long), typeof(long), typeof(bool), typeof(bool), typeof(bool) }, null);
                MethodInfo text = StatisticsViewType.GetMethod("QueuePressureTextForTesting", Hidden);
                for (int i = 0; i < n * 2; i++)
                {
                    bool virtualQueue = r.Chance(40);
                    bool age = r.Chance(40);
                    long limit = r.Chance(10) ? r.Range(-3, 0) : RandomCount(r) % 2000000000L;
                    long occupied = r.Chance(30) ? limit : (r.Chance(10) ? -RandomCount(r) % 1000 : RandomCount(r) % 3000000000L);
                    if (r.Chance(30)) occupied = limit * r.Range(0, 130) / 100 + r.Range(-1, 1);
                    // StatisticsView ignores an unchanged state; vary the pulse flag per call so each call applies.
                    setPressure.Invoke(view, new object[] { true, occupied, limit, false, virtualQueue, age });
                    string normal = (string)text.Invoke(view, new object[] { false });
                    string compact = (string)text.Invoke(view, new object[] { true });
                    table.Row("pressure", B(virtualQueue), B(age), I(occupied), I(limit), normal, compact);
                }
            }
            double[] ratios = { 0, 0.1, 0.6499999, 0.65, 0.6500001, 0.89999, 0.9, 0.90001, 0.99, 1.0, 1.0000001, 1.5, 100.0, -1.0 };
            for (int i = 0; i < ratios.Length; i++)
            {
                for (int pulse = 0; pulse < 2; pulse++)
                {
                    System.Drawing.Color color = QueuePressurePalette.Choose(ratios[i], pulse == 1);
                    string name = color == QueuePressurePalette.Low ? "Low" : color == QueuePressurePalette.Medium ? "Medium" :
                        color == QueuePressurePalette.High ? "High" : "OverLimit";
                    table.Row("color", Hex(ratios[i]), I(pulse), name,
                        I(color.R) + "," + I(color.G) + "," + I(color.B));
                }
            }
            table.Row("palette", I(QueuePressurePalette.Background.R) + "," + I(QueuePressurePalette.Background.G) + "," + I(QueuePressurePalette.Background.B),
                I(QueuePressurePalette.Outline.R) + "," + I(QueuePressurePalette.Outline.G) + "," + I(QueuePressurePalette.Outline.B));

            // --- Stopwatch tick conversion (MainForm.StopwatchTicksToMicroseconds uses Stopwatch.Frequency)
            if (primary)
            {
                table.Row("stopwatchfrequency", I(Stopwatch.Frequency));
                long[] ticks = { 0, 1, 9, 10, 11, 12345, 10000000, 123456789, 1234567890123L, 99999999999999L, 4611686018427387L };
                List<long> tickList = new List<long>(ticks);
                for (int i = 0; i < 200; i++) tickList.Add(RandomCount(r) * (r.Chance(50) ? 1 : 1000));
                for (int i = 0; i < tickList.Count; i++)
                {
                    long result = (long)CallStatic(MainFormType, "StopwatchTicksToMicroseconds", tickList[i]);
                    table.Row("stopwatch", I(tickList[i]), I(result));
                }
            }
        }

        // RefreshStatistics' SetValues array and configuredRate, copied from MainForm.cs 3358-3392
        // with the window's state passed in (the v1 helpers it calls are the real ones).
        private static string[] StatisticsCells(PlaybackSnapshot snapshot, bool perNoteEnabled, bool delayedEnabled,
            bool noteAttackEnabled, bool outputRateEnabled, long engineProcessing, long engineEventsPerSecond,
            double? speed, double? outputRate, double? outputRateMaximum, bool compactLayout)
        {
            string configuredRate = perNoteEnabled
                ? MainForm.FormatPerNoteFrameRate(engineProcessing, compactLayout)
                : delayedEnabled
                    ? MainForm.FormatDelayedNoteOffDuration(engineProcessing, compactLayout)
                    : noteAttackEnabled
                        ? (engineEventsPerSecond == 0 ? "unlimited note starts" :
                            engineEventsPerSecond.ToString("N0", CultureInfo.CurrentCulture) +
                            (compactLayout ? " starts/s" : " note starts/sec"))
                    : outputRateEnabled
                        ? (engineEventsPerSecond == 0 ? "unlimited output" :
                            engineEventsPerSecond.ToString("N0", CultureInfo.CurrentCulture) +
                            (compactLayout ? " events/s max" : " maximum events/sec")) +
                            (snapshot.OutputRateSafetySoftExcess == 0 ? String.Empty :
                                compactLayout
                                    ? " (+" + snapshot.OutputRateSafetySoftExcess.ToString("N0", CultureInfo.CurrentCulture) + " required over target)"
                                    : " (" + snapshot.OutputRateSafetySoftExcess.ToString("N0", CultureInfo.CurrentCulture) + " required messages sent over target)")
                    : MainForm.FormatMaximumRate(snapshot, outputRateMaximum, compactLayout);
            return new string[]
            {
                (string)CallStatic(MainFormType, "FormatTime", snapshot.IntendedTimelineMicroseconds) + (compactLayout ? "\u2009/\u2009" : " / ") +
                    (string)CallStatic(MainFormType, "FormatTime", snapshot.LastDispatchedTimelineMicroseconds),
                snapshot.OutstandingEvents.ToString("N0", CultureInfo.CurrentCulture) +
                    (compactLayout ? "\u2009/\u2009" : " / ") +
                    snapshot.MaximumQueueLength.ToString("N0", CultureInfo.CurrentCulture) +
                    (snapshot.VirtualQueueActive ? (compactLayout ? " actual" : " actual backlog") : String.Empty),
                configuredRate,
                compactLayout && outputRate.HasValue
                    ? outputRate.Value.ToString("N0", CultureInfo.CurrentCulture) + " events/s"
                    : RollingOutputRate.Format(outputRate),
                MainForm.FormatEventCounts(snapshot, compactLayout),
                EffectivePlaybackSpeed.Format(speed),
                MainForm.FormatLagMilliseconds(snapshot.MaximumLagMicroseconds),
                MainForm.FormatLagMilliseconds(snapshot.CurrentLagMicroseconds)
            };
        }

        private sealed class FakeEventStore : IMidiEventStore
        {
            private readonly int _count;
            internal FakeEventStore(int count) { _count = count; }
            public int Count { get { return _count; } }
            public MidiEventView GetEvent(int index) { return default(MidiEventView); }
            public int LowerBoundByTime(long microseconds) { return 0; }
        }
    }
}
