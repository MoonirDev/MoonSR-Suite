// Frame generation module contract. Header owned by the lead. Implementation: frame-generation-engineer.
//
// FrameGenerator keeps a short history of REAL frames at game resolution, estimates motion between the two most
// recent ones once per pair, and synthesises intermediate frames at arbitrary phases from that ORIGINAL pair.
// A generated frame is never used as an interpolation source.
//
// Cost structure (deliberate): PushFrame does the heavy work once per real frame (copy, pyramid, bidirectional
// optical flow, confidence, scene-cut and static-region analysis). Interpolate is the light per-generated-frame
// work (warp both frames to phase t, blend by confidence, fallbacks). This is what makes 3X-6X affordable.
//
// Everything works at the game's resolution; upscaling real and generated frames happens afterwards (ScalePass).
//
// Threading: one thread only (the thread that owns the GpuDevice immediate context).
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

namespace moonsr {

enum class FrameGenQuality : uint32_t {
    Performance = 0, // integrated / old GPUs: coarsest flow, cheapest warp
    Balanced,
    Quality,
};

struct FrameGenSettings {
    FrameGenQuality quality = FrameGenQuality::Performance;
    float flowScale = 0.5f;      // resolution of motion estimation relative to the frame, 0.25 .. 1.0
    bool hudProtection = true;   // keep static overlay regions (HUD, text) from being warped
    bool preciseMotion = false;  // refine subpixel motion for higher frame-generation multipliers
    bool operator==(const FrameGenSettings&) const = default;
};

// Result of analysing the newest pair, available after PushFrame.
struct FramePairInfo {
    bool valid = false;        // two frames of equal size are available and flow was computed
    bool sceneCut = false;     // the pair is unrelated (cut / teleport); Interpolate must not blend across it
    float meanConfidence = 0;  // 0..1, average flow confidence over the frame (may lag by one pair if read back async)
    float meanMotionPx = 0;    // average motion magnitude in frame pixels (same remark)
    int64_t prevQpc = 0;
    int64_t currQpc = 0;
};

enum class FrameGenDebugView : uint32_t { None = 0, Flow, Confidence, StaticMask };

// Read-only view of the motion analysis of the newest pair, for other GPU stages (temporal upscaling).
// The generator is also the project's motion estimator: one analysis per real frame serves both frame generation
// and temporal reconstruction. All resources stay owned by the generator and are valid until the next PushFrame /
// Reset / SetSettings; bind them as shader resources only.
struct MotionFieldView {
    bool valid = false; // false until two frames of equal size have been analysed
    // Texture2DArray, RGBA32F, one texel per motion cell. Slice 0: motion of content from the previous frame to the
    // newest (prev -> curr). Slice 1: newest to previous (curr -> prev). xy = displacement in FRAME pixels,
    // z = confidence 0..1 (forward-backward consistency x match quality), w = normalised match residual
    // (rms / (stddev + 2)).
    ID3D11ShaderResourceView* flow = nullptr;
    Size cells;              // motion-field resolution
    // Frame pixels per cell (cell size / flow scale). The grid is rounded up, so cells.width * cellSizePx can
    // exceed the frame width by less than one cell.
    float cellSizePx = 0.0f;
    // Full-resolution R8 mask of static overlays (HUD); may be null when HUD protection is off.
    ID3D11ShaderResourceView* staticMask = nullptr;
    // Buffer<uint> whose element `sceneCutIndex` is non-zero when the newest pair is a scene cut. Readable in a
    // shader in the same frame (no CPU readback involved). May be null if unavailable; a consumer then has no
    // scene-cut signal.
    ID3D11ShaderResourceView* stats = nullptr;
    uint32_t sceneCutIndex = 0;
};

class FrameGenerator {
public:
    static std::unique_ptr<FrameGenerator> Create(GpuDevice& device, std::wstring& error);
    virtual ~FrameGenerator() = default;

    virtual void SetSettings(const FrameGenSettings& settings) = 0;

    // Copies `srcRect` of `src` into the history as the newest real frame (the caller may release `src`
    // afterwards) and analyses the (previous, newest) pair. A size change resets the history.
    // `qpc` is the frame's capture time. Returns false on a D3D failure (see log).
    virtual bool PushFrame(ID3D11Texture2D* src, const RectI& srcRect, int64_t qpc) = 0;

    // Drops all history (e.g. after a pipeline reset).
    virtual void Reset() = 0;

    virtual FramePairInfo PairInfo() const = 0;

    // History access: index 0 = newest real frame, 1 = the one before. Null if not available.
    // Textures are BGRA8 UNORM at frame size, shader-resource bindable, whole texture valid.
    virtual ID3D11Texture2D* RealFrame(uint32_t index) const = 0;
    virtual Size FrameSize() const = 0;

    // Synthesises the frame at phase t in (0,1) between RealFrame(1) (t = 0) and RealFrame(0) (t = 1) into `dst`,
    // a render target of exactly FrameSize(). On a scene cut it writes the nearer real frame instead of a blend.
    // Returns false if no valid pair exists or on a D3D failure. Leaves no RTV / SRV / UAV bound.
    // Optionally borrows the local phase's half-resolution RGBA16F selection (xy: motion in frame pixels,
    // z: newest-frame blend share, w: confidence). Use it before the next interpolation, push, or reset.
    // The selection is null on failure, in debug mode, or if the generator is on another device.
    virtual bool Interpolate(float t, ID3D11RenderTargetView* dst,
                             ID3D11ShaderResourceView** selection = nullptr) = 0;

    // Motion analysis of the newest pair for other GPU stages. See MotionFieldView.
    virtual MotionFieldView MotionField() const = 0;

    // Diagnostic output instead of the interpolated colour (for tools and tuning).
    virtual void SetDebugView(FrameGenDebugView view) = 0;
};

} // namespace moonsr
