param(
    [switch]$Test,
    [switch]$MidiIntegration
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$buildDirectory = Join-Path $projectRoot 'build'
$distributionDirectory = Join-Path $projectRoot 'dist'
$manifest = Join-Path $projectRoot 'app.manifest'
$runtimeConfiguration = Join-Path $projectRoot 'app.config'
$applicationIcon = Join-Path $projectRoot 'assets\icon\MIDIBottleneck Player.ico'

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "The Windows .NET Framework C# compiler was not found at $compiler"
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $distributionDirectory | Out-Null

$sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Filter '*.cs' | ForEach-Object { $_.FullName })
$testSources = @($sourceFiles | Where-Object { [System.IO.Path]::GetFileName($_) -ne 'Program.cs' })
$testSources += (Join-Path $projectRoot 'tests\TestRunner.cs')

function Get-PeMachine([string]$path) {
    $stream = [System.IO.File]::OpenRead($path)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset + 4
        return $reader.ReadUInt16()
    }
    finally { $stream.Dispose() }
}

function Build-Architecture([string]$architecture, [int]$expectedMachine) {
    $application = Join-Path $distributionDirectory ("MIDIBottleneck Player {0}.exe" -f $architecture)
    $testApplication = Join-Path $buildDirectory ("MidiBottleneck.Tests-{0}.exe" -f $architecture)
    $define = if ($architecture -eq 'x86') { 'ARCH_X86' } else { 'ARCH_X64' }

    & $compiler /nologo /target:winexe "/platform:$architecture" "/define:$define" /optimize+ /warn:4 "/out:$application" "/win32manifest:$manifest" "/win32icon:$applicationIcon" "/resource:$applicationIcon,MidiBottleneck.ProductIcon.ico" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll $sourceFiles
    if ($LASTEXITCODE -ne 0) { throw "$architecture application compilation failed." }
    $machine = Get-PeMachine $application
    if ($machine -ne $expectedMachine) {
        throw ("{0} PE machine mismatch: expected 0x{1:X4}, got 0x{2:X4}." -f $architecture, $expectedMachine, $machine)
    }
    Write-Host ("Built {0} (PE machine 0x{1:X4})" -f $application, $machine)
    Copy-Item -LiteralPath $runtimeConfiguration -Destination ($application + '.config') -Force

    if ($Test) {
        & $compiler /nologo /target:exe "/platform:$architecture" "/define:$define" /optimize+ /warn:4 /nowarn:0649 "/out:$testApplication" "/resource:$applicationIcon,MidiBottleneck.ProductIcon.ico" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll $testSources
        if ($LASTEXITCODE -ne 0) { throw "$architecture test compilation failed." }
        Copy-Item -LiteralPath $runtimeConfiguration -Destination ($testApplication + '.config') -Force
        if ($MidiIntegration) { & $testApplication --midi-integration } else { & $testApplication }
        if ($LASTEXITCODE -ne 0) { throw "$architecture tests failed." }
    }
}

Build-Architecture 'x86' 0x014c
Build-Architecture 'x64' 0x8664
