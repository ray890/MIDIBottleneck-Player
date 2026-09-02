# MIDI Event Bottleneck Simulator

A small Windows desktop application for playing Standard MIDI Files through a Windows MIDI output while simulating a single-event processing bottleneck. It is designed for dense MIDI workloads where queue buildup, lag, recovery, or event loss are the behavior under test.

## Run

The built application is `dist\MidiBottleneck.exe`. Its output list uses the Windows MIDI output API, so it includes installed software synthesizers (such as Microsoft GS Wavetable Synth, when present) and physical/virtual MIDI devices. When OmniMIDI is installed, the adjacent **KDMAPI** checkbox selects its direct API instead; the ordinary output list is disabled because KDMAPI targets OmniMIDI itself.

To rebuild on Windows PowerShell without installing an SDK:

```powershell
.\build.ps1 -Test
```

An optional hardware integration pass opens every installed MIDI output twice, sends a harmless non-commercial framed SysEx packet, resets it, and switches to the next device:

```powershell
.\build.ps1 -Test -MidiIntegration
```

The build uses the 64-bit .NET Framework compiler included with Windows and produces a WinForms executable. No NuGet packages or network access are required.

## Timing architecture

1. The SMF parser expands delta times to absolute ticks, expands running status, preserves track/order metadata, and represents each channel/system/SysEx message as one dispatchable event.
2. All tempo meta-events are merged into a tempo map. The default 500,000 µs/quarter tempo applies until the first tempo event. Each event tick is converted to an intended real-time timestamp before playback.
3. A dedicated above-normal-priority scheduler thread uses `Stopwatch` (the high-resolution monotonic performance counter) as the transport clock.
4. A Windows high-resolution waitable timer handles coarse waiting. Only the final ~200 µs uses short yield/spin checks, avoiding both ordinary low-resolution sleeps and a continuously pegged CPU core.
5. With **Queue length limit** unchecked, the established FIFO single-server queue remains unlimited. It uses a contiguous index range in the sorted event array, so even a very large backlog adds O(1) queue memory rather than allocating another object per event.
6. With **Queue length limit** checked, arrivals and service completions use a bounded FIFO with the selected overflow policy. The configured count includes the event in service plus pending events. The default limit is 2,000 event slots; this is a practical simulator default, not a claim about any MIDI device.
7. Completed events are dispatched through the selected output layer. Ordinary outputs use `winmm.dll`: short messages use `midiOutShortMsg`, and SysEx buffers use prepared `midiOutLongMsg` headers whose memory remains alive until the driver reports completion. KDMAPI uses OmniMIDI's `SendDirectData` and prepared `SendDirectLongData` calls with the corresponding stream lifecycle.

The processing controls separate two independent concepts:

- **Simulate slowdown** controls whether dispatchable events consume simulated service time. When unchecked, effective service duration is zero without erasing the configured service value.
- **Queue length limit** controls whether buffering is unlimited or finite. It is independent of slowdown, so all four on/off combinations are valid.

The **Rate model** dropdown selects one of two calculations. **Processing time per event** charges the selected constant number of microseconds before every dispatch. **MIDI serial bitrate** charges the actual expanded MIDI message byte count using 10 serial bits per byte at any user-entered bit rate. The **5-pin DIN** button sets exactly 31,250 bit/s; at that rate the model gives 640 µs for two bytes, 960 µs for three bytes, 3.2 ms for ten bytes, and 32 ms for 100 bytes. Each calculation retains its configured value when switching. Processing time, bitrate, Simulate slowdown, and the Rate model are live: a change applies to the next event that begins service and never retroactively changes the event already in service.

The processing-time slider dedicates its first 60% to a linear 0–5,000 µs calibration range, then changes continuously to a logarithmic 5,000 µs–1 second range. Clicking anywhere on the track selects that position; exact numeric entry remains available.

Changing the MIDI-output dropdown or KDMAPI checkbox during playback performs a deliberate restart at the current source position: the scheduler stops, the old output is reset and closed, backlog/statistics are cleared, the replacement is opened, and playback resumes (or remains paused). This is not seamless MIDI state chase; earlier bank/program/controller/sustain/pitch/held-note state is not reconstructed. Enabling/disabling the queue limit and changing its count remain locked during active playback because bounded versus unlimited scheduling is structural. The overflow policy is live and the selected policy is read when the next overflow actually occurs.

## Model notes

- This is intentionally not a tempo multiplier. Source arrivals remain on the tempo-resolved MIDI timeline. Only actual dispatch is delayed or discarded.
- Tempo and other meta-events affect parsing but are not sent to a MIDI output and therefore are not charged as processing units. Channel messages, supported system messages, and each SMF SysEx event are charged once.
- Delayed Note Off messages lengthen notes. An overflow policy that discards a Note Off can leave a note sounding. Pause sends the channel panic sequence directly. Stop and Seek first reset/discard native queued data and then send sustain-off, all-sound-off, and all-notes-off on all 16 channels, so the panic cannot itself be discarded by reset. After Resume or Seek, silenced note/controller state is not reconstructed.
- **Drop newest** preserves the existing bounded FIFO contents and rejects the arrival that finds all slots occupied.
- **Drop oldest** evicts the oldest pending event and admits the new arrival. It never cancels the event already in service; with a one-slot limit it therefore falls back to dropping the newest arrival.
- **Clear buffer and jump to realtime** is experimental. On overflow it discards the simulated in-service and pending backlog, skips source arrivals already behind the current transport clock, clears simulated lag, and terminates sounding notes before continuing from realtime. It is inspired by an observed class of behavior but is not presented as an emulation of the Roland FP-7F or any other hardware.
- Strict FIFO service can reduce backlog and return new events toward their intended timestamps during a sparse passage, but it cannot undo the lateness of events already dispatched.
- The seek timeline maps pointer coordinates directly to 64-bit source time, so long files are not limited by a native trackbar's integer range. Seeking performs a binary search in the sorted event list, resets the MIDI device, discards the old scheduler thread and queue, resets simulation statistics/lag, and starts a fresh service cursor at the selected source timestamp. Active playback resumes immediately; paused playback remains paused. The ±5 second buttons use the same path.
- Format 0 and synchronous format 1 PPQN files are supported. Independent-sequence format 2 and SMPTE time division are rejected with a clear error in this first version.
- MIDI output drivers have their own buffering, transport bandwidth, synthesis latency, and polyphony. The displayed lag is the simulator's dispatch lag, not measured acoustic latency.

### System Exclusive handling

SMF `F0` and `F7` continuation events remain separate processing units in the bottleneck model, but each output layer assembles them into a complete `F0 ... F7` packet before dispatch. Unframed standalone F7 escape data is not submitted as a long message because it is not a complete SysEx packet. Both APIs share the packed Windows multimedia `MIDIHDR` ABI: 64 bytes on x86 and 112 bytes on x64. The exact packet length is stored in `dwBufferLength`, the input-oriented `dwBytesRecorded` field remains zero, flags/reserved fields start at zero, and the data/header remain allocated until completion or reset. WinMM sends through `midiOutLongMsg`; KDMAPI uses OmniMIDI's `PrepareLongData`, `SendDirectLongData`, and `UnprepareLongData` exports.

OmniMIDI's direct path loads and retains the driver module without keeping a bootstrap WinMM device open. It checks the required exports, KDMAPI availability and version before stream initialization, and terminates the stream before releasing the module. A rejected long message stops playback with a bounded diagnostic containing source file/event/tick, F0/F7 fragment sequence, packet framing/length/edge bytes, MIDIHDR size and initialized fields, native operation, and error code. The player does not silently discard all SysEx setup messages. KDMAPI `SendDirectData` and `ResetKDMAPIStream` are `void` in OmniMIDI's contract, so those calls cannot report a per-message native result; long-message calls and initialization do return status.

## Diagnostics

The custom statistics surface contains eight entries in a two-column/four-row grid: theoretical maximum rate, queue current/maximum, processed/dropped events, Effective playback speed, current/maximum lag, Timeline / MIDI output, and Current output rate. The merged timeline value shows intended playback position first and the timestamp of the latest successfully sent MIDI event second.

**Effective playback speed** measures advancement of the MIDI-output frontier relative to transport-clock advancement. Its default 250 ms rolling estimate uses least-squares regression over all valid samples rather than two endpoints. When the finite/unlimited queue is synchronized and empty, the logical output frontier follows the intended timeline through sparse gaps; during backlog, the actual most-recently-sent source timestamp is used. Values above 100% remain visible during catch-up, ordinary variation is not clamped, and insufficient movement displays `—`. Right-click the statistic for Instantaneous, 100 ms, 250 ms, 500 ms, 1.5 s, or a validated custom interval; the preference is remembered while Reset stats clears only the sample history. This measures dispatched source-timeline progress, not synthesizer acoustic tempo or BASS audio-rendering CPU load.

**Current output rate** is a separate 250 ms rolling count of successfully sent events per second. Dropped events do not contribute.

With a finite queue, the custom statistics surface adds a queue-pressure meter. Its numerator includes pending events plus the event currently in service, matching the configured event-slot limit. Green/amber/red accents indicate ordinary, approaching-capacity, and near-full/overflow conditions. The meter is absent for unlimited queues. **Reset stats** resets processed/dropped counts and sets the maximum queue/lag baselines to their current values; it does not seek, clear backlog, reset output, terminate notes, pause, or stop playback.

Statistic values are painted as fixed-width, right-aligned monospace text by one double-buffered custom control. A 16 ms UI timer updates its value snapshot; unchanged snapshots do nothing, and updates invalidate only that surface without TableLayout reflow or ten separate Label repaints.

The playback position, progress bar, and total duration are likewise painted by one double-buffered timeline control. The old path changed a native `TrackBar.Value` and an adjacent auto-sized label independently every frame, causing two separate native/control-tree repaint paths and visible flicker. The unified control updates one immutable position snapshot, performs no layout, and does not invalidate again once the source clock is unchanged at end-of-file. Queue drainage continues to be represented by the separate playback/lag statistics.

The **Analysis...** window is an explicit whole-file projection from 00:00 through the source duration. It displays its aggregation interval (100 ms for ordinary files, automatically widened for very long files), total events and bytes, unique timestamps, cluster participation, message-type totals, and the selected simulator configuration. It shows one primary graph that matches the Rate model: Events/sec for constant processing time or MIDI bytes/sec for serial bitrate. A dashed green reference identifies the selected maximum rate.

Purple diamonds occupy a separate `Simultaneous burst ≥50 events` lane. With a finite limit, translucent red regions identify predicted overflow and a separate green/amber/red strip shows predicted pressure; the legend uses both color/symbol and plain text. Hover and a persistent click-pin are independent. A pin keeps **Seek to pin** enabled while the cursor continues inspecting other buckets; double-clicking also uses the established safe seek path.

The mouse wheel zooms horizontally around the cursor, dragging pans, and **Reset zoom** restores the whole-file range. Axis labels, hover/pin mapping, and seek mapping follow the visible viewport. Two live dashed markers show **Playback timeline** and **MIDI output position** without rebuilding the workload analysis. MIDI output position can remain on the last sent event in a healthy sparse passage. Optional Follow modes track either marker only when it approaches a viewport edge and never alter zoom. Open Analysis windows rebuild after a short debounce for relevant configuration changes, but live markers update independently from playback snapshots. Predictions remain deterministic simulator projections, not hardware measurements.

At widths below 650 client pixels, the main window switches once to a compact layout: nonessential explanatory text is hidden, margins tighten, captions shorten, and the eight statistics remain in two columns. The verified compact minimum is 560 × 600 pixels; normal layout uses a 600 × 660 minimum. Returning to normal restores widths, margins, explanatory rows, and minimum sizing. Controls are not recreated during playback or continuously during resize.

Production Drop tracing records intended timestamp, scheduler decision time, simulated service start/end, accepted/dropped result and reason, track/type/data, same-timestamp cluster size, consecutive overflow drops, instantaneous/maximum buffer occupancy, and current busy-until time. The automated test harness uses this against the real playback scheduler rather than only the reference formula. Capacity-sweep findings and trace locations are in `diagnostics\DropModeAnalysis.md`.

When production Drop tracing is inactive, bounded playback skips trace-only timestamp-cluster scans, diagnostic-string creation, buffer-copy construction, and full queued-service busy-until projections. Overflow decisions, counts, FIFO order, and all three policies are unchanged. This removes an avoidable queue-length-dependent hot-path cost on multi-million-event inputs without introducing benchmark-only scheduler shortcuts.

## Experimental calibration notes

These are listening comparisons supplied by the user, not measurements of device internals or scientifically established service times. With the approximately 166,112-note *Necrofantasia II* reference file:

- about **979 µs/event** has sounded somewhat similar to the user's USB-to-5-pin MIDI adapter reference;
- about **1,399 µs/event** has sounded somewhat similar to the user's Roland FP-7F reference.

The comparisons use recognizable passages near 0:30, 1:17, and 1:36. They are useful repeatability markers for further subjective calibration only.
