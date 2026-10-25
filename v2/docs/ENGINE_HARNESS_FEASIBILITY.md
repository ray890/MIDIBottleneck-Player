# Can v1's playback engine run deterministically inside the harness? (Milestone 2 Step 4, Milestone 3 Step 2)

*Historical record, written while v2 was still meant to be an exact copy. "Bit-exact" below means exact recordings of v1's engine against which v2's are compared; under the current contract (all v1 features, improved) a difference passes when a register entry explains it (`DIFFERENCES_FROM_V1.md`).*

**Update (Milestone 3 spike, then the Pause/Seek follow-up): feasible for Play, Stop, restart, live setting changes, steady-state playback, Pause/Resume and Seek, in both the classic and the playback-preserving ("ordered") paths; see the last sections.** The Milestone 2 finding follows unchanged.

**Milestone 2 finding: partly feasible.** Replacing v1's time and wait sources without editing any v1 file is **feasible and proven** by a small proof of concept. Making a whole playback run deterministic also needs control over *when* the engine's real worker thread runs relative to the harness's commands. That part is plausible but unproven, and the Pause paths need extra work.

## What was checked

v1's playback engine (`PlaybackEngine.cs`, `PlaybackEngine.Ordered.cs`) reads time only through the static `Stopwatch.GetTimestamp()` and `Stopwatch.Frequency` (85 uses across the two files). It waits only through:

- `HighResolutionWaiter.WaitMicroseconds` (a kernel waitable timer);
- a `SpinWait` loop for the final ~200 µs (`PlaybackEngine.cs:3550`, `PlaybackEngine.Ordered.cs:1194`);
- `EventWaitHandle` waits: zero-timeout polls, the paused worker's untimed `_wake.WaitOne()` (`PlaybackEngine.cs:3512`), and the control thread's `_pauseAcknowledged.WaitOne(timeout)` (`PlaybackEngine.cs:471`).

The engine creates no timers or tasks. The only other `Stopwatch` use outside the UI is the WinMM readiness probe (`WindowsMidiOutput.cs:128`), which a harness with its own output never calls.

## Proof of concept (`v2/harness/v1-reference/poc/VirtualTimePoc.cs`)

- **Method.** The PoC compiles every v1 source except `Program.cs` and `HighResolutionWaiter.cs`, unchanged and verified identical to tag `v1.0.44`. It adds three harness types in the `MidiBottleneck` namespace:
  - `Stopwatch`: a virtual 1 MHz clock;
  - `SpinWait`: each spin advances virtual time by 1 µs;
  - `HighResolutionWaiter`: same surface as v1's; waiting advances virtual time.
- **Why it works.** C# looks up a type name in the enclosing namespace before the types imported by `using System.Diagnostics;` / `using System.Threading;`. The replacements therefore apply to every v1 file without touching it, and the compiler raised no errors or ambiguities.
- **Result: `POC: PASS`.**
  - v1's own `PlaybackEngine.MicrosecondsToTicks(1234)` returns 1234 and `TicksToMicroseconds(987654)` returns 987654 under the virtual clock.
  - v1 code reads the virtual timestamp.
  - The engine's `WaitUntil` is bound to the harness's `HighResolutionWaiter`.

Build it with the harness's `csc.exe` command line, leaving out `HighResolutionWaiter.cs`; the output is `v2\out\poc\virtual-time-poc.exe`.

## What still blocks bit-exact engine runs

1. **Real worker thread.** Play, Seek and settings changes start a real `new Thread(PlaybackWorker)` (`PlaybackEngine.cs:375/434/499`). A virtual clock alone does not fix the order in which the harness's commands and the worker's steps interleave. The natural fix is a lock-step rendezvous: the virtual waiter blocks the worker and hands control to the harness, which then applies scripted commands, advances virtual time, and resumes it. Steady-state playback waits only in the waiter, so it is a single, well-defined scheduling point. This is not yet built or tested.
2. **Pause.** The paused worker blocks on an untimed real `_wake.WaitOne()`. The control thread waits on `_pauseAcknowledged.WaitOne(timeout)` with a real-millisecond timeout. Under a lock-step scheduler these need either a shadowed `EventWaitHandle`, which is risky because the shadowing is compilation-wide, or harness-side knowledge of those two states. Pause/Seek-preserving scenarios therefore carry the most risk.
3. **Output cost.** A harness `IMidiOutput` can advance virtual time inside each send to script service time. That covers the wall-clock-dependent decisions listed in ARCHITECTURE.md §5.1: Output-rate dispatch time, the Drop external-backlog switch, and ordered admit-before-retire.
4. **Compilation-wide shadowing.** The replacement `Stopwatch` also replaces it in the UI forms. That does not matter in a headless harness, but the harness must never call the WinMM probe, whose 5-second loop would not end unless sends advance time.

## Recommendation

Before Milestone 5, spend a bounded spike (no engine port) on the rendezvous scheduler for Play, Stop and steady-state playback over the existing fixtures, using a recording output. If v1 runs repeat byte-identically under it, Milestone 5's live-engine parity can be bit-exact for scripted scenarios instead of relying on "structural equivalence plus property checks"; Pause and Seek-preserve scenarios would then be evaluated separately. If the spike fails, the §5.1 fallback stands.

## Milestone 3 spike: lock-step rendezvous (result)

**Result: success within the approved scope.** Repeated v1 runs of the same script produce byte-identical recorded output, on x86 and x64, with v1's sources unchanged (verified identical to `v1.0.44`). Code: `v2/harness/v1-reference/spike/` (`build-spike.ps1`, `run-spike.ps1`).

**How it works.**
- The proof of concept's shadow `Stopwatch`, `SpinWait` and `HighResolutionWaiter` remain. (`EventWaitHandle` was not shadowed in this first spike; the follow-up below adds it.)
- The replacement `WaitMicroseconds` is a rendezvous. It keeps v1's early return when `_wake` is already set. Otherwise the worker parks with a deadline and blocks until either the harness releases it or v1 signals `_wake`.
- While the worker is parked, the harness either applies its next scripted command (advancing virtual time to the command's time first) or advances virtual time to the deadline and releases the worker. Only one of the two runs at a time.
- An overloaded worker never waits, because each send advances the clock. So the worker also yields after a completed send when a command is due, and only while the engine is playing. That yield is released by a change in v1's wake generation, because v1 leaves `_wake` set during long dispatch runs.
- Stop runs on the harness thread while the worker is parked. Its wake signal releases the worker, which exits and sends Reset and Panic on its own thread. Start while playing works the same way, since it calls Stop first.
- The recording `IMidiOutput` logs every send, Reset and Panic with the virtual time and the sending thread, and advances virtual time by the message's bytes at 31,250 bit/s.

**Evidence.**
- Six scenarios on every one of the 23 valid fixtures, 5 runs each on x86 and x64: 276 output and schedule files, 0 divergent, no hangs or failures. The scenarios are:
  - Queue mode to the end;
  - Stop then Start at a later position (state chase);
  - Start while playing;
  - a bitrate-to-processing-time change while playing;
  - Drop mode with a 32-event limit;
  - the ordered (preserve-on-Pause/Seek) path with Stop.
- The same matrix also stayed identical with 12 CPU-burner processes loading the machine.
- **Negative control:** the same build with the rendezvous switched off. Every scenario with a mid-run command diverged (42 of 72 files). The command-free scenarios stayed identical and match the lock-step hashes, which shows the scheduler fixes command timing without changing what v1 sends.
- **Independent check (main session):** a fresh build, 4 fixtures × 6 scenarios × 3 runs × 2 architectures, gave 48 files and 0 divergent. With the rendezvous switched off, 16 of 24 files diverged.

**Modelling choice.** A command is delivered at the first scheduling point at or after its scripted time: a timed wait, or the end of a send once the command is due. This produces one canonical, reproducible v1 trace for a script. It is not a copy of any particular real-time run, in which a command could land mid-computation.

**Recommendation for Milestone 5 (from the first spike).**
- Use bit-exact live-engine parity for scripted Play, Stop, restart and setting-change scenarios across the queue, drop and ordered paths, with this harness producing the v1 reference. The v2 engine test harness must deliver commands at the same points (timed waits, and after a completed send once a command is due) with the same send-cost script.
- Superseded by the follow-up below: the `EventWaitHandle` stand-in is built, so Pause and Seek-preserving scenarios can be bit-exact too.

## Pause and Seek follow-up: `EventWaitHandle` stand-in (result)

**Result: success.** The spike now also runs v1's Pause, Resume and Seek, in the classic path and in the preserve-on-Pause/Seek ("ordered") path, and the recorded output is byte-identical across repeated runs, on x86 and x64. v1's sources are still unchanged (`build-spike.ps1` still refuses to build unless all 62 files used are byte-identical to `v1.0.44`). Code: `v2/harness/v1-reference/spike/` (new `EventWaitHandleShadow.cs` and `SpikeSelfTest.cs`; changes to `LockStepScheduler.cs`, `RendezvousSpike.cs`, `run-spike.ps1`).

**How the stand-in works.**
- v1 uses `EventWaitHandle` for exactly two fields, `_wake` and `_pauseAcknowledged` (checked by searching all of `src/`; the only other user is `HighResolutionWaiter`, which the spike already replaces). The shadow type has the constructor `(bool, EventResetMode)`, `Set`, `Reset`, `WaitOne()`, `WaitOne(int)` and `Dispose`, which is everything v1 calls.
- The signalled/clear state, and AutoReset versus ManualReset behaviour, live in a real operating-system event, so `Set`, `Reset` and the zero-timeout poll behave exactly as before. Only the two blocking waits are redirected to the scheduler:
  - **Paused worker, `_wake.WaitOne()` (untimed).** It becomes a park with no deadline. Only `Set` releases it; the harness never advances virtual time to release it. A paused run therefore sits at its virtual time until a scripted command (Resume, Stop, a setter, a Seek) arrives. If a paused run has no commands left, the driver ends it with a Stop at the current virtual time.
  - **Control thread, `_pauseAcknowledged.WaitOne(2000)` (timed).** The control thread is the harness thread, inside a Pause command. It does not block the schedule. It lends the turn to the worker. If the worker sets the handle (the acknowledgement), the wait returns true. If the worker parks without acknowledging, virtual time moves to whichever comes first: the worker's deadline (the worker is then released) or the 2000 ms control deadline (the wait returns false). The timeout is therefore in virtual time and cannot depend on real time.
- The replacement `WaitMicroseconds` still returns early when `_wake` is already set, as before.
- One new rule was needed. v1's non-preserving Pause and Seek start the replacement worker thread and then call `SignalWake` once more. The new worker could start before or after that last wake signal, depending on real-time scheduling. The first full run showed this: 10 of 966 recorded files (all scheduling traces, from one process) differed. The fix is a start gate: while a harness command is running, a newly started worker waits before doing anything, and runs once the command has returned. After the fix there were no differences.

**Scenarios added (15, on top of the six above).** The commands are v1's public API as MainForm uses it: `Pause()`, `Resume()` (the Play button while paused), `Seek(target)` (timeline), `Stop()`, `Start()`. Seven scenarios run in the classic path (`queue-*`) and again in the preserving path (`ordered-*`, with `PreservePlaybackQueueOnPauseSeek`, a 300 µs processing cost and the retained queue):
- `pause-resume`: Pause at 30% of the song, Resume at 50%.
- `pause-seek-resume`: Pause at 30%, a Seek to 70% while paused at 40%, Resume at 50%. In the preserving path this is the position-keeping Seek.
- `seek-playing`: a Seek to 70% at 30% of the song and a Seek back to 10% at 50%, while playing.
- `pause-stop`: Pause, then Stop while paused.
- `pause-at-end`: Pause and never resume; the driver ends the run with a Stop.
- `pause-then-start`: Pause, then Start at a new position while paused.
- `pause-resume-twice`: two Pause/Resume pairs (a second pause acknowledgement).

The fifteenth is `ordered-drop-pause-seek-resume`: the preserving finite-queue (Drop, limit 32, 1500 µs) path through Pause, Seek while paused and Resume.

For these 15 scenarios the output file also logs the engine's snapshot (state, processed count, position, intended position, worker state) after each command, so the Pause and Seek effects are visible in the recorded output. The older six scenarios do not log it, so their files are unchanged.

**Evidence.**
- **Matrix:** 21 scenarios (6 old, 15 new) on each of the 23 valid fixtures, 5 runs each on x86 and x64, each run a fresh process: 966 recorded files, each with 10 copies, **966 identical, 0 divergent**; no hangs, no process failures, and no engine errors or command exceptions in any recording.
- **Under load:** the same matrix with 8 runs per architecture and 12 CPU-burner processes running: 966 of 966 identical.
- **Old scenarios unchanged:** the 276 files of the six original scenarios were produced with the pre-change build and again with the new build. Every hash matches, on x86 and on x64.
- **Negative control** (same build, rendezvous off, `-Mode free`, 5 runs per architecture): the new scenarios diverge (337 of 345 output files and 296 of 345 trace files differ between runs); the six old ones diverge as before (89 of 138 output files, 76 of 138 trace files). The command-free scenarios (queue to the end, Drop) stayed identical. Nothing hung.
- **Self test of the stand-in** (`rendezvous-spike.exe --selftest`, both architectures, run before each matrix): AutoReset is consumed by one wait and ManualReset is not; a control wait of 2000 ms against a worker parked for 10 s times out at exactly 2000 ms of virtual time and leaves the worker parked; a control wait of 20000 ms releases the worker at its 10 s deadline, returns true when the worker acknowledges, and leaves the worker waiting with no deadline; only `Set` releases that wait; a control wait with no live worker times out at exactly the timeout. All 11 checks pass, and the test was repeated 8 times on each architecture. The control-wait timeout is exercised only by this self test: in all 15 real scenarios the worker acknowledged within the wait.
- **No hangs:** each scheduler wait has a 20 s real-time limit. The program has an in-process watchdog (300 s per scenario by default, `SPIKE_WATCHDOG_SECONDS`) that prints the stuck scenario and exits with code 3. `run-spike.ps1` also kills a process after its own timeout. The watchdog was checked with `--selftest --hang` and a 2 s limit: it ended the run with exit code 3.

**Limits and notes.**
- The delivery rule is unchanged: a command lands at the first scheduling point at or after its time. A Pause is therefore delivered at a timed wait or after a completed send, never inside a send.
- The 2000 ms acknowledgement timeout is judged only where the worker parks. A worker that ran past 2000 virtual ms without ever parking (a very expensive send) would not trigger it. No scenario does this.
- Seek while paused in the preserving path only queues the request and updates the intended position; the worker applies it on Resume. The recordings show exactly that.
- No v1 behaviour that looks like a defect was found. One benign real-time race exists in v1 itself: the non-preserving Pause and Seek call `SignalWake` after starting the replacement worker, so the new worker may or may not see that wake. It only causes an extra early wake-up, and the harness's start gate hides it.

**Recommendation for Milestone 5.**
- Treat Pause, Resume and Seek as bit-exact live-engine parity scenarios too, across the classic and preserving paths, with this harness producing the v1 reference. The v2 engine test harness must deliver commands at the same points and model the acknowledgement of a preserving Pause the same way: the command waits for the worker's acknowledgement while the worker runs, and a paused worker advances no virtual time.
- Keep the section 5.1 fallback (structural equivalence plus property checks) only for behaviour this harness cannot reach: a hung output call and the real 2000 ms Pause/Stop timeouts under real time.

## Milestone 5 scenario extension (result)

**Result: success.** The scenarios are now data, and 140 new ones cover the engine options that the first 21 did not. Code: `v2/harness/v1-reference/spike/` (new `ScenarioFile.cs` and `ScenarioBinder.cs`; changes to `RendezvousSpike.cs`, `LockStepScheduler.cs`, `VirtualClockShims.cs`, `run-spike.ps1`). The scenario file is `v2/harness/engine-scenarios.txt`; its grammar, the engine defaults, the time formulas and the exact recorded-output format (every line of `.out.txt` and `.sched.txt`) are specified in `v2/harness/ENGINE_SCENARIOS.md`. The future C++ harness reads the same file. v1's sources are still unchanged (`build-spike.ps1` verifies 62 files against `v1.0.44`).

**Data-driven.** The spike reads the file (`--scenarios`, default the committed one; `--only` filters; `run-spike.ps1 -ScenarioFile -Only`). The 21 earlier scenarios are written in it as plain lines and reproduce their recordings exactly: all 1,932 recorded files (966 per architecture, output and schedule) of a full pre-change run are byte-identical to the same run after the change.

**New scenarios (140), all on the 23 valid fixtures.** Every one logs the engine snapshot (state, counters, positions, and the gate and queue counters) after each command.
- Rate models: none, processing time, MIDI bitrate and events/sec in Queue mode, with live edits and live model switches (8).
- Drop mode: all six overflow policies; the queue waiting-time limit with its three policies; forward-only (apply the limit without slowdown) with its two policies; the other rate models; live limit and policy edits (15).
- Gates: Per-note interval (plain, zero, interval scaling, polyphony limit), Delayed NoteOff, Note attack limit and Output rate limit, each also with a live value change as MainForm applies it (stop, new value, start again at the intended position, also while paused) and with Pause, Seek and Resume (18).
- The ordered (preserve-on-Pause/Seek) path with every rate model, every finite-queue policy, forward-only and every gate (26).
- Output-paced timeline, classic and ordered, with all four gates on the ordered path (14).
- Whole-state chase on and off: Start and Seek at a mid-song position, Pause/Seek/Resume, over Queue, Drop, gates and the ordered path (22).
- Live channel overrides (set and cleared while playing, paused, stopped, before Start) and disabled channels, over Queue, Drop, gates and the ordered path (22).
- Track routing, applied as MainForm does: exact-safe, GM-family and aggressive tiers (capacity 1 to 4), over Queue, Drop, gates, the ordered path, chase and overrides (15). Across the 23 fixtures the compiled plans include 5 with generated patch messages and 4 to 5 with omitted lanes (aggressive tier, capacity 1 or 2); a plan v1 rejects as invalid plays unrouted and says so in the `ROUTE` line.

**Evidence.**
- **Matrix:** 161 scenarios on each of the 23 valid fixtures, 5 runs on x86 and 5 on x64, each run a fresh process: 7,406 recorded files (161 x 23 x 2), each with 10 copies, **7,406 identical, 0 divergent**; no hangs, no process failures. Both self tests passed first (11 of 11 checks on each architecture).
- **Old scenarios unchanged:** 1,932 of 1,932 file hashes (x86 and x64) match the pre-change baseline.
- **No errors in any recording:** no `PlaybackFailed`, `CMD-EXCEPTION`, `HARNESS-ERROR`, `ChannelControlFailed` or protocol error in the files of the first x64 run and the last x86 run (all runs are identical).
- **Negative control** (rendezvous off, `-Mode free`, 5 fixtures, only the 140 new scenarios, 3 runs per architecture): of 700 new-scenario output files 630 diverge between runs, and 290 of 700 schedule traces; the other 70 output files happened to stay identical. Nothing hung.

**Two v1 behaviours found (recorded, not changed).**
- *Ordered path busy-polls the clock.* While a live channel control (override, enable/disable) or an Output-paced service hold is pending and the next source event is already due, v1's ordered workers loop with no wait until the in-service item completes. A virtual clock cannot advance in a poll loop. The scheduler now fails such a worker after 5,000,000 clock reads at one virtual time (`PlaybackFailed`, "busy-poll livelock") instead of hanging for the 20 s limit; no committed scenario reaches it. The ordered scenarios with controls or with the Output-paced timeline run with the simulated slowdown off. The classic path with the Output-paced timeline and the slowdown on is covered.
- *Suspected defect: stale override mask on the ordered path.* Clearing the last channel override before the worker applied the pending one leaves the override "pending output" mask set. All four ordered workers then stop admitting source events and spin, so playback freezes, and a later Start does not clear it. A minimal repeatable case: ordered path, Pause, set and clear an override while paused, Resume. The classic path is not affected. Details and evidence location are in `ENGINE_SCENARIOS.md`, "Known limits".

**Not driven through the public API.** Channel monitoring, `ChaseLatestSourceChannelAttribute` and `SendExplicitChannelAttribute` are not scenario commands (they belong to the monitor window); the drop and source-dispatch traces and the testing hooks are not used.

**Nine scenarios added to close mutation-check gaps (170 in all).** The Milestone 5 engine mutation check (`v2/tools/mutation-check.ps1 -Family engine`) found five changed decisions that the first 161 scenarios did not detect; section I of `engine-scenarios.txt` adds nine scenarios (two events/sec scenarios with a carried remainder, two ordered Output-paced scenarios with a real 1 us service time, two ordered Clear-buffer scenarios with disabled channels, three mid-song Output-rate scenarios whose first decision falls on a slot edge). Evidence for the added scenarios: 170 scenarios on the 23 fixtures, 3 runs on x86 and 3 on x64: 7,820 recorded files, each with 6 identical copies; no `PlaybackFailed`, `CMD-EXCEPTION`, `HARNESS-ERROR` or livelock in the files of the first x64 run and the last x86 run; the 7,406 files of the first 161 scenarios are byte-identical to the committed digests (the digest lists gained only the nine new lines each). Findings:
- *The ordered Output-paced hold is reachable in the lock-step harness after all, with a 1 us service time.* The busy-poll described above needs a source event inside the hold; with a 1 us hold none falls inside it on any of the 23 fixtures (the next source event is later than the hold start by at least 1 us). A 3 us hold livelocks on `decimal-halfway` (the harness error "worker polled the clock 5000000 times ... busy-poll livelock"), so the scenarios use 1 us.
- *A rare extra wake in the v1 spike under heavy load.* In one of six runs that were started in parallel on a 12-thread PC (six spike processes at once), `ordered-drop-forward-drop-newest` on `sysex-short` recorded one additional `RESUME-WAKE` + `WAIT` pair after `CMD Pause` in its `.sched.txt` (the `.out.txt` was identical). The other five copies, and a repeat of that run alone, match the committed digest. The recordings are not written to the digests unless every copy agrees, so the digests are unaffected; the extra wake is a timing-dependent event in v1's real threads that the rendezvous does not exclude (cause not investigated; the Pause path signals the worker), not seen in earlier runs (five runs per architecture, run one at a time). Practical rule: run the spike for the digests with at most a few instances at a time, and treat a lone differing `.sched.txt` as a reason to rerun, not as a v2 difference. The evidence is kept in `v2/out/spike/evidence-sched-spurious-wake-under-load-x64-run1/` (untracked).
