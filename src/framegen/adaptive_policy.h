// CPU-only evidence gate for experimental per-game frame-generation tuning.
// Each observation compares two configurations against the same held-out real frame.
#pragma once

#include <cstdint>
#include <vector>

namespace moonsr::fgadaptive {

struct Observation {
    uint64_t sampleId = 0;
    double baselineError = 0.0;  // mean squared error against the real middle frame
    double candidateError = 0.0;
    double baselineGpuMs = 0.0;
    double candidateGpuMs = 0.0;
    bool candidateFirst = false; // evaluation order for separating pipeline queue delay from GPU work
};

struct EvidenceStatus {
    uint32_t training = 0, validation = 0;
    uint32_t baselineTimings = 0, candidateTimings = 0;
    double trainingBaseline = 0.0, trainingCandidate = 0.0;
    double validationBaseline = 0.0, validationCandidate = 0.0;
    double baselineGpuMs = 0.0, candidateGpuMs = 0.0;
    bool recentRegression = false;
    bool accepted = false;
};

// Retains a bounded recent sample window. One quarter is assigned to validation by sample id, independently of
// the measured values. A candidate is never accepted on training data alone.
class Evidence {
public:
    bool Add(const Observation& observation);
    EvidenceStatus Status() const;
    void Reset();
    const std::vector<Observation>& Samples() const { return samples_; }

private:
    std::vector<Observation> samples_;
};

} // namespace moonsr::fgadaptive
