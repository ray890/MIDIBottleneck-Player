# Self-check of the shared round-count rule (adaptive-repeats.ps1) on made-up numbers. Runs in a second; starts nothing.
#   .\v2\tools\adaptive-repeats-selfcheck.ps1      # prints one line per case and "ADAPTIVE-REPEATS: PASS" or "FAIL"
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'adaptive-repeats.ps1')
$fails = 0
function Check([string]$name, $verdict, [bool]$expectExtend) {
    $ok = ($verdict.Extend -eq $expectExtend)
    if (-not $ok) { $script:fails++ }
    Write-Host ("{0,-4} {1,-58} extend={2}  ({3})" -f $(if ($ok) { 'ok' } else { 'FAIL' }), $name, $verdict.Extend, $verdict.Reason)
}
# comparison: medians 100 vs 120, spreads 2 and 3 -> clearly different -> stop at 3
Check 'pair clearly different' (Get-AdaptiveVerdict -Samples @{ A = @(100, 101, 102); B = @(119, 120, 122) } -Pairs @(, @('A', 'B')) -Done 3) $false
# medians 100 vs 101 (1 %), below 3 % -> close -> extend
Check 'pair within 3 percent' (Get-AdaptiveVerdict -Samples @{ A = @(100, 100.2, 100.1); B = @(101, 101.1, 100.9) } -Pairs @(, @('A', 'B')) -Done 3) $true
# medians 100 vs 108 (8 %), but A spreads 12 (noise bigger than the gap) -> extend
Check 'pair gap smaller than the spread of one set' (Get-AdaptiveVerdict -Samples @{ A = @(94, 100, 106); B = @(107, 108, 109) } -Pairs @(, @('A', 'B')) -Done 3) $true
# same at the ceiling: no more rounds, reason says so
$atMax = Get-AdaptiveVerdict -Samples @{ A = @(94, 100, 106); B = @(107, 108, 109) } -Pairs @(, @('A', 'B')) -Done 7
Check 'ceiling of 7 stops the extension' $atMax $false
if ($atMax.Reason -notlike '*ceiling*') { $fails++; Write-Host 'FAIL reason at the ceiling does not say so' }
# single set: spread 5 % of the median -> clear; spread 20 % -> extend
Check 'single set, spread 5 percent' (Get-AdaptiveVerdict -Samples @{ S = @(98, 100, 103) } -Done 3) $false
Check 'single set, spread 20 percent' (Get-AdaptiveVerdict -Samples @{ S = @(90, 100, 110) } -Done 3) $true
# a pair member is not also judged as a single set (A noisy but far from B -> stop)
Check 'noisy sets far apart stop' (Get-AdaptiveVerdict -Samples @{ A = @(50, 100, 150); B = @(900, 1000, 1100) } -Pairs @(, @('A', 'B')) -Done 3) $false
# plan: explicit count wins, default is 3..7
$e = Get-AdaptivePlan -Explicit $true -ExplicitCount 5
if ($e.Min -ne 5 -or $e.Max -ne 5 -or -not $e.Explicit) { $fails++; Write-Host 'FAIL explicit plan' } else { Write-Host 'ok   explicit count is exact (5..5)' }
$d = Get-AdaptivePlan -Explicit $false
if ($d.Min -ne 3 -or $d.Max -ne 7) { $fails++; Write-Host 'FAIL default plan' } else { Write-Host 'ok   default plan is 3..7' }
# simulated loop: A/B 100 vs 104 with noise +-1 -> stops by itself at 3 (gap 4 > noise 2 and 3 %)
$rng = New-Object System.Random 7
$a = @(); $b = @(); $n = 0
while ($true) {
    $n++; $a += 100 + $rng.NextDouble() * 2 - 1; $b += 104 + $rng.NextDouble() * 2 - 1
    if ($n -lt 3) { continue }
    $v = Get-AdaptiveVerdict -Samples @{ A = $a; B = $b } -Pairs @(, @('A', 'B')) -Done $n
    if (-not $v.Extend) { break }
}
if ($n -ne 3) { $fails++; Write-Host "FAIL simulated clear case used $n rounds" } else { Write-Host 'ok   simulated clear case stops at 3 rounds' }
Write-Host (Format-AdaptiveNote -Used 4 -Explicit $false -Reason 'example')
Write-Host (Format-AdaptiveNote -Used 5 -Explicit $true -Reason '')
Write-Host $(if ($fails -eq 0) { 'ADAPTIVE-REPEATS: PASS' } else { "ADAPTIVE-REPEATS: FAIL ($fails)" })
exit $(if ($fails -eq 0) { 0 } else { 1 })
