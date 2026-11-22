#include "engine/engine.h"
#include "engine/frame_pacing.h"
#include "engine/gpu_priority.h"

#include "capture/capture_source.h"
#include "core/game_profile.h"
#include "core/log.h"
#include "cursor/cursor_overlay.h"
#include "diag/frame_recorder.h"
#include "overlay/stats_overlay.h"
#include "gpu/gpu_device.h"
#include "neural/neural_bridge.h"
#include "present/presenter.h"

#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <exception>
#include <format>
#include <future>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>

namespace moonsr {

namespace {
constexpr uint32_t kAcquireTimeoutMs = 16;
constexpr uint32_t kAcquireTimeoutCursorMs = 4; // shorter wait so a software cursor keeps moving over a static image
constexpr int kMaxRecoveryAttempts = 5;      // init attempts within one recovery
constexpr int kMaxRecoveriesPerWindow = 3;   // successful recoveries allowed per kRecoveryWindowMs
constexpr double kRecoveryWindowMs = 10000.0;
constexpr double kStatsIntervalMs = 500.0;
constexpr uint32_t kOverrunWindow = 30;      // real frames per "shown late" evaluation (status note only)
constexpr int kRaiseHoldFrames = 30;         // real frames a higher multiplier must fit before it is used
constexpr int kMaxPushFailures = 3;

// Exponential moving average helper.
void Ema(double& acc, double sample, double alpha = 0.1) { acc = acc == 0.0 ? sample : acc + (sample - acc) * alpha; }
} // namespace

struct Engine::Impl {
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> alive{false};

    mutable std::mutex mutex; // guards stats, liveScale, liveVsync
    EngineStats stats;
    ScaleSettings liveScale;
    bool liveVsync = false;
    FrameGenConfig liveFrameGen;
    bool liveFrameCounter = false;
    PipelineOrder liveOrder;
    AlsrSettings liveAlsr;          // guarded by mutex
    PictureSettings livePicture;    // guarded by mutex
    struct NeuralChange {
        bool enabled;
        std::wstring modelId;
        NeuralSettings settings;
    };
    std::optional<NeuralChange> pendingNeuralChange; // guarded by mutex
    bool alsrResetRequested = false; // guarded by mutex
    bool fgLearningResetRequested = false; // guarded by mutex
    uint32_t recordRequestFrames = 0; // guarded by mutex; non-zero = start a recording
    std::wstring recordRequestDir;

    // Pipeline-thread state.
    EngineConfig config;
    std::unique_ptr<GpuDevice> device;
    std::unique_ptr<Presenter> presenter;
    std::unique_ptr<ICaptureSource> capture;
    std::unique_ptr<ScalePass> scale;
    std::unique_ptr<NeuralBridge> neural;
    bool neuralChangedLive = false;
    bool neuralReset = true;
    uint64_t neuralSequence = 0;
    RectI neuralRect{};
    int64_t neuralQpc = 0;
    uint64_t neuralOutputSequence = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> neuralOriginal, neuralGuide;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> neuralGuideRtv;
    Size neuralOriginalSize, neuralGuideSize;

    bool StartNeural(std::wstring& error) {
        // AMD and Intel runtimes carry their own model (amd-nr / intel-nr beside MoonSR.exe): no model id needed.
        if (device->Info().vendor == Vendor::Amd || device->Info().vendor == Vendor::Intel) {
            neural = NeuralBridge::Create(*device, {}, config.neuralSettings, error);
            return neural != nullptr;
        }
        for (const auto& model : Engine::Models().List()) {
            if (model.id != config.neuralModelId || model.kind != ModelKind::DlssNeuralRendering) continue;
            neural = NeuralBridge::Create(*device, model.storedPath, config.neuralSettings, error);
            return neural != nullptr;
        }
        error = L"Select an installed DLSS Neural Rendering model.";
        return false;
    }

    void DisableNeuralAfterChange(const std::wstring& error) {
        neural.reset();
        config.neuralRendering = false;
        MSR_LOG_WARN(L"Neural rendering was disabled after a live change: {}", error);
        std::lock_guard lock(mutex);
        stats.neuralRenderingActive = false;
        stats.neuralRenderingNote = L"Neural rendering was disabled. " + error;
    }

    bool ApplyNeuralChange(const NeuralChange& change) {
        const bool wasEnabled = neural != nullptr;
        const bool sameModel = device->Info().vendor == Vendor::Amd || device->Info().vendor == Vendor::Intel ||
                               config.neuralModelId == change.modelId;
        const bool sameSettings = config.neuralSettings == change.settings;
        if (wasEnabled == change.enabled && sameModel && sameSettings) return false;

        config.neuralRendering = change.enabled;
        config.neuralModelId = change.modelId;
        config.neuralSettings = change.settings;
        std::wstring error;
        bool ok = true;
        if (change.enabled && wasEnabled && sameModel && neural->SupportsLiveSettings()) {
            ok = neural->UpdateSettings(change.settings, error);
        } else {
            neural.reset();
            if (change.enabled) ok = StartNeural(error);
            neuralReset = true;
        }
        if (!ok) {
            DisableNeuralAfterChange(error);
        } else {
            std::lock_guard lock(mutex);
            if (!neural) stats.neuralRenderingActive = false;
            stats.neuralRenderingNote.clear();
        }
        neuralChangedLive = wasEnabled || change.enabled;
        return neuralChangedLive;
    }

    bool SaveNeuralOriginal(ID3D11Texture2D* source, const RectI& rect) {
        const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
        if (size.Empty()) return false;
        if (!neuralOriginal || neuralOriginalSize != size) {
            D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d);
            d.Width = size.width; d.Height = size.height; d.MipLevels = d.ArraySize = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = d.MiscFlags = 0;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            neuralOriginal.Reset();
            if (FAILED(device->Device()->CreateTexture2D(&d, nullptr, &neuralOriginal))) return false;
            neuralOriginalSize = size;
        }
        D3D11_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                      static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1};
        device->Context()->CopySubresourceRegion(neuralOriginal.Get(), 0, 0, 0, 0, source, 0, &box);
        return true;
    }
    bool PrepareNeuralGuide(Size size, std::wstring& error) {
        if (!neuralOriginal || size.Empty()) { error = L"Original optical-flow guide is unavailable."; return false; }
        if (!neuralGuide || neuralGuideSize != size) {
            D3D11_TEXTURE2D_DESC d{}; neuralOriginal->GetDesc(&d);
            d.Width = size.width; d.Height = size.height;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            neuralGuideRtv.Reset(); neuralGuide.Reset();
            if (FAILED(device->Device()->CreateTexture2D(&d, nullptr, &neuralGuide)) ||
                FAILED(device->Device()->CreateRenderTargetView(neuralGuide.Get(), nullptr, &neuralGuideRtv))) {
                error = L"Could not create the original optical-flow guide."; return false;
            }
            neuralGuideSize = size;
            neuralReset = true;
        }
        ScaleSettings guideSettings;
        guideSettings.scaler = ScalerKind::Bilinear;
        guideSettings.fit = FitMode::Stretch;
        RectI originalRect{0, 0, static_cast<int32_t>(neuralOriginalSize.width), static_cast<int32_t>(neuralOriginalSize.height)};
        if (!scale->Render(neuralOriginal.Get(), originalRect, neuralGuideRtv.Get(), size, guideSettings, nullptr)) {
            error = L"Could not prepare the original optical-flow guide."; return false;
        }
        return true;
    }

    bool NeuralProcess(ID3D11Texture2D*& tex, RectI& rect, int64_t qpc, const PictureSettings& picture,
                       std::wstring& error, bool inputIsOriginal = false) {
        ID3D11Texture2D* guide = nullptr;
        if (device->Info().vendor == Vendor::Nvidia && !inputIsOriginal) {
            const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
            if (neuralOriginal && neuralOriginalSize == size) {
                guide = neuralOriginal.Get();
            } else {
                if (!PrepareNeuralGuide(size, error)) return false;
                guide = neuralGuide.Get();
            }
        }
        ID3D11Texture2D* result = nullptr; RectI resultRect;
        if (!neural->ProcessWithGuide(tex, rect, guide, qpc, ++neuralOutputSequence, neuralReset,
                                      picture, result, resultRect, error)) return false;
        neuralReset = false;
        tex = result; rect = resultRect;
        return true;
    }
    // Software cursor path: the scaled image is kept in `scene` so it can be re-composited with the cursor at a
    // new position even when the game delivers no new frame.
    std::unique_ptr<CursorOverlay> cursor;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> sceneRtv;
    Size sceneSize;
    bool sceneValid = false;
    RectI sceneDestRect;
    CursorState drawnCursor;

    // Frame counter overlay: "game fps / displayed fps", refreshed with the statistics.
    std::unique_ptr<StatsOverlay> overlay;
    bool overlayFailed = false;
    bool overlayEnabled = false;
    uint32_t overlayRendered = 0, overlayTotal = 0;

    // Diagnostic recorder. recInfo describes the frame about to be presented.
    std::unique_ptr<FrameRecorder> recorder;
    RecordedFrameInfo recInfo;
    uint64_t realCounter = 0;

    // Frame generation and pacing. After a real frame arrives, `pending` holds the presents still owed for it:
    // the generated frames between the previous and the new real frame, then the new real frame itself, at evenly
    // spaced due times one slot (real frame time / multiplier) apart.
    struct Pending {
        int64_t dueQpc;
        float t;   // interpolation phase for generated frames
        bool real; // true: present the newest real frame
    };
    std::unique_ptr<FrameGenerator> fg;
    std::unique_ptr<fgadaptive::Controller> fgLearning;
    FrameGenSettings fgLearningBaseline;
    std::wstring fgLearningGame, fgLearningDirectory;
    bool fgLearningFailed = false;
    int64_t fgLearningLastDiscontinuity = 0;

    // Moon ALSR. It needs the generator as its motion estimator, so the generator also runs when frame generation
    // is off but ALSR is selected. Real frames: AlsrUpscaler::Render once per PushFrame into alsrOut. Generated
    // frames: RenderSingle into alsrGen. Both are destination-rectangle sized and are then copied 1:1.
    std::unique_ptr<AlsrUpscaler> alsr;
    // When ALSR precedes frame generation, its motion field must be measured on the original-sized input,
    // independently of the generator that interpolates the upscaled result.
    std::unique_ptr<FrameGenerator> alsrMotion;
    bool alsrMotionFailed = false;
    bool alsrFailed = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> alsrOut, alsrGen;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> alsrOutRtv, alsrGenRtv;
    Size alsrSize;
    bool pendingAlsr = false; // the real frame of the current schedule was reconstructed by ALSR
    bool pendingAlsrAfterNeural = false;
    std::wstring gameKey;     // file-name-safe id of the captured game (exe name) for the per-game profile

    bool EnsureAlsrTargets(Size size) {
        if (alsrOut && alsrGen && alsrSize == size) return true;
        alsrOutRtv.Reset();
        alsrGenRtv.Reset();
        alsrOut.Reset();
        alsrGen.Reset();
        if (scale) scale->ResetSourceCache();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->Device()->CreateTexture2D(&td, nullptr, &alsrOut);
        if (SUCCEEDED(hr)) hr = device->Device()->CreateRenderTargetView(alsrOut.Get(), nullptr, &alsrOutRtv);
        if (SUCCEEDED(hr)) hr = device->Device()->CreateTexture2D(&td, nullptr, &alsrGen);
        if (SUCCEEDED(hr)) hr = device->Device()->CreateRenderTargetView(alsrGen.Get(), nullptr, &alsrGenRtv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Moon ALSR output textures {}x{} failed: {}", size.width, size.height, HrToString(hr));
            alsrOut.Reset();
            alsrGen.Reset();
            return false;
        }
        alsrSize = size;
        return true;
    }

    void ReleaseAlsr() {
        alsr.reset(); // saves the per-game profile
        alsrMotion.reset();
        alsrMotionFailed = false;
        alsrOutRtv.Reset();
        alsrGenRtv.Reset();
        alsrOut.Reset();
        alsrGen.Reset();
        alsrSize = {};
        pendingAlsr = false;
        pendingAlsrAfterNeural = false;
        if (scale) scale->ResetSourceCache();
    }
    bool fgCreateFailed = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> fgOut;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> fgOutRtv;
    Size fgOutSize;
    std::array<Pending, 8> pending{};
    uint32_t pendingCount = 0, pendingIndex = 0;
    double pendingSlotMs = 0.0;
    int64_t pendingRealArrivalQpc = 0;
    int64_t lastRealQpc = 0;
    double periodMs = 0.0; // smoothed real frame time (fast: used for slot spacing)
    double periodSlowMs = 0.0; // heavily smoothed frame time (used for the refresh-rate cap; capture timestamps jitter with the compositor)
    // Effective multiplier. Decided once per real frame (DecideMultiplier), never per loop iteration:
    //  - refresh cap with hysteresis: never more displayed frames than the monitor refreshes;
    //  There is deliberately no GPU-cost-based multiplier limit. Normally a late slot is shown; while adaptive
    //  learning is enabled, V-Sync catch-up may omit a sufficiently stale generated frame to reduce backlog.
    uint32_t fgRequested = 1;
    uint32_t fgEffective = 1;
    uint32_t overrunCount = 0, overrunTotal = 0;
    int raiseStreak = 0;
    bool fgRunningLate = false; // most real frames of the last window were shown later than one frame time
    int outlierStreak = 0;
    int64_t rateStartQpc = 0; // frame-rate counting window
    uint32_t rateFrames = 0;
    double rateFps = 0.0;     // real frames per second counted over the last full window; 0 = unknown
    int fgFailStreak = 0;
    int fgLimitReason = 0;            // 0 none, 1 monitor refresh rate, 2 waiting to confirm a higher multiplier
    bool fgHasHistory = false;        // the generator holds frames from the current run of generated frames
    uint32_t scheduledMultiplier = 1; // multiplier of the schedule currently in `pending`
    bool pendingStable = false;       // the schedule was built from a stable frame-time estimate
    int64_t pendingBaseQpc = 0;       // arrival time of the real frame the schedule belongs to
    double outputRefreshHz = 0.0;
    HANDLE pacingTimer = nullptr;
    bool appliedVsync = false;
    bool waitForSwapchain = false;
    double presentCallAvg = 0.0, swapchainWaitAvg = 0.0;
    std::array<double, 240> presentIntervals{};
    size_t presentIntervalCount = 0, presentIntervalNext = 0;
    int64_t previousPresentQpc = 0;
    std::array<double, 240> displayIntervals{};
    size_t displayIntervalCount = 0, displayIntervalNext = 0;
    int64_t previousDisplayQpc = 0;
    uint32_t previousDisplayPresent = 0;
    bool displayTimingEnabled = false;
    bool startReported = false;
    int64_t recoveryWindowStart = 0;
    int recoveriesInWindow = 0;

    void SetState(EngineState s, const std::wstring* error = nullptr) {
        std::lock_guard lock(mutex);
        stats.state = s;
        if (error) stats.error = *error;
    }

    void Teardown() {
        // Order matters: users of the device go first. Destroying the cursor overlay restores the system cursor.
        presentIntervalCount = presentIntervalNext = 0;
        previousPresentQpc = 0;
        displayIntervalCount = displayIntervalNext = 0;
        previousDisplayQpc = 0; previousDisplayPresent = 0;
        recorder.reset(); // finishes writing queued frames
        neural.reset();
        neuralReset = true;
        neuralSequence = 0;
        neuralRect = {};
        neuralQpc = 0;
        neuralOutputSequence = 0;
        neuralGuideRtv.Reset(); neuralGuide.Reset(); neuralOriginal.Reset();
        neuralGuideSize = {}; neuralOriginalSize = {};
        ReleaseAlsr();
        alsrFailed = false;
        overlay.reset();
        overlayFailed = false;
        fgLearning.reset(); // drains profile writes before the GPU device is destroyed
        fgLearningFailed = false;
        fgLearningLastDiscontinuity = 0;
        ResetFrameGen();
        fgCreateFailed = false;
        cursor.reset();
        sceneRtv.Reset();
        scene.Reset();
        sceneSize = {};
        sceneValid = false;
        drawnCursor = {};
        scale.reset();
        capture.reset();
        presenter.reset();
        device.reset();
    }

    bool Init(std::wstring& error) {
        if (!IsWindow(config.targetWindow)) {
            error = L"The target window no longer exists.";
            return false;
        }
        HMONITOR monitor = config.outputMonitor;
        if (!monitor) monitor = MonitorFromWindow(config.targetWindow, MONITOR_DEFAULTTONEAREST);

        // Default processing adapter: the one that drives the output monitor. That avoids a cross-adapter copy
        // at presentation time. An explicit choice (dual-GPU setups) overrides it.
        const bool userChoseAdapter = !LuidIsZero(config.processingAdapter);
        LUID adapter = config.processingAdapter;
        if (!userChoseAdapter) {
            for (const auto& m : EnumerateMonitors())
                if (m.handle == monitor) adapter = m.adapterLuid;
        }
        outputRefreshHz = 0.0;
        for (const auto& m : EnumerateMonitors())
            if (m.handle == monitor) outputRefreshHz = m.refreshHz;

        device = GpuDevice::Create(LuidIsZero(adapter) ? nullptr : &adapter, error);
        if (!device && !userChoseAdapter && !LuidIsZero(adapter)) {
            // The monitor may be driven by a display-only adapter (indirect / virtual / USB display).
            MSR_LOG_WARN(L"No device on the adapter driving the output monitor ({}); using the default adapter", error);
            error.clear();
            device = GpuDevice::Create(nullptr, error);
        }
        if (!device) return false;

        // MoonSR's work goes ahead of the game's on a busy GPU (see engine/gpu_priority.h). Done before any
        // submission and before the neural bridge is created: the bridge's D3D12 queue follows the device priority.
        // MOONSR_GPU_PRIORITY=normal leaves the priority alone (diagnostics and A/B measurements).
        wchar_t priorityOverride[16]{};
        const DWORD overrideLength = GetEnvironmentVariableW(L"MOONSR_GPU_PRIORITY", priorityOverride, 16);
        if (overrideLength > 0 && overrideLength < 16 && _wcsicmp(priorityOverride, L"normal") == 0) {
            MSR_LOG_WARN(L"GPU priority: left at normal (MOONSR_GPU_PRIORITY=normal)");
        } else {
            const GpuPriorityResult prio = RaiseGpuPriority(device->Device());
            if (prio.Realtime())
                MSR_LOG_INFO(L"GPU priority: process class realtime, device thread priority {}", prio.deviceThreadPriority);
            else
                MSR_LOG_WARN(L"GPU priority: realtime class refused (status 0x{:08X}); using {}, device thread priority {}. "
                             L"MoonSR shares the GPU with the game when it is fully loaded",
                             static_cast<unsigned long>(prio.processStatus), GpuSchedulingClassName(prio.processClass),
                             prio.deviceThreadPriority);
        }

        PresentOptions po;
        po.monitor = monitor;
        po.vsync = config.vsync;
        presenter = Presenter::Create(*device, po, error);
        if (!presenter) return false;
        appliedVsync = config.vsync;
        // The frame-latency waitable starts signalled once. Consume that before the first frame so that every
        // later wait corresponds to the frame we actually presented (otherwise one extra frame stays queued).
        waitForSwapchain = true;

        if (config.cursor == CursorMode::Software) {
            std::wstring cursorError;
            cursor = CursorOverlay::Create(*device, cursorError);
            if (!cursor)
                MSR_LOG_WARN(L"Software cursor unavailable ({}); capturing the cursor into the image instead", cursorError);
        }

        CaptureOptions co;
        co.captureCursor = config.cursor == CursorMode::Captured || (config.cursor == CursorMode::Software && !cursor);
        capture = CreateWgcCapture(*device, config.targetWindow, co, error);
        if (!capture) return false;

        scale = ScalePass::Create(*device, error);
        if (!scale) return false;

        if (config.neuralRendering && !StartNeural(error)) {
            error = L"Neural rendering could not start: " + error;
            return false;
        }

        // Per-game identity for Moon ALSR profiles: the captured window's exe name, reduced to safe characters.
        gameKey.clear();
        for (const auto& w : EnumerateCaptureWindows()) {
            if (w.hwnd != config.targetWindow) continue;
            for (wchar_t c : w.exeName)
                gameKey.push_back((iswalnum(c) || c == L'.' || c == L'-' || c == L'_') ? static_cast<wchar_t>(towlower(c)) : L'_');
            break;
        }
        if (gameKey.empty()) gameKey = L"unknown";
        // Use the executable's full path for learning identity so games with the same filename stay separate.
        fgLearningGame.clear();
        DWORD targetPid = 0;
        GetWindowThreadProcessId(config.targetWindow, &targetPid);
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid)) {
            wchar_t executable[32768]{};
            DWORD len = static_cast<DWORD>(std::size(executable));
            if (QueryFullProcessImageNameW(process, 0, executable, &len)) {
                fgLearningGame.assign(executable, len);
                std::transform(fgLearningGame.begin(), fgLearningGame.end(), fgLearningGame.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            }
            CloseHandle(process);
        }
        wchar_t localAppData[MAX_PATH]{};
        const DWORD localLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        fgLearningDirectory = localLen && localLen < MAX_PATH ? std::wstring(localAppData) + L"\\MoonSR\\profiles" : L"";

        std::lock_guard lock(mutex);
        stats.adapterName = device->Info().name;
        stats.adapterVendor = device->Info().vendor;
        stats.captureMethod = capture->MethodName();
        stats.outputSize = presenter->BackBufferSize();
        stats.neuralRenderingActive = false;
        stats.neuralRenderingNote.clear();
        MSR_LOG_INFO(L"Pipeline initialised: adapter '{}', capture '{}', output {}x{}", stats.adapterName,
                     stats.captureMethod, stats.outputSize.width, stats.outputSize.height);
        return true;
    }

    // Tears everything down and rebuilds it. Used for device loss and presenter invalidation.
    // Bounded twice: a few init attempts per recovery, and a few recoveries per time window, so a fault that
    // repeats every frame ends in Failed instead of flashing the screen forever.
    bool Recover(const wchar_t* reason) {
        const int64_t now = QpcNow();
        if (recoveryWindowStart == 0 || QpcToMs(now - recoveryWindowStart) > kRecoveryWindowMs) {
            recoveryWindowStart = now;
            recoveriesInWindow = 0;
        }
        if (++recoveriesInWindow > kMaxRecoveriesPerWindow) {
            MSR_LOG_ERROR(L"Giving up: {} recoveries within {:.0f} s (last reason: {})", recoveriesInWindow - 1,
                          kRecoveryWindowMs / 1000.0, reason);
            return false;
        }

        MSR_LOG_WARN(L"Recovering pipeline: {}", reason);
        SetState(EngineState::Recovering);
        for (int attempt = 1; attempt <= kMaxRecoveryAttempts && !stopRequested; ++attempt) {
            Teardown();
            Sleep(attempt == 1 ? 100 : 500);
            std::wstring error;
            if (Init(error)) {
                std::lock_guard lock(mutex);
                stats.state = EngineState::Running;
                stats.recoveries++;
                return true;
            }
            MSR_LOG_WARN(L"Recovery attempt {} failed: {}", attempt, error);
            if (!IsWindow(config.targetWindow)) break;
        }
        return false;
    }

    void ResetFrameGen() {
        fg.reset();
        alsrMotion.reset();
        fgOutRtv.Reset();
        fgOut.Reset();
        fgOutSize = {};
        preScaledRtv.Reset();
        preScaled.Reset();
        preScaledSize = {};
        pendingPreScaled = false;
        if (alsr) alsr->Reset();
        pendingAlsr = false;
        pendingAlsrAfterNeural = false;
        pendingCount = pendingIndex = 0;
        pendingSlotMs = 0.0;
        lastRealQpc = 0;
        periodMs = periodSlowMs = 0.0;
        rateStartQpc = 0;
        rateFrames = 0;
        rateFps = 0.0;
        overrunCount = overrunTotal = 0;
        raiseStreak = 0;
        fgRunningLate = false;
        fgFailStreak = 0;
        fgHasHistory = false;
        scheduledMultiplier = 1;
        if (scale) scale->ResetSourceCache();
    }

    // Updates the smoothed real frame time from a new capture frame. Returns true if this frame fits the current
    // estimate ("stable"). Hitches, pauses and rate changes return false: such a frame is shown immediately, no
    // frames are generated for it, and it does not disturb the estimate unless the new rate persists.
    bool UpdatePeriod(const CapturedFrame& frame) {
        // Pacing uses the actual interval between frames we can present. Dividing by discarded captures
        // schedules interpolated frames too close together when latest-wins acquisition skips frames.
        const double dt = lastRealQpc ? QpcToMs(frame.captureQpc - lastRealQpc) : 0.0;
        lastRealQpc = frame.captureQpc;

        // Frame RATE by counting frames over about a second. Individual arrival intervals are quantised to the
        // compositor's refresh (a 60 fps game on a 144 Hz desktop arrives 13.9 / 20.8 / 6.9 ms apart), so any
        // estimate built from single intervals wanders; a count over a window does not. This drives the
        // refresh-rate cap and anchors the slot spacing.
        ++rateFrames;
        if (rateStartQpc == 0 || dt > 250.0) { // start, or resume after a pause
            rateStartQpc = frame.captureQpc;
            rateFrames = 0;
            if (dt > 250.0) rateFps = 0.0;
        } else {
            const double elapsed = QpcToMs(frame.captureQpc - rateStartQpc);
            if (elapsed >= 1000.0) {
                rateFps = rateFrames * 1000.0 / elapsed;
                rateStartQpc = frame.captureQpc;
                rateFrames = 0;
            }
        }
        periodSlowMs = rateFps > 0.0 ? 1000.0 / rateFps : 0.0;

        if (dt < 2.0 || dt > 250.0) {
            periodMs = 0.0;
            outlierStreak = 0;
            return false;
        }
        if (periodMs == 0.0) {
            periodMs = periodSlowMs > 0.0 ? periodSlowMs : dt;
            return false;
        }
        // Reference frame time: the counted rate when known, else the running estimate.
        const double ref = periodSlowMs > 0.0 ? periodSlowMs : periodMs;
        if (dt > ref * 2.2 || dt < ref * 0.35) {
            // A hitch or a burst: show this frame at once, generate nothing for it, leave the estimate alone.
            if (++outlierStreak >= 5 && periodSlowMs == 0.0) {
                periodMs = dt;
                outlierStreak = 0;
            }
            return false;
        }
        outlierStreak = 0;
        periodMs += (dt - periodMs) * 0.1;
        if (periodSlowMs > 0.0) periodMs = std::clamp(periodMs, periodSlowMs * 0.9, periodSlowMs * 1.1);
        return true;
    }

    uint32_t DecideMultiplier(bool vsync) {
        if (overrunTotal >= kOverrunWindow) {
            const bool late = overrunCount * 2 > overrunTotal;
            if (late && !fgRunningLate)
                MSR_LOG_WARN(L"Frame generation x{}: {} of {} real frames were presented more than one capture interval "
                             L"after arrival; capture, processing and presentation delays can all contribute",
                             fgEffective, overrunCount, overrunTotal);
            fgRunningLate = late;
            overrunCount = overrunTotal = 0;
        }

        // Only the display's refresh rate limits the multiplier. Cost never does.
        const uint32_t target = fgRequested;
        uint32_t m = std::clamp<uint32_t>(fgEffective, 1, target);
        bool waitingToRaise = false;
        if (periodSlowMs > 0.0 && outputRefreshHz > 1.0) {
            const double fps = 1000.0 / periodSlowMs;
            // With V-Sync every present waits for a refresh, so there is almost no headroom above the refresh rate.
            const double keep = vsync ? 1.02 : 1.05;
            const double raise = keep - 0.02;
            while (m > 1 && fps * m > outputRefreshHz * keep) --m;
            if (m < target && fps * (m + 1) <= outputRefreshHz * raise) {
                if (++raiseStreak >= kRaiseHoldFrames) {
                    ++m;
                    raiseStreak = 0;
                }
                waitingToRaise = m < target && fps * (m + 1) <= outputRefreshHz * raise;
            } else {
                raiseStreak = 0;
            }
        } else {
            // No frame-time estimate (start, pause, rate change): keep the current multiplier. Raising it blindly
            // here made it bounce back to the requested value after every estimate reset.
            raiseStreak = 0;
            waitingToRaise = m < target;
        }
        fgEffective = m;
        fgLimitReason = m >= fgRequested ? 0 : (waitingToRaise ? 2 : 1);
        return m;
    }

    // Scales `tex` into the kept scene (software cursor active) or straight into the back buffer. No present.
    bool RenderOnly(ID3D11Texture2D* tex, const RectI& rect, const ScaleSettings& settings, RectI& destRect, bool& toScene) {
        toScene = cursor && EnsureScene();
        ID3D11RenderTargetView* target = toScene ? sceneRtv.Get() : presenter->BackBufferRtv();
        const bool ok = scale->Render(tex, rect, target, presenter->BackBufferSize(), settings, &destRect);
        if (ok && toScene) {
            sceneValid = true;
            sceneDestRect = destRect;
        }
        return ok;
    }

    // "Upscaling before frame generation": real frames are upscaled into this texture (destination-rectangle
    // size) and THAT is what the generator sees; presented frames are then already at output size.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> preScaled;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> preScaledRtv;
    Size preScaledSize;
    bool pendingPreScaled = false; // the frames of the current schedule are pre-scaled
    bool pendingNeuralDone = false;
    Size lastInputSize;            // size of the captured game image (reported as the input resolution)

    bool EnsurePreScaled(Size size) {
        if (preScaled && preScaledSize == size) return true;
        preScaledRtv.Reset();
        preScaled.Reset();
        if (scale) scale->ResetSourceCache();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->Device()->CreateTexture2D(&td, nullptr, &preScaled);
        if (SUCCEEDED(hr)) hr = device->Device()->CreateRenderTargetView(preScaled.Get(), nullptr, &preScaledRtv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Pre-scaled frame texture {}x{} failed: {}", size.width, size.height, HrToString(hr));
            preScaled.Reset();
            return false;
        }
        preScaledSize = size;
        return true;
    }

    bool EnsureFgOut(Size size) {
        if (fgOut && fgOutSize == size) return true;
        fgOutRtv.Reset();
        fgOut.Reset();
        if (scale) scale->ResetSourceCache();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->Device()->CreateTexture2D(&td, nullptr, &fgOut);
        if (SUCCEEDED(hr)) hr = device->Device()->CreateRenderTargetView(fgOut.Get(), nullptr, &fgOutRtv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Generated-frame texture {}x{} failed: {}", size.width, size.height, HrToString(hr));
            fgOut.Reset();
            return false;
        }
        fgOutSize = size;
        return true;
    }

    // Sleeps until shortly before `dueQpc` with a high-resolution timer, then spins for the last stretch.
    void WaitUntil(int64_t dueQpc) {
        for (;;) {
            const double remainingMs = QpcToMs(dueQpc - QpcNow());
            if (remainingMs <= 0.0 || remainingMs > 250.0) return; // > 250 ms would be a scheduling bug; never hang
            if (remainingMs > 1.5 && pacingTimer) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((remainingMs - 1.0) * 10000.0);
                SetWaitableTimer(pacingTimer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(pacingTimer, static_cast<DWORD>(remainingMs) + 2);
            } else {
                YieldProcessor();
            }
        }
    }

    // Presents the back buffer. When a recording is active the back buffer is saved first, exactly as shown.
    PresentStatus PresentBackBuffer() {
        if (overlayEnabled) {
            if (!overlay && !overlayFailed) {
                std::wstring overlayError;
                overlay = StatsOverlay::Create(*device, overlayError);
                if (!overlay) {
                    overlayFailed = true;
                    MSR_LOG_WARN(L"Frame counter unavailable: {}", overlayError);
                }
            }
            if (overlay) overlay->Draw(presenter->BackBufferRtv(), presenter->BackBufferSize(), overlayRendered, overlayTotal);
        }
        if (recorder && recorder->Active()) {
            Microsoft::WRL::ComPtr<ID3D11Resource> res;
            presenter->BackBufferRtv()->GetResource(&res);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            if (res && SUCCEEDED(res.As(&tex))) {
                recInfo.actualQpc = QpcNow();
                recInfo.realIndex = realCounter;
                recorder->Capture(tex.Get(), RectI{}, recInfo);
            }
        }
        recInfo = RecordedFrameInfo{}; // default for the next present: a real frame, unscheduled
        const int64_t begin = QpcNow();
        const PresentStatus result = presenter->Present();
        const int64_t end = QpcNow();
        Ema(presentCallAvg, QpcToMs(end - begin));
        if (result == PresentStatus::Ok) {
            const double dt = previousPresentQpc ? QpcToMs(end - previousPresentQpc) : 0.0;
            if (dt > 0.0 && dt < 250.0) {
                presentIntervals[presentIntervalNext] = dt;
                presentIntervalNext = (presentIntervalNext + 1) % presentIntervals.size();
                presentIntervalCount = std::min(presentIntervalCount + 1, presentIntervals.size());
            }
            previousPresentQpc = end;
            DXGI_FRAME_STATISTICS f{};
            if (displayTimingEnabled && presenter->FrameStatistics(f) && f.SyncQPCTime.QuadPart > 0 && f.PresentCount != previousDisplayPresent) {
                if (previousDisplayQpc && f.PresentCount == previousDisplayPresent + 1) {
                    const double displayDt = QpcToMs(f.SyncQPCTime.QuadPart - previousDisplayQpc);
                    if (displayDt > 0.0 && displayDt < 250.0) {
                        displayIntervals[displayIntervalNext] = displayDt;
                        displayIntervalNext = (displayIntervalNext + 1) % displayIntervals.size();
                        displayIntervalCount = std::min(displayIntervalCount + 1, displayIntervals.size());
                    }
                }
                previousDisplayQpc = f.SyncQPCTime.QuadPart;
                previousDisplayPresent = f.PresentCount;
            }
        }
        return result;
    }

    // Scales `tex` to the output (through the kept scene when the software cursor is active) and presents.
    PresentStatus RenderAndPresent(ID3D11Texture2D* tex, const RectI& rect, const ScaleSettings& settings,
                                   const CursorState& cursorState, RectI& destRect, bool& rendered) {
        const bool useScene = cursor && EnsureScene();
        ID3D11RenderTargetView* target = useScene ? sceneRtv.Get() : presenter->BackBufferRtv();
        rendered = scale->Render(tex, rect, target, presenter->BackBufferSize(), settings, &destRect);
        if (!rendered) return PresentStatus::Occluded; // "nothing presented"
        if (useScene) {
            sceneValid = true;
            sceneDestRect = destRect;
            return ComposeAndPresent(cursorState);
        }
        return PresentBackBuffer();
    }

    bool EnsureScene() {
        const Size size = presenter->BackBufferSize();
        if (scene && sceneSize == size) return true;
        sceneRtv.Reset();
        scene.Reset();
        sceneValid = false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = presenter->BackBufferFormat();
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->Device()->CreateTexture2D(&td, nullptr, &scene);
        if (SUCCEEDED(hr)) hr = device->Device()->CreateRenderTargetView(scene.Get(), nullptr, &sceneRtv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Scene texture {}x{} failed: {}", size.width, size.height, HrToString(hr));
            scene.Reset();
            return false;
        }
        sceneSize = size;
        return true;
    }

    // Copies the kept scene to the back buffer, draws the software cursor on top and presents.
    PresentStatus ComposeAndPresent(const CursorState& state) {
        ID3D11RenderTargetView* bb = presenter->BackBufferRtv();
        Microsoft::WRL::ComPtr<ID3D11Resource> bbTex;
        bb->GetResource(&bbTex);
        device->Context()->CopyResource(bbTex.Get(), scene.Get());
        cursor->Draw(state, config.targetWindow, bb, sceneSize, sceneDestRect);
        drawnCursor = state;
        return PresentBackBuffer();
    }

    void ReportStart(std::promise<std::wstring>& started, const std::wstring& error) {
        if (startReported) return;
        startReported = true;
        started.set_value(error);
    }

    void ThreadMain(std::promise<std::wstring> started) {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        startReported = false;
        pacingTimer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                             TIMER_ALL_ACCESS);
        if (!pacingTimer) {
            // Older Windows 10 builds: tick-resolution timer only. Pacing then relies on the final spin and may
            // oversleep; generated frames are then shown slightly late.
            MSR_LOG_WARN(L"High-resolution waitable timer unavailable; frame pacing precision is reduced");
            pacingTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }

        std::wstring exitError;
        EngineState exitState = EngineState::Idle;
        try {
            Run(started, exitError, exitState);
        } catch (const std::exception& e) {
            const std::string what = e.what();
            exitError = L"Unexpected internal error: " + std::wstring(what.begin(), what.end());
            exitState = EngineState::Failed;
        } catch (...) {
            exitError = L"Unexpected internal error.";
            exitState = EngineState::Failed;
        }

        Teardown();
        {
            std::lock_guard lock(mutex);
            stats.state = exitState;
            stats.error = exitError;
            stats.capturedFps = stats.presentedFps = stats.generatedFps = 0.0;
            stats.neuralRenderingActive = false;
        }
        if (!exitError.empty()) MSR_LOG_WARN(L"Pipeline stopped: {}", exitError);
        // If we never reported a start result (exception during init), report the failure now.
        ReportStart(started, exitError.empty() ? L"The pipeline stopped during start-up." : exitError);
        if (pacingTimer) {
            CloseHandle(pacingTimer);
            pacingTimer = nullptr;
        }
        if (SUCCEEDED(coHr)) CoUninitialize();
        alive = false;
    }

    void Run(std::promise<std::wstring>& started, std::wstring& exitError, EngineState& exitState) {
        std::wstring error;
        if (!Init(error)) {
            if (error.empty()) error = L"Pipeline initialisation failed.";
            exitError = error;
            exitState = EngineState::Failed;
            ReportStart(started, error);
            return;
        }
        SetState(EngineState::Running);
        ReportStart(started, L"");
        if (GetModuleHandleW(L"RTSSHooks64.dll"))
            MSR_LOG_WARN(L"RivaTuner Statistics Server (RTSSHooks64.dll) is loaded in this process: a global RTSS frame "
                         L"limit will also limit MoonSR's output and can prevent frame generation");

        // Decides how the pipeline ends when a recovery did not succeed.
        auto recoveryFailed = [&](const wchar_t* message) {
            if (stopRequested) {
                exitState = EngineState::Idle; // the user stopped us mid-recovery; not a failure
            } else if (!IsWindow(config.targetWindow)) {
                exitError = L"The captured window was closed.";
                exitState = EngineState::Idle;
            } else {
                exitError = message;
                exitState = EngineState::Failed;
            }
        };

        int64_t windowStart = QpcNow();
        uint32_t windowCaptured = 0, windowPresented = 0, windowGenerated = 0;
        uint32_t windowReal = 0, windowRealWithGen = 0; // real frames seen while generation is requested / given generated frames
        int64_t lastStatsLogQpc = QpcNow();
        uint32_t activeMultiplier = 1;
        double latencyAvg = 0.0, cpuAvg = 0.0;
        Size lastTextureSize;
        bool renderFailureLogged = false, waitTimeoutLogged = false;
        wchar_t displayTimingText[4]{};
        displayTimingEnabled = GetEnvironmentVariableW(L"MOONSR_FG_DISPLAY_STATS", displayTimingText,
                                                        static_cast<DWORD>(std::size(displayTimingText))) && displayTimingText[0] == L'1';
        // Give interpolation a small amount of time before the first slot. Both generated and real slots move
        // together, avoiding an extra delay between the pair. The override supports controlled comparisons.
        double scheduleDelayMs = 1.0;
        wchar_t delayText[24]{};
        if (GetEnvironmentVariableW(L"MOONSR_FG_SCHEDULE_DELAY_MS", delayText, static_cast<DWORD>(std::size(delayText)))) {
            wchar_t* end = nullptr;
            const double value = wcstod(delayText, &end);
            if (end != delayText && *end == L'\0' && std::isfinite(value)) scheduleDelayMs = std::clamp(value, 0.0, 8.0);
        }
        double catchupSlots = -1.0; // automatic during learning or sustained presentation backlog
        wchar_t catchupText[24]{};
        if (GetEnvironmentVariableW(L"MOONSR_FG_CATCHUP_SLOTS", catchupText, static_cast<DWORD>(std::size(catchupText)))) {
            wchar_t* end = nullptr;
            const double value = wcstod(catchupText, &end);
            if (end != catchupText && *end == L'\0' && std::isfinite(value) && value >= 0.0)
                catchupSlots = value == 0.0 ? 0.0 : std::clamp(value, 1.0, 4.0);
        }
        bool haveAppliedOrder = false, lastUpscaleFirst = false;
        bool haveFgInputMode = false, lastFgPreScaled = false;
        int64_t lastLateLearningProbe = 0;
        ScaleSettings lastUpscaleSettings;
        PipelineOrder appliedOrder;

        while (!stopRequested) {
            if (!presenter->PumpMessages()) {
                if (!Recover(L"output window invalidated (display change or closed)")) {
                    recoveryFailed(L"The output window could not be recreated.");
                    break;
                }
                continue;
            }

            ScaleSettings settings;
            FrameGenConfig fgCfg;
            PipelineOrder order;
            AlsrSettings alsrSettings;
            PictureSettings pictureSettings;
            std::optional<NeuralChange> neuralChange;
            bool alsrReset = false;
            bool fgLearningReset = false;
            bool vsync;
            {
                std::lock_guard lock(mutex);
                settings = liveScale;
                vsync = liveVsync;
                fgCfg = liveFrameGen;
                order = liveOrder;
                alsrSettings = liveAlsr;
                pictureSettings = livePicture;
                if (pendingIndex >= pendingCount) neuralChange.swap(pendingNeuralChange);
                alsrReset = alsrResetRequested;
                alsrResetRequested = false;
                fgLearningReset = fgLearningResetRequested;
                fgLearningResetRequested = false;
                overlayEnabled = liveFrameCounter;
            }

            // The generator's lifetime follows the REQUESTED multiplier. The effective multiplier is decided once
            // per real frame, when it is scheduled.
            const uint32_t requested = std::clamp<uint32_t>(fgCfg.multiplier, 1, 6);
            FrameGenSettings runtimeFgSettings = fgCfg.settings;
            runtimeFgSettings.preciseMotion = runtimeFgSettings.preciseMotion || requested >= 3;
            if (requested != fgRequested) {
                fgRequested = requested;
                fgEffective = StartingFrameMultiplier(requested, fgEffective);
                overrunCount = overrunTotal = 0;
                raiseStreak = 0;
                fgRunningLate = false;
            }
            // Moon ALSR selected? It needs the generator as its motion estimator even with frame generation off.
            const bool wantAlsr = settings.enabled && settings.scaler == ScalerKind::MoonALSR && !alsrFailed;
            alsrSettings.sharpness = settings.sharpness;
            if ((requested > 1 || wantAlsr) && !fg && !fgCreateFailed) {
                std::wstring fgError;
                fg = FrameGenerator::Create(*device, fgError);
                if (!fg) {
                    fgCreateFailed = true;
                    MSR_LOG_ERROR(L"Frame generation unavailable: {}", fgError);
                }
            } else if (requested <= 1 && !wantAlsr && fg && pendingIndex >= pendingCount) {
                // Only once the presents still owed have been shown, so the last real frame is not lost.
                ResetFrameGen();
            }
            if (fgLearning) {
                if (!fgCfg.adaptiveLearning || requested <= 1) {
                    fgadaptive::ProfileContext unused;
                    fgLearning->Configure(false, unused, runtimeFgSettings, fgLearningDirectory);
                }
                fgLearning->Tick(false);
                if (fgLearningReset) fgLearning->ResetLearning();
            }
            if (fg) {
                const bool useLearned = fgLearning && fgCfg.adaptiveLearning && requested > 1 &&
                                        fgLearningBaseline == runtimeFgSettings;
                fg->SetSettings(useLearned ? fgLearning->EffectiveSettings() : runtimeFgSettings);
            }
            if (wantAlsr && fg && !alsr) {
                std::wstring alsrError;
                alsr = AlsrUpscaler::Create(*device, alsrError);
                if (!alsr) {
                    alsrFailed = true;
                    MSR_LOG_ERROR(L"Moon ALSR unavailable ({}); using Lanczos-3 instead", alsrError);
                } else {
                    const std::wstring dir = GameProfileDirectory(fgLearningDirectory, fgLearningGame);
                    // Older ALSR versions used only an executable name. Move the old file once, without
                    // overwriting a profile already associated with this full path.
                    if (!dir.empty() && gameKey != L"unknown") {
                        std::error_code ec;
                        std::filesystem::create_directories(dir, ec);
                        if (!ec) {
                            const auto oldPath = std::filesystem::path(fgLearningDirectory) / (gameKey + L".alsr");
                            const auto newPath = std::filesystem::path(dir) / L"upscaling.alsr";
                            MoveFileW(oldPath.c_str(), newPath.c_str());
                        }
                    }
                    if (!alsr->SetProfile(dir, dir.empty() ? L"" : L"upscaling")) MSR_LOG_WARN(L"Moon ALSR: profile folder '{}' unavailable; learning will not persist", dir);
                    MSR_LOG_INFO(L"Moon ALSR active for game '{}'", gameKey);
                }
            } else if (!wantAlsr && alsr && pendingIndex >= pendingCount) {
                ReleaseAlsr();
            }
            if (alsr && alsrReset) alsr->ResetLearning();
            const bool useAlsr = wantAlsr && alsr && fg;
            if (!fg) activeMultiplier = 1;
            // Load order: is upscaling placed before frame generation?
            order = Engine::NormalizeOrder(order);
            if (pendingIndex >= pendingCount && (!haveAppliedOrder || order != appliedOrder)) {
                if (haveAppliedOrder) {
                    if (fg) { fg->Reset(); fgHasHistory = false; }
                    if (alsr) alsr->Reset();
                    if (alsrMotion) alsrMotion->Reset();
                    neuralReset = true;
                    fgLearningLastDiscontinuity = QpcNow();
                }
                appliedOrder = order;
            }
            order = appliedOrder;
            const auto posOf = [&order](PipelineStage st) {
                return std::find(order.stages.begin(), order.stages.end(), st) - order.stages.begin();
            };
            const bool orderUpscaleFirst = posOf(PipelineStage::Upscaling) < posOf(PipelineStage::FrameGeneration);
            const bool neuralBeforeFg = posOf(PipelineStage::NeuralRendering) < posOf(PipelineStage::FrameGeneration);
            const bool upscaleBeforeNeural = posOf(PipelineStage::Upscaling) < posOf(PipelineStage::NeuralRendering);
            if (neuralChange && ApplyNeuralChange(*neuralChange)) {
                if (neuralBeforeFg && fg) { fg->Reset(); fgHasHistory = false; }
                if (alsr) alsr->Reset();
                if (alsrMotion) alsrMotion->Reset();
                fgLearningLastDiscontinuity = QpcNow();
            }
            const bool upscaleFirst = orderUpscaleFirst && settings.enabled;
            // Finish the old schedule first, then discard history before changing the generator's input.
            if (pendingIndex >= pendingCount && (!haveAppliedOrder || lastUpscaleFirst != upscaleFirst ||
                                                 (upscaleFirst && lastUpscaleSettings != settings))) {
                if (haveAppliedOrder && fg) { fg->Reset(); fgHasHistory = false; }
                if (haveAppliedOrder && alsr) alsr->Reset();
                if (alsrMotion) alsrMotion->Reset();
                fgLearningLastDiscontinuity = QpcNow();
                lastUpscaleFirst = upscaleFirst;
                lastUpscaleSettings = settings;
                haveAppliedOrder = true;
            }
            if (vsync != appliedVsync) {
                presenter->SetVsync(vsync);
                appliedVsync = vsync;
            }

            // Wait before picking a new capture so we do not queue stale real frames. Consecutive VSync-scheduled
            // frames already have their source and due times; waiting here between them adds a refresh of delay.
            // Let Present and the swapchain's buffer limit provide backpressure for those frames instead.
            if (waitForSwapchain && !(vsync && fg && pendingIndex < pendingCount)) {
                if (const HANDLE waitable = presenter->FrameLatencyWaitable()) {
                    const int64_t waitStart = QpcNow();
                    const DWORD waitResult = WaitForSingleObject(waitable, 100);
                    Ema(swapchainWaitAvg, QpcToMs(QpcNow() - waitStart));
                    if (waitResult == WAIT_TIMEOUT && !waitTimeoutLogged) {
                        waitTimeoutLogged = true;
                        MSR_LOG_WARN(L"Swapchain did not become ready within 100 ms (display asleep or occluded?)");
                    }
                }
                waitForSwapchain = false;
            }
            if (fgLearning) {
                fgLearning->SetOverloaded(fgRunningLate);
                // Poll results here. Submit learning work only after capture confirms there is no frame waiting.
                fgLearning->Tick(false);
            }

            CursorState cursorState;
            if (cursor) {
                cursorState = cursor->Poll(config.targetWindow);
                cursor->SetSystemCursorHidden(cursorState.visible);
            }

            // Presents still owed for the newest real frame come first: generated frames, then the real one.
            if (fg && pendingIndex < pendingCount) {
                const Pending p = pending[pendingIndex++];
                const double activeCatchup = catchupSlots >= 0.0 ? catchupSlots :
                    ((fgCfg.adaptiveLearning || fgRunningLate) ? 1.5 : 0.0);
                if (activeCatchup > 0.0 &&
                    ShouldSkipStaleGenerated(vsync, p.real, pendingSlotMs, QpcToMs(QpcNow() - p.dueQpc), activeCatchup)) {
                    std::lock_guard lock(mutex);
                    ++stats.generatedSkipped;
                    continue;
                }
                // With VSync off, move subsequent slots back when a slot is
                // late so presents do not bunch up; with VSync on, the display already spaces them by refreshes.
                // Submit the GPU work BEFORE waiting for the slot, so that the frame is ready when its time comes
                // instead of starting to render at its due time.
                const int64_t t0 = QpcNow();
                const Size fs = fg->FrameSize();
                ID3D11Texture2D* tex = fg->RealFrame(0);
                Size texSize = fs;
                bool alreadyUpscaled = pendingPreScaled;
                if (p.real) {
                    if (pendingAlsr && alsrOut) { // reconstructed by Moon ALSR when the frame was pushed
                        tex = alsrOut.Get();
                        texSize = alsrSize;
                        alreadyUpscaled = true;
                    }
                } else {
                    tex = (EnsureFgOut(fs) && fg->Interpolate(p.t, fgOutRtv.Get())) ? fgOut.Get() : nullptr;
                    // Generated frames get ALSR's single-frame reconstruction so they match the real frames' look.
                    if (tex && pendingAlsr && alsr && alsrGen &&
                        alsr->RenderSingle(fgOut.Get(), alsrGenRtv.Get(), alsrSize, alsrSettings)) {
                        tex = alsrGen.Get();
                        texSize = alsrSize;
                        alreadyUpscaled = true;
                    }
                }
                if (tex && neural && !pendingNeuralDone) {
                    RectI currentRect{0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)};
                    if (upscaleBeforeNeural && settings.enabled && !alreadyUpscaled) {
                        const RectI dr = ScalePass::ComputeDestRect(texSize, presenter->BackBufferSize(), settings.fit);
                        const Size ds{static_cast<uint32_t>(dr.Width()), static_cast<uint32_t>(dr.Height())};
                        ScaleSettings fill = settings; fill.fit = FitMode::Stretch;
                        if (ds.Empty() || !EnsurePreScaled(ds) || !scale->Render(tex, currentRect, preScaledRtv.Get(), ds, fill, nullptr)) {
                            exitError = L"Could not upscale a frame before neural rendering."; exitState = EngineState::Failed; break;
                        }
                        tex = preScaled.Get(); texSize = ds; alreadyUpscaled = true;
                        currentRect = {0, 0, static_cast<int32_t>(ds.width), static_cast<int32_t>(ds.height)};
                    }
                    const Size neuralInputSize{static_cast<uint32_t>(currentRect.Width()), static_cast<uint32_t>(currentRect.Height())};
                    std::wstring neuralError;
                    if (!NeuralProcess(tex, currentRect, p.dueQpc, pictureSettings, neuralError)) {
                        if (!neuralChangedLive) {
                            exitError = L"Neural rendering stopped: " + neuralError; exitState = EngineState::Failed; break;
                        }
                        DisableNeuralAfterChange(neuralError);
                    } else {
                        texSize = {static_cast<uint32_t>(currentRect.Width()), static_cast<uint32_t>(currentRect.Height())};
                        std::lock_guard lock(mutex);
                        stats.neuralRenderingActive = true;
                        stats.neuralInputSize = neuralInputSize;
                        ++stats.neuralProcessedFrames;
                        if (!p.real) ++stats.neuralProcessedGeneratedFrames;
                    }
                }
                if (tex && pendingAlsrAfterNeural && alsr) {
                    bool ok = false;
                    if (p.real) {
                        if (!alsrMotion && !alsrMotionFailed) {
                            std::wstring motionError; alsrMotion = FrameGenerator::Create(*device, motionError);
                            if (!alsrMotion) { alsrMotionFailed = true; MSR_LOG_ERROR(L"Moon ALSR motion unavailable: {}", motionError); }
                        }
                        const RectI r{0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)};
                        ok = alsrMotion && alsrMotion->PushFrame(tex, r, pendingRealArrivalQpc) &&
                             alsr->Render(*alsrMotion, alsrOutRtv.Get(), alsrSize, alsrSettings);
                        if (ok) tex = alsrOut.Get();
                    } else {
                        ok = alsr->RenderSingle(tex, alsrGenRtv.Get(), alsrSize, alsrSettings);
                        if (ok) tex = alsrGen.Get();
                    }
                    if (!ok) { exitError = L"Moon ALSR could not run after neural rendering."; exitState = EngineState::Failed; break; }
                    texSize = alsrSize; alreadyUpscaled = true;
                }
                RectI destRect;
                bool rendered = false, toScene = false;
                if (tex) {
                    // Frames of an "upscaling first" schedule are already at output size: copy them 1:1 (the
                    // destination rectangle the fit mode yields for an image of that size is the same rectangle).
                    ScaleSettings blit = settings;
                    if (alreadyUpscaled) {
                        blit.enabled = true;
                        blit.scaler = ScalerKind::Nearest;
                        blit.sharpness = 0.0f;
                    }
                    rendered = RenderOnly(tex, RectI{0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)},
                                          blit, destRect, toScene);
                }
                const double cpuMs = QpcToMs(QpcNow() - t0);

                if (const int64_t late = QpcNow() - p.dueQpc;
                    pendingSlotMs > 0.0 && QpcToMs(late) > pendingSlotMs * 0.5) {
                    // VSync already spaces presents by at least one refresh. Shifting the next slot by the full
                    // capture delay as well can force an extra refresh between every pair and throttle the output.
                    if (!vsync)
                        for (uint32_t i = pendingIndex; i < pendingCount; ++i) pending[i].dueQpc += late;
                    if (!p.real) {
                        std::lock_guard lock(mutex);
                        stats.generatedLate++;
                    }
                }
                // Submit each frame at its scheduled time. Subtracting a whole refresh caused generated and
                // real frames to be submitted back-to-back on 60 -> 120 FPS / 144 Hz, despite a correct FPS count.
                WaitUntil(p.dueQpc);
                recInfo.kind = p.real ? RecordedKind::Real : RecordedKind::Generated;
                recInfo.phase = p.t;
                recInfo.dueQpc = p.dueQpc;

                PresentStatus ps = PresentStatus::Occluded;
                if (rendered && toScene) {
                    // Sample the cursor as late as possible: after the wait, right before compositing.
                    cursorState = cursor->Poll(config.targetWindow);
                    ps = ComposeAndPresent(cursorState);
                } else if (rendered) {
                    ps = PresentBackBuffer();
                }
                const int64_t t1 = QpcNow();
                const bool deviceLost = ps == PresentStatus::DeviceLost || device->DeviceStatus() != S_OK;
                if (deviceLost || ps == PresentStatus::Error) {
                    if (!Recover(deviceLost ? L"device lost" : L"present failure")) {
                        recoveryFailed(L"The graphics device was lost and could not be recovered.");
                        break;
                    }
                    continue;
                }
                if (ps == PresentStatus::Ok) {
                    waitForSwapchain = true;
                    windowPresented++;
                    if (p.real) {
                        Ema(latencyAvg, QpcToMs(t1 - pendingRealArrivalQpc));
                        // Everything owed for one real frame must be shown within one real frame time of its
                        // arrival. Counted only for schedules built on a stable frame time, so that hitches in the
                        // game do not look like overload on our side.
                        if (scheduledMultiplier > 1 && pendingStable) {
                            overrunTotal++;
                            if (QpcToMs(t1 - pendingBaseQpc) > periodMs * 1.05) overrunCount++;
                        }
                    } else {
                        windowGenerated++;
                    }
                }
                Ema(cpuAvg, cpuMs + QpcToMs(QpcNow() - t1));
                std::lock_guard lock(mutex);
                stats.inputSize = lastInputSize.Empty() ? fs : lastInputSize;
                stats.outputSize = presenter->BackBufferSize();
                if (rendered) stats.destRect = destRect;
                if (ps == PresentStatus::Ok) {
                    stats.framesPresented++;
                    if (!p.real) stats.generatedPresented++;
                }
                stats.scalerName = !settings.enabled ? L"Off (shown 1:1)"
                                  : (settings.scaler == ScalerKind::MoonALSR && !pendingAlsr && !pendingAlsrAfterNeural && !(useAlsr && pendingPreScaled)) ? L"Lanczos-3 (Moon ALSR not active)"
                                  : ScalePass::ScalerName(settings.scaler);
            } else {
            {
                uint32_t wantFrames = 0;
                std::wstring wantDir;
                {
                    std::lock_guard lock(mutex);
                    wantFrames = recordRequestFrames;
                    wantDir = recordRequestDir;
                    recordRequestFrames = 0;
                }
                if (wantFrames > 0) {
                    std::wstring recError;
                    if (!recorder) recorder = FrameRecorder::Create(*device, recError);
                    const std::wstring description = std::format(
                        L"scaler '{}', sharpness {:.2f}, frame generation requested x{} effective x{}, input {}x{}, output {}x{}",
                        ScalePass::ScalerName(settings.scaler), settings.sharpness, fgRequested, activeMultiplier,
                        stats.inputSize.width, stats.inputSize.height, stats.outputSize.width, stats.outputSize.height);
                    if (!recorder || !recorder->Begin(wantDir, wantFrames, description))
                        MSR_LOG_WARN(L"Could not start a recording in '{}': {}", wantDir, recError);
                    else
                        MSR_LOG_INFO(L"Recording {} frames to '{}'", wantFrames, wantDir);
                }
                std::wstring doneDir;
                const bool finished = recorder && recorder->PollFinished(doneDir);
                const bool active = recorder && recorder->Active();
                if (finished) MSR_LOG_INFO(L"Recording finished: '{}'", doneDir);
                std::lock_guard lock(mutex);
                stats.recording = active;
                if (finished) stats.lastRecordingDir = doneDir;
            }

            CapturedFrame frame;
            const int64_t captureCheckQpc = QpcNow();
            const bool learningProbe = fgRunningLate &&
                (!lastLateLearningProbe || QpcToMs(captureCheckQpc - lastLateLearningProbe) >= 8000.0);
            const bool learningReady = fgLearning && fgLearning->ReadyForEvaluation() &&
                (!fgRunningLate || learningProbe) && periodMs > 0.0 && !(recorder && recorder->Active()) &&
                (!fgLearningLastDiscontinuity || QpcToMs(captureCheckQpc - fgLearningLastDiscontinuity) > 1000.0);
            const uint32_t captureWaitMs = learningReady ? 0 : (cursor ? kAcquireTimeoutCursorMs : kAcquireTimeoutMs);
            const CaptureStatus cs = capture->AcquireFrame(captureWaitMs, frame);
            if (cs == CaptureStatus::Timeout) {
                if (learningReady) {
                    fgLearning->Tick(true);
                    if (learningProbe) lastLateLearningProbe = captureCheckQpc;
                }
                // Nothing new from the game; the last presented frame stays on screen. With a software cursor,
                // re-composite when the cursor moved, changed shape or appeared / disappeared.
                if (cursor && sceneValid && !(cursorState == drawnCursor)) {
                    const PresentStatus ps = ComposeAndPresent(cursorState);
                    const bool deviceLost = ps == PresentStatus::DeviceLost || device->DeviceStatus() != S_OK;
                    if (deviceLost || ps == PresentStatus::Error) {
                        if (!Recover(deviceLost ? L"device lost" : L"present failure")) {
                            recoveryFailed(L"The graphics device was lost and could not be recovered.");
                            break;
                        }
                        continue;
                    }
                    if (ps == PresentStatus::Ok) waitForSwapchain = true;
                }
            } else if (cs == CaptureStatus::Ok) {
                const int64_t t0 = QpcNow();
                ++realCounter;
                lastInputSize = {static_cast<uint32_t>(frame.contentRect.Width()), static_cast<uint32_t>(frame.contentRect.Height())};
                ID3D11Texture2D* pipelineTexture = frame.texture;
                RectI pipelineRect = frame.contentRect;
                if (neural && device->Info().vendor == Vendor::Nvidia) {
                    neuralReset = neuralReset || frame.contentRect != neuralRect ||
                        (neuralSequence && frame.sequence != neuralSequence + 1) ||
                        (neuralQpc && (frame.captureQpc <= neuralQpc || QpcToMs(frame.captureQpc - neuralQpc) > 1000.0));
                    neuralSequence = frame.sequence; neuralRect = frame.contentRect; neuralQpc = frame.captureQpc;
                    const bool originalFeedsNeural = neuralBeforeFg && !(upscaleBeforeNeural && settings.enabled);
                    if (!originalFeedsNeural && !SaveNeuralOriginal(frame.texture, frame.contentRect)) {
                        capture->ReleaseFrame(); exitError = L"Could not preserve the original frame for neural optical flow.";
                        exitState = EngineState::Failed; break;
                    }
                }
                if (recorder && recorder->Active()) {
                    RecordedFrameInfo in;
                    in.kind = RecordedKind::Input;
                    in.actualQpc = frame.captureQpc;
                    in.realIndex = realCounter;
                    recorder->Capture(frame.texture, frame.contentRect, in);
                }
                if (frame.textureSize != lastTextureSize) {
                    // The capture pool was recreated; drop views that pin the old textures.
                    scale->ResetSourceCache();
                    lastTextureSize = frame.textureSize;
                }
                // Real frame time estimate; maintained with and without generation.
                const bool stable = UpdatePeriod(frame);
                if (!stable || frame.droppedBefore > 4) fgLearningLastDiscontinuity = QpcNow();
                const uint32_t effective = fg ? DecideMultiplier(vsync) : 1;
                activeMultiplier = effective;
                const bool willGenerate = fg && effective > 1;
                bool capturePreScaled = false;
                bool captureNeuralDone = false;
                // If neural rendering follows upscaling but precedes frame generation, both operations must
                // happen on the real capture before it enters the generator.
                if (neural && (neuralBeforeFg || !willGenerate)) {
                    if (upscaleBeforeNeural && settings.enabled) {
                        const Size content{static_cast<uint32_t>(pipelineRect.Width()), static_cast<uint32_t>(pipelineRect.Height())};
                        const RectI dr = ScalePass::ComputeDestRect(content, presenter->BackBufferSize(), settings.fit);
                        const Size ds{static_cast<uint32_t>(dr.Width()), static_cast<uint32_t>(dr.Height())};
                        if (useAlsr) {
                            if (!alsrMotion && !alsrMotionFailed) {
                                std::wstring e; alsrMotion = FrameGenerator::Create(*device, e);
                                if (!alsrMotion) { alsrMotionFailed = true; MSR_LOG_ERROR(L"Moon ALSR motion unavailable: {}", e); }
                            }
                            capturePreScaled = alsrMotion && !ds.Empty() && EnsureAlsrTargets(ds) &&
                                alsrMotion->PushFrame(pipelineTexture, pipelineRect, frame.captureQpc) &&
                                alsr->Render(*alsrMotion, alsrOutRtv.Get(), ds, alsrSettings);
                            if (capturePreScaled) pipelineTexture = alsrOut.Get();
                        } else {
                            ScaleSettings fill = settings; fill.fit = FitMode::Stretch;
                            capturePreScaled = !ds.Empty() && EnsurePreScaled(ds) &&
                                scale->Render(pipelineTexture, pipelineRect, preScaledRtv.Get(), ds, fill, nullptr);
                            if (capturePreScaled) pipelineTexture = preScaled.Get();
                        }
                        if (!capturePreScaled) {
                            capture->ReleaseFrame(); exitError = L"Could not upscale before neural rendering.";
                            exitState = EngineState::Failed; break;
                        }
                        pipelineRect = {0, 0, static_cast<int32_t>(ds.width), static_cast<int32_t>(ds.height)};
                    }
                    const Size neuralInputSize{static_cast<uint32_t>(pipelineRect.Width()), static_cast<uint32_t>(pipelineRect.Height())};
                    std::wstring neuralError;
                    if (!NeuralProcess(pipelineTexture, pipelineRect, frame.captureQpc, pictureSettings, neuralError,
                                       !capturePreScaled)) {
                        if (!neuralChangedLive) {
                            capture->ReleaseFrame(); exitError = L"Neural rendering stopped: " + neuralError;
                            exitState = EngineState::Failed; break;
                        }
                        DisableNeuralAfterChange(neuralError);
                    } else {
                        captureNeuralDone = true;
                        std::lock_guard lock(mutex);
                        stats.neuralRenderingActive = true;
                        stats.neuralInputSize = neuralInputSize;
                        ++stats.neuralProcessedFrames;
                    }
                }

                if (fg && (effective > 1 || useAlsr)) {
                    // Frame generation path: hand the frame to the generator (it keeps its own copy), then
                    // schedule the presents owed for it. Nothing is presented in this iteration.
                    // Load order: with upscaling placed before frame generation, upscale the real frame now and
                    // hand the UPSCALED image to the generator.
                    bool preScaledNow = capturePreScaled;
                    if (upscaleFirst && settings.enabled && !preScaledNow) {
                        const Size content{static_cast<uint32_t>(frame.contentRect.Width()),
                                           static_cast<uint32_t>(frame.contentRect.Height())};
                        const RectI dr = ScalePass::ComputeDestRect(content, presenter->BackBufferSize(), settings.fit);
                        const Size ds{static_cast<uint32_t>(dr.Width()), static_cast<uint32_t>(dr.Height())};
                        ScaleSettings fill = settings;
                        fill.fit = FitMode::Stretch; // the target is exactly the destination rectangle
                        if (useAlsr && effective > 1) {
                            if (!alsrMotion && !alsrMotionFailed) {
                                std::wstring motionError;
                                alsrMotion = FrameGenerator::Create(*device, motionError);
                                if (!alsrMotion) {
                                    alsrMotionFailed = true;
                                    MSR_LOG_ERROR(L"Moon ALSR separate motion estimator unavailable: {}", motionError);
                                }
                            }
                            preScaledNow = alsrMotion && !ds.Empty() && EnsureAlsrTargets(ds) &&
                                alsrMotion->PushFrame(pipelineTexture, pipelineRect, frame.captureQpc) &&
                                alsr->Render(*alsrMotion, alsrOutRtv.Get(), ds, alsrSettings);
                        } else if (!useAlsr) {
                            preScaledNow = !ds.Empty() && EnsurePreScaled(ds) &&
                                           scale->Render(pipelineTexture, pipelineRect, preScaledRtv.Get(), ds, fill, nullptr);
                        }
                    }
                    ID3D11Texture2D* fgInput = capturePreScaled ? pipelineTexture : preScaledNow ? (useAlsr ? alsrOut.Get() : preScaled.Get()) : pipelineTexture;
                    const Size fgInputSize = capturePreScaled ? Size{static_cast<uint32_t>(pipelineRect.Width()), static_cast<uint32_t>(pipelineRect.Height())} : useAlsr ? alsrSize : preScaledSize;
                    const RectI fgRect = preScaledNow ? RectI{0, 0, static_cast<int32_t>(fgInputSize.width),
                                                              static_cast<int32_t>(fgInputSize.height)} : pipelineRect;
                    if (haveFgInputMode && lastFgPreScaled != preScaledNow) {
                        fg->Reset();
                        fgHasHistory = false;
                        fgLearningLastDiscontinuity = QpcNow();
                    }
                    haveFgInputMode = true;
                    lastFgPreScaled = preScaledNow;
                    if (fgCfg.adaptiveLearning && requested > 1 && !fgLearning && !fgLearningFailed) {
                        try { fgLearning = std::make_unique<fgadaptive::Controller>(*device); }
                        catch (const std::exception&) {
                            fgLearningFailed = true;
                            MSR_LOG_ERROR(L"Adaptive frame generation could not start its profile worker");
                        }
                    }
                    if (fgLearning && fgCfg.adaptiveLearning && requested > 1) {
                        fgadaptive::ProfileContext context;
                        context.game = fgLearningGame;
                        context.gameIdentity = fgLearningGame;
                        // Different processing orders and NR runtimes produce different real inputs to frame generation.
                        if (!context.game.empty()) {
                            context.game += preScaledNow ? L"|upscaled" : L"|capture";
                            if (neural && captureNeuralDone) {
                                const auto& n = config.neuralSettings;
                                context.game += std::format(L"|nr:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
                                    config.neuralModelId, n.passes, n.resolutionPercent, n.preset, n.style, n.intensityPercent,
                                    n.structurePercent, n.tonePercent, n.skinPercent, n.detailPercent, n.colourPercent, n.autoMask);
                                if (capturePreScaled) context.game += L"|nr-after-upscale";
                                if (pictureSettings.enabled)
                                    context.game += std::format(L"|picture:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
                                        pictureSettings.exposure, pictureSettings.contrast, pictureSettings.midtones,
                                        pictureSettings.shadows, pictureSettings.highlights, pictureSettings.saturation,
                                        pictureSettings.vibrance, pictureSettings.temperature, pictureSettings.tint, pictureSettings.sharpen);
                            }
                            if (preScaledNow)
                                context.game += std::format(L"|scaler:{}:sharp:{:.3f}", static_cast<uint32_t>(settings.scaler), settings.sharpness);
                        }
                        context.gpuName = device->Info().name;
                        context.gpuVendor = device->Info().vendorId;
                        context.gpuDevice = device->Info().deviceId;
                        context.driverVersion = device->Info().driverVersion;
                        context.width = static_cast<uint32_t>(fgRect.Width());
                        context.height = static_cast<uint32_t>(fgRect.Height());
                        context.maxFlowPercent = static_cast<uint32_t>(std::lround(fgCfg.settings.flowScale * 100.0f));
                        context.hudProtection = fgCfg.settings.hudProtection;
                        context.preciseMotion = runtimeFgSettings.preciseMotion;
                        fgLearning->Configure(true, context, runtimeFgSettings, fgLearningDirectory);
                        fgLearningBaseline = runtimeFgSettings;
                        fgLearning->Tick(false);
                        fg->SetSettings(fgLearning->EffectiveSettings());
                    }
                    const bool pushed = fg->PushFrame(fgInput, fgRect, frame.captureQpc);
                    if (pushed && fgLearning && fgCfg.adaptiveLearning && requested > 1)
                        fgLearning->Observe(fgInput, fgRect, frame.captureQpc, stable && frame.droppedBefore <= 4);
                    if (pushed) { pendingPreScaled = preScaledNow; pendingNeuralDone = captureNeuralDone; }
                    // Moon ALSR: reconstruct this real frame now (exactly once per PushFrame).
                    if (pushed) {
                        pendingAlsr = false;
                        pendingAlsrAfterNeural = false;
                        if (useAlsr && !preScaledNow) {
                            const Size content{static_cast<uint32_t>(frame.contentRect.Width()),
                                               static_cast<uint32_t>(frame.contentRect.Height())};
                            const RectI dr = ScalePass::ComputeDestRect(content, presenter->BackBufferSize(), settings.fit);
                            const Size ds{static_cast<uint32_t>(dr.Width()), static_cast<uint32_t>(dr.Height())};
                            const bool alsrAfterNeural = neural && !captureNeuralDone && !upscaleBeforeNeural;
                            if (alsrAfterNeural && !ds.Empty() && EnsureAlsrTargets(ds)) {
                                pendingAlsrAfterNeural = true;
                            } else if (!alsrAfterNeural && !ds.Empty() && EnsureAlsrTargets(ds) && alsr->Render(*fg, alsrOutRtv.Get(), ds, alsrSettings)) {
                                pendingAlsr = true;
                            } else {
                                MSR_LOG_ERROR(L"Moon ALSR failed to render; switching to Lanczos-3 for this session");
                                alsrFailed = true;
                            }
                        }
                    }
                    if (device->DeviceStatus() != S_OK) {
                        capture->ReleaseFrame();
                        if (!Recover(L"device lost")) {
                            recoveryFailed(L"The graphics device was lost and could not be recovered.");
                            break;
                        }
                        continue;
                    }
                    if (pushed) {
                        capture->ReleaseFrame();
                        fgFailStreak = 0;
                        fgHasHistory = true;
                        windowCaptured += 1 + frame.droppedBefore;
                        {
                            std::lock_guard lock(mutex);
                            stats.framesCaptured += 1 + frame.droppedBefore;
                            stats.framesDropped += frame.droppedBefore;
                        }
                        // Due times are measured from the frame's ARRIVAL, not from now: a frame picked up late
                        // must not get the full hold-back on top of the delay it already had. Slots that are
                        // already past are shown as soon as they are ready (the rest of the schedule shifts).
                        const bool validSchedule = effective > 1 && stable && fg->PairInfo().valid;
                        const int64_t delay = validSchedule && vsync
                            ? static_cast<int64_t>(std::min(scheduleDelayMs, periodMs / effective * 0.2) *
                                                   static_cast<double>(QpcFrequency()) / 1000.0) : 0;
                        const int64_t base = frame.captureQpc + delay;
                        pendingCount = pendingIndex = 0;
                        pendingRealArrivalQpc = frame.captureQpc;
                        pendingBaseQpc = frame.captureQpc;
                        pendingStable = stable;
                        windowReal++;
                        if (validSchedule) {
                            windowRealWithGen++;
                            scheduledMultiplier = effective;
                            pendingSlotMs = periodMs / effective;
                            const int64_t slot = static_cast<int64_t>(pendingSlotMs * static_cast<double>(QpcFrequency()) / 1000.0);
                            for (uint32_t k = 1; k < effective; ++k)
                                pending[pendingCount++] = {base + static_cast<int64_t>(k - 1) * slot,
                                                           static_cast<float>(k) / static_cast<float>(effective), false};
                            pending[pendingCount++] = {base + static_cast<int64_t>(effective - 1) * slot, 1.0f, true};
                        } else {
                            // First frame, hitch, pause or rate change: show the real frame immediately.
                            scheduledMultiplier = 1;
                            pendingSlotMs = 0.0;
                            pending[pendingCount++] = {base, 1.0f, true};
                        }
                        continue;
                    }
                    // The generator could not take the frame (allocation failure, bad rectangle). Never freeze
                    // the picture: show this frame through the normal path below, and give up on generation if
                    // it keeps failing.
                    if (++fgFailStreak >= kMaxPushFailures) {
                        MSR_LOG_ERROR(L"Frame generation disabled: the generator rejected {} frames in a row", fgFailStreak);
                        ResetFrameGen();
                        fgCreateFailed = true;
                        activeMultiplier = 1;
                    }
                } else if (fg) {
                    windowReal++;
                }
                if (fg && effective <= 1 && !useAlsr && fgHasHistory) {
                    // Effective multiplier dropped to 1 (refresh cap): stop paying for motion
                    // estimation. Frames go straight through; the history restarts when generation resumes.
                    fg->Reset();
                    fgHasHistory = false;
                    scheduledMultiplier = 1;
                    pendingSlotMs = 0.0;
                }

                RectI destRect;
                bool rendered = false;
                ScaleSettings directSettings = settings;
                if (capturePreScaled) { directSettings.enabled = true; directSettings.scaler = ScalerKind::Nearest; directSettings.sharpness = 0.0f; }
                const PresentStatus ps =
                    RenderAndPresent(pipelineTexture, pipelineRect, directSettings, cursorState, destRect, rendered);
                // Release after Present so the OS cannot recycle the texture before our draw was submitted.
                capture->ReleaseFrame();
                const int64_t t1 = QpcNow();

                const bool deviceLost = ps == PresentStatus::DeviceLost || device->DeviceStatus() != S_OK;
                if (deviceLost || ps == PresentStatus::Error) {
                    if (!Recover(deviceLost ? L"device lost" : L"present failure")) {
                        recoveryFailed(L"The graphics device was lost and could not be recovered.");
                        break;
                    }
                    continue;
                }
                if (!rendered && !renderFailureLogged) {
                    // Healthy device but the frame could not be drawn (bad rectangle, out of memory, ...).
                    // Skip the frame; rebuilding the pipeline would not help.
                    renderFailureLogged = true;
                    MSR_LOG_ERROR(L"Frame skipped: scaling failed on a healthy device (logged once)");
                }

                windowCaptured += 1 + frame.droppedBefore;
                if (ps == PresentStatus::Ok) {
                    waitForSwapchain = true;
                    windowPresented++;
                    Ema(latencyAvg, QpcToMs(t1 - frame.captureQpc));
                }
                Ema(cpuAvg, QpcToMs(t1 - t0));

                std::lock_guard lock(mutex);
                stats.inputSize = {static_cast<uint32_t>(frame.contentRect.Width()),
                                   static_cast<uint32_t>(frame.contentRect.Height())};
                stats.outputSize = presenter->BackBufferSize();
                stats.destRect = destRect;
                stats.framesCaptured += 1 + frame.droppedBefore;
                stats.framesDropped += frame.droppedBefore;
                if (ps == PresentStatus::Ok) stats.framesPresented++;
                stats.scalerName = !settings.enabled ? L"Off (shown 1:1)"
                                  : (settings.scaler == ScalerKind::MoonALSR && !pendingAlsr) ? L"Lanczos-3 (Moon ALSR not active)"
                                  : ScalePass::ScalerName(settings.scaler);
            } else if (cs == CaptureStatus::TargetLost) {
                exitError = L"The captured window was closed or can no longer be captured.";
                exitState = EngineState::Idle;
                break;
            } else { // DeviceLost or Error
                if (!Recover(cs == CaptureStatus::DeviceLost ? L"capture reported device lost" : L"capture error")) {
                    recoveryFailed(L"Capture failed and could not be restarted.");
                    break;
                }
                continue;
            }
            } // acquire path

            const double elapsed = QpcToMs(QpcNow() - windowStart);
            if (elapsed >= kStatsIntervalMs) {
                std::lock_guard lock(mutex);
                stats.capturedFps = windowCaptured * 1000.0 / elapsed;
                stats.presentedFps = windowPresented * 1000.0 / elapsed;
                stats.generatedFps = windowGenerated * 1000.0 / elapsed;
                stats.frameGenMultiplier = activeMultiplier;
                stats.frameGenLimited = fg && activeMultiplier < fgRequested;
                stats.frameGenDelayMs = scheduledMultiplier > 1 ? pendingSlotMs * (scheduledMultiplier - 1) : 0.0;
                stats.frameGenLearning = fgLearning ? fgLearning->Status() : fgadaptive::ControllerStatus{};
                if (fgLearningFailed) stats.frameGenLearning.note = L"Adaptive learning could not start (see the log).";
                else if (fgCfg.adaptiveLearning && requested > 1 && activeMultiplier <= 1 && !useAlsr) {
                    stats.frameGenLearning.enabled = true;
                    stats.frameGenLearning.collecting = false;
                    stats.frameGenLearning.requestedFlow = fgCfg.settings.flowScale;
                    stats.frameGenLearning.effectiveFlow = fgCfg.settings.flowScale;
                    stats.frameGenLearning.note = L"Learning is paused until frame generation runs; check the display refresh limit.";
                }
                stats.latencyMs = latencyAvg;
                overlayRendered = static_cast<uint32_t>(stats.capturedFps + 0.5);
                overlayTotal = static_cast<uint32_t>(stats.presentedFps + 0.5);
                stats.cpuFrameMs = cpuAvg;
                stats.presentCallMs = presentCallAvg;
                stats.swapchainWaitMs = swapchainWaitAvg;
                if (presentIntervalCount) {
                    auto intervals = presentIntervals;
                    std::sort(intervals.begin(), intervals.begin() + presentIntervalCount);
                    stats.presentIntervalMedianMs = intervals[presentIntervalCount / 2];
                    stats.presentIntervalP95Ms = intervals[(presentIntervalCount - 1) * 95 / 100];
                }
                stats.displayIntervalSamples = static_cast<uint32_t>(displayIntervalCount);
                if (displayIntervalCount) {
                    auto intervals = displayIntervals;
                    std::sort(intervals.begin(), intervals.begin() + displayIntervalCount);
                    stats.displayIntervalMedianMs = intervals[displayIntervalCount / 2];
                    stats.displayIntervalP95Ms = intervals[(displayIntervalCount - 1) * 95 / 100];
                }
                {
                    std::wstring text;
                    PipelineOrder actual = order;
                    if (orderUpscaleFirst && settings.enabled && !pendingPreScaled && requested > 1) {
                        auto scalePos = std::find(actual.stages.begin(), actual.stages.end(), PipelineStage::Upscaling);
                        actual.stages.erase(scalePos);
                        actual.stages.push_back(PipelineStage::Upscaling);
                    }
                    for (PipelineStage st : actual.stages) {
                        const bool active = st == PipelineStage::Upscaling ? settings.enabled
                                          : st == PipelineStage::FrameGeneration ? activeMultiplier > 1
                                          : neural != nullptr;
                        if (!active) continue;
                        if (!text.empty()) text += L" > ";
                        text += Engine::StageName(st);
                    }
                    stats.pipelineOrderText = text.empty() ? L"(no processing: frames shown as captured)" : text;
                    stats.pipelineOrderNote.clear();
                    if (orderUpscaleFirst && settings.enabled && !pendingPreScaled && requested > 1) {
                        if (!stats.pipelineOrderNote.empty()) stats.pipelineOrderNote += L" ";
                        stats.pipelineOrderNote += L"Upscaling could not run before frame generation; using frame generation first.";
                    } else if (upscaleFirst && settings.enabled && activeMultiplier > 1) {
                        if (!stats.pipelineOrderNote.empty()) stats.pipelineOrderNote += L" ";
                        stats.pipelineOrderNote += L"Upscaling runs before frame generation: motion analysis works at output "
                                                   L"resolution, which costs more GPU time than the default order.";
                    }
                }
                stats.alsrActive = pendingAlsr || pendingAlsrAfterNeural || (useAlsr && pendingPreScaled);
                stats.alsr = alsr ? alsr->LearningStatus() : AlsrLearningStatus{};
                stats.alsrNote.clear();
                if (settings.enabled && settings.scaler == ScalerKind::MoonALSR) {
                    if (alsrFailed || fgCreateFailed)
                        stats.alsrNote = L"Moon ALSR could not start on this GPU (see the log). Lanczos-3 is used instead.";
                    else if (stats.alsrActive && !stats.alsr.learningScaleSupported)
                        stats.alsrNote = L"Adaptive learning is available above 1.01x and up to 1.75x on each axis. Temporal reconstruction remains active.";
                }
                stats.frameGenCoverage = windowReal ? static_cast<double>(windowRealWithGen) / windowReal : 0.0;
                stats.frameGenNote.clear();
                if (fgRequested > 1) {
                    const double gameFps = periodSlowMs > 0.0 ? 1000.0 / periodSlowMs : stats.capturedFps;
                    if (!fg) {
                        stats.frameGenNote = L"Frame generation is unavailable on this GPU or failed to start (see the log).";
                    } else if (activeMultiplier < fgRequested && fgLimitReason == 2) {
                        stats.frameGenNote = L"Measuring the game frame rate before enabling a higher multiplier.";
                    } else if (activeMultiplier < fgRequested && fgLimitReason == 1) {
                        stats.frameGenNote = std::format(
                            L"Frame generation limited to {}X: the game runs at {:.0f} fps and {}X would exceed the {:.0f} Hz "
                            L"display. Cap the game at {:.0f} fps or lower to get {}X.",
                            activeMultiplier, gameFps, fgRequested, outputRefreshHz, outputRefreshHz / fgRequested, fgRequested);
                    } else if (activeMultiplier < fgRequested && GetModuleHandleW(L"RTSSHooks64.dll")) {
                        // RivaTuner Statistics Server injects into every Direct3D process. A frame limit set in
                        // its Global profile therefore also limits OUR presents, which is exactly this symptom.
                        stats.frameGenNote = std::format(
                            L"Frame generation limited to {}X: RivaTuner (RTSS) is hooking MoonSR, so its frame limit also "
                            L"caps MoonSR's output. In RTSS click Add, choose MoonSR.exe and set 'Application detection "
                            L"level' to None - or set the limit in the game's own RTSS profile instead of Global.",
                            activeMultiplier);
                    } else if (fgRunningLate) {
                        stats.frameGenNote = std::format(
                            L"{}X presentation is falling behind the capture pace. Processing, display synchronization "
                            L"or other system load may contribute. Try lower quality or resolution if the output stutters.",
                            activeMultiplier);
                    } else if (windowReal >= 5 && stats.frameGenCoverage < 0.7) {
                        stats.frameGenNote = std::format(
                            L"Frame generation is active on only {:.0f}% of frames: the game's frame times are irregular. "
                            L"A steady frame cap in the game helps.", stats.frameGenCoverage * 100.0);
                    }
                }
                if (QpcToMs(QpcNow() - lastStatsLogQpc) >= 5000.0) {
                    lastStatsLogQpc = QpcNow();
                    MSR_LOG_INFO(L"Stats: in {}x{} captured {:.1f} fps, displayed {:.1f} fps, generated {:.1f} fps, FG requested x{} "
                                 L"effective x{} (limit reason {}, coverage {:.0f}%, frame time {:.2f} ms fast / {:.2f} ms slow, "
                                 L"refresh {:.1f} Hz), latency {:.2f} ms, late {}, dropped {}{}{}",
                                 stats.inputSize.width, stats.inputSize.height, stats.capturedFps, stats.presentedFps,
                                 stats.generatedFps, fgRequested, activeMultiplier, fgLimitReason, stats.frameGenCoverage * 100.0,
                                 periodMs, periodSlowMs, outputRefreshHz, stats.latencyMs, stats.generatedLate,
                                 stats.framesDropped, stats.frameGenNote.empty() ? L"" : L" | ", stats.frameGenNote);
                }
                windowStart = QpcNow();
                windowCaptured = windowPresented = windowGenerated = 0;
                windowReal = windowRealWithGen = 0;
            }
        }
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() { Stop(); }

std::vector<WindowInfo> Engine::ListWindows() { return EnumerateCaptureWindows(); }
std::vector<MonitorInfo> Engine::ListMonitors() { return EnumerateMonitors(); }
std::vector<AdapterInfo> Engine::ListAdapters() { return GpuDevice::EnumerateAdapters(); }
std::vector<ScalerDesc> Engine::ListScalers() {
    auto list = ScalePass::ListScalers();
    // Moon ALSR is not a ScalePass shader; the engine provides it.
    for (auto& s : list)
        if (s.kind == ScalerKind::MoonALSR) s.available = true;
    return list;
}

bool Engine::IntelNeuralRuntimeAvailable() {
    return !NeuralBridge::IntelRuntimePath().empty();
}

bool Engine::AmdNeuralRuntimeAvailable() {
    return !NeuralBridge::AmdRuntimePath().empty();
}

ModelStore& Engine::Models() {
    static ModelStore store = [] {
        wchar_t base[MAX_PATH] = L"";
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(base) : std::wstring(L".");
        return ModelStore(dir + L"\\MoonSR\\models");
    }();
    return store;
}

bool Engine::Start(const EngineConfig& config, std::wstring& error) {
    Stop();
    impl_->stopRequested = false;
    impl_->config = config;
    impl_->neuralChangedLive = false;
    impl_->recoveryWindowStart = 0;
    impl_->recoveriesInWindow = 0;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stats = EngineStats{};
        impl_->stats.state = EngineState::Starting;
        impl_->liveScale = config.scale;
        impl_->liveVsync = config.vsync;
        impl_->liveFrameGen = config.frameGen;
        impl_->liveFrameCounter = config.showFrameCounter;
        impl_->liveOrder = NormalizeOrder(config.order);
        impl_->liveAlsr = config.alsr;
        impl_->livePicture = config.pictureSettings;
        impl_->pendingNeuralChange.reset();
        impl_->alsrResetRequested = false;
        impl_->fgLearningResetRequested = false;
        // The user-selected model is driven only through the optional, separately built bridge.
        impl_->stats.neuralRenderingActive = false;
        impl_->stats.neuralRenderingNote = config.neuralRendering
            ? L"Starting the separately built neural-rendering bridge."
            : L"";
    }
    std::promise<std::wstring> started;
    auto result = started.get_future();
    impl_->alive = true;
    try {
        impl_->thread = std::thread([this, p = std::move(started)]() mutable { impl_->ThreadMain(std::move(p)); });
    } catch (const std::system_error&) {
        impl_->alive = false;
        error = L"Could not create the pipeline thread.";
        impl_->SetState(EngineState::Failed, &error);
        return false;
    }
    error = result.get();
    if (!error.empty()) {
        impl_->thread.join();
        return false;
    }
    return true;
}

void Engine::Stop() {
    impl_->stopRequested = true;
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool Engine::IsRunning() const { return impl_->alive; }

EngineStats Engine::GetStats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

void Engine::SetScaleSettings(const ScaleSettings& settings) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveScale = settings;
}

void Engine::SetVsync(bool vsync) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveVsync = vsync;
}

bool Engine::RecordFrames(uint32_t outputFrames, const std::wstring& directory) {
    if (!impl_->alive || outputFrames == 0 || directory.empty()) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stats.recording || impl_->recordRequestFrames != 0) return false;
    impl_->recordRequestFrames = outputFrames;
    impl_->recordRequestDir = directory;
    return true;
}

PipelineOrder Engine::DefaultOrder() { return PipelineOrder{}; }

const wchar_t* Engine::StageName(PipelineStage stage) {
    switch (stage) {
    case PipelineStage::NeuralRendering: return L"Neural rendering";
    case PipelineStage::FrameGeneration: return L"Frame generation";
    default: return L"Upscaling";
    }
}

const wchar_t* Engine::StageDescription(PipelineStage stage) {
    switch (stage) {
    case PipelineStage::NeuralRendering:
        return L"Enhances image detail and appearance using your selected neural rendering model.";
    case PipelineStage::FrameGeneration:
        return L"Before upscaling it interpolates at the game's resolution (lighter); after it, at output resolution (heavier).";
    default:
        return L"Enlarges the image to the output size.";
    }
}

PipelineOrder Engine::NormalizeOrder(const PipelineOrder& order) {
    const PipelineOrder def;
    if (order.stages.size() != def.stages.size()) return def;
    for (PipelineStage st : def.stages)
        if (std::count(order.stages.begin(), order.stages.end(), st) != 1) return def;
    return order;
}

void Engine::SetAlsrSettings(const AlsrSettings& settings) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveAlsr = settings;
}

void Engine::ResetAlsrLearning() {
    std::lock_guard lock(impl_->mutex);
    impl_->alsrResetRequested = true;
}

void Engine::ResetFrameGenLearning() {
    std::lock_guard lock(impl_->mutex);
    impl_->fgLearningResetRequested = true;
}

void Engine::SetPipelineOrder(const PipelineOrder& order) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveOrder = NormalizeOrder(order);
}

void Engine::SetFrameCounter(bool show) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveFrameCounter = show;
}

void Engine::SetPictureSettings(const PictureSettings& settings) {
    std::lock_guard lock(impl_->mutex);
    impl_->livePicture = settings;
}

void Engine::SetNeuralSettings(bool enabled, const std::wstring& modelId, const NeuralSettings& settings) {
    std::lock_guard lock(impl_->mutex);
    impl_->pendingNeuralChange = Impl::NeuralChange{enabled, modelId, settings};
}

void Engine::SetFrameGen(const FrameGenConfig& frameGen) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveFrameGen = frameGen;
}

} // namespace moonsr
