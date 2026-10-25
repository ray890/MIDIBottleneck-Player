# Builds the v2 player (midibn-win32.exe) for x64 (default), x86 or a Windows XP variant and prints the exe path.
# xp-ia32 is the XP release build (/arch:IA32, no SSE2 needed); xp (SSE2) is kept only as a test comparison.
#   .\v2\tools\build-player.ps1            # x64
#   .\v2\tools\build-player.ps1 -Build x86
#   .\v2\tools\build-player.ps1 -Build xp-ia32   # the XP release build
#   .\v2\tools\build-player.ps1 -Build xp        # SSE2, test comparison only
param([ValidateSet('x64', 'x86', 'xp', 'xp-ia32')][string]$Build = 'x64')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$dir = Join-Path $repo "v2\out\build-$Build"
Push-Location $repo
try {
    switch ($Build) {
        'x64' { & $cmake -S v2 -B $dir -G 'Visual Studio 17 2022' -A x64 -T v143 }
        'x86' { & $cmake -S v2 -B $dir -G 'Visual Studio 17 2022' -A Win32 -T v143 }
        'xp'  { & $cmake -S v2 -B $dir -G 'Visual Studio 17 2022' -A Win32 -T v141_xp -DMIDIBN_XP=ON }
        'xp-ia32' { & $cmake -S v2 -B $dir -G 'Visual Studio 17 2022' -A Win32 -T v141_xp -DMIDIBN_XP=ON -DMIDIBN_XP_IA32=ON }
    }
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
    & $cmake --build $dir --config Release --target midibn-win32 -- /m /v:m
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
} finally { Pop-Location }
Write-Host ("Built: " + (Join-Path $dir 'Release\midibn-win32.exe'))
