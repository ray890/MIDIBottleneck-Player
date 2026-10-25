# Writes MIDI files that contain ONLY silent controller traffic (no Note On at all), so they can be played to a real
# synthesizer without any sound: controller 1 (modulation) and controller 11 (expression) sweeping up and down on all
# 16 channels. Nothing is played or opened by this script; it only writes files.
#
# Files (default folder v2\out\fixtures-silent\, not committed):
#   silent-cc-light-2k-45s.mid    2,000 events/s for 45 s     (a busy but ordinary song's controller traffic)
#   silent-cc-medium-200k-30s.mid 200,000 events/s for 30 s   (a dense black-MIDI-like load)
#   silent-cc-heavy-3m-10s.mid    3,000,000 events/s for 10 s (30 million events; about 120 MB; the engine cannot keep up
#                                                              on most synthesizers, which is the point of the test)
# All are format 0, 480 ticks per quarter at 120 BPM (a tick is about 1.04 ms); the events of one tick share that tick.
# The sweep: for channel c and event number n, the controller alternates between 1 and 11 and the value runs 0..127..0
# (a triangle), so every message differs from the one before it but is harmless: with no note sounding, a modulation or
# expression change makes no sound on any synthesizer.
#
# Usage (from the repository root, Windows PowerShell):
#   .\v2\tools\device\make-silent-fixture.ps1
#   .\v2\tools\device\make-silent-fixture.ps1 -Rate 5000 -Seconds 60 -OutDir C:\temp\fx   # one custom file only
param(
    [string]$OutDir = '',
    [double]$Rate = 0,        # a custom file: events per second (together with -Seconds); omit for the three standard files
    [double]$Seconds = 45
)
$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'out\fixtures-silent' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class SilentFixture
{
    static void Vlq(Stream s, uint x)
    {
        byte[] b = new byte[5]; int n = 0;
        b[n++] = (byte)(x & 0x7F);
        while ((x >>= 7) != 0) b[n++] = (byte)((x & 0x7F) | 0x80);
        while (n > 0) s.WriteByte(b[--n]);
    }
    static void U32(Stream s, uint x) { for (int sh = 24; sh >= 0; sh -= 8) s.WriteByte((byte)((x >> sh) & 0xFF)); }

    // Returns the number of events written.
    public static long Write(string path, double eventsPerSecond, double seconds)
    {
        const int Division = 480; const uint Tempo = 500000;            // 960 ticks per second
        double ticksPerSecond = Division * 1e6 / Tempo;
        long ticks = (long)(seconds * ticksPerSecond);
        double perTick = eventsPerSecond / ticksPerSecond;
        long events = 0; double carry = 0; uint pendingDelta = 0;
        using (FileStream f = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20))
        {
            BufferedStream o = new BufferedStream(f, 1 << 20);
            o.WriteByte((byte)'M'); o.WriteByte((byte)'T'); o.WriteByte((byte)'h'); o.WriteByte((byte)'d');
            U32(o, 6); o.WriteByte(0); o.WriteByte(0); o.WriteByte(0); o.WriteByte(1);
            o.WriteByte((byte)((Division >> 8) & 0xFF)); o.WriteByte((byte)(Division & 0xFF));
            o.WriteByte((byte)'M'); o.WriteByte((byte)'T'); o.WriteByte((byte)'r'); o.WriteByte((byte)'k');
            o.Flush();
            long lengthPosition = f.Position;
            U32(o, 0);

            long bytes = 0;
            // tempo meta
            o.WriteByte(0); o.WriteByte(0xFF); o.WriteByte(0x51); o.WriteByte(0x03);
            o.WriteByte((byte)((Tempo >> 16) & 0xFF)); o.WriteByte((byte)((Tempo >> 8) & 0xFF)); o.WriteByte((byte)(Tempo & 0xFF));
            bytes += 7;
            byte[] ev = new byte[4];
            for (long t = 0; t < ticks; t++)
            {
                carry += perTick;
                long count = (long)carry; carry -= count;
                if (count == 0) { pendingDelta++; continue; }
                for (long i = 0; i < count; i++)
                {
                    uint delta = (i == 0) ? pendingDelta : 0;
                    // delta times here are 0 or small (a few ticks), so at most 2 bytes
                    int n = 0;
                    if (delta < 128) ev[n++] = (byte)delta; else { ev[n++] = (byte)(0x80 | ((delta >> 7) & 0x7F)); ev[n++] = (byte)(delta & 0x7F); }
                    o.Write(ev, 0, n); bytes += n;
                    long e = events;
                    int channel = (int)(e & 15);
                    int controller = (((e >> 4) & 1) == 0) ? 1 : 11;
                    int phase = (int)((e >> 5) % 254);                   // triangle 0..127..0
                    int value = phase < 128 ? phase : 254 - phase;
                    o.WriteByte((byte)(0xB0 | channel)); o.WriteByte((byte)controller); o.WriteByte((byte)value);
                    bytes += 3; events++;
                }
                pendingDelta = 1;
            }
            Vlq(o, pendingDelta); bytes += pendingDelta < 128 ? 1 : (pendingDelta < 16384 ? 2 : 3);
            o.WriteByte(0xFF); o.WriteByte(0x2F); o.WriteByte(0x00); bytes += 3;
            o.Flush();
            f.Position = lengthPosition;
            byte[] len = new byte[] { (byte)((bytes >> 24) & 0xFF), (byte)((bytes >> 16) & 0xFF), (byte)((bytes >> 8) & 0xFF), (byte)(bytes & 0xFF) };
            f.Write(len, 0, 4); f.Flush();
        }
        return events;
    }
}
'@

function New-Fixture([string]$name, [double]$rate, [double]$secs) {
    $path = Join-Path $OutDir $name
    $n = [SilentFixture]::Write($path, $rate, $secs)
    $mb = [math]::Round((Get-Item $path).Length / 1MB, 1)
    "{0}: {1:N0} events in {2} s, {3} MB" -f $name, $n, $secs, $mb
}

if ($Rate -gt 0) {
    New-Fixture ("silent-cc-custom-{0}-{1}s.mid" -f [int]$Rate, [int]$Seconds) $Rate $Seconds
} else {
    New-Fixture 'silent-cc-light-2k-45s.mid' 2000 45
    New-Fixture 'silent-cc-medium-200k-30s.mid' 200000 30
    New-Fixture 'silent-cc-heavy-3m-10s.mid' 3000000 10
}
