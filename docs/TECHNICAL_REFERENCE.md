# Technical reference

## Parsing and event representation

The parser supports Standard MIDI File format 0 and synchronous format 1 with PPQN time division. It expands running status, preserves stable track/event ordering, assembles tempo changes into one map, and converts each dispatchable event to an intended microsecond timestamp.

Playback and Analysis consume `IMidiEventStore`, an indexed read-only boundary with exact access and lower-bound search. Ordinary short messages store their bytes inline; variable-length SysEx data retains an immutable side payload. The current backend still uses one `MidiEvent` object per event and one contiguous final reference array.

A compact per-song index records state-changing channel messages for logarithmic one-attribute source-value chase. It is not automatic whole-song state chase.

## Scheduler and timing

A dedicated above-normal-priority worker uses `Stopwatch` as the monotonic transport clock. A Windows waitable timer handles coarse waits; only the short final interval uses yield/spin checks.

Immediate dispatch returns to admission/statistics publication after at most 2,048 sends or about 8 ms of completed output work. Those checkpoints add no deliberate sleep and preserve payload/order exactly. Stop, Pause, and Seek are observed before the next send after any blocked native call returns.

The scheduler is the sole ordered writer to output state. UI control requests—overrides, one-value chase, mute safety, and output restarts—cross the same serialized boundary rather than calling native output concurrently. A monotonic wake generation closes the race between signalling the manual-reset wake event and the worker resetting it, so a control request cannot sleep until the next source event merely because those operations crossed.

## Queue semantics

The unlimited queue uses contiguous indices into the sorted event store, so backlog does not allocate one queue node per event. Finite queues use explicit admission and overflow decisions. Occupancy includes the event in service.

`Drop incoming complete notes` tracks channel/key occurrences in fixed-size state. A rejected NoteOn’s paired NoteOff is suppressed; a NoteOff protecting an already sent note is retained. Protected events and non-note messages can make the configured capacity a soft ceiling, and statistics/pressure include that excess.

Muted channel events and source attribute changes conflicting with forced overrides are filtered before scheduler admission. They consume no simulated service, queue space, output rate, or sent count and are reported separately from overflow drops. An event already executing inside a native call cannot be recalled.

## Output boundaries

All outputs implement the same MIDI-output abstraction:

- WinMM packs short messages for `midiOutShortMsg` and keeps prepared `MIDIHDR`/SysEx memory alive through completion and unprepare.
- KDMAPI validates architecture and required exports, uses direct short-message submission, and supports prepared long messages only when the provider exposes the complete lifecycle.
- None implements allocation-free no-op open, send, reset, panic, and close operations while preserving logical dispatch statistics.

Detailed identity/error strings are built only on native failure. Stable module/device identity and architecture-dependent header sizes are cached outside the per-event success path.

Reset boundaries retire the worker before reset/panic whenever the active native call can return. Panic uses sustain-off, all-sound-off, and all-notes-off safety ordering. In-process managed code cannot forcibly cancel unmanaged code that never returns.

## Analysis

Whole-file Analysis separates reusable file/resolution workload scanning from configuration-dependent queue projection. Workload counts and buckets can be reused when only service settings change; exact queue projections still recompute when their inputs change.

Auto resolution is explicit state. Its visible label reports the resolution of the accepted graph, not a pending request. Cancellation retires the active/pending generation, and layout-only height changes do not re-arm the cancelled request.

Queue Projection drains accepted modeled work after the last arrival to calculate predicted output completion. Rejected events add no later service. Static Analysis intentionally excludes live Channel Monitor mutes and overrides.

## UI publication cadence

The main form samples the newest immutable playback/loading state on an approximately 16 ms presentation heartbeat and changes displayed values only when content differs. Loading memory is sampled about once per second. Its fixed-height double-buffered status surface avoids parent layout and partial-text painting on each update.

The Channel Monitor’s scheduler-side state is fixed-size and allocation-conscious. The UI copies one 16-channel snapshot on its presentation cadence and updates only changed cells. Closing the monitor removes the presentation cost; routing/override state remains engine-owned when required.

Analysis recomputation debounce, delayed busy indication, effective-speed smoothing, and native playback timing remain separate mechanisms because they serve different semantics.
