# Builds the portable core's tests, the v2 baseline (parity) program and the engine harness with the Android NDK as
# native executables, pushes them with their data files to a device or the headless emulator (/data/local/tmp/midibn)
# and runs them there. Mirrors v2/tools/linux-check.sh. Nothing opens a MIDI device or makes a sound.
#
#   powershell -File v2\android\run-native-tests.ps1                  # builds x86_64 + arm64-v8a, runs x86_64 on the emulator
#   powershell -File v2\android\run-native-tests.ps1 -Quick           # parity without the 3,956 engine runs, no engine harness
#   powershell -File v2\android\run-native-tests.ps1 -SkipBuild       # reuse the executables of the last build
#   powershell -File v2\android\run-native-tests.ps1 -Abis arm64-v8a  # build (and run, if a phone with that ABI is
#                                                                     # connected and -UseConnectedDevice is given) only that ABI
# Without -UseConnectedDevice the script uses the headless emulator only: it starts it if no emulator is running and
# stops it again at the end (adb emu kill) when it started it. Results: -OutDir (default v2\out\prompt-012\P12-AND1).
param(
    [string[]]$Abis = @('x86_64', 'arm64-v8a'),
    [string]$Kit = $(if ($env:ANDROID_KIT) { $env:ANDROID_KIT } else { 'D:\Android' }),
    [string]$OutDir = '',
    [int]$ApiLevel = 26,   # tests/linux_platform_tests.cpp uses pthread_getname_np (API 26); the platform layer itself builds at API 23 (the Gradle android6 build)
    [switch]$Quick,
    [switch]$SkipBuild,
    [switch]$BuildOnly,   # compile every ABI, do not touch a device or the emulator
    [switch]$UseConnectedDevice,
    [string]$Avd = 'midibn-api36'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $OutDir) { $OutDir = Join-Path $repo 'v2\out\prompt-012\P12-AND1' }
$buildRoot = Join-Path $repo 'v2\out\android-native'
$testOut = Join-Path $OutDir 'native-tests'
New-Item -ItemType Directory -Force $buildRoot, $testOut | Out-Null

# Per-process environment only (never user or system variables).
$env:JAVA_HOME = Join-Path $Kit 'jdk-17'
$env:ANDROID_HOME = Join-Path $Kit 'Sdk'
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
$env:ANDROID_USER_HOME = Join-Path $Kit 'user-home'
$env:ANDROID_AVD_HOME = Join-Path $Kit 'avd'
$ndk = Join-Path $env:ANDROID_HOME 'ndk\27.3.13750724'
$cmakeBin = Join-Path $env:ANDROID_HOME 'cmake\3.22.1\bin'
$adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'
$emulator = Join-Path $env:ANDROID_HOME 'emulator\emulator.exe'
$remote = '/data/local/tmp/midibn'

function Invoke-Timed([string]$exe, [string]$arguments, [string]$logFile, [int]$timeoutSeconds) {
    # Runs a program, writes stdout+stderr to $logFile, returns the exit code (-999 on timeout; the process is killed).
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = $arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        try { $process.Kill() } catch { }
        [IO.File]::WriteAllText($logFile, $stdout.Result + $stderr.Result + "`nTIMEOUT after $timeoutSeconds s`n")
        return -999
    }
    $process.WaitForExit()
    [IO.File]::WriteAllText($logFile, $stdout.Result + $stderr.Result)
    return $process.ExitCode
}

# ---- build -------------------------------------------------------------------------------------------------------
foreach ($abi in $Abis) {
    $dir = Join-Path $buildRoot $abi
    if ($SkipBuild) { continue }
    Write-Host "== build $abi (API $ApiLevel, Release)"
    $log = Join-Path $testOut "build-$abi.log"
    $configure = "-S `"$here\native`" -B `"$dir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$cmakeBin\ninja.exe`" " +
        "-DCMAKE_TOOLCHAIN_FILE=`"$ndk\build\cmake\android.toolchain.cmake`" -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-$ApiLevel " +
        "-DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DMIDIBN_ANDROID_TESTS=ON"
    $rc = Invoke-Timed (Join-Path $cmakeBin 'cmake.exe') $configure $log 240
    if ($rc -eq 0) { $rc = Invoke-Timed (Join-Path $cmakeBin 'cmake.exe') "--build `"$dir`"" ($log + '.build') 1500 }
    if ($rc -ne 0) { Write-Host "  BUILD FAILED for $abi (exit $rc); see $log and $log.build"; exit 1 }
    $warnings = (Select-String -Path ($log + '.build') -Pattern 'warning:' -SimpleMatch | Measure-Object).Count
    Write-Host "  built, $warnings compiler warning lines"
}

if ($BuildOnly) { Write-Host 'NATIVE-TESTS: built only'; exit 0 }

# ---- device ------------------------------------------------------------------------------------------------------
function Get-Devices { ((& $adb devices) | Select-Object -Skip 1 | Where-Object { $_ -match '\tdevice$' } | ForEach-Object { ($_ -split '\t')[0] }) }
$startedEmulator = $false
$emulatorProcess = $null
$devices = @(Get-Devices | Where-Object { $UseConnectedDevice -or $_ -like 'emulator-*' })
if ($devices.Count -eq 0) {
    Write-Host "== starting the headless emulator ($Avd)"
    $emulatorProcess = Start-Process -FilePath $emulator -ArgumentList @('-avd', $Avd, '-no-window', '-no-audio', '-no-boot-anim', '-no-snapshot', '-gpu', 'swiftshader_indirect') -PassThru -WindowStyle Hidden
    $startedEmulator = $true
    $deadline = (Get-Date).AddMinutes(8)   # a cold first boot takes several minutes
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        $devices = @(Get-Devices | Where-Object { $_ -like 'emulator-*' })
        if ($devices.Count -gt 0) {
            $booted = (& $adb -s $devices[0] shell getprop sys.boot_completed 2>$null)
            if ("$booted".Trim() -eq '1') { break }
        }
    }
    if ($devices.Count -eq 0) {
        Write-Host 'emulator did not come up within 8 minutes'
        & $adb emu kill 2>$null | Out-Null
        try { $emulatorProcess.Kill() } catch { }
        exit 1
    }
}
$serial = $devices[0]
$deviceAbi = ("" + (& $adb -s $serial shell getprop ro.product.cpu.abi)).Trim()
Write-Host "== device $serial, ABI $deviceAbi"

$failures = 0
try {
    # Data files: fixtures and expected results, packed once (tar) and unpacked on the device.
    $tar = Join-Path $buildRoot 'data.tar'
    if (-not (Test-Path $tar) -or -not $SkipBuild) {
        Push-Location $repo
        & tar.exe -cf $tar 'v2/harness/fixtures' 'v2/harness/expected' 'v2/harness/expected-v2' 'v2/harness/expected-v2-ui' 'v2/harness/engine-scenarios.txt'
        Pop-Location
    }
    & $adb -s $serial shell "rm -rf $remote; mkdir -p $remote/bin" | Out-Null
    & $adb -s $serial push $tar "$remote/data.tar" | Out-Null
    & $adb -s $serial shell "cd $remote && tar -xf data.tar && rm data.tar" | Out-Null

    foreach ($abi in $Abis) {
        if ($abi -ne $deviceAbi) { Write-Host "== $abi built, not run (device is $deviceAbi)"; continue }
        $dir = Join-Path $buildRoot $abi
        $resultDir = Join-Path $testOut $abi
        New-Item -ItemType Directory -Force $resultDir | Out-Null
        $names = @('midibn-tests', 'midibn-ui-tests', 'midibn-channels-app-tests', 'midibn-app-tests', 'midibn-analysis-app-tests',
            'midibn-routing-app-tests', 'midibn-engine-tests', 'midibn-linux-platform-tests', 'midibn-route-tests', 'midibn-synth-tests', 'midibn-parity', 'midibn-engine-harness')
        foreach ($n in $names) { & $adb -s $serial push (Join-Path $dir $n) "$remote/bin/$n" | Out-Null }
        & $adb -s $serial shell "chmod 755 $remote/bin/*" | Out-Null

        $parity = '--fixtures v2/harness/fixtures --expected v2/harness/expected --baseline v2/harness/expected-v2 --scenarios v2/harness/engine-scenarios.txt'
        if ($Quick) { $parity += ' --no-engine' }
        $runs = @(
            @('midibn-tests', ''),
            @('midibn-ui-tests', 'v2/harness/expected/ui'),
            @('midibn-channels-app-tests', 'v2/harness/expected/channels-ui'),
            @('midibn-app-tests', 'v2/harness/fixtures'),
            @('midibn-analysis-app-tests', 'v2/harness/fixtures v2/harness/expected/analysis-ui'),
            @('midibn-routing-app-tests', 'v2/harness/fixtures v2/harness/expected/routing-ui v2/harness/expected-v2-ui/routing-ui/explained-states.txt'),
            @('midibn-engine-tests', ''),
            @('midibn-linux-platform-tests', ''),
            @('midibn-route-tests', ''),
            @('midibn-synth-tests', ''),
            @('midibn-parity', $parity))
        if (-not $Quick) {
            $runs += , @('midibn-engine-harness', '--fixtures v2/harness/fixtures --scenarios v2/harness/engine-scenarios.txt --expected v2/harness/expected/engine --rules v2/harness/expected/differences-from-v1.txt --baseline v2/harness/expected-v2/engine')
        }
        $summary = @()
        foreach ($run in $runs) {
            $name = $run[0]
            $log = Join-Path $resultDir "$name.log"
            $started = Get-Date
            $rc = Invoke-Timed $adb "-s $serial shell `"cd $remote && ./bin/$name $($run[1]); echo EXIT-CODE=`$?`"" $log 1500
            $text = [IO.File]::ReadAllText($log)
            $code = if ($text -match 'EXIT-CODE=(\d+)') { [int]$Matches[1] } else { -1 }
            $seconds = [int]((Get-Date) - $started).TotalSeconds
            $lastLine = ($text -split "`n" | Where-Object { $_ -match '^(RESULT|PASS:|FAIL:|UI TESTS|CHANNELS APP TESTS|midibn-analysis-app-tests:|PARITY)' } | Select-Object -Last 1)
            $status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
            if ($code -ne 0) { $failures++ }
            $line = '{0,-30} {1}  {2,4} s  {3}' -f $name, $status, $seconds, ("" + $lastLine).Trim()
            Write-Host $line
            $summary += $line
        }
        $summary | Set-Content (Join-Path $resultDir 'summary.txt')
    }
}
finally {
    if ($startedEmulator) {
        Write-Host '== stopping the emulator'
        & $adb -s $serial emu kill 2>$null | Out-Null
        if ($emulatorProcess) { $null = $emulatorProcess.WaitForExit(30000); if (-not $emulatorProcess.HasExited) { try { $emulatorProcess.Kill() } catch { } } }
    }
}
if ($failures -eq 0) { Write-Host 'NATIVE-TESTS: PASS'; exit 0 }
Write-Host "NATIVE-TESTS: FAIL ($failures)"; exit 1
