# Runs a command (or a script) on a private hidden Win32 desktop, so that every window the command's programs
# create is painted normally but never appears on any monitor and never takes focus. Reusable by any UI test:
#
#   .\v2\tools\visual\run-hidden.ps1 -Command '& .\v2\out\build-x64\Release\midibn-win32.exe --no-settings --selftest r.txt --open f.mid'
#   .\v2\tools\visual\run-hidden.ps1 -File .\v2\tools\ui-smoke.ps1 -Arguments '-Build x64'
#
# The command runs in a new Windows PowerShell started on the hidden desktop (working directory = the current
# directory unless -WorkingDirectory is given); its output is streamed here and kept in -LogFile (default: a file in
# %TEMP%). This script exits with the command's exit code (the last native exit code, or 1 when the command failed
# or the timeout was reached). Programs the command starts inherit the desktop and are killed afterwards, also on error.
#
# Window placement preference for this project's UI tests: first choice is this hidden desktop. When a window really
# has to be on a screen, show it without activation on the 1920x1080 monitor ("Internal Display"); the 3840x2160
# monitor ("LG HDR 4K") only if that is genuinely unavoidable (the user prefers to keep testing off it). Identify the
# monitor by its friendly name or size, not by fixed coordinates: the arrangement can change.
#
# Limits: the hidden desktop has no shell (no taskbar, no notification area, no real foreground window); input is
# never delivered to it, so tests must drive programs with window messages (as visual-gate.ps1 does).
[CmdletBinding()]
param(
    [string]$Command,
    [string]$File,
    [string]$Arguments = '',
    [string]$LogFile,
    [string]$WorkingDirectory,
    [int]$TimeoutMinutes = 60
)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
if ((-not $Command) -eq (-not $File)) { throw 'give exactly one of -Command or -File' }
if (-not $WorkingDirectory) { $WorkingDirectory = (Get-Location).Path }
if (-not $LogFile) { $LogFile = Join-Path $env:TEMP ('run-hidden-' + $PID + '.log') }
$exitFile = $LogFile + '.exit'
Remove-Item $LogFile, $exitFile -ErrorAction SilentlyContinue
if (-not ('HiddenDesktop' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'VisualDriver.cs') -ReferencedAssemblies System.Drawing }

if ($File) { $body = "& '$File' $Arguments" } else { $body = $Command }
$script = "`$ProgressPreference = 'SilentlyContinue'; `$ErrorActionPreference = 'Continue'; `$global:LASTEXITCODE = 0; try { $body } catch { Write-Output (`$_ | Out-String); `$global:LASTEXITCODE = 1 }; [IO.File]::WriteAllText('$exitFile', [string]`$global:LASTEXITCODE)"
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("& { $script } *> '$LogFile'"))
$name = 'midibn-hidden-' + $PID + '-' + [Environment]::TickCount
$desk = [HiddenDesktop]::Create($name)
$innerPid = 0; $timedOut = $false
try {
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $innerPid = [HiddenDesktop]::Start($name, $ps, "-NoProfile -ExecutionPolicy Bypass -OutputFormat Text -EncodedCommand $encoded", $WorkingDirectory)
    $proc = Get-Process -Id $innerPid
    $null = $proc.Handle     # keep the handle so ExitCode stays readable
    $script:shown = 0; $clock = [Diagnostics.Stopwatch]::StartNew()
    $print = {
        if (Test-Path $LogFile) {
            try {
                $stream = New-Object IO.FileStream($LogFile, 'Open', 'Read', 'ReadWrite')
                $reader = New-Object IO.StreamReader($stream)
                $all = $reader.ReadToEnd(); $reader.Dispose()
                $lines = $all -split "`r?`n"
                if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = $lines[0..($lines.Count - 2)] }
                for (; $script:shown -lt $lines.Count; $script:shown++) { Write-Host $lines[$script:shown] }
            } catch { }
        }
    }
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 700
        & $print
        if ($clock.Elapsed.TotalMinutes -gt $TimeoutMinutes) { $timedOut = $true; Write-Host "run-hidden: timeout after $TimeoutMinutes minutes, stopping"; break }
    }
    Start-Sleep -Milliseconds 300
    & $print
} finally {
    # nothing started on the hidden desktop may survive
    if ($innerPid) {
        try {
            Get-CimInstance Win32_Process -Filter "ParentProcessId=$innerPid" -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
            Stop-Process -Id $innerPid -Force -ErrorAction SilentlyContinue
        } catch { }
    }
    [HiddenDesktop]::Close($desk)
}
$code = 1
if (-not $timedOut -and (Test-Path $exitFile)) { try { $code = [int](Get-Content $exitFile -TotalCount 1) } catch { $code = 1 } }
exit $code
