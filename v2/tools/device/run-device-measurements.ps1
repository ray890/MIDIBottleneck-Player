# Real-device MIDI output measurements, v1 against v2 (user prompt 007 Step 2; reworked for prompt 008, Step 4).
#
# This script PLAYS THE MIDI DATA AS IT IS, AT ITS OWN VOLUME. It changes NO audio setting: no mute, no unmute,
# no volume change, no device switch, no power-plan change. (The earlier mute -> run -> restore design is gone.)
# The only audio call it makes is the read-only `audio-mute.ps1 -Action Status`, which lists the playback
# endpoints and their current mute/volume state into the log (so a result can be read together with the state
# the PC was in). Nothing is written to any audio device setting.
#
# What sounds: the generated fixtures use controllers 20..31 (undefined by the MIDI standard), so a synthesizer
# makes almost no sound for them; the user's own files (-UserFolder) are music and WILL sound on the real paths.
# Use -DryRun first: it runs the whole script on the null path only (no device is opened, no sound).
#
# Paths measured (all end in the installed OmniMIDI, except null):
#   winmm-system  OmniMIDI through the system WinMM (midiOut* on the "OmniMIDI" device)
#   kdmapi        OmniMIDI through KDMAPI (loads OmniMIDI.dll from System32)
#   winmm-wrp     OmniMIDI through WinMM with the third-party winmmwrp winmm.dll. The DLL is copied ONLY into
#                 a git-ignored private folder (v2\out\device-test\<arch>-winmmwrp\<program>\) next to private
#                 copies of the test executables, and deleted again when the run ends. Never committed,
#                 packaged or backed up. x64 only (the DLL is x64).
#   winmm-direct  Direct WinMM driver access (DD-085): the same device through the installed driver's own modMessage entry
#                 point instead of midiOutShortMsg (v2 only; v1 has no such path). Also runs the stress cycles (open, flood,
#                 Stop / close / panic during the flood, $StressCycles times each) and lists which WinMM devices it can drive.
#                 Never put a winmm.dll beside the executable for this path (it is not used with winmm-wrp's private folders).
#   null          no device (engine-only ceiling; the engine runs with an output that does nothing)
#
# Programs: v1 (the v1.0.44 engine in a C# driver), v2 (midibn-device-measure). Any v2-<name> kept as
# v2\out\device-test\v2-<name>\midibn-device-measure.exe can be named too.
#
# What is measured, per path and program:
#   - the density band 9 to 10 million events per second (uniform and clustered files at 9, 9.5 and 10 million per
#     second): throughput actually delivered and timing fidelity (lateness percentiles, stalls) at that density;
#   - a 1 million per second file, an over-capacity file (path capacity), loads relative to the path's capacity,
#     raw send ceilings and instrumentation overhead (as before);
#   - with -UserFolder: the user's own MIDI files, read-only (see below).
#
# Measurement conditions are recorded for every run (and once in env.txt): active power plan (powercfg
# /getactivescheme), whole-PC CPU load, `% Processor Performance` (clock relative to base), the busiest other
# processes, before the run and again right after it. They appear in runs.csv / raw-ceilings.csv as cond_* columns
# and in summary.md. The quiet-CPU wait is kept: before every test process the script waits (up to
# -MaxQuietWaitSeconds) until the whole PC is at or below -QuietCpuPercent (default 20 %, the project rule); a run
# that had to start anyway is marked cond_quiet=no (with -StrictQuiet the script stops instead).
#
# -UserFolder <dir>: runs the user's own MIDI files (.mid .midi .rmi, not recursive unless -UserRecurse; at most
# -UserFileLimit files, default 6, 0 = all) through every path and both programs. The files are only READ: never
# copied, never written next to, never renamed (their size and modified time are compared before and after).
# They are called "user file 1..n" everywhere in logs, runs.csv, summary.md and raw text files; the mapping from
# number to real file name exists only in v2\out\device-test\user-file-map-<stamp>.txt (git-ignored, private).
# Files are played whole, -UserRepeats times each (default 1): choose short pieces, or use -UserFileLimit.
# -UserOnly runs only those files (no generated fixtures, no probes, raw ceilings or overhead runs).
#
# -DryRun: all of the above on the null path only, short fixtures, one repeat; refuses to open any device.
#
# Usage (from the repository root, Windows PowerShell):
#   .\v2\tools\device\run-device-measurements.ps1 -DryRun             # no sound, a few minutes
#   .\v2\tools\device\run-device-measurements.ps1                     # the full set, v1 and v2, x64, all four paths
#   .\v2\tools\device\run-device-measurements.ps1 -Paths kdmapi,null  # fewer paths
#   .\v2\tools\device\run-device-measurements.ps1 -UserFolder D:\my\midi -UserOnly
#   .\v2\tools\device\run-device-measurements.ps1 -Quick              # smoke test: short fixtures, one repeat
#   .\v2\tools\device\run-device-measurements.ps1 -StressOnly -DeviceName OmniMIDI   # only the direct WinMM stress (DD-094);
#                                                     -StressModes stop,close,panic picks modes; add -ShowPlan to only print it;
#                                                     -StressCloseStyles reset,notesoff,settle [-StressSettleMs 250] compares
#                                                     how the device is reset before MODM_CLOSE (one run per style and mode)
#   -Build builds the v2 tool (x64 Release) and the v1 driver first.
# Results: v2\out\device-test\results\<stamp>\ (runs.csv, raw-ceilings.csv, summary.md, env.txt, audio-status.txt, run-log.txt).
param(
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    [string[]]$Programs = @('v1', 'v2'),
    [string[]]$Paths = @('winmm-system', 'kdmapi', 'winmm-wrp', 'winmm-direct', 'null'),
    [string[]]$Fixtures = @('uniform-9M', 'uniform-9.5M', 'uniform-10M', 'clustered-9M', 'clustered-9.5M', 'clustered-10M', 'clustered-1M', 'overcap'),
    [string[]]$OverheadPaths = @('null', 'kdmapi'),
    [string[]]$RelativeLoads = @("0.5", "0.8", "0.95", "1.15"),
    [int]$Repeats = 3,
    [switch]$Quick,
    [switch]$DryRun,
    [switch]$SkipRaw,
    [switch]$SkipOverhead,
    [switch]$Build,
    [switch]$OverheadOnly,
    [string]$UserFolder = '',
    [switch]$UserRecurse,
    [int]$UserFileLimit = 6,
    [int]$UserRepeats = 1,
    [switch]$UserOnly,
    [string]$Aggregate = '',
    [string]$V2Exe,
    [string]$V1Exe,
    [string]$WinmmWrp = $env:MIDIBN_WINMMWRP_DLL,   # path of the winmmwrp winmm.dll; needed only for winmm-wrp (or set MIDIBN_WINMMWRP_DLL)
    [string]$Label = '',
    [string]$DeviceName = 'OmniMIDI',      # the WinMM device the real paths play (e.g. 'VirtualMIDISynth #1'; KDMAPI ignores it)
    [int]$StressCycles = 200,               # winmm-direct only: open/flood/stop (and close, panic) repetitions
    [switch]$SkipStress,
    [switch]$StressOnly,                    # DD-094: only the direct WinMM stress (v2, path winmm-direct): device list + stress modes
    [string[]]$StressModes = @('stop', 'close', 'panic'),
    [string[]]$StressCloseStyles = @('reset'),   # DD-094 A/B: reset (MODM_RESET, the player's way), notesoff, settle
    [switch]$StressKeepOpen,                      # DD-111: the stress cycles share one output that stays open across Stop (as the player)
    [int]$StressSettleMs = 250,                 # the wait before MODM_RESET of close style 'settle'
    [string]$ResultsRoot,
    [int]$ProcessTimeoutSeconds = 900,
    [double]$QuietCpuPercent = 20,
    [int]$MaxQuietWaitSeconds = 90,
    [switch]$StrictQuiet,
    # ---- the user's own files, passage by passage (final v1-vs-v2 comparison); see the user-passages section below
    [Alias('UserFiles')][string[]]$UserFilePaths = @(),
    [string]$UserPassages = '',
    [string[]]$UserPaths = @('winmm-wrp', 'kdmapi', 'winmm-system', 'winmm-direct'),
    [string]$UserModes = '1=direct,earlier;2=direct,earlier;3=earlier',
    [double]$EarlierBy = 10,
    [string[]]$PassageInstr = @('count', 'sample'),
    [int]$Chase = 1,
    [switch]$FromStart,
    [int]$FromStartFile = 2,
    [string]$FromStartPath = 'winmm-wrp',
    [string[]]$CapPaths = @('winmm-system'),
    [double]$CapExtraSeconds = 10,
    [double]$DefaultCapExtraSeconds = 120,
    [int]$X86MaxFileMB = 300,
    [int]$PathSettleMs = 3000,
    [int]$UserProcessTimeoutSeconds = 14400,
    [switch]$NoMemoryCheck,
    [switch]$LoadOnly,
    [switch]$ShowPlan,
    [switch]$AllowSound
)
$ErrorActionPreference = 'Stop'
# `powershell -File` passes "a,b" as one string; accept both forms.
$Programs = @($Programs | ForEach-Object { $_ -split ',' })
$Paths = @($Paths | ForEach-Object { $_ -split ',' })
$Fixtures = @($Fixtures | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$OverheadPaths = @($OverheadPaths | ForEach-Object { $_ -split ',' })
$StressModes = @($StressModes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($sm in $StressModes) { if ($sm -notin 'stop', 'close', 'panic') { throw "unknown stress mode '$sm' (stop, close, panic)" } }
$StressCloseStyles = @($StressCloseStyles | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($cs in $StressCloseStyles) { if ($cs -notin 'reset', 'notesoff', 'settle') { throw "unknown stress close style '$cs' (reset, notesoff, settle)" } }
# One stress run per close style and mode; the default style keeps the earlier test names (direct-stress-<mode>).
function Get-StressRuns {
    foreach ($cs in $StressCloseStyles) {
        foreach ($sm in $StressModes) {
            $a = @('--mode', $sm, '--close-style', $cs)
            if ($cs -eq 'settle') { $a += @('--settle-ms', [string]$StressSettleMs) }
            $suffix = ''
            if ($StressKeepOpen) { $a += @('--keep-open', '1'); $suffix = '-keepopen' }   # DD-111: one output kept open across the cycles
            [pscustomobject]@{ Name = $(if ($cs -eq 'reset') { "direct-stress-$sm$suffix" } else { "direct-stress-$sm-$cs$suffix" }); Args = $a }
        }
    }
}
$loads = New-Object System.Collections.ArrayList
foreach ($x in $RelativeLoads) { foreach ($part in ("$x" -split ",")) { [void]$loads.Add([double]::Parse($part, [Globalization.CultureInfo]::InvariantCulture)) } }
if ($StressOnly) {
    # -StressOnly: nothing but the direct WinMM device list and the stress cycles (no probe, fixtures, loads, raw or overhead runs).
    if ($DryRun -or $SkipStress -or $UserFolder -or $UserFilePaths.Count -gt 0) { throw '-StressOnly cannot be combined with -DryRun, -SkipStress or user files.' }
    $Programs = @('v2'); $Paths = @('winmm-direct'); $Fixtures = @(); $SkipRaw = [switch]$true; $SkipOverhead = [switch]$true; $loads.Clear()
}
$root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..\..')).Path
$deviceDir = Join-Path $root 'v2\out\device-test'
New-Item -ItemType Directory -Force -Path $deviceDir | Out-Null

if ($DryRun) {
    # Nothing but the null path: no device is opened and no sound can be made.
    $Paths = @('null'); $OverheadPaths = @('null'); $Quick = [switch]$true
    if (-not $Label) { $Label = 'dryrun' }
    # A dry run is not a measurement: do not wait long for a quiet PC (the wait logic itself still runs).
    if (-not $PSBoundParameters.ContainsKey('MaxQuietWaitSeconds')) { $MaxQuietWaitSeconds = 3 }
}
if ($UserOnly) {
    if (-not $UserFolder) { throw '-UserOnly needs -UserFolder.' }
    $Fixtures = @(); $SkipRaw = [switch]$true; $SkipOverhead = [switch]$true; $loads.Clear()
}
# -UserFiles: the passage comparison on the user's own files replaces the generated-fixture tests.
$UserFilePaths = @($UserFilePaths | ForEach-Object { if (Test-Path -LiteralPath $_) { $_ } else { $_ -split ',' } } | Where-Object { $_ })
$UserFilesMode = ($UserFilePaths.Count -gt 0)
if ($UserFilesMode) {
    $Paths = @(); $Fixtures = @(); $SkipRaw = [switch]$true; $SkipOverhead = [switch]$true; $loads.Clear()
    if (-not $DryRun -and -not $LoadOnly -and -not $ShowPlan -and -not $AllowSound) {
        throw 'The user-file passages play the music on real outputs and make sound. Add -AllowSound (main session, after announcing), or use -DryRun (null output) / -LoadOnly / -ShowPlan.'
    }
    if (-not $LoadOnly -and -not $ShowPlan -and -not $UserPassages) { throw '-UserFiles needs -UserPassages, e.g. ''1=123-126;2=133-138,195-210'' (or use -LoadOnly).' }
}
function Assert-Silent([string]$output) {
    if ($DryRun -and $output -ne 'null') { throw "dry run refuses to use output '$output'" }
}

# ---------------------------------------------------------------- locate programs

$cmake = $null
if ($Build) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -property installationPath
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $bdir = Join-Path $root "v2\out\build-$Arch"
    $platform = if ($Arch -eq 'x64') { 'x64' } else { 'Win32' }
    & $cmake -S (Join-Path $root 'v2') -B $bdir -G 'Visual Studio 17 2022' -A $platform -T v143 | Out-Null
    & $cmake --build $bdir --config Release --target midibn-device-measure --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'v2 build failed' }
    & (Join-Path $root 'v2\harness\v1-reference\device\build-device.ps1')
}
if (-not $V2Exe) { $V2Exe = Join-Path $root "v2\out\build-$Arch\Release\midibn-device-measure.exe" }
if (-not $V1Exe) { $V1Exe = Join-Path $root "v2\out\device-test\v1-$Arch\v1-device.exe" }
$analyzer = (Resolve-Path (Join-Path $root 'v2\out\build-x64\Release\midibn-device-measure.exe') -ErrorAction SilentlyContinue)
if (-not $analyzer) { $analyzer = $V2Exe }
# Program names: v1; v2 (the current build); or any v2-<name> kept as v2\out\device-test\v2-<name>\midibn-device-measure.exe.
$ProgramExe = @{}
foreach ($p in $Programs) {
    $exe = if ($p -eq 'v1') { $V1Exe } elseif ($p -eq 'v2') { $V2Exe } else { Join-Path $root "v2\out\device-test\$p\midibn-device-measure.exe" }
    if (-not (Test-Path -LiteralPath $exe)) { throw "$p program not found: $exe (use -Build)" }
    $ProgramExe[$p] = $exe
}
if ($StressOnly -and $ShowPlan) {
    # Prints what -StressOnly would run and stops: no results folder, no fixture, no device, no audio query.
    $planCycles = if ($Quick) { [math]::Min($StressCycles, 20) } else { $StressCycles }
    Write-Host "plan (-StressOnly -ShowPlan): program $($ProgramExe['v2'])"
    Write-Host "plan: devices"
    foreach ($run in (Get-StressRuns)) { Write-Host "plan: $($run.Name): direct-stress --fixture <clustered-9M> --device-name $DeviceName --cycles $planCycles --flood-ms 100 $($run.Args -join ' ') (timeout 1800 s)" }
    Write-Host 'plan only (-ShowPlan): nothing was run'
    return
}
$monitorDir = Join-Path $root 'v2\out\monitor'
New-Item -ItemType Directory -Force -Path $monitorDir | Out-Null
$monitor = Join-Path $monitorDir 'NativeWindowMonitor.exe'
$monitorSource = Join-Path $root 'archive\v1\tools\NativeWindowMonitor.cs'
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $monitor) -or (Get-Item $monitorSource).LastWriteTime -gt (Get-Item $monitor).LastWriteTime) {
    & $csc /nologo /target:exe /platform:anycpu /optimize+ /warn:4 "/out:$monitor" $monitorSource
    if ($LASTEXITCODE -ne 0) { throw 'Native-window monitor compilation failed.' }
}

# ---------------------------------------------------------------- results folder, log

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if (-not $ResultsRoot) { $ResultsRoot = Join-Path $deviceDir 'results' }
$resultDir = Join-Path $ResultsRoot ($stamp + $(if ($Label) { "-$Label" } else { '' }))
if ($Aggregate) { $resultDir = (Resolve-Path -LiteralPath $Aggregate).Path; $Paths = @() }
$rawDir = Join-Path $resultDir 'raw'
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null
$logPath = Join-Path $resultDir 'run-log.txt'
function Log([string]$text) {
    $line = '{0} {1}' -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $text
    Write-Host $line
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
}
$inv = [Globalization.CultureInfo]::InvariantCulture

# ---------------------------------------------------------------- measurement conditions (power plan, CPU load, clock)

function Get-PowerPlan {
    try {
        $t = (& powercfg.exe /getactivescheme 2>$null) -join ' '
        if ($t -match 'GUID:\s*([0-9a-fA-F-]+)\s*\((.+)\)') { return ('{0} ({1})' -f $Matches[2], $Matches[1]) }
        return $t.Trim()
    } catch { return 'unknown' }
}
$powerPlan = Get-PowerPlan

# One short look at the whole PC: total CPU %, processor performance % (clock relative to base), busiest other processes.
function Sample-Machine([int]$seconds = 2, [bool]$withTop = $true) {
    $r = [ordered]@{ busy = -1.0; perf = -1.0; top = '' }
    # Whole-PC figures first (a stable counter pair; retried), the per-process list separately (its instances come and go).
    for ($try = 0; $try -lt 3 -and $r['busy'] -lt 0; $try++) {
        try {
            $cs = (Get-Counter -Counter @('\Processor(_Total)\% Processor Time', '\Processor Information(_Total)\% Processor Performance') `
                    -SampleInterval 1 -MaxSamples $seconds -ErrorAction Stop).CounterSamples
            $busy = $cs | Where-Object { $_.Path -like '*\processor(_total)\% processor time' }
            if ($busy) { $r['busy'] = [math]::Round(($busy | Measure-Object -Property CookedValue -Average).Average, 1) }
            $perf = $cs | Where-Object { $_.Path -like '*processor information(_total)\% processor performance' }
            if ($perf) { $r['perf'] = [math]::Round(($perf | Measure-Object -Property CookedValue -Average).Average, 0) }
        } catch { Start-Sleep -Milliseconds 300 }
    }
    if (-not $withTop) { return $r }
    try {
        $cs = (Get-Counter -Counter '\Process(*)\% Processor Time' -SampleInterval 1 -MaxSamples 1 -ErrorAction Stop).CounterSamples
        $byName = @{}
        foreach ($s in $cs) {
            $n = $s.InstanceName
            if ($n -match '^(_total|idle|powershell|v1-device|midibn-device-measure|nativewindowmonitor)(#\d+)?$') { continue }
            if (-not $byName.ContainsKey($n)) { $byName[$n] = @() }
            $byName[$n] += $s.CookedValue
        }
        $rows = foreach ($k in $byName.Keys) { [pscustomobject]@{ n = $k; v = ($byName[$k] | Measure-Object -Average).Average } }
        $r['top'] = (($rows | Sort-Object v -Descending | Select-Object -First 3 | Where-Object { $_.v -ge 1 } | ForEach-Object { '{0}:{1:N0}%' -f $_.n, $_.v }) -join ' ')
    } catch { }
    return $r
}

# ---------------------------------------------------------------- environment record

function Get-OmniConfig {
    $cfg = Get-ItemProperty -Path 'HKCU:\Software\OmniMIDI\Configuration' -ErrorAction SilentlyContinue
    return $cfg
}
$envLines = @()
$envLines += "stamp=$stamp arch=$Arch programs=$($Programs -join ',') paths=$($Paths -join ',') dryrun=$([bool]$DryRun)"
$os = Get-CimInstance Win32_OperatingSystem
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$envLines += "os=$($os.Caption) $($os.Version) cpu=$($cpu.Name) logical=$($cpu.NumberOfLogicalProcessors) cores=$($cpu.NumberOfCores) base_clock_mhz=$($cpu.MaxClockSpeed) current_clock_mhz=$($cpu.CurrentClockSpeed)"
$envLines += "power_plan=$powerPlan"
$m0 = Sample-Machine 2
$envLines += "start_conditions cpu_busy_pct=$($m0.busy) processor_performance_pct=$($m0.perf) busiest_other=[$($m0.top)]"
$envLines += "quiet_rule=run only when whole-PC CPU <= $QuietCpuPercent % (waits up to $MaxQuietWaitSeconds s per test; strict=$([bool]$StrictQuiet))"
foreach ($f in @('C:\Windows\System32\OmniMIDI.dll', 'C:\Windows\SysWOW64\OmniMIDI.dll')) {
    if (Test-Path $f) { $v = (Get-Item $f).VersionInfo; $envLines += "omnimidi file=$f version=$($v.FileVersion) size=$((Get-Item $f).Length)" }
}
$cfg = Get-OmniConfig
$keys = 'CurrentEngine', 'WASAPIOutput', 'WASAPIExclusive', 'FollowDefaultAudioDevice', 'KDMAPIEnabled', 'FeedbackEnabled', 'FeedbackDevice',
    'EvBufferSize', 'EvBufferMultRatio', 'HyperPlayback', 'DriverPriority', 'ASIOOutput', 'BufferLength', 'MaxVoices', 'IgnoreAllEvents', 'StockWinMM'
foreach ($k in $keys) { if ($cfg -and ($cfg.PSObject.Properties.Name -contains $k)) { $envLines += "omnimidi-config $k=$($cfg.$k)" } }
$omniEngine = if ($cfg -and ($cfg.PSObject.Properties.Name -contains 'CurrentEngine')) { [int]$cfg.CurrentEngine } else { -1 }
# OmniMIDI engine numbers (OmniMIDI.h): 0 audio-to-WAV, 1 BASS output (DirectSound / default device), 2 ASIO, 3 WASAPI, 4 XAudio2.
$engineNames = @{ 0 = 'AUDIO-TO-WAV (no sound device)'; 1 = 'BASS output (default device)'; 2 = 'ASIO'; 3 = 'WASAPI (shared, default device unless WASAPIOutput names one)'; 4 = 'XAudio2 (default device)' }
$envLines += "omnimidi-engine=$omniEngine $(if ($engineNames.ContainsKey($omniEngine)) { $engineNames[$omniEngine] })"

# Read-only look at the playback endpoints (mute flag and volume as they are). Nothing is written.
$audioStatusFile = Join-Path $resultDir 'audio-status.txt'
if (-not $Aggregate) {
    try {
        $st = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'v2\tools\device\audio-mute.ps1') -Action Status 2>&1
        ($st | ForEach-Object { "$_" }) | Set-Content -LiteralPath $audioStatusFile -Encoding UTF8
        foreach ($l in ($st | ForEach-Object { "$_" })) { $envLines += "audio-status (read only) $l" }
    } catch { $envLines += "audio-status unavailable: $($_.Exception.Message)" }
}

$wrpOk = $false
$wrpDir = Join-Path $deviceDir "$Arch-winmmwrp"
if (($Paths -contains 'winmm-wrp') -or ($UserFilesMode -and -not $DryRun -and -not $LoadOnly -and (@($UserPaths | ForEach-Object { $_ -split ',' }) -contains 'winmm-wrp'))) {
    if (-not $WinmmWrp) { Log "winmm-wrp skipped: no winmmwrp DLL given: pass -WinmmWrp <path to the winmmwrp winmm.dll> or set the environment variable MIDIBN_WINMMWRP_DLL" }
    elseif (-not (Test-Path -LiteralPath $WinmmWrp)) { Log "winmm-wrp skipped: $WinmmWrp not found" }
    else {
        $b = [IO.File]::ReadAllBytes($WinmmWrp)
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        $machine = [BitConverter]::ToUInt16($b, $pe + 4)
        $expected = if ($Arch -eq 'x64') { 0x8664 } else { 0x014C }
        $envLines += ('winmmwrp size={0} sha256={1} pe-machine=0x{2:X4}' -f $b.Length, (Get-FileHash -LiteralPath $WinmmWrp -Algorithm SHA256).Hash, $machine)
        if ($machine -ne $expected) { Log ("winmm-wrp skipped: the DLL is PE machine 0x{0:X4}, not {1}" -f $machine, $Arch) }
        else { $wrpOk = $true }
    }
}

# ---------------------------------------------------------------- the user's own files (read only)

$userFiles = @()      # @{ Index; Path; Length; Utc }
$userMapPath = ''
if ($UserFilesMode) {
    # The files are given explicitly: "user file N" = position in -UserFiles. Read only, like -UserFolder.
    $userMapPath = Join-Path $deviceDir "user-file-map-$stamp.txt"
    $mapLines = @("# PRIVATE - maps 'user file N' to the real file. Never copy this into a summary or a report.")
    for ($i = 0; $i -lt $UserFilePaths.Count; $i++) {
        if (-not (Test-Path -LiteralPath $UserFilePaths[$i] -PathType Leaf)) { throw "user file $($i + 1) not found" }
        $it = Get-Item -LiteralPath $UserFilePaths[$i]
        $userFiles += @{ Index = $i + 1; Path = $it.FullName; Length = $it.Length; Utc = $it.LastWriteTimeUtc }
        $mapLines += ("user file {0}`t{1}`t{2} bytes`t{3:s}Z" -f ($i + 1), $it.FullName, $it.Length, $it.LastWriteTimeUtc)
    }
    Set-Content -LiteralPath $userMapPath -Value $mapLines -Encoding UTF8
    $envLines += "user files: $($userFiles.Count) (names are only in the private map file; the files are read, never copied or written)"
}
elseif ($UserFolder) {
    if (-not (Test-Path -LiteralPath $UserFolder -PathType Container)) { throw "-UserFolder not found: $UserFolder" }
    $found = @(Get-ChildItem -LiteralPath $UserFolder -File -Recurse:$UserRecurse | Where-Object { $_.Extension -in '.mid', '.midi', '.rmi' } | Sort-Object FullName)
    if ($UserFileLimit -gt 0 -and $found.Count -gt $UserFileLimit) { $found = @($found | Select-Object -First $UserFileLimit) }
    if ($found.Count -eq 0) { throw "no .mid/.midi/.rmi file in $UserFolder" }
    $userMapPath = Join-Path $deviceDir "user-file-map-$stamp.txt"
    $mapLines = @("# PRIVATE - maps 'user file N' to the real file. Never copy this into a summary or a report.")
    for ($i = 0; $i -lt $found.Count; $i++) {
        $userFiles += @{ Index = $i + 1; Path = $found[$i].FullName; Length = $found[$i].Length; Utc = $found[$i].LastWriteTimeUtc }
        $mapLines += ("user file {0}`t{1}`t{2} bytes`t{3:s}Z" -f ($i + 1), $found[$i].FullName, $found[$i].Length, $found[$i].LastWriteTimeUtc)
    }
    Set-Content -LiteralPath $userMapPath -Value $mapLines -Encoding UTF8
    $envLines += "user files: $($userFiles.Count) (names are only in the private map file; the files are read, never copied or written)"
}
if (-not $Aggregate) { $envLines | Set-Content -LiteralPath (Join-Path $resultDir 'env.txt') -Encoding UTF8 }
Log "environment recorded; power plan: $powerPlan; CPU busy $($m0.busy) %; processor performance $($m0.perf) %; OmniMIDI engine $omniEngine"
if ($DryRun) { Log 'DRY RUN: null path only, no device is opened, no sound.' }
elseif (-not $Aggregate) { Log 'REAL RUN: the MIDI data is played as it is, at its volume, through the real paths. No audio setting is changed.' }

# ---------------------------------------------------------------- fixtures (generated on demand, git-ignored)

$fixtureDir = Join-Path $deviceDir 'fixtures'
New-Item -ItemType Directory -Force -Path $fixtureDir | Out-Null
$fixtureDefs = @{ 'overcap' = @{ Kind = 'clustered'; Rate = 25000000; Seconds = 1 } }
foreach ($kind in 'uniform', 'clustered') {
    foreach ($m in @(@('9M', 9000000), @('9.5M', 9500000), @('10M', 10000000), @('5M', 5000000), @('1M', 1000000))) {
        $fixtureDefs[('{0}-{1}' -f $kind, $m[0])] = @{ Kind = $kind; Rate = $m[1]; Seconds = 3 }
    }
}
if ($Quick) { $Repeats = 1 }
. (Join-Path $PSScriptRoot '..\adaptive-repeats.ps1')
# 3 plays per measurement; one more at a time (to 7) while the Meps of the plays spread over 10 % of their median. An explicit -Repeats N is exact.
$script:AdaptiveRepeats = (-not $PSBoundParameters.ContainsKey('Repeats')) -and (-not $Quick)
$pathR = @{}   # raw send ceiling per path, Meps (probe)
$pathC = @{}   # engine capacity per path, Meps (median overcap throughput, lower of the programs)
# A slow path (system WinMM delivers only a few tens of thousands of events per second) would take hours to play a
# 28-million-event file, so every generated fixture is cut to what the path can play in about eight seconds
# (events <= 8 x measured capacity); the density (events per second) stays as defined. 'overcap' is sized
# from the raw probe so that it is always about three times what the path can deliver.
function Get-FixtureDef([string]$name, [string]$pathName) {
    $d = $fixtureDefs[$name]
    if (-not $d) { throw "unknown fixture $name" }
    $def = @{ Kind = $d.Kind; Rate = [long]$d.Rate; Seconds = [double]$d.Seconds }
    if ($name -eq 'overcap') {
        $r = $pathR[$pathName]
        if ($r -and $r -lt 8) { $def.Seconds = 0.25; $def.Rate = [long](12 * $r * 1000000) }
    }
    elseif ($pathC.ContainsKey($pathName)) {
        $sec = [math]::Max(0.05, [math]::Min($def.Seconds, 8 * $pathC[$pathName] * 1000000 / $def.Rate))
        $def.Seconds = [math]::Round($sec, 2)
    }
    if ($Quick) { $def.Seconds = [math]::Min($def.Seconds, 0.5) }
    return $def
}
function Get-Fixture([string]$name, [string]$pathName) {
    if ($name -like 'user-file-*') {
        $n = [int]($name.Substring('user-file-'.Length))
        return ($userFiles | Where-Object { $_.Index -eq $n } | Select-Object -First 1).Path
    }
    $d = Get-FixtureDef $name $pathName
    $secText = $d.Seconds.ToString('0.##', $inv)
    $file = Join-Path $fixtureDir ('{0}-{1}-{2}s.mid' -f $name, $d.Rate, $secText.Replace('.', 'p'))
    if (-not (Test-Path -LiteralPath $file)) {
        Log "generating fixture $file"
        & $analyzer gen $file $d.Kind ([string]$d.Rate) $secText | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "fixture generation failed: $name" }
    }
    return $file
}

# ---------------------------------------------------------------- running one test process under the monitor

function Stop-Leftovers {
    foreach ($n in 'v1-device', 'midibn-device-measure') {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$deviceDir*" -or $_.Path -like "$root*" } | ForEach-Object {
            Log "killing leftover process $($_.Id) $($_.Name)"
            $_.Kill()
        }
    }
}

$monitorFailures = @()
$nonQuiet = @()

# Other agents compile on this PC. Before each test process wait (up to MaxQuietWaitSeconds) for the whole
# machine to be at or below QuietCpuPercent busy. After that: start anyway and mark the run cond_quiet=no
# (or stop, with -StrictQuiet). The conditions of every run are recorded.
function Wait-Quiet {
    $deadline = (Get-Date).AddSeconds($MaxQuietWaitSeconds)
    while ($true) {
        $s = Sample-Machine 2
        if ($s.busy -ge 0 -and $s.busy -le $QuietCpuPercent) { return @{ s = $s; quiet = 'yes' } }
        if ($s.busy -lt 0) { return @{ s = $s; quiet = 'unknown' } }
        if ((Get-Date) -gt $deadline) {
            if ($StrictQuiet) { throw "the PC stayed busy ($($s.busy) % > $QuietCpuPercent %) for $MaxQuietWaitSeconds s; stopping (-StrictQuiet). Busiest: $($s.top)" }
            return @{ s = $s; quiet = 'no' }
        }
        Start-Sleep -Seconds 2
    }
}

function Invoke-Test([string]$name, [string]$exe, [string[]]$arguments, [int]$timeoutSeconds) {
    $w = Wait-Quiet
    $before = $w.s
    Log ("START $name (CPU busy before start: {0} %, processor performance {1} %, quiet={2}, busiest: {3})" -f $before.busy, $before.perf, $w.quiet, $before.top)
    if ($w.quiet -eq 'no') { $script:nonQuiet += $name }
    $mlog = Join-Path $rawDir "$name.monitor.jsonl"
    $outFile = Join-Path $rawDir "$name.stdout.txt"
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # the monitor writes warnings to stderr; they are data, not failures
    $out = & $monitor --output $mlog --post-exit-ms 500 --poll-ms 2000 --timeout-ms ($timeoutSeconds * 1000) -- $exe @arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    $after = Sample-Machine 1 $false
    ($out | ForEach-Object { "$_" }) | Set-Content -LiteralPath $outFile -Encoding UTF8
    $cond = @(
        "cond_time=$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))",
        "cond_power_plan=$powerPlan",
        "cond_cpu_busy_before_pct=$($before.busy)", "cond_proc_perf_before_pct=$($before.perf)", "cond_busiest_before=$($before.top)",
        "cond_cpu_busy_after_pct=$($after.busy)", "cond_proc_perf_after_pct=$($after.perf)",
        "cond_quiet=$($w.quiet)")
    Set-Content -LiteralPath (Join-Path $rawDir "$name.conditions.txt") -Value $cond -Encoding UTF8
    if ($code -ne 0) {
        $script:monitorFailures += "$name exit=$code"
        Log "PROBLEM $name monitor/child exit code $code (see $mlog)"
        if ($code -eq 4) { Stop-Leftovers }
    }
    Log ("END   $name exit=$code (CPU busy after: {0} %, processor performance {1} %)" -f $after.busy, $after.perf)
    return [pscustomobject]@{ Code = $code; Lines = @($out | ForEach-Object { "$_" }) }
}

function Read-KeyValues([string]$path) {
    $d = [ordered]@{}
    if (Test-Path -LiteralPath $path) {
        foreach ($line in Get-Content -LiteralPath $path) {
            $i = $line.IndexOf('=')
            if ($i -gt 0) { $d[$line.Substring(0, $i)] = $line.Substring($i + 1) }
        }
    }
    return $d
}

# Replaces the real path and name of a user file by "user file N" in the text files a run produced.
function Hide-UserName([string]$label, $uf) {
    $full = $uf.Path
    $leaf = Split-Path -Leaf $full
    $folder = Split-Path -Parent $full
    $hidden = "user file $($uf.Index)"
    foreach ($f in (Get-ChildItem -LiteralPath $rawDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "$label*" -and $_.Extension -in '.txt', '.jsonl' })) {
        $t = [IO.File]::ReadAllText($f.FullName)
        $t2 = $t.Replace($full, $hidden).Replace($full.Replace('\', '\\'), $hidden).Replace($leaf, $hidden)
        $t2 = $t2.Replace($folder, '<user folder>').Replace($folder.Replace('\', '\\'), '<user folder>')
        if ($t2 -ne $t) { [IO.File]::WriteAllText($f.FullName, $t2, (New-Object Text.UTF8Encoding($false))) }
    }
}

function Add-Conditions($row, [string]$base) {
    $c = Read-KeyValues "$base.conditions.txt"
    foreach ($k in $c.Keys) { if (-not $row.Contains($k)) { $row[$k] = $c[$k] } }
}

$directDevices = @()
$directStress = @()
$runRows = New-Object System.Collections.ArrayList
$rawRows = New-Object System.Collections.ArrayList

function Get-PathSetup([string]$pathName) {
    # returns @{ Output; Exe; Probe } with the executables to use for this path
    $setup = @{ Output = ''; Exe = @{}; Probe = '' }
    foreach ($p in $Programs) { $setup.Exe[$p] = $ProgramExe[$p] }
    $setup.Probe = $setup.Exe[($Programs | Where-Object { $_ -ne 'v1' } | Select-Object -First 1)]
    switch ($pathName) {
        'winmm-system' { $setup.Output = 'winmm' }
        'kdmapi' { $setup.Output = 'kdmapi' }
        'winmm-direct' { $setup.Output = 'winmm-direct' }
        'null' { $setup.Output = 'null' }
        'winmm-wrp' {
            $setup.Output = 'winmm'
            # One private folder per program: the program's executable and the wrapper DLL side by side.
            foreach ($p in $Programs) {
                $dir = Join-Path $wrpDir $p
                New-Item -ItemType Directory -Force -Path $dir | Out-Null
                $name = Split-Path -Leaf $ProgramExe[$p]
                Copy-Item -LiteralPath $ProgramExe[$p] -Destination (Join-Path $dir $name) -Force
                Copy-Item -LiteralPath $WinmmWrp -Destination (Join-Path $dir 'winmm.dll') -Force
                $setup.Exe[$p] = Join-Path $dir $name
            }
            if ($setup.Probe) { $setup.Probe = $setup.Exe[($Programs | Where-Object { $_ -ne 'v1' } | Select-Object -First 1)] }
        }
    }
    Assert-Silent $setup.Output
    return $setup
}

# Programs that have a given path: v1 has no direct WinMM path.
function Programs-For([string]$pathName) {
    if ($pathName -eq 'winmm-direct') { return @($Programs | Where-Object { $_ -ne 'v1' }) }
    return $Programs
}

function Run-Measured([string]$program, [string]$pathName, [string]$fixtureName, [string]$instr, [int]$playCount = 0, [int]$timeout = 0) {
    # (PowerShell variable names are case-insensitive: the parameter must not be called "repeats".)
    $nPlays = if ($playCount -gt 0) { $playCount } else { $Repeats }
    if ($timeout -le 0) { $timeout = $ProcessTimeoutSeconds }
    $setup = Get-PathSetup $pathName
    $exe = $setup.Exe[$program]
    $fixture = Get-Fixture $fixtureName $pathName
    $label = "$program-$pathName-$fixtureName" + $(if ($instr -ne 'sample') { "-$instr" } else { '' })
    $cmdArgs = @('run', '--output', $setup.Output, '--fixture', $fixture, '--out', $rawDir, '--label', $label,
        '--repeats', [string]$nPlays, '--instr', $instr, '--sample-every', '128', '--device-name', $DeviceName, '--cleanup-log', (Join-Path $rawDir "$label.cleanup.txt"))
    $t = Invoke-Test $label $exe $cmdArgs $timeout
    $uf = $null
    if ($fixtureName -like 'user-file-*') {
        $n = [int]($fixtureName.Substring('user-file-'.Length))
        $uf = $userFiles | Where-Object { $_.Index -eq $n } | Select-Object -First 1
        Hide-UserName $label $uf
    }
    $firstRow = $runRows.Count
    Add-PlayRows $program $pathName $fixtureName $instr $label $uf $t 0 $nPlays
    # Adaptive extension (adaptive-repeats.ps1): after the first 3 plays, one more play at a time (a separate process
    # of one play, label <label>-x<k>), up to 7, while the plays of this one measurement spread over 10 % of their median.
    # Not used when -Repeats was given, for -Quick/-DryRun, or for the user-file passes (their play count is fixed).
    if ($script:AdaptiveRepeats -and $playCount -le 0 -and $t.Code -eq 0) {
        $total = $nPlays; $reason = ''
        while ($true) {
            $vals = @($runRows | Select-Object -Skip $firstRow | Where-Object { $_.throughput_over_ended_meps } | ForEach-Object { [double]::Parse([string]$_.throughput_over_ended_meps, $inv) })
            $v = Get-AdaptiveVerdict -Samples @{ 'Meps' = $vals } -Done $total -Max 7
            if (-not $v.Extend) { $reason = $v.Reason; break }
            $extLabel = "$label-x$($total + 1)"
            $extArgs = @('run', '--output', $setup.Output, '--fixture', $fixture, '--out', $rawDir, '--label', $extLabel,
                '--repeats', '1', '--instr', $instr, '--sample-every', '128', '--device-name', $DeviceName, '--cleanup-log', (Join-Path $rawDir "$extLabel.cleanup.txt"))
            $tx = Invoke-Test $extLabel $exe $extArgs $timeout
            Add-PlayRows $program $pathName $fixtureName $instr $extLabel $uf $tx $total 1
            $total++
        }
        Log ("$label : " + (Format-AdaptiveNote -Used $total -Explicit $false -Reason $reason))
    }
}

# Reads the result files of $count plays of one process into rows; repeatIndex continues after $indexOffset plays.
function Add-PlayRows([string]$program, [string]$pathName, [string]$fixtureName, [string]$instr, [string]$label, $uf, $t, [int]$indexOffset, [int]$count) {
    for ($r = 1; $r -le $count; $r++) {
        $base = Join-Path $rawDir "$label.r$r"
        $row = [ordered]@{ program = $program; path = $pathName; fixtureName = $fixtureName; repeatIndex = ($r + $indexOffset); instrMode = $instr }
        $info = Read-KeyValues "$base.run.txt"
        foreach ($k in $info.Keys) { if (-not $row.Contains($k)) { $row[$k] = $info[$k] } }
        if ($uf) { $row['fixture'] = "user file $($uf.Index)" }
        Add-Conditions $row (Join-Path $rawDir $label)
        if (Test-Path -LiteralPath "$base.samples.bin") {
            $an = & $analyzer analyze "$base.samples.bin" "$base.timeline.csv"
            foreach ($line in $an) {
                $i = $line.IndexOf('=')
                if ($i -gt 0 -and -not $row.Contains($line.Substring(0, $i))) { $row[$line.Substring(0, $i)] = $line.Substring($i + 1) }
            }
            $an | Set-Content -LiteralPath "$base.analysis.txt" -Encoding UTF8
        }
        if (-not $info.Contains('end_reason')) { $row['end_reason'] = "NO-RESULT(exit $($t.Code))" }
        [void]$runRows.Add([pscustomobject]$row)
    }
}

function Run-Raw([string]$program, [string]$pathName, [string]$layer) {
    $setup = Get-PathSetup $pathName
    $exe = $setup.Exe[$program]
    $fixture = Get-Fixture "clustered-1M" $pathName
    $seconds = if ($Quick) { 1 } else { 2 }
    $label = "raw-$program-$pathName-$layer"
    $cmdArgs = @('raw', '--output', $setup.Output, '--layer', $layer, '--seconds', [string]$seconds, '--repeats', [string]$Repeats, '--fixture', $fixture, '--device-name', $DeviceName, '--cleanup-log', (Join-Path $rawDir "$label.cleanup.txt"))
    $t = Invoke-Test $label $exe $cmdArgs 300
    foreach ($line in $t.Lines) {
        if ($line -like 'RAW *') {
            $m = [regex]::Match($line, 'identity=\[(.*)\] meps=(.*)$')
            $row = [ordered]@{ program = $program; path = $pathName; layer = $layer; identity = $m.Groups[1].Value; meps_runs = $m.Groups[2].Value }
            Add-Conditions $row (Join-Path $rawDir $label)
            [void]$rawRows.Add([pscustomobject]$row)
        }
    }
}

function Get-Median([double[]]$v) {
    if ($v.Count -eq 0) { return $null }
    $s = $v | Sort-Object
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[($n - 1) / 2] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

# ---------------------------------------------------------------- the user's files: passages on the real paths (final v1-vs-v2 comparison)
#
# -UserFiles <p1>,<p2>,... -UserPassages '1=123-126;2=133-138,195-210;3=60-75'   (file number = position in -UserFiles;
# start-end in source seconds; several passages per file allowed). Per file (smallest first, the biggest last), per program,
# the file is loaded ONCE per process group and every passage is played on every path of the group in the same process:
#   group "system": kdmapi, winmm-direct, null, and winmm-system last (a hopeless flood can hang the driver's close);
#   group "wrp"   : winmm-wrp (a private copy of the program next to the winmmwrp DLL).
# Modes per passage: direct (start exactly at the passage start; the state chase restores controllers and programs), earlier
# (start -EarlierBy seconds before it, arriving at it in playback), fromstart (-FromStart: play from 0:00 through the passage).
# The measurement programs report lag the way the players define it ("Maximum lag": the largest lateness of any event sent
# since playback started) and are run with the players' default settings (Rate model None, Chase MIDI state on Play/Seek on).

$UserFilesMode = ($UserFilePaths.Count -gt 0)
$passageRows = New-Object System.Collections.ArrayList
$loadRows = New-Object System.Collections.ArrayList
$userNotes = New-Object System.Collections.ArrayList

function Add-Note([string]$text) { [void]$userNotes.Add($text); Log "NOTE: $text" }

# Path name -> output kind of the measurement programs, process group, whether the path needs the winmmwrp copy.
# Data driven: a path whose kind a program does not list in its `outputs` answer is skipped for that program with a note.
$UserPathDefs = [ordered]@{
    'winmm-wrp'    = @{ Kind = 'winmm';        Group = 'wrp' }
    'kdmapi'       = @{ Kind = 'kdmapi';       Group = 'system' }
    'winmm-direct' = @{ Kind = 'winmm-direct'; Group = 'system' }
    'winmm-system' = @{ Kind = 'winmm';        Group = 'system' }
    'null'         = @{ Kind = 'null';         Group = 'system' }
}

function Get-SupportedOutputs([string]$exe) {
    try {
        $o = & $exe outputs 2>$null
        foreach ($l in @($o)) { if ("$l" -like 'outputs=*') { return @(("$l".Substring(8)) -split ',') } }
    } catch { }
    return @('null', 'winmm', 'kdmapi')   # an older build without the `outputs` command
}

function Parse-IndexedList([string]$text, [string]$what) {
    # '1=a,b;2=c' -> @{ 1 = 'a,b'; 2 = 'c' }
    $map = @{}
    foreach ($part in ($text -split ';')) {
        $p = $part.Trim(); if (-not $p) { continue }
        $m = [regex]::Match($p, '^(\d+)\s*=\s*(.+)$')
        if (-not $m.Success) { throw "bad $what part '$p' (use N=value;N=value)" }
        $map[[int]$m.Groups[1].Value] = $m.Groups[2].Value
    }
    return $map
}

function Fmt([double]$v) { return $v.ToString('0.###', $inv) }

# The windows (tool passages) for one user file: per passage, per mode.
function Get-FileWindows($uf, $passageMap, $modeMap) {
    $wins = @()
    $k = 0
    if (-not $passageMap.ContainsKey($uf.Index)) { return $wins }
    $modes = if ($modeMap.ContainsKey($uf.Index)) { @($modeMap[$uf.Index] -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) } else { @('direct', 'earlier') }
    foreach ($range in ($passageMap[$uf.Index] -split ',')) {
        $rm = [regex]::Match($range.Trim(), '^([0-9.]+)\s*-\s*([0-9.]+)$')
        if (-not $rm.Success) { throw "bad passage '$range' for user file $($uf.Index) (use start-end in seconds)" }
        $ps = [double]::Parse($rm.Groups[1].Value, $inv); $pe = [double]::Parse($rm.Groups[2].Value, $inv)
        foreach ($mode in $modes) {
            if ($mode -notin 'direct', 'earlier') { throw "unknown mode '$mode' (direct, earlier; -FromStart is a separate switch)" }
            $k++
            $ws = if ($mode -eq 'direct') { $ps } else { [math]::Max(0.0, $ps - $EarlierBy) }
            $wins += [pscustomobject]@{ K = $k; Mode = $mode; PassStart = $ps; PassEnd = $pe; WinStart = $ws; WinEnd = $pe; Mark = $ps }
        }
    }
    return $wins
}

function Get-WindowSpec($wins) { return (($wins | ForEach-Object { '{0}-{1}:{2}' -f (Fmt $_.WinStart), (Fmt $_.WinEnd), (Fmt $_.Mark) }) -join ',') }

function Test-UserFilesUnchanged([string]$when) {
    foreach ($uf in $userFiles) {
        $now = Get-Item -LiteralPath $uf.Path -ErrorAction SilentlyContinue
        if (-not $now -or $now.Length -ne $uf.Length -or $now.LastWriteTimeUtc -ne $uf.Utc) {
            Log "WARNING: user file $($uf.Index) changed size or modified time ($when)"
            $script:monitorFailures += "user file $($uf.Index) changed ($when)"
        }
    }
}

# "user file N" instead of the real names in every text file the runs of that file wrote.
function Hide-UserNameByIndex($uf) {
    $full = $uf.Path; $leaf = Split-Path -Leaf $full; $folder = Split-Path -Parent $full; $hidden = "user file $($uf.Index)"
    foreach ($f in (Get-ChildItem -LiteralPath $rawDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*user-file-$($uf.Index)*" -and $_.Extension -in '.txt', '.jsonl', '.csv' })) {
        $t = [IO.File]::ReadAllText($f.FullName)
        $t2 = $t.Replace($full, $hidden).Replace($full.Replace('\', '\\'), $hidden).Replace($leaf, $hidden)
        $t2 = $t2.Replace($folder, '<user folder>').Replace($folder.Replace('\', '\\'), '<user folder>')
        if ($t2 -ne $t) { [IO.File]::WriteAllText($f.FullName, $t2, (New-Object Text.UTF8Encoding($false))) }
    }
}

function Test-MemoryFor([string]$program, [double]$fileMB) {
    if ($NoMemoryCheck -or $DryRun) { return $true }
    # From the user's history: v1 needs about 0.03 GB per MB of file (23 GB for 809 MB, 50 GB for 1,849 MB); v2 is expected
    # to need less (measured by -LoadOnly); the margin covers Windows and the other programs.
    $needGB = if ($program -eq 'v1') { 0.03 * $fileMB + 3 } else { 0.012 * $fileMB + 3 }
    $freeGB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
    if ($freeGB -lt $needGB) {
        Add-Note ("{0} skipped: about {1:N0} GB are needed, only {2:N1} GB of physical memory are free. Close programs and run again (-NoMemoryCheck overrides)." -f $program, $needGB, $freeGB)
        return $false
    }
    Log ("memory check {0}: about {1:N0} GB needed, {2:N1} GB free" -f $program, $needGB, $freeGB)
    return $true
}

# A private copy of the program next to the winmmwrp DLL (only while the run lasts; removed in the finally block).
function Get-WrpExe([string]$program) {
    $dir = Join-Path $wrpDir "$program-user"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $name = Split-Path -Leaf $ProgramExe[$program]
    Copy-Item -LiteralPath $ProgramExe[$program] -Destination (Join-Path $dir $name) -Force
    Copy-Item -LiteralPath $WinmmWrp -Destination (Join-Path $dir 'winmm.dll') -Force
    return (Join-Path $dir $name)
}

function Get-UserRow($program, [string]$pathName, $uf, $win, [string]$instr, [string]$base, [string]$procName, $loadInfo) {
    $row = [ordered]@{ program = $program; arch = $Arch; user_file = "user file $($uf.Index)"; path = $pathName; mode = $win.Mode
        passage = ('{0}-{1} s' -f (Fmt $win.PassStart), (Fmt $win.PassEnd)); window = ('{0}-{1} s' -f (Fmt $win.WinStart), (Fmt $win.WinEnd))
        window_k = $win.K; instr = $instr }
    $info = Read-KeyValues "$base.run.txt"
    $row['result'] = if ($info.Contains('end_reason')) { $info['end_reason'] } else { 'NO-RESULT' }
    $row['chase'] = $info['chase_midi_state_on_play_seek']
    $row['rate_model'] = $info['rate_model']
    $row['instrumentation'] = $info['instrumentation']
    $row['window_events'] = $info['window_events']
    $row['passage_events'] = $info['passage_events']
    $row['events_sent'] = $info['window_sends']
    $row['window_complete'] = $info['window_complete']
    $row['capped'] = $info['capped']
    $row['wall_cap_s'] = $info['wall_cap_s']
    $row['ended_ms'] = $info['ended_ms']
    $row['throughput_over_ended_meps'] = $info['throughput_over_ended_meps']
    $row['max_lag_ms'] = if ($info['engine_max_lag_us']) { [math]::Round([double]::Parse($info['engine_max_lag_us'], $inv) / 1000.0, 1).ToString($inv) } else { '' }
    $row['max_lag_at_mark_ms'] = if ($info['max_lag_at_mark_us']) { [math]::Round([double]::Parse($info['max_lag_at_mark_us'], $inv) / 1000.0, 1).ToString($inv) } else { '' }
    $row['current_lag_end_ms'] = if ($info['engine_current_lag_us']) { [math]::Round([double]::Parse($info['engine_current_lag_us'], $inv) / 1000.0, 1).ToString($inv) } else { '' }
    foreach ($k in 'eff_speed_overall_pct', 'eff_speed_min_pct', 'eff_speed_p50_pct', 'eff_speed_passage_overall_pct', 'eff_speed_passage_min_pct', 'eff_speed_passage_p50_pct',
        'held_at_start_voices', 'held_at_start_distinct', 'held_at_mark_voices', 'pedal_down_channels_at_start', 'chase_sends', 'chase_check',
        'chase_restored_cc7_volume', 'chase_restored_cc11_expression', 'chase_restored_cc64_sustain', 'chase_restored_program', 'chase_restored_bank_msb', 'chase_restored_bank_lsb',
        'dropped_events', 'cpu_other_processes_pct', 'timer_resolution_us_before_play', 'playback_failed') { $row[$k] = $info[$k] }
    if (Test-Path -LiteralPath "$base.samples.bin") {
        $an = & $analyzer analyze "$base.samples.bin"
        $an | Set-Content -LiteralPath "$base.analysis.txt" -Encoding UTF8
        $a = [ordered]@{}
        foreach ($line in $an) { $i = $line.IndexOf('='); if ($i -gt 0) { $a[$line.Substring(0, $i)] = $line.Substring($i + 1) } }
        foreach ($k in 'throughput_meps', 'late_us_p50', 'late_us_p99', 'late_us_max', 'stall_count_gt_1ms', 'stall_count_gt_250us', 'samples') { $row["a_$k"] = $a[$k] }
    }
    $row['load_s'] = $loadInfo['load_s']
    $row['peak_private_mb_after_load'] = $loadInfo['peak_private_mb_after_load']
    $row['peak_working_set_mb_after_load'] = $loadInfo['peak_working_set_mb_after_load']
    Add-Conditions $row (Join-Path $rawDir $procName)
    return $row
}

# One process = one program loading one user file once and playing every window on every path of its group.
function Invoke-UserJob($job) {
    $uf = $job.File; $program = $job.Program
    $fileMB = $uf.Length / 1MB
    if (-not (Test-MemoryFor $program $fileMB)) { return }
    $exe = if ($job.Group -eq 'wrp' -and -not $DryRun) { Get-WrpExe $program } else { $ProgramExe[$program] }
    $supported = Get-SupportedOutputs $exe
    $remaining = @()
    foreach ($pn in $job.Paths) {
        $kind = if ($DryRun) { 'null' } else { $UserPathDefs[$pn].Kind }
        if ($supported -notcontains $kind) { Add-Note "$program skips path $pn (user file $($uf.Index)): this build does not list output '$kind' (path not available in these tools yet)"; continue }
        $remaining += $pn
    }
    $spec = Get-WindowSpec $job.Windows
    $instrList = @($PassageInstr | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    for ($attempt = 1; $attempt -le 3 -and $remaining.Count -gt 0; $attempt++) {
        $kinds = @($remaining | ForEach-Object { if ($DryRun) { 'null' } else { $UserPathDefs[$_].Kind } })
        $caps = @($remaining | ForEach-Object { if ($CapPaths -contains $_) { Fmt $CapExtraSeconds } else { Fmt $DefaultCapExtraSeconds } })
        $procName = "$program-$($job.Group)-user-file-$($uf.Index)" + $(if ($job.Tag) { "-$($job.Tag)" } else { '' }) + $(if ($attempt -gt 1) { "-retry$attempt" } else { '' })
        $args2 = @('run', '--outputs', ($kinds -join ','), '--path-tags', ($remaining -join ','), '--fixture', $uf.Path, '--out', $rawDir, '--label', $procName,
            '--label-template', "$program-{path}-user-file-$($uf.Index)$(if ($job.Tag) { '-' + $job.Tag } else { '' })-p{pass}", '--passages', $spec,
            '--instr', ($instrList -join ','), '--repeats', '1', '--chase', [string]$Chase, '--sample-every', '128', '--device-name', 'OmniMIDI',
            '--cap-extras', ($caps -join ','), '--path-settle-ms', [string]$PathSettleMs)
        Log ("USER JOB {0} {1} user file {2}: paths {3}; windows {4}; instrumentation {5}; attempt {6}" -f $program, $job.Group, $uf.Index, ($remaining -join ','), $spec, ($instrList -join ','), $attempt)
        Test-UserFilesUnchanged "before $procName"
        $t = Invoke-Test $procName $exe $args2 $UserProcessTimeoutSeconds
        Test-UserFilesUnchanged "after $procName"
        Hide-UserNameByIndex $uf
        $load = Read-KeyValues (Join-Path $rawDir "$procName.load.txt")
        if ($load.Count -gt 0 -and $attempt -eq 1) {
            [void]$loadRows.Add([pscustomobject][ordered]@{ program = $program; arch = $Arch; user_file = "user file $($uf.Index)"; group = $job.Group
                size_mb = [math]::Round($fileMB, 1); events = $load['events']; duration_s = [math]::Round([double]::Parse($load['duration_ms'], $inv) / 1000.0, 1)
                load_s = $load['load_s']; ready_to_play_s = $load['ready_to_play_s']; chase_prepare_s = $load['chase_prepare_s']
                private_mb_after_load = $load['private_mb_after_load']; peak_private_mb = $load['peak_private_mb_after_load']; peak_working_set_mb = $load['peak_working_set_mb_after_load'] })
        }
        $still = @()
        foreach ($pn in $remaining) {
            $expected = 0; $found = 0
            foreach ($w in $job.Windows) {
                foreach ($instr in $instrList) {
                    $expected++
                    $suffix = if ($instrList.Count -gt 1) { "-i$instr" } else { '' }
                    $base = Join-Path $rawDir ("$program-$pn-user-file-$($uf.Index)$(if ($job.Tag) { '-' + $job.Tag } else { '' })-p$($w.K)$suffix.r1")
                    if (Test-Path -LiteralPath "$base.run.txt") { $found++ }
                    [void]$passageRows.Add([pscustomobject](Get-UserRow $program $pn $uf $w $instr $base $procName $load))
                }
            }
            $errFile = Join-Path $rawDir ("$program-$pn-user-file-$($uf.Index)$(if ($job.Tag) { '-' + $job.Tag } else { '' })-pall.error.txt")
            if ($found -eq 0 -and -not (Test-Path -LiteralPath $errFile)) { $still += $pn }
            elseif ($found -eq 0) { Add-Note "$program path $pn (user file $($uf.Index)): the output could not be opened (see raw/*.error.txt)" }
            elseif ($found -lt $expected) { Add-Note "$program path $pn (user file $($uf.Index)): only $found of $expected runs finished" }
        }
        # Rows of paths that will be re-run in a new process are dropped (the retry produces them).
        if ($still.Count -gt 0 -and $attempt -lt 3) {
            $drop = @($passageRows | Where-Object { $_.program -eq $program -and $_.user_file -eq "user file $($uf.Index)" -and $still -contains $_.path -and $_.result -eq 'NO-RESULT' -and $_.mode -eq $(if ($job.Tag) { 'fromstart' } else { $_.mode }) })
            foreach ($d in $drop) { $passageRows.Remove($d) }
            Add-Note "$program (user file $($uf.Index)): path(s) $($still -join ',') did not run (the driver was ended by its watchdog or the process stopped); running them again in a new process (the file is loaded again)"
        }
        elseif ($still.Count -gt 0) { Add-Note "$program (user file $($uf.Index)): path(s) $($still -join ',') gave no result after 3 processes" }
        $remaining = $still
    }
}

function Invoke-UserComparison {
    $passageMap = Parse-IndexedList $UserPassages '-UserPassages'
    $modeMap = Parse-IndexedList $UserModes '-UserModes'
    $UserPaths = @($UserPaths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    foreach ($pn in $UserPaths) { if (-not $UserPathDefs.Contains($pn)) { Add-Note "path '$pn' is unknown to this script and is skipped" } }
    $paths = @($UserPaths | Where-Object { $UserPathDefs.Contains($_) })
    if (-not $wrpOk -and -not $DryRun) { if ($paths -contains 'winmm-wrp') { Add-Note 'path winmm-wrp skipped: the winmmwrp DLL is missing or not for this architecture' }; $paths = @($paths | Where-Object { $_ -ne 'winmm-wrp' }) }
    # winmm-system last in its group: after a flood the driver can hang in close, and the watchdog then ends the process.
    $systemPaths = @($paths | Where-Object { $UserPathDefs[$_].Group -eq 'system' -and $_ -ne 'winmm-system' }) + @($paths | Where-Object { $_ -eq 'winmm-system' })
    $wrpPaths = @($paths | Where-Object { $UserPathDefs[$_].Group -eq 'wrp' })
    $ordered = @($userFiles | Sort-Object { [double]$_.Length })   # the biggest file last
    $jobs = @()
    foreach ($uf in $ordered) {
        if ($Arch -eq 'x86' -and ($uf.Length / 1MB) -gt $X86MaxFileMB) { Add-Note "user file $($uf.Index) ($([math]::Round($uf.Length / 1MB)) MB) is skipped for x86: 32-bit programs cannot hold files this large"; continue }
        $wins = Get-FileWindows $uf $passageMap $modeMap
        if ($wins.Count -eq 0 -and -not $LoadOnly) { Add-Note "user file $($uf.Index) has no passage in -UserPassages; skipped"; continue }
        foreach ($program in $Programs) {
            if ($LoadOnly) { $jobs += @{ File = $uf; Program = $program; Group = 'load'; Paths = @(); Windows = @(); Tag = '' }; continue }
            if ($systemPaths.Count -gt 0) { $jobs += @{ File = $uf; Program = $program; Group = 'system'; Paths = $systemPaths; Windows = $wins; Tag = '' } }
            if ($wrpPaths.Count -gt 0) { $jobs += @{ File = $uf; Program = $program; Group = 'wrp'; Paths = $wrpPaths; Windows = $wins; Tag = '' } }
        }
        if ($FromStart -and $uf.Index -eq $FromStartFile -and $passageMap.ContainsKey($uf.Index) -and -not $LoadOnly) {
            $first = $wins | Select-Object -First 1
            $fw = [pscustomobject]@{ K = 100; Mode = 'fromstart'; PassStart = $first.PassStart; PassEnd = $first.PassEnd; WinStart = 0.0; WinEnd = $first.PassEnd; Mark = $first.PassStart }
            $fp = @($FromStartPath -split ',' | Where-Object { $UserPathDefs.Contains($_) -and $paths -contains $_ })
            if ($fp.Count -eq 0) { Add-Note "-FromStart: path '$FromStartPath' is not among the available paths; skipped" }
            foreach ($program in $Programs) {
                foreach ($pn in $fp) { $jobs += @{ File = $uf; Program = $program; Group = $UserPathDefs[$pn].Group; Paths = @($pn); Windows = @($fw); Tag = 'fromstart' } }
            }
        }
    }
    # The plan
    Log '---- plan (smallest file first; the biggest file last) ----'
    foreach ($j in $jobs) {
        Log ("plan: {0,-3} user file {1} ({2:N0} MB) group {3,-6} {4}{5}" -f $j.Program, $j.File.Index, ($j.File.Length / 1MB), $j.Group, $(if ($j.Group -eq 'load') { 'load time and memory only' } else { 'paths ' + ($j.Paths -join ',') + '; windows ' + (Get-WindowSpec $j.Windows) }), $(if ($j.Tag) { " [$($j.Tag)]" } else { '' }))
    }
    if ($ShowPlan) { Log 'plan only (-ShowPlan): nothing was run'; return }
    foreach ($j in $jobs) {
        if ($j.Group -eq 'load') {
            $uf = $j.File
            if (-not (Test-MemoryFor $j.Program ($uf.Length / 1MB))) { continue }
            $procName = "$($j.Program)-load-user-file-$($uf.Index)"
            Test-UserFilesUnchanged "before $procName"
            $t = Invoke-Test $procName $ProgramExe[$j.Program] @('run', '--load-only', '1', '--fixture', $uf.Path, '--out', $rawDir, '--label', $procName) $UserProcessTimeoutSeconds
            Test-UserFilesUnchanged "after $procName"
            Hide-UserNameByIndex $uf
            $load = Read-KeyValues (Join-Path $rawDir "$procName.load.txt")
            if ($load.Count -gt 0) {
                [void]$loadRows.Add([pscustomobject][ordered]@{ program = $j.Program; arch = $Arch; user_file = "user file $($uf.Index)"; group = 'load-only'
                    size_mb = [math]::Round($uf.Length / 1MB, 1); events = $load['events']; duration_s = [math]::Round([double]::Parse($load['duration_ms'], $inv) / 1000.0, 1)
                    load_s = $load['load_s']; ready_to_play_s = $load['ready_to_play_s']; chase_prepare_s = $load['chase_prepare_s']
                    private_mb_after_load = $load['private_mb_after_load']; peak_private_mb = $load['peak_private_mb_after_load']; peak_working_set_mb = $load['peak_working_set_mb_after_load'] })
            }
            continue
        }
        Invoke-UserJob $j
    }
}

# ---------------------------------------------------------------- the run

# -Aggregate <results folder>: rebuild runs.csv / summary.md from the raw files of an earlier (or interrupted) run.
function Import-Rows {
    foreach ($f in (Get-ChildItem -LiteralPath $rawDir -Filter '*.run.txt' | Sort-Object Name)) {
        $m = [regex]::Match($f.Name, '^(v1|v2(?:-[a-z0-9]+)?)-(kdmapi|winmm-wrp|winmm-system|winmm-direct|null)-(.+?)(?:-(none|count|tsall))?\.r(\d+)\.run\.txt$')
        if (-not $m.Success) { continue }
        $base = $f.FullName -replace '\.run\.txt$', ''
        $row = [ordered]@{ program = $m.Groups[1].Value; path = $m.Groups[2].Value; fixtureName = $m.Groups[3].Value
            repeatIndex = [int]$m.Groups[5].Value; instrMode = $(if ($m.Groups[4].Success) { $m.Groups[4].Value } else { 'sample' }) }
        $info = Read-KeyValues $f.FullName
        foreach ($k in $info.Keys) { if (-not $row.Contains($k)) { $row[$k] = $info[$k] } }
        Add-Conditions $row ($base -replace '\.r\d+$', '')
        if (Test-Path -LiteralPath "$base.samples.bin") {
            foreach ($line in (& $analyzer analyze "$base.samples.bin" "$base.timeline.csv")) {
                $i = $line.IndexOf('=')
                if ($i -gt 0 -and -not $row.Contains($line.Substring(0, $i))) { $row[$line.Substring(0, $i)] = $line.Substring($i + 1) }
            }
        }
        [void]$runRows.Add([pscustomobject]$row)
    }
    foreach ($f in (Get-ChildItem -LiteralPath $rawDir -Filter 'raw-*.stdout.txt')) {
        foreach ($line in (Get-Content -LiteralPath $f.FullName)) {
            if ($line -like 'RAW *') {
                $pm = [regex]::Match($line, 'program=(\S+) .*output=(\S+) layer=(\S+) .*identity=\[(.*)\] meps=(.*)$')
                $pathName = [regex]::Match($f.Name, '^raw-[^-]+(?:-[a-z0-9]+)?-(kdmapi|winmm-wrp|winmm-system|winmm-direct|null)-').Groups[1].Value
                $prog = [regex]::Match($f.Name, '^raw-(v1|v2(?:-[a-z0-9]+)?)-').Groups[1].Value
                $row = [ordered]@{ program = $prog; path = $pathName; layer = $pm.Groups[3].Value; identity = $pm.Groups[4].Value; meps_runs = $pm.Groups[5].Value }
                Add-Conditions $row ($f.FullName -replace '\.stdout\.txt$', '')
                [void]$rawRows.Add([pscustomobject]$row)
            }
        }
    }
}
if ($Aggregate) { Import-Rows } else {
try {
    $activePaths = @($Paths | Where-Object { $_ -ne 'winmm-wrp' -or $wrpOk })
    foreach ($pathName in $activePaths) {
        if ($OverheadOnly -or $StressOnly) { continue }   # -StressOnly: straight to the device list and stress below
        # 1. Raw probe (v2, no engine, one second): how fast can the output take messages at all?
        if ($pathName -eq 'null') { $pathR[$pathName] = 100.0 }
        elseif ($UserOnly) { $pathR[$pathName] = 1.0 }
        else {
            $setup = Get-PathSetup $pathName
            $t = Invoke-Test "probe-$pathName" $setup.Probe @('raw', '--output', $setup.Output, '--layer', 'output', '--seconds', '1', '--repeats', '1', '--device-name', $DeviceName, '--cleanup-log', (Join-Path $rawDir "probe-$pathName.cleanup.txt")) 120
            $probe = @($t.Lines | Where-Object { $_ -like 'RAW *' })
            $m = if ($probe.Count) { [regex]::Match($probe[0], 'meps=([0-9.]+)') } else { $null }
            $pathR[$pathName] = if ($m -and $m.Success) { [double]::Parse($m.Groups[1].Value, $inv) } else { 0.05 }
            Log ("probe {0}: raw send ceiling about {1:N3} Meps" -f $pathName, $pathR[$pathName])
        }
        # 2. Over-capacity file first: the path's real engine capacity.
        if ($Fixtures -contains 'overcap') {
            foreach ($program in (Programs-For $pathName)) { Run-Measured $program $pathName 'overcap' 'sample' }
            $caps = @()
            foreach ($program in (Programs-For $pathName)) {
                $rows = @($runRows | Where-Object { $_.path -eq $pathName -and $_.program -eq $program -and $_.fixtureName -eq 'overcap' -and $_.instrMode -eq 'sample' })
                $vals = @($rows | ForEach-Object { if ($_.throughput_meps) { [double]::Parse($_.throughput_meps, $inv) } })
                if ($vals.Count -gt 0) { $caps += (Get-Median $vals) }
            }
            if ($caps.Count -gt 0) { $pathC[$pathName] = ($caps | Measure-Object -Minimum).Minimum }
            Log ("capacity {0}: {1} Meps (lower of the programs)" -f $pathName, $pathC[$pathName])
        }
        if (-not $pathC.ContainsKey($pathName)) { $pathC[$pathName] = [math]::Min($pathR[$pathName], 4.0) }
        # 3. The other densities (fixture by fixture, v1 then v2, so the two see similar machine conditions).
        foreach ($fx in ($Fixtures | Where-Object { $_ -ne 'overcap' })) {
            foreach ($program in (Programs-For $pathName)) { Run-Measured $program $pathName $fx 'sample' }
        }
        # 4. Loads relative to what this path can really deliver (fidelity near saturation, wherever saturation is).
        foreach ($fraction in $loads) {
            $rate = [math]::Round($pathC[$pathName] * $fraction, 2) * 1000000
            if ($rate -lt 20000) { continue }
            $name = 'clustered-rel{0}' -f [int]($fraction * 100)
            $fixtureDefs[$name] = @{ Kind = 'clustered'; Rate = [long]$rate; Seconds = 3 }
            Log ("relative load {0} on {1}: {2:N2} Meps ({3:P0} of {4:N2})" -f $name, $pathName, ($rate / 1e6), $fraction, $pathC[$pathName])
            foreach ($program in (Programs-For $pathName)) { Run-Measured $program $pathName $name 'sample' }
        }
        # 5. The user's own files, read only, whole, v1 then v2 per file.
        foreach ($uf in $userFiles) {
            foreach ($program in (Programs-For $pathName)) { Run-Measured $program $pathName "user-file-$($uf.Index)" 'sample' $UserRepeats 3600 }
        }
    }
    if ($UserFilesMode) { Invoke-UserComparison }
    if (-not $SkipRaw) {
        foreach ($program in $Programs) {
            foreach ($pathName in $activePaths) {
                if ($program -notin (Programs-For $pathName)) { continue }
                $progKind = if ($program -eq 'v1') { 'v1' } else { 'v2' }
                $layers = switch ("$progKind/$pathName") {
                    'v1/winmm-system' { @('api', 'func', 'output') }
                    'v1/winmm-wrp' { @('api', 'func', 'output') }
                    'v1/kdmapi' { @('delegate', 'dllimport', 'output') }
                    'v2/winmm-system' { @('api', 'output') }
                    'v2/winmm-wrp' { @('api', 'output') }
                    'v2/kdmapi' { @('native', 'output') }
                    'v2/winmm-direct' { @('output') }
                    default { @('output') }
                }
                foreach ($layer in $layers) { Run-Raw $program $pathName $layer }
            }
        }
    }
    # Direct WinMM driver access (DD-085): which devices it can drive (the fallback list), then the stress cycles. Only
    # non-sounding fixtures; the process runs under the native-window monitor like every other test.
    if ($activePaths -contains 'winmm-direct' -and -not $OverheadOnly) {
        $setup = Get-PathSetup 'winmm-direct'
        $t = Invoke-Test 'direct-devices' $setup.Probe @('devices') 120
        $script:directDevices = @($t.Lines)
        if (-not $SkipStress) {
            $stressFixture = Get-Fixture 'clustered-9M' 'winmm-direct'
            $cycles = if ($Quick) { [math]::Min($StressCycles, 20) } else { $StressCycles }
            foreach ($run in (Get-StressRuns)) {
                $t = Invoke-Test $run.Name $setup.Probe (@('direct-stress', '--fixture', $stressFixture, '--device-name', $DeviceName, '--cycles', [string]$cycles, '--flood-ms', '100') + $run.Args) 1800
                foreach ($line in $t.Lines) { if ($line -like 'DIRECT-STRESS *') { $script:directStress += $line } }
            }
        }
    }
    if (-not $SkipOverhead) {
        foreach ($pathName in @($OverheadPaths | Where-Object { $_ -in $activePaths })) {
            foreach ($program in $Programs) {
                foreach ($instr in 'none', 'count', 'tsall') { Run-Measured $program $pathName 'overcap' $instr }
            }
        }
    }
}
finally {
    Stop-Leftovers
    # The private winmmwrp copy is removed again: it exists only while the test runs.
    if (Test-Path -LiteralPath $wrpDir) {
        Get-ChildItem -LiteralPath $wrpDir -Recurse -Filter 'winmm.dll' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    # The user's files were only read: prove it.
    foreach ($uf in $userFiles) {
        $now = Get-Item -LiteralPath $uf.Path -ErrorAction SilentlyContinue
        if (-not $now -or $now.Length -ne $uf.Length -or $now.LastWriteTimeUtc -ne $uf.Utc) { Log "WARNING: user file $($uf.Index) changed size or modified time during the run"; $monitorFailures += "user file $($uf.Index) changed" }
    }
    if ($userFiles.Count -gt 0) { Log "user files: $($userFiles.Count) checked, size and modified time unchanged unless a WARNING says otherwise" }
}
}

# ---------------------------------------------------------------- results

$runRows | Export-Csv -LiteralPath (Join-Path $resultDir 'runs.csv') -NoTypeInformation -Encoding UTF8
$rawRows | Export-Csv -LiteralPath (Join-Path $resultDir 'raw-ceilings.csv') -NoTypeInformation -Encoding UTF8

function Cell($rows, [string]$key, [int]$digits = 2) {
    $vals = @($rows | ForEach-Object { $x = $_.$key; if ($null -ne $x -and "$x" -ne '') { [double]::Parse("$x", [Globalization.CultureInfo]::InvariantCulture) } })
    if ($vals.Count -eq 0) { return 'n/a' }
    $med = Get-Median $vals
    $mn = ($vals | Measure-Object -Minimum).Minimum
    $mx = ($vals | Measure-Object -Maximum).Maximum
    $f = "N$digits"
    return ('{0} ({1}-{2})' -f $med.ToString($f), $mn.ToString($f), $mx.ToString($f))
}

$md = New-Object System.Collections.ArrayList
[void]$md.Add("# Device measurement summary ($stamp, $Arch$(if ($DryRun) { ', DRY RUN: null path only' }))")
[void]$md.Add('')
[void]$md.Add('Median of the repeats, with the range (min-max) in brackets. Lateness = actual send time minus intended time, microseconds. Meps = millions of events per second.')
[void]$md.Add("The MIDI data was played as it is, at its volume; no audio setting was changed.")
[void]$md.Add('')
[void]$md.Add("## Conditions")
[void]$md.Add('')
[void]$md.Add("- Power plan: $powerPlan")
$cb = @($runRows | ForEach-Object { $_.cond_cpu_busy_before_pct } | Where-Object { $_ })
$cp = @($runRows | ForEach-Object { $_.cond_proc_perf_before_pct } | Where-Object { $_ })
if ($cb.Count -gt 0) {
    $cbv = $cb | ForEach-Object { [double]::Parse($_, $inv) }
    $cpv = $cp | ForEach-Object { [double]::Parse($_, $inv) }
    [void]$md.Add(('- Whole-PC CPU load before each run: median {0:N1} %, range {1:N1}-{2:N1} % (quiet limit {3} %).' -f (Get-Median $cbv), ($cbv | Measure-Object -Minimum).Minimum, ($cbv | Measure-Object -Maximum).Maximum, $QuietCpuPercent))
    if ($cpv.Count -gt 0) { [void]$md.Add(('- Processor performance (clock relative to base) before each run: median {0:N0} %, range {1:N0}-{2:N0} %.' -f (Get-Median $cpv), ($cpv | Measure-Object -Minimum).Minimum, ($cpv | Measure-Object -Maximum).Maximum)) }
}
$nq = @($runRows | Where-Object { $_.cond_quiet -eq 'no' } | ForEach-Object { "$($_.program)-$($_.path)-$($_.fixtureName)" } | Select-Object -Unique)
[void]$md.Add(("- Runs that had to start while the PC was busier than {0} %: {1}" -f $QuietCpuPercent, $(if ($nq.Count) { $nq -join ', ' } else { 'none' })))
[void]$md.Add('- Every row of runs.csv has the cond_* columns (time, power plan, CPU before/after, processor performance before/after, busiest other processes).')
if ($userFiles.Count -gt 0) {
    [void]$md.Add("- User files: $($userFiles.Count) (called user file 1 to $($userFiles.Count); read only, unchanged; the real names are only in the private map file).")
}
[void]$md.Add('')
$groups = $runRows | Where-Object { $_.instrMode -eq 'sample' } | Group-Object path, fixtureName
foreach ($g in $groups) {
    [void]$md.Add("## $($g.Name)")
    [void]$md.Add('')
    [void]$md.Add('| program | delivered Meps (first-to-last send) | late p50 | p90 | p99 | p99.9 | max | stalls >250us | stalls >1ms | stall ms total | send call ns (mean) | send share % | other-process CPU % |')
    [void]$md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($program in $Programs) {
        $rows = @($g.Group | Where-Object { $_.program -eq $program })
        if ($rows.Count -eq 0) { continue }
        [void]$md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} |' -f $program,
            (Cell $rows 'throughput_meps' 3), (Cell $rows 'late_us_p50' 1), (Cell $rows 'late_us_p90' 1), (Cell $rows 'late_us_p99' 1),
            (Cell $rows 'late_us_p999' 1), (Cell $rows 'late_us_max' 0), (Cell $rows 'stall_count_gt_250us' 0), (Cell $rows 'stall_count_gt_1ms' 0),
            (Cell $rows 'stall_total_ms' 1), (Cell $rows 'send_ns_mean' 1), (Cell $rows 'send_fraction_of_span_pct' 1), (Cell $rows 'cpu_other_processes_pct' 1)))
    }
    [void]$md.Add('')
}
$ohGroups = $runRows | Where-Object { $_.fixtureName -eq 'overcap' } | Group-Object path, program
if ($ohGroups) {
    [void]$md.Add('## Instrumentation overhead (overcap fixture; throughput over the whole run, Meps)')
    [void]$md.Add('')
    [void]$md.Add('| path, program | none (no decorator) | count | sample (every 128th) | tsall (timestamp every send) |')
    [void]$md.Add('|---|---|---|---|---|')
    foreach ($g in $ohGroups) {
        $cells = @()
        foreach ($i in 'none', 'count', 'sample', 'tsall') { $cells += (Cell @($g.Group | Where-Object { $_.instrMode -eq $i }) 'throughput_over_ended_meps' 3) }
        [void]$md.Add(('| {0} | {1} |' -f $g.Name, ($cells -join ' | ')))
    }
    [void]$md.Add('')
}
if ($rawRows.Count -gt 0) {
    [void]$md.Add('## Raw send ceilings (no engine; Meps over the repeats)')
    [void]$md.Add('')
    [void]$md.Add('| program | path | layer | Meps per repeat | what is called |')
    [void]$md.Add('|---|---|---|---|---|')
    foreach ($r in $rawRows) { [void]$md.Add(('| {0} | {1} | {2} | {3} | {4} |' -f $r.program, $r.path, $r.layer, $r.meps_runs, $r.identity)) }
    [void]$md.Add('')
}
if ($directDevices.Count -gt 0) {
    [void]$md.Add('## Direct WinMM driver access: which WinMM devices it can drive (nothing is opened)')
    [void]$md.Add('')
    foreach ($l in $directDevices) { [void]$md.Add('    ' + $l) }
    [void]$md.Add('')
}
if ($directStress.Count -gt 0) {
    [void]$md.Add('## Direct WinMM stress (open, flood, then Stop / close / panic during the flood; repeated)')
    [void]$md.Add('')
    foreach ($l in $directStress) { [void]$md.Add('    ' + $l) }
    [void]$md.Add('')
}
if ($UserFilesMode) {
    # ---- user passages: CSV files (all columns of all rows) and the summary sections
    if ($passageRows.Count -gt 0) {
        $cols = @($passageRows | ForEach-Object { $_.PSObject.Properties.Name } | Select-Object -Unique)
        $passageRows | Select-Object $cols | Export-Csv -LiteralPath (Join-Path $resultDir 'user-passages.csv') -NoTypeInformation -Encoding UTF8
    }
    if ($loadRows.Count -gt 0) { $loadRows | Export-Csv -LiteralPath (Join-Path $resultDir 'load-memory.csv') -NoTypeInformation -Encoding UTF8 }
    [void]$md.Add('## User files: passages on the real paths')
    [void]$md.Add('')
    [void]$md.Add("Settings of every run: Rate model None (Queue, no limit), Chase MIDI state on Play/Seek $(if ($Chase) { 'ON (the players'' default)' } else { 'OFF' }), $Arch. Maximum lag is the players' statistic (the largest lateness of any event sent since playback started, from the engine snapshot). Mode direct = playback starts exactly at the window start; earlier = $EarlierBy s before it, arriving at it in playback; fromstart = from 0:00.")
    [void]$md.Add("Instrumentation: count = a counter per send only (the lowest overhead; the headline columns come from these runs); sample = two timer reads on every 128th send (lateness percentiles and stalls). Both are shown so the effect of the instrumentation on the lag is visible.")
    [void]$md.Add('')
    foreach ($note in $userNotes) { [void]$md.Add("- NOTE: $note") }
    if ($userNotes.Count -gt 0) { [void]$md.Add('') }
    if ($loadRows.Count -gt 0) {
        [void]$md.Add('### Load time and memory')
        [void]$md.Add('')
        [void]$md.Add('| program | arch | file | size MB | events | duration s | load s (open to ready to play) | peak private MB | peak working set MB | group |')
        [void]$md.Add('|---|---|---|---|---|---|---|---|---|---|')
        foreach ($l in $loadRows) { [void]$md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} |' -f $l.program, $l.arch, $l.user_file, $l.size_mb, $l.events, $l.duration_s, $l.load_s, $l.peak_private_mb, $l.peak_working_set_mb, $l.group)) }
        [void]$md.Add('')
    }
    $byWindow = $passageRows | Group-Object { "$($_.user_file)|$($_.window_k)" }
    foreach ($g in ($byWindow | Sort-Object { $_.Group[0].user_file }, { [int]$_.Group[0].window_k })) {
        $first = $g.Group[0]
        [void]$md.Add(("### {0}, passage {1}, mode {2} (window {3}; passage events {4}, window events {5})" -f $first.user_file, $first.passage, $first.mode, $first.window, $first.passage_events, $first.window_events))
        [void]$md.Add('')
        [void]$md.Add('| program | path | events sent | M events/s | MAX LAG ms (count run) | MAX LAG ms (sample run) | lag at passage start ms | late p50 / p99 / max us | stalls >= 1 ms | effective speed % (passage: overall / lowest) | capped | held notes at start | chase |')
        [void]$md.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
        foreach ($pg in ($g.Group | Group-Object program, path)) {
            $cnt = $pg.Group | Where-Object { $_.instr -eq 'count' } | Select-Object -First 1
            $smp = $pg.Group | Where-Object { $_.instr -eq 'sample' } | Select-Object -First 1
            $head = if ($cnt) { $cnt } else { $smp }
            $late = if ($smp -and $smp.a_late_us_p50) { '{0} / {1} / {2}' -f $smp.a_late_us_p50, $smp.a_late_us_p99, $smp.a_late_us_max } else { 'n/a' }
            $stall = if ($smp -and $smp.a_stall_count_gt_1ms) { $smp.a_stall_count_gt_1ms } else { 'n/a' }
            $speed = if ($head.eff_speed_passage_overall_pct) { '{0} / {1}' -f $head.eff_speed_passage_overall_pct, $head.eff_speed_passage_min_pct } else { 'n/a' }
            [void]$md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} |' -f $head.program, $head.path,
                $head.events_sent, $head.throughput_over_ended_meps, $(if ($cnt) { $cnt.max_lag_ms } else { 'n/a' }), $(if ($smp) { $smp.max_lag_ms } else { 'n/a' }),
                $head.max_lag_at_mark_ms, $late, $stall, $speed, $(if ($head.capped -eq 'yes') { "YES (stopped at $($head.wall_cap_s) s)" } else { "no" }), $head.held_at_start_voices, $head.chase))
        }
        [void]$md.Add('')
    }
    [void]$md.Add('Every row of user-passages.csv has the cond_* columns (time, power plan, CPU before/after, processor performance, busiest other processes, quiet yes/no), the chase restore checks (chase_restored_*), the held-note counts and the effective-speed figures.')
    [void]$md.Add('')
}
$findings = @()
foreach ($c in (Get-ChildItem -LiteralPath $rawDir -Filter '*.cleanup.txt' -ErrorAction SilentlyContinue)) {
    $t = Get-Content -LiteralPath $c.FullName -Raw
    if ($t -match 'cleanup-timeout') { $findings += "$($c.Name -replace '\.cleanup\.txt$',''): the driver did not return from reset/close within 20 s after the run; the watchdog ended the process" }
}
if ($findings.Count -gt 0) {
    [void]$md.Add('## Driver findings')
    foreach ($f in $findings) { [void]$md.Add("- $f") }
    [void]$md.Add('')
}
if ($monitorFailures.Count -gt 0) {
    [void]$md.Add('## Problems')
    foreach ($f in $monitorFailures) { [void]$md.Add("- $f") }
}
$md | Set-Content -LiteralPath (Join-Path $resultDir 'summary.md') -Encoding UTF8
Log "results written to $resultDir"
if ($monitorFailures.Count -gt 0) { exit 1 }
exit 0
