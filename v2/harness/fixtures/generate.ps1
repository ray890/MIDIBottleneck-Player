# Regenerates the committed parity fixtures (and, with -Large, the large
# performance fixture under fixtures\large\, which is not committed).
param([switch]$Large)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$out = Join-Path $repo 'v2\out\fixture-generator'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'FixtureGenerator.exe'
& 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe' /nologo /target:exe /platform:anycpu /optimize+ /warn:4 "/out:$exe" (Join-Path $here 'FixtureGenerator.cs')
if ($LASTEXITCODE -ne 0) { throw 'Fixture generator compilation failed.' }
$arguments = @($here)
if ($Large) { $arguments += '--large' }
& $exe $arguments
if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed.' }
