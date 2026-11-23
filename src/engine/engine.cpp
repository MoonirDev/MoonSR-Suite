#include "engine/engine.h"
#include "engine/frame_pacing.h"
#include "engine/gpu_priority.h"
#include "engine/remote_stages.h"

#include "capture/capture_source.h"
#include "core/log.h"
#include "cursor/cursor_overlay.h"
#include "diag/frame_recorder.h"
#include "overlay/stats_overlay.h"
#include "gpu/cross_adapter.h"
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
    ReliefSettings liveRelief;      // guarded by mutex
    bool liveSplit = false;         // guarded by mutex
    // A/B compare (pipeline thread): the raw capture, nearest + integer scaled to the output size, once per real frame.
    bool splitView = false, splitValid = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> splitTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> splitRtv;
    Size splitSize{};
    struct NeuralChange {
        bool enabled;
        std::wstring modelId;
        NeuralSettings settings;
    };
    std::optional<NeuralChange> pendingNeuralChange; // guarded by mutex
    uint32_t recordRequestFrames = 0; // guarded by mutex; non-zero = start a recording
    std::wstring recordRequestDir;

    // Pipeline-thread state.
    EngineConfig config;
    // Main device: the Upscaling stage's GPU. It captures, upscales and presents. Frame generation and neural
    // rendering run on fgDevice / nrDevice when the user chose another GPU for them (null = the main device), through
    // the cross-GPU wrappers in engine/remote_stages.h. extraDevices owns those other devices.
    std::unique_ptr<GpuDevice> device;
    std::vector<std::unique_ptr<GpuDevice>> extraDevices;
    GpuDevice* fgDevice = nullptr;
    GpuDevice* nrDevice = nullptr;
    GpuDevice* reliefDevice = nullptr; // null when disabled, otherwise owns the relief context
    std::unique_ptr<GpuDevice> ownedReliefDevice;
    // The adapter driving the output monitor (the "Auto" GPU, also where MoonSR.exe's Direct2D UI device lives).
    // NVIDIA neural rendering anywhere else runs in a helper process (see StartNeural).
    LUID outputMonitorAdapter{};
    bool fgIsRemote = false;
    GpuDevice& NrDevice() { return nrDevice ? *nrDevice : *device; }

    // Creates the frame generator. It runs on the frame-generation GPU only when it generates frames; when it exists
    // just as Moon ALSR's motion estimator (multiplier 1) it stays on the main device next to ALSR.
    std::unique_ptr<FrameGenerator> CreateGenerator(bool generating, std::wstring& error) {
        fgIsRemote = fgDevice && generating;
        return fgIsRemote ? CreateRemoteFrameGenerator(*device, *fgDevice, error) : FrameGenerator::Create(*device, error);
    }

    // The generator whose motion field Moon ALSR reads for this real frame. A remote generator does not share its
    // motion field across GPUs, so the frame is also pushed into a local motion estimator on the main device.
    FrameGenerator* LocalMotionSource(ID3D11Texture2D* input, const RectI& rect, int64_t qpc) {
        if (!fgIsRemote) return fg.get();
        if (!alsrMotion && !alsrMotionFailed) {
            std::wstring e;
            alsrMotion = FrameGenerator::Create(*device, e);
            if (!alsrMotion) { alsrMotionFailed = true; MSR_LOG_ERROR(L"MoonSR motion estimator unavailable: {}", e); }
        }
        return alsrMotion && alsrMotion->PushFrame(input, rect, qpc) ? alsrMotion.get() : nullptr;
    }

    bool AnyDeviceLost() const {
        if (device && device->DeviceStatus() != S_OK) return true;
        if (ownedReliefDevice && ownedReliefDevice->DeviceStatus() != S_OK) return true;
        for (const auto& d : extraDevices)
            if (d->DeviceStatus() != S_OK) return true;
        return false;
    }

    std::wstring StageGpuText(const PipelineOrder& order) const {
        std::wstring text;
        for (const PipelineStage stage : order.stages) {
            const GpuDevice* selected = stage == PipelineStage::FrameGeneration ? fgDevice
                                      : stage == PipelineStage::NeuralRendering ? nrDevice
                                      : stage == PipelineStage::Relief ? reliefDevice : nullptr;
            if (stage == PipelineStage::Relief && !relief) continue;
            if (!text.empty()) text += L" > ";
            const GpuDevice* gpu = selected ? selected : device.get();
            text += std::format(L"{}: {}{}", Engine::StageName(stage), gpu->Info().name,
                                gpu != device.get() ? L" (frames copied across GPUs)" : L"");
        }
        return text;
    }
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
    double neuralCallMsAvg = 0.0, neuralCallMsMax = 0.0; // pipeline-thread time per NeuralProcess call

    bool StartNeural(std::wstring& error) {
        GpuDevice& nr = NrDevice();
        std::unique_ptr<NeuralBridge> bridge;
        // AMD and Intel runtimes carry their own model (amd-nr / intel-nr beside MoonSR.exe): no model id needed.
        if (nr.Info().vendor == Vendor::Amd || nr.Info().vendor == Vendor::Intel) {
            bridge = NeuralBridge::Create(nr, {}, config.neuralSettings, error);
        } else {
            for (const auto& model : Engine::Models().List()) {
                if (model.id != config.neuralModelId || model.kind != ModelKind::DlssNeuralRendering) continue;
                // NGX only creates features on the adapter of the oldest D3D device in a process. In MoonSR.exe that
                // is the output monitor's GPU (the Direct2D UI's device), so NVIDIA's runtime on any other GPU runs in
                // a helper process (remote_stages.h), wherever Upscaling is.
                if (!LuidIsZero(outputMonitorAdapter) && !LuidEqual(nr.Info().luid, outputMonitorAdapter)) {
                    neural = CreateHostedNeuralBridge(*device, nr, model.storedPath, config.neuralSettings, error);
                    return neural != nullptr;
                }
                bridge = NeuralBridge::Create(nr, model.storedPath, config.neuralSettings, error);
                if (bridge && nrDevice) bridge = CreateRemoteNeuralBridge(*device, *nrDevice, std::move(bridge), error);
                // The monitor's GPU is a guess at the oldest device (the UI window may be elsewhere): if NGX refuses
                // the first feature creation anyway, the stage moves to the helper process.
                neural = WithHostedFallback(*device, nr, std::move(bridge), model.storedPath, config.neuralSettings);
                return neural != nullptr;
            }
            error = L"Select an installed DLSS Neural Rendering model.";
            return false;
        }
        if (bridge && nrDevice) bridge = CreateRemoteNeuralBridge(*device, *nrDevice, std::move(bridge), error);
        neural = std::move(bridge);
        return neural != nullptr;
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
        const bool sameModel = NrDevice().Info().vendor == Vendor::Amd || NrDevice().Info().vendor == Vendor::Intel ||
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
        if (NrDevice().Info().vendor == Vendor::Nvidia && !inputIsOriginal) {
            const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
            if (neuralOriginal && neuralOriginalSize == size) {
                guide = neuralOriginal.Get();
            } else {
                if (!PrepareNeuralGuide(size, error)) return false;
                guide = neuralGuide.Get();
            }
        }
        ID3D11Texture2D* result = nullptr; RectI resultRect;
        const int64_t start = QpcNow();
        if (!neural->ProcessWithGuide(tex, rect, guide, qpc, ++neuralOutputSequence, neuralReset,
                                      picture, result, resultRect, error)) return false;
        // Pipeline-thread time spent in the bridge (submission, plus any wait the runtime does): logged with the stats.
        const double ms = QpcToMs(QpcNow() - start);
        neuralCallMsAvg = neuralCallMsAvg <= 0.0 ? ms : neuralCallMsAvg * 0.9 + ms * 0.1;
        neuralCallMsMax = std::max(neuralCallMsMax, ms);
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

    // Smart parallax analyses each original capture. Its output is consumed at the selected stage boundary.
    // A stage running after the final scale can use the kept scene and draw directly to the back buffer.
    std::unique_ptr<ReliefPass> relief;
    std::unique_ptr<FrameGenerator> reliefMotion;
    std::unique_ptr<CrossAdapterLink> reliefCaptureUpload, reliefUpload, reliefDownload;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> reliefInput, reliefOutput;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> reliefInputSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> reliefOutputRtv;
    Size reliefApplySize;
    DXGI_FORMAT reliefFormat = DXGI_FORMAT_UNKNOWN;
    bool reliefMotionFailed = false, reliefCopyFailed = false, reliefCopyProblem = false;
    bool reliefFailed = false;      // creation failed; not retried until the pipeline is rebuilt
    bool reliefAnalysed = false;    // the newest real frame was analysed (Apply is skipped otherwise)
    float reliefPhase = 1.0f;       // phase of the frame in `scene`: 1 = real, (0,1) = generated
    bool reliefInScene = false;
    bool pendingReliefDone = false;
    bool reliefBeforeScale = false;
    ScaleSettings reliefScaleSettings;
    ReliefSettings appliedRelief;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSrv;

    // Scene routing: the software cursor and relief both need the finished image in `scene`.
    bool WantScene() const { return cursor != nullptr || reliefInScene; }

    void ReleaseRelief() {
        if (relief && scale) scale->ResetSourceCache();
        relief.reset();
        reliefMotion.reset();
        reliefInputSrv.Reset();
        reliefInput.Reset();
        reliefOutputRtv.Reset();
        reliefOutput.Reset();
        reliefApplySize = {};
        reliefFormat = DXGI_FORMAT_UNKNOWN;
        reliefUpload.reset();
        reliefCaptureUpload.reset();
        reliefDownload.reset();
        reliefDevice = nullptr;
        ownedReliefDevice.reset();
        reliefMotionFailed = reliefCopyFailed = reliefCopyProblem = false;
        reliefAnalysed = false;
        reliefInScene = false;
        reliefPhase = 1.0f;
        appliedRelief = ReliefSettings{};
        sceneSrv.Reset();
        if (!cursor) { // the scene texture existed only for relief
            sceneRtv.Reset();
            scene.Reset();
            sceneSize = {};
            sceneValid = false;
        }
    }

    bool StartRelief(std::wstring& error) {
        const LUID requested = config.stageAdapters.relief;
        const LUID luid = LuidIsZero(requested) ? outputMonitorAdapter : requested;
        reliefDevice = device.get();
        if (!LuidIsZero(luid) && !LuidEqual(luid, device->Info().luid)) {
            for (const auto& d : extraDevices)
                if (LuidEqual(luid, d->Info().luid)) reliefDevice = d.get();
            if (reliefDevice == device.get()) {
                ownedReliefDevice = GpuDevice::Create(&luid, error);
                if (!ownedReliefDevice && !LuidIsZero(requested)) return false;
                if (ownedReliefDevice) {
                    reliefDevice = ownedReliefDevice.get();
                    wchar_t value[16]{};
                    const DWORD len = GetEnvironmentVariableW(L"MOONSR_GPU_PRIORITY", value, 16);
                    if (!len || len >= 16 || _wcsicmp(value, L"normal") != 0) RaiseGpuPriority(reliefDevice->Device());
                }
            }
        }
        if (reliefDevice != device.get()) {
            // The original capture and the stage input can have different sizes. Keep separate
            // transfer caches so a stage after upscaling does not recreate resources every frame.
            reliefCaptureUpload = CrossAdapterLink::Create(*device, *reliefDevice, error);
            if (!reliefCaptureUpload) return false;
            reliefUpload = CrossAdapterLink::Create(*device, *reliefDevice, error);
            if (!reliefUpload) return false;
            reliefDownload = CrossAdapterLink::Create(*reliefDevice, *device, error);
            if (!reliefDownload) return false;
        }
        relief = ReliefPass::Create(*reliefDevice, error);
        if (!relief) return false;
        MSR_LOG_INFO(L"Smart parallax runs on '{}'{}", reliefDevice->Info().name,
                     reliefUpload ? std::format(L" ({})", reliefUpload->Method()) : L"");
        return true;
    }

    // Creates / releases / configures the pass for this loop iteration (pipeline thread).
    void UpdateRelief(const ReliefSettings& requested) {
        const bool want = requested.tier != ReliefTier::Off;
        if (!want) {
            if (relief) {
                ReleaseRelief();
                MSR_LOG_INFO(L"Smart parallax off");
            }
            return;
        }
        if (!relief && !reliefFailed) {
            std::wstring error;
            if (!StartRelief(error)) {
                ReleaseRelief();
                reliefFailed = true;
                MSR_LOG_ERROR(L"Smart parallax could not start: {}", error);
                return;
            }
            appliedRelief = ReliefSettings{};
            reliefAnalysed = false;
        }
        if (relief && !(requested == appliedRelief)) {
            relief->SetSettings(requested);
            if (requested.tier != ReliefTier::MotionDepth) reliefMotion.reset();
            appliedRelief = requested;
        }
    }

    void ResetReliefHistory() {
        if (relief) relief->Reset();
        if (reliefMotion) reliefMotion->Reset();
        reliefAnalysed = false;
    }

    void LogReliefCopyFailure(const std::wstring& error) {
        if (!reliefCopyFailed) MSR_LOG_WARN(L"Smart parallax frame transfer or target failed: {}", error);
        reliefCopyFailed = true;
        reliefCopyProblem = true;
    }

    // A compatible field from an earlier stage is borrowed on the same device. Motion depth otherwise
    // measures its own field from real captures; this generator never interpolates or schedules a frame.
    void ReliefAnalyse(const CapturedFrame& frame, const MotionFieldView* motion) {
        if (!relief) return;
        ID3D11Texture2D* input = frame.texture;
        RectI rect = frame.contentRect;
        if (reliefCaptureUpload) {
            std::wstring error;
            input = reliefCaptureUpload->Transfer(input, rect, error);
            if (!input) {
                LogReliefCopyFailure(error);
                ResetReliefHistory();
                return;
            }
            rect = {0, 0, rect.Width(), rect.Height()};
            motion = nullptr; // a main-device SRV cannot be bound on the relief GPU
        }
        MotionFieldView independent;
        if (motion && reliefMotion) reliefMotion.reset();
        if (!motion && appliedRelief.tier == ReliefTier::MotionDepth && !reliefMotionFailed) {
            if (!reliefMotion) {
                std::wstring error;
                reliefMotion = FrameGenerator::Create(*reliefDevice, error);
                if (!reliefMotion) {
                    reliefMotionFailed = true;
                    MSR_LOG_WARN(L"Smart parallax motion estimator unavailable: {}", error);
                } else {
                    FrameGenSettings motionSettings;
                    motionSettings.quality = FrameGenQuality::Performance;
                    motionSettings.flowScale = 0.5f;
                    reliefMotion->SetSettings(motionSettings);
                    MSR_LOG_INFO(L"Motion depth estimates motion on '{}'", reliefDevice->Info().name);
                }
            }
            if (reliefMotion && reliefMotion->PushFrame(input, rect, frame.captureQpc)) {
                independent = reliefMotion->MotionField();
                if (independent.valid) motion = &independent;
            }
        }
        reliefAnalysed = relief->Analyse(input, rect, motion, frame.captureQpc);
    }

    bool EnsureReliefTargets(Size size, DXGI_FORMAT format) {
        if (size == reliefApplySize && format == reliefFormat && reliefOutputRtv) return true;
        reliefApplySize = {};
        reliefInputSrv.Reset();
        reliefInput.Reset();
        reliefOutputRtv.Reset();
        reliefOutput.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = format;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = reliefDevice->Device()->CreateTexture2D(&td, nullptr, &reliefInput);
        if (SUCCEEDED(hr)) hr = reliefDevice->Device()->CreateShaderResourceView(reliefInput.Get(), nullptr, &reliefInputSrv);
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(hr)) hr = reliefDevice->Device()->CreateTexture2D(&td, nullptr, &reliefOutput);
        if (SUCCEEDED(hr)) hr = reliefDevice->Device()->CreateRenderTargetView(reliefOutput.Get(), nullptr, &reliefOutputRtv);
        if (FAILED(hr)) {
            LogReliefCopyFailure(HrToString(hr));
            return false;
        }
        reliefApplySize = size;
        reliefFormat = format;
        return true;
    }

    ID3D11Texture2D* RenderRelief(ID3D11Texture2D* input, const RectI& rect, const RectI& content, float phase,
                               float outputPixelScale = 1.0f) {
        if (!relief || !reliefAnalysed || !input || rect.Empty()) return nullptr;
        std::wstring error;
        ID3D11Texture2D* source = input;
        const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
        const RectI full{0, 0, rect.Width(), rect.Height()};
        RectI sourceRect = rect;
        if (reliefUpload) {
            source = reliefUpload->Transfer(input, rect, error);
            if (!source) {
                LogReliefCopyFailure(error);
                return nullptr;
            }
            sourceRect = full;
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (!EnsureReliefTargets(size, desc.Format)) return nullptr;
        const D3D11_BOX crop{static_cast<UINT>(sourceRect.left), static_cast<UINT>(sourceRect.top), 0,
                             static_cast<UINT>(sourceRect.right), static_cast<UINT>(sourceRect.bottom), 1};
        reliefDevice->Context()->CopySubresourceRegion(reliefInput.Get(), 0, 0, 0, 0, source, 0, &crop);
        if (!relief->Apply(reliefInputSrv.Get(), size, reliefOutputRtv.Get(), size, content, phase, outputPixelScale))
            return nullptr;
        reliefCopyProblem = false;
        if (!reliefDownload) return reliefOutput.Get();
        ID3D11Texture2D* result = reliefDownload->Transfer(reliefOutput.Get(), full, error);
        if (!result) LogReliefCopyFailure(error);
        return result;
    }

    void ApplyReliefBefore(bool precedes, bool& done, ID3D11Texture2D*& texture, RectI& rect, float phase) {
        if (!precedes || done || !relief || !reliefAnalysed || !texture || rect.Empty()) return;
        const RectI full{0, 0, rect.Width(), rect.Height()};
        float outputPixelScale = 1.0f;
        if (reliefBeforeScale && reliefScaleSettings.enabled) {
            const auto w = static_cast<uint32_t>(rect.Width()), h = static_cast<uint32_t>(rect.Height());
            const RectI output = ScalePass::ComputeDestRect({w, h}, presenter->BackBufferSize(), reliefScaleSettings.fit);
            outputPixelScale = std::max(static_cast<float>(output.Width()) / static_cast<float>(w),
                                        static_cast<float>(output.Height()) / static_cast<float>(h));
        }
        if (auto* processed = RenderRelief(texture, rect, full, phase, outputPixelScale)) {
            texture = processed;
            rect = full;
        }
        done = true; // failed transfers leave this frame usable by the next stage
    }

    bool EnsureSceneSrv() {
        if (sceneSrv) return true;
        if (!scene) return false;
        const HRESULT hr = device->Device()->CreateShaderResourceView(scene.Get(), nullptr, &sceneSrv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Scene view for Smart parallax failed: {}", HrToString(hr));
            return false;
        }
        return true;
    }

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

    // MoonSR temporal upscaler (internal name Moon ALSR). It needs the generator as its motion estimator, so the
    // generator also runs when frame generation
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
            MSR_LOG_ERROR(L"MoonSR output textures {}x{} failed: {}", size.width, size.height, HrToString(hr));
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
    // Fixed mode needs up to kMaxFrameGenMultiplier entries; Auto needs (real frame time / slot) + 1, e.g. 37 for a
    // 4 fps game at 144 fps (UpdatePeriod rejects frame times above 250 ms). 128 * 16 bytes, no allocation.
    static constexpr uint32_t kMaxPending = 128;
    std::array<Pending, kMaxPending> pending{};
    uint32_t pendingCount = 0, pendingIndex = 0;
    // Auto mode (FrameGenMode::Auto): presents on a grid at the target rate, see AutoPhaseScheduler.
    AutoPhaseScheduler autoSched;
    std::array<AutoSlot, kMaxPending> autoSlots{};
    int64_t autoOriginQpc = 0;      // origin of the scheduler's millisecond clock
    bool autoMode = false;          // the current request is Auto
    uint32_t autoRequestedFps = 0;
    double autoTargetFps = 0.0;     // target after caps
    int autoLimitReason = 0;        // 0 none, 1 display refresh (V-Sync on), 2 GPU cannot keep up
    bool autoEngaged = false;       // the target is above the game's frame rate: frames are being generated
    double autoGpuCapFps = 0.0;     // 0 = none; lowered while presents run late
    uint32_t autoSlotsWin = 0, autoSkippedWin = 0; // generated slots / skipped ones in the stats window
    AutoLoadController autoLoad;
    bool pendingAuto = false;       // the schedule in `pending` is an Auto schedule
    double pendingSlotMs = 0.0;
    int64_t pendingRealArrivalQpc = 0;
    int64_t lastRealQpc = 0;
    double periodMs = 0.0; // smoothed real frame time (fast: used for slot spacing)
    double periodSlowMs = 0.0; // heavily smoothed frame time (used for the refresh-rate cap; capture timestamps jitter with the compositor)
    // Effective multiplier. Decided once per real frame (DecideMultiplier), never per loop iteration:
    //  - refresh cap with hysteresis: never more displayed frames than the monitor refreshes;
    //  There is deliberately no GPU-cost-based multiplier limit. Normally a late slot is shown; while presentation
    //  runs late (fgRunningLate), V-Sync catch-up may omit a sufficiently stale generated frame to reduce backlog.
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
        ResetFrameGen();
        fgCreateFailed = false;
        ReleaseRelief();
        reliefFailed = false;
        cursor.reset();
        sceneSrv.Reset();
        sceneRtv.Reset();
        scene.Reset();
        sceneSize = {};
        sceneValid = false;
        drawnCursor = {};
        scale.reset();
        capture.reset();
        presenter.reset();
        fgDevice = nrDevice = nullptr;
        fgIsRemote = false;
        extraDevices.clear(); // after every stage that used them
        device.reset();
    }

    bool Init(std::wstring& error) {
        if (!IsWindow(config.targetWindow)) {
            error = L"The target window no longer exists.";
            return false;
        }
        HMONITOR monitor = config.outputMonitor;
        if (!monitor) monitor = MonitorFromWindow(config.targetWindow, MONITOR_DEFAULTTONEAREST);

        // Stage GPUs. Auto (zero) is the adapter that drives the output monitor; that keeps the default free of
        // cross-GPU copies. The Upscaling stage's GPU is the main device: capture, upscaling and presentation.
        LUID monitorAdapter{};
        outputMonitorAdapter = {};
        outputRefreshHz = 0.0;
        for (const auto& m : EnumerateMonitors()) {
            if (m.handle != monitor) continue;
            monitorAdapter = m.adapterLuid;
            outputMonitorAdapter = m.adapterLuid;
            outputRefreshHz = m.refreshHz;
        }
        const auto resolve = [&](const LUID& chosen) { return LuidIsZero(chosen) ? monitorAdapter : chosen; };
        const LUID mainAdapter = resolve(config.stageAdapters.upscaling);
        const bool userChoseMain = !LuidIsZero(config.stageAdapters.upscaling);
        // Neural rendering on the output monitor's GPU runs in this process. NVIDIA's NGX accepts it only if that GPU
        // has the oldest device of the process: in MoonSR.exe the UI's Direct2D device already sees to that, in a
        // process without a UI (moonsr_cli) the stage's device must be created before the main one.
        const LUID nrAdapter = resolve(config.stageAdapters.neuralRendering);
        if (!LuidIsZero(nrAdapter) && LuidEqual(nrAdapter, monitorAdapter) && !LuidEqual(nrAdapter, mainAdapter)) {
            std::wstring stageError;
            if (auto created = GpuDevice::Create(&nrAdapter, stageError))
                extraDevices.push_back(std::move(created));
            else
                MSR_LOG_WARN(L"Neural rendering: could not create its device ahead of the main one ({}); it is retried "
                             L"after the main device", stageError);
        }
        device = GpuDevice::Create(LuidIsZero(mainAdapter) ? nullptr : &mainAdapter, error);
        if (!device && !userChoseMain && !LuidIsZero(mainAdapter)) {
            // The monitor may be driven by a display-only adapter (indirect / virtual / USB display).
            MSR_LOG_WARN(L"No device on the adapter driving the output monitor ({}); using the default adapter", error);
            error.clear();
            device = GpuDevice::Create(nullptr, error);
        }
        if (!device) return false;

        // Frame generation and neural rendering get their own device only when their GPU differs from the main one.
        // A stage left on Auto whose monitor adapter cannot run a device (display-only adapter) stays on the main GPU.
        const auto stageDevice = [&](const LUID& chosen, const wchar_t* stage) -> GpuDevice* {
            const LUID luid = resolve(chosen);
            if (LuidIsZero(luid) || LuidEqual(luid, device->Info().luid)) return nullptr;
            for (const auto& d : extraDevices)
                if (LuidEqual(d->Info().luid, luid)) return d.get();
            std::wstring stageError;
            auto created = GpuDevice::Create(&luid, stageError);
            if (!created) {
                if (!LuidIsZero(chosen)) {
                    error = std::format(L"{}: the selected GPU could not be used ({}).", stage, stageError);
                    return reinterpret_cast<GpuDevice*>(-1);
                }
                MSR_LOG_WARN(L"{}: no device on the monitor's adapter ({}); using the main GPU", stage, stageError);
                return nullptr;
            }
            extraDevices.push_back(std::move(created));
            return extraDevices.back().get();
        };
        GpuDevice* const failed = reinterpret_cast<GpuDevice*>(-1);
        fgDevice = stageDevice(config.stageAdapters.frameGeneration, L"Frame generation");
        if (fgDevice == failed) { fgDevice = nullptr; return false; }
        nrDevice = stageDevice(config.stageAdapters.neuralRendering, L"Neural rendering");
        if (nrDevice == failed) { nrDevice = nullptr; return false; }

        // MoonSR's work goes ahead of the game's on a busy GPU (see engine/gpu_priority.h). Done before any
        // submission and before the neural bridge is created: the bridge's D3D12 queue follows the device priority.
        // MOONSR_GPU_PRIORITY=normal leaves the priority alone (diagnostics and A/B measurements).
        wchar_t priorityOverride[16]{};
        const DWORD overrideLength = GetEnvironmentVariableW(L"MOONSR_GPU_PRIORITY", priorityOverride, 16);
        if (overrideLength > 0 && overrideLength < 16 && _wcsicmp(priorityOverride, L"normal") == 0) {
            MSR_LOG_WARN(L"GPU priority: left at normal (MOONSR_GPU_PRIORITY=normal)");
        } else {
            const GpuPriorityResult prio = RaiseGpuPriority(device->Device());
            for (const auto& d : extraDevices) RaiseGpuPriority(d->Device());
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
        po.keepAbove = config.targetWindow; // ordinary window kept directly above the game, not topmost
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

        std::lock_guard lock(mutex);
        stats.adapterName = device->Info().name;
        stats.adapterVendor = device->Info().vendor;
        stats.captureMethod = capture->MethodName();
        stats.outputSize = presenter->BackBufferSize();
        stats.neuralRenderingActive = false;
        stats.neuralRenderingNote.clear();
        // Which GPU runs each stage, in load order. Remote stages copy their frames across (see remote_stages.h).
        stats.stageGpuText = StageGpuText(Engine::NormalizeOrder(config.order));
        MSR_LOG_INFO(L"Pipeline initialised: main GPU '{}', capture '{}', output {}x{}; stages: {}", stats.adapterName,
                     stats.captureMethod, stats.outputSize.width, stats.outputSize.height, stats.stageGpuText);
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
        ResetAuto();
        if (scale) scale->ResetSourceCache();
    }

    void ResetAuto() {
        autoSched.Reset();
        autoEngaged = false;
        autoGpuCapFps = 0.0;
        autoSlotsWin = autoSkippedWin = 0;
        autoLoad.Reset();
        pendingAuto = false;
    }

    double AutoMs(int64_t qpc) const { return QpcToMs(qpc - autoOriginQpc); }
    int64_t AutoQpc(double ms) const {
        return autoOriginQpc + static_cast<int64_t>(ms * static_cast<double>(QpcFrequency()) / 1000.0);
    }

    // Auto: the presented rate to aim for after caps, and whether frames are generated at all. Once per real frame.
    bool DecideAuto(bool vsync) {
        double target = static_cast<double>(autoRequestedFps);
        autoLimitReason = 0;
        // With V-Sync on every present waits for a refresh: never aim above the refresh rate. With V-Sync off frames
        // are presented at the requested rate even above it (owner's decision); only the GPU limits it.
        if (vsync && outputRefreshHz > 1.0 && target > outputRefreshHz) {
            target = outputRefreshHz;
            autoLimitReason = 1;
        }
        if (autoGpuCapFps > 0.0 && autoGpuCapFps < target) {
            target = autoGpuCapFps;
            autoLimitReason = 2;
        }
        if (autoTargetFps > 0.0 && std::abs(target - autoTargetFps) > target * 0.005) autoSched.Reset(); // new grid
        autoTargetFps = target;
        const double gameFps = rateFps > 0.0 ? rateFps : (periodMs > 0.0 ? 1000.0 / periodMs : 0.0);
        if (gameFps <= 0.0) {
            autoEngaged = false;
        } else {
            // Hysteresis so that a target close to the game's rate does not switch generation on and off.
            const double ratio = target / gameFps;
            autoEngaged = autoEngaged ? ratio > 1.03 : ratio > 1.08;
        }
        return autoEngaged;
    }

    // Auto: lowers the target while presents keep running late (the GPU cannot generate that many frames) and tries
    // the full target again after a quiet stretch. Called once per stats window.
    void EvaluateAutoLoad(double gameFps) {
        if (autoMode && autoEngaged) {
            bool changed = false;
            const double before = autoGpuCapFps;
            autoGpuCapFps = autoLoad.Evaluate(autoSlotsWin, autoSkippedWin, autoTargetFps, gameFps, autoRequestedFps,
                                              autoGpuCapFps, changed);
            if (changed) {
                if (autoGpuCapFps > 0.0 && (before == 0.0 || autoGpuCapFps < before))
                    MSR_LOG_WARN(L"Frame generation (Auto): {} of {} generated frames skipped at {:.0f} fps; aiming for {:.0f} fps",
                                 autoSkippedWin, autoSlotsWin, autoTargetFps, autoGpuCapFps);
                autoSched.Reset();
            }
        }
        autoSlotsWin = autoSkippedWin = 0;
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

        // Only the display's refresh rate limits the multiplier, and only with V-Sync on (with V-Sync off frames are
        // presented above the refresh rate; owner's decision). Cost never does.
        const uint32_t target = fgRequested;
        uint32_t m = std::clamp<uint32_t>(fgEffective, 1, target);
        bool waitingToRaise = false;
        if (!vsync) {
            m = target;
            raiseStreak = 0;
        } else if (periodSlowMs > 0.0 && outputRefreshHz > 1.0) {
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
        toScene = WantScene() && EnsureScene();
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
    // A/B compare: draws the raw capture into splitTex (output size, nearest + integer scaling, centred).
    void RenderSplitRaw(ID3D11Texture2D* raw, const RectI& rect) {
        splitValid = false;
        if (!scale || !presenter) return;
        const Size size = presenter->BackBufferSize();
        if (!splitTex || splitSize.width != size.width || splitSize.height != size.height) {
            splitTex.Reset(); splitRtv.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = size.width; d.Height = size.height; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = presenter->BackBufferFormat(); d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            HRESULT hr = device->Device()->CreateTexture2D(&d, nullptr, &splitTex);
            if (SUCCEEDED(hr)) hr = device->Device()->CreateRenderTargetView(splitTex.Get(), nullptr, &splitRtv);
            if (FAILED(hr)) {
                MSR_LOG_ERROR(L"A/B compare texture {}x{} failed: {}", size.width, size.height, HrToString(hr));
                splitTex.Reset(); splitRtv.Reset(); return;
            }
            splitSize = size;
        }
        const float black[4] = {0, 0, 0, 1};
        device->Context()->ClearRenderTargetView(splitRtv.Get(), black);
        ScaleSettings s;
        s.enabled = true; s.scaler = ScalerKind::Nearest; s.fit = FitMode::Integer; s.sharpness = 0.0f;
        splitValid = scale->Render(raw, rect, splitRtv.Get(), size, s);
    }

    // A/B compare: replaces the left half of the back buffer with the raw capture.
    void ApplySplit() {
        if (!splitView || !splitValid || !splitTex) return;
        Microsoft::WRL::ComPtr<ID3D11Resource> bbTex;
        presenter->BackBufferRtv()->GetResource(&bbTex);
        const D3D11_BOX box{0, 0, 0, splitSize.width / 2, splitSize.height, 1};
        device->Context()->CopySubresourceRegion(bbTex.Get(), 0, 0, 0, 0, splitTex.Get(), 0, &box);
    }

    PresentStatus PresentBackBuffer() {
        ApplySplit();
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
        const bool useScene = WantScene() && EnsureScene();
        ID3D11RenderTargetView* target = useScene ? sceneRtv.Get() : presenter->BackBufferRtv();
        rendered = scale->Render(tex, rect, target, presenter->BackBufferSize(), settings, &destRect);
        if (!rendered) return PresentStatus::Occluded; // "nothing presented"
        if (useScene) {
            sceneValid = true;
            sceneDestRect = destRect;
            reliefPhase = 1.0f; // this path only shows real frames
            return ComposeAndPresent(cursorState);
        }
        return PresentBackBuffer();
    }

    bool EnsureScene() {
        const Size size = presenter->BackBufferSize();
        if (scene && sceneSize == size) return true;
        sceneSrv.Reset();
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

    // Brings the kept scene to the back buffer (through Smart parallax when it is on, else a plain copy), draws the
    // software cursor on top and presents. Relief runs before the cursor and the frame counter (PresentBackBuffer), so
    // neither overlay is relief-processed, and a cursor-only re-composite re-applies relief to the same frame.
    PresentStatus ComposeAndPresent(const CursorState& state) {
        ID3D11RenderTargetView* bb = presenter->BackBufferRtv();
        bool applied = false;
        ID3D11Texture2D* composed = scene.Get();
        if (reliefInScene && relief && reliefAnalysed) {
            if (!reliefUpload && EnsureSceneSrv()) {
                applied = relief->Apply(sceneSrv.Get(), sceneSize, bb, presenter->BackBufferSize(), sceneDestRect, reliefPhase);
            } else if (reliefUpload) {
                const RectI full{0, 0, static_cast<int32_t>(sceneSize.width), static_cast<int32_t>(sceneSize.height)};
                if (auto* processed = RenderRelief(scene.Get(), full, sceneDestRect, reliefPhase)) composed = processed;
            }
        }
        if (!applied) {
            Microsoft::WRL::ComPtr<ID3D11Resource> bbTex;
            bb->GetResource(&bbTex);
            device->Context()->CopyResource(bbTex.Get(), composed);
        }
        if (cursor) {
            cursor->Draw(state, config.targetWindow, bb, sceneSize, sceneDestRect);
            drawnCursor = state;
        }
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
            stats.reliefActive = false;
            stats.reliefTier = ReliefTier::Off;
            stats.reliefGpuMs = 0.0;
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
        uint32_t windowAcquired = 0; // real frames taken from capture (frameGenRatio = presented / acquired)
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
        double catchupSlots = -1.0; // automatic during sustained presentation backlog
        wchar_t catchupText[24]{};
        if (GetEnvironmentVariableW(L"MOONSR_FG_CATCHUP_SLOTS", catchupText, static_cast<DWORD>(std::size(catchupText)))) {
            wchar_t* end = nullptr;
            const double value = wcstod(catchupText, &end);
            if (end != catchupText && *end == L'\0' && std::isfinite(value) && value >= 0.0)
                catchupSlots = value == 0.0 ? 0.0 : std::clamp(value, 1.0, 4.0);
        }
        bool haveAppliedOrder = false, lastUpscaleFirst = false;
        bool haveFgInputMode = false, lastFgPreScaled = false;
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
            ReliefSettings reliefSettings;
            std::optional<NeuralChange> neuralChange;
            bool vsync;
            {
                std::lock_guard lock(mutex);
                settings = liveScale;
                vsync = liveVsync;
                fgCfg = liveFrameGen;
                order = liveOrder;
                alsrSettings = liveAlsr;
                pictureSettings = livePicture;
                reliefSettings = liveRelief;
                splitView = liveSplit;
                if (pendingIndex >= pendingCount) neuralChange.swap(pendingNeuralChange);
                overlayEnabled = liveFrameCounter;
            }

            // The generator's lifetime follows the REQUESTED multiplier. The effective multiplier is decided once
            // per real frame, when it is scheduled.
            // Fixed: `requested` is the multiplier (1 = off). Auto: the target rate drives the schedule and
            // `requested` stays 1; `fgWanted` says whether frame generation is on in either mode.
            const bool wantAuto = fgCfg.mode == FrameGenMode::Auto;
            const uint32_t requested = wantAuto ? 1u : std::clamp<uint32_t>(fgCfg.multiplier, 1, kMaxFrameGenMultiplier);
            const uint32_t requestedFps = wantAuto ? std::min<uint32_t>(fgCfg.targetFps, 1000) : 0u;
            const bool fgWanted = requested > 1 || requestedFps > 0;
            if (wantAuto != autoMode || requestedFps != autoRequestedFps) {
                // A new Auto request starts a new grid once the presents still owed have been shown.
                if (pendingIndex >= pendingCount) {
                    autoMode = wantAuto;
                    autoRequestedFps = requestedFps;
                    ResetAuto();
                }
            }
            FrameGenSettings runtimeFgSettings = fgCfg.settings;
            // Auto synthesises arbitrary phases, typically several per real frame: refine subpixel motion as for >= 3X.
            runtimeFgSettings.preciseMotion = runtimeFgSettings.preciseMotion || requested >= 3 || requestedFps > 0;
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
            // Frame generation on another GPU: the generator moves when it starts or stops generating frames (see
            // CreateGenerator), once the presents still owed have been shown.
            if (fg && fgDevice && fgIsRemote != (fgWanted) && pendingIndex >= pendingCount) ResetFrameGen();
            if ((fgWanted || wantAlsr) && !fg && !fgCreateFailed) {
                std::wstring fgError;
                fg = CreateGenerator(fgWanted, fgError);
                if (!fg) {
                    fgCreateFailed = true;
                    MSR_LOG_ERROR(L"Frame generation unavailable: {}", fgError);
                }
            } else if (!fgWanted && !wantAlsr && fg && pendingIndex >= pendingCount) {
                // Only once the presents still owed have been shown, so the last real frame is not lost.
                ResetFrameGen();
            }
            if (fg) fg->SetSettings(runtimeFgSettings);
            if (wantAlsr && fg && !alsr) {
                std::wstring alsrError;
                alsr = AlsrUpscaler::Create(*device, alsrError);
                if (!alsr) {
                    alsrFailed = true;
                    MSR_LOG_ERROR(L"MoonSR temporal upscaler unavailable ({}); using Lanczos-3 instead", alsrError);
                } else {
                    MSR_LOG_INFO(L"MoonSR temporal upscaler active");
                }
            } else if (!wantAlsr && alsr && pendingIndex >= pendingCount) {
                ReleaseAlsr();
            }
            const bool useAlsr = wantAlsr && alsr && fg;
            if (!fg) activeMultiplier = 1;
            // Load order: is upscaling placed before frame generation?
            order = Engine::NormalizeOrder(order);
            if (pendingIndex >= pendingCount && (!haveAppliedOrder || order != appliedOrder)) {
                if (haveAppliedOrder) {
                    if (fg) { fg->Reset(); fgHasHistory = false; }
                    if (alsr) alsr->Reset();
                    if (alsrMotion) alsrMotion->Reset();
                    ResetReliefHistory();
                    reliefInScene = false;
                    neuralReset = true;
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
            const auto reliefPosition = posOf(PipelineStage::Relief);
            const bool reliefBeforeFg = reliefPosition < posOf(PipelineStage::FrameGeneration);
            const bool reliefBeforeUp = reliefPosition < posOf(PipelineStage::Upscaling);
            const bool reliefBeforeNeural = reliefPosition < posOf(PipelineStage::NeuralRendering);
            reliefBeforeScale = reliefBeforeUp;
            reliefScaleSettings = settings;
            if (pendingIndex >= pendingCount) {
                const bool wasEnabled = relief != nullptr;
                UpdateRelief(reliefSettings);
                if (wasEnabled != (relief != nullptr)) {
                    if (reliefBeforeFg && fg) { fg->Reset(); fgHasHistory = false; }
                    if (reliefBeforeUp && alsr) alsr->Reset();
                    if (reliefBeforeUp && alsrMotion) alsrMotion->Reset();
                    if (reliefBeforeNeural) neuralReset = true;
                }
            }
            if (neuralChange && ApplyNeuralChange(*neuralChange)) {
                if (neuralBeforeFg && fg) { fg->Reset(); fgHasHistory = false; }
                if (alsr) alsr->Reset();
                if (alsrMotion) alsrMotion->Reset();
            }
            const bool upscaleFirst = orderUpscaleFirst && settings.enabled;
            // Finish the old schedule first, then discard history before changing the generator's input.
            if (pendingIndex >= pendingCount && (!haveAppliedOrder || lastUpscaleFirst != upscaleFirst ||
                                                 (upscaleFirst && lastUpscaleSettings != settings))) {
                if (haveAppliedOrder && fg) { fg->Reset(); fgHasHistory = false; }
                if (haveAppliedOrder && alsr) alsr->Reset();
                if (alsrMotion) alsrMotion->Reset();
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

            CursorState cursorState;
            if (cursor) {
                cursorState = cursor->Poll(config.targetWindow);
                cursor->SetSystemCursorHidden(cursorState.visible);
            }

            // Presents still owed for the newest real frame come first: generated frames, then the real one.
            if (fg && pendingIndex < pendingCount) {
                const Pending p = pending[pendingIndex++];
                const double activeCatchup = catchupSlots >= 0.0 ? catchupSlots :
                    (fgRunningLate ? 1.5 : 0.0);
                // Auto: a late generated frame is presented late, not dropped (owner's request). It is omitted only
                // when the slot after the next one is already due as well, i.e. presentation is more than one slot
                // behind and would otherwise never catch up. Real frames are never omitted.
                const bool autoStale = pendingAuto && !p.real && pendingIndex + 1 < pendingCount &&
                                       QpcNow() > pending[pendingIndex + 1].dueQpc;
                if (autoStale || (!pendingAuto && activeCatchup > 0.0 &&
                    ShouldSkipStaleGenerated(vsync, p.real, pendingSlotMs, QpcToMs(QpcNow() - p.dueQpc), activeCatchup))) {
                    if (pendingAuto) { ++autoSlotsWin; ++autoSkippedWin; }
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
                const float phase = p.real ? 1.0f : p.t;
                bool reliefDone = pendingReliefDone;
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
                    if (tex && pendingAlsr && alsr && alsrGen) {
                        RectI rect{0, 0, static_cast<int32_t>(fs.width), static_cast<int32_t>(fs.height)};
                        ApplyReliefBefore(reliefBeforeUp, reliefDone, tex, rect, phase);
                        if (alsr->RenderSingle(tex, alsrGenRtv.Get(), alsrSize, alsrSettings)) {
                            tex = alsrGen.Get();
                            texSize = alsrSize;
                            alreadyUpscaled = true;
                        }
                    }
                }
                auto renderDeferredAlsr = [&](RectI& rect) {
                    ApplyReliefBefore(reliefBeforeUp, reliefDone, tex, rect, phase);
                    if (p.real) {
                        if (!alsrMotion && !alsrMotionFailed) {
                            std::wstring error;
                            alsrMotion = FrameGenerator::Create(*device, error);
                            if (!alsrMotion) {
                                alsrMotionFailed = true;
                                MSR_LOG_ERROR(L"MoonSR motion estimator unavailable: {}", error);
                            }
                        }
                        if (!alsrMotion || !alsrMotion->PushFrame(tex, rect, pendingRealArrivalQpc) ||
                            !alsr->Render(*alsrMotion, alsrOutRtv.Get(), alsrSize, alsrSettings)) return false;
                        tex = alsrOut.Get();
                    } else {
                        if (!alsr->RenderSingle(tex, alsrGenRtv.Get(), alsrSize, alsrSettings)) return false;
                        tex = alsrGen.Get();
                    }
                    texSize = alsrSize;
                    alreadyUpscaled = true;
                    rect = {0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)};
                    return true;
                };
                if (tex && neural && !pendingNeuralDone) {
                    RectI currentRect{0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)};
                    if (upscaleBeforeNeural && settings.enabled && !alreadyUpscaled) {
                        if (pendingAlsrAfterNeural && alsr) {
                            if (!renderDeferredAlsr(currentRect)) {
                                exitError = L"MoonSR could not upscale a frame before neural rendering.";
                                exitState = EngineState::Failed;
                                break;
                            }
                        } else {
                            ApplyReliefBefore(reliefBeforeUp, reliefDone, tex, currentRect, phase);
                            const RectI dr = ScalePass::ComputeDestRect(texSize, presenter->BackBufferSize(), settings.fit);
                            const Size ds{static_cast<uint32_t>(dr.Width()), static_cast<uint32_t>(dr.Height())};
                            ScaleSettings fill = settings; fill.fit = FitMode::Stretch;
                            if (ds.Empty() || !EnsurePreScaled(ds) || !scale->Render(tex, currentRect, preScaledRtv.Get(), ds, fill, nullptr)) {
                                exitError = L"Could not upscale a frame before neural rendering."; exitState = EngineState::Failed; break;
                            }
                            tex = preScaled.Get(); texSize = ds; alreadyUpscaled = true;
                            currentRect = {0, 0, static_cast<int32_t>(ds.width), static_cast<int32_t>(ds.height)};
                        }
                    }
                    ApplyReliefBefore(reliefBeforeNeural, reliefDone, tex, currentRect, phase);
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
                if (tex && pendingAlsrAfterNeural && alsr && !alreadyUpscaled) {
                    RectI r{0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)};
                    if (!renderDeferredAlsr(r)) { exitError = L"MoonSR could not upscale the frame."; exitState = EngineState::Failed; break; }
                }
                RectI destRect;
                bool rendered = false, toScene = false;
                if (tex) {
                    RectI rect{0, 0, static_cast<int32_t>(texSize.width), static_cast<int32_t>(texSize.height)};
                    ApplyReliefBefore(reliefBeforeUp && !alreadyUpscaled, reliefDone, tex, rect, phase);
                    reliefInScene = relief && reliefAnalysed && !reliefDone;
                    reliefPhase = phase;
                    // Frames of an "upscaling first" schedule are already at output size: copy them 1:1 (the
                    // destination rectangle the fit mode yields for an image of that size is the same rectangle).
                    ScaleSettings blit = settings;
                    if (alreadyUpscaled) {
                        blit.enabled = true;
                        blit.scaler = ScalerKind::Nearest;
                        blit.sharpness = 0.0f;
                    }
                    rendered = RenderOnly(tex, rect, blit, destRect, toScene);
                }
                const double cpuMs = QpcToMs(QpcNow() - t0);

                if (const int64_t late = QpcNow() - p.dueQpc;
                    pendingSlotMs > 0.0 && QpcToMs(late) > pendingSlotMs * 0.5) {
                    // VSync already spaces presents by at least one refresh. Shifting the next slot by the full
                    // capture delay as well can force an extra refresh between every pair and throttle the output.
                    // Auto keeps its grid (the scheduler places the next real frame's slots on it).
                    if (!vsync && !pendingAuto)
                        for (uint32_t i = pendingIndex; i < pendingCount; ++i) pending[i].dueQpc += late;
                    if (!p.real) {
                        std::lock_guard lock(mutex);
                        stats.generatedLate++;
                    }
                }
                // Submit each frame at its scheduled time. Subtracting a whole refresh caused generated and
                // real frames to be submitted back-to-back on 60 -> 120 FPS / 144 Hz, despite a correct FPS count.
                WaitUntil(p.dueQpc);
                if (pendingAuto && !p.real) {
                    ++autoSlotsWin; // presented, possibly late: late presents do not count as GPU overload
                }
                recInfo.kind = p.real ? RecordedKind::Real : RecordedKind::Generated;
                recInfo.phase = p.t;
                recInfo.dueQpc = p.dueQpc;

                PresentStatus ps = PresentStatus::Occluded;
                if (rendered && toScene) {
                    // Sample the cursor as late as possible: after the wait, right before compositing.
                    if (cursor) cursorState = cursor->Poll(config.targetWindow);
                    ps = ComposeAndPresent(cursorState);
                } else if (rendered) {
                    ps = PresentBackBuffer();
                }
                const int64_t t1 = QpcNow();
                const bool deviceLost = ps == PresentStatus::DeviceLost || AnyDeviceLost();
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
                                  : (settings.scaler == ScalerKind::MoonALSR && !pendingAlsr && !pendingAlsrAfterNeural && !(useAlsr && pendingPreScaled)) ? L"Lanczos-3 (MoonSR not active)"
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
            const uint32_t captureWaitMs = cursor ? kAcquireTimeoutCursorMs : kAcquireTimeoutMs;
            const CaptureStatus cs = capture->AcquireFrame(captureWaitMs, frame);
            if (cs == CaptureStatus::Timeout) {
                // Nothing new from the game; the last presented frame stays on screen. With a software cursor,
                // re-composite when the cursor moved, changed shape or appeared / disappeared.
                if (cursor && sceneValid && !(cursorState == drawnCursor)) {
                    const PresentStatus ps = ComposeAndPresent(cursorState);
                    const bool deviceLost = ps == PresentStatus::DeviceLost || AnyDeviceLost();
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
                if (splitView) RenderSplitRaw(frame.texture, frame.contentRect);
                else if (splitTex) { splitTex.Reset(); splitRtv.Reset(); splitValid = false; splitSize = {}; }
                if (neural && NrDevice().Info().vendor == Vendor::Nvidia) {
                    neuralReset = neuralReset || frame.contentRect != neuralRect ||
                        (neuralSequence && frame.sequence != neuralSequence + 1) ||
                        (neuralQpc && (frame.captureQpc <= neuralQpc || QpcToMs(frame.captureQpc - neuralQpc) > 1000.0));
                    neuralSequence = frame.sequence; neuralRect = frame.contentRect; neuralQpc = frame.captureQpc;
                    const bool originalFeedsNeural = neuralBeforeFg && !(upscaleBeforeNeural && settings.enabled) &&
                                                     !(relief && reliefBeforeNeural);
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
                    ResetReliefHistory();
                    lastTextureSize = frame.textureSize;
                }
                // Real frame time estimate; maintained with and without generation.
                const bool stable = UpdatePeriod(frame);
                ++windowAcquired;
                // Auto uses effective = 2 to mean "generating" (the per-frame count follows from the schedule).
                const uint32_t effective = !fg ? 1u : autoMode ? (DecideAuto(vsync) ? 2u : 1u) : DecideMultiplier(vsync);
                activeMultiplier = effective;
                const bool willGenerate = fg && effective > 1;
                bool reliefDone = false;
                const bool analyseReliefEarly = reliefBeforeFg || !willGenerate;
                if (analyseReliefEarly) ReliefAnalyse(frame, nullptr);
                bool capturePreScaled = false;
                bool captureNeuralDone = false;
                // If neural rendering follows upscaling but precedes frame generation, both operations must
                // happen on the real capture before it enters the generator.
                if (neural && (neuralBeforeFg || !willGenerate)) {
                    if (upscaleBeforeNeural && settings.enabled) {
                        ApplyReliefBefore(reliefBeforeUp, reliefDone, pipelineTexture, pipelineRect, 1.0f);
                        const Size content{static_cast<uint32_t>(pipelineRect.Width()), static_cast<uint32_t>(pipelineRect.Height())};
                        const RectI dr = ScalePass::ComputeDestRect(content, presenter->BackBufferSize(), settings.fit);
                        const Size ds{static_cast<uint32_t>(dr.Width()), static_cast<uint32_t>(dr.Height())};
                        if (useAlsr) {
                            if (!alsrMotion && !alsrMotionFailed) {
                                std::wstring e; alsrMotion = FrameGenerator::Create(*device, e);
                                if (!alsrMotion) { alsrMotionFailed = true; MSR_LOG_ERROR(L"MoonSR motion estimator unavailable: {}", e); }
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
                    ApplyReliefBefore(reliefBeforeNeural, reliefDone, pipelineTexture, pipelineRect, 1.0f);
                    const Size neuralInputSize{static_cast<uint32_t>(pipelineRect.Width()), static_cast<uint32_t>(pipelineRect.Height())};
                    std::wstring neuralError;
                    if (!NeuralProcess(pipelineTexture, pipelineRect, frame.captureQpc, pictureSettings, neuralError,
                                       !capturePreScaled && !reliefDone)) {
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
                        ApplyReliefBefore(reliefBeforeUp, reliefDone, pipelineTexture, pipelineRect, 1.0f);
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
                                    MSR_LOG_ERROR(L"MoonSR separate motion estimator unavailable: {}", motionError);
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
                    RectI fgRect = preScaledNow ? RectI{0, 0, static_cast<int32_t>(fgInputSize.width),
                                                       static_cast<int32_t>(fgInputSize.height)} : pipelineRect;
                    const bool reliefDoneBeforePush = reliefDone;
                    ApplyReliefBefore(reliefBeforeFg, reliefDone, fgInput, fgRect, 1.0f);
                    if (haveFgInputMode && lastFgPreScaled != preScaledNow) {
                        fg->Reset();
                        fgHasHistory = false;
                    }
                    haveFgInputMode = true;
                    lastFgPreScaled = preScaledNow;
                    const bool pushed = fg->PushFrame(fgInput, fgRect, frame.captureQpc);
                    if (pushed) {
                        pendingPreScaled = preScaledNow;
                        pendingNeuralDone = captureNeuralDone;
                        pendingReliefDone = reliefDone;
                    } else {
                        reliefDone = reliefDoneBeforePush;
                    }
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
                            FrameGenerator* motionSource = nullptr;
                            const bool reliefBeforeAlsr = relief && reliefBeforeUp && !reliefDone;
                            if ((alsrAfterNeural || reliefBeforeAlsr) && !ds.Empty() && EnsureAlsrTargets(ds)) {
                                pendingAlsrAfterNeural = true;
                            } else if (!alsrAfterNeural && !ds.Empty() && EnsureAlsrTargets(ds) &&
                                       (motionSource = LocalMotionSource(fgInput, fgRect, frame.captureQpc)) != nullptr &&
                                       alsr->Render(*motionSource, alsrOutRtv.Get(), ds, alsrSettings)) {
                                pendingAlsr = true;
                            } else {
                                MSR_LOG_ERROR(L"MoonSR failed to render; switching to Lanczos-3 for this session");
                                alsrFailed = true;
                            }
                        }
                    }
                    if (AnyDeviceLost()) {
                        capture->ReleaseFrame();
                        if (!Recover(L"device lost")) {
                            recoveryFailed(L"The graphics device was lost and could not be recovered.");
                            break;
                        }
                        continue;
                    }
                    if (pushed) {
                        // Smart parallax analyses the original capture (still held). The generator's motion field is
                        // passed only when it measured this real frame on the main device in capture pixels: local
                        // generator, fed with a capture-sized image (not pre-scaled; neural output keeps the geometry).
                        if (relief && !analyseReliefEarly) {
                            MotionFieldView motion;
                            const Size content{static_cast<uint32_t>(frame.contentRect.Width()),
                                               static_cast<uint32_t>(frame.contentRect.Height())};
                            const bool motionMatches = !fgIsRemote && !preScaledNow && !capturePreScaled &&
                                                       fg->FrameSize() == content;
                            if (motionMatches) motion = fg->MotionField();
                            ReliefAnalyse(frame, motionMatches && motion.valid ? &motion : nullptr);
                        }
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
                        pendingAuto = false;
                        windowReal++;
                        uint32_t autoCount = 0;
                        if (autoMode) {
                            if (autoOriginQpc == 0) autoOriginQpc = frame.captureQpc;
                            uint32_t missed = 0;
                            const double slotMs = 1000.0 / autoTargetFps;
                            autoCount = autoSched.OnRealFrame(AutoMs(frame.captureQpc), AutoMs(QpcNow()),
                                                              validSchedule, periodMs, slotMs, vsync ? scheduleDelayMs : 0.0,
                                                              autoSlots.data(), kMaxPending, missed);
                            if (missed) {
                                autoSlotsWin += missed;
                                autoSkippedWin += missed;
                                std::lock_guard lock(mutex);
                                stats.generatedSkipped += missed;
                            }
                            if (autoCount > 0) {
                                bool anyGenerated = false;
                                for (uint32_t k = 0; k < autoCount; ++k) {
                                    pending[pendingCount++] = {AutoQpc(autoSlots[k].displayMs), autoSlots[k].t, autoSlots[k].real};
                                    anyGenerated = anyGenerated || !autoSlots[k].real;
                                }
                                if (anyGenerated) windowRealWithGen++;
                                pendingAuto = true;
                                scheduledMultiplier = 1; // the Fixed overrun accounting does not apply
                                pendingSlotMs = slotMs;
                            }
                        }
                        if (autoMode) {
                            if (autoCount == 0) {
                                // First frame, hitch, pause, resync: show the real frame immediately.
                                scheduledMultiplier = 1;
                                pendingSlotMs = 0.0;
                                pending[pendingCount++] = {base, 1.0f, true};
                            }
                        } else if (validSchedule) {
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

                if (!analyseReliefEarly) ReliefAnalyse(frame, nullptr);
                ApplyReliefBefore(reliefBeforeUp && !capturePreScaled, reliefDone, pipelineTexture, pipelineRect, 1.0f);
                reliefInScene = relief && reliefAnalysed && !reliefDone;
                RectI destRect;
                bool rendered = false;
                ScaleSettings directSettings = settings;
                if (capturePreScaled) { directSettings.enabled = true; directSettings.scaler = ScalerKind::Nearest; directSettings.sharpness = 0.0f; }
                const PresentStatus ps =
                    RenderAndPresent(pipelineTexture, pipelineRect, directSettings, cursorState, destRect, rendered);
                // Release after Present so the OS cannot recycle the texture before our draw was submitted.
                capture->ReleaseFrame();
                const int64_t t1 = QpcNow();

                const bool deviceLost = ps == PresentStatus::DeviceLost || AnyDeviceLost();
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
                                  : (settings.scaler == ScalerKind::MoonALSR && !pendingAlsr) ? L"Lanczos-3 (MoonSR not active)"
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
                const double gameFpsNow = rateFps > 0.0 ? rateFps : periodSlowMs > 0.0 ? 1000.0 / periodSlowMs : stats.capturedFps;
                EvaluateAutoLoad(gameFpsNow);
                stats.frameGenMode = autoMode ? FrameGenMode::Auto : FrameGenMode::Fixed;
                stats.frameGenRatio = windowAcquired ? static_cast<double>(windowPresented) / windowAcquired : 1.0;
                if (autoMode) {
                    const bool autoOn = fg && autoRequestedFps > 0;
                    stats.frameGenTargetFps = autoOn ? autoTargetFps : 0.0;
                    stats.frameGenMultiplier = autoOn && autoEngaged
                        ? std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(stats.frameGenRatio))) : 1;
                    stats.frameGenLimited = autoOn && autoLimitReason != 0;
                    stats.frameGenDelayMs = autoOn && autoEngaged ? autoSched.HoldBackMs() : 0.0;
                } else {
                    stats.frameGenTargetFps = 0.0;
                    stats.frameGenMultiplier = activeMultiplier;
                    stats.frameGenLimited = fg && activeMultiplier < fgRequested;
                    stats.frameGenDelayMs = scheduledMultiplier > 1 ? pendingSlotMs * (scheduledMultiplier - 1) : 0.0;
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
                    if (orderUpscaleFirst && settings.enabled && !pendingPreScaled && fgWanted) {
                        auto scalePos = std::find(actual.stages.begin(), actual.stages.end(), PipelineStage::Upscaling);
                        actual.stages.erase(scalePos);
                        actual.stages.push_back(PipelineStage::Upscaling);
                    }
                    for (PipelineStage st : actual.stages) {
                        const bool active = st == PipelineStage::Upscaling ? settings.enabled
                                          : st == PipelineStage::FrameGeneration ? activeMultiplier > 1
                                          : st == PipelineStage::Relief ? relief != nullptr : neural != nullptr;
                        if (!active) continue;
                        if (!text.empty()) text += L" > ";
                        text += Engine::StageName(st);
                    }
                    stats.stageGpuText = StageGpuText(actual);
                    stats.pipelineOrderText = text.empty() ? L"(no processing: frames shown as captured)" : text;
                    stats.pipelineOrderNote.clear();
                    if (orderUpscaleFirst && settings.enabled && !pendingPreScaled && fgWanted) {
                        if (!stats.pipelineOrderNote.empty()) stats.pipelineOrderNote += L" ";
                        stats.pipelineOrderNote += L"Upscaling could not run before frame generation; using frame generation first.";
                    } else if (upscaleFirst && settings.enabled && activeMultiplier > 1) {
                        if (!stats.pipelineOrderNote.empty()) stats.pipelineOrderNote += L" ";
                        stats.pipelineOrderNote += L"Upscaling runs before frame generation: motion analysis works at output "
                                                   L"resolution, which costs more GPU time than the default order.";
                    }
                }
                stats.alsrActive = pendingAlsr || pendingAlsrAfterNeural || (useAlsr && pendingPreScaled);
                stats.alsrNote.clear();
                if (settings.enabled && settings.scaler == ScalerKind::MoonALSR && (alsrFailed || fgCreateFailed))
                    stats.alsrNote = L"MoonSR could not start on this GPU (see the log). Lanczos-3 is used instead.";
                if (relief) {
                    const ReliefStats rs = relief->Stats();
                    stats.reliefActive = rs.active;
                    stats.reliefTier = rs.tierInEffect;
                    stats.reliefNote = rs.reason;
                    if (reliefMotionFailed && reliefSettings.tier == ReliefTier::MotionDepth &&
                        rs.tierInEffect == ReliefTier::Parallax)
                        stats.reliefNote = L"The Motion depth estimator could not start. Using Parallax.";
                    if (reliefCopyProblem) {
                        stats.reliefActive = false;
                        stats.reliefNote = L"Smart parallax was bypassed because its frame transfer or render target failed.";
                    }
                    stats.reliefGpuMs = rs.applyGpuMs + rs.analyseGpuMs;
                } else {
                    stats.reliefActive = false;
                    stats.reliefTier = ReliefTier::Off;
                    stats.reliefGpuMs = 0.0;
                    stats.reliefNote = reliefSettings.tier != ReliefTier::Off && reliefFailed
                        ? L"Smart parallax could not start on this GPU (see the log)." : L"";
                }
                stats.frameGenCoverage = windowReal ? static_cast<double>(windowRealWithGen) / windowReal : 0.0;
                stats.frameGenNote.clear();
                if (autoMode && autoRequestedFps > 0) {
                    const double gameFps = gameFpsNow;
                    std::wstring capNote;
                    if (autoLimitReason == 1 || (autoGpuCapFps > 0.0 && vsync && outputRefreshHz > 1.0 &&
                                                 autoRequestedFps > outputRefreshHz))
                        capNote = std::format(L"Target {} fps is above the display's {:.0f} Hz with V-Sync on; presenting at "
                                              L"{:.0f} fps.", autoRequestedFps, outputRefreshHz,
                                              std::min<double>(autoRequestedFps, outputRefreshHz));
                    std::wstring note;
                    if (!fg) {
                        note = L"Frame generation is unavailable on this GPU or failed to start (see the log).";
                    } else if (gameFps <= 0.0) {
                        note = L"Measuring the game frame rate before generating frames.";
                    } else if (autoLimitReason == 2) {
                        note = std::format(L"The GPU cannot generate frames fast enough for {:.0f} fps; aiming for {:.0f} fps "
                                           L"(about {:.0f} fps presented). Lower the target, quality or resolution.",
                                           std::min<double>(autoRequestedFps, vsync && outputRefreshHz > 1.0 ? outputRefreshHz : 1e9),
                                           autoTargetFps, stats.presentedFps);
                    } else if (!autoEngaged) {
                        note = std::format(L"The game already runs at {:.0f} fps, at or above the {:.0f} fps target: real "
                                           L"frames are shown as they arrive and none are generated.", gameFps, autoTargetFps);
                    } else if (windowReal >= 5 && stats.frameGenCoverage < 0.7) {
                        note = std::format(
                            L"Frame generation is active on only {:.0f}% of frames: the game's frame times are irregular. "
                            L"A steady frame cap in the game helps.", stats.frameGenCoverage * 100.0);
                    }
                    stats.frameGenNote = capNote;
                    if (!note.empty()) stats.frameGenNote += (capNote.empty() ? L"" : L" ") + note;
                } else if (fgRequested > 1) {
                    const double gameFps = periodSlowMs > 0.0 ? 1000.0 / periodSlowMs : stats.capturedFps;
                    if (!fg) {
                        stats.frameGenNote = L"Frame generation is unavailable on this GPU or failed to start (see the log).";
                    } else if (activeMultiplier < fgRequested && fgLimitReason == 2) {
                        stats.frameGenNote = L"Measuring the game frame rate before enabling a higher multiplier.";
                    } else if (activeMultiplier < fgRequested && fgLimitReason == 1) {
                        stats.frameGenNote = std::format(
                            L"Frame generation limited to {}X: the game runs at {:.0f} fps and {}X would exceed the {:.0f} Hz "
                            L"display with V-Sync on. Cap the game at {:.0f} fps or lower, or turn V-Sync off, to get {}X.",
                            activeMultiplier, gameFps, fgRequested, outputRefreshHz, outputRefreshHz / fgRequested, fgRequested);
                    } else if (activeMultiplier < fgRequested && GetModuleHandleW(L"RTSSHooks64.dll")) {
                        // RivaTuner Statistics Server injects into every Direct3D process. A frame limit set in
                        // its Global profile therefore also limits OUR presents, which is exactly this symptom.
                        stats.frameGenNote = std::format(
                            L"Frame generation limited to {}X: RivaTuner (RTSS) is hooking MoonSR, so its frame limit also "
                            L"caps MoonSR's output. In RTSS click Add, choose MoonSR.exe and set 'Application detection "
                            L"level' to None - or set the limit in the game's own RTSS profile instead of Global.",
                            activeMultiplier);
                    } else if (!vsync && fg && windowReal >= 5 && stats.frameGenCoverage >= 0.7 &&
                               stats.presentedFps < 0.9 * gameFps * activeMultiplier) {
                        // V-Sync off: nothing but throughput limits the presented rate.
                        stats.frameGenNote = std::format(
                            L"{}X would present {:.0f} fps, but the GPU delivers about {:.0f} fps. Lower the multiplier, "
                            L"quality or resolution.", activeMultiplier, gameFps * activeMultiplier, stats.presentedFps);
                    } else if (fgRunningLate && vsync) {
                        // With V-Sync off the measured rate above is the honest signal: real frames shown a little
                        // after one frame time at high multipliers are not a problem while the full rate is delivered.
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
                                 L"refresh {:.1f} Hz, auto target {:.1f} ratio {:.2f}), latency {:.2f} ms, late {}, dropped {}{}{}",
                                 stats.inputSize.width, stats.inputSize.height, stats.capturedFps, stats.presentedFps,
                                 stats.generatedFps, fgRequested, activeMultiplier, fgLimitReason, stats.frameGenCoverage * 100.0,
                                 periodMs, periodSlowMs, outputRefreshHz, stats.frameGenTargetFps, stats.frameGenRatio,
                                 stats.latencyMs, stats.generatedLate,
                                 stats.framesDropped, stats.frameGenNote.empty() ? L"" : L" | ", stats.frameGenNote);
                    if (neural) {
                        MSR_LOG_INFO(L"Stats: neural rendering call {:.2f} ms average, {:.2f} ms max (pipeline thread), "
                                     L"present call {:.2f} ms, swapchain wait {:.2f} ms",
                                     neuralCallMsAvg, neuralCallMsMax, presentCallAvg, swapchainWaitAvg);
                        neuralCallMsMax = 0.0;
                    }
                }
                windowStart = QpcNow();
                windowCaptured = windowPresented = windowGenerated = 0;
                windowReal = windowRealWithGen = 0;
                windowAcquired = 0;
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

std::wstring Engine::NeuralRuntimeDirectory(Vendor vendor) {
    return NeuralBridge::RuntimeDirectory(vendor);
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
        impl_->liveRelief = config.relief;
        impl_->liveSplit = config.splitView;
        impl_->pendingNeuralChange.reset();
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
    case PipelineStage::Relief: return L"Smart parallax";
    default: return L"Upscaling";
    }
}

const wchar_t* Engine::StageDescription(PipelineStage stage) {
    switch (stage) {
    case PipelineStage::NeuralRendering:
        return L"Enhances image detail and appearance using your selected neural rendering model.";
    case PipelineStage::FrameGeneration:
        return L"Before upscaling it interpolates at the game's resolution (lighter); after it, at output resolution (heavier).";
    case PipelineStage::Relief:
        return L"Adds shading and surface displacement. Motion depth can estimate motion independently.";
    default:
        return L"Enlarges the image to the output size.";
    }
}

PipelineOrder Engine::NormalizeOrder(const PipelineOrder& order) {
    const PipelineOrder def;
    if (order.stages.size() + 1 == def.stages.size() &&
        std::count(order.stages.begin(), order.stages.end(), PipelineStage::Relief) == 0) {
        PipelineOrder migrated = order;
        migrated.stages.push_back(PipelineStage::Relief);
        return NormalizeOrder(migrated);
    }
    if (order.stages.size() != def.stages.size()) return def;
    for (PipelineStage st : def.stages)
        if (std::count(order.stages.begin(), order.stages.end(), st) != 1) return def;
    return order;
}

void Engine::SetAlsrSettings(const AlsrSettings& settings) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveAlsr = settings;
}

void Engine::SetSplitView(bool on) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveSplit = on;
}

void Engine::SetReliefSettings(const ReliefSettings& settings) {
    std::lock_guard lock(impl_->mutex);
    impl_->liveRelief = settings;
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
