# MIDIBottleneck Player — Agent Continuity

Read this file first in a fresh implementation or planning/audit session.

## Current boundary

Build 44 / `1.0.44.0` is published and closed at commit `3f108be527efcc584b367edaffbee2bfdc28a49e`, annotated tag `v1.0.44`, and release `https://github.com/ray890/MIDIBottleneck-Player/releases/tag/v1.0.44`. There is no active unfinished build.

Read next:

1. `diagnostics/Shared/ProjectState.md` for the published baseline and product decisions.
2. `diagnostics/Shared/WorkflowProtocol.md` for provider-neutral working rules.

Do not load archived conversations or the full Build 44 chronology during ordinary startup. Use `diagnostics/Build 44 - shutdown-observability/ProgressResume.md` only as a short closure pointer; consult its linked archive only for a specific historical question, audit, or publication reconciliation.

## Authority and roles

- An implementation agent may change the repository, run tests, and prepare releases only within the user's accepted scope. Publication, destructive operations, and meaningful scope expansion require explicit user authority.
- A planning/audit agent is read-only unless the user explicitly authorizes a repository-changing action.
- Current user instructions outrank durable documents.

Before any change, verify repository identity and scoped working-tree state read-only. Never rewrite a published commit or tag.

## Beginning the next phase

The .NET v1 line is in maintenance; no further v1 feature builds are planned. v1.0.44 (`3f108be`) is the frozen behavioral reference for v2. The recorded next phase is **v2 Milestone 1**, which awaits its own explicit scoping prompt from the user. The v2 plan and requirements are recorded in `ROADMAP.md` and `Opportunities.md`. Until that prompt arrives, do not create a v2 folder or branch, write v2 code, or change v1 behavior to address v2 requirements.

When the user starts a scoped phase, create a concise `ProgressResume.md` for it (for a v1 maintenance build, `diagnostics/Build <number> - <name>/`; for v2, a location the scoping prompt defines) containing the active scope, base identity, working-tree inventory, current hashes, accepted evidence, limitations, exact next actions, and applicable resource boundaries. Keep detailed chronology in a separate archive rather than ordinary startup context.

Portability, compositor synchronization, producer/consumer separation, and helper-process MIDI isolation (backend) are now v2 design questions. Do not begin them incidentally.
