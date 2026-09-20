# Contributing to MIDIBottleneck Player

Thank you for helping improve MIDIBottleneck Player.

## Development environment

- Windows 10 or Windows 11.
- .NET Framework 4.8, or a compatible installed .NET Framework 4.x runtime and compiler.
- Windows PowerShell 5.1 or newer.

No NuGet restore or network download is required. From the repository root, run:

```powershell
.\build.ps1 -Test
```

This builds and tests explicit x86 and x64 applications. A pull request that changes production behavior should pass both complete deterministic suites. During development, focused test groups are welcome, but they do not replace the final cross-architecture run.

## Pull requests

Keep changes focused and preserve deterministic MIDI ordering, output safety, cancellation, and queue/statistics semantics. Call out any effect on:

- x86/x64 behavior or native structure layout;
- the playback/output hot path or allocations;
- Stop, Pause, Seek, output switching, SysEx, or failure handling;
- queue admission, overflow, filtering, or Analysis equivalence;
- compact/default layout or accessibility.

Include tests and, for visible changes, a concise screenshot. Never submit proprietary MIDI files, synthesizers, provider DLLs, driver logs, credentials, personal paths, or redistribution-uncertain test material.

Native-provider checks are optional integration work. Keep them bounded and separate from deterministic tests, identify the provider and architecture, and do not terminate applications you do not own.

## Style and scope

The project intentionally uses direct WinForms/.NET Framework source and a small PowerShell build. Prefer contained changes over framework migrations or broad rewrites. Preserve existing user files and unrelated working-tree changes.

By contributing, you agree that your contribution is licensed under GPL-3.0-or-later.
