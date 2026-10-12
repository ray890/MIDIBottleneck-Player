# Roadmap

This is a direction of travel, not a promise of dates. Correct MIDI behavior, deterministic results, x86/x64 parity, and safe output lifecycles take priority over feature count.

## Current status

- **Version 1 is in maintenance.** Build 44 (`v1.0.44`) is the last planned feature build of the current Windows .NET player. No further v1 feature builds are planned.
- **v1.0.44 is the frozen reference.** Its behavior, source, and tests define exactly what the planned version 2 must reproduce.
- **Version 2 is under way privately.** Development happens on a private local branch, and milestone status is tracked privately. Version 2 will be published only when it matches v1's features.

## Version 2 plan

Version 2 (numbered 2.x) is a rewrite, not a feature build. These are the recorded decisions guiding the private version 2 development.

- **Goal:** reproduce v1.0.44 behavior exactly, with equal or better performance, including memory and processor cost per MIDI event for very large files.
- **Design:** one portable, optimized C++17 core shared by thin frontends for each platform. The core itself never calls the operating system directly; a small platform layer supplies timers, threads, file access, and MIDI output.
- **Proving parity:** a comparison harness will play the same deterministic test files through v1 and v2, record every emitted MIDI message and its timing, and compare the results. v1's source and tests are the reference.
- **Platform order:** Windows first, with a plain Win32 interface; then Android, Linux, macOS, and possibly a web version.
- **Android order:** the standard Android MIDI system first (on Android 6 and later it already supports ordinary class-compliant USB MIDI devices), then wider USB MIDI device support, then an optional built-in BASSMIDI synthesizer for Android only. BASSMIDI would need a third-party license review and an explicit exception to the current rule that releases never bundle a synthesizer. Users would supply their own SoundFonts.
- **Windows XP SP3:** occasional builds, not every release, which may lag behind the main core. C++17 was chosen partly so that XP-capable compilers remain possible. XP gets its own fallbacks—a smaller maximum file size because of 32-bit memory limits, coarser timers, and no newer processor instructions—without making any other build worse. XP support continues only while recorded evidence shows it causes no meaningful harm elsewhere, and its cost is logged.
- **Windows 98 SE:** an experimental one-time fork. The core is never degraded for it. A later 98 SE build is possible only as a fresh downport from the then-current core, following a recorded downport recipe.
- **Development and publication:** version 2 is developed privately and will not be published until it matches v1's features. When it is published, the v1 files will move into a v1 archive folder.

### Version 2 requirements found while testing Build 44

- **Integrated header redesign.** In v1.0.44 the Integrated header is still a separate 24-pixel strip (≡, "File and output", —, □, ×) above the File and output box, so it occupies its own row. The intended design puts the window controls inside rows that already exist—for example ≡ before **Open MIDI…** and —, □, × at the right end of the file-name row—with no separate strip or box heading, making the window shorter by the strip's height. Dragging or double-clicking the file name or an empty area would move or maximize the window. The Auto-hide overlay is close to this intent.
- **Resize cursor feedback.** In borderless mode, v2 must show the correct resize pointer at every invisible edge and corner, including where player controls cover the edge. (See the v1 limitation below.)
- **When output falls behind.** v2 may offer a choice of what happens when the MIDI output cannot keep up: catch up (the current default), Output-paced timeline (the existing option, keeping its name), and possible new choices—gradual recovery, smoothed catch-up, waiting a bounded time and then skipping ahead with state chase, or adaptive tempo. The new choices are candidates that still need specification, not commitments.

## Recently completed

- **Build 44 — musical traffic control, track routing, and trustworthy shutdown verification:** isolated the native dialog to the synthetic test harness, moved tests onto a real WinForms message loop, added native-window and descendant-process teardown monitoring, fixed forced-override timing/accounting paths, implemented shared Playback/Analysis Delayed NoteOff, added a complete stopped-session track-routing planner/editor and a reversible three-style borderless main window, corrected external-backlog admission, added optional same-worker Pause plus queue-preserving Seek behavior, and completed the default-off Output-paced timeline. The Note start and total Output rate gates combine exact fractional pacing, a bounded batching tolerance, and an exact rolling one-second ceiling so below-limit/high-limit material remains transparent while sustained excess stays bounded. Track routing uses plain MIDI track/channel wording, compiled-channel emphasis, shared GM Patch names, source-channel-global state association, destination-specific patch timelines with clean instrument handoffs, and deterministic global Aggressive representative selection with explicit confirmed omissions. Bounded RIFF/RMID loading and the disclosed unambiguous post-meta running-status repair are included. Channel 10 presents recognized GM2/GS/XG drum-kit names from Bank and Program with a truthful fallback. Borderless resizing forwards child-control edge drags to the native resize path without drawing a border. The final release gate passed 120/120 tests per architecture plus monitored packaged-player launch/close.
- **Build 43 — presentation and soft-limit clarity:** restored visible noninteractive Rate headings and native selector rendering, improved low-scale font legibility, removed blank standard-window height, restored queue event-count statistics, added a purple over-limit state, and protected required release/safety traffic consistently in Playback and Analysis.
- **Build 42 — queue-age and presentation controls:** Rate headings are static presentation elements rather than selectable items, each model remembers its own value, zero-interval Per-note processing has deterministic source-time boundaries, Playback and Analysis share an optional waiting-time queue limit, and the main, Analysis, and Channels windows have non-cumulative 50%–200% scaling.
- **Build 41 — contextual offline Help:** F1 and a title-bar Help command open one reusable guide at the topic associated with the focused main-window control.
- **Build 40 — discoverable processing models:** None, three ordinary rate models, and Per-note interval gate now share one grouped selector. Forward-only finite queue admission is labelled separately when active; the scheduler algorithms are unchanged.
- **Build 39 — numeric controls and Per-note Analysis:** centered measured numeric fields and gentler Queue scrubbing; Analysis now projects the shared Per-note gate's actual selected messages and their logical emission times alongside raw source workload.
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

- **Channel override extensions** — the narrow default-off per-Analysis-window forced-override projection is complete and passes frozen monitored x86/x64 acceptance in Build 44; presets, requested-versus-forced display, static transformations, and broader override systems remain deferred and would now be considered for version 2.

## Build 44 retained boundaries

- All three borderless styles are implemented, and invisible edges and corners can be dragged to resize even where player controls cover them. Two differences from the intended design remain and are moved to version 2: the Integrated header is still a separate strip rather than controls placed inside existing rows, and the pointer never changes to a resize arrow when hovering over an edge or corner, although dragging there still resizes. The narrow per-Analysis-window forced-override projection is complete. Delayed NoteOff melody retention, zero-interval identity, and Output-rate transition/cadence are closed with evidence. The Output ceiling is exact; short graph-bucket peaks and their one-second ledger echo are retained truthful consequences of bounded batch preservation, and no separate musical strategy was justified. Keep waiting-time **Drop background notes** unavailable unless separately safe semantics are demonstrated.

## Version 2 design questions

These formerly deferred items will be decided as part of the version 2 design, not added to v1:

- portability, now addressed by the version 2 plan above;
- compositor-synchronized rendering;
- producer/consumer output separation, which would redefine queue and lag semantics;
- helper-process MIDI isolation, as part of the version 2 output backend.

Detailed engineering notes remain in [Opportunities.md](Opportunities.md). User-visible boundaries are listed in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).
