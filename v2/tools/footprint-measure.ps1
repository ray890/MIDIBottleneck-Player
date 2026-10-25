# Footprint measurements (Milestone 7): MIDI load time and peak memory, player start-up time, executable sizes.
# Repeatable by anyone: it measures the builds in v2\out\build-<variant>\Release, and optionally compares them with an
# earlier set of builds (-Before) in INTERLEAVED order (A, B, A, B, ...) so both see the same machine load.
#
#   .\v2\tools\footprint-measure.ps1                                  # everything for x64, x86, xp, xp-ia32
#   .\v2\tools\footprint-measure.ps1 -Variants x64 -Runs 9 -Output out.md
#   .\v2\tools\footprint-measure.ps1 -Before D:\saved-builds          # A/B: <Before>\<variant>\midibn-perf.exe (+ midibn-win32.exe)
#   .\v2\tools\footprint-measure.ps1 -Mode Sizes                      # only the executable sizes
#   .\v2\tools\footprint-measure.ps1 -IncludeV1                       # add v1's load time, peak memory and executable size (x64, x86) for reference
#   .\v2\tools\footprint-measure.ps1 -Mode Capacity                   # the biggest file the 32-bit builds can load (makes files of up to 0.7 GB in %TEMP%)
#
# Sections (-Mode All runs Sizes, Load, Flow and Startup; Capacity is separate because it makes big files):
#   Sizes    byte sizes of midibn-win32.exe and midibn-perf.exe per variant (and of the -Before builds)
#   Load     fresh-process load of the large fixture by midibn-perf: load ms (the parser's own call), peak working set,
#            peak private bytes; one untimed warm-up per build first, then -Runs interleaved timed runs; medians
#   Flow     the load as the player runs it (midibn-load-flow): the inspection pass that a big file gets first (files of
#            15 MiB and more in a 32-bit build, 63 MiB and more in a 64-bit one; forced here for every build), then the
#            parse; whole flow, inspection and parse in ms
#   Capacity the number of events the 32-bit builds (x86, xp, xp-ia32) can load: files made by repeating the fixture's
#            16 tracks 9, 10, 13, 17 and 21 times (72 ... 168 million events) are loaded one after the other
#   Startup  midibn-win32.exe --report ... --auto-close-ms: raw performance-counter readings (the counter is
#            system-wide) at the launch (this script, just before CreateProcess), at entry to wWinMain, after the window
#            was created and shown, and at the first idle moment of the message loop; also the working set and
#            private bytes of the idle player. The player is started ONLY on a private hidden desktop (the script
#            starts itself through run-hidden.ps1), never plays, and never presses Play. Start-up is warm-cache.
#
# Conditions are recorded with every run: power plan, processor load before and after, processor performance counter.
# A set is labelled "quiet" only when the processor load stayed under 20%, else "under load (interleaved A/B)".
param(
    [ValidateSet('All', 'Sizes', 'Load', 'Flow', 'Capacity', 'Startup')][string]$Mode = 'All',
    [string[]]$Variants = @('x64', 'x86', 'xp', 'xp-ia32'),
    [int]$Runs = 3,             # without -Runs: 3 runs, plus one at a time up to 7 while a result is unclear (adaptive-repeats.ps1); -Runs N = exactly N
    [string]$Fixture = 'v2\harness\fixtures\large\large-8m.mid',
    [string]$Before = '',
    [string]$Output = '',
    [switch]$IncludeV1,
    [string]$V1Dir = '',        # where v1's "MIDIBottleneck Player x64.exe" and "... x86.exe" are (default: dist\ of this checkout or of the main checkout)
    [int]$AutoCloseMs = 900,
    [string]$Position = '',    # 'X,Y': start-up runs on the real desktop with the player opened there (--position; prompt 009: windows
                                # visible on Internal Display, e.g. '3880,1133'), instead of the hidden desktop. Only for builds that know --position.
    [switch]$InHidden,          # internal: set when the script re-starts itself on the hidden desktop for the start-up section
    [string]$StartupOut = ''    # internal: file the hidden run writes its tables to
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo
$self = $MyInvocation.MyCommand.Path
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim("'", ' ') } | Where-Object { $_ })   # also accepts a comma list from powershell -File

. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')
$RunsExplicit = $PSBoundParameters.ContainsKey('Runs')
$AdaptivePlan = Get-AdaptivePlan -Explicit $RunsExplicit -ExplicitCount $Runs
$runsText = if ($RunsExplicit) { "$Runs" } else { '3 to 7 (adaptive)' }
# Runs the measurement once per set per round (interleaved), 3 rounds first, then one more at a time while unclear.
# $One = { param($tool) ... } runs a set once; $Metric = { param($result) ... } gives the number judged.
# A "<x> before" / "<x> after" pair is judged as a comparison, every other set on its own spread.
function Invoke-AdaptiveRuns($sets, [scriptblock]$One, [scriptblock]$Metric, [string]$Name) {
    $results = @{}; foreach ($k in $sets.Keys) { $results[$k] = @() }
    $n = 0; $reason = ''
    while ($true) {
        $n++
        foreach ($k in $sets.Keys) { $results[$k] += , (& $One $sets[$k]) }
        if ($n -lt $AdaptivePlan.Min) { continue }
        if ($AdaptivePlan.Explicit) { break }
        $samples = @{}; foreach ($k in $sets.Keys) { $samples[$k] = @($results[$k] | ForEach-Object { [double](& $Metric $_) }) }
        $pairs = @(); foreach ($k in $sets.Keys) { if ($k -like '* before' -and $sets.Contains(($k -replace ' before$', ' after'))) { $pairs += , @($k, ($k -replace ' before$', ' after')) } }
        $verdict = Get-AdaptiveVerdict -Samples $samples -Pairs $pairs -Done $n -Max $AdaptivePlan.Max
        if (-not $verdict.Extend) { $reason = $verdict.Reason; break }
    }
    $script:LastAdaptiveNote = Format-AdaptiveNote -Used $n -Explicit $AdaptivePlan.Explicit -Reason $reason -Name $Name
    return $results
}

function Median([double[]]$values) {
    $s = @($values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    $mid = [int][math]::Floor($s.Count / 2)
    if ($s.Count % 2 -eq 1) { return $s[$mid] } else { return ($s[$mid - 1] + $s[$mid]) / 2 }
}
function Spread([double[]]$values, [string]$format = 'N0') {
    $s = @($values | Sort-Object)
    return ('{0} ({1}-{2})' -f (Median $s).ToString($format), $s[0].ToString($format), $s[-1].ToString($format))
}
function Cpu-Sample {
    $c = Get-Counter '\Processor(_Total)\% Processor Time', '\Processor Information(_Total)\% Processor Performance' -SampleInterval 1 -MaxSamples 3
    $load = @($c | ForEach-Object { $_.CounterSamples[0].CookedValue })
    $perf = @($c | ForEach-Object { $_.CounterSamples[1].CookedValue })
    return [pscustomobject]@{ Load = ($load | Measure-Object -Average).Average; Perf = ($perf | Measure-Object -Average).Average }
}
function Conditions-Before { $script:cpuStart = Cpu-Sample }
function Conditions-After([string]$what) {
    $end = Cpu-Sample
    $plan = ((powercfg /getactivescheme) -join ' ') -replace '^.*\((.*)\).*$', '$1'
    $avg = ($script:cpuStart.Load + $end.Load) / 2
    $label = if ($avg -lt 20) { 'quiet' } else { 'under load (interleaved A/B)' }
    return ('Conditions for {0}: power plan {1}; processor load {2:N0}% before and {3:N0}% after; processor performance {4:N0}% / {5:N0}%; label: {6}.' -f
        $what, $plan, $script:cpuStart.Load, $end.Load, $script:cpuStart.Perf, $end.Perf, $label)
}
function Header-Lines {
    $cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
    $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
    $os = Get-CimInstance Win32_OperatingSystem
    $commit = (& git rev-parse --short HEAD 2>$null)
    return @("# Footprint measurements, commit $commit, $(Get-Date -Format 'yyyy-MM-dd HH:mm') | $cpu | $ram GB RAM | $($os.Caption) $($os.Version)")
}
function After-Dir([string]$variant) { Join-Path $repo "v2\out\build-$variant\Release" }
function Before-Dir([string]$variant) { Join-Path $Before $variant }
function Size-Of([string]$path) { if ($path -and (Test-Path $path)) { (Get-Item $path).Length } else { $null } }
function Kib($bytes) { if ($null -eq $bytes) { return 'n/a' } else { return ('{0:N0}' -f ($bytes / 1KB)) } }

$lines = @()

# ------------------------------------------------------------------ sizes
function Section-Sizes {
    $out = @()
    $out += ''
    $out += '## Executable sizes (KiB)'
    if ($Before) {
        $out += '| Variant | midibn-win32 before | midibn-win32 after | midibn-perf before | midibn-perf after |'
        $out += '|---|---|---|---|---|'
    } else {
        $out += '| Variant | midibn-win32 | midibn-perf |'
        $out += '|---|---|---|'
    }
    foreach ($v in $Variants) {
        $aw = Size-Of (Join-Path (After-Dir $v) 'midibn-win32.exe'); $ap = Size-Of (Join-Path (After-Dir $v) 'midibn-perf.exe')
        if ($Before) {
            $bw = Size-Of (Join-Path (Before-Dir $v) 'midibn-win32.exe'); $bp = Size-Of (Join-Path (Before-Dir $v) 'midibn-perf.exe')
            $out += ('| {0} | {1} | {2} | {3} | {4} |' -f $v, (Kib $bw), (Kib $aw), (Kib $bp), (Kib $ap))
        } else {
            $out += ('| {0} | {1} | {2} |' -f $v, (Kib $aw), (Kib $ap))
        }
    }
    if ($IncludeV1) {
        $dir = $V1Dir
        if (-not $dir) {
            foreach ($c in (Join-Path $repo 'dist'), (Join-Path $repo '..\..\..\dist')) {
                if (Test-Path (Join-Path $c 'MIDIBottleneck Player x64.exe')) { $dir = (Resolve-Path $c).Path; break }
            }
        }
        $out += ''
        $out += 'v1 (C#; needs the .NET Framework 4.8, which Windows 10 includes; v2 is self-contained):'
        foreach ($a in 'x64', 'x86') {
            $p = if ($dir) { Join-Path $dir "MIDIBottleneck Player $a.exe" } else { '' }
            $out += ('- v1 {0}: {1} KiB' -f $a, (Kib (Size-Of $p)))
        }
    }
    return $out
}

# ------------------------------------------------------------------ load
function Run-Perf([string[]]$tool, [string]$midi) {
    $exe = $tool[0]
    $arguments = @($tool | Select-Object -Skip 1) + $midi
    $values = @{}
    foreach ($line in (& $exe @arguments)) { $parts = $line -split '=', 2; if ($parts.Count -eq 2) { $values[$parts[0]] = $parts[1] } }
    if ($LASTEXITCODE -ne 0) { throw "$exe failed" }
    return $values
}
function Section-Load {
    $midi = (Resolve-Path $Fixture).Path
    $out = @()
    $out += ''
    $out += "## Load of $(Split-Path -Leaf $midi) ($((Get-Item $midi).Length) bytes), fresh process per run, $runsText interleaved runs after one warm-up each"
    $out += 'Load ms is the parser call only (min-max in brackets); memory is the process peak. Medians.'
    $out += '| Build | Load ms | Peak working set MiB | Peak private MiB |'
    $out += '|---|---|---|---|'
    $notes = @()
    Conditions-Before
    foreach ($v in $Variants) {
        $sets = [ordered]@{}
        $afterExe = Join-Path (After-Dir $v) 'midibn-perf.exe'
        if (-not (Test-Path $afterExe)) { continue }
        if ($Before) {
            $beforeExe = Join-Path (Before-Dir $v) 'midibn-perf.exe'
            if (Test-Path $beforeExe) { $sets["$v before"] = @($beforeExe) }
        }
        $sets["$v after"] = @($afterExe)
        if ($IncludeV1 -and $v -in 'x64', 'x86') {
            $v1 = "v2\out\v1-reference\$v\v1-reference.exe"
            if (Test-Path $v1) { $sets["$v v1 (C#)"] = @($v1, 'perf-load') }
        }
        foreach ($k in $sets.Keys) { Run-Perf $sets[$k] $midi | Out-Null }   # warm-up
        $results = Invoke-AdaptiveRuns $sets { param($tool) Run-Perf $tool $midi } { param($x) $x['load-ms'] } "load $v"
        $notes += $script:LastAdaptiveNote
        foreach ($k in $sets.Keys) {
            $r = $results[$k]
            $t = @($r | ForEach-Object { [double]$_['load-ms'] })
            $ws = @($r | ForEach-Object { [double]$_['peak-working-set-bytes'] / 1MB })
            $pv = @($r | ForEach-Object { [double]$_['peak-private-bytes'] / 1MB })
            $out += ('| {0} | {1} | {2} | {3} |' -f $k, (Spread $t), (Spread $ws), (Spread $pv))
        }
    }
    $out += ''
    $out += $notes
    $out += (Conditions-After 'the load section')
    return $out
}

# ------------------------------------------------------------------ flow and capacity
function Section-Flow {
    $midi = (Resolve-Path $Fixture).Path
    $out = @()
    $out += ''
    $out += "## Load flow as the player runs it, $(Split-Path -Leaf $midi), $runsText runs after one warm-up (medians, min-max)"
    $out += '| Build | Whole flow ms | Inspection ms | Parse ms | Peak private MiB |'
    $out += '|---|---|---|---|---|'
    $notes = @()
    Conditions-Before
    foreach ($v in $Variants) {
        $exe = Join-Path (After-Dir $v) 'midibn-load-flow.exe'
        if (-not (Test-Path $exe)) { continue }
        Run-Perf @($exe, '--force-preflight') $midi | Out-Null
        $flowResults = Invoke-AdaptiveRuns ([ordered]@{ $v = @($exe, '--force-preflight') }) { param($tool) Run-Perf $tool $midi } { param($x) $x['flow-ms'] } "flow $v"
        $notes += $script:LastAdaptiveNote
        $r = $flowResults[$v]
        $out += ('| {0} (inspection forced) | {1} | {2} | {3} | {4} |' -f $v, (Spread @($r | ForEach-Object { [double]$_['flow-ms'] })), (Spread @($r | ForEach-Object { [double]$_['preflight-ms'] })),
            (Spread @($r | ForEach-Object { [double]$_['load-ms'] })), (Spread @($r | ForEach-Object { [double]$_['peak-private-bytes'] / 1MB })))
    }
    $out += ''
    $out += $notes
    $out += (Conditions-After 'the flow section')
    return $out
}
function Make-Big([int]$repeats, [string]$path, [string]$source) {
    $b = [IO.File]::ReadAllBytes($source)
    $n = ($b[10] -shl 8) + $b[11]
    $body = New-Object byte[] ($b.Length - 14); [Array]::Copy($b, 14, $body, 0, $body.Length)
    $hdr = $b[0..13]; $total = $n * $repeats; $hdr[10] = [byte]($total -shr 8); $hdr[11] = [byte]($total -band 255)
    $fs = [IO.File]::Create($path)
    try { $fs.Write($hdr, 0, 14); for ($i = 0; $i -lt $repeats; $i++) { $fs.Write($body, 0, $body.Length) } } finally { $fs.Close() }
}
function Section-Capacity {
    $source = (Resolve-Path $Fixture).Path
    $out = @()
    $out += ''
    $out += "## Capacity of the 32-bit builds: files made by repeating the fixture's tracks"
    $out += 'Each file is loaded in a fresh midibn-perf; "fail" is the out-of-memory message. 32-bit programs get a 2 GiB address space, 4 GiB on a 64-bit Windows when the program is large-address-aware.'
    $repeatList = 9, 10, 13, 17, 21
    $out += '| Build | ' + (($repeatList | ForEach-Object { '{0} M events' -f [math]::Round(8.00016 * $_) }) -join ' | ') + ' |'
    $out += '|---|' + ('---|' * $repeatList.Count)
    $dir = Join-Path ([IO.Path]::GetTempPath()) ('midibn-capacity-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir | Out-Null
    $cells = @{}
    foreach ($r in $repeatList) {
        $big = Join-Path $dir "big-$r.mid"
        Make-Big $r $big $source
        foreach ($v in @($Variants | Where-Object { $_ -in 'x86', 'xp', 'xp-ia32' })) {
            foreach ($side in 'before', 'after') {
                if ($side -eq 'before' -and -not $Before) { continue }
                $exe = if ($side -eq 'before') { Join-Path (Before-Dir $v) 'midibn-perf.exe' } else { Join-Path (After-Dir $v) 'midibn-perf.exe' }
                if (-not (Test-Path $exe)) { continue }
                $text = (& $exe $big 2>&1) -join "`n"
                $key = "$v $side"
                if (-not $cells.ContainsKey($key)) { $cells[$key] = @() }
                if ($text -match 'peak-private-bytes=(\d+)') { $cells[$key] += ('ok ({0:N1} GiB)' -f ([double]$matches[1] / 1GB)) } else { $cells[$key] += 'fail' }
            }
        }
        [IO.File]::Delete($big)
    }
    [IO.Directory]::Delete($dir)
    foreach ($k in ($cells.Keys | Sort-Object)) { $out += ('| {0} | {1} |' -f $k, ($cells[$k] -join ' | ')) }
    return $out
}

# ------------------------------------------------------------------ start-up (hidden desktop only)
function Start-One([string]$exe) {
    $report = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'midibn-startup-' + [Guid]::NewGuid().ToString('N') + '.txt')
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "--no-settings --report `"$report`" --auto-close-ms $AutoCloseMs"
    if ($Position) { $psi.Arguments += " --position $Position" }
    $psi.UseShellExecute = $false
    $t0 = [Diagnostics.Stopwatch]::GetTimestamp()
    $p = [Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds ([int]($AutoCloseMs * 0.6))
    $ws = 0; $pv = 0
    try { $p.Refresh(); $ws = $p.WorkingSet64; $pv = $p.PrivateMemorySize64 } catch { }
    if (-not $p.WaitForExit(30000)) { $p.Kill(); throw "$exe did not close" }
    if (-not (Test-Path $report)) { throw "$exe wrote no report" }
    $v = @{}
    foreach ($line in (Get-Content $report)) { $parts = $line -split '=', 2; if ($parts.Count -eq 2) { $v[$parts[0]] = $parts[1] } }
    [IO.File]::Delete($report)
    $f = [double]$v['qpc_frequency']
    $toEntry = ([double]$v['qpc_entry'] - $t0) * 1000.0 / $f
    $create = [double]$v['startup_create_ms']
    $idle = [double]$v['startup_idle_ms']
    return [pscustomobject]@{ ToEntry = $toEntry; Create = $create; Idle = $idle; Total = $toEntry + $idle; Ws = $ws / 1MB; Private = $pv / 1MB }
}
function Section-Startup {
    $out = @()
    $out += ''
    $where = if ($Position) { "visibly on the real desktop at $Position" } else { 'on a hidden desktop' }
    $out += "## Player start-up (midibn-win32.exe $where, warm file cache), $runsText interleaved runs after one warm-up each"
    $out += 'Launch -> entry: from just before CreateProcess to wWinMain (process creation, loader, static initialisation). Entry -> window: creating and showing the window. Entry -> idle: until the message queue first empties. Total: launch -> first idle. Memory is the idle player. Medians (min-max).'
    $out += '| Build | Launch -> entry ms | Entry -> window ms | Entry -> idle ms | Total ms | Working set MiB | Private MiB |'
    $out += '|---|---|---|---|---|---|---|'
    $notes = @()
    Conditions-Before
    foreach ($v in $Variants) {
        $sets = [ordered]@{}
        $afterExe = Join-Path (After-Dir $v) 'midibn-win32.exe'
        if (-not (Test-Path $afterExe)) { continue }
        if ($Before) {
            $beforeExe = Join-Path (Before-Dir $v) 'midibn-win32.exe'
            if (Test-Path $beforeExe) { $sets["$v before"] = $beforeExe }
        }
        $sets["$v after"] = $afterExe
        foreach ($k in $sets.Keys) { Start-One $sets[$k] | Out-Null }   # warm-up
        $results = Invoke-AdaptiveRuns $sets { param($exePath) Start-One $exePath } { param($x) $x.Total } "start-up $v"
        $notes += $script:LastAdaptiveNote
        foreach ($k in $sets.Keys) {
            $r = $results[$k]
            $out += ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $k,
                (Spread @($r | ForEach-Object { $_.ToEntry }) 'N1'), (Spread @($r | ForEach-Object { $_.Create }) 'N1'), (Spread @($r | ForEach-Object { $_.Idle }) 'N1'),
                (Spread @($r | ForEach-Object { $_.Total }) 'N1'), (Spread @($r | ForEach-Object { $_.Ws }) 'N1'), (Spread @($r | ForEach-Object { $_.Private }) 'N1'))
        }
    }
    $out += ''
    $out += $notes
    $out += (Conditions-After 'the start-up section')
    return $out
}

# ------------------------------------------------------------------ main
if ($InHidden -or ($Position -and $Mode -eq 'Startup')) {
    $table = Section-Startup
    if ($StartupOut) { $table | Set-Content -Encoding utf8 $StartupOut }
    if ($Output) { $table | Set-Content -Encoding utf8 $Output }
    $table | ForEach-Object { Write-Host $_ }
    exit 0
}

$lines += Header-Lines
if ($Mode -in 'All', 'Sizes') { $lines += Section-Sizes }
if ($Mode -in 'All', 'Load') { $lines += Section-Load }
if ($Mode -in 'All', 'Flow') { $lines += Section-Flow }
if ($Mode -eq 'Capacity') { $lines += Section-Capacity }
if ($Mode -in 'All', 'Startup') {
    # The player may only be started on the hidden desktop.
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('midibn-footprint-startup-' + [Guid]::NewGuid().ToString('N') + '.md')
    $argText = "-InHidden -Mode Startup $(if ($RunsExplicit) { "-Runs $Runs " })-AutoCloseMs $AutoCloseMs -StartupOut '$tmp' -Variants " + (($Variants | ForEach-Object { "'$_'" }) -join ',')
    if ($Before) { $argText += " -Before '$Before'" }
    & (Join-Path $repo 'v2\tools\visual\run-hidden.ps1') -File $self -Arguments $argText -WorkingDirectory $repo -TimeoutMinutes 30 *>$null
    if (Test-Path $tmp) { $lines += (Get-Content $tmp); [IO.File]::Delete($tmp) } else { $lines += ''; $lines += '## Player start-up: the hidden run produced no table' }
}
$lines | ForEach-Object { Write-Host $_ }
if ($Output) { $lines | Set-Content -Encoding utf8 $Output }
