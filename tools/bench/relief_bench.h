// moonsr_bench --relief: Smart parallax measurement (synthetic scenes with known height). Owner: upscaling-engineer.
#pragma once
#include "common.h"
#include "core/types.h"

#include <filesystem>
#include <vector>

namespace bench {

struct ReliefBenchOptions {
    std::vector<float> strengths{0.5f, 1.0f}; // proposed default strength and the maximum
    float detailScalePx = -1.0f; // < 0: the ReliefSettings default
    bool timing = true;
    bool parallax = false;
    bool saveImages = false;
    std::filesystem::path outDir;
    std::vector<std::filesystem::path> images; // --relief-image: real screenshots (any WIC-decodable format)
};

// Runs every scene at every strength on `adapter`, prints the metric table, the self-checks and the GPU timing.
// Returns 1 if any self-check fails.
int RunReliefBench(const ReliefBenchOptions& o, const moonsr::AdapterInfo& adapter);
// Runs each image of o.images as a static sequence and then a small pan; prints coverage and auto-disable state.
int RunReliefImage(const ReliefBenchOptions& o, const moonsr::AdapterInfo& adapter);
int RunReliefMotionBench(const moonsr::AdapterInfo& adapter);

} // namespace bench
