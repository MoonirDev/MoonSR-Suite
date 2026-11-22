#include "framegen/adaptive_sampler.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <vector>
#include <wrl/client.h>

namespace moonsr::fgadaptive {
std::pair<uint32_t, uint32_t> SampleTileOffset(uint32_t ordinal, uint32_t spanX, uint32_t spanY) {
    if (ordinal < 9) return {spanX * (ordinal % 3) / 2, spanY * (ordinal / 3) / 2};
    const auto radicalInverse = [](uint32_t n, uint32_t base) {
        double value = 0.0, factor = 1.0 / base;
        while (n) { value += (n % base) * factor; n /= base; factor /= base; }
        return value;
    };
    const uint32_t n = ordinal - 8;
    return {static_cast<uint32_t>(radicalInverse(n, 2) * spanX),
            static_cast<uint32_t>(radicalInverse(n, 3) * spanY)};
}

namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kTile = 160;
constexpr uint32_t kMargin = 12;

struct CpuJob {
    std::array<std::vector<uint8_t>, 5> pixels; // original A,B,C; baseline and candidate interpolation
    uint32_t width = 0, height = 0;
    double baselineMs = 0, candidateMs = 0;
    uint64_t id = 0;
    bool candidateFirst = false;
};

std::optional<Observation> Score(CpuJob job) {
    double motionAB = 0, motionBC = 0, baselineError = 0, candidateError = 0;
    uint64_t count = 0;
    const uint32_t border = std::min(kMargin, std::min(job.width, job.height) / 4);
    for (uint32_t y = border; y < job.height - border; ++y) {
        for (uint32_t x = border; x < job.width - border; ++x) {
            const size_t p = (static_cast<size_t>(y) * job.width + x) * 4;
            for (uint32_t channel = 0; channel < 3; ++channel) {
                const int a = job.pixels[0][p + channel], b = job.pixels[1][p + channel];
                const int c = job.pixels[2][p + channel];
                const int d0 = int(job.pixels[3][p + channel]) - b;
                const int d1 = int(job.pixels[4][p + channel]) - b;
                motionAB += std::abs(a - b); motionBC += std::abs(b - c);
                baselineError += d0 * d0; candidateError += d1 * d1;
                ++count;
            }
        }
    }
    if (!count) return std::nullopt;
    motionAB /= count; motionBC /= count;
    // Reject static samples, obvious cuts, and one-sided changes that often indicate a cut or occlusion.
    if ((motionAB + motionBC) < 1.0 || motionAB > 48.0 || motionBC > 48.0 ||
        motionAB > 3.0 * motionBC + 2.0 || motionBC > 3.0 * motionAB + 2.0) return std::nullopt;
    return Observation{job.id, baselineError / count, candidateError / count, job.baselineMs, job.candidateMs,
                       job.candidateFirst};
}

class Sampler final : public TripletSampler {
public:
    Sampler(GpuDevice& gpu, FrameGenSettings base, float candidate, double interval, bool fullContext)
        : gpu_(gpu), ctx_(gpu.Context()), baseline_(base), candidate_(base), intervalMs_(interval), fullContext_(fullContext) {
        candidate_.flowScale = candidate;
    }
    ~Sampler() override { CancelPartial(); }

    bool Init(std::wstring& error) {
        baseline_.quality = candidate_.quality = FrameGenQuality::Performance;
        baselineFg_ = FrameGenerator::Create(gpu_, error);
        if (!baselineFg_) return false;
        candidateFg_ = FrameGenerator::Create(gpu_, error);
        if (!candidateFg_) return false;
        baselineFg_->SetSettings(baseline_);
        candidateFg_->SetSettings(candidate_);
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        HRESULT hr = gpu_.Device()->CreateQuery(&desc, &done_);
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (SUCCEEDED(hr)) hr = gpu_.Device()->CreateQuery(&desc, &disjoint_);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& t : timestamps_) if (SUCCEEDED(hr)) hr = gpu_.Device()->CreateQuery(&desc, &t);
        if (FAILED(hr)) { error = L"Adaptive frame generation: could not create GPU queries"; return false; }
        return true;
    }

    void Observe(ID3D11Texture2D* frame, const RectI& rect, int64_t qpc, bool stable) override {
        if (frame) ++diagnostics_.frames;
        if (!stable || !frame || qpc <= 0 || rect.Empty()) { if (frame && !stable) ++diagnostics_.unstableFrames; collecting_ = 0; return; }
        if (ready_ || gpuPending_ || worker_.valid()) return;
        // Full-frame copies are substantial on a busy or slower GPU. Do not allocate or collect a triplet that
        // the controller will be unable to evaluate while presentation is already falling behind.
        if (fullContext_ && overloaded_) { collecting_ = 0; return; }
        const double interval = overloaded_ ? std::max(intervalMs_, 8000.0) : intervalMs_;
        if (!collecting_ && lastStart_ && QpcToMs(qpc - lastStart_) < interval) return;
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        if ((desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
             desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS) || desc.SampleDesc.Count != 1 || rect.left < 0 || rect.top < 0 ||
            rect.right > static_cast<int32_t>(desc.Width) || rect.bottom > static_cast<int32_t>(desc.Height)) {
            collecting_ = 0;
            diagnostics_.unsupportedFormat = true;
            return;
        }
        diagnostics_.unsupportedFormat = false;
        if (!collecting_) {
            const uint32_t w = std::min<uint32_t>(kTile, rect.Width());
            const uint32_t h = std::min<uint32_t>(kTile, rect.Height());
            const uint32_t analysisW = fullContext_ ? static_cast<uint32_t>(rect.Width()) : w;
            const uint32_t analysisH = fullContext_ ? static_cast<uint32_t>(rect.Height()) : h;
            if (w < 64 || h < 64) return;
            // Full-context allocations can fail on a constrained GPU. Do not retry on every captured frame.
            if (lastAllocationFailure_ && QpcToMs(qpc - lastAllocationFailure_) < 5000.0) return;
            if (!EnsureResources(w, h, analysisW, analysisH)) {
                diagnostics_.allocationFailed = true;
                lastAllocationFailure_ = qpc;
                return;
            }
            diagnostics_.allocationFailed = false;
            lastAllocationFailure_ = 0;
            if (!explorationSeeded_) {
                // Vary the starting point between sessions, so short sessions do not repeatedly see only the
                // same first few locations. The later sequence remains deterministic for this session.
                const uint64_t seed = static_cast<uint64_t>(qpc);
                nextExplore_ = 9 + static_cast<uint32_t>((seed ^ (seed >> 17) ^ (seed >> 37)) % 128);
                explorationSeeded_ = true;
            }
            // Revisit a recently useful region occasionally, while spending two thirds of attempts exploring
            // the whole frame. Static sky/HUD tiles otherwise slow learning in games with localized motion.
            currentTile_ = haveUsefulTile_ && sampleOrdinal_ % 3 == 0 ? usefulTile_ : nextExplore_++;
            const auto [dx, dy] = SampleTileOffset(currentTile_, static_cast<uint32_t>(rect.Width()) - w,
                                                   static_cast<uint32_t>(rect.Height()) - h);
            x_ = rect.left + dx;
            y_ = rect.top + dy;
            sourceRect_ = rect;
            lastStart_ = qpc;
            ++sampleOrdinal_;
        } else if (rect.left != sourceRect_.left || rect.top != sourceRect_.top ||
                   rect.right != sourceRect_.right || rect.bottom != sourceRect_.bottom || qpc <= times_[collecting_ - 1]) {
            collecting_ = 0;
            return;
        }
        if (collecting_ == 1) {
            // The held-out middle frame is only scored in the chosen region; no full-size copy is needed.
            const D3D11_BOX tile{x_, y_, 0, x_ + width_, y_ + height_, 1};
            ctx_->CopySubresourceRegion(staging_[1].Get(), 0, 0, 0, 0, frame, 0, &tile);
        } else {
            const UINT left = fullContext_ ? static_cast<UINT>(rect.left) : x_;
            const UINT top = fullContext_ ? static_cast<UINT>(rect.top) : y_;
            const D3D11_BOX box{left, top, 0, left + analysisWidth_, top + analysisHeight_, 1};
            ctx_->CopySubresourceRegion(source_[collecting_ == 0 ? 0 : 1].Get(), 0, 0, 0, 0, frame, 0, &box);
        }
        times_[collecting_] = qpc;
        if (++collecting_ == 3) {
            collecting_ = 0;
            const double a = QpcToMs(times_[1] - times_[0]), b = QpcToMs(times_[2] - times_[1]);
            if (a < 2.0 || b < 2.0 || a > 250.0 || b > 250.0 || a > b * 1.5 || b > a * 1.5) { ++diagnostics_.timingRejected; return; }
            phase_ = static_cast<float>(a / (a + b));
            FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
            id_ = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            ready_ = true;
            ++diagnostics_.triplets;
        }
    }

    void SubmitIfReady(bool idle) override {
        if (!ready_ || gpuPending_ || worker_.valid()) return;
        if (!idle) {
            // Do not run a stale experiment after a prolonged overload; wait for a fresh triplet instead.
            if (QpcToMs(QpcNow() - times_[2]) > 250.0) { CancelPartial(); ready_ = false; ++diagnostics_.expired; }
            return;
        }
        if (partial_ && QpcNow() < nextStageQpc_) return;
        const RectI rect{0, 0, static_cast<int32_t>(analysisWidth_), static_cast<int32_t>(analysisHeight_)};
        const UINT sampleX = fullContext_ ? x_ - static_cast<UINT>(sourceRect_.left) : 0;
        const UINT sampleY = fullContext_ ? y_ - static_cast<UINT>(sourceRect_.top) : 0;
        const D3D11_BOX sampleBox{sampleX, sampleY, 0, sampleX + width_, sampleY + height_, 1};
        auto eval = [&](FrameGenerator& fg, ID3D11RenderTargetView* rtv) {
            fg.Reset();
            return fg.PushFrame(source_[0].Get(), rect, times_[0]) && fg.PushFrame(source_[1].Get(), rect, times_[2]) &&
                   fg.Interpolate(phase_, rtv);
        };
        // Alternating order prevents unrelated GPU work ahead of the experiment from consistently inflating
        // the baseline or candidate. The first evaluation after allocation is warm-up and is not learned from.
        // Alternate accepted comparisons, not attempts: rejected static tiles must not bias timing order.
        if (!partial_) {
            gpuCandidateFirst_ = (diagnostics_.evaluations & 1u) != 0;
            ctx_->Begin(disjoint_.Get());
            ctx_->End(timestamps_[0].Get());
            const bool first = gpuCandidateFirst_ ? eval(*candidateFg_, outputRtv_.Get())
                                                  : eval(*baselineFg_, outputRtv_.Get());
            ctx_->End(timestamps_[1].Get());
            partial_ = true;
            if (!first) {
                MSR_LOG_WARN(L"Adaptive frame generation: evaluation failed");
                diagnostics_.evaluationFailed = true;
                CancelPartial(); ready_ = false;
                return;
            }
            // Preserve this small scored region before reusing the full-size output for the other configuration.
            ctx_->CopySubresourceRegion(staging_[gpuCandidateFirst_ ? 4 : 3].Get(), 0, 0, 0, 0,
                                        output_.Get(), 0, &sampleBox);
            if (fullContext_) {
                const double gapMs = std::clamp(QpcToMs(times_[2] - times_[1]) * 0.5, 2.0, 8.0);
                nextStageQpc_ = QpcNow() + static_cast<int64_t>(gapMs * QpcFrequency() / 1000.0);
                return;
            }
        }
        ctx_->End(timestamps_[2].Get());
        const bool second = gpuCandidateFirst_ ? eval(*baselineFg_, outputRtv_.Get())
                                               : eval(*candidateFg_, outputRtv_.Get());
        ctx_->End(timestamps_[3].Get());
        ctx_->End(disjoint_.Get());
        partial_ = false;
        ready_ = false;
        if (!second) {
            MSR_LOG_WARN(L"Adaptive frame generation: evaluation failed");
            diagnostics_.evaluationFailed = true;
            ctx_->End(done_.Get()); gpuPending_ = true; discardPending_ = true;
            return;
        }
        diagnostics_.evaluationFailed = false;
        ctx_->CopySubresourceRegion(staging_[gpuCandidateFirst_ ? 3 : 4].Get(), 0, 0, 0, 0,
                                    output_.Get(), 0, &sampleBox);
        ctx_->CopySubresourceRegion(staging_[0].Get(), 0, 0, 0, 0, source_[0].Get(), 0, &sampleBox);
        ctx_->CopySubresourceRegion(staging_[2].Get(), 0, 0, 0, 0, source_[1].Get(), 0, &sampleBox);
        ctx_->End(done_.Get());
        gpuPending_ = true;
    }

    std::optional<Observation> Poll() override {
        if (worker_.valid()) {
            if (worker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
            auto result = worker_.get();
            if (discardPending_) { discardPending_ = false; return std::nullopt; }
            if (!result) {
                ++diagnostics_.motionRejected;
                if (haveUsefulTile_ && usefulTile_ == currentTile_) haveUsefulTile_ = false;
            } else {
                ++diagnostics_.evaluations;
                usefulTile_ = currentTile_;
                haveUsefulTile_ = true;
            }
            return result;
        }
        if (!gpuPending_) return std::nullopt;
        const HRESULT ready = ctx_->GetData(done_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_FALSE) return std::nullopt;
        gpuPending_ = false;
        if (FAILED(ready) || discardPending_) { if (FAILED(ready)) ++diagnostics_.readbackRejected; discardPending_ = false; return std::nullopt; }
        if (!warmed_) { warmed_ = true; return std::nullopt; }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        std::array<UINT64, 4> ts{};
        if (ctx_->GetData(disjoint_.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint || !dj.Frequency)
            return std::nullopt;
        for (size_t i = 0; i < ts.size(); ++i)
            if (ctx_->GetData(timestamps_[i].Get(), &ts[i], sizeof(ts[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                return std::nullopt;
        if (ts[1] <= ts[0] || ts[3] <= ts[2]) return std::nullopt;
        CpuJob job;
        job.width = width_; job.height = height_; job.id = id_;
        job.candidateFirst = gpuCandidateFirst_;
        const double firstMs = double(ts[1] - ts[0]) * 1000.0 / dj.Frequency;
        const double secondMs = double(ts[3] - ts[2]) * 1000.0 / dj.Frequency;
        job.baselineMs = gpuCandidateFirst_ ? secondMs : firstMs;
        job.candidateMs = gpuCandidateFirst_ ? firstMs : secondMs;
        for (size_t i = 0; i < staging_.size(); ++i) {
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(ctx_->Map(staging_[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) return std::nullopt;
            job.pixels[i].resize(static_cast<size_t>(width_) * height_ * 4);
            for (uint32_t y = 0; y < height_; ++y)
                memcpy(job.pixels[i].data() + static_cast<size_t>(y) * width_ * 4,
                       static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, width_ * 4);
            ctx_->Unmap(staging_[i].Get(), 0);
        }
        try {
            worker_ = std::async(std::launch::async, Score, std::move(job));
        } catch (const std::exception&) {
            MSR_LOG_WARN(L"Adaptive frame generation: could not start scoring worker");
        }
        return std::nullopt;
    }

    void Reset() override {
        CancelPartial();
        collecting_ = 0; ready_ = false; lastStart_ = 0;
        haveUsefulTile_ = false;
        if (gpuPending_ || worker_.valid()) discardPending_ = true;
    }
    bool Busy() const override { return collecting_ != 0 || ready_ || gpuPending_ || worker_.valid(); }
    bool Ready() const override { return ready_ && (!partial_ || QpcNow() >= nextStageQpc_); }
    SamplerDiagnostics Diagnostics() const override { return diagnostics_; }
    void SetOverloaded(bool overloaded) override { overloaded_ = overloaded; }

private:
    void CancelPartial() {
        if (!partial_) return;
        ctx_->End(disjoint_.Get());
        ctx_->End(done_.Get());
        partial_ = false;
        gpuPending_ = true;
        discardPending_ = true;
    }

    bool HasFullContextHeadroom(uint32_t w, uint32_t h) const {
        if (!fullContext_) return true;
        ComPtr<IDXGIAdapter3> adapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (FAILED(gpu_.Adapter()->QueryInterface(IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) || !memory.Budget)
            return true; // some adapters do not report a usable budget; let allocation determine availability
        const uint64_t available = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
        // Conservative estimate includes the two motion generators, full-size images, and driver overhead.
        const uint64_t required = static_cast<uint64_t>(w) * h * 128 + 64ull * 1024 * 1024;
        return available >= required;
    }

    bool EnsureResources(uint32_t w, uint32_t h, uint32_t analysisW, uint32_t analysisH) {
        if (width_ == w && height_ == h && analysisWidth_ == analysisW && analysisHeight_ == analysisH && source_[0]) return true;
        const auto releaseImages = [&] {
            outputRtv_.Reset();
            for (auto& t : source_) t.Reset();
            output_.Reset();
            for (auto& t : staging_) t.Reset();
            width_ = height_ = analysisWidth_ = analysisHeight_ = 0;
        };
        // Release old or partially created images before checking the budget; otherwise a failed resize can
        // keep consuming the very headroom needed by every subsequent retry.
        releaseImages();
        if (!HasFullContextHeadroom(analysisW, analysisH)) return false;
        warmed_ = false;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = analysisW; d.Height = analysisH; d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = S_OK;
        for (auto& t : source_) if (SUCCEEDED(hr)) hr = gpu_.Device()->CreateTexture2D(&d, nullptr, &t);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = gpu_.Device()->CreateTexture2D(&d, nullptr, &output_);
        if (SUCCEEDED(hr)) hr = gpu_.Device()->CreateRenderTargetView(output_.Get(), nullptr, &outputRtv_);
        d.Width = w; d.Height = h;
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& t : staging_) if (SUCCEEDED(hr)) hr = gpu_.Device()->CreateTexture2D(&d, nullptr, &t);
        if (FAILED(hr)) {
            MSR_LOG_WARN(L"Adaptive frame generation: sample allocation failed: {}", HrToString(hr));
            releaseImages();
            return false;
        }
        width_ = w; height_ = h; analysisWidth_ = analysisW; analysisHeight_ = analysisH;
        return true;
    }

    GpuDevice& gpu_;
    ID3D11DeviceContext1* ctx_;
    FrameGenSettings baseline_, candidate_;
    double intervalMs_;
    bool fullContext_ = false;
    std::unique_ptr<FrameGenerator> baselineFg_, candidateFg_;
    std::array<ComPtr<ID3D11Texture2D>, 2> source_;
    ComPtr<ID3D11Texture2D> output_;
    ComPtr<ID3D11RenderTargetView> outputRtv_;
    std::array<ComPtr<ID3D11Texture2D>, 5> staging_;
    ComPtr<ID3D11Query> done_, disjoint_;
    std::array<ComPtr<ID3D11Query>, 4> timestamps_;
    std::future<std::optional<Observation>> worker_;
    std::array<int64_t, 3> times_{};
    RectI sourceRect_{};
    uint32_t width_ = 0, height_ = 0, x_ = 0, y_ = 0, collecting_ = 0;
    uint32_t analysisWidth_ = 0, analysisHeight_ = 0;
    uint32_t currentTile_ = 0, usefulTile_ = 0, nextExplore_ = 0;
    bool haveUsefulTile_ = false, explorationSeeded_ = false;
    uint64_t sampleOrdinal_ = 0, id_ = 0;
    int64_t lastStart_ = 0;
    int64_t lastAllocationFailure_ = 0;
    int64_t nextStageQpc_ = 0;
    float phase_ = 0.5f;
    bool ready_ = false, gpuPending_ = false, discardPending_ = false, warmed_ = false, gpuCandidateFirst_ = false;
    bool partial_ = false;
    bool overloaded_ = false;
    SamplerDiagnostics diagnostics_;
};
}

std::unique_ptr<TripletSampler> TripletSampler::Create(GpuDevice& gpu, const FrameGenSettings& baseline,
                                                       float candidateFlow, std::wstring& error, double intervalMs,
                                                       bool fullContext) {
    if (!std::isfinite(candidateFlow) || candidateFlow < 0.25f || candidateFlow >= baseline.flowScale || intervalMs < 0.0) {
        error = L"Adaptive frame generation: invalid flow scale or sample interval";
        return nullptr;
    }
    auto sampler = std::make_unique<Sampler>(gpu, baseline, candidateFlow, intervalMs, fullContext);
    return sampler->Init(error) ? std::move(sampler) : nullptr;
}
} // namespace moonsr::fgadaptive
