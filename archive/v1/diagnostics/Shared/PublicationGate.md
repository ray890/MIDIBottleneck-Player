# Public release gate — Build 22

Prepared: 2026-09-20

## Candidate history

- 22 linear commits, with subjects `Build 01` through `Build 22`.
- Reconstructed author and committer dates match the documented original handoff chronology.
- Public identity: `Ray890 <11812575+Ray890@users.noreply.github.com>`, verified against GitHub account ID 11812575.
- Builds 01, 03–12, 15, and 16 remain labelled `nearest preserved`; the bounded snapshot audit did not recover a complete missing handoff tree for any of them.
- Historical commits contain no private diagnostics. Build 22 contains only the curated build index, public mapping, readiness record, publication plan, and this gate record.
- Full-history scans found no executables, DLLs, MIDI files, archives, PDBs, logs, personal absolute paths, credential patterns, or raw provider output.
- Private snapshot and safety references remain local and are not reachable from `main` or the explicit version tags.

## Clean-clone verification

The sanitized candidate was fetched into a separate empty repository and built without native-provider or private test material.

- x86 deterministic suite: 91/91 passed.
- x64 deterministic suite: 91/91 passed.
- The first x86 run had one transient responsive-layout timing-threshold failure (4,428 ms total). The focused 9-test UI group passed at 1,880 ms, and the decisive complete x86 rerun passed at 2,183 ms. No source change was made between those runs.
- PE machines: x86 `0x014C`; x64 `0x8664`.
- File version: `1.0.22.0`.
- Product version/informational identity: `Build 22 (2026-09-20)`.
- Managed form-icon and About-metadata regression tests passed in both suites; associated native icons were extracted from both executables.
- Both packaged applications passed the built-in `--launch-smoke` normal-close path.

## First-party package

The explicit allowlist produced seven files: the two applications, two matching configurations, `LICENSE`, `README.md`, and `SHA256SUMS.txt`.

| File | SHA-256 |
|---|---|
| `MIDIBottleneck Player x86.exe` | `058AE730DAD1BF7E0A7A08FC3D7A9D5C2C669B8D22E5FA7E1EF3B48E4C3FF56A` |
| `MIDIBottleneck Player x86.exe.config` | `E485156E609127C9AD22068B9F5794E5DA96953FB6A316D6B818D9A737AEC7F6` |
| `MIDIBottleneck Player x64.exe` | `DDF59C1A2C8B75953E617FCACD242394833D652D2E7E51D1F1EC7A03C9DA7A56` |
| `MIDIBottleneck Player x64.exe.config` | `E485156E609127C9AD22068B9F5794E5DA96953FB6A316D6B818D9A737AEC7F6` |

The checked-out Windows package copy of `LICENSE` uses CRLF line endings; the tracked text is the official GPLv3 text.

## External publication result

This section records the original gate and its resolution. At the time of the clean local gate, external publication was waiting for authentication. Ray890 subsequently authenticated Git Credential Manager, the write route was verified without exposing a token, and publication completed on 2026-09-20.

The public repository is [Ray890/MIDIBottleneck-Player](https://github.com/ray890/MIDIBottleneck-Player). Only sanitized `main` and the explicit Build 01–22 tags were pushed. All 22 Release pages were created, Build 22 assets were independently downloaded and checksum-verified, and a fresh public clone passed 91/91 tests on both architectures. No private snapshot/safety refs were published.
