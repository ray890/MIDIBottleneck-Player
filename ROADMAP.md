# Roadmap

This is a direction of travel, not a promise of dates. Correct MIDI behavior, deterministic results, x86/x64 parity, and safe output lifecycles take priority over feature count.

## Near term

- **Drop oldest complete note** — when a finite queue is full, remove the oldest Note On together with its matching Note Off without scanning a large queue on every overflow. Playback and Analysis must make the same decision.

## Recently completed

- **Build 33 — direct event rate:** added an exact Events/sec service model shared by playback and Analysis, with generation-safe live changes and zero as unlimited/immediate service.
- **Build 32 — whole-state chase:** checked-by-default Play/Seek restoration for bank, Program, ordinary controllers, bend, pressure, and ordered RPN/NRPN values without replaying notes.
- **Build 31 — queue-state correctness:** finite mode now reports real backlog behind a blocked output, while an optional forward-only queue model can apply Drop newest or complete-note protection without deliberately delaying accepted MIDI.
- **Build 30 — human-facing refinement:** direct executable downloads, clearer large-file warnings, paused warning-dialog time, Per-note frame-rate reporting, stable Per-note effective speed, a public changelog, and simpler documentation.
- **Build 29 — large-file preflight:** exact cancellable counting and storage-aware memory estimates only for files large enough to justify a second pass.
- **Build 28 — direct compact parser:** bounded track readers, segmented provisional data, direct stable merge, segmented tempo/source histories, and no runtime configuration sidecar.
- **Build 27 — compact final store:** immutable 40-byte event records and segmented long payloads with allocation-free playback and Analysis readers.
- **Build 26 — gate and Analysis hardening:** growable note-occurrence storage, live channel-disable continuity, and an initial workload-only Analysis preview.
- **Build 25 — corrected Per-note behavior:** overlapping source notes no longer create first-channel ownership; repeated Note On strikes can retrigger through a clean release gap.

See [CHANGELOG.md](CHANGELOG.md) for the complete history.

## Needs a product decision

- **Channel override extensions** — presets, requested-versus-forced display, and optional static transformations that Analysis can model.
- **Track routing** — route tracks onto constrained hardware channels with explicit rules for program/controller conflicts, percussion, and overlapping merged notes.
- **Low-resolution presentation modes** — measured row hiding, roll-up, or non-cumulative scaling without destabilizing the canonical layout.

## Longer-term research or parked work

- legacy-Windows or cross-platform portability;
- compositor-synchronized rendering;
- helper-process isolation for permanently blocked native providers;
- producer/consumer output separation, which would redefine queue and lag semantics;
- further giant-file pause attribution after storage and paging pressure are reduced.

Detailed engineering notes remain in [Opportunities.md](Opportunities.md). User-visible boundaries are listed in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).
