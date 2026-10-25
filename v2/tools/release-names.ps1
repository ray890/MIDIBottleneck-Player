# The release names of the player (user decision, 2026-10-17), shared by package-release.ps1 and refresh-test-kit.ps1
# (dot-source: . "$PSScriptRoot\release-names.ps1").
#
# The build always produces "midibn-win32.exe" (CMake target midibn-win32): the program does not depend on its own file
# name anywhere (no window class, mutex or path is derived from it), but about 30 test scripts look for midibn-win32.exe
# in the build folders. So the names below are given only when files are packaged or copied into a test kit; the build
# output and the tools keep the old name.
#
#   Key       build folder (v2\out\build-<Build>\Release)   file name given to the copy         kit folder    kind
#   x64       build-x64                                     MIDIBottleneck Player x64.exe       x64           release
#   x86       build-x86                                     MIDIBottleneck Player x86.exe       x86           release
#   xp        build-xp-ia32 (no SSE2 needed)                MIDIBottleneck Player XP.exe        xp            release
#   xp-sse2   build-xp (needs SSE2)                         MIDIBottleneck Player XP SSE2 test.exe  xp-sse2-test  test only

$script:BuiltPlayerName = 'midibn-win32.exe'
$script:ReleaseVariants = @(
    [pscustomobject]@{ Key = 'x64';     Build = 'x64';     Exe = 'MIDIBottleneck Player x64.exe';          Folder = 'x64';          Kind = 'release'; What = 'the normal 64-bit player' }
    [pscustomobject]@{ Key = 'x86';     Build = 'x86';     Exe = 'MIDIBottleneck Player x86.exe';          Folder = 'x86';          Kind = 'release'; What = 'the 32-bit player' }
    [pscustomobject]@{ Key = 'xp';      Build = 'xp-ia32'; Exe = 'MIDIBottleneck Player XP.exe';           Folder = 'xp';           Kind = 'release'; What = 'the Windows XP release build (runs on any processor, no SSE2 needed; also runs on newer Windows)' }
    [pscustomobject]@{ Key = 'xp-sse2'; Build = 'xp';      Exe = 'MIDIBottleneck Player XP SSE2 test.exe'; Folder = 'xp-sse2-test'; Kind = 'test';    What = 'the XP build that uses SSE2 (test comparison only, never released)' }
)

function Get-BuiltPlayerPath([string]$BuildRoot, [string]$Build) {
    return (Join-Path $BuildRoot "build-$Build\Release\$($script:BuiltPlayerName)")
}

# Safety net for every script that copies or deletes files: a file is never touched when its name is a DLL at all
# (winmm.dll above all). Returns $true when the path is allowed to be copied, moved or deleted.
function Test-NotADll([string]$Path) {
    $name = [System.IO.Path]::GetFileName($Path)
    return ($name -notmatch '\.dll$')
}
