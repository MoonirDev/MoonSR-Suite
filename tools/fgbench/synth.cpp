// Deterministic synthetic sequences with exact ground truth. Every scene is a continuous function of (x, y, t), so
// frame A (t = 0), frame B (t = 1) and any intermediate phase are rendered the same way (supersampled box filter).
#include "fgbench.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace fgb {

void ParallelFor(uint32_t n, const std::function<void(uint32_t)>& fn) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned nt = std::min<unsigned>(hw, std::max<uint32_t>(1, n));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; ++t)
        th.emplace_back([=, &fn] {
            for (uint32_t y = t; y < n; y += nt) fn(y);
        });
    for (auto& t : th) t.join();
}

namespace {

constexpr double kPi = 3.14159265358979323846;

uint32_t Hash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return h;
}
double Hash01(int32_t x, int32_t y, uint32_t seed) { return (Hash(x, y, seed) & 0xFFFFFF) / 16777215.0; }

double Fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }

// Smooth value noise in [0, 1] with lattice spacing `scale` pixels.
double ValueNoise(double x, double y, double scale, uint32_t seed) {
    x /= scale;
    y /= scale;
    const double fx = std::floor(x), fy = std::floor(y);
    const int32_t ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
    const double u = Fade(x - fx), v = Fade(y - fy);
    const double a = Hash01(ix, iy, seed), b = Hash01(ix + 1, iy, seed);
    const double c = Hash01(ix, iy + 1, seed), d = Hash01(ix + 1, iy + 1, seed);
    return (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;
}

double Fbm(double x, double y, uint32_t seed) {
    double s = 0, amp = 0.5, scale = 96;
    for (int o = 0; o < 4; ++o) {
        s += amp * ValueNoise(x, y, scale, seed + o * 17);
        amp *= 0.5;
        scale *= 0.5;
    }
    return s / 0.9375;
}

// "Game-like" texture: smooth colour variation, brick tiles with sharp mortar lines, fine detail noise.
Rgb Texture(double x, double y, uint32_t seed) {
    const double r0 = Fbm(x, y, seed), g0 = Fbm(x, y, seed + 101), b0 = Fbm(x, y, seed + 202);
    const double bw = 52, bh = 26;
    const double row = std::floor(y / bh);
    const double xo = x + (static_cast<int64_t>(row) & 1 ? bw * 0.5 : 0.0);
    const double col = std::floor(xo / bw);
    const double tint = Hash01(static_cast<int32_t>(col), static_cast<int32_t>(row), seed + 7);
    const double fx = xo - col * bw, fy = y - row * bh;
    const bool mortar = fx < 2.5 || fy < 2.5;
    const double detail = ValueNoise(x, y, 2.5, seed + 33) - 0.5;
    Rgb c;
    c.r = 0.25 + 0.55 * r0 + 0.25 * (tint - 0.5);
    c.g = 0.22 + 0.5 * g0 + 0.15 * (tint - 0.5);
    c.b = 0.2 + 0.45 * b0;
    if (mortar) {
        c.r = c.r * 0.35 + 0.05;
        c.g = c.g * 0.35 + 0.05;
        c.b = c.b * 0.35 + 0.05;
    }
    c.r += 0.12 * detail;
    c.g += 0.12 * detail;
    c.b += 0.12 * detail;
    return c;
}

Rgb Lerp(const Rgb& a, const Rgb& b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

Sequence Pan(const std::string& name, double dx, double dy, uint32_t seed) {
    Sequence s;
    s.name = name;
    char b[128];
    snprintf(b, sizeof(b), "global pan, displacement A->B (%.2f, %.2f) px", dx, dy);
    s.description = b;
    s.scene = [=](double x, double y, double t) { return Texture(x + dx * t + 1000, y + dy * t + 1000, seed); };
    return s;
}

// HUD: opaque elements (bars, glyph rows, crosshair) and a semi-transparent panel. Returns coverage and colour.
struct HudSample {
    double alpha = 0; // opaque-element coverage 0/1
    Rgb color;
    double panelAlpha = 0;
};

HudSample Hud(double x, double y, uint32_t w, uint32_t h) {
    HudSample o;
    const double W = w, H = h;
    // Health bar, bottom-left: white 3 px frame, red fill to 70 %.
    const double bx0 = 40, by0 = H - 60, bx1 = 40 + 0.22 * W, by1 = H - 40;
    if (x >= bx0 && x < bx1 && y >= by0 && y < by1) {
        o.alpha = 1;
        const bool frame = x < bx0 + 3 || x >= bx1 - 3 || y < by0 + 3 || y >= by1 - 3;
        const bool fill = x < bx0 + 3 + 0.7 * (bx1 - bx0 - 6);
        o.color = frame ? Rgb{0.95, 0.95, 0.95} : (fill ? Rgb{0.85, 0.1, 0.1} : Rgb{0.15, 0.05, 0.05});
        return o;
    }
    // Text: three rows of pseudo glyphs at top-left (8x12 cells, strokes 2 px).
    if (x >= 32 && x < 32 + 8 * 28 && y >= 28 && y < 28 + 3 * 18) {
        const int cx = static_cast<int>((x - 32) / 8), cy = static_cast<int>((y - 28) / 18);
        const double gx = x - 32 - cx * 8, gy = y - 28 - cy * 18;
        if (gy < 12 && gx < 6 && Hash(cx, cy, 991) % 7 != 0) {
            const uint32_t g = Hash(cx, cy, 4242);
            const bool v0 = (g & 1) && gx < 2, v1 = (g & 2) && gx >= 4, h0 = (g & 4) && gy < 2, h1 = (g & 8) && gy >= 5 && gy < 7,
                       h2 = (g & 16) && gy >= 10;
            if (v0 || v1 || h0 || h1 || h2) {
                o.alpha = 1;
                o.color = {1.0, 0.92, 0.35};
                return o;
            }
        }
    }
    // Crosshair: 2-px white lines with 1-px dark outline, 6-px gap, arms 14 px.
    const double cx = W * 0.5, cy = H * 0.5;
    const double ax = std::abs(x - cx), ay = std::abs(y - cy);
    const bool armH = ay < 2 && ax >= 6 && ax < 20, armV = ax < 2 && ay >= 6 && ay < 20;
    const bool outH = ay < 3 && ax >= 5 && ax < 21, outV = ax < 3 && ay >= 5 && ay < 21;
    if (armH || armV) {
        o.alpha = 1;
        o.color = {1, 1, 1};
        return o;
    }
    if (outH || outV) {
        o.alpha = 1;
        o.color = {0.05, 0.05, 0.05};
        return o;
    }
    // Semi-transparent minimap-like panel, top-right (not counted as opaque HUD).
    if (x >= W - 260 && x < W - 40 && y >= 30 && y < 130) o.panelAlpha = 0.55;
    return o;
}

} // namespace

std::vector<Sequence> MakeSequences(uint32_t w, uint32_t h) {
    std::vector<Sequence> v;
    v.push_back(Pan("pan_subpx", 0.4, 0.25, 11));
    v.push_back(Pan("pan_5px", 5.3, 2.1, 12));
    v.push_back(Pan("pan_20px", 19.6, -7.4, 13));
    v.push_back(Pan("pan_45px", 44.7, 11.2, 14));
    {
        Sequence s;
        s.name = "hud_pan";
        s.description = "pan (19.6,-7.4) px under a static HUD: health bar, glyph text, crosshair, 55% panel";
        s.scene = [=](double x, double y, double t) {
            Rgb c = Texture(x + 19.6 * t + 1000, y - 7.4 * t + 1000, 15);
            const HudSample hs = Hud(x, y, w, h);
            if (hs.alpha > 0) return hs.color;
            if (hs.panelAlpha > 0) c = Lerp(c, Rgb{0.05, 0.08, 0.12}, hs.panelAlpha);
            return c;
        };
        s.hudCoverage = [=](double x, double y) { return Hud(x, y, w, h).alpha; };
        v.push_back(std::move(s));
    }
    {
        Sequence s;
        s.name = "layers_occlusion";
        s.description = "background pans (12,3) px, three textured foreground boxes move (-14,-4) px (occlusion/disocclusion)";
        const double W = w, H = h;
        s.scene = [=](double x, double y, double t) {
            const double fx = x + 14 * t, fy = y + 4 * t; // foreground layer coordinates
            const double boxes[3][4] = {{0.15 * W, 0.2 * H, 0.38 * W, 0.55 * H},
                                        {0.5 * W, 0.45 * H, 0.75 * W, 0.85 * H},
                                        {0.62 * W, 0.08 * H, 0.9 * W, 0.3 * H}};
            for (const auto& b : boxes)
                if (fx >= b[0] && fx < b[2] && fy >= b[1] && fy < b[3]) return Texture(fx + 5000, fy + 3000, 77);
            return Texture(x - 12 * t + 1000, y - 3 * t + 1000, 16);
        };
        v.push_back(std::move(s));
    }
    {
        Sequence s;
        s.name = "small_fast_object";
        s.description = "36 px disc moving (64,22) px over a background panning (4,1) px";
        const double cx0 = 0.3 * w, cy0 = 0.4 * h;
        s.scene = [=](double x, double y, double t) {
            const double ox = cx0 + 64 * t, oy = cy0 + 22 * t;
            const double dx = x - ox, dy = y - oy;
            if (dx * dx + dy * dy < 18 * 18) {
                const bool chk = ((static_cast<int>(std::floor((dx + 18) / 6)) + static_cast<int>(std::floor((dy + 18) / 6))) & 1) != 0;
                return chk ? Rgb{0.95, 0.85, 0.1} : Rgb{0.1, 0.2, 0.85};
            }
            return Texture(x + 4 * t + 1000, y + 1 * t + 1000, 17);
        };
        v.push_back(std::move(s));
    }
    {
        Sequence s;
        s.name = "zoom_rotate";
        s.description = "zoom 6% and rotation 3 deg about the centre between A and B";
        const double cx = w * 0.5, cy = h * 0.5;
        s.scene = [=](double x, double y, double t) {
            const double a = -3.0 * kPi / 180.0 * t, z = 1.0 + 0.06 * t;
            const double px = (x - cx) / z, py = (y - cy) / z;
            const double rx = px * std::cos(a) - py * std::sin(a), ry = px * std::sin(a) + py * std::cos(a);
            return Texture(rx + cx + 1000, ry + cy + 1000, 18);
        };
        v.push_back(std::move(s));
    }
    {
        Sequence s;
        s.name = "brightness_pan";
        s.description = "pan (6.2,1.7) px with gain 1 -> 1.3 and offset 0 -> +0.03 between A and B";
        s.scene = [=](double x, double y, double t) {
            Rgb c = Texture(x + 6.2 * t + 1000, y + 1.7 * t + 1000, 19);
            const double g = 1.0 + 0.3 * t, o = 0.03 * t;
            return Rgb{c.r * g + o, c.g * g + o, c.b * g + o};
        };
        v.push_back(std::move(s));
    }
    {
        Sequence s;
        s.name = "scene_cut";
        s.description = "unrelated pair: A and the frame before it from one scene, B from another";
        s.sceneCut = true;
        s.scene = [=](double x, double y, double t) {
            if (t < 0.999) return Texture(x + 3 * t + 1000, y + 1000, 20);
            const Rgb c = Texture(0.6 * x + 7000, 0.6 * y + 9000, 99);
            return Rgb{c.b * 1.1, c.r * 0.8, c.g};
        };
        v.push_back(std::move(s));
    }
    {
        Sequence s;
        s.name = "periodic_fence";
        s.description = "fence (4 px posts every 14 px, rails every 60 px) over faint texture, pan (23,1.5) px";
        s.scene = [=](double x, double y, double t) {
            const double u = x + 23.0 * t + 1000, v2 = y + 1.5 * t + 1000;
            const Rgb bg = Texture(u, v2, 21);
            const double pu = u - std::floor(u / 14.0) * 14.0, pv = v2 - std::floor(v2 / 60.0) * 60.0;
            if (pu < 4.0 || pv < 5.0) return Rgb{0.42, 0.27, 0.12};
            return Rgb{0.55 + 0.25 * (bg.r - 0.5), 0.6 + 0.25 * (bg.g - 0.5), 0.65 + 0.25 * (bg.b - 0.5)};
        };
        v.push_back(std::move(s));
    }
    return v;
}

// ---- sequences from a real captured frame ----
namespace {

// Nearest texel of the source at continuous source position (u, v), mirrored outside the image.
Rgb SrcNearest(const Image& s, double u, double v) {
    const auto mirror = [](double c, double n) {
        c = std::fmod(c, 2.0 * n);
        if (c < 0) c += 2.0 * n;
        return c < n ? c : 2.0 * n - c - 1e-6;
    };
    const uint32_t x = std::min<uint32_t>(s.w - 1, static_cast<uint32_t>(mirror(u, s.w)));
    const uint32_t y = std::min<uint32_t>(s.h - 1, static_cast<uint32_t>(mirror(v, s.h)));
    const uint8_t* p = &s.px[(static_cast<size_t>(y) * s.w + x) * 4];
    return {p[2] / 255.0, p[1] / 255.0, p[0] / 255.0};
}

struct RectD {
    double x0, y0, x1, y1;
    bool In(double x, double y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

} // namespace

std::vector<Sequence> MakeRealSequences(const Image& source, uint32_t w, uint32_t h) {
    std::vector<Sequence> v;
    if (source.Empty()) return v;
    auto src = std::make_shared<Image>(source);
    // Screen -> source pixel units (sequences are defined for a 1280x720 source; other sizes scale).
    const double k = static_cast<double>(src->w) / w, ky = static_cast<double>(src->h) / h;
    const double cx = src->w * 0.5, cy = src->h * 0.5;
    const double sx = src->w / 1280.0, sy = src->h / 720.0; // layout of the reference frame (ATLYSS, 1280x720)
    // Static overlay rectangles of the reference frame (minimap, quest list, action bar, counters), source units.
    const std::vector<RectD> hud{{0, 0, 178 * sx, 152 * sy}, {1020 * sx, 22 * sy, 1280 * sx, 302 * sy},
                                 {400 * sx, 636 * sy, 880 * sx, 720 * sy}, {1110 * sx, 560 * sy, 1280 * sx, 636 * sy}};
    const auto hudAt = [=](double x, double y) -> int {
        for (size_t i = 0; i < hud.size(); ++i)
            if (hud[i].In(x * k, y * ky)) return static_cast<int>(i);
        return -1;
    };
    {
        Sequence s;
        s.name = "real_turn";
        s.description = "real frame, camera turn: pan (20,4) px + 2.5% zoom out, point sampled (aliased)";
        s.samples = 1;
        s.scene = [=](double x, double y, double t) {
            const double u = x * k, vv = y * ky;
            const double z = 1.0 - 0.025 * t;
            return SrcNearest(*src, cx + (u - cx) * z + 20 * t, cy + (vv - cy) * z + 4 * t);
        };
        v.push_back(std::move(s));
    }
    for (uint32_t n : {1u, 3u}) {
        Sequence s;
        s.name = n == 1 ? "real_rotate" : "real_rotate_aa";
        s.description = std::string("real frame, camera roll 2 deg about a point below the frame + pan (6,0) px, ") +
                        (n == 1 ? "point sampled (aliased)" : "3x3 supersampled (anti-aliased)");
        s.samples = n;
        s.scene = [=](double x, double y, double t) {
            const double a = 2.0 * kPi / 180.0 * t, ca = std::cos(a), sa = std::sin(a);
            const double px = x * k - cx, py = y * ky - src->h * 1.2;
            return SrcNearest(*src, cx + ca * px - sa * py + 6 * t, src->h * 1.2 + sa * px + ca * py);
        };
        v.push_back(std::move(s));
    }
    for (uint32_t n : {1u, 3u}) {
        Sequence s;
        s.name = n == 1 ? "real_layers" : "real_layers_aa";
        s.description = std::string("real frame: background pan (10,2) + 1.5% zoom, elliptic sprite (character) moving (-24,5), "
                                    "slanted pillar sprite moving (30,0), static HUD rectangles; ") +
                        (n == 1 ? "point sampled (aliased)" : "3x3 supersampled (anti-aliased)");
        s.samples = n;
        s.scene = [=](double x, double y, double t) {
            const int hi = hudAt(x, y);
            const double u = x * k, vv = y * ky;
            if (hi >= 0) return SrcNearest(*src, u, vv);
            // Pillar: parallelogram 80 px wide, 300 tall, slanted 1:4, source origin (440, 10), moving (30, 0) t.
            {
                const double ox = 440 * sx, oy = 10 * sy;
                const double qx = u - (ox + 30 * t), qy = vv - oy;
                if (qy >= 0 && qy < 300 * sy && std::abs(qx - 0.25 * qy - 40 * sx) < 40 * sx) return SrcNearest(*src, ox + qx, oy + qy);
            }
            // Character: ellipse radii (70, 105) around source (640, 415), moving (-24, 5) t.
            {
                const double c1x = 640 * sx, c1y = 415 * sy;
                const double qx = u - (c1x - 24 * t), qy = vv - (c1y + 5 * t);
                if ((qx * qx) / (70 * sx * 70 * sx) + (qy * qy) / (105 * sy * 105 * sy) <= 1.0) return SrcNearest(*src, c1x + qx, c1y + qy);
            }
            const double z = 1.0 - 0.015 * t;
            return SrcNearest(*src, cx + (u - cx) * z + 10 * t, cy + (vv - cy) * z + 2 * t);
        };
        s.hudCoverage = [=](double x, double y) { return hudAt(x, y) >= 0 ? 1.0 : 0.0; };
        v.push_back(std::move(s));
    }
    return v;
}

Image Render(const Sequence& s, uint32_t w, uint32_t h, double t, uint32_t n) {
    if (n == 0) n = s.samples;
    Image img;
    img.Alloc(w, h);
    const double inv = 1.0 / (n * n);
    ParallelFor(h, [&](uint32_t y) {
        uint8_t* row = img.px.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0;
            for (uint32_t j = 0; j < n; ++j)
                for (uint32_t i = 0; i < n; ++i) {
                    const Rgb c = s.scene(x + (i + 0.5) / n, y + (j + 0.5) / n, t);
                    r += Clamp01(c.r);
                    g += Clamp01(c.g);
                    b += Clamp01(c.b);
                }
            row[x * 4 + 0] = static_cast<uint8_t>(std::lround(b * inv * 255.0));
            row[x * 4 + 1] = static_cast<uint8_t>(std::lround(g * inv * 255.0));
            row[x * 4 + 2] = static_cast<uint8_t>(std::lround(r * inv * 255.0));
            row[x * 4 + 3] = 255;
        }
    });
    return img;
}

Mask RenderHudMask(const Sequence& s, uint32_t w, uint32_t h) {
    Mask m;
    m.w = w;
    m.h = h;
    m.m.assign(static_cast<size_t>(w) * h, 0);
    if (!s.hudCoverage) return m;
    ParallelFor(h, [&](uint32_t y) {
        for (uint32_t x = 0; x < w; ++x) {
            double c = 0;
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) c += s.hudCoverage(x + (i + 0.5) / 3, y + (j + 0.5) / 3);
            // Fully covered pixels only: partially covered edge pixels contain moving background and are not overlay.
            m.m[static_cast<size_t>(y) * w + x] = c >= 9 ? 1 : 0;
        }
    });
    return m;
}

Image NoiseImage(uint32_t w, uint32_t h, uint32_t seed, int shiftX, int shiftY) {
    Image img;
    img.Alloc(w, h);
    ParallelFor(h, [&](uint32_t y) {
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = static_cast<int>(x) + shiftX, sy = static_cast<int>(y) + shiftY;
            // Smooth-ish random texture: 2-px value noise plus a coarser component (matchable, not white noise).
            const double v = 0.6 * ValueNoise(sx + 0.5, sy + 0.5, 3.0, seed) + 0.4 * ValueNoise(sx + 0.5, sy + 0.5, 17.0, seed + 1);
            const double v2 = ValueNoise(sx + 0.5, sy + 0.5, 5.0, seed + 2);
            uint8_t* p = img.px.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>(std::lround(255 * v2));
            p[1] = static_cast<uint8_t>(std::lround(255 * v));
            p[2] = static_cast<uint8_t>(std::lround(255 * (1 - v)));
        }
    });
    return img;
}

} // namespace fgb
