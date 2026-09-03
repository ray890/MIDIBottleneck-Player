using System;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] arguments)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.ThreadException += delegate(object sender, System.Threading.ThreadExceptionEventArgs e)
            {
                MessageBox.Show(e.Exception.Message, "Unexpected error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            };
            using (MainForm form = new MainForm())
            {
                if (arguments != null && arguments.Length == 1 &&
                    String.Equals(arguments[0], "--launch-smoke", StringComparison.OrdinalIgnoreCase))
                    form.Shown += delegate { form.BeginInvoke((MethodInvoker)delegate { form.Close(); }); };
                Application.Run(form);
            }
        }
    }
}
