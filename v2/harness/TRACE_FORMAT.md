# Canonical trace format (version 1)

A trace is a deterministic, text-diffable description of what a MIDIBottleneck implementation produced for one input. The v1 reference harness (`v1-reference/`) and the v2 dump tool (`v2/tools/midibn-dump`) both write it; the parity runner compares them byte for byte after removing comment lines; a difference from v1 passes only when a rule explains it ("Difference rules" below).

## Encoding rules

- ASCII only, `\n` line endings, no trailing spaces, final line `[end]`.
- Lines starting with `#` are informational (producer, build, timestamps) and are **ignored** by parity comparison. Nothing else may differ between implementations, unless a difference rule explains it.
- Fields are separated by one space. Integers are decimal, signed where meaningful, no thousands separators. Booleans are `0`/`1`. Hex is uppercase without prefix.
- Strings are wrapped in `"…"`. Inside, bytes 0x20–0x7E are literal except `"` → `\"` and `\` → `\\`; every other UTF-16 code unit is written as `\uXXXX`. Strings are compared as UTF-16 code units because v1 stores track names that way (a truncation can leave a lone surrogate).
- Errors are written as `status error <ExceptionType> "<message>"`, where `<ExceptionType>` is the unqualified .NET exception type v1 raised (`InvalidDataException`, `EndOfStreamException`, `OverflowException`, `IOException`, …) and the message is the user-visible text. v2 writes the same type; the message is v1's except where the register replaced a raw .NET text by a plain sentence (DD-003, DD-012).
- Sections appear in the order below. A section that cannot be produced (for example `[tempo]` after a load error) is omitted entirely.

## Header

```
trace-format 1
mode full|summary
```

`summary` mode (used for the large performance fixture) replaces the per-line bodies of `[tempo]` and `[events]` with a digest line (see below). Everything else is identical.

## `[file]`

```
size <bytes>                 physical file size
container smf|rmid|error     result of the container probe (bare SMF, RIFF/RMID data chunk)
```

## `[preflight]`

Result of v1 `MidiLargeFilePreflight.Scan` (always run, independent of file size) and `Estimate` for both pointer sizes, so x86 and x64 traces are identical.

```
status ok | status error <Type> "<message>"
format <n>
tracks <n>
division <ticks per quarter>
dispatchable-events <n>
note-ons <n>
final-long-payload-bytes <n>
provisional-payload-bytes <n>
tempo-changes <n>
source-value-entries <n>
chase-state-entries <n>
chase-parameter-value-entries <n>
running-status-repairs <n>
routing-lanes <n>
routing-patch-state-events <n>
routing-patch-timeline-upper-bound <n>
routing-generated-state-upper-bound <n>
track <index> events <n> payload-bytes <n> tempo-changes <n>      (one per track)
source-slots <144 counts: channel-major, 9 attributes per channel>
routing-note-lanes-by-channel <16 counts>
routing-patch-state-by-channel <16 counts>
estimate ptr<4|8> retained <n> peak-low <n> peak-high <n> provisional <n> merge-workspace <n> names <n> routing-retained <n> routing-peak <n> retained-with-routing <n> routing-peak-total <n> warning-threshold <n> architecture-risk <0|1> requires-warning <0|1>
requires-preflight ptr4 <0|1> ptr8 <0|1>
```

The `estimate` and `requires-*` lines appear only when `status ok`; `requires-preflight` depends only on file size and appears whenever `[file]` succeeded.

## `[song]`

Result of v1 `DirectCompactMidiParser.Load` (the production loader).

```
status ok | status error <Type> "<message>"
format <n>
tracks <n>
division <n>
note-count <n>                         NoteOn with velocity > 0
running-status-repairs <n>
duration-us <n>
events <n>
payload-bytes <n>                      total long-payload bytes in the final store
track <index> name "<s>" name-truncated <0|1> instrument "<s>" instrument-truncated <0|1>
```

## `[tempo]`

The merged tempo map, in application order (tick, track, per-track order), produced with v1's own private track parser and tempo clock (via reflection in the harness):

```
count <n>
t <tick> <time-us> <microseconds-per-quarter> <track> <order>
```

`time-us` is the clock's rounded time at that tick. Tempo meta events with value 0 or length ≠ 3 are not tempo changes (v1 behaviour) and do not appear.

## `[events]`

One line per stored event, in final store order:

```
count <n>
e <index> <tick> <time-us> <track> <kind> <channel> <status> <length> <data>
```

- `kind`: `NoteOff`, `NoteOn`, `PolyphonicAftertouch`, `ControlChange`, `ProgramChange`, `ChannelAftertouch`, `PitchBend`, `SystemExclusive`, `SystemMessage`.
- `channel`: 0–15, or `-1` for system events.
- `status`: two hex digits.
- `data`: `-` when `length` is 0; for `length` 1–3 the stored bytes as hex (e.g. `903C64`); for longer payloads `sha256:<64 hex>` of the stored bytes (SysEx `F0` prefix included, as stored). v1 stores SysEx of 1–3 bytes as zero bytes and its trace records exactly that; v2 stores the real bytes (improvement DD-001, `v2/docs/DIFFERENCES_FROM_V1.md`), so those lines are explained by a rule in `expected/differences-from-v1.txt`.

## `[source-index]` (Milestone 2)

The source-value index (v1 `ChannelSourceValueIndex`) as built by the loader's merge pass.

```
count <total entries>
slot <channel> <attribute> <n>        attribute: BankMsb BankLsb Program Volume Expression Pan Sustain PitchBend Aftertouch
v <event-index> <value>               n entries, in index order
```

Only non-empty slots appear, in channel-major attribute order.

## `[chase-index]` (Milestone 2)

The whole-state chase index (v1 `MidiStateChaseIndex`).

```
entries <EntryCount> parameter-series <ParameterSeriesCount>
state <channel> <slot> <n>            slot 0-119 controller, 120 Program, 121 pitch bend, 122 channel pressure
reset <channel> <n>                   Reset All Controllers positions
selector <channel> <n>                RPN/NRPN selector history (-1 = null selector)
parameter <channel> <key> <n>         per-parameter data values, keys ascending (NRPN keys have bit 0x4000)
v <event-index> <value>
```

All `state` slots first (channel-major), then `reset`, `selector`, and `parameter` groups; empty series are omitted.

## Index query sections (Milestone 2; full mode only)

These record what every query returns at every event index. Each line lists only what changed since the previous line, so the complete state at every index is reconstructed by replaying the lines; an index with no change has no line.

- `[source-query]`: `q <index> <time-us> <channel>.<attribute>=<value|-> ...` — `TryGetLatest(channel, attribute, time)` at each event's own time, for all 144 slots (`-` = no value). Initial state: all `-`.
- `[chase-readouts]`: `r <exclusive> <channel>.<field>=<value> ...` — `CreateAttributeReadouts(exclusive)` for every exclusive index 0…count; fields `BankMsb BankLsb Program Volume Expression Pan Sustain PitchBend ChannelPressure`. Initial state: v1's unknown values (-1, pitch bend -2147483648).
- `[chase-messages]`: `c <exclusive> <message-count> sha256:<digest>` whenever the message list of `CreateMessages(exclusive, null, null)` differs from the previous index; the digest is SHA-256 of the list text, and `final <list text>` gives the full list at exclusive = count. List text: messages separated by one space, each `<Kind>:<hex bytes>` (for example `ControlChange:B00764`); an empty list is `-` (the initial value, which therefore has no `c` line).
- `[chase-messages-filtered]`: the same for `CreateMessages(exclusive, routing, overrides)` with the **filtered chase configuration**: channels with `channel % 3 == 1` disabled; forced values on `channel % 4 == 0`: Volume 100, Program 5, PitchBend 0; on `channel % 4 == 1`: BankMsb 1, BankLsb 2, Pan 64, Expression 90, Sustain 1, Aftertouch 10; on `channel % 4 == 3`: PitchBend -8192, Aftertouch 0, Volume 0.

In summary mode the bodies of `[source-index]` and `[chase-index]` are digested like `[events]`, and the query sections are omitted (the large performance fixture would need billions of queries); the committed fixtures cover the queries.

## Flow traces (`kind flow`, Milestone 2)

A separate trace per fixture and CLR pointer size records v1 `MidiFileParser.LoadWithPreflight`: progress reports, the large-file decision, and cancellation outcomes. The v1 harness can only use its own pointer size, so `expected/flow/<name>.ptr4.trace` comes from the x86 harness and `.ptr8.trace` from the x64 harness; v2 produces both on every build.

```
trace-format 1
kind flow
mode full|summary
pointer-size 4|8
[scenario <name>]
p "<stage>" <completed-tracks> <total-tracks> <overall-completed> <overall-total> <stage-completed> <stage-total>
reports <n> decision-requested <0|1>
outcome ok events <n> notes <n> duration-us <n> | outcome declined | outcome error <Type> "<message>"
[cancel-sweep <name>]
k <K> reports <n> decision-requested <0|1> outcome ...
[end]
```

- Scenarios (force-preflight, force-warning, decision): `natural` (0, 0, accept), `preflight` (1, 0, accept), `warning-decline` (1, 1, decline), `warning-accept` (1, 1, accept), `warning-accept-cancel` (1, 1, accept and request cancellation inside the decision).
- `p` lines are the progress values exactly as v1's `MidiLoadProgress` holds them (after its clamping and the flow's rescaling). In summary mode they are digested.
- Cancellation is scripted, never timed: in a sweep, `K = 0` cancels before the call and `K >= 1` requests cancellation inside the K-th progress report; `K` runs from 0 to the scenario's report count + 1. `reports` counts every report delivered before the run ended. Sweeps run for `natural` and `warning-accept` in full mode only.
- Cancellation surfaces as `outcome error OperationCanceledException "The operation was canceled."`.

## Decimal vectors

- `expected/decimal-vectors.txt` (Milestone 1): tempo-clock sequences (`D` lines), compared as values.
- `expected/decimal-ops.txt` (Milestone 2): primitive operations compared **bit for bit** as `<lo> <mid> <hi> <scale>`: `M a b` multiply, `V lo mid hi divisor` scale-0 division, `A <a bits> <b bits>` addition (or `OverflowException`), `R <bits>` `ToInt64(Round(v, 0, AwayFromZero))` (or `OverflowException`). Targeted boundary values (mantissas near 2^96 at many scales, powers of ten, .5 ties, the Int64 limit) come first, then seeded random values.

## Summary digests

In `summary` mode the bodies of `[tempo]` and `[events]` are replaced by:

```
count <n>
digest sha256 <64 hex>
```

The digest is SHA-256 over the exact full-mode body lines (each followed by `\n`), excluding the `count` line. A summary trace therefore matches if and only if the full traces would.

## Model traces (Milestone 3)

`expected/models/<family>.trace`, written by `v1-reference.exe models <family> <fixture-dir> <out>` (`V1ModelHarness*.cs`), which drives v1's own model classes, unchanged. These files are scripts as well as expected results: v2 (`midibn-dump --models`, and the parity runner) reads only their `>` lines, replays each call against the v2 model, and writes the same format. The two outputs must be identical apart from differences explained by the difference rules, as for other traces (there are none for models so far).

```
model-trace 1
family <rate|queue|ordered|pernote|delayed|attack|outputrate>
[<section>]
> <op> <args...>       one call, every input spelled out
= <result>             its return value (absent for void calls)
! <Type> <param|-> <first message line>   the .NET exception it threw instead
~ <state>              model state after the call (private fields, read by reflection on v1)
[end]
```

- `> fixture <name>` loads a fixture with the production loader and answers `= events <n> duration <us>`. Event arguments are tokens: `e<index>` (the song store's event), `e<index>@<channel>` (`WithOutputChannel`), `e<index>#<n>` (`WithLogicalEventIndex`), `x` (the default, invalid view).
- An emitted or stored event is printed as `<eventIndex>:<tick>:<us>:<track>:<channel>:<STATUS>:<length>[:<bytes>]`; bytes (at most three) are printed only for channel messages, because no model reads system or SysEx bytes. `invalid` is the default view.
- A work item is `<generation>/<kind>/<sourceIndex>/<us>/<protection>/<occurrenceId>/<event|->` (kinds and protections as v1 enum numbers).
- State lines print every scalar field and a digest (`d<16 hex>`, FNV-1a 64 of an exact text) of each model's arrays and linked lists: per-pitch arrays, occurrence pools walked in list order with node numbers, queue contents, pacer history. Node numbers are compared, so a change in how v2's free lists allocate shows as a difference that needs a rule. In the `pernote` and `delayed` families a gate's state line is followed by ` | ` and the adaptive clock's state.
- The driver chooses calls with a seeded generator and from v1's results (as v1 Analysis does); that choice is never re-derived by v2.
- Not covered here: Analysis bucketing (Milestone 4), the live engine (Milestone 5), and `OrderedFiniteAdmissionCore.DispatchDueClockTicks`, which converts through the engine's clock frequency.

### Configuration matrix

| Family (models) | Fixtures | Settings, including zero/minimum/maximum/boundary values | Live setting changes | Protected/safety traffic, same-time batches, overload |
|---|---|---|---|---|
| `rate` (service durations, progressive pacer, adaptive interval clock) | models-notes, f0-all-message-types, long-sysex-segment-crossing | Processing time −5, 0, 1, 333, 10⁶, Int64.Max; bitrate −1, 0, 1, 31,250, 9,999,999, Int64.Max; events/sec −1, 0, 1, 3, 7, 3,000, 9,999,999, 10⁷, Int64.Max; byte counts −1 … Int32.Max × bitrates (checked overflow). Pacer: rates 0, 1, 2, 19, 20, 21, 39, 40, 41, 1,000, 3,000, 9,999,999 × units 10⁶, 10⁷, 1, 7 × minimum burst 1, 2, 64, plus invalid rates/units/bursts, burst Int64.Max, units Int64.Max; timestamps from 0, negative, and Int64.Max − 3 (exhaustion). Adaptive interval −1, 0, 1, 1,000, 5,208, Int64.Max/3, Int64.Max; exact close/effective boundary cases | A new clock every 40 events continues the same stream; look-ahead copies | Same-timestamp consume batches until refused; backwards timestamps (monotonic error); reservations completed and cancelled with none outstanding |
| `queue` (pending queue, forward-drop queue, complete-note tracker, musical priority) | models-notes, models-dense, f0-all-message-types, index-chase-rich | Pending capacity 0 (error), 1, 2, 8, 64 × policies newest, oldest, oldest complete note, least important note, head-only (waiting time); service 2,000/9,000/40,000 events/sec. Forward-drop capacity 0 (error), 1, 2, 3, 16, 10⁶, Int32.Max × service 0, 100, 5,208, 50,000 µs, negative service, completion overflow near Int64.Max. Priority: 400 candidate sets with ties in every key, track −1/Int32.Min/Int32.Max, channel −1/9, cursor 0/Int32.Max, count 0 and count > capacity | Note index switched on and off mid-stream (both directions), clear mid-stream; capacity crossing 256-entry segments | Safety controllers (CC64 < 64, CC120–127) admitted past capacity; dense overload with constant eviction; duplicate attack index (v1's partial-state exception) |
| `ordered` (generation queue, finite admission core) | models-notes, models-dense, index-chase-rich | Capacity 1, 3, 16 (and 1, 5, 31 alternating); every overflow policy and an invalid one; 1–4 live generations; capture decisions none, channel mask, mask plus routing, bad identity, invalid view; negative/Int64.Max times and limits | Capacity alternates every 40 steps; policy switches halfway; seeks append generations with chase prefixes | Chase prefix and safety traffic as soft excess; protected releases; preservation barriers stalling waiting-time repair; clear/catch-up with filtered and dropped skipped work |
| `pernote` (Per-note interval gate + adaptive clock) | models-notes, models-dense, f1-v1-test-midi, many-tracks-ties, index-chase-rich | Interval 0, 1, 1,000, 2,604, 5,208, 20,000, 250,000, Int64.Max/2; polyphony limit off, 1, 2, 3, 5, 128; adaptive on/off; invalid interval/limit; 127-entry buffer | A new gate (different settings) halfway through the stream | Channel retirement (including −1 and 16) at scripted points; chords and the 48-note burst; overlapping same-key notes across tracks |
| `delayed` (Delayed NoteOff + adaptive clock) | same as `pernote` | D 0, 1, 4,000, 20,000, 52,080, 160,000 (recovery capped at 40,000), 200,000, Int64.Max/3; polyphony limit off, 1, 3, 6, 128; adaptive on/off; invalid values | A new gate halfway | Sustain pedal (63/64 boundary), note-terminating controllers (CC120, 123–127) retiring channels, exact-time source/boundary ties |
| `attack` (Note start rate gate) | models-notes, models-dense, many-tracks-ties, f0-all-message-types | Rates 0, 1, 3, 7, 20, 21, 40, 100, 1,000, 9,999,999; −1 and 10⁷ (errors) | A new gate halfway | Releases of rejected attacks suppressed; channel retirement; tick not completed (error) |
| `outputrate` (Output rate gate) | as `attack` | Rates 0, 1, 5, 20, 21, 41, 100, 1,000, 3,125, 9,999,999; units 10⁶ and 10⁷; invalid rates and units | A new gate halfway | Analysis mode decides and dispatches at ideal source time. Playback mode decides at a scripted wall time (source time plus 0–750 µs wake latency) and records each dispatch at a scripted send end (320 µs, or the message's bytes at 31,250 bit/s); some dispatches are flagged as required releases; exceptional (cleanup) dispatches; channel retirement |

## Analysis and routing traces (Milestone 4)

Two more model-trace families use the same `>` / `=` / `!` / `~` line kinds and the same replay rule (v2 replays the `>` lines; the script decides what is observed, never the result). Drivers: `V1ModelHarness.Analysis.cs` and `V1ModelHarness.Routing.cs`; v2 replay: `core/src/models/replay_analysis.cpp`. Both run with the invariant culture (v1's dialog formats `N0` with the current culture; see ARCHITECTURE.md §9). Doubles are written as their IEEE-754 bit patterns (16 lower-case hex digits). Text that may hold non-ASCII (track names, preview text) is written with every character outside 0x20-0x7E, and the backslash, as `\uXXXX`.

**`analysis` family ops.**
- `> bucket-count <duration> <bucket>` (`CalculateBucketCount`), `> default-resolution <duration>`.
- `> analyze <bucket> <config>`: `WorkloadAnalyzer.Analyze` with the song's base-workload cache (two results, 2,000,000 buckets in total, oldest evicted first). `> cache-clear` drops the cache (a new song also starts with an empty one). `> uncached <bucket> <config>`: `AnalyzeUncached`.
- `> project <pernote|delayed|attack|outputrate> <bucket> <config>`: a gate projection over a copy of the base workload, with v1's emission observer (`= emitted <count> <digest> <first four events@time>`).
- Results (`=` lines): `state` (projection state, HasGateProjection, HasQueueProjection, failure reason, `ready <bucket>` when the workload-ready callback ran), `totals`, `rates` (average, peak events/bytes, service capacities, gate peak), `predicted`, `gate` counters (including override forced/filtered counts), `types` (kind:events:bytes), `buckets <count> <digest> <first six non-empty buckets>` (each bucket `events,bytes,cluster,service,occupancy,age,dropped,clears`), `gate-buckets null|<count> <digest> <sample>`.
- State line (`~`, written even after an exception): `progress <count> <digest> <stage*runs/...>` over every progress report (`stage|overall‰|stage‰`).
- Configuration token: `null`, or `sim=0|1,mode=0..2,proc=,bit=,eps=,ql=<on>:<limit>,qa=<on>:<µs>,pol=0..5,pn=,dn=,scale=,ip=<on>:<limit>,na=,or=,paced=,aq=,ovr=-|none|<c.a=v;...>` (`ovr=-` no projection snapshot; `none` a snapshot with no forced value; otherwise forced values by channel and attribute index).

**`routing` family ops.**
- `> route tier=0..2,cap=<n>,perc=0|1,usable=<hex mask>,manual=-|<track.channel=dest;...>`: v1's `TrackRoutingForm` (created, never shown) over `TrackRoutePlanner.Compile`. Results: header (valid, explicit-apply, highest tier, counts), `error` lines, one `lane` line per profile (names, counts, first/last, dynamic-state flags and signature, representative patch, dominance, patch history size, sounding-interval digest, patch-timeline digest and first eight changes), `assign` lines (destination, tier, eight flags manual/representative/conflict/overlap/handoff/percussion/retimbred/omitted, warnings), `dest` lines, `stats`, `event-destinations <count> <digest> <histogram>`, `generated <sources> <events> <digest> <sample>`, then the dialog: `row` lines (the five cells and the row colour class), every `preview` line, `used` (bold channel captions), `apply`/`confirm` (valid, confirmation required, Apply enabled, status in red, status text; `confirm` after ticking the lossy confirmation), `applied` (published, explicitly confirmed, settings recorded).
- `> routed-view`: `TrackRouteApplication.CreateRoutedSong` for the last route: `= view <count> <digest> lower <index> <sample>` (each event `Ev/<source index>`). A route left over from the previous fixture is applied once to the new song and must be refused.
- `> routed-analyze <bucket> <config>`: `Analyze` over the routed song (its own cache).
- Not traced: the correction selector index (`manual + 1`, a UI mapping for Milestone 6); control visibility (a never-shown form reports every control invisible, so the harness reports v1's formula `valid && RequiresExplicitApply` instead).

**`routing-large` family (held-notes limit).** `V1ReferenceHarness models routing-large <fixtures\large> <out>` runs the `routing` ops (`fixture`, `route`, `routed-view`) over two files that `generate.ps1 -Large` writes next to `large-8m.mid` and that are not committed: `routing-held-1000000.mid` (1,000,000 note-ons all sounding at once, then 1,000,000 note-offs; exactly `TrackRoutePlanner.MaximumActiveAttacks`, accepted) and `routing-held-1000001.mid` (one more, rejected with `! InvalidOperationException - Track-routing analysis exceeded the bounded active-note occurrence limit.`). Each is routed with `tier=0,cap=16` and `tier=2,cap=4`; an accepted route also gets a `routed-view`. The expected trace is small (about 4 KB) and committed as `expected\models-large\routing-large.trace` (outside `expected\models\`, so the XP self-test package, which has no large files, never sees it). `parity.ps1` regenerates it with v1 (x64 and x86) and replays it with every v2 variant (`midibn-parity --large-model <trace> --large-model-fixtures <dir>`) only when both files exist. Timings on the reference PC: v1 x64 about 13 s, x86 about 15 s; v2 x64 6.6 s, x86 and XP 10 s.

### Configuration matrix (Milestone 4)

| Area | Fixtures | Settings | Edge cases |
|---|---|---|---|
| Bucketing and limits | all Analysis fixtures | default resolution, a bounded small bucket (duration/20,000), 1 ms, 25 ms, 100 ms, 10^12 µs | `CalculateBucketCount` at 1,999,999/2,000,000/2,000,001 buckets, bucket 0 and negative, negative and Int64 durations (C# wrap-around); `DefaultResolution` boundaries and overflow; on `tempo-changes.mid` a base of 1,035,031 buckets growing to exactly 2,000,000, a base of 1,811,303 throwing while growing past it, and an immediate 2,070,061-bucket rejection |
| Raw workload | models-notes, models-dense, index-chase-rich, many-tracks-ties, tempo-changes, empty-tracks, sysex-variants, routing-lanes, f1-v1-test-midi | — | cluster classes 2/10/50/100, message types, per-bucket bytes |
| Rate models (service) | same | processing time 0, 1, 7, 250, 1,000, 5,000, 40,000 µs and Int64.Max/2 (overflow); bitrate 1 … 1,000,000; events/sec 0 … 9,999,999 | instantaneous service, Events/sec remainder carried across events |
| Queue pressure | same | unlimited; finite limits 1, 2, 3, 8, 32, 500, 100,000; age limits 1, 900, 5,000, 60,000 µs; every overflow policy; Output-paced on/off; forward-only (no slowdown) with every policy | Drop oldest events and Clear buffer rejected without slowdown; clear-at-completion skipping; complete-note and background-note evictions; protected traffic |
| Gates | same | Per-note and Delayed NoteOff intervals 1, 250, 1,000, 20,000, 250,000 µs (and -5: rejected), traffic scaling on/off, polyphony limit off and -3, 0, 1, 2, 5, 128, 300; Note start and Output rate 0, 1, 5, 30, 1,000, 9,999,999 (and -1, 10,000,000: rejected) | gate-bucket growth past the song end; observer emission sequences |
| Forced-override projection | same | off, a snapshot with no forced value, random forced values, a CC121-conflict set (Expression, Pan, Sustain, Pitch bend, Aftertouch) | filtered source messages, forced messages added to the first gate bucket |
| Routing tiers | routing-lanes, routing-many-lanes, models-notes, index-chase-rich, many-tracks-ties, f0-all-message-types, metadata-names, empty-tracks | exact-safe, GM-family, aggressive; capacity 0 (clamped), 1, 3, 4, 8, 16, 99 (clamped) and random | same-channel preference, GM family and nonzero banks, representative scores, destructive-merge omission, handoffs, state conflicts, same-key overlaps |
| Corrections and policies | same | manual destinations (valid, -1, 16, unusable), more manual destinations than capacity, percussion reserved/any, channel 10 unusable, no usable channel, random usable masks | percussion convention overrides; multiple destinations for one source channel |
| Outcomes | same, plus routing-patches-4096/4097 | — | valid exact-safe plans, lossy plans needing confirmation (confirm and apply), unresolved plans (Apply refused, routed view refused), a route for a different song (refused), the 4,096-distinct-patch limit (accepted) and 4,097 (rejected) |
| Routed view consumers | valid routes above | routed-view event stream and time search; Analysis over the routed song (base workload and one Delayed NoteOff projection with a forced override) | generated Bank/Program events before source events; system messages passed through |
## Engine digests (Milestone 5)

The engine scenarios (`engine-scenarios.txt`; recordings specified in `ENGINE_SCENARIOS.md` section 9) produce two text recordings per fixture and scenario, `<scenario>__<fixture stem>.out.txt` (the comparison contract) and `.sched.txt` (the scheduler trace). They are far too large to commit (about 560 MB for one full run), so v1's recordings are committed as SHA-256 digests in `expected\engine\`:

- `manifest.txt`: `#` comment lines, then `format 1`, `scenarios-sha256 <64 upper-case hex>` (SHA-256 of the scenario file with CRLF and CR read as LF; the runner refuses digests made for another scenario file), `scenarios <n>`, and `fixtures <n>`. (The number of `.out.txt` lines that the DD-001 normalizer maps is pinned in the engine rule of `expected\differences-from-v1.txt`, not here; `make-engine-expected.ps1` recounts it in v1's recordings.)
- `<fixture stem>.digests`: a `#` comment line, then one line per scenario, `<scenario> <SHA-256 of .out.txt> <SHA-256 of .sched.txt>`, upper-case hex, scenarios in ordinal order.

The digests come from the v1 lock-step spike (`harness\v1-reference\spike`), written by `harness\make-engine-expected.ps1` only when every copy of every file is byte-identical across architectures (x86 = x64) and across all runs given. The v2 side hashes its own recordings and compares; a mismatch is reported as fixture / scenario / file, and `midibn-engine-harness --v1 DIR` shows the first differing line against the spike's files. `make-engine-expected.ps1 -Check` is the drift check (regenerate with the spike, compare with the committed digests).

## Difference rules (`expected/differences-from-v1.txt`)

v2 does not have to reproduce v1.0.44 exactly (`v2/docs/DIFFERENCES_FROM_V1.md`), so every difference between a v1 reference and the v2 output must be explained by a rule. The runners (`midibn-parity`, `v2-xp-selftest`; the engine rules also by `midibn-engine-harness --rules`) read the rule file next to `expected\`. Code: `v2/tools/difference_rules.h|cpp`.

**File format.** `#` starts a comment line. The first rule line is `format 1`. Every other line is one rule: `<kind> <DD-id> <target> <arguments>`, separated by blanks. `<DD-id>` is an entry of the register (`DD-` and digits). An argument written `/.../` is a regular expression (ECMAScript, matched against the **whole** line, may contain blanks; write `\/` for a slash). A glob uses `*` and `?` and may list alternatives separated by commas; it is case-sensitive.

| Rule | Meaning |
|---|---|
| `trace <id> <trace glob> /<v1 regex>/ /<v2 regex>/` | Lines that **differ at the same position**: the v1 line matches the first expression and the v2 line the second. In the second expression `${N}` stands for the text that group N of the first expression captured, so a rule can say "the same line, except for this part" (DD-001: the line is the same, only the data field changed); `${N+1}` stands for that text read as a whole number, plus one (DD-020: a time that is exactly one microsecond later). |
| `insert <id> <trace glob> /<v2 regex>/` | A v2 line that v1 does not have (an added line; for example a duplicate event is no longer sent, a clearer message). |
| `delete <id> <trace glob> /<v1 regex>/` | A v1 line that v2 no longer has. |
| `engine <id> <scenario glob>@<fixture glob> normalize=<name> [pin=<fixture>:<n>,...] [files=out\|sched\|both]` | Engine recordings of the matching (scenario, fixture stem) runs: the named normalizer is applied to v2's `.out` recording before hashing it against v1's digest (it maps v2's text back to what v1 would have recorded). `pin` fixes how many recording lines the normalizer must change per fixture (over all scenarios, checked on a run of the full scenario set), so the rule cannot go stale unnoticed. Normalizers are listed in `DifferenceRules::find_normalizer` (`short-sysex-zeroed`, DD-001). |
| `engine <id> <scenario glob>@<fixture glob> explained [files=out\|sched\|both]` | The v2 recording of the matching runs is **allowed to differ** from v1's (the difference is explained by the entry, which has the evidence). Such a run must still equal the v2 baseline exactly. With the v1 recording on hand (`--engine-v1 DIR`) the first differing line is printed, v1 text next to v2 text. |

The trace glob is matched against the expected trace's name with `/` separators: `sysex-short-mid.trace`, `flow/<stem>.ptr4.trace`, `models/<family>.trace`, `large-8m-mid.summary.trace`, `models-large/routing-large.trace`. Trace lines are compared after the `#` comment lines are removed. Walking both traces together, equal lines are skipped; where they differ, a matching `trace` rule consumes both lines, otherwise a matching `insert` rule consumes the v2 line, otherwise a matching `delete` rule consumes the v1 line; if none matches, the trace fails at that line (`FAIL <file>: line N, expected (v1) / actual (v2)`).

**What fails.** (1) A difference that no rule explains. (2) A rule whose targets were compared in this run but which explained nothing (`FAIL rule DD-nnn (rules file line N: ...) no longer occurs`): remove the rule, or restore the behaviour. (3) A normalizer whose changed-line count differs from its pin. (4) A malformed rule file. A rule whose targets were not run (a subset engine run, `--no-engine`, a trace that was not compared) is reported as `note rule ... not exercised`; that is not a failure. A trace that fails stops being compared at its first unexplained line, and the rules for it are then not judged stale.

**Summary.** After the comparisons the runner prints one line per register entry (`register DD-001: 19 changed trace lines, 2628 engine recording lines normalized (2 rules)`) and the RESULT line carries the totals. `--show-differences` prints every explained difference (otherwise the first three per entry and trace).

**Adding a difference.** See "How to add an entry" in `v2/docs/DIFFERENCES_FROM_V1.md`. In short: register entry, rules, code, `make-v2-baseline.ps1`.

## v2 baseline (`expected-v2/`)

The second, independent tier. It records **v2's own output**, so that later v2 changes are still checked exactly even though v1 is no longer the exact reference, and so that an improvement cannot change anything it did not mean to. It is produced by `v2/harness/make-v2-baseline.ps1` (which runs `midibn-parity --write-baseline`) and checked by every parity run:

- `traces.sha256`: `#` comment line, then `<trace name> <SHA-256>` per line: the load/index trace and both load-flow traces of every fixture (`sysex-short-mid.trace`, `flow/<stem>.ptr4.trace`), every model trace (`models/<family>.trace`) and the large-fixture traces (`large-8m-mid.summary.trace`, `flow/large-8m-mid.ptr4|8.summary.trace`, `models-large/routing-large.trace`; checked only when the large fixtures are given). The digest covers the v2 text with its `#` comment lines removed, so the comment that names the build does not matter.
- `text/<name with / replaced by _>`: the full v2 text of those traces that differ from v1 (today the two SysEx traces), so that a mismatch can show its first differing line.
- `engine/manifest.txt` and `engine/<fixture stem>.digests`: `format 1`, the digest of the scenario file the entries were made with (the runner refuses a baseline made for another scenario file), the counts, and per fixture one line `<scenario> <SHA-256 of v2's raw .out.txt> <SHA-256 of v2's raw .sched.txt>` (raw: before any normalizer).

A mismatch fails with `FAIL baseline <file>: v2's output changed from its recorded baseline` (or `FAIL engine <fixture> / <scenario>: <file> differs from the v2 baseline`), naming the register and the regeneration script. An entry that this run did not produce also fails (a removed fixture or model); a trace without an entry fails. The baseline is identical for the x64, x86, XP (`v141_xp`) and XP-IA32 builds. `make-v2-baseline.ps1 -Check` prints what would change without writing. The XP self-test package carries the rule file and the baseline.

## Extension rules (for later milestones)

- New information goes into **new sections** (e.g. `[source-index]`, `[chase-index]`, `[model <name> <config>]`, `[analysis <config>]`, `[playback <scenario>]`). Existing sections never change meaning or field order.
- A planned playback/model section uses one line per emitted message: `m <sequence> <time-us> <bytes hex|sha256:…> [<annotation tokens>]`, with time taken from a scripted virtual clock so it is deterministic.
- Incompatible changes increment `trace-format`. The parity runner refuses to compare traces of different versions.
