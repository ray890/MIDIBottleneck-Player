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
$commonFiles = @(
    @{ Source = (Join-Path $projectRoot 'README.md'); Name = 'README.md' },
    @{ Source = (Join-Path $projectRoot 'LICENSE'); Name = 'LICENSE' }
)

foreach ($required in @($x86Application, $x64Application, ($x64Application + '.config')) + @($commonFiles | ForEach-Object { $_.Source })) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required first-party release file is missing: $required"
    }
}

$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($x64Application).FileVersion
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)\.\d+$') {
    throw "Unexpected application file version: $version"
}
$releaseVersion = $Matches[1] + '.' + $Matches[2] + '.' + $Matches[3]
$x86ZipName = "MIDIBottleneck-Player-v$releaseVersion-x86.zip"
$x64ZipName = "MIDIBottleneck-Player-v$releaseVersion-x64.zip"

$packages = @(
    @{
        Name = $x86ZipName
        Files = @(
            @{ Source = $x86Application; Name = 'MIDIBottleneck Player x86.exe' }
        ) + $commonFiles
    },
    @{
        Name = $x64ZipName
        Files = @(
            @{ Source = $x64Application; Name = 'MIDIBottleneck Player x64.exe' },
            @{ Source = ($x64Application + '.config'); Name = 'MIDIBottleneck Player x64.exe.config' }
        ) + $commonFiles
    }
)

Write-Host "Release v$releaseVersion package plan:"
foreach ($package in $packages) {
    Write-Host ("  {0}" -f $package.Name)
    foreach ($file in $package.Files) {
        $item = Get-Item -LiteralPath $file.Source
        Write-Host ("    {0} ({1} bytes)" -f $file.Name, $item.Length)
    }
}

if (-not $WritePackage) {
    Write-Host 'Dry run only. Pass -WritePackage to create the two allowlisted ZIPs and SHA256SUMS.txt.'
    return
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

foreach ($package in $packages) {
    $zipPath = Join-Path $OutputDirectory $package.Name
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    $archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $package.Files) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.Source, $file.Name, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally {
        $archive.Dispose()
    }
}

$checksumPath = Join-Path $OutputDirectory 'SHA256SUMS.txt'
$checksumLines = foreach ($package in $packages) {
    $zipPath = Join-Path $OutputDirectory $package.Name
    $hash = Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
    $hash.Hash.ToLowerInvariant() + '  ' + $package.Name
}
Set-Content -LiteralPath $checksumPath -Value $checksumLines -Encoding ASCII

Write-Host "Wrote first-party release packages to $OutputDirectory"
Get-ChildItem -LiteralPath $OutputDirectory -File |
    Where-Object { $_.Name -in @($x86ZipName, $x64ZipName, 'SHA256SUMS.txt') } |
    Sort-Object Name |
    ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
        [PSCustomObject]@{ Name = $_.Name; Bytes = $_.Length; SHA256 = $hash.Hash }
    } | Format-Table -AutoSize
