# End-to-end test of the Win32 player (midibn-win32.exe) with a real or null output (prompt 008, Step 4).
#
#   .\v2\tools\device\player-e2e.ps1 -None                          # output None: no device, no sound (the dry run)
#   .\v2\tools\device\player-e2e.ps1 -Device OmniMIDI -AllowSound   # a Windows MIDI device by name (substring ok)  -> SOUND
#   .\v2\tools\device\player-e2e.ps1 -Kdmapi -AllowSound            # the KDMAPI box ticked                         -> SOUND
#   -Build x64|x86|xp|xp-ia32   which built player (default x64)      -Exe <path>  an explicit player
#   -Fixture <mid>              the file to play (default: a generated 144 s tune, see listening-tune.ps1)
#   -Scenario Full|Companions|Short   Full = everything below; Companions = the short version used by monitor-launch.ps1;
#                               Short = load, choose the output, Play ~5 s, Stop, Play ~3 s, Stop, close (real-driver checks)
#   -LargeFile                  the file triggers the "Large MIDI memory warning" dialog (a big file, or the player's test switch
#                               --force-large-warning via -PlayerArgs): expect it exactly once after loading starts, log its text,
#                               press Continue, then wait for the load (up to 600 s, progress every 30 s). Without -LargeFile
#                               that dialog is an unexpected window and fails the run.
#   -PlayerArgs <string[]>      extra command-line arguments for the player (e.g. --thread-report <file> --thread-report-delay-ms 3000)
#   -DryRun                     same as -None
#   -Direct                     with -Device: tick the "Direct WinMM" checkbox left of KDMAPI (DD-085, DD-127)
#                               and check its status line while playing (without -Direct the item must stay unticked)
#   -Visible                    run on the real desktop: the player is placed WITHOUT activation on the 1920x1080 monitor
#                               ("Internal Display") instead of the hidden desktop (prompt 009; never the 4K monitor)
# Any real output needs -AllowSound (a deliberate second word), and plays the file's notes through the device.
# No audio setting is ever changed. The player's own saved preferences are not touched (the speed-window menu is not used).
#
# Everything runs on a private HIDDEN desktop (run-hidden.ps1): nothing appears on any monitor, no focus is taken. The
# player is started under v1's native-dialog/process monitor (archive\v1\tools\NativeWindowMonitor.cs), so every window that
# opens or closes is logged and a crash/error dialog fails the run. The player is driven only with window messages.
#
# Steps (Full): load the file; choose the output in the MIDI output box (or tick KDMAPI); Play; Pause; Play;
# seek with the timeline, with -5 sec and with +5 sec; change the Rate model live through all eight choices;
# tick Queue length limit and go through every Overflow choice; open Analysis, then Channels from it; Stop;
# open Track routing (it can only be opened while stopped) and Cancel; Help and About; close the window.
# After every step: the main window must answer a message within 3 s (SendMessageTimeout), and no unexpected window
# (a message box, "#32770") may exist in the process; any such window is captured with PrintWindow, listed and closed.
# Also checked: the state text (Playing/Paused/Stopped), the statistics area changes while playing, the timeline
# stands still while paused and moves on a seek, the process exits with code 0, the monitor saw no error window/process.
# Results: v2\out\e2e\<stamp>-<build>-<output>\ (steps log, pictures of companion windows and of any dialog, monitor log).
# Ends with PLAYER-E2E: PASS or PLAYER-E2E: FAIL (exit code 0 / 1).
[CmdletBinding()]
param(
    [string]$Device = '',
    [switch]$Kdmapi,
    [switch]$None,
    [switch]$DryRun,
    [switch]$AllowSound,
    [ValidateSet('x64', 'x86', 'xp', 'xp-ia32')][string]$Build = 'x64',
    [string]$Exe = '',
    [string]$Fixture = '',
    [ValidateSet('Full', 'Companions', 'Short')][string]$Scenario = 'Full',
    [switch]$LargeFile,
    [string[]]$PlayerArgs = @(),
    [string]$OutDir = '',
    [int]$TimeoutMinutes = 12,
    [switch]$GateSelfTest,
    [switch]$Direct,
    [switch]$Visible,
    [switch]$Inner
)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
if ($DryRun) { $None = [switch]$true; $Device = ''; $Kdmapi = [switch]$false }
$modeCount = [int][bool]$Device + [int][bool]$Kdmapi + [int][bool]$None
if ($modeCount -ne 1) { throw 'Give exactly one of -None (or -DryRun), -Device <name>, -Kdmapi.' }
$mode = if ($None) { 'none' } elseif ($Kdmapi) { 'kdmapi' } else { 'device' }
if ($mode -ne 'none' -and -not $AllowSound) { throw 'This run would send the file to a real output and make sound. Add -AllowSound to confirm, or use -None.' }
if ($GateSelfTest -and $mode -ne 'none') { throw '-GateSelfTest is for -None only.' }
if ($Scenario -eq 'Companions' -and $mode -ne 'none') { throw 'The Companions scenario is for output None only.' }
if ($Direct -and $mode -ne 'device') { throw '-Direct needs -Device <name> (direct access applies to WinMM devices only).' }
if (-not $Exe) { $Exe = Join-Path $repo "v2\out\build-$Build\Release\midibn-win32.exe" }
if (-not (Test-Path -LiteralPath $Exe)) { throw "player not found: $Exe (build it: .\v2\tools\build-player.ps1 -Build $Build)" }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$outputTag = if ($mode -eq 'none') { 'none' } elseif ($mode -eq 'kdmapi') { 'kdmapi' } elseif ($Direct) { 'device-direct' } else { 'device' }
if (-not $OutDir) { $OutDir = Join-Path $repo ("v2\out\e2e\{0}-{1}-{2}-{3}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Build, $outputTag, $Scenario.ToLower()) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# ================================================================================================ outer: hidden desktop
if (-not $Inner) {
    $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
    $a = "-Inner -Build $Build -Scenario $Scenario -Exe $(& $q $Exe) -OutDir $(& $q $OutDir) -TimeoutMinutes $TimeoutMinutes"
    if ($Fixture) { $a += " -Fixture $(& $q $Fixture)" }
    if ($LargeFile) { $a += ' -LargeFile' }
    if ($PlayerArgs.Count -gt 0) { $a += ' -PlayerArgs @(' + ((@($PlayerArgs) | ForEach-Object { & $q $_ }) -join ',') + ')' }
    if ($GateSelfTest) { $a += ' -GateSelfTest' }
    if ($Direct) { $a += ' -Direct' }
    if ($mode -eq 'none') { $a += ' -None' } elseif ($mode -eq 'kdmapi') { $a += ' -Kdmapi -AllowSound' } else { $a += " -Device $(& $q $Device) -AllowSound" }
    Write-Host ("player-e2e: {0} build, output {1}{2}, scenario {3}; running on a hidden desktop; results in {4}" -f $Build, $outputTag, $(if ($Device) { " '$Device'" } else { '' }), $Scenario, $OutDir)
    if ($mode -ne 'none') { Write-Host 'player-e2e: THIS RUN SENDS THE FILE TO A REAL OUTPUT AND MAKES SOUND (confirmed by -AllowSound).' }
    if ($Visible) {
        Write-Host 'player-e2e: VISIBLE run on the real desktop (the player is placed on the 1920x1080 monitor without activation)'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& '$($MyInvocation.MyCommand.Path)' $a -Visible" 2>&1 | Tee-Object -FilePath (Join-Path $OutDir 'e2e.log')
        exit $LASTEXITCODE
    }
    & (Join-Path $repo 'v2\tools\visual\run-hidden.ps1') -File $MyInvocation.MyCommand.Path -Arguments $a -LogFile (Join-Path $OutDir 'e2e.log') -WorkingDirectory $repo -TimeoutMinutes ($TimeoutMinutes + 3)
    exit $LASTEXITCODE
}

# ================================================================================================ inner (on the hidden desktop)
if (-not ('E2eDriver' -as [type])) {
    Add-Type -Path (Join-Path $repo 'v2\tools\visual\VisualCapture.cs'), (Join-Path $repo 'v2\tools\visual\VisualDriver.cs'), (Join-Path $here 'E2eDriver.cs'), (Join-Path $here 'WatchDriver.cs') -ReferencedAssemblies System.Drawing
}
[VisualDriver]::MakeDpiAware()

# the monitor (compiled untracked, as monitor-launch.ps1 does)
$monDir = Join-Path $repo 'v2\out\monitor'
New-Item -ItemType Directory -Force -Path $monDir | Out-Null
$monitor = Join-Path $monDir 'NativeWindowMonitor.exe'
$monSource = Join-Path $repo 'archive\v1\tools\NativeWindowMonitor.cs'
if (-not (Test-Path -LiteralPath $monitor) -or (Get-Item $monSource).LastWriteTime -gt (Get-Item $monitor).LastWriteTime) {
    & 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe' /nologo /target:exe /platform:anycpu /optimize+ /warn:4 "/out:$monitor" $monSource
    if ($LASTEXITCODE -ne 0) { throw 'Native-window monitor compilation failed.' }
}

# the file to play
if (-not $Fixture) {
    . (Join-Path $here 'listening-tune.ps1')
    $cycles = if ($Scenario -eq 'Full') { 12 } else { 4 }   # 144 s / 48 s: long enough that the song does not finish while the steps run
    $Fixture = Join-Path $repo ("v2\out\device-test\fixtures\e2e-tune-{0}s.mid" -f (12 * $cycles))
    $info = New-ListeningTune -Path $Fixture -Seconds 12 -Cycles $cycles
    Write-Host "fixture: generated tune ($info)"
}
elseif (-not (Test-Path -LiteralPath $Fixture)) { throw "fixture not found: $Fixture" }
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path

# ---------------------------------------------------------------- bookkeeping
$script:Fails = New-Object System.Collections.ArrayList
$script:Passes = 0
$script:Pings = New-Object System.Collections.ArrayList
$script:Expected = New-Object 'System.Collections.Generic.HashSet[long]'
$script:Shot = 0
$script:H = [IntPtr]::Zero
$script:PlayerPid = 0
function Pass([string]$s) { Write-Host "PASS $s"; $script:Passes++ }
function Fail([string]$s, [string]$why) { Write-Host "FAIL ${s}: $why"; [void]$script:Fails.Add("${s}: $why") }
function Check([string]$s, [bool]$ok, [string]$why = '') { if ($ok) { Pass $s } else { Fail $s $why } }
function Info([string]$s) { Write-Host "     $s" }
function Wait-Ms([int]$ms) { Start-Sleep -Milliseconds $ms }
function Snap([string]$name, [IntPtr]$h) {
    $script:Shot++
    $png = Join-Path $OutDir ('{0:00}-{1}.png' -f $script:Shot, ($name -replace '[^A-Za-z0-9_-]', '-'))
    try { [void][VisualDriver]::PopupShot($h, $png) } catch { $png = "(capture failed: $($_.Exception.Message))" }
    return $png
}

# control ids of the main window (main_window.h)
$Id = @{ Output = 111; Kdmapi = 112; State = 113; Queue = 114; Overflow = 118; Rate = 120; Timeline = 126; Play = 127; Stop = 128; Back5 = 129; Fwd5 = 130; Analysis = 131; Stats = 133; Direct = 134 }
# system-menu command ids
$Cmd = @{ About = 0x1F20; Help = 0x1F90; Routing = 0x2030 }
function Ctl([string]$name) {
    foreach ($d in [VisualDriver]::Descendants($script:H, $false)) {
        if ([VisualDriver]::CtrlId($d) -eq $Id[$name] -and [VisualDriver]::Cls($d) -notmatch 'Caption') { return $d }
    }
    return [IntPtr]::Zero
}
function State-Text { $c = Ctl 'State'; if ($c -eq [IntPtr]::Zero) { return '?' }; return ([VisualDriver]::Txt($c)).Trim() }
function Play-Text { $c = Ctl 'Play'; if ($c -eq [IntPtr]::Zero) { return '?' }; return [VisualDriver]::Txt($c) }
# Pictures of a control. A single capture can show a transient repaint state (seen: identical timelines whose
# hashes differ in one capture), so judgements use several captures and the most frequent picture.
function Sample-Hashes([IntPtr]$control, [int]$n, [int]$gapMs) {
    $list = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt $n; $i++) { [void]$list.Add([E2eDriver]::RegionHash($script:H, $control)); if ($i -lt $n - 1) { Wait-Ms $gapMs } }
    return , $list
}
function Dominant($list) {
    $g = @($list | Group-Object | Sort-Object Count -Descending)[0]
    return [pscustomobject]@{ Hash = $g.Name; Count = $g.Count; Total = $list.Count }
}
# The usual picture of a control right now (3 captures 150 ms apart).
function Stable-Hash([IntPtr]$control) { return (Dominant (Sample-Hashes $control 3 150)).Hash }
# The picture of a control once it is standing still: at least 4 of 6 captures (250 ms apart) identical; $null if not within $maxMs.
function Wait-Still([IntPtr]$control, [int]$maxMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $d = Dominant (Sample-Hashes $control 6 250)
        if ($d.Count -ge 4) { return $d.Hash }
        if ($sw.ElapsedMilliseconds -gt $maxMs) { return $null }
    }
}
function Wait-State([string[]]$names, [int]$timeoutMs = 8000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $s = State-Text
        if ($names -contains $s) { return $s }
        Wait-Ms 100
    }
    return (State-Text)
}

# Responsiveness and unexpected windows. Returns $true when all is well.
function Health([string]$where) {
    $ok = $true
    $ms = [E2eDriver]::Ping($script:H, 3000)
    [void]$script:Pings.Add($ms)
    if ($ms -lt 0) { Fail "$where : window responsive" 'the main window did not answer a message within 3000 ms'; $ok = $false }
    if (-not (Get-Process -Id $script:PlayerPid -ErrorAction SilentlyContinue)) { Fail "$where : process alive" 'the player process is gone'; return $false }
    foreach ($w in [VisualDriver]::ProcessTopWindows($script:PlayerPid, $true)) {
        if ($w -eq $script:H -or $script:Expected.Contains($w.ToInt64())) { continue }
        $desc = [E2eDriver]::Describe($w)
        $text = [E2eDriver]::AllText($w)
        $png = Snap ("unexpected-window-after-$where") $w
        Fail "$where : no unexpected window" "$desc; text: $text; picture: $png"
        $ok = $false
        $btn = [VisualDriver]::Find($w, 'Button', '', 1)
        if ($btn -ne [IntPtr]::Zero) { [VisualDriver]::Click($btn) } else { [VisualDriver]::Close($w) }
        Wait-Ms 300
    }
    if ($ok) { Pass "$where : responsive ($ms ms), no unexpected window" }
    return $ok
}

function Known-Set { $s = New-Object 'System.Collections.Generic.HashSet[long]'; foreach ($w in [VisualDriver]::ProcessTopWindows($script:PlayerPid, $true)) { [void]$s.Add($w.ToInt64()) }; return , $s }
function Wait-NewWindow($known, [int]$timeoutMs = 8000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        foreach ($w in [VisualDriver]::ProcessTopWindows($script:PlayerPid, $true)) { if (-not $known.Contains($w.ToInt64())) { return $w } }
        Wait-Ms 60
    }
    return [IntPtr]::Zero
}
function Expect([IntPtr]$w) { [void]$script:Expected.Add($w.ToInt64()) }
function Unexpect([IntPtr]$w) { [void]$script:Expected.Remove($w.ToInt64()) }

# FAIL-CLOSED OUTPUT GATE. The player starts with the first REAL device selected, so a run that skipped or lost the
# output choice would make sound. Every Play/Pause click goes through this: the MIDI output box and the KDMAPI box are
# read again and must show exactly what this run's mode requires; otherwise nothing is pressed and the run stops.
#   -None / -DryRun : item "None" selected, KDMAPI unticked.
#   -Device <name>  : the chosen device selected (needs -AllowSound), KDMAPI unticked.
#   -Kdmapi         : KDMAPI ticked (needs -AllowSound).
function Assert-OutputGate {
    $o = Ctl 'Output'; $k = Ctl 'Kdmapi'
    $why = ''
    if ($o -eq [IntPtr]::Zero -or $k -eq [IntPtr]::Zero) { $why = 'the output controls were not found' }
    else {
        $sel = [VisualDriver]::ComboSel($o)
        $item = if ($sel -ge 0) { [VisualDriver]::ComboItem($o, $sel) } else { '(nothing selected)' }
        $kd = [VisualDriver]::CheckState($k)
        if ($mode -eq 'none') {
            if ($sel -ne 0 -or $item -ne 'None') { $why = "output box shows '$item' (index $sel), not None" }
            elseif ($kd -ne 0) { $why = 'KDMAPI is ticked' }
        }
        elseif ($mode -eq 'device') {
            if (-not $AllowSound) { $why = '-AllowSound missing' }
            elseif ($kd -ne 0) { $why = 'KDMAPI is ticked' }
            elseif ($item -eq 'None' -or ($item -ine $Device -and $item -notlike "*$Device*")) { $why = "output box shows '$item', not the requested device '$Device'" }
        }
        elseif ($mode -eq 'kdmapi') {
            if (-not $AllowSound) { $why = '-AllowSound missing' }
            elseif ($kd -ne 1) { $why = 'KDMAPI is not ticked' }
        }
    }
    if (-not $why) {
        # the Direct WinMM checkbox must be exactly as this run needs it (ticked only with -Direct)
        $dc = Ctl 'Direct'
        $d = if ($dc -eq [IntPtr]::Zero) { -1 } else { [VisualDriver]::CheckState($dc) }
        if ($d -lt 0) { $why = 'the Direct WinMM checkbox was not found' }
        elseif (($d -eq 1) -ne [bool]$Direct) { $why = "Direct WinMM box is $(if ($d -eq 1) { 'ticked' } else { 'not ticked' }), this run needs it $(if ($Direct) { 'ticked' } else { 'cleared' })" }
    }
    if ($why) { throw "OUTPUT GATE: not pressing Play - $why" }
}
function Press-Play { Assert-OutputGate; [VisualDriver]::Click((Ctl 'Play')) }

$script:LoadOk = $false
function Step([string]$name, [scriptblock]$body) {
    if ($GateSelfTest -and $name -notin 'load the file', 'gate self-test', 'close the player') { return }
    if ($name -eq 'gate self-test' -and -not $GateSelfTest) { return }
    if (-not $script:LoadOk -and $name -ne 'load the file' -and $name -ne 'close the player') { Write-Host "---- $name : skipped (the file did not load or the output could not be chosen)"; return }
    Write-Host "---- $name"
    try { & $body } catch {
        $msg = $_.Exception.Message
        Fail $name ("exception: " + $msg)
        try { [void](Health "$name (after exception)") } catch { }
        # without the chosen output nothing may be played: the remaining steps are skipped
        if ($name -eq 'choose the output' -or $msg -like 'OUTPUT GATE*') { $script:LoadOk = $false }
    }
}

# Opens a window by a command, checks it, takes its picture, closes it by $closer. Returns nothing.
function Open-Close([string]$name, [scriptblock]$opener, [scriptblock]$closer, [int]$settleMs = 1200) {
    $known = Known-Set
    & $opener
    $w = Wait-NewWindow $known 8000
    if ($w -eq [IntPtr]::Zero) { Fail "$name opens" 'no new window appeared within 8 s'; [void](Health "$name"); return }
    Expect $w
    Pass "$name opens ($([E2eDriver]::Describe($w)))"
    Wait-Ms $settleMs
    [void](Health "$name open")
    $png = Snap $name $w
    Info "picture: $png"
    & $closer $w
    if ([E2eDriver]::WaitGone($w, 6000)) { Pass "$name closes" } else { Fail "$name closes" 'the window is still there 6 s after the close request'; [VisualDriver]::Close($w); Wait-Ms 500 }
    Unexpect $w
    [void](Health "$name closed")
}

# ================================================================================================ start the player under the monitor
Write-Host ("player-e2e (inner): player {0} ({1}); output {2}{3}; scenario {4}; file {5}" -f $Exe, $Build, $outputTag, $(if ($Device) { " '$Device'" } else { '' }), $Scenario, (Split-Path -Leaf $Fixture))
$mlog = Join-Path $OutDir 'monitor.jsonl'
$mout = Join-Path $OutDir 'monitor-stdout.txt'
$merr = Join-Path $OutDir 'monitor-stderr.txt'
function Quote([string]$s) { if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' } return $s }
$autoClose = ($TimeoutMinutes * 60000) + 60000
# -Visible: the player opens directly on Internal Display (--position), so it never appears on the primary (4K) monitor first
$margs = @('--output', $mlog, '--post-exit-ms', '3000', '--poll-ms', '50', '--timeout-ms', [string]($TimeoutMinutes * 60000), '--', $Exe, '--no-settings', '--auto-close-ms', [string]$autoClose) + $(if ($Visible) { @('--position', '3880,1133') } else { @() }) + @($PlayerArgs) + @('--open', $Fixture) | ForEach-Object { Quote $_ }
$mon = Start-Process -FilePath $monitor -ArgumentList ($margs -join ' ') -PassThru -RedirectStandardOutput $mout -RedirectStandardError $merr -WindowStyle Hidden
$null = $mon.Handle
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($script:PlayerPid -eq 0 -and $sw.ElapsedMilliseconds -lt 15000) {
    if (Test-Path $mout) {
        $txt = [string](Get-Content -Raw -LiteralPath $mout -ErrorAction SilentlyContinue)
        $m = [regex]::Match($txt, 'Monitoring PID (\d+)')
        if ($m.Success) { $script:PlayerPid = [int]$m.Groups[1].Value }
    }
    if ($script:PlayerPid -eq 0) { Wait-Ms 100 }
}
if ($script:PlayerPid -eq 0) { Get-Process -Id $mon.Id -ErrorAction SilentlyContinue | Stop-Process -Force; throw 'the monitor did not report the player process' }
Info "monitor pid $($mon.Id), player pid $($script:PlayerPid)"
$script:H = [VisualDriver]::WaitWindow($script:PlayerPid, [IntPtr]::Zero, 'MIDIBottleneckPlayerV2', '', 20000)   # (class of the main window; the process also has system helper windows)

try {
    if ($script:H -eq [IntPtr]::Zero) { throw 'the player main window did not appear' }
    Pass "player started ($([E2eDriver]::Describe($script:H)))"
    if ($Visible) {
        $mons = @([WatchDriver]::Monitors() | ForEach-Object { $f = $_ -split '\|'; $g = $f[1] -split ' '; [pscustomobject]@{ Dev = $f[0]; X = [int]$g[0]; Y = [int]$g[1]; W = [int]$g[2]; H = [int]$g[3]; Mon = $f[4] } })
        $m = $mons | Where-Object { $_.W -eq 1920 -and $_.H -eq 1080 -and $_.Mon -notmatch 'LG' } | Select-Object -First 1
        if ($m) {
            $r = [WatchDriver]::Rect($script:H)
            [WatchDriver]::PlaceNoActivate($script:H, $m.X + 40, $m.Y + 60, [Math]::Min($r[2] - $r[0], 1840), [Math]::Min($r[3] - $r[1], 1000))
            Info "placed on $($m.Dev) (1920x1080) at $($m.X + 40),$($m.Y + 60) without activation"
        }
        else { Fail 'visible placement' 'no 1920x1080 monitor other than the LG one was found; the player stays where Windows put it' }
    }

    Step 'load the file' {
        $sw = [Diagnostics.Stopwatch]::StartNew(); $ready = $false
        $loadLimitMs = if ($LargeFile) { 600000 } else { 90000 }
        $warnSeen = 0; $nextProgress = 30000
        while ($sw.ElapsedMilliseconds -lt $loadLimitMs) {
            $b = Ctl 'Analysis'
            if ($b -ne [IntPtr]::Zero -and [VisualDriver]::Enabled($b) -and ((-not $LargeFile) -or $warnSeen -gt 0)) { $ready = $true; break }
            # -LargeFile: the "Large MIDI memory warning" dialog (class MidiBnDialog) must appear; answer it with Continue
            if ($LargeFile) {
                foreach ($w in [VisualDriver]::ProcessTopWindows($script:PlayerPid, $true)) {
                    if ([VisualDriver]::Cls($w) -eq 'MidiBnDialog' -and ([E2eDriver]::TextOf($w) -eq 'Large MIDI memory warning')) {
                        $warnSeen++
                        Info ("large-file warning text: " + [E2eDriver]::AllText($w))
                        Info ("picture: " + (Snap 'large-memory-warning' $w))
                        $cont = [VisualDriver]::Find($w, 'Button', '=Continue', -1)
                        if ($cont -ne [IntPtr]::Zero) { [VisualDriver]::Click($cont); Info 'pressed Continue' }
                        else { [E2eDriver]::PressEnter($w); Info 'Continue button not found; sent Enter' }
                        [void][E2eDriver]::WaitGone($w, 8000)
                    }
                }
            }
            if (-not (Get-Process -Id $script:PlayerPid -ErrorAction SilentlyContinue)) { break }
            if ($sw.ElapsedMilliseconds -ge $nextProgress) { Info ("still loading... {0} s (warning seen: {1})" -f [int]($sw.ElapsedMilliseconds / 1000), $warnSeen); $nextProgress += 30000 }
            # an error box ("#32770") during the load ends the wait: the file will not load
            $box = @([VisualDriver]::ProcessTopWindows($script:PlayerPid, $true) | Where-Object { [VisualDriver]::Cls($_) -eq '#32770' })
            if ($box.Count -gt 0) { Start-Sleep -Milliseconds 400; break }
            Wait-Ms 150
        }
        if ($LargeFile) { Check 'large-memory warning appeared exactly once' ($warnSeen -eq 1) "the warning was seen $warnSeen times" }
        Check 'file loaded (Analysis button enabled)' $ready "the file did not finish loading in $([int]($loadLimitMs / 1000)) s"
        $script:LoadOk = $ready
        [void](Health 'after load')
        Info ("state text: '" + (State-Text) + "'")
    }

    # -GateSelfTest (output None only, never presses Play): proves the fail-closed gate. The player starts with the first real
    # device selected; the gate must refuse then, and accept only after None has been chosen and read back.
    Step 'gate self-test' {
        $o = Ctl 'Output'
        $first = [VisualDriver]::ComboItem($o, [VisualDriver]::ComboSel($o))
        Info "output box at start: '$first'"
        $refused = $false
        try { Assert-OutputGate } catch { $refused = $true; Info "gate said: $($_.Exception.Message)" }
        Check 'gate refuses while the start-up device is still selected' ($refused -or $first -eq 'None') 'the gate accepted a real device'
        [VisualDriver]::ComboSelect($o, 0); Wait-Ms 500
        $accepted = $true
        try { Assert-OutputGate } catch { $accepted = $false }
        Check 'gate accepts after None is selected and read back' $accepted 'the gate refused although None is selected'
        # a ticked KDMAPI box must be refused too
        $k = Ctl 'Kdmapi'; [VisualDriver]::Click($k); Wait-Ms 400
        $refused2 = $false
        try { Assert-OutputGate } catch { $refused2 = $true }
        Check 'gate refuses while KDMAPI is ticked' $refused2 'the gate accepted a ticked KDMAPI box'
        if ([VisualDriver]::CheckState($k) -eq 1) { [VisualDriver]::Click($k); Wait-Ms 400 }
        Check 'KDMAPI box cleared again' ([VisualDriver]::CheckState($k) -eq 0) 'could not clear KDMAPI'
    }

    Step 'choose the output' {
        $o = Ctl 'Output'
        if ($o -eq [IntPtr]::Zero) { throw 'MIDI output box not found' }
        $items = @(); for ($i = 0; $i -lt [VisualDriver]::ComboCount($o); $i++) { $items += [VisualDriver]::ComboItem($o, $i) }
        Info ("MIDI output list: " + ($items -join ' | '))
        $k = Ctl 'Kdmapi'
        if ($mode -eq 'kdmapi') {
            if ([VisualDriver]::CheckState($k) -ne 1) { [VisualDriver]::Click($k); Wait-Ms 400 }
            Check 'KDMAPI box ticked' ([VisualDriver]::CheckState($k) -eq 1) 'could not tick the KDMAPI box'
            Check 'MIDI output box disabled while KDMAPI is ticked' (-not [VisualDriver]::Enabled($o)) 'the output box is still enabled'
        }
        else {
            if ([VisualDriver]::CheckState($k) -ne 0) { [VisualDriver]::Click($k); Wait-Ms 400 }
            Check 'KDMAPI box not ticked' ([VisualDriver]::CheckState($k) -eq 0) 'KDMAPI is ticked and could not be cleared'
            $want = 0
            if ($mode -eq 'device') {
                $want = -1
                for ($i = 0; $i -lt $items.Count; $i++) { if ($items[$i] -ieq $Device) { $want = $i; break } }
                if ($want -lt 0) { for ($i = 0; $i -lt $items.Count; $i++) { if ($items[$i] -like "*$Device*") { $want = $i; break } } }
                if ($want -lt 0) { throw "no MIDI output named like '$Device' in the list" }
            }
            [VisualDriver]::ComboSelect($o, $want); Wait-Ms 500
            Check "MIDI output '$($items[$want])' selected" ([VisualDriver]::ComboSel($o) -eq $want) 'the selection did not stick'
            Info "output: $($items[$want])"
        }
        $wantDirect = [bool]$Direct
        $dc = Ctl 'Direct'
        for ($try = 0; $try -lt 3 -and ([VisualDriver]::CheckState($dc) -eq 1) -ne $wantDirect; $try++) { [VisualDriver]::Click($dc); Wait-Ms 600 }
        Check "Direct WinMM box $(if ($Direct) { 'ticked' } else { 'not ticked' })" (([VisualDriver]::CheckState($dc) -eq 1) -eq $wantDirect) 'the checkbox did not change (greyed for this output?)'
        [void](Health 'output chosen')
    }

    $tl = Ctl 'Timeline'
    $stats = Ctl 'Stats'

    if ($Scenario -eq 'Short') {
        Step 'Short: Play 5 s, Stop' {
            Press-Play
            $s = Wait-State @('Playing', 'Completed') 10000
            Check 'state is Playing after Play' ($s -eq 'Playing') "state text is '$s'"
            Wait-Ms 5000
            [void](Health 'short playing 1')
            [VisualDriver]::Click((Ctl 'Stop'))
            $s = Wait-State @('Stopped') 8000
            Check 'state is Stopped after Stop' ($s -eq 'Stopped') "state text is '$s'"
            [void](Health 'short stopped 1')
        }
        Step 'Short: Play 3 s, Stop' {
            Press-Play
            $s = Wait-State @('Playing', 'Completed') 10000
            Check 'state is Playing after second Play' ($s -eq 'Playing') "state text is '$s'"
            Wait-Ms 3000
            [void](Health 'short playing 2')
            [VisualDriver]::Click((Ctl 'Stop'))
            $s = Wait-State @('Stopped') 8000
            Check 'state is Stopped after second Stop' ($s -eq 'Stopped') "state text is '$s'"
            [void](Health 'short stopped 2')
        }
    }
    else {
    Step 'Play' {
        Press-Play
        $s = Wait-State @('Playing', 'Completed') 10000
        Check 'state is Playing after Play' ($s -eq 'Playing') "state text is '$s'"
        if ($Direct) {
            Wait-Ms 500
            # the status line is gone (its text is the checkbox tooltip now, DD-127): the box stays ticked and usable
            $dc = Ctl 'Direct'
            Check 'Direct WinMM box ticked and enabled while playing' (([VisualDriver]::CheckState($dc) -eq 1) -and [VisualDriver]::Enabled($dc)) 'the box is not ticked or is greyed'
        }
        Check 'Play button reads Pause while playing' ((Play-Text) -match 'Pause') "button text is '$(Play-Text)'"
        Wait-Ms 600
        $hs = Sample-Hashes $stats 5 300
        Check 'statistics advance while playing' (@($hs | Select-Object -Unique).Count -ge 2) 'the statistics area looked the same in 5 captures over 1.2 s'
        $ts = Sample-Hashes $tl 5 250
        Check 'timeline moves while playing' (@($ts | Select-Object -Unique).Count -ge 3) 'the timeline looked (almost) the same in 5 captures over 1 s'
        [void](Health 'playing')
    }

    Step 'Pause, Play (resume)' {
        Press-Play; $s = Wait-State @('Paused') 6000
        Check 'state is Paused after Pause' ($s -eq 'Paused') "state text is '$s'"
        Check 'Play button reads Resume while paused' ((Play-Text) -match 'Resume|Play') "button text is '$(Play-Text)'"
        # the last live update after a pause may lag a moment (busy PC): the picture must settle within 4 s and then stay
        $b = Wait-Still $tl 4000
        Check 'timeline stands still while paused' ($null -ne $b) 'the timeline kept changing for 4 s after Pause'
        if ($null -ne $b) { Wait-Ms 900; Check 'timeline still the same picture 1 s later' ((Stable-Hash $tl) -eq $b) 'the timeline moved while paused' }
        [void](Health 'paused')
        Press-Play; $s = Wait-State @('Playing', 'Completed') 6000
        Check 'state is Playing after resume' ($s -eq 'Playing') "state text is '$s'"
        Wait-Ms 1500
        $c = Stable-Hash $tl
        Check 'timeline moved on after resume' ($c -ne $b) 'the timeline is where it was when paused'
        [void](Health 'resumed')
    }

    Step 'seek: timeline, -5 sec, +5 sec' {
        # pause first so a changed picture can only mean a changed position
        Press-Play; [void](Wait-State @('Paused') 6000); Wait-Ms 500
        $p0 = Stable-Hash $tl
        $size = [VisualDriver]::ClientSize($tl)
        [E2eDriver]::ClickClient($tl, [int]($size[0] * 0.30), [int]($size[1] / 2)); Wait-Ms 800
        $p1 = Stable-Hash $tl
        Check 'timeline click moves the position' ($p1 -ne $p0) 'the timeline looked the same after the click'
        [void](Health 'seek by timeline')
        [VisualDriver]::Click((Ctl 'Fwd5')); Wait-Ms 800
        $p2 = Stable-Hash $tl
        Check '+5 sec moves the position' ($p2 -ne $p1) 'the timeline looked the same after +5 sec'
        [void](Health '+5 sec')
        [VisualDriver]::Click((Ctl 'Back5')); Wait-Ms 800
        $p3 = Stable-Hash $tl
        Check '-5 sec moves the position' ($p3 -ne $p2) 'the timeline looked the same after -5 sec'
        if ($p3 -eq $p1) { Pass '-5 sec after +5 sec returns to the same position' } else { Info 'NOTE: -5 after +5 did not give the same picture as before (focus or rounding)' }
        [void](Health '-5 sec')
        # seeking while playing
        Press-Play; [void](Wait-State @('Playing', 'Completed') 6000)
        [E2eDriver]::ClickClient($tl, [int]($size[0] * 0.15), [int]($size[1] / 2)); Wait-Ms 700
        [VisualDriver]::Click((Ctl 'Fwd5')); Wait-Ms 500
        [VisualDriver]::Click((Ctl 'Back5')); Wait-Ms 700
        Check 'still Playing after seeking while playing' ((State-Text) -eq 'Playing') "state text is '$(State-Text)'"
        [void](Health 'seeking while playing')
    }

    if ($Scenario -eq 'Full') {
        Step 'change the Rate model live' {
            $rate = Ctl 'Rate'
            $n = [VisualDriver]::ComboCount($rate)
            Info ("Rate model choices: $n")
            $before = [VisualDriver]::ComboSel($rate)
            $order = @(1, 2, 3, 4, 5, 6, 7, 0) | Where-Object { $_ -lt $n }
            foreach ($i in $order) {
                [VisualDriver]::ComboSelect($rate, $i); Wait-Ms 900
                Check "Rate model choice $i selected" ([VisualDriver]::ComboSel($rate) -eq $i) "the box shows choice $([VisualDriver]::ComboSel($rate))"
                $s = State-Text
                Check "player still running after Rate model $i" (@('Playing', 'Paused', 'Completed') -contains $s) "state text is '$s'"
                [void](Health "rate model $i")
            }
            if ($before -ne 0) { [VisualDriver]::ComboSelect($rate, $before); Wait-Ms 600 }
        }

        Step 'Queue length limit and Overflow' {
            $q = Ctl 'Queue'
            if ([VisualDriver]::CheckState($q) -ne 1) { [VisualDriver]::Click($q); Wait-Ms 700 }
            Check 'Queue length limit ticked' ([VisualDriver]::CheckState($q) -eq 1) 'the box did not tick'
            $ov = Ctl 'Overflow'
            Check 'Overflow box enabled with the limit on' ([VisualDriver]::Enabled($ov)) 'the Overflow box is disabled'
            $n = [VisualDriver]::ComboCount($ov)
            Info ("Overflow choices: $n")
            for ($i = 0; $i -lt $n; $i++) {
                [VisualDriver]::ComboSelect($ov, $i); Wait-Ms 800
                Check "Overflow choice $i ($([VisualDriver]::ComboItem($ov, $i))) selected" ([VisualDriver]::ComboSel($ov) -eq $i) "the box shows choice $([VisualDriver]::ComboSel($ov))"
                [void](Health "overflow $i")
            }
            [VisualDriver]::ComboSelect($ov, 0); Wait-Ms 500
            [VisualDriver]::Click($q); Wait-Ms 700
            Check 'Queue length limit cleared again' ([VisualDriver]::CheckState($q) -eq 0) 'the box did not clear'
            [void](Health 'queue limit off')
        }
    }

    Step 'Analysis and Channels' {
        $known = Known-Set
        [VisualDriver]::Click((Ctl 'Analysis'))
        $a = Wait-NewWindow $known 10000
        if ($a -eq [IntPtr]::Zero) { Fail 'Analysis window opens' 'no new window within 10 s'; return }
        Expect $a
        Pass "Analysis window opens ($([E2eDriver]::Describe($a)))"
        Wait-Ms 2500
        [void](Health 'analysis open')
        Info ("picture: " + (Snap 'analysis' $a))
        $known = Known-Set
        $btn = [VisualDriver]::WaitFind($a, 'Button', 'Channels', -1, 5000)
        if ($btn -eq [IntPtr]::Zero) { Fail 'Channels button in Analysis' 'not found' }
        else {
            [VisualDriver]::Click($btn)
            $c = Wait-NewWindow $known 8000
            if ($c -eq [IntPtr]::Zero) { Fail 'Channels window opens' 'no new window within 8 s' }
            else {
                Expect $c
                Pass "Channels window opens ($([E2eDriver]::Describe($c)))"
                Wait-Ms 2000
                [void](Health 'channels open')
                Info ("picture: " + (Snap 'channels' $c))
                [VisualDriver]::Close($c)
                Check 'Channels window closes' ([E2eDriver]::WaitGone($c, 6000)) 'still open 6 s after the close request'
                Unexpect $c
                [void](Health 'channels closed')
            }
        }
        [VisualDriver]::Close($a)
        Check 'Analysis window closes' ([E2eDriver]::WaitGone($a, 6000)) 'still open 6 s after the close request'
        Unexpect $a
        [void](Health 'analysis closed')
    }

    Step 'Stop' {
        [VisualDriver]::Click((Ctl 'Stop'))
        $s = Wait-State @('Stopped') 8000
        Check 'state is Stopped after Stop' ($s -eq 'Stopped') "state text is '$s'"
        Check 'Play button reads Play after Stop' ((Play-Text) -match '^Play') "button text is '$(Play-Text)'"
        [void](Health 'stopped')
    }

    Step 'Track routing (open, Cancel)' {
        Open-Close 'Track routing' { [VisualDriver]::SysCommand($script:H, $Cmd.Routing) } {
            param($w)
            $cancel = [VisualDriver]::WaitFind($w, 'Button', '=Cancel', -1, 3000)
            if ($cancel -ne [IntPtr]::Zero) { [VisualDriver]::Click($cancel) } else { [VisualDriver]::Close($w) }
        } 2000
        Check 'main window enabled again after Track routing' ([VisualDriver]::Enabled($script:H)) 'the main window stayed disabled'
    }

    Step 'Help' {
        Open-Close 'Help' { [VisualDriver]::SysCommand($script:H, $Cmd.Help) } { param($w) [VisualDriver]::Close($w) } 1200
    }
    Step 'About' {
        Open-Close 'About' { [VisualDriver]::SysCommand($script:H, $Cmd.About) } {
            param($w)
            $ok = [VisualDriver]::WaitFind($w, 'Button', '=OK', -1, 2000)
            if ($ok -ne [IntPtr]::Zero) { [VisualDriver]::Click($ok) } else { [VisualDriver]::Close($w) }
        } 1000
    }

    }   # end of the non-Short steps

    Step 'close the player' {
        $proc = Get-Process -Id $script:PlayerPid -ErrorAction SilentlyContinue
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($proc -and -not $proc.HasExited -and $sw.ElapsedMilliseconds -lt 25000) {
            [VisualDriver]::Close($script:H)   # the window asks again later when the output is still stopping
            Wait-Ms 600
            $proc.Refresh()
        }
        Check 'player process exited after the close request' ($proc -eq $null -or $proc.HasExited) 'still running 25 s after WM_CLOSE'
        if ($proc -and -not $proc.HasExited) { $proc.Kill() }
    }
}
catch { Fail 'run' ("exception: " + $_.Exception.Message) }
finally {
    if (-not $mon.HasExited) { [void]$mon.WaitForExit(15000) }
    if (-not $mon.HasExited) { Fail 'monitor' 'did not finish within 15 s after the player closed'; $mon.Kill() }
    Get-Process -Id $script:PlayerPid -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- the monitor's verdict
Write-Host '---- native-window / process monitor'
$monitorExit = if ($mon.HasExited) { $mon.ExitCode } else { -1 }
Check 'monitor exit code 0 (clean exit, nothing suspicious)' ($monitorExit -eq 0) "monitor exit code $monitorExit (0 = clean; 3 = suspicious window/process; 4 = still running at the timeout; else the player's own exit code)"
if (Test-Path $mlog) {
    $lines = @(Get-Content -LiteralPath $mlog)
    $done = $lines | Where-Object { $_ -like '*"event":"monitor-complete"*' } | Select-Object -Last 1
    if ($done) {
        $ec = [regex]::Match($done, '"exitCode":(-?\d+|null)').Groups[1].Value
        Check 'player exit code 0' ($ec -eq '0') "player exit code $ec"
        Check 'monitor: no suspicious window' ($done -like '*"suspiciousWindowDetected":false*') 'a suspicious window was detected (see monitor.jsonl)'
        Check 'monitor: no suspicious process' ($done -like '*"suspiciousProcessDetected":false*') 'a suspicious process was detected (see monitor.jsonl)'
    }
    else { Fail 'monitor log' 'no monitor-complete record' }
    $titles = @()
    foreach ($l in $lines) {
        if ($l -like '*"event":"window-detected"*' -and $l -like "*`"ownerPid`":$($script:PlayerPid),*") {
            $cls = [regex]::Match($l, '"class":"([^"]*)"').Groups[1].Value
            $title = [regex]::Match($l, '"title":"([^"]*)"').Groups[1].Value
            $titles += "$cls '$title'"
        }
    }
    $uniqueTitles = (@($titles | Select-Object -Unique)) -join '; '
    Info ("windows of the player seen by the monitor ({0} events): {1}" -f $titles.Count, $uniqueTitles)
    $arch = [regex]::Match(($lines -join "`n"), '"architecture":"(x64|x86)"').Groups[1].Value
    if ($arch) { Info "player process architecture (monitor): $arch" }
}
else { Fail 'monitor log' 'monitor.jsonl missing' }
$pings = @($script:Pings | Where-Object { $_ -ge 0 })
if ($pings.Count -gt 0) { Info ("responsiveness: {0} checks, slowest answer {1} ms, mean {2:N1} ms" -f $pings.Count, ($pings | Measure-Object -Maximum).Maximum, ($pings | Measure-Object -Average).Average) }

Write-Host ''
if ($script:Fails.Count -gt 0) {
    Write-Host ("PLAYER-E2E: FAIL ({0} problems, {1} checks passed)" -f $script:Fails.Count, $script:Passes)
    foreach ($f in $script:Fails) { Write-Host "  - $f" }
    exit 1
}
Write-Host ("PLAYER-E2E: PASS ({0} checks; {1} build, output {2}, scenario {3})" -f $script:Passes, $Build, $outputTag, $Scenario)
exit 0
