// moonsr_bench --relief: Smart parallax (screen-space relief) measurement, milestone M0 of
// documentation/research/screen_space_relief.md. Owner: upscaling-engineer (relief mode of the bench tool).
//
// Synthetic scenes with KNOWN height maps are rendered procedurally as albedo x baked cavity (the "flat" picture a game
// shows; what MoonSR captures) and, as ground truth, the same surface lit by the relief light (albedo x cavity x
// N.L shading with the true normals). The real ReliefPass runs on the GPU (Analyse + Apply, 1:1 size) over a slow
// diagonal pan ((0.25, 0.1) px per frame, HUD static), and the following are measured on the read-back output:
//   - height: Pearson / Spearman of the estimated H (Height debug view) against the band-passed true height, and the
//     sign accuracy, on truly textured pixels;
//   - shading fidelity: Pearson of the produced luminance change against the true relief shading change;
//   - false relief: mean |dY|/Y (%) on flat, sky, character-interior and dense-smoke pixels, and separate opaque and
//     translucent HUD max |d| in 8-bit levels;
//   - halo: mean |dY|/Y in a 4 px band around silhouettes divided by the textured-interior mean;
//   - temporal: flicker of the relief change along the known pan trajectory (mean |delta_t(x) - delta_t-1(x+v)| over
//     mean |delta|);
//   - ReliefStats after the run: coverage, auto-disable and reason; GPU time.
// Two CPU baselines with the same energy as MoonSR's output on each scene (M0 exit criterion): a naive greyscale
// emboss and a "clarity" local-contrast boost. Allocation and readback are free here (offline tool).
#include "relief_bench.h"

#include "imageops.h"
#include "metrics.h"

#include "framegen/frame_generator.h"
#include "gpu/gpu_device.h"
#include "relief/relief_pass.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

namespace bench {
namespace {

using Microsoft::WRL::ComPtr;
using namespace moonsr;
namespace fs = std::filesystem;

constexpr float kPi = 3.14159265358979f;

// ------------------------------------------------------------------ procedural helpers
uint32_t Hash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x85ebca6bu; h ^= h >> 16;
    return h;
}
float Hash01(int32_t x, int32_t y, uint32_t seed) { return static_cast<float>(Hash(x, y, seed) & 0xFFFFFF) / 16777215.0f; }
float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }
float SmoothStep(float a, float b, float x) { return Smooth(std::clamp((x - a) / (b - a), 0.0f, 1.0f)); }
float ValueNoise(float x, float y, uint32_t seed) {
    const int32_t xi = static_cast<int32_t>(std::floor(x)), yi = static_cast<int32_t>(std::floor(y));
    const float fx = Smooth(x - xi), fy = Smooth(y - yi);
    const float a = Hash01(xi, yi, seed), b = Hash01(xi + 1, yi, seed), c = Hash01(xi, yi + 1, seed), d = Hash01(xi + 1, yi + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
float Fbm(float x, float y, uint32_t seed, int oct) {
    float s = 0, a = 0.5f, n = 0;
    for (int i = 0; i < oct; ++i) { s += a * ValueNoise(x, y, seed + i * 17); n += a; x *= 2.03f; y *= 2.03f; a *= 0.5f; }
    return s / n;
}
float SrgbToLin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinToSrgb(float c) { c = std::clamp(c, 0.0f, 1.0f); return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
float Lum(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// World (2x supersampled frame space plus pan margin). Per texel: linear albedo, true height, baked cavity factor,
// class (0 textured, 1 flat: no true relief) and object id (silhouette boundaries).
enum Cls : uint8_t { kTextured = 0, kFlat = 1 };
enum Hud : uint8_t { kNoHud = 0, kOpaqueHud, kTranslucentHud };
struct World {
    int w = 0, h = 0;
    std::vector<float> r, g, b, height, cav;
    std::vector<uint8_t> cls, obj;
    void Alloc(int W, int H) {
        w = W; h = H;
        const size_t n = static_cast<size_t>(W) * H;
        r.assign(n, 0); g.assign(n, 0); b.assign(n, 0); height.assign(n, 0); cav.assign(n, 1); cls.assign(n, kTextured); obj.assign(n, 0);
    }
};

struct SceneDef {
    const char* name;
    bool texturedTruth; // the scene has true relief (counts for the height-correlation gate)
    bool hud;           // static HUD drawn over the panning surface
    bool expectAutoDisable;
};
const SceneDef kScenes[] = {
    {"bricks", true, false, false},     {"cobblestones", true, false, false}, {"rock", true, false, false},
    {"wood_planks", true, false, false},{"painted_wall", false, false, false},{"sky", false, false, true},
    {"hud_over_bricks", true, true, false}, {"silhouette", true, false, false}, {"stylised", false, false, true},
    {"pixel_art", false, false, true}, {"smoke_over_bricks", true, false, false},
};
constexpr int kSceneCount = static_cast<int>(sizeof(kScenes) / sizeof(kScenes[0]));

void Bricks(float fx, float fy, float& h, float& ar, float& ag, float& ab, uint32_t seed) {
    const float bw = 48, bh = 20, m = 3;
    const int row = static_cast<int>(std::floor(fy / bh));
    const float ox = (row & 1) ? bw * 0.5f : 0.0f;
    const int col = static_cast<int>(std::floor((fx + ox) / bw));
    const float lx = fx + ox - col * bw, ly = fy - row * bh;
    const float dEdge = std::min({lx, bw - lx, ly, bh - ly}) - m * 0.5f; // distance into the brick from the mortar
    const float v = Hash01(col, row, seed);
    const float n = Fbm(fx * 0.35f, fy * 0.35f, seed + 3, 3);
    if (dEdge <= 0) { // mortar: recessed, mid grey with grit
        h = 0.05f * n;
        ar = ag = 0.30f + 0.06f * n; ab = 0.28f + 0.05f * n;
    } else {
        h = SmoothStep(0.0f, 2.5f, dEdge) * (0.85f + 0.15f * n);
        const float k = 0.85f + 0.3f * v + 0.15f * (n - 0.5f);
        ar = 0.42f * k; ag = 0.16f * k; ab = 0.10f * k;
    }
}

void GenWorld(int scene, int W, int H, World& w) {
    w.Alloc(W, H);
    const uint32_t seed = 1234u + static_cast<uint32_t>(scene) * 101u;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            const float fx = (x + 0.5f) * 0.5f, fy = (y + 0.5f) * 0.5f; // frame px
            float h = 0, ar = 0.5f, ag = 0.5f, ab = 0.5f;
            uint8_t cls = kTextured, obj = 0;
            bool bakeCavity = true;
            switch (scene) {
            case 0: case 6: case 10: Bricks(fx, fy, h, ar, ag, ab, seed); break;
            case 1: { // cobblestones: jittered Voronoi, domes, dark gaps
                const float cs = 26;
                const int cx = static_cast<int>(std::floor(fx / cs)), cy = static_cast<int>(std::floor(fy / cs));
                float d1 = 1e9f, d2 = 1e9f; int id = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int k = -1; k <= 1; ++k) {
                        const float px = (cx + k + 0.15f + 0.7f * Hash01(cx + k, cy + j, seed)) * cs;
                        const float py = (cy + j + 0.15f + 0.7f * Hash01(cx + k, cy + j, seed + 9)) * cs;
                        const float d = std::hypot(fx - px, fy - py);
                        if (d < d1) { d2 = d1; d1 = d; id = Hash(cx + k, cy + j, seed) & 1023; }
                        else if (d < d2) d2 = d;
                    }
                const float edge = 0.5f * (d2 - d1); // distance to the cell border
                const float n = Fbm(fx * 0.3f, fy * 0.3f, seed + 5, 3);
                h = std::sqrt(SmoothStep(0.5f, 9.0f, edge)) * (0.9f + 0.1f * n);
                const float v = 0.8f + 0.4f * static_cast<float>(id) / 1023.0f;
                ar = 0.30f * v + 0.04f * n; ag = 0.27f * v + 0.04f * n; ab = 0.23f * v + 0.03f * n;
                break;
            }
            case 2: case 7: { // rock (fBm height), albedo partly independent
                h = Fbm(fx * 0.08f, fy * 0.08f, seed, 5);
                h = SmoothStep(0.25f, 0.75f, h);
                const float a = 0.33f + 0.10f * Fbm(fx * 0.05f, fy * 0.05f, seed + 77, 3);
                ar = a; ag = a * 0.95f; ab = a * 0.88f;
                break;
            }
            case 3: { // wood planks: grooves are relief, grain is dark albedo only (partial polarity trap)
                const float ph = 30;
                const int row = static_cast<int>(std::floor(fy / ph));
                const float ly = fy - row * ph;
                const float d = std::min(ly, ph - ly) - 1.0f;
                h = SmoothStep(0.0f, 1.5f, d);
                const float grain = 0.5f + 0.5f * std::sin(fy * 1.7f + 3.0f * Fbm(fx * 0.02f, fy * 0.1f, seed + row, 3) * 6.0f);
                const float k = 0.75f + 0.25f * Hash01(row, 0, seed) - 0.25f * grain * grain;
                ar = 0.50f * k; ag = 0.32f * k; ab = 0.17f * k;
                break;
            }
            case 4: { // painted flat wall, dark albedo patterns (polarity trap): NO true relief
                h = 0; cls = kFlat; bakeCavity = false;
                const float stripe = SmoothStep(0.3f, 0.7f, std::abs(std::fmod(fx + fy * 0.3f, 60.0f) / 60.0f - 0.5f) * 2.0f);
                const float blob = Fbm(fx * 0.04f, fy * 0.04f, seed, 3) > 0.62f ? 1.0f : 0.0f;
                const float glyph = (static_cast<int>(fx / 6) % 3 == 0 && static_cast<int>(fy / 9) % 4 == 1 &&
                                     Hash01(static_cast<int>(fx / 6), static_cast<int>(fy / 9), seed) > 0.4f) ? 1.0f : 0.0f;
                const float dark = std::max({stripe * 0.6f, blob * 0.8f, glyph});
                ar = 0.62f - 0.5f * dark; ag = 0.60f - 0.48f * dark; ab = 0.52f - 0.4f * dark;
                break;
            }
            case 5: { // sky: gradient + soft clouds, NO relief
                h = 0; cls = kFlat; bakeCavity = false;
                const float t = fy / (H * 0.5f);
                const float cloud = SmoothStep(0.45f, 0.75f, Fbm(fx * 0.006f, fy * 0.012f, seed, 4));
                ar = (0.20f + 0.15f * t) * (1 - cloud) + 0.85f * cloud;
                ag = (0.35f + 0.20f * t) * (1 - cloud) + 0.86f * cloud;
                ab = (0.75f + 0.10f * t) * (1 - cloud) + 0.90f * cloud;
                break;
            }
            case 8: { // stylised: flat colour regions and dark outlines, NO relief
                h = 0; cls = kFlat; bakeCavity = false;
                const float cxs = 140, cys = 120;
                const int ci = static_cast<int>(std::floor(fx / cxs)), cj = static_cast<int>(std::floor(fy / cys));
                const float lx = fx - (ci + 0.5f) * cxs, ly = fy - (cj + 0.5f) * cys;
                const float rr = std::hypot(lx, ly), R = 40 + 15 * Hash01(ci, cj, seed);
                const float pal[4][3] = {{0.85f, 0.35f, 0.25f}, {0.25f, 0.55f, 0.80f}, {0.95f, 0.85f, 0.35f}, {0.35f, 0.75f, 0.40f}};
                const int pi = static_cast<int>(Hash(ci, cj, seed) & 3), bi = static_cast<int>(Hash(ci + 7, cj, seed) & 3);
                const float* c = rr < R ? pal[pi] : pal[(bi + 1) & 3];
                ar = c[0]; ag = c[1]; ab = c[2];
                if (std::abs(rr - R) < 1.5f) ar = ag = ab = 0.03f;
                break;
            }
            case 9: { // Pixel-art tiles, with a limited palette and sharp, stepped silhouettes.
                cls = kFlat;
                bakeCavity = false;
                const int px = static_cast<int>(fx) / 8;
                const int py = static_cast<int>(fy) / 8;
                const int row = py % 16;
                const int col = px % 16;
                const int edge = 6 - std::abs(col - 8);
                constexpr float palette[][3] = {{0.04f, 0.08f, 0.16f}, {0.07f, 0.22f, 0.19f},
                                                {0.15f, 0.41f, 0.24f}, {0.52f, 0.69f, 0.27f}};
                const int hill = 11 - std::max(edge, 0);
                int shade = row < hill ? 0 : row == hill ? 3 : row < hill + 3 ? 2 : 1;
                if (row == 2 && (col == 4 || col == 12)) shade = 3;
                ar = palette[shade][0]; ag = palette[shade][1]; ab = palette[shade][2];
                break;
            }
            }
            if (scene == 7) { // character-like silhouette over rock: smooth dark capsule body + head, no relief inside
                const float cx = W * 0.25f + 10.0f;
                const float body = std::hypot((fx - cx) / 60.0f, std::max(0.0f, std::abs(fy - H * 0.30f) - 60.0f) / 60.0f);
                const float head = std::hypot(fx - cx, fy - (H * 0.30f - 120.0f)) / 38.0f;
                if (body < 1.0f || head < 1.0f) {
                    obj = 1; cls = kFlat; h = 0; bakeCavity = false;
                    const float s = 0.55f + 0.45f * (1.0f - std::min(body, head)); // smooth shading
                    ar = 0.10f * s; ag = 0.11f * s; ab = 0.16f * s;
                }
            }
            if (scene == 10) {
                const float xFromPlume = (fx - 0.23f * W) / (0.12f * W);
                const float yFromPlume = (fy - 0.24f * H) / (0.20f * H);
                const float radial = std::hypot(xFromPlume, yFromPlume);
                const float density = 1.0f - SmoothStep(0.30f, 1.0f, radial);
                const float smoke = 0.30f + 0.20f * Fbm(fx * 0.033f, fy * 0.033f, seed + 10, 4);
                const float wallCavity = 0.55f + 0.45f * h;
                ar = std::lerp(ar * wallCavity, smoke, density);
                ag = std::lerp(ag * wallCavity, 0.95f * smoke, density);
                ab = std::lerp(ab * wallCavity, 0.92f * smoke, density);
                h *= 1.0f - density;
                if (density >= 0.85f) cls = kFlat;
                bakeCavity = false;
            }
            w.r[i] = ar; w.g[i] = ag; w.b[i] = ab;
            w.height[i] = h;
            w.cav[i] = bakeCavity ? 0.55f + 0.45f * h : 1.0f; // baked AO: recesses darker (what games ship)
            w.cls[i] = cls;
            w.obj[i] = obj;
        }
}

// Frame at pan offset `shift` (frame px): 2x2 box supersample of the world at world x = 2*(x + shift) (bilinear).
struct Frame {
    int w = 0, h = 0;
    std::vector<float> inLin, refLin; // linear luminance of the flat input and the relief-lit reference
    std::vector<float> height;        // true height
    Image img;                        // sRGB 8-bit input (BGRA)
    std::vector<uint8_t> cls, obj, hud;
};

void RenderFrame(const World& wd, int W, int H, float shift, float shiftY, bool hud, Frame& f,
                 Image* litReference = nullptr) {
    f.w = W; f.h = H;
    const size_t n = static_cast<size_t>(W) * H;
    f.inLin.assign(n, 0); f.refLin.assign(n, 0); f.height.assign(n, 0); f.cls.assign(n, 0); f.obj.assign(n, 0); f.hud.assign(n, kNoHud);
    f.img.Alloc(W, H);
    if (litReference) litReference->Alloc(W, H);
    constexpr float az = 135.0f * kPi / 180.0f, el = 45.0f * kPi / 180.0f;
    const float Lx = std::cos(el) * std::cos(az), Ly = -std::cos(el) * std::sin(az), Lz = std::sin(el);
    auto at = [&](const std::vector<float>& v, int x, int y) {
        x = std::clamp(x, 0, wd.w - 1); y = std::clamp(y, 0, wd.h - 1);
        return v[static_cast<size_t>(y) * wd.w + x];
    };
    auto bil = [&](const std::vector<float>& v, float x, float y) {
        const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
        const float fx = x - xi, fy = y - yi;
        return (at(v, xi, yi) * (1 - fx) + at(v, xi + 1, yi) * fx) * (1 - fy) + (at(v, xi, yi + 1) * (1 - fx) + at(v, xi + 1, yi + 1) * fx) * fy;
    };
    ParallelFor(static_cast<uint32_t>(H), [&](uint32_t y) {
        for (int x = 0; x < W; ++x) {
            float r = 0, g = 0, b = 0, ref = 0, hh = 0;
            float litR = 0, litG = 0, litB = 0;
            for (int s = 0; s < 4; ++s) {
                const float wx = 2.0f * (x + shift) + (s & 1), wy = 2.0f * (y + shiftY) + (s >> 1);
                const float c = bil(wd.cav, wx, wy);
                const float ar = bil(wd.r, wx, wy) * c, ag = bil(wd.g, wx, wy) * c, ab = bil(wd.b, wx, wy) * c;
                r += ar; g += ag; b += ab;
                // true normal (world px -> frame px: x2), relief depth 2 frame px per height unit
                const float dhx = (bil(wd.height, wx + 1, wy) - bil(wd.height, wx - 1, wy)) * 2.0f * 0.5f;
                const float dhy = (bil(wd.height, wx, wy + 1) - bil(wd.height, wx, wy - 1)) * 2.0f * 0.5f;
                float nx = -2.0f * dhx, ny = -2.0f * dhy, nz = 1.0f;
                const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= nl; ny /= nl; nz /= nl;
                const float S = (0.3f + 0.7f * std::max(0.0f, nx * Lx + ny * Ly + nz * Lz)) / (0.3f + 0.7f * Lz);
                ref += Lum(ar, ag, ab) * S;
                if (litReference) { litR += ar * S; litG += ag * S; litB += ab * S; }
                hh += bil(wd.height, wx, wy);
            }
            r *= 0.25f; g *= 0.25f; b *= 0.25f;
            const size_t i = static_cast<size_t>(y) * W + x;
            const int wx0 = std::clamp(static_cast<int>(2.0f * (x + shift) + 1.0f), 0, wd.w - 1), wy0 = std::clamp(static_cast<int>(2.0f * (y + shiftY) + 1.0f), 0, wd.h - 1);
            f.cls[i] = wd.cls[static_cast<size_t>(wy0) * wd.w + wx0];
            f.obj[i] = wd.obj[static_cast<size_t>(wy0) * wd.w + wx0];
            f.height[i] = hh * 0.25f;
            uint8_t* p = f.img.Row(y) + x * 4;
            p[2] = static_cast<uint8_t>(std::lround(LinToSrgb(r) * 255.0f));
            p[1] = static_cast<uint8_t>(std::lround(LinToSrgb(g) * 255.0f));
            p[0] = static_cast<uint8_t>(std::lround(LinToSrgb(b) * 255.0f));
            f.inLin[i] = Lum(SrgbToLin(p[2] / 255.0f), SrgbToLin(p[1] / 255.0f), SrgbToLin(p[0] / 255.0f));
            f.refLin[i] = ref * 0.25f;
            if (litReference) {
                uint8_t* q = litReference->Row(y) + x * 4;
                q[2] = static_cast<uint8_t>(std::lround(LinToSrgb(litR * 0.25f) * 255.0f));
                q[1] = static_cast<uint8_t>(std::lround(LinToSrgb(litG * 0.25f) * 255.0f));
                q[0] = static_cast<uint8_t>(std::lround(LinToSrgb(litB * 0.25f) * 255.0f));
                q[3] = 255;
            }
        }
    });
    if (hud) { // static HUD (frame space): opaque panels, a health bar, blocky text, and a translucent panel
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                bool on = false;
                float c[3] = {0, 0, 0};
                if (x >= 24 && x < 284 && y >= H - 64 && y < H - 24) { on = true; c[0] = 0.08f; c[1] = 0.08f; c[2] = 0.10f; // panel
                    if (x >= 34 && x < 34 + 180 && y >= H - 54 && y < H - 34) { c[0] = 0.85f; c[1] = 0.15f; c[2] = 0.12f; } }
                if (x >= W - 300 && x < W - 24 && y >= 20 && y < 60) {
                    on = true;
                    const int gx = (x - (W - 300)) / 4, gy = (y - 20) / 4;
                    const bool ink = gy >= 2 && gy <= 7 && (gx % 6) < 4 && Hash01(gx, gy, 99) > 0.35f;
                    const float v = ink ? 0.95f : 0.05f;
                    c[0] = c[1] = c[2] = v;
                }
                const bool translucent = x >= W / 2 - 60 && x < W / 2 + 60 && y >= 26 && y < 54;
                if (!on && !translucent) continue;
                const size_t i = static_cast<size_t>(y) * W + x;
                uint8_t* p = f.img.Row(static_cast<uint32_t>(y)) + x * 4;
                if (translucent) {
                    for (size_t channel = 0; channel < 3; ++channel)
                        c[channel] = std::lerp(SrgbToLin(p[2 - channel] / 255.0f), 0.24f, 0.55f);
                }
                p[2] = static_cast<uint8_t>(std::lround(LinToSrgb(c[0]) * 255)); p[1] = static_cast<uint8_t>(std::lround(LinToSrgb(c[1]) * 255));
                p[0] = static_cast<uint8_t>(std::lround(LinToSrgb(c[2]) * 255));
                if (litReference) std::memcpy(litReference->Row(y) + x * 4, p, 4);
                f.inLin[i] = f.refLin[i] = Lum(SrgbToLin(p[2] / 255.0f), SrgbToLin(p[1] / 255.0f), SrgbToLin(p[0] / 255.0f));
                f.height[i] = 0; f.cls[i] = kFlat; f.hud[i] = translucent ? kTranslucentHud : kOpaqueHud;
            }
    }
}

// ------------------------------------------------------------------ statistics
double Pearson(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() < 3) return 0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
    ma /= a.size(); mb /= b.size();
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i) { const double x = a[i] - ma, y = b[i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
    return (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : 0.0;
}
std::vector<float> Ranks(const std::vector<float>& v) {
    std::vector<uint32_t> idx(v.size());
    std::iota(idx.begin(), idx.end(), 0u);
    std::sort(idx.begin(), idx.end(), [&](uint32_t x, uint32_t y) { return v[x] < v[y]; });
    std::vector<float> r(v.size());
    for (size_t i = 0; i < idx.size();) { // average ranks for ties
        size_t j = i;
        while (j + 1 < idx.size() && v[idx[j + 1]] == v[idx[i]]) ++j;
        const float rk = 0.5f * static_cast<float>(i + j);
        for (size_t k = i; k <= j; ++k) r[idx[k]] = rk;
        i = j + 1;
    }
    return r;
}
double Spearman(const std::vector<float>& a, const std::vector<float>& b) { return Pearson(Ranks(a), Ranks(b)); }

// Separable Gaussian (clamped borders).
std::vector<float> Blur(const std::vector<float>& v, int W, int H, float sigma) {
    const int r = static_cast<int>(std::ceil(3 * sigma));
    std::vector<float> k(2 * r + 1);
    float s = 0;
    for (int i = -r; i <= r; ++i) s += k[i + r] = std::exp(-0.5f * i * i / (sigma * sigma));
    for (float& x : k) x /= s;
    std::vector<float> t(v.size()), o(v.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float a = 0;
            for (int i = -r; i <= r; ++i) a += k[i + r] * v[static_cast<size_t>(y) * W + std::clamp(x + i, 0, W - 1)];
            t[static_cast<size_t>(y) * W + x] = a;
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float a = 0;
            for (int i = -r; i <= r; ++i) a += k[i + r] * t[static_cast<size_t>(std::clamp(y + i, 0, H - 1)) * W + x];
            o[static_cast<size_t>(y) * W + x] = a;
        }
    return o;
}

std::vector<float> LinLum(const Image& im) {
    std::vector<float> o(static_cast<size_t>(im.w) * im.h);
    for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x) {
            const uint8_t* p = im.Row(y) + x * 4;
            o[static_cast<size_t>(y) * im.w + x] = Lum(SrgbToLin(p[2] / 255.0f), SrgbToLin(p[1] / 255.0f), SrgbToLin(p[0] / 255.0f));
        }
    return o;
}

// ------------------------------------------------------------------ GPU harness
struct Harness {
    GpuDevice* dev = nullptr;
    std::unique_ptr<ReliefPass> pass;
    ComPtr<ID3D11Texture2D> inTex, outTex, staging;
    ComPtr<ID3D11ShaderResourceView> inSrv;
    ComPtr<ID3D11RenderTargetView> outRtv;
    uint32_t w = 0, h = 0;

    bool Init(GpuDevice& d, uint32_t W, uint32_t H, std::string& err) {
        dev = &d; w = W; h = H;
        std::wstring e;
        pass = ReliefPass::Create(d, e);
        if (!pass) { err = "ReliefPass::Create: " + Narrow(e); return false; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = d.Device()->CreateTexture2D(&td, nullptr, &inTex);
        if (SUCCEEDED(hr)) hr = d.Device()->CreateShaderResourceView(inTex.Get(), nullptr, &inSrv);
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = d.Device()->CreateTexture2D(&td, nullptr, &outTex);
        if (SUCCEEDED(hr)) hr = d.Device()->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv);
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = d.Device()->CreateTexture2D(&td, nullptr, &staging);
        if (FAILED(hr)) { err = "texture creation failed"; return false; }
        return true;
    }
    void Upload(const Image& im) { dev->Context()->UpdateSubresource(inTex.Get(), 0, nullptr, im.px.data(), im.w * 4, 0); }
    bool Analyse(const MotionFieldView* mv = nullptr) {
        return pass->Analyse(inTex.Get(), RectI{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)}, mv, QpcNow());
    }
    bool Apply(float phase = 1.0f, bool filteredParallax = false, ID3D11ShaderResourceView* selection = nullptr) {
        return pass->Apply(inSrv.Get(), Size{w, h}, outRtv.Get(), Size{w, h},
                           RectI{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)}, phase, 1.0f, filteredParallax,
                           selection);
    }
    bool Read(Image& out) {
        ID3D11DeviceContext1* ctx = dev->Context();
        ctx->CopyResource(staging.Get(), outTex.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out.Alloc(w, h);
        for (uint32_t y = 0; y < h; ++y) std::memcpy(out.Row(y), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, w * 4);
        ctx->Unmap(staging.Get(), 0);
        return true;
    }
};

struct SceneResult {
    std::string name;
    double pearsonH = 0, spearmanH = 0, signAcc = 0, shadeCorr = 0, litEdge = 0, shadowEdge = 0;
    double falseRelief = 0, hudMax = 0, halo = -1, hudHalo = -1, interior = 0, flicker = 0;
    double maxOffsetPx = 0, maxHudOffsetPx = 0, generatedHudMax = 0, translucentHudMax = 0;
    double embossCorr = 0, clarityCorr = 0, embossFalse = 0, clarityFalse = 0, embossHalo = -1, clarityHalo = -1;
    double heightHalo = -1;
    int staticChange = 0, staticFlicker = 0;
    float coverage = 0;
    bool autoDisabled = false, active = false;
    std::wstring reason;
};

// Relative linear-luminance change of `out` against the input, per pixel.
std::vector<float> Delta(const std::vector<float>& inLin, const std::vector<float>& outLin) {
    std::vector<float> d(inLin.size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = outLin[i] / std::max(inLin[i], 1e-4f) - 1.0f;
    return d;
}

// Masks (frame space): textured interior (true relief, away from silhouettes and HUD), flat/no-relief, edge band.
void Masks(const Frame& f, std::vector<uint8_t>& tex, std::vector<uint8_t>& flat, std::vector<uint8_t>& band,
           std::vector<uint8_t>& hudBand) {
    const int W = f.w, H = f.h;
    const size_t n = static_cast<size_t>(W) * H;
    tex.assign(n, 0); flat.assign(n, 0); band.assign(n, 0); hudBand.assign(n, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = static_cast<size_t>(y) * W + x;
            bool nearObj = false, nearHud = false;
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx) {
                    const int xx = std::clamp(x + dx, 0, W - 1), yy = std::clamp(y + dy, 0, H - 1);
                    const size_t j = static_cast<size_t>(yy) * W + xx;
                    nearObj |= f.obj[j] != f.obj[i];
                    nearHud |= f.hud[j] != f.hud[i];
                }
            const bool border = x < 8 || y < 8 || x >= W - 8 || y >= H - 8;
            if (nearHud && !f.hud[i]) hudBand[i] = 1;
            else if (nearObj) band[i] = 1;
            else if (!border && f.cls[i] == kTextured) tex[i] = 1;
            else if (!border && f.cls[i] == kFlat) flat[i] = 1;
        }
}

double MeanAbs(const std::vector<float>& d, const std::vector<uint8_t>& m) {
    double s = 0; size_t c = 0;
    for (size_t i = 0; i < d.size(); ++i) if (m[i]) { s += std::abs(d[i]); ++c; }
    return c ? s / c : 0.0;
}
double MaskedPearson(const std::vector<float>& a, const std::vector<float>& b, const std::vector<uint8_t>& m) {
    std::vector<float> x, y;
    for (size_t i = 0; i < a.size(); ++i) if (m[i]) { x.push_back(a[i]); y.push_back(b[i]); }
    return Pearson(x, y);
}

// CPU baselines on the input luminance, scaled to `targetEnergy` mean |delta| on the textured (or whole) mask.
std::vector<float> Baseline(const Frame& f, bool emboss, float gain) {
    const int W = f.w, H = f.h;
    std::vector<float> l(f.inLin.size());
    for (size_t i = 0; i < l.size(); ++i) l[i] = std::log2(f.inLin[i] + 1.0f / 256.0f);
    std::vector<float> d(l.size(), 0.0f);
    if (emboss) { // naive greyscale emboss: brightness = height, derivative towards the upper-left light
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x)
                d[static_cast<size_t>(y) * W + x] = l[static_cast<size_t>(y + 1) * W + x + 1] - l[static_cast<size_t>(y - 1) * W + x - 1];
    } else { // clarity: local contrast boost l - gauss(l, 4)
        const std::vector<float> b = Blur(l, W, H, 4.0f);
        for (size_t i = 0; i < d.size(); ++i) d[i] = l[i] - b[i];
    }
    for (float& v : d) v = std::clamp(v * gain, -0.06f, 0.06f);
    return d;
}

void SaveCrop(const fs::path& dir, const std::string& name, const Image& a, const Image& b, int cx, int cy, int size, int zoom) {
    Image c;
    c.Alloc(static_cast<uint32_t>(size * zoom * 2 + 4), static_cast<uint32_t>(size * zoom));
    for (uint32_t y = 0; y < c.h; ++y)
        for (uint32_t x = 0; x < c.w; ++x) {
            uint8_t* p = c.Row(y) + x * 4;
            const int half = size * zoom + 4;
            if (static_cast<int>(x) >= size * zoom && static_cast<int>(x) < half) { p[0] = p[1] = p[2] = 128; continue; }
            const Image& s = static_cast<int>(x) < size * zoom ? a : b;
            const int lx = (static_cast<int>(x) < size * zoom ? static_cast<int>(x) : static_cast<int>(x) - half) / zoom;
            const int sx = std::clamp(cx + lx, 0, static_cast<int>(s.w) - 1), sy = std::clamp(cy + static_cast<int>(y) / zoom, 0, static_cast<int>(s.h) - 1);
            std::memcpy(p, s.Row(static_cast<uint32_t>(sy)) + sx * 4, 4);
        }
    std::string e;
    WritePng((dir / (name + ".png")).wstring(), c, e);
}

bool RunScene(GpuDevice& dev, int scene, float strength, const ReliefBenchOptions& o, float baselineGain[2], SceneResult& res,
              std::string& err) {
    constexpr int W = 960, H = 540, kFrames = 40, kFlickerFrom = 20;
    constexpr float kPan = 0.25f, kPanY = 0.1f; // slow diagonal pan (an axis-aligned pan is a known weak case: see report)
    const SceneDef& sd = kScenes[scene];
    res.name = sd.name;
    World wd;
    GenWorld(scene, 2 * (W + 16), 2 * (H + 8), wd);
    Harness hs;
    if (!hs.Init(dev, W, H, err)) return false;
    ReliefSettings rs;
    rs.tier = o.parallax ? ReliefTier::Parallax : ReliefTier::Shading;
    rs.strength = strength;
    if (o.parallax) rs.parallaxMaxPx = 3.0f;
    if (o.detailScalePx > 0.0f) rs.detailScalePx = o.detailScalePx;
    hs.pass->SetSettings(rs);

    Frame f;
    Image out, heightImg, litReference;
    const bool exportFidelity = o.saveImages && !o.parallax && (scene == 0 || scene == 2);
    std::vector<float> prevDelta;
    double flickNum = 0, flickDen = 0;
    for (int k = 0; k < kFrames; ++k) {
        RenderFrame(wd, W, H, k * kPan, k * kPanY, sd.hud, f,
                    exportFidelity && k == kFrames - 1 ? &litReference : nullptr);
        hs.Upload(f.img);
        if (!hs.Analyse()) { err = "Analyse failed"; return false; }
        rs.debugView = ReliefDebugView::None;
        hs.pass->SetSettings(rs);
        if (!hs.Apply() || !hs.Read(out)) { err = "Apply/readback failed"; return false; }
        const std::vector<float> delta = Delta(f.inLin, LinLum(out));
        if (k >= kFlickerFrom && !prevDelta.empty()) { // content at x now was at x + v in the previous frame
            for (int y = 8; y < H - 8; ++y)
                for (int x = 8; x < W - 8; ++x) {
                    const size_t i = static_cast<size_t>(y) * W + x;
                    if (f.hud[i]) continue;
                    const float p = (prevDelta[i] * (1 - kPan) + prevDelta[i + 1] * kPan) * (1 - kPanY) +
                                    (prevDelta[i + W] * (1 - kPan) + prevDelta[i + W + 1] * kPan) * kPanY;
                    flickNum += std::abs(delta[i] - p);
                    flickDen += std::abs(delta[i]);
                }
        }
        prevDelta = delta;
    }
    // Final frame: height view and metrics.
    rs.debugView = ReliefDebugView::Height;
    hs.pass->SetSettings(rs);
    if (!hs.Apply() || !hs.Read(heightImg)) { err = "height readback failed"; return false; }
    if (o.parallax) {
        Image offset;
        rs.debugView = ReliefDebugView::Offset;
        hs.pass->SetSettings(rs);
        if (!hs.Apply() || !hs.Read(offset)) { err = "offset readback failed"; return false; }
        for (size_t i = 0; i < f.hud.size(); ++i) {
            const double px = 3.0 * offset.px[i * 4] / 255.0; // blue channel holds the offset length
            res.maxOffsetPx = std::max(res.maxOffsetPx, px);
            if (f.hud[i] == kOpaqueHud) res.maxHudOffsetPx = std::max(res.maxHudOffsetPx, px);
        }
    }
    rs.debugView = ReliefDebugView::None;
    hs.pass->SetSettings(rs);
    if (!hs.Apply() || !hs.Read(out)) { err = "readback failed"; return false; }
    const ReliefStats st = hs.pass->Stats();
    res.coverage = st.texturedCoverage;
    res.autoDisabled = st.autoDisabled;
    res.active = st.active;
    res.reason = st.reason;

    std::vector<uint8_t> tex, flat, band, hudBand;
    Masks(f, tex, flat, band, hudBand);
    const std::vector<float> outLin = LinLum(out);
    const std::vector<float> delta = Delta(f.inLin, outLin);
    const std::vector<float> truthDelta = Delta(f.inLin, f.refLin);
    // Band-passed true height at the analysis scale (sigma 3 px, detailScalePx 1.5 x 2).
    const std::vector<float> hb = Blur(f.height, W, H, 3.0f);
    std::vector<float> est, tru;
    size_t signOk = 0, signN = 0;
    double heightBandEnergy = 0, heightInteriorEnergy = 0;
    size_t bandCount = 0;
    for (size_t i = 0; i < tex.size(); ++i) {
        const float hEst = 2.0f * (heightImg.px[i * 4 + 1] / 255.0f) - 1.0f;
        if (band[i]) {
            heightBandEnergy += hEst * hEst;
            ++bandCount;
        }
        if (!tex[i]) continue;
        heightInteriorEnergy += hEst * hEst;
        const float t = f.height[i] - hb[i];
        est.push_back(hEst);
        tru.push_back(t);
        if (std::abs(t) > 0.05f && std::abs(hEst) > 0.02f) { ++signN; if ((t > 0) == (hEst > 0)) ++signOk; }
    }
    res.pearsonH = Pearson(est, tru);
    res.spearmanH = Spearman(est, tru);
    res.signAcc = signN ? static_cast<double>(signOk) / signN : 0.0;
    if (bandCount > 100 && heightInteriorEnergy > 0)
        res.heightHalo = (heightBandEnergy / bandCount) / (heightInteriorEnergy / est.size());
    res.shadeCorr = MaskedPearson(delta, truthDelta, tex);
    // Bevel check: on pixels where the TRUE relief lighting brightens (light-facing slopes) or darkens (slopes facing
    // away) by more than 10 %, the mean produced change. Lit bevels need lit > 0 and shadow < 0; cavity-only darkening
    // would make both negative.
    {
        double ls = 0, ss = 0; size_t lc = 0, sc = 0;
        for (size_t i = 0; i < tex.size(); ++i) {
            if (!tex[i]) continue;
            if (truthDelta[i] > 0.1f) { ls += delta[i]; ++lc; }
            else if (truthDelta[i] < -0.1f) { ss += delta[i]; ++sc; }
        }
        res.litEdge = lc ? ls / lc : 0.0;
        res.shadowEdge = sc ? ss / sc : 0.0;
    }
    res.interior = MeanAbs(delta, tex);
    res.falseRelief = MeanAbs(delta, flat);
    if (bandCount > 100) res.halo = res.interior > 0 ? MeanAbs(delta, band) / res.interior : -1;
    if (sd.hud && res.interior > 0) res.hudHalo = MeanAbs(delta, hudBand) / res.interior;
    size_t hudChanged = 0, hudAt = 0;
    for (size_t i = 0; i < f.hud.size(); ++i)
        if (f.hud[i]) {
            double m = 0;
            for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(static_cast<double>(out.px[i * 4 + c]) - f.img.px[i * 4 + c]));
            if (f.hud[i] == kTranslucentHud) {
                res.translucentHudMax = std::max(res.translucentHudMax, m);
                continue;
            }
            if (m > 0) { ++hudChanged; if (m > res.hudMax) hudAt = i; }
            res.hudMax = std::max(res.hudMax, m);
        }
    if (hudChanged)
        printf("  note: %s: %zu HUD pixels changed, max %.0f levels at (%zu, %zu)\n", sd.name, hudChanged, res.hudMax, hudAt % W, hudAt / W);
    if (sd.hud) {
        Image generated;
        for (const bool filtered : {false, true}) {
            if (filtered && !o.parallax) break;
            for (float phase : {0.25f, 0.5f, 0.75f}) {
                if (!hs.Apply(phase, filtered) || !hs.Read(generated)) { err = "generated HUD readback failed"; return false; }
                for (size_t i = 0; i < f.hud.size(); ++i) {
                    if (f.hud[i] != kOpaqueHud) continue;
                    for (size_t c = 0; c < 3; ++c)
                        res.generatedHudMax = std::max(res.generatedHudMax,
                            double(std::abs(int(generated.px[4 * i + c]) - int(f.img.px[4 * i + c]))));
                }
            }
        }
    }
    res.flicker = flickDen > 0 ? flickNum / flickDen : 0.0;

    char tagBuffer[64];
    snprintf(tagBuffer, sizeof(tagBuffer), "%s_s%03d", sd.name, static_cast<int>(std::lround(strength * 100)));
    const std::string tag = tagBuffer;
    if (exportFidelity) {
        std::error_code ec;
        fs::create_directories(o.outDir, ec);
        if (ec) { err = ec.message(); return false; }
        if (!WritePng((o.outDir / (tag + "_reference.png")).wstring(), litReference, err)) return false;
    }
    // Set one gain per baseline and strength from bricks, then reuse it on every scene. Baseline() caps each pixel.
    for (int bi = 0; bi < 2; ++bi) {
        if (scene == 0) {
            const std::vector<float> unit = Baseline(f, bi == 0, 1.0f);
            double e = 0; size_t c = 0;
            for (size_t i = 0; i < unit.size(); ++i) if (tex[i]) { e += std::abs(unit[i]); ++c; } // unclipped at gain 1
            baselineGain[bi] = (c && e > 0) ? static_cast<float>(res.interior / (e / c)) : 0.0f;
        }
        const std::vector<float> bd = Baseline(f, bi == 0, baselineGain[bi]);
        const double corr = MaskedPearson(bd, truthDelta, tex), fr = MeanAbs(bd, flat);
        const double halo = (bandCount > 100 && MeanAbs(bd, tex) > 0) ? MeanAbs(bd, band) / MeanAbs(bd, tex) : -1;
        (bi == 0 ? res.embossCorr : res.clarityCorr) = corr;
        (bi == 0 ? res.embossFalse : res.clarityFalse) = fr;
        (bi == 0 ? res.embossHalo : res.clarityHalo) = halo;
        if (exportFidelity) {
            Image baseline = f.img;
            for (size_t i = 0; i < bd.size(); ++i) {
                for (size_t c = 0; c < 3; ++c) {
                    uint8_t& v = baseline.px[i * 4 + c];
                    v = static_cast<uint8_t>(std::lround(LinToSrgb(SrgbToLin(v / 255.0f) * (1.0f + bd[i])) * 255.0f));
                }
            }
            const char* suffix = bi == 0 ? "_emboss.png" : "_clarity.png";
            if (!WritePng((o.outDir / (tag + suffix)).wstring(), baseline, err)) return false;
        }
    }

    if (o.saveImages) {
        std::error_code ec;
        fs::create_directories(o.outDir, ec);
        std::string e;
        WritePng((o.outDir / (std::string(tag) + "_input.png")).wstring(), f.img, e);
        WritePng((o.outDir / (std::string(tag) + "_relief.png")).wstring(), out, e);
        WritePng((o.outDir / (std::string(tag) + "_height.png")).wstring(), heightImg, e);
        // Before | after crops, 160x160 game px at 3x zoom; HUD scene: crop around the bottom-left panel.
        const int cx = sd.hud ? 0 : W / 2 - 80, cy = sd.hud ? H - 160 : H / 2 - 80;
        SaveCrop(o.outDir, std::string(tag) + "_crop_before_after", f.img, out, cx, cy, 160, 3);
        rs.debugView = ReliefDebugView::Split;
        hs.pass->SetSettings(rs);
        Image split;
        if (hs.Apply() && hs.Read(split)) WritePng((o.outDir / (std::string(tag) + "_split.png")).wstring(), split, e);
        rs.debugView = ReliefDebugView::Protection;
        hs.pass->SetSettings(rs);
        if (hs.Apply() && hs.Read(split)) WritePng((o.outDir / (std::string(tag) + "_protection.png")).wstring(), split, e);
    }
    if (scene == 0) {
        // Stop the pan and let history settle on exactly the same textured input. Check subsequent output frames
        // against both the input (relief is still active) and the first settled output (no new flicker).
        rs.debugView = ReliefDebugView::None;
        hs.pass->SetSettings(rs);
        Image settled, next;
        for (int k = 0; k < 40; ++k) {
            if (!hs.Analyse()) { err = "static Analyse failed"; return false; }
            if (k < 31) continue;
            if (!hs.Apply() || !hs.Read(k == 31 ? settled : next)) { err = "static readback failed"; return false; }
            if (k == 31) continue;
            for (size_t i = 0; i < settled.px.size(); i += 4) {
                for (size_t c = 0; c < 3; ++c) {
                    const size_t index = i + c;
                    res.staticChange = std::max(res.staticChange, std::abs(int(next.px[index]) - int(f.img.px[index])));
                    res.staticFlicker = std::max(res.staticFlicker, std::abs(int(next.px[index]) - int(settled.px[index])));
                }
            }
        }
    }
    return true;
}

struct CameraPose {
    float x, z, yaw;
    bool orbit = false;
};

// Trace rays against a flat wall and against the same wall displaced by a known height map.
// The test disables directional lighting and cavity so the metric isolates the offset.
bool MeasureParallaxPath(GpuDevice& dev, const CameraPose& step, float capPx, double& flatSsim, double& reliefSsim,
                         double& extraWarpError, const fs::path& exportPrefix, std::string& err) {
    constexpr int W = 480, H = 270, frames = 24, margin = 16;
    constexpr float focal = 300.0f, halfW = W * 0.5f, halfH = H * 0.5f;
    const float amplitude = 3.0f / std::hypot(halfW, halfH);
    auto height = [](float x, float y) {
        return 0.6f * std::sin(113.0f * x) * std::cos(93.0f * y) + 0.4f * std::sin(63.0f * x + 71.0f * y);
    };
    auto tone = [&](float x, float y) {
        return 128.0f + 52.0f * height(x, y);
    };
    Harness hs;
    if (!hs.Init(dev, W, H, err)) return false;
    ReliefSettings rs;
    rs.tier = ReliefTier::Parallax;
    rs.strength = 1.0f;
    rs.parallaxMaxPx = capPx;
    rs.shading = rs.cavity = 0.0f;
    hs.pass->SetSettings(rs);
    Image input, output, prevInput, prevOutput;
    CameraPose prevPose{};
    input.Alloc(W, H);
    std::vector<float> ref, flat, relief;
    const auto sample = [](const Image& image, float x, float y) {
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        const float dx = x - static_cast<float>(ix), dy = y - static_cast<float>(iy);
        const auto* a = image.Row(static_cast<uint32_t>(iy)) + 4 * ix;
        const auto* b = image.Row(static_cast<uint32_t>(iy + 1)) + 4 * ix;
        return std::lerp(std::lerp(float(a[0]), float(a[4]), dx), std::lerp(float(b[0]), float(b[4]), dx), dy);
    };
    for (int frame = 0; frame < frames; ++frame) {
        const float t = static_cast<float>(frame);
        const float yaw = step.yaw * t;
        const float c = std::cos(yaw), s = std::sin(yaw);
        // Orbit the wall centre at (0, 1), with the camera continually facing it.
        const CameraPose p = step.orbit ? CameraPose{-s, 1.0f - c, yaw}
                                       : CameraPose{step.x * t, step.z * t, yaw};
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float u = (static_cast<float>(x) + 0.5f - halfW) / focal;
                const float v = (static_cast<float>(y) + 0.5f - halfH) / focal;
                const float rx = c * u + s, rz = c - s * u;
                const float d = (1.0f - p.z) / rz;
                const auto code = static_cast<uint8_t>(std::lround(tone(p.x + rx * d, v * d)));
                auto* dst = input.Row(static_cast<uint32_t>(y)) + 4 * x;
                dst[0] = dst[1] = dst[2] = code;
                dst[3] = 255;
            }
        hs.Upload(input);
        if (!hs.Analyse() || !hs.Apply() || !hs.Read(output)) { err = "parallax camera path render failed"; return false; }
        if (frame > 7) {
            const float prevC = std::cos(prevPose.yaw), prevS = std::sin(prevPose.yaw);
            double warpDelta = 0.0;
            ref.clear();
            flat.clear();
            relief.clear();
            // Use only the upper half, where the wall model applies.
            for (int y = margin; y < H / 2; ++y)
                for (int x = margin; x < W - margin; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f - halfW) / focal;
                    const float v = (static_cast<float>(y) + 0.5f - halfH) / focal;
                    const float rx = c * u + s, rz = c - s * u;
                    const float d = (1.0f - p.z) / rz;
                    const float wx = p.x + rx * d, wy = v * d;
                    float hit = d;
                    for (int i = 0; i < 8; ++i)
                        hit = (1.0f - p.z - amplitude * height(p.x + rx * hit, v * hit)) / rz;
                    ref.push_back(tone(p.x + rx * hit, v * hit));
                    flat.push_back(input.Row(static_cast<uint32_t>(y))[4 * x]);
                    relief.push_back(output.Row(static_cast<uint32_t>(y))[4 * x]);

                    const float dx = wx - prevPose.x, dz = 1.0f - prevPose.z;
                    const float prevZ = prevS * dx + prevC * dz;
                    const float prevX = focal * (prevC * dx - prevS * dz) / prevZ + halfW - 0.5f;
                    const float prevY = focal * wy / prevZ + halfH - 0.5f;
                    warpDelta += std::abs(relief.back() - sample(prevOutput, prevX, prevY)) -
                                 std::abs(flat.back() - sample(prevInput, prevX, prevY));
                }
            flatSsim += SsimPlanes(flat, ref, W - 2 * margin, H / 2 - margin);
            reliefSsim += SsimPlanes(relief, ref, W - 2 * margin, H / 2 - margin);
            extraWarpError += warpDelta / static_cast<double>(ref.size());
        }
        prevInput = input;
        prevOutput = output;
        prevPose = p;
    }
    flatSsim /= frames - 8;
    reliefSsim /= frames - 8;
    extraWarpError /= frames - 8;
    if (!exportPrefix.empty()) {
        Image crop;
        crop.Alloc(W - 2 * margin, H / 2 - margin);
        const std::pair<const char*, const std::vector<float>*> planes[] = {
            {"-reference.png", &ref}, {"-shading.png", &flat}, {"-parallax.png", &relief},
        };
        for (const auto& [suffix, values] : planes) {
            for (size_t i = 0; i < values->size(); ++i) {
                const auto code = static_cast<uint8_t>(std::lround((*values)[i]));
                crop.px[4 * i] = crop.px[4 * i + 1] = crop.px[4 * i + 2] = code;
                crop.px[4 * i + 3] = 255;
            }
            fs::path filename = exportPrefix;
            filename += suffix;
            if (!WritePng(filename.wstring(), crop, err)) return false;
        }
    }
    return true;
}

// Time only the stage, with filler copies to keep GPU clocks up or with a requested 2 ms sleep every eight frames.
bool TimeRelief(GpuDevice& dev, uint32_t gw, uint32_t gh, uint32_t ow, uint32_t oh, int mode, ReliefTier tier,
                double& analyseMs, double& applyMs, std::string& err, int phases = 1) {
    World wd;
    GenWorld(0, static_cast<int>(2 * (gw + 16)), static_cast<int>(2 * (gh + 8)), wd);
    Frame f;
    RenderFrame(wd, static_cast<int>(gw), static_cast<int>(gh), 0.0f, 0.0f, false, f);
    Harness g;
    if (!g.Init(dev, gw, gh, err)) return false;
    g.Upload(f.img);
    // Supply the same textured input at the output size. Upscaling happens once on the CPU outside the GPU timers.
    Harness o;
    if (!o.Init(dev, ow, oh, err)) return false;
    o.Upload(ow == gw && oh == gh ? f.img : BilinearUpscale(f.img, ow, oh));
    ReliefSettings rs;
    rs.tier = tier;
    if (tier == ReliefTier::Parallax) {
        rs.strength = 1.0f;
        rs.parallaxMaxPx = 3.0f;
    }
    g.pass->SetSettings(rs);
    ID3D11DeviceContext1* ctx = dev.Context();
    ComPtr<ID3D11Query> batchDone;
    const D3D11_QUERY_DESC batchDesc{D3D11_QUERY_EVENT, 0};
    if (FAILED(dev.Device()->CreateQuery(&batchDesc, &batchDone))) { err = "GPU timing batch query"; return false; }
    ComPtr<ID3D11Texture2D> fillA, fillB;
    if (mode == 0) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 3840; td.Height = 2160; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev.Device()->CreateTexture2D(&td, nullptr, &fillA)) || FAILED(dev.Device()->CreateTexture2D(&td, nullptr, &fillB))) {
            err = "filler textures"; return false;
        }
    }
    auto filler = [&] {
        if (mode == 0)
            for (int k = 0; k < 6; ++k) ctx->CopyResource((k & 1) ? fillA.Get() : fillB.Get(), (k & 1) ? fillB.Get() : fillA.Get());
    };
    for (int i = 0; i < 400; ++i) {
        filler();
        if (!g.Analyse()) { err = "Analyse failed"; return false; }
        for (int k = 1; k <= phases; ++k) {
            const float phase = static_cast<float>(k) / phases;
            if (!g.pass->Apply(o.inSrv.Get(), Size{ow, oh}, o.outRtv.Get(), Size{ow, oh},
                               RectI{0, 0, static_cast<int32_t>(ow), static_cast<int32_t>(oh)}, phase, 1.0f, phases > 1)) {
                err = "Apply failed"; return false;
            }
        }
        if ((i & 7) == 7) {
            // Bound the queued work so the pass can collect completed timestamp sets in the next batch.
            // The wait and optional idle gap are outside the Analyse and Apply timestamp intervals.
            ctx->End(batchDone.Get());
            ctx->Flush();
            const ULONGLONG deadline = GetTickCount64() + 5000;
            HRESULT ready;
            while ((ready = ctx->GetData(batchDone.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
                   GetTickCount64() < deadline) {
                SwitchToThread();
            }
            if (ready != S_OK) { err = "GPU timing batch did not complete"; return false; }
            if (mode == 1) Sleep(2);
        }
    }
    const ReliefStats st = g.pass->Stats();
    analyseMs = st.analyseGpuMs;
    applyMs = st.applyGpuMs;
    if (analyseMs <= 0.0 || applyMs <= 0.0) { err = "GPU timestamp samples unavailable"; return false; }
    return true;
}

} // namespace

int RunReliefMotionBench(const moonsr::AdapterInfo& adapter) {
    constexpr uint32_t W = 640, H = 352, cellPx = 8;
    constexpr Size cells{W / cellPx, H / cellPx};
    std::wstring error;
    auto dev = GpuDevice::Create(&adapter.luid, error);
    if (!dev) { printf("error: %s\n", Narrow(error).c_str()); return 1; }
    printf("Motion depth bench | adapter %s\n", Narrow(adapter.name).c_str());
    Harness hs;
    std::string err;
    if (!hs.Init(*dev, W, H, err)) { printf("error: %s\n", err.c_str()); return 1; }
    World world;
    GenWorld(0, 2 * (W + 16), 2 * (H + 8), world);
    Frame frame;
    RenderFrame(world, W, H, 0.0f, 0.0f, false, frame);
    hs.Upload(frame.img);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = cells.width;
    td.Height = cells.height;
    td.ArraySize = 2;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> flow;
    ComPtr<ID3D11ShaderResourceView> view;
    HRESULT hr = dev->Device()->CreateTexture2D(&td, nullptr, &flow);
    if (SUCCEEDED(hr)) hr = dev->Device()->CreateShaderResourceView(flow.Get(), nullptr, &view);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(uint32_t);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Buffer> cutBuffer;
    if (SUCCEEDED(hr)) hr = dev->Device()->CreateBuffer(&bd, nullptr, &cutBuffer);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = 1;
    ComPtr<ID3D11ShaderResourceView> cutView;
    if (SUCCEEDED(hr)) hr = dev->Device()->CreateShaderResourceView(cutBuffer.Get(), &sd, &cutView);
    if (FAILED(hr)) { printf("error: synthetic motion resource creation failed (0x%08lx)\n", static_cast<unsigned long>(hr)); return 1; }

    MotionFieldView motion;
    motion.valid = true;
    motion.flow = view.Get();
    motion.cells = cells;
    motion.cellSizePx = cellPx;
    motion.stats = cutView.Get();
    ReliefSettings rs;
    rs.tier = ReliefTier::MotionDepth;
    rs.strength = 1.0f;
    rs.debugView = ReliefDebugView::Depth;
    hs.pass->SetSettings(rs);
    std::vector<std::array<float, 4>> vectors(static_cast<size_t>(cells.width) * cells.height);
    constexpr RectI foreground{212, 112, 364, 248};
    constexpr RectI independent{438, 156, 494, 222};
    constexpr RectI background{74, 92, 166, 202};
    auto within = [](float x, float y, const RectI& area) {
        return x >= static_cast<float>(area.left) && x < static_cast<float>(area.right) &&
               y >= static_cast<float>(area.top) && y < static_cast<float>(area.bottom);
    };
    auto upload = [&](bool rotation, uint32_t cut, bool still = false, float forwardConfidence = 1.0f) {
        for (uint32_t y = 0; y < cells.height; ++y)
            for (uint32_t x = 0; x < cells.width; ++x) {
                const float px = (static_cast<float>(x) + 0.5f) * cellPx;
                const float py = (static_cast<float>(y) + 0.5f) * cellPx;
                float dx = 2.0f, dy = 0.0f;
                if (still) {
                    dx = 0.0f;
                } else if (rotation) {
                    constexpr float focal = 360.0f, angle = 0.012f;
                    const float rayX = (px - W * 0.5f) / focal;
                    const float rayY = (py - H * 0.5f) / focal;
                    const float z = std::cos(angle) - rayX * std::sin(angle);
                    dx = focal * ((rayX * std::cos(angle) + std::sin(angle)) / z - rayX);
                    dy = focal * (rayY / z - rayY);
                } else {
                    if (within(px, py, foreground)) dx = 5.0f;
                    if (within(px, py, independent)) dy = 7.0f;
                }
                vectors[static_cast<size_t>(y) * cells.width + x] = {dx, dy, 1.0f, 0.0f};
            }
        auto* ctx = dev->Context();
        ctx->UpdateSubresource(flow.Get(), 1, nullptr, vectors.data(), cells.width * 4 * sizeof(float), 0);
        for (auto& v : vectors) { v[0] = -v[0]; v[1] = -v[1]; v[2] = forwardConfidence; }
        ctx->UpdateSubresource(flow.Get(), 0, nullptr, vectors.data(), cells.width * 4 * sizeof(float), 0);
        ctx->UpdateSubresource(cutBuffer.Get(), 0, nullptr, &cut, 0, 0);
    };

    Image out;
    auto render = [&](const MotionFieldView* field, int frames) {
        for (int i = 0; i < frames; ++i)
            if (!hs.Analyse(field) || !hs.Apply() || !hs.Read(out)) return false;
        return true;
    };
    auto mean = [](const Image& image, const RectI& area, auto value) {
        double sum = 0.0;
        for (int y = area.top; y < area.bottom; ++y)
            for (int x = area.left; x < area.right; ++x)
                sum += value(image.Row(static_cast<uint32_t>(y)) + 4 * x);
        return sum / (static_cast<double>(area.Width()) * area.Height());
    };
    auto neutralError = [](const uint8_t* p) { return std::abs(double(p[1]) - 128.0); };
    auto green = [](const uint8_t* p) { return double(p[1]); };
    auto maskRed = [](const uint8_t* p) { return double(p[2]) - double(p[1]); };
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };

    if (!render(nullptr, 8)) return 1;
    check(hs.pass->Stats().tierInEffect == ReliefTier::Parallax, "falls back to Parallax without a motion field");
    upload(true, 0);
    if (!render(&motion, 12)) return 1;
    check(hs.pass->Stats().tierInEffect == ReliefTier::MotionDepth, "uses a supplied matching motion field");
    const double rotationError = mean(out, foreground, neutralError);
    printf("  pure yaw: mean depth-code deviation %.3f\n", rotationError);
    check(rotationError <= 1.0, "pure yaw does not invent depth");

    upload(false, 0);
    if (!render(&motion, 84)) return 1;
    const RectI foregroundInterior{236, 136, 340, 223};
    const RectI independentInterior{450, 168, 482, 210};
    const double fgDepth = mean(out, foregroundInterior, green);
    const double bgDepth = mean(out, background, green);
    const double movingMask = mean(out, independentInterior, maskRed);
    printf("  lateral translation: foreground %.2f, background %.2f, independent-motion red %.2f\n",
           fgDepth, bgDepth, movingMask);
    const auto fitNote = hs.pass->Stats().reason;
    if (!fitNote.empty()) printf("  fit: %s\n", Narrow(fitNote).c_str());
    check(fgDepth > bgDepth + 12.0, "coarse depth distinguishes the near plane from the background");
    check(movingMask > 32.0, "independent motion is protected");

    rs.debugView = ReliefDebugView::None;
    hs.pass->SetSettings(rs);
    if (!render(&motion, 84)) return 1;
    const ReliefStats cost = hs.pass->Stats();
    const Image depthOutput = out;
    rs.tier = ReliefTier::Parallax;
    hs.pass->SetSettings(rs);
    if (!render(&motion, 84)) return 1;
    auto delta = [&](const Image& image, const RectI& area) {
        double sum = 0.0;
        for (int y = area.top; y < area.bottom; ++y)
            for (int x = area.left; x < area.right; ++x)
                for (int c = 0; c < 3; ++c)
                    sum += std::abs(int(image.Row(static_cast<uint32_t>(y))[4 * x + c]) -
                                    int(frame.img.Row(static_cast<uint32_t>(y))[4 * x + c]));
        return sum / (3.0 * area.Width() * area.Height());
    };
    const double plainChange = delta(out, independentInterior);
    const double guardedChange = delta(depthOutput, independentInterior);
    printf("  independent region mean RGB change: Parallax %.3f, Motion depth %.3f code values\n",
           plainChange, guardedChange);
    check(plainChange > 0.0 && guardedChange < 0.6 * plainChange,
          "motion mask reduces relief changes on the moving object");
    const ReliefStats base = hs.pass->Stats();
    printf("  %ux%u GPU timestamp medians: analyse %.4f ms, apply %.4f ms (Motion depth)\n",
           W, H, cost.analyseGpuMs, cost.applyGpuMs);
    printf("  %ux%u GPU timestamp medians: analyse %.4f ms, apply %.4f ms (Parallax, same flow)\n",
           W, H, base.analyseGpuMs, base.applyGpuMs);

    // With asymmetric confidence, relief should recover across generated phases instead of switching on at phase 1.
    upload(false, 0, true, 0.0f);
    if (!render(&motion, 24)) return 1;
    const double newestChange = delta(out, background);
    double earlyChange = 0.0;
    bool smoothRecovery = true;
    for (const float phase : {0.25f, 0.5f, 0.75f}) {
        if (!hs.Apply(phase) || !hs.Read(out)) return 1;
        const double change = delta(out, background);
        smoothRecovery &= change > earlyChange && change < newestChange;
        earlyChange = change;
    }
    check(smoothRecovery, "asymmetric motion confidence ramps relief toward the newest real frame");

    // The post-generation filter keeps the displacement visible while attenuating the colour sweep.
    rs.shading = rs.cavity = 0.0f;
    hs.pass->SetSettings(rs);
    upload(false, 0, true);
    if (!render(&motion, 24)) return 1;
    bool filtered = true;
    for (const float phase : {0.25f, 0.5f, 0.75f, 1.0f}) {
        if (!hs.Apply(phase) || !hs.Read(out)) return 1;
        const double regularChange = delta(out, foregroundInterior);
        if (!hs.Apply(phase, true) || !hs.Read(out)) return 1;
        const double filteredChange = delta(out, foregroundInterior);
        filtered &= filteredChange > regularChange * 0.35 && filteredChange < regularChange * 0.8;
    }
    check(filtered, "filtered Parallax retains displacement on real and generated frames");

    // A coherent selected warp may shift height lookups on generated frames only.
    D3D11_TEXTURE2D_DESC selectionDesc{};
    selectionDesc.Width = W / 2;
    selectionDesc.Height = H / 2;
    selectionDesc.MipLevels = selectionDesc.ArraySize = 1;
    selectionDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    selectionDesc.SampleDesc.Count = 1;
    selectionDesc.Usage = D3D11_USAGE_DEFAULT;
    selectionDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> selection;
    ComPtr<ID3D11ShaderResourceView> selectionView;
    hr = dev->Device()->CreateTexture2D(&selectionDesc, nullptr, &selection);
    if (SUCCEEDED(hr)) hr = dev->Device()->CreateShaderResourceView(selection.Get(), nullptr, &selectionView);
    if (FAILED(hr)) { printf("error: synthetic selection resource creation failed (0x%08lx)\n", static_cast<unsigned long>(hr)); return 1; }
    std::vector<std::array<float, 4>> hypotheses(static_cast<size_t>(selectionDesc.Width) * selectionDesc.Height);
    auto setSelection = [&](float confidence) {
        std::fill(hypotheses.begin(), hypotheses.end(), std::array{5.0f, 0.0f, 0.5f, confidence});
        dev->Context()->UpdateSubresource(selection.Get(), 0, nullptr, hypotheses.data(),
                                         selectionDesc.Width * static_cast<UINT>(sizeof(hypotheses.front())), 0);
    };
    setSelection(1.0f);
    Image baseline;
    if (!hs.Apply(0.5f, true) || !hs.Read(baseline) ||
        !hs.Apply(0.5f, true, selectionView.Get()) || !hs.Read(out)) return 1;
    check(out.px != baseline.px, "coherent selected motion changes generated height sampling");
    setSelection(0.0f);
    if (!hs.Apply(0.5f, true, selectionView.Get()) || !hs.Read(out)) return 1;
    check(out.px == baseline.px, "unreliable selected motion falls back to coarse flow");
    setSelection(1.0f);
    if (!hs.Apply(1.0f, true) || !hs.Read(baseline) ||
        !hs.Apply(1.0f, true, selectionView.Get()) || !hs.Read(out)) return 1;
    check(out.px == baseline.px, "interpolation selection leaves real frames pixel-exact");

    rs.shading = rs.cavity = 1.0f;

    rs.tier = ReliefTier::MotionDepth;
    hs.pass->SetSettings(rs);
    upload(false, 0, true);
    if (!render(&motion, 24)) return 1;
    const Image real = out;
    bool steady = true;
    for (const float phase : {0.25f, 0.5f, 0.75f}) {
        if (!hs.Apply(phase) || !hs.Read(out)) return 1;
        steady &= out.px == real.px;
    }
    check(steady, "stationary generated frames match the real frame at 2x and 4x phases");

    // Keep the analysed height and depth fixed and only vary the colour passed to Apply.
    rs.debugView = ReliefDebugView::Offset;
    hs.pass->SetSettings(rs);
    if (!hs.Apply() || !hs.Read(out)) return 1;
    const auto offsetCode = [](const uint8_t* p) { return double(p[0]); };
    const RectI wholeFrame{0, 0, W, H};
    const double normalOffset = mean(out, wholeFrame, offsetCode);
    Image highlighted = frame.img;
    for (size_t i = 0; i < highlighted.px.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            highlighted.px[i + c] = static_cast<uint8_t>(220 + (35 * highlighted.px[i + c]) / 255);
    hs.Upload(highlighted);
    if (!hs.Apply() || !hs.Read(out)) return 1;
    const double brightOffset = mean(out, wholeFrame, offsetCode);
    check(normalOffset > 0.0 && brightOffset < 0.8 * normalOffset, "bright output rolls off Motion depth displacement");
    hs.Upload(frame.img);
    rs.debugView = ReliefDebugView::None;
    hs.pass->SetSettings(rs);

    // The unchanged motion pair represents a large exposure change. An unwarped luminance
    // comparison can mistake it for a scene cut even though the motion analysis remains valid.
    Image darkened = frame.img;
    for (size_t i = 0; i < darkened.px.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            darkened.px[i + c] /= 3;
    hs.Upload(darkened);
    if (!render(&motion, 1)) return 1;
    check(out.px != darkened.px, "large luminance change with a valid motion pair keeps relief active");
    hs.Upload(frame.img);

    upload(false, 1);
    if (!render(&motion, 1)) return 1;
    bool noFlash = out.px == frame.img.px;
    for (const float phase : {0.25f, 0.5f, 0.75f}) {
        if (!hs.Apply(phase, true, selectionView.Get()) || !hs.Read(out)) return 1;
        noFlash &= out.px == frame.img.px;
    }
    check(noFlash, "scene cut passes real and generated colours through without a flash");
    rs.debugView = ReliefDebugView::Depth;
    hs.pass->SetSettings(rs);
    if (!hs.Apply() || !hs.Read(out)) return 1;
    check(mean(out, foreground, neutralError) <= 1.0, "scene cut clears depth immediately");

    hs.pass->Reset();
    rs.tier = ReliefTier::Parallax;
    rs.debugView = ReliefDebugView::None;
    hs.pass->SetSettings(rs);
    upload(false, 0, true);
    Frame overlay;
    for (int k = 0; k < 48; ++k) {
        RenderFrame(world, W, H, 0.25f * k, 0.1f * k, true, overlay);
        hs.Upload(overlay.img);
        if (!hs.Analyse(&motion)) return 1;
    }
    bool hudExact = true;
    size_t hudPixels = 0;
    for (const float phase : {0.25f, 0.5f, 0.75f}) {
        if (!hs.Apply(phase, true, selectionView.Get()) || !hs.Read(out)) return 1;
        for (size_t i = 0; i < overlay.hud.size(); ++i) {
            if (overlay.hud[i] != kOpaqueHud) continue;
            ++hudPixels;
            hudExact &= std::memcmp(&out.px[4 * i], &overlay.img.px[4 * i], 4) == 0;
        }
    }
    check(hudExact && hudPixels > 0 && out.px != overlay.img.px, "selected motion leaves opaque HUD pixels untouched");
    printf("Motion depth checks: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// Real-content check: one image (e.g. an owner screenshot) through the real ReliefPass. 24 identical frames (a static
// scene), then 24 frames of a 1 px per frame horizontal pan of a window inset by 32 px (so a HUD drawn into the image
// moves with it: HUD protection cannot engage here, which is the conservative case for coverage). Reports the
// coverage, auto-disable state and reason after each phase, and saves Split and Protection views at each strength.
int RunReliefImage(const ReliefBenchOptions& o, const moonsr::AdapterInfo& adapter) {
    std::wstring werr;
    auto dev = GpuDevice::Create(&adapter.luid, werr);
    if (!dev) { printf("error: GpuDevice::Create: %s\n", Narrow(werr).c_str()); return 1; }
    int rc = 0;
    for (const auto& path : o.images) {
        Image full;
        std::string err;
        if (!ReadPng(path.wstring(), full, err)) {
            printf("error: cannot decode %s: %s (WIC codec missing for this format?)\n", path.string().c_str(), err.c_str());
            rc = 1;
            continue;
        }
        constexpr uint32_t kInset = 32;
        if (full.w <= 2 * kInset || full.h <= 2 * kInset) { printf("error: %s too small\n", path.string().c_str()); rc = 1; continue; }
        const uint32_t W = full.w - 2 * kInset, H = full.h - 2 * kInset;
        Harness hs;
        if (!hs.Init(*dev, W, H, err)) { printf("error: %s\n", err.c_str()); return 1; }
        Image frame;
        frame.Alloc(W, H);
        auto window = [&](uint32_t ox) {
            for (uint32_t y = 0; y < H; ++y) std::memcpy(frame.Row(y), full.Row(y + kInset) + (ox * 4), static_cast<size_t>(W) * 4);
        };
        ReliefSettings rs;
        rs.tier = o.parallax ? ReliefTier::Parallax : ReliefTier::Shading;
        rs.strength = o.strengths.empty() ? 0.5f : o.strengths.front();
        if (o.detailScalePx > 0.0f) rs.detailScalePx = o.detailScalePx;
        hs.pass->SetSettings(rs);
        Image out;
        auto report = [&](const char* phase) {
            for (int i = 0; i < 4; ++i) { hs.Analyse(); hs.Apply(); hs.Read(out); } // lets the lagged stats arrive
            const ReliefStats st = hs.pass->Stats();
            printf("  %-28s coverage %.3f  active %s  auto-disabled %s  reason '%s'\n", phase, st.texturedCoverage,
                   st.active ? "yes" : "no", st.autoDisabled ? "yes" : "no", Narrow(st.reason).c_str());
        };
        printf("%s (%ux%u analysed, inset %u px)\n", path.filename().string().c_str(), W, H, kInset);
        window(kInset);
        hs.Upload(frame);
        for (int k = 0; k < 24; ++k) { hs.Analyse(); hs.Apply(); }
        report("static (24 frames)");
        for (uint32_t k = 0; k < 24; ++k) {
            window(kInset - 12 + k);
            hs.Upload(frame);
            hs.Analyse();
            hs.Apply();
        }
        report("after 24-frame pan");
        // A still camera in a live game is not a still image: TAA jitter, film grain and small animations change most
        // pixels by a code value or two every frame. Simulate that with +-2 levels of random noise for 90 frames.
        {
            uint32_t rng = 0x9E3779B9u;
            Image base = frame;
            for (int k = 0; k < 90; ++k) {
                for (size_t i = 0; i < frame.px.size(); ++i) {
                    if ((i & 3) == 3) continue;
                    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                    const int v = static_cast<int>(base.px[i]) + static_cast<int>(rng % 5) - 2;
                    frame.px[i] = static_cast<uint8_t>(std::clamp(v, 0, 255));
                }
                hs.Upload(frame);
                hs.Analyse();
                hs.Apply();
            }
            report("still camera + noise (90)");
            if (o.saveImages) {
                std::error_code ec;
                fs::create_directories(o.outDir, ec);
                rs.debugView = ReliefDebugView::Protection;
                hs.pass->SetSettings(rs);
                std::string e;
                if (hs.Apply() && hs.Read(out))
                    WritePng((o.outDir / (path.stem().string() + "_noise_protection.png")).wstring(), out, e);
                rs.debugView = ReliefDebugView::None;
                hs.pass->SetSettings(rs);
            }
            frame = base;
        }
        if (o.saveImages) {
            std::error_code ec;
            fs::create_directories(o.outDir, ec);
            const std::string stem = path.stem().string();
            std::string e;
            WritePng((o.outDir / (stem + "_input.png")).wstring(), frame, e);
            hs.Upload(frame);
            for (float s : o.strengths) {
                rs.strength = s;
                char tag[32];
                snprintf(tag, sizeof(tag), "_s%03d", static_cast<int>(std::lround(s * 100)));
                for (auto [view, name] : {std::pair{ReliefDebugView::Split, "_split"},
                                          std::pair{ReliefDebugView::Protection, "_protection"},
                                          std::pair{ReliefDebugView::Height, "_height"},
                                          std::pair{ReliefDebugView::None, "_relief"}}) {
                    rs.debugView = view;
                    hs.pass->SetSettings(rs);
                    hs.Analyse();
                    if (hs.Apply() && hs.Read(out)) WritePng((o.outDir / (stem + tag + name + ".png")).wstring(), out, e);
                }
                rs.debugView = ReliefDebugView::None;
                hs.pass->SetSettings(rs);
                SaveCrop(o.outDir, stem + tag + "_crop_before_after", frame, out, static_cast<int>(W) / 2 - 120,
                         static_cast<int>(H) / 2 - 80, 240, 2);
            }
        }
    }
    return rc;
}

int RunReliefBench(const ReliefBenchOptions& o, const moonsr::AdapterInfo& adapter) {
    std::wstring werr;
    auto dev = GpuDevice::Create(&adapter.luid, werr);
    if (!dev) { printf("error: GpuDevice::Create: %s\n", Narrow(werr).c_str()); return 1; }
    printf("Smart parallax (relief) bench | adapter %s | scenes 960x540, pan (0.25, 0.1) px/frame, 40 frames | metrics on the last frame\n",
           Narrow(adapter.name).c_str());
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
        if (!ok) ++failures;
    };
    for (float strength : o.strengths) {
        printf("\nstrength %.2f (shading 1, cavity 1, detail %.2f px, light 135/45)\n", strength, o.detailScalePx > 0.0f ? o.detailScalePx : moonsr::ReliefSettings{}.detailScalePx);
        printf("%-16s %6s %6s %6s %6s | %7s %7s %6s %6s %6s %6s | %5s %4s | emboss corr/false/halo | clarity corr/false/halo\n",
               "scene", "Hpear", "Hspear", "sign", "shadeR", "intr%", "false%", "hudMax", "halo", "flick", "cov", "auto", "act");
        printf("  (halo: silhouette band / interior; HUD scene adds hudBand / interior after the table)\n");
        std::vector<SceneResult> results;
        float baselineGain[2] = {0, 0};
        for (int s = 0; s < kSceneCount; ++s) {
            SceneResult r;
            std::string err;
            if (!RunScene(*dev, s, strength, o, baselineGain, r, err)) { printf("error: scene %s: %s\n", kScenes[s].name, err.c_str()); return 1; }
            auto halo = [](double h) { static char b[4][16]; static int n = 0; char* p = b[n++ & 3]; if (h < 0) snprintf(p, 16, "  -  "); else snprintf(p, 16, "%5.2f", h); return p; };
            printf("%-16s %6.3f %6.3f %6.3f %6.3f | %7.3f %7.3f %6.0f %6s %6.3f %6.3f | %5s %4s | %6.3f %6.3f %5s | %6.3f %6.3f %5s%s%s\n",
                   r.name.c_str(), r.pearsonH, r.spearmanH, r.signAcc, r.shadeCorr, 100 * r.interior, 100 * r.falseRelief, r.hudMax,
                   halo(r.halo), r.flicker, r.coverage, r.autoDisabled ? "yes" : "no", r.active ? "yes" : "no", r.embossCorr,
                   100 * r.embossFalse, halo(r.embossHalo), r.clarityCorr, 100 * r.clarityFalse, halo(r.clarityHalo),
                   r.reason.empty() ? "" : " | ", Narrow(r.reason).c_str());
            if (r.hudHalo >= 0) printf("%-16s HUD-adjacent band / interior: %.2f\n", "", r.hudHalo);
            if (kScenes[s].hud) printf("%-16s translucent HUD maximum RGB change: %.0f levels\n", "", r.translucentHudMax);
            if (r.heightHalo >= 0) printf("%-16s raw height halo-band / interior energy: %.3f\n", "", r.heightHalo);
            if (o.parallax) printf("%-16s maximum offset %.3f px, opaque HUD maximum %.3f px\n",
                                  "", r.maxOffsetPx, r.maxHudOffsetPx);
            if (kScenes[s].texturedTruth)
                printf("%-16s bevels: mean change on truly lit slopes %+.2f %%, on truly shadowed slopes %+.2f %%\n", "",
                       100 * r.litEdge, 100 * r.shadowEdge);
            results.push_back(r);
        }
        // Self-checks (thresholds: research note 7.1 M1/M2 exits where they apply; first-iteration values otherwise).
        int good = 0;
        for (int s = 0; s < 5; ++s) good += (kScenes[s].texturedTruth && results[s].spearmanH >= 0.5) ? 1 : 0;
        check(good >= 3, "height Spearman >= 0.5 on at least 3 of the 4 textured materials (bricks, cobblestones, rock, wood): " + std::to_string(good));
        for (int s = 0; s < kSceneCount; ++s) {
            const SceneResult& r = results[s];
            const SceneDef& d = kScenes[s];
            if (s == 0) {
                check(strength == 0.0f || r.staticChange > 0, "bricks: relief remains visible after the camera stops");
                check(r.staticFlicker <= 1, "bricks: settled still frames vary by at most one RGB code value (max " +
                                            std::to_string(r.staticFlicker) + ")");
            }
            if (d.expectAutoDisable) check(r.autoDisabled, std::string(d.name) + ": auto-disabled");
            else if (d.texturedTruth) {
                check(!r.autoDisabled && r.active, std::string(d.name) + ": active, not auto-disabled");
                check(r.shadeCorr > 0.0, std::string(d.name) + ": shading change correlates positively with the true relief shading");
                check(r.litEdge > 0.0 && r.shadowEdge < 0.0, std::string(d.name) + ": lit bevels (light-facing slopes brighten, opposite slopes darken)");
                check(r.flicker < 0.5, std::string(d.name) + ": relief flicker along the pan < 0.5");
            }
            if (d.hud) {
                check(r.hudMax == 0.0, std::string(d.name) + ": opaque HUD untouched (max |d| = 0)");
                check(r.generatedHudMax == 0.0, std::string(d.name) + ": opaque HUD untouched at generated phases");
            }
            if (o.parallax) {
                if (d.hud) check(r.maxHudOffsetPx == 0.0, std::string(d.name) + ": zero offset on opaque HUD");
                if (d.expectAutoDisable) check(r.maxOffsetPx == 0.0, std::string(d.name) + ": zero offset when paused");
                if (d.texturedTruth) check(r.maxOffsetPx > 0.0 && r.maxOffsetPx <= 3.0 * strength + 3.0 / 255.0,
                                           std::string(d.name) + ": nonzero offset within the configured cap");
            }
            if (r.halo >= 0) check(r.halo <= 0.25, std::string(d.name) + ": silhouette halo band <= 25 % of the interior change");
            if (r.hudHalo >= 0) check(r.hudHalo <= 1.0, std::string(d.name) + ": surface next to the HUD gets no more change than the interior (no HUD-edge emboss)");
            // False relief scales with strength: <= 1 % mean at the proposed default 0.5 (painted_wall is the polarity
            // trap; the other flat scenes must stay near 0).
            if (!d.texturedTruth) {
                const double lim = 0.01 * std::max(1.0, static_cast<double>(strength) / 0.5);
                char what[128];
                snprintf(what, sizeof(what), ": false relief on flat regions < %.2f %% mean", 100 * lim);
                check(r.falseRelief < lim, std::string(d.name) + what);
            }
        }
    }

    if (o.parallax) {
        constexpr std::pair<const char*, CameraPose> paths[] = {
            {"strafe", {0.0025f, 0.0f, 0.0f}}, {"dolly", {0.0f, 0.0025f, 0.0f}}, {"yaw", {0.0f, 0.0f, 0.002f}},
            {"orbit", {0.0f, 0.0f, 0.002f, true}},
        };
        printf("\nWall displacement reference (shading/cavity off, SSIM and mean extra 8-bit warp error)\n");
        for (const float capPx : {2.0f, 3.0f}) {
            printf("  %.0f px cap\n", capPx);
            for (const auto& [path, step] : paths) {
                double flat = 0.0, relief = 0.0, warpError = 0.0;
                std::string err;
                const fs::path exportPrefix = o.saveImages && capPx == 3.0f ? o.outDir / (std::string("wall-") + path) : fs::path{};
                if (!MeasureParallaxPath(*dev, step, capPx, flat, relief, warpError, exportPrefix, err)) {
                    printf("error: %s\n", err.c_str());
                    return 1;
                }
                printf("  %-7s SSIM %.6f -> %.6f, extra warp error %+.4f code values\n", path, flat, relief, warpError);
                if (step.yaw == 0.0f) check(relief > flat, std::string(path) + ": offset improves SSIM to the displaced wall");
                check(warpError <= 0.5, std::string(path) + ": mean extra warp error <= 0.5 code values");
            }
        }
    }

    if (o.timing) {
        const ReliefTier tier = o.parallax ? ReliefTier::Parallax : ReliefTier::Shading;
        printf("\nGPU time (64-sample medians, %s, default detail)\n",
               o.parallax ? "Parallax, 100% strength, 3 px cap" : "Shading, 50% strength");
        const uint32_t sizes[][4] = {{1280, 720, 1920, 1080}, {1920, 1080, 1920, 1080}, {1920, 1080, 2560, 1440}};
        double warmA = 0, warmP = 0;
        std::string warmErr;
        (void)TimeRelief(*dev, 1280, 720, 1920, 1080, 0, tier, warmA, warmP, warmErr); // warm-up (first-use paging), discarded
        for (const int mode : {0, 1}) {
            printf("  %s\n", mode == 0 ? "loaded (GPU kept busy by a filler copy between frames, as under a running game)"
                                       : "idle gaps (Windows Sleep(2) every 8 frames)");
            for (const auto& s : sizes) {
                double a = 0, p = 0;
                std::string err;
                if (!TimeRelief(*dev, s[0], s[1], s[2], s[3], mode, tier, a, p, err)) { printf("error: timing: %s\n", err.c_str()); return 1; }
                printf("    %4ux%-4u -> %4ux%-4u  analyse %.4f ms  apply %.4f ms  total %.4f ms\n", s[0], s[1], s[2], s[3], a, p, a + p);
            }
        }
        printf("  loaded, synthetic generated phases on a still input (no motion field or generator cost)\n");
        for (const auto& s : sizes) {
            if (s[0] == s[2] && s[1] == s[3]) continue;
            for (const int phases : {2, 4}) {
                double a = 0, p = 0;
                std::string err;
                if (!TimeRelief(*dev, s[0], s[1], s[2], s[3], 0, tier, a, p, err, phases)) {
                    printf("error: phase timing: %s\n", err.c_str());
                    return 1;
                }
                printf("    %4ux%-4u -> %4ux%-4u  %dx  analyse %.4f ms per real frame  apply %.4f ms per display phase\n",
                       s[0], s[1], s[2], s[3], phases, a, p);
            }
        }
    }
    printf("\nRelief bench self-checks: %s (%d failed)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

} // namespace bench
