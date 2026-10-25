// Scripted interaction flows against v1's real ChannelMonitorForm (right-click behaviours,
// the in-place editor, request flow, completion and error feedback), and the --render mode.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static partial class ChannelsUiReference
    {
        private static int Bit(ChannelAttribute attribute) { return 1 << (int)attribute; }

        // The flow state: channel 1 has a forced Volume and a historical Program, channel 2 is
        // disabled with a historical Pan, channel 3 has unknown values that the source readouts fill,
        // channel 4 a pending forced Sustain and a forced Pitch bend, channel 10 a drum kit.
        internal static State FlowState()
        {
            MidiChannelSnapshot[] ch = ChannelPlaybackSnapshot.Empty().Channels;
            ch[0].Volume = 90; ch[0].ForcedAttributeMask |= Bit(ChannelAttribute.Volume); ch[0].ForcedVolume = 100;
            ch[0].Program = 4; ch[0].HistoricalAttributeMask |= Bit(ChannelAttribute.Program);
            ch[0].Pan = 64; ch[0].Sustain = 1; ch[0].PitchBend = 0; ch[0].BankMsb = 0; ch[0].BankLsb = 0;
            ch[0].KeysDown = 3; ch[0].PeakKeysDown = 7; ch[0].SentEvents = 123456; ch[0].LastDispatchedMicroseconds = 61234567;
            ch[1].Enabled = false; ch[1].MutedFilteredEvents = 1234; ch[1].Pan = 10;
            ch[1].HistoricalAttributeMask |= Bit(ChannelAttribute.Pan);
            ch[3].ForcedAttributeMask |= Bit(ChannelAttribute.Sustain) | Bit(ChannelAttribute.PitchBend);
            ch[3].PendingForcedAttributeMask |= Bit(ChannelAttribute.Sustain);
            ch[3].ForcedSustain = 1; ch[3].ForcedPitchBend = -100; ch[3].PitchBend = 20; ch[3].Sustain = 0;
            ch[9].BankMsb = 120; ch[9].BankLsb = 0; ch[9].Program = 8; ch[9].Volume = 80;
            MidiChannelSnapshot[] readouts = new MidiChannelSnapshot[16];
            for (int i = 0; i < 16; i++) readouts[i] = ChannelPlaybackSnapshot.Unknown(i);
            readouts[2].Volume = 77; readouts[2].Expression = 5; readouts[2].Program = 3; readouts[2].PitchBend = -42;
            readouts[2].BankMsb = 0; readouts[2].BankLsb = 0;
            readouts[1].Pan = 99;
            return new State { Name = "flow", Channels = ch, Readouts = readouts };
        }

        // Column numbers: 0 Ch, 1 Polyphony, 2 Peak, 3 Sent, 4 Dropped, 5 Override filtered, 6 Bank MSB, 7 Bank LSB,
        // 8 Program, 9 Volume, 10 Expr, 11 Pan, 12 Sustain, 13 Pitch bend, 14 Aftertouch, 15 Position.
        private static readonly Dictionary<string, string[]> Scenarios = BuildScenarios();

        private static Dictionary<string, string[]> BuildScenarios()
        {
            Dictionary<string, string[]> s = new Dictionary<string, string[]>();
            s["seeds-without-snapshot"] = new string[] {
                "click L 0 9", "hide commit", "click L 0 10", "hide abandon", "click L 0 11", "hide abandon",
                "click L 0 13", "hide abandon", "click L 0 8", "hide abandon", "click L 0 12", "hide abandon", "click R 0 9", "click L 0 0" };
            s["right-click-forced-auto"] = new string[] {
                "state 0", "click R 0 9", "cfg override fail:The output is not open.", "click R 0 9", "click R 3 13", "cfg override ok", "click R 3 12" };
            s["right-click-historical-chase"] = new string[] {
                "state 0", "click R 0 8", "cfg chase fail:No source value here.", "click R 0 8",
                "cfg chase throw:Engine busy.", "click R 0 8", "cfg chase ok", "click R 2 9", "click R 2 13", "click R 2 14" };
            s["right-click-nothing"] = new string[] {
                "state 0", "click R 0 10", "click R 0 14", "click R 0 1", "click R 0 0", "click R 5 8", "click R 9 8" };
            s["right-click-disabled-channel"] = new string[] {
                "state 0", "click R 1 11", "click R 1 9", "click L 1 11", "hide abandon" };
            s["mute-toggle"] = new string[] {
                "state 0", "click L 0 0", "click L 1 0", "cfg enabled fail:Cannot change the channel.", "click L 4 0", "cfg enabled ok",
                "click R 0 0", "click L 0 1", "click L 0 15" };
            s["editor-scrub-volume"] = new string[] {
                "state 0", "click L 0 9", "scrub 8 0", "scrub 7 0", "scrub 1 0", "scrub -40 1", "scrub 100000 0", "scrub 5 0", "scrub 1 0",
                "cfg override fail:Output failed.", "scrub 8 0", "cfg override ok", "scrub 16 1", "hide commit" };
            s["editor-scrub-bend"] = new string[] {
                "state 0", "click L 3 13", "scrub 1 0", "scrub 1 1", "scrub -10000 0", "scrub 20000 0", "scrub -1 1", "scrub 0 0", "hide commit" };
            s["editor-scrub-sustain"] = new string[] {
                "state 0", "click L 3 12", "scrub 1 0", "scrub -1 0", "scrub 0 0", "scrub 3 1", "scrub -3 1", "hide commit",
                "click L 0 12", "scrub 5 0", "scrub 5 0", "scrub -5 0" };
            s["editor-scrub-program"] = new string[] {
                "state 0", "click L 0 8", "scrub 8 0", "scrub 8 0", "scrub -3000 0", "scrub 4 0", "scrub 4 0", "scrub 4 1", "hide commit" };
            s["editor-scrub-unknown-source"] = new string[] {
                "state 0", "click L 2 9", "scrub 8 0", "hide commit", "click L 2 13", "scrub 1 0", "hide commit",
                "click L 5 9", "scrub -1 0", "scrub -8 0", "hide commit" };
            s["editor-click-typing-range"] = new string[] {
                "state 0", "click L 0 8", "up", "commit 0", "typing", "commit 129", "typing", "commit 128", "typing", "commit abc",
                "typing", "commit 1", "typing", "commit  5 ", "typing", "commit +7", "typing", "commit -1", "typing", "commit 1,2",
                "typing", "commit ", "typing", "commit 99999999999", "hide commit" };
            s["editor-typing-sustain"] = new string[] {
                "state 0", "click L 3 12", "up", "commit on", "typing", "commit OFF", "typing", "commit 2", "typing", "commit 1",
                "typing", "commit  oN ", "typing", "commit off", "typing", "arrow up", "commit", "typing", "arrow down", "commit", "hide commit" };
            s["editor-typing-bend"] = new string[] {
                "state 0", "click L 3 13", "up", "commit -8193", "typing", "commit 8192", "typing", "commit -8192", "typing", "commit 8191",
                "typing", "commit +5", "typing", "commit -0", "typing", "commit 0", "typing", "arrow up", "arrow up", "commit", "hide commit" };
            s["editor-typing-bank-volume"] = new string[] {
                "state 0", "click L 0 6", "up", "commit 127", "typing", "commit 128", "typing", "commit -1", "typing", "arrow down", "arrow down", "commit",
                "hide commit", "click L 0 9", "up", "arrow up", "arrow up", "commit", "typing", "escape", "typing", "arrow down", "escape" };
            s["editor-focus-loss"] = new string[] {
                "state 0", "click L 0 9", "up", "focusloss 100", "click L 0 9", "up", "focusloss 55", "click L 0 9", "up", "focusloss zzz",
                "click L 0 10", "up", "focusloss 127", "click L 0 10", "up", "hide commit", "click L 0 10", "up", "arrow up", "hide commit",
                "click L 0 10", "up", "arrow up", "hide abandon" };
            s["editor-escape-and-hide"] = new string[] {
                "state 0", "click L 0 9", "up", "escape", "click L 0 9", "up", "arrow up", "escape", "click L 0 9", "up", "arrow up", "hide abandon",
                "click L 0 9", "up", "arrow up", "click L 0 0" };
            s["editor-right-click-forced"] = new string[] {
                "state 0", "click L 0 9", "edright", "click L 0 9", "cfg override fail:Cannot release.", "edright", "cfg override ok", "edright" };
            s["editor-right-click-historical"] = new string[] {
                "state 0", "click L 0 8", "edright", "click L 0 8", "cfg chase fail:No source value.", "edright", "cfg chase throw:Not playing.",
                "edright", "cfg chase ok", "click L 1 11", "edright", "click L 2 9", "edright", "click L 0 10", "edright" };
            s["editor-request-failure"] = new string[] {
                "state 0", "click L 0 9", "cfg override fail:The output is not open.", "scrub 8 0", "scrub 8 0", "cfg override ok", "scrub 8 0",
                "up", "typing", "commit 5", "cfg override fail:Typed failure.", "typing", "commit 6", "cfg override ok", "hide commit" };
            s["control-error"] = new string[] {
                "state 0", "controlerror 5 2 Program could not be sent.", "controlerror 0 7 Pitch bend failed.", "controlerror 16 0 ignored",
                "controlerror -1 0 ignored", "controlerror 3 8 ", "controlerror 2 0 First", "state 0" };
            s["complete-chase"] = new string[] {
                "state 0", "completechase 0 2 boom", "completechase 0 2 -", "completechase 7 3 Not available.", "state 0" };
            s["detach-attach"] = new string[] {
                "state 0", "detach Loading new MIDI…", "state 0", "click L 0 9", "attach next.mid", "state 0", "click R 0 8",
                "detach MIDI loading cancelled. No MIDI file is loaded.", "attach again.mid", "click R 0 8", "state 1", "click R 3 9" };
            s["state-changes-with-editor-open"] = new string[] {
                "state 0", "click L 0 9", "state 1", "scrub 8 0", "state 0", "up", "state 1", "typing", "commit 50", "hide commit", "state 0" };
            return s;
        }

        private static List<State> _flowStates;

        private sealed class Sim
        {
            internal ChannelMonitorForm Form;
            internal readonly List<string> Log = new List<string>();
            internal string OverrideMode = "ok", ChaseMode = "ok", EnabledMode = "ok";
            internal ChannelPlaybackSnapshot Current;

            private static bool Failure(string mode, out string message)
            {
                message = null;
                if (mode.StartsWith("fail:", StringComparison.Ordinal)) { message = mode.Substring(5); return true; }
                if (mode.StartsWith("throw:", StringComparison.Ordinal)) { message = mode.Substring(6); return true; }
                return false;
            }

            internal Sim(ChannelMonitorForm form)
            {
                Form = form;
                form.OverrideRequested += delegate(object sender, ChannelOverrideRequestEventArgs e)
                {
                    Log.Add("r\tev\toverride\t" + I(e.Channel) + "\t" + I((int)e.Attribute) + "\t" +
                        (e.Value == ChannelOverrideState.AutoValue ? "auto" : I(e.Value)));
                    string message;
                    if (Current != null) Form.UpdateSnapshot(Current);
                    if (Failure(OverrideMode, out message)) e.Error = new Exception(message);
                };
                form.HistoricalChaseRequested += delegate(object sender, ChannelChaseRequestEventArgs e)
                {
                    Log.Add("r\tev\tchase\t" + I(e.Channel) + "\t" + I((int)e.Attribute));
                    string message;
                    if (!Failure(ChaseMode, out message)) { e.Complete(null); return; }
                    Exception ex = new Exception(message);
                    if (ChaseMode.StartsWith("throw:", StringComparison.Ordinal)) { e.Error = ex; e.Complete(ex); }
                    else e.Complete(ex);
                };
                form.ChannelEnabledRequested += delegate(object sender, ChannelEnabledRequestEventArgs e)
                {
                    Log.Add("r\tev\tenable\t" + I(e.Channel) + "\t" + (e.Enabled ? "1" : "0"));
                    string message;
                    if (Failure(EnabledMode, out message)) { e.Error = new Exception(message); return; }
                    if (Current != null) Form.UpdateSnapshot(Current);
                };
            }
        }

        private static void WriteFlows()
        {
            _flowStates = new List<State>();
            _flowStates.Add(FlowState());
            _flowStates.Add(BuildStates()[1]);
            using (Table inputs = new Table("flow-inputs.txt"))
            {
                inputs.Comment("The two states the flows use (state 0 = F0, state 1 = F1); same layout as inputs.txt.");
                DumpInputs(inputs, "F0", _flowStates[0]);
                DumpInputs(inputs, "F1", _flowStates[1]);
            }
            using (Table t = new Table("flows.txt"))
            {
                t.Comment("scenario <name> | op <text> followed by its result lines r <kind> ... ; states are in flow-inputs.txt");
                List<string> names = new List<string>(Scenarios.Keys);
                names.Sort(StringComparer.Ordinal);
                foreach (string name in names)
                {
                    t.Row("scenario", name);
                    ChannelMonitorForm form = NewForm("flow.mid");
                    Sim sim = new Sim(form);
                    foreach (string op in Scenarios[name])
                    {
                        t.Row("op", op);
                        sim.Log.Clear();
                        int tipRow = -1, tipCol = -1;
                        Execute(sim, op, ref tipRow, ref tipCol);
                        foreach (string line in sim.Log)
                        {
                            string[] parts = line.Split('\t');
                            t.Row(parts);
                        }
                        ScrubOrTypeTextBox ed = form.EditorForTesting;
                        t.Row("r", "ed", B(ed.Visible), I(Field<int>(form, "_editorChannel")), I((int)Field<ChannelAttribute>(form, "_editorAttribute")),
                            ed.Visible ? ed.Text : "-", I(ed.CurrentValue), B(ed.IsForced), B(ed.IsTyping), B(ed.GestureArmedForTesting),
                            ed.LastError, FontCode(ed.Font.Style) + ColorCode(ed.ForeColor));
                        if (tipRow >= 0 && tipRow < 16 && tipCol >= 0)
                            t.Row("r", "tip", I(tipRow), I(tipCol), Pool(form.GridForTesting.Rows[tipRow].Cells[tipCol].ToolTipText));
                    }
                    form.Dispose();
                }
            }
        }

        private static void Execute(Sim sim, string op, ref int tipRow, ref int tipCol)
        {
            ChannelMonitorForm form = sim.Form;
            DataGridView grid = form.GridForTesting;
            ScrubOrTypeTextBox ed = form.EditorForTesting;
            string[] p = op.Split(' ');
            switch (p[0])
            {
                case "state":
                {
                    State state = _flowStates[Int32.Parse(p[1], Inv)];
                    sim.Current = new ChannelPlaybackSnapshot(state.Channels);
                    ApplyState(form, state);
                    break;
                }
                case "click":
                {
                    int row = Int32.Parse(p[2], Inv), col = Int32.Parse(p[3], Inv);
                    MouseButtons button = p[1] == "L" ? MouseButtons.Left : MouseButtons.Right;
                    tipRow = row; tipCol = col;
                    Call(form, "GridCellMouseDown", grid,
                        new DataGridViewCellMouseEventArgs(col, row, 0, 0, new MouseEventArgs(button, 1, 0, 0, 0)));
                    break;
                }
                case "scrub": ed.ApplyScrubDeltaForTesting(Int32.Parse(p[1], Inv), p[2] == "1"); break;
                case "up": Call(form, "GridMouseUp", grid, new MouseEventArgs(MouseButtons.Left, 1, 0, 0, 0)); break;
                case "typing": ed.EnterTypingForTesting(); break;
                case "commit": ed.CommitTextForTesting(p.Length > 1 ? op.Substring(7) : ed.Text); break;
                case "focusloss": ed.CommitFocusLossForTesting(p.Length > 1 ? op.Substring(10) : ed.Text); break;
                case "arrow": ed.ArrowForTesting(p[1] == "up"); break;
                case "escape": ed.EscapeForTesting(); break;
                case "edright": ed.BeginPointerGesture(new Point(1, 1), MouseButtons.Right); break;
                case "hide": Call(form, "HideEditor", p[1] == "commit"); break;
                case "cfg":
                {
                    string mode = op.Substring(("cfg " + p[1] + " ").Length);
                    if (p[1] == "override") sim.OverrideMode = mode;
                    else if (p[1] == "chase") sim.ChaseMode = mode;
                    else sim.EnabledMode = mode;
                    break;
                }
                case "controlerror":
                {
                    int ch = Int32.Parse(p[1], Inv);
                    int attr = Int32.Parse(p[2], Inv);
                    string message = op.Length > ("controlerror " + p[1] + " " + p[2] + " ").Length
                        ? op.Substring(("controlerror " + p[1] + " " + p[2] + " ").Length) : "";
                    tipRow = ch; tipCol = 6 + attr;
                    form.ShowControlError(ch, (ChannelAttribute)attr, message);
                    break;
                }
                case "completechase":
                {
                    int ch = Int32.Parse(p[1], Inv);
                    int attr = Int32.Parse(p[2], Inv);
                    string message = op.Substring(("completechase " + p[1] + " " + p[2] + " ").Length);
                    tipRow = ch; tipCol = 6 + attr;
                    Call(form, "CompleteHistoricalChase", ch, (ChannelAttribute)attr, message == "-" ? null : new Exception(message));
                    break;
                }
                case "detach": form.DetachForSongReplacement(op.Substring(7)); break;
                case "attach": form.AttachSong(op.Substring(7)); break;
                default: throw new InvalidOperationException("unknown op " + op);
            }
        }

        // ---------------------------------------------------------------- --render

        private static int Render(string png, string stateName)
        {
            Application.EnableVisualStyles();
            State state = stateName == "flow" ? FlowState() : BuildStates()[2];
            using (ChannelMonitorForm form = new ChannelMonitorForm("render.mid"))
            {
                ApplyState(form, state);
                form.Show();
                for (int i = 0; i < 30; i++) { Application.DoEvents(); Thread.Sleep(10); }
                Rectangle r = form.GridForTesting.Bounds;
                DataGridView grid = form.GridForTesting;
                StringBuilder info = new StringBuilder();
                info.Append("client=" + form.ClientSize.Width + "x" + form.ClientSize.Height + " window=" + form.Width + "x" + form.Height);
                info.Append(" header=" + grid.ColumnHeadersHeight + " row=" + grid.Rows[0].Height + " widths=");
                for (int c = 0; c < grid.Columns.Count; c++) info.Append(grid.Columns[c].Width + (c + 1 < grid.Columns.Count ? "," : ""));
                Console.WriteLine(info.ToString());
                using (Bitmap bitmap = new Bitmap(form.Width, form.Height))
                {
                    using (Graphics g = Graphics.FromImage(bitmap))
                    {
                        IntPtr dc = g.GetHdc();
                        try { PrintWindow(form.Handle, dc, 0); }
                        finally { g.ReleaseHdc(dc); }
                    }
                    bitmap.Save(png, System.Drawing.Imaging.ImageFormat.Png);
                }
                form.Close();
            }
            return 0;
        }

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        private static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);
    }
}
