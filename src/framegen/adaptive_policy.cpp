#include "framegen/adaptive_policy.h"

#include <algorithm>
#include <cmath>

namespace moonsr::fgadaptive {
namespace {
constexpr size_t kMaxSamples = 128;
constexpr uint32_t kMinTraining = 24;
constexpr uint32_t kMinValidation = 8;

bool Validation(uint64_t id) {
    // Splitmix64 finalizer: sequential frame IDs are distributed across both independent sets.
    id ^= id >> 30; id *= 0xbf58476d1ce4e5b9ULL;
    id ^= id >> 27; id *= 0x94d049bb133111ebULL;
    id ^= id >> 31;
    return (id & 3u) == 0;
}
}

bool Evidence::Add(const Observation& o) {
    if (!std::isfinite(o.baselineError) || !std::isfinite(o.candidateError) ||
        !std::isfinite(o.baselineGpuMs) || !std::isfinite(o.candidateGpuMs) ||
        o.baselineError < 0 || o.candidateError < 0 || o.baselineGpuMs <= 0 || o.candidateGpuMs <= 0)
        return false;
    if (std::any_of(samples_.begin(), samples_.end(), [&](const auto& s) { return s.sampleId == o.sampleId; }))
        return false;
    if (samples_.size() == kMaxSamples) samples_.erase(samples_.begin());
    samples_.push_back(o);
    return true;
}

EvidenceStatus Evidence::Status() const {
    EvidenceStatus s;
    double trainA = 0, trainB = 0, validA = 0, validB = 0;
    uint32_t severe = 0;
    std::vector<double> baselineSecond, candidateSecond;
    baselineSecond.reserve(samples_.size()); candidateSecond.reserve(samples_.size());
    for (const auto& o : samples_) {
        // The first evaluation can contain CPU/GPU queue gaps after a Present. Compare only the second run of
        // each ordering, whose GPU work is already queued, and use medians to resist unrelated preemption.
        if (o.candidateFirst) baselineSecond.push_back(o.baselineGpuMs);
        else candidateSecond.push_back(o.candidateGpuMs);
        if (Validation(o.sampleId)) {
            ++s.validation; validA += o.baselineError; validB += o.candidateError;
            if (o.candidateError > o.baselineError * 1.15 + 1e-8) ++severe;
        } else {
            ++s.training; trainA += o.baselineError; trainB += o.candidateError;
        }
    }
    if (s.training) { s.trainingBaseline = trainA / s.training; s.trainingCandidate = trainB / s.training; }
    if (s.validation) { s.validationBaseline = validA / s.validation; s.validationCandidate = validB / s.validation; }
    std::sort(baselineSecond.begin(), baselineSecond.end());
    std::sort(candidateSecond.begin(), candidateSecond.end());
    s.baselineTimings = static_cast<uint32_t>(baselineSecond.size());
    s.candidateTimings = static_cast<uint32_t>(candidateSecond.size());
    if (!baselineSecond.empty()) s.baselineGpuMs = baselineSecond[baselineSecond.size() / 2];
    if (!candidateSecond.empty()) s.candidateGpuMs = candidateSecond[candidateSecond.size() / 2];
    // A scene can change much faster than the full 128-sample history. Reject a currently applied candidate
    // when three of the latest four independently captured tiles show a large, visible error increase.
    // The absolute margin prevents low-level noise in near-perfect reconstructions from triggering the guard.
    uint32_t recentSevere = 0;
    const size_t recentStart = samples_.size() > 4 ? samples_.size() - 4 : 0;
    for (size_t i = recentStart; i < samples_.size(); ++i) {
        const auto& o = samples_[i];
        if (o.candidateError > o.baselineError * 1.5 + 1.0) ++recentSevere;
    }
    s.recentRegression = samples_.size() >= 4 && recentSevere >= 3;
    // Require real reconstruction evidence, similar error on both sets, no frequent severe held-out regressions,
    // and a measured 5% GPU saving on the sampled tiles. Zero-error/zero-motion samples cannot establish useful quality evidence.
    s.accepted = s.training >= kMinTraining && s.validation >= kMinValidation && trainA > 1e-8 && validA > 1e-8 &&
                 trainB <= trainA * 1.02 && validB <= validA * 1.02 && severe * 10 <= s.validation &&
                 baselineSecond.size() >= 8 && candidateSecond.size() >= 8 &&
                 s.candidateGpuMs <= s.baselineGpuMs * 0.95 && !s.recentRegression;
    return s;
}

void Evidence::Reset() { samples_.clear(); }

} // namespace moonsr::fgadaptive
