// moonsr_cli: command-line driver for the engine. Used for integration testing and scripted measurements.
//
//   moonsr_cli --list
//   moonsr_cli --window <title substring> [--seconds N] [--scaler <name>] [--sharpness 0..1]
//              [--fit fit|stretch|integer] [--vsync] [--monitor <index>] [--adapter <name substring>] [--cycle]
//              [--cursor none|captured|software] [--fg <multiplier 1..20>] [--fgquality performance|balanced|quality]
//              [--flowscale 0.25..1] [--record <frames> <directory>] (starts recording 2 s into the run)
//              [--fg-auto <fps>] (Auto frame generation: present at this rate; overrides --fg)
//              [--fg-diag] (prints presentation timing diagnostics every second)
//              [--vsync-switch-at <second>] (toggles V-Sync live at that second of the run)
//              [--neural <installed model id>] (neural rendering, one pass, full working resolution)
//              [--order <nr,fg,up,aa,relief in any order>] (load order; overrides --upscale-first)
//              [--gpu-nr|--gpu-fg|--gpu-up|--gpu-aa|--gpu-relief <adapter name substring>] (--adapter sets all stages)
//              [--relief off|shading|parallax|motion] [--relief-strength 0..1] (Smart parallax, experimental)
//              [--relief-parallax-px 0..3] [--relief-view none|split|height|normals|protection|offset|depth]
//              [--aa off|spatial|directional|temporal] [--aa-strength 0..1] [--aa-quality performance|balanced|quality]
//              [--aa-edge-threshold 0.02..0.5] [--aa-edge-floor 0..0.2] [--aa-no-protect]
//              [--aa-view none|edges|weights|split] (Anti-aliasing, experimental)
//   moonsr_cli --relief-selftest [--adapter <name substring>]
//              (Smart parallax identity check: Analyse + Apply at strength 0 on a 1920x1080 noise image into a BGRA8 and
//               an RGBA8 destination, read back and compared byte for byte (zero change must leave every pixel exact);
//               prints the measured Analyse and Apply GPU times)
//
// --scaler takes a scaler name substring (case-insensitive), or one of the short names nearest, bilinear, bicubic,
// lanczos, spatial, moonsr (the MoonSR temporal upscaler; `alsr` is accepted as an alias).
// --cycle switches through every available scaler during the run (exercises live setting changes).
#include "core/log.h"
#include "engine/engine.h"

#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace moonsr;

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool Contains(const std::wstring& hay, const std::wstring& needle) { return Lower(hay).find(Lower(needle)) != std::wstring::npos; }

// Short scaler names. "moonsr" selects the temporal upscaler (as a substring it would also match "MoonSR Spatial");
// "alsr" is its former name, kept as an alias.
bool ShortScalerName(const std::wstring& name, ScalerKind& kind) {
    const std::wstring n = Lower(name);
    if (n == L"moonsr" || n == L"alsr" || n == L"moonalsr") kind = ScalerKind::MoonALSR;
    else if (n == L"spatial" || n == L"moonsr-spatial") kind = ScalerKind::MoonSRSpatial;
    else if (n == L"nearest") kind = ScalerKind::Nearest;
    else if (n == L"bilinear") kind = ScalerKind::Bilinear;
    else if (n == L"bicubic" || n == L"catmull-rom") kind = ScalerKind::Bicubic;
    else if (n == L"lanczos") kind = ScalerKind::Lanczos;
    else return false;
    return true;
}

const wchar_t* StateName(EngineState s) {
    switch (s) {
    case EngineState::Idle: return L"Idle";
    case EngineState::Starting: return L"Starting";
    case EngineState::Running: return L"Running";
    case EngineState::Recovering: return L"Recovering";
    default: return L"Failed";
    }
}

void List() {
    wprintf(L"Adapters:\n");
    for (const auto& a : Engine::ListAdapters()) {
        wprintf(L"  %s  [%04X:%04X] LUID %08X:%08X  VRAM %.1f GB  FL 0x%X  compute=%d tearing=%d r16f=%d min16=%d outputs=%u%s%s\n",
                a.name.c_str(), a.vendorId, a.deviceId, static_cast<unsigned>(a.luid.HighPart), a.luid.LowPart, a.dedicatedVideoMemory / 1073741824.0,
                static_cast<unsigned>(a.maxFeatureLevel), a.supportsCompute, a.supportsTearing, a.supportsR16FRenderTarget,
                a.supportsMinPrecision16, a.outputCount, a.isIntegrated ? L" integrated" : L"", a.isSoftware ? L" software" : L"");
    }
    wprintf(L"Monitors:\n");
    int i = 0;
    for (const auto& m : Engine::ListMonitors()) {
        wprintf(L"  %d: %s (%s) %ldx%ld @ %.3f Hz%s%s\n", i++, m.deviceName.c_str(), m.friendlyName.c_str(),
                m.desktopRect.right - m.desktopRect.left, m.desktopRect.bottom - m.desktopRect.top, m.refreshHz,
                m.primary ? L" primary" : L"", m.hdrEnabled ? L" HDR" : L"");
    }
    wprintf(L"Scalers (--scaler takes a name substring or nearest, bilinear, bicubic, lanczos, spatial, moonsr):\n");
    for (const auto& s : Engine::ListScalers()) wprintf(L"  %s%s\n", s.name, s.available ? L"" : L"  (not in this build)");
    wprintf(L"Windows:\n");
    for (const auto& w : Engine::ListWindows())
        wprintf(L"  [%s] %ux%u  %s\n", w.exeName.c_str(), w.clientSize.width, w.clientSize.height, w.title.c_str());
}

const wchar_t* ReliefTierName(ReliefTier t) {
    switch (t) {
    case ReliefTier::Shading: return L"shading";
    case ReliefTier::Parallax: return L"parallax";
    case ReliefTier::MotionDepth: return L"motion";
    default: return L"off";
    }
}

const wchar_t* AaModeName(AaMode m) {
    switch (m) {
    case AaMode::Spatial: return L"spatial";
    case AaMode::Directional: return L"directional";
    case AaMode::Temporal: return L"temporal";
    default: return L"off";
    }
}

// Smart parallax identity self-test (test tool: allocates and reads back freely). Returns the process exit code.
int ReliefSelfTest(const std::wstring& adapterName) {
    using Microsoft::WRL::ComPtr;
    LUID luid{};
    bool haveLuid = false;
    if (!adapterName.empty())
        for (const auto& a : Engine::ListAdapters())
            if (Contains(a.name, adapterName)) { luid = a.luid; haveLuid = true; break; }
    std::wstring error;
    auto device = GpuDevice::Create(haveLuid ? &luid : nullptr, error);
    if (!device) { fwprintf(stderr, L"Device: %s\n", error.c_str()); return 1; }
    wprintf(L"Adapter: %s\n", device->Info().name.c_str());
    ID3D11Device1* d = device->Device();
    ID3D11DeviceContext1* ctx = device->Context();
    constexpr UINT W = 1920, H = 1080;
    std::vector<uint32_t> noise(static_cast<size_t>(W) * H);
    uint32_t x = 0x12345678u;
    for (uint32_t& v : noise) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v = x; } // xorshift32, alpha random too
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{noise.data(), W * 4, 0};
    ComPtr<ID3D11Texture2D> src;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(d->CreateTexture2D(&td, &init, &src)) || FAILED(d->CreateShaderResourceView(src.Get(), nullptr, &srv))) {
        fwprintf(stderr, L"Source texture creation failed\n");
        return 1;
    }
    int failures = 0;
    // Apply always runs the relief shader; at strength 0 its change is zero and it must return each source pixel exactly
    // (same format and R/B-swapped RGBA8 destination).
    for (const DXGI_FORMAT dstFormat : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}) {
        const bool swapRb = dstFormat == DXGI_FORMAT_R8G8B8A8_UNORM;
        auto pass = ReliefPass::Create(*device, error);
        if (!pass) { fwprintf(stderr, L"ReliefPass::Create: %s\n", error.c_str()); return 1; }
        ReliefSettings rs;
        rs.tier = ReliefTier::Shading;
        rs.strength = 0.0f;
        pass->SetSettings(rs);
        D3D11_TEXTURE2D_DESC dd = td;
        dd.Format = dstFormat; dd.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> dst;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(d->CreateTexture2D(&dd, nullptr, &dst)) || FAILED(d->CreateRenderTargetView(dst.Get(), nullptr, &rtv))) {
            fwprintf(stderr, L"Destination texture creation failed\n");
            return 1;
        }
        const RectI rect{0, 0, static_cast<int32_t>(W), static_cast<int32_t>(H)};
        bool ok = pass->Analyse(src.Get(), rect, nullptr, QpcNow());
        constexpr int kRuns = 300;
        const int64_t loopStart = QpcNow();
        for (int i = 0; i < kRuns && ok; ++i) {
            ok = pass->Apply(srv.Get(), Size{W, H}, rtv.Get(), Size{W, H}, rect, 1.0f);
            if ((i & 15) == 15) { ctx->Flush(); Sleep(1); } // let timestamps complete; the engine never waits
        }
        ctx->Flush();
        wprintf(L"  %d Apply calls submitted in %.1f ms (CPU wall time incl. sleeps)\n", kRuns, QpcToMs(QpcNow() - loopStart));
        D3D11_TEXTURE2D_DESC sdsc = dd;
        sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (!ok || FAILED(d->CreateTexture2D(&sdsc, nullptr, &staging))) { fwprintf(stderr, L"Apply or staging failed\n"); return 1; }
        ctx->CopyResource(staging.Get(), dst.Get());
        const int64_t mapStart = QpcNow();
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { fwprintf(stderr, L"Map failed\n"); return 1; }
        wprintf(L"  readback Map waited %.1f ms for the GPU\n", QpcToMs(QpcNow() - mapStart));
        uint64_t mismatched = 0;
        for (UINT yy = 0; yy < H; ++yy) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(yy) * m.RowPitch);
            for (UINT xx = 0; xx < W; ++xx) {
                uint32_t v = row[xx];
                if (swapRb) v = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) | ((v & 0x000000FFu) << 16);
                if (v != noise[static_cast<size_t>(yy) * W + xx]) ++mismatched;
            }
        }
        ctx->Unmap(staging.Get(), 0);
        // The GPU has finished every Apply above (Map waited for it): further Apply calls collect their timestamps.
        for (int i = 0; i < 20; ++i) {
            (void)pass->Apply(srv.Get(), Size{W, H}, rtv.Get(), Size{W, H}, rect, 1.0f);
            ctx->Flush();
            Sleep(10);
        }
        const ReliefStats st = pass->Stats();
        wprintf(L"%s destination, strength 0: %llu of %u pixels differ -> %s; Apply GPU time %.4f ms (median of the last 64 "
                L"timestamped calls, %dx%d); stats: active %d, tier %s, note '%s'\n",
                swapRb ? L"R8G8B8A8" : L"B8G8R8A8", mismatched, W * H,
                mismatched ? L"FAIL" : L"exact", st.applyGpuMs, W, H, st.active ? 1 : 0, ReliefTierName(st.tierInEffect),
                st.reason.c_str());
        if (mismatched) ++failures;
    }
    wprintf(L"Relief self-test: %s\n", failures ? L"FAILED" : L"passed");
    return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring window, scalerName, adapterName, fit = L"fit", cursorMode = L"software";
    double seconds = 5.0;
    float sharpness = 0.0f;
    int monitorIndex = -1;
    bool vsync = false, list = false, cycle = false, counter = false, noscale = false, upscaleFirst = false, fgDiag = false;
    int fgMultiplier = 1;
    int fgAutoFps = 0;
    int orderSwitchAt = 0;
    int vsyncSwitchAt = 0;
    float flowScale = 0.5f;
    std::wstring fgQuality = L"performance";
    int recordFrames = 0;
    std::wstring recordDir, neuralModel, gpuNr, gpuFg, gpuUp, gpuAa, gpuRelief, orderText;
    std::wstring reliefMode = L"off";
    float reliefStrength = -1.0f; // < 0: the ReliefSettings default
    float reliefParallaxPx = -1.0f;
    bool compare = false;
    std::wstring reliefView = L"none";
    bool reliefSelfTest = false;
    std::wstring aaMode = L"off", aaQuality = L"balanced", aaView = L"none";
    float aaStrength = -1.0f; // < 0: the AaSettings default
    float aaEdgeThreshold = -1.0f, aaEdgeFloor = -1.0f; // < 0: the AaSettings defaults
    bool aaProtect = true;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--list") list = true;
        else if (a == L"--window") window = next();
        else if (a == L"--seconds") seconds = _wtof(next().c_str());
        else if (a == L"--scaler") scalerName = next();
        else if (a == L"--sharpness") sharpness = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--fit") fit = Lower(next());
        else if (a == L"--vsync") vsync = true;
        else if (a == L"--monitor") monitorIndex = _wtoi(next().c_str());
        else if (a == L"--adapter") adapterName = next();
        else if (a == L"--cycle") cycle = true;
        else if (a == L"--counter") counter = true;
        else if (a == L"--noscale") noscale = true;
        else if (a == L"--upscale-first") upscaleFirst = true;
        else if (a == L"--order-switch-at") orderSwitchAt = _wtoi(next().c_str());
        else if (a == L"--vsync-switch-at") vsyncSwitchAt = _wtoi(next().c_str());
        else if (a == L"--neural") neuralModel = next();
        else if (a == L"--order") orderText = Lower(next());
        else if (a == L"--gpu-nr") gpuNr = next();
        else if (a == L"--gpu-fg") gpuFg = next();
        else if (a == L"--gpu-up") gpuUp = next();
        else if (a == L"--gpu-relief") gpuRelief = next();
        else if (a == L"--gpu-aa") gpuAa = next();
        else if (a == L"--cursor") cursorMode = Lower(next());
        else if (a == L"--fg") fgMultiplier = _wtoi(next().c_str());
        else if (a == L"--fg-auto") fgAutoFps = _wtoi(next().c_str());
        else if (a == L"--fg-diag") fgDiag = true;
        else if (a == L"--record") {
            recordFrames = _wtoi(next().c_str());
            recordDir = next();
        }
        else if (a == L"--fgquality") fgQuality = Lower(next());
        else if (a == L"--flowscale") flowScale = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--relief") reliefMode = Lower(next());
        else if (a == L"--relief-strength") reliefStrength = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--relief-parallax-px") reliefParallaxPx = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--relief-selftest") reliefSelfTest = true;
        else if (a == L"--relief-view") reliefView = Lower(next());
        else if (a == L"--aa") aaMode = Lower(next());
        else if (a == L"--aa-strength") aaStrength = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--aa-quality") aaQuality = Lower(next());
        else if (a == L"--aa-edge-threshold") aaEdgeThreshold = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--aa-edge-floor") aaEdgeFloor = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--aa-no-protect") aaProtect = false;
        else if (a == L"--aa-view") aaView = Lower(next());
        else if (a == L"--compare") compare = true;
        else {
            fwprintf(stderr, L"Unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    InitLog(L"moonsr_cli.log");

    if (reliefSelfTest) return ReliefSelfTest(adapterName);

    ReliefTier reliefTier = ReliefTier::Off;
    if (reliefMode == L"shading") reliefTier = ReliefTier::Shading;
    else if (reliefMode == L"parallax") reliefTier = ReliefTier::Parallax;
    else if (reliefMode == L"motion" || reliefMode == L"motiondepth") reliefTier = ReliefTier::MotionDepth;
    else if (reliefMode != L"off") {
        fwprintf(stderr, L"--relief takes off, shading, parallax or motion (got '%s')\n", reliefMode.c_str());
        return 2;
    }

    AaMode aaRequested = AaMode::Off;
    if (aaMode == L"spatial") aaRequested = AaMode::Spatial;
    else if (aaMode == L"directional") aaRequested = AaMode::Directional;
    else if (aaMode == L"temporal") aaRequested = AaMode::Temporal;
    else if (aaMode != L"off") {
        fwprintf(stderr, L"--aa takes off, spatial, directional or temporal (got '%s')\n", aaMode.c_str());
        return 2;
    }

    if (list || window.empty()) {
        List();
        return 0;
    }

    EngineConfig cfg;
    for (const auto& w : Engine::ListWindows()) {
        if (Contains(w.title, window) || Contains(w.exeName, window)) {
            cfg.targetWindow = w.hwnd;
            wprintf(L"Target: [%s] %s (%ux%u)\n", w.exeName.c_str(), w.title.c_str(), w.clientSize.width, w.clientSize.height);
            break;
        }
    }
    if (!cfg.targetWindow) {
        fwprintf(stderr, L"No capturable window matches '%s'.\n", window.c_str());
        return 1;
    }

    std::vector<ScalerKind> available;
    ScalerKind shortKind{};
    const bool isShort = !scalerName.empty() && ShortScalerName(scalerName, shortKind);
    bool scalerFound = scalerName.empty();
    for (const auto& s : Engine::ListScalers()) {
        if (!s.available) continue;
        available.push_back(s.kind);
        if (isShort ? s.kind == shortKind : (!scalerName.empty() && Contains(s.name, scalerName))) {
            cfg.scale.scaler = s.kind;
            scalerFound = true;
        }
    }
    if (!scalerFound) fwprintf(stderr, L"No scaler matches '%s'; using the default.\n", scalerName.c_str());
    cfg.scale.sharpness = sharpness;
    cfg.scale.fit = fit == L"stretch" ? FitMode::Stretch : fit == L"integer" ? FitMode::Integer : FitMode::Fit;
    cfg.vsync = vsync;
    cfg.showFrameCounter = counter;
    cfg.scale.enabled = !noscale;
    if (upscaleFirst)
        cfg.order.stages = {PipelineStage::NeuralRendering, PipelineStage::Upscaling, PipelineStage::FrameGeneration};
    if (!orderText.empty()) {
        std::vector<PipelineStage> stages;
        size_t start = 0;
        for (;;) {
            const size_t comma = orderText.find(L',', start);
            const std::wstring tok = orderText.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (tok == L"nr") stages.push_back(PipelineStage::NeuralRendering);
            else if (tok == L"fg") stages.push_back(PipelineStage::FrameGeneration);
            else if (tok == L"up") stages.push_back(PipelineStage::Upscaling);
            else if (tok == L"relief") stages.push_back(PipelineStage::Relief);
            else if (tok == L"aa") stages.push_back(PipelineStage::AntiAliasing);
            else {
                fwprintf(stderr, L"--order takes nr, fg, up, aa and relief separated by commas (got '%s')\n", tok.c_str());
                return 2;
            }
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
        cfg.order.stages = stages;
        upscaleFirst = true; // prints the applied order every second
    }
    cfg.frameGen.multiplier = static_cast<uint32_t>(std::clamp(fgMultiplier, 1, 20));
    if (fgAutoFps > 0) {
        cfg.frameGen.mode = FrameGenMode::Auto;
        cfg.frameGen.targetFps = static_cast<uint32_t>(std::min(fgAutoFps, 1000));
    }
    const bool fgOn = cfg.frameGen.mode == FrameGenMode::Auto ? cfg.frameGen.targetFps > 0 : cfg.frameGen.multiplier > 1;
    double sumPresented = 0.0, sumRatio = 0.0, sumMedian = 0.0, sumP95 = 0.0;
    int samples = 0;
    if (!neuralModel.empty()) {
        // Neural rendering with an installed model id (see %LOCALAPPDATA%\MoonSR\models\catalogue.txt), one pass.
        cfg.neuralRendering = true;
        cfg.neuralModelId = neuralModel;
        cfg.neuralSettings.passes = 1;
        cfg.neuralSettings.resolutionPercent = 100;
    }
    cfg.frameGen.settings.flowScale = flowScale;
    cfg.frameGen.settings.quality = fgQuality == L"performance" ? FrameGenQuality::Performance
                                    : fgQuality == L"quality"   ? FrameGenQuality::Quality
                                                                : FrameGenQuality::Balanced;
    cfg.cursor = cursorMode == L"none" ? CursorMode::None : cursorMode == L"captured" ? CursorMode::Captured : CursorMode::Software;
    cfg.relief.tier = reliefTier;
    cfg.splitView = compare;
    if (reliefStrength >= 0.0f) cfg.relief.strength = std::clamp(reliefStrength, 0.0f, 1.0f);
    if (reliefParallaxPx >= 0.0f) cfg.relief.parallaxMaxPx = std::min(reliefParallaxPx, 3.0f);
    if (reliefView == L"split") cfg.relief.debugView = ReliefDebugView::Split;
    else if (reliefView == L"height") cfg.relief.debugView = ReliefDebugView::Height;
    else if (reliefView == L"normals") cfg.relief.debugView = ReliefDebugView::Normals;
    else if (reliefView == L"protection") cfg.relief.debugView = ReliefDebugView::Protection;
    else if (reliefView == L"offset") cfg.relief.debugView = ReliefDebugView::Offset;
    else if (reliefView == L"depth") cfg.relief.debugView = ReliefDebugView::Depth;
    else if (reliefView != L"none") {
        fwprintf(stderr, L"--relief-view takes none, split, height, normals, protection, offset or depth (got '%s')\n", reliefView.c_str());
        return 2;
    }
    cfg.aa.mode = aaRequested;
    if (aaStrength >= 0.0f) cfg.aa.strength = std::clamp(aaStrength, 0.0f, 1.0f);
    if (aaEdgeThreshold >= 0.0f) cfg.aa.edgeThreshold = std::clamp(aaEdgeThreshold, 0.02f, 0.5f);
    if (aaEdgeFloor >= 0.0f) cfg.aa.edgeThresholdMin = std::clamp(aaEdgeFloor, 0.0f, 0.2f);
    cfg.aa.protectText = cfg.aa.protectHud = aaProtect;
    cfg.aa.quality = aaQuality == L"performance" ? AaQuality::Performance
                   : aaQuality == L"quality"     ? AaQuality::Quality
                                                 : AaQuality::Balanced;
    if (aaQuality != L"performance" && aaQuality != L"balanced" && aaQuality != L"quality") {
        fwprintf(stderr, L"--aa-quality takes performance, balanced or quality (got '%s')\n", aaQuality.c_str());
        return 2;
    }
    if (aaView == L"edges") cfg.aa.debugView = AaDebugView::EdgeMask;
    else if (aaView == L"weights") cfg.aa.debugView = AaDebugView::BlendWeight;
    else if (aaView == L"split") cfg.aa.debugView = AaDebugView::Split;
    else if (aaView != L"none") {
        fwprintf(stderr, L"--aa-view takes none, edges, weights or split (got '%s')\n", aaView.c_str());
        return 2;
    }

    if (monitorIndex >= 0) {
        const auto monitors = Engine::ListMonitors();
        if (monitorIndex < static_cast<int>(monitors.size())) cfg.outputMonitor = monitors[monitorIndex].handle;
    }
    if (!adapterName.empty()) {
        for (const auto& a : Engine::ListAdapters())
            if (Contains(a.name, adapterName)) {
                cfg.stageAdapters.SetAll(a.luid);
                break;
            }
    }

    // Per-stage GPUs (adapter name substrings); applied after --adapter, which sets all stages.
    const auto pickGpu = [](const std::wstring& name, LUID& luid) {
        if (name.empty()) return;
        for (const auto& a : Engine::ListAdapters())
            if (Contains(a.name, name)) { luid = a.luid; return; }
        fwprintf(stderr, L"No adapter matches '%s'\n", name.c_str());
    };
    pickGpu(gpuNr, cfg.stageAdapters.neuralRendering);
    pickGpu(gpuFg, cfg.stageAdapters.frameGeneration);
    pickGpu(gpuUp, cfg.stageAdapters.upscaling);
    pickGpu(gpuAa, cfg.stageAdapters.antiAliasing);
    pickGpu(gpuRelief, cfg.stageAdapters.relief);

    Engine engine;
    std::wstring error;
    if (!engine.Start(cfg, error)) {
        fwprintf(stderr, L"Start failed: %s\n", error.c_str());
        return 1;
    }

    const int64_t start = QpcNow();
    size_t cycleIndex = 0;
    int tick = 0;
    while (engine.IsRunning() && QpcToMs(QpcNow() - start) < seconds * 1000.0) {
        Sleep(1000);
        if (tick == 1 && recordFrames > 0) {
            wchar_t full[MAX_PATH];
            GetFullPathNameW(recordDir.c_str(), MAX_PATH, full, nullptr);
            wprintf(L"     recording %d frames to %s: %s\n", recordFrames, full,
                    engine.RecordFrames(static_cast<uint32_t>(recordFrames), full) ? L"started" : L"FAILED to start");
        }
        const EngineStats s = engine.GetStats();
        if (!s.lastRecordingDir.empty() && recordFrames > 0) {
            wprintf(L"     recording finished: %s\n", s.lastRecordingDir.c_str());
            recordFrames = 0;
        }
        wprintf(L"[%2d] %-10s in %ux%u -> out %ux%u dest(%d,%d %dx%d)  captured %.1f fps  presented %.1f fps  "
                L"latency %.2f ms  cpu %.3f ms  dropped %llu  scaler '%s'  gpu '%s'\n",
                ++tick, StateName(s.state), s.inputSize.width, s.inputSize.height, s.outputSize.width, s.outputSize.height,
                s.destRect.left, s.destRect.top, s.destRect.Width(), s.destRect.Height(), s.capturedFps, s.presentedFps,
                s.latencyMs, s.cpuFrameMs, s.framesDropped, s.scalerName.c_str(), s.adapterName.c_str());
        if (fgOn) {
            wprintf(L"     frame gen %s x%u target %.1f ratio %.2f%s: generated %.1f fps, hold-back %.2f ms, generated presented %llu, "
                    L"scheduler late %llu, skipped %llu; present intervals median %.2f ms p95 %.2f ms\n",
                    s.frameGenMode == FrameGenMode::Auto ? L"auto" : L"fixed", s.frameGenMultiplier, s.frameGenTargetFps,
                    s.frameGenRatio, s.frameGenLimited ? L" (limited by the engine)" : L"", s.generatedFps,
                    s.frameGenDelayMs, s.generatedPresented, s.generatedLate, s.generatedSkipped,
                    s.presentIntervalMedianMs, s.presentIntervalP95Ms);
        }
        if (tick >= 3 && s.state == EngineState::Running) { // skip start-up seconds in the averages
            sumPresented += s.presentedFps; sumRatio += s.frameGenRatio;
            sumMedian += s.presentIntervalMedianMs; sumP95 += s.presentIntervalP95Ms;
            ++samples;
        }
        if (fgDiag && s.generatedSkipped) wprintf(L"     catch-up: generated frames skipped %llu\n", s.generatedSkipped);
        if (!s.frameGenNote.empty()) wprintf(L"     note: %s\n", s.frameGenNote.c_str());
        if (fgDiag) wprintf(L"     present call %.2f ms, swapchain wait %.2f ms; present intervals median %.2f ms p95 %.2f ms\n",
                             s.presentCallMs, s.swapchainWaitMs, s.presentIntervalMedianMs, s.presentIntervalP95Ms);
        if (fgDiag && s.displayIntervalSamples) wprintf(L"     DXGI display intervals median %.2f ms p95 %.2f ms (%u samples)\n",
                             s.displayIntervalMedianMs, s.displayIntervalP95Ms, s.displayIntervalSamples);
        if (cfg.scale.scaler == ScalerKind::MoonALSR)
            wprintf(L"     MoonSR %s%s%s\n", s.alsrActive ? L"active" : L"NOT active", s.alsrNote.empty() ? L"" : L" | ",
                    s.alsrNote.c_str());
        if (reliefTier != ReliefTier::Off)
            wprintf(L"     smart parallax: requested %s, in effect %s, %s, GPU %.4f ms%s%s\n", ReliefTierName(reliefTier),
                    ReliefTierName(s.reliefTier), s.reliefActive ? L"active" : L"not active", s.reliefGpuMs,
                    s.reliefNote.empty() ? L"" : L" | ", s.reliefNote.c_str());
        if (aaRequested != AaMode::Off)
            wprintf(L"     anti-aliasing: requested %s, in effect %s, %s, edges %.1f %%, strength %.2f, GPU %.4f ms%s%s\n",
                    AaModeName(aaRequested), AaModeName(s.aaMode), s.aaActive ? L"active" : L"not active",
                    s.aaEdgeCoverage * 100.0f, s.aaStrength, s.aaGpuMs, s.aaNote.empty() ? L"" : L" | ", s.aaNote.c_str());
        if (upscaleFirst || orderSwitchAt) wprintf(L"     order: %s%s%s\n", s.pipelineOrderText.c_str(), s.pipelineOrderNote.empty() ? L"" : L" | ", s.pipelineOrderNote.c_str());
        if (tick == orderSwitchAt) {
            cfg.order.stages = upscaleFirst ? Engine::DefaultOrder().stages :
                std::vector<PipelineStage>{PipelineStage::NeuralRendering, PipelineStage::Upscaling, PipelineStage::FrameGeneration};
            engine.SetPipelineOrder(cfg.order);
            wprintf(L"     changed load order\n");
        }
        if (tick == vsyncSwitchAt) {
            cfg.vsync = !cfg.vsync;
            engine.SetVsync(cfg.vsync);
            wprintf(L"     V-Sync switched %s\n", cfg.vsync ? L"on" : L"off");
        }
        if (cycle && !available.empty()) {
            ScaleSettings ss = cfg.scale;
            ss.scaler = available[++cycleIndex % available.size()];
            ss.sharpness = (cycleIndex % 2) ? 0.5f : 0.0f;
            engine.SetScaleSettings(ss);
        }
    }
    wprintf(L"GPUs: %s\n", engine.GetStats().stageGpuText.c_str());
    if (!neuralModel.empty()) {
        const EngineStats n = engine.GetStats();
        wprintf(L"Neural rendering: %s, processed %llu frames%s%s\n", n.neuralRenderingActive ? L"active" : L"inactive",
                n.neuralProcessedFrames, n.neuralRenderingNote.empty() ? L"" : L" | ", n.neuralRenderingNote.c_str());
    }
    const EngineStats last = engine.GetStats();
    engine.Stop();
    const EngineStats s = engine.GetStats();
    if (samples > 0)
        wprintf(L"Summary (seconds 3..%d): mode %s, target %.1f fps, presented %.1f fps, ratio %.2f, present interval "
                L"median %.2f ms p95 %.2f ms, generated late %llu, skipped %llu, dropped %llu%s%s\n", tick,
                last.frameGenMode == FrameGenMode::Auto ? L"auto" : L"fixed", last.frameGenTargetFps,
                sumPresented / samples, sumRatio / samples, sumMedian / samples, sumP95 / samples, last.generatedLate,
                last.generatedSkipped, last.framesDropped, last.frameGenNote.empty() ? L"" : L" | note: ",
                last.frameGenNote.c_str());
    wprintf(L"Final: state %s, captured %llu, presented %llu, dropped %llu, recoveries %u%s%s\n", StateName(s.state),
            s.framesCaptured, s.framesPresented, s.framesDropped, s.recoveries, s.error.empty() ? L"" : L", error: ",
            s.error.c_str());
    return s.state == EngineState::Failed ? 1 : 0;
}
