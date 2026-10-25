# Audible tests: what to run, in what order (main session only, after the user's yes)

Nothing here changes any audio setting (no mute, no volume, no device switch). Everything that can make sound is
announced to the user in chat first and started only by the main session. Run from the repository root in Windows
PowerShell. Every script has a silent form (`-DryRun`, or `-None` for the player test); run that first.

Needs: the builds (`.\v2\tools\build-player.ps1`, `-Build x86`; `.\v2\tools\device\run-device-measurements.ps1 -DryRun -Build`
builds the measurement tool and the v1 driver), and a quiet PC (below 20 % CPU; the measurement script waits for that itself).
The winmmwrp path needs the path of the winmmwrp `winmm.dll`, given with `-WinmmWrp <path>` or the environment variable `MIDIBN_WINMMWRP_DLL` (there is no default; the DLL is not part of the repository; it is copied only into a git-ignored
private folder under `v2\out\device-test\`, deleted again at the end).

## 0. Silent rehearsal (no sound, about 8 minutes in all)

| command | time | prints |
|---|---|---|
| `.\v2\tools\device\run-device-measurements.ps1 -DryRun -Paths null -Fixtures uniform-9M,clustered-10M,overcap` | 3-5 min | `DRY RUN: null path only`, a START/END line per test with CPU, clock and busiest processes, `results written to ...` |
| `.\v2\tools\device\listening-clips.ps1 -DryRun -Pause 1` | 1.5 min | six `CLIP n of 6: ...` lines (null output), `LISTENING-CLIPS: DONE 6 clips (dry run, no sound)` |
| `.\v2\tools\device\player-e2e.ps1 -None` | 1 min | about 97 `PASS` lines, `PLAYER-E2E: PASS` |
| `.\v2\tools\device\player-e2e.ps1 -None -GateSelfTest` | 20 s | proves the fail-closed output gate; presses Play never |
| `.\v2\tools\monitor-launch.ps1` | 3-4 min | proofs, launch and companion windows for x64, x86, xp, xp-ia32; `MONITOR-LAUNCH: PASS` |

## 1. Listening clips (about 3.5 minutes with `-Pause 20`; sound)

Announce to the user first: six clips of 12 s; each pair is the same passage on v1 then v2 through one path.

```
.\v2\tools\device\listening-clips.ps1 -Pause 20
```

Prints, before each clip and before its wait, one line, for example `CLIP 3 of 6: v1 - OmniMIDI via KDMAPI` (with a long dash).
Order: 1 v1 / 2 v2 system WinMM, 3 v1 / 4 v2 KDMAPI, 5 v1 / 6 v2 WinMM with winmmwrp. Announce each clip in chat when its
line appears; the clip starts 20 s later. For control of the exact moment use `-GoFile C:\path\go.txt` instead (the script waits
until that file exists, deletes it, then plays; create it after announcing). Without either option the script waits for Enter in
its console (it refuses to run when there is no console). Ends with `LISTENING-CLIPS: DONE 6 clips`.
What to hear: the Ode-to-Joy theme (about 3.4 s), then three fast celesta rolls at about 1,000 / 4,000 / 10,000 events per
second (about 1.4 s each, rests between), then the theme's ending. `-Fixture <mid>` plays another file instead (read only).

## 2. End-to-end player test with a real output (about 1 minute each; sound for about 50 s)

Needs `-AllowSound`. The script selects the output, then **re-checks it before every Play and refuses to press Play** if
the box does not show the requested output (the player starts on the first real device otherwise).

```
.\v2\tools\device\player-e2e.ps1 -Device OmniMIDI -AllowSound              # OmniMIDI through the system WinMM
.\v2\tools\device\player-e2e.ps1 -Kdmapi -AllowSound                       # KDMAPI box ticked
.\v2\tools\device\player-e2e.ps1 -Device OmniMIDI -AllowSound -Build x86   # the same with the x86 player
```

Runs on the hidden desktop (nothing appears on any monitor), under v1's native-dialog/process monitor. Drives Play, Pause,
Play, seek (timeline, -5, +5), all eight Rate models live, Queue limit with every Overflow choice, Analysis, Channels, Stop,
Track routing (Cancel; only possible while stopped), Help, About, close. After each step it checks the window answers within
3 s and that no message box exists (any is captured to a picture). Prints `PASS`/`FAIL` lines, the monitor verdict, the
slowest answer time, and `PLAYER-E2E: PASS` or `FAIL` (exit 0 / 1). Pictures and logs: `v2\out\e2e\<stamp>-<build>-<output>-full\`.

## 3. Device measurements (sound only if user files are used; the generated files use controllers 20-31)

```
.\v2\tools\device\run-device-measurements.ps1                       # all four paths, v1 and v2: about 60-75 minutes
.\v2\tools\device\run-device-measurements.ps1 -Paths kdmapi,null    # about 30 minutes
.\v2\tools\device\run-device-measurements.ps1 -Paths winmm-system,winmm-direct -Programs v2 -DeviceName "VirtualMIDISynth #1"   # DD-085 direct driver access (modMessage) against the system WinMM; also the stop/close/panic stress cycles and the list of devices it can drive; run once per synthesizer (OmniMIDI is the default -DeviceName). Close every other MIDI program first.
.\v2\tools\device\run-device-measurements.ps1 -UserFolder <dir> -UserOnly -UserFileLimit 3   # sound: the files are music
```

Plays the data as it is, at its volume. Prints for every test a START line (CPU busy before, processor performance, busiest
processes, `quiet=yes|no`) and an END line (after); the density band is the 9M / 9.5M / 10M uniform and clustered files, then
1M, the over-capacity file, loads relative to each path's capacity, raw ceilings and instrumentation overhead. A user file
takes its own length times (programs x paths x `-UserRepeats`), so choose short pieces. Results:
`v2\out\device-test\results\<stamp>\` (`summary.md`, `runs.csv` with `cond_*` columns, `raw-ceilings.csv`, `env.txt` with the
power plan, `audio-status.txt` (read only), `run-log.txt`). User files appear only as "user file 1..n"; the name map is the
private `v2\out\device-test\user-file-map-<stamp>.txt` (never copy it into a report). If the PC is busier than 20 % for
90 s the run starts anyway and is marked `cond_quiet=no` (add `-StrictQuiet` to stop instead).

## 3b. Final v1-vs-v2 comparison on the user's own files (measurement tools; sound; main session only)

The user's three reference files are read only (never copied, modified, committed or packaged); they appear everywhere as
"user file 1..3" (the private name map is `v2\out\device-test\user-file-map-<stamp>.txt`). The passages are the user's own windows
and are used as given (`1=123-126;2=133-138,195-210;3=60-75`, file number = position in `-UserFiles`, seconds).

What is measured per file, program (v1 / v2, x64), path and passage: events sent, delivered millions of events per second, lateness
p50 / p99 / max, stalls of 1 ms or more, **Maximum lag as the players define it** (the largest lateness of any event sent since
playback started, from the engine itself: the figure the players show as "Maximum lag"), the lag at the moment the passage itself
starts, the effective speed (the players' own 250 ms measure: overall and lowest in the passage), whether the run was stopped by the
time limit, the chase result and the notes sounding at the start. Plus, for every file and program, load time (open to ready to play)
and peak memory (private bytes and working set). Settings of every run: Rate model None, Chase MIDI state on Play/Seek ON (the
players' default; recorded in every row), timing instrumentation recorded in every row (see `-PassageInstr`).

Start modes per passage: **direct** (playback starts exactly at the passage start; with Chase on, the engine first sends the latest
volume, expression, pedal, program and bank of every channel, which the run verifies against the file: `chase_restored_*` in
`user-passages.csv`) and **earlier** (starts `-EarlierBy` = 10 s before it and arrives at it in playback). Defaults: files 1 and 2
run both, file 3 runs earlier only (`-UserModes '1=direct,earlier;2=direct,earlier;3=earlier'`). `-FromStart` (off by default, user
file 2 only, `-FromStartFile 2`) adds one run from 0:00 through the first passage (about 2 min 18 s of sound for 2:13-2:18) on ONE
path per program (`-FromStartPath winmm-wrp`).

Instrumentation (`-PassageInstr count,sample`): `count` = one counter per sent event (the lowest overhead; the headline Maximum lag,
events and speed come from these runs) and `sample` = two timer reads on every 128th event (needed for lateness percentiles and stalls).
Each passage is played once with each, so the effect of the instrumentation on the lag is visible. The statistics are sampled every
16 ms like the players' own display timer.

Paths (`-UserPaths`, default `winmm-wrp,kdmapi,winmm-system,winmm-direct`): data driven. A path the program does not offer yet
(`winmm-direct` until it is added to the tools) is skipped with a note in `summary.md`. `winmm-system` uses a time cap: it stops
after the passage length + 10 s (`-CapExtraSeconds`), reports the lag reached and marks the row `capped=YES`; the other paths are
capped at +120 s (`-DefaultCapExtraSeconds`). The file is loaded once per program and process group and played on every path of the
group in turn: group `system` = kdmapi, winmm-direct, winmm-system (last, because after a flood the driver can hang when closed; the
script then runs the paths not reached in a new process), group `wrp` = winmm-wrp (a private copy of the program beside the
third-party DLL, deleted at the end). So each file is loaded twice per program (the files are loaded smallest first, the biggest last).

Silent first (no device, no sound):

```
.\v2\tools\device\run-device-measurements.ps1 -UserFiles <file1>,<file2>,<file3> -UserPassages '1=123-126;2=133-138,195-210;3=60-75' -ShowPlan
.\v2\tools\device\run-device-measurements.ps1 -DryRun -UserFiles <file1> -UserPassages '1=123-126' -UserPaths kdmapi,winmm-system,winmm-wrp,winmm-direct   # about 6 minutes, null output
.\v2\tools\device\run-device-measurements.ps1 -LoadOnly -UserFiles <file1>,<file2>,<file3>      # load time and memory only: no device
```

The real run (announce first; `-AllowSound` is required, the script refuses to start without it):

```
.\v2\tools\device\run-device-measurements.ps1 -UserFiles <file1>,<file2>,<file3> -UserPassages '1=123-126;2=133-138,195-210;3=60-75' -AllowSound
.\v2\tools\device\run-device-measurements.ps1 -UserFiles <file1>,<file2>,<file3> -UserPassages '1=123-126;2=133-138,195-210;3=60-75' -AllowSound -FromStart   # adds the 2:18 run on file 2
.\v2\tools\device\run-device-measurements.ps1 -UserFiles <file1> -UserPassages '1=123-126' -AllowSound -Arch x86 -Programs v1,v2   # 32-bit builds: only file 1 (files above -X86MaxFileMB 300 are skipped with a note)
```

Results: `v2\out\device-test\results\<stamp>\` with `summary.md` (conditions, load time and memory table, one table per file and
passage and mode), `user-passages.csv` (every run, all columns, `cond_*` conditions: power plan, CPU load before/after, processor
performance, busiest processes, `cond_quiet`), `load-memory.csv`, `run-log.txt`, `env.txt`, and `raw/` (per run key=value files,
`.snapshots.csv` with the lag, speed and position every 16 ms for plotting the lag build-up).

Expected durations (estimates; the PC must be quiet, below 20 % CPU): loads, v1: file 1 about 8 s, file 2 about 110 s (history),
file 3 about 8-12 min (history); v2 x64 is expected to be far quicker (measure with `-LoadOnly` first). Each process also plays
every window for every path twice (count and sample) at real time: file 1 about 3 minutes per process, file 2 about 9 minutes, file 3
about 5 minutes, plus the loads. In all, with four paths, two programs and two loads per file: about 2 hours, dominated by file 3's
four loads (two per program, 8-12 min each for v1).

Measured on 2026-10-15 (PC busy, 64 % CPU, so only indicative): v2 x64 loads user file 2 in 10.5 s with 5.2 GB (private bytes; peak working set 5.2 GB) and finds its 227,108,447 events; v2 loads user file 1 in 0.4-0.7 s (241 MB), v1 in 8-9 s (777 MB). Run `-LoadOnly` in the quiet phase for the real figures.

The native-window monitor that wraps every process watches the whole desktop: a crash window of an unrelated program (for example a Windows Error Reporting window from another tool) makes a job end with "exit code 3" and the script with exit 1 although the measurement itself finished; check `raw/<job>.monitor.jsonl` for which window it was.

Memory warnings (63 GB RAM): v1 needs about 0.03 GB per MB of file (file 2 about 23 GB, file 3 about 50 GB) and v2's 32-bit builds cannot
hold files 2 and 3 (skipped with a note). Only one process runs at a time, but close everything else before file 3 and make sure about
55 GB are free; the script checks free physical memory before every load and skips (with a note) when it is not enough
(`-NoMemoryCheck` overrides). File 3 is always last.

Order recommended: `-LoadOnly` for all three files (silent), the dry run, then the real run for files 1 and 2 first (give all
three paths so the numbering stays, but passages only for files 1 and 2: `-UserPassages '1=123-126;2=133-138,195-210'`; file 3 is then
skipped with a note), and file 3 last in a run of its own (`-UserPassages '3=60-75'`). The script itself always runs the files
smallest first, so a single command with all three passages also ends with the biggest file.
## 4. Watch the real players (sound; main session only)

The user watches and listens while the REAL players (v1 and v2) play heavy passages of their own MIDI files, one after the
other, side by side on one monitor. The user's files are only read; in every output they are "user file N" (the private name
map is `v2\out\device-test\watch\user-file-map-<stamp>.txt`, never copy it).

```
.\v2\tools\device\watch-passages.ps1 -UserFiles <file1>,<file2>,<file3> -UserPassages '1=123-126;2=133-138,195-210;3=60-75' `
    -Paths winmm-wrp,kdmapi,winmm-system -Modes direct,earlier -EarlierBy 10 -AllowSound -AcceptLargeFileWarning -GoFile C:pathgo.txt
```

Silent rehearsal first (hidden desktop, output None, no window on any monitor, about 1-2 minutes per path and passage):
`.\v2\tools\device\watch-passages.ps1 -DryRun -UserFiles <file1> -UserPassages '1=123-126' -Paths kdmapi,winmm-system,winmm-wrp`
ends with `WATCH-PASSAGES: DRYRUN PASS (n steps)` (exit 0) or `DRYRUN FAIL` (exit 1). `-ListMonitors` only lists the monitors.

What it shows: both players are placed without taking focus on the monitor Windows calls "Internal Display" (1920x1080,
`\\.\DISPLAY1`; v1 on the left, v2 on the right, about 940 x 700 each). The primary 4K monitor is never used unless
`-Monitor` names it. Players are started minimized and then placed, so nothing flashes on the other monitor. Files
are taken in order of size, the biggest last. Each file is loaded once per program and process group (system WinMM and KDMAPI
share one load; the winmm-wrp private copies need their own), v1 through its File Open dialog (that dialog is visible for a
moment and may take focus), v2 on its command line. For every passage and start mode the player is put at the passage start (mode `direct`, the default; the players' own "Chase MIDI state on Play/Seek", on by default, restores controllers and programs there) or `-EarlierBy` seconds before it (mode `earlier`, arriving at the passage in playback); use `-Modes direct,earlier` for both; for user file 3 use `-Modes earlier` in a separate command. The position shown by the player is read back (it must be within 1 s; the actual value is logged), and then for every
passage, v1 first and then v2 (never both playing):

```
NOW: v1 — user file 1, passage 2:03–2:06, OmniMIDI via KDMAPI
```

That is one bare line (no time stamp, long dashes), printed after the position is set and before the wait. The main session
announces it in chat; the passage starts `-Pause` seconds later (default 20) or, with `-GoFile <path>`, when that file exists
(the script deletes it, then presses Play; create it after announcing; a stale file is deleted first). The player plays
the earlier part + passage + 2 s and is stopped; then its own statistics ("Maximum lag", "Events sent / dropped", "Effective speed") are
read from its window (the players draw them themselves, so they are read with the Windows text recogniser; a picture goes to
the private folder if that fails) and written to `v2\out\device-test\watch\<stamp>\watch-log.txt` and `watch-results.csv`
(program, path, user file N, passage, start position verified, max lag, events sent, dropped, effective speed, notes).
Paths: `winmm-system` (KDMAPI box off), `kdmapi` (box on), `winmm-wrp` (private copies of both players beside the winmmwrp
`winmm.dll` in `v2\out\device-test\x64-winmmwrp-watch\`, deleted at the end even after an error; PE x64 checked),
`winmm-direct` (not available in these players yet: skipped with a note).

Safety: every output box AND the KDMAPI box are re-read before EVERY Play; if they do not show the requested path, Play is not
pressed and the run stops. Without `-AllowSound` the script refuses to start (a dry run needs none).

Warnings before the real run (announce them):
- Duration and memory (the user's history): file 2 took v1 about 110 s to load and about 23 GB; file 3 took v1 about 8-12 min
  and about 50 GB. The PC has 63 GB RAM; v2 x64 loads are expected to be much lighter. File 3 is always loaded last. Check free
  physical memory first (the script logs it before and after every load, and every minute while loading) and close everything
  else; both programs hold a file at the same time, one process pair per file and process group (so file 3 is loaded in the
  `std` pair and again in the `wrp` pair, one pair at a time, each pair closed before the next).
- Loading time is counted out of `-LoadTimeoutMinutes` (default 60 per program and file).
- v1 shows a "Large MIDI memory warning" for very large files; `-AcceptLargeFileWarning` presses Continue (leave it off to be
  asked first: the run then stops that load with a picture in the private folder).
- A run with all three files, three paths and the passages above plays 2 programs x 3 paths x 5 passages = 30 plays of 10-30 s plus a 20 s wait
  each, plus the loads: plan about an hour for files 1 and 2, much more with file 3.

## Rules that apply to all of it

- Do not start any of this without announcing it to the user first, and never from a nested helper.
- A real device is only ever reached through `-AllowSound` (player test) or by leaving `-DryRun` off (clips, measurements).
- If a run is stopped early, nothing needs restoring: no audio setting was changed.
