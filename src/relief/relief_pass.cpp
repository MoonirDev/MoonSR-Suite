// Smart parallax (screen-space relief): tier T1 "relief shading" (milestones M1 + M2 of
// documentation/research/screen_space_relief.md). Owner: upscaling-engineer. Clean-room implementation from the papers
// cited in the research note; no third-party shader code was read or used.
//
// State: FIRST ITERATION of T1. Parallax (T2) and Motion depth (T3) are NOT built: both fall back to relief shading and
// say so in ReliefStats::reason.
//
// Analyse (once per real frame, game resolution, original capture read in place through a cached view; copied only
// when the capture is not shader-bindable), 5 compute passes:
//   luma    prefiltered log2 luminance (full size, separable 5x5 binomial)      shaders/relief/relief_luma_cs.hlsl
//   box 1-2 self-guided filter: (a, b), then their box mean, at half size (fast    relief_box_cs.hlsl
//           guided filter)
//   gate    detail energy, texture / edge / HUD gates, static (HUD) counter and   relief_gate_cs.hlsl
//           all frame statistics (half size); its last group updates the frame
//           state: scene cut, fade, auto-disable, coverage                       relief_frame.hlsli
//   height  H = tanh(D / (k sigma + floor)),                                     relief_height_cs.hlsl
//           temporal history (motion-warped when a MotionFieldView is supplied) (full size)
// Apply (once per displayed frame, output resolution): normals from H, fixed light, directional shading + cavity
// darkening, soft-capped at +-15 % in linear light, scaled by P and the fade; debug views and the Split A/B view
// (relief_apply_ps.hlsl). Generated frames (phase < 1) fetch H and P warped by the prev -> curr motion when available.
//
// Missing (later milestones): parallax offset and march (M3), protection from motion discontinuities / flow confidence,
// generated-frame confidence scaling, history of P (M4), Motion depth (M5), "follow scene light" (the Fixed light is
// used for both settings), pixel-art detection. The internal constants below are first-iteration values tuned on the
// synthetic scenes of `moonsr_bench --relief`, not on real games.
//
// Threading: pipeline thread only (owner of the GpuDevice immediate context). No per-frame heap allocation: game-size
// resources are created on the first Analyse and recreated only when the analysed size or the capture format changes.
// Statistics reach the CPU through a ring of small staging buffers mapped with DO_NOT_WAIT a few frames later (no
// stall, no flush), like the GPU timestamps.
#include "relief/relief_pass.h"

#include "core/log.h"
#include "framegen/frame_generator.h"

#include "g_cs_relief_box1.h"
#include "g_cs_relief_box2.h"
#include "g_cs_relief_gate.h"
#include "g_cs_relief_height.h"
#include "g_cs_relief_luma.h"
#include "g_ps_relief_apply.h"
#include "g_vs_fullscreen.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

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

// ---- internal constants (first iteration; see the header comment) ----
constexpr float kEps = 0.03f;            // guided-filter regulariser, log2-luma^2: steps above ~0.17 stop stay in B
constexpr float kSigmaK = 1.5f;          // contrast normalisation
constexpr float kSigmaFloor = 0.02f;     // stops noise / grain / dark regions from being amplified
constexpr float kTexGate[4] = {0.012f, 0.045f, 0.8f, 1.4f}; // sigma_local ramps (log2 luma)
constexpr float kEdgeGate[2] = {0.22f, 0.55f};              // |grad B| ramp (stops per px), dilated 3 px
constexpr float kHistoryAlpha = 0.35f;   // new-estimate weight where history is valid
constexpr float kFrameChangedMin = 0.1f; // share of changed pixels (|dL| > 0.01 stop) for the HUD counter to advance
constexpr float kMaxDelta = 0.15f;       // soft cap of the luminance change (fraction of linear luminance)
// Gains per unit strength. Directional shading dominates cavity darkening, so borders read as lit bevels (highlight on
// the light-facing edge, shadow on the opposite edge) rather than as uniformly darkened outlines.
constexpr float kShadeK = 0.60f, kCavityK = 0.20f, kBump = 2.5f;

struct alignas(16) AnalyseCB {
    int32_t srcOffset[2];
    uint32_t size[2];
    int32_t radius;
    float eps, sigmaK, sigmaFloor;
    float texGate[4];
    float edgeGate[2];
    uint32_t flags, sceneCutIndex;
    float cellInv[2];
    uint32_t cells[2];
    float historyAlpha, frameChangedMin;
    uint32_t half[2];
};
static_assert(sizeof(AnalyseCB) % 16 == 0);

struct alignas(16) ApplyCB {
    float content[4];
    float gameSize[2], gameTexel[2];
    float light[4];
    float shadeGain, cavityGain, maxDelta, bump;
    uint32_t debug;
    float splitX, warp;
    uint32_t srcLinear;
    float cellInv[2];
    uint32_t cells[2];
    float tapPx;
    float pad[3];
};
static_assert(sizeof(ApplyCB) % 16 == 0);

constexpr uint32_t kFlagHud = 1, kFlagEdges = 2, kFlagAuto = 4, kFlagMotion = 8, kFlagMotionCut = 16, kFlagReset = 32;
constexpr uint32_t kStateSlots = 16;
enum StateSlot : uint32_t { kFade = 0, kCovEma, kHudEma, kMeanY, kReason, kFrames, kCov, kHud, kCut, kChanged };

bool IsLinearFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT: return true;
    default: return false;
    }
}

class ReliefPassImpl final : public ReliefPass {
public:
    explicit ReliefPassImpl(GpuDevice& device) : device_(device) {}

    bool Init(std::wstring& error) {
        ID3D11Device1* d = device_.Device();
        HRESULT hr = d->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &vs_);
        if (SUCCEEDED(hr)) hr = d->CreatePixelShader(g_ps_relief_apply, sizeof(g_ps_relief_apply), nullptr, &psApply_);
        if (SUCCEEDED(hr)) hr = d->CreateComputeShader(g_cs_relief_luma, sizeof(g_cs_relief_luma), nullptr, &csLuma_);
        if (SUCCEEDED(hr)) hr = d->CreateComputeShader(g_cs_relief_box1, sizeof(g_cs_relief_box1), nullptr, &csBox_[0]);
        if (SUCCEEDED(hr)) hr = d->CreateComputeShader(g_cs_relief_box2, sizeof(g_cs_relief_box2), nullptr, &csBox_[1]);
        if (SUCCEEDED(hr)) hr = d->CreateComputeShader(g_cs_relief_height, sizeof(g_cs_relief_height), nullptr, &csHeight_);
        if (SUCCEEDED(hr)) hr = d->CreateComputeShader(g_cs_relief_gate, sizeof(g_cs_relief_gate), nullptr, &csGate_);
        if (FAILED(hr)) {
            error = L"Smart parallax shaders could not be created: " + HrToString(hr);
            return false;
        }
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.ByteWidth = sizeof(AnalyseCB);
        hr = d->CreateBuffer(&cb, nullptr, &cbAnalyse_);
        cb.ByteWidth = sizeof(ApplyCB);
        if (SUCCEEDED(hr)) hr = d->CreateBuffer(&cb, nullptr, &cbApply_);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        if (SUCCEEDED(hr)) hr = d->CreateSamplerState(&sd, &linClamp_);
        // Small raw buffers: the frame statistics (slot-spread counters) and the persistent frame state (64 bytes).
        // Statistic counters, each spread over 64 slots (relief_common.hlsli RS_SLOTS): 3 counters in A, 2 in B.
        if (SUCCEEDED(hr)) hr = CreateRawBuffer((5 * 64 + 4) * 4, stats_); // 5 counters x 64 slots + finished-group counter (relief_common.hlsli)
        if (SUCCEEDED(hr)) hr = CreateRawBuffer(kStateSlots * 4, state_);
        D3D11_BUFFER_DESC st{};
        st.ByteWidth = kStateSlots * 4;
        st.Usage = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (Readback& r : readback_)
            if (SUCCEEDED(hr)) hr = d->CreateBuffer(&st, nullptr, &r.buffer);
        if (FAILED(hr)) {
            error = L"Smart parallax resources could not be created: " + HrToString(hr);
            return false;
        }
        // GPU timing is optional: without it everything still works and reports 0 ms.
        timingAvailable_ = true;
        for (auto* ring : {&applyQ_, &analyseQ_}) {
            for (QuerySet& q : ring->sets) {
                D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                hr = d->CreateQuery(&qd, &q.disjoint);
                qd.Query = D3D11_QUERY_TIMESTAMP;
                if (SUCCEEDED(hr)) hr = d->CreateQuery(&qd, &q.begin);
                if (SUCCEEDED(hr)) hr = d->CreateQuery(&qd, &q.end);
                if (FAILED(hr)) break;
            }
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
        const bool scaleChanged = s.detailScalePx != settings_.detailScalePx;
        settings_ = s;
        if (tierChanged || scaleChanged) Reset(); // history of a different band-pass is not reusable
    }

    bool Analyse(ID3D11Texture2D* capture, const RectI& rect, const MotionFieldView* motion, int64_t qpc) override {
        if (!capture || rect.Empty() || rect.left < 0 || rect.top < 0) return false;
        if (settings_.tier == ReliefTier::Off) return false;
        D3D11_TEXTURE2D_DESC cd{};
        capture->GetDesc(&cd);
        if (static_cast<uint32_t>(rect.right) > cd.Width || static_cast<uint32_t>(rect.bottom) > cd.Height ||
            cd.SampleDesc.Count != 1)
            return false;
        const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};
        if (size != analysedSize_ || cd.Format != srcFormat_) {
            if (!analysedSize_.Empty())
                MSR_LOG_INFO(L"Smart parallax: analysis size {}x{} -> {}x{}", analysedSize_.width, analysedSize_.height,
                             size.width, size.height);
            if (!CreateSized(size, cd.Format)) return false;
        }
        ID3D11DeviceContext1* ctx = device_.Context();
        CollectTimings(analyseQ_);
        CollectState();
        QuerySet* q = BeginTiming(analyseQ_);

        // The luma pass reads the capture directly through a cached view (one per capture pool texture, created the
        // first time that texture is seen). A capture that cannot be bound as a shader resource is copied instead.
        ID3D11ShaderResourceView* srcView = nullptr;
        int32_t srcOffset[2] = {0, 0};
        if ((cd.BindFlags & D3D11_BIND_SHADER_RESOURCE) && cd.MipLevels == 1 && cd.ArraySize == 1) {
            srcView = CaptureView(capture);
            srcOffset[0] = rect.left;
            srcOffset[1] = rect.top;
        }
        if (!srcView) {
            if (!srcCopy_ && !CreateSourceCopy(size, cd.Format)) {
                EndTiming(analyseQ_, q);
                return false;
            }
            const D3D11_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                                static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1};
            ctx->CopySubresourceRegion(srcCopy_.Get(), 0, 0, 0, 0, capture, 0, &box);
            srcView = srcSrv_.Get();
            srcOffset[0] = srcOffset[1] = 0;
        }

        cur_ ^= 1u;
        const uint32_t prev = cur_ ^ 1u;
        motionValid_ = motion != nullptr && motion->valid && motion->flow != nullptr && motion->cellSizePx > 0.0f &&
                       !motion->cells.Empty();
        motionFlow_ = motionValid_ ? motion->flow : nullptr; // kept (AddRef) for Apply of generated frames
        motionCells_ = motionValid_ ? motion->cells : Size{};
        motionCellPx_ = motionValid_ ? motion->cellSizePx : 0.0f;

        AnalyseCB c{};
        c.srcOffset[0] = srcOffset[0];
        c.srcOffset[1] = srcOffset[1];
        c.size[0] = size.width;
        c.size[1] = size.height;
        // Base radius about 3 x detail scale game px, applied at half resolution (fast guided filter).
        c.radius = std::clamp(static_cast<int32_t>(std::lround(1.5f * settings_.detailScalePx)), 1, 6);
        c.half[0] = (size.width + 1) / 2;
        c.half[1] = (size.height + 1) / 2;
        c.eps = kEps;
        c.sigmaK = kSigmaK;
        c.sigmaFloor = kSigmaFloor;
        std::memcpy(c.texGate, kTexGate, sizeof(c.texGate));
        std::memcpy(c.edgeGate, kEdgeGate, sizeof(c.edgeGate));
        c.flags = (settings_.protectHud ? kFlagHud : 0) | (settings_.protectEdges ? kFlagEdges : 0) |
                  (settings_.autoDisable ? kFlagAuto : 0) | (motionValid_ ? kFlagMotion : 0) |
                  (motionValid_ && motion->stats ? kFlagMotionCut : 0) | (resetPending_ ? kFlagReset : 0);
        c.sceneCutIndex = motionValid_ ? motion->sceneCutIndex : 0;
        if (motionValid_) {
            c.cellInv[0] = c.cellInv[1] = 1.0f / motion->cellSizePx;
            c.cells[0] = motion->cells.width;
            c.cells[1] = motion->cells.height;
        }
        c.historyAlpha = kHistoryAlpha;
        c.frameChangedMin = kFrameChangedMin;
        if (!WriteCb(cbAnalyse_.Get(), &c, sizeof(c))) {
            EndTiming(analyseQ_, q);
            return false;
        }
        resetPending_ = false;

        const UINT zeros[4] = {0, 0, 0, 0};
        ctx->ClearUnorderedAccessViewUint(stats_.uav.Get(), zeros);
        ctx->CSSetConstantBuffers(0, 1, cbAnalyse_.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linClamp_.GetAddressOf());
        const UINT g16x = (size.width + 15) / 16, g16y = (size.height + 15) / 16;
        const UINT g8fx = (size.width + 7) / 8, g8fy = (size.height + 7) / 8;

        const UINT gHx = ((size.width + 1) / 2 + 15) / 16, gHy = ((size.height + 1) / 2 + 15) / 16; // half size
        Dispatch(csLuma_.Get(), {srcView}, {l_[cur_].uav.Get()}, g16x, g16y);
        Dispatch(csBox_[0].Get(), {l_[cur_].srv.Get()}, {t1_.uav.Get()}, gHx, gHy);
        Dispatch(csBox_[1].Get(), {t1_.srv.Get()}, {t2_.uav.Get()}, gHx, gHy);
        Dispatch(csGate_.Get(),
                 {l_[cur_].srv.Get(), t2_.srv.Get(), count_[prev].srv.Get(), l_[prev].srv.Get(),
                  motionValid_ ? motion->stats : nullptr},
                 {p_.uav.Get(), stats_.uav.Get(), state_.uav.Get(), count_[cur_].uav.Get()}, gHx, gHy);
        Dispatch(csHeight_.Get(),
                 {t2_.srv.Get(), l_[cur_].srv.Get(), nullptr, h_[prev].srv.Get(),
                  motionValid_ ? motion->flow : nullptr, motionValid_ ? motion->stats : nullptr, state_.srv.Get(),
                  p_.srv.Get()},
                 {h_[cur_].uav.Get()}, g8fx, g8fy);
        ID3D11Buffer* nullCb = nullptr;
        ctx->CSSetConstantBuffers(0, 1, &nullCb);

        // Statistics copy for the CPU (read a few frames later without waiting).
        Readback& r = readback_[readNext_];
        if (!r.pending) {
            ctx->CopyResource(r.buffer.Get(), state_.buffer.Get());
            r.pending = true;
            readNext_ = (readNext_ + 1) % kReadback;
        }
        EndTiming(analyseQ_, q);
        lastQpc_ = qpc;
        ++analysedFrames_;
        return true;
    }

    bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
               const RectI& contentInDst, float phase) override {
        if (!src || !dst || srcSize.Empty() || srcSize != dstSize) {
            if (!applyArgsLogged_) {
                applyArgsLogged_ = true;
                MSR_LOG_ERROR(L"Smart parallax: Apply called with invalid arguments ({}x{} -> {}x{}; logged once)",
                              srcSize.width, srcSize.height, dstSize.width, dstSize.height);
            }
            return false;
        }
        if (analysedFrames_ == 0 || !h_[cur_].srv || settings_.tier == ReliefTier::Off) return false;
        ComPtr<ID3D11Resource> srcRes, dstRes;
        src->GetResource(&srcRes);
        dst->GetResource(&dstRes);
        if (!srcRes || !dstRes || srcRes.Get() == dstRes.Get()) return false; // contract: dst is a different texture
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        src->GetDesc(&svd);

        ID3D11DeviceContext1* ctx = device_.Context();
        CollectTimings(applyQ_);
        QuerySet* q = BeginTiming(applyQ_);

        RectI content = contentInDst;
        content.left = std::clamp(content.left, 0, static_cast<int32_t>(dstSize.width));
        content.top = std::clamp(content.top, 0, static_cast<int32_t>(dstSize.height));
        content.right = std::clamp(content.right, content.left, static_cast<int32_t>(dstSize.width));
        content.bottom = std::clamp(content.bottom, content.top, static_cast<int32_t>(dstSize.height));

        ApplyCB c{};
        c.content[0] = static_cast<float>(content.left);
        c.content[1] = static_cast<float>(content.top);
        c.content[2] = static_cast<float>(std::max(1, content.Width()));
        c.content[3] = static_cast<float>(std::max(1, content.Height()));
        c.gameSize[0] = static_cast<float>(analysedSize_.width);
        c.gameSize[1] = static_cast<float>(analysedSize_.height);
        c.gameTexel[0] = 1.0f / c.gameSize[0];
        c.gameTexel[1] = 1.0f / c.gameSize[1];
        constexpr float kDeg = 3.14159265358979f / 180.0f;
        const float az = settings_.lightAzimuthDeg * kDeg, el = settings_.lightElevationDeg * kDeg;
        c.light[0] = std::cos(el) * std::cos(az);
        c.light[1] = -std::cos(el) * std::sin(az); // screen y points down; 90 degrees = up
        c.light[2] = std::sin(el);
        c.shadeGain = settings_.strength * settings_.shading * kShadeK;
        c.cavityGain = settings_.strength * settings_.cavity * kCavityK;
        c.maxDelta = kMaxDelta;
        c.bump = kBump;
        c.tapPx = std::max(1.0f, settings_.detailScalePx / 1.5f); // 1 game px at the old 1.5 px detail size
        c.debug = static_cast<uint32_t>(settings_.debugView);
        if (settings_.debugView == ReliefDebugView::Offset || settings_.debugView == ReliefDebugView::Depth)
            c.debug = 0; // parallax / motion-depth views do not exist yet: show the shaded result
        c.splitX = c.content[0] + std::floor(0.5f * c.content[2]);
        const bool warp = motionValid_ && motionFlow_ && phase > 0.0f && phase < 1.0f;
        c.warp = warp ? 1.0f - phase : 0.0f;
        c.srcLinear = IsLinearFormat(svd.Format) ? 1u : 0u;
        if (warp) {
            c.cellInv[0] = c.cellInv[1] = 1.0f / motionCellPx_;
            c.cells[0] = motionCells_.width;
            c.cells[1] = motionCells_.height;
        }
        bool ok = WriteCb(cbApply_.Get(), &c, sizeof(c));
        if (ok) {
            const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(dstSize.width), static_cast<float>(dstSize.height),
                                    0.0f, 1.0f};
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs_.Get(), nullptr, 0);
            ctx->PSSetShader(psApply_.Get(), nullptr, 0);
            ctx->RSSetState(nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->OMSetRenderTargets(1, &dst, nullptr);
            ID3D11ShaderResourceView* srvs[5] = {src, h_[cur_].srv.Get(), p_.srv.Get(), state_.srv.Get(),
                                                 warp ? motionFlow_.Get() : nullptr};
            ctx->PSSetShaderResources(0, 5, srvs);
            ctx->PSSetConstantBuffers(0, 1, cbApply_.GetAddressOf());
            ctx->PSSetSamplers(0, 1, linClamp_.GetAddressOf());
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* nullSrv[5] = {};
            ctx->PSSetShaderResources(0, 5, nullSrv);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        EndTiming(applyQ_, q);
        if (ok) ++appliedFrames_;
        return ok;
    }

    void Reset() override {
        resetPending_ = true; // next Analyse: no history, HUD counter cleared, fade restarts from 0
        motionValid_ = false;
        motionFlow_.Reset();
        lastQpc_ = 0;
    }

    ReliefStats Stats() const override {
        ReliefStats st;
        st.tierInEffect = settings_.tier == ReliefTier::Off ? ReliefTier::Off : ReliefTier::Shading;
        st.analyseGpuMs = Median(analyseQ_);
        st.applyGpuMs = Median(applyQ_);
        if (settings_.tier == ReliefTier::Off) return st;
        const bool haveState = stateFrames_ > 0;
        const float fade = haveState ? stateVals_[kFade] : 0.0f;
        const int reason = haveState ? static_cast<int>(stateVals_[kReason] + 0.5f) : 0;
        st.autoDisabled = reason != 0;
        st.active = haveState && fade > 0.0f && analysedFrames_ > 0;
        st.texturedCoverage = st.active ? std::clamp(stateVals_[kCov], 0.0f, 1.0f) : 0.0f;
        std::wstring why;
        if (settings_.tier == ReliefTier::Parallax || settings_.tier == ReliefTier::MotionDepth)
            why = L"Parallax is not built yet; showing relief shading.";
        auto add = [&why](const wchar_t* s) {
            if (!why.empty()) why += L' ';
            why += s;
        };
        switch (reason) {
        case 1: add(L"Paused: few textured surfaces (flat-shaded or stylised picture, or mostly sky)."); break;
        case 2: add(L"Paused: the picture is mostly static overlay (menu or paused screen)."); break;
        case 3: add(L"Paused: the picture is very dark."); break;
        default:
            if (!haveState) add(L"Starting: waiting for the first analysed frames.");
            break;
        }
        st.reason = why;
        return st;
    }

private:
    static constexpr uint32_t kQuerySets = 6; // enough in-flight frames that reading never waits on the GPU
    static constexpr uint32_t kSamples = 64;  // GPU time = median of the last kSamples measured calls
    static constexpr uint32_t kReadback = 4;

    struct QuerySet {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    struct QueryRing {
        std::array<QuerySet, kQuerySets> sets{};
        uint32_t next = 0;
        std::array<double, kSamples> samples{};
        uint32_t sampleNext = 0, sampleCount = 0;
    };
    struct Readback {
        ComPtr<ID3D11Buffer> buffer;
        bool pending = false;
    };
    struct Tex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };
    struct RawBuf {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };

    HRESULT CreateRawBuffer(UINT bytes, RawBuf& b) {
        ID3D11Device1* d = device_.Device();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        HRESULT hr = d->CreateBuffer(&bd, nullptr, &b.buffer);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.NumElements = bytes / 4;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (SUCCEEDED(hr)) hr = d->CreateShaderResourceView(b.buffer.Get(), &sv, &b.srv);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = bytes / 4;
        uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(b.buffer.Get(), &uv, &b.uav);
        return hr;
    }

    HRESULT CreateTex(Size s, DXGI_FORMAT f, Tex& t) {
        ID3D11Device1* d = device_.Device();
        t = Tex{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s.width;
        td.Height = s.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = d->CreateTexture2D(&td, nullptr, &t.tex);
        if (SUCCEEDED(hr)) hr = d->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
        if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav);
        return hr;
    }

    // Cached shader-resource view of a capture pool texture (round-robin over kViews entries; views are created only
    // when a texture is seen for the first time, so the steady state allocates nothing).
    ID3D11ShaderResourceView* CaptureView(ID3D11Texture2D* tex) {
        for (CaptureViewEntry& e : views_)
            if (e.tex.Get() == tex) return e.srv.Get();
        CaptureViewEntry& e = views_[viewNext_];
        viewNext_ = (viewNext_ + 1) % kViews;
        e = CaptureViewEntry{};
        const HRESULT hr = device_.Device()->CreateShaderResourceView(tex, nullptr, &e.srv);
        if (FAILED(hr)) {
            if (!viewLogged_) {
                viewLogged_ = true;
                MSR_LOG_WARN(L"Smart parallax: no shader view of the capture ({}); copying it instead (logged once)",
                             HrToString(hr));
            }
            e = CaptureViewEntry{};
            return nullptr;
        }
        e.tex = tex;
        return e.srv.Get();
    }

    bool CreateSourceCopy(Size s, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s.width;
        td.Height = s.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_.Device()->CreateTexture2D(&td, nullptr, &srcCopy_);
        if (SUCCEEDED(hr)) hr = device_.Device()->CreateShaderResourceView(srcCopy_.Get(), nullptr, &srcSrv_);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Smart parallax: capture copy {}x{} failed: {}", s.width, s.height, HrToString(hr));
            srcCopy_.Reset();
            srcSrv_.Reset();
            return false;
        }
        return true;
    }

    bool CreateSized(Size s, DXGI_FORMAT captureFormat) {
        analysedSize_ = {};
        srcFormat_ = DXGI_FORMAT_UNKNOWN;
        srcCopy_.Reset();
        srcSrv_.Reset();
        HRESULT hr = S_OK;
        for (uint32_t i = 0; i < 2 && SUCCEEDED(hr); ++i) {
            hr = CreateTex(s, DXGI_FORMAT_R16_UNORM, l_[i]); // encoded log2 luminance (relief_common.hlsli LEnc)
            if (SUCCEEDED(hr)) hr = CreateTex(s, DXGI_FORMAT_R16_FLOAT, h_[i]);
        }
        const Size halfSize{(s.width + 1) / 2, (s.height + 1) / 2};
        for (uint32_t i = 0; i < 2 && SUCCEEDED(hr); ++i) // static counter (count / 64, encoded reference L), half size
            hr = CreateTex(halfSize, DXGI_FORMAT_R16G16_UNORM, count_[i]);
        if (SUCCEEDED(hr)) hr = CreateTex(halfSize, DXGI_FORMAT_R32G32_FLOAT, t1_);
        if (SUCCEEDED(hr)) hr = CreateTex(halfSize, DXGI_FORMAT_R16G16_FLOAT, t2_);
        if (SUCCEEDED(hr)) hr = CreateTex(halfSize, DXGI_FORMAT_R8G8B8A8_UNORM, p_); // gates (hud, edge x tex, changed, encoded sigma)
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"Smart parallax: analysis resources {}x{} (capture format {}) failed: {}", s.width, s.height,
                          static_cast<int>(captureFormat), HrToString(hr));
            srcCopy_.Reset();
            srcSrv_.Reset();
            return false;
        }
        analysedSize_ = s;
        srcFormat_ = captureFormat;
        analysedFrames_ = 0;
        resetPending_ = true;
        return true;
    }

    bool WriteCb(ID3D11Buffer* b, const void* data, size_t bytes) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = device_.Context()->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) {
            if (!cbLogged_) {
                cbLogged_ = true;
                MSR_LOG_ERROR(L"Smart parallax: constant buffer map failed: {} (logged once)", HrToString(hr));
            }
            return false;
        }
        std::memcpy(m.pData, data, bytes);
        device_.Context()->Unmap(b, 0);
        return true;
    }

    // Binds SRVs t0.. and UAVs u0.., dispatches, and unbinds both so the next pass can read what this one wrote.
    void Dispatch(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                  std::initializer_list<ID3D11UnorderedAccessView*> uavs, UINT gx, UINT gy) {
        ID3D11DeviceContext1* ctx = device_.Context();
        std::array<ID3D11ShaderResourceView*, 9> s{};
        std::array<ID3D11UnorderedAccessView*, 5> u{};
        const UINT ns = static_cast<UINT>(std::min<size_t>(srvs.size(), s.size()));
        const UINT nu = static_cast<UINT>(std::min<size_t>(uavs.size(), u.size()));
        std::copy_n(srvs.begin(), ns, s.begin());
        std::copy_n(uavs.begin(), nu, u.begin());
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetShaderResources(0, ns, s.data());
        ctx->CSSetUnorderedAccessViews(0, nu, u.data(), nullptr);
        ctx->Dispatch(gx, gy, 1);
        const std::array<ID3D11ShaderResourceView*, 9> ns0{};
        const std::array<ID3D11UnorderedAccessView*, 5> nu0{};
        ctx->CSSetShaderResources(0, ns, ns0.data());
        ctx->CSSetUnorderedAccessViews(0, nu, nu0.data(), nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }

    QuerySet* BeginTiming(QueryRing& ring) {
        if (!timingAvailable_ || ring.sets[ring.next].pending) return nullptr;
        QuerySet* q = &ring.sets[ring.next];
        device_.Context()->Begin(q->disjoint.Get());
        device_.Context()->End(q->begin.Get());
        return q;
    }
    void EndTiming(QueryRing& ring, QuerySet* q) {
        if (!q) return;
        device_.Context()->End(q->end.Get());
        device_.Context()->End(q->disjoint.Get());
        q->pending = true;
        ring.next = (ring.next + 1) % kQuerySets;
    }

    // Reads every finished query set without flushing. A set that is not ready yet is left for a later frame.
    void CollectTimings(QueryRing& ring) {
        if (!timingAvailable_) return;
        ID3D11DeviceContext1* ctx = device_.Context();
        for (QuerySet& q : ring.sets) {
            if (!q.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 t0 = 0, t1 = 0;
            if (ctx->GetData(q.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(q.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            q.pending = false;
            if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
            ring.samples[ring.sampleNext] = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
            ring.sampleNext = (ring.sampleNext + 1) % kSamples;
            ring.sampleCount = std::min(ring.sampleCount + 1, kSamples);
        }
    }

    static double Median(const QueryRing& ring) {
        if (ring.sampleCount == 0) return 0.0;
        std::array<double, kSamples> s = ring.samples; // stack copy; Stats() runs at UI rate
        const auto mid = s.begin() + ring.sampleCount / 2;
        std::nth_element(s.begin(), mid, s.begin() + ring.sampleCount);
        return *mid;
    }

    // Reads finished statistics copies (oldest first) without waiting.
    void CollectState() {
        ID3D11DeviceContext1* ctx = device_.Context();
        for (uint32_t k = 0; k < kReadback; ++k) {
            Readback& r = readback_[(readNext_ + k) % kReadback];
            if (!r.pending) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(r.buffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (SUCCEEDED(hr)) {
                std::memcpy(stateVals_.data(), m.pData, sizeof(float) * kStateSlots);
                ctx->Unmap(r.buffer.Get(), 0);
                ++stateFrames_;
            }
            r.pending = false;
        }
    }

    GpuDevice& device_;
    ReliefSettings settings_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psApply_;
    ComPtr<ID3D11ComputeShader> csLuma_, csGate_, csHeight_;
    std::array<ComPtr<ID3D11ComputeShader>, 2> csBox_;
    ComPtr<ID3D11Buffer> cbAnalyse_, cbApply_;
    ComPtr<ID3D11SamplerState> linClamp_;
    RawBuf stats_, state_;
    std::array<Readback, kReadback> readback_{};
    uint32_t readNext_ = 0;
    std::array<float, kStateSlots> stateVals_{};
    uint64_t stateFrames_ = 0;

    static constexpr uint32_t kViews = 4;
    struct CaptureViewEntry {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::array<CaptureViewEntry, kViews> views_{};
    uint32_t viewNext_ = 0;
    bool viewLogged_ = false;
    ComPtr<ID3D11Texture2D> srcCopy_; // fallback when the capture cannot be bound as a shader resource
    ComPtr<ID3D11ShaderResourceView> srcSrv_;
    DXGI_FORMAT srcFormat_ = DXGI_FORMAT_UNKNOWN;
    std::array<Tex, 2> l_, h_, count_;
    Tex t1_, t2_, p_;
    uint32_t cur_ = 0;
    bool resetPending_ = true;

    ComPtr<ID3D11ShaderResourceView> motionFlow_;
    Size motionCells_;
    float motionCellPx_ = 0.0f;
    bool motionValid_ = false;

    QueryRing applyQ_, analyseQ_;
    bool timingAvailable_ = false;
    Size analysedSize_;
    int64_t lastQpc_ = 0;
    uint64_t analysedFrames_ = 0, appliedFrames_ = 0;
    bool applyArgsLogged_ = false, cbLogged_ = false;
};

} // namespace

std::unique_ptr<ReliefPass> ReliefPass::Create(GpuDevice& device, std::wstring& error) {
    auto pass = std::make_unique<ReliefPassImpl>(device);
    if (!pass->Init(error)) {
        MSR_LOG_ERROR(L"Smart parallax unavailable: {}", error);
        return nullptr;
    }
    MSR_LOG_INFO(L"Smart parallax pass created (T1 relief shading, first iteration; parallax not built)");
    return pass;
}

} // namespace moonsr
