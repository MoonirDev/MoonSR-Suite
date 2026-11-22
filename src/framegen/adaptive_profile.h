// Per-game frame-generation tuning profile. File operations are blocking; call on a worker thread.
#pragma once
#include "framegen/adaptive_policy.h"
#include <string>

namespace moonsr::fgadaptive {
struct ProfileContext {
    std::wstring game;
    std::wstring gpuName;
    uint32_t gpuVendor = 0, gpuDevice = 0;
    uint32_t width = 0, height = 0, maxFlowPercent = 0;
    bool hudProtection = true;
    uint64_t driverVersion = 0;
    uint32_t candidateFlowPercent = 0;
    std::wstring gameIdentity; // canonical executable path, without pipeline qualifiers
    bool preciseMotion = false; // separate evidence when the motion estimator changes
};

// Stable filename key; profiles of distinct games, GPUs, resolutions and user settings do not share evidence.
uint64_t ProfileKey(const ProfileContext& context);
std::wstring ProfilePath(const std::wstring& directory, const ProfileContext& context);
std::wstring LegacyProfilePath(const std::wstring& directory, const ProfileContext& context);
// Returns false for missing, corrupt or incompatible files; never replaces the supplied evidence on failure.
bool LoadProfile(const std::wstring& path, const ProfileContext& context, Evidence& evidence);
// Writes a temporary file beside the target and atomically replaces it on success.
bool SaveProfile(const std::wstring& path, const ProfileContext& context, const Evidence& evidence);
} // namespace moonsr::fgadaptive
