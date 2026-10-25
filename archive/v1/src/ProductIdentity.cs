using System;
using System.Reflection;

namespace MidiBottleneck
{
    internal static class ProductIdentity
    {
        internal const string Name = "MIDIBottleneck Player";

        private static Assembly RuntimeAssembly
        {
            get { return Assembly.GetEntryAssembly() ?? Assembly.GetExecutingAssembly(); }
        }

        internal static string Version
        {
            get
            {
                AssemblyFileVersionAttribute value = (AssemblyFileVersionAttribute)Attribute.GetCustomAttribute(
                    RuntimeAssembly, typeof(AssemblyFileVersionAttribute));
                return value == null ? RuntimeAssembly.GetName().Version.ToString() : value.Version;
            }
        }

        internal static string InformationalVersion
        {
            get
            {
                AssemblyInformationalVersionAttribute value = (AssemblyInformationalVersionAttribute)Attribute.GetCustomAttribute(
                    RuntimeAssembly, typeof(AssemblyInformationalVersionAttribute));
                return value == null ? Version : value.InformationalVersion;
            }
        }
    }
}
