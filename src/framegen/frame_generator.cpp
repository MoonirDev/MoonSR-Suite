// FrameGenerator implementation: original MoonSR frame interpolation (Direct3D 11 FL 11_0, SM 5.0 compute).
//
// Per real frame (PushFrame), all on the GPU, no CPU readback on the hot path:
//   copy -> luma pyramid at flow resolution (kept with the frame; the previous frame's pyramid is reused) ->
//   regional luma histograms -> for each level coarse to fine, both directions in one dispatch:
//   [prediction + window search (zero-mean SSD + census)] -> [neighbour-consensus regularisation] ->
//   4x4 sub-block refinement + sub-pixel steps -> forward-backward consistency / confidence / stats ->
//   surroundings-motion measure -> HUD mask -> scene-cut decision (GPU, written to a stats buffer) ->
//   asynchronous copy of the stats into a 3-deep staging ring (PairInfo polls it with DO_NOT_WAIT).
// Per generated frame (Interpolate): splat both cell fields to time t (atomic max on a confidence key) ->
// resolve + hole fill -> half-resolution robust hypothesis selection (patch error, visibility, occlusion reasoning) ->
// full-resolution pixel shader: smooth (bilinear) motion upsampling, evidence-weighted blending at motion boundaries,
// confidence fallback, HUD protection, scene-cut handling, anti-aliasing guide -> guided edge-directed anti-aliasing
// into the caller's target. Shader headers document each pass.
//
// Every generated frame is synthesised from the ORIGINAL pair (RealFrame(1), RealFrame(0)); nothing generated is
// fed back. Steady state performs no heap allocation: all resources are created when the frame size or the
// settings change.
#include "framegen/frame_generator.h"

#include "core/log.h"
#ifdef MOONSR_EXPERIMENTAL_HARDWARE_FLOW
#include "hardware_flow_analysis.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <thread>

#include "g_vs_fullscreen.h"
#include "g_cs_fg_luma.h"
#include "g_cs_fg_downsample.h"
#include "g_cs_fg_histogram.h"
#include "g_cs_fg_search.h"
#include "g_cs_fg_search_r4.h"
#include "g_cs_fg_search_fast.h"
#include "g_cs_fg_search_r4_fast.h"
#include "g_cs_fg_regularize.h"
#include "g_cs_fg_refine.h"
#include "g_cs_fg_cross_splat.h"
#include "g_cs_fg_cross.h"
#include "g_cs_fg_consistency.h"
#include "g_cs_fg_motionfrac.h"
#include "g_cs_fg_hudmask.h"
#include "g_cs_fg_scenecut.h"
#include "g_cs_fg_splat.h"
#include "g_cs_fg_resolve.h"
#include "g_ps_fg_interpolate.h"
#include "g_cs_fg_pixsel.h"
#include "g_ps_fg_aa.h"

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

// Mirror of `cbuffer FgConstants` in shaders/framegen/fg_common.hlsli.
struct FgConstants {
    int32_t srcSize[2];
    int32_t dstSize[2];
    int32_t fieldSize[2];
    int32_t auxSize[2];
    float p0[4];
    float p1[4];
    uint32_t u0[4];
    uint32_t u1[4];
};
static_assert(sizeof(FgConstants) == 96);

constexpr uint32_t kMaxLevels = 7;
constexpr uint32_t kBlock = 8;
constexpr uint32_t kCell = 4;
constexpr uint32_t kStatSlots = 16;
constexpr uint32_t kHistCounters = 144;
constexpr uint32_t kReadbackDepth = 3;
// Stats buffer slots, see fg_common.hlsli.
constexpr uint32_t kStatCut = 5;
constexpr uint32_t kStatHistDist = 6;
constexpr uint32_t kStatBadFrac = 7;
constexpr uint32_t kStatMeanConf = 8;
constexpr uint32_t kStatMeanMotion = 9;

struct TierParams {
    uint32_t extraLevels;
    uint32_t radius;       // window search radius at every level but the coarsest
    uint32_t radiusCoarse; // at the coarsest level (few blocks, so a wide search is cheap)
    bool census;
    uint32_t refineCandidates; // 4 or 9
    uint32_t subpixelSteps;    // 0..2
    uint32_t interpCandidates; // time-t cells considered per selection texel: 4 or 9
    bool cubicWarp;            // Catmull-Rom final fetch instead of bilinear
    uint32_t propagation;      // long-range propagation (2 passes, steps 4 and 2): 0 off, 1 levels >= 1, 2 all levels
    uint32_t finestLevel;      // finest searched level (0, or 1: the level-0 search is skipped, refine starts at level 1)
    uint32_t radius0;          // search radius at level 0 (0 = radius)
};

TierParams Tier(FrameGenQuality q) {
    switch (q) {
    case FrameGenQuality::Performance: return {0, 2, 6, false, 4, 0, 4, false, 0, 0, 0};
    // The parent prediction covers large motion; a radius of 2 at the finest level avoids 81-way searches
    // per full-resolution block while preserving the wider searches at coarser levels.
    case FrameGenQuality::Quality: return {1, 4, 8, true, 9, 2, 9, true, 2, 0, 2};
    case FrameGenQuality::Balanced:
    default: return {0, 3, 8, true, 4, 1, 4, true, 0, 0, 1};
    }
}

uint32_t DivUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Near-best margin of the search's repeating-texture disambiguation (cost units: rms luma levels + census term).
constexpr float kNearAbs = 1.0f;
constexpr float kNearRel = 0.05f;

// Search radius per level: radiusCoarse at the coarsest level, the tier's radius elsewhere. (Growing the radius with
// the level, min(8, r + 2 l), was measured: it did not recover the small fast object, cost ~0.2 ms at 1080p and let the
// periodic fence lock onto aliases, -6.8 dB. Not used.)
uint32_t LevelRadius(const TierParams& tp, uint32_t level, bool coarsest) {
    if (coarsest) return tp.radiusCoarse;
    return level == 0 && tp.radius0 != 0 ? tp.radius0 : tp.radius;
}

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
    void Reset() {
        tex.Reset();
        srv.Reset();
        uav.Reset();
        rtv.Reset();
    }
};

struct Buf {
    ComPtr<ID3D11Buffer> buf;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

class FrameGeneratorImpl final : public FrameGenerator {
public:
    bool Init(GpuDevice& device, std::wstring& error) {
        dev_ = device.Device();
        ctx_ = device.Context();
        if (dev_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            error = L"FrameGenerator: feature level 11_0 required";
            return false;
        }
        struct CsDesc {
            const BYTE* data;
            size_t size;
            ComPtr<ID3D11ComputeShader>* out;
            const wchar_t* name;
        };
        const CsDesc cs[] = {
            {g_cs_fg_luma, sizeof(g_cs_fg_luma), &csLuma_, L"luma"},
            {g_cs_fg_downsample, sizeof(g_cs_fg_downsample), &csDownsample_, L"downsample"},
            {g_cs_fg_histogram, sizeof(g_cs_fg_histogram), &csHistogram_, L"histogram"},
            {g_cs_fg_search, sizeof(g_cs_fg_search), &csSearch_, L"search"},
            {g_cs_fg_search_r4, sizeof(g_cs_fg_search_r4), &csSearchR4_, L"search r4"},
            {g_cs_fg_search_fast, sizeof(g_cs_fg_search_fast), &csSearchFast_, L"search fast"},
            {g_cs_fg_search_r4_fast, sizeof(g_cs_fg_search_r4_fast), &csSearchR4Fast_, L"search r4 fast"},
            {g_cs_fg_regularize, sizeof(g_cs_fg_regularize), &csRegularize_, L"regularize"},
            {g_cs_fg_refine, sizeof(g_cs_fg_refine), &csRefine_, L"refine"},
            {g_cs_fg_cross_splat, sizeof(g_cs_fg_cross_splat), &csCrossSplat_, L"cross splat"},
            {g_cs_fg_cross, sizeof(g_cs_fg_cross), &csCross_, L"cross"},
            {g_cs_fg_consistency, sizeof(g_cs_fg_consistency), &csConsistency_, L"consistency"},
            {g_cs_fg_motionfrac, sizeof(g_cs_fg_motionfrac), &csMotionFrac_, L"motionfrac"},
            {g_cs_fg_hudmask, sizeof(g_cs_fg_hudmask), &csHudMask_, L"hudmask"},
            {g_cs_fg_scenecut, sizeof(g_cs_fg_scenecut), &csSceneCut_, L"scenecut"},
            {g_cs_fg_splat, sizeof(g_cs_fg_splat), &csSplat_, L"splat"},
            {g_cs_fg_resolve, sizeof(g_cs_fg_resolve), &csResolve_, L"resolve"},
            {g_cs_fg_pixsel, sizeof(g_cs_fg_pixsel), &csPixsel_, L"pixsel"},
        };
        for (const auto& c : cs) {
            const HRESULT hr = dev_->CreateComputeShader(c.data, c.size, nullptr, c.out->GetAddressOf());
            if (FAILED(hr)) return Fail(error, std::wstring(L"CreateComputeShader ") + c.name, hr);
        }
        HRESULT hr = dev_->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &vs_);
        if (FAILED(hr)) return Fail(error, L"CreateVertexShader", hr);
        hr = dev_->CreatePixelShader(g_ps_fg_interpolate, sizeof(g_ps_fg_interpolate), nullptr, &psInterp_);
        if (FAILED(hr)) return Fail(error, L"CreatePixelShader interpolate", hr);
        hr = dev_->CreatePixelShader(g_ps_fg_aa, sizeof(g_ps_fg_aa), nullptr, &psAa_);
        if (FAILED(hr)) return Fail(error, L"CreatePixelShader aa", hr);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = dev_->CreateSamplerState(&sd, &linear_);
        if (FAILED(hr)) return Fail(error, L"CreateSamplerState", hr);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = dev_->CreateRasterizerState(&rd, &raster_);
        if (FAILED(hr)) return Fail(error, L"CreateRasterizerState", hr);

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FgConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = dev_->CreateBuffer(&bd, nullptr, &cbInterp_);
        if (FAILED(hr)) return Fail(error, L"CreateBuffer(interp constants)", hr);

        // Stats: 16 counters, cleared per pair; 3-deep staging ring for the asynchronous readback.
        if (!CreateUintBuffer(kStatSlots, stats_, L"stats")) return Fail(error, L"stats buffer", E_FAIL);
        for (auto& h : hist_)
            if (!CreateUintBuffer(kHistCounters, h, L"histogram")) return Fail(error, L"histogram buffer", E_FAIL);
        D3D11_BUFFER_DESC sb{};
        sb.ByteWidth = kStatSlots * 4;
        sb.Usage = D3D11_USAGE_STAGING;
        sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : staging_) {
            hr = dev_->CreateBuffer(&sb, nullptr, &s.buf);
            if (FAILED(hr)) return Fail(error, L"CreateBuffer(staging)", hr);
        }
        D3D11_QUERY_DESC eq{D3D11_QUERY_EVENT, 0};
        for (auto& q : stagingDone_) {
            hr = dev_->CreateQuery(&eq, &q);
            if (FAILED(hr)) return Fail(error, L"CreateQuery(event)", hr);
        }
        return true;
    }

    void SetSettings(const FrameGenSettings& settings) override {
        FrameGenSettings s = settings;
        s.flowScale = std::isfinite(s.flowScale) ? std::clamp(s.flowScale, 0.25f, 1.0f) : 0.5f;
        if (s == settings_) return;
        const bool flowChanged = s.quality != settings_.quality || s.flowScale != settings_.flowScale ||
                                 s.preciseMotion != settings_.preciseMotion;
        settings_ = s;
        if (flowChanged) flowDirty_ = true;
        if (!settings_.hudProtection) ClearHud(); // the flow passes weight by the mask: keep it neutral
    }

    bool PushFrame(ID3D11Texture2D* src, const RectI& srcRectIn, int64_t qpc) override {
        if (!src) return false;
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (sd.SampleDesc.Count != 1 || (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM && sd.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                                         sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            if (!formatWarned_) MSR_LOG_ERROR(L"FrameGenerator: unsupported source format {} / {} samples", static_cast<int>(sd.Format), sd.SampleDesc.Count);
            formatWarned_ = true;
            return false;
        }
        const RectI rect{std::max(srcRectIn.left, 0), std::max(srcRectIn.top, 0),
                         std::min(srcRectIn.right, static_cast<int32_t>(sd.Width)),
                         std::min(srcRectIn.bottom, static_cast<int32_t>(sd.Height))};
        if (rect.Empty()) return false;
        const Size size{static_cast<uint32_t>(rect.Width()), static_cast<uint32_t>(rect.Height())};

        if (size != frameSize_) {
            ResetHistory();
            if (!AllocateFrameResources(size)) {
                frameSize_ = {};
                return false;
            }
            frameSize_ = size;
            flowDirty_ = true;
        }
        if (flowDirty_) {
            if (!AllocateFlowResources()) {
                frameSize_ = {};
                ResetHistory();
                return false;
            }
            flowDirty_ = false;
            prevFinalValid_ = false;
            // The current newest frame becomes the previous one: rebuild its pyramid once for the new configuration.
            if (histCount_ >= 1) BuildPyramid(cur_);
            if (histCount_ >= 2) histCount_ = 1;
        }

        cur_ ^= 1u;
        D3D11_BOX box{static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right),
                      static_cast<UINT>(rect.bottom), 1};
        ctx_->CopySubresourceRegion(frames_[cur_].tex.Get(), 0, 0, 0, 0, src, 0, &box);
        qpc_[cur_] = qpc;
        BuildPyramid(cur_);
        histCount_ = std::min(2u, histCount_ + 1);
        ++pairId_;

        info_ = {};
        info_.currQpc = qpc;
        if (histCount_ < 2) return true;
        info_.prevQpc = qpc_[cur_ ^ 1u];
        if (!Analyse()) return false;
        info_.valid = true;
        return true;
    }

    void Reset() override { ResetHistory(); }

    FramePairInfo PairInfo() const override {
        PollStats();
        FramePairInfo i = info_;
        if (!i.valid) return i;
        if (statsPairId_ != 0) {
            i.meanConfidence = statsMeanConf_;
            i.meanMotionPx = statsMeanMotion_;
            // A cut flag belongs to exactly one pair; never carry it over to a later pair.
            i.sceneCut = statsPairId_ == pairId_ && statsCut_;
        }
        return i;
    }

    ID3D11Texture2D* RealFrame(uint32_t index) const override {
        if (index >= histCount_ || index > 1) return nullptr;
        return frames_[index == 0 ? cur_ : (cur_ ^ 1u)].tex.Get();
    }

    Size FrameSize() const override { return frameSize_; }

    bool Interpolate(float t, ID3D11RenderTargetView* dst, ID3D11ShaderResourceView** selection) override {
        if (selection) *selection = nullptr;
        if (!dst || histCount_ < 2 || !info_.valid) return false;
        t = std::clamp(t, 0.0f, 1.0f);
        const TierParams tp = Tier(settings_.quality);

        D3D11_MAPPED_SUBRESOURCE map;
        HRESULT hr = ctx_->Map(cbInterp_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"FrameGenerator: Map(constants) failed: {}", HrToString(hr));
            return false;
        }
        FgConstants c{};
        SetSize(c.srcSize, frameSize_);
        SetSize(c.dstSize, cells_);
        SetSize(c.fieldSize, selSize_);
        SetSize(c.auxSize, levelSize_[meanLevel_]);
        // The fields were built with activeFlowScale_; a settings change only takes effect at the next PushFrame.
        const float cellPx = static_cast<float>(kCell) / activeFlowScale_;
        c.p0[0] = t;
        c.p0[1] = cellPx;
        c.p0[2] = 1.0f / cellPx;
        c.p0[3] = fallbackMode_;
        // Patch agreement sigma, luma levels (mean abs difference). Measured on the real_* and synthetic sequences:
        // 12 sent too much to the blend fallback (ghosting); 24 / 32 / 40 gave +0.6 / +0.9 / +0.9 dB over 12.
        c.p1[0] = 32.0f;
        c.p1[1] = 1.0f / activeFlowScale_; // visibility tolerance: 1 flow pixel ...
        c.p1[2] = 0.1f;                    // ... + 10% of the motion
        c.p1[3] = 10.0f;                   // occlusion cost, luma levels (score of a one-sided hypothesis)
        c.u0[0] = static_cast<uint32_t>(debugView_);
        c.u0[1] = tp.interpCandidates;
        c.u0[2] = tp.cubicWarp ? 1u : 0u;
        c.u1[0] = settings_.hudProtection ? 1u : 0u;
        c.u1[1] = 0u; // anti-aliasing guide view (shader debug output), off
        memcpy(map.pData, &c, sizeof(c));
        ctx_->Unmap(cbInterp_.Get(), 0);

        ID3D11ShaderResourceView* field = final_[finalCur_].srv.Get();
        const UINT gx = DivUp(cells_.width, 8), gy = DivUp(cells_.height, 8);
        const UINT zero[4] = {0, 0, 0, 0};
        ctx_->ClearUnorderedAccessViewUint(keys_.uav.Get(), zero);
        RunCS(csSplat_.Get(), cbInterp_.Get(), {field}, {keys_.uav.Get()}, gx, gy, 2);
        RunCS(csResolve_.Get(), cbInterp_.Get(), {field, keys_.srv.Get()}, {tfield_.uav.Get()}, gx, gy, 1);
        const uint32_t prev = cur_ ^ 1u;

        RunCS(csPixsel_.Get(), cbInterp_.Get(),
              {tfield_.srv.Get(), field, pyr_[prev][0].srv.Get(), pyr_[cur_][0].srv.Get(), pyr_[prev][meanLevel_].srv.Get(),
               pyr_[cur_][meanLevel_].srv.Get(), hud_.srv.Get()},
              {sel_.uav.Get(), selAux_.uav.Get()}, DivUp(selSize_.width, 8), DivUp(selSize_.height, 8), 1);

        ID3D11ShaderResourceView* srvs[8] = {frames_[prev].srv.Get(), frames_[cur_].srv.Get(), sel_.srv.Get(), selAux_.srv.Get(),
                                             hud_.srv.Get(), stats_.srv.Get(), pyr_[prev][meanLevel_].srv.Get(),
                                             pyr_[cur_][meanLevel_].srv.Get()};
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->RSSetState(raster_.Get());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(nullptr, 0);
        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(frameSize_.width), static_cast<float>(frameSize_.height), 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rts[2] = {gen_.rtv.Get(), guide_.rtv.Get()};
        ctx_->OMSetRenderTargets(2, rts, nullptr);
        ctx_->PSSetShader(psInterp_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 8, srvs);
        ctx_->PSSetSamplers(0, 1, linear_.GetAddressOf());
        ctx_->PSSetConstantBuffers(0, 1, cbInterp_.GetAddressOf());
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv[8] = {};
        ctx_->PSSetShaderResources(0, 8, nullSrv);

        // Guided anti-aliasing of the generated frame into the caller's target (fg_aa_ps.hlsl).
        ctx_->OMSetRenderTargets(1, &dst, nullptr);
        ID3D11ShaderResourceView* aaSrvs[2] = {gen_.srv.Get(), guide_.srv.Get()};
        ctx_->PSSetShader(psAa_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 2, aaSrvs);
        ctx_->Draw(3, 0);
        ctx_->PSSetShaderResources(0, 2, nullSrv);
        ID3D11RenderTargetView* nullRtv[2] = {};
        ctx_->OMSetRenderTargets(2, nullRtv, nullptr);
        if (selection && debugView_ == FrameGenDebugView::None) *selection = sel_.srv.Get();
        return true;
    }

    void SetDebugView(FrameGenDebugView view) override { debugView_ = view; }

    // Views of the newest pair's analysis (written by Analyse in PushFrame). Added by upscaling-engineer for the
    // temporal upscaler; no generator state changes. Exact semantics as stored:
    //   flow: final_[finalCur_], R32G32B32A32_FLOAT array, slice 0 prev->curr, slice 1 curr->prev, xy in frame px,
    //         z = confidence (consistency x match), w = normalised match residual rel = rms / (stddev + 2) (>= 0).
    //   cell c covers frame pixels [c * cellPx, (c + 1) * cellPx) with cellPx = kCell / activeFlowScale_ (the grid
    //   may extend slightly beyond the frame: cells_ = ceil(flow width / 4)).
    //   staticMask: hud_ (R8, frame size) of this pair, null when HUD protection is off (then it is all zero).
    //   stats: stats_ with kStatCut written by the scene-cut pass of this pair.
    MotionFieldView MotionField() const override {
        MotionFieldView v;
        if (histCount_ < 2 || !info_.valid || !final_[finalCur_].srv) return v;
        v.valid = true;
        v.flow = final_[finalCur_].srv.Get();
        v.cells = cells_;
        v.cellSizePx = static_cast<float>(kCell) / activeFlowScale_;
        v.staticMask = settings_.hudProtection ? hud_.srv.Get() : nullptr;
        v.stats = stats_.srv.Get();
        v.sceneCutIndex = kStatCut;
        return v;
    }

private:
    static bool Fail(std::wstring& error, const std::wstring& what, HRESULT hr) {
        error = L"FrameGenerator: " + what + L" failed: " + HrToString(hr);
        MSR_LOG_ERROR(L"{}", error);
        return false;
    }

    static void SetSize(int32_t out[2], Size s) {
        out[0] = static_cast<int32_t>(s.width);
        out[1] = static_cast<int32_t>(s.height);
    }

    // The HUD mask of the previous pair weights the matching cost (overlay pixels excluded). Zero = no exclusion.
    void ClearHud() {
        if (!hud_.uav) return;
        const float zero[4] = {0, 0, 0, 0};
        ctx_->ClearUnorderedAccessViewFloat(hud_.uav.Get(), zero);
    }

    void ResetHistory() {
#ifdef MOONSR_EXPERIMENTAL_HARDWARE_FLOW
        hardwareFlow_.reset();
#endif
        ClearHud();
        histCount_ = 0;
        info_ = {};
        prevFinalValid_ = false;
        statsPairId_ = 0;
        for (auto& p : pendingPair_) p = 0;
    }

    // ---- resource creation (only on size / settings change) ----

    bool CreateUintBuffer(uint32_t count, Buf& b, const wchar_t* name) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = count * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = dev_->CreateBuffer(&bd, nullptr, &b.buf);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_UINT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.NumElements = count;
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(b.buf.Get(), &sv, &b.srv);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_UINT;
        uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = count;
        if (SUCCEEDED(hr)) hr = dev_->CreateUnorderedAccessView(b.buf.Get(), &uv, &b.uav);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"FrameGenerator: {} buffer failed: {}", name, HrToString(hr));
            return false;
        }
        return true;
    }

    bool CreateTex(Tex& t, Size s, DXGI_FORMAT fmt, uint32_t slices, bool uav, bool rtv, const wchar_t* name) {
        t.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s.width;
        td.Height = s.height;
        td.MipLevels = 1;
        td.ArraySize = slices;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0) | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
        HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &t.tex);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
        if (SUCCEEDED(hr) && uav) hr = dev_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav);
        if (SUCCEEDED(hr) && rtv) hr = dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"FrameGenerator: texture {} {}x{}x{} failed: {}", name, s.width, s.height, slices, HrToString(hr));
            t.Reset();
            return false;
        }
        return true;
    }

    bool CreateImmutableCb(const FgConstants& c, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FgConstants);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{&c, 0, 0};
        const HRESULT hr = dev_->CreateBuffer(&bd, &sd, out.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"FrameGenerator: CreateBuffer(constants) failed: {}", HrToString(hr));
            return false;
        }
        return true;
    }

    bool AllocateFrameResources(Size size) {
        for (auto& f : frames_)
            if (!CreateTex(f, size, DXGI_FORMAT_B8G8R8A8_UNORM, 1, false, false, L"history")) return false;
        if (!CreateTex(hud_, size, DXGI_FORMAT_R8_UNORM, 1, true, false, L"hud mask")) return false;
        // Interpolation intermediates: half-resolution hypothesis selection, generated colour + anti-aliasing guide.
        selSize_ = {DivUp(size.width, 2), DivUp(size.height, 2)};
        if (!CreateTex(sel_, selSize_, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, true, false, L"selection") ||
            !CreateTex(selAux_, selSize_, DXGI_FORMAT_R8G8_UNORM, 1, true, false, L"selection aux") ||
            !CreateTex(gen_, size, DXGI_FORMAT_R8G8B8A8_UNORM, 1, false, true, L"generated") ||
            !CreateTex(guide_, size, DXGI_FORMAT_R8_UNORM, 1, false, true, L"aa guide"))
            return false;
        ClearHud();
        MSR_LOG_INFO(L"FrameGenerator: frame size {}x{}", size.width, size.height);
        return true;
    }

    bool AllocateFlowResources() {
        TierParams tp = Tier(settings_.quality);
        if (settings_.quality == FrameGenQuality::Performance && settings_.preciseMotion) tp.subpixelSteps = 1;
        const float s = settings_.flowScale;
        activeFlowScale_ = s;
        const Size l0{std::max(16u, static_cast<uint32_t>(std::lround(frameSize_.width * s))),
                      std::max(16u, static_cast<uint32_t>(std::lround(frameSize_.height * s)))};
        // Levels: enough that the coarsest level is <= ~96 px wide (then the search range in frame pixels is roughly
        // independent of flowScale), plus the tier's extra level, never coarser than 16 px.
        uint32_t levels = 1;
        for (uint32_t w = l0.width; w > 96 && levels < kMaxLevels; w = (w + 1) / 2) ++levels;
        levels = std::clamp(levels + tp.extraLevels, 2u, kMaxLevels);
        levelSize_[0] = l0;
        // Exact frame -> flow ratio (horizontal; the vertical one differs by rounding only, < 0.2 %).
        activeFlowScale_ = static_cast<float>(l0.width) / static_cast<float>(frameSize_.width);
        uint32_t built = 1;
        for (; built < levels; ++built) {
            const Size p = levelSize_[built - 1];
            const Size n{(p.width + 1) / 2, (p.height + 1) / 2};
            if (n.width < 16 || n.height < 9) break;
            levelSize_[built] = n;
        }
        levels_ = built;
        cells_ = {DivUp(l0.width, kCell), DivUp(l0.height, kCell)};
        meanLevel_ = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(std::log2(8.0 * s))), 0, static_cast<int>(levels_) - 1));
        histLevel_ = std::min(1u, levels_ - 1);

        for (auto& fr : pyr_)
            for (uint32_t l = 0; l < kMaxLevels; ++l) {
                fr[l].Reset();
                if (l < levels_ && !CreateTex(fr[l], levelSize_[l], DXGI_FORMAT_R16_FLOAT, 1, true, false, L"pyramid")) return false;
            }
        for (uint32_t l = 0; l < kMaxLevels; ++l) {
            fieldA_[l].Reset();
            fieldB_[l].Reset();
            if (l >= levels_) continue;
            const Size b{DivUp(levelSize_[l].width, kBlock), DivUp(levelSize_[l].height, kBlock)};
            blocks_[l] = b;
            if (!CreateTex(fieldA_[l], b, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, true, false, L"field") ||
                !CreateTex(fieldB_[l], b, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, true, false, L"field"))
                return false;
        }
        if (!CreateTex(refined_, cells_, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, true, false, L"refined") ||
            !CreateTex(refined2_, cells_, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, true, false, L"refined (cross)") ||
            !CreateTex(crossKeys_, cells_, DXGI_FORMAT_R32_UINT, 2, true, false, L"cross keys") ||
            !CreateTex(final_[0], cells_, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, true, false, L"final") ||
            !CreateTex(final_[1], cells_, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, true, false, L"final") ||
            !CreateTex(motionFrac_, cells_, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, true, false, L"surroundings motion") ||
            !CreateTex(keys_, cells_, DXGI_FORMAT_R32_UINT, 1, true, false, L"splat keys") ||
            !CreateTex(tfield_, cells_, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, true, false, L"t field"))
            return false;

        // Constant buffers for all per-pair passes (immutable: they only depend on sizes and settings).
        FgConstants c{};
        SetSize(c.srcSize, frameSize_);
        SetSize(c.dstSize, l0);
        c.u0[0] = std::clamp(static_cast<uint32_t>(std::ceil(static_cast<float>(frameSize_.width) / l0.width)), 1u, 4u);
        if (!CreateImmutableCb(c, cbLuma_)) return false;
        for (uint32_t l = 0; l + 1 < levels_; ++l) {
            c = {};
            SetSize(c.srcSize, levelSize_[l]);
            SetSize(c.dstSize, levelSize_[l + 1]);
            if (!CreateImmutableCb(c, cbDown_[l])) return false;
        }
        c = {};
        SetSize(c.srcSize, levelSize_[histLevel_]);
        if (!CreateImmutableCb(c, cbHist_)) return false;

        for (uint32_t l = 0; l < levels_; ++l) {
            const bool coarsest = l + 1 == levels_;
            const float levelScale = activeFlowScale_ / static_cast<float>(1u << l); // frame px -> level px
            c = {};
            SetSize(c.srcSize, levelSize_[l]);
            SetSize(c.dstSize, blocks_[l]);
            if (!coarsest) SetSize(c.fieldSize, blocks_[l + 1]);
            SetSize(c.auxSize, cells_);
            c.p0[0] = 0.1f; // lambdaS: prior penalty per level pixel
            c.p0[1] = 4.0f; // lambdaC: census weight (mean Hamming bits per pixel)
            c.p0[2] = levelScale;
            c.p0[3] = 2.0f; // census threshold, luma levels
            c.p1[0] = static_cast<float>(1u << l) / static_cast<float>(kCell);
            c.p1[1] = kNearAbs; // repeating-texture disambiguation (see fg_search_cs.hlsl)
            c.p1[2] = kNearRel;
            c.u0[0] = LevelRadius(tp, l, coarsest);
            c.u0[1] = coarsest ? 0u : 1u;
            c.u0[2] = 0; // temporal candidate: set per pair (see cbSearchT_)
            c.u0[3] = tp.census ? 1u : 0u;
            c.u1[0] = 1; // HUD weights (neutral while the mask is zero)
            c.u1[2] = 9; // parent candidates (4 measured: -0.02 ms, -3 dB on periodic_fence; not worth it)
            if (!CreateImmutableCb(c, cbSearch_[l])) return false;
            c.u0[2] = 1;
            if (!CreateImmutableCb(c, cbSearchT_[l])) return false;

            c = {};
            SetSize(c.srcSize, levelSize_[l]);
            SetSize(c.dstSize, blocks_[l]);
            c.p0[0] = 1.0f; // mu
            c.p0[1] = 4.0f; // tau, level px
            c.u1[0] = 1;
            c.u0[0] = 1;
            if (!CreateImmutableCb(c, cbRegularize_[l])) return false;
            c.u0[0] = 4;
            c.p0[0] = 0.0f; // propagation is data-driven only: a far vector is adopted only if it matches better
            if (!CreateImmutableCb(c, cbProp4_[l])) return false;
            c.u0[0] = 2;
            if (!CreateImmutableCb(c, cbProp2_[l])) return false;
        }

        c = {};
        const uint32_t finest = std::min(tp.finestLevel, levels_ - 1);
        SetSize(c.srcSize, l0);
        SetSize(c.dstSize, cells_);
        SetSize(c.fieldSize, blocks_[finest]);
        c.p0[0] = 0.1f;
        c.u0[0] = tp.refineCandidates;
        c.u0[1] = tp.subpixelSteps;
        c.u0[2] = 1u + finest; // log2(cells per parent block)
        c.u1[0] = 1;
        c.u1[1] = 1; // skip per-sample reference weights away from the overlay (see fg_refine_cs)
        if (!CreateImmutableCb(c, cbRefine_)) return false;

        c = {};
        SetSize(c.srcSize, l0);
        SetSize(c.dstSize, cells_);
        c.p0[0] = 0.8f; // cross candidate must lower the rms by 20 %
        if (!CreateImmutableCb(c, cbCross_)) return false;

        c = {};
        SetSize(c.srcSize, l0);
        SetSize(c.dstSize, cells_);
        c.p0[0] = 0.75f; // tau0, flow px
        c.p0[1] = 0.1f;  // tauK
        c.p0[2] = static_cast<float>(frameSize_.width) / static_cast<float>(l0.width);
        c.p0[3] = 4.0f;  // textured: source std dev > 4 levels
        c.p1[0] = 0.35f; // relLo
        c.p1[1] = 0.8f;  // relHi
        c.p1[2] = 0.6f;  // bad match
        if (!CreateImmutableCb(c, cbConsistency_)) return false;

        c = {};
        SetSize(c.dstSize, cells_);
        c.p0[0] = 0.75f; // moving: |v| > 0.75 frame px
        c.p0[1] = 0.3f;  // with confidence > 0.3
        c.u0[1] = std::max(1u, static_cast<uint32_t>(std::lround(3.0f * s)));
        if (!CreateImmutableCb(c, cbMotionFrac_)) return false;

        c = {};
        SetSize(c.srcSize, frameSize_);
        SetSize(c.dstSize, frameSize_);
        SetSize(c.fieldSize, cells_);
        c.p0[0] = 2.0f;  // static: 3x3 max channel difference below 2 levels (soft to 6)
        c.p0[1] = 6.0f;
        c.p0[2] = 6.0f;  // textured: 3x3 luma range above 6 levels (soft to 16)
        c.p0[3] = 16.0f;
        c.p1[0] = 0.15f; // surroundings moving: fraction 0.15 .. 0.4
        c.p1[1] = 0.4f;
        c.p1[2] = activeFlowScale_ / static_cast<float>(kCell);
        if (!CreateImmutableCb(c, cbHud_)) return false;

        c = {};
        c.p0[0] = 0.6f;  // badHard
        c.p0[1] = 0.5f;  // histSoft
        c.p0[2] = 0.3f;  // badSoft
        c.p0[3] = 0.8f;  // histFlat
        c.p1[0] = 0.05f; // min textured fraction
        if (!CreateImmutableCb(c, cbSceneCut_)) return false;

        MSR_LOG_INFO(L"FrameGenerator: flow {}x{} (scale {}), {} levels, {}x{} cells, quality {}", l0.width, l0.height, s,
                     levels_, cells_.width, cells_.height, static_cast<int>(settings_.quality));
        return true;
    }

    // ---- passes ----

    void RunCS(ID3D11ComputeShader* cs, ID3D11Buffer* cb, std::initializer_list<ID3D11ShaderResourceView*> srvs,
               std::initializer_list<ID3D11UnorderedAccessView*> uavs, UINT x, UINT y, UINT z) {
        std::array<ID3D11ShaderResourceView*, 8> s{};
        std::array<ID3D11UnorderedAccessView*, 2> u{};
        std::copy(srvs.begin(), srvs.end(), s.begin());
        std::copy(uavs.begin(), uavs.end(), u.begin());
        ctx_->CSSetShader(cs, nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, &cb);
        ctx_->CSSetShaderResources(0, static_cast<UINT>(s.size()), s.data());
        ctx_->CSSetUnorderedAccessViews(0, static_cast<UINT>(u.size()), u.data(), nullptr);
        ctx_->CSSetSamplers(0, 1, linear_.GetAddressOf());
        ctx_->Dispatch(std::max(1u, x), std::max(1u, y), std::max(1u, z));
        s.fill(nullptr);
        u.fill(nullptr);
        ctx_->CSSetShaderResources(0, static_cast<UINT>(s.size()), s.data());
        ctx_->CSSetUnorderedAccessViews(0, static_cast<UINT>(u.size()), u.data(), nullptr);
    }

    void BuildPyramid(uint32_t slot) {
        auto& p = pyr_[slot];
        RunCS(csLuma_.Get(), cbLuma_.Get(), {frames_[slot].srv.Get()}, {p[0].uav.Get()}, DivUp(levelSize_[0].width, 8),
              DivUp(levelSize_[0].height, 8), 1);
        for (uint32_t l = 0; l + 1 < levels_; ++l)
            RunCS(csDownsample_.Get(), cbDown_[l].Get(), {p[l].srv.Get()}, {p[l + 1].uav.Get()},
                  DivUp(levelSize_[l + 1].width, 8), DivUp(levelSize_[l + 1].height, 8), 1);
        const UINT zero[4] = {0, 0, 0, 0};
        ctx_->ClearUnorderedAccessViewUint(hist_[slot].uav.Get(), zero);
        RunCS(csHistogram_.Get(), cbHist_.Get(), {p[histLevel_].srv.Get()}, {hist_[slot].uav.Get()},
              DivUp(levelSize_[histLevel_].width, 16), DivUp(levelSize_[histLevel_].height, 16), 1);
    }

    bool Analyse() {
        const TierParams tp = Tier(settings_.quality);
        const uint32_t prev = cur_ ^ 1u;
        auto& pp = pyr_[prev];
        auto& pc = pyr_[cur_];
        const uint32_t prevFinal = finalCur_;
        const uint32_t nextFinal = finalCur_ ^ 1u;
        ID3D11ShaderResourceView* temporal = prevFinalValid_ ? final_[prevFinal].srv.Get() : nullptr;

        ID3D11ShaderResourceView* hardwareField = nullptr;
#ifdef MOONSR_EXPERIMENTAL_HARDWARE_FLOW
        char backend[8]{};
        if (GetEnvironmentVariableA("MOONSR_FG_HARDWARE_FLOW",backend,sizeof(backend)) && backend[0]=='1') {
            if (settings_.flowScale != 1.0f) {
                MSR_LOG_ERROR(L"Experimental hardware flow requires flow scale 100%.");
                return false;
            }
            try {
                if (!hardwareFlow_) {
                    char hints[8]{};
                    const bool useHints=GetEnvironmentVariableA("MOONSR_FG_HW_HINTS",hints,sizeof(hints)) && hints[0]=='1';
                    hardwareFlow_=std::make_unique<HardwareFlowAnalysis>(dev_,frameSize_.width,frameSize_.height,DXGI_FORMAT_B8G8R8A8_UNORM,useHints);
                    MSR_LOG_INFO(L"FrameGenerator: experimental hardware flow active (full resolution, medium, temporal hints {})",useHints);
                }
                hardwareField=hardwareFlow_->Analyse(frames_[prev].tex.Get(),frames_[cur_].tex.Get(),pairId_);
            } catch (const std::exception& e) {
                const std::string message=e.what();
                MSR_LOG_ERROR(L"Experimental hardware flow failed: {}", std::wstring(message.begin(),message.end()));
                return false;
            }
        }
#endif

        const int finest = static_cast<int>(std::min(tp.finestLevel, levels_ - 1));
        if (!hardwareField) {
        for (int l = static_cast<int>(levels_) - 1; l >= finest; --l) {
            const bool coarsest = l + 1 == static_cast<int>(levels_);
            ID3D11Buffer* cb = prevFinalValid_ ? cbSearchT_[l].Get() : cbSearch_[l].Get();
            // Radius <= 4 uses the variant with smaller groupshared arrays (higher occupancy).
            const uint32_t radius = LevelRadius(tp, static_cast<uint32_t>(l), coarsest);
            ID3D11ComputeShader* search = tp.census ? (radius <= 4 ? csSearchR4_.Get() : csSearch_.Get())
                                                   : (radius <= 4 ? csSearchR4Fast_.Get() : csSearchFast_.Get());
            RunCS(search, cb,
                  {pp[l].srv.Get(), pc[l].srv.Get(), coarsest ? nullptr : fieldB_[l + 1].srv.Get(), temporal, hud_.srv.Get()},
                  {fieldA_[l].uav.Get()}, blocks_[l].width, blocks_[l].height, 2);
            RunCS(csRegularize_.Get(), cbRegularize_[l].Get(), {pp[l].srv.Get(), pc[l].srv.Get(), fieldA_[l].srv.Get(), hud_.srv.Get()},
                  {fieldB_[l].uav.Get()}, blocks_[l].width, blocks_[l].height, 2);
            if ((tp.propagation == 1 && l >= 1) || tp.propagation == 2) {
                // Long-range propagation. Balanced skips level 0 (4x the blocks of level 1; the hierarchy carries the
                // propagated vectors down); measured on hud_pan, level-0 propagation is worth ~0.4 dB for ~0.2 ms.
                // B -> A (step 4), A -> B (step 2): the level's result stays in fieldB_.
                RunCS(csRegularize_.Get(), cbProp4_[l].Get(), {pp[l].srv.Get(), pc[l].srv.Get(), fieldB_[l].srv.Get(), hud_.srv.Get()},
                      {fieldA_[l].uav.Get()}, blocks_[l].width, blocks_[l].height, 2);
                RunCS(csRegularize_.Get(), cbProp2_[l].Get(), {pp[l].srv.Get(), pc[l].srv.Get(), fieldA_[l].srv.Get(), hud_.srv.Get()},
                      {fieldB_[l].uav.Get()}, blocks_[l].width, blocks_[l].height, 2);
            }
        }
        const UINT gx = DivUp(cells_.width, 8), gy = DivUp(cells_.height, 8);
        RunCS(csRefine_.Get(), cbRefine_.Get(), {pp[0].srv.Get(), pc[0].srv.Get(), fieldB_[finest].srv.Get(), hud_.srv.Get()},
              {refined_.uav.Get()}, gx, gy, 2);
        const UINT zero[4] = {0, 0, 0, 0};
        // Cross-direction candidates (see fg_cross_cs.hlsl).
        ctx_->ClearUnorderedAccessViewUint(crossKeys_.uav.Get(), zero);
        RunCS(csCrossSplat_.Get(), cbCross_.Get(), {nullptr, nullptr, refined_.srv.Get()}, {crossKeys_.uav.Get()}, gx, gy, 2);
        RunCS(csCross_.Get(), cbCross_.Get(), {pp[0].srv.Get(), pc[0].srv.Get(), refined_.srv.Get(), crossKeys_.srv.Get()},
              {nullptr, refined2_.uav.Get()}, gx, gy, 2);
        }
        const UINT gx = DivUp(cells_.width, 8), gy = DivUp(cells_.height, 8);
        const UINT zero[4] = {0, 0, 0, 0};
        ctx_->ClearUnorderedAccessViewUint(stats_.uav.Get(), zero);
        RunCS(csConsistency_.Get(), cbConsistency_.Get(), {hardwareField ? hardwareField : refined2_.srv.Get()},
              {final_[nextFinal].uav.Get(), stats_.uav.Get()}, gx, gy, 2);
        finalCur_ = nextFinal;
        prevFinalValid_ = true;

        if (settings_.hudProtection) {
            RunCS(csMotionFrac_.Get(), cbMotionFrac_.Get(), {final_[finalCur_].srv.Get()}, {motionFrac_.uav.Get()}, gx, gy, 1);
            RunCS(csHudMask_.Get(), cbHud_.Get(), {frames_[prev].srv.Get(), frames_[cur_].srv.Get(), motionFrac_.srv.Get()},
                  {hud_.uav.Get()}, DivUp(frameSize_.width, 16), DivUp(frameSize_.height, 16), 1);
        }
        RunCS(csSceneCut_.Get(), cbSceneCut_.Get(), {hist_[prev].srv.Get(), hist_[cur_].srv.Get()}, {stats_.uav.Get()}, 1, 1, 1);

        // Asynchronous statistics readback: never waited on.
        auto& slot = staging_[stagingNext_];
        ctx_->CopyResource(slot.buf.Get(), stats_.buf.Get());
        ctx_->End(stagingDone_[stagingNext_].Get());
        pendingPair_[stagingNext_] = pairId_;
        stagingNext_ = (stagingNext_ + 1) % kReadbackDepth;
        return true;
    }

    void PollStats() const {
        // Oldest first so the newest completed result wins.
        for (uint32_t k = 0; k < kReadbackDepth; ++k) {
            const uint32_t i = (stagingNext_ + k) % kReadbackDepth;
            if (pendingPair_[i] == 0) continue;
            // Completion is detected with an event query placed after the copy (GetData never blocks with
            // DONOTFLUSH); only then is the buffer mapped, so the Map cannot stall. Map(DO_NOT_WAIT) alone was
            // measured to keep returning WAS_STILL_DRAWING on an AMD iGPU long after the copy had completed.
            BOOL done = FALSE;
            const HRESULT q = ctx_->GetData(stagingDone_[i].Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (q != S_OK || !done) continue;
            D3D11_MAPPED_SUBRESOURCE map;
            const HRESULT hr = ctx_->Map(staging_[i].buf.Get(), 0, D3D11_MAP_READ, 0, &map);
            if (FAILED(hr)) {
                MSR_LOG_WARN(L"FrameGenerator: stats readback Map failed: {}", HrToString(hr));
                pendingPair_[i] = 0;
                continue;
            }
            uint32_t v[kStatSlots];
            memcpy(v, map.pData, sizeof(v));
            ctx_->Unmap(staging_[i].buf.Get(), 0);
            if (pendingPair_[i] > statsPairId_) {
                statsPairId_ = pendingPair_[i];
                statsCut_ = v[kStatCut] != 0;
                float f;
                memcpy(&f, &v[kStatMeanConf], 4);
                statsMeanConf_ = std::isfinite(f) ? f : 0.0f;
                memcpy(&f, &v[kStatMeanMotion], 4);
                statsMeanMotion_ = std::isfinite(f) ? f : 0.0f;
                statsHistDist_ = v[kStatHistDist] / 1000.0f;
                statsBadFrac_ = v[kStatBadFrac] / 1000.0f;
            }
            pendingPair_[i] = 0;
        }
    }

    ID3D11Device1* dev_ = nullptr;
    ID3D11DeviceContext1* ctx_ = nullptr;

    ComPtr<ID3D11ComputeShader> csCrossSplat_, csCross_;
    ComPtr<ID3D11ComputeShader> csLuma_, csDownsample_, csHistogram_, csSearch_, csSearchR4_, csSearchFast_, csSearchR4Fast_, csRegularize_, csRefine_, csConsistency_,
        csMotionFrac_, csHudMask_, csSceneCut_, csSplat_, csResolve_, csPixsel_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psInterp_, psAa_;
    ComPtr<ID3D11SamplerState> linear_;
    ComPtr<ID3D11RasterizerState> raster_;

    ComPtr<ID3D11Buffer> cbCross_;
    ComPtr<ID3D11Buffer> cbInterp_, cbLuma_, cbHist_, cbRefine_, cbConsistency_, cbMotionFrac_, cbHud_, cbSceneCut_;
    std::array<ComPtr<ID3D11Buffer>, kMaxLevels> cbDown_, cbSearch_, cbSearchT_, cbRegularize_, cbProp4_, cbProp2_;

    std::array<Tex, 2> frames_;
    std::array<std::array<Tex, kMaxLevels>, 2> pyr_;
    std::array<Buf, 2> hist_;
    std::array<Tex, kMaxLevels> fieldA_, fieldB_;
    Tex refined_, refined2_, crossKeys_, motionFrac_, keys_, tfield_, hud_;
    Tex sel_, selAux_, gen_, guide_; // per generated frame: hypothesis selection (half res), colour + AA guide
    Size selSize_{};
    std::array<Tex, 2> final_;
    Buf stats_;
    std::array<Buf, kReadbackDepth> staging_;
    std::array<ComPtr<ID3D11Query>, kReadbackDepth> stagingDone_;

    FrameGenSettings settings_{};
    FrameGenDebugView debugView_ = FrameGenDebugView::None;
    float fallbackMode_ = 0.0f;
    bool flowDirty_ = true;
    float activeFlowScale_ = 0.5f;
    bool formatWarned_ = false;

    Size frameSize_{};
    std::array<Size, kMaxLevels> levelSize_{}, blocks_{};
    Size cells_{};
    uint32_t levels_ = 0, meanLevel_ = 0, histLevel_ = 0;

    uint32_t cur_ = 0, histCount_ = 0, finalCur_ = 0;
    bool prevFinalValid_ = false;
    std::array<int64_t, 2> qpc_{};
    uint64_t pairId_ = 0;
    FramePairInfo info_{};

    uint32_t stagingNext_ = 0;

#ifdef MOONSR_EXPERIMENTAL_HARDWARE_FLOW
    std::unique_ptr<HardwareFlowAnalysis> hardwareFlow_;
#endif
    mutable std::array<uint64_t, kReadbackDepth> pendingPair_{};
    mutable uint64_t statsPairId_ = 0;
    mutable bool statsCut_ = false;
    mutable float statsMeanConf_ = 0, statsMeanMotion_ = 0, statsHistDist_ = 0, statsBadFrac_ = 0;
};

} // namespace

std::unique_ptr<FrameGenerator> FrameGenerator::Create(GpuDevice& device, std::wstring& error) {
    auto p = std::make_unique<FrameGeneratorImpl>();
    if (!p->Init(device, error)) return nullptr;
    return p;
}

} // namespace moonsr
