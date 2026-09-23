using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal sealed class LargeMidiWarningDialog : Form
    {
        internal LargeMidiWarningDialog(MidiLargeFileInspection inspection)
        {
            if (inspection == null) throw new ArgumentNullException("inspection");
            ProductIcon.Apply(this);
            Text = "Large MIDI memory warning";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = false;
            MaximizeBox = false;
            ShowInTaskbar = false;
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Padding = new Padding(16);

            Label heading = new Label
            {
                Text = "This MIDI may need a large amount of memory.",
                Font = new Font(Font, FontStyle.Bold),
                AutoSize = true,
                MaximumSize = new Size(540, 0),
                Margin = new Padding(0, 0, 0, 12)
            };
            Label details = new Label
            {
                Text = CreateMessage(inspection),
                AutoSize = true,
                MaximumSize = new Size(540, 0),
                Margin = new Padding(0, 0, 0, 14)
            };
            Button continueButton = new Button
            {
                Text = "Continue",
                DialogResult = DialogResult.OK,
                AutoSize = true,
                Margin = new Padding(0, 0, 8, 0)
            };
            Button cancelButton = new Button
            {
                Text = "Cancel",
                DialogResult = DialogResult.Cancel,
                AutoSize = true,
                Margin = new Padding(0)
            };

            FlowLayoutPanel buttons = new FlowLayoutPanel
            {
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                Dock = DockStyle.Fill,
                FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false,
                Margin = new Padding(0)
            };
            buttons.Controls.Add(continueButton);
            buttons.Controls.Add(cancelButton);

            TableLayoutPanel layout = new TableLayoutPanel
            {
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                ColumnCount = 1,
                RowCount = 3,
                Dock = DockStyle.Fill,
                Margin = new Padding(0)
            };
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.Controls.Add(heading, 0, 0);
            layout.Controls.Add(details, 0, 1);
            layout.Controls.Add(buttons, 0, 2);
            Controls.Add(layout);
            AcceptButton = continueButton;
            CancelButton = cancelButton;
        }

        internal static string CreateMessage(MidiLargeFileInspection inspection)
        {
            MidiMemoryProjection projection = inspection.Projection;
            string risk = projection.IsArchitectureRisk
                ? Environment.NewLine + Environment.NewLine +
                    "32-bit warning: this MIDI may be too large for the x86 version. Use the x64 version if possible."
                : String.Empty;
            return "File: " + System.IO.Path.GetFileName(inspection.FilePath) + Environment.NewLine +
                "MIDI events to process: " + inspection.Counts.DispatchableEventCount.ToString("N0") +
                Environment.NewLine + Environment.NewLine +
                "Estimated memory while the MIDI is open: " + FormatBytes(projection.ProjectedRetainedBytes) + Environment.NewLine +
                "Estimated highest memory use while loading: " + FormatBytes(projection.ProjectedPeakLowBytes) + "–" +
                    FormatBytes(projection.ProjectedPeakHighBytes) + Environment.NewLine + Environment.NewLine +
                "These are estimates. Loading may still fail if enough memory is not available." + risk;
        }

        internal static string FormatBytes(long bytes)
        {
            double gib = Math.Max(0, bytes) / 1073741824.0;
            if (gib >= 1) return gib.ToString(gib >= 10 ? "N1" : "N2") + " GiB";
            return (Math.Max(0, bytes) / 1048576.0).ToString("N0") + " MiB";
        }
    }

    internal sealed class AboutProductDialog : Form
    {
        internal AboutProductDialog()
        {
            ProductIcon.Apply(this);
            Text = "About " + ProductIdentity.Name;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = false;
            MaximizeBox = false;
            ShowInTaskbar = false;
            ClientSize = new Size(390, 165);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            Label name = new Label { Text = ProductIdentity.Name, Font = new Font(Font, FontStyle.Bold), AutoSize = true, Location = new Point(18, 18) };
            Label build = new Label { Text = ProductIdentity.InformationalVersion + " • version " + ProductIdentity.Version, AutoSize = true, Location = new Point(18, 50) };
            Label description = new Label
            {
                Text = "A MIDI workload, queue, timing, and bottleneck laboratory.",
                AutoSize = true,
                Location = new Point(18, 78),
                ForeColor = Color.DimGray
            };
            Button close = new Button { Text = "OK", DialogResult = DialogResult.OK, AutoSize = true, Location = new Point(296, 121) };
            Controls.Add(name); Controls.Add(build); Controls.Add(description); Controls.Add(close);
            AcceptButton = close; CancelButton = close;
        }
    }

}
