using System;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Windows.Forms;

namespace MidiBottleneck
{
    internal static class ProductIcon
    {
        private const string ResourceName = "MidiBottleneck.ProductIcon.ico";
        private static readonly Icon SharedIcon = LoadIcon();

        internal static Icon Value { get { return SharedIcon; } }

        internal static void Apply(Form form)
        {
            if (form == null) throw new ArgumentNullException("form");
            form.Icon = SharedIcon;
        }

        private static Icon LoadIcon()
        {
            Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName);
            if (stream == null) throw new InvalidOperationException("The embedded MIDIBottleneck Player icon resource is missing.");
            using (stream)
            using (Icon resourceIcon = new Icon(stream))
                return (Icon)resourceIcon.Clone();
        }
    }
}
