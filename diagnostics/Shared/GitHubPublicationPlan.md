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
- build, packaging, compatibility, provenance, and Build 01–22 release notes;
- a concise public build index.

The clean release package contains only the two MIDIBottleneck executables, their matching configurations, `LICENSE`, `README.md`, and `SHA256SUMS.txt`.

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

## Current status

The complete local gate passed on 2026-09-20; see [PublicationGate.md](PublicationGate.md). Annotated historical tags and Release-page bodies are prepared locally. Publication stopped at the external authentication boundary: this host has no GitHub CLI account, Git Credential Manager has no GitHub account, and the in-app browser is signed out. No remote, public repository, pushed ref, tag, or Release entry exists yet.
