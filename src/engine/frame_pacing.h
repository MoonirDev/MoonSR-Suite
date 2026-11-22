#pragma once
#include <algorithm>
#include <cstdint>

namespace moonsr {

// Start higher requests at a modest rate and let the measured refresh-cap logic raise them when they fit.
inline uint32_t StartingFrameMultiplier(uint32_t requested, uint32_t previous) {
    return requested <= 1 ? 1 : std::min(requested, std::max(previous, 2u));
}

// A real frame or a frame that is still near its slot is never skipped.
inline bool ShouldSkipStaleGenerated(bool vsync, bool real, double slotMs, double latenessMs, double catchupSlots) {
    return catchupSlots > 0.0 && vsync && !real && slotMs > 0.0 && latenessMs > catchupSlots * slotMs;
}

} // namespace moonsr
