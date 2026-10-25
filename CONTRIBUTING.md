# Contributing to MIDIBottleneck Player

Thank you for helping improve MIDIBottleneck Player.

## Development environment

Version 2 is a portable C++17 program with a plain Win32 interface. You need Windows 10 or 11 and the Visual Studio Build Tools 2022 (MSVC v143, a Windows SDK and the CMake tools); the Windows XP build also needs the v141 XP tools. No package manager and no third-party library is used. [`v2/BUILDING.md`](v2/BUILDING.md) lists the exact components, one install command, and every build and check command. In short, from the repository root:

```powershell
cmake -S v2 -B v2/out/build-x64 -G "Visual Studio 17 2022" -A x64 -T v143
cmake --build v2/out/build-x64 --config Release
.\v2\out\build-x64\Release\midibn-tests.exe
.\v2\tools\parity.ps1
```

`cmake.exe` is installed with the Build Tools but is not on `PATH` by default; `v2/BUILDING.md` shows how to find it. `parity.ps1` must end with `PARITY: PASS`. A pull request that changes the engine, the file loader or the model code should also pass the mutation checks described there, and should be built for x64 and x86 (and XP if it touches anything XP-specific). During development, focused tests are welcome, but they do not replace the final run.

Version 1 (.NET Framework) is kept unchanged in `archive/v1` for reference; it is not maintained and changes to it are not accepted.

## Pull requests

Keep changes focused and preserve deterministic MIDI ordering, output safety, cancellation, and queue/statistics semantics. Version 2 keeps every version 1 feature; a change must not remove or weaken one. Call out any effect on:

- x64, x86 or XP behavior, or native structure layout;
- the playback/output hot path or allocations;
- Stop, Pause, Seek, output switching, SysEx, or failure handling;
- queue admission, overflow, filtering, or Analysis equivalence;
- compact/default layout or accessibility.

Include tests and, for visible changes, a concise screenshot of the player only. Never submit proprietary MIDI files, synthesizers, provider DLLs, driver logs, credentials, personal paths, or redistribution-uncertain test material.

Real-driver checks are optional integration work. Keep them bounded and separate from the deterministic tests, identify the driver and architecture, and do not terminate applications you do not own.

## Style and scope

The project intentionally uses plain C++17, the Win32 API, CMake and a small set of PowerShell scripts, with no third-party libraries. Prefer contained changes over framework migrations or broad rewrites. Preserve existing user files and unrelated working-tree changes.

By contributing, you agree that your contribution is licensed under GPL-3.0-or-later.
