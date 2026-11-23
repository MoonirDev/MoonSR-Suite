// MoonSR engine API. This is the ONLY header the user interface may include from the engine side.
// Owner: lead.
//
// Threading: all Engine methods are safe to call from the UI thread. The engine runs its own pipeline thread that
// owns the D3D device, the capture source and the output window. No method blocks on GPU work, except Start(),
// which waits for pipeline initialisation (typically well under a second), and Stop(), which joins the thread.
#pragma once

#include "core/types.h"
#include "framegen/frame_generator.h"
#include "models/model_store.h"
#include "neural/neural_settings.h"
#include "relief/relief_pass.h"
#include "upscale/alsr_upscaler.h"
#include "upscale/scale_pass.h"

#include <memory>
#include <string>
#include <vector>

namespace moonsr {

enum class CursorMode : uint32_t {
    None = 0, // no cursor in the scaled image; the system cursor stays where it really is
    Captured, // the OS draws the cursor into the captured image; the system cursor stays visible too
    Software, // MoonSR draws the cursor at the mapped position and hides the system cursor over the game
};

enum class FrameGenMode : uint32_t {
    Fixed = 0, // a fixed number of displayed frames per real frame (`multiplier`)
    Auto,      // generate as many frames as needed, varying per real frame, to present at `targetFps`
};

// Frame generation is ON when (mode == Fixed && multiplier >= 2) or (mode == Auto && targetFps > 0).
// Refresh-rate limit: with V-Sync ON the engine never presents more frames per second than the display refreshes
// (it lowers the multiplier / target). With V-Sync OFF frames are presented at the requested rate even above the
// refresh rate (they tear); only GPU throughput limits it then.
struct FrameGenConfig {
    FrameGenMode mode = FrameGenMode::Fixed;
    uint32_t multiplier = 1; // Fixed: 1 = off. 2..20 = displayed frames per real frame (M - 1 generated per real frame)
    // Auto: desired presented frames per second (0 = off). The engine presents at this rate (capped to what the GPU can
    // generate, and with V-Sync on also to the display refresh rate), placing each generated frame at its true time between the two real frames, so the number of
    // generated frames per real frame follows the game's frame rate and need not be a whole number on average.
    uint32_t targetFps = 0;
    FrameGenSettings settings;
    bool operator==(const FrameGenConfig&) const = default;
};

// Processing stages whose ORDER the user can arrange ("load order"). Earlier stages feed later ones.
enum class PipelineStage : uint32_t {
    NeuralRendering = 0, // user-supplied model, applied to real captures
    FrameGeneration,
    Upscaling,
};

// The order of the stages. Must contain each PipelineStage exactly once; anything else is replaced by the default.
//   Frame generation BEFORE upscaling (default): frames are interpolated at the game's resolution, then every
//     displayed frame is upscaled. Cheapest.
//   Upscaling BEFORE frame generation: real frames are upscaled first and interpolation runs on the upscaled
//     images. Heavier (motion analysis at output resolution); generated frames are built from more detail.
    //   Neural rendering follows the selected position; optical flow uses original captures.
struct PipelineOrder {
    std::vector<PipelineStage> stages{PipelineStage::NeuralRendering, PipelineStage::FrameGeneration, PipelineStage::Upscaling};
    bool operator==(const PipelineOrder&) const = default;
};

// The GPU that runs each pipeline stage. A zero LUID means Auto: the adapter that drives the output monitor.
// Capture runs on the GPU of the first stage in the load order; the finished image is presented by the monitor's GPU.
// Wherever two consecutive steps run on different GPUs the engine copies the frame between them (one cross-GPU copy
// per frame and hop, generated frames included). Applied when scaling starts; not changed live.
struct StageAdapters {
    LUID neuralRendering{}, frameGeneration{}, upscaling{};
    LUID& For(PipelineStage s) {
        return s == PipelineStage::NeuralRendering ? neuralRendering
             : s == PipelineStage::FrameGeneration ? frameGeneration : upscaling;
    }
    const LUID& For(PipelineStage s) const { return const_cast<StageAdapters*>(this)->For(s); }
    void SetAll(const LUID& luid) { neuralRendering = frameGeneration = upscaling = luid; }
};

struct EngineConfig {
    HWND targetWindow = nullptr;   // window to capture (required)
    HMONITOR outputMonitor = nullptr; // null = the monitor the target window is on
    StageAdapters stageAdapters;   // per-stage GPU; all zero = everything on the monitor's GPU
    ScaleSettings scale;
    bool vsync = false;
    CursorMode cursor = CursorMode::Software;
    FrameGenConfig frameGen;
    PipelineOrder order;
    // MoonSR temporal upscaler options, used when scale.scaler == ScalerKind::MoonALSR. `sharpness` here is
    // ignored: the engine takes it from scale.sharpness. The upscaler keeps no per-game state.
    AlsrSettings alsr;
    // Neural rendering with a USER-SUPPLIED model (see ModelStore). `neuralModelId` selects a model from
    // Engine::Models(). The separately built bridge loads the selected runtime.
    bool neuralRendering = false;
    std::wstring neuralModelId;
    NeuralSettings neuralSettings;
    PictureSettings pictureSettings;
    bool showFrameCounter = false; // draw "game fps / displayed fps" in the top-left corner of the output
    // Smart parallax (experimental, relief/relief_pass.h). With tier Off (the default) nothing is created and the frame
    // path is unchanged. Otherwise Analyse runs once per real frame on the original capture and Apply runs on every
    // displayed frame as the last image operation on the main (Upscaling) GPU, before the software cursor and the frame
    // counter are drawn, so neither is relief-processed.
    ReliefSettings relief;
    // A/B compare: left half of the output shows the raw capture (nearest, integer scaling), right half the final image.
    bool splitView = false;
};

enum class EngineState : uint32_t { Idle, Starting, Running, Recovering, Failed };

struct EngineStats {
    EngineState state = EngineState::Idle;
    Size inputSize;             // captured game image size
    Size outputSize;            // output monitor resolution
    RectI destRect;             // where the image is drawn on the output
    double capturedFps = 0.0;   // ORIGINAL frames delivered by the game/OS per second
    double presentedFps = 0.0;  // frames presented by MoonSR per second (equals capturedFps without frame generation)
    double generatedFps = 0.0;  // generated (interpolated) frames per second; 0 until frame generation exists
    // Average time from the moment the OS delivered the frame to us until Present() returned. This is MoonSR's
    // own processing delay only. It excludes game render time, DWM composition/capture delay and display scan-out.
    double latencyMs = 0.0;
    double cpuFrameMs = 0.0;    // average pipeline-thread time spent per frame (render submit + present)
    double presentCallMs = 0.0, swapchainWaitMs = 0.0;
    double presentIntervalMedianMs = 0.0, presentIntervalP95Ms = 0.0;
    double displayIntervalMedianMs = 0.0, displayIntervalP95Ms = 0.0;
    uint32_t displayIntervalSamples = 0;
    uint64_t framesCaptured = 0;
    uint64_t framesPresented = 0;
    uint64_t framesDropped = 0; // captured frames discarded because a newer one was already available
    uint32_t recoveries = 0;    // successful device-loss / presenter recoveries
    // Frame generation. Interpolation needs the NEXT real frame before it can show anything between two real
    // frames, so every real frame is deliberately held back by frameGenDelayMs. That is added latency, and it is
    // included in latencyMs. Generated frames raise displayed FPS only; they do not make the game more responsive.
    uint32_t frameGenMultiplier = 1;   // multiplier currently in effect (1 = off or unavailable); Auto: rounded average
    bool frameGenLimited = false;      // the engine lowered the requested multiplier / target (display refresh with V-Sync on, or GPU)
    FrameGenMode frameGenMode = FrameGenMode::Fixed;
    double frameGenTargetFps = 0.0;    // Auto: the presented rate the engine is aiming for after any cap (0 otherwise)
    double frameGenRatio = 1.0;        // measured displayed frames per real frame (fractional in Auto)
    // Plain-language explanation whenever frame generation is requested but not fully active (limited, paused on
    // irregular frames, unavailable). Empty when everything requested is being delivered. Show it to the user.
    std::wstring frameGenNote;
    // Neural rendering: false until a runtime actually processes frames. The note explains the current state.
    bool neuralRenderingActive = false;
    uint64_t neuralProcessedFrames = 0, neuralProcessedGeneratedFrames = 0;
    Size neuralInputSize;
    std::wstring neuralRenderingNote;
    // The order actually in effect, as text, e.g. "Frame generation > Upscaling" (inactive stages left out), and a
    // note when the requested order could not be honoured or has a cost the user should know about.
    std::wstring pipelineOrderText;
    std::wstring pipelineOrderNote;
    // Which GPU runs each stage and how frames cross between GPUs, e.g. "Frame generation: RTX A400 (copy: D3D12
    // cross-adapter heap) > Upscaling: RTX 5060". Empty while idle.
    std::wstring stageGpuText;
    // MoonSR temporal upscaler: true while it is producing the displayed image. `alsrNote` explains when it was
    // requested but is not in use.
    bool alsrActive = false;
    std::wstring alsrNote;
    // Smart parallax. reliefActive: the effect is changing the image (false while it is in development, off, or not
    // running). reliefTier: the tier in effect (may be lower than requested; Off when not running). reliefNote: plain-
    // language state when requested but not (fully) active. reliefGpuMs: measured GPU time per displayed frame of Apply
    // plus per real frame of Analyse (timestamp medians, lagged; 0 when off or not measured).
    bool reliefActive = false;
    ReliefTier reliefTier = ReliefTier::Off;
    std::wstring reliefNote;
    double reliefGpuMs = 0.0;
    // Diagnostic recording (see Engine::RecordFrames).
    bool recording = false;            // a recording is in progress
    std::wstring lastRecordingDir;     // folder of the most recently finished recording
    double frameGenCoverage = 0.0;     // share of real frames (0..1) that got generated frames in the last interval
    double frameGenDelayMs = 0.0;      // intentional hold-back of real frames: (M - 1) / M of the real frame time
    uint64_t generatedPresented = 0;   // generated frames shown
    uint64_t generatedLate = 0;        // CPU reached pre-Present scheduling more than half a slot after the due time
    uint64_t generatedSkipped = 0;     // stale generated frames omitted during catch-up
    std::wstring adapterName;
    Vendor adapterVendor = Vendor::Unknown;
    std::wstring captureMethod;
    std::wstring scalerName;
    std::wstring error;         // last error; non-empty when state == Failed or the engine stopped by itself
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Enumeration helpers (callable any time, independent of engine state).
    static std::vector<WindowInfo> ListWindows();
    static std::vector<MonitorInfo> ListMonitors();
    static std::vector<AdapterInfo> ListAdapters();
    static std::vector<ScalerDesc> ListScalers();
    // The store of user-supplied model files (%LOCALAPPDATA%\MoonSR\models). Usable any time, on any thread
    // (its calls do blocking file IO). The UI adds, lists, verifies and removes models through it.
    static ModelStore& Models();
    static bool AmdNeuralRuntimeAvailable();
    // True when intel-nr\ beside MoonSR.exe holds d5c_chain.dll, its gemm.spv shaders and dlssnr-logical.safetensors
    // (the user-built DLSS5_INTEL runtime and the user's own weights). Used on Intel processing GPUs.
    static bool IntelNeuralRuntimeAvailable();
    // The folder the neural rendering bridge loads the AMD (amd-nr) or Intel (intel-nr) runtime from, beside
    // MoonSR.exe. Empty for other vendors. The UI imports the user's runtime files there.
    static std::wstring NeuralRuntimeDirectory(Vendor vendor);

    // Starts capturing and presenting. Returns false and fills `error` if initialisation fails.
    bool Start(const EngineConfig& config, std::wstring& error);
    // Stops the pipeline and destroys the output window. Safe to call when not running.
    void Stop();
    // True while the pipeline thread is alive (Starting, Running or Recovering).
    bool IsRunning() const;

    // Diagnostics: saves the next `outputFrames` presented frames (and the captured input frames they came from)
    // as PNG files into `directory`. The output window is excluded from screen capture, so this is how the real
    // output can be inspected. Recording stalls the pipeline on every frame: the images are exact, the timing is
    // not representative. Returns false if the engine is not running or a recording is already in progress.
    bool RecordFrames(uint32_t outputFrames, const std::wstring& directory);

    EngineStats GetStats() const;

    // Live setting changes; take effect on the next frame.
    void SetScaleSettings(const ScaleSettings& settings);
    void SetVsync(bool vsync);
    void SetFrameGen(const FrameGenConfig& frameGen);
    void SetFrameCounter(bool show);
    void SetPictureSettings(const PictureSettings& settings);
    void SetNeuralSettings(bool enabled, const std::wstring& modelId, const NeuralSettings& settings);
    void SetPipelineOrder(const PipelineOrder& order);
    void SetAlsrSettings(const AlsrSettings& settings);
    // Smart parallax. Turning it on creates the pass on the next frame; turning it off releases it and its resources.
    void SetReliefSettings(const ReliefSettings& settings);
    void SetSplitView(bool on);

    // Default order, display names and one-line descriptions for the UI.
    static PipelineOrder DefaultOrder();
    static const wchar_t* StageName(PipelineStage stage);
    static const wchar_t* StageDescription(PipelineStage stage);
    // Returns `order` if it is a valid permutation, else the default.
    static PipelineOrder NormalizeOrder(const PipelineOrder& order);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moonsr
