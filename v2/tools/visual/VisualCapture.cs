// Visual capture helper for the v2 visual gate (loaded by v2\tools\visual\*.ps1 with Add-Type).
// Microsoft toolchain only: Win32 through P/Invoke, PNG writing through System.Drawing.
//
// Capture method (valid): run the program on a private hidden desktop (HiddenDesktop in VisualDriver.cs; see
// visual-gate.ps1). Windows there are painted like on a monitor, PrintWindow(PW_RENDERFULLCONTENT) returns those
// pixels, and nothing reaches any monitor.
//
// INVALID - do not use: Start(minimized) + Offscreen()/MoveOffscreen(). A window beyond every monitor is not
// painted at all (blank capture); computed from a DPI-unaware process the "beyond the right edge" position lands
// on a real monitor (windows flash there); and a minimized-then-restored window paints differently from a window
// shown normally (group interiors came out grey where the real window showed white). Kept only so older notes
// still compile; the visual gate never calls them.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class VisualCapture
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct WINDOWPLACEMENT { public int length, flags, showCmd; public POINT min, max; public RECT normal; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")] static extern bool SetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] static extern bool GetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool RedrawWindow(IntPtr hwnd, IntPtr rect, IntPtr rgn, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, int flags, IntPtr env, string dir,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    const int SW_SHOWMINNOACTIVE = 7, SW_SHOWNOACTIVATE = 4, STARTF_USESHOWWINDOW = 1;
    const uint PW_RENDERFULLCONTENT = 2;

    // Starts a program minimized and inactive; returns its process id.
    public static int Start(string exe, string arguments, string workingDirectory) { return Start(exe, arguments, workingDirectory, false); }

    // shownInactive: start shown normally but without activation (SW_SHOWNOACTIVATE) instead of minimized. Used for
    // v1, whose window measures its default geometry when it is first shown and so must not start minimized; the
    // window is visible at its normal place until Offscreen() moves it (a brief flash, no focus change).
    public static int Start(string exe, string arguments, string workingDirectory, bool shownInactive)
    {
        return StartShow(exe, arguments, workingDirectory, shownInactive ? SW_SHOWNOACTIVATE : SW_SHOWMINNOACTIVE);
    }

    // Starts a program with an explicit first show command (STARTUPINFO.wShowWindow, used by Windows for the program's
    // first ShowWindow call). 0 = SW_HIDE: the main window is created where the program wants it but stays hidden, so
    // the caller can show it on the chosen monitor without it ever appearing elsewhere (visual gate -Visible).
    public static int StartShow(string exe, string arguments, string workingDirectory, int showCommand)
    {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = (short)showCommand;
        PROCESS_INFORMATION pi;
        var cmd = new StringBuilder("\"" + exe + "\" " + (arguments ?? ""));
        if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, workingDirectory, ref si, out pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    // Visible top-level windows of a process (owned windows included), newest z-order first.
    public static IntPtr[] TopWindows(int pid)
    {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => {
            int p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    public static IntPtr WaitMainWindow(int pid, string titleContains, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs) {
            foreach (var h in TopWindows(pid)) {
                if (GetWindow(h, 4 /*GW_OWNER*/) != IntPtr.Zero) continue;
                if (string.IsNullOrEmpty(titleContains) || Text(h).Contains(titleContains)) return h;
            }
            Thread.Sleep(50);
        }
        return IntPtr.Zero;
    }

    public static string Text(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
    public static string ClassOf(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }

    // The x coordinate just beyond every monitor (virtual screen right edge + margin).
    public static int OffscreenX() { return GetSystemMetrics(76) + GetSystemMetrics(78) + 200; }

    // Moves a window beyond the right edge of the virtual screen and shows it there without activating it.
    public static void Offscreen(IntPtr h) { Offscreen(h, 0, 0); }

    // Same, with an explicit outer window size (0 = keep). Programs that lay out from their restore size when
    // started minimized (v1's WinForms window does) get their real default size this way.
    public static void Offscreen(IntPtr h, int width, int height)
    {
        RECT r; GetWindowRect(h, out r);
        var wp = new WINDOWPLACEMENT(); wp.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
        GetWindowPlacement(h, ref wp);
        int w = wp.normal.Right - wp.normal.Left, ht = wp.normal.Bottom - wp.normal.Top;
        if (w <= 0) { w = r.Right - r.Left; ht = r.Bottom - r.Top; }
        if (width > 0) w = width;
        if (height > 0) ht = height;
        int x = OffscreenX();
        wp.normal.Left = x; wp.normal.Top = 40; wp.normal.Right = x + w; wp.normal.Bottom = 40 + ht;
        wp.showCmd = SW_SHOWNOACTIVATE;
        wp.flags = 0;
        SetWindowPlacement(h, ref wp);
    }

    // Moves an (owned, already shown) window to an off-screen position without activating it.
    public static void MoveOffscreen(IntPtr h, int dx, int y)
    {
        SetWindowPos(h, IntPtr.Zero, OffscreenX() + dx, y, 0, 0, 0x0001 /*NOSIZE*/ | 0x0004 /*NOZORDER*/ | 0x0010 /*NOACTIVATE*/);
    }

    public static RECT WindowRect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }

    // Captures the window's real rendered pixels (PW_RENDERFULLCONTENT) to a PNG; returns "w x h".
    public static string Shot(IntPtr h, string png)
    {
        RedrawWindow(h, IntPtr.Zero, IntPtr.Zero, 0x0001 | 0x0004 | 0x0080 | 0x0100 /*INVALIDATE|ERASE|ALLCHILDREN|UPDATENOW*/);
        Thread.Sleep(120);
        RECT r; GetWindowRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        using (var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr dc = g.GetHdc();
                PrintWindow(h, dc, PW_RENDERFULLCONTENT);
                g.ReleaseHdc(dc);
            }
            bmp.Save(png, ImageFormat.Png);
        }
        return w + " x " + ht;
    }

    // Captures without forcing a repaint first: shows what the screen shows at this moment (used to catch
    // transient unpainted areas right after a window appears).
    public static void ShotRaw(IntPtr h, string png)
    {
        RECT r; GetWindowRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        using (var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, PW_RENDERFULLCONTENT); g.ReleaseHdc(dc); }
            bmp.Save(png, ImageFormat.Png);
        }
    }

    // One line per visible child window: handle, parent handle, class, rectangle relative to the top-level
    // window's top-left corner, and text. Used by the pixel check.
    public static string[] Children(IntPtr top)
    {
        RECT tr; GetWindowRect(top, out tr);
        var lines = new List<string>();
        EnumChildWindows(top, (h, l) => {
            if (!IsWindowVisible(h)) return true;
            RECT r; GetWindowRect(h, out r);
            int style = GetWindowLong(h, -16);
            lines.Add(string.Format("{0}\t{1}\t{2}\t{3}\t{4}\t{5}\t{6}\t{7:X8}\t{8}", h.ToInt64(), GetParent(h).ToInt64(), ClassOf(h),
                r.Left - tr.Left, r.Top - tr.Top, r.Right - tr.Left, r.Bottom - tr.Top, style, Text(h).Replace("\t", " ").Replace("\r", " ").Replace("\n", " ")));
            return true;
        }, IntPtr.Zero);
        return lines.ToArray();
    }
}
