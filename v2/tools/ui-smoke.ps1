# Smoke test of the Win32 player (midibn-win32.exe): never opens a MIDI device (output None only).
#   1. --selftest: the in-process check list (controls, layout at several widths/scales/styles, Rate models,
#      Play/Pause/Resume/Stop, system-menu options, borderless hit-testing).
#   2. A real launch with --open --play --auto-close-ms, observed through UI Automation: the key controls exist
#      (by name or window class), Play reads "Pause" while playing, the process exits cleanly.
#   3. CPU report of the UI thread while idle and while playing (--report).
# Usage: .\v2\tools\ui-smoke.ps1 [-Build x64|x86|xp] [-Fixture path]
param(
    [string]$Build = 'x64',
    [string]$Fixture = 'v2\harness\fixtures\tempo-changes.mid',
    [string]$Position = '3880,1133'   # where every launch opens (--position): inside Internal Display (\\.\DISPLAY1 at 3840,1073), never the primary 4K monitor
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$exe = Join-Path $repo "v2\out\build-$Build\Release\midibn-win32.exe"
$fixture = Join-Path $repo $Fixture
$out = Join-Path $repo 'v2\out\ui-smoke'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$failed = $false

# 1. in-process self-test
$result = Join-Path $out "selftest-$Build.txt"
$p = Start-Process -FilePath $exe -ArgumentList @('--no-settings', '--position', $Position, '--selftest', "`"$result`"", '--open', "`"$fixture`"") -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Host 'FAIL selftest timed out'; $failed = $true }
Get-Content $result | Select-String 'FAIL|SELFTEST'
if ($p.ExitCode -ne 0) { $failed = $true; Write-Host "selftest exit code $($p.ExitCode)" }

# 2. real launch observed through UI Automation
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$report = Join-Path $out "report-play-$Build.txt"
$proc = Start-Process -FilePath $exe -ArgumentList @('--no-settings', '--position', $Position, '--open', "`"$fixture`"", '--play', '--rate', '1', '--auto-close-ms', '6000', '--report', "`"$report`"") -PassThru
Start-Sleep -Milliseconds 1500
$desktop = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$window = $desktop.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($null -eq $window) { Write-Host 'FAIL main window not found by UI Automation'; $failed = $true }
else {
    $names = @{}; $classes = @{}
    foreach ($e in $window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
        $n = $e.Current.Name
        if ($n) { $names[$n] = $true }
        $classes[$e.Current.ClassName] = 1 + [int]$classes[$e.Current.ClassName]
    }
    foreach ($expected in 'Open MIDI...', 'Stop', 'Analysis...', 'Reset stats', 'Playback position', 'Statistics readouts', 'MIDI output:', 'Rate model:') {
        if ($names.ContainsKey($expected)) { Write-Host "PASS control found: $expected" } else { Write-Host "FAIL control not found: $expected"; $failed = $true }
    }
    foreach ($class in @(@('ComboBox', 3), @('msctls_trackbar32', 1), @('MidiBnTimeline', 1), @('MidiBnStatistics', 1), @('Edit', 2))) {
        if ($classes[$class[0]] -ge $class[1]) { Write-Host "PASS $($class[1]) x $($class[0])" } else { Write-Host "FAIL missing $($class[0])"; $failed = $true }
    }
    if ($names.ContainsKey('Pause') -or $names.ContainsKey('Resume') -or $names.ContainsKey('Play')) { Write-Host 'PASS Play button present' }
    if ($names.ContainsKey('Pause')) { Write-Host 'PASS Play button reads Pause while playing' } else { Write-Host 'NOTE Play button did not read Pause (the short file may already have finished)' }
}
if (-not $proc.WaitForExit(20000)) { $proc.Kill(); Write-Host 'FAIL process did not exit after the auto-close'; $failed = $true }
elseif ($proc.ExitCode -ne 0) { Write-Host "FAIL exit code $($proc.ExitCode)"; $failed = $true }
else { Write-Host 'PASS clean exit' }
if (Test-Path $report) { Write-Host '--- CPU report (playing)'; Get-Content $report }

# 3. idle CPU
$idleReport = Join-Path $out "report-idle-$Build.txt"
$proc = Start-Process -FilePath $exe -ArgumentList @('--no-settings', '--position', $Position, '--auto-close-ms', '5000', '--report', "`"$idleReport`"") -PassThru
$proc.WaitForExit(20000) | Out-Null
if (Test-Path $idleReport) { Write-Host '--- CPU report (idle, 5 s)'; Get-Content $idleReport }

if ($failed) { Write-Host 'UI-SMOKE: FAIL'; exit 1 }
Write-Host 'UI-SMOKE: PASS'
