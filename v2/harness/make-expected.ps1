# Writes the v1 reference traces into v2\harness\expected\:
#   <name>.trace              load + index traces (x64 harness; x86 must be identical)
#   flow\<name>.ptr4.trace    load-flow traces from the x86 harness (32-bit pointer size)
#   flow\<name>.ptr8.trace    load-flow traces from the x64 harness (64-bit pointer size)
#   decimal-ops.txt           .NET decimal primitive vectors (x64; x86 must be identical)
#   models\<family>.trace     model traces (x64 harness; x86 must be identical)
# With -Large, also the large-fixture summary traces (fixtures\large\large-8m.mid)
# and models-large\routing-large.trace (fixtures\large\routing-held-*.mid).
# With -Engine, also rewrites expected\engine (v1's engine recording digests) by running make-engine-expected.ps1
# on the newest full spike run (run the spike first: v1-reference\spike\run-spike.ps1 -AllValid -Runs 2 -Tag engine-expected,
# or pass -EngineRunRoot DIR); see make-engine-expected.ps1.
param([switch]$Large, [switch]$Engine, [string]$EngineRunRoot = '')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$fixtures = Join-Path $here 'fixtures'
$expected = Join-Path $here 'expected'
$flow = Join-Path $expected 'flow'
$tmp = Join-Path $repo 'v2\out\v1-traces'
$models = Join-Path $expected 'models'
# Model families, in Milestone 3 group order (V1ModelHarness*.cs).
$modelFamilies = @('rate', 'queue', 'ordered', 'pernote', 'delayed', 'attack', 'outputrate', 'analysis', 'routing')
New-Item -ItemType Directory -Force -Path $expected, $flow, $tmp, $models | Out-Null
$x64 = Join-Path $repo 'v2\out\v1-reference\x64\v1-reference.exe'
$x86 = Join-Path $repo 'v2\out\v1-reference\x86\v1-reference.exe'

function Strip([string]$path) {
    ([System.IO.File]::ReadAllText($path) -split "`n" | Where-Object { -not $_.StartsWith('#') }) -join "`n"
}
function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe $($arguments -join ' ') failed" }
}

$files = Get-ChildItem -LiteralPath $fixtures -File | Where-Object { $_.Extension -in '.mid', '.rmi' } | Sort-Object Name
$different = 0
foreach ($file in $files) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($file.Name) + $file.Extension.Replace('.', '-')
    $target = Join-Path $expected "$stem.trace"
    $other = Join-Path $tmp "x86-$stem.trace"
    Run $x64 @('dump', $file.FullName, '--out', $target)
    Run $x86 @('dump', $file.FullName, '--out', $other)
    if ((Strip $target) -ne (Strip $other)) { Write-Host "x86/x64 DIFFER: $($file.Name)"; $different++ }
    Run $x86 @('flow', $file.FullName, '--out', (Join-Path $flow "$stem.ptr4.trace"))
    Run $x64 @('flow', $file.FullName, '--out', (Join-Path $flow "$stem.ptr8.trace"))
}

Run $x64 @('decimal-ops', '20261012', (Join-Path $expected 'decimal-ops.txt'))
Run $x86 @('decimal-ops', '20261012', (Join-Path $tmp 'x86-decimal-ops.txt'))
if ((Strip (Join-Path $expected 'decimal-ops.txt')) -ne (Strip (Join-Path $tmp 'x86-decimal-ops.txt'))) {
    Write-Host 'x86/x64 DIFFER: decimal-ops'; $different++
}

foreach ($family in $modelFamilies) {
    $target = Join-Path $models "$family.trace"
    $other = Join-Path $tmp "x86-model-$family.trace"
    Run $x64 @('models', $family, $fixtures, $target)
    Run $x86 @('models', $family, $fixtures, $other)
    if ((Strip $target) -ne (Strip $other)) { Write-Host "x86/x64 DIFFER: model $family"; $different++ }
}

if ($Large) {
    $big = Join-Path $fixtures 'large\large-8m.mid'
    $target = Join-Path $expected 'large-8m-mid.summary.trace'
    Run $x64 @('dump', $big, '--summary', '--out', $target)
    Run $x86 @('dump', $big, '--summary', '--out', (Join-Path $tmp 'x86-large.trace'))
    if ((Strip $target) -ne (Strip (Join-Path $tmp 'x86-large.trace'))) { Write-Host 'x86/x64 DIFFER: large'; $different++ }
    Run $x86 @('flow', $big, '--summary', '--out', (Join-Path $flow 'large-8m-mid.ptr4.summary.trace'))
    Run $x64 @('flow', $big, '--summary', '--out', (Join-Path $flow 'large-8m-mid.ptr8.summary.trace'))
    # Routing 1,000,000-held-notes limit (fixtures\large\routing-held-*.mid): a small trace, x86 must agree.
    $largeModels = Join-Path $expected 'models-large'
    New-Item -ItemType Directory -Force -Path $largeModels | Out-Null
    $target = Join-Path $largeModels 'routing-large.trace'
    Run $x64 @('models', 'routing-large', (Join-Path $fixtures 'large'), $target)
    Run $x86 @('models', 'routing-large', (Join-Path $fixtures 'large'), (Join-Path $tmp 'x86-model-routing-large.trace'))
    if ((Strip $target) -ne (Strip (Join-Path $tmp 'x86-model-routing-large.trace'))) { Write-Host 'x86/x64 DIFFER: model routing-large'; $different++ }
}
Write-Host ("{0} fixtures: load/index and flow traces written; {1} model families; x86/x64 differences: {2}" -f $files.Count, $modelFamilies.Count, $different)
if ($different -ne 0) { exit 1 }
if ($Engine) {
    if ($EngineRunRoot -ne '') { & (Join-Path $here 'make-engine-expected.ps1') -RunRoot $EngineRunRoot }
    else { & (Join-Path $here 'make-engine-expected.ps1') }
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
