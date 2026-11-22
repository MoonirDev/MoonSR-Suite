# Generates the MoonSR application icon (assets\brand\moonsr.ico) from the logo mark in assets\brand\moonsr_mark.svg.
#
# The mark (crescent moon with motion trails, orbit ring, four-point star; no wordmark) is redrawn with GDI+ from the same
# geometry as the SVG and the in-app mark (src/app/ui/gfx.cpp, Canvas::LogoMark), on a navy rounded-square plate so the white
# star and the trails stay visible on light and dark taskbars. Geometry is in a 256-unit box. Each size is rendered
# oversampled (up to 1024 px) and box-filtered down, so small sizes are not aliased.
# Sizes up to 32 px use the simplified mark: no trails and no glow, a thicker ring, a larger star and a larger moon.
# 256 px is stored as PNG, the others as 32-bit DIB (maximum compatibility with rc.exe and LoadImage).
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-Out assets\brand\moonsr.ico] [-PreviewDir <dir>]
# With -PreviewDir, every size is also written as icon_<size>.png, plus mark_512.png (the mark alone on the design's navy).
# Keep the geometry in step with Canvas::LogoMark and assets\brand\moonsr_mark.svg.
param([string]$Out = "", [string]$PreviewDir = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root "assets\brand\moonsr.ico" }

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

public static class MoonIcon {
    static Color A(double alpha, int rgb) {
        int a = (int)Math.Round(Math.Max(0, Math.Min(1, alpha)) * 255);
        return Color.FromArgb(a, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    }

    // ---- Mark geometry (256-unit box); the same numbers are in Canvas::LogoMark and moonsr_mark.svg. ----
    // Crescent: disc (MX, MY, MR) minus the cut disc (CX, CY, CR). Ring: lower half of a rotated ellipse band.
    public const float MX = 140, MY = 124, MR = 80, CX = 168, CY = 112, CR = 71;
    public const float RX = 130, RY = 150, RA = 116, ROUT = 40, RIN = 25, RROT = -18;
    public const float SX = 160, SY = 98, SV = 27, SH = 21, SK = 3.2f;
    static readonly float[,] Trails = { { -13, 3, 0.30f }, { -25, 6, 0.16f }, { -36, 9, 0.07f } };

    static GraphicsPath Circle(float cx, float cy, float r) {
        GraphicsPath p = new GraphicsPath();
        p.AddEllipse(cx - r, cy - r, r * 2, r * 2);
        return p;
    }

    static Region Crescent(float dx, float dy, float grow) {
        Region reg = new Region(Circle(MX + dx, MY + dy, MR + grow));
        reg.Exclude(Circle(CX + dx, CY + dy, CR - grow * 0.4f));
        return reg;
    }

    static GraphicsPath Ring(float outB, float inB) {
        GraphicsPath p = new GraphicsPath();
        p.AddArc(RX - RA, RY - outB, RA * 2, outB * 2, 180, -180);  // left -> bottom -> right (outer edge)
        p.AddArc(RX - RA, RY - inB, RA * 2, inB * 2, 0, 180);       // right -> bottom -> left (inner edge)
        p.CloseFigure();
        using (Matrix m = new Matrix()) { m.RotateAt(RROT, new PointF(RX, RY)); p.Transform(m); }
        return p;
    }

    static PointF P(float x, float y) { return new PointF(x, y); }

    static GraphicsPath Star(float cx, float cy, float v, float h, float k) {
        PointF[] tips = { P(cx, cy - v), P(cx + h, cy), P(cx, cy + v), P(cx - h, cy) };
        PointF[] ctl = { P(cx + k, cy - k), P(cx + k, cy + k), P(cx - k, cy + k), P(cx - k, cy - k) };
        GraphicsPath p = new GraphicsPath();
        for (int i = 0; i < 4; i++) {
            PointF a = tips[i], c = ctl[i], b = tips[(i + 1) % 4];
            p.AddBezier(a, P(a.X + (c.X - a.X) * 2 / 3f, a.Y + (c.Y - a.Y) * 2 / 3f), P(b.X + (c.X - b.X) * 2 / 3f, b.Y + (c.Y - b.Y) * 2 / 3f), b);
        }
        p.CloseFigure();
        return p;
    }

    static void Glow(Graphics g, float cx, float cy, float r, double alpha, int rgb) {
        using (GraphicsPath gp = Circle(cx, cy, r))
        using (PathGradientBrush b = new PathGradientBrush(gp)) {
            b.CenterColor = A(alpha, rgb);
            b.SurroundColors = new Color[] { A(0, rgb) };
            Blend bl = new Blend(4);
            bl.Factors = new float[] { 0f, 0.35f, 0.75f, 1f };
            bl.Positions = new float[] { 0f, 0.35f, 0.7f, 1f };
            b.Blend = bl;
            g.FillPath(b, gp);
        }
    }

    // Draws the mark in mark space. small = simplified version for 16..32 px.
    public static void DrawMark(Graphics g, bool small) {
        const int ringA = 0x3EE0FF, ringB = 0xA979FF, moonA = 0xA6EDFF, moonB = 0x5B7CFF, moonC = 0xA979FF;
        if (!small) Glow(g, MX - 6, MY + 4, MR * 1.55f, 0.42, 0x5B7CFF);
        RectangleF mb = new RectangleF(MX - MR, MY - MR, MR * 2, MR * 2);
        if (!small) {
            for (int i = Trails.GetLength(0) - 1; i >= 0; i--)
                using (Region t = Crescent(Trails[i, 0], Trails[i, 1], 0))
                using (Brush tb = new SolidBrush(A(Trails[i, 2], 0x5B7CFF))) g.FillRegion(tb, t);
        }
        float grow = small ? 6 : 0;
        using (Region moon = Crescent(0, 0, grow))
        using (LinearGradientBrush mbr = new LinearGradientBrush(new PointF(mb.Left, mb.Top), new PointF(mb.Right, mb.Bottom), A(1, moonA), A(1, moonC))) {
            ColorBlend cb = new ColorBlend(3);
            cb.Positions = new float[] { 0f, 0.5f, 1f };
            cb.Colors = new Color[] { A(1, moonA), A(1, moonB), A(1, moonC) };
            mbr.InterpolationColors = cb;
            g.FillRegion(mbr, moon);
            // Bright inner rim: the crescent intersected with a band along the cut circle.
            using (GraphicsPath cut = Circle(CX, CY, CR - grow * 0.4f))
            using (Pen wp = new Pen(Color.Black, small ? 10 : 7)) {
                cut.Widen(wp);
                using (Region rim = moon.Clone()) {
                    rim.Intersect(cut);
                    using (Brush rb = new SolidBrush(A(small ? 0.75 : 0.85, 0xEDE6FF))) g.FillRegion(rb, rim);
                }
            }
        }
        using (GraphicsPath ring = Ring(small ? 46 : ROUT, small ? 22 : RIN))
        using (LinearGradientBrush rbr = new LinearGradientBrush(new PointF(RX - RA, 0), new PointF(RX + RA, 0), A(1, ringA), A(1, ringB))) {
            g.FillPath(rbr, ring);
        }
        if (!small) Glow(g, SX, SY, 22, 0.55, 0xFFFFFF);
        using (GraphicsPath st = small ? Star(SX + 2, SY + 2, 36, 28, 5.5f) : Star(SX, SY, SV, SH, SK))
        using (Brush sb = new SolidBrush(A(1, 0xFFFFFF))) g.FillPath(sb, st);
    }

    static GraphicsPath RoundRect(float x, float y, float w, float h, float r) {
        GraphicsPath p = new GraphicsPath();
        float d = r * 2;
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    // Renders the icon (plate + mark) at S x S pixels, premultiplied ARGB. plate=false: the mark alone on the design navy.
    public static int[] Render(int S, bool small, bool plate) {
        int[] result = new int[S * S];
        using (Bitmap bmp = new Bitmap(S, S, PixelFormat.Format32bppPArgb)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.Clear(plate ? Color.Transparent : A(1, 0x070A1C));
                g.ScaleTransform(S / 256f, S / 256f);
                if (plate) {
                    float inset = small ? 4 : 10, rad = small ? 46 : 52;
                    using (GraphicsPath pl = RoundRect(inset, inset, 256 - 2 * inset, 256 - 2 * inset, rad))
                    using (LinearGradientBrush pb = new LinearGradientBrush(new PointF(0, 0), new PointF(256, 256), A(1, 0x1A2350), A(1, 0x080B1E))) {
                        g.FillPath(pb, pl);
                        if (!small) using (Pen bp = new Pen(A(0.35, 0x5B7CFF), 2)) g.DrawPath(bp, pl);
                    }
                    // Fit the mark (visible box about x 14..246, y 32..208) into the plate.
                    float k = small ? 1.04f : 0.86f;
                    g.TranslateTransform(128, 128);
                    g.ScaleTransform(k, k);
                    g.TranslateTransform(-130, small ? -122 : -120);
                } else {
                    g.TranslateTransform(128, 128);
                    g.ScaleTransform(0.92f, 0.92f);
                    g.TranslateTransform(-130, -120);
                }
                DrawMark(g, small);
            }
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, S, S), ImageLockMode.ReadOnly, PixelFormat.Format32bppPArgb);
            System.Runtime.InteropServices.Marshal.Copy(bd.Scan0, result, 0, result.Length);
            bmp.UnlockBits(bd);
        }
        return result;
    }

    // Box-filters a premultiplied S*S buffer down by an integer factor and returns straight-alpha BGRA bytes (top-down).
    public static byte[] Down(int[] src, int S, int size) {
        int k = S / size;
        byte[] o = new byte[size * size * 4];
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                long a = 0, r = 0, gg = 0, b = 0;
                for (int j = 0; j < k; j++)
                    for (int i = 0; i < k; i++) {
                        int p = src[(y * k + j) * S + x * k + i];
                        a += (p >> 24) & 255; r += (p >> 16) & 255; gg += (p >> 8) & 255; b += p & 255;
                    }
                int n = k * k;
                double av = (double)a / n;
                int o4 = (y * size + x) * 4;
                if (av > 0.0) {
                    double inv = 255.0 / av;
                    o[o4 + 0] = (byte)Math.Min(255, Math.Round(b / (double)n * inv));
                    o[o4 + 1] = (byte)Math.Min(255, Math.Round(gg / (double)n * inv));
                    o[o4 + 2] = (byte)Math.Min(255, Math.Round(r / (double)n * inv));
                }
                o[o4 + 3] = (byte)Math.Round(av);
            }
        return o;
    }

    static Bitmap ToBitmap(byte[] bgra, int size) {
        Bitmap bm = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        BitmapData d = bm.LockBits(new Rectangle(0, 0, size, size), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        System.Runtime.InteropServices.Marshal.Copy(bgra, 0, d.Scan0, bgra.Length);
        bm.UnlockBits(d);
        return bm;
    }

    public static void Build(int[] sizes, string outPath, string previewDir) {
        List<byte[]> images = new List<byte[]>();
        foreach (int size in sizes) {
            bool small = size <= 32;
            int S = Math.Min(1024, size * 16);
            while (S % size != 0) S++;
            byte[] bgra = Down(Render(S, small, true), S, size);
            using (Bitmap bm = ToBitmap(bgra, size)) {
                if (!String.IsNullOrEmpty(previewDir)) bm.Save(Path.Combine(previewDir, "icon_" + size + ".png"), ImageFormat.Png);
                MemoryStream ms = new MemoryStream();
                if (size >= 256) {
                    bm.Save(ms, ImageFormat.Png);
                } else {
                    BinaryWriter w = new BinaryWriter(ms);
                    w.Write(40); w.Write(size); w.Write(size * 2); w.Write((short)1); w.Write((short)32);
                    w.Write(0); w.Write(0); w.Write(0); w.Write(0); w.Write(0); w.Write(0);
                    for (int y = size - 1; y >= 0; y--) w.Write(bgra, y * size * 4, size * 4);
                    int maskRow = ((size + 31) / 32) * 4;
                    w.Write(new byte[maskRow * size]);
                    w.Flush();
                }
                images.Add(ms.ToArray());
            }
        }
        if (!String.IsNullOrEmpty(previewDir)) {
            byte[] big = Down(Render(1024, false, false), 1024, 512);
            using (Bitmap bm = ToBitmap(big, 512)) bm.Save(Path.Combine(previewDir, "mark_512.png"), ImageFormat.Png);
        }
        using (FileStream fs = new FileStream(outPath, FileMode.Create))
        using (BinaryWriter w = new BinaryWriter(fs)) {
            w.Write((short)0); w.Write((short)1); w.Write((short)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int n = 0; n < sizes.Length; n++) {
                int s = sizes[n];
                w.Write((byte)(s >= 256 ? 0 : s)); w.Write((byte)(s >= 256 ? 0 : s));
                w.Write((byte)0); w.Write((byte)0); w.Write((short)1); w.Write((short)32);
                w.Write(images[n].Length); w.Write(offset);
                offset += images[n].Length;
            }
            foreach (byte[] im in images) w.Write(im);
        }
    }
}
"@

if ($PreviewDir) { New-Item -ItemType Directory -Force $PreviewDir | Out-Null }
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
try { [MoonIcon]::Build([int[]](16, 20, 24, 32, 40, 48, 64, 256), $Out, $PreviewDir) } catch { $_.Exception.ToString(); throw }
"Wrote $Out ($((Get-Item $Out).Length) bytes)"
