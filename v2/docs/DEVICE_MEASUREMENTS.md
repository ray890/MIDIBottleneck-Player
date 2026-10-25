# Real-device output measurements, v1 against v2

Status: measured on one PC (see "Machine and conditions"). Prompt 007, Step 2. Facts only; no design is chosen here. The quiet-PC figures of prompt 009 are in the section "Quiet session, 2026-10-16 (prompt 009)" at the end of this file; they supersede the under-load figures below wherever the same quantity was measured again.

## What was measured and how

Both programs play the same generated MIDI files through their own real playback engine into the same real
output, and record when each send really happens:

- v1: `v2/harness/v1-reference/device/DeviceMeasure.cs`, compiled against the unchanged v1.0.44 sources
  (real `Stopwatch`, `SpinWait`, `HighResolutionWaiter`, `WindowsMidiOutput`, `KdmApiMidiOutput`).
- v2: `v2/tools/device_measure_main.cpp` (target `midibn-device-measure`), v2's engine and its Win32 WinMM and KDMAPI outputs.
- Statistics for both come from the same code, `v2/tools/device_analysis.cpp`, reading the same sample-file format.
- Runner: `v2/tools/device/run-device-measurements.ps1` (at the time of prompt 007: mute, run, restore; see "Mute handling"; it no longer mutes).

Output paths (all end in the installed OmniMIDI 14.8.5, engine WASAPI shared mode on the default playback device):

| name | what it is |
|---|---|
| `winmm-system` | OmniMIDI through the system WinMM (`midiOut*` on the "OmniMIDI" device) |
| `kdmapi` | OmniMIDI through KDMAPI (`OmniMIDI.dll` loaded from System32, `SendDirectData`) |
| `winmm-wrp` | OmniMIDI through WinMM with the third-party winmmwrp `winmm.dll` placed next to private copies of the test executables (x64 DLL, so x64 only) |
| `null` | no device; the engine runs against an output that does nothing (engine-only ceiling) |

Both engines are set up as v1's `MainForm` sets its engine (queue mode, "simulate slowdown" off, no limits,
channel monitoring off), so the immediate-dispatch path is the one measured.

### Fixtures (generated on demand under `v2/out/device-test/fixtures/`, never committed)

Single track of controller messages 20 to 31 (undefined by the MIDI standard: a synthesizer does almost no work
for them, so the measurement is about delivery, not about sound generation).

- `uniform`: 1 microsecond ticks, events spread evenly (about 9.5 per microsecond at 9.5 million per second).
- `clustered`: 960 ticks per quarter at 120 BPM (a tick is 520.8 microseconds) with all events of a tick at the same
  instant, the way a real dense file is laid out.
- `overcap`: clustered, sized to be about three times what the path can deliver (25 million events per second for
  1 second on the fast paths), to find the maximum sustained throughput.
- `clustered-rel50`, `clustered-rel80`: clustered, 50 % and 80 % of the path's own measured capacity.
- A fixture is cut to at most eight seconds of playback at the path's capacity (`winmm-system` plays only about
  40 thousand events per second, so a 28-million-event file would take twelve minutes per play).

### Lateness and the other numbers

Every 128th send (and the last) is timestamped before and after the output call; the event's intended time is read
from the song afterwards. Lateness = (time of the send, counted from the engine's own time zero) minus (the
event's intended time). The engine's time zero is found from two snapshots taken a few milliseconds into the run
(timer reading minus the transport position the snapshot reports). That adds one brief snapshot per run.
The counter that ticks the timestamps runs at 10 MHz, so a single call duration is a multiple of 100 ns; only the mean over
many samples is meaningful below that, and the tables give the mean.

"Stall" = the send schedule slips by more than the stated amount between two consecutive samples (lateness jumps up).
"Spacing mode" = the most common distance between stall starts.

## Programs compared

| name | what it is |
|---|---|
| v1 | the unchanged v1.0.44 engine and outputs, .NET Framework 4.8 |
| v2-spinwait | v2 as it was before the final-approach change (commit `4c65aba` engine: the ported .NET `SpinWait` that sleeps one timer period) |
| v2-precise | v2 after commit `9838ffc` (keeps the processor in the last stretch before an event; no `Sleep(1)`) |

## Machine and conditions

Windows 10 22H2, 12 logical processors (6 cores), OmniMIDI 14.8.5 (engine WASAPI shared mode, default playback device).
Other agents were compiling and browsers were open all the time: the rest of the PC used 30 % to 65 % of all
processors during the runs (recorded per run as `cpu_other_processes_pct` in `runs.csv`; medians per table line were
between 29 % and 91 %). Absolute numbers are therefore lower and noisier than on a quiet PC; the comparison between the
three programs is fair only in that they ran back to back under similar load (fixture by fixture, v1 then the two v2
builds). Each figure is the median of 3 plays (the 3 plays of one file run in one process, after a short warm-up
play). The scripted run is `run-device-measurements.ps1`; raw results are kept under `v2/out/device-test/results/` (not committed).
x64 only; x86 was not measured (the winmmwrp DLL is x64, so that path cannot be tested in x86 anyway).

## Mute handling

(Historical, prompt 007. The runner no longer mutes anything: `run-device-measurements.ps1` now plays the data as it is, at its
volume, and changes no audio setting; it only records the endpoints' current state read-only. See `v2/tools/device/AUDIBLE-TESTS.md`.)

All active playback endpoints (Headphones, which is the default for all three roles, and Speakers) were muted through
Windows Core Audio before any device output; the mute flag was read back; the originals (both not muted, volume
levels 1 % and 57 %, never changed) were restored after every run and read back. OmniMIDI's configuration was
inspected first: engine 3 (WASAPI, shared), `WASAPIOutput` holds a placeholder id, so it plays on the default device
(ASIO would have meant no output at all). The fixtures use controller messages that make no sound anyway. Two runs were stopped early by hand;
both were restored with the standalone command `audio-mute.ps1 -Action Restore` and verified. Lesson: the runner's
`finally` block did not run when its console pipe was closed early, so the restore must be checked after any early stop.

## 1. Maximum sustained throughput

*Superseded for v1 against the current v2 by the quiet figures at the end of this file (generated fixtures); kept as the record of the intermediate builds v2-spinwait and v2-precise, measured under load.*

Millions of events per second actually delivered (first to last send), median of 3, fixture sized to be far over capacity
(`overcap`) and the two 9.5-million-per-second files (clustered / uniform), so the three columns show the spread under load.

| path | v1 | v2-spinwait | v2-precise |
|---|---|---|---|
| null (engine only, no device) | 4.8 / 4.9 / 4.8 | 5.4 / 5.4 / 5.3 | 5.5 / 5.7 / 5.3 |
| kdmapi | 3.5 / 3.4 / 3.2 | 3.9 / 4.3 / 4.4 | 3.8 / 4.3 / 4.5 |
| winmm-wrp | 3.3 / 3.8 / 3.9 | 4.6 / 4.6 / 4.6 | 4.6 / 4.6 / 4.4 |
| winmm-system | about 0.04 | about 0.04 | about 0.04 |

(order in each cell: overcap / clustered 9.5M / uniform 9.5M). Findings:

- The engine, not the device, is the limit on this PC. With no device at all the engine delivers only about 5 million
  events per second (180 to 200 ns per event). At the nominal 9.5 million per second every program is always behind
  (lateness grows to 1 to 5 seconds), so "9.5 million per second" is over capacity for all of them here.
- v2 delivers about 12 % to 40 % more than v1 on the same path (v2 about 4.3 to 4.6 against v1 3.3 to 3.9 through KDMAPI and winmmwrp).
  v2-spinwait and v2-precise are the same speed (the final-approach change is irrelevant once playback is saturated).
- Your reference ("v1 reaches about 11 million with WinMM and winmmwrp"): the 11 million appears when only v1's output call is
  timed in a tight loop (v1 `WindowsMidiOutput.Send`: 11.0 to 12.2 million per second through winmmwrp; 9.6 to 10.2 through KDMAPI).
  The whole v1 engine in front of it delivered 3.3 to 3.9 million per second on this PC under the load above. The engine
  does per event: three lock acquisitions and four timer reads (`Stopwatch.GetTimestamp`, about 20 ns each here) plus
  bookkeeping, about 200 ns for a do-nothing output. A quiet PC with a faster timer read would be faster; that was not
  tested here, so the gap between 11 and 3.5 is explained by the engine's per-event cost, with machine load as a possible but unmeasured factor.
- System WinMM into OmniMIDI is slow for every program: about 37 000 to 44 000 events per second (25 microseconds per call,
  the call goes through `wdmaud.drv` and the kernel driver; `winmm.dll`, `wdmaud.drv`, `ksuser.dll` and `VirtualMIDISynth.dll`
  were loaded in the process). The same in v1 and v2 (v1 `midiOutShortMsg` loop 0.046 to 0.051, v2 0.039 to 0.044 million per second).

Raw send ceilings, no engine (million calls per second, 3 repeats):

| call | v1 | v2 |
|---|---|---|
| output object, null | 78 to 82 | 188 to 198 |
| output object, KDMAPI | 9.6 to 10.2 | 33 to 42 |
| output object, WinMM + winmmwrp | 11.0 to 12.2 | 28 to 32 |
| bare provider call, KDMAPI | 16 to 19 (v1 delegate), 27 to 34 (DllImport) | 77 to 92 (function pointer) |
| bare `midiOutShortMsg`, winmmwrp | 29 to 32 (DllImport), 30 to 37 (through v1's `Func`) | 65 to 89 |

## 2. Why KDMAPI is slower than WinMM in v1, and whether v2 has the same cost

Both end in the same OmniMIDI function: the winmmwrp DLL's `midiOutShortMsg` is `KDMAPI_midiOutShortMsg`, which calls
`SendDirectData`. So the difference is on the application side.

- v1 WinMM: `Send` calls a `Func<IntPtr,uint,uint>` that is bound to a `DllImport` method (a plain, blittable P/Invoke).
- v1 KDMAPI: `Send` calls through the `IKdmApiNative` interface to `DynamicKdmApiNative.SendShort`, which invokes a delegate
  created with `Marshal.GetDelegateForFunctionPointer`. That delegate goes through the generic function-pointer marshalling
  stub, which is slower than a DllImport call: measured bare call 16 to 19 million per second for v1's delegate against 27 to 34
  for a `DllImport` of the same function (about 1.8 times the time per call), and 9.6 to 10.2 against 11.0 to 12.2 million per
  second through the full output object (about 15 % slower). In the engine the send call is 75 to 95 ns for v1 on both paths, so the user-visible gap is small.
- v2: `KdmApiMidiOutput::send_short` makes one virtual call and then a plain function-pointer call; there is no marshalling stub.
  v2 KDMAPI is not slower than v2 WinMM (output object 33 to 42 against 28 to 32 million per second; WinMM carries an extra `std::function` indirection).
  v2's send call inside the engine is 28 to 36 ns on both paths. So v2 does not have the v1 KDMAPI penalty.

## 3. Timing fidelity and the "quantized" sound

*The lateness tables are superseded for v1 against the current v2 by the quiet figures at the end of this file; the cause analysis below stays valid.*

Clustered file at 1 million events per second for 3 seconds (well below every program's capacity, so it should be on time).
Lateness in microseconds (median of the 3 plays), stalls = jumps of more than 1 ms in 3 seconds.

| path | program | p50 | p99 | max | stalls over 1 ms | most common distance between stalls |
|---|---|---|---|---|---|---|
| null | v1 | 4 937 | 13 175 | 16 594 | 191 | 15.75 ms (also 1.25) |
| null | v2-spinwait | 5 137 | 12 918 | 14 927 | 193 | 0.75 to 15.75 ms |
| null | v2-precise | 261 | 926 | 1 976 | 8 | 1.25 ms |
| kdmapi | v1 | 582 | 5 975 | 15 425 | 564 | 1.75 ms |
| kdmapi | v2-spinwait | 583 | 2 044 | 9 395 | 655 | 1.75 ms |
| kdmapi | v2-precise | 259 | 1 023 | 10 804 | 8 | 1.25 ms |
| winmm-wrp | v1 | 566 | 2 734 | 11 702 | 576 | 1.75 ms |
| winmm-wrp | v2-spinwait | 584 | 2 005 | 3 207 | 661 | 1.75 ms |
| winmm-wrp | v2-precise | 256 | 944 | 5 212 | 15 | 1.25 ms |

Lateness of the same file when loaded to a share of what the path can deliver (clustered, 3 s; p50 / p99 in microseconds;
share of the lower of the programs' measured capacity: kdmapi 3.46, winmm-wrp 3.29, null 4.82 million per second):

| path, load | v1 | v2-spinwait | v2-precise |
|---|---|---|---|
| kdmapi 50 % | 775 / 2 157 | 667 / 2 076 | 255 / 1 138 |
| kdmapi 80 % | 1 573 / 10 702 | 706 / 6 932 | 273 / 1 608 |
| kdmapi 95 % | 1 643 / 8 137 | 964 / 2 926 | 280 / 3 013 |
| kdmapi 115 % | 313 195 / 801 088 | 1 732 / 9 653 | 6 664 / 56 296 (note) |
| winmm-wrp 50 % | 700 / 2 098 | 686 / 3 346 | 250 / 1 066 |
| winmm-wrp 80 % | 910 / 4 613 | 861 / 3 511 | 298 / 2 359 |
| winmm-wrp 95 % | 14 571 / 63 915 | 935 / 3 550 | 294 / 3 962 |
| winmm-wrp 115 % | 166 377 / 527 773 | 1 453 / 10 481 | 350 / 3 486 |
| null 50 % | 4 570 / 14 393 | 4 257 / 15 100 | 288 / 3 364 |
| null 80 % | 5 282 / 16 285 | 4 862 / 15 859 | 239 / 4 151 |
| null 95 % | 9 753 / 25 106 | 5 297 / 16 127 | 291 / 5 652 |

Note: the kdmapi 115 % file (3.98 million per second) is at v2's own capacity; v2-precise fell behind in that run
(PC load 66 %) and recovered slowly. At the edge of capacity the result is decided by momentary capacity, not by the wait mechanism.

Findings:

- Cause CONFIRMED. When a cluster of events has been sent and the next cluster is a few hundred microseconds away, the engine's
  wait ends with the `SpinWait` loop, which after about 30 spins calls `Sleep(1)`. That sleep lasts one timer period.
  Evidence: (1) with no device (nothing in the process asks for a fast timer) lateness rises to 12 to 17 ms in a sawtooth with
  bursts about every 15.6 ms (timeline files `*.timeline.csv`: sends happen in 3 to 4 ms bursts, then 12 ms of silence; p50 4.9 ms, p99 13 ms);
  (2) with OmniMIDI in the process the same pattern shrinks to 1 to 2 ms (stall distance 1.75 ms), which fits OmniMIDI asking
  for a 1 ms timer (inferred; the in-process sleep length was not measured separately);
  (3) v2-spinwait, which reproduces the .NET loop on purpose, shows the same numbers as v1; (4) v2-precise, which has no `Sleep(1)`,
  has median lateness 250 to 300 microseconds (the time to send a 520-event cluster at about 70 ns each is the floor) and 8 to 15 stalls
  instead of 190 to 660. At 1 million per second the quantization is removed on the paths with a device; without a device the 15 ms pattern is removed (p50 4.9 ms to 0.26 ms).
- The user's "oddly quantized" sound is therefore this mechanism: events due within a window are held and sent together, in groups every 1 to 2 ms
  (with OmniMIDI loaded) or about every 15 ms (when the process has no fast timer).
- Catch-up bursts near saturation (suspect b): ruled out as a periodic source. At the over-capacity and 9.5 million files no
  waits happen (1 to 5 stalls over 1 ms in the whole run); lateness simply grows steadily to seconds, with no ~1 ms or ~15.6 ms periodicity.
  What near-saturation does produce is large irregular delay: v1 at 95 % on winmm-wrp had p50 14.6 ms and p99 64 ms, v2 stayed under 1 ms p50.
- v2-precise costs no noticeable processor time (this process used 0.1 to 0.3 % of all processors at 1 million per second, v1 0.6 to 1.4 %).
- First-send offset: v1's start call takes about 340 microseconds and the engine's time zero is 100 to 125 microseconds after it; v2: about 75 and 12 microseconds.

## 4. Where the time goes at high density

Time per event, engine saturated (milliseconds per million events = nanoseconds per event):

| | v1 | v2 |
|---|---|---|
| whole engine with a do-nothing output | about 196 ns (5.1 Meps) | about 182 ns (5.5 Meps) |
| inside the output call, KDMAPI | 75 to 95 ns (27 to 29 % of the run) | 28 to 36 ns (12 to 14 %) |
| inside the output call, winmm-wrp | 63 to 95 ns (25 to 27 %) | 28 to 44 ns (13 to 16 %) |
| the rest (scheduling, statistics, locks, timer reads) | 180 to 200 ns | 150 to 180 ns |

The rest cannot be split by sampling without changing the engine; from the code, per event the engine takes the engine lock three times
(`current_transport_ticks` twice, the in-service statistics update once) and reads the timer four times, with calibrated costs of
17 to 20 ns per lock and about 20 ns per timer read for v1 (.NET) and 14 to 19 ns and 13 to 15 ns for v2. That accounts for about
130 ns of v1's 196 ns and about 105 ns of v2's 182 ns (estimate: counts from the source times measured unit costs, not a direct split).
The statistics are published once per batch of up to 2048 events or 8 ms; that part is small. The send call itself is about a quarter (v1) or about an eighth (v2) of the time.

## 5. Instrumentation overhead

Throughput of the same over-capacity file with: no wrapper around the output (none), a wrapper that only counts (count),
the wrapper timing every 128th send (sample, used for all tables above), and timing every send (tsall):

| | none | count | sample | tsall |
|---|---|---|---|---|
| null, v1 | 5.25 | 4.89 | 4.82 | 4.02 |
| null, v2-precise | 5.61 | 5.24 | 5.47 | 4.52 |
| null, v2-spinwait | 5.49 | 5.30 | 5.36 | 4.71 |
| kdmapi, v1 | 3.38 | 3.32 | 3.46 | 2.90 |
| kdmapi, v2-precise | 4.43 | 4.17 | 3.84 | 3.71 |
| kdmapi, v2-spinwait | 4.10 | 4.23 | 3.87 | 3.91 |

The wrapper (one extra call and counter per event) costs about 2 to 7 %; sampling every 128th send adds nothing measurable beyond that
(differences are inside the run-to-run noise of 3 to 8 % on this loaded PC); timing every send costs 15 to 23 %. So the
sampled figures are slightly below what the engine does unwrapped (throughput in the tables is about 2 to 8 % low) and the timing shape is unaffected. The timer ticks every 100 ns,
so individual send durations are multiples of 100 ns; the send-call means are averages over thousands of samples with the timer-read cost (14 to 21 ns) subtracted.

## 6. Driver and tool problems

- System WinMM: after a 3-million-event flood the process hung in the driver's reset or close and never returned (killed after 6 minutes; the run data had been written first).
  Every program now ends with a 20-second watchdog around reset and close. The watchdog fired 7 times in an earlier, interrupted full run
  (v1 kdmapi once, v1 winmm-wrp 4 times, v2 winmm-wrp 2 times) and twice in the final run (v2-spinwait kdmapi, after the over-capacity file and after the 80 % file).
  Both v1 and v2 hit it; no OmniMIDI dialog was seen. Playing itself never failed or dropped events (`dropped_events` 0, no playback errors).
- The native-window monitor flagged a Windows Error Reporting window (`WerFault.exe`) twice while a test finished normally; it belonged to another process on the PC
  (not the test, which had exited with code 0). The monitor itself uses noticeable processor time at its default poll rate (25 % of a core at 250 ms), so the runner polls every 2 s.
- The winmmwrp DLL is identical in size and SHA-256 to the installed `OmniMIDI.dll` (it is OmniMIDI's own library acting as `winmm.dll`). It lived only in
  `v2/out/device-test/x64-winmmwrp/<program>/` and was deleted at the end of every run.

## Rerun

```
.\v2\tools\device\run-device-measurements.ps1 -Programs v1,v2-precise -Label rerun
```
`v2-<name>` programs are executables kept at `v2\out\device-test\v2-<name>\midibn-device-measure.exe` (copy a new build there). `-Aggregate <results folder>`
rebuilds the tables from the raw files; `-OverheadOnly`, `-Quick`, `-Paths`, `-Fixtures` narrow a run. The v2 tool needs `-Build` once (or build target `midibn-device-measure`).

## User-file passages (tool options added for the final comparison)

Both `midibn-device-measure run` and the v1 driver (`v1-device.exe run`) can play a part of a file, as the players do on
Play-from-position: `--start <s> [--seconds <d>] [--mark <s>]` or `--passages 118-126:123,190-210` (start-end[:mark] in source
seconds; the mark is where the heavy part itself begins). The engine is started at that source time (`Start(song, output, mode,
startMicroseconds)`), so the state chase of controllers, programs and banks runs exactly as in the players (`--chase 0` turns it off;
the players default to on). Reported per run: the events of the window and of the passage, events sent, throughput, lateness
percentiles and stalls (from the sampled sends, with intended times counted from the start position), the engine's own Maximum lag
(the players' statistic: the largest lateness of any event sent since playback started) and the lag at the mark, effective speed
(the players' 250 ms measure, sampled every 16 ms like their statistics timer), a wall-clock cap (`--cap-extras`, the run is then marked
`capped=yes`), whether the chase restored the volume, expression, pedal, program and bank the file had set before the start, and the
notes the file has sounding at the start (`held_at_*`). `--outputs a,b,c --path-tags ... --label-template ...` plays the passages on
several outputs in one process so a big file is loaded once; `--load-only 1` reports load time and peak memory only; `--instr
count,sample` plays each passage once per instrumentation. The user-facing recipe is in `v2\tools\device\AUDIBLE-TESTS.md` (section 3b).

## Update, Milestone 7: engine throughput (DD-065 to DD-067)

The engine cost per event reported in section 4 (about 180 ns, 100-110 ns after DD-011) was reduced to about 15 ns per event on x64 with an output that does nothing (register entries DD-065 to DD-067): events that are already due are sent straight from their stored form, the event in service is published without a lock, and the clock is read once per group of up to 16 events. Tables 1 to 3 above describe the engine before this change; the outputs' own send cost (28 to 36 ns) is unchanged.

Repeatable, silent measurement of the engine path (no MIDI device is opened; outputs `null`, `count`, `recording` only): `v2\tools\throughput-measure.ps1`. It runs `midibn-device-measure.exe` on the real clock with the real worker thread, optionally interleaved with an older build (`-Baseline <older midibn-device-measure.exe>`: A, B, A, B, ...), records the power plan, whole-PC CPU load and processor clock for every set, and labels the result "under load (interleaved)" unless the PC was under 20 % busy.

```
.\v2\tools\throughput-measure.ps1 -Arch x64 -Baseline <older>\midibn-device-measure.exe -Rounds 5
.\v2\tools\throughput-measure.ps1 -Arch x64 -Sets late-9.5M,late-1M -Rounds 8 -Baseline <older>\midibn-device-measure.exe
```

The real-device runs (`v2\tools\device\run-device-measurements.ps1`) use the same executable and therefore the same engine path. (Superseded by the quiet figures in "Engine throughput, quiet" at the end of this file; kept as the original under-load record.) Measured at the time of the change (x64, whole-PC CPU 30 to 90 %, balanced power plan, interleaved, medians): 12 million events due at once, null output, 13.99 -> 67.28 million events per second (4.8 times); Drop mode 11.12 -> 53.62; counting output 12.18 -> 47.95 with identical message hash; recording output 12.49 -> 43.74. With a send of about 30 ns this gives about 22 million events per second end to end on this PC.

## Direct WinMM on Windows XP (DD-085; the Windows XP run of the prompt 008 package, recorded 2026-10-16)
Windows XP SP3, Pentium 4 3.8 GHz (2 logical processors), 32-bit check program `v2-xp-direct-check-ia32.exe`, silent messages only (controller messages that make no sound).
- **OmniMIDI through direct access:** 3 of 3 open/send/close rounds passed; SysEx completions 3 of 3; nothing outstanding after close.
- **Speed:** standard WinMM about 0.59 million messages per second, direct access about 5.5 million (about 9.3 times faster).
- **First open:** 1.2 to 3.5 s (OmniMIDI starting its synthesizer).
- **Microsoft GS Wavetable Synth:** no driver this program may open directly serves it, so it fell back to standard WinMM, with the message shown.
- **Refused as designed:** Windows' own MIDI driver (`wdmaud.drv`) and the MIDI Mapper.
- The quick run's check took 10 s and the full run's 8 s; neither is part of OVERALL.

**Second XP run (the prompt 009 package, recorded 2026-10-17; same Windows XP SP3 Pentium 4 PC):** quick run PASS (117 s in total; direct-access check 8 s), full run PASS (905 s in total; direct-access check 8 s). Standard WinMM about 0.58 million messages per second; direct access 2.26-2.45 million (3.9-4.2 times faster). That is slower than the first XP run above (5.5 million, 9.3 times; each send 182 ns then, 410-442 ns now). Cause: DD-094's call gate put two locked processor operations around every send, and a Pentium 4 pays over 100 ns for each; fixed in DD-110 (one per send, none inside a burst), real XP figures pending. Full details are in `XP_COST_LOG.md`.

**Third XP run (the prompt 010 package, recorded 2026-10-18 in prompt 011; same Windows XP SP3 Pentium 4 3.8 GHz PC with Hyper-Threading; package `v2-xp-selftest.zip`, SHA-256 starting 458FE1D7DFD07B2A):** quick run OVERALL PASS (145 s in total: release build 88 s, SSE2 test build 27 s, direct check 16 s, thread reports 10 s), full run OVERALL PASS (933 s in total: 455 s + 451 s, direct check 13 s, thread reports 11 s; traces 55, flows 110, models 9; explained differences 403 in traces and 761 in engine files; baseline 174 and engine runs 3,956 identical).
- **Speed with real OmniMIDI:** standard WinMM 0.600 million messages per second; direct access 6.437 million per second (155 ns per send), 10.7 times faster. This is better than the prompt 008 package (5.5 million, 182 ns) and far better than the prompt 009 package (2.26 million), so **DD-110 (the cheaper send gate) is confirmed on real hardware.**
- **Cost with a no-op test driver (no sound, no synthesizer):** bare call 23.3 ns; one send at a time 117.7 ns, of which the output wrapper is 94.4 ns; sends in 256-message bursts 79.7 ns, wrapper 56.4 ns. The single-send wrapper cost is a candidate for a later optimisation scan.
- **Threads (DD-113):** Windows' own `wdmaud.drv` has a Time Critical thread before anything opens. With OmniMIDI open (the standard and the direct path alike) the Time Critical threads are DirectSound x2, BASS x1 and OmniMIDI x2; BASS also has one at Highest. OmniMIDI leaves one ordinary thread alive after close.
- **KDMAPI did not open on XP:** "No KDMAPI provider was found." OmniMIDI is installed at `C:\WINDOWS\system32\OmniMIDI\OmniMIDI.dll`, a subfolder that v2's KDMAPI search (application folder, then System32) does not look in; v1 most likely has the same gap. Fix in progress (prompt 011).
**Fourth XP run (the prompt 011 package; run 2026-10-18 9:36-9:52 PM, recorded 2026-10-24 in prompt 012; same Windows XP SP3 Pentium 4 3.80 GHz PC, 2 logical processors; numbers read from the result files):** quick run OVERALL PASS (144 s in total: release build 88 s, SSE2 test build 26 s, direct check 15 s, thread reports 14 s), full run OVERALL PASS (941 s: 462 s + 450 s, direct check 12 s, thread reports 15 s; traces 55, flows 110, models 9; explained differences 403 in traces and 761 in engine files; baseline 174 and engine runs 3,956 identical).
- **KDMAPI is found now (DD-119 confirmed on real hardware).** `kdmapi-check-result.txt`: the application folder and the system folder have no file; the registered driver folder (Drivers32, search position 2) has `C:\WINDOWS\system32\OmniMIDI\OmniMIDI.dll`, an x86 file (PE machine 0x014C), architecture check OK. The KDMAPI thread report opens the backend ("backend: KDMAPI") and sends its 32 silent messages with 0 failed.
- **Speed with real OmniMIDI, full run's check:** standard WinMM 0.594 million messages per second (1,683 ns each), direct access 6.468 million (155 ns each), 10.9 times faster, 3 of 3 rounds, no errors, first open 1,078 ms. The quick run's check (inside `selftest-result.txt`): standard 0.562 million, direct 6.614 million, 11.8 times faster, first open 3,055 ms. (Prompt 010 package: 0.600 million against 6.437 million, 10.7 times.)
- **Cost with the no-op test driver (full run):** bare call 23.5 ns; one send at a time 117.7 ns (output wrapper 94.2 ns); 256-message bursts 79.3 ns (wrapper 55.8 ns). The prompt 010 package gave 23.3 / 117.7 (wrapper 94.4) / 79.7 (wrapper 56.4) ns, so **nothing changed**, as expected: the scan's item 1 (per-send status object built only on failure) was not in this package. The quick run's check: 23.3 / 117.9 / 94.6 / 79.4 / 56.0 ns (7 rounds).
- **Threads (DD-113), this package:** the same picture as before. Windows' own `wdmaud.drv` has a Time Critical thread before anything opens. With OmniMIDI open through standard WinMM there are 11 threads in the process: Time Critical DirectSound x2, BASS x1 and OmniMIDI x2, BASS also one at Highest. Through KDMAPI 12 (one of them "no access" to read); through direct access 15 (the extra ones are ordinary-priority threads from ADVAPI32 and ntdll). After close, standard WinMM and KDMAPI leave 3 threads in total, one of them an ordinary OmniMIDI thread; direct access leaves 7, again one OmniMIDI ordinary thread plus the ADVAPI32 and ntdll ones.

## Direct WinMM on this PC (Windows 10 22H2, Xeon W-11855M; 2026-10-16, quiet, Balanced plan)
`midibn-direct-check` (the Windows 11 check package's program) on the real drivers, silent messages only, one program at a time:
- **OmniMIDI:** standard WinMM 0.053 million messages per second (about 19 µs each), direct access 41.6 million (24 ns each), 5 of 5 rounds, no errors.
- **VirtualMIDISynth #1:** standard 0.050 million, direct 5.4 million, 5 of 5 rounds. Its speed is **limited by the synth** (VirtualMIDISynth 2.x has an internal bottleneck); it is kept for compatibility and correctness only, and no conclusion is drawn from its speed.
- The single-instance guard was confirmed: a second copy started while the first ran waited for it and then ran on its own.

## Direct WinMM on a first tester's Windows 11 PC (the Windows 11 check package; recorded 2026-10-17)
`midibn-direct-check` run once by a tester on a Windows 11 PC, silent messages only.
- **The PC:** Windows build 22631.6199 (Windows 11 23H2; the registry's product name says "Windows 10 Pro", which is normal for Windows 11), AMD Ryzen 9 9950X3D, 32 logical processors.
- **Windows MIDI Services (MidiSrv):** not installed, so this PC still uses the classic WinMM path. OmniMIDI is registered as `midi3` in the system folder.
- **Both start-up paths worked** (the program starting the driver itself, and the driver already started by Windows).
- **OmniMIDI through direct access:** 5 of 5 rounds passed; SysEx completions 3 of 3; nothing outstanding after close; no errors.
- **First open:** 4.9 s (OmniMIDI starting its synthesizer).
- **Speed:** standard WinMM 0.041 million messages per second (24,460 ns each); direct access 58.7 million (17 ns each), about 1,437 times faster.
- **Not claimed:** nothing beyond this one PC is claimed about Windows 11. The machine with Windows MIDI Services installed has not been tested.

## Direct WinMM on a second tester's Windows 11 PC, with Windows MIDI Services installed (run 2026-10-18, recorded 2026-10-24)
`midibn-direct-check` (64-bit program) run once by a second tester, silent messages only. The machine is not named here.
- **The PC:** Windows build 26200.9457, display version 25H2 (Windows 11 25H2; the registry's product name says "Windows 10 Home", which is normal for Windows 11), AMD Ryzen 5 5600GT with Radeon Graphics, 12 logical processors, 9.8 % busy before the check.
- **Windows MIDI Services:** the service `MidiSrv` is **installed and running**; `C:\Windows\system32` holds 10 `Midi2.*.dll` files (the transports and transforms) plus `midimap.dll` and `MidiSrv.exe`.
- **Drivers32 list:** `midi` = `wdmaud.drv` (Windows' own driver, refused); `midi1` = **`wdmaud2.drv`**, listed as a user-mode driver, found and loaded, **0 devices**; `midi2` = `OmniMIDI.dll` in the system folder itself, 1 device; `midimapper` = `OmniMIDI\OmniMapper.dll` (the MIDI Mapper, refused). Both start-up paths (the program starting the drivers itself, and the drivers already started by Windows) give the same list.
- **WinMM devices:** device 0 "Microsoft GS Wavetable Synth" (no driver this program may open directly serves it: standard path); device 1 "OmniMIDI": direct access possible through `OmniMIDI.dll`, the driver's own device 0, borrowed from winmm.
- **OmniMIDI through direct access:** 5 of 5 rounds passed; SysEx completions 3 of 3; nothing outstanding after close; no errors; no busy waits. First open 1,525.59 ms.
- **Speed:** standard WinMM 0.030 million messages per second (33,633 ns each; 29,733 per second in the RESULT line); direct access 20.649 million (48 ns each), **694.5 times** as fast.
- **What it shows:** Direct mode still works on a Windows 11 PC that has Windows MIDI Services running. `wdmaud2.drv`, the new service's driver, appears as a user-mode driver with no devices; it is not a target for direct access (see Step 3 of prompt 012: classify it as Windows' own driver and refuse it).
- **Not claimed:** nothing beyond these two Windows 11 PCs.

## Quiet session, 2026-10-16 (prompt 009)

These are the first measurements of v1 against v2 on a quiet PC. They **supersede the "under load" figures** of the earlier sections wherever the same quantity was measured again (the older tables stay because they compare v2-spinwait and v2-precise, which were intermediate builds, and because they carry the cause analysis; each is marked where it is superseded). Nothing in this section is a design decision.

### Conditions

- **PC:** Windows 10 22H2 (19045), Xeon W-11855M (6 cores, 12 logical processors, base 3.20 GHz), 63.2 GB RAM, OmniMIDI 14.8.5 (engine WASAPI shared), CoolSoft VirtualMIDISynth #1 (a 2.x version).
- **Power plan:** Balanced for everything below unless a row says otherwise; the Windows counter "processor performance" read 143-147 % (about 4.6 GHz) before every run. The power-plan comparison names the other two plans.
- **Quiet PC:** whole-PC CPU load before the measurement runs was mostly 8-13 % (median about 9-10 %; 14.6 % on average at the 3:07 AM entry check). About 4-6 percentage points of that is Windows' own "System" process, which showed 50-70 % of one logical processor in every sample before and after, so it is treated as the PC's baseline. Nothing else was running during the measurements. The tool-run summaries list no run that started above the 20 % quiet limit (the nearest, a Throttle run, started at 19.9 %).
- **VirtualMIDISynth busy:** from about 4:11 AM to about 4:26 AM VirtualMIDISynth's own process kept one logical processor busy (about 8 % of the PC); it had been started at 4:05:25 AM by the Windows 11 check and kept rendering. Rows taken in that window are labelled "VirtualMIDISynth busy". Every VirtualMIDISynth speed or lag figure is also labelled **limited by the synth** (its 2.x version has an internal bottleneck) and **no conclusion is drawn from it**; it is kept for compatibility and correctness only.
- **The user's own files** are named "user file 1, 2, 3": file 1 is 39.6 MB (10.4 million events, 217 s), file 2 is 809 MB (227 million events, 245 s), file 3 is 1,849 MB. Their real names and paths are not recorded here. The files were read only and were unchanged at the end.
- **Programs and tools:** the x64 players (v1.0.44 and the v2 preview, on the test monitor, 60 Hz), read through their own "Maximum lag" and "effective speed" statistics; and the measurement tools (v1: `DeviceMeasure.cs` on the unchanged v1 sources; v2: `midibn-device-measure`), which time every 128th send. The tools play a passage the way the players do (Play-from-position, state chase on). Output paths: OmniMIDI through KDMAPI, through the system WinMM, through the third-party winmmwrp copy (a private test copy, never shipped), and (v2 only) through direct WinMM driver access. Rate model None (Queue, no limit); no event was dropped anywhere.
- **Player figures are single plays** (one number per cell). Tool figures on the user's files are single plays per window, and the generated-fixture tool runs are one play per fixture and program (the earlier sections used the median of three). Treat differences of a few percent as noise.
- "Maximum lag" is the largest lateness of any event sent since playback started (the players' statistic); "at passage" means playback started at the heavy passage, "10 s earlier" means it started 10 s before and arrived at it.

### Audible check clips (3:14-3:17 AM)

Six clips of 12 s, v1 then v2, on the system WinMM, KDMAPI and winmmwrp, all played. Real playback took 11.96 s in every clip: v1 11,964 / 11,963 / 11,963 ms, v2 11,959 / 11,959 / 11,958 ms.

### Loading and memory (the largest differences)

Measurement tool, x64, `-LoadOnly` (results `device-test\results\20261016-063349` and, for v1 on file 3 with the memory check overridden, `20261016-065717`; the tool's own check had skipped v1 on file 3 because it estimated 58 GB, but the real peak was 22.3 GB, so the estimate is far too cautious).

| file | v1 load | v2 load | faster | v1 peak memory | v2 peak memory | less |
|---|---|---|---|---|---|---|
| user file 1 (40 MB) | 5.75 s | 0.30 s | 19 times | 776 MB | 241 MB | 3.2 times |
| user file 2 (809 MB) | 204.7 s | 6.94 s | 29.5 times | 13,348 MB | 5,229 MB | 2.6 times |
| user file 3 (1,849 MB) | 598.7 s | 15.46 s | 38.7 times | 22,336 MB | 11,182 MB | 2.0 times |
| 8-million-event fixture, x64 (`footprint-measure`, 7 runs) | 5,518 ms | 484 ms | 11.4 times | 761 MiB | 220 MiB | 3.5 times |
| 8-million-event fixture, x86 | 7,526 ms | 678 ms | 11.1 times | 636 MiB | 219 MiB | 2.9 times |

Cross-checks from the players themselves (the player's load includes building its window state, so it is a little longer than the tool's):

| | v1 | v2 |
|---|---|---|
| user file 2, player load (two loads each) | 191.7 s and 199.9 s | 7.6 s both times (about 25 times faster) |
| user file 3, player load | 515.4 s and 514.7 s | 22.1 s and 21.9 s (about 23 times faster) |
| user file 3, free memory after load (PC with 63.2 GB, about 47 GB free before) | 25.8 GB (about 22 GB used) | 37.4 GB (about 10 GB used) |
| user file 2, first tool load (3:35-5:20 AM series) | 174.0 s, 14,176 MB | 6.8 s, 5,229 MB |

With both players holding user file 2 at once, 29.9 GB were free. v1's load of file 2 varied from 174 s to 205 s between runs. Start-up and the other builds (x86, XP, XP-IA32) are further down and in `XP_COST_LOG.md`. No paging trouble was seen.

### The players' own "Maximum lag" and effective speed (Part A, single plays)

User file 1, passage 2:03-2:06 (2.46 million events in 3 s), already loaded, 3:21-4:00 AM. Format: Maximum lag, effective speed over the passage.

| path | start | v1 | v2 |
|---|---|---|---|
| None (engine only) | at passage | 56.9 ms, 100.0 % | 1.8 ms, 100.3 % |
| None | 10 s earlier | 51.3 ms, 106.0 % | 1.6 ms, 100.0 % |
| KDMAPI | at passage | 156.3 ms, 100.0 % | 2.1 ms, 100.0 % |
| KDMAPI | 10 s earlier | 336.9 ms, 100.0 % | 2.1 ms, 100.5 % |
| winmmwrp copy | at passage | 169.8 ms, 99.9 % | 2.9 ms, 99.8 % |
| winmmwrp copy | 10 s earlier | 126.9 ms, 100.0 % | 3.3 ms, 100.2 % |
| direct WinMM (v2 only) | at passage | not available | 2.3 ms, 100.0 % |
| direct WinMM | 10 s earlier | not available | 2.3 ms, 100.2 % |
| standard WinMM | at passage | 3,005.6 ms, 17.4 % | 2,727.5 ms, 18.2 % |
| standard WinMM | 10 s earlier | 3,077.0 ms, 17 % | 2,859.6 ms, 18.2 % |

User file 2 (a 16-million-event passage 2:13-2:18, then a 94-million-event passage 3:15-3:30; each cell "at passage, then 10 s earlier"):

| path | passage 2:13-2:18 | passage 3:15-3:30 |
|---|---|---|
| None | v1 1,669 ms (99.9 %), then 1,162 ms; v2 3.2 ms, then 2.1 ms | v1 11,041 ms (23.5 %), then 11,003 ms (27 %); v2 2.3 ms (100.4 %), then 2.3 ms |
| KDMAPI | v1 2,260 ms, then 2,224 ms; v2 2.0 ms, then 3.4 ms | v1 12,160 ms (17.8 %), then 11,837 ms (17.9 %); v2 25.3 ms (100.2 %), then 28.5 ms |
| winmmwrp copy | v1 1,979 ms, then 2,139 ms; v2 3.3 ms, then 3.1 ms | v1 11,821 ms (17.4 %), then 11,278 ms (19.6 %); v2 28.7 ms (99.6 %), then 22.2 ms (100 %) |
| direct WinMM (v2 only) | 2.3 ms, then 3.2 ms | 18.3 ms (100 %), then 29.7 ms (100 %) |
| standard WinMM | v1 6,653 ms (3.5 %), then 12,209 ms; v2 6,608 ms (4.5 %), then 12,153 ms | v1 15,229 ms, then 26,510 ms (2.5 %); v2 15,320 ms, then 26,480 ms (3.4 %) |

User file 3 (v1 alone, then v2 alone; passage 1:00-1:15, started 10 s earlier; only KDMAPI and winmmwrp were played, so that v1's 8.5-minute loads were not repeated for every path):

| path | v1 | v2 |
|---|---|---|
| KDMAPI | 12,477.8 ms, 6.5 % | 283.0 ms, 99.7 % |
| winmmwrp copy | 12,314.9 ms, 6.6 % | 417.6 ms, 99.4 % |

What the tables say:
- **Standard WinMM is the same in v1 and v2** (17-18 % speed on file 1, 2-5 % on file 2): Windows' own `midiOutShortMsg` path, about 19 microseconds per message, is the limit. That is the reason for KDMAPI, winmmwrp and v2's direct access.
- On every other path v2's lag is 1.6-3.4 ms on the lighter passages (v1: 50 ms to 2.3 s) and v2 stays at full speed on the heaviest passages, where v1 fell 11-12 s behind (file 2) or 12.5 s behind (file 3). v1's engine alone (the None path) could not keep up with the 94-million-event passage (23-27 % speed); v2's engine did, with 2.3 ms.
- Where v1 shows an effective speed above 100 % (106 % on file 1, None, 10 s earlier; 136-188 % in the power-plan runs), that is v1 catching up after falling behind.

### The measurement tool on the user's files (4:47-5:20 AM, results `device-test\results\20261016-044734`)

Tool runs, v1 then v2. Maximum lag in ms (the engine's own statistic) for "at passage / 10 s earlier"; then the lateness of the sampled sends at the passage start (99th percentile / maximum, ms) and the rate actually delivered during the passage.

| passage and path | Maximum lag at / earlier (v1 -> v2) | lateness p99 / max, at passage (v1 -> v2) | delivered rate (v1 -> v2) |
|---|---|---|---|
| file 1, 2:03-2:06, KDMAPI | 175.3 / 148.2 -> 3.5 / 3.2 | 147.9 / 150.5 -> 2.06 / 2.65 | 0.80 -> 0.80 million per second (the passage's own rate) |
| file 1, winmmwrp copy | 149.7 / 143.7 -> 3.9 / 2.6 | 113.9 / 116.7 -> 3.00 / 3.46 | same |
| file 1, direct WinMM (v2) | 3.7 / 3.3 | 3.06 / 3.53 | same |
| file 1, standard WinMM | 11,124 / 11,124 -> 11,108 / 11,106 | 11,011 / 11,123 -> 10,995 / 11,118 | 0.051 -> 0.054 |
| file 2, 2:13-2:18, KDMAPI | 2,087.6 / 1,912.4 -> 3.5 / 3.3 | 1,848 / 1,863 -> 1.60 / 2.57 | 3.30 -> 3.30 |
| file 2, 2:13-2:18, winmmwrp copy | 1,845.6 / 1,838.6 -> 2.9 / 2.5 | 1,882 / 1,897 -> 1.81 / 2.94 | same |
| file 2, 2:13-2:18, direct WinMM (v2) | 4.5 / 2.9 | 2.13 / 3.22 | same |
| file 2, 2:13-2:18, standard WinMM | 14,429 / 19,018 -> 14,417 / 18,845 | 14,280 / 14,426 -> 14,290 / 14,416 | 0.054 -> 0.056 |
| file 2, 3:15-3:30 (93.8 million events), KDMAPI | 18,874.7 / 18,537 -> 3.9 / 9.7 | 18,459 / 18,662 -> 2.16 / 4.01 | 3.71 -> 6.26 (the passage's own rate) |
| file 2, 3:15-3:30, winmmwrp copy | 17,534 / 18,676.7 -> 15.5 / 13.0 | 17,131 / 17,312 -> 5.15 / 7.50 | 3.92 -> 6.26 |
| file 2, 3:15-3:30, direct WinMM (v2) | 14.9 / 17.4 | 6.13 / 9.03 | 6.26 |
| file 2, 3:15-3:30, standard WinMM | 23,052 / 34,175 -> 23,047 / 34,129 | 22,790 / 23,053 -> 22,787 / 23,050 | 0.053 -> 0.054 |

The tool's Maximum lag figures differ from the players' by a few percent because the plays are separate; the tool's loads are in the loading table above.

### Generated fixtures (6:02-6:29 AM, results `20261016-060155` and `20261016-062923`; Balanced, quiet, single plays)

Controller messages 20-31, as in the earlier sections. "Delivered" = million events per second actually sent from first to last send. v2's null-output "over capacity" result of 25.0 equals the file's own rate (25 million events per second), so it is a lower bound for v2's engine, not its ceiling.

**Throughput (fixtures far over capacity), million events per second:**

| path | v1 | v2 |
|---|---|---|
| null (engine only) | 5.82 | 25.0 (the file's own rate; not saturated) |
| KDMAPI | 4.04 | 24.4 |
| winmmwrp copy | 4.19 | 18.5 |
| direct WinMM | not available | 17.6 |
| standard WinMM (clustered file at 1 million per second) | 0.053 | 0.057 |

Raw send ceilings (no engine, the output call alone, million calls per second): KDMAPI v1 10.7 against v2 44.9; winmmwrp v1 13.1 against v2 37.1; direct WinMM (v2) 36.9; null output v1 88.7 against v2 219.9; standard WinMM v1 0.056 against v2 0.057.

**Lateness at 1 million events per second (clustered, 3 s), 99th percentile / maximum in microseconds:**

| path | v1 | v2 |
|---|---|---|
| null | 13,050 / 15,338 | 791 / 1,903 |
| KDMAPI | 1,980 / 3,987 | 788 / 1,167 |
| winmmwrp copy | 1,941 / 2,135 | 790 / 1,056 |
| direct WinMM | not available | 793 / 1,835 |
| standard WinMM | 8,617,180 / 8,696,881 | 8,058,403 / 8,147,515 |

**At 9.5 million per second (uniform):** v1 delivers 5.80 (null), 4.09 (KDMAPI) and 4.21 (winmmwrp) and falls 1.9-3.9 s behind (p99); v2 delivers the full 9.50 on all four paths with p99 54 (null), 257 (KDMAPI), 241 (winmmwrp) and 205 (direct) microseconds and a maximum of 0.5-1.2 ms.

**At a share of each path's own measured capacity (clustered, 3 s), p99 lateness in microseconds, v1 -> v2** (capacity from the over-capacity play: null 5.82, KDMAPI 4.04, winmmwrp 4.19 million per second, the lower of the two programs):

| load | null | KDMAPI | winmmwrp copy |
|---|---|---|---|
| 50 % | 14,239 -> 776 | 2,097 -> 797 | 2,081 -> 788 |
| 80 % | 15,847 -> 784 | 3,941 -> 2,534 (one maximum of 15.6 ms) | 2,868 -> 803 |
| 95 % | 23,871 -> 804 | 11,503 -> 811 | 85,486 -> 826 |
| 115 % | 500,032 -> 776 (v2 delivered 6.70) | 319,444 -> 830 (v2 delivered 4.64 against v1 4.20) | 401,542 -> 826 (v2 4.81 against v1 4.24) |

v2's direct WinMM path (capacity 17.6 million per second), v2 only: p99 at 50 % (8.8 million per second) 1,117 microseconds; at 80 % (14.1) 40,794; at 95 % (16.8) 187,836; at 115 % (20.3) 579,155. In these single plays its sustained limit therefore lies between about 10 and 14 million events per second; nothing was investigated further.

**Direct WinMM against standard WinMM, OmniMIDI** (the Windows 10 system path delivers 0.05 million per second whatever the fixture): direct delivers the full 1.000 million per second on the clustered 1-million file (p99 793 microseconds, maximum 1.8 ms) and 9.501 on the uniform 9.5-million file (p99 205 microseconds, maximum 1.2 ms). A second OmniMIDI run (`20261016-041111`, taken while VirtualMIDISynth was busy; essentially the same): standard WinMM 0.051 whatever the fixture (p99 7.1 s on clustered-1M, 9.2 s on uniform-9.5M); direct clustered-1M 1.000 (p99 864 microseconds, maximum 1.1 ms), uniform-9.5M 9.501 (p99 316 microseconds, maximum 0.7 ms), and at 50/80/95/115 % of the null ceiling (2.0/3.2/3.8/4.6 million per second) p99 0.86-0.89 ms, maximum 1.5-2.2 ms.

**VirtualMIDISynth #1 (results `20261016-042045`; VirtualMIDISynth busy; limited by the synth, compatibility only):** direct access delivered 1.000 on clustered-1M but only 4.05 on uniform-9.5M (p99 4.0 s) and 3.996 at 115 %. No conclusion is drawn.

The earlier "section 1" and "section 3" tables (under load, v1 against v2-spinwait and v2-precise) are **superseded** for v1 against the current v2 by the numbers above; they remain as the record of the intermediate builds and of the cause of the quantized sound.

### Power-plan comparison

The three plans, set with `powercfg /setactive` and read back; the Windows "processor performance" counter was read 15 s after each switch: Balanced 146 % (about 4.67 GHz), subtrottle 73-74 % (about 2.35 GHz), Throttle 25 % (about 0.80 GHz). The plan was set back to Balanced after each comparison (4:04:35 AM and 6:56:58 AM, both verified). Nothing else was changed.

**Part A: user file 1 passage 2:03-2:06 (2.46 million events in 3 s), already loaded, started at the passage; the players' Maximum lag** (4:00-4:04 AM; no drops; every play reached full speed over the passage; v1's 136-188 % speed on KDMAPI is catch-up after falling behind):

| plan | None: v1 | None: v2 | KDMAPI: v1 | KDMAPI: v2 |
|---|---|---|---|---|
| Balanced | 52.8 ms | 2.3 ms | 130.3 ms | 2.7 ms |
| subtrottle | 285.3 ms | 3.2 ms | 624.8 ms | 5.2 ms |
| Throttle | 1,570.6 ms | 6.3 ms | 2,184.4 ms | 31.5 ms |

**Part B: generated fixtures, engine only (null output), v1 and v2 side by side** (6:38-6:57 AM, results `20261016-063850` Balanced, `20261016-064341` subtrottle, `20261016-064913` Throttle; one play per fixture; CPU load before the runs 9.6 / 10.4 / 13.0 % median). The relative loads are shares of the lower of the two programs' own measured null ceiling on that plan: 5.58, 2.88 and 1.04 million per second. "Delivered" in million events per second; lateness in microseconds (p99 / maximum).

| fixture | measure | Balanced v1 | Balanced v2 | subtrottle v1 | subtrottle v2 | Throttle v1 | Throttle v2 |
|---|---|---|---|---|---|---|---|
| over capacity | delivered | 5.58 | 25.0 (file's rate) | 2.88 | 25.0 (file's rate) | 1.05 | 11.9 |
| over capacity | p99 / max | 3,452,009 / 3,484,678 | 948 / 2,564 | 7,595,691 / 7,679,461 | 3,731 / 6,026 | 22,706,165 / 22,937,774 | 1,090,227 / 1,101,161 |
| uniform 9.5 M/s | delivered | 5.62 | 9.50 | 2.92 | 9.50 | 1.02 | 9.51 |
| uniform 9.5 M/s | p99 / max | 2,054,520 / 2,073,202 | 92 / 559 | 5,436,201 / 5,491,386 | 76 / 1,243 | 7,280,727 / 7,352,064 | 762 / 1,121 |
| clustered 10 M/s | delivered | 5.49 | 10.00 | 2.90 | 10.00 | 1.05 | 10.00 |
| clustered 10 M/s | p99 / max | 2,448,217 / 2,468,908 | 774 / 2,067 | 5,580,476 / 5,650,913 | 838 / 1,426 | 7,076,769 / 7,148,235 | 3,355 / 4,303 |
| 50 % of ceiling | p99 / max | 15,059 / 16,859 | 782 / 1,723 | 13,692 / 16,705 | 751 / 1,766 | 1,076 / 7,188 | 1,107 / 3,413 |
| 80 % of ceiling | p99 / max | 15,955 / 17,111 | 782 / 1,182 | 14,060 / 17,056 | 775 / 1,264 | 3,820 / 8,669 | 888 / 1,367 |
| 95 % of ceiling | p99 / max | 22,199 / 24,093 | 773 / 1,831 | 6,208 / 7,193 | 911 / 1,482 | 6,427 / 8,642 | 849 / 1,246 |
| 115 % of ceiling | delivered | 5.53 | 6.42 | 2.97 | 3.31 | 1.05 | 1.20 |
| 115 % of ceiling | p99 / max | 481,875 / 486,791 | 768 / 1,141 | 348,744 / 350,180 | 779 / 1,223 | 447,383 / 451,495 | 816 / 1,434 |
| raw send call, null output | million per second | 87.3 | 203.5 | 45.0 | 123.9 | 15.5 | 42.1 |

How to read it: with the processor slowed, v1 delivers at most 2.9 (subtrottle) and 1.0 (Throttle) million events per second, so every dense fixture runs seconds behind. v2 still delivers the full 9.5-10 million per second of the denser fixtures on both slow plans, and its 99th percentile stays under 1 ms on the 9.5-million-per-second file and on the relative loads (except 1.1 ms at 50 % on Throttle, and 3.4 ms for the 10-million-per-second clustered file at 25 % clock). The 25-million-per-second file, which no run can keep up with, ends 3.7 ms behind on subtrottle and 1.09 s behind on Throttle for v2 (v1: 7.6 s and 22.7 s). At the lightest load on Throttle (50 %) v1 and v2 are about equal (v2 had 36 stalls over 1 ms against v1's 3, and its play showed 59 % CPU used by other processes, so that cell is noisy). The loads are relative to the slower program's own ceiling, so v2 was not stressed at them; the three "delivered" rows are the cleaner comparison.

### Engine throughput, quiet (interleaved baseline against current, DD-065 to DD-067)

`v2\tools\throughput-measure.ps1`, outputs that make no sound, 15 interleaved plays per program, Balanced, CPU load before the runs 8.6-13.4 %, results `throughput\results\20261016-052416-quiet-x64`, `...-052958-quiet-x86` and `...-053629-late-recheck`. Baseline = the engine before DD-065 to DD-067 (commit 70c8e50). Million events per second, medians:

| fixture (null output unless stated) | x64 baseline | x64 current | x64 factor | x86 baseline | x86 current | x86 factor |
|---|---|---|---|---|---|---|
| 12 million events due at once (Queue) | 17.28 (57.9 ns each) | 74.70 (13.4 ns) | 4.32 | 9.63 | 52.10 | 5.41 |
| same, Drop mode | 15.37 | 65.78 | 4.28 | 7.98 | 48.66 | 6.10 |
| same, counting output | 17.32 | 73.87 | 4.26 (identical message hash) | 9.44 | 47.74 | 5.06 |
| 3.2 million events, recording output | 16.75 | 63.53 | 3.79 | 9.07 | 42.27 | 4.66 |

Lateness (p99 / maximum, microseconds, baseline -> current): at 1 million per second on x64 782 / 1,308 -> 795 / 1,220 (equal); at 9.5 million per second on x64 1,285 / 2,012 -> 787 / 1,218. On x86 the baseline could not keep up with 9.5 million per second (delivered 9.02, p99 80.9 ms) while the current build delivered 9.50 with p99 777 microseconds. A re-check of the x64 9.5-million fixture (it started at 22 % PC load, so its own file labels it "under load") gave 1,186 -> 789 microseconds. The "Measured at the time of the change" paragraph above (under load) is **superseded** by these quiet figures.

Engine timing properties (`engine-properties.ps1 -Repeats 3`): 528 of 528 runs finished with no process failure, and every printed property passed in all four groups; the report then stopped on a property with no entry (fixed in the tool). A re-check with `-CheckOnly` (7:08 AM) passed everything except K01 "not exercised" (it needs at least 5 samples per side); it is to be repeated with `-Repeats 5`, on the "subtrottle" plan.

### The display-refresh cost (about 5:53 AM, `refresh-pacing-measure.ps1`, x64 player on the test monitor)

60 Hz monitor, 5 runs of 14.5 s each, the 8-million-event fixture playing on output None, Balanced, CPU load before 8.7-17.1 %, clock 139-146 %. Medians, with ranges:

| scheme | updates in 14.5 s | mean interval | p99 interval | maximum interval | UI-thread CPU | UI time per update |
|---|---|---|---|---|---|---|
| old plain 16 ms timer | 575-594 | 25.6 ms (p50 30.5) | 40.5 ms | 41.6-46.7 ms | 187.5 ms (94-359) | 0.15 ms |
| display pacing (v2 default) | 899-900 | 16.667 ms | 17.5 ms | 17.9-23.8 ms | 718.8 ms (688-859) | 0.086 ms |
| display pacing capped at 60 | 899-900 | 16.667 ms | 17.5 ms | 18.6-24.7 ms | 843.8 ms (734-922) | 0.085 ms |
| precise 60 per second timer | 899-900 | 16.666 ms | 17.6 ms | 19.7-24.3 ms | 421.9 ms (359-516) | 0.078 ms |
| precise 144 per second timer | 2,157-2,162 | 6.947 ms | 7.7 ms | 11.6-15.4 ms | 562.5 ms (359-1,188) | 0.067 ms |

On a 60 Hz screen display pacing and the 60-per-second timer are equally smooth (p99 within 0.1 ms) and the cap changes nothing. The UI thread's cost with display pacing is about 700-850 ms in 14.5 s: about 5-6 % of one logical processor and under 0.5 % of the PC. The old plain 16 ms timer, measured here, ticked about 40 times a second in uneven steps (mean 25.6 ms, p99 about 40 ms), fewer than the 64 per second the first description assumed. Recommendation recorded for the user: no 60 Hz cap (the cost is trivial); this goes to the user as a question in the handoff. A 144 Hz monitor was not available.

### Start-up and size (about 5:45-5:50 AM, `footprint-measure.ps1`, quiet, Balanced)

Player start-up, visible on the test monitor (opened there with the new test option `--position`), 7 runs after one warm-up, medians (ranges for the total). v1's start-up was not measured in this session.

| build | launch to program entry | entry to window shown | entry to first idle | total, launch to first idle | working set | private memory at idle |
|---|---|---|---|---|---|---|
| x64 | 60.0 ms | 109.3 ms | 111.7 ms | 173.7 ms (162.0-191.5) | 18.0 MiB | 4.3 MiB |
| x86 | 73.3 ms | 118.8 ms | 120.8 ms | 195.6 ms (189.1-210.8) | 17.8 MiB | 4.3 MiB |
| xp (SSE2) | 68.7 ms | 119.6 ms | 122.7 ms | 194.9 ms (182.2-208.1) | 17.8 MiB | 4.3 MiB |
| xp-ia32 | 70.5 ms | 114.0 ms | 116.1 ms | 190.4 ms (172.0-206.7) | 17.7 MiB | 3.9 MiB |

Load flow as the player runs it (8-million-event fixture, inspection pass forced; 7 runs): x64 619 ms (inspection 139, parse 480), x86 867 ms, xp 1,014 ms, xp-ia32 1,034 ms; peak private memory 219-220 MiB in all four. Executable sizes (KiB, `midibn-win32` / `midibn-perf`): x64 1,669 / 728; x86 1,457 / 619; xp 1,557 / 730; xp-ia32 1,548 / 725 (these builds also contain the later features, which is why they are larger than at DD-059's measurement). The IA32 against SSE2 figures are in `XP_COST_LOG.md`.

### Direct WinMM stress: the watchdog findings (pre-fix; the fix is pending as DD-094)

The stress run opens the driver, floods it, then stops, closes or panics during the flood, 200 cycles per mode; a 30 s watchdog ends the process if no cycle finishes.

| run | driver | stop | close | panic |
|---|---|---|---|---|
| 4:11-4:26 AM (VirtualMIDISynth busy) | VirtualMIDISynth #1 (limited by the synth) | 200 of 200, open about 1.4 ms | 200 of 200 | 200 of 200 |
| 4:11-4:26 AM (VirtualMIDISynth busy) | OmniMIDI | **hung: watchdog fired, exit 3** | 200 of 200 (open mean 136-150 ms, maximum about 310 ms) | 200 of 200 |
| 6:02-6:29 AM (quiet) | OmniMIDI | **watchdog fired 6:23:49, exit 3** | **watchdog fired 6:24:51, exit 3** | **watchdog fired 6:25:40, exit 3** |

The watchdog worked every time (no process was left behind). With the code as it was (before the fix) the hang on OmniMIDI is therefore frequent, not rare: one of three modes in the first run, all three in the second. The fix (DD-094, helper P9-DIRECT) must be confirmed on the real driver after the merge; the real-driver reproduction stays with the main session. The single-instance guard and the forced exit of the Windows 11 check package were confirmed on the real drivers at 4:05 AM (see the earlier section; the package is ready to send).

### Real-output end-to-end player tests (Milestone 6's closing condition)

4:06-4:10 AM, windows visible on the test monitor, Balanced, real drivers, one program at a time (results `e2e\20261016-040612...` to `...040933`): x64 on OmniMIDI through standard WinMM 98 checks passed; x64 through KDMAPI 98; x86 on OmniMIDI 98; x64 on OmniMIDI with direct WinMM 99 (status line "Direct WinMM driver access: OmniMIDI.dll (tested driver)"); x64 on VirtualMIDISynth #1 with direct WinMM 99 ("...VirtualMIDISynth.dll (tested driver)"). All pass.

### The user's impressions, next to the numbers

The user's impressions from trying the v2 preview, and the figures measured in this session:

| impression | measured | comparison |
|---|---|---|
| Loading about 16 times faster | 19 times (file 1), 29.5 times (file 2), 38.7 times (file 3) in the tool; about 25 and 23 times in the players (files 2 and 3); 11 times on the 8-million-event fixture | The impression sits inside the measured range (11 to 39 times) and is on the cautious side for the big files. The factor grows with file size. |
| About 5 times less memory | 2.0 to 3.2 times less peak memory on the user's files (3.2, 2.6 and 2.0 times); 3.5 times on the 8-million-event fixture; the players' free-memory difference on file 3 was about 2.2 times (22 GB against 10 GB used) | Measured is smaller than the impression: v2 uses about one half to one third of v1's memory here, not one fifth. No measured case reached 5 times. |
| About 4 to 5 times faster playback | The engine's events per second: 4.3 times (x64) and 5.4 times (x86) against the previous v2 engine, and at least 4.3 times against v1's engine on the null output (25.0, the file's own rate, against 5.8); the heaviest passage of file 2 ran at 17-27 % speed in v1 and 100 % in v2 (3.7 to 5.7 times); file 3's passage at 6.5 % against 99.7 % (15 times) | The impression matches the engine and the heavy passages. On light passages both play at 100 % speed, so the difference there is the lag (tens to hundreds of ms in v1, 2-3 ms in v2). |
| Direct WinMM driver access was surprisingly effective; not as fast as winmmwrp or KDMAPI, but somewhat close | The system WinMM path delivers 0.05 million events per second; direct access delivers 17.6 million per second through the whole engine, against winmmwrp 18.5 and KDMAPI 24.4; raw send call 36.9 million per second against 37.1 (winmmwrp) and 44.9 (KDMAPI); heavy passage of file 2, maximum lag 14.9 ms (direct), 15.5 ms (winmmwrp) and 3.9 ms (KDMAPI); the check program measured 41.6 million messages per second direct against 0.053 standard | The ordering is exactly as perceived. Direct access is about 330 times faster than the system path (through the whole engine), within about 5 % of winmmwrp, and about 28 % (whole engine) or 18 % (raw call) below KDMAPI. "Somewhat close" is accurate, and for winmmwrp it is closer than that. |

## Tenth round (2026-10-17): direct send cost, device kept open, thread report, large addresses, one or two processors

Real-driver runs were made by the main session on this PC (Windows 10, busy-ish, overnight power plan "subthrottle"), silent messages only. Runs on a fake or no-op driver were made on the same PC.

### Direct send cost (no device; DD-110)

`midibn-direct-check --fake-driver` sends through the real `WinMmDirectOutput::send_short` into an in-process no-op driver. Why the prompt 009 package sent slower on the XP PC: 182 ns per direct send (prompt 008 package) against 410 to 442 ns (prompt 009 package) is the two locked processor operations DD-094 added around every send, not the driver or the measurement. After DD-110 the same send costs one locked operation, and none inside a burst. ns per send on this PC, XP IA32 build: prompt 008 code 19-20, prompt 009 code 57-63, now 31-33 one at a time and 20-21 in a batch of 256; x64 17 / 50 / 25 / 16. The check tool's real-device speed figure now sends in batches of 256 (as the engine sends a burst), so it is comparable with the prompt 008 figure. The first XP run of the new package should show about 185 ns (batched) and about 300 ns (single, no-op driver).

**After the lean send (DD-131; prompt 012, this PC, no device).** `midibn-direct-check --fake-driver --seconds 0.4`, median of 5 interleaved runs (each itself the median of 3-7 rounds), before = `send_short` as the engine called it, after = `try_send_short` as the engine now calls it (a send builds a status object only when it fails):

| build | one send at a time | burst of 256 (what the engine does) |
|---|---|---|
| x64 | 11.7 -> 9.4 ns (-2.3) | 7.8 -> 5.7 ns (-2.1, -27 %) |
| XP IA32 | 14.2 -> 11.3 ns (-2.9) | 9.2 -> 6.1 ns (-3.1, -34 %) |

The bare call of the no-op driver costs 1.9 ns (x64) and 2.4 ns (IA32); the wrapper cost per burst send fell from 5.9 to 3.8 ns (x64) and from 6.8 to 3.7 ns (IA32). The old `send_short` entry itself is now about 1 ns (single) to 2 ns (burst) slower because it goes through `try_send_short`; the engine no longer uses it for bursts (only panic and tools). The figures differ from the table above because they were taken on a different day and load (this PC is not quiet). On the real Pentium 4 the saving should be larger (the scan estimated 20-30 ns); `v2-xp-direct-check-ia32.exe --fake-driver` prints the new `lean_ns` and `lean_batch_ns` next to the old figures. Baseline from the real XP run of the prompt 011 package (before the change): one send at a time 117.7 ns, bursts 79.3 ns, wrapper 94.2 / 55.8 ns, bare call 23.5 ns.

### Keeping the device open across Stop (DD-111)

`run-device-measurements.ps1 -StressOnly -StressKeepOpen`: 200 open, flood, Stop (or panic) cycles on ONE output object that stays open, as the player now does in Direct mode. (Prompt 009, closing and reopening every cycle: OmniMIDI's close hung after 2 to 198 cycles.)

| Driver | Mode | opens answered by the open device | times the driver stopped answering | Stop / panic, longest | final close |
|---|---|---|---|---|---|
| OmniMIDI | Stop | 199 | 0 | 2.6 ms | 4.3 ms |
| OmniMIDI | panic | 199 | 0 | 11.1 ms | 1.8 ms |
| VirtualMIDISynth | Stop | 199 | 0 | 12.0 ms | 12.1 ms |
| VirtualMIDISynth | panic | 199 | 0 | 32.5 ms | 20.5 ms |

The player's Short scenario (Play, Stop, Play, Stop) in Direct mode passed on OmniMIDI and on VirtualMIDISynth.

### Threads in the player's process (`midibn-thread-report`; DD-113)

| Source | Threads created in the player's process |
|---|---|
| OmniMIDI (standard WinMM, KDMAPI and direct alike) | its OmniMIDI.dll worker (Normal at open, raised to Time Critical while playing); two BASS.dll threads at Time Critical; one BASS.dll thread Above Normal; a BASSWASAPI thread at priority 13; two BASSMIDI threads Normal |
| VirtualMIDISynth | one thread at Lowest (its synthesizer runs in its own service process) |
| v2's own | the playback worker ("MIDI bottleneck scheduler") and the display pacer, both Above Normal; none Time Critical |

Conclusion: the Time Critical threads seen on the Windows XP test PC are OmniMIDI's (its audio library). Direct mode adds no thread; its extra load is only the higher send rate. OmniMIDI's author has abandoned the project, so there is no one to report a close hang to; v2 works around it (DD-094).

### Large addresses (`midibn-large-address-probe`, x86 build; DD-114)

The probe fills the low 2 GB, then sends controllers 121 and 123 and a GM System On SysEx from a buffer at 0x8FFF0000 (silent). All PASS: OmniMIDI standard WinMM, VirtualMIDISynth standard WinMM, OmniMIDI direct, VirtualMIDISynth direct, OmniMIDI KDMAPI; 46 to 245 program allocations above 2 GB while the output was in use, 0 below. The x86 player with a generated silent 104,000,000-event file (memory warning "3.10 GiB", Continue): the Short scenario passed on OmniMIDI standard WinMM, KDMAPI and Direct (21 checks each).

### One or two logical processors (DD-112; about 4:01-4:13 AM EDT; a Windows 10 PC with 12 logical processors, not quiet)

x64 build, process restricted to logical processor 1 (mask 1) or 1+2 (mask 3, the two hyper-threads of one core), `midibn-few-cpu-measure`: real engine and clock, output None with a 300 ns busy cost per message, a silent fixture of 15 million controller events in 5 s (so the worker is always behind), a stand-in Time Critical "audio" thread busy 0 % or 30 % of every 10 ms, a stand-in Normal "window" thread wanting to run 60 times a second. 3 interleaved rounds, medians. Lag = how much later than 5 s the song ended.

| processors / audio | option | lag (s) | M events/s | window wake delay p50 / p99 / max (ms) |
|---|---|---|---|---|
| 1 / 0 % | off | 3.5 | 1.76 | 0.2 / 12 / 5,497 |
| 1 / 0 % | Normal threads | 4.9 | 1.52 | 38 / 99 / 168 |
| 1 / 0 % | yield 2 ms | 4.0 | 1.66 | 1.9 / 41 / 108 |
| 1 / 0 % | nap 20 ms | 4.6 | 1.56 | 12 / 51 / 611 |
| 1 / 30 % | off | 7.0 | 1.25 | 0.5 / 4.8 / 4,871 |
| 1 / 30 % | Normal threads | 8.4 | 1.12 | 61 / 141 / 219 |
| 1 / 30 % | yield 2 ms | 7.7 | 1.18 | 2.5 / 11 / 110 |
| 1 / 30 % | nap 20 ms | 8.9 | 1.08 | 13 / 40 / 762 |
| 2 / 0 % | off | 1.7 | 2.23 | 0.4 / 15 / 62 |
| 2 / 0 % | Normal threads | 1.9 | 2.19 | 0.4 / 20 / 32 |
| 2 / 0 % | yield 2 ms | 1.8 | 2.19 | 0.3 / 11 / 32 |
| 2 / 30 % | off | 1.9 | 2.18 | 0.6 / 44 / 125 |
| 2 / 30 % | Normal threads | 2.4 | 2.02 | 0.6 / 36 / 65 |
| 2 / 30 % | yield 2 ms | 2.4 | 2.04 | 0.6 / 17 / 66 |

Findings: (1) On ONE processor with the worker behind, the Above Normal worker starves Normal threads for about 5 seconds at a time (the "sluggish Windows" symptom): the stand-in window thread's longest delay was 4.9-5.5 s. A Time Critical thread on the same processor halves throughput simply by taking 30 % of it. (2) `yield 2 ms` cuts the longest delay to about 110 ms for 10-15 % more lag while behind; Normal priority also fixes it but costs 20-40 %; nap is worse than yield. (3) On TWO logical processors nothing starved (longest delay 62-125 ms without any option) and every option only costs throughput; recommendation: restrict any option to exactly one usable processor. (4) Not measured: a real Hyper-Threaded Pentium 4 and OmniMIDI's real thread. (5) The dense-song fast path bypasses the ordinary send function, so the yield hook is in both.

**Package form, this PC (prompt 012; DD-132).** `midibn-few-cpu-measure --auto` (XP IA32 name `v2-xp-few-cpu-ia32.exe`): its own silent test file (controller messages only, 250,000 a second for 3 s), output None with a counter costing 5 microseconds per message so playback always falls behind (no device, no driver, no sound), the option "Let other programs run while playback is behind" (yield every 2 ms) off against on, with 2 logical processors (the two lowest usable ones) and with 1, in 2 interleaved rounds, medians; 12 logical processors restricted by affinity; 34-35 s in total on x64 and on XP IA32.

| processors / option | behind at end | messages/s | competing thread delay typical / 99 % / longest (ms) |
|---|---|---|---|
| 2 / off | 0.9 s | 190,000 | 0.0 / 11 / 59 |
| 2 / on | 1.0 s | 187,000 | 0.1 / 3 / 18 |
| 1 / off | 1.1 s | 183,000 | 0.7 / 2,867 / 2,867 |
| 1 / on | 1.0 s | 184,000 | 1.9 / 7 / 29 |

(x64; IA32 gives the same picture: 1 processor off, longest 3,596 ms; on, 24 ms.) On one logical processor the option removes the multi-second starvation of ordinary threads at no measurable cost in playback; on two it changes little. This is the same finding as the table above, now in a form the real Pentium 4 can run; the real-PC figure decides, and this PC is not a Pentium 4.

### Adaptive display rate and silent fixtures

The costs of one live update and the tier behaviour (DD-109) are listed in that entry. The silent fixtures (`tools/device/make-silent-fixture.ps1`, written under `v2/out/fixtures-silent/`, never committed) have no Note On: light (2,000 events/s, 45 s), medium (200,000/s, 30 s) and heavy (3,000,000/s, 10 s, 30 M events); `-Rate R -Seconds S` makes one custom file.

## Linux waiter accuracy and thread priority (WSL2; Milestone 8, 2026-10-18; DD-122, DD-123)

Measured in WSL2 (Ubuntu 22.04, kernel 6.18) on the development PC with `midibn-linux-capability` and `midibn-linux-platform-tests`. This is a virtual machine on Windows, not real Linux hardware: real figures are still to be taken by a tester (see the Linux self-test package; the report file `capability-report.txt` has the same numbers).

- **Waiter accuracy** (same split as Windows: a sleep on the wake event until 200 microseconds before the target, then a spin; timer slack of the thread set to 1 ns). The coarse wait returns about **100 to 140 microseconds before** the target, so the spin has the remainder. After the spin the dispatch is a median **0.1 to 0.2 microseconds late**. Single outliers of **50 to 2,000 microseconds** happen now and then; they are scheduler noise of the virtual machine (the bounds in the tests are loose for that reason). The result was the same as an unprivileged user and as root.
- **Clock:** `CLOCK_MONOTONIC`, resolution 1 ns, one read about 30 ns.
- **Thread priority (finding).** "Above normal" is requested as nice -3 (`setpriority` on the calling thread); no real-time class is used. As **root** the request is **granted** (thread nice -3). As an **unprivileged user** (soft limit `RLIMIT_NICE` 0) it is **refused** with `EACCES`; the thread silently stays at nice 0 and playback still works. A user who wants the raise can allow it (`ulimit -e` or `/etc/security/limits.conf`, for example `@audio - nice -5`); that is a matter for the installation notes, not for code. The outcome is recorded per thread and printed in the capability report.
- **Memory:** peak working set is `VmHWM`; "peak private" is `VmPeak`, an upper bound (about 145 MB for a small process, mostly thread stacks and libraries).
- **Not measurable here:** the cost per message of the three ALSA send patterns (the development PC has no `/dev/snd/seq`). Command for a tester: `midibn-alsa-send-check 14:0 --seconds 3` (silent controller messages only); see BUILDING.md.

## Linux self-test package on a tester's real Linux PC (two full runs; recorded 2026-10-24, prompt 012)

A tester ran the Linux self-test package (built 2026-10-18 5:47 AM on Ubuntu 22.04.5 LTS) twice on one PC. Facts only; the failing test is analysed separately. Sources: `selftest-result-full.txt`, `selftest-details.txt`, `capability-report.txt`, `alsa-ports.txt` and `logs.zip` (first run) and `Archive.zip` (second run), read as data only.
- **The PC:** Debian GNU/Linux 13 (trixie), kernel 6.12.113+deb13-amd64, x86_64, glibc 2.41, AMD Ryzen 5 5500 (6 physical cores, 12 logical processors, all 12 usable by the process), 32,659,780 kB of memory, ALSA library 1.2.14, `/dev/snd/seq` present and readable and writable.
- **First run:** started 2026-10-22 4:40:24 PM, 76 s in total. **Second run** (after the tester installed Qsynth for FluidSynth): started 9:06:15 PM, 74 s in total. Both: OVERALL FAIL, with identical outcomes line for line.
- **Counts as the files show them:** 13 lines in the result: 10 PASS, 1 FAIL, 2 "done" (the capability report and the port list, which are information and not part of OVERALL). That is 10 of the 11 real checks passing, not "12 of 13". The ten passes: core (54 checks), display, channels (129,741 checks), main window (113), analysis (9,137), routing (7,373 checks; 736 sessions, 6,813 dialog states compared with v1), engine (387), Linux platform (149), v2 baseline and engine runs (traces 55, flows 110, models 9, all passed, 0 failed; 38 s first run, 37 s second), engine harness (23 fixtures, 3,956 runs, all identical, 761 explained by rules, 0 failed).
- **The one failure:** `unit-tests-alsa-output` (exit code 1, 0 s): "FAIL loopback: the four events arrive in order with the right types", 158 checks, 1 failure. The test prints nothing about what arrived. Identical in both runs; the open and the sends succeeded in the other 157 checks. This test cannot run in WSL (no `/dev/snd/seq`).
- **Timing (capability report, first run; the second differs only in the memory figures):** `CLOCK_MONOTONIC` resolution 1 ns, one clock read 21.9 ns. A 2,000 microsecond wait is on average 0.1 microseconds late (maximum 0.1); a 500 microsecond wait 0.0 late (maximum 0.1). The platform tests show the coarse wait returning 144 microseconds early (second run: 142 for the 2,000 microsecond wait), then the spin: median 0.0 (first run) or 0.1 microseconds late (second run), maximum 25.2 (first run) or 0.1 (second run) for the 500 microsecond wait. Timer slack of the main thread 50,000 ns; threads the player starts set 1 ns. Event wait of 20 ms took 20.06 ms; a 5 s wait cut by a signal returned after 30.0 ms.
- **Thread priority:** `RLIMIT_NICE` 0, user id 1000 (unprivileged). The request for above normal (nice -3) is **refused** and the thread stays at Normal (nice 0), as predicted in the WSL section above. Playback still works.
- **Memory:** capability report peak resident 3,158,016 bytes (first run) and 3,309,568 (second), peak virtual 148,439,040 and 148,434,944 bytes; the platform tests report a peak resident of about 22.7 to 22.8 MB.
- **ALSA ports:** both runs list one port only, `14:0 Midi Through: Midi Through Port-0 (14:0)`. In the second run FluidSynth/Qsynth was installed but offered no input port while the test ran, so no real synthesizer was reached. No GUI run is shown in either set of files.
- **Not measured:** the cost per message of the ALSA send patterns (`midibn-alsa-send-check` is not part of the package's result).
- **Follow-up (prompt 012, DD-133):** the loopback test of `midibn-alsa-output-tests` now prints every event that arrived and judges only the events whose source is the player. Cause of the failure above: not yet confirmed; the tester's next run shows what arrives in `logs/unit-tests-alsa-output.txt`. The test package was rebuilt with the player first (DD-134).
