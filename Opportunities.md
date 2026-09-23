# Engineering opportunities

These are evidence-backed future directions, not promises for a particular release. The shorter public view is [ROADMAP.md](ROADMAP.md).

## Near term

### Drop oldest complete note

Add a finite-queue policy that removes the oldest safe complete note: its Note On and matching Note Off. A production design needs intrusive pending-queue nodes plus per-channel/key occurrence links so both messages can be unlinked in amortized O(1), without scanning a large queue on every overflow.

The policy still needs exact rules for a Note Off that has not arrived, an event already in service, and a policy changed during playback. Playback and Analysis must share the same decision logic.

## Completed foundations

- **Build 18 — archive/history foundation:** corrected the private diagnostics chronology after a provenance audit. The sanitized public history keeps exact/nearest-preserved qualifications and excludes private evidence.
- **Build 25 — corrected Per-note gate:** separates source-note occurrences from one constrained Up/Down output state per pitch. Repeated Note On strikes can retrigger through a clean release gap; simultaneous layers and dense overload are deterministic.
- **Build 26 — gate/Analysis hardening:** uses growable occurrence segments, preserves surviving same-pitch support after a channel disable, and publishes a generation-safe workload-only Analysis preview.
- **Build 27 — compact final event store:** uses immutable 40-byte record segments and a segmented long-payload store. Playback and Analysis read compact views without per-event object allocation.
- **Build 28 — direct compact parser:** parses bounded track streams into provisional segments, merges directly into the final store, and segments tempo/source histories. No runtime configuration sidecar remains.
- **Build 29 — large-file preflight:** scans only files large enough to justify a second pass, then estimates open-file and loading-peak memory from exact SMF counts.
- **Build 30 — human-facing refinement:** clarifies memory warnings, gate statistics, downloads, documentation, release history, and roadmap. Effective speed now uses a monotonic gate-progress frontier.

## Playback and MIDI state decisions

### Whole-state chase on Play/Seek

The eventual product behavior would be enabled by default with a system-menu option to disable it. It should restore bank/program, controllers, bend/pressure, and ordered RPN/NRPN data-entry state before later notes, without replaying notes.

This needs a compact checkpoint index suitable for very large files, explicit SysEx exclusions or bounded rules, forced-override precedence, disabled-channel behavior, and exact reset-before-note ordering. Do not expose a partial menu command.

### Channel-override extensions

Current overrides safely cover Bank MSB/LSB, Program, Volume, Expression, Pan, Sustain, pitch bend, and channel aftertouch. Source conflicts are filtered before scheduler admission; explicit controls remain ordered. The Channel Monitor also supports one-attribute source-value chase.

Future decisions include presets, simultaneous requested/forced display, and whether saved/static transformations should affect Analysis. Automatic whole-song state chase remains separate.

### Track routing for constrained hardware

A possible static path is:

`source event → optional track-to-output-channel route → queue/service model → runtime override filter → MIDI output`

The parser would need retained track names. The design must handle multiple source programs/channels, merged overlapping notes, bank/controller/bend conflicts, channel-10 percussion, ownership of conflicting state, and warnings for lossy routes. One editable channel cell per track is not automatically safe.

### Virtual finite capacity without slowdown

A shadow queue can honestly reject current arrivals for forward-only policies. It cannot retract an event already sent, so Drop oldest or clear-buffer semantics become misleading without a new ordered producer/consumer contract and explicit real-versus-modeled queue/lag displays.

## Presentation decisions

### Low-resolution and alternate modes

- Hiding Statistics or Processing controls requires measured row collapse, constraint recomputation, and exact restoration.
- Runtime scaling should derive each size non-cumulatively from the canonical 100% layout. Repeated `Control.Scale()` is unsafe with native controls, fonts, DPI, and compact minimums.
- A Shift-revealed 25% item may be an undocumented joke, but must not be selectable.
- A mini title bar changes taskbar/Alt-Tab identity and non-client size.
- Inactive opacity needs layered-window accessibility and recovery rules.
- Roll-up needs exact compact/default size restoration.
- Classic-Windows presentation is best treated as a startup mode because visual styles and fonts affect the entire measured layout.

### Help presentation

Native `WS_EX_CONTEXTHELP` conflicts with the principal windows' minimize/maximize styles. A future system-menu Help command, F1 help, or custom presentation is worthwhile only when it opens useful guidance; do not add a decorative question mark.

## Public project maintenance

The public repository uses explicit first-party packaging and explicit `main`/tag pushes. Build 30 publishes versioned loose executables plus checksums. Private diagnostics, provider logs, machine paths, and recovery refs stay local.

A bounded GitHub Actions trial was retired because realized WinForms geometry tests depend on interactive desktop, font, and working-area metrics. Future CI should separate display-independent tests or provide a controlled interactive desktop instead of weakening layout assertions.

## Longer-term / high-risk research

- **Compositor synchronization:** current painting is responsive; a monitor-derived timer is not real compositor synchronization.
- **Helper-process native-output isolation:** could recover from a driver call that never returns, but changes IPC, timing, SysEx ownership, and output lifetime.
- **Producer/consumer output separation:** might isolate provider latency, but redefines backlog, lag, cancellation, finite pressure, and Stop/Seek semantics; providers may not be thread-safe.
- **Extreme-load pause attribution:** further claims need isolated ETW/GC/commit/page-fault evidence after the compact storage improvements.
- **Legacy Windows or cross-platform portability:** a longer-term direction, not a current compatibility promise.
