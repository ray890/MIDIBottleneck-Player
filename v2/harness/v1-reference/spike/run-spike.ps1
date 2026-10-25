# Runs the rendezvous spike $Runs times per architecture, each run in a fresh
# process with a real-time timeout, then compares SHA-256 of every recorded
# file across runs and across x86/x64. Outputs go to v2\out\spike\runs-<Mode>
# (plus -<Tag> when given). Before the runs, each architecture's built-in self
# test of the EventWaitHandle stand-in is run (lockstep mode only).
# -Mode free runs the negative control (no rendezvous; see LockStepScheduler).
# Hang protection: every scheduler wait has a 20 s real-time limit, the exe has
# an in-process watchdog (SPIKE_WATCHDOG_SECONDS, default 300 s; exit code 3),
# and this script kills a process after -TimeoutSeconds and counts a timeout.
# -AllValid uses the 23 valid fixtures of the Milestone 3 evidence matrix (the
# six default fixtures of the exe are a subset); -Fixtures takes names (comma
# separated or a list).
# -ScenarioFile selects the scenario file (default: the committed
# v2\harness\engine-scenarios.txt, specified in v2\harness\ENGINE_SCENARIOS.md);
# -Only restricts the run to scenario names (comma separated; a trailing * is a
# prefix wildcard), e.g. -Only 'gate-*,route-*'.
param([int]$Runs = 5, [int]$TimeoutSeconds = 600, [string[]]$Fixtures = @(), [string]$Mode = 'lockstep', [string]$Tag = '', [switch]$AllValid,
    [string]$ScenarioFile = '', [string]$Only = '')

$ErrorActionPreference = 'Stop'
$Fixtures = @($Fixtures | ForEach-Object { $_ -split "," } | Where-Object { $_ })
if ($AllValid) {
    $Fixtures = @("decimal-halfway.mid", "decimal-stress.mid", "empty-tracks.mid", "f0-all-message-types.mid",
        "f1-cross-track-tempo.mid", "f1-v1-test-midi.mid", "format0-two-tracks.mid", "header-extra-bytes.mid",
        "index-chase-rich.mid", "long-sysex-segment-crossing.mid", "many-events-segments.mid", "many-tracks-ties.mid",
        "metadata-edge.mid", "metadata-names.mid", "models-dense.mid", "models-notes.mid", "rmid-plain-odd.rmi",
        "rmid-wrapped.rmi", "running-status-repair.mid", "sysex-short.mid", "sysex-variants.mid",
        "tempo-changes.mid", "tempo-edge-values.mid")
}
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$fixtureDir = Join-Path $repo 'v2\harness\fixtures'
if ($ScenarioFile -eq '') { $ScenarioFile = Join-Path $repo 'v2\harness\engine-scenarios.txt' }
$name = 'runs-' + $Mode
if ($Tag -ne '') { $name += '-' + $Tag }
$root = Join-Path $repo ('v2\out\spike\' + $name)
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
New-Item -ItemType Directory -Force -Path $root | Out-Null

$hangs = 0
$failures = 0

if ($Mode -ne 'free') {
    foreach ($arch in 'x86', 'x64') {
        $exe = Join-Path $repo "v2\out\spike\$arch\rendezvous-spike.exe"
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = '--selftest'
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $proc = [System.Diagnostics.Process]::Start($psi)
        $stdout = $proc.StandardOutput.ReadToEndAsync()
        if (-not $proc.WaitForExit(60000)) { $proc.Kill(); $hangs++; Write-Host "$arch selftest TIMEOUT"; continue }
        $proc.WaitForExit()
        $text = $stdout.Result
        $text | Set-Content -LiteralPath (Join-Path $root "selftest-$arch.txt") -Encoding utf8
        if ($proc.ExitCode -ne 0) { $failures++; Write-Host $text }
        Write-Host ("{0} selftest: exit={1} ({2} PASS lines)" -f $arch, $proc.ExitCode, ([regex]::Matches($text, '(?m)^PASS ')).Count)
    }
}

foreach ($arch in 'x86', 'x64') {
    $exe = Join-Path $repo "v2\out\spike\$arch\rendezvous-spike.exe"
    for ($run = 1; $run -le $Runs; $run++) {
        $dir = Join-Path $root "$arch\run$run"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $options = @('--scenarios', ('"' + $ScenarioFile + '"'))
        if ($Only -ne '') { $options += @('--only', ('"' + $Only + '"')) }
        $quoted = $options + @('"' + $fixtureDir + '"', '"' + $dir + '"') + $Fixtures
        $psi.Arguments = ($quoted -join ' ')
        $psi.UseShellExecute = $false
        if ($Mode -eq 'free') { $psi.EnvironmentVariables['SPIKE_MODE'] = 'free' }
        $psi.RedirectStandardOutput = $true
        $proc = [System.Diagnostics.Process]::Start($psi)
        $stdout = $proc.StandardOutput.ReadToEndAsync()
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
            $proc.Kill(); $hangs++
            Write-Host "$arch run$run TIMEOUT after $TimeoutSeconds s"
            continue
        }
        $proc.WaitForExit()
        $stdout.Result | Set-Content -LiteralPath (Join-Path $dir 'console.txt') -Encoding utf8
        if ($proc.ExitCode -ne 0) {
            # DD-006: v1's ordered path freezes in the scenario ordered-override-pause-set-clear-resume (the harness ends it
            # with its livelock guard and the spike reports ERROR). That is the recorded v1 behaviour, not a process failure.
            $errorLines = @(Get-Content -LiteralPath (Join-Path $dir 'console.txt') | Where-Object { $_ -match ' ERROR' })
            $unexpected = @($errorLines | Where-Object { $_ -notmatch '^\W*ordered-override-pause-set-clear-resume ' })
            if ($errorLines.Count -eq 0 -or $unexpected.Count -ne 0) { $failures++ }
        }
        Write-Host ("{0} run{1}: exit={2} wall={3:N1}s" -f $arch, $run, $proc.ExitCode, $timer.Elapsed.TotalSeconds)
    }
}

# Compare every recorded file by SHA-256 across all runs and both architectures.
$table = @{}
Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Name -like '*.out.txt' -or $_.Name -like '*.sched.txt' } | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    if (-not $table.ContainsKey($_.Name)) { $table[$_.Name] = @{} }
    $table[$_.Name][$hash] = 1 + [int]$table[$_.Name][$hash]
}
$divergent = 0
foreach ($file in ($table.Keys | Sort-Object)) {
    $hashes = $table[$file]
    $count = ($hashes.Values | Measure-Object -Sum).Sum
    if ($hashes.Count -ne 1) {
        $divergent++
        Write-Host ("DIVERGENT {0}: {1}" -f $file, (($hashes.GetEnumerator() | ForEach-Object { $_.Key.Substring(0, 16) + 'x' + $_.Value }) -join ', '))
    }
    else { Write-Host ("same      {0}  {1}  ({2} copies)" -f (@($hashes.Keys)[0]), $file, $count) }
}
Write-Host ("Files compared: {0}; identical across all runs and architectures: {1}; divergent: {2}; process failures: {3}; timeouts: {4}" -f $table.Count, ($table.Count - $divergent), $divergent, $failures, $hangs)
if ($divergent -ne 0 -or $failures -ne 0 -or $hangs -ne 0) { exit 1 }
