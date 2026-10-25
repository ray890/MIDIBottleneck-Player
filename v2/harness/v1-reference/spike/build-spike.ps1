# Builds the lock-step rendezvous spike (x86 and x64) from the UNCHANGED
# v1.0.44 sources minus Program.cs and HighResolutionWaiter.cs, plus the spike
# sources in this folder. Refuses to build if any v1 source used differs from
# tag v1.0.44 (raw bytes, via git blob ids with no filters), as build.ps1 does.
param([string]$Tag = 'v1.0.44')

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$out = Join-Path $repo 'v2\out\spike'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "csc.exe not found at $compiler" }

$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'archive1src') -Filter '*.cs' |
    Where-Object { $_.Name -ne 'Program.cs' -and $_.Name -ne 'HighResolutionWaiter.cs' } | Sort-Object Name)
$mismatch = @()
foreach ($file in $sources) {
    $relative = 'src/' + $file.Name
    $working = (& git -C $repo hash-object --no-filters -- $file.FullName).Trim()
    $tagged = (& git -C $repo rev-parse "${Tag}:$relative" 2>$null)
    if ($LASTEXITCODE -ne 0 -or $working -ne $tagged.Trim()) { $mismatch += $relative }
}
if ($mismatch.Count -gt 0) { throw ("v1 sources differ from ${Tag}: " + ($mismatch -join ', ')) }
Write-Host ("Verified {0} v1 source files byte-identical to {1}." -f $sources.Count, $Tag)

$spike = @(Get-ChildItem -LiteralPath $here -Filter '*.cs' | Sort-Object Name | ForEach-Object { $_.FullName })
foreach ($arch in 'x86', 'x64') {
    $dir = Join-Path $out $arch
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $exe = Join-Path $dir 'rendezvous-spike.exe'
    & $compiler /nologo /target:exe "/platform:$arch" /optimize+ /warn:4 /nowarn:0649 `
        "/out:$exe" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll `
        /reference:System.Windows.Forms.dll ($sources | ForEach-Object { $_.FullName }) $spike
    if ($LASTEXITCODE -ne 0) { throw "$arch spike compilation failed." }
    if (Test-Path -LiteralPath ($exe + '.config')) { Remove-Item -LiteralPath ($exe + '.config') -Force }
    Write-Host "Built $exe"
}
