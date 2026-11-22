// Exercises capture, neural rendering, frame generation and presentation with a user-selected model.
#include "engine/engine.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cstdlib>

namespace {
unsigned g_frame = 0;
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT rect{}; GetClientRect(hwnd, &rect);
        HBRUSH bg = CreateSolidBrush(RGB(25, 80, 130)); FillRect(dc, &rect, bg); DeleteObject(bg);
        RECT moving{static_cast<LONG>(g_frame % 400), 80, static_cast<LONG>(g_frame % 400 + 100), 220};
        HBRUSH bar = CreateSolidBrush(RGB(220, 100, 40)); FillRect(dc, &moving, bar); DeleteObject(bar);
        EndPaint(hwnd, &ps); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3 && argc != 4 && argc != 5 && argc != 6) {
        fwprintf(stderr, L"Usage: moonsr_nr_engine_smoke <model id | --amd> [order index [live] | passes resolution preset]\n"); return 2;
    }
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MoonSrNrEngineSmoke"; RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MoonSR NR engine smoke", WS_OVERLAPPEDWINDOW,
                              80, 80, 660, 400, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 3;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE); UpdateWindow(hwnd);
    moonsr::EngineConfig cfg;
    cfg.targetWindow = hwnd;
    cfg.cursor = moonsr::CursorMode::None;
    cfg.neuralRendering = true;
    const bool amd = std::wstring(argv[1]) == L"--amd";
    if (!amd) cfg.neuralModelId = argv[1];
    if (cfg.neuralModelId.find(L'\\') != std::wstring::npos || cfg.neuralModelId.find(L'/') != std::wstring::npos) {
        moonsr::ModelInfo info; std::wstring importError;
        const auto result = moonsr::Engine::Models().Import(cfg.neuralModelId, info, importError);
        if (result != moonsr::ImportResult::Ok && result != moonsr::ImportResult::AlreadyInstalled) {
            fwprintf(stderr, L"Could not import test model: %ls\n", importError.c_str()); return 5;
        }
        cfg.neuralModelId = info.id;
    }
    std::wstring expectedOrder;
    bool expectNeuralGenerated = false;
    bool expectNeuralUpscaled = false;
    moonsr::PipelineOrder switchedOrder;
    std::wstring switchedText;
    const bool exerciseLive = argc == 4 && std::wstring(argv[3]) == L"live";
    if (argc == 3 || argc == 4 || argc == 6) {
        using S = moonsr::PipelineStage;
        const std::vector<std::vector<S>> orders{{S::NeuralRendering,S::FrameGeneration,S::Upscaling},
            {S::NeuralRendering,S::Upscaling,S::FrameGeneration}, {S::FrameGeneration,S::NeuralRendering,S::Upscaling},
            {S::FrameGeneration,S::Upscaling,S::NeuralRendering}, {S::Upscaling,S::NeuralRendering,S::FrameGeneration},
            {S::Upscaling,S::FrameGeneration,S::NeuralRendering}};
        const int index = _wtoi(argv[2]);
        if (index < 0 || index >= static_cast<int>(orders.size())) return 2;
        cfg.order.stages = orders[static_cast<size_t>(index)];
        expectNeuralGenerated = std::find(cfg.order.stages.begin(), cfg.order.stages.end(), S::FrameGeneration) <
                                std::find(cfg.order.stages.begin(), cfg.order.stages.end(), S::NeuralRendering);
        expectNeuralUpscaled = std::find(cfg.order.stages.begin(), cfg.order.stages.end(), S::Upscaling) <
                              std::find(cfg.order.stages.begin(), cfg.order.stages.end(), S::NeuralRendering);
        for (const auto stage : cfg.order.stages) {
            if (!expectedOrder.empty()) expectedOrder += L" > ";
            expectedOrder += moonsr::Engine::StageName(stage);
        }
        cfg.neuralSettings.passes = 1;
        if (argc >= 4 && std::wstring(argv[3]) == L"alsr") cfg.scale.scaler = moonsr::ScalerKind::MoonALSR;
        if (argc == 6) {
            const int target = _wtoi(argv[5]);
            if (target < 0 || target >= static_cast<int>(orders.size())) return 2;
            switchedOrder.stages = orders[static_cast<size_t>(target)];
            for (const auto stage : switchedOrder.stages) {
                if (!switchedText.empty()) switchedText += L" > ";
                switchedText += moonsr::Engine::StageName(stage);
            }
        }
    }
    if (argc != 3 && argc != 4 && argc != 6) cfg.neuralSettings.passes = 3;
    cfg.neuralSettings.resolutionPercent = 50;
    cfg.neuralSettings.preset = 2;
    if (argc == 5) {
        cfg.neuralSettings.passes = static_cast<uint32_t>(std::clamp(_wtoi(argv[2]), 1, 20));
        cfg.neuralSettings.resolutionPercent = static_cast<uint32_t>(std::clamp(_wtoi(argv[3]), 25, 100));
        cfg.neuralSettings.preset = static_cast<uint32_t>(std::clamp(_wtoi(argv[4]), 0, 2));
    }
    cfg.frameGen.multiplier = 2;
    uint64_t largestMemory = 0;
    bool foundAdapter = false;
    const auto vendor = amd ? moonsr::Vendor::Amd : moonsr::Vendor::Nvidia;
    for (const auto& adapter : moonsr::Engine::ListAdapters()) {
        if (adapter.vendor == vendor && !adapter.isSoftware &&
            (!foundAdapter || adapter.dedicatedVideoMemory > largestMemory)) {
            foundAdapter = true;
            largestMemory = adapter.dedicatedVideoMemory; cfg.processingAdapter = adapter.luid;
        }
    }
    if (!foundAdapter) {
        fwprintf(stderr, L"No processing adapter for the requested neural runtime\n");
        DestroyWindow(hwnd);
        return 6;
    }
    moonsr::Engine engine;
    std::wstring error;
    if (!engine.Start(cfg, error)) {
        fwprintf(stderr, L"Engine start failed: %ls\n", error.c_str()); DestroyWindow(hwnd); return 4;
    }
    const ULONGLONG start = GetTickCount64();
    moonsr::EngineStats stats;
    bool pictureChanged = false;
    bool orderChanged = false;
    bool neuralResumed = !exerciseLive;
    uint64_t captureAtChange = 0;
    while (GetTickCount64() - start < 15000 && engine.IsRunning()) {
        MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        ++g_frame; InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd);
        Sleep(16);
        stats = engine.GetStats();
        if (!switchedText.empty() && !orderChanged && stats.pipelineOrderText == expectedOrder) {
            engine.SetPipelineOrder(switchedOrder);
            expectedOrder = switchedText;
            using S = moonsr::PipelineStage;
            expectNeuralGenerated = std::find(switchedOrder.stages.begin(), switchedOrder.stages.end(), S::FrameGeneration) <
                                    std::find(switchedOrder.stages.begin(), switchedOrder.stages.end(), S::NeuralRendering);
            expectNeuralUpscaled = std::find(switchedOrder.stages.begin(), switchedOrder.stages.end(), S::Upscaling) <
                                  std::find(switchedOrder.stages.begin(), switchedOrder.stages.end(), S::NeuralRendering);
            orderChanged = true;
        }
        if (!pictureChanged && stats.neuralRenderingActive && stats.framesCaptured >= 2) {
            moonsr::PictureSettings picture;
            picture.enabled = true;
            picture.exposure = 100;
            picture.saturation = 130;
            engine.SetPictureSettings(picture);
            if (exerciseLive) engine.SetNeuralSettings(false, cfg.neuralModelId, cfg.neuralSettings);
            pictureChanged = true;
            captureAtChange = stats.framesCaptured;
        }
        if (pictureChanged && !neuralResumed && !stats.neuralRenderingActive &&
            stats.framesCaptured >= captureAtChange + 2) {
            cfg.neuralSettings.resolutionPercent = 75;
            cfg.neuralSettings.tonePercent = 65;
            engine.SetNeuralSettings(true, cfg.neuralModelId, cfg.neuralSettings);
            neuralResumed = true;
            captureAtChange = stats.framesCaptured;
        }
        if (pictureChanged && neuralResumed && stats.neuralRenderingActive && stats.framesCaptured >= captureAtChange + 3 &&
            stats.framesPresented >= 3 && stats.generatedPresented >= 1 &&
            (expectedOrder.empty() || stats.pipelineOrderText == expectedOrder) &&
            (!expectNeuralGenerated || stats.neuralProcessedGeneratedFrames > 0) &&
            (expectedOrder.empty() || (expectNeuralUpscaled ? stats.neuralInputSize.width > stats.inputSize.width :
                                       stats.neuralInputSize.width == stats.inputSize.width)) && (switchedText.empty() || orderChanged))
            break;
    }
    const bool ok = pictureChanged && neuralResumed && stats.neuralRenderingActive && stats.framesCaptured >= captureAtChange + 3 &&
                    stats.framesPresented >= 3 && stats.generatedPresented >= 1 &&
                    (expectedOrder.empty() || stats.pipelineOrderText == expectedOrder) &&
                    (!expectNeuralGenerated || stats.neuralProcessedGeneratedFrames > 0) &&
                    (expectedOrder.empty() || (expectNeuralUpscaled ? stats.neuralInputSize.width > stats.inputSize.width :
                                               stats.neuralInputSize.width == stats.inputSize.width)) && (switchedText.empty() || orderChanged);
    wprintf(L"NR %ls, %u passes, %u%%, preset %u, captured %llu, presented %llu, generated %llu, state %u, input %ux%u, neural input %ux%u, order %ls, error %ls\n",
            stats.neuralRenderingActive ? L"active" : L"inactive", cfg.neuralSettings.passes,
            cfg.neuralSettings.resolutionPercent, cfg.neuralSettings.preset,
            stats.framesCaptured, stats.framesPresented,
            stats.generatedPresented, static_cast<unsigned>(stats.state), stats.inputSize.width, stats.inputSize.height,
            stats.neuralInputSize.width, stats.neuralInputSize.height, stats.pipelineOrderText.c_str(), stats.error.c_str());
    wprintf(L"Timing: captured %.1f fps, presented %.1f fps, latency %.1f ms, CPU frame %.2f ms, present interval "
            L"median %.2f p95 %.2f ms\n", stats.capturedFps, stats.presentedFps, stats.latencyMs, stats.cpuFrameMs,
            stats.presentIntervalMedianMs, stats.presentIntervalP95Ms);
    engine.Stop(); DestroyWindow(hwnd);
    return ok ? 0 : 1;
}
