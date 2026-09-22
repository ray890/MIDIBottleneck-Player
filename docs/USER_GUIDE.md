# User guide

## Loading a MIDI file

Use **Open MIDI…** or drag one existing `.mid` or `.midi` file anywhere onto the main window. Unsupported files, folders, URLs, and multi-file drops leave the current song untouched.

Loading is asynchronous and cancellable. The header shows the filename, parser stage, two progress bars, elapsed time, and process-private committed memory. Cancelling or replacing a load retires the older generation so it cannot publish stale results.

## Choosing an output

**WinMM** lists MIDI outputs reported by Windows. Choose the executable architecture that matches any native wrapper you use.

**KDMAPI** selects a compatible direct-output provider. A matching application-local provider is deliberate and takes priority; initialization failures are reported rather than silently falling back to another synthesizer.

**None** opens no native output. It is useful for separating parser/scheduler performance from driver and synthesizer behavior. Events still count as successfully dispatched for the simulator’s statistics.

Changing output during playback uses the same safe restart boundary as seeking: the old worker retires, the real provider is silenced and closed where applicable, queued work/statistics are reset as documented, and playback resumes from the same source position.

## Processing model

**Simulate slowdown** applies the selected service model:

- **Processing time per event** assigns the configured time to every accepted dispatchable event.
- **MIDI bitrate** derives service time from the message’s serialized byte count and selected bitrate.

With slowdown disabled, accepted events are dispatched as soon as their source time and output calls permit. A configured processing time of exactly 0 µs uses the same effective-immediate scheduler path while retaining the logical setting.

## Queue length and overflow

With **Queue length limit** off, the simulated application queue is unlimited. With it on, the configured occupancy includes the event in service plus pending events.

Overflow choices include:

- **Drop newest** — reject the arriving event when full.
- **Drop oldest** — discard the oldest safe pending event under the existing event policy.
- **Clear buffer and jump to realtime** — silence output, clear pending work, and jump to the exact source-time boundary.
- **Drop incoming complete notes** — reject a NoteOn and later suppress its paired NoteOff, while preserving NoteOffs for notes that were actually sent and preserving non-note messages. Protected events can temporarily take occupancy above the configured soft ceiling.

Channel mutes and forced-attribute conflicts are filtered before queue admission. They are reported separately and do not consume simulated service or queue capacity.

### Per-note interval gate

The title-bar system menu can enable **Per-note interval gate** when the processing-time value is greater than zero. While enabled, the player locks the rate model to Processing time per event and temporarily disables the generic slowdown and queue controls without changing their saved choices.

The interval applies independently to each MIDI pitch across all channels. Source notes remain distinct by track, channel, pitch, and FIFO occurrence; the constrained output remembers only whether that pitch is up or down and the channel on which its current NoteOn was actually sent. A later attack is therefore not suppressed merely because a longer note on another track or channel is still sustaining.

If a selected attack arrives while its pitch is down, the player sends a release at the preceding boundary and the new NoteOn at its assigned boundary. Very short and zero-duration source notes still receive one complete interval. NoteOns at the same absolute MIDI tick are one simultaneous candidate: the strongest velocity is emitted, with source order breaking a tie, while every selected layer can support the resulting sustain. Under genuine overload, a bounded one-boundary look-ahead retains the maximum feasible alternating attacks, prefers the stronger of otherwise equivalent adjacent candidates, and prevents an unbounded transition backlog.

The output limit remains exactly one NoteOn, one NoteOff, or no transition for each pitch at each boundary. Non-note messages retain their ordinary source-time path.

The Sent/dropped statistic shows simultaneous coalescing and interval-overload rejections separately in parentheses. Changing the gate, its interval, output, or transport state uses the normal safe silence/restart boundary, so stale pitch state cannot survive Pause, Seek, Stop, or an output restart.

## Playback and statistics

Play, Pause, Seek, Stop, output switching, and file replacement use ordered worker boundaries so native sends do not overlap. A native call already executing cannot be forcibly interrupted; control takes effect as soon as it returns.

Statistics include source timeline/output frontier, current/maximum queue occupancy, theoretical or observed maximum rate, live rolling output rate, sent/dropped events, effective playback speed, and current/maximum lag.

In an immediate-service mode, **Maximum rate** is the highest observed rolling dispatch rate since the applicable statistics reset. It is workload-specific, not a theoretical hardware limit. **Reset stats** rebases counters and peaks without disrupting playback.

## Analysis

The Analysis window summarizes message types and workload, then graphs event/byte density and modeled queue pressure at an adaptive or fixed resolution. Time-axis labels use aligned human-readable intervals; hover and pinned inspection provide exact arbitrary positions.

Queue Projection reports source duration, predicted accepted-output completion, and positive overrun after the source ends. This models the selected simulator service and queue policy. It does not predict provider buffering, synthesizer rendering, voice release tails, or audio-device latency.

Analysis work is asynchronous, cancellable, cached by reusable file/resolution workload, and protected from stale results. Cancelling keeps the last completed graph when one exists.

## MIDI Channel Monitor

**Channels…** opens a modeless 16-row monitor sourced from successfully dispatched channel messages and filtering decisions—not from a rescan of the file.

It shows MIDI-observed held-key polyphony, peak polyphony, sent/filtered counts, bank, program, volume, expression, pan, sustain, pitch bend, channel aftertouch, and last output position. These are MIDI observations, not a synthesizer’s internal voice count.

Double-click an adjustable attribute to activate its scrub-or-type editor:

- drag horizontally to scrub;
- click and type, then press Enter or leave the field to commit a changed value;
- press Escape to abandon typing;
- right-click a forced blue value to return it to Auto without state chase;
- right-click the resulting gray historical value to send the latest applicable source-file value for that one attribute at the current position.

Forced values are applied through the ordered output boundary. Conflicting source changes are filtered before queue admission. Clicking the Channel cell disables/enables that channel; disabling first sends channel-specific sustain-off and note-silencing safety messages. Re-enabling does not replay missed notes or reconstruct state.

Overrides persist across normal playback boundaries for the same file and clear on file replacement. Whole-channel state chase on arbitrary Play/Seek remains deferred.

## Compact layout and system menu

The main window switches to a measured compact arrangement below its responsive width boundary. Controls retain full tooltips where captions or values must ellipsize.

The title-bar system menu contains **About**, a session-only **Always on top** option, and **Per-note interval gate**.
