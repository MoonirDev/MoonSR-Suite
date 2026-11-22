"""Reference implementation and synthetic PSNR benchmark for MoonSR Spatial v0.1.

This is a scalar, per-pixel port of shaders/upscale/moonsr_spatial_ps.hlsl. It intentionally mirrors the shader
line by line (same taps, same tensor, same kernel equations, same constants, same deringing) so that the two can be
compared. It uses only the Python standard library: NumPy and Pillow were not installed on the development machine,
and a scalar port is also the most direct mirror of a per-pixel shader. Images are written as PNG with zlib.

Usage (from the project root):
    python benchmarks/reference/moonsr_spatial_ref.py [--size 256] [--no-images]

Protocol: each synthetic test image is generated at size x size (supersampled 4x4 where it has edges, so the
"ground truth" is itself anti-aliased like a game frame), quantised to 8 bit, box-downsampled 2x2 -> low-res input
(quantised to 8 bit, like a BGRA8 capture), then upscaled 2x by each method, quantised to 8 bit and compared with the
high-res original. PSNR is over all pixels and all three channels, peak 1.0.

The sample-position convention matches shaders/common/scale_common.hlsli: output pixel centre (x+0.5)/W maps to
source position srcRectOrigin + uv * srcRectSize, texel centres at integer + 0.5.
"""

import argparse
import math
import os
import random
import struct
import sys
import time
import zlib

# --------------------------------------------------------------------------------------------------------------------
# Constants: keep in sync with moonsr_spatial_ps.hlsl
# --------------------------------------------------------------------------------------------------------------------
NOISE_SIGMA_DEFAULT = 0.015   # param2 default: expected luma noise std-dev; regularises coherence
STRENGTH_DEFAULT = 1.0        # param1 default: edge adaptivity strength
COH_LO, COH_HI = 0.35, 0.85   # coherence -> anisotropy ramp
EDGE_LO, EDGE_HI = 0.02, 0.12  # sqrt(lambda1) -> across-edge lobe strength ramp
STRETCH_MAX = 0.5             # along-edge stretch: S = 1 + STRETCH_MAX * A
MAG_RAMP = 4.0                # anisotropy fades in between 1.0x and 1.25x magnification
DERING_SOFT = 0.1             # soft-clamp overshoot allowance as a fraction of the local 2x2 range


def saturate(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def smoothstep(e0, e1, x):
    t = saturate((x - e0) / (e1 - e0))
    return t * t * (3.0 - 2.0 * t)


def luma(r, g, b):
    return 0.299 * r + 0.587 * g + 0.114 * b


# --------------------------------------------------------------------------------------------------------------------
# Image container: three flat float lists, edge-replicated padding of 2 texels for 4x4 neighbourhoods.
# --------------------------------------------------------------------------------------------------------------------
class Img:
    def __init__(self, w, h, r=None, g=None, b=None):
        self.w, self.h = w, h
        n = w * h
        self.r = r if r is not None else [0.0] * n
        self.g = g if g is not None else [0.0] * n
        self.b = b if b is not None else [0.0] * n

    @staticmethod
    def gray(w, h, vals):
        return Img(w, h, list(vals), list(vals), list(vals))

    def padded(self, p):
        """Returns (pw, R, G, B, Y) padded by p texels with clamp-to-edge (equals LoadSrc clamping)."""
        w, h = self.w, self.h
        pw, ph = w + 2 * p, h + 2 * p
        R, G, B, Y = [0.0] * (pw * ph), [0.0] * (pw * ph), [0.0] * (pw * ph), [0.0] * (pw * ph)
        for y in range(ph):
            sy = min(max(y - p, 0), h - 1)
            for x in range(pw):
                sx = min(max(x - p, 0), w - 1)
                s = sy * w + sx
                d = y * pw + x
                R[d], G[d], B[d] = self.r[s], self.g[s], self.b[s]
                Y[d] = luma(R[d], G[d], B[d])
        return pw, R, G, B, Y


def quantize8(img):
    q = lambda v: round(saturate(v) * 255.0) / 255.0
    return Img(img.w, img.h, [q(v) for v in img.r], [q(v) for v in img.g], [q(v) for v in img.b])


def box_down(img, f):
    w, h = img.w // f, img.h // f
    out = Img(w, h)
    inv = 1.0 / (f * f)
    for ch_in, ch_out in ((img.r, out.r), (img.g, out.g), (img.b, out.b)):
        for y in range(h):
            for x in range(w):
                s = 0.0
                for dy in range(f):
                    row = (f * y + dy) * img.w + f * x
                    for dx in range(f):
                        s += ch_in[row + dx]
                ch_out[y * w + x] = s * inv
    return out


def box_down2(img):
    return box_down(img, 2)


def psnr(a, b):
    se = 0.0
    for ca, cb in ((a.r, b.r), (a.g, b.g), (a.b, b.b)):
        for x, y in zip(ca, cb):
            d = x - y
            se += d * d
    mse = se / (3 * len(a.r))
    return float("inf") if mse == 0 else 10.0 * math.log10(1.0 / mse)


def write_png(path, img):
    rows = []
    for y in range(img.h):
        row = bytearray([0])
        for x in range(img.w):
            i = y * img.w + x
            row += bytes((round(saturate(img.r[i]) * 255), round(saturate(img.g[i]) * 255),
                          round(saturate(img.b[i]) * 255)))
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 6)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", raw))
        f.write(chunk(b"IEND", b""))


# --------------------------------------------------------------------------------------------------------------------
# Generic resampling driver: calls fn(ctx, px, py) with the continuous source position in texels.
# --------------------------------------------------------------------------------------------------------------------
PAD = 2


def resample(src, dw, dh, pixel_fn, **kw):
    pw, R, G, B, Y = src.padded(PAD)
    ctx = dict(pw=pw, R=R, G=G, B=B, Y=Y, sw=src.w, sh=src.h, dw=dw, dh=dh, **kw)
    out = Img(dw, dh)
    for oy in range(dh):
        py = (oy + 0.5) / dh * src.h
        for ox in range(dw):
            px = (ox + 0.5) / dw * src.w
            r, g, b = pixel_fn(ctx, px, py)
            i = oy * dw + ox
            out.r[i], out.g[i], out.b[i] = r, g, b
    return out


def bilinear_px(ctx, px, py):
    sw, sh = ctx["sw"], ctx["sh"]
    # ClampToSrcRect, then bilinear (equivalent of SampleSrcLinear)
    px = min(max(px, 0.5), sw - 0.5)
    py = min(max(py, 0.5), sh - 0.5)
    fx, fy = px - 0.5, py - 0.5
    bx, by = math.floor(fx), math.floor(fy)
    tx, ty = fx - bx, fy - by
    pw = ctx["pw"]
    i00 = (by + PAD) * pw + bx + PAD
    res = []
    for C in (ctx["R"], ctx["G"], ctx["B"]):
        a = C[i00] * (1 - tx) + C[i00 + 1] * tx
        c = C[i00 + pw] * (1 - tx) + C[i00 + pw + 1] * tx
        res.append(a * (1 - ty) + c * ty)
    return res


def catmull_rom_w(t):
    # weights for taps at offsets -1, 0, 1, 2 (B=0, C=0.5)
    t2, t3 = t * t, t * t * t
    return (-0.5 * t3 + t2 - 0.5 * t,
            1.5 * t3 - 2.5 * t2 + 1.0,
            -1.5 * t3 + 2.0 * t2 + 0.5 * t,
            0.5 * t3 - 0.5 * t2)


def bicubic_px(ctx, px, py):
    fx, fy = px - 0.5, py - 0.5
    bx, by = math.floor(fx), math.floor(fy)
    wx, wy = catmull_rom_w(fx - bx), catmull_rom_w(fy - by)
    pw = ctx["pw"]
    res = []
    for C in (ctx["R"], ctx["G"], ctx["B"]):
        acc = 0.0
        for j in range(4):
            row = (by - 1 + j + PAD) * pw + bx - 1 + PAD
            acc += wy[j] * (wx[0] * C[row] + wx[1] * C[row + 1] + wx[2] * C[row + 2] + wx[3] * C[row + 3])
        res.append(acc)
    return res


# --------------------------------------------------------------------------------------------------------------------
# MoonSR Spatial v0.1 (mirror of the shader)
# --------------------------------------------------------------------------------------------------------------------
def k_iso(x):
    """Isotropic fallback 1D profile: (1 - x^2) * (1 - x^2/4)^4. Interpolating (0 at +-1, +-2), Lanczos-2-like."""
    x2 = x * x
    w = max(0.0, 1.0 - 0.25 * x2)
    w2 = w * w
    return (1.0 - x2) * w2 * w2


def soft_clamp(c, lo, hi):
    ov = DERING_SOFT * (hi - lo) + 1e-6
    if c > hi:
        e = c - hi
        return hi + e * ov / (ov + e)
    if c < lo:
        e = lo - c
        return lo - e * ov / (ov + e)
    return c


def moonsr_px(ctx, px, py):
    pw, R, G, B, Y = ctx["pw"], ctx["R"], ctx["G"], ctx["B"], ctx["Y"]
    fx, fy = px - 0.5, py - 0.5
    bx, by = math.floor(fx), math.floor(fy)
    tx, ty = fx - bx, fy - by

    # 4x4 neighbourhood, index j*4+i, texel (bx-1+i, by-1+j)
    idx = [(by - 1 + j + PAD) * pw + (bx - 1 + PAD) + i for j in range(4) for i in range(4)]
    L = [Y[k] for k in idx]

    # ---- structure tensor: 3x3 corner gradients (2x2 box derivative), quadratic B-spline weights ----
    wx = (0.5 * (1.0 - tx) ** 2, 0.75 - (tx - 0.5) ** 2, 0.5 * tx * tx)
    wy = (0.5 * (1.0 - ty) ** 2, 0.75 - (ty - 0.5) ** 2, 0.5 * ty * ty)
    jxx = jxy = jyy = 0.0
    for l in range(3):
        for k in range(3):
            a, b = L[l * 4 + k], L[l * 4 + k + 1]
            c, d = L[(l + 1) * 4 + k], L[(l + 1) * 4 + k + 1]
            gx = 0.5 * ((b - a) + (d - c))
            gy = 0.5 * ((c - a) + (d - b))
            w = wx[k] * wy[l]
            jxx += w * gx * gx
            jxy += w * gx * gy
            jyy += w * gy * gy

    sigma = ctx["noise_sigma"]
    strength = ctx["strength"]
    trace = jxx + jyy
    dxx = jxx - jyy
    disc = math.sqrt(dxx * dxx + 4.0 * jxy * jxy)
    coh = disc / (trace + 2.0 * sigma * sigma + 1e-12)
    lam1 = 0.5 * (trace + disc)

    mag = min(ctx["dw"] / ctx["sw"], ctx["dh"] / ctx["sh"])
    ramp = saturate((mag - 1.0) * MAG_RAMP)
    A = saturate(smoothstep(COH_LO, COH_HI, coh) * ramp * strength)
    E = smoothstep(EDGE_LO, EDGE_HI, math.sqrt(lam1))
    gamma = 1.0 - E                       # 1: Lanczos-2-like lobe, 0: stronger lobe

    rd = 1.0 / max(disc, 1e-10)
    c2, s2 = dxx * rd, 2.0 * jxy * rd
    nx = math.sqrt(saturate(0.5 + 0.5 * c2))   # across-edge unit vector (dominant gradient direction)
    ny = math.sqrt(saturate(0.5 - 0.5 * c2))
    if s2 < 0.0:
        ny = -ny

    # Ellipse metric: across-edge axis scaled by the projected lattice density (|nx|+|ny|: 1 on axis, sqrt2 at 45deg),
    # along-edge axis stretched by S. Both reduce to the isotropic metric as A -> 0.
    kap = 1.0 + A * (abs(nx) + abs(ny) - 1.0)
    inv_s = 1.0 / (1.0 + STRETCH_MAX * A)
    ux, uy = nx * kap, ny * kap
    vx, vy = -ny * inv_s, nx * inv_s

    hx = [k_iso(i - 1 - tx) for i in range(4)]
    hy = [k_iso(j - 1 - ty) for j in range(4)]
    # separable support guard: flat-topped, exactly 0 at |d| = 2 so weights stay continuous across texel cells
    gx_ = [max(0.0, 1.0 - (0.25 * (i - 1 - tx) ** 2) ** 3) for i in range(4)]
    gy_ = [max(0.0, 1.0 - (0.25 * (j - 1 - ty) ** 2) ** 3) for j in range(4)]

    wi_sum = wa_sum = 0.0
    ci = [0.0, 0.0, 0.0]
    ca = [0.0, 0.0, 0.0]
    for j in range(4):
        dy = j - 1 - ty
        for i in range(4):
            dx = i - 1 - tx
            k = idx[j * 4 + i]
            r, g, b = R[k], G[k], B[k]
            wi = hx[i] * hy[j]
            # elliptical kernel: rho^2 = (kappa*u)^2 + (v/S)^2, u across, v along the edge
            u = dx * ux + dy * uy
            v = dx * vx + dy * vy
            p2 = u * u + v * v
            wp = max(0.0, 1.0 - 0.25 * p2)
            wa = (1.0 - p2) * wp * wp * wp * (1.0 - gamma * 0.25 * p2) * gx_[i] * gy_[j]
            wi_sum += wi
            wa_sum += wa
            ci[0] += wi * r; ci[1] += wi * g; ci[2] += wi * b
            ca[0] += wa * r; ca[1] += wa * g; ca[2] += wa * b

    ri = 1.0 / wi_sum
    ra = 1.0 / max(wa_sum, 1e-4)
    # Deringing bounds: continuous generalisation of the nearest-2x2 min/max. A hard 2x2 window switches when the
    # sample crosses a texel centre and produced output jumps of up to 0.10 at cell boundaries (visible seams).
    # Texel membership f = tent(dx)*tent(dy), tent(x) = saturate(1.5 - |x|); each texel contributes
    # lerp(bil, C, f) to the min and max, where bil is the bilinear value (continuous, inside the 2x2 range).
    # At a cell centre this is exactly the 2x2 min/max; at a texel centre the bounds reach half-way to the neighbours.
    fx_ = [saturate(1.5 - abs(i - 1 - tx)) for i in range(4)]
    fy_ = [saturate(1.5 - abs(j - 1 - ty)) for j in range(4)]
    out = []
    for ch, C in enumerate((R, G, B)):
        v = ci[ch] * ri + A * (ca[ch] * ra - ci[ch] * ri)
        if not ctx.get("dering", True):  # ablation only; the shader always deringes
            out.append(v)
            continue
        bil = ((C[idx[5]] * (1 - tx) + C[idx[6]] * tx) * (1 - ty) + (C[idx[9]] * (1 - tx) + C[idx[10]] * tx) * ty)
        lo = hi = bil
        for j in range(4):
            for i in range(4):
                m = bil + (C[idx[j * 4 + i]] - bil) * (fx_[i] * fy_[j])
                hi = max(hi, m)
                lo = min(lo, m)
        out.append(soft_clamp(v, lo, hi))
    return out


# --------------------------------------------------------------------------------------------------------------------
# Synthetic test images (all generated in code, deterministic)
# --------------------------------------------------------------------------------------------------------------------
SS = 4  # supersampling per axis for anti-aliased ground truth


def supersample(n, f):
    vals = []
    inv = 1.0 / SS
    for y in range(n):
        for x in range(n):
            acc = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    acc += f(x + (sx + 0.5) * inv, y + (sy + 0.5) * inv)
            vals.append(acc / (SS * SS))
    return Img.gray(n, n, vals)


def img_zone_plate(n):
    # cos(k r^2): local frequency k*r/pi cycles/px; reaches 0.25 cyc/px (low-res Nyquist) at r = n/2,
    # so the corners are beyond low-res Nyquist (aliased in the input on purpose).
    c = n / 2.0
    k = 0.25 * math.pi / c
    return supersample(n, lambda x, y: 0.5 + 0.5 * math.cos(k * ((x - c) ** 2 + (y - c) ** 2)))


def img_slanted_bars(n, deg, period=24.0):
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    # bars along direction `deg` from horizontal: coordinate across the bars is (-x sin + y cos)
    return supersample(n, lambda x, y: 0.85 if ((-x * sa + y * ca) % period) < period / 2 else 0.15)


def img_thin_lines(n):
    # 1-px and 2-px wide (high-res) dark-on-light lines at 0, 0.5, 2, 10, 45 degrees, plus vertical
    # (angle from horizontal in degrees, width in high-res px, point the line passes through as fraction of n)
    specs = [(0.0, 1.0, 0.5, 0.10), (0.5, 1.0, 0.5, 0.22), (2.0, 2.0, 0.5, 0.34), (10.0, 1.0, 0.5, 0.46),
             (90.0, 1.0, 0.62, 0.5), (88.0, 2.0, 0.74, 0.5), (45.0, 2.0, 0.5, 0.75), (30.0, 2.0, 0.5, 0.9)]
    lines = []
    for deg, width, fx, fy in specs:
        a = math.radians(deg)
        lines.append((-math.sin(a), math.cos(a), fx * n + 0.3, fy * n + 0.3, width * 0.5))

    def f(x, y):
        for nx_, ny_, px, py, hw in lines:
            if abs(nx_ * (x - px) + ny_ * (y - py)) < hw:
                return 0.1
        return 0.9
    return supersample(n, f)


def img_checker_text(n):
    rnd = random.Random(1234)
    vals = [0.9] * (n * n)

    def put(x, y, v):
        if 0 <= x < n and 0 <= y < n:
            vals[y * n + x] = v
    # top-left quadrant: checkerboards with 2, 3, 4 px cells
    q = n // 2
    for y in range(q):
        for x in range(q):
            cell = 2 if x < q // 3 else (3 if x < 2 * q // 3 else 4)
            vals[y * n + x] = 0.1 if ((x // cell) + (y // cell)) % 2 else 0.9
    # rest: random 5x7 glyphs, glyph pixel = 2 px (-> 1 low-res texel strokes) and 3 px
    for region, gp in (((q, 0, n, q), 2), ((0, q, n, n), 3)):
        x0, y0, x1, y1 = region
        gy = y0 + 2
        while gy + 7 * gp <= y1:
            gx = x0 + 2
            while gx + 5 * gp <= x1:
                for cy in range(7):
                    for cx in range(5):
                        if rnd.random() < 0.45:
                            for py in range(gp):
                                for px in range(gp):
                                    put(gx + cx * gp + px, gy + cy * gp + py, 0.05)
                gx += 6 * gp
            gy += 9 * gp
    return Img.gray(n, n, vals)


def img_gradient_noise(n, sigma=0.03):
    rnd = random.Random(42)
    r, g, b = [], [], []
    for y in range(n):
        for x in range(n):
            base = 0.15 + 0.7 * (x + y) / (2 * n)
            r.append(base + rnd.gauss(0, sigma))
            g.append(0.8 - 0.6 * y / n + rnd.gauss(0, sigma))
            b.append(0.3 + 0.4 * x / n + rnd.gauss(0, sigma))
    return Img(n, n, r, g, b)


def test_images(n):
    yield "zone_plate", img_zone_plate(n)
    for deg in (1, 3, 5, 15, 30, 45):
        yield "bars_%02ddeg" % deg, img_slanted_bars(n, deg)
    yield "thin_lines", img_thin_lines(n)
    yield "checker_text", img_checker_text(n)
    yield "gradient_noise", img_gradient_noise(n)


# --------------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=256, help="high-res test image size (must be even)")
    ap.add_argument("--no-images", action="store_true", help="do not write PNGs")
    ap.add_argument("--outdir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "output"))
    args = ap.parse_args()
    n = args.size
    outdir = os.path.normpath(args.outdir)
    if not args.no_images:
        os.makedirs(outdir, exist_ok=True)

    base_kw = dict(noise_sigma=NOISE_SIGMA_DEFAULT, strength=STRENGTH_DEFAULT)
    methods = [
        ("bilinear", bilinear_px, base_kw),
        ("catmull_rom", bicubic_px, base_kw),
        ("moonsr_iso_only", moonsr_px, dict(base_kw, strength=0.0)),  # ablation: anisotropy disabled
        ("moonsr_no_dering", moonsr_px, dict(base_kw, dering=False)),  # ablation: deringing disabled
        ("moonsr_v0.1", moonsr_px, base_kw),
    ]

    # 1:1 identity check (exact pass-through expected: interpolating kernel, anisotropy ramped out at 1.0x)
    probe = quantize8(box_down2(quantize8(img_checker_text(64))))
    same = resample(probe, probe.w, probe.h, moonsr_px, **base_kw)
    max_err = max(max(abs(a - b) for a, b in zip(x, y)) for x, y in ((same.r, probe.r), (same.g, probe.g), (same.b, probe.b)))
    print("1:1 identity check (moonsr_v0.1, 32x32 text/checker): max abs error = %.3e" % max_err)

    # Continuity across texel-cell boundaries (where the 4x4 window shifts): output at boundary -/+ eps, eps = 1e-6.
    # A discontinuous term (window-dependent bound, weight not vanishing at the window edge) shows up as a jump here
    # and as a grid of seams in the image.
    eps, worst = 1e-6, 0.0
    for _, img in list(test_images(48))[::3]:
        lr = quantize8(box_down2(quantize8(img)))
        pw, R, G, B, Y = lr.padded(PAD)
        ctx = dict(pw=pw, R=R, G=G, B=B, Y=Y, sw=lr.w, sh=lr.h, dw=2 * lr.w, dh=2 * lr.h, **base_kw)
        for y in range(2, lr.h - 2):
            for k in range(2, lr.w - 2):
                b = k + 0.5
                for p0, p1 in (((b - eps, y + 0.3), (b + eps, y + 0.3)), ((y + 0.3, b - eps), (y + 0.3, b + eps))):
                    a0, a1 = moonsr_px(ctx, *p0), moonsr_px(ctx, *p1)
                    worst = max(worst, max(abs(u - v) for u, v in zip(a0, a1)))
    print("Cell-boundary continuity (moonsr_v0.1): max jump = %.2e (eps = 1e-6; ~1e-5 or less means continuous)" % worst)

    print("\nPSNR (dB) vs high-res original, %dx%d -> box 2x down -> 2x up, 8-bit quantised at every stage\n" % (n, n))
    header = "| image | " + " | ".join(m[0] for m in methods) + " |"
    print(header)
    print("|" + "---|" * (len(methods) + 1))
    t_start = time.time()
    for name, hr in test_images(n):
        hr = quantize8(hr)
        lr = quantize8(box_down2(hr))
        if not args.no_images:
            write_png(os.path.join(outdir, "%s_hr.png" % name), hr)
            write_png(os.path.join(outdir, "%s_lr.png" % name), lr)
        row = []
        for mname, fn, kw in methods:
            up = quantize8(resample(lr, n, n, fn, **kw))
            row.append("%.2f" % psnr(hr, up))
            if not args.no_images:
                write_png(os.path.join(outdir, "%s_%s.png" % (name, mname)), up)
        print("| %s | %s |" % (name, " | ".join(row)))
        sys.stdout.flush()
    # 3x: box 3x3 down, 3x up (no images written)
    n3 = max(96, (int(n * 0.75) // 6) * 6)
    print("\nPSNR (dB), %dx%d -> box 3x down -> 3x up\n" % (n3, n3))
    print(header)
    print("|" + "---|" * (len(methods) + 1))
    for name, hr in test_images(n3):
        hr = quantize8(hr)
        lr = quantize8(box_down(hr, 3))
        row = ["%.2f" % psnr(hr, quantize8(resample(lr, n3, n3, fn, **kw))) for _, fn, kw in methods]
        print("| %s | %s |" % (name, " | ".join(row)))
        sys.stdout.flush()
    print("\n(total %.1f s)" % (time.time() - t_start))


if __name__ == "__main__":
    main()
