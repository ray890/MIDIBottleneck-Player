# Compatibility and known limits

- Windows 10 or Windows 11 with a compatible .NET Framework 4.x runtime is required.
- x86 and x64 are distinct builds. Native WinMM wrappers and KDMAPI-compatible providers must match the executable architecture.
- Native providers are user-supplied and are not distributed by this project. Deterministic tests use fake or `None` output and do not establish compatibility with every driver.
- Format 0 and synchronous format 1 PPQN MIDI files are supported. Format 2 and SMPTE time division are rejected.
- The published song uses compact segmented records, but parsing still creates per-track objects and a contiguous merged legacy list before conversion. Very large files can still encounter parser-time memory, paging, and structural limits before the final compact store exists.
- The x64 executable must remain beside its `.exe.config`; the configuration enables the .NET Framework very-large-array setting before managed startup for the remaining legacy parse/merge path. Direct construction into segments is required before this dependency can be reconsidered.
- In-process code cannot forcibly interrupt a native output call that never returns. Stop, Pause, Seek, and output switching avoid concurrent native calls and will not start a replacement worker across an unsafe boundary.
- Starting or seeking in the middle of a song does not reconstruct complete prior channel state. The Channel Monitor offers a deliberate one-attribute source-value chase; whole-state chase remains deferred.
- Analysis is a deterministic projection of immutable source events and selected simulator settings. It does not predict synthesizer voice stealing, audible release tails, provider buffering, live Channel Monitor mutes/overrides, or the live-only per-note interval gate.
- An initial workload-only Analysis preview intentionally omits all queue-projection values until projection completes. Cancelling retains the scanned workload view; projection failure also retains it and reports the failure reason.
- Per-note gate occurrence storage grows with the maximum number of simultaneously unmatched source notes. It no longer has a 16,384-note pairing ceiling, but an actual segment-allocation failure stops and reports playback rather than continuing with lossy pairing.
- KDMAPI prepared SysEx requires a provider exposing the complete prepare/send/unprepare contract. Unsupported combinations fail explicitly.
