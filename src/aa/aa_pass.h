// Anti-aliasing contract. Header owned by the lead.
// Implementation: upscaling-engineer (algorithms) with shader-engineer (HLSL under shaders/aa/).
// Design and evidence: documentation/research/anti_aliasing.md.
//
// What it is: the universal capture path receives a finished, resolved picture with no depth, motion vectors or MSAA
// samples, so the only available anti-aliasing is a spatial post-process filter. Spatial estimates edge coverage from
// luminance transitions and blends toward a directional average. It is off by default and it is an approximation: it
// cannot recover detail the game never rendered, and it softens content the game already anti-aliased.
//
// Where it runs: at the position given by the user's load order. The default order places it after upscaling, so it
// runs at output resolution once per displayed frame. Before upscaling it runs at game resolution once per real frame.
//
// Threading: pipeline thread only (the owner of the GpuDevice immediate context). No per-frame heap allocation;
// resources are created on first use and recreated only when sizes or formats change. Statistics reach the CPU through
// small staging buffers read a few frames later (never a stall, never a flush).
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

enum class AaMode : uint32_t {
    Off = 0,
    Spatial = 1,      // T1: edge-directed luma blend (cheapest)
    Directional = 2,  // T2: pattern / area reconstruction (better diagonals and text); not built yet
    Temporal = 3,     // T3: T2 with history from the frame generator's flow (research; not built)
};
enum class AaQuality : uint32_t { Performance = 0, Balanced = 1, Quality = 2 };
enum class AaDebugView : uint32_t { None = 0, EdgeMask, BlendWeight, Split };

// Every field is clamped by the implementation. Ranges and defaults: research note section 7.2.
struct AaSettings {
    AaMode mode = AaMode::Off;
    AaQuality quality = AaQuality::Balanced;
    float strength = 1.0f;          // master 0..1 blend
    float edgeThreshold = 0.10f;    // relative luma contrast, 0.02..0.5
    float edgeThresholdMin = 0.03f; // absolute floor for dark scenes, 0..0.2
    bool protectText = true;        // thin-stroke / text protection
    bool protectHud = true;         // static pixels (T2 and later; T1 uses the stroke test only)
    bool autoReduce = false;        // opt-in: lower the strength when the picture already looks resolved
    AaDebugView debugView = AaDebugView::None; // EdgeMask / BlendWeight / Split are diagnostic
    bool operator==(const AaSettings&) const = default;
};

struct AaStats {                 // read with a lag; never stalls the CPU
    bool active = false;
    AaMode modeInEffect = AaMode::Off;
    float edgeCoverage = 0.0f;    // share of sampled pixels classified as an aliased edge, 0..1
    float strengthInEffect = 1.0f; // after autoReduce
    std::wstring reason;          // plain-language state when reduced or not active; empty otherwise
    double gpuMs = 0.0;           // GPU timestamp median over recent Apply calls
};

class AaPass {
public:
    static std::unique_ptr<AaPass> Create(GpuDevice& device, std::wstring& error);
    virtual ~AaPass() = default;

    virtual void SetSettings(const AaSettings& settings) = 0;

    // Once per frame that reaches this stage. Reads `src` (the finished image of the previous stage, `srcSize`) and
    // writes `dst` (same size, different texture). `contentInDst` is where the game image lies inside dst (letterbox
    // excluded). `outputPixelScale` is the scale from captured pixels to output pixels, so the filter keeps its width
    // in output pixels when it runs before upscaling. Returns false on failure (logged once); the engine then keeps
    // the unfiltered image for that frame.
    virtual bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
                       const RectI& contentInDst, float outputPixelScale = 1.0f) = 0;

    virtual void Reset() = 0;      // scene cut, resize, device recovery
    virtual AaStats Stats() const = 0;
};

} // namespace moonsr
