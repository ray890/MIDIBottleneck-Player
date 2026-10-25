# 2.0.0 (2026-10-25)

Version 2.0 is a complete rewrite of MIDIBottleneck Player as a native Windows program. **Every feature of version 1.0.44 is kept.** Nothing is removed. The sections below list what is new or better. Each item has an entry with its evidence in the improvement register (`v2/docs/DIFFERENCES_FROM_V1.md`); the entry numbers are in brackets.

Speed and size figures were measured on the author's development PC, with no sound device involved; yours will differ.

## Three programs, no runtime

- **MIDIBottleneck Player x64.exe**, **MIDIBottleneck Player x86.exe** and **MIDIBottleneck Player XP.exe** (Windows XP SP3 and any Pentium-class processor, no SSE2 needed). Single files, nothing to install. The .NET Framework that version 1 needed is no longer required.
- The 32-bit programs can use up to 4 GiB of memory on 64-bit Windows, so larger files load (about 150 million events, against about 76 million without it) (DD-060).
- The programs are not signed. See the README for the Windows warning and the checksums.
- This release is version 2.0.0; the About box reads "Release 2.0.0 • version 2.0.0.0".

## New

- **Direct WinMM** checkbox next to KDMAPI (off by default): plays a WinMM device by talking to the installed synthesizer driver directly, many times faster than Windows' standard path on Windows 8 and later. Tested with OmniMIDI and CoolSoft VirtualMIDISynth. It is greyed out as soon as you choose an output that cannot use it (None, the MIDI Mapper, Windows' own MIDI driver including Windows MIDI Services, a device with no suitable driver), with its tick kept, and its tooltip says in plain sentences whether Direct mode is in use or why not (DD-085, DD-094, DD-110, DD-111, DD-127, DD-130).
- **Windows XP is supported** by a dedicated build (DD-095, DD-117).
- An optional menu item, "Let other programs run while playback is behind", appears on a PC with a single processor core (on by default only when exactly one logical processor is usable; the choice is remembered) (DD-112).
- The state of playback (Stopped, Paused, Playing, Completed) is shown at the right end of the Playback heading in every window style and stays visible while a file loads (DD-128).
- F1 works inside the Analysis, Channels and Track routing windows (DD-120).
- The Channels grid can be used entirely from the keyboard (DD-031).
- The KDMAPI search also looks where the installed driver is registered (DD-119).

## Better

- **Notes arrive on time.** The final approach to each event no longer sleeps, so notes and controller changes go out within microseconds of their time instead of up to 15 milliseconds late (DD-002).
- **Dense playback clears about four times faster** (a burst of events already due is sent back to back; 4.3 times on the x64 program in a quiet-PC test) (DD-065).
- **Loading** uses about half the memory and about half the time; events take 24 bytes instead of 40 (DD-021, DD-022, DD-056).
- Track routing is planned faster; the Analysis queue projection is two to four times faster (DD-023, DD-075).
- The Analysis queue projection with a limited queue and "Drop background notes" is far faster on very large files (a 10.4-million-event file: 22 minutes before, 3.5 seconds now; v1 took 32 minutes), with the same results; changing a value stops the running projection within about a millisecond (DD-160).
- An open Channels window costs far less playback speed than it did earlier in the rewrite (DD-092); it is still a little slower than with the window closed.
- The window appears sooner (DD-057); resizing repaints cleanly (DD-090).
- Live numbers are redrawn in step with your monitor and only while something changes; drawing never takes more than a set share of the interface thread (DD-035, DD-109).
- Application scale also works at 50% and 75% without anything being cut off; check boxes are drawn at the chosen scale (DD-099, DD-100, DD-101).
- Every secondary window opens on the same monitor as the main window (DD-098).
- Error messages start with one plain sentence saying what went wrong. Below it, after a blank line, comes "Details:" with the technical information (an error code, a position in the file, the driver's or Windows' own text) for bug reports, only where it helps (DD-003, DD-012, DD-129).
- A built-in Help written for easy reading, with the same eight topics (DD-108).

## Fixed (defects in version 1.0.44)

- **Very short SysEx messages** (1 to 3 bytes) were stored as zeros; they are now sent exactly as written (DD-001).
- With a forced override or a muted channel, a very large backlog could freeze the player; it now stays responsive (DD-093).
- The "Interval polyphony" label no longer changes back to "Queue length limit" in a narrow window (UI differences, item 5).
- "Clear buffer" at a full queue no longer counts muted channels' events as dropped (DD-041).
- Several statistics of the Per-note and Delayed NoteOff modes were corrected (DD-009), and a Pause or restart exactly on an event's time no longer sends that event twice (DD-007).
- A manual routing destination outside channels 1 to 16 no longer breaks the dialog (DD-005).

## Changed on purpose

- **Settings are remembered between sessions** in a small text file: `%APPDATA%MIDIBottleneck Playersettings.ini`, or, for a portable copy, an empty file named `MIDIBottleneck Player.ini` placed next to the program (it is then used instead). Remembered: the output, the KDMAPI and Direct WinMM ticks, every option of the window menu (scale, borderless style, Always on top, the playback and queue options, the Effective-speed window) and the few-processor option. Never remembered: the window position and size and the loaded file. A missing or damaged file never stops the program from starting; delete it to return to the standard settings. Version 1 remembered only the Effective-speed window and did not read or write this file; version 2 never touches the registry (DD-026, DD-153).
- Overflow choices have clearer names and the complete-note choices come first; with a waiting-time limit, "Clear buffer and jump to realtime" is first (DD-025, DD-086).
- A provider that never answers the readiness probes now fails to open with a clear message (DD-010).
- Borderless-mode resize arrows: the pointer now changes at every edge and corner (UI differences, item 2).
- Scrub boxes (the draggable number fields) drag logarithmically without Shift (DD-104).

## Removed

Nothing.

