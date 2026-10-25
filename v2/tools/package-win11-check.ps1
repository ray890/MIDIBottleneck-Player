# Assembles the Windows 11 check package: v2\out\win11-check\ (midibn-direct-check.exe, RUN-CHECK.cmd, README.txt)
# and the zip v2\out\v2-win11-check.zip. Needs the x64 Release build of midibn-direct-check (see v2\BUILDING.md).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo
$exe = 'v2\out\build-x64\Release\midibn-direct-check.exe'
if (-not (Test-Path $exe)) { throw "Build the x64 Release configuration first (see v2\BUILDING.md): missing $exe" }
$target = 'v2\out\win11-check'
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item $exe $target
foreach ($name in 'RUN-CHECK.cmd', 'README.txt') {
    $text = [System.IO.File]::ReadAllText((Join-Path $repo "v2\tools\win11-check\$name"))
    [System.IO.File]::WriteAllText((Join-Path $repo "$target\$name"), $text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
}

$zip = 'v2\out\v2-win11-check.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
# Entries are written one by one with '/' separators (see package-xp-selftest.ps1).
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = (Resolve-Path $target).Path
$archive = [System.IO.Compression.ZipFile]::Open((Join-Path $repo $zip), [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $root -Recurse -File) {
        $entryName = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally { $archive.Dispose() }
foreach ($file in Get-ChildItem $target -File) { Write-Host ("{0,-28} {1,10:N0} bytes" -f $file.Name, $file.Length) }
Write-Host ("exe SHA-256: " + (Get-FileHash (Join-Path $target 'midibn-direct-check.exe') -Algorithm SHA256).Hash)
Write-Host ("{0} {1:N0} bytes" -f $zip, (Get-Item $zip).Length)
Write-Host ("zip SHA-256: " + (Get-FileHash $zip -Algorithm SHA256).Hash)
