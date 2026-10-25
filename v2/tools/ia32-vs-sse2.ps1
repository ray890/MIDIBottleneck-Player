# Interleaved measurement of the two Windows-XP-toolset x86 builds of midibn-perf:
#   SSE2 = v2\out\build-xp\Release\midibn-perf.exe
#   IA32 = v2\out\build-xp-ia32\Release\midibn-perf.exe
# (-IncludeModern adds build-x86 and build-x64 as extra, non-compared columns in the raw data.)
#
# Usage (Windows PowerShell 5.1, from anywhere):
#   .\v2\tools\ia32-vs-sse2.ps1 [-Runs 7] [-Fixture <midi>] [-Output result.md] [-SkipEngine]
#                               [-IncludeModern] [-Before DIR] [-Micro] [-Warmup 1] [-DryRun]
# Rounds: 3, plus one at a time (to 7) while SSE2 vs IA32 is unclear on some metric (adaptive-repeats.ps1); -Runs N = exactly N.
# Per round r = 1..Runs the builds run strictly in turn (SSE2, IA32, [x86, x64]); every
# measurement is a fresh process. Per build and round: one `--analysis` process (load time and
# Analysis base/queue/gates) and, unless -SkipEngine, one `--engine` process (wall ms, events/s).
# Warm-up runs (untimed, same interleaved order) come first. All analysis hashes, doubles
# hashes and the engine output hash must be identical in every run and between builds.
# -Before DIR adds two more builds to the same interleaved rounds: DIR\xp\midibn-perf.exe ('SSE2 before') and
# DIR\xp-ia32\midibn-perf.exe ('IA32 before'), saved from earlier code, for a before/after comparison of a change; their
# result hashes must be identical to the current builds' (a change must not alter any result).
# -Micro adds one `--micro` process per build and round (small simple workloads: decoded event reads, 48-byte copies,
# 64-bit division, double conversion; tools\perf_main.cpp) to locate a gap; not for the -Before builds when they lack it.
# -Output writes the markdown (conditions + table) and <Output>.raw.csv (per-run numbers).
# -DryRun fabricates numbers (no executables run) to test the statistics and report logic.
param(
    [int]$Runs = 3,   # without -Runs: 3 rounds, plus one at a time up to 7 while SSE2 vs IA32 is unclear (adaptive-repeats.ps1)
    [string]$Fixture = 'v2\harness\fixtures\large\large-8m.mid',
    [string]$Output = '',
    [switch]$SkipEngine,
    [switch]$IncludeModern,
    [string]$Before = '',
    [switch]$Micro,
    [int]$Warmup = 1,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
if ($Runs -lt 3) { throw '-Runs must be at least 3' }
. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')
$adp = Get-AdaptivePlan -Explicit $PSBoundParameters.ContainsKey('Runs') -ExplicitCount $Runs
$adpReason = ''
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo

$builds = [ordered]@{
    'SSE2' = 'v2\out\build-xp\Release\midibn-perf.exe'
    'IA32' = 'v2\out\build-xp-ia32\Release\midibn-perf.exe'
}
if ($Before) {
    $builds['SSE2 before'] = Join-Path $Before 'xp\midibn-perf.exe'
    $builds['IA32 before'] = Join-Path $Before 'xp-ia32\midibn-perf.exe'
}
if ($IncludeModern) {
    $builds['x86'] = 'v2\out\build-x86\Release\midibn-perf.exe'
    $builds['x64'] = 'v2\out\build-x64\Release\midibn-perf.exe'
}
if (-not $DryRun) {
    foreach ($xp in 'SSE2', 'IA32') {
        if (-not (Test-Path $builds[$xp])) { throw "The $xp build is missing ($($builds[$xp])). Both XP builds are required; build them first." }
    }
    foreach ($name in @($builds.Keys)) {
        if (-not (Test-Path $builds[$name])) { Write-Host "Skipping build '$name': $($builds[$name]) not found."; $builds.Remove($name) }
    }
    $midi = (Resolve-Path $Fixture).Path
} else { $midi = $Fixture }

# ---------- helpers ----------
function Parse-Lines($lines) {
    $values = @{}
    foreach ($line in $lines) { $parts = ([string]$line) -split '=', 2; if ($parts.Count -eq 2) { $values[$parts[0].Trim()] = $parts[1].Trim() } }
    return $values
}
$script:dryRandom = New-Object System.Random 12345
function Invoke-Perf([string]$name, [string[]]$flags) {
    if ($DryRun) {
        $skew = if ($name -eq 'IA32') { 1.04 } else { 1.0 }
        $v = @{}
        $j = { param($base) $base * $skew * (1 + ($script:dryRandom.NextDouble() - 0.5) * 0.06) }
        if ($flags -contains '--analysis') {
            $v['load-ms'] = '{0:F1}' -f (& $j 500)
            foreach ($p in 'base', 'queue', 'gates') {
                $v["analysis-$p-ms"] = '{0:F1}' -f (& $j 800)
                $v["analysis-$p-hash"] = 'abcd0123'; $v["analysis-$p-doubles-hash"] = '00000000deadbeef'
            }
        }
        if ($flags -contains '--engine') {
            $ms = & $j 2000
            $v['engine-wall-ms'] = '{0:F1}' -f $ms
            $v['engine-events-per-second'] = '{0:F0}' -f (8000000 * 1000.0 / $ms)
            $v['engine-output-hash'] = '1122334455667788'
        }
        return $v
    }
    $out = & $builds[$name] @flags $midi
    if ($LASTEXITCODE -ne 0) { throw "$($builds[$name]) $($flags -join ' ') failed (exit $LASTEXITCODE)" }
    return (Parse-Lines $out)
}
function Median([double[]]$x) {
    $s = @($x | Sort-Object); $n = $s.Count
    if ($n % 2 -eq 1) { return $s[($n - 1) / 2] } else { return ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }
}
function Sample-Counters {
    if ($DryRun) { return @{ cpu = 10.0 + $script:dryRandom.NextDouble() * 5; perf = 100.0 } }
    $r = @{ cpu = $null; perf = $null }
    try { $r.cpu = [double](Get-Counter '\Processor(_Total)\% Processor Time' -ErrorAction Stop).CounterSamples[0].CookedValue } catch {}
    try { $r.perf = [double](Get-Counter '\Processor Information(_Total)\% Processor Performance' -ErrorAction Stop).CounterSamples[0].CookedValue } catch {}
    return $r
}

# ---------- conditions ----------
$cpuInfo = Get-CimInstance Win32_Processor | Select-Object -First 1
$cs = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$ram = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
$plan = try { ((powercfg /getactivescheme) -join ' ').Trim() } catch { 'unknown' }
$eastern = [TimeZoneInfo]::FindSystemTimeZoneById('Eastern Standard Time')
$easternNow = [TimeZoneInfo]::ConvertTime([DateTime]::UtcNow, $eastern)
$zone = if ($eastern.IsDaylightSavingTime($easternNow)) { 'EDT' } else { 'EST' }
$startLabel = '{0} {1}' -f $easternNow.ToString('yyyy-MM-dd h:mm tt', [Globalization.CultureInfo]::InvariantCulture), $zone

# ---------- measurement ----------
$names = @($builds.Keys)
if ($DryRun) { $names = @('SSE2', 'IA32') + @(if ($Before) { 'SSE2 before', 'IA32 before' }) + @(if ($IncludeModern) { 'x86', 'x64' }) }
$samples = New-Object System.Collections.ArrayList
$samples.Add((Sample-Counters)) | Out-Null
for ($w = 1; $w -le $Warmup; $w++) {
    foreach ($n in $names) {
        Write-Host "warm-up $w $n"
        Invoke-Perf $n @('--analysis') | Out-Null
        if (-not $SkipEngine) { Invoke-Perf $n @('--engine') | Out-Null }
    }
}
$rows = New-Object System.Collections.ArrayList   # build, run, metric, value
$hashSets = @{}                                   # build -> set of hash strings
$microChecks = New-Object System.Collections.Generic.HashSet[string]   # --micro checksums (must be one string)
foreach ($n in $names) { $hashSets[$n] = New-Object System.Collections.Generic.HashSet[string] }
$r = 0
while ($true) {
    $r++
    foreach ($n in $names) {
        Write-Host "round $r $n"
        $a = Invoke-Perf $n @('--analysis')
        $rows.Add([pscustomobject]@{ Build = $n; Run = $r; Metric = 'load-ms'; Value = [double]$a['load-ms'] }) | Out-Null
        foreach ($p in 'base', 'queue', 'gates') {
            $rows.Add([pscustomobject]@{ Build = $n; Run = $r; Metric = "analysis-$p-ms"; Value = [double]$a["analysis-$p-ms"] }) | Out-Null
        }
        $hk = @($a.Keys | Where-Object { $_ -like 'analysis-*hash' } | Sort-Object)
        if ($hk.Count -eq 0) { throw "$n produced no analysis hashes" }
        $hashSets[$n].Add((($hk | ForEach-Object { "$_=$($a[$_])" }) -join ' ')) | Out-Null
        if (-not $SkipEngine) {
            $e = Invoke-Perf $n @('--engine')
            $rows.Add([pscustomobject]@{ Build = $n; Run = $r; Metric = 'engine-wall-ms'; Value = [double]$e['engine-wall-ms'] }) | Out-Null
            $rows.Add([pscustomobject]@{ Build = $n; Run = $r; Metric = 'engine-events-per-second'; Value = [double]$e['engine-events-per-second'] }) | Out-Null
            if (-not $e.ContainsKey('engine-output-hash')) { throw "$n produced no engine-output-hash" }
            $hashSets[$n].Add("engine-output-hash=$($e['engine-output-hash'])") | Out-Null
        }
        if ($Micro -and -not ($n -like '* before')) {
            $u = if ($DryRun) { @{ 'micro-view-copy-ms' = '10'; 'micro-view-copy-check' = 'ab' } } else { Invoke-Perf $n @('--micro') }
            foreach ($k in @($u.Keys | Where-Object { $_ -like 'micro-*-ms' })) {
                $rows.Add([pscustomobject]@{ Build = $n; Run = $r; Metric = $k; Value = [double]$u[$k] }) | Out-Null
            }
            $microChecks.Add((($u.Keys | Where-Object { $_ -like 'micro-*-check' } | Sort-Object | ForEach-Object { "$_=$($u[$_])" }) -join ' ')) | Out-Null
        }
    }
    $samples.Add((Sample-Counters)) | Out-Null
    if ($r -lt $adp.Min) { continue }
    if ($adp.Explicit) { break }
    # The comparison being made is SSE2 against IA32, on every timed metric (events/s is the same figure as wall time).
    $adpSamples = @{}; $adpPairs = @()
    foreach ($metric in @($rows | ForEach-Object { $_.Metric } | Select-Object -Unique | Where-Object { $_ -notlike 'micro-*' -and $_ -ne 'engine-events-per-second' })) {
        foreach ($b in 'SSE2', 'IA32') { $adpSamples["$metric|$b"] = @($rows | Where-Object { $_.Build -eq $b -and $_.Metric -eq $metric } | ForEach-Object { $_.Value }) }
        $adpPairs += , @("$metric|SSE2", "$metric|IA32")
    }
    $verdict = Get-AdaptiveVerdict -Samples $adpSamples -Pairs $adpPairs -Done $r -Max $adp.Max
    if (-not $verdict.Extend) { $adpReason = $verdict.Reason; break }
}
$roundsUsed = $r
$adpNote = Format-AdaptiveNote -Used $roundsUsed -Explicit $adp.Explicit -Reason $adpReason
Write-Host $adpNote

# ---------- hash verification ----------
$all = New-Object System.Collections.Generic.HashSet[string]
foreach ($n in $names) { foreach ($h in $hashSets[$n]) { $all.Add($h) | Out-Null } }
# every run of every build contributes the same two strings (analysis, engine) when identical
$expectedCount = if ($SkipEngine) { 1 } else { 2 }
$identical = ($all.Count -eq $expectedCount)
$hashLine = if ($identical) { "Result hashes (analysis incl. doubles hashes, engine output): PASS - identical in all $roundsUsed runs of all builds" }
            else { '*** DIFFERENT *** result hashes differ between runs or builds: ' + (($names | ForEach-Object { "$_ => " + ($hashSets[$_] -join ' | ') }) -join ' ;; ') }

# ---------- statistics ----------
$cpuVals = @($samples | Where-Object { $null -ne $_.cpu } | ForEach-Object { $_.cpu })
$perfVals = @($samples | Where-Object { $null -ne $_.perf } | ForEach-Object { $_.perf })
$cpuMean = if ($cpuVals.Count) { ($cpuVals | Measure-Object -Average).Average } else { $null }
$label = if ($null -ne $cpuMean -and $cpuMean -lt 20) { 'quiet PC (interleaved)' } else { 'under load (interleaved)' }
if ($null -eq $cpuMean) { $label = 'under load (interleaved)' }

function Stat($build, $metric) {
    $v = @($rows | Where-Object { $_.Build -eq $build -and $_.Metric -eq $metric } | ForEach-Object { $_.Value })
    $m = $v | Measure-Object -Minimum -Maximum
    return [pscustomobject]@{ Min = $m.Minimum; Med = (Median $v); Max = $m.Maximum }
}
$metricList = @(
    @('Load (ms)', 'load-ms', 'N1'), @('Analysis base (ms)', 'analysis-base-ms', 'N1'),
    @('Analysis queue (ms)', 'analysis-queue-ms', 'N1'), @('Analysis gates (ms)', 'analysis-gates-ms', 'N1')
)
if (-not $SkipEngine) { $metricList += , @('Engine wall (ms)', 'engine-wall-ms', 'N1'); $metricList += , @('Engine events/s', 'engine-events-per-second', 'N0') }

$md = @()
$md += "# IA32 vs SSE2 (Windows XP toolset x86) - $label"
$md += ''
$md += '## Conditions'
$md += "- Started: $startLabel"
$md += "- CPU: $($cpuInfo.Name.Trim()), $($cs.NumberOfLogicalProcessors) logical processors"
$md += "- RAM: $ram GB; OS: $($os.Caption) $($os.Version)"
$md += "- Power plan: $plan"
$f = { param($x) if ($x.Count) { $m = $x | Measure-Object -Average -Minimum -Maximum; 'mean {0:N1} / min {1:N1} / max {2:N1}' -f $m.Average, $m.Minimum, $m.Maximum } else { 'unavailable' } }
$md += "- CPU load %% (Processor Time, $($cpuVals.Count) samples: before first run and after each round): $(& $f $cpuVals)".Replace('%%', '%')
$md += "- Processor performance %% (Processor Information): $(& $f $perfVals)".Replace('%%', '%')
$md += "- Fixture: $(Split-Path -Leaf $midi); $roundsUsed interleaved runs per build after $Warmup warm-up; builds: $($names -join ', ')$(if ($DryRun) { ' [DRY RUN - fabricated numbers]' })"
$md += "- $adpNote"
$md += '- Reference: on the real Windows XP machine the full self-test engine scenarios took 333 s (IA32) vs 342 s (SSE2).'
$md += "- $hashLine"
$md += ''
$md += '| Metric | SSE2 min / median / max | IA32 min / median / max | IA32/SSE2 (medians) |'
$md += '|---|---|---|---|'
foreach ($m in $metricList) {
    $s = Stat 'SSE2' $m[1]; $i = Stat 'IA32' $m[1]; $fmt = $m[2]
    $md += ('| {0} | {1} / {2} / {3} | {4} / {5} / {6} | {7:N3} |' -f $m[0], $s.Min.ToString($fmt), $s.Med.ToString($fmt), $s.Max.ToString($fmt), $i.Min.ToString($fmt), $i.Med.ToString($fmt), $i.Max.ToString($fmt), ($i.Med / $s.Med))
}
if ($Before -and ($names -contains 'SSE2 before') -and ($names -contains 'IA32 before')) {
    $md += ''
    $md += "Before/after (before = the builds in $Before; medians; same interleaved rounds):"
    $md += '| Metric | SSE2 before | IA32 before | IA32/SSE2 before | SSE2 after/before | IA32 after/before | IA32/SSE2 after |'
    $md += '|---|---|---|---|---|---|---|'
    foreach ($m in $metricList) {
        $sb = (Stat 'SSE2 before' $m[1]).Med; $ib = (Stat 'IA32 before' $m[1]).Med
        $sa = (Stat 'SSE2' $m[1]).Med; $ia = (Stat 'IA32' $m[1]).Med; $fmt = $m[2]
        $md += ('| {0} | {1} | {2} | {3:N3} | {4:N3} | {5:N3} | {6:N3} |' -f $m[0], $sb.ToString($fmt), $ib.ToString($fmt), ($ib / $sb), ($sa / $sb), ($ia / $ib), ($ia / $sa))
    }
}if ($Micro) {
    $md += ''
    $md += "Micro workloads (medians, ms; checksums: $(if ($microChecks.Count -eq 1) { 'PASS - identical in all runs of all builds' } else { '*** DIFFERENT ***' })):"
    $mnames = @($names | Where-Object { $_ -notlike '* before' })
    $md += '| Workload | ' + ($mnames -join ' | ') + ' | IA32/SSE2 |'
    $md += '|---|' + (($mnames | ForEach-Object { '---|' }) -join '') + '---|'
    foreach ($k in @($rows | Where-Object { $_.Metric -like 'micro-*' } | ForEach-Object { $_.Metric } | Select-Object -Unique)) {
        $cells = $mnames | ForEach-Object { (Stat $_ $k).Med.ToString('N1') }
        $md += ("| $($k -replace '^micro-|-ms$', '') | $($cells -join ' | ') | {0:N3} |" -f ((Stat 'IA32' $k).Med / (Stat 'SSE2' $k).Med))
    }
}
if ($IncludeModern) {
    $md += ''
    $md += 'Modern builds (median only):'
    $md += '| Metric | ' + (($names | Where-Object { $_ -in 'x86', 'x64' }) -join ' | ') + ' |'
    $md += '|---|' + ((($names | Where-Object { $_ -in 'x86', 'x64' }) | ForEach-Object { '---|' }) -join '')
    foreach ($m in $metricList) {
        $cells = ($names | Where-Object { $_ -in 'x86', 'x64' }) | ForEach-Object { (Stat $_ $m[1]).Med.ToString($m[2]) }
        $md += "| $($m[0]) | $($cells -join ' | ') |"
    }
}
$md | ForEach-Object { Write-Host $_ }
if ($Output) {
    $md | Set-Content -Encoding utf8 $Output
    $rows | Export-Csv -NoTypeInformation -Encoding utf8 ($Output + '.raw.csv')
    Write-Host "Wrote $Output and $Output.raw.csv"
}
if (-not $identical -or ($Micro -and $microChecks.Count -ne 1)) { exit 1 }
