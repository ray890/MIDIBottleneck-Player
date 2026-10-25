# Launches midibn-win32.exe under the native-dialog/process monitor (v1's archive\v1\tools\NativeWindowMonitor.cs,
# compiled untracked into v2\out\monitor) for every build that exists, as the release protocol requires:
#   1. prove the window detector  (--native-window-monitor-fixture, expects the proof window title)
#   2. prove the descendant-process detector (--native-process-monitor-fixture, expects a midibn-win32 child)
#   3. real run: open a committed fixture, close after -AutoCloseMs via WM_CLOSE, watch 5 s after exit.
#   4. companion windows (unless -SkipCompanions): the player is started under the same monitor, output None, and
#      driven (v2\tools\device\player-e2e.ps1 -Scenario Companions) through: Play, Analysis, Channels, Stop, Track routing
#      (Cancel), Help, About, and closed. The monitor therefore watches the launch AND the opening and closing of every
#      companion window; a native error window, an error-reporting process, an unexpected message box, a hung window or a
#      non-zero exit code fails the build.
# Everything runs on a private HIDDEN desktop (v2\tools\visual\run-hidden.ps1): nothing appears on any monitor and no
# focus is taken. -Visible runs it on the current desktop instead (the windows then appear on the screen).
# Logs (JSON lines) stay in v2\out\monitor\ (companion runs: v2\out\monitor\companions-<build>\ with pictures).
# Exit code 0 only if every proof and every run passed. Output None only: no sound, no device opened.
param(
    [string[]]$Builds = @('x64', 'x86', 'xp', 'xp-ia32'),
    [int]$AutoCloseMs = 3000,
    [string]$Fixture = 'v2\harness\fixtures\tempo-changes.mid',
    [switch]$SkipCompanions,
    [switch]$Visible,
    [string]$Position = '3880,1133',   # with -Visible: where every launch opens (--position), inside Internal Display (\\.\DISPLAY1 at 3840,1073)
    [switch]$Inner
)
$ErrorActionPreference = 'Stop'
$Builds = @($Builds | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

if (-not $Inner -and -not $Visible) {
    # (hidden desktop: nothing reaches a monitor, so no --position is needed)
    $a = "-Inner -Builds $($Builds -join ',') -AutoCloseMs $AutoCloseMs -Fixture '$Fixture'"
    if ($SkipCompanions) { $a += ' -SkipCompanions' }
    & (Join-Path $root 'v2\tools\visual\run-hidden.ps1') -File $MyInvocation.MyCommand.Path -Arguments $a -WorkingDirectory $root -TimeoutMinutes 60
    exit $LASTEXITCODE
}

$outDir = Join-Path $root 'v2\out\monitor'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$monitor = Join-Path $outDir 'NativeWindowMonitor.exe'
$source = Join-Path $root 'archive\v1\tools\NativeWindowMonitor.cs'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $monitor) -or (Get-Item $source).LastWriteTime -gt (Get-Item $monitor).LastWriteTime) {
    & $compiler /nologo /target:exe /platform:anycpu /optimize+ /warn:4 "/out:$monitor" $source
    if ($LASTEXITCODE -ne 0) { throw 'Native-window monitor compilation failed.' }
}
$fixturePath = Join-Path $root $Fixture
$failed = $false
$results = @()
foreach ($build in $Builds) {
    $exe = Join-Path $root "v2\out\build-$build\Release\midibn-win32.exe"
    if (-not (Test-Path -LiteralPath $exe)) { Write-Host "[$build] not built, skipped"; continue }
    $pos = if ($Visible) { @('--position', $Position) } else { @() }   # on the real desktop every launch is placed explicitly
    $win = Join-Path $outDir "window-proof-$build.jsonl"
    & $monitor --output $win --post-exit-ms 2000 --match 'Deliberately identifiable detector proof window' --expect-match -- $exe @pos --native-window-monitor-fixture | Out-Null
    $windowProof = $LASTEXITCODE
    $proc = Join-Path $outDir "process-proof-$build.jsonl"
    & $monitor --output $proc --post-exit-ms 2000 --process-match 'midibn-win32' --expect-process-match -- $exe @pos --native-process-monitor-fixture | Out-Null
    $processProof = $LASTEXITCODE
    $real = Join-Path $outDir "launch-$build.jsonl"
    $realCode = -1
    if ($windowProof -eq 0 -and $processProof -eq 0) {
        & $monitor --output $real --post-exit-ms 5000 --timeout-ms ($AutoCloseMs + 60000) -- $exe @pos --no-settings --auto-close-ms $AutoCloseMs --open $fixturePath | Out-Null
        $realCode = $LASTEXITCODE
    }
    $companions = 'skipped'
    if (-not $SkipCompanions -and $windowProof -eq 0 -and $processProof -eq 0) {
        $cdir = Join-Path $outDir "companions-$build"
        Remove-Item -LiteralPath $cdir -Recurse -Force -ErrorAction SilentlyContinue
        $lines = @()
        try {
            $e2eVisible = if ($Visible) { @('-Visible') } else { @() }   # player-e2e -Visible opens the player with --position 3880,1133
            $lines = @(& (Join-Path $root 'v2\tools\device\player-e2e.ps1') -Inner -None @e2eVisible -Scenario Companions -Build $build -OutDir $cdir *>&1 | ForEach-Object { "$_" })
            $ec = $LASTEXITCODE
        }
        catch { $ec = 1; $lines += "exception: $($_.Exception.Message)" }
        New-Item -ItemType Directory -Force -Path $cdir | Out-Null
        $lines | Set-Content -LiteralPath (Join-Path $cdir 'companions.log') -Encoding UTF8
        $verdict = $lines | Where-Object { $_ -like 'PLAYER-E2E:*' } | Select-Object -Last 1
        $companions = if ($ec -eq 0 -and $verdict -like 'PLAYER-E2E: PASS*') { 'PASS' } else { 'FAIL' }
        Write-Host ("[{0}] companion windows under the monitor: {1}  ({2})" -f $build, $companions, $verdict)
        if ($companions -eq 'FAIL') { $lines | Where-Object { $_ -like 'FAIL*' -or $_ -like '  - *' } | ForEach-Object { Write-Host "     $_" } }
    }
    $pass = ($windowProof -eq 0 -and $processProof -eq 0 -and $realCode -eq 0 -and $companions -ne 'FAIL')
    if (-not $pass) { $failed = $true }
    $results += [pscustomobject]@{ Build = $build; WindowProof = $windowProof; ProcessProof = $processProof; RealRun = $realCode; Companions = $companions; Verdict = $(if ($pass) { 'PASS' } else { 'FAIL' }) }
}
$results | Format-Table -AutoSize | Out-String | Write-Host
if ($failed -or $results.Count -eq 0) { Write-Host 'MONITOR-LAUNCH: FAIL'; exit 1 }
Write-Host 'MONITOR-LAUNCH: PASS'
exit 0
