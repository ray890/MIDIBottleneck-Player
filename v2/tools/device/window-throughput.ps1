# Does an open companion window slow playback? A silent, repeatable GUI measurement of the REAL players (prompt 009).
#
#   .\v2\tools\device\window-throughput.ps1 -SelfCheck                       # validates everything, prints the plan, starts NOTHING
#   .\v2\tools\device\window-throughput.ps1 [-Programs v1,v2,v2-old] [-V2OldExe <older midibn-win32.exe>] [-Windows none,channels,analysis,routing]
#                                           [-Rounds 3] [-Fixture <mid>] [-Rate 20000000] [-Seconds 1.0] [-Label <text>] [-Monitor 'Internal Display']
#
# What it does: for each player (v1 = the C# player, v2 = midibn-win32.exe, v2-old = a second v2 build for before/after comparisons) it
# starts the player, places it WITHOUT activation on the monitor "Internal Display", loads the file once and chooses output "None". Then,
# for every round and every window kind, it presses Stop (rewind to 0), opens the companion window (none = no window = the reference;
# channels = the MIDI Channel Monitor with its Forced Overrides; analysis = the Analysis window; routing = Track routing), places that
# window without activation on the same monitor, presses Play, and measures
#   * play_ms  = wall time from the Play press until the state text leaves "Playing" (polled every 20 ms in compiled code, timeout 120 s);
#   * meps     = events / play_ms / 1000, i.e. million events per second of wall time. The event count is the number printed by the generator
#                ("midibn-device-measure gen ... events=N") for generated files, or the "events=" line of "midibn-device-measure run
#                --load-only 1" (.load.txt) for a file given with -Fixture; the same number is used for every program;
#   * the main window's and the companion window's responsiveness: SendMessageTimeout(WM_NULL) every 100 ms, reply time max and 95th percentile
#     (a window that does not answer in 5 s counts as 5000 ms);
#   * "Events sent / dropped" read from the player's statistics area with the Windows text recogniser (after the timed part; -NoOcr skips it);
#   * the CPU load and the processor performance (clock as % of base) for 2 s before and 2 s after the play, and the active power plan.
# Rounds run interleaved (round 1 of every window kind, then round 2, ...) so slow drift of the PC does not favour one kind. The companion
# window is closed after each round. Track routing is modal and only allowed while stopped (v1 MainForm.ShowTrackRoutingDialog and v2
# open_routing: the menu item is greyed during playback and the dialog disables the main window), so that cell is recorded as
# "not available during playback" instead of being measured.
#
# SAFETY (fail closed, mandatory): the players start on the first REAL MIDI device. Before EVERY Play the script selects output "None",
# makes sure KDMAPI is not ticked, then reads the output box text and the KDMAPI state again; unless they show exactly None / not ticked,
# Play is not pressed, the failure is recorded and the whole script stops (WINDOW-THROUGHPUT: FAIL). No real device is chosen, no volume,
# mute or system setting (including the power plan) is changed, the real mouse is never used (window messages only), nothing runs on the
# hidden desktop. Every window is shown on the monitor "Internal Display" (1920x1080); "LG HDR 4K" is refused. The run refuses to start
# while the PC is busy (CPU above 20 %) unless -IgnoreBusy is given.
#
# Results: v2\out\window-throughput\<stamp>-<label>\ results.csv, summary.md (medians per program x window and the ratio to that
# program's "none" row), log.txt. Ends with WINDOW-THROUGHPUT: DONE or WINDOW-THROUGHPUT: FAIL (exit 0 / 1). -SelfCheck ends with
# WINDOW-THROUGHPUT: SELFCHECK PASS / FAIL.
[CmdletBinding()]
param(
    [string[]]$Programs = @('v1', 'v2'),
    [string]$V2Exe = '',
    [string]$V2OldExe = '',
    [string]$V1Exe = '',
    [string[]]$Windows = @('none', 'channels', 'analysis', 'routing'),
    [string]$Fixture = '',
    [long]$Rate = 20000000,
    [double]$Seconds = 1.0,
    [int]$Rounds = 3,
    [string]$Monitor = 'Internal Display',
    [string]$Label = '',
    [string]$OutDir = '',
    [int]$TimeoutSeconds = 120,
    [int]$SettleMs = 2500,
    [int]$LoadTimeoutMinutes = 10,
    [switch]$AcceptLargeFileWarning,
    [switch]$NoOcr,
    [switch]$IgnoreBusy,
    [switch]$SelfCheck
)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$here = $PSScriptRoot
. (Join-Path $here '..\adaptive-repeats.ps1')   # 3 rounds, plus one at a time up to 7 while a window's effect is unclear; an explicit -Rounds N is exact
$script:RoundNotes = New-Object System.Collections.ArrayList
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$inv = [Globalization.CultureInfo]::InvariantCulture
function Split-List([string[]]$list) { return @($list | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ }) }

# ---------------------------------------------------------------- parameters
if ($V2OldExe -and -not $PSBoundParameters.ContainsKey('Programs')) { $Programs = @('v1', 'v2', 'v2-old') }
$Programs = Split-List $Programs; $Windows = Split-List $Windows
foreach ($p in $Programs) { if ($p -notin 'v1', 'v2', 'v2-old') { throw "unknown program '$p' (v1, v2, v2-old)" } }
foreach ($w in $Windows) { if ($w -notin 'none', 'channels', 'analysis', 'routing') { throw "unknown window kind '$w' (none, channels, analysis, routing)" } }
if ($Programs.Count -eq 0 -or $Windows.Count -eq 0) { throw 'give at least one program and one window kind' }
if ($Programs -contains 'v2-old' -and -not $V2OldExe) { throw '-Programs v2-old needs -V2OldExe <path>' }
if ($Rounds -lt 1 -or $Seconds -le 0 -or $Rate -lt 1000) { throw 'bad -Rounds / -Seconds / -Rate' }
if ($Monitor -match 'LG' -or $Monitor -match '4K') { throw "refusing monitor '$Monitor': this script only uses 'Internal Display' (never the LG HDR 4K screen)" }
if ($Windows -contains 'none') { $Windows = @('none') + @($Windows | Where-Object { $_ -ne 'none' }) }   # the reference first in every round

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$tag = if ($Label) { ($Label -replace '[^A-Za-z0-9_.-]', '-') } else { 'run' }
if (-not $OutDir) { $OutDir = Join-Path $repo "v2\out\window-throughput\$stamp-$tag" }
$fixDir = Join-Path $repo 'v2\out\window-throughput\fixtures'
$Gen = Join-Path $repo 'v2\out\build-x64\Release\midibn-device-measure.exe'

# ---------------------------------------------------------------- compiled helpers (the existing drivers, plus the timing loop)
$samplerSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
// Presses Play and times the run. Polls the state text every 20 ms; a second thread pings the main window and the companion window
// (WM_NULL) every 100 ms. Self-contained: window messages only, no real mouse.
public static class WtSampler
{
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SendMessageTimeout(IntPtr h, int msg, IntPtr w, IntPtr l, int flags, int timeoutMs, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr SendMessageTimeout(IntPtr h, int msg, IntPtr w, StringBuilder l, int flags, int timeoutMs, out IntPtr result);
    static volatile bool stopFlag;
    const int PingTimeout = 5000;

    static double Ping(IntPtr h)
    {
        if (h == IntPtr.Zero || !IsWindow(h)) return -1;
        Stopwatch sw = Stopwatch.StartNew(); IntPtr r;
        IntPtr ok = SendMessageTimeout(h, 0, IntPtr.Zero, IntPtr.Zero, 0, PingTimeout, out r);   // flags 0: a busy window shows its real delay
        return ok == IntPtr.Zero ? PingTimeout : sw.Elapsed.TotalMilliseconds;
    }
    static string State(IntPtr h)
    {
        StringBuilder sb = new StringBuilder(64); IntPtr r;
        if (SendMessageTimeout(h, 0x000D /*WM_GETTEXT*/, (IntPtr)64, sb, 0, 2000, out r) == IntPtr.Zero) return null;
        return sb.ToString().Trim();
    }
    static double P95(List<double> v)
    {
        if (v.Count == 0) return -1;
        double[] a = v.ToArray(); Array.Sort(a);
        return a[Math.Max(0, (int)Math.Ceiling(0.95 * a.Length) - 1)];
    }
    static double Max(List<double> v) { double m = -1; foreach (double d in v) if (d > m) m = d; return m; }

    // "OK|play_ms|final state|main max|main p95|companion max|companion p95|n main|n companion"
    public static string Run(IntPtr play, IntPtr state, IntPtr mainWin, IntPtr compWin, int timeoutMs)
    {
        List<double> mp = new List<double>(), cp = new List<double>();
        stopFlag = false;
        Thread t = new Thread(delegate() {
            while (!stopFlag) {
                double a = Ping(mainWin); if (a >= 0) lock (mp) mp.Add(a);
                double b = Ping(compWin); if (b >= 0) lock (cp) cp.Add(b);
                Thread.Sleep(100);
            }
        });
        t.IsBackground = true;
        Stopwatch sw = Stopwatch.StartNew();
        PostMessage(play, 0x00F5 /*BM_CLICK*/, IntPtr.Zero, IntPtr.Zero);
        t.Start();
        bool sawPlaying = false; string last = "", final = ""; long endMs;
        while (true) {
            string s = State(state);
            endMs = sw.ElapsedMilliseconds;
            if (s != null) {
                last = s;
                if (s == "Playing") sawPlaying = true;
                else if (s == "Completed" || (sawPlaying && s.Length > 0)) { final = s; break; }
            }
            if (endMs > timeoutMs) { final = "TIMEOUT(" + last + ")"; break; }
            Thread.Sleep(20);
        }
        stopFlag = true; t.Join(7000);
        lock (mp) lock (cp)
            return string.Format(CultureInfo.InvariantCulture, "OK|{0}|{1}|{2:F1}|{3:F1}|{4:F1}|{5:F1}|{6}|{7}", endMs, final, Max(mp), P95(mp), Max(cp), P95(cp), mp.Count, cp.Count);
    }
}
'@
if (-not ('WatchDriver' -as [type])) {
    Add-Type -Path (Join-Path $repo 'v2\tools\visual\VisualCapture.cs'), (Join-Path $repo 'v2\tools\visual\VisualDriver.cs'), (Join-Path $here 'E2eDriver.cs'), (Join-Path $here 'WatchDriver.cs') -ReferencedAssemblies System.Drawing
}
if (-not ('WtSampler' -as [type])) { Add-Type -TypeDefinition $samplerSource -Language CSharp }
[VisualDriver]::MakeDpiAware()

# ---------------------------------------------------------------- the monitor (as watch-passages.ps1)
function Get-MonitorList {
    $list = @()
    foreach ($line in [WatchDriver]::Monitors()) {
        $f = $line -split '\|'; $g = $f[1] -split ' '
        $list += [pscustomobject]@{ Device = $f[0]; Left = [int]$g[0]; Top = [int]$g[1]; Width = [int]$g[2]; Height = [int]$g[3]; Adapter = $f[3]; Name = $f[4] }
    }
    return , $list
}
function Resolve-Monitor([string]$name) {
    $all = Get-MonitorList; $n = $name.Trim().ToLowerInvariant()
    if ($n -eq 'internal display') { $m = @($all | Where-Object { $_.Device -eq '\\.\DISPLAY1' -and $_.Width -eq 1920 -and $_.Height -eq 1080 }) }
    else { $m = @($all | Where-Object { $_.Device -ieq $name -or $_.Name -like "*$name*" -or $_.Adapter -like "*$name*" }) }
    if ($m.Count -ne 1) { throw ("monitor '{0}' not found exactly once (found {1}); monitors: {2}" -f $name, $m.Count, (($all | ForEach-Object { '{0} {1}x{2}@{3},{4}' -f $_.Device, $_.Width, $_.Height, $_.Left, $_.Top }) -join '; ')) }
    if ($m[0].Width -ge 3840 -or $m[0].Height -ge 2160) { throw "monitor '$name' resolves to a 4K screen ($($m[0].Device) $($m[0].Width)x$($m[0].Height)): refused" }
    return $m[0]
}

# ---------------------------------------------------------------- programs, builds, fixture
function Get-V1Exe {
    if ($V1Exe) { return (Resolve-Path -LiteralPath $V1Exe).Path }
    $name = 'release-package\MIDIBottleneck-Player-v1.0.44-x64.exe'
    $c = @((Join-Path $repo $name))
    try { $common = (& git -C $repo rev-parse --git-common-dir 2>$null); if ($common) { $cd = if ([IO.Path]::IsPathRooted($common)) { $common } else { Join-Path $repo $common }; $c += (Join-Path (Split-Path -Parent ((Resolve-Path $cd).Path)) $name) } } catch { }
    foreach ($p in $c) { if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path } }
    throw "v1 player not found (looked in: $($c -join '; ')); give -V1Exe"
}
function Get-V2Exe([string]$given) {
    $p = if ($given) { $given } else { Join-Path $repo 'v2\out\build-x64\Release\midibn-win32.exe' }
    if (-not (Test-Path -LiteralPath $p)) { throw "v2 player not found: $p (build it: .\v2\tools\build-player.ps1)" }
    return (Resolve-Path -LiteralPath $p).Path
}
function Get-BuildId([string]$exe) {
    $f = Get-Item -LiteralPath $exe
    $sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.Substring(0, 8).ToLowerInvariant()
    return ('{0:yyyyMMdd-HHmm}-{1}' -f $f.LastWriteTime, $sha)
}
function Get-PowerPlanName {
    try { $t = (& powercfg.exe /getactivescheme 2>$null) -join ' '; if ($t -match '\(([^)]+)\)\s*$') { return $Matches[1] }; return $t.Trim() } catch { return 'unknown' }
}
# Fixture and its event count. A generated fixture is cached with a sidecar file holding the generator's event count.
function Get-Events-LoadOnly([string]$path) {
    if (-not (Test-Path -LiteralPath $Gen)) { throw "midibn-device-measure.exe not found: $Gen (needed to count the events)" }
    $d = Join-Path $OutDir 'load-only'; New-Item -ItemType Directory -Force -Path $d | Out-Null
    & $Gen run --load-only 1 --fixture $path --out $d --label wt | Out-Null
    $f = Join-Path $d 'wt.load.txt'
    if (Test-Path -LiteralPath $f) { foreach ($l in (Get-Content -LiteralPath $f)) { if ($l -match '^events=(\d+)\s*$') { return [int64]$Matches[1] } } }
    throw "could not read events= from $f"
}
function Resolve-Fixture([bool]$create) {
    if ($Fixture) {
        if (-not (Test-Path -LiteralPath $Fixture)) { throw "fixture not found: $Fixture" }
        $p = (Resolve-Path -LiteralPath $Fixture).Path
        return @{ Path = $p; Events = $(if ($create) { Get-Events-LoadOnly $p } else { -1 }); Source = 'given (events from midibn-device-measure run --load-only 1)' }
    }
    New-Item -ItemType Directory -Force -Path $fixDir | Out-Null
    $secText = $Seconds.ToString('0.##', $inv)
    $p = Join-Path $fixDir ('clustered-{0}-{1}s.mid' -f $Rate, $secText.Replace('.', 'p'))
    $side = "$p.events"
    if ((Test-Path -LiteralPath $p) -and (Test-Path -LiteralPath $side)) { return @{ Path = $p; Events = [int64](Get-Content -LiteralPath $side -TotalCount 1); Source = 'generated earlier (cached; events from the generator output)' } }
    if (-not $create) { return @{ Path = $p; Events = -1; Source = 'will be generated with midibn-device-measure gen' } }
    if (-not (Test-Path -LiteralPath $Gen)) { throw "midibn-device-measure.exe not found: $Gen (build the v2 tools, or give -Fixture)" }
    $out = (& $Gen gen $p clustered ([string]$Rate) $secText) -join ' '
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $p)) { throw "fixture generation failed: $out" }
    $ev = if ($out -match 'events=(\d+)') { [int64]$Matches[1] } else { Get-Events-LoadOnly $p }
    Set-Content -LiteralPath $side -Value $ev
    return @{ Path = $p; Events = $ev; Source = 'generated now (events from the generator output)' }
}

# ---------------------------------------------------------------- self-check
$progDefs = @()
foreach ($p in $Programs) {
    $exe = switch ($p) { 'v1' { Get-V1Exe } 'v2' { Get-V2Exe $V2Exe } 'v2-old' { Get-V2Exe $V2OldExe } }
    $progDefs += @{ Name = $p; Kind = $(if ($p -eq 'v1') { 'v1' } else { 'v2' }); Exe = $exe; Build = (Get-BuildId $exe) }
}
$mon = Resolve-Monitor $Monitor
$fx = Resolve-Fixture $false
$planName = Get-PowerPlanName
if ($SelfCheck) {
    Write-Host "window-throughput self-check (nothing is started)"
    foreach ($d in $progDefs) { Write-Host ("  program {0,-7} {1}  build {2}" -f $d.Name, $d.Exe, $d.Build) }
    Write-Host ("  monitor: {0} {1}x{2} at {3},{4} (windows go here, without activation)" -f $mon.Device, $mon.Width, $mon.Height, $mon.Left, $mon.Top)
    Write-Host ("  fixture: {0} ({1}); events {2}" -f $fx.Path, $fx.Source, $(if ($fx.Events -ge 0) { $fx.Events } else { 'not known yet' }))
    Write-Host ("  generator: {0} ({1})" -f $Gen, $(if (Test-Path -LiteralPath $Gen) { 'found' } else { 'MISSING' }))
    Write-Host ("  power plan: {0}; windows: {1}; rounds: {2}; timeout {3} s; settle {4} ms; OCR {5}" -f $planName, ($Windows -join ','), $Rounds, $TimeoutSeconds, $SettleMs, $(if ($NoOcr) { 'off' } else { 'on' }))
    $measured = ($Windows | Where-Object { $_ -ne 'routing' }).Count
    Write-Host ("  plan: {0} program(s) x {1} measured window kind(s) x {2} round(s) = {3} plays, each preceded by the output-None gate{4}" -f $progDefs.Count, $measured, $Rounds, ($progDefs.Count * $measured * $Rounds), $(if ($Windows -contains 'routing') { '; routing is checked once per program and recorded as not available during playback' } else { '' }))
    Write-Host '  output order per Play: select None, clear KDMAPI, re-read both, abort if not exactly None / not ticked'
    $ok = (Test-Path -LiteralPath $Gen) -or $Fixture
    Write-Host $(if ($ok) { 'WINDOW-THROUGHPUT: SELFCHECK PASS' } else { 'WINDOW-THROUGHPUT: SELFCHECK FAIL (no midibn-device-measure.exe and no -Fixture)' })
    exit $(if ($ok) { 0 } else { 1 })
}

# ================================================================================================ the run
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$logPath = Join-Path $OutDir 'log.txt'; $csvPath = Join-Path $OutDir 'results.csv'; $sumPath = Join-Path $OutDir 'summary.md'; $ocrPng = Join-Path $OutDir 'ocr-tmp.png'
function Log([string]$t) { $line = '{0} {1}' -f (Get-Date).ToString('HH:mm:ss'), $t; Write-Host $line; Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8 }
$script:Fails = New-Object System.Collections.ArrayList
function Fail([string]$s, [string]$why) { Log "FAIL ${s}: $why"; [void]$script:Fails.Add("${s}: $why") }
function Wait-Ms([int]$ms) { Start-Sleep -Milliseconds $ms }
function Csv-Row($fields) { return (($fields | ForEach-Object { $s = [string]$_; if ($s -match '[",\r\n]') { '"' + ($s -replace '"', '""') + '"' } else { $s } }) -join ',') }
$csvHeader = 'program,build,window,round,events,play_ms,meps,main_ping_max_ms,main_ping_p95_ms,companion_ping_max_ms,cpu_before,cpu_after,perf_pct,plan,status,events_sent,dropped,notes'
Set-Content -LiteralPath $csvPath -Value $csvHeader -Encoding UTF8
$script:Rows = New-Object System.Collections.ArrayList

# CPU load and processor performance (whole PC), average over $sec seconds
function Sample-Cpu([int]$sec = 2) {
    $r = @{ Busy = -1.0; Perf = -1.0 }
    for ($try = 0; $try -lt 3 -and $r.Busy -lt 0; $try++) {
        try {
            $cs = (Get-Counter -Counter @('\Processor(_Total)\% Processor Time', '\Processor Information(_Total)\% Processor Performance') -SampleInterval 1 -MaxSamples $sec -ErrorAction Stop).CounterSamples
            $b = $cs | Where-Object { $_.Path -like '*\processor(_total)\% processor time' }
            if ($b) { $r.Busy = [math]::Round(($b | Measure-Object -Property CookedValue -Average).Average, 1) }
            $p = $cs | Where-Object { $_.Path -like '*processor information(_total)\% processor performance' }
            if ($p) { $r.Perf = [math]::Round(($p | Measure-Object -Property CookedValue -Average).Average, 0) }
        } catch { Wait-Ms 300 }
    }
    return $r
}

# ---------------------------------------------------------------- controls of both players (as watch-passages.ps1)
$V2Id = @{ Output = 111; Kdmapi = 112; State = 113; Play = 127; Stop = 128; Analysis = 131; Reset = 132; Stats = 133 }
function Rect-Inside($inner, $outer) { return ($inner[0] -ge $outer[0] - 2 -and $inner[1] -ge $outer[1] - 2 -and $inner[2] -le $outer[2] + 2 -and $inner[3] -le $outer[3] + 2) }
function Main-Handle($pl) {
    if ($pl.Kind -eq 'v2') { return $pl.H }
    if ([VisualDriver]::Alive($pl.H) -and [VisualDriver]::Visible($pl.H)) { return $pl.H }
    foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) {
        if ([VisualDriver]::Cls($w) -like 'WindowsForms10.Window*' -and [VisualDriver]::Txt($w) -eq 'MIDIBottleneck Player') { $pl.H = $w; return $w }
    }
    return [IntPtr]::Zero
}
function Ctl($pl, [string]$name) {
    if ($pl.Kind -eq 'v2') {
        $id = $V2Id[$name]
        foreach ($d in [VisualDriver]::Descendants($pl.H, $false)) { if ([VisualDriver]::CtrlId($d) -eq $id -and [VisualDriver]::Cls($d) -notmatch 'Caption') { return $d } }
        return [IntPtr]::Zero
    }
    $h = Main-Handle $pl
    if ($h -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    $all = [VisualDriver]::Descendants($h, $false)
    $btn = { param($pred) foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and (& $pred ([VisualDriver]::Txt($d)))) { return $d } }; return [IntPtr]::Zero }
    switch ($name) {
        'Play' { return (& $btn { param($t) $t -eq 'Play' -or $t -eq 'Pause' -or $t -eq 'Resume' }) }
        'Stop' { return (& $btn { param($t) $t -eq 'Stop' }) }
        'Analysis' { return (& $btn { param($t) $t -like 'Analysis*' }) }
        'Reset' { return (& $btn { param($t) $t -eq 'Reset stats' }) }
        'Open' { return (& $btn { param($t) $t -like 'Open MIDI*' }) }
        'Kdmapi' { return (& $btn { param($t) $t -eq 'KDMAPI' }) }
        'State' { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'STATIC' -and [VisualDriver]::Txt($d) -match '^(Stopped|Playing|Paused|Completed|Stopping|Loading.*)$') { return $d } }; return [IntPtr]::Zero }
        'Output' {
            foreach ($d in $all) {
                if ([VisualDriver]::Cls($d) -notmatch 'COMBOBOX') { continue }
                $n = [VisualDriver]::ComboCount($d); if ($n -lt 1) { continue }
                if ([VisualDriver]::ComboItem($d, 0) -eq 'None' -and $n -ne 8) { return $d }
            }
            return [IntPtr]::Zero
        }
        'Stats' {   # the empty panel inside the "Statistics" group (the players' own drawn control has no text)
            $group = [IntPtr]::Zero
            foreach ($d in $all) { if ([VisualDriver]::Cls($d) -like 'WindowsForms10.Window*' -and [VisualDriver]::Txt($d) -eq 'Statistics') { $group = $d; break } }
            if ($group -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
            $gr = [VisualDriver]::Rect($group); $best = [IntPtr]::Zero; $bestTop = [int]::MaxValue
            foreach ($d in $all) {
                if ($d -eq $group -or [VisualDriver]::Cls($d) -notlike 'WindowsForms10.Window*' -or [VisualDriver]::Txt($d) -ne '') { continue }
                $r = [VisualDriver]::Rect($d); $hgt = $r[3] - $r[1]
                if (-not (Rect-Inside $r $gr) -or ($r[2] - $r[0]) -lt 300 -or $hgt -lt 60 -or $hgt -gt 140) { continue }
                if ($r[1] -lt $bestTop) { $best = $d; $bestTop = $r[1] }
            }
            return $best
        }
    }
    return [IntPtr]::Zero
}
function State-Text($pl) { $c = Ctl $pl 'State'; if ($c -eq [IntPtr]::Zero) { return '?' }; return ([VisualDriver]::Txt($c)).Trim() }
function Wait-State($pl, [string[]]$names, [int]$timeoutMs = 10000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) { $s = State-Text $pl; if ($names -contains $s) { return $s }; Wait-Ms 100 }
    return (State-Text $pl)
}

# ---------------------------------------------------------------- the output gate: output None, KDMAPI off (fail closed)
# v1 draws its KDMAPI box itself (BM_GETCHECK always says "not ticked"); v1 greys out its output list exactly while KDMAPI is ticked.
function Kd-State($pl, [IntPtr]$k) {
    if ($pl.Kind -ne 'v1') { return [VisualDriver]::CheckState($k) }
    $o = Ctl $pl 'Output'
    if ($o -eq [IntPtr]::Zero -or [VisualDriver]::ComboCount($o) -le 0) { return -1 }
    return $(if ([VisualDriver]::Enabled($o)) { 0 } else { 1 })
}
function Set-NoneOutput($pl) {
    $o = Ctl $pl 'Output'; $k = Ctl $pl 'Kdmapi'
    if ($o -eq [IntPtr]::Zero -or $k -eq [IntPtr]::Zero) { throw 'MIDI output controls not found' }
    for ($try = 0; $try -lt 3 -and (Kd-State $pl $k) -ne 0; $try++) {
        if ($try -eq 1 -and $pl.Kind -eq 'v1') { [WatchDriver]::NotifyClicked($k) } else { [VisualDriver]::Click($k) }
        Wait-Ms 500
    }
    if ((Kd-State $pl $k) -ne 0) { throw 'KDMAPI is ticked (or unreadable) and could not be cleared' }
    for ($try = 0; $try -lt 15 -and [VisualDriver]::ComboSel($o) -ne 0; $try++) {
        if ([VisualDriver]::Enabled($o)) { [VisualDriver]::ComboSelect($o, 0) }
        Wait-Ms 400
    }
}
# Throws (and the caller does not press Play) unless the output box shows None and KDMAPI is not ticked.
function Assert-OutputGate($pl) {
    $o = Ctl $pl 'Output'; $k = Ctl $pl 'Kdmapi'; $why = ''
    if ($o -eq [IntPtr]::Zero -or $k -eq [IntPtr]::Zero) { $why = 'the output controls were not found' }
    else {
        $sel = [VisualDriver]::ComboSel($o)
        $item = if ($sel -ge 0) { [VisualDriver]::ComboItem($o, $sel) } else { '(nothing selected)' }
        if ($sel -ne 0 -or $item -ne 'None') { $why = "output box shows '$item' (index $sel), not None" }
        elseif ((Kd-State $pl $k) -ne 0) { $why = 'KDMAPI is ticked (or its state is unreadable)' }
    }
    if ($why) { throw "OUTPUT GATE: not pressing Play ($($pl.Name)) - $why" }
}

# ---------------------------------------------------------------- text recogniser for "Events sent / dropped" (as watch-passages.ps1)
$script:Ocr = $null; $script:AsTask = $null
function Init-Ocr {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    [void][Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
    [void][Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
    [void][Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]
    $script:AsTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
    $script:Ocr = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    if (-not $script:Ocr) { throw 'no text recogniser language is installed' }
}
function Await($op, [Type]$t) { $task = $script:AsTask.MakeGenericMethod($t).Invoke($null, @($op)); [void]$task.Wait(-1); return $task.Result }
function Ocr-File([string]$png) {
    $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($png)) ([Windows.Storage.StorageFile])
    $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    try {
        $dec = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bmp = Await ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        return ((Await ($script:Ocr.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])).Text -replace '\s+', ' ')
    }
    finally { $stream.Dispose() }
}
# Consolas' slashed zero is read as a letter: inside number tokens those letters mean 0, I / l mean 1.
function Fix-Num([string]$s) {
    return ((($s -split ' ') | ForEach-Object {
        if ($_ -match '^(ms|x|%|events/sec|events/s)$') { $_ }
        else { ($_ -replace "[$([char]0x00F8)$([char]0x00D8)eEaAoO$([char]0x00D3)$([char]0x00D2)]", '0') -replace '[Il|]', '1' }
    }) -join ' ')
}
# The "Events sent / dropped" cell of the 2 x 4 statistics grid (column 0, row 2); returns the text after the colon or ''.
function Read-EventsCell($pl) {
    $c = Ctl $pl 'Stats'
    if ($c -eq [IntPtr]::Zero) { return '' }
    for ($try = 0; $try -lt 3; $try++) {
        [void][WatchDriver]::ControlCellPicture($pl.H, $c, 0, 2, 2, 4, 3, $ocrPng)
        $text = Ocr-File $ocrPng
        $v = Fix-Num $(if ($text -match '^[^:]*:\s*(.*)$') { $Matches[1] } else { $text }).Trim()
        if ($v) { return $v }
        Wait-Ms 200
    }
    return ''
}

# ---------------------------------------------------------------- players
$script:Players = New-Object System.Collections.ArrayList
$script:Place = $null
function Compute-Placement {
    $w = [math]::Min(940, [int](($mon.Width - 30) / 2)); $h = [math]::Min(700, $mon.Height - 100)
    $y = $mon.Top + [int](($mon.Height - $h) / 2) - 20
    $script:Place = @{ Main = @(($mon.Left + 10), $y, $w, $h); Companion = @(($mon.Left + 20 + $w), $y, $w, $h) }
}
function On-Monitor([IntPtr]$h) {
    $r = [VisualDriver]::Rect($h)
    return ($r[0] -ge $mon.Left - 2 -and $r[1] -ge $mon.Top - 2 -and $r[2] -le $mon.Left + $mon.Width + 2 -and $r[3] -le $mon.Top + $mon.Height + 2)
}
function Stop-Pl($pl) {
    if (-not $pl.Alive) { return }
    $pl.Alive = $false
    try {
        $p = Get-Process -Id $pl.Pid -ErrorAction SilentlyContinue
        if ($p -and -not $p.HasExited) { [VisualDriver]::Close($pl.H); if (-not $p.WaitForExit(15000)) { $p.Kill(); [void]$p.WaitForExit(10000) } }
    } catch { }
}
function Close-All { foreach ($pl in @($script:Players)) { Stop-Pl $pl }; $script:Players.Clear() }
function Start-Player($def, [string]$file) {
    # v2 is placed explicitly with --position inside the chosen monitor (never left to the mouse); v1 has no such switch and
    # starts minimized, then PlaceNoActivate moves it onto the monitor below before it is ever shown.
    $argText = if ($def.Kind -eq 'v2') { '--no-settings --auto-close-ms 21600000 --position {0},{1} --open "{2}"' -f ($mon.Left + 40), ($mon.Top + 60), $file } else { '' }
    $pid2 = [VisualCapture]::Start($def.Exe, $argText, (Split-Path $def.Exe), $false)   # starts minimized (SW_SHOWMINNOACTIVE)
    $pl = @{ Name = $def.Name; Kind = $def.Kind; Pid = $pid2; H = [IntPtr]::Zero; Alive = $true; Known = (New-Object 'System.Collections.Generic.HashSet[long]') }
    [void]$script:Players.Add($pl)
    if ($def.Kind -eq 'v2') { $pl.H = [VisualDriver]::WaitWindow($pid2, [IntPtr]::Zero, 'MIDIBottleneckPlayerV2', '', 30000) }
    else { $pl.H = [VisualCapture]::WaitMainWindow($pid2, 'MIDIBottleneck Player', 30000) }
    if ($pl.H -eq [IntPtr]::Zero) { Stop-Pl $pl; throw "$($def.Name) main window did not appear" }
    $pr = $script:Place.Main
    [WatchDriver]::PlaceNoActivate($pl.H, $pr[0], $pr[1], $pr[2], $pr[3]); Wait-Ms 600
    if (-not (On-Monitor $pl.H)) { Stop-Pl $pl; throw "$($def.Name) window is not on the requested monitor (rect $([VisualDriver]::Rect($pl.H) -join ','))" }
    foreach ($w in [VisualDriver]::ProcessTopWindows($pid2, $false)) { [void]$pl.Known.Add($w.ToInt64()) }
    Log ("{0} started (pid {1}), placed {2}x{3} on {4}" -f $def.Name, $pid2, $pr[2], $pr[3], $mon.Device)
    return $pl
}
function Handle-Dialogs($pl) {   # dialogs that appear while loading (v1: large-file memory warning); 'ok' or 'failed'
    foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) {
        if ($w -eq $pl.H -or $pl.Known.Contains($w.ToInt64())) { continue }
        if ([VisualDriver]::Find($w, 'Edit', '', 1148) -ne [IntPtr]::Zero -or [VisualDriver]::Cls($w) -match 'tooltip') { continue }   # File Open dialog still closing / tooltip
        $title = [E2eDriver]::TextOf($w)
        Log ("$($pl.Name): a window appeared while loading: " + [E2eDriver]::AllText($w))
        $btnName = if ($title -like 'Large MIDI memory warning*' -and $AcceptLargeFileWarning) { 'Continue' } elseif ($title -like 'MIDI compatibility repair*') { 'OK' } else { '' }
        $b = if ($btnName) { [VisualDriver]::Find($w, 'Button', $btnName, -1) } else { [IntPtr]::Zero }
        if ($b -ne [IntPtr]::Zero) { [VisualDriver]::Click($b); [void]$pl.Known.Add($w.ToInt64()); Wait-Ms 500; continue }
        Log "$($pl.Name): unexpected window left open, load abandoned"
        return 'failed'
    }
    return 'ok'
}
function Load-File($pl, [string]$file) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($pl.Kind -eq 'v1') {   # through v1's File Open dialog
        $h = Main-Handle $pl
        $known = New-Object 'System.Collections.Generic.HashSet[long]'
        foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) { [void]$known.Add($w.ToInt64()) }
        [VisualDriver]::Click((Ctl $pl 'Open'))
        $dlg = [IntPtr]::Zero; $t = [Diagnostics.Stopwatch]::StartNew()
        while ($dlg -eq [IntPtr]::Zero -and $t.ElapsedMilliseconds -lt 10000) {
            foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) { if (-not $known.Contains($w.ToInt64()) -and $w -ne $h) { $dlg = $w; break } }
            if ($dlg -eq [IntPtr]::Zero) { Wait-Ms 100 }
        }
        if ($dlg -eq [IntPtr]::Zero) { throw 'v1 Open dialog did not appear' }
        Wait-Ms 700
        $edit = [IntPtr]::Zero; $t.Restart()
        while ($edit -eq [IntPtr]::Zero -and $t.ElapsedMilliseconds -lt 5000) { $edit = [VisualDriver]::Find($dlg, 'Edit', '', 1148); if ($edit -eq [IntPtr]::Zero) { Wait-Ms 100 } }
        if ($edit -eq [IntPtr]::Zero) { throw 'v1 Open dialog: file name box not found' }
        [VisualDriver]::SetText($edit, $file); Wait-Ms 200
        [VisualDriver]::Click([VisualDriver]::Find($dlg, 'Button', '', 1)); Wait-Ms 800
    }
    while ($sw.ElapsedMilliseconds -lt $LoadTimeoutMinutes * 60000) {
        if (-not (Get-Process -Id $pl.Pid -ErrorAction SilentlyContinue)) { throw "$($pl.Name) process ended while loading" }
        if ((Handle-Dialogs $pl) -eq 'failed') { return $false }
        $b = Ctl $pl 'Analysis'
        if ($b -ne [IntPtr]::Zero -and [VisualDriver]::Enabled($b)) { Wait-Ms 600; Log ("{0}: loaded in {1:N1} s" -f $pl.Name, ($sw.ElapsedMilliseconds / 1000)); return $true }
        Wait-Ms 250
    }
    return $false
}

# ---------------------------------------------------------------- companion windows
# Opens the companion window of one kind. Returns @{ H; Blocked; Note }.  channels = Analysis window -> its "Channels" button (v1 and v2 alike:
# v1 DiagnosticsForm.ChannelsRequested -> ShowChannelMonitor; v2 AnalysisWindow IdChannels); the Analysis window is closed again afterwards so that
# only the Channel Monitor remains. routing = the system-menu command 0x2030 (both players), a modal dialog.
function Known-Set($pl) { $s = New-Object 'System.Collections.Generic.HashSet[long]'; foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) { [void]$s.Add($w.ToInt64()) }; return , $s }
function Wait-New($pl, $known, [int]$timeoutMs = 10000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) { if (-not $known.Contains($w.ToInt64())) { return $w } }
        Wait-Ms 60
    }
    return [IntPtr]::Zero
}
function Open-Companion($pl, [string]$kind) {
    $r = @{ H = [IntPtr]::Zero; Blocked = $false; Note = '' }
    if ($kind -eq 'none') { return $r }
    $mainH = Main-Handle $pl
    $known = Known-Set $pl
    if ($kind -eq 'routing') {
        [VisualDriver]::SysCommand($mainH, 0x2030)
        $w = Wait-New $pl $known 5000
        if ($w -eq [IntPtr]::Zero) { $r.Blocked = $true; $r.Note = 'not available during playback (Track routing did not open)'; return $r }
        $r.H = $w; Wait-Ms 800
        if (-not [VisualDriver]::Enabled($mainH)) { $r.Blocked = $true; $r.Note = 'not available during playback (modal dialog disables the main window, so Play cannot be pressed; the menu item is stopped-only)' }
        return $r
    }
    [VisualDriver]::Click((Ctl $pl 'Analysis'))
    $a = Wait-New $pl $known 10000
    if ($a -eq [IntPtr]::Zero) { $r.Note = 'Analysis window did not open'; return $r }
    if ($kind -eq 'analysis') { $r.H = $a; Wait-Ms $SettleMs; return $r }
    Wait-Ms $SettleMs
    $btn = [VisualDriver]::WaitFind($a, 'Button', 'Channels', -1, 5000)
    if ($btn -eq [IntPtr]::Zero) { $r.Note = 'Channels button not found in the Analysis window'; [VisualDriver]::Close($a); return $r }
    $known2 = Known-Set $pl
    [VisualDriver]::Click($btn)
    $c = Wait-New $pl $known2 8000
    [VisualDriver]::Close($a); [void][E2eDriver]::WaitGone($a, 6000)
    if ($c -eq [IntPtr]::Zero) { $r.Note = 'Channel Monitor did not open'; return $r }
    if (-not [VisualDriver]::Alive($c)) { $r.Note = 'Channel Monitor closed together with the Analysis window'; return $r }
    $r.H = $c; Wait-Ms $SettleMs
    return $r
}
function Place-Companion([IntPtr]$h) {
    $r = [VisualDriver]::Rect($h); $pr = $script:Place.Companion
    [WatchDriver]::PlaceNoActivate($h, $pr[0], $pr[1], [math]::Min($r[2] - $r[0], $pr[2]), [math]::Min($r[3] - $r[1], $pr[3])); Wait-Ms 400
    return (On-Monitor $h)
}
function Close-Companion($comp) {
    if ($comp.H -eq [IntPtr]::Zero -or -not [VisualDriver]::Alive($comp.H)) { return }
    $c = [VisualDriver]::Find($comp.H, 'Button', '=Cancel', -1)
    if ($c -ne [IntPtr]::Zero) { [VisualDriver]::Click($c) } else { [VisualDriver]::Close($comp.H) }
    if (-not [E2eDriver]::WaitGone($comp.H, 6000)) { [VisualDriver]::Close($comp.H); if (-not [E2eDriver]::WaitGone($comp.H, 6000)) { throw 'companion window would not close' } }
    Wait-Ms 500
}

# ---------------------------------------------------------------- one round of one window kind on one player
function Add-Row($def, [string]$kind, [int]$round, $f) {
    $row = [pscustomobject]$f
    [void]$script:Rows.Add($row)
    Add-Content -LiteralPath $csvPath -Value (Csv-Row @($def.Name, $def.Build, $kind, $round, $row.events, $row.play_ms, $row.meps, $row.main_max, $row.main_p95, $row.comp_max, $row.cpu_before, $row.cpu_after, $row.perf, $planName, $row.status, $row.sent, $row.dropped, $row.notes)) -Encoding UTF8
    $row | Add-Member -NotePropertyName Prog -NotePropertyValue $def.Name -PassThru | Add-Member -NotePropertyName Kind -NotePropertyValue $kind | Out-Null
}
$script:NA = @{}
function Measure-Round($pl, $def, [string]$kind, [int]$round, [int64]$events) {
    $f = @{ events = $events; play_ms = ''; meps = ''; main_max = ''; main_p95 = ''; comp_max = ''; cpu_before = ''; cpu_after = ''; perf = ''; status = 'OK'; sent = ''; dropped = ''; notes = '' }
    $naKey = "$($def.Name)|$kind"
    if ($script:NA.ContainsKey($naKey)) { $f.status = 'NA'; $f.notes = $script:NA[$naKey]; Add-Row $def $kind $round $f; return }
    [VisualDriver]::Click((Ctl $pl 'Stop'))
    if ((Wait-State $pl @('Stopped') 30000) -ne 'Stopped') { $f.status = 'FAIL'; $f.notes = "could not reach Stopped (state '$(State-Text $pl)')"; Fail "$($def.Name)/$kind/r$round" $f.notes; Add-Row $def $kind $round $f; return }
    $rs = Ctl $pl 'Reset'; if ($rs -ne [IntPtr]::Zero) { [VisualDriver]::Click($rs); Wait-Ms 300 }
    $comp = @{ H = [IntPtr]::Zero }
    try {
        $comp = Open-Companion $pl $kind
        if ($comp.Blocked) { $f.status = 'NA'; $f.notes = $comp.Note; $script:NA[$naKey] = $comp.Note; Log "$($def.Name)/${kind}: $($comp.Note)"; Add-Row $def $kind $round $f; return }
        if ($kind -ne 'none') {
            if ($comp.H -eq [IntPtr]::Zero) { $f.status = 'FAIL'; $f.notes = $comp.Note; Fail "$($def.Name)/$kind/r$round" $comp.Note; Add-Row $def $kind $round $f; return }
            if (-not (Place-Companion $comp.H)) { $f.status = 'FAIL'; $f.notes = 'companion window is not on the requested monitor'; Fail "$($def.Name)/$kind/r$round" $f.notes; Add-Row $def $kind $round $f; return }
        }
        $c0 = Sample-Cpu 2
        $f.cpu_before = $c0.Busy; $f.perf = $c0.Perf
        if ($c0.Busy -gt 20) { $f.notes += "BUSY before play ($($c0.Busy) %); " }
        # the gate, immediately before Play: None, KDMAPI off, both read back; a failure stops the whole script
        Set-NoneOutput $pl
        Assert-OutputGate $pl
        $play = Ctl $pl 'Play'; $state = Ctl $pl 'State'
        $res = ([WtSampler]::Run($play, $state, (Main-Handle $pl), $comp.H, $TimeoutSeconds * 1000)) -split '\|'
        $playMs = [double]$res[1]; $final = $res[2]
        $f.play_ms = [int]$playMs; $f.meps = if ($playMs -gt 0 -and $events -gt 0) { [math]::Round($events / $playMs / 1000.0, 3) } else { '' }
        $f.main_max = $res[3]; $f.main_p95 = $res[4]
        if ($kind -ne 'none') { $f.comp_max = $res[5] }
        if ($final -like 'TIMEOUT*') { $f.status = 'FAIL'; $f.notes += "no end of playback within $TimeoutSeconds s ($final); "; Fail "$($def.Name)/$kind/r$round" $f.notes }
        elseif ($playMs -lt 150) { $f.status = 'FAIL'; $f.notes += "playback ended after $playMs ms (the position was not at 0?); "; Fail "$($def.Name)/$kind/r$round" $f.notes }
        if (-not $NoOcr -and $final -eq 'Completed') {
            try {
                $ev = Read-EventsCell $pl
                if ($ev -match '^\s*([\d.,\s]+?)\s*[/|]\s*([\d.,\s]+?)\s*$') { $f.sent = $Matches[1]; $f.dropped = $Matches[2] } else { $f.sent = $ev; if (-not $ev) { $f.notes += 'events sent/dropped not readable; ' } }
            } catch { $f.notes += "OCR failed: $($_.Exception.Message); " }
        }
        [VisualDriver]::Click((Ctl $pl 'Stop')); [void](Wait-State $pl @('Stopped') 30000)
        $c1 = Sample-Cpu 2; $f.cpu_after = $c1.Busy
        Log ("{0,-7} {1,-9} r{2}: play {3} ms = {4} Meps; main ping max {5} / p95 {6} ms; companion ping max {7} ms; sent/dropped {8}/{9}; cpu {10} -> {11} %, perf {12} %" -f $def.Name, $kind, $round, $f.play_ms, $f.meps, $f.main_max, $f.main_p95, $f.comp_max, $f.sent, $f.dropped, $f.cpu_before, $f.cpu_after, $f.perf)
        Add-Row $def $kind $round $f
    }
    finally {
        try { Close-Companion $comp } catch { Fail "$($def.Name)/$kind/r$round" "closing the companion window: $($_.Exception.Message)" }
    }
}

# ---------------------------------------------------------------- summary.md
function Median($values) {
    $a = @($values | Where-Object { $null -ne $_ -and "$_" -ne '' } | ForEach-Object { [double]$_ } | Sort-Object)
    if ($a.Count -eq 0) { return $null }
    if ($a.Count % 2) { return $a[($a.Count - 1) / 2] } else { return ($a[$a.Count / 2 - 1] + $a[$a.Count / 2]) / 2 }
}
function Fmt($v, [string]$f = '0.##') { if ($null -eq $v) { return '-' }; return ([double]$v).ToString($f, $inv) }
function Write-Summary([string]$verdict, $cpuNotes) {
    $L = New-Object System.Collections.ArrayList
    [void]$L.Add("# Window throughput - $tag ($stamp)")
    [void]$L.Add('')
    [void]$L.Add("- Verdict: $verdict")
    [void]$L.Add("- Power plan: **$planName** (every figure below was measured on this plan)")
    [void]$L.Add("- Fixture: $($script:Fx.Path); events $($script:Fx.Events) ($($script:Fx.Source)); interleaved by window kind")
    foreach ($n in $script:RoundNotes) { [void]$L.Add("- $n") }
    [void]$L.Add("- Monitor: $($mon.Device) $($mon.Width)x$($mon.Height) at $($mon.Left),$($mon.Top); output None, KDMAPI off for every Play")
    foreach ($d in $progDefs) { [void]$L.Add("- $($d.Name): $($d.Exe) (build $($d.Build))") }
    foreach ($n in $cpuNotes) { [void]$L.Add("- $n") }
    [void]$L.Add('')
    [void]$L.Add('Medians over the rounds. "slowdown" = play_ms of this window divided by play_ms of the same program with no window (1.00 = no effect; 10 = ten times slower). Ping figures are the median of the per-round maximum / 95th percentile reply time of a WM_NULL message.')
    [void]$L.Add('')
    [void]$L.Add('| program | window | n | play_ms | Meps | slowdown vs none | Meps ratio vs none | main ping max ms | main ping p95 ms | companion ping max ms | events sent / dropped |')
    [void]$L.Add('|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($d in $progDefs) {
        $base = @($script:Rows | Where-Object { $_.Prog -eq $d.Name -and $_.Kind -eq 'none' -and $_.status -eq 'OK' })
        $baseMs = Median ($base | ForEach-Object { $_.play_ms }); $baseMeps = Median ($base | ForEach-Object { $_.meps })
        foreach ($k in $Windows) {
            $rows = @($script:Rows | Where-Object { $_.Prog -eq $d.Name -and $_.Kind -eq $k })
            if ($rows.Count -eq 0) { continue }
            $ok = @($rows | Where-Object { $_.status -eq 'OK' })
            if ($ok.Count -eq 0) {
                $why = ($rows | Select-Object -First 1).notes
                $word = if (($rows | Select-Object -First 1).status -eq 'NA') { 'n/a' } else { 'FAILED' }
                [void]$L.Add("| $($d.Name) | $k | 0 | $word | | | | | | | $why |"); continue
            }
            $ms = Median ($ok | ForEach-Object { $_.play_ms }); $me = Median ($ok | ForEach-Object { $_.meps })
            $slow = if ($baseMs) { Fmt ($ms / $baseMs) '0.00' } else { '-' }
            $mr = if ($baseMeps) { Fmt ($me / $baseMeps) '0.00' } else { '-' }
            $sd = ($ok | ForEach-Object { if ($_.sent) { "$($_.sent) / $($_.dropped)" } } | Select-Object -First 1)
            [void]$L.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |" -f $d.Name, $k, $ok.Count, (Fmt $ms '0'), (Fmt $me '0.000'), $slow, $mr,
                (Fmt (Median ($ok | ForEach-Object { $_.main_max })) '0.0'), (Fmt (Median ($ok | ForEach-Object { $_.main_p95 })) '0.0'), $(if ($k -eq 'none') { '-' } else { Fmt (Median ($ok | ForEach-Object { $_.comp_max })) '0.0' }), $sd))
        }
    }
    if ($script:Fails.Count -gt 0) { [void]$L.Add(''); [void]$L.Add('## Problems'); foreach ($x in $script:Fails) { [void]$L.Add("- $x") } }
    Set-Content -LiteralPath $sumPath -Value $L -Encoding UTF8
}

# ================================================================================================ main
$cpuNotes = New-Object System.Collections.ArrayList
$verdict = 'FAIL'
$adp = Get-AdaptivePlan -Explicit $PSBoundParameters.ContainsKey('Rounds') -ExplicitCount $Rounds
try {
    Log ("window-throughput: programs {0}; windows {1}; rounds {2}; power plan {3}; results in {4}" -f ($Programs -join ','), ($Windows -join ','), $Rounds, $planName, $OutDir)
    $start = Sample-Cpu 3
    Log ("PC at start: CPU {0} %, processor performance {1} %, plan {2}" -f $start.Busy, $start.Perf, $planName)
    if ($start.Busy -gt 20 -and -not $IgnoreBusy) { throw "the PC is busy (CPU $($start.Busy) %, above 20 %): no timing measurement now (use -IgnoreBusy to override)" }
    $script:Fx = Resolve-Fixture $true
    Log ("fixture {0}: {1} events ({2})" -f $script:Fx.Path, $script:Fx.Events, $script:Fx.Source)
    if ($script:Fx.Events -le 0) { throw 'event count of the fixture is unknown' }
    if (-not $NoOcr) { Init-Ocr }
    Compute-Placement
    foreach ($def in $progDefs) {
        Log "==== $($def.Name) ($($def.Build))"
        $c0 = Sample-Cpu 2
        [void]$cpuNotes.Add(("{0}: set start CPU {1} %, processor performance {2} %, plan {3}" -f $def.Name, $c0.Busy, $c0.Perf, $planName))
        $pl = Start-Player $def $script:Fx.Path
        if (-not (Load-File $pl $script:Fx.Path)) { Fail "$($def.Name)" 'the file did not load'; Close-All; continue }
        Set-NoneOutput $pl; Assert-OutputGate $pl   # (the gate runs again before every Play)
        Log "$($def.Name): output None verified, KDMAPI off"
        $round = 0; $adpReason = ''
        while ($true) {
            $round++
            foreach ($kind in $Windows) { Measure-Round $pl $def $kind $round $script:Fx.Events }
            if ($round -lt $adp.Min) { continue }
            if ($adp.Explicit -or $script:Fails.Count -gt 0) { break }
            # Comparison being made: play time with no window against play time with each companion window.
            $adpSamples = @{}; $adpPairs = @()
            foreach ($kind in $Windows) {
                $adpSamples[$kind] = @($script:Rows | Where-Object { $_.Prog -eq $def.Name -and $_.Kind -eq $kind -and $_.status -eq 'OK' -and $_.play_ms -ne '' } | ForEach-Object { [double]$_.play_ms })
                if ($kind -ne 'none' -and $Windows -contains 'none') { $adpPairs += , @('none', $kind) }
            }
            $verdict = Get-AdaptiveVerdict -Samples $adpSamples -Pairs $adpPairs -Done $round -Max $adp.Max
            if (-not $verdict.Extend) { $adpReason = $verdict.Reason; break }
        }
        $note = Format-AdaptiveNote -Used $round -Explicit $adp.Explicit -Reason $adpReason -Name $def.Name
        [void]$script:RoundNotes.Add($note); Log $note
        $c1 = Sample-Cpu 2
        [void]$cpuNotes.Add(("{0}: set end CPU {1} %, processor performance {2} %, plan {3}" -f $def.Name, $c1.Busy, $c1.Perf, $planName))
        Close-All; Wait-Ms 1500
    }
}
catch { Fail 'run' ("exception: " + $_.Exception.Message) }
finally {
    Close-All
    Remove-Item -LiteralPath $ocrPng -Force -ErrorAction SilentlyContinue
}
$verdict = if ($script:Fails.Count -eq 0) { 'DONE' } else { 'FAIL' }
if (-not $script:Fx) { $script:Fx = @{ Path = $fx.Path; Events = $fx.Events; Source = $fx.Source } }
Write-Summary $verdict $cpuNotes
Log "results: $csvPath ; $sumPath"
if ($script:Fails.Count -gt 0) {
    foreach ($x in $script:Fails) { Write-Host "  - $x" }
    Write-Host ("WINDOW-THROUGHPUT: FAIL ({0} problems)" -f $script:Fails.Count)
    exit 1
}
Write-Host ("WINDOW-THROUGHPUT: DONE ({0} rows)" -f $script:Rows.Count)
exit 0
