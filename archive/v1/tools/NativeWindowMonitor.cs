using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class NativeWindowMonitor
{
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint ProcessQueryInformation = 0x0400;
    private const uint SmtoAbortIfHung = 0x0002;
    private const uint WmGetText = 0x000D;
    private const uint Th32csSnapProcess = 0x00000002;
    private const int JobObjectAssociateCompletionPortInformation = 7;
    private const uint JobObjectMessageNewProcess = 6;
    private static readonly string[] BuiltInTerms = {
        ".NET-BroadcastEventWindow", "MidiBottleneck.Tests", "unknown software exception",
        "0xC0020001", "Windows Error Reporting", "WerFault"
    };
    private static readonly string[] ErrorReportingProcessNames = {
        "WerFault", "WerFaultSecure", "wermgr", "dw20", "dwwin"
    };

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ProcessEntry32
    {
        public uint Size;
        public uint Usage;
        public uint ProcessId;
        public IntPtr DefaultHeapId;
        public uint ModuleId;
        public uint Threads;
        public uint ParentProcessId;
        public int BasePriority;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string ExeFile;
    }

    private sealed class Options
    {
        public string OutputPath;
        public int PollMilliseconds = 50;
        public int PostExitMilliseconds = 5000;
        public int TimeoutMilliseconds = 120000;
        public string Match;
        public bool ExpectMatch;
        public string ProcessMatch;
        public bool ExpectProcessMatch;
        public string Executable;
        public readonly List<string> Arguments = new List<string>();
    }

    private sealed class WindowRecord
    {
        public IntPtr Handle;
        public int ProcessId;
        public int ParentProcessId;
        public string Title;
        public string ClassName;
        public string DialogText;
        public string Executable;
        public string ProcessName;
        public string Architecture;
        public DateTime FirstDetectedUtc;
        public bool Visible;
        public bool ChildExited;
        public bool Suspicious;
        public bool BuiltInSuspicious;
        public bool CustomMatch;
        public string Reason;
    }

    private sealed class ProcessRecord
    {
        public int ProcessId;
        public int ParentProcessId;
        public string ProcessName;
        public string Executable;
        public string Architecture;
        public DateTime FirstDetectedUtc;
        public bool ChildExited;
        public bool AssociatedWithChild;
        public bool ErrorReportingProcess;
        public bool CustomMatch;
        public bool Suspicious;
        public bool JobAssociated;
        public bool UnidentifiedJobDescendant;
        public string DetectionSource;
        public string Reason;
        public readonly List<int> Ancestry = new List<int>();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectAssociateCompletionPort
    {
        public IntPtr CompletionKey;
        public IntPtr CompletionPort;
    }

    private sealed class JobProcessObserver : IDisposable
    {
        private readonly object _sync = new object();
        private readonly Queue<ProcessRecord> _pending = new Queue<ProcessRecord>();
        private readonly IntPtr _job;
        private readonly IntPtr _completionPort;
        private readonly Thread _thread;
        private volatile bool _stopping;

        public JobProcessObserver()
        {
            _job = CreateJobObject(IntPtr.Zero, null);
            if (_job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create the monitor job object.");
            _completionPort = CreateIoCompletionPort(new IntPtr(-1), IntPtr.Zero, UIntPtr.Zero, 1);
            if (_completionPort == IntPtr.Zero)
            {
                CloseHandle(_job);
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create the process-observer completion port.");
            }
            JobObjectAssociateCompletionPort association = new JobObjectAssociateCompletionPort();
            association.CompletionKey = _job;
            association.CompletionPort = _completionPort;
            if (!SetInformationJobObject(_job, JobObjectAssociateCompletionPortInformation,
                ref association, unchecked((uint)Marshal.SizeOf(typeof(JobObjectAssociateCompletionPort)))))
            {
                int error = Marshal.GetLastWin32Error();
                CloseHandle(_completionPort);
                CloseHandle(_job);
                throw new Win32Exception(error, "Could not associate the monitor job with its completion port.");
            }
            _thread = new Thread(Observe);
            _thread.IsBackground = true;
            _thread.Name = "MIDIBottleneck descendant-process observer";
            _thread.Start();
        }

        public void Attach(Process child)
        {
            if (!AssignProcessToJobObject(_job, child.Handle))
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not attach the monitored child to the descendant-process job.");
        }

        private void Observe()
        {
            while (!_stopping)
            {
                uint message;
                UIntPtr key;
                IntPtr processIdValue;
                bool received = GetQueuedCompletionStatus(_completionPort, out message, out key,
                    out processIdValue, 100);
                if (!received || message != JobObjectMessageNewProcess || processIdValue == IntPtr.Zero)
                    continue;
                ProcessRecord record = new ProcessRecord();
                record.ProcessId = unchecked((int)processIdValue.ToInt64());
                record.FirstDetectedUtc = DateTime.UtcNow;
                record.DetectionSource = "JobObjectCompletionPort";
                record.JobAssociated = true;
                Dictionary<int, ProcessRecord> snapshot = SnapshotProcesses("JobObjectCompletionPort+Toolhelp32Snapshot");
                ProcessRecord live;
                if (snapshot.TryGetValue(record.ProcessId, out live))
                {
                    record.ParentProcessId = live.ParentProcessId;
                    record.ProcessName = live.ProcessName;
                    EnrichProcessRecord(record, live);
                }
                lock (_sync) _pending.Enqueue(record);
            }
        }

        public List<ProcessRecord> Drain()
        {
            List<ProcessRecord> result = new List<ProcessRecord>();
            lock (_sync)
                while (_pending.Count != 0) result.Add(_pending.Dequeue());
            return result;
        }

        public void Dispose()
        {
            _stopping = true;
            _thread.Join(1000);
            CloseHandle(_completionPort);
            CloseHandle(_job);
        }
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder text, int capacity);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam,
        StringBuilder text, uint flags, uint timeout, out IntPtr result);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageName(IntPtr process, int flags,
        StringBuilder path, ref int size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool IsWow64Process(IntPtr process, out bool wow64);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32First(IntPtr snapshot, ref ProcessEntry32 entry);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32Next(IntPtr snapshot, ref ProcessEntry32 entry);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr jobAttributes, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr job, int informationClass,
        ref JobObjectAssociateCompletionPort information, uint informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateIoCompletionPort(IntPtr fileHandle,
        IntPtr existingCompletionPort, UIntPtr completionKey, uint concurrentThreads);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetQueuedCompletionStatus(IntPtr completionPort,
        out uint bytesTransferred, out UIntPtr completionKey, out IntPtr overlapped,
        uint milliseconds);

    private static int Main(string[] arguments)
    {
        try
        {
            Options options = Parse(arguments);
            return Monitor(options);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("MONITOR ERROR: " + ex);
            return 10;
        }
    }

    private static int Monitor(Options options)
    {
        string outputDirectory = Path.GetDirectoryName(Path.GetFullPath(options.OutputPath));
        if (!String.IsNullOrEmpty(outputDirectory)) Directory.CreateDirectory(outputDirectory);

        Dictionary<int, ProcessRecord> initialProcesses = SnapshotProcesses("initial-snapshot");
        Dictionary<int, int> parents = ParentMap(initialProcesses);
        Dictionary<int, int> knownParents = new Dictionary<int, int>(parents);
        HashSet<int> baselineProcessIds = new HashSet<int>(initialProcesses.Keys);
        HashSet<IntPtr> baseline = new HashSet<IntPtr>();
        foreach (WindowRecord window in EnumerateWindows(false, 0, parents, options.Match))
            baseline.Add(window.Handle);

        ProcessStartInfo start = new ProcessStartInfo(options.Executable, QuoteArguments(options.Arguments));
        start.UseShellExecute = false;
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        start.CreateNoWindow = false;
        Process child = new Process();
        child.StartInfo = start;
        child.EnableRaisingEvents = true;
        StringBuilder standardOutput = new StringBuilder();
        StringBuilder standardError = new StringBuilder();
        child.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e)
        {
            if (e.Data != null) lock (standardOutput) standardOutput.AppendLine(e.Data);
        };
        child.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e)
        {
            if (e.Data != null) lock (standardError) standardError.AppendLine(e.Data);
        };

        using (JobProcessObserver processObserver = new JobProcessObserver())
        {
            DateTime startUtc = DateTime.UtcNow;
            if (!child.Start()) throw new InvalidOperationException("The child process did not start.");
            int childProcessId = child.Id;
            processObserver.Attach(child);
            child.BeginOutputReadLine();
            child.BeginErrorReadLine();
            Console.WriteLine("Monitoring PID {0} ({1})", childProcessId, options.Executable);

            Dictionary<IntPtr, WindowRecord> observed = new Dictionary<IntPtr, WindowRecord>();
            Dictionary<int, ProcessRecord> pendingProcesses = new Dictionary<int, ProcessRecord>();
            HashSet<int> finalizedProcessIds = new HashSet<int>(baselineProcessIds);
            finalizedProcessIds.Add(childProcessId);
            DateTime? exitDetectedUtc = null;
            bool timedOut = false;
            bool anySuspiciousWindow = false;
            bool anySuspiciousProcess = false;
            bool anyUnexpectedSuspicious = false;
            bool expectedMatchSeen = false;
            bool expectedProcessMatchSeen = false;

            using (StreamWriter log = new StreamWriter(options.OutputPath, false, new UTF8Encoding(false)))
            {
                log.AutoFlush = true;
                WriteEvent(log, "monitor-start", startUtc, childProcessId,
                    "\"processObserver\":\"JobObjectCompletionPort+Toolhelp32Snapshot\"");
                while (true)
                {
                    bool childExited = child.HasExited;
                    DateTime nowUtc = DateTime.UtcNow;
                    if (childExited && !exitDetectedUtc.HasValue)
                    {
                        exitDetectedUtc = nowUtc;
                        WriteEvent(log, "child-exit-detected", nowUtc, childProcessId,
                            "\"exitCode\":" + child.ExitCode.ToString(CultureInfo.InvariantCulture));
                    }

                    List<ProcessRecord> starts = processObserver.Drain();
                    Dictionary<int, ProcessRecord> processSnapshot = SnapshotProcesses("Toolhelp32Snapshot");
                    foreach (ProcessRecord process in starts)
                    {
                        knownParents[process.ProcessId] = process.ParentProcessId;
                        MergeProcessCandidate(pendingProcesses, finalizedProcessIds, process);
                    }
                    foreach (ProcessRecord process in processSnapshot.Values)
                    {
                        knownParents[process.ProcessId] = process.ParentProcessId;
                        if (process.FirstDetectedUtc == DateTime.MinValue) process.FirstDetectedUtc = nowUtc;
                        MergeProcessCandidate(pendingProcesses, finalizedProcessIds, process);
                    }
                    parents = ParentMap(processSnapshot);

                    List<int> completedProcesses = new List<int>();
                    foreach (KeyValuePair<int, ProcessRecord> pair in pendingProcesses)
                    {
                        ProcessRecord process = pair.Value;
                        if (process.ProcessId == childProcessId)
                        {
                            completedProcesses.Add(pair.Key);
                            continue;
                        }
                        bool resolved;
                        bool ancestryAssociated = ResolveChildAncestry(process.ProcessId, childProcessId,
                            knownParents, baselineProcessIds, process.Ancestry, out resolved);
                        bool associated = process.JobAssociated || ancestryAssociated;
                        if (!associated)
                        {
                            if (resolved) completedProcesses.Add(pair.Key);
                            continue;
                        }
                        if (!process.JobAssociated &&
                            (nowUtc - process.FirstDetectedUtc).TotalMilliseconds < 250)
                            continue;
                        ProcessRecord live;
                        if (processSnapshot.TryGetValue(process.ProcessId, out live))
                            EnrichProcessRecord(process, live);
                        if (process.JobAssociated && process.Ancestry.Count == 0)
                            process.Ancestry.Add(childProcessId);
                        process.ChildExited = childExited;
                        process.AssociatedWithChild = true;
                        process.ErrorReportingProcess = IsErrorReportingProcess(process.ProcessName);
                        process.UnidentifiedJobDescendant = process.JobAssociated &&
                            String.IsNullOrEmpty(process.ProcessName) && String.IsNullOrEmpty(process.Executable);
                        process.CustomMatch = Contains(process.ProcessName, options.ProcessMatch) ||
                            Contains(process.Executable, options.ProcessMatch);
                        process.Suspicious = process.ErrorReportingProcess || process.CustomMatch ||
                            process.UnidentifiedJobDescendant;
                        process.Reason = ProcessObservationReason(process, options.ProcessMatch);
                        WriteProcessEvent(log, "process-detected", process.FirstDetectedUtc,
                            childProcessId, process);
                        if (process.Suspicious) anySuspiciousProcess = true;
                        if (process.CustomMatch) expectedProcessMatchSeen = true;
                        if (process.ErrorReportingProcess || process.UnidentifiedJobDescendant ||
                            (process.CustomMatch && !options.ExpectProcessMatch))
                            anyUnexpectedSuspicious = true;
                        completedProcesses.Add(pair.Key);
                    }
                    foreach (int processId in completedProcesses)
                    {
                        pendingProcesses.Remove(processId);
                        finalizedProcessIds.Add(processId);
                    }

                    List<WindowRecord> windows = EnumerateWindows(childExited, childProcessId, parents, options.Match);
                    HashSet<IntPtr> current = new HashSet<IntPtr>();
                    foreach (WindowRecord window in windows)
                    {
                        current.Add(window.Handle);
                        WindowRecord previous;
                        if (observed.TryGetValue(window.Handle, out previous))
                        {
                            window.FirstDetectedUtc = previous.FirstDetectedUtc;
                            observed[window.Handle] = window;
                            bool newlyBuiltInSuspicious = !previous.BuiltInSuspicious && window.BuiltInSuspicious;
                            bool newlyCustomMatched = !previous.CustomMatch && window.CustomMatch;
                            if (!newlyBuiltInSuspicious && !newlyCustomMatched) continue;
                            bool updatedSinceBaseline = !baseline.Contains(window.Handle);
                            if (window.Suspicious && updatedSinceBaseline)
                            {
                                anySuspiciousWindow = true;
                                if (window.CustomMatch) expectedMatchSeen = true;
                                if (window.BuiltInSuspicious ||
                                    (window.CustomMatch && !options.ExpectMatch))
                                    anyUnexpectedSuspicious = true;
                            }
                            WriteWindowEvent(log, "window-updated", nowUtc, childProcessId, window);
                            continue;
                        }
                        window.FirstDetectedUtc = nowUtc;
                        observed.Add(window.Handle, window);
                        bool newSinceBaseline = !baseline.Contains(window.Handle);
                        bool ownedByChild = window.ProcessId == childProcessId;
                        if (!newSinceBaseline && !ownedByChild && !window.Suspicious) continue;
                        if (window.Suspicious && newSinceBaseline)
                        {
                            anySuspiciousWindow = true;
                            if (window.CustomMatch) expectedMatchSeen = true;
                            if (window.BuiltInSuspicious ||
                                (window.CustomMatch && !options.ExpectMatch))
                                anyUnexpectedSuspicious = true;
                        }
                        WriteWindowEvent(log, newSinceBaseline ? "window-detected" : "preexisting-window",
                            nowUtc, childProcessId, window);
                    }

                    List<IntPtr> closed = new List<IntPtr>();
                    foreach (KeyValuePair<IntPtr, WindowRecord> pair in observed)
                        if (!current.Contains(pair.Key)) closed.Add(pair.Key);
                    foreach (IntPtr handle in closed)
                    {
                        WindowRecord previous = observed[handle];
                        WriteWindowEvent(log, "window-closed", nowUtc, childProcessId, previous);
                        observed.Remove(handle);
                    }

                    if (exitDetectedUtc.HasValue &&
                        (nowUtc - exitDetectedUtc.Value).TotalMilliseconds >= options.PostExitMilliseconds)
                        break;
                    if (!exitDetectedUtc.HasValue &&
                        (nowUtc - startUtc).TotalMilliseconds >= options.TimeoutMilliseconds)
                    {
                        timedOut = true;
                        WriteEvent(log, "monitor-timeout", nowUtc, childProcessId,
                            "\"childStillRunning\":true");
                        break;
                    }
                    Thread.Sleep(options.PollMilliseconds);
                }

                if (child.HasExited) child.WaitForExit();
                string stdout;
                string stderr;
                lock (standardOutput) stdout = standardOutput.ToString();
                lock (standardError) stderr = standardError.ToString();
                WriteEvent(log, "monitor-complete", DateTime.UtcNow, childProcessId,
                    "\"childExited\":" + Bool(child.HasExited) +
                    ",\"exitCode\":" + (child.HasExited ? child.ExitCode.ToString(CultureInfo.InvariantCulture) : "null") +
                    ",\"suspiciousWindowDetected\":" + Bool(anySuspiciousWindow) +
                    ",\"suspiciousProcessDetected\":" + Bool(anySuspiciousProcess) +
                    ",\"unexpectedSuspiciousActivityDetected\":" + Bool(anyUnexpectedSuspicious) +
                    ",\"expectedMatchDetected\":" + Bool(expectedMatchSeen) +
                    ",\"expectedProcessMatchDetected\":" + Bool(expectedProcessMatchSeen) +
                    ",\"stdout\":\"" + Escape(stdout) + "\",\"stderr\":\"" + Escape(stderr) + "\"");
                if (!String.IsNullOrEmpty(stdout)) Console.Out.Write(stdout);
                if (!String.IsNullOrEmpty(stderr)) Console.Error.Write(stderr);
            }

            if (timedOut)
            {
                Console.Error.WriteLine("Child PID {0} remained active after {1} ms; it was left running for inspection.",
                    childProcessId, options.TimeoutMilliseconds);
                return 4;
            }
            if (options.ExpectMatch && !expectedMatchSeen)
            {
                Console.Error.WriteLine("Expected window/text detector match was not observed.");
                return 5;
            }
            if (options.ExpectProcessMatch && !expectedProcessMatchSeen)
            {
                Console.Error.WriteLine("Expected descendant-process detector match was not observed.");
                return 6;
            }
            if (anyUnexpectedSuspicious)
            {
                Console.Error.WriteLine("Suspicious native window or associated process detected; see " + options.OutputPath);
                return 3;
            }
            if (child.ExitCode != 0) return child.ExitCode;
            Console.WriteLine("Clean monitored exit; log: " + options.OutputPath);
            return 0;
        }
    }

    private static List<WindowRecord> EnumerateWindows(bool childExited, int childProcessId,
        Dictionary<int, int> parents, string customMatch)
    {
        List<WindowRecord> windows = new List<WindowRecord>();
        EnumWindows(delegate(IntPtr handle, IntPtr parameter)
        {
            uint processId;
            GetWindowThreadProcessId(handle, out processId);
            WindowRecord record = new WindowRecord();
            record.Handle = handle;
            record.ProcessId = unchecked((int)processId);
            int parent;
            record.ParentProcessId = parents.TryGetValue(record.ProcessId, out parent) ? parent : 0;
            record.Title = ReadWindowText(handle);
            record.ClassName = ReadClassName(handle);
            record.DialogText = ReadDescendantText(handle);
            record.Visible = IsWindowVisible(handle);
            record.ChildExited = childExited;
            ReadProcess(record);
            record.Reason = ObservationReason(record, childProcessId, customMatch);
            record.CustomMatch = !String.IsNullOrEmpty(customMatch) && Contains(record, customMatch);
            record.BuiltInSuspicious = IsBuiltInSuspicious(record);
            record.Suspicious = record.BuiltInSuspicious || record.CustomMatch;
            windows.Add(record);
            return true;
        }, IntPtr.Zero);
        return windows;
    }

    private static string ObservationReason(WindowRecord window, int childProcessId, string customMatch)
    {
        List<string> reasons = new List<string>();
        if (window.ProcessId == childProcessId) reasons.Add("child-owned");
        string process = window.ProcessName ?? String.Empty;
        if (IsErrorReportingProcess(process))
            reasons.Add("error-reporting-process");
        foreach (string term in BuiltInTerms)
            if (Contains(window, term)) reasons.Add("match:" + term);
        if (!String.IsNullOrEmpty(customMatch) && Contains(window, customMatch))
            reasons.Add("custom-match:" + customMatch);
        return String.Join(";", reasons.ToArray());
    }

    private static bool IsBuiltInSuspicious(WindowRecord window)
    {
        string process = window.ProcessName ?? String.Empty;
        if (IsErrorReportingProcess(process)) return true;
        if (Contains(window, "unknown software exception") || Contains(window, "0xC0020001"))
            return true;
        if (window.Visible && Contains(window, ".NET-BroadcastEventWindow")) return true;
        if (window.Visible && window.ClassName == "#32770" &&
            (Contains(window, "MidiBottleneck.Tests") || Contains(window, "exception") ||
             Contains(window, "Windows Error Reporting"))) return true;
        return false;
    }

    private static bool Contains(WindowRecord window, string term)
    {
        return Contains(window.Title, term) || Contains(window.ClassName, term) ||
            Contains(window.DialogText, term) || Contains(window.Executable, term) ||
            Contains(window.ProcessName, term);
    }

    private static bool Contains(string value, string term)
    {
        return !String.IsNullOrEmpty(value) && !String.IsNullOrEmpty(term) &&
            value.IndexOf(term, StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private static string ReadWindowText(IntPtr window)
    {
        StringBuilder text = new StringBuilder(2048);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }

    private static string ReadClassName(IntPtr window)
    {
        StringBuilder text = new StringBuilder(512);
        GetClassName(window, text, text.Capacity);
        return text.ToString();
    }

    private static string ReadDescendantText(IntPtr parent)
    {
        List<string> parts = new List<string>();
        EnumChildWindows(parent, delegate(IntPtr child, IntPtr parameter)
        {
            StringBuilder text = new StringBuilder(4096);
            IntPtr result;
            SendMessageTimeout(child, WmGetText, new IntPtr(text.Capacity), text,
                SmtoAbortIfHung, 100, out result);
            string value = text.ToString().Trim();
            if (!String.IsNullOrEmpty(value) && !parts.Contains(value)) parts.Add(value);
            return true;
        }, IntPtr.Zero);
        return String.Join(" | ", parts.ToArray());
    }

    private static void ReadProcess(WindowRecord record)
    {
        string executable;
        string processName;
        string architecture;
        ReadProcessDetails(record.ProcessId, out executable, out processName, out architecture);
        record.Executable = executable;
        record.ProcessName = processName;
        record.Architecture = architecture;
    }

    private static void ReadProcessDetails(int processId, out string executable,
        out string processName, out string architecture)
    {
        executable = null;
        processName = null;
        architecture = "unknown";
        IntPtr process = OpenProcess(ProcessQueryLimitedInformation | ProcessQueryInformation,
            false, unchecked((uint)processId));
        if (process == IntPtr.Zero)
            return;
        try
        {
            StringBuilder path = new StringBuilder(32768);
            int length = path.Capacity;
            if (QueryFullProcessImageName(process, 0, path, ref length))
            {
                executable = path.ToString();
                processName = Path.GetFileNameWithoutExtension(executable);
            }
            bool wow64;
            if (Environment.Is64BitOperatingSystem && IsWow64Process(process, out wow64))
                architecture = wow64 ? "x86" : "x64";
            else if (!Environment.Is64BitOperatingSystem)
                architecture = "x86";
        }
        finally
        {
            CloseHandle(process);
        }
    }

    private static Dictionary<int, ProcessRecord> SnapshotProcesses(string source)
    {
        Dictionary<int, ProcessRecord> result = new Dictionary<int, ProcessRecord>();
        IntPtr snapshot = CreateToolhelp32Snapshot(Th32csSnapProcess, 0);
        if (snapshot == new IntPtr(-1)) return result;
        try
        {
            ProcessEntry32 entry = new ProcessEntry32();
            entry.Size = unchecked((uint)Marshal.SizeOf(typeof(ProcessEntry32)));
            if (!Process32First(snapshot, ref entry)) return result;
            do
            {
                ProcessRecord record = new ProcessRecord();
                record.ProcessId = unchecked((int)entry.ProcessId);
                record.ParentProcessId = unchecked((int)entry.ParentProcessId);
                record.ProcessName = Path.GetFileNameWithoutExtension(entry.ExeFile ?? String.Empty);
                record.DetectionSource = source;
                result[record.ProcessId] = record;
                entry.Size = unchecked((uint)Marshal.SizeOf(typeof(ProcessEntry32)));
            }
            while (Process32Next(snapshot, ref entry));
        }
        finally
        {
            CloseHandle(snapshot);
        }
        return result;
    }

    private static Dictionary<int, int> ParentMap(Dictionary<int, ProcessRecord> processes)
    {
        Dictionary<int, int> result = new Dictionary<int, int>();
        foreach (ProcessRecord process in processes.Values)
            result[process.ProcessId] = process.ParentProcessId;
        return result;
    }

    private static void MergeProcessCandidate(Dictionary<int, ProcessRecord> pending,
        HashSet<int> finalized, ProcessRecord incoming)
    {
        if (finalized.Contains(incoming.ProcessId)) return;
        ProcessRecord existing;
        if (!pending.TryGetValue(incoming.ProcessId, out existing))
        {
            if (incoming.FirstDetectedUtc == DateTime.MinValue)
                incoming.FirstDetectedUtc = DateTime.UtcNow;
            pending.Add(incoming.ProcessId, incoming);
            return;
        }
        EnrichProcessRecord(existing, incoming);
        existing.JobAssociated = existing.JobAssociated || incoming.JobAssociated;
        if (incoming.FirstDetectedUtc != DateTime.MinValue &&
            incoming.FirstDetectedUtc < existing.FirstDetectedUtc)
            existing.FirstDetectedUtc = incoming.FirstDetectedUtc;
        if (existing.ParentProcessId == 0) existing.ParentProcessId = incoming.ParentProcessId;
        if (String.IsNullOrEmpty(existing.DetectionSource)) existing.DetectionSource = incoming.DetectionSource;
        else if (!Contains(existing.DetectionSource, incoming.DetectionSource))
            existing.DetectionSource += "+" + incoming.DetectionSource;
    }

    private static void EnrichProcessRecord(ProcessRecord target, ProcessRecord source)
    {
        if (String.IsNullOrEmpty(target.ProcessName)) target.ProcessName = source.ProcessName;
        if (String.IsNullOrEmpty(target.Executable)) target.Executable = source.Executable;
        if (String.IsNullOrEmpty(target.Architecture) || target.Architecture == "unknown")
            target.Architecture = source.Architecture;
        if (String.IsNullOrEmpty(target.Executable) || String.IsNullOrEmpty(target.Architecture) ||
            target.Architecture == "unknown")
        {
            string executable;
            string processName;
            string architecture;
            ReadProcessDetails(target.ProcessId, out executable, out processName, out architecture);
            if (!String.IsNullOrEmpty(executable)) target.Executable = executable;
            if (!String.IsNullOrEmpty(processName)) target.ProcessName = processName;
            if (!String.IsNullOrEmpty(architecture)) target.Architecture = architecture;
        }
        if (String.IsNullOrEmpty(target.Architecture)) target.Architecture = "unknown";
    }

    private static bool ResolveChildAncestry(int processId, int childProcessId,
        Dictionary<int, int> knownParents, HashSet<int> baselineProcessIds,
        List<int> ancestry, out bool resolved)
    {
        ancestry.Clear();
        HashSet<int> visited = new HashSet<int>();
        int current = processId;
        while (visited.Add(current))
        {
            int parent;
            if (!knownParents.TryGetValue(current, out parent))
            {
                resolved = false;
                return false;
            }
            ancestry.Add(parent);
            if (parent == childProcessId)
            {
                resolved = true;
                return true;
            }
            if (parent == 0 || baselineProcessIds.Contains(parent))
            {
                resolved = true;
                return false;
            }
            current = parent;
        }
        resolved = true;
        return false;
    }

    private static bool IsErrorReportingProcess(string processName)
    {
        string normalized = Path.GetFileNameWithoutExtension(processName ?? String.Empty);
        foreach (string candidate in ErrorReportingProcessNames)
            if (normalized.Equals(candidate, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    private static string ProcessObservationReason(ProcessRecord process, string customMatch)
    {
        List<string> reasons = new List<string>();
        reasons.Add("descendant-of-monitored-child");
        if (process.JobAssociated) reasons.Add("job-object-notification");
        if (process.ErrorReportingProcess) reasons.Add("error-reporting-process");
        if (process.UnidentifiedJobDescendant) reasons.Add("unidentified-short-lived-descendant");
        if (process.CustomMatch) reasons.Add("custom-process-match:" + customMatch);
        return String.Join(";", reasons.ToArray());
    }

    private static void WriteWindowEvent(StreamWriter log, string eventName, DateTime timeUtc,
        int childProcessId, WindowRecord window)
    {
        string fields =
            "\"handle\":\"0x" + window.Handle.ToInt64().ToString("X", CultureInfo.InvariantCulture) + "\"" +
            ",\"title\":\"" + Escape(window.Title) + "\"" +
            ",\"class\":\"" + Escape(window.ClassName) + "\"" +
            ",\"dialogText\":\"" + Escape(window.DialogText) + "\"" +
            ",\"ownerPid\":" + window.ProcessId.ToString(CultureInfo.InvariantCulture) +
            ",\"ownerParentPid\":" + window.ParentProcessId.ToString(CultureInfo.InvariantCulture) +
            ",\"ownerExecutable\":\"" + Escape(window.Executable) + "\"" +
            ",\"ownerProcess\":\"" + Escape(window.ProcessName) + "\"" +
            ",\"architecture\":\"" + Escape(window.Architecture) + "\"" +
            ",\"visible\":" + Bool(window.Visible) +
            ",\"originalChildExited\":" + Bool(window.ChildExited) +
            ",\"suspicious\":" + Bool(window.Suspicious) +
            ",\"builtInSuspicious\":" + Bool(window.BuiltInSuspicious) +
            ",\"customMatch\":" + Bool(window.CustomMatch) +
            ",\"reason\":\"" + Escape(window.Reason) + "\"";
        WriteEvent(log, eventName, timeUtc, childProcessId, fields);
    }

    private static void WriteProcessEvent(StreamWriter log, string eventName, DateTime timeUtc,
        int childProcessId, ProcessRecord process)
    {
        StringBuilder ancestry = new StringBuilder();
        ancestry.Append('[');
        for (int i = 0; i < process.Ancestry.Count; i++)
        {
            if (i != 0) ancestry.Append(',');
            ancestry.Append(process.Ancestry[i].ToString(CultureInfo.InvariantCulture));
        }
        ancestry.Append(']');
        string fields =
            "\"pid\":" + process.ProcessId.ToString(CultureInfo.InvariantCulture) +
            ",\"parentPid\":" + process.ParentProcessId.ToString(CultureInfo.InvariantCulture) +
            ",\"ancestry\":" + ancestry.ToString() +
            ",\"processName\":\"" + Escape(process.ProcessName) + "\"" +
            ",\"executable\":\"" + Escape(process.Executable) + "\"" +
            ",\"architecture\":\"" + Escape(process.Architecture) + "\"" +
            ",\"detectionSource\":\"" + Escape(process.DetectionSource) + "\"" +
            ",\"jobAssociated\":" + Bool(process.JobAssociated) +
            ",\"associatedWithChild\":" + Bool(process.AssociatedWithChild) +
            ",\"originalChildExited\":" + Bool(process.ChildExited) +
            ",\"errorReportingProcess\":" + Bool(process.ErrorReportingProcess) +
            ",\"unidentifiedJobDescendant\":" + Bool(process.UnidentifiedJobDescendant) +
            ",\"customMatch\":" + Bool(process.CustomMatch) +
            ",\"suspicious\":" + Bool(process.Suspicious) +
            ",\"reason\":\"" + Escape(process.Reason) + "\"";
        WriteEvent(log, eventName, timeUtc, childProcessId, fields);
    }

    private static void WriteEvent(StreamWriter log, string eventName, DateTime timeUtc,
        int childProcessId, string fields)
    {
        log.Write("{\"event\":\"");
        log.Write(Escape(eventName));
        log.Write("\",\"detectionTimeUtc\":\"");
        log.Write(timeUtc.ToString("o", CultureInfo.InvariantCulture));
        log.Write("\",\"childPid\":");
        log.Write(childProcessId.ToString(CultureInfo.InvariantCulture));
        if (!String.IsNullOrEmpty(fields))
        {
            log.Write(',');
            log.Write(fields);
        }
        log.WriteLine("}");
    }

    private static Options Parse(string[] arguments)
    {
        Options options = new Options();
        int separator = Array.IndexOf(arguments, "--");
        if (separator < 0 || separator + 1 >= arguments.Length)
            throw new ArgumentException("Usage: NativeWindowMonitor --output <jsonl> [--post-exit-ms N] [--timeout-ms N] [--match text] [--expect-match] [--process-match text] [--expect-process-match] -- <executable> [arguments]");
        for (int i = 0; i < separator; i++)
        {
            string argument = arguments[i];
            if (argument == "--output" && ++i < separator) options.OutputPath = arguments[i];
            else if (argument == "--post-exit-ms" && ++i < separator) options.PostExitMilliseconds = ParsePositive(arguments[i], argument);
            else if (argument == "--poll-ms" && ++i < separator) options.PollMilliseconds = ParsePositive(arguments[i], argument);
            else if (argument == "--timeout-ms" && ++i < separator) options.TimeoutMilliseconds = ParsePositive(arguments[i], argument);
            else if (argument == "--match" && ++i < separator) options.Match = arguments[i];
            else if (argument == "--expect-match") options.ExpectMatch = true;
            else if (argument == "--process-match" && ++i < separator) options.ProcessMatch = arguments[i];
            else if (argument == "--expect-process-match") options.ExpectProcessMatch = true;
            else throw new ArgumentException("Unknown or incomplete monitor option: " + argument);
        }
        if (String.IsNullOrEmpty(options.OutputPath)) throw new ArgumentException("--output is required.");
        if (options.ExpectMatch && String.IsNullOrEmpty(options.Match))
            throw new ArgumentException("--expect-match requires --match.");
        if (options.ExpectProcessMatch && String.IsNullOrEmpty(options.ProcessMatch))
            throw new ArgumentException("--expect-process-match requires --process-match.");
        options.Executable = arguments[separator + 1];
        for (int i = separator + 2; i < arguments.Length; i++) options.Arguments.Add(arguments[i]);
        return options;
    }

    private static int ParsePositive(string value, string name)
    {
        int parsed;
        if (!Int32.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out parsed) || parsed <= 0)
            throw new ArgumentException(name + " requires a positive integer.");
        return parsed;
    }

    private static string QuoteArguments(List<string> arguments)
    {
        List<string> quoted = new List<string>();
        foreach (string argument in arguments) quoted.Add(QuoteArgument(argument));
        return String.Join(" ", quoted.ToArray());
    }

    private static string QuoteArgument(string argument)
    {
        if (argument.Length > 0 && argument.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return argument;
        StringBuilder result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char character in argument)
        {
            if (character == '\\') { slashes++; continue; }
            if (character == '"')
            {
                result.Append('\\', slashes * 2 + 1);
                result.Append('"');
                slashes = 0;
                continue;
            }
            result.Append('\\', slashes);
            slashes = 0;
            result.Append(character);
        }
        result.Append('\\', slashes * 2);
        result.Append('"');
        return result.ToString();
    }

    private static string Escape(string value)
    {
        if (value == null) return String.Empty;
        StringBuilder result = new StringBuilder(value.Length + 16);
        foreach (char character in value)
        {
            switch (character)
            {
                case '\\': result.Append("\\\\"); break;
                case '"': result.Append("\\\""); break;
                case '\r': result.Append("\\r"); break;
                case '\n': result.Append("\\n"); break;
                case '\t': result.Append("\\t"); break;
                default:
                    if (character < 0x20) result.Append("\\u" + ((int)character).ToString("X4"));
                    else result.Append(character);
                    break;
            }
        }
        return result.ToString();
    }

    private static string Bool(bool value) { return value ? "true" : "false"; }
}
