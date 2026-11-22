// moonsr_cli: command-line driver for the engine. Used for integration testing and scripted measurements.
//
//   moonsr_cli --list
//   moonsr_cli --window <title substring> [--seconds N] [--scaler <name substring>] [--sharpness 0..1]
//              [--fit fit|stretch|integer] [--vsync] [--monitor <index>] [--adapter <name substring>] [--cycle]
//              [--cursor none|captured|software] [--fg <multiplier 1..6>] [--fgquality performance|balanced|quality]
//              [--flowscale 0.25..1] [--record <frames> <directory>] (starts recording 2 s into the run)
//              [--fg-learn] [--fg-reset-at N] [--fg-disable-at N] (diagnostics, seconds)
//              [--neural <installed model id>] (neural rendering, one pass, full working resolution)
//
// --cycle switches through every available scaler during the run (exercises live setting changes).
#include "core/log.h"
#include "engine/engine.h"

#include <algorithm>
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
    wprintf(L"Scalers:\n");
    for (const auto& s : Engine::ListScalers()) wprintf(L"  %s%s\n", s.name, s.available ? L"" : L"  (not in this build)");
    wprintf(L"Windows:\n");
    for (const auto& w : Engine::ListWindows())
        wprintf(L"  [%s] %ux%u  %s\n", w.exeName.c_str(), w.clientSize.width, w.clientSize.height, w.title.c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring window, scalerName, adapterName, fit = L"fit", cursorMode = L"software";
    double seconds = 5.0;
    float sharpness = 0.0f;
    int monitorIndex = -1;
    bool vsync = false, list = false, cycle = false, counter = false, noscale = false, upscaleFirst = false, nolearn = false, fgLearn = false;
    int fgMultiplier = 1;
    int fgResetAt = 0, fgDisableAt = 0, orderSwitchAt = 0;
    float flowScale = 0.5f;
    std::wstring fgQuality = L"performance";
    int recordFrames = 0;
    std::wstring recordDir, neuralModel;

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
        else if (a == L"--nolearn") nolearn = true;
        else if (a == L"--neural") neuralModel = next();
        else if (a == L"--cursor") cursorMode = Lower(next());
        else if (a == L"--fg") fgMultiplier = _wtoi(next().c_str());
        else if (a == L"--fg-learn") fgLearn = true;
        else if (a == L"--fg-reset-at") fgResetAt = _wtoi(next().c_str());
        else if (a == L"--fg-disable-at") fgDisableAt = _wtoi(next().c_str());
        else if (a == L"--record") {
            recordFrames = _wtoi(next().c_str());
            recordDir = next();
        }
        else if (a == L"--fgquality") fgQuality = Lower(next());
        else if (a == L"--flowscale") flowScale = static_cast<float>(_wtof(next().c_str()));
        else {
            fwprintf(stderr, L"Unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    InitLog(L"moonsr_cli.log");

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
    for (const auto& s : Engine::ListScalers()) {
        if (!s.available) continue;
        available.push_back(s.kind);
        if (!scalerName.empty() && Contains(s.name, scalerName)) cfg.scale.scaler = s.kind;
    }
    cfg.scale.sharpness = sharpness;
    cfg.scale.fit = fit == L"stretch" ? FitMode::Stretch : fit == L"integer" ? FitMode::Integer : FitMode::Fit;
    cfg.vsync = vsync;
    cfg.showFrameCounter = counter;
    cfg.scale.enabled = !noscale;
    cfg.alsr.learning = !nolearn;
    if (upscaleFirst)
        cfg.order.stages = {PipelineStage::NeuralRendering, PipelineStage::Upscaling, PipelineStage::FrameGeneration};
    cfg.frameGen.multiplier = static_cast<uint32_t>(std::clamp(fgMultiplier, 1, 6));
    cfg.frameGen.adaptiveLearning = fgLearn;
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

    if (monitorIndex >= 0) {
        const auto monitors = Engine::ListMonitors();
        if (monitorIndex < static_cast<int>(monitors.size())) cfg.outputMonitor = monitors[monitorIndex].handle;
    }
    if (!adapterName.empty()) {
        for (const auto& a : Engine::ListAdapters())
            if (Contains(a.name, adapterName)) {
                cfg.processingAdapter = a.luid;
                break;
            }
    }

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
        if (cfg.frameGen.multiplier > 1)
            wprintf(L"     frame gen x%u%s: generated %.1f fps, hold-back %.2f ms, generated presented %llu, scheduler late %llu\n",
                    s.frameGenMultiplier, s.frameGenLimited ? L" (limited by the engine)" : L"", s.generatedFps,
                    s.frameGenDelayMs, s.generatedPresented, s.generatedLate);
        if (fgLearn && s.generatedSkipped) wprintf(L"     catch-up: generated frames skipped %llu\n", s.generatedSkipped);
        if (!s.frameGenNote.empty()) wprintf(L"     note: %s\n", s.frameGenNote.c_str());
        if (fgLearn) wprintf(L"     present call %.2f ms, swapchain wait %.2f ms; present intervals median %.2f ms p95 %.2f ms\n",
                             s.presentCallMs, s.swapchainWaitMs, s.presentIntervalMedianMs, s.presentIntervalP95Ms);
        if (fgLearn && s.displayIntervalSamples) wprintf(L"     DXGI display intervals median %.2f ms p95 %.2f ms (%u samples)\n",
                             s.displayIntervalMedianMs, s.displayIntervalP95Ms, s.displayIntervalSamples);
        if (fgLearn)
            wprintf(L"     learning: %s | profile %s | samples %u/%u | flow %.0f%% / %.0f%% | frames %llu unstable %llu, triplets %llu timing %llu motion %llu busy %llu evaluated %llu\n",
                    s.frameGenLearning.note.c_str(), s.frameGenLearning.profileLoaded ? L"loaded" : L"new",
                    s.frameGenLearning.training, s.frameGenLearning.validation,
                    s.frameGenLearning.effectiveFlow * 100.0f, s.frameGenLearning.requestedFlow * 100.0f,
                    s.frameGenLearning.sampler.frames, s.frameGenLearning.sampler.unstableFrames, s.frameGenLearning.sampler.triplets,
                    s.frameGenLearning.sampler.timingRejected, s.frameGenLearning.sampler.motionRejected,
                    s.frameGenLearning.sampler.expired, s.frameGenLearning.sampler.evaluations);
        if (tick == fgResetAt) { engine.ResetFrameGenLearning(); wprintf(L"     requested learning reset\n"); }
        if (tick == fgDisableAt) {
            cfg.frameGen.adaptiveLearning = false;
            engine.SetFrameGen(cfg.frameGen);
            wprintf(L"     disabled adaptive learning\n");
        }
        if (cfg.scale.scaler == ScalerKind::MoonALSR)
            wprintf(L"     ALSR %s: profile %s, samples %llu, solves %u, coverage %.0f%%, validation error generic %.3f / learned %.3f%s%s\n",
                    s.alsrActive ? L"active" : L"NOT active", s.alsr.profileLoaded ? L"loaded" : L"new", s.alsr.samples, s.alsr.solves,
                    s.alsr.coverage * 100.0f, s.alsr.genericError, s.alsr.learnedError, s.alsrNote.empty() ? L"" : L" | ", s.alsrNote.c_str());
        if (upscaleFirst || orderSwitchAt) wprintf(L"     order: %s%s%s\n", s.pipelineOrderText.c_str(), s.pipelineOrderNote.empty() ? L"" : L" | ", s.pipelineOrderNote.c_str());
        if (tick == orderSwitchAt) {
            cfg.order.stages = upscaleFirst ? Engine::DefaultOrder().stages :
                std::vector<PipelineStage>{PipelineStage::NeuralRendering, PipelineStage::Upscaling, PipelineStage::FrameGeneration};
            engine.SetPipelineOrder(cfg.order);
            wprintf(L"     changed load order\n");
        }
        if (cycle && !available.empty()) {
            ScaleSettings ss = cfg.scale;
            ss.scaler = available[++cycleIndex % available.size()];
            ss.sharpness = (cycleIndex % 2) ? 0.5f : 0.0f;
            engine.SetScaleSettings(ss);
        }
    }
    if (!neuralModel.empty()) {
        const EngineStats n = engine.GetStats();
        wprintf(L"Neural rendering: %s, processed %llu frames%s%s\n", n.neuralRenderingActive ? L"active" : L"inactive",
                n.neuralProcessedFrames, n.neuralRenderingNote.empty() ? L"" : L" | ", n.neuralRenderingNote.c_str());
    }
    engine.Stop();
    const EngineStats s = engine.GetStats();
    wprintf(L"Final: state %s, captured %llu, presented %llu, dropped %llu, recoveries %u%s%s\n", StateName(s.state),
            s.framesCaptured, s.framesPresented, s.framesDropped, s.recoveries, s.error.empty() ? L"" : L", error: ",
            s.error.c_str());
    return s.state == EngineState::Failed ? 1 : 0;
}
