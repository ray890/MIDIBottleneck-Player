// Helpers for watch-passages.ps1 (monitor lookup, window placement without activation, picture of a control for
// reading its text, timeline track measurement). Compiled together with v2\tools\visual\VisualCapture.cs and
// VisualDriver.cs; C# 5, Windows API through P/Invoke only. Nothing here changes any setting of the PC.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class WatchDriver
{
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    struct WINDOWPLACEMENT { public int length, flags, showCmd; public POINT ptMinPosition, ptMaxPosition; public RECT rcNormalPosition; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MONITORINFOEX { public int cbSize; public RECT rcMonitor, rcWork; public int dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DISPLAY_DEVICE
    {
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
    [DllImport("user32.dll")] static extern bool SetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] static extern bool GetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);

    // One line per attached monitor: device name | left top width height | primary | adapter text | monitor text | monitor id.
    public static string[] Monitors()
    {
        var list = new List<string>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr d) {
            MONITORINFOEX mi = new MONITORINFOEX(); mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
            if (GetMonitorInfo(hMon, ref mi)) {
                DISPLAY_DEVICE mon = new DISPLAY_DEVICE(); mon.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
                string monText = "", monId = "", adText = "";
                if (EnumDisplayDevices(mi.szDevice, 0, ref mon, 0)) { monText = mon.DeviceString; monId = mon.DeviceID; }
                // the adapter text: enumerate the adapters (device = null) and find the one with this name
                for (int i = 0; ; i++) {
                    DISPLAY_DEVICE a = new DISPLAY_DEVICE(); a.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
                    if (!EnumDisplayDevices(null, i, ref a, 0)) break;
                    if (string.Equals(a.DeviceName, mi.szDevice, StringComparison.OrdinalIgnoreCase)) { adText = a.DeviceString; break; }
                }
                list.Add(string.Format("{0}|{1} {2} {3} {4}|{5}|{6}|{7}|{8}", mi.szDevice, mi.rcMonitor.Left, mi.rcMonitor.Top,
                    mi.rcMonitor.Right - mi.rcMonitor.Left, mi.rcMonitor.Bottom - mi.rcMonitor.Top, (mi.dwFlags & 1) != 0 ? "primary" : "secondary", adText, monText, monId));
            }
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    // Puts a window (also a minimized one) at the given rectangle and shows it WITHOUT activating it.
    public static void PlaceNoActivate(IntPtr h, int x, int y, int w, int hgt)
    {
        WINDOWPLACEMENT p = new WINDOWPLACEMENT(); p.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
        GetWindowPlacement(h, ref p);
        p.showCmd = 4;   // SW_SHOWNOACTIVATE
        p.rcNormalPosition.Left = x; p.rcNormalPosition.Top = y; p.rcNormalPosition.Right = x + w; p.rcNormalPosition.Bottom = y + hgt;
        SetWindowPlacement(h, ref p);
        // SWP_NOZORDER | SWP_NOACTIVATE
        SetWindowPos(h, IntPtr.Zero, x, y, w, hgt, 0x0004 | 0x0010);
    }

    public static bool Minimized(IntPtr h) { return IsIconic(h); }

    // The player's system (window) menu: -1 = no such item, 0 = not ticked, 1 = ticked (read only).
    [DllImport("user32.dll")] static extern IntPtr GetSystemMenu(IntPtr h, bool revert);
    [DllImport("user32.dll")] static extern uint GetMenuState(IntPtr menu, uint id, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static int SystemMenuChecked(IntPtr h, int id)
    {
        IntPtr m = GetSystemMenu(h, false);
        if (m == IntPtr.Zero) return -1;
        // the v2 player sets its menu ticks when the window menu opens (WM_INITMENUPOPUP, system menu), so ask it to
        // do that first; otherwise the tick shows an old state
        SendMessage(h, 0x0117 /*WM_INITMENUPOPUP*/, m, (IntPtr)0x10000);
        uint s = GetMenuState(m, (uint)id, 0 /*MF_BYCOMMAND*/);
        if (s == 0xFFFFFFFF) return -1;
        return (s & 0x8 /*MF_CHECKED*/) != 0 ? 1 : 0;
    }
    // Tells a button's parent that the button was clicked (WM_COMMAND, BN_CLICKED), as a real click would. Used for v1's
    // check boxes: a posted BM_CLICK is not acted on while v1's window is not the active one (seen 2026-10-16).
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
    public static void NotifyClicked(IntPtr button)
    {
        SendMessage(GetParent(button), 0x0111 /*WM_COMMAND*/, (IntPtr)(GetDlgCtrlID(button) & 0xFFFF), button);
    }
    // A left-button press and release sent straight to a control at its centre (no real mouse, no activation).
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    public static void PressCentre(IntPtr h)
    {
        RECT r; GetClientRect(h, out r);
        IntPtr lp = (IntPtr)((((r.Bottom / 2) & 0xFFFF) << 16) | ((r.Right / 2) & 0xFFFF));
        SendMessage(h, 0x0201 /*WM_LBUTTONDOWN*/, (IntPtr)1, lp);
        SendMessage(h, 0x0202 /*WM_LBUTTONUP*/, IntPtr.Zero, lp);
    }
    // Chooses one item of the player's system menu (WM_SYSCOMMAND), as a click on it would.
    public static void SystemCommand(IntPtr h, int id) { SendMessage(h, 0x0112 /*WM_SYSCOMMAND*/, (IntPtr)id, IntPtr.Zero); }

    public static int[] Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return new[] { r.Left, r.Top, r.Right, r.Bottom }; }

    // Picture of a control (captured with PrintWindow through its top window), magnified by 'scale', saved as a PNG; returns the path.
    // Used to read the text a control draws itself (the players' statistics and timeline are not Windows text controls).
    public static string ControlPicture(IntPtr top, IntPtr child, int scale, string png)
    {
        return ControlCellPicture(top, child, 0, 1, 0, 1, scale, png);
    }

    // The right-hand part of a control (rightPx wide, inset px smaller on top, bottom and right): the time text of the timeline,
    // without the focus rectangle that surrounds the control after a click (it confuses the text recogniser).
    public static string ControlRightPicture(IntPtr top, IntPtr child, int rightPx, int inset, int scale, string png)
    {
        RECT tr, cr; GetWindowRect(top, out tr); GetWindowRect(child, out cr);
        using (Bitmap full = VisualDriver.Grab(top)) {
            int w = Math.Min(rightPx, cr.Right - cr.Left) - inset;
            Rectangle want = new Rectangle(cr.Right - tr.Left - w - inset, cr.Top - tr.Top + inset, w, (cr.Bottom - cr.Top) - 2 * inset);
            Rectangle r = Rectangle.Intersect(new Rectangle(0, 0, full.Width, full.Height), want);
            if (r.Width <= 0 || r.Height <= 0) throw new Exception("empty region");
            using (Bitmap big = new Bitmap(r.Width * scale, r.Height * scale, PixelFormat.Format32bppArgb))
            using (Graphics g = Graphics.FromImage(big)) {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.Clear(Color.White);
                g.DrawImage(full, new Rectangle(0, 0, big.Width, big.Height), r, GraphicsUnit.Pixel);
                big.Save(png, ImageFormat.Png);
            }
        }
        return png;
    }

    // The same for one cell of a control divided into cols x rows equal cells (the players' statistics area is 2 columns x 4 rows;
    // a whole-area picture is read in column order, a single cell is read as "label: value").
    public static string ControlCellPicture(IntPtr top, IntPtr child, int col, int cols, int row, int rows, int scale, string png)
    {
        RECT tr, cr; GetWindowRect(top, out tr); GetWindowRect(child == IntPtr.Zero ? top : child, out cr);
        using (Bitmap full = VisualDriver.Grab(top)) {
            int cw = (cr.Right - cr.Left) / cols, ch = (cr.Bottom - cr.Top) / rows;
            Rectangle want = new Rectangle(cr.Left - tr.Left + col * cw, cr.Top - tr.Top + row * ch, cw, ch);
            Rectangle r = Rectangle.Intersect(new Rectangle(0, 0, full.Width, full.Height), want);
            if (r.Width <= 0 || r.Height <= 0) throw new Exception("empty region");
            using (Bitmap big = new Bitmap(r.Width * scale, r.Height * scale, PixelFormat.Format32bppArgb))
            using (Graphics g = Graphics.FromImage(big)) {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.Clear(Color.White);
                g.DrawImage(full, new Rectangle(0, 0, big.Width, big.Height), r, GraphicsUnit.Pixel);
                big.Save(png, ImageFormat.Png);
            }
        }
        return png;
    }

    // Left and right x (client pixels of 'timeline') of the horizontal track of the players' timeline slider, found by looking along the
    // middle row of its picture for the track colours (grey background, blue fill, border). Returns {-1,-1} when not found.
    public static int[] TimelineTrack(IntPtr top, IntPtr timeline)
    {
        RECT tr, cr; GetWindowRect(top, out tr); GetWindowRect(timeline, out cr);
        using (Bitmap full = VisualDriver.Grab(top)) {
            int ox = cr.Left - tr.Left, oy = cr.Top - tr.Top, w = cr.Right - cr.Left, h = cr.Bottom - cr.Top;
            if (ox < 0 || oy < 0 || ox + w > full.Width || oy + h > full.Height || w < 40) return new[] { -1, -1 };
            int left = -1, right = -1, y = oy + h / 2;
            Func<Color, bool> isTrack = delegate(Color c) {
                return Near(c, 205, 209, 214) || Near(c, 52, 120, 214) || Near(c, 125, 130, 138) || Near(c, 225, 225, 225) || Near(c, 160, 170, 180);
            };
            for (int x = 0; x < w; x++) {
                bool t = isTrack(full.GetPixel(ox + x, y));
                if (t && left < 0) left = x;
                if (left >= 0) {
                    if (t) right = x;
                    else if (x - right > 6 && right >= 0) break;   // a gap after the track: the time text starts
                }
            }
            return new[] { left, right };
        }
    }
    static bool Near(Color c, int r, int g, int b) { return Math.Abs(c.R - r) <= 10 && Math.Abs(c.G - g) <= 10 && Math.Abs(c.B - b) <= 10; }
}
