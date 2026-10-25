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
$applicationIcon = Join-Path $projectRoot 'assets\icon\MIDIBottleneck Player.ico'
$nativeWindowMonitor = Join-Path $buildDirectory 'NativeWindowMonitor.exe'

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "The Windows .NET Framework C# compiler was not found at $compiler"
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $distributionDirectory | Out-Null

if ($Test) {
    & $compiler /nologo /target:exe /platform:anycpu /optimize+ /warn:4 "/out:$nativeWindowMonitor" (Join-Path $projectRoot 'tools\NativeWindowMonitor.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Native-window monitor compilation failed.' }
}

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
    $applicationConfiguration = $application + '.config'
    if (Test-Path -LiteralPath $applicationConfiguration) {
        Remove-Item -LiteralPath $applicationConfiguration -Force
    }

    if ($Test) {
        & $compiler /nologo /target:exe "/platform:$architecture" "/define:$define" /optimize+ /warn:4 /nowarn:0649 "/out:$testApplication" "/resource:$applicationIcon,MidiBottleneck.ProductIcon.ico" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll $testSources
        if ($LASTEXITCODE -ne 0) { throw "$architecture test compilation failed." }
        $testConfiguration = $testApplication + '.config'
        if (Test-Path -LiteralPath $testConfiguration) {
            Remove-Item -LiteralPath $testConfiguration -Force
        }
        $windowProofLog = Join-Path $buildDirectory ("native-window-proof-{0}.jsonl" -f $architecture)
        & $nativeWindowMonitor --output $windowProofLog --post-exit-ms 2000 --match 'Deliberately identifiable detector proof window' --expect-match -- $testApplication --native-window-monitor-fixture
        if ($LASTEXITCODE -ne 0) { throw "$architecture native-window detector proof failed." }
        $processProofLog = Join-Path $buildDirectory ("native-process-proof-{0}.jsonl" -f $architecture)
        & $nativeWindowMonitor --output $processProofLog --post-exit-ms 2000 --process-match ("MidiBottleneck.Tests-{0}" -f $architecture) --expect-process-match -- $testApplication --native-process-monitor-fixture
        if ($LASTEXITCODE -ne 0) { throw "$architecture descendant-process detector proof failed." }
        $windowLog = Join-Path $buildDirectory ("native-window-{0}.jsonl" -f $architecture)
        $monitorArguments = @('--output', $windowLog, '--post-exit-ms', '5000', '--timeout-ms', '240000', '--', $testApplication)
        if ($MidiIntegration) { $monitorArguments += '--midi-integration' }
        & $nativeWindowMonitor $monitorArguments
        if ($LASTEXITCODE -ne 0) { throw "$architecture tests failed." }
    }
}

Build-Architecture 'x86' 0x014c
Build-Architecture 'x64' 0x8664
