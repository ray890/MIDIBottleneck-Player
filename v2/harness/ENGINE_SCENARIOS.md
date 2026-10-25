# Engine scenarios: file format and recorded-output format

`v2/harness/engine-scenarios.txt` is the single definition of the scripted engine scenarios. It is read by

- the v1 reference side, the lock-step spike in `v2/harness/v1-reference/spike/` (`ScenarioFile.cs` parses it, `ScenarioBinder.cs` turns it into calls on v1's unchanged `PlaybackEngine`), which writes the reference recordings, and
- the C++ engine harness (section 12), which reads the same file, runs the same scenarios against the v2 engine, and produces the same `.out.txt` and `.sched.txt` recordings.

This document is the specification of both the file and the recordings. The lock-step method itself (why the recordings repeat byte for byte) is in `v2/docs/ENGINE_HARNESS_FEASIBILITY.md`.

The scenario text is frozen once reference output exists for it: a change to an existing scenario changes its recordings and must be re-verified. Add new scenarios; do not edit old ones.

## 1. Lexical rules

- UTF-8, LF or CRLF. One directive per line. `#` starts a comment that runs to the end of the line. Blank lines are ignored.
- A line is split into tokens at spaces and tabs. The first token is the directive. Tokens never contain `#`.
- Names (scenario and profile names) use lower-case letters, digits and `-` only.
- Token spelling for v1 enum members and `PlaybackEngine` properties is the C# name in lower-case kebab-case: an upper-case letter starts a new word, words are joined with `-` (`DropOldestCompleteNote` is `drop-oldest-complete-note`, `SimulateSlowdown` is `simulate-slowdown`, `ReserveChannel10` is `reserve-channel10`). The logs use the C# names.
- Booleans are `on` and `off`. Integers are decimal with an optional leading `-`.

## 2. Grammar

```
file        := "format 1" { profile | scenario }
profile     := "profile" NAME  { setup }  "end-profile"
scenario    := "scenario" NAME { option | setup | use } { command }
option      := "describe" TEXT...            # documentation only
             | "snapshots" ("none"|"on"|"full")   # default none
             | "stop-after-end" ("on"|"off")      # default on
use         := "use" PROFILE                 # splices the profile's setup lines here
setup       := "set" KEY VALUE
             | "service" MODE PROCESSING_US BITRATE EVENTS_PER_SECOND
             | "override" CHANNEL ATTRIBUTE (VALUE|"auto")
             | "channel" CHANNEL ("on"|"off")
             | "route" { KEY "=" VALUE }
command     := "at" TIME VERB { ARGUMENT }
```

- `format 1` is the first directive. A profile is a named list of setup lines (no options, no `use`, no commands, no nesting) and must be defined before the first `use` of it.
- A scenario's setup lines and `use` lines must all come before its first `at` line. A scenario may contain at most one `route`.
- Setup lines apply, in file order, to a freshly created engine before the first command. `route` is not an engine call; it is applied to the song (section 6) and is independent of the order of the other setup lines.
- Commands execute in file order. A command whose time has already passed when its turn comes executes at the current virtual time (virtual time never goes back). Files normally list commands in non-decreasing time.

## 3. Time expressions

`TIME := TERM { "+" TERM }` with no spaces. `TERM := UNSIGNED "/" UNSIGNED | UNSIGNED`.

Let `D = max(1000, song duration in microseconds)` (`MidiSong.DurationMicroseconds`; the routed view has the same duration). The value is in virtual microseconds and is computed with 64-bit signed integers:

- `N/M` is `D * N / M`: multiply first, then truncating integer division (`M` must not be 0; `N` and `M` are at most 1,000,000).
- A plain integer is that many microseconds.
- Terms are summed.

Examples: `3/10` is `D*3/10`; `4/10+50000` is `D*4/10 + 50000` (the fraction is truncated, then 50000 is added); `1/2` is `D/2`; `0` is 0.

The time after `at` is an offset from the scenario base time (section 8). The same expression syntax is used for every position argument (`start`, `seek`).

## 4. Settings (`set KEY VALUE`)

Each key is the kebab-case name of a `PlaybackEngine` property and sets that property. v1 clamps out-of-range values; the harness passes values through unchanged. Defaults are those of a new `PlaybackEngine`; a scenario that does not name a key gets the default.

| key | property | value | default | v1 clamp |
|---|---|---|---|---|
| `simulate-slowdown` | `SimulateSlowdown` | on/off | on | |
| `apply-queue-limit-without-slowdown` | `ApplyQueueLimitWithoutSlowdown` | on/off | off | |
| `chase-midi-state-on-play-seek` | `ChaseMidiStateOnPlaySeek` | on/off | on | |
| `preserve-playback-queue-on-pause-seek` | `PreservePlaybackQueueOnPauseSeek` (the ordered path) | on/off | off | |
| `output-paced-timeline` | `OutputPacedTimeline` | on/off | off | |
| `scale-note-interval-by-traffic` | `ScaleNoteIntervalByTraffic` | on/off | off | |
| `interval-polyphony-limit-enabled` | `IntervalPolyphonyLimitEnabled` | on/off | off | |
| `interval-polyphony-limit` | `IntervalPolyphonyLimit` | int | 128 | 1 to 128 |
| `queue-length-limit` | `QueueLengthLimit` | int | 2000 | 1 to 1,000,000 |
| `queue-age-limit-enabled` | `QueueAgeLimitEnabled` | on/off | off | |
| `queue-age-limit-microseconds` | `QueueAgeLimitMicroseconds` | int | 1,000,000 | 1 to 3,600,000,000 |
| `overflow-policy` | `OverflowPolicy` | enum | `drop-newest` | |
| `service-duration-mode` | `ServiceDurationMode` | enum | `processing-time` | |
| `processing-microseconds` | `ProcessingMicroseconds` | int | 0 | at least 0 |
| `midi-bitrate` | `MidiBitrate` | int | 31250 | at least 1 |
| `events-per-second` | `EventsPerSecond` | int | 10000 | 0 to 9,999,999 |

Other engine defaults the scenarios rely on: `WorkerStopTimeoutMilliseconds` 2000 (the real-time Pause/Stop timeouts are not exercised; the harness models the 2000 ms acknowledgement in virtual time), no drop trace, no source-dispatch trace, no channel monitoring.

`overflow-policy` tokens: `drop-newest`, `drop-oldest`, `clear-buffer-and-catch-up`, `drop-incoming-complete-notes`, `drop-oldest-complete-note`, `drop-background-notes` (`OverflowPolicy` 0 to 5). `service-duration-mode` tokens: `processing-time`, `midi-bitrate`, `events-per-second` (0 to 2).

`service MODE PROCESSING_US BITRATE EVENTS_PER_SECOND` is `PlaybackEngine.SetServiceConfiguration`, which sets the model and its three parameters in one call (MainForm uses it for every rate-model change). It is a different call from setting the properties one by one: it signals the worker once and clamps all three values.

Setup lines are executed as engine calls in order. The order matters when two calls interact (for example `simulate-slowdown` before `service`); the committed file keeps the order MainForm uses.

`override CHANNEL ATTRIBUTE VALUE` is `SetChannelOverride`; `channel CHANNEL on|off` is `SetChannelEnabled`. In setup they run before the first `start`, so no output is attached yet: the override only records the forced value (it is sent at `start`), and a disabled channel stays disabled. Attributes: `bank-msb`, `bank-lsb`, `program`, `volume`, `expression`, `pan`, `sustain`, `pitch-bend`, `aftertouch` (`ChannelAttribute` 0 to 8). Channels are 0 to 15. Values: 0 to 127; `sustain` 0 or 1; `pitch-bend` -8192 to 8191; `auto` clears the override.

## 5. Processing modes, gates and how MainForm configures them

`start` takes a `ProcessingMode` token: `queue`, `drop`, `per-note-interval-gate`, `delayed-note-off`, `note-attack-limit`, `output-rate-limit` (`ProcessingMode` 0 to 5). The four gates are processing modes.

The committed profiles reproduce what MainForm does to the engine when the user chooses a rate model or a gate (`MainForm.ApplyRateChoice`, `SynchronizeRateProcessing`, `ConfigureQueueLimitControls`):

| UI choice | engine configuration | Start mode |
|---|---|---|
| Rate model "(none)" | `simulate-slowdown off`; `service processing-time P B E` | `queue` or `drop` |
| Processing time | `simulate-slowdown on`; `service processing-time P B E` | `queue` or `drop` |
| MIDI bitrate | `simulate-slowdown on`; `service midi-bitrate P B E` | `queue` or `drop` |
| Events/sec | `simulate-slowdown on`; `service events-per-second P B E` | `queue` or `drop` |
| Per-note interval gate | `simulate-slowdown off`; `service processing-time INTERVAL B E` | `per-note-interval-gate` |
| Delayed NoteOff | `simulate-slowdown off`; `service processing-time MIN_ON B E` | `delayed-note-off` |
| Note attack limit | `simulate-slowdown off`; `service events-per-second P B STARTS` | `note-attack-limit` |
| Output rate limit | `simulate-slowdown off`; `service events-per-second P B RATE` | `output-rate-limit` |

(`P`, `B`, `E` are the unused service fields; MainForm leaves them at its defaults 100, 31250 and 10000.) A finite queue is Start mode `drop` with `queue-length-limit` and `overflow-policy` set. Forward-only ("apply queue limit without slowdown") is `apply-queue-limit-without-slowdown on` with `simulate-slowdown off`; v1 allows it only with `drop-newest` or `drop-incoming-complete-notes`, and a queue waiting-time limit (`queue-age-limit-enabled on`) only with `drop-oldest`, `drop-oldest-complete-note` or `clear-buffer-and-catch-up` and not together with forward-only. `interval-polyphony-limit-*` and `scale-note-interval-by-traffic` apply to the two interval gates (per-note interval and delayed NoteOff). MainForm changes the structural options (chase, ordered path, output-paced timeline, scaling, polyphony, forward-only, age limit) only while stopped; scenarios set them in setup. Rate values (`processing-microseconds`, `midi-bitrate`, `events-per-second`), `queue-length-limit` and `overflow-policy` can change while playing.

## 6. Commands (`at TIME VERB ...`)

Every command is applied by the harness thread at a scheduling point (section 8). The recording logs `H CMD <name>` first (section 9). The commands that make v1 create a new worker thread, which the harness holds back until the command has returned (section 8), are `start`; `pause` and `seek` on the classic path while Playing (`seek` also while Paused); and `restart` while Playing or Paused. On the ordered path with a live worker, `pause` and `seek` start no thread.

| command | engine call | log name | notes |
|---|---|---|---|
| `start MODE POS [paused]` | `Start(song, output, MODE, POS, paused)` | `Start(<Mode>,pos=<POS>)` and `Start(<Mode>,pos=<POS>,paused)` | `song` is the loaded song or, with `route`, the routed view. `Start` stops a running worker first. |
| `stop` | `Stop()` | `Stop` | |
| `pause` | `Pause()` | `Pause` | Classic path: stops the worker and starts a replacement that waits paused. Ordered path with a live worker: the worker acknowledges. |
| `resume` | `Resume()` | `Resume` | |
| `seek POS` | `Seek(POS)` | `Seek(<POS>)` | |
| `snapshot` | none | `Snapshot` | A scheduling point only; the snapshot lines follow like for any command. |
| `set KEY VALUE` | the property setter of section 4 | `Set(<Property>=<value>)` | Booleans log `1` or `0`, enums log the C# member name. |
| `service MODE P B E` | `SetServiceConfiguration` | `SetService(<ServiceDurationMode>,<P>us)` | The log name omits B and E. |
| `override CH ATTR VALUE` | `SetChannelOverride` | `Override(ch=<CH>,<Attribute>=<value>)` | `auto` logs `Auto`. |
| `channel CH on\|off` | `SetChannelEnabled` | `Channel(ch=<CH>,enabled=<1\|0>)` | |
| `restart MODE [KEY=VALUE ...]` | MainForm's processing-mode restart (below) | `Restart(<Mode>[,<Property>=<value>...])` | |

`<POS>` and `<Mode>` in log names: POS is the evaluated integer, Mode the C# `ProcessingMode` member name (`Queue`, `Drop`, `PerNoteIntervalGate`, `DelayedNoteOff`, `NoteAttackLimit`, `OutputRateLimit`).

**`restart`** is `MainForm.RestartForProcessingModeChange`, which MainForm runs when a gate's value (interval, minimum NoteOn duration, starts per second, output rate) is edited while playing or paused. If the engine state is `Playing` or `Paused`: take `previous = State` and `position = GetSnapshot().IntendedTimelineMicroseconds`, call `Stop()`, apply each `KEY=VALUE` setter in order, then `Start(song, output, MODE, position, previous == Paused)`. In any other state it only applies the setters. MainForm sets the new value one step earlier (before the snapshot and the `Stop`); the harness applies it after `Stop` so that a setter-triggered wake of the worker cannot run the worker concurrently with the harness (the worker is stopping anyway).

**Scenario routing (`route KEY=VALUE ...`).** MainForm applies a track route with `TrackRoutePlanner.Compile(song, settings)` followed by `TrackRouteApplication.CreateRoutedSong(song, route)` and plays the routed view. Keys (all optional; defaults are `TrackRoutingSettings` defaults): `tier=exact-safe|gm-family|aggressive` (default `exact-safe`); `cap=1..16` target capacity (default 16); `perc=reserve-channel10|allow-any-destination` (default `reserve-channel10`); `usable=XXXX` four hex digits, bit `c` set = destination channel `c` usable (default `FFFF`); `manual=T.C=D;T.C=D...` manual destination `D` for source lane track `T` channel `C` (zero-based). If the compiled route is invalid (`Errors` not empty; MainForm's Apply is then disabled) nothing is applied and the unrouted song plays. The route is confirmed explicitly, as the dialog does for a route that changes instrument identity. The recording gets one `ROUTE` line (section 9).

## 7. Scenario catalog

The file has 172 scenarios in eleven groups (21 from the Milestone 3 spike and its Pause/Seek follow-up, 140 added by Milestone 5, 9 added later to close mutation-check coverage gaps, 2 added with the DD-006 fix); the names are the contract (output files are named after them).

| group | names (count) | what it exercises |
|---|---|---|
| Milestone 3 and Pause/Seek | `queue-*` (11), `ordered-pause-*` and `ordered-seek-playing` (7), `ordered-queue-300us-stop`, `ordered-drop-pause-seek-resume`, `drop-1500us-limit32` (21 in all) | Play, Stop, restart, live setting change, Drop, Pause/Resume/Seek on the classic and ordered paths |
| A. rate models | `rate-*` (8) | none, processing time, MIDI bitrate, events/sec in Queue mode; live edits and model switches |
| B. Drop mode | `drop-policy-*` (6), `drop-age-*` (3), `drop-forward-*` (2), `drop-rate-*` (3), `drop-live-*` (1) | all six overflow policies, the queue waiting-time limit, forward-only, other rate models, live limit/policy edits |
| C. gates | `gate-*` (18) | Per-note interval, Delayed NoteOff, Note attack limit, Output rate limit: plain, live edit (`restart`), Pause/Seek, restart while paused, interval scaling, polyphony limit |
| D. ordered path | `ordered-rate-*` (5), `ordered-drop-policy-*` (6), `ordered-drop-age-*` (2), `ordered-drop-forward-*` (2), `ordered-drop-live-*` (1), `ordered-gate-*` (10) | the preserve-on-Pause/Seek path over the rate models, finite-queue policies, forward-only and the gates, with Pause, position-keeping Seek and Resume |
| E. output-paced | `output-paced-*` (4), `ordered-output-paced-*` (10) | Output-paced timeline, classic and ordered, with the four gates on the ordered path |
| F. chase | `chase-*` (16), `ordered-chase-*` (6) | Start/Seek at a mid-song position with the chase on and off, over Queue, Drop, the gates and the ordered path |
| G. controls | `override-*` (10), `channel-*` (9), `ordered-override-*` (2), `ordered-channel-*` (1) | channel overrides set/cleared while playing, paused, stopped; channels disabled/enabled; with Drop and the gates |
| H. routing | `route-*` (15) | routed views: tiers with generated patch messages, lane omission, over Queue, Drop, gates, ordered path, chase, overrides |
| I. mutation-check coverage | `rate-eps13-*` (2), `ordered-output-paced-rate-1us-*` (2), `ordered-drop-policy-clear-buffer-channels-off*` (2), `gate-output-rate-limit-chase-*` (3) | decisions the first 161 scenarios could not show: an events/sec rate with a carried remainder (13 per second: 1,000,000 mod 13 = 1), the ordered Output-paced service hold with a real service time (1 us, so that no source event falls inside the hold on these fixtures; see section 11), Clear buffer and catch up at capacity with disabled channels (filtered arrivals), an Output rate limit started mid-song whose first decision falls on a slot edge to the microsecond |

| J. DD-006 | `ordered-override-pause-set-clear-resume`, `override-pause-set-clear-resume` | an override set and cleared while paused, then Resume: the ordered path froze in v1 (DD-006); the classic path is the control. v1 recordings were added with `make-engine-expected.ps1 -Merge` |

All scenarios run on every valid fixture of `v2/harness/fixtures` (the 23 files named in the header comment of the scenario file, `run-spike.ps1 -AllValid`).

## 8. Execution model (what the C++ harness must reproduce)

For each (fixture, scenario) the harness loads the fixture with the compact parser, creates a **new engine** with the defaults of section 4, applies the setup lines, and runs the commands.

- **Virtual clock.** One tick is one microsecond (`Stopwatch.Frequency` = 1,000,000). Every scenario starts at virtual time `BASE = 10,000,000`; a command at offset `T` is due at `BASE + T`. All waits (`WaitMicroseconds`, `SpinWait`) are virtual and advance only through the scheduler.
- **Output cost.** The recording output logs a message, then advances the clock by `cost`: `max(1, bytes) * 320` microseconds (31,250 bit/s, 10 bits per byte), where `bytes` is the length of the message including its status byte. `Reset` and `Panic` cost nothing.
- **Scheduling points and delivery.** The engine's worker thread runs only while the harness thread waits. The worker stops at one of: a timed wait with a deadline (`WaitMicroseconds`), an untimed wait (the paused worker), or the end of a completed `Send` when a command is due (a yield with deadline = now; taken only while the engine state is `Playing`). The harness then either applies the next command or advances virtual time to the deadline and releases the worker. The next command is applied when its due time is at or before the worker's deadline (an untimed wait has an infinite deadline), or when no worker thread is alive. Applying a command first moves the clock to `max(now, BASE + T)`. A command lands at the first scheduling point at or after its time; it never lands inside a `Send`. Commands that signal the worker (v1's `SignalWake`: setters, `Stop`, `Start`, `Seek`, `Resume`, overrides, enable/disable) release it; the harness waits until the worker is stopped again or its thread has ended before looking at the next command. A worker started by `Start`, or by the classic `Pause` and `Seek`, does not run until the command that created it has returned. A preserving `Pause` waits for the worker's acknowledgement with a 2000 ms timeout measured in virtual time; the worker advances no virtual time while paused.
- **Running to the end.** After the last command the harness keeps releasing the worker until its thread ends. Then, unless `stop-after-end off`, it issues one final `Stop` (logged `Stop(after end)`). If the worker is waiting with no deadline (left paused) it issues `Stop(paused at end)` at the current time instead, then the final stop. A worker whose deadline is at or beyond `CAP = BASE + 40 * D + 600,000,000` is stopped with `Stop(CAP)`.
- **End of the recording.** After the final stop the harness logs `FINAL` and disposes the engine (`DISPOSED`).
- **No ambient events.** Besides the engine events in section 9, nothing else is logged.

## 9. The recorded output

For each (scenario, fixture) the spike writes two files: `<scenario>__<fixture stem>.out.txt` and `<scenario>__<fixture stem>.sched.txt` (the fixture stem is the file name without extension, `models-dense` for `models-dense.mid`). Both are UTF-8 without BOM; every line ends with a single LF (`\n`).

### 9.1 `.out.txt` (the comparison contract)

Every line except the four marked **bare** starts with the virtual time in microseconds (decimal, no padding) and one space. `H` is the thread that runs the scenario (the harness, which applies commands); `W` is any other thread (the engine worker). A line is written when its event happens; the scheduler guarantees a fixed order.

| line | when |
|---|---|
| `<t> H CMD <name>` | a command is about to be applied; `<name>` as in section 6, plus the driver's own `Stop(after end)`, `Stop(paused at end)`, `Stop(CAP)` |
| `<t> W|H SEND <Kind> src=<index> <HEX> cost=<c>` | `IMidiOutput.Send`. `<Kind>` is the `MidiEventKind` member (`NoteOff NoteOn PolyphonicAftertouch ControlChange ProgramChange ChannelAftertouch PitchBend SystemExclusive SystemMessage`). `<index>` is the source event index, -1 for a message the engine generated (chase, override, channel control, route patch messages). `<HEX>` is every byte of the message, two upper-case hex digits each, status first. `<c>` is the cost of section 8. `t` is the time before the cost is added. |
| `<t> W|H RESET` / `<t> W|H PANIC` | `IMidiOutput.Reset`, `IMidiOutput.Panic` |
| `<t> W EVENT PlaybackEnded` | the engine's `PlaybackEnded` event |
| `<t> W EVENT PlaybackFailed <ExceptionType>: <message>` | `PlaybackFailed` (never expected) |
| `<t> W EVENT ChannelControlFailed ch=<n> <Attribute> <ExceptionType>: <message>` | `ChannelControlFailed` (never expected) |
| `<t> H ROUTE valid=<0\|1> explicit=<0\|1> tier=<n> errors=<n> lanes=<n> destinations=<n> merged=<n> retimbred=<n> omitted=<n> omittedStarts=<n> generated=<n> included=<n> output=<n> applied=<0\|1>` | once, first line, in a scenario with `route`: `valid` = `IsValid`; `explicit` = `RequiresExplicitApply`; `tier` = numeric `HighestTier` (0 exact-safe, 1 gm-family, 2 aggressive); `errors` = `Errors.Count`; then `TrackRouteStatistics` `RoutedLaneCount DestinationCount MergedLaneCount RetimbredLaneCount OmittedLaneCount OmittedNoteStartCount GeneratedDestinationStateCount IncludedSourceEventCount RoutedOutputEventCount`; `applied` = 1 when the routed view plays |
| `<t> H SNAP state=<State> processed=<n> dropped=<n> queue=<n> position=<us> intended=<us> worker=<w>` | with `snapshots on` or `full`: after every command, once the worker is stopped again. `State` is `Stopped`, `Playing`, `Paused` or `Completed`; `processed`, `dropped`, `queue`, `position`, `intended` are `ProcessedEvents`, `DroppedEvents`, `QueueLength`, `PlaybackMicroseconds`, `IntendedTimelineMicroseconds` from `GetSnapshot()`; `<w>` is `parked` (worker stopped at a timed wait or yield), `waiting-no-deadline` (untimed wait, paused) or `none` (no live worker thread) |
| `<t> H SNAPX gateFiltered=<n> musicalRejectedAttacks=<n> musicalSuppressedReleases=<n> rateRejectedAttacks=<n> rateSuppressedReleases=<n> rateRejectedOther=<n> rateSafetySoftExcess=<n> rateSafetyBypass=<n> virtualQueue=<n> maxQueue=<n> lastDispatched=<us> frontier=<us> lag=<us> maxLag=<us>` | with `snapshots full`, directly after the `SNAP` line. Fields are `GateFilteredEvents`, `MusicalLimitRejectedAttacks`, `MusicalLimitSuppressedReleases`, `OutputRateRejectedAttacks`, `OutputRateSuppressedReleases`, `OutputRateRejectedOtherMessages`, `OutputRateSafetySoftExcess`, `OutputRateSafetyBypassMessages`, `VirtualQueueLength`, `MaximumQueueLength`, `LastDispatchedTimelineMicroseconds`, `EffectiveSpeedFrontierMicroseconds`, `CurrentLagMicroseconds`, `MaximumLagMicroseconds` |
| **bare** `CMD-EXCEPTION <name> <ExceptionType>: <message>` | a command threw (for example `Start` refusing an illegal option combination); logged when the worker is next stopped, before the `SNAP` line. Diagnostic: the exception type and text are .NET's. No committed scenario intends one. |
| **bare** `HARNESS-ERROR <ExceptionType>: <message>` | the scheduler failed (hang, protocol error). Never expected. |
| **bare** `FINAL t=<t> state=<State> processed=<n> dropped=<n> maxQueue=<n> maxLagUs=<n> position=<us>` | after the last command and final stop: `ProcessedEvents`, `DroppedEvents`, `MaximumQueueLength`, `MaximumLagMicroseconds`, `PlaybackMicroseconds` |
| **bare** `DISPOSED t=<t>` | after `Dispose` |

The `SNAP` and `SNAPX` fields come from v1's `GetSnapshot()`, so they are part of the comparison. A command that throws writes `CMD-EXCEPTION`; the run continues.

### 9.2 `.sched.txt` (the scheduling trace of the v1 spike)

The scheduler's own trace, one record per line. It is specific to the rendezvous implementation; the v2 harness reproduces it exactly and its digest is compared too, which proves that the v2 engine blocks, wakes and starts threads at the same points as v1.

| line | meaning |
|---|---|
| `CMD <name> t=<now>` | a command was applied |
| `WORKER-START gen=<n> t=<now>` | a worker thread registered (counts threads in the run) |
| `WORKER-WAITER-DISPOSE t=<now>` | a worker thread is exiting |
| `PARK t=<now> until=<deadline>` | the worker stopped at a timed wait with that virtual deadline |
| `YIELD t=<now> until=<now>` | the worker stopped at the end of a `Send` because a command is due |
| `WAIT t=<now> until=none` | the paused worker stopped at an untimed wait |
| `RESUME-GO t=<now>` | the worker was released by the harness at the deadline |
| `RESUME-WAKE t=<now>` | the worker was released by v1's wake signal |
| `CONTROL-WAIT-RELEASE t=<now> until=<deadline>` | a preserving `Pause` (control wait) released the parked worker at its deadline |
| `CONTROL-WAIT-TIMEOUT t=<now>` | a control wait timed out |

### 9.3 Console line (not part of the contract)

The spike also prints one summary line per run (scenario, fixture, 16-hex-digit prefixes of the SHA-256 of both files, counters, real time). It contains wall-clock time and is not compared.

## 10. Running and checking

```
v2\harness\v1-reference\spike\build-spike.ps1                 # refuses unless 62 v1 files equal tag v1.0.44
v2\harness\v1-reference\spike\run-spike.ps1 -AllValid -Runs 5  # x86 and x64, fresh processes, SHA-256 across runs and architectures
v2\harness\v1-reference\spike\run-spike.ps1 -AllValid -Runs 1 -Only 'gate-*,route-*'
v2\out\spike\x64\rendezvous-spike.exe --list                   # scenario names in the default file
v2\out\spike\x64\rendezvous-spike.exe [--scenarios FILE] [--only NAME,PREFIX*] FIXTUREDIR OUTDIR [fixture ...]
```

`--scenarios FILE` and `-ScenarioFile` select another scenario file (default: the committed one). A malformed file stops the run with `SCENARIO FILE ERROR: <file> line <n>: <reason>`.

## 11. Known limits and observations

- **Ordered path busy-polls the clock.** v1's ordered ("preserve") worker loops without any wait, reading `Stopwatch`, while a live channel control (override or enable/disable) or an Output-paced service hold is pending and the next source event is already due; it spins until the in-service item completes. A virtual clock cannot advance inside a poll loop, so the spike fails such a run after 5,000,000 idle reads (`PlaybackFailed`, "busy-poll livelock") instead of hanging. The scenarios therefore run the ordered path with controls and with the Output-paced timeline only where the simulated slowdown is off (rate model "(none)", the gates, forward-only Drop). The classic path with the Output-paced timeline and the slowdown on is covered (`output-paced-*`). This is a v1 behaviour, recorded, not changed.
- **Ordered path and a stale override mask (a v1 defect, fixed by DD-006; the text describes v1's behaviour, which the v1 recordings show).** `SetChannelOverride` sets a bit in a per-channel "pending" mask only when it sets a value; clearing an override (`auto`) leaves the bit. `ApplyPendingOverrides` returns at once when no override is set, so it never takes that bit. If the last override is cleared before the worker applied the pending one, `HasPendingOutput` stays true. All four ordered workers (`RunOrdered*Mode`) then stop admitting source events (`controlBoundary`) and loop forever at the top of their loop (`!hasInService && no pending work` then `continue`), so playback freezes with the worker spinning, and a later `Start` does not clear the mask. A minimal repeatable case on any fixture: ordered path, `pause`, `override 0 volume 20`, `override 0 volume auto` while paused (the paused worker applies channel controls but not overrides), `resume`. On dense songs the same happens without a pause because a backlog delays the worker. The classic path is not affected. The recordings and the repro scenarios are in `v2/out/spike/evidence-ordered-override-livelock/` (untracked); the spike's idle-read guard ends the run. The two ordered override scenarios keep one override (channel 9 pan) set to the end so that the boundary stays reachable.
- **Not driven.** Channel monitoring (`SetChannelMonitoring`, `ChaseStateForNewMonitor`), `ChaseLatestSourceChannelAttribute`, `SendExplicitChannelAttribute`, the drop and source-dispatch traces and the testing hooks are not scenario commands. The Windows-only output safety and device paths are replaced by the recording output.
- **Gate edits are restarts.** The four gates change only through `restart` (as MainForm does); the interval polyphony limit, scaling and the queue structure are fixed for a run (MainForm locks them while playing).
- A run is one canonical v1 trace for the script. A command delivered mid-computation in real time is not modelled (section 8).

## 12. The v2 harness (Milestone 5, Group E)

`midibn-engine-harness` and the engine part of `midibn-parity` (= the XP self-test `v2-xp-selftest[-ia32].exe`) run this scenario file against the v2 engine and compare the result with v1's recordings. Sources: `v2/harness/engine/` (library `midibn_engine_harness`: `scenario_file`, `scenario_binder`, `lockstep_scheduler`, `scenario_runner`) and `v2/tools/engine_harness_main.cpp`, `engine_compare.{h,cpp}`.

**Method (and why).** The harness is a line-for-line C++ twin of the spike: the real v2 engine runs on a real worker thread, and a rendezvous between the harness thread and that thread makes every interleaving a function of the script. The alternatives (fibers, a single-threaded re-implementation of the engine's blocking points) were rejected because they would change what a lock, a join or a thread start means; with real threads and the spike's exact rules, "same schedule" is true by construction and the only thing left to differ is the engine. `LockStepScheduler` is the `platform::EngineServices` implementation:

| Spike | v2 harness |
|---|---|
| `Stopwatch` shim (virtual 1 MHz clock, idle-read guard) | `Clock::now_ticks` reads the virtual time and counts consecutive reads of the same value by the registered worker; the 5,000,000th fails the worker with `TimeoutException: LockStepScheduler: worker polled the clock 5000000 times at virtual time <t> (busy-poll livelock)` |
| `SpinWait` shim | `Waiter::spin_once` = +1 virtual microsecond |
| `HighResolutionWaiter` (registers the worker, start gate) | `create_waiter` registers the calling thread as the worker (WORKER-START) after the start gate; `Waiter::wait` is `WaitMicroseconds` (overflow check, early `Woken` when the wake event is set, park with a virtual deadline) |
| `EventWaitHandle` shadow | `WakeEvent`: `wait(-1)` on the worker is an untimed park; `wait(ms)` on the control thread is the Pause-acknowledgement wait that lends the turn and times out in virtual time (`CONTROL-WAIT-RELEASE` / `-TIMEOUT`) |
| `RecordingMidiOutput` | `HarnessOutput`: logs the send, advances virtual time by `max(1, bytes) * 320`, then yields (`YIELD`) when a command is due and `state()` is Playing, released by a change of `wake_generation()`. `src=` and the event kind come from `set_send_observer`, the bytes from the output call itself. |
| `Thread` | `start_thread`: a `std::thread` whose body waits at the start gate in `create_waiter`; `Thread::join(ms)` is a real-time wait, as `Thread.Join` |
| `Drive` | `scenario_runner.cpp` `drive`, same decisions in the same order |

Every real-time wait has the spike's 20 s limit and raises the same `TimeoutException` texts. After a harness error all parties are released and joined before the engine is destroyed.

**The recordings.** `<scenario>__<fixture>.out.txt` and `.sched.txt` are written byte-identical to the spike's, except for the differences that the improvement register explains (`docs/DIFFERENCES_FROM_V1.md`; the engine rules of `expected/differences-from-v1.txt`). The first was DD-001: v1 reads a SysEx event of 1 to 3 stored bytes back as zeros, v2 keeps the file's bytes, so the hex of such a `SEND SystemExclusive` line differs in `sysex-short` and `sysex-variants` (1,957 and 671 lines over all 170 scenarios; nothing else about those runs differs). The comparison maps v2's `.out.txt` to what v1 would have recorded (those bytes become zeros) before hashing or diffing, through the named normalizer `short-sysex-zeroed` of the engine rule in `expected/differences-from-v1.txt`; the number of lines mapped is pinned in that rule (`pin=sysex-short:1957,sysex-variants:671`), so a change in either direction fails. The files `--out` writes are v2's raw recordings. Further differences from v1 are classified by engine rules (`normalize=` or `explained`; `TRACE_FORMAT.md`, "Difference rules"), and every raw v2 recording is also checked against its own recorded digest (`expected-v2/engine/`, "v2 baseline"), whatever the rules say.

**Digests.** v1's recordings are committed as SHA-256 digests (`v2/harness/expected/engine/`: `manifest.txt` and one `<fixture>.digests` per fixture, one line `<scenario> <out digest> <sched digest>` per scenario; format in `TRACE_FORMAT.md`). `make-engine-expected.ps1` writes them from a spike run directory and refuses unless every copy is identical (x86 = x64, all runs).

**Commands.**

```
midibn-engine-harness --list
midibn-engine-harness --fixtures v2\harness\fixtures --scenarios v2\harness\engine-scenarios.txt --expected v2\harness\expected\engine
    [--only NAME,PREFIX*] [--out DIR [--differences-only]] [--v1 DIR] [--verbose] [fixture.mid ...]
```

`--expected` compares digests (a mismatch is reported as `FAIL engine <fixture> / <scenario>: <file> differs (v2 <digest>.. expected <digest>..)`); `--v1 DIR` points at a spike output directory (for example `v2\out\spike\runs-lockstep-m5ext\x64\run1`) and prints the first differing line of every recording that differs, with or without `--expected`; `--out DIR` writes the files so they can be diffed with any tool; `--verbose` prints the spike's per-run console summary (sends, parks, resumes, yields, control waits), whose counters are equal to the spike's. `midibn-parity` runs the same comparison (options `--scenarios FILE`, `--engine-fixtures NAME,NAME`, `--engine-only NAME,PREFIX*`, `--no-engine`) and `tools\parity.ps1` adds the v1 drift check (regenerates v1's recordings with the spike when it is built and compares their digests with the committed ones). The full set takes about 45 s (x64) to 65 s (x86, XP) per build on the reference PC.
