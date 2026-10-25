// Extra driving helpers for the end-to-end player test (player-e2e.ps1). Compiled together with
// v2\tools\visual\VisualCapture.cs and VisualDriver.cs (it uses their public helpers); C# 5, Windows API through
// P/Invoke only. Nothing here changes any setting of the PC.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

public static class E2eDriver
{
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SendMessageTimeout(IntPtr h, int msg, IntPtr w, IntPtr l, int flags, int timeoutMs, out IntPtr result);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SendMessageTimeout(IntPtr h, int msg, IntPtr w, StringBuilder l, int flags, int timeoutMs, out IntPtr result);
    [DllImport("user32.dll")] static extern bool IsHungAppWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);

    const int WM_NULL = 0x0000, WM_GETTEXT = 0x000D, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202;
    const int SMTO_ABORTIFHUNG = 0x0002;

    // Milliseconds the window took to answer a WM_NULL, or -1 when it did not answer within timeoutMs (or is hung).
    public static int Ping(IntPtr h, int timeoutMs)
    {
        if (!IsWindow(h)) return -1;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        IntPtr result;
        IntPtr r = SendMessageTimeout(h, WM_NULL, IntPtr.Zero, IntPtr.Zero, SMTO_ABORTIFHUNG, timeoutMs, out result);
        if (r == IntPtr.Zero) return -1;
        return (int)sw.ElapsedMilliseconds;
    }

    public static bool Hung(IntPtr h) { return IsHungAppWindow(h); }

    // Text of one window read with a timeout (never blocks on a hung window).
    public static string TextOf(IntPtr h)
    {
        var sb = new StringBuilder(2048);
        IntPtr result;
        if (SendMessageTimeout(h, WM_GETTEXT, (IntPtr)sb.Capacity, sb, SMTO_ABORTIFHUNG, 300, out result) == IntPtr.Zero) return "";
        return sb.ToString();
    }

    // Title plus the text of every child (what a message box shows), one line.
    public static string AllText(IntPtr top)
    {
        var parts = new List<string>();
        string t = TextOf(top);
        if (t.Length > 0) parts.Add("[" + t + "]");
        foreach (IntPtr c in VisualDriver.Descendants(top, false)) {
            string s = TextOf(c).Trim();
            if (s.Length > 0 && !parts.Contains(s)) parts.Add(s);
        }
        return string.Join(" | ", parts.ToArray()).Replace("\r", " ").Replace("\n", " ");
    }

    // class, title, visibility, rectangle of one window, for logs.
    public static string Describe(IntPtr h)
    {
        RECT r; GetWindowRect(h, out r);
        return string.Format("class={0} title='{1}' visible={2} rect={3},{4},{5},{6}", VisualDriver.Cls(h), TextOf(h), VisualDriver.Visible(h) ? "yes" : "no", r.Left, r.Top, r.Right, r.Bottom);
    }

    // A click at client coordinates (x, y) of a control, as a real mouse click would deliver it to the control.
    public static void ClickClient(IntPtr h, int x, int y)
    {
        IntPtr l = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, WM_LBUTTONDOWN, (IntPtr)1, l);
        PostMessage(h, WM_LBUTTONUP, IntPtr.Zero, l);
    }

    // Hash of what a control currently shows: the window is captured with PrintWindow and the control's rectangle cut out.
    // Equal hashes = identical pixels (a playhead or a readout that did not change).
    public static string RegionHash(IntPtr top, IntPtr child)
    {
        RECT tr, cr; GetWindowRect(top, out tr); GetWindowRect(child, out cr);
        using (Bitmap full = VisualDriver.Grab(top)) {
            Rectangle want = new Rectangle(cr.Left - tr.Left, cr.Top - tr.Top, cr.Right - cr.Left, cr.Bottom - cr.Top);
            Rectangle r = Rectangle.Intersect(new Rectangle(0, 0, full.Width, full.Height), want);
            if (r.Width <= 0 || r.Height <= 0) return "empty";
            BitmapData d = full.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                byte[] buf = new byte[Math.Abs(d.Stride) * r.Height];
                Marshal.Copy(d.Scan0, buf, 0, buf.Length);
                using (SHA1 s = SHA1.Create()) return BitConverter.ToString(s.ComputeHash(buf)).Replace("-", "");
            }
            finally { full.UnlockBits(d); }
        }
    }

    // Enter pressed in a dialog (its default button): posted key down and up.
    public static void PressEnter(IntPtr h)
    {
        PostMessage(h, 0x0100, (IntPtr)0x0D, IntPtr.Zero);
        PostMessage(h, 0x0101, (IntPtr)0x0D, IntPtr.Zero);
    }

    public static bool WaitGone(IntPtr h, int timeoutMs)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs) {
            if (!IsWindow(h)) return true;
            System.Threading.Thread.Sleep(50);
        }
        return !IsWindow(h);
    }

    // One line per visible top-level window of the process (for the log).
    public static string[] WindowList(int pid)
    {
        var list = new List<string>();
        foreach (IntPtr h in VisualDriver.ProcessTopWindows(pid, true)) list.Add(h.ToInt64() + " " + Describe(h));
        return list.ToArray();
    }
}
