// Smart parallax (screen-space relief) contract. Header owned by the lead.
// Implementation: upscaling-engineer (algorithms) with shader-engineer (HLSL under shaders/relief/).
// Design and evidence: documentation/research/screen_space_relief.md.
//
// What it is: the universal capture path has no depth, normals or camera data, so this stage ESTIMATES a relief height
// from the captured picture (and, when the frame generator already runs, from the motion it measures) and adds subtle
// relief shading and a small screen-position-dependent parallax offset to textured surfaces. It is an approximation,
// not shape recovery, and it is off by default.
//
// Where it runs (lead decision D1): a two-rate stage at the END of the pipeline, after upscaling and frame generation,
// outside the user-arranged load order (no PipelineStage entry, so saved load orders are unaffected). It uses the
// device of the Upscaling GPU (D2: no per-stage GPU). The engine never starts the motion estimator just for relief
// (D7): MotionDepth falls back to Parallax when no motion field exists.
//
// Threading: pipeline thread only (the owner of the GpuDevice immediate context). No per-frame heap allocation;
// resources are created on first use and recreated only when sizes or formats change.
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;

namespace moonsr {

struct MotionFieldView;

enum class ReliefTier : uint32_t {
    Off = 0,
    Shading = 1,     // T1: relief lighting and cavity darkening from the estimated height
    Parallax = 2,    // T2: T1 plus a small parallax offset that depends on screen position (<= parallaxMaxPx)
    MotionDepth = 3, // T3: T2 with coarse depth from motion parallax; needs the frame generator's motion field
};
enum class ReliefLight : uint32_t { Fixed = 0, FollowScene = 1 /* experimental */ };
enum class ReliefDebugView : uint32_t { None = 0, Height, Normals, Protection, Offset, Depth, Split };

// Every field is clamped by the implementation. Ranges and defaults: research note section 7.2.
struct ReliefSettings {
    ReliefTier tier = ReliefTier::Off;
    float strength = 0.5f;        // master 0..1; luminance change soft-capped at about +-15 %; parallaxMaxPx of offset at 1.0
    float shading = 1.0f;         // 0..1 share of directional relief shading within `strength`
    float cavity = 1.0f;          // 0..1 share of cavity darkening within `strength`
    float parallaxMaxPx = 2.0f;   // output pixels, 0..3; ignored below Parallax
    float detailScalePx = 2.0f;   // band-pass centre in game pixels, 0.75..4
    ReliefLight light = ReliefLight::Fixed;
    float lightAzimuthDeg = 135.0f;  // 0 = right, 90 = up (135 = upper-left)
    float lightElevationDeg = 45.0f; // 15..80
    bool protectHud = true, protectEdges = true, protectCharacters = true; // characters: MotionDepth only
    bool autoDisable = true;      // stylised / menu / dark-frame / low-confidence detection
    ReliefDebugView debugView = ReliefDebugView::None; // Split = left half off, right half on (owner A/B)
    bool operator==(const ReliefSettings&) const = default;
};

struct ReliefStats {              // read with a lag; never stalls the CPU
    bool active = false;
    ReliefTier tierInEffect = ReliefTier::Off; // may be lower than requested (e.g. no motion for MotionDepth)
    float texturedCoverage = 0.0f; // share of pixels that received relief, 0..1
    float meanOffsetPx = 0.0f;     // Parallax and above
    bool autoDisabled = false;
    std::wstring reason;           // plain-language reason when not active or reduced; empty otherwise
    double analyseGpuMs = 0.0, applyGpuMs = 0.0; // GPU timestamp medians over recent calls
};

class ReliefPass {
public:
    static std::unique_ptr<ReliefPass> Create(GpuDevice& device, std::wstring& error);
    virtual ~ReliefPass() = default;

    virtual void SetSettings(const ReliefSettings& settings) = 0;

    // Once per REAL frame, on the ORIGINAL capture at game resolution, before any other stage changes it. `motion`
    // may be null or invalid: Shading and Parallax then run without it. Returns false on failure (logged); the engine
    // then skips Apply for that frame.
    virtual bool Analyse(ID3D11Texture2D* capture, const RectI& rect, const MotionFieldView* motion, int64_t qpc) = 0;

    // Once per DISPLAYED frame. Reads `src` (the finished image at dstSize) and writes `dst` (same size, different
    // texture). `contentInDst` is where the game image lies inside dst (letterbox excluded). phase: 1 = the newest real
    // frame; (0,1) = a generated frame between the previous and the newest real frame (height and protection are
    // warped to that phase). When the effect is off or inactive the engine does not call Apply at all.
    virtual bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
                       const RectI& contentInDst, float phase) = 0;

    virtual void Reset() = 0;     // scene cut, resize, device recovery
    virtual ReliefStats Stats() const = 0;
};

} // namespace moonsr
