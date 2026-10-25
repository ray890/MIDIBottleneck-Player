// Labelled contact sheets for the v2 visual gate: one row per state, v2 on the left and v1 on the right, with a
// caption (state, scale, style) above each row. Plain System.Drawing; PNG output.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

public static class ContactSheet
{
    // captions[i] is the row caption; left[i] / right[i] are PNG paths ("" or missing file = "not captured").
    // Rows are scaled down (never up) so each image fits maxImageWidth; the sheet is as wide as two panels.
    public static string Build(string title, string[] captions, string[] left, string[] right, string leftLabel, string rightLabel,
        string outPng, int maxImageWidth)
    {
        int n = captions.Length;
        int pad = 8, capH = 26, labH = 20;
        var imgsL = new Bitmap[n]; var imgsR = new Bitmap[n];
        var sizesL = new Size[n]; var sizesR = new Size[n];
        int[] rowH = new int[n];
        for (int i = 0; i < n; i++) {
            imgsL[i] = Load(left[i]); imgsR[i] = Load(right[i]);
            sizesL[i] = Fit(imgsL[i], maxImageWidth); sizesR[i] = Fit(imgsR[i], maxImageWidth);
            rowH[i] = Math.Max(Math.Max(sizesL[i].Height, sizesR[i].Height), 40);
        }
        int width = pad + maxImageWidth + pad + maxImageWidth + pad;
        int height = pad + 30;
        for (int i = 0; i < n; i++) height += capH + labH + rowH[i] + pad * 2;
        using (var sheet = new Bitmap(width, height, PixelFormat.Format24bppRgb))
        using (var g = Graphics.FromImage(sheet))
        using (var big = new Font("Segoe UI", 13f, FontStyle.Bold))
        using (var cap = new Font("Segoe UI", 10.5f, FontStyle.Bold))
        using (var small = new Font("Segoe UI", 9f))
        using (var white = new SolidBrush(Color.White))
        using (var dark = new SolidBrush(Color.FromArgb(40, 40, 40)))
        using (var capBg = new SolidBrush(Color.FromArgb(70, 90, 120)))
        using (var panel = new SolidBrush(Color.FromArgb(120, 120, 120)))
        using (var grey = new SolidBrush(Color.FromArgb(210, 210, 210)))
        {
            g.Clear(Color.FromArgb(60, 60, 60));
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawString(title, big, white, pad, pad);
            int y = pad + 30;
            for (int i = 0; i < n; i++) {
                g.FillRectangle(capBg, 0, y, width, capH);
                g.DrawString(captions[i], cap, white, pad, y + 3);
                y += capH;
                g.FillRectangle(panel, pad, y, maxImageWidth, labH + rowH[i] + pad);
                g.FillRectangle(panel, pad * 2 + maxImageWidth, y, maxImageWidth, labH + rowH[i] + pad);
                g.DrawString(leftLabel + (imgsL[i] == null ? "  (not captured)" : "  " + imgsL[i].Width + " x " + imgsL[i].Height), small, white, pad + 4, y + 2);
                g.DrawString(rightLabel + (imgsR[i] == null ? "  (not captured)" : "  " + imgsR[i].Width + " x " + imgsR[i].Height), small, white, pad * 2 + maxImageWidth + 4, y + 2);
                if (imgsL[i] != null) g.DrawImage(imgsL[i], pad, y + labH, sizesL[i].Width, sizesL[i].Height);
                if (imgsR[i] != null) g.DrawImage(imgsR[i], pad * 2 + maxImageWidth, y + labH, sizesR[i].Width, sizesR[i].Height);
                y += labH + rowH[i] + pad * 2;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPng)));
            sheet.Save(outPng, ImageFormat.Png);
        }
        for (int i = 0; i < n; i++) { if (imgsL[i] != null) imgsL[i].Dispose(); if (imgsR[i] != null) imgsR[i].Dispose(); }
        return outPng;
    }

    static Bitmap Load(string path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
        using (var b = new Bitmap(path)) return new Bitmap(b);
    }

    static Size Fit(Bitmap b, int maxW)
    {
        if (b == null) return new Size(0, 0);
        double s = b.Width > maxW ? (double)maxW / b.Width : 1.0;
        return new Size((int)Math.Round(b.Width * s), (int)Math.Round(b.Height * s));
    }
}
