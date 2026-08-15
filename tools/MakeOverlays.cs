using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

class MakeOverlays {
    static void Main(string[] args) {
        string dir = Path.GetFullPath(Path.Combine(
            AppDomain.CurrentDomain.BaseDirectory, @"..\..\obs\overlays"));
        if (args.Length > 0) dir = args[0];
        Directory.CreateDirectory(dir);
        WriteLetterbox(Path.Combine(dir, "letterbox.png"));
        WriteVignette(Path.Combine(dir, "vignette.png"));
        WriteBug(Path.Combine(dir, "pathtraced-bug.png"));
        Console.WriteLine("Wrote overlays to " + dir);
    }

    static void WriteLetterbox(string path) {
        var bmp = new Bitmap(1920, 1080, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.Clear(Color.Transparent);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var black = new SolidBrush(Color.FromArgb(235, 0, 0, 0))) {
                g.FillRectangle(black, 0, 0, 1920, 78);
                g.FillRectangle(black, 0, 1002, 1920, 78);
            }
            using (var gold = new Pen(Color.FromArgb(210, 212, 168, 72), 1.5f)) {
                g.DrawLine(gold, 80, 78, 1840, 78);
                g.DrawLine(gold, 80, 1002, 1840, 1002);
            }
        }
        bmp.Save(path, ImageFormat.Png);
        bmp.Dispose();
    }

    static void WriteVignette(string path) {
        var bmp = new Bitmap(1920, 1080, PixelFormat.Format32bppArgb);
        var bits = new int[1920 * 1080];
        for (int y = 0; y < 1080; ++y) {
            float ny = (y - 540f) / 540f;
            for (int x = 0; x < 1920; ++x) {
                float nx = (x - 960f) / 960f;
                float r2 = nx * nx + ny * ny;
                float a = Math.Min(1f, Math.Max(0f, (r2 - 0.42f) * 0.85f));
                bits[y * 1920 + x] = ((int)(a * 150) << 24);
            }
        }
        var data = bmp.LockBits(new Rectangle(0, 0, 1920, 1080),
            ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        System.Runtime.InteropServices.Marshal.Copy(bits, 0, data.Scan0, bits.Length);
        bmp.UnlockBits(data);
        bmp.Save(path, ImageFormat.Png);
        bmp.Dispose();
    }

    static void WriteBug(string path) {
        var bmp = new Bitmap(520, 56, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            g.Clear(Color.Transparent);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            using (var fill = new SolidBrush(Color.FromArgb(160, 8, 10, 8)))
            using (var gold = new Pen(Color.FromArgb(220, 212, 168, 72), 1.25f))
            using (var font = new Font("Segoe UI Semibold", 16, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var text = new SolidBrush(Color.FromArgb(235, 236, 220, 170))) {
                g.FillRectangle(fill, 1, 1, 518, 54);
                g.DrawRectangle(gold, 1, 1, 517, 53);
                g.DrawString("PATH TRACED  ·  10,000,000 BLADES", font, text, 16, 16);
            }
        }
        bmp.Save(path, ImageFormat.Png);
        bmp.Dispose();
    }
}
