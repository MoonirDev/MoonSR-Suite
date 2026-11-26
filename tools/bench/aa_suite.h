// moonsr_bench --aa-suite / --aa-sweep: thorough anti-aliasing benchmark with supersampled ground truth, gate checks,
// a settings sweep and an owner visual-review pack. Owner: benchmark-specialist with upscaling-engineer.
//
// Design and thresholds: documentation/research/aa_benchmark.md. The fast regression mode remains RunAaBench() in
// aa_bench.h; this suite is the acceptance gate for filter changes and setting choices.
#pragma once
#include "common.h"
#include "core/types.h"

#include <filesystem>
#include <vector>

namespace bench {

struct AaSuiteOptions {
    std::filesystem::path outDir;
    std::vector<std::filesystem::path> images; // real captures, no ground truth (review + do-no-harm stats)
    uint32_t width = 1280, height = 720;
    int supersample = 4;

    // Candidate grid. The current default (Balanced, 1.0, 0.10, 0.03, protection on) is always added.
    bool sweep = false;
    std::vector<int> qualities{0, 1, 2};
    std::vector<float> strengths{0.70f, 0.85f, 1.0f};
    std::vector<float> thresholds{0.08f, 0.10f, 0.12f};
    std::vector<float> floors{0.03f};
    bool protect = true;

    bool timing = true;
    bool saveImages = true;
};

// Runs the suite (and the sweep when o.sweep) on `adapter`, writes the report to o.outDir, prints the ranked table.
// Returns 1 if the current default fails a gate (the suite itself can still complete and report other candidates).
int RunAaSuite(const AaSuiteOptions& o, const moonsr::AdapterInfo& adapter);

} // namespace bench
