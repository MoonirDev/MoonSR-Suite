// SPDX-License-Identifier: GPL-3.0-only
// Exercises the bridge's Intel backend (intel_model.cpp) with the d5c_chain stand-in (intel_fixture.cpp):
// cropped capture input, bit-exact output through readback/upload/handoff, picture adjustments, live settings,
// history reset, resize, the Vulkan driver filter and its restoration.
//
//   MoonSR_IntelNrSmoke.exe [adapter name substring]      (any D3D11 hardware adapter; no Intel GPU needed)
#include "moonsr_nr_bridge.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
int g_failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
void Hr(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(what); }
uint32_t Pixel(unsigned x, unsigned y, unsigned frame) {
    return 0xff000000u | ((x * 7 + frame * 13) & 255) << 16 | ((y * 5) & 255) << 8 | ((x ^ y) & 255);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto dir = std::filesystem::path(exe).parent_path();
        const std::wstring fixture = (dir / L"MoonSR_IntelNrFixture.dll").wstring();
        HMODULE bridgeDll = LoadLibraryW((dir / L"MoonSR_NrBridge.dll").c_str());
        if (!bridgeDll) throw std::runtime_error("Load MoonSR_NrBridge.dll");
        const auto getApi = reinterpret_cast<MoonSrNrBridgeGetApiFn>(GetProcAddress(bridgeDll, "MoonSR_NrBridge_GetApi"));
        const auto update = reinterpret_cast<MoonSrNrBridgeUpdateSettingsFn>(
            GetProcAddress(bridgeDll, "MoonSR_NrBridge_UpdateSettings"));
        const MoonSrNrBridgeApi* api = getApi ? getApi(MOONSR_NR_BRIDGE_API_VERSION) : nullptr;
        if (!api || !update) throw std::runtime_error("Bridge API");

        ComPtr<IDXGIFactory6> factory;
        Hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (argc < 2 || std::wstring_view(d.Description).find(argv[1]) != std::wstring_view::npos)) {
                std::printf("D3D11 adapter: %ls (the Intel backend's D3D11 side is vendor-neutral)\n", d.Description);
                break;
            }
            adapter.Reset();
        }
        if (!adapter) throw std::runtime_error("No adapter");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                             0, D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11 device");

        const bool hadFilter = GetEnvironmentVariableW(L"VK_LOADER_DRIVERS_SELECT", nullptr, 0) > 0;
        MoonSrNrBridgeConfig cfg{};
        cfg.size = sizeof(cfg); cfg.apiVersion = MOONSR_NR_BRIDGE_API_VERSION;
        cfg.d3d11Device = device.Get(); cfg.d3d11Context = context.Get();
        cfg.modelPath = fixture.c_str(); cfg.adapterVendorId = 0x8086;
        cfg.passes = 1; cfg.resolutionPercent = 100; cfg.intensityPercent = 120; cfg.structurePercent = 80;
        cfg.tonePercent = 30; cfg.skinPercent = 70; cfg.detailPercent = 100; cfg.colourPercent = 150; cfg.autoMask = 1;
        cfg.style = 2;
        void* bridge = nullptr;
        const wchar_t* error = nullptr;
        if (api->Create(&cfg, &bridge, &error) != MOONSR_NR_BRIDGE_OK) {
            std::printf("Create: %ls\n", error ? error : L"?");
            return 1;
        }
        // The bridge creates its Intel backend (and loads the runtime) at the first frame, so look the fixture up lazily.
        auto report = []() -> const char* {
            HMODULE m = GetModuleHandleW(L"MoonSR_IntelNrFixture.dll");
            const auto fn = m ? reinterpret_cast<const char* (*)()>(GetProcAddress(m, "intel_fixture_report")) : nullptr;
            return fn ? fn() : "(fixture not loaded)";
        };

        auto run = [&](unsigned outerW, unsigned outerH, int left, int top, unsigned w, unsigned h, unsigned frame,
                       bool reset, bool pictureOn, std::vector<uint32_t>& out) {
            std::vector<uint32_t> pixels(size_t(outerW) * outerH);
            for (unsigned y = 0; y < outerH; ++y)
                for (unsigned x = 0; x < outerW; ++x) pixels[size_t(y) * outerW + x] = Pixel(x, y, frame);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = outerW; d.Height = outerH; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{pixels.data(), outerW * 4, 0};
            ComPtr<ID3D11Texture2D> source;
            Hr(device->CreateTexture2D(&d, &data, &source), "Source");
            MoonSrNrBridgeFrame f{};
            f.size = sizeof(f); f.texture = source.Get();
            f.left = left; f.top = top; f.right = left + int(w); f.bottom = top + int(h);
            f.sequence = frame; f.reset = reset ? 1u : 0u;
            f.pictureEnabled = pictureOn ? 1u : 0u; f.saturation = 100; f.exposure = pictureOn ? 100 : 0;
            MoonSrNrBridgeOutput o{};
            o.size = sizeof(o);
            if (api->Process(bridge, &f, &o, &error) != MOONSR_NR_BRIDGE_OK) {
                std::printf("Process: %ls\n", error ? error : L"?");
                return false;
            }
            auto* tex = static_cast<ID3D11Texture2D*>(o.texture);
            D3D11_TEXTURE2D_DESC od{};
            tex->GetDesc(&od);
            od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            Hr(device->CreateTexture2D(&od, nullptr, &staging), "Readback");
            context->CopyResource(staging.Get(), tex);
            D3D11_MAPPED_SUBRESOURCE m{};
            Hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
            out.assign(size_t(w) * h, 0);
            for (unsigned y = 0; y < h; ++y)
                std::memcpy(out.data() + size_t(y) * w, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, w * 4);
            context->Unmap(staging.Get(), 0);
            return o.right - o.left == int(w) && o.bottom - o.top == int(h);
        };
        auto inverseMatches = [&](const std::vector<uint32_t>& out, int left, int top, unsigned w, unsigned h,
                                  unsigned frame) {
            unsigned worst = 0;
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const uint32_t want = Pixel(x + left, y + top, frame) ^ 0x00ffffffu;
                    const uint32_t got = out[size_t(y) * w + x];
                    for (int c = 0; c < 24; c += 8) {
                        const int dlt = int((want >> c) & 255) - int((got >> c) & 255);
                        worst = std::max<unsigned>(worst, unsigned(dlt < 0 ? -dlt : dlt));
                    }
                }
            return worst;
        };

        std::vector<uint32_t> out;
        bool ok = run(336, 216, 8, 4, 320, 200, 1, true, false, out);
        Expect(ok, "frame 1 (cropped 320x200 from 336x216) processed, output rectangle matches");
        Expect(ok && inverseMatches(out, 8, 4, 320, 200, 1) == 0, "frame 1 output is the runtime's result, bit-exact");
        ok = run(336, 216, 8, 4, 320, 200, 2, false, false, out);
        Expect(ok && inverseMatches(out, 8, 4, 320, 200, 2) == 0, "frame 2 bit-exact (no stale frame)");
        ok = run(336, 216, 8, 4, 320, 200, 3, false, true, out);
        Expect(ok && inverseMatches(out, 8, 4, 320, 200, 3) > 8, "picture adjustment (+1 stop) changes the output");
        std::string r = report();
        std::printf("  fixture: %s\n", r.c_str());
        Expect(r.find("frames 3 ") != std::string::npos && r.find("resets 1 ") != std::string::npos,
               "runtime saw 3 frames and 1 history reset");
        Expect(r.find("intensity 1.20 tone 0.30 structure 0.80 skin 0.70 mask 1 style 2 colour 1.50") != std::string::npos,
               "Phase 30 settings reach the runtime");
        Expect(r.find("dlssnr-logical.safetensors") != std::string::npos, "weights path is intel-nr's safetensors");
        wchar_t filter[64]{};
        GetEnvironmentVariableW(L"VK_LOADER_DRIVERS_SELECT", filter, 64);
        std::printf("  VK_LOADER_DRIVERS_SELECT while active: %ls\n", filter);

        cfg.resolutionPercent = 50; cfg.style = 1; cfg.autoMask = 0;
        Expect(update(bridge, &cfg, &error) == MOONSR_NR_BRIDGE_OK, "live settings accepted");
        ok = run(336, 216, 8, 4, 320, 200, 4, false, false, out);
        r = report();
        Expect(ok && r.find("scale 0.50") != std::string::npos && r.find("skin -1.00 mask 0 style 1") != std::string::npos &&
               r.find("frames 4 ") != std::string::npos, "live scale/style/mask applied without a rebuild");

        ok = run(640, 360, 0, 0, 640, 360, 5, false, false, out);
        Expect(ok && inverseMatches(out, 0, 0, 640, 360, 5) == 0, "resize to 640x360 (uncropped) bit-exact");

        api->Destroy(bridge);
        Expect(hadFilter || GetEnvironmentVariableW(L"VK_LOADER_DRIVERS_SELECT", nullptr, 0) == 0,
               "Vulkan driver filter removed after destroy");
        std::printf(g_failures ? "Intel NR smoke FAILED (%d)\n" : "Intel NR smoke passed\n", g_failures);
        return g_failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::printf("Intel NR smoke error: %s\n", e.what());
        return 2;
    }
}
