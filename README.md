# MIDI Event Bottleneck Simulator

A small Windows desktop application for playing Standard MIDI Files through a Windows MIDI output while simulating a single-event processing bottleneck. It is designed for dense MIDI workloads where queue buildup, lag, recovery, or event loss are the behavior under test.

## Run

The built application is `dist\MidiBottleneck.exe`. It uses the Windows MIDI output API, so the output list includes installed software synthesizers (such as Microsoft GS Wavetable Synth, when present) and physical/virtual MIDI devices.

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
5. Queue mode is an explicit FIFO single-server model. The queue is a contiguous index range in the sorted event array, which makes a very large backlog O(1) additional memory instead of allocating a second object per queued event.
6. Drop mode walks arrivals and service completions chronologically. An arrival strictly before the current service completion is discarded; an arrival exactly at completion is accepted.
7. Completed events are dispatched through `winmm.dll`. Short messages use `midiOutShortMsg`; SysEx buffers use prepared `midiOutLongMsg` headers whose memory remains alive until the driver reports completion.

Processing time is charged before dispatch. At 0 µs, the simulated service constraint is removed, although Windows, the selected MIDI driver, and the host CPU still impose real limits. A live processing-time edit applies when the next event begins service; it does not retroactively change an event already being processed.

## Model notes

- This is intentionally not a tempo multiplier. Source arrivals remain on the tempo-resolved MIDI timeline. Only actual dispatch is delayed or discarded.
- Tempo and other meta-events affect parsing but are not sent to a MIDI output and therefore are not charged as processing units. Channel messages, supported system messages, and each SMF SysEx event are charged once.
- Delayed Note Off messages lengthen notes. In drop mode, a dropped Note Off can leave a note sounding. Pause and Stop send an all-sound-off/all-notes-off safety reset; after Resume, notes that were silenced are not reconstructed.
- Drop mode is a zero-capacity waiting-room model: the event already in service is retained and every arrival strictly inside its busy interval is discarded. Events at or after service completion survive. Dense Black MIDI commonly has many messages at the exact same timestamp, so at any nonzero service time only the first message in such a simultaneous cluster can survive; sparse engine-level tests verify that nonzero service time by itself does not cause drops.
- Strict FIFO service can reduce backlog and return new events toward their intended timestamps during a sparse passage, but it cannot undo the lateness of events already dispatched.
- The seek bar has one million logical positions, giving sub-millisecond positioning for ordinary song lengths. Seeking performs a binary search in the sorted event list, resets the MIDI device, discards the old scheduler thread and queue, resets simulation statistics/lag, and starts a fresh service cursor at the selected source timestamp. Active playback resumes immediately; paused playback remains paused. The ±5/±10 second buttons use the same path.
- Format 0 and synchronous format 1 PPQN files are supported. Independent-sequence format 2 and SMPTE time division are rejected with a clear error in this first version.
- MIDI output drivers have their own buffering, transport bandwidth, synthesis latency, and polyphony. The displayed lag is the simulator's dispatch lag, not measured acoustic latency.

### System Exclusive handling

SMF `F0` and `F7` continuation events remain separate processing units in the bottleneck model, but the Windows output layer assembles them into a complete `F0 ... F7` packet before calling `midiOutLongMsg`. Unframed standalone F7 escape data is skipped because strict Windows drivers reject it as a long message. Output `MIDIHDR` structures set `dwBytesRecorded` to zero, as required for output buffers, and remain allocated until completion or reset. Opening another output cleanly resets, unprepares, and closes the previous device first.

## Diagnostics

The interface reports the configured theoretical service rate, current and maximum queue length, processed/dropped counts, monotonic playback clock, intended timeline position, source timestamp of the latest dispatched event, and current/maximum simulated dispatch lag. Reset Statistics clears counters and maxima without changing playback.

Statistic values use fixed-width, right-aligned monospace fields so changes in units or digit counts do not shift adjacent text.

## Experimental calibration notes

These are listening comparisons supplied by the user, not measurements of device internals or scientifically established service times. With the approximately 166,112-note *Necrofantasia II* reference file:

- about **979 µs/event** has sounded somewhat similar to the user's USB-to-5-pin MIDI adapter reference;
- about **1,399 µs/event** has sounded somewhat similar to the user's Roland FP-7F reference.

The comparisons use recognizable passages near 0:30, 1:17, and 1:36. They are useful repeatability markers for further subjective calibration only.
