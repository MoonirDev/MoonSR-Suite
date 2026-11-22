// Upscale module: runs a scaler (and optional sharpening stage) from a source texture region to a render target.
// Owner: lead (v0 infrastructure). Scaler algorithms live in shaders/ and are registered in scale_pass.cpp.
//
// Threading: one thread only (the thread that owns the GpuDevice immediate context).
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>
#include <vector>

namespace moonsr {

enum class ScalerKind : uint32_t {
    Nearest = 0,
    Bilinear,
    Bicubic,       // Catmull-Rom
    Lanczos,       // Lanczos-3
    MoonSRSpatial, // MoonSR custom spatial upscaler
    MoonALSR,      // "MoonSR" in the UI: image-based temporal reconstruction (internal name Moon ALSR). Not a
                   // ScalePass shader: the engine runs AlsrUpscaler for it; ScalePass itself falls back to Lanczos-3.
    Count
};

enum class FitMode : uint32_t {
    Fit = 0, // preserve aspect ratio, letterbox / pillarbox
    Stretch, // fill the output, ignore aspect ratio
    Integer, // largest integer multiple that fits, centred
    Native,  // no scaling: shown 1:1, centred (falls back to Fit if the source is larger than the output)
};

struct ScaleSettings {
    // false = upscaling switched off: the image is shown unscaled (1:1, centred, exact pixels) whatever `scaler`,
    // `fit` and `sharpness` say. Frame generation and the other features keep working.
    bool enabled = true;
    ScalerKind scaler = ScalerKind::Bilinear;
    FitMode fit = FitMode::Fit;
    float sharpness = 0.0f; // 0 disables the sharpening stage
    bool operator==(const ScaleSettings&) const = default;
};

struct ScalerDesc {
    ScalerKind kind;
    const wchar_t* name;
    bool available; // false if the shader is not part of this build
};

class ScalePass {
public:
    static std::unique_ptr<ScalePass> Create(GpuDevice& device, std::wstring& error);
    virtual ~ScalePass() = default;

    // Scales `srcRect` of `src` into `dst` (size dstSize). Clears `dst` to black first, so letterbox bars are black.
    // If the requested scaler is unavailable, bilinear is used. Returns false on a D3D failure (see log).
    // `outDestRect`, if not null, receives the rectangle the image was drawn into.
    virtual bool Render(ID3D11Texture2D* src, const RectI& srcRect, ID3D11RenderTargetView* dst, Size dstSize,
                        const ScaleSettings& settings, RectI* outDestRect = nullptr) = 0;

    // Drops cached views of source textures. Call when the capture source is recreated.
    virtual void ResetSourceCache() = 0;

    static std::vector<ScalerDesc> ListScalers();
    static const wchar_t* ScalerName(ScalerKind kind);
    static RectI ComputeDestRect(Size src, Size dst, FitMode fit);
};

} // namespace moonsr
