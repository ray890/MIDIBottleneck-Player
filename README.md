# MIDIBottleneck Player

[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

MIDIBottleneck Player is a Windows MIDI player and deterministic workload simulator for studying output throughput, queue pressure, lag, and overflow. It focuses on dense MIDI workloads and diagnostic controls that are difficult to find together in an ordinary player.

**[Download the latest release](https://github.com/ray890/MIDIBottleneck-Player/releases/latest)**

![MIDIBottleneck Player main window](docs/images/main-window.png)

## What it does

- Plays Standard MIDI Files through WinMM or a compatible KDMAPI provider.
- Offers a **None** output for measuring the scheduler without opening a MIDI device.
- Simulates a configurable per-event processing cost or MIDI serial bitrate.
- Models unlimited and finite queues with several overflow policies.
- Shows live throughput, queue, lag, effective-speed, and channel statistics.
- Provides whole-file Analysis graphs for event density, bitrate, queue pressure, and predicted output completion.
- Loads files asynchronously with cancellation, progress, elapsed time, and process-private committed memory.
- Handles unusually dense files with packed short-message storage and an x64 very-large-array configuration.
- Supports drag-and-drop loading, a compact layout, x86/x64 builds, and a read-only/override-capable 16-channel monitor.

MIDIBottleneck is an independent project. It is not based on, and is not intended to compete with, any particular MIDI player.

## Requirements

- Windows 10 or Windows 11.
- .NET Framework 4.8, or a compatible installed .NET Framework 4.x runtime.
- A user-supplied MIDI output when using WinMM or KDMAPI. No provider or synthesizer is bundled.

Windows 7 SP1 has not been tested. Windows XP and cross-platform support are possible future directions, not current compatibility guarantees.

## Quick start

1. Download the x64 or x86 ZIP from the [latest release](https://github.com/ray890/MIDIBottleneck-Player/releases/latest).
2. Extract the complete ZIP. Keep the executable and its adjacent `.exe.config` together.
3. Run `MIDIBottleneck Player x64.exe` on a typical modern system, or the x86 build when a 32-bit MIDI environment requires it.
4. Choose a MIDI output, then open or drag one `.mid`/`.midi` file onto the window.
5. Leave **Simulate slowdown** and **Queue length limit** off for ordinary uncapped playback, or enable the model you want to study.

## Output choices

- **WinMM** uses the standard Windows Multimedia MIDI API. Native wrappers and devices must match the executable architecture where applicable.
- **KDMAPI** uses a compatible direct-output provider selected by the checkbox. A deliberately supplied application-local provider takes precedence over an installed provider and must expose the required ABI.
- **None** logically accepts output without opening a native device. Parsing, timing, queue behavior, statistics, SysEx handling, and overrides still run, making it useful for scheduler-only measurements.

## Processing and queues

The processing model can apply a fixed time per event or MIDI serial bitrate. A finite queue can then use drop-newest, drop-oldest, clear-and-catch-up, or complete-note-aware overflow behavior. The queue count includes the item in service and can temporarily exceed its configured soft ceiling when a protected NoteOff or non-note message must be retained for output safety.

Analysis uses the immutable source file and selected simulator settings. Live Channel Monitor mutes and overrides intentionally do not rewrite the static Analysis projection.

## Documentation

- [User guide](docs/USER_GUIDE.md)
- [Technical reference](docs/TECHNICAL_REFERENCE.md)
- [Performance and compatibility notes](docs/PERFORMANCE_COMPATIBILITY.md)
- [Public roadmap](ROADMAP.md)
- [Building and testing](docs/BUILDING.md)
- [Release packaging](docs/PACKAGING.md)
- [Known limitations](docs/KNOWN_LIMITATIONS.md)
- [Release history](docs/releases/)
- [Public-history provenance](docs/PUBLIC_HISTORY.md)

## Build and test

From Windows PowerShell:

```powershell
.\build.ps1 -Test
```

The build uses the installed .NET Framework compiler, produces explicit x86 and x64 applications, verifies their PE machine fields, and runs the deterministic test suite in both processes. It requires no NuGet packages or native MIDI provider.

See [Building and testing](docs/BUILDING.md) for prerequisites and the separately gated native-provider integration option.

## Contributing and security

Focused bug reports and pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting changes. Report security issues privately as described in [SECURITY.md](SECURITY.md).

## License

MIDIBottleneck Player is free software under **GPL-3.0-or-later**. Copyright © 2026 Ray890. See [LICENSE](LICENSE).

## Development transparency

The project was developed with Codex using separate planning/review and implementation tasks. Its early history was reconstructed from retained milestones because Git commits did not originally exist; exact and nearest-preserved states are identified in the [public-history notes](docs/PUBLIC_HISTORY.md).
