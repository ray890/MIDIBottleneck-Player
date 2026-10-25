# Refreshes the private test kit (default v2\out\test-player\) with the current builds under the release names:
#   x64\              MIDIBottleneck Player x64.exe
#   x64-winmmwrp\     MIDIBottleneck Player x64.exe  (+ your own winmm.dll, which this script never touches)
#   x86\              MIDIBottleneck Player x86.exe
#   xp\               MIDIBottleneck Player XP.exe           (the XP release build, no SSE2 needed)
#   xp-sse2-test\     MIDIBottleneck Player XP SSE2 test.exe (test comparison only)
# It also removes the old midibn-win32.exe copies, removes the old folders xp-no-sse2\ and xp-sse2\, and rewrites
# HOW-TO-TEST.txt (same content, new names and folders).
#
# winmm.dll: this script never creates, copies, moves, renames or deletes any .dll. Your winmm.dll stays in
# x64-winmmwrp\ exactly as it is (if it is missing, a note is printed). An old folder that holds anything besides
# midibn-win32.exe is NOT deleted (only its midibn-win32.exe is) and a note says so.
#
#   .\v2\tools\refresh-test-kit.ps1 -WhatIf        # only prints what would be done
#   .\v2\tools\refresh-test-kit.ps1                # does it
#   .\v2\tools\refresh-test-kit.ps1 -Root <kit folder> -BuildRoot <folder with build-x64\Release\ ...>   # other places (tests)
# Build the four players first (v2\tools\build-player.ps1 -Build x64 | x86 | xp-ia32 | xp). If one is missing nothing is changed.
param(
    [string]$Root = '',
    [string]$BuildRoot = '',
    [switch]$WhatIf
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'release-names.ps1')
if (-not $Root) { $Root = Join-Path $repo 'v2\out\test-player' }
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo 'v2\out' }
$oldFolders = @('xp-no-sse2', 'xp-sse2')

# ---- 1. every source must exist before anything is changed
$missing = @()
foreach ($v in $script:ReleaseVariants) {
    $src = Get-BuiltPlayerPath $BuildRoot $v.Build
    if (-not (Test-Path -LiteralPath $src)) { $missing += "$($v.Exe): $src" }
}
if ($missing.Count -gt 0) { throw ("not built yet, nothing was changed:`n  " + ($missing -join "`n  ")) }
if (-not (Test-Path -LiteralPath $Root)) {
    if ($WhatIf) { Write-Host "WOULD create the kit folder $Root" } else { New-Item -ItemType Directory -Force -Path $Root | Out-Null }
}
$prefix = if ($WhatIf) { 'WOULD' } else { 'DONE ' }
$notes = New-Object System.Collections.ArrayList

# ---- 2. copy the named programs; remove the old midibn-win32.exe copies
$targets = @()
foreach ($v in $script:ReleaseVariants) { $targets += [pscustomobject]@{ Folder = $v.Folder; Variant = $v } }
$targets += [pscustomobject]@{ Folder = 'x64-winmmwrp'; Variant = ($script:ReleaseVariants | Where-Object { $_.Key -eq 'x64' }) }
foreach ($t in $targets) {
    $dir = Join-Path $Root $t.Folder
    $src = Get-BuiltPlayerPath $BuildRoot $t.Variant.Build
    $dst = Join-Path $dir $t.Variant.Exe
    if (-not (Test-NotADll $src) -or -not (Test-NotADll $dst)) { throw 'refusing to copy a DLL' }
    Write-Host "$prefix copy  $($t.Folder)\$($t.Variant.Exe)"
    $old = Join-Path $dir $script:BuiltPlayerName
    if ($WhatIf) { if (Test-Path -LiteralPath $old) { Write-Host "$prefix remove $($t.Folder)\$($script:BuiltPlayerName)" } }
    else {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Force; Write-Host "$prefix remove $($t.Folder)\$($script:BuiltPlayerName)" }
    }
}

# ---- 3. winmm.dll: only looked at, never touched
$wrpDll = Join-Path $Root 'x64-winmmwrp\winmm.dll'
if (Test-Path -LiteralPath $wrpDll) { Write-Host "note: your winmm.dll in x64-winmmwrp\ is left exactly as it is." }
else { [void]$notes.Add('note: x64-winmmwrp\winmm.dll is missing. Put your own winmm.dll there (this script never creates or copies one); until then that folder runs like x64\.') }

# ---- 4. old folders
foreach ($name in $oldFolders) {
    $dir = Join-Path $Root $name
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    $items = @(Get-ChildItem -LiteralPath $dir -Force)
    $others = @($items | Where-Object { $_.Name -ne $script:BuiltPlayerName })
    if ($others.Count -gt 0) {
        Write-Host "$prefix remove $name\$($script:BuiltPlayerName) (the folder stays: it also holds $($others.Name -join ', '))"
        [void]$notes.Add("note: old folder $name\ was NOT deleted because it holds other files ($($others.Name -join ', ')).")
        if (-not $WhatIf) { $exe = Join-Path $dir $script:BuiltPlayerName; if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force } }
    }
    else {
        Write-Host "$prefix remove folder $name\"
        if (-not $WhatIf) {
            foreach ($i in $items) { Remove-Item -LiteralPath $i.FullName -Force }   # only midibn-win32.exe can be in here (checked above)
            [System.IO.Directory]::Delete($dir)                                       # fails (safely) if anything else is inside
        }
    }
}

# ---- 5. HOW-TO-TEST.txt (the previous content, kept; names and folders updated)
$stamp = Get-Date -Format 'yyyy-MM-dd h:mm tt'
$howTo = @"
MIDIBottleneck Player v2 - test kit (private; do not share - the x64-winmmwrp folder holds your winmmwrp winmm.dll)

Folders
  x64\            "MIDIBottleneck Player x64.exe": the normal 64-bit player. Uses Windows' own MIDI output (system WinMM), or KDMAPI if you tick KDMAPI.
  x64-winmmwrp\   The same 64-bit player ("MIDIBottleneck Player x64.exe") with your winmmwrp winmm.dll beside it, so its WinMM output goes
                  straight to OmniMIDI (the same as putting winmm.dll beside v1). The refresh never touches that winmm.dll.
  x86\            "MIDIBottleneck Player x86.exe": the 32-bit player (system WinMM or KDMAPI).
  xp\             "MIDIBottleneck Player XP.exe": the Windows XP release build (runs on any processor, no SSE2 needed).
                  Also runs on newer Windows.
  xp-sse2-test\   "MIDIBottleneck Player XP SSE2 test.exe": the XP build that uses SSE2 - kept only as a test comparison, not for release.

How to try it
  1. Double-click the .exe in one of the folders (or drag a .mid file onto it).
  2. Like v1, it starts with your first real MIDI device selected. Choose "None" under MIDI output for
     silent testing; choose a device (or tick KDMAPI) to hear it.
  3. Open MIDI..., Play, Pause, Stop, -5 sec / +5 sec, drag the position bar, change Rate model and
     Overflow while playing, Analysis..., Reset stats.
  4. Right-click the title bar (or click the program icon) for the window menu: Application scale,
     compact layout, borderless styles (including the Integrated header), Channels, Track routing, Help.

Known state of this kit (it is replaced as the run goes on)
  - Every window (main, Analysis, Channels, Track routing, Help, About, drop-downs) has been compared with v1
    in screenshots at every scale and frame style; report anything that still looks wrong.
  - Channels window: arrow keys, Home/End, Page Up/Down, Enter or F2 to edit, Escape to cancel, Space to mute.
  - Overflow choices now read "Drop newest events" / "Drop oldest events", after the two complete-note choices.
  - Settings are remembered (new): the output, the KDMAPI and Direct WinMM ticks and every option of the window menu are kept in
    settings.ini (folder MIDIBottleneck Player under %APPDATA%; or in "MIDIBottleneck Player.ini" next to the program if that file exists).
    Window position and the loaded file are not remembered. To try it: change two options (for example Application scale
    150% and Chase MIDI state off), pick another output, close the program and start it again: they are restored.
    A damaged file (put some random text into settings.ini) gives the standard settings and a note in the file-information
    area, not a box; deleting the file returns to the standard settings. The Analysis now projects a limited queue with
    "Drop background notes" in seconds instead of minutes. Report anything that is not restored or is restored wrongly.

What to send back
  Anything missing, overlapping, cut off, unreadable or behaving differently from v1, with the window
  and what you did. A screenshot helps.

Update $stamp (refresh-test-kit.ps1): the programs now carry their release names (above) instead of midibn-win32.exe.
  The old folders xp-no-sse2\ (now xp\) and xp-sse2\ (now xp-sse2-test\) are gone; the old midibn-win32.exe copies are removed.
  The programs are the current builds (see the file dates).
"@
$howTo = ($howTo -replace "`r?`n", "`r`n")
$howToPath = Join-Path $Root 'HOW-TO-TEST.txt'
Write-Host "$prefix write $howToPath"
if (-not $WhatIf) { [System.IO.File]::WriteAllText($howToPath, $howTo + "`r`n") }

$notes | ForEach-Object { Write-Host $_ }
Write-Host $(if ($WhatIf) { 'REFRESH-TEST-KIT: WHATIF (nothing was changed)' } else { 'REFRESH-TEST-KIT: DONE' })
