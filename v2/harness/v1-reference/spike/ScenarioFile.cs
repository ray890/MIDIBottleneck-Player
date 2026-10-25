// Parser for the committed data-driven scenario file (v2/harness/engine-scenarios.txt).
// The grammar and every key are specified in v2/harness/ENGINE_SCENARIOS.md;
// this file only reads text. ScenarioBinder.cs turns the parsed steps into
// calls on v1's PlaybackEngine.
//
// Compiled with C# 5 (the .NET Framework csc.exe): no string interpolation,
// no null-conditional operators.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace MidiBottleneck
{
    // A command or setup time: a sum of terms, each either "N/M" (the integer
    // D * N / M, multiplication first, truncating division, D = max(1000,
    // song duration in microseconds)) or a plain unsigned integer (absolute
    // microseconds). Written "4/10+50000". Setup-less lines have no time.
    internal sealed class ScenarioTime
    {
        private readonly List<long[]> _terms = new List<long[]>();   // {numerator, denominator}; denominator 0 = absolute

        internal static ScenarioTime Parse(string text)
        {
            if (String.IsNullOrEmpty(text)) throw new FormatException("empty time");
            ScenarioTime time = new ScenarioTime();
            foreach (string term in text.Split('+'))
            {
                int slash = term.IndexOf('/');
                if (slash < 0)
                {
                    time._terms.Add(new long[] { ParseUnsigned(term, 1000000000000L), 0 });
                }
                else
                {
                    long numerator = ParseUnsigned(term.Substring(0, slash), 1000000L);
                    long denominator = ParseUnsigned(term.Substring(slash + 1), 1000000L);
                    if (denominator == 0) throw new FormatException("zero denominator in time '" + text + "'");
                    time._terms.Add(new long[] { numerator, denominator });
                }
            }
            return time;
        }

        private static long ParseUnsigned(string text, long maximum)
        {
            long value;
            if (text.Length == 0 || text.Length > 15 || text.Trim() != text ||
                !Int64.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value) || value > maximum)
                throw new FormatException("bad unsigned integer '" + text + "' in time");
            return value;
        }

        internal long Evaluate(long duration)
        {
            long total = 0;
            for (int i = 0; i < _terms.Count; i++)
            {
                long[] term = _terms[i];
                total += term[1] == 0 ? term[0] : duration * term[0] / term[1];
            }
            return total;
        }
    }

    // One directive line (setup directive or "at" command), tokenised.
    internal sealed class ScenarioStep
    {
        internal int Line;
        internal ScenarioTime At;      // null for setup directives
        internal string Verb;
        internal string[] Args;
    }

    internal sealed class ScenarioDefinition
    {
        internal string Name;
        internal int Line;
        internal string Description = "";
        internal int Snapshots;                  // 0 none, 1 on, 2 full
        internal bool StopAfterEnd = true;
        internal ScenarioStep Route;             // null = play the loaded song unrouted
        internal readonly List<ScenarioStep> Setup = new List<ScenarioStep>();
        internal readonly List<ScenarioStep> Commands = new List<ScenarioStep>();
    }

    internal static class ScenarioFile
    {
        internal const string FileName = "engine-scenarios.txt";

        // Looks for <ancestor>\harness\engine-scenarios.txt starting at the
        // executable's directory (v2\out\spike\x64 -> v2\harness).
        internal static string FindDefault()
        {
            DirectoryInfo dir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
            while (dir != null)
            {
                string candidate = Path.Combine(Path.Combine(dir.FullName, "harness"), FileName);
                if (File.Exists(candidate)) return candidate;
                dir = dir.Parent;
            }
            throw new FileNotFoundException("Cannot find harness\\" + FileName + " above " +
                AppDomain.CurrentDomain.BaseDirectory + "; pass --scenarios <file>.");
        }

        internal static List<ScenarioDefinition> Load(string path)
        {
            string[] lines = File.ReadAllText(path, new UTF8Encoding(false)).Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
            List<ScenarioDefinition> scenarios = new List<ScenarioDefinition>();
            Dictionary<string, List<ScenarioStep>> profiles = new Dictionary<string, List<ScenarioStep>>();
            HashSet<string> scenarioNames = new HashSet<string>();
            ScenarioDefinition current = null;
            List<ScenarioStep> profile = null;
            string profileName = null;
            bool sawFormat = false;
            for (int index = 0; index < lines.Length; index++)
            {
                int lineNumber = index + 1;
                string text = lines[index];
                int hash = text.IndexOf('#');
                if (hash >= 0) text = text.Substring(0, hash);
                string[] tokens = text.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                if (tokens.Length == 0) continue;
                string verb = tokens[0];
                string[] args = new string[tokens.Length - 1];
                Array.Copy(tokens, 1, args, 0, args.Length);
                try
                {
                    if (!sawFormat)
                    {
                        if (verb != "format" || args.Length != 1 || args[0] != "1")
                            throw new FormatException("the first directive must be 'format 1'");
                        sawFormat = true;
                        continue;
                    }
                    if (verb == "scenario")
                    {
                        if (profile != null) throw new FormatException("'scenario' inside a profile");
                        RequireArgs(verb, args, 1);
                        RequireName(args[0]);
                        if (!scenarioNames.Add(args[0])) throw new FormatException("duplicate scenario '" + args[0] + "'");
                        current = new ScenarioDefinition();
                        current.Name = args[0];
                        current.Line = lineNumber;
                        scenarios.Add(current);
                        continue;
                    }
                    if (verb == "profile")
                    {
                        if (profile != null) throw new FormatException("nested 'profile'");
                        RequireArgs(verb, args, 1);
                        RequireName(args[0]);
                        if (profiles.ContainsKey(args[0])) throw new FormatException("duplicate profile '" + args[0] + "'");
                        profileName = args[0];
                        profile = new List<ScenarioStep>();
                        current = null;
                        continue;
                    }
                    if (verb == "end-profile")
                    {
                        if (profile == null) throw new FormatException("'end-profile' outside a profile");
                        profiles[profileName] = profile;
                        profile = null;
                        continue;
                    }
                    if (profile != null)
                    {
                        if (verb == "at" || verb == "use" || verb == "describe" || verb == "snapshots" || verb == "stop-after-end")
                            throw new FormatException("'" + verb + "' is not allowed inside a profile");
                        profile.Add(MakeStep(lineNumber, verb, args));
                        continue;
                    }
                    if (current == null) throw new FormatException("'" + verb + "' before the first scenario");
                    switch (verb)
                    {
                        case "describe":
                            current.Description = String.Join(" ", args);
                            break;
                        case "snapshots":
                            RequireArgs(verb, args, 1);
                            if (args[0] == "none") current.Snapshots = 0;
                            else if (args[0] == "on") current.Snapshots = 1;
                            else if (args[0] == "full") current.Snapshots = 2;
                            else throw new FormatException("snapshots takes none|on|full");
                            break;
                        case "stop-after-end":
                            RequireArgs(verb, args, 1);
                            if (args[0] == "on") current.StopAfterEnd = true;
                            else if (args[0] == "off") current.StopAfterEnd = false;
                            else throw new FormatException("stop-after-end takes on|off");
                            break;
                        case "use":
                            RequireArgs(verb, args, 1);
                            List<ScenarioStep> used;
                            if (current.Commands.Count != 0) throw new FormatException("'use' after the first 'at'");
                            if (!profiles.TryGetValue(args[0], out used)) throw new FormatException("unknown profile '" + args[0] + "'");
                            for (int i = 0; i < used.Count; i++) AddSetup(current, used[i]);
                            break;
                        case "at":
                            if (args.Length < 2) throw new FormatException("'at' takes a time and a command");
                            ScenarioStep command = new ScenarioStep();
                            command.Line = lineNumber;
                            command.At = ScenarioTime.Parse(args[0]);
                            command.Verb = args[1];
                            command.Args = new string[args.Length - 2];
                            Array.Copy(args, 2, command.Args, 0, command.Args.Length);
                            current.Commands.Add(command);
                            break;
                        default:
                            if (current.Commands.Count != 0) throw new FormatException("setup directive '" + verb + "' after the first 'at'");
                            AddSetup(current, MakeStep(lineNumber, verb, args));
                            break;
                    }
                }
                catch (FormatException ex)
                {
                    throw new FormatException(path + " line " + lineNumber + ": " + ex.Message);
                }
            }
            if (profile != null) throw new FormatException(path + ": profile '" + profileName + "' is not closed with end-profile");
            if (!sawFormat) throw new FormatException(path + ": missing 'format 1'");
            return scenarios;
        }

        private static ScenarioStep MakeStep(int line, string verb, string[] args)
        {
            ScenarioStep step = new ScenarioStep();
            step.Line = line;
            step.Verb = verb;
            step.Args = args;
            return step;
        }

        private static void AddSetup(ScenarioDefinition scenario, ScenarioStep step)
        {
            if (step.Verb == "route")
            {
                if (scenario.Route != null) throw new FormatException("more than one 'route' in a scenario");
                scenario.Route = step;
                return;
            }
            scenario.Setup.Add(step);
        }

        private static void RequireArgs(string verb, string[] args, int count)
        {
            if (args.Length != count) throw new FormatException("'" + verb + "' takes " + count + " argument(s)");
        }

        private static void RequireName(string name)
        {
            for (int i = 0; i < name.Length; i++)
            {
                char c = name[i];
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                    throw new FormatException("name '" + name + "' must be lower-case letters, digits and '-'");
            }
            if (name.Length == 0) throw new FormatException("empty name");
        }
    }
}
