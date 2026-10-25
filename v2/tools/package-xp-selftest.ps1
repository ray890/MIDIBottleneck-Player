# Assembles the Windows XP self-test folder (and a zip of it) from the two XP builds
# (build-xp-ia32 = /arch:IA32, no SSE2 needed: THE XP RELEASE BUILD (user decision, prompt 009);
#  build-xp = default code generation, SSE2: kept only as a test comparison, never released):
#   v2\out\xp-selftest\  ->  v2-xp-selftest-ia32.exe, v2-xp-selftest.exe, v2-xp-direct-check-ia32.exe (silent check of direct WinMM driver access, DD-085; the last step of both launchers, information only, not part of OVERALL), RUN-SELFTEST.cmd (quick, the default),
#                            RUN-SELFTEST-FULL.cmd (everything), README.txt, v2-xp-thread-report-ia32.exe (P10-THR: the last step of both launchers, silent thread report per MIDI path -> thread-report-winmm/kdmapi/direct.txt, information only),
#                            v2-xp-kdmapi-check-ia32.exe (P11-OPT: the very last step of both launchers, silent check of where OmniMIDI.dll would be found -> kdmapi-check-result.txt, information only),
#                            fixtures\*.mid|*.rmi, expected\*.trace, expected\flow\*.trace,
#                            expected\models\*.trace, expected\decimal-vectors.txt, decimal-ops.txt,
#                            differences-from-v1.txt (the rules that explain differences from v1), engine-scenarios.txt,
#                            expected\engine\*.digests|manifest.txt, expected-v2\ (v2's own recorded baseline)
#
# The two launchers (v2/BUILDING.md, "Windows XP self-test"). Subsets are deterministic and documented in README.txt:
#   RUN-SELFTEST.cmd       quick, about 3 minutes on a Pentium 4 3.8 GHz Windows XP machine (about 30 s on a modern PC):
#       variant 1 (any processor, also runs the quick capability report): every load trace, flow trace, model trace
#         (the first third of every section), the difference rules, the v2 baseline, the decimal checks, and an
#         engine sample: scenario #j of the file on fixture #i of the list when (i + j) % 10 == 0 (every scenario
#         and every fixture is touched, 391 of 3,910 runs);
#       variant 2 (SSE2, only if the processor has it): every 4th fixture's traces (by name order), the first 1/12 of
#         every model section, the decimal checks, and a smaller engine sample, (i + j + 5) % 34 == 0 (115 runs).
#   RUN-SELFTEST-FULL.cmd  everything in both variants, as in the earlier packages (about 15 minutes on that machine).
# Whatever a quick run leaves out is reported as "not exercised", never as a failure.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')).Path
Set-Location $repo
$exes = @('v2\out\build-xp-ia32\Release\v2-xp-selftest-ia32.exe', 'v2\out\build-xp\Release\v2-xp-selftest.exe', 'v2\out\build-xp-ia32\Release\v2-xp-direct-check-ia32.exe', 'v2\out\build-xp-ia32\Release\v2-xp-thread-report-ia32.exe', 'v2\out\build-xp-ia32\Release\v2-xp-kdmapi-check-ia32.exe', 'v2\out\build-xp-ia32\Release\v2-xp-few-cpu-ia32.exe')
foreach ($exe in $exes) { if (-not (Test-Path $exe)) { throw "Build both XP variants first (see v2\BUILDING.md): missing $exe" } }
$target = 'v2\out\xp-selftest'
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
New-Item -ItemType Directory -Force -Path "$target\fixtures", "$target\expected\flow", "$target\expected\models", "$target\expected\engine" | Out-Null
foreach ($exe in $exes) { Copy-Item $exe $target }
Get-ChildItem 'v2\harness\fixtures' -File | Where-Object { $_.Extension -in '.mid', '.rmi' } | Copy-Item -Destination "$target\fixtures"
Get-ChildItem 'v2\harness\expected' -File -Filter '*.trace' | Where-Object { $_.Name -notlike 'large-*' } | Copy-Item -Destination "$target\expected"
Get-ChildItem 'v2\harness\expected\flow' -File -Filter '*.trace' | Where-Object { $_.Name -notlike 'large-*' } | Copy-Item -Destination "$target\expected\flow"
Get-ChildItem 'v2\harness\expected\models' -File -Filter '*.trace' | Copy-Item -Destination "$target\expected\models"
Copy-Item 'v2\harness\engine-scenarios.txt' $target
Get-ChildItem 'v2\harness\expected\engine' -File | Copy-Item -Destination "$target\expected\engine"
foreach ($name in 'decimal-vectors.txt', 'decimal-ops.txt', 'differences-from-v1.txt') { Copy-Item "v2\harness\expected\$name" "$target\expected" }
# v2's own recorded baseline (second tier of the comparison), whole folder: traces.sha256, text\, engine\
Copy-Item -Recurse 'v2\harness\expected-v2' "$target\expected-v2"

# Quick-run subsets (see the header). The SSE2 variant's trace sample is every 4th fixture (positions 0, 4, 8, ...)
# of the fixture file names in ordinal order.
[string[]]$fixtureNames = @(Get-ChildItem "$target\fixtures" -File | ForEach-Object { $_.Name })
[Array]::Sort($fixtureNames, [StringComparer]::Ordinal)
$sse2Sample = @(for ($i = 0; $i -lt $fixtureNames.Count; $i += 4) { $fixtureNames[$i] })
$quickIa32Args = '--model-prefix 3 --engine-sample 10 --capability-report %CAP% --capability-quick'
$quickSse2Args = '--trace-fixtures ' + ($sse2Sample -join ',') + ' --model-prefix 12 --engine-sample 34,5'
$fullIa32Args = '--capability-report %CAP%'
$fullSse2Args = ''

# One launcher template for both modes. It measures each step with the Windows clock (whole seconds; XP's cmd has
# no better timer) and writes the times next to each variant's own "Elapsed:" and "Parts" lines.
$cmdTemplate = @'
@echo off
setlocal
cd /d "%~dp0"
set OUT=@@OUT@@
set CAP=capability-report.txt
echo @@TITLE@@
echo Each part prints how long it took when it finishes; the times are also in %OUT%.
echo.
>%OUT% echo @@TITLE@@
>>%OUT% echo.
set T0=%TIME%
set TSTEP=%TIME%

echo Step 1 of 4: release build, for any processor, no SSE2 needed (also measures this PC)...
>>%OUT% echo ===== VARIANT 1 of 2: RELEASE BUILD, program for processors without SSE2 - v2-xp-selftest-ia32.exe @@MODE@@ =====
.\v2-xp-selftest-ia32.exe @@IA32ARGS@@ >>%OUT% 2>&1
set IA32RC=%errorlevel%
call :ELAPSED "%TSTEP%" STEP1
set TSTEP=%TIME%
echo   done in %STEP1% s
>>%OUT% echo Wall-clock time of variant 1 (including the capability report): %STEP1% s

>>%OUT% echo.
echo Step 2 of 4: asking Windows whether this processor has SSE2...
>>%OUT% echo Does this processor have SSE2? Asked of Windows by variant 1:
.\v2-xp-selftest-ia32.exe --has-sse2 >>%OUT% 2>&1
set SSE2RC=%errorlevel%

>>%OUT% echo.
>>%OUT% echo ===== VARIANT 2 of 2: TEST COMPARISON ONLY, program using SSE2 - v2-xp-selftest.exe @@MODE@@ =====
if "%SSE2RC%"=="0" goto RUNSSE2
echo Step 3 of 4: skipped: no SSE2
>>%OUT% echo skipped: no SSE2
set SSE2STATE=skipped: no SSE2
set STEP3=0
goto DIRECT

:RUNSSE2
echo Step 3 of 4: SSE2 program, a test comparison only (not a release build)...
set TSTEP=%TIME%
.\v2-xp-selftest.exe @@SSE2ARGS@@ >>%OUT% 2>&1
if errorlevel 1 (set SSE2STATE=FAIL) else (set SSE2STATE=PASS)
call :ELAPSED "%TSTEP%" STEP3
echo   done in %STEP3% s
>>%OUT% echo Wall-clock time of variant 2: %STEP3% s

:DIRECT
echo Step 4 of 4: silent check of direct access to the MIDI drivers (no sound, nothing is played)...
>>%OUT% echo.
>>%OUT% echo ===== DIRECT WINMM DRIVER ACCESS CHECK (silent) =====
if exist direct-check-result.txt del direct-check-result.txt
set TSTEP=%TIME%
.\v2-xp-direct-check-ia32.exe --seconds 0.3 --cycles 3 --out direct-check-result.txt >>%OUT% 2>&1
set DIRECTRC=%errorlevel%
>>%OUT% echo.
>>%OUT% echo ----- cost of one direct send into a no-op driver (no device, no driver; information only, DD-110) -----
.\v2-xp-direct-check-ia32.exe --fake-driver --seconds 0.3 --out direct-fake-result.txt >>%OUT% 2>&1
call :ELAPSED "%TSTEP%" STEP4
echo   done in %STEP4% s
>>%OUT% echo Wall-clock time of the direct access check: %STEP4% s
set DIRECTSTATE=FAILED
if not "%DIRECTRC%"=="0" goto DIRECTEND
find "DIRECT-CHECK: DONE" direct-check-result.txt >nul 2>&1
if not errorlevel 1 set DIRECTSTATE=DONE
:DIRECTEND

:THREADS
echo Step 5: silent thread report for each MIDI path (no sound; only "reset all controllers" and "all notes off" are sent)...
>>%OUT% echo.
>>%OUT% echo ===== THREAD REPORT (silent; which threads each MIDI path creates, and at which priority) =====
set TSTEP=%TIME%
for %%P in (winmm kdmapi direct) do (
  if exist thread-report-%%P.txt del thread-report-%%P.txt
  .\v2-xp-thread-report-ia32.exe --path %%P --out thread-report-%%P.txt >>%OUT% 2>&1
)
call :ELAPSED "%TSTEP%" STEP5
echo   done in %STEP5% s
>>%OUT% echo Wall-clock time of the thread reports: %STEP5% s

:KDMAPICHECK
echo Step 6: silent check of where the KDMAPI provider (OmniMIDI.dll) would be found (nothing is loaded, no sound)...
>>%OUT% echo.
>>%OUT% echo ===== KDMAPI PROVIDER SEARCH CHECK (silent; where OmniMIDI.dll would be found; nothing is loaded) =====
if exist kdmapi-check-result.txt del kdmapi-check-result.txt
set TSTEP=%TIME%
.\v2-xp-kdmapi-check-ia32.exe --out kdmapi-check-result.txt >>%OUT% 2>&1
call :ELAPSED "%TSTEP%" STEP6
echo   done in %STEP6% s
>>%OUT% echo Wall-clock time of the KDMAPI search check: %STEP6% s

:FEWCPU
echo Step 7: silent few-processor measurement (no sound, no MIDI device; about 1 to 2 minutes, please wait)...
>>%OUT% echo.
>>%OUT% echo ===== FEW-PROCESSOR MEASUREMENT (silent; output None; option off against on, 2 and 1 logical processors) =====
if exist few-processor-result.txt del few-processor-result.txt
set TSTEP=%TIME%
.\v2-xp-few-cpu-ia32.exe --auto --out few-processor-result.txt >>%OUT% 2>&1
call :ELAPSED "%TSTEP%" STEP7
echo   done in %STEP7% s
>>%OUT% echo Wall-clock time of the few-processor measurement: %STEP7% s

:SUMMARY
call :ELAPSED "%T0%" TOTAL
if "%IA32RC%"=="0" (set IA32STATE=PASS) else (set IA32STATE=FAIL)
set OVERALL=PASS
if not "%IA32STATE%"=="PASS" set OVERALL=FAIL
if "%SSE2STATE%"=="FAIL" set OVERALL=FAIL
>>%OUT% echo.
>>%OUT% echo ===== SUMMARY (@@MODE@@) =====
>>%OUT% echo Variant 1 - release build, program for processors without SSE2: %IA32STATE%  (%STEP1% s)
>>%OUT% echo Variant 2 - test comparison only, program using SSE2: %SSE2STATE%  (%STEP3% s)
>>%OUT% echo Direct WinMM driver access check: %DIRECTSTATE%  (%STEP4% s)  - information about this PC, not part of OVERALL
>>%OUT% echo Thread reports (thread-report-winmm.txt, -kdmapi.txt, -direct.txt): %STEP5% s  - information, not part of OVERALL
>>%OUT% echo KDMAPI provider search check (kdmapi-check-result.txt): %STEP6% s  - information, not part of OVERALL
>>%OUT% echo Few-processor measurement (few-processor-result.txt): %STEP7% s  - information, not part of OVERALL
>>%OUT% echo Total time: %TOTAL% s
>>%OUT% echo OVERALL: %OVERALL%  - skipped means this processor has no SSE2, which is not a failure
type %OUT%
echo.
echo The full result was saved to %OUT% in this folder.
echo The machine measurements were saved to capability-report.txt in this folder.
echo The direct driver access check was saved to direct-check-result.txt in this folder.
echo The thread reports were saved to thread-report-winmm.txt, thread-report-kdmapi.txt and thread-report-direct.txt in this folder.
echo The KDMAPI provider search check was saved to kdmapi-check-result.txt in this folder.
echo The few-processor measurement was saved to few-processor-result.txt in this folder.
if not defined SELFTEST_NOPAUSE pause
exit /b 0

:ELAPSED
rem %1 = start time as printed by %TIME%; sets variable %2 to the whole seconds elapsed since then (midnight handled)
call :TOSEC "%~1" S_START
call :TOSEC "%TIME%" S_END
set /a "%~2=S_END-S_START"
if %S_END% LSS %S_START% set /a "%~2=S_END+86400-S_START"
goto :eof

:TOSEC
rem %1 = a time such as 13:05:09.12 or " 9:05:09,12"; sets variable %2 to seconds since midnight.
rem The 100 prefix avoids 08 and 09 being read as bad octal numbers.
for /f "tokens=1-3 delims=:., " %%a in ("%~1") do set /a "%~2=(100%%a %% 100)*3600+(100%%b %% 100)*60+(100%%c %% 100)"
goto :eof
'@
function New-Launcher([string]$file, [string]$title, [string]$mode, [string]$out, [string]$ia32, [string]$sse2) {
    $text = $cmdTemplate.Replace('@@TITLE@@', $title).Replace('@@MODE@@', $mode).Replace('@@OUT@@', $out).Replace('@@IA32ARGS@@', $ia32).Replace('@@SSE2ARGS@@', $sse2)
    [System.IO.File]::WriteAllText((Join-Path $repo "$target\$file"), $text.Replace("`r`n", "`n").Replace("`n", "`r`n"))
}
New-Launcher 'RUN-SELFTEST.cmd' 'MIDIBottleneck v2 self-test - QUICK run (a sample of the full test; see README.txt)' 'quick' 'selftest-result.txt' $quickIa32Args $quickSse2Args
New-Launcher 'RUN-SELFTEST-FULL.cmd' 'MIDIBottleneck v2 self-test - FULL run (everything, both program variants)' 'full' 'selftest-result-full.txt' $fullIa32Args $fullSse2Args

$readme = @'
MIDIBottleneck Player v2 - Windows XP self-test (private)

Two ways to run it
  RUN-SELFTEST.cmd         QUICK (the one to use normally). Checks a deliberate,
                           documented sample of everything. It should take about
                           3 minutes or less on a Pentium 4 class Windows XP
                           computer (the earlier all-in-one run took about 15).
                           Result file: selftest-result.txt
  RUN-SELFTEST-FULL.cmd    FULL. Checks everything in both program variants.
                           Expect about 15 minutes on a Pentium 4 class computer.
                           Result file: selftest-result-full.txt

  Both also write capability-report.txt (what this PC can do; see below) and
  direct-check-result.txt (the driver access check; see below).
  Run the quick one first. Run the full one when asked to, or after a quick
  run reports a failure, to see everything.

What this checks (the full run)
  The new C++ engine loads 55 small test MIDI files and compares every detail
  it reads (events, timings, track names, error messages, memory estimates,
  the channel-state lookups used when playing from the middle of a song)
  with what the published v1.0.44 player reads from the same files. It also
  replays v1's loading steps - progress messages, the large-file warning
  decision, and cancelling at every possible point - and compares those.
  It then replays about 660,000 lines of recorded decisions from v1's timing,
  queue and note-gate rules (what to send, drop, delay or hold back, and when)
  and checks that the new code makes every one the same way. It then repeats
  v1's Analysis graphs (workload buckets and every queue and gate projection,
  including the 2,000,000-bucket limit) and v1's track-routing planner (every
  tier, manual corrections, omissions, the plan preview text and the Apply
  rules), and compares every result. It checks 6,000 timing-arithmetic examples
  and about 22,000 basic decimal calculations recorded from v1.
  It also runs v2's playback engine through 170 scripted playback scenarios on 23
  of the test files (play, stop, pause, seek, the rate models, queue limits, the note
  gates, channel controls, track routing) under a simulated clock, and compares every
  message the engine would send, with its time, and the engine's own counters with what
  the published v1.0.44 engine produced for the same script. That is about 3,900
  simulated playbacks per program variant.
  v2 is allowed to differ from v1 only where an improvement is recorded: for
  example, very short SysEx messages, which v1 turns into zeros, are fixed.
  The self-test lists each such line as an "explained difference" with the
  number of the improvement; any difference that is not explained is a failure.
  It also checks that v2 still gives exactly the answers it gave when they were
  last recorded (a second, independent comparison), and fails if not.
  Nothing is installed, no sound is played, and no MIDI device is opened.

What the quick run leaves out
  The quick run does the same kinds of checks on a smaller, fixed selection. The
  selection is the same every time (nothing is random), so two quick runs on
  different computers check exactly the same things.
    Program for any processor (the release build; runs first, always):
      - all 55 test files, their load traces and both load-flow traces;
      - all the recorded model scripts, but only the first third of each section
        of each script (at least 400 lines); this still covers every model;
      - all the explained-difference rules, the v2 recorded answers for the
        traces it ran, and all the decimal and timing-arithmetic checks;
      - 391 of the 3,910 playback simulations: scenario number j on test file
        number i, whenever (i + j) divides evenly by 10. Every one of the 170
        scenarios and every one of the 23 test files is used at least once;
      - the capability report, with a 16 MB instead of 64 MB disk speed test.
    Program using SSE2 (test comparison only; only if the processor has SSE2):
      - every 4th test file (14 of 55) with its traces;
      - the first 1/12 of each section of each model script;
      - all the decimal and timing-arithmetic checks;
      - 115 playback simulations: scenario j on test file i whenever
        (i + j + 5) divides evenly by 34.
      That is enough to show that the SSE2 code gives the same answers as the
      any-processor program; the full run checks all of it.
  Anything the quick run did not check is reported as "not exercised" in the
  result (for instance an explained-difference rule none of whose test files
  were run). "Not exercised" is not a failure and does not change PASS.

Two program variants - which one is the release build
  The test is done by two versions of the same program. They are built from
  exactly the same source code and must give exactly the same answers; they
  differ only in which processor instructions the compiler is allowed to use.

    v2-xp-selftest-ia32.exe   THE RELEASE BUILD. "Any processor" version: the
                              Windows XP player will be shipped only in this
                              form. Uses only the basic instructions that every
                              Pentium-class processor understands, so it also
                              runs on older processors that lack "SSE2".
    v2-xp-selftest.exe        TEST COMPARISON ONLY, not a release build. "SSE2"
                              version: uses SSE2, an instruction set built into
                              processors from about 2001 on (Pentium 4,
                              Athlon 64 and newer). It does not run on
                              processors without SSE2. It is kept in this
                              package only to compare speed and answers with
                              the release build.

  The launcher decides by itself which to run. It always runs the "any
  processor" version first; that version also asks Windows whether this
  processor has SSE2 and writes the answer into the result. The "SSE2" version
  is run only if the answer is yes. If it is no, the result says
  "skipped: no SSE2" for the second version - that is not a failure.

  After its test results, the first version also measures what this PC can do
  (once only, not repeated by the second version): timer and sleep accuracy,
  threads, memory, disk speed (including a temporary large sparse file created
  in this folder and deleted again afterwards), the names of the installed MIDI
  devices (nothing is opened or played), fonts and screen size. The report
  appears in the result file and a compact copy is written to
  capability-report.txt in this folder.

How to run
  1. Copy this whole folder to the XP computer (any location; a USB stick works).
  2. Double-click RUN-SELFTEST.cmd (or RUN-SELFTEST-FULL.cmd).
     (Or open a Command Prompt in this folder and type the name.)
  3. Wait until the window shows "OVERALL: PASS" or "OVERALL: FAIL" near the
     end, then press a key to close. The window shows how long each of the
     four steps took, and the result file has the same times: the launcher's
     own clock for each program, each program's own "Elapsed:" time, and a
     "Parts" line that splits it into traces, models, playback simulations and
     arithmetic, plus the time of the capability report and the total.
  4. Send back the result file (selftest-result.txt, or selftest-result-full.txt
     for the full run), capability-report.txt, direct-check-result.txt, kdmapi-check-result.txt, few-processor-result.txt and the three thread-report-*.txt files, which appear in this folder
     (or copy the last lines of the result file, which include the Windows version).

What the result looks like
  The result file has a section for each variant, each with its own RESULT,
  Parts and Elapsed lines, and a SUMMARY at the end with the total time.
  "skipped: no SSE2" for variant 2 only means this processor has no SSE2; it
  does not mean anything went wrong.

The driver access check (last step, about 10 seconds)
  After the self-test, the launcher runs one more small program,
  v2-xp-direct-check-ia32.exe. It finds out whether the player could talk to
  this PC's MIDI drivers directly instead of through Windows' standard MIDI
  layer, which can be faster. It is completely silent: no MIDI sound is made,
  nothing is installed, and no setting is changed. Windows' own devices
  (Microsoft GS Wavetable Synth, sound hardware) are never opened. Its output
  is added to the main result file and also saved as direct-check-result.txt.
  "Not available on this PC" for a device is a normal answer, not a failure.
  It does not change PASS or FAIL; the SUMMARY only says whether the check
  itself finished (DONE) or crashed or hung (FAILED).

The thread report (after the driver access check, about 10 seconds)
  One more silent program, v2-xp-thread-report-ia32.exe, runs three times,
  once for each way the player can reach the first MIDI output that is not
  made by Microsoft (standard Windows MIDI, KDMAPI, direct driver access).
  Each time it sends only "reset all controllers" and "all notes off" (no
  note, so no sound), lists the program's threads with their priority and
  the file each one comes from, waits 2 seconds, lists them again (new ones
  marked NEW) and closes the output. The results are thread-report-winmm.txt,
  thread-report-kdmapi.txt and thread-report-direct.txt in this folder; please
  send them back too. A path that does not exist on this PC (for example
  KDMAPI without OmniMIDI) just says so.

The KDMAPI provider search check (last step, a second)
  A last silent program, v2-xp-kdmapi-check-ia32.exe, only says WHERE the player
  would find OmniMIDI.dll on this PC: beside the program, in the folder where the
  installed MIDI driver is registered (for example the OmniMIDI folder inside the
  Windows system folder), or in the Windows system folder, and whether that file
  is built for this program (32-bit). It does not load or start anything and makes
  no sound. The result is added to the main result file and saved as
  kdmapi-check-result.txt; please send it back too. "NOT FOUND" is a normal
  answer on a PC without OmniMIDI. It does not change PASS or FAIL.

The few-processor measurement (step 7, 1 to 2 minutes)
  The last silent program, v2-xp-few-cpu-ia32.exe, plays a made-up, very dense
  stream of silent controller messages into a counter (no MIDI device, no
  driver, no sound) that this PC cannot keep up with, and measures how far
  behind playback ends, how many messages it sends per second, and how long an
  ordinary program thread has to wait to get a turn. It does this with the
  option "Let other programs run while playback is behind" off and on, once
  with two logical processors and once with only one (it restricts itself; your
  Windows settings are not touched). The result is few-processor-result.txt;
  please send it back too. It does not change PASS or FAIL.

Requirements
  Windows XP SP3 (32-bit or 64-bit) or later. No .NET, no Visual C++
  runtime, and no administrator rights are needed.
'@
[System.IO.File]::WriteAllText((Join-Path $repo "$target\README.txt"), $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"))

$zip = 'v2\out\v2-xp-selftest.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
# .NET ZipFile writes '/' separators (Compress-Archive in Windows PowerShell 5.1
# can write '\', which older extractors such as XP's built-in one mishandle).
# Entries are written one by one so their names use '/' regardless of the
# .NET Framework backslash compatibility switch.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = (Resolve-Path $target).Path
$archive = [System.IO.Compression.ZipFile]::Open((Join-Path $repo $zip), [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $root -Recurse -File) {
        $entryName = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally { $archive.Dispose() }
$count = (Get-ChildItem $target -Recurse -File).Count
Write-Host ("Packaged {0} files into {1} and {2} ({3:N0} bytes)" -f $count, $target, $zip, (Get-Item $zip).Length)
