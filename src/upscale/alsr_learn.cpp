// Moon ALSR per-game learning, CPU side. Owner: upscaling-engineer. Design: see alsr_learn.h.
#include "upscale/alsr_learn.h"

#include "core/log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace moonsr::alsr {

namespace {

constexpr uint32_t kMinTrain = 96;     // training samples before a bucket is solved
constexpr uint32_t kMinVal = 24;       // held-out samples before a bucket may be used
constexpr double kVirtual = 32.0;      // ridge strength in "virtual samples" pulling towards the generic filter
constexpr double kGateMargin = 0.01;   // held-out SSE must be >= 1 % below the generic SSE
constexpr double kEvidence = 64.0;     // alpha evidence ramp: n_val / (n_val + kEvidence)
constexpr uint32_t kEstimateJobs = 8;  // jobs used to choose the sampling model
constexpr double kMaeDecay = 0.97;     // per job, running held-out MAE
constexpr uint32_t kMargin = 3;        // low-res margin (window 5x5 and the generic 4x4 must be inside the tile)

constexpr char kMagic[8] = {'M', 'S', 'R', 'A', 'L', 'S', 'R', '1'};
constexpr uint32_t kVersion = 2;

inline float Smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float Sat(float x) { return std::clamp(x, 0.0f, 1.0f); }
inline float Luma3(const float c[3]) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

struct Lut {
    float toLin[256];
    float u8[256];
    Lut() {
        for (int i = 0; i < 256; ++i) {
            const double v = i / 255.0;
            toLin[i] = static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
            u8[i] = static_cast<float>(v);
        }
    }
};
const Lut& L() {
    static const Lut l;
    return l;
}
uint8_t EncodeSrgb8(double lin) {
    lin = std::clamp(lin, 0.0, 1.0);
    const double v = lin <= 0.0031308 ? lin * 12.92 : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

// Load texel (x, y) as gamma RGB 0..1 (clamped coordinates).
inline void Texel(const uint8_t* src, size_t pitch, bool bgra, int sw, int sh, int x, int y, float c[3]) {
    x = std::clamp(x, 0, sw - 1);
    y = std::clamp(y, 0, sh - 1);
    const uint8_t* p = src + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
    const float* u = L().u8;
    if (bgra) {
        c[0] = u[p[2]];
        c[1] = u[p[1]];
        c[2] = u[p[0]];
    } else {
        c[0] = u[p[0]];
        c[1] = u[p[1]];
        c[2] = u[p[2]];
    }
}

uint64_t Hash(uint64_t a) {
    a ^= a >> 33;
    a *= 0xff51afd7ed558ccdull;
    a ^= a >> 33;
    a *= 0xc4ceb9fe1a85ec53ull;
    a ^= a >> 33;
    return a;
}

void Prior(float w[kFeat]) {
    for (uint32_t k = 0; k < kFeat; ++k) w[k] = 0.0f;
    w[0] = 1.0f;
}

// Held-out SSE of filter w from the held-out normal equations.
template <class T> double Sse(const double* ata, const double* atb, double btb, const T* w) {
    double q = 0, l = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < kFeat; ++i) {
        for (uint32_t j = i; j < kFeat; ++j, ++k) q += (i == j ? 1.0 : 2.0) * ata[k] * w[i] * w[j];
        l += atb[i] * w[i];
    }
    return std::max(0.0, q - 2.0 * l + btb);
}

// Solves M x = r for symmetric positive definite M (n x n, row-major, overwritten). Returns false if not SPD.
bool Cholesky(double* m, double* r, uint32_t n) {
    for (uint32_t j = 0; j < n; ++j) {
        double d = m[j * n + j];
        for (uint32_t k = 0; k < j; ++k) d -= m[j * n + k] * m[j * n + k];
        if (!(d > 1e-300)) return false;
        d = std::sqrt(d);
        m[j * n + j] = d;
        for (uint32_t i = j + 1; i < n; ++i) {
            double s = m[i * n + j];
            for (uint32_t k = 0; k < j; ++k) s -= m[i * n + k] * m[j * n + k];
            m[i * n + j] = s / d;
        }
    }
    for (uint32_t i = 0; i < n; ++i) { // forward: L y = r
        double s = r[i];
        for (uint32_t k = 0; k < i; ++k) s -= m[i * n + k] * r[k];
        r[i] = s / m[i * n + i];
    }
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) { // back: L' x = y
        double s = r[i];
        for (uint32_t k = static_cast<uint32_t>(i) + 1; k < n; ++k) s -= m[k * n + i] * r[k];
        r[i] = s / m[i * n + i];
    }
    return true;
}

// Edge-step sharpness statistic of a luma plane: mean over strong 3-pixel runs (|c - a| > 0.1, horizontal and
// vertical) of max(|b - a|, |c - b|) / |c - a|. 1 = every edge is a single-pixel step (no anti-aliasing), 0.5 =
// transitions spread over two pixels.
void EdgeStat(const std::vector<float>& l, uint32_t w, uint32_t h, double& sum, double& n) {
    for (uint32_t y = 1; y + 1 < h; ++y)
        for (uint32_t x = 1; x + 1 < w; ++x) {
            const float b = l[static_cast<size_t>(y) * w + x];
            const float ax = l[static_cast<size_t>(y) * w + x - 1], cx = l[static_cast<size_t>(y) * w + x + 1];
            const float ay = l[static_cast<size_t>(y - 1) * w + x], cy = l[static_cast<size_t>(y + 1) * w + x];
            if (std::abs(cx - ax) > 0.1f && (b - ax) * (cx - b) >= 0.0f) {
                sum += std::max(std::abs(b - ax), std::abs(cx - b)) / std::abs(cx - ax);
                n += 1;
            }
            if (std::abs(cy - ay) > 0.1f && (b - ay) * (cy - b) >= 0.0f) {
                sum += std::max(std::abs(b - ay), std::abs(cy - b)) / std::abs(cy - ay);
                n += 1;
            }
        }
}

// Tile-level training geometry: target size (tw x th, multiple of the period), low size (lw x lh).
void PairSizes(const Axis& a, uint32_t w, uint32_t& tw, uint32_t& lw) {
    if (a.periodic) {
        tw = (w / a.q) * a.q;
        lw = static_cast<uint32_t>(std::lround(tw / a.s));
    } else {
        lw = static_cast<uint32_t>(std::floor(w / a.s));
        tw = w;
    }
}

void PutBe(std::vector<uint8_t>& v, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}
template <class T> void Put(std::vector<uint8_t>& v, const T& x) { PutBe(v, &x, sizeof(T)); }

struct Reader {
    const std::vector<uint8_t>& d;
    size_t pos = 0;
    bool ok = true;
    template <class T> T Get() {
        T x{};
        if (pos + sizeof(T) > d.size()) {
            ok = false;
            return x;
        }
        memcpy(&x, d.data() + pos, sizeof(T));
        pos += sizeof(T);
        return x;
    }
    void GetN(void* p, size_t n) {
        if (pos + n > d.size()) {
            ok = false;
            return;
        }
        memcpy(p, d.data() + pos, n);
        pos += n;
    }
};

uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

const wchar_t* ModelName(SamplingModel m) {
    switch (m) {
    case SamplingModel::Point: return L"point";
    case SamplingModel::Area: return L"area";
    default: return L"unknown";
    }
}

Axis MakeAxis(float s) {
    Axis a;
    a.s = s;
    for (uint32_t q = 1; q <= kMaxPeriod; ++q) {
        const double v = q / static_cast<double>(s);
        if (v >= 0.999 && std::abs(v - std::round(v)) < 1e-4) {
            a.q = q;
            a.periodic = true;
            return a;
        }
    }
    a.q = kPhaseBins;
    a.periodic = false;
    return a;
}

uint32_t GeoBucket(const float l[kTaps]) {
    static const float bw[3] = {0.25f, 0.5f, 0.25f};
    float jxx = 0, jxy = 0, jyy = 0;
    for (int j = 1; j <= 3; ++j)
        for (int i = 1; i <= 3; ++i) {
            const float gx = 0.5f * (l[j * kWin + i + 1] - l[j * kWin + i - 1]);
            const float gy = 0.5f * (l[(j + 1) * kWin + i] - l[(j - 1) * kWin + i]);
            const float w = bw[i - 1] * bw[j - 1];
            jxx += w * gx * gx;
            jxy += w * gx * gy;
            jyy += w * gy * gy;
        }
    const float tr = jxx + jyy, dxx = jxx - jyy;
    const float disc = std::sqrt(dxx * dxx + 4.0f * jxy * jxy);
    const float l1 = std::sqrt(std::max(0.0f, 0.5f * (tr + disc))), l2 = std::sqrt(std::max(0.0f, 0.5f * (tr - disc)));
    const float coh = (l1 - l2) / (l1 + l2 + 1e-9f);
    float ang = 0.5f * std::atan2(2.0f * jxy, dxx); // gradient direction, (-pi/2, pi/2]
    if (ang < 0.0f) ang += 3.14159265f;
    uint32_t a = static_cast<uint32_t>(ang * (kAngles / 3.14159265f));
    if (a >= kAngles) a = kAngles - 1;
    const uint32_t s = l1 < kStrengthT[0] ? 0u : (l1 < kStrengthT[1] ? 1u : 2u);
    const uint32_t c = coh < kCoherenceT[0] ? 0u : (coh < kCoherenceT[1] ? 1u : 2u);
    return (a * kStrengths + s) * kCoherences + c;
}

// ---- CPU port of moonsr_spatial_ps.hlsl (param1 = param2 = 0 -> defaults). Keep in sync with the shader. ----
void SpatialCpu(const uint8_t* src, size_t pitch, bool bgra, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                uint32_t dx, uint32_t dy, float rgb[3]) {
    const float u = (dx + 0.5f) / static_cast<float>(dw), v = (dy + 0.5f) / static_cast<float>(dh);
    const float fx = u * static_cast<float>(sw) - 0.5f, fy = v * static_cast<float>(sh) - 0.5f;
    const float bx = std::floor(fx), by = std::floor(fy);
    const float tx = fx - bx, ty = fy - by;
    float C[16][3];
    const int ibx = static_cast<int>(bx) - 1, iby = static_cast<int>(by) - 1;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) Texel(src, pitch, bgra, static_cast<int>(sw), static_cast<int>(sh), ibx + i, iby + j, C[j * 4 + i]);
    float Lm[16];
    for (int n = 0; n < 16; ++n) Lm[n] = Luma3(C[n]);

    const float wx[3] = {0.5f * (1.0f - tx) * (1.0f - tx), 0.75f - (tx - 0.5f) * (tx - 0.5f), 0.5f * tx * tx};
    const float wy[3] = {0.5f * (1.0f - ty) * (1.0f - ty), 0.75f - (ty - 0.5f) * (ty - 0.5f), 0.5f * ty * ty};
    float jxx = 0, jxy = 0, jyy = 0;
    for (int cy = 0; cy < 3; ++cy)
        for (int cx = 0; cx < 3; ++cx) {
            const float a = Lm[cy * 4 + cx], b = Lm[cy * 4 + cx + 1];
            const float c = Lm[(cy + 1) * 4 + cx], d = Lm[(cy + 1) * 4 + cx + 1];
            const float gx = 0.5f * ((b - a) + (d - c));
            const float gy = 0.5f * ((c - a) + (d - b));
            const float w = wx[cx] * wy[cy];
            jxx += w * gx * gx;
            jxy += w * gx * gy;
            jyy += w * gy * gy;
        }
    const float sigma = 0.015f;
    const float trace = jxx + jyy, dxx = jxx - jyy;
    const float disc = std::sqrt(dxx * dxx + 4.0f * jxy * jxy);
    const float coh = disc / (trace + 2.0f * sigma * sigma + 1e-12f);
    const float lam1 = 0.5f * (trace + disc);
    const float mag = std::min(static_cast<float>(dw) / sw, static_cast<float>(dh) / sh);
    const float A = Sat(Smooth(0.35f, 0.85f, coh) * Sat((mag - 1.0f) * 4.0f) * 1.0f);
    const float E = Smooth(0.02f, 0.12f, std::sqrt(lam1));
    const float gamma = 1.0f - E;

    auto KIso = [](float x) {
        const float x2 = x * x;
        float w = Sat(1.0f - 0.25f * x2);
        w *= w;
        return (1.0f - x2) * w * w;
    };
    auto Guard = [](float x) {
        const float a = 0.25f * x * x;
        return Sat(1.0f - a * a * a);
    };
    float hx[4], hy[4], gdx[4], gdy[4];
    for (int m = 0; m < 4; ++m) {
        const float ox = static_cast<float>(m) - 1.0f - tx, oy = static_cast<float>(m) - 1.0f - ty;
        hx[m] = KIso(ox);
        hy[m] = KIso(oy);
        gdx[m] = Guard(ox);
        gdy[m] = Guard(oy);
    }
    float cIso[3] = {0, 0, 0}, wIso = 0;
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) {
            const float w = hx[k] * hy[j];
            for (int ch = 0; ch < 3; ++ch) cIso[ch] += w * C[j * 4 + k][ch];
            wIso += w;
        }
    float color[3] = {cIso[0] / wIso, cIso[1] / wIso, cIso[2] / wIso};
    if (A > 0.0f) {
        const float rd = 1.0f / std::max(disc, 1e-10f);
        const float c2 = dxx * rd, s2 = 2.0f * jxy * rd;
        const float nx = std::sqrt(Sat(0.5f + 0.5f * c2));
        const float ny = (s2 < 0.0f ? -1.0f : 1.0f) * std::sqrt(Sat(0.5f - 0.5f * c2));
        const float kap = 1.0f + A * (std::abs(nx) + std::abs(ny) - 1.0f);
        const float invS = 1.0f / (1.0f + 0.5f * A);
        const float uax = nx * kap, uay = ny * kap, vax = -ny * invS, vay = nx * invS;
        float cAni[3] = {0, 0, 0}, wAni = 0;
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) {
                const float ddx = static_cast<float>(k) - 1.0f - tx, ddy = static_cast<float>(j) - 1.0f - ty;
                const float uu = ddx * uax + ddy * uay, vv = ddx * vax + ddy * vay;
                const float p2 = uu * uu + vv * vv;
                const float wp = Sat(1.0f - 0.25f * p2);
                const float w = (1.0f - p2) * wp * wp * wp * (1.0f - gamma * 0.25f * p2) * gdx[k] * gdy[j];
                for (int ch = 0; ch < 3; ++ch) cAni[ch] += w * C[j * 4 + k][ch];
                wAni += w;
            }
        const float iw = 1.0f / std::max(wAni, 1e-4f);
        for (int ch = 0; ch < 3; ++ch) color[ch] = color[ch] + (cAni[ch] * iw - color[ch]) * A;
    }
    float tx4[4], ty4[4];
    for (int q = 0; q < 4; ++q) {
        tx4[q] = Sat(1.5f - std::abs(static_cast<float>(q) - 1.0f - tx));
        ty4[q] = Sat(1.5f - std::abs(static_cast<float>(q) - 1.0f - ty));
    }
    for (int ch = 0; ch < 3; ++ch) {
        const float top = C[5][ch] + (C[6][ch] - C[5][ch]) * tx, bot = C[9][ch] + (C[10][ch] - C[9][ch]) * tx;
        const float bil = top + (bot - top) * ty;
        float lo = bil, hi = bil;
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) {
                const float mb = bil + (C[j * 4 + k][ch] - bil) * (tx4[k] * ty4[j]);
                lo = std::min(lo, mb);
                hi = std::max(hi, mb);
            }
        const float c = color[ch];
        const float ov = 0.1f * (hi - lo) + 1e-6f;
        const float eh = std::max(c - hi, 0.0f), el = std::max(lo - c, 0.0f);
        rgb[ch] = std::clamp(c, lo, hi) + eh * ov / (ov + eh) - el * ov / (ov + el);
    }
}

// ---------------- Learner ----------------

Learner::Learner() {
    job_.px.assign(static_cast<size_t>(kTile) * kTile * 4, 0);
    lastSave_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this] { Worker(); });
}

Learner::~Learner() {
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(learnMutex_);
    if (!path_.empty() && dirty_) SaveLocked();
}

void Learner::InitSection(Section& s, float sx, float sy) {
    s = Section{};
    s.sx = sx;
    s.sy = sy;
    s.ax = MakeAxis(sx);
    s.ay = MakeAxis(sy);
    s.b.resize(static_cast<size_t>(s.ax.q) * s.ay.q * kGeo);
    for (auto& b : s.b) {
        memset(&b.tr, 0, sizeof(Eq));
        memset(&b.va, 0, sizeof(Eq));
        memset(&b.lo, 0, sizeof(Eq));
        b.transfer = 0.0f;
        Prior(b.w);
        b.alpha = 0.0f;
    }
}

bool Learner::SetProfilePath(const std::wstring& path) {
    WaitIdle();
    std::lock_guard<std::mutex> lk(learnMutex_);
    if (!path_.empty() && dirty_) SaveLocked();
    path_ = path;
    // A new game: forget the in-memory state of the previous one.
    const float sx = active_ >= 0 ? sections_[active_].sx : 0.0f, sy = active_ >= 0 ? sections_[active_].sy : 0.0f;
    sections_.clear();
    active_ = -1;
    loaded_ = false;
    dirty_ = false;
    maeN_ = maeG_ = maeL_ = 0;
    if (!path_.empty()) loaded_ = LoadLocked();
    if (sx > 0.0f) {
        configured_.store(false, std::memory_order_release);
        // re-select the scale section under the new profile
        int found = -1;
        for (size_t i = 0; i < sections_.size(); ++i)
            if (std::abs(sections_[i].sx - sx) < 1e-3f && std::abs(sections_[i].sy - sy) < 1e-3f) found = static_cast<int>(i);
        if (found < 0) {
            sections_.emplace_back();
            InitSection(sections_.back(), sx, sy);
            found = static_cast<int>(sections_.size()) - 1;
        }
        active_ = found;
        Publish(sections_[active_]);
        UpdateStatusLocked(sections_[active_]);
        configured_.store(true, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> sl(statusMutex_);
        status_.profileLoaded = loaded_;
    }
    return loaded_;
}

void Learner::Configure(float sx, float sy) {
    if (configured_.load(std::memory_order_acquire) && active_ >= 0 && std::abs(sections_[active_].sx - sx) < 1e-3f &&
        std::abs(sections_[active_].sy - sy) < 1e-3f)
        return;
    WaitIdle();
    std::lock_guard<std::mutex> lk(learnMutex_);
    int found = -1;
    for (size_t i = 0; i < sections_.size(); ++i)
        if (std::abs(sections_[i].sx - sx) < 1e-3f && std::abs(sections_[i].sy - sy) < 1e-3f) found = static_cast<int>(i);
    if (found < 0) {
        sections_.emplace_back();
        InitSection(sections_.back(), sx, sy);
        if (forceModel_ != SamplingModel::Unknown) {
            sections_.back().model = forceModel_;
            sections_.back().forced = true;
        }
        found = static_cast<int>(sections_.size()) - 1;
    }
    active_ = found;
    ax_ = sections_[active_].ax;
    ay_ = sections_[active_].ay;
    maeN_ = maeG_ = maeL_ = 0;
    Publish(sections_[active_]);
    UpdateStatusLocked(sections_[active_]);
    configured_.store(true, std::memory_order_release);
    MSR_LOG_INFO(L"ALSR learning: scale {:.4f} x {:.4f}, phases {} x {} ({}), {} buckets, model {}", sx, sy, ax_.q, ay_.q,
                 ax_.periodic ? L"exact period" : L"quantised", BucketCount(), ModelName(sections_[active_].model));
}

void Learner::Submit() {
    if (!configured_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        busy_.store(true, std::memory_order_release);
        jobReady_ = true;
    }
    cv_.notify_one();
}

void Learner::WaitIdle() {
    std::unique_lock<std::mutex> lk(jobMutex_);
    idleCv_.wait(lk, [this] { return !busy_.load(std::memory_order_acquire); });
}

void Learner::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    SetThreadDescription(GetCurrentThread(), L"ALSR learner");
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(jobMutex_);
            cv_.wait(lk, [this] { return jobReady_ || stop_; });
            if (stop_ && !jobReady_) return;
            jobReady_ = false;
        }
        {
            std::lock_guard<std::mutex> lk(learnMutex_);
            Section* s = Active();
            if (s) {
                const auto t0 = std::chrono::steady_clock::now();
                Process(job_, *s);
                if (++jobsSinceSolve_ >= solveEvery_) {
                    jobsSinceSolve_ = 0;
                    Solve(*s);
                    Publish(*s);
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                workerMsSum_ += ms;
                ++jobs_;
                UpdateStatusLocked(*s);
                if (!path_.empty() && dirty_ && autosaveSec_ > 0 &&
                    std::chrono::steady_clock::now() - lastSave_ > std::chrono::seconds(autosaveSec_))
                    SaveLocked();
            }
        }
        {
            std::lock_guard<std::mutex> lk(jobMutex_);
            busy_.store(false, std::memory_order_release);
        }
        idleCv_.notify_all();
    }
}

namespace {

// Point model: low pixel i takes the value of ONE F pixel near its centre c = (i + 0.5) * r - 0.5 (F index
// coordinate). Nearest-pixel selection is biased (2x: every centre lies between four F pixels, so the whole low grid
// moves by half an F pixel; 1.5x: alternating -/+ 0.25 F px by phase) and the learner learns that shift as part of
// the filter - measured: -9 to -11 dB against true ground truth at 2x. Stochastic rounding per column and per row
// (floor(c) or floor(c) + 1 with probability frac(c), hashed) is unbiased in expectation, keeps straight edges
// straight and keeps every low value a single F value (hard, un-anti-aliased steps).
void PointIndices(uint32_t n, uint32_t full, float r, uint64_t seed, std::vector<uint32_t>& idx) {
    idx.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float c = (i + 0.5f) * r - 0.5f;
        const float b = std::floor(c);
        const double u = static_cast<double>(Hash(seed * 0x9E3779B97F4A7C15ull + i) >> 11) * (1.0 / 9007199254740992.0);
        const int k = static_cast<int>(b) + (u < static_cast<double>(c - b) ? 1 : 0);
        idx[i] = static_cast<uint32_t>(std::clamp(k, 0, static_cast<int>(full) - 1));
    }
}

// Reduces src (tw x th used) to lw x lh with the sampling model; 4 bytes per pixel, channel order preserved.
void Reduce(const uint8_t* src, size_t pitch, uint32_t tw, uint32_t th, uint32_t lw, uint32_t lh, SamplingModel m,
            uint64_t seed, std::vector<uint8_t>& out) {
    const Lut& lut = L();
    const float rx = static_cast<float>(tw) / lw, ry = static_cast<float>(th) / lh;
    out.resize(static_cast<size_t>(lw) * lh * 4);
    thread_local std::vector<uint32_t> ix, iy;
    if (m == SamplingModel::Point) {
        PointIndices(lw, tw, rx, seed * 2 + 1, ix);
        PointIndices(lh, th, ry, seed * 2 + 2, iy);
    }
    for (uint32_t y = 0; y < lh; ++y)
        for (uint32_t x = 0; x < lw; ++x) {
            uint8_t* o = &out[(static_cast<size_t>(y) * lw + x) * 4];
            if (m == SamplingModel::Point) {
                memcpy(o, src + iy[y] * pitch + ix[x] * 4, 4);
                continue;
            }
            // exact box [x*rx, (x+1)*rx) x [y*ry, (y+1)*ry), fractional coverage, linear light
            const float x0 = x * rx, x1 = (x + 1) * rx, y0 = y * ry, y1 = (y + 1) * ry;
            double acc[3] = {0, 0, 0}, wsum = 0;
            for (uint32_t yy = static_cast<uint32_t>(y0); yy < std::min(th, static_cast<uint32_t>(std::ceil(y1))); ++yy) {
                const float wy = std::min(y1, yy + 1.0f) - std::max(y0, static_cast<float>(yy));
                if (wy <= 0) continue;
                for (uint32_t xx = static_cast<uint32_t>(x0); xx < std::min(tw, static_cast<uint32_t>(std::ceil(x1))); ++xx) {
                    const float wx = std::min(x1, xx + 1.0f) - std::max(x0, static_cast<float>(xx));
                    if (wx <= 0) continue;
                    const uint8_t* p = src + yy * pitch + xx * 4;
                    const double w = static_cast<double>(wx) * wy;
                    for (int c = 0; c < 3; ++c) acc[c] += w * lut.toLin[p[c]];
                    wsum += w;
                }
            }
            for (int c = 0; c < 3; ++c) o[c] = EncodeSrgb8(acc[c] / wsum);
            o[3] = 255;
        }
}

void LumaPlane(const uint8_t* src, size_t pitch, bool bgra, uint32_t w, uint32_t h, std::vector<float>& out) {
    const Lut& lut = L();
    out.resize(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = src + y * pitch + x * 4;
            const float c[3] = {lut.u8[bgra ? p[2] : p[0]], lut.u8[p[1]], lut.u8[bgra ? p[0] : p[2]]};
            out[static_cast<size_t>(y) * w + x] = Luma3(c);
        }
}

struct PairScratch {
    std::vector<float> tl;
    std::vector<uint8_t> low;
    uint32_t lw = 0, lh = 0;
};

// Builds the training pair of `src` (w x h, the truth) and calls fn(bucket, f[kFeat], target, tx, ty) for every target
// pixel whose windows lie inside the reduced image. Leaves the reduced image in sc.low (sc.lw x sc.lh).
template <class Fn>
void ForEachSample(const Axis& ax, const Axis& ay, const uint8_t* src, size_t pitch, bool bgra, uint32_t w, uint32_t h,
                   SamplingModel model, uint64_t seed, PairScratch& sc, Fn&& fn) {
    uint32_t tw, th, lw, lh;
    PairSizes(ax, w, tw, lw);
    PairSizes(ay, h, th, lh);
    sc.lw = sc.lh = 0;
    if (lw < 2 * kMargin + 2 || lh < 2 * kMargin + 2) return;
    LumaPlane(src, pitch, bgra, tw, th, sc.tl);
    Reduce(src, pitch, tw, th, lw, lh, model, seed, sc.low);
    sc.lw = lw;
    sc.lh = lh;
    const Lut& lut = L();
    const float rx = static_cast<float>(tw) / lw, ry = static_cast<float>(th) / lh;
    const uint8_t* low = sc.low.data();
    for (uint32_t ty = 0; ty < th; ++ty) {
        const float fy = (ty + 0.5f) / ry - 0.5f;
        const int cy = CentreOf(fy);
        if (cy < static_cast<int>(kMargin) || cy + static_cast<int>(kMargin) >= static_cast<int>(lh)) continue;
        const uint32_t py = PhaseOf(ay, ty, fy);
        for (uint32_t tx = 0; tx < tw; ++tx) {
            const float fx = (tx + 0.5f) / rx - 0.5f;
            const int cx = CentreOf(fx);
            if (cx < static_cast<int>(kMargin) || cx + static_cast<int>(kMargin) >= static_cast<int>(lw)) continue;
            float f[kFeat];
            for (int wy = 0; wy < static_cast<int>(kWin); ++wy)
                for (int wx = 0; wx < static_cast<int>(kWin); ++wx) {
                    const uint8_t* p = &low[(static_cast<size_t>(cy + wy - 2) * lw + (cx + wx - 2)) * 4];
                    const float c[3] = {lut.u8[bgra ? p[2] : p[0]], lut.u8[p[1]], lut.u8[bgra ? p[0] : p[2]]};
                    f[1 + wy * kWin + wx] = Luma3(c);
                }
            float g[3];
            SpatialCpu(low, static_cast<size_t>(lw) * 4, bgra, lw, lh, tw, th, tx, ty, g);
            for (float& v : g) v = std::round(Sat(v) * 255.0f) / 255.0f; // the GPU writes it to RGBA8
            f[0] = Luma3(g);
            const uint32_t bucket = (py * ax.q + PhaseOf(ax, tx, fx)) * kGeo + GeoBucket(f + 1);
            fn(bucket, f, static_cast<double>(sc.tl[static_cast<size_t>(ty) * tw + tx]), tx, ty);
        }
    }
}

inline void AddSample(double* ata, double* atb, double& btb, uint64_t& n, const float* f, double b) {
    uint32_t k = 0;
    for (uint32_t a = 0; a < kFeat; ++a) {
        const double fa = f[a];
        for (uint32_t c = a; c < kFeat; ++c, ++k) ata[k] += fa * f[c];
        atb[a] += fa * b;
    }
    btb += b * b;
    ++n;
}

inline double Predict(const float* w, const float* f) {
    double p = 0;
    for (uint32_t a = 0; a < kFeat; ++a) p += static_cast<double>(w[a]) * f[a];
    return std::clamp(p, 0.0, 1.0);
}

// Ridge solve towards the prior. Returns false (w = prior) when there is too little data or the system is singular.
bool SolveEq(const double* ata, const double* atb, uint64_t count, float w[kFeat]) {
    float prior[kFeat];
    Prior(prior);
    Prior(w);
    if (count < kMinTrain) return false;
    double m[kFeat * kFeat], r[kFeat], trace = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < kFeat; ++i)
        for (uint32_t j = i; j < kFeat; ++j, ++k) {
            m[i * kFeat + j] = m[j * kFeat + i] = ata[k];
            if (i == j) trace += ata[k];
        }
    const double n = static_cast<double>(count);
    const double mu = (kVirtual + 1e-3 * n) * trace / (kFeat * n) + 1e-12;
    for (uint32_t i = 0; i < kFeat; ++i) {
        m[i * kFeat + i] += mu;
        r[i] = atb[i] + mu * prior[i];
    }
    if (!Cholesky(m, r, kFeat)) return false;
    for (uint32_t i = 0; i < kFeat; ++i)
        if (!std::isfinite(r[i]) || std::abs(r[i]) > 1e3) return false;
    for (uint32_t i = 0; i < kFeat; ++i) w[i] = static_cast<float>(r[i]);
    return true;
}

inline double Ramp(double imp) { return std::clamp((imp - kGateMargin) / (2.0 * kGateMargin), 0.0, 1.0); }

} // namespace

void Learner::Process(const Job& j, Section& s) {
    thread_local PairScratch sc0, sc1;
    thread_local std::vector<float> pl, al;
    thread_local std::vector<uint8_t> tmp;

    // Sampling-model estimation (first kEstimateJobs jobs of a section unless forced); these jobs are not trained on.
    if (!s.forced && s.model == SamplingModel::Unknown) {
        uint32_t tw, th, lw, lh;
        PairSizes(s.ax, j.w, tw, lw);
        PairSizes(s.ay, j.h, th, lh);
        if (lw < 8 || lh < 8) return;
        double fs = 0, fn = 0, ps = 0, pn = 0, as = 0, an = 0;
        LumaPlane(j.px.data(), j.pitch, j.bgra, tw, th, sc0.tl);
        EdgeStat(sc0.tl, tw, th, fs, fn);
        Reduce(j.px.data(), j.pitch, tw, th, lw, lh, SamplingModel::Point, j.frame, tmp);
        LumaPlane(tmp.data(), static_cast<size_t>(lw) * 4, j.bgra, lw, lh, pl);
        EdgeStat(pl, lw, lh, ps, pn);
        Reduce(j.px.data(), j.pitch, tw, th, lw, lh, SamplingModel::Area, j.frame, tmp);
        LumaPlane(tmp.data(), static_cast<size_t>(lw) * 4, j.bgra, lw, lh, al);
        EdgeStat(al, lw, lh, as, an);
        if (fn > 50 && pn > 20 && an > 20) {
            s.estF += fs / fn;
            s.estP += ps / pn;
            s.estA += as / an;
            ++s.estJobs;
        }
        if (s.estJobs >= kEstimateJobs) {
            const double f = s.estF / s.estJobs, p = s.estP / s.estJobs, a = s.estA / s.estJobs;
            s.model = std::abs(p - f) <= std::abs(a - f) ? SamplingModel::Point : SamplingModel::Area;
            MSR_LOG_INFO(L"ALSR learning: sampling model {} (edge-step sharpness: frame {:.3f}, point {:.3f}, area {:.3f}, {} jobs)",
                         ModelName(s.model), f, p, a, s.estJobs);
            dirty_ = true;
        }
        return;
    }
    SamplingModel model = s.model;
    if (model == SamplingModel::Unknown) model = SamplingModel::Point;
    // filters_ is written only under learnMutex_ (which the caller holds), so reading it here needs no filterMutex_.
    const bool havePub = filters_.size() == s.b.size() * kFilterStride;

    // Level 0: pairs (F reduced -> F), training / held-out.
    double mN = 0, mG = 0, mL = 0;
    ForEachSample(s.ax, s.ay, j.px.data(), j.pitch, j.bgra, j.w, j.h, model, j.frame, sc0,
                  [&](uint32_t bucket, const float* f, double b, uint32_t tx, uint32_t ty) {
                      Bucket& bk = s.b[bucket];
                      const bool val = (Hash(j.frame * 0x9E3779B97F4A7C15ull + (static_cast<uint64_t>(ty) << 20) + tx) & 7u) == 0;
                      Eq& e = val ? bk.va : bk.tr;
                      AddSample(e.ata, e.atb, e.btb, e.n, f, b);
                      if (val) {
                          const double pred = havePub ? Predict(&filters_[static_cast<size_t>(bucket) * kFilterStride], f) : f[0];
                          mN += 1;
                          mG += std::abs(f[0] - b);
                          mL += std::abs(pred - b);
                      }
                  });
    // Level -1: pairs (F reduced twice -> F reduced) for the cross-scale transfer gate.
    if (sc0.lw > 0) {
        ForEachSample(s.ax, s.ay, sc0.low.data(), static_cast<size_t>(sc0.lw) * 4, j.bgra, sc0.lw, sc0.lh, model, ~j.frame, sc1,
                      [&](uint32_t bucket, const float* f, double b, uint32_t, uint32_t) {
                          Eq& e = s.b[bucket].lo;
                          AddSample(e.ata, e.atb, e.btb, e.n, f, b);
                      });
    }
    maeN_ = maeN_ * kMaeDecay + mN;
    maeG_ = maeG_ * kMaeDecay + mG;
    maeL_ = maeL_ * kMaeDecay + mL;
    dirty_ = true;
}

void Learner::Solve(Section& s) {
    float prior[kFeat], wlo[kFeat];
    Prior(prior);
    for (auto& b : s.b) {
        b.alpha = 0.0f;
        b.transfer = 0.0f;
        if (!SolveEq(b.tr.ata, b.tr.atb, b.tr.n, b.w)) continue;
        if (b.va.n < kMinVal) continue;
        // 1. self-supervised gate: held-out samples of the same level
        const double sg = Sse(b.va.ata, b.va.atb, b.va.btb, prior);
        const double sl = Sse(b.va.ata, b.va.atb, b.va.btb, b.w);
        if (!(sg > 0.0) || !(sl < (1.0 - kGateMargin) * sg)) continue;
        const double nv = static_cast<double>(b.va.n);
        const double aSelf = nv / (nv + kEvidence) * Ramp(1.0 - sl / sg);
        // 2. cross-scale transfer gate: the filter learned one level lower, evaluated on ALL level-0 samples (which it
        //    never saw). It mirrors the real use (learned at F/s -> F, applied at F -> F*s).
        double aT = 1.0;
        if (transferGate_) {
            aT = 0.0;
            if (SolveEq(b.lo.ata, b.lo.atb, b.lo.n, wlo)) {
                const double g0 = Sse(b.tr.ata, b.tr.atb, b.tr.btb, prior) + sg;
                const double l0 = Sse(b.tr.ata, b.tr.atb, b.tr.btb, wlo) + Sse(b.va.ata, b.va.atb, b.va.btb, wlo);
                if (g0 > 0.0) {
                    b.transfer = static_cast<float>(1.0 - l0 / g0);
                    aT = Ramp(1.0 - l0 / g0);
                }
            }
        }
        b.alpha = static_cast<float>(aSelf * aT);
    }
    ++s.solves;
    dirty_ = true;
}

void Learner::Publish(const Section& s) {
    std::lock_guard<std::mutex> lk(filterMutex_);
    filters_.assign(s.b.size() * kFilterStride, 0.0f);
    float prior[kFeat];
    Prior(prior);
    for (size_t i = 0; i < s.b.size(); ++i) {
        const Bucket& b = s.b[i];
        float* d = &filters_[i * kFilterStride];
        for (uint32_t k = 0; k < kFeat; ++k) d[k] = b.alpha * b.w[k] + (1.0f - b.alpha) * prior[k];
        d[kFeat] = b.alpha;
    }
    ++filtersVersion_;
}

bool Learner::TakeFilters(std::vector<float>& dst) {
    std::unique_lock<std::mutex> lk(filterMutex_, std::try_to_lock);
    if (!lk.owns_lock() || filtersVersion_ == takenVersion_ || dst.size() != filters_.size()) return false;
    memcpy(dst.data(), filters_.data(), filters_.size() * sizeof(float));
    takenVersion_ = filtersVersion_;
    return true;
}

void Learner::UpdateStatusLocked(const Section& s) {
    Status st;
    st.profileLoaded = loaded_;
    st.configured = true;
    st.solves = s.solves;
    st.jobs = jobs_;
    st.model = s.model;
    double sg = 0, sp = 0, nv = 0;
    uint32_t active = 0;
    float prior[kFeat];
    Prior(prior);
    for (const auto& b : s.b) {
        st.samples += b.tr.n + b.va.n;
        if (b.alpha > 0.0f) ++active;
        if (b.va.n == 0) continue;
        float pub[kFeat];
        for (uint32_t k = 0; k < kFeat; ++k) pub[k] = b.alpha * b.w[k] + (1.0f - b.alpha) * prior[k];
        sg += Sse(b.va.ata, b.va.atb, b.va.btb, prior);
        sp += Sse(b.va.ata, b.va.atb, b.va.btb, pub);
        nv += static_cast<double>(b.va.n);
    }
    st.coverage = s.b.empty() ? 0.0f : static_cast<float>(active) / s.b.size();
    if (nv > 0) {
        st.genericRmse = std::sqrt(sg / nv) * 255.0;
        st.learnedRmse = std::sqrt(sp / nv) * 255.0;
    }
    if (maeN_ > 0) {
        st.genericError = static_cast<float>(maeG_ / maeN_ * 255.0);
        st.learnedError = static_cast<float>(maeL_ / maeN_ * 255.0);
    }
    st.workerMs = jobs_ ? workerMsSum_ / jobs_ : 0.0;
    st.profileBytes = profileBytes_;
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_ = st;
}

Status Learner::GetStatus() const {
    std::lock_guard<std::mutex> lk(statusMutex_);
    return status_;
}

// ---- persistence ----
// Layout (little endian): magic[8] "MSRALSR1", u32 version, u32 sections, per section: f32 sx, f32 sy, u32 qx, u32 qy,
// u32 periodic bits, u32 kGeo, u32 kFeat, u32 model, u32 forced, u32 solves, f64 estF, estP, estA, u32 estJobs,
// u32 buckets, per bucket: 3 x {f64 ata[kTri], f64 atb[kFeat], f64 btb, u64 n} (training, held-out, level -1),
// f32 w[kFeat], f32 alpha, f32 transfer; then u64 FNV-1a of everything before it.
std::vector<uint8_t> Learner::Serialize() const {
    std::vector<uint8_t> v;
    size_t need = 64;
    for (const auto& s : sections_) need += 96 + s.b.size() * (3 * sizeof(Eq) + (kFeat + 2) * 4);
    v.reserve(need);
    PutBe(v, kMagic, 8);
    Put(v, kVersion);
    Put(v, static_cast<uint32_t>(sections_.size()));
    for (const auto& s : sections_) {
        Put(v, s.sx);
        Put(v, s.sy);
        Put(v, s.ax.q);
        Put(v, s.ay.q);
        Put(v, (s.ax.periodic ? 1u : 0u) | (s.ay.periodic ? 2u : 0u));
        Put(v, kGeo);
        Put(v, kFeat);
        Put(v, static_cast<uint32_t>(s.model));
        Put(v, s.forced ? 1u : 0u);
        Put(v, s.solves);
        Put(v, s.estF);
        Put(v, s.estP);
        Put(v, s.estA);
        Put(v, s.estJobs);
        Put(v, static_cast<uint32_t>(s.b.size()));
        for (const auto& b : s.b) {
            for (const Eq* e : {&b.tr, &b.va, &b.lo}) {
                PutBe(v, e->ata, sizeof(e->ata));
                PutBe(v, e->atb, sizeof(e->atb));
                Put(v, e->btb);
                Put(v, e->n);
            }
            PutBe(v, b.w, sizeof(b.w));
            Put(v, b.alpha);
            Put(v, b.transfer);
        }
    }
    Put(v, Fnv(v.data(), v.size()));
    return v;
}

bool Learner::Deserialize(const std::vector<uint8_t>& data, std::wstring& why) {
    if (data.size() < 24) {
        why = L"file too short";
        return false;
    }
    uint64_t sum;
    memcpy(&sum, data.data() + data.size() - 8, 8);
    if (sum != Fnv(data.data(), data.size() - 8)) {
        why = L"checksum mismatch";
        return false;
    }
    Reader r{data};
    char magic[8];
    r.GetN(magic, 8);
    if (memcmp(magic, kMagic, 8) != 0) {
        why = L"not an ALSR profile";
        return false;
    }
    const uint32_t ver = r.Get<uint32_t>();
    if (ver != kVersion) {
        why = L"unsupported version " + std::to_wstring(ver);
        return false;
    }
    const uint32_t n = r.Get<uint32_t>();
    if (n > 64) {
        why = L"too many sections";
        return false;
    }
    std::vector<Section> secs(n);
    for (auto& s : secs) {
        const float sx = r.Get<float>(), sy = r.Get<float>();
        const uint32_t qx = r.Get<uint32_t>(), qy = r.Get<uint32_t>(), per = r.Get<uint32_t>();
        const uint32_t geo = r.Get<uint32_t>(), feat = r.Get<uint32_t>();
        if (!r.ok || !(sx > 0.1f && sx < 16.0f) || !(sy > 0.1f && sy < 16.0f)) {
            why = L"bad scale";
            return false;
        }
        InitSection(s, sx, sy);
        if (geo != kGeo || feat != kFeat || qx != s.ax.q || qy != s.ay.q || per != ((s.ax.periodic ? 1u : 0u) | (s.ay.periodic ? 2u : 0u))) {
            why = L"incompatible bucket layout";
            return false;
        }
        const uint32_t model = r.Get<uint32_t>();
        s.model = model <= 2 ? static_cast<SamplingModel>(model) : SamplingModel::Unknown;
        s.forced = r.Get<uint32_t>() != 0;
        s.solves = r.Get<uint32_t>();
        s.estF = r.Get<double>();
        s.estP = r.Get<double>();
        s.estA = r.Get<double>();
        s.estJobs = r.Get<uint32_t>();
        const uint32_t nb = r.Get<uint32_t>();
        if (!r.ok || nb != s.b.size()) {
            why = L"bucket count mismatch";
            return false;
        }
        for (auto& b : s.b) {
            for (Eq* e : {&b.tr, &b.va, &b.lo}) {
                r.GetN(e->ata, sizeof(e->ata));
                r.GetN(e->atb, sizeof(e->atb));
                e->btb = r.Get<double>();
                e->n = r.Get<uint64_t>();
            }
            r.GetN(b.w, sizeof(b.w));
            b.alpha = r.Get<float>();
            b.transfer = r.Get<float>();
            bool fin = std::isfinite(b.alpha) && b.alpha >= 0.0f && b.alpha <= 1.0f && std::isfinite(b.tr.btb) && std::isfinite(b.va.btb);
            for (float w : b.w) fin &= std::isfinite(w);
            if (!fin) {
                why = L"non-finite values";
                return false;
            }
        }
    }
    if (!r.ok || r.pos != data.size() - 8) {
        why = L"size mismatch";
        return false;
    }
    sections_ = std::move(secs);
    return true;
}

bool Learner::SaveLocked() {
    if (path_.empty()) return true;
    const std::vector<uint8_t> v = Serialize();
    const std::wstring tmp = path_ + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        MSR_LOG_WARN(L"ALSR learning: cannot write {} (error {})", tmp, GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, v.data(), static_cast<DWORD>(v.size()), &written, nullptr) && written == v.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        MSR_LOG_WARN(L"ALSR learning: saving {} failed (error {})", path_, GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    dirty_ = false;
    profileBytes_ = v.size();
    lastSave_ = std::chrono::steady_clock::now();
    return true;
}

bool Learner::LoadLocked() {
    HANDLE f = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        MSR_LOG_INFO(L"ALSR learning: no profile at {} - starting fresh", path_);
        return false;
    }
    LARGE_INTEGER size{};
    std::vector<uint8_t> v;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20);
    if (ok) {
        v.resize(static_cast<size_t>(size.QuadPart));
        DWORD rd = 0;
        ok = ReadFile(f, v.data(), static_cast<DWORD>(v.size()), &rd, nullptr) && rd == v.size();
    }
    CloseHandle(f);
    std::wstring why = L"read error";
    if (!ok || !Deserialize(v, why)) {
        sections_.clear();
        MSR_LOG_WARN(L"ALSR learning: profile {} ignored ({}) - starting fresh", path_, why);
        return false;
    }
    profileBytes_ = v.size();
    MSR_LOG_INFO(L"ALSR learning: profile {} loaded ({} sections, {} bytes)", path_, sections_.size(), v.size());
    return true;
}

bool Learner::Save() {
    WaitIdle();
    std::lock_guard<std::mutex> lk(learnMutex_);
    const bool ok = SaveLocked();
    if (Section* s = Active()) UpdateStatusLocked(*s);
    return ok;
}

void Learner::Reset() {
    WaitIdle();
    std::lock_guard<std::mutex> lk(learnMutex_);
    for (auto& s : sections_) {
        const float sx = s.sx, sy = s.sy;
        InitSection(s, sx, sy);
        if (forceModel_ != SamplingModel::Unknown) {
            s.model = forceModel_;
            s.forced = true;
        }
    }
    maeN_ = maeG_ = maeL_ = 0;
    loaded_ = false;
    dirty_ = false;
    profileBytes_ = 0;
    if (!path_.empty()) DeleteFileW(path_.c_str());
    if (Section* s = Active()) {
        Publish(*s);
        UpdateStatusLocked(*s);
    }
}

void ApplyCpuRow(const std::vector<float>& table, const Axis& ax, const Axis& ay, const uint8_t* src, size_t pitch, bool bgra,
                 uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint32_t dy, uint8_t* genericRow, uint8_t* learnedRow,
                 uint32_t* bucketRow) {
    const float fy = (dy + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
    const int cy = CentreOf(fy);
    const uint32_t py = PhaseOf(ay, dy, fy);
    const bool haveTable = table.size() == static_cast<size_t>(ax.q) * ay.q * kGeo * kFilterStride;
    for (uint32_t dx = 0; dx < dw; ++dx) {
        float g[3];
        SpatialCpu(src, pitch, bgra, sw, sh, dw, dh, dx, dy, g);
        for (float& v : g) v = std::round(Sat(v) * 255.0f) / 255.0f;
        const float fx = (dx + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
        const int cx = CentreOf(fx);
        float S[kTaps][3], l[kTaps];
        for (int wy = 0; wy < static_cast<int>(kWin); ++wy)
            for (int wx = 0; wx < static_cast<int>(kWin); ++wx) {
                const int k = wy * kWin + wx;
                Texel(src, pitch, bgra, static_cast<int>(sw), static_cast<int>(sh), cx + wx - 2, cy + wy - 2, S[k]);
                l[k] = Luma3(S[k]);
            }
        float out[3] = {g[0], g[1], g[2]};
        const uint32_t bucket = (py * ax.q + PhaseOf(ax, dx, fx)) * kGeo + GeoBucket(l);
        if (bucketRow) bucketRow[dx] = bucket;
        if (haveTable) {
            const float* w = &table[static_cast<size_t>(bucket) * kFilterStride];
            for (int c = 0; c < 3; ++c) {
                float acc = w[0] * g[c];
                for (uint32_t k = 0; k < kTaps; ++k) acc += w[1 + k] * S[k][c];
                out[c] = acc;
            }
        }
        uint8_t* go = genericRow + static_cast<size_t>(dx) * 4;
        uint8_t* lo = learnedRow + static_cast<size_t>(dx) * 4;
        const int r = bgra ? 2 : 0, b = bgra ? 0 : 2;
        go[r] = static_cast<uint8_t>(std::lround(g[0] * 255.0f));
        go[1] = static_cast<uint8_t>(std::lround(g[1] * 255.0f));
        go[b] = static_cast<uint8_t>(std::lround(g[2] * 255.0f));
        go[3] = 255;
        lo[r] = static_cast<uint8_t>(std::lround(Sat(out[0]) * 255.0f));
        lo[1] = static_cast<uint8_t>(std::lround(Sat(out[1]) * 255.0f));
        lo[b] = static_cast<uint8_t>(std::lround(Sat(out[2]) * 255.0f));
        lo[3] = 255;
    }
}

} // namespace moonsr::alsr
