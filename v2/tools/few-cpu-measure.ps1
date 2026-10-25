# Playback on a PC with one or two logical processors, with and without the few-processor options (P10-THR).
# Runs midibn-few-cpu-measure (real engine, real clock, output None with a per-message processor cost; NO MIDI device is
# opened, no driver loaded, no sound) for every combination of
#   processors   1 (affinity mask 1) and 2 (mask 3)
#   audio thread a stand-in Time Critical thread that is busy for 0 % or 30 % of every 10 ms (the synthesizer's own thread)
#   option       off | normal | yield=2 | normal,yield=2   (--few-cpu; see win32_timing.h)
# in interleaved rounds (-Rounds, default 3; add rounds only when the results are close) and writes one line per run to
# v2\out\few-cpu\<stamp>\runs.txt plus a table of medians to summary.txt. The PC is not quiet while other work runs: the
# summary header records the whole-PC processor load before the runs.
#
# Fixture: v2\out\fixtures-silent\silent-cc-custom-3000000-5s.mid (make it with
#   .\v2\tools\device\make-silent-fixture.ps1 -Rate 3000000 -Seconds 5)
#
# Usage: .\v2\tools\few-cpu-measure.ps1 [-Rounds 3] [-CostNs 300] [-Exe path] [-Fixture path]
param(
    [int]$Rounds = 3,
    [int]$CostNs = 300,
    [string]$Exe = '',
    [string]$Fixture = '',
    [string[]]$Options = @('off', 'normal', 'yield=2', 'normal,yield=2'),   # also: nap=MS (yield by a 1 ms sleep)
    [string[]]$Cpus = @('1', '2')
)
$Options = @($Options | ForEach-Object { $_ -split ';' })   # from a command line: -Options 'off;nap=20' (a comma is part of normal,yield=2)
$Cpus = @($Cpus | ForEach-Object { $_ -split ',' })
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Exe) { $Exe = Join-Path $root 'v2\out\build-x64\Release\midibn-few-cpu-measure.exe' }
if (-not $Fixture) { $Fixture = Join-Path $root 'v2\out\fixtures-silent\silent-cc-custom-3000000-5s.mid' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dir = Join-Path $root "v2\out\few-cpu\$stamp"
New-Item -ItemType Directory -Force $dir | Out-Null
$runs = Join-Path $dir 'runs.txt'

$load = [math]::Round(((Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 3).CounterSamples | Measure-Object CookedValue -Average).Average, 1)
$conditions = @()
foreach ($cpu in @(@('1', '1'), @('2', '3'))) {
    if ($Cpus -notcontains $cpu[0]) { continue }
    foreach ($audio in @(0, 30)) {
        foreach ($opt in $Options) {
            $conditions += [pscustomobject]@{ cpus = $cpu[0]; mask = $cpu[1]; audio = $audio; opt = $opt }
        }
    }
}
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($c in $conditions) {
        $label = "cpu$($c.cpus)-audio$($c.audio)-" + $($c.opt)
        & $Exe --fixture $Fixture --label $label --affinity $c.mask --send-cost-ns $CostNs --audio-percent $c.audio --few-cpu $c.opt --out $runs | Out-Null
    }
}

function Get-Value($line, $key) { if ($line -match "(?:^|\s)$key=(\S+)") { [double]$Matches[1] } else { [double]::NaN } }
function Get-Median($values) { $s = @($values | Sort-Object); if ($s.Count -eq 0) { return [double]::NaN }; $s[[int][math]::Floor(($s.Count - 1) / 2)] }
$lines = Get-Content $runs
$out = @()
$out += "few-cpu measurement $stamp   whole-PC load before the runs: $load %   rounds: $Rounds   send cost: $CostNs ns/message   (medians of the rounds)"
$out += "label                                 lag_ms   meps  ui_late_p50  ui_late_p99  ui_late_max  audio_cpu_ms"
foreach ($g in ($lines | Group-Object { ($_ -split ' ')[0] })) {
    $l = $g.Group
    $f = { param($k) Get-Median ($l | ForEach-Object { Get-Value $_ $k }) }
    $out += ("{0,-36} {1,7:N0} {2,6:N2} {3,11:N2} {4,12:N2} {5,12:N1} {6,12:N0}" -f $g.Name.Replace('label=', ''), (& $f 'lag_ms'), (& $f 'meps'), (& $f 'ui_wake_late_ms_p50'), (& $f 'p99'), (& $f 'max'), (& $f 'audio_cpu_ms'))
}
$out | Set-Content (Join-Path $dir 'summary.txt')
$out

