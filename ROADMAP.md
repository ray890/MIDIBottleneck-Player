# Roadmap

This is a public product roadmap, not a promise of dates. Correctness, deterministic behavior, x86/x64 parity, and bounded native-output lifecycles take priority over feature count.

## Near term

- **Compact segmented event storage** — replace per-event reference objects and the single contiguous final array in measured stages while preserving exact order, seeking, Analysis, SysEx, and diagnostics.
- **Large-file preflight** — provide cancellable exact event counting and a storage-aware memory estimate without misleading file-size guesses.
- **Drop oldest complete note** — add a bounded data structure that can remove the oldest safe complete note without scanning a large queue on every overflow.

Build 24 completed the first live per-note interval gate with deterministic pitch ownership, lifecycle reset, separate accounting, and explicit Analysis disclosure.

## Needs a product decision or more evidence

- **Whole-state chase on Play/Seek** — restore prior channel state without replaying notes, using a compact index and explicit rules for overrides, disabled channels, RPN/NRPN, and SysEx.
- **Virtual finite capacity without applied slowdown** — define honest semantics for policies that cannot retract events already sent.
- **Channel override extensions** — presets, requested-versus-forced display, and optional static transformations that Analysis can model.
- **Track routing** — route tracks onto constrained hardware channels with explicit handling for program/controller conflicts, percussion, and merged notes.
- **Low-resolution UI modes** — non-cumulative scaling, measured row hiding, roll-up, or classic presentation without destabilizing the canonical layout.

## Parked/high risk

- compositor-synchronized rendering;
- helper-process isolation for permanently blocked native providers;
- producer/consumer output separation that would redefine queue and lag semantics;
- further giant-file pause attribution before storage/paging pressure is materially reduced.

Detailed engineering evidence remains in [Opportunities.md](Opportunities.md), while user-visible limitations are listed in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).
