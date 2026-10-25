# Watch and listen: the REAL players (v1 and v2) play heavy passages of the user's own MIDI files, one after the other,
# side by side on one monitor (prompt 008 follow-up; main session only, with sound, after announcing it to the user).
#
#   .\v2\tools\device\watch-passages.ps1 -UserFiles <p1>,<p2>,<p3> -UserPassages '1=123-126;2=133-138,195-210;3=60-75' `
#       -Paths winmm-wrp,kdmapi,winmm-system -AllowSound [-Modes direct,earlier] [-EarlierBy 10] [-Pause 20 | -GoFile C:\path\go.txt] [-Programs v1,v2] [-Monitor 'Internal Display']
#   .\v2\tools\device\watch-passages.ps1 -DryRun -UserFiles <p1> -UserPassages '1=123-126' -Paths kdmapi,winmm-system,winmm-wrp   # no sound, no window on any monitor
#   .\v2\tools\device\watch-passages.ps1 -ListMonitors                                                                          # only lists the monitors and the one that would be used
#
# Passage syntax (the same as run-device-measurements.ps1): <file number>=<start>-<end> in seconds; several passages of one file
# separated by commas, several files by semicolons. The file number is the position in -UserFiles (1 = the first path given).
# Paths: winmm-system (OmniMIDI through the system WinMM, KDMAPI box off), kdmapi (KDMAPI box on), winmm-wrp (WinMM with OmniMIDI chosen,
# but each player runs as a private copy beside the third-party winmm.dll, in a git-ignored folder that is deleted again at the end),
# winmm-direct (v2 only: system WinMM with the system-menu item "Direct WinMM driver access (ModMessage)" ticked; v1 skips it),
# none (output None: the engine-only ceiling of the same load, no device, no sound).
#
# What happens: files are taken in order of size (the biggest last). For each file and each group of paths one v1 and one v2 player
# are started (minimized, then placed WITHOUT activation on the chosen monitor: v1 left, v2 right, about 940 x 700 each; default
# monitor "Internal Display", 1920 x 1080, never "LG HDR 4K" unless -Monitor says so), the file is loaded in both (v1 through its
# File Open dialog, v2 on its command line), and for every path the output is chosen in both players. For every passage and
# every program the player is put at (passage start, or passage start - EarlierBy for the mode earlier) seconds, the shown position is read back (it must be within 1 s,
# otherwise it is retried and reported), then the script prints ONE line
#     NOW: v1 <dash> user file 1, passage 2:03<dash>2:06, OmniMIDI via KDMAPI
# and waits -Pause seconds (default 20) or until the -GoFile appears (it is deleted, then the passage plays). The player plays
# EarlierBy (mode earlier only) + passage + 2 s and is stopped; v1 first, then v2, never both playing. After each play the player's own statistics
# ("Maximum lag", "Events sent / dropped", "Effective speed") are read from its window and logged.
#
# SAFETY (fail closed): the players start on the first REAL MIDI device. The output is chosen BEFORE any Play and the output
# box and the KDMAPI box are read again before EVERY Play; if they do not show the requested path, Play is not pressed and the
# run stops. A real device needs -AllowSound (otherwise the script refuses to start). -DryRun uses output "None" for every path
# (paths are only labelled), runs on a hidden desktop (nothing on any monitor, no focus) and never opens a real device by choice.
#
# Reading the numbers: the players draw their statistics themselves (no Windows text), so the script takes a picture of the
# player window, enlarges the statistics / timeline area and reads it with the Windows text recogniser (Windows.Media.Ocr, part
# of Windows). If a number cannot be read, a picture of the player window is saved under v2\out\device-test\watch\<stamp>\private-png\
# (it may show the file name: private, git-ignored) and the row says so.
#
# Results: v2\out\device-test\watch\<stamp>\watch-log.txt and watch-results.csv. Only "user file N" appears in them; the map from
# number to real file is the private v2\out\device-test\watch\user-file-map-<stamp>.txt. The user files are only read (size and
# modified time are compared before and after). Ends with WATCH-PASSAGES: DRYRUN PASS (n steps) / FAIL, or DONE / FAIL for a real run.
[CmdletBinding()]
param(
    [string[]]$UserFiles = @(),
    [string]$UserPassages = '',
    [string[]]$Paths = @('kdmapi', 'winmm-system', 'winmm-wrp'),
    [string[]]$Modes = @('direct'),
    [int]$EarlierBy = 10,
    [int]$Pause = -1,
    [string]$GoFile = '',
    [string[]]$Programs = @('v1', 'v2'),
    [switch]$AllowSound,
    [switch]$DryRun,
    [switch]$SelfTest,
    [string]$Monitor = 'Internal Display',
    [switch]$ListMonitors,
    [switch]$AcceptLargeFileWarning,
    [int]$Tail = 2,
    [int]$LoadTimeoutMinutes = 60,
    [string]$V1Exe = '',
    [string]$V2Exe = '',
    [string]$WinmmWrp = $env:MIDIBN_WINMMWRP_DLL,   # path of the winmmwrp winmm.dll; required for winmm-wrp (or set MIDIBN_WINMMWRP_DLL)
    [string]$OutDir = '',
    [string]$MapFile = '',
    [string[]]$PowerPlans = @(),
    [switch]$Inner
)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
if ($SelfTest) { $DryRun = [switch]$true }
$Dash = [string][char]0x2014; $EnDash = [string][char]0x2013

# ---------------------------------------------------------------- paths of the paths (data-driven)
# Group 'std' = the ordinary program files; group 'wrp' = private copies beside the wrapper DLL.
$PathDefs = [ordered]@{
    'none'         = @{ Label = 'output None (engine only, no device)'; Group = 'std'; Mode = 'none'; Supported = $true; Direct = $false; V2Only = $false }
    'winmm-system' = @{ Label = 'OmniMIDI via system WinMM'; Group = 'std'; Mode = 'winmm'; Supported = $true; Direct = $false; V2Only = $false }
    'kdmapi'       = @{ Label = 'OmniMIDI via KDMAPI'; Group = 'std'; Mode = 'kdmapi'; Supported = $true; Direct = $false; V2Only = $false }
    'winmm-wrp'    = @{ Label = 'OmniMIDI via WinMM with winmmwrp'; Group = 'wrp'; Mode = 'winmm'; Supported = $true; Direct = $false; V2Only = $false }
    'winmm-direct' = @{ Label = 'OmniMIDI via direct WinMM'; Group = 'std'; Mode = 'winmm'; Supported = $true; Direct = $true; V2Only = $true }
}

# ---------------------------------------------------------------- argument clean-up
function Split-List([string[]]$list) { return @($list | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
$Paths = Split-List $Paths
$Programs = Split-List $Programs
# file paths may legitimately contain commas: a piece that exists as a file is kept whole, otherwise it is split at commas
$fileList = @()
foreach ($u in $UserFiles) {
    if (Test-Path -LiteralPath $u -PathType Leaf) { $fileList += $u }
    else { foreach ($piece in ($u -split ',')) { if ($piece.Trim()) { $fileList += $piece.Trim() } } }
}
foreach ($p in $Paths) { if (-not $PathDefs.Contains($p)) { throw "unknown path '$p' (known: $($PathDefs.Keys -join ', '))" } }
foreach ($p in $Programs) { if ($p -notin 'v1', 'v2') { throw "unknown program '$p' (v1 or v2)" } }

function Parse-Passages([string]$spec, [int]$fileCount) {
    $map = @{}
    foreach ($seg in ($spec -split ';')) {
        $seg = $seg.Trim(); if (-not $seg) { continue }
        if ($seg -notmatch '^(\d+)\s*=\s*(.+)$') { throw "bad passage group '$seg' (expected N=start-end[,start-end...])" }
        $idx = [int]$Matches[1]; $ranges = $Matches[2]
        if ($idx -lt 1 -or $idx -gt $fileCount) { throw "passage group '$seg': file number $idx is outside 1..$fileCount" }
        foreach ($r in ($ranges -split ',')) {
            $r = $r.Trim()
            if ($r -notmatch '^(\d+(?:\.\d+)?)\s*-\s*(\d+(?:\.\d+)?)$') { throw "bad passage '$r' in '$seg' (expected start-end in seconds)" }
            $a = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture); $b = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
            if ($b -le $a) { throw "passage '$r': the end must be after the start" }
            if (-not $map.ContainsKey($idx)) { $map[$idx] = New-Object System.Collections.ArrayList }
            [void]$map[$idx].Add(@{ Start = $a; End = $b })
        }
    }
    return $map
}


# ---------------------------------------------------------------- results folder, stamp
$watchRoot = Join-Path $repo 'v2\out\device-test\watch'
if (-not $OutDir) { $OutDir = Join-Path $watchRoot (Get-Date -Format 'yyyyMMdd-HHmmss') }
$stamp = Split-Path -Leaf $OutDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$logPath = Join-Path $OutDir 'watch-log.txt'
$csvPath = Join-Path $OutDir 'watch-results.csv'
if (-not $MapFile) { $MapFile = Join-Path $watchRoot "user-file-map-$stamp.txt" }

# ---------------------------------------------------------------- the user's files (read only)
$script:Files = @()      # @{ Index; Path; Length; Utc }
function Load-FileList {
    if ($Inner -and (Test-Path -LiteralPath $MapFile)) {
        foreach ($line in (Get-Content -LiteralPath $MapFile -Encoding UTF8)) {
            if ($line -like '#*' -or -not $line.Trim()) { continue }
            $parts = $line -split "`t", 2
            $f = Get-Item -LiteralPath $parts[1]
            $script:Files += @{ Index = [int]$parts[0]; Path = $f.FullName; Length = $f.Length; Utc = $f.LastWriteTimeUtc }
        }
    }
    else {
        for ($i = 0; $i -lt $fileList.Count; $i++) {
            if (-not (Test-Path -LiteralPath $fileList[$i] -PathType Leaf)) { throw "user file $($i + 1) not found" }
            $f = Get-Item -LiteralPath $fileList[$i]
            $script:Files += @{ Index = $i + 1; Path = $f.FullName; Length = $f.Length; Utc = $f.LastWriteTimeUtc }
        }
    }
}
function Test-FilesUnchanged {
    $bad = @()
    foreach ($f in $script:Files) {
        $now = Get-Item -LiteralPath $f.Path -ErrorAction SilentlyContinue
        if (-not $now -or $now.Length -ne $f.Length -or $now.LastWriteTimeUtc -ne $f.Utc) { $bad += $f.Index }
    }
    return , $bad
}

# ================================================================================================ outer (dry run): hidden desktop
if ($DryRun -and -not $Inner) {
    if ($ListMonitors) { }
    else {
        if ($fileList.Count -eq 0) { throw 'give -UserFiles' }
        [void](Parse-Passages $UserPassages $fileList.Count)
        Load-FileList
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $MapFile) | Out-Null
        $mapLines = @("# PRIVATE - maps 'user file N' to the real file. Never copy this into a report or commit it.")
        foreach ($f in $script:Files) { $mapLines += ("{0}`t{1}" -f $f.Index, $f.Path) }
        Set-Content -LiteralPath $MapFile -Value $mapLines -Encoding UTF8
        $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
        $pauseArg = if ($Pause -ge 0) { $Pause } else { 1 }
        $a = "-Inner -DryRun -MapFile $(& $q $MapFile) -OutDir $(& $q $OutDir) -UserPassages $(& $q $UserPassages) -Paths $(& $q ($Paths -join ',')) -Programs $(& $q ($Programs -join ',')) -Modes $(& $q (@($Modes | ForEach-Object { $_ -split "," }) -join ",")) -EarlierBy $EarlierBy -Pause $pauseArg -Tail $Tail -Monitor $(& $q $Monitor) -WinmmWrp $(& $q $WinmmWrp) -LoadTimeoutMinutes $LoadTimeoutMinutes"
        if ($V1Exe) { $a += " -V1Exe $(& $q $V1Exe)" }
        if ($V2Exe) { $a += " -V2Exe $(& $q $V2Exe)" }
        if ($GoFile) { $a += " -GoFile $(& $q $GoFile)" }
        Write-Host ("watch-passages: DRY RUN on a hidden desktop (output None, nothing on any monitor); results in {0}" -f $OutDir)
        & (Join-Path $repo 'v2\tools\visual\run-hidden.ps1') -File $MyInvocation.MyCommand.Path -Arguments $a -LogFile (Join-Path $OutDir 'inner-console.log') -WorkingDirectory $repo -TimeoutMinutes 90
        $code = $LASTEXITCODE
        $bad = Test-FilesUnchanged
        if ($bad.Count -gt 0) { Write-Host ("WATCH-PASSAGES: FAIL - user file(s) {0} changed size or modified time" -f ($bad -join ',')); $code = 1 }
        else { Write-Host 'user files: size and modified time unchanged (checked by the outer script)' }
        exit $code
    }
}

# ================================================================================================ the run itself (hidden desktop in a dry run, the real desktop otherwise)
if (-not ('WatchDriver' -as [type])) {
    Add-Type -Path (Join-Path $repo 'v2\tools\visual\VisualCapture.cs'), (Join-Path $repo 'v2\tools\visual\VisualDriver.cs'), (Join-Path $here 'E2eDriver.cs'), (Join-Path $here 'WatchDriver.cs') -ReferencedAssemblies System.Drawing
}
[VisualDriver]::MakeDpiAware()

# ---------------------------------------------------------------- the monitor
function Get-MonitorList {
    $list = @()
    foreach ($line in [WatchDriver]::Monitors()) {
        $f = $line -split '\|'
        $g = $f[1] -split ' '
        $list += [pscustomobject]@{ Device = $f[0]; Left = [int]$g[0]; Top = [int]$g[1]; Width = [int]$g[2]; Height = [int]$g[3]; Primary = ($f[2] -eq 'primary'); Adapter = $f[3]; Name = $f[4]; Id = $f[5] }
    }
    return , $list
}
# "Internal Display" = device \\.\DISPLAY1 AND 1920x1080 (both must match); "LG HDR 4K" = 3840x2160; otherwise a device name (\\.\DISPLAYn) or text of the monitor/adapter.
function Resolve-Monitor([string]$name) {
    $all = Get-MonitorList
    $n = $name.Trim().ToLowerInvariant()
    if ($n -eq 'internal display') { $m = @($all | Where-Object { $_.Device -eq '\\.\DISPLAY1' -and $_.Width -eq 1920 -and $_.Height -eq 1080 }) }
    elseif ($n -eq 'lg hdr 4k') { $m = @($all | Where-Object { $_.Width -eq 3840 -and $_.Height -eq 2160 }) }
    else { $m = @($all | Where-Object { $_.Device -ieq $name -or $_.Name -like "*$name*" -or $_.Adapter -like "*$name*" }) }
    if ($m.Count -ne 1) { throw ("monitor '{0}' not found exactly once (found {1}); monitors: {2}" -f $name, $m.Count, (($all | ForEach-Object { '{0} {1}x{2}@{3},{4}' -f $_.Device, $_.Width, $_.Height, $_.Left, $_.Top }) -join '; ')) }
    return $m[0]
}
if ($ListMonitors) {
    foreach ($m in (Get-MonitorList)) { Write-Host ('{0}  {1}x{2} at {3},{4}  {5}  adapter="{6}" monitor="{7}"' -f $m.Device, $m.Width, $m.Height, $m.Left, $m.Top, $(if ($m.Primary) { 'primary' } else { 'secondary' }), $m.Adapter, $m.Name) }
    $r = Resolve-Monitor $Monitor
    Write-Host ("-Monitor '{0}' would use {1} ({2}x{3} at {4},{5})" -f $Monitor, $r.Device, $r.Width, $r.Height, $r.Left, $r.Top)
    try { [IO.Directory]::Delete($OutDir) } catch { }   # (the empty results folder made at start-up)
    exit 0
}

# ---------------------------------------------------------------- guards
if ($fileList.Count -eq 0 -and -not $Inner) { throw 'give -UserFiles' }
if (-not $DryRun -and -not $AllowSound) { throw 'This run plays the files through real outputs and makes sound. Add -AllowSound to confirm (the main session does this after announcing it to the user), or use -DryRun.' }
if (-not $DryRun -and $Inner) { throw '-Inner is only for the dry run.' }
if ($Pause -lt 0) { $Pause = if ($DryRun) { 1 } else { 20 } }
if ($GoFile) { $GoFile = [IO.Path]::GetFullPath($GoFile) }
Load-FileList
$passages = Parse-Passages $UserPassages $script:Files.Count
if ($passages.Count -eq 0) { throw 'give -UserPassages' }
if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $MapFile) | Out-Null
    $mapLines = @("# PRIVATE - maps 'user file N' to the real file. Never copy this into a report or commit it.")
    foreach ($f in $script:Files) { $mapLines += ("{0}`t{1}" -f $f.Index, $f.Path) }
    Set-Content -LiteralPath $MapFile -Value $mapLines -Encoding UTF8
}
$privateDir = Join-Path $OutDir 'private-png'
$ocrPng = Join-Path $OutDir 'ocr-tmp.png'

# ---------------------------------------------------------------- log (names of user files never appear)
function Scrub([string]$t) {
    if (-not $t) { return $t }
    foreach ($f in $script:Files) {
        $label = "user file $($f.Index)"
        $leaf = Split-Path -Leaf $f.Path; $folder = Split-Path -Parent $f.Path; $bare = [IO.Path]::GetFileNameWithoutExtension($f.Path)
        foreach ($s in @($f.Path, $f.Path.Replace('\', '\\'), $folder, $folder.Replace('\', '\\'), $leaf, $bare)) {
            if ($s -and $s.Length -ge 4) { $t = [regex]::Replace($t, [regex]::Escape($s), $(if ($s -eq $folder -or $s -eq $folder.Replace('\', '\\')) { '<user folder>' } else { $label }), 'IgnoreCase') }
        }
    }
    return $t
}
# Appends a line; a reader that holds the file for a moment (a log viewer) must not end the run, so it is retried for about 2 s.
function Append-Line([string]$path, [string]$text) {
    for ($i = 0; ; $i++) {
        try { Add-Content -LiteralPath $path -Value $text -Encoding UTF8 -ErrorAction Stop; return }
        catch { if ($i -ge 40) { Write-Host "(could not write to ${path}: $($_.Exception.Message))"; return }; Start-Sleep -Milliseconds 50 }
    }
}
function Log([string]$text) {
    $line = '{0} {1}' -f (Get-Date).ToString('HH:mm:ss'), (Scrub $text)
    Write-Host $line
    Append-Line $logPath $line
}
$script:Fails = New-Object System.Collections.ArrayList
$script:Steps = 0
$script:Plays = 0
function Pass([string]$s) { $script:Steps++; Log "PASS $s" }
function Fail([string]$s, [string]$why) { $script:Steps++; Log "FAIL ${s}: $why"; [void]$script:Fails.Add("${s}: $why") }
function Check([string]$s, [bool]$ok, [string]$why = '') { if ($ok) { Pass $s } else { Fail $s $why } }
function Wait-Ms([int]$ms) { Start-Sleep -Milliseconds $ms }

# ---------------------------------------------------------------- text recogniser (Windows.Media.Ocr, part of Windows 10)
$script:Ocr = $null; $script:AsTask = $null
function Init-Ocr {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    [void][Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
    [void][Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
    [void][Windows.Storage.StorageFile, Windows.Foundation, ContentType = WindowsRuntime]
    $script:AsTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
    $script:Ocr = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    if (-not $script:Ocr) { throw 'no text recogniser language is installed' }
}
function Await($op, [Type]$t) { $task = $script:AsTask.MakeGenericMethod($t).Invoke($null, @($op)); [void]$task.Wait(-1); return $task.Result }
function Ocr-File([string]$png) {
    $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($png)) ([Windows.Storage.StorageFile])
    $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    try {
        $dec = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bmp = Await ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $res = Await ($script:Ocr.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])
        return ($res.Text -replace '\s+', ' ')
    }
    finally { $stream.Dispose() }
}
# The players draw numbers in Consolas, whose slashed zero the recogniser reads as a letter (o, e, a with a stroke): inside number
# tokens those letters mean 0, and I / l mean 1. Unit tokens (ms, x, %) are left alone.
function Fix-Num([string]$s) {
    $tokens = $s -split ' '
    return (($tokens | ForEach-Object {
        if ($_ -match '^(ms|x|%|events/sec|events/s)$') { $_ }
        else { ($_ -replace "[$([char]0x00F8)$([char]0x00D8)eEaAoO$([char]0x00D3)$([char]0x00D2)]", '0') -replace '[Il|]', '1' }
    }) -join ' ')
}
function Read-Region($pl, [string]$ctlName, [int]$scale, [int]$col = 0, [int]$cols = 1, [int]$row = 0, [int]$rows = 1) {
    $c = Ctl $pl $ctlName
    if ($c -eq [IntPtr]::Zero) { return '' }
    [void][WatchDriver]::ControlCellPicture($pl.H, $c, $col, $cols, $row, $rows, $scale, $ocrPng)
    return (Ocr-File $ocrPng)
}
function Keep-Picture($pl, [string]$name) {
    New-Item -ItemType Directory -Force -Path $privateDir | Out-Null
    $png = Join-Path $privateDir (($name -replace '[^A-Za-z0-9_-]', '-') + '.png')
    try { [void][VisualDriver]::PopupShot($pl.H, $png); return $png } catch { return "(picture failed: $($_.Exception.Message))" }
}

# ---------------------------------------------------------------- controls of both players
$V2Id = @{ Output = 111; Kdmapi = 112; State = 113; Timeline = 126; Play = 127; Stop = 128; Analysis = 131; Reset = 132; Stats = 133; Direct = 134 }
function Rect-Inside($inner, $outer) { return ($inner[0] -ge $outer[0] - 2 -and $inner[1] -ge $outer[1] - 2 -and $inner[2] -le $outer[2] + 2 -and $inner[3] -le $outer[3] + 2) }
function V1-Main($pl) {
    if ([VisualDriver]::Alive($pl.H) -and [VisualDriver]::Visible($pl.H)) { return $pl.H }
    foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) {
        if ([VisualDriver]::Cls($w) -like 'WindowsForms10.Window*' -and [VisualDriver]::Txt($w) -eq 'MIDIBottleneck Player') { $pl.H = $w; return $w }
    }
    return [IntPtr]::Zero
}
function Ctl($pl, [string]$name) {
    if ($pl.Prog -eq 'v2') {
        $id = $V2Id[$name]
        foreach ($d in [VisualDriver]::Descendants($pl.H, $false)) { if ([VisualDriver]::CtrlId($d) -eq $id -and [VisualDriver]::Cls($d) -notmatch 'Caption') { return $d } }
        return [IntPtr]::Zero
    }
    $h = V1-Main $pl
    if ($h -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    $all = [VisualDriver]::Descendants($h, $false)
    $btn = { param($pred) foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and (& $pred ([VisualDriver]::Txt($d)))) { return $d } }; return [IntPtr]::Zero }
    switch ($name) {
        'Play' { return (& $btn { param($t) $t -eq 'Play' -or $t -eq 'Pause' -or $t -eq 'Resume' }) }
        'Stop' { return (& $btn { param($t) $t -eq 'Stop' }) }
        'Analysis' { return (& $btn { param($t) $t -like 'Analysis*' }) }
        'Reset' { return (& $btn { param($t) $t -eq 'Reset stats' }) }
        'Open' { return (& $btn { param($t) $t -like 'Open MIDI*' }) }
        'Kdmapi' { return (& $btn { param($t) $t -eq 'KDMAPI' }) }
        'State' { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'STATIC' -and [VisualDriver]::Txt($d) -match '^(Stopped|Playing|Paused|Completed|Stopping|Loading.*)$') { return $d } }; return [IntPtr]::Zero }
        'Output' {
            foreach ($d in $all) {
                if ([VisualDriver]::Cls($d) -notmatch 'COMBOBOX') { continue }
                $n = [VisualDriver]::ComboCount($d); if ($n -lt 1) { continue }
                if ([VisualDriver]::ComboItem($d, 0) -eq 'None' -and $n -ne 8) { return $d }
            }
            return [IntPtr]::Zero
        }
        { $_ -in 'Timeline', 'Stats' } {
            # the players' own drawn controls have no text: they are found by place. Timeline = the empty panel inside the "Playback"
            # group that does not hold the buttons; Stats = the empty panel inside the "Statistics" group.
            $groupName = if ($name -eq 'Timeline') { 'Playback' } else { 'Statistics' }
            $group = [IntPtr]::Zero
            foreach ($d in $all) { if ([VisualDriver]::Cls($d) -like 'WindowsForms10.Window*' -and [VisualDriver]::Txt($d) -eq $groupName) { $group = $d; break } }
            if ($group -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
            $gr = [VisualDriver]::Rect($group)
            $play = & $btn { param($t) $t -eq 'Play' -or $t -eq 'Pause' -or $t -eq 'Resume' }
            $pr = if ($play -ne [IntPtr]::Zero) { [VisualDriver]::Rect($play) } else { @(0, 0, 0, 0) }
            $best = [IntPtr]::Zero; $bestTop = [int]::MaxValue
            foreach ($d in $all) {
                if ($d -eq $group -or [VisualDriver]::Cls($d) -notlike 'WindowsForms10.Window*' -or [VisualDriver]::Txt($d) -ne '') { continue }
                $r = [VisualDriver]::Rect($d)
                if (-not (Rect-Inside $r $gr) -or ($r[2] - $r[0]) -lt 300) { continue }
                $hgt = $r[3] - $r[1]
                if ($name -eq 'Stats') { if ($hgt -lt 60 -or $hgt -gt 140) { continue } }
                else {
                    if ($hgt -lt 20 -or $hgt -gt 70) { continue }
                    if ($pr[0] -ge $r[0] -and $pr[2] -le $r[2] -and $pr[1] -ge $r[1] -and $pr[3] -le $r[3]) { continue }   # holds the Play button: the button row
                }
                if ($r[1] -lt $bestTop) { $best = $d; $bestTop = $r[1] }
            }
            return $best
        }
    }
    return [IntPtr]::Zero
}
function State-Text($pl) { $c = Ctl $pl 'State'; if ($c -eq [IntPtr]::Zero) { return '?' }; return ([VisualDriver]::Txt($c)).Trim() }
function Play-Text($pl) { $c = Ctl $pl 'Play'; if ($c -eq [IntPtr]::Zero) { return '?' }; return [VisualDriver]::Txt($c) }
function Wait-State($pl, [string[]]$names, [int]$timeoutMs = 10000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) { $s = State-Text $pl; if ($names -contains $s) { return $s }; Wait-Ms 100 }
    return (State-Text $pl)
}

# ---------------------------------------------------------------- the output gate (fail closed)
$script:CurMode = 'none'      # none | winmm | kdmapi: what the CURRENT path requires
$script:CurDirect = $false    # v2 only: whether the CURRENT path requires the Direct WinMM box ticked
# The KDMAPI box. v1 draws its check boxes itself (WinForms), so the Windows button query BM_GETCHECK always answers
# "not ticked" there (and UI Automation offers no toggle pattern for it). v1 itself shows the real state: it greys out its
# MIDI output list exactly while KDMAPI is ticked (MainForm.UpdateTransportControls: Enabled = !KDMAPI && items > 0),
# so v1's KDMAPI state is read from that. v2's box is a normal Windows check box.
function Kd-State($pl, [IntPtr]$k) {
    if ($pl.Prog -ne 'v1') { return [VisualDriver]::CheckState($k) }
    $o = Ctl $pl 'Output'
    if ($o -eq [IntPtr]::Zero -or [VisualDriver]::ComboCount($o) -le 0) { return -1 }
    return $(if ([VisualDriver]::Enabled($o)) { 0 } else { 1 })
}
function Kd-Toggle($pl, [IntPtr]$k) { [VisualDriver]::Click($k) }
function Read-Output($pl) {
    $o = Ctl $pl 'Output'; $k = Ctl $pl 'Kdmapi'
    if ($o -eq [IntPtr]::Zero -or $k -eq [IntPtr]::Zero) { return @{ Ok = $false; Why = 'the output controls were not found' } }
    $sel = [VisualDriver]::ComboSel($o)
    $item = if ($sel -ge 0) { [VisualDriver]::ComboItem($o, $sel) } else { '(nothing selected)' }
    return @{ Ok = $true; Item = $item; Sel = $sel; Kd = (Kd-State $pl $k); OutEnabled = [VisualDriver]::Enabled($o) }
}
# Throws (and the caller does not press Play) unless BOTH the MIDI output box and the KDMAPI box show what the current path requires.
function Assert-OutputGate($pl) {
    $r = Read-Output $pl
    $why = ''
    if (-not $r.Ok) { $why = $r.Why }
    elseif ($script:CurMode -eq 'none') {
        if ($r.Sel -ne 0 -or $r.Item -ne 'None') { $why = "output box shows '$($r.Item)' (index $($r.Sel)), not None" }
        elseif ($r.Kd -ne 0) { $why = 'KDMAPI is ticked' }
    }
    else {
        if ($DryRun) { $why = 'dry run: only output None is allowed' }
        elseif (-not $AllowSound) { $why = '-AllowSound missing' }
        elseif ($script:CurMode -eq 'kdmapi') { if ($r.Kd -ne 1) { $why = 'KDMAPI is not ticked' } }
        else {
            if ($r.Kd -ne 0) { $why = 'KDMAPI is ticked' }
            elseif ($r.Item -notlike '*OmniMIDI*') { $why = "output box shows '$($r.Item)', not OmniMIDI" }
        }
    }
    if (-not $why -and $pl.Prog -eq 'v2') {
        # the "Direct WinMM" checkbox left of KDMAPI (DD-127; it replaced the system-menu item and the status line)
        $dc = Ctl $pl 'Direct'
        $d = if ($dc -eq [IntPtr]::Zero) { -1 } else { [VisualDriver]::CheckState($dc) }
        if ($d -lt 0) { $why = 'the Direct WinMM checkbox was not found' }
        elseif (($d -eq 1) -ne $script:CurDirect) { $why = "Direct WinMM box is $(if ($d -eq 1) { 'ticked' } else { 'not ticked' }), the path needs it $(if ($script:CurDirect) { 'ticked' } else { 'cleared' })" }
        elseif ($script:CurDirect -and -not [VisualDriver]::Enabled($dc)) { $why = 'the Direct WinMM box is greyed for this output' }
    }
    if ($why) { throw "OUTPUT GATE: not pressing Play ($($pl.Prog)) - $why" }
}
function Set-PathOutput($pl, [string]$mode) {
    $o = Ctl $pl 'Output'; $k = Ctl $pl 'Kdmapi'
    if ($o -eq [IntPtr]::Zero -or $k -eq [IntPtr]::Zero) { throw 'MIDI output controls not found' }
    if ($pl.Prog -eq 'v2') {
        $dc = Ctl $pl 'Direct'
        for ($try = 0; $try -lt 3 -and ([VisualDriver]::CheckState($dc) -eq 1) -ne $script:CurDirect; $try++) { [VisualDriver]::Click($dc); Wait-Ms 800 }
        Check "$($pl.Prog): Direct WinMM box $(if ($script:CurDirect) { 'ticked' } else { 'cleared' })" (([VisualDriver]::CheckState($dc) -eq 1) -eq $script:CurDirect) 'the checkbox did not change (greyed for this output?)'
    }
    if ($mode -eq 'kdmapi') {
        # v1 greys the KDMAPI box out while output None is selected (seen 2026-10-16): choose the OmniMIDI device first
        if ((Kd-State $pl $k) -ne 1 -and -not [VisualDriver]::Enabled($k)) {
            $omni = -1; for ($i = 0; $i -lt [VisualDriver]::ComboCount($o); $i++) { if ([VisualDriver]::ComboItem($o, $i) -like '*OmniMIDI*') { $omni = $i; break } }
            if ($omni -ge 0) { [VisualDriver]::ComboSelect($o, $omni); Wait-Ms 600 }
        }
        if ((Kd-State $pl $k) -ne 1) { Kd-Toggle $pl $k; Wait-Ms 500 }
        Check "$($pl.Prog): KDMAPI box ticked" ((Kd-State $pl $k) -eq 1) 'could not tick the KDMAPI box'
        return
    }
    if ((Kd-State $pl $k) -ne 0) { Kd-Toggle $pl $k; Wait-Ms 500 }
    if ((Kd-State $pl $k) -ne 0) { throw 'KDMAPI is ticked and could not be cleared' }
    $want = -1
    $items = @(); for ($i = 0; $i -lt [VisualDriver]::ComboCount($o); $i++) { $items += [VisualDriver]::ComboItem($o, $i) }
    if ($mode -eq 'none') { $want = 0 }
    else {
        for ($i = 0; $i -lt $items.Count; $i++) { if ($items[$i] -ieq 'OmniMIDI') { $want = $i; break } }
        if ($want -lt 0) { for ($i = 0; $i -lt $items.Count; $i++) { if ($items[$i] -like '*OmniMIDI*') { $want = $i; break } } }
        if ($want -lt 0) { throw "no MIDI output named like OmniMIDI in the list ($($items -join ' | '))" }
    }
    for ($try = 0; $try -lt 15 -and [VisualDriver]::ComboSel($o) -ne $want; $try++) {
        if ([VisualDriver]::Enabled($o)) { [VisualDriver]::ComboSelect($o, $want) }
        Wait-Ms 400
    }
    Check "$($pl.Prog): MIDI output '$($items[$want])' selected" ([VisualDriver]::ComboSel($o) -eq $want) 'the selection did not stick'
}

# ---------------------------------------------------------------- reading numbers
function Parse-Time([string]$m, [string]$s, [string]$ms) { return ([int]$m * 60 + [int]$s + [int]$ms / 1000.0) }
function Fmt-Ms([double]$sec) { $t = [TimeSpan]::FromSeconds($sec); return ('{0:00}:{1:00}.{2:000}' -f [int][math]::Floor($t.TotalMinutes), $t.Seconds, $t.Milliseconds) }
function Fmt-Min([double]$sec) { $s = [int][math]::Floor($sec); return ('{0}:{1:00}' -f [int][math]::Floor($s / 60), ($s % 60)) }
function Read-TimelineText($pl) {
    $c = Ctl $pl 'Timeline'
    if ($c -eq [IntPtr]::Zero) { return '' }
    [void][WatchDriver]::ControlRightPicture($pl.H, $c, 190, 4, 3, $ocrPng)
    return (Ocr-File $ocrPng)
}
# Position and duration shown by the timeline of a player; $null when it cannot be read.
function Get-Timeline($pl) {
    $rx = '(\d+)\s*[:;]\s*(\d\d)\s*[.,]\s*(\d{3})\D{1,4}(\d+)\s*[:;]\s*(\d\d)\s*[.,]\s*(\d{3})'
    $reads = @()
    for ($try = 0; $try -lt 4 -and $reads.Count -lt 2; $try++) {
        $t = Fix-Num (Read-TimelineText $pl)
        $m = [regex]::Match($t, $rx)
        if ($m.Success) { $reads += , @{ Pos = (Parse-Time $m.Groups[1].Value $m.Groups[2].Value $m.Groups[3].Value); Dur = (Parse-Time $m.Groups[4].Value $m.Groups[5].Value $m.Groups[6].Value) } }
        else { Wait-Ms 300 }
    }
    if ($reads.Count -eq 0) { return $null }
    if ($reads.Count -ge 2 -and [math]::Abs($reads[0].Pos - $reads[1].Pos) -gt 0.5) { Wait-Ms 400; $again = Fix-Num (Read-TimelineText $pl); $m = [regex]::Match($again, $rx); if ($m.Success) { return @{ Pos = (Parse-Time $m.Groups[1].Value $m.Groups[2].Value $m.Groups[3].Value); Dur = (Parse-Time $m.Groups[4].Value $m.Groups[5].Value $m.Groups[6].Value) } } }
    return $reads[$reads.Count - 1]
}
# The statistics area is a grid of 2 columns x 4 rows ("label: value" cells; the same in both players). Each wanted cell is read
# on its own (a picture of the whole area is read in column order and mixes labels and values). Returns a hashtable of the values
# found (Events, Speed, MaxLag, CurLag), possibly empty. Cell order: Timeline/output, Queue, Max rate, Output rate, Events sent /
# dropped, Effective speed, Maximum lag, Current lag.
$StatCells = [ordered]@{ Events = @(0, 2); Speed = @(1, 2); MaxLag = @(0, 3); CurLag = @(1, 3) }
function Read-Stats($pl) {
    $out = @{}
    foreach ($name in $StatCells.Keys) {
        $cell = $StatCells[$name]
        $val = ''
        for ($try = 0; $try -lt 3 -and -not $val; $try++) {   # (the recogniser now and then misses a value: look again)
            $text = Read-Region $pl 'Stats' 3 $cell[0] 2 $cell[1] 4
            $val = if ($text -match '^[^:]*:\s*(.*)$') { $Matches[1] } else { $text }
            $val = (Fix-Num $val.Trim())
            if (-not $val) { Wait-Ms 200 }
        }
        if ($name -eq 'Speed' -and $val -match '^[\d.,]+\s*[xX]$') { $val = ($val -replace '\s*[xX]$', '%') }   # the speed is always shown in % (the recogniser sometimes reads % as x)
        if ($val) { $out[$name] = $val } else { Log "  (statistics cell $name read as '$text')" }
    }
    return $out
}

# ---------------------------------------------------------------- starting and closing players
$script:Players = New-Object System.Collections.ArrayList
$script:Place = @{}
$script:MonRect = $null
function Compute-Placement {
    try { $m = Resolve-Monitor $Monitor }
    catch {
        if (-not $DryRun) { throw }
        Log ('NOTE: ' + $_.Exception.Message + ' - the dry run continues with a nominal 1920x1080 monitor')
        $m = [pscustomobject]@{ Device = '(none)'; Left = 0; Top = 0; Width = 1920; Height = 1080 }
    }
    $script:MonRect = $m
    $w = [math]::Min(940, [int](($m.Width - 30) / 2)); $h = [math]::Min(700, $m.Height - 100)
    $y = $m.Top + [int](($m.Height - $h) / 2) - 20
    $script:Place['v1'] = @(($m.Left + 10), $y, $w, $h)
    $script:Place['v2'] = @(($m.Left + 20 + $w), $y, $w, $h)
    if ($DryRun) { Log ("monitor that a real run would use: {0} ({1}x{2} at {3},{4}); a dry run places no window there (hidden desktop)" -f $m.Device, $m.Width, $m.Height, $m.Left, $m.Top) }
    else { Log ("monitor: {0} ({1}x{2} at {3},{4}); v1 at {5},{6} v2 at {7},{8}, {9}x{10} each" -f $m.Device, $m.Width, $m.Height, $m.Left, $m.Top, $script:Place['v1'][0], $y, $script:Place['v2'][0], $y, $w, $h) }
}
function Get-V1Exe {
    if ($V1Exe) { return (Resolve-Path -LiteralPath $V1Exe).Path }
    $name = 'release-package\MIDIBottleneck-Player-v1.0.44-x64.exe'
    $c = @((Join-Path $repo $name))
    try { $common = (& git -C $repo rev-parse --git-common-dir 2>$null); if ($common) { $cd = if ([IO.Path]::IsPathRooted($common)) { $common } else { Join-Path $repo $common }; $c += (Join-Path (Split-Path -Parent ((Resolve-Path $cd).Path)) $name) } } catch { }
    foreach ($p in $c) { if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path } }
    throw "v1 player not found (looked in: $($c -join '; ')); give -V1Exe"
}
function Get-V2Exe {
    if ($V2Exe) { return (Resolve-Path -LiteralPath $V2Exe).Path }
    $p = Join-Path $repo 'v2\out\build-x64\Release\midibn-win32.exe'
    if (-not (Test-Path -LiteralPath $p)) { throw "v2 player not found: $p (build it: .\v2\tools\build-player.ps1)" }
    return (Resolve-Path -LiteralPath $p).Path
}
$wrpRoot = Join-Path $repo 'v2\out\device-test\x64-winmmwrp-watch'
$script:WrpNote = ''
function Prepare-Wrp([string]$prog, [string]$exe) {
    $dir = Join-Path $wrpRoot $prog
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dest = Join-Path $dir (Split-Path -Leaf $exe)
    Copy-Item -LiteralPath $exe -Destination $dest -Force
    if ($DryRun) { $script:WrpNote = 'dry run: private copy of the player WITHOUT the wrapper DLL (the DLL is not loaded in a dry run)'; return $dest }
    if (-not $WinmmWrp) { throw "winmm-wrp: no winmmwrp DLL given: pass -WinmmWrp <path to the winmmwrp winmm.dll> or set the environment variable MIDIBN_WINMMWRP_DLL" }
    if (-not (Test-Path -LiteralPath $WinmmWrp)) { throw "winmm-wrp: $WinmmWrp not found" }
    $b = [IO.File]::ReadAllBytes($WinmmWrp)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    if ($machine -ne 0x8664) { throw ('winmm-wrp: the DLL is PE machine 0x{0:X4}, not x64' -f $machine) }
    Copy-Item -LiteralPath $WinmmWrp -Destination (Join-Path $dir 'winmm.dll') -Force
    Log ("winmm-wrp: private copy of the {0} player beside winmm.dll (PE x64, {1} bytes) in a git-ignored folder" -f $prog, $b.Length)
    return $dest
}
function Start-Player([string]$prog, [string]$exe, [string]$group, [object]$file) {
    $argText = ''
    # v2 gets an explicit --position inside the chosen monitor (never the monitor under the mouse). v1 has no such switch: it
    # starts minimized and is moved onto the monitor by PlaceNoActivate right after its window appears (never shown elsewhere).
    if ($prog -eq 'v2') {
        $argText = '--no-settings --auto-close-ms 21600000 --open "' + $file.Path + '"'
        if (-not $DryRun -and $script:Place.ContainsKey('v2')) { $argText = "--position $($script:Place['v2'][0]),$($script:Place['v2'][1]) " + $argText }
    }
    $pid2 = [VisualCapture]::Start($exe, $argText, (Split-Path $exe), [bool]$DryRun)   # a real run starts minimized (SW_SHOWMINNOACTIVE): nothing flashes on another monitor
    $pl = @{ Prog = $prog; Pid = $pid2; H = [IntPtr]::Zero; Exe = $exe; Group = $group; Known = (New-Object 'System.Collections.Generic.HashSet[long]'); Loaded = $false; Alive = $true }
    [void]$script:Players.Add($pl)
    if ($prog -eq 'v2') { $pl.H = [VisualDriver]::WaitWindow($pid2, [IntPtr]::Zero, 'MIDIBottleneckPlayerV2', '', 30000) }
    else { $pl.H = [VisualCapture]::WaitMainWindow($pid2, 'MIDIBottleneck Player', 30000) }
    if ($pl.H -eq [IntPtr]::Zero) { Stop-Pl $pl; throw "$prog main window did not appear" }
    $pr = $script:Place[$prog]
    [WatchDriver]::PlaceNoActivate($pl.H, $pr[0], $pr[1], $pr[2], $pr[3])
    Wait-Ms 600
    if (-not $DryRun) {
        $r = [VisualDriver]::Rect($pl.H); $m = $script:MonRect
        $inside = ($r[0] -ge $m.Left - 2 -and $r[1] -ge $m.Top - 2 -and $r[2] -le $m.Left + $m.Width + 2 -and $r[3] -le $m.Top + $m.Height + 2)
        if (-not $inside) { Stop-Pl $pl; throw "$prog window is not on the requested monitor (rect $($r -join ','))" }
    }
    foreach ($w in [VisualDriver]::ProcessTopWindows($pid2, $false)) { [void]$pl.Known.Add($w.ToInt64()) }
    Log ("{0} started (pid {1}), placed {2}x{3}" -f $prog, $pid2, $pr[2], $pr[3])
    return $pl
}
function Stop-Pl($pl) {
    if (-not $pl.Alive) { return }
    $pl.Alive = $false
    try {
        $p = Get-Process -Id $pl.Pid -ErrorAction SilentlyContinue
        if ($p -and -not $p.HasExited) {
            [VisualDriver]::Close($pl.H)
            if (-not $p.WaitForExit(15000)) { $p.Kill(); [void]$p.WaitForExit(10000) }
        }
    } catch { }
}
function Close-All { foreach ($pl in @($script:Players)) { Stop-Pl $pl }; $script:Players.Clear() }

# ---------------------------------------------------------------- loading a file
function Handle-Dialogs($pl) {
    # returns 'ok' | 'failed'. Dialogs the player opens while loading (v1: the large-file memory warning).
    foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) {
        if ($w -eq $pl.H -or $pl.Known.Contains($w.ToInt64())) { continue }
        if ([VisualDriver]::Find($w, 'Edit', '', 1148) -ne [IntPtr]::Zero) { continue }   # the File Open dialog, still closing
        if ([VisualDriver]::Cls($w) -match 'tooltip') { continue }
        $title = [E2eDriver]::TextOf($w); $text = [E2eDriver]::AllText($w)
        Log ("$($pl.Prog): a window appeared while loading: $text")
        if ($title -like 'Large MIDI memory warning*' -and $AcceptLargeFileWarning) {
            $b = [VisualDriver]::Find($w, 'Button', 'Continue', -1)
            if ($b -ne [IntPtr]::Zero) { [VisualDriver]::Click($b); Log "$($pl.Prog): -AcceptLargeFileWarning given: pressed Continue"; [void]$pl.Known.Add($w.ToInt64()); Wait-Ms 500; continue }
        }
        if ($title -like 'MIDI compatibility repair*') {
            $b = [VisualDriver]::Find($w, 'Button', 'OK', -1)
            if ($b -ne [IntPtr]::Zero) { [VisualDriver]::Click($b); [void]$pl.Known.Add($w.ToInt64()); Wait-Ms 500; continue }
        }
        $png = Keep-Picture $pl "unexpected-window-$($pl.Prog)"
        Log "$($pl.Prog): unexpected window left open, load abandoned (picture: $png)"
        return 'failed'
    }
    return 'ok'
}
function Load-File($pl, $file) {
    $free = [math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB, 1)
    Log ("loading user file {0} ({1:N1} MB) in {2}; free physical memory {3} GB" -f $file.Index, ($file.Length / 1MB), $pl.Prog, $free)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($pl.Prog -eq 'v1') {
        $h = V1-Main $pl
        $known = New-Object 'System.Collections.Generic.HashSet[long]'
        foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) { [void]$known.Add($w.ToInt64()) }
        [VisualDriver]::Click((Ctl $pl 'Open'))
        $dlg = [IntPtr]::Zero; $t = [Diagnostics.Stopwatch]::StartNew()
        while ($dlg -eq [IntPtr]::Zero -and $t.ElapsedMilliseconds -lt 10000) {
            foreach ($w in [VisualDriver]::ProcessTopWindows($pl.Pid, $true)) { if (-not $known.Contains($w.ToInt64()) -and $w -ne $h) { $dlg = $w; break } }
            if ($dlg -eq [IntPtr]::Zero) { Wait-Ms 100 }
        }
        if ($dlg -eq [IntPtr]::Zero) { throw 'v1 Open dialog did not appear' }
        Wait-Ms 700
        $edit = [IntPtr]::Zero; $t.Restart()
        while ($edit -eq [IntPtr]::Zero -and $t.ElapsedMilliseconds -lt 5000) { $edit = [VisualDriver]::Find($dlg, 'Edit', '', 1148); if ($edit -eq [IntPtr]::Zero) { Wait-Ms 100 } }
        if ($edit -eq [IntPtr]::Zero) { throw 'v1 Open dialog: file name box not found' }
        [VisualDriver]::SetText($edit, $file.Path)
        Wait-Ms 200
        $ok = [VisualDriver]::Find($dlg, 'Button', '', 1)
        [VisualDriver]::Click($ok)
        Wait-Ms 800
    }
    $deadline = $LoadTimeoutMinutes * 60000
    $ready = $false; $nextMem = 60000
    while ($sw.ElapsedMilliseconds -lt $deadline) {
        if (-not (Get-Process -Id $pl.Pid -ErrorAction SilentlyContinue)) { throw "$($pl.Prog) process ended while loading" }
        if ((Handle-Dialogs $pl) -eq 'failed') { return $false }
        $b = Ctl $pl 'Analysis'
        if ($b -ne [IntPtr]::Zero -and [VisualDriver]::Enabled($b)) { $ready = $true; break }
        if ($sw.ElapsedMilliseconds -gt $nextMem) { $nextMem += 60000; Log ("  still loading in {0} ({1:N0} s; free physical memory {2:N1} GB)" -f $pl.Prog, ($sw.ElapsedMilliseconds / 1000), ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)) }
        Wait-Ms 250
    }
    if (-not $ready) { Log "$($pl.Prog): the file did not finish loading in $LoadTimeoutMinutes minutes"; return $false }
    Wait-Ms 600
    Log ("{0}: loaded in {1:N1} s; free physical memory {2:N1} GB" -f $pl.Prog, ($sw.ElapsedMilliseconds / 1000), ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB))
    $pl.Loaded = $true
    return $true
}

# ---------------------------------------------------------------- the position
# Puts the player at $target seconds: one click on the timeline at the proportional place, then corrections with the timeline's own
# keys (Page Up/Down = 5 s, Right/Left = 1 s). The shown position is read back after each try. Returns @{Ok; Actual; Dur; Tries}.
function Set-Position($pl, [double]$target) {
    $tl = Ctl $pl 'Timeline'
    if ($tl -eq [IntPtr]::Zero) { return @{ Ok = $false; Actual = $null; Dur = 0; Tries = 0; Why = 'timeline control not found' } }
    $info = Get-Timeline $pl
    if (-not $info) { $png = Keep-Picture $pl "timeline-unreadable-$($pl.Prog)"; return @{ Ok = $false; Actual = $null; Dur = 0; Tries = 0; Why = "timeline text not readable (picture $png)" } }
    $dur = $info.Dur
    for ($try = 1; $try -le 5; $try++) {
        if ($try -eq 1) {
            $tr = [WatchDriver]::TimelineTrack($pl.H, $tl)
            $size = [VisualDriver]::ClientSize($tl)
            $left = if ($tr[0] -ge 0) { $tr[0] } else { 8 }; $right = if ($tr[1] -gt $left) { $tr[1] } else { $size[0] - 170 }
            $x = [int]($left + ($right - $left) * [math]::Min(1.0, $target / [math]::Max(1.0, $dur)))
            [E2eDriver]::ClickClient($tl, $x, [int]($size[1] / 2))
            Wait-Ms 900
        }
        else {
            $delta = $target - $info.Pos
            $n = 0
            while ([math]::Abs($delta) -ge 5 -and $n -lt 60) { if ($delta -gt 0) { [VisualDriver]::Key($tl, 0x21); $delta -= 5 } else { [VisualDriver]::Key($tl, 0x22); $delta += 5 }; $n++; Wait-Ms 150 }
            while ([math]::Abs($delta) -ge 0.5 -and $n -lt 60) { if ($delta -gt 0) { [VisualDriver]::Key($tl, 0x27); $delta -= 1 } else { [VisualDriver]::Key($tl, 0x25); $delta += 1 }; $n++; Wait-Ms 150 }
            Wait-Ms 700
        }
        $info = Get-Timeline $pl
        if (-not $info) { $png = Keep-Picture $pl "timeline-unreadable-$($pl.Prog)"; return @{ Ok = $false; Actual = $null; Dur = $dur; Tries = $try; Why = "timeline text not readable after seek (picture $png)" } }
        if ([math]::Abs($info.Pos - $target) -le 1.0) { return @{ Ok = $true; Actual = $info.Pos; Dur = $info.Dur; Tries = $try; Why = '' } }
    }
    return @{ Ok = $false; Actual = $info.Pos; Dur = $info.Dur; Tries = 5; Why = ('position stayed {0:N1} s away from the target' -f [math]::Abs($info.Pos - $target)) }
}

# ---------------------------------------------------------------- one passage on one player
function Csv-Row($fields) { return (($fields | ForEach-Object { $s = [string]$_; if ($s -match '[",\r\n]') { '"' + ($s -replace '"', '""') + '"' } else { $s } }) -join ',') }
Set-Content -LiteralPath $csvPath -Value 'program,path,user file N,passage,start position verified,max lag,events sent,dropped,effective speed,notes' -Encoding UTF8
# The NOW: line is printed bare (no time stamp) so the main session can read it; the log file gets it with a time.
function Say-Raw([string]$line) { Write-Host $line; Append-Line $logPath ('{0} {1}' -f (Get-Date).ToString('HH:mm:ss'), $line) }
function Wait-Go([string]$nowLine) {
    if ($GoFile) {
        Remove-Item -LiteralPath $GoFile -Force -ErrorAction SilentlyContinue
        Say-Raw $nowLine
        Log ("waiting for the go file: {0}" -f $GoFile)
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath $GoFile)) { if ($sw.Elapsed.TotalMinutes -gt 60) { throw 'no go file within 60 minutes' }; Wait-Ms 200 }
        Remove-Item -LiteralPath $GoFile -Force -ErrorAction SilentlyContinue
    }
    else {
        Say-Raw $nowLine
        Wait-Ms ($Pause * 1000)
    }
}
# Each passage once per start mode: direct = the player is put exactly at the passage start (the state chase restores the
# controllers and programs there); earlier = put -EarlierBy seconds before it, so playback arrives at the passage.
function Expand-Modes($list) {
    $out = @()
    foreach ($pass in $list) {
        foreach ($mode in (Split-List $Modes)) {
            if ($mode -notin 'direct', 'earlier') { throw "unknown start mode '$mode' (direct, earlier)" }
            $out += [pscustomobject]@{ Start = $pass.Start; End = $pass.End; Mode = $mode; Lead = $(if ($mode -eq 'earlier') { $EarlierBy } else { 0 }) }
        }
    }
    return $out
}
# Power-plan comparison (prompt 009 only; the user allowed exactly these plans): -PowerPlans <GUID>,<GUID>,... switches the
# active Windows power plan with powercfg /setactive before each group of plays of a passage, waits 15 s, records the processor
# clock, and ALWAYS returns to Balanced afterwards (also when the run fails). Returns the tag ' [plan name]' or '' on failure.
$BalancedPlan = '381b4222-f694-41f0-9685-ff5bb260df2e'
$script:PlanTag = ''
function Switch-Plan([string]$guid) {
    $out = & powercfg /setactive $guid 2>&1
    $act = (& powercfg /getactivescheme) -join ' '
    if ($LASTEXITCODE -ne 0 -or $act -notmatch [regex]::Escape($guid)) {
        Log "POWER PLAN: switch to $guid FAILED ($out); back to Balanced, comparison skipped"
        & powercfg /setactive $BalancedPlan | Out-Null
        return ''
    }
    $name = if ($act -match '\(([^)]+)\)\s*$') { $Matches[1] } else { $guid }
    Wait-Ms 15000
    $perf = try { [math]::Round((Get-Counter '\Processor Information(_Total)\% Processor Performance' -SampleInterval 2 -MaxSamples 1).CounterSamples[0].CookedValue, 0) } catch { '?' }
    $mhz = try { (Get-CimInstance Win32_Processor | Select-Object -First 1).MaxClockSpeed } catch { '?' }
    Log ("POWER PLAN: now {0} ({1}); after 15 s: processor performance {2} %, clock about {3:N0} MHz (base x performance)" -f $name, $guid, $perf, $(if ($perf -is [double] -and $mhz -is [uint32]) { $mhz * $perf / 100 } else { 0 }))
    return " [$name]"
}
function Play-Passage($pl, $file, [int]$pIndex, $pass, [string]$pathName) {
    $def = $PathDefs[$pathName]
    $passText = '{0}{1}{2}' -f (Fmt-Min $pass.Start), $EnDash, (Fmt-Min $pass.End)
    $label = "$($pl.Prog), $pathName$($script:PlanTag), user file $($file.Index), passage $passText"
    $notes = New-Object System.Collections.ArrayList
    $row = @{ pos = ''; lag = ''; sent = ''; dropped = ''; speed = '' }
    $started = $false
    try {
        $target = [math]::Max(0.0, $pass.Start - $pass.Lead)
        $pos = Set-Position $pl $target
        if ($pos.Dur -gt 0 -and $pass.End -gt $pos.Dur) { [void]$notes.Add(('passage ends after the end of the file ({0}); skipped' -f (Fmt-Ms $pos.Dur))); Log "SKIP $label : the file lasts only $(Fmt-Ms $pos.Dur)"; $row.pos = 'skipped'; return }
        if ($pos.Actual -ne $null) { $row.pos = Fmt-Ms $pos.Actual }
        Check "$label : start position within 1 s of $(Fmt-Ms $target)" $pos.Ok "$($pos.Why) (shown: $(if ($pos.Actual -ne $null) { Fmt-Ms $pos.Actual } else { 'unreadable' }))"
        if ($pos.Ok) { Log ("$label : start position verified {0} (wanted {1}, {2} try/tries)" -f (Fmt-Ms $pos.Actual), (Fmt-Ms $target), $pos.Tries) }
        else { [void]$notes.Add("start position NOT verified: $($pos.Why)") }
        # statistics start clean for this passage
        $rs = Ctl $pl 'Reset'; if ($rs -ne [IntPtr]::Zero) { [VisualDriver]::Click($rs); Wait-Ms 300 }
        $nowLine = 'NOW: {0} {1} user file {2}, passage {3}, {4}' -f $pl.Prog, $Dash, $file.Index, $passText, $def.Label
        if ($script:PlanTag) { $nowLine += ",$($script:PlanTag)" }
        $nowLine += $(if ($pass.Lead -gt 0) { ", starting $($pass.Lead) s earlier" } else { ', starting directly at the passage' })
        if ($DryRun) { $nowLine += ' [dry run: output None]' }
        Wait-Go $nowLine
        # the gate: re-read BOTH boxes, then press Play
        Assert-OutputGate $pl
        Pass "$label : output gate passed before Play"
        [VisualDriver]::Click((Ctl $pl 'Play'))
        $started = $true
        $t0 = [Diagnostics.Stopwatch]::StartNew()
        $s = Wait-State $pl @('Playing', 'Completed') 20000
        Check "$label : state is Playing" ($s -eq 'Playing' -or $s -eq 'Completed') "state text is '$s'"
        $playMs = [int](($pass.Lead + ($pass.End - $pass.Start) + $Tail) * 1000)
        while ($t0.ElapsedMilliseconds -lt $playMs) {
            if ((State-Text $pl) -eq 'Completed') { [void]$notes.Add('file ended before the passage end'); break }
            Wait-Ms 250
        }
        # numbers: read while the player still shows them, then stop
        $stats = Read-Stats $pl
        if (-not $stats.ContainsKey('MaxLag')) { Wait-Ms 400; $stats = Read-Stats $pl }
        [VisualDriver]::Click((Ctl $pl 'Stop'))
        $s2 = Wait-State $pl @('Stopped') 30000
        $started = $false
        Check "$label : stopped" ($s2 -eq 'Stopped') "state text is '$s2'"
        $script:Plays++
        if ($stats.Count -ge 3) {
            $row.lag = $stats['MaxLag']
            $row.speed = $stats['Speed']
            if ($stats['Events'] -match '^\s*([\d.,\s]+?)\s*[/|]\s*([\d.,\s]+?)\s*$') { $row.sent = $Matches[1]; $row.dropped = $Matches[2] } else { $row.sent = $stats['Events'] }
            Pass "$label : statistics read (Maximum lag $($row.lag); Events sent / dropped $($stats['Events']); Effective speed $($row.speed))"
            Log ("  all values read: " + (($stats.Keys | ForEach-Object { "$_=$($stats[$_])" }) -join '; '))
        }
        else {
            $png = Keep-Picture $pl ("stats-unreadable-$($pl.Prog)-$pathName-file$($file.Index)-p$pIndex")
            Fail "$label : statistics read" "the statistics could not be read as text; picture of the player window saved ($png)"
            [void]$notes.Add('statistics NOT read as text; picture saved in the private folder')
        }
    }
    catch {
        $msg = $_.Exception.Message
        if ($msg -like 'OUTPUT GATE*') { throw }
        Fail $label ("exception: " + $msg)
        [void]$notes.Add("exception: $msg")
        if ($started) { try { [VisualDriver]::Click((Ctl $pl 'Stop')); [void](Wait-State $pl @('Stopped') 30000) } catch { } }
    }
    finally {
        if ($row.pos -ne 'skipped' -or $notes.Count -gt 0) {
            Append-Line $csvPath (Csv-Row @($pl.Prog, "$pathName$($script:PlanTag)", "user file $($file.Index)", $passText, $row.pos, $row.lag, $row.sent, $row.dropped, $row.speed, (Scrub ($notes -join '; '))))
        }
    }
}

# ================================================================================================ the run
$exitCode = 1
try {
    Log ("watch-passages: {0}; programs {1}; paths {2}; start modes {3}; pause {4}; tail {5} s; results in {6}" -f $(if ($DryRun) { 'DRY RUN (output None, hidden desktop)' } else { 'REAL RUN (sound)' }), ($Programs -join ','), ($Paths -join ','), "$Modes (earlier = $EarlierBy s before)", $(if ($GoFile) { "go file" } else { "$Pause s" }), $Tail, $OutDir)
    foreach ($f in $script:Files) { Log ("user file {0}: {1:N1} MB" -f $f.Index, ($f.Length / 1MB)) }
    Init-Ocr
    Compute-Placement
    if ($GoFile) { Remove-Item -LiteralPath $GoFile -Force -ErrorAction SilentlyContinue }

    # supported paths, in the requested order, grouped by the kind of process they need
    $active = @()
    foreach ($p in $Paths) {
        if (-not $PathDefs[$p].Supported) { Log "NOTE: path $p : path not available in these players yet (skipped)"; continue }
        $active += $p
    }
    if ($active.Count -eq 0) { throw 'no usable path' }
    $groups = @(); foreach ($p in $active) { if ($PathDefs[$p].Group -notin $groups) { $groups += $PathDefs[$p].Group } }

    $v1Path = $null; $v2Path = $null
    if ($Programs -contains 'v1') { $v1Path = Get-V1Exe }
    if ($Programs -contains 'v2') { $v2Path = Get-V2Exe }
    if ($v1Path) { Log "v1 player: $(Split-Path -Leaf $v1Path)" }
    if ($v2Path) { Log "v2 player: $(Split-Path -Leaf $v2Path)" }

    $ordered = @($script:Files | Where-Object { $passages.ContainsKey($_.Index) } | Sort-Object { $_.Length })
    foreach ($file in $ordered) {
        foreach ($group in $groups) {
            $groupPaths = @($active | Where-Object { $PathDefs[$_].Group -eq $group })
            Log ("---- user file {0}, process group '{1}' (paths: {2})" -f $file.Index, $group, ($groupPaths -join ', '))
            $pls = @{}
            $gateTested = $false
            try {
                foreach ($prog in $Programs) {
                    $exe = if ($prog -eq 'v1') { $v1Path } else { $v2Path }
                    if ($group -eq 'wrp') { $exe = Prepare-Wrp $prog $exe; if ($script:WrpNote) { Log "NOTE: $($script:WrpNote)" } }
                    $pls[$prog] = Start-Player $prog $exe $group $file
                }
                foreach ($prog in $Programs) {
                    $pl = $pls[$prog]
                    $ok = $false
                    try { $ok = Load-File $pl $file } catch { Log "$prog load problem: $($_.Exception.Message)" }
                    Check "$prog : user file $($file.Index) loaded" $ok 'the file did not load'
                }
                foreach ($pathName in $groupPaths) {
                    $def = $PathDefs[$pathName]
                    $script:CurMode = if ($DryRun) { 'none' } else { $def.Mode }
                    $script:CurDirect = [bool]$def.Direct
                    Log ("== path {0} ({1}); output requirement for the players: {2}" -f $pathName, $def.Label, $(if ($DryRun) { 'None (dry run, labelled only)' } else { $def.Mode }))
                    if ($DryRun -and -not $gateTested) {
                        # gate self-test, part 1 (never presses Play): the players start on the first real device, so the gate must refuse now
                        foreach ($prog in $Programs) {
                            $pl = $pls[$prog]; if (-not $pl.Loaded) { continue }
                            $first = (Read-Output $pl).Item
                            if ($first -eq 'None') { Log "NOTE: $prog starts with output None here: the start-up refusal could not be tested"; continue }
                            $refused = $false; try { Assert-OutputGate $pl } catch { $refused = $true }
                            Check "$prog : gate refuses while the start-up device '$first' is still selected" $refused 'the gate accepted a real device'
                        }
                    }
                    $pathProgs = @($Programs | Where-Object { -not ($def.V2Only -and $_ -eq 'v1') })
                    if ($def.V2Only -and $Programs -contains 'v1') { Log "NOTE: path $pathName is v2 only (v1 has no such option): v1 skips it" }
                    foreach ($prog in $pathProgs) { if ($pls[$prog].Loaded) { Set-PathOutput $pls[$prog] $script:CurMode } }
                    foreach ($prog in $pathProgs) { if ($pls[$prog].Loaded) { try { Assert-OutputGate $pls[$prog]; Pass "$prog : output verified for $pathName" } catch { Fail "$prog : output verified for $pathName" $_.Exception.Message; $pls[$prog].Loaded = $false } } }
                    if ($DryRun -and -not $gateTested) {
                        # part 2: a real-output requirement is refused in a dry run; a ticked KDMAPI box is refused while None is required (v2 only)
                        $gateTested = $true
                        foreach ($prog in $Programs) {
                            $pl = $pls[$prog]; if (-not $pl.Loaded) { continue }
                            $script:CurMode = 'kdmapi'; $refusedReal = $false; try { Assert-OutputGate $pl } catch { $refusedReal = $true }
                            $script:CurMode = 'none'
                            Check "$prog : gate refuses a real-output requirement in a dry run" $refusedReal 'the gate accepted'
                            if ($prog -eq 'v2') {
                                $k = Ctl $pl 'Kdmapi'; [VisualDriver]::Click($k); Wait-Ms 500
                                $refusedKd = $false; try { Assert-OutputGate $pl } catch { $refusedKd = $true }
                                if ([VisualDriver]::CheckState($k) -eq 1) { [VisualDriver]::Click($k); Wait-Ms 500 }
                                Check "$prog : gate refuses while KDMAPI is ticked and None is required" $refusedKd 'the gate accepted a ticked KDMAPI box'
                                Check "$prog : KDMAPI box cleared again" ([VisualDriver]::CheckState($k) -eq 0) 'could not clear the KDMAPI box'
                                try { Assert-OutputGate $pl; Pass "$prog : gate accepts again after clearing" } catch { Fail "$prog : gate accepts again after clearing" $_.Exception.Message }
                            }
                        }
                    }
                    $pIndex = 0
                    foreach ($pass in (Expand-Modes $passages[$file.Index])) {
                        $pIndex++
                        $planList = if ($PowerPlans.Count -gt 0) { $PowerPlans } else { @('') }
                        foreach ($plan in $planList) {
                            $script:PlanTag = ''
                            if ($plan) { $script:PlanTag = Switch-Plan $plan; if (-not $script:PlanTag) { break } }
                            foreach ($prog in $pathProgs) {
                                if ($pls[$prog].Loaded) { Play-Passage $pls[$prog] $file $pIndex $pass $pathName }
                            }
                        }
                        if ($PowerPlans.Count -gt 0) { & powercfg /setactive $BalancedPlan | Out-Null; Log "POWER PLAN: back to Balanced after this passage" }
                    }
                }
            }
            finally { Close-All; Wait-Ms 1500 }
        }
    }
}
catch {
    Fail 'run' ("exception: " + $_.Exception.Message)
}
finally {
    Close-All
    if ($PowerPlans.Count -gt 0) { & powercfg /setactive $BalancedPlan | Out-Null; Log "POWER PLAN: run ended; active plan: $((& powercfg /getactivescheme) -join ' ')" }
    if (Test-Path -LiteralPath $wrpRoot) { Remove-Item -LiteralPath $wrpRoot -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $ocrPng -Force -ErrorAction SilentlyContinue
    $bad = Test-FilesUnchanged
    if ($bad.Count -gt 0) { Fail 'user files' ("user file(s) {0} changed size or modified time" -f ($bad -join ',')) } else { Pass 'user files: size and modified time unchanged' }
    if (Test-Path -LiteralPath $wrpRoot) { Fail 'cleanup' 'the private winmm-wrp folder is still there' } else { Pass 'private winmm-wrp folder removed (or never created)' }
    if ($GoFile -and (Test-Path -LiteralPath $GoFile)) { Remove-Item -LiteralPath $GoFile -Force -ErrorAction SilentlyContinue }
}
Log ("results: {0}" -f $csvPath)
if ($script:Fails.Count -gt 0) {
    $word = if ($DryRun) { 'DRYRUN FAIL' } else { 'FAIL' }
    Write-Host ("WATCH-PASSAGES: {0} ({1} problems, {2} steps)" -f $word, $script:Fails.Count, $script:Steps)
    foreach ($f in $script:Fails) { Write-Host (Scrub "  - $f") }
    exit 1
}
if ($DryRun) { Write-Host ("WATCH-PASSAGES: DRYRUN PASS ({0} steps)" -f $script:Steps) }
else { Write-Host ("WATCH-PASSAGES: DONE ({0} plays, {1} steps)" -f $script:Plays, $script:Steps) }
exit 0
