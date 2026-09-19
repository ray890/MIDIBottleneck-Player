using System;
using System.Drawing;
using System.Windows.Forms;

namespace MidiBottleneck
{
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
