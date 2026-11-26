// moonsr_bench --aa: anti-aliasing measurement, milestone M1 of documentation/research/anti_aliasing.md.
// Owner: upscaling-engineer (algorithms) with shader-engineer (HLSL under shaders/aa/).
//
// Synthetic scenes with a known structure are generated on the CPU as hard, aliased 8-bit pictures (what a game without
// anti-aliasing produces and what MoonSR captures), the real AaPass runs on the GPU, and the following are measured on
// the read-back output:
//   - edge-step reduction: the mean over scanlines of the largest luma step in that scanline (and the same over
//     columns), on hard edges at 20, 30, 45, 60 and 70 degrees. An aliased edge has one large step per row; the filter
//     spreads it over two pixels, so the largest step must shrink;
//   - exact edges: the same measure on 0 and 90 degree edges, which the pixel grid already resolves exactly. The
//     filter must leave them (nearly) alone: blurring them is softening, not anti-aliasing;
//   - texture energy: RMS of the 4-neighbour Laplacian on a 2 px checker with noise, after / before (must stay close
//     to 1: high-frequency texture must not be blurred);
//   - thin-line contrast: mean luma of one-pixel lines after / before (text and one-pixel strokes must stay sharp);
//   - HUD protection: changed pixels and maximum change inside the known text mask over a busy background;
//   - gradient: maximum change on a smooth gradient (nothing to anti-alias there);
//   - AaStats after the run: aliased-edge coverage, effective strength, reason; GPU time (timestamp median).
// These are measurements of this filter, not a comparison with in-game anti-aliasing.
#include "aa_bench.h"

#include "imageops.h"
#include "metrics.h"

#include "aa/aa_pass.h"
#include "gpu/gpu_device.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>

namespace bench {
namespace {

using Microsoft::WRL::ComPtr;
using namespace moonsr;
namespace fs = std::filesystem;

constexpr float kPi = 3.14159265358979f;

float Lum8(uint8_t b, uint8_t g, uint8_t r) { return 0.114f * b + 0.587f * g + 0.299f * r; }
float LumaAt(const Image& im, int x, int y) {
    const uint8_t* p = im.Row(static_cast<uint32_t>(y)) + static_cast<size_t>(x) * 4;
    return Lum8(p[0], p[1], p[2]);
}
void SetPx(Image& im, int x, int y, uint8_t b, uint8_t g, uint8_t r) {
    uint8_t* p = im.Row(static_cast<uint32_t>(y)) + static_cast<size_t>(x) * 4;
    p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
}

enum class Kind { Edge, Exact, Lines, Texture, Hud, Gradient };

struct Scene {
    std::string name;
    Kind kind = Kind::Edge;
    Image img;
    std::vector<uint8_t> hud; // 1 = HUD / text stroke pixel
};

void GenEdge(int W, int H, float degrees, Image& im) {
    im.Alloc(W, H);
    const float a = degrees * kPi / 180.0f;
    const float nx = std::cos(a), ny = std::sin(a);
    const float cx = W * 0.5f, cy = H * 0.5f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float d = (x + 0.5f - cx) * nx + (y + 0.5f - cy) * ny;
            const uint8_t v = d < 0.0f ? 218 : 34;
            SetPx(im, x, y, v, v, v);
        }
}

void GenLines(int W, int H, Image& im) {
    im.Alloc(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool on = (x % 8 == 0) || (y % 8 == 0);
            const uint8_t v = on ? 225 : 28;
            SetPx(im, x, y, v, v, v);
        }
}

void GenTexture(int W, int H, Image& im) {
    im.Alloc(W, H);
    uint32_t seed = 0x12345678u;
    auto rnd = [&seed] {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float>(seed & 0xFFFF) / 65535.0f;
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool c = (((x / 2) + (y / 2)) & 1) != 0;
            const float n = (rnd() - 0.5f) * 34.0f;
            const float v = std::clamp((c ? 196.0f : 54.0f) + n, 0.0f, 255.0f);
            const uint8_t b = static_cast<uint8_t>(v);
            SetPx(im, x, y, b, b, b);
        }
}

// Busy background plus one-pixel white strokes that look like small text; the mask marks exactly the stroke pixels.
void GenTextHud(int W, int H, Scene& s) {
    Image& im = s.img;
    im.Alloc(W, H);
    s.hud.assign(static_cast<size_t>(W) * H, 0);
    uint32_t seed = 0x9e3779b9u;
    auto rnd = [&seed] {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float>(seed & 0xFFFF) / 65535.0f;
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool c = (((x / 2) + (y / 2)) & 1) != 0;
            const float n = (rnd() - 0.5f) * 26.0f;
            const float v = std::clamp((c ? 150.0f : 70.0f) + n, 0.0f, 255.0f);
            const uint8_t b = static_cast<uint8_t>(v);
            SetPx(im, x, y, b, b, b);
        }
    // Three rows of 12 one-pixel glyph strokes; each glyph is a vertical stem with two horizontal arms.
    for (int row = 0; row < 3; ++row) {
        const int y0 = 80 + row * 46;
        for (int g = 0; g < 12; ++g) {
            const int x0 = 70 + g * 90;
            for (int k = 0; k < 9; ++k) {
                const int y = y0 + k;
                if (y >= 0 && y < H && x0 >= 0 && x0 < W) {
                    SetPx(im, x0, y, 245, 245, 245);
                    s.hud[static_cast<size_t>(y) * W + x0] = 1;
                }
            }
            for (int k = 1; k <= 5; ++k) {
                const int x = x0 + k;
                if (x >= 0 && x < W) {
                    SetPx(im, x, y0, 245, 245, 245);
                    s.hud[static_cast<size_t>(y0) * W + x] = 1;
                    SetPx(im, x, y0 + 8, 245, 245, 245);
                    s.hud[static_cast<size_t>(y0 + 8) * W + x] = 1;
                }
            }
        }
    }
}

void GenGradient(int W, int H, Image& im) {
    im.Alloc(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float u = static_cast<float>(x) / std::max(1, W - 1);
            const float v = static_cast<float>(y) / std::max(1, H - 1);
            const float t = 0.5f * u + 0.5f * v;
            const float val = 24.0f + 208.0f * (t * t * (3.0f - 2.0f * t));
            const uint8_t b = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
            SetPx(im, x, y, b, b, b);
        }
}

// Mean over scanlines of the largest horizontal luma step, averaged with the same measure over columns. An aliased
// edge puts the whole transition into one pixel pair; a resolved edge spreads it over two pairs and the largest step
// halves. Independent of how many edge pixels there are, so it is not diluted by large flat areas.
double MeanMaxStep(const Image& im) {
    double sumX = 0.0, sumY = 0.0;
    for (uint32_t y = 1; y + 1 < im.h; ++y) {
        float m = 0.0f;
        for (uint32_t x = 1; x + 2 < im.w; ++x)
            m = std::max(m, std::abs(LumaAt(im, static_cast<int>(x) + 1, static_cast<int>(y)) -
                                     LumaAt(im, static_cast<int>(x), static_cast<int>(y))));
        sumX += m;
    }
    for (uint32_t x = 1; x + 1 < im.w; ++x) {
        float m = 0.0f;
        for (uint32_t y = 1; y + 2 < im.h; ++y)
            m = std::max(m, std::abs(LumaAt(im, static_cast<int>(x), static_cast<int>(y) + 1) -
                                     LumaAt(im, static_cast<int>(x), static_cast<int>(y))));
        sumY += m;
    }
    return 0.5 * (sumX / static_cast<double>(im.h - 2) + sumY / static_cast<double>(im.w - 2));
}

// RMS of the 4-neighbour Laplacian on luma, 0..255 units.
double HighPassEnergy(const Image& im) {
    double sum = 0.0;
    size_t n = 0;
    for (uint32_t y = 1; y + 1 < im.h; ++y)
        for (uint32_t x = 1; x + 1 < im.w; ++x) {
            const float c = LumaAt(im, static_cast<int>(x), static_cast<int>(y));
            const float lap = 4.0f * c - LumaAt(im, static_cast<int>(x) + 1, static_cast<int>(y)) -
                              LumaAt(im, static_cast<int>(x) - 1, static_cast<int>(y)) -
                              LumaAt(im, static_cast<int>(x), static_cast<int>(y) + 1) -
                              LumaAt(im, static_cast<int>(x), static_cast<int>(y) - 1);
            sum += static_cast<double>(lap) * lap;
            ++n;
        }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

double MeanLuma(const Image& im, const std::vector<uint8_t>& mask) {
    double sum = 0.0;
    size_t n = 0;
    for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x) {
            if (!mask[static_cast<size_t>(y) * im.w + x]) continue;
            sum += LumaAt(im, static_cast<int>(x), static_cast<int>(y));
            ++n;
        }
    return n ? sum / static_cast<double>(n) : 0.0;
}

struct HudResult {
    size_t changed = 0, total = 0;
    int maxDiff = 0;
};

HudResult HudDifference(const Image& a, const Image& b, const std::vector<uint8_t>& mask) {
    HudResult r;
    for (uint32_t y = 0; y < a.h; ++y)
        for (uint32_t x = 0; x < a.w; ++x) {
            if (!mask[static_cast<size_t>(y) * a.w + x]) continue;
            ++r.total;
            const uint8_t* pa = a.Row(y) + static_cast<size_t>(x) * 4;
            const uint8_t* pb = b.Row(y) + static_cast<size_t>(x) * 4;
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(static_cast<int>(pa[c]) - static_cast<int>(pb[c])));
            r.maxDiff = std::max(r.maxDiff, d);
            if (d > 0) ++r.changed;
        }
    return r;
}

int MaxDifference(const Image& a, const Image& b) {
    int d = 0;
    for (size_t i = 0; i < a.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(static_cast<int>(a.px[i + c]) - static_cast<int>(b.px[i + c])));
    return d;
}

// How much of the picture the filter touched: share of pixels with any channel change, mean and maximum change per pixel
// (0..255). On a real frame this is the honest "did it do anything" number; the energy ratio says whether the finest
// detail survived.
struct PixelChangeStats {
    double changedPct = 0.0;
    double meanAbs = 0.0;
    int maxDiff = 0;
};

PixelChangeStats PixelChange(const Image& a, const Image& b) {
    PixelChangeStats s;
    size_t changed = 0;
    double sum = 0.0;
    const size_t n = a.px.size() / 4;
    for (size_t i = 0; i < a.px.size(); i += 4) {
        int d = 0;
        for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(static_cast<int>(a.px[i + c]) - static_cast<int>(b.px[i + c])));
        if (d > 0) ++changed;
        s.maxDiff = std::max(s.maxDiff, d);
        sum += static_cast<double>(d);
    }
    s.changedPct = n ? 100.0 * static_cast<double>(changed) / static_cast<double>(n) : 0.0;
    s.meanAbs = n ? sum / static_cast<double>(n) : 0.0;
    return s;
}

// ------------------------------------------------------------------ GPU harness
struct Harness {
    GpuDevice* dev = nullptr;
    std::unique_ptr<AaPass> pass;
    ComPtr<ID3D11Texture2D> inTex, outTex, staging;
    ComPtr<ID3D11ShaderResourceView> inSrv;
    ComPtr<ID3D11RenderTargetView> outRtv;
    uint32_t w = 0, h = 0;

    bool Init(GpuDevice& d, uint32_t W, uint32_t H, const AaSettings& s, std::string& err) {
        dev = &d; w = W; h = H;
        std::wstring e;
        pass = AaPass::Create(d, e);
        if (!pass) { err = "AaPass::Create: " + Narrow(e); return false; }
        pass->SetSettings(s);
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
    bool Apply() {
        return pass->Apply(inSrv.Get(), Size{w, h}, outRtv.Get(), Size{w, h},
                           RectI{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)}, 1.0f);
    }
    bool Read(Image& out) {
        ID3D11DeviceContext1* ctx = dev->Context();
        ctx->CopyResource(staging.Get(), outTex.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out.Alloc(w, h);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(out.Row(y), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, w * 4);
        ctx->Unmap(staging.Get(), 0);
        return true;
    }
    // Runs `frames` passes and drains the queue with a blocking readback, so the pass's own non-blocking timestamp
    // queries are complete when Stats() is read next.
    void Run(int frames) {
        for (int i = 0; i < frames; ++i) Apply();
        Image sink;
        Read(sink);
        Apply();
        Read(sink);
    }
};

struct SceneResult {
    std::string name;
    double before = 0, after = 0, ratio = 0;
    int maxDiff = 0;
    double hudChangedPct = 0;
    int hudMaxDiff = 0;
    float coverage = 0;
    bool ok = false;
};

AaSettings DefaultSettings(const AaBenchOptions& o) {
    AaSettings s;
    s.mode = AaMode::Spatial;
    s.quality = o.quality == 0   ? AaQuality::Performance
              : o.quality == 2   ? AaQuality::Quality
                                 : AaQuality::Balanced;
    s.strength = o.strength >= 0.0f ? o.strength : 1.0f;
    s.edgeThreshold = 0.10f;
    s.edgeThresholdMin = 0.03f;
    s.protectText = o.protectText;
    s.protectHud = true;
    s.debugView = o.debugView == 1   ? AaDebugView::EdgeMask
                : o.debugView == 2   ? AaDebugView::BlendWeight
                : o.debugView == 3   ? AaDebugView::Split
                                     : AaDebugView::None;
    return s;
}

} // namespace

int RunAaBench(const AaBenchOptions& o, const AdapterInfo& adapter) {
    std::string err;
    std::wstring werr;
    auto dev = GpuDevice::Create(&adapter.luid, werr);
    if (!dev) {
        printf("error: adapter '%s': %s\n", Narrow(adapter.name).c_str(), Narrow(werr).c_str());
        return 1;
    }
    printf("\n-- Adapter: %s (VEN_%04X DEV_%04X)\n", Narrow(adapter.name).c_str(), adapter.vendorId, adapter.deviceId);
    const AaSettings def = DefaultSettings(o);
    printf("Anti-aliasing: spatial tier, %s, strength %.2f, edge threshold %.2f, floor %.2f, text protection %s.\n",
           def.quality == AaQuality::Performance ? "Performance"
           : def.quality == AaQuality::Quality   ? "Quality" : "Balanced",
           static_cast<double>(def.strength), static_cast<double>(def.edgeThreshold),
           static_cast<double>(def.edgeThresholdMin), def.protectText ? "on" : "off");
    printf("Synthetic scenes %ux%u, hard (aliased) edges, one frame each. Measurements are of this filter only.\n\n",
           1280u, 720u);

    const uint32_t W = 1280, H = 720;
    std::vector<Scene> scenes;
    {
        for (float a : {20.0f, 30.0f, 45.0f, 60.0f, 70.0f}) {
            Scene s;
            s.name = "edge_" + std::to_string(static_cast<int>(a));
            s.kind = Kind::Edge;
            GenEdge(W, H, a, s.img);
            scenes.push_back(std::move(s));
        }
        for (float a : {0.0f, 90.0f}) {
            Scene s;
            s.name = "exact_" + std::to_string(static_cast<int>(a));
            s.kind = Kind::Exact;
            GenEdge(W, H, a, s.img);
            scenes.push_back(std::move(s));
        }
        Scene lines; lines.name = "thin_lines"; lines.kind = Kind::Lines; GenLines(W, H, lines.img); scenes.push_back(std::move(lines));
        Scene hud; hud.name = "text_hud"; hud.kind = Kind::Hud; GenTextHud(W, H, hud); scenes.push_back(std::move(hud));
        Scene tex; tex.name = "texture_2px"; tex.kind = Kind::Texture; GenTexture(W, H, tex.img); scenes.push_back(std::move(tex));
        Scene grad; grad.name = "gradient"; grad.kind = Kind::Gradient; GenGradient(W, H, grad.img); scenes.push_back(std::move(grad));
    }

    Harness h;
    if (!h.Init(*dev, W, H, DefaultSettings(o), err)) {
        printf("error: %s\n", err.c_str());
        return 1;
    }

    std::vector<SceneResult> results;
    int failures = 0;
    for (const Scene& s : scenes) {
        h.Upload(s.img);
        if (!h.Apply()) {
            printf("error: %s: Apply failed\n", s.name.c_str());
            ++failures;
            continue;
        }
        Image out;
        if (!h.Read(out)) {
            printf("error: %s: readback failed\n", s.name.c_str());
            ++failures;
            continue;
        }
        SceneResult r;
        r.name = s.name;
        r.maxDiff = MaxDifference(s.img, out);
        // The coverage detector reads back with a lag and is smoothed over frames; reset it so each scene is measured
        // on its own.
        h.pass->Reset();
        h.Run(40);
        r.coverage = h.pass->Stats().edgeCoverage;
        switch (s.kind) {
        case Kind::Edge:
            r.before = MeanMaxStep(s.img);
            r.after = MeanMaxStep(out);
            r.ratio = r.before > 0.0 ? r.after / r.before : 1.0;
            r.ok = r.ratio <= 0.80; // at least 20 % off the strongest step in a scanline
            break;
        case Kind::Exact:
            r.before = MeanMaxStep(s.img);
            r.after = MeanMaxStep(out);
            r.ratio = r.before > 0.0 ? r.after / r.before : 1.0;
            r.ok = r.ratio >= 0.98; // an edge the pixel grid already resolves must not be softened
            break;
        case Kind::Texture:
            r.before = HighPassEnergy(s.img);
            r.after = HighPassEnergy(out);
            r.ratio = r.before > 0.0 ? r.after / r.before : 1.0;
            r.ok = r.ratio >= 0.85 && r.ratio <= 1.15;
            break;
        case Kind::Lines: {
            // One-pixel line pixels are every 8th column/row; measure their mean luma.
            std::vector<uint8_t> mask(static_cast<size_t>(W) * H, 0);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    if (x % 8 == 0 || y % 8 == 0) mask[static_cast<size_t>(y) * W + x] = 1;
            r.before = MeanLuma(s.img, mask);
            r.after = MeanLuma(out, mask);
            r.ratio = r.before > 0.0 ? r.after / r.before : 1.0;
            r.ok = r.ratio >= 0.90; // strokes keep at least 90 % of their luma
            break;
        }
        case Kind::Hud: {
            const HudResult hr = HudDifference(s.img, out, s.hud);
            r.hudChangedPct = hr.total ? 100.0 * static_cast<double>(hr.changed) / static_cast<double>(hr.total) : 0.0;
            r.hudMaxDiff = hr.maxDiff;
            // Strokes are allowed to lose a little contrast (a one-texel feature is blended at half weight), but the
            // mean luma of the stroke pixels must stay close and only a small share of them may change at all. The
            // maximum is a sanity bound that catches a stroke being smeared into the background, not a 1-pixel limit:
            // a corner pixel is exactly the staircase the filter is meant to resolve.
            const double before = MeanLuma(s.img, s.hud);
            const double after = MeanLuma(out, s.hud);
            r.before = before;
            r.after = after;
            r.ratio = before > 0.0 ? after / before : 1.0;
            r.ok = r.hudChangedPct <= 5.0 && r.ratio >= 0.97 && r.hudMaxDiff <= 64;
            break;
        }
        case Kind::Gradient:
            r.ok = r.maxDiff <= 4;
            break;
        }
        if (!r.ok) ++failures;
        if (o.saveImages && !o.outDir.empty()) {
            std::error_code ec;
            fs::create_directories(o.outDir, ec);
            WritePng((o.outDir / (s.name + "_aa.png")).wstring(), out, err);
        }
        results.push_back(std::move(r));
    }

    // GPU time on a busy, texture-like picture at the two target resolutions (median of the pass's own timestamps).
    struct TimingRow { std::string size; double gpuMs = 0; float coverage = 0; };
    std::vector<TimingRow> timing;
    if (o.timing) {
        for (const auto& [tw, th] : {std::pair<uint32_t, uint32_t>{1920, 1080}, {2560, 1440}}) {
            Harness th2;
            if (!th2.Init(*dev, tw, th, DefaultSettings(o), err)) { printf("error: %s\n", err.c_str()); ++failures; continue; }
            Scene busy;
            GenTextHud(tw, th, busy);
            th2.Upload(busy.img);
            th2.Run(150);
            const AaStats st = th2.pass->Stats();
            TimingRow row;
            row.size = std::to_string(tw) + "x" + std::to_string(th);
            row.gpuMs = st.gpuMs;
            row.coverage = st.edgeCoverage;
            timing.push_back(row);
        }
    }

    printf("%-14s %10s %10s %8s %8s %10s %8s %s\n", "scene", "before", "after", "ratio", "maxdiff", "hud_chg%",
           "coverage", "check");
    for (const SceneResult& r : results) {
        printf("%-14s %10.3f %10.3f %8.3f %8d %10.2f %7.1f%% %s\n", r.name.c_str(), r.before, r.after, r.ratio,
               r.maxDiff, r.hudChangedPct, static_cast<double>(r.coverage) * 100.0, r.ok ? "pass" : "FAIL");
    }
    printf("\nChecks: aliased edges (20-70 deg) reduce the largest step in a scanline by >= 20 %%; exact edges (0, 90 deg)\n"
           "change by < 2 %%; texture energy stays within +-15 %%; one-pixel lines keep >= 90 %% of their luma; text/HUD\n"
           "strokes keep >= 97 %% of their mean luma, change in <= 5 %% of their pixels and never move by > 64/255;\n"
           "gradient max change <= 4/255.\n");
    if (!timing.empty()) {
        printf("\nGPU time (spatial pass + statistics dispatch, timestamp median, otherwise idle GPU):\n");
        for (const TimingRow& t : timing)
            printf("  %-10s %7.3f ms   (edge coverage %.1f %%)\n", t.size.c_str(), t.gpuMs,
                   static_cast<double>(t.coverage) * 100.0);
        printf("  This is the isolated pass, not in-game cost: it excludes capture, present and contention.\n");
    }
    printf("\n%s (%d failing check%s)\n", failures ? "FAILED" : "all checks passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

int RunAaImage(const AaBenchOptions& o, const AdapterInfo& adapter) {
    std::string err;
    std::wstring werr;
    auto dev = GpuDevice::Create(&adapter.luid, werr);
    if (!dev) {
        printf("error: adapter '%s': %s\n", Narrow(adapter.name).c_str(), Narrow(werr).c_str());
        return 1;
    }
    int failures = 0;
    for (const fs::path& path : o.images) {
        Image src;
        if (!ReadPng(path.wstring(), src, err)) {
            printf("error: %s: %s\n", path.string().c_str(), err.c_str());
            ++failures;
            continue;
        }
        Harness h;
        if (!h.Init(*dev, src.w, src.h, DefaultSettings(o), err)) {
            printf("error: %s\n", err.c_str());
            ++failures;
            continue;
        }
        h.Upload(src);
        Image out;
        if (!h.Apply()) { printf("error: %s: Apply failed\n", path.string().c_str()); ++failures; continue; }
        if (!h.Read(out)) { printf("error: %s: readback failed\n", path.string().c_str()); ++failures; continue; }
        h.Run(120);
        const AaStats st = h.pass->Stats();
        const double before = HighPassEnergy(src);
        const double after = HighPassEnergy(out);
        const double ratio = before > 0.0 ? after / before : 1.0;
        const PixelChangeStats cs = PixelChange(src, out);
        const AaSettings def = DefaultSettings(o);
        printf("%s: %ux%u, %s strength %.2f, coverage %.1f %%, changed %.2f %% of pixels (mean |d| %.2f), "
               "high-pass energy %.2f -> %.2f (%.3f), max change %d, GPU %.3f ms\n",
               path.filename().string().c_str(), src.w, src.h,
               def.quality == AaQuality::Performance ? "Performance" : def.quality == AaQuality::Quality ? "Quality" : "Balanced",
               static_cast<double>(def.strength), static_cast<double>(st.edgeCoverage) * 100.0, cs.changedPct, cs.meanAbs,
               before, after, ratio, cs.maxDiff, st.gpuMs);
        if (o.saveImages && !o.outDir.empty()) {
            std::error_code ec;
            fs::create_directories(o.outDir, ec);
            const fs::path dst = o.outDir / (path.stem().string() + "_aa.png");
            if (WritePng(dst.wstring(), out, err)) printf("  wrote %s\n", dst.string().c_str());
            else printf("  warning: %s\n", err.c_str());
        }
    }
    return failures ? 1 : 0;
}

} // namespace bench
