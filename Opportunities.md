# Engineering opportunities

These are evidence-backed future directions, not promises for a particular release. The shorter public view is [ROADMAP.md](ROADMAP.md).

## Near term

### Direct Events per second rate input

Add **Events per second** as a third visible Rate model rather than reinterpreting Queue limit. Processing time already defines service rate, while Queue limit independently defines burst capacity. The direct model should use fixed-point/rational service accumulation so selected rates are not distorted by integer-microsecond reciprocals; switching models should preserve the nearest equivalent behavior. Playback and Analysis must share the calculation and Build 31's forward-only policy restrictions remain unchanged.

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
- **Build 31 — queue-state correctness:** distinguishes real blocked-output backlog from virtual rate-model pressure and applies forward-only Drop newest/complete-note decisions without delaying accepted MIDI.

## Playback and MIDI state decisions

### Whole-state chase on Play/Seek

Completed in Build 32. The checked-by-default power-user option uses segmented controller/parameter histories and event-index lower bounds, restores ordered state before later source events, respects disabled channels and overrides, and excludes notes, SysEx, poly pressure, system/meta messages, and channel-mode commands.

### Channel-override extensions

Current overrides safely cover Bank MSB/LSB, Program, Volume, Expression, Pan, Sustain, pitch bend, and channel aftertouch. Source conflicts are filtered before scheduler admission; explicit controls remain ordered. The Channel Monitor also supports one-attribute source-value chase.

Future decisions include presets, simultaneous requested/forced display, and whether saved/static transformations should affect Analysis. Automatic whole-song state chase remains separate.

### Track routing for constrained hardware

A possible static path is:

`source event → optional track-to-output-channel route → queue/service model → runtime override filter → MIDI output`

The parser would need retained track names. The design must handle multiple source programs/channels, merged overlapping notes, bank/controller/bend conflicts, channel-10 percussion, ownership of conflicting state, and warnings for lossy routes. One editable channel cell per track is not automatically safe. The intended planner targets a configurable number of melodic destination channels: merge exact-program material first, then consider instrument-family similarity and temporal overlap. Percussion needs an explicit policy, controller/bend conflicts need warnings, every destination needs a representative patch, and manual routing overrides must remain available.

### Build 31 forward-only finite capacity

Completed for **Drop newest** and **Drop incoming complete notes**. Playback and Analysis share the bounded pressure model, accepted output is not delayed, and real native/scheduler backlog is displayed separately. Drop oldest and clear/catch-up still require Simulate slowdown; supporting them without delay would require an ordered producer/consumer contract and cannot honestly be emulated after output is sent.

## Presentation decisions

### Low-resolution and alternate modes

- Hiding Statistics or Processing controls requires measured row collapse, constraint recomputation, and exact restoration.
- Runtime scaling should derive each size non-cumulatively from the canonical 100% layout. Repeated `Control.Scale()` is unsafe with native controls, fonts, DPI, and compact minimums.
- A Shift-revealed 25% item may be an undocumented joke, but must not be selectable.
- A mini title bar changes taskbar/Alt-Tab identity and non-client size.
- Inactive opacity needs layered-window accessibility and recovery rules.
- Roll-up needs exact compact/default size restoration.
- Classic-Windows presentation is best treated as a startup mode because visual styles and fonts affect the entire measured layout.

Build 32's placement audit kept Per-note gate, forward-only queue admission, state chase, and Always on top in the system menu. They are model/session switches used less often than transport controls, and no superior visible location fits both default and compact layouts without adding clutter.

### Help presentation

Native `WS_EX_CONTEXTHELP` conflicts with the principal windows' minimize/maximize styles. A future system-menu Help command, F1 help, or custom presentation is worthwhile only when it opens useful guidance; do not add a decorative question mark.

## Public project maintenance

The public repository uses explicit first-party packaging and explicit `main`/tag pushes. Build 31 continues to publish versioned loose executables plus checksums. Private diagnostics, provider logs, machine paths, and recovery refs stay local.

A bounded GitHub Actions trial was retired because realized WinForms geometry tests depend on interactive desktop, font, and working-area metrics. Future CI should separate display-independent tests or provide a controlled interactive desktop instead of weakening layout assertions.

## Longer-term / high-risk research

- **Compositor synchronization:** current painting is responsive; a monitor-derived timer is not real compositor synchronization.
- **Helper-process native-output isolation:** could recover from a driver call that never returns, but changes IPC, timing, SysEx ownership, and output lifetime.
- **Producer/consumer output separation:** might isolate provider latency, but redefines backlog, lag, cancellation, finite pressure, and Stop/Seek semantics; providers may not be thread-safe.
- **Extreme-load pause attribution:** further claims need isolated ETW/GC/commit/page-fault evidence after the compact storage improvements.
- **Legacy Windows or cross-platform portability:** a longer-term direction, not a current compatibility promise.
- **Static Per-note Analysis:** exact projection means running the shared gate state machine offline and graphing its admitted transitions/events per second. A theoretical frame-rate line alone is not equivalent.
- **Contextual help:** one F1/system-menu Help window could open at the section associated with the focused control. It should provide real guidance rather than decorative title-bar behavior.
