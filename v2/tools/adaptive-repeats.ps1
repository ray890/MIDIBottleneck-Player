# Shared rule for "how many rounds does a measurement need" (dot-source this file: . "$PSScriptRoot\adaptive-repeats.ps1").
#
# Every measurement tool starts with 3 rounds and adds ONE round at a time, up to 7, only while the answer is unclear.
# "Unclear" is defined here and nowhere else:
#   comparison of two sets of numbers (A against B): unclear while the two medians differ by less than the LARGER of
#       3 % of the larger median, the spread (max - min) of A, the spread of B;
#   a single set of numbers (no comparison): unclear while its spread (max - min) is more than 10 % of its median.
# An explicit round count given by the caller (-Rounds / -Runs on the command line) always wins: the tool then runs
# exactly that many rounds, as before, so old command lines stay reproducible.
#
# A tool calls Get-AdaptiveVerdict after each round from the 3rd on, and prints the sentence from Format-AdaptiveNote
# (rounds used and why) once it stops.

$script:AdaptiveMinRounds = 3
$script:AdaptiveMaxRounds = 7
$script:AdaptiveCloseFraction = 0.03
$script:AdaptiveSpreadFraction = 0.10

function Get-AdaptiveMedian([double[]]$Values) {
    $s = @($Values | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    $m = [int][math]::Floor($s.Count / 2)
    if ($s.Count % 2) { return [double]$s[$m] }
    return ([double]$s[$m - 1] + [double]$s[$m]) / 2
}

function Get-AdaptiveSpread([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $mm = $Values | Measure-Object -Minimum -Maximum
    return [double]($mm.Maximum - $mm.Minimum)
}

# -Samples: one hashtable, name -> numbers measured so far (one number per round or per play).
# -Pairs:   comparisons being made, each @('nameA','nameB'); names not listed in any pair are judged on their own spread.
# -Done:    rounds finished so far.   -Max: ceiling (default 7).
# Returns an object: Extend ($true = run one more round), Reason (text), Unclear (list of what is unclear).
function Get-AdaptiveVerdict {
    param([hashtable]$Samples, [object[]]$Pairs = @(), [int]$Done, [int]$Max = 7)
    $unclear = New-Object System.Collections.ArrayList
    $inPair = @{}
    foreach ($p in $Pairs) {
        $inPair[$p[0]] = $true; $inPair[$p[1]] = $true
        $a = [double[]]@($Samples[$p[0]]); $b = [double[]]@($Samples[$p[1]])
        if ($a.Count -eq 0 -or $b.Count -eq 0) { continue }
        $ma = Get-AdaptiveMedian $a; $mb = Get-AdaptiveMedian $b
        $diff = [math]::Abs($ma - $mb)
        $limit = [math]::Max([math]::Max($script:AdaptiveCloseFraction * [math]::Max([math]::Abs($ma), [math]::Abs($mb)), (Get-AdaptiveSpread $a)), (Get-AdaptiveSpread $b))
        if ($diff -lt $limit) { [void]$unclear.Add(("{0} vs {1}: medians differ by {2:N3}, less than the noise limit {3:N3}" -f $p[0], $p[1], $diff, $limit)) }
    }
    foreach ($name in $Samples.Keys) {
        if ($inPair.ContainsKey($name)) { continue }
        $v = [double[]]@($Samples[$name])
        if ($v.Count -lt 2) { continue }
        $med = Get-AdaptiveMedian $v
        $spread = Get-AdaptiveSpread $v
        if ($med -ne 0 -and $spread -gt $script:AdaptiveSpreadFraction * [math]::Abs($med)) {
            [void]$unclear.Add(("{0}: spread {1:N3} is more than 10 % of the median {2:N3}" -f $name, $spread, $med))
        }
    }
    $extend = ($unclear.Count -gt 0) -and ($Done -lt $Max)
    if ($unclear.Count -eq 0) { $reason = "clear after $Done round(s)" }
    elseif ($extend) { $reason = "unclear after $Done round(s), adding one: " + ($unclear[0]) + $(if ($unclear.Count -gt 1) { " (+$($unclear.Count - 1) more)" } else { '' }) }
    else { $reason = "still unclear at the ceiling of $Max rounds: " + ($unclear -join '; ') }
    [pscustomobject]@{ Extend = $extend; Reason = $reason; Unclear = @($unclear) }
}

# The sentence every tool prints: how many rounds were used and why.
function Format-AdaptiveNote {
    param([int]$Used, [bool]$Explicit, [string]$Reason, [string]$Name = '')
    $prefix = if ($Name) { "$Name`: " } else { '' }
    if ($Explicit) { return "${prefix}rounds used: $Used (fixed by the command line; adaptive extension off)" }
    return "${prefix}rounds used: $Used (adaptive 3 to 7; $Reason)"
}

# -Explicit: $true when the caller typed the count.  Returns the number of rounds to start with and the ceiling.
function Get-AdaptivePlan {
    param([bool]$Explicit, [int]$ExplicitCount = 3)
    if ($Explicit) { return [pscustomobject]@{ Min = $ExplicitCount; Max = $ExplicitCount; Explicit = $true } }
    return [pscustomobject]@{ Min = $script:AdaptiveMinRounds; Max = $script:AdaptiveMaxRounds; Explicit = $false }
}
