# Roadmap

This is a direction of travel, not a promise of dates. Correct MIDI behavior, deterministic results, x86/x64 parity, and safe output lifecycles take priority over feature count.

## Near term

- The next feature is not committed to a release. Remaining presentation modes and playback-model additions need separate design and verification.

## Recently completed

- **Build 38 — main numeric refinement:** the Processing and Queue fields size to the visible number, and processing scrubbing follows the selected rate scale without rounding exact typed values. UI scaling remains a separate measured design.
- **Build 37 — interface refinements:** clearer statistics, graph-hover Space transport, ordered observed-state Channels opening, scrub-or-type main numeric fields, finer low-rate Events/sec adjustment, and a segmented FIFO recovery for older finite-queue policy throughput.
- **Build 36 — session view controls:** two checked-by-default system-menu commands independently hide Processing model and Statistics, collapsing their rows in standard and compact layouts without changing playback or model state.
- **Build 35 — oldest complete-note overflow:** a finite delayed queue can remove the oldest eligible unsent note and its matching release without scanning the queue. Protected releases may raise occupancy above the configured soft limit; Analysis applies the same source-file policy.
- **Build 34 — uninterrupted rate edits and Channels provenance:** live rate changes keep the queue and sounding state, while source-indexed channel values appear as historical when output has not confirmed them.
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
- **Other low-resolution presentation modes** — roll-up or non-cumulative scaling without destabilizing the canonical layout. Session-only section hiding is complete.
- **Exact static Per-note Analysis** — would run the same gate state machine offline and graph actual admitted transitions, with Playback/Analysis parity tests; a frame-rate reference line alone would not be an exact projection.
- **Rate-model presentation** — consider more discoverable grouping of None, simulated slowdown, and note gating without changing their established scheduler behavior.
- **Note-release and real-time rate ideas** — a delayed matching NoteOff or a separate drop-based Events/sec cap each needs its own note-pairing and safety contract before becoming a control.

## Longer-term research or parked work

- legacy-Windows or cross-platform portability;
- compositor-synchronized rendering;
- helper-process isolation for permanently blocked native providers;
- producer/consumer output separation, which would redefine queue and lag semantics;
- further giant-file pause attribution after storage and paging pressure are reduced.

Detailed engineering notes remain in [Opportunities.md](Opportunities.md). User-visible boundaries are listed in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).
