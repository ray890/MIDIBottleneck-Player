# GitHub readiness

The project is licensed under GPL-3.0-or-later and was published from the sanitized public-history candidate on 2026-09-20. Later releases move forward without rewriting that history.

## Publication boundary

- Public history retains source, tests, build scripts, first-party icon assets, and project documentation.
- The private diagnostics archive is omitted from every outgoing commit because it contains raw provider logs, personal machine paths, and publication-irrelevant compatibility material.
- Provider DLLs, synthesizers, MIDI files, build output, PDBs, archives, and local test inputs are not tracked or packaged.
- External test material lives outside this project and is not part of the audit.
- Private `refs/codex-snapshots/*` and `refs/codex-safety/*` are local recovery state. Only `main` and individually reviewed version tags may be pushed.

## Gate result

The sanitized candidate passed full-history path/secret/binary scans, `git fsck`, an isolated clean-clone build, 91/91 deterministic tests on x86 and x64, explicit package verification, icon/metadata checks, and both launch/normal-close smokes. Details and hashes are in [PublicationGate.md](PublicationGate.md).

## Published state

The public repository is [Ray890/MIDIBottleneck-Player](https://github.com/ray890/MIDIBottleneck-Player). Build 01–23 tags and Release pages were published explicitly. Current releases use explicit x86/x64 ZIPs and a checksum manifest rather than enumerating the ignored `dist` directory. Build 22's original assets and the later ZIP layout were independently downloaded and verified during publication maintenance.

Only `main` and the explicit version tags were pushed. Private snapshot and safety refs remain local. Future publication must retain that explicit-ref rule and must not use mirror, all-ref, or wildcard pushes.
