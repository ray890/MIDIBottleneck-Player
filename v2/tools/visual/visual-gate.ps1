# Visual gate for the v2 Win32 player: captures what each window really shows, in every main state, next to v1,
# checks the pixels of every control, and writes labelled contact sheets.
#
#   .\v2\tools\visual\visual-gate.ps1                       # x64 build, v2 and v1, gallery in v2\out\visual\gallery\
#   .\v2\tools\visual\visual-gate.ps1 -Build x86 -SkipV1    # the x86 build (v1 is the same program: reuse -V1From)
#   .\v2\tools\visual\visual-gate.ps1 -Quick -SkipV1        # a handful of states (about 1 minute)
#   .\v2\tools\visual\visual-gate.ps1 -V2Exe other.exe      # judge another build (used for the mutation proof)
#   .\v2\tools\visual\visual-gate.ps1 -Visible              # real screen pixels on "Internal Display" + the resize checks
#   .\v2\tools\visual\visual-gate.ps1 -Visible -Only 'resize-*' -V2Exe old.exe -SkipV1   # resize checks only (gate proof)
# Ends with "VISUAL-GATE: PASS" or "VISUAL-GATE: FAIL" (exit code 0 / 1). See v2\BUILDING.md, "Visual gate".
#
# -Visible (prompt 009): nothing runs on the hidden desktop. Every program is started hidden and then shown WITHOUT
# activation, top-most, on the monitor named by -Monitor (default "Internal Display": the 1920x1080 monitor that is not
# the LG one; never "LG HDR 4K"), and every capture is the real screen pixels of the window, taken WITHOUT forcing a
# repaint (so pixels a program leaves stale are captured). A window that appears elsewhere is moved there at once. If
# that monitor's scale differs from the one the players lay themselves out for, Windows stretches their windows and
# no capture can be pixel-faithful: every capture is then marked failed with that reason. Only in this mode the gate
# also runs the RESIZE checks (resize-run.ps1): each frame style at each scale, v2 and v1, is resized step by step
# (shrinking and growing, slowly and as a fast drag, by the right and the left edge, across the compact-layout
# boundary); after every step the screen is captured as the resize left it, then the window is made to repaint
# completely and captured again: a window that repaints correctly while it is resized shows identical client pixels
# both times. Any differing pixel in v2 fails the gate (-ResizeTolerance); v1's counts are reported for comparison.
#
# How it captures: everything runs on a private hidden Win32 desktop (nothing reaches any monitor, no focus is
# taken). The script starts a copy of itself on that desktop (-Inner); the copy starts v2 and v1 there, drives
# them with window messages only (the same ones for both programs) and captures each window with
# PrintWindow(PW_RENDERFULLCONTENT). A capture is taken only when two captures 500 ms apart are identical (the
# program is idle and fully painted); a timeout is recorded as a note. Moving content (playing, loading) is
# captured "live" after a short settle time.
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')][string]$Build = 'x64',
    [string]$OutDir,                  # captures, trees and results (default v2\out\visual\<build>)
    [string]$Gallery,                 # contact sheets (default v2\out\visual\gallery)
    [string]$V2Exe,                   # default v2\out\build-<build>\Release\midibn-win32.exe
    [string]$V1Exe,                   # default release-package\MIDIBottleneck-Player-v1.0.44-x64.exe
    [string]$LargeFixture,            # default v2\harness\fixtures\large\large-8m.mid (searched in the main checkout too)
    [switch]$SkipV1,                  # do not run v1
    [string]$V1From,                  # reuse the v1 captures of an earlier run (its OutDir) instead of running v1
    [switch]$Quick,                   # a few states only
    [string[]]$Only,                  # wildcard patterns on state keys (e.g. 'scale-*', 'win-*')
    [int]$TimeoutMinutes = 40,
    [switch]$Visible,                 # real screen pixels on -Monitor instead of the hidden desktop; adds the resize checks
    [string]$Monitor = 'Internal Display',
    [switch]$NoResize,                # -Visible without the resize checks
    [int]$ResizeTolerance = 0,        # differing client pixels a v2 resize step may show
    [string]$Tag,                     # gallery file tag (default: the build name)
    [switch]$Inner, [string]$ConfigFile
)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$here = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))

# ======================================================================================================== outer
if (-not $Inner) {
    if (-not $OutDir) { $OutDir = Join-Path $repo "v2\out\visual\$Build" }
    if (-not $Gallery) { $Gallery = Join-Path $repo 'v2\out\visual\gallery' }
    if (-not $V2Exe) { $V2Exe = Join-Path $repo "v2\out\build-$Build\Release\midibn-win32.exe" }
    if (-not $V1Exe) {
        $V1Exe = Join-Path $repo 'release-package\MIDIBottleneck-Player-v1.0.44-x64.exe'
        if (-not (Test-Path $V1Exe)) {   # a worktree has no release-package: use the main checkout's
            $common = (& git -C $repo rev-parse --git-common-dir) -replace '/', '\'
            $main = Split-Path -Parent ([IO.Path]::GetFullPath([IO.Path]::Combine($repo, $common)))
            $V1Exe = Join-Path $main 'release-package\MIDIBottleneck-Player-v1.0.44-x64.exe'
        }
    }
    if (-not $LargeFixture) {
        $LargeFixture = Join-Path $repo 'v2\harness\fixtures\large\large-8m.mid'
        if (-not (Test-Path $LargeFixture)) {
            $common = (& git -C $repo rev-parse --git-common-dir) -replace '/', '\'
            $main = Split-Path -Parent ([IO.Path]::GetFullPath([IO.Path]::Combine($repo, $common)))
            $LargeFixture = Join-Path $main 'v2\harness\fixtures\large\large-8m.mid'
        }
    }
    if (-not (Test-Path $V2Exe)) { throw "v2 executable not found: $V2Exe (build it with v2/tools/build-player.ps1)" }
    New-Item -ItemType Directory -Force $OutDir, $Gallery | Out-Null
    $OutDir = (Resolve-Path $OutDir).Path; $Gallery = (Resolve-Path $Gallery).Path
    Get-ChildItem $OutDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
    $cfg = @{ Build = $Build; OutDir = $OutDir; Gallery = $Gallery; V2Exe = (Resolve-Path $V2Exe).Path; V1Exe = $V1Exe; LargeFixture = $LargeFixture
              SkipV1 = [bool]$SkipV1; V1From = $V1From; Quick = [bool]$Quick; Only = @($Only); Repo = $repo
              Visible = [bool]$Visible; Monitor = $Monitor; NoResize = [bool]$NoResize; ResizeTolerance = $ResizeTolerance
              Tag = $(if ($Tag) { $Tag } else { $Build }) }
    $cfgFile = Join-Path $OutDir 'config.json'
    $cfg | ConvertTo-Json | Set-Content -Encoding UTF8 $cfgFile
    $log = Join-Path $OutDir 'gate.log'
    if ($Visible) {
        # prompt 009: on the real desktop (no hidden desktop); the inner run places every window on -Monitor
        Write-Host "visual-gate: VISIBLE run on '$Monitor' (windows are shown there without activation); log $log"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path -Inner -ConfigFile $cfgFile 2>&1 | Tee-Object -FilePath $log
        $resultFile = Join-Path $OutDir 'result.txt'
        if (Test-Path $resultFile -PathType Leaf) { $final = (Get-Content $resultFile -TotalCount 1) } else { $final = 'VISUAL-GATE: FAIL (the capture run did not finish)'; Write-Host $final }
        if ($final -match 'PASS') { exit 0 } else { exit 1 }
    }
    # everything runs on a private hidden desktop (see run-hidden.ps1); the output is streamed here
    $self = $MyInvocation.MyCommand.Path
    & (Join-Path $here 'run-hidden.ps1') -File $self -Arguments "-Inner -ConfigFile '$cfgFile'" -LogFile $log -WorkingDirectory $repo -TimeoutMinutes $TimeoutMinutes
    $resultFile = Join-Path $OutDir 'result.txt'
    if (Test-Path $resultFile -PathType Leaf) { $final = (Get-Content $resultFile -TotalCount 1) } else { $final = 'VISUAL-GATE: FAIL (the capture run did not finish)'; Write-Host $final }
    if ($final -match 'PASS') { exit 0 } else { exit 1 }
}

# ======================================================================================================== inner
$cfg = Get-Content -Raw $ConfigFile | ConvertFrom-Json
$OutDir = $cfg.OutDir; $Gallery = $cfg.Gallery; $V2Exe = $cfg.V2Exe; $V1Exe = $cfg.V1Exe; $Large = $cfg.LargeFixture
$Fix = Join-Path $cfg.Repo 'v2\harness\fixtures'
$Quick = [bool]$cfg.Quick; $SkipV1 = [bool]$cfg.SkipV1; $V1From = $cfg.V1From
$OnlyPatterns = @($cfg.Only | Where-Object { $_ })
Add-Type -Path (Join-Path $here 'VisualCapture.cs'), (Join-Path $here 'VisualDriver.cs'), (Join-Path $here 'PixelCheck.cs'), (Join-Path $here 'ContactSheet.cs') -ReferencedAssemblies System.Drawing
[VisualDriver]::MakeDpiAware()
$Visible = [bool]$cfg.Visible
$script:Mon = $null
$script:Unfaithful = ''
$script:PlayerDpi = 96
if ($Visible) {
    [VisualDriver]::ScreenMode = $true
    $script:PlayerDpi = [VisualDriver]::SystemDpi()
    $mons = @([VisualDriver]::Monitors() | ForEach-Object {
        $f = $_ -split '\|'; $g = $f[1] -split ' '; $wa = $f[5] -split ' '
        [pscustomobject]@{ Dev = $f[0]; X = [int]$g[0]; Y = [int]$g[1]; W = [int]$g[2]; H = [int]$g[3]; Primary = $f[2]; Text = $f[3]; Dpi = [int]$f[4]
                           WL = [int]$wa[0]; WT = [int]$wa[1]; WR = [int]$wa[2]; WB = [int]$wa[3] } })
    foreach ($m in $mons) { Write-Output ("monitor: {0} at {1},{2} {3}x{4} {5} '{6}' dpi {7}, work area {8},{9}-{10},{11}" -f $m.Dev, $m.X, $m.Y, $m.W, $m.H, $m.Primary, $m.Text, $m.Dpi, $m.WL, $m.WT, $m.WR, $m.WB) }
    $want = [string]$cfg.Monitor
    if ($want -eq 'Internal Display') {
        # "Internal Display" is the 1920x1080 panel; the other monitor ("LG HDR 4K") is 3840x2160 and is never used
        $script:Mon = $mons | Where-Object { $_.W -eq 1920 -and $_.H -eq 1080 -and $_.Text -notmatch 'LG' } | Select-Object -First 1
    }
    else { $script:Mon = $mons | Where-Object { $_.Dev -eq $want -or $_.Text -like "*$want*" } | Select-Object -First 1 }
    if (-not $script:Mon) {
        Write-Output "FAIL gate error: monitor '$want' not found; nothing is shown anywhere else"
        Set-Content -Path (Join-Path $cfg.OutDir 'result.txt') -Value "VISUAL-GATE: FAIL (monitor '$want' not found)" -Encoding UTF8
        exit 1
    }
    Write-Output ("captures on {0} at {1},{2} ({3}x{4}, {5} dpi); the players lay out for {6} dpi" -f $script:Mon.Dev, $script:Mon.X, $script:Mon.Y, $script:Mon.W, $script:Mon.H, $script:Mon.Dpi, $script:PlayerDpi)
    if ([VisualDriver]::ScreenIsDark($script:Mon.X, $script:Mon.Y, $script:Mon.W, $script:Mon.H)) {
        # A switched-off display reads back black. The windows are still composed there: take each window's composed
        # surface (the pixels the compositor puts on that monitor for it, stale ones included, no forced repaint).
        [VisualDriver]::SurfaceMode = $true
        Write-Output "WARNING: $($script:Mon.Dev) reads back entirely black (the display is off or not presenting): captures are taken from each window's composed surface on that monitor (PrintWindow PW_RENDERFULLCONTENT without a forced repaint) instead of the screen"
    }
    if ($script:Mon.Dpi -gt 0 -and $script:Mon.Dpi -ne $script:PlayerDpi) {
        $script:Unfaithful = "the monitor's scale ($($script:Mon.Dpi) dpi) differs from the players' ($($script:PlayerDpi) dpi): Windows stretches their windows there, so the screen pixels are not the programs' own"
        Write-Output "WARNING: $($script:Unfaithful)"
    }
}
# Shows a window on the gate's monitor (visible mode) unless it is already fully there (drop-down lists, dialogs
# centred on their owner). Never activates it.
function Ensure-OnMonitor([IntPtr]$w, [int]$dx = 20, [int]$dy = 20) {
    if (-not $Visible -or $w -eq [IntPtr]::Zero) { return }
    $r = [VisualDriver]::Rect($w); $m = $script:Mon
    if ($r[0] -ge $m.X -and $r[1] -ge $m.Y -and $r[2] -le $m.X + $m.W -and $r[3] -le $m.Y + $m.H -and [VisualDriver]::Visible($w)) { return }
    [VisualDriver]::PlaceNoActivate($w, $m.X + $dx, $m.Y + $dy)
}
$c = [System.Drawing.SystemColors]::Control
$BG = ([int]$c.R -shl 16) -bor ([int]$c.G -shl 8) -bor [int]$c.B
$StableTimeoutMs = 15000

# Explicit allow-list of controls that are deliberately empty or single coloured. Entry: check|class regex|id|text regex|reason
$AllowList = [string[]]@(
    'blank|^msctls_progress32$|*|*|empty progress bar (nothing is loading)',
    'blank|^MidiBnLabel$|*|^$|v2 label with no text (for example the load status while idle)',
    'blank|STATIC|*|^$|v1 label with no text',
    'blank|\.Window\.8\.|*|^$|v1 layout panel / spacer without content of its own',
    'bg|^MIDIBottleneckPlayerV2$|*|^160,160,160$|borderless "classic strip": the dark grey drag strip is drawn by the main window itself',
    'bg|^MIDIBottleneckAnalysisV2$|*|^160,160,160$|the Analysis window''s splitter bar between the summary and the graph (v1 SplitContainer; 6 canonical pixels, so 9 at 150 %)',
    'overlap|^msctls_updown32$|*|*|spin button sits over the right edge of its buddy edit box (UDS_ALIGNRIGHT)'
)

$script:Caps = New-Object System.Collections.ArrayList
$script:ResizeKeys = New-Object System.Collections.ArrayList   # filled by resize-run.ps1 (gallery section 08)
$script:Started = New-Object System.Collections.ArrayList
$script:V2Fails = 0
$script:Warns = 0
$script:Notes = New-Object System.Collections.ArrayList
$Sw = [Diagnostics.Stopwatch]::StartNew()

function Log([string]$s) { Write-Output $s }
function Want([string]$key) {
    if ($OnlyPatterns.Count -eq 0) { return $true }
    foreach ($p in $OnlyPatterns) { if ($key -like $p) { return $true } }
    return $false
}
function Stop-Pid([int]$procId) { try { Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue } catch { } }
function Cw([int]$s) { [int][math]::Round(560 * $s / 100) }     # compact client width at scale s
function Sw([int]$s) { [int][math]::Round(790 * $s / 100) }     # standard client width at scale s
function Pause-Ms([int]$ms) { Start-Sleep -Milliseconds $ms }

# Records and judges one capture of window $h.
function Take([string]$prog, [string]$key, [string]$caption, [IntPtr]$h, [bool]$stable = $true, [int]$settle = 1500, [string]$note = '') {
    $png = Join-Path $OutDir "$prog-$key.png"
    Ensure-OnMonitor $h
    $st = [VisualDriver]::ShotStable($h, $png, $StableTimeoutMs, $stable, $settle)
    $tree = [VisualDriver]::Tree($h, $true)
    [IO.File]::WriteAllLines((Join-Path $OutDir "$prog-$key.tree.txt"), $tree)
    $r = [PixelCheck]::Run($png, [string[]]$tree, $AllowList, $BG)
    # P10-FIX: a live (not idle-stable) capture is one forced repaint followed by one PrintWindow, and while the program keeps
    # painting that pair can race: a text label (the Channels window's explanation line at 50 %) came out blank in about 1 of
    # 12 captures under load, and a capture taken at once afterwards, with or without a forced repaint, was always complete. So
    # a live capture that shows a blank control is taken again, up to twice; a control that really draws nothing is blank
    # every time and still fails. The retry is written to the notes.
    if (-not $stable -and $r.Fails -gt 0 -and ($r.Lines | Where-Object { $_ -like '*blank*' })) {
        for ($try = 1; $try -le 2 -and $r.Fails -gt 0; $try++) {
            Start-Sleep -Milliseconds 400
            $st = [VisualDriver]::ShotStable($h, $png, $StableTimeoutMs, $false, 300)
            $tree = [VisualDriver]::Tree($h, $true)
            [IO.File]::WriteAllLines((Join-Path $OutDir "$prog-$key.tree.txt"), $tree)
            $r = [PixelCheck]::Run($png, [string[]]$tree, $AllowList, $BG)
            [void]$script:Notes.Add("$prog $key : a blank control in a live capture; captured again ($try)" + $(if ($r.Fails -eq 0) { ', complete' } else { ', still blank' }))
        }
    }
    if ($script:Unfaithful) { $r.Fails++; $r.Lines.Add("FAIL capture not faithful: " + $script:Unfaithful) }
    $size = ([VisualDriver]::Rect($h)); $w = $size[2] - $size[0]; $ht = $size[3] - $size[1]
    $status = if ($r.Fails -eq 0) { 'PASS' } else { 'FAIL' }
    if ($prog -ne 'v2' -and $r.Fails -gt 0) { $status = 'INFO' }
    if ($prog -eq 'v2' -and $r.Fails -gt 0) { $script:V2Fails += $r.Fails }
    if ($st -like 'TIMEOUT*') { $script:Warns++; [void]$script:Notes.Add("$prog $key : not idle after $StableTimeoutMs ms ($st), captured anyway") }
    Log ("{0} {1} {2} [{3}] {4}x{5} controls={6} allowed={7} findings={8}{9}" -f $status.PadRight(4), $prog, $key, $st, $w, $ht, $r.Controls, $r.Allowed, $r.Fails, $(if ($note) { "  # $note" } else { '' }))
    foreach ($l in $r.Lines) { Log ("       " + $l) }
    [void]$script:Caps.Add([pscustomobject]@{ Prog = $prog; Key = $key; Caption = $caption; Png = $png; Status = $status; Idle = $st; Findings = $r.Fails })
}

# A capture that is not judged by the control check (a popup composited onto its window; checked for being non-blank only).
function Take-Composite([string]$prog, [string]$key, [string]$caption, [IntPtr]$main, [IntPtr[]]$popups) {
    $png = Join-Path $OutDir "$prog-$key.png"
    $mr = [VisualDriver]::Rect($main)
    $xs = New-Object 'int[]' $popups.Count; $ys = New-Object 'int[]' $popups.Count
    for ($i = 0; $i -lt $popups.Count; $i++) { $pr = [VisualDriver]::Rect($popups[$i]); $xs[$i] = $pr[0] - $mr[0]; $ys[$i] = $pr[1] - $mr[1] }
    $size = [VisualDriver]::CompositeShot($main, $popups, $xs, $ys, $png)
    # judge: each popup must show more than one colour
    $bad = 0
    foreach ($p in $popups) {
        $tmp = Join-Path $OutDir "tmp-popup.png"; [void][VisualDriver]::PopupShot($p, $tmp)
        $tree = @([VisualDriver]::Tree($p, $true)[0]) ; # top only
        $rr = [PixelCheck]::Run($tmp, [string[]]$tree, [string[]]@('blank|*|*|^$|none'), $BG)
        foreach ($l in $rr.Lines) { if ($l -like 'FAIL blank*whole capture*') { $bad++ } }
    }
    $status = if ($bad -eq 0) { 'PASS' } else { 'FAIL' }
    if ($prog -ne 'v2' -and $bad -gt 0) { $status = 'INFO' }
    if ($prog -eq 'v2') { $script:V2Fails += $bad }
    Log ("{0} {1} {2} [composite {3}] popups={4} blank-popups={5}" -f $status.PadRight(4), $prog, $key, $size, $popups.Count, $bad)
    [void]$script:Caps.Add([pscustomobject]@{ Prog = $prog; Key = $key; Caption = $caption; Png = $png; Status = $status; Idle = 'composite'; Findings = $bad })
}

function Fail-State([string]$prog, [string]$key, [string]$why) {
    if ($prog -eq 'v2') { $script:V2Fails++; Log ("FAIL {0} {1} state could not be captured: {2}" -f $prog, $key, $why) }
    else { $script:Warns++; Log ("INFO {0} {1} state could not be captured: {2}" -f $prog, $key, $why) }
    [void]$script:Notes.Add("$prog $key : $why")
}

function New-Windows([int]$procId, $known) {
    $new = @()
    foreach ($w in [VisualDriver]::ProcessTopWindows($procId, $true)) { if (-not $known.Contains($w.ToInt64())) { $new += $w } }
    return $new
}
function Known-Windows([int]$procId) {
    $s = New-Object 'System.Collections.Generic.HashSet[long]'
    foreach ($w in [VisualDriver]::ProcessTopWindows($procId, $true)) { [void]$s.Add($w.ToInt64()) }
    return , $s
}
# DD-098 (visible mode): a window or dialog that a program opens while its main window is on the gate's monitor must
# open on that monitor, wholly inside its work area. Checked BEFORE the gate moves anything; v2: a finding, v1: noted.
$script:Placements = New-Object System.Collections.ArrayList
function Check-Placement([int]$procId, [IntPtr]$w) {
    if (-not $Visible -or $w -eq [IntPtr]::Zero) { return }
    $cls = [VisualDriver]::Cls($w)
    if ($cls -match '^(ComboLBox|#32768|tooltips_class32)$') { return }
    $prog = if ($script:V1 -and $procId -eq $script:V1.Pid) { 'v1' } else { 'v2' }
    $r = [VisualDriver]::Rect($w); $m = $script:Mon
    $ok = $r[0] -ge $m.WL -and $r[1] -ge $m.WT -and $r[2] -le $m.WR -and $r[3] -le $m.WB
    $line = "{0} {1} '{2}' opened at {3},{4}-{5},{6}" -f $(if ($ok) { 'PASS' } elseif ($prog -eq 'v2') { 'FAIL' } else { 'INFO' }), $prog, [VisualDriver]::Txt($w), $r[0], $r[1], $r[2], $r[3]
    $line += $(if ($ok) { ' (inside the work area of the main window''s monitor)' } else { " (NOT inside the work area $($m.WL),$($m.WT)-$($m.WR),$($m.WB) of the main window's monitor)" })
    # (not logged here: this runs inside functions whose output is their return value; the verdict lists the lines)
    [void]$script:Placements.Add($line)
    if (-not $ok -and $prog -eq 'v2') { $script:V2Fails++; [void]$script:Notes.Add("placement: $line") }
}
function Wait-NewWindow([int]$procId, $known, [int]$timeoutMs = 8000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $n = @(New-Windows $procId $known)
        if ($n.Count -gt 0) {
            # visible mode: a new window (not a drop-down list or menu) that Windows put elsewhere goes to the gate's monitor
            Check-Placement $procId $n[0]
            if ([VisualDriver]::Cls($n[0]) -notmatch '^(ComboLBox|#32768)$') { Ensure-OnMonitor $n[0] 60 60 }
            return $n[0]
        }
        Start-Sleep -Milliseconds 20
    }
    return [IntPtr]::Zero
}

# ------------------------------------------------------------------------------------------------ program adapters
# Menu command ids (the same numbers in v1 and v2).
$Cmd = @{ About = 0x1F20; Help = 0x1F90; Routing = 0x2030; Borderless = 0x2040; Classic = 0x2060; Integrated = 0x2070; Overlay = 0x2080
          S50 = 0x1FB0; S75 = 0x1FC0; S100 = 0x1FD0; S125 = 0x1FE0; S150 = 0x1FF0; S200 = 0x2000 }

# v2 control ids
$V2Id = @{ Output = 111; Kdmapi = 112; Direct = 134; Queue = 114; Overflow = 118; Rate = 120; Play = 127; Stop = 128; Analysis = 131 }

function Find-Ctl([string]$prog, [IntPtr]$h, [string]$name) {
    if ($prog -eq 'v2') {
        $id = $V2Id[$name]
        foreach ($d in [VisualDriver]::Descendants($h, $false)) { if ([VisualDriver]::CtrlId($d) -eq $id -and [VisualDriver]::Pid($d) -gt 0) { if ([VisualDriver]::Cls($d) -notmatch 'Caption') { return $d } } }
        return [IntPtr]::Zero
    }
    # v1: WinForms controls have no stable ids; find by class and text / items
    $all = [VisualDriver]::Descendants($h, $false)
    switch ($name) {
        'Play'     { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and ([VisualDriver]::Txt($d) -eq 'Play' -or [VisualDriver]::Txt($d) -eq 'Pause')) { return $d } } }
        'Stop'     { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and [VisualDriver]::Txt($d) -eq 'Stop') { return $d } } }
        'Analysis' { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and [VisualDriver]::Txt($d) -like 'Analysis*') { return $d } } }
        'Open'     { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and [VisualDriver]::Txt($d) -like 'Open MIDI*') { return $d } } }
        'Kdmapi'   { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and [VisualDriver]::Txt($d) -eq 'KDMAPI') { return $d } } }
        'Queue'    { foreach ($d in $all) { if ([VisualDriver]::Cls($d) -match 'BUTTON' -and [VisualDriver]::Txt($d) -like 'Queue*') { return $d } } }
        default {
            foreach ($d in $all) {
                if ([VisualDriver]::Cls($d) -notmatch 'COMBOBOX') { continue }
                $n = [VisualDriver]::ComboCount($d); if ($n -lt 1) { continue }
                $first = [VisualDriver]::ComboItem($d, 0)
                if ($name -eq 'Overflow' -and $first -eq 'Drop newest') { return $d }
                if ($name -eq 'Rate' -and $n -eq 8 -and $first -eq 'None') { return $d }
                if ($name -eq 'Output' -and $first -eq 'None' -and $n -ne 8) { return $d }
            }
        }
    }
    return [IntPtr]::Zero
}

function Select-OutputNone([string]$prog, [IntPtr]$h) {
    $o = Find-Ctl $prog $h 'Output'
    if ($o -eq [IntPtr]::Zero) { throw 'MIDI output combo not found' }
    # the combo is disabled while a file loads: retry for a few seconds
    for ($try = 0; $try -lt 12 -and [VisualDriver]::ComboSel($o) -ne 0; $try++) {
        if ([VisualDriver]::Enabled($o)) { [VisualDriver]::ComboSelect($o, 0) }
        Pause-Ms 400
    }
    if ([VisualDriver]::ComboSel($o) -ne 0 -or [VisualDriver]::ComboItem($o, 0) -ne 'None') { throw 'output None could not be selected' }
    $k = Find-Ctl $prog $h 'Kdmapi'
    if ($k -ne [IntPtr]::Zero -and [VisualDriver]::CheckState($k) -ne 0) { [VisualDriver]::Click($k); Pause-Ms 300 }
    if ($k -ne [IntPtr]::Zero -and [VisualDriver]::CheckState($k) -ne 0) { throw 'KDMAPI is ticked and could not be cleared' }
}

# ---------------------------------------------------------------- v2 launcher
function Start-V2([string]$extra, [bool]$selectNone = $true) {
    if ($Visible) {
        # started hidden, then shown on the gate's monitor: it never appears anywhere else
        # (--position puts the window on the gate's monitor from the start, whatever the mouse is doing; v1 below has no
        # such switch and is moved right after its window appears, while still hidden)
        $procId = [VisualCapture]::StartShow($V2Exe, "--no-settings --auto-close-ms 600000 --position $($script:Mon.X + 20),$($script:Mon.Y + 8) $extra", (Split-Path $V2Exe), 0)
        [void]$script:Started.Add($procId)
        $h = [VisualDriver]::WaitTopWindowAny($procId, 'MIDIBottleneckPlayerV2', '', 10000)
        # wait until the program has finished building its window (its thread answers again) before moving it: a size
        # set while it is still being built raced with its own layout
        if ($h -ne [IntPtr]::Zero) { [void][VisualDriver]::WaitPainted($h, 15000); [VisualDriver]::PlaceNoActivate($h, $script:Mon.X + 20, $script:Mon.Y + 8) }
        # Windows animates a window that is shown (a short fade-in): a capture taken at once (the live loading state)
        # read the half-faded window, not the program's pixels
        Pause-Ms 400
    }
    else {
        $procId = [VisualCapture]::Start($V2Exe, "--no-settings --auto-close-ms 600000 $extra", (Split-Path $V2Exe), $true)
        [void]$script:Started.Add($procId)
        $h = [VisualCapture]::WaitMainWindow($procId, '', 10000)
    }
    if ($h -eq [IntPtr]::Zero) { Stop-Pid $procId; throw "v2 main window did not appear ($extra)" }
    # output None (as for v1), so nothing can ever sound and both programs show the same device
    if ($selectNone) { try { Select-OutputNone 'v2' $h } catch { Stop-Pid $procId; throw } }
    return [pscustomobject]@{ Pid = $procId; H = $h }
}
function Wait-V2Loaded($p, [int]$timeoutMs = 60000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $b = Find-Ctl 'v2' $p.H 'Analysis'
        if ($b -ne [IntPtr]::Zero -and [VisualDriver]::Enabled($b)) { return }
        Start-Sleep -Milliseconds 100
    }
    throw 'v2 file did not finish loading'
}
function Wait-Playing([string]$prog, [IntPtr]$h, [int]$timeoutMs = 15000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $b = Find-Ctl $prog $h 'Play'
        if ($b -ne [IntPtr]::Zero -and [VisualDriver]::Txt($b) -match 'Pause') { return }
        Start-Sleep -Milliseconds 100
    }
    throw 'playback did not start'
}

# ---------------------------------------------------------------- v1 driver
$script:V1 = $null
function V1-Main {
    $p = $script:V1
    if ([VisualDriver]::Alive($p.H) -and [VisualDriver]::Visible($p.H)) { return $p.H }
    # the window handle changes when WinForms recreates the window (borderless switch)
    foreach ($w in [VisualDriver]::ProcessTopWindows($p.Pid, $true)) {
        if ([VisualDriver]::Cls($w) -like 'WindowsForms10.Window*' -and [VisualDriver]::Txt($w) -eq 'MIDIBottleneck Player') { $p.H = $w; Ensure-OnMonitor $w 20 8; return $w }
    }
    throw 'v1 main window not found'
}
# Starts v1 (one instance; visible mode: hidden first, then shown on the gate's monitor). Returns its process id.
function Start-V1 {
    if ($Visible) {
        $procId = [VisualCapture]::StartShow($V1Exe, '', (Split-Path $V1Exe), 0)
        [void]$script:Started.Add($procId)
        $h = [VisualDriver]::WaitTopWindowAny($procId, 'WindowsForms10.Window', 'MIDIBottleneck Player', 20000)
        # wait until the program has finished building its window (its thread answers again) before moving it: a size
        # set while it is still being built raced with its own layout
        if ($h -ne [IntPtr]::Zero) { [void][VisualDriver]::WaitPainted($h, 15000); [VisualDriver]::PlaceNoActivate($h, $script:Mon.X + 20, $script:Mon.Y + 8) }
    }
    else {
        $procId = [VisualCapture]::Start($V1Exe, '', (Split-Path $V1Exe), $true)
        [void]$script:Started.Add($procId)
        $h = [VisualCapture]::WaitMainWindow($procId, 'MIDIBottleneck', 20000)
    }
    if ($h -eq [IntPtr]::Zero) { throw 'v1 main window did not appear' }
    $script:V1 = [pscustomobject]@{ Pid = $procId; H = $h }
    return $procId
}
function V1-OpenFile([string]$path) {
    $h = V1-Main
    $known = Known-Windows $script:V1.Pid
    [VisualDriver]::Click((Find-Ctl 'v1' $h 'Open'))
    $dlg = Wait-NewWindow $script:V1.Pid $known 8000
    if ($dlg -eq [IntPtr]::Zero) { throw 'v1 Open dialog did not appear' }
    Pause-Ms 700
    $edit = [IntPtr]::Zero
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($edit -eq [IntPtr]::Zero -and $sw.ElapsedMilliseconds -lt 5000) { $edit = [VisualDriver]::Find($dlg, 'Edit', '', 1148); if ($edit -eq [IntPtr]::Zero) { Pause-Ms 100 } }
    if ($edit -eq [IntPtr]::Zero) { throw 'v1 Open dialog: file name box not found' }
    [VisualDriver]::SetText($edit, $path)
    Pause-Ms 200
    $ok = [VisualDriver]::Find($dlg, 'Button', '', 1)
    [VisualDriver]::Click($ok)
}
function V1-WaitLoaded([int]$timeoutMs = 180000) {
    $h = V1-Main
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $b = Find-Ctl 'v1' $h 'Analysis'
        if ($b -ne [IntPtr]::Zero -and [VisualDriver]::Enabled($b)) { Pause-Ms 400; return }
        Start-Sleep -Milliseconds 150
    }
    throw 'v1 file did not finish loading'
}
function V1-Load([string]$path) { V1-OpenFile $path; Pause-Ms 500; V1-WaitLoaded }

# ======================================================================================================== v2 captures
function Run-State([string]$prog, [string]$key, [string]$caption, [scriptblock]$body) {
    if (-not (Want $key)) { return }
    try { & $body } catch { Fail-State $prog $key $_.Exception.Message }
}

# Opens a drop-down (Overflow or Rate model) and captures it composited onto the main window.
function Dropdown-Capture([string]$prog, [int]$procId, [IntPtr]$h, [string]$key, [string]$caption, [string]$which) {
    if ($which -eq 'Overflow') {
        $q = Find-Ctl $prog $h 'Queue'
        if ($q -ne [IntPtr]::Zero -and [VisualDriver]::CheckState($q) -eq 0) { [VisualDriver]::Click($q); Pause-Ms 600 }
    }
    $combo = Find-Ctl $prog $h $which
    if ($combo -eq [IntPtr]::Zero) { throw "$which combo not found" }
    $known = Known-Windows $procId
    [VisualDriver]::ComboDrop($combo, $true)
    $pop = Wait-NewWindow $procId $known 4000
    if ($pop -eq [IntPtr]::Zero) { throw "$which drop-down did not open" }
    Pause-Ms 700
    Take-Composite $prog $key $caption $h ([IntPtr[]]@($pop))
    [VisualDriver]::ComboDrop($combo, $false)
    [VisualDriver]::Key($pop, 0x1B)
    Pause-Ms 200
}


# ======================================================================================================== captures, v1, gallery, verdict
. (Join-Path $here "gate-run.ps1")
