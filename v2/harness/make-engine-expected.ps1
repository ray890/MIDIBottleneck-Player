# Writes the digest list of v1's engine recordings into v2\harness\expected\engine\:
#   manifest.txt         SHA-256 of the scenario file (CRLF/CR read as LF), scenario and fixture counts,
#                        (the DD-001 line counts are pinned in expected\differences-from-v1.txt; this script recounts them in
#                        v1's recordings and refuses digests that disagree, see v2\docs\DIFFERENCES_FROM_V1.md)
#   <fixture>.digests    one line per scenario: <scenario> <SHA-256 of .out.txt> <SHA-256 of .sched.txt>
# The recordings are the v1 lock-step spike's output (v2\harness\v1-reference\spike; the real v1.0.44
# PlaybackEngine under a virtual clock). The digests are only written when every copy of a file is
# identical: across both architectures (x86 = x64) and across every run directory found.
#
#   make-engine-expected.ps1                 # use the newest full run: v2\out\spike\runs-lockstep-m5ext
#   make-engine-expected.ps1 -RunRoot DIR    # DIR holds x64\run*\ and x86\run*\ spike output
#   make-engine-expected.ps1 -Regenerate     # first run the spike (run-spike.ps1 -AllValid -Runs 2 -Tag engine-expected)
#   make-engine-expected.ps1 -Check          # do not write; compare with the committed digests (the v1 drift check)
#   make-engine-expected.ps1 -RunRoot DIR -Merge   # DIR holds the recordings of NEW scenarios only (run-spike.ps1 -Only ...);
#                                                  # their digests are merged into the committed lists (the others are kept);
#                                                  # the DD-001 pins are not checked: it prints the added short-SysEx line counts
param([string]$RunRoot = '', [switch]$Regenerate, [switch]$Check, [int]$Runs = 2, [switch]$Merge)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$scenarioFile = Join-Path $here 'engine-scenarios.txt'
$target = Join-Path $here 'expected\engine'
$rulesFile = Join-Path $here 'expected\differences-from-v1.txt'

if ($Regenerate) {
    $log = & (Join-Path $here 'v1-reference\spike\run-spike.ps1') -AllValid -Runs $Runs -Tag 'engine-expected'
    $log | Select-Object -Last 3 | ForEach-Object { Write-Host $_ }
    $RunRoot = Join-Path $repo 'v2\out\spike\runs-lockstep-engine-expected'
}
if ($RunRoot -eq '') { $RunRoot = Join-Path $repo 'v2\out\spike\runs-lockstep-m5ext' }
if (-not (Test-Path -LiteralPath $RunRoot)) { throw "Run directory not found: $RunRoot" }

# Scenario names (scenario file order) and the file's digest.
$scenarioText = [System.IO.File]::ReadAllText($scenarioFile).Replace("`r`n", "`n").Replace("`r", "`n")
$sha = [System.Security.Cryptography.SHA256]::Create()
function Hex([byte[]]$bytes) { ([System.BitConverter]::ToString($bytes)).Replace('-', '') }
$scenarioDigest = Hex ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($scenarioText)))
$scenarios = @([regex]::Matches($scenarioText, '(?m)^scenario\s+(\S+)') | ForEach-Object { $_.Groups[1].Value })
if ($scenarios.Count -eq 0) { throw 'no scenarios found in the scenario file' }
$sortedScenarios = [string[]]$scenarios.Clone()
[System.Array]::Sort($sortedScenarios, [System.StringComparer]::Ordinal)
$totalScenarioCount = $scenarios.Count

# Every run directory (arch\runN) with its files.
$dirs = @()
foreach ($arch in 'x64', 'x86') {
    $archDir = Join-Path $RunRoot $arch
    if (-not (Test-Path -LiteralPath $archDir)) { throw "Missing $archDir (both architectures are required)" }
    $dirs += @(Get-ChildItem -LiteralPath $archDir -Directory | Sort-Object Name)
}
if ($dirs.Count -lt 2) { throw 'need at least one run per architecture' }
Write-Host ("{0} run directories ({1})" -f $dirs.Count, (($dirs | ForEach-Object { $_.Parent.Name + '\' + $_.Name }) -join ', '))

function FileHash([string]$path) {
    $stream = [System.IO.File]::OpenRead($path)
    try { return Hex ($sha.ComputeHash($stream)) } finally { $stream.Dispose() }
}

# Fixture stems from the file names of the first run directory.
$first = $dirs[0]
$stems = @(Get-ChildItem -LiteralPath $first.FullName -Filter '*.out.txt' | ForEach-Object {
    $i = $_.Name.IndexOf('__'); $_.Name.Substring($i + 2, $_.Name.Length - $i - 2 - '.out.txt'.Length) } | Sort-Object -Unique)
Write-Host ("{0} fixtures, {1} scenarios" -f $stems.Count, $scenarios.Count)

# The short-SysEx normalizer (DD-001) pins, per fixture, how many v2 recording lines it maps to v1's zeros
# (engine rule ... normalize=short-sysex-zeroed pin=<fixture>:<lines>,... in the difference rules). This script
# recounts the v1 recordings' zeroed short-SysEx SEND lines and refuses digests that disagree with the pins.
$ddPins = @{}
foreach ($line in Get-Content -LiteralPath $rulesFile) {
    if ($line -match '^engine\s+DD-\d+\s+\S+\s+.*normalize=short-sysex-zeroed\s+.*pin=(\S+)') {
        foreach ($item in ($Matches[1] -split ',')) { $p = $item -split ':'; $ddPins[$p[0]] = [int]$p[1] }
    }
}
# The pins count v2's recording lines; in runs that a rule explains as different from v1 (v2 plays on where v1 stopped, DD-006)
# v2 has more of them than v1 has. A comment line "# v1-pin: <fixture>:<n>,..." in the rule file then gives the v1 counts.
$v1Pins = @{}
foreach ($line in Get-Content -LiteralPath $rulesFile) {
    if ($line -match '^#\s*v1-pin:\s*(\S+)') { foreach ($item in ($Matches[1] -split ',')) { $p = $item -split ':'; $v1Pins[$p[0]] = [int]$p[1] } }
}
foreach ($k in @($ddPins.Keys)) { if (-not $v1Pins.ContainsKey($k)) { $v1Pins[$k] = $ddPins[$k] } }
$ddStems = @($ddPins.Keys | Sort-Object)

if ($Merge) {
    $firstDir = @(Get-ChildItem -LiteralPath (Join-Path $RunRoot 'x64') -Directory | Sort-Object Name)[0]
    $newNames = @(Get-ChildItem -LiteralPath $firstDir.FullName -Filter '*.out.txt' | ForEach-Object { $_.Name.Substring(0, $_.Name.IndexOf('__')) } | Sort-Object -Unique)
    $sortedScenarios = [string[]]$newNames
    [System.Array]::Sort($sortedScenarios, [System.StringComparer]::Ordinal)
    Write-Host ("merge: {0} new scenarios: {1}" -f $sortedScenarios.Count, ($sortedScenarios -join ', '))
}
$problems = 0
$lists = @{}
$ddCounts = @{}
foreach ($stem in $stems) {
    $lines = New-Object System.Collections.Generic.List[string]
    $ddLines = 0
    foreach ($scenario in $sortedScenarios) {
        $hashes = @{}
        foreach ($kind in 'out', 'sched') {
            $name = "${scenario}__${stem}.$kind.txt"
            $seen = @{}
            foreach ($dir in $dirs) {
                $path = Join-Path $dir.FullName $name
                if (-not (Test-Path -LiteralPath $path)) { Write-Host "MISSING $($dir.Parent.Name)\$($dir.Name)\$name"; $problems++; continue }
                $h = FileHash $path
                $seen[$h] = 1 + [int]$seen[$h]
            }
            if ($seen.Count -ne 1) { Write-Host "DIVERGENT $name : $($seen.Count) distinct digests"; $problems++ }
            $hashes[$kind] = @($seen.Keys)[0]
        }
        $lines.Add("$scenario $($hashes['out']) $($hashes['sched'])")
        if ($ddStems -contains $stem) {
            $text = [System.IO.File]::ReadAllText((Join-Path $first.FullName "${scenario}__${stem}.out.txt"))
            $ddLines += [regex]::Matches($text, '(?m)^\d+ [WH] SEND SystemExclusive src=\S+ (?:00){1,3} cost=').Count
        }
    }
    $lists[$stem] = $lines
    if ($ddStems -contains $stem) { $ddCounts[$stem] = $ddLines }
}
if ($problems -ne 0) { throw "$problems problem(s): the v1 recordings are not identical across runs and architectures; no digests written" }
if ($Merge) {
    foreach ($stem in $stems) {
        $existing = @{}
        foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $target "$stem.digests"))) {
            if ($line.StartsWith('#') -or $line -eq '') { continue }
            $existing[($line -split ' ')[0]] = $line
        }
        foreach ($line in $lists[$stem]) { $existing[($line -split ' ')[0]] = $line }
        $names = [string[]]@($existing.Keys)
        [System.Array]::Sort($names, [System.StringComparer]::Ordinal)
        $merged = New-Object System.Collections.Generic.List[string]
        foreach ($n in $names) { $merged.Add($existing[$n]) }
        $lists[$stem] = $merged
    }
    foreach ($stem in $ddStems) { Write-Host ("merge: new zeroed short-SysEx SEND lines in v1 recordings of {0}: {1} (add to the DD-001 pin)" -f $stem, $ddCounts[$stem]) }
}

$manifest = New-Object System.Collections.Generic.List[string]
$manifest.Add('# Engine comparison manifest, written by v2/harness/make-engine-expected.ps1. See v2/harness/TRACE_FORMAT.md.')
$manifest.Add('format 1')
$manifest.Add("scenarios-sha256 $scenarioDigest")
$manifest.Add("scenarios $totalScenarioCount")
$manifest.Add("fixtures $($stems.Count)")
foreach ($stem in $ddStems) {
    if (-not $Merge -and $ddCounts[$stem] -ne $v1Pins[$stem]) { throw "the v1 recordings of $stem have $($ddCounts[$stem]) zeroed short-SysEx SEND lines, the DD-001 engine rule pins $($v1Pins[$stem]) for v1" }
}

if ($Check) {
    $drift = 0
    if (-not (Test-Path (Join-Path $target 'manifest.txt'))) { throw 'no committed manifest' }
    $committed = [System.IO.File]::ReadAllText((Join-Path $target 'manifest.txt')).Replace("`r`n", "`n")
    if ($committed -ne (($manifest -join "`n") + "`n")) { Write-Host 'DRIFT manifest'; $drift++ }
    foreach ($stem in $stems) {
        $path = Join-Path $target "$stem.digests"
        $want = "# v1 engine recordings of $stem`: SHA-256 of .out.txt and .sched.txt per scenario (make-engine-expected.ps1)`n" + (($lists[$stem] -join "`n") + "`n")
        if (-not (Test-Path $path) -or [System.IO.File]::ReadAllText($path).Replace("`r`n", "`n") -ne $want) { Write-Host "DRIFT $stem"; $drift++ }
    }
    Write-Host ("v1 engine recordings: {0} fixtures, {1} drifted lists" -f $stems.Count, $drift)
    if ($drift -ne 0) { exit 1 }
    exit 0
}

New-Item -ItemType Directory -Force -Path $target | Out-Null
if (-not $Merge) { Get-ChildItem -LiteralPath $target -File | Remove-Item }
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $target 'manifest.txt'), (($manifest -join "`n") + "`n"), $utf8)
foreach ($stem in $stems) {
    $head = "# v1 engine recordings of $stem`: SHA-256 of .out.txt and .sched.txt per scenario (make-engine-expected.ps1)`n"
    [System.IO.File]::WriteAllText((Join-Path $target "$stem.digests"), $head + (($lists[$stem] -join "`n") + "`n"), $utf8)
}
Write-Host ("Wrote {0} digest lists and manifest.txt to {1} (DD-001: {2})" -f $stems.Count, $target,
    (($ddCounts.Keys | Sort-Object | ForEach-Object { "$_ $($ddCounts[$_])" }) -join ', '))
