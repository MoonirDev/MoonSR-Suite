#include "framegen/adaptive_controller.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>

namespace moonsr::fgadaptive {
namespace {
struct LoadResult { uint64_t generation = 0; bool loaded = false; Evidence evidence; };

constexpr unsigned kFlowStages = 3;

float CandidateFlow(float base, unsigned stage) {
    constexpr float factors[kFlowStages] = {0.75f, 0.90f, 0.50f};
    const float factor = factors[stage];
    return std::max(0.25f, std::floor((base * factor + 0.0001f) * 20.0f) / 20.0f);
}

// Serializes profile operations off the rendering thread. FIFO ordering prevents a reset from being undone by
// an older queued save. The worker never accesses the D3D device or captured image data.
class ProfileWorker {
    enum class Op { Load, Save, Clear };
    struct Job { Op op; uint64_t generation; std::wstring path; ProfileContext context; Evidence evidence; std::wstring legacy; };
public:
    ProfileWorker() : thread_([this] { Run(); }) {}
    ~ProfileWorker() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    void Load(uint64_t generation, const std::wstring& path, const ProfileContext& context, const std::wstring& legacy) {
        Push({Op::Load, generation, path, context, {}, legacy});
    }
    void Save(const std::wstring& path, const ProfileContext& context, const Evidence& evidence) {
        if (!path.empty() && !evidence.Samples().empty()) Push({Op::Save, 0, path, context, evidence});
    }
    void Clear(const std::wstring& path) { if (!path.empty()) Push({Op::Clear, 0, path, {}, {}}); }
    bool Take(LoadResult& result) {
        std::lock_guard lock(mutex_);
        if (results_.empty()) return false;
        result = std::move(results_.front()); results_.pop_front();
        return true;
    }
private:
    void Push(Job job) {
        { std::lock_guard lock(mutex_); jobs_.push_back(std::move(job)); }
        cv_.notify_one();
    }
    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty() && stopping_) break;
                job = std::move(jobs_.front()); jobs_.pop_front();
            }
            try {
                if (job.op == Op::Load) {
                    LoadResult result;
                    result.generation = job.generation;
                    result.loaded = LoadProfile(job.path, job.context, result.evidence);
                    if (!result.loaded && job.legacy != job.path && !job.legacy.empty() &&
                        GetFileAttributesW(job.path.c_str()) == INVALID_FILE_ATTRIBUTES &&
                        LoadProfile(job.legacy, job.context, result.evidence)) {
                        result.loaded = true;
                        if (SaveProfile(job.path, job.context, result.evidence)) DeleteFileW(job.legacy.c_str());
                    }
                    std::lock_guard lock(mutex_); results_.push_back(std::move(result));
                } else if (job.op == Op::Save) {
                    if (!SaveProfile(job.path, job.context, job.evidence))
                        MSR_LOG_WARN(L"Adaptive frame generation: could not save profile {}", job.path);
                } else if (!DeleteFileW(job.path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                    MSR_LOG_WARN(L"Adaptive frame generation: could not remove profile {} (error {})", job.path, GetLastError());
                }
            } catch (const std::exception&) {
                MSR_LOG_WARN(L"Adaptive frame generation: profile operation failed");
                if (job.op == Op::Load) {
                    std::lock_guard lock(mutex_); results_.push_back({job.generation, false, {}});
                }
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<LoadResult> results_;
    bool stopping_ = false;
    std::thread thread_;
};
}

struct Controller::Impl {
    explicit Impl(GpuDevice& g) : gpu(g) {}
    ~Impl() { Save(); }

    struct Config {
        bool enabled = false;
        ProfileContext context;
        FrameGenSettings baseline;
        std::wstring directory;
    };
    GpuDevice& gpu;
    ProfileWorker worker;
    Config desired, active;
    bool desiredSet = false, activeSet = false, switching = false, loadReady = false, loaded = false, applied = false;
    bool fullContext = false, overloaded = false;
    uint64_t generation = 0;
    uint32_t unsaved = 0;
    unsigned desiredStage = 0, activeStage = 0;
    float candidateFlow = 0;
    std::wstring path, failure;
    Evidence evidence;
    std::unique_ptr<TripletSampler> sampler;

    void Save() {
        if (activeSet && !path.empty() && unsaved) {
            worker.Save(path, active.context, evidence);
            unsaved = 0;
        }
    }
    void SwitchIfReady() {
        if (!switching) return;
        if (sampler && sampler->Busy()) { sampler->Poll(); return; }
        sampler.reset();
        active = desired;
        activeStage = desiredStage;
        activeSet = true;
        switching = false;
        evidence.Reset();
        failure.clear();
        loaded = false;
        applied = false;
        loadReady = false;
        path.clear();
        candidateFlow = 0;
        unsaved = 0;
        if (!active.enabled) return;
#ifdef MOONSR_EXPERIMENTAL_HARDWARE_FLOW
        char hardware[8]{};
        if (GetEnvironmentVariableA("MOONSR_FG_HARDWARE_FLOW",hardware,sizeof(hardware)) && hardware[0]=='1') {
            failure = L"Adaptive learning is unavailable for the experimental hardware-flow backend.";
            return;
        }
#endif
        if (active.baseline.quality != FrameGenQuality::Performance) {
            failure = L"Adaptive learning requires the Performance frame-generation profile.";
            return;
        }
        const float base = std::clamp(active.baseline.flowScale, 0.25f, 1.0f);
        // Try 75%, then 90%, then half resolution from a full-resolution baseline.
        candidateFlow = CandidateFlow(base, activeStage);
        if (candidateFlow >= base - 0.001f) { failure = L"Flow scale is already at its minimum."; return; }
        std::wstring error;
        wchar_t fullText[4]{};
        const DWORD fullLength = GetEnvironmentVariableW(L"MOONSR_FG_FULL_CONTEXT", fullText,
                                                         static_cast<DWORD>(std::size(fullText)));
        // Full-frame analysis is the normal mode. The tile-only mode remains available for diagnostics.
        fullContext = !(fullLength && fullText[0] == L'0');
        // Full-frame comparisons cost more than tile-only comparisons, so run them less frequently in normal use.
        double intervalMs = fullContext ? 8000.0 : 2000.0;
        wchar_t intervalText[16]{};
        if (GetEnvironmentVariableW(L"MOONSR_FG_SAMPLE_INTERVAL_MS", intervalText, static_cast<DWORD>(std::size(intervalText)))) {
            wchar_t* end = nullptr;
            const long value = wcstol(intervalText, &end, 10);
            if (end != intervalText && *end == L'\0') intervalMs = static_cast<double>(std::clamp(value, 100L, 10000L));
        }
        sampler = TripletSampler::Create(gpu, active.baseline, candidateFlow, error, intervalMs, fullContext);
        if (!sampler) { failure = error.empty() ? L"The sampler could not start." : error; return; }
        // Full-context evidence must never be confused with the existing tile-only profiles.
        if (fullContext && !active.context.game.empty()) active.context.game += L"|full-context-v1";
        if (!active.directory.empty() && !active.context.game.empty()) {
            active.context.candidateFlowPercent = static_cast<uint32_t>(std::lround(candidateFlow * 100.0f));
            path = ProfilePath(active.directory, active.context);
            worker.Load(generation, path, active.context, LegacyProfilePath(active.directory, active.context));
        } else {
            loadReady = true; // no trustworthy game identity or profile directory: session only
        }
    }
};

Controller::Controller(GpuDevice& gpu) : impl_(std::make_unique<Impl>(gpu)) {}
Controller::~Controller() = default;

void Controller::Configure(bool enabled, const ProfileContext& context, const FrameGenSettings& baseline,
                           const std::wstring& directory) {
    auto& p = *impl_;
    if (p.desiredSet && p.desired.enabled == enabled && ProfileKey(p.desired.context) == ProfileKey(context) &&
        p.desired.baseline == baseline && p.desired.directory == directory) return;
    if (!p.switching) p.Save();
    p.desired = {enabled, context, baseline, directory};
    p.desiredStage = 0;
    p.desiredSet = true;
    p.switching = true;
    ++p.generation;
    if (p.sampler) p.sampler->Reset();
}

void Controller::Tick(bool idle) {
    auto& p = *impl_;
    p.SwitchIfReady();
    LoadResult result;
    while (p.worker.Take(result)) {
        if (!p.switching && p.activeSet && result.generation == p.generation) {
            p.evidence = std::move(result.evidence);
            p.loaded = result.loaded;
            p.loadReady = true;
            p.applied = p.evidence.Status().accepted;
        }
    }
    if (p.switching || !p.active.enabled || !p.sampler || !p.loadReady) return;
    if (auto sample = p.sampler->Poll(); sample && p.evidence.Add(*sample)) {
        ++p.unsaved;
        const bool accepted = p.evidence.Status().accepted;
        const bool decisionChanged = accepted != p.applied;
        p.applied = accepted;
        // Persist a changed decision even if gameplay ends before the next periodic save.
        if (decisionChanged || p.unsaved >= 8) p.Save();
    }
    const auto status = p.evidence.Status();
    const float base = std::clamp(p.active.baseline.flowScale, 0.25f, 1.0f);
    const unsigned nextStage = p.activeStage + 1;
    const float nextFlow = nextStage < kFlowStages ? CandidateFlow(base, nextStage) : base;
    const bool anotherCandidate = (nextStage == 1 && nextFlow > p.candidateFlow + 0.001f) ||
                                  (nextStage == 2 && base == 1.0f);
    if (anotherCandidate && status.training >= 24 && status.validation >= 8 &&
        status.baselineTimings >= 8 && status.candidateTimings >= 8 && !status.accepted &&
        nextFlow < base - 0.001f) {
        p.Save();
        p.desiredStage = nextStage;
        p.switching = true;
        ++p.generation;
        p.sampler->Reset();
        return;
    }
    p.sampler->SubmitIfReady(idle && (!p.fullContext || !p.overloaded));
}

void Controller::Observe(ID3D11Texture2D* frame, const RectI& rect, int64_t qpc, bool stable) {
    auto& p = *impl_;
    if (!p.switching && p.activeSet && p.active.enabled && p.loadReady && p.sampler)
        p.sampler->Observe(frame, rect, qpc, stable);
}

bool Controller::ReadyForEvaluation() const {
    const auto& p = *impl_;
    return !p.switching && p.activeSet && p.active.enabled && p.loadReady && p.sampler && p.sampler->Ready();
}

void Controller::SetOverloaded(bool overloaded) {
    impl_->overloaded = overloaded;
    if (impl_->sampler) impl_->sampler->SetOverloaded(overloaded);
}

void Controller::ResetLearning() {
    auto& p = *impl_;
    if (!p.activeSet || p.switching) return;
    ++p.generation; // invalidates any pending profile load
    if (!p.active.directory.empty() && !p.active.context.game.empty()) {
        auto context = p.active.context;
        const float base = std::clamp(p.active.baseline.flowScale, 0.25f, 1.0f);
        for (unsigned stage = 0; stage < (base == 1.0f ? kFlowStages : 2); ++stage) {
            context.candidateFlowPercent = static_cast<uint32_t>(std::lround(CandidateFlow(base, stage) * 100.0f));
            p.worker.Clear(ProfilePath(p.active.directory, context));
            p.worker.Clear(LegacyProfilePath(p.active.directory, context));
        }
    }
    p.evidence.Reset();
    p.unsaved = 0;
    p.loaded = false;
    p.applied = false;
    p.loadReady = false;
    p.desiredStage = 0;
    p.switching = true;
    if (p.sampler) p.sampler->Reset();
}

FrameGenSettings Controller::EffectiveSettings() const {
    const auto& p = *impl_;
    FrameGenSettings result = p.desired.baseline;
    if (!p.switching && p.activeSet && p.active.enabled && p.loadReady && p.sampler && p.applied)
        result.flowScale = p.candidateFlow;
    return result;
}

ControllerStatus Controller::Status() const {
    const auto& p = *impl_;
    ControllerStatus s;
    s.enabled = p.desiredSet && p.desired.enabled;
    s.requestedFlow = p.desired.baseline.flowScale;
    s.effectiveFlow = EffectiveSettings().flowScale;
    if (!s.enabled) { s.note = L"Learning is off."; return s; }
    if (p.switching) { s.note = L"Preparing the current game profile."; return s; }
    if (!p.failure.empty()) { s.note = p.failure; return s; }
    s.persistent = !p.path.empty();
    s.profileLoaded = p.loaded;
    s.collecting = p.sampler && p.loadReady;
    if (p.sampler) s.sampler = p.sampler->Diagnostics();
    const auto e = p.evidence.Status();
    s.training = e.training; s.validation = e.validation;
    s.accepted = s.collecting && p.applied;
    s.baselineError = e.validationBaseline; s.candidateError = e.validationCandidate;
    s.note = !p.loadReady ? L"Loading the game profile."
             : s.sampler.allocationFailed ? L"Learning could not allocate GPU memory; retrying later."
             : s.sampler.evaluationFailed ? L"Learning could not evaluate a sample on the GPU; retrying later."
             : p.fullContext && p.overloaded ? L"Learning is paused while frame generation is falling behind."
             : s.accepted ? (p.fullContext ? L"Lower flow scale active after full-frame motion validation."
                                           : L"Lower flow scale active after sampled-tile validation.")
             : e.recentRegression ? L"Recent motion samples show a quality regression; using the selected flow scale."
             : e.training >= 24 && e.validation >= 8 ? L"The candidate has not passed validation; continuing to learn."
             : s.sampler.unsupportedFormat ? L"Learning cannot read this frame format."
             : !s.sampler.frames ? L"Waiting for game frames."
             : !s.sampler.triplets && s.sampler.unstableFrames > 30 ? L"Waiting for stable game frames."
             : s.sampler.expired > 2 && !s.sampler.evaluations ? L"Learning is waiting for GPU headroom."
             : s.sampler.motionRejected > 2 && !s.sampler.evaluations ? L"Waiting for suitable motion in the game."
             : L"Collecting and validating game samples.";
    if (p.loadReady && !s.persistent) s.note += L" Profile storage is unavailable; this session only.";
    return s;
}
} // namespace moonsr::fgadaptive
