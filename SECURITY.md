# Security policy

## Supported version

Security fixes target the latest published MIDIBottleneck Player 2.x release. Version 1 (the last release is v1.0.44) is no longer maintained; its files are kept unchanged in `archive/v1` and on the `v1-maintenance` branch for reference. Older historical tags document prior milestones and are not maintained separately.

## Reporting a vulnerability

Please use GitHub's **Report a vulnerability** private-reporting feature for this repository. Do not disclose an exploitable driver crash, unsafe native-call boundary, credential exposure, or similar issue in a public issue before it can be triaged.

Include the program version, the build (x64, x86 or XP), the Windows version, the selected output (WinMM, Direct WinMM, KDMAPI or None), a bounded reproduction, and whether any test input can legally be shared. Do not upload proprietary MIDI files, provider binaries, memory dumps containing personal data, or credentials.

Ordinary playback bugs and deterministic model discrepancies may use the public issue templates.
