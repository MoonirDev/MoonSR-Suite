// Deterministic synthetic ground-truth generator. Every pattern is a continuous function of pixel position that is
// box-filtered by regular supersampling (so edges and lines are anti-aliased, not hard aliased) and quantised to 8-bit
// sRGB-encoded values. Supersampling averages the encoded values (no linearisation), consistent with the rest of the
// harness. Only the text image depends on the system fonts (Segoe UI / Consolas, rendered by GDI).
#include "gen.h"

#include <windows.h>

#include <cmath>
#include <numbers>

namespace bench {

namespace {

struct V3 {
    double r = 0, g = 0, b = 0;
};
inline V3 Mix(V3 a, V3 b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
inline V3 Gray(double v) { return {v, v, v}; }
constexpr double kPi = std::numbers::pi;

inline uint32_t Hash32(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16;
    return v;
}
inline uint32_t Hash2(int32_t x, int32_t y, uint32_t seed) {
    return Hash32(static_cast<uint32_t>(x) * 0x9E3779B1U ^ Hash32(static_cast<uint32_t>(y) + seed * 0x85EBCA6BU));
}
inline double Unit(uint32_t h) { return (h >> 8) * (1.0 / 16777216.0); } // [0,1)

double ValueNoise(double x, double y, uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    const double a = Unit(Hash2(ix, iy, seed)), b = Unit(Hash2(ix + 1, iy, seed));
    const double c = Unit(Hash2(ix, iy + 1, seed)), d = Unit(Hash2(ix + 1, iy + 1, seed));
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}
double Fbm(double x, double y, double period, int octaves, uint32_t seed) {
    double sum = 0, amp = 0.5, norm = 0, f = 1.0 / period;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * ValueNoise(x * f, y * f, seed + o * 101);
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}
inline double Smooth(double e0, double e1, double v) {
    const double t = std::clamp((v - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

using PixelFn = std::function<V3(double, double)>;

// Renders fn with ss x ss regular supersampling; noise adds per-pixel triangular noise of the given amplitude (in 8-bit
// levels) to each channel before quantisation.
Image Render(uint32_t w, uint32_t h, int ss, const PixelFn& fn, double noise = 0.0, uint32_t seed = 1) {
    Image img;
    img.Alloc(w, h);
    ParallelFor(h, [&](uint32_t y) {
        uint8_t* row = img.Row(y);
        for (uint32_t x = 0; x < w; ++x) {
            V3 acc;
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const V3 c = fn(x + (i + 0.5) / ss, y + (j + 0.5) / ss);
                    acc.r += c.r; acc.g += c.g; acc.b += c.b;
                }
            const double inv = 1.0 / (ss * ss);
            const double ch[3] = {acc.b * inv, acc.g * inv, acc.r * inv}; // BGR order
            for (int c = 0; c < 3; ++c) {
                double v = ch[c] * 255.0;
                if (noise > 0) {
                    const double n1 = Unit(Hash2(static_cast<int32_t>(x), static_cast<int32_t>(y), seed * 7 + c * 2));
                    const double n2 = Unit(Hash2(static_cast<int32_t>(x), static_cast<int32_t>(y), seed * 7 + c * 2 + 1));
                    v += (n1 + n2 - 1.0) * noise;
                }
                row[x * 4 + c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
            }
        }
    });
    return img;
}

// ---------------------------------------------------------------------------------------------------------------

Image ZonePlate(uint32_t w, uint32_t h) {
    const double cx = w * 0.5, cy = h * 0.5, rmax = std::hypot(cx, cy);
    // Local frequency = r / (2 rmax) cycles per pixel: 0 at the centre, exactly 0.5 (Nyquist) at the corners.
    return Render(w, h, 3, [=](double x, double y) {
        const double r2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        return Gray(0.5 + 0.5 * std::cos(kPi * r2 / (2.0 * rmax)));
    });
}

Image SlantedEdges(uint32_t w, uint32_t h) {
    static const double kAngles[6] = {2, 5, 15, 30, 45, 75};
    const V3 dark{0.12, 0.14, 0.22}, light{0.90, 0.86, 0.78}, gutter = Gray(0.5);
    const uint32_t tw = w / 3, th = h / 2;
    return Render(w, h, 8, [=](double x, double y) {
        const uint32_t col = std::min<uint32_t>(2, static_cast<uint32_t>(x) / tw);
        const uint32_t row = std::min<uint32_t>(1, static_cast<uint32_t>(y) / th);
        const double ox = col * static_cast<double>(tw), oy = row * static_cast<double>(th);
        const double lx = x - ox, ly = y - oy;
        const double m = 12;
        if (lx < m || ly < m || lx > tw - m || ly > th - m) return gutter;
        const double a = kAngles[row * 3 + col] * kPi / 180.0;
        // Edge through the tile centre with direction (cos a, sin a); the offset keeps the crossing phase non-trivial.
        const double d = (lx - tw * 0.5 + 0.31) * -std::sin(a) + (ly - th * 0.5 + 0.17) * std::cos(a);
        return d > 0 ? light : dark;
    });
}

Image ThinLines(uint32_t w, uint32_t h) {
    const V3 bg = Gray(0.92), ink = Gray(0.05);
    const uint32_t tw = w / 3, th = h / 2;
    return Render(w, h, 4, [=](double x, double y) {
        const uint32_t col = std::min<uint32_t>(2, static_cast<uint32_t>(x) / tw);
        const uint32_t row = std::min<uint32_t>(1, static_cast<uint32_t>(y) / th);
        const double lx = x - col * static_cast<double>(tw), ly = y - row * static_cast<double>(th);
        const double cx = tw * 0.5, cy = th * 0.5;
        const uint32_t zone = row * 3 + col;
        auto band = [](double v, double period, double width) { return std::fmod(v + 4096 * period, period) < width; };
        switch (zone) {
        case 0: return band(ly, 6, 1) ? ink : bg;                          // horizontal, 1 px, period 6
        case 1: return band(lx, 8, 2) ? ink : bg;                          // vertical, 2 px, period 8
        case 2: return band((lx + ly) / std::sqrt(2.0), 8, 1) ? ink : bg;  // 45 deg diagonal, 1 px
        case 3: {                                                          // 20 deg, 1.5 px
            const double a = 20.0 * kPi / 180.0;
            return band(-lx * std::sin(a) + ly * std::cos(a), 10, 1.5) ? ink : bg;
        }
        case 4: return band(std::hypot(lx - cx, ly - cy), 7, 1) ? ink : bg; // concentric rings, 1 px
        default: {                                                          // sparse coloured 1 px lines on mid gray
            const V3 g = Gray(0.5);
            if (band(ly, 40, 1)) return V3{0.95, 0.1, 0.1};
            if (band(lx, 40, 1)) return V3{0.1, 0.85, 0.1};
            if (band((lx - ly) / std::sqrt(2.0), 40, 1)) return V3{0.15, 0.25, 0.95};
            return g;
        }
        }
    });
}

Image Checker(uint32_t w, uint32_t h) {
    const V3 a = Gray(0.05), b = Gray(0.95);
    const uint32_t bw = w / 4;
    return Render(w, h, 4, [=](double x, double y) {
        const uint32_t band = std::min<uint32_t>(3, static_cast<uint32_t>(x) / bw);
        const double lx = x - band * static_cast<double>(bw);
        if (band < 3) {
            const int cell = 1 << band; // 1, 2, 4 px, pixel aligned
            return ((static_cast<int>(std::floor(lx / cell)) + static_cast<int>(std::floor(y / cell))) & 1) ? b : a;
        }
        const double ang = 15.0 * kPi / 180.0, s = std::sin(ang), c = std::cos(ang); // 3 px cells rotated 15 deg
        const double u = lx * c + y * s, v = -lx * s + y * c;
        return ((static_cast<int>(std::floor(u / 3.0)) + static_cast<int>(std::floor(v / 3.0))) & 1) ? b : a;
    });
}

Image GradientNoise(uint32_t w, uint32_t h) {
    return Render(w, h, 1, [=](double x, double y) {
        const double u = x / w, v = y / h;
        if (u < 0.5 && v < 0.5) return Gray(u * 2.0);                         // horizontal ramp 0..1
        if (u >= 0.5 && v < 0.5) {                                             // vertical ramp, tinted
            const double t = v * 2.0;
            return V3{t, t * 0.8 + 0.1, 1.0 - t};
        }
        if (u < 0.5) {                                                         // smooth radial colour field
            const double r = std::hypot(u - 0.25, v - 0.75) * 4.0;
            return V3{0.5 + 0.5 * std::cos(2.0 * r), 0.5 + 0.5 * std::cos(2.0 * r + 2.1), 0.5 + 0.5 * std::cos(2.0 * r + 4.2)};
        }
        const double t = ((u - 0.5) + (v - 0.5)) * 1.0;                        // low-contrast diagonal (banding-prone)
        return Mix(V3{0.40, 0.45, 0.55}, V3{0.60, 0.55, 0.45}, t);
    }, 2.0, 3);
}

struct Disc {
    double x, y, r, soft, alpha;
    V3 col;
};

Image Natural(uint32_t w, uint32_t h) {
    uint32_t s = 12345;
    auto rnd = [&]() { return Unit(s = Hash32(s + 0x9E3779B9U)); };
    const double k = h / 1080.0;
    std::vector<Disc> discs;
    for (int i = 0; i < 70; ++i) {
        Disc d;
        d.x = rnd() * w;
        d.y = rnd() * h;
        d.r = k * 15.0 * std::pow(260.0 / 15.0, rnd());
        d.soft = 0.7 * std::pow(40.0 / 0.7, rnd());
        d.alpha = 0.6 + 0.4 * rnd();
        d.col = {rnd(), rnd(), rnd()};
        discs.push_back(d);
    }
    struct Rect { double cx, cy, hw, hh, ang; V3 col; };
    std::vector<Rect> rects;
    for (int i = 0; i < 14; ++i)
        rects.push_back({rnd() * w, rnd() * h, k * (10 + 90 * rnd()), k * (5 + 50 * rnd()), (rnd() - 0.5) * kPi, {rnd(), rnd(), rnd()}});
    return Render(w, h, 4, [=](double x, double y) {
        const double n = Fbm(x, y, 300 * k, 6, 11), m = Fbm(x, y, 170 * k, 5, 29);
        V3 c = Mix(V3{0.25, 0.35, 0.20}, V3{0.80, 0.70, 0.45}, n);
        c = Mix(c, V3{0.30, 0.45, 0.70}, Smooth(0.45, 0.65, m) * 0.6);
        for (const Disc& d : discs) {
            const double dist = std::hypot(x - d.x, y - d.y);
            const double cov = 1.0 - Smooth(d.r - d.soft, d.r + d.soft * 0.5 + 0.25, dist);
            if (cov > 0) c = Mix(c, d.col, cov * d.alpha);
        }
        for (const Rect& r : rects) {
            const double ca = std::cos(r.ang), sa = std::sin(r.ang);
            const double u = (x - r.cx) * ca + (y - r.cy) * sa, v = -(x - r.cx) * sa + (y - r.cy) * ca;
            if (std::abs(u) < r.hw && std::abs(v) < r.hh) c = r.col;
        }
        // Fine texture (grain with 2-4 px features) so the image also contains genuine high-frequency detail.
        const double g = (ValueNoise(x / 2.0, y / 2.0, 77) - 0.5) * 0.10 + (ValueNoise(x / 4.0, y / 4.0, 78) - 0.5) * 0.10;
        return V3{c.r + g, c.g + g, c.b + g};
    });
}

// GDI text. Greyscale anti-aliasing (not ClearType) so the image is colour-neutral apart from the coloured samples.
Image Text(uint32_t w, uint32_t h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_LEFT | TA_TOP);

    auto fill = [&](int x0, int y0, int x1, int y1, COLORREF c) {
        HBRUSH br = CreateSolidBrush(c);
        RECT r{x0, y0, x1, y1};
        FillRect(dc, &r, br);
        DeleteObject(br);
    };
    const int half = static_cast<int>(w) / 2;
    fill(0, 0, half, static_cast<int>(h), RGB(255, 255, 255));
    fill(half, 0, static_cast<int>(w), static_cast<int>(h), RGB(26, 30, 46));

    const wchar_t* kLines[] = {L"The quick brown fox jumps over the lazy dog 0123456789",
                               L"HP 100/100   AMMO 24/90   FPS 143   x:-12.5 y:303.0"};
    struct Style { const wchar_t* face; int px; };
    const Style styles[] = {{L"Segoe UI", 9},  {L"Segoe UI", 10}, {L"Segoe UI", 11}, {L"Segoe UI", 12}, {L"Segoe UI", 13},
                            {L"Segoe UI", 14}, {L"Segoe UI", 16}, {L"Segoe UI", 18}, {L"Segoe UI", 22}, {L"Segoe UI", 28},
                            {L"Segoe UI", 36}, {L"Segoe UI", 34}, {L"Consolas", 10}, {L"Consolas", 12}, {L"Consolas", 14},
                            {L"Consolas", 16}, {L"Consolas", 20}};
    const COLORREF darkCols[] = {RGB(0, 0, 0), RGB(0, 0, 0), RGB(200, 20, 20), RGB(20, 20, 170)};
    const COLORREF lightCols[] = {RGB(255, 255, 255), RGB(230, 230, 230), RGB(255, 190, 40), RGB(80, 220, 120), RGB(120, 170, 255)};
    for (int col = 0; col < 2; ++col) {
        int y = 16;
        int idx = 0;
        const int x0 = col * half + 24;
        for (const Style& st : styles) {
            HFONT f = CreateFontW(-st.px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, st.face);
            HGDIOBJ of = SelectObject(dc, f);
            SetTextColor(dc, col == 0 ? darkCols[idx % 4] : lightCols[idx % 5]);
            const wchar_t* t = kLines[idx % 2];
            TextOutW(dc, x0, y, t, static_cast<int>(wcslen(t)));
            SelectObject(dc, of);
            DeleteObject(f);
            y += st.px + st.px / 2 + 8;
            ++idx;
        }
    }
    GdiFlush();
    Image img;
    img.Alloc(w, h);
    const auto* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        img.px[i * 4 + 0] = src[i * 4 + 0];
        img.px[i * 4 + 1] = src[i * 4 + 1];
        img.px[i * 4 + 2] = src[i * 4 + 2];
        img.px[i * 4 + 3] = 255;
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return img;
}

} // namespace

const std::vector<std::string>& GeneratorNames() {
    static const std::vector<std::string> n = {"zone_plate", "slanted_edges", "thin_lines", "checker",
                                               "text",       "gradient_noise", "natural"};
    return n;
}

bool Generate(const std::string& name, uint32_t w, uint32_t h, Image& out) {
    if (name == "zone_plate") out = ZonePlate(w, h);
    else if (name == "slanted_edges") out = SlantedEdges(w, h);
    else if (name == "thin_lines") out = ThinLines(w, h);
    else if (name == "checker") out = Checker(w, h);
    else if (name == "text") out = Text(w, h);
    else if (name == "gradient_noise") out = GradientNoise(w, h);
    else if (name == "natural") out = Natural(w, h);
    else return false;
    return true;
}

} // namespace bench
