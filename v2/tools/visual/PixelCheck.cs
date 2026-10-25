// Pixel check of the v2 visual gate: judges one window capture (PNG) against the window tree recorded at the same
// moment (VisualDriver.Tree). Checks, each reported as "FAIL <check>: ..." lines:
//   blank    a visible leaf control whose area in the capture is a single colour (nothing drawn: the control is hidden
//            behind something, clipped away, or never painted)
//   overlap  two sibling controls whose rectangles overlap by more than 1 px in both directions (group boxes and
//            their contents excluded)
//   outside  a control that is not inside its parent's client area (1 px tolerance)
//   frame    a control that overlaps a group frame (sibling group box) but is not fully inside it (2 px tolerance)
//   bg       a region of the window's background (client area minus every leaf control, minus the group frames'
//            own lines and titles) that is not the face colour (COLOR_BTNFACE) and larger than a few pixels
// Controls that are deliberately empty or single coloured are listed in an explicit allow-list (see visual-gate.ps1):
// each entry is "check|class regex|id|text regex|reason" ("*" = any).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

public static class PixelCheck
{
    class Node
    {
        public long H, Parent; public string Cls, Text; public int Id;
        public int Wl, Wt, Wr, Wb, Cl, Ct, Cr, Cb; public uint Style, Ex;
        public List<Node> Kids = new List<Node>();
        public Node ParentNode;
        // A group frame: a BS_GROUPBOX button (v2) or a titled WinForms container (v1 GroupBox).
        public bool IsGroup {
            get {
                if (Cls.Equals("Button", StringComparison.OrdinalIgnoreCase) && (Style & 0xF) == 7) return true;
                return Cls.StartsWith("WindowsForms10.Window") && Text.Length > 0 && Kids.Count > 0;
            }
        }
        // Layout containers paint nothing of their own (their gaps are judged); every other window with children
        // (list views with a header, combo boxes with an edit box, ...) paints its whole rectangle itself.
        public bool PureContainer { get { return Cls.StartsWith("WindowsForms10.Window") && Kids.Count > 0; } }
        public string Name { get { return Cls + "#" + Id + (Text.Length > 0 ? " \"" + (Text.Length > 30 ? Text.Substring(0, 30) + "..." : Text) + "\"" : "") + " [" + Wl + "," + Wt + "-" + Wr + "," + Wb + "]"; } }
    }

    class Allow { public string Check, Cls, Id, Text; }

    public class Result
    {
        public List<string> Lines = new List<string>();
        public int Fails, Controls, Allowed;
    }

    static List<Allow> ParseAllow(string[] allow)
    {
        var l = new List<Allow>();
        if (allow == null) return l;
        foreach (var s in allow) {
            if (string.IsNullOrWhiteSpace(s) || s.TrimStart().StartsWith("#")) continue;
            var p = s.Split('|');
            if (p.Length < 4) continue;
            l.Add(new Allow { Check = p[0].Trim(), Cls = p[1].Trim(), Id = p[2].Trim(), Text = p[3] });
        }
        return l;
    }

    static bool Allowed(List<Allow> allow, string check, Node n)
    {
        foreach (var a in allow) {
            if (a.Check != "*" && a.Check != check) continue;
            if (a.Cls != "*" && !Regex.IsMatch(n.Cls, a.Cls)) continue;
            if (a.Id != "*" && a.Id != n.Id.ToString()) continue;
            if (a.Text != "*" && !Regex.IsMatch(n.Text, a.Text)) continue;
            return true;
        }
        return false;
    }

    static List<Node> Parse(string[] tree, out Node top)
    {
        var all = new List<Node>();
        var byH = new Dictionary<long, Node>();
        top = null;
        foreach (var line in tree) {
            var f = line.Split('\t');
            if (f.Length < 15) continue;
            var n = new Node {
                H = long.Parse(f[0]), Parent = long.Parse(f[1]), Cls = f[2], Id = int.Parse(f[3]),
                Wl = int.Parse(f[4]), Wt = int.Parse(f[5]), Wr = int.Parse(f[6]), Wb = int.Parse(f[7]),
                Cl = int.Parse(f[8]), Ct = int.Parse(f[9]), Cr = int.Parse(f[10]), Cb = int.Parse(f[11]),
                Style = uint.Parse(f[12], NumberStyles.HexNumber), Ex = uint.Parse(f[13], NumberStyles.HexNumber), Text = f[14]
            };
            all.Add(n); byH[n.H] = n;
            if (top == null) top = n;
        }
        foreach (var n in all) {
            Node p;
            if (n != top && byH.TryGetValue(n.Parent, out p)) { n.ParentNode = p; p.Kids.Add(n); }
        }
        return all;
    }

    static int[] ReadPixels(Bitmap b)
    {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            var px = new int[b.Width * b.Height];
            for (int y = 0; y < b.Height; y++) Marshal.Copy(IntPtr.Add(d.Scan0, y * d.Stride), px, y * b.Width, b.Width);
            return px;
        } finally { b.UnlockBits(d); }
    }

    static bool Visible(Node n) { return (n.Style & 0x10000000) != 0; }

    // Intersection of rectangles (l,t,r,b).
    static bool Inter(int al, int at, int ar, int ab, int bl, int bt, int br, int bb, out int il, out int it, out int ir, out int ib)
    {
        il = Math.Max(al, bl); it = Math.Max(at, bt); ir = Math.Min(ar, br); ib = Math.Min(ab, bb);
        return ir > il && ib > it;
    }

    // The part of a node's window rectangle that is actually displayed: clipped to every ancestor's client area.
    static bool VisibleRect(Node n, out int l, out int t, out int r, out int b)
    {
        l = n.Wl; t = n.Wt; r = n.Wr; b = n.Wb;
        for (Node p = n.ParentNode; p != null; p = p.ParentNode) {
            int il, it, ir, ib;
            if (!Inter(l, t, r, b, p.Cl, p.Ct, p.Cr, p.Cb, out il, out it, out ir, out ib)) return false;
            l = il; t = it; r = ir; b = ib;
        }
        return true;
    }

    // png: capture of the window (window rectangle, frame included); tree: VisualDriver.Tree lines;
    // allow: allow-list entries; bgColor: expected face colour as 0xRRGGBB.
    public static Result Run(string png, string[] tree, string[] allowEntries, int bgColor)
    {
        var res = new Result();
        Node top;
        var all = Parse(tree, out top);
        var allow = ParseAllow(allowEntries);
        if (top == null) { res.Lines.Add("FAIL capture: empty window tree"); res.Fails++; return res; }
        int[] px; int W, H;
        using (var bmp = new Bitmap(png)) { W = bmp.Width; H = bmp.Height; px = ReadPixels(bmp); }
        int bg = bgColor | unchecked((int)0xFF000000);

        // The whole capture must not be one colour.
        {
            int first = px[0]; bool same = true;
            for (int i = 1; i < px.Length && same; i++) if (px[i] != first) same = false;
            if (same) { res.Lines.Add("FAIL blank: the whole capture is one colour"); res.Fails++; }
        }

        var visible = new List<Node>();
        foreach (var n in all) if (n != top && Visible(n)) visible.Add(n);
        res.Controls = visible.Count;

        // scale of the window from the height of a push button (23 px at 100%); a group box draws its top line half a
        // font height below the top of its window rectangle (also with an empty caption), so that is its visible top
        double k1 = 1.0;
        foreach (var n in visible) if (n.Cls.Equals("Button", StringComparison.OrdinalIgnoreCase) && !n.IsGroup && (n.Style & 0xF) <= 1) { k1 = Math.Max(1.0, (n.Wb - n.Wt) / 23.0); break; }
        int halfFont = (int)Math.Round(15 * k1 / 2.0);

        // ---- outside: each control inside its parent's client area (a group frame: its visible border)
        foreach (var n in visible) {
            Node p = n.ParentNode;
            if (p == null) continue;
            const int tol = 1;
            int visTop = n.IsGroup ? n.Wt + halfFont : n.Wt;
            if (n.Wl < p.Cl - tol || visTop < p.Ct - tol || n.Wr > p.Cr + tol || n.Wb > p.Cb + tol) {
                if (Allowed(allow, "outside", n)) { res.Allowed++; continue; }
                res.Lines.Add("FAIL outside: " + n.Name + " is not inside its parent's client area [" + p.Cl + "," + p.Ct + "-" + p.Cr + "," + p.Cb + "] (" + p.Cls + "#" + p.Id + ")");
                res.Fails++;
            }
        }

        // ---- overlap between siblings (not group boxes)
        var parents = new List<Node>(); parents.Add(top);
        foreach (var n in visible) if (n.Kids.Count > 0) parents.Add(n);
        foreach (var p in parents) {
            var sib = new List<Node>();
            foreach (var k in p.Kids) if (Visible(k) && !k.IsGroup && k.Wr > k.Wl && k.Wb > k.Wt) sib.Add(k);
            for (int i = 0; i < sib.Count; i++) for (int j = i + 1; j < sib.Count; j++) {
                int il, it, ir, ib;
                if (!Inter(sib[i].Wl, sib[i].Wt, sib[i].Wr, sib[i].Wb, sib[j].Wl, sib[j].Wt, sib[j].Wr, sib[j].Wb, out il, out it, out ir, out ib)) continue;
                if (ir - il <= 1 || ib - it <= 1) continue;
                if (Allowed(allow, "overlap", sib[i]) || Allowed(allow, "overlap", sib[j])) { res.Allowed++; continue; }
                res.Lines.Add("FAIL overlap: " + sib[i].Name + " and " + sib[j].Name + " overlap by " + (ir - il) + " x " + (ib - it) + " px");
                res.Fails++;
            }
        }

        // ---- frame: controls crossing a group frame
        foreach (var p in parents) {
            foreach (var g in p.Kids) {
                if (!Visible(g) || !g.IsGroup) continue;
                foreach (var k in p.Kids) {
                    if (k == g || !Visible(k) || k.IsGroup) continue;
                    int il, it, ir, ib;
                    if (!Inter(g.Wl, g.Wt, g.Wr, g.Wb, k.Wl, k.Wt, k.Wr, k.Wb, out il, out it, out ir, out ib)) continue;
                    if (ir - il <= 1 || ib - it <= 1) continue;
                    const int tol = 2;
                    if (k.Wl >= g.Wl - tol && k.Wt >= g.Wt - tol && k.Wr <= g.Wr + tol && k.Wb <= g.Wb + tol) continue;
                    if (Allowed(allow, "frame", k)) { res.Allowed++; continue; }
                    res.Lines.Add("FAIL frame: " + k.Name + " crosses the frame of " + g.Name);
                    res.Fails++;
                }
            }
        }

        // ---- blank: leaf controls drawn as a single colour
        foreach (var n in visible) {
            bool leaf = true;
            foreach (var k in n.Kids) if (Visible(k)) { leaf = false; break; }
            if (!leaf || n.IsGroup) continue;
            int l, t, r, b;
            if (!VisibleRect(n, out l, out t, out r, out b)) {
                if (Allowed(allow, "blank", n)) { res.Allowed++; continue; }
                res.Lines.Add("FAIL blank: " + n.Name + " has no visible area (fully clipped by its parents)");
                res.Fails++; continue;
            }
            l = Math.Max(0, l); t = Math.Max(0, t); r = Math.Min(W, r); b = Math.Min(H, b);
            if ((r - l) * (b - t) < 6) continue;
            int first = px[t * W + l]; bool one = true;
            for (int y = t; y < b && one; y++) for (int x = l; x < r; x++) if (px[y * W + x] != first) { one = false; break; }
            if (!one) continue;
            if (Allowed(allow, "blank", n)) { res.Allowed++; continue; }
            res.Lines.Add("FAIL blank: " + n.Name + " is a single colour (" + ((first >> 16) & 255) + "," + ((first >> 8) & 255) + "," + (first & 255) + ") - nothing drawn");
            res.Fails++;
        }

        // ---- bg: gaps between controls must be the face colour
        var covered = new bool[W * H];
        // outside the client area (frame, caption) is not judged
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            if (x < top.Cl || x >= top.Cr || y < top.Ct || y >= top.Cb) covered[y * W + x] = true;
        // button height gives the scale of the group titles
        int titleBand = (int)Math.Round(20 * k1);
        foreach (var n in visible) {
            int l, t, r, b;
            if (!VisibleRect(n, out l, out t, out r, out b)) continue;
            bool leaf = true;
            foreach (var kk in n.Kids) if (Visible(kk)) { leaf = false; break; }
            if (n.IsGroup) {
                // frame lines and title are the group's own drawing: cover a thin border and the title band
                int bw = 3;
                for (int y = Math.Max(0, t); y < Math.Min(H, b); y++) for (int x = Math.Max(0, l); x < Math.Min(W, r); x++) {
                    bool edge = x < l + bw || x >= r - bw || y >= b - bw || y < t + titleBand;
                    if (edge) covered[y * W + x] = true;
                }
            }
            else if (leaf || !n.PureContainer) {
                for (int y = Math.Max(0, t); y < Math.Min(H, b); y++) for (int x = Math.Max(0, l); x < Math.Min(W, r); x++) covered[y * W + x] = true;
            }
        }
        var seen = new bool[W * H];
        var stack = new Stack<int>();
        int regions = 0;
        for (int y0 = 0; y0 < H; y0++) for (int x0 = 0; x0 < W; x0++) {
            int i0 = y0 * W + x0;
            if (covered[i0] || seen[i0] || px[i0] == bg) continue;
            // flood fill the connected non-face pixels
            int minx = x0, miny = y0, maxx = x0, maxy = y0, area = 0;
            var colors = new Dictionary<int, int>();
            stack.Push(i0); seen[i0] = true;
            while (stack.Count > 0) {
                int i = stack.Pop(); int x = i % W, y = i / W;
                area++;
                if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
                int c = px[i]; int cnt; colors[c] = colors.TryGetValue(c, out cnt) ? cnt + 1 : 1;
                int[] nx = { x - 1, x + 1, x, x }, ny = { y, y, y - 1, y + 1 };
                for (int d = 0; d < 4; d++) {
                    if (nx[d] < 0 || nx[d] >= W || ny[d] < 0 || ny[d] >= H) continue;
                    int j = ny[d] * W + nx[d];
                    if (covered[j] || seen[j] || px[j] == bg) continue;
                    seen[j] = true; stack.Push(j);
                }
            }
            if (area < 16) continue;                       // a few stray pixels
            int bw2 = maxx - minx + 1, bh2 = maxy - miny + 1;
            if (area < 40 && (bw2 < 4 || bh2 < 4)) continue;
            int dom = 0, domCount = -1;
            foreach (var kv in colors) if (kv.Value > domCount) { dom = kv.Key; domCount = kv.Value; }
            // lighter than the face colour in every channel (typically white) is always wrong; any other colour only
            // when the region is thick (thin lines such as splitter bars and group frames are drawn by their window)
            bool lighter = ((dom >> 16) & 255) > ((bg >> 16) & 255) && ((dom >> 8) & 255) > ((bg >> 8) & 255) && (dom & 255) > (bg & 255);
            if (!lighter && !(Math.Min(bw2, bh2) >= 8 && area >= 400)) continue;
            var pseudo = new Node { Cls = top.Cls, Id = -1, Text = ((dom >> 16) & 255) + "," + ((dom >> 8) & 255) + "," + (dom & 255) };
            if (Allowed(allow, "bg", pseudo)) { res.Allowed++; continue; }
            regions++;
            res.Lines.Add("FAIL bg: background region " + minx + "," + miny + "-" + (maxx + 1) + "," + (maxy + 1) + " (" + bw2 + " x " + bh2 + " px, " + area + " px) is " +
                ((dom >> 16) & 255) + "," + ((dom >> 8) & 255) + "," + (dom & 255) + " instead of the face colour " + ((bg >> 16) & 255) + "," + ((bg >> 8) & 255) + "," + (bg & 255));
            res.Fails++;
            if (regions >= 25) { res.Lines.Add("FAIL bg: more than 25 background regions, listing stopped"); break; }
        }
        return res;
    }
}
