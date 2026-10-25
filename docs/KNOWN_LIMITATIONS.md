# Known limitations (2.0)

## Sound and drivers

- **No synthesizer is included.** You need a MIDI synthesizer or driver that you install yourself (hardware or software). Without one, use the output **None**, which makes no sound.
- The 64-bit program works only with 64-bit drivers, the 32-bit programs (x86 and XP) only with 32-bit drivers. Drivers are never distributed by this project, and the tests cannot cover every driver.
- **Direct WinMM** (off until you tick it) has been tested with OmniMIDI and CoolSoft VirtualMIDISynth on Windows 10, and its speed gain was also seen on two Windows 11 PCs, one of them with Windows MIDI Services. Other drivers are allowed but untested. It cannot be used with None, the MIDI Mapper, Microsoft GS Wavetable Synth or any other device that Windows' own MIDI driver serves (including Windows MIDI Services), hardware or USB ports. The checkbox is greyed out as soon as you choose such an output, and its tooltip explains. Choosing an output does not open the device or play anything to find this out.
- With Direct WinMM, the player keeps the device open between Stop and the next Play, so another program cannot use a one-user synthesizer in that time. It is released when you change device or output, or close the player.
- A synthesizer driver can occasionally fail to answer when it is stopped or closed (seen with OmniMIDI after a very heavy flood of messages). The player gives up waiting after a short time and says so instead of freezing, but notes still sounding in that driver cannot be silenced by this program.
- A call into a driver that never returns cannot be interrupted by any program. Stop, Pause, Seek and output changes avoid starting a second call at the same time.
- KDMAPI needs a provider that supports the whole prepare/send/unprepare contract for long (SysEx) messages; unsupported combinations fail with a message.

## Windows versions

- Tested: Windows 10, Windows 11 (two PCs) and Windows XP SP3 (XP program). Windows 7, 8 and 8.1 are not tested.
- The XP program is built for any Pentium-class processor (no SSE2) and is somewhat slower than the other two on the same machine.
- The programs are not signed. Windows SmartScreen and some antivirus programs may warn about a newly downloaded, unknown program.
- Windows 8 and later have a slow standard WinMM path (a few tens of thousands of messages per second). Direct WinMM and KDMAPI avoid it.

## MIDI files

- Supported: format 0 and format 1 files with pulses-per-quarter-note timing, bare or in a RIFF/RMID wrapper. Not supported: format 2, SMPTE time division, damaged RIFF wrappers, and files whose running status cannot be recovered unambiguously.
- Very large files can still exhaust the memory of your computer. The 32-bit programs can use up to 4 GiB on 64-bit Windows (2 GiB on 32-bit Windows) and warn earlier. The large-file memory figures are estimates, not guarantees; a load can still fail cleanly if the system cannot give more memory.
- There is a limit of 2,147,483,647 events per file.

## What the models can and cannot do

- Analysis is a prediction from the file and your settings. It does not know about synthesizer delay, live channel mutes (unless you ask it to project the current forced values), or a driver that blocks.
- The Output rate gate's Analysis shows the ideal result for an output that never blocks; live playback uses the real time each call takes.
- Dropping oldest events needs a simulated delayed rate model, because already-sent MIDI cannot be taken back.
- Queue limits are soft for required releases and safety messages: the queue can briefly exceed its limit (purple on the queue bar).
- Dropping single events can drop a Note Off and leave a note sounding; the "complete note" choices avoid this.
- The Chase option restores ordinary channel state when starting in the middle. It deliberately does not restore notes, key pressure, SysEx, system and meta messages, or channel-mode commands.
- **Per-note Maximum rate** counts interval frames per second, not total MIDI traffic.
- "Limit queue by waiting time" measures the player's own modelled waiting, not buffering inside a driver.

## Window and display

- Application scale sits on top of Windows' own display scaling. Very large percentages can be limited by the size of your screen; the Analysis and Channels windows then shrink to fit.
- With Application scale at 50%, text is very small but nothing is cut off.
- Live displays follow your monitor's refresh. On Windows XP, without desktop composition, and in remote sessions, a steady 60 times a second is used.
- The Channel Monitor window costs a little playback speed while it is open (about 1.7 times the cost of playback with it closed in the engine's no-sound test, down from about 10 times earlier in the rewrite).

## Not remembered

- Not remembered, on purpose: the window position and size, the loaded file, and the values on the main window (queue limit, Overflow, Rate model, processing values) and in Analysis, Channels and Track routing. What is remembered (the output, the KDMAPI and Direct WinMM ticks, the window menu options) is kept in `%APPDATA%MIDIBottleneck Playersettings.ini`, or in `MIDIBottleneck Player.ini` next to the program for a portable copy; the two files are not shared.

## Version 1

- Version 1.0.44 stores very short SysEx messages (1 to 3 bytes) as zeros. Version 2 does not have this defect.
