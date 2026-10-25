# Part of visual-gate.ps1 (dot-sourced by the inner run, on the hidden desktop): the v2 and v1 captures, the
# contact sheets and the verdict. Uses the helper functions defined in visual-gate.ps1.

$scales = if ($Quick) { @(100, 200) } else { @(50, 75, 100, 125, 150, 200) }
$dense = "$Fix\models-dense.mid"
$lanes = "$Fix\routing-lanes.mid"
$long = "$Fix\routing-patches-4096.mid"
$styles = @(@('classic', 0, $Cmd.Classic, 'Borderless: classic strip'), @('integrated', 1, $Cmd.Integrated, 'Borderless: integrated header'), @('overlay', 2, $Cmd.Overlay, 'Borderless: auto-hide overlay'))

function V2-Simple([string]$key, [string]$caption, [string]$extra, [string]$fixture = '', [bool]$stable = $true, [scriptblock]$act = $null) {
    Run-State 'v2' $key $caption {
        $a = $extra
        if ($fixture) { $a += " --open `"$fixture`"" }
        $p = Start-V2 $a
        try {
            if ($fixture) { Wait-V2Loaded $p }
            $target = $p.H
            if ($act) { $r = & $act $p; $t = @($r | Where-Object { $_ -is [IntPtr] -and $_ -ne [IntPtr]::Zero }); if ($t.Count -gt 0) { $target = $t[0] } }
            Take 'v2' $key $caption $target $stable 1500
        } finally { Stop-Pid $p.Pid }
    }
}
function V2-Dropdown([string]$key, [string]$caption, [string]$extra, [string]$which) {
    Run-State 'v2' $key $caption {
        $p = Start-V2 ($extra + " --open `"$dense`"")
        try { Wait-V2Loaded $p; Dropdown-Capture 'v2' $p.Pid $p.H $key $caption $which } finally { Stop-Pid $p.Pid }
    }
}

# The Analysis window's resolution box and the index of its "Custom..." item (v1 and v2).
function Find-CustomCombo([IntPtr]$analysis) {
    foreach ($d in [VisualDriver]::Descendants($analysis, $false)) {
        if ([VisualDriver]::Cls($d) -notmatch 'COMBOBOX') { continue }
        $n = [VisualDriver]::ComboCount($d)
        for ($i = 0; $i -lt $n; $i++) { if ([VisualDriver]::ComboItem($d, $i) -like 'Custom*') { return [pscustomobject]@{ H = $d; I = $i } } }
    }
    throw 'the Analysis resolution box was not found'
}

try {
    # ================================================================================================== v2
    Log '--- v2 captures'
    V2-Simple 'main-nofile-std' 'No file loaded, standard layout, scale 100, native frame' ''
    V2-Simple 'main-nofile-compact' 'No file loaded, compact layout (client width 560), scale 100' "--width $(Cw 100)"
    # (prompt 009, clipping review at 50%) the no-file state at 50%
    V2-Simple 'main-nofile-50-std' 'No file loaded, standard layout, scale 50, native frame' '--scale 50'
    V2-Simple 'main-nofile-50-compact' 'No file loaded, compact layout, scale 50, native frame' "--scale 50 --width $(Cw 50)"
    # the loading state and the large-file warning, at 100% and (prompt 009, clipping check) at 75% and 50%
    foreach ($lv in @(@('main-loading-std', 'dlg-largefile', ''), @('main-loading-75', 'dlg75-largefile', '--scale 75'), @('main-loading-50', 'dlg50-largefile', '--scale 50'))) {
        $lk = $lv[0]; $dk = $lv[1]; $lx = $lv[2]
        if (-not ((Want $lk) -or (Want $dk))) { continue }
        $rk = if (Want $lk) { $lk } else { $dk }
        Run-State 'v2' $rk "Loading the 8-million-event file, standard layout $lx" {
            $p = Start-V2 "$lx $(if ($cfg.Build -eq "x86") { "--force-large-warning-32" } else { "--force-large-warning" }) --open `"$Large`"" $false
            try {
                $known = Known-Windows $p.Pid; $known.Add($p.H.ToInt64()) | Out-Null
                $seen = $false; $sw = [Diagnostics.Stopwatch]::StartNew()
                while ($sw.ElapsedMilliseconds -lt 2500) {
                    if ([VisualDriver]::Find($p.H, 'msctls_progress32', '', -1) -ne [IntPtr]::Zero) { $seen = $true; break }
                    Start-Sleep -Milliseconds 5
                }
                Start-Sleep -Milliseconds 800   # P10-UI2: a forced warning on a small file appears a moment after the progress bars
                $dlg = @(New-Windows $p.Pid $known)
                if ($dlg.Count -gt 0) { Check-Placement $p.Pid $dlg[0]; Ensure-OnMonitor $dlg[0] 60 60 }
                Take 'v2' $lk 'Loading' $p.H $false 0 $(if ($seen) { 'progress bars visible' } else { 'load finished before it could be captured' })
                if ($dlg.Count -gt 0) { Take 'v2' $dk "Large-file warning dialog $lx" $dlg[0] $true 800 }
            } finally { Stop-Pid $p.Pid }
        }
    }
    V2-Simple 'main-loaded-std' 'File loaded, standard layout, scale 100, native frame' '' $dense
    V2-Simple 'main-loaded-compact' 'File loaded, compact layout (client width 560)' "--width $(Cw 100)" $dense
    foreach ($mode in 'std', 'compact') {
        $w = if ($mode -eq 'std') { '' } else { "--width $(Cw 100)" }
        V2-Simple "main-playing-$mode" "Playing on output None, $mode layout" "$w --play" $long $false { param($p) Wait-Playing 'v2' $p.H; Pause-Ms 1500 }
        $w = if ($mode -eq 'std') { '--scale 50' } else { "--scale 50 --width $(Cw 50)" }
        V2-Simple "main-playing-50-$mode" "Playing on output None, $mode layout, scale 50" "$w --play" $long $false { param($p) Wait-Playing 'v2' $p.H; Pause-Ms 1500 }
    }
    foreach ($s in $scales) {
        V2-Simple "scale-$s-std" "Application scale $s%, standard layout, file loaded" "--scale $s" $dense
        V2-Simple "scale-$s-compact" "Application scale $s%, compact layout, file loaded" "--scale $s --width $(Cw $s)" $dense
    }
    foreach ($st in $styles) {
        V2-Simple "border-$($st[0])-std" "$($st[3]), standard layout" "--borderless $($st[1])" $dense
        V2-Simple "border-$($st[0])-compact" "$($st[3]), compact layout" "--borderless $($st[1]) --width $(Cw 100)" $dense
    }
    # (prompt 009, clipping review at 50%) every frame style at 50%, both layouts (native frame: scale-50-* above)
    foreach ($st in $styles) {
        V2-Simple "border50-$($st[0])-std" "$($st[3]), standard layout, scale 50" "--borderless $($st[1]) --scale 50" $dense
        V2-Simple "border50-$($st[0])-compact" "$($st[3]), compact layout, scale 50" "--borderless $($st[1]) --scale 50 --width $(Cw 50)" $dense
    }
    # P12-CHK (prompt 012, step 2): every frame style at 75% too (before/after pictures of the Direct WinMM checkbox work)
    foreach ($st in $styles) {
        V2-Simple "border75-$($st[0])-std" "$($st[3]), standard layout, scale 75" "--borderless $($st[1]) --scale 75" $dense
        V2-Simple "border75-$($st[0])-compact" "$($st[3]), compact layout, scale 75" "--borderless $($st[1]) --scale 75 --width $(Cw 75)" $dense
    }
    # P12-CHK: the "Direct WinMM" checkbox (left of KDMAPI) in every frame style, both layouts, at 100/75/50%:
    #   off         a MIDI device is selected (selecting opens nothing), the box is enabled and not ticked
    #   on          the same, ticked
    #   grey-none   output None selected with the box ticked: greyed, tick kept (reason 1; unticked + None = every
    #               ordinary state above)
    #   grey-kdmapi KDMAPI ticked with the box ticked: greyed, tick kept (reason 2)
    # and the playback state text on the Playback heading line: Stopped (every state with a file), Paused, Playing.
    # The "stopped" and "playing" pictures are the existing main-loaded-* / main-playing-* states.
    function Direct-Act($p, [string]$mode) {
        $d = Find-Ctl 'v2' $p.H 'Direct'
        if ($d -eq [IntPtr]::Zero) { throw 'the Direct WinMM checkbox was not found' }
        $o = Find-Ctl 'v2' $p.H 'Output'
        for ($w8 = 0; $w8 -lt 20 -and $o -ne [IntPtr]::Zero -and [VisualDriver]::ComboCount($o) -lt 2; $w8++) { Pause-Ms 150 }   # the device list is filled by a worker thread (DD-057)
        $hasDevice = ($o -ne [IntPtr]::Zero -and [VisualDriver]::ComboCount($o) -ge 2)
        if ($mode -eq 'grey-own' -and $hasDevice) {   # R1: the stand-in "own-driver-late" answers 1.5 s after the start, so the box can be ticked first
            [VisualDriver]::ComboSelect($o, 1); Pause-Ms 200
            [VisualDriver]::SetCheck($d, $true); Pause-Ms 2200
            return
        }
        elseif ($hasDevice) { [VisualDriver]::ComboSelect($o, 1); Pause-Ms 400 }
        elseif ($mode -eq 'off' -or $mode -eq 'on') { [void]$script:Notes.Add("direct-$mode`: no MIDI device in the list (the box stays greyed)") }
        if ($mode -ne 'off') { [VisualDriver]::SetCheck($d, $true); Pause-Ms 300 }
        if ($mode -eq 'grey-none') { [VisualDriver]::ComboSelect($o, 0); Pause-Ms 300 }
        if ($mode -eq 'grey-kdmapi') { [VisualDriver]::SetCheck((Find-Ctl 'v2' $p.H 'Kdmapi'), $true); Pause-Ms 300 }
        Pause-Ms 400
    }
    $dstyles = @(@('native', -1), @('classic', 0), @('integrated', 1), @('overlay', 2))
    foreach ($ds in $dstyles) {
        foreach ($s in 100, 75, 50) {
            foreach ($mode in 'std', 'compact') {
                $w = "--scale $s" + $(if ($mode -eq 'compact') { " --width $(Cw $s)" } else { '' }) + $(if ($ds[1] -ge 0) { " --borderless $($ds[1])" } else { '' })
                foreach ($dm in 'off', 'on') {
                    $dk = "direct-$dm-$($ds[0])-$s-$mode"
                    $dmm = $dm
                    V2-Simple $dk "Direct WinMM checkbox $dm, $($ds[0]) frame, scale $s%, $mode layout" "$w --direct-check usable" $dense $true { param($p) Direct-Act $p $dmm }
                }
            }
        }
    }
    foreach ($ds in @(@('native', -1), @('integrated', 1))) {
        foreach ($s in 100, 50) {
            foreach ($mode in 'std', 'compact') {
                $w = "--scale $s" + $(if ($mode -eq 'compact') { " --width $(Cw $s)" } else { '' }) + $(if ($ds[1] -ge 0) { " --borderless $($ds[1])" } else { '' })
                foreach ($dm in 'grey-none', 'grey-kdmapi', 'grey-own') {
                    $dk = "direct-$dm-$($ds[0])-$s-$mode"
                    $dmm = $dm
                    $chk = if ($dm -eq 'grey-own') { 'own-driver-late' } else { 'usable' }   # R1: stand-in for the check (grey-own: a device that is Windows' own driver)
                    V2-Simple $dk "Direct WinMM checkbox greyed ($dm), $($ds[0]) frame, scale $s%, $mode layout" "$w --direct-check $chk" $dense $true { param($p) Direct-Act $p $dmm }
                }
                # Paused (Playing = main-playing-*; Stopped = every state with a file)
                $dk = "state-paused-$($ds[0])-$s-$mode"
                V2-Simple $dk "Playback state Paused, $($ds[0]) frame, scale $s%, $mode layout" "$w --play" $long $false {
                    param($p) Wait-Playing 'v2' $p.H; [VisualDriver]::Click((Find-Ctl 'v2' $p.H 'Play')); Pause-Ms 1200
                }
            }
        }
    }
    # the loading state: the playback state text stays on the Playback heading while the progress bars use the right of row 2
    foreach ($lv in @(@('direct-loading-100', ''), @('direct-loading-75', '--scale 75'), @('direct-loading-50', '--scale 50'))) {
        $lk = $lv[0]; $lx = $lv[1]
        if (-not (Want $lk)) { continue }
        Run-State 'v2' $lk "Direct WinMM checkbox and playback state while loading $lx" {
            $p = Start-V2 "$lx --open `"$Large`"" $false
            try {
                Pause-Ms 300
                Take 'v2' $lk "Direct WinMM checkbox and playback state while loading $lx" $p.H $false 0 'live capture while loading'
            } finally { Stop-Pid $p.Pid }
        }
    }
    # the windows at 100% (key win-*, dlg-*) and, for the clipping checks of prompt 009, at 75% (win75-*, dlg75-*) and
    # 50% (win50-*, dlg50-*)
    foreach ($wv in @(@('', ''), @('75', '--scale 75'), @('50', '--scale 50'), @('150', '--scale 150'))) {
    $ws = $wv[0]; $wx = $wv[1]
    V2-Simple "win$ws-analysis" "Analysis window with results (routing-lanes.mid, 12 tracks) $wx" $wx $lanes $true {
        param($p)
        $known = Known-Windows $p.Pid
        [VisualDriver]::Click((Find-Ctl 'v2' $p.H 'Analysis'))
        $w = Wait-NewWindow $p.Pid $known 8000
        if ($w -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
        Pause-Ms 2000
        $w
    }
    # (P11-HELP) F1 inside the Analysis window opens Help on the Analysis topic
    V2-Simple "dlg$ws-help-f1-analysis" "Help opened with F1 inside the Analysis window $wx" $wx $lanes $true {
        param($p)
        $known = Known-Windows $p.Pid
        [VisualDriver]::Click((Find-Ctl 'v2' $p.H 'Analysis'))
        $a = Wait-NewWindow $p.Pid $known 8000
        if ($a -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
        Pause-Ms 1500
        $known = Known-Windows $p.Pid
        [VisualDriver]::Key($a, 0x70)
        $w = Wait-NewWindow $p.Pid $known 8000
        if ($w -eq [IntPtr]::Zero) { throw ('Help did not open with F1 in Analysis (window ' + [VisualDriver]::Cls($a) + ' / ' + [VisualDriver]::Txt($a) + ')') }
        $w
    }
    V2-Simple "dlg$ws-help-f1-channels" "Help opened with F1 inside the Channels window $wx" $wx $lanes $true {
        param($p)
        $known = Known-Windows $p.Pid
        [VisualDriver]::Click((Find-Ctl 'v2' $p.H 'Analysis'))
        $a = Wait-NewWindow $p.Pid $known 8000
        if ($a -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
        Pause-Ms 1500
        $known = Known-Windows $p.Pid
        [VisualDriver]::Click([VisualDriver]::WaitFind($a, 'Button', 'Channels', -1, 5000))
        $c = Wait-NewWindow $p.Pid $known 8000
        if ($c -eq [IntPtr]::Zero) { throw 'Channels window did not open' }
        Pause-Ms 1500
        $known = Known-Windows $p.Pid
        [VisualDriver]::Key($c, 0x70)
        $w = Wait-NewWindow $p.Pid $known 8000
        if ($w -eq [IntPtr]::Zero) { throw 'Help did not open with F1 in Channels' }
        $w
    }
    V2-Simple "dlg$ws-help-f1-routing" "Help opened with F1 inside the Track routing editor $wx" $wx $lanes $true {
        param($p)
        $known = Known-Windows $p.Pid
        [VisualDriver]::SysCommand($p.H, $Cmd.Routing)
        $r = Wait-NewWindow $p.Pid $known 8000
        if ($r -eq [IntPtr]::Zero) { throw 'Track routing editor did not open' }
        Pause-Ms 2000
        $known = Known-Windows $p.Pid
        [VisualDriver]::Key($r, 0x70)
        $w = Wait-NewWindow $p.Pid $known 8000
        if ($w -eq [IntPtr]::Zero) { throw 'Help did not open with F1 in Track routing' }
        $w
    }
    V2-Simple "win$ws-channels" "Channels window while playing (routing-patches-4096.mid) $wx" "$wx --play" $long $false {
        param($p)
        Wait-Playing 'v2' $p.H
        $known = Known-Windows $p.Pid
        [VisualDriver]::Click((Find-Ctl 'v2' $p.H 'Analysis'))
        $a = Wait-NewWindow $p.Pid $known 8000
        if ($a -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
        Pause-Ms 1500
        $known = Known-Windows $p.Pid
        $btn = [VisualDriver]::WaitFind($a, 'Button', 'Channels', -1, 5000)
        [VisualDriver]::Click($btn)
        $c = Wait-NewWindow $p.Pid $known 8000
        if ($c -eq [IntPtr]::Zero) { throw 'Channels window did not open' }
        Pause-Ms 2000
        $c
    }
    V2-Simple "win$ws-routing" "Track routing editor with a plan (routing-lanes.mid) $wx" $wx $lanes $true {
        param($p)
        $known = Known-Windows $p.Pid
        [VisualDriver]::SysCommand($p.H, $Cmd.Routing)
        $w = Wait-NewWindow $p.Pid $known 8000
        if ($w -eq [IntPtr]::Zero) { throw 'Track routing editor did not open' }
        Pause-Ms 2500
        $w
    }
    foreach ($d in @(@("dlg$ws-about", 'About', $Cmd.About), @("dlg$ws-help", 'Help', $Cmd.Help))) {
        $dk = $d[0]; $dn = $d[1]; $dc = $d[2]
        V2-Simple $dk "$dn dialog $wx" $wx '' $true {
            param($p)
            $known = Known-Windows $p.Pid
            [VisualDriver]::SysCommand($p.H, $dc)
            $w = Wait-NewWindow $p.Pid $known 8000
            if ($w -eq [IntPtr]::Zero) { throw "$dn window did not open" }
            $w
        }
    }
    # (P11-HELP) F1 in the main window opens Help on the topic of the focused control: the Rate model drop-down is
    # focused first (through the program's own input queue), so Help must open on "Rate model", not the Overview
    if (-not ('P11Focus' -as [type])) {
        Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class P11Focus {
            [DllImport("user32.dll")] static extern bool AttachThreadInput(int a, int b, bool attach);
            [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr h);
            [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
            [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();
            public static void Focus(IntPtr h) { int pid; int t = GetWindowThreadProcessId(h, out pid); int me = GetCurrentThreadId();
                AttachThreadInput(me, t, true); SetFocus(h); AttachThreadInput(me, t, false); } }'
    }
    V2-Simple "dlg$ws-help-f1" "Help opened with F1 from the main window, Rate model focused $wx" $wx '' $true {
        param($p)
        $known = Known-Windows $p.Pid
        $rate = Find-Ctl 'v2' $p.H 'Rate'
        [P11Focus]::Focus($rate)
        Pause-Ms 300
        [VisualDriver]::Key($rate, 0x70)
        $w = Wait-NewWindow $p.Pid $known 8000
        if ($w -eq [IntPtr]::Zero) { throw 'Help did not open with F1' }
        $w
    }
    # the two prompts: Effective playback speed window (right-click menu on Effective speed > Custom...; the menu is
    # opened with the message the statistics view sends, then Up selects its last item, Custom..., and Enter chooses it)
    # and Custom Analysis resolution (the Analysis window's resolution box set to Custom...)
    V2-Simple "dlg$ws-speedwindow" "Effective playback speed window prompt $wx" $wx '' $true {
        param($p)
        $known = Known-Windows $p.Pid
        $r = [VisualDriver]::Rect($p.H)
        [VisualDriver]::PostPoint($p.H, 0x800C, $r[0] + 200, $r[1] + 150)   # WM_MIDIBN_SPEEDMENU (WM_APP + 12)
        $menu = Wait-NewWindow $p.Pid $known 5000
        if ($menu -eq [IntPtr]::Zero -or [VisualDriver]::Cls($menu) -ne '#32768') { throw 'the Effective speed menu did not open' }
        Pause-Ms 300
        $known = Known-Windows $p.Pid
        [VisualDriver]::Key($menu, 0x26); Pause-Ms 200; [VisualDriver]::Key($menu, 0x0D)
        $w = Wait-NewWindow $p.Pid $known 5000
        if ($w -eq [IntPtr]::Zero) { throw 'the speed window prompt did not open' }
        $w
    }
    V2-Simple "dlg$ws-customres" "Custom Analysis resolution prompt $wx" $wx $lanes $true {
        param($p)
        $known = Known-Windows $p.Pid
        [VisualDriver]::Click((Find-Ctl 'v2' $p.H 'Analysis'))
        $a = Wait-NewWindow $p.Pid $known 8000
        if ($a -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
        Pause-Ms 1500
        $cc = Find-CustomCombo $a
        $known = Known-Windows $p.Pid
        [VisualDriver]::ComboSelectPost($cc.H, $cc.I)
        $w = Wait-NewWindow $p.Pid $known 5000
        if ($w -eq [IntPtr]::Zero) { throw 'the Custom resolution prompt did not open' }
        $w
    }
    }
    V2-Dropdown 'dd-overflow-std' 'Overflow drop-down open, standard layout' '' 'Overflow'
    V2-Dropdown 'dd-rate-std' 'Rate model drop-down open, standard layout' '' 'Rate'
    V2-Dropdown 'dd-overflow-compact' 'Overflow drop-down open, compact layout' "--width $(Cw 100)" 'Overflow'
    V2-Dropdown 'dd-rate-compact' 'Rate model drop-down open, compact layout' "--width $(Cw 100)" 'Rate'

    # ================================================================================================== v1
    if ($SkipV1 -or $V1From) {
        if ($V1From) {
            Log "--- v1: reusing the captures of $V1From"
            foreach ($f in Get-ChildItem $V1From -Filter 'v1-*.png') {
                $key = $f.BaseName.Substring(3)
                Copy-Item $f.FullName (Join-Path $OutDir $f.Name) -Force
                [void]$script:Caps.Add([pscustomobject]@{ Prog = 'v1'; Key = $key; Caption = ''; Png = (Join-Path $OutDir $f.Name); Status = 'reused'; Idle = 'reused'; Findings = 0 })
            }
        } else { Log '--- v1: skipped' }
    } else {
        Log '--- v1 captures (one instance, driven through its states)'
        $procId = Start-V1
        Pause-Ms 3000
        $script:V1Borderless = $false

        function V1-Step([string]$key, [string]$caption, [scriptblock]$body) {
            if (-not (Want $key)) { return }
            try { & $body } catch { Fail-State 'v1' $key $_.Exception.Message }
        }
        function V1-Width([int]$w) { $h = V1-Main; $ch = ([VisualDriver]::ClientSize($h))[1]; [VisualDriver]::SetClientSize($h, $w, $ch); Pause-Ms 1200 }
        function V1-Scale([int]$s) { [VisualDriver]::SysCommand((V1-Main), $Cmd["S$s"]); Pause-Ms 1500 }
        function V1-Shot([string]$key, [string]$caption, [bool]$stable = $true, [int]$settle = 1500, [string]$note = '') { Take 'v1' $key $caption (V1-Main) $stable $settle $note }
        function V1-Sub([string]$key, [string]$caption, [scriptblock]$open, [bool]$stable = $true, [bool]$closeAll = $true) {
            V1-Step $key $caption {
                $known = Known-Windows $script:V1.Pid
                $w = & $open $known
                if ($w -eq [IntPtr]::Zero) { throw 'window did not appear' }
                Pause-Ms 2000
                Take 'v1' $key $caption $w $stable 1500
                [VisualDriver]::Close($w); Pause-Ms 800
            }
        }

        V1-Step 'setup' 'select output None' { Select-OutputNone 'v1' (V1-Main) }
        # no file
        V1-Step 'main-nofile-std' 'No file' { V1-Shot 'main-nofile-std' 'No file loaded, standard layout' }
        V1-Step 'main-nofile-compact' 'No file compact' { V1-Width (Cw 100); V1-Shot 'main-nofile-compact' 'No file loaded, compact layout'; V1-Width (Sw 100) }
        # loading the large file
        V1-Step 'main-loading-std' 'Loading' {
            $known = Known-Windows $script:V1.Pid
            V1-OpenFile $Large
            $sw = [Diagnostics.Stopwatch]::StartNew(); $seen = $false
            while ($sw.ElapsedMilliseconds -lt 20000) {
                if ([VisualDriver]::Find((V1-Main), 'msctls_progress32', '', -1) -ne [IntPtr]::Zero) { $seen = $true; break }
                Pause-Ms 20
            }
            Pause-Ms 400
            V1-Shot 'main-loading-std' 'Loading' $false 0 $(if ($seen) { 'progress bar visible' } else { 'progress bar not found' })
            $known.Add((V1-Main).ToInt64()) | Out-Null
            $dlg = @(New-Windows $script:V1.Pid $known)
            if ($dlg.Count -gt 0) { Take 'v1' 'dlg-largefile' 'Large-file warning dialog' $dlg[0] $true 800 }
            V1-WaitLoaded
        }
        V1-Step 'load-dense' 'load models-dense' { V1-Load $dense }
        V1-Step 'main-loaded-std' 'Loaded' { V1-Shot 'main-loaded-std' 'File loaded, standard layout' }
        V1-Step 'main-loaded-compact' 'Loaded compact' { V1-Width (Cw 100); V1-Shot 'main-loaded-compact' 'File loaded, compact layout'; V1-Width (Sw 100) }
        foreach ($s in $scales) {
            V1-Step "scale-$s-std" "scale $s" {
                V1-Scale $s; V1-Width (Sw $s); V1-Shot "scale-$s-std" "Application scale $s%, standard layout"
                V1-Width (Cw $s); V1-Shot "scale-$s-compact" "Application scale $s%, compact layout"
            }
        }
        V1-Step 'scale-back' 'back to 100' { V1-Scale 100; V1-Width (Sw 100) }
        function V1-Style($st) {
            [VisualDriver]::SysCommand((V1-Main), $st[2]); Pause-Ms 1200
            if (-not $script:V1Borderless) { [VisualDriver]::SysCommand((V1-Main), $Cmd.Borderless); Pause-Ms 2000; $script:V1Borderless = $true }
        }
        foreach ($st in $styles) {
            V1-Step "border-$($st[0])-std" "border $($st[0])" {
                V1-Style $st
                V1-Width (Sw 100); V1-Shot "border-$($st[0])-std" "$($st[3]), standard layout"
                V1-Width (Cw 100); V1-Shot "border-$($st[0])-compact" "$($st[3]), compact layout"
                V1-Width (Sw 100)
            }
            # (prompt 009) the same at 50%
            V1-Step "border50-$($st[0])-std" "border $($st[0]) at 50%" {
                V1-Style $st
                V1-Scale 50
                V1-Width (Sw 50); V1-Shot "border50-$($st[0])-std" "$($st[3]), standard layout, scale 50"
                V1-Width (Cw 50); V1-Shot "border50-$($st[0])-compact" "$($st[3]), compact layout, scale 50"
                V1-Scale 100; V1-Width (Sw 100)
            }
        }
        V1-Step 'border-off' 'back to native' {
            if ($script:V1Borderless) { [VisualDriver]::SysCommand((V1-Main), $Cmd.Borderless); Pause-Ms 2000; $script:V1Borderless = $false }
            [VisualDriver]::SysCommand((V1-Main), $Cmd.Classic); Pause-Ms 800
            [void](V1-Main); V1-Width (Sw 100)
        }
        foreach ($dd in @(@('dd-overflow-std', 'Overflow drop-down open, standard layout', 'Overflow', $false), @('dd-rate-std', 'Rate model drop-down open, standard layout', 'Rate', $false),
                          @('dd-overflow-compact', 'Overflow drop-down open, compact layout', 'Overflow', $true), @('dd-rate-compact', 'Rate model drop-down open, compact layout', 'Rate', $true))) {
            $ddk = $dd[0]; $ddc = $dd[1]; $ddw = $dd[2]; $ddcompact = $dd[3]
            V1-Step $ddk $ddc {
                if ($ddcompact) { V1-Width (Cw 100) }
                Dropdown-Capture 'v1' $script:V1.Pid (V1-Main) $ddk $ddc $ddw
                if ($ddcompact) { V1-Width (Sw 100) }
            }
        }
        V1-Step 'queue-off' 'untick queue limit' {
            $q = Find-Ctl 'v1' (V1-Main) 'Queue'
            if ($q -ne [IntPtr]::Zero -and [VisualDriver]::CheckState($q) -ne 0) { [VisualDriver]::Click($q); Pause-Ms 500 }
        }
        # windows (routing-lanes.mid)
        V1-Step 'load-lanes' 'load routing-lanes' { V1-Load $lanes }
        V1-Sub 'win-routing' 'Track routing editor' { param($known) [VisualDriver]::SysCommand((V1-Main), $Cmd.Routing); Pause-Ms 1500; Wait-NewWindow $script:V1.Pid $known 8000 }
        V1-Sub 'win-analysis' 'Analysis window' { param($known) [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Analysis')); Wait-NewWindow $script:V1.Pid $known 8000 }
        V1-Sub 'dlg-about' 'About' { param($known) [VisualDriver]::SysCommand((V1-Main), $Cmd.About); Wait-NewWindow $script:V1.Pid $known 8000 }
        V1-Sub 'dlg-help' 'Help' { param($known) [VisualDriver]::SysCommand((V1-Main), $Cmd.Help); Wait-NewWindow $script:V1.Pid $known 8000 }
        function V1-CustomRes([string]$key, [string]$caption) {
            V1-Step $key $caption {
                $known = Known-Windows $script:V1.Pid
                [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Analysis'))
                $a = Wait-NewWindow $script:V1.Pid $known 8000
                if ($a -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
                Pause-Ms 2000
                try {
                    $cc = Find-CustomCombo $a
                    $known = Known-Windows $script:V1.Pid
                    [VisualDriver]::ComboSelectPost($cc.H, $cc.I)
                    $w = Wait-NewWindow $script:V1.Pid $known 5000
                    if ($w -eq [IntPtr]::Zero) { throw 'the Custom resolution prompt did not open' }
                    Take 'v1' $key $caption $w $true 1500
                    [VisualDriver]::Close($w); Pause-Ms 600
                } finally { [VisualDriver]::Close($a); Pause-Ms 800 }
            }
        }
        V1-CustomRes 'dlg-customres' 'Custom Analysis resolution prompt'
        # (prompt 009, clipping review) the same windows at 50%
        if (@('win50-routing', 'win50-analysis', 'dlg50-about', 'dlg50-help', 'dlg50-customres' | Where-Object { Want $_ }).Count -gt 0) {
            V1-Scale 50
            V1-Sub 'win50-routing' 'Track routing editor, scale 50' { param($known) [VisualDriver]::SysCommand((V1-Main), $Cmd.Routing); Pause-Ms 1500; Wait-NewWindow $script:V1.Pid $known 8000 }
            V1-Sub 'win50-analysis' 'Analysis window, scale 50' { param($known) [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Analysis')); Wait-NewWindow $script:V1.Pid $known 8000 }
            V1-Sub 'dlg50-about' 'About, scale 50' { param($known) [VisualDriver]::SysCommand((V1-Main), $Cmd.About); Wait-NewWindow $script:V1.Pid $known 8000 }
            V1-Sub 'dlg50-help' 'Help, scale 50' { param($known) [VisualDriver]::SysCommand((V1-Main), $Cmd.Help); Wait-NewWindow $script:V1.Pid $known 8000 }
            V1-CustomRes 'dlg50-customres' 'Custom Analysis resolution prompt, scale 50'
            V1-Scale 100
        }
        # playing (routing-patches-4096.mid) and the Channels window
        V1-Step 'load-long' 'load long file' { V1-Load $long; Select-OutputNone 'v1' (V1-Main) }
        V1-Step 'main-playing-std' 'Playing' {
            [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Play')); Wait-Playing 'v1' (V1-Main); Pause-Ms 1500
            V1-Shot 'main-playing-std' 'Playing on output None, standard layout' $false 1500
            V1-Width (Cw 100); Pause-Ms 800
            V1-Shot 'main-playing-compact' 'Playing on output None, compact layout' $false 1500
            V1-Width (Sw 100)
        }
        foreach ($cv in @(@('win-channels', 100, 'Channels window while playing'), @('win50-channels', 50, 'Channels window while playing, scale 50'))) {
        $ck = $cv[0]; $cs = $cv[1]; $ccap = $cv[2]
        V1-Step $ck $ccap {
            if ($cs -ne 100) { V1-Scale $cs }
            if ([VisualDriver]::Txt((Find-Ctl 'v1' (V1-Main) 'Play')) -notmatch 'Pause') {
                Select-OutputNone 'v1' (V1-Main)
                [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Play')); Wait-Playing 'v1' (V1-Main); Pause-Ms 1000
            }
            $known = Known-Windows $script:V1.Pid
            [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Analysis'))
            $a = Wait-NewWindow $script:V1.Pid $known 8000
            if ($a -eq [IntPtr]::Zero) { throw 'Analysis window did not open' }
            Pause-Ms 1500
            $known = Known-Windows $script:V1.Pid
            [VisualDriver]::Click([VisualDriver]::WaitFind($a, 'BUTTON', 'Channels', -1, 5000))
            $c = Wait-NewWindow $script:V1.Pid $known 8000
            if ($c -eq [IntPtr]::Zero) { throw 'Channels window did not open' }
            Pause-Ms 2000
            Take 'v1' $ck $ccap $c $false 1500
            [VisualDriver]::Close($c); [VisualDriver]::Close($a); Pause-Ms 500
            [VisualDriver]::Click((Find-Ctl 'v1' (V1-Main) 'Stop')); Pause-Ms 500
            if ($cs -ne 100) { V1-Scale 100 }
        }
        }
    }
} catch {
    Log ("FAIL gate error: " + $_.Exception.Message)
    $script:V2Fails++
    [void]$script:Notes.Add('gate error: ' + $_.Exception.Message)
} finally {
    foreach ($procId in $script:Started) { Stop-Pid ([int]$procId) }
}

# ====================================================================================================== resize checks
# (prompt 009) only with real screen pixels: a hidden-desktop capture repaints the window and cannot see stale pixels
if ($Visible -and -not $cfg.NoResize) {
    try { . (Join-Path $here 'resize-run.ps1') }
    catch {
        Log ("FAIL resize run error: " + $_.Exception.Message)
        $script:V2Fails++
        [void]$script:Notes.Add('resize run error: ' + $_.Exception.Message)
    }
    finally { foreach ($procId in $script:Started) { Stop-Pid ([int]$procId) } }
}
elseif (-not $Visible) { Log 'resize checks: skipped (they need real screen pixels: run with -Visible)' }

# ====================================================================================================== contact sheets
$sections = @(
    @('01-main-states', 'Main window states (scale 100, native frame)', @('main-nofile-std', 'main-nofile-compact', 'main-loading-std', 'main-loaded-std', 'main-loaded-compact', 'main-playing-std', 'main-playing-compact')),
    @('02-scale-standard', 'Application scale, standard layout', @('scale-50-std', 'scale-75-std', 'scale-100-std', 'scale-125-std', 'scale-150-std', 'scale-200-std')),
    @('03-scale-compact', 'Application scale, compact layout', @('scale-50-compact', 'scale-75-compact', 'scale-100-compact', 'scale-125-compact', 'scale-150-compact', 'scale-200-compact')),
    @('04-frame-styles', 'Frame styles (native frame = main-loaded in sheet 01)', @('border-classic-std', 'border-classic-compact', 'border-integrated-std', 'border-integrated-compact', 'border-overlay-std', 'border-overlay-compact')),
    @('04b-main-50', 'Main window at application scale 50%, every frame style and layout (clipping check; native frame with a file = sheets 02 and 03)', @('main-nofile-50-std', 'main-nofile-50-compact', 'main-playing-50-std', 'main-playing-50-compact', 'border50-classic-std', 'border50-classic-compact', 'border50-integrated-std', 'border50-integrated-compact', 'border50-overlay-std', 'border50-overlay-compact', 'main-loading-50')),
    @('05-windows', 'Other windows and dialogs', @('win-analysis', 'win-channels', 'win-routing', 'dlg-about', 'dlg-help', 'dlg-largefile', 'dlg-speedwindow', 'dlg-customres')),
    @('05b-windows-75', 'Other windows and dialogs at application scale 75% (clipping check)', @('win75-analysis', 'win75-channels', 'win75-routing', 'dlg75-about', 'dlg75-help', 'dlg75-largefile', 'dlg75-speedwindow', 'dlg75-customres', 'main-loading-75')),
    @('05c-windows-50', 'Other windows and dialogs at application scale 50% (clipping check)', @('win50-analysis', 'win50-channels', 'win50-routing', 'dlg50-about', 'dlg50-help', 'dlg50-largefile', 'dlg50-speedwindow', 'dlg50-customres')),
    @('06-dropdowns', 'Drop-downs open', @('dd-overflow-std', 'dd-rate-std', 'dd-overflow-compact', 'dd-rate-compact')),
    @('04c-main-75', 'Main window at application scale 75%, every frame style and layout', @('border75-classic-std', 'border75-classic-compact', 'border75-integrated-std', 'border75-integrated-compact', 'border75-overlay-std', 'border75-overlay-compact')),
    @('07-direct-checkbox', 'Direct WinMM checkbox: off and on (a MIDI device selected), every frame style, both layouts, 100/75/50%', @(foreach ($d in 'native', 'classic', 'integrated', 'overlay') { foreach ($s in 100, 75, 50) { foreach ($m in 'std', 'compact') { foreach ($o in 'off', 'on') { "direct-$o-$d-$s-$m" } } } })),
    @('07b-direct-greyed', 'Direct WinMM checkbox greyed, tick kept: output None, KDMAPI ticked, and a device that is Windows'' own driver (stand-in check)', @(foreach ($d in 'native', 'integrated') { foreach ($s in 100, 50) { foreach ($m in 'std', 'compact') { foreach ($o in 'grey-none', 'grey-kdmapi', 'grey-own') { "direct-$o-$d-$s-$m" } } } })),
    @('07c-playback-state', 'Playback state text on the Playback heading line: Paused (Playing = sheets 01 and 04b), and while loading', @(@(foreach ($d in 'native', 'integrated') { foreach ($s in 100, 50) { foreach ($m in 'std', 'compact') { "state-paused-$d-$s-$m" } } }) + @('direct-loading-100', 'direct-loading-75', 'direct-loading-50'))),
    @('08-resize', 'After resizing (as the resize left the screen; magenta = pixels that differed from a full repaint)', @($script:ResizeKeys))
)
function Find-Png([string]$prog, [string]$key) {
    $c = $script:Caps | Where-Object { $_.Prog -eq $prog -and $_.Key -eq $key } | Select-Object -Last 1
    if ($c -and (Test-Path $c.Png)) { return $c.Png }
    return ''
}
function Caption-For([string]$key) {
    $c = $script:Caps | Where-Object { $_.Key -eq $key -and $_.Caption } | Select-Object -First 1
    if ($c) { return $key + '   -   ' + $c.Caption }
    return $key
}
if ($Gallery) {
    New-Item -ItemType Directory -Force $Gallery | Out-Null
    $tag = if ($cfg.Tag) { $cfg.Tag } else { $cfg.Build }
    foreach ($sec in $sections) {
        $keys = @($sec[2] | Where-Object { (Find-Png 'v2' $_) -or (Find-Png 'v1' $_) })
        if ($keys.Count -eq 0) { continue }
        $page = 0
        for ($i = 0; $i -lt $keys.Count; $i += 3) {
            $part = @($keys[$i..([Math]::Min($i + 2, $keys.Count - 1))])
            $page++
            $rowCaps = [string[]]@($part | ForEach-Object { Caption-For $_ })
            $l = [string[]]@($part | ForEach-Object { Find-Png 'v2' $_ })
            $r = [string[]]@($part | ForEach-Object { Find-Png 'v1' $_ })
            $out = Join-Path $Gallery ("{0}-{1}-p{2}.png" -f $sec[0], $tag, $page)
            [void][ContactSheet]::Build("$($sec[1])   (v2 $tag on the left, v1 on the right)", $rowCaps, $l, $r, "v2 $tag", 'v1', $out, 1000)
            Log ("gallery: " + $out)
        }
    }
}

# ====================================================================================================== verdict
$rows = $script:Caps | ForEach-Object { "{0}`t{1}`t{2}`t{3}`t{4}`t{5}" -f $_.Prog, $_.Key, $_.Status, $_.Idle, $_.Findings, $_.Png }
[IO.File]::WriteAllLines((Join-Path $OutDir 'results.tsv'), [string[]]$rows)
if ($Visible) {
    [IO.File]::WriteAllLines((Join-Path $OutDir 'placements.txt'), [string[]]@($script:Placements))
    foreach ($pl in $script:Placements) { Log ("placement: " + $pl) }
}
$v2n = @($script:Caps | Where-Object { $_.Prog -eq 'v2' }).Count
$v1n = @($script:Caps | Where-Object { $_.Prog -eq 'v1' }).Count
$v1find = ($script:Caps | Where-Object { $_.Prog -eq 'v1' } | Measure-Object -Property Findings -Sum).Sum
Log ''
Log ("captures: v2 {0}, v1 {1} (v1 findings, informational: {2}); idle timeouts/notes: {3}; elapsed {4:N0} s" -f $v2n, $v1n, $v1find, $script:Notes.Count, $Sw.Elapsed.TotalSeconds)
foreach ($n in $script:Notes) { Log ("note: $n") }
$verdict = if ($script:V2Fails -eq 0 -and $v2n -gt 0) { 'VISUAL-GATE: PASS' } else { "VISUAL-GATE: FAIL ($($script:V2Fails) v2 findings)" }
Log $verdict
Set-Content -Path (Join-Path $OutDir 'result.txt') -Value $verdict -Encoding UTF8
