// Smart parallax (screen-space relief): GROUNDWORK implementation. Owner: upscaling-engineer.
//
// State: first iteration of the plumbing only. There is NO relief algorithm here yet:
//   - Analyse records the analysed size and whether a motion field was supplied (no GPU work).
//   - Apply is an exact pass-through: CopySubresourceRegion when source and destination have the same format and size,
//     otherwise a fullscreen pass that loads each source texel 1:1 (shaders/relief/relief_passthrough_ps.hlsl).
//   - Stats reports active = false with a plain-language reason, the tier that would be in effect, and the measured GPU
//     time of Apply (timestamp queries, read back several frames later without flushing or stalling).
// Missing (milestones in documentation/research/screen_space_relief.md, Part 7): height estimation and protection (M1),
// relief shading (M2), parallax (M3), history / generated-frame warping / auto-disable (M4), Motion depth (M5), the debug
// views (Height, Normals, Protection, Offset, Depth, Split).
//
// Threading: pipeline thread only (owner of the GpuDevice immediate context). No per-frame heap allocation: every D3D
// object is created in Create(); Apply and Analyse only use stack values and ComPtr references to existing objects.
#include "relief/relief_pass.h"

#include "core/log.h"
#include "framegen/frame_generator.h"

#include "g_ps_relief_passthrough.h"
#include "g_vs_fullscreen.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace moonsr {
namespace {

using Microsoft::WRL::ComPtr;

float ClampFinite(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

ReliefSettings Sanitize(const ReliefSettings& in) {
    const ReliefSettings def;
    ReliefSettings s = in;
    if (static_cast<uint32_t>(s.tier) > static_cast<uint32_t>(ReliefTier::MotionDepth)) s.tier = ReliefTier::Off;
    if (static_cast<uint32_t>(s.light) > static_cast<uint32_t>(ReliefLight::FollowScene)) s.light = ReliefLight::Fixed;
    if (static_cast<uint32_t>(s.debugView) > static_cast<uint32_t>(ReliefDebugView::Split)) s.debugView = ReliefDebugView::None;
    s.strength = ClampFinite(s.strength, 0.0f, 1.0f, def.strength);
    s.shading = ClampFinite(s.shading, 0.0f, 1.0f, def.shading);
    s.cavity = ClampFinite(s.cavity, 0.0f, 1.0f, def.cavity);
    s.parallaxMaxPx = ClampFinite(s.parallaxMaxPx, 0.0f, 3.0f, def.parallaxMaxPx);
    s.detailScalePx = ClampFinite(s.detailScalePx, 0.75f, 4.0f, def.detailScalePx);
    s.lightAzimuthDeg = std::isfinite(s.lightAzimuthDeg) ? std::fmod(std::fmod(s.lightAzimuthDeg, 360.0f) + 360.0f, 360.0f)
                                                         : def.lightAzimuthDeg;
    s.lightElevationDeg = ClampFinite(s.lightElevationDeg, 15.0f, 80.0f, def.lightElevationDeg);
    return s;
}

class ReliefPassImpl final : public ReliefPass {
public:
    explicit ReliefPassImpl(GpuDevice& device) : device_(device) {}

    bool Init(std::wstring& error) {
        ID3D11Device1* d = device_.Device();
        HRESULT hr = d->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &vs_);
        if (SUCCEEDED(hr)) hr = d->CreatePixelShader(g_ps_relief_passthrough, sizeof(g_ps_relief_passthrough), nullptr, &ps_);
        if (FAILED(hr)) {
            error = L"Smart parallax shaders could not be created: " + HrToString(hr);
            return false;
        }
        // GPU timing is optional: without it Apply still works and reports 0 ms.
        timingAvailable_ = true;
        for (QuerySet& q : queries_) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            hr = d->CreateQuery(&qd, &q.disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (SUCCEEDED(hr)) hr = d->CreateQuery(&qd, &q.begin);
            if (SUCCEEDED(hr)) hr = d->CreateQuery(&qd, &q.end);
            if (FAILED(hr)) {
                MSR_LOG_WARN(L"Smart parallax: GPU timestamp queries unavailable ({}); timing is not reported",
                             HrToString(hr));
                timingAvailable_ = false;
                break;
            }
        }
        return true;
    }

    void SetSettings(const ReliefSettings& settings) override {
        const ReliefSettings s = Sanitize(settings);
        if (s == settings_) return;
        const bool tierChanged = s.tier != settings_.tier;
        settings_ = s;
        if (tierChanged) Reset();
    }

    bool Analyse(ID3D11Texture2D* capture, const RectI& rect, const MotionFieldView* motion, int64_t qpc) override {
        if (!capture || rect.Empty() || rect.left < 0 || rect.top < 0) return false;
        const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
        if (size != analysedSize_) {
            if (!analysedSize_.Empty())
                MSR_LOG_INFO(L"Smart parallax: analysis size {}x{} -> {}x{}", analysedSize_.width, analysedSize_.height,
                             size.width, size.height);
            analysedSize_ = size;
        }
        motionValid_ = motion != nullptr && motion->valid;
        lastQpc_ = qpc;
        ++analysedFrames_;
        return true;
    }

    bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
               const RectI& contentInDst, float phase) override {
        (void)contentInDst; // used by the relief algorithm (letterbox exclusion); the pass-through copies everything
        (void)phase;        // used by the relief algorithm (generated-frame warping)
        if (!src || !dst || srcSize.Empty() || srcSize != dstSize) {
            if (!applyArgsLogged_) {
                applyArgsLogged_ = true;
                MSR_LOG_ERROR(L"Smart parallax: Apply called with invalid arguments ({}x{} -> {}x{}; logged once)",
                              srcSize.width, srcSize.height, dstSize.width, dstSize.height);
            }
            return false;
        }
        ID3D11DeviceContext1* ctx = device_.Context();
        CollectTimings();
        QuerySet* q = nullptr;
        if (timingAvailable_ && !queries_[next_].pending) {
            q = &queries_[next_];
            ctx->Begin(q->disjoint.Get());
            ctx->End(q->begin.Get());
        }

        const bool ok = CopyOrDraw(src, dst, dstSize);

        if (q) {
            ctx->End(q->end.Get());
            ctx->End(q->disjoint.Get());
            q->pending = true;
            next_ = (next_ + 1) % kQuerySets;
        }
        if (ok) ++appliedFrames_;
        return ok;
    }

    void Reset() override {
        analysedSize_ = {};
        motionValid_ = false;
        lastQpc_ = 0;
        // Pending timestamp queries stay pending; they complete on their own and are collected by the next Apply.
    }

    ReliefStats Stats() const override {
        ReliefStats st;
        st.active = false; // groundwork: the pass never changes the image
        st.tierInEffect = TierInEffect();
        st.texturedCoverage = 0.0f;
        st.meanOffsetPx = 0.0f;
        st.autoDisabled = false;
        if (settings_.tier != ReliefTier::Off) {
            st.reason = L"Smart parallax is in development: no effect yet.";
            if (settings_.tier == ReliefTier::MotionDepth && st.tierInEffect != ReliefTier::MotionDepth)
                st.reason += L" Motion depth needs frame generation on; Parallax would be used.";
        }
        st.analyseGpuMs = 0.0; // Analyse issues no GPU work yet
        st.applyGpuMs = MedianApplyMs();
        return st;
    }

private:
    static constexpr uint32_t kQuerySets = 6; // enough in-flight frames that reading never waits on the GPU
    // Apply GPU time is reported as the median of the last kSamples measured frames: one frame delayed by another
    // process or by memory paging (seen at start-up: one 840 ms sample among 0.02 ms ones) must not dominate it.
    static constexpr uint32_t kSamples = 64;

    double MedianApplyMs() const {
        if (sampleCount_ == 0) return 0.0;
        std::array<double, kSamples> s = samples_; // stack copy; Stats() runs at UI rate
        const auto mid = s.begin() + sampleCount_ / 2;
        std::nth_element(s.begin(), mid, s.begin() + sampleCount_);
        return *mid;
    }

    struct QuerySet {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };

    ReliefTier TierInEffect() const {
        if (settings_.tier == ReliefTier::MotionDepth && !motionValid_) return ReliefTier::Parallax; // D7 fallback
        return settings_.tier;
    }

    // Reads every finished query set without flushing. A set that is not ready yet is left for a later frame.
    void CollectTimings() {
        if (!timingAvailable_) return;
        ID3D11DeviceContext1* ctx = device_.Context();
        for (QuerySet& q : queries_) {
            if (!q.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 t0 = 0, t1 = 0;
            if (ctx->GetData(q.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(q.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            q.pending = false;
            if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
            samples_[sampleNext_] = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
            sampleNext_ = (sampleNext_ + 1) % kSamples;
            sampleCount_ = std::min(sampleCount_ + 1, kSamples);
        }
    }

    bool CopyOrDraw(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, Size size) {
        ID3D11DeviceContext1* ctx = device_.Context();
        ComPtr<ID3D11Resource> srcRes, dstRes;
        src->GetResource(&srcRes);
        dst->GetResource(&dstRes);
        ComPtr<ID3D11Texture2D> srcTex, dstTex;
        if (!srcRes || !dstRes || FAILED(srcRes.As(&srcTex)) || FAILED(dstRes.As(&dstTex))) return false;
        if (srcTex.Get() == dstTex.Get()) return false; // contract: dst is a different texture
        D3D11_TEXTURE2D_DESC sd{}, dd{};
        srcTex->GetDesc(&sd);
        dstTex->GetDesc(&dd);
        if (sd.Width < size.width || sd.Height < size.height || dd.Width < size.width || dd.Height < size.height) return false;

        // Fast exact path: same format, same size, single-sampled, single subresource on both sides.
        if (sd.Format == dd.Format && sd.Width == dd.Width && sd.Height == dd.Height && sd.SampleDesc.Count == 1 &&
            dd.SampleDesc.Count == 1 && sd.MipLevels == 1 && dd.MipLevels == 1 && sd.ArraySize == 1 && dd.ArraySize == 1) {
            ctx->CopySubresourceRegion(dstTex.Get(), 0, 0, 0, 0, srcTex.Get(), 0, nullptr);
            return true;
        }

        // Fullscreen pass-through (format conversion). Explicit state: other stages may leave theirs bound.
        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height), 0.0f, 1.0f};
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->OMSetRenderTargets(1, &dst, nullptr);
        ctx->PSSetShaderResources(0, 1, &src);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    GpuDevice& device_;
    ReliefSettings settings_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    std::array<QuerySet, kQuerySets> queries_{};
    uint32_t next_ = 0;
    bool timingAvailable_ = false;
    std::array<double, kSamples> samples_{};
    uint32_t sampleNext_ = 0, sampleCount_ = 0;
    Size analysedSize_;
    bool motionValid_ = false;
    int64_t lastQpc_ = 0;
    uint64_t analysedFrames_ = 0, appliedFrames_ = 0;
    bool applyArgsLogged_ = false;
};

} // namespace

std::unique_ptr<ReliefPass> ReliefPass::Create(GpuDevice& device, std::wstring& error) {
    auto pass = std::make_unique<ReliefPassImpl>(device);
    if (!pass->Init(error)) {
        MSR_LOG_ERROR(L"Smart parallax unavailable: {}", error);
        return nullptr;
    }
    MSR_LOG_INFO(L"Smart parallax pass created (groundwork pass-through, no effect yet)");
    return pass;
}

} // namespace moonsr
