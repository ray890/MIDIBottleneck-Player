using System;
using System.Globalization;
using System.IO;

namespace MidiBottleneck
{
    internal static class UserPreferences
    {
        private static string SettingsPath
        {
            get
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "MidiBottleneck", "settings.txt");
            }
        }

        public static long LoadEffectiveSpeedWindow()
        {
            try
            {
                string text = File.ReadAllText(SettingsPath).Trim();
                long value;
                if (TryParseEffectiveSpeedWindow(text, out value)) return value;
            }
            catch { }
            return EffectivePlaybackSpeed.DefaultWindowMicroseconds;
        }

        public static void SaveEffectiveSpeedWindow(long microseconds)
        {
            if (!EffectivePlaybackSpeed.IsValidWindow(microseconds)) throw new ArgumentOutOfRangeException("microseconds");
            try
            {
                string path = SettingsPath;
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllText(path, microseconds.ToString(CultureInfo.InvariantCulture));
            }
            catch { }
        }

        internal static bool TryParseEffectiveSpeedWindow(string text, out long microseconds)
        {
            if (!Int64.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out microseconds)) return false;
            return EffectivePlaybackSpeed.IsValidWindow(microseconds);
        }
    }
}
