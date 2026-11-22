// Collects and evaluates small triplets of real captured frames on the selected processing GPU.
// All methods are called on the D3D immediate-context owner thread; evaluation readback never waits.
#pragma once
#include "framegen/adaptive_policy.h"
#include "framegen/frame_generator.h"
#include <memory>
#include <optional>
#include <utility>

namespace moonsr::fgadaptive {
// Start with the original 3x3 coverage, then explore the spaces between those locations deterministically.
std::pair<uint32_t, uint32_t> SampleTileOffset(uint32_t ordinal, uint32_t spanX, uint32_t spanY);

struct SamplerDiagnostics {
    uint64_t frames = 0, unstableFrames = 0, triplets = 0, timingRejected = 0;
    uint64_t expired = 0, motionRejected = 0, evaluations = 0, readbackRejected = 0;
    bool unsupportedFormat = false, allocationFailed = false, evaluationFailed = false;
};
class TripletSampler {
public:
    static std::unique_ptr<TripletSampler> Create(GpuDevice& gpu, const FrameGenSettings& baseline,
                                                   float candidateFlow, std::wstring& error,
                                                   double intervalMs = 2000.0, bool fullContext = false);
    virtual ~TripletSampler() = default;
    // Supply consecutive ORIGINAL frames. stable must be false after a drop, hitch, or source discontinuity.
    virtual void Observe(ID3D11Texture2D* frame, const RectI& rect, int64_t qpc, bool stable) = 0;
    // Dispatch pending evaluation only when the caller considers the pipeline idle. Never waits for GPU completion.
    virtual void SubmitIfReady(bool idle) = 0;
    // Returns a valid comparison only after asynchronous GPU readback and CPU evaluation complete.
    virtual std::optional<Observation> Poll() = 0;
    virtual bool Busy() const = 0;
    virtual bool Ready() const = 0;
    virtual SamplerDiagnostics Diagnostics() const = 0;
    // Back off sampling during sustained presentation overload to avoid repeated unused GPU copies.
    virtual void SetOverloaded(bool overloaded) = 0;
    virtual void Reset() = 0;
};
} // namespace moonsr::fgadaptive
