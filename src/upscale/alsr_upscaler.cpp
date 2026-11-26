// MoonSR temporal upscaler (internal name Moon ALSR) - image-based temporal reconstruction with a MoonSR Spatial seed.
// Owner: upscaling-engineer. Contract: alsr_upscaler.h (lead). Direct3D 11 FL 11_0, Shader Model 5.0 only.
//
// Per Render (one real frame), all on the GPU, no CPU readback, no allocation in steady state:
//   0. alsr_prep       (source res)  working-space copy (linear light) + blurred luma of the current frame
//   1. alsr_cellrefine (cell res)    Lucas-Kanade refinement + velocity filter of the estimator's curr -> prev cell vectors
//   2. alsr_motion     (source res)  per-pixel motion: smooth interpolation inside coherent motion, selection by matching
//                                  at motion boundaries; visibility, confidence, HUD -> zero motion
//   3. alsr_reproject  (output res)  history reprojection (Catmull-Rom), YCoCg box clip, confidence / disocclusion gate;
//                                  writes trust x history and a depth mask "trust < 1"
//   3b. alsr_seedblend (output res)  ONLY where trust < 1 (early depth test): adds (1 - trust) x the single-frame
//                                  reconstruction ("seed" = MoonSR Spatial, alsr_seed.hlsli)
//   4. alsr_residual   (source res)  residual of each new sample against its prediction from the reprojected history
//   5. alsr_update     (output res)  history += gain * interpolated residual (recursive back-projection); gain 0 on fresh
//                                  pixels (restart = single-frame reconstruction) and on zero-motion pixels unless the
//                                  input changed in place (no new sample phase = no new information); also writes an
//                                  RGBA8 gamma copy of the history (hardware sRGB encode)
//   6. alsr_final      (output res)  adaptive sharpening of the gamma copy, debug views -> destination
//   7. alsr_refy + alsr_refluma (source res)  registration reference (blurred history luma) for the next frame
// Passes 1-2 are skipped / trivial when there is no valid history. The shader headers document each pass exactly.
// RenderSingle: alsr_seed (seed of every pixel), plus alsr_final when sharpening is enabled.
// There is no CPU readback and no state on disk. There is no time-based fallback of any kind: the work per frame
// never depends on how long earlier frames took.
//
// Memory (output W x H, source w x h): history RGBA16F + prediction RGBA16F (16 B/px), history gamma copy RGBA8, seed
// mask D16, trust R8, seed RGBA8 (RenderSingle / debug) = 27 B per output pixel; per-pixel motion
// RGBA16F (8), two working copies RGBA16F (16), residual RGBA16F (8), reference R16F + R32F (6), overlay evidence 2 x R8,
// layer RG8 (4) = 42 B per source pixel; two refined cell fields RGBA32F (negligible).
// 1280x720 -> 1920x1080: about 90 MiB; 1920x1080 -> 2560x1440: about 178 MiB; 1920x1080 -> 3840x2160: about 297 MiB.
//
// Development tuning (never needed in normal use): environment variable MOONSR_ALSR_TUNE, a comma-separated list
// of name=value pairs read once in Create (names: see Tune below). Logged when present.
#include "upscale/alsr_upscaler.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "g_vs_fullscreen.h"
#include "g_ps_alsr_prep.h"
#include "g_ps_alsr_prep_hud.h"
#include "g_ps_alsr_motion_affine.h"
#include "g_ps_alsr_motion_pixel.h"
#include "g_ps_alsr_motion_affine_pixel.h"
#include "g_cs_alsr_cellrefine.h"
#include "g_ps_alsr_motion.h"
#include "g_ps_alsr_reproject.h"
#include "g_ps_alsr_reproject_fast.h"
#include "g_ps_alsr_reproject_robust.h"
#include "g_ps_alsr_update_robust.h"
#include "g_ps_alsr_update_robust_direct.h"
#include "g_ps_alsr_residual.h"
#include "g_ps_alsr_residual_adaptive.h"
#include "g_ps_alsr_update.h"
#include "g_ps_alsr_update_direct.h"
#include "g_ps_alsr_accumulate.h"
#include "g_ps_alsr_accumulate_direct.h"
#include "g_ps_alsr_accumulate_point.h"
#include "g_ps_alsr_accumulate_point_direct.h"
#include "g_ps_alsr_point_residual.h"
#include "g_ps_alsr_point_residual_direct.h"
#include "g_ps_alsr_guarded_point.h"
#include "g_ps_alsr_guarded_point_direct.h"
#include "g_ps_alsr_point_fixed_count.h"
#include "g_ps_alsr_point_fixed_count_direct.h"
#include "g_ps_alsr_point_phase.h"
#include "g_ps_alsr_point_phase_direct.h"
#include "g_ps_alsr_accumulate_cubic.h"
#include "g_ps_alsr_accumulate_cubic_direct.h"
#include "g_ps_alsr_update_gated.h"
#include "g_ps_alsr_update_gated_direct.h"
#include "g_ps_alsr_final.h"
#include "g_ps_alsr_lite.h"
#include "g_ps_alsr_lite_seed.h"
#include "g_cs_alsr_area_tile.h"
#include "g_cs_alsr_area_padded.h"
#include "g_cs_alsr_area_cached_hull.h"
#include "g_cs_alsr_area_adaptive.h"
#include "g_cs_alsr_area_robust.h"
#include "g_ps_alsr_area_resolve.h"
#include "g_ps_alsr_refluma.h"
#include "g_cs_alsr_reference.h"
#include "g_cs_alsr_sampling.h"
#include "g_cs_alsr_sampling_seed.h"
#include "g_ps_alsr_refy.h"
#include "g_ps_alsr_seed.h"
#include "g_ps_alsr_seedblend.h"
#include "g_ps_alsr_seedblend_static.h"
#include "g_ps_alsr_reproject_static.h"
#include "g_ps_alsr_seed_lanczos.h"
#include "g_ps_alsr_seedblend_lanczos.h"
#include "g_ps_alsr_seed_directional.h"
#include "g_ps_alsr_seedblend_directional.h"
#include "g_ps_alsr_seed_covariance.h"
#include "g_ps_alsr_seedblend_covariance.h"

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

// Mirror of `cbuffer AlsrConstants` in shaders/upscale/alsr_common.hlsli.
struct AlsrConstants {
    float in[4];
    float out[4];
    float scale[4];
    float cell[4];
    float p0[4];
    float p1[4];
    float p2[4];
    float p3[4];
    uint32_t u0[4];
    uint32_t u1[4];
    float p4[4];
    uint32_t u2[4];
};
static_assert(sizeof(AlsrConstants) == 192);

// Mirror of `cbuffer SeedConstants` (b1) in shaders/upscale/alsr_seed.hlsli.
struct SeedConstants {
    float src[4];
    float dst[4];
};
static_assert(sizeof(SeedConstants) == 32);

// Algorithm parameters. Defaults are the values chosen by measurement (see the report / BENCHMARKS.md).
struct Tune {
    float clipK = 2.0f;        // clip box shrink in motion = clipKMotion / clipK (1 = static)
    float clipKMotion = 1.0f;  // ... reached at `motionTight` source px per frame
    float motionTight = 4.0f;  // source px
    float maxCount = 16.0f;    // accumulated count cap
    float gMin = -1.0f;        // < 0: from AlsrSettings::stability, clamp(1.4 - 1.25 stability, 0.12, 1)
    float confLo = 0.1f, confHi = 0.5f;    // trust ramp on the per-pixel confidence
    float matchLo = 0.06f, matchHi = 0.2f; // per-pixel match error ramp (gamma luma, 0..1)
    float boostLo = 0.03f, boostHi = 0.12f; // gain boost on |residual luma| (working space)
    float tau0 = 1.5f, tauK = 0.1f;        // visibility tolerance, frame px
    float spread = 0.5f;       // smooth-candidate threshold, frame px (alsr_motion)
    float agree = 0.5f;        // per-pixel motion agreement for bilinear motion, source px (alsr_reproject)
    float velMin = 0.15f, velLo = 0.5f, velHi = 1.5f; // velocity filter (alsr_cellrefine)
    float margin = 0.25f;      // hull margin (fraction of the 3x3 range) for the clip and the anti-ringing clamp
    uint32_t lkIters = 2; // Gauss-Newton steps of the cell refinement (alsr_cellrefine_cs); 4 measured equal within +-0.11 dB
    uint32_t linear = 1; // accumulate in linear light (measured better than gamma on both input kinds)
    uint32_t variant = 0; // development flags (bits), see the shaders: 1, 4, 8, 16, 32, 64, 128, 256, 1024
};

Tune ReadTune() {
    Tune t;
    char buf[1024] = {};
    const DWORD n = GetEnvironmentVariableA("MOONSR_ALSR_TUNE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return t;
    std::string s(buf);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string kv = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq);
        const float v = static_cast<float>(std::atof(kv.c_str() + eq + 1));
        if (k == "clipK") t.clipK = v;
        else if (k == "clipKMotion") t.clipKMotion = v;
        else if (k == "motionTight") t.motionTight = v;
        else if (k == "maxCount") t.maxCount = v;
        else if (k == "gMin") t.gMin = v;
        else if (k == "confLo") t.confLo = v;
        else if (k == "confHi") t.confHi = v;
        else if (k == "matchLo") t.matchLo = v;
        else if (k == "matchHi") t.matchHi = v;
        else if (k == "boostLo") t.boostLo = v;
        else if (k == "boostHi") t.boostHi = v;
        else if (k == "tau0") t.tau0 = v;
        else if (k == "tauK") t.tauK = v;
        else if (k == "spread") t.spread = v;
        else if (k == "agree") t.agree = v;
        else if (k == "velMin") t.velMin = v;
        else if (k == "velLo") t.velLo = v;
        else if (k == "velHi") t.velHi = v;
        else if (k == "margin") t.margin = v;
        else if (k == "lkIters") t.lkIters = static_cast<uint32_t>(v);
        else if (k == "linear") t.linear = v != 0.0f ? 1u : 0u;
        else if (k == "variant") t.variant = static_cast<uint32_t>(v);
    }
    MSR_LOG_INFO(L"AlsrUpscaler: development tuning MOONSR_ALSR_TUNE={}", std::wstring(s.begin(), s.end()));
    return t;
}

struct RtTex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11UnorderedAccessView> uav; // only for textures created with uav = true
    void Reset() {
        tex.Reset();
        srv.Reset();
        rtv.Reset();
        uav.Reset();
    }
};

class AlsrUpscalerImpl final : public AlsrUpscaler {
public:
    bool Init(GpuDevice& device, std::wstring& error) {
        dev_ = device.Device();
        ctx_ = device.Context();
        if (dev_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            error = L"AlsrUpscaler: feature level 11_0 required";
            return false;
        }
        tune_ = ReadTune();
        HRESULT hr = dev_->CreateVertexShader(g_vs_fullscreen, sizeof(g_vs_fullscreen), nullptr, &vs_);
        if (FAILED(hr)) return Fail(error, L"CreateVertexShader", hr);
        struct PsDesc {
            const BYTE* data;
            size_t size;
            ComPtr<ID3D11PixelShader>* out;
            const wchar_t* name;
        };
        const PsDesc ps[] = {
            {g_ps_alsr_prep_hud, sizeof(g_ps_alsr_prep_hud), &psPrepHud_, L"persistent HUD"},
            {g_ps_alsr_motion_pixel, sizeof(g_ps_alsr_motion_pixel), &psMotionPixel_, L"pixel refinement"},
            {g_ps_alsr_motion_affine_pixel, sizeof(g_ps_alsr_motion_affine_pixel), &psMotionAffinePixel_, L"affine and pixel refinement"},
            {g_ps_alsr_motion_affine, sizeof(g_ps_alsr_motion_affine), &psMotionAffine_, L"affine motion"},
            {g_ps_alsr_prep, sizeof(g_ps_alsr_prep), &psPrep_, L"prep"},
            {g_ps_alsr_motion, sizeof(g_ps_alsr_motion), &psMotion_, L"motion"},
            {g_ps_alsr_reproject, sizeof(g_ps_alsr_reproject), &psReproject_, L"reproject"},
            {g_ps_alsr_reproject_fast, sizeof(g_ps_alsr_reproject_fast), &psReprojectFast_, L"fast reproject"},
            {g_ps_alsr_reproject_robust, sizeof(g_ps_alsr_reproject_robust), &psReprojectRobust_, L"robust reproject"},
            {g_ps_alsr_update_robust, sizeof(g_ps_alsr_update_robust), &psUpdateRobust_, L"robust update"},
            {g_ps_alsr_update_robust_direct, sizeof(g_ps_alsr_update_robust_direct), &psUpdateRobustDirect_, L"robust update (direct)"},
            {g_ps_alsr_residual, sizeof(g_ps_alsr_residual), &psResidual_, L"residual"},
            {g_ps_alsr_residual_adaptive, sizeof(g_ps_alsr_residual_adaptive), &psResidualAdaptive_, L"adaptive measurement residual"},
            {g_ps_alsr_update, sizeof(g_ps_alsr_update), &psUpdate_, L"update"},
            {g_ps_alsr_update_direct, sizeof(g_ps_alsr_update_direct), &psUpdateDirect_, L"update (direct)"},
            {g_ps_alsr_accumulate, sizeof(g_ps_alsr_accumulate), &psAccumulate_, L"fused accumulation"},
            {g_ps_alsr_accumulate_direct, sizeof(g_ps_alsr_accumulate_direct), &psAccumulateDirect_, L"fused accumulation (direct)"},
            {g_ps_alsr_accumulate_cubic, sizeof(g_ps_alsr_accumulate_cubic), &psCubic_, L"accumulate_cubic"},
            {g_ps_alsr_accumulate_cubic_direct, sizeof(g_ps_alsr_accumulate_cubic_direct), &psCubicDirect_, L"accumulate_cubic (direct)"},
            {g_ps_alsr_update_gated, sizeof(g_ps_alsr_update_gated), &psGated_, L"update_gated"},
            {g_ps_alsr_update_gated_direct, sizeof(g_ps_alsr_update_gated_direct), &psGatedDirect_, L"update_gated (direct)"},
            {g_ps_alsr_accumulate_point, sizeof(g_ps_alsr_accumulate_point), &psPoint_, L"point accumulation"},
            {g_ps_alsr_accumulate_point_direct, sizeof(g_ps_alsr_accumulate_point_direct), &psPointDirect_, L"point accumulation (direct)"},
            {g_ps_alsr_point_residual, sizeof(g_ps_alsr_point_residual), &psPointResidual_, L"point_residual"},
            {g_ps_alsr_point_residual_direct, sizeof(g_ps_alsr_point_residual_direct), &psPointResidualDirect_, L"point_residual_direct"},
            {g_ps_alsr_guarded_point, sizeof(g_ps_alsr_guarded_point), &psGuardedPoint_, L"guarded_point"},
            {g_ps_alsr_guarded_point_direct, sizeof(g_ps_alsr_guarded_point_direct), &psGuardedPointDirect_, L"guarded_point_direct"},
            {g_ps_alsr_point_fixed_count, sizeof(g_ps_alsr_point_fixed_count), &psPointFixedCount_, L"point fixed count"},
            {g_ps_alsr_point_fixed_count_direct, sizeof(g_ps_alsr_point_fixed_count_direct), &psPointFixedCountDirect_, L"point fixed count (direct)"},
            {g_ps_alsr_point_phase, sizeof(g_ps_alsr_point_phase), &psPointPhase_, L"point phase"},
            {g_ps_alsr_point_phase_direct, sizeof(g_ps_alsr_point_phase_direct), &psPointPhaseDirect_, L"point phase (direct)"},
            {g_ps_alsr_area_resolve, sizeof(g_ps_alsr_area_resolve), &psAreaResolve_, L"tiled area resolve"},
            {g_ps_alsr_lite_seed, sizeof(g_ps_alsr_lite_seed), &psLiteSeed_, L"Lite seed and stabilizer"},
            {g_ps_alsr_lite, sizeof(g_ps_alsr_lite), &psLite_, L"Lite stabilizer"},
            {g_ps_alsr_final, sizeof(g_ps_alsr_final), &psFinal_, L"final"},
            {g_ps_alsr_refluma, sizeof(g_ps_alsr_refluma), &psRef_, L"reference luma"},
            {g_ps_alsr_refy, sizeof(g_ps_alsr_refy), &psRefY_, L"reference luma (Y)"},
            {g_ps_alsr_seed, sizeof(g_ps_alsr_seed), &psSeed_, L"seed"},
            {g_ps_alsr_seedblend_static, sizeof(g_ps_alsr_seedblend_static), &psSeedStatic_, L"static seed blend"},
            {g_ps_alsr_reproject_static, sizeof(g_ps_alsr_reproject_static), &psReprojectStatic_, L"static reproject"},
            {g_ps_alsr_seedblend, sizeof(g_ps_alsr_seedblend), &psSeedBlend_, L"seed blend"},
        };
        for (const auto& p : ps) {
            hr = dev_->CreatePixelShader(p.data, p.size, nullptr, p.out->GetAddressOf());
            if (FAILED(hr)) return Fail(error, std::wstring(L"CreatePixelShader ") + p.name, hr);
        }
        hr = dev_->CreateComputeShader(g_cs_alsr_cellrefine, sizeof(g_cs_alsr_cellrefine), nullptr, &csCellRefine_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader cell refine", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_reference, sizeof(g_cs_alsr_reference), nullptr, &csReference_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader registration reference", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_sampling, sizeof(g_cs_alsr_sampling), nullptr, &csSampling_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader sampling detector", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_sampling_seed, sizeof(g_cs_alsr_sampling_seed), nullptr, &csSamplingSeed_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader sampling detector with seed arguments", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_area_tile, sizeof(g_cs_alsr_area_tile), nullptr, &csAreaTile_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader tiled area accumulation", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_area_padded, sizeof(g_cs_alsr_area_padded), nullptr, &csAreaPadded_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader padded area accumulation", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_area_cached_hull, sizeof(g_cs_alsr_area_cached_hull), nullptr, &csCachedHull_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader cached hull accumulation", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_area_adaptive, sizeof(g_cs_alsr_area_adaptive), nullptr, &csAreaAdaptive_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader adaptive padded accumulation", hr);
        hr = dev_->CreateComputeShader(g_cs_alsr_area_robust, sizeof(g_cs_alsr_area_robust), nullptr, &csAreaRobust_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader tiled robust area accumulation", hr);
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = sizeof(AlsrConstants);
        hr = dev_->CreateBuffer(&bd, nullptr, &cb_);
        if (FAILED(hr)) return Fail(error, L"CreateBuffer(constants)", hr);
        bd.ByteWidth = sizeof(SeedConstants);
        hr = dev_->CreateBuffer(&bd, nullptr, &cbSeed_);
        if (FAILED(hr)) return Fail(error, L"CreateBuffer(seed constants)", hr);

        D3D11_SAMPLER_DESC sd{};
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        hr = dev_->CreateSamplerState(&sd, &linear_);
        if (FAILED(hr)) return Fail(error, L"CreateSamplerState(linear)", hr);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        hr = dev_->CreateSamplerState(&sd, &point_);
        if (FAILED(hr)) return Fail(error, L"CreateSamplerState(point)", hr);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = dev_->CreateRasterizerState(&rd, &raster_);
        if (FAILED(hr)) return Fail(error, L"CreateRasterizerState", hr);
        // alsr_seedblend: Hw.rgb += src.rgb, alpha (count) untouched.
        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr = dev_->CreateBlendState(&bld, &blendAdd_);
        if (FAILED(hr)) return Fail(error, L"CreateBlendState", hr);
        // alsr_reproject writes the "needs a seed" mask as depth (test ALWAYS); alsr_seedblend tests LESS at z = 0.
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        hr = dev_->CreateDepthStencilState(&dsd, &dsWrite_);
        if (SUCCEEDED(hr)) {
            dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dsd.DepthFunc = D3D11_COMPARISON_LESS;
            hr = dev_->CreateDepthStencilState(&dsd, &dsTest_);
        }
        if (FAILED(hr)) return Fail(error, L"CreateDepthStencilState", hr);
        return true;
    }

    bool Render(const FrameGenerator& motion, ID3D11RenderTargetView* dst, Size outputSize,
                const AlsrSettings& settings, AlsrPassObserver observer, void* user) override {
        seedArgsValid_ = false;
        if (!dst || outputSize.Empty()) return false;
        ID3D11Texture2D* curr = motion.RealFrame(0);
        const Size frame = motion.FrameSize();
        if (!curr || frame.Empty()) {
            if (!warnedNoFrame_) MSR_LOG_WARN(L"AlsrUpscaler: Render without a real frame in the generator");
            warnedNoFrame_ = true;
            return false;
        }
        const bool direct = !(settings.sharpness > 0.0f) && debugView_ == AlsrDebugView::None;
        const bool adaptivePadded = settings.accumulation == AlsrAccumulation::AdaptivePadded && !settings.robustHistory;
        const bool cachedHull = settings.accumulation == AlsrAccumulation::CachedHull && !settings.robustHistory;
        const bool paddedArea = settings.accumulation == AlsrAccumulation::PaddedArea || adaptivePadded || cachedHull;
        const bool tiledArea = (paddedArea || settings.accumulation == AlsrAccumulation::TiledArea) && direct &&
                               (!settings.robustHistory || paddedArea) &&
                               frame.width <= outputSize.width && frame.height <= outputSize.height;
        if (!EnsureResources(frame, outputSize, settings.tiledReference, settings.persistentHud, tiledArea)) return false;
        if (settings.robustHistory && !EnsureHistoryState(outputSize)) return false;
        ID3D11ShaderResourceView* currSrv = SourceView(curr);
        if (!currSrv) return false;

        const MotionFieldView mv = motion.MotionField();
        // The previous frame itself is not read: its blurred luma is kept from the last Render (work_[prev]).
        const bool historyValid = histValid_ && mv.valid && motion.RealFrame(1) && mv.flow && !mv.cells.Empty() &&
                                  EnsureCells(mv.cells);
        const uint32_t cw = workCur_ ^ 1u, pw = workCur_;
        workCur_ = cw;
        if (!historyValid) prevCellsValid_ = false;
        const uint32_t cc = cellCur_ ^ 1u, pc = cellCur_;
        if (historyValid) cellCur_ = cc;

        if (!WriteConstants(settings, &mv, historyValid, false, frame, outputSize)) return false;
        const bool staticFocus = settings.accumulation == AlsrAccumulation::StaticFocus && !settings.robustHistory;
        const bool autoSampling = (UsesSamplingModel(settings.accumulation) && !settings.robustHistory) ||
                                  staticFocus;

        const bool adaptiveSeed = UsesAdaptiveSeed(settings.accumulation) && autoSampling;
        const bool adaptiveFootprint = UsesAdaptiveFootprint(settings.accumulation) && autoSampling;
        auto* seedBlend = SeedShader(adaptiveSeed ? AlsrSeed::Spatial : settings.seed, true);
        if (!seedBlend || !WriteSeedConstants(frame, outputSize)) return false;
        auto* pointSeed = adaptiveSeed ? SeedShader(AlsrSeed::Covariance, true) : nullptr;
        if (adaptiveSeed && (!pointSeed || !EnsureSeedArgs())) return false;
        if (autoSampling && !EnsureSamplingState()) return false;
        ComPtr<ID3D11ShaderResourceView> keepCurrent;
        ID3D11ShaderResourceView* previous = nullptr;
        if (staticFocus && historyValid) {
            keepCurrent = currSrv;
            previous = SourceView(motion.RealFrame(1));
            if (!previous) return false;
        }
        BeginPasses();

        if (autoSampling) {
            ID3D11ShaderResourceView* s[4] = {
                currSrv, samplingValid_ ? sampling_[pw].srv.Get() : nullptr,
                historyValid ? mv.stats : nullptr, previous
            };
            if (observer) observer(user, AlsrPass::Sampling, true);
            auto* shader = adaptiveSeed ? csSamplingSeed_.Get() : csSampling_.Get();
            Dispatch(shader, s, 4, sampling_[cw].uav.Get(), 1, 1, adaptiveSeed ? seedArgsUav_.Get() : nullptr);
            if (observer) observer(user, AlsrPass::Sampling, false);
        }
        // 0. working copy + blurred luma (source res)
        {
            ID3D11RenderTargetView* rt[2] = {work_[cw].rtv.Get(), ovl_[cw].rtv.Get()};
            ID3D11ShaderResourceView* s[5] = {currSrv, prevWorkValid_ ? work_[pw].srv.Get() : nullptr,
                                              mv.valid ? mv.staticMask : nullptr, prevWorkValid_ ? ovl_[pw].srv.Get() : nullptr,
                                              settings.persistentHud && mv.valid ? mv.stats : nullptr};
            if (observer) observer(user, AlsrPass::Prep, true);
            Draw(settings.persistentHud ? psPrepHud_.Get() : psPrep_.Get(), cb_.Get(), rt, 2, s, settings.persistentHud ? 5 : 4, frame);
            if (observer) observer(user, AlsrPass::Prep, false);
        }
        if (historyValid) {
            // 1. cell refinement (cell res)
            {
                ID3D11ShaderResourceView* s[6] = {work_[cw].srv.Get(), work_[pw].srv.Get(), mv.flow, ref_.srv.Get(),
                                                  prevCellsValid_ ? cells_[pc].srv.Get() : nullptr, mv.stats};
                if (observer) observer(user, AlsrPass::CellRefine, true);
                Dispatch(csCellRefine_.Get(), s, 6, cells_[cc].uav.Get(), (mv.cells.width + 1) / 2, (mv.cells.height + 1) / 2);
                if (observer) observer(user, AlsrPass::CellRefine, false);
            }
        }
        // 2. per-pixel motion (source res)
        {
            ID3D11RenderTargetView* rt[2] = {motion_.rtv.Get(), layer_.rtv.Get()};
            ID3D11ShaderResourceView* s[7] = {work_[cw].srv.Get(), historyValid ? work_[pw].srv.Get() : nullptr,
                                              historyValid ? mv.flow : nullptr, historyValid ? ovl_[cw].srv.Get() : nullptr,
                                              historyValid ? mv.stats : nullptr, historyValid ? cells_[cc].srv.Get() : nullptr,
                                              historyValid ? ref_.srv.Get() : nullptr};
            if (observer) observer(user, AlsrPass::Motion, true);
            ID3D11PixelShader* motionShader = settings.affineMotion
                ? (settings.pixelRefine ? psMotionAffinePixel_.Get() : psMotionAffine_.Get())
                : (settings.pixelRefine ? psMotionPixel_.Get() : psMotion_.Get());
            Draw(motionShader, cb_.Get(), rt, 2, s, 7, frame);
            if (observer) observer(user, AlsrPass::Motion, false);
        }
        // 3. reprojection + single-frame reconstruction where needed (output res)
        {
            ID3D11RenderTargetView* rt[3] = {hw_.rtv.Get(), trust_.rtv.Get(),
                                            settings.robustHistory ? historyState_[cw].rtv.Get() : nullptr};
            ID3D11ShaderResourceView* s[8] = {hist_.srv.Get(), motion_.srv.Get(), work_[cw].srv.Get(),
                                              settings.robustHistory && historyStateValid_ ? historyState_[pw].srv.Get() :
                                                  (staticFocus && historyValid ? work_[pw].srv.Get() : nullptr),
                                              historyValid ? mv.stats : nullptr, historyValid ? ovl_[cw].srv.Get() : nullptr,
                                              layer_.srv.Get(), staticFocus ? sampling_[cw].srv.Get() : nullptr};
            ctx_->OMSetDepthStencilState(dsWrite_.Get(), 0);
            dsv_ = seedMaskDsv_.Get();
            if (observer) observer(user, AlsrPass::Reproject, true);
            ID3D11PixelShader* reproject = psReproject_.Get();
            if (settings.robustHistory) reproject = psReprojectRobust_.Get();
            else if (staticFocus) reproject = psReprojectStatic_.Get();
            else if (settings.accumulation == AlsrAccumulation::PooledFootprintFast) reproject = psReprojectFast_.Get();
            Draw(reproject, cb_.Get(), rt, settings.robustHistory ? 3 : 2, s, staticFocus ? 8 : 7, outputSize);
            if (observer) observer(user, AlsrPass::Reproject, false);
        }
        // 3b. single-frame reconstruction where trust < 1, added into Hw (output res)
        {
            ID3D11RenderTargetView* rt[1] = {hw_.rtv.Get()};
            ID3D11ShaderResourceView* s[5] = {trust_.srv.Get(), work_[cw].srv.Get(), layer_.srv.Get(), currSrv,
                                              staticFocus ? sampling_[cw].srv.Get() : nullptr};
            ctx_->OMSetBlendState(blendAdd_.Get(), nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsTest_.Get(), 0);
            dsv_ = seedMaskDsv_.Get();
            if (observer) observer(user, AlsrPass::SeedBlend, true);
            auto* shader = staticFocus ? psSeedStatic_.Get() : seedBlend;
            Draw(shader, cb_.Get(), rt, 1, s, staticFocus ? 5 : 4, outputSize, pointSeed);
            if (observer) observer(user, AlsrPass::SeedBlend, false);
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(nullptr, 0);
        }
        // 4. residual (source res) when using the separate back-projection pass.
        const bool fused = !settings.robustHistory && (settings.accumulation == AlsrAccumulation::FusedBilinear ||
                                                       settings.accumulation == AlsrAccumulation::FusedCubic ||
                                                       settings.accumulation == AlsrAccumulation::PointEvidence ||
                                                       settings.accumulation == AlsrAccumulation::PointResidual);
        if (!fused && !tiledArea) {
            ID3D11RenderTargetView* rt[1] = {residual_.rtv.Get()};
            ID3D11ShaderResourceView* s[4] = {hw_.srv.Get(), work_[cw].srv.Get(), layer_.srv.Get(),
                                              adaptiveFootprint ? sampling_[cw].srv.Get() : nullptr};
            if (observer) observer(user, AlsrPass::Residual, true);
            Draw(adaptiveFootprint ? psResidualAdaptive_.Get() : psResidual_.Get(), cb_.Get(), rt, 1, s,
                 adaptiveFootprint ? 4 : 3, frame);
            if (observer) observer(user, AlsrPass::Residual, false);
        }
        // 5. update (output res) -> history + its gamma copy; with sharpness 0 and no debug view the final pass would be
        //    an exact copy, so the update writes the destination itself and the final pass is skipped.
        if (tiledArea) {
            auto* state = settings.robustHistory ? historyState_[cw].srv.Get() :
                          (adaptivePadded ? sampling_[cw].srv.Get() : nullptr);
            ID3D11ShaderResourceView* s[] = {hw_.srv.Get(), work_[cw].srv.Get(),
                                            historyValid ? work_[pw].srv.Get() : nullptr,
                                            motion_.srv.Get(), layer_.srv.Get(), state};
            auto* accumulate = paddedArea ? csAreaPadded_.Get() : csAreaTile_.Get();
            if (settings.robustHistory) accumulate = csAreaRobust_.Get();
            else if (adaptivePadded) accumulate = csAreaAdaptive_.Get();
            else if (cachedHull) accumulate = csCachedHull_.Get();
            if (observer) observer(user, AlsrPass::Update, true);
            Dispatch(accumulate, s, state ? 6 : 5, hist_.uav.Get(),
                     (outputSize.width + 15) / 16, (outputSize.height + 7) / 8);
            if (observer) observer(user, AlsrPass::Update, false);
            ID3D11RenderTargetView* rt[] = {dst};
            ID3D11ShaderResourceView* history[] = {hist_.srv.Get()};
            if (observer) observer(user, AlsrPass::Final, true);
            Draw(psAreaResolve_.Get(), cb_.Get(), rt, 1, history, 1, outputSize);
            if (observer) observer(user, AlsrPass::Final, false);
        } else {
            ID3D11RenderTargetView* rt[2] = {hist_.rtv.Get(), direct ? dst : histGamma_.rtv.Get()};
            ID3D11ShaderResourceView* s[7] = {hw_.srv.Get(), fused ? layer_.srv.Get() : residual_.srv.Get(),
                                              work_[cw].srv.Get(), motion_.srv.Get(),
                                              historyValid ? work_[pw].srv.Get() : nullptr,
                                              settings.robustHistory ? historyState_[cw].srv.Get() : nullptr,
                                              autoSampling ? sampling_[cw].srv.Get() : nullptr};
            if (observer) observer(user, AlsrPass::Update, true);
            ID3D11PixelShader* accumulate = direct ? psUpdateDirect_.Get() : psUpdate_.Get();
            switch (settings.accumulation) {
            case AlsrAccumulation::Separate:
            case AlsrAccumulation::StaticFocus:
            case AlsrAccumulation::PooledFootprint:
            case AlsrAccumulation::PooledFootprintFast:
            case AlsrAccumulation::TiledArea:
            case AlsrAccumulation::CachedHull:
            case AlsrAccumulation::PaddedArea: break;
            case AlsrAccumulation::FusedBilinear:
                accumulate = direct ? psAccumulateDirect_.Get() : psAccumulate_.Get();
                break;
            case AlsrAccumulation::FusedCubic:
                accumulate = direct ? psCubicDirect_.Get() : psCubic_.Get();
                break;
            case AlsrAccumulation::GatedSeparate:
                accumulate = direct ? psGatedDirect_.Get() : psGated_.Get();
                break;
            case AlsrAccumulation::PointResidual:
                accumulate = direct ? psPointResidualDirect_.Get() : psPointResidual_.Get();
                break;
            case AlsrAccumulation::PooledSpatial:
            case AlsrAccumulation::AdaptivePooled:
            case AlsrAccumulation::AdaptivePadded:
            case AlsrAccumulation::AdaptiveSeed:
            case AlsrAccumulation::AdaptiveGuarded:
            case AlsrAccumulation::AutoGuarded:
            case AlsrAccumulation::GuardedPoint:
                accumulate = direct ? psGuardedPointDirect_.Get() : psGuardedPoint_.Get();
                break;
            case AlsrAccumulation::PooledFixedCount:
                accumulate = direct ? psPointFixedCountDirect_.Get() : psPointFixedCount_.Get();
                break;
            case AlsrAccumulation::PooledPhase:
                accumulate = direct ? psPointPhaseDirect_.Get() : psPointPhase_.Get();
                break;
            case AlsrAccumulation::PointEvidence:
                accumulate = direct ? psPointDirect_.Get() : psPoint_.Get();
                break;
            }
            const bool pointUpdate = autoSampling && !staticFocus && !adaptiveFootprint;
            if (!settings.robustHistory && (settings.accumulation == AlsrAccumulation::GuardedPoint || pointUpdate))
                s[5] = layer_.srv.Get();
            if (settings.robustHistory) accumulate = direct ? psUpdateRobustDirect_.Get() : psUpdateRobust_.Get();
            Draw(accumulate, cb_.Get(), rt, 2, s, pointUpdate ? 7 : (s[5] ? 6 : 5), outputSize);
            if (observer) observer(user, AlsrPass::Update, false);
        }
        // 6. final (output res) -> destination
        if (!direct) {
            ID3D11RenderTargetView* rt[1] = {dst};
            ID3D11ShaderResourceView* s[3] = {histGamma_.srv.Get(), trust_.srv.Get(), motion_.srv.Get()};
            if (observer) observer(user, AlsrPass::Final, true);
            Draw(psFinal_.Get(), cb_.Get(), rt, 1, s, 3, outputSize);
            if (observer) observer(user, AlsrPass::Final, false);
        }
        // Registration reference for the next frame (source res).
        if (settings.tiledReference) {
            ID3D11ShaderResourceView* s[1] = {hist_.srv.Get()};
            if (observer) observer(user, AlsrPass::ReferenceBlur, true);
            Dispatch(csReference_.Get(), s, 1, ref_.uav.Get(), (frame.width + 15) / 16, (frame.height + 7) / 8);
            if (observer) observer(user, AlsrPass::ReferenceBlur, false);
        } else {
            ID3D11RenderTargetView* rt[1] = {refY_.rtv.Get()};
            ID3D11ShaderResourceView* s[1] = {hist_.srv.Get()};
            if (observer) observer(user, AlsrPass::ReferenceLuma, true);
            Draw(psRefY_.Get(), cb_.Get(), rt, 1, s, 1, frame);
            if (observer) observer(user, AlsrPass::ReferenceLuma, false);
            rt[0] = ref_.rtv.Get();
            s[0] = refY_.srv.Get();
            if (observer) observer(user, AlsrPass::ReferenceBlur, true);
            Draw(psRef_.Get(), cb_.Get(), rt, 1, s, 1, frame);
            if (observer) observer(user, AlsrPass::ReferenceBlur, false);
        }
        EndPasses();
        prevWorkValid_ = true;
        prevCellsValid_ = historyValid;
        histValid_ = true;
        historyStateValid_ = settings.robustHistory;
        samplingValid_ = autoSampling;
        seedArgsValid_ = adaptiveSeed;
        return true;
    }

    bool RenderSingle(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                      const AlsrSettings& settings) override {
        if (!frame || !dst || outputSize.Empty()) return false;
        D3D11_TEXTURE2D_DESC fd;
        frame->GetDesc(&fd);
        const Size fs{fd.Width, fd.Height};
        if (fs.Empty()) return false;
        ID3D11ShaderResourceView* srv = SourceView(frame);
        if (!srv) return false;
        const bool adaptive = UsesAdaptiveSeed(settings.accumulation) &&
                              !settings.robustHistory && seedArgsValid_ && fs == inSize_;
        auto* seed = SeedShader(adaptive ? AlsrSeed::Spatial : settings.seed, false);
        auto* pointSeed = adaptive ? SeedShader(AlsrSeed::Covariance, false) : nullptr;
        if (!seed || (adaptive && !pointSeed) || !WriteSeedConstants(fs, outputSize)) return false;
        if (!(settings.sharpness > 0.0f)) {
            // The seed is already quantized to eight bits. With zero sharpening an intermediate
            // render target and the final pass would just copy those values to the destination.
            BeginPasses();
            SeedPass(seed, srv, dst, outputSize, pointSeed);
            EndPasses();
            return true;
        }

        // The temporary seed target never carries history state.
        RtTex* target = &seed_;
        if (!(outputSize == outSize_) || !seed_.tex) {
            if (!(singleSize_ == outputSize) || !single_.tex) {
                if (!CreateRt(single_, outputSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"single-frame target")) return false;
                singleSize_ = outputSize;
            }
            target = &single_;
        }
        if (!WriteConstants(settings, nullptr, false, true, fs, outputSize)) return false;
        BeginPasses();
        SeedPass(seed, srv, target->rtv.Get(), outputSize, pointSeed);
        {
            ID3D11RenderTargetView* rt[1] = {dst};
            ID3D11ShaderResourceView* s[1] = {target->srv.Get()};
            Draw(psFinal_.Get(), cb_.Get(), rt, 1, s, 1, outputSize);
        }
        EndPasses();
        return true;
    }

    bool RenderLite(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                    const AlsrSettings& settings) override {
        if (!frame || !dst || outputSize.Empty()) return false;
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        const Size input{desc.Width, desc.Height};
        if (!(liteInputSize_ == input) || liteSeed_ != settings.seed || liteSharpness_ != settings.sharpness)
            liteValid_ = false;
        if (!(liteOutputSize_ == outputSize) || !lite_[0].tex || !lite_[1].tex) {
            liteOutputSize_ = {};
            liteValid_ = false;
            if (!CreateRt(lite_[0], outputSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"Lite seed") ||
                !CreateRt(lite_[1], outputSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"Lite seed"))
                return false;
            liteOutputSize_ = outputSize;
        }
        const uint32_t next = liteCur_ ^ 1u;
        auto* previous = liteValid_ ? lite_[liteCur_].srv.Get() : nullptr;
        const bool direct = !(settings.sharpness > 0.0f) && settings.seed == AlsrSeed::Spatial;
        if (direct) {
            auto* current = SourceView(frame);
            if (!current || !WriteSeedConstants(input, outputSize)) return false;
            ID3D11RenderTargetView* targets[] = {dst, lite_[next].rtv.Get()};
            ID3D11ShaderResourceView* sources[] = {current, previous};
            BeginPasses();
            Draw(psLiteSeed_.Get(), cb_.Get(), targets, 2, sources, 2, outputSize);
        } else {
            if (!RenderSingle(frame, lite_[next].rtv.Get(), outputSize, settings)) return false;
            ID3D11RenderTargetView* targets[] = {dst};
            ID3D11ShaderResourceView* sources[] = {lite_[next].srv.Get(), previous};
            BeginPasses();
            Draw(psLite_.Get(), cb_.Get(), targets, 1, sources, 2, outputSize);
        }
        EndPasses();
        liteCur_ = next;
        liteInputSize_ = input;
        liteSeed_ = settings.seed;
        liteSharpness_ = settings.sharpness;
        liteValid_ = true;
        return true;
    }

    ID3D11Texture2D* SamplingState() const override {
        return samplingValid_ ? sampling_[workCur_].tex.Get() : nullptr;
    }

    ID3D11Texture2D* RefinedMotion() const override { return histValid_ ? motion_.tex.Get() : nullptr; }

    void Reset() override {
        liteValid_ = false;
        samplingValid_ = false;
        seedArgsValid_ = false;
        histValid_ = false;
        historyStateValid_ = false;
        prevWorkValid_ = false;
    }

    void SetDebugView(AlsrDebugView view) override { debugView_ = view; }

private:
    ID3D11PixelShader* SeedShader(AlsrSeed seed, bool blend) {
        if (seed == AlsrSeed::Spatial) return blend ? psSeedBlend_.Get() : psSeed_.Get();

        struct Bytecode { const BYTE* data; size_t size; };
        static constexpr Bytecode variants[][2] = {
            {{g_ps_alsr_seed_lanczos, sizeof(g_ps_alsr_seed_lanczos)},
             {g_ps_alsr_seedblend_lanczos, sizeof(g_ps_alsr_seedblend_lanczos)}},
            {{g_ps_alsr_seed_directional, sizeof(g_ps_alsr_seed_directional)},
             {g_ps_alsr_seedblend_directional, sizeof(g_ps_alsr_seedblend_directional)}},
            {{g_ps_alsr_seed_covariance, sizeof(g_ps_alsr_seed_covariance)},
             {g_ps_alsr_seedblend_covariance, sizeof(g_ps_alsr_seedblend_covariance)}}
        };
        const size_t index = static_cast<size_t>(seed) - 1;
        if (index >= std::size(variants)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: invalid seed filter {}", static_cast<uint32_t>(seed));
            return nullptr;
        }
        const size_t path = blend ? 1 : 0;
        auto& shader = psSeedVariants_[index][path];
        if (!shader) {
            const auto& code = variants[index][path];
            const HRESULT hr = dev_->CreatePixelShader(code.data, code.size, nullptr, shader.GetAddressOf());
            if (FAILED(hr)) {
                MSR_LOG_ERROR(L"AlsrUpscaler: CreatePixelShader(seed variant) failed: {}", HrToString(hr));
                return nullptr;
            }
        }
        return shader.Get();
    }

    // Seed constants (b1, alsr_seed.hlsli): source / destination size.
    bool WriteSeedConstants(Size in, Size outSize) {
        D3D11_MAPPED_SUBRESOURCE map;
        const HRESULT hr = ctx_->Map(cbSeed_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: Map(seed constants) failed: {}", HrToString(hr));
            return false;
        }
        SeedConstants c{};
        c.src[0] = static_cast<float>(in.width);
        c.src[1] = static_cast<float>(in.height);
        c.src[2] = 1.0f / c.src[0];
        c.src[3] = 1.0f / c.src[1];
        c.dst[0] = static_cast<float>(outSize.width);
        c.dst[1] = static_cast<float>(outSize.height);
        c.dst[2] = std::min(c.dst[0] / c.src[0], c.dst[1] / c.src[1]);
        c.dst[3] = 0.0f; // must be 0: exact compiler barrier in AlsrSeed (alsr_seed.hlsli)
        memcpy(map.pData, &c, sizeof(c));
        ctx_->Unmap(cbSeed_.Get(), 0);
        return true;
    }

    // Single-frame reconstruction of every pixel (alsr_seed_ps) into `out` (gamma RGBA8). Constants: WriteSeedConstants.
    void SeedPass(ID3D11PixelShader* shader, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* out, Size outSize,
                  ID3D11PixelShader* pointSeed) {
        ID3D11RenderTargetView* rt[1] = {out};
        ID3D11ShaderResourceView* s[1] = {src};
        Draw(shader, cb_.Get(), rt, 1, s, 1, outSize, pointSeed);
    }

    static bool Fail(std::wstring& error, const std::wstring& what, HRESULT hr) {
        error = L"AlsrUpscaler: " + what + L" failed: " + HrToString(hr);
        MSR_LOG_ERROR(L"{}", error);
        return false;
    }

    // `srvFmt` / `rtvFmt` (UNKNOWN = the texture format) give different views of a TYPELESS texture.
    bool CreateRt(RtTex& t, Size s, DXGI_FORMAT fmt, const wchar_t* name, bool uav = false, DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN,
                  DXGI_FORMAT rtvFmt = DXGI_FORMAT_UNKNOWN) {
        t.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = s.width;
        td.Height = s.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &t.tex);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srvFmt;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = rtvFmt;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(t.tex.Get(), srvFmt != DXGI_FORMAT_UNKNOWN ? &sv : nullptr, &t.srv);
        if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(t.tex.Get(), rtvFmt != DXGI_FORMAT_UNKNOWN ? &rv : nullptr, &t.rtv);
        if (SUCCEEDED(hr) && uav) hr = dev_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: texture {} {}x{} failed: {}", name, s.width, s.height, HrToString(hr));
            t.Reset();
            return false;
        }
        return true;
    }

    bool EnsureResources(Size frame, Size out, bool tiledReference, bool persistentHud, bool tiledArea) {
        if (frame == inSize_ && out == outSize_ && hist_.tex && tiledReference_ == tiledReference &&
            persistentHud_ == persistentHud && tiledArea_ == tiledArea) return true;
        histValid_ = false;
        prevWorkValid_ = false;
        inSize_ = {};
        outSize_ = {};
        srcCache_ = {};
        cellSize_ = {};
        if (tiledReference) refY_.Reset();
        if (tiledArea) residual_.Reset();
        const DXGI_FORMAT overlayFormat = persistentHud ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
        const bool ok = CreateRt(motion_, frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"motion") &&
                        CreateRt(work_[0], frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"working copy") &&
                        CreateRt(work_[1], frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"working copy") &&
                        CreateRt(ovl_[0], frame, overlayFormat, L"overlay mask") &&
                        CreateRt(ovl_[1], frame, overlayFormat, L"overlay mask") &&
                        (tiledArea || CreateRt(residual_, frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"residual")) &&
                        CreateRt(ref_, frame, tiledReference ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16_FLOAT,
                                 L"registration reference", tiledReference) &&
                        (tiledReference || CreateRt(refY_, frame, DXGI_FORMAT_R32_FLOAT, L"registration reference (unblurred)")) &&
                        CreateRt(layer_, frame, DXGI_FORMAT_R8G8_UNORM, L"overlay layer") &&
                        CreateRt(hist_, out, DXGI_FORMAT_R16G16B16A16_FLOAT, L"history", tiledArea) &&
                        CreateRt(histGamma_, out, DXGI_FORMAT_R8G8B8A8_TYPELESS, L"history gamma copy", false, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 tune_.linear ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM) &&
                        CreateRt(hw_, out, DXGI_FORMAT_R16G16B16A16_FLOAT, L"prediction") &&
                        CreateRt(trust_, out, DXGI_FORMAT_R8_UNORM, L"trust") &&
                        CreateRt(seed_, out, DXGI_FORMAT_R8G8B8A8_UNORM, L"seed");
        if (!ok) return false;
        {
            seedMaskDsv_.Reset();
            seedMask_.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = out.width;
            td.Height = out.height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_D16_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &seedMask_);
            if (SUCCEEDED(hr)) hr = dev_->CreateDepthStencilView(seedMask_.Get(), nullptr, &seedMaskDsv_);
            if (FAILED(hr)) {
                MSR_LOG_ERROR(L"AlsrUpscaler: seed mask depth buffer {}x{} failed: {}", out.width, out.height, HrToString(hr));
                seedMask_.Reset();
                seedMaskDsv_.Reset();
                return false;
            }
        }
        inSize_ = frame;
        outSize_ = out;
        tiledReference_ = tiledReference;
        persistentHud_ = persistentHud;
        tiledArea_ = tiledArea;
        MSR_LOG_INFO(L"AlsrUpscaler: {}x{} -> {}x{}", frame.width, frame.height, out.width, out.height);
        return true;
    }

    bool EnsureSamplingState() {
        if (sampling_[0].tex && sampling_[1].tex) return true;
        samplingValid_ = false;
        const Size stateSize{kAlsrSamplingStateWidth, 1};
        return CreateRt(sampling_[0], stateSize, DXGI_FORMAT_R32G32B32A32_FLOAT, L"sampling state", true) &&
               CreateRt(sampling_[1], stateSize, DXGI_FORMAT_R32G32B32A32_FLOAT, L"sampling state", true);
    }

    bool EnsureSeedArgs() {
        if (seedArgs_) return true;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 2 * sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        buffer.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ComPtr<ID3D11Buffer> args;
        HRESULT hr = dev_->CreateBuffer(&buffer, nullptr, &args);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: CreateBuffer(seed arguments) failed: {}", HrToString(hr));
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = buffer.ByteWidth / sizeof(uint32_t);
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        hr = dev_->CreateUnorderedAccessView(args.Get(), &view, &seedArgsUav_);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: CreateUnorderedAccessView(seed arguments) failed: {}", HrToString(hr));
            return false;
        }
        seedArgs_ = std::move(args);
        return true;
    }

    bool EnsureHistoryState(Size out) {
        if (historyStateSize_ == out && historyState_[0].tex && historyState_[1].tex) return true;
        historyStateSize_ = {};
        historyStateValid_ = false;
        if (!CreateRt(historyState_[0], out, DXGI_FORMAT_R8G8B8A8_UNORM, L"history protection") ||
            !CreateRt(historyState_[1], out, DXGI_FORMAT_R8G8B8A8_UNORM, L"history protection"))
            return false;
        historyStateSize_ = out;
        return true;
    }

    // The motion-cell grid changes with the generator's flow settings; it is not part of the history.
    bool EnsureCells(Size cells) {
        if (cells == cellSize_ && cells_[0].tex) return true;
        cellSize_ = {};
        prevCellsValid_ = false;
        if (!CreateRt(cells_[0], cells, DXGI_FORMAT_R32G32B32A32_FLOAT, L"refined cells", true) ||
            !CreateRt(cells_[1], cells, DXGI_FORMAT_R32G32B32A32_FLOAT, L"refined cells", true))
            return false;
        cellSize_ = cells;
        return true;
    }

    // Generator frames are a small fixed set of textures; cache one SRV per texture (no per-frame allocation).
    ID3D11ShaderResourceView* SourceView(ID3D11Texture2D* tex) {
        for (auto& e : srcCache_)
            if (e.tex.Get() == tex) return e.srv.Get();
        auto& slot = srcCache_[srcNext_];
        srcNext_ = (srcNext_ + 1) % srcCache_.size();
        slot.tex.Reset();
        slot.srv.Reset();
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        const HRESULT hr = dev_->CreateShaderResourceView(tex, &sv, &slot.srv);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: CreateShaderResourceView(frame) failed: {}", HrToString(hr));
            slot.srv.Reset();
            return nullptr;
        }
        slot.tex = tex;
        return slot.srv.Get();
    }

    bool WriteConstants(const AlsrSettings& s, const MotionFieldView* mv, bool historyValid, bool inputGamma, Size in,
                        Size out) {
        D3D11_MAPPED_SUBRESOURCE map;
        const HRESULT hr = ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"AlsrUpscaler: Map(constants) failed: {}", HrToString(hr));
            return false;
        }
        AlsrConstants c{};
        const float iw = static_cast<float>(std::max(1u, in.width)), ih = static_cast<float>(std::max(1u, in.height));
        const float ow = static_cast<float>(std::max(1u, out.width)), oh = static_cast<float>(std::max(1u, out.height));
        c.in[0] = iw;
        c.in[1] = ih;
        c.in[2] = 1.0f / iw;
        c.in[3] = 1.0f / ih;
        c.out[0] = ow;
        c.out[1] = oh;
        c.out[2] = 1.0f / ow;
        c.out[3] = 1.0f / oh;
        c.scale[0] = ow / iw;
        c.scale[1] = oh / ih;
        c.scale[2] = iw / ow;
        c.scale[3] = ih / oh;
        if (mv && mv->valid && mv->cellSizePx > 0.0f) {
            c.cell[0] = static_cast<float>(mv->cells.width);
            c.cell[1] = static_cast<float>(mv->cells.height);
            c.cell[2] = mv->cellSizePx;
            c.cell[3] = 1.0f / mv->cellSizePx;
        }
        const float stability = std::isfinite(s.stability) ? std::clamp(s.stability, 0.0f, 0.98f) : 0.85f;
        c.p0[0] = std::isfinite(s.sharpness) ? std::clamp(s.sharpness, 0.0f, 1.0f) : 0.0f;
        c.p0[1] = tune_.gMin >= 0.0f ? tune_.gMin : std::clamp(1.4f - 1.25f * stability, 0.12f, 1.0f);
        c.p0[2] = tune_.clipK;
        c.p0[3] = tune_.maxCount;
        c.p1[0] = tune_.confLo;
        c.p1[1] = tune_.confHi;
        c.p1[2] = tune_.matchLo;
        c.p1[3] = tune_.matchHi;
        c.p2[0] = tune_.boostLo;
        c.p2[1] = tune_.boostHi;
        c.p2[2] = tune_.clipKMotion;
        c.p2[3] = tune_.motionTight;
        c.p3[0] = tune_.tau0;
        c.p3[1] = tune_.tauK;
        c.p3[2] = tune_.spread;
        c.p3[3] = tune_.agree;
        c.u0[0] = historyValid ? 1u : 0u;
        c.u0[1] = (historyValid && mv && mv->staticMask) ? 1u : 0u;
        c.u0[2] = tune_.linear;
        c.u0[3] = inputGamma ? 0u : static_cast<uint32_t>(debugView_);
        c.u1[0] = mv ? mv->sceneCutIndex : 0u;
        c.u1[1] = (historyValid && mv && mv->stats) ? 1u : 0u;
        c.u1[2] = inputGamma ? 1u : 0u;
        c.u1[3] = tune_.variant;
        if (UsesSamplingModel(s.accumulation) && !s.robustHistory) c.u1[3] |= (1u << 18);
        if (!s.robustHistory && (s.accumulation == AlsrAccumulation::AdaptiveGuarded ||
                                 UsesAdaptiveSeed(s.accumulation))) c.u1[3] |= (1u << 20);
        if (!s.robustHistory && UsesPooledSampling(s.accumulation)) c.u1[3] |= (1u << 21);
        if (s.accumulation == AlsrAccumulation::StaticFocus && !s.robustHistory) c.u1[3] |= (1u << 19);
        c.p4[0] = tune_.velMin;
        c.p4[1] = tune_.velLo;
        c.p4[2] = tune_.velHi;
        c.p4[3] = tune_.margin;
        c.u2[0] = (historyValid && prevCellsValid_) ? 1u : 0u;
        c.u2[1] = prevWorkValid_ ? 1u : 0u;
        c.u2[2] = (mv && mv->valid && mv->staticMask) ? 1u : 0u;
        c.u2[3] = tune_.lkIters;
        memcpy(map.pData, &c, sizeof(c));
        ctx_->Unmap(cb_.Get(), 0);
        return true;
    }

    void BeginPasses() {
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->RSSetState(raster_.Get());
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(nullptr, 0);
        ID3D11SamplerState* samplers[2] = {linear_.Get(), point_.Get()};
        ctx_->PSSetSamplers(0, 2, samplers);
        ID3D11Buffer* cbSeed = cbSeed_.Get();
        ctx_->PSSetConstantBuffers(1, 1, &cbSeed); // alsr_seed.hlsli (b1)
    }

    void Draw(ID3D11PixelShader* ps, ID3D11Buffer* cb, ID3D11RenderTargetView* const* rtv, UINT nrt,
              ID3D11ShaderResourceView* const* srv, UINT nsrv, Size viewport, ID3D11PixelShader* pointSeed = nullptr) {
        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(viewport.width), static_cast<float>(viewport.height), 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->OMSetRenderTargets(nrt, rtv, dsv_);
        dsv_ = nullptr; // one draw only
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cb);
        ctx_->PSSetShaderResources(0, nsrv, srv);
        if (pointSeed) {
            // The detector writes a zero instance count for the inactive filter.
            ctx_->DrawInstancedIndirect(seedArgs_.Get(), 0);
            ctx_->PSSetShader(pointSeed, nullptr, 0);
            ctx_->DrawInstancedIndirect(seedArgs_.Get(), sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS));
        } else {
            ctx_->Draw(3, 0);
        }
        // Unbind before the next pass writes any of these as a render target.
        ID3D11ShaderResourceView* nullSrv[16] = {};
        ctx_->PSSetShaderResources(0, nsrv, nullSrv);
        ID3D11RenderTargetView* nullRtv[3] = {};
        ctx_->OMSetRenderTargets(3, nullRtv, nullptr);
    }

    // Compute pass: SRVs t0.., UAVs u0 and optional u1, constants b0, samplers s0 linear / s1 point.
    void Dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srv, UINT nsrv, ID3D11UnorderedAccessView* uav,
                  UINT gx, UINT gy, ID3D11UnorderedAccessView* uav1 = nullptr) {
        ID3D11Buffer* cb = cb_.Get();
        ID3D11SamplerState* samplers[2] = {linear_.Get(), point_.Get()};
        ID3D11UnorderedAccessView* outputs[2] = {uav, uav1};
        const UINT numOutputs = uav1 ? 2 : 1;
        ctx_->CSSetShader(cs, nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, &cb);
        ctx_->CSSetSamplers(0, 2, samplers);
        ctx_->CSSetShaderResources(0, nsrv, srv);
        ctx_->CSSetUnorderedAccessViews(0, numOutputs, outputs, nullptr);
        ctx_->Dispatch(gx, gy, 1);
        ID3D11ShaderResourceView* nullSrv[8] = {};
        ID3D11UnorderedAccessView* nullUav[2] = {};
        ctx_->CSSetShaderResources(0, nsrv, nullSrv);
        ctx_->CSSetUnorderedAccessViews(0, numOutputs, nullUav, nullptr);
        ctx_->CSSetShader(nullptr, nullptr, 0);
    }

    void EndPasses() {
        ID3D11ShaderResourceView* nullSrv[8] = {};
        ctx_->PSSetShaderResources(0, 8, nullSrv);
        ID3D11RenderTargetView* nullRtv[3] = {};
        ctx_->OMSetRenderTargets(3, nullRtv, nullptr);
    }

    struct SrcEntry {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };

    ID3D11Device1* dev_ = nullptr;
    ID3D11DeviceContext1* ctx_ = nullptr;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11ComputeShader> csCellRefine_;
    RtTex historyState_[2];
    Size historyStateSize_{};
    bool historyStateValid_ = false;
    ComPtr<ID3D11PixelShader> psSeedStatic_, psReprojectStatic_;
    ComPtr<ID3D11PixelShader> psReprojectFast_;
    ComPtr<ID3D11PixelShader> psReprojectRobust_, psUpdateRobust_, psUpdateRobustDirect_;
    ComPtr<ID3D11PixelShader> psPointResidual_, psPointResidualDirect_, psGuardedPoint_, psGuardedPointDirect_;
    ComPtr<ID3D11PixelShader> psPointFixedCount_, psPointFixedCountDirect_, psPointPhase_, psPointPhaseDirect_;
    ComPtr<ID3D11PixelShader> psResidualAdaptive_;
    ComPtr<ID3D11PixelShader> psAccumulate_, psAccumulateDirect_, psPoint_, psPointDirect_;
    ComPtr<ID3D11PixelShader> psCubic_, psCubicDirect_, psGated_, psGatedDirect_;
    ComPtr<ID3D11PixelShader> psLite_, psLiteSeed_, psAreaResolve_;
    RtTex lite_[2];
    Size liteInputSize_{}, liteOutputSize_{};
    uint32_t liteCur_ = 0;
    AlsrSeed liteSeed_ = AlsrSeed::Spatial;
    float liteSharpness_ = 0.0f;
    bool liteValid_ = false;
    ComPtr<ID3D11PixelShader> psPrepHud_, psMotionAffine_, psMotionPixel_, psMotionAffinePixel_;
    ComPtr<ID3D11PixelShader> psPrep_, psMotion_, psReproject_, psResidual_, psUpdate_, psUpdateDirect_, psFinal_, psRef_, psRefY_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11RasterizerState> raster_;

    RtTex motion_, residual_, ref_, refY_, layer_;       // source resolution
    std::array<RtTex, 2> work_;                   // source resolution, current / previous (ping-pong)
    std::array<RtTex, 2> ovl_;                    // persistent overlay mask, ping-pong with work_
    bool prevWorkValid_ = false;
    std::array<RtTex, 2> cells_;                  // motion-cell resolution, current / previous (ping-pong)
    uint32_t cellCur_ = 0;
    bool prevCellsValid_ = false;
    Size cellSize_{};
    uint32_t workCur_ = 0;
    RtTex hist_, histGamma_, hw_, trust_, seed_; // output resolution (seed_: RenderSingle only)
    RtTex single_;                                // RenderSingle target when its output size differs
    Size inSize_{}, outSize_{}, singleSize_{};
    std::array<SrcEntry, 6> srcCache_{};
    size_t srcNext_ = 0;

    // single-frame reconstruction (seed) and the depth-masked seed blend
    ComPtr<ID3D11PixelShader> psSeed_, psSeedBlend_;
    ComPtr<ID3D11PixelShader> psSeedVariants_[3][2];
    ComPtr<ID3D11BlendState> blendAdd_;
    ComPtr<ID3D11DepthStencilState> dsWrite_, dsTest_;
    ComPtr<ID3D11Texture2D> seedMask_;              // D16 output res: 1 = trust < 1 (alsr_seedblend runs), 0 = not
    ComPtr<ID3D11DepthStencilView> seedMaskDsv_;
    ID3D11DepthStencilView* dsv_ = nullptr;         // depth target for the next Draw only
    ComPtr<ID3D11Buffer> cbSeed_;
    Tune tune_{};
    AlsrDebugView debugView_ = AlsrDebugView::None;
    bool histValid_ = false;
    bool tiledReference_ = false;
    bool persistentHud_ = false;
    bool tiledArea_ = false;
    ComPtr<ID3D11ComputeShader> csReference_, csSampling_, csSamplingSeed_, csAreaTile_, csAreaPadded_, csAreaRobust_;
    ComPtr<ID3D11ComputeShader> csAreaAdaptive_, csCachedHull_;
    RtTex sampling_[2];
    bool samplingValid_ = false;
    ComPtr<ID3D11Buffer> seedArgs_;
    ComPtr<ID3D11UnorderedAccessView> seedArgsUav_;
    bool seedArgsValid_ = false;
    bool warnedNoFrame_ = false;
};

} // namespace

std::unique_ptr<AlsrUpscaler> AlsrUpscaler::Create(GpuDevice& device, std::wstring& error) {
    auto p = std::make_unique<AlsrUpscalerImpl>();
    if (!p->Init(device, error)) return nullptr;
    return p;
}

} // namespace moonsr
