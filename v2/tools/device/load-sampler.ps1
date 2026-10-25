# Read-only background load sampler. Changes no setting.
# Every -IntervalSeconds it appends one line to -LogFile:
#   time (EDT, local), total CPU %, processor performance % (clock relative to base),
#   total GPU engine utilization % (3D/compute/copy/video engines summed per adapter, max taken),
#   top 3 processes by CPU, active power plan name (read once a minute at most).
# Stop it by stopping the process, or by creating the file named by -StopFile.
param(
    [Parameter(Mandatory = $true)][string]$LogFile,
    [int]$IntervalSeconds = 45,
    [string]$StopFile = ''
)

$ErrorActionPreference = 'SilentlyContinue'
if (-not (Test-Path $LogFile)) {
    "time`tcpu%`tperf%`tgpu%`ttop processes (cpu% of one core)`tpower plan" | Out-File -FilePath $LogFile -Encoding utf8
}
$cores = [Environment]::ProcessorCount
$plan = ''
$planAt = [DateTime]::MinValue
while ($true) {
    if ($StopFile -and (Test-Path $StopFile)) { break }
    $now = Get-Date
    if (($now - $planAt).TotalSeconds -ge 60) {
        $p = (powercfg /getactivescheme) 2>$null
        if ($p -match '\((.+)\)') { $plan = $Matches[1] }
        $planAt = $now
    }
    $counters = @('\Processor(_Total)\% Processor Time', '\Processor Information(_Total)\% Processor Performance', '\Process(*)\% Processor Time')
    $s = Get-Counter -Counter $counters -SampleInterval 2 -MaxSamples 1
    $cpu = ''; $perf = ''; $top = ''
    if ($s) {
        $samples = $s.CounterSamples
        $c = $samples | Where-Object { $_.Path -like '*\processor(_total)\% processor time' } | Select-Object -First 1
        if ($c) { $cpu = '{0:N0}' -f $c.CookedValue }
        $f = $samples | Where-Object { $_.Path -like '*processor information(_total)\% processor performance' } | Select-Object -First 1
        if ($f) { $perf = '{0:N0}' -f $f.CookedValue }
        $procs = $samples | Where-Object { $_.Path -like '*\process(*' -and $_.InstanceName -ne '_total' -and $_.InstanceName -ne 'idle' } |
            Sort-Object CookedValue -Descending | Select-Object -First 3
        $top = ($procs | ForEach-Object { '{0} {1:N0}' -f $_.InstanceName, $_.CookedValue }) -join ', '
    }
    $gpu = ''
    $g = Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage' -MaxSamples 1
    if ($g) {
        $byType = @{}
        foreach ($x in $g.CounterSamples) {
            if ($x.InstanceName -match 'engtype_(\w+)') {
                $t = $Matches[1]
                if (-not $byType.ContainsKey($t)) { $byType[$t] = 0.0 }
                $byType[$t] += $x.CookedValue
            }
        }
        if ($byType.Count -gt 0) { $gpu = '{0:N0}' -f (($byType.Values | Measure-Object -Maximum).Maximum) }
    }
    ('{0}`t{1}`t{2}`t{3}`t{4}`t{5}' -f $now.ToString('yyyy-MM-dd h:mm:ss tt'), $cpu, $perf, $gpu, $top, $plan) -replace '`t', "`t" |
        Out-File -FilePath $LogFile -Append -Encoding utf8
    Start-Sleep -Seconds ([Math]::Max(5, $IntervalSeconds - 3))
}
