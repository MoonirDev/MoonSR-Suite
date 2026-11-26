// Procedural moving scenes for the temporal benchmark. Every scene is a pure function of (x, y, frame) at
// ground-truth resolution. Content is an infinite deterministic "world" (hashed grid of discs and rotated rectangles
// of two size classes, fractal value noise, 2-4 px grain) so that panning never runs out of texture and detail exists
// at every scale; the thin-pattern and rotating-element scenes add the cases the world does not cover.
#include "temporal.h"

#include "imageops.h"

#include <cmath>
#include <numbers>

namespace bench {

namespace {

using Rgb = Sequence::Rgb;
constexpr double kPi = std::numbers::pi;

inline uint32_t Hash32(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352dU; v ^= v >> 15; v *= 0x846ca68bU; v ^= v >> 16;
    return v;
}
inline uint32_t Hash2(int32_t x, int32_t y, uint32_t seed) {
    return Hash32(static_cast<uint32_t>(x) * 0x9E3779B1U ^ Hash32(static_cast<uint32_t>(y) + seed * 0x85EBCA6BU));
}
inline double Unit(uint32_t h) { return (h >> 8) * (1.0 / 16777216.0); }
inline Rgb Mix(Rgb a, Rgb b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
inline Rgb Gray(double v) { return {v, v, v}; }
inline double Smooth(double e0, double e1, double v) {
    const double t = std::clamp((v - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

double ValueNoise(double x, double y, uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    const double a = Unit(Hash2(ix, iy, seed)), b = Unit(Hash2(ix + 1, iy, seed));
    const double c = Unit(Hash2(ix, iy + 1, seed)), d = Unit(Hash2(ix + 1, iy + 1, seed));
    const double top = a + (b - a) * tx, bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}
double Fbm(double x, double y, double period, int octaves, uint32_t seed) {
    double sum = 0, amp = 0.5, norm = 0, f = 1.0 / period;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * ValueNoise(x * f, y * f, seed + static_cast<uint32_t>(o) * 101);
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}

// One hard-edged shape per grid cell (with probability `density`); the 3x3 neighbourhood is tested, which covers every
// shape of radius <= 0.5 cell. Later cells overwrite earlier ones, so the result is a deterministic painter's order.
Rgb Shapes(Rgb c, double x, double y, double cell, double rmin, double rmax, double aspectMin, uint32_t seed, double density) {
    const int cx0 = static_cast<int>(std::floor(x / cell)), cy0 = static_cast<int>(std::floor(y / cell));
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = cx0 + dx, cy = cy0 + dy;
            const uint32_t h = Hash2(cx, cy, seed);
            if (Unit(h) > density) continue;
            const double px = (cx + 0.15 + 0.7 * Unit(Hash32(h ^ 0x1111u))) * cell;
            const double py = (cy + 0.15 + 0.7 * Unit(Hash32(h ^ 0x2222u))) * cell;
            const double r = rmin + (rmax - rmin) * Unit(Hash32(h ^ 0x3333u));
            const Rgb col{Unit(Hash32(h ^ 0x5555u)), Unit(Hash32(h ^ 0x6666u)), Unit(Hash32(h ^ 0x7777u))};
            const double ox = x - px, oy = y - py;
            if ((Hash32(h ^ 0x4444u) & 1u) == 0) {
                if (ox * ox + oy * oy < r * r) c = col;
            } else {
                const double a = Unit(Hash32(h ^ 0x8888u)) * kPi, ca = std::cos(a), sa = std::sin(a);
                const double hh = r * (aspectMin + (0.6 - aspectMin) * Unit(Hash32(h ^ 0x9999u)));
                const double u = ox * ca + oy * sa, v = -ox * sa + oy * ca;
                if (std::abs(u) < r && std::abs(v) < hh) c = col;
            }
        }
    return c;
}

// The base texture. `k` scales every length (k = H / 1080) so other ground-truth sizes keep the same look.
Rgb World(double x, double y, uint32_t seed, double k) {
    const double n = Fbm(x, y, 260 * k, 5, seed), m = Fbm(x, y, 140 * k, 4, seed + 9);
    Rgb c = Mix(Rgb{0.28, 0.36, 0.22}, Rgb{0.78, 0.68, 0.46}, n);
    c = Mix(c, Rgb{0.30, 0.45, 0.70}, Smooth(0.45, 0.65, m) * 0.6);
    c = Shapes(c, x, y, 200 * k, 30 * k, 95 * k, 0.15, seed + 31, 0.75);  // large objects
    c = Shapes(c, x, y, 44 * k, 2.2 * k, 12 * k, 0.07, seed + 57, 0.8);   // small, partly thin objects
    const double g = (ValueNoise(x / 2.0, y / 2.0, seed + 77) - 0.5) * 0.10 + (ValueNoise(x / 4.0, y / 4.0, seed + 78) - 0.5) * 0.10;
    return {c.r + g, c.g + g, c.b + g};
}

inline bool InRect(double x, double y, double x0, double y0, double x1, double y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }
inline Rgb Over(Rgb under, Rgb col, double a) { return Mix(under, col, a); }

// Seven-segment digit in a 28 x 52 cell (lx, ly in cell pixels scaled by k).
bool SegDigit(double lx, double ly, int d, double k) {
    static const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F}; // gfedcba
    const double t = 3.5 * k, w = 28 * k, h = 52 * k, hm = h * 0.5;
    const uint8_t s = kSeg[d % 10];
    if ((s & 0x01) && InRect(lx, ly, t, 0, w - t, t)) return true;                        // a top
    if ((s & 0x02) && InRect(lx, ly, w - t, t, w, hm)) return true;                       // b top right
    if ((s & 0x04) && InRect(lx, ly, w - t, hm, w, h - t)) return true;                   // c bottom right
    if ((s & 0x08) && InRect(lx, ly, t, h - t, w - t, h)) return true;                    // d bottom
    if ((s & 0x10) && InRect(lx, ly, 0, hm, t, h - t)) return true;                       // e bottom left
    if ((s & 0x20) && InRect(lx, ly, 0, t, t, hm)) return true;                           // f top left
    if ((s & 0x40) && InRect(lx, ly, t, hm - t * 0.5, w - t, hm + t * 0.5)) return true;  // g middle
    return false;
}

// Static screen-space overlay (HUD): health bar, score readout on a translucent panel, ammo ticks, crosshair.
Rgb HudOverlay(Rgb c, double x, double y, double k) {
    const double X = x / k, Y = y / k; // overlay is authored on a 1920x1080 canvas
    if (InRect(X, Y, 48, 36, 448, 68)) { // health bar
        c = Over(c, Gray(0.05), 0.9);
        if (InRect(X, Y, 52, 40, 444, 64)) {
            c = Gray(0.15);
            if (X < 52 + 0.7 * 392) c = Mix(Rgb{0.15, 0.80, 0.22}, Rgb{0.35, 0.95, 0.30}, (Y - 40) / 24.0);
            const double tick = std::fmod(X - 52 + 4096, 39.2);
            if (tick < 1.0) c = Over(c, Gray(0.95), 0.5);
        }
    }
    if (InRect(X, Y, 1560, 30, 1880, 96)) { // score panel and digits
        c = Over(c, Gray(0.0), 0.5);
        static const int kDigits[6] = {3, 1, 0, 4, 2, 7};
        for (int i = 0; i < 6; ++i) {
            const double ox = 1572 + i * 50, oy = 38;
            if (InRect(X, Y, ox, oy, ox + 28, oy + 52) && SegDigit((X - ox) * k, (Y - oy) * k, kDigits[i], k)) c = Rgb{0.96, 0.95, 0.70};
        }
    }
    for (int i = 0; i < 24; ++i) { // ammo ticks
        const double tx = 60 + i * 12;
        if (InRect(X, Y, tx, 1010, tx + 3, 1036)) c = i < 16 ? Rgb{0.95, 0.60, 0.10} : Gray(0.35);
    }
    // crosshair: 1 px lines with a gap in the middle
    const bool hArm = Y >= 539.5 && Y < 540.5 && ((X >= 944 && X < 954) || (X >= 966 && X < 976));
    const bool vArm = X >= 959.5 && X < 960.5 && ((Y >= 524 && Y < 534) || (Y >= 546 && Y < 556));
    if (hArm || vArm) c = Over(c, Gray(0.97), 0.92);
    return c;
}

// Thin-line / high-frequency patterns: four screen-space zones, texture moves with the pan (u, v).
Rgb ThinZones(double x, double y, double u, double v, double w, double h, double k) {
    const Rgb bg = Gray(0.92), ink = Gray(0.05);
    auto band = [](double a, double period, double width) { return std::fmod(a + 65536.0 * period, period) < width; };
    const double gx = 4 * k;
    if (std::abs(x - w * 0.5) < gx * 0.5 || std::abs(y - h * 0.5) < gx * 0.5) return Gray(0.5); // static dividers
    const bool right = x > w * 0.5, bottom = y > h * 0.5;
    if (!right && !bottom) return band(v, 6 * k, 1 * k) ? ink : bg;                       // horizontal, 1 px
    if (right && !bottom) return band(u, 7 * k, 1.5 * k) ? ink : bg;                      // vertical, 1.5 px
    if (!right && bottom) return band((u + v) / std::sqrt(2.0), 8 * k, 1 * k) ? ink : bg; // 45 degrees, 1 px
    return band(std::hypot(u - w * 0.75, v - h * 0.75), 7 * k, 1 * k) ? ink : bg;         // rings, 1 px
}

// Periodic, independently drifting soft particles, composited over the opaque pan.
double ParticleOpacity(double x, double y, int t, double k) {
    const double u = (x / k + 1.7 * t) / 64;
    const double v = (y / k - 2.1 * t) / 64;
    const double tileX = std::floor(u), tileY = std::floor(v);
    const double offsetX = 0.2 * std::sin(2.7 * tileX + 4.3 * tileY);
    const double offsetY = 0.2 * std::sin(4.1 * tileX - 2.1 * tileY);
    const double px = u - tileX - 0.5 - offsetX, py = v - tileY - 0.5 - offsetY;
    const double falloff = std::max(0.0, 1.0 - (px * px + py * py) / 0.0625);
    return 0.7 * falloff * falloff;
}

int MotionSteps(int frame, int stop, int resume)
{
    return std::min(frame, stop - 1) + std::max(0, frame - resume + 1);
}

double DiscAngle(int t) { return 0.022 * t; }
double DiscZoom(int t) { return 1.0 + 0.25 * std::sin(2.0 * kPi * t / 48.0); }

uint8_t Q(double v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v * 255.0)), 0, 255)); }

} // namespace

const std::vector<std::string>& SequenceNames() {
    static const std::vector<std::string> n = {"pan_slow", "pan_medium", "pan_fast", "rot_zoom", "hud_pan", "scene_cut", "thin_pan", "particles", "still"};
    return n;
}

std::unique_ptr<Sequence> Sequence::Create(const std::string& name, uint32_t gtW, uint32_t gtH, int frames, int ss) {
    auto s = std::unique_ptr<Sequence>(new Sequence());
    s->name_ = name;
    s->w_ = gtW;
    s->h_ = gtH;
    s->frames_ = std::max(1, frames);
    s->ss_ = std::max(1, ss);
    const double k = gtH / 1080.0;
    const double W = gtW, H = gtH;
    constexpr uint32_t kSeedA = 1000, kSeedB = 2000;
    auto pan = [k](uint32_t seed, double vx, double vy) -> EvalFn {
        return [=](double x, double y, int t) { return World(x - vx * k * t, y - vy * k * t, seed, k); };
    };
    if (name == "pan_slow") {
        s->eval_ = pan(kSeedA, 1.1, 0.5);
        s->panX_ = 1.1 * k; s->panY_ = 0.5 * k;
    } else if (name == "pan_medium") {
        s->panX_ = 5.1 * k; s->panY_ = 1.7 * k;
        s->eval_ = pan(kSeedA, 5.1, 1.7);
    } else if (name == "pan_fast") {
        s->eval_ = pan(kSeedA, 17.3, 6.1);
        s->panX_ = 17.3 * k; s->panY_ = 6.1 * k;
    } else if (name == "hud_pan") {
        EvalFn base = pan(kSeedA, 3.7, 1.3);
        s->panX_ = 3.7 * k; s->panY_ = 1.3 * k;
        s->eval_ = [=](double x, double y, int t) { return HudOverlay(base(x, y, t), x, y, k); };
        const int d = 6;
        for (const HudRect& r : {HudRect{48 - d, 36 - d, 448 + d, 68 + d}, HudRect{1560 - d, 30 - d, 1880 + d, 96 + d},
                                 HudRect{60 - d, 1010 - d, 60 + 24 * 12 + d, 1036 + d}, HudRect{944 - d, 524 - d, 976 + d, 556 + d}})
            s->hud_.push_back({static_cast<int>(std::floor(r.x0 * k)), static_cast<int>(std::floor(r.y0 * k)),
                               std::min(static_cast<int>(gtW), static_cast<int>(std::ceil(r.x1 * k))),
                               std::min(static_cast<int>(gtH), static_cast<int>(std::ceil(r.y1 * k)))});
    } else if (name == "scene_cut") {
        s->cut_ = s->frames_ / 2;
        const int cut = s->cut_;
        s->panX_ = 2.3 * k; s->panY_ = 0.9 * k;
        s->postCutPanX_ = -1.7 * k; s->postCutPanY_ = 1.4 * k;
        EvalFn a = pan(kSeedA, 2.3, 0.9), b = pan(kSeedB, -1.7, 1.4);
        s->eval_ = [=](double x, double y, int t) { return t < cut ? a(x, y, t) : b(x, y, t - cut); };
    } else if (name == "rot_zoom") {
        s->eval_ = [=](double x, double y, int t) {
            const double theta = DiscAngle(t), zoom = DiscZoom(t);
            const double px = x - W * 0.5, py = y - H * 0.5;
            const double ca = std::cos(theta), sa = std::sin(theta);
            const double lx = (px * ca + py * sa) / zoom, ly = (-px * sa + py * ca) / zoom; // rotate by -theta, scale 1/zoom
            const double R = 280 * k, r = std::hypot(lx, ly);
            if (r < R) {
                if (r > R - 7 * k) return Gray(0.06); // dark rim
                const double ang = std::atan2(ly, lx);
                const double spoke = std::fmod(ang * 8.0 / (2 * kPi) + 64.0, 1.0); // 8 spokes
                if (std::abs(spoke - 0.5) * (2 * kPi * r / 8.0) < 1.6 * k) return Gray(0.95); // light spokes, ~3 px wide
                return World(lx + 5000 * k, ly, kSeedB + 7, k);
            }
            return World(x, y, kSeedA, k);
        };
    } else if (name == "thin_pan") {
        const double vx = 0.73 * k, vy = 0.41 * k;
        s->panX_ = 0.73 * k; s->panY_ = 0.41 * k;
        s->eval_ = [=](double x, double y, int t) { return ThinZones(x, y, x - vx * t, y - vy * t, W, H, k); };
    } else if (name == "particles") {
        s->panX_ = 3.7 * k; s->panY_ = 1.3 * k;
        const auto base = pan(kSeedA, 3.7, 1.3);
        s->eval_ = [=](double x, double y, int t) {
            const auto bg = base(x, y, t);
            const double a = ParticleOpacity(x, y, t, k);
            return Rgb{bg.r * (1 - a) + a, bg.g * (1 - a) + 0.7 * a, bg.b * (1 - a) + 0.35 * a};
        };
    } else if (name == "start_stop") {
        s->stop_ = std::max(1, s->frames_ / 4);
        s->resume_ = std::max(s->stop_ + 1, 3 * s->frames_ / 4);
        s->panX_ = 3.7 * k;
        s->panY_ = 1.3 * k;
        const auto base = pan(kSeedA, 3.7, 1.3);
        const int stop = s->stop_, resume = s->resume_;
        s->eval_ = [=](double x, double y, int t) { return base(x, y, MotionSteps(t, stop, resume)); };
    } else if (name == "still") {
        s->static_ = true;
        s->eval_ = [=](double x, double y, int) { return World(x, y, kSeedA, k); };
    } else {
        return nullptr;
    }
    return s;
}

std::unique_ptr<Sequence> Sequence::CreateRecorded(uint32_t gtW, uint32_t gtH, int frames) {
    auto s = std::unique_ptr<Sequence>(new Sequence());
    s->name_ = "native_recording";
    s->w_ = gtW;
    s->h_ = gtH;
    s->frames_ = frames;
    s->recorded_ = true;
    return s;
}

std::unique_ptr<Sequence> Sequence::CreatePan(uint32_t gtW, uint32_t gtH, int frames, int ss, double vx, double vy) {
    auto s = std::unique_ptr<Sequence>(new Sequence());
    s->name_ = "pan_test";
    s->panX_ = vx; s->panY_ = vy;
    s->w_ = gtW;
    s->h_ = gtH;
    s->frames_ = std::max(1, frames);
    s->ss_ = std::max(1, ss);
    const double k = gtH / 1080.0;
    s->eval_ = [=](double x, double y, int t) { return World(x - vx * t, y - vy * t, 1000, k); };
    return s;
}

const char* Sequence::ContentClass() const {
    if (recorded_) return "recorded";
    if (name_ == "still") return "static";
    if (name_ == "start_stop") return "motion_transitions";
    if (name_ == "pan_medium") return "medium_pan";
    if (name_ == "pan_fast") return "fast_pan";
    if (name_ == "rot_zoom") return "rotation_zoom";
    if (name_ == "hud_pan") return "hud";
    if (name_ == "particles") return "particles_translucency";
    if (name_ == "scene_cut") return "scene_cut";
    if (name_ == "thin_pan") return "thin_features";
    return "slow_pan";
}

bool Sequence::BackwardFlow(double x, double y, int frame, double& dx, double& dy) const {
    if (recorded_ || frame <= 0 || frame == cut_) return false;
    const double k = h_ / 1080.0;
    const bool afterCut = cut_ >= 0 && frame > cut_;
    dx = -(afterCut ? postCutPanX_ : panX_);
    dy = -(afterCut ? postCutPanY_ : panY_);
    if (stop_ >= 0 && MotionSteps(frame, stop_, resume_) == MotionSteps(frame - 1, stop_, resume_))
        dx = dy = 0.0;

    if (name_ == "rot_zoom") {
        const double px = x - w_ * 0.5, py = y - h_ * 0.5;
        const double radius = std::hypot(px, py);
        const double current = 280 * k * DiscZoom(frame);
        const double previous = 280 * k * DiscZoom(frame - 1);
        if (radius < current) {
            const double theta = DiscAngle(frame - 1) - DiscAngle(frame);
            const double ratio = DiscZoom(frame - 1) / DiscZoom(frame);
            dx = ratio * (std::cos(theta) * px - std::sin(theta) * py) - px;
            dy = ratio * (std::sin(theta) * px + std::cos(theta) * py) - py;
        } else if (radius < previous) {
            return false; // newly visible background
        }
    }
    if (name_ == "thin_pan") {
        // A sample crossing the static divider has no match in the translated pattern.
        const double px = x - w_ * 0.5, py = y - h_ * 0.5;
        if (std::abs(px) < 2 * k || std::abs(py) < 2 * k) return false;
        if (std::abs(px + dx) < 2 * k || std::abs(py + dy) < 2 * k ||
            (px < 0) != (px + dx < 0) || (py < 0) != (py + dy < 0)) return false;
    }
    if (name_ == "particles" &&
        (ParticleOpacity(x, y, frame, k) > 0 || ParticleOpacity(x + dx, y + dy, frame - 1, k) > 0)) return false;
    for (const auto& r : hud_) {
        const auto inside = [&r](double a, double b) { return a >= r.x0 && a < r.x1 && b >= r.y0 && b < r.y1; };
        // HUD rectangles include translucency and padding, so their composited flow is ambiguous.
        if (inside(x, y) || inside(x + dx, y + dy)) return false;
    }
    return x + dx >= 0 && x + dx < w_ && y + dy >= 0 && y + dy < h_;
}

Image Sequence::GroundTruth(int frame) const {
    Image img;
    img.Alloc(w_, h_);
    ParallelFor(h_, [&](uint32_t y) {
        uint8_t* row = img.Row(y);
        for (uint32_t x = 0; x < w_; ++x) {
            double r = 0, g = 0, b = 0;
            for (int j = 0; j < ss_; ++j)
                for (int i = 0; i < ss_; ++i) {
                    const Rgb c = eval_(x + (i + 0.5) / ss_, y + (j + 0.5) / ss_, frame);
                    r += c.r; g += c.g; b += c.b;
                }
            const double inv = 1.0 / (ss_ * ss_);
            row[x * 4 + 0] = Q(b * inv);
            row[x * 4 + 1] = Q(g * inv);
            row[x * 4 + 2] = Q(r * inv);
        }
    });
    return img;
}

Image Sequence::InputPoint(int frame, uint32_t iw, uint32_t ih) const {
    Image img;
    img.Alloc(iw, ih);
    const double sx = static_cast<double>(w_) / iw, sy = static_cast<double>(h_) / ih;
    ParallelFor(ih, [&](uint32_t y) {
        uint8_t* row = img.Row(y);
        for (uint32_t x = 0; x < iw; ++x) {
            const Rgb c = eval_((x + 0.5) * sx, (y + 0.5) * sy, frame);
            row[x * 4 + 0] = Q(c.b);
            row[x * 4 + 1] = Q(c.g);
            row[x * 4 + 2] = Q(c.r);
        }
    });
    return img;
}

Image Sequence::InputArea(const Image& gt, uint32_t iw, uint32_t ih) { return AreaDownsample(gt, iw, ih); }

} // namespace bench
