# End-to-end playback throughput and timing of the v2 engine on outputs that make no sound (Milestone 7, DD-065).
#
# "End to end" = the engine's scheduling, its statistics and its send to the output, on the real clock with the real
# worker thread (midibn-device-measure `run`), so the figure is what a dense song gets, minus only the real device's own
# cost per send. This script opens NO MIDI device and loads no provider DLL: the only outputs it can use are
#   null       does nothing (the engine's own cost)
#   count      counts every message and folds it into an order-dependent hash (the hash proves that two engines send the
#              same messages in the same order)
#   recording  keeps every message in memory (the test double the engine tests use)
# Real devices (WinMM, KDMAPI, winmmwrp) are measured by v2\tools\device\run-device-measurements.ps1, which runs the
# same midibn-device-measure.exe and so uses the same engine path.
#
# Interleaving: with -Baseline <older midibn-device-measure.exe> every set runs A (baseline), B (current), A, B, ...
# (-Rounds times each, each run itself -Repeats plays after one warm-up play) and the report gives medians. Without
# -Baseline only the current build runs. The PC is usually shared: every set records the power plan, the whole-PC CPU
# load before and during the runs and the processor clock (% Processor Performance), and the report says
# "under load (interleaved)" unless the whole-PC load was under -QuietCpuPercent (default 20 %), in which case it
# says "quiet PC".
#
# Sets (-Sets):
#   overcap          12 million events due faster than any path can take them, Queue mode, null output, no wrapper:
#                    maximum sustained throughput (million events per second)
#   overcap-drop     the same in Drop mode (finite queue, never full here)
#   overcap-count    the same into the counting output: throughput and the message hash (A and B must agree)
#   overcap-recording the same, 3 million events, into the recording output
#   late-1M          clustered song, 1 million events per second for 3 s, null output, every 128th send timed:
#                    lateness of the sends (p50 / p99 / p99.9 / max in microseconds), the timing-precision check
#   late-9.5M        clustered song at 9.5 million per second for 1.5 s: lateness near the density of the v1 figure
# Open windows, emulated without a window (prompt 009 bugs 4.2 and 4.3; register entries DD-092 and DD-093):
#   overcap-monitor     the overcap set with channel monitoring on (what the MIDI Channel Monitor window turns on)
#   overcap-monitor-ui  the same, plus the display refresh's statistics calls 60 times a second on a second thread
#   paced-ui            a silent output that takes at most 15 million messages per second (a fast real device), a song
#                       at 25 million per second (so a backlog builds up), the display refresh at 60 Hz: the reference
#   paced-override-ui   the same with monitoring on and a forced override active (bug 4.3: the player stopped answering
#                       and sent in chunks); reported: how long the refresh's calls took, how late the refreshes came and
#                       the longest stretch with nothing sent
#   These sets need a midibn-device-measure that knows --monitor / --override / --ui-hz / --output paced; to measure an
#   older engine, build that older engine with this tool's source (the options use only the engine's public interface).
#
# Usage (from the repository root, Windows PowerShell):
#   .\v2\tools\throughput-measure.ps1                                   # current x64 build, all sets
#   .\v2\tools\throughput-measure.ps1 -Baseline D:\old\midibn-device-measure.exe -Rounds 5
#   .\v2\tools\throughput-measure.ps1 -Arch x86 -Sets overcap,late-1M
#   .\v2\tools\throughput-measure.ps1 -Build                            # builds midibn-device-measure first
#   .\v2\tools\throughput-measure.ps1 -Quick                            # short fixtures, 2 rounds: a smoke test
# Results: v2\out\throughput\results\<stamp>\ (summary.md, runs.csv, env.txt; untracked).
param(
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    [string]$Exe = '',
    [string]$Baseline = '',
    [string[]]$Sets = @('overcap', 'overcap-drop', 'overcap-count', 'overcap-recording', 'late-1M', 'late-9.5M'),
    [int]$Rounds = 3,   # 3 rounds, plus one at a time up to 7 while a set is unclear (adaptive-repeats.ps1); an explicit -Rounds N is exact
    [int]$Repeats = 3,
    [switch]$Build,
    [switch]$Quick,
    [string]$Label = '',
    [string]$ResultsRoot = '',
    [double]$QuietCpuPercent = 20
)
$ErrorActionPreference = 'Stop'
$Sets = @($Sets | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo
$validSets = 'overcap', 'overcap-drop', 'overcap-count', 'overcap-recording', 'late-1M', 'late-9.5M',
    'overcap-monitor', 'overcap-monitor-ui', 'paced-ui', 'paced-override-ui'
foreach ($s in $Sets) { if ($validSets -notcontains $s) { throw "unknown set '$s' (known: $($validSets -join ', '))" } }

if ($Build) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -property installationPath
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $bdir = Join-Path $repo "v2\out\build-$Arch"
    $platform = if ($Arch -eq 'x86') { 'Win32' } else { 'x64' }
    if (-not (Test-Path (Join-Path $bdir 'CMakeCache.txt'))) { & $cmake -S v2 -B $bdir -G 'Visual Studio 17 2022' -A $platform -T v143 | Out-Null }
    & $cmake --build $bdir --config Release --target midibn-device-measure --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
if (-not $Exe) { $Exe = Join-Path $repo "v2\out\build-$Arch\Release\midibn-device-measure.exe" }
if (-not (Test-Path -LiteralPath $Exe)) { throw "missing $Exe (run with -Build)" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
if ($Baseline) {
    if (-not (Test-Path -LiteralPath $Baseline)) { throw "missing baseline $Baseline" }
    $Baseline = (Resolve-Path -LiteralPath $Baseline).Path
}
. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')
$plan = Get-AdaptivePlan -Explicit ($PSBoundParameters.ContainsKey('Rounds') -or [bool]$Quick) -ExplicitCount $Rounds
if ($Quick) { $Rounds = 2; $Repeats = 2; $plan = Get-AdaptivePlan -Explicit $true -ExplicitCount 2 }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $ResultsRoot) { $ResultsRoot = Join-Path $repo 'v2\out\throughput\results' }
$resultDir = Join-Path $ResultsRoot ($stamp + $(if ($Label) { "-$Label" } else { '' }))
$rawDir = Join-Path $resultDir 'raw'
$fixtureDir = Join-Path $repo 'v2\out\throughput\fixtures'
New-Item -ItemType Directory -Force -Path $rawDir, $fixtureDir | Out-Null

# ---------------------------------------------------------------- fixtures
# name -> kind, events per second, seconds (clustered: 960 ticks per quarter at 120 BPM, all events of a tick at one instant)
$scale = if ($Quick) { 0.25 } else { 1.0 }
$setup = @{
    'overcap'           = @{ Output = 'null';      Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 80000000; Seconds = 0.15 * $scale; Metric = 'throughput' }
    'overcap-drop'      = @{ Output = 'null';      Mode = 'drop';  Instr = 'none';   Kind = 'clustered'; Rate = 80000000; Seconds = 0.15 * $scale; Metric = 'throughput' }
    'overcap-count'     = @{ Output = 'count';     Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 80000000; Seconds = 0.15 * $scale; Metric = 'throughput' }
    'overcap-recording' = @{ Output = 'recording'; Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 80000000; Seconds = 0.04 * $scale; Metric = 'throughput' }
    'late-1M'           = @{ Output = 'null';      Mode = 'queue'; Instr = 'sample'; Kind = 'clustered'; Rate = 1000000;  Seconds = 3 * $scale;    Metric = 'lateness' }
    'late-9.5M'         = @{ Output = 'null';      Mode = 'queue'; Instr = 'sample'; Kind = 'clustered'; Rate = 9500000;  Seconds = 1.5 * $scale;  Metric = 'lateness' }
    'overcap-monitor'    = @{ Output = 'null';     Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 80000000; Seconds = 0.15 * $scale; Metric = 'throughput'; Extra = @('--monitor', '1') }
    'overcap-monitor-ui' = @{ Output = 'null';     Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 80000000; Seconds = 0.15 * $scale; Metric = 'ui'; Extra = @('--monitor', '1', '--ui-hz', '60') }
    'paced-ui'           = @{ Output = 'paced';    Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 25000000; Seconds = 0.5 * $scale;  Metric = 'ui'; Extra = @('--paced-mps', '15', '--ui-hz', '60') }
    'paced-override-ui'  = @{ Output = 'paced';    Mode = 'queue'; Instr = 'none';   Kind = 'clustered'; Rate = 25000000; Seconds = 0.5 * $scale;  Metric = 'ui'; Extra = @('--paced-mps', '15', '--monitor', '1', '--override', '1', '--ui-hz', '60') }
}
foreach ($s in $Sets) { if (@('null', 'count', 'recording', 'paced') -notcontains $setup[$s].Output) { throw 'only silent outputs are allowed here' } }

function Get-Fixture([hashtable]$spec) {
    $name = '{0}-{1}M-{2}s.mid' -f $spec.Kind, ([double]$spec.Rate / 1e6), $spec.Seconds
    $path = Join-Path $fixtureDir $name
    if (-not (Test-Path $path)) { & $Exe gen $path $spec.Kind ([string]$spec.Rate) ([string]$spec.Seconds) | Out-Null }
    return $path
}

# ---------------------------------------------------------------- conditions
function Get-Conditions {
    $c = Get-Counter '\Processor(_Total)\% Processor Time', '\Processor Information(_Total)\% Processor Performance' -SampleInterval 1 -MaxSamples 3
    $cpu = @($c | ForEach-Object { $_.CounterSamples | Where-Object { $_.Path -like '*% processor time' } | ForEach-Object { $_.CookedValue } })
    $perf = @($c | ForEach-Object { $_.CounterSamples | Where-Object { $_.Path -like '*% processor performance' } | ForEach-Object { $_.CookedValue } })
    $plan = (powercfg /getactivescheme) -replace '^Power Scheme GUID:\s*', ''
    return @{ Cpu = [math]::Round(($cpu | Measure-Object -Average).Average, 1); Perf = [math]::Round(($perf | Measure-Object -Average).Average, 0); Plan = $plan }
}
function Median($values) {
    $v = @($values | Sort-Object)
    if ($v.Count -eq 0) { return [double]::NaN }
    if ($v.Count % 2 -eq 1) { return $v[($v.Count - 1) / 2] }
    return ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2
}
function Parse-Lines($lines) {
    $h = @{}
    foreach ($l in $lines) { if ($l -match '^([A-Za-z0-9_]+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }
    return $h
}

$cond0 = Get-Conditions
$cpuInfo = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$os = Get-CimInstance Win32_OperatingSystem
$envLines = @(
    "# throughput-measure $stamp $Label",
    "exe=$Exe", "baseline=$Baseline", "arch=$Arch", "sets=$($Sets -join ',')", "rounds=$(if ($plan.Explicit) { $plan.Min } else { 'adaptive 3-7' })", "repeats=$Repeats", "quick=$Quick",
    "cpu=$cpuInfo", "logical_cpus=$((Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors)", "os=$($os.Caption) $($os.Version)",
    "power_plan=$($cond0.Plan)", "cpu_load_pct_at_start=$($cond0.Cpu)", "processor_performance_pct_at_start=$($cond0.Perf)"
)
Set-Content -Path (Join-Path $resultDir 'env.txt') -Value $envLines

# ---------------------------------------------------------------- runs
$programs = @()
if ($Baseline) { $programs += , @('A', 'baseline', $Baseline) }
$programs += , @($(if ($Baseline) { 'B' } else { 'A' }), 'current', $Exe)

$rows = New-Object System.Collections.ArrayList   # one row per play
$summary = New-Object System.Collections.ArrayList
[void]$summary.Add("# Throughput and timing, $stamp $Label")
[void]$summary.Add('')
[void]$summary.Add("Arch $Arch; CPU: $cpuInfo; power plan: $($cond0.Plan)")
[void]$summary.Add("Programs: " + (($programs | ForEach-Object { "$($_[0]) = $($_[1]) ($($_[2]))" }) -join '; '))
[void]$summary.Add('Outputs used: null / count / recording only (no MIDI device is opened, no sound).')
[void]$summary.Add('')

foreach ($setName in $Sets) {
    $spec = $setup[$setName]
    $fixture = Get-Fixture $spec
    $before = Get-Conditions
    $setRows = New-Object System.Collections.ArrayList
    Write-Host "== $setName ($($spec.Output), $($spec.Mode), $([System.IO.Path]::GetFileName($fixture)))"
    $round = 0
    $roundNote = ''
    while ($true) {
        $round++
        foreach ($p in $programs) {
            $label = '{0}-{1}-r{2}' -f $setName, $p[0], $round
            $extra = if ($spec.Extra) { [string[]]$spec.Extra } else { @() }
            $out = & $p[2] run --output $spec.Output --fixture $fixture --out $rawDir --label $label --repeats $Repeats --instr $spec.Instr --warm 1 --settle-ms 200 --mode $spec.Mode @extra
            if ($LASTEXITCODE -ne 0) { throw "$($p[1]) failed on $setName" }
            for ($k = 1; $k -le $Repeats; $k++) {
                $info = Parse-Lines (Get-Content (Join-Path $rawDir "$label.r$k.run.txt"))
                $row = [ordered]@{ set = $setName; program = $p[0]; round = $round; repeat = $k; events = $info['events']; processed = $info['processed_events']
                    dropped = $info['dropped_events']; ended_ms = $info['ended_ms']; meps = $info['throughput_over_ended_meps']
                    cpu_total_pct = $info['cpu_total_busy_pct']; cpu_other_pct = $info['cpu_other_processes_pct']; hash = $info['output_hash']
                    messages = $(if ($info['output_shorts']) { $info['output_shorts'] } else { $info['output_messages'] }); engine_max_lag_us = $info['engine_max_lag_us']
                    ui_call_max_ms = $info['ui_call_max_ms']; ui_call_p99_ms = $info['ui_call_p99_ms']; ui_tick_gap_max_ms = $info['ui_tick_gap_max_ms']
                    send_stall_max_ms = $info['send_stall_max_ms']; emulated_monitor = $info['emulated_monitor'] }
                if ($spec.Extra -and ($spec.Extra -contains '--monitor') -and $info['emulated_monitor'] -ne 'on') { throw "$($p[1]) does not know --monitor (build it with this tool's source)" }
                if ($spec.Metric -eq 'lateness') {
                    $samples = Join-Path $rawDir "$label.r$k.samples.bin"
                    $a = Parse-Lines (& $p[2] analyze $samples)
                    $row['late_p50_us'] = $a['late_us_p50']; $row['late_p99_us'] = $a['late_us_p99']; $row['late_p999_us'] = $a['late_us_p999']
                    $row['late_max_us'] = $a['late_us_max']
                    Remove-Item $samples -ErrorAction SilentlyContinue
                }
                [void]$setRows.Add([pscustomobject]$row)
                [void]$rows.Add([pscustomobject]$row)
            }
        }
        if ($round -lt $plan.Min) { continue }
        if ($plan.Explicit) { break }
        # The number judged is the metric of the set: Meps for throughput and UI sets, p99 lateness for lateness sets.
        $field = if ($spec.Metric -eq 'lateness') { 'late_p99_us' } else { 'meps' }
        $samples = @{}
        foreach ($p in $programs) { $samples[$p[0]] = @($setRows | Where-Object { $_.program -eq $p[0] } | ForEach-Object { [double]$_.$field }) }
        $pairs = if ($Baseline) { , @('A', 'B') } else { @() }
        $v = Get-AdaptiveVerdict -Samples $samples -Pairs $pairs -Done $round -Max $plan.Max
        if (-not $v.Extend) { $roundNote = $v.Reason; break }
    }
    $adaptiveText = Format-AdaptiveNote -Used $round -Explicit $plan.Explicit -Reason $roundNote -Name $setName
    Write-Host "  $adaptiveText"
    $after = Get-Conditions
    $otherCpu = Median ($setRows | ForEach-Object { [double]$_.cpu_other_pct })
    $loadAvg = ($before.Cpu + $after.Cpu + $otherCpu) / 3
    $loaded = ($before.Cpu -ge $QuietCpuPercent) -or ($after.Cpu -ge $QuietCpuPercent) -or ($otherCpu -ge $QuietCpuPercent)
    $kind = if ($Baseline) { 'interleaved' } else { 'single build' }
    $condText = if ($loaded) { "under load ($kind)" } else { "quiet PC ($kind)" }
    [void]$summary.Add("## $setName - $condText")
    [void]$summary.Add('')
    [void]$summary.Add("Output $($spec.Output), mode $($spec.Mode), fixture $([System.IO.Path]::GetFileName($fixture)) ($($setRows[0].events) events). Power plan: $($before.Plan). Whole-PC CPU before / after: $($before.Cpu) % / $($after.Cpu) %; other processes during the runs (median): $otherCpu %; processor performance before / after: $($before.Perf) % / $($after.Perf) %.")
    [void]$summary.Add($adaptiveText)
    [void]$summary.Add('')
    if ($spec.Metric -eq 'throughput') {
        [void]$summary.Add('| program | plays | Meps median | min | max | ns per event (median) | events processed (all plays) |')
        [void]$summary.Add('|---|---|---|---|---|---|---|')
        $med = @{}
        foreach ($p in $programs) {
            $mine = @($setRows | Where-Object { $_.program -eq $p[0] })
            $v = @($mine | ForEach-Object { [double]$_.meps })
            $med[$p[0]] = Median $v
            $okAll = (@($mine | Where-Object { $_.processed -ne $_.events }).Count -eq 0) -and (@($mine | Where-Object { [int64]$_.dropped -ne 0 }).Count -eq 0)
            [void]$summary.Add(('| {0} ({1}) | {2} | {3:N2} | {4:N2} | {5:N2} | {6:N1} | {7} |' -f $p[0], $p[1], $v.Count, $med[$p[0]], ($v | Measure-Object -Minimum).Minimum, ($v | Measure-Object -Maximum).Maximum, (1000 / $med[$p[0]]), $(if ($okAll) { 'all' } else { 'MISMATCH' })))
        }
        if ($Baseline) { [void]$summary.Add(''); [void]$summary.Add(('Current / baseline = {0:N2}x (median over medians of interleaved plays).' -f ($med['B'] / $med['A']))) }
        if ($spec.Output -eq 'count') {
            $hashes = @($setRows | ForEach-Object { $_.hash } | Sort-Object -Unique)
            [void]$summary.Add(''); [void]$summary.Add("Message hash over all plays of both programs: $($hashes.Count) distinct value(s) ($($hashes -join ', ')) - " + $(if ($hashes.Count -eq 1) { 'identical messages in identical order.' } else { 'DIFFERENT.' }))
        }
    }
    elseif ($spec.Metric -eq 'ui') {
        [void]$summary.Add('| program | plays | Meps median | refresh call max ms (median / worst) | refresh call p99 ms (median) | latest refresh gap ms (median / worst) | longest send stall ms (median / worst) |')
        [void]$summary.Add('|---|---|---|---|---|---|---|')
        $med = @{}
        foreach ($p in $programs) {
            $mine = @($setRows | Where-Object { $_.program -eq $p[0] })
            $med[$p[0]] = Median ($mine | ForEach-Object { [double]$_.meps })
            $worst = { param($field) ($mine | ForEach-Object { [double]$_.$field } | Measure-Object -Maximum).Maximum }
            [void]$summary.Add(('| {0} ({1}) | {2} | {3:N2} | {4:N2} / {5:N2} | {6:N2} | {7:N1} / {8:N1} | {9:N1} / {10:N1} |' -f $p[0], $p[1], $mine.Count, $med[$p[0]],
                (Median ($mine | ForEach-Object { [double]$_.ui_call_max_ms })), (& $worst 'ui_call_max_ms'),
                (Median ($mine | ForEach-Object { [double]$_.ui_call_p99_ms })),
                (Median ($mine | ForEach-Object { [double]$_.ui_tick_gap_max_ms })), (& $worst 'ui_tick_gap_max_ms'),
                (Median ($mine | ForEach-Object { [double]$_.send_stall_max_ms })), (& $worst 'send_stall_max_ms')))
        }
        if ($Baseline) { [void]$summary.Add(''); [void]$summary.Add(('Throughput current / baseline = {0:N2}x.' -f ($med['B'] / $med['A']))) }
        [void]$summary.Add(''); [void]$summary.Add('The refresh runs at 60 Hz (a gap of about 16.7 ms is on time); a send stall shorter than the waiting loop''s sleep (about 1-16 ms) is not resolved.')
    }
    else {
        [void]$summary.Add('| program | plays | p50 us (median) | p99 us (median) | p99.9 us (median) | max us (median) | delivered Meps (median) |')
        [void]$summary.Add('|---|---|---|---|---|---|---|')
        foreach ($p in $programs) {
            $mine = @($setRows | Where-Object { $_.program -eq $p[0] })
            [void]$summary.Add(('| {0} ({1}) | {2} | {3:N1} | {4:N1} | {5:N1} | {6:N1} | {7:N2} |' -f $p[0], $p[1], $mine.Count,
                (Median ($mine | ForEach-Object { [double]$_.late_p50_us })), (Median ($mine | ForEach-Object { [double]$_.late_p99_us })),
                (Median ($mine | ForEach-Object { [double]$_.late_p999_us })), (Median ($mine | ForEach-Object { [double]$_.late_max_us })),
                (Median ($mine | ForEach-Object { [double]$_.meps }))))
        }
    }
    [void]$summary.Add('')
}
$rows | Export-Csv -Path (Join-Path $resultDir 'runs.csv') -NoTypeInformation
Set-Content -Path (Join-Path $resultDir 'summary.md') -Value $summary
$summary | ForEach-Object { Write-Host $_ }
Write-Host "Results: $resultDir"
