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
//   6. alsr_final      (output res)  adaptive sharpening of the gamma copy, debug views -> destination + LastOutput copy
//   7. alsr_refy + alsr_refluma (source res)  registration reference (blurred history luma) for the next frame
// Passes 1-2 are skipped / trivial when there is no valid history. The shader headers document each pass exactly.
// RenderSingle: alsr_seed (seed of every pixel) + alsr_final.
// There is no CPU readback and no state on disk. There is no time-based fallback of any kind: the work per frame
// never depends on how long earlier frames took.
//
// Memory (output W x H, source w x h): history RGBA16F + prediction RGBA16F (16 B/px), history gamma copy RGBA8, seed
// mask D16, trust R8, seed RGBA8 (RenderSingle / debug), LastOutput BGRA8 = 31 B per output pixel; per-pixel motion
// RGBA16F (8), two working copies RGBA16F (16), residual RGBA16F (8), reference R16F + R32F (6), overlay evidence 2 x R8,
// layer RG8 (4) = 42 B per source pixel; two refined cell fields RGBA32F (negligible).
// 1280x720 -> 1920x1080: 98 MiB; 1920x1080 -> 2560x1440: 192 MiB; 1920x1080 -> 3840x2160: 328 MiB.
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
#include "g_cs_alsr_cellrefine.h"
#include "g_ps_alsr_motion.h"
#include "g_ps_alsr_reproject.h"
#include "g_ps_alsr_residual.h"
#include "g_ps_alsr_update.h"
#include "g_ps_alsr_update_direct.h"
#include "g_ps_alsr_final.h"
#include "g_ps_alsr_refluma.h"
#include "g_ps_alsr_refy.h"
#include "g_ps_alsr_seed.h"
#include "g_ps_alsr_seedblend.h"

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
            {g_ps_alsr_prep, sizeof(g_ps_alsr_prep), &psPrep_, L"prep"},
            {g_ps_alsr_motion, sizeof(g_ps_alsr_motion), &psMotion_, L"motion"},
            {g_ps_alsr_reproject, sizeof(g_ps_alsr_reproject), &psReproject_, L"reproject"},
            {g_ps_alsr_residual, sizeof(g_ps_alsr_residual), &psResidual_, L"residual"},
            {g_ps_alsr_update, sizeof(g_ps_alsr_update), &psUpdate_, L"update"},
            {g_ps_alsr_update_direct, sizeof(g_ps_alsr_update_direct), &psUpdateDirect_, L"update (direct)"},
            {g_ps_alsr_final, sizeof(g_ps_alsr_final), &psFinal_, L"final"},
            {g_ps_alsr_refluma, sizeof(g_ps_alsr_refluma), &psRef_, L"reference luma"},
            {g_ps_alsr_refy, sizeof(g_ps_alsr_refy), &psRefY_, L"reference luma (Y)"},
            {g_ps_alsr_seed, sizeof(g_ps_alsr_seed), &psSeed_, L"seed"},
            {g_ps_alsr_seedblend, sizeof(g_ps_alsr_seedblend), &psSeedBlend_, L"seed blend"},
        };
        for (const auto& p : ps) {
            hr = dev_->CreatePixelShader(p.data, p.size, nullptr, p.out->GetAddressOf());
            if (FAILED(hr)) return Fail(error, std::wstring(L"CreatePixelShader ") + p.name, hr);
        }
        hr = dev_->CreateComputeShader(g_cs_alsr_cellrefine, sizeof(g_cs_alsr_cellrefine), nullptr, &csCellRefine_);
        if (FAILED(hr)) return Fail(error, L"CreateComputeShader cell refine", hr);
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
                const AlsrSettings& settings) override {
        if (!dst || outputSize.Empty()) return false;
        ID3D11Texture2D* curr = motion.RealFrame(0);
        const Size frame = motion.FrameSize();
        if (!curr || frame.Empty()) {
            if (!warnedNoFrame_) MSR_LOG_WARN(L"AlsrUpscaler: Render without a real frame in the generator");
            warnedNoFrame_ = true;
            return false;
        }
        if (!EnsureResources(frame, outputSize)) return false;
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
        if (!WriteSeedConstants(frame, outputSize)) return false;
        BeginPasses();

        // 0. working copy + blurred luma (source res)
        {
            ID3D11RenderTargetView* rt[2] = {work_[cw].rtv.Get(), ovl_[cw].rtv.Get()};
            ID3D11ShaderResourceView* s[4] = {currSrv, prevWorkValid_ ? work_[pw].srv.Get() : nullptr,
                                              mv.valid ? mv.staticMask : nullptr, prevWorkValid_ ? ovl_[pw].srv.Get() : nullptr};
            Draw(psPrep_.Get(), cb_.Get(), rt, 2, s, 4, frame);
        }
        if (historyValid) {
            // 1. cell refinement (cell res)
            {
                ID3D11ShaderResourceView* s[6] = {work_[cw].srv.Get(), work_[pw].srv.Get(), mv.flow, ref_.srv.Get(),
                                                  prevCellsValid_ ? cells_[pc].srv.Get() : nullptr, mv.stats};
                Dispatch(csCellRefine_.Get(), s, 6, cells_[cc].uav.Get(), (mv.cells.width + 1) / 2, (mv.cells.height + 1) / 2);
            }
        }
        // 2. per-pixel motion (source res)
        {
            ID3D11RenderTargetView* rt[2] = {motion_.rtv.Get(), layer_.rtv.Get()};
            ID3D11ShaderResourceView* s[7] = {work_[cw].srv.Get(), historyValid ? work_[pw].srv.Get() : nullptr,
                                              historyValid ? mv.flow : nullptr, historyValid ? ovl_[cw].srv.Get() : nullptr,
                                              historyValid ? mv.stats : nullptr, historyValid ? cells_[cc].srv.Get() : nullptr,
                                              historyValid ? ref_.srv.Get() : nullptr};
            Draw(psMotion_.Get(), cb_.Get(), rt, 2, s, 7, frame);
        }
        // 3. reprojection + single-frame reconstruction where needed (output res)
        {
            ID3D11RenderTargetView* rt[2] = {hw_.rtv.Get(), trust_.rtv.Get()};
            ID3D11ShaderResourceView* s[7] = {hist_.srv.Get(), motion_.srv.Get(), work_[cw].srv.Get(), nullptr,
                                              historyValid ? mv.stats : nullptr, historyValid ? ovl_[cw].srv.Get() : nullptr,
                                              layer_.srv.Get()};
            ctx_->OMSetDepthStencilState(dsWrite_.Get(), 0);
            dsv_ = seedMaskDsv_.Get();
            Draw(psReproject_.Get(), cb_.Get(), rt, 2, s, 7, outputSize);
        }
        // 3b. single-frame reconstruction where trust < 1, added into Hw (output res)
        {
            ID3D11RenderTargetView* rt[1] = {hw_.rtv.Get()};
            ID3D11ShaderResourceView* s[4] = {trust_.srv.Get(), work_[cw].srv.Get(), layer_.srv.Get(), currSrv};
            ctx_->OMSetBlendState(blendAdd_.Get(), nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(dsTest_.Get(), 0);
            dsv_ = seedMaskDsv_.Get();
            Draw(psSeedBlend_.Get(), cb_.Get(), rt, 1, s, 4, outputSize);
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(nullptr, 0);
        }
        // 4. residual (source res)
        {
            ID3D11RenderTargetView* rt[1] = {residual_.rtv.Get()};
            ID3D11ShaderResourceView* s[3] = {hw_.srv.Get(), work_[cw].srv.Get(), layer_.srv.Get()};
            Draw(psResidual_.Get(), cb_.Get(), rt, 1, s, 3, frame);
        }
        // 5. update (output res) -> history + its gamma copy; with sharpness 0 and no debug view the final pass would be
        //    an exact copy, so the update writes the destination and LastOutput itself and the final pass is skipped.
        const bool direct = !(settings.sharpness > 0.0f) && debugView_ == AlsrDebugView::None;
        {
            ID3D11RenderTargetView* rt[3] = {hist_.rtv.Get(), direct ? dst : histGamma_.rtv.Get(), last_.rtv.Get()};
            ID3D11ShaderResourceView* s[5] = {hw_.srv.Get(), residual_.srv.Get(), work_[cw].srv.Get(), motion_.srv.Get(),
                                              historyValid ? work_[pw].srv.Get() : nullptr};
            Draw(direct ? psUpdateDirect_.Get() : psUpdate_.Get(), cb_.Get(), rt, direct ? 3 : 2, s, 5, outputSize);
        }
        // 6. final (output res) -> destination + LastOutput
        if (!direct) {
            ID3D11RenderTargetView* rt[2] = {dst, last_.rtv.Get()};
            ID3D11ShaderResourceView* s[3] = {histGamma_.srv.Get(), trust_.srv.Get(), motion_.srv.Get()};
            Draw(psFinal_.Get(), cb_.Get(), rt, 2, s, 3, outputSize);
        }
        // 8. registration reference for the next frame (source res)
        {
            ID3D11RenderTargetView* rt[1] = {refY_.rtv.Get()};
            ID3D11ShaderResourceView* s[1] = {hist_.srv.Get()};
            Draw(psRefY_.Get(), cb_.Get(), rt, 1, s, 1, frame);
            rt[0] = ref_.rtv.Get();
            s[0] = refY_.srv.Get();
            Draw(psRef_.Get(), cb_.Get(), rt, 1, s, 1, frame);
        }
        EndPasses();
        prevWorkValid_ = true;
        prevCellsValid_ = historyValid;
        histValid_ = true;
        lastValid_ = true;
        return true;
    }

    ID3D11Texture2D* LastOutput() const override { return lastValid_ ? last_.tex.Get() : nullptr; }

    bool RenderSingle(ID3D11Texture2D* frame, ID3D11RenderTargetView* dst, Size outputSize,
                      const AlsrSettings& settings) override {
        if (!frame || !dst || outputSize.Empty()) return false;
        D3D11_TEXTURE2D_DESC fd;
        frame->GetDesc(&fd);
        const Size fs{fd.Width, fd.Height};
        if (fs.Empty()) return false;
        // Uses the Render seed target when the output size matches (it holds no state between calls), otherwise its
        // own target. Never touches the history, LastOutput or the history validity.
        RtTex* target = &seed_;
        if (!(outputSize == outSize_) || !seed_.tex) {
            if (!(singleSize_ == outputSize) || !single_.tex) {
                if (!CreateRt(single_, outputSize, DXGI_FORMAT_R8G8B8A8_UNORM, L"single-frame target")) return false;
                singleSize_ = outputSize;
            }
            target = &single_;
        }
        ID3D11ShaderResourceView* srv = SourceView(frame);
        if (!srv) return false;
        if (!WriteConstants(settings, nullptr, false, true, fs, outputSize)) return false;
        if (!WriteSeedConstants(fs, outputSize)) return false;
        BeginPasses();
        SeedPass(srv, target->rtv.Get(), outputSize);
        {
            ID3D11RenderTargetView* rt[1] = {dst};
            ID3D11ShaderResourceView* s[1] = {target->srv.Get()};
            Draw(psFinal_.Get(), cb_.Get(), rt, 1, s, 1, outputSize);
        }
        EndPasses();
        return true;
    }

    void Reset() override {
        histValid_ = false;
        prevWorkValid_ = false;
    }

    void SetDebugView(AlsrDebugView view) override { debugView_ = view; }

private:
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
    void SeedPass(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* out, Size outSize) {
        ID3D11RenderTargetView* rt[1] = {out};
        ID3D11ShaderResourceView* s[1] = {src};
        Draw(psSeed_.Get(), cb_.Get(), rt, 1, s, 1, outSize);
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

    bool EnsureResources(Size frame, Size out) {
        if (frame == inSize_ && out == outSize_ && hist_.tex) return true;
        histValid_ = false;
        lastValid_ = false;
        prevWorkValid_ = false;
        inSize_ = {};
        outSize_ = {};
        srcCache_ = {};
        cellSize_ = {};
        const bool ok = CreateRt(motion_, frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"motion") &&
                        CreateRt(work_[0], frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"working copy") &&
                        CreateRt(work_[1], frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"working copy") &&
                        CreateRt(ovl_[0], frame, DXGI_FORMAT_R8_UNORM, L"overlay mask") &&
                        CreateRt(ovl_[1], frame, DXGI_FORMAT_R8_UNORM, L"overlay mask") &&
                        CreateRt(residual_, frame, DXGI_FORMAT_R16G16B16A16_FLOAT, L"residual") &&
                        CreateRt(ref_, frame, DXGI_FORMAT_R16_FLOAT, L"registration reference") &&
                        CreateRt(refY_, frame, DXGI_FORMAT_R32_FLOAT, L"registration reference (unblurred)") &&
                        CreateRt(layer_, frame, DXGI_FORMAT_R8G8_UNORM, L"overlay layer") &&
                        CreateRt(hist_, out, DXGI_FORMAT_R16G16B16A16_FLOAT, L"history") &&
                        CreateRt(histGamma_, out, DXGI_FORMAT_R8G8B8A8_TYPELESS, L"history gamma copy", false, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 tune_.linear ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM) &&
                        CreateRt(hw_, out, DXGI_FORMAT_R16G16B16A16_FLOAT, L"prediction") &&
                        CreateRt(trust_, out, DXGI_FORMAT_R8_UNORM, L"trust") &&
                        CreateRt(seed_, out, DXGI_FORMAT_R8G8B8A8_UNORM, L"seed") &&
                        CreateRt(last_, out, DXGI_FORMAT_B8G8R8A8_UNORM, L"output copy");
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
        MSR_LOG_INFO(L"AlsrUpscaler: {}x{} -> {}x{}", frame.width, frame.height, out.width, out.height);
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
              ID3D11ShaderResourceView* const* srv, UINT nsrv, Size viewport) {
        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(viewport.width), static_cast<float>(viewport.height), 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->OMSetRenderTargets(nrt, rtv, dsv_);
        dsv_ = nullptr; // one draw only
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cb);
        ctx_->PSSetShaderResources(0, nsrv, srv);
        ctx_->Draw(3, 0);
        // Unbind before the next pass writes any of these as a render target.
        ID3D11ShaderResourceView* nullSrv[16] = {};
        ctx_->PSSetShaderResources(0, nsrv, nullSrv);
        ID3D11RenderTargetView* nullRtv[3] = {};
        ctx_->OMSetRenderTargets(3, nullRtv, nullptr);
    }

    // Compute pass: SRVs t0.., one UAV u0, constants b0 = cb_, samplers s0 linear / s1 point. Unbinds afterwards.
    void Dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srv, UINT nsrv, ID3D11UnorderedAccessView* uav,
                  UINT gx, UINT gy) {
        ID3D11Buffer* cb = cb_.Get();
        ID3D11SamplerState* samplers[2] = {linear_.Get(), point_.Get()};
        ctx_->CSSetShader(cs, nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, &cb);
        ctx_->CSSetSamplers(0, 2, samplers);
        ctx_->CSSetShaderResources(0, nsrv, srv);
        ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx_->Dispatch(gx, gy, 1);
        ID3D11ShaderResourceView* nullSrv[8] = {};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx_->CSSetShaderResources(0, nsrv, nullSrv);
        ctx_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
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
    RtTex hist_, histGamma_, hw_, trust_, seed_, last_; // output resolution (seed_: RenderSingle only)
    RtTex single_;                                // RenderSingle target when its output size differs
    Size inSize_{}, outSize_{}, singleSize_{};
    std::array<SrcEntry, 6> srcCache_{};
    size_t srcNext_ = 0;

    // single-frame reconstruction (seed) and the depth-masked seed blend
    ComPtr<ID3D11PixelShader> psSeed_, psSeedBlend_;
    ComPtr<ID3D11BlendState> blendAdd_;
    ComPtr<ID3D11DepthStencilState> dsWrite_, dsTest_;
    ComPtr<ID3D11Texture2D> seedMask_;              // D16 output res: 1 = trust < 1 (alsr_seedblend runs), 0 = not
    ComPtr<ID3D11DepthStencilView> seedMaskDsv_;
    ID3D11DepthStencilView* dsv_ = nullptr;         // depth target for the next Draw only
    ComPtr<ID3D11Buffer> cbSeed_;
    Tune tune_{};
    AlsrDebugView debugView_ = AlsrDebugView::None;
    bool histValid_ = false;
    bool lastValid_ = false;
    bool warnedNoFrame_ = false;
};

} // namespace

std::unique_ptr<AlsrUpscaler> AlsrUpscaler::Create(GpuDevice& device, std::wstring& error) {
    auto p = std::make_unique<AlsrUpscalerImpl>();
    if (!p->Init(device, error)) return nullptr;
    return p;
}

} // namespace moonsr
