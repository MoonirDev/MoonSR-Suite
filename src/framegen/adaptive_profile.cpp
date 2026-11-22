#include "framegen/adaptive_profile.h"
#include "core/game_profile.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace moonsr::fgadaptive {
namespace {
void Mix(uint64_t& h, uint64_t value) {
    for (int i = 0; i < 8; ++i) { h ^= value & 0xff; h *= 1099511628211ULL; value >>= 8; }
}
void MixText(uint64_t& h, const std::wstring& value) {
    Mix(h, value.size());
    for (const auto c : value) Mix(h, static_cast<uint16_t>(c));
}
}

uint64_t ProfileKey(const ProfileContext& c) {
    uint64_t h = 14695981039346656037ULL;
    MixText(h, c.game); MixText(h, c.gpuName);
    Mix(h, c.gpuVendor); Mix(h, c.gpuDevice); Mix(h, c.width); Mix(h, c.height);
    Mix(h, c.maxFlowPercent); Mix(h, c.hudProtection ? 1 : 0);
    Mix(h, c.driverVersion);
    Mix(h, c.candidateFlowPercent);
    if (c.preciseMotion) Mix(h, 0x7072656369736531ULL); // preserve existing keys for the default mode
    return h;
}

std::wstring LegacyProfilePath(const std::wstring& directory, const ProfileContext& context) {
    std::wostringstream name;
    name << L"fg-" << std::hex << std::setw(16) << std::setfill(L'0') << ProfileKey(context) << L".profile";
    return (std::filesystem::path(directory) / name.str()).wstring();
}

std::wstring ProfilePath(const std::wstring& directory, const ProfileContext& context) {
    const auto gameDirectory = GameProfileDirectory(directory, context.gameIdentity);
    return LegacyProfilePath(gameDirectory.empty() ? directory : gameDirectory, context);
}

bool LoadProfile(const std::wstring& path, const ProfileContext& context, Evidence& evidence) {
    std::ifstream f{std::filesystem::path(path)};
    std::string magic;
    unsigned version = 0;
    uint64_t key = 0;
    size_t count = 0;
    if (!(f >> magic >> version >> key >> count) || magic != "MOONSR_FG" || version != 5 ||
        key != ProfileKey(context) || count > 128) return false;
    Evidence candidate;
    for (size_t i = 0; i < count; ++i) {
        Observation o;
        unsigned order = 0;
        if (!(f >> o.sampleId >> o.baselineError >> o.candidateError >> o.baselineGpuMs >> o.candidateGpuMs >> order) || order > 1)
            return false;
        o.candidateFirst = order != 0;
        if (!candidate.Add(o)) return false;
    }
    std::string trailing;
    if (f >> trailing) return false;
    evidence = std::move(candidate);
    return true;
}

bool SaveProfile(const std::wstring& path, const ProfileContext& context, const Evidence& evidence) {
    if (path.empty()) return false;
    const std::filesystem::path target(path);
    const std::filesystem::path temp(path + L".tmp");
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return false;
    {
        std::ofstream f(temp, std::ios::trunc);
        if (!f) return false;
        f << "MOONSR_FG 5 " << ProfileKey(context) << ' ' << evidence.Samples().size() << '\n';
        f << std::setprecision(std::numeric_limits<double>::max_digits10);
        for (const auto& s : evidence.Samples())
            f << s.sampleId << ' ' << s.baselineError << ' ' << s.candidateError << ' ' << s.baselineGpuMs << ' ' << s.candidateGpuMs
              << ' ' << (s.candidateFirst ? 1 : 0) << '\n';
        f.flush();
        if (!f) return false;
    }
    return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
} // namespace moonsr::fgadaptive
