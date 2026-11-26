// moonsr_bench --aa: anti-aliasing measurement (synthetic aliased scenes with known masks).
// Owner: upscaling-engineer (algorithms) with shader-engineer (HLSL under shaders/aa/).
#pragma once
#include "common.h"
#include "core/types.h"

#include <filesystem>
#include <vector>

namespace bench {

struct AaBenchOptions {
    bool timing = true;
    bool saveImages = false;
    std::filesystem::path outDir;
    std::vector<std::filesystem::path> images; // --aa-image: real screenshots (any WIC-decodable format)
    // Settings override; -1 / empty keeps the defaults (Balanced, strength 1.0).
    int quality = -1;   // 0 Performance, 1 Balanced, 2 Quality
    float strength = -1.0f;
};

// Runs the synthetic scenes (hard edges at several angles, thin lines, text/HUD over texture, high-frequency texture,
// gradient) through the real AaPass on `adapter`, prints the metric table, the self-checks and the GPU timing.
// Returns 1 if any self-check fails.
int RunAaBench(const AaBenchOptions& o, const moonsr::AdapterInfo& adapter);
// Runs each image of o.images through the pass at its own size, prints coverage / texture-energy / timing and writes
// the filtered image next to the source when saveImages is on.
int RunAaImage(const AaBenchOptions& o, const moonsr::AdapterInfo& adapter);

} // namespace bench
