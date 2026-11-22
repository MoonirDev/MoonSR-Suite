// Moon ALSR ("Adaptive Learning Super Resolution") contract. Header owned by the lead.
// Implementation: upscaling-engineer.
//
// Moon ALSR reconstructs the output-resolution image from the stream of captured frames, and adapts itself to
// each game while it runs.
//
// 1. Temporal reconstruction. No engine motion vectors, depth or camera jitter exist on the capture path. Motion
//    comes from the image-based estimator in FrameGenerator (MotionFieldView); sub-pixel sample diversity comes
//    from the scene's own motion. History is rejected where motion is unreliable, on disocclusion and on scene
//    cuts. Where nothing moves there is no new information and the result is the single-frame reconstruction,
//    kept stable.
//
// 2. Per-game learning (self-supervised, online). The captured frames themselves are the training data: a frame
//    is reduced by the current scale factor, reconstructed, and compared with the frame it came from. From these
//    pairs ALSR learns a small bank of reconstruction filters specialised to the game's content (bucketed by local
//    edge orientation, strength and coherence), which replace the generic single-frame reconstruction used as
//    seed and fallback. The learned state lives in a per-game profile on disk, is loaded on the next run and keeps
//    refining. Rules:
//      - Learning never blocks or stalls Render: sample collection has a small fixed GPU budget and uses
//        non-blocking readback or GPU-side accumulation; solving happens on a low-priority worker thread.
//      - A learned filter is used only where it beats the generic reconstruction on held-out samples from the same
//        game (validation gating), blended in gradually as evidence accumulates. Learning can never make the
//        output worse than the generic path by more than measurement noise.
//      - Profiles are versioned and keyed by game and scale factor; a corrupt or incompatible file is ignored.
//
// Threading: Create / SetProfile / Render / RenderSingle / Reset / SaveProfile / ResetLearning / LearningStatus /
// destructor are called on one thread (the owner of the GpuDevice immediate context). Internal worker threads
// never touch D3D.
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
    bool learning = true;    // collect samples and refine the per-game profile while running
    bool useLearned = false; // experimental: learned filters can reduce quality on some content
    bool operator==(const AlsrSettings&) const = default;
};

struct AlsrLearningStatus {
    bool profileLoaded = false;   // a profile from an earlier session was loaded
    bool learningActive = false;  // samples are being collected right now
    bool learningScaleSupported = false; // reconstruction still runs when this scale cannot safely learn
    bool learnedFiltersActive = false;
    uint64_t samples = 0;         // training samples accumulated in this profile (all sessions)
    uint32_t solves = 0;          // filter updates computed so far
    float coverage = 0.0f;        // 0..1 share of filter buckets with enough evidence to be in use
    // Self-supervised validation error on held-out samples (mean absolute luma error, 0..255 scale): generic
    // reconstruction vs learned filters on proxy samples; this does not guarantee real-game quality.
    float genericError = 0.0f;
    float learnedError = 0.0f;
    std::wstring profilePath;     // file used for this game, empty if persistence is off
};

enum class AlsrDebugView : uint32_t { None = 0, HistoryWeight, Rejection, Motion, LearnedBlend };

class AlsrUpscaler {
public:
    static std::unique_ptr<AlsrUpscaler> Create(GpuDevice& device, std::wstring& error);
    // Saves the profile (if any) and stops the worker thread.
    virtual ~AlsrUpscaler() = default;

    // Selects the per-game profile: loads `<directory>\<gameKey>.alsr` if it exists and saves there from now on.
    // `gameKey` is a file-name-safe identifier of the game (the engine passes the exe name). An empty key turns
    // persistence off (learning still works for the session). Call before the first Render and whenever the
    // captured game changes. Returns false only if the directory cannot be created; a missing, corrupt or
    // incompatible profile is not an error (learning starts fresh, and the fact is logged).
    virtual bool SetProfile(const std::wstring& directory, const std::wstring& gameKey) = 0;

    // Advances the history with the generator's newest real frame (RealFrame(0)) and its motion field, and writes
    // the reconstructed image into `dst`, a render target of exactly `outputSize`. Call EXACTLY once per
    // PushFrame, after it. With no valid pair, on a scene cut, or after Reset(), the history restarts from the
    // single-frame reconstruction. Returns false on a D3D failure. Leaves no RTV / SRV / UAV bound.
    virtual bool Render(const FrameGenerator& motion, ID3D11RenderTargetView* dst, Size outputSize,
                        const AlsrSettings& settings) = 0;

    // Output of the last Render as a texture (BGRA8 UNORM, outputSize), or null.
    virtual ID3D11Texture2D* LastOutput() const = 0;

    // Upscales an arbitrary frame-sized image (e.g. a GENERATED frame) with the single-frame reconstruction
    // (learned filters included) and the same sharpening, without touching the history or the learning state.
    virtual bool RenderSingle(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                              const AlsrSettings& settings) = 0;

    // Drops the temporal history (not the learned profile).
    virtual void Reset() = 0;

    // Writes the profile now. Also happens automatically every few minutes and on destruction.
    virtual bool SaveProfile() = 0;

    // Forgets everything learned for the current profile and deletes its file.
    virtual void ResetLearning() = 0;

    virtual AlsrLearningStatus LearningStatus() const = 0;

    virtual void SetDebugView(AlsrDebugView view) = 0;
};

} // namespace moonsr
