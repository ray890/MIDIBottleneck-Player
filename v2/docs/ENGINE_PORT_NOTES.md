# Engine port notes (Milestone 5, Group D)

Port of v1.0.44's playback engine (`src/PlaybackEngine.cs`, `src/PlaybackEngine.Ordered.cs` and the classes only the engine uses) to the portable C++17 core, as a port that keeps v1's behaviour except where the improvement register (`DIFFERENCES_FROM_V1.md`) says otherwise. Group D delivered the engine, the platform access it needs, and deterministic smoke/property tests; Group E's comparison with v1's recordings (exact, every difference explained by a register rule) is at the end of this file.

## Files

| Path | Contents |
|---|---|
| `core/include/midibn/engine/playback_engine.h`, `core/src/engine/playback_engine.cpp` | `PlaybackEngine` (the owner's object, v1's public members) over `EngineCore` (state and logic, shared with the workers): settings, transport (Start, Seek, Pause, Resume, Stop, Unload, Dispose), channel controls, snapshots, publication, output sends, waits, transport time. The header carries the v1 → v2 public member map. |
| `core/src/engine/engine_worker.cpp` | `PlaybackWorker` and the classic modes (Queue, Drop, forward-only Drop, Per-note, Delayed NoteOff, Note start rate, Output rate) with their dispatch helpers. |
| `core/src/engine/engine_ordered.cpp` | The preserve-on-Pause/Seek modes (`PlaybackEngine.Ordered.cs`), including the Output-paced timeline paths. |
| `core/include/midibn/engine/ordered_adapters.h`, `core/src/engine/ordered_adapters.cpp` | `OrderedUnlimitedQueueAdapter`, `OrderedVirtualForwardAdapter`, `OrderedMusicalGateAdapter`, `OrderedNoteAttackAdapter`, `OrderedOutputRateAdapter`. |
| `core/include/midibn/engine/channel_state.h`, `core/src/engine/channel_state.cpp` | `ChannelStateTracker` (+ snapshots), the live `ChannelOverrideState`, `ChannelRoutingState`. |
| `core/include/midibn/engine/engine_types.h`, `core/src/engine/engine_types.cpp` | `ProcessingMode`, `PlaybackState`, `PlaybackSnapshot`, `ServiceDurationSettings`, `EngineError`, `PlaybackSong` (v1 `MidiSong` as the engine sees it), `TimeConversion` (v1 `MicrosecondsToTicks`/`TicksToMicroseconds`), `dispatch_due_clock_ticks` (v1 `*.DispatchDueClockTicks`). |
| `core/src/engine/engine_internal.h` | `FilteredSourceEvents`, the exception-capture helper. |
| `core/include/midibn/platform.h` | New `platform::EngineServices` factory; `Waiter::wait` returns `WaitResult`; `Thread` gained `join(timeout)`, `is_alive`, `is_current`; `WakeEventRole`, `PlatformError`. |
| `platform/win32/win32_timing.*` | `Win32EngineServices` (QPC clock, kernel events, waitable-timer waiter, `_beginthreadex` threads); `Win32Waiter` no longer throws; `Win32Thread` implements the new members. |
| `platform/virtual/lockstep_services.*` | `LockStepEngineServices`: deterministic lock-step implementation for tests (library `midibn_platform_virtual`). |
| `tests/engine_tests.cpp` | `midibn-engine-tests`. |

## Class map (v1 → v2)

| v1 | v2 | Status |
|---|---|---|
| `PlaybackEngine` (both partial files) | `engine::PlaybackEngine` | ported |
| `PlaybackSnapshot`, `ProcessingMode`, `PlaybackState`, `OverflowPolicy` | `engine::PlaybackSnapshot`, `ProcessingMode`, `PlaybackState`; `models::OverflowPolicy` (reused) | ported / reused |
| `ServiceDurationSettings` | `engine::ServiceDurationSettings` (published as `shared_ptr<const>`; identity compared like v1's `ReferenceEquals`) | ported |
| `ServiceDurationClock`, `ServiceDurationCalculator` | `models::ServiceDurationClock` | reused (Milestone 3) |
| `PendingMidiQueue`, `ForwardDropQueue`, `CompleteNoteTracker` | `models::*` | reused |
| `PerNoteIntervalGate`, `DelayedNoteOffGate`, `AdaptiveNoteIntervalClock`, `MusicalAttackRateLimiter`, `OutputRateGate`, `MusicalNotePriority` | `models::*` | reused |
| `OrderedPlaybackGenerations` (work items, generation, queue), `OrderedFiniteAdmissionCore` | `models::*` | reused; `DispatchDueClockTicks` ported here as `engine::dispatch_due_clock_ticks` |
| `Ordered{UnlimitedQueue,VirtualForward,MusicalGate,NoteAttack,OutputRate}Adapter` | `engine::Ordered*Adapter` | ported |
| `ChannelStateTracker`, `MidiChannelSnapshot`, `ChannelPlaybackSnapshot` | `engine::*` | ported |
| `ChannelOverrideState` (live) | `engine::ChannelOverrideState` | ported (the Analysis snapshot stays in `overrides/override_snapshot.h`) |
| `ChannelRoutingState` | `engine::ChannelRoutingState` | ported |
| `MidiStateChaseIndex`, `ChannelSourceValueIndex` | `ChaseIndex`, `SourceValueIndex` (`channel_index.h`) | reused; built over a derived view by `make_playback_song(song, view, …)` as v1 rebuilds them lazily for a routed `MidiSong` |
| `MidiSong`, `MidiEventReader` | `engine::PlaybackSong` (`models::EventSource` + payload store + indexes) | adapted |
| `MidiEventView`, `MidiEvent` (generated messages) | `models::EventView` (`EventView::create`) | reused |
| `IMidiOutput`, `IMidiOutputContext`, `MidiOutputSafety.ResetAndSilence` | `platform::MidiOutput`, `set_source_file`, `platform::reset_and_silence` | reused; `send_long` takes the SysEx event as designed |
| `HighResolutionWaiter`, `SpinWait`, `Stopwatch`, `EventWaitHandle`, `Thread` | `platform::Waiter`, `Waiter::spin_once`, `platform::Clock`, `platform::WakeEvent`, `platform::Thread`, all from `platform::EngineServices` | interfaces |
| `ChannelControlRequest`, `ChannelControlKind`, `PreservedSeekRequest`, `FilteredSourceEvents` | private members of `PlaybackEngine` | ported |
| `PlaybackErrorEventArgs`, `ChannelControlErrorEventArgs`, `PlaybackWorkerTimeoutException` | `EngineError` passed to the callbacks; type name `PlaybackWorkerTimeoutException` | ported |
| `DropTraceRecorder`, `SourceDispatchTrace`, gate test hooks | — | not ported (see below) |
| `EffectivePlaybackSpeed`, `RollingOutputRate` | — | not engine code (MainForm statistics); belongs to `core/stats` with the Win32 frontend (Milestone 6) |

## Platform factory and how the comparison harness drives the engine

`platform::EngineServices` (platform.h) is the only route to time, blocking and threads:

- `clock()` — every v1 `Stopwatch.GetTimestamp()` is `clock().now_ticks()`, at the same points and the same number of times; `Stopwatch.Frequency` is read once at construction (v1's is a static readonly).
- `create_wake_event(role, initiallySet, autoReset)` — called twice by the engine constructor in v1's field order: `WakeEventRole::Wake` (v1 `_wake`) then `WakeEventRole::PauseAcknowledged` (v1 `_pauseAcknowledged`), both manual-reset and clear. v1 calls map one to one: `Set` → `set`, `Reset` → `reset`, `WaitOne(0)` → `poll`, the paused worker's `_wake.WaitOne()` → `wait(-1)` **on the worker thread**, the control thread's `_pauseAcknowledged.WaitOne(timeout)` → `wait(timeout)` **on the control thread**.
- `create_waiter(wake, error)` — called first thing on every worker thread (v1 constructs `HighResolutionWaiter` there), so it identifies the worker; destroyed on that thread right after the mode loop returns (v1 `using`), before the worker's exit bookkeeping. `Waiter::wait(us, error)` is v1 `WaitMicroseconds` (`Elapsed` = true, `Woken` = false, `Failed` = v1 threw, with the exception type and text); `Waiter::spin_once` is v1 `SpinWait.SpinOnce` in the final sub-timer loop.
- `start_thread(body, AboveNormal, "MIDI bottleneck scheduler", error)` — v1 `new Thread(PlaybackWorker)` + `Start`, called with the engine mutex held (as v1). The returned `Thread` must report `is_alive()` from this call until the body returns; `is_current()` is v1's `thread == Thread.CurrentThread`; `join(timeout)` is v1 `Thread.Join(timeout)` on the control thread (Stop/Seek/Pause/Unload boundaries); `detach()` abandons a still-running worker without waiting. The body holds a `shared_ptr` to the engine core and must be destroyed on the thread that ran it (not kept in the `Thread` object). A failed start is reported as v1's `OutOfMemoryException`.
- Ownership: `PlaybackEngine` takes `shared_ptr<EngineServices>` and `start` takes `shared_ptr<MidiOutput>`; the core keeps both, and each worker keeps the core, so an abandoned worker never outlives anything it uses.

Nothing else blocks except the engine mutex (v1 `_sync`, recursive) and two leaf mutexes held for a few instructions (v1 `ChannelOverrideState._changeSync`, and a private lock in `ChannelStateTracker` — see design choices).

For the comparison harness, Group E implements `EngineServices` with the spike's rendezvous (`ENGINE_HARNESS_FEASIBILITY.md`):

- park the worker in `Waiter::wait` (deadline = now + us; released by the harness at the deadline or by `Wake.set()`), keep v1's early `Woken` when the wake event is already set, and the `checked(coarse * 10)` overflow;
- park the paused worker in `Wake.wait(-1)` with no deadline (only `set` releases it);
- model `PauseAcknowledged.wait(2000)` on the control thread as the spike's control wait;
- a start gate in `start_thread` (a worker started during a command waits until the command returns);
- `Thread::join(timeout)` waits for the worker to finish;
- the recording output advances virtual time per send and yields after a completed send once a command is due, released by a change of `PlaybackEngine::wake_generation()` (v1 `_wakeGeneration`) and only while `state()` is Playing — `state()` and `wake_generation()` are safe to call from inside a send;
- `PlaybackEngine::set_send_observer` reports, on the sending thread and immediately before each output call, the event exactly as v1 passes it to `IMidiOutput.Send`, including v1's `EventIndex` (source or routed logical index; 0 for messages v1 converts implicitly from a `MidiEvent`: forced overrides, manual chase, channel-disable silence; −1 for `FromEvent(…, −1)`: Play/Seek chase, monitor chase, protected seek markers). The v2 `send_short` carries only the packed bytes, so the observer is how a harness logs v1's `src=` column.

`platform/virtual/lockstep_services.h` is a simplified version of that protocol used by `midibn-engine-tests` (commands delivered only at timed waits; no send-boundary yield); Group E can start from it.

## Design choices (no user-visible effect)

1. **Errors.** Inside the engine module, v1's exceptions are thrown as `EngineError` values (type name, Message, first inner exception) and models keep throwing `ModelException`; every public member catches at the boundary and returns `EngineError` (`ok()` when v1 does not throw). .NET's "\r\nParameter name: x" suffix is appended for the `Argument*Exception` family. Output failures (`OutputStatus`) become the .NET type they stand for (`InvalidOperationException`, `Win32Exception`, …). An allocation failure maps to v1's `OutOfMemoryException` text.
2. **One recursive mutex** for v1 `lock (_sync)`: v1's Monitor is re-entered on the same thread (e.g. `SetChannelOverride` sends under the lock and `SendProtectedEvent` locks again). The lock is held across an output call exactly where v1 does (direct sends with no worker).
3. **Atomics** for every `Volatile`/`Interlocked` field; generation counters use unsigned atomics so wrap-around matches C# unchecked `int`.
4. **`ServiceDurationSettings` identity**: `std::shared_ptr<const …>` with `std::atomic_load/store`; the worker compares pointers, so `SetServiceConfiguration` with equal values still restarts the service phase, as in v1.
5. **Thread ownership**: v1 drops `_thread` while the worker still runs on it. v2 keeps every started thread in a list of `shared_ptr` owners and reaps finished ones from control operations; `thread_` is only v1's "current worker" marker. A worker never joins or destroys its own thread object (it detaches it if it releases the last reference to the core).
6. **Destructor = v1 Dispose, then abandonment** (user decision, answer to the former open question 1): `~PlaybackEngine` runs v1's `Dispose` (Unload → Stop with v1's bounded 2000 ms Join and texts), then detaches any worker still blocked in an output call — on Win32 the thread handle is closed without joining — and drops the event handlers (nobody can observe them any more). The state that worker uses lives in `EngineCore`, which the worker body owns through a `shared_ptr` together with the services and the output; when the call finally returns, the worker finishes v1's exit path (final reset and panic, as v1's timeout text promises) and frees the core on its own thread. Closing therefore never blocks longer than v1. `test_hung_output_abandoned` proves Stop and Dispose return after ~2 s with v1's text and that the released worker finishes cleanly.
7. **ChannelStateTracker lock**: v1 has none (it relies on the engine's ordering); v2 adds a private leaf mutex so concurrent monitor reads are defined behaviour in C++. It never blocks on anything else.
8. **Ordered adapters' per-generation state** lives in a map keyed by generation id inside each adapter (v1 stores it in `PlaybackGeneration.ModeState`; `OrderedVirtualForwardAdapter` already uses a dictionary in v1). States of retired generations are dropped after a successful Seek append. The Milestone 3 generation classes are unchanged.
9. **Drop trace and source-dispatch trace** (`DropTraceRecorder`, `SourceDispatchTrace`, `SetGateBoundaryDispatchHookForTesting`, `SetGateLogicalOutputTraceForTesting`) are test/diagnostic hooks MainForm never sets. With them unset v1's `traceEnabled` is false, so v2 ports exactly the branches v1 takes then (the trace-only bookkeeping — cluster size, consecutive drops, busy-until estimates, maximum buffer occupancy — is omitted; none of it feeds a decision when tracing is off).
10. **SysEx payloads** are read from the song's compact store at the event's source index (a routed view keeps source indices; generated routing events are never SysEx) and passed to `send_long` as a `SystemExclusiveEvent`.
11. **Zero-length non-SysEx events** are skipped by the engine, as the v2 output contract specifies (v1's backends return at `DataLength == 0`). Accepted as internal (user decision): the loader never produces such events (every SMF channel/system event the loader stores has at least its status byte), so v1's WinMM "No MIDI output device is open." ordering for a closed device cannot be reached through them.
12. **`PlaybackSong` identity** replaces `MidiSong` reference identity (v1 compares songs by reference in Pause and in the monitor chase).
13. **Chase messages** are created from the chase index with snapshots of the live routing/override state taken at the call (v1 reads the live objects while iterating; equal unless the UI changes them during that loop).
14. **Spin state**: v1 creates a fresh `SpinWait` per wait; the Win32 waiter keeps one counter, which only changes when it yields the time slice, not how many spins happen.

## v1 behaviours found while porting, and what became of them

Everything below was ported unchanged first (the Milestone 5 port was an exact copy) and then judged under the current contract ("all v1 features, improved").

| v1 behaviour | Outcome |
|---|---|
| Channel pressure generated by an override or chase is labelled `SystemMessage` (v1 `ChannelOverrideState.CreateMessage`) | Kept: a label that nothing reads (the message sent and every counter are right), but the trace digests and engine recordings print it, so changing it would add differences to explain for no benefit. |
| Drop mode reads `OverflowPolicy` twice when creating its queue | Fixed: read once (DD-040; no output change). |
| The overflow-time "Clear buffer" (classic Drop path) counts every skipped due source event as dropped, including events of muted channels, while the waiting-time "Clear buffer" classifies them as filtered | Fixed: both paths treat muted or override-replaced events as filtered, as the user guide says (DD-041). |
| Pause, or Stop and Start, exactly on an event time sends that event again | Fixed (DD-007). |
| Ordered Per-note / Delayed NoteOff snapshots report wrong queue figures | Fixed (DD-009). |
| Ordered path: clearing the last override before the worker applied the pending one froze playback | Fixed (DD-006). |
| The ordered Drop path did not count a suppressed NoteOff as dropped | Fixed (DD-008). |
| A playback thread that cannot be started: raw .NET text, and the next Stop threw `ThreadStateException` | Plain sentence and clean Stop (DD-003); the engine is Stopped afterwards, not Playing (DD-045). |
| Generated messages carry v1's event index (0 or -1, see above) | Kept: bookkeeping that only the harness's `src=` column and the traces show; it has no effect on playback. |
| `++_pauseRequestGeneration` / `++_preservedSeekGeneration` wrap; a wrapped pause generation of 0 takes the non-preserving path | Kept: reachable only after 2^32 Pause requests; a counter has to wrap somewhere. |
| `OrderedVirtualForwardAdapter` registers a generation's state before constructing the generation | Kept: internal order of two steps with no visible effect. |
## Decisions on the former open questions (user, via the coordinator)

1. **Hung worker at destruction**: must match v1 — implemented as design choice 6.
2. **Thread start failure text**: was to be v1's text (the contract then was exact copying; DD-003 has since replaced it with a plain sentence and a clean Stop, DD-045 returns the engine to Stopped). Verified with a csc-compiled check (`new Thread(fn, int.MaxValue).Start()` on x86, .NET Framework 4.8): `Thread.Start` throws `System.OutOfMemoryException` with Message "Exception of type 'System.OutOfMemoryException' was thrown."; `IsAlive` is then false and a later `Join` throws `System.Threading.ThreadStateException` "Thread has not been started.". v1 assigns `_thread` before `Start`, so after the failure the engine is Playing with an unstarted `_thread`, and the next Stop (and therefore Start, Seek, Pause, Unload) throws that ThreadStateException. The marker `UnstartedThread` lets Stop recognise that case; `test_thread_start_failure` checks the current behaviour. MainForm shows the Start exception's Message under "Unable to start playback".
3. **Zero-length non-SysEx events skipped**: accepted as internal (design choice 11).
4. **Decimal branch of `TicksToMicroseconds`** (clock frequency above Int64.MaxValue/1,000,000 ≈ 9.2×10¹²): v2 computes the exact truncated quotient; .NET's decimal division keeps 28–29 significant digits and could round a quotient within ~10⁻¹⁵ of the next integer up before truncation, which only matters above ~10¹⁵ Hz. Accepted as internal: no supported hardware has such a clock. `MicrosecondsToTicks` is exact in both.
5. **Suspected v1 defects**: ported unchanged by this milestone; after the contract changed they were fixed as register entries (table above).
## Not ported / out of scope

- `DropTraceRecorder`, `SourceDispatchTrace` and the gate test hooks (see design choice 9).
- `EffectivePlaybackSpeed`/`RollingOutputRate` and everything MainForm decides around the engine (play preconditions, rate-model state machine, restart on output change): app/frontend layer, Milestone 6.

## Results at the time of writing

- Builds: x64 (v143), x86 (v143), XP (v141_xp): 0 compiler/linker warnings (MSB8051 on XP is Microsoft's deprecation notice).
- Tests on all three builds: `midibn-tests` 24, `midibn-platform-tests` 13, `midibn-midi-output-tests` 102, `midibn-engine-tests` 276 checks, all passing; engine tests repeat identically and include a run-twice determinism check.
- Parity: `midibn-parity.exe --fixtures v2/harness/fixtures --expected v2/harness/expected` RESULT: PASS on x64, x86 and XP builds. (`tools/parity.ps1`'s v1 drift step could not run here: `v1-reference/build.ps1` refuses to build because this worktree checks `src/` out with CRLF line endings, so blob ids differ from the tag.)

## Group E: comparison with v1's recordings (Milestone 5)

The engine was run against v1's real `PlaybackEngine` through the lock-step harness. Method, files, commands and the recordings: `v2/harness/ENGINE_SCENARIOS.md` section 12; digest format: `v2/harness/TRACE_FORMAT.md` "Engine digests". In short: `v2/harness/engine/` is a C++ twin of the v1 spike's rendezvous (real worker thread, start gate, parks with virtual deadlines, untimed park of the paused worker, control wait that lends the turn and times out in virtual time, yield at the end of a send once a command is due released by `wake_generation()`, recording output at 320 us per byte, SpinOnce = 1 us, the 5,000,000-idle-read livelock guard with the spike's text); `set_send_observer` supplies the `src=` column. `midibn-engine-harness` writes `.out.txt` and `.sched.txt`; `midibn-parity` (= `v2-xp-selftest[-ia32]`) runs the same comparison against `v2/harness/expected/engine/` (SHA-256 of v1's 7,820 recordings; x86 = x64 and every run identical: five runs each for the first 7,406, three runs each for the 414 of the nine scenarios added later).

### Results

- **170 scenarios x 23 fixtures = 3,910 runs; all 7,820 recordings (`.out.txt` and `.sched.txt`) equal v1's on all four builds (at the time of the Milestone 5 comparison; since then the register's engine rules explain the recordings that DD-001, DD-006 to DD-009 changed)** (x64, x86, XP, XP-IA32), by scenario family (identical / total per build; the four builds give the same numbers):

  | family | scenarios | runs identical / total |
  |---|---|---|
  | Milestone 3 and Pause/Seek (`queue-*`, `ordered-pause-*`, ...) | 21 | 483 / 483 |
  | A. rate models | 8 | 184 / 184 |
  | B. Drop mode | 15 | 345 / 345 |
  | C. gates | 18 | 414 / 414 |
  | D. ordered path | 26 | 598 / 598 |
  | E. output-paced | 14 | 322 / 322 |
  | F. chase | 22 | 506 / 506 |
  | G. controls | 22 | 506 / 506 |
  | H. routing | 15 | 345 / 345 |
  | I. mutation-check coverage (added later) | 9 | 207 / 207 |
  | total | 170 | 3,910 / 3,910 |

  The comparison covers every `SEND`/`RESET`/`PANIC` with its virtual time, source index and bytes, every engine event, every `SNAP`/`SNAPX` field (processed, dropped, queue, positions, gate and rate-gate counters, lag), `FINAL`, and the scheduler trace (park and resume points, yields, control-wait releases and timeouts, worker starts), so the engine also blocks, wakes and starts threads exactly where v1 does. The harness's own counters (sends, parks, go/wake resumes, spins, yields, event waits, control waits) equal the spike's console line for every run.
- **Porting errors found: none.** The Group D port was identical to v1 on the first complete run of the harness; no engine source changed in Group E. (A mutation check shows the comparison is not vacuous: adding 1 us to the classic Pause's rounded position fails `queue-pause-resume` and the other Pause scenarios at their first `SNAP` line.)
- **Mutation check: 50 of 50 engine mutations detected** (`v2\tools\mutation-check.ps1 -Family engine`, x64; each mutation rebuilds `midibn-engine-harness` and the comparison with v1's digests must fail). Over the first 161 scenarios 44 mutations were detected and five changed decisions were not (the events/sec remainder rounding up and not carried, the ordered Output-paced service ticks, the ordered Clear-buffer filtered-arrival test, the Output rate decision time + 1 us). Nine scenarios (section I of the scenario file) now show them; the five moved from the former `engine-gap` family into `engine`, and one more mutation (the finite ordered worker's own service-tick line, next to the unlimited one) was added: 44 + 5 + 1 = 50, none undetected. How each is shown:
  - events/sec remainder: `rate-eps13-queue` and `rate-eps13-drop` (13 per second; 1,000,000 mod 13 = 1, so the carried remainder walks 1 to 12). Both mutations fail 11 of the 23 fixtures in each.
  - ordered Output-paced service ticks: `ordered-output-paced-rate-1us-queue` and `-drop`. v1's ordered worker busy-polls the clock while an Output-paced service hold is pending and a source event falls inside the hold, which the lock-step clock cannot advance (`ENGINE_SCENARIOS.md` section 11). With a 1 us service time no source event falls inside the hold on any fixture (3 us already livelocks on `decimal-halfway`), the hold is real and the pacing shift (service time plus send time) shows in every send after the first service. This is therefore a covered case in the harness; it did not have to be left to the real-time property comparison.
  - ordered Clear buffer filtered arrivals: `ordered-drop-policy-clear-buffer-channels-off` and `-slow` (channels disabled in setup, so no channel control is pending at capacity and the ordered worker does not busy-poll).
  - Output rate decision time + 1 us: `gate-output-rate-limit-chase-163`, `-239`, `-299`. The decision time is the dispatch time in microseconds, and the pacing decision depends on it only at a slot edge to the microsecond. A systematic search was needed (a probe copy of the pacer, compared at `t` and `t + 1`, over rates 1 to 400 and Start positions 0 and mid-song): the edge occurs when the Output rate gate starts mid-song on `index-chase-rich`, where the state-chase messages consume pacing slots and the first source ticks are decided on the 320 us send lattice. Of 31 rates where the probe saw a difference in the available slots, 5 changed the recording (163, 239, 277, 299, 353); three are committed. A +37 us error on the same decision changed 1 of 828 runs in a general sweep, so this decision is nearly insensitive to the time, and the three scenarios are what detects a +1 us error; they detect it on `index-chase-rich` only.
- **DD-001 appears in two fixtures.** `sysex-short` and `sysex-variants` contain SysEx events of 1 to 3 stored bytes, which v1 reads back as zeros and v2 reads correctly (`DIFFERENCES_FROM_V1.md`, decided before this milestone). Their `SEND SystemExclusive` lines differ in the hex only (1,957 and 671 lines over all 170 scenarios; 327 of the 340 `.out.txt` recordings of those two fixtures). The comparison applies DD-001 explicitly: v2's `.out.txt` is mapped to v1's view (those bytes become zeros) before hashing, and the number of mapped lines is pinned in the engine rule (originally in the digest manifest). Nothing new was added to the register. With the mapping, all 3,910 runs are identical.
- **Determinism.** Ten simultaneous instances of the full comparison (20 busy threads), and a run restricted to one processor, give the same 3,703 identical results as a quiet run. The schedule is a function of the script, not of timing; the real-time limits (20 s per wait, as the spike) were never reached.
- **Suspected v1 defects at the time of the comparison** were ported unchanged and matched v1 (the ordered-path stale-override livelock ended in the clock-read livelock guard, `PlaybackFailed TimeoutException ... busy-poll livelock`, as v1's evidence in `v2/out/spike/evidence-ordered-override-livelock/`; the pause-on-event-time resend occurred in the comparison constantly). They are fixed now (DD-006, DD-007); `test_ordered_override_set_clear_while_paused` and `test_fixed_v1_defects` in `midibn-engine-tests` state the corrected behaviour.
- **Timings** (reference PC, harness alone, full set): x64 about 45 s, x86 and XP about 60 s, XP-IA32 about 60 s; one processor only (affinity mask 1, XP build) 137 s. The XP self-test package runs the engine part in both variants: it adds 126 s here (63 s per variant; the two variants' self-tests took 92 s and 89 s in all, against about 27 s each before), and more on slower hardware, so `package-xp-selftest.ps1 -EngineSubset quick` builds a package that runs the 161 scenarios on 6 fixtures only. The Windows parity run (`tools\parity.ps1`) always runs the full set. The previous package took about a minute on the XP test machine. The nine scenarios added later for the mutation check (207 runs) take 1.4 s (XP) and 1.5 s (XP-IA32) here, about 3 s more for the two variants in the package (the full engine part measured 54 s and 53 s with 170 scenarios, whole self-tests 76.5 s and 76.7 s on a quiet PC); the harness alone on x64 runs them in 1.1 s.
- **Not covered here (by design):** real-time behaviour (cannot be compared exactly), a genuinely hung output call (the 2 s Join/acknowledgement timeouts are real time; `test_hung_output_abandoned` covers Stop and Dispose), channel monitoring and the other undriven members listed in `ENGINE_SCENARIOS.md` section 11.
