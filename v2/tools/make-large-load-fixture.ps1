# Makes a generated MIDI file whose LOAD needs more than 2 GiB of memory in the 32-bit builds (about 104 million events,
# peak about 2.7 GiB), for checking that the large-address-aware 32-bit player loads it (DD-060). Not a user file.
#   .\v2\tools\make-large-load-fixture.ps1 -Silent                # default: only silent events (controllers 20-31), safe on any output
#   .\v2\tools\make-large-load-fixture.ps1 -From <large-8m.mid>   # the 8-million-event test file repeated 13 times (it HAS notes: sound)
# Options: -Repeats N (default 13)   -Out <file> (default v2\out\fixtures-large\large-13x-silent.mid or large-13x.mid)
# -Silent writes 16 tracks x 500,000 events x Repeats, every event a Control Change 20..31 (undefined controllers: no sound on
# any synthesizer), 100 events per tick (about 1 ms at the default tempo), so the song lasts about 17 minutes and a Play
# of it is a flood of silent messages. -From repeats the 16 tracks of the given file (the method of footprint-measure.ps1
# -Mode Capacity; the 8M file is made by v2\harness\fixtures\generate.ps1 -Large and is not in the repository).
[CmdletBinding()]
param(
    [switch]$Silent,
    [string]$From = '',
    [int]$Repeats = 13,
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Silent -and -not $From) { $Silent = [switch]$true }
if (-not $Out) { $Out = if ($Silent) { 'v2\out\fixtures-large\large-13x-silent.mid' } else { 'v2\out\fixtures-large\large-13x.mid' } }
$dir = Split-Path -Parent $Out
if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$fullOut = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }

if ($Silent) {
    $eventsPerTrack = 500000
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Capacity = 4 * $eventsPerTrack + 16
    $body.AddRange([byte[]](0x00, 0xB0, 0x14, 0x00))          # delta 0, Control Change 20 on channel 1 (sets the running status)
    for ($i = 1; $i -lt $eventsPerTrack; $i++) {
        $delta = if ($i % 100 -eq 0) { 1 } else { 0 }
        $body.Add([byte]$delta); $body.Add([byte](20 + ($i % 12))); $body.Add([byte]($i % 128))   # running status: data bytes only
    }
    $body.AddRange([byte[]](0x00, 0xFF, 0x2F, 0x00))          # end of track
    $trackBody = $body.ToArray()
    $length = $trackBody.Length
    $chunkHeader = [byte[]](0x4D, 0x54, 0x72, 0x6B, [byte](($length -shr 24) -band 255), [byte](($length -shr 16) -band 255), [byte](($length -shr 8) -band 255), [byte]($length -band 255))
    $tracks = 16 * $Repeats
    $header = [byte[]](0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, [byte]($tracks -shr 8), [byte]($tracks -band 255), 0x01, 0xE0)
    $fs = [IO.File]::Create($fullOut)
    try {
        $fs.Write($header, 0, 14)
        for ($t = 0; $t -lt $tracks; $t++) { $fs.Write($chunkHeader, 0, 8); $fs.Write($trackBody, 0, $length) }
    }
    finally { $fs.Close() }
    ('{0}: {1} tracks, {2:N0} million silent events, {3:N0} MB' -f $Out, $tracks, ($tracks * $eventsPerTrack / 1e6), ((Get-Item $fullOut).Length / 1MB))
    return
}

$src = (Resolve-Path $From).Path
$b = [IO.File]::ReadAllBytes($src)
$tracks = ($b[10] -shl 8) + $b[11]
if ($tracks * $Repeats -gt 65535) { throw "too many tracks: $($tracks * $Repeats)" }
$body = New-Object byte[] ($b.Length - 14)
[Array]::Copy($b, 14, $body, 0, $body.Length)
$header = $b[0..13]
$total = $tracks * $Repeats
$header[10] = [byte]($total -shr 8)
$header[11] = [byte]($total -band 255)
$fs = [IO.File]::Create($fullOut)
try {
    $fs.Write($header, 0, 14)
    for ($i = 0; $i -lt $Repeats; $i++) { $fs.Write($body, 0, $body.Length) }
}
finally { $fs.Close() }
('{0}: {1} tracks, about {2:N0} million events, {3:N0} MB (HAS NOTES: sound)' -f $Out, $total, (8.00016 * $Repeats), ((Get-Item $fullOut).Length / 1MB))
