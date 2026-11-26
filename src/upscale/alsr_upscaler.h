// MoonSR temporal upscaler contract (internal name "Moon ALSR"; the classes and files keep the alsr_ prefix).
// Header owned by the lead. Implementation: upscaling-engineer.
//
// The upscaler reconstructs the output-resolution image from the stream of captured frames.
//
// Temporal reconstruction. No engine motion vectors, depth or camera jitter exist on the capture path. Motion comes
// from the image-based estimator in FrameGenerator (MotionFieldView); sub-pixel sample diversity comes from the
// scene's own motion. History is rejected where motion is unreliable, on disocclusion and on scene cuts. Where
// nothing moves there is no new information and the result is the single-frame reconstruction (MoonSR Spatial),
// kept stable. The single-frame reconstruction is also the seed and fallback wherever the history is not trusted.
//
// There is no online learning and no per-game state on disk: the output depends only on the frames, the motion
// field and AlsrSettings.
//
// Threading: Create / Render / RenderSingle / Reset / SetDebugView / destructor are called on one thread (the owner
// of the GpuDevice immediate context).
#pragma once

#include "core/types.h"
#include "framegen/frame_generator.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

namespace moonsr {

// Research controls used by the temporal harness. Values match SeedConstants in the shaders.
enum class AlsrSeed : uint32_t { Spatial, Lanczos, Directional, Covariance };

enum class AlsrAccumulation {
    Separate, FusedBilinear, FusedCubic, GatedSeparate, PointEvidence, PointResidual, GuardedPoint,
    AutoGuarded, StaticFocus, TiledArea, PaddedArea, CachedHull, AdaptiveGuarded, AdaptiveSeed, AdaptivePadded,
    AdaptivePooled, PooledSpatial, PooledFixedCount, PooledPhase, PooledFootprint, PooledFootprintFast
};

constexpr bool UsesAdaptiveFootprint(AlsrAccumulation mode) {
    return mode == AlsrAccumulation::PooledFootprint || mode == AlsrAccumulation::PooledFootprintFast;
}

constexpr bool UsesPooledSampling(AlsrAccumulation mode) {
    return mode == AlsrAccumulation::AdaptivePooled || mode == AlsrAccumulation::PooledSpatial ||
           mode == AlsrAccumulation::PooledFixedCount || mode == AlsrAccumulation::PooledPhase ||
           UsesAdaptiveFootprint(mode);
}

constexpr bool UsesAdaptiveSeed(AlsrAccumulation mode) {
    return mode == AlsrAccumulation::AdaptiveSeed || mode == AlsrAccumulation::AdaptivePadded ||
           (UsesPooledSampling(mode) && mode != AlsrAccumulation::PooledSpatial);
}

constexpr bool UsesSamplingModel(AlsrAccumulation mode) {
    return mode == AlsrAccumulation::AutoGuarded || mode == AlsrAccumulation::AdaptiveGuarded ||
           UsesAdaptiveSeed(mode) || UsesPooledSampling(mode);
}

inline constexpr uint32_t kAlsrSamplingStateWidth = 3;

struct AlsrSettings {
    float sharpness = 0.0f;  // 0..1, final adaptive sharpening applied inside the upscaler
    float stability = 0.85f; // 0..1, upper bound on how strongly history is trusted
    AlsrAccumulation accumulation = AlsrAccumulation::Separate;
    bool tiledReference = false; // experimental one-dispatch registration reference
    bool pixelRefine = false;
    bool affineMotion = false;
    bool persistentHud = false;
    bool robustHistory = false; // experimental thin-feature locks and instability weighting
    AlsrSeed seed = AlsrSeed::Spatial;
    bool operator==(const AlsrSettings&) const = default;
};

enum class AlsrDebugView : uint32_t { None = 0, HistoryWeight, Rejection, Motion };

enum class AlsrPass : uint32_t { Prep, CellRefine, Motion, Reproject, SeedBlend, Residual, Update, Final, ReferenceLuma, ReferenceBlur, Sampling, Count };

// Optional benchmark observer called on the immediate-context thread. A skipped pass has no callbacks.
using AlsrPassObserver = void (*)(void* user, AlsrPass pass, bool begin);

class AlsrUpscaler {
public:
    static std::unique_ptr<AlsrUpscaler> Create(GpuDevice& device, std::wstring& error);
    virtual ~AlsrUpscaler() = default;

    // Advances the history with the generator's newest real frame (RealFrame(0)) and its motion field, and writes
    // the reconstructed image into `dst`, a render target of exactly `outputSize`. Call EXACTLY once per
    // PushFrame, after it. With no valid pair, on a scene cut, or after Reset(), the history restarts from the
    // single-frame reconstruction. Returns false on a D3D failure. Leaves no RTV / SRV / UAV bound.
    virtual bool Render(const FrameGenerator& motion, ID3D11RenderTargetView* dst, Size outputSize,
                        const AlsrSettings& settings, AlsrPassObserver observer = nullptr, void* user = nullptr) = 0;

    // Upscales an arbitrary frame-sized image (e.g. a GENERATED frame) with the single-frame reconstruction and the
    // same sharpening, without touching the history.
    virtual bool RenderSingle(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                              const AlsrSettings& settings) = 0;

    // Motion-free temporal tier for real input frames. Uses the seed and a bounded
    // screen-space stabilizer. RenderSingle remains the generated-frame entry point.
    virtual bool RenderLite(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                            const AlsrSettings& settings) = 0;

    // Borrowed 3x1 RGBA32F texture from the latest auto-sampling render.
    // Texel 0: model (unknown=0, area=1, point=2), point streak, static blend, static streak.
    // Texel 1: edge count, mean concentration, abrupt fraction, flags (bit 0 candidate, bit 1 restart).
    // Texel 2: pooled raw evidence when enabled, zero otherwise.
    virtual ID3D11Texture2D* SamplingState() const = 0;

    // Borrowed source-resolution RGBA16F field written by the latest Render.
    virtual ID3D11Texture2D* RefinedMotion() const = 0;

    // Drops the temporal history.
    virtual void Reset() = 0;

    virtual void SetDebugView(AlsrDebugView view) = 0;
};

} // namespace moonsr
