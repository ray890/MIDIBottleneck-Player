# GitHub publication record

Prepared: 2026-09-20

Repository target: `Ray890/MIDIBottleneck-Player`  
License: GPL-3.0-or-later  
Copyright holder: Ray890

## Sanitization decision

The public candidate preserves the chronological source/test/root-file history while removing the entire private diagnostics subtree from historical commits. Build 22 adds only a curated provenance index and publication-safe planning record. This avoids publishing personal absolute paths, raw provider/native-probe output, private manifests, or hundreds of screenshots whose pixels were not all suitable for public review.

No complete missing handoff tree was recovered during the bounded protected-snapshot audit. `nearest preserved` milestones remain explicitly qualified; no source lines were inferred from screenshots or changelogs.

## Public contents

- application source and deterministic tests;
- first-party icon assets and manifests;
- build and explicit allowlist packaging scripts;
- README, GPL license, contribution/security policy, issue and pull-request templates;
- build, packaging, compatibility, provenance, and versioned release notes;
- a concise public build index.

Current releases use two explicit architecture ZIPs plus `SHA256SUMS.txt`. Each ZIP contains its executable, `LICENSE`, and `README.md`; x64 additionally includes its required `.exe.config`, while x86 deliberately has no ineffective configuration sidecar.

## Repository presentation

Suggested description:

> A Windows MIDI player and deterministic workload simulator for studying output throughput, queue pressure, lag, overflow, and dense Standard MIDI Files.

Suggested topics: `midi`, `winforms`, `windows`, `midi-player`, `performance-testing`, `queue-simulation`, `winmm`, `kdmapi`, `dotnet-framework`.

Use GitHub private vulnerability reporting as the security contact. Public issues and pull requests use the tracked templates. Version tags follow `v1.0.<Build>`; assembly/file versions follow `1.0.<Build>.0`. Historical release bodies show `Original handoff date` because GitHub's Release page date is the later publication date.

## Safe publication procedure

1. Verify the authenticated GitHub account and its GitHub-provided noreply identity.
2. Create an empty public `Ray890/MIDIBottleneck-Player` repository without generated files.
3. Add and inspect only the intended `origin`.
4. Push `main` explicitly, then push only `refs/tags/v1.0.1` through `refs/tags/v1.0.22` explicitly.
5. Confirm no snapshot or safety refs exist remotely.
6. Create Build 01–22 Releases from the tracked release-note bodies. Historical entries have source tags/notes only unless an exact original binary survives.
7. Upload only the current verified first-party package files to v1.0.22.
8. Set description, topics, default branch, and private vulnerability reporting.
9. Clone the public repository anew and repeat the decisive build/test/package/checksum gate.

Never push with `--mirror`, `--all`, `--tags` without an explicit reviewed tag set, or wildcard refspecs. Do not create or store tokens in this repository.

## Publication result

The local gate passed on 2026-09-20; see [PublicationGate.md](PublicationGate.md). Git Credential Manager then provided an authenticated Ray890 write route. The public repository is [Ray890/MIDIBottleneck-Player](https://github.com/ray890/MIDIBottleneck-Player).

The sanitized `main`, explicit `v1.0.1`–`v1.0.22` tags, and 22 Release pages were published without exposing local snapshot/safety refs. Build 22's seven original loose assets were downloaded again, validated against `SHA256SUMS.txt`, and launch-smoked. A new public clone passed 91/91 deterministic tests on x86 and x64 and `git fsck` reported no structural problem.

Build 23 replaces the loose current-release layout with explicit x86/x64 ZIPs and a top-level checksum file. This record remains the publication procedure and provenance evidence; it is no longer an authentication-blocker notice.

Build 24 retains that layout and removes the x86 configuration sidecar after focused architecture verification. The x64 sidecar remains required until segmented storage eliminates the possible greater-than-2-GB contiguous reference array.
