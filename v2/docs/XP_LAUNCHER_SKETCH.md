# Sketch: an XP launcher that picks the SSE2 or the IA32 build

**Superseded (prompt 009, 2026-10-16):** the user chose the "Single IA32 executable" option below: the XP release build is the `/arch:IA32` build only, so no launcher is needed and none will be built. The SSE2 XP build remains only as a test comparison. The sketch is kept for the record.

Status: a sketch only. Nothing here is built, nothing is decided, and no wording in it is final. It exists so the user can compare the options in `XP_COST_LOG.md` ("Second XP variant measured") with a concrete picture of the launcher one. Every user-visible string below is a placeholder and is the user's decision.

## What it would be

A tiny Win32 program built with `/arch:IA32` (so it runs on any Pentium-class processor, including the ones without SSE2) that:

1. asks whether SSE2 can be used;
2. starts either the SSE2 build or the IA32 build of the player, passing its own command line through;
3. exits (or waits, see "Exit codes").

It would ship only in the XP package. Modern builds (x64, v143 x86) never need it.

## Deciding "SSE2 or not"

- Preferred signal: `IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE)`. It exists on XP, it is what the C runtime itself uses to enable its SSE2 paths, and it also reflects whether the operating system saves the SSE state (CPUID alone says only that the chip can).
- The same call is already what `v2-xp-selftest-ia32.exe --has-sse2` uses in the self-test package (exit code 0 = yes, 1 = no), so the logic would already have been exercised on the user's XP machine.
- CPUID leaf 1, EDX bit 26, is the alternative; it needs CPUID itself (all Pentium-class processors have it).
- A test override (an environment variable or a hidden argument forcing one variant) is useful for testing and support, but it is a new user-visible surface: the user's call.

## How it finds the two executables

- Build absolute paths from the launcher's own folder (`GetModuleFileNameW(NULL, ...)`, strip the file name), never relative names and never a `PATH` search. A concrete reason: the environment of this development session sets `NoDefaultCurrentDirectoryInExePath=1`, under which `cmd.exe` no longer finds an executable in the current folder by bare name; the self-test script had to switch to `.\name.exe` for that reason. `CreateProcessW` with an explicit application name and a full path has no such dependency.
- Layout options: all three side by side (`Player.exe` = launcher, `Player-sse2.exe`, `Player-ia32.exe`), or the two real executables in a subfolder (`bin\`) so a user does not double-click the wrong one. File names are a naming decision for the user.
- Check with `GetFileAttributesW` first so the "missing file" case gets its own message.

## Command line, quoting, passthrough

Two approaches:

- **Verbatim tail (sketch's choice for discussion).** Take `GetCommandLineW()`, skip the first token (the program name has its own simpler rule than the other arguments: a quoted name ends at the next quote, an unquoted one at the first space or tab), and append the rest unchanged after the target's quoted full path. The target sees exactly the arguments the launcher received, including odd quoting, empty arguments, trailing backslashes (`"C:\songs\"`), UNC paths and a drop of several files. About 15 lines; the edge cases are only around the program-name token and the whitespace after it.
- **Re-quote from `argv`.** `CommandLineToArgvW` then rebuild a command line with the documented quoting rules (double backslashes before a quote, quote arguments containing space/tab/quote, empty arguments as `""`). More code, and every mismatch silently changes what the player receives. Only needed if the launcher wants to add, drop or inspect arguments.

The v2 prototype window already reads `argv[1]` with `CommandLineToArgvW(GetCommandLineW())`, so it works behind either approach. The v1 player ignores its command line (see below); whether v2 should open a file from the command line at all is a product decision, not a launcher one.

Other limits: `CreateProcessW` accepts at most 32,767 characters of command line (shell drops of very many files can exceed this earlier, before the launcher is even started). Use the `W` APIs throughout.

## File associations, "Open with", drag and drop

- v1 registers no file association itself (`src/` has no registry code). If v2 or an installer registers one, it should register the **launcher**, not one of the real executables: `...\shell\open\command` = `"<launcher>" "%1"`. Registering a real executable would break when the other CPU class is used, or after the user copies the folder to another machine.
- The launcher then needs the product icon, version resource (file description, product name) and manifest, because Explorer takes the icon and the "Open with" display name from the registered executable.
- Drag and drop onto the launcher or onto a shortcut to it: Explorer starts it with the dropped files on the command line (quoted if they contain spaces), so the passthrough above covers it. Dropping onto a real executable bypasses the launcher; on a processor without SSE2 the SSE2 build then dies at start with an illegal-instruction error (0xC000001D). Hiding the real executables in a subfolder reduces that.
- v1's own drag-and-drop inside the window (`MainForm.MidiFileDragDrop`, one `.mid`/`.midi`/`.rmi` file) is unaffected.

## Single instance

- v1 has none: `src/Program.cs` creates one `MainForm` and runs it; there is no mutex, named pipe or window message anywhere in `src/`. Every start is a new process and window. The launcher adds nothing to change that.
- If a single-instance feature is ever added (named mutex plus a message to the first window), both variants must use the same mutex name and window class so that either can forward to the other, and the launcher should stay out of it (spawn, then exit). Otherwise a user switching variants would get two "single" instances.

## Window activation and foreground rights

- Windows lets a process take the foreground if it was started by the foreground process, among other conditions. A launcher started by Explorer from a click is foreground; its child normally qualifies as "started by the foreground process" even after the launcher has exited, but this is the rule that most often goes wrong in practice (window opens behind others, taskbar button flashes).
- Mitigations: `AllowSetForegroundWindow(pi.dwProcessId)` right after `CreateProcessW` (available on XP), and `WaitForInputIdle(pi.hProcess, <short timeout>)` before the launcher exits so the child has created its window while the launcher is still the foreground process; the second also ends Explorer's "working in background" cursor at the right time.
- Forward the launcher's own `STARTUPINFO` (`GetStartupInfoW`, keeping `wShowWindow` when `STARTF_USESHOWWINDOW` is set) so a shortcut's "Run: Minimized/Maximized" and "Start in" still apply. `STARTF_TITLEISLINKNAME` matters for shortcut-based taskbar behaviour.
- Taskbar grouping / `AppUserModelID`: not applicable on XP (the concept starts with Windows 7). On Windows 7+ a shortcut to the launcher and a running real executable would be two different taskbar identities unless both are given the same explicit ID; moot while the launcher is XP-package-only.

## Working directory and environment

Inherit both (`lpEnvironment = NULL`, `lpCurrentDirectory = NULL`): relative file arguments then resolve against the same folder as if the player had been started directly, and a shortcut's "Start in" is honoured. Do not switch to the executable's folder. `bInheritHandles = FALSE` (a GUI program has no console handles to hand over).

## Exit codes

Two designs:

- **Spawn and exit.** Launcher returns 0 after a successful `CreateProcessW`, non-zero if it could not start the target. Simplest, no extra process stays resident, but anything that waits on the program (`start /wait`, an installer step, a test harness) sees the launcher's exit, not the player's.
- **Wait and propagate.** Launcher waits for the child and returns its exit code. Keeps scripts and harnesses correct (the v1 `--launch-smoke` argument exists for exactly such a check) but leaves a second, idle process in Task Manager for the whole session, and the launcher must not exit early on a crash of its own.

An optional extra: if the child dies within a short time with `STATUS_ILLEGAL_INSTRUCTION`, the launcher could retry with the IA32 build. That would cover mis-detection, at the price of extra code and a harder-to-test path.

## Error dialogs (wording is the user's decision)

Cases that need some message: target file not found; `CreateProcessW` fails (the error code is the useful part); possibly "could not determine SSE2 support". A `MessageBoxW` with the product name as title, the file name and the system error number is enough; no resources or localisation machinery are needed for three strings. Because these are new user-visible texts, no wording is proposed here beyond the shape: what failed, which file, which error number.

## Antivirus and reputation

An unsigned small executable that starts a sibling executable and exits quickly matches the classic "dropper" shape that heuristics look at. Not measured here (nothing was built). Mitigations, each with its own cost: a real version resource and manifest, no packing or self-modifying code, no network, keeping the launcher in the same signed/zipped package as the players, and code signing if the project ever signs releases. Old XP-era scanners are less likely to object than current Windows Defender / SmartScreen on a modern machine, but the XP package is also unzipped on modern machines by testers.

## Cost estimate

- Code: roughly 120 to 200 lines of C++ (path building 20, SSE2 check 10, command-line tail 25, `CreateProcessW` with startup-info forwarding 25, activation 15, exit-code handling 10, error dialogs 20) plus a `.rc` file (icon, version, manifest, about 30 lines).
- Build and package: one more CMake target (about 10 lines), the packaging script and the release layout gain a third executable; the self-test package would not need it.
- Tests: a small unit test of the command-line tail parser (about 15 cases: quoted and unquoted program name, extra spaces, no arguments, spaces and quotes in arguments, trailing backslash, UNC path, a very long line); a manual matrix on real XP SP3 (double-click, drag onto the launcher, drag onto a shortcut, "Open with", launched from `cmd`, missing target, forced non-SSE2 path via the test override). A genuinely non-SSE2 machine is not available here; the override or an emulator that masks CPUID bit 26 covers that path.
- Effort: about one to two working days including the XP-machine round trips; ongoing: three binaries to keep in step in every release.

## Risks (top of the list)

1. Antivirus / reputation false positives on a tiny launcher (see above).
2. Activation and focus oddities when one process starts another (window behind others, busy cursor).
3. A user bypasses the launcher (double-clicks the SSE2 exe, drops a file on it) on a processor without SSE2 and gets an operating-system illegal-instruction error with no explanation.
4. Three binaries to version, sign and test together; mixed versions after a partial copy or update.
5. Crash reports and diagnostics name the real executable, not the launcher; support questions must say which variant ran.
6. Future single-instance or IPC work must be variant-agnostic (see above).

## Alternatives (neutral)

| Option | What it is | What it costs / gives |
|---|---|---|
| Single IA32 executable | Ship only the `/arch:IA32` build everywhere on XP. | Simplest: one binary, runs on any Pentium-class processor, no launcher. Measured cost on this PC: load about 2.0x, Analysis 1.2x to 1.4x slower than the SSE2 build (`XP_COST_LOG.md`). |
| Single SSE2 executable | Today's XP build. | Needs a Pentium 4 / Athlon 64 or newer; fastest of the two measured; unchanged behaviour. |
| Two executables, chosen by installer or user | Package contains both; the installer (or a README step) picks one, using the same SSE2 test as `--has-sse2`. | No launcher process, no spawn issues. A wrong manual choice produces an illegal-instruction crash on an old CPU. File associations point at the chosen file. |
| Launcher (this sketch) | Tiny IA32 exe picks and spawns one of the two at every start. | Automatic and survives copying the folder to another machine; costs the items above. |
| Runtime dispatch inside one executable | Build the program as IA32, but compile a few hot loops (the stage that dominates the large-file load) in a separate source file with `/arch:SSE2` and call it only after the SSE2 check. | One binary and a speed-up only where it matters. Hazards: inline functions and templates compiled in both files can be merged by the linker into the SSE2 copy (illegal instruction on old CPUs), so the SSE2 file must be isolated and re-scanned with the disassembly check after every change; two code paths to test; results of the two paths must stay bit-identical (the parity suite would need to run both). Gain not measured. |

No recommendation is made here; the numbers for the first two rows are in `XP_COST_LOG.md`.
