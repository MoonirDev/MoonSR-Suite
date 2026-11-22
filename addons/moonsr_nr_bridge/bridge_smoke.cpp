// SPDX-License-Identifier: GPL-3.0-only
// Exercises the actual bridge with the reference's identity-model fixture on NVIDIA hardware.
#include "moonsr_nr_bridge.h"
#include "model_runtime_fixture.h"
#include "engine/gpu_priority.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <string_view>
#include <algorithm>
#include <chrono>
using Microsoft::WRL::ComPtr;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void hrcheck(HRESULT hr, const char* message) { check(SUCCEEDED(hr), message); }
int wmain(int argc, wchar_t** argv) {
    // --priority applies MoonSR's GPU priority policy (engine/gpu_priority.h) to the capture device, as the engine does.
    bool priority = false;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--priority") { priority = true; --argc; ++argv; }
    const bool timing = argc > 2 && std::wstring_view(argv[1]) == L"--timing";
    if (timing) { --argc; ++argv; }
    HMODULE module = nullptr;
    HMODULE fixture = nullptr;
    void* instance = nullptr;
    const MoonSrNrBridgeApi* api = nullptr;
    try {
        wchar_t executable[32768]{};
        check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Get executable path");
        auto dir = std::filesystem::path(executable).parent_path();
        auto dll = dir / L"MoonSR_NrBridge.dll";
        const bool amdFixture = argc > 1 && std::wstring_view(argv[1]) == L"--amd-fixture";
        const bool amd = amdFixture || (argc > 1 && std::wstring_view(argv[1]) == L"--amd");
        const int modelArg = amd ? 2 : 1;
        check(argc <= modelArg + 2 && (!amd || argc > modelArg),
              "Usage: MoonSR_NrBridgeSmoke.exe [--priority] [--timing] [--amd] [runtime path [adapter name substring]]");
        const bool usingFixture = argc == 1;
        auto model = usingFixture ? (dir / L"MoonSR_NrFixture.dll").wstring() : std::filesystem::absolute(argv[modelArg]).wstring();
        int (*configure)(unsigned, unsigned) = nullptr;
        unsigned (*live)() = nullptr;
        unsigned (*evaluates)() = nullptr;
        const FixtureEvaluation* (*trace)() = nullptr;
        if (usingFixture) {
            fixture = LoadLibraryW(model.c_str());
            check(fixture != nullptr, "Load model fixture");
            configure = reinterpret_cast<decltype(configure)>(GetProcAddress(fixture, "neural_fixture_configure"));
            live = reinterpret_cast<decltype(live)>(GetProcAddress(fixture, "neural_fixture_live"));
            evaluates = reinterpret_cast<decltype(evaluates)>(GetProcAddress(fixture, "neural_fixture_evaluates"));
            trace = reinterpret_cast<decltype(trace)>(GetProcAddress(fixture, "neural_fixture_trace"));
            check(configure && live && evaluates && trace, "Resolve fixture exports");
        }
        module = LoadLibraryW(dll.c_str());
        check(module != nullptr, "Load bridge");
        auto getApi = reinterpret_cast<MoonSrNrBridgeGetApiFn>(GetProcAddress(module, "MoonSR_NrBridge_GetApi"));
        check(getApi && (api = getApi(MOONSR_NR_BRIDGE_API_VERSION)), "Get bridge API");
        auto updateSettings = reinterpret_cast<MoonSrNrBridgeUpdateSettingsFn>(
            GetProcAddress(module, "MoonSR_NrBridge_UpdateSettings"));
        check(updateSettings != nullptr, "Get live settings export");
        ComPtr<IDXGIFactory6> factory;
        hrcheck(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                 IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (desc.VendorId == (amd ? 0x1002u : 0x10deu) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (argc <= modelArg + 1 || std::wstring_view(desc.Description).find(argv[modelArg + 1]) != std::wstring_view::npos)) {
                printf("Testing on %ls\n", desc.Description);
                break;
            }
            adapter.Reset();
        }
        check(adapter != nullptr, amd ? "Find AMD adapter" : "Find NVIDIA adapter");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        hrcheck(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                  nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Create capture device");
        if (priority) {
            const auto p = moonsr::RaiseGpuPriority(device.Get());
            printf("GPU priority: process class %ls (status 0x%08lx), device thread priority %d (hr 0x%08lx)\n",
                   moonsr::GpuSchedulingClassName(p.processClass), static_cast<unsigned long>(p.processStatus),
                   p.deviceThreadPriority,
                   static_cast<unsigned long>(p.deviceHr));
        }
        MoonSrNrBridgeConfig cfg{}; cfg.size = sizeof(cfg); cfg.apiVersion = MOONSR_NR_BRIDGE_API_VERSION;
        cfg.d3d11Device = device.Get(); cfg.d3d11Context = context.Get(); cfg.modelPath = model.c_str();
        cfg.adapterVendorId = amd ? 0x1002 : 0x10de;
        cfg.passes = 1; cfg.resolutionPercent = 100; cfg.intensityPercent = 100; cfg.structurePercent = 80;
        cfg.tonePercent = 30; cfg.skinPercent = 80; cfg.detailPercent = cfg.colourPercent = 100; cfg.autoMask = 1;
        const wchar_t* error = nullptr;
        if (timing) {
            constexpr unsigned width = 1280, height = 720, warmup = 24, count = 160;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> images[2], readback;
            for (unsigned frame = 0; frame < 2; ++frame) {
                std::vector<uint32_t> pixels(width * height);
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = 0; x < width; ++x)
                        pixels[y * width + x] = 0xff000000u | (((x + frame * 2) % 224 + 16) << 16) |
                                                ((y % 224 + 16) << 8) | (x / 8 % 128 + 64);
                D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
                hrcheck(device->CreateTexture2D(&desc, &data, &images[frame]), "Create timing input");
            }
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hrcheck(device->CreateTexture2D(&desc, nullptr, &readback), "Create timing readback");
            if (api->Create(&cfg, &instance, &error) != MOONSR_NR_BRIDGE_OK) {
                fwprintf(stderr, L"Timing create: %ls\n", error ? error : L"unknown");
                throw std::runtime_error("Timing create failed");
            }
            std::vector<double> elapsed;
            elapsed.reserve(count);
            for (unsigned i = 0; i < warmup + count; ++i) {
                MoonSrNrBridgeFrame frame{};
                frame.size = sizeof(frame);
                frame.texture = images[i % 2].Get();
                frame.right = width;
                frame.bottom = height;
                frame.sequence = i + 1;
                frame.reset = i == 0;
                MoonSrNrBridgeOutput out{};
                out.size = sizeof(out);
                const auto start = std::chrono::steady_clock::now();
                if (api->Process(instance, &frame, &out, &error) != MOONSR_NR_BRIDGE_OK) {
                    fwprintf(stderr, L"Timing frame %u: %ls\n", i, error ? error : L"unknown");
                    throw std::runtime_error("Timing process failed");
                }
                check(out.texture != nullptr, "Timing frame has no output");
                // A one-pixel readback waits for completed output without copying the whole frame.
                const D3D11_BOX pixel{0, 0, 0, 1, 1, 1};
                context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0,
                                               static_cast<ID3D11Texture2D*>(out.texture), 0, &pixel);
                D3D11_MAPPED_SUBRESOURCE map{};
                hrcheck(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Wait for timing output");
                context->Unmap(readback.Get(), 0);
                if (i >= warmup)
                    elapsed.push_back(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count());
            }
            std::sort(elapsed.begin(), elapsed.end());
            printf("%ux%u, %u frames, one pass: p50 %.3f ms, p95 %.3f ms, max %.3f ms\n",
                   width, height, count, elapsed[count / 2 - 1], elapsed[count * 95 / 100 - 1], elapsed.back());
            api->Destroy(instance);
            instance = nullptr;
            FreeLibrary(module);
            return 0;
        }
        uint64_t defaultStyleDigest = 0, originalDigest = 0;
        unsigned amdAltered = 0;
        for (unsigned scenario = 0; scenario < ((usingFixture || amd) ? 4u : 7u); ++scenario) {
        const unsigned passes = scenario == 1 ? 3 : scenario == 2 ? 20 : 1;
        if (usingFixture) check(configure(0, 0) == 1, "Reset model fixture");
        cfg.passes = passes;
        cfg.resolutionPercent = passes == 1 ? 100 : 50;
        cfg.preset = scenario == 5 ? 1 : scenario == 6 ? 2 : scenario == 2 ? 2 : passes == 3 ? 1 : 0;
        cfg.style = scenario == 4 ? 2 : passes == 3 ? 1 : 0;
        cfg.tonePercent = 45;
        cfg.detailPercent = cfg.colourPercent = scenario == 3 ? 0 : 100;
        if (api->Create(&cfg, &instance, &error) != MOONSR_NR_BRIDGE_OK) {
            fwprintf(stderr, L"Create: %ls\n", error ? error : L"unknown"); throw std::runtime_error("Create failed");
        }
        for (unsigned frameIndex = 0; frameIndex < 4; ++frameIndex) {
            if (usingFixture && scenario == 0 && frameIndex == 1) {
                cfg.tonePercent = 65;
                check(updateSettings(instance, &cfg, &error) == MOONSR_NR_BRIDGE_OK, "Update live appearance");
                auto invalid = cfg;
                invalid.resolutionPercent = 0;
                check(updateSettings(instance, &invalid, &error) != MOONSR_NR_BRIDGE_OK, "Reject invalid live settings");
            }
            if ((usingFixture || amd) && scenario == 0 && frameIndex == 2) {
                cfg.resolutionPercent = 50;
                cfg.passes = 2;
                check(updateSettings(instance, &cfg, &error) == MOONSR_NR_BRIDGE_OK, "Update live model resources");
            }
            const unsigned width = frameIndex == 3 ? 352 : 320, height = 240;
            std::vector<unsigned char> pixels(width * height * 4);
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
                size_t i = (size_t(y) * width + x) * 4;
                pixels[i] = static_cast<unsigned char>((x + frameIndex * 3) % 180 + 30);
                pixels[i + 1] = static_cast<unsigned char>(y % 160 + 40);
                pixels[i + 2] = static_cast<unsigned char>((x + y) % 160 + 40); pixels[i + 3] = 255;
            }
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0}; ComPtr<ID3D11Texture2D> source;
            if (usingFixture && frameIndex == 0) desc.BindFlags = 0; // Exercise the bridge's copy fallback.
            hrcheck(device->CreateTexture2D(&desc, &data, &source), "Create source");
            MoonSrNrBridgeFrame frame{}; frame.size = sizeof(frame); frame.texture = source.Get();
            ComPtr<ID3D11Texture2D> separateGuide;
            if (usingFixture && frameIndex == 1) {
                hrcheck(device->CreateTexture2D(&desc, &data, &separateGuide), "Create separate flow guide");
                frame.flowGuide = separateGuide.Get();
            }
            frame.right = width; frame.bottom = height; frame.sequence = frameIndex + 1; frame.reset = frameIndex == 0 || frameIndex == 3;
            frame.saturation = 100;
            if (scenario == 3 && (frameIndex == 1 || frameIndex == 2)) frame.pictureEnabled = 1;
            if (scenario == 3 && frameIndex == 2) frame.exposure = 100;
            MoonSrNrBridgeOutput out{}; out.size = sizeof(out);
            if (api->Process(instance, &frame, &out, &error) != MOONSR_NR_BRIDGE_OK) {
                fwprintf(stderr, L"Process frame %u: %ls\n", frameIndex, error ? error : L"unknown"); throw std::runtime_error("Process failed");
            }
            if (usingFixture) {
                const unsigned activePasses = cfg.passes;
                const unsigned expectedCalls = scenario == 0 && frameIndex >= 2 ?
                    2 + (frameIndex - 1) * activePasses : (frameIndex + 1) * activePasses;
                check(live() == activePasses, "Incorrect active pass count");
                check(evaluates() == expectedCalls, "Incorrect evaluation count");
                const auto* calls = trace() + expectedCalls - activePasses;
                check(std::abs(calls[0].localTone - cfg.tonePercent / 100.0f) < 0.0001f,
                      "Model tone setting was not applied");
                check(calls[0].colour->GetDesc().Width == width * cfg.resolutionPercent / 100 &&
                      calls[0].colour->GetDesc().Height == height * cfg.resolutionPercent / 100, "Wrong working resolution");
                for (unsigned pass = 1; pass < activePasses; ++pass)
                    check(calls[pass].colour == calls[pass - 1].output && calls[pass].feature != calls[pass - 1].feature &&
                          calls[pass].localTone == 0, "Incorrect multipass chain");
            }
            check(out.texture && out.right == static_cast<int>(width) && out.bottom == static_cast<int>(height), "Output dimensions");
            auto* texture = static_cast<ID3D11Texture2D*>(out.texture);
            D3D11_TEXTURE2D_DESC stagingDesc{}; texture->GetDesc(&stagingDesc);
            check(stagingDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "Output format");
            stagingDesc.Usage = D3D11_USAGE_STAGING; stagingDesc.BindFlags = 0; stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging; hrcheck(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "Create readback");
            context->CopyResource(staging.Get(), texture); D3D11_MAPPED_SUBRESOURCE map{};
            hrcheck(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Read output");
            int maxDiff = 0;
            unsigned long long rgbSum = 0, inputRgbSum = 0;
            uint64_t digest = 1469598103934665603ull;
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width * 4; ++x)
            {
                const auto value = static_cast<unsigned char*>(map.pData)[size_t(y) * map.RowPitch + x];
                if (x % 4 != 3) { rgbSum += value; inputRgbSum += pixels[size_t(y) * width * 4 + x]; }
                digest = (digest ^ value) * 1099511628211ull;
                maxDiff = std::max(maxDiff, std::abs(int(value) - int(pixels[size_t(y) * width * 4 + x])));
            }
            context->Unmap(staging.Get(), 0);
            printf("%u passes %u%% frame %u: max channel difference %d\n", cfg.passes, cfg.resolutionPercent, frameIndex, maxDiff);
            fflush(stdout);
            if (amd && scenario == 0 && maxDiff > 1) ++amdAltered;
            if ((usingFixture || amdFixture) && !(scenario == 3 && frameIndex == 2)) check(maxDiff <= 3, "Unexpected bridge pixels");
            else check(rgbSum > 0, "Model produced a black output");
            if (scenario == 3 && frameIndex != 2) check(maxDiff <= 3, "Neutral or disabled picture should preserve the original image");
            if (scenario == 3 && frameIndex == 2) check(rgbSum > inputRgbSum && maxDiff > 3, "Exposure change did not brighten output");
            if (!usingFixture && frameIndex == 0 && scenario == 0) defaultStyleDigest = digest;
            if (!usingFixture && frameIndex == 0 && scenario == 4) {
                printf("Default versus cinematic style: %s\n", digest != defaultStyleDigest ? "different output" : "identical output");
                check(digest != defaultStyleDigest, "Style selection had no observable effect");
            }
            if (!usingFixture && frameIndex == 0 && scenario == 5) originalDigest = digest;
            if (!usingFixture && frameIndex == 0 && scenario == 6) {
                printf("Original versus Fast network hint: %s\n", digest != originalDigest ? "different output" : "identical output");
                check(digest != originalDigest, "Original and Fast produced identical output on the sample");
            }
        }
        api->Destroy(instance); instance = nullptr;
        if (usingFixture) check(live() == 0, "Features remained live after bridge destruction");
        }
        if (amd && !amdFixture) check(amdAltered >= 2, "AMDNR did not enhance the test frames");
        if (fixture) FreeLibrary(fixture); fixture = nullptr; FreeLibrary(module); puts("Bridge smoke passed"); return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Bridge smoke failed: %s\n", e.what());
        if (instance && api) api->Destroy(instance);
        if (fixture) FreeLibrary(fixture);
        if (module) FreeLibrary(module);
        return 1;
    }
}
