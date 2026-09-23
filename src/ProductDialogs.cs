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
            ClientSize = new Size(530, 276);
            Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);

            Label heading = new Label
            {
                Text = "This MIDI is expected to require substantial memory.",
                Font = new Font(Font, FontStyle.Bold),
                AutoSize = true,
                Location = new Point(18, 16)
            };
            Label details = new Label
            {
                Text = CreateMessage(inspection),
                Location = new Point(18, 48),
                Size = new Size(494, 166),
                AutoEllipsis = true
            };
            Button continueButton = new Button
            {
                Text = "Continue",
                DialogResult = DialogResult.OK,
                AutoSize = true,
                Location = new Point(346, 230)
            };
            Button cancelButton = new Button
            {
                Text = "Cancel",
                DialogResult = DialogResult.Cancel,
                AutoSize = true,
                Location = new Point(436, 230)
            };
            Controls.Add(heading);
            Controls.Add(details);
            Controls.Add(continueButton);
            Controls.Add(cancelButton);
            AcceptButton = continueButton;
            CancelButton = cancelButton;
        }

        internal static string CreateMessage(MidiLargeFileInspection inspection)
        {
            MidiMemoryProjection projection = inspection.Projection;
            string risk = projection.IsArchitectureRisk
                ? Environment.NewLine + Environment.NewLine +
                    "32-bit warning: this projection approaches the practical process address-space limit. The x64 build is strongly recommended."
                : String.Empty;
            return "File: " + System.IO.Path.GetFileName(inspection.FilePath) + Environment.NewLine +
                "Dispatchable events: " + inspection.Counts.DispatchableEventCount.ToString("N0") + Environment.NewLine +
                "Projected retained MIDI memory: " + FormatBytes(projection.ProjectedRetainedBytes) + Environment.NewLine +
                "Conservative loading peak: " + FormatBytes(projection.ProjectedPeakLowBytes) + "–" +
                    FormatBytes(projection.ProjectedPeakHighBytes) + Environment.NewLine + Environment.NewLine +
                "These are storage-aware projections, not a guarantee. Other application and system memory is additional." + risk;
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
