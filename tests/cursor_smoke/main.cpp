// moonsr_cursor_smoke: console smoke test for src/cursor (CursorOverlay).
//
// Threading: single-threaded. The test window, the D3D device and the overlay all live on the main thread, which is
// per-monitor-v2 DPI aware and COM MTA (mirrors the engine pipeline thread).
//
// What it does (moves the REAL cursor; the original position is restored at the end, needs an interactive desktop):
//  1. Poll: cursor inside the 640x360 client area -> visible at the exact position; outside / over the caption /
//     window minimised -> not visible.
//  2. Draw: for IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_CROSS, IDC_SIZEALL, IDC_WAIT (set via WM_SETCURSOR) draws into a
//     1280x720 BGRA target (destRect = whole target, 2x) and compares with a GDI DrawIconEx reference of the same
//     cursor: untouched far pixels, changed pixels near the mapped hotspot, bounding box, per-texel colours.
//     The I-beam is also drawn on dark and light backgrounds (invert path). One extra letterboxed 1.5x case.
//  3. SetSystemCursorHidden(true/false) and destructor restore, printing GetCursorInfo flags.
//  4. PNGs under benchmarks/output/cursor/.
// Exit code: 0 if every check passed, 1 otherwise. Log: cursor_smoke.log in the working directory.
#include "core/log.h"
#include "cursor/cursor_overlay.h"
#include "gpu/gpu_device.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace moonsr;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kClientW = 640, kClientH = 360;
constexpr UINT kRtW = 1280, kRtH = 720;
constexpr uint8_t kGrey = 128;

HCURSOR g_cursor = nullptr;
int g_failures = 0;

void Print(const std::wstring& s) {
    fputws(s.c_str(), stdout);
    fflush(stdout);
}

void Check(const std::wstring& name, bool ok, const std::wstring& detail = L"") {
    Print(std::format(L"[{}] {}{}{}\n", ok ? L"PASS" : L"FAIL", name, detail.empty() ? L"" : L" - ", detail));
    if (!ok) ++g_failures;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(g_cursor);
            return TRUE;
        }
        break;
    case WM_CLOSE: return 0; // the test closes it
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Pump(DWORD ms = 0) {
    const ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (ms) Sleep(5);
    } while (GetTickCount64() < end);
}

POINT ClientOrigin(HWND hwnd) {
    POINT p{0, 0};
    ClientToScreen(hwnd, &p);
    return p;
}

// Moves the real cursor and lets WM_SETCURSOR run.
void MoveCursor(POINT p) {
    SetCursorPos(p.x + 1, p.y);
    Pump(20);
    SetCursorPos(p.x, p.y);
    Pump(40);
}

DWORD CursorFlags(HCURSOR* shape = nullptr) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return 0xFFFFFFFF;
    if (shape) *shape = ci.hCursor;
    return ci.flags;
}

std::wstring FlagsStr(DWORD f) {
    if (f == 0xFFFFFFFF) return L"GetCursorInfo failed";
    return std::format(L"flags=0x{:X} ({}{})", f, (f & CURSOR_SHOWING) ? L"SHOWING" : L"hidden",
                       (f & CURSOR_SUPPRESSED) ? L", SUPPRESSED" : L"");
}

// ---- image helpers --------------------------------------------------------------------------------------------

struct Image {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px; // BGRA
    uint32_t& at(UINT x, UINT y) { return px[static_cast<size_t>(y) * w + x]; }
    uint32_t at(UINT x, UINT y) const { return px[static_cast<size_t>(y) * w + x]; }
};

uint32_t Bg(uint8_t v) { return 0xFF000000u | (v << 16) | (v << 8) | v; }

int ChannelDiff(uint32_t a, uint32_t b) {
    int d = 0;
    for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs(static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF)));
    return d;
}

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

// Nearest-neighbour crop + zoom for inspection.
Image CropZoom(const Image& src, int cx, int cy, int half, int zoom) {
    Image o;
    o.w = o.h = static_cast<UINT>(half * 2 * zoom);
    o.px.assign(static_cast<size_t>(o.w) * o.h, 0xFFFF00FF);
    for (UINT y = 0; y < o.h; ++y)
        for (UINT x = 0; x < o.w; ++x) {
            const int sx = cx - half + static_cast<int>(x) / zoom, sy = cy - half + static_cast<int>(y) / zoom;
            if (sx >= 0 && sy >= 0 && sx < static_cast<int>(src.w) && sy < static_cast<int>(src.h)) o.at(x, y) = src.at(sx, sy);
        }
    return o;
}

// ---- GDI reference --------------------------------------------------------------------------------------------

struct CursorGeom {
    int w = 0, h = 0, hx = 0, hy = 0;
    bool mono = false;
};

bool GetGeom(HCURSOR c, CursorGeom& g) {
    ICONINFO ii{};
    if (!GetIconInfo(c, &ii)) return false;
    BITMAP bm{};
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    g.mono = ii.hbmColor == nullptr;
    g.w = bm.bmWidth;
    g.h = g.mono ? bm.bmHeight / 2 : bm.bmHeight;
    g.hx = static_cast<int>(ii.xHotspot);
    g.hy = static_cast<int>(ii.yHotspot);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return g.w > 0 && g.h > 0;
}

// DrawIconEx(DI_NORMAL) of the cursor at its native size onto a solid background: the ground truth.
bool GdiReference(HCURSOR c, const CursorGeom& g, uint8_t bg, Image& out) {
    out.w = static_cast<UINT>(g.w);
    out.h = static_cast<UINT>(g.h);
    out.px.assign(static_cast<size_t>(g.w) * g.h, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g.w;
    bi.bmiHeader.biHeight = -g.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(dc, dib);
        auto* p = static_cast<uint32_t*>(bits);
        for (size_t i = 0; i < out.px.size(); ++i) p[i] = Bg(bg);
        ok = DrawIconEx(dc, 0, 0, c, g.w, g.h, 0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = p[i] | 0xFF000000u;
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    return ok;
}

// ---- synthetic cursors (cover the kinds no default system cursor uses) ------------------------------------------

void SetBit(std::vector<BYTE>& plane, int x, int y, bool v) { // 32 px wide 1-bpp plane, MSB = leftmost
    BYTE& b = plane[static_cast<size_t>(y) * 4 + x / 8];
    const BYTE m = static_cast<BYTE>(0x80 >> (x % 8));
    b = v ? static_cast<BYTE>(b | m) : static_cast<BYTE>(b & ~m);
}

// Monochrome 32x32, hotspot (3,4). Quadrants of a 24x24 block: TL black, TR white, BL invert, BR transparent.
HCURSOR MakeCustomMono() {
    std::vector<BYTE> andPlane(32 * 4, 0xFF), xorPlane(32 * 4, 0x00);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const bool right = x >= 12, bottom = y >= 12;
            if (!right && !bottom) { SetBit(andPlane, x, y, false); SetBit(xorPlane, x, y, false); }
            if (right && !bottom) { SetBit(andPlane, x, y, false); SetBit(xorPlane, x, y, true); }
            if (!right && bottom) { SetBit(andPlane, x, y, true); SetBit(xorPlane, x, y, true); }
        }
    return CreateCursor(GetModuleHandleW(nullptr), 3, 4, 32, 32, andPlane.data(), xorPlane.data());
}

// Masked colour 32x32 (32-bit colour bitmap, all alpha zero = legacy), hotspot (5,7). Quadrants of a 24x24 block:
// TL opaque red, TR opaque green, BL XOR white (invert), BR XOR blue.
HCURSOR MakeCustomMaskedColour() {
    std::vector<BYTE> andPlane(32 * 4, 0xFF);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32;
    bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) return nullptr;
    auto* p = static_cast<uint32_t*>(bits);
    for (int i = 0; i < 32 * 32; ++i) p[i] = 0;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const bool right = x >= 12, bottom = y >= 12;
            uint32_t c = 0;
            if (!bottom) {
                SetBit(andPlane, x, y, false);
                c = right ? 0x0000FF00u : 0x00FF0000u; // green / red (alpha 0)
            } else {
                c = right ? 0x000000FFu : 0x00FFFFFFu; // XOR blue / XOR white
            }
            p[y * 32 + x] = c;
        }
    GdiFlush();
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, andPlane.data());
    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = 5;
    ii.yHotspot = 7;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR h = mask ? CreateIconIndirect(&ii) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return h;
}

// ---- GPU render target ----------------------------------------------------------------------------------------

struct Target {
    ComPtr<ID3D11Texture2D> tex, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
};

bool MakeTarget(GpuDevice& dev, Target& t) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kRtW;
    td.Height = kRtH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = dev.Device()->CreateTexture2D(&td, nullptr, &t.tex);
    if (SUCCEEDED(hr)) hr = dev.Device()->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = dev.Device()->CreateTexture2D(&td, nullptr, &t.staging);
    if (FAILED(hr)) Print(std::format(L"render target creation failed: {}\n", HrToString(hr)));
    return SUCCEEDED(hr);
}

// Test-only CPU readback (not the frame path).
bool ReadBack(GpuDevice& dev, Target& t, Image& out) {
    auto* ctx = dev.Context();
    ctx->CopyResource(t.staging.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    const HRESULT hr = ctx->Map(t.staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        Print(std::format(L"Map(staging) failed: {}\n", HrToString(hr)));
        return false;
    }
    out.w = kRtW;
    out.h = kRtH;
    out.px.resize(static_cast<size_t>(kRtW) * kRtH);
    for (UINT y = 0; y < kRtH; ++y)
        memcpy(&out.px[static_cast<size_t>(y) * kRtW], static_cast<const BYTE*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, kRtW * 4);
    ctx->Unmap(t.staging.Get(), 0);
    return true;
}

bool Render(GpuDevice& dev, CursorOverlay& ov, Target& t, HWND hwnd, const CursorState& st, const RectI& dest,
            uint8_t bg, Image& out) {
    const float c = bg / 255.0f;
    const float clear[4] = {c, c, c, 1.0f};
    dev.Context()->ClearRenderTargetView(t.rtv.Get(), clear);
    if (!ov.Draw(st, hwnd, t.rtv.Get(), Size{kRtW, kRtH}, dest)) return false;
    // Contract: nothing left bound.
    ComPtr<ID3D11RenderTargetView> boundRtv;
    dev.Context()->OMGetRenderTargets(1, &boundRtv, nullptr);
    ID3D11ShaderResourceView* boundSrv[2] = {};
    dev.Context()->PSGetShaderResources(0, 2, boundSrv);
    const bool srvBound = boundSrv[0] || boundSrv[1];
    for (auto* s : boundSrv)
        if (s) s->Release();
    if (boundRtv || srvBound) {
        Print(L"  Draw left a render target or SRV bound\n");
        return false;
    }
    return ReadBack(dev, t, out);
}

struct Box {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN; // inclusive
    bool Empty() const { return x0 > x1; }
    void Add(int x, int y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    std::wstring Str() const { return Empty() ? L"(empty)" : std::format(L"[{},{}]-[{},{}]", x0, y0, x1, y1); }
};

// Verifies a render against the GDI reference at integer or fractional scale `s` with the hotspot mapped to (mx, my).
// Returns true if all sub-checks pass (each is printed).
void VerifyRender(const std::wstring& name, const Image& img, const Image& gdi, const CursorGeom& g, uint8_t bg,
                  double s, int mx, int my) {
    const uint32_t bgPx = Bg(bg);
    // Expected quad, same rounding as the implementation.
    const int qx0 = static_cast<int>(std::lround(mx - g.hx * s)), qy0 = static_cast<int>(std::lround(my - g.hy * s));
    const int qw = static_cast<int>(std::lround(g.w * s)), qh = static_cast<int>(std::lround(g.h * s));

    Box changed;
    size_t nChanged = 0, nFar = 0;
    const int margin = 4;
    for (UINT y = 0; y < img.h; ++y)
        for (UINT x = 0; x < img.w; ++x) {
            if (img.at(x, y) == bgPx) continue;
            ++nChanged;
            changed.Add(static_cast<int>(x), static_cast<int>(y));
            const int ix = static_cast<int>(x), iy = static_cast<int>(y);
            if (ix < qx0 - margin || iy < qy0 - margin || ix >= qx0 + qw + margin || iy >= qy0 + qh + margin) ++nFar;
        }
    Check(name + L": pixels far from the mapped hotspot untouched", nFar == 0, std::format(L"{} changed outside quad+{}px", nFar, margin));
    Check(name + L": pixels changed near the mapped hotspot", nChanged >= 20,
          std::format(L"{} changed, mapped hotspot ({},{})", nChanged, mx, my));

    Box ref;
    for (UINT y = 0; y < gdi.h; ++y)
        for (UINT x = 0; x < gdi.w; ++x)
            if (gdi.at(x, y) != bgPx) ref.Add(static_cast<int>(x), static_cast<int>(y));
    Box exp;
    if (!ref.Empty()) {
        exp.x0 = qx0 + static_cast<int>(std::floor(ref.x0 * s));
        exp.y0 = qy0 + static_cast<int>(std::floor(ref.y0 * s));
        exp.x1 = qx0 + static_cast<int>(std::ceil((ref.x1 + 1) * s)) - 1;
        exp.y1 = qy0 + static_cast<int>(std::ceil((ref.y1 + 1) * s)) - 1;
    }
    const int tol = 2;
    const bool boxOk = !changed.Empty() && !exp.Empty() && std::abs(changed.x0 - exp.x0) <= tol &&
                       std::abs(changed.y0 - exp.y0) <= tol && std::abs(changed.x1 - exp.x1) <= tol &&
                       std::abs(changed.y1 - exp.y1) <= tol;
    Check(name + L": changed bbox matches hotspot + scaled GDI shape", boxOk,
          std::format(L"got {} expected {} (cursor {}x{} hotspot ({},{}) {}, GDI bbox {}, quad {}x{} at ({},{}))",
                      changed.Str(), exp.Str(), g.w, g.h, g.hx, g.hy, g.mono ? L"mono" : L"colour", ref.Str(), qw, qh,
                      qx0, qy0));

    // Per-pixel colour + orientation: on a uniform background both blend modes are linear in the source texel, so
    // the expected output is the GDI DrawIconEx composite resampled bilinearly (clamped) exactly like the GPU does
    // (also where paint and XOR texels meet: the single dual-source pass is linear in both, see cursor_ps.hlsl).
    {
        auto bil = [&](double tx, double ty) {
            tx = std::clamp(tx, 0.0, gdi.w - 1.0);
            ty = std::clamp(ty, 0.0, gdi.h - 1.0);
            const int x0 = static_cast<int>(std::floor(tx)), y0 = static_cast<int>(std::floor(ty));
            const int x1 = std::min(x0 + 1, static_cast<int>(gdi.w) - 1), y1 = std::min(y0 + 1, static_cast<int>(gdi.h) - 1);
            const double fx = tx - x0, fy = ty - y0;
            uint32_t r = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) {
                auto ch = [&](int x, int y) { return static_cast<double>((gdi.at(x, y) >> sh) & 0xFF); };
                const double v = (ch(x0, y0) * (1 - fx) + ch(x1, y0) * fx) * (1 - fy) + (ch(x0, y1) * (1 - fx) + ch(x1, y1) * fx) * fy;
                r |= static_cast<uint32_t>(std::lround(v)) << sh;
            }
            return r;
        };
        size_t compared = 0, nonBg = 0, mismatched = 0;
        int maxDiff = 0;
        for (int oy = std::max(qy0, 0); oy < std::min(qy0 + qh, static_cast<int>(img.h)); ++oy)
            for (int ox = std::max(qx0, 0); ox < std::min(qx0 + qw, static_cast<int>(img.w)); ++ox) {
                const double tx = (ox - qx0 + 0.5) * g.w / qw - 0.5, ty = (oy - qy0 + 0.5) * g.h / qh - 0.5;
                const uint32_t e = bil(tx, ty);
                ++compared;
                if (e != bgPx) ++nonBg;
                const int d = ChannelDiff(img.at(ox, oy), e);
                maxDiff = std::max(maxDiff, d);
                if (d > 3) ++mismatched;
            }
        const bool ok = nonBg > 0 && mismatched == 0;
        Check(name + L": colours match bilinear-resampled GDI DrawIconEx", ok,
              std::format(L"{} px compared ({} non-background), {} differ by >3, max diff {}", compared, nonBg,
                          mismatched, maxDiff));
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo)) {
        Print(std::format(L"CoInitializeEx failed: {}\n", HrToString(hrCo)));
        return 1;
    }
    InitLog(L"cursor_smoke.log");

    const std::filesystem::path outDir = std::filesystem::path(MOONSR_SOURCE_DIR) / L"benchmarks" / L"output" / L"cursor";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::wstring err;
    auto dev = GpuDevice::Create(nullptr, err);
    if (!dev) {
        Print(L"GpuDevice::Create failed: " + err + L"\n");
        return 1;
    }
    Print(std::format(L"Adapter: {}\n", dev->Info().name));
    auto overlay = CursorOverlay::Create(*dev, err);
    Check(L"CursorOverlay::Create", overlay != nullptr, err);
    if (!overlay) return 1;

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) Print(std::format(L"WIC unavailable ({}); PNGs will not be written\n", HrToString(hr)));

    POINT originalPos{};
    GetCursorPos(&originalPos);

    // ---- test window -------------------------------------------------------------------------------------------
    g_cursor = LoadCursorW(nullptr, IDC_ARROW);
    HBRUSH brush = CreateSolidBrush(RGB(kGrey, kGrey, kGrey));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = brush;
    wc.lpszClassName = L"MoonSRCursorSmoke";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_TOPMOST;
    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"MoonSR cursor smoke", style, 200, 200, kClientW, kClientH,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        Print(L"CreateWindowEx failed\n");
        return 1;
    }
    RECT wr{0, 0, kClientW, kClientH};
    AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, HWND_TOPMOST, 200, 200, wr.right - wr.left, wr.bottom - wr.top, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    Pump(150);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    Check(L"test window client is 640x360", rc.right == kClientW && rc.bottom == kClientH,
          std::format(L"{}x{}, DPI {}", rc.right, rc.bottom, GetDpiForWindow(hwnd)));
    POINT org = ClientOrigin(hwnd);
    Print(std::format(L"Client origin (screen, physical px): ({},{})\n", org.x, org.y));

    // ---- 1. Poll -----------------------------------------------------------------------------------------------
    const POINT inClient{200, 150};
    MoveCursor({org.x + inClient.x, org.y + inClient.y});
    CursorState st = overlay->Poll(hwnd);
    Check(L"Poll inside client: visible", st.visible);
    Check(L"Poll inside client: position", st.screenPos.x == org.x + inClient.x && st.screenPos.y == org.y + inClient.y,
          std::format(L"got ({},{}) expected ({},{})", st.screenPos.x, st.screenPos.y, org.x + inClient.x, org.y + inClient.y));
    {
        CursorState st2 = overlay->Poll(hwnd);
        Check(L"Poll is stable while the cursor does not move", st2 == st);
    }
    MoveCursor({org.x - 40, org.y + 150});
    st = overlay->Poll(hwnd);
    Check(L"Poll left of window: not visible", !st.visible);
    MoveCursor({org.x + 200, org.y - 5});
    st = overlay->Poll(hwnd);
    Check(L"Poll over caption (non-client): not visible", !st.visible);
    MoveCursor({org.x + kClientW - 1, org.y + kClientH - 1});
    st = overlay->Poll(hwnd);
    Check(L"Poll at last client pixel: visible", st.visible);
    MoveCursor({org.x + kClientW, org.y + kClientH - 1});
    st = overlay->Poll(hwnd);
    Check(L"Poll one pixel right of client: not visible", !st.visible);
    MoveCursor({org.x + inClient.x, org.y + inClient.y});
    ShowWindow(hwnd, SW_MINIMIZE);
    Pump(300);
    st = overlay->Poll(hwnd);
    Check(L"Poll with target minimised: not visible", !st.visible);
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    Pump(300);
    org = ClientOrigin(hwnd);

    // ---- 2. Draw per cursor ------------------------------------------------------------------------------------
    Target target;
    if (!MakeTarget(*dev, target)) return 1;
    const RectI full{0, 0, static_cast<int32_t>(kRtW), static_cast<int32_t>(kRtH)};

    struct Named {
        const wchar_t* name;
        HCURSOR h;
        bool multiBg; // also test dark and light backgrounds (invert path)
    };
    const HCURSOR customMono = MakeCustomMono();
    const HCURSOR customMasked = MakeCustomMaskedColour();
    Check(L"synthetic cursors created", customMono && customMasked);
    const Named cursors[] = {{L"arrow", LoadCursorW(nullptr, IDC_ARROW), false},
                             {L"ibeam", LoadCursorW(nullptr, IDC_IBEAM), true},
                             {L"hand", LoadCursorW(nullptr, IDC_HAND), false},
                             {L"cross", LoadCursorW(nullptr, IDC_CROSS), false},
                             {L"sizeall", LoadCursorW(nullptr, IDC_SIZEALL), false},
                             {L"wait", LoadCursorW(nullptr, IDC_WAIT), false},
                             {L"synthetic_mono", customMono, true},
                             {L"synthetic_masked_colour", customMasked, true}};
    for (const auto& c : cursors) {
        const HCURSOR h = c.h;
        if (!h) continue;
        g_cursor = h;
        MoveCursor({org.x + inClient.x, org.y + inClient.y});
        SetCursor(h);
        Pump(30);
        st = overlay->Poll(hwnd);
        Check(std::format(L"{}: GetCursorInfo reports the class cursor", c.name), st.visible && st.shape == h,
              std::format(L"visible={} shape={} expected={}", st.visible, static_cast<const void*>(st.shape),
                          static_cast<const void*>(h)));
        if (!st.visible) continue;
        CursorGeom g;
        if (!GetGeom(h, g)) {
            Check(std::format(L"{}: GetIconInfo", c.name), false);
            continue;
        }
        const int mx = 2 * inClient.x, my = 2 * inClient.y;
        const uint8_t bgs[] = {kGrey, 16, 240};
        const size_t nBg = c.multiBg ? 3 : 1;
        for (size_t b = 0; b < nBg; ++b) {
            const uint8_t bg = bgs[b];
            Image img, gdi;
            const bool ok = Render(*dev, *overlay, target, hwnd, st, full, bg, img);
            Check(std::format(L"{} bg{}: Draw", c.name, bg), ok);
            if (!ok) continue;
            GdiReference(h, g, bg, gdi);
            VerifyRender(std::format(L"{} bg{}", c.name, bg), img, gdi, g, bg, 2.0, mx, my);
            if (c.multiBg && bg != kGrey) {
                // Invert path: on dark backgrounds the beam must be lighter, on light ones darker.
                size_t lighter = 0, darker = 0;
                for (uint32_t p : img.px) {
                    const int v = static_cast<int>((p >> 8) & 0xFF);
                    if (v > bg) ++lighter;
                    if (v < bg) ++darker;
                }
                if (bg < 128)
                    Check(std::format(L"{}: visible on dark background (lighter pixels)", c.name), lighter >= 20,
                          std::format(L"{} lighter, {} darker", lighter, darker));
                else
                    Check(std::format(L"{}: visible on light background (darker pixels)", c.name), darker >= 20,
                          std::format(L"{} lighter, {} darker", lighter, darker));
            }
            if (wic) {
                SavePng(wic.Get(), outDir / std::format(L"{}_bg{}.png", c.name, bg), img);
                SavePng(wic.Get(), outDir / std::format(L"{}_bg{}_crop.png", c.name, bg), CropZoom(img, mx, my, 64, 4));
                if (b == 0) SavePng(wic.Get(), outDir / std::format(L"{}_gdi_reference_x8.png", c.name), CropZoom(gdi, g.w / 2, g.h / 2, std::max(g.w, g.h) / 2, 8));
            }
        }
    }

    // Letterboxed, non-integer scale: destRect 960x540 at (100,60) -> 1.5x.
    {
        const HCURSOR h = LoadCursorW(nullptr, IDC_ARROW);
        g_cursor = h;
        MoveCursor({org.x + inClient.x, org.y + inClient.y});
        SetCursor(h);
        Pump(30);
        st = overlay->Poll(hwnd);
        CursorGeom g;
        Image img, gdi;
        if (st.visible && GetGeom(h, g) && Render(*dev, *overlay, target, hwnd, st, RectI{100, 60, 1060, 600}, kGrey, img)) {
            GdiReference(h, g, kGrey, gdi);
            VerifyRender(L"arrow letterbox 1.5x", img, gdi, g, kGrey, 1.5, 100 + 300, 60 + 225);
            if (wic) SavePng(wic.Get(), outDir / L"arrow_letterbox_1p5x.png", img);
        } else {
            Check(L"arrow letterbox 1.5x: render", false);
        }
        // Overhang at the bottom-right corner: must not fail; changes only inside the target.
        MoveCursor({org.x + kClientW - 1, org.y + kClientH - 1});
        st = overlay->Poll(hwnd);
        Check(L"arrow at bottom-right corner (overhang): Draw", st.visible && Render(*dev, *overlay, target, hwnd, st, full, kGrey, img));
        if (wic && !img.px.empty()) SavePng(wic.Get(), outDir / L"arrow_corner_overhang.png", img);
        // GDI leak check: every shape switch re-validates the cache entry with GetIconInfo (which creates bitmaps).
        {
            MoveCursor({org.x + inClient.x, org.y + inClient.y});
            CursorState a = overlay->Poll(hwnd), b = a;
            a.shape = LoadCursorW(nullptr, IDC_ARROW);
            b.shape = LoadCursorW(nullptr, IDC_HAND);
            Image tmp;
            Render(*dev, *overlay, target, hwnd, a, full, kGrey, tmp);
            Render(*dev, *overlay, target, hwnd, b, full, kGrey, tmp);
            const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            bool allOk = true;
            for (int i = 0; i < 200; ++i) {
                allOk &= overlay->Draw(a, hwnd, target.rtv.Get(), Size{kRtW, kRtH}, full);
                allOk &= overlay->Draw(b, hwnd, target.rtv.Get(), Size{kRtW, kRtH}, full);
            }
            const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            Check(L"no GDI object leak over 400 shape switches", allOk && gdiAfter == gdiBefore,
                  std::format(L"GDI objects {} -> {}", gdiBefore, gdiAfter));
        }
        // Invisible state draws nothing.
        CursorState invisible;
        Check(L"Draw with !visible leaves target untouched",
              Render(*dev, *overlay, target, hwnd, invisible, full, kGrey, img) &&
                  std::all_of(img.px.begin(), img.px.end(), [](uint32_t p) { return p == Bg(kGrey); }));
    }

    // ---- 3. System cursor hiding -------------------------------------------------------------------------------
    MoveCursor({org.x + inClient.x, org.y + inClient.y});
    Print(std::format(L"System cursor before hide: {}\n", FlagsStr(CursorFlags())));
    overlay->SetSystemCursorHidden(true);
    overlay->SetSystemCursorHidden(true); // idempotent
    Pump(100);
    const DWORD fHidden = CursorFlags();
    st = overlay->Poll(hwnd);
    Print(std::format(L"System cursor after SetSystemCursorHidden(true): {}; Poll visible={}\n", FlagsStr(fHidden), st.visible));
    MoveCursor({org.x + inClient.x + 10, org.y + inClient.y + 10});
    Print(std::format(L"  after moving the cursor while hidden: {}; Poll visible={}\n", FlagsStr(CursorFlags()), overlay->Poll(hwnd).visible));
    Check(L"Poll keeps reporting visible while the system cursor is hidden by us", overlay->Poll(hwnd).visible);
    overlay->SetSystemCursorHidden(false);
    overlay->SetSystemCursorHidden(false);
    Pump(100);
    const DWORD fShown = CursorFlags();
    Print(std::format(L"System cursor after SetSystemCursorHidden(false): {}\n", FlagsStr(fShown)));
    Check(L"system cursor showing after unhide", fShown != 0xFFFFFFFF && (fShown & CURSOR_SHOWING));

    overlay->SetSystemCursorHidden(true);
    Pump(50);
    overlay.reset(); // destructor must restore
    Pump(100);
    const DWORD fAfterDtor = CursorFlags();
    Print(std::format(L"System cursor after destroying a hiding overlay: {}\n", FlagsStr(fAfterDtor)));
    Check(L"destructor restores the system cursor", fAfterDtor != 0xFFFFFFFF && (fAfterDtor & CURSOR_SHOWING));

    // ---- cleanup -----------------------------------------------------------------------------------------------
    DestroyWindow(hwnd);
    SetCursorPos(originalPos.x, originalPos.y);
    Pump(50);
    if (customMono) DestroyCursor(customMono);
    if (customMasked) DestroyCursor(customMasked);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    DeleteObject(brush);
    wic.Reset();
    target = Target{};
    dev.reset();
    CoUninitialize();

    Print(std::format(L"PNG output: {}\n", outDir.wstring()));
    Print(g_failures == 0 ? L"RESULT: PASS\n" : std::format(L"RESULT: FAIL ({} check(s) failed)\n", g_failures));
    return g_failures == 0 ? 0 : 1;
}
