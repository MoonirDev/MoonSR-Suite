// moonsr_imgtool: small WIC-only PNG inspection tool (crop / mosaic / diff / strip / info).
// Single-threaded console tool. No third-party code.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;  // BGRA, tightly packed
    void Alloc(uint32_t W, uint32_t H, uint8_t fill = 0) {
        w = W;
        h = H;
        px.assign(static_cast<size_t>(W) * H * 4, fill);
    }
};

IWICImagingFactory* Factory() {
    static IWICImagingFactory* f = [] {
        IWICImagingFactory* p = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p));
        return p;
    }();
    return f;
}

bool ReadPng(const std::wstring& path, Image& out) {
    IWICImagingFactory* f = Factory();
    if (!f) { fprintf(stderr, "error: WIC factory unavailable\n"); return false; }
    ComPtr<IWICBitmapDecoder> dec;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) { fprintf(stderr, "error: cannot open '%ls' (0x%08X)\n", path.c_str(), static_cast<unsigned>(hr)); return false; }
    ComPtr<IWICBitmapFrameDecode> fr;
    hr = dec->GetFrame(0, &fr);
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    if (FAILED(hr)) { fprintf(stderr, "error: cannot decode '%ls' (0x%08X)\n", path.c_str(), static_cast<unsigned>(hr)); return false; }
    out.Alloc(w, h);
    hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size()), out.px.data());
    if (FAILED(hr)) { fprintf(stderr, "error: CopyPixels failed (0x%08X)\n", static_cast<unsigned>(hr)); return false; }
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    return true;
}

bool WritePng(const std::wstring& path, const Image& img) {
    IWICImagingFactory* f = Factory();
    if (!f) { fprintf(stderr, "error: WIC factory unavailable\n"); return false; }
    ComPtr<IWICStream> stream;
    HRESULT hr = f->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> enc;
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> fr;
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
    if (FAILED(hr)) { fprintf(stderr, "error: cannot create '%ls' (0x%08X)\n", path.c_str(), static_cast<unsigned>(hr)); return false; }
    std::vector<uint8_t> rgb(static_cast<size_t>(img.w) * img.h * 3);
    for (size_t i = 0, n = static_cast<size_t>(img.w) * img.h; i < n; ++i) {
        rgb[i * 3 + 0] = img.px[i * 4 + 0];
        rgb[i * 3 + 1] = img.px[i * 4 + 1];
        rgb[i * 3 + 2] = img.px[i * 4 + 2];
    }
    hr = fr->WritePixels(img.h, img.w * 3, static_cast<UINT>(rgb.size()), rgb.data());
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) { fprintf(stderr, "error: PNG write failed (0x%08X)\n", static_cast<unsigned>(hr)); return false; }
    return true;
}

struct Region {  // in pixels of the first image
    int x = 0, y = 0, w = 0, h = 0;
};

// Clamps a requested region to an image; false if empty.
bool ClampRegion(const Image& img, int x, int y, int w, int h, Region& r) {
    const int x0 = std::clamp(x, 0, static_cast<int>(img.w));
    const int y0 = std::clamp(y, 0, static_cast<int>(img.h));
    const int x1 = std::clamp(x + w, 0, static_cast<int>(img.w));
    const int y1 = std::clamp(y + h, 0, static_cast<int>(img.h));
    if (x1 <= x0 || y1 <= y0) return false;
    r = Region{x0, y0, x1 - x0, y1 - y0};
    return true;
}

// Nearest-neighbour magnified copy of source rect (rx, ry, rw, rh in `src` pixels, fractional allowed) into dst at
// (ox, oy), tile size tw x th.
void BlitTile(const Image& src, double rx, double ry, double rw, double rh, Image& dst, uint32_t ox, uint32_t oy, uint32_t tw, uint32_t th) {
    for (uint32_t y = 0; y < th; ++y) {
        int sy = static_cast<int>(std::floor(ry + (y + 0.5) / th * rh));
        sy = std::clamp(sy, 0, static_cast<int>(src.h) - 1);
        for (uint32_t x = 0; x < tw; ++x) {
            int sx = static_cast<int>(std::floor(rx + (x + 0.5) / tw * rw));
            sx = std::clamp(sx, 0, static_cast<int>(src.w) - 1);
            const uint8_t* s = &src.px[(static_cast<size_t>(sy) * src.w + sx) * 4];
            uint8_t* d = &dst.px[(static_cast<size_t>(oy + y) * dst.w + ox + x) * 4];
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
    }
}

// Lays out the region of each image (mapped proportionally from the first image) left to right.
bool MakeMosaic(const std::vector<Image>& imgs, const Region& r, int zoom, Image& out) {
    const int sep = 4;
    const uint32_t tw = static_cast<uint32_t>(r.w * zoom), th = static_cast<uint32_t>(r.h * zoom);
    const uint32_t n = static_cast<uint32_t>(imgs.size());
    out.Alloc(tw * n + static_cast<uint32_t>(sep) * (n - 1), th, 255);
    // Separator columns are mid-grey.
    for (uint32_t i = 0; i + 1 < n; ++i)
        for (uint32_t y = 0; y < th; ++y)
            for (int s = 0; s < sep; ++s) {
                uint8_t* d = &out.px[(static_cast<size_t>(y) * out.w + (i + 1) * tw + i * sep + s) * 4];
                d[0] = d[1] = d[2] = 128;
                d[3] = 255;
            }
    const Image& first = imgs[0];
    for (uint32_t i = 0; i < n; ++i) {
        const Image& im = imgs[i];
        const double sx = static_cast<double>(im.w) / first.w, sy = static_cast<double>(im.h) / first.h;
        BlitTile(im, r.x * sx, r.y * sy, r.w * sx, r.h * sy, out, i * (tw + sep), 0, tw, th);
    }
    return true;
}

bool ParseInt(const char* s, int& v) {
    char* end = nullptr;
    const long l = strtol(s, &end, 10);
    if (end == s || *end != '\0') { fprintf(stderr, "error: '%s' is not an integer\n", s); return false; }
    v = static_cast<int>(l);
    return true;
}
bool ParseZoom(const char* s, int& z) {
    if (!ParseInt(s, z)) return false;
    if (z < 1 || z > 16) { fprintf(stderr, "error: zoom must be 1..16\n"); return false; }
    return true;
}

std::wstring W(const char* s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 1)) - 1, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

void Usage() {
    fputs(
        "moonsr_imgtool - PNG inspection helper\n"
        "usage:\n"
        "  crop   <in.png> <x> <y> <w> <h> <zoom 1..16> <out.png>\n"
        "  mosaic <out.png> <zoom> <x> <y> <w> <h> <in1.png> [in2.png ...]\n"
        "         region is in pixels of in1; other images use the same region in normalised coordinates\n"
        "  diff   <a.png> <b.png> <out.png> [gain=8]\n"
        "         prints mean/max abs diff, PSNR and the bbox of pixels differing by more than 2 levels\n"
        "  temporal <out1.png> <out2.png> <truth1.png> <truth2.png>\n"
        "         reports error in the change between two generated phases\n"
        "  strip  <out.png> <zoom> <x> <y> <w> <h> <dir> <first> <count>\n"
        "         out_NNNN_*.png for NNNN = first..first+count-1, same crop of each, left to right\n"
        "  info   <in.png>\n",
        stderr);
}

int CmdCrop(int argc, char** argv) {
    if (argc != 9) { Usage(); return 2; }
    int x, y, w, h, z;
    if (!ParseInt(argv[3], x) || !ParseInt(argv[4], y) || !ParseInt(argv[5], w) || !ParseInt(argv[6], h) || !ParseZoom(argv[7], z)) return 2;
    Image in, out;
    if (!ReadPng(W(argv[2]), in)) return 1;
    Region r;
    if (!ClampRegion(in, x, y, w, h, r)) { fprintf(stderr, "error: region is outside the %ux%u image\n", in.w, in.h); return 1; }
    std::vector<Image> v{std::move(in)};
    MakeMosaic(v, r, z, out);
    if (!WritePng(W(argv[8]), out)) return 1;
    printf("crop %d,%d %dx%d zoom %d -> %ux%u\n", r.x, r.y, r.w, r.h, z, out.w, out.h);
    return 0;
}

int CmdMosaic(int argc, char** argv) {
    if (argc < 9) { Usage(); return 2; }
    int z, x, y, w, h;
    if (!ParseZoom(argv[3], z) || !ParseInt(argv[4], x) || !ParseInt(argv[5], y) || !ParseInt(argv[6], w) || !ParseInt(argv[7], h)) return 2;
    std::vector<Image> imgs;
    for (int i = 8; i < argc; ++i) {
        Image im;
        if (!ReadPng(W(argv[i]), im)) return 1;
        imgs.push_back(std::move(im));
    }
    Region r;
    if (!ClampRegion(imgs[0], x, y, w, h, r)) { fprintf(stderr, "error: region is outside the first image\n"); return 1; }
    Image out;
    MakeMosaic(imgs, r, z, out);
    if (!WritePng(W(argv[2]), out)) return 1;
    printf("mosaic of %zu tiles, region %d,%d %dx%d of first image (%ux%u), zoom %d -> %ux%u\n", imgs.size(), r.x, r.y, r.w, r.h,
           imgs[0].w, imgs[0].h, z, out.w, out.h);
    return 0;
}

int CmdDiff(int argc, char** argv) {
    if (argc != 5 && argc != 6) { Usage(); return 2; }
    double gain = 8.0;
    if (argc == 6) {
        char* end = nullptr;
        gain = strtod(argv[5], &end);
        if (end == argv[5] || *end != '\0' || gain < 0) { fprintf(stderr, "error: bad gain\n"); return 2; }
    }
    Image a, b, out;
    if (!ReadPng(W(argv[2]), a) || !ReadPng(W(argv[3]), b)) return 1;
    if (a.w != b.w || a.h != b.h) { fprintf(stderr, "error: size mismatch %ux%u vs %ux%u\n", a.w, a.h, b.w, b.h); return 1; }
    out.Alloc(a.w, a.h, 255);
    double sum = 0, sq = 0;
    int maxd = 0;
    int bx0 = static_cast<int>(a.w), by0 = static_cast<int>(a.h), bx1 = -1, by1 = -1;
    for (uint32_t y = 0; y < a.h; ++y)
        for (uint32_t x = 0; x < a.w; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.w + x) * 4;
            int pixMax = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>(a.px[i + c]) - static_cast<int>(b.px[i + c]));
                sum += d;
                sq += static_cast<double>(d) * d;
                pixMax = std::max(pixMax, d);
                out.px[i + c] = static_cast<uint8_t>(std::min(255.0, d * gain));
            }
            maxd = std::max(maxd, pixMax);
            if (pixMax > 2) {
                bx0 = std::min(bx0, static_cast<int>(x)); by0 = std::min(by0, static_cast<int>(y));
                bx1 = std::max(bx1, static_cast<int>(x)); by1 = std::max(by1, static_cast<int>(y));
            }
        }
    const double n = static_cast<double>(a.w) * a.h * 3.0;
    const double mse = sq / n;
    if (!WritePng(W(argv[4]), out)) return 1;
    printf("size %ux%u gain %.2f\n", a.w, a.h, gain);
    printf("mean abs diff: %.4f\n", sum / n);
    printf("max abs diff:  %d\n", maxd);
    if (mse == 0) printf("PSNR: inf (identical)\n"); else printf("PSNR: %.3f dB\n", 10.0 * std::log10(255.0 * 255.0 / mse));
    if (bx1 < 0) printf("bbox of pixels differing by >2: none\n");
    else printf("bbox of pixels differing by >2: x %d..%d, y %d..%d (%dx%d)\n", bx0, bx1, by0, by1, bx1 - bx0 + 1, by1 - by0 + 1);
    return 0;
}

int CmdTemporal(int argc, char** argv) {
    if (argc != 6) { Usage(); return 2; }
    std::array<Image, 4> frames;
    for (int i = 0; i < 4; ++i) {
        if (!ReadPng(W(argv[i + 2]), frames[i])) return 1;
        if (i && (frames[i].w != frames[0].w || frames[i].h != frames[0].h)) {
            fprintf(stderr, "error: temporal inputs must have matching dimensions\n");
            return 1;
        }
    }
    double sum = 0.0, squared = 0.0;
    for (size_t i = 0; i < frames[0].px.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            const int delta = (static_cast<int>(frames[1].px[i + c]) - frames[0].px[i + c]) -
                              (static_cast<int>(frames[3].px[i + c]) - frames[2].px[i + c]);
            sum += std::abs(delta);
            squared += static_cast<double>(delta) * delta;
        }
    }
    const double n = static_cast<double>(frames[0].w) * frames[0].h * 3.0;
    printf("temporal difference error: mean absolute %.4f, RMS %.4f (0 is exact)\n", sum / n, std::sqrt(squared / n));
    return 0;
}

int CmdStrip(int argc, char** argv) {
    if (argc != 11) { Usage(); return 2; }
    int z, x, y, w, h, first, count;
    if (!ParseZoom(argv[3], z) || !ParseInt(argv[4], x) || !ParseInt(argv[5], y) || !ParseInt(argv[6], w) || !ParseInt(argv[7], h) ||
        !ParseInt(argv[9], first) || !ParseInt(argv[10], count))
        return 2;
    if (count < 1) { fprintf(stderr, "error: count must be >= 1\n"); return 2; }
    std::error_code ec;
    std::vector<std::pair<int, fs::path>> files;
    for (const auto& e : fs::directory_iterator(fs::path(W(argv[8])), ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() < 12 || name.compare(0, 4, "out_") != 0 || e.path().extension() != ".png") continue;
        char* end = nullptr;
        const long n = strtol(name.c_str() + 4, &end, 10);
        if (end != name.c_str() + 8 || *end != '_') continue;
        if (n >= first && n < first + count) files.emplace_back(static_cast<int>(n), e.path());
    }
    if (ec) { fprintf(stderr, "error: cannot list '%s'\n", argv[8]); return 1; }
    std::sort(files.begin(), files.end());
    if (files.empty()) { fprintf(stderr, "error: no out_NNNN_*.png in range %d..%d\n", first, first + count - 1); return 1; }
    std::vector<Image> imgs;
    for (const auto& f : files) {
        Image im;
        if (!ReadPng(f.second.wstring(), im)) return 1;
        printf("%s\n", f.second.filename().string().c_str());
        imgs.push_back(std::move(im));
    }
    Region r;
    if (!ClampRegion(imgs[0], x, y, w, h, r)) { fprintf(stderr, "error: region is outside the first image\n"); return 1; }
    Image out;
    MakeMosaic(imgs, r, z, out);
    if (!WritePng(W(argv[2]), out)) return 1;
    printf("strip of %zu tiles -> %ux%u\n", imgs.size(), out.w, out.h);
    return 0;
}

int CmdInfo(int argc, char** argv) {
    if (argc != 3) { Usage(); return 2; }
    Image im;
    if (!ReadPng(W(argv[2]), im)) return 1;
    double s[3] = {0, 0, 0};
    for (size_t i = 0; i < im.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) s[c] += im.px[i + c];
    const double n = static_cast<double>(im.w) * im.h;
    printf("%ux%u mean RGB (%.2f, %.2f, %.2f)\n", im.w, im.h, s[2] / n, s[1] / n, s[0] / n);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { Usage(); return 2; }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) { fprintf(stderr, "error: CoInitializeEx failed\n"); return 1; }
    const std::string cmd = argv[1];
    int rc;
    if (cmd == "crop") rc = CmdCrop(argc, argv);
    else if (cmd == "mosaic") rc = CmdMosaic(argc, argv);
    else if (cmd == "diff") rc = CmdDiff(argc, argv);
    else if (cmd == "temporal") rc = CmdTemporal(argc, argv);
    else if (cmd == "strip") rc = CmdStrip(argc, argv);
    else if (cmd == "info") rc = CmdInfo(argc, argv);
    else { Usage(); rc = 2; }
    fflush(stdout);
    return rc;  // WIC factory is intentionally not released before process exit
}
