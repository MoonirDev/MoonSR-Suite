#pragma once
#include "common.h"
#include "gpu/gpu_device.h"
#include "upscale/scale_pass.h"

#include <memory>
#include <string>
#include <vector>

namespace bench {

struct TimingResult {
    uint32_t samples = 0;          // iterations that contributed (disjoint-free batches only)
    uint32_t discardedBatches = 0; // disjoint batches thrown away
    double meanMs = 0, medianMs = 0, minMs = 0, maxMs = 0, p95Ms = 0;
    uint64_t timestampFreq = 0;
    // Cross-check: CPU wall-clock time from first submit of a batch to its last timestamp being readable, divided by
    // the iterations in the batch (accumulated over the accepted batches). Includes submission cost and the final wait,
    // so it is an upper bound on the GPU time when the GPU is the bottleneck and larger when the CPU submission is.
    double wallPerIterMs = 0;
};

struct TimingParams {
    uint32_t inW = 1280, inH = 720, outW = 1920, outH = 1080;
    moonsr::ScalerKind scaler = moonsr::ScalerKind::Bilinear;
    float sharpness = 0.0f;
    uint32_t iterations = 200;
    uint32_t warmup = 50;
    uint32_t idleMs = 0; // Optional pause between small batches when generating competing GPU load.
};

// One GPU device plus a ScalePass created on it.
class GpuRunner {
public:
    static std::unique_ptr<GpuRunner> Open(const moonsr::AdapterInfo& info, std::string& err);

    const moonsr::AdapterInfo& Info() const { return info_; }

    // Uploads `in` as B8G8R8A8_UNORM, renders with FitMode::Stretch into a B8G8R8A8_UNORM target of dw x dh and reads
    // the result back.
    bool Upscale(const Image& in, uint32_t dw, uint32_t dh, moonsr::ScalerKind scaler, float sharpness, Image& out,
                 std::string& err);

    // Times ScalePass::Render with D3D11 timestamp queries. `content` supplies the source pixels (any image of size
    // inW x inH). Each iteration is bracketed individually by two timestamps inside a disjoint query.
    bool Time(const TimingParams& p, const Image& content, TimingResult& out, std::string& err);

private:
    moonsr::AdapterInfo info_;
    std::unique_ptr<moonsr::GpuDevice> dev_;
    std::unique_ptr<moonsr::ScalePass> pass_;
};

std::string DriverVersionString(uint64_t packed);

} // namespace bench
