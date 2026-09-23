# Compatibility and known limits

- Windows 10 or Windows 11 with a compatible .NET Framework 4.x runtime is required.
- x86 and x64 are distinct builds. Native WinMM wrappers and KDMAPI-compatible providers must match the executable architecture.
- Native providers are user-supplied and are not distributed by this project. Deterministic tests use fake or `None` output and do not establish compatibility with every driver.
- Format 0 and synchronous format 1 PPQN MIDI files are supported. Format 2 and SMPTE time division are rejected.
- Loading uses bounded stream buffers plus compact segmented provisional/final records, payloads, tempo changes, and source-value indexes. It no longer creates a per-event object graph or contiguous merged list, but huge files can still exhaust aggregate process commit/address space or the explicit 2,147,483,647-event indexed-store limit.
- Neither architecture needs an adjacent `.exe.config`. Individual SMF payload lengths remain bounded by the format's four-byte variable-length quantity, and Analysis buckets retain their documented cap.
- Large-file memory figures are storage-aware projections, not guarantees. They model MIDI records, payloads, tempo data, source-value histories, segment/reference overhead, and merge overlap, but actual CLR fragmentation, other process state, commit availability, and paging vary. A continued load can still fail cleanly if the system cannot satisfy later allocations.
- In-process code cannot forcibly interrupt a native output call that never returns. Stop, Pause, Seek, and output switching avoid concurrent native calls and will not start a replacement worker across an unsafe boundary.
- Starting or seeking in the middle of a song does not reconstruct complete prior channel state. The Channel Monitor offers a deliberate one-attribute source-value chase; whole-state chase remains deferred.
- Analysis is a deterministic projection of immutable source events and selected simulator settings. It does not predict synthesizer voice stealing, audible release tails, provider buffering, live Channel Monitor mutes/overrides, or the live-only per-note interval gate.
- An initial workload-only Analysis preview intentionally omits all queue-projection values until projection completes. Cancelling retains the scanned workload view; projection failure also retains it and reports the failure reason.
- Per-note gate occurrence storage grows with the maximum number of simultaneously unmatched source notes. It no longer has a 16,384-note pairing ceiling, but an actual segment-allocation failure stops and reports playback rather than continuing with lossy pairing.
- KDMAPI prepared SysEx requires a provider exposing the complete prepare/send/unprepare contract. Unsupported combinations fail explicitly.
