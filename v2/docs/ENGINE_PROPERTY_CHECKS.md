# Engine property checks: the real-time complement to the lock-step parity

Milestone 5 exit test, `ARCHITECTURE.md` section 5.1 and section 11 (row 5). v1's live loop reads the wall clock for several decisions, so v1 and v2 cannot be compared exactly in real time. The scripted scenarios are compared exactly (every difference explained by the improvement register) under the lock-step harness (`ENGINE_SCENARIOS.md`). This document defines what is checked on **real-time runs of both programs against output that sends nowhere**: recorded properties derived from v1's code, each with the v1 source line it comes from.

Run it with `v2\tools\engine-properties.ps1` (about 8 minutes for the default five repeats; 479 s measured). It ends with `ENGINE-PROPERTIES: PASS` or `FAIL`. Raw logs stay in `v2\out\engine-properties\runs\` (untracked).

## 1. What runs

| Piece | Where | Notes |
|---|---|---|
| v1 side | `v2/harness/v1-reference/realtime/` → `v1-realtime.exe` (x86, x64) | v1's 63 sources, byte-identical to tag `v1.0.44` (verified by blob id, as `build.ps1` does), including the real `HighResolutionWaiter.cs`; `Program.cs` is replaced by `RealtimeHarness.cs`. Real `Stopwatch`, `SpinWait`, `EventWaitHandle`, threads: nothing is shadowed. The spike's `ScenarioFile.cs` and `ScenarioBinder.cs` are compiled in unchanged, so the scenario vocabulary is exactly that of `ENGINE_SCENARIOS.md`. |
| v2 side | `v2/tools/engine_realtime_main.cpp` → `midibn-engine-realtime.exe` (x86, x64) | The v2 engine over `Win32EngineServices` (QueryPerformanceCounter, waitable-timer waiter, kernel events, `_beginthreadex` threads). Its own scenario parser and binder for the same grammar. |
| Output | both | A recording output that logs and sends nowhere. No MIDI device is opened and no provider DLL (KDMAPI, OmniMIDI, third-party) is loaded. The JIT/first-use warm-up uses each program's own None output (`NullMidiOutput`). |
| Scenarios | `v2/harness/engine-realtime-scenarios.txt` | Same grammar as `engine-scenarios.txt`, but every time is **real** elapsed microseconds (`N/M` is still a fraction of the song duration); a control thread applies the commands at those wall-clock offsets, so a command lands at an arbitrary point of the worker's loop. `route` is not supported. The `describe` line carries `fixture=`, `class=exact|prop` and `ends=` tags. |
| Fixtures | `v2/harness/fixtures` | `models-dense` (9,166 events in 1.41 s, two ~4,500-event bursts), `index-chase-rich` (1,255 controller/program/bend/aftertouch events on all channels, 3.16 s), `f1-v1-test-midi`, `f0-all-message-types`, `sysex-short`, `sysex-variants`; plus `rt-protected`, a 7,655-event 1 s song the script generates (never committed) with overlapping notes, velocity-0 releases, orphan NoteOffs, sustain and CC120/121/123. |
| Lanes | script | x64 and x86 for both programs, four lanes in parallel (each is one worker thread plus a control thread; a 12-thread PC is not loaded by four of them). `-Serial` runs them one at a time. |
| Repeats | script | Every configuration runs `-Repeats` times (default 5) in a fresh process per run. |
| Checker | `v2/tools/engine-properties/PropertyChecker.cs` | Compiled with the .NET Framework `csc.exe`. Reads the logs, checks the properties of section 3 on every log and the cross-run equalities, prints a report. |

The song event lists both programs read (`events` command) are compared first; they must be identical apart from the differences the improvement register explains (so far DD-001, section 4).

### Log format

One text file per run, one record per line; timed records start with microseconds since the scenario began (`BEGIN`), measured on the program's own monotonic clock.

```
RUN program=v1|v2 arch=x64|x86 scenario=… fixture=… duration=… events=… timerFrequency=…
DESCRIBE <tags and text>              PLAN-SETUP <verb> <args>     PLAN-AT <offset> <command name>
<t> BEGIN
<t> SNAP state=… processed=… dropped=… queue=… outstanding=… maxQueue=… gateFiltered=… musicalRejectedAttacks=…
        musicalSuppressedReleases=… rateRejectedAttacks=… rateSuppressedReleases=… rateRejectedOther=… rateSafetySoftExcess=…
        rateSafetyBypass=… virtualQueue=… virtualMaxQueue=… position=… intended=… lastDispatched=… frontier=… lag=… maxLag=…
<t> CMD <name>   …   <t> CMD-DONE      (a SNAP is logged just before every CMD and every 20 ms)
<t> SEND <MidiEventKind> src=<index> <HEX bytes>     src>=1 is a source event; -1 and 0 are engine-generated messages
<t> RESET     <t> PANIC     <t> EVENT PlaybackEnded|PlaybackFailed …|ChannelControlFailed …
<t> FINAL <snapshot fields> sends=<n>      <t> END
```

The identity of a source send is its index (`src=`). Index 0 is also what v1 gives forced-override and channel-silence messages, so the checker treats `src=0` as the song's event 0 only when kind and bytes equal it (the fixtures' first event is a NoteOn, a bank-select that no scenario overrides, a program change, or a SysEx).

## 2. The exact class and why it is justified

A configuration is `class=exact` when none of v1's decisions in it depends on the wall clock, so the whole sent sequence (kind, source index, bytes, RESET/PANIC) must repeat bit for bit across repeats, programs and architectures. v1's code supports this for:

* **Queue mode** (classic and ordered): events leave in index order and none is ever dropped (`PlaybackEngine.cs:1241-1357`; the service model only delays them). With no live command the sequence is fixed. Wall-clock time decides *when*, not *what*.
* **The song-time gates** (Per-note interval gate, Delayed NoteOff, Note attack limit; `PlaybackEngine.cs:2173-2566`): their decisions are made by `PerNoteIntervalGate`, `DelayedNoteOffGate`, `MusicalAttackRateLimiter` and `AdaptiveNoteIntervalClock`, which are given only song ticks and microseconds from the song, never a clock (verified: none of those classes mentions `Stopwatch`); `nowMicroseconds` only decides when a due batch is processed.
* **Start with the chase at a fixed position, and Pause/Resume on the ordered path, which keeps its queue** (`PlaybackEngine.Ordered.cs`).

It is **not** justified for: the Output rate limit (it decides on `TicksToMicroseconds(Stopwatch.GetTimestamp())`, `PlaybackEngine.cs:2624`, `OutputRateGate.EndSourceTick(…, decisionTimestamp)`), Clear buffer and catch up (`FindFirstEventAfterTransport(nextArrival, now)`, `:1967`), the classic Pause (it discards the events that were due but not yet sent, `:486-499`, so what is lost depends on how far the worker lags) and anything that stops, seeks or edits at a wall-clock instant. Those are `class=prop`: only the properties of section 3 are checked. For the Drop policies the checker additionally reports whether the counters happened to be identical in every run (`STABLE`); that is an observation, because v1 has three real-time branches there (a send that straddles the next arrival, an 8 ms-long send batch, Clear buffer).

## 3. The properties

Applicability: *every* = every log; the check counts in the script output are the number of individual checks that ran. A property that ran no check in some program/architecture is reported `NOT-EXERCISED` and fails the run.

### Termination, final state, events

| ID | Rule | v1 source |
|---|---|---|
| T01 | The run reaches `END`; no `TIMEOUT`, watchdog or Dispose error; every command returns (`CMD-DONE`) without an exception. | `Stop`/`Start`/`Seek`/`Pause` return after the worker has stopped: `PlaybackEngine.cs:532-575` (bounded `Join`, 2000 ms) |
| T02 | No `PlaybackFailed` and no `ChannelControlFailed` event. | worker failure path `:1148-1160`, raised `:1210-1213`; channel control `:891-900` |
| T03 | `PlaybackEnded` fires exactly as many times as the scenario plays to the end (`ends=`), the engine is `Completed` just before the final `Stop`, and `FINAL` (after `Stop`) says `Stopped`. | `completed = _state == Playing && _song != null` `:1146`; `_state = Completed` `:1195-1202`; `PlaybackEnded` `:1215-1218`; `Stop` sets `Stopped` `:535-545` |

### Stop, Pause, Seek, Start and the chase

| ID | Rule | v1 source |
|---|---|---|
| S01 | Every Stop, every Start after the first, every classic Pause (engine Playing) and every classic Seek ends with exactly one RESET immediately followed by one PANIC inside the command. The first Start, a queue-preserving (ordered) Pause or Seek, and a Pause when not Playing produce none. After `FINAL`, `Dispose` adds one RESET+PANIC. | `Stop` → `StopWorkerForTransition` `:532-575`, worker exit `ResetAndSilence` `:1176-1186`, no-worker case `:553-558`; `Start` calls `Stop()` `:334`; classic Seek `:410`; classic Pause `:481`; preserving Pause/Seek skip the worker retirement `:457-463`, `:395-407`; `MidiOutputSafety.ResetAndSilence` = Reset then Panic `Model.cs:568-583` |
| S02 | Nothing reaches the output between a Stop returning and the next Start. | `Join` before return `:561-575` |
| S03 | While Paused (after a Pause or a paused Start) nothing is sent, and `processed` and `position` are frozen. | `WaitWhilePaused` `:3498-3513`; `FreezeTransportLocked` `:3593-3600`; `CurrentTransportTicksLocked` `:3582-3591` |
| S04 | **Classic** Pause then Resume restarts at `FindFirstEventAtOrAfter(frozen position)`: the first source event sent after Resume has exactly that index (the due-but-unsent backlog is discarded, nothing before it is resent). **Ordered** Pause/Resume (Queue) loses and repeats nothing: the source indices of the whole run are `start … N-1` once each. | `:486-499`; ordered anchors shifted by the pause time `PlaybackEngine.Ordered.cs:32-45`; `Resume` `:508-518` |
| S05 | Classic Seek: after RESET+PANIC the segment restarts at `LowerBound(target)` (Queue: that index first, contiguous after). Ordered Seek: the retained queue is delivered, then the 16-message marker `B0 7B 00 … BF 7B 00` (generated), then the new generation from `LowerBound(target)`. | classic `:410-441`; ordered `:395-407`, marker `OrderedPlaybackGenerations.cs:529-541`, `OrderedUnlimitedQueueAdapter.cs:86`, `OrderedFiniteAdmissionCore.cs:365`, emission `DispatchProtectedAllNotesOff` `PlaybackEngine.cs:1227-1239` |
| S06 | At a Start, classic Seek, Resume after a classic Pause or ordered Seek at a position with event index > 0 and the chase on, the generated messages sent before the first source event are state restores (controller, program, pitch bend, channel pressure) whose value equals the latest source value before that index (controllers 6, 38, 96–101 are parameter sequences and are exempt) and are not empty when the song has such state. With the chase off, or at index 0, none are sent. | `_pendingStateChaseEventIndex` `:349-350`, `:416-417`, `:489-490`; `ApplyPendingStateChase` `:3343-3363`; `MidiStateChaseIndex.CreateMessages` `MidiStateChaseIndex.cs:100-140`; ordered prefix `PlaybackEngine.Ordered.cs:1052-1092` |
| S07 | Source events are sent in strictly increasing index order, once each, never before the segment's start index. Queue modes: the sent indices are contiguous from the start index, and reach `N-1` when playback ended. For the gates only the order the gate defines is checked (Note attack limit and Output rate: all events increasing; Per-note interval gate and Delayed NoteOff: all non-note events increasing). This implies the per-channel order. | Queue/Drop service is FIFO `:1241-1357`, `:1359-2063`; gates dispatch non-note events at admission `:2253-2262`, `:2427-2437`, emissions per tick `:2534-2545`, `:2624-2640` |
| S08 | Every sent source event has exactly the bytes of the song event it names (SysEx included). Allowed: a velocity-0 NoteOn re-emitted as the NoteOff it means (`9n kk 00` → `8n kk 00`) by a gate; the Per-note gate and Delayed NoteOff build their note messages themselves, so their note messages are not compared by index. Every non-source message has index -1 or 0. | the sent view is the stored event `MidiEventView.Data`; the gates emit new events (`PerNoteIntervalGate.cs`, `DelayedNoteOffGate.cs`) |

### Counts

| ID | Rule | v1 source |
|---|---|---|
| C01 | At every snapshot where the engine is not Playing, `processed` equals the number of source events sent since the last statistics reset (Start and classic Seek); while Playing it never exceeds it. | `_processedEvents` counts exactly the source/gate events dispatched: `:2737`, `:2793`, `:2851`, `PlaybackEngine.Ordered.cs:1161`; resets `:359`, `:421` |
| C02 | Conservation in the segment that played out: Queue `processed = N − start`; Drop `processed + dropped = N − start` (every Drop policy counts what it discards, Clear buffer included); Per-note gate and Delayed NoteOff `processed + gateFiltered = N − start`; Note attack limit `processed + musicalRejectedAttacks + musicalSuppressedReleases`; Output rate `processed + rateRejectedAttacks + rateSuppressedReleases + rateRejectedOther`. A classic Pause adds the events it discards. Not checked when a channel is overridden or disabled (those events are neither processed nor dropped) or after an ordered Seek. | `:1685`, `:1713`, `:1737`, `:1578`, `:1991`, `:2159`, `:2692`, `:2667-2689` |
| C03 | `processed`, `dropped` and every gate counter never decrease between restarts. | counters only increase between `ResetStatisticsLocked` calls |

### Protected traffic and queue limits (Drop modes)

| ID | Rule | v1 source |
|---|---|---|
| P01 | Classic Drop with Drop newest events or Drop oldest events (no waiting-time limit, no overrides): **every NoteOff (including NoteOn velocity 0) and every safety control (CC64 < 64, CC120–127) of the stream is sent**. A protected arrival over the limit is admitted past it; Drop oldest events never evicts a protected head. | `protectedTraffic` `:1755-1756`; admission past the limit `:2019-2032`, `:1819-1831`; `oldestProtected` `:1797-1799`; `CompleteNoteTracker.Classify/IsSafetyControl` `CompleteNoteTracker.cs:43-62` |
| P02 | Complete-note policies (Drop incoming complete notes, Drop oldest complete note, Drop background notes; classic, ordered and forward-only): pairing NoteOn/NoteOff by FIFO occurrence per channel and key, a NoteOff is dropped **exactly when** its NoteOn was dropped; safety controls and unmatched NoteOffs are never dropped. | `TakeNoteOff` / "An unmatched NoteOff is never suppressed" `CompleteNoteTracker.cs:93-117`; `noteMatch.Suppress` `PlaybackEngine.cs:1680-1690`; `ownAttackEvicted` `:1875-1881`, `:1932-1937`; ordered: `OrderedFiniteAdmissionCore.cs:250-300`; forward-only: `PlaybackEngine.cs:2110-2113`, `:2119-2124` |
| P03 | A PANIC during play (one not directly after a RESET) comes only from Clear buffer and catch up, and at least once if it dropped anything; no other policy sends one. | `:1582`, `:1995` |
| Q01 | A finite queue drops only when its limit was reached (`dropped > 0` ⇒ `maxQueue ≥ limit`); a limit larger than the song drops nothing. Not applied to the waiting-time limit, to the ordered path or with live limit edits. | admission test `:1759`; overflow branch `:1792-2040` |
| Q02 | Snapshot sanity: non-negative counters and queue figures, `intended` within `[0, duration]`, lag ≥ 0, `processed ≤ N`, legal state; the forward-only Drop newest events virtual queue never exceeds its limit. | `GetSnapshot` `:1015-1095`; `ForwardDropQueue.TryAdmit` `ForwardDropQueue.cs:40-58` |

### Channel controls

| ID | Rule | v1 source |
|---|---|---|
| O01 | `override ch attr value` while Playing: the forced message (`ChannelOverrideState.CreateMessage`) is sent exactly once, and from then on a source event of that attribute and channel is sent only if its value equals the forced value (a CC121 that would reset a forced attribute is suppressed too). There must be at least one such source event, or the check is vacuous and fails. | `SetChannelOverride` `:746-788`; `ApplyPendingOverrides` `:3293-3341`; `ShouldSuppress` `ChannelOverrides.cs:205-226`; `CreateMessage` `:256-283` |
| O02 | `channel ch off` while Playing: the worker sends CC64=0, CC120=0, CC123=0 on that channel (in that order, generated), and afterwards no source event of that channel is sent until it is enabled. | `ExecuteChannelControl` `:918-975` (`:961-963`); `SubmitChannelControl` `:844-877` |

### Timing

| ID | Rule | v1 source |
|---|---|---|
| M01 | No source event is dispatched before its due time: in a Start segment with no Pause/Resume/Seek/Start after it, every source send happens no earlier than (time of the Start command) + (event time − start position). | an event is dispatched when its time is due: Queue `FindFirstEventAfterTransport(nextArrival, now)` `:1290`, Drop `arrivalTicks <= now` and `completionTicks <= now` (`RunDropMode`), gates `nextAction > nowMicroseconds` `:2235`, `:2389`, `:2521`, `:2610` |

**Quality properties: v2 must be equal to or better than v1.** v2 is no longer meant to reproduce v1's timing (its lateness, jitter and command latency were measured in the observations below to be much tighter, and v2 will get tighter still), so these three do not ask for equality; they ask for "no worse" than v1's own measurement of the same configuration, on the same architecture, in the same run. A value below v1's always passes. The comparison has a noise margin because the PC is not quiet and four lanes run at once: v2 passes when `v2 <= 1.25 x v1 + margin`. Ordering, count and correctness properties (everything else in this document) stay exact; these three only add checks.

| ID | Rule | Margin (added to 25 %) |
|---|---|---|
| L01 | Dispatch lateness: for every configuration with Queue mode and no service time that ran in both programs (single uninterrupted Start segments, repeats pooled, at least 20 samples), the median lateness of v2 is no worse than v1's; with at least 100 samples the 99th percentile as well. | median 0.2 ms, 99th percentile 1 ms |
| J01 | Dispatch jitter: the spread (99th percentile minus median lateness) of v2 is no worse than v1's (at least 100 samples). | 1 ms |
| K01 | Command-to-effect latency: for `override` and `channel disable` (at least 5 measurements in each program), the median and the worst latency of v2 are no worse than v1's. | median 0.5 ms, worst 5 ms |

The v1 lane of each comparison gets a passing check for every comparison it served as reference in, so a quality property that never ran is still reported `NOT-EXERCISED`. The thresholds are constants at the top of `QualityChecks` in `PropertyChecker.cs`.

### Cross-run

| ID | Rule |
|---|---|
| X01 | For `class=exact` configurations the sent sequence (kind, index, bytes, RESET, PANIC) is identical in every run: repeats, v1/v2, x64/x86. SysEx events of 1–3 stored bytes are compared by length (DD-001). |
| X02 | …and so are the final counters (processed, dropped, gate counters). |
| X03 | The chase block of a start index (the generated messages before the first source event) is identical wherever it occurs: every program, architecture, repeat, scenario (Queue, Drop, gates, ordered, Resume, Seek). |

## 4. v1 behaviours the checks account for (all ported unchanged by v2)

1. **DD-001 (an improvement, `DIFFERENCES_FROM_V1.md`)**: v1 reads a SysEx event of 1–3 stored bytes back as zeros; v2 keeps the file's bytes. The event lists and X01 compare those events by length; each program's log is checked against its own event list.
2. **Classic Pause discards the backlog**: events due before the frozen position but not yet sent are never sent (`PlaybackEngine.cs:486-499`). With a lagging worker (v1 cold start, dense bursts) that can be hundreds of events. The ordered path keeps them.
3. **Ordered Drop does not count suppressed releases** (suspected v1 defect, display only): `PlaybackEngine.Ordered.cs:898-903` adds `result.Evicted.Length` plus one for a rejected event, but skips the rejected NoteOff of an already dropped NoteOn (`PairedReleaseSuppressed`, `OrderedFiniteAdmissionCore.cs:250-256`), whereas the classic path counts it (`PlaybackEngine.cs:1685`). `processed + dropped` is then less than `N − start` by the number of such releases. C02 requires the shortfall to be **at most the number of NoteOffs whose NoteOn was also unsent** and reports the amount.
4. **Forward-only Drop newest events has no NoteOff protection**: `ForwardDropQueue.TryAdmit` rejects any event at the limit unless `safetyAdmission`, which only Drop incoming complete notes sets (`:2119-2124`). P01 therefore does not apply to forward-only; P02 does for Drop incoming complete notes.
5. **A CC121 is suppressed by an override** whose attribute it would reset (`ChannelOverrides.cs:213-221`), so P01 is not applied in scenarios with overrides.
6. **Gates rewrite notes**: the Per-note gate and Delayed NoteOff emit their own note messages (S08), and a velocity-0 NoteOn leaves as `8n kk 00`.
7. **Not covered on purpose**: the ordered-path stale override mask livelock (`ENGINE_SCENARIOS.md` section 11; it hangs the worker, so a real-time run would only time out), channel monitoring, the drop/dispatch traces, `route`, a genuinely hung output call (the 2 s acknowledgement timeout is covered by `midibn-engine-tests`), the Windows XP builds.

## 5. Scenarios (what each one exercises)

See `v2/harness/engine-realtime-scenarios.txt` (the `describe` lines). Groups: Queue (immediate, 300 µs service, ordered), the gates (Per-note, Delayed NoteOff, Note attack limit, Output rate limit, ordered Per-note, ordered Output rate with the Output-paced timeline), Drop (every overflow policy on `models-dense` and on the synthetic protected-traffic song, waiting-time limit, forward-only, ample limit, live edits, ordered), transport (classic and ordered Pause/Resume on sparse and dense songs, classic Seek, ordered Seeks with and without a retained backlog, Pause+Seek+Resume, Stop/Start, Start while playing, Start paused), chase (mid-song, off, ordered gate) and channel controls (overrides set and cleared, channels disabled and enabled, over a slow finite queue, and on the ordered path with the Output-paced timeline, where the ordered worker busy-polls the clock while a live control or an Output-paced service hold is pending).

## 6. Results

Run on this PC (Windows 10 19045, 12 logical processors, screen locked), tree at commit `b85a37d` plus the uncommitted files of this work: `engine-properties.ps1` with the defaults, 44 configurations x 5 repeats x 4 lanes (v1 and v2, x64 and x86) = 880 real-time runs in 479 s including the build. **Every run terminated, 0 process failures, `ENGINE-PROPERTIES: PASS`.** Song event lists: identical in v1 and v2 for all 7 fixtures except the 14 + 5 very short SysEx events of DD-001.

Checks that ran (each program/architecture lane; v1 x64 / v2 x64 / v1 x86 / v2 x86 all PASS, none NOT-EXERCISED):

| ID | checks per lane (about) | ID | checks per lane (about) |
|---|---|---|---|
| T01 T02 T03 | 220 each | P01 | 20 |
| S01 | 725 | P02 | 25 |
| S02 | 225 | P03 | 65 |
| S03 | 30 | Q01 | 55 |
| S04 | 20 | Q02 | 10,400-10,500 |
| S05 | 45 | O01 | 90 |
| S06 | 265 | O02 | 60 |
| S07 | 265 | M01 | 383,000-391,000 source dispatches |
| S08 | 501,000-511,000 sends | X01 X02 | 85 (17 exact configurations x 5 repeats) |
| C01 C03 | 10,200-10,300 | X03 | 59-69 chase blocks |
| C02 | 175 | | |

Cross-program equalities (X01-X03): the 17 `class=exact` configurations (Queue and ordered Queue, the three song-time gates, chase at 60/70 percent and with the chase off, sparse classic and dense ordered Pause/Resume) produced the **same sequence of sends (kind, source index, bytes, RESET, PANIC) and the same final counters in all 20 runs of each** (5 repeats x v1/v2 x x64/x86). Every chase block for the same start position was identical wherever it occurred (classic, ordered, Drop, gates, Resume, Seek).

Observed but not required: 15 non-exact configurations (including Drop newest events, Drop oldest events, incoming/background/oldest-note, the waiting-time limit and forward-only Drop) gave identical processed/dropped counts in every run of all four lanes (`STABLE` in `results.txt`); the real-time-dependent ones differ as expected (Clear buffer: processed 34-124 in v1 against 581-588 in v2; Output rate limit 532-568 against 537-542; classic Pause on the dense song: v1 loses 350-520 events to the discarded backlog, v2 0-2; the ordered Drop path, 2,100-3,260 processed, depends on the queue's real-time evolution).

### Findings

1. **No v2 engine discrepancy was found.** No property failed in v2 that passed in v1, and the exact class is bit-identical.
2. **v1 defect, fixed in v2 by DD-008 (the runs recorded here predate the fix; C02 keeps accepting the shortfall for v1).** In the ordered finite Drop path, a NoteOff suppressed because its NoteOn was dropped is not counted in `DroppedEvents` (`PlaybackEngine.Ordered.cs:898-903` versus the classic path's `PlaybackEngine.cs:1685`): 2,108-2,337 uncounted per run on `rt-protected`. C02 therefore accepts a shortfall only up to the number of NoteOffs whose NoteOn was also unsent.
3. **Forward-only Drop newest events does not protect NoteOffs** (`ForwardDropQueue.TryAdmit`, `PlaybackEngine.cs:2119-2124`): stuck notes are possible with that combination. Kept as v1 by the user's decision (DD-025 in `DIFFERENCES_FROM_V1.md`: the proposal to protect them was declined; stuck notes are an accepted consequence of dropping single events).
4. **DD-001** (short SysEx bytes) is the only register entry met; handled in the comparison. (The property checker does not read the rule file of the parity runner: a later entry that changes what these real-time checks see has to be handled in `PropertyChecker.cs` and `engine-properties.ps1` as DD-001 is.)

**Quality properties added (contract change, x64 lanes only, 5 repeats, other agents building on the same PC):** `ENGINE-PROPERTIES: PASS`, 440 of 440 runs; L01 15 checks, J01 7, K01 4, all passed, every earlier property unchanged (S08 about 501,000 sends, M01 about 383,000 dispatches per lane). On that busy PC the measured medians were: lateness rt-queue-dense v1 7,119 us / v2 6,696 us (p99 16,690 / 14,860), rt-chase-mid v1 728 / v2 394 (p99 16,136 / 14,487); command latency override v1 2,047 us / v2 37 us, channel disable v1 2,758 / v2 36 us. v2's lateness is therefore currently about equal to v1's (it still carries the ported `SpinWait`/`Sleep(1)` approach), which is exactly where the noise margin matters. Register entry DD-002 then removed that lateness from v2 (see the end of "Timing observations").

### Timing observations (the numbers behind L01, J01 and K01)

Lag = how far behind its due time a source event is sent, in microseconds, pooled over the repeats, four lanes running in parallel on an otherwise idle 12-thread PC. The `SpinWait` change of `b85a37d` was made between the first and the second run.

| scenario (x64) | v1 p50 / p99 | v2 before the spin port p50 / p99 | v2 after p50 / p99 |
|---|---|---|---|
| `rt-queue-dense` (two 4,500-event bursts) | 6,500 / 14,900 (max 17,200) | 8 / 271 (max 809) | 38 / 226 (max 816) |
| `rt-queue-ordered-dense` | 7,100 / 16,800 | 124 / 443 | 175 / 445 |
| `rt-chase-mid` (sparse, 447 events) | 500-580 / 15,100-15,250 | 203 / 844 (one 21.8 ms outlier) | 266 / 869 (max 1.1 ms) |
| command to effect (override / channel disable), median, max | 2.1-2.8 ms, 16 ms / 3.0-3.1 ms, 13 ms | 28, 237 / 12, 69 | 29, 167 / 13, 318 |

x86 is the same (v1 p99 14.4-15.1 ms on the same scenarios; v2 after: dense p50 39 / p99 107, sparse 223 / 846). No dispatch was ever early (M01).

v1's tail is the final-approach `SpinWait`: .NET Framework's `SpinOnce` calls `Thread.Sleep(1)` on every 20th yield, and `Sleep(1)` takes about 15-16 ms here (measured: 60 `SpinOnce` calls took 30 ms, the slowest 15 ms; a native `Sleep(1)` loop also takes 16 ms, before or after creating a high-resolution timer). The port in `b85a37d` follows that code, yet **v2's lag is still not like v1's**: after the port v2's p99 stays below 1 ms and v2's command latency stays at tens of microseconds, so the port did not make v2 resemble v1 on this machine. The `SpinWait` itself is faithful (the busy phase of v1 is 128 us here, about what 10 x `YieldProcessor(4<<i)` costs), so the difference is probably earlier in the wait: how long the high-resolution timer wake leaves before the target, or how often the worker reaches the 30th `spin_once` of a wait. It is not explained by this data; counting `Sleep(1)` calls inside `Win32Waiter::spin_once` on a dense run would settle it. It affects timing only, in v2's favour.

**DD-002: the final approach no longer sleeps.** The `SpinWait`/`Sleep(1)` port was removed (`Win32Waiter::spin_once` now pauses the processor and the caller re-reads the clock; after 1 ms of spinning, or on a single-processor machine, it yields the time slice). Two runs of `engine-properties.ps1 -NoBuild -Repeats 3` on the same busy PC, before and after (x64 / x86, lag in microseconds, p50 / p99): `rt-queue-dense` v2 206 / 12,082 and 609 / 14,553 before, 12 / 491 and 12 / 323 after, against v1 4,509-6,294 / 15,781-24,918 and 5,178-5,343 / 15,024-16,251; `rt-queue-ordered-dense` v2 5,653 / 19,972 and 13,092 / 30,583 before, 1,113 / 8,698 and 624 / 11,381 after (v1 4,420-6,962 / 15,438-18,073; the ordered path adds per-event queue work, so its lag is dominated by the backlog of the 4,500-event bursts, not the final approach); `rt-chase-mid` p99 11,919 and 15,104 before, 5,632 and 4,618 after (v1 8,471-13,245 and 10,391-11,112). Command latency (override / channel disable, median): v2 17-34 us before and after, v1 1.5-4.0 ms. The run before the change ended `ENGINE-PROPERTIES: FAIL` (L01, J01 and K01 on x86: v2 worse than v1 because it carried v1's `Sleep(1)`); the run after ends `PASS`.

### Not covered

The ordered-path stale-override livelock (it would hang), channel monitoring, `route`, a hung output call (covered by `midibn-engine-tests`), the XP builds (`engine-properties.ps1 -Archs` takes `x64` and `x86` only), and any real device or provider.
