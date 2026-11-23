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
    bool saveImages = false;
    std::filesystem::path outDir;              // PNGs (input, relief, height, split, protection, crops)
};

// Runs every scene at every strength on `adapter`, prints the metric table, the self-checks and the GPU timing.
// Returns 1 if any self-check fails.
int RunReliefBench(const ReliefBenchOptions& o, const moonsr::AdapterInfo& adapter);

} // namespace bench
