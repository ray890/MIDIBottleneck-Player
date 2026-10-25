# Real-time property comparison of the v1 and v2 playback engines on the None
# output (Milestone 5 exit test; v2/docs/ENGINE_PROPERTY_CHECKS.md,
# ARCHITECTURE.md section 5.1).
#
# Builds (unless -NoBuild) and runs v1-realtime.exe (v1's unchanged engine,
# real Stopwatch/HighResolutionWaiter/threads) and midibn-engine-realtime.exe
# (the v2 engine over the Win32 engine services) on x64 and x86, every scenario
# of v2\harness\engine-realtime-scenarios.txt on its fixtures, -Repeats times,
# against recording outputs that send nowhere (no MIDI device is opened, no
# provider DLL is loaded). Then checks every property on every log and the
# cross-run / cross-program equalities, prints PASS or FAIL per property and
# ends with ENGINE-PROPERTIES: PASS or FAIL.
#
#   .\v2\tools\engine-properties.ps1                      # build, run 5 repeats, check
#   .\v2\tools\engine-properties.ps1 -Repeats 3 -Only 'rt-drop-*,rt-queue-*'
#   .\v2\tools\engine-properties.ps1 -NoBuild -CheckOnly  # re-check the logs of the last run
#
# The four program/architecture lanes run in parallel by default (a 12-thread
# machine is not loaded by four single-worker processes); -Serial runs them
# one after another (about four times longer) for cleaner timing observations.
# Raw logs stay under v2\out\engine-properties\runs\ (untracked).
param(
    [int]$Repeats = 5,
    [string[]]$Archs = @('x64', 'x86'),
    [string]$Only = '',
    [switch]$NoBuild,
    [switch]$NoWarmup,
    [switch]$Serial,
    [switch]$CheckOnly,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if ($OutDir -eq '') { $OutDir = Join-Path $repo 'v2\out\engine-properties' }
$runsDir = Join-Path $OutDir 'runs'
$eventsDir = Join-Path $OutDir 'events'
$binDir = Join-Path $OutDir 'bin'
$scenarioFile = Join-Path $repo 'v2\harness\engine-realtime-scenarios.txt'
$fixtureDir = Join-Path $repo 'v2\harness\fixtures'
$generatedDir = Join-Path $OutDir 'fixtures'
$v1Exe = @{}; $v2Exe = @{}
foreach ($a in $Archs) {
    $v1Exe[$a] = Join-Path $repo "v2\out\realtime\v1\$a\v1-realtime.exe"
    $v2Exe[$a] = Join-Path $repo "v2\out\build-rt-$a\Release\midibn-engine-realtime.exe"
}
$checkerExe = Join-Path $binDir 'engine-property-checker.exe'
$overall = $true
$started = Get-Date

function Section($text) { Write-Host ''; Write-Host $text }

# ---------------------------------------------------------------- build
if (-not $NoBuild -and -not $CheckOnly) {
    Section 'Building'
    & (Join-Path $repo 'v2\harness\v1-reference\realtime\build-realtime.ps1')
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -property installationPath
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    foreach ($a in $Archs) {
        $dir = Join-Path $repo "v2\out\build-rt-$a"
        $platform = if ($a -eq 'x86') { 'Win32' } else { 'x64' }
        if (-not (Test-Path (Join-Path $dir 'CMakeCache.txt'))) {
            & $cmake -S (Join-Path $repo 'v2') -B $dir -G 'Visual Studio 17 2022' -A $platform -T v143 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $a" }
        }
        & $cmake --build $dir --config Release --target midibn-engine-realtime | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $a" }
        Write-Host "Built $($v2Exe[$a])"
    }
}
# The property checker is one small C# file (about a second to compile). It is rebuilt whenever it is missing or older
# than its source, also under -NoBuild/-CheckOnly: this script lists the properties from the SOURCE, so a stale checker
# executable would silently lack newer properties (L01/J01/K01 then read NOT-EXERCISED) and miss fixes.
$checkerSource = Join-Path $repo 'v2\tools\engine-properties\PropertyChecker.cs'
if (-not (Test-Path -LiteralPath $checkerExe) -or (Get-Item -LiteralPath $checkerExe).LastWriteTimeUtc -lt (Get-Item -LiteralPath $checkerSource).LastWriteTimeUtc) {
    New-Item -ItemType Directory -Force $binDir | Out-Null
    $csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $csc /nologo /target:exe /optimize+ /warn:4 "/out:$checkerExe" $checkerSource
    if ($LASTEXITCODE -ne 0) { throw 'checker compilation failed' }
    Write-Host "Built $checkerExe"
}
foreach ($exe in @($checkerExe) + @($v1Exe.Values) + @($v2Exe.Values)) {
    if (-not (Test-Path -LiteralPath $exe)) { throw "missing $exe (run without -NoBuild)" }
}

# ---------------------------------------------------------------- scenarios and fixtures
$scenarios = @()
$lines = Get-Content -LiteralPath $scenarioFile
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^scenario\s+(\S+)\s*$') {
        $name = $Matches[1]
        $fixtures = @()
        for ($j = $i + 1; $j -lt $lines.Count -and $lines[$j] -notmatch '^scenario\s'; $j++) {
            if ($lines[$j] -match '^\s*describe\s+.*fixture=([^\s|]+)') { $fixtures = $Matches[1] -split ','; break }
        }
        if ($fixtures.Count -eq 0) { throw "scenario $name has no fixture= tag" }
        $scenarios += [pscustomobject]@{ Name = $name; Fixtures = $fixtures }
    }
}
if ($Only -ne '') {
    $patterns = $Only -split ','
    $scenarios = @($scenarios | Where-Object { $n = $_.Name; ($patterns | Where-Object { $n -like $_ }).Count -gt 0 })
}
if ($scenarios.Count -eq 0) { throw 'no scenario selected' }
$configs = @()
foreach ($s in $scenarios) { foreach ($f in $s.Fixtures) { $configs += [pscustomobject]@{ Scenario = $s.Name; Fixture = $f } } }

# A synthetic song for the protected-traffic rules (generated here, never committed): 1 s of a dense mix with sustain,
# all-notes-off / all-sound-off / reset-all-controllers messages, overlapping notes, velocity-0 releases and orphan NoteOffs.
function New-ProtectedFixture([string]$path) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    function Vlq([System.Collections.Generic.List[byte]]$list, [long]$value) {
        $stack = New-Object System.Collections.Generic.List[byte]
        $stack.Add([byte]($value -band 0x7F)); $value = $value -shr 7
        while ($value -gt 0) { $stack.Add([byte](0x80 -bor ($value -band 0x7F))); $value = $value -shr 7 }
        for ($i = $stack.Count - 1; $i -ge 0; $i--) { $list.Add($stack[$i]) }
    }
    $track = New-Object System.Collections.Generic.List[byte]
    # tempo 125000 us per quarter at 960 ticks per quarter: 130 us per tick
    Vlq $track 0; $track.AddRange([byte[]](0xFF, 0x51, 0x03, 0x01, 0xE8, 0x48))
    Vlq $track 0; $track.AddRange([byte[]](0x90, 60, 100))
    $sounding = New-Object System.Collections.Generic.List[int]
    $sounding.Add(60)
    $script:rngState = [long]20261013
    function NextRandom { $script:rngState = ($script:rngState * 1103515245 + 12345) % 2147483648; return [int]($script:rngState -shr 8) }
    for ($n = 0; $n -lt 7000; $n++) {
        $d = (NextRandom) % 10
        $delta = if ($d -lt 3) { 0 } elseif ($d -lt 7) { 1 } else { 2 }
        Vlq $track $delta
        $q = (NextRandom) % 100
        $channel = (NextRandom) % 4
        $a = (NextRandom) % 128
        $b = (NextRandom) % 128
        if ($q -lt 35) {
            $key = 36 + $a % 49
            $track.AddRange([byte[]]((0x90 -bor $channel), $key, (1 + $b % 127))); $sounding.Add($channel * 128 + $key)
        } elseif ($q -lt 60) {
            if ($sounding.Count -gt 0 -and $q -lt 55) {
                $pick = $sounding[$a % $sounding.Count]; $sounding.Remove($pick) | Out-Null
                $track.AddRange([byte[]]((0x80 -bor [int][Math]::Floor($pick / 128)), ($pick % 128), 64))
            } else { $track.AddRange([byte[]]((0x80 -bor $channel), (36 + $a % 49), 0)) }   # orphan release
        } elseif ($q -lt 65) {
            if ($sounding.Count -gt 0) {
                $pick = $sounding[$a % $sounding.Count]; $sounding.Remove($pick) | Out-Null
                $track.AddRange([byte[]]((0x90 -bor [int][Math]::Floor($pick / 128)), ($pick % 128), 0))   # velocity-0 release
            } else { $track.AddRange([byte[]]((0xB0 -bor $channel), 7, $b)) }
        } elseif ($q -lt 75) { $track.AddRange([byte[]]((0xB0 -bor $channel), 64, ($(if (($a % 2) -eq 0) { 0 } else { 127 })))) }
        elseif ($q -lt 78) { $track.AddRange([byte[]]((0xB0 -bor $channel), 123, 0)) }
        elseif ($q -lt 80) { $track.AddRange([byte[]]((0xB0 -bor $channel), 120, 0)) }
        elseif ($q -lt 82) { $track.AddRange([byte[]]((0xB0 -bor $channel), 121, 0)) }
        elseif ($q -lt 90) { $track.AddRange([byte[]]((0xB0 -bor $channel), @(7, 10, 11)[$a % 3], $b)) }
        elseif ($q -lt 97) { $track.AddRange([byte[]]((0xE0 -bor $channel), $a, $b)) }
        else { $track.AddRange([byte[]]((0xA0 -bor $channel), (36 + $a % 49), $b)) }
    }
    foreach ($pick in $sounding) { Vlq $track 1; $track.AddRange([byte[]]((0x80 -bor [int][Math]::Floor($pick / 128)), ($pick % 128), 0)) }
    Vlq $track 1; $track.AddRange([byte[]](0xFF, 0x2F, 0x00))
    $bytes.AddRange([byte[]](0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0, 0x4D, 0x54, 0x72, 0x6B))
    $length = $track.Count
    $bytes.AddRange([byte[]]((($length -shr 24) -band 255), (($length -shr 16) -band 255), (($length -shr 8) -band 255), ($length -band 255)))
    $bytes.AddRange($track)
    [System.IO.File]::WriteAllBytes($path, $bytes.ToArray())
}

# ---------------------------------------------------------------- generated fixture and input check: both programs read the same song events
New-Item -ItemType Directory -Force $eventsDir | Out-Null
New-Item -ItemType Directory -Force $generatedDir | Out-Null
New-ProtectedFixture (Join-Path $generatedDir 'rt-protected.mid')
Copy-Item -LiteralPath (Join-Path $fixtureDir 'f0-all-message-types.mid') -Destination $generatedDir -Force   # the warm-up song is looked up next to the fixture
function Get-FixturePath([string]$stem) {
    $generated = Join-Path $generatedDir "$stem.mid"
    if (Test-Path -LiteralPath $generated) { return $generated }
    return Join-Path $fixtureDir "$stem.mid"
}
Section 'Input check: song event lists of v1 and v2'
$fixtureNames = @($configs | ForEach-Object { $_.Fixture } | Sort-Object -Unique)
$arch0 = $Archs[0]
$inputOk = $true
Get-ChildItem -LiteralPath $eventsDir -Filter '*.txt' | Remove-Item -Force
foreach ($f in $fixtureNames) {
    $path = Get-FixturePath $f
    $e1 = Join-Path $eventsDir "$f.v1.txt"
    $e2 = Join-Path $eventsDir "$f.v2.txt"
    & $v1Exe[$arch0] events $path $e1 | Out-Null
    & $v2Exe[$arch0] events $path $e2 | Out-Null
    $l1 = Get-Content -LiteralPath $e1
    $l2 = Get-Content -LiteralPath $e2
    # DD-001 (v2\docs\DIFFERENCES_FROM_V1.md): v1 reads a SysEx event of 1-3 stored bytes back as zeros, v2 keeps the file's bytes.
    $dd001 = 0; $other = 0
    if ($l1.Count -ne $l2.Count) { $other++ }
    else {
        for ($i = 0; $i -lt $l1.Count; $i++) {
            if ($l1[$i] -eq $l2[$i]) { continue }
            $a = $l1[$i] -split ' '; $b = $l2[$i] -split ' '
            if ($a.Count -eq 4 -and $b.Count -eq 4 -and $a[0] -eq $b[0] -and $a[1] -eq $b[1] -and $a[2] -eq 'SystemExclusive' -and $b[2] -eq 'SystemExclusive' -and
                $a[3] -match '^(00){1,3}$' -and $a[3].Length -eq $b[3].Length) { $dd001++ } else { $other++ }
        }
    }
    Write-Host ("  {0,-28} {1}  {2}" -f $f, $(if ($other -gt 0) { 'DIFFERENT' } elseif ($dd001 -gt 0) { "identical except $dd001 DD-001 short SysEx event(s)" } else { 'identical' }), $l1[0])
    if ($other -gt 0) { $inputOk = $false }
}
Write-Host ("INPUT (song events identical in v1 and v2, apart from DD-001): " + $(if ($inputOk) { 'PASS' } else { 'FAIL' }))
if (-not $inputOk) { $overall = $false }

# ---------------------------------------------------------------- runs
if (-not $CheckOnly) {
    if (Test-Path -LiteralPath $runsDir) { Remove-Item -LiteralPath $runsDir -Recurse -Force }
    New-Item -ItemType Directory -Force $runsDir | Out-Null
    $lanes = @()
    foreach ($a in $Archs) {
        $lanes += [pscustomobject]@{ Program = 'v1'; Arch = $a; Exe = $v1Exe[$a] }
        $lanes += [pscustomobject]@{ Program = 'v2'; Arch = $a; Exe = $v2Exe[$a] }
    }
    $jobsPerLane = @()
    foreach ($lane in $lanes) {
        $jobs = @()
        for ($r = 1; $r -le $Repeats; $r++) {
            foreach ($c in $configs) {
                $jobs += [pscustomobject]@{
                    Scenario = $c.Scenario; Fixture = $c.Fixture; Rep = $r; Path = (Get-FixturePath $c.Fixture)
                    Out = (Join-Path $runsDir ("{0}-{1}-{2}-{3}-r{4}.log" -f $lane.Program, $lane.Arch, $c.Scenario, $c.Fixture, $r))
                }
            }
        }
        $jobsPerLane += , $jobs
    }
    Section ("Running {0} configurations x {1} repeats x {2} lanes ({3}){4}" -f $configs.Count, $Repeats, $lanes.Count, $(if ($Serial) { 'serial' } else { 'parallel' }), $(if ($NoWarmup) { ', cold start' } else { '' }))

    $laneBlock = {
        param($exe, $jobs, $scenarioFile, $fixtureDir, $warm)
        $failed = @()
        foreach ($j in $jobs) {
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $exe
            $arguments = @('run', $scenarioFile, $j.Scenario, $j.Path, $j.Out)
            if ($warm) { $arguments += 'warm' }
            $psi.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.CreateNoWindow = $true
            $p = [System.Diagnostics.Process]::Start($psi)
            $null = $p.StandardOutput.ReadToEnd()
            if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {} ; $failed += "$($j.Scenario)/$($j.Fixture)/r$($j.Rep): timeout" }
            elseif ($p.ExitCode -ne 0) { $failed += "$($j.Scenario)/$($j.Fixture)/r$($j.Rep): exit $($p.ExitCode)" }
        }
        $failed
    }
    $handles = @()
    for ($i = 0; $i -lt $lanes.Count; $i++) {
        $handles += Start-Job -ScriptBlock $laneBlock -ArgumentList $lanes[$i].Exe, $jobsPerLane[$i], $scenarioFile, $fixtureDir, (-not $NoWarmup)
        if ($Serial) { $null = Wait-Job $handles[-1] }
    }
    $runFailures = @()
    for ($i = 0; $i -lt $lanes.Count; $i++) {
        $null = Wait-Job $handles[$i]
        $out = @(Receive-Job $handles[$i])
        foreach ($f in $out) { $runFailures += "$($lanes[$i].Program)-$($lanes[$i].Arch) $f" }
        Remove-Job $handles[$i]
    }
    $logCount = @(Get-ChildItem -LiteralPath $runsDir -Filter '*.log').Count
    $expectedLogs = $lanes.Count * $Repeats * $configs.Count
    Write-Host ("Runs finished: {0} of {1} logs, {2} process failure(s)" -f $logCount, $expectedLogs, $runFailures.Count)
    foreach ($f in $runFailures) { Write-Host "  RUN FAILURE: $f" }
    if ($logCount -ne $expectedLogs -or $runFailures.Count -gt 0) { $overall = $false }
}

# ---------------------------------------------------------------- checks
Section 'Checking'
$resultFile = Join-Path $OutDir 'results.txt'
& $checkerExe $eventsDir $runsDir $resultFile | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'checker failed' }
$result = Get-Content -LiteralPath $resultFile
$props = @{}
$order = @()
$groups = @()
foreach ($a in $Archs) { $groups += "v1-$a"; $groups += "v2-$a" }
$descriptions = @{}
# property texts come from the checker's source so that the script and the checker cannot drift
$source = Get-Content -LiteralPath (Join-Path $repo 'v2\tools\engine-properties\PropertyChecker.cs')
foreach ($line in $source) {
    if ($line -match '^\s*new\[\] \{ "([A-Z][0-9]{2})", "(.*)" \},?\s*$') { $descriptions[$Matches[1]] = $Matches[2]; $order += $Matches[1] }
}
foreach ($line in $result) {
    if ($line -match '^PROP (\S+) (\S+) (\d+) (\d+)$') {
        if (-not $props.ContainsKey($Matches[1])) { $props[$Matches[1]] = @{} }
        $props[$Matches[1]][$Matches[2]] = @{ Checks = [int64]$Matches[3]; Failures = [int64]$Matches[4] }
    }
}
$crossIds = @('X01', 'X02', 'X03')
Write-Host ''
Write-Host ('{0,-5} {1,-70} {2}' -f 'ID', 'PROPERTY', (($groups | ForEach-Object { '{0,-16}' -f $_ }) -join ' '))
$failedProps = @()
foreach ($id in $order) {
    $cells = @()
    $propOk = $true
    foreach ($g in $groups) {
        $r = if ($props.Contains($id)) { $props[$id][$g] } else { $null }   # a property no run exercised has no entry at all
        if ($r -eq $null -or $r.Checks -eq 0) {
            $cells += ('{0,-16}' -f 'NOT-EXERCISED'); $propOk = $false
        }
        elseif ($r.Failures -gt 0) { $cells += ('{0,-16}' -f ("FAIL {0}/{1}" -f $r.Failures, $r.Checks)); $propOk = $false }
        else { $cells += ('{0,-16}' -f ("PASS {0}" -f $r.Checks)) }
    }
    $text = $descriptions[$id]
    if ($text.Length -gt 68) { $text = $text.Substring(0, 65) + '...' }
    Write-Host ('{0,-5} {1,-70} {2}' -f $id, $text, ($cells -join ' '))
    if (-not $propOk) { $failedProps += $id }
}
Write-Host ''
Write-Host 'Properties per line: PASS n = n checks passed in that program/architecture; the X lines compare each log with the first log of its configuration.'
foreach ($line in $result) {
    if ($line -match '^FAIL (\S+) (\S+) (.*)$') {
        Write-Host ("  FAIL {0} {1}: {2}" -f $Matches[1], $Matches[2], $Matches[3])
    }
}
$notes = @($result | Where-Object { $_ -like 'NOTE *' })
if ($notes.Count -gt 0) {
    Section 'Notes (v1 behaviour that the checks had to account for)'
    foreach ($n in $notes) { Write-Host ('  ' + $n.Substring(5)) }
}
$info = @($result | Where-Object { $_ -like 'INFO *' })
if ($info.Count -gt 0) {
    Section 'Observations (information only, not pass/fail; every line is in results.txt)'
    $stable = @($info | Where-Object { $_ -like '*SPREAD*STABLE*' })
    foreach ($n in $info) {
        if ($n -like '*SPREAD*STABLE*') { continue }
        if ($n -like '*LAG *' -and $n -notmatch 'rt-queue-dense|rt-queue-ordered-dense|rt-chase-mid') { continue }
        Write-Host ('  ' + $n.Substring(5))
    }
    Write-Host ("  {0} non-exact configuration(s) gave identical processed/dropped counts in every run of all four lanes (listed as STABLE in results.txt)" -f $stable.Count)
}
if ($failedProps.Count -gt 0) { $overall = $false }
$elapsed = (Get-Date) - $started
Write-Host ''
Write-Host ("Elapsed {0:N0} s" -f $elapsed.TotalSeconds)
Write-Host ("ENGINE-PROPERTIES: " + $(if ($overall) { 'PASS' } else { 'FAIL' }))
if (-not $overall) { exit 1 }
