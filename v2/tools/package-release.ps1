# Collects the four players under their release names in v2\out\release\<date>\ (a private folder, not tracked, not uploaded):
#   MIDIBottleneck Player x64.exe          (build-x64)
#   MIDIBottleneck Player x86.exe          (build-x86)
#   MIDIBottleneck Player XP.exe           (build-xp-ia32: the XP release build, runs without SSE2)
#   test-only\MIDIBottleneck Player XP SSE2 test.exe   (build-xp: SSE2, kept only for test comparisons; never released)
# plus README.txt (what each file is) and SHA256SUMS.txt (SHA-256 of each file). Nothing is published, uploaded or signed,
# and no DLL (no winmm.dll, no provider) is ever copied: only the four .exe files are touched.
#
#   .\v2\tools\package-release.ps1                 # uses the players already built (v2\out\build-*\Release\midibn-win32.exe)
#   .\v2\tools\package-release.ps1 -Build          # builds the four first (v2\tools\build-player.ps1), then packages
#   .\v2\tools\package-release.ps1 -WhatIf         # only prints what it would do
#   .\v2\tools\package-release.ps1 -Date 2026-10-17 -OutRoot D:\somewhere   # other date / output root
# Why the names are given here and not by the build: see release-names.ps1.
param(
    [switch]$Build,
    [string]$Date = '',
    [string]$OutRoot = '',
    [string]$BuildRoot = '',
    [switch]$WhatIf
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'release-names.ps1')
if (-not $Date) { $Date = Get-Date -Format 'yyyy-MM-dd' }
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'v2\out\release' }
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo 'v2\out' }
$dest = Join-Path $OutRoot $Date
$testDir = Join-Path $dest 'test-only'

if ($Build) {
    foreach ($v in $script:ReleaseVariants) {
        if ($WhatIf) { Write-Host "WOULD build: v2\tools\build-player.ps1 -Build $($v.Build)"; continue }
        & (Join-Path $PSScriptRoot 'build-player.ps1') -Build $v.Build | Out-Host
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw "build of $($v.Build) failed" }
    }
}

# every source must exist before anything is written
$missing = @()
foreach ($v in $script:ReleaseVariants) {
    $src = Get-BuiltPlayerPath $BuildRoot $v.Build
    if (-not (Test-Path -LiteralPath $src)) { $missing += "$($v.Exe): $src" }
}
if ($missing.Count -gt 0 -and -not ($WhatIf -and $Build)) {
    throw ("not built yet (run again with -Build, or build them with v2\tools\build-player.ps1):`n  " + ($missing -join "`n  "))
}

Write-Host "Release folder: $dest"
$sums = New-Object System.Collections.ArrayList
$placed = New-Object System.Collections.ArrayList
foreach ($v in $script:ReleaseVariants) {
    $src = Get-BuiltPlayerPath $BuildRoot $v.Build
    $dir = if ($v.Kind -eq 'test') { $testDir } else { $dest }
    $target = Join-Path $dir $v.Exe
    if (-not (Test-NotADll $src) -or -not (Test-NotADll $target)) { throw 'refusing to copy a DLL' }
    if ($WhatIf) { Write-Host "WOULD copy: $src -> $target"; continue }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath $src -Destination $target -Force
    $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    $rel = if ($v.Kind -eq 'test') { "test-only\$($v.Exe)" } else { $v.Exe }
    [void]$sums.Add(("{0} *{1}" -f $hash.ToLowerInvariant(), $rel))
    [void]$placed.Add([pscustomobject]@{ Exe = $v.Exe; Rel = $rel; Kind = $v.Kind; What = $v.What; Size = (Get-Item -LiteralPath $target).Length; Hash = $hash })
    Write-Host ("  {0}  ({1:N0} bytes)" -f $rel, (Get-Item -LiteralPath $target).Length)
}

$readme = @(
    "MIDIBottleneck Player v2 - release candidates, $Date (private; nothing here has been published or uploaded)",
    '',
    'Files',
    '  MIDIBottleneck Player x64.exe   the normal 64-bit player (Windows 10 / 11).',
    '  MIDIBottleneck Player x86.exe   the 32-bit player (32-bit Windows, 32-bit MIDI setups).',
    '  MIDIBottleneck Player XP.exe    the Windows XP release build. Runs on any processor (it needs no SSE2) and also on newer Windows.',
    '  test-only\MIDIBottleneck Player XP SSE2 test.exe',
    '                                  the XP build that uses SSE2. For test comparisons only; it is never released.',
    '  SHA256SUMS.txt                  the SHA-256 checksum of each file (check with: certutil -hashfile "<file>" SHA256).',
    '',
    'Notes',
    '  - Each file is a single program that you can start with a double-click or by dropping a .mid file on it.',
    '  - The files are the build output copied under these names; the program does not depend on its own file name.',
    '  - No winmm.dll and no other DLL is included or needed. A private winmm.dll next to a player is a test setup of yours only.',
    '  - Output None is silent; choose a device or tick KDMAPI to hear it.'
)
if ($WhatIf) { Write-Host "WOULD write: $(Join-Path $dest 'README.txt') and $(Join-Path $dest 'SHA256SUMS.txt')"; Write-Host 'WHATIF: nothing was written'; exit 0 }
[System.IO.File]::WriteAllText((Join-Path $dest 'README.txt'), (($readme -join "`r`n") + "`r`n"))
[System.IO.File]::WriteAllText((Join-Path $dest 'SHA256SUMS.txt'), (($sums -join "`r`n") + "`r`n"))
$dlls = @(Get-ChildItem -LiteralPath $dest -Recurse -File | Where-Object { $_.Name -match '\.dll$' })
if ($dlls.Count -gt 0) { throw "a DLL is in the release folder: $($dlls[0].FullName)" }
Write-Host "Wrote README.txt and SHA256SUMS.txt; no DLL in the folder."
Write-Host "PACKAGE-RELEASE: DONE ($($placed.Count) programs)"
