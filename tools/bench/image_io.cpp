#include "imageops.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace bench {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

namespace {
// The factory is intentionally never released: a thread_local ComPtr would be destroyed after CoUninitialize at exit.
ComPtr<IWICImagingFactory> Factory() {
    static thread_local IWICImagingFactory* f = nullptr;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}
std::string Hr(const char* what, HRESULT hr) {
    char b[96];
    snprintf(b, sizeof(b), "%s failed (0x%08X)", what, static_cast<unsigned>(hr));
    return b;
}
} // namespace

bool ReadPng(const std::wstring& path, Image& out, std::string& err) {
    auto f = Factory();
    if (!f) { err = "WIC factory unavailable (CoInitialize missing?)"; return false; }
    ComPtr<IWICBitmapDecoder> dec;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) { err = Hr("CreateDecoderFromFilename", hr) + " for " + Narrow(path); return false; }
    ComPtr<IWICBitmapFrameDecode> fr;
    hr = dec->GetFrame(0, &fr);
    if (FAILED(hr)) { err = Hr("GetFrame", hr); return false; }
    ComPtr<IWICFormatConverter> conv;
    f->CreateFormatConverter(&conv);
    hr = conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { err = Hr("FormatConverter::Initialize", hr); return false; }
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    out.Alloc(w, h);
    hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size()), out.px.data());
    if (FAILED(hr)) { err = Hr("CopyPixels", hr); return false; }
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    return true;
}

bool WritePng(const std::wstring& path, const Image& img, std::string& err) {
    auto f = Factory();
    if (!f) { err = "WIC factory unavailable"; return false; }
    ComPtr<IWICStream> stream;
    f->CreateStream(&stream);
    HRESULT hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { err = Hr("InitializeFromFilename", hr) + " for " + Narrow(path); return false; }
    ComPtr<IWICBitmapEncoder> enc;
    hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> fr;
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
    if (FAILED(hr)) { err = Hr("PNG encoder setup", hr); return false; }
    std::vector<uint8_t> rgb(static_cast<size_t>(img.w) * img.h * 3);
    for (size_t i = 0, n = static_cast<size_t>(img.w) * img.h; i < n; ++i) {
        rgb[i * 3 + 0] = img.px[i * 4 + 0];
        rgb[i * 3 + 1] = img.px[i * 4 + 1];
        rgb[i * 3 + 2] = img.px[i * 4 + 2];
    }
    hr = fr->WritePixels(img.h, img.w * 3, static_cast<UINT>(rgb.size()), rgb.data());
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) { err = Hr("PNG write", hr); return false; }
    return true;
}

} // namespace bench
