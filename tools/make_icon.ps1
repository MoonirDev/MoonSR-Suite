# Generates the MoonSR application icon (assets\brand\moonsr.ico) from the logo mark in assets\brand\moonsr_mockup.svg.
#
# The mark is redrawn with GDI+ from the SVG geometry (2x2 glass tiles, crescent moon, check mark; no wordmark, transparent
# background). Each size is rendered 4x..16x oversampled and box-filtered down, so small sizes are not aliased. Sizes up to
# 32 px use a simplified drawing (no halo, no drop shadow, no highlight arcs, wider gaps between the tiles, thicker check
# mark, higher tile opacity) so the four tiles, the crescent and the check stay legible. 256 px is stored as PNG, the others
# as 32-bit DIB (maximum compatibility with rc.exe and LoadImage).
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-Out assets\brand\moonsr.ico] [-PreviewDir <dir>]
# The in-app logo (src/app/ui/gfx.cpp, Canvas::LogoMark) uses the same geometry; keep the two in step.
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

    static double Erfc(double x) {
        double z = Math.Abs(x), t = 1.0 / (1.0 + 0.5 * z);
        double r = t * Math.Exp(-z * z - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 +
                   t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
        return x >= 0 ? r : 2.0 - r;
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

    static GraphicsPath Circle(float cx, float cy, float r) {
        GraphicsPath p = new GraphicsPath();
        p.AddEllipse(cx - r, cy - r, r * 2, r * 2);
        return p;
    }

    static Brush Glass(double opacity, double a0, double a1, double a2, bool small) {
        int c0 = small ? 0x7DDDFF : 0xB8F5FF;
        LinearGradientBrush b = new LinearGradientBrush(new PointF(67, 49), new PointF(309, 307), A(a0 * opacity, c0), A(a2 * opacity, 0x8279FF));
        ColorBlend cb = new ColorBlend(3);
        cb.Positions = new float[] { 0f, 0.45f, 1f };
        cb.Colors = new Color[] { A(a0 * opacity, c0), A(a1 * opacity, 0x5BC7FF), A(a2 * opacity, 0x8279FF) };
        b.InterpolationColors = cb;
        return b;
    }

    // Blurs the alpha channel of a premultiplied ARGB buffer (3 box passes), colour is forced to black * alpha * k.
    static void BlurAlpha(int[] px, int w, int h, int box) {
        float[] a = new float[w * h], t = new float[w * h];
        for (int i = 0; i < a.Length; i++) a[i] = ((px[i] >> 24) & 255) / 255f;
        for (int pass = 0; pass < 3; pass++) {
            for (int y = 0; y < h; y++) {
                float sum = 0; int r = box / 2;
                for (int x = -r; x <= r; x++) sum += (x >= 0 && x < w) ? a[y * w + x] : 0;
                for (int x = 0; x < w; x++) {
                    t[y * w + x] = sum / (2 * r + 1);
                    int add = x + r + 1, rem = x - r;
                    if (add < w) sum += a[y * w + add];
                    if (rem >= 0) sum -= a[y * w + rem];
                }
            }
            for (int x = 0; x < w; x++) {
                float sum = 0; int r = box / 2;
                for (int y = -r; y <= r; y++) sum += (y >= 0 && y < h) ? t[y * w + x] : 0;
                for (int y = 0; y < h; y++) {
                    a[y * w + x] = sum / (2 * r + 1);
                    int add = y + r + 1, rem = y - r;
                    if (add < h) sum += t[add * w + x];
                    if (rem >= 0) sum -= t[rem * w + x];
                }
            }
        }
        for (int i = 0; i < px.Length; i++) px[i] = ((int)Math.Round(Math.Min(1f, a[i]) * 255)) << 24;
    }

    static void SetView(Graphics g, int S, double cx, double cy, double vs) {
        g.TranslateTransform(S / 2f, S / 2f);
        g.ScaleTransform((float)(S / vs), (float)(S / vs));
        g.TranslateTransform((float)-cx, (float)-cy);
    }

    static void DrawTiles(Graphics g, double inset, bool tiny, bool shadowPass, bool small) {
        float i = (float)inset;
        double[] op = tiny ? new double[] { 1.0, 0.95, 0.92, 0.88 } : new double[] { 1.0, 0.78, 0.69, 0.57 };
        float[][] rc = new float[][] {
            new float[] { 74, 72, 123, 123, 31 }, new float[] { 207, 72, 100, 123, 29 },
            new float[] { 74, 207, 123, 100, 29 }, new float[] { 207, 207, 100, 100, 28 } };
        using (GraphicsPath bite = Circle(291, 76, 35)) {
            for (int n = 0; n < 4; n++) {
                float[] r = rc[n];
                using (GraphicsPath tile = RoundRect(r[0] + i, r[1] + i, r[2] - 2 * i, r[3] - 2 * i, Math.Max(2, r[4] - i * 0.5f)))
                using (Brush br = shadowPass ? (Brush)new SolidBrush(Color.Black) : Glass(op[n], small ? 1.0 : 0.88, small ? 0.95 : 0.73, small ? 0.97 : 0.82, small)) {
                    if (n == 1) {
                        using (Region reg = new Region(tile)) { reg.Exclude(bite); g.FillRegion(br, reg); }
                    } else g.FillPath(br, tile);
                }
            }
        }
    }

    public static int[] Render(int S, bool detail, bool tiny) {
        double inset = detail ? 0 : (tiny ? 6.5 : 5);
        int[] result = new int[S * S];
        using (Bitmap bmp = new Bitmap(S, S, PixelFormat.Format32bppPArgb)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.Clear(Color.Transparent);
                double cx = detail ? 192 : 190.5, cy = detail ? 186 : 189.5, vs = detail ? 340 : (tiny ? 246 : 252);
                float sc = (float)(S / vs);
                GraphicsState st0 = g.Save();
                SetView(g, S, cx, cy, vs);

                if (detail) {
                    // Halo and glow are computed per pixel (smooth, no brush artefacts) and composited under the tiles.
                    g.ResetTransform();
                    BitmapData hd = bmp.LockBits(new Rectangle(0, 0, S, S), ImageLockMode.ReadWrite, PixelFormat.Format32bppPArgb);
                    int[] hb = new int[S * S];
                    for (int y = 0; y < S; y++)
                        for (int x = 0; x < S; x++) {
                            double ux = (x + 0.5) / sc + cx - vs / 2, uy = (y + 0.5) / sc + cy - vs / 2;
                            // Halo: radial falloff (ellipse 175 x 139 at 199,176), clipped to the circle r157 at 197,177.
                            double ha = 0;
                            double hx = (ux - 199) / 175, hy = (uy - 176) / 139;
                            double ht = Math.Sqrt(hx * hx + hy * hy);
                            double cd = Math.Sqrt((ux - 197) * (ux - 197) + (uy - 177) * (uy - 177));
                            if (ht < 1 && cd < 157) ha = 0.30 * (1 - ht);
                            // Glow: blurred circle r109, sigma 19, opacity .65, colour 588DFF.
                            double gd = Math.Sqrt((ux - 191) * (ux - 191) + (uy - 190) * (uy - 190));
                            double ga = 0.65 * 0.5 * Erfc((gd - 109.0) / (19.0 * Math.Sqrt(2.0)));
                            // Composite glow over halo (premultiplied).
                            double hr = 0x45 / 255.0, hg = 0xBF / 255.0, hbl = 0xFF / 255.0;
                            double gr = 0x58 / 255.0, gg = 0x8D / 255.0, gbl = 0xFF / 255.0;
                            double oa = ga + ha * (1 - ga);
                            double or_ = gr * ga + hr * ha * (1 - ga), og = gg * ga + hg * ha * (1 - ga), ob = gbl * ga + hbl * ha * (1 - ga);
                            hb[y * S + x] = ((int)Math.Round(oa * 255) << 24) | ((int)Math.Round(or_ * 255) << 16) |
                                            ((int)Math.Round(og * 255) << 8) | (int)Math.Round(ob * 255);
                        }
                    System.Runtime.InteropServices.Marshal.Copy(hb, 0, hd.Scan0, hb.Length);
                    bmp.UnlockBits(hd);
                    g.Restore(st0);
                    if (Environment.GetEnvironmentVariable("MOON_NOSHADOW") == null) {
                    // Drop shadow of the tiles: blurred alpha, offset down 11, 34 % strength.
                    using (Bitmap sb = new Bitmap(S, S, PixelFormat.Format32bppPArgb)) {
                        using (Graphics sg = Graphics.FromImage(sb)) {
                            sg.SmoothingMode = SmoothingMode.AntiAlias;
                            sg.Clear(Color.Transparent);
                            SetView(sg, S, cx, cy - 11, vs);
                            DrawTiles(sg, 0, false, true, false);
                        }
                        BitmapData d = sb.LockBits(new Rectangle(0, 0, S, S), ImageLockMode.ReadWrite, PixelFormat.Format32bppPArgb);
                        int[] buf = new int[S * S];
                        System.Runtime.InteropServices.Marshal.Copy(d.Scan0, buf, 0, buf.Length);
                        BlurAlpha(buf, S, S, Math.Max(3, (int)(2 * 12 * sc)));
                        for (int k = 0; k < buf.Length; k++) {
                            int al = (int)(((buf[k] >> 24) & 255) * 0.34);
                            buf[k] = al << 24;                       // premultiplied black
                        }
                        System.Runtime.InteropServices.Marshal.Copy(buf, 0, d.Scan0, buf.Length);
                        sb.UnlockBits(d);
                        g.ResetTransform();
                        g.DrawImageUnscaled(sb, 0, 0);
                    }
                    }
                    SetView(g, S, cx, cy, vs);
                }

                DrawTiles(g, inset, tiny, false, !detail);

                if (detail) {
                    using (Pen hp = new Pen(A(0.55, 0xFFFFFF), 5f)) {
                        hp.StartCap = LineCap.Round; hp.EndCap = LineCap.Round;
                        g.DrawBezier(hp, 104, 97, 128, 80, 156, 82, 177, 93);
                        hp.Color = A(0.45, 0xFFFFFF);
                        g.DrawBezier(hp, 224, 97, 241, 85, 263, 84, 282, 92);
                    }
                }
                // Crescent: moon circle minus the cut circle.
                using (GraphicsPath moon = Circle(275, 89, tiny ? 38 : 36))
                using (GraphicsPath cut = Circle(291, 76, 35))
                using (Region reg = new Region(moon))
                using (Brush mb = new SolidBrush(A(1.0, 0xFFF2C7))) {
                    reg.Exclude(cut);
                    g.FillRegion(mb, reg);
                }
                // Check mark.
                float cw = tiny ? 15f : (detail ? 8f : 11f);
                using (Pen cp = new Pen(A(1.0, 0xF4FBFF), cw)) {
                    cp.StartCap = LineCap.Round; cp.EndCap = LineCap.Round; cp.LineJoin = LineJoin.Round;
                    g.DrawLines(cp, new PointF[] { new PointF(240, 258), new PointF(254, 272), new PointF(285, 236) });
                }
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

    public static void Build(int[] sizes, string outPath, string previewDir) {
        List<byte[]> images = new List<byte[]>();
        foreach (int size in sizes) {
            bool detail = size >= 40, tiny = size <= 24;
            int S = size >= 256 ? 1024 : size * 16;
            if (S > 1024) S = 1024;
            while (S % size != 0) S++;
            int[] big = Render(S, detail, tiny);
            byte[] bgra = Down(big, S, size);
            using (Bitmap bm = new Bitmap(size, size, PixelFormat.Format32bppArgb)) {
                BitmapData d = bm.LockBits(new Rectangle(0, 0, size, size), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
                System.Runtime.InteropServices.Marshal.Copy(bgra, 0, d.Scan0, bgra.Length);
                bm.UnlockBits(d);
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
