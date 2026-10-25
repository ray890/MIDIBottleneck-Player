// Driving helpers for the v2 visual gate (loaded together with VisualCapture.cs by visual-gate.ps1).
// Microsoft toolchain only: Win32 messages through P/Invoke, PNG through System.Drawing. No UI Automation is needed:
// everything is driven with window messages, the same way for v2 (plain Win32) and v1 (WinForms).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// A private Win32 desktop (never shown on any monitor, nobody can click into it) on which a child process runs.
// Windows of programs started inside it are painted normally by the system, so PrintWindow captures what a monitor
// would show, while nothing appears on the user's screens and no focus is taken.
public static class HiddenDesktop
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateDesktop(string name, string device, IntPtr devmode, int flags, uint access, IntPtr sa);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desk);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, int flags, IntPtr env, string dir,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    public static IntPtr Create(string name)
    {
        IntPtr d = CreateDesktop(name, null, IntPtr.Zero, 0, 0x10000000 /*GENERIC_ALL*/, IntPtr.Zero);
        if (d == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
        return d;
    }

    // Starts a program on the desktop (it and everything it starts stay there); returns its process id.
    public static int Start(string desktopName, string exe, string args, string dir)
    {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = desktopName;
        PROCESS_INFORMATION pi;
        var cmd = new StringBuilder("\"" + exe + "\" " + args);
        if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    public static void Close(IntPtr d) { CloseDesktop(d); }
}

public static class VisualDriver
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr hwnd, ref POINT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern bool RedrawWindow(IntPtr hwnd, IntPtr rect, IntPtr rgn, uint flags);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr w, string l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool AdjustWindowRectEx(ref RECT r, int style, bool menu, int exStyle);
    [DllImport("user32.dll")] static extern IntPtr GetSystemMenu(IntPtr hwnd, bool revert);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] static extern int GetMenuItemID(IntPtr menu, int pos);
    [DllImport("user32.dll")] static extern IntPtr GetSubMenu(IntPtr menu, int pos);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetMenuString(IntPtr menu, int pos, StringBuilder s, int n, int flags);

    // ---------------------------------------------------------------- basic helpers

    public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
    public static string Txt(IntPtr h) { var s = new StringBuilder(1024); GetWindowText(h, s, 1024); return s.ToString(); }
    public static bool Alive(IntPtr h) { return h != IntPtr.Zero && IsWindow(h); }
    public static int Pid(IntPtr h) { int p; GetWindowThreadProcessId(h, out p); return p; }
    public static int CtrlId(IntPtr h) { return GetDlgCtrlID(h); }
    public static bool Visible(IntPtr h) { return IsWindowVisible(h); }
    public static bool Enabled(IntPtr h) { return IsWindowEnabled(h); }
    public static int[] Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return new[] { r.Left, r.Top, r.Right, r.Bottom }; }
    public static int OffscreenX() { return GetSystemMetrics(76) + GetSystemMetrics(78) + 200; }
    public static int VirtualRight() { return GetSystemMetrics(76) + GetSystemMetrics(78); }

    // Every window of the process (visible or not), top-level ones including owned windows, popups and menus.
    public static IntPtr[] ProcessTopWindows(int pid, bool visibleOnly)
    {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => {
            int p; GetWindowThreadProcessId(h, out p);
            if (p == pid && (!visibleOnly || IsWindowVisible(h))) {
                string c = Cls(h);
                // system-injected helper windows that every program on the desktop gets
                if (c.StartsWith("UAC_") || c == "UAC Input Indicator" || c == "IME" || c.StartsWith("MSCTFIME") || c == "tooltips_class32") return true;
                list.Add(h);
            }
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    // Visible top-level windows of the process other than `except`, class name and text (for finding dialogs).
    public static IntPtr[] OtherWindows(int pid, IntPtr except)
    {
        var list = new List<IntPtr>();
        foreach (var h in ProcessTopWindows(pid, true)) if (h != except) list.Add(h);
        return list.ToArray();
    }

    public static IntPtr[] Descendants(IntPtr top, bool visibleOnly)
    {
        var list = new List<IntPtr>();
        EnumChildWindows(top, (h, l) => { if (!visibleOnly || IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
        return list.ToArray();
    }

    // First descendant matching class (exact when clsExact, else substring; "" = any), text substring ("" = any; use
    // "=" prefix for an exact match) and control id (-1 = any).
    public static IntPtr Find(IntPtr top, string cls, string text, int id)
    {
        foreach (var h in Descendants(top, true)) {
            if (!string.IsNullOrEmpty(cls) && Cls(h).IndexOf(cls, StringComparison.OrdinalIgnoreCase) < 0) continue;
            if (id >= 0 && GetDlgCtrlID(h) != id) continue;
            if (!string.IsNullOrEmpty(text)) {
                string t = Txt(h);
                if (text[0] == '=' ? t != text.Substring(1) : t.IndexOf(text, StringComparison.OrdinalIgnoreCase) < 0) continue;
            }
            return h;
        }
        return IntPtr.Zero;
    }

    public static IntPtr WaitFind(IntPtr top, string cls, string text, int id, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        do {
            IntPtr h = Find(top, cls, text, id);
            if (h != IntPtr.Zero) return h;
            Thread.Sleep(60);
        } while (sw.ElapsedMilliseconds < timeoutMs);
        return IntPtr.Zero;
    }

    // First visible top-level window of the process (not `except`) whose class and text contain the given strings.
    public static IntPtr WaitWindow(int pid, IntPtr except, string cls, string text, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        do {
            foreach (var h in ProcessTopWindows(pid, true)) {
                if (h == except) continue;
                if (!string.IsNullOrEmpty(cls) && Cls(h).IndexOf(cls, StringComparison.OrdinalIgnoreCase) < 0) continue;
                if (!string.IsNullOrEmpty(text) && Txt(h).IndexOf(text, StringComparison.OrdinalIgnoreCase) < 0) continue;
                return h;
            }
            Thread.Sleep(60);
        } while (sw.ElapsedMilliseconds < timeoutMs);
        return IntPtr.Zero;
    }

    // ---------------------------------------------------------------- driving

    public static void Click(IntPtr h) { PostMessage(h, 0x00F5 /*BM_CLICK*/, IntPtr.Zero, IntPtr.Zero); }
    public static int CheckState(IntPtr h) { return (int)SendMessage(h, 0x00F0 /*BM_GETCHECK*/, IntPtr.Zero, IntPtr.Zero); }
    public static void SetCheck(IntPtr h, bool on)
    {
        if ((CheckState(h) == 1) != on) Click(h);
    }
    public static void Close(IntPtr h) { PostMessage(h, 0x0010 /*WM_CLOSE*/, IntPtr.Zero, IntPtr.Zero); }
    public static void Key(IntPtr h, int vk) { PostMessage(h, 0x0100, (IntPtr)vk, IntPtr.Zero); PostMessage(h, 0x0101, (IntPtr)vk, IntPtr.Zero); }
    public static void SetText(IntPtr h, string text) { SendMessage(h, 0x000C /*WM_SETTEXT*/, IntPtr.Zero, text); }

    // WM_SYSCOMMAND is posted (not sent) so a command that opens a modal dialog does not block the driver.
    public static void SysCommand(IntPtr top, int id) { PostMessage(top, 0x0112, (IntPtr)id, IntPtr.Zero); }

    public static int ComboCount(IntPtr combo) { return (int)SendMessage(combo, 0x0146 /*CB_GETCOUNT*/, IntPtr.Zero, IntPtr.Zero); }
    public static int ComboSel(IntPtr combo) { return (int)SendMessage(combo, 0x0147 /*CB_GETCURSEL*/, IntPtr.Zero, IntPtr.Zero); }
    public static string ComboItem(IntPtr combo, int i)
    {
        int n = (int)SendMessage(combo, 0x0149 /*CB_GETLBTEXTLEN*/, (IntPtr)i, IntPtr.Zero);
        if (n < 0) return "";
        var sb = new StringBuilder(n + 2);
        SendMessage(combo, 0x0148 /*CB_GETLBTEXT*/, (IntPtr)i, sb);
        return sb.ToString();
    }
    // Selects an item and tells the parent (CBN_SELCHANGE), as a click on the item would.
    public static void ComboSelect(IntPtr combo, int index)
    {
        SendMessage(combo, 0x014E /*CB_SETCURSEL*/, (IntPtr)index, IntPtr.Zero);
        int id = GetDlgCtrlID(combo);
        SendMessage(GetParent(combo), 0x0111 /*WM_COMMAND*/, (IntPtr)((1 << 16) | (id & 0xFFFF)), combo);
    }
    // The same, but the parent is told with a POSTED message: for an item that opens a modal prompt (the parent's
    // handler does not return until the prompt closes, so a sent message would block the caller).
    public static void ComboSelectPost(IntPtr combo, int index)
    {
        SendMessage(combo, 0x014E /*CB_SETCURSEL*/, (IntPtr)index, IntPtr.Zero);
        int id = GetDlgCtrlID(combo);
        PostMessage(GetParent(combo), 0x0111 /*WM_COMMAND*/, (IntPtr)((1 << 16) | (id & 0xFFFF)), combo);
    }
    // A posted message with a screen point packed into lParam (MAKELPARAM(x, y)).
    public static void PostPoint(IntPtr h, int msg, int x, int y)
    {
        PostMessage(h, msg, IntPtr.Zero, (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF)));
    }
    public static void ComboDrop(IntPtr combo, bool open) { PostMessage(combo, 0x014F /*CB_SHOWDROPDOWN*/, open ? (IntPtr)1 : IntPtr.Zero, IntPtr.Zero); }

    // Menu item id of the system menu entries of a window (cross-process read), "id<TAB>text" lines; sub menus are
    // listed after their parent with a leading "  ".
    public static string[] SystemMenuItems(IntPtr top)
    {
        var lines = new List<string>();
        IntPtr m = GetSystemMenu(top, false);
        Walk(m, "", lines, 0);
        return lines.ToArray();
    }
    static void Walk(IntPtr m, string indent, List<string> lines, int depth)
    {
        if (m == IntPtr.Zero || depth > 3) return;
        int n = GetMenuItemCount(m);
        for (int i = 0; i < n; i++) {
            var sb = new StringBuilder(256);
            GetMenuString(m, i, sb, 256, 0x400 /*MF_BYPOSITION*/);
            IntPtr sub = GetSubMenu(m, i);
            lines.Add(indent + GetMenuItemID(m, i) + "\t" + sb.ToString());
            if (sub != IntPtr.Zero) Walk(sub, indent + "  ", lines, depth + 1);
        }
    }

    // Resizes a window so its client area has the requested size (position kept), without activation.
    public static void SetClientSize(IntPtr top, int clientW, int clientH)
    {
        RECT r = new RECT { Left = 0, Top = 0, Right = clientW, Bottom = clientH };
        AdjustWindowRectEx(ref r, GetWindowLong(top, -16), false, GetWindowLong(top, -20));
        SetWindowPos(top, IntPtr.Zero, 0, 0, r.Right - r.Left, r.Bottom - r.Top, 0x0002 | 0x0004 | 0x0010 /*NOMOVE|NOZORDER|NOACTIVATE*/);
    }
    public static int[] ClientSize(IntPtr h) { RECT r; GetClientRect(h, out r); return new[] { r.Right, r.Bottom }; }

    // Moves a window (top-level) to x offset beyond the virtual screen's right edge, keeping its size, without activation.
    public static void MoveOffscreen(IntPtr h, int dx, int y)
    {
        SetWindowPos(h, IntPtr.Zero, OffscreenX() + dx, y, 0, 0, 0x0001 | 0x0004 | 0x0010);
    }

    // ---------------------------------------------------------------- capture

    // Screen mode (prompt 009, "-Visible"): every capture is the real screen pixels of the window on its monitor, taken
    // WITHOUT forcing a repaint first, so stale pixels a program leaves behind are captured as the user would see them.
    // The window is raised to the top-most band without activation (so nothing covers it). Default (hidden desktop):
    // a forced complete repaint followed by PrintWindow, as before.
    public static bool ScreenMode = false;

    public static Bitmap Grab(IntPtr h)
    {
        if (ScreenMode) return GrabScreen(h);
        RedrawWindow(h, IntPtr.Zero, IntPtr.Zero, 0x0001 | 0x0004 | 0x0080 | 0x0100);
        RECT r; GetWindowRect(h, out r);
        int w = Math.Max(1, r.Right - r.Left), ht = Math.Max(1, r.Bottom - r.Top);
        var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc();
            PrintWindow(h, dc, 2 /*PW_RENDERFULLCONTENT*/);
            g.ReleaseHdc(dc);
        }
        return bmp;
    }

    static byte[] Bytes(Bitmap b)
    {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            var buf = new byte[Math.Abs(d.Stride) * b.Height];
            Marshal.Copy(d.Scan0, buf, 0, buf.Length);
            return buf;
        } finally { b.UnlockBits(d); }
    }

    static bool Same(Bitmap a, Bitmap b)
    {
        if (a.Width != b.Width || a.Height != b.Height) return false;
        byte[] x = Bytes(a), y = Bytes(b);
        for (int i = 0; i < x.Length; i++) if (x[i] != y[i]) return false;
        return true;
    }

    // Waits until two consecutive captures 500 ms apart are identical (the program is idle and painted), then
    // saves the last one. stable=false (moving content such as a playing timeline): waits settleMs, captures once.
    // Returns "stable <ms>" or "TIMEOUT <ms>" (saved anyway) or "live".
    public static string ShotStable(IntPtr h, string png, int timeoutMs, bool stable, int settleMs)
    {
        var sw = Stopwatch.StartNew();
        Bitmap prev = null, cur = null;
        string result;
        if (!stable) {
            Thread.Sleep(settleMs);
            cur = Grab(h);
            result = "live";
        } else {
            Thread.Sleep(250);
            prev = Grab(h);
            result = null;
            while (true) {
                Thread.Sleep(500);
                cur = Grab(h);
                if (Same(prev, cur)) { result = "stable " + sw.ElapsedMilliseconds; break; }
                prev.Dispose(); prev = cur; cur = null;
                if (sw.ElapsedMilliseconds > timeoutMs) { cur = prev; prev = null; result = "TIMEOUT " + sw.ElapsedMilliseconds; break; }
            }
        }
        cur.Save(png, ImageFormat.Png);
        cur.Dispose();
        if (prev != null) prev.Dispose();
        return result;
    }

    // Window geometry and one line per window of the tree (the top window first), tab separated:
    // hwnd parent class ctrlId wl wt wr wb (window rect) cl ct cr cb (client rect) - all relative to the top-level
    // window's top-left corner, i.e. in capture pixels - style exstyle(hex) text
    public static string[] Tree(IntPtr top, bool visibleOnly)
    {
        RECT tr; GetWindowRect(top, out tr);
        var lines = new List<string>();
        Action<IntPtr> add = h => {
            RECT r; GetWindowRect(h, out r);
            RECT c; GetClientRect(h, out c);
            var p = new POINT(); ClientToScreen(h, ref p);
            int cl = p.X - tr.Left, ct = p.Y - tr.Top;
            lines.Add(string.Format("{0}\t{1}\t{2}\t{3}\t{4}\t{5}\t{6}\t{7}\t{8}\t{9}\t{10}\t{11}\t{12:X8}\t{13:X8}\t{14}",
                h.ToInt64(), h == top ? 0 : GetParent(h).ToInt64(), Cls(h), GetDlgCtrlID(h),
                r.Left - tr.Left, r.Top - tr.Top, r.Right - tr.Left, r.Bottom - tr.Top,
                cl, ct, cl + c.Right, ct + c.Bottom, GetWindowLong(h, -16), GetWindowLong(h, -20),
                Txt(h).Replace("\t", " ").Replace("\r", " ").Replace("\n", " ")));
        };
        add(top);
        foreach (var h in Descendants(top, visibleOnly)) add(h);
        return lines.ToArray();
    }

    // Composites popup windows (drop-down lists, menus) onto a window capture. Each popup is captured with
    // PrintWindow and drawn at (left, top) given in the main capture's pixel coordinates; the canvas grows when a
    // popup reaches beyond it. popups: handle, x, y triples.
    public static string CompositeShot(IntPtr main, IntPtr[] popups, int[] xs, int[] ys, string png)
    {
        Bitmap m = Grab(main);
        int w = m.Width, ht = m.Height;
        var shots = new List<Bitmap>();
        for (int i = 0; i < popups.Length; i++) {
            Bitmap p = Grab(popups[i]);
            shots.Add(p);
            w = Math.Max(w, xs[i] + p.Width);
            ht = Math.Max(ht, ys[i] + p.Height);
        }
        using (var canvas = new Bitmap(w, ht, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(canvas)) {
            g.Clear(Color.FromArgb(255, 245, 245, 245));
            g.DrawImageUnscaled(m, 0, 0);
            for (int i = 0; i < shots.Count; i++) g.DrawImageUnscaled(shots[i], xs[i], ys[i]);
            canvas.Save(png, ImageFormat.Png);
        }
        m.Dispose();
        foreach (var s in shots) s.Dispose();
        return w + " x " + ht;
    }

    // Saves only a popup window (its own capture).
    public static string PopupShot(IntPtr popup, string png)
    {
        using (var b = Grab(popup)) { b.Save(png, ImageFormat.Png); return b.Width + " x " + b.Height; }
    }

    [DllImport("user32.dll")] static extern int SetWindowLong(IntPtr hwnd, int index, int value);
    [DllImport("user32.dll")] static extern bool SetLayeredWindowAttributes(IntPtr hwnd, uint key, byte alpha, uint flags);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out int value, int size);

    // Makes a window invisible to the user but still composed and capturable: layered, alpha 0, click-through,
    // never activated.
    public static void MakeInvisible(IntPtr h)
    {
        int ex = GetWindowLong(h, -20);
        int want = ex | 0x00080000 /*LAYERED*/ | 0x00000020 /*TRANSPARENT*/ | 0x08000000 /*NOACTIVATE*/ | 0x00000080 /*TOOLWINDOW*/;
        if (want != ex) SetWindowLong(h, -20, want);
        SetLayeredWindowAttributes(h, 0, 0, 2 /*LWA_ALPHA*/);
    }
    public static void SetAlpha(IntPtr h, int a) { SetLayeredWindowAttributes(h, 0, (byte)a, 2); }
    public static int Cloaked(IntPtr h) { int v; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, out v, 4); return v; }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    // Must run before any window or metric call of this process: otherwise Windows reports scaled-down coordinates
    // (a "beyond the right edge" position computed from them can land on a real monitor).
    public static void MakeDpiAware()
    {
        try { if (SetProcessDpiAwarenessContext((IntPtr)(-4) /*PER_MONITOR_AWARE_V2*/)) return; } catch { }
        SetProcessDPIAware();
    }

    // Ground-truth experiment helper: the pixels of a screen rectangle (what the monitors show right now).
    public static void ScreenGrab(int x, int y, int w, int h, string png)
    {
        using (var b = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(b)) {
            g.CopyFromScreen(x, y, 0, 0, new Size(w, h));
            b.Save(png, ImageFormat.Png);
        }
    }

    // Debug helpers: colour of one pixel of a PNG; number of differing pixels between two PNGs.
    public static string Pixel(string png, int x, int y)
    {
        using (var b = new Bitmap(png)) { Color c = b.GetPixel(x, y); return c.R + "," + c.G + "," + c.B; }
    }
    public static string DiffImages(string a, string b) { return DiffRegion(a, b, 0, 0, int.MaxValue, int.MaxValue); }
    public static string DiffRegion(string a, string b, int x0, int y0, int x1, int y1)
    {
        using (var x = new Bitmap(a)) using (var y = new Bitmap(b)) {
            if (x.Width != y.Width || x.Height != y.Height) return "size " + x.Width + "x" + x.Height + " vs " + y.Width + "x" + y.Height;
            int n = 0, minx = int.MaxValue, miny = int.MaxValue, maxx = -1, maxy = -1;
            for (int j = Math.Max(0, y0); j < Math.Min(x.Height, y1); j++) for (int i = Math.Max(0, x0); i < Math.Min(x.Width, x1); i++)
                if (x.GetPixel(i, j).ToArgb() != y.GetPixel(i, j).ToArgb()) { n++; minx = Math.Min(minx, i); miny = Math.Min(miny, j); maxx = Math.Max(maxx, i); maxy = Math.Max(maxy, j); }
            return n + " different pixels, bbox " + minx + "," + miny + " - " + maxx + "," + maxy;
        }
    }

    // ================================================================ screen captures and the resize check (prompt 009)

    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out RECT value, int size);
    [DllImport("dwmapi.dll")] static extern int DwmFlush();
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr hwnd, int msg, IntPtr w, IntPtr l, int flags, int timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern bool GetUpdateRect(IntPtr hwnd, IntPtr rect, bool erase);
    [DllImport("user32.dll")] static extern bool SetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] static extern bool GetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] static extern IntPtr MonitorFromPoint(POINT p, int flags);
    [DllImport("shcore.dll")] static extern int GetDpiForMonitor(IntPtr monitor, int type, out int dpiX, out int dpiY);
    [DllImport("user32.dll")] static extern int GetDpiForSystem();
    [StructLayout(LayoutKind.Sequential)] struct WINDOWPLACEMENT { public int length, flags, showCmd; public POINT min, max; public RECT normal; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MONITORINFOEX { public int cbSize; public RECT rcMonitor, rcWork; public int dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DISPLAY_DEVICE {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, ref RECT rect, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplayDevices(string device, int index, ref DISPLAY_DEVICE dd, int flags);

    // One line per monitor: device | left top width height | primary/secondary | monitor text | effective DPI |
    // work area left top right bottom.
    public static string[] Monitors()
    {
        var list = new List<string>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr d) {
            MONITORINFOEX mi = new MONITORINFOEX(); mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
            if (GetMonitorInfo(hMon, ref mi)) {
                DISPLAY_DEVICE mon = new DISPLAY_DEVICE(); mon.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
                string monText = EnumDisplayDevices(mi.szDevice, 0, ref mon, 0) ? mon.DeviceString : "";
                int dx = 0, dy = 0;
                try { GetDpiForMonitor(hMon, 0 /*MDT_EFFECTIVE_DPI*/, out dx, out dy); } catch { dx = 0; }
                list.Add(string.Format("{0}|{1} {2} {3} {4}|{5}|{6}|{7}|{8} {9} {10} {11}", mi.szDevice, mi.rcMonitor.Left, mi.rcMonitor.Top,
                    mi.rcMonitor.Right - mi.rcMonitor.Left, mi.rcMonitor.Bottom - mi.rcMonitor.Top, (mi.dwFlags & 1) != 0 ? "primary" : "secondary", monText, dx,
                    mi.rcWork.Left, mi.rcWork.Top, mi.rcWork.Right, mi.rcWork.Bottom));
            }
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    // The DPI every DPI-aware (not per-monitor) program such as the player lays itself out for.
    public static int SystemDpi() { try { return GetDpiForSystem(); } catch { return 96; } }

    // Shows a top-level window (also a hidden or minimized one) with its top-left corner at (x, y), keeping its size,
    // WITHOUT activating it, and puts it in the top-most band so nothing covers it on the screen.
    public static void PlaceNoActivate(IntPtr h, int x, int y)
    {
        RECT r; GetWindowRect(h, out r);
        WINDOWPLACEMENT p = new WINDOWPLACEMENT(); p.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
        GetWindowPlacement(h, ref p);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        p.showCmd = 4;   // SW_SHOWNOACTIVATE
        p.flags = 0;
        p.normal.Left = x; p.normal.Top = y; p.normal.Right = x + w; p.normal.Bottom = y + ht;
        SetWindowPlacement(h, ref p);
        SetWindowPos(h, (IntPtr)(-1) /*HWND_TOPMOST*/, x, y, 0, 0, 0x0001 /*NOSIZE*/ | 0x0010 /*NOACTIVATE*/ | 0x0040 /*SHOWWINDOW*/);
    }

    // First top-level window of the process (shown or hidden, not owned) with this class prefix and, if given, exact
    // title. Used for programs started hidden, which are then shown on the chosen monitor.
    public static IntPtr WaitTopWindowAny(int pid, string clsPrefix, string title, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        do {
            IntPtr found = IntPtr.Zero;
            EnumWindows((h, l) => {
                int p; GetWindowThreadProcessId(h, out p);
                if (p != pid || GetWindow(h, 4 /*GW_OWNER*/) != IntPtr.Zero) return true;
                if (!Cls(h).StartsWith(clsPrefix, StringComparison.OrdinalIgnoreCase)) return true;
                if (!string.IsNullOrEmpty(title) && Txt(h) != title) return true;
                found = h; return false;
            }, IntPtr.Zero);
            if (found != IntPtr.Zero) return found;
            Thread.Sleep(40);
        } while (sw.ElapsedMilliseconds < timeoutMs);
        return IntPtr.Zero;
    }

    // Waits until the window's thread has handled everything sent to it and no window of the tree has anything left
    // to paint, then until the desktop compositor has put the result on the screen.
    public static bool WaitPainted(IntPtr top, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        bool clean = false;
        while (sw.ElapsedMilliseconds < timeoutMs) {
            IntPtr res;
            SendMessageTimeout(top, 0 /*WM_NULL*/, IntPtr.Zero, IntPtr.Zero, 0x0002 /*SMTO_ABORTIFHUNG*/, 2000, out res);
            clean = !GetUpdateRect(top, IntPtr.Zero, false);
            if (clean) foreach (var c in Descendants(top, true)) if (GetUpdateRect(c, IntPtr.Zero, false)) { clean = false; break; }
            if (clean) break;
            Thread.Sleep(10);
        }
        try { DwmFlush(); } catch { }
        return clean;
    }

    // Set when the monitor itself shows nothing (a display that is switched off reads back black): the capture then
    // takes the window's own composed surface from the desktop compositor (PrintWindow with PW_RENDERFULLCONTENT and
    // NO forced repaint), i.e. exactly the pixels the compositor would put on that monitor for this window, stale
    // pixels included. Reported by the gate on every capture.
    public static bool SurfaceMode = false;

    // True when a rectangle of the screen reads back entirely black (a monitor that is off or not presenting).
    public static bool ScreenIsDark(int x, int y, int w, int h)
    {
        using (var b = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(b)) {
            g.CopyFromScreen(x, y, 0, 0, new Size(w, h));
            byte[] px = Bytes(b);
            for (int i = 0; i < px.Length; i += 4) if (px[i] != 0 || px[i + 1] != 0 || px[i + 2] != 0) return false;
            return true;
        }
    }

    static Bitmap GrabScreen(IntPtr h)
    {
        SetWindowPos(h, (IntPtr)(-1) /*HWND_TOPMOST*/, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010 /*NOSIZE|NOMOVE|NOACTIVATE*/);
        try { DwmFlush(); } catch { }
        if (SurfaceMode) {
            RECT sr; GetWindowRect(h, out sr);
            var sb = new Bitmap(Math.Max(1, sr.Right - sr.Left), Math.Max(1, sr.Bottom - sr.Top), PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(sb)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2 /*PW_RENDERFULLCONTENT*/); g.ReleaseHdc(dc); }
            return sb;
        }
        RECT r; GetWindowRect(h, out r);
        int w = Math.Max(1, r.Right - r.Left), ht = Math.Max(1, r.Bottom - r.Top);
        var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.CopyFromScreen(r.Left, r.Top, 0, 0, new Size(w, ht), CopyPixelOperation.SourceCopy);
            // Windows 10 draws no frame in the invisible resize borders around a window: the screen shows whatever is
            // behind it there. Blank that band (black, as PrintWindow leaves it) so it never counts as window content.
            RECT f;
            if (DwmGetWindowAttribute(h, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, out f, Marshal.SizeOf(typeof(RECT))) == 0) {
                using (var black = new SolidBrush(Color.Black)) {
                    int l = f.Left - r.Left, t = f.Top - r.Top, rr = f.Right - r.Left, b = f.Bottom - r.Top;
                    if (l > 0) g.FillRectangle(black, 0, 0, l, ht);
                    if (t > 0) g.FillRectangle(black, 0, 0, w, t);
                    if (rr < w) g.FillRectangle(black, rr, 0, w - rr, ht);
                    if (b < ht) g.FillRectangle(black, 0, b, w, ht - b);
                }
            }
        }
        return bmp;
    }

    // A capture without forcing a repaint: the screen in screen mode, else PrintWindow on what the window holds.
    public static Bitmap GrabAsIs(IntPtr h)
    {
        if (ScreenMode) return GrabScreen(h);
        RECT r; GetWindowRect(h, out r);
        var bmp = new Bitmap(Math.Max(1, r.Right - r.Left), Math.Max(1, r.Bottom - r.Top), PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
        return bmp;
    }

    // Asks the window and every child to repaint completely now (what a fresh window of this size shows).
    public static void ForceRepaint(IntPtr h)
    {
        RedrawWindow(h, IntPtr.Zero, IntPtr.Zero, 0x0001 | 0x0004 | 0x0400 /*FRAME*/ | 0x0080 | 0x0100);
    }

    // The client area of a top-level window relative to its window rectangle (capture pixels).
    public static int[] ClientInWindow(IntPtr h)
    {
        RECT w; GetWindowRect(h, out w);
        RECT c; GetClientRect(h, out c);
        var p = new POINT(); ClientToScreen(h, ref p);
        return new[] { p.X - w.Left, p.Y - w.Top, p.X - w.Left + c.Right, p.Y - w.Top + c.Bottom };
    }

    // Changes the client width of a window, keeping the height; leftEdge moves the left edge (the right one stays), as
    // dragging the left border does. Sent as one SetWindowPos (the program has handled it when this returns).
    public static void SetClientWidth(IntPtr top, int clientW, bool leftEdge)
    {
        RECT w; GetWindowRect(top, out w);
        RECT c; GetClientRect(top, out c);
        int nc = (w.Right - w.Left) - c.Right;
        int newW = clientW + nc;
        int x = leftEdge ? w.Right - newW : w.Left;
        SetWindowPos(top, IntPtr.Zero, x, w.Top, newW, w.Bottom - w.Top, 0x0004 /*NOZORDER*/ | 0x0010 /*NOACTIVATE*/);
    }
    // A user's drag runs inside Windows' move/size loop; programs are told when it starts and ends.
    public static void SizeLoop(IntPtr top, bool enter) { SendMessage(top, enter ? 0x0231 /*WM_ENTERSIZEMOVE*/ : 0x0232 /*WM_EXITSIZEMOVE*/, IntPtr.Zero, IntPtr.Zero); }

    // Compares two captures of the same window inside rectangle (x0,y0)-(x1,y1). Writes, when they differ, a picture:
    // the first capture with every differing pixel magenta and the bounding box outlined. Returns
    // "<count>|<bbox or ->".
    public static string CompareRegion(Bitmap a, Bitmap b, int x0, int y0, int x1, int y1, string diffPng)
    {
        if (a.Width != b.Width || a.Height != b.Height) return "-1|size " + a.Width + "x" + a.Height + " vs " + b.Width + "x" + b.Height;
        byte[] x = Bytes(a), y = Bytes(b);
        int stride = a.Width * 4;
        int n = 0, minx = int.MaxValue, miny = int.MaxValue, maxx = -1, maxy = -1;
        x0 = Math.Max(0, x0); y0 = Math.Max(0, y0); x1 = Math.Min(a.Width, x1); y1 = Math.Min(a.Height, y1);
        var mask = new bool[a.Width * a.Height];
        for (int j = y0; j < y1; j++)
            for (int i = x0; i < x1; i++) {
                int o = j * stride + i * 4;
                if (x[o] != y[o] || x[o + 1] != y[o + 1] || x[o + 2] != y[o + 2]) {
                    n++; mask[j * a.Width + i] = true;
                    if (i < minx) minx = i; if (j < miny) miny = j; if (i > maxx) maxx = i; if (j > maxy) maxy = j;
                }
            }
        if (n > 0 && !string.IsNullOrEmpty(diffPng)) {
            using (var d = new Bitmap(a)) {
                for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) if (mask[j * a.Width + i]) d.SetPixel(i, j, Color.Magenta);
                using (var g = Graphics.FromImage(d)) using (var pen = new Pen(Color.Red, 1)) g.DrawRectangle(pen, minx - 2, miny - 2, maxx - minx + 4, maxy - miny + 4);
                d.Save(diffPng, ImageFormat.Png);
            }
        }
        return n + "|" + (n > 0 ? minx + "," + miny + "-" + maxx + "," + maxy : "-");
    }

    // One resize check. The window is already at the size to judge. Waits until it has painted, captures what it shows
    // ("as left by the resize"), forces a complete repaint, captures again ("fresh") and compares the client areas: a
    // window that repaints correctly while it is resized shows the same pixels both times. Saves the as-left capture to
    // asIsPng when they differ or saveAlways, and, when they differ, the fresh capture and the difference picture.
    // Returns "<differing pixels>|<bbox>|<client w>x<client h>|<painted: clean or timeout>".
    public static string ResizeCheck(IntPtr top, int settleMs, string asIsPng, string freshPng, string diffPng, bool saveAlways)
    {
        bool clean = WaitPainted(top, 3000);
        Thread.Sleep(settleMs);
        try { DwmFlush(); } catch { }
        int[] c = ClientInWindow(top);
        using (Bitmap asIs = GrabAsIs(top)) {
            ForceRepaint(top);
            WaitPainted(top, 3000);
            Thread.Sleep(settleMs);
            try { DwmFlush(); } catch { }
            using (Bitmap fresh = GrabAsIs(top)) {
                string cmp = CompareRegion(asIs, fresh, c[0], c[1], c[2], c[3], diffPng);
                bool differs = !cmp.StartsWith("0|");
                if ((differs || saveAlways) && !string.IsNullOrEmpty(asIsPng)) asIs.Save(asIsPng, ImageFormat.Png);
                if (differs && !string.IsNullOrEmpty(freshPng)) fresh.Save(freshPng, ImageFormat.Png);
                return cmp + "|" + (c[2] - c[0]) + "x" + (c[3] - c[1]) + "|" + (clean ? "clean" : "timeout");
            }
        }
    }

    // Safety net: is any part of any window of this process on a real monitor right now?
    public static string[] OnScreenWindows(int pid)
    {
        int right = VirtualRight();
        var list = new List<string>();
        foreach (var h in ProcessTopWindows(pid, true)) {
            RECT r; GetWindowRect(h, out r);
            if (r.Left < right) list.Add(Cls(h) + " \"" + Txt(h) + "\" at " + r.Left + "," + r.Top);
        }
        return list.ToArray();
    }
}
