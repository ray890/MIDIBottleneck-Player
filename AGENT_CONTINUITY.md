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

The user must explicitly choose the next build or rewrite. When that happens, create a concise `diagnostics/Build <number> - <name>/ProgressResume.md` containing the active scope, base identity, working-tree inventory, current hashes, accepted evidence, limitations, exact next actions, and applicable resource boundaries. Keep detailed chronology in a separate archive rather than ordinary startup context.

Portability, compositor synchronization, producer/consumer separation, helper-process MIDI isolation, and other deferred architecture items remain decisions for a future explicitly scoped phase. Do not begin them incidentally.
