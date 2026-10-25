# Load-time and peak-memory comparison on the generated large fixture.
# Each measurement is a fresh process. One untimed warm-up load per
# implementation first, so every timed run sees a warm file cache.
# -Analysis additionally times midibn-perf --analysis (three fixed Analysis
# runs after the load; v2 tools only) and prints a second table, plus whether
# every result hash (including double bit patterns) agrees across the runs and
# across implementations.
# Runs: without -Runs each implementation gets 3 runs, plus one at a time (up to 7) while its load times spread more
# than 10 % of their median (adaptive-repeats.ps1); an explicit -Runs N means exactly N runs.
param([int]$Runs = 3, [string]$Output = '', [switch]$Analysis, [string]$Fixture = 'v2\harness\fixtures\large\large-8m.mid')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo
. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')
$plan = Get-AdaptivePlan -Explicit $PSBoundParameters.ContainsKey('Runs') -ExplicitCount $Runs
$midi = (Resolve-Path $Fixture).Path
$tools = [ordered]@{
    'v1 x64' = @('v2\out\v1-reference\x64\v1-reference.exe', 'perf-load')
    'v1 x86' = @('v2\out\v1-reference\x86\v1-reference.exe', 'perf-load')
    'v2 x64' = @('v2\out\build-x64\Release\midibn-perf.exe')
    'v2 x86' = @('v2\out\build-x86\Release\midibn-perf.exe')
    'v2 XP-toolset x86' = @('v2\out\build-xp\Release\midibn-perf.exe')
    'v2 XP-toolset x86 IA32' = @('v2\out\build-xp-ia32\Release\midibn-perf.exe')
}

function Run([string[]]$tool) {
    $exe = $tool[0]
    $arguments = @($tool | Select-Object -Skip 1)
    if ($Analysis -and $tool[0] -like '*midibn-perf.exe') { $arguments += '--analysis' }
    $arguments += $midi
    $values = @{}
    foreach ($line in (& $exe @arguments)) { $parts = $line -split '=', 2; if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] } }
    if ($LASTEXITCODE -ne 0) { throw "$exe failed" }
    return $values
}

$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$os = (Get-CimInstance Win32_OperatingSystem)
$lines = @()
$lines += "# Load comparison on $(Split-Path -Leaf $midi) ($((Get-Item $midi).Length) bytes)"
$lines += "# $(Get-Date -Format 'yyyy-MM-dd HH:mm') | $cpu | $ram GB RAM | $($os.Caption) $($os.Version)"
$lines += "# 3 to 7 (adaptive) timed fresh-process runs each after one warm-up, or exactly $Runs when -Runs was given; times are the parser's own load call only"
$lines += '| Implementation | Load ms (min / median / max) | Peak working set MiB (median) | Peak private MiB (median) |'
$lines += '|---|---|---|---|'
$analysisLines = @()
$analysisLines += "# Analysis (midibn-perf --analysis), default resolution, fresh cache per run; ms min / median / max over the runs of each implementation"
$analysisLines += '| Implementation | base workload | queue projection | gate projections | result hashes |'
$analysisLines += '|---|---|---|---|---|'
$referenceHashes = $null
$roundNotes = @()
foreach ($name in $tools.Keys) {
    if (-not (Test-Path $tools[$name][0])) { continue }
    Run $tools[$name] | Out-Null
    $results = @()
    while ($true) {
        $results += , (Run $tools[$name])
        if ($results.Count -lt $plan.Min) { continue }
        if ($plan.Explicit) { $note = ''; break }
        $v = Get-AdaptiveVerdict -Samples @{ 'load ms' = @($results | ForEach-Object { [double]$_['load-ms'] }) } -Done $results.Count -Max $plan.Max
        if (-not $v.Extend) { $note = $v.Reason; break }
    }
    $roundNotes += Format-AdaptiveNote -Used $results.Count -Explicit $plan.Explicit -Reason $note -Name $name
    $mid = [int][math]::Floor($results.Count / 2)
    $times = @($results | ForEach-Object { [double]$_['load-ms'] } | Sort-Object)
    $ws = @($results | ForEach-Object { [double]$_['peak-working-set-bytes'] / 1MB } | Sort-Object)
    $pv = @($results | ForEach-Object { [double]$_['peak-private-bytes'] / 1MB } | Sort-Object)
    $lines += ('| {0} | {1:N0} / {2:N0} / {3:N0} | {4:N0} | {5:N0} |' -f $name, $times[0], $times[$mid], $times[-1], $ws[$mid], $pv[$mid])
    if ($Analysis -and $results[0].ContainsKey('analysis-base-ms')) {
        $cells = foreach ($part in 'base', 'queue', 'gates') {
            $t = @($results | ForEach-Object { [double]$_["analysis-$part-ms"] } | Sort-Object)
            '{0:N0} / {1:N0} / {2:N0}' -f $t[0], $t[$mid], $t[-1]
        }
        $hashes = @($results | ForEach-Object { 'base ' + $_['analysis-base-hash'] + '/' + $_['analysis-base-doubles-hash'] + ' queue ' + $_['analysis-queue-hash'] + '/' + $_['analysis-queue-doubles-hash'] + ' gates ' + $_['analysis-gates-hash'] + '/' + $_['analysis-gates-doubles-hash'] } | Select-Object -Unique)
        $state = if ($hashes.Count -ne 1) { 'VARIES between runs' } elseif ($null -eq $referenceHashes) { $referenceHashes = $hashes[0]; 'reference' } elseif ($hashes[0] -eq $referenceHashes) { 'identical to first implementation' } else { 'DIFFERENT from first implementation' }
        $analysisLines += ('| {0} | {1} | {2} | {3} | {4} |' -f $name, $cells[0], $cells[1], $cells[2], $state)
    }
}
if ($Analysis) { $lines += ''; $lines += $analysisLines }
$lines += ''
$lines += $roundNotes
$lines | ForEach-Object { Write-Host $_ }
if ($Output) { $lines | Set-Content -Encoding utf8 $Output }
