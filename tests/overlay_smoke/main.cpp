// moonsr_overlay_smoke: console smoke test for src/overlay (StatsOverlay).
//
// Threading: single-threaded (main thread, COM MTA), mirrors the engine pipeline thread.
//
// For a 1920x1080 and a 3840x2160 BGRA8 offscreen target, for backgrounds mid-grey / white / black and the texts
// "30/60", "144/144", "7/120", "0/0": clear, Draw, read back and check
//   (a) pixels outside a generous top-left box (400x80, scaled with the target height) are untouched,
//   (b) a meaningful number of pixels inside it changed,
//   (c) dark pixels exist on the white background and light pixels on the black one (readability on both),
//   (d) identical numbers give identical output, different numbers give different output,
//   (e) the changed-pixel bounding box starts near the margin and its height is close to the expected text height,
//   plus: the 4K text is about 2x the 1080p text and no render target / SRV is left bound. Each result is saved
//   (top-left 480x120 cropped, 3x nearest neighbour) as a PNG under benchmarks/output/overlay/.
// Exit code 0 when every check passed, 1 otherwise. Log: overlay_smoke.log in the working directory.
#include "core/log.h"
#include "gpu/gpu_device.h"
#include "overlay/stats_overlay.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace moonsr;
using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void Print(const std::wstring& s) {
    fputws(s.c_str(), stdout);
    fflush(stdout);
}

void Check(const std::wstring& name, bool ok, const std::wstring& detail = L"") {
    Print(std::format(L"[{}] {}{}{}\n", ok ? L"PASS" : L"FAIL", name, detail.empty() ? L"" : L" - ", detail));
    if (!ok) ++g_failures;
}

struct Image {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px; // BGRA
    uint32_t at(UINT x, UINT y) const { return px[static_cast<size_t>(y) * w + x]; }
};

bool SavePng(IWICImagingFactory* wic, const std::filesystem::path& path, const Image& img) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat32bppBGRA) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr))
        hr = frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(img.px.size() * 4),
                                reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.px.data())));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) Print(std::format(L"  (PNG save failed for {}: {})\n", path.wstring(), HrToString(hr)));
    return SUCCEEDED(hr);
}

// Top-left crop (cw x ch) magnified by `zoom` with nearest neighbour, forced opaque.
Image CropZoom(const Image& src, UINT cw, UINT ch, UINT zoom) {
    Image o;
    o.w = cw * zoom;
    o.h = ch * zoom;
    o.px.assign(static_cast<size_t>(o.w) * o.h, 0xFFFF00FF);
    for (UINT y = 0; y < o.h; ++y)
        for (UINT x = 0; x < o.w; ++x) {
            const UINT sx = x / zoom, sy = y / zoom;
            if (sx < src.w && sy < src.h) o.px[static_cast<size_t>(y) * o.w + x] = src.at(sx, sy) | 0xFF000000u;
        }
    return o;
}

struct Rig {
    ID3D11Device1* dev = nullptr;
    ID3D11DeviceContext1* ctx = nullptr;
    ComPtr<ID3D11Texture2D> rt, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    Size size{};

    bool Init(GpuDevice& gd, Size s) {
        dev = gd.Device();
        ctx = gd.Context();
        size = s;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s.width;
        td.Height = s.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) return false;
        if (FAILED(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv))) return false;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging));
    }

    void Clear(float v) {
        const float c[4] = {v, v, v, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), c);
    }

    bool Read(Image& out) {
        ctx->CopyResource(staging.Get(), rt.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out.w = size.width;
        out.h = size.height;
        out.px.resize(static_cast<size_t>(out.w) * out.h);
        for (UINT y = 0; y < out.h; ++y)
            memcpy(&out.px[static_cast<size_t>(y) * out.w],
                   static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, out.w * 4);
        ctx->Unmap(staging.Get(), 0);
        return true;
    }
};

int Luma(uint32_t p) {
    const int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
    return (r * 54 + g * 183 + b * 19) >> 8;
}

struct Analysis {
    uint64_t changedOutside = 0, changedInside = 0, dark = 0, light = 0;
    int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1;
};

Analysis Analyse(const Image& img, uint32_t bg, UINT boxW, UINT boxH) {
    Analysis a;
    for (UINT y = 0; y < img.h; ++y)
        for (UINT x = 0; x < img.w; ++x) {
            const uint32_t p = img.at(x, y);
            if ((p & 0x00FFFFFF) == (bg & 0x00FFFFFF)) continue;
            if (x >= boxW || y >= boxH) {
                ++a.changedOutside;
                continue;
            }
            ++a.changedInside;
            a.minX = std::min<int>(a.minX, static_cast<int>(x));
            a.maxX = std::max<int>(a.maxX, static_cast<int>(x));
            a.minY = std::min<int>(a.minY, static_cast<int>(y));
            a.maxY = std::max<int>(a.maxY, static_cast<int>(y));
            const int l = Luma(p);
            if (l < 100) ++a.dark;
            if (l > 200) ++a.light;
        }
    return a;
}

std::wstring SafeName(const std::wstring& s) {
    std::wstring o = s;
    std::replace(o.begin(), o.end(), L'/', L'-');
    return o;
}

struct TextCase {
    uint32_t rendered, total;
    std::wstring name;
};

struct Bg {
    float v;
    const wchar_t* name;
};

} // namespace

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo)) {
        Print(std::format(L"CoInitializeEx failed: {}\n", HrToString(hrCo)));
        return 1;
    }
    InitLog(L"overlay_smoke.log");

    const std::filesystem::path outDir =
        std::filesystem::path(MOONSR_SOURCE_DIR) / L"benchmarks" / L"output" / L"overlay";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::wstring err;
    auto dev = GpuDevice::Create(nullptr, err);
    if (!dev) {
        Print(L"GpuDevice::Create failed: " + err + L"\n");
        return 1;
    }
    Print(std::format(L"Adapter: {}\n", dev->Info().name));
    auto overlay = StatsOverlay::Create(*dev, err);
    Check(L"StatsOverlay::Create", overlay != nullptr, err);
    if (!overlay) return 1;

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) Print(std::format(L"WIC unavailable ({}); PNGs will not be written\n", HrToString(hr)));

    const TextCase texts[] = {{30, 60, L"30/60"}, {144, 144, L"144/144"}, {7, 120, L"7/120"}, {0, 0, L"0/0"}};
    const Bg bgs[] = {{0.5f, L"grey"}, {1.0f, L"white"}, {0.0f, L"black"}};
    const Size sizes[] = {{1920, 1080}, {3840, 2160}};

    int bboxHeight1080 = 0; // for the scaling check
    std::map<std::wstring, Image> results;

    for (const Size sz : sizes) {
        Rig rig;
        if (!rig.Init(*dev, sz)) {
            Check(std::format(L"{}x{} render target", sz.width, sz.height), false);
            continue;
        }
        const uint32_t textH = std::max<uint32_t>(16, sz.height / 40);
        const UINT boxW = 400 * sz.height / 1080, boxH = 80 * sz.height / 1080;
        Print(std::format(L"\n=== target {}x{}: expected text height ~{} px, check box {}x{} ===\n", sz.width, sz.height,
                          textH, boxW, boxH));

        for (const Bg& bg : bgs) {
            // Background pixel value as the target actually stores it.
            Image blank;
            rig.Clear(bg.v);
            rig.Read(blank);
            const uint32_t bgPx = blank.px[0];

            for (const TextCase& t : texts) {
                const std::wstring tag = std::format(L"{}x{} {} {}", sz.width, sz.height, bg.name, t.name);
                rig.Clear(bg.v);
                const bool ok = overlay->Draw(rig.rtv.Get(), sz, t.rendered, t.total);
                Image img;
                const bool rd = rig.Read(img);
                Check(tag + L": Draw + readback", ok && rd);
                if (!ok || !rd) continue;

                const Analysis a = Analyse(img, bgPx, boxW, boxH);
                Check(tag + L": (a) nothing changed outside the box", a.changedOutside == 0,
                      std::format(L"{} px outside", a.changedOutside));
                Check(tag + L": (b) pixels changed inside", a.changedInside > 60 * textH / 27,
                      std::format(L"{} px changed", a.changedInside));
                if (bg.v == 1.0f) Check(tag + L": (c) dark pixels on white", a.dark > 10, std::format(L"{} dark", a.dark));
                if (bg.v == 0.0f) Check(tag + L": (c) light pixels on black", a.light > 10, std::format(L"{} light", a.light));

                const int margin = static_cast<int>(std::lround(0.6 * textH));
                const int bw = a.maxX - a.minX + 1, bh = a.maxY - a.minY + 1;
                const bool startOk = a.minY >= margin - 4 && a.minY <= margin + 2 && a.minX >= margin - 4 &&
                                     a.minX <= margin + static_cast<int>(textH) / 2 + 4;
                // Digits are ~0.7 em tall plus the outline on both sides; the slash is a little taller.
                const bool heightOk = bh >= static_cast<int>(0.55 * textH) && bh <= static_cast<int>(1.15 * textH) + 4;
                Check(tag + L": (e) bbox starts near the margin", startOk,
                      std::format(L"bbox x {}..{} y {}..{} ({}x{}), margin {}", a.minX, a.maxX, a.minY, a.maxY, bw, bh, margin));
                Check(tag + L": (e) bbox height close to text height", heightOk,
                      std::format(L"{} px vs text height {}", bh, textH));
                if (sz.width == 1920 && bg.v == 0.5f && t.name == L"30/60") bboxHeight1080 = bh;
                if (sz.width == 3840 && bg.v == 0.5f && t.name == L"30/60" && bboxHeight1080 > 0) {
                    const double ratio = static_cast<double>(bh) / bboxHeight1080;
                    Check(L"text scales with the target: 4K / 1080p height ratio about 2", ratio > 1.7 && ratio < 2.3,
                          std::format(L"{:.2f}", ratio));
                }

                // An unchanged redraw reproduces the image, and no state is left bound.
                if (t.name == L"30/60" || t.name == L"7/120") {
                    rig.Clear(bg.v);
                    overlay->Draw(rig.rtv.Get(), sz, t.rendered, t.total);
                    Image again;
                    rig.Read(again);
                    Check(tag + L": (d) same numbers drawn twice are identical", again.px == img.px);
                    ID3D11RenderTargetView* boundRtv = nullptr;
                    ID3D11ShaderResourceView* boundSrv = nullptr;
                    rig.ctx->OMGetRenderTargets(1, &boundRtv, nullptr);
                    rig.ctx->PSGetShaderResources(0, 1, &boundSrv);
                    Check(tag + L": no render target / SRV left bound", !boundRtv && !boundSrv);
                    if (boundRtv) boundRtv->Release();
                    if (boundSrv) boundSrv->Release();
                }

                const std::wstring key = std::format(L"{}x{}_{}_{}", sz.width, sz.height, bg.name, SafeName(t.name));
                results[key] = img;
                if (wic) SavePng(wic.Get(), outDir / (L"overlay_" + key + L".png"), CropZoom(img, 480, 120, 3));
            }
            // Different numbers give different output.
            const std::wstring base = std::format(L"{}x{}_{}_", sz.width, sz.height, bg.name);
            const Image& a1 = results[base + L"30-60"];
            const Image& a2 = results[base + L"144-144"];
            const Image& a3 = results[base + L"7-120"];
            Check(std::format(L"{}x{} {}: (d) different numbers give different output", sz.width, sz.height, bg.name),
                  !a1.px.empty() && a1.px != a2.px && a1.px != a3.px && a2.px != a3.px);
        }
    }

    Print(std::format(L"\n{} failure(s)\n", g_failures));
    overlay.reset();
    return g_failures == 0 ? 0 : 1;
}
