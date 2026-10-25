# Roadmap

This is a direction of travel, not a promise of dates. Correct MIDI behavior, deterministic results, x64/x86/XP parity, and safe output lifecycles take priority over feature count.

## Current status

- **Version 2.0.0 is released for Windows** (x64, x86 and Windows XP). It is a rewrite of version 1 as a native C++17 program and keeps every feature of version 1.0.44. See [CHANGELOG.md](CHANGELOG.md) for what is new and [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) for the boundaries.
- **Version 1 is no longer maintained.** Its last release is `v1.0.44`. Its files are kept unchanged in [`archive/v1`](archive/v1) (history in `archive/v1/CHANGELOG.md`) and on the `v1-maintenance` branch.

## Version 2 plan: done in 2.0

The decisions recorded for the rewrite have been carried out:

- **Goal:** all v1.0.44 features, with bug fixes, performance and quality improvements: better timing precision, lower memory and processor cost per MIDI event for very large files, clearer messages, and cleaner code. Version 2 is never made worse just to match v1. Every difference is listed with its reason and evidence in [v2/docs/DIFFERENCES_FROM_V1.md](v2/docs/DIFFERENCES_FROM_V1.md).
- **Design:** one portable, optimized C++17 core shared by thin frontends for each platform. The core never calls the operating system directly; a small platform layer supplies timers, threads, file access, and MIDI output.
- **Checking against v1:** a comparison harness plays the same deterministic test files through v1 and v2, records every emitted MIDI message and its timing, and compares the results. Every difference must be an intended, recorded improvement; an unexplained difference is a failure.
- **Windows first, with a plain Win32 interface**, in three programs: x64, x86 and a Windows XP SP3 build that needs no SSE2.
- **Windows XP SP3:** supported by its own build. XP support continues only while recorded evidence shows it causes no meaningful harm to the other builds; its cost is logged in [v2/docs/XP_COST_LOG.md](v2/docs/XP_COST_LOG.md).
- **Version 1 archived:** the v1 files moved to `archive/v1`.
- **Requirements found while testing v1.0.44:** the Integrated header redesign and resize-cursor feedback in borderless mode are done in version 2 (see the changelog and `v2/docs/UI_DIFFERENCES.md`).
- **New in 2.0 beyond v1:** Direct WinMM output, remembered settings, a Windows XP build, and the improvements listed in the changelog.

## Planned next (not part of 2.0)

- **Android:** first the standard Android MIDI system (on Android 6 and later it already supports ordinary class-compliant USB MIDI devices), then wider USB MIDI device support, then possibly an optional built-in synthesizer for Android only. A built-in synthesizer would need a third-party licence review and an explicit exception to the current rule that releases never bundle a synthesizer; users would supply their own SoundFonts.
- **Linux:** a GTK 3 player with an ALSA output is being prepared.
- **macOS**, and possibly a web version, after that.
- **Helper-process MIDI isolation:** may be reconsidered as part of the output backend, because it changes timing, SysEx ownership, and output lifetime.
- **Tempo scale:** a control to play a song faster or slower than written.
- **Compositor-synchronized rendering** and **producer/consumer output separation** (which would redefine queue and lag semantics) remain design questions.
- **Channel override extensions** (presets, requested-versus-forced display, static transformations, and broader override systems) remain deferred and need a product decision.
- **Windows 98 SE:** an experimental one-time fork at most. The core is never degraded for it.

Detailed engineering notes remain in [Opportunities.md](Opportunities.md). User-visible boundaries are listed in [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).
