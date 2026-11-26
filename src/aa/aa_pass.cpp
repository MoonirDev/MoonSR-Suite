// Anti-aliasing, spatial tier (T1). Owner: upscaling-engineer (algorithms) with shader-engineer (HLSL under
// shaders/aa/). Clean-room implementation from the published description of the technique
// (documentation/research/anti_aliasing.md, Part 2); no third-party shader code was read or used.
//
// One full-screen pass: detect a luminance step, orient it from the local gradient, average across it with a
// slope-following offset, and blend by a sub-pixel weight (aa_spatial_ps.hlsl). A tiny compute pass samples a 128x128 grid
// of the source and counts aliased edges (aa_stats_cs.hlsl); its counters reach the CPU a few frames later through a
// staging ring and drive the "already anti-aliased" reduction and the status text.
//
// Missing: the Directional (pattern/area) and Temporal tiers. They are accepted by the settings and reported as a
// fallback to Spatial; the research note explains why they are separate milestones.
//
// Threading: pipeline thread only (owner of the GpuDevice immediate context). No per-frame heap allocation: the
// sampler, constant buffers, query sets, counters and staging buffers are created once in Create().
#include "aa/aa_pass.h"

#include "core/log.h"

#include "g_cs_aa_stats.h"
#include "g_ps_aa_spatial.h"
#include "g_vs_fullscreen.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace moonsr {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kQuerySets = 6;
constexpr uint32_t kSamples = 8;
constexpr uint32_t kReadbackSlots = 4;
constexpr uint32_t kStatsGrid = 128; // samples per axis (16x16 groups of 8x8 threads)

// Mirrors AaConstants in shaders/aa/aa_common.hlsli. Keep in sync.
struct AaConstants {
    float texSize[2];
    float texelSize[2];
    float contentOrigin[2];
    float contentSize[2];
    float strength;
    float edgeThreshold;
    float edgeThresholdMin;
    float outputScale;
    uint32_t quality;
    uint32_t protectText;
    uint32_t debug;
    float pad;
};
static_assert(sizeof(AaConstants) == 64, "AaConstants must match the HLSL constant buffer");

// Mirrors AaStatsConstants in shaders/aa/aa_stats_cs.hlsl.
struct AaStatsConstants {
    float texSize[2];
    float texelSize[2];
    float contentOrigin[2];
    float contentSize[2];
    float edgeThreshold;
    float edgeThresholdMin;
    uint32_t grid[2];
};
static_assert(sizeof(AaStatsConstants) == 48, "AaStatsConstants must match the HLSL constant buffer");

float ClampFinite(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

AaSettings Sanitize(const AaSettings& in) {
    const AaSettings def;
    AaSettings s = in;
    if (static_cast<uint32_t>(s.mode) > static_cast<uint32_t>(AaMode::Temporal)) s.mode = AaMode::Off;
    if (static_cast<uint32_t>(s.quality) > static_cast<uint32_t>(AaQuality::Quality)) s.quality = def.quality;
    if (static_cast<uint32_t>(s.debugView) > static_cast<uint32_t>(AaDebugView::Split)) s.debugView = AaDebugView::None;
    s.strength = ClampFinite(s.strength, 0.0f, 1.0f, def.strength);
    s.edgeThreshold = ClampFinite(s.edgeThreshold, 0.02f, 0.5f, def.edgeThreshold);
    s.edgeThresholdMin = ClampFinite(s.edgeThresholdMin, 0.0f, 0.2f, def.edgeThresholdMin);
    return s;
}


struct QuerySet {
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;
};

struct QueryRing {
    QuerySet sets[kQuerySets];
    double samples[kSamples]{};
    uint32_t sampleNext = 0, sampleCount = 0;
    uint32_t next = 0;
};

double Median(const QueryRing& ring) {
    if (ring.sampleCount == 0) return 0.0;
    std::array<double, kSamples> v{};
    for (uint32_t i = 0; i < ring.sampleCount; ++i) v[i] = ring.samples[i];
    std::sort(v.begin(), v.begin() + ring.sampleCount);
    return v[ring.sampleCount / 2];
}

class AaPassImpl final : public AaPass {
public:
    static std::unique_ptr<AaPass> Create(GpuDevice& device, std::wstring& error);

    void SetSettings(const AaSettings& settings) override { settings_ = Sanitize(settings); }

    bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
               const RectI& contentInDst, float outputPixelScale) override;

    void Reset() override {
        // No history in T1; only the measurements need a clean start. A scene cut invalidates the coverage estimate,
        // so the next reading replaces it instead of being averaged into the old one.
        for (QuerySet& q : timing_.sets) q.pending = false;
        timing_.sampleCount = timing_.sampleNext = 0;
        readbackNext_ = 0;
        for (bool& pending : readbackPending_) pending = false;
        coverageEma_ = -1.0f;
    }

    AaStats Stats() const override;

private:
    explicit AaPassImpl(GpuDevice& device) : device_(device) {}

    bool Init(std::wstring& error);
    void CollectTimings();
    void CollectStats();

    float EffectiveStrength() const { return settings_.strength; }

    GpuDevice& device_;
    AaSettings settings_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11ComputeShader> csStats_;
    ComPtr<ID3D11Buffer> cbDraw_, cbStats_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> counter_, counterStaging_[kReadbackSlots];
    ComPtr<ID3D11UnorderedAccessView> counterUav_;
    bool readbackPending_[kReadbackSlots]{};
    uint32_t readbackNext_ = 0;

    QueryRing timing_;
    bool timingAvailable_ = false;
    uint64_t appliedFrames_ = 0;
    float coverageEma_ = -1.0f; // < 0: no reading yet
    bool logOnce_ = false;
};

bool AaPassImpl::Init(std::wstring& error) {
    ID3D11Device* d = device_.Device();
    HRESULT hr = d->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = d->CreatePixelShader(g_ps_aa_spatial, sizeof(g_ps_aa_spatial), nullptr, &ps_);
    if (SUCCEEDED(hr)) hr = d->CreateComputeShader(g_cs_aa_stats, sizeof(g_cs_aa_stats), nullptr, &csStats_);
    if (FAILED(hr)) {
        error = L"Anti-aliasing shaders could not be created: " + HrToString(hr);
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = sizeof(AaConstants);
    hr = d->CreateBuffer(&cb, nullptr, &cbDraw_);
    cb.ByteWidth = sizeof(AaStatsConstants);
    if (SUCCEEDED(hr)) hr = d->CreateBuffer(&cb, nullptr, &cbStats_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (SUCCEEDED(hr)) hr = d->CreateSamplerState(&sd, &sampler_);

    // Two counters (aliased-edge samples, samples taken), copied to a small staging ring a few frames later.
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 2 * sizeof(uint32_t);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(uint32_t);
    if (SUCCEEDED(hr)) hr = d->CreateBuffer(&bd, nullptr, &counter_);
    if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(counter_.Get(), nullptr, &counterUav_);
    D3D11_BUFFER_DESC st{};
    st.ByteWidth = 2 * sizeof(uint32_t);
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (ComPtr<ID3D11Buffer>& b : counterStaging_)
        if (SUCCEEDED(hr)) hr = d->CreateBuffer(&st, nullptr, &b);

    if (FAILED(hr)) {
        error = L"Anti-aliasing resources could not be created: " + HrToString(hr);
        return false;
    }

    timingAvailable_ = true;
    for (QuerySet& q : timing_.sets) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        hr = d->CreateQuery(&qd, &q.disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (SUCCEEDED(hr)) hr = d->CreateQuery(&qd, &q.begin);
        if (SUCCEEDED(hr)) hr = d->CreateQuery(&qd, &q.end);
        if (FAILED(hr)) {
            timingAvailable_ = false;
            break;
        }
    }
    if (!timingAvailable_)
        MSR_LOG_WARN(L"Anti-aliasing: GPU timing queries unavailable; the cost will read 0 ms");
    return true;
}

bool AaPassImpl::Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
                       const RectI& contentInDst, float outputPixelScale) {
    if (!src || !dst || srcSize.Empty() || dstSize.Empty()) return false;
    if (srcSize.width != dstSize.width || srcSize.height != dstSize.height) {
        if (!logOnce_) {
            logOnce_ = true;
            MSR_LOG_ERROR(L"Anti-aliasing needs matching source and destination sizes ({}x{} vs {}x{}); skipped",
                          srcSize.width, srcSize.height, dstSize.width, dstSize.height);
        }
        return false;
    }

    RectI content = contentInDst;
    if (content.Empty()) content = {0, 0, static_cast<int32_t>(srcSize.width), static_cast<int32_t>(srcSize.height)};
    content.left = std::clamp(content.left, 0, static_cast<int32_t>(srcSize.width) - 1);
    content.top = std::clamp(content.top, 0, static_cast<int32_t>(srcSize.height) - 1);
    content.right = std::clamp(content.right, content.left + 1, static_cast<int32_t>(srcSize.width));
    content.bottom = std::clamp(content.bottom, content.top + 1, static_cast<int32_t>(srcSize.height));

    ID3D11DeviceContext1* ctx = device_.Context();
    const float strength = EffectiveStrength();

    AaConstants c{};
    c.texSize[0] = static_cast<float>(srcSize.width);
    c.texSize[1] = static_cast<float>(srcSize.height);
    c.texelSize[0] = 1.0f / static_cast<float>(srcSize.width);
    c.texelSize[1] = 1.0f / static_cast<float>(srcSize.height);
    c.contentOrigin[0] = static_cast<float>(content.left);
    c.contentOrigin[1] = static_cast<float>(content.top);
    c.contentSize[0] = static_cast<float>(content.Width());
    c.contentSize[1] = static_cast<float>(content.Height());
    c.strength = strength;
    c.edgeThreshold = settings_.edgeThreshold;
    c.edgeThresholdMin = settings_.edgeThresholdMin;
    c.outputScale = ClampFinite(outputPixelScale, 0.25f, 16.0f, 1.0f);
    c.quality = static_cast<uint32_t>(settings_.quality);
    c.protectText = settings_.protectText ? 1u : 0u;
    c.debug = static_cast<uint32_t>(settings_.debugView);

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(cbDraw_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) {
        if (!logOnce_) {
            logOnce_ = true;
            MSR_LOG_ERROR(L"Anti-aliasing constant buffer map failed: {}", HrToString(hr));
        }
        return false;
    }
    std::memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(cbDraw_.Get(), 0);

    AaStatsConstants sc{};
    sc.texSize[0] = c.texSize[0];
    sc.texSize[1] = c.texSize[1];
    sc.texelSize[0] = c.texelSize[0];
    sc.texelSize[1] = c.texelSize[1];
    sc.contentOrigin[0] = c.contentOrigin[0];
    sc.contentOrigin[1] = c.contentOrigin[1];
    sc.contentSize[0] = c.contentSize[0];
    sc.contentSize[1] = c.contentSize[1];
    sc.edgeThreshold = settings_.edgeThreshold;
    sc.edgeThresholdMin = settings_.edgeThresholdMin;
    sc.grid[0] = sc.grid[1] = kStatsGrid;
    hr = ctx->Map(cbStats_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(hr)) return false;
    std::memcpy(m.pData, &sc, sizeof(sc));
    ctx->Unmap(cbStats_.Get(), 0);

    QuerySet* q = nullptr;
    if (timingAvailable_ && !timing_.sets[timing_.next].pending) {
        q = &timing_.sets[timing_.next];
        ctx->Begin(q->disjoint.Get());
        ctx->End(q->begin.Get());
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(dstSize.width), static_cast<float>(dstSize.height),
                            0.0f, 1.0f};
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
    ctx->PSSetConstantBuffers(0, 1, cbDraw_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Aliased-edge coverage for the already-AA detector. One small dispatch per frame; the counters are read back a
    // few frames later without waiting.
    const UINT zeros[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(counterUav_.Get(), zeros);
    ctx->CSSetShader(csStats_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, counterUav_.GetAddressOf(), nullptr);
    ctx->CSSetConstantBuffers(0, 1, cbStats_.GetAddressOf());
    ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->Dispatch(kStatsGrid / 8, kStatsGrid / 8, 1);
    ID3D11ShaderResourceView* nullCsSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullCsSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    if (q) {
        ctx->End(q->end.Get());
        ctx->End(q->disjoint.Get());
        q->pending = true;
        timing_.next = (timing_.next + 1) % kQuerySets;
    }

    // Hand the counters to the staging ring, then try to read an older slot.
    if (!readbackPending_[readbackNext_]) {
        ctx->CopyResource(counterStaging_[readbackNext_].Get(), counter_.Get());
        readbackPending_[readbackNext_] = true;
        readbackNext_ = (readbackNext_ + 1) % kReadbackSlots;
    }
    CollectTimings();
    CollectStats();
    ++appliedFrames_;
    return true;
}

void AaPassImpl::CollectTimings() {
    if (!timingAvailable_) return;
    ID3D11DeviceContext1* ctx = device_.Context();
    for (QuerySet& q : timing_.sets) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(q.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(q.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        q.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        timing_.samples[timing_.sampleNext] =
            static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        timing_.sampleNext = (timing_.sampleNext + 1) % kSamples;
        timing_.sampleCount = std::min(timing_.sampleCount + 1, kSamples);
    }
}

void AaPassImpl::CollectStats() {
    ID3D11DeviceContext1* ctx = device_.Context();
    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
        if (!readbackPending_[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(counterStaging_[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) {
            readbackPending_[i] = false;
            continue;
        }
        const auto* counts = static_cast<const uint32_t*>(m.pData);
        const uint32_t edges = counts[0], total = counts[1];
        ctx->Unmap(counterStaging_[i].Get(), 0);
        readbackPending_[i] = false;
        if (total == 0) continue;
        const float coverage = static_cast<float>(edges) / static_cast<float>(total);
        coverageEma_ = coverageEma_ < 0.0f ? coverage : coverageEma_ + (coverage - coverageEma_) * 0.15f;
    }
}

AaStats AaPassImpl::Stats() const {
    AaStats st;
    st.active = settings_.mode != AaMode::Off && appliedFrames_ > 0;
    st.modeInEffect = settings_.mode == AaMode::Off ? AaMode::Off : AaMode::Spatial;
    st.edgeCoverage = coverageEma_ < 0.0f ? 0.0f : coverageEma_;
    st.strengthInEffect = EffectiveStrength();
    st.gpuMs = Median(timing_);
    if (settings_.mode == AaMode::Directional || settings_.mode == AaMode::Temporal)
        st.reason = L"The selected anti-aliasing mode is not built yet; the spatial filter is used.";
    return st;
}

std::unique_ptr<AaPass> AaPassImpl::Create(GpuDevice& device, std::wstring& error) {
    auto pass = std::unique_ptr<AaPassImpl>(new AaPassImpl(device));
    if (!pass->Init(error)) return nullptr;
    MSR_LOG_INFO(L"Anti-aliasing (spatial) ready on '{}'", device.Info().name);
    return pass;
}

} // namespace

std::unique_ptr<AaPass> AaPass::Create(GpuDevice& device, std::wstring& error) {
    return AaPassImpl::Create(device, error);
}

} // namespace moonsr
