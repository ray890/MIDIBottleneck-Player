# Differences from v1.0.44: the improvement register

## The contract

v2 has all the features of v1.0.44, made to work as they were meant to, plus bug fixes, better performance, more precise timing, clearer messages and cleaner code. It does not have to behave exactly like v1.0.44 any more.

v1.0.44 is therefore the **functional reference and a difference detector**, no longer a copy to be matched bit for bit. The test programs still compare v2 with v1 everywhere (file reading, load steps, the timing and queue rules, the Analysis and track-routing results, and the 3,910 scripted playbacks). Every difference they find must be **explained by an entry in this register**. A difference that no entry explains is a failure, and so is an entry whose difference has stopped showing up (a stale entry).

Two independent checks guard this:

1. **Against v1.** Each entry names the rules that say exactly which differences it explains (the rule file `v2/harness/expected/differences-from-v1.txt`; how the rules work is described in `v2/harness/TRACE_FORMAT.md`, section "Difference rules"). Anything outside the rules fails.
2. **Against v2's own earlier answers.** v2's output is also compared, exactly, with what v2 itself recorded last time (`v2/harness/expected-v2/`; `TRACE_FORMAT.md`, section "v2 baseline"). So a new improvement cannot quietly change anything else, and an improvement does not weaken the checks for the parts it does not touch. Re-recording that baseline is a deliberate act (`v2/harness/make-v2-baseline.ps1`), done together with the register entry.

An entry is never written to make a failing check pass. The order is: decide the improvement (the user decides each judgment call), write the entry, write the rules, change v2, re-record the baseline.

## How to add an entry (recipe)

1. Add a section `## DD-NNN — <title>` below, with the next free number, using the template. The heading must start with `## DD-` and the number: `parity.ps1` checks that every id used in the rule file has such a heading.
2. Add the rules for the differences it causes to `v2/harness/expected/differences-from-v1.txt` (one or a few lines; the grammar and examples are at the top of that file).
3. Make the change in v2.
4. Run `v2/tools/parity.ps1` (or one built variant of `midibn-parity`): the differences must now show up as "explained by DD-NNN" with the numbers you expect, and nothing else may differ. The per-entry counts are printed at the end (`register DD-NNN: ...`).
5. Run `v2/harness/make-v2-baseline.ps1`; it prints which baseline entries changed. They must be exactly the ones your change should affect. Commit the entry, the rules, the code and the new baseline together.
6. Evidence: for a playback (engine) difference, `midibn-parity --engine-v1 <v1 recording folder>` or `midibn-engine-harness --v1 <folder>` prints the first differing line (the v1 text next to the v2 text) for each explained difference. Put a short excerpt in the entry's Evidence part.

Entry template:

```
## DD-NNN — Title

- **Category:** bug fix | performance | timing precision | clearer message | internal quality | judgment call to review
- **What changed:** v1: ...  v2: ...
- **Why:** ...
- **User-visible effect:** what a person using the player notices (or "none")
- **Evidence:** fixtures, scenarios, trace lines, measurements; the decision (who, when)
- **Risk:** what could go wrong, what else it touches
- **Harness rules:** the rule ids/lines that classify the resulting differences
```

"Judgment call to review" marks a change where reasonable people could disagree (a behaviour that v1 arguably did on purpose). It stays open for the user's decision; the harness treats it like any other entry.

## Entries

## DD-001 — Very short SysEx messages keep their bytes

- **Category:** bug fix
- **What changed:**
  - v1.0.44: a SysEx event whose stored length is 1 to 3 bytes keeps its length but its bytes read back as zeros. This affects `F0` events with an SMF length of 0 to 2 (stored with the `F0` prefix, so 1 to 3 bytes, for example `F0`, `F0 F7`, `F0 43 F7`) and `F7` escape or continuation packets with an SMF length of 1 to 3. The intended message is therefore not reproduced on output.
  - v2: every SysEx message is stored and read back exactly as it appears in the file; the merge keeps those 1 to 3 bytes inline in the record's packed field (`core/src/midi_loader.cpp`, merge loop). The final long-payload store is unchanged (still only lengths of 4 or more), so `payload-bytes`, preflight counts and memory projections stay identical to v1.
  - Cause in v1: the direct parser stores the SysEx bytes in the provisional payload store and leaves the packed short-message field zero (`DirectCompactMidiParser.cs:443-454`). The merge then copies the event through `CompactMidiEventStore.Builder.Add(MidiEventView, long)` (`MidiEventStore.cs:407-420`), which treats every event with a length of 3 or less as a packed short message and copies only the zero packed field.
- **Why:** the v1 result is a data-loss defect, not a design choice; no v1 feature depends on the zeros. Decided 2026-10-12 by the user (prompt 003): record the defect publicly as a v1 known limitation (`docs/KNOWN_LIMITATIONS.md` and `docs/TECHNICAL_REFERENCE.md` on `main`, commit `514c003`), do not fix v1, fix it in v2.
- **User-visible effect:** a very short SysEx message in a file (for example a one-byte `F0`/`F7` or a short manufacturer message) is sent to the synthesizer as written instead of as zero bytes. Files without such messages behave the same.
- **Evidence:**
  - Fixtures `sysex-short.mid` (every form: `F0` length 0-2, terminated and unterminated; `F7` length 1-3; boundaries `F0` length 3, `F7` length 0 and 4; same-tick ties across two tracks) and `sysex-variants.mid`.
  - Load traces: v1 shows the zeroed data fields, v2 the bytes from the file. The 19 differing lines (14 in `sysex-short-mid.trace`, 5 in `sysex-variants-mid.trace`) are exactly the SysEx events of 1 to 3 stored bytes, and in each only the data field differs. Every other line of both traces, all index sections, all flow traces and all other fixtures are identical.
  - Boundary events (`F0` length 3 = 4 stored bytes, `F7` length 0 and 4) are identical in v1 and v2.
  - Engine recordings: v2's `SEND SystemExclusive` lines of 1 to 3 bytes differ from v1's in `sysex-short` (1,957 recording lines over the 170 scenarios) and `sysex-variants` (671); nothing else about those runs differs. These counts are pinned in the rule and re-counted in v1's recordings by `make-engine-expected.ps1`.
- **Risk:** low. The change is confined to events of 1 to 3 stored bytes. Memory and the long-payload store are unchanged. A synthesizer now receives short SysEx messages that v1 turned into zeros (which it ignored or misread).
- **Harness rules:** in `v2/harness/expected/differences-from-v1.txt`:
  - `trace DD-001 sysex-short-mid.trace,sysex-variants-mid.trace ...`: the load/index trace line is the same except that the data field, all zeros in v1, holds the file's bytes in v2 (one rule, written with the placeholders described in `TRACE_FORMAT.md`).
  - `engine DD-001 *@sysex-short,sysex-variants normalize=short-sysex-zeroed pin=sysex-short:1957,sysex-variants:671`: the named normalizer `short-sysex-zeroed` maps v2's recording to what v1 would have recorded (the bytes of a 1-3 byte SysEx `SEND` line become zeros) before hashing; the pinned counts are an internal check.

## DD-002 — Notes are sent on time: the final approach to each event no longer sleeps

- **Category:** timing precision
- **What changed:**
  - v1.0.44: after a waitable timer has covered all but the last 200 microseconds before an event, v1 waits out the rest with .NET's `SpinWait`. That helper busy-waits only about 40 microseconds, then yields the processor and, on every 20th yield, calls `Thread.Sleep(1)`, which on Windows lasts up to one timer period (15.6 ms, or 1-2 ms with the multimedia timer raised). Events are therefore often sent several milliseconds late, and sometimes 15 ms late.
  - v2 (before this entry): the engine port had reproduced that `SpinWait` and its `Sleep(1)` (commits `b85a37d`, `0a4c5ae`), on the earlier reading of the contract ("no deliberate difference").
  - v2 now: the final approach keeps the processor and reads the clock after every spin (one `YieldProcessor` pause per check), so the event goes out within a few microseconds of its time. The CPU cost stays bounded: the timer still covers all but the last 200 microseconds; if the approach lasts longer than 1 ms (a late timer wake-up, an unusual target) or the machine has a single processor, the thread gives its time slice away on every spin (`SwitchToThread`) instead of burning it. The virtual and lock-step waiters are unchanged.
- **Why:** the contract (prompt 007) is that v2 is better than v1, never worse to match it. Late sends are audible as note-timing jitter on dense passages and were the one place where v2 had been made deliberately worse.
- **User-visible effect:** notes and controller changes are sent on time (typically within 10-20 microseconds on a dense song) instead of up to 15 ms late; the playback position and statistics are unchanged. The playback thread uses a little more CPU for at most the last 200 microseconds before each event, as v1's busy phase did.
- **Evidence:** `v2\tools\engine-properties.ps1 -Repeats 3` (real clock, output that sends nowhere, four lanes in parallel on a busy PC, so only the relative numbers matter; `rt-queue-dense`, 27,498 sends, lateness behind the due time in microseconds, median / 90th / 99th percentile):

  | | x64 | x86 |
  |---|---|---|
  | v1.0.44 | 4,509-6,294 / 11,943-12,732 / 15,781-24,918 | 5,178-5,343 / 11,781-11,992 / 15,024-16,251 |
  | v2 with the ported `SpinWait` | 206 / 7,760 / 12,082 | 609 / 9,685 / 14,553 |
  | v2 now | 12 / 20 / 491 | 12 / 17 / 323 |

  (The two v1 values per cell are the runs before and after the change; the runs are independent, other programs were compiling on the same PC.) Before the port, on a quiet PC, v2 measured 8 / - / 271 microseconds on this test, against v1's 6,500 / - / 14,900. All properties, including the quality properties L01 (lateness), J01 (jitter) and K01 (command latency), pass; before this change the same run failed L01/J01/K01 on x86 (`ENGINE-PROPERTIES: FAIL`: v2 p99 15-30 ms against v1 10-17 ms), because v2 carried v1's `Sleep(1)`.
  Final verification run (after DD-011 as well, `-Repeats 3`, `ENGINE-PROPERTIES: PASS`): `rt-queue-dense` v2 p50 / p99 = 8 / 916 us (x64) and 10 / 358 us (x86) against v1 7,122 / 15,687 and 6,248 / 13,999; `rt-queue-ordered-dense` v2 118 / 490 and 196 / 485 against v1 6,748 / 16,847 and 7,129 / 28,171; `rt-chase-mid` v2 271 / 988 and 296 / 1,006 against v1 563 / 14,652 and 475 / 13,129; command latency (median) v2 18-30 us against v1 1.8-2.9 ms.
- **Risk:** low. No decision depends on the spin (it only waits for a target time that the loop re-checks); the lock-step harness never runs it, so the bit-exact engine comparison is unaffected. A little more CPU in the last 200 microseconds before each event; bounded to 1 ms per event by the yield fallback.
- **Harness rules:** none (the real-time properties check it: `v2\docs\ENGINE_PROPERTY_CHECKS.md`, "Timing observations").

## DD-003 — Failure messages say what failed and what to do

- **Category:** clearer message
- **What changed:** v1 showed the raw text of a .NET exception in several places; v2 replaces each with a sentence that says what failed and what the user can do, keeping the file path where it helps. The playback engine's failure texts are in `core/include/midibn/user_messages.h`, the file and memory texts of the MIDI outputs in `platform/win32/win32_error_text.h` (renamed from `dotnet_io_message.h`).
  - KDMAPI provider file cannot be opened for the architecture check (all cases): was `Could not find file 'P'.`, `Could not find a part of the path 'P'.`, `Access to the path 'P' is denied.`, `The process cannot access the file 'P' because it is being used by another process.`, the path-too-long text, `Could not find the drive 'P'. ...`, `The operation was canceled.`, and for every other Windows error the untrimmed system message with its trailing line break or `Unknown error "<n>".` Now, for example: `The KDMAPI provider file was not found: P. Check that OmniMIDI is installed and that the file has not been moved or deleted.` or `The KDMAPI provider file is being used by another program and cannot be read right now: P. Close the other program and try again.`; other errors: `The KDMAPI provider file could not be opened: P. Windows reports: <description without the line break> (error <n>).`
  - The provider file ends before its header is complete: was `Unable to read beyond the end of the stream.`; now `The KDMAPI provider file ended before its header could be read: P. The file may be damaged or incomplete; reinstall OmniMIDI or choose a different provider file.`
  - A SysEx buffer cannot be allocated: was `Insufficient memory to continue the execution of the program.`; now `There is not enough memory to send a System Exclusive (SysEx) message to the MIDI device. Close other programs and try again.`
  - Windows cannot start the playback thread: was `Exception of type 'System.OutOfMemoryException' was thrown.`; now `Windows could not start the playback thread (not enough memory or system resources). Close other programs and try again.` The follow-on failure of v1 (after that, every Stop, and with it Start, Seek, Pause and Unload, failed with `Thread has not been started.`, so the player could not recover) is fixed too: Stop after a failed start stops cleanly and resets the output.
  - A playback allocation fails: was the same OutOfMemoryException text; now `There is not enough memory to complete this operation. Close other programs and try again.`
  - The wait for the next event is too long to arm the timer: was `Arithmetic operation resulted in an overflow.`; now `The time until the next MIDI event is too long to schedule, so playback stopped. Check the song's tempo and the playback timing settings.`
  - The engine reporting a model's .NET-style exception: `ArgumentOutOfRangeException` ("Specified argument was out of the range of valid values." plus a `Parameter name:` line) becomes `The <setting name> is outside the range the player accepts. Choose a value inside the allowed range.`; `OverflowException` becomes `A playback calculation became too large for the player to handle. Use a smaller processing time, MIDI bitrate or events-per-second setting.`; `IndexOutOfRangeException` and `ArgumentNullException` become short plain sentences. A null song or output given to Start: `No song was supplied to start playback.` / `No MIDI output was supplied to start playback.`; the internal `NullReferenceException` text in an ordered Seek: `Playback lost track of its internal queue while seeking. Stop and start playback again.`
  - The internal exception type names stay as categories (tests and diagnostics read them); only the message changes.
- **Why:** raw .NET texts mean nothing to the person using the player and give no way forward; the contract (prompt 007) asks for clearer messages. None of the texts was a designed feature of v1.
- **User-visible effect:** only in the rare failure cases above; nothing changes while everything works.
- **Evidence:** `midibn-midi-output-tests` (`test_error_texts` and the architecture-check cases, 140 checks), `midibn-engine-tests` (`test_thread_start_failure`: the new text, Stop recovers, a second Stop is harmless; `test_start_errors`). No recorded engine run or trace contains these texts (parity unchanged by this entry).
- **User decision 2026-10-18 (prompt 011):** accepted as an objective improvement (plain sentences in place of raw .NET error texts).
- **Risk:** low. Windows' own description in the fallback text is in the user's language. The remaining loader texts are replaced in DD-012.
- **Harness rules:** none.

## DD-004 — Misuse of the rate and queue models is handled cleanly

- **Category:** bug fix
- **What changed:** three cases that the models of v1 mishandled when they were used in ways no v1 caller does today (found by the model traces; each makes a future caller safe):
  - A pending-queue attack whose index is already in the queue (Note-index mode): v1 threw `An item with the same key has already been added.` only after it had linked the new node, leaving a half-added entry (count, tail and free-list changed). v2 throws the same exception before changing anything.
  - Progressive rate pacer: a negative timestamp was clamped to 0 when slots expired but recorded unclamped (for example -230), so the history held a time that nothing else used and a later timestamp between -230 and 0 failed the monotonic check. v2 records the clamped time.
  - Service time by MIDI bitrate: with a bitrate near Int64.Max, `bytes x 10^7 + bitrate - 1` overflowed and threw `OverflowException` although the answer (1 microsecond, rounded up) is representable. v2 rounds the division up without the intermediate sum; every other bitrate gives the same answer as before.
- **Examined, not changed:** `OutputRateGate.RecordDispatch` overwrites its `requiredRelease` argument with the queued flag while reservations are pending. The overwritten value is never used afterwards (a pending reservation means the slot was already taken, so the answer is always "no excess"); it is dead code, not a defect, so there is nothing to make robust. v2 does not carry the dead assignment.
- **Why:** robustness; the exceptions and states of the three cases were accidents of evaluation order.
- **User-visible effect:** none today (no caller of the player reaches them).
- **Evidence:** model traces: `queue` 4 changed state lines (the lines after the refused duplicate, until the queue is cleared), `rate` 142 results (`OverflowException` in v1, `1` in v2: every byte count with bitrate Int64.Max) and 18 state lines (pacer tail time -NN in v1, 0 in v2). Unit tests in `midibn-tests` (bitrate Int64.Max, ordinary bitrates unchanged, clamped pacer timestamps, refused duplicate leaves the queue unchanged and usable). The Analysis traces and all 3,910 engine runs are unchanged.
- **Risk:** low; the changed code paths are reached only by those inputs.
- **Harness rules:** `trace DD-004 models/queue.trace ...` (four exact lines), `trace DD-004 models/rate.trace` (two patterns).

## DD-005 — A manual routing destination outside channels 1-16 no longer breaks the dialog

- **Category:** bug fix
- **What changed:** v1's track-routing dialog threw `ArgumentOutOfRangeException` (`SelectedIndex`) while opening if the saved routing settings gave the first lane a manual destination of 16 (a 17th channel), because it set the selector to index 17 of 17 entries. The planner already reported such a lane as `has an unavailable manual destination` (a hard error that blocks Apply). v2's selector index function returns -1 (nothing selected) for a destination outside 0-15 instead of an index past the end; the lane still shows the planner's error.
- **Why:** a hand-edited or damaged settings file must not make a dialog unopenable; the selector cannot produce such a value itself.
- **User-visible effect:** the dialog opens, the lane shows its "unavailable manual destination" error and an empty correction selector; choosing a valid destination repairs it.
- **Evidence:** `midibn-tests` (indices for destinations 0, 15, 16, -3). The planner's handling of the value is covered by 62 `unavailable manual destination` lines in the routing model trace, unchanged.
- **Risk:** none for valid settings (indices 0-16 as before).
- **Harness rules:** none (no trace reaches the selector).

## DD-006 — An override cleared while playback is paused no longer freezes the ordered path

- **Category:** bug fix
- **What changed:** v1: with "preserve the queue on Pause/Seek" on (the ordered path), setting a channel override and clearing it again before the worker had sent the forced message (for example both while paused) left the channel marked as having a forced message waiting. The four ordered workers then stopped taking in song events and looped reading the clock: after Resume the playback froze with one processor core fully busy, and a later Start did not clear it. v2: clearing an override cancels the message that was waiting for it, so nothing is left marked. The classic path was never affected.
- **Why:** a plain defect: a person pausing, nudging a channel setting and putting it back, then resuming, must not lose playback.
- **User-visible effect:** playback resumes normally after set + clear while paused; no frozen playback and no busy processor core.
- **Evidence:** new scenarios `ordered-override-pause-set-clear-resume` (ordered path) and `override-pause-set-clear-resume` (classic control), 23 fixtures each. v1 (recorded by the v1 spike): the ordered run ends in the harness's livelock guard (`PlaybackFailed TimeoutException ... busy-poll livelock`), nothing is sent after Resume; v2: plays to the end (46 runs, 23 recordings of `.out` and `.sched` each, explained). `midibn-engine-tests` `test_v1_ordered_override_livelock` now checks that the ordered path plays to the end after Resume.
- **Risk:** low; an override that is set and still in force keeps its pending message exactly as before.
- **Harness rules:** `engine DD-006 ordered-override-pause-set-clear-resume@* explained files=both`. The two DD-001 pins (`sysex-short`, `sysex-variants`) were raised by 9 and 2 lines because v2 now plays on where v1 stopped; `# v1-pin:` in the rule file keeps the v1 counts (1976, 679) for the check against v1's recordings.

## DD-007 — A Pause or restart that lands exactly on an event's time does not send that event again

- **Category:** bug fix
- **What changed:** v1: Pause (classic path) restarted playback at the first event at or after the paused time. An event whose time equals the paused time has already been sent, so it was sent a second time after Resume (a repeated NoteOn plays the note twice). The same happened when the player stops and starts again at the position it just reached (changing a gate setting or the output while playing restarts like that). v2: Pause restarts at the first event strictly after the paused time. `start` has a new optional argument, `continuation`: a restart that continues where playback just was passes true and skips the events at exactly the start time; a start the person chose (from the beginning, or a position they picked) still plays the events at the start position (including an event at time 0).
- **Why:** a duplicate event is a defect (the intended behaviour of Pause is to continue after the paused time). The player controller passes `continuation=true` in its two restart paths (processing-mode/gate setting change while playing or paused, output change); a Play pressed by the person starts normally.
- **User-visible effect:** none audible in normal use (a Pause lands on an event's exact microsecond very rarely); a repeated note at the pause point cannot occur.
- **Evidence:** the lock-step runs make the coincidence frequent on songs with several events at one microsecond: 48 runs of 13 scenarios (Pause/Resume twice, override while paused, restart for a gate edit, restart while paused) on `long-sysex-segment-crossing`, `many-events-segments`, `metadata-edge`, `metadata-names` differ from v1; unit test: Pause/Resume sends the 500 ms note once (v1 twice), `continuation` start does not resend, a chosen start at 500 ms plays it.
- **User decision 2026-10-18 (prompt 011):** accepted as an objective improvement (no repeated note after Pause). Confirmed by the user 2026-10-19 (recorded in prompt 012, Step 1.1).
- **Risk:** low. An event due exactly at the pause that the worker had not yet sent is not sent after Resume (v1 sent it only when it was also the repeat); the classic Pause already discards due events that were not yet sent.
- **Harness rules:** four `engine DD-007 ... explained files=both` rules (grouped by fixture).

## DD-008 — The ordered Drop path counts suppressed NoteOffs as dropped

- **Category:** bug fix
- **What changed:** v1's classic Drop path counts a NoteOff that is suppressed because its NoteOn was dropped in the Dropped events figure; the ordered (queue-preserving) Drop path did not (`PlaybackEngine.Ordered.cs:898-903`), so the same song gave a smaller Dropped figure on the ordered path (for example 2,100-2,300 fewer in the real-time protected-traffic test). v2 counts it on both.
- **Why:** the user guide says that each discarded attack and each discarded or later suppressed release counts separately.
- **User-visible effect:** the Dropped events statistic is complete on the ordered path; playback is unchanged.
- **Evidence:** 8 runs (`ordered-drop-policy-drop-oldest-complete-note`, `-drop-background-notes`, `-drop-incoming-complete-notes` on `many-events-segments`, `models-dense`, `models-notes`): only the dropped counters of the `SNAP`/`FINAL` lines differ (`.out`; the schedule is identical).
- **Risk:** none (a statistic).
- **Harness rules:** two `engine DD-008 ... explained files=out` rules.

## DD-009 — Statistics of the ordered Per-note and Delayed NoteOff modes are correct

- **Category:** bug fix
- **What changed:** v1's ordered Per-note interval and Delayed NoteOff modes published the work that was waiting but never published the index of the next song event, so each snapshot counted "newly due song events" from event 0 (or from an earlier run's value) and reported a wrong queue length, outstanding count and queue age. v2 publishes the next song event and the due time of the oldest waiting item.
- **Why:** the statistics (queue length, outstanding events, age, and the pressure meter that uses them) are shown to the person.
- **User-visible effect:** the queue figures of those two modes are meaningful; playback unchanged.
- **Evidence:** 163 runs: seven scenarios on all 23 fixtures (`ordered-gate-per-note-interval[-scale-polyphony-4|-live]`, `ordered-gate-delayed-note-off[-live]`, `ordered-output-paced-gate-per-note-interval`, `-delayed-note-off`): the queue, outstanding and age fields of the `SNAP` lines differ (`.out`); the two `-live` scenarios also differ in the schedule file because their restart position comes from the corrected figures. Unit test `test_v1_quirks`.
- **Risk:** low (statistics and the restart position derived from them).
- **Harness rules:** two `engine DD-009 ... explained` rules.

## DD-010 — A provider that never answers the readiness probes cannot open

- **Category:** bug fix
- **What changed:** when a MIDI output is an application-local provider (a copy of winmm-compatible DLL beside the player), the player sends harmless sustain-off probes every 10 ms for up to 5 seconds and requires two accepted answers in a row before it uses the device. v1 built its error from the code of the last probe; if that last probe happened to be accepted (code 0) while the earlier ones were not, the error code was 0 and 0 means success, so the open succeeded although the provider had never been ready. v2 reports the most recent error code, so the open fails with the intended message (`MIDI error while waiting for two accepted initialization probes from ...; first returns ...`).
- **Why:** the code and its message document the requirement of two consecutive accepted probes; accepting a provider that never met it contradicts that and can lose the first notes of a song.
- **User-visible effect:** only with such a provider (rare): a clear failure message instead of a possibly unready device.
- **Evidence:** reasoning from the code (the case needs probe results alternating until the 5-second limit and a 0 as the final one); no device was used (not testable without one). Unit tests of the error-code choice (`probe_failure_code`).
- **User decision 2026-10-18 (prompt 011):** accepted as an objective improvement (no false success from an unready provider). Confirmed by the user 2026-10-19 (recorded in prompt 012, Step 1.1).
- **Risk:** a provider that needed longer than 5 seconds or flickered at the end now fails to open instead of risking use; nothing else changes (two accepted probes in a row, or the cumulative-byte-count contract, still open it at once).
- **Harness rules:** none.

## DD-011 — Less work per event in the playback loop

- **Category:** performance
- **What changed:** the loop that sends events that are already due (Queue mode, no service time, and the same helper in Drop mode) did, for every event, three engine-lock acquisitions and four clock reads: it read the transport time before and after the send although the callers never used them, read the clock again for the Output rate gate even when no such gate is on, and loaded the settings pointer atomically to see whether a setting changed. v2 now skips the unused transport reads, reads the clock for the Output rate gate only when that gate is active, and notices a settings change with one atomic counter (`settings_generation`) checked per event, confirming with the pointer comparison only when the counter moved. What the engine publishes, sends and records is unchanged.
- **Why:** real-device measurements showed the engine itself, about 215-250 ns per event, was the throughput limit with a very fast output, long before the raw send path (20 million messages per second).
- **User-visible effect:** dense songs and bursts clear their backlog about 2.3 times faster; nothing else.
- **Evidence:** `midibn-perf --engine-real` (new: real Win32 services and worker thread, clock running 1000 times faster so the 8,000,160-event `large-8m` song is always due; output hash identical to the lock-step run, `e253baeecf2f25fe`), median-ish of 4 runs on this PC with other programs running: x64 245-276 ns per event (3.6-4.1 million events/s) before, 103-114 ns (8.8-9.7 million/s) after; x86 465-517 ns (1.9-2.1 million/s) before, 205-222 ns (4.5-4.9 million/s) after. The 3,956 lock-step engine runs are unchanged against the v2 baseline (no baseline entry changed) and `midibn-engine-tests` passes (287 checks).
- **Risk:** low: the removed reads and locks had no effect on state; the generation counter changes with every publication of settings.
- **Harness rules:** none.

## DD-012 — The loader's remaining .NET texts are replaced by plain sentences

- **Category:** clearer message
- **What changed:** the loader and the Analysis window still carried four raw .NET texts. `Unable to read beyond the end of the stream.` (a file that ends inside the MIDI header) is now `The file ended before its MIDI header was complete. It is damaged or not a complete MIDI file.` `Arithmetic operation resulted in an overflow.` and `Value was either too large or too small for an Int64.` (an event time beyond the representable range; neither is reachable with a real file) are now `An event time in this file is too large for the player to represent, so the file cannot be played. The file may be damaged.` Running out of memory while loading (`Exception of type 'System.OutOfMemoryException' was thrown.`) and while analysing (`Insufficient memory to continue the execution of the program.`) is now `There is not enough memory to complete this operation. Close other programs and try again.` The error type names are unchanged. Completes DD-003 (which covers the playback and output texts).
- **Why:** the same reason as DD-003: a raw .NET text says nothing to the person using the player.
- **User-visible effect:** only in these failure cases; opening a truncated file shows the new sentence.
- **Evidence:** 24 changed trace lines, all in the truncated-header fixture `err-header-truncated`: 2 in its load trace and 11 in each of the two load-flow traces (x64/x86 pointer sizes); nothing else changes. The other three texts have no fixture (they need a 2^63 tick sum or an exhausted address space). The text in the Analysis UI expectation `analysis-ui/failure.txt` is a model exception shown by a test and is left alone.
- **User decision 2026-10-18 (prompt 011):** accepted as an objective improvement (same decision as DD-003).
- **Risk:** none.
- **Harness rules:** `trace DD-012 err-header-truncated-mid.trace,flow/err-header-truncated-mid.ptr4.trace,flow/err-header-truncated-mid.ptr8.trace ...` (one rule).

## DD-020 — Event times are computed exactly

- **Category:** timing precision
- **What changed:** v1: the time of an event is summed in .NET `decimal` (28 digits), which rounds every non-terminating piece (a third of a microsecond, for example) and so, where the true time lies exactly halfway between two whole microseconds, lets that rounding noise decide the direction; v1 then comes out one microsecond low. v2: the time is `round_half_up(sum of ticks x tempo / ticks-per-quarter)` in exact integer arithmetic (whole microseconds plus a remainder, `core/src/tempo_clock.cpp`), so it is always the correctly rounded value of the true time.
- **Why:** it is the exact answer, needs no 128-bit decimal emulation, and is faster (see below).
- **User-visible effect:** an event whose true time is exactly halfway between two microseconds is one microsecond later than in v1 (1 microsecond; only in files with tempo changes whose pieces do not divide evenly). Everything else is identical.
- **Evidence:** the .NET decimal vectors (6,000 chains computed by real .NET `decimal`) are compared with the new clock by `midibn-parity` ("exact integer clock vs .NET decimal"): every chain is identical except those where the .NET value is just below the halfway point and the exact answer is one more. Fixtures: `decimal-halfway.mid` (2 trace lines: tempo-map entry and the first event) and `tempo-changes.mid` (2 lines); the large fixture's event digest changes (Analysis digests of those fixtures and the engine recordings of the two fixtures follow from the shifted time). The `.NET decimal` emulation (`core/src/net_decimal.cpp`) is no longer in the product; it stays in the test-oracle library `midibn_decimal_oracle`.
- **Risk:** low; one microsecond on rare events.
- **Harness rules:** `trace DD-020` lines for the two load traces (`${N+1}` states "v1 time plus one"), the large summary digest and the Analysis digests; `engine DD-020 *@decimal-halfway,tempo-changes explained`.

## DD-021 — Events take 24 bytes instead of 40

- **Category:** performance
- **What changed:** v1 (and v2 until now): 40 bytes per stored event. v2: 24 bytes (tick 8, time 8, message bytes 4, track 2, kind/channel/flags 2). The length, status and payload position that v1 kept in separate fields are derived; a SysEx of 4 bytes or more keeps its payload in the payload store and one 8-byte end offset in a table; shorter SysEx lives inside the record. Callers still read the familiar wide record (`EventStore::records[i]` returns it by value), so nothing outside the loader changed.
- **Why:** less memory per event and less data to scan (Analysis, playback, routing).
- **User-visible effect:** less memory (see DD-022 for the numbers); the maximum loadable file size on 32-bit systems grows.
- **Evidence:** every trace, index, flow, model and engine comparison is unchanged by this entry alone (proved: the new code with the old decimal clock reproduces the old Analysis result hashes of the large fixture bit for bit); events 40 -> 24 bytes (the large fixture stores 8,000,160 events in 183 MiB instead of 305 MiB); measurements in DD-022 and `XP_COST_LOG.md`.
- **Risk:** low; the record shape seen by callers is unchanged.
- **Harness rules:** none (no output difference).

## DD-022 — Loading uses about half the memory and about half the time

- **Category:** performance
- **What changed:** (a) index series start with a small array that doubles, instead of reserving a 32 KB segment for each of thousands of sparse series (v1's policy, kept by v2 until now); (b) the merge replaces the top of its heap in one step instead of a pop and a push; (c) a SysEx of up to three bytes is read straight into its record instead of passing through the per-track payload store; (d) long payloads move to the final store in 64 KB pieces, not through a buffer as large as the payload.
- **Why:** the loading peak was well above the final footprint (large fixture: 539 MiB peak private, 376 MiB afterwards).
- **User-visible effect:** a large file loads with a lower memory peak, the peak equals the final size, and loads sooner.
- **Evidence:** `midibn-perf` on the 8,000,160-event fixture, median of 5 interleaved runs: peak private memory 539 -> 219 MiB (equal to the size after loading; before it was 377 MiB after loading), peak working set 464 -> 224 MiB, load time x64 1,361 -> 771 ms, x86 1,980 -> 976 ms, XP 2,005 -> 1,078 ms, XP-IA32 3,853 -> 1,167 ms. On a 32-bit system the largest loadable file grows from about 44 to about 76 million events. Details in `XP_COST_LOG.md`.
- **Risk:** low; all load, index, flow and engine traces are unchanged by this entry alone.
- **Harness rules:** none (no output difference).

## DD-023 — Track routing is planned faster

- **Category:** performance
- **What changed:** the check "may these two lanes share a destination without a patch change while one is sounding" built a sorted list and two ordered maps of every sounding interval of both lanes for each pair of lanes; it now walks the lanes' already sorted event positions in one pass (the sorted end positions are kept with the lane profile).
- **Why:** on the 8,000,160-event fixture the route compile took about 35 s in v1 and in v2 until now.
- **User-visible effect:** the Track routing dialog opens/updates much sooner on very large files; same plan.
- **Evidence:** `midibn-perf --routing` on the 8,000,160-event fixture, 3 runs: x64 34.5 s (30.5-45.9) -> 8.2 s (7.6-9.2), x86 7.8 s, XP 8.5 s; the routing model trace (51,829 lines) and the 7,225 checks of `midibn-routing-app-tests` are unchanged. Details in `XP_COST_LOG.md`.
- **Risk:** low; same answers for every pair (proved by the routing traces).
- **Harness rules:** none (no output difference).

## DD-024 — The large-file warning uses v2's real memory use

- **Category:** performance / clearer message
- **What changed:** v1 estimated memory with .NET object-header sizes and the 40-byte record; v2 estimates what it actually stores (24-byte events, an 8-byte table entry per long SysEx, index entries and series, per-track lists, the merge buffer, and the track-routing structures with their real sizes). The warning thresholds (1 GiB for 32-bit, 4 GiB for 64-bit) and all wording are unchanged; the `estimate` numbers in the preflight trace are v2's.
- **Why:** the warning should be about real memory (ARCHITECTURE.md section 10, item 7).
- **User-visible effect:** a file warns later (at about twice the size) than in v1; the numbers shown in the warning are smaller and true to v2.
- **Evidence:** the `estimate` line of every load trace changes; `PreflightCounts` is unchanged except for one added count (long SysEx events). Estimate versus measured peak private memory on the large fixture in `XP_COST_LOG.md`.
- **Risk:** medium-low; an estimate that is too low would let a file through that fails to load on a 32-bit system (the load still reports a normal out-of-memory message). Estimates are deliberately upper bounds.
- **Harness rules:** `trace DD-024 ... /estimate ptr(\d) retained .*/` for the load traces and the large summary.


## DD-025 — Overflow choices: clearer names, complete-note choices first

- **Category:** judgment call (decided by the user); clearer message
- **What changed:** v1 names the single-event overflow choices "Drop newest" and "Drop oldest" (the Analysis report says "Drop oldest pending event"). v2 names them **"Drop newest events"** and **"Drop oldest events"** everywhere (the Overflow list, the Analysis report, messages, Help and the tooltips), and the Overflow list starts with the two complete-note choices: Drop incoming complete notes, Drop oldest complete note, Drop newest events, Drop oldest events, Clear buffer and jump to realtime, Drop background notes. The default is still the same policy (Drop newest events), which is now the third entry. With a queue waiting-time limit the list is Clear buffer and jump to realtime, Drop oldest complete note (soft limit), Drop oldest events (DD-086: the default of that list first, then the order of the preview). The choices are the same policies as before: the numbers in traces, saved analysis settings and scenarios are unchanged, only the list order and the names differ.
- **Decision recorded:** these two choices are meant to drop single events. The earlier proposal to protect the Note Offs of notes that were already sent (in the forward-only "Apply queue limit without slowdown" mode with Drop newest events) is **declined**: a dropped Note Off can leave a note sounding, and that is accepted. Behaviour is unchanged (v1's); only the Help now says so ("With Apply queue limit without slowdown, Drop newest events can discard a Note Off and leave a note sounding; the complete-note choices avoid this.").
- **Why:** "Drop newest" and "Drop oldest" read like the complete-note choices below them although they remove single events; the new names say what they drop, and the safer note-preserving choices are the first ones in the list.
- **User-visible effect:** the Overflow list has a new order and two new names; the same words appear in the Analysis report, in the messages that name a choice (starting playback with a policy that needs simulated slowdown, a waiting-time limit with an unsuitable choice, the Analysis error for a forward-only model) and in the Help.
- **Evidence:** the app tests check the list, its order, the default and the index-to-policy mapping in both modes (event count and waiting time), and that index 3 / 0 / 2 reach the engine as Drop oldest events / Drop incoming complete notes / Drop newest events; the Analysis format tests compare v1's report text with the renamed one (the summary and format tables of v1); the Help test applies the one documented substitution to v1's text. 16 trace lines of the Analysis model trace (the error text of a forward-only Drop oldest) differ from v1.
- **Risk:** none for behaviour (no policy logic changed). Harness and scenario ids (`DropNewest`, `drop-oldest-complete-note`, ...) stay as they are: they are internal names, not user-visible text.
- **Harness rules:** `trace DD-025 models/analysis.trace ...` (the one model error text). The analysis, UI and Help tables of v1 are not rewritten; their tests apply the substitutions named here (`fix_expected_policy`, `test_formats`, `test_help`).

## DD-026 — Settings are remembered between sessions

- **Category:** judgment call (decided by the user; replaced 2026-10-25, the earlier decision was "nothing is remembered").
- **What changed:** the Windows player keeps the output choice, the KDMAPI and Direct WinMM ticks, every option of the window menu (including Application scale, the borderless choices, Always on top, Show Processing model / Statistics, the playback and queue options), the Effective-speed window and the few-processor option in a small text file (`%APPDATA%\MIDIBottleneck Player\settings.ini`, or `MIDIBottleneck Player.ini` next to the program for portable use). Not remembered: the window position and size, the loaded file, and the values on the main window and in Analysis, Channels and Track routing. A missing or damaged file never stops the start; the standard settings are used and the window says so once. The Linux player does the same (DD-153). Nothing is written to the registry.
- **Why:** the user first wanted nothing remembered (2026-10-18); on 2026-10-25 the user decided that the choices should be remembered, except the window position and the loaded file.
- **User-visible effect:** the program starts with the choices of the last run. A remembered output that is no longer connected is replaced by the usual one. Delete the file to return to the standard settings. Improvement over v1, which remembered one number (the Effective-speed window) in `%LOCALAPPDATA%\MidiBottleneck\settings.txt`; v2 does not read v1's file. The "parse" rows of v1's `speed-window` table, which tested v1's settings-file reader, are not used (v2 has its own format).
- **Evidence:** DD-153 (file format, location, tests).
- **Risk:** low (a bad file falls back to the defaults per option).
- **Harness rules:** none (no output difference).

## DD-027 — The routing memory estimate is tight; routing generates its patch events compactly

- **Category:** performance / clearer message (decided by the user: "tighten the routing worst-case memory bound")
- **What changed:** after DD-024 the 32-bit large-file warning still appeared on the 8-million-event test file, although applying Track routing there needs about 0.5 GB. The estimate assumed, as a worst case, a generated Bank/Program event (112-120 bytes) for every note start. v2 now stores the Bank/Program events a route generates as 12-byte groups (source event number, destination channel, which of Bank MSB / Bank LSB / Program Change, their values; the events themselves are built from the source event when read) instead of a map of vectors of 64-byte event views, and the routed event view takes one 4-byte number per event (8 bytes on 32-bit and 16 on 64-bit before). The estimate counts exactly these structures: at most one group per note start or patch-state event, three events at most per group, and the growth of the group list. It is still an upper bound for every route setting (also the one that merges all lanes into one destination).
- **Why:** the warning should be about what the file really needs; a false warning on a file that fits teaches people to ignore it.
- **User-visible effect:** the 8,000,160-event test file no longer shows the large-file warning on 32-bit (estimate 814 MB against the 1 GiB limit; it needed 2,244 MB before). The numbers in the warning for routing are smaller. Applying a route needs less memory, and a route that generates many patch events needs far less (about 12 bytes per group, not about 270). Track routing results are unchanged.
- **Evidence:** measured peak private memory with `midibn-perf --routing` (x86, default settings): `large-8m.mid` 462 MB after planning and 494 MB with the routed view built, estimate 814 MB (before: 2,244 MB); `routing-held-1000000.mid` 167 MB measured, estimate 216 MB; x64 465 / 497 MB against 837 MB, and 182 MB against 236 MB. Conditions and the full table are in `XP_COST_LOG.md`. The routing model traces (generated-event digests included), the 7,225 routing dialog checks and 148 new checks that the generated groups stay inside the counts the estimate uses (9 fixtures, four settings including capacity 1 with the aggressive tier) pass unchanged. The `estimate` line of every load trace changes (the DD-024 rule covers it).
- **Risk:** low-medium. An estimate that is too low would let a file through that fails to load on 32-bit; the bound argument (one group per note start or patch-state event, the preflight counts) is checked by the new tests on the routing fixtures and the measured values stay below the estimate. The data structure change is covered by the routing traces.
- **Harness rules:** the existing `trace DD-024 ... /estimate ptr(\d) retained .*/` rule covers the changed `estimate` lines; no other output changes.

## DD-035 — Live displays refresh in step with the monitor

- **Category:** timing precision / performance (user decision; `ARCHITECTURE.md` section 10, question 1)
- **What changed:** v1 refreshed its live displays from 16 ms WinForms timers, which in practice tick in uneven steps of about 15.6 ms and are not tied to the monitor. v2 posts one refresh per desktop-compositor pass from a small pacing thread (`frontends/win32/display_pacer.cpp`) that waits on `DwmFlush()`: a 60 Hz monitor gets 60 even updates per second, a 144 Hz monitor 144. The thread runs only while playing or loading, does no drawing and reads no player state; it posts at most one pending message per window, so a busy UI thread skips refreshes instead of building a backlog. The refresh covers the main window readouts, timeline and statistics and, through the same update, the Channels and Analysis windows. The statistics, the engine and playback timing are not touched (the thread only wakes the UI thread). There is no cap by default; `--pace-max-hz N` (and the one `configure` call in `MainWindow::create`) set one if the measured cost at high refresh rates turns out not to be trivial.
- **Why:** smooth, even motion of the timeline and readouts, and no wasted updates between monitor refreshes.
- **User-visible effect:** smoother motion on any monitor; on a 60 Hz monitor 60 even updates instead of 64 uneven ones.
- **Evidence:** the player's `--report` lists, for the live updates, the count and the mean, standard deviation, median, 99th percentile and maximum interval, the UI time per update and the number of coalesced ticks (DD-039); `tools/refresh-pacing-measure.ps1` compares the old 16 ms timer, display pacing, display pacing capped at 60 and the 60 and 144 per second timers (UI-thread CPU, process CPU, intervals, events processed in a fixed time). The measurement is run by the main session in a quiet window and recorded in `XP_COST_LOG.md`; samples taken while the PC was busy (not results): display pacing on a 60 Hz monitor delivered 834 updates in 14 s with a median interval of 16.7 ms and 0.6 ms of UI time per update on the 8,000,160-event fixture.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** 60 Hz the test monitor, x64 player, 5 runs of 14.5 s with the 8-million-event fixture playing: display pacing gave 899-900 updates, mean interval 16.667 ms, 99th percentile 17.5 ms (maximum 17.9-23.8 ms), UI-thread CPU 688-859 ms (about 5-6 % of one logical processor, under 0.5 % of the PC); the old plain 16 ms timer gave 575-594 updates (mean 25.6 ms, 99th percentile 40.5 ms), about 40 a second rather than 64. Supersedes the busy-PC sample above. `DEVICE_MEASUREMENTS.md`, "The display-refresh cost".
- **Risk:** low. The pacing thread is separate from the engine and cannot change a statistic or a send time. If `DwmFlush` ever stopped returning (a sleeping display) updates would pause until it returns, which only matters for an invisible window; `DwmFlush` failing or returning implausibly fast switches to the timer (DD-036).
- **Harness rules:** none (no output difference).

## DD-036 — Where the monitor cannot pace the refresh, a precise 60-per-second timer does

- **Category:** timing precision
- **What changed:** on Windows XP (no `dwmapi.dll`; the XP build never imports it), with desktop composition off (Windows Vista/7 Basic theme), in a remote session, when `DwmFlush` fails or returns faster than 600 times a second, or when `--pace timer` is given, the same pacing thread beats 60 times a second on absolute `QueryPerformanceCounter` deadlines (a waitable timer, high-resolution where Windows has `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, otherwise with a 1 ms timer period held only while the thread runs). A stall (sleep, debugger) skips the missed beats instead of bursting. `dwmapi.dll` is loaded at run time from the system folder (`LOAD_LIBRARY_SEARCH_SYSTEM32` where Windows has it, otherwise the full system path).
- **Why:** an ordinary 16 ms Windows timer fires in 15.6 ms steps and drifts against 60 per second; the new beat does not drift and has no step.
- **User-visible effect:** the same smooth 60 updates per second on systems without display pacing.
- **Evidence:** the XP player imports no `dwmapi.dll` (dumpbin); `--pace timer` reports the timer source and its interval statistics in `--report`; XP numbers are taken by the main session.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** on the same 60 Hz screen the precise 60-per-second timer gave 899-900 updates, mean 16.666 ms, 99th percentile 17.6 ms (maximum 19.7-24.3 ms), UI-thread CPU 359-516 ms: as smooth as display pacing. The 144-per-second timer: mean 6.947 ms, 99th percentile 7.7 ms, UI-thread CPU 359-1,188 ms.
- **Risk:** low; on Windows XP without a high-resolution timer the beat has up to about 1 ms of jitter.
- **Harness rules:** none (no output difference).

## DD-037 — The workload graph's animation follows the same refresh

- **Category:** timing precision
- **What changed:** the workload graph in the Analysis window animated from its own 16 ms timer after zoom, scroll and similar input; it now subscribes to the same pacing (one step per refresh, only while it is animating).
- **Why:** the same evenness as the other live displays, and one pacing thread instead of one more uneven timer.
- **User-visible effect:** smoother graph animation.
- **Evidence:** the Analysis window logic (9,137 checks) and its smoke test are unchanged.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** not measured separately; it follows the pacing measured under DD-035.
- **Risk:** low.
- **Harness rules:** none (no output difference).

## DD-038 — Drawing stays on the UI thread

- **Category:** internal quality / judgment call to review
- **What changed:** nothing in how the displays are drawn. The pacing thread only wakes the UI thread; all drawing stays on it. A separate drawing thread was not introduced because nothing shows that the UI thread's drawing affects playback (the engine has its own thread) or input; the measurement script reports the UI time per update, and that is the number that would justify one.
- **Why:** the request allowed a drawing thread only if measurement showed a need.
- **User-visible effect:** none.
- **Evidence:** engine and UI run on separate threads already; sample under load (not a result): 0.15 ms of UI time per update on the dense fixture, 0.6 ms on the 8,000,160-event fixture.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** UI time per update 0.086 ms with display pacing (0.067-0.098 ms over all schemes) against 0.15 ms for the old timer; supersedes the 0.15 ms and 0.6 ms samples above, which were taken on a busy PC.
- **Risk:** low.
- **Harness rules:** none.

## DD-039 — The test report records the live-update timing

- **Category:** internal quality
- **What changed:** `midibn-win32.exe --report FILE` additionally writes the pacing source, the number of live updates, their interval statistics (mean, standard deviation, median, 99th percentile, maximum), the UI time per update and the pacer's posted and coalesced counts; new test switches `--pace auto|display|timer`, `--pace-hz N`, `--pace-max-hz N` select the source (`--refresh-ms N` still selects the old plain Windows timer, for comparison).
- **Why:** to measure evenness and cost, not just total CPU.
- **User-visible effect:** none (test switches).
- **Evidence:** `tools/refresh-pacing-measure.ps1`; `BUILDING.md`.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** the report fields produced all of the figures above (updates, interval statistics, UI time, coalesced counts: 1 coalesced tick in 3 of the 5 runs of the 144-per-second timer, none in any other run).
- **Risk:** none.
- **Harness rules:** none.

## DD-040 — Drop mode reads the overflow policy once

- **Category:** internal quality
- **What changed:** when Drop mode creates its pending queue it asked for the overflow policy twice (once per comparison), so a policy changed from the window between the two reads could build the queue for a mixed answer. v2 reads it once.
- **Why:** one decision from one value; found while rechecking the Milestone 5 port notes for behaviour kept only because v1 had it.
- **User-visible effect:** none (the two reads happen microseconds apart at the start of a run).
- **Evidence:** no recording, trace or test output changes (parity PASS, 3,956 engine runs unchanged).
- **Risk:** none.
- **Harness rules:** none (no output difference).

## DD-041 — Clear buffer at a full queue no longer counts muted channels' events as dropped

- **Category:** bug fix
- **What changed:** Drop mode with the policy "Clear buffer and catch up" has two places that clear the buffer: when the oldest waiting event is older than the waiting-time limit, and when the queue is full as an event arrives. Both skip ahead to the song position that is due now. The waiting-time path classified each skipped event: an event of a muted channel (or one that a forced override replaces) is *filtered*, so it is not counted as dropped and its note ownership is recorded. The full-queue path counted every skipped event as dropped, muted ones included, and did not record them. v2 gives both paths the same classification.
- **Why:** the user guide and the technical reference say muted-channel and override-filtered events are "reported separately from overflow drops" and consume no queue capacity; counting them as dropped inflated the Dropped events figure and the muted channel's Dropped column.
- **User-visible effect:** with a muted channel or a forced override and "Clear buffer and catch up" at a full queue, the Dropped events statistic is lower (it no longer includes events the person muted); playback is unchanged.
- **Evidence:** none of the 170 engine scenarios reaches the case (all 3,956 engine runs are unchanged), so the evidence is a new unit test, `test_clear_buffer_filters_muted_channel` in `midibn-engine-tests`: a dense song with notes on channels 1 and 2, channel 2 muted, a queue limit of 4: v2 counts processed plus dropped events equal to the channel 1 events only; the same test fails with v1's counting.
- **Risk:** low; the same code the waiting-time path has run since Milestone 5.
- **Harness rules:** none (no recording differs).

## DD-042 — A number box refuses text after the number that a person cannot type

- **Category:** internal quality
- **What changed:** the number boxes (queue limit, rates, channel values) read what is typed with the same rules as .NET's integer parser, which also accepts NUL characters after the digits (`15` followed by NUL reads as 15). v2 rejects them.
- **Why:** it is an accident of the parser; an edit control cannot hold a NUL, so the case only arises from pasted or scripted text.
- **User-visible effect:** none for typed text.
- **Evidence:** the 6 NUL rows in each of the two parse tables recorded from v1 (`scrub-parse-en-US.txt`, `scrub-parse-de-DE.txt`; numeric, channel and On/Off boxes) now read "not a number"; `midibn-ui-tests` expects exactly that for them (UI_DIFFERENCES.md); every other check of the 168,757 is unchanged.
- **Risk:** none.
- **Harness rules:** none (the UI tables have no rule file; the expectation is coded in `ui_tests.cpp`, marked DD-042).

## DD-043 — The application-local provider check is skipped when Windows cannot say which WinMM is loaded

- **Category:** bug fix
- **What changed:** after opening a Windows MIDI output the player checks whether the loaded `winmm.dll` is a copy placed next to the program (an application-local provider) and, only then, waits for its readiness probes. When Windows could not name the loaded module, v1 used the words "not loaded" or "unknown" as if they were a file name in the current folder, so the check compared the application folder with the current folder and could go on as if a local provider were present. v2 treats an unnamed module as "no application-local provider".
- **Why:** a placeholder text must not be resolved as a path.
- **User-visible effect:** none in practice: after a successful `midiOutOpen` the module is always loaded and named.
- **Evidence:** code review (the branch cannot be reached through the public API); `midibn-midi-output-tests` 143 checks unchanged.
- **Risk:** none.
- **Harness rules:** none.

## DD-044 — A track name is never cut in the middle of a character

- **Category:** bug fix
- **What changed:** track and instrument names are kept to 128 UTF-16 units. A character outside the Basic Multilingual Plane (an emoji, for example) takes two units; when the 128th unit was the first half of such a character, v1 kept that half and the name ended with a broken glyph (a lone surrogate). v2 drops the half character, so the name ends before it.
- **Why:** a name that ends in half a character is displayed as a replacement box (or an invalid string for text functions).
- **User-visible effect:** only for names longer than 128 units with a two-unit character exactly at the cut: the last box is gone. Track routing lane names are shorter by that unit in the same case.
- **Evidence:** fixture `metadata-names`: 1 line of its load trace and 108 lines of the routing model trace (lane, row and preview lines that show the name) lose the trailing `\uD83D`; 677 dialog states of the routing editor tables recorded from v1 (`metadata-names.txt`; the tables hold hashes of the dialog text) change with it. Nothing else changes.
- **Risk:** low; names shorter than 128 units, or whose cut falls between whole characters, are unchanged.
- **Harness rules:** four `trace DD-044` rules (load trace; routing model trace: lane, row, preview). The routing editor's v1 tables cannot express the change (they hold hashes of the dialog text), so `midibn-routing-app-tests` lists the 677 differing states with their v2 lines in `harness/expected-v2-ui/routing-ui/explained-states.txt`, requires each to still differ from v1 and to equal the listed line, and fails if an entry no longer occurs (regenerate with `--write-explained FILE` after a deliberate change).

## DD-045 — A playback thread that cannot be started leaves the player stopped

- **Category:** bug fix
- **What changed:** if Windows refused to create the playback thread, v1 reported the error but left its state at Playing with no thread behind it, so the window kept showing a playback that was not running and the next Stop had to cope with the missing thread. v2 returns the engine to Stopped when the start fails (the plain sentence of DD-003 is shown).
- **Why:** the state shown to the person must match what runs.
- **User-visible effect:** after the "Windows could not start the playback thread" message the Play button is ready again instead of showing a playback in progress.
- **Evidence:** `test_thread_start_failure` in `midibn-engine-tests` (the engine is Stopped after the failed start; Stop is still clean).
- **Risk:** none; the failure needs an exhausted system.
- **Harness rules:** none.

## DD-046 — The Position cell's tooltip no longer speaks of an attribute

- **Category:** clearer message
- **What changed:** the Channels window's Position cell, when it shows a historical value, had the tooltip of the attribute cells: "Right-click to restore the latest source-file value for this attribute", although Position is not an attribute and right-click does nothing there. v2 says "Historical last-sent position; it is not enforced and may no longer be current."
- **Why:** the old text promised an action that does not exist.
- **User-visible effect:** the tooltip of a historical Position cell.
- **Evidence:** `midibn-channels-app-tests`: 463 Position cells of the tables recorded from v1 carry the new text (the tests expect exactly that substitution); every other check is unchanged.
- **Risk:** none.
- **Harness rules:** none (the UI tables have no rule file; the expectation is coded in `channels_app_tests.cpp`, marked DD-046).
## DD-031 — The Channels grid can be used from the keyboard

- **Category:** usability
- **What changed:**
  - v1.0.44: the Channels window's cells could be changed only with the mouse (click to type or drag, right-click to release or restore, click on Ch to mute). (v1's grid is a standard Windows data grid, so its selection may have followed the arrow keys, but no cell could be edited or acted on from the keyboard.)
  - v2: the grid has a current cell, drawn with a dotted focus rectangle while the grid has the keyboard focus (a click also makes its cell current). Arrow keys move it; Home and End go to the first and last column of the row, Ctrl+Home and Ctrl+End to the first and last cell, Page Up and Page Down move a page of rows; the movement stops at the edges and the grid scrolls to keep the cell visible. Enter or F2 starts typing a value into an attribute cell (the same as clicking it without dragging); on the Ch cell they mute or unmute the channel. Space mutes or unmutes the channel of the current row from any cell. Delete does what right-click does (release a forced value to Auto, or restore the latest source value of a historical one). Escape cancels a typed edit and, when an editor is still open, closes it; with nothing to cancel it is left to the window. After Enter or Escape in the edit box the keyboard focus returns to the grid, so the arrow keys work again. The grid also has an accessible name ("MIDI channel grid"), and the window gives the keyboard focus to the grid.
  - Tab is not used by the grid (the window has no other control to move to), so nothing changes for it.
  - The Help text of the Channels topic and the keys are listed in `UI_DIFFERENCES.md`.
- **Why:** the player should be usable without a mouse (keyboard-only users, assistive technology); the contract allows new behaviour that keeps every v1 feature.
- **User-visible effect:** a person can reach and change every cell from the keyboard. Mouse use is unchanged apart from the dotted rectangle on the clicked cell while the grid has the focus.
- **Evidence:** `channels_app_tests.cpp` `test_navigation` (the cell each key reaches, edges, paging, every key from every cell stays inside the grid; portable function `channel_navigate` in `core/include/midibn/app/channel_navigation.h`); `channels_window_smoke.cpp` (real key messages: movement, Enter/F2 typing and commit, Escape, Delete, Space and Enter mute/unmute, scrolling into view, accessible name). v1's Channels tables (`harness/expected/channels-ui`) have nothing to compare because v1 had no such behaviour.
- **Risk:** low. The new keys do nothing the mouse could not already do. A key press while an editor is open first closes it (committing a typed value only through Enter, as before).
- **Harness rules:** none for the Channels tables (new behaviour, no v1 line changes). The Help text of the Channels topic gains a paragraph; `tests/ui_tests.cpp` `kHelpSubstitutions` maps v1's last Channels sentence to v2's text before comparing with `harness/expected/ui/help-Channels.txt` (v1's table is untouched).

## DD-033 — A failed mute is shown on the Ch cell

- **Category:** bug fix
- **What changed:** v1: when the engine could not apply a mute or unmute (for example the MIDI output refused the safety messages), the error text and tip appeared under the Bank MSB cell of that channel, because the engine reports a mute request with the placeholder attribute Bank MSB and the window used that attribute to find the cell. v2: the engine's detail handler (`set_channel_control_failure_detail_handler`, called together with the v1 handler) also says that the request was a mute or unmute, and the Channels window shows the tip, and keeps the text as the tooltip, on the Ch cell that was actually edited. A failed Bank MSB change still appears on the Bank MSB cell. The engine's own event (and so every engine recording) is unchanged.
- **Why:** the message belongs to the cell the person clicked, and the Bank MSB cell showed an error for something that was never changed there.
- **User-visible effect:** the error tip appears under the Ch cell of the channel that could not be muted.
- **Evidence:** `channels_app_tests.cpp` (`control_failed` with the mute flag: tip and tooltip on Ch, a Bank MSB failure unchanged), `engine_tests.cpp` (`test_channel_control_failure`: the detail handler reports an attribute request with `false` and its attribute). A mute request that fails inside the engine needs a failing output, which the tests do not simulate; the flag comes from the request's kind (`SetEnabled`).
- **Risk:** low. The only new engine surface is an additional optional callback.
- **Harness rules:** none needed: the engine recordings print the v1 handler's line (placeholder attribute included), which is unchanged, and the Channels flow tables drive `controlerror` with an explicit attribute, which behaves as before.

## DD-085 — Direct WinMM driver access (ModMessage): a new, optional output path

- **Category:** new feature (approved by the user; ARCHITECTURE.md section 1 and section 12)
- **What changed:**
  - v1.0.44: a WinMM device is always played through `midiOutOpen` / `midiOutShortMsg` / `midiOutLongMsg` of the system winmm.
  - v2: the main window's system menu has a new option, **Direct WinMM driver access (ModMessage)**, placed after "Output-paced timeline". It is off by default; its choice is remembered (DD-026, DD-153). With it on, a WinMM device is played by calling the installed user-mode MIDI driver's own `modMessage` entry point (found through the `Drivers32` registry list, loaded only by its full path inside the Windows system folder, matched to the selected device by name and identity). The option is also offered on Windows XP.
  - A device that cannot be driven this way (Windows' own driver: Microsoft GS Wavetable Synth, hardware and USB ports; the MIDI Mapper; a device no driver reports; a driver that cannot be loaded or refuses to open) plays through standard WinMM, and a status line (caption row of the "File and output" frame, right side, in a few words with the full sentence as its tooltip, DD-088; also the tooltip of the MIDI output box) says so and why: never a silent fallback and never a message box. While the direct path is in use the line names the driver and says "tested driver" (OmniMIDI and CoolSoft VirtualMIDISynth are on the tested list) or "unverified driver: not tested" (any other driver is allowed).
  - A failure while playing stops that playback with the normal output error message; the path is never switched in the middle of a song.
- **Why:** on Windows 8 and later the system WinMM path is far slower than the synthesizer (prompt 007: about 0.04 million events/s through system WinMM against 4.4-4.6 through winmmwrp and 3.8-4.5 through KDMAPI); the option is the built-in, driver-independent equivalent of the winmmwrp trick. No third-party code is shipped.
- **User-visible effect:** nothing changes unless the option is switched on (the default window is pixel-identical; the visual gate states are unchanged). With it on, WinMM playback through a supported synthesizer is much faster and the status line shows the path in use. The new menu item and the status line are listed in `UI_DIFFERENCES.md` item 11.
- **Evidence:** see `ARCHITECTURE.md` section 12 (design, sources, the double-initialisation finding) and the measurement report of the helper that built it; `tests/direct_output_tests.cpp`, `tests/app_tests.cpp` (`test_direct_winmm`), `tools/direct_check_main.cpp`, `tools/device/run-device-measurements.ps1` (path `winmm-direct`).
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** check program on the real drivers (OmniMIDI 14.8.5): standard WinMM 0.053 million messages per second (about 19 microseconds each), direct 41.6 million (24 ns each), 5 of 5 rounds; the x86 build direct 24.4 million. Through the whole engine: system WinMM 0.053 million events per second (99th percentile lateness 8.1-8.6 s at 1 million per second), direct WinMM 17.6 million over capacity and the full 9.501 at 9.5 million per second with a 99th percentile of 205 microseconds, against winmmwrp 18.5 and KDMAPI 24.4 million; the heaviest passage of user file 2: maximum lag 14.9 ms direct, 15.5 ms winmmwrp, 3.9 ms KDMAPI. The user found direct access "surprisingly effective", not as fast as winmmwrp or KDMAPI but somewhat close. VirtualMIDISynth #1: direct 5.4 million messages per second in the check (limited by the synth; compatibility only). **Stress finding, before the fix:** direct access to OmniMIDI hung during open / flood / stop, close or panic cycles (the 30 s watchdog fired in 1 of 3 modes at 4:11-4:26 AM and in all 3 at 6:02-6:29 AM; no process left); the fix is pending as DD-094. See `DEVICE_MEASUREMENTS.md`.
- **Risk:** medium, contained: the option is off by default. A driver other than the two tested ones is used at the user's choice and marked unverified. Calls other than the per-event data call run inside an exception guard, so a crashing driver gives an error message. The direct path cannot play a device that a standard WinMM handle already holds open (the drivers answer "allocated"). Several programs driving the same synthesizer driver at once can make that driver (OmniMIDI in the tests) open slowly, crash or stall, with either path. The engine's single-sender rule is unchanged.
- **Harness rules:** none. No v1 trace changes (the option is off in every recorded scenario; the engine and parity runners never use it).
- **Amended by DD-094** (closing the direct path while a driver call is still running).

- **Amended by DD-127 and DD-130 (prompt 012):** the option is a **Direct WinMM** checkbox on the output row (next to KDMAPI) and no longer a system-menu item; its status line is replaced by the checkbox's tooltip; the old trial label and the "unverified driver" wording are gone from the program (the tooltip says whether a driver has been tested). The box is greyed out as soon as an output that cannot use Direct mode is selected (DD-127). `wdmaud2.drv` (Windows MIDI Services) is refused like `wdmaud.drv` (DD-130). Direct WinMM is a normal feature, off by default (user decision, prompt 012).

## DD-094 — Direct WinMM driver access: a close never runs underneath a driver call that has not returned, and a reset or close that does not return is given up (amends DD-085)

- **Category:** bug fix (robustness of the DD-085 path; no musical change)
- **What changed:**
  - Found by the prompt 009 stress test on the real OmniMIDI driver (4:15 AM, 16 Oct 2026): open the device directly, flood it, Stop during the flood, 200 times. The "stop" run ended through its 30-second watchdog, by its length (73 s, of which 30 s waiting, at about 0.32 s per cycle) after roughly 130 good cycles, not in the first one; the "panic" run (exactly the same steps in the tool) and the "close" run passed 200 of 200, so the hang is rare, not tied to the mode. The tool did not record which step hung.
  - Code facts: every wait of the engine's Stop is bounded (2 s, then "Unable to stop MIDI output safely"), and no engine lock is held across a driver call, so Stop itself cannot wait 30 s. What was unbounded: the direct output's own driver calls on the closing thread, and nothing kept a close (MODM_RESET, MODM_CLOSE) or a new open from running while another thread was still inside the driver's MODM_DATA; OmniMIDI can stay inside MODM_DATA while its queue is full. With standard WinMM, winmm's handle sits between the two; the direct path had nothing. The player could reach this: after a Stop that timed out, pressing Play closed and reopened the direct output underneath the still-running call.
  - v2 now: (1) the direct output counts the calls in progress (two interlocked operations per call, no lock; amended by DD-110: now one per message and none inside a burst). A close waits at most 2 s for a call another thread has inside the driver; if it has not returned, the close returns at once with a plain message and the thread inside the driver resets and closes the device itself the moment its call returns. From the close request on, nothing more is sent (sends fail, reset and panic do nothing, a "driver busy" retry gives up), and the device is not opened again until that close is done. (2) Play while a timed-out Stop's call is still running shows "MIDI output is still stopping" and touches nothing; Play works again once the call has returned. (3) The stress tool names the cycle, the step and the calls in progress when its watchdog fires, and ends the cycles after a Stop that timed out instead of opening the device a second time.
  - **Second part (the close step itself).** The real-driver repeat after part 1 (8:21-8:31 AM, OmniMIDI, 200 cycles per mode) still hit the watchdog in all three modes, now named precisely: cycles 34 (stop), 61 (close) and 154 (panic), each in the close step (device reset, then MODM_CLOSE) with no other call in progress. So OmniMIDI sometimes does not return from a reset or close made on its own; part 1 was still needed (it removes the overlap) but cannot help here. v2 now: (4) the device's reset (Stop, Seek, Pause, before closing) and MODM_CLOSE run on a short-lived helper thread and the caller waits at most 1.5 s (inside the engine's 2 s Stop wait, so Stop ends with a clear message). (5) A call that does not return in time is abandoned: the helper thread stays inside the driver and keeps it loaded; nothing more is sent (no all-notes-off either: the driver no longer answers); the driver is recorded as having stopped answering and is not called again in this process, by this output or a new one, nor asked for its devices; the next Play therefore uses standard WinMM, and the status line says "Direct WinMM: the synthesizer driver stopped answering; standard WinMM is used from the next playback." and, after that Play, why direct access is not available. (6) The stress tool's watchdog and summary name the call (MODM_RESET or MODM_CLOSE) and the time from the last MODM_DATA to it, and the tool can compare reset styles on the real driver (--close-style reset | notesoff | settle; -StressCloseStyles in run-device-measurements.ps1): notesoff replaces MODM_RESET by the 48 Panic controller messages, settle waits --settle-ms before MODM_RESET. The player keeps MODM_RESET (v1's midiOutReset) until a comparison shows a better choice; changing it would be proposed to the user first.
- **Why:** Stop and Play must never hang the player, and a synthesizer driver must never get a reset or close in the middle of a call it has not finished (a likely way to deadlock it). A driver that stops answering must cost the user a message and a fallback, never a frozen window. Sound safety is unchanged or better: after Stop nothing more is sent, and the device is still reset (all notes off) before it is closed.
- **User-visible effect:** normally none. Only when a synthesizer stops answering for more than 2 s: Stop reports it as before, and Play then says the output is still stopping instead of risking a hang. When the driver hangs in a reset or close: Stop (or closing, or switching the output) returns after about 1.5 s with a message saying the driver stopped answering, the status line says so, and the next playback in that session uses standard WinMM; restarting the program tries direct access again. Musical behaviour and timing are unchanged; the counting adds a few nanoseconds per message on the direct path.
- **Evidence:** `tests/direct_output_tests.cpp` `test_close_while_driver_full` (a fake driver whose MODM_DATA stays inside while "full": close returns after its bounded wait, no MODM_RESET/MODM_CLOSE during the call, sends refused, no second MODM_OPEN, the sender's thread resets and closes after the call returns, the device opens again; a "busy" retry stops at a close); `test_driver_stops_answering` (a fake driver whose MODM_CLOSE or MODM_RESET does not return: the call is given up after its limit, nothing more is sent, no MODM_CLOSE after a hung reset, no second open by this or a new output, the helper thread keeps the driver loaded until it returns; the notesoff and settle styles); `tests/app_tests.cpp` `test_play_waits_for_stuck_output`, `test_status_when_driver_stopped_answering`; `mutation-check.ps1 -Family direct` (eight mutations, 8 of 8 detected); real-driver watchdog lines of 8:21-8:31 AM in `v2\out\device-test\results\20261016-082127\raw\`; the real-driver repeat of the stress test is the main session's (OmniMIDI only it may open).
- **Risk:** low to medium. The normal path adds the call count and one short-lived thread per reset or close (tens of microseconds; never per message). Notes still sounding when a driver stops answering cannot be silenced by this program. Standard WinMM after an abandoned call goes through the same driver, which may still be stuck; that is the user's fallback, as asked, and is reported in the status line. MODM_OPEN and the per-message MODM_DATA have no time limit (a thread per message is far too slow; MODM_OPEN on a helper thread could change the driver's behaviour); the stress tool's watchdog still names them if they hang.
- **Harness rules:** none (the direct path is off in every recorded scenario).

## DD-086 — With a waiting-time limit, "Clear buffer and jump to realtime" is the first Overflow choice

- **Category:** judgment call (decided by the user, prompt 009, decision 4)
- **What changed:** v1: with "Limit queue by waiting time" on, the Overflow list is Drop oldest, Drop oldest complete note, Clear buffer and jump to realtime. Preview (DD-025): Drop oldest complete note (soft limit), Drop oldest events, Clear buffer and jump to realtime. v2: **Clear buffer and jump to realtime**, Drop oldest complete note (soft limit), Drop oldest events. Only "Clear buffer" moved; the default of this list (Clear buffer) and the list without a waiting-time limit are unchanged. The choice is remembered by policy, not by position, when the waiting-time limit is switched off and on (`player_controller.cpp`, `populate_overflow_policies`).
- **Why:** the user's decision: the default of the list is its first entry.
- **User-visible effect:** the order of the three choices with a waiting-time limit.
- **Evidence:** `tests/app_tests.cpp` `test_queue_settings`: the order, the default (index 0, Clear buffer, reaching the engine), index 2 = Drop oldest events, and the policy restored by identity after leaving and re-entering waiting-time mode: `midibn-app-tests` PASS (100 checks, 0 failures), 2026-10-16, subtrottle.
- **Risk:** none for behaviour (the same policies; traces, analysis settings and scenarios use the policy, not the list position).
- **Harness rules:** none (no v1 table lists this order).

## DD-087 — The Channels grid shows its focus rectangle only after a key press

- **Category:** judgment call (decided by the user, prompt 009, decision 3)
- **What changed:** preview (DD-031): the dotted focus rectangle on the current cell was drawn whenever the grid had the keyboard focus, so it appeared as soon as the window opened. v2: it appears only after the first key press in the grid (or in its edit box), and then follows the keyboard focus as before. There is no filled highlight (v1 had none either). `channel_grid.cpp` (`note_key_press`, `focus_cue_visible`).
- **Why:** a mouse user does not need the keyboard cue; the user decided it should appear only once the keyboard is used.
- **User-visible effect:** no dotted rectangle in the Channels grid until a key is pressed there.
- **Evidence:** `tests/channels_window_smoke.cpp`: no focus cue when the window opens; after a key press the cue follows the keyboard focus: CHANNELS WINDOW SMOKE: PASS (90 checks), 2026-10-16.
- **Risk:** none.
- **Harness rules:** none (v1 had no keyboard cue).

## DD-088 — The Direct WinMM status line is short, on the right, with the full text in its tooltip

- **Category:** clearer message; layout change approved by the user (prompt 009, Step 5.2 and 5.3, decision 7)
- **What changed:** preview (DD-085): the status line started right after the "File and output" heading and showed the whole sentence, which was cut off at the default width (for an unavailable device it was unreadable) and had no working tooltip. v2: the line is right-aligned in the caption row and as wide as its text, and shows a short form that fits at the default width: "Direct WinMM: <driver> (tested)" / "(unverified)", "Direct WinMM unavailable: standard WinMM in use", "Direct WinMM on: device checked at Play", "Direct WinMM: not used with None" / "with KDMAPI". Its tooltip shows the full sentence followed by the explanation of the option (the MIDI output box's tooltip still has the full sentence). In the borderless Integrated header style (no caption row) the short line is now visible as well, in the output row between the MIDI output box and KDMAPI; the box keeps room for its longest device name and the line is hidden (tooltips only) if less than 90 pixels would remain. The short text comes from the controller (`PlayerView::output_path_short`).
- **Why:** the user could not read the message at the default width (prompt 009, Step 5.3), and asked for the right side (Step 5.2) and for a visible status in the Integrated header style (decision 7).
- **User-visible effect:** with the option on, a short status at the right of the File and output caption (or in the output row of the Integrated header); hovering it shows the whole explanation.
- **Evidence:** `tests/app_tests.cpp` `test_direct_winmm` (the short text in every state); self-test (`--selftest`): the line shows the short text, sits at the right side of the caption row, fits at the default width, its tooltip has the full sentence, it takes the mouse; in the Integrated header it lies between the output box and KDMAPI and fits. Visual gate states `direct-*` (gallery sheet 07). Results 2026-10-16: app tests PASS (100 checks); self-test SELFTEST: PASS (169 checks); visual gate `-Visible` x64 and x86 PASS (the seven `direct-*` states included).
- **Risk:** low; presentation only.
- **Harness rules:** none.

- **Superseded by DD-127 (prompt 012):** the status line no longer exists. The sentences it carried are the tooltip of the **Direct WinMM** checkbox (texts in DD-127).

## DD-089 — The tooltips of the file name, file information, status and loading texts appear

- **Category:** bug fix (a v1 feature that did not work in the preview)
- **What changed:** preview: the main window's text labels let every mouse message pass through to the window below, so the tooltips registered for the file name, the file information and the Direct WinMM status line never appeared, and the loading hint was not attached at all. v2: these labels take the mouse wherever the main window would treat the point as ordinary client area (a borderless drag zone or resize edge is still passed through, so the Integrated header still drags the window); while a file loads, the loading status and both progress bars show v1's hint (the stage, then "The upper bar shows overall parser work; the lower bar shows the current stage. Click Cancel to stop loading."; v1 `MainForm.cs` sets it on the loading panel and its status label). `controls.cpp` (`LabelTooltip`), `main_window.cpp` (`tooltip_for`).
- **Why:** v1 shows these tooltips; for a text cut off with "..." the tooltip is the only way to read it.
- **User-visible effect:** hovering the file name, the file information, the status line or the loading display shows its tooltip, as in v1.
- **Evidence:** self-test: the status line answers the mouse hit test as client area (the tooltip's precondition): SELFTEST: PASS (169 checks), 2026-10-16. The tooltip text itself is checked (full sentence); its appearance under a real mouse was not driven (the tests never move the real mouse).
- **Risk:** low. Clicking a label no longer reaches the frame below it, which has no click action.
- **Harness rules:** none.

## DD-090 — Resizing the main window repaints cleanly

- **Category:** bug fix (found by the user, prompt 009, Step 4.1)
- **What changed:** preview: while the main window was resized, old pixels stayed on the screen, most visibly in the File and output and Statistics frames (file name and file information, the state text, the status line, the statistics values and columns, frame lines). Cause: the text labels and the statistics view draw from their full width (right-aligned or centred text, "...", columns at half the width), but their window classes did not ask for a full repaint on a size change and they did not invalidate themselves, so Windows kept the old pixels and repainted only the newly uncovered strip; and the layout let Windows copy the old pixels of every resized control. v2: these classes repaint completely when their size changes (`CS_HREDRAW | CS_VREDRAW`, plus an explicit invalidation in the label and statistics `WM_SIZE`), and the layout resizes a control with `SWP_NOCOPYBITS` (a control that only moves keeps its pixels).
- **Why:** visible garbage on resize is a defect.
- **User-visible effect:** resizing (growing, shrinking, slowly or fast, either border) shows a clean window throughout.
- **Evidence:** self-test: after shrinking and after growing the window, every label, the statistics view and every frame whose width changed has its whole area waiting to be repainted (the read happens before anything is painted). Visual gate `-Visible` resize checks (each frame style at 50-200 %, v2 and v1): the screen as the resize left it equals a complete repaint, pixel for pixel; the gate was first proven to FAIL on the preview build (`b06107f`). Results 2026-10-16 (subtrottle; the test monitor read back black because the display was off, so the captures came from each window's composed surface without a forced repaint, see BUILDING.md): preview build `b06107f`, `-Quick` (scales 100 and 200, four frame styles): VISUAL-GATE: FAIL (130 v2 findings), every style and scale, up to 26,333 stale pixels in one step (stale file-information text and statistics columns, the user's symptom); this build: PASS (0 of 208 steps). Full run, every style at 50-200 %: v2 24 x 26 steps 0 stale pixels; v1 24 x 26 steps 0 stale pixels. The full run first found one more case: a size set by another program while the player was still building its window was dropped by the layout (`relayout` ignored a size change that arrived during a layout pass), leaving a layout for the old width (8,184 stale pixels at overlay 75 %); `relayout` now runs again for the final size. Self-test: after shrinking and growing, every label, the statistics view and all four frames are wholly marked for repainting (SELFTEST: PASS, 169 checks).
- **Risk:** low. A resized control is repainted completely, which costs a little more drawing during a drag.
- **Harness rules:** none.

## DD-097 — The Channels window fits its grid at 75 % and 125 %, and opens on the main window's monitor

- **Category:** bug fix (found by the prompt 009 clipping review)
- **What changed:** preview: at Application scale 75 % (and 125 %) the auto-fitted Channels window was one pixel narrower than its columns, because the fit used `px(12)` for the two side margins while the layout uses `2 * px(6)` (9 against 10 pixels at 75 %); a horizontal scroll bar appeared and with it a vertical one, so the Position column and channel 16 were cut off. Also, the window was placed next to the main window but kept inside the PRIMARY monitor's working area, so with the main window on another monitor it opened on the primary one (v1 keeps it on its own screen, `Screen.FromControl`). v2: the fit uses the layout's own margins (identical at 50, 100, 150 and 200 %), and the placement uses the main window's monitor (`channels_window.cpp`).
- **Why:** clipped columns and rows; a window opening on another monitor.
- **User-visible effect:** at 75 % and 125 % the whole grid is visible without scroll bars; on a multi-monitor desktop the window opens beside the main window.
- **Evidence:** visual gate `-Visible`, `win75-channels` before (scroll bars, Position and channel 16 cut off: `v2\out\visual\x64-visible\v2-win75-channels.png`) and after (`v2\out\visual\x64-channels-fix\v2-win75-channels.png`, all 16 rows and every column); CHANNELS WINDOW SMOKE: PASS (90 checks; the 100 % fit is unchanged: client 1066x411). 2026-10-16.
- **Risk:** low; presentation only.
- **Harness rules:** none.

## DD-098 — Every secondary window and dialog opens on the main window's monitor

- **Category:** bug fix (found by the user, prompt 009: the Track routing window opened on the primary monitor while the main window was on the other one)
- **What changed:** preview: several windows measured or clamped against the PRIMARY monitor's work area, or were not kept on any monitor at all. v1 places each of them on its owner's screen (`FormStartPosition.CenterParent`, which centres over the owner and keeps the window inside the work area of the owner's screen, or `Screen.FromControl`). v2 now does the same through one shared rule (`monitor_work_area`, `centre_on_owner` in `ui_common.cpp`), window by window:
  - **Track routing** (`routing_dialog.cpp`): was centred over the main window but then pushed inside the primary monitor's work area, so it jumped to the primary monitor. Now centred over the main window inside that window's monitor. Its size, which v1 derives from the PRIMARY monitor's work area (`Screen.PrimaryScreen`), now comes from the main window's monitor, where it opens (an improvement over v1: on a small second monitor it no longer comes out larger than that monitor).
  - **Help** (`dialogs.cpp`): same fault as Track routing (primary work area); now centred over the main window inside its monitor, and at most that monitor's work area in size (v1 `OnShown`, `Screen.FromControl`).
  - **About, the large-file warning and the Effective playback speed window prompt** (`dialogs.cpp`, shared dialog frame): were centred over the main window without any monitor check and only kept right of and below the point 0,0 (so on a monitor left of or above the primary one they were moved off it, and near a screen edge they could hang partly off it). Now centred over the main window inside its monitor's work area (v1 CenterParent).
  - **Custom Analysis resolution prompt** (`analysis_window.cpp`): same as About, relative to the Analysis window; now centred over the Analysis window inside the work area of the Analysis window's monitor (v1 CenterParent).
  - **Analysis window** (`analysis_window.cpp`): already opened beside the main window on the main window's monitor (v1 rule: right edge + 12, else left + 36). Two refinements: it is also kept left of the work area's right edge (v1 could leave it partly off its screen when the main window was near the right edge), and an Application scale change keeps it inside the work area of the monitor it is on now (v1 `Screen.FromControl(this)`), not the main window's.
  - **Channels** (`channels_window.cpp`): fixed in DD-097; it now uses the same shared helper (no change in behaviour).
  - **Message boxes** are placed by Windows on their owner's monitor (every v2 message box has an owner, as in v1); not changed.
  - **Main window** (not changed, proposal): v1 opens it centred on the monitor under the mouse pointer (`CenterScreen`); v2 opens it on the primary monitor.
- **Why:** a window opening on another monitor than the one the user works on, or partly off the screen, is a defect; v1 keeps each of them on its owner's screen.
- **User-visible effect:** with the main window on any monitor, Track routing, Help, About, the prompts, the large-file warning and the Analysis and Channels windows open on that same monitor, wholly inside its usable area (above the taskbar). On a one-monitor desktop nothing changes except that a window near a screen edge is no longer partly off the screen.
- **Evidence:** routing dialog smoke test: every opening of the dialog checks that it is on the stand-in main window's monitor, inside its work area and centred over it; arithmetic checks on a two-monitor layout like a common one (a 4K primary monitor and a 1920x1080 monitor to its right and lower), a monitor with negative coordinates, a corner and a window larger than the work area (no window is opened on the primary monitor). Visual gate `-Visible`: every new window and dialog is checked, before the gate moves anything, to open inside the work area of the test monitor, where the main window is (`placement:` lines, `placements.txt`). Results 2026-10-16 (Balanced plan): routing dialog smoke test PASS at 100, 75 and 50 % (65 checks each, stand-in main window on the test monitor), Analysis window smoke test PASS (68 checks, 2 new placement checks); visual gate run `v2\out\visual\x64-p9ui2-run2\placements.txt`: all 27 v2 windows and dialogs (Analysis, Channels, Track routing, About, Help, both prompts, at 100, 75 and 50 %) opened inside the test monitor's work area, and so did v1's 23; final run on the finished build (`v2\out\visual\x64-p9ui2-final\placements.txt`): the same, 27 of 27 v2 and 23 of 23 v1 inside. Not run on the LG monitor (the user's rule: no test windows there); that side is covered by the arithmetic checks.
- **Risk:** low; placement only.
- **Harness rules:** none.

## DD-099 — Nothing is cut off at Application scale 50 % (check boxes, buttons, prompts, About, header, Channels)

- **Category:** bug fix (found by the prompt 009 clipping review at 50 %)
- **What changed:** below 100 % the font shrinks less than the layout (v1's `UiScaleFont` keeps it at 5.5 points at 50 %), and some room does not shrink at all (the check box itself, a push button's border, the caption font). At 50 % the preview cut off:
  - the labels of the **KDMAPI** and **Queue length limit:** check boxes (main window, every frame style), **5-pin DIN** (same rule), **Playback statistics** (Analysis) and the channel numbers **10-16** (Track routing): the room for the box was the scaled 22 (18) pixels instead of the box's real width;
  - the lower part of **push-button texts** (Play, Stop, -5 sec, +5 sec, Analysis..., Reset stats, Use original routing, Apply): the buttons were the font height plus the scaled 8 (10) pixels;
  - the **About** box's last line, and the label "Measurement window:" of the **Effective playback speed window** prompt, "Aggregation interval:" and the note of the **Custom Analysis resolution** prompt: fixed widths that were too narrow for the text, so the text wrapped onto a second line that was not shown;
  - the **titles** of About and both prompts ("Effective playbac...", "Custom Analysis resolu..."), because the caption font does not follow the scale;
  - the **file information** in the integrated header ("...ents  •  9 tracks  •  ..."): the right-aligned one-line text was wider than its place;
  - the last word of the **Channels** explanation ("historical"): when the window grew to fit its grid, the explanation kept the pixels it had drawn for its earlier, narrower width (where that word had wrapped onto a hidden second line) and repainted only the newly uncovered strip, so only the final "." appeared (the same kind of fault as DD-090).
  
  v2 now: check boxes get at least the box's system width plus 4 pixels (`check_box_space`), buttons at least 8 pixels of padding (`button_padding`); the dialogs measure their texts and titles and grow to fit them, moving what follows a label and keeping OK / Cancel at the right edge (`caption_fit_client_width`); the file information takes the compact layout's two-line form when the one-line form does not fit; the Channels explanation is repainted completely whenever its size changes.
- **Why:** text and controls that are cut off.
- **User-visible effect:** at 50 % every label, button text, title and the whole explanation are readable; the main window is up to 7 pixels taller, the prompts and About a little wider, the Track routing channel boxes wrap onto a second row. At 100 % and above nothing changes (the 100 % captures have the same sizes as before); at 75 % the main window is 2 pixels taller (button padding) and the prompts are wider so that their titles fit.
- **Evidence:** visual gate `-Visible` on the test monitor. Before (2026-10-16, build with DD-098 only, `v2\out\visual\x64-p9ui2-run1\` and `x64-p9ui2-run2\`): `v2-scale-50-std.png` (KDMAF, "Queue length limi", "Plav" / "Stoo"), `v2-dlg50-about.png`, `v2-dlg50-speedwindow.png`, `v2-dlg50-customres.png`, `v2-direct-integrated-50.png`, `v2-win50-routing.png` (1( 11 12 ...), `v2-win50-analysis.png`, `v2-win50-channels.png`. After: the same names in `v2\out\visual\x64-p9ui2-final\` (gallery `v2\out\visual\gallery-p9ui2-final\`, sheets 02, 03, 04b and 05c). Results 2026-10-16 (Balanced plan): final visible gate with v1 on the test monitor VISUAL-GATE: PASS (95 v2 and 71 v1 captures, 0 v2 findings; resize checks 24 styles x scales, 0 steps with stale pixels); 100 % captures unchanged in size (main window 806x505, About 406x204, prompts 346x144 and 406x159, Track routing 1066x729, Analysis 1096x699; the Channels width varies with the program name shown); SELFTEST: PASS (169 checks) on x64 and x86; routing dialog smoke test PASS at 100, 75 and 50 % on x64 and x86; Analysis window smoke test PASS (68 checks); CHANNELS WINDOW SMOKE: PASS (100 % fit unchanged, client 1066x411); unit, app, UI, routing, Channels and Analysis model tests PASS on x64 and x86; x64, x86, XP and XP-IA32 build. Parity skipped (frontend-only changes; the x64 part of a started run passed before it was stopped).
- **Evidence (P10-FIX, 2026-10-17):** the final full gate showed `win50-channels` once with the explanation line blank. Cause: with the 6 pt minimum at 50 % (P10-UI1) a live capture (one forced repaint, one PrintWindow) of the Channels window came out with that one label blank in about 1 of 12 captures under load (0 of 240 with 5.5 pt, 0 of 640 on the DD-099 build); a capture taken again at once, with or without a forced repaint, was always complete, so the program does not leave the label blank. The gate now takes a live capture that shows a blank control again, up to twice (`visual-gate.ps1`, `Take`). 24 of 24 loaded runs of that state PASS afterwards (8 at a time).
- **Risk:** low; presentation only, below 100 %.
- **Harness rules:** none.

## DD-091 — The Open MIDI... button fits its text

- **Category:** layout change approved by the user (prompt 009, Step 5.1)
- **What changed:** preview: the button was the width of "Open MIDI..." plus 24 pixels. v2: the longer of "Open MIDI..." and "Cancel" (its text while loading) measured with the button's own font, plus normal push-button padding of 8 pixels a side, at least Windows' standard 75 pixels; all values follow the scale. At 100 % it is about 8 pixels narrower, and the file name and the MIDI output box start that much further left.
- **Why:** the user found the button wider than it needs to be.
- **User-visible effect:** a slightly narrower Open MIDI... button.
- **Evidence:** visual gate `-Visible` x64 and x86 PASS (every scale, layout and frame style; no clipped text); 82 px wide at 100 % (was 90; measured from the gate's window trees).
- **Risk:** none.
- **Harness rules:** none.

## DD-055 — Footprint measurements can be repeated (start-up timing, load-flow timing, a measuring script)

- **Category:** internal quality
- **What changed:** v1 and v2 until now: only the parser's load time and peak memory were measured (`perf-compare.ps1`); nothing measured start-up, the executable sizes or the 32-bit file-size limit. v2: `midibn-win32.exe --report` also writes the performance-counter readings at entry to the program, after the window was created and shown, and at the first idle moment, plus the time of each start-up step (`mark_<step>_ms`); the new tool `midibn-load-flow` times a load the way the player runs it (the inspection pass a big file gets first, then the parse); the new script `v2\tools\footprint-measure.ps1` measures sizes, load, load flow, start-up (always on a hidden desktop, never plays) and the 32-bit capacity, compares with earlier builds in interleaved order (A, B, A, B, ...) and records the power plan, processor load and processor performance of every set.
- **Why:** the milestone needs numbers that anyone can reproduce, on a PC that is shared and busy.
- **User-visible effect:** none (test switches and tools).
- **Evidence:** `footprint-measure.ps1` output in the milestone report; the new report keys appear only with `--report`.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** the script produced all the quiet footprint figures of 2026-10-16 (processor load 9-11 %, clock 145-146 %, labelled quiet): sizes, load, load flow and start-up for x64, x86, XP and XP-IA32; see `XP_COST_LOG.md` and `DEVICE_MEASUREMENTS.md`.
- **Risk:** low; the only code on the normal path is a performance-counter read at start and one more `PeekMessage` before the first message wait, and only when `--report` is given.
- **Harness rules:** none (no output difference).

## DD-056 — A large file loads faster

- **Category:** performance
- **What changed:** v1 and v2 until now: the merge stage (the longest part of a load) went through the track heap for every event, handed every event to both channel indexes (which ignore notes, polyphonic aftertouch, SysEx and system messages), made a non-inlined call for each stored event and each time lookup, and computed each event's time from the start of its tempo segment with a long multiplication and (on 32-bit) a 64-bit division. v2: (a) the track at the top of the heap keeps supplying events while its next event still sorts before the smallest next event of the other tracks (the order produced is exactly the heap's), (b) only controller, program, channel-pressure and pitch-bend messages are passed to the indexes, (c) the clock steps from the previously asked tick (the same exact whole microseconds and remainder; small steps need only a 32-bit division) and answers a repeated tick without a call, (d) stored events are appended and released through inline code.
- **Why:** load time of big files.
- **User-visible effect:** a big file opens sooner (see Evidence).
- **Evidence:** parity PASS on x64, x86, XP and XP-IA32 (every load, index and flow trace and the 3,956 engine runs unchanged); unit test `a clock asked many ticks in a row equals a clock asked each tick alone` (120,000 queries, repeated and backward ticks; it fails when the backward-tick fallback is removed); large fixture (8,000,160 events), interleaved runs, median, under load (see the milestone report for the full tables): x64 719 -> 602 ms, x86 1,012 -> 1,005 ms (noise: the busy PC), XP 1,114 -> 1,069 ms, XP-IA32 1,175 -> 1,086 ms; peak private memory unchanged at 220 MiB (it already equals the size after loading). Tried and not adopted: reading the file through a memory mapping (reading takes 5 ms of 600; a mapped view and copy takes 11 ms), a fast path in the track parser (5 percent of the parse), whole-program optimisation /GL /LTCG (not faster, 10 percent slower in the run), parsing tracks on several threads (a disk that is not yet cached would be read in 16 interleaved streams; needs the user's decision on threads in the loader).
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** load of the 8-million-event fixture (parser only): x64 484 ms, x86 678, XP 802, XP-IA32 820, against v1 (C#) x64 5,518 ms (11.4 times slower) and x86 7,526 ms; peak private memory 219-220 MiB against v1 761 MiB (x64) and 636 MiB (x86). The user's own files (x64 tool, user file 1, 2 and 3): load 5.75 -> 0.30 s, 204.7 -> 6.94 s and 598.7 -> 15.46 s; peak memory 776 -> 241, 13,348 -> 5,229 and 22,336 -> 11,182 MB. Supersedes the under-load times above.
- **Risk:** low; same output, checked by every trace comparison.
- **Harness rules:** none (no output difference).

## DD-057 — The player window appears sooner: the MIDI device list is prepared while the window is built

- **Category:** performance
- **What changed:** v1 and v2 until now: the list of MIDI output devices was made before the window existed (Windows starts every installed MIDI driver for it; about 70 ms on this PC). v2: the list is made on a worker thread started as the program starts, the window is built meanwhile, and the controller asks for the list at the first moment it needs it (`PlayerEnvironment::defer_device_listing`; until then its view shows only None; the first refresh, the output selection and Play all list the devices first). The list, the default selection (the first real device) and the behaviour of Play are unchanged.
- **Why:** start-up time.
- **User-visible effect:** the window is ready earlier (see Evidence).
- **Evidence:** `app_tests` `test_deferred_device_listing` (eager and deferred controllers give the same list and selection, the devices are listed once, choosing None first and playing works); in-process self-test PASS (143 checks) and `player-e2e.ps1 -None` PASS on all four builds; start-up of x64, entry to first idle, median under load: 195 -> 144 ms in a 9-run interleaved A/B (full tables in the milestone report).
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** launch to first idle 173.7 ms (x64), 195.6 (x86), 194.9 (XP), 190.4 (XP-IA32); program entry to first idle 111.7 / 120.8 / 122.7 / 116.1 ms; on x64 the window is shown 109 ms after entry. Measured with the window visible on the test monitor; the 144 ms above was entry to first idle on a hidden desktop under load. v1's start-up was not measured in this session.
- **Risk:** low; WinMM is asked from a second thread (it supports that). Sound safety: nothing changes about which output is selected at start (the first real device, as in v1); `--play` still selects None, reads it back and refuses otherwise.
- **Harness rules:** none (no output difference).

## DD-058 — Accessible names are set right after the window is shown

- **Category:** performance
- **What changed:** v1 and v2 until now: the names that screen readers read for the controls were set while the window was built, which starts COM and loads the accessibility service (tens of milliseconds). v2: they are collected while the window is built and set from a posted message right after the window is shown (the self-test sets them itself first).
- **Why:** start-up time.
- **User-visible effect:** none for a person; an accessibility tool that asks for a name in the first few milliseconds after the window appears may find it a moment later.
- **Evidence:** start-up marks (`access_names` step 28 ms -> 0 in the build phase); self-test PASS.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** not measured separately; it is part of the start-up figures under DD-057.
- **Risk:** low.
- **Harness rules:** none.

## DD-059 — Smaller programs: window and application code is compiled for size

- **Category:** performance (size)
- **What changed:** the window code, the application layer behind it and the text formatting (30 source files, listed in `CMakeLists.txt`) are compiled with /O1 instead of /O2; the engine, models, loader, indexes, Analysis scan and track routing keep /O2.
- **Why:** executable size.
- **User-visible effect:** none (the file is smaller).
- **Evidence:** `midibn-win32.exe` x64 1,710 -> 1,595 KiB, x86 1,534 -> 1,388, XP 1,619 -> 1,489, XP-IA32 1,608 -> 1,478. Measured and not adopted: /O1 everywhere (-18 percent, but it would slow the engine and the loader), /Gw /Zc:inline (no change), /GL /LTCG (-4.7 percent, no faster, much slower to build), delay-loading comdlg32/ole32/psapi/version (no measurable start-up gain). The linker already folds identical functions and removes unused ones (/OPT:REF,ICF are the default without /DEBUG).
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** sizes of the current builds (they contain later features): `midibn-win32` x64 1,669 KiB, x86 1,457, XP 1,557, XP-IA32 1,548; `midibn-perf` 728 / 619 / 730 / 725.
- **Risk:** low.
- **Harness rules:** none.

## DD-060 — The 32-bit programs can use up to 4 GiB on a 64-bit Windows: bigger files load

- **Category:** performance (capacity)
- **What changed:** the 32-bit player, `midibn-perf`, `midibn-dump` and `midibn-load-flow` are marked large-address-aware (`/LARGEADDRESSAWARE`). On a 64-bit Windows they get 4 GiB of address space instead of 2 GiB (on 32-bit Windows 3 GiB with the /3GB option, else unchanged). The self-test package is left as it was. The large-file warning thresholds are unchanged (1 GiB for 32-bit builds).
- **Why:** DD-022 raised the 32-bit limit to about 76 million events; this roughly doubles it again where Windows allows.
- **User-visible effect:** a 32-bit player on 64-bit Windows can load files of about 150 million events (before: 76 million), after the usual large-file warning.
- **Evidence:** files made by repeating the fixture's tracks, `midibn-perf` x86: 72 million events load with and without the flag; 80 million fail without it (`OutOfMemoryException`) and load with it (peak private 2.1 GiB); 104 and 136 million load with it (peak 2.7 and 3.5 GiB); parity PASS (the dump tool is large-address-aware); self-test and `player-e2e.ps1 -None` PASS on the 32-bit builds.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** not re-measured in the quiet session; the capacity figures above stand.
- **Risk:** low to medium: code that kept an address in a signed 32-bit number would break above 2 GiB (none found; the test runs above 2 GiB). On Windows XP without /3GB nothing changes.
- **Harness rules:** none.


## DD-065 — Dense playback is sent three to five times faster (immediate bursts)

- **Category:** performance
- **What changed:** when many events are already due (a dense passage, or a backlog after a stall) the engine sends them back to back. Per event it used to take the engine lock, build a full event record, read the clock and convert the time to microseconds. Now (a) an event is sent straight from its stored form when nothing needs the decoded record (no send observer, Channels monitoring off, no routed view); (b) the event in service is published with one atomic write instead of a lock (the statistics reader folds it in, so it sees what it saw before); (c) the clock is read once per group of up to 16 events, the group being sized to last about 2 microseconds (slow outputs give groups of one, i.e. the former behaviour). The messages, their order and their times are unchanged.
- **Why:** with a fast output the engine, not the output, limited dense playback: about 75-90 ns of engine work per event (DD-011 had halved it), against 30 ns for the send itself. The target was to match or beat v1's 11 million sends per second end to end.
- **User-visible effect:** dense songs and bursts clear their backlog three to five times faster; nothing else changes. Lag figures: see DD-067.
- **Evidence:** `v2\tools\throughput-measure.ps1` (real clock, real worker thread, outputs that make no sound), interleaved baseline (commit 70c8e50) and new build, 5 rounds of 3 plays each, 12 million events due at once, x64 under load (whole-PC CPU 30-90 %, balanced plan, clock 136 %): null output 13.99 -> 67.28 million events/s (4.81x, 71.5 -> 14.9 ns per event); counting output 12.18 -> 47.95 (3.94x), and the order-dependent hash of all messages is identical for both programs; recording output 12.49 -> 43.74 (3.50x). x86 (3 rounds, earlier build): 7.6 -> 44.0. Sampling profile of the new code: about half of the worker's time is the send path itself (record read and the output call). With a real output at 30 ns per send the end-to-end figure is therefore about 1 / (15 + 30) ns = 22 million events/s on this PC. Tests: `test_direct_send_equivalence`, `test_burst_lag_statistics`, `test_burst_snapshot_while_send_blocked`; mutation family `burst` (7 of 7).
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** interleaved baseline against current, 15 plays each, CPU load 8.6-13.4 %: x64 null output 17.28 -> 74.70 million events per second (4.32 times; 57.9 -> 13.4 ns per event), counting output 17.32 -> 73.87 (4.26 times, message hash identical), recording output 16.75 -> 63.53 (3.79 times); x86 null 9.63 -> 52.10 (5.41 times), counting 9.44 -> 47.74, recording 9.07 -> 42.27. Whole engine with real outputs, v1 against v2: KDMAPI 4.04 -> 24.4, winmmwrp copy 4.19 -> 18.5, direct WinMM 17.6 million events per second; null output 5.82 -> 25.0 (the test file's own rate, so a lower bound). Supersedes the under-load times above.
- **Risk:** low. The lock-step comparison is unchanged (3956 runs identical) because it sets a send observer and its sends take virtual time, so it runs the decoded path with groups of one; the new paths are covered by the tests above, which also pass on the engine as it was before.
- **Harness rules:** none.

## DD-066 — Finding the events that are due no longer converts every probe

- **Category:** performance
- **What changed:** "which events are due now" (the search at each wake-up, and the Drop-mode admission loop) converted the time of every probed event into clock ticks (two multiplications and a division). The engine now converts the clock reading once into the latest event time that counts as due (`TimeConversion::latest_microseconds_due`, the exact inverse of the tick conversion), compares stored times with it, and searches outwards from the previous position first. Where the exact value is not available in 64-bit arithmetic the former per-event conversion is used.
- **Why:** the search was a visible part of Drop mode's cost per event and of every wake-up in a long song.
- **User-visible effect:** Drop mode with a dense song runs about as fast as Queue mode. An event time so large that its tick count overflows (about 29,000 years) no longer raises an overflow error at this point (unreachable with a real file).
- **Evidence:** Drop mode, null output, x64: 11.12 -> 53.62 million events/s together with DD-065 (4.82x); x86 6.1 -> 41.8. Unit test of the inverse over nine clock frequencies (`test_time_conversion`).
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** Drop mode, null output, 12 million events due at once: x64 15.37 -> 65.78 million events per second (4.28 times), x86 7.98 -> 48.66 (6.10 times).
- **Risk:** low.
- **Harness rules:** none.

## DD-067 — The lag statistics of a dense burst are read once per group of events

- **Category:** performance (measurement)
- **What changed:** "current lag" and "maximum lag" (how far behind its intended time an event was sent) are computed from a clock reading taken after each group of up to 16 events instead of after each event. The last event of a group gets its exact lag; the oldest event of the group is measured at the end of the group, so a reported maximum is never below the former value and exceeds it by at most the duration of a group (about 2 microseconds, by the group sizing of DD-065).
- **Why:** reading the clock and converting it per event was a quarter of the engine's cost.
- **User-visible effect:** the Max lag figure of a dense backlog can be up to about 2 microseconds higher than before; with a slow output (tens of microseconds per send) groups are one event and nothing changes.
- **Evidence:** `test_burst_lag_statistics` (the maximum lag after 100 simultaneous events equals 100 x the cost of a send for send costs of 0, 1, 3, 7 and 320 microseconds, with the decoded and the direct path); the same test passes on the engine of commit 70c8e50.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** the players' Maximum lag on a passage of 2.46 million events (user file 1, Balanced): v2 1.6-3.3 ms on None, KDMAPI, winmmwrp and direct WinMM, v1 51-337 ms. At 9.5 million events per second on x64 the 99th percentile lateness went 1,285 -> 787 microseconds (maximum 2,012 -> 1,218); the x86 baseline could not keep up (99th percentile 80.9 ms) where the current build delivers the full 9.50 with 777 microseconds.
- **Risk:** none for sound; the figure is display only.
- **Harness rules:** none.
## DD-075 — The Analysis queue projection is two to four times faster

- **Category:** performance
- **What changed:** (a) the complete-note tracker (which pairs each Note Off with its Note On) kept three sets of "unmatched" event numbers in hash tables and its per-key waiting lists in `std::deque`; it now keeps the three sets as flag bytes in 64 KiB pages indexed by event number and the lists as plain first-in first-out vectors (same order, same answers; `model_trace` prints the sets in the same ascending order); (b) the bucket of an event is remembered between events (`BucketLocator`) so the 64-bit division by the bucket length runs only when the bucket changes, and the gate output recording skips two divisions for times inside the known duration; (c) the loops read the time or tick of an event straight from the stored 24-byte record (`micros_at`, `tick_at`) and the workload scan reads that record directly instead of building a decoded copy; (d) `checked_add`, `checked_sub`, `checked_mul`, `EventView::data_byte` and `EventView::from_store` are inline (they were calls into another file for every event), and `checked_mul` returns at once when both numbers fit in 31 bits (no 64-bit division).
- **Why:** the finite-queue projection of the 8-million-event fixture took 2.2-4.3 s; most of it was hash-table and allocation work per note and calls for a few instructions of arithmetic.
- **User-visible effect:** Analysis of a very large file with a queue limit (and the same arithmetic during playback) finishes sooner; less memory is used while it runs. Nothing else changes.
- **Evidence:** every Analysis result hash (including the double-precision hash) is identical before and after on `large-8m.mid` and `routing-held-1000000.mid` in all four builds (`perf-compare.ps1 -Analysis`-style check); `parity.ps1` PASS on x64, x86, XP and XP-IA32; a differential run of 80 random MIDI files x about 590 Analysis configurations (all six overflow policies, paced timeline, forward-only queue, all four gates with polyphony limit and traffic scaling) against the previous build: 0 differences. Times (large-8m, median of 3 interleaved runs, PC busy, old = commit 70c8e50): x64 queue 2,182 -> 651 ms, x86 2,576 -> 887, XP-SSE2 2,730 -> 902, XP-IA32 3,451 -> 1,245; base workload 58 -> 41 ms (x64); `routing-held-1000000` x64 queue about 1,216 -> 200 ms.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** IA32 against SSE2 (7 interleaved runs, x86 builds, 8-million-event fixture): queue projection SSE2 771.5 ms, IA32 1,036.5 ms, ratio 1.343 (1.38 under load); modern x86 725.0 ms, x64 549.0 ms; result hashes identical.
- **Risk:** low; a pure data-structure change under the existing model traces and mutation checks (the mutation list was updated for the renamed lines of `workload_analysis.cpp`).
- **Harness rules:** none (no output difference).

## DD-076 — The per-note gate does less work per source tick

- **Category:** performance
- **What changed:** `PerNoteIntervalGate` (Analysis projection and playback): (a) source records are updated in place instead of being copied out, changed and copied back (160 bytes each way), and a freed or reused record is only written where something is read later; an invalid event view is cleared by its flag only; (b) the loops over the 128 pitches (end of a source tick, the due-boundary scan, the next-boundary minimum) visit only the pitches that can have work: one pitch bit set records which pitches got a note in the current tick, another which pitches may hold a finite boundary, and a pitch found idle drops out until something arms it again; the visiting order (ascending pitch) is unchanged; (c) a pending attack is looked at in place and copied only when it is actually emitted (it used to be copied for each of the 128 pitches at every boundary); (d) the boundary rounding uses one division and a precomputed limit instead of three.
- **Why:** after DD-075 the gate projection dominated the Analysis time, and on the IA32 build every 64-byte copy is a slow string-move instruction (see DD-077).
- **User-visible effect:** the Per-note interval projection (Analysis) and playback with the gate on use less processor time; nothing else changes.
- **Evidence:** identical Analysis hashes (above), `parity.ps1` PASS on all four builds, the gate model traces and the engine comparison (3,956 runs) unchanged, the same differential run as DD-075; times (large-8m, per-note gate projection, median of 3, old = 70c8e50): x64 2,118 -> 1,285 ms, x86 3,066 -> 1,518, XP-SSE2 3,128 -> 1,576, XP-IA32 4,273 -> 1,922.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** the four gate projections: SSE2 1,215.8 ms, IA32 1,383.1 ms, ratio 1.138 (1.22 under load); x86 1,236.6 ms, x64 1,081.8 ms.
- **Risk:** low to medium (the pitch bit sets must be armed at every place that gives a pitch a boundary; the gate model traces, the mutation checks and the differential run cover it).
- **Harness rules:** none.

## DD-077 — The event view is 48 bytes and is built without writing every field twice

- **Category:** performance
- **What changed:** `EventView` (the decoded copy of one event passed around by the models, Analysis and the engine) is reordered from 64 to 48 bytes (`static_assert`), and `EventView::from_store` fills it without first zeroing it (a private no-init constructor; the default view is still the all-zero invalid view).
- **Why:** the IA32 build (`/arch:IA32`, no SSE) copies a 64-byte structure with `rep movs`, which has a start-up cost of tens of cycles; profiling the IA32 build showed one such copy per pitch per boundary taking 19 % of the Analysis time. Fewer and smaller copies help every build, and the IA32 build most.
- **User-visible effect:** none except speed; the XP build without SSE2 (IA32) has a smaller Analysis penalty than before (see XP_COST_LOG.md, 2026-10-15 row).
- **Evidence:** results identical (hashes, parity, 3,956 engine runs); object-code scan of `per_note_gate.obj` before the change: 42 `rep movs` in the IA32 build against 6 in the SSE2 build.
- **Evidence, quiet (2026-10-16, quiet PC, Balanced plan):** base workload SSE2 52.6 ms, IA32 51.2 ms (0.973); load 805.8 against 809.7 ms (1.005); engine 4,977 against 4,940 ms (0.993): on a quiet PC the IA32 variant costs nothing measurable except in the queue and gate projections (`XP_COST_LOG.md`, 2026-10-16).
- **Risk:** low.
- **Harness rules:** none.

## DD-092 — An open MIDI Channel Monitor window no longer slows playback

- **Category:** performance (prompt 009, bug 4.2)
- **What changed:** while the MIDI Channel Monitor window is open the engine keeps per-channel live state (keys down, sent events, the last controller values). For a dense backlog (the immediate burst, DD-065) this cost far more than the send itself: (a) monitoring switched off the direct send from the stored events and decoded every event first; (b) every sent message took the monitor's lock and copied that channel's 160-byte state out and back; (c) before every send the engine read the clock to decide whether to publish a new monitor picture (at most 60 times a second). Now (a) the direct send is kept and the monitor's record of a message is taken from the stored form (`ChannelStateTracker::sent_record`), (b) a group of up to 16 sends (DD-065's group) is recorded under one lock when the group closes (`record_successful_batch`), the state is changed in place, and (c) the publication check uses the clock reading the group already takes. The monitor records the same messages in the same order with the same values; only when they are recorded moves, by at most one group (about 2 microseconds).
- **Why:** with the window open v2 played dense files about ten times slower (user report, prompt 009). v1 has the same per-message work, but its baseline per message is so much slower that the extra cost hardly shows.
- **User-visible effect:** playback with the Channel Monitor open is about as fast as with it closed; the window shows the same values.
- **Evidence:** `test_direct_send_equivalence` now also checks, with monitoring on, that the monitor's final state on the decoded and on the direct path equals recording every sent event one by one (v1 RecordSuccessful), and that both ways of reading a stored message give the same record; mutation family `burst` gained two cases (a group not recorded; the second data byte lost on the direct path), both must be detected. Throughput (2026-10-16 about 9:40 AM EDT, power plan "subtrottle", x64, output null, 12 million events due at 80 million per second, 15 interleaved plays each, old = engine of b06107f built with the same tool source; whole-PC load 11.5 % at the start, other processes 20-32 % median during the sets, so partly under load): channel monitoring on, old 6.73 million events per second (median; 34.09 with monitoring off, so the window cost 5.1x), new 19.60 (32.82 with it off, 1.7x); with the display refresh emulated at 60 Hz old 6.69, new 20.32. Monitoring off: old 34.09, new 32.82 (no change beyond noise). The remaining monitor cost is the monitor's own bookkeeping per message; with a real device's cost per send it shrinks further. GUI figures (v1 / old / new player with each window open): `window-throughput.ps1`, run by the main session.
- **Risk:** low; the monitor is display only (nothing it records is sent); a reset of the monitor's statistics requested from the window while a group is in flight applies before that group's records instead of between two of them (the same race v1 has, widened from nanoseconds to about 2 microseconds).
- **Harness rules:** none (the lock-step harness does not turn monitoring on; no output difference).

## DD-093 — With a forced override or a muted channel, a large backlog no longer freezes the player (v1 defect)

- **Category:** bug fix, v1 defect (prompt 009, bug 4.3; v1 is not changed)
- **What changed:** the Queue mode's statistics snapshot (queue length and queue age, read by every display refresh and by the Channel Monitor) counts the waiting events that pass the live source filters. v1 (`CountSnapshotEligible`) reused its previous count only when the start of the range was unchanged; during playback the start is the next event to send and moves on every refresh, so v1, and v2 until now, scanned the whole waiting backlog again on every refresh while holding the engine lock. With a forced override (or a disabled channel) active and an output slower than the file (about 15 million events per second and more), the backlog reaches tens of millions of events: a refresh took seconds, the window answered only every 1-5 s, and the engine, which needs the same lock between its bursts, sent in chunks. The count is now kept per block of 4,096 events (`SnapshotEligibleCounter`): a moved start subtracts the passed blocks, and only the events that became due or were sent since the previous refresh are examined. The numbers are exactly v1's for every sequence of calls (v1's cached answers included). Both this count and the engine's own admission of due events (`mark_new_source_filtered`) first try an exact shortcut: a stored event whose channel is neither disabled nor has an override (or that has no channel) passes both filters, which its stored record shows without decoding the event; only events of affected channels are decided the full way.
- **Why:** the interface must stay responsive and the output must slow down smoothly (user report, prompt 009).
- **User-visible effect:** with an override active during very dense playback the player keeps answering at its display rate and the output no longer stops and starts; the statistics show the same values. One full count still happens right after the filters change (when an override is set or cleared), as the engine's own retirement of the backlog does (v1 behaviour).
- **Evidence:** `test_snapshot_eligible_counter` (4,000 random calls — sending, new due events, seeks, filter changes, filters on/off, statistics resets — against the former algorithm: the same count and first eligible event every time); the engine comparison (the snapshot's queue figures are in the parity recordings) unchanged; mutation family `burst` gained one case (passed blocks not subtracted) and family `engine` one (the shortcut lets every channel pass). Responsiveness and output stalls (same session and conditions as DD-092; a silent output that takes at most 15 million messages per second, a song of 12.5 million events due at 25 million per second, monitoring on, one forced override, the display refresh's calls at 60 Hz; 12 plays each): old engine 0.42 million events per second (the 12.5 million events took about 30 s instead of under 1 s), refresh calls up to 722 ms (median of the per-play maximum; worst 761 ms), nothing sent for up to 719 ms at a time (worst 769 ms); new engine 10.20 million events per second (13.16 without the override and monitor), refresh calls up to 7.7 ms (worst 8.7 ms), longest stretch with nothing sent 16.6 ms (the measuring loop's resolution; 16.5 ms without the override). All events sent in every play of both. Parity: `parity.ps1` PASS on x64, x86, XP and XP-IA32 (3,956 engine runs identical); mutation families burst 10/10, engine and harness 64/64 detected.
- **Risk:** low (exactness covered by the randomized comparison and the parity recordings).
- **Harness rules:** none (no output difference).
## DD-095 — The XP release build (IA32) no longer copies events with a slow block-copy instruction

- **Category:** performance (XP release build)
- **What changed:** (a) `EventSource::at` returns the decoded event through two `return` statements instead of one conditional expression, so the 48-byte view is built straight into the caller's variable (the conditional built it in a temporary and copied it once more for every event read; all builds); (b) in the XP release build only (`/arch:IA32`, `MIDIBN_XP_IA32_BUILD`), `EventView` copies itself with twelve plain 4-byte moves (padding included, so the copy is byte-for-byte the default copy) instead of the compiler's `rep movs`; the SSE2, x86 and x64 builds keep the compiler's own copy; (c) the complete-note queue writes and unlinks its 36-byte nodes in place instead of building or copying them through a local, and the four places that rank notes for a polyphony or complete-note eviction fill their 40-byte candidates in place (all builds).
- **Why:** the user decided that the XP player ships only as the IA32 build (prompt 009). Small A/B tests between the IA32 and the SSE2 XP builds (`midibn-perf --micro`, XP_COST_LOG.md 2026-10-16) found no floating-point or 64-bit-division difference on the hot paths (no x87 instruction and the same `_alldiv`/`_allmul` calls in both builds' Analysis, queue and gate code); the whole remaining gap was block copies of 36- to 48-byte structures: the SSE2 build copies them with three 16-byte moves, the IA32 build with `rep movs`, whose start-up cost is far above the copy itself (copying 128 views 40,000 times: SSE2 8 ms, IA32 180 ms on this PC). The Analysis queue projection read every event through such a copy, and the per-note gate made several per boundary.
- **User-visible effect:** none except speed: the XP release build's Analysis queue and gate projections are faster and now about as fast as the SSE2 build's (figures below); every other build is unchanged or slightly faster.
- **Evidence:** `v2\tools\ia32-vs-sse2.ps1 -Runs 9 -Micro -Before <builds of commit f7a869e>` on this PC (Xeon W-11855M, Windows 10 under WOW64), power plan "subtrottle", **under load** from other work (processor load 38-60 %, mean 48 %; processor performance 74 %), 9 interleaved fresh-process runs per build after a warm-up, medians, `large-8m.mid`: Analysis queue projection XP-IA32 2,506 -> 1,764 ms (-30 %), XP-SSE2 1,891 -> 1,724 ms; per-note gate projection XP-IA32 2,994 -> 2,520 ms (-16 %), XP-SSE2 2,697 -> 2,477 ms; IA32/SSE2 ratio queue 1.33 -> 1.02, gates 1.11 -> 1.02; load, base workload and engine unchanged within the spread (IA32/SSE2 0.98-1.00 after). A first series (7 runs, load 37-78 %) gave the same picture (queue 1.31 -> 1.01, gates 1.16 -> 1.00). Micro workloads of the same run: copying 48-byte structures with the compiler's own copy, IA32 179 ms against SSE2 7.7 ms (23x); with the copy of (b), IA32 19.4 ms against 8.8 ms; reading decoded events 1.02x; 64-bit division 1.04x. Object code of the IA32 build, `rep movs` count: `workload_analysis.obj` 53 -> 18 (0 left in the queue projection), `per_note_gate.obj` 23 -> 2, `queue.obj` 9 -> 3. Every Analysis result hash (including the double-precision hash) and the engine output hash are identical in every run of all four builds, before and after; `parity.ps1` PASS on x64, x86, XP and XP-IA32 (traces 56, flows 112, models 10, v2 baseline 178, decimal 6,000 + 22,378, engine runs 3,956 identical); the unit, engine, platform, output, direct-output, app, Analysis, Channels, routing and UI tests pass on all four builds. See XP_COST_LOG.md, 2026-10-16.
- **Risk:** low: no arithmetic changed; the copy in (b) copies exactly the 48 bytes the default copy copies (`static_assert` on the size); the in-place writes in (c) set every field the copied local set, and the unlinked node is not written by anything else in that function (its neighbours are other nodes).
- **Harness rules:** none (no output difference).

## DD-096 — The queue projection asks for a service time without decoding the event when the mode does not read it

- **Category:** performance
- **What changed:** `ServiceDurationClock::next_microseconds(events, index)` returns the processing time (Processing time mode) or the next events-per-second step (Events/sec mode) without decoding the event, and decodes it only in MIDI bitrate mode, the one mode that reads it; the Analysis queue projections (finite, unlimited, output-paced, and the Events/sec service-demand pass) use it for every serviced event. The Events/sec step divides with one 32-bit division when both numbers fit in 32 bits (always, for rates up to 9,999,999 per second), instead of a 64-bit division and a 64-bit remainder, which on the 32-bit builds are two library calls.
- **Why:** the serviced event was decoded only to be ignored; on the 32-bit builds the 64-bit division pair is the most expensive step of the Events/sec clock.
- **User-visible effect:** none except speed.
- **Evidence:** the queue projection measured for DD-095 uses Processing time, so its figures include this change; identical results as listed there (all hashes, `parity.ps1` PASS on all four builds, including the rate, queue and Analysis model traces). The mutation checks for the Events/sec quotient and remainder now change the 32-bit lines (`rate` and `engine` families), and a new `analysis` mutation changes the Processing-time shortcut; `mutation-check.ps1` detected 50 of 50 (model families and harness) and 6 of 6 engine mutations in `rate.cpp` and `ordered_admission.cpp`.
- **Risk:** low: the results are the same by construction (the decoded event was not read in those modes; an unsigned 32-bit quotient and remainder of non-negative numbers below 2^32 equal the 64-bit ones); covered by the rate model traces, the queue model traces and the Analysis comparisons of the parity run.
- **Harness rules:** none.

## DD-100 — Check boxes are drawn at the Application scale (improvement over v1)

- **Category:** clearer interface
- **What changed:** v1: Windows draws the box of a check box at the system size (13 pixels at 100 % screen scale) whatever the Application scale, so at 125-200 % the box looks small next to the larger text, and at 50-75 % it is too big and the label is cut off. v2: the check boxes (main window: KDMAPI and "Queue length limit"; Analysis: "Playback statistics"; Track routing: the 16 channel boxes and the lossy-routing confirmation) draw their box at the Application scale (13 canonical pixels: 7 px at 50 %, 20 px at 150 %, 26 px at 200 %). They are still ordinary Windows check boxes: mouse, Space key, tab order, keyboard focus rectangle, disabled look, hover and pressed look (the Windows theme look when the theme is on, the classic look when it is off, as on Windows XP without a visual style) and screen-reader role are unchanged. The player has no radio buttons today; the same drawing code handles them if one is added. Code: `attach_scaled_button` in `v2/frontends/win32/controls.cpp`; the room a check box needs (`UiContext::check_box_space`) follows the scale, so the 16 channel boxes of the routing editor fit one row at 50 %.
- **Why:** the box must look in proportion to its label at every Application scale.
- **User-visible effect:** check boxes are larger or smaller together with the text.
- **Evidence:** the `ui` tests; pictures of the windows at 50 % to 200 % (visual gate). Windows XP: only XP-era calls (`uxtheme.dll` is loaded at run time with a classic-drawing fallback).
- **Risk:** low (drawing only).
- **Harness rules:** none.

## DD-101 — Readability at Application scale 50 % and 75 %

- **Category:** clearer interface
- **What changed:** the smallest font size at 50 % is now 6 pt (was 5.5 pt); 75 % keeps its 7 pt minimum. Greyed-out (disabled) text of v2's own drawn controls (labels, the greyed "2,000" queue field, the Analysis and Channels explanation text, the timeline time) is about 22 % darker at 75 % and 38 % darker at 50 %; at 100 % and above it is the system's grey as before (`UiContext::disabled_text_color`). The system-drawn drop-down boxes keep Windows' own grey. Side effect, expected: with the slightly larger 50 % font the file information (events, tracks, notes, length) no longer fits on one line next to the file name in the standard layout and takes two lines, as the layout already does when it runs out of room (the window is about 19 pixels taller at 50 %; queued question for the user).
- **Why:** the user found text at 50 % and 75 % hard to read.
- **User-visible effect:** slightly larger, darker small text at 50 % and 75 %.
- **Evidence:** the `ui` tests; pictures at 50 % and 75 %.
- **User decision 2026-10-18 (prompt 011):** the main window being about 19 pixels taller at 50 % is accepted.
- **Risk:** low.
- **Harness rules:** none.

## DD-102 — The "blue" file name and file information at 50 % (examined; no defect found)

- **Category:** finding (nothing changed beyond DD-101)
- **What changed:** nothing. The text colour is the system's black (checked in the code and in the pixels: the average ink is neutral, red = green = blue). The bluish look comes from Windows' ClearType (coloured sub-pixel edges) on very thin 7-8 pixel text; v1's pixels show the same technique. The file name and information read lighter than the button text because they are thin text on a light grey background, which the eye reads as blue-grey. Grey-scale smoothing was tried and is neutral but lighter and thinner, so it was not kept. The 6 pt minimum (DD-101) makes the text a little heavier.
- **Why:** user observation at 50 %.
- **User-visible effect:** none beyond DD-101.
- **Evidence:** pixel averages of the file-name label at 50 %.
- **Risk:** none.
- **Harness rules:** none.

## DD-103 — The Direct WinMM status sits at the top right of the Integrated header

- **Category:** clearer interface
- **What changed:** v1 has no direct-status line (DD-085 and DD-088 are v2 additions). v2 before: in the Integrated header style the short status sat between the output box and KDMAPI and was cut at 50 %. v2 now: in the Integrated header style only, the short status ("Direct WinMM: not used with None" and the like) sits in the top row, just left of the minimize button, at every Application scale. It takes at most 40 % of the header width; the file name and file information share the rest. When the status does not fit completely it ends with "..." (full sentence in its tooltip). When the file information would be cut off even in its two-line form (compact layout, 50 %), it is not shown there; the file name's tooltip then carries it (queued question for the user). Other frame styles are unchanged. While a file is loading the status is hidden in this style (the loading text needs the room).
- **Why:** the status was cut off at 50 % and crowded the output row.
- **User-visible effect:** as described.
- **Evidence:** self-test "integrated header: the status line is shown at the top right, left of the minimize button"; pictures of the Integrated header at each scale and layout.
- **User decision 2026-10-18 (prompt 011):** hiding the file information (tooltip only) in compact Integrated-header layouts is accepted.
- **Updated 2026-10-18 (prompt 011):** the status moved again. In the Integrated header style the short Direct status now sits on the same line as the **Playback** frame heading, at its right end, as wide as its text. It never touches the heading; when the row is too narrow it ends in "..." and the full sentence stays in the tooltip. It is no longer in the top row beside the minimize button, so the **file information is back in the top row** with the same fitting as the other header styles, and the status **shows while a file is loading** (the Playback heading row is free then; nothing moves). The tooltip fallback stays but is decided by measured text width: when the file information does not fit (for example Integrated at scale 50, compact layout) it moves into the file name's tooltip and the name gets the room; the file name's tooltip otherwise holds only the file path. The other header styles keep their place (File and output heading row). Evidence: self-tests "integrated header: the status line is on the Playback frame's caption row" and "file information stays visible" (the information is fully visible or in the tooltip in 14 layouts); visual-gate states `direct-int-<scale>-<std|compact>`, `direct-integrated-*`, `direct-int-loading-100/75/50`.
- **Risk:** low.
- **Harness rules:** none.

- **Superseded by DD-127 and DD-128 (prompt 012):** there is no Direct status line any more, in any header style. What sat on the Playback heading line in the Integrated header is now the playback state word (DD-128) in every view.

## DD-104 — Scrub boxes: logarithmic drag feel over the whole range without Shift

- **Category:** clearer interface (judgment call; the user asked for an easier drag)
- **What changed:** v1: queue length 4 pixels per unit up to 500, then logarithmic; waiting time 250 microseconds per pixel up to 1 s; processing time / bitrate / events per second v1's slider-based steps. v2 (queue length, queue waiting time, and the processing/bitrate/events-per-second box): one logarithmic curve over the whole range. Near the lower limit about one unit per 3 pixels (1 to 2 takes 3 pixels); every 100 pixels multiply the distance above the lower limit by e (about 1 % per pixel at the top), so steps get steadily coarser. Limits, rounding and number format are unchanged. Shift (fine) is exactly as before. Not changed: the interval polyphony limit (1 to 128, a linear box that is already easy to set) and the channel-monitor attribute cells (small fixed ranges with their own steps). v1's three mapping functions stay in `ui_mappings.h` as the reference.
- **Why:** reaching a large value took a very long drag.
- **User-visible effect:** dragging a number box reaches big values quickly and small values precisely.
- **Evidence:** `ui_tests` (the logarithmic mapping against the stated limits and rounding).
- **Risk:** low.
- **Harness rules:** none.

## DD-105 — Large-MIDI memory warning: the memory size on its own bold line

- **Category:** clearer message
- **What changed:** the window was already a small custom dialog of the program (Continue default, Cancel/Esc, no icon, no Alt keys: the same as v1's own dialog). It now puts a line break after "Estimated memory while the MIDI is open:" and shows that size alone on the next line in bold, and the whole 32-bit warning line (x86 builds) in bold. Buttons, default button, keyboard behaviour and results are unchanged.
- **Why:** the size is the number the user must read to decide.
- **User-visible effect:** as described.
- **Evidence:** pictures of the dialog (64-bit and 32-bit texts).
- **Risk:** none.
- **Harness rules:** none.

## DD-106 — The main window opens on the monitor under the mouse

- **Category:** bug fix (reclassified 2026-10-19 on the user's delegation, recorded in prompt 012, Step 1.4; it was queued as a judgment call). v1's `MainForm.cs` uses `FormStartPosition.CenterScreen`, which in Windows Forms centres an owner-less window on the monitor holding the mouse pointer. An earlier v2 build opened elsewhere (a bug); v2's current behaviour restores v1's.
- **What changed:** v1: CenterScreen (centred in the work area of the monitor holding the mouse pointer). v2 before: horizontally centred, one third from the top of the primary monitor (the bug). v2 now: centred horizontally and vertically in the work area of the monitor that holds the mouse (the nearest monitor if the pointer is outside every monitor), kept inside it. The test option `--position` overrides it exactly.
- **Why:** with two screens the window should open where the user is working.
- **User-visible effect:** as described.
- **Evidence:** `ui_tests` and `routing_dialog_smoke` (the placement rule with several monitors).
- **User decision 2026-10-19 (recorded in prompt 012):** keep v2's current behaviour; reclassified as a bug fix. The taste list's earlier wording about v1's start position was wrong and is corrected.
- **Risk:** low; to be checked once with a real mouse.
- **Harness rules:** none.

## DD-107 — Track routing list: the header is no wider than the list

- **Category:** bug fix (presentation)
- **What changed:** with a vertical scroll bar the header was left at the width the list had before the scroll bar appeared (17 pixels too wide), and the Status column was that much wider than visible (its text was cut a little early). The Status column and the header now end at the list's visible edge.
- **Why:** text was cut early.
- **User-visible effect:** Status text uses the full visible column.
- **Evidence:** routing editor pictures; the visual gate.
- **Risk:** none.
- **Harness rules:** none.

## DD-108 — v2 has its own Help text

- **Category:** documentation inside the program (no musical change)
- **What changed:** v1's Help window text is no longer shown word for word. v2's Help keeps the same eight topics in the same order (Overview, File and output, Rate model, Queue and overflow, Playback, Analysis, Statistics, Channels) and the same F1 mapping, but the text is written for v2: title-bar menu contents, Application scale (50 to 200 %), the three borderless styles, settings remembered between sessions (replaced 2026-10-25, DD-026), display paced to the monitor, windows opening on the main window's monitor, Direct WinMM driver access (off by default, status line, fallback, tested drivers), Track routing, the queue limit by count and by waiting time with each mode's Overflow choices and defaults, Output-paced timeline, Preserve playback queue, the Analysis counts-per-interval option and Custom resolution, the Channels keyboard use. The wording is plainer than v1's. v1's text is untouched. `help_text.cpp` is now written by hand; `harness/ui-reference/build.ps1 -EmitHelp` writes v1's text to `v2/out/ui-reference/v1-help-text.cpp` for reference instead of overwriting it. The v1 table files `help-*.txt` under `harness/expected/ui` are kept as v1's reference; the test no longer compares v2 with them except the topic titles (the DD-025 and DD-031 substitution list was removed with that comparison).
- **Why:** v1's text described v1's window and could not mention anything new (UD-008). The user approved "v2's own Help text". The user should read it once.
- **User-visible effect:** the Help window reads differently. Nothing else changes.
- **Evidence:** the `ui` tests (168,759 checks, 0 failures) check the topic titles and order, that each topic exists and uses CRLF line breaks, that each topic names the v2 items it must explain, and that v1's old overflow names do not return. Pictures of the Help window at 100 % and 50 %: `v2/out/P10-TEXT/visual/v2-dlg-help.png`, `v2-dlg50-help.png` (not committed).
- **Updated 2026-10-18 (prompt 011):** the Help is now written for easy reading: same eight topics, same order, same F1 jumps, same facts, but each topic has a heading, short sections with headings, bullet lists, numbered steps, bold names of what you see on screen and short examples. The Overview starts with a five-step Quick start and a topic list. The text is kept in a tiny neutral markup in `help_text.cpp` (`# ` heading, `## ` section, `- ` bullet, `1. ` step, `**bold**`); `parse_help_markup()` (portable core, unit-tested) turns it into blocks, and the Win32 window draws them in a read-only rich edit control (Riched20, present on Windows XP and later) with sizes taken from the scaled UI font, so they follow Application scale 50 % to 200 % (redrawn when the scale changes). If the control cannot be loaded the body falls back to a plain edit box with the same text. The Linux interface draws the same blocks with text tags. Evidence: `v2/out/prompt-011/p11-pictures/help/` (100 %, 50 %, and Help opened with F1 on the Rate model drop-down); `help` and `help-markup` tests in `midibn-ui-tests`; visual-gate states `dlg-help`, `dlg50-help`, `dlg-help-f1`. The Linux wording is DD-126; F1 inside the companion windows is DD-120.
- **Risk:** none for playback.
- **Harness rules:** none.

## DD-109 — Adaptive display rate: drawing never takes more than a set share of the interface thread, and backs off when playback is behind

- **Category:** improvement (v1 always drew at its own fixed rate; v2 adapts; a modern PC is unchanged)
- **What changed (code facts):**
  - New pure rule `core/include/midibn/app/display_tier.h` (measured costs in, tier out; unit-tested). The interface thread times what each live update costs it (the update itself, the repaint of the position bar and time, the repaint of the statistics block, other repaints) with the performance counter and, every half second, asks the rule for a tier.
  - Tiers, cheapest visible loss first (each keeps the losses of the one before). 0: everything at the screen's rate; only values that changed are redrawn (the cell-level invalidation that was already there; the statistics painter now also skips cells outside the repainted area). Always on, not a tier of its own. 1: position bar and time at full rate; the statistics are drawn half on one update (top two rows), half on the next (bottom two rows), so each statistic is at most one update (about 16 ms at 60 per second) older than the clock. 2 and further: the update rate is also lowered, 60, 30, 15, 10 per second. A rung must be clearly below the screen's own rate (59.94 Hz counts as 60, so 60 Hz screens have 30, 15, 10; a 144 Hz screen has 60, 30, 15, 10).
  - Budget (share of the interface thread's time that drawing may take), by the number of processors the player may use: 1 processor 8 %, 2 processors 12 %, 3 or more 25 %. With 1 or 2 processors drawing competes with the playback worker and the synthesizer driver; with 3 or more it has a processor of its own. Measured on a modern PC (12 processors, 60 Hz screens): one update costs about 1.7 ms of interface-thread time, i.e. 10 % of the thread at 60 per second and about 22 % at 144 per second, so a modern PC stays at full rate; a 2-processor PC with the same costs is just over its 12 % (it steps to tier 1, sometimes 2).
  - Stepping down: over the budget for two half-second windows in a row (or over twice the budget once). Stepping up: the share predicted at the rung above must be under 0.6 times the budget for six windows in a row. One rung per decision, so it cannot flicker (the unit tests sweep steady costs from 50 microseconds to 30 ms on 60 and 144 Hz screens and require no direction reversal and no change after settling).
  - Playback first: when the playback worker's lag is 20 ms or more in a window, drawing steps down one rung per window and does not step up while behind. **The main session limited this "playback behind" back-off to PCs with 1 or 2 usable processors (commit 30efd40):** with more processors a late output is the device's limit, so lowering the display rate would not help playback.
  - The display-pacing thread's rate is lowered (`DisplayPacer::set_rate_limit`), never its priority (the pacer stays at or below the playback worker). In display mode the pacer skips compositor passes on an accumulating deadline (a cap of 60 on a 144 Hz screen averages 60, not 72); in timer mode it lengthens its period. Nothing newer than Windows 98 SE or XP is needed (waitable timer or timeBeginPeriod as before, performance counter, `GetProcessAffinityMask`).
- **Test switches (never on by default):** `--adaptive-display off`, `--display-tier N`, `--display-cost-us N[,S]`, `--display-cost-for-ms MS`, `--display-pretend-behind START[,END]`, `--display-budget-pct P`, `--display-paint-all`. `--report` gains `display_adaptive`, `display_cost_us_per_update`, `display_tier_ms` and one line per tier change. Tool: `v2/tools/display-tier-measure.ps1 -Set cost|half|adapt` (opens the player with an explicit position, output None).
- **Why:** on slow or few-processor PCs drawing must not take the processor from playback; on fast PCs nothing is lost.
- **User-visible effect:** none on a PC that can afford drawing. On a slow or few-processor PC the position bar and time stay smooth as long as possible, the statistics lag by one update, and only then does everything slow to 30, 15, 10 per second; it returns by itself when the load goes. No setting, no visible control (UI_DIFFERENCES.md).
- **Evidence (a modern PC, 12 processors, two 60 Hz screens, not quiet; 3 runs each, medians; `large-8m.mid`, output None):** cost of one live update at 60 per second 1,740-1,810 microseconds = update 230-260 + bar and time repaint 450-490 + statistics repaint 910-970 + other repaints 95 (at a 144 per second timer 1,450-1,530). Changed-cells-only painting: 1,870 to 1,813 microseconds (3 %, within noise; kept, free). Half statistics: update 1,742 to 1,469 (-16 %) at 60 per second, 1,453 to 1,141 (-21 %) at 144; not half the cost because the statistics are about 55 % of an update and the repaint has a large fixed part; 30 per second takes the interface thread from 11.8 to 7.2 % of one processor. Tier behaviour: nothing pretended stays at tier 0; +3 ms per update reaches tier 2 at 2 s; +8 ms tier 3 at 2 s; +20 ms tier 4 (10 per second) by 2.6 s; two processors (`--affinity 3`) nothing pretended: tier 1 (sometimes 2); playback pretended behind from 3 s: one rung every half second down to 10 per second; +8 ms for the first 5 s of a 28 s run: down to tier 3 by 3 s, back up one rung per about 3 s, tier 0 at 15.2 s; adaptive off: stays at 60 per second. Updates per second matched the tier rates (30 per second at tier 2: 361 in 12 s). Statistic values stay correct (picture `v2/out/P10-ADR/shot-tier1.png`, not committed; timeline and statistics differ by at most one update). Not tested: a real 144 Hz screen in display mode, a 1-processor PC, real XP hardware. The tier tests were not re-run after commit 30efd40 (a one-line condition on the processor count).
- **Risk:** low; drawing only.
- **Harness rules:** none.

## DD-110 — Direct WinMM sends cost one interlocked operation each (none inside a burst): the slowdown seen on Windows XP is gone (amends DD-094 part 1)

- **Category:** performance fix (no musical change)
- **What changed:**
  - The Windows XP test PC (Pentium 4 3.8 GHz) measured direct sends at 182 ns each with the prompt 008 package and 410 to 442 ns with the prompt 009 package. Cause (code facts): DD-094 part 1 (commit c96ba22, merged in ea78f48) bracketed every `send_short` with two interlocked operations (`lock xadd` on entry and on exit of the call gate). The send path was otherwise unchanged; the prompt 008 package has no gate. Interlocked operations are cheap on a current processor (about 8 to 16 ns each) but a Pentium 4 pays well over 100 ns for each, because it must finish all earlier stores (the driver's own writes) before the instruction completes.
  - v2 now: (1) `send_short` enters the gate with ONE exchange on a flag that only the sending thread owns (the exchange is also the barrier between "a send is inside the driver" and the look at the closed flag, so a close either sees the send or the send sees the close) and leaves with a plain store. (2) A burst of sends takes the gate once: `MidiOutput::begin_send_batch()` / `end_send_batch()` (default: nothing). The engine brackets each immediate burst (at most 2048 sends or 8 ms); the check tool brackets 256 sends. Inside a batch a send is a plain look at the closed flag, then the driver call: zero interlocked operations per message. (3) Every other public call (reset, panic, SysEx) keeps the counted form (two interlocked operations; not per message). (4) A second thread that arrives while the flag is taken gets the counted form, so it can never clear the first thread's flag. (5) The deferred-close hand-off is unchanged in behaviour: if a close gave up waiting for a call inside the driver, the thread that leaves that call finishes the close; the closer also looks again for 100 microseconds, to cover the few nanoseconds in which a thread that leaves with a plain store may not yet have seen the close request.
  - DD-094's guarantees are kept: no MODM_RESET, MODM_CLOSE or MODM_OPEN underneath a send; a bounded wait (2 s); after a close is asked for nothing more is sent (inside a batch the next send is refused, the batch ends, the close completes); the abandoned-driver rule is untouched. In DD-094, "two interlocked operations per call" now reads "one per message, none inside a burst (DD-110)".
- **Why:** the safeguard must not cost the speed that is the point of direct access, least of all on the old PCs the XP build is for.
- **User-visible effect:** none except speed. Expected on the XP PC (estimate from the lock counts, to be confirmed by the XP package): a single send about 300 ns, a send inside an engine burst about the prompt 008 figure (about 185 ns), against 410 to 442 ns with the prompt 009 package.
- **Evidence (a Windows 10 22H2 PC, Xeon W-11855M, `midibn-direct-check --fake-driver`: the real `WinMmDirectOutput::send_short` path into an in-process no-op driver; ns per send, medians of 2 interleaved runs of 3 to 7 rounds, 0.5 s per round):**

  | Code | x64 | XP IA32 build |
  |---|---|---|
  | prompt 008 (before DD-094), c96ba22^ | 17.8 / 16.9 | 18.9 / 20.0 |
  | prompt 009, 7c95315 (two interlocked operations) | 48.5 / 51.5 | 57.1 / 62.8 |
  | now, one send at a time (one exchange) | 23.1 / 26.4 | 32.7 / 30.7 |
  | now, 256 sends per batch | 15.8 / 16.5 | 19.9 / 21.2 |
  | bare call of the no-op function (reference) | 3.8 to 4.3 | 4.8 to 5.7 |

  Locked operations on the per-message path (compiler listings, x86 and x64): prompt 008: 0; prompt 009: 2; now: 1 (one `xchg`) one at a time, and 1 per burst in a batch. Tests: `tests/direct_output_tests.cpp` `test_cheap_gate_counts`, `test_cheap_gate_close_race` (300 open/send/close cycles with a sender thread in a tight loop and a close from another thread at a random moment, half with a zero wait so the close is left to the sender, a quarter with batches: never a reset, close or open under a send, every close finished exactly once), and the earlier `test_close_while_driver_full` unchanged; 182-183 checks pass in x64, x86 and XP IA32. Engine tests (387 checks) and the engine harness (3,956 runs identical to before) pass. Real XP figures: pending the next XP package run.
- **Confirmed on real hardware (prompt 010 XP package, 2026-10-18; Windows XP SP3, Pentium 4 3.8 GHz, real OmniMIDI):** standard WinMM 0.600 million messages per second, direct access 6.437 million per second (155 ns per send), 10.7 times faster; better than prompt 008's 5.5 million (182 ns) and prompt 009's 2.26 million. DD-110 is confirmed on real hardware. With a no-op test driver on XP: bare call 23.3 ns; one send at a time 117.7 ns (output wrapper 94.4 ns of it); sends in 256-message bursts 79.7 ns (wrapper 56.4 ns). The single-send wrapper cost is a candidate for a later optimisation scan. See `DEVICE_MEASUREMENTS.md`, "Direct WinMM on Windows XP".
- **Risk:** low to medium. The ordering argument is the classic one (exchange = full barrier on x86, a close sets its flag with an interlocked operation and then waits for the send flag to clear); it holds on x86 and x64 only, the only targets. The 100-microsecond look-again after a deferred close is a time bound, not a proof. Contract unchanged: one sending thread at a time.
- **Harness rules:** none (the direct path is off in every recorded scenario; the batch calls are no-ops for every other output).

## DD-111 — Direct WinMM keeps the device open across Stop (user decision 5, prompt 009)

- **Category:** improvement (decided by the user)
- **What changed:** before, Stop reset and silenced the device (MODM_RESET, then the 48 all-sound-off / all-notes-off / sustain-off messages) and the player kept the output object, but the device was closed and opened again by the NEXT Play, because `WinMmDirectOutput::open` always began with a full close (reset, MODM_CLOSE) and ended with MODM_OPEN. That close/reopen on every Play is the step OmniMIDI sometimes never returned from after a heavy direct flood. Now `open()` for the device that is already open and healthy (not closing, not abandoned) does not touch the driver: it clears the unfinished-SysEx state, returns the SysEx buffers and succeeds. The device closes only when (a) a different device is opened, (b) the path changes (the player closes the direct output when the output, KDMAPI or Direct WinMM setting changes, also while stopped; before, a change while stopped closed nothing until the next Play), (c) the program exits, (d) DD-094 gives the driver up. Those closes keep all DD-094 protections. Standard WinMM and KDMAPI are unchanged: their `open()` still closes and reopens on every Play.
- **Why:** fewer driver open/close cycles mean fewer chances to meet the driver's rare hang, and a faster Play.
- **User-visible effect:** the device stays claimed by this program between Stop and the next Play (another program cannot use a one-user synthesizer meanwhile; it is released on a device or path change and at exit). A synthesizer's own state (programs, volumes set by the song) is no longer rebuilt by a reopen: Stop's reset and all-notes-off still run, but the next Play starts from whatever channel state the previous song left. Queued question for the user: should Play send "reset all controllers" (CC121) on every channel first? Recommendation: no (v1 and the standard path do not either), unless real-driver runs show a difference.
- **Evidence:** `test_keep_open_across_stop` (six Play/Stop cycles: one MODM_OPEN, no MODM_CLOSE, six MODM_RESET; a different device closes the first and opens the second; `close()` closes; a send stuck inside the driver: `open()` of the same device neither closes nor reopens, and a close during that send is bounded and finished by the sender); `app_tests` `test_direct_winmm` (Stop and the next Play do not close the direct output; a device change while stopped closes it). **Real drivers (this PC, Windows 10, busy-ish, overnight power plan "subthrottle"; silent; DEVICE_MEASUREMENTS.md, "Keeping the device open"):** 200 open/flood/Stop cycles on one output: OmniMIDI Stop mode 199 opens answered by the open device, 0 times the driver stopped answering, Stop max 2.6 ms, final close 4.3 ms; panic mode 199/0/11.1 ms/1.8 ms; VirtualMIDISynth Stop 199/0/12.0 ms/12.1 ms, panic 199/0/32.5 ms/20.5 ms (prompt 009 closing every cycle: OmniMIDI's close hung after 2-198 cycles). Player Short scenario (Play/Stop/Play/Stop) in Direct mode PASS on OmniMIDI and VirtualMIDISynth.
- **User decision 2026-10-18 (prompt 011):** no extra reset with the device kept open across Stop (no CC121 at Play) is accepted.
- **Risk:** low to medium. A driver that got into a bad state during a song keeps it until a device change or restart (before, the reopen reset it); mitigated by the abandon rule.
- **Harness rules:** none.

## DD-112 — Few-processor options (test switches, and one menu item on a one-core PC; off by default)

- **Category:** performance policy, judgment call to review (queued: few-processor yield option)
- **What changed:** test switches `--affinity HEX` and `--few-cpu normal|yield=MS|nap=MS` (combinable, e.g. `normal,yield=2`). With at most 2 usable logical processors (process affinity counts): `normal` runs v2's own threads (playback worker, display pacer) at Normal instead of Above Normal; `yield=MS` makes a playback worker that has sent for MS ms without waiting (it is behind) call SwitchToThread (`nap=MS`: Sleep(1) instead). Hook: `EngineCore::yield_if_behind` (one test of a member per send when off), `EngineServices::behind_yield_interval_microseconds / yield_to_others`, `win32::set_few_cpu_policy`. Nothing is on by default. The display pacer never runs above the playback worker (both Above Normal today; Normal with the option); the window thread is Normal; the adaptive display rate (DD-109) lowers the pacer's rate, never raises its priority.
- **Why:** when the worker is permanently behind (a synthesizer that cannot keep up) it keeps a one-processor PC busy at a priority above the window thread and everything else.
- **User-visible effect when ON and behind on 1 processor:** the window and other programs stay responsive; the song falls behind a little more (10 % with yield=2). Nothing changes when playback is not behind. Musical content and order are untouched.
- **Evidence:** DEVICE_MEASUREMENTS.md, "One or two logical processors" (a stand-in audio thread, not OmniMIDI itself): on ONE processor with the worker behind, today's worker starves Normal threads for about 5 seconds at a time; `yield 2 ms` cuts the longest delay from about 5 s to about 110 ms for 10-15 % more lag; Normal priority also fixes it but costs 20-40 %; nap is worse than yield. On TWO logical processors nothing starved and every option only costs throughput (2-30 %). Recommendation: yield only, only with exactly one usable processor (the code applies to up to 2 and would need the limit lowered). Not measured: a real Hyper-Threaded Pentium 4, OmniMIDI's real thread.
- **User decision 2026-10-18 (prompt 011):** a few-processor option is wanted, as an option and off by default (the exact form is still to be decided).
- **User decision 2026-10-22 (prompt 011 question Q2): replaced 2026-10-25.** The option is offered only on a PC with exactly one physical core. It is on by default only when exactly one logical processor is usable by the process (process affinity read at start); a Hyper-Threaded single core (1 physical, 2 logical) stays off by default until the XP measurement. The user's choice is remembered and wins over the automatic default (key `few_cpu_yield`: `auto`, 0 or 1; DD-153).
- **Updated 2026-10-18 (prompt 011): the option is implemented as a menu item.** The system menu of the main window gets one tick item, **"Let other programs run while playback is behind"**, in the group of "Direct WinMM driver access (ModMessage)" (after it, before the separator that starts the queue-limit group). It is **off by default** (see the replaced decision above: on by default only with exactly one usable logical processor) and **remembered** (DD-026, DD-153). It is **present only on a PC with exactly one physical processor core**: a Hyper-Threaded single-core Pentium 4 (Windows sees 2 logical processors) counts as one. Physical cores come from `GetLogicalProcessorInformation` (relation "processor core"), looked up with `GetProcAddress` so Windows versions without it still start; if it is unavailable the logical processor count decides (exactly 1). When ticked, a playback worker that has sent MIDI for 2 ms without waiting (it is behind) calls `SwitchToThread` (the "yield 2" setting above); the tick can be changed at any time, also while playing, and takes effect at the next send. Nothing changes on PCs with two or more physical cores. On a one-core PC ticked means the PC stays responsive while playback is behind and the song falls behind a little more. Test switch `--force-one-core` makes the program act as a one-core PC so the item can be tested on a many-core PC (it does not restrict the processors, so the effect is not measured with it); `--affinity` and `--few-cpu` work as before (the menu item overrides the yield interval of `--few-cpu yield=`). Code: `win32::count_physical_cores`, `few_cpu_yield_option_offered`, `physical_core_count`, `few_cpu_yield_option_available` (`platform/win32/win32_timing.*`); `EngineCore::set_behind_yield_microseconds` (the interval is now an atomic value that can change while running); `MainWindow` command `CmdFewCpuYield` (0x20A0). Evidence: `tests/platform_tests.cpp` (core counting from a fake processor-information array; the offer decision for 1/1, 1/2, 2/2, 6/12 and unknown physical counts; the test switch); the in-process self-test with `--force-one-core` (the item is present exactly when offered, starts off, ticks on and off). Not measured: a real Hyper-Threaded Pentium 4. The GTK player offers the same item under the same rule (`/proc/cpuinfo` shows one physical core, or `--force-one-core`).
- **Risk:** low (off by default; absent on most PCs).
- **Harness rules:** none.

- **Updated (prompt 012):** the menu item sits after "Output-paced timeline" (the "Direct WinMM driver access (ModMessage)" item before it was replaced by a checkbox, DD-127). The few-processor measurement is now also available as a package tool for the real Pentium 4 (DD-132).

## DD-113 — Thread report tool and the player's `--thread-report` option (diagnostics; no behaviour change)

- **Category:** diagnostics
- **What changed:** new read-only tool `midibn-thread-report` (XP name `v2-xp-thread-report[-ia32].exe`): lists every thread of its own process (priority, module of the start address, processor time), opens one MIDI output through standard WinMM, KDMAPI or direct driver access (the player's own output classes), sends only controllers 121 and 123 on the 16 channels, waits 2 s, lists again (new threads marked NEW), closes, lists once more. The player gets the test option `--thread-report FILE [--thread-report-delay-ms N]` (per-thread table N ms after playback starts, any output). The XP self-test package runs the tool for each path on the first non-Microsoft output (`thread-report-winmm/kdmapi/direct.txt`).
- **Why:** on a Pentium 4 with Hyper-Threading under XP a thread in the player's process ran at Time Critical while OmniMIDI stuttered; the report shows which module starts such a thread.
- **User-visible effect:** none (test option and tools only).
- **Evidence:** built x64 and XP-IA32; `--path none` runs; player report on a hidden desktop. **Real drivers (this PC, Windows 10; DEVICE_MEASUREMENTS.md, "Threads in the player's process"):** OmniMIDI creates in our process on every path (standard, KDMAPI, direct) its OmniMIDI.dll worker (Normal at open, raised to Time Critical while playing), two BASS.dll threads at Time Critical, one BASS.dll Above Normal, a BASSWASAPI thread at priority 13 and two BASSMIDI threads Normal. VirtualMIDISynth: one in-process thread at Lowest (its synthesizer runs in its own service process). v2's own threads, the playback worker ("MIDI bottleneck scheduler") and the display pacer, are both Above Normal; none is Time Critical. Conclusion: the Time Critical threads on the Windows XP test PC are OmniMIDI's (its audio library); Direct mode adds no thread, its extra load is only the higher send rate.
- **Real XP result (prompt 010 package, 2026-10-18):** Windows' own `wdmaud.drv` has a Time Critical thread before anything opens. With OmniMIDI open (standard and direct paths alike) the Time Critical threads are DirectSound x2, BASS x1 and OmniMIDI x2; BASS also has one at Highest; OmniMIDI leaves one ordinary thread alive after close. On the same PC the KDMAPI backend failed to open: "No KDMAPI provider was found.", because OmniMIDI sits in `C:\WINDOWS\system32\OmniMIDI\OmniMIDI.dll`, a subfolder the KDMAPI search (application folder, then System32) does not look in (v1 most likely has the same gap); fix in progress (prompt 011).
- **Risk:** none for playback. The tool never sends a note.
- **Harness rules:** none.

## DD-114 — Large-address-aware 32-bit builds: probe tool and generated large-load fixtures (tooling; no behaviour change)

- **Category:** tooling
- **What changed:** new tool `midibn-large-address-probe` (`tools/large_address_probe_main.cpp`): reserves address space until a new allocation can only land above 2 GB (prints the pointer values), routes every `operator new` of the program to a private heap created afterwards (so the output classes' MIDIHDR and SysEx copy are above 2 GB, counted), then opens an output (`none`, `fake-direct`, or on request `standard`, `kdmapi`, `direct`), sends silent messages (controllers 121 and 123 on channel 1, then SysEx GM System On `F0 7E 7F 09 01 F7` from a buffer above 2 GB), reset, panic, close, and checks every return code. New script `tools/make-large-load-fixture.ps1`: a generated MIDI file of 104 million silent events (controllers 20 to 31, 298 MB) whose load needs more than 2 GiB in a 32-bit program (not a user file); `-From <large-8m.mid>` repeats the 8M test file instead. The direct check gains `--fake-driver` (DD-110) and the XP self-test package runs it as an information line after the real check. Finding: the player opens the output only at Play (or when the output setting changes during playback); loading a file never opens a device.
- **Why:** to prove that the 32-bit builds (up to 4 GiB of addresses on 64-bit Windows, DD-060) work with a driver when program memory, including the buffers handed to the driver, lies above 2 GB.
- **User-visible effect:** none.
- **Evidence:** no device, x86 (v143) and XP IA32: paths none and fake-direct PASS (first allocation after filling the low 2.2 GB at 0x8FFF0000, SysEx source buffer at 0x8FFF0000, 17 and 45 program memory requests above 2 GB, 0 below); x64 also passes. The 32-bit `midibn-perf` loads the 104 million silent events with a peak of 3.36 GB (3.13 GiB). **Real drivers (this PC, Windows 10, x86 build, silent; DEVICE_MEASUREMENTS.md, "Large addresses"):** OmniMIDI standard WinMM, VirtualMIDISynth standard WinMM, OmniMIDI direct, VirtualMIDISynth direct and OmniMIDI KDMAPI all PASS, 46-245 program allocations above 2 GB while the output was in use, 0 below. The x86 player with a generated silent 104,000,000-event file (memory warning "3.10 GiB", Continue): the Short play/stop scenario PASS on OmniMIDI standard WinMM, KDMAPI and Direct (21 checks each).
- **Risk:** none (tools only).
- **Harness rules:** none.

## DD-115 — Measurement tools use adaptive round counts

- **Category:** tooling
- **What changed:** the seven measurement tools run 3 rounds and add one round at a time (to at most 7) only while a result is unclear (two compared medians closer than the larger of 3 % or either set's spread; a single set whose spread is over 10 % of its median). An explicit count on the command line stays exact. Each tool prints and records how many rounds it used and why. One shared rule: `v2/tools/adaptive-repeats.ps1` (self-check: `adaptive-repeats-selfcheck.ps1`). `run-device-measurements.ps1`: without `-Repeats`, a measurement of 3 plays gets extra one-play processes (label suffix `-x4`..`-x7`) while the plays' Meps spread over 10 %; each extra play is a fresh process, so it includes process start-up conditions the first three plays (one process) do not.
- **Why:** fewer long runs when the answer is clear, more rounds only where noise hides the answer.
- **User-visible effect:** none (tools).
- **Evidence:** self-check on made-up numbers PASS; `ia32-vs-sse2.ps1 -DryRun` extended to 7 rounds on fabricated 4 % differences with 6 % noise and said why. Real-tool runs: not done (long runs); the device-measurement extension is untested on real drivers.
- **User decision 2026-10-18 (prompt 011):** the no-SSE2 against SSE2 comparison often running 7 rounds is accepted.
- **Risk:** none for the player.
- **Harness rules:** none.

## DD-116 — Release names are given when packaging, not by the build

- **Category:** tooling
- **What changed:** "MIDIBottleneck Player x64.exe / x86.exe / XP.exe" and "MIDIBottleneck Player XP SSE2 test.exe" are produced by `package-release.ps1` and `refresh-test-kit.ps1`; the build output stays `midibn-win32.exe`.
- **Why:** nothing in the program depends on its file name, but about 30 test scripts look for `midibn-win32.exe`; renaming only at packaging is the cheaper and safer choice.
- **User-visible effect:** none for the program; the packaged files carry the final names.
- **Evidence:** the scripts (they are used by the release procedure).
- **Risk:** none.
- **Harness rules:** none.

## DD-117 — The XP IA32 release build uses whole-program optimization (/GL and /LTCG)

- **Category:** performance (XP release build only; commit 0de3190)
- **What changed:** the XP IA32 release build is compiled with `/GL` and linked with `/LTCG`; no other build changes. Measured in a cost study (P10-COST) that built the same sources four times (x64 and XP IA32, each normal and with /GL /LTCG): on x64 no gain (engine 31.05 against 31.55 million events/s, inside noise; load 1,244 against 1,242 ms; size +0.5 %), so x64 is unchanged. On XP IA32 loading the 8-million-event file went from 1,796 to 1,688 ms (about 6 %) and the engine from 182 to 170 ns per event (about 6 %), with the same message hash. Two proposals of the same study were NOT implemented: (a) a producer/consumer playback worker (cost 40 to 60 weekly points, no gain below about 20 million events/s, changes what "lag" and "backlog" mean) and (b) per-track parallel loading (12 to 18 points, about 1.5 times faster on big multi-track files; deferred, it did not fit this run's budget; a parallel merge, 40 to 64 points, is not recommended).
- **Why:** slow machines are where speed matters most, and the change is one line.
- **User-visible effect:** none except about 6 % faster loading in the XP player.
- **User decision 2026-10-18 (prompt 011):** parallel loading (per-track, mentioned in the cost study) stays deferred.
- **Evidence:** `v2/docs/XP_COST_LOG.md`; interleaved rounds on a loaded PC (70-87 % busy), only the `midibn-perf` tool measured, not the player. Parity and tests are to be confirmed on the adopted build.
- **Risk:** low.
- **Harness rules:** none.

## DD-118 — The 32-bit large-file warning threshold is half of the usable address space

- **Category:** performance/usability policy (refines DD-024 and DD-105; user decision)
- **What changed:** in a **32-bit** build the large-MIDI memory warning (and the earlier "inspect this file first" size check, which comes from the same limit) now uses **half of the address space the program can really use**. The size is read from the system at start (`GetSystemInfo`, highest usable application address, rounded to the nearest 64 MiB so the small guard area at the top does not show). It is not a Windows version check. Result: **2 GiB** for the 32-bit player on 64-bit Windows, **1 GiB** on 32-bit Windows and on Windows XP, **1.5 GiB** when Windows is started with /3GB. Before: 1 GiB everywhere (v1's value). **64-bit builds are unchanged** (4 GiB). The extra sentence about the 32-bit limit in the warning follows the same figure. If the system gives no address (cannot happen in practice) the old 1 GiB is used.
- **Why:** a 32-bit program on 64-bit Windows can use 4 GiB, so warning at 1 GiB came too early; half leaves room for the loader's peak and the rest of the program.
- **User-visible effect:** on 64-bit Windows the 32-bit player warns for fewer files: files whose estimate lies between 1 and 2 GiB now load without the question. The wording of the warning is unchanged. Nothing changes on 32-bit Windows or in the 64-bit player.
- **User decision 2026-10-18 (prompt 011):** approved.
- **Code:** `half_address_space_warning_threshold`; `preflight_estimate`, `preflight_required` and `LoadFlowOptions::warning_threshold` take an optional threshold (0 = built-in value, so the harness traces and tools are unchanged); `PlayerEnvironment::memory_warning_bytes`; `MainWindow::create`.
- **Evidence:** `tests/unit_tests.cpp` (0x7FFEFFFF gives 1 GiB, 0xBFFEFFFF gives 1.5 GiB, 0xFFFEFFFF gives 2 GiB, a 64-bit user space, an unknown address, the built-in 32-bit and 64-bit values unchanged, the inspection size limit follows the threshold); x64 and x86 builds, `midibn-tests`, `midibn-app-tests`.
- **Risk:** low. The estimate itself is unchanged.
- **Harness rules:** none (the traces use the built-in value).

## DD-119 — The KDMAPI provider search also looks where the installed driver is registered (bug fix)

- **Category:** bug fix, found on the user's real Windows XP PC (no change to what is played)
- **What changed:** on Windows XP with OmniMIDI installed at `C:\WINDOWS\system32\OmniMIDI\OmniMIDI.dll`, v2 said "No KDMAPI provider was found." because it looked only in the application folder and then in the system folder itself. The search order is now: (1) the application folder (unchanged, still wins); (2) the folder of each user-mode MIDI driver registered under `Drivers32` (value names `midi`, `midi1`, ...; for example `OmniMIDI\OmniMapper.dll` gives `System32\OmniMIDI`), in registry order, using the same code as the Direct mode (the registry view of this program, SysWOW64 and the System32 spelling handled for the 32-bit program on 64-bit Windows); (3) the system folder (unchanged fallback). The first file found is used; as before its PE header must match the program's architecture (x86 or x64), or the player names the file and says so. The wording of "No KDMAPI provider was found..." is unchanged.
- **Why:** the OmniMIDI installer registers its driver in a sub-folder of the system folder; the old search could never find it there.
- **User-visible effect:** KDMAPI works with OmniMIDI installed the way the XP PC has it, without copying `OmniMIDI.dll` next to the program.
- **Evidence:** `tests/midi_output_tests.cpp` (`test_provider_files`: the registered-folder list from fake `Drivers32` entries, with wdmaud, mapper and outside-system-folder entries left out, the System32 spelling turned into the SysWOW64 folder, duplicates and the system folder itself dropped; the order application, registered, system, not found; temporary folders and dummy files, nothing is loaded). On the Windows 10 PC `midibn-kdmapi-check` x64 and XP IA32 found the provider in System32 with the right architecture. New read-only tool `midibn-kdmapi-check` (XP name `v2-xp-kdmapi-check-ia32`): lists the searched places, the one used and the architecture check, and never loads the provider. It is step 6 (last, information only, not part of OVERALL) of both XP self-test launchers; the XP package zip was **not** rebuilt yet.
- **Real XP confirmation (recorded 2026-10-24):** the XP PC's `kdmapi-check-result.txt` finds the provider at search position 2, in the registered driver folder, with the right architecture (`XP_COST_LOG.md`, `DEVICE_MEASUREMENTS.md`).
- **User decision 2026-10-22 (prompt 011 question Q3, recorded in prompt 012):** no fixed `system32\OmniMIDI` fallback is needed; the XP check found the driver through the registered driver folder.
- **Risk:** low. It only adds candidates between two existing ones.
- **Harness rules:** none.

## DD-120 — F1 works inside the Analysis, Channels and Track routing windows

- **Category:** small improvement
- **What changed:** v1 handled F1 only in the main window (v1's own Help says "Press F1 while a main-window control is focused"); in the Analysis, Channels and Track routing windows F1 did nothing. In v2, F1 in Analysis opens the Help topic Analysis, in Channels the topic Channels, and in Track routing the topic File and output (where Track routing is described). Help and About themselves are unchanged.
- **Why:** F1 should help where you are.
- **User-visible effect:** F1 in the three companion windows opens Help at the right topic.
- **Evidence:** `v2/out/prompt-011/p11-pictures/help/help-f1-inside-analysis-100pct.png`; visual-gate state `dlg-help-f1-analysis`. Channels and Track routing use the same code path but were not captured separately. On Linux, F1 works in Analysis and Channels; Track routing there is modal and cannot open Help (see UI_DIFFERENCES.md, "Linux").
- **Risk:** none.
- **Harness rules:** none.

## DD-121 — The portable core builds and passes every test on Linux (GCC and Clang); one hidden overflow made explicit

- **Category:** internal quality (Milestone 8, stage 2); no change on Windows
- **What changed:** the portable core, the decimal oracle, the virtual platform, the engine harness and every test program and tool that needs no Win32 build and pass on Linux with GCC 11.4 and Clang 14 (Release, `-Wall -Wextra`). The parity program (same source as on Windows) gives the same result against the same committed data: the same engine-run count, and all traces, flows, models and v2 baseline items identical. Changes needed:
  - `per_note_gate.cpp`: the lowest-set-bit helper uses `_BitScanForward` on MSVC (same code as before), `__builtin_ctz` on GCC and Clang.
  - `workload_analysis.cpp` (`default_resolution`, `calculate_bucket_count`): `duration + 4999` and `duration / bucket + 1` overflow for durations near Int64.Max. MSVC wraps (as v1's unchecked C# does); GCC and Clang treat signed overflow as impossible and gave different answers. The additions are now done in `uint64_t` and converted back, so every compiler gives the wrapped value MSVC always gave. **No Windows result changes.** Found by the v2 baseline (model trace `analysis`) and by UBSan.
  - small portability fixes: missing includes (`<cstddef>`, `<cmath>`), `static constexpr` for class constants that Clang/GCC could not link when passed by reference (same values), POSIX file helpers and `main` for the tools (`MIDIBN_TOOL_MAIN`), forward slashes in a test's paths.
  - CMake: every Win32-only target is behind `if(WIN32)`.
- **Why:** the Linux player needs the same core, proven by the same tests.
- **User-visible effect:** none on Windows.
- **Evidence:** all test programs and `midibn-parity` (tier 1 and tier 2, all 3,956 engine runs) pass on Linux with both compilers; the Windows builds and their tests are unchanged.
- **Risk:** low. The explicit unsigned arithmetic reproduces what MSVC did.
- **Harness rules:** none.

## DD-122 — A Linux platform layer (clock, waiter, threads, files, memory) replaces the stand-in used for the first Linux test runs

- **Category:** architecture (Milestone 8, stage 3); Linux only
- **What changed:** new `v2/platform/linux/` implements the core platform interfaces over POSIX. It replaces the stage-2 stand-in `platform/portable` (deleted; nothing on Windows used it). See ARCHITECTURE.md §4. Points a user or tester can notice:
  - **Timing:** `CLOCK_MONOTONIC` in nanoseconds; the waiter sleeps on the wake event until 200 microseconds before the target and spins the rest (the same split as Windows), and timer slack is set to 1 ns on every player thread.
  - **Priority:** "above normal" is a nice value of -3, if the system allows it. Without that right (a normal user, soft limit 0) the request is refused and the thread silently stays at normal; playback still works. No real-time scheduling class is used. A user who wants the raise can allow it with `ulimit -e` or `/etc/security/limits.conf` (`@audio - nice -5`).
  - **Memory figures:** "peak working set" is `VmHWM`. Linux has no per-process peak of private memory, so "peak private" is `VmPeak` (peak virtual size), an **upper bound** that also counts thread stacks and mapped libraries (about 145 MB for a small process); read it as "never more than".
  - File paths are UTF-8 as they are; a path that does not exist as written and contains a backslash is tried once more with slashes (only the test suite builds Windows-style paths).
- **Why:** the Windows layer cannot run on Linux, and the stand-in was not accurate enough for timing.
- **User-visible effect:** none on Windows.
- **Evidence:** `midibn-linux-platform-tests` (150 checks); `midibn-linux-capability` report. Waiter accuracy: DEVICE_MEASUREMENTS.md, "Linux waiter accuracy and thread priority (WSL2)".
- **Risk:** low for Windows (untouched). On real Linux hardware the figures are still to be confirmed by a tester.
- **Harness rules:** none.

## DD-123 — Linux plays through the ALSA sequencer (direct delivery, one write per burst)

- **Category:** new output path, Linux only (Milestone 8, stage 4)
- **What changed:** new MIDI output `alsa_midi_output` (client "MIDIBottleneck Player", one port "Output", connected to the chosen destination). The output list shows the writable, subscribable ports of other programs and devices (hardware, software synthesizers, "Midi Through"), not the system's timer and announce ports. Messages are delivered directly, with no queue and no timestamps: the engine's own clock decides when to send. Short messages map to the matching ALSA events (note on/off, key pressure, controller, program change, channel pressure, pitch bend, the system real-time and common messages); bad bytes are refused with "This MIDI message cannot be sent through the ALSA sequencer." SysEx is assembled as on Windows and sent in chunks of at most 512 bytes, each delivered before the next, so the order is exact. Reset forgets an unfinished SysEx and drops what is buffered; panic uses the same 48-message order as Windows. Errors are plain sentences (no `/dev/snd/seq`, not allowed (suggests the audio group), busy, port vanished). A burst of due events is written once (`SendBatch`, the default send pattern `Batched`) instead of once per message; `Direct` is the conservative fallback (one line).
- **Why:** it is the standard way for Linux programs to reach hardware and software synthesizers.
- **User-visible effect:** on Linux only: ALSA ports in the output list.
- **Evidence:** `midibn-alsa-output-tests` (153 checks on a fake sequencer; a client-to-client loopback through the real library when `/dev/snd/seq` exists, otherwise "SKIPPED"). The development PC has no `/dev/snd/seq`, so the cost per message of the three send patterns could not be measured; the default follows from the system-call count and must be confirmed on a real Linux machine (`midibn-alsa-send-check`, commands in BUILDING.md).
- **Risk:** low (new path, Linux only); the send-pattern choice is unconfirmed on real hardware.
- **Harness rules:** none.

- **Updated (prompt 012):** the real-sequencer loopback test failed on a tester's PC in its last check only; the test now reports what arrived and judges only the player's own events (DD-133). The cause is not yet confirmed.

## DD-124 — The Linux player (GTK 3) and two small shared-code changes

- **Category:** new platform frontend (Milestone 8, stage 5)
- **What changed:** new frontend `v2/frontends/gtk/`, program `midibn-gtk` ("MIDIBottleneck Player"), built on Linux only when GTK 3 development files are found (dynamic link, nothing shipped). It has the main window with every control (Open and drag-and-drop, outputs, Play/Pause/Stop, timeline, rate model and its settings, queue limit and Overflow, statistics with queue-pressure bar and tooltips, effective-speed menu), Help, About, Analysis with its graph, Channels and Track routing. Behaviour comes from the same portable app layer as Windows; the GTK files only present it. What differs from Windows, and why, is in UI_DIFFERENCES.md, section "Linux (GTK) interface". Shared-code changes:
  - **Statistics hover texts** moved from `frontends/win32/controls.cpp` to the portable layer (`core/include/midibn/app/statistics_tips.h`, `core/src/app/statistics_tips.cpp`, in the `midibn_core` source list); same words, now used by both frontends.
  - **Playback-thread message:** `messages::playback_thread_not_started()` says "The system could not start the playback thread ..." on non-Windows builds; the Windows text is unchanged byte for byte (`#ifdef _WIN32`).
  - Display pacing uses the GTK frame clock while live updates are needed; the update-rate tiers (`display_tier.h`) are the same.
- **Why:** to offer v2 on Linux with the same features.
- **User-visible effect:** Linux only; Windows pictures and texts are unchanged.
- **Evidence:** scripted checks under a virtual X server only (`tools/linux-check.sh BUILD quick gtk`: open a file, play on output None, pause, seek, rate model change, scale 150/75/100 %, Help topics, Analysis and Channels open, close all, no GLib warnings; Track routing and the large-file warning under `--strict`). Pictures: `v2/out/prompt-011/p11-pictures/linux/`. Windows x64: `midibn-app-tests` 113 checks and `midibn-ui-tests` 168,933 checks, 0 failures.
- **Not tested by script:** mouse editing in the Channels grid, scrub dragging, drag-and-drop, the file chooser, graph pointer input (no input tool). A person on a real desktop has to look at these.
- **Risk:** low for Windows; the Linux interface is new.
- **Harness rules:** none.

## DD-125 — A Linux self-test package for a tester

- **Category:** tooling (Milestone 8, stage 6); no effect on the player
- **What changed:** `v2/tools/package-linux-selftest.sh` builds Release with the new CMake option `MIDIBN_PORTABLE_LINUX` (static libstdc++ and libgcc; no effect on Windows), strips, scans and packs `MIDIBottleneck-linux-selftest.tar.gz` with a checksum: the test programs, two launchers (quick, 1 to 3 minutes; full, 8 to 20 minutes), the data they need and a README. The GTK player is included when the build makes it. The script fails if any `GLIBCXX_` or `CXXABI_` symbol remains. Highest `GLIBC_` symbol: **2.34**, so it runs on Ubuntu 22.04+, Linux Mint 21+, Debian 12+, Fedora 35+, RHEL/Alma/Rocky 9+; not on Ubuntu 20.04, Debian 11 or RHEL 8 (it would have to be built in such an environment). x86-64 only. About 17 MB packed.
- **Why:** to let a tester, on a real Linux machine, run the same checks and send back the result files.
- **User-visible effect:** none for the player.
- **Evidence:** fresh extract, both launchers, as root and as an unprivileged user in WSL Ubuntu 22.04: OVERALL PASS every time (quick 40 to 58 s, full about 8 min). Not tested: the send check with a real port, a second distribution.
- **Risk:** none.
- **Harness rules:** none.

- **Updated (prompt 012):** the package now puts the player first, in its own folder, with a one-page README (DD-134).

## DD-126 — Help has a Linux wording

- **Category:** documentation inside the program (no musical change)
- **What changed:** `help_text` now has a platform choice (`HelpPlatform::Windows` or `Linux`; the one-argument form gives the wording of the platform the program runs on). The **Windows text is byte for byte the same as before** (checked by a before/after dump of all eight topics). The Linux wording: the Output section names ALSA sequencer ports (hardware, software synthesizers, Midi Through) instead of WinMM and KDMAPI; the "title-bar menu" is the menu button (the three-bar sign); the Overview has no borderless styles, Alt+Space or Windows XP remarks; the Direct WinMM driver access section is left out, as are the places that refer to it. The other topics are shared; their remaining differences are only the menu name.
- **Why:** the first Linux build showed Windows-only advice (open question of the GTK stage).
- **User-visible effect:** Linux only.
- **Evidence:** `midibn-ui-tests` checks both wordings on every build (titles, no Windows-only words in the Linux text, the topics that must stay identical, `help-linux` checks). `v2/out/prompt-011/p11-pictures/linux/help-100-linux-text.png`.
- **Risk:** none.
- **Harness rules:** none.

- **Updated (prompt 012):** the Windows text of the Help changed in two places because of DD-127 and DD-128 (the Direct WinMM section now describes the checkbox and its greying, with no trial wording and no "title-bar menu"; the Playback topic says where the state is shown); the Linux wording still has no Direct WinMM section.

## DD-127 — Direct WinMM is a checkbox next to KDMAPI; it is greyed out as soon as an output that cannot use it is selected

- **Category:** clearer interface; user decision (prompt 012): Direct WinMM is a normal feature, off by default, a normal feature. Amends DD-085, DD-088, DD-103, DD-112.
- **What changed:**
  - v2 before: a system-menu item "Direct WinMM driver access (ModMessage)" plus a status line (caption row, or the Playback heading line in the Integrated header) that came and went and moved the layout.
  - v2 now: a **Direct WinMM** checkbox to the left of the KDMAPI checkbox on the output row, in every header style (native frame, Classic, Integrated header, Auto-hide overlay), both layouts and every scale. Row two reads: `MIDI output:` list, **Direct WinMM**, **KDMAPI**; while a file loads the progress bars keep the right end of the row as before. Tab order: output list, Direct WinMM, KDMAPI. It is an ordinary auto-checkbox (same class and look as KDMAPI and "Queue length limit"; Space, mouse and screen reader work the same; drawn the same on XP; F1 opens the File and output topic). The menu item and the status line are gone. The sentences of the status line are the checkbox's tooltip, rewritten in short full sentences. Off by default; the tick is remembered (DD-026, DD-153).
  - **Greying (decided when the output is selected, not after the first Play).** The tick is kept in every greyed state.

| Selected output | Box | Decided |
|---|---|---|
| None | greyed, tick kept | at once |
| KDMAPI ticked | greyed, tick kept | at once |
| MIDI Mapper | greyed, tick kept | at once, no driver call |
| A device the check finds unusable (Windows' own driver, no driver found, ambiguous, a driver that stopped answering earlier, DD-094) | greyed, tick kept | by the check, at selection |
| A device whose check is still running | enabled | until the answer arrives |
| A device the check finds usable | enabled | by the check |
| A device the check allowed but the driver refuses when opened at Play | greyed after that Play (fallback, kept) | at Play |
| No direct output in this build or system (never on Windows; Linux) | greyed | - |

  A greyed box cannot be ticked or unticked; to turn Direct mode off for such an output, choose another output first. Choosing another output and coming back asks again only if a driver stopped answering earlier; otherwise the answer is kept for the session (the answer about a stalled driver is not remembered between sessions; the Direct WinMM tick itself is, DD-153).
  - **How the check runs.** The player already asked Windows for the number of MIDI devices at start, so Windows has loaded every MIDI driver in this process; the check borrows them and never opens a device or starts a synthesizer (one exception: a driver listed in the registry that Windows failed to load is started by the check and stopped again when it ends). It runs on a worker thread started by the controller (a driver call may block, DD-094), so the window never waits; the box stays enabled until the answer arrives. When the box is ticked and the check already says "not usable", Play does not try the direct path at all (no driver call): the notes go through Windows and the tooltip says why. Environment hook: `PlayerEnvironment::probe_direct(device id) -> {usable, reason}`; Linux has no direct output and is unchanged.
  - **Tooltip texts, word for word** (`<device>` = the output list entry; `<driver>` = the driver file name, for example `OmniMIDI.dll`; `<Reason>` = the reason of the check with its first letter capitalised, only if there is one):
    1. Off, a device selected (enabled): "Direct mode is off. Tick this box to send the notes straight to the synthesizer's driver instead of through Windows. On Windows 8 and later this is much faster, so big files are less likely to fall behind. It does not work with Windows' own synthesizer or the MIDI Mapper. This choice is remembered for the next time you start the program."
    2. On, not yet played with this device (enabled): "Direct mode is on. The program checks the selected device when you press Play. If the device cannot be used this way, the notes play through Windows as usual and this box tells you why."
    3. In use, tested driver (enabled): "Direct mode is in use. The notes go straight to the driver `<driver>`. This driver has been tested with this program. Untick this box to go back to the normal way through Windows."
    4. In use, other driver (enabled): "Direct mode is in use. The notes go straight to the driver `<driver>`. This driver has not been tested with this program, so listen for anything that sounds wrong. Untick this box to go back to the normal way through Windows."
    5. Found unusable when the output was selected (greyed, tick kept): "Direct mode cannot be used with `<device>`. `<Reason>`. This box is greyed out for this output. To use Direct mode, choose another output." and, when the box is ticked, it continues: " The notes play through Windows as usual. The tick stays and works again as soon as you choose another output." (For the MIDI Mapper the reason is "This is the MIDI Mapper, which Windows routes to another device".)
    6. The driver refused at Play (greyed, tick kept): "Direct mode is not in use for `<device>`. `<Reason>`. The notes play through Windows as usual, and this box is greyed out for this output. To try again, choose another output and then this one. To turn Direct mode off, choose another output first."
    7. Driver stopped answering (enabled, so it can be unticked): "Direct mode has stopped. The synthesizer's driver did not answer when it was asked to stop, so this program does not call it again. The next playback uses Windows as usual. Restart the program to use Direct mode with this driver again."
    8. KDMAPI ticked (greyed): "Direct mode is not in use. KDMAPI is ticked, and KDMAPI already sends the notes straight to the synthesizer. Untick KDMAPI if you want to use Direct WinMM instead." and, when the box is ticked: " The tick stays and works again as soon as that changes."
    9. Output None (greyed): "Direct mode is not in use. The output is None, which plays no sound. Choose a MIDI device in the list to use Direct WinMM." (+ the same tick sentence when ticked).
    10. No direct output on this system: "Direct mode is not available on this computer."
    The driver name appears in texts 3 and 4 because it identifies the synthesizer in use. The words "ModMessage", ".drv", the old trial label and "unverified driver" no longer appear anywhere in the program.
  - **Help.** The File and output topic describes the box, the tooltip and the greying and no longer has the trial label or "title-bar menu"; the Overview's menu list lost the old item.
- **Why:** the user decided (prompt 012) that Direct WinMM is a normal, default-off feature: a visible box next to KDMAPI instead of a hidden menu item and a status line that moved the layout. The greying is decided at selection because the check needs no device (the first plan, greying only after a first Play, was dropped for that reason).
- **User-visible effect:** as described. With the box unticked nothing else changes.
- **Evidence:** `app_tests` (greying on selection for a stand-in "Windows' own driver" device, a usable driver, the Mapper with no driver call counted, None, KDMAPI ticked; pending stays enabled; per-device answer), self-test (menu item gone; box text, state, greying, tick kept, tab order, position; for each of 23 style x scale x width cases "the box fits left of KDMAPI clear of the list"). Visual gate states `direct-{off,on}-{native,classic,integrated,overlay}-{100,75,50}-{std,compact}`, `direct-grey-none-*`, `direct-grey-kdmapi-*`, `direct-grey-own-*` (test-only switch `--direct-check usable|own-driver-late`, ignored in normal use), `direct-loading-*`. Pictures: `v2/out/prompt-012/P12-CHK/` and `.../P12-ERR/` (before and after sheets).
- **Risk:** low; presentation and a read-only check. The Linux window still has an always-empty path-status label that reads three unused fields of the player view; remove them with the label when the GTK window is next edited.
- **Harness rules:** none.

## DD-128 — The playback state word sits at the right end of the Playback heading

- **Category:** clearer interface (prompt 012). Amends DD-103.
- **What changed:** "Stopped / Paused / Playing / Completed" used to sit at the right end of the output row, where the loading panel's progress bars replaced it while a file loaded. It now sits at the **right end of the Playback group's heading line** in every view (native frame, Classic, Integrated, Overlay; standard and compact; every scale), and stays visible while loading. The label is as wide as the longest state word plus 8 canonical pixels, its right edge 8 px inside the group frame, and it never touches the heading. The freed space on the output row goes to the output list. File information in compact layouts is decided by the measured width of the file row, which this change does not touch; the fits-or-tooltip rule is unchanged.
- **Why:** the state should not disappear while a file loads, and the output row needed room for the new checkbox.
- **User-visible effect:** as described (Linux: the state stays next to the output list, where the GTK window shows it).
- **Evidence:** self-test "the state text fits on the Playback heading line clear of the heading" for every one of the 23 style x scale x width cases; visual gate states `state-paused-*`, `direct-loading-*`, `border75-*`; sheets `07c-playback-state`.
- **Risk:** low.
- **Harness rules:** none.

## DD-129 — Error boxes start with one plain sentence; technical detail comes below, under "Details:"

- **Category:** clearer message; user decision 2026-10-19 (error-message layout), now built (prompt 012). Extends DD-003 and DD-012.
- **What changed:** every failure message box now starts with one plain sentence saying what went wrong; below it, after a blank line, the heading `Details:` and the technical detail (an error code, a file position, a driver's or Windows' own text), only where it helps a bug report. Every fact of the old text is kept (the details are the old wording, word for word); the box title is unchanged. Windows: a plain `MessageBox` (no new window class, XP-safe), text `<plain>\n\nDetails:\n<detail>` (a smaller type size is not possible in a MessageBox). Linux (GTK): the plain sentence is the dialog's main text and the details are its secondary text under `Details:`, so both players say the same words. The layout is decided in one header-only module (`core/include/midibn/app/error_text.h`: `explain_error`, `explain_engine_error`, `explain_load_error`, `join_error_text`, `split_error_text`); the lower layers keep their exact texts because the harness, the diagnostic tools and the comparison tests read them; the controller applies the rule at its single message entry. When the engine kept the output's own failure under its sentence (for example "MIDI reset/all-notes-off failed while stopping playback"), it is now shown as a `Cause:` line in the details; before it was never shown.
  Examples, before -> after: `Could not open Z:\no such folder\missing song.mid (Windows error 3).` -> "The MIDI file could not be opened." + "Details:" + the old text; `Unsupported system status 0xF5.` -> "This file could not be read as a MIDI file. It may be damaged or not a complete MIDI file." + "Details:" + the old text; a driver error -> "The MIDI output could not be opened." + "Details:" + the old text. A message that was already plain with no detail (for example "SMPTE time division is not supported; this version requires PPQN timing.") is unchanged.
- **Why:** the user wanted the first thing read to be plain, with the technical part separate.
- **User-visible effect:** as described.
- **Evidence:** `app_tests` (`test_error_text_layout`: layout and split, every message family, the engine cause, the loader kinds, the missing file end to end, a plain controller message stays plain). No other test read these texts. The complete before -> after list is in the P12-ERR working files (not committed).
- **Risk:** low; wording and layout only.
- **Harness rules:** none (the harness reads the lower layers' texts, which did not change).

## DD-130 — Direct mode refuses `wdmaud2.drv` (Windows MIDI Services)

- **Category:** safety (extends DD-085); found on a tester's Windows 11 PC.
- **What changed:** a `Drivers32` MIDI value whose file name is `wdmaud2.drv` (case-insensitive, with or without a path) is classified as Windows' own driver and refused for Direct mode, exactly like `wdmaud.drv`. The reason text reads "Windows' own MIDI driver (wdmaud2.drv, Windows MIDI Services) serves it".
- **Why:** on Windows 11 with Windows MIDI Services the registry names `wdmaud2.drv`; on a tester's PC it appeared as a user-mode driver with no devices, so it would have been offered as a Direct candidate. It is part of Windows, not a synthesizer driver.
- **User-visible effect:** such an entry is greyed out with the reason (DD-127) instead of being listed as a candidate with no devices.
- **Evidence:** `direct_output_tests`: three new checks (plain, upper case, full path); 186 checks pass on x64, 185 on XP IA32 (the other check is x64 only).
- **Risk:** none (refusal only).
- **Harness rules:** none.

## DD-131 — A send builds a status object only when it fails (optimization scan item 1)

- **Category:** performance (engine send path); no behaviour change.
- **What changed:** `MidiOutput::try_send_short(packed, failure)` returns true on success without building a status; on failure it stores exactly the status `send_short` would have returned. The direct WinMM and KDMAPI outputs override it (`send_short` now calls it); all other outputs (None, recording, standard WinMM, ALSA, test doubles) use the default, which calls `send_short`. The engine burst (`send_burst_event`) calls it with one status object kept in the engine and raises the same error as before on failure.
- **Why:** every send built and destroyed an `OutputStatus` with two strings.
- **User-visible effect:** none; slightly lower processor cost per message (largest on old processors).
- **Evidence:** measured on this PC (no-op test driver, median of 5 interleaved runs of `--fake-driver --seconds 0.4`): x64 one send at a time 11.7 -> 9.4 ns, burst of 256 7.8 -> 5.7 ns (-27 %); XP IA32 14.2 -> 11.3 ns and 9.2 -> 6.1 ns (-34 %). All 10 test programs pass on x64; the v2 side of the parity check passes (3,956 engine runs identical, digests unchanged); on Linux the engine, ALSA output and unit tests pass. New checks in `direct_output_tests` and `midi_output_tests`. Details in DEVICE_MEASUREMENTS.md ("Direct send cost"). Not yet measured on the real Pentium 4 (a `v2-xp-direct-check-ia32.exe --fake-driver` run prints the new `lean_ns` and `lean_batch_ns` figures).
- **Risk:** low; the failure path and its texts are unchanged.
- **Harness rules:** none (digests identical).

## DD-132 — A few-processor measurement tool in the XP package

- **Category:** tooling (diagnostics); no player change.
- **What changed:** `midibn-few-cpu-measure --auto [--out FILE] [--rounds N]` (XP IA32 name `v2-xp-few-cpu-ia32.exe`) writes its own silent test file, runs itself as child processes with output None (no device, no driver, no sound), the option "Let other programs run while playback is behind" off against on, with 2 logical processors and with 1, in 2 interleaved rounds, and writes `few-processor-result.txt` (medians: how far behind playback ends, worst send lateness, messages per second, and the wake-up delay of a competing thread). No new run starts after 80 s. Step 7 of both XP launchers runs it (information only, not part of OVERALL).
- **Why:** to inform whether the option should ever switch on by itself when very few processors are usable (the user's Hyper-Threaded Pentium 4). The first decision (Q2) was that it is not remembered; replaced 2026-10-25 (remembered; on by default only with exactly one usable logical processor, DD-112).
- **User-visible effect:** none for the player; one more small file in the XP package (XP_COST_LOG.md).
- **Evidence:** table in DEVICE_MEASUREMENTS.md ("One or two logical processors", package form). On one logical processor the option removes the multi-second starvation of ordinary threads at no measurable cost in playback; on two it changes little.
- **Risk:** none.
- **Harness rules:** none.

## DD-133 — The Linux loopback test reports what arrived and judges only the player's own events

- **Category:** test fix (Linux); the ALSA output was not changed.
- **Status:** cause not yet confirmed. On the tester's Debian 13 PC (kernel 6.12, ALSA library 1.2.14) the real-sequencer loopback test failed in its last check only: opening the port and all four sends succeeded, but "the four events arrive in order with the right types" did not hold. The kernel source (6.12) shows the "port subscribed" notice goes only to subscribers of the system announce port, so that notice is not the cause. WSL has no `/dev/snd/seq`, so the cause could not be reproduced here.
- **What changed:** the old test took the first four events of any kind that arrived on its test port and required exactly note on, controller, pitch bend, SysEx. The test now (1) finds the player's client from the subscription on its test port, (2) reads for up to one second and then 150 ms more, (3) prints every event that arrived (type, source and destination client:port, channel, value, SysEx length), and (4) judges only the events whose source is the player: note on (channel 3, note 60), controller (channel 3, controller 7), pitch bend (channel 3, value 0), SysEx (4 bytes), in that order. Events from other clients or the system are listed but do not fail.
- **Verification without a sequencer:** the judging is a plain function fed recorded values; `test_loopback_judging` in `midibn-alsa-output-tests` feeds it the sequences a real PC can produce (clean; an announce notice and another client's event in between; a missing SysEx, swapped order, wrong value, an extra player event, nothing from the player). The tester's next run prints the real events in `logs/unit-tests-alsa-output.txt`, which settles the cause if the check still fails.
- **User-visible effect:** none for the player.
- **Risk:** none.
- **Harness rules:** none.

## DD-134 — The Linux tester package puts the player first

- **Category:** tooling (Linux); amends DD-125.
- **What changed:** the package has a top-level one-page `README.txt`, `start-player.sh`, a folder `MIDIBottleneck Player` (the GTK player under that name) and a separate folder `self-test` with the checks, the launchers and the data. The README has four steps: unpack; make Qsynth's MIDI input appear (Setup, MIDI tab, "Enable MIDI input", driver `alsa_seq`, keep Qsynth open, check with `aconnect -o`); run the self-test with the silent send check to that port; the player as its own step (open a file, choose the Qsynth port, play, send screenshots and anything odd). Technical details moved to `self-test/TECHNICAL-DETAILS.txt`. The player is required for the package (stored as `MIDIBottleneck Player/MIDIBottleneck Player`). `MIDIBN_PKG_OUT` writes the package somewhere other than `v2/out/linux-selftest`; `MIDIBN_PKG_BUILD` sets the build folder. The glibc floor is unchanged (2.34; whether to keep it is the open question Q4).
- **Why:** the first tester package listed the player among the tools and Qsynth's input port did not appear during the tests (no real synthesizer was reached).
- **User-visible effect:** none for the player.
- **Risk:** none.
- **Harness rules:** none.

## DD-135 — Android is a new platform: two builds from one source (modern API 29 and Android 6+ API 23)

- **Category:** new platform (prompt 012, work units U11 to U19); no change to the core's behaviour.
- **What changed:** v1 has no Android version. v2 adds a Gradle project `v2/android/` (modules `core`, `player`, `selftest`) around the unchanged portable core and the Linux platform layer (clock, threads, files compiled with Bionic). Two flavors are built from the same source: **modern** (Android 10 and newer, API 29) sends through the native AMidi interface from C++; **android6** (Android 6 and newer, API 23, application id suffix `.android6`, names "... (Android 6+)") sends through Java's `android.media.midi`. Three processor types are built (`arm64-v8a`, `armeabi-v7a`, `x86_64`), 16 KB page alignment, static C++ runtime. The APKs are debug-signed; a release signing and packaging step does not exist yet. The only library bundled is the built-in synthesizer (DD-140); the Android Gradle plugin and the NDK are build tools.
- **Why:** the user wants the player on his two phones; the native route fits dense, precisely timed output, the Java route reaches older phones at a higher cost per burst.
- **User-visible effect:** a new app, "MIDIBottleneck Player" (the 6+ build has the suffix in its name). Its differences from the Windows player are in DD-136 to DD-139 and in `UI_DIFFERENCES.md`.
- **Evidence:** all player and self-test APKs build for three processor types; the portable test programs run on a phone through the self-test app (DD-142). Timing on the phones is not measured yet.
- **Risk:** low for the other platforms; the shared files touched are listed in DD-143.
- **Harness rules:** none.

## DD-136 — Android output routes: one write per burst, no call into Java per message

- **Category:** performance and platform difference; no audible effect.
- **What changed:** v1 and Windows hand every message to the driver in its own call. On Android the engine sends through `BatchingOutput` (`platform/android/route/`): it encodes each message to raw MIDI bytes, collects one engine burst (`begin_send_batch` to `end_send_batch`) in one buffer and writes it to the route with **one write**; a message is never split between writes, a buffer holds at most 1000 bytes, and a long SysEx message is cut into 1000-byte writes. The route targets are the native AMidi port (one `AMidiInputPort_send` per write, from the engine thread, no Java), the Java route (one JNI call and one `MidiInputPort.send` per write, so a dense file costs one call per burst, not one per message) and a stand-in that records the writes for tests. The player's own output, `RoutedOutput` (header only, no JNI), lists and opens devices through a route catalog and forwards every send, reset, panic and batch call, in C++, to the opened route's `BatchingOutput`. The output list is: None; the devices of the **preferred hardware route** (AMidi on the modern flavor, the Java route on android6; never both, so no device appears twice); then the devices of each non-hardware route (the built-in synthesizer, DD-140). A device is found again by name if the list changed between listing and opening; if it is gone, the open fails with a plain sentence. SysEx is sent whole (assembled F0..F7, as on the other platforms); there is no per-driver SysEx switch. `reset()` drops the buffer and the unfinished SysEx; `panic()` is the shared 48 messages in one write. A write error or an unplug is remembered (the same text for every later send, no more calls into the dead device) until the output is closed and opened again; nothing throws.
  This replaces the first design (a Java `RouteFactory` looked up by reflection, and a Java call per message); only the behaviour above remains.
- **Why:** a call from C++ into Java per message costs time and a copy; a burst of several hundred messages should cost one.
- **User-visible effect:** none audible; the order of messages inside a burst is unchanged. Hardware: USB MIDI devices (plugged in directly or through an adapter; class-compliant; no USB permission is needed) and Bluetooth MIDI devices once connected in the system settings. A phone without USB host mode, and a device with a vendor-specific USB driver, do not appear.
- **Evidence:** host tests of the batching and encoding (C++ and Java), native batching tests for x86_64 and arm64-v8a, all APKs build. Not yet run with a real device: a USB keyboard or module on both phones, how `MidiInputPort.send` behaves on unplug, and `RouteBench` at 250 messages per burst on both routes.
- **Risk:** low; the timing of the two routes is the open measurement.
- **Harness rules:** none.

## DD-137 — Android devices: the list follows plugging and unplugging, Rescan keeps everything, the device opens in the background

- **Category:** new behaviour (a core addition used only on Android); nothing changes on Windows or Linux.
- **What changed:**
  - `PlayerController::refresh_devices()` reads the output device list again without rebuilding the controller. The loaded file, the Rate model, the queue settings and every option stay. The selection stays on the same device, found by name (the index may change); None stays None (a device that comes back is not selected on its own). If the selected device is gone, the output becomes **None** and a Warning "MIDI output removed" names the device and says the output is now None (the controller has no automatic fallback for a failed open elsewhere: a failed open reports an error and keeps the selection; None is the safe choice that never fails). It works only while stopped: while playing, paused or while a timed-out output call is still running it changes nothing and says "Stop playback first" (Information). The Direct WinMM answers are forgotten and asked again, since device numbers may change. The Windows and GTK players do not call it.
  - The Android player calls it from the **Rescan** button and also by itself, when stopped, whenever a route reports that a device was added or removed. While playing, the rescan waits for Stop. A device lost while playing makes later sends fail with the route's sentence ("The MIDI device ... was unplugged. Choose the output again."), reported like any output failure; the app does not crash.
  - A hardware device is opened in the background when it is selected, so Play does not wait for it. (The core opens the output on the main thread at Play, which can take up to 5 seconds for a slow device. A complete fix would be an asynchronous open in the controller with an "Opening the MIDI output" state; it is not needed now.)
  This replaces the first design, where Rescan rebuilt the whole player and opened the last file again, so the rate model, queue and other options returned to their defaults.
- **Why:** USB devices come and go; a phone user plugs in a keyboard after the app has started.
- **User-visible effect:** as described.
- **Evidence:** `midibn-app-tests`, `test_refresh_devices` (device added in front, vanished device, None stays None, playing, deferred controller).
- **Risk:** low.
- **Harness rules:** none.

## DD-138 — The Android player interface: one portrait page, the transport at the bottom

- **Category:** platform interface (the table is in `UI_DIFFERENCES.md`, "Android interface").
- **What changed:** plain framework views (Java, no AndroidX, no library) over the same `PlayerController` as the other players. One portrait page: the transport (position bar, time, -5 s, Play/Pause, Stop, +5 s) is pinned at the bottom, the rest scrolls (File and output, Processing model, Statistics, More settings). The system-menu items are check boxes in **More settings**. Files come from the system file picker, with no storage permission. A foreground service of type media playback (a low-importance notification with the file name, the state and a Stop action, and a partial wake lock held only while a song plays or is paused) keeps playback going with the screen off; refusing the notification permission (asked once on Android 13 and newer) hides only the notification. The display refreshes about 10 times a second while playing or loading. Not on Android: the KDMAPI and Direct WinMM boxes, Application scale, always on top, borderless, show/hide sections (the system font and display size apply), number-box dragging (typed entry; Done or leaving the field commits), the coloured queue-pressure bar (a text line, red for 0.8 s after an overflow). Not built yet: the Analysis, Channels and Track routing pages, statistics tooltips, the effective-speed window choice, opening a file from outside the app, a Pause action on the notification.
- **Why:** a phone is held in one hand; a thin native layer gives real controls, the system's look and accessibility.
- **User-visible effect:** as described.
- **Evidence:** the APKs build; not hand-tested on a phone yet.
- **Later in the same run:** "More settings" became the top bar's menu (DD-144); the statistics follow the Analysis cells (DD-150); the refresh follows the display (DD-151); the look is DD-152. The rest stands.
- **Risk:** the missing pages are the largest gap against the other players.
- **Harness rules:** none.

## DD-139 — Android Help has its own wording and credits the built-in synthesizer

- **Category:** clearer text (extends DD-108 and DD-126).
- **What changed:** `HelpPlatform::Android` is a third wording of the shared Help. Overview: file picker instead of drag and drop; the output list (None, the built-in synthesizer, USB and Bluetooth MIDI devices); **Rescan**; **More settings** instead of a menu; no window frame, borderless mode or Application scale; the transport at the bottom; playing with the screen off through the notification; the output, Synth settings and menu switches remembered (DD-154). File and output: the file picker, the output list, Rescan (the song and all options stay; a vanished device becomes None and the status line says so), the built-in synthesizer. Channels: "With touch" replaces the mouse and keyboard sections. Rate model and Statistics are word for word the Windows text. Not mentioned on Android: ALSA, KDMAPI, WinMM, Direct WinMM, title bar, borderless, Alt+Space, drag and drop, F1, mouse and keyboard keys. **Credit:** the Overview ("Good to know") and the Output section say that the built-in synthesizer is Sonivox, Android's own, under the Apache License 2.0, and that the licence text is in the app.
- **Later in the same run:** the Overview describes the top bar's menu (with **Synth settings...** and **Help**) instead of More settings, File and output gains a **Synth settings** section, the live-number line follows DD-151, and the state is "shown in the **Playback** cell of the Statistics" (DD-150). Windows and Linux Help dumped before and after: byte-identical (`v2/out/prompt-012/U26-help/hashes.txt`).
- **Why:** the Windows and Linux wording does not apply on a phone; the licence asks for the credit.
- **User-visible effect:** as described. Windows and Linux Help are unchanged.
- **Evidence:** all 16 Windows and Linux topic texts dumped before and after the Android wording change are byte-identical, and again after the Sonivox credit (SHA-256 lists in `v2/out/prompt-012/P12-AND5/help-sha-*.txt` and `v2/out/prompt-012/P12-FOLD2/help-sha-*.txt`); `midibn-ui-tests`, section "help-android" (titles, forbidden words in every topic, required words, shared topics, the credit only on Android).
- **Risk:** none for the other platforms.
- **Harness rules:** none.

## DD-140 — Android has a built-in software synthesizer (Sonivox); Windows and Linux do not

- **Category:** new feature on one platform; user decision 2026-10-24 (recorded in V2_RULES.md, section 2).
- **What changed:** the Android output list has **Built-in synth (Sonivox)**, always listed (even with no USB device), with the type "Software"; it is never the default. It is the Sonivox EAS synthesizer of the Android Open Source Project (Apache License 2.0), built from source (archive from `android.googlesource.com/platform/external/sonivox`, SHA-256 E164D65B8E4D730C70A4D896FED8D199F5B25D07E2091C91A3546DAC71BA1F25, 2026-10-24) and vendored in `v2/android/third_party/sonivox/` with its notice, which is also in the APK as `sonivox-NOTICE.txt`. It is the only bundled third-party component. The sound is Sonivox's: the General MIDI wavetable built into the library (about 0.5 MB, 22.05 kHz), 64 voices, stereo output with a simple reverb, no soundfont loading, no per-channel mixer; it sounds like the old Android ringtone player. Build difference to Android's own copy: the open-source release declares but does not contain the raw MIDI stream functions (`EAS_OpenMIDIStream`, `EAS_WriteMIDIStream`, `EAS_CloseMIDIStream`), so the player implements them (about 80 lines, `platform/android/synth/sonivox_midi_stream.c`) on Sonivox's own parser; no vendored file is changed. Not built: JET and the XMF, OTA, iMelody and RTTTL parsers; plain C sample kernels. Code layout: `SonivoxSynth` (the only code that touches Sonivox, on the audio thread), a lock-free byte ring from the output thread, `SonivoxRouteTarget` (so the shared batching, SysEx, reset and panic code applies) and `AudioSink` with the OpenSL ES implementation.
- **Later in the same run:** the 64-voice and no-soundfont limits above were lifted: up to 4096 voices (DD-146) and DLS sound banks (DD-149), with a Synth settings page (DD-145).
- **Why:** a phone without a MIDI device should still make a sound. Better control is a later decision (BASSMIDI is open).
- **User-visible effect:** as described. Size: the stripped native library grows 0.58 MB per processor type (deflated about 0.29 MB), so a release build grows about 1 MB for three types. Debug APKs: player-modern 65.9 -> 70.2 MB, player-android6 64.0 -> 68.2 MB.
- **Evidence:** unit tests of the synthesizer, ring and sink; APK sizes before and after as above. The sound has not been heard on a phone yet.
- **Risk:** licence (Apache 2.0, notice shipped); the sound quality is Sonivox's and fixed at build time.
- **Harness rules:** none.

## DD-141 — The built-in synthesizer's delay, overload and silence behaviour

- **Category:** timing note for DD-140.
- **What changed:** audio goes through OpenSL ES in 3 buffers of 11.6 ms, so about 35 ms of audio is queued ahead of the speaker, plus the phone's own audio delay (22.05 kHz is not a native rate on most phones, so Android resamples and the fast path is not used). Notes start on 11.6 ms boundaries with a constant delay; the engine's own timing and the MIDI sent are unchanged. The send side never waits for audio. If the audio thread stalls for several seconds, the 64 KiB queue (about 21,000 messages) fills and later messages are dropped and counted; this never blocks the player and is never reported as an output failure (a dropped note-off can leave a note sounding until Stop or the next reset). Messages sent while the audio is not running are dropped and counted too. Reset throws away MIDI not yet read by the audio thread. Panic (CC 120, 123, 64 = 0 on all channels) makes the synthesizer fade out within about 11 ms of reading it (measured in the unit test); audio already in OpenSL ES (up to about 35 ms) still plays, so silence is reached about 45 ms after Stop.
- **Why:** the audio path cannot be made to wait for the engine, and the engine must never wait for audio.
- **User-visible effect:** the sound comes about 0.04 s after the note is sent; a USB device has no such delay.
- **Evidence:** unit tests. To measure on the phone (DEVICE_MEASUREMENTS.md): the reported latency and whether any buffer is late.
- **Risk:** low.
- **Harness rules:** none.

## DD-142 — The Android self-test is its own app

- **Category:** tooling (Android); no player change.
- **What changed:** `:selftest` (package `org.midibottleneck.selftest`, "MIDIBottleneck Self-test", plus the `.android6` flavor) is never part of the player. Its native tests are the unchanged test sources built as shared libraries `libst_<name>.so`, run in a forked child process through a small JNI library (`dlopen`, then `main`), with output going to files; a crash or hang in one program fails only that program, and no permission to run files from the app directory is needed. The test data (fixtures, expected results, the v2 baseline; about 73 MB, 10 MB zipped) is one zip asset, unpacked on first run. Results are written to `Downloads/MIDIBottleneck self-test/<date_time_model>/` (MediaStore on API 29 and newer; public Downloads with a run-time storage permission on API 23 to 28); `summary.txt` ends with `OVERALL PASS` or `OVERALL FAIL`, and INFO and SKIP lines never fail a run. It also records the device's capabilities, a 2 ms tick timing run (normal and raised priority) and a send check per route. APK size: main about 58 MB, 6+ about 56 MB (45 MB native libraries, 10 MB test data).
- **Why:** to let the user run every check on his phones with one tap.
- **User-visible effect:** none for the player.
- **Evidence:** built for three processor types in both flavors; `main` is exported from every `libst_*.so`. The first phone run has not happened yet; its numbers go into DEVICE_MEASUREMENTS.md.
- **Risk:** none.
- **Harness rules:** none.

## DD-143 — Changes in shared files found by the Android build

- **Category:** bug fix (portable code) and build rule.
- **What changed:**
  1. `PosixThread::start` (`platform/linux/linux_platform.*`) takes the body as `std::function<void()>&&` and empties it right after moving it into the thread's argument. Cause: Android's C++ library (libc++) copy-constructs a small stored lambda on a `std::function` move and leaves the moved-from function holding its own copy; the caller's copy of the captures could then be the last reference to a `shared_ptr` and be destroyed on the caller thread if the new thread finished first (the check "the body's captures are destroyed on the worker thread" failed on the phone). libstdc++ steals the stored object, so GCC never showed it. No call site changed; no behaviour change on GCC.
  2. The test "peak resident size is reported (VmHWM)" allocated 20 MB it never read, and Clang removes such an allocation. The test now reads one byte per page through a volatile pointer. The platform code was right.
  3. `linux_platform_tests.cpp` uses `TMPDIR` for its temporary files when it is set (default `/tmp`), because an Android app has no `/tmp`.
  4. The Android native code is built with **`-ffp-contract=off`**. Clang on arm64 contracts `a*b+c` into a fused multiply-add by default, which changes the last bit of `double` results; the results must match Windows and Linux bit for bit (the "effective-speed" rows of the ui-tests), so contraction is off in every Android target.
- **Why:** as stated per item.
- **User-visible effect:** none.
- **Evidence:** `midibn-linux-platform-tests` failed 2 of 149 checks on the phone before items 1 and 2; the engine and unit tests pass on the other platforms after the change.
- **Risk:** none.
- **Harness rules:** none.

## DD-144 — Android: a top bar with a menu button replaces the "More settings" section

- **Category:** platform interface (updates DD-138; user request 2026-10-24).
- **What changed:** the main screen has a top bar (app name on the left, a menu button of three drawn bars on the right) that stays visible while the page scrolls. The menu holds the check rows of the desktop player's system menu (same texts, same actions; checked and enabled state read again every time it opens), then **Synth settings...** and **Help**. Help moved from a button into the menu. The "More settings" section is gone. Rows are at least 48 dp high; a long app name is shortened with "...".
- **Why:** the user asked for a menu at the top instead of the section at the bottom.
- **User-visible effect:** as described; one tap more to reach Help.
- **Evidence:** builds; used on the test phone by the user (the menu and the Synth settings page).
- **Risk:** none.
- **Harness rules:** none.

## DD-145 — Android: Synth settings page for the built-in synthesizer

- **Category:** new feature on one platform (extends DD-140; user request 2026-10-24).
- **What changed:** a page opened from the menu, only for **Built-in synth (Sonivox)**: volume 0 to 200 % (100 = normal; above 100 "may distort"), voice limit (DD-146), reverb on/off (default off, as Sonivox ships), reverb type (large hall, hall, chamber, room; default hall), reverb amount 0 to 100 % (default 85), **Reset to defaults**, the sound bank (DD-149), and at the bottom the level meter (DD-148) and the synth's status line (latency, dropped messages, slow buffers, voices sounding; twice a second). Every change is heard at once: the page only stores the value and the audio thread applies it before its next buffer. When the synth is not running the page says how to start it and the values are used when it starts. The settings were first not saved; since 2026-10-25 they are remembered (replaced, DD-154). Limits of Sonivox the page cannot remove: one reverb amount only (no usable separate dry control), the reverb acts on the whole mix; lowering the voice limit while notes sound mutes the oldest voices at once (done in the player's own Sonivox glue, because Sonivox's own call does not reach sounding voices).
- **Why:** the built-in synthesizer needs its controls; the user asked for a Synth settings page.
- **User-visible effect:** as described. Windows and Linux are unchanged (no synthesizer is included there).
- **Evidence:** `midibn-synth-tests` (153 checks after this item, 224 after DD-146 to DD-149), in WSL (plain and under ThreadSanitizer) and on the phone: volume 0 is silent, 50 % gives about half the peak, 200 % is louder, voice limits 1 and 2 hold a six-note chord to 1 and 2 voices (also when lowered live), reverb lengthens the tail, amount 0 equals off, the presets differ, values set while closed apply on open, clamping and reset. Logs: `v2/out/prompt-012/P12-AND8/`, `P12-AND9/`, `android-phone-3/`.
- **Risk:** low.
- **Harness rules:** none.

## DD-146 — Android synthesizer: up to 4096 voices, a number field beside the voice slider

- **Category:** improvement on one platform (user decision 2026-10-24).
- **What changed:** the Sonivox library is built with a maximum of 4096 voices (Google's build has 64); the synth still opens at 64. The voice slider covers 1 to 1024; the number field next to it takes any value from 1 to the build maximum. A typed value above the slider's range is applied, and the slider is dimmed and pinned at its end until the user touches it again. An empty, zero or too large entry shows a one-line hint and is not applied. Sonivox allows at most 2 voices per key per channel, so non-drum sounds top out near 3,500 voices and dense files need many channels to get there.
- **Why:** the user asked for in-app control above 64 voices.
- **User-visible effect:** as described.
- **Evidence:** render benchmark on the test phone, time per 11.61 ms buffer: 64 voices 0.5 %, 1,024 voices 5.2 %, 2,724 voices (the most a test file reached) 13.7 % (`v2/out/prompt-012/android-phone-4/native/synth-bench.txt`).
- **Risk:** a weaker phone runs out of time at a lower count; slow buffers are counted on the status line.
- **Harness rules:** none.

## DD-147 — Android synthesizer: the volume gives real headroom against clipping

- **Category:** bug fix (of the first version of DD-145; found by the user on the phone).
- **What changed:** the volume used to be a gain on the finished audio, but Sonivox already cuts its mix to 16 bits inside its renderer, so turning down only gave quieter, still flat-topped sound. The volume now goes through Sonivox's own master volume (1 dB steps, applied before its limiter), plus a small ramped gain for the steps in between and exact silence at 0 %. Above 100 % the clipping returns ("may distort").
- **Why:** the user saw that lowering the volume did not reduce the clipping.
- **User-visible effect:** lowering the volume now removes clipping: on a very loud chord the clipped samples fell from 2,378 at 100 % to 32 at 50 % and to 0 at 35 %.
- **Evidence:** `midibn-synth-tests` (clip counts per volume), on the phone.
- **Risk:** none.
- **Harness rules:** none.

## DD-148 — Android synthesizer: level meter and CLIP light

- **Category:** new feature on one platform (user request 2026-10-24).
- **What changed:** at the bottom of the Synth settings page: a bar with the peak level, a **CLIP** light that stays lit for about a second after any clipped sample, and the number of buffers with clipping so far. Clipping means samples at the 16-bit limits in Sonivox's own output. Read ten times a second while the page is open.
- **Why:** to show when to lower the volume.
- **User-visible effect:** as described.
- **Evidence:** unit tests of the meter counters.
- **Risk:** none.
- **Harness rules:** none.

## DD-149 — Android synthesizer: "Load DLS sound bank..."

- **Category:** new feature on one platform (user request 2026-10-24).
- **What changed:** the Synth settings page opens the system file picker (every file selectable). The player reads the bank on a background thread through the file descriptor the picker grants and hands the finished bank to the audio thread, which installs it between buffers without stopping the sound. The page shows the file name, **Use built-in sounds**, and one plain sentence if loading fails (not a DLS file, the synth cannot use the bank, too large, cannot be read). Instruments the bank does not have keep the built-in sounds; a General MIDI bank (bank 0) replaces the default melodic instruments and its drum kits replace the drum kit. Replaced 2026-10-25: the bank is remembered (DD-154); no copy of the file is kept (a provider that cannot be read at an offset is copied to the app cache and the copy is deleted straight after loading). Sonivox's limits apply (up to 256 instruments, 1024 samples, 1 MiB per sample, mono 8/16-bit PCM); the player limits the file to 192 MiB. The loader is the player's own (`EAS_LoadDLSCollection` in `platform/android/synth/`); no vendored file is changed.
- **Why:** the user asked for DLS loading.
- **User-visible effect:** as described.
- **Evidence:** unit tests with generated DLS files; `gm16.dls` was copied to the phone (`/sdcard/soundfonts/`) for the user to try.
- **Risk:** a bank outside Sonivox's limits is refused with a sentence.
- **Harness rules:** none.

## DD-150 — Android statistics follow the Analysis window's Playback statistics

- **Category:** platform interface (user request 2026-10-24; updates DD-138).
- **What changed:** the Statistics card shows the cells of the Analysis window's Playback statistics with the same captions, units and order (Playback, Timeline / output, Queue now / maximum, Events sent / dropped, Output rate in the long form "events/sec", Effective speed, Lag current / maximum), followed by the main window's Maximum rate cell. The two lag cells became the one Lag cell and the separate state line became the Playback cell. The queue-pressure line and **Reset statistics** stay. The strings come from the shared controller, as on the desktop.
- **Why:** the user asked for statistics like the desktop's Analysis window.
- **User-visible effect:** as described. Windows and Linux are unchanged.
- **Evidence:** builds; seen on the phone.
- **Risk:** none. Open: Maximum rate keeps the short "events/s" (the long form needs an additive controller getter); queued for the user.
- **Harness rules:** none.

## DD-151 — Android live updates follow the display, with the desktop's adaptive rule

- **Category:** performance and platform interface (user request 2026-10-24; updates DD-138).
- **What changed:** instead of polling every 100 ms, while playing or loading the page updates once per display frame (Choreographer), arms the next frame only after the update has finished, and uses the desktop's adaptive rule (the `display_tier.h` ladder and budgets): half of the statistics cells per update when drawing costs too much, then 60, 30, 15 or 10 updates a second, and back up when calm. Idle states update on events only. Wake calls from the engine are coalesced, so they cannot pile up on a busy main thread. The rule is `PlayerBridge.RefreshPacing`, host-tested. Debug read-out: a long press on the app name shows the counters of the latest run of updates (also logged with the tag `MIDIBN-pace`).
- **Why:** the user asked for the adaptive refresh on Android.
- **User-visible effect:** smoother live numbers on a calm phone, fewer updates on a busy one.
- **Evidence:** host tests (130 checks in total with DD-152); on-phone counters not yet read.
- **Risk:** the cost counted for the tier rule is the update plus the gap beyond one frame before the next callback (the layout and drawing pass lies in that gap); the on-phone counters will show whether that is right.
- **Harness rules:** none.

## DD-152 — Android: the player's own look, light and dark

- **Category:** platform interface (user request 2026-10-24).
- **What changed:** one palette (`Palette`, pure Java) with a light and a dark set that follows the system setting; `Ui` holds the spacing, shapes, text sizes and the styling helpers. Calm blue accent, rounded section cards, tonal secondary buttons and filled primary buttons (Open MIDI, Play), the statistics cells as tiles, a top bar in the accent colour reaching under the status bar. Every page uses it (main screen, Help, Synth settings); the manifest theme carries the same colours for everything else (dialogs, menus). No control, text, order or behaviour changed. Switching the system between light and dark recreates the page; playback is unaffected.
- **Why:** the user asked for a better-looking Android player.
- **User-visible effect:** as described.
- **Evidence:** a host test checks the WCAG contrast of every text pair (4.5:1) and control pair (3:1) in both modes and that `colors.xml` equals the palette; dark-mode screenshot of the main screen on the phone (`v2/out/prompt-012/android-phone-4/theme-main.png`). The Synth settings page in the new look has not been seen on a phone yet.
- **Risk:** none.
- **Harness rules:** none.

## DD-153 — Settings file: where it is, how it is written, what a damaged file does (Windows and Linux)

- **Category:** feature (user decision 2026-10-25, DD-026).
- **What changed:** UTF-8 text file: a comment line, `format=1`, then `key=value` lines. Keys: `output` (the output by name, `None` for no output; restored only when a device of that name exists, otherwise the usual default, and the name is kept until the user picks another), `kdmapi`, `direct_winmm`, `chase_midi_state` (default 1), `preserve_queue`, `output_paced`, `apply_queue_limit`, `queue_age_limit`, `scale_note_interval`, `speed_window_us` (250000), `always_on_top`, `borderless`, `borderless_style` (0 to 2), `show_processing`, `show_statistics` (both 1), `scale_percent` (100), `few_cpu_yield` (`auto`, 0, 1). Unknown keys are ignored; a wrong value falls back to the default of that key only; a newer `format` number is read (known keys) but the file is never overwritten. **Location:** Windows `%APPDATA%\MIDIBottleneck Player\settings.ini` (shared by the x64, x86 and XP programs); if a file named exactly `MIDIBottleneck Player.ini` exists next to the program, that file is used instead (portable mode; an empty file switches it on; the program never writes into its own folder unless that file is already there). Linux: `$XDG_CONFIG_HOME/midibottleneck/settings.ini`, else `~/.config/midibottleneck/settings.ini`. **Writing:** atomic (a temporary file in the same folder, flushed, then `MoveFileExW` with replace and write-through; Linux fsync and rename); only when a remembered value changes, never at start; a failed write never shows a box. **Damaged file:** no box. A sentence appears in the file-information area of the main window while no file is loaded (and in its tooltip, with the technical detail below "Details:"): "Your saved settings could not be fully read, so the standard settings are used for those options."; "Your saved settings could not be read, so the standard settings are used." (cannot be opened); "Your saved settings were written by a newer version of this program and are left unchanged." (changes made then are not saved); "Your settings could not be saved, so they are kept only until the program closes." (a save failed, noted once). A successful save repairs the file. **Tests and scripts** never touch the user's file: `--no-settings` (every v2 launch script passes it; implicit for `--selftest`, `--screenshot`, `--popup-shot`, `--thread-report`, `--report` and `--auto-close-ms`), `--settings FILE` (a test with its own file), `--settings-appdata DIR` (tests the default-location code), `--settings-change` with `--selftest` (round-trip check).
- **Why:** user decision 2026-10-25. A fixed portable file name was chosen over `<program name>.ini` because the release programs have different file names per build.
- **User-visible effect:** see DD-026. The Help (Overview, "Good to know") and the Direct WinMM tooltip say what is remembered and where the file is.
- **Evidence:** tests in the prompt 013 run (draft P13-SETW, folded here); the Android Help topics are byte-identical.
- **Risk:** low. Open points (recommendation: keep all): a remembered KDMAPI tick with KDMAPI no longer installed gives the usual "KDMAPI is not available" message; borderless and Always on top are remembered because they are menu options; a gone output falls back to the default and its name is kept so a USB synthesizer plugged in later is found again.
- **Harness rules:** none.

## DD-154 — Android remembers its settings

- **Category:** platform interface (user decision 2026-10-25; matches the desktop rule).
- **What changed:** remembered between runs: the chosen MIDI output (by the text of its list entry; chosen again only when that entry is in the list, so a USB device that appears a moment late is still picked up, and an absent device leaves the normal default), the Synth settings (volume, voice limit, reverb on/off, type, amount), the loaded DLS sound bank, up to 8 recently loaded banks, and the six toggles of the menu. Not remembered: the loaded MIDI file, scroll position, which groups are folded, window state, and the Analysis, Channels and Track routing values (those pages do not exist yet). Stored in the app's SharedPreferences file `midibn_player` as text, read back per key: `output.item`, `synth.volume` (0..200), `synth.voices` (1..build maximum), `synth.reverbOn`, `synth.reverbPreset` (0..3), `synth.reverbAmount` (0..100), `synth.dls.uri`, `synth.dls.name`, `menu.t0` to `menu.t5`, `dls.recent`. A value that is missing or out of range counts as damaged and is not applied (the core's own default stays); a damaged key never affects another key and never stops the app. A toggle that is greyed out at start is applied as soon as it is enabled while stopped. A bank from a content URI is kept readable with a persisted read permission (released when the entry leaves the recent list). If the bank cannot be opened at start, the built-in sounds are used and the Synth settings page shows one line: "The sound bank used last time (...) could not be opened; the built-in sounds are used." The entry stays in the recent list, marked "not available" when it cannot be opened; a tap that fails removes it with a note.
- **Why:** user decision 2026-10-25.
- **User-visible effect:** the player starts as it was left. To reset: **Reset to defaults** on the Synth settings page (everything except the sound bank), or clear the app's storage in Android settings.
- **Evidence:** host test `PrefsHostTest` (tolerant reads, per-key fallback, the recent list). Not yet seen on a phone.
- **Risk:** low.
- **Harness rules:** none.

## DD-155 — Android: foldable groups

- **Category:** platform interface.
- **What changed:** on the main page and the Synth settings page, tapping a group heading folds the group away and tapping again opens it (a chevron at the right of the heading turns). Kept for the run of the app only (not remembered).
- **Why:** a phone page is long; folding keeps it short.
- **User-visible effect:** as described. Not yet seen on a phone.
- **Risk:** none.
- **Harness rules:** none.

## DD-156 — Android: touch entry for number boxes, finer sliders

- **Category:** platform interface.
- **What changed:** a number box on touch: tap to type, or drag sideways on the box to change the value, with acceleration (the desktop's "drag to scrub, or type"). Done, the keyboard's close button, Back or a tap outside the box applies the value and leaves the box. The processing-model slider and the voice-limit slider are finer at low values: value = max * (e^(k*x) - 1) / (e^k - 1) with x the thumb position 0..1, k = 2.5 for the processing-model slider (bottom of the track 4.5 times finer, top 2.7 times coarser) and k = 4 for the voice-limit slider (bottom 13 times finer). The curve sits in front of the core's slider scale (v1's: linear to 5000 us, then logarithmic, 0..10000), so the core, the typed value and the DIN preset are unchanged.
- **Why:** a finger cannot hit small values on a linear slider; there is no mouse for scrubbing.
- **User-visible effect:** as described. Not yet seen on a phone.
- **Risk:** low.
- **Harness rules:** none.

## DD-157 — Android: choosing a file

- **Category:** platform interface.
- **What changed:** **Open MIDI...** offers folders and MIDI files (by type); **Open any file...** lists everything; **Other app...** lets any installed file app (for example File Manager+) hand over the file. The player also opens .mid files that other apps open ("Open with") or share.
- **Why:** some phones' file picker hides MIDI files because of their media type.
- **User-visible effect:** two extra buttons under Open. Recommendation: keep.
- **Risk:** none.
- **Harness rules:** none.

## DD-158 — Android: the statistics keep their place

- **Category:** platform interface (fix).
- **What changed:** the Statistics values use equal-width digits and a cell height that never shrinks back, so nothing around them moves while the numbers change.
- **Why:** the values jumped around while playing.
- **User-visible effect:** a steadier page. Check on the phone: a long press on the Statistics heading cycles test values (9 / 10 / 100 / 1000 at 20 per second); nothing around may move.
- **Risk:** none.
- **Harness rules:** none.

## DD-159 — Android: About and licences screen

- **Category:** platform interface.
- **What changed:** the menu has **About and licences**: the player notice, the GPL text (starts folded), the library list, and the Sonivox notice and licence. This replaces the earlier asset file that only existed inside the APK.
- **Why:** the licence text must be reachable in the app (the Help says so).
- **User-visible effect:** one more menu entry.
- **Risk:** none.
- **Harness rules:** none.

## DD-160 — The Analysis's "Projecting finite queue pressure" no longer takes minutes, and a changed value stops it at once

- **Category:** performance (Analysis)
- **What changed:** (a) With the queue length limit on and the overflow policy "Drop background notes", every incoming event that found the queue full made the projection rank every queued note again to find the least important one (v1's way: about 5,000 notes ranked, through three hash tables, for each of millions of events). The Analysis now keeps its own copy of the pending queue (`analysis::FastPendingQueue`, `core/include/midibn/analysis/fast_pending_queue.h`) with an ordered index of the queued notes, kept up to date as notes enter and leave, as their release arrives and as their chord's lowest or highest pitch changes; the least important note is read from the front of that index. The choice it makes is exactly the one the old ranking makes (same order of the six rules, same fairness cursor), so every drop, every queue size and every bucket is unchanged. The live engine keeps using `models::PendingMidiQueue` unchanged. (b) The projection looks at the cancellation request every 1,024 passes of its loop instead of every 16,384 events (the progress report keeps its rhythm).
- **Why:** with a queue of 5,381 notes and a file of 10.4 million events the projection took about 22 minutes (v1: 32 min 35 s, measured with v1's own analyzer on the same file and settings). While it ran, changing any value in the Analysis window started a new projection; the old one was cancelled correctly (the window does not wait for it), but it only noticed after up to 16,384 more events, which in the slowest stretches took seconds, so the window felt stuck.
- **User-visible effect:** the same Analysis figures and graphs, much sooner: user file 1 (10,385,604 events; queue limit 5,381, Drop background notes, 1,833 microseconds per event) 22 min 16 s -> 3.5 s. A changed value cancels the running projection within about a millisecond.
- **Evidence:** `v2\out\prompt-013\P13-ANA\timings.md` (all times), `compare-*.txt` (before/after result hashes equal on 3 files x 5 to 12 settings each, all 6 overflow policies, with and without the age limit), `before-userfile1.log` and `after-userfile1.log` (Analysis result hash and double-precision hash identical), unit test "FastPendingQueue matches PendingMidiQueue on 1,200 random queues in three set-ups" (722,485 step-by-step comparisons of every eviction, queue order and fairness cursor against the engine's queue with the old ranking), cancellation-poll test, parity PASS on x64, x86 and XP-IA32 (v2 baselines byte-identical), Analysis, app and UI tests pass.
- **Risk:** low: the old ranking stays in the engine and is the reference in the unit test; the Analysis queue is only a different way to find the same note. Memory: about 80 bytes per queued note instead of 36 (only matters with an age limit and a very long queue).
- **Harness rules:** none (no output difference).

## The prompt 011 questions: answers (recorded in prompt 012) and what is still open

- **Q1** (the matters of taste): all accepted, keep v2's way (2026-10-19). See `UI_DIFFERENCES.md`.
- **Q2** (is the few-processor option remembered): first no (2026-10-19); replaced 2026-10-25: yes, it is remembered with the other settings (DD-026, DD-112).
- **Q3** (a fixed `system32\OmniMIDI` fallback for KDMAPI): not needed (DD-119).
- **Q5** (Linux in 2.0): Linux is not part of 2.0 unless it is ready at the same time as Windows.
- **Q6** (Android): started in the prompt 012 run; the state is DD-135 to DD-152.
- **Q7** (optimisation scan item 1, the per-send status object built only on failure): done in the prompt 012 run (DD-131, measured numbers there).
- **Open: Q4, Q8 and Q9.** Q8 is the Linux thread priority (an ordinary user is refused the raise on the tester's PC, DD-122); Q9 decides whether F1 works inside the Linux Track routing window (DD-124 table in `UI_DIFFERENCES.md`); the Linux items that depend on them wait. Q4's text is not recorded in the published documents.

## Proposals awaiting the user's decision (nothing changed)


- **Help text** (resolved: v2 has its own Help text, DD-108; the user should read it once).
- **32-bit large-file warning threshold** (resolved: half of the usable address space, DD-118; user decision 2026-10-18, prompt 011).
- **Reset all controllers (CC121) at Play** now that Direct WinMM keeps the device open (DD-111). Recommendation: no. User decision 2026-10-18 (prompt 011): no extra reset (DD-111).
- **Few-processor yield option** (DD-112): off by default; proposal to enable `yield` only on a PC with exactly one usable processor. User decision 2026-10-18 (prompt 011): wanted as an option, off by default (implemented as a menu item on a one-core PC, see the update in DD-112).
- **Window height at 50 %** (about 19 pixels taller, DD-101) and **file information hidden in compact Integrated-header layouts** (DD-103): both accepted for now, queued for the user's review. User decision 2026-10-18 (prompt 011): both accepted.
- **`ia32-vs-sse2.ps1` can extend to 7 rounds** (DD-115): time cost of long runs, queued for the user. User decision 2026-10-18 (prompt 011): accepted.
- **Mixed rounding of the sliders and scrub fields** (UI_DIFFERENCES.md): some mappings round half to even, others half away from zero, as v1 does. Not a bug; the rules decide where each slider position lands. Proposal (only if wanted): one rule everywhere, which moves a few positions by one unit.
- **The application font size steps at exactly 50% and 75%** (`UiScaleFont`): the point size is raised only at those two scale percentages and not at 60% or 74%. Layout, so not changed; proposal: smooth steps.
