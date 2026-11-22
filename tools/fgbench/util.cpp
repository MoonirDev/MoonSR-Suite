// Metrics (PSNR, SSIM), CPU baselines and PNG output for moonsr_fgbench.
#include "fgbench.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>

using Microsoft::WRL::ComPtr;

namespace fgb {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

namespace {

double Psnr(double mse) { return mse <= 0 ? kInf : 10.0 * std::log10(255.0 * 255.0 / mse); }

std::vector<float> LumaPlane(const Image& img) {
    std::vector<float> l(static_cast<size_t>(img.w) * img.h);
    for (size_t i = 0; i < l.size(); ++i) {
        const uint8_t* p = &img.px[i * 4];
        l[i] = static_cast<float>(0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2]);
    }
    return l;
}

// Mean SSIM, 11x11 Gaussian (sigma 1.5), K1 0.01, K2 0.03, L 255, over the valid region (window fully inside).
double Ssim(const std::vector<float>& x, const std::vector<float>& y, uint32_t w, uint32_t h) {
    if (w < 11 || h < 11) return 0;
    double g[11], gs = 0;
    for (int i = 0; i < 11; ++i) gs += (g[i] = std::exp(-((i - 5) * (i - 5)) / (2 * 1.5 * 1.5)));
    for (double& v : g) v /= gs;
    const size_t n = static_cast<size_t>(w) * h;
    // Horizontal pass for mx, my, xx, yy, xy.
    std::vector<double> hx(n), hy(n), hxx(n), hyy(n), hxy(n);
    ParallelFor(h, [&](uint32_t r) {
        for (uint32_t c = 5; c + 5 < w; ++c) {
            double a = 0, b = 0, aa = 0, bb = 0, ab = 0;
            for (int k = 0; k < 11; ++k) {
                const size_t i = static_cast<size_t>(r) * w + c + k - 5;
                const double xv = x[i], yv = y[i];
                a += g[k] * xv;
                b += g[k] * yv;
                aa += g[k] * xv * xv;
                bb += g[k] * yv * yv;
                ab += g[k] * xv * yv;
            }
            const size_t o = static_cast<size_t>(r) * w + c;
            hx[o] = a;
            hy[o] = b;
            hxx[o] = aa;
            hyy[o] = bb;
            hxy[o] = ab;
        }
    });
    const double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    std::vector<double> rowSum(h, 0.0);
    ParallelFor(h, [&](uint32_t r) {
        if (r < 5 || r + 5 >= h) return;
        double s = 0;
        for (uint32_t c = 5; c + 5 < w; ++c) {
            double a = 0, b = 0, aa = 0, bb = 0, ab = 0;
            for (int k = 0; k < 11; ++k) {
                const size_t i = static_cast<size_t>(r + k - 5) * w + c;
                a += g[k] * hx[i];
                b += g[k] * hy[i];
                aa += g[k] * hxx[i];
                bb += g[k] * hyy[i];
                ab += g[k] * hxy[i];
            }
            const double vx = aa - a * a, vy = bb - b * b, cxy = ab - a * b;
            s += ((2 * a * b + c1) * (2 * cxy + c2)) / ((a * a + b * b + c1) * (vx + vy + c2));
        }
        rowSum[r] = s;
    });
    double total = 0;
    for (double v : rowSum) total += v;
    return total / (static_cast<double>(w - 10) * (h - 10));
}

} // namespace

Quality Compare(const Image& ref, const Image& test, uint32_t border, const Mask* mask) {
    Quality q;
    double se = 0, seIn = 0, seM = 0, seE = 0;
    uint64_t nIn = 0, nM = 0, nE = 0;
    const auto lum = [&](uint32_t x, uint32_t y) {
        const uint8_t* p = &ref.px[(static_cast<size_t>(y) * ref.w + x) * 4];
        return 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];
    };
    for (uint32_t y = 0; y < ref.h; ++y)
        for (uint32_t x = 0; x < ref.w; ++x) {
            const size_t i = (static_cast<size_t>(y) * ref.w + x) * 4;
            double e = 0;
            for (int c = 0; c < 3; ++c) {
                const double d = static_cast<double>(ref.px[i + c]) - test.px[i + c];
                e += d * d;
            }
            se += e;
            if (x >= border && y >= border && x + border < ref.w && y + border < ref.h) {
                seIn += e;
                ++nIn;
                if (x > 0 && y > 0 && x + 1 < ref.w && y + 1 < ref.h &&
                    std::abs(lum(x + 1, y) - lum(x - 1, y)) + std::abs(lum(x, y + 1) - lum(x, y - 1)) >= 48.0) {
                    seE += e;
                    ++nE;
                }
            }
            if (mask && mask->m[static_cast<size_t>(y) * ref.w + x]) {
                seM += e;
                ++nM;
            }
        }
    q.psnr = Psnr(se / (3.0 * ref.w * ref.h));
    q.psnrInner = nIn ? Psnr(seIn / (3.0 * nIn)) : 0;
    q.psnrMask = nM ? Psnr(seM / (3.0 * nM)) : 0;
    q.psnrEdge = nE ? Psnr(seE / (3.0 * nE)) : 0;
    q.ssim = Ssim(LumaPlane(ref), LumaPlane(test), ref.w, ref.h);
    return q;
}

double MaxAbsDiff(const Image& a, const Image& b, uint32_t border) {
    double m = 0;
    for (uint32_t y = border; y + border < a.h; ++y)
        for (uint32_t x = border; x + border < a.w; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.w + x) * 4;
            for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(static_cast<double>(a.px[i + c]) - b.px[i + c]));
        }
    return m;
}

Image RepeatNearest(const Image& a, const Image& b, double t) { return t <= 0.5 ? a : b; }

Image LinearBlend(const Image& a, const Image& b, double t) {
    Image o;
    o.Alloc(a.w, a.h);
    for (size_t i = 0; i < o.px.size(); ++i) {
        if ((i & 3) == 3) continue;
        o.px[i] = static_cast<uint8_t>(std::lround(a.px[i] * (1 - t) + b.px[i] * t));
    }
    return o;
}

namespace {
ComPtr<IWICImagingFactory> Factory() {
    static IWICImagingFactory* f = nullptr; // intentionally leaked (process lifetime; COM is torn down at exit)
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}
} // namespace

bool WritePng(const std::wstring& path, const Image& img, std::string& err) {
    auto f = Factory();
    if (!f) {
        err = "WIC factory unavailable";
        return false;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    HRESULT hr = f->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
    std::vector<uint8_t> rgb(static_cast<size_t>(img.w) * img.h * 3);
    for (size_t i = 0, n = static_cast<size_t>(img.w) * img.h; i < n; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = img.px[i * 4 + c];
    if (SUCCEEDED(hr)) hr = fr->WritePixels(img.h, img.w * 3, static_cast<UINT>(rgb.size()), rgb.data());
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) {
        char b[96];
        snprintf(b, sizeof(b), "PNG write failed (0x%08X)", static_cast<unsigned>(hr));
        err = b;
        return false;
    }
    return true;
}

bool ReadPng(const std::wstring& path, Image& img, std::string& err) {
    auto f = Factory();
    if (!f) {
        err = "WIC factory unavailable";
        return false;
    }
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    if (SUCCEEDED(hr)) {
        img.Alloc(w, h);
        hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(img.px.size()), img.px.data());
    }
    if (FAILED(hr)) {
        char b[96];
        snprintf(b, sizeof(b), "PNG read failed (0x%08X)", static_cast<unsigned>(hr));
        err = b;
        return false;
    }
    for (size_t i = 3; i < img.px.size(); i += 4) img.px[i] = 255;
    return true;
}

} // namespace fgb
