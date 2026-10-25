# Measures the adaptive display rate (Step 4): what one live update costs the interface thread and how that splits
# (the fixed part, the moving parts = position bar and time, the statistics block), and how the tiers engage and
# release. Every run plays a fixture for -Seconds on output None (the player's --play always selects None and
# refuses to play otherwise; --rate 0 is the Rate model) and reads the player's own --report.
#   .\v2\tools\display-tier-measure.ps1 -Set cost      # cost per update at fixed tiers (adaptive switched off): 3..7 rounds
#   .\v2\tools\display-tier-measure.ps1 -Set adapt     # the tiers engaging and releasing under pretended costs, and a modern-PC run
#   .\v2\tools\display-tier-measure.ps1 -Set custom -ExtraArgs '--display-cost-us 3000' -Name mytest
# The player window opens at -Position (inside Internal Display; never the 4K monitor); close nothing else.
# It waits (up to -QuietWaitSeconds) for total CPU under 20 %; if the PC stays busy it runs anyway and the table is
# labelled NOISY. Conditions are printed with the numbers.
param(
    [ValidateSet('cost', 'half', 'adapt', 'custom')][string]$Set = 'cost',
    [string]$ExtraArgs = '', [string]$Name = 'custom',
    [int]$Runs = 3,
    [int]$Seconds = 12,
    [string]$Build = 'x64',
    [string]$Fixture = (Join-Path $PSScriptRoot '..\harness\fixtures\large\large-8m.mid'),
    [string]$Position = '3880,1133',
    [int]$QuietWaitSeconds = 60,
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$exe = Join-Path $repo "v2\out\build-$Build\Release\midibn-win32.exe"
if (-not (Test-Path $exe)) { throw "missing $exe" }
if (-not (Test-Path $Fixture)) { throw "missing fixture $Fixture" }
if (-not $Position) { throw 'a test launch must place the window: pass -Position X,Y (inside Internal Display)' }
$temp = Join-Path $env:TEMP 'display-tier'
New-Item -ItemType Directory -Force -Path $temp | Out-Null
. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')

$sets = @{
    cost = [ordered]@{
        'paint-all-tier0'  = '--adaptive-display off --display-paint-all'
        'tier0'            = '--display-tier 0'
        'tier1-half-stats' = '--display-tier 1'
        'tier2-30Hz'       = '--display-tier 2'
        'tier0-timer144'   = '--pace timer --pace-hz 144 --display-tier 0'
        'tier1-timer144'   = '--pace timer --pace-hz 144 --display-tier 1'
    }
    half = [ordered]@{
        'tier0'          = '--display-tier 0'
        'tier1-half'     = '--display-tier 1'
        'tier0-timer144' = '--pace timer --pace-hz 144 --display-tier 0'
        'tier1-timer144' = '--pace timer --pace-hz 144 --display-tier 1'
    }
    adapt = [ordered]@{
        'modern-pc'          = ''
        'cost-3000us'        = '--display-cost-us 3000'
        'cost-8000us'        = '--display-cost-us 8000'
        'cost-20000us'       = '--display-cost-us 20000'
        'stats-heavy-12ms'   = '--display-cost-us 0,12000'
        '2cpu-modern'        = '--affinity 3'
        '2cpu-cost-3000us'   = '--affinity 3 --display-cost-us 3000'
        'behind-from-3s'     = '--display-pretend-behind 3000'
        'off-cost-8000us'    = '--adaptive-display off --display-cost-us 8000'
    }
    custom = [ordered]@{ $Name = $ExtraArgs }
}
$scenarios = $sets[$Set]

function Get-CpuLoad {
    $s = (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples | ForEach-Object { $_.CookedValue }
    return [math]::Round(($s | Measure-Object -Average).Average, 1)
}
$noisy = $false
function Wait-Quiet {
    $deadline = (Get-Date).AddSeconds($QuietWaitSeconds)
    while ($true) {
        $load = Get-CpuLoad
        if ($load -lt 20) { return $load }
        if ((Get-Date) -gt $deadline) { $script:noisy = $true; return $load }
        Start-Sleep -Seconds 5
    }
}
function Get-Median($values) { return Get-AdaptiveMedian ([double[]]@($values | Where-Object { $_ -ne $null })) }

$plan = (powercfg /getactivescheme) -replace '^.*:\s*', ''
Write-Host "=== display tiers, set '$Set': build=$Build seconds=$Seconds fixture=$(Split-Path -Leaf $Fixture) time=$(Get-Date -Format 'yyyy-MM-dd hh:mm tt') EDT"
Write-Host "power plan: $plan; logical processors: $([Environment]::ProcessorCount)"

$adp = Get-AdaptivePlan -Explicit $PSBoundParameters.ContainsKey('Runs') -ExplicitCount $Runs
$rows = @()
$run = 0
$reason = ''
while ($true) {
    $run++
    foreach ($key in $scenarios.Keys) {
        $load = Wait-Quiet
        $report = Join-Path $temp "$Set-$key-$run.txt"
        Remove-Item $report -ErrorAction SilentlyContinue
        $arguments = @('--no-settings', '--open', "`"$Fixture`"", '--play', '--rate', '0', '--auto-close-ms', ($Seconds * 1000), '--report', "`"$report`"", '--position', $Position)
        if ($scenarios[$key]) { $arguments += ($scenarios[$key] -split ' ') }
        $p = Start-Process $exe -ArgumentList $arguments -PassThru
        if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { $p.Kill() }
        $t = @{}
        $log = ''
        if (Test-Path $report) {
            foreach ($line in Get-Content $report) {
                if ($line -like 't=*tier*') { $log += $line + "`n"; continue }
                foreach ($m in [regex]::Matches($line, '(\w+)=([-\d.]+)')) { $t[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
            }
        }
        $rows += [pscustomobject]@{
            run = $run; scenario = $key; cpu_before = $load
            updates = $t['refresh_updates']; interval_ms = $t['mean']; p99_ms = $t['p99']
            total_us = $t['total']; tick_us = $t['tick']; stats_values_us = $t['statistics_values']
            paint_timeline_us = $t['paint_timeline']; paint_stats_us = $t['paint_statistics']; paint_other_us = $t['paint_other']
            ui_cpu_ms = $t['ui_thread_cpu_ms']; final_tier = $t['final_tier']; changes = $t['tier_changes']
            events = $t['events_processed']; tiers = ((0..4 | ForEach-Object { $t["t$_"] }) -join '/'); log = $log
        }
    }
    if ($run -lt $adp.Min) { continue }
    if ($adp.Explicit) { break }
    $samples = @{}
    foreach ($key in $scenarios.Keys) { $samples[$key] = @($rows | Where-Object { $_.scenario -eq $key -and $null -ne $_.total_us } | ForEach-Object { [double]$_.total_us }) }
    $verdict = Get-AdaptiveVerdict -Samples $samples -Done $run -Max $adp.Max
    if (-not $verdict.Extend) { $reason = $verdict.Reason; break }
}
Write-Host (Format-AdaptiveNote -Used $run -Explicit $adp.Explicit -Reason $reason)
$label = if ($noisy) { 'NOISY (the PC stayed above 20 % CPU)' } else { 'quiet' }
Write-Host "--- medians over $run runs, cpu before runs $((($rows | Measure-Object cpu_before -Minimum -Maximum) | ForEach-Object { "$($_.Minimum)-$($_.Maximum) %" })): $label"
$summary = foreach ($key in $scenarios.Keys) {
    $g = @($rows | Where-Object { $_.scenario -eq $key })
    [pscustomobject]@{
        scenario = $key
        updates = Get-Median ($g.updates); interval_ms = Get-Median ($g.interval_ms); p99_ms = Get-Median ($g.p99_ms)
        total_us = Get-Median ($g.total_us); tick_us = Get-Median ($g.tick_us); stats_vals_us = Get-Median ($g.stats_values_us)
        pt_us = Get-Median ($g.paint_timeline_us); ps_us = Get-Median ($g.paint_stats_us); po_us = Get-Median ($g.paint_other_us)
        ui_cpu_ms = Get-Median ($g.ui_cpu_ms); tier = ($g | Select-Object -Last 1).final_tier; changes = Get-Median ($g.changes)
        ms_in_tiers = ($g | Select-Object -Last 1).tiers
    }
}
$summary | Format-Table -AutoSize | Out-String -Width 250 | Write-Host
foreach ($key in $scenarios.Keys) {
    $last = $rows | Where-Object { $_.scenario -eq $key -and $_.log } | Select-Object -Last 1
    if ($last) { Write-Host "[$key] tier changes (last run):"; Write-Host $last.log }
}
if ($Out) { $rows | Export-Csv -NoTypeInformation -Path $Out }
