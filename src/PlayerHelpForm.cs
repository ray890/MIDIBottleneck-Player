using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal enum PlayerHelpTopic
    {
        Overview,
        FileAndOutput,
        RateModel,
        QueueAndOverflow,
        Playback,
        Analysis,
        Statistics,
        Channels
    }

    // A small offline guide, not a decorative title-bar button. MainForm
    // selects the topic from the control that had focus when F1 was pressed.
    internal sealed class PlayerHelpForm : Form
    {
        private readonly ListBox _topics;
        private readonly TextBox _body;

        internal PlayerHelpForm()
        {
            ProductIcon.Apply(this);
            Text = ProductIdentity.Name + " Help";
            StartPosition = FormStartPosition.CenterParent;
            ShowInTaskbar = false;
            MinimizeBox = false;
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            ClientSize = new Size(640, 390);
            MinimumSize = new Size(420, 300);

            _topics = new ListBox { Dock = DockStyle.Fill, IntegralHeight = false,
                AccessibleName = "Help topics" };
            _topics.Items.AddRange(new object[] {
                "Overview", "File and output", "Rate model", "Queue and overflow",
                "Playback", "Analysis", "Statistics", "Channels" });
            _topics.SelectedIndexChanged += delegate { UpdateBody(); };

            _body = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true,
                ScrollBars = ScrollBars.Vertical, TabStop = true,
                AccessibleName = "Help for the selected topic" };
            Button close = new Button { Text = "Close", DialogResult = DialogResult.Cancel,
                AutoSize = true, Anchor = AnchorStyles.Right };
            close.Click += delegate { Close(); };

            TableLayoutPanel layout = new TableLayoutPanel { Dock = DockStyle.Fill,
                ColumnCount = 2, RowCount = 2, Padding = new Padding(10) };
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 155));
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.Controls.Add(_topics, 0, 0);
            layout.Controls.Add(_body, 1, 0);
            layout.Controls.Add(close, 1, 1);
            Controls.Add(layout);
            CancelButton = close;
            SelectTopic(PlayerHelpTopic.Overview);
        }

        internal PlayerHelpTopic SelectedTopic
        {
            get { return (PlayerHelpTopic)Math.Max(0, _topics.SelectedIndex); }
        }

        internal string VisibleHelpText { get { return _body.Text; } }

        internal void SelectTopic(PlayerHelpTopic topic)
        {
            int index = (int)topic;
            if (index < 0 || index >= _topics.Items.Count) index = 0;
            _topics.SelectedIndex = index;
            UpdateBody();
        }

        protected override void OnShown(EventArgs e)
        {
            Rectangle area = Screen.FromControl(this).WorkingArea;
            Size = new Size(Math.Min(Width, area.Width), Math.Min(Height, area.Height));
            base.OnShown(e);
        }

        private void UpdateBody()
        {
            _body.Text = TextForTopic(SelectedTopic);
            _body.SelectionStart = 0;
            _body.SelectionLength = 0;
        }

        internal static string TextForTopic(PlayerHelpTopic topic)
        {
            switch (topic)
            {
                case PlayerHelpTopic.FileAndOutput:
                    return "Open a .mid, .midi, or RIFF/RMID .rmi file, or drag one file anywhere onto the main window. " +
                        "You can cancel a load without leaving a partly loaded song. Large files may first be inspected and may show a memory warning. " +
                        "A narrowly repairable nonstandard running-status message is disclosed after loading; other malformed MIDI remains rejected.\r\n\r\n" +
                        "WinMM uses a Windows MIDI output. KDMAPI needs a compatible provider that you supply. " +
                        "None runs the player without audible MIDI output; it is useful for testing timing and Analysis.\r\n\r\n" +
                        "TRACK ROUTING\r\n" +
                        "With a file loaded and playback stopped, choose Track routing from the title-bar system menu. The planner shows every MIDI track and channel combination, so one MIDI track can appear more than once when it uses multiple channels. It shows the complete destination plan, representative patches, conflicts, retimbrings, percussion exceptions, and unresolved errors, and lets you correct the selected row's destination.\r\n" +
                        "• Aggressive routing chooses destination representatives across the whole song. It balances important parts, overlapping roles, state conflicts, and distinct instrument colors instead of filling channels with the first safe groups.\r\n" +
                        "• If merging a lower-value track/channel entry would be more damaging than leaving it out, Aggressive may mark that row Omitted. The table and full preview show the entry and its note-start count. This is a valid lossy plan, not an unresolved error, but Apply remains unavailable until you confirm the warning.\r\n" +
                        "• Apply is unavailable while any MIDI track/channel entry is genuinely unresolved. GM-family, aggressive, omitted, and manual plans require an explicit warning confirmation.\r\n" +
                        "• Bank and Program state follows its source MIDI channel even when stored in another track. State-only initialized channels do not become musical entries. Each output channel gets its own deliberate patch timeline: clean non-overlapping instrument handoffs are retained, while competing changes during real overlap yield to the selected representative.\r\n" +
                        "• The summary states how many MIDI track/channel entries were routed, shared an output channel, were retimbred or omitted, and how many destination Bank/Program messages were generated. These counts describe the same routed view used by Playback and Analysis.\r\n" +
                        "• Large-file inspection includes bounded routing workspace and retained-name allowances. It does not build a second MIDI copy merely to preview a route; Cancel at the memory warning prevents the risky load.\r\n" +
                        "• Routing is session-only. It creates an immutable output-channel view shared by Playback, Analysis, state chase, Channels, filtering, overrides, and statistics; the original MIDI data is unchanged.\r\n" +
                        "• Use original routing clears the stopped session plan. The command is unavailable while Playing or Paused.";
                case PlayerHelpTopic.RateModel:
                    return "NO ARTIFICIAL DELAY\r\n" +
                        "None sends eligible MIDI without adding a processing delay. It is the startup choice.\r\n\r\n" +
                        "SLOWDOWN MODELS — RETAIN WORK\r\n" +
                        "• Processing time per event, MIDI serial bitrate, and Events per second model slower service.\r\n" +
                        "• Retained work can build a queue. Zero processing time or zero Events/sec means immediate modeled service.\r\n" +
                        "• A live rate change applies when the next MIDI event begins service.\r\n\r\n" +
                        "NOTE-INTERVAL GATES — REDUCE NOTE TRAFFIC\r\n" +
                        "• Per-note interval gate permits one selected transition per pitch in a logical interval.\r\n" +
                        "• Delayed NoteOff gives each pitch one owner, a minimum duration, and a short recovery period after release. During that recovery it retains only one musically selected replacement for the pitch, so a melody retrigger is not lost merely because the old owner had to close first.\r\n" +
                        "• These gates drop unused attacks with their paired releases. Required releases for attacks already sent remain protected.\r\n\r\n" +
                        "NOTE START RATE GATE — LIMIT NOTEONS\r\n" +
                        "This gate counts velocity-positive NoteOn starts, not all MIDI traffic. It uses exact fractional pacing plus a short, fixed batching tolerance so ordinary chords and below-limit passages remain intact. An exact rolling second never exceeds the selected maximum, and quiet time cannot enlarge the fixed tolerance into saved burst credit. The maximum setting leaves practical simultaneous chords unchanged.\r\n" +
                        "• Simultaneous starts use the shared musical ranking, so prominent chord boundaries and fair lane representation are preferred.\r\n" +
                        "• A rejected start and its future paired release are both suppressed. Releases for admitted starts, state messages, and safety traffic remain protected.\r\n\r\n" +
                        "OUTPUT RATE GATE — LIMIT ALL TRANSMITTED MIDI\r\n" +
                        "Maximum output rate applies exact fractional pacing and an exact rolling one-second ceiling to ordinary transmitted MIDI messages. A short, fixed batching tolerance preserves ordinary below-limit timing and chords without becoming saved quiet-period credit. The maximum setting leaves practical same-time batches unchanged. This is separate from the Events per second slowdown model, and rejected work never waits in a catch-up queue.\r\n" +
                        "• An admitted attack reserves ordinary capacity for its release. Controllers and source state use ordinary capacity too. Immediate Stop, Panic, replacement, failure, and disposal cleanup may bypass the musical gate and is counted separately. Rare required generated state sent without capacity is reported as required messages sent over target.\r\n" +
                        "• When capacity is short, complete unsent note occurrences are rejected with the shared musical ranking; their paired releases remain suppressed.\r\n" +
                        "• Analysis shows the deterministic ideal result for a nonblocking output. Live decisions use the actual time each output call completes, so unpredictable provider blocking can legitimately change later admissions.\r\n\r\n" +
                        "GRAPH AND DROP RHYTHM\r\n" +
                        "Analysis events/sec is an average inside the selected graph interval, not the exact rolling-second measurement. A short interval can briefly read above Maximum output rate when the fixed batching tolerance preserves a group, even though every half-open rolling second remains within the target. The corresponding rejected count can echo one second later as that group expires from the exact ledger. Keeping this bounded rhythm avoids severe live underuse when Windows wakes the output worker in small batches.\r\n\r\n" +
                        "OPTION: SCALE NOTE INTERVAL BY NOTE TRAFFIC\r\n" +
                        "This option is off by default and can be prepared under any Rate model while playback is stopped. Only the two note-interval gates use it. If a logical batch sends N NoteOn/NoteOff messages, the next batch waits N selected intervals. A chord can still be one burst followed by proportional waiting, so this is not an instantaneous events/sec ceiling.\r\n\r\n" +
                        "INTERNAL AND EXTERNAL DELAY\r\n" +
                        "A note occurrence does not survive into another batch solely because interval pacing held it. Delayed NoteOff's one per-pitch replacement during its short recovery is bounded ownership state, not a queue; the other exception is an excess required release held by Interval polyphony. At a zero interval, neither note gate adds positive delay or time quantization. Scheduler delay or a blocking MIDI output is external: already accepted work remains retained and may be sent close together when the delay ends. Analysis shows the deterministic logical timing, not unpredictable device blocking.\r\n\r\n" +
                        "Switching to or from None or a gate safely silences and restarts playback at the same position.";
                case PlayerHelpTopic.QueueAndOverflow:
                    return "ORDINARY QUEUE LIMIT\r\n" +
                        "Queue length normally caps delayed work by event count. The numeric count can be edited during ordinary finite-queue playback: an increase applies immediately; a decrease does not retract the event being sent or silently purge accepted work. New arrivals use the selected limit.\r\n\r\n" +
                        "OVERFLOW CHOICES\r\n" +
                        "• Drop newest or Drop oldest removes individual pending work.\r\n" +
                        "• Complete-note choices keep a note start and its matching release together.\r\n" +
                        "• Drop background notes uses musical context to keep prominent chord voices and fair track/lane representation.\r\n" +
                        "• Required releases and safety work stay protected, so an ordinary queue limit can be exceeded softly.\r\n\r\n" +
                        "INTERVAL POLYPHONY\r\n" +
                        "With Per-note interval gate or Delayed NoteOff selected, the same controls become a separate Interval polyphony limit. It is off by default and allows 1–128 transmitted NoteOn/NoteOff messages in one logical interval. It is not the synthesizer's total sounding-voice limit.\r\n" +
                        "• Required releases take priority over new attacks.\r\n" +
                        "• If required releases alone fill the interval, only their excess carries to the earliest later intervals.\r\n" +
                        "• No interval exceeds the hard value; new attacks are dropped rather than delayed into another interval.\r\n\r\n" +
                        "WAITING-TIME AND FORWARD-ONLY MODES\r\n" +
                        "Limit queue by waiting time measures the oldest pending event in microseconds. Apply queue limit without slowdown rejects future arrivals without delaying accepted output. Each mode shows only overflow choices that can work safely.\r\n\r\n" +
                        "Queue now / maximum reports outstanding work. Green, amber, and red show increasing pressure; purple marks protected release or safety traffic beyond an ordinary soft limit.";
                case PlayerHelpTopic.Playback:
                    return "Play starts the loaded MIDI. The same button pauses and resumes. Stop silences the output and clears pending work. " +
                        "Use the timeline or five-second buttons to seek.\r\n\r\n" +
                        "Chase MIDI state on Play/Seek is on by default in the title-bar menu. It restores earlier bank, program, controller, bend, " +
                        "and pressure settings at a nonzero starting point, but does not replay earlier notes. " +
                        "Pause and Seek normally use a safe reset boundary. The default-off Preserve playback queue on Pause and Seek command instead parks the same worker on Pause; " +
                        "Seek drains admitted work, then sends All Notes Off before target chase and target events. It can take a long time and does not guarantee immediate silence.\r\n\r\n" +
                        "The default-off Output-paced timeline command pauses source progression while deterministic service or a synchronous output call is being accepted. " +
                        "Later source timestamps keep their spacing instead of catching up in a burst. Analysis models deterministic service but cannot predict native-provider blocking.";

                case PlayerHelpTopic.Analysis:
                    return "Analysis examines the MIDI source without sending it to an output. It graphs source workload and, for ordinary rate models, " +
                        "projects queue pressure, overflow, and output completion. With a note or output gate selected, it instead projects " +
                        "the gate's selected MIDI messages at their logical times. Output rate gate is an ideal nonblocking projection; live provider blocking can change later decisions. " +
                        "When Output-paced timeline is on, ordinary slowdown projection pauses source progression during deterministic simulated service, but it cannot predict third-party provider blocking.\r\n\r\n" +
                        "The first workload preview may appear before the slower queue projection is ready. Analysis does not predict driver or synthesizer delay or live channel mutes. " +
                        "This window's default-off Project forced overrides command snapshots the current forced values, applies them after Track routing, and reports generated forced messages separately from filtered competing source messages. It affects only this Analysis window and never sends MIDI or changes Playback.\r\n\r\n" +
                        "Drag the divider, zoom the graph, or click to inspect a time. Seek to pin can move the player to the selected point. " +
                        "This Analysis window's title-bar menu can show integer event counts per accepted graph interval instead of events/sec; the switch redraws the current result without recalculating it.";
                case PlayerHelpTopic.Statistics:
                    return "Timeline / output shows the player's current source position and the last dispatched output position. " +
                        "Queue now / maximum measures unsent scheduler/output work, including a blocked output call.\r\n\r\n" +
                        "Events sent excludes muted and override-filtered source events. Queue drops and gate exclusions are combined in the visible dropped total while diagnostics retain separate reasons. " +
                        "Output rate gate reports required safety/state messages sent over its configured target separately, not as dropped work. " +
                        "Output rate is a recent dispatch rate. Effective speed compares resolved source progress with real time. " +
                        "Lag compares a sent event's time with its original MIDI time; it does not measure audio-device latency.\r\n\r\n" +
                        "Reset stats clears counters without changing playback.";
                case PlayerHelpTopic.Channels:
                    return "Open Channels from Analysis to inspect 16 MIDI channels. Ordinary values are those the player observed sending. " +
                        "Blue bold values are forced overrides. Gray italic values are historical source or former-output information, not proof of current sound.\r\n\r\n" +
                        "Program numbers remain visible. Melodic channels use General MIDI instrument references. Channel 10 instead uses the displayed Bank and Program to name recognized GM2, GS, and XG drum kits; an unknown combination is shown as a numbered drum kit. Device-specific mappings can differ.\r\n\r\n" +
                        "Click a value to type or drag it. Right-click a forced value to release it to Auto; right-click a historical value " +
                        "to request one source-derived value through the ordered output path. Click Ch to mute or unmute that channel. " +
                        "Muting sends safety releases and filters later source channel messages.";
                default:
                    return "MIDIBottleneck Player plays MIDI and shows how dense files interact with output rates and queues.\r\n\r\n" +
                        "Open or drop a MIDI file, choose an output, and press Play. None output lets you inspect the model without sound. " +
                        "Rate model starts at None; choose a simulated slowdown or Per-note gate only when you want it.\r\n\r\n" +
                        "The title-bar menu can hide Processing or Statistics, scale the application windows from 50% through 200%, or switch the main window to a borderless mode. " +
                        "Borderless style offers a Classic strip, controls integrated into the File and output heading, or an Auto-hide overlay that never shifts the player layout. Every style keeps taskbar identity, native moving/snapping, keyboard-accessible window-menu/minimize/maximize/close controls, and Alt+Space. Invisible edges and corners route pointer drags through ordinary player controls into native resizing without drawing a border; human pointer feel remains part of prototype review. Turning borderless mode off restores the prior native-frame bounds and state. These view choices last for the session and do not change playback. Analysis and Channels keep their native frames.\r\n\r\n" +
                        "Press F1 while a main-window control is focused for its topic. The title-bar menu also opens this guide. " +
                        "The full user guide is available in the public repository.";
            }
        }
    }
}
