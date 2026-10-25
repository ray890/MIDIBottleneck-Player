# Measures the live-display refresh (DD-035 to DD-038): for each pacing scheme, plays a fixture on output None
# for a fixed time and reads the player's own --report: UI-thread and process CPU, the evenness of the live
# updates (mean / median / 99th percentile / maximum interval), UI time per update, and the playback progress
# (events processed in the fixed time; completion time when the file is short).
#   .\v2\tools\refresh-pacing-measure.ps1 [-Build x64|x86|xp] [-Runs 5] [-Seconds 15] [-Fixture path] [-Out file.csv] [-SmokeTest]
# It opens a player window for every run: run it on a hidden desktop or when that is acceptable.
# It waits for a quiet PC (total CPU under 20 %) before each run; -SmokeTest skips the wait and labels the output
# "under load, not a result". Conditions (power plan, CPU load before each run, processor clock, monitor refresh
# rate) are printed with the numbers.
# Schemes: old-16ms (the previous 16 ms Windows timer), display (monitor refresh via DwmFlush), display-cap60
# (the same, at most about 60 per second), timer-60 (precise 60 per second timer: the fallback), timer-144
# (what a 144 Hz monitor would cost with the timer source; the monitor here may run slower).
param(
    [ValidateSet('x64', 'x86', 'xp')][string]$Build = 'x64',
    [int]$Runs = 3,           # without -Runs: 3 runs, plus one at a time up to 7 while a scheme's UI CPU spreads over 10 % (adaptive-repeats.ps1); -Runs N = exactly N
    [int]$Seconds = 15,
    [string]$Fixture = 'v2\harness\fixtures\large\large-8m.mid',
    [string]$Out = '',
    [string]$Position = '3880,1133',   # 'X,Y': where the player opens (--position): inside Internal Display (\\.\DISPLAY1 at 3840,1073). Never leave it to the default monitor.
    [switch]$SmokeTest
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$exe = Join-Path $repo "v2\out\build-$Build\Release\midibn-win32.exe"
$fixturePath = if ([System.IO.Path]::IsPathRooted($Fixture)) { $Fixture } else { Join-Path $repo $Fixture }
if (-not (Test-Path $exe)) { throw "missing $exe (build it with v2\tools\build-player.ps1)" }
if (-not (Test-Path $fixturePath)) { throw "missing fixture $fixturePath" }
$temp = Join-Path $env:TEMP 'refresh-pacing'
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$schemes = [ordered]@{
    'old-16ms'      = @('--refresh-ms', '16')
    'display'       = @('--pace', 'display')
    'display-cap60' = @('--pace', 'display', '--pace-max-hz', '60')
    'timer-60'      = @('--pace', 'timer', '--pace-hz', '60')
    'timer-144'     = @('--pace', 'timer', '--pace-hz', '144')
}

function Get-CpuLoad {
    $s = (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples | ForEach-Object { $_.CookedValue }
    return [math]::Round(($s | Measure-Object -Average).Average, 1)
}
function Wait-Quiet {
    if ($SmokeTest) { return Get-CpuLoad }
    for ($try = 0; $try -lt 360; $try++) {
        $load = Get-CpuLoad
        if ($load -lt 20) { return $load }
        Start-Sleep -Seconds 5
    }
    throw 'the PC did not become quiet (total CPU stayed above 20 %) within 30 minutes'
}
function Get-Median($values) {
    $v = @($values | Where-Object { $_ -ne $null } | Sort-Object)
    if ($v.Count -eq 0) { return $null }
    return $v[[int][math]::Floor(($v.Count - 1) / 2)]
}

$plan = (powercfg /getactivescheme) -replace '^.*:\s*', ''
$monitors = Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentRefreshRate } | ForEach-Object { "$($_.Name) $($_.CurrentRefreshRate) Hz" }
$title = if ($SmokeTest) { 'UNDER LOAD, NOT A RESULT (smoke test)' } else { 'measurement' }
Write-Host "=== refresh pacing: $title"
Write-Host "build=$Build runs=$Runs seconds=$Seconds fixture=$(Split-Path -Leaf $fixturePath)"
Write-Host "power plan: $plan"
Write-Host "monitor refresh rates: $($monitors -join '; ')"
Write-Host "time: $(Get-Date -Format 'yyyy-MM-dd hh:mm tt')"

. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')
$adp = Get-AdaptivePlan -Explicit $PSBoundParameters.ContainsKey('Runs') -ExplicitCount $Runs
$adpReason = ''
$rows = @()
$run = 0
while ($true) {
    $run++
    foreach ($name in $schemes.Keys) {
        $load = Wait-Quiet
        $clock = [math]::Round((Get-Counter '\Processor Information(_Total)\% Processor Performance' -SampleInterval 1 -MaxSamples 1).CounterSamples[0].CookedValue, 0)
        $report = Join-Path $temp "$name-$run.txt"
        Remove-Item $report -ErrorAction SilentlyContinue
        $arguments = @('--no-settings', '--open', "`"$fixturePath`"", '--play', '--rate', '0', '--auto-close-ms', ($Seconds * 1000), '--report', "`"$report`"") + $schemes[$name]
    if ($Position) { $arguments += @('--position', $Position) }
        if (-not $Position) { throw 'a test launch must place the window: pass -Position X,Y (inside Internal Display, e.g. 3880,1133)' }
        $p = Start-Process $exe -ArgumentList $arguments -PassThru
        if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { $p.Kill() }
        $t = @{}
        if (Test-Path $report) {
            foreach ($line in Get-Content $report) { foreach ($m in [regex]::Matches($line, '(\w+)=([-\d.]+)')) { $t[$m.Groups[1].Value] = [double]$m.Groups[2].Value } }
        }
        $rows += [pscustomobject]@{
            run = $run; scheme = $name; cpu_before = $load; clock_pct = $clock
            ui_cpu_ms = $t['ui_thread_cpu_ms']; process_cpu_ms = $t['process_cpu_ms']; play_ms = $t['play_ms']; events = $t['events_processed']
            updates = $t['refresh_updates']; mean_ms = $t['mean']; sd_ms = $t['sd']; p50_ms = $t['p50']; p99_ms = $t['p99']; max_ms = $t['max']
            ui_ms_per_update = $t['per_update']; coalesced = $t['pacer_coalesced']
        }
    }
    if ($run -lt $adp.Min) { continue }
    if ($adp.Explicit) { break }
    $adpSamples = @{}
    foreach ($name in $schemes.Keys) { $adpSamples[$name] = @($rows | Where-Object { $_.scheme -eq $name -and $null -ne $_.ui_cpu_ms } | ForEach-Object { [double]$_.ui_cpu_ms }) }
    $verdict = Get-AdaptiveVerdict -Samples $adpSamples -Done $run -Max $adp.Max
    if (-not $verdict.Extend) { $adpReason = $verdict.Reason; break }
}
$Runs = $run
Write-Host (Format-AdaptiveNote -Used $run -Explicit $adp.Explicit -Reason $adpReason)
if ($Out) { $rows | Export-Csv -NoTypeInformation -Path $Out }

Write-Host ''
Write-Host "--- medians over $Runs runs ($title); cpu load before runs: $((($rows | Measure-Object cpu_before -Minimum -Maximum) | ForEach-Object { "$($_.Minimum)-$($_.Maximum) %" })), clock $((($rows | Measure-Object clock_pct -Minimum -Maximum) | ForEach-Object { "$($_.Minimum)-$($_.Maximum) %" })) of nominal"
$summary = foreach ($name in $schemes.Keys) {
    $g = @($rows | Where-Object { $_.scheme -eq $name })
    [pscustomobject]@{
        scheme = $name
        updates = Get-Median ($g.updates)
        ui_cpu_ms = Get-Median ($g.ui_cpu_ms)
        process_cpu_ms = Get-Median ($g.process_cpu_ms)
        ui_ms_per_update = Get-Median ($g.ui_ms_per_update)
        mean_ms = Get-Median ($g.mean_ms)
        p50_ms = Get-Median ($g.p50_ms)
        p99_ms = Get-Median ($g.p99_ms)
        max_ms = Get-Median ($g.max_ms)
        events = Get-Median ($g.events)
        play_ms = Get-Median ($g.play_ms)
        coalesced = Get-Median ($g.coalesced)
    }
}
$summary | Format-Table -AutoSize | Out-String -Width 250 | Write-Host
if ($SmokeTest) { Write-Host 'UNDER LOAD, NOT A RESULT' }
