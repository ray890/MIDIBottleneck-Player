# Part of visual-gate.ps1 -Visible (dot-sourced by gate-run.ps1 after the other captures): the RESIZE checks of
# prompt 009. Uses the helpers of visual-gate.ps1 (Start-V2, Start-V1, V1-Main, V1-Load, Log, Want, ...).
#
# For every frame style (native, classic strip, integrated header, auto-hide overlay) at every application scale, v2
# and then v1 (with a file loaded) are resized the way a person drags a border:
#   shrink        slowly from the standard width to the minimum, across the compact-layout boundary
#   grow          slowly back and beyond the standard width
#   fast-shrink   a fast drag (a new size every 15 ms) to the minimum, checked during the drag and after it
#   fast-grow     the same back to the standard width
#   left-grow / left-shrink   fast drags of the LEFT border (the window moves while it is resized)
#   final         back to the standard width (this capture goes to the gallery)
# The drags are framed by WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE as in Windows' own move/size loop. After each step the
# window is captured as the resize left the screen; then it is made to repaint completely and captured again; the
# client areas must be identical. Any differing v2 pixel (more than -ResizeTolerance) fails the gate; the pictures of a
# difference (magenta = differing pixels) are kept in <OutDir>\resize\.

$rsDir = Join-Path $OutDir 'resize'
New-Item -ItemType Directory -Force $rsDir | Out-Null
$rsScales = if ($Quick) { @(100, 200) } else { @(50, 75, 100, 125, 150, 200) }
$rsStyles = @(@('native', -1, 0), @('classic', 0, $Cmd.Classic), @('integrated', 1, $Cmd.Integrated), @('overlay', 2, $Cmd.Overlay))
$rsTol = [int]$cfg.ResizeTolerance
$rsSettle = 200
$script:RsCount = @{}

function Rs-Dev([int]$canonical, [int]$s) { [int][math]::Round($canonical * $s / 100.0 * $script:PlayerDpi / 96.0) }

# One check at the current size (logged; counted per program and key).
function Rs-Check([string]$prog, [string]$key, [string]$step, [IntPtr]$h, [bool]$keep) {
    $base = Join-Path $rsDir ("{0}-{1}-{2}" -f $prog, $key, $step)
    $res = [VisualDriver]::ResizeCheck($h, $rsSettle, "$base-asis.png", "$base-fresh.png", "$base-diff.png", $keep)
    $f = $res -split '\|'
    $n = [int]$f[0]
    $bad = ($n -lt 0) -or ($n -gt $rsTol)
    $status = if (-not $bad) { 'PASS' } elseif ($prog -eq 'v2') { 'FAIL' } else { 'INFO' }
    if ($prog -eq 'v2' -and $bad) { $script:V2Fails++ }
    $c = $script:RsCount["$prog|$key"]
    if (-not $c) { $c = [pscustomobject]@{ Steps = 0; Bad = 0; Max = 0; FirstDiff = ''; Final = '' }; $script:RsCount["$prog|$key"] = $c }
    $c.Steps++
    if ($bad) { $c.Bad++; if ($n -gt $c.Max) { $c.Max = $n }; if (-not $c.FirstDiff -and (Test-Path "$base-diff.png")) { $c.FirstDiff = "$base-diff.png" } }
    if ($keep -and (Test-Path "$base-asis.png")) { $c.Final = "$base-asis.png" }
    $line = "{0} {1} {2} {3}: client {4}, {5} stale px{6} (painted: {7})" -f $status.PadRight(4), $prog, $key, $step, $f[2], $n,
        $(if ($n -gt 0) { " in $($f[1]); see $base-diff.png" } else { '' }), $f[3]
    Log $line
}

# Resizes to each client width in turn ($fast: 15 ms apart, checked only at the end), inside one size loop.
function Rs-Sweep([string]$prog, [string]$key, [string]$step, [IntPtr]$h, [int[]]$widths, [bool]$fast, [bool]$left) {
    [VisualDriver]::SizeLoop($h, $true)
    $i = 0
    foreach ($w in $widths) {
        $i++
        [VisualDriver]::SetClientWidth($h, $w, $left)
        if ($fast) { [Threading.Thread]::Sleep(15) }
        else { Rs-Check $prog $key ("{0}{1}-{2}" -f $step, $i, $w) $h $false }
    }
    if ($fast) { Rs-Check $prog $key "$step-during" $h $false }
    [VisualDriver]::SizeLoop($h, $false)
    Rs-Check $prog $key "$step-after" $h $false   # after the drag ended (a program may lay out again then)
}

function Rs-Steps([int]$from, [int]$to, [int]$count) {
    $list = New-Object System.Collections.Generic.List[int]
    for ($k = 1; $k -le $count; $k++) { $list.Add([int][math]::Round($from + ($to - $from) * $k / $count)) }
    return , $list.ToArray()
}

# The whole sequence for one window (already showing the file at the scale and frame style to test).
function Rs-Run([string]$prog, [string]$key, [int]$s, [scriptblock]$getH) {
    $h = & $getH
    $m = $script:Mon
    [VisualDriver]::PlaceNoActivate($h, $m.X + 20, $m.Y + 8)
    $ci = [VisualDriver]::ClientInWindow($h); $wr = [VisualDriver]::Rect($h)
    $nc = ($wr[2] - $wr[0]) - ($ci[2] - $ci[0])
    $minW = Rs-Dev 416 $s
    $maxW = [Math]::Min((Rs-Dev 1100 $s), $m.W - 40 - $nc)
    $std = [Math]::Min((Rs-Dev 790 $s), $maxW)
    $clamp = { param($w) [Math]::Max($minW, [Math]::Min($maxW, $w)) }
    [VisualDriver]::SetClientWidth($h, $std, $false)
    Rs-Check $prog $key 'start' $h $false
    $shrink = @(700, 640, 600, 560, 480, 416 | ForEach-Object { & $clamp (Rs-Dev $_ $s) })
    $grow = @(480, 560, 639, 641, 700, 790, 950, 1100 | ForEach-Object { & $clamp (Rs-Dev $_ $s) })
    Rs-Sweep $prog $key 'shrink' $h $shrink $false $false
    Rs-Sweep $prog $key 'grow' $h $grow $false $false
    $cur = ([VisualDriver]::ClientSize($h))[0]
    Rs-Sweep $prog $key 'fast-shrink' $h (Rs-Steps $cur $minW 24) $true $false
    Rs-Sweep $prog $key 'fast-grow' $h (Rs-Steps $minW $std 24) $true $false
    # the left border: put the window at the right side of the monitor first, so the growing window stays on it
    $wr = [VisualDriver]::Rect($h)
    [VisualDriver]::PlaceNoActivate($h, $m.X + $m.W - ($wr[2] - $wr[0]) - 20, $m.Y + 8)
    $h = & $getH
    $wide = & $clamp (Rs-Dev 1000 $s)
    Rs-Sweep $prog $key 'left-grow' $h (Rs-Steps $std $wide 12) $true $true
    Rs-Sweep $prog $key 'left-shrink' $h (Rs-Steps $wide (& $clamp (Rs-Dev 560 $s)) 12) $true $true
    [VisualDriver]::PlaceNoActivate($h, $m.X + 20, $m.Y + 8)
    [VisualDriver]::SizeLoop($h, $true)
    [VisualDriver]::SetClientWidth($h, $std, $false)
    [VisualDriver]::SizeLoop($h, $false)
    Rs-Check $prog $key 'final' $h $true
}

function Rs-Register([string]$prog, [string]$key, [string]$caption) {
    $c = $script:RsCount["$prog|$key"]
    if (-not $c) { return }
    $png = if ($c.FirstDiff) { $c.FirstDiff } else { $c.Final }
    if (-not $png) { return }
    if (-not $script:ResizeKeys.Contains($key)) { [void]$script:ResizeKeys.Add($key) }
    $status = if ($c.Bad -eq 0) { 'PASS' } elseif ($prog -eq 'v2') { 'FAIL' } else { 'INFO' }
    [void]$script:Caps.Add([pscustomobject]@{ Prog = $prog; Key = $key; Caption = $caption; Png = $png; Status = $status; Idle = 'resize'; Findings = $c.Bad })
    Log ("{0} {1} {2} summary: {3} checks, {4} with stale pixels (largest {5} px)" -f $status.PadRight(4), $prog, $key, $c.Steps, $c.Bad, $c.Max)
}

Log '--- resize checks (real screen pixels, as the resize left them, against a complete repaint)'
# ---------------------------------------------------------------- v2: one program per frame style and scale
foreach ($s in $rsScales) {
    foreach ($st in $rsStyles) {
        $key = "resize-$($st[0])-$s"
        if (-not (Want $key)) { continue }
        $caption = "Resized, $($st[0]) frame, scale $s% (left: v2, right: v1; a diff picture is shown when a step failed)"
        try {
            $a = "--scale $s --open `"$dense`""
            if ($st[1] -ge 0) { $a += " --borderless $($st[1])" }
            $p = Start-V2 $a
            try {
                Wait-V2Loaded $p
                Pause-Ms 600
                Rs-Run 'v2' $key $s { $p.H }
            } finally { Stop-Pid $p.Pid }
        } catch { Fail-State 'v2' $key $_.Exception.Message }
        Rs-Register 'v2' $key $caption
    }
}

# ---------------------------------------------------------------- v1: one instance, driven through scales and styles
$rsV1Keys = @(foreach ($s in $rsScales) { foreach ($st in $rsStyles) { "resize-$($st[0])-$s" } }) | Where-Object { Want $_ }
if (-not $SkipV1 -and -not $V1From -and @($rsV1Keys).Count -gt 0) {
    try {
        [void](Start-V1)
        Pause-Ms 3000
        Select-OutputNone 'v1' (V1-Main)
        V1-Load $dense
        $script:V1Borderless = $false
        foreach ($s in $rsScales) {
            [VisualDriver]::SysCommand((V1-Main), $Cmd["S$s"]); Pause-Ms 1500
            foreach ($st in $rsStyles) {
                $key = "resize-$($st[0])-$s"
                if (-not (Want $key)) { continue }
                try {
                    if ($st[1] -lt 0) {
                        if ($script:V1Borderless) { [VisualDriver]::SysCommand((V1-Main), $Cmd.Borderless); Pause-Ms 2000; $script:V1Borderless = $false }
                    }
                    else {
                        [VisualDriver]::SysCommand((V1-Main), $st[2]); Pause-Ms 1200
                        if (-not $script:V1Borderless) { [VisualDriver]::SysCommand((V1-Main), $Cmd.Borderless); Pause-Ms 2000; $script:V1Borderless = $true }
                    }
                    Rs-Run 'v1' $key $s { V1-Main }
                } catch { Fail-State 'v1' $key $_.Exception.Message }
                Rs-Register 'v1' $key ''
            }
        }
    } catch { Fail-State 'v1' 'resize' $_.Exception.Message }
}
