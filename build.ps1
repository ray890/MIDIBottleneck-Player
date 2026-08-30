param(
    [switch]$Test,
    [string]$OutputName = 'MidiBottleneck.exe',
    [switch]$MidiIntegration
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$buildDirectory = Join-Path $projectRoot 'build'
$distributionDirectory = Join-Path $projectRoot 'dist'

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "The Windows .NET Framework C# compiler was not found at $compiler"
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $distributionDirectory | Out-Null

$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Filter '*.cs' | ForEach-Object { $_.FullName }
$application = Join-Path $distributionDirectory $OutputName
$manifest = Join-Path $projectRoot 'app.manifest'

& $compiler /nologo /target:winexe /platform:anycpu /optimize+ /warn:4 "/out:$application" "/win32manifest:$manifest" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll $sourceFiles
if ($LASTEXITCODE -ne 0) { throw 'Application compilation failed.' }

Write-Host "Built $application"

if ($Test) {
    $testApplication = Join-Path $buildDirectory 'MidiBottleneck.Tests.exe'
    $testSources = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Filter '*.cs' | Where-Object { $_.Name -ne 'Program.cs' } | ForEach-Object { $_.FullName })
    $testSources += (Join-Path $projectRoot 'tests\TestRunner.cs')
    & $compiler /nologo /target:exe /platform:anycpu /optimize+ /warn:4 /nowarn:0649 "/out:$testApplication" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll $testSources
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed.' }
    if ($MidiIntegration) {
        & $testApplication --midi-integration
    } else {
        & $testApplication
    }
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
