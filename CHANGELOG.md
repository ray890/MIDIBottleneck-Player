# Changelog

This is the human-readable history of MIDIBottleneck Player. Dates are original handoff dates, not the later dates on which historical GitHub Release pages were published. See [Public history](docs/PUBLIC_HISTORY.md) for exact-versus-nearest source provenance.

## Build 01 — 2026-08-29 9:23 PM EDT

### Player changes

- Introduced the initial Windows Forms MIDI player.
- Added file playback and the first per-event processing and queue controls.

### Archive note

- The exact handoff tree is unavailable. The tag uses the nearest source snapshot captured during later Build 02 work; missing lines were not invented.

## Build 02 — 2026-08-29 10:56 PM EDT

### Player changes

- Kept SysEx buffers alive through the complete native send lifecycle.
- Hardened Stop and Seek cleanup while preserving event order.

### Verification

- The exact implementation snapshot survives. No contemporaneous suite total was recorded.

## Build 03 — 2026-08-30 2:32 AM EDT

### Player changes

- Added bounded queue overflow and event dropping.
- Added early live queue/playback statistics and capacity measurements.

### Archive note

- Seven private diagnostic artifacts survive, but no reliable suite total or exact source tree does.

## Build 04 — 2026-08-30 9:21 AM EDT

### Player changes

- Separated processing slowdown from queue-limit controls.
- Introduced whole-file Workload Analysis for event density, bitrate, and modeled dropping.

### Archive note

- The exact tree and a trustworthy suite total are unavailable; the nearest preserved source is used.

## Build 05 — approximately 2026-08-30 9:04 PM EDT

### Player changes

- Refined compact layout, minimum sizing, Analysis presentation, and live-rate display.

### Archive note

- Four visual artifacts survive. The exact handoff time/tree is unavailable; a shortly later snapshot is the nearest implementation evidence.

## Build 06 — 2026-09-02 1:11 AM EDT

### Player changes

- Hardened WinMM and KDMAPI handling, architecture-correct MIDI headers, SysEx lifetime, and live processing controls.
- Added explicit x86/x64 builds and expanded Analysis/UI verification.

### Archive note

- The exact tree is unavailable. Bounded native evidence was mixed and is not represented as a compatibility success.

## Build 07 — 2026-09-03 12:07 PM EDT

### Player changes

- Added cancellable asynchronous MIDI loading and background Analysis.
- Completed progress reporting, graph navigation, cleanup, compact layouts, and bounded performance diagnostics.

### Archive note

- Several intermediate work stages belong to this single release. The exact final tree is unavailable; the nearest preserved snapshot is used.

## Build 08 — 2026-09-07 12:18 PM EDT

### Player changes

- Refined WinMM/KDMAPI compatibility handling.
- Added two-level loading progress and improved Analysis resolution, splitter behavior, compact restoration, and benchmarks.

### Archive note

- The tag uses the nearest preserved source; 54/54 deterministic tests were reported on each architecture.

## Build 09 — 2026-09-08 7:39 AM EDT

### Player changes

- Removed expensive diagnostic work from successful WinMM short-message sends.
- Restored live queue publication during dense output.
- Reused Analysis workload scans when only simulator settings changed.

### Archive note

- The tag uses the nearest preserved source. Incomplete native probes are not presented as performance proof.

## Build 10 — 2026-09-10 1:03 AM EDT

### Player changes

- Hardened output-worker lifetime and outbound timing.
- Improved publication cadence, compact layout, and ordered-output diagnostics.

### Archive note

- The tag uses the nearest preserved source; 62/62 tests were reported on each architecture.

## Build 11 — 2026-09-11 8:19 PM EDT

### Player changes

- Made Pause and Stop safe when a native output call is delayed.
- Added complete-note-aware overflow behavior and faster clear-and-catch-up.
- Added aligned adaptive time ticks to Analysis.

### Archive note

- The tag uses the nearest preserved source; 65/65 tests were reported on each architecture.

## Build 12 — 2026-09-12 10:58 AM EDT

### Player changes

- Added the explicit **None** output for silent scheduler playback and benchmarks.
- Clarified which measurements belong to the application rather than a provider or synthesizer.

### Archive note

- The tag uses the nearest preserved source; 68/68 deterministic tests were reported on each architecture.

## Build 13 — 2026-09-17 12:30 AM EDT

### Player changes

- Added the indexed event-store boundary and inline packed storage for ordinary short messages.
- Fixed Analysis Cancel restarting itself after layout changes.
- Made Auto resolution show the accepted graph interval.
- Added corrected loading telemetry and the first read-only 16-channel monitor.

### Verification

- The exact source is preserved; 73/73 deterministic tests were reported on each architecture.

## Build 14 — 2026-09-17 12:36 PM EDT

### Player changes

- Added safe live overrides for bank, program, volume, expression, pan, sustain, pitch bend, and channel aftertouch.
- Kept Analysis windows across file replacement without retaining the old song.
- Refined loading width and Channel Monitor lifecycle behavior.

### Verification

- The exact source is preserved; 75/75 deterministic tests were reported on each architecture.

## Build 15 — 2026-09-17 6:52 PM EDT

### Player changes

- Renamed the product to MIDIBottleneck Player.
- Added the application icon, About/build identity, live override editor, and persistent detached Analysis/Channel Monitor shells.

### Archive note

- The tag uses the nearest preserved source; the handoff reported 78/78 tests on each architecture.

## Build 16 — 2026-09-18 2:27 AM EDT

### Player changes

- Replaced popup override dialogs with one reusable scrub-or-type editor.
- Refined compact widths, icon assignment, and form layout.

### Archive note

- The exact handoff tree does not survive; Build 17 is the next complete accumulated source. The handoff reported 81/81 tests on each architecture.

## Build 17 — 2026-09-18 1:33 PM EDT

### Player changes

- Fixed first-gesture handoff in the Channel Monitor scrub/type editor.
- Distinguished current, forced, and historical values.
- Added one-shot channel controls, per-channel muting, measured monitor fitting, and cleaner Analysis geometry.

### Verification

- The complete source is preserved; 83/83 deterministic tests were reported on each architecture.

## Build 18 — 2026-09-18 8:20 PM EDT

### Archive and repository milestone

- Established recovery references, a provenance-aware diagnostics archive, reconstructed Git history, and the build index.
- Made no new application-behavior claim; the application source matches Build 17.
- Hash-accounted 437 private artifacts, which were deliberately excluded from the sanitized public history.

## Build 19 — 2026-09-18 9:19 PM EDT

### Player changes

- Prevented an unchanged editor click from creating an override.
- Added non-blocking acknowledgement for one-shot channel messages.
- Refined Channel Monitor sizing, labels, and interaction.

### Verification

- The complete handoff source is preserved; 84/84 deterministic tests were reported on each architecture.

## Build 20 — 2026-09-19 11:20 AM EDT

### Player changes

- Moved disabled-channel and forced-override filtering before queue admission.
- Filtered source events no longer consume simulated service, queue space, output rate, or sent counts.
- Kept mute/override filtering separate from queue-overflow drops.
- Made assembly metadata the single runtime source of build/version identity.

## Build 21 — 2026-09-19 5:22 PM EDT

### Player changes

- Made right-click on a historical Channel Monitor value restore the latest applicable source-file value for that one attribute.
- Added a compact lookup index suitable for very large songs.
- Waited for ordered output confirmation before changing the cell from historical to current; failures stayed visible and retryable.

### Important note

- This is deliberate one-attribute source chase, not automatic whole-state or note reconstruction.

## Build 22 — 2026-09-20 4:21 AM EDT

### Player changes

- Added one-file drag-and-drop loading over the complete main window.
- Added predicted accepted-output completion and source-end overrun to Queue Projection.
- Added a session-only **Always on top** system-menu option.

### Project and repository changes

- Completed the sanitized public history, GPL-3.0-or-later licensing, contribution/security documents, and verified first-party release packaging.
- The complete deterministic suite passed 91/91 on both architectures.

## Build 23 — 2026-09-21 12:12:11 AM EDT

### Player change

- Fixed a scheduler wake/reset race that could delay a live Channel Monitor command until the next source event.

### Project and distribution changes

- Reorganized the public documentation and added the public-safe screenshot and roadmap.
- Rewrote historical Release descriptions and established architecture-specific packaging.
- Investigated hosted CI, then documented why realized WinForms layout tests need controlled interactive-desktop metrics.

## Build 24 — 2026-09-21 7:20 AM EDT

### Player changes

- Introduced the first Per-note interval gate and separate gate-filter accounting.
- Added safe live mode/interval changes through the existing silence-and-restart boundary.
- Used a first-NoteOn/channel ownership interpretation that could suppress later valid same-pitch strikes; Build 25 corrected that behavior.

### Distribution changes

- Removed the ineffective x86 configuration sidecar. The then-current x64 storage still required its sidecar.

## Build 25 — 2026-09-22 12:48 AM EDT

### Player changes

- Replaced Build 24's first-channel ownership model with distinct source-note occurrences and one global Up/Down output state per pitch.
- Allowed repeated Note On strikes of a sounding pitch to retrigger through a clean one-interval release gap.
- Coalesced truly simultaneous layers deterministically, retained the strongest velocity, and gave short notes a complete interval.
- Added bounded one-boundary look-ahead so dense overload is thinned without an unbounded transition queue.

### Important note

- Static Analysis still excludes this live-only gate and says so explicitly.

## Build 26 — 2026-09-22 5:12 AM EDT

### Player changes

- Replaced the Per-note gate's fixed 16,384-occurrence pool with reusable 4,096-entry segments.
- Prevented overflowed notes from retiring a different track's valid occurrence.
- Restored a sounding pitch from surviving enabled support after its representative channel is disabled.
- Added one immutable workload-only Analysis preview before queue projection completes.
- Kept completed graphs visible during recalculation and rejected stale preview/final generations independently.

### Verification

- The complete deterministic suite passed 93/93 on x86 and 93/93 on x64.

## Build 27 — 2026-09-22 7:55 AM EDT

### Player changes

- Replaced final per-event objects and reference slots with immutable 40-byte records in bounded segments.
- Stored long SysEx/system payloads in a byte-exact segmented side store.
- Moved playback, seeking, Analysis, diagnostics, channel state, source lookup, SysEx assembly, and the Per-note gate to compact event views.
- Preserved useful workload-preview data and a concise reason when queue projection fails.

### Memory result

- The one-million-event x64 fixture's retained managed memory fell from about 81.3 MB to 41.3 MB.
- Parsing still used legacy per-track objects and a merged list; Build 28 removed those remaining structures.

## Build 28 — 2026-09-22 12:50 PM EDT

### Player changes

- Replaced whole-track byte arrays and per-event parsing objects with a bounded reader and compact provisional segments.
- Performed the stable tick/track/source-order merge directly into the final compact store while assigning timestamps.
- Segmented tempo histories and one-value source-chase histories.
- Removed the post-merge conversion and progressively released consumed provisional data.

### Distribution and verification

- Removed the last supported need for `gcAllowVeryLargeObjects`; both architectures became configuration-free.
- The complete deterministic suite passed 95/95 on each architecture.

## Build 29 — 2026-09-22 8:10 PM EDT

### Player changes

- Added a cancellable, allocation-light count-only scan for files large enough to justify a second pass.
- Kept ordinary files on the direct one-pass compact parser.
- Counted events, long payloads, tempo changes, and source-value history before allocating the production stores.
- Added architecture-aware open-file/loading-peak estimates and Continue/Cancel warning behavior.

### Verification and limits

- The complete deterministic suite passed 96/96 on x86 and 96/96 on x64.
- The generated 250,000-event scan used roughly 74 KiB and no Gen0 collection in the recorded run.
- Estimates cannot guarantee available commit, fragmentation, or paging behavior.

## Build 30 — 2026-09-22 10:50 PM EDT

### Player changes

- Reworded the Large MIDI warning around events to process, expected memory while open, and highest expected loading use.
- Pauses and subtracts the user's warning-dialog decision time from the loading timer.
- Shows the Per-note interval as a frame frequency and explains why it is not total MIDI throughput.
- Keeps event-level lag behavior and gives Effective speed a monotonic gate-progress frontier that advances through sparse work but stalls behind blocked output.

### Project and distribution changes

- Changed current and future downloads to loose, versioned x86/x64 executables plus checksums.
- Added this public changelog and simplified the README, roadmap, packaging instructions, and release descriptions.

## Build 31 — 2026-09-23

### Player changes

- Corrected finite-queue statistics so MIDI that becomes due behind a blocked output call appears in the real unsent backlog, even when simulated slowdown is off.
- Excludes muted channels and source attribute changes blocked by forced overrides from that backlog.
- Added the checked-by-default **Apply queue limit without slowdown** option. It uses the selected rate model for virtual pressure and forward-only Drop newest or complete-note rejection while sending accepted MIDI immediately.
- Shows actual backlog separately from the virtual pressure bar and prevents policies that would claim to retract already-sent MIDI.
- Gives static Analysis the same forward-drop decisions while clearly excluding real driver/synthesizer blocking.

### Verification

- Deterministic blocked-output, filtering, complete-note, Analysis-equivalence, lifecycle, and system-menu coverage was added on both architectures.

## Build 32 — 2026-09-23

### Player changes

- Added checked-by-default MIDI-state chase when Play or Seek starts in the middle of a file.
- Restores bank and Program, ordinary controllers, pitch bend, channel pressure, and known RPN/NRPN parameter values before later source events.
- Preserves MIDI ordering and gives forced overrides precedence while skipping disabled channels.
- Deliberately does not replay notes, polyphonic key pressure, SysEx, system/meta messages, or channel-mode silence commands.

### Storage and safety

- Builds immutable segmented state histories during the compact merge; seeking uses binary lookup instead of rescanning the file.
- Reset, Pause/Resume, Seek, output replacement, cancellation, and output-failure behavior use the existing ordered safety boundary.

## Build 33 — 2026-09-23

### Player changes

- Added **Events per second** as a third visible service-rate model from 0 through 1,000,000 events/sec; zero means unlimited/immediate service.
- Uses an exact integer rational clock, so non-divisor rates such as 3,000 events/sec do not accumulate reciprocal-rounding drift.
- Shares service-phase rules among delayed playback, forward-only queue admission, and static Analysis. Rejected events consume no service phase.
- Converts directly between Processing time and Events/sec using the nearest whole equivalent. Active edits retire and restart the scheduler safely at the same source position.

### Verification

- Added deterministic boundary, drift, finite/unlimited queue, forward-drop, conversion, responsive-layout, live-restart, and whole-state-chase interaction coverage.
- The one-million-step clock benchmark completed without a Gen0 collection in the recorded x64 run.

## Build 34 — 2026-09-23

### Player changes

- Restored uninterrupted edits for Processing time, MIDI bitrate, and Events/sec. Each edit applies when the next event starts service; the current service duration, pending queue, sounding notes, transport, and statistics continue.
- An Events/sec configuration change starts a fresh fractional remainder at the next service start.
- Channels can show indexed source values that precede the current position as gray historical readouts when output has not confirmed a value. Forced values remain blue and take precedence.
- **Apply queue limit without slowdown** now starts unchecked. The system menu places **Chase MIDI state on Play/Seek** above **Always on top**.

### Design and verification

- Investigated Pause/Seek preservation; a combined switch would give misleading Seek behavior and is deferred.
- Focused tests cover live finite and virtual queues, rapid edits, output continuity, strict source-history boundaries, monitor presentation, and the new default.

## Build 35 — 2026-09-24

### Player changes

- Added **Drop oldest complete note** as a separate finite-queue overflow choice when simulated slowdown is on. It removes the oldest queued NoteOn that has not begun service and removes or later suppresses its matching NoteOff.
- Kept attacks already in service or sent safe from eviction. Required NoteOffs, sustain-off, and channel-mode safety controls can temporarily take queue occupancy above the configured limit; ordinary arrivals with no eligible older note are rejected explicitly.
- Kept channel-mute and forced-override filtering before queue admission. A release paired with a previously muted attack remains filtered if the channel is re-enabled before that release.
- Static Analysis projects the same complete-note overflow decisions from the unfiltered source file. Existing **Drop oldest** remains the single-event policy, while forward-only limiting without slowdown still cannot remove already-sent MIDI.

### Implementation and verification

- Added reusable value-node segments with separate service-order and oldest-eligible-attack links. Pending nodes are recycled; overflow does not scan the queue or allocate one object per source event.
- Added focused Playback/Analysis pairing, soft-capacity, filtering, live-policy, lifecycle, failure, and dense-decision tests, plus a bounded queue-index benchmark on both architectures.

## Build 36 — 2026-09-24

### Player changes

- Added checked-by-default **Show Processing model** and **Show Statistics** commands to the main window's native system menu. Either section, or both, can be hidden for the current session without changing playback settings or statistics.
- Hidden sections relinquish their layout rows. Compact view remeasures its fixed height; standard view remeasures its minimum while preserving any height the user added above that minimum. Restoring both returns to the established layouts.
- Commands remain available during loading, playback, and Pause, including through the normal keyboard system-menu route. The File/output and Playback sections remain visible.

### Verification

- Added realized WinForms tests for independent toggles, repeated restoration, compact/default breakpoint crossings, menu-handle recreation, loading and playback state, focus, and finite queue-pressure presentation. Scheduler and note-overflow behavior were not changed.

## Build 37 — 2026-09-24

### Player changes

- Clarified Per-note gate filtering in Events sent/excluded: the usual case no longer shows a redundant zero queue-drop count. When both kinds occur, each remains labelled. Compact slash readouts use measured narrower spacing without changing the standard view.
- The Analysis graph's Playback Statistics panel shortens long values before its captions and provides the complete readout on hover. Space over the active graph now uses the player's existing Play/Pause/Resume action without taking Space from text editing or modal dialogs.
- Opening Channels for the first time during playback or Pause can request the existing ordered MIDI-state chase when that option is on. Only successfully sent values become ordinary observed readouts; merely inferred source values remain historical. Reactivating an open Channels window does not repeat the chase.
- Processing value and Queue limit now use the same click-to-type and horizontal-scrub interaction as Channels, but remain ordinary numeric settings. Events/sec gives finer slider control from 1–100 and accepts exact typed rates through 9,999,999. The rational service clock retains exact long-run rates above one million, including zero-duration individual service quanta.
- Regrouped **Always on top** with the two session view commands in the title-bar menu.

### Queue performance follow-up

- A bounded Build 34 versus current comparison confirmed that the older finite overflow policies slowed after they began sharing Build 35's linked note-eviction queue. Policies that do not need complete-note eviction now use reusable 256-entry FIFO segments. The old indexed path remains for **Drop oldest complete note**.
- The change recovered much, but not all, of the measured local Analysis time. Projected drops and maximum occupancy matched; the measured loops had no Gen0 collections. These synthetic results do not claim native provider or giant-file throughput.

### Verification

- Focused x86/x64 tests cover the changed controls, menu, graph shortcut and readout, ordered Channels opening, rational high rates, policy transitions, finite queue, and lifecycle behavior. Full deterministic architecture totals and exact release hashes are recorded in the private Build 37 checkpoint.

## Build 38 — 2026-09-25

### Player changes

- Processing value and Queue limit now grow or shrink from the width of the number actually displayed. This keeps long values legible without giving short values an unnecessarily wide field. Compact and standard layouts were measured again for label alignment and control fit.
- Horizontal scrubbing of the processing value follows the selected rate slider's scale: small Events/sec values change gently, while high values can be reached without thousands of pixels of dragging. Holding Shift gives one displayed unit per pixel. The drag begins at the exact typed number, so an untouched or reversed gesture does not round it to a slider position.
- Queue-limit scrubbing keeps its established four-pixels-per-step behavior, or eight with Shift. The Channel Monitor's force, Auto, and historical editor behavior is unchanged.

### Project work

- Recorded a bounded feasibility assessment for non-cumulative UI scaling; no scaling control or partial runtime mode was added. Added separate future-design notes for rate-model presentation, delayed note release, and a distinct drop-based Events/sec limit.

### Verification

- Focused realized WinForms tests cover default/compact layout, maximum numeric values, text growth/settling, enlarged font, exact typed-value preservation, model-specific drag mapping, and modifier behavior. The full x86/x64 deterministic totals and release hashes are recorded in the private Build 38 checkpoint.

## Build 39 — 2026-09-25

### Player changes

- The main Processing and Queue fields now center their numbers within a width measured from the actual displayed digits. They retain a small white margin at compact and standard sizes without squeezing long values.
- Queue-limit dragging remains precise at low values and accelerates smoothly at larger values. Holding Shift provides finer two-pixel steps. Reversing a drag restores the exact starting value; merely activating the field changes nothing.
- Analysis now runs the shared Per-note gate over source MIDI and graphs the selected output messages at their logical emission times beside the original source workload. Repeated strikes, simultaneous notes, releases, and end-of-file tails use the live gate's decisions. The report keeps gate filtering separate from ordinary queue overflow.

### Project work and limitations

- The reusable workload scan remains cached across gate-interval changes. Initial previews remain workload-only until projection completes, and cancellation cannot publish an unfinished projection. Static results exclude live channel mutes, forced overrides, native-driver delays, and synthesizer tails.
- Runtime UI scaling remains a separate measured design; the canonical 100% layouts and native DPI behavior are unchanged.

### Verification

- Focused tests compare Analysis' selected MIDI payloads and order with a live fake-output gate run, including an overlapping long note and repeated strikes. They also cover compact/reference stores, interval cache reuse, cancellation, preview replacement, and realized Analysis presentation. The complete x86/x64 totals and release hashes are retained in the private Build 39 checkpoint.

## Build 40 — 2026-09-25

### Player changes

- Replaced the separate **Simulate slowdown** checkbox and hidden Per-note gate command with one visible Rate model selector. **None** is the fresh-launch default. The dropdown groups the three ordinary service-rate choices and the existing Per-note interval gate under nonselectable headings.
- Kept each rate value when switching away from a model. Ordinary live rate/model edits still apply from the next service start without interrupting sounding notes. Entering or leaving None or the Per-note gate uses the established safe restart when playback is active.
- Preserved the optional finite forward-only queue. When active, its heading says **Forward-only queue admission** rather than suggesting accepted MIDI is slowed. Selecting None cannot leave a hidden virtual rate running; activating the option from None visibly selects the last ordinary rate model.
- Removed one Processing model layout row while keeping the standard and compact controls usable. Queue policies, rate calculations, Per-note musical decisions, and Analysis projections were not changed.

### Verification

- Focused realized tests cover fresh defaults, nonselectable headings, remembered values, finite/unlimited and forward-only combinations, active and paused transitions, compact layout, and the unchanged queue/gate paths. Complete x86/x64 totals and release hashes are recorded in the private Build 40 checkpoint.

## Build 41 — 2026-09-25

### Player changes

- Added one offline Help window reachable with **F1** or **Help (F1)…** in the main window's title-bar menu. It opens at the topic associated with the focused File/output, Processing, Queue, Playback, Analysis, or Statistics control, while its topic list also covers Channels and general use.
- Reopening Help reuses the same modeless window and changes its topic; closing it releases that shell. Help does not change playback, processing settings, queue state, or statistics. Its content is built into the executable and uses the application's icon.

### Verification

- Focused realized WinForms tests cover key preprocessing, native system-menu invocation, focused-section selection, content, reuse, closure/reopening, handle recreation, and working-area fit. Complete architecture totals and exact release hashes are retained in the private Build 41 checkpoint.
