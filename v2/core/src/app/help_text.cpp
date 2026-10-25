// The offline Help text of MIDIBottleneck Player v2, written by hand for v2.
// It keeps v1's eight topics and their order, and v2's F1 mapping uses the same topics.
// The text is in the small markup described in help_text.h (# heading, ## section, - bullet, 1. step, **bold**).
// Keep sections short, prefer lists and numbered steps to paragraphs, and bold the names the user sees on screen.
// Plain ASCII only (the markup parser itself is UTF-8 safe).
#include "midibn/app/help_text.h"

namespace midibn {
namespace app {
namespace {

const char kHelpOverview[] = R"HELP(# Overview
MIDIBottleneck Player plays MIDI files and shows what happens when a very dense file meets an output that can take only so many messages per second, or a queue that can hold only so much.

## Quick start
1. Open a MIDI file with **Open MIDI...**, or drag it onto the main window.
2. Choose an **Output**. **None** makes no sound; it is the safe choice for studying timing and the Analysis.
3. Press **Play**. Press it again to pause.
4. Leave the **Rate model** on **None** (no artificial delay) until you want to try a slowdown or a gate.
5. Press **F1** on any control to open the Help topic for it.

## Help topics
- **Overview** - this page.
- **File and output** - opening files, outputs, Track routing.
- **Rate model** - slowdowns and gates.
- **Queue and overflow** - queue limits and what is dropped.
- **Playback** - Play, Pause, Stop, seeking and related options.
- **Analysis** - the prediction graph.
- **Statistics** - the live numbers.
- **Channels** - the 16-channel window.

## The title-bar menu
Click the program icon in the title bar, press **Alt+Space**, or use the menu button when the title bar is hidden. It holds:
- **About**, **Help**, **Track routing**, **Always on top**.
- **Borderless main window** and **Borderless style**.
- **Show Processing model** and **Show Statistics**.
- **Application scale**.
- Playback options: **Chase MIDI state**, **Preserve playback queue**, **Output-paced timeline**.
- On a PC with a single processor core only: **Let other programs run while playback is behind**.
- Queue options: **Apply queue limit without slowdown**, **Limit queue by waiting time**, **Scale note interval by note traffic**.

## Window size and look
- **Application scale** makes the program's windows larger or smaller, from 50% to 200%, on top of the scaling Windows already uses for your screen. At 50% everything is small but nothing is cut off.
- **Borderless main window** removes the Windows frame. **Borderless style** picks how:
- **Classic** - a strip with the window buttons.
- **Integrated header** - the window buttons are built into the File and output heading.
- **Auto-hide overlay** - never moves the player's layout.
- Every style keeps the taskbar button, moving and snapping, Alt+Space, and keyboard access to minimize, maximize and close. You can resize from any edge or corner. Turning borderless off restores the earlier frame, size and position.
- **Show Processing model** and **Show Statistics** hide or show those two parts of the window.

## Good to know
- The program **remembers** the choices in the title-bar menu, the **Output**, and the **KDMAPI** and **Direct WinMM** ticks, and starts with them next time. A remembered output that is no longer connected is replaced by the usual one. It does not remember the window position or size, the loaded file, or the values in the main window, Analysis, Channels and Track routing.
- They are kept in a small text file, **settings.ini**, in the folder **MIDIBottleneck Player** under your Application Data folder (%APPDATA%). To keep them next to the program instead, put an empty file named **MIDIBottleneck Player.ini** in the program's folder. Delete the file to go back to the standard settings. These choices never change how the music is played.
- Live numbers, timeline and graphs are redrawn once per refresh of your monitor (60 times a second on a 60 Hz monitor). On Windows XP, without desktop composition, and in remote sessions a steady 60 times a second is used. Nothing is redrawn while the player is idle.
- Analysis, Channels, Track routing, Help, About and every prompt open on the same monitor as the main window.
- The full user guide is in the public repository.
)HELP";

const char kHelpFileAndOutput[] = R"HELP(# File and output

## Opening a file
- Open a **.mid**, **.midi** or RIFF/RMID **.rmi** file, or drag one file anywhere onto the main window.
- Loading runs in the background with two progress bars. You can cancel at any time; no half-loaded song is left behind.
- A large file is inspected first and may show a memory warning. **Cancel** there prevents the risky load.
- A small, repairable running-status fault is repaired and the repair is announced afterwards. Other damaged files are refused with a plain message saying what is wrong.

## Output
- **WinMM** - plays through a Windows MIDI output.
- **KDMAPI** - plays through OmniMIDI's direct interface. It needs a compatible provider that you install yourself.
- **None** - plays nothing audible. For testing timing and Analysis.

## Direct WinMM driver access
The **Direct WinMM** box sits next to **KDMAPI**. It is off by default; once you tick it, it stays ticked the next time the program starts (it is remembered with the other settings). On Windows 8 and later the normal WinMM path is slow: only a few tens of thousands of messages a second. With the box ticked, a WinMM device is played by talking to the installed synthesizer driver directly, which is many times faster.
- Hover over the box to read, in plain words, whether Direct mode is in use or why not. When it is in use, the tooltip names the driver and says whether it has been tested (OmniMIDI and CoolSoft VirtualMIDISynth have been tested; any other driver is allowed but not tested).
- The box is greyed out, with its tick kept, when Direct mode cannot be used: while **KDMAPI** is ticked, when the output is **None**, and for a device that cannot be driven this way, such as Microsoft GS Wavetable Synth or the MIDI Mapper. Such a device plays through normal WinMM, and the tooltip says so and why. The program checks this as soon as you choose the output, without opening the device; if a driver refuses only when playback starts, the box turns grey then, and choosing another output and then this one tries again. The path is never switched in the middle of a song.
- If a synthesizer stops answering when it is stopped or closed, the player gives up waiting after a short time and says so instead of freezing. Notes that were sounding in that driver cannot be silenced by this program.
- If you hear anything wrong, untick the box.
## Track routing
Use it to send each track and channel of a song to a destination channel of your choice.
1. Load a file and stop playback.
2. Choose **Track routing** in the title-bar menu.
3. Read the list: every track and channel combination (one track appears more than once if it uses several channels), where it will be sent, and warnings about conflicts, instrument changes and drum channels.
4. Correct the destination of the selected row if you wish.
5. Confirm any warning, then press **Apply**.

The planner works in the background, so the window stays usable while it calculates.

### Things to know
- **Aggressive routing** picks destinations across the whole song, balancing important parts and distinct instrument colors, instead of filling channels with the first safe groups.
- Aggressive may mark a minor row **Omitted** when merging it would damage the music more than leaving it out. This is a valid plan that loses some notes, not an error.
- **Apply** is unavailable while any entry is genuinely unresolved. Plans that use General MIDI families, Aggressive routing, omitted entries or manual changes need your confirmation of a warning.
- Bank and Program settings follow their source channel even when stored in another track. Each output channel gets its own instrument timeline.
- A summary says how many entries were routed, shared a channel, changed instrument or were omitted, and how many Bank/Program messages were generated.
- Playback, Analysis, Channels, filtering and statistics all use the routed view.
- Routing lasts only for the session; the MIDI file is never changed. **Use original routing** clears the plan while playback is stopped.
- Large files are handled without making a second copy of the song just to preview a route.
)HELP";

const char kHelpRateModel[] = R"HELP(# Rate model
The Rate model decides how the player imitates a slower output or limits what is sent. Pick one; the settings below it change with the choice.

## None
Every message is sent as soon as it is due, with no delay. This is the choice at start-up.

## Slowdown models: the work is kept
**Processing time per event**, **MIDI serial bitrate** and **Events per second** imitate a slower output.
- Work that cannot be served in time waits in a queue.
- Zero processing time, or zero events per second, means immediate service.
- A live change of the rate applies when the next event starts to be served.

## Note-interval gates: fewer note messages
- **Per-note interval gate** lets through one chosen transition per pitch in each interval.
- **Delayed NoteOff** gives each pitch one owner, a minimum length and a short recovery after release. During the recovery it keeps one musically chosen replacement, so a repeated melody note is not lost just because the old note had to end first.
- These gates drop unused note starts together with their matching releases. Releases for notes already sent are always kept.

## Note start rate gate: limit note starts
Counts only the starts of notes (NoteOn with a velocity above zero), not all MIDI traffic.
- In any rolling second it never lets through more than the maximum you set.
- A small fixed tolerance keeps ordinary chords and passages below the limit intact. Quiet time cannot be saved up as extra allowance. At the maximum setting normal chords are unchanged.
- When several starts compete, the shared musical ranking prefers prominent chord notes and fair representation of the parts.
- A rejected start and its matching release are both removed. Releases of admitted starts, state messages and safety messages are protected.

## Output rate gate: limit all messages sent
**Maximum output rate** keeps the number of ordinary messages sent in any rolling second at or below the maximum, with the same small tolerance. It is separate from the Events per second slowdown, and rejected work never waits in a catch-up queue.
- A note start that is let through reserves room for its release. Controllers and source state use ordinary room too.
- Stop, Panic, replacement, failure and shutdown clean-up may go beyond the limit and are counted separately. A rare required message sent without room is reported as **required messages sent over target**.
- When room is short, whole unsent notes are rejected using the shared musical ranking.
- Analysis shows the ideal result for an output that never blocks. Live playback uses the real time each output call takes, so a driver that blocks can change later decisions.

## Reading the graph
The Analysis graph shows events per second averaged over the chosen interval, which is not the exact rolling-second count. A short interval can briefly read above **Maximum output rate** because of the tolerance, even though every rolling second stays within the target. The matching rejected count can appear one second later.

## Scale note interval by note traffic
Off by default. It can be prepared under any Rate model while playback is stopped, and only the two note-interval gates use it.
- Example: if a batch sends 3 note messages, the next batch waits 3 intervals.
- A chord can still be one burst followed by proportional waiting, so this is not an events-per-second ceiling.

## Delays inside and outside the player
- A note is never carried into another batch only because interval pacing held it. The one replacement kept by Delayed NoteOff and the excess releases held by Interval polyphony are the only exceptions.
- With a zero interval neither gate adds delay.
- A scheduler delay or a blocking MIDI output is outside the model: work already accepted stays and may be sent close together when the delay ends. Analysis shows the planned timing, not driver blocking.

Switching to or from None or a gate safely silences the output and restarts playback at the same position.
)HELP";

const char kHelpQueueAndOverflow[] = R"HELP(# Queue and overflow
When the output is slower than the music, work waits in a queue. These settings say how long that queue may grow and what is thrown away when it is full.

## Queue limit
**Queue length** caps the delayed work by number of events.
- You can change the number during normal limited playback.
- An increase applies at once.
- A decrease neither cuts the event being sent nor silently discards work already accepted. New arrivals follow the new limit.

## Limit queue by waiting time
In the title-bar menu, **Limit queue by waiting time** replaces the event count by a time: the queue may not hold an event that has already waited longer than the number you give, in microseconds (millionths of a second). Example: 100000 means 0.1 second.

Overflow then offers three choices:
- **Clear buffer and jump to realtime** (the first, and the default for this mode)
- **Drop oldest complete note** (a soft limit)
- **Drop oldest events**

## Overflow choices (with a queue length limit)
The list is: **Drop incoming complete notes**, **Drop oldest complete note**, **Drop newest events** (the default), **Drop oldest events**, **Clear buffer and jump to realtime**, **Drop background notes**.
- **Drop newest events** and **Drop oldest events** remove single pending events. A single event can be a Note Off, so with **Apply queue limit without slowdown** one of these can leave a note sounding. The complete-note choices avoid this.
- The complete-note choices keep a note start and its matching release together.
- **Clear buffer and jump to realtime** discards what is waiting and continues with the music as it is now.
- **Drop background notes** uses musical context to keep prominent chord notes and a fair share of every track.
- Required releases and safety work are always kept, so an ordinary queue limit can be exceeded slightly.

## Apply queue limit without slowdown
Turns the limit into a pure rule: new arrivals beyond the limit are rejected, and accepted output is never delayed. Each mode shows only the overflow choices that can work safely.

## Interval polyphony
With **Per-note interval gate** or **Delayed NoteOff** selected, the same controls become a separate **Interval polyphony** limit. It is off by default and allows 1 to 128 note messages in one interval. It is not the synthesizer's own limit on sounding voices.
- Required releases come before new note starts.
- If releases alone fill an interval, only the excess moves to the earliest later intervals.
- No interval goes over the value; new starts are dropped, not postponed to another interval.

## The queue bar
**Queue now / maximum** shows how much work is waiting.
- **Green**, **amber** and **red** show rising pressure.
- **Purple** marks protected releases or safety work beyond an ordinary soft limit.
)HELP";

const char kHelpPlayback[] = R"HELP(# Playback

## The basics
- **Play** starts the loaded song; the same button pauses and resumes.
- **Stop** silences the output and clears pending work.
- Use the **timeline** or the **five-second buttons** to seek.
- The state (**Stopped**, **Paused**, **Playing** or **Completed**) is shown at the right end of the Playback heading.

## Chase MIDI state on Play/Seek
On by default (title-bar menu). When playback starts or seeks to a point after the beginning, it first restores the instrument, controller, pitch-bend and pressure settings the song had set before that point. It does not replay earlier notes.

## Preserve playback queue on Pause and Seek
Off by default. Pause and Seek normally use a safe reset. With this option the queued work is kept instead:
- **Pause** parks the same worker.
- **Seek** lets the accepted work finish, sends All Notes Off, then the chased state and target events.
- This can take a long time and does not guarantee instant silence.

## Output-paced timeline
Off by default. It holds back the song's own progress while the output is busy accepting a message or while modeled service is in progress. Later events keep their spacing instead of catching up in a burst. Analysis models the planned service but cannot predict a driver that blocks.

## Direct WinMM driver access
Now a box next to **KDMAPI**, described under **File and output**. It is off at every start.
)HELP";

const char kHelpAnalysis[] = R"HELP(# Analysis
Analysis examines the song without sending anything to an output.

## What it shows
- For the ordinary rate models: the workload of the source, queue pressure, overflow, and when the output will finish.
- With a note gate or the output rate gate: which messages the gate would let through, at their planned times. The output rate gate is an ideal prediction for an output that never blocks.
- With **Output-paced timeline** on, the song's progress pauses during simulated service. A driver that blocks cannot be predicted.
- The first workload picture can appear before the slower queue prediction is ready.
- It does not predict synthesizer delay or live channel mutes. Its overflow wording uses the same names as the main window.

## Using the graph
- Drag the divider, zoom the graph, or click to inspect a time.
- **Seek to pin** can move the player to the selected point.
- The window's title-bar menu can show whole event counts per graph interval instead of events per second. The graph is redrawn without recalculating.
- **Custom resolution** lets you type an interval of your own.

## Project forced overrides
Off by default. It takes a snapshot of the values forced in the Channels window, applies them after Track routing, and reports the generated messages separately from the filtered source messages. It affects only the Analysis window and never sends MIDI.
)HELP";

const char kHelpStatistics[] = R"HELP(# Statistics
The live numbers under the timeline.

## What each number means
- **Timeline / output** - the song's current position, and the position of the last message sent to the output.
- **Queue now / maximum** - work waiting to be sent, including while an output call is blocked.
- **Events sent** - leaves out events removed by a mute or a forced override.
- **Dropped** - events dropped by the queue plus events rejected by a gate, added together. The separate reasons are kept for diagnostics.
- **Output rate** - the recent rate of sending.
- **Effective speed** - the progress of the song compared with real time.
- **Lag** - the time a message was sent compared with its original MIDI time. It does not measure the latency of your sound device.

## Good to know
- With the Output rate gate, required safety or state messages sent over its target are reported on their own and are not counted as dropped.
- **Reset stats** clears the counters without changing playback.
)HELP";

const char kHelpChannels[] = R"HELP(# Channels
Open **Channels** from Analysis to look at the 16 MIDI channels.

## Reading the values
- Normal values - those the player has seen sent.
- **Blue bold** values - forced overrides.
- **Gray italic** values - history from the source or from a former output; not proof of what sounds now.
- Program numbers stay visible. Melodic channels show General MIDI instrument names.
- Channel 10 uses the displayed Bank and Program to name recognized GM2, GS and XG drum kits; an unknown combination is shown as a numbered drum kit. Names can differ on particular devices.

## With the mouse
- **Click** a value to type it or drag it.
- **Right-click** a forced value to hand it back to Auto.
- **Right-click** a historical value to ask for one value taken from the song, sent in order with the rest of the output.
- **Click the Ch cell** to mute or unmute a channel. Muting sends safety releases and filters the channel's later messages.

## With the keyboard
- **Arrow keys** move between cells. **Home** and **End** go to the first and last column; **Ctrl+Home** and **Ctrl+End** to the first and last cell; **Page Up** and **Page Down** move a page.
- **Enter** or **F2** types a value into the current cell (on the Ch cell they mute or unmute the channel).
- **Space** mutes or unmutes the channel of the current row.
- **Delete** does what right-click does.
- **Escape** cancels an edit.
- The dotted focus mark appears after the first key press in the grid.
)HELP";

// ---- Linux wording (DD-126). The Windows topics above are used as they are; the Linux text is made from them by
// ---- swapping the few parts that talk about Windows. "@MENU@" is the menu button's label (the three-bar sign).
const char kHelpOverviewLinux[] = R"HELP(# Overview
MIDIBottleneck Player plays MIDI files and shows what happens when a very dense file meets an output that can take only so many messages per second, or a queue that can hold only so much.

## Quick start
1. Open a MIDI file with **Open MIDI...**, or drag it onto the main window.
2. Choose an **Output**. **None** makes no sound; it is the safe choice for studying timing and the Analysis. Any other entry is a MIDI port of the Linux sound system (ALSA).
3. Press **Play**. Press it again to pause.
4. Leave the **Rate model** on **None** (no artificial delay) until you want to try a slowdown or a gate.
5. Press **F1** on any control to open the Help topic for it.

## Help topics
- **Overview** - this page.
- **File and output** - opening files, outputs, Track routing.
- **Rate model** - slowdowns and gates.
- **Queue and overflow** - queue limits and what is dropped.
- **Playback** - Play, Pause, Stop, seeking and related options.
- **Analysis** - the prediction graph.
- **Statistics** - the live numbers.
- **Channels** - the 16-channel window.

## The menu
Click the **@MENU@** button next to the file name. It holds:
- **About**, **Help**, **Track routing**, **Always on top**.
- **Show Processing model** and **Show Statistics**.
- **Application scale**.
- Playback options: **Chase MIDI state**, **Preserve playback queue**, **Output-paced timeline**.
- On a PC with a single processor core only: **Let other programs run while playback is behind**.
- Queue options: **Apply queue limit without slowdown**, **Limit queue by waiting time**, **Scale note interval by note traffic**.

## Window size and look
- **Application scale** makes the program's windows larger or smaller, from 50% to 200%, on top of the font size your desktop already uses.
- **Show Processing model** and **Show Statistics** hide or show those two parts of the window.
- **Always on top** asks your window manager to keep the window above the others. Some window managers ignore the request.
- The window frame, its buttons and its menu belong to your desktop.

## Good to know
- The program **remembers** the choices in the menu, and the **Output**, and starts with them next time. A remembered output that is no longer there is replaced by the usual one. It does not remember the window position or size, the loaded file, or the values in the main window, Analysis, Channels and Track routing.
- They are kept in a small text file, **settings.ini**, in the **midibottleneck** folder of your configuration folder (usually ~/.config, or $XDG_CONFIG_HOME). Delete the file to go back to the standard settings. These choices never change how the music is played.
- Live numbers, timeline and graphs are redrawn in step with your screen, and only while there is something to show. Nothing is redrawn while the player is idle.
- The full user guide is in the public repository.
)HELP";

const char kHelpOutputLinux[] = R"HELP(## Output
- **None** - plays nothing audible. For testing timing and Analysis.
- Every other entry is a MIDI port of the ALSA sequencer: a hardware MIDI device, or a software synthesizer such as FluidSynth or TiMidity++, or **Midi Through**, which only passes the messages on to other programs. Start the synthesizer first, then choose its port.
- Only **None** in the list means no usable port was found. If the program cannot open the sequencer, it says so and suggests joining the **audio** group.
- Messages are sent to the port at once, the moment they are due. Long System Exclusive messages are sent in pieces.

)HELP";

// ---- Android wording. The phone app has a file picker instead of drag and drop, USB MIDI devices instead of ALSA ports
// ---- or Windows drivers, the built-in Sonivox synthesizer and its Synth settings, no window frame, and the menu of the
// ---- top bar (three bars) instead of a title-bar menu.
const char kHelpOverviewAndroid[] = R"HELP(# Overview
MIDIBottleneck Player plays MIDI files and shows what happens when a very dense file meets an output that can take only so many messages per second, or a queue that can hold only so much.

## Quick start
1. Press **Open MIDI...** and pick a MIDI file with your phone's file picker.
2. Choose an **Output**. **None** makes no sound; it is the safe choice for studying timing and the Analysis. **Built-in synth (Sonivox)** plays through the phone's speaker or headphones; any other entry is a USB MIDI device or a Bluetooth MIDI device that is connected.
3. Press **Play**. Press it again to pause.
4. Leave the **Rate model** on **None** (no artificial delay) until you want to try a slowdown or a gate.
5. Plugged in a device after the program started? Press **Rescan** next to the output list.

## Help topics
- **Overview** - this page.
- **File and output** - opening files, outputs, Track routing.
- **Rate model** - slowdowns and gates.
- **Queue and overflow** - queue limits and what is dropped.
- **Playback** - Play, Pause, Stop, seeking and related options.
- **Analysis** - the prediction graph.
- **Statistics** - the live numbers.
- **Channels** - the 16-channel view.

## The menu
The button with three bars at the top right opens the menu. It holds:
- Playback options: **Chase MIDI state**, **Preserve playback queue**, **Output-paced timeline**.
- On a phone with a single processor core only: **Let other programs run while playback is behind**.
- Queue options: **Apply queue limit without slowdown**, **Limit queue by waiting time**, **Scale note interval by note traffic**.
- **Synth settings...** - volume, voices, reverb and sound bank of the built-in synth (see **File and output**).
- **Help** - this help.

## Screen and look
- The text size and display size are the ones set in your phone's settings. There is no window frame to change and no application scale.
- Turn the phone sideways if you want more room; the player keeps playing.
- The transport (position bar, **Play**, **Stop** and the five-second buttons) stays at the bottom of the screen.

## Playing with the screen off
- While a song plays or is paused, the program shows a notification and keeps playing when the screen turns off or you switch to another app. Its **Stop** button stops the song.
- If you refuse the notification, the music still plays; only the notification is hidden. Android may stop a program with no notification sooner.
- When nothing plays, the program holds nothing.

## Good to know
- The program **remembers** the MIDI output, the switches in the menu, the **Synth settings** and the sound bank, and starts with them next time. It does not remember the MIDI file. These choices never change how the music is played.
- Tap a group's heading to fold it away; tap it again to open it.
- Number boxes: tap to type, or drag sideways on the box to change the value (the further you drag, the faster it goes). **Done**, closing the on-screen keypad or a tap outside the box applies the value.
- Live numbers are redrawn in step with your screen while a song plays or loads, less often when the phone is busy, otherwise only when something changes.
- The built-in synthesizer is Sonivox, Android's own, under the Apache License 2.0; the licence text is in the app.
- The full user guide is in the public repository.
)HELP";

const char kHelpOpenAndOutputAndroid[] = R"HELP(## Opening a file
- Press **Open MIDI...** and pick a **.mid**, **.midi** or RIFF/RMID **.rmi** file in the file picker; it shows folders and MIDI files. If your file does not appear, use **Open any file...**, or **Other app...** to pick it with another installed file manager. You can also open a MIDI file from another app with **Open with** or **Share**. No storage permission is needed: the program reads only the file you pick.
- Loading runs in the background with two progress bars. You can cancel at any time; no half-loaded song is left behind.
- A large file is inspected first and may show a memory warning. **Cancel** there prevents the risky load.
- A small, repairable running-status fault is repaired and the repair is announced afterwards. Other damaged files are refused with a plain message saying what is wrong.

## Output
- **None** - plays nothing audible. For testing timing and Analysis.
- **Built-in synth (Sonivox)** - the phone plays the notes itself, through its speaker or headphones, with Android's own plain general-purpose sound. The sound comes about 0.04 seconds after each note is sent. It is Sonivox, under the Apache License 2.0; the licence text is in the app.
- Every other entry is a USB MIDI device or a Bluetooth MIDI device that is connected to the phone (directly or through an adapter); a hardware synthesizer or sound module sounds better than the built-in synth.
- Devices come and go. Press **Rescan** to read the list again; the song and all options stay as they are. If the device you were using is gone, the output falls back to **None** and the status line says so.
- Messages are sent to the device at once, the moment they are due. Long System Exclusive messages are sent in pieces.

## Synth settings
For the built-in synth only: open the menu and choose **Synth settings...**. Changes are heard at once while playing and are remembered for the next time; **Reset to defaults** goes back to the starting values at any time.
- **Volume** - 0 to 200 %. Lower it if the **CLIP** light comes on. Above 100 % the sound may distort.
- **Voice limit** - how many notes can sound at once; the synth starts with 64. The slider goes to 1024; type a number in the field next to it for more. More voices need more processing; when the limit is reached, older notes are cut off.
- **Reverb** - on or off, the type of room and the amount.
- **Load DLS sound bank...** - pick a DLS file to use its instrument sounds. Instruments the bank does not have keep the built-in sounds. **Use built-in sounds** goes back. The banks you loaded last are listed under **Recently loaded DLS sound banks**: tap one to load it again.
- At the bottom: the level bar, the **CLIP** light (it lights for about a second whenever the sound is cut at full scale) and the synth's status line.

)HELP";

const char kHelpChannelsTouchAndroid[] = R"HELP(## With touch
- **Tap** a value to type it.
- **Long-press** a forced value to hand it back to Auto.
- **Long-press** a historical value to ask for one value taken from the song, sent in order with the rest of the output.
- **Tap the Ch cell** to mute or unmute a channel. Muting sends safety releases and filters the channel's later messages.
)HELP";

std::string replace_all(std::string text, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

// Replaces everything from the first "begin" up to (not including) the first "end" after it. Without "end", up to the end of the text.
std::string replace_between(const std::string& text, const char* begin, const char* end, const std::string& with)
{
    const size_t b = text.find(begin);
    if (b == std::string::npos) return text;
    const size_t e = end ? text.find(end, b) : std::string::npos;
    return text.substr(0, b) + with + (e == std::string::npos ? std::string() : text.substr(e));
}

const char kMenuButtonLabel[] = "\xE2\x89\xA1";   // U+2261, the label of the menu button

std::string windows_help_text(HelpTopic topic)
{
    switch (topic) {
    case HelpTopic::FileAndOutput: return std::string(kHelpFileAndOutput, sizeof kHelpFileAndOutput - 1);
    case HelpTopic::RateModel: return std::string(kHelpRateModel, sizeof kHelpRateModel - 1);
    case HelpTopic::QueueAndOverflow: return std::string(kHelpQueueAndOverflow, sizeof kHelpQueueAndOverflow - 1);
    case HelpTopic::Playback: return std::string(kHelpPlayback, sizeof kHelpPlayback - 1);
    case HelpTopic::Analysis: return std::string(kHelpAnalysis, sizeof kHelpAnalysis - 1);
    case HelpTopic::Statistics: return std::string(kHelpStatistics, sizeof kHelpStatistics - 1);
    case HelpTopic::Channels: return std::string(kHelpChannels, sizeof kHelpChannels - 1);
    case HelpTopic::Overview: break;
    }
    return std::string(kHelpOverview, sizeof kHelpOverview - 1);
}

std::string linux_help_text(HelpTopic topic)
{
    std::string text;
    switch (topic) {
    case HelpTopic::FileAndOutput:
        // The Output list and the Direct WinMM section are Windows-only; they are replaced by the ALSA wording.
        text = replace_between(windows_help_text(topic), "## Output", "## Track routing",
                               std::string(kHelpOutputLinux, sizeof kHelpOutputLinux - 1));
        break;
    case HelpTopic::Playback:
        text = replace_between(windows_help_text(topic), "## Direct WinMM driver access", nullptr, std::string());
        break;
    case HelpTopic::RateModel:
    case HelpTopic::QueueAndOverflow:
    case HelpTopic::Analysis:
    case HelpTopic::Statistics:
    case HelpTopic::Channels:
        text = windows_help_text(topic);
        break;
    case HelpTopic::Overview:
    default:   // an out-of-range topic gives the Overview, as on Windows
        text = std::string(kHelpOverviewLinux, sizeof kHelpOverviewLinux - 1);
        break;
    }
    text = replace_all(text, "at the right end of the Playback heading", "next to the output list");
    text = replace_all(text, "title-bar menu", "@MENU@ menu");
    return replace_all(text, "@MENU@", kMenuButtonLabel);
}

std::string android_help_text(HelpTopic topic)
{
    std::string text;
    switch (topic) {
    case HelpTopic::FileAndOutput:
        // Opening, Output and the Windows-only Direct WinMM section are replaced by the Android wording.
        text = replace_between(windows_help_text(topic), "## Opening a file", "## Track routing",
                               std::string(kHelpOpenAndOutputAndroid, sizeof kHelpOpenAndOutputAndroid - 1));
        break;
    case HelpTopic::Playback:
        text = replace_between(windows_help_text(topic), "## Direct WinMM driver access", nullptr, std::string());
        break;
    case HelpTopic::Channels:
        text = replace_between(windows_help_text(topic), "## With the mouse", nullptr,
                               std::string(kHelpChannelsTouchAndroid, sizeof kHelpChannelsTouchAndroid - 1));
        break;
    case HelpTopic::RateModel:
    case HelpTopic::QueueAndOverflow:
    case HelpTopic::Analysis:
    case HelpTopic::Statistics:
        text = windows_help_text(topic);
        break;
    case HelpTopic::Overview:
    default:   // an out-of-range topic gives the Overview, as on Windows
        text = std::string(kHelpOverviewAndroid, sizeof kHelpOverviewAndroid - 1);
        break;
    }
    text = replace_all(text, "is shown at the right end of the Playback heading", "is shown in the **Playback** cell of the Statistics");
    text = replace_all(text, "(title-bar menu)", "(menu)");
    text = replace_all(text, "in the title-bar menu", "in the menu");
    text = replace_all(text, "In the title-bar menu", "In the menu");
    text = replace_all(text, "The window's title-bar menu", "The Analysis page's menu");
    text = replace_all(text, "Drag the divider, zoom the graph, or click to inspect a time.", "Drag the divider, pinch to zoom the graph, or tap to inspect a time.");
    text = replace_all(text, "so the window stays usable", "so the page stays usable");
    text = replace_all(text, "the same names as the main window", "the same names as the main page");
    text = replace_all(text, "Channels window", "Channels page");
    text = replace_all(text, "Analysis window", "Analysis page");
    // Pages the Android player does not have yet: one line under the heading. Remove each when its page exists.
    const char kNotYet[] = "- **Not on Android yet:** this page is only in the desktop versions for now.\n";
    if (topic == HelpTopic::Analysis || topic == HelpTopic::Channels) {
        const size_t end = text.find('\n');
        if (end != std::string::npos) text.insert(end + 1, kNotYet);
    }
    text = replace_all(text, "## Track routing\n", std::string("## Track routing\n") + kNotYet);
    return text;
}

} // namespace

const char* help_topic_title(HelpTopic topic)
{
    switch (topic) {
    case HelpTopic::Overview: return "Overview";
    case HelpTopic::FileAndOutput: return "File and output";
    case HelpTopic::RateModel: return "Rate model";
    case HelpTopic::QueueAndOverflow: return "Queue and overflow";
    case HelpTopic::Playback: return "Playback";
    case HelpTopic::Analysis: return "Analysis";
    case HelpTopic::Statistics: return "Statistics";
    case HelpTopic::Channels: return "Channels";
    }
    return "Overview";
}

std::string help_text(HelpTopic topic, HelpPlatform platform)
{
    switch (platform) {
    case HelpPlatform::Linux: return linux_help_text(topic);
    case HelpPlatform::Android: return android_help_text(topic);
    case HelpPlatform::Windows: break;
    }
    return windows_help_text(topic);
}

std::string help_text(HelpTopic topic)
{
    return help_text(topic, kNativeHelpPlatform);
}

namespace {

// Splits one line into plain and bold runs. "**" toggles bold; a "**" that is never closed stays literal text.
std::vector<HelpRun> parse_runs(const std::string& line)
{
    std::vector<HelpRun> runs;
    std::string current;
    bool bold = false;
    size_t i = 0;
    // Find where the last unmatched "**" is, by counting the markers first.
    size_t markers = 0;
    for (size_t k = 0; k + 1 < line.size(); ++k)
        if (line[k] == '*' && line[k + 1] == '*') { ++markers; ++k; }
    const bool odd = (markers % 2) == 1;
    size_t seen = 0;
    auto flush = [&]() {
        if (current.empty()) return;
        if (!runs.empty() && runs.back().bold == bold) runs.back().text += current;
        else { HelpRun r; r.text = current; r.bold = bold; runs.push_back(r); }
        current.clear();
    };
    while (i < line.size()) {
        if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
            ++seen;
            if (odd && seen == markers) { current += "**"; i += 2; continue; }   // the unclosed marker is literal
            flush();
            bold = !bold;
            i += 2;
            continue;
        }
        current += line[i++];
    }
    flush();
    if (runs.empty()) { HelpRun r; runs.push_back(r); }
    return runs;
}

bool starts_with(const std::string& s, const char* prefix)
{
    size_t n = 0;
    while (prefix[n]) ++n;
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

} // namespace

std::vector<HelpBlock> parse_help_markup(const std::string& markup)
{
    std::vector<HelpBlock> blocks;
    size_t pos = 0;
    while (pos <= markup.size()) {
        size_t end = markup.find('\n', pos);
        if (end == std::string::npos) end = markup.size();
        std::string line = markup.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        HelpBlock block;
        std::string body;
        if (starts_with(line, "## ")) { block.kind = HelpBlockKind::Subheading; body = line.substr(3); }
        else if (starts_with(line, "### ")) { block.kind = HelpBlockKind::Subheading; body = line.substr(4); }
        else if (starts_with(line, "# ")) { block.kind = HelpBlockKind::Heading; body = line.substr(2); }
        else if (starts_with(line, "- ")) { block.kind = HelpBlockKind::Bullet; body = line.substr(2); }
        else {
            size_t d = 0;
            while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
            if (d > 0 && d <= 4 && d + 1 < line.size() && line[d] == '.' && line[d + 1] == ' ') {
                block.kind = HelpBlockKind::Step;
                block.number = std::stoi(line.substr(0, d));
                body = line.substr(d + 2);
            } else {
                block.kind = HelpBlockKind::Paragraph;
                body = line;
            }
        }
        block.runs = parse_runs(body);
        blocks.push_back(block);
    }
    return blocks;
}

} // namespace app
} // namespace midibn
