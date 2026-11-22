// Per-game adaptive frame-generation controller. All public methods run on the D3D owner thread.
#pragma once
#include "framegen/adaptive_profile.h"
#include "framegen/adaptive_sampler.h"
#include <memory>

namespace moonsr::fgadaptive {
struct ControllerStatus {
    bool enabled = false, collecting = false, profileLoaded = false, accepted = false, persistent = false;
    uint32_t training = 0, validation = 0;
    float requestedFlow = 0.0f, effectiveFlow = 0.0f;
    double baselineError = 0.0, candidateError = 0.0;
    SamplerDiagnostics sampler;
    std::wstring note;
};

class Controller {
public:
    explicit Controller(GpuDevice& gpu);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // Reconfiguration discards in-flight observations and restores the requested setting until validated.
    void Configure(bool enabled, const ProfileContext& context, const FrameGenSettings& baseline,
                   const std::wstring& profileDirectory);
    void Observe(ID3D11Texture2D* frame, const RectI& rect, int64_t qpc, bool stable);
    // Called regularly even while disabled, so in-flight work can retire without waiting.
    void Tick(bool idle);
    bool ReadyForEvaluation() const;
    void SetOverloaded(bool overloaded);
    // Deletes only the active game/GPU/resolution profile; pending old results are discarded.
    void ResetLearning();
    FrameGenSettings EffectiveSettings() const;
    ControllerStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace moonsr::fgadaptive
