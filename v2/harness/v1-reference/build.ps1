# Builds the v1 reference harness (x86 and x64) from the UNCHANGED v1.0.44
# sources plus V1ReferenceHarness.cs, using the same .NET Framework csc.exe
# that the v1 build.ps1 uses. Refuses to build if any v1 source used differs
# from tag v1.0.44 (raw bytes, via git blob ids with no filters).
param([string]$Tag = 'v1.0.44')

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$out = Join-Path $repo 'v2\out\v1-reference'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "csc.exe not found at $compiler" }

$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'archive\v1\src') -Filter '*.cs' |
    Where-Object { $_.Name -ne 'Program.cs' } | Sort-Object Name)
$mismatch = @()
foreach ($file in $sources) {
    $relative = 'src/' + $file.Name
    $working = (& git -C $repo hash-object --no-filters -- $file.FullName).Trim()
    $tagged = (& git -C $repo rev-parse "${Tag}:$relative" 2>$null)
    if ($LASTEXITCODE -ne 0 -or $working -ne $tagged.Trim()) { $mismatch += $relative }
}
if ($mismatch.Count -gt 0) { throw ("v1 sources differ from ${Tag}: " + ($mismatch -join ', ')) }
# v1's product icon is embedded as v1 embeds it, so TrackRoutingForm can be created.
$icon = Join-Path $repo 'assets\icon\MIDIBottleneck Player.ico'
$iconWorking = (& git -C $repo hash-object --no-filters -- $icon).Trim()
$iconTagged = (& git -C $repo rev-parse "${Tag}:assets/icon/MIDIBottleneck Player.ico" 2>$null)
if ($LASTEXITCODE -ne 0 -or $iconWorking -ne $iconTagged.Trim()) { throw "v1 icon differs from ${Tag}" }
Write-Host ("Verified {0} v1 source files byte-identical to {1}." -f $sources.Count, $Tag)

$harness = @(Get-ChildItem -LiteralPath $here -Filter 'V1*.cs' | Sort-Object Name | ForEach-Object { $_.FullName })
foreach ($arch in 'x86', 'x64') {
    $dir = Join-Path $out $arch
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $exe = Join-Path $dir 'v1-reference.exe'
    $define = if ($arch -eq 'x86') { 'ARCH_X86' } else { 'ARCH_X64' }
    & $compiler /nologo /target:exe "/platform:$arch" "/define:$define" /optimize+ /warn:4 /nowarn:0649 `
        "/out:$exe" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll `
        "/resource:$icon,MidiBottleneck.ProductIcon.ico" /reference:System.Windows.Forms.dll ($sources | ForEach-Object { $_.FullName }) $harness
    if ($LASTEXITCODE -ne 0) { throw "$arch harness compilation failed." }
    if (Test-Path -LiteralPath ($exe + '.config')) { Remove-Item -LiteralPath ($exe + '.config') -Force }
    Write-Host "Built $exe"
}
