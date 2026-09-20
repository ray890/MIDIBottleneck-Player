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

# This allowlist is deliberately explicit. Never sweep the ignored dist
# directory into a package, even when it currently appears to contain only
# first-party output.
$releaseFiles = @(
    @{ Source = (Join-Path $distributionDirectory 'MIDIBottleneck Player x86.exe'); Name = 'MIDIBottleneck Player x86.exe' },
    @{ Source = (Join-Path $distributionDirectory 'MIDIBottleneck Player x86.exe.config'); Name = 'MIDIBottleneck Player x86.exe.config' },
    @{ Source = (Join-Path $distributionDirectory 'MIDIBottleneck Player x64.exe'); Name = 'MIDIBottleneck Player x64.exe' },
    @{ Source = (Join-Path $distributionDirectory 'MIDIBottleneck Player x64.exe.config'); Name = 'MIDIBottleneck Player x64.exe.config' },
    @{ Source = (Join-Path $projectRoot 'LICENSE'); Name = 'LICENSE' },
    @{ Source = (Join-Path $projectRoot 'README.md'); Name = 'README.md' }
)

$entries = foreach ($file in $releaseFiles) {
    $path = $file.Source
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required first-party release file is missing: $path"
    }
    $item = Get-Item -LiteralPath $path
    $hash = Get-FileHash -LiteralPath $path -Algorithm SHA256
    [PSCustomObject]@{ Name = $file.Name; Source = $path; Bytes = $item.Length; SHA256 = $hash.Hash }
}

$entries | Select-Object Name, Bytes, SHA256 | Format-Table -AutoSize
if (-not $WritePackage) {
    Write-Host 'Dry run only. Pass -WritePackage to copy exactly these allowlisted first-party files and write SHA256SUMS.txt.'
    return
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
foreach ($entry in $entries) {
    Copy-Item -LiteralPath $entry.Source -Destination (Join-Path $OutputDirectory $entry.Name) -Force
}
$checksumLines = $entries | ForEach-Object { $_.SHA256.ToLowerInvariant() + '  ' + $_.Name }
Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Value $checksumLines -Encoding ASCII
Write-Host "Wrote first-party release package files to $OutputDirectory"
