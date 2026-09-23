# Roadmap

This is a public product roadmap, not a promise of dates. Correctness, deterministic behavior, x86/x64 parity, and bounded native-output lifecycles take priority over feature count.

## Near term

- **Drop oldest complete note** — add a bounded data structure that can remove the oldest safe complete note without scanning a large queue on every overflow.

Build 25 corrected the live per-note interval gate so overlapping source notes no longer create first-channel ownership. Repeated attacks retrigger predictably under one transition per pitch/boundary, while Analysis continues to disclose that it excludes the live gate.

Build 26 removes the gate's former 16,384 simultaneously unmatched-note ceiling, restores surviving layered support after a live representative-channel disable, and adds a generation-safe workload-only preview for initial Analysis calculations.

Build 27 moves loaded songs to immutable compact record/payload segments, preserves the list backend as a deterministic oracle, and keeps workload preview data visible with a useful reason when queue projection fails.

Build 28 streams track chunks into compact provisional segments and performs timestamp assignment during the stable direct-to-final merge. Tempo and source-value histories are segmented, the post-merge conversion is gone, and neither architecture needs a runtime configuration sidecar.

Build 29 keeps ordinary files on that one-pass path and adds an exact, cancellable grammar scan only for unusually large inputs. Its architecture-aware warning reports exact dispatchable events plus projected retained and conservative peak MIDI memory before allocating compact stores.

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
