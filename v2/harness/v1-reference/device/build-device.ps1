# Builds the v1 real-device measurement driver (x86 and x64) from the UNCHANGED v1.0.44
# sources (including the real HighResolutionWaiter.cs, WindowsMidiOutput.cs and
# KdmApiMidiOutput.cs; only Program.cs is left out because the driver has its own Main)
# plus DeviceMeasure.cs. Nothing is shadowed: Stopwatch, SpinWait, EventWaitHandle and
# threads are the framework's. Refuses to build if any v1 source used differs from tag
# v1.0.44 (raw bytes, via git blob ids with no filters), as the other v1 builds do.
#
# Output: v2\out\device-test\v1-x64\v1-device.exe and v2\out\device-test\v1-x86\v1-device.exe
param([string]$Tag = 'v1.0.44')

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$out = Join-Path $repo 'v2\out\device-test'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "csc.exe not found at $compiler" }

$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'archive1src') -Filter '*.cs' |
    Where-Object { $_.Name -ne 'Program.cs' } | Sort-Object Name)
$mismatch = @()
foreach ($file in $sources) {
    $relative = 'src/' + $file.Name
    $tagged = (& git -C $repo rev-parse "${Tag}:$relative" 2>$null)
    if ($LASTEXITCODE -ne 0) { $mismatch += $relative; continue }
    $working = (& git -C $repo hash-object --no-filters -- $file.FullName).Trim()
    if ($working -ne $tagged.Trim()) {
        # A checkout with core.autocrlf=true holds CRLF text; git's own (filtered) hash maps it back to the committed LF form.
        $filtered = (& git -C $repo hash-object -- $file.FullName).Trim()
        if ($filtered -ne $tagged.Trim()) { $mismatch += $relative }
    }
}
if ($mismatch.Count -gt 0) { throw ("v1 sources differ from ${Tag}: " + ($mismatch -join ', ')) }
$icon = Join-Path $repo 'assets\icon\MIDIBottleneck Player.ico'
$iconWorking = (& git -C $repo hash-object --no-filters -- $icon).Trim()
$iconTagged = (& git -C $repo rev-parse "${Tag}:assets/icon/MIDIBottleneck Player.ico" 2>$null)
if ($LASTEXITCODE -ne 0 -or $iconWorking -ne $iconTagged.Trim()) { throw "v1 icon differs from ${Tag}" }
Write-Host ("Verified {0} v1 source files identical in content to {1} (Program.cs excluded; line endings aside)." -f $sources.Count, $Tag)

$own = @(Get-ChildItem -LiteralPath $here -Filter '*.cs' | Sort-Object Name | ForEach-Object { $_.FullName })
foreach ($arch in 'x86', 'x64') {
    $dir = Join-Path $out "v1-$arch"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $exe = Join-Path $dir 'v1-device.exe'
    & $compiler /nologo /target:exe "/platform:$arch" /optimize+ /warn:4 /nowarn:0649 `
        "/out:$exe" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll `
        "/resource:$icon,MidiBottleneck.ProductIcon.ico" /reference:System.Windows.Forms.dll `
        ($sources | ForEach-Object { $_.FullName }) $own
    if ($LASTEXITCODE -ne 0) { throw "$arch compilation failed." }
    if (Test-Path -LiteralPath ($exe + '.config')) { Remove-Item -LiteralPath ($exe + '.config') -Force }
    Write-Host "Built $exe"
}

