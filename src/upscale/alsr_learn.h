// Moon ALSR per-game learning, CPU side (no D3D). Owner: upscaling-engineer. Internal to src/upscale (tools may use it
// for offline evaluation; the engine must not).
//
// WHAT IS LEARNED. A bank of linear filters (RAISR idea: Romano, Isidoro, Milanfar, "RAISR: Rapid and Accurate Image
// Super Resolution", IEEE TCI 2016; independent implementation, no code from the paper). Each output pixel is
//     out = sum_k w[k] * f[k],  f = { G, S(-2,-2) .. S(2,2) }        (kFeat = 26 features, applied per RGB channel)
// G = the generic single-frame reconstruction (MoonSR Spatial) at that output pixel, S = the 5x5 source pixels around
// the source pixel nearest to the output pixel's sample position. The generic tap makes "w = (1, 0, ..., 0)" the
// generic path exactly; every filter is regularised towards it (ridge with a prior), so with little evidence a
// learned filter IS the generic reconstruction.
// One filter per bucket = phase x geometry:
//   phase    : sub-pixel position of the output pixel relative to the source grid. When the scale is a ratio with a
//              period q <= 8 output pixels per axis (1.5x: q = 3, 2x: q = 2) phase = x mod q (exact); otherwise the
//              fractional sample offset is quantised into kPhaseBins bins.
//   geometry : structure tensor of the 5x5 source luma (central differences on the inner 3x3, binomial weights):
//              orientation (kAngles bins of the gradient direction), strength sqrt(lambda1) (kStrengths bins),
//              coherence (sqrt l1 - sqrt l2) / (sqrt l1 + sqrt l2) (kCoherences bins). HLSL mirror: GeoBucket() in
//              shaders/upscale/alsr_seed.hlsli - the two must stay identical.
//
// TRAINING PAIRS (self-supervised). A tile of a captured frame F (source resolution) is the truth. F_low = the tile
// reduced by the scale factor with the game's sampling model (Point: the F pixel containing each low pixel's centre,
// like a game without anti-aliasing; Area: exact box average in linear light, like a game with good anti-aliasing),
// quantised to 8 bit. G is computed on the CPU with a port of shaders/upscale/moonsr_spatial_ps.hlsl (SpatialCpu).
// Luma = 0.299 R + 0.587 G + 0.114 B of the gamma-encoded values (as the shaders), 0..1.
// Sampling model: estimated from the frames (edge-step sharpness statistic of F compared with that of both reduced
// versions; the model whose F_low looks like F wins) unless forced.
//
// ACCUMULATION. Per bucket, double precision normal equations of the training samples (A^T A upper triangle, A^T b,
// b^T b, n) and SEPARATELY of the held-out samples (1 in 8, hashed by position and frame). Because the held-out
// equations are kept, the held-out squared error of ANY filter is exact: SSE(w) = w'Hw - 2w'h + hb.
// SOLVE. (A^T A + mu I) w = A^T b + mu w_prior, mu = (kVirtual + 1e-3 n) * trace(A^T A) / (kFeat n), Cholesky.
// GATING / BLENDING. A bucket's filter is used only if (1) n_val >= kMinVal and its held-out SSE is at least
// kGateMargin below the generic SSE (self-supervised gate) AND (2) the same scale relation holds one pyramid level
// lower: a filter solved from pairs (F reduced twice -> F reduced) must also beat the generic reconstruction on the
// level-0 pairs (F reduced -> F), which it never saw (cross-scale transfer gate). (2) tests the assumption the whole
// method rests on - that what is learned at F/s -> F also holds at F -> F*s - and was added after measuring that
// (1) alone accepts filters that are much worse than generic against true ground truth (2x; see alsr_progress.md).
// alpha = evidence ramp * self-supervised improvement ramp * transfer improvement ramp; the published filter is
// alpha * w + (1 - alpha) * w_prior. SSE is convex in alpha and SSE(0) = generic, so the blended held-out error can
// never exceed the generic one (checked by the self-test).
//
// THREADING. Owner thread (the upscaler's D3D thread) calls everything; the worker thread only processes jobs,
// solves, publishes filters and autosaves; it never touches D3D. Owner-thread calls that never block: CanSubmit,
// JobBuffer, Submit, TakeFilters (try_lock), GetStatus (brief lock on a small struct). Calls that may wait for the job
// in progress (tens of ms): SetProfilePath, Configure (new scale only), Save, Reset, WaitIdle,
// destructor - never called from Render.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace moonsr::alsr {

constexpr uint32_t kWin = 5, kTaps = kWin * kWin; // 5x5 source taps
constexpr uint32_t kFeat = kTaps + 1;               // + generic tap (index 0)
constexpr uint32_t kTri = kFeat * (kFeat + 1) / 2;
constexpr uint32_t kAngles = 8, kStrengths = 3, kCoherences = 3;
constexpr uint32_t kGeo = kAngles * kStrengths * kCoherences; // 72
constexpr uint32_t kFilterStride = 28; // floats per bucket in the GPU table: 26 weights, [26] = alpha, [27] = 0
constexpr uint32_t kMaxPeriod = 8, kPhaseBins = 4;
constexpr uint32_t kTile = 240;        // training tile edge in F pixels (divisible by most periods)
constexpr float kStrengthT[2] = {0.015f, 0.06f};
// Learning is used only up to this output/source ratio per axis. Evidence (synthetic, TRUE ground truth, 30 training
// frames; alsr_progress.md 3a.3): at 1.5x the correct sampling model gains +1.5 dB (trained content) / +0.5 dB (unseen
// content) for both aliased and prefiltered input; at 2x every model / input combination is neutral or worse (point
// model on aliased input -6.7 dB) while the self-supervised and transfer gates stay open, i.e. no self-supervised
// test available here detects the failure. Above the limit the generic path is used and nothing is collected.
constexpr float kMaxLearnScale = 1.75f;
inline bool ScaleSupported(float sx, float sy) { return sx > 1.01f && sy > 1.01f && sx <= kMaxLearnScale && sy <= kMaxLearnScale; }
constexpr float kCoherenceT[2] = {0.3f, 0.6f};

enum class SamplingModel : uint32_t { Unknown = 0, Point = 1, Area = 2 };
const wchar_t* ModelName(SamplingModel m);

// Phase structure of one axis for output/source ratio s.
struct Axis {
    float s = 1;           // output / source
    uint32_t q = 1;        // phases
    bool periodic = false; // q = exact period in output pixels; otherwise kPhaseBins quantised bins
};
Axis MakeAxis(float s);
// Phase of output pixel x (index) on an axis; `f` = its continuous source index coordinate ((x + 0.5) / s - 0.5).
// Centre source pixel of the 5x5 window = the source pixel nearest to f. The 1e-3 bias makes exact half-way cases
// (1.5x: every third output pixel) resolve the same way on the CPU and the GPU despite last-bit rounding differences.
inline int CentreOf(float f) { return static_cast<int>(f + 0.5f + 1e-3f + 4096.0f) - 4096; }
inline uint32_t PhaseOf(const Axis& a, uint32_t x, float f) {
    if (a.periodic) return x % a.q;
    const float t = f - static_cast<float>(CentreOf(f)) + 0.5f; // ~0..1
    const int b = static_cast<int>(t * kPhaseBins);
    return static_cast<uint32_t>(std::clamp(b, 0, static_cast<int>(kPhaseBins) - 1));
}
// Geometric bucket (0..kGeo-1) of a 5x5 luma window (row-major, 0..1).
uint32_t GeoBucket(const float l[kTaps]);

// CPU port of shaders/upscale/moonsr_spatial_ps.hlsl (defaults: param1 = param2 = 0) for one output pixel (dx, dy)
// of a dw x dh output from a sw x sh 8-bit source (4 bytes per pixel, `bgra` selects the channel order). Returns
// gamma RGB 0..1 (not quantised).
void SpatialCpu(const uint8_t* src, size_t pitch, bool bgra, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                uint32_t dx, uint32_t dy, float rgb[3]);

// CPU reference of the GPU seed (shaders/upscale/alsr_seed.hlsli) for output row `dy`: writes generic and learned
// output (4 bytes per pixel, same channel order as the source, quantised to 8 bit) for every x, and optionally each
// pixel's bucket. `table` = published filter table for axes (ax, ay). Tools only.
void ApplyCpuRow(const std::vector<float>& table, const Axis& ax, const Axis& ay, const uint8_t* src, size_t pitch,
                 bool bgra, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint32_t dy, uint8_t* genericRow,
                 uint8_t* learnedRow, uint32_t* bucketRow = nullptr);

// A training tile (owner thread fills it while CanSubmit()). Pixels: 4 bytes, BGRA or RGBA, `pitch` bytes per row.
struct Job {
    uint32_t w = 0, h = 0;
    size_t pitch = 0;
    bool bgra = true;
    uint64_t frame = 0;
    std::vector<uint8_t> px; // allocated once (kTile x kTile x 4)
};

struct Status {
    bool profileLoaded = false;
    bool configured = false;
    uint64_t samples = 0;       // training + held-out samples of the active section (all sessions)
    uint32_t solves = 0;
    uint32_t jobs = 0;          // jobs processed in this session
    float coverage = 0;         // share of buckets with alpha > 0
    float genericError = 0, learnedError = 0; // held-out mean |error| 0..255, recent jobs, with the filters in use
    double genericRmse = 0, learnedRmse = 0;  // held-out RMS 0..255 over ALL held-out samples (exact, from the equations)
    SamplingModel model = SamplingModel::Unknown;
    double workerMs = 0;        // worker CPU time per job (mean, this session)
    uint64_t profileBytes = 0;  // size of the serialised profile
};

class Learner {
public:
    Learner();
    ~Learner(); // stops the worker and saves (if a path is set)
    Learner(const Learner&) = delete;
    Learner& operator=(const Learner&) = delete;

    // Profile file (empty = no persistence). Loads it if present (all scale sections). Returns true if a valid
    // profile was loaded.
    bool SetProfilePath(const std::wstring& path);
    // Selects (creating if needed) the section for output/source ratios (sx, sy).
    void Configure(float sx, float sy);
    Axis AxisX() const { return ax_; } // owner thread, after Configure
    Axis AxisY() const { return ay_; }

    bool CanSubmit() const { return !busy_.load(std::memory_order_acquire); }
    Job& JobBuffer() { return job_; }
    void Submit();

    // Owner thread, never blocks: copies the newest published filter table (Buckets() x kFilterStride floats) into
    // `dst` (resized by the caller beforehand to BucketCount() * kFilterStride) if it changed since the last call.
    bool TakeFilters(std::vector<float>& dst);
    uint32_t BucketCount() const { return ax_.q * ay_.q * kGeo; }

    bool Save();   // atomic write (temp file + replace); true if saved or nothing to save
    void Reset();  // forgets everything learned (all sections) and deletes the file
    Status GetStatus() const;
    std::wstring Path() const { return path_; }

    // Waits until the worker has finished the current job.
    void WaitIdle();

private:
    struct Eq {
        double ata[kTri];
        double atb[kFeat];
        double btb;
        uint64_t n;
    };
    struct Bucket {
        Eq tr, va;       // level 0: training / held-out
        Eq lo;           // level -1 (pairs one pyramid level lower), all samples: cross-scale transfer gate
        float w[kFeat];  // solved (unblended)
        float alpha;
        float transfer;  // relative SSE reduction of the level -1 filter on the level-0 samples (diagnostic)
    };
    struct Section {
        float sx = 0, sy = 0;
        SamplingModel model = SamplingModel::Unknown;
        bool forced = false;
        uint32_t solves = 0;
        double estF = 0, estP = 0, estA = 0; // model estimation: summed edge-step sharpness statistic
        uint32_t estJobs = 0;
        Axis ax, ay;
        std::vector<Bucket> b;
    };

    void Worker();
    void Process(const Job& j, Section& s);
    void Solve(Section& s);
    void Publish(const Section& s);
    void UpdateStatusLocked(const Section& s);
    bool SaveLocked();
    bool LoadLocked();
    std::vector<uint8_t> Serialize() const;
    bool Deserialize(const std::vector<uint8_t>& data, std::wstring& why);
    static void InitSection(Section& s, float sx, float sy);
    Section* Active() { return active_ >= 0 ? &sections_[active_] : nullptr; }

    std::wstring path_;
    std::vector<Section> sections_;
    int active_ = -1;
    Axis ax_, ay_;
    std::atomic<bool> configured_{false};
    bool loaded_ = false;
    bool dirty_ = false;
    SamplingModel forceModel_ = SamplingModel::Unknown;
    Job job_, work_;
    uint32_t solveEvery_ = 8, jobsSinceSolve_ = 0;
    uint32_t autosaveSec_ = 180;
    bool transferGate_ = true;

    // per-session running held-out MAE (decayed per job), sums 0..1 luma
    double maeN_ = 0, maeG_ = 0, maeL_ = 0;
    uint32_t jobs_ = 0;
    double workerMsSum_ = 0;
    uint64_t profileBytes_ = 0;

    mutable std::mutex learnMutex_; // sections_, active_, work_, all learning state (worker holds it per job)
    std::mutex jobMutex_;
    std::condition_variable cv_, idleCv_;
    bool jobReady_ = false, stop_ = false;
    std::atomic<bool> busy_{false};

    std::mutex filterMutex_; // filters_, filtersVersion_
    std::vector<float> filters_;
    uint64_t filtersVersion_ = 0, takenVersion_ = 0;

    mutable std::mutex statusMutex_;
    Status status_;
    std::chrono::steady_clock::time_point lastSave_;
    std::thread thread_;
};

} // namespace moonsr::alsr
