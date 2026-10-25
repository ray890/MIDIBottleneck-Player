# Building MIDIBottleneck Player 2.0

This page describes how to build the Windows programs from source and how to run the checks. Run every command from the repository root. Build output goes to `v2\out\` (never committed). Commands that need a real MIDI driver, your own MIDI files or a second PC are marked optional.

## What you need (one-time install)

All from Microsoft, through the Visual Studio Installer or `winget`:

| Component | Visual Studio Installer component ID |
|---|---|
| Visual Studio Build Tools 2022 (17.14 or later) | `Microsoft.VisualStudio.2022.BuildTools` (winget package) |
| MSVC v143 x64/x86 build tools | `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` |
| Windows 11 SDK (10.0.26100) | `Microsoft.VisualStudio.Component.Windows11SDK.26100` |
| C++ CMake tools for Windows | `Microsoft.VisualStudio.Component.VC.CMake.Project` |
| MSVC v141 x64/x86 (VS 2017) — XP build only | `Microsoft.VisualStudio.Component.VC.v141.x86.x64` |
| C++ Windows XP Support for VS 2017 (v141) — XP build only | `Microsoft.VisualStudio.Component.WinXP` |

One command installs all of it (Windows asks for administrator approval once):

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --override "--passive --wait --norestart --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.VC.v141.x86.x64 --add Microsoft.VisualStudio.Component.WinXP"
```

The version 1 reference harness (optional; the expected results are committed) uses the .NET Framework 4.8 C# compiler that ships with Windows, the same one version 1's `build.ps1` uses. Version 1's files are kept in `archive\v1`; the harness scripts under `v2\harness\v1-reference` read them from there. No other tools, libraries, or package managers are used.

## Commands

Run from the repository root in Windows PowerShell. CMake is not on `PATH` by default; this finds the copy installed with the Build Tools:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
```

### 1. v1 reference side (optional — the expected traces are committed)

```powershell
.\v2\harness\v1-reference\build.ps1          # verifies v1 sources equal tag v1.0.44, builds x86 + x64 harness
.\v2\harness\fixtures\generate.ps1 -Large    # regenerates fixtures; -Large also writes the 8M-event file (not committed)
.\v2\harness\make-expected.ps1 -Large        # rewrites expected load/index traces (x64; x86 must agree), flow traces
                                             # (x86 = pointer size 4, x64 = 8), decimal-ops.txt, model traces (models\*.trace;
                                             # x86 must agree), and with -Large the large summaries
```

The tempo-clock decimal vectors (Milestone 1) are produced separately; the large-fixture summary command is shown for reference (`make-expected.ps1 -Large` runs it):

```powershell
.\v2\out\v1-reference\x64\v1-reference.exe dump .\v2\harness\fixtures\large\large-8m.mid --summary --out .\v2\harness\expected\large-8m-mid.summary.trace
.\v2\out\v1-reference\x64\v1-reference.exe decimal-vectors 6000 20261012 .\v2\harness\expected\decimal-vectors.txt
```

### 2. v2 builds

```powershell
& $cmake -S v2 -B v2/out/build-x64 -G "Visual Studio 17 2022" -A x64   -T v143
& $cmake -S v2 -B v2/out/build-x86 -G "Visual Studio 17 2022" -A Win32 -T v143
& $cmake -S v2 -B v2/out/build-xp  -G "Visual Studio 17 2022" -A Win32 -T v141_xp -DMIDIBN_XP=ON   # SSE2: test comparison only
& $cmake -S v2 -B v2/out/build-xp-ia32 -G "Visual Studio 17 2022" -A Win32 -T v141_xp -DMIDIBN_XP=ON -DMIDIBN_XP_IA32=ON   # the XP release build (no SSE2 needed)
& $cmake --build v2/out/build-x64 --config Release
& $cmake --build v2/out/build-x86 --config Release
& $cmake --build v2/out/build-xp  --config Release
& $cmake --build v2/out/build-xp-ia32 --config Release
```

The XP build prints Microsoft's warning MSB8051 ("Support for targeting Windows XP is deprecated"); that is expected.

**XP variants.** `build-xp` uses the compiler's default x86 code generation (SSE2; needs a Pentium 4 / Athlon 64 or newer processor). `build-xp-ia32` (`-DMIDIBN_XP_IA32=ON`, valid only together with `MIDIBN_XP=ON`) compiles everything with `/arch:IA32`: the program's own code then uses no SSE or SSE2 instructions and floating point goes through the x87 unit. Its self-test is named `v2-xp-selftest-ia32.exe` (the SSE2 build's is `v2-xp-selftest.exe`), so both fit in one self-test package. **`build-xp-ia32` is the XP release build** (project decision): the XP player ships only in this form. `build-xp` (SSE2) is kept only as a test comparison: the speed A/B (`tools\ia32-vs-sse2.ps1`) and the second variant of the XP self-test package; it is never released (`docs\XP_COST_LOG.md`). The static C runtime still contains SSE2/AVX code paths in a few places (memory and string functions, some math, floating-point environment), each guarded by a run-time check that Windows reports SSE2 before using it.

Outputs (in `v2\out\build-*\Release\`): `midibn-win32.exe` (the player: open or drop a MIDI file, choose WinMM / KDMAPI / None, Play/Pause/Stop/seek, every Rate model and queue setting, statistics, borderless styles, Application scale, Help; see `docs\UI_DIFFERENCES.md` for the Milestone 6 presentation notes), `midibn-app-tests.exe` (the main window behaviour layer: Rate model state machine, load, transport on None, recording outputs only), `midibn-ui-tests.exe` (formatting and UI mappings against v1-computed tables), `midibn-dump.exe` (writes a trace for one MIDI file; `--flow 4|8` writes its load-flow trace instead; `--models <v1 model trace> --fixtures <dir>` replays a model trace against the v2 models), `midibn-parity.exe` (named `v2-xp-selftest.exe` in the XP build and `v2-xp-selftest-ia32.exe` in the IA32 build; `--has-sse2` prints one line and exits 0 if Windows reports SSE2, 1 if not), `midibn-load-flow.exe` (one load through the player's load flow: the inspection pass a big file gets first, then the parse, each timed; `--force-preflight`), `midibn-perf.exe` (times one load; `--analysis` also times three fixed Analysis runs (`--profile` samples where their time goes, modern builds with symbols only), `--micro` times small simple workloads for build-to-build A/B tests (decoded event reads, 48-byte copies, 64-bit division, double conversion; `tools\ia32-vs-sse2.ps1 -Micro`), `--stages` the load stages, `--engine` plays the song on the engine under the lock-step virtual clock, `--engine-real` on the real services with a 1000-fold faster clock to measure the engine's cost per event), `midibn-tests.exe` (unit tests), `midibn-platform-tests.exe` (timing and threads), `midibn-midi-output-tests.exe` (output backends, no device opened), `midibn-engine-tests.exe` (playback engine under lock-step virtual time with recording outputs, plus a real-time smoke run on the None output; no device opened), `midibn-engine-harness.exe` (the engine comparison: runs the 170 engine scenarios on 23 fixtures under a lock-step virtual clock, writes the recordings and compares them with the committed digests of v1's, where every difference must be explained by a rule, and with v2's own baseline digests; no device opened).

The player can also be started with a file: `midibn-win32.exe "C:\path\song.mid"`. Loading runs on its own thread with progress and the large-file warning, so the window stays responsive.

### 3. Checks

```powershell
.\v2\out\build-x64\Release\midibn-tests.exe   # unit tests
.\v2\tools\parity.ps1                         # v1 drift check + v2 parity for every built variant (includes the engine comparison)
.\v2\out\build-x64\Release\midibn-engine-harness.exe --fixtures v2\harness\fixtures --scenarios v2\harness\engine-scenarios.txt --expected v2\harness\expected\engine --rules v2\harness\expected\differences-from-v1.txt --baseline v2\harness\expected-v2\engine
                                              # the engine comparison alone; --out DIR writes the recordings, --v1 DIR shows the first differing line (v1 text next to v2 text, also for explained differences), --fail-fast stops at the first difference, --only NAME,PREFIX* selects scenarios
.\v2\harness\make-engine-expected.ps1         # rewrites expected\engine (v1's recording digests) from a spike run
.\v2\harness\make-v2-baseline.ps1             # re-records v2's own output (expected-v2\) and prints which entries changed (-Check: only print); a deliberate act, see v2\docs\DIFFERENCES_FROM_V1.md
.\v2\tools\perf-compare.ps1 -Runs 5           # load time and peak memory, v1 versus v2, on the large fixture
.\v2\tools\perf-compare.ps1 -Runs 5 -Analysis # the same, plus Analysis times and a bit-for-bit result check (v2 builds)
.\v2\tools\footprint-measure.ps1              # Milestone 7 footprint (see the note after this block): executable sizes, load time and peak memory, the load flow, player start-up time (hidden desktop); -Before DIR compares with earlier builds in interleaved order, -Mode Capacity finds the biggest file the 32-bit builds load, -IncludeV1 adds v1
.\v2\tools\mutation-check.ps1                 # one changed decision per model must fail its model trace (x64 build)
.\v2\tools\mutation-check.ps1 -Family engine  # one changed decision per engine component must fail the engine comparison (x64 build)
.\v2\tools\mutation-check.ps1 -Family harness # eight cases that test the comparison itself: an unexplained difference, a stale rule and a v2 baseline drift must each fail (copies of the expected files; no source changes)
.\v2\tools\package-xp-selftest.ps1            # builds v2\out\xp-selftest\ and v2\out\v2-xp-selftest.zip (RUN-SELFTEST.cmd = quick sample, RUN-SELFTEST-FULL.cmd = everything)
.\v2\tools\package-win11-check.ps1            # builds v2\out\win11-check\ and v2\out\v2-win11-check.zip (x64 build-x64 midibn-direct-check.exe + RUN-CHECK.cmd + README.txt): the silent Direct WinMM driver access check for a Windows 11 PC (DD-085); one result file, no administrator rights
.\v2\out\build-x64\Release\midibn-direct-output-tests.exe   # Direct WinMM driver access (DD-085): path policy, matching, start-up sequence, the output on a fake modMessage (no driver is loaded)
.\v2\out\build-x64\Release\midibn-direct-check.exe --list   # the same silent check the packages run (--list: only the driver and device lists; without it every directly drivable device is opened and closed with silent messages and the speed of standard WinMM and the direct path is compared; only one copy may run at a time). Do not run it while another program uses the same synthesizer
.\v2\tools\package-release.ps1 [-Build] [-WhatIf]   # the four players under their release names (MIDIBottleneck Player x64.exe / x86.exe / XP.exe, test-only\... XP SSE2 test.exe) + README.txt + SHA256SUMS.txt in v2\out\release\<date>\; it never contains a DLL. The build itself still produces midibn-win32.exe (the tools look for that name); names are given only when packaging
.\v2\tools\refresh-test-kit.ps1 [-WhatIf]     # refreshes v2\out\test-player\ (x64\, x86\, xp\, xp-sse2-test\; the maintainer's test layout) with the named players, removes the old midibn-win32.exe copies and the folders xp-no-sse2\ and xp-sse2\, rewrites HOW-TO-TEST.txt; never creates, copies, moves or deletes any winmm.dll
.\v2\tools\adaptive-repeats-selfcheck.ps1     # checks the shared round-count rule (adaptive-repeats.ps1) on made-up numbers; the measurement tools (throughput-measure, ia32-vs-sse2, perf-compare, footprint-measure, refresh-pacing-measure, device\run-device-measurements, device\window-throughput) run 3 rounds and add one at a time up to 7 while a result is unclear; an explicit -Rounds / -Runs / -Repeats N is exact
.\v2\tools\device\make-silent-fixture.ps1      # silent MIDI files (controllers only, no Note On) under v2\out\fixtures-silent\: light, medium, heavy; -Rate R -Seconds S makes one custom file. Used for every real-driver run
.\v2\tools\make-large-load-fixture.ps1 -Silent  # a generated 104-million-event silent file (298 MB) whose load needs more than 2 GiB in a 32-bit program (-From <file> repeats another file instead).\v2\out\build-x86\Release\midibn-large-address-probe.exe --path none|fake-direct|standard|kdmapi|direct [--device ID]   # fills the low 2 GB, then opens an output and sends silent messages (controllers 121 and 123, a GM System On SysEx) from buffers above 2 GB; --list shows the devices; ends with PROBE: PASS (DD-114). The paths `standard`, `kdmapi` and `direct` need a real MIDI driver (optional); `none` and `fake-direct` need none
.\v2\out\build-x64\Release\midibn-thread-report.exe --path none|standard|kdmapi|direct [--device ID]   # lists the threads of its own process (priority, start module) before, after and when closing an output; only controllers 121 and 123 are sent (DD-113). The player has --thread-report FILE [--thread-report-delay-ms N]
.\v2\tools\few-cpu-measure.ps1                  # playback restricted to one or two processors with the --few-cpu options (DD-112); output None with a per-message cost, no device
.\v2\tools\display-tier-measure.ps1 -Set cost|half|adapt   # cost of one live update and the adaptive display rate's tier behaviour (DD-109); output None; needs the test switches --display-tier, --display-cost-us and so on
.\v2\out\build-x64\Release\midibn-few-cpu-measure.exe --auto   # silent, about 35 s: its own test file, output None, the few-processor option off against on with 2 and 1 processors; writes few-processor-result.txt (DD-132; XP IA32 name v2-xp-few-cpu-ia32.exe, step 7 of the XP launchers)
.\v2\out\build-x64\Release\midibn-direct-check.exe --fake-driver   # the real direct send path into an in-process no-op driver: ns per send, one at a time and in a batch of 256 (DD-110), plus `lean_ns` and `lean_batch_ns` for the engine's lean send (DD-131); no device
.\v2\tools\monitor-launch.ps1                 # monitored launch/close of midibn-win32.exe for every built variant
.\v2\tools\ui-smoke.ps1 -Build x64            # player smoke test: in-process self-test, UI Automation check, CPU report (output None only)
.\v2\out\build-x64\Release\midibn-app-tests.exe v2\harness\fixtures   # main-window behaviour tests
```

`footprint-measure.ps1` (Milestone 7): run it from the repository root on a quiet PC (it labels a set "quiet" only when the processor load stayed under 20 percent) after building all four variants. Without `-Before` it measures the current builds (sizes, load of the 8M fixture, load flow, start-up on a hidden desktop). For a before/after comparison give `-Before DIR`, a folder with `<variant>\midibn-perf.exe` and `<variant>\midibn-win32.exe` for x64, x86, xp and xp-ia32 saved from the earlier code; runs alternate before/after so both see the same load. **x86 baseline note:** for a start-up comparison the "before" player must contain the start-up timing of `--report` (commit 534ee6c or later); a player built before it writes no timing and its row shows nonsense (the Milestone 7 saved x86 baseline was one of these: rebuild the baseline x86 player from 534ee6c with the pre-Milestone-7 loader, or compare only builds that have the keys). `-Mode Capacity -Before DIR` makes files of up to 0.7 GB in `%TEMP%` and loads them with the 32-bit builds (slow: several minutes per build); `-IncludeV1` adds v1's load time, peak memory and executable size. The player is only ever started on a private hidden desktop and never plays.

`parity.ps1` must end with `PARITY: PASS`. The parity checker compares the load/index trace and both flow traces of every fixture, replays every model trace (`v2\harness\TRACE_FORMAT.md`, "Model traces"), and allows only the differences from v1 that a rule of `v2\harness\expected\differences-from-v1.txt` explains (each rule belongs to an entry of the improvement register `v2\docs\DIFFERENCES_FROM_V1.md`; grammar in `v2\harness\TRACE_FORMAT.md`, "Difference rules"); each one it applies is printed with its v1 and v2 line and its register id, and a rule that explains nothing any more is a failure. As a second, independent tier it checks v2's output against v2's own recorded baseline (`v2\harness\expected-v2\`, regenerated deliberately by `v2\harness\make-v2-baseline.ps1`).

Test-only switches of `midibn-win32.exe` (a normal launch ignores them; none ever opens a MIDI device — `--play` always uses output None and refuses to play, writing `play_refused=...` to the `--report` file, if the selection does not read back as None; the in-process self-test skips its play part and records a FAIL in the same case): `--open FILE`, `--play`, `--rate N` (0 None … 7 Output rate gate), `--scale 50..200`, `--width W`, `--borderless 0|1|2`, `--refresh-ms N` (the previous scheme, for comparison: a plain Windows timer at N ms instead of display pacing), `--pace auto|display|timer` (live-update pacing: `auto` follows the monitor via DwmFlush and falls back to a precise timer; `timer` forces the timer), `--pace-hz N` (rate of the timer, default 60), `--pace-max-hz N` (cap of the display pacing), `--auto-close-ms N`, `--screenshot FILE.bmp [--screenshot-delay-ms N]` (renders the window's own client area, never the screen), `--popup-shot FILE.bmp` (the Rate model popup), `--report FILE` (UI-thread and process CPU, playback time, the timing of the live updates: count, mean, standard deviation, median, 99th percentile and maximum interval, UI time per update, pacing source, and the start-up timing of `footprint-measure.ps1`: raw performance-counter readings `qpc_entry`, `qpc_created`, `qpc_idle` with `qpc_frequency`, `startup_create_ms`, `startup_idle_ms`, and `mark_<step>_ms` for each step of the start-up), `--selftest RESULT --open FILE` (the in-process check list).

`monitor-launch.ps1` is the release protocol's native-dialog/process check for the Win32 frontend. It compiles version 1's `archive\v1\tools\NativeWindowMonitor.cs` into `v2\out\monitor\`, then for every existing build (`-Builds x64,x86,xp,xp-ia32` to choose) proves both detector channels (`midibn-win32.exe --native-window-monitor-fixture`, which shows a window titled `Deliberately identifiable detector proof window`, and `--native-process-monitor-fixture`, which starts a short-lived copy of itself) and then runs the real launch: `--auto-close-ms 3000 --open v2\harness\fixtures\tempo-changes.mid` (loads the file, then closes through a posted `WM_CLOSE`) with a 5-second post-exit watch. These switches are test-only; a normal launch ignores them. The JSON-lines logs stay in `v2\out\monitor\`; the script prints one row per build and ends with `MONITOR-LAUNCH: PASS` or `FAIL`. The whole script runs on a hidden desktop (`-Visible` to use the current one), and a fourth stage (`-SkipCompanions` to omit) starts the player under the same monitor on output None and drives it (`v2\tools\device\player-e2e.ps1 -Scenario Companions`) through Play, Analysis, Channels, Stop, Track routing (Cancel), Help and About, then closes it, so the opening and closing of every companion window is watched too. The audible/real-device tests are described in `v2\tools\device\AUDIBLE-TESTS.md`.

### Visual gate

```powershell
.\v2\tools\visual\visual-gate.ps1                       # x64 build, v2 and v1; contact sheets in v2\out\visual\gallery\
.\v2\tools\visual\visual-gate.ps1 -Build x86 -V1From v2\out\visual\x64   # x86 build, reuse the v1 pictures of the x64 run
.\v2\tools\visual\visual-gate.ps1 -Quick -SkipV1        # a handful of v2 states, about 1 minute
.\v2\tools\visual\visual-gate.ps1 -V2Exe C:\other\midibn-win32.exe -SkipV1   # judge another build (mutation proof)
.\v2\tools\visual\visual-gate.ps1 -Visible              # real screen pixels on the monitor named by -Monitor, plus the resize checks
.\v2\tools\visual\visual-gate.ps1 -Visible -Only 'resize-*' -SkipV1 -V2Exe C:\old\midibn-win32.exe   # resize checks only
```

**Visible mode (`-Visible`).** Instead of a hidden desktop: each program is started hidden and then shown, without activation and top-most, on the monitor named by `-Monitor` (a 1920x1080 monitor is expected; pass the name of one of your monitors); any new window that appears elsewhere is moved there at once. Every capture is the screen's own pixels, taken without forcing a repaint, so pixels a program leaves stale are captured. If that monitor's scale differs from the scale the players lay out for, Windows stretches their windows and every capture is marked failed with that reason. Only this mode runs the **resize checks** (`resize-run.ps1`, keys `resize-<style>-<scale>`): each frame style at each scale, v2 and v1 with a file loaded, is resized in steps inside a simulated move/size loop: slowly shrinking to the minimum width and growing beyond the standard width (across the compact-layout boundary), fast drags (a size every 15 ms, checked during and after the drag), and fast drags of the left border; after each step the screen is captured as the resize left it, the window is made to repaint completely, captured again, and the client areas are compared. Any differing v2 pixel fails the gate (`-ResizeTolerance`); v1's counts are printed as `INFO`; difference pictures (magenta pixels, red box) are in `<OutDir>\resize\`; gallery sheet 08. Proof: on the preview build `b06107f` (resize garbage, DD-090) the resize checks end `VISUAL-GATE: FAIL (130 v2 findings)`; on the fixed build `PASS` (DD-090's evidence). A monitor that is switched off reads back black: the gate detects this and then takes each window's composed surface from the desktop compositor (`PrintWindow(PW_RENDERFULLCONTENT)` with no forced repaint, still stale pixels included, which is how the proof above was made) and says so in its log. Each program is shown on the monitor only after its window is fully built (a size set while the player was still building its window raced with its layout). Both modes also capture the Direct WinMM status line in each frame style (`direct-*`, sheet 07) and every other window at 75 % (`win75-*`, `dlg75-*`, sheet 05b) and at 50 % (`win50-*`, `dlg50-*`, sheet 05c; v1 too, except the large-file warning and the speed-window prompt), the main window at 50 % in every frame style and layout, without a file and playing (`main-nofile-50-*`, `main-playing-50-*`, `border50-*`, `main-loading-50`, sheet 04b; v1 `border50-*`), and the two prompts at 100, 75 and 50 % (`dlg*-speedwindow`: Effective playback speed window, opened through the Effective speed menu, v2 only; `dlg*-customres`: Custom Analysis resolution). In visible mode every new window or dialog is also checked, before the gate moves anything, to open inside the work area of the gate's monitor, where the main window is (DD-098): a v2 window that opens elsewhere is a finding, v1's are listed as `INFO`; the lines are in the log (`placement:`) and in `<OutDir>\placements.txt`.

It ends with `VISUAL-GATE: PASS` or `VISUAL-GATE: FAIL` (exit code 0 / 1). Only v2 findings fail the gate; the same checks run on v1's pictures and are printed as `INFO` (v1 is the reference for comparison, not under test). Parameters: `-OutDir` (pictures, window trees, `results.tsv`, `gate.log`; default `v2\out\visual\<build>`), `-Gallery` (contact sheets), `-Only 'scale-*','win-*'` (state keys), `-V1Exe`, `-LargeFixture`.

**How it captures.** The script starts a copy of itself on a private hidden Win32 desktop (`CreateDesktop`) and runs everything there: nothing ever appears on a monitor and no focus is taken. On that desktop it starts v2 and v1 normally (no test switch except `--open`, `--play`, `--scale`, `--width`, `--borderless`), drives them only with window messages (system-menu commands, button clicks, combo selections; v1's "Open MIDI" file dialog is filled in through its file-name box), and captures each window with `PrintWindow(PW_RENDERFULLCONTENT)`. This was checked against a visible window and the screen itself (`CopyFromScreen`): identical outside the window frame. Do not capture by moving a window off-screen: a window beyond every monitor is not painted at all, and a minimized-then-restored window paints differently from one that was simply shown. Output is always **None** (selected in both programs before anything is played) and KDMAPI is verified unticked. A capture is taken only when two captures 500 ms apart are identical (the program is idle and fully painted); a timeout is printed as a note. Playing and loading are captured "live" after a short settle time.

**States** (key prefix `main-`, `scale-`, `border-`, `win-`, `dlg-`, `dd-`): no file / loading (`large-8m.mid`) / loaded / playing, standard and compact layout; Application scale 50, 75, 100, 125, 150, 200 in both layouts; native frame and the three borderless styles in both layouts; Analysis window with results, Channels window while playing, Track routing editor with a plan, Help, About, the large-file dialog (only when the program shows it); the Overflow and Rate model drop-downs open (the popup window is captured and composited at its real position onto the main window).

**The pixel check** (per capture, from the window tree recorded at the same moment): `blank` (a visible control is a single colour, so nothing was drawn: hidden behind another control, clipped, or never painted), `overlap` (two sibling controls overlap by more than 1 px), `outside` (a control is not inside its parent's client area), `frame` (a control crosses a group frame), `bg` (a region of background between the controls, larger than a few pixels, that is not the face colour; anything lighter than the face colour is always a finding). Deliberately empty or single-coloured controls are listed in the explicit allow-list at the top of `visual-gate.ps1` (each entry has a reason). Reading a line: `FAIL v2 main-loaded-std [stable 1366] 806x505 controls=27 allowed=0 findings=5` is state key, idle wait in ms, window size, controls examined, allow-listed exceptions, findings; the indented lines name each finding with rectangles in capture pixels. The pictures and `*.tree.txt` window trees of every state are in the output folder, so a finding can be looked at directly.

**The contact sheets** (`v2\out\visual\gallery\NN-section-<build>-pK.png`) put v2 (left) next to v1 (right) per state with a caption. **Mutation proof** (done when the gate was written): the player built from the commit before the blank-window fix (`9cdb927~1`) ends `FAIL (461 v2 findings)` (every control reported `blank`); the build from `9cdb927`, which still had white group interiors, ends `FAIL (84 v2 findings)` (the `bg` check reports the white regions). To repeat: `git worktree add --detach v2\out\mut <commit>`, build it with its own `v2\tools\build-player.ps1`, and run the gate with `-V2Exe` pointing at that build and `-SkipV1 -Quick`. Run both the `x64` and the `x86` build before a release.

**Other UI tests on the hidden desktop**: `.\v2\tools\visual\run-hidden.ps1 -File <script> -Arguments '<args>'` (or `-Command '<command>'`) runs any command there and exits with its exit code; programs started by it paint normally but never reach a monitor, and input is never delivered to them (drive them with window messages). Prefer the hidden desktop for UI tests; use a real screen only if unavoidable, and identify monitors by name or size, not by coordinates.

### Engine-harness spike (Milestone 3, v1 side only)

```powershell
.\v2\harness\v1-reference\spike\build-spike.ps1          # v1 sources (minus Program.cs, HighResolutionWaiter.cs) + lock-step scheduler
.\v2\harness\v1-reference\spike\run-spike.ps1 -Runs 5    # repeated runs must give identical SHA-256; -Mode free is the negative control
```

See `v2\docs\ENGINE_HARNESS_FEASIBILITY.md`.

## Linux

The Linux build makes the portable core, every test program that needs no Windows, the ALSA output and the GTK 3 player. It was developed in WSL ("Ubuntu-22.04"), but any Linux works. Nothing in this section opens a MIDI device unless it says so.

**Needed (Ubuntu / Debian package names):** `g++` 11 or later (or `clang` 14 or later), `cmake` 3.20 or later, `ninja-build`, `libasound2-dev` (the ALSA output and its tests and tools; without it the build still works, without them), `libgtk-3-dev` and `pkg-config` (the GTK player), `xvfb` (the GTK checks run only on a virtual screen). Keep build folders in the Linux file system (for example `/root/build/midibn`), not under `/mnt/d`.

```bash
cmake -S v2 -B /root/build/midibn/gcc   -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake -S v2 -B /root/build/midibn/clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build /root/build/midibn/gcc
```

Run from the repository root. From Windows: `wsl -d Ubuntu-22.04 -u root -- bash -c "cd /mnt/d/<repo> && <command>"`.

**Checks (no sound, no MIDI port):**

```bash
v2/tools/linux-check.sh /root/build/midibn/gcc              # every portable test, the parity check (tier 1 and 2) and the engine harness
v2/tools/linux-check.sh /root/build/midibn/gcc quick        # the same without the engine runs (about 1 minute)
v2/tools/linux-check.sh /root/build/midibn/gcc quick gtk    # also the GTK player's scripted check under Xvfb (it starts and stops Xvfb itself)
```

It ends with `LINUX-CHECK: PASS` or `FAIL`. The v1 comparison against a regenerated .NET trace stays Windows-only (`tools/parity.ps1`); on Linux the committed v1 traces and engine digests are compared exactly as on Windows. The Linux build makes `midibn-tests`, `midibn-ui-tests`, `midibn-channels-app-tests`, `midibn-app-tests`, `midibn-analysis-app-tests`, `midibn-routing-app-tests`, `midibn-engine-tests`, `midibn-linux-platform-tests`, `midibn-alsa-output-tests`, `midibn-parity`, `midibn-engine-harness`, `midibn-dump`, `midibn-gtk`, and the tools `midibn-linux-capability`, `midibn-alsa-ports` and `midibn-alsa-send-check`.

**GTK pictures and smoke test (virtual screen only):** `xvfb-run -a -s "-screen 0 1920x1080x24" build/midibn-gtk --output none --capture OUT.png --capture-window help` (also `main`, `dialog`, `about`, `analysis`, `channels`, `routing`; `--scale P`, `--show help|about|analysis|channels|routing`, `--help-topic N`, `--capture-after-ms N`). `--smoke` runs the scripted check; `--strict` makes a GLib warning an error. Never start the GTK player on a real screen from a script.

**Optional, on a real Linux machine (the first two may open the sequencer; they send nothing audible):**

```bash
build/midibn-linux-capability --quick       # kernel, glibc, CPU, clock, waiter accuracy, priority, ALSA
build/midibn-alsa-ports                     # lists destinations; pick a harmless one, for example "Midi Through" (14:0)
build/midibn-alsa-output-tests              # the loopback part runs when /dev/snd/seq exists; it prints every event that arrived and judges only the player's own (DD-133)
build/midibn-alsa-send-check 14:0 --seconds 3   # silent controller messages only; compares direct / buffered / batched
```

The Linux player is not part of the 2.0 release; this section is for people who want to build it from source.

## Windows XP self-test

`package-xp-selftest.ps1` produces `v2\out\v2-xp-selftest.zip` from both XP builds (`build-xp` and `build-xp-ia32` must be built). The IA32 program (`v2-xp-selftest-ia32.exe`, variant 1) is the release build; the SSE2 program (`v2-xp-selftest.exe`, variant 2) is a test comparison only, and the package README and the result file say so. Copy it to the XP machine, extract it, and double-click one of two launchers. No .NET, no Visual C++ runtime, and no administrator rights are needed. `README.txt` in the package explains both in plain words.

| Launcher | What it runs | Result file | Time on a Pentium 4 3.8 GHz XP machine |
|---|---|---|---|
| `RUN-SELFTEST.cmd` (default, **quick**) | IA32 variant: all 55 load traces and 110 flow traces, all 9 model scripts but only the first third of every section (at least 400 lines; `--model-prefix 3`), the difference rules and v2 baseline for what ran, all decimal vectors and operations, an engine sample (`--engine-sample 10`: scenario #j of the file on fixture #i of the list when `(i + j) % 10 == 0`, 391 of 3,910 runs; every scenario family, scenario and fixture is touched), and the capability report with a 16 MB instead of 64 MB disk test (`--capability-quick`). Then, only if Windows reports SSE2: the SSE2 variant on every 4th fixture by name order (14 of 55; `--trace-fixtures`), the first 1/12 of every model section (`--model-prefix 12`), all decimal checks, and a smaller engine sample (`--engine-sample 34,5`: `(i + j + 5) % 34 == 0`, 115 runs). | `selftest-result.txt`, `capability-report.txt` | estimated about 2 minutes (limit 3); was about 15 |
| `RUN-SELFTEST-FULL.cmd` (**full**) | Everything in both variants and the full capability report, as in the earlier packages. | `selftest-result-full.txt`, `capability-report.txt` | about 15 minutes |

The subsets are deterministic (no random choice), so two quick runs check the same things. Whatever a quick run leaves out is reported as `not exercised` and never fails: a difference rule whose targets were not run (or whose sampled runs it did not change), the v2 baseline entries of the fixtures left out, the whole-trace digests of the cut model scripts, and the engine pairs not sampled (the pair counts are printed in a `note engine scenarios: quick sample` line). A model script that is cut short is compared with the same cut of v1's recording; the `~` state lines of a section that follows a cut section are masked until that section creates its own adaptive part (they would show state left over from the unreplayed rest of the previous section). The comparison still fails on any other difference (checked by altering a recorded line inside the replayed part). The sample options are additive options of the parity tool (`--trace-fixtures`, `--model-prefix`, `--engine-sample`, `--capability-quick`; see the comment at the top of `tools\parity_main.cpp`); `--write-baseline` refuses to run with any of them.

The launcher runs the IA32 self-test first (it also writes the capability report and reports the processor's SSE2 support), then the SSE2 self-test only if SSE2 is present (otherwise it writes `skipped: no SSE2`). Each run reports its own time per part: the launcher's clock for each program (whole seconds), each program's `Parts (seconds)` line (traces and flows, models, engine scenarios, decimal arithmetic), its `Elapsed:` line, the capability report's time, and a total in the SUMMARY. Set the environment variable `SELFTEST_NOPAUSE=1` to skip the final `pause` (for automated runs). A last step (4 of 4, both launchers) runs `v2-xp-direct-check-ia32.exe --seconds 0.3 --cycles 3 --out direct-check-result.txt` (DD-085, source `tools\direct_check_main.cpp`): a silent check of whether the MIDI drivers can be driven directly (no sound, Windows' own devices never opened). Its output is added to the main result file and saved as `direct-check-result.txt`; the SUMMARY line `Direct WinMM driver access check: DONE|FAILED (n s)` is information only and never changes `OVERALL` (FAILED means a nonzero exit code, a hang, or no `DIRECT-CHECK: DONE` line). Step 7 (both launchers, information only) runs `v2-xp-few-cpu-ia32.exe --auto` (DD-132, about 35 s here, up to about 2 minutes on a slow PC) and writes `few-processor-result.txt`; the package README lists it among the files to send back.

## Layout

| Path | Contents |
|---|---|
| `core/` | Portable C++17 library (no OS calls): loader with progress/cancellation and preflight decision flow, compact store, source-value and chase indexes, SHA-256, trace writer (the .NET decimal emulation lives only in the test-oracle library) |
| `platform/win32/` | Win32 file access, process memory, OS description, timing/threads (`Win32EngineServices`), WinMM and KDMAPI outputs |
| `platform/linux/` | POSIX clock, waiter, threads, files, memory, and the ALSA sequencer output (Linux) |
| `frontends/win32/`, `frontends/gtk/` | The Windows (plain Win32) and Linux (GTK 3) programs: presentation only |
| `platform/virtual/` | Lock-step virtual `EngineServices` for deterministic engine tests |
| `tools/` | Dump, parity, performance, packaging scripts |
| `tests/` | First-party unit tests |
| `harness/` | v1 reference harness, trace format, fixtures and their generator, expected traces |
| `docs/` | Architecture, XP cost log, the improvement register (differences from v1) |
| `out/` | All build output (never committed) |
