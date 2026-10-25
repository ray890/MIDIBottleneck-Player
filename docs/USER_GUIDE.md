# MIDIBottleneck Player 2.0: user guide

This guide is the same material as the program's built-in Help (press **F1**), with a few more explanations. Words in **bold** are names you see on screen.

## 1. What it does

The player reads a MIDI file and sends its messages on time to an output. You can make the output behave as if it were slower or smaller than it is (a "bottleneck"), and see how many messages wait, are dropped or arrive late. It is meant for very dense MIDI files, but it plays ordinary files too.

**No synthesizer is included.** To hear sound you need a MIDI synthesizer or driver that you install yourself (a hardware device, or a software synthesizer). With the output **None** the player works fully but silently.

## 2. The three programs

- **MIDIBottleneck Player x64.exe**: for 64-bit Windows.
- **MIDIBottleneck Player x86.exe**: for 32-bit Windows, or when your MIDI driver is 32-bit.
- **MIDIBottleneck Player XP.exe**: for Windows XP SP3 and any Pentium-class processor (it does not need SSE2).

They all look and behave the same. A program can only use a MIDI driver of its own kind (64-bit with 64-bit, 32-bit with 32-bit).

## 3. Quick start

1. Open a MIDI file with **Open MIDI...**, or drag it onto the main window. Files ending in `.mid`, `.midi` or `.rmi` are accepted.
2. Choose an **Output**. **None** makes no sound.
3. Press **Play**. Press it again to pause.
4. Leave the **Rate model** on **None** (no artificial delay) until you want to try a slowdown or a limit.
5. Press **F1** on any control to open its Help topic.

## 4. Opening a file

- Loading runs in the background with two progress bars. You can cancel at any time; nothing half-loaded is left behind.
- A very large file is looked at first and may show a memory warning with the estimated size. Choose **Cancel** to avoid the risky load.
- A small, repairable fault in the file (a missing status byte after a meta event) is repaired and the repair is announced. Other damaged files are refused with a plain sentence saying what is wrong. Every error box starts with one such sentence; if technical details help (an error code, a position in the file), they follow below under "Details:".
- Supported: standard MIDI files of format 0 and format 1 with the usual "pulses per quarter note" timing, bare or inside a RIFF/RMID wrapper. Not supported: format 2 and SMPTE time division.

## 5. Choosing an output

- **WinMM**: Windows' standard MIDI output.
- **KDMAPI**: the direct interface of synthesizers such as OmniMIDI. You install the synthesizer yourself.
- **None**: no sound; for studying timing and the Analysis.

### Direct WinMM

**Direct WinMM** is a checkbox next to the KDMAPI checkbox. It is off the first time you run the program; after that the program remembers your choice (see "Remembered settings").

Why it exists: on Windows 8 and later, the standard WinMM path handles only a few tens of thousands of messages per second. With the box ticked, the player talks to the installed synthesizer driver directly, which is many times faster for very dense files.

- It has been tested with OmniMIDI and CoolSoft VirtualMIDISynth. Any other synthesizer driver is allowed, but has not been tested.
- It is greyed out as soon as you choose an output that cannot use it: Windows' own synthesizer (Microsoft GS Wavetable Synth) and anything else served by Windows' own MIDI driver (including Windows MIDI Services in Windows 11), the MIDI Mapper, hardware and USB ports, a device with no driver found, and **None**. Those play through normal WinMM. You do not have to press Play first.
- It has no effect while KDMAPI is ticked, so it is greyed out then too (it keeps its tick).
- Hover over the checkbox: its tooltip says in plain sentences whether Direct mode is in use, or why not. A greyed-out box cannot be unticked; choose another output first.
- The path is never switched in the middle of a song.
- If a synthesizer stops answering when it is stopped or closed, the player gives up waiting after a short time and tells you instead of freezing. Notes that were sounding in that synthesizer cannot be silenced by this program.
- If you hear anything wrong, untick it.

## 6. Track routing

Use it to send each track and channel of a song to a destination channel of your choice, for example to fit a song to a device with few usable channels.

1. Load a file and stop playback.
2. Choose **Track routing** in the title-bar menu.
3. Read the list: each track and channel combination, where it will be sent, and warnings about conflicts, instrument changes and drum channels.
4. Correct the destination of the selected row if you wish.
5. Confirm any warning, then press **Apply**.

Good to know:
- **Aggressive routing** picks destinations across the whole song to keep important parts and distinct instrument colours. It may mark a minor row **Omitted** when merging it would damage the music more than leaving it out.
- **Apply** is unavailable while any entry is unresolved.
- Playback, Analysis, Channels, filtering and statistics all use the routed view.
- Routing lasts only for the session; the MIDI file is never changed. **Use original routing** clears the plan while playback is stopped.

## 7. Rate model

The Rate model decides how the player imitates a slower output or limits what is sent. Pick one; the settings under it change with the choice.

- **None**: every message is sent as soon as it is due. This is the start-up choice.
- **Processing time per event**, **MIDI serial bitrate**, **Events per second**: imitate a slower output. Work that cannot be served in time waits in a queue; nothing is lost unless you also set a queue limit.
- **Per-note interval gate** and **Delayed NoteOff**: let fewer note messages through. Unused note starts are dropped together with their matching releases. Releases of notes already sent are always kept.
- **Note start rate gate**: limits how many notes may start in any rolling second.
- **Output rate gate**: limits how many messages of any kind are sent in any rolling second. Rejected work never waits in a catch-up queue.
- **Scale note interval by note traffic** (title-bar menu, off by default): the next interval waits as long as the number of note messages just sent.

Switching to or from None or a gate safely silences the output and restarts playback at the same position.

## 8. Queue and overflow

When the output is slower than the music, work waits in a queue. These settings say how long it may grow and what is thrown away when it is full.

- **Queue length** limits the queue by number of events. You can change it during playback.
- **Limit queue by waiting time** (title-bar menu) limits by how long an event has waited instead (in microseconds; 100000 is 0.1 second).
- **Overflow choices**: **Drop incoming complete notes**, **Drop oldest complete note**, **Drop newest events** (default), **Drop oldest events**, **Clear buffer and jump to realtime**, **Drop background notes**. The "complete note" choices keep a note start and its release together, so no note is left hanging. Dropping single events can drop a Note Off.
- **Apply queue limit without slowdown**: new events beyond the limit are rejected and accepted output is never delayed.
- **Interval polyphony**: with the note-interval gates, the same controls limit note messages per interval (1 to 128).
- Required releases and safety messages are always kept, so a limit can be exceeded slightly.
- The queue bar shows pressure in green, amber and red. Purple means protected releases or safety work beyond an ordinary soft limit.

## 9. Playback

- **Play** starts or resumes; **Stop** silences the output and clears pending work. Use the timeline or the five-second buttons to seek.
- **Chase MIDI state on Play/Seek** (on by default): when starting in the middle, the player first restores the instruments, controllers, pitch bend and pressure the song had set before that point. It does not replay earlier notes.
- **Preserve playback queue on Pause and Seek** (off by default): keeps queued work instead of a safe reset. This can take a long time.
- **Output-paced timeline** (off by default): holds back the song while the output is busy, so later events keep their spacing instead of catching up in a burst.
- **Let other programs run while playback is behind** appears on a PC with a single processor core only (off by default). It keeps the window and other programs responsive when the player cannot keep up, at the cost of falling a little further behind.

## 10. Analysis

Analysis looks at the song without sending anything to an output. It shows the workload, queue pressure, overflow and when the output will finish. With a note gate or the output rate gate it shows which messages the gate would let through. Drag the divider, zoom, or click to inspect a time; **Seek to pin** moves the player to the selected point. **Custom resolution** lets you type your own interval. It does not predict synthesizer delay or live channel mutes.

## 11. Statistics

The live numbers under the timeline: **Timeline / output** (song position and position of the last message sent), **Queue now / maximum**, **Events sent**, **Dropped** (queue drops and gate rejections together), **Output rate**, **Effective speed** and **Lag** (time sent compared with the original MIDI time; it does not measure your sound device). **Reset stats** clears the counters without changing playback.

## 12. Channels

Open **Channels** from Analysis to see the 16 MIDI channels. Normal values are what the player has sent; **blue bold** values are forced overrides; **gray italic** values are history and not proof of what sounds now. Click a value to type or drag it; right-click a forced value to return it to Auto; click the **Ch** cell to mute or unmute a channel. The grid works with the keyboard too (arrow keys, Enter or F2 to edit, Space to mute, Delete to clear, Escape to cancel).

## 13. Window, size and look

Open the title-bar menu by clicking the program icon, pressing **Alt+Space**, or using the menu button when the title bar is hidden.

- **Application scale** (50% to 200%) makes everything larger or smaller on top of Windows' own scaling. At 50% everything is small but nothing is cut off.
- **Borderless main window** removes the Windows frame; **Borderless style** picks Classic, Integrated header or Auto-hide overlay. You can resize from any edge or corner. The mouse pointer changes to a resize arrow at the edges and corners.
- **Show Processing model** and **Show Statistics** hide or show those parts of the window.
- **Always on top** keeps the window above others.

The playback state (Stopped, Paused, Playing, Completed) is shown at the right end of the Playback group's heading.

## 14. Remembered settings

The player remembers the output you chose, the KDMAPI and Direct WinMM ticks and every option of the window menu (scale, borderless style, Always on top, the playback and queue options, the Effective-speed window, and on a PC with one processor core the option "Let other programs run while playback is behind"). It does not remember the window position or the loaded file. The choices are kept in `%APPDATA%\MIDIBottleneck Player\settings.ini`; to keep them next to the program instead, put an empty file named `MIDIBottleneck Player.ini` in the program's folder. Delete the file to return to the standard settings. The program writes no registry entries. None of these choices changes how the music is played.

If the file is damaged, the program starts with the standard settings for the damaged options and says so once in the file-information area of the main window; the next change you make saves a fresh file.
