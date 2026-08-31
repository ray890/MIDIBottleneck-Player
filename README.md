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

The **Rate model** dropdown selects one of two calculations. **Processing time per event** charges the selected constant number of microseconds before every dispatch. **MIDI bitrate / byte transmission** charges the actual expanded MIDI message byte count using 10 serial bits per byte at any user-entered bit rate. At the exact 31,250 bit/s DIN preset this gives 640 µs for two bytes, 960 µs for three bytes, 3.2 ms for ten bytes, and 32 ms for 100 bytes. Each calculation retains its configured value when switching. A live value edit applies when the next event begins service; it does not retroactively change an event already being processed.

The MIDI-output dropdown and KDMAPI checkbox remain selectable during playback without replacing or closing the active output. Their new selection applies on the next playback start. Queue-limit and overflow controls remain locked during playback because those structural settings are snapshotted when a run starts.

## Model notes

- This is intentionally not a tempo multiplier. Source arrivals remain on the tempo-resolved MIDI timeline. Only actual dispatch is delayed or discarded.
- Tempo and other meta-events affect parsing but are not sent to a MIDI output and therefore are not charged as processing units. Channel messages, supported system messages, and each SMF SysEx event are charged once.
- Delayed Note Off messages lengthen notes. An overflow policy that discards a Note Off can leave a note sounding. Pause and Stop send an all-sound-off/all-notes-off safety reset; after Resume, notes that were silenced are not reconstructed.
- **Drop newest** preserves the existing bounded FIFO contents and rejects the arrival that finds all slots occupied.
- **Drop oldest** evicts the oldest pending event and admits the new arrival. It never cancels the event already in service; with a one-slot limit it therefore falls back to dropping the newest arrival.
- **Clear buffer and jump to realtime** is experimental. On overflow it discards the simulated in-service and pending backlog, skips source arrivals already behind the current transport clock, clears simulated lag, and terminates sounding notes before continuing from realtime. It is inspired by an observed class of behavior but is not presented as an emulation of the Roland FP-7F or any other hardware.
- Strict FIFO service can reduce backlog and return new events toward their intended timestamps during a sparse passage, but it cannot undo the lateness of events already dispatched.
- The seek timeline maps pointer coordinates directly to 64-bit source time, so long files are not limited by a native trackbar's integer range. Seeking performs a binary search in the sorted event list, resets the MIDI device, discards the old scheduler thread and queue, resets simulation statistics/lag, and starts a fresh service cursor at the selected source timestamp. Active playback resumes immediately; paused playback remains paused. The ±5 second buttons use the same path.
- Format 0 and synchronous format 1 PPQN files are supported. Independent-sequence format 2 and SMPTE time division are rejected with a clear error in this first version.
- MIDI output drivers have their own buffering, transport bandwidth, synthesis latency, and polyphony. The displayed lag is the simulator's dispatch lag, not measured acoustic latency.

### System Exclusive handling

SMF `F0` and `F7` continuation events remain separate processing units in the bottleneck model, but each output layer assembles them into a complete `F0 ... F7` packet before dispatch. Unframed standalone F7 escape data is skipped because strict drivers reject it as a long message. Output `MIDIHDR` structures set `dwBytesRecorded` to zero and remain allocated until completion or reset. WinMM sends the header through `midiOutLongMsg`; KDMAPI uses OmniMIDI's `PrepareLongData`, `SendDirectLongData`, and `UnprepareLongData` exports. Opening another output cleanly resets, unprepares, and closes the previous path first.

## Diagnostics

The interface reports the configured theoretical service rate, current and maximum queue length, processed/dropped counts, monotonic playback clock, intended timeline position, source timestamp of the latest dispatched event, and current/maximum simulated dispatch lag. Seeking starts a fresh scheduler run and resets the statistics. Play and Pause share one state-aware button.

Statistic values are painted as fixed-width, right-aligned monospace text by one double-buffered custom control. A 16 ms UI timer updates its value snapshot; unchanged snapshots do nothing, and updates invalidate only that surface without TableLayout reflow or ten separate Label repaints.

The playback position, progress bar, and total duration are likewise painted by one double-buffered timeline control. The old path changed a native `TrackBar.Value` and an adjacent auto-sized label independently every frame, causing two separate native/control-tree repaint paths and visible flicker. The unified control updates one immutable position snapshot, performs no layout, and does not invalidate again once the source clock is unchanged at end-of-file. Queue drainage continues to be represented by the separate playback/lag statistics.

The **Analysis...** window is an explicit whole-file projection from 00:00 through the source duration. It displays its aggregation interval (100 ms for ordinary files, automatically widened for very long files), total events and bytes, unique timestamps, cluster participation, message-type totals, and the selected simulator configuration. Double-buffered historical graphs show event rate and MIDI byte rate against a labeled time axis. A dashed green line and label show the selected **maximum rate** in events/sec or bytes/sec. Magenta ticks identify buckets containing clusters of at least 50 simultaneous events; red ticks identify predicted overflow. With a finite limit, a pressure strip shows predicted occupancy relative to the selected capacity. Open Analysis windows refresh after a short debounce whenever the rate model, rate value, slowdown switch, queue limit, or overflow policy changes. Predicted drops and buffer clears are deterministic simulator projections, not hardware measurements.

Production Drop tracing records intended timestamp, scheduler decision time, simulated service start/end, accepted/dropped result and reason, track/type/data, same-timestamp cluster size, consecutive overflow drops, instantaneous/maximum buffer occupancy, and current busy-until time. The automated test harness uses this against the real playback scheduler rather than only the reference formula. Capacity-sweep findings and trace locations are in `diagnostics\DropModeAnalysis.md`.

## Experimental calibration notes

These are listening comparisons supplied by the user, not measurements of device internals or scientifically established service times. With the approximately 166,112-note *Necrofantasia II* reference file:

- about **979 µs/event** has sounded somewhat similar to the user's USB-to-5-pin MIDI adapter reference;
- about **1,399 µs/event** has sounded somewhat similar to the user's Roland FP-7F reference.

The comparisons use recognizable passages near 0:30, 1:17, and 1:36. They are useful repeatability markers for further subjective calibration only.
