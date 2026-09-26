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
                    return "Open a .mid or .midi file, or drag one file anywhere onto the main window. " +
                        "You can cancel a load without leaving a partly loaded song. Large files may first be inspected and may show a memory warning.\r\n\r\n" +
                        "WinMM uses a Windows MIDI output. KDMAPI needs a compatible provider that you supply. " +
                        "None runs the player without audible MIDI output; it is useful for testing timing and Analysis.";
                case PlayerHelpTopic.RateModel:
                    return "None sends eligible MIDI without an artificial processing delay. It is the startup choice.\r\n\r\n" +
                        "Under Simulated slowdown, choose Processing time per event, MIDI serial bitrate, or Events per second. " +
                        "The number and slider below control the chosen model. Zero processing time or zero Events/sec means immediate modeled service. " +
                        "A live ordinary rate change takes effect when the next MIDI event begins service.\r\n\r\n" +
                        "Per-note interval gate limits each pitch to one note change per interval. At zero, distinct source timestamps resolve immediately without a positive minimum interval. " +
                        "Switching to or from None or the gate safely silences and restarts playback at the same position.";
                case PlayerHelpTopic.QueueAndOverflow:
                    return "Queue length limit normally caps delayed queue pressure by event count. Some note releases and safety messages are protected, so the limit can be soft. " +
                        "Drop newest rejects an arriving event. Drop oldest removes pending work. Complete-note choices keep matching note starts and releases together.\r\n\r\n" +
                        "Limit queue by waiting time is a title-bar menu choice. It changes Queue limit to microseconds and measures the oldest pending event against logical MIDI time. " +
                        "Drop oldest, Drop oldest complete note, and Clear buffer can be used; policies that cannot repair an overdue head are hidden.\r\n\r\n" +
                        "Apply queue limit without slowdown is a title-bar menu choice. With a finite queue and an ordinary rate model, " +
                        "it models pressure and rejects future arrivals without delaying accepted output. Its heading changes to Forward-only queue admission. " +
                        "Only Drop newest and Drop incoming complete notes work there; already-sent MIDI cannot be withdrawn. It cannot be combined with waiting-time mode.\r\n\r\n" +
                        "Queue now reports real unsent backlog. The Virtual pressure bar, when shown, is a separate model.";
                case PlayerHelpTopic.Playback:
                    return "Play starts the loaded MIDI. The same button pauses and resumes. Stop silences the output and clears pending work. " +
                        "Use the timeline or five-second buttons to seek.\r\n\r\n" +
                        "Chase MIDI state on Play/Seek is on by default in the title-bar menu. It restores earlier bank, program, controller, bend, " +
                        "and pressure settings at a nonzero starting point, but does not replay earlier notes. " +
                        "A seek or a structural processing change uses a safe reset boundary.";
                case PlayerHelpTopic.Analysis:
                    return "Analysis examines the MIDI source without sending it to an output. It graphs source workload and, for ordinary rate models, " +
                        "projects queue pressure, overflow, and output completion. With Per-note interval gate selected, it instead projects " +
                        "the gate's selected note messages at their logical times.\r\n\r\n" +
                        "The first workload preview may appear before the slower queue projection is ready. Analysis does not predict driver or synthesizer delay, " +
                        "and it does not include live channel mutes or forced overrides.\r\n\r\n" +
                        "Drag the divider, zoom the graph, or click to inspect a time. Seek to pin can move the player to the selected point.";
                case PlayerHelpTopic.Statistics:
                    return "Timeline / output shows the player's current source position and the last dispatched output position. " +
                        "Queue now / maximum measures unsent scheduler/output work, including a blocked output call.\r\n\r\n" +
                        "Events sent excludes muted and override-filtered source events. Queue drops and Per-note gate filtering are labelled separately. " +
                        "Output rate is a recent dispatch rate. Effective speed compares resolved source progress with real time. " +
                        "Lag compares a sent event's time with its original MIDI time; it does not measure audio-device latency.\r\n\r\n" +
                        "Reset stats clears counters without changing playback.";
                case PlayerHelpTopic.Channels:
                    return "Open Channels from Analysis to inspect 16 MIDI channels. Ordinary values are those the player observed sending. " +
                        "Blue bold values are forced overrides. Gray italic values are historical source or former-output information, not proof of current sound.\r\n\r\n" +
                        "Click a value to type or drag it. Right-click a forced value to release it to Auto; right-click a historical value " +
                        "to request one source-derived value through the ordered output path. Click Ch to mute or unmute that channel. " +
                        "Muting sends safety releases and filters later source channel messages.";
                default:
                    return "MIDIBottleneck Player plays MIDI and shows how dense files interact with output rates and queues.\r\n\r\n" +
                        "Open or drop a MIDI file, choose an output, and press Play. None output lets you inspect the model without sound. " +
                        "Rate model starts at None; choose a simulated slowdown or Per-note gate only when you want it.\r\n\r\n" +
                        "The title-bar menu can hide Processing or Statistics and can scale the application windows from 50% through 200%. " +
                        "These view choices last for the session and do not change playback. Analysis and Channels keep their normal Windows size.\r\n\r\n" +
                        "Press F1 while a main-window control is focused for its topic. The title-bar menu also opens this guide. " +
                        "The full user guide is available in the public repository.";
            }
        }
    }
}
