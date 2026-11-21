# Draws the game's logo, exe icon, startup splash and installer art. No external tools needed.
# Run: powershell -File Tools\branding\make_branding.ps1   (from the project folder)
# Outputs: Build\Windows\Application.ico, Content\Splash\Splash.bmp, Content\Brand\Logo.png, Tools\installer\art\*
$root = Split-Path (Split-Path $PSScriptRoot)
$src = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;

public static class MRBrand
{
    static readonly Color Amber = Color.FromArgb(255, 184, 26);
    static readonly Color Navy0 = Color.FromArgb(10, 14, 22);
    static readonly Color Navy1 = Color.FromArgb(30, 40, 60);

    static GraphicsPath Round(RectangleF r, float rad)
    {
        var p = new GraphicsPath(); float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure(); return p;
    }

    static void Spaced(Graphics g, string s, Font f, Brush b, float cx, float y, float gap)
    {
        var fmt = StringFormat.GenericTypographic; float total = 0; var w = new List<float>();
        foreach (char c in s) { float cw = g.MeasureString(c.ToString(), f, 9999, fmt).Width; w.Add(cw); total += cw + gap; }
        total -= gap; float x = cx - total / 2;
        for (int i = 0; i < s.Length; i++) { g.DrawString(s[i].ToString(), f, b, x, y, fmt); x += w[i] + gap; }
    }

    // The mark: a road bent into an M. u = size of the square it is drawn in.
    static void Mark(Graphics g, float x, float y, float u, bool withWord)
    {
        float top = withWord ? 0.20f : 0.27f, bot = withWord ? 0.63f : 0.73f, mid = withWord ? 0.50f : 0.57f;
        var pts = new[] {
            new PointF(x + 0.235f * u, y + bot * u), new PointF(x + 0.235f * u, y + top * u), new PointF(x + 0.5f * u, y + mid * u),
            new PointF(x + 0.765f * u, y + top * u), new PointF(x + 0.765f * u, y + bot * u) };
        using (var road = new Pen(Amber, 0.125f * u) { LineJoin = LineJoin.Round, StartCap = LineCap.Round, EndCap = LineCap.Round })
            g.DrawLines(road, pts);
        if (u >= 48)
            using (var dash = new Pen(Navy0, 0.016f * u) { LineJoin = LineJoin.Round, DashStyle = DashStyle.Custom, DashPattern = new[] { 3.2f, 2.6f } })
                g.DrawLines(dash, pts);
        if (withWord)
            using (var f = new Font("Segoe UI", 0.125f * u, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var b = new SolidBrush(Color.White))
                Spaced(g, "RIDE", f, b, x + 0.5f * u, y + 0.715f * u, 0.055f * u);
    }

    public static Bitmap Logo(int n, bool withWord)
    {
        var bmp = new Bitmap(n, n, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit; g.Clear(Color.Transparent);
            var r = new RectangleF(n * 0.03f, n * 0.03f, n * 0.94f, n * 0.94f);
            using (var p = Round(r, n * 0.2f))
            {
                using (var bg = new LinearGradientBrush(new PointF(0, 0), new PointF(0, n), Navy1, Navy0)) g.FillPath(bg, p);
                if (n >= 48) { g.SetClip(p); using (var edge = new Pen(Color.FromArgb(90, Amber), Math.Max(2f, n * 0.024f))) g.DrawPath(edge, p); g.ResetClip(); }
            }
            Mark(g, 0, 0, n, withWord);
        }
        return bmp;
    }

    static void Backdrop(Graphics g, int w, int h)
    {
        using (var bg = new LinearGradientBrush(new Point(0, 0), new Point(w, h), Navy1, Navy0)) g.FillRectangle(bg, 0, 0, w, h);
        // faint street grid, like the in-game map
        var rnd = new Random(7);
        using (var pen = new Pen(Color.FromArgb(16, 255, 255, 255), 2f))
            for (int i = 0; i < 26; i++)
            {
                float a = (float)(rnd.NextDouble() * Math.PI), cx = (float)rnd.NextDouble() * w, cy = (float)rnd.NextDouble() * h, L = w * 1.2f;
                g.DrawLine(pen, cx - (float)Math.Cos(a) * L, cy - (float)Math.Sin(a) * L, cx + (float)Math.Cos(a) * L, cy + (float)Math.Sin(a) * L);
            }
    }

    public static Bitmap Splash(int w, int h)
    {
        var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit; g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            Backdrop(g, w, h);
            int n = (int)(h * 0.6f);
            using (var logo = Logo(n, true)) g.DrawImage(logo, w * 0.07f, (h - n) / 2f - h * 0.03f, n, n);
            float tx = w * 0.07f + n + w * 0.05f;
            using (var f1 = new Font("Segoe UI", h * 0.125f, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var f2 = new Font("Segoe UI", h * 0.047f, FontStyle.Regular, GraphicsUnit.Pixel))
            using (var white = new SolidBrush(Color.White)) using (var amber = new SolidBrush(Amber)) using (var dim = new SolidBrush(Color.FromArgb(170, 176, 190)))
            {
                g.DrawString("MANCHESTER", f1, white, tx, h * 0.25f, StringFormat.GenericTypographic);
                g.DrawString("RIDE", f1, amber, tx, h * 0.39f, StringFormat.GenericTypographic);
                g.DrawString("Open-world riding across Greater Manchester", f2, dim, tx, h * 0.575f, StringFormat.GenericTypographic);
                g.DrawString("Loading...", f2, amber, tx, h * 0.66f, StringFormat.GenericTypographic);
            }
            using (var bar = new SolidBrush(Amber)) g.FillRectangle(bar, 0, h - 6, w, 6);
        }
        return bmp;
    }

    // Tall panel for the installer's left side.
    public static Bitmap Side(int w, int h)
    {
        var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit; g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            Backdrop(g, w, h);
            int n = (int)(w * 0.78f);
            using (var logo = Logo(n, true)) g.DrawImage(logo, (w - n) / 2f, h * 0.12f, n, n);
            using (var bar = new SolidBrush(Amber)) g.FillRectangle(bar, 0, h - 5, w, 5);
        }
        return bmp;
    }

    public static void Ico(string path)
    {
        int[] sizes = { 16, 24, 32, 48, 64, 128, 256 };
        var blobs = new List<byte[]>();
        foreach (int n in sizes)
        {
            using (var big = Logo(n * 4, false)) using (var b = new Bitmap(n, n, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(b)) { g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.PixelOffsetMode = PixelOffsetMode.HighQuality; g.DrawImage(big, 0, 0, n, n); }
                var ms = new MemoryStream();
                if (n == 256) b.Save(ms, ImageFormat.Png);
                else
                {
                    var w = new BinaryWriter(ms);
                    w.Write(40); w.Write(n); w.Write(n * 2); w.Write((short)1); w.Write((short)32); w.Write(0); w.Write(0); w.Write(0); w.Write(0); w.Write(0); w.Write(0);
                    for (int y = n - 1; y >= 0; y--) for (int x = 0; x < n; x++) { var c = b.GetPixel(x, y); w.Write(c.B); w.Write(c.G); w.Write(c.R); w.Write(c.A); }
                    int maskRow = ((n + 31) / 32) * 4; w.Write(new byte[maskRow * n]);
                }
                blobs.Add(ms.ToArray());
            }
        }
        using (var fs = File.Create(path)) using (var w = new BinaryWriter(fs))
        {
            w.Write((short)0); w.Write((short)1); w.Write((short)sizes.Length);
            int off = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++)
            {
                w.Write((byte)(sizes[i] == 256 ? 0 : sizes[i])); w.Write((byte)(sizes[i] == 256 ? 0 : sizes[i])); w.Write((byte)0); w.Write((byte)0);
                w.Write((short)1); w.Write((short)32); w.Write(blobs[i].Length); w.Write(off); off += blobs[i].Length;
            }
            foreach (var b in blobs) w.Write(b);
        }
    }
}
'@
Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing
foreach ($d in 'Build\Windows','Content\Splash','Content\Brand','Tools\installer\art') { New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null }
[MRBrand]::Ico((Join-Path $root 'Build\Windows\Application.ico'))
Copy-Item (Join-Path $root 'Build\Windows\Application.ico') (Join-Path $root 'Tools\installer\art\app.ico') -Force
$l = [MRBrand]::Logo(1024, $true); $l.Save((Join-Path $root 'Content\Brand\Logo.png'), [System.Drawing.Imaging.ImageFormat]::Png); $l.Dispose()
$s = [MRBrand]::Splash(960, 420); $s.Save((Join-Path $root 'Content\Splash\Splash.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$s.Save((Join-Path $root 'Tools\installer\art\splash.png'), [System.Drawing.Imaging.ImageFormat]::Png); $s.Dispose()
$p = [MRBrand]::Side(240, 420); $p.Save((Join-Path $root 'Tools\installer\art\side.png'), [System.Drawing.Imaging.ImageFormat]::Png); $p.Dispose()
"branding written"
