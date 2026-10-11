# User guide

## Loading a MIDI file

Use **Open MIDI…** or drag one existing `.mid`, `.midi`, or `.rmi` file anywhere onto the main window. `.rmi` supports a Standard MIDI File stored in a RIFF/RMID container. Unsupported files, folders, URLs, and multi-file drops leave the current song untouched.

Loading is asynchronous and cancellable. The header shows the filename, parser stage, two progress bars, elapsed time, and process-private committed memory. Cancelling or replacing a load retires the older generation so it cannot publish stale results.

Ordinary files enter the one-pass compact parser immediately. Files large enough to plausibly cross the memory-warning boundary are first inspected by a count-only SMF scanner. The loading header shows **Inspecting large MIDI** with monotonic overall/stage progress. Cancel stops either inspection or parsing.

Malformed MIDI remains rejected, with one narrow compatibility exception. If a channel message immediately after metadata omits its status byte, and the most recent channel status is the one unambiguous valid interpretation, the player repairs that message and discloses the number of nonstandard running-status repairs in the loaded-file summary and an information message.

If the exact scan projects substantial use, MIDIBottleneck Player shows the filename, exact number of MIDI events to process, estimated memory while the file is open, and estimated highest memory use while loading. **Continue** parses normally; **Cancel** leaves no file loaded. The displayed loading timer pauses while this decision is open, so thinking time is not counted as loading time. The figures are estimates, not guarantees that an allocation will succeed.

## Choosing an output

**WinMM** lists MIDI outputs reported by Windows. Choose the executable architecture that matches any native wrapper you use.

**KDMAPI** selects a compatible direct-output provider. A matching application-local provider is deliberate and takes priority; initialization failures are reported rather than silently falling back to another synthesizer.

**None** opens no native output. It is useful for separating parser/scheduler performance from driver and synthesizer behavior. Events still count as successfully dispatched for the simulator’s statistics.

Changing output during playback uses the same safe restart boundary as seeking: the old worker retires, the real provider is silenced and closed where applicable, queued work/statistics are reset as documented, and playback resumes from the same source position.

## Routing tracks to constrained hardware

With a MIDI file loaded and playback stopped, open the title-bar system menu and choose **Track routing…**. Routing is session-only and lists each MIDI track and channel combination. One MIDI track can therefore appear more than once when it uses multiple channels. The original source file and its stored events are never changed.

Choose the usable output channels, the maximum melodic destination count, the channel-10 percussion policy, and the maximum automatic tier:

- **Exact-safe only** merges only track/channel entries whose patch, dynamic state, and simultaneous note ownership are compatible.
- **GM-family** may combine related General MIDI instrument families and can retimbre an entry.
- **Aggressive** chooses representatives across the complete song. It balances musical importance, independent overlapping roles, state conflicts, distinct patches/families, and the cost of retimbring or omitting an entry. It may accept additional state, overlap, handoff, or percussion conflicts.

The table shows every MIDI track/channel entry, detected representative patch, event/note counts, proposed destination, tier, and warnings. In **Usable output channels**, bold channel numbers are the destinations actually used by the current compiled plan. Hover a Patch cell to see its 1-based Program number and canonical General MIDI instrument name; a non-GM or unknown bank is explicitly labeled as a reference rather than a promise about its sound.

Select a table row to choose the MIDI track and channel being edited. **Automatic suggestion** restores automatic routing for that row; choosing an output channel manually corrects its destination. A usable manual destination outside the initial ascending suggestion replaces an automatic destination when necessary and does not silently create an extra melodic destination. The complete preview groups entries by output channel and identifies the representative patch, manual assignments, retimbrings, same-key ambiguity, state conflicts, audible handoffs, and percussion exceptions.

The preview summary separately reports routed entries and output channels, additional entries that share a destination, retimbred entries, omitted entries and note starts, and generated destination Bank/Program messages. Its source-plus-generated total is the same routed message view used by Playback and Analysis; “merged” does not mean that notes disappeared. For large files, the loading inspection includes a separate routing-memory allowance for this compact map and planning workspace. It does not build a second copy of the MIDI just to preview a route, and cancelling the memory warning prevents the risky load.

When every possible merge would combine destructive same-note overlap and incompatible channel state, Aggressive may omit a lower-value track/channel entry rather than damage more important parts. An omitted row is highlighted and the complete preview reports its exact note-start count. It is a complete but lossy route—not an unresolved hard error—and becomes applicable only after the existing warning confirmation. Aggressive can also retain multiple important entries with the same patch on separate destinations when their overlapping roles make sharing unsafe; patch uniqueness is valuable but not absolute.

MIDI Bank and Program state belongs to a source channel even when a format-1 file stores that state in a different track from the notes. The routed view associates that state with the note-bearing track/channel entries it governs and excludes note-empty initialized channels from destination selection. Each output channel receives a separately compiled patch timeline: a meaningful later instrument passage can take over during a clean non-overlapping handoff, while simultaneous incompatible parts retain the deliberately selected representative sound. Redundant and losing Bank/Program messages are suppressed, so final instruments do not depend on source-track ordering or a burst of competing Program changes. Other incompatible channel-wide state remains visible as a routing conflict rather than being described as exactly preserved.

**Apply** stays unavailable while any MIDI track/channel entry is unresolved. GM-family, aggressive, omitted, and manual plans require the visible warning confirmation before Apply. Applying is atomic: a failed or unconfirmed plan leaves the current routing untouched. **Use original routing** clears an active stopped-session plan. The command is disabled while Playing or Paused.

The accepted route creates one immutable channel-rewritten view shared by Playback, Analysis, state chase, the Channels window, destination-channel mutes, forced overrides, note ownership, and statistics. Source track and event identity remain intact, channel-less system/SysEx traffic is unchanged, and file/output replacement clears the session plan.

## Starting in the middle of a file

The title-bar system menu checks **Chase MIDI state on Play/Seek** by default. Starting Play at a nonzero position or seeking restores the bank, program, ordinary controller values, pitch bend, channel pressure, and known RPN/NRPN parameter values that were effective immediately before that position. Bank selection is sent before Program, and parameter selectors/data remain in valid order.

Chase never replays notes, polyphonic key pressure, SysEx, meta data, system messages, or channel-mode silence commands. Events exactly at the selected position remain ordinary source events and are not sent twice. Disabled channels receive no state. A forced Channel Monitor value wins over a conflicting source value.

By default, Pause resets and silences the output, so state is restored once when Resume continues; nothing is resent while paused. The chase option is disabled while playback is active and can be unchecked while stopped. This does not change the Channel Monitor's deliberate one-attribute source chase.

The default-off, session-only **Preserve playback queue on Pause and Seek** system-menu option deliberately changes those two boundaries. Pause parks the same worker and freezes its logical clock, queue, pending musical-model state, counters, and sounding output until Resume. A preserving Seek lets the event already in service and every admitted event finish in order, then sends All Notes Off on all 16 channels before state chase and events from the selected target. The selected timeline moves immediately, while the output timeline continues to show what is actually leaving the retained queue. Repeated seeks retain each already queued boundary but replace target work that was not yet admitted. This experimental mode trades immediate seeking and guaranteed silence for lossless queue preservation; a large queue can make the new position take a long time to become audible, and sustain or receiver-held sound can remain until the queued boundary or a later Stop. Stop, file/output replacement, disposal, and output failure remain hard safety boundaries.

The separate default-off, session-only **Output-paced timeline** option makes source time wait at each due timestamp (including its complete same-time batch) while simulated service or a synchronous MIDI output call is being accepted. Future source timestamps therefore keep their original spacing after the output returns instead of becoming overdue and draining in a catch-up burst. It does not delay rejected work for later, and turning it off preserves the ordinary source-clock behavior. Native provider blocking remains unknowable in advance: live Playback pauses for the time the call actually takes, while Analysis models only deterministic simulated service and says so explicitly.

## Processing model

The title-bar menu (Alt+Space) has checked **Show Processing model** and **Show Statistics** commands. Uncheck either to reclaim that section's height; both can be hidden together. File/output and Playback stay visible. These choices last only until the application closes. Hiding a section does not turn off its model, reset statistics, or interrupt playback; current values appear when the section returns. The same commands work while loading, playing, paused, or stopped.

**Application scale** in the same menu offers 50%, 75%, 100%, 125%, 150%, and 200%. Each choice is calculated directly from each window's established 100% measurements at the current Windows DPI, rather than scaling the previous result. Returning to 100% therefore restores the exact ordinary layout and fonts. At 50% and 75%, the player uses strongly hinted Windows small-screen fonts for better readability. The setting lasts for the session and applies to the main, Analysis, and Channels windows. Analysis preserves its report/graph split; Channels preserves automatic fitting or manually resized window and column widths.

**Borderless main window** is a separate default-off session view. Its **Borderless style** submenu offers **Classic strip**, **Integrated header**, and **Auto-hide overlay**. Integrated header places the window menu beside “File and output” and the window buttons on that heading; Auto-hide overlay reveals the four controls while the pointer is inside the main window without moving the player layout. Every style keeps keyboard access, Alt+Space, native moving/snapping, taskbar and Alt+Tab identity. Invisible edges and corners can start native resizing even over ordinary player controls, without drawing a border. Human pointer feel is still part of prototype review. Turning the option off restores the exact native-frame bounds and state saved when the mode was entered; Analysis and Channels always keep their native frames.

Both compact and standard main-window layouts use their measured content height. Vertical resizing is fixed so extra height cannot turn into an unexplained blank area; horizontal resizing and width-only maximize behavior remain available.

**Rate model** starts at **None**, which sends eligible MIDI without a simulated service delay. Processing time, MIDI bitrate, Events/sec, Per-note interval, Delayed NoteOff, Note start rate gate, and Output rate gate each remember their own last value while another choice is active. The dropdown has two nonselectable headings: **Simulated slowdown** for the three ordinary service models, and **Bandwidth / note gating** for the gate models. The headings are labels rather than choices: mouse, keyboard, wheel, search, and programmatic navigation skip them. Choose an ordinary model to apply its service rate:

- **Processing time per event** assigns the configured time to every accepted dispatchable event.
- **Events per second** directly sets the service rate from 0 through 9,999,999 events/sec. Zero means unlimited/immediate service.
- **MIDI bitrate** derives service time from the message’s serialized byte count and selected bitrate.

With **None** selected, accepted events are dispatched as soon as their source time and output calls permit. A selected Processing time of exactly 0 µs also uses the effective-immediate scheduler path, but retains the ordinary model as the selected choice.

The Events/sec model carries fractional microseconds between events, so rates such as 3,000 events/sec remain exact over time rather than being rounded event by event. Switching models restores that model's last exact value rather than converting another model's value. A live rate or ordinary model change applies when the next event begins service. The event already in service keeps its assigned duration; the pending queue, sounding notes, transport, and statistics continue. A changed Events/sec setting starts a fresh fractional phase at that next service start.

The Events/sec slider gives its lower half to small, evenly spaced changes from 1 to 100 events/sec; its upper half covers higher rates. The numeric field keeps an exact typed value even when the slider can only point near it. Above one million events/sec, some individual modeled service times are zero microseconds and others are one; the shared fractional clock keeps the long-run rate exact.

The processing value and queue limit use flat scrub-or-type fields. Click to type, then press Enter or leave the field after a change; Escape cancels. Drag sideways to adjust without opening a dialog. A small movement or an unchanged click does not change a setting. In event-count mode, Queue limit changes by one event per four pixels, or one per eight with Shift. Waiting-time mode uses a wider scale suitable for microseconds. Up/Down also adjusts the focused value.

The processing value follows the selected slider's scale when dragged: fine control for small Events/sec values and larger steps toward the high end of each model. Hold Shift for one displayed unit per pixel. Dragging begins from the exact typed value; simply touching or reversing a drag does not round it to a slider position. The field's tooltip gives its current units and keyboard step. Both fields grow to fit the displayed number and return to its measured width when editing ends.

## Queue length and overflow

With **Queue length limit** off, the simulated application queue is unlimited. With it on, the configured occupancy includes the event in service plus pending events.

During ordinary finite-queue playback, the numeric event-count limit remains editable. Raising it affects the next admission immediately. Lowering it below current occupancy keeps the event in service and every already accepted pending event in order; new arrivals use the selected overflow policy against the lower limit until occupancy returns within it. Protected releases and safety traffic can still create documented soft-limit excess. The Queue limit checkbox, event-count/waiting-time mode, forward-only admission, Preserve Pause/Seek, and automatic state-chase choices remain locked while Playing or Paused because they change the queue or worker structure rather than one atomic admission value.

The title-bar system menu's **Apply queue limit without slowdown** option is off by default. When it is checked with a finite queue and an ordinary rate model selected, the dropdown heading changes to **Forward-only queue admission**. That rate drives a virtual queue, but accepted MIDI is sent immediately. The model may reject a newly arriving event; it never removes one already sent. The queue statistic above the pressure bar remains the real unsent scheduler/output backlog. The bar is labelled **Virtual** and shows separate modeled pressure.

Checking the option while **None** and a finite queue are selected visibly selects the last ordinary rate model; no hidden rate runs under None. Choosing **None** from active forward-only mode turns the option off. With Queue length limit off, the option is remembered but inactive and an ordinary selected rate still means simulated slowdown. None with a finite queue and the option off has no virtual rate limit, although real blocked-output backlog can still fill the finite scheduler queue.

Only **Drop newest** and **Drop incoming complete notes** are valid in forward-only mode. **Drop oldest**, **Drop oldest complete note**, and **Clear buffer and jump to realtime** require simulated slowdown: they can remove pending MIDI, but cannot retract MIDI already sent. The player keeps the selected policy and explains why Play is unavailable instead of silently changing it. The system-menu option can be changed only while playback is stopped; the choice applies to the next playback start. Switching between None, ordinary processing, and the Per-note gate while active uses a safe silence/restart when the processing structure changes. Switching directly among ordinary rate models keeps the live queue and sounding state.

Overflow choices include:

- **Drop newest** — reject the arriving event when full.
- **Drop oldest** — discard the oldest safe pending event under the existing event policy.
- **Clear buffer and jump to realtime** — silence output, clear pending work, and jump to the exact source-time boundary.
- **Drop incoming complete notes** — reject a NoteOn and later suppress its paired NoteOff, while preserving NoteOffs for notes that were actually sent and preserving non-note messages. Protected events can temporarily take occupancy above the configured soft ceiling.
- **Drop oldest complete note** — when full, remove the oldest queued NoteOn that has not begun service. If its matching NoteOff is already queued, remove that too; if it arrives later, suppress it then. Matching uses first-in, first-out occurrences of the same channel and pitch, including velocity-zero NoteOns as releases. An attack already in service or sent cannot be removed. This is different from **Drop oldest**, which removes one pending event without treating its whole note as a pair.

If no old unsent attack can be removed, this new policy rejects an arriving NoteOn or ordinary non-note message and counts it as dropped. It keeps NoteOffs and note-silencing controls (sustain off and channel-mode safety commands), even when that temporarily puts occupancy above the configured limit. This is a safety exception, not a strict hard cap. Each discarded attack and each discarded or later suppressed release counts separately. Changing the choice during playback affects later overflow decisions, but does not undo MIDI already sent or discarded under the previous choice.

Channel mutes and forced-attribute conflicts are filtered before queue admission. They are reported separately and do not consume simulated service or queue capacity.

The title-bar menu's default-off **Limit queue by waiting time (µs)** command changes Queue limit from an event count to a maximum pending age. Age is measured from the oldest eligible unsent event against the scheduler's logical MIDI timeline; an event exactly at the limit is allowed, and overflow begins only after it becomes older. The field remembers its event-count and microsecond values separately. The practical waiting-time range is 1 µs through 1,800,000,000 µs (30 minutes).

Waiting-time mode offers only policies that can reduce an overdue head: **Drop oldest**, **Drop oldest complete note**, and **Clear buffer and jump to realtime**. It falls back to Clear buffer when enabled from an incompatible event-count policy, then restores the remembered event-count policy when returning. Complete-note removal is a soft limit: it discards a note occurrence only when that occurrence is the overdue head. A protected release or safety message may remain older than the limit; an ordinary incoming event is then rejected rather than discarding unrelated later notes or retrying indefinitely.

Waiting-time mode and forward-only admission cannot be active together. Enabling either one turns the other off and restores the appropriate remembered units and policies. This is deliberate: forward-only output does not retain accepted events that a pending-age limit could later remove.

A blocked driver or synthesizer call can create a real backlog even when simulated slowdown is off. **Queue now / maximum** always counts current and maximum outstanding events, including the call currently in service. The same live measurement applies to None, ordinary service models, Per-note, and Delayed NoteOff even when queue limiting is unavailable. Muted, override-filtered, and coalesced messages are excluded. A deliberately future gate transition joins only when its logical deadline arrives. Waiting-time mode keeps age in the pressure readout rather than replacing event counts. Real backlog is separate from virtual pressure and can exceed a soft limit while required releases or safety messages remain pending.

Queue pressure is green below 65%, amber from 65% to below 90%, and red from 90% through 100%. Purple means required protected traffic has pushed the queue strictly above its configured soft limit. The numeric pressure readout receives space before the decorative bar, and its tooltip retains the complete value if a very narrow layout needs an abbreviated display.

### Per-note interval gate

Choose **Per-note interval gate** in the Rate model dropdown. The interval value and slider remain editable; the generic queue controls are temporarily unavailable without losing their saved choices. At zero, each distinct source timestamp is resolved immediately with no positive minimum interval. Choosing another model exits the gate through the same safe playback boundary used previously.

The interval applies independently to each MIDI pitch across all channels. Source notes remain distinct by track, channel, pitch, and FIFO occurrence; the constrained output remembers only whether that pitch is up or down and the channel on which its current NoteOn was actually sent. A later attack is therefore not suppressed merely because a longer note on another track or channel is still sustaining.

If a selected attack arrives while its pitch is down, the player sends the required release before a permitted replacement. With a positive interval, very short and zero-duration source notes still receive one complete interval. At zero, each distinct source timestamp is an immediate boundary; if one pitch needs both a release and a new attack there, they resolve in deterministic same-time phases without pretending that one microsecond elapsed. NoteOns at the same absolute MIDI tick are one simultaneous candidate: the strongest velocity is emitted, with source order breaking a tie, while every selected layer can support the resulting sustain. An attack that cannot be selected in the logical batch currently being resolved is discarded with its paired release rather than queued by the gate for another interval.

The output limit remains exactly one NoteOn, one NoteOff, or no transition for each pitch at each boundary. Non-note messages retain their ordinary source-time path.

The title-bar system menu provides a default-off, session-only **Scale note interval by note traffic** option. It can be changed whenever playback is stopped, regardless of the selected Rate model. None, the ordinary slowdown models, and other non-applicable models simply ignore the retained check; selecting Per-note interval gate or Delayed NoteOff later applies it. The selected duration is one transmitted note-transition interval: after a boundary emits `N` actual NoteOn/NoteOff messages, the next logical note batch waits `N` intervals (with a minimum multiplier of one). A chord can therefore still be sent as one burst and then be followed by proportional waiting; this is not a strict instantaneous events-per-second ceiling. Filtered, coalesced, losing, and non-note messages do not increase `N`, while synthesized Delayed retrigger releases and replacement attacks do.

Note work held only by the gate's own interval pacing is limited to the logical batch currently being resolved. Unselected attacks and their paired releases are discarded. Work already accepted but made late by scheduler delay, an unrelated stall, or a blocking MIDI provider is different: it remains retained and several overdue logical batches may drain close together after execution resumes. Analysis keeps the same deterministic logical timestamps; the option does not add a second wall-clock limiter.

### Interval polyphony limit

While Per-note interval gate or Delayed NoteOff is selected, the Queue length controls become a separate, default-off **Interval polyphony limit**. Its value is the hard maximum number of transmitted NoteOn and NoteOff messages in one logical interval, from 1 through 128. It does not describe the synthesizer's total sounding voices.

Required releases for attacks that were actually sent have priority over new attacks. Lower-priority attacks are discarded with their paired releases when needed. If required releases alone exceed the value, only those excess releases carry to the earliest later intervals; no attack, replacement, or unprotected release is carried. Each interval remains at or below the hard value. With note-traffic scaling enabled, only the messages actually emitted in that interval determine the next wait.

The **Events sent / dropped** statistic shows one dropped total. It combines queue overflow with note or rate-gate exclusions without counting the same source event twice; Analysis keeps the separate reasons available for diagnosis. Forced Override suppression remains a separate Channels statistic. Gate filtering covers simultaneous note combining and interval overload; it is not queue overflow. Changing the gate, its interval, output, or transport state uses the normal safe silence/restart boundary, so stale pitch state cannot survive Pause, Seek, Stop, or an output restart.

Very large numbers of simultaneously unmatched source notes are tracked in growable reusable segments. If the selected output channel for a layered pitch is disabled during playback, another already selected enabled layer can re-establish that pitch at the next interval boundary after the safety silence.

### Delayed NoteOff

Choose **Delayed NoteOff** to give each pitch one global owner across all tracks and MIDI channels. The value is both the minimum duration of each transmitted NoteOn and the earliest time that pitch may begin a retrigger. A matching release that arrives sooner is held until that time. A competing attack strictly before the threshold is coalesced with its paired release.

When an eligible replacement arrives while the old note remains held, the player sends the old owner's NoteOff, waits through a derived recovery interval, and then sends at most one live replacement. Recovery is one quarter of the selected value, capped at 40,000 µs; it is an experimental bounded interval rather than receiver calibration and has no separate control. During recovery, one candidate slot per pitch keeps the musically preferred replacement, so an eligible melody return is not discarded simply because the old note had to close first. This is bounded note ownership, not a queue of later attacks. Stronger attacks win, with deterministic source order resolving equal velocities. Candidates that have already ended are not emitted. Other pitches remain independent.

At a zero interval, Per-note interval gate and Delayed NoteOff add no positive-duration delay or time quantization. Intentional same-pitch ownership, occurrence pairing, and same-time transition resolution still apply; independent note occurrences retain their source timestamps.

At the threshold—or any time after it—a repeated attack remains eligible. A due release is sent first; if an older source occurrence is still held, the player inserts the needed NoteOff immediately before the new NoteOn so the retrigger stays distinct without stacking voices. A later release from the displaced older occurrence cannot release the newer voice. Same pitches on different MIDI channels remain independent, and a transmitted source release retains its NoteOff or velocity-zero NoteOn encoding. The **Events sent / coalesced** figures report only genuinely inside-threshold attacks and their paired releases, separately from Forced Override filtering and queue drops.

The ordinary Queue controls are unavailable in this standalone model, but they are repurposed as the separate Interval polyphony limit described above. Pause, Resume, Seek, Stop, output changes, model changes, and live delay edits use the established silence-and-restart boundary. Analysis runs the same occurrence matcher, adaptive boundary clock, interval cap, and logical scheduler as Playback.

### Note start rate gate

Choose **Note start rate gate** to limit velocity-positive NoteOn starts. Zero means unlimited. Exact fractional pacing is combined with a short, fixed batching tolerance so ordinary chords and passages below the selected rate remain unchanged. The exact rolling one-second count never exceeds the maximum, and quiet time cannot enlarge the fixed tolerance into saved burst credit. At 9,999,999, practical simultaneous chords remain unchanged. Releases, controllers, program/bank state, pressure, Pitch Bend, SysEx, system messages, and safety traffic do not consume this Note-start limit.

When simultaneous starts exceed the capacity matured at that instant, the shared musical ranking prefers chord-boundary voices, then strength, structural duration where known, and represented tracks or lanes with deterministic fairness. Percussion is treated as a represented lane rather than interpreting drum-key numbers as bass and melody. A rejected start's matching release is suppressed; admitted and unmatched releases pass. The statistics keep rejected starts and suppressed releases separate from queue drops, while Analysis uses the same logical-time decisions over the immutable source file.

### Output rate gate

Choose **Output rate gate** to limit ordinary MIDI messages that are actually transmitted. **Maximum output rate** is an exact rolling one-second ceiling from 0 through 9,999,999 events/sec; zero means unlimited. Exact fractional pacing plus a short, fixed batching tolerance preserves below-limit passages and ordinary same-time groups without saving quiet-period credit. At 9,999,999, practical same-time fixtures remain unchanged. This is a separate nonblocking admission model, not the Events/sec slowdown clock: rejected work is dropped at its source position and is never saved for later catch-up.

An admitted attack reserves ordinary capacity for its matching release. Controllers and source Bank/Program state consume ordinary capacity as well. Immediate Stop, Panic, output replacement, failure, timeout, and disposal cleanup may bypass the musical gate; that safety work is separate from ordinary gated output. Rare required generated state sent without capacity is reported as **required messages sent over target** rather than as dropped work.

Program/bank state, controllers and other essential state, safety messages, and releases for attacks that already reached output consume matured capacity before new attacks. If a required message must be sent without capacity, it is counted as a **required message sent over target** rather than as dropped work. Such excess neither rewrites older capacity nor creates pacing debt, so it cannot indefinitely postpone ordinary attacks. When capacity is insufficient, the same deterministic chord, velocity, lane/fairness, percussion, and source-order ranking selects complete unsent note occurrences. A rejected attack stays paired with and suppresses its later release; a release for an emitted attack is never discarded.

Playback advances its pacing decision from the actual time each synchronous output call completes. Analysis cannot predict a third-party provider stall, so it shows the deterministic ideal result for a nonblocking output using the same source-time selection rules. Playback and Analysis agree under deterministic nonblocking conditions; after unpredictable provider blocking, later live admissions may legitimately differ. Stop, Panic, file/output replacement, disposal, and output failure clear the gate rather than carrying pacing state into another session.

Analysis **events/sec** is a graph-interval average, not the exact rolling one-second counter used for admission. A short graph interval can therefore briefly display more than the configured maximum when the fixed 50 ms tolerance keeps a source batch intact, while direct dispatch timestamps still prove that no half-open rolling second exceeds the target. A compensating dip in admissions and an echo in rejected counts can appear exactly one second later when that earlier group expires from the ledger. This bounded rhythm is retained because removing the tolerance caused severe live underuse when ordinary Windows scheduling delivered due source work in small batches; it does not represent queued catch-up or saved quiet-period credit.

## Playback and statistics

Play, Stop, output switching, and file replacement use ordered worker boundaries so native sends do not overlap. Pause and Seek use the same safe reset boundary by default; the optional preserving mode uses the ordered behavior described above. A native call already executing cannot be forcibly interrupted; control takes effect as soon as it returns.

Statistics include source timeline/output frontier, current/maximum queue occupancy, theoretical or observed maximum rate, live rolling output rate, sent/dropped events, effective playback speed, and current/maximum lag.

In an immediate-service mode, **Maximum rate** is the highest observed rolling dispatch rate since the applicable statistics reset. It is workload-specific, not a theoretical hardware limit. **Reset stats** rebases counters and peaks without disrupting playback.

In Per-note mode with a positive interval, **Maximum rate** becomes the gate's frame frequency. One frame is one configured interval. Each of the 128 pitches can make at most one Note On or Note Off transition in a frame, while different pitches can transition together. Controllers and other non-note messages are not limited by this number, so it is not the player's total event-throughput ceiling. At zero, the readout says **Source-time boundaries** because dividing by a zero interval would be meaningless.

**Current lag** is the lateness of the most recently sent MIDI event compared with its original time in the file. **Maximum lag** is the largest such value since statistics were reset. In Per-note mode, lag includes deliberate waiting for an interval boundary as well as scheduler or output delay. Several transitions in one frame can come from different source times, so current lag may move up and down. It does not measure synthesizer rendering or audio-device latency.

**Effective speed** compares resolved source-timeline progress with real playback time over the selected measurement window. In Per-note mode, the frontier advances only after a gate frame and all of its output calls finish. It still advances through sparse or filtered passages that the gate has processed, but stops when output is genuinely blocked.

## Analysis

The Analysis window summarizes message types and workload, then graphs event/byte density and modeled queue pressure at an adaptive or fixed resolution. Time-axis labels use aligned human-readable intervals; hover and pinned inspection provide exact arbitrary positions.

The Processing value and Queue limit fields can be typed or adjusted by dragging horizontally. A short movement does nothing. Queue dragging is gentle at small limits and gets faster at larger limits; hold Shift for precise adjustment. The number remains centered and fully visible as the field grows or shrinks.

Queue Projection reports source duration, predicted accepted-output completion, and positive overrun after the source ends. This models the selected simulator service and queue policy. It does not predict provider buffering, synthesizer rendering, voice release tails, or audio-device latency.

When a gate Rate model is selected, Analysis instead shows its dedicated output projection. The blue graph is the original MIDI workload; the amber graph shows the messages the shared model would send. Per-note reports interval-boundary output and gate exclusions; Delayed NoteOff reports global per-pitch ownership, minimum-duration output, coalescing, and the derived capped recovery interval; Note start rate gate reports rejected starts and suppressed paired releases; Output rate gate additionally reports required messages sent over target. Ordinary queue-pressure figures are not shown because queue processing is disabled in these standalone modes. Live channel mutes are not included. Forced overrides are included only when that Analysis window's default-off **Project forced overrides** command is enabled. Output rate Analysis is explicitly the ideal nonblocking result; unpredictable live provider blocking can change later pacing decisions.

Analysis work is asynchronous, cancellable, cached by reusable file/resolution workload, and protected from stale results. On an initial calculation, the window shows a workload-only graph as soon as event/byte density, clusters, and message-type scanning finishes. It is labelled **Queue projection pending** or **Per-note gate projection pending**, as appropriate. Unfinished output, occupancy, overflow, and completion figures are omitted rather than shown as zero.

The final projection replaces that preview atomically. Cancelling keeps the last completed graph when one exists. If there is no completed graph yet, a published preview remains useful and is relabelled **Workload only — projection cancelled**.

The graph's optional Playback Statistics panel shortens long readouts before shortening their captions. Hover over it to see the complete values. When Analysis is the active window, Space uses the player's usual Play/Pause/Resume action regardless of pointer location and takes priority over a focused button. Active text editing, editable or open dropdowns, modal dialogs, and inactive Analysis windows keep their ordinary keyboard behavior.

Each Analysis window also has a default-off **Show event counts** command in its title-bar system menu. The ordinary view reports events/sec. Event-count view instead shows the integer source and modeled-output events in each accepted graph interval, names that interval resolution in the panel heading, and gives the exact interval start/end during hover or pinned inspection. Switching is immediate and reuses the current preview or completed result; it does not recalculate Analysis. Byte-rate, queue-pressure, drop, cluster, timeline, and whole-file average/peak rate figures remain unchanged. The choice belongs only to that Analysis window and is discarded when it closes.

Each Analysis window also has an independent default-off **Project forced overrides** command. Turning it on snapshots the current forced channel values after Track routing, recalculates only that window, and labels generated forced messages separately from source messages filtered because they conflict. Later override edits refresh enabled windows with a new snapshot; disabled Analysis windows remain unchanged. This is a read-only projection: it sends no MIDI, does not alter Playback, and is forgotten when the window closes. Override presets, requested-versus-forced comparison views, and broader override systems are not part of this option.

Each Analysis window has its own default-off **Always on top** command in its title-bar system menu. The choice affects only that Analysis instance and is discarded when it closes.

## MIDI Channel Monitor

**Channels…** opens a modeless 16-row monitor. Sent, filtering, polyphony, and output position come from actual dispatch decisions. If Play/Seek state chase is enabled, attributes without a confirmed output observation first show the latest indexed source-file value before the current position in gray. While playback is active or paused, opening the monitor also requests an ordered restoration of that indexed channel state. Each value becomes ordinary black only after its output send succeeds; a failed or pending send does not turn a guess into an observation. Reopening an already-visible monitor does not chase again. While stopped, the indexed values remain gray until actually sent. With the chase option off, opening Channels does not send an automatic chase.

The Channel Monitor has its own default-off **Always on top** title-bar command. It is independent of the main window and every Analysis window, and a reopened monitor starts off again.

It shows MIDI-observed held-key polyphony, peak polyphony, sent/filtered counts, bank, program, volume, expression, pan, sustain, pitch bend, channel aftertouch, and last output position. These are MIDI observations, not a synthesizer’s internal voice count. Program numbers stay visible. Melodic channels use General MIDI instrument reference names. Channel 10 instead combines its displayed Bank and Program to name recognized standard/common GM2, GS, and XG drum kits; an unrecognized combination appears as **Drum kit N**. Its tooltip identifies the bank and reminds you that device-specific drum mappings can differ.

Ordinary black attributes were observed at the output, including a confirmed ordered state restoration. Bold blue attributes are forced. Gray italic attributes are historical; a source-derived gray value describes the file and does not prove the output received it. Returning a just-sent forced value to Auto keeps that last wire value gray until source state is explicitly restored or later observed. Forced values take precedence, disabled channels receive no chase, and no unknown value is invented. The monitor-opening restoration does not count as a source event or change Sent, polyphony, or the last dispatched source position.

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

The title-bar system menu places **Help (F1)…** after **About**, then groups **Always on top**, **Borderless main window**, its three-choice **Borderless style** submenu, **Show Processing model**, **Show Statistics**, and **Application scale**. A later group contains checked-by-default **Chase MIDI state on Play/Seek** plus default-off **Preserve playback queue on Pause and Seek**, **Output-paced timeline**, **Apply queue limit without slowdown**, and **Limit queue by waiting time (µs)**. Structural choices are locked while playback is active. Per-note gating is selected in the visible Rate model dropdown.

Press **F1** while the main window has focus for a small offline guide. It opens at File/output, Rate model, Queue, Playback, Analysis, or Statistics according to the focused control; the system-menu command uses the same route. The window can stay open while playback continues and is reused when help is requested again. Choose another topic from its list at any time. This is application help, not a special Windows question-mark title-bar mode.
