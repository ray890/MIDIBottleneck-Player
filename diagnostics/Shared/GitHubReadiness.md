# GitHub readiness

Build 22 is licensed under GPL-3.0-or-later and has a sanitized public-history candidate.

## Publication boundary

- Public history retains source, tests, build scripts, first-party icon assets, and project documentation.
- The private diagnostics archive is omitted from every outgoing commit because it contains raw provider logs, personal machine paths, and publication-irrelevant compatibility material.
- Provider DLLs, synthesizers, MIDI files, build output, PDBs, archives, and local test inputs are not tracked or packaged.
- External test material lives outside this project and is not part of the audit.
- Private `refs/codex-snapshots/*` and `refs/codex-safety/*` are local recovery state. Only `main` and explicit `v1.0.1`–`v1.0.22` tags may be pushed.

## Gate result

The sanitized candidate passed full-history path/secret/binary scans, `git fsck`, an isolated clean-clone build, 91/91 deterministic tests on x86 and x64, explicit package verification, icon/metadata checks, and both launch/normal-close smokes. Details and hashes are in [PublicationGate.md](PublicationGate.md).

External publication is blocked: GitHub CLI is unavailable, Git Credential Manager has no authenticated GitHub account, and the available browser is signed out. No remote or GitHub repository was created. The public repository is `Ray890/MIDIBottleneck-Player` only after Ray890 authenticates a safe write route and remote verification succeeds. Do not use mirror, all-ref, or wildcard pushes.
