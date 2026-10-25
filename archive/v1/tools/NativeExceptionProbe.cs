using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class NativeExceptionProbe
{
    private const uint DebugOnlyThisProcess = 0x00000002;
    private const uint DebugContinue = 0x00010002;
    private const uint DebugExceptionNotHandled = 0x80010001;
    private const uint ExceptionDebugEvent = 1;
    private const uint CreateProcessDebugEvent = 3;
    private const uint ExitProcessDebugEvent = 5;
    private const uint LoadDllDebugEvent = 6;
    private const uint UnloadDllDebugEvent = 7;
    private const uint ExceptionBreakpoint = 0x80000003;
    private const uint ExceptionSingleStep = 0x80000004;
    private const uint TargetException = 0xC0020001;

    [Flags]
    private enum MiniDumpType : uint
    {
        Normal = 0,
        WithDataSegs = 0x00000001,
        WithHandleData = 0x00000004,
        WithUnloadedModules = 0x00000020,
        WithIndirectlyReferencedMemory = 0x00000040,
        WithThreadInfo = 0x00001000
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public uint Size;
        public string Reserved;
        public string Desktop;
        public string Title;
        public uint X;
        public uint Y;
        public uint XSize;
        public uint YSize;
        public uint XCountChars;
        public uint YCountChars;
        public uint FillAttribute;
        public uint Flags;
        public ushort ShowWindow;
        public ushort Reserved2Size;
        public IntPtr Reserved2;
        public IntPtr StandardInput;
        public IntPtr StandardOutput;
        public IntPtr StandardError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr Process;
        public IntPtr Thread;
        public uint ProcessId;
        public uint ThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private unsafe struct ExceptionRecord
    {
        public uint Code;
        public uint Flags;
        public IntPtr NestedRecord;
        public IntPtr Address;
        public uint ParameterCount;
        public fixed ulong Information[15];
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ExceptionDebugInfo
    {
        public ExceptionRecord Record;
        public uint FirstChance;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct CreateProcessDebugInfo
    {
        public IntPtr File;
        public IntPtr Process;
        public IntPtr Thread;
        public IntPtr ImageBase;
        public uint DebugInfoFileOffset;
        public uint DebugInfoSize;
        public IntPtr ThreadLocalBase;
        public IntPtr StartAddress;
        public IntPtr ImageName;
        public ushort Unicode;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ExitProcessDebugInfo { public uint ExitCode; }

    [StructLayout(LayoutKind.Sequential)]
    private struct LoadDllDebugInfo
    {
        public IntPtr File;
        public IntPtr Base;
        public uint DebugInfoFileOffset;
        public uint DebugInfoSize;
        public IntPtr ImageName;
        public ushort Unicode;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct UnloadDllDebugInfo { public IntPtr Base; }

    [StructLayout(LayoutKind.Explicit)]
    private struct DebugEventUnion
    {
        [FieldOffset(0)] public ExceptionDebugInfo Exception;
        [FieldOffset(0)] public CreateProcessDebugInfo CreateProcess;
        [FieldOffset(0)] public ExitProcessDebugInfo ExitProcess;
        [FieldOffset(0)] public LoadDllDebugInfo LoadDll;
        [FieldOffset(0)] public UnloadDllDebugInfo UnloadDll;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DebugEvent
    {
        public uint Code;
        public uint ProcessId;
        public uint ThreadId;
        public DebugEventUnion Information;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcess(string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint creationFlags,
        IntPtr environment, string currentDirectory, ref StartupInfo startupInfo,
        out ProcessInformation processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WaitForDebugEvent(out DebugEvent debugEvent, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ContinueDebugEvent(uint processId, uint threadId, uint continueStatus);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DebugSetProcessKillOnExit(bool killOnExit);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(IntPtr file, StringBuilder path,
        uint capacity, uint flags);

    [DllImport("dbghelp.dll", SetLastError = true)]
    private static extern bool MiniDumpWriteDump(IntPtr process, uint processId, IntPtr file,
        MiniDumpType dumpType, IntPtr exception, IntPtr userStream, IntPtr callback);

    private static int Main(string[] arguments)
    {
        if (arguments.Length < 5 || arguments[0] != "--log" || arguments[2] != "--dump")
        {
            Console.Error.WriteLine("Usage: NativeExceptionProbe --log <jsonl> --dump <dmp> -- <executable> [arguments]");
            return 2;
        }
        int separator = Array.IndexOf(arguments, "--");
        if (separator != 4 || separator + 1 >= arguments.Length)
        {
            Console.Error.WriteLine("The executable must follow --.");
            return 2;
        }

        string logPath = Path.GetFullPath(arguments[1]);
        string dumpPath = Path.GetFullPath(arguments[3]);
        string executable = Path.GetFullPath(arguments[separator + 1]);
        List<string> childArguments = new List<string>();
        for (int i = separator + 2; i < arguments.Length; i++) childArguments.Add(arguments[i]);
        Directory.CreateDirectory(Path.GetDirectoryName(logPath));
        Directory.CreateDirectory(Path.GetDirectoryName(dumpPath));

        StartupInfo startup = new StartupInfo();
        startup.Size = unchecked((uint)Marshal.SizeOf(typeof(StartupInfo)));
        ProcessInformation process;
        StringBuilder commandLine = new StringBuilder(Quote(executable) +
            (childArguments.Count == 0 ? String.Empty : " " + QuoteArguments(childArguments)));
        if (!CreateProcess(executable, commandLine, IntPtr.Zero, IntPtr.Zero, false,
            DebugOnlyThisProcess, IntPtr.Zero, Environment.CurrentDirectory, ref startup, out process))
        {
            Console.Error.WriteLine("CreateProcess failed: " + new Win32Exception(Marshal.GetLastWin32Error()).Message);
            return 3;
        }

        DebugSetProcessKillOnExit(false);
        SortedDictionary<ulong, string> modules = new SortedDictionary<ulong, string>();
        bool dumpWritten = false;
        uint exitCode = UInt32.MaxValue;
        using (StreamWriter log = new StreamWriter(logPath, false, new UTF8Encoding(false)))
        {
            log.AutoFlush = true;
            Write(log, "probe-start", "\"processId\":" + process.ProcessId +
                ",\"executable\":\"" + Escape(executable) + "\"");
            try
            {
                bool active = true;
                while (active)
                {
                    DebugEvent debugEvent;
                    if (!WaitForDebugEvent(out debugEvent, UInt32.MaxValue))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "WaitForDebugEvent failed.");
                    uint continuation = DebugContinue;
                    if (debugEvent.Code == CreateProcessDebugEvent)
                    {
                        string path = PathFromHandle(debugEvent.Information.CreateProcess.File);
                        modules[Pointer(debugEvent.Information.CreateProcess.ImageBase)] = path;
                        Write(log, "process-created", "\"threadId\":" + debugEvent.ThreadId +
                            ",\"imageBase\":\"0x" + Pointer(debugEvent.Information.CreateProcess.ImageBase).ToString("X") +
                            "\",\"image\":\"" + Escape(path) + "\"");
                        CloseIfValid(debugEvent.Information.CreateProcess.File);
                    }
                    else if (debugEvent.Code == LoadDllDebugEvent)
                    {
                        string path = PathFromHandle(debugEvent.Information.LoadDll.File);
                        ulong address = Pointer(debugEvent.Information.LoadDll.Base);
                        modules[address] = path;
                        Write(log, "module-loaded", "\"base\":\"0x" + address.ToString("X") +
                            "\",\"module\":\"" + Escape(path) + "\"");
                        CloseIfValid(debugEvent.Information.LoadDll.File);
                    }
                    else if (debugEvent.Code == UnloadDllDebugEvent)
                    {
                        modules.Remove(Pointer(debugEvent.Information.UnloadDll.Base));
                    }
                    else if (debugEvent.Code == ExceptionDebugEvent)
                    {
                        ExceptionDebugInfo exception = debugEvent.Information.Exception;
                        uint code = exception.Record.Code;
                        ulong address = Pointer(exception.Record.Address);
                        string module = FindModule(modules, address);
                        Write(log, "exception", "\"threadId\":" + debugEvent.ThreadId +
                            ",\"code\":\"0x" + code.ToString("X8") +
                            "\",\"address\":\"0x" + address.ToString("X") +
                            "\",\"firstChance\":" + (exception.FirstChance != 0 ? "true" : "false") +
                            ",\"nearestModule\":\"" + Escape(module) + "\"");
                        if (code == TargetException && !dumpWritten)
                        {
                            dumpWritten = WriteDump(process.Process, process.ProcessId, dumpPath);
                            Write(log, "dump", "\"path\":\"" + Escape(dumpPath) +
                                "\",\"written\":" + (dumpWritten ? "true" : "false") +
                                ",\"lastError\":" + Marshal.GetLastWin32Error());
                        }
                        continuation = (code == ExceptionBreakpoint || code == ExceptionSingleStep)
                            ? DebugContinue : DebugExceptionNotHandled;
                    }
                    else if (debugEvent.Code == ExitProcessDebugEvent)
                    {
                        exitCode = debugEvent.Information.ExitProcess.ExitCode;
                        Write(log, "process-exit", "\"exitCode\":" + exitCode);
                        active = false;
                    }

                    if (!ContinueDebugEvent(debugEvent.ProcessId, debugEvent.ThreadId, continuation))
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "ContinueDebugEvent failed.");
                }
            }
            finally
            {
                CloseIfValid(process.Thread);
                CloseIfValid(process.Process);
            }
            Write(log, "probe-complete", "\"targetExceptionObserved\":" + (dumpWritten ? "true" : "false") +
                ",\"exitCode\":" + exitCode);
        }

        Console.WriteLine("Native exception log: " + logPath);
        if (dumpWritten) Console.WriteLine("Native dump: " + dumpPath);
        return dumpWritten ? 0 : 6;
    }

    private static bool WriteDump(IntPtr process, uint processId, string path)
    {
        using (FileStream file = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            MiniDumpType type = MiniDumpType.WithDataSegs | MiniDumpType.WithHandleData |
                MiniDumpType.WithUnloadedModules | MiniDumpType.WithIndirectlyReferencedMemory |
                MiniDumpType.WithThreadInfo;
            return MiniDumpWriteDump(process, processId, file.SafeFileHandle.DangerousGetHandle(),
                type, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        }
    }

    private static string FindModule(SortedDictionary<ulong, string> modules, ulong address)
    {
        ulong nearest = 0;
        string path = String.Empty;
        foreach (KeyValuePair<ulong, string> module in modules)
        {
            if (module.Key > address) break;
            nearest = module.Key;
            path = module.Value;
        }
        return String.IsNullOrEmpty(path) ? String.Empty : path + "+0x" + (address - nearest).ToString("X");
    }

    private static string PathFromHandle(IntPtr file)
    {
        if (file == IntPtr.Zero || file == new IntPtr(-1)) return String.Empty;
        StringBuilder path = new StringBuilder(32768);
        uint length = GetFinalPathNameByHandle(file, path, unchecked((uint)path.Capacity), 0);
        if (length == 0 || length >= path.Capacity) return String.Empty;
        string value = path.ToString();
        return value.StartsWith("\\\\?\\", StringComparison.Ordinal) ? value.Substring(4) : value;
    }

    private static void CloseIfValid(IntPtr handle)
    {
        if (handle != IntPtr.Zero && handle != new IntPtr(-1)) CloseHandle(handle);
    }

    private static ulong Pointer(IntPtr value)
    {
        return unchecked((ulong)value.ToInt64());
    }

    private static string QuoteArguments(List<string> arguments)
    {
        List<string> quoted = new List<string>();
        foreach (string argument in arguments) quoted.Add(Quote(argument));
        return String.Join(" ", quoted.ToArray());
    }

    private static string Quote(string argument)
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

    private static void Write(StreamWriter log, string eventName, string fields)
    {
        log.Write("{\"event\":\"");
        log.Write(Escape(eventName));
        log.Write("\",\"timeUtc\":\"");
        log.Write(DateTime.UtcNow.ToString("o", CultureInfo.InvariantCulture));
        log.Write('"');
        if (!String.IsNullOrEmpty(fields)) { log.Write(','); log.Write(fields); }
        log.WriteLine("}");
    }

    private static string Escape(string value)
    {
        if (value == null) return String.Empty;
        return value.Replace("\\", "\\\\").Replace("\"", "\\\"")
            .Replace("\r", "\\r").Replace("\n", "\\n").Replace("\t", "\\t");
    }
}
