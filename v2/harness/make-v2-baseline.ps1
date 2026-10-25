# Regenerates v2's own baseline, v2\harness\expected-v2\, and prints which entries changed.
#
# The baseline holds v2's recorded output, so that later v2 changes are still checked exactly even though v1 is no
# longer the exact reference (v2\docs\DIFFERENCES_FROM_V1.md):
#   traces.sha256         SHA-256 of every v2 trace the parity runner produces (load/index, load-flow, model, large),
#                         comment lines removed
#   text\                 the full v2 text of the traces that differ from v1 (small; lets a mismatch show its first differing line)
#   engine\manifest.txt   hash of the scenario file the engine digests were made with, scenario and fixture counts
#   engine\<fixture>.digests   SHA-256 of v2's raw .out and .sched recording of every scenario
# The parity runner checks v2's output against it exactly (second tier, independent of the v1 rules); a mismatch fails.
# Regenerating the baseline is a deliberate act: do it only for a change that is explained in the improvement
# register (v2\docs\DIFFERENCES_FROM_V1.md), then commit the register entry, the rules and the new baseline together.
#
#   make-v2-baseline.ps1               # regenerate (x64 build), print what changed, replace v2\harness\expected-v2\
#   make-v2-baseline.ps1 -Check        # only print what would change; exit 1 if anything would
#   make-v2-baseline.ps1 -Build x86    # use another built variant (the baseline is identical for x64, x86, xp, xp-ia32)
# Prerequisites: a built variant (v2\BUILDING.md) and the large fixtures (v2\harness\fixtures\generate.ps1 -Large).
param([ValidateSet('x64', 'x86', 'xp', 'xp-ia32')][string]$Build = 'x64', [switch]$Check)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo
[Environment]::CurrentDirectory = $repo
$exe = switch ($Build) {
    'x64' { 'v2\out\build-x64\Release\midibn-parity.exe' }
    'x86' { 'v2\out\build-x86\Release\midibn-parity.exe' }
    'xp' { 'v2\out\build-xp\Release\v2-xp-selftest.exe' }
    'xp-ia32' { 'v2\out\build-xp-ia32\Release\v2-xp-selftest-ia32.exe' }
}
if (-not (Test-Path $exe)) { throw "Build the $Build variant first (v2\BUILDING.md): missing $exe" }
$large = 'v2\harness\fixtures\large\large-8m.mid'
$largeDir = 'v2\harness\fixtures\large'
foreach ($f in $large, "$largeDir\routing-held-1000000.mid", "$largeDir\routing-held-1000001.mid") {
    if (-not (Test-Path $f)) { throw "Missing ${f}: the baseline includes the large fixtures; run v2\harness\fixtures\generate.ps1 -Large" }
}
$committed = 'v2\harness\expected-v2'
$new = 'v2\out\baseline-new'
if (Test-Path $new) { Remove-Item -Recurse -Force $new }

$arguments = @('--fixtures', 'v2\harness\fixtures', '--expected', 'v2\harness\expected', '--scenarios', 'v2\harness\engine-scenarios.txt',
    '--large', $large, '--large-expected', 'v2\harness\expected\large-8m-mid.summary.trace',
    '--large-model', 'v2\harness\expected\models-large\routing-large.trace', '--large-model-fixtures', $largeDir,
    '--write-baseline', $new)
Write-Host "Running $Build to produce the v2 baseline (about a minute)..."
$output = & $exe @arguments
$exit = $LASTEXITCODE
$output | Where-Object { $_ -match '^(FAIL|OK v2 baseline|note rule)' } | ForEach-Object { Write-Host "  $_" }
if ($exit -ne 0 -or -not (Test-Path "$new\traces.sha256")) { throw 'The parity runner could not write the baseline.' }

# Entries as "key" -> text, for traces (name -> digest) and engine recordings (fixture/scenario/file -> digest).
function Read-Entries([string]$dir) {
    $entries = @{}
    $traces = Join-Path $dir 'traces.sha256'
    if (Test-Path $traces) {
        foreach ($line in [IO.File]::ReadAllLines($traces)) {
            if ($line -eq '' -or $line.StartsWith('#')) { continue }
            $p = $line -split ' '
            $entries["trace  $($p[0])"] = $p[1]
        }
    }
    $engine = Join-Path $dir 'engine'
    if (Test-Path $engine) {
        foreach ($file in Get-ChildItem $engine -Filter '*.digests') {
            foreach ($line in [IO.File]::ReadAllLines($file.FullName)) {
                if ($line -eq '' -or $line.StartsWith('#')) { continue }
                $p = $line -split ' '
                $entries["engine $($file.BaseName) / $($p[0]) .out"] = $p[1]
                $entries["engine $($file.BaseName) / $($p[0]) .sched"] = $p[2]
            }
        }
        $manifest = Join-Path $engine 'manifest.txt'
        if (Test-Path $manifest) {
            foreach ($line in [IO.File]::ReadAllLines($manifest)) {
                if ($line -match '^(scenarios-sha256|scenarios|fixtures) (.+)$') { $entries["engine manifest $($Matches[1])"] = $Matches[2] }
            }
        }
    }
    return $entries
}
$old = Read-Entries $committed
$fresh = Read-Entries $new
$changed = @(); $added = @(); $removed = @()
foreach ($key in ($fresh.Keys | Sort-Object)) {
    if (-not $old.ContainsKey($key)) { $added += $key }
    elseif ($old[$key] -ne $fresh[$key]) { $changed += $key }
}
foreach ($key in ($old.Keys | Sort-Object)) { if (-not $fresh.ContainsKey($key)) { $removed += $key } }

$report = @()
foreach ($k in $changed) { $report += "CHANGED $k" }
foreach ($k in $added) { $report += "ADDED   $k" }
foreach ($k in $removed) { $report += "REMOVED $k" }
Write-Host ("v2 baseline: {0} entries; {1} changed, {2} added, {3} removed" -f $fresh.Count, $changed.Count, $added.Count, $removed.Count)
# Traces one by one; engine recordings grouped by fixture (there are 340 per fixture), full list in a file.
$reportFile = 'v2\out\baseline-changes.txt'
[IO.File]::WriteAllLines((Join-Path $repo $reportFile), [string[]]$report)
$shown = 0
$engineByFixture = [ordered]@{}
foreach ($line in $report) {
    if ($line -match '^(CHANGED|ADDED  |REMOVED) engine (\S+) / (\S+) (\.out|\.sched)$') {
        $key = "$($Matches[1].Trim()) engine $($Matches[2])"
        if (-not $engineByFixture.Contains($key)) { $engineByFixture[$key] = New-Object System.Collections.Generic.List[string] }
        $engineByFixture[$key].Add("$($Matches[3])$($Matches[4])")
    }
    elseif ($shown++ -lt 40) { Write-Host "  $line" }
}
foreach ($key in $engineByFixture.Keys) {
    $items = $engineByFixture[$key]
    $sample = ($items | Select-Object -First 3) -join ', '
    Write-Host ("  {0}: {1} recording file(s), e.g. {2}{3}" -f $key, $items.Count, $sample, $(if ($items.Count -gt 3) { ', ...' } else { '' }))
}
if ($shown -gt 40) { Write-Host "  ... more trace entries (all of them in $reportFile)" }
Write-Host "  (full list: $reportFile)"
if ($report.Count -eq 0) { Write-Host 'The committed baseline is already current; nothing to change.'; exit 0 }
if ($Check) { Write-Host 'BASELINE CHECK: the committed baseline differs from what v2 produces now (not written: -Check)'; exit 1 }

if (Test-Path $committed) { Remove-Item -Recurse -Force $committed }
Copy-Item -Recurse $new $committed
Write-Host "Wrote $committed. This is a deliberate act: record the change in v2\docs\DIFFERENCES_FROM_V1.md (and the rules in v2\harness\expected\differences-from-v1.txt) and commit them together."
