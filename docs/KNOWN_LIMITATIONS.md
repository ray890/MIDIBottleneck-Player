# Compatibility and known limits

- Windows 10 or Windows 11 with a compatible .NET Framework 4.x runtime is required.
- x86 and x64 are distinct builds. Native WinMM wrappers and KDMAPI-compatible providers must match the executable architecture.
- Native providers are user-supplied and are not distributed by this project. Deterministic tests use fake or `None` output and do not establish compatibility with every driver.
- Format 0 and synchronous format 1 PPQN MIDI files are supported. Format 2 and SMPTE time division are rejected.
- The event store still uses one object per event and a contiguous final reference array. Very large files can require tens of gigabytes, substantial paging, and long loading times even on x64.
- In-process code cannot forcibly interrupt a native output call that never returns. Stop, Pause, Seek, and output switching avoid concurrent native calls and will not start a replacement worker across an unsafe boundary.
- Starting or seeking in the middle of a song does not reconstruct complete prior channel state. The Channel Monitor offers a deliberate one-attribute source-value chase; whole-state chase remains deferred.
- Analysis is a deterministic projection of immutable source events and selected simulator settings. It does not predict synthesizer voice stealing, audible release tails, provider buffering, or live Channel Monitor mutes/overrides.
- KDMAPI prepared SysEx requires a provider exposing the complete prepare/send/unprepare contract. Unsupported combinations fail explicitly.
