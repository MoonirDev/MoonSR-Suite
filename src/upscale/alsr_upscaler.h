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

struct AlsrSettings {
    float sharpness = 0.0f;  // 0..1, final adaptive sharpening applied inside the upscaler
    float stability = 0.85f; // 0..1, upper bound on how strongly history is trusted
    bool operator==(const AlsrSettings&) const = default;
};

enum class AlsrDebugView : uint32_t { None = 0, HistoryWeight, Rejection, Motion };

class AlsrUpscaler {
public:
    static std::unique_ptr<AlsrUpscaler> Create(GpuDevice& device, std::wstring& error);
    virtual ~AlsrUpscaler() = default;

    // Advances the history with the generator's newest real frame (RealFrame(0)) and its motion field, and writes
    // the reconstructed image into `dst`, a render target of exactly `outputSize`. Call EXACTLY once per
    // PushFrame, after it. With no valid pair, on a scene cut, or after Reset(), the history restarts from the
    // single-frame reconstruction. Returns false on a D3D failure. Leaves no RTV / SRV / UAV bound.
    virtual bool Render(const FrameGenerator& motion, ID3D11RenderTargetView* dst, Size outputSize,
                        const AlsrSettings& settings) = 0;

    // Output of the last Render as a texture (BGRA8 UNORM, outputSize), or null.
    virtual ID3D11Texture2D* LastOutput() const = 0;

    // Upscales an arbitrary frame-sized image (e.g. a GENERATED frame) with the single-frame reconstruction and the
    // same sharpening, without touching the history.
    virtual bool RenderSingle(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                              const AlsrSettings& settings) = 0;

    // Drops the temporal history.
    virtual void Reset() = 0;

    virtual void SetDebugView(AlsrDebugView view) = 0;
};

} // namespace moonsr
