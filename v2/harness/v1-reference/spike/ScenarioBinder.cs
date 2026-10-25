// Binds parsed scenario steps (ScenarioFile.cs) to v1's public engine API.
// Every property key, enum token and command name is specified in
// v2/harness/ENGINE_SCENARIOS.md. Tokens that name a v1 enum member are the
// member name in lower-case kebab-case ("DropOldestCompleteNote" ->
// "drop-oldest-complete-note"); keys that name a PlaybackEngine property are
// the property name in the same form ("SimulateSlowdown" -> "simulate-slowdown").
// Log names (the text after "H CMD ") use the v1 member names.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace MidiBottleneck
{
    internal static class ScenarioBinder
    {
        private sealed class Property
        {
            internal string Member;
            internal string Key;
            internal char Kind;            // 'b' on|off, 'n' integer, 'e' enum token
            internal Type EnumType;
            internal long Minimum;
            internal long Maximum;
            internal Action<PlaybackEngine, long> Set;
        }

        private static readonly Dictionary<string, Property> Properties = BuildProperties();

        private static Dictionary<string, Property> BuildProperties()
        {
            Dictionary<string, Property> map = new Dictionary<string, Property>();
            AddBool(map, "SimulateSlowdown", delegate(PlaybackEngine e, long v) { e.SimulateSlowdown = v != 0; });
            AddBool(map, "ApplyQueueLimitWithoutSlowdown", delegate(PlaybackEngine e, long v) { e.ApplyQueueLimitWithoutSlowdown = v != 0; });
            AddBool(map, "ChaseMidiStateOnPlaySeek", delegate(PlaybackEngine e, long v) { e.ChaseMidiStateOnPlaySeek = v != 0; });
            AddBool(map, "PreservePlaybackQueueOnPauseSeek", delegate(PlaybackEngine e, long v) { e.PreservePlaybackQueueOnPauseSeek = v != 0; });
            AddBool(map, "OutputPacedTimeline", delegate(PlaybackEngine e, long v) { e.OutputPacedTimeline = v != 0; });
            AddBool(map, "ScaleNoteIntervalByTraffic", delegate(PlaybackEngine e, long v) { e.ScaleNoteIntervalByTraffic = v != 0; });
            AddBool(map, "IntervalPolyphonyLimitEnabled", delegate(PlaybackEngine e, long v) { e.IntervalPolyphonyLimitEnabled = v != 0; });
            AddNumber(map, "IntervalPolyphonyLimit", Int32.MinValue, Int32.MaxValue, delegate(PlaybackEngine e, long v) { e.IntervalPolyphonyLimit = (int)v; });
            AddNumber(map, "QueueLengthLimit", Int32.MinValue, Int32.MaxValue, delegate(PlaybackEngine e, long v) { e.QueueLengthLimit = (int)v; });
            AddBool(map, "QueueAgeLimitEnabled", delegate(PlaybackEngine e, long v) { e.QueueAgeLimitEnabled = v != 0; });
            AddNumber(map, "QueueAgeLimitMicroseconds", Int64.MinValue, Int64.MaxValue, delegate(PlaybackEngine e, long v) { e.QueueAgeLimitMicroseconds = v; });
            AddEnum(map, "OverflowPolicy", typeof(OverflowPolicy), delegate(PlaybackEngine e, long v) { e.OverflowPolicy = (OverflowPolicy)(int)v; });
            AddEnum(map, "ServiceDurationMode", typeof(ServiceDurationMode), delegate(PlaybackEngine e, long v) { e.ServiceDurationMode = (ServiceDurationMode)(int)v; });
            AddNumber(map, "ProcessingMicroseconds", Int64.MinValue, Int64.MaxValue, delegate(PlaybackEngine e, long v) { e.ProcessingMicroseconds = v; });
            AddNumber(map, "MidiBitrate", Int64.MinValue, Int64.MaxValue, delegate(PlaybackEngine e, long v) { e.MidiBitrate = v; });
            AddNumber(map, "EventsPerSecond", Int64.MinValue, Int64.MaxValue, delegate(PlaybackEngine e, long v) { e.EventsPerSecond = v; });
            return map;
        }

        private static void AddBool(Dictionary<string, Property> map, string member, Action<PlaybackEngine, long> set)
        {
            Property p = new Property();
            p.Member = member; p.Key = Kebab(member); p.Kind = 'b'; p.Minimum = 0; p.Maximum = 1; p.Set = set;
            map[p.Key] = p;
        }

        private static void AddNumber(Dictionary<string, Property> map, string member, long min, long max, Action<PlaybackEngine, long> set)
        {
            Property p = new Property();
            p.Member = member; p.Key = Kebab(member); p.Kind = 'n'; p.Minimum = min; p.Maximum = max; p.Set = set;
            map[p.Key] = p;
        }

        private static void AddEnum(Dictionary<string, Property> map, string member, Type enumType, Action<PlaybackEngine, long> set)
        {
            Property p = new Property();
            p.Member = member; p.Key = Kebab(member); p.Kind = 'e'; p.EnumType = enumType; p.Set = set;
            map[p.Key] = p;
        }

        internal static string Kebab(string pascal)
        {
            StringBuilder text = new StringBuilder();
            for (int i = 0; i < pascal.Length; i++)
            {
                char c = pascal[i];
                if (Char.IsUpper(c))
                {
                    if (i > 0) text.Append('-');
                    text.Append(Char.ToLowerInvariant(c));
                }
                else text.Append(c);
            }
            return text.ToString();
        }

        private static T ParseEnum<T>(string token, string what) where T : struct
        {
            string[] names = Enum.GetNames(typeof(T));
            for (int i = 0; i < names.Length; i++)
                if (Kebab(names[i]) == token) return (T)Enum.Parse(typeof(T), names[i]);
            throw new FormatException("unknown " + what + " '" + token + "'");
        }

        private static long ParseNumber(string token, long min, long max, string what)
        {
            long value;
            if (!Int64.TryParse(token, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out value) ||
                value < min || value > max)
                throw new FormatException("bad " + what + " '" + token + "'");
            return value;
        }

        // Parses a property value into the long the setter takes, and the
        // text used in log names.
        private static long ParseValue(Property property, string token, out string logText)
        {
            if (property.Kind == 'b')
            {
                if (token == "on") { logText = "1"; return 1; }
                if (token == "off") { logText = "0"; return 0; }
                throw new FormatException(property.Key + " takes on|off");
            }
            if (property.Kind == 'e')
            {
                string[] names = Enum.GetNames(property.EnumType);
                for (int i = 0; i < names.Length; i++)
                    if (Kebab(names[i]) == token)
                    {
                        logText = names[i];
                        return (int)Enum.Parse(property.EnumType, names[i]);
                    }
                throw new FormatException("unknown value '" + token + "' for " + property.Key);
            }
            long value = ParseNumber(token, property.Minimum, property.Maximum, property.Key + " value");
            logText = value.ToString(CultureInfo.InvariantCulture);
            return value;
        }

        private static Property FindProperty(string key)
        {
            Property property;
            if (!Properties.TryGetValue(key, out property)) throw new FormatException("unknown setting key '" + key + "'");
            return property;
        }

        private static void Need(ScenarioStep step, int min, int max)
        {
            if (step.Args.Length < min || step.Args.Length > max)
                throw new FormatException("'" + step.Verb + "' takes " + (min == max ? min.ToString() : min + " to " + max) + " argument(s)");
        }

        // ---- setup directives ------------------------------------------------

        internal static Action<PlaybackEngine> BindSetup(ScenarioStep step)
        {
            try
            {
                switch (step.Verb)
                {
                    case "set":
                    {
                        Need(step, 2, 2);
                        Property property = FindProperty(step.Args[0]);
                        string unused;
                        long value = ParseValue(property, step.Args[1], out unused);
                        return delegate(PlaybackEngine e) { property.Set(e, value); };
                    }
                    case "service":
                    {
                        ServiceDurationMode mode; long processing, bitrate, rate;
                        ParseService(step, out mode, out processing, out bitrate, out rate);
                        return delegate(PlaybackEngine e) { e.SetServiceConfiguration(mode, processing, bitrate, rate); };
                    }
                    case "override":
                    {
                        int channel; ChannelAttribute attribute; int value; string unused;
                        ParseOverride(step, out channel, out attribute, out value, out unused);
                        return delegate(PlaybackEngine e) { e.SetChannelOverride(channel, attribute, value); };
                    }
                    case "channel":
                    {
                        int channel; bool enabled;
                        ParseChannel(step, out channel, out enabled);
                        return delegate(PlaybackEngine e) { e.SetChannelEnabled(channel, enabled); };
                    }
                    default:
                        throw new FormatException("unknown setup directive '" + step.Verb + "'");
                }
            }
            catch (FormatException ex)
            {
                throw new FormatException("line " + step.Line + ": " + ex.Message);
            }
        }

        private static void ParseService(ScenarioStep step, out ServiceDurationMode mode, out long processing, out long bitrate, out long rate)
        {
            Need(step, 4, 4);
            mode = ParseEnum<ServiceDurationMode>(step.Args[0], "service mode");
            processing = ParseNumber(step.Args[1], Int64.MinValue, Int64.MaxValue, "processing microseconds");
            bitrate = ParseNumber(step.Args[2], Int64.MinValue, Int64.MaxValue, "bitrate");
            rate = ParseNumber(step.Args[3], Int64.MinValue, Int64.MaxValue, "events per second");
        }

        private static void ParseOverride(ScenarioStep step, out int channel, out ChannelAttribute attribute, out int value, out string valueText)
        {
            Need(step, 3, 3);
            channel = (int)ParseNumber(step.Args[0], 0, 15, "channel");
            attribute = ParseEnum<ChannelAttribute>(step.Args[1], "channel attribute");
            if (step.Args[2] == "auto") { value = ChannelOverrideState.AutoValue; valueText = "Auto"; }
            else
            {
                value = (int)ParseNumber(step.Args[2], ChannelOverrideState.Minimum(attribute), ChannelOverrideState.Maximum(attribute), "override value");
                valueText = value.ToString(CultureInfo.InvariantCulture);
            }
        }

        private static void ParseChannel(ScenarioStep step, out int channel, out bool enabled)
        {
            Need(step, 2, 2);
            channel = (int)ParseNumber(step.Args[0], 0, 15, "channel");
            if (step.Args[1] == "on") enabled = true;
            else if (step.Args[1] == "off") enabled = false;
            else throw new FormatException("channel takes on|off");
        }

        // ---- route -------------------------------------------------------------

        internal static TrackRoutingSettings ParseRoute(ScenarioStep step)
        {
            TrackRoutingSettings settings = new TrackRoutingSettings();
            try
            {
                for (int i = 0; i < step.Args.Length; i++)
                {
                    int eq = step.Args[i].IndexOf('=');
                    if (eq <= 0) throw new FormatException("route takes key=value arguments");
                    string key = step.Args[i].Substring(0, eq), value = step.Args[i].Substring(eq + 1);
                    if (key == "tier") settings.MaximumAutomaticTier = ParseEnum<TrackRoutingTier>(value, "routing tier");
                    else if (key == "cap") settings.TargetCapacity = (int)ParseNumber(value, 1, 16, "capacity");
                    else if (key == "perc") settings.PercussionPolicy = ParseEnum<PercussionRoutingPolicy>(value, "percussion policy");
                    else if (key == "usable")
                    {
                        int mask;
                        if (value.Length != 4 || !Int32.TryParse(value, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out mask))
                            throw new FormatException("usable takes four hex digits");
                        for (int channel = 0; channel < 16; channel++)
                            settings.UsableDestinationChannels[channel] = (mask & (1 << channel)) != 0;
                    }
                    else if (key == "manual")
                    {
                        foreach (string entry in value.Split(';'))
                        {
                            string[] f = entry.Split('.', '=');
                            if (f.Length != 3) throw new FormatException("manual takes track.channel=destination;...");
                            settings.ManualDestinations[new TrackLaneKey((int)ParseNumber(f[0], 0, 65535, "track"),
                                (int)ParseNumber(f[1], 0, 15, "source channel"))] = (int)ParseNumber(f[2], 0, 15, "destination");
                        }
                    }
                    else throw new FormatException("unknown route key '" + key + "'");
                }
            }
            catch (FormatException ex)
            {
                throw new FormatException("line " + step.Line + ": " + ex.Message);
            }
            return settings;
        }

        // ---- commands --------------------------------------------------------

        internal static SpikeCommand BindCommand(ScenarioStep step, long duration)
        {
            try { return BindCommandCore(step, duration); }
            catch (FormatException ex) { throw new FormatException("line " + step.Line + ": " + ex.Message); }
        }

        private static SpikeCommand BindCommandCore(ScenarioStep step, long duration)
        {
            long offset = step.At.Evaluate(duration);
            string[] a = step.Args;
            switch (step.Verb)
            {
                case "start":
                {
                    Need(step, 2, 3);
                    ProcessingMode mode = ParseEnum<ProcessingMode>(a[0], "processing mode");
                    long position = ScenarioTime.Parse(a[1]).Evaluate(duration);
                    bool paused = false;
                    if (a.Length == 3)
                    {
                        if (a[2] != "paused") throw new FormatException("start's third argument must be 'paused'");
                        paused = true;
                    }
                    return new SpikeCommand(offset, "Start(" + mode + ",pos=" + position + (paused ? ",paused" : "") + ")", true,
                        delegate(PlaybackEngine e) { e.Start(RendezvousSpike.CurrentSong, RendezvousSpike.CurrentOutput, mode, position, paused); });
                }
                case "stop":
                    Need(step, 0, 0);
                    return new SpikeCommand(offset, "Stop", false, delegate(PlaybackEngine e) { e.Stop(); });
                case "pause":
                    Need(step, 0, 0);
                    return new SpikeCommand(offset, "Pause", PauseStartsWorker, delegate(PlaybackEngine e) { e.Pause(); });
                case "resume":
                    Need(step, 0, 0);
                    return new SpikeCommand(offset, "Resume", false, delegate(PlaybackEngine e) { e.Resume(); });
                case "seek":
                {
                    Need(step, 1, 1);
                    long target = ScenarioTime.Parse(a[0]).Evaluate(duration);
                    return new SpikeCommand(offset, "Seek(" + target + ")", SeekStartsWorker,
                        delegate(PlaybackEngine e) { e.Seek(target); });
                }
                case "snapshot":
                    Need(step, 0, 0);
                    return new SpikeCommand(offset, "Snapshot", false, delegate(PlaybackEngine e) { });
                case "set":
                {
                    Need(step, 2, 2);
                    Property property = FindProperty(a[0]);
                    string text;
                    long value = ParseValue(property, a[1], out text);
                    return new SpikeCommand(offset, "Set(" + property.Member + "=" + text + ")", false,
                        delegate(PlaybackEngine e) { property.Set(e, value); });
                }
                case "service":
                {
                    ServiceDurationMode mode; long processing, bitrate, rate;
                    ParseService(step, out mode, out processing, out bitrate, out rate);
                    return new SpikeCommand(offset, "SetService(" + mode + "," + processing + "us)", false,
                        delegate(PlaybackEngine e) { e.SetServiceConfiguration(mode, processing, bitrate, rate); });
                }
                case "restart":
                {
                    if (a.Length < 1) throw new FormatException("'restart' takes a processing mode");
                    ProcessingMode mode = ParseEnum<ProcessingMode>(a[0], "processing mode");
                    List<Property> properties = new List<Property>();
                    List<long> values = new List<long>();
                    StringBuilder name = new StringBuilder("Restart(").Append(mode);
                    for (int i = 1; i < a.Length; i++)
                    {
                        int eq = a[i].IndexOf('=');
                        if (eq <= 0) throw new FormatException("restart settings are key=value");
                        Property property = FindProperty(a[i].Substring(0, eq));
                        string text;
                        values.Add(ParseValue(property, a[i].Substring(eq + 1), out text));
                        properties.Add(property);
                        name.Append(',').Append(property.Member).Append('=').Append(text);
                    }
                    name.Append(')');
                    return new SpikeCommand(offset, name.ToString(), RestartStartsWorker, delegate(PlaybackEngine e)
                    {
                        // MainForm.RestartForProcessingModeChange: remember the
                        // state and the intended timeline position, Stop, apply
                        // the new settings, Start again (paused when it was
                        // paused). Only while Playing or Paused.
                        PlaybackState previous = e.State;
                        bool active = previous == PlaybackState.Playing || previous == PlaybackState.Paused;
                        long position = 0;
                        if (active)
                        {
                            position = e.GetSnapshot().IntendedTimelineMicroseconds;
                            e.Stop();
                        }
                        for (int i = 0; i < properties.Count; i++) properties[i].Set(e, values[i]);
                        if (active)
                            e.Start(RendezvousSpike.CurrentSong, RendezvousSpike.CurrentOutput, mode, position,
                                previous == PlaybackState.Paused);
                    });
                }
                case "override":
                {
                    int channel; ChannelAttribute attribute; int value; string text;
                    ParseOverride(step, out channel, out attribute, out value, out text);
                    return new SpikeCommand(offset, "Override(ch=" + channel + "," + attribute + "=" + text + ")", false,
                        delegate(PlaybackEngine e) { e.SetChannelOverride(channel, attribute, value); });
                }
                case "channel":
                {
                    int channel; bool enabled;
                    ParseChannel(step, out channel, out enabled);
                    return new SpikeCommand(offset, "Channel(ch=" + channel + ",enabled=" + (enabled ? "1" : "0") + ")", false,
                        delegate(PlaybackEngine e) { e.SetChannelEnabled(channel, enabled); });
                }
                default:
                    throw new FormatException("unknown command '" + step.Verb + "'");
            }
        }

        // Pause: a worker is created unless the preserving path (live worker,
        // flag set) handles it, in which case the control thread waits for
        // _pauseAcknowledged.
        private static bool PauseStartsWorker(PlaybackEngine e)
        {
            return e.State == PlaybackState.Playing && !(e.PreservePlaybackQueueOnPauseSeek && e.HasLiveWorker);
        }

        private static bool SeekStartsWorker(PlaybackEngine e)
        {
            PlaybackState state = e.State;
            return (state == PlaybackState.Playing || state == PlaybackState.Paused) &&
                !(e.PreservePlaybackQueueOnPauseSeek && e.HasLiveWorker);
        }

        private static bool RestartStartsWorker(PlaybackEngine e)
        {
            PlaybackState state = e.State;
            return state == PlaybackState.Playing || state == PlaybackState.Paused;
        }

        // ---- whole scenarios -------------------------------------------------

        // Binds every scenario against a dummy duration so malformed steps are
        // reported before any run starts.
        internal static void Validate(List<ScenarioDefinition> definitions)
        {
            foreach (ScenarioDefinition definition in definitions) Build(definition, 1000);
        }

        internal static SpikeScenario Build(ScenarioDefinition definition, long duration)
        {
            SpikeScenario scenario = new SpikeScenario();
            scenario.Name = definition.Name;
            scenario.SnapshotMode = definition.Snapshots;
            scenario.StopAfterEnd = definition.StopAfterEnd;
            if (definition.Route != null) scenario.Route = ParseRoute(definition.Route);
            List<Action<PlaybackEngine>> setup = new List<Action<PlaybackEngine>>();
            for (int i = 0; i < definition.Setup.Count; i++) setup.Add(BindSetup(definition.Setup[i]));
            if (setup.Count != 0)
                scenario.Configure = delegate(PlaybackEngine e)
                {
                    for (int i = 0; i < setup.Count; i++) setup[i](e);
                };
            for (int i = 0; i < definition.Commands.Count; i++)
                scenario.Commands.Add(BindCommand(definition.Commands[i], duration));
            return scenario;
        }
    }
}
