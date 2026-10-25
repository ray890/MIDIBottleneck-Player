param(
    [string]$OutputDirectory,
    [switch]$WritePackage
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$distributionDirectory = Join-Path $projectRoot 'dist'
if ([String]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'release-package'
}

$x86Application = Join-Path $distributionDirectory 'MIDIBottleneck Player x86.exe'
$x64Application = Join-Path $distributionDirectory 'MIDIBottleneck Player x64.exe'
foreach ($required in @($x86Application, $x64Application)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required first-party release file is missing: $required"
    }
}

$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($x64Application).FileVersion
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)\.\d+$') {
    throw "Unexpected application file version: $version"
}
$releaseVersion = $Matches[1] + '.' + $Matches[2] + '.' + $Matches[3]
$x86ReleaseName = "MIDIBottleneck-Player-v$releaseVersion-x86.exe"
$x64ReleaseName = "MIDIBottleneck-Player-v$releaseVersion-x64.exe"

$assets = @(
    @{ Source = $x86Application; Name = $x86ReleaseName },
    @{ Source = $x64Application; Name = $x64ReleaseName }
)

Write-Host "Release v$releaseVersion loose-asset plan:"
foreach ($asset in $assets) {
    $item = Get-Item -LiteralPath $asset.Source
    Write-Host ("  {0} ({1} bytes)" -f $asset.Name, $item.Length)
}

if (-not $WritePackage) {
    Write-Host 'Dry run only. Pass -WritePackage to copy the two allowlisted executables and create SHA256SUMS.txt.'
    return
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$expectedOutputNames = @($x86ReleaseName, $x64ReleaseName, 'SHA256SUMS.txt')
$unexpected = @(Get-ChildItem -LiteralPath $OutputDirectory -File |
    Where-Object { $_.Name -notin $expectedOutputNames })
if ($unexpected.Count -ne 0) {
    throw "Release output directory is not empty. Remove unrelated/stale files or choose a clean directory: $OutputDirectory"
}

foreach ($asset in $assets) {
    Copy-Item -LiteralPath $asset.Source -Destination (Join-Path $OutputDirectory $asset.Name) -Force
}

$checksumPath = Join-Path $OutputDirectory 'SHA256SUMS.txt'
$checksumLines = foreach ($asset in $assets) {
    $path = Join-Path $OutputDirectory $asset.Name
    $hash = Get-FileHash -LiteralPath $path -Algorithm SHA256
    $hash.Hash.ToLowerInvariant() + '  ' + $asset.Name
}
Set-Content -LiteralPath $checksumPath -Value $checksumLines -Encoding ASCII

Write-Host "Wrote first-party release assets to $OutputDirectory"
Get-ChildItem -LiteralPath $OutputDirectory -File |
    Where-Object { $_.Name -in @($x86ReleaseName, $x64ReleaseName, 'SHA256SUMS.txt') } |
    Sort-Object Name |
    ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
        [PSCustomObject]@{ Name = $_.Name; Bytes = $_.Length; SHA256 = $hash.Hash }
    } | Format-Table -AutoSize
