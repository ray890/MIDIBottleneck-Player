# Builds the v1 UI reference tool (x64 only) from the UNCHANGED v1.0.44 sources plus
# UiReference*.cs, using the .NET Framework csc.exe that v1's build.ps1 uses. Refuses to
# build if any v1 source used differs from tag v1.0.44 (raw bytes, via git blob ids).
#
#   .\build.ps1                build v2\out\ui-reference\x64\ui-reference.exe
#   .\build.ps1 -Run           ... and rewrite v2\harness\expected\ui\*.txt
#   .\build.ps1 -EmitHelp      ... and write v1's help text (for reference only) to v2\out\ui-reference\v1-help-text.cpp
param([string]$Tag = 'v1.0.44', [switch]$Run, [switch]$EmitHelp)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$out = Join-Path $repo 'v2\out\ui-reference'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "csc.exe not found at $compiler" }

$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'archive1src') -Filter '*.cs' |
    Where-Object { $_.Name -ne 'Program.cs' } | Sort-Object Name)
$mismatch = @()
foreach ($file in $sources) {
    $relative = 'src/' + $file.Name
    # Raw bytes, or (a checkout with core.autocrlf=true has CRLF files) the bytes after git's own
    # line-ending normalisation: either must equal the tagged blob.
    $raw = (& git -C $repo hash-object --no-filters -- $file.FullName).Trim()
    $normalised = (& git -C $repo hash-object -- $file.FullName).Trim()
    $tagged = (& git -C $repo rev-parse "${Tag}:$relative" 2>$null)
    if ($LASTEXITCODE -ne 0 -or ($raw -ne $tagged.Trim() -and $normalised -ne $tagged.Trim())) { $mismatch += $relative }
}
if ($mismatch.Count -gt 0) { throw ("v1 sources differ from ${Tag}: " + ($mismatch -join ', ')) }
# v1's product icon is embedded as v1 embeds it, so the help and routing forms can be created.
$icon = Join-Path $repo 'assets\icon\MIDIBottleneck Player.ico'
$iconWorking = (& git -C $repo hash-object --no-filters -- $icon).Trim()
$iconTagged = (& git -C $repo rev-parse "${Tag}:assets/icon/MIDIBottleneck Player.ico" 2>$null)
if ($LASTEXITCODE -ne 0 -or $iconWorking -ne $iconTagged.Trim()) { throw "v1 icon differs from ${Tag}" }
Write-Host ("Verified {0} v1 source files equal to {1}." -f $sources.Count, $Tag)

$harness = @(Get-ChildItem -LiteralPath $here -Filter 'UiReference*.cs' | Sort-Object Name | ForEach-Object { $_.FullName })
$dir = Join-Path $out 'x64'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$exe = Join-Path $dir 'ui-reference.exe'
& $compiler /nologo /target:exe /platform:x64 /define:ARCH_X64 /optimize+ /warn:4 /nowarn:0649 `
    "/out:$exe" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll `
    "/resource:$icon,MidiBottleneck.ProductIcon.ico" /reference:System.Windows.Forms.dll `
    ($sources | ForEach-Object { $_.FullName }) $harness
if ($LASTEXITCODE -ne 0) { throw 'ui-reference compilation failed.' }
if (Test-Path -LiteralPath ($exe + '.config')) { Remove-Item -LiteralPath ($exe + '.config') -Force }
Write-Host "Built $exe"

if ($Run) {
    $tables = Join-Path $repo 'v2\harness\expected\ui'
    & $exe $tables
    if ($LASTEXITCODE -ne 0) { throw 'ui-reference run failed.' }
}
if ($EmitHelp) {
    $helpOut = Join-Path $repo 'v2\out\ui-reference\v1-help-text.cpp'   # v2's Help text is hand-written; this only writes v1's text for reference
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $helpOut) | Out-Null
    & $exe --emit-help $helpOut
    if ($LASTEXITCODE -ne 0) { throw 'ui-reference --emit-help failed.' }
}
