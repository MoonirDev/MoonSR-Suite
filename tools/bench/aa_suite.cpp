// Thorough anti-aliasing benchmark: supersampled ground truth, gate checks, settings sweep and owner review pack.
// Design: documentation/research/aa_benchmark.md.
#include "aa_suite.h"

#include "aa/aa_pass.h"
#include "gpu/gpu_device.h"
#include "imageops.h"
#include "metrics.h"
#include "report.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace bench {
namespace {

using Microsoft::WRL::ComPtr;
using namespace moonsr;
namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;
constexpr double kBudget1080Ms = 0.30;
constexpr double kBudget1440Ms = 0.60;

struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb kDark{34, 34, 34};
constexpr Rgb kLight{218, 218, 218};
constexpr Rgb kRed{215, 45, 45};
constexpr Rgb kBlue{45, 65, 215};

void Put(Image& im, uint32_t x, uint32_t y, Rgb c) {
    uint8_t* p = im.Row(y) + static_cast<size_t>(x) * 4;
    p[0] = c.b;
    p[1] = c.g;
    p[2] = c.r;
    p[3] = 255;
}

float Rnd(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x9e3779b9u ^ y * 0x85ebca6bu ^ seed;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}

void FillRectHi(Image& hi, int ss, double x0, double y0, double x1, double y1, Rgb c) {
    const int hx0 = std::max(0, static_cast<int>(std::floor(x0 * ss)));
    const int hy0 = std::max(0, static_cast<int>(std::floor(y0 * ss)));
    const int hx1 = std::min(static_cast<int>(hi.w), static_cast<int>(std::ceil(x1 * ss)));
    const int hy1 = std::min(static_cast<int>(hi.h), static_cast<int>(std::ceil(y1 * ss)));
    for (int y = hy0; y < hy1; ++y)
        for (int x = hx0; x < hx1; ++x) {
            const double tx = (x + 0.5) / ss, ty = (y + 0.5) / ss;
            if (tx >= x0 && tx < x1 && ty >= y0 && ty < y1) Put(hi, static_cast<uint32_t>(x), static_cast<uint32_t>(y), c);
        }
}

Image GenEdgeHi(uint32_t w, uint32_t h, int ss, float degrees, Rgb a, Rgb b) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    const float ang = degrees * static_cast<float>(kPi) / 180.0f;
    const float nx = std::cos(ang), ny = std::sin(ang);
    const float cx = static_cast<float>(w) * 0.5f, cy = static_cast<float>(h) * 0.5f;
    for (uint32_t y = 0; y < hi.h; ++y) {
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(ss);
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            const float d = (tx - cx) * nx + (ty - cy) * ny;
            Put(hi, x, y, d < 0.0f ? a : b);
        }
    }
    return hi;
}

Image GenColorHi(uint32_t w, uint32_t h, int ss) { return GenEdgeHi(w, h, ss, 30.0f, kRed, kBlue); }

Image GenGradientHi(uint32_t w, uint32_t h, int ss) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    for (uint32_t y = 0; y < hi.h; ++y) {
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(ss);
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            const float t = 0.5f * tx / static_cast<float>(w) + 0.5f * ty / static_cast<float>(h);
            const float v = 24.0f + 208.0f * (t * t * (3.0f - 2.0f * t));
            const uint8_t c = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
            Put(hi, x, y, {c, c, c});
        }
    }
    return hi;
}

Image GenCheckerHi(uint32_t w, uint32_t h, int ss, int block, uint32_t seed) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    for (uint32_t y = 0; y < hi.h; ++y) {
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(ss);
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            const bool c = (static_cast<int>(std::floor(tx / block)) + static_cast<int>(std::floor(ty / block))) & 1;
            const float n = (Rnd(x, y, seed) - 0.5f) * 30.0f;
            const float v = std::clamp((c ? 196.0f : 54.0f) + n, 0.0f, 255.0f);
            const uint8_t b = static_cast<uint8_t>(v);
            Put(hi, x, y, {b, b, b});
        }
    }
    return hi;
}

Image GenThinHi(uint32_t w, uint32_t h, int ss) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    for (uint32_t y = 0; y < hi.h; ++y) {
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(ss);
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            const auto on = [](float t) {
                const float local = t - 0.25f;
                const float k = std::floor(local / 8.0f) * 8.0f;
                return local - k >= 0.0f && local - k < 1.5f;
            };
            Put(hi, x, y, (on(tx) || on(ty)) ? Rgb{225, 225, 225} : Rgb{28, 28, 28});
        }
    }
    return hi;
}

Image GenRooflineHi(uint32_t w, uint32_t h, int ss) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    for (uint32_t y = 0; y < hi.h; ++y) {
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(ss);
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            const float edge = static_cast<float>(h) * 0.18f + tx * 0.08f;
            const bool band = ty >= edge && ty < edge + 3.0f;
            Put(hi, x, y, band ? Rgb{205, 205, 205} : Rgb{38, 38, 38});
        }
    }
    return hi;
}

Image GenSubpixelHi(uint32_t w, uint32_t h, int ss) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    for (uint32_t y = 0; y < hi.h; ++y) {
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            bool on = false;
            for (double base : {120.25, 320.50, 520.75, 720.25, 920.50})
                if (tx >= base && tx < base + 2.0) on = true;
            Put(hi, x, y, on ? Rgb{220, 220, 220} : Rgb{30, 30, 30});
        }
    }
    return hi;
}

Image GenTextHi(uint32_t w, uint32_t h, int ss, std::vector<uint8_t>& mask) {
    Image hi;
    hi.Alloc(w * static_cast<uint32_t>(ss), h * static_cast<uint32_t>(ss));
    for (uint32_t y = 0; y < hi.h; ++y) {
        const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(ss);
        for (uint32_t x = 0; x < hi.w; ++x) {
            const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(ss);
            const bool c = (static_cast<int>(std::floor(tx / 2.0f)) + static_cast<int>(std::floor(ty / 2.0f))) & 1;
            const float n = (Rnd(x, y, 0x9e3779b9u) - 0.5f) * 26.0f;
            const float v = std::clamp((c ? 150.0f : 70.0f) + n, 0.0f, 255.0f);
            const uint8_t b = static_cast<uint8_t>(v);
            Put(hi, x, y, {b, b, b});
        }
    }
    mask.assign(static_cast<size_t>(w) * h, 0);
    for (int row = 0; row < 3; ++row) {
        const double y0 = 80.0 + row * 46.0;
        for (int g = 0; g < 12; ++g) {
            const double x0 = 70.0 + g * 90.0;
            const double x1 = x0 + 1.5;
            FillRectHi(hi, ss, x0, y0, x1, y0 + 9.0, Rgb{245, 245, 245});
            FillRectHi(hi, ss, x1, y0, x0 + 5.5, y0 + 1.5, Rgb{245, 245, 245});
            FillRectHi(hi, ss, x1, y0 + 7.5, x0 + 5.5, y0 + 9.0, Rgb{245, 245, 245});
            for (uint32_t y = 0; y < h; ++y) {
                const double cy = y + 0.5;
                for (uint32_t x = 0; x < w; ++x) {
                    const double cx = x + 0.5;
                    const bool inStem = cx >= x0 && cx < x1 && cy >= y0 && cy < y0 + 9.0;
                    const bool inArm = cx >= x1 && cx < x0 + 5.5 &&
                                       ((cy >= y0 && cy < y0 + 1.5) || (cy >= y0 + 7.5 && cy < y0 + 9.0));
                    if (inStem || inArm) mask[static_cast<size_t>(y) * w + x] = 1;
                }
            }
        }
    }
    return hi;
}

Image GenBusy(uint32_t w, uint32_t h) {
    Image im;
    im.Alloc(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const bool c = ((x / 2) + (y / 2)) & 1;
            const float n = (Rnd(x, y, 0x51ed270bu) - 0.5f) * 34.0f;
            const float v = std::clamp((c ? 190.0f : 55.0f) + n, 0.0f, 255.0f);
            const uint8_t b = static_cast<uint8_t>(v);
            Put(im, x, y, {b, b, b});
        }
    }
    return im;
}

enum class Kind { Slanted, Roofline, Color, Exact, Text, Thin, Texture, Gradient, Subpixel };

struct Scene {
    std::string name;
    Kind kind = Kind::Slanted;
    Image in, gt;
    std::vector<uint8_t> mask;
    uint32_t cropX = 0, cropY = 0, cropW = 200, cropH = 200;
};

Image PointSample(const Image& hi, uint32_t w, uint32_t h, int ss) {
    Image im;
    im.Alloc(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        const int sy = std::clamp(static_cast<int>(std::floor((y + 0.5) * ss)), 0, static_cast<int>(hi.h) - 1);
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = std::clamp(static_cast<int>(std::floor((x + 0.5) * ss)), 0, static_cast<int>(hi.w) - 1);
            std::memcpy(im.Row(y) + static_cast<size_t>(x) * 4, hi.Row(static_cast<uint32_t>(sy)) + static_cast<size_t>(sx) * 4, 4);
        }
    }
    return im;
}

Scene MakeScene(std::string name, Kind kind, const Image& hi, uint32_t w, uint32_t h, int ss,
                std::vector<uint8_t> mask = {}) {
    Scene s;
    s.name = std::move(name);
    s.kind = kind;
    s.gt = AreaDownsample(hi, w, h);
    s.in = PointSample(hi, w, h, ss);
    s.mask = std::move(mask);
    s.cropX = (w - s.cropW) / 2;
    s.cropY = (h - s.cropH) / 2;
    if (kind == Kind::Text) {
        s.cropX = 60;
        s.cropY = 70;
        s.cropW = 300;
        s.cropH = 150;
    } else if (kind == Kind::Color) {
        s.cropX = w / 2 - 100;
        s.cropY = h / 2 - 100;
    }
    return s;
}

std::vector<Scene> MakeScenes(uint32_t w, uint32_t h, int ss) {
    std::vector<Scene> scenes;
    for (float a : {20.0f, 45.0f, 70.0f}) {
        const std::string n = "edge_" + std::to_string(static_cast<int>(a));
        scenes.push_back(MakeScene(n, Kind::Slanted, GenEdgeHi(w, h, ss, a, kDark, kLight), w, h, ss));
    }
    scenes.push_back(MakeScene("roofline_8", Kind::Roofline, GenRooflineHi(w, h, ss), w, h, ss));
    scenes.push_back(MakeScene("color_30", Kind::Color, GenColorHi(w, h, ss), w, h, ss));
    scenes.push_back(MakeScene("exact_0", Kind::Exact, GenEdgeHi(w, h, ss, 0.0f, kDark, kLight), w, h, ss));
    scenes.push_back(MakeScene("subpixel_edges", Kind::Subpixel, GenSubpixelHi(w, h, ss), w, h, ss));
    {
        std::vector<uint8_t> mask;
        Image hi = GenTextHi(w, h, ss, mask);
        scenes.push_back(MakeScene("text_hud", Kind::Text, hi, w, h, ss, std::move(mask)));
    }
    {
        Image hi = GenThinHi(w, h, ss);
        std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const float tx = static_cast<float>(x) + 0.5f, ty = static_cast<float>(y) + 0.5f;
                const auto on = [](float t) {
                    const float local = t - 0.25f;
                    const float k = std::floor(local / 8.0f) * 8.0f;
                    return local - k >= 0.0f && local - k < 1.5f;
                };
                if (on(tx) || on(ty)) mask[static_cast<size_t>(y) * w + x] = 1;
            }
        scenes.push_back(MakeScene("thin_lines", Kind::Thin, hi, w, h, ss, std::move(mask)));
    }
    scenes.push_back(MakeScene("checker_4px", Kind::Texture, GenCheckerHi(w, h, ss, 4, 0x12345678u), w, h, ss));
    scenes.push_back(MakeScene("gradient", Kind::Gradient, GenGradientHi(w, h, ss), w, h, ss));
    return scenes;
}

// ---------------------------------------------------------------- metrics
double LumaAt(const Image& im, int x, int y) {
    const uint8_t* p = im.Row(static_cast<uint32_t>(y)) + static_cast<size_t>(x) * 4;
    return 0.299 * p[2] + 0.587 * p[1] + 0.114 * p[0];
}

double MeanMaxStep(const Image& im) {
    double sx = 0.0, sy = 0.0;
    for (uint32_t y = 1; y + 1 < im.h; ++y) {
        float m = 0.0f;
        for (uint32_t x = 1; x + 2 < im.w; ++x)
            m = std::max(m, static_cast<float>(std::abs(LumaAt(im, static_cast<int>(x) + 1, static_cast<int>(y)) -
                                                         LumaAt(im, static_cast<int>(x), static_cast<int>(y)))));
        sx += m;
    }
    for (uint32_t x = 1; x + 1 < im.w; ++x) {
        float m = 0.0f;
        for (uint32_t y = 1; y + 2 < im.h; ++y)
            m = std::max(m, static_cast<float>(std::abs(LumaAt(im, static_cast<int>(x), static_cast<int>(y) + 1) -
                                                         LumaAt(im, static_cast<int>(x), static_cast<int>(y)))));
        sy += m;
    }
    return 0.5 * (sx / static_cast<double>(im.h - 2) + sy / static_cast<double>(im.w - 2));
}

double HighPassEnergy(const Image& im) {
    double sum = 0.0;
    size_t n = 0;
    for (uint32_t y = 1; y + 1 < im.h; ++y)
        for (uint32_t x = 1; x + 1 < im.w; ++x) {
            const double c = LumaAt(im, static_cast<int>(x), static_cast<int>(y));
            const double lap = 4.0 * c - LumaAt(im, static_cast<int>(x) + 1, static_cast<int>(y)) -
                               LumaAt(im, static_cast<int>(x) - 1, static_cast<int>(y)) -
                               LumaAt(im, static_cast<int>(x), static_cast<int>(y) + 1) -
                               LumaAt(im, static_cast<int>(x), static_cast<int>(y) - 1);
            sum += lap * lap;
            ++n;
        }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

double LumaStdDev(const Image& im) {
    double s = 0.0, s2 = 0.0;
    const size_t n = static_cast<size_t>(im.w) * im.h;
    for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x) {
            const double v = LumaAt(im, static_cast<int>(x), static_cast<int>(y));
            s += v;
            s2 += v * v;
        }
    const double mean = s / static_cast<double>(n);
    const double var = s2 / static_cast<double>(n) - mean * mean;
    return var > 0.0 ? std::sqrt(var) : 0.0;
}

struct MaskStats {
    size_t changed = 0, total = 0;
    int maxDiff = 0;
    double meanBefore = 0.0, meanAfter = 0.0;
};

MaskStats MaskDifference(const Image& a, const Image& b, const std::vector<uint8_t>& mask) {
    MaskStats r;
    double sumA = 0.0, sumB = 0.0;
    for (uint32_t y = 0; y < a.h; ++y)
        for (uint32_t x = 0; x < a.w; ++x) {
            if (mask.empty() || !mask[static_cast<size_t>(y) * a.w + x]) continue;
            ++r.total;
            const uint8_t* pa = a.Row(y) + static_cast<size_t>(x) * 4;
            const uint8_t* pb = b.Row(y) + static_cast<size_t>(x) * 4;
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(static_cast<int>(pa[c]) - static_cast<int>(pb[c])));
            r.maxDiff = std::max(r.maxDiff, d);
            if (d > 0) ++r.changed;
            sumA += 0.299 * pa[2] + 0.587 * pa[1] + 0.114 * pa[0];
            sumB += 0.299 * pb[2] + 0.587 * pb[1] + 0.114 * pb[0];
        }
    if (r.total) {
        r.meanBefore = sumA / static_cast<double>(r.total);
        r.meanAfter = sumB / static_cast<double>(r.total);
    }
    return r;
}

int MaxDifference(const Image& a, const Image& b) {
    int d = 0;
    const size_t n = a.px.size() / 4;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c)
            d = std::max(d, std::abs(static_cast<int>(a.px[i * 4 + c]) - static_cast<int>(b.px[i * 4 + c])));
    return d;
}

double MeanSaturation(const Image& im) {
    double sum = 0.0;
    const size_t n = static_cast<size_t>(im.w) * im.h;
    for (size_t i = 0; i < n; ++i) {
        const int b = im.px[i * 4 + 0], g = im.px[i * 4 + 1], r = im.px[i * 4 + 2];
        const int mx = std::max({b, g, r}), mn = std::min({b, g, r});
        sum += mx > 0 ? static_cast<double>(mx - mn) / mx : 0.0;
    }
    return sum / static_cast<double>(n);
}

// ---------------------------------------------------------------- GPU harness
struct Harness {
    GpuDevice* dev = nullptr;
    std::unique_ptr<AaPass> pass;
    ComPtr<ID3D11Texture2D> inTex, outTex, staging;
    ComPtr<ID3D11ShaderResourceView> inSrv;
    ComPtr<ID3D11RenderTargetView> outRtv;
    uint32_t w = 0, h = 0;

    bool Init(GpuDevice& d, uint32_t width, uint32_t height, std::string& err) {
        dev = &d;
        w = width;
        h = height;
        std::wstring e;
        pass = AaPass::Create(d, e);
        if (!pass) {
            err = "AaPass::Create: " + Narrow(e);
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = d.Device()->CreateTexture2D(&td, nullptr, &inTex);
        if (SUCCEEDED(hr)) hr = d.Device()->CreateShaderResourceView(inTex.Get(), nullptr, &inSrv);
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = d.Device()->CreateTexture2D(&td, nullptr, &outTex);
        if (SUCCEEDED(hr)) hr = d.Device()->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = d.Device()->CreateTexture2D(&td, nullptr, &staging);
        if (FAILED(hr)) {
            err = "suite texture creation failed";
            return false;
        }
        return true;
    }
    void Upload(const Image& im) {
        dev->Context()->UpdateSubresource(inTex.Get(), 0, nullptr, im.px.data(), im.w * 4, 0);
    }
    bool Apply(const AaSettings& s) {
        pass->SetSettings(s);
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
    void Run(int frames, const AaSettings& s) {
        for (int i = 0; i < frames; ++i) {
            if (!Apply(s)) return;
        }
        Image sink;
        Read(sink);
        Apply(s);
        Read(sink);
    }
    AaSettings settings_{};
};

struct Candidate {
    int quality = 1;
    float strength = 1.0f;
    float threshold = 0.10f;
    float floor = 0.03f;
    bool protect = true;

    AaSettings Settings() const {
        AaSettings s;
        s.mode = AaMode::Spatial;
        s.quality = quality == 0 ? AaQuality::Performance : quality == 2 ? AaQuality::Quality : AaQuality::Balanced;
        s.strength = strength;
        s.edgeThreshold = threshold;
        s.edgeThresholdMin = floor;
        s.protectText = protect;
        s.protectHud = protect;
        s.debugView = AaDebugView::None;
        return s;
    }
    std::string Label() const {
        char b[96];
        snprintf(b, sizeof(b), "q%d_s%.2f_t%.2f_f%.2f%s", quality, static_cast<double>(strength),
                 static_cast<double>(threshold), static_cast<double>(floor), protect ? "" : "_noprotect");
        return b;
    }
    bool IsDefault() const {
        return quality == 1 && std::abs(strength - 1.0f) < 1e-6f && std::abs(threshold - 0.10f) < 1e-6f &&
               std::abs(floor - 0.03f) < 1e-6f && protect;
    }
};

struct SceneMetrics {
    std::string scene;
    Kind kind = Kind::Slanted;
    double inPsnr = 0.0, outPsnr = 0.0, inSsim = 0.0, outSsim = 0.0;
    double dPsnr = 0.0, dSsim = 0.0;
    double stepIn = 0.0, stepOut = 0.0, stepRatio = 1.0;
    double hpIn = 0.0, hpOut = 0.0, hpRatio = 1.0;
    double sdIn = 0.0, sdOut = 0.0, sdRatio = 1.0;
    double textChanged = 0.0, textMeanRatio = 1.0;
    int textMax = 0;
    int maxDiff = 0;
    double satIn = 0.0, satOut = 0.0, satGt = 0.0, satPenalty = 0.0;
    bool pass = true;
    std::string fail;
};

const char* KindName(Kind k) {
    switch (k) {
    case Kind::Slanted: return "slanted";
    case Kind::Roofline: return "roofline";
    case Kind::Color: return "color";
    case Kind::Exact: return "exact";
    case Kind::Text: return "text";
    case Kind::Thin: return "thin";
    case Kind::Texture: return "texture";
    case Kind::Gradient: return "gradient";
    default: return "subpixel";
    }
}

SceneMetrics MeasureScene(const Scene& s, const Image& out) {
    SceneMetrics m;
    m.scene = s.name;
    m.kind = s.kind;
    const Metrics inGt = Compare(s.gt, s.in);
    const Metrics outGt = Compare(s.gt, out);
    m.inPsnr = inGt.psnrRgb;
    m.outPsnr = outGt.psnrRgb;
    m.inSsim = inGt.ssim;
    m.outSsim = outGt.ssim;
    m.dPsnr = m.outPsnr - m.inPsnr;
    m.dSsim = m.outSsim - m.inSsim;
    m.stepIn = MeanMaxStep(s.in);
    m.stepOut = MeanMaxStep(out);
    m.stepRatio = m.stepIn > 0.0 ? m.stepOut / m.stepIn : 1.0;
    m.hpIn = HighPassEnergy(s.in);
    m.hpOut = HighPassEnergy(out);
    m.hpRatio = m.hpIn > 0.0 ? m.hpOut / m.hpIn : 1.0;
    m.sdIn = LumaStdDev(s.in);
    m.sdOut = LumaStdDev(out);
    m.sdRatio = m.sdIn > 0.0 ? m.sdOut / m.sdIn : 1.0;
    m.maxDiff = MaxDifference(s.in, out);

    auto failWith = [&](std::string why) {
        m.pass = false;
        if (!m.fail.empty()) m.fail += "; ";
        m.fail += std::move(why);
    };

    switch (s.kind) {
    case Kind::Slanted:
        if (m.stepRatio > 0.80) failWith("step ratio " + std::to_string(m.stepRatio));
        break;
    case Kind::Roofline:
        // A 4.6-degree band has ~12-16 texel runs; the endpoint model is held to a 10 % step reduction, not 20 %.
        if (m.stepRatio > 0.90) failWith("roofline step " + std::to_string(m.stepRatio));
        break;
    case Kind::Subpixel:
        // Straight vertical bars at fractional positions are not a staircase; the endpoint model cannot move them.
        // Kept as a diagnostic and as a GT term, with no step gate.
        break;
    case Kind::Exact:
        if (m.stepRatio < 0.98) failWith("exact step " + std::to_string(m.stepRatio));
        if (m.maxDiff > 32) failWith("exact max " + std::to_string(m.maxDiff));
        break;
    case Kind::Text: {
        const MaskStats ms = MaskDifference(s.in, out, s.mask);
        m.textChanged = ms.total ? 100.0 * static_cast<double>(ms.changed) / static_cast<double>(ms.total) : 0.0;
        m.textMeanRatio = ms.meanBefore > 0.0 ? ms.meanAfter / ms.meanBefore : 1.0;
        m.textMax = ms.maxDiff;
        // The suite's glyph is 1.5 px wide, harder than the fast mode's one-texel stress scene (which stays at 0 %).
        // The 10 % bound is the measured tier-1 trade-off; the real ATLYSS HUD is 11-22 % and is a T2 item.
        if (m.textChanged > 10.0) failWith("text changed " + std::to_string(m.textChanged));
        if (m.textMeanRatio < 0.98) failWith("text mean " + std::to_string(m.textMeanRatio));
        if (m.textMax > 80) failWith("text max " + std::to_string(m.textMax));
        break;
    }
    case Kind::Thin: {
        const MaskStats ms = MaskDifference(s.in, out, s.mask);
        m.textChanged = ms.total ? 100.0 * static_cast<double>(ms.changed) / static_cast<double>(ms.total) : 0.0;
        m.textMeanRatio = ms.meanBefore > 0.0 ? ms.meanAfter / ms.meanBefore : 1.0;
        m.textMax = ms.maxDiff;
        if (m.textMeanRatio < 0.90) failWith("thin mean " + std::to_string(m.textMeanRatio));
        break;
    }
    case Kind::Texture:
        if (m.hpRatio < 0.90) failWith("texture high-pass " + std::to_string(m.hpRatio));
        if (m.sdRatio < 0.95) failWith("texture stddev " + std::to_string(m.sdRatio));
        break;
    case Kind::Gradient:
        if (m.maxDiff > 4) failWith("gradient max " + std::to_string(m.maxDiff));
        break;
    case Kind::Color:
        m.satIn = MeanSaturation(s.in);
        m.satOut = MeanSaturation(out);
        m.satGt = MeanSaturation(s.gt);
        m.satPenalty = std::abs(m.satOut - m.satGt) - std::abs(m.satIn - m.satGt);
        if (m.stepRatio > 0.80) failWith("colour step " + std::to_string(m.stepRatio));
        if (m.satPenalty > 0.01) failWith("saturation " + std::to_string(m.satPenalty));
        break;
    }
    return m;
}

struct CandidateResult {
    Candidate candidate;
    std::vector<SceneMetrics> scenes;
    bool pass = true;
    std::string fail;
    double score = 0.0;
    double meanDp = 0.0, meanDs = 0.0, meanStep = 0.0;
    double ms1080 = 0.0, ms1440 = 0.0;
};

void Aggregate(CandidateResult& r) {
    for (const SceneMetrics& m : r.scenes) {
        if (!m.pass) {
            r.pass = false;
            if (!r.fail.empty()) r.fail += "; ";
            r.fail += m.scene + ": " + m.fail;
        }
    }
    std::vector<double> dp, ds, st;
    for (const SceneMetrics& m : r.scenes) {
        if (m.kind == Kind::Slanted || m.kind == Kind::Roofline || m.kind == Kind::Color || m.kind == Kind::Subpixel) {
            dp.push_back(m.dPsnr);
            ds.push_back(m.dSsim);
            st.push_back(1.0 - m.stepRatio);
        }
    }
    auto mean = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    };
    r.meanDp = mean(dp);
    r.meanDs = mean(ds);
    r.meanStep = mean(st);
    r.score = 100.0 * r.meanStep + 10.0 * r.meanDp + 1000.0 * r.meanDs;
}

std::vector<Candidate> BuildCandidates(const AaSuiteOptions& o) {
    std::vector<Candidate> list;
    Candidate def;
    list.push_back(def);
    Candidate baseline;
    baseline.strength = 0.0f;
    list.push_back(baseline);
    if (o.sweep) {
        for (int q : o.qualities)
            for (float s : o.strengths)
                for (float t : o.thresholds)
                    for (float f : o.floors) {
                        Candidate c;
                        c.quality = std::clamp(q, 0, 2);
                        c.strength = std::clamp(s, 0.0f, 1.0f);
                        c.threshold = std::clamp(t, 0.02f, 0.5f);
                        c.floor = std::clamp(f, 0.0f, 0.2f);
                        c.protect = o.protect;
                        list.push_back(c);
                    }
    }
    std::vector<Candidate> unique;
    for (const Candidate& c : list) {
        const std::string label = c.Label();
        bool seen = false;
        for (const Candidate& u : unique) seen = seen || u.Label() == label;
        if (!seen) unique.push_back(c);
    }
    return unique;
}

// ---------------------------------------------------------------- image output
Image CropZoom(const Image& src, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int zoom) {
    Image out;
    out.Alloc(w * static_cast<uint32_t>(zoom), h * static_cast<uint32_t>(zoom));
    for (uint32_t dy = 0; dy < out.h; ++dy) {
        const uint32_t sy = std::min(src.h - 1, y + dy / static_cast<uint32_t>(zoom));
        for (uint32_t dx = 0; dx < out.w; ++dx) {
            const uint32_t sx = std::min(src.w - 1, x + dx / static_cast<uint32_t>(zoom));
            std::memcpy(out.Row(dy) + static_cast<size_t>(dx) * 4, src.Row(sy) + static_cast<size_t>(sx) * 4, 4);
        }
    }
    return out;
}

Image DiffAmplified(const Image& a, const Image& b, double gain) {
    Image out;
    out.Alloc(a.w, a.h);
    for (size_t i = 0; i < a.px.size() / 4; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(static_cast<int>(a.px[i * 4 + c]) - static_cast<int>(b.px[i * 4 + c]));
            out.px[i * 4 + c] = static_cast<uint8_t>(std::min(255, static_cast<int>(d * gain)));
        }
        out.px[i * 4 + 3] = 255;
    }
    return out;
}

bool Save(const fs::path& path, const Image& img, std::string& err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return WritePng(path.wstring(), img, err);
}

const char* KindLabel(Kind k) { return KindName(k); }

// ---------------------------------------------------------------- report
struct RealResult {
    std::string name;
    double changed = 0.0, mean = 0.0, hpRatio = 1.0;
    int maxDiff = 0;
    double ms = 0.0;
};

void WriteJsonMeta(std::ofstream& os, const AaSuiteOptions& o, const moonsr::AdapterInfo& a, const std::string& commit) {
    os << "{\n  \"commit\": \"" << commit << "\",\n  \"adapter\": \"" << Narrow(a.name) << "\",\n  \"width\": " << o.width
       << ",\n  \"height\": " << o.height << ",\n  \"supersample\": " << o.supersample << "\n}\n";
}

void WriteReport(const fs::path& dir, const AaSuiteOptions& o, const moonsr::AdapterInfo& adapter,
                 const std::vector<Scene>& scenes, const std::vector<CandidateResult>& results,
                 const CandidateResult& top, const CandidateResult* def, GpuDevice& dev,
                 const std::vector<RealResult>& real, const std::string& commit, const std::string& date) {
    std::error_code ec;
    fs::create_directories(dir / "images", ec);

    const bool haveDefault = def && def->candidate.Label() != top.candidate.Label();
    // Save the scene inputs/GT once.
    std::string err;
    for (const Scene& s : scenes) {
        Save(dir / "images" / (s.name + "_in.png"), s.in, err);
        Save(dir / "images" / (s.name + "_gt.png"), s.gt, err);
        const int zoom = s.cropW > 250 ? 3 : 4;
        Save(dir / "images" / (s.name + "_in_crop.png"),
             CropZoom(s.in, s.cropX, s.cropY, s.cropW, s.cropH, zoom), err);
        Save(dir / "images" / (s.name + "_gt_crop.png"),
             CropZoom(s.gt, s.cropX, s.cropY, s.cropW, s.cropH, zoom), err);
    }

    // Re-run the top/default candidates for the saved outputs.
    Harness h;
    if (!h.Init(dev, o.width, o.height, err)) {
        printf("error: report harness: %s\n", err.c_str());
        return;
    }
    auto renderScene = [&](const Candidate& c, const char* suffix) {
        for (const Scene& s : scenes) {
            h.Upload(s.in);
            Image out;
            if (!h.Apply(c.Settings()) || !h.Read(out)) continue;
            Save(dir / "images" / (s.name + "_" + suffix + ".png"), out, err);
            const int zoom = s.cropW > 250 ? 3 : 4;
            Save(dir / "images" / (s.name + "_" + suffix + "_crop.png"),
                 CropZoom(out, s.cropX, s.cropY, s.cropW, s.cropH, zoom), err);
        }
    };
    renderScene(top.candidate, "top");
    if (haveDefault) renderScene(def->candidate, "default");

    std::ofstream csv(dir / "candidates.csv");
    csv << "candidate,quality,strength,threshold,floor,protect,eligible,score,mean_dpsnr,mean_dssim,mean_step,"
           "ms1080,ms1440,fail\n";
    csv << std::fixed << std::setprecision(4);
    for (const CandidateResult& r : results) {
        csv << r.candidate.Label() << ',' << r.candidate.quality << ',' << r.candidate.strength << ','
            << r.candidate.threshold << ',' << r.candidate.floor << ',' << (r.candidate.protect ? 1 : 0) << ','
            << (r.pass ? 1 : 0) << ',' << r.score << ',' << r.meanDp << ',' << r.meanDs << ',' << r.meanStep << ','
            << r.ms1080 << ',' << r.ms1440 << ",\"" << r.fail << "\"\n";
    }
    std::ofstream scenesCsv(dir / "scenes.csv");
    scenesCsv << "candidate,scene,kind,dpsnr,dssim,step_ratio,hp_ratio,sd_ratio,text_changed,text_mean,sat_penalty,"
                 "maxdiff,eligible,fail\n";
    scenesCsv << std::fixed << std::setprecision(4);
    for (const CandidateResult& r : results)
        for (const SceneMetrics& m : r.scenes) {
            scenesCsv << r.candidate.Label() << ',' << m.scene << ',' << KindLabel(m.kind) << ',' << m.dPsnr << ','
                      << m.dSsim << ',' << m.stepRatio << ',' << m.hpRatio << ',' << m.sdRatio << ',' << m.textChanged
                      << ',' << m.textMeanRatio << ',' << m.satPenalty << ',' << m.maxDiff << ',' << (m.pass ? 1 : 0)
                      << ",\"" << m.fail << "\"\n";
        }

    std::ofstream html(dir / "index.html");
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>MoonSR AA suite</title><style>"
            "body{font:14px Segoe UI,Arial,sans-serif;background:#111;color:#ddd;margin:24px}"
            "h1,h2{color:#fff}table{border-collapse:collapse;margin:12px 0}"
            "td,th{border:1px solid #444;padding:3px 6px;vertical-align:top;font-size:12px}"
            "th{background:#222;position:sticky;top:0}img{image-rendering:pixelated;background:#000}"
            ".ok{color:#7dff9b}.bad{color:#ff7d7d}.muted{color:#888}.small{font-size:11px}"
            "</style></head><body>\n";
    html << "<h1>MoonSR anti-aliasing suite</h1>\n";
    html << "<p class=\"muted\">Commit: " << commit << " | " << date << " | adapter: " << Narrow(adapter.name)
         << " | suite " << o.width << "x" << o.height << " | supersample " << o.supersample << "x | "
         << results.size() << " candidates</p>\n";
    html << "<p class=\"muted\">Ground truth is an area downsample of a supersampled scene; the aliased input is a point "
            "sample of the same scene. PSNR/SSIM compare the candidate output with the ground truth. Real captures have "
            "no ground truth and are a visual/do-no-harm check only.</p>\n";

    html << "<h2>Candidate ranking</h2><table><tr><th>#</th><th>candidate</th><th>eligible</th><th>score</th>"
            "<th>dPSNR</th><th>dSSIM</th><th>step</th><th>1080p ms</th><th>1440p ms</th><th>failure</th></tr>\n";
    int rank = 0;
    for (const CandidateResult& r : results) {
        ++rank;
        html << "<tr><td>" << rank << "</td><td>" << r.candidate.Label() << "</td><td class=\""
             << (r.pass ? "ok" : "bad") << "\">" << (r.pass ? "yes" : "no") << "</td><td>" << std::fixed
             << std::setprecision(1) << r.score << "</td><td>" << std::setprecision(3) << r.meanDp << "</td><td>"
             << std::setprecision(4) << r.meanDs << "</td><td>" << std::setprecision(3) << r.meanStep << "</td><td>"
             << std::setprecision(3) << r.ms1080 << "</td><td>" << r.ms1440 << "</td><td class=\"small\">" << r.fail
             << "</td></tr>\n";
    }
    html << "</table>\n";

    auto sceneTable = [&](const CandidateResult& r, const char* suffix) {
        html << "<h2>Visual review: " << suffix << " (" << r.candidate.Label() << ")</h2>\n";
        html << "<table><tr><th>scene</th><th>input (aliased)</th><th>ground truth</th><th>output</th>"
                "<th>dPSNR</th><th>dSSIM</th><th>step</th><th>gate</th></tr>\n";
        for (const SceneMetrics& m : r.scenes) {
            html << "<tr><td>" << m.scene << "<br><span class=\"small muted\">" << KindLabel(m.kind)
                 << "</span></td><td><img src=\"images/" << m.scene << "_in.png\" width=\"220\"></td><td><img src=\"images/"
                 << m.scene << "_gt.png\" width=\"220\"></td><td><img src=\"images/" << m.scene << "_" << suffix
                 << ".png\" width=\"220\"></td><td>" << std::fixed << std::setprecision(3) << m.dPsnr << "</td><td>"
                 << std::setprecision(4) << m.dSsim << "</td><td>" << std::setprecision(3) << m.stepRatio
                 << "</td><td class=\"" << (m.pass ? "ok" : "bad") << "\">" << (m.pass ? "pass" : "FAIL") << "<br>"
                 << "<span class=\"small\">" << m.fail << "</span></td></tr>\n";
        }
        html << "</table>\n";
        html << "<h3>4x crops, input / ground truth / output</h3><table><tr><th>scene</th><th>input</th>"
                "<th>ground truth</th><th>output</th></tr>\n";
        for (const SceneMetrics& m : r.scenes) {
            html << "<tr><td>" << m.scene << "</td><td><img src=\"images/" << m.scene << "_in_crop.png\" width=\"300\"></td>"
                 << "<td><img src=\"images/" << m.scene << "_gt_crop.png\" width=\"300\"></td>"
                 << "<td><img src=\"images/" << m.scene << "_" << suffix << "_crop.png\" width=\"300\"></td></tr>\n";
        }
        html << "</table>\n";
    };
    sceneTable(top, "top");
    if (haveDefault) sceneTable(*def, "default");

    if (!real.empty()) {
        html << "<h2>Real captures (" << top.candidate.Label() << ")</h2><table><tr><th>capture</th><th>input</th>"
                "<th>output</th><th>difference x4</th><th>changed</th><th>mean |d|</th><th>high-pass</th><th>max</th>"
                "<th>GPU</th></tr>\n";
        for (const RealResult& r : real) {
            html << "<tr><td>" << r.name << "</td><td><img src=\"images/real_" << r.name << "_in.png\" width=\"260\"></td>"
                 << "<td><img src=\"images/real_" << r.name << "_top.png\" width=\"260\"></td>"
                 << "<td><img src=\"images/real_" << r.name << "_diff.png\" width=\"260\"></td><td>" << std::fixed
                 << std::setprecision(2) << r.changed << " %</td><td>" << r.mean << "</td><td>" << std::setprecision(3)
                 << r.hpRatio << "</td><td>" << r.maxDiff << "</td><td>" << std::setprecision(3) << r.ms
                 << "</td></tr>\n";
        }
        html << "</table>\n";
    }

    html << "<h2>Review checklist</h2><ul>"
            "<li>4x crops: is the staircase gone on the slanted edges and the roofline?</li>"
            "<li>Text/HUD: do the glyphs stay crisp at 1:1 and 4x?</li>"
            "<li>4-px checker: is it still crisp, not smeared?</li>"
            "<li>Real captures: is the effect visible in motion, and worth the cost?</li>"
            "<li>Does the game's own AA look better, worse or different? Record it; that is allowed.</li>"
            "</ul>\n</body></html>\n";

    std::ofstream meta(dir / "meta.json");
    WriteJsonMeta(meta, o, adapter, commit);

    printf("\nReview pack: %s\n", (dir / "index.html").string().c_str());
    printf("Candidate CSV: %s\n", (dir / "candidates.csv").string().c_str());
    printf("Scene CSV: %s\n", (dir / "scenes.csv").string().c_str());
}

// ---------------------------------------------------------------- real captures
std::vector<RealResult> RunReal(const AaSuiteOptions& o, GpuDevice& dev, const Candidate& c, const fs::path& dir) {
    std::vector<RealResult> out;
    for (const fs::path& p : o.images) {
        Image src;
        std::string err;
        if (!ReadPng(p.wstring(), src, err)) {
            printf("warning: %s: %s\n", p.string().c_str(), err.c_str());
            continue;
        }
        Harness h;
        if (!h.Init(dev, src.w, src.h, err)) {
            printf("warning: %s: %s\n", p.string().c_str(), err.c_str());
            continue;
        }
        h.Upload(src);
        Image dst;
        if (!h.Apply(c.Settings()) || !h.Read(dst)) {
            printf("warning: %s: apply/read failed\n", p.string().c_str());
            continue;
        }
        h.Run(60, c.Settings());
        const AaStats st = h.pass->Stats();

        const std::string stem = p.stem().string();
        Save(dir / "images" / ("real_" + stem + "_in.png"), src, err);
        Save(dir / "images" / ("real_" + stem + "_top.png"), dst, err);
        Save(dir / "images" / ("real_" + stem + "_diff.png"), DiffAmplified(src, dst, 4.0), err);
        const uint32_t cw = std::min<uint32_t>(240, src.w), ch = std::min<uint32_t>(240, src.h);
        Save(dir / "images" / ("real_" + stem + "_in_crop.png"),
             CropZoom(src, (src.w - cw) / 2, (src.h - ch) / 2, cw, ch, 4), err);
        Save(dir / "images" / ("real_" + stem + "_top_crop.png"),
             CropZoom(dst, (dst.w - cw) / 2, (dst.h - ch) / 2, cw, ch, 4), err);

        RealResult r;
        r.name = stem;
        const double hp0 = HighPassEnergy(src), hp1 = HighPassEnergy(dst);
        r.hpRatio = hp0 > 0.0 ? hp1 / hp0 : 1.0;
        r.maxDiff = MaxDifference(src, dst);
        r.ms = st.gpuMs;
        size_t changed = 0;
        double sum = 0.0;
        const size_t n = static_cast<size_t>(src.w) * src.h;
        for (size_t i = 0; i < n; ++i) {
            int d = 0;
            for (int ch2 = 0; ch2 < 3; ++ch2)
                d = std::max(d, std::abs(static_cast<int>(src.px[i * 4 + ch2]) - static_cast<int>(dst.px[i * 4 + ch2])));
            if (d > 0) ++changed;
            sum += d;
        }
        r.changed = 100.0 * static_cast<double>(changed) / static_cast<double>(n);
        r.mean = sum / static_cast<double>(n);
        out.push_back(r);
        printf("real %-28s %ux%u changed %5.2f%% mean %5.2f high-pass %.3f max %3d GPU %.3f ms\n", stem.c_str(),
               src.w, src.h, r.changed, r.mean, r.hpRatio, r.maxDiff, r.ms);
    }
    return out;
}

} // namespace

int RunAaSuite(const AaSuiteOptions& o, const moonsr::AdapterInfo& adapter) {
    std::string err;
    std::wstring werr;
    auto dev = GpuDevice::Create(&adapter.luid, werr);
    if (!dev) {
        printf("error: adapter '%s': %s\n", Narrow(adapter.name).c_str(), Narrow(werr).c_str());
        return 1;
    }
    printf("\nAA suite on %s (%ux%u, %dx supersample)%s\n", Narrow(adapter.name).c_str(), o.width, o.height,
           o.supersample, o.sweep ? ", sweep" : "");
    printf("Generating ground truth...\n");
    const std::vector<Scene> scenes = MakeScenes(o.width, o.height, o.supersample);
    const std::vector<Candidate> candidates = BuildCandidates(o);

    Harness qh;
    if (!qh.Init(*dev, o.width, o.height, err)) {
        printf("error: %s\n", err.c_str());
        return 1;
    }

    // Cost harnesses at the two budget resolutions.
    Harness cost1080, cost1440;
    Image busy1080, busy1440;
    if (o.timing && cost1080.Init(*dev, 1920, 1080, err) && cost1440.Init(*dev, 2560, 1440, err)) {
        busy1080 = GenBusy(1920, 1080);
        busy1440 = GenBusy(2560, 1440);
        cost1080.Upload(busy1080);
        cost1440.Upload(busy1440);
    }

    std::vector<CandidateResult> results;
    int defaultFailures = 0;
    bool haveDefault = false;
    for (const Candidate& c : candidates) {
        CandidateResult r;
        r.candidate = c;
        for (const Scene& s : scenes) {
            qh.Upload(s.in);
            Image out;
            if (!qh.Apply(c.Settings()) || !qh.Read(out)) {
                printf("error: %s/%s: apply/read failed\n", c.Label().c_str(), s.name.c_str());
                r.pass = false;
                r.fail += "apply/read failed; ";
                continue;
            }
            r.scenes.push_back(MeasureScene(s, out));
        }
        if (o.timing && cost1080.pass && cost1440.pass) {
            // Three independent medians; a single clock/thermal spike must not decide the ranking.
            auto measure = [](Harness& h, const AaSettings& s) {
                h.pass->Reset();
                h.Run(20, s);
                double v[3]{};
                for (double& x : v) {
                    h.pass->Reset();
                    h.Run(45, s);
                    x = h.pass->Stats().gpuMs;
                }
                std::sort(v, v + 3);
                return v[1];
            };
            r.ms1080 = measure(cost1080, c.Settings());
            r.ms1440 = measure(cost1440, c.Settings());
            if (r.ms1080 > kBudget1080Ms) {
                r.pass = false;
                r.fail += "1080p budget; ";
            }
            if (r.ms1440 > kBudget1440Ms) {
                r.pass = false;
                r.fail += "1440p budget; ";
            }
        }
        Aggregate(r);
        if (c.IsDefault()) {
            haveDefault = true;
            if (!r.pass) ++defaultFailures;
        }
        results.push_back(std::move(r));
        printf("  %-28s %s score %8.1f  dPSNR %6.3f  dSSIM %7.4f  step %5.3f  %6.3f/%6.3f ms\n",
               c.Label().c_str(), results.back().pass ? "pass " : "FAIL ", results.back().score,
               results.back().meanDp, results.back().meanDs, results.back().meanStep, results.back().ms1080,
               results.back().ms1440);
    }

    std::sort(results.begin(), results.end(), [](const CandidateResult& a, const CandidateResult& b) {
        if (a.pass != b.pass) return a.pass > b.pass;
        if (std::abs(a.score - b.score) > 0.05) return a.score > b.score;
        const double ca = a.ms1080 + a.ms1440, cb = b.ms1080 + b.ms1440;
        if (std::abs(ca - cb) > 0.02) return ca < cb;
        const auto distance = [](const Candidate& c) {
            return std::abs(c.threshold - 0.10f) + 0.1f * std::abs(c.quality - 1) +
                   0.1f * std::abs(c.strength - 1.0f);
        };
        return distance(a.candidate) < distance(b.candidate);
    });
    const CandidateResult& top = results.front();
    const CandidateResult* def = nullptr;
    for (const CandidateResult& r : results)
        if (r.candidate.IsDefault()) def = &r;

    printf("\nRanked candidates:\n");
    printf("%-28s %-5s %8s %8s %8s %8s %8s %8s %s\n", "candidate", "pass", "score", "dPSNR", "dSSIM", "step", "1080ms",
           "1440ms", "failure");
    for (const CandidateResult& r : results) {
        printf("%-28s %-5s %8.1f %8.3f %8.4f %8.3f %8.3f %8.3f %s\n", r.candidate.Label().c_str(),
               r.pass ? "yes" : "no", r.score, r.meanDp, r.meanDs, r.meanStep, r.ms1080, r.ms1440, r.fail.c_str());
    }
    printf("\n%s: %s\n", top.pass ? "Top eligible" : "No candidate passed; best effort",
           top.candidate.Label().c_str());
    if (!top.pass) printf("warning: no candidate passed all gates\n");

    const fs::path dir = o.outDir;
    const SystemInfo si = CollectSystemInfo(FindRepoRoot(), "");
    const std::vector<RealResult> real = RunReal(o, *dev, top.candidate, dir);
    WriteReport(dir, o, adapter, scenes, results, top, def, *dev, real, si.gitCommit, si.date);

    if (!haveDefault) printf("warning: default candidate was not measured\n");
    return defaultFailures ? 1 : 0;
}

} // namespace bench
