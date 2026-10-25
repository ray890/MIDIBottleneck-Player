# Generates the short tune used by the listening clips and the end-to-end player test. Dot-source it:
#   . .\v2\tools\device\listening-tune.ps1
#   New-ListeningTune -Path out.mid -Seconds 12 [-Cycles 1]
#
# Why this file exists: none of the committed fixtures in v2\harness\fixtures is a recognisable piece of music
# (they are test data), and a new fixture there would need new expected traces. The tune is generated into a
# git-ignored folder instead (v2\out\device-test\fixtures\).
#
# The tune (one cycle, `Seconds` long, format 0, 960 ticks per quarter, 4 channels, normal velocities 64 to 100,
# no volume or expression controllers, so the sound level is whatever the synthesizer's default is):
#   A  (28 % of the time)  the theme of Beethoven's "Ode to Joy" (bars 1-2), piano, with chords and a bass note;
#   B  (44 %)              three "glitter" bursts on a celesta sound, each a fast arpeggio roll that sweeps up and
#                          down across five octaves, at about 1,000, 4,000 and 10,000 events per second
#                          (note-on plus note-off), with 0.3 s of rest between them. This is where the player's
#                          timing resolution can be heard: a player that sends its events in bunches every 1 to 2 ms
#                          turns the fast rolls into a different texture than one that sends each event on time;
#   C  (28 %)              the theme again, bars 7-8, ending on the tonic.
# -Cycles repeats the whole piece (for the longer end-to-end test file).
# C# 5 only (the .NET Framework compiler that ships with Windows); no external code.

if (-not ('ListeningTuneMaker' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;

public static class ListeningTuneMaker
{
    struct Ev { public long Tick; public int Rank; public int Seq; public byte S, A, B; }

    static List<Ev> events;
    static int seq;
    static double ticksPerSecond;

    static void Add(double seconds, int rank, int status, int d1, int d2)
    {
        Ev e = new Ev();
        e.Tick = (long)Math.Round(seconds * ticksPerSecond);
        e.Rank = rank; e.Seq = seq++; e.S = (byte)status; e.A = (byte)d1; e.B = (byte)d2;
        events.Add(e);
    }
    static void Note(double start, double length, int channel, int key, int velocity)
    {
        Add(start, 1, 0x90 | channel, key, velocity);
        Add(start + length, 0, 0x80 | channel, key, 0x40);
    }
    static void Vlq(List<byte> v, long x)
    {
        Stack<byte> s = new Stack<byte>();
        s.Push((byte)(x & 0x7F));
        while ((x >>= 7) != 0) s.Push((byte)((x & 0x7F) | 0x80));
        while (s.Count > 0) v.Add(s.Pop());
    }

    // Returns "events=N duration_s=D".
    public static string Make(string path, double seconds, int cycles)
    {
        events = new List<Ev>(); seq = 0;
        double q = 0.035 * seconds;                 // seconds per quarter note: 16 quarter notes of theme take 56 % of the cycle
        int ppqn = 960;
        ticksPerSecond = ppqn / q;
        double cycle = seconds;
        // Program changes (one data byte, written at the start of the track): piano, piano, acoustic bass, celesta
        List<int[]> programs = new List<int[]>();
        programs.Add(new int[] { 0, 0 }); programs.Add(new int[] { 1, 0 }); programs.Add(new int[] { 2, 32 }); programs.Add(new int[] { 3, 8 });

        for (int c = 0; c < cycles; c++) {
            double t0 = c * cycle;
            // A: bars 1-2: E E F G | G F E D  (E5=76 F5=77 G5=79 D5=74 C5=72)
            int[] melA = { 76, 76, 77, 79, 79, 77, 76, 74 };
            for (int i = 0; i < melA.Length; i++) Note(t0 + i * q, q * 0.9, 0, melA[i], 90);
            Chord(t0, 4 * q, new int[] { 60, 64, 67 }, 64); Note(t0, 4 * q * 0.95, 2, 48, 80);
            Chord(t0 + 4 * q, 4 * q, new int[] { 55, 59, 62 }, 64); Note(t0 + 4 * q, 4 * q * 0.95, 2, 43, 80);
            // B: three glitter bursts
            double bStart = t0 + 8 * q;
            double bLen = cycle - 16 * q;
            double lead = 0.05 * bLen, rest = 0.3, tail = 0.05 * bLen;
            double burst = (bLen - lead - tail - 2 * rest) / 3.0;
            double[] density = { 1000, 4000, 10000 };
            int[][] chordTones = { new int[] { 0, 4, 7 }, new int[] { 9, 0, 4 }, new int[] { 7, 11, 2 } };
            uint rng = 12345;
            for (int b = 0; b < 3; b++) {
                double bs = bStart + lead + b * (burst + rest);
                // pitch list: chord tones over five octaves (C2 to about G7), played up and down
                List<int> up = new List<int>();
                for (int oct = 3; oct <= 7; oct++)
                    for (int k = 0; k < 3; k++) {
                        int p = 12 * oct + chordTones[b][k];
                        if (p > 100) p -= 12;
                        up.Add(p);
                    }
                up.Sort();
                List<int> pat = new List<int>(up);
                for (int i = up.Count - 2; i >= 1; i--) pat.Add(up[i]);
                double notesPerSecond = density[b] / 2.0;
                double spacing = 1.0 / notesPerSecond;
                double len = Math.Min(0.02, Math.Max(0.004, 10.0 / notesPerSecond));   // short, so notes do not pile up
                long count = (long)(burst * notesPerSecond);
                for (long n = 0; n < count; n++) {
                    rng = rng * 1664525u + 1013904223u;
                    int vel = 64 + (int)((rng >> 16) % 37u);                           // 64..100
                    Note(bs + n * spacing, len, 3, pat[(int)(n % pat.Count)], vel);
                }
            }
            // C: bars 7-8: C C D E | D. C C  (the last two notes are an eighth and a half note after the dotted quarter)
            double cs = t0 + 8 * q + bLen;
            int[] melC = { 72, 72, 74, 76 };
            for (int i = 0; i < melC.Length; i++) Note(cs + i * q, q * 0.9, 0, melC[i], 90);
            Note(cs + 4 * q, q * 1.4, 0, 74, 90);
            Note(cs + 5.5 * q, q * 0.45, 0, 72, 90);
            Note(cs + 6 * q, q * 1.9, 0, 72, 90);
            Chord(cs, 4 * q, new int[] { 60, 64, 67 }, 64); Note(cs, 4 * q * 0.95, 2, 48, 80);
            Chord(cs + 4 * q, 2 * q, new int[] { 55, 59, 62 }, 64); Note(cs + 4 * q, 2 * q * 0.95, 2, 43, 80);
            Chord(cs + 6 * q, 2 * q, new int[] { 60, 64, 67 }, 64); Note(cs + 6 * q, 2 * q * 0.95, 2, 48, 80);
        }
        events.Sort(delegate(Ev a, Ev b) {
            int r = a.Tick.CompareTo(b.Tick); if (r != 0) return r;
            r = a.Rank.CompareTo(b.Rank); if (r != 0) return r;
            return a.Seq.CompareTo(b.Seq);
        });

        List<byte> trk = new List<byte>();
        // tempo (microseconds per quarter), time signature 4/4, track name
        Vlq(trk, 0); trk.Add(0xFF); trk.Add(0x51); trk.Add(0x03);
        long tempo = (long)Math.Round(q * 1e6);
        trk.Add((byte)(tempo >> 16)); trk.Add((byte)(tempo >> 8)); trk.Add((byte)tempo);
        Vlq(trk, 0); trk.Add(0xFF); trk.Add(0x58); trk.Add(0x04); trk.Add(4); trk.Add(2); trk.Add(24); trk.Add(8);
        string name = "Listening tune";
        Vlq(trk, 0); trk.Add(0xFF); trk.Add(0x03); trk.Add((byte)name.Length); foreach (char x in name) trk.Add((byte)x);
        foreach (int[] p in programs) { Vlq(trk, 0); trk.Add((byte)(0xC0 | p[0])); trk.Add((byte)p[1]); }
        long last = 0; int count2 = 0;
        foreach (Ev e in events) {
            Vlq(trk, e.Tick - last); last = e.Tick;
            trk.Add(e.S); trk.Add(e.A); trk.Add(e.B);
            count2++;
        }
        Vlq(trk, (long)Math.Round(0.3 * ticksPerSecond)); trk.Add(0xFF); trk.Add(0x2F); trk.Add(0x00);

        List<byte> file = new List<byte>();
        file.AddRange(new byte[] { (byte)'M', (byte)'T', (byte)'h', (byte)'d', 0, 0, 0, 6, 0, 0, 0, 1, (byte)(ppqn >> 8), (byte)ppqn });
        file.AddRange(new byte[] { (byte)'M', (byte)'T', (byte)'r', (byte)'k' });
        int n2 = trk.Count;
        file.Add((byte)(n2 >> 24)); file.Add((byte)(n2 >> 16)); file.Add((byte)(n2 >> 8)); file.Add((byte)n2);
        file.AddRange(trk);
        File.WriteAllBytes(path, file.ToArray());
        return "events=" + count2 + " duration_s=" + ((last / ticksPerSecond) + 0.3).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture);
    }

    static void Chord(double start, double length, int[] keys, int velocity)
    {
        foreach (int k in keys) Note(start, length * 0.95, 1, k, velocity);
    }
}
'@
}

function New-ListeningTune {
    param([Parameter(Mandatory = $true)][string]$Path, [double]$Seconds = 12, [int]$Cycles = 1)
    if ($Seconds -lt 8) { throw 'A tune needs at least 8 seconds per cycle.' }
    $dir = Split-Path -Parent $Path
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $info = [ListeningTuneMaker]::Make($Path, $Seconds, $Cycles)
    return $info
}
