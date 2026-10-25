using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

internal static class IconBuilder
{
    private static readonly int[] Sizes = new int[] { 16, 24, 32, 48, 64, 128, 256 };

    private static int Main(string[] arguments)
    {
        if (arguments.Length != 2) return 2;
        string sourcePath = arguments[0];
        string outputDirectory = arguments[1];
        Directory.CreateDirectory(outputDirectory);
        List<byte[]> images = new List<byte[]>();
        using (Bitmap source = new Bitmap(sourcePath))
        {
            Rectangle content = FindContentBounds(source);
            for (int index = 0; index < Sizes.Length; index++)
            {
                int size = Sizes[index];
                using (Bitmap frame = RenderFrame(source, content, size))
                using (MemoryStream stream = new MemoryStream())
                {
                    frame.Save(Path.Combine(outputDirectory, "icon-" + size + ".png"), ImageFormat.Png);
                    frame.Save(stream, ImageFormat.Png);
                    images.Add(stream.ToArray());
                }
            }
        }
        WriteIcon(Path.Combine(outputDirectory, "MIDIBottleneck Player.ico"), images);
        WritePreview(Path.Combine(outputDirectory, "icon-preview.png"), outputDirectory);
        return 0;
    }

    private static Rectangle FindContentBounds(Bitmap bitmap)
    {
        int left = bitmap.Width, top = bitmap.Height, right = -1, bottom = -1;
        for (int y = 0; y < bitmap.Height; y += 2)
            for (int x = 0; x < bitmap.Width; x += 2)
                if (bitmap.GetPixel(x, y).A > 8)
                {
                    left = Math.Min(left, x); top = Math.Min(top, y);
                    right = Math.Max(right, x); bottom = Math.Max(bottom, y);
                }
        return right < left ? new Rectangle(0, 0, bitmap.Width, bitmap.Height) : Rectangle.FromLTRB(left, top, right + 1, bottom + 1);
    }

    private static Bitmap RenderFrame(Bitmap source, Rectangle content, int size)
    {
        Bitmap result = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (Graphics graphics = Graphics.FromImage(result))
        {
            graphics.Clear(Color.Transparent);
            graphics.CompositingMode = CompositingMode.SourceCopy;
            graphics.CompositingQuality = CompositingQuality.HighQuality;
            graphics.InterpolationMode = size <= 32 ? InterpolationMode.HighQualityBilinear : InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.SmoothingMode = SmoothingMode.HighQuality;
            int inset = size <= 24 ? 1 : Math.Max(2, size / 24);
            float scale = Math.Min((size - inset * 2f) / content.Width, (size - inset * 2f) / content.Height);
            int width = Math.Max(1, (int)Math.Round(content.Width * scale));
            int height = Math.Max(1, (int)Math.Round(content.Height * scale));
            Rectangle destination = new Rectangle((size - width) / 2, (size - height) / 2, width, height);
            graphics.DrawImage(source, destination, content, GraphicsUnit.Pixel);
        }
        return result;
    }

    private static void WriteIcon(string path, IList<byte[]> images)
    {
        using (FileStream stream = File.Create(path))
        using (BinaryWriter writer = new BinaryWriter(stream))
        {
            writer.Write((ushort)0); writer.Write((ushort)1); writer.Write((ushort)images.Count);
            int offset = 6 + images.Count * 16;
            for (int index = 0; index < images.Count; index++)
            {
                int size = Sizes[index];
                writer.Write((byte)(size == 256 ? 0 : size));
                writer.Write((byte)(size == 256 ? 0 : size));
                writer.Write((byte)0); writer.Write((byte)0);
                writer.Write((ushort)1); writer.Write((ushort)32);
                writer.Write(images[index].Length); writer.Write(offset);
                offset += images[index].Length;
            }
            for (int index = 0; index < images.Count; index++) writer.Write(images[index]);
        }
    }

    private static void WritePreview(string path, string directory)
    {
        using (Bitmap preview = new Bitmap(520, 420, PixelFormat.Format32bppArgb))
        using (Graphics graphics = Graphics.FromImage(preview))
        using (Font font = new Font("Segoe UI", 10f))
        {
            graphics.Clear(Color.FromArgb(240, 242, 246));
            int x = 16, y = 20;
            for (int index = 0; index < Sizes.Length; index++)
            {
                int size = Sizes[index];
                using (Bitmap image = new Bitmap(Path.Combine(directory, "icon-" + size + ".png")))
                {
                    graphics.DrawImageUnscaled(image, x, y);
                    graphics.DrawString(size + " px", font, Brushes.Black, x, y + size + 5);
                }
                x += Math.Max(54, size + 18);
                if (index == 4) { x = 16; y = 120; }
            }
            preview.Save(path, ImageFormat.Png);
        }
    }
}
