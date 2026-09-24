# User guide

## Loading a MIDI file

Use **Open MIDI…** or drag one existing `.mid` or `.midi` file anywhere onto the main window. Unsupported files, folders, URLs, and multi-file drops leave the current song untouched.

Loading is asynchronous and cancellable. The header shows the filename, parser stage, two progress bars, elapsed time, and process-private committed memory. Cancelling or replacing a load retires the older generation so it cannot publish stale results.

Ordinary files enter the one-pass compact parser immediately. Files large enough to plausibly cross the memory-warning boundary are first inspected by a count-only SMF scanner. The loading header shows **Inspecting large MIDI** with monotonic overall/stage progress. Cancel stops either inspection or parsing.

If the exact scan projects substantial use, MIDIBottleneck Player shows the filename, exact number of MIDI events to process, estimated memory while the file is open, and estimated highest memory use while loading. **Continue** parses normally; **Cancel** leaves no file loaded. The displayed loading timer pauses while this decision is open, so thinking time is not counted as loading time. The figures are estimates, not guarantees that an allocation will succeed.

## Choosing an output

**WinMM** lists MIDI outputs reported by Windows. Choose the executable architecture that matches any native wrapper you use.

**KDMAPI** selects a compatible direct-output provider. A matching application-local provider is deliberate and takes priority; initialization failures are reported rather than silently falling back to another synthesizer.

**None** opens no native output. It is useful for separating parser/scheduler performance from driver and synthesizer behavior. Events still count as successfully dispatched for the simulator’s statistics.

Changing output during playback uses the same safe restart boundary as seeking: the old worker retires, the real provider is silenced and closed where applicable, queued work/statistics are reset as documented, and playback resumes from the same source position.

## Starting in the middle of a file

The title-bar system menu checks **Chase MIDI state on Play/Seek** by default. Starting Play at a nonzero position or seeking restores the bank, program, ordinary controller values, pitch bend, channel pressure, and known RPN/NRPN parameter values that were effective immediately before that position. Bank selection is sent before Program, and parameter selectors/data remain in valid order.

Chase never replays notes, polyphonic key pressure, SysEx, meta data, system messages, or channel-mode silence commands. Events exactly at the selected position remain ordinary source events and are not sent twice. Disabled channels receive no state. A forced Channel Monitor value wins over a conflicting source value.

Pause resets and silences the output, so state is restored once when Resume continues; nothing is resent while paused. The option is disabled while playback is active and can be unchecked while stopped. This does not change the Channel Monitor's deliberate one-attribute source chase.

## Processing model

The title-bar menu (Alt+Space) has checked **Show Processing model** and **Show Statistics** commands. Uncheck either to reclaim that section's height; both can be hidden together. File/output and Playback stay visible. These choices last only until the application closes. Hiding a section does not turn off its model, reset statistics, or interrupt playback; current values appear when the section returns. The same commands work while loading, playing, paused, or stopped.

**Simulate slowdown** applies the selected service model:

- **Processing time per event** assigns the configured time to every accepted dispatchable event.
- **Events per second** directly sets the service rate from 0 through 1,000,000 events/sec. Zero means unlimited/immediate service.
- **MIDI bitrate** derives service time from the message’s serialized byte count and selected bitrate.

With slowdown disabled, accepted events are dispatched as soon as their source time and output calls permit. A configured processing time of exactly 0 µs uses the same effective-immediate scheduler path while retaining the logical setting.

The Events/sec model carries fractional microseconds between events, so rates such as 3,000 events/sec remain exact over time rather than being rounded event by event. Switching directly between Processing time and Events/sec chooses the nearest whole equivalent. A live rate or ordinary model change applies when the next event begins service. The event already in service keeps its assigned duration; the pending queue, sounding notes, transport, and statistics continue. A changed Events/sec setting starts a fresh fractional phase at that next service start.

## Queue length and overflow

With **Queue length limit** off, the simulated application queue is unlimited. With it on, the configured occupancy includes the event in service plus pending events.

The title-bar system menu's **Apply queue limit without slowdown** option is off by default. If you enable it while Queue length limit is on and Simulate slowdown is off, it keeps a virtual queue using the selected processing-time, Events/sec, or MIDI-bitrate model. It may reject a newly arriving event, but accepted MIDI is sent immediately; the model never removes something already sent. The queue statistic above the pressure bar remains the real unsent scheduler/output backlog. The bar is labelled **Virtual** and shows the separate modeled pressure.

Only **Drop newest** and **Drop incoming complete notes** are valid in this forward-only mode. **Drop oldest**, **Drop oldest complete note**, and **Clear buffer and jump to realtime** require Simulate slowdown: they can remove pending MIDI, but cannot retract MIDI already sent. The player keeps the selected policy and explains why Play is unavailable instead of silently changing it. The system-menu option can be changed only while playback is stopped; the choice applies to the next playback start. Turning Simulate slowdown on or off during finite playback uses a safe silence/restart at the same position because it changes between the real delayed queue and the virtual forward-only model.

Overflow choices include:

- **Drop newest** — reject the arriving event when full.
- **Drop oldest** — discard the oldest safe pending event under the existing event policy.
- **Clear buffer and jump to realtime** — silence output, clear pending work, and jump to the exact source-time boundary.
- **Drop incoming complete notes** — reject a NoteOn and later suppress its paired NoteOff, while preserving NoteOffs for notes that were actually sent and preserving non-note messages. Protected events can temporarily take occupancy above the configured soft ceiling.
- **Drop oldest complete note** — when full, remove the oldest queued NoteOn that has not begun service. If its matching NoteOff is already queued, remove that too; if it arrives later, suppress it then. Matching uses first-in, first-out occurrences of the same channel and pitch, including velocity-zero NoteOns as releases. An attack already in service or sent cannot be removed. This is different from **Drop oldest**, which removes one pending event without treating its whole note as a pair.

If no old unsent attack can be removed, this new policy rejects an arriving NoteOn or ordinary non-note message and counts it as dropped. It keeps NoteOffs and note-silencing controls (sustain off and channel-mode safety commands), even when that temporarily puts occupancy above the configured limit. This is a safety exception, not a strict hard cap. Each discarded attack and each discarded or later suppressed release counts separately. Changing the choice during playback affects later overflow decisions, but does not undo MIDI already sent or discarded under the previous choice.

Channel mutes and forced-attribute conflicts are filtered before queue admission. They are reported separately and do not consume simulated service or queue capacity.

A blocked driver or synthesizer call can create a real backlog even when simulated slowdown is off. **Queue now / maximum** counts those due, eligible, unsent events and the call currently in service. Muted channels and source attribute changes blocked by a forced override are excluded. This real backlog is separate from virtual pressure and can exceed the configured virtual limit while a native call is unable to return.

### Per-note interval gate

The title-bar system menu can enable **Per-note interval gate** when the processing-time value is greater than zero. While enabled, the player locks the rate model to Processing time per event and temporarily disables the generic slowdown and queue controls without changing their saved choices.

The interval applies independently to each MIDI pitch across all channels. Source notes remain distinct by track, channel, pitch, and FIFO occurrence; the constrained output remembers only whether that pitch is up or down and the channel on which its current NoteOn was actually sent. A later attack is therefore not suppressed merely because a longer note on another track or channel is still sustaining.

If a selected attack arrives while its pitch is down, the player sends a release at the preceding boundary and the new NoteOn at its assigned boundary. Very short and zero-duration source notes still receive one complete interval. NoteOns at the same absolute MIDI tick are one simultaneous candidate: the strongest velocity is emitted, with source order breaking a tie, while every selected layer can support the resulting sustain. Under genuine overload, a bounded one-boundary look-ahead retains the maximum feasible alternating attacks, prefers the stronger of otherwise equivalent adjacent candidates, and prevents an unbounded transition backlog.

The output limit remains exactly one NoteOn, one NoteOff, or no transition for each pitch at each boundary. Non-note messages retain their ordinary source-time path.

The Sent/dropped statistic shows simultaneous coalescing and interval-overload rejections separately in parentheses. Changing the gate, its interval, output, or transport state uses the normal safe silence/restart boundary, so stale pitch state cannot survive Pause, Seek, Stop, or an output restart.

Very large numbers of simultaneously unmatched source notes are tracked in growable reusable segments. If the selected output channel for a layered pitch is disabled during playback, another already selected enabled layer can re-establish that pitch at the next interval boundary after the safety silence.

## Playback and statistics

Play, Pause, Seek, Stop, output switching, and file replacement use ordered worker boundaries so native sends do not overlap. A native call already executing cannot be forcibly interrupted; control takes effect as soon as it returns.

Statistics include source timeline/output frontier, current/maximum queue occupancy, theoretical or observed maximum rate, live rolling output rate, sent/dropped events, effective playback speed, and current/maximum lag.

In an immediate-service mode, **Maximum rate** is the highest observed rolling dispatch rate since the applicable statistics reset. It is workload-specific, not a theoretical hardware limit. **Reset stats** rebases counters and peaks without disrupting playback.

In Per-note mode, **Maximum rate** becomes the gate's frame frequency. One frame is one configured interval. Each of the 128 pitches can make at most one Note On or Note Off transition in a frame, while different pitches can transition together. Controllers and other non-note messages are not limited by this number, so it is not the player's total event-throughput ceiling.

**Current lag** is the lateness of the most recently sent MIDI event compared with its original time in the file. **Maximum lag** is the largest such value since statistics were reset. In Per-note mode, lag includes deliberate waiting for an interval boundary as well as scheduler or output delay. Several transitions in one frame can come from different source times, so current lag may move up and down. It does not measure synthesizer rendering or audio-device latency.

**Effective speed** compares resolved source-timeline progress with real playback time over the selected measurement window. In Per-note mode, the frontier advances only after a gate frame and all of its output calls finish. It still advances through sparse or filtered passages that the gate has processed, but stops when output is genuinely blocked.

## Analysis

The Analysis window summarizes message types and workload, then graphs event/byte density and modeled queue pressure at an adaptive or fixed resolution. Time-axis labels use aligned human-readable intervals; hover and pinned inspection provide exact arbitrary positions.

Queue Projection reports source duration, predicted accepted-output completion, and positive overrun after the source ends. This models the selected simulator service and queue policy. It does not predict provider buffering, synthesizer rendering, voice release tails, or audio-device latency.

Analysis work is asynchronous, cancellable, cached by reusable file/resolution workload, and protected from stale results. On an initial calculation, the window shows a workload-only graph as soon as event/byte density, clusters, and message-type scanning finishes. It is labelled **Queue projection pending** and deliberately omits occupancy, overflow, predicted drops, buffer clears, and completion values until they are calculated.

The final projection replaces that preview atomically. Cancelling keeps the last completed graph when one exists. If there is no completed graph yet, a published preview remains useful and is relabelled **Workload only — projection cancelled**.

## MIDI Channel Monitor

**Channels…** opens a modeless 16-row monitor. Sent, filtering, polyphony, and output position come from actual dispatch decisions. If Play/Seek state chase is enabled, attributes with no confirmed output observation can also show the latest indexed source-file value before the current position. This uses the song's existing state index and does not send MIDI.

It shows MIDI-observed held-key polyphony, peak polyphony, sent/filtered counts, bank, program, volume, expression, pan, sustain, pitch bend, channel aftertouch, and last output position. These are MIDI observations, not a synthesizer’s internal voice count.

Ordinary black attributes were observed at the output. Bold blue attributes are forced. Gray italic attributes are historical; a source-derived gray value describes the file and does not prove the output received it. Forced values take precedence. No unknown value is invented, and opening Channels does not change Sent, polyphony, or output position.

Double-click an adjustable attribute to activate its scrub-or-type editor:

- drag horizontally to scrub;
- click and type, then press Enter or leave the field to commit a changed value;
- press Escape to abandon typing;
- right-click a forced blue value to return it to Auto without state chase;
- right-click the resulting gray historical value to send the latest applicable source-file value for that one attribute at the current position.

Forced values are applied through the ordered output boundary. Conflicting source changes are filtered before queue admission. Clicking the Channel cell disables/enables that channel; disabling first sends channel-specific sustain-off and note-silencing safety messages. Re-enabling does not replay missed notes or reconstruct state.

Overrides persist across normal playback boundaries for the same file and clear on file replacement. Automatic Play/Seek chase respects those overrides and never recreates notes.

## Compact layout and system menu

The main window switches to a measured compact arrangement below its responsive width boundary. Controls retain full tooltips where captions or values must ellipsize.

The title-bar system menu contains **About**, checked-by-default **Chase MIDI state on Play/Seek**, session-only **Always on top**, **Per-note interval gate**, and **Apply queue limit without slowdown**, which starts unchecked.
