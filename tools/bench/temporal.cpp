// Temporal-sequence benchmark: scoring, GPU runner (spatial scalers and MoonSR temporal), driver, self-test.
#include "temporal.h"

#include "imageops.h"
#include "metrics.h"

#include <DirectXPackedVector.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace bench {

// ------------------------------------------------------------------------------------------------------------------
// Scoring

SeqScorer::SeqScorer(uint32_t w, uint32_t h, const std::vector<HudRect>& hud) : w_(w), h_(h), hud_(hud) {}

namespace {
std::vector<float> Crop(const std::vector<float>& p, uint32_t w, const HudRect& r) {
    const uint32_t cw = static_cast<uint32_t>(r.x1 - r.x0), ch = static_cast<uint32_t>(r.y1 - r.y0);
    std::vector<float> o(static_cast<size_t>(cw) * ch);
    for (uint32_t y = 0; y < ch; ++y)
        std::copy_n(p.begin() + static_cast<size_t>(r.y0 + static_cast<int>(y)) * w + static_cast<size_t>(r.x0), cw, o.begin() + static_cast<size_t>(y) * cw);
    return o;
}
} // namespace

SeqScore SeqScorer::Add(const std::vector<float>& gt, const Image& out) {
    SeqScore s;
    const std::vector<float> lum = Luma(out);
    const size_t n = lum.size();
    std::vector<float> err(n);
    double sq = 0;
    for (size_t i = 0; i < n; ++i) {
        err[i] = lum[i] - gt[i];
        sq += static_cast<double>(err[i]) * err[i];
    }
    s.psnr = PsnrFromMse(sq / static_cast<double>(n));
    s.ssim = SsimPlanes(gt, lum, w_, h_);
    const bool havePrev = prevErr_.size() == n;
    if (havePrev) {
        double d = 0;
        for (size_t i = 0; i < n; ++i) d += std::abs(static_cast<double>(err[i]) - prevErr_[i]);
        s.flicker = d / static_cast<double>(n);
    }
    if (!hud_.empty()) {
        s.hasHud = true;
        double hsq = 0, hd = 0, hn = 0, ssimSum = 0;
        for (const HudRect& r : hud_) {
            for (int y = r.y0; y < r.y1; ++y)
                for (int x = r.x0; x < r.x1; ++x) {
                    const size_t i = static_cast<size_t>(y) * w_ + static_cast<size_t>(x);
                    hsq += static_cast<double>(err[i]) * err[i];
                    if (havePrev) hd += std::abs(static_cast<double>(err[i]) - prevErr_[i]);
                    hn += 1;
                }
            ssimSum += SsimPlanes(Crop(gt, w_, r), Crop(lum, w_, r), static_cast<uint32_t>(r.x1 - r.x0), static_cast<uint32_t>(r.y1 - r.y0));
        }
        s.hudPsnr = PsnrFromMse(hsq / hn);
        s.hudSsim = ssimSum / static_cast<double>(hud_.size());
        if (havePrev) s.hudFlicker = hd / hn;
    }
    prevErr_ = std::move(err);
    return s;
}

// ------------------------------------------------------------------------------------------------------------------
// GPU runner

namespace {

bool WaitQ(ID3D11DeviceContext* ctx, ID3D11Query* q, void* data, UINT size) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const HRESULT hr = ctx->GetData(q, data, size, 0);
        if (hr == S_OK) return true;
        if (FAILED(hr)) return false;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) return false;
        std::this_thread::yield();
    }
}

bool ScoreFlow(ID3D11DeviceContext* ctx, const moonsr::MotionFieldView& field, const Sequence& sequence,
               int frame, uint32_t iw, uint32_t ih, ComPtr<ID3D11Texture2D>& readback, double& epe) {
    if (!field.valid || frame == sequence.CutFrame()) return true;
    if (!field.flow || field.cells.Empty() || field.cellSizePx <= 0) return false;
    if (!readback) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = field.cells.width;
        desc.Height = field.cells.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return false;
    }
    ComPtr<ID3D11Resource> resource;
    field.flow->GetResource(&resource);
    ctx->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, resource.Get(), D3D11CalcSubresource(0, 1, 1), nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    const double scaleX = static_cast<double>(sequence.W()) / iw;
    const double scaleY = static_cast<double>(sequence.H()) / ih;
    double sum = 0;
    size_t count = 0;
    for (uint32_t y = 0; y < field.cells.height; ++y) {
        const double iy = (y + 0.5) * field.cellSizePx;
        if (iy >= ih) break;
        const auto* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(map.pData) + size_t{y} * map.RowPitch);
        for (uint32_t x = 0; x < field.cells.width; ++x) {
            const double ix = (x + 0.5) * field.cellSizePx;
            if (ix >= iw) break;
            double dx = 0, dy = 0;
            if (!sequence.BackwardFlow(ix * scaleX, iy * scaleY, frame, dx, dy)) continue;
            const auto* cell = row + size_t{x} * 4;
            const double error = std::hypot(cell[0] - dx / scaleX, cell[1] - dy / scaleY);
            if (!std::isfinite(error)) {
                ctx->Unmap(readback.Get(), 0);
                return false;
            }
            sum += error;
            ++count;
        }
    }
    ctx->Unmap(readback.Get(), 0);
    if (count) epe = sum / static_cast<double>(count);
    return true;
}

bool ScoreRefinedFlow(ID3D11DeviceContext* ctx, ID3D11Texture2D* motion, const Sequence& sequence, int frame,
                      ComPtr<ID3D11Texture2D>& staging, SeqScore& score) {
    if (frame == 0 || frame == sequence.CutFrame()) return true;
    if (!motion) return false;
    D3D11_TEXTURE2D_DESC desc{};
    motion->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || !desc.Width || !desc.Height) return false;
    if (!staging) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    }
    ctx->CopyResource(staging.Get(), motion);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    const double scaleX = static_cast<double>(sequence.W()) / desc.Width;
    const double scaleY = static_cast<double>(sequence.H()) / desc.Height;
    double total = 0.0, movingTotal = 0.0;
    size_t count = 0, movingCount = 0;
    bool valid = true;
    constexpr uint32_t stride = 4;
    for (uint32_t y = static_cast<uint32_t>(frame / stride) % stride; y < desc.Height; y += stride) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(map.pData) + size_t{y} * map.RowPitch);
        for (uint32_t x = static_cast<uint32_t>(frame) % stride; x < desc.Width; x += stride) {
            double dx = 0, dy = 0;
            if (!sequence.BackwardFlow((x + 0.5) * scaleX, (y + 0.5) * scaleY, frame, dx, dy)) continue;
            const auto* value = row + size_t{x} * 4;
            const double vx = DirectX::PackedVector::XMConvertHalfToFloat(value[0]);
            const double vy = DirectX::PackedVector::XMConvertHalfToFloat(value[1]);
            const double error = std::hypot(vx - dx / scaleX, vy - dy / scaleY);
            if (!std::isfinite(error)) { valid = false; break; }
            total += error;
            ++count;
            if (std::hypot(dx / scaleX, dy / scaleY) > 0.1) {
                movingTotal += error;
                ++movingCount;
            }
        }
        if (!valid) break;
    }
    ctx->Unmap(staging.Get(), 0);
    if (count) score.refinedFlowEpe = total / static_cast<double>(count);
    if (movingCount) score.movingRefinedFlowEpe = movingTotal / static_cast<double>(movingCount);
    return valid;
}

} // namespace

bool RunMethod(GpuRunner& gpu, const Sequence& seq, const std::vector<Image>& inputs, const std::vector<std::vector<float>>& gtLuma,
               const MethodSpec& m, int keepFrame, RunOut& out, std::string& err) {
    out = RunOut{};
    if (inputs.empty() || inputs.size() != gtLuma.size()) { err = "bad sequence data"; return false; }
    moonsr::GpuDevice& dev = gpu.Device();
    ID3D11Device* d3d = dev.Device();
    ID3D11DeviceContext* ctx = dev.Context();
    const uint32_t iw = inputs[0].w, ih = inputs[0].h, ow = seq.W(), oh = seq.H();

    ComPtr<ID3D11Texture2D> inTex, dst, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = iw; td.Height = ih; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d->CreateTexture2D(&td, nullptr, &inTex))) { err = "create input texture failed"; return false; }
    td.Width = ow; td.Height = oh; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d->CreateTexture2D(&td, nullptr, &dst)) || FAILED(d3d->CreateRenderTargetView(dst.Get(), nullptr, &rtv))) { err = "create output target failed"; return false; }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d3d->CreateTexture2D(&td, nullptr, &staging))) { err = "create staging failed"; return false; }

    std::unique_ptr<moonsr::FrameGenerator> fg;
    std::unique_ptr<moonsr::AlsrUpscaler> alsr;
    moonsr::AlsrSettings as;
    moonsr::ScaleSettings ss;
    std::wstring werr;
    if (m.alsr) {
        fg = moonsr::FrameGenerator::Create(dev, werr);
        if (!fg) { err = "FrameGenerator::Create: " + Narrow(werr); return false; }
    }
    if (m.alsr || m.single || m.lite) {
        alsr = moonsr::AlsrUpscaler::Create(dev, werr);
        if (!alsr) { err = "AlsrUpscaler::Create: " + Narrow(werr); return false; }
        as.sharpness = m.sharpness;
        as.seed = m.seed;
        as.accumulation = m.accumulation;
        as.robustHistory = m.robustHistory;
        as.tiledReference = m.tiledReference;
        as.affineMotion = m.affineMotion;
        as.pixelRefine = m.pixelRefine;
        as.persistentHud = m.persistentHud;
    } else {
        gpu.Pass().ResetSourceCache();
        ss.scaler = m.kind;
        ss.fit = moonsr::FitMode::Stretch;
        ss.sharpness = m.sharpness;
    }

    LARGE_INTEGER qf{};
    QueryPerformanceFrequency(&qf);
    const moonsr::RectI srcRect{0, 0, static_cast<int32_t>(iw), static_cast<int32_t>(ih)};
    SeqScorer scorer(ow, oh, seq.Hud());
    uint64_t hash = 1469598103934665603ull;
    ComPtr<ID3D11Texture2D> flowReadback, samplingReadback, refinedReadback;
    if (moonsr::UsesSamplingModel(m.accumulation) ||
        m.accumulation == moonsr::AlsrAccumulation::StaticFocus) {
        td.Width = moonsr::kAlsrSamplingStateWidth;
        td.Height = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (FAILED(d3d->CreateTexture2D(&td, nullptr, &samplingReadback))) {
            err = "create sampling state staging failed";
            return false;
        }
    }
    Image frame;
    frame.Alloc(ow, oh);

    for (size_t t = 0; t < inputs.size(); ++t) {
        ctx->UpdateSubresource(inTex.Get(), 0, nullptr, inputs[t].px.data(), iw * 4, 0);
        bool ok = true;
        if (m.alsr) ok = fg->PushFrame(inTex.Get(), srcRect, static_cast<int64_t>(t) * (qf.QuadPart / 60)) && alsr->Render(*fg, rtv.Get(), moonsr::Size{ow, oh}, as);
        else if (m.lite) ok = alsr->RenderLite(inTex.Get(), rtv.Get(), moonsr::Size{ow, oh}, as);
        else if (m.single) ok = alsr->RenderSingle(inTex.Get(), rtv.Get(), moonsr::Size{ow, oh}, as);
        else ok = gpu.Pass().Render(inTex.Get(), srcRect, rtv.Get(), moonsr::Size{ow, oh}, ss, nullptr);
        if (!ok) { err = std::string(alsr ? "ALSR" : "ScalePass") + " failed at frame " + std::to_string(t); return false; }
        ctx->CopyResource(staging.Get(), dst.Get());

        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { err = "Map staging failed"; return false; }
        for (uint32_t y = 0; y < oh; ++y) memcpy(frame.Row(y), static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, ow * 4);
        ctx->Unmap(staging.Get(), 0);
        for (size_t i = 3; i < frame.px.size(); i += 4) frame.px[i] = 255;
        hash = Fnv1a(frame.px.data(), frame.px.size(), hash);
        auto score = scorer.Add(gtLuma[t], frame);
        if (fg && seq.HasGroundTruthFlow() && !ScoreFlow(ctx, fg->MotionField(), seq, static_cast<int>(t), iw, ih, flowReadback, score.flowEpe)) {
            err = "flow readback or scoring failed at frame " + std::to_string(t);
            return false;
        }
        if (alsr && m.alsr && m.scoreRefinedFlow && seq.HasGroundTruthFlow() &&
            !ScoreRefinedFlow(ctx, alsr->RefinedMotion(), seq, static_cast<int>(t), refinedReadback, score)) {
            err = "refined flow scoring failed at frame " + std::to_string(t);
            return false;
        }
        if (samplingReadback) {
            auto* state = alsr->SamplingState();
            if (!state) { err = "missing GPU sampling state"; return false; }
            ctx->CopyResource(samplingReadback.Get(), state);
            if (FAILED(ctx->Map(samplingReadback.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                err = "read GPU sampling state failed";
                return false;
            }
            const auto* values = static_cast<const float*>(map.pData);
            score.samplingModel = static_cast<int>(values[0]);
            score.staticBlend = values[2];
            score.samplingEdgeCount = static_cast<int>(values[4]);
            score.samplingConcentration = values[5];
            score.samplingAbruptFraction = values[6];
            const auto flags = static_cast<unsigned>(values[7]);
            score.samplingPointCandidate = (flags & 1u) != 0;
            score.samplingRestart = (flags & 2u) != 0;
            ctx->Unmap(samplingReadback.Get(), 0);
        }
        out.scores.push_back(score);
        if (static_cast<int>(t) != keepFrame) continue;
        out.keptFrame = frame;
        if (moonsr::UsesAdaptiveSeed(m.accumulation)) {
            if (!alsr->RenderSingle(inTex.Get(), rtv.Get(), {ow, oh}, as)) {
                err = "adaptive generated-frame seed failed";
                return false;
            }
            ctx->CopyResource(staging.Get(), dst.Get());
            if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                err = "read adaptive generated-frame seed failed";
                return false;
            }
            out.keptSingle.Alloc(ow, oh);
            for (uint32_t y = 0; y < oh; ++y)
                memcpy(out.keptSingle.Row(y), static_cast<const uint8_t*>(map.pData) + size_t{y} * map.RowPitch, ow * 4);
            ctx->Unmap(staging.Get(), 0);
        }
    }
    if (!alsr) gpu.Pass().ResetSourceCache();
    out.outHash = hash;
    return true;
}

// GPU time per frame, measured separately from scoring so that CPU-side metric work between frames cannot let the GPU
// clock down. The whole sequence is submitted back to back TWICE (fresh state, all input textures pre-uploaded) and the
// per-frame GPU times of the second pass are reported: the first pass ramps clocks and finishes lazy driver work; the
// second pass starts with a jump from the last frame to frame 0, which the warm-up frames of the caller cover. Each
// frame is bracketed by timestamp queries: spatial = ScalePass::Render; ALSR = FrameGenerator::PushFrame (motion
// estimator) and AlsrUpscaler::Render (main) separately. A third traversal measures individual ALSR passes so
// the extra timestamp commands stay outside the whole-render measurement.
bool TimeMethod(GpuRunner& gpu, const Sequence& seq, const std::vector<Image>& inputs, const MethodSpec& m, RunOut& out, std::string& err) {
    moonsr::GpuDevice& dev = gpu.Device();
    ID3D11Device* d3d = dev.Device();
    ID3D11DeviceContext* ctx = dev.Context();
    const uint32_t iw = inputs[0].w, ih = inputs[0].h, ow = seq.W(), oh = seq.H();
    const size_t n = inputs.size();
    std::vector<ComPtr<ID3D11Texture2D>> in(n);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = iw; td.Height = ih; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (size_t t = 0; t < n; ++t) {
        D3D11_SUBRESOURCE_DATA sd{inputs[t].px.data(), iw * 4, 0};
        if (FAILED(d3d->CreateTexture2D(&td, &sd, &in[t]))) { err = "create input texture failed"; return false; }
    }
    ComPtr<ID3D11Texture2D> dst;
    ComPtr<ID3D11RenderTargetView> rtv;
    td.Width = ow; td.Height = oh; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d->CreateTexture2D(&td, nullptr, &dst)) || FAILED(d3d->CreateRenderTargetView(dst.Get(), nullptr, &rtv))) { err = "create output target failed"; return false; }

    const size_t measuredFrames = 2 * n;
    const size_t submittedFrames = measuredFrames + (m.alsr ? n : 0);
    std::vector<ComPtr<ID3D11Query>> ts(measuredFrames * 3);
    constexpr size_t passCount = static_cast<size_t>(moonsr::AlsrPass::Count);
    using PassQueries = std::array<ComPtr<ID3D11Query>, passCount * 2>;
    std::vector<PassQueries> passQueries(m.alsr ? n : 0);
    std::vector<std::array<bool, passCount>> issued(passQueries.size());
    struct PassTiming {
        ID3D11DeviceContext* ctx;
        PassQueries& queries;
        std::array<bool, passCount>& issued;
    };
    const auto observe = [](void* user, moonsr::AlsrPass pass, bool begin) {
        auto& timing = *static_cast<PassTiming*>(user);
        const size_t index = static_cast<size_t>(pass);
        timing.ctx->End(timing.queries[index * 2 + (begin ? 0 : 1)].Get());
        if (!begin) timing.issued[index] = true;
    };
    ComPtr<ID3D11Query> dj;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(d3d->CreateQuery(&qd, &dj))) { err = "CreateQuery(disjoint) failed"; return false; }
    qd.Query = D3D11_QUERY_TIMESTAMP;
    for (auto& q : ts) if (FAILED(d3d->CreateQuery(&qd, &q))) { err = "CreateQuery(timestamp) failed"; return false; }

    for (auto& frame : passQueries)
        for (auto& q : frame)
            if (FAILED(d3d->CreateQuery(&qd, &q))) { err = "CreateQuery(pass timestamp) failed"; return false; }

    LARGE_INTEGER qf{};
    QueryPerformanceFrequency(&qf);
    const moonsr::RectI srcRect{0, 0, static_cast<int32_t>(iw), static_cast<int32_t>(ih)};
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::unique_ptr<moonsr::FrameGenerator> fg;
        std::unique_ptr<moonsr::AlsrUpscaler> alsr;
        moonsr::AlsrSettings as;
        moonsr::ScaleSettings ss;
        std::wstring werr;
        if (m.alsr) {
            fg = moonsr::FrameGenerator::Create(dev, werr);
            if (!fg) { err = "FrameGenerator::Create: " + Narrow(werr); return false; }
        }
        if (m.alsr || m.single || m.lite) {
            alsr = moonsr::AlsrUpscaler::Create(dev, werr);
            if (!alsr) { err = "AlsrUpscaler::Create: " + Narrow(werr); return false; }
            as.sharpness = m.sharpness;
            as.seed = m.seed;
            as.accumulation = m.accumulation;
            as.robustHistory = m.robustHistory;
            as.tiledReference = m.tiledReference;
            as.affineMotion = m.affineMotion;
            as.pixelRefine = m.pixelRefine;
            as.persistentHud = m.persistentHud;
        } else {
            gpu.Pass().ResetSourceCache();
            ss.scaler = m.kind; ss.fit = moonsr::FitMode::Stretch; ss.sharpness = m.sharpness;
        }
        ctx->Begin(dj.Get());
        for (size_t f = 0; f < submittedFrames; ++f) {
            ID3D11Texture2D* src = in[f % n].Get();
            if (f >= measuredFrames) {
                const size_t t = f - measuredFrames;
                issued[t].fill(false);
                PassTiming timing{ctx, passQueries[t], issued[t]};
                if (!fg->PushFrame(src, srcRect, static_cast<int64_t>(f) * (qf.QuadPart / 60)) ||
                    !alsr->Render(*fg, rtv.Get(), moonsr::Size{ow, oh}, as, observe, &timing)) {
                    err = "render failed during per-pass timing";
                    return false;
                }
                continue;
            }
            ctx->End(ts[f * 3 + 0].Get());
            bool ok = true;
            if (m.alsr) {
                ok = fg->PushFrame(src, srcRect, static_cast<int64_t>(f) * (qf.QuadPart / 60));
                ctx->End(ts[f * 3 + 1].Get());
                ok = ok && alsr->Render(*fg, rtv.Get(), moonsr::Size{ow, oh}, as);
                ctx->End(ts[f * 3 + 2].Get());
            } else {
                if (m.lite) ok = alsr->RenderLite(src, rtv.Get(), moonsr::Size{ow, oh}, as);
                else ok = m.single ? alsr->RenderSingle(src, rtv.Get(), moonsr::Size{ow, oh}, as) :
                                gpu.Pass().Render(src, srcRect, rtv.Get(), moonsr::Size{ow, oh}, ss, nullptr);
                ctx->End(ts[f * 3 + 1].Get());
            }
            if (!ok) { err = "render failed during timing pass"; return false; }
        }
        ctx->End(dj.Get());
        ctx->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT djd{};
        if (!WaitQ(ctx, dj.Get(), &djd, sizeof(djd))) { err = "disjoint query wait failed"; return false; }
        std::vector<UINT64> v(measuredFrames * 3, 0);
        for (size_t f = 0; f < measuredFrames; ++f)
            for (size_t i = 0; i < (m.alsr ? 3u : 2u); ++i)
                if (!WaitQ(ctx, ts[f * 3 + i].Get(), &v[f * 3 + i], sizeof(UINT64))) { err = "timestamp wait failed"; return false; }
        if (!alsr) gpu.Pass().ResetSourceCache();
        if (djd.Disjoint) continue;
        const double k = 1000.0 / static_cast<double>(djd.Frequency);
        out.msMain.assign(n, kNaN);
        out.msMotion.assign(n, kNaN);
        for (size_t t = 0; t < n; ++t) {
            const size_t f = n + t;
            if (m.alsr) {
                if (v[f * 3 + 1] >= v[f * 3] && v[f * 3 + 2] >= v[f * 3 + 1]) {
                    out.msMotion[t] = static_cast<double>(v[f * 3 + 1] - v[f * 3]) * k;
                    out.msMain[t] = static_cast<double>(v[f * 3 + 2] - v[f * 3 + 1]) * k;
                }
            } else if (v[f * 3 + 1] >= v[f * 3]) {
                out.msMain[t] = static_cast<double>(v[f * 3 + 1] - v[f * 3]) * k;
            }
        }
        for (size_t p = 0; m.alsr && p < passCount; ++p) {
            auto& times = out.msPass[p];
            times.assign(n, kNaN);
            for (size_t t = 0; t < n; ++t) {
                if (!issued[t][p]) continue;
                UINT64 begin{}, end{};
                if (!WaitQ(ctx, passQueries[t][p * 2].Get(), &begin, sizeof(begin)) ||
                    !WaitQ(ctx, passQueries[t][p * 2 + 1].Get(), &end, sizeof(end))) {
                    err = "pass timestamp wait failed";
                    return false;
                }
                if (end >= begin) times[t] = static_cast<double>(end - begin) * k;
            }
        }
        return true;
    }
    err = "timestamp queries stayed disjoint";
    return false;
}

// ------------------------------------------------------------------------------------------------------------------
// Driver

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string G(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%g", v);
    return b;
}

constexpr std::array<const char*, static_cast<size_t>(moonsr::AlsrPass::Count)> kPassNames{
    "prep", "cell_refine", "motion", "reproject", "seed_blend", "residual", "update", "final", "reference_luma", "reference_blur", "sampling"};

// Edge-step sampling evidence from the source image only. Low-pass AA can spread an
// otherwise abrupt monotone edge across two intervals, but texture content can do the same.
struct EdgeEvidence {
    double sum = 0.0;
    long long count = 0;
    long long abrupt = 0;
};

EdgeEvidence MeasureEdgeSteps(const std::vector<Image>& frames, size_t warmup) {
    constexpr uint32_t stride = 5;
    const auto luma = [](const Image& frame, uint32_t x, uint32_t y) {
        const uint8_t* pixel = frame.Row(y) + static_cast<size_t>(x) * 4;
        return 0.0722 * pixel[0] + 0.7152 * pixel[1] + 0.2126 * pixel[2];
    };
    EdgeEvidence out;
    const auto add = [&out](double first, double middle, double last) {
        const double nearStep = middle - first;
        const double farStep = last - middle;
        const double edge = std::abs(last - first);
        if (edge > 25.5 && nearStep * farStep >= 0.0) {
            const double concentration = std::max(std::abs(nearStep), std::abs(farStep)) / edge;
            out.sum += concentration;
            ++out.count;
            out.abrupt += concentration >= 0.90;
        }
    };
    for (size_t t = warmup; t < frames.size(); ++t) {
        const auto& frame = frames[t];
        const uint32_t phaseX = static_cast<uint32_t>(t % stride);
        const uint32_t phaseY = static_cast<uint32_t>((t / stride) % stride);
        for (uint32_t y = 1 + phaseY; y + 1 < frame.h; y += stride) {
            for (uint32_t x = 1 + phaseX; x + 1 < frame.w; x += stride) {
                const double middle = luma(frame, x, y);
                add(luma(frame, x - 1, y), middle, luma(frame, x + 1, y));
                add(luma(frame, x, y - 1), middle, luma(frame, x, y + 1));
            }
        }
    }
    return out;
}

struct Agg {
    double psnr = 0, psnrMin = 0, ssim = 0, flicker = kNaN, hudPsnr = kNaN, hudSsim = kNaN, hudFlicker = kNaN;
    double flowEpe = kNaN;
    double cutPsnr = kNaN, cutSsim = kNaN, cutFramePsnr = kNaN;
    double msMean = kNaN, msMed = kNaN, msP95 = kNaN, msMax = kNaN, motMean = kNaN, motMed = kNaN;
    int n = 0, disjoint = 0;
};

double MeanOf(const std::vector<double>& v) {
    if (v.empty()) return kNaN;
    double s = 0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}
double Median(std::vector<double> v) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Per-frame medians across sweeps retain the frame association used by the scoring CSV.
template <class Select>
std::vector<double> MedianTiming(const std::vector<RunOut>& runs, size_t frames, Select select) {
    std::vector<double> result(frames, kNaN);
    std::vector<double> samples;
    samples.reserve(runs.size());
    for (size_t t = 0; t < frames; ++t) {
        samples.clear();
        for (const auto& run : runs) {
            const auto& values = select(run);
            if (t < values.size() && std::isfinite(values[t])) samples.push_back(values[t]);
        }
        if (samples.empty()) continue;
        std::sort(samples.begin(), samples.end());
        const size_t hi = samples.size() / 2;
        const size_t lo = (samples.size() - 1) / 2;
        result[t] = 0.5 * (samples[lo] + samples[hi]);
    }
    return result;
}

// Frames >= warmup are averaged. Flicker excludes the scene-cut frame itself (its "previous frame" is another scene).
// Cut window: frames [cut, cut + kCutWindow).
constexpr int kCutWindow = 8;

Agg Aggregate(const RunOut& r, int warmup, int cut) {
    Agg a;
    std::vector<double> ps, ss, fl, hp, hs, hf, ms, mot, cp, cs, epe;
    for (int t = warmup; t < static_cast<int>(r.scores.size()); ++t) {
        const SeqScore& s = r.scores[static_cast<size_t>(t)];
        if (std::isfinite(s.flowEpe)) epe.push_back(s.flowEpe);
        ps.push_back(s.psnr);
        ss.push_back(s.ssim);
        if (t != cut && !std::isnan(s.flicker)) fl.push_back(s.flicker);
        if (s.hasHud) {
            hp.push_back(s.hudPsnr);
            hs.push_back(s.hudSsim);
            if (t != cut && !std::isnan(s.hudFlicker)) hf.push_back(s.hudFlicker);
        }
        const double m1 = r.msMain[static_cast<size_t>(t)], m2 = r.msMotion[static_cast<size_t>(t)];
        if (std::isnan(m1)) ++a.disjoint; else ms.push_back(m1);
        if (!std::isnan(m2)) mot.push_back(m2);
    }
    if (cut >= 0)
        for (int t = cut; t < std::min(cut + kCutWindow, static_cast<int>(r.scores.size())); ++t) {
            cp.push_back(r.scores[static_cast<size_t>(t)].psnr);
            cs.push_back(r.scores[static_cast<size_t>(t)].ssim);
        }
    a.n = static_cast<int>(ps.size());
    a.flowEpe = MeanOf(epe);
    a.psnr = MeanOf(ps);
    a.psnrMin = ps.empty() ? kNaN : *std::min_element(ps.begin(), ps.end());
    a.ssim = MeanOf(ss);
    a.flicker = MeanOf(fl);
    a.hudPsnr = MeanOf(hp); a.hudSsim = MeanOf(hs); a.hudFlicker = MeanOf(hf);
    a.cutPsnr = MeanOf(cp); a.cutSsim = MeanOf(cs);
    if (cut >= 0 && cut < static_cast<int>(r.scores.size())) a.cutFramePsnr = r.scores[static_cast<size_t>(cut)].psnr;
    a.msMean = MeanOf(ms); a.msMed = Median(ms);
    if (!ms.empty()) {
        std::sort(ms.begin(), ms.end());
        a.msMax = ms.back();
        a.msP95 = ms[std::min(ms.size() - 1, static_cast<size_t>(std::ceil(0.95 * static_cast<double>(ms.size()))) - 1)];
    }
    a.motMean = MeanOf(mot); a.motMed = Median(mot);
    return a;
}

std::string HashHex(uint64_t h) {
    char b[32];
    snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
    return b;
}

} // namespace

int RunTemporalBench(const TemporalOptions& o, const std::vector<std::unique_ptr<GpuRunner>>& gpus, const SystemInfo& si, const std::string& tag) {
    std::string err;
    // Methods.
    std::vector<MethodSpec> methods;
    auto selected = [&](const std::string& label) {
        if (o.methods.empty()) return label == "Lanczos-3" || label == "MoonSR Spatial" || label == "MoonSR";
        for (const auto& f : o.methods) {
            if (Lower(label).find(f) != std::string::npos) return true;
            // The temporal upscaler was called "Moon ALSR"; "alsr" still selects it.
            if (label == "MoonSR" && Lower("Moon ALSR").find(f) != std::string::npos) return true;
        }
        return false;
    };
    const std::pair<const char*, moonsr::AlsrSeed> seedCandidates[] = {
        {"Spatial", moonsr::AlsrSeed::Spatial}, {"Lanczos-3", moonsr::AlsrSeed::Lanczos},
        {"Directional", moonsr::AlsrSeed::Directional}, {"Covariance", moonsr::AlsrSeed::Covariance}
    };
    for (float sh : o.sharpness) {
        for (const auto& s : moonsr::ScalePass::ListScalers()) {
            if (!s.available || s.kind == moonsr::ScalerKind::MoonALSR) continue;
            const std::string label = Narrow(s.name);
            if (selected(label)) methods.push_back({label, false, s.kind, sh});
        }
        if (selected("MoonSR")) methods.push_back({"MoonSR", true, moonsr::ScalerKind::MoonALSR, sh});
        using Accumulation = moonsr::AlsrAccumulation;
        for (const char* label : {"Affine motion", "HUD persistence", "Affine + HUD"}) {
            if (!selected(label)) continue;
            MethodSpec trial{label, true, moonsr::ScalerKind::MoonALSR, sh};
            trial.affineMotion = trial.label != "HUD persistence";
            trial.persistentHud = trial.label != "Affine motion";
            methods.push_back(std::move(trial));
        }
        if (selected("Pixel refine")) {
            MethodSpec trial{"Pixel refine", true, moonsr::ScalerKind::MoonALSR, sh};
            trial.pixelRefine = true;
            methods.push_back(std::move(trial));
        }
        if (selected("Affine + pixel")) {
            MethodSpec trial{"Affine + pixel", true, moonsr::ScalerKind::MoonALSR, sh};
            trial.affineMotion = trial.pixelRefine = true;
            methods.push_back(std::move(trial));
        }
        if (selected("MoonSR Lite")) {
            MethodSpec trial{"MoonSR Lite", false, moonsr::ScalerKind::MoonALSR, sh};
            trial.lite = true;
            methods.push_back(std::move(trial));
        }
        if (selected("Robust history"))
            methods.push_back({"Robust history", true, moonsr::ScalerKind::MoonALSR, sh, false,
                               moonsr::AlsrSeed::Spatial, Accumulation::GatedSeparate, true});
        if (selected("Tiled reference"))
            methods.push_back({"Tiled reference", true, moonsr::ScalerKind::MoonALSR, sh, false,
                               moonsr::AlsrSeed::Spatial, Accumulation::Separate, false, true});
        if (selected("Robust padded"))
            methods.push_back({"Robust padded", true, moonsr::ScalerKind::MoonALSR, sh, false,
                               moonsr::AlsrSeed::Spatial, Accumulation::PaddedArea, true});
        if (selected("Robust tiled"))
            methods.push_back({"Robust tiled", true, moonsr::ScalerKind::MoonALSR, sh, false,
                               moonsr::AlsrSeed::Spatial, Accumulation::GatedSeparate, true, true});
        const std::pair<const char*, Accumulation> accumulationCandidates[] = {
            {"Fused area", Accumulation::FusedBilinear}, {"Fused cubic", Accumulation::FusedCubic},
            {"Gated cubic", Accumulation::GatedSeparate}, {"Point evidence", Accumulation::PointEvidence},
            {"Point residual", Accumulation::PointResidual}, {"Guarded point", Accumulation::GuardedPoint},
            {"Auto sampling", Accumulation::AutoGuarded}, {"Adaptive sampling", Accumulation::AdaptiveGuarded},
            {"Adaptive seed", Accumulation::AdaptiveSeed}, {"Static focus", Accumulation::StaticFocus},
            {"Tiled area", Accumulation::TiledArea}, {"Padded area", Accumulation::PaddedArea},
            {"Cached hull", Accumulation::CachedHull},
            {"Adaptive padded", Accumulation::AdaptivePadded}, {"Adaptive pooled", Accumulation::AdaptivePooled},
            {"Pooled Spatial", Accumulation::PooledSpatial}, {"Pooled fixed count", Accumulation::PooledFixedCount},
            {"Pooled phase", Accumulation::PooledPhase}, {"Pooled footprint", Accumulation::PooledFootprint},
            {"Fast footprint", Accumulation::PooledFootprintFast}
        };
        for (const auto& [name, mode] : accumulationCandidates) {
            if (selected(name))
                methods.push_back({name, true, moonsr::ScalerKind::MoonALSR, sh, false, moonsr::AlsrSeed::Spatial, mode});
        }
        if (selected("Point covariance"))
            methods.push_back({"Point covariance", true, moonsr::ScalerKind::MoonALSR, sh, false,
                               moonsr::AlsrSeed::Covariance, Accumulation::PointEvidence});
        for (const auto& [name, seed] : seedCandidates) {
            const std::string single = std::string("Seed ") + name;
            if (selected(single)) methods.push_back({single, false, moonsr::ScalerKind::MoonALSR, sh, true, seed});
            const std::string temporal = std::string("Temporal ") + name;
            if (seed != moonsr::AlsrSeed::Spatial && selected(temporal))
                methods.push_back({temporal, true, moonsr::ScalerKind::MoonALSR, sh, false, seed});
        }
    }
    if (methods.empty()) { printf("error: no method selected\n"); return 1; }
    const bool recorded = !o.nativeFrames.empty();
    std::vector<fs::path> nativeFiles;
    if (recorded) {
        std::error_code ec;
        for (fs::directory_iterator it(o.nativeFrames, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".png")
                nativeFiles.push_back(it->path());
        }
        if (ec) { printf("error: reading native frame directory: %s\n", ec.message().c_str()); return 1; }
        std::sort(nativeFiles.begin(), nativeFiles.end());
        if (o.frames > 0 && nativeFiles.size() < static_cast<size_t>(o.frames)) {
            printf("error: native frame directory has fewer than %d PNGs\n", o.frames);
            return 1;
        }
    }
    const std::vector<std::string> seqNames = recorded ? std::vector<std::string>{"native_recording"} :
                                                        (o.seqs.empty() ? SequenceNames() : o.seqs);
    const std::vector<std::string> modes = recorded ? std::vector<std::string>{"recorded_area"} : o.modes;
    if (o.frames < o.warmup + kCutWindow + 4 || o.frames < 24) { printf("error: --frames must be >= %d\n", std::max(24, o.warmup + kCutWindow + 4)); return 1; }

    if (recorded) {
        printf("\nTEMPORAL: %d native recording frames, warm-up %d. Input is an area downsample.\nMethods:",
               o.frames, o.warmup);
    } else {
        printf("\nTEMPORAL: GT %ux%u, %d frames per sequence (warm-up %d frames excluded), GT supersampling %dx%d.\n"
               "Inputs: aliased = point sample of the continuous scene at input pixel centres; area = exact box average of the GT.\n"
               "Methods:", o.gtW, o.gtH, o.frames, o.warmup, o.ss, o.ss);
    }
    for (auto& m : methods) printf(" [%s s=%g]", m.label.c_str(), m.sharpness);
    printf("\n");

    std::vector<Record> frameRows, sumRows, timingRows;
    int failures = 0;
    for (const std::string& sn : seqNames) {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Image> gt(static_cast<size_t>(o.frames));
        std::vector<std::vector<float>> gtLuma(static_cast<size_t>(o.frames));
        std::unique_ptr<Sequence> seq;
        if (recorded) {
            if (!ReadPng(nativeFiles.front().wstring(), gt[0], err)) {
                printf("error: loading native reference: %s\n", err.c_str());
                return 1;
            }
            seq = Sequence::CreateRecorded(gt[0].w, gt[0].h, o.frames);
        } else {
            seq = Sequence::Create(sn, o.gtW, o.gtH, o.frames, o.ss);
            if (!seq) { printf("error: unknown sequence '%s'\n", sn.c_str()); return 1; }
        }
        for (int t = 0; t < o.frames; ++t) {
            if (seq->IsStatic() && t > 0) { gt[static_cast<size_t>(t)] = gt[0]; gtLuma[static_cast<size_t>(t)] = gtLuma[0]; continue; }
            auto& frame = gt[static_cast<size_t>(t)];
            if (!recorded) frame = seq->GroundTruth(t);
            else if (t > 0 && (!ReadPng(nativeFiles[static_cast<size_t>(t)].wstring(), frame, err) ||
                                frame.w != seq->W() || frame.h != seq->H())) {
                printf("error: invalid native frame %d or inconsistent dimensions: %s\n", t, err.c_str());
                return 1;
            }
            gtLuma[static_cast<size_t>(t)] = Luma(gt[static_cast<size_t>(t)]);
        }
        uint64_t gtHash = 1469598103934665603ull;
        for (const Image& im : gt) gtHash = Fnv1a(im.px.data(), im.px.size(), gtHash);
        printf("\n[%s] ground truth rendered in %.1f s (FNV-1a of all frames %s)\n", sn.c_str(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), HashHex(gtHash).c_str());

        for (double scale : o.scales) {
            const uint32_t iw = ScaledExtent(seq->W(), scale), ih = ScaledExtent(seq->H(), scale);
            for (const std::string& mode : modes) {
                std::vector<Image> inputs(static_cast<size_t>(o.frames));
                for (int t = 0; t < o.frames; ++t) {
                    if (mode == "aliased") inputs[static_cast<size_t>(t)] = (seq->IsStatic() && t > 0) ? inputs[0] : seq->InputPoint(t, iw, ih);
                    else inputs[static_cast<size_t>(t)] = Sequence::InputArea(gt[static_cast<size_t>(t)], iw, ih);
                }
                const auto edges = MeasureEdgeSteps(inputs, static_cast<size_t>(o.warmup));
                for (const auto& gp : gpus) {
                    std::vector<std::vector<RunOut>> timing(methods.size());
                    if (o.timingSweeps > 1) {
                        for (int sweep = 0; sweep < o.timingSweeps; ++sweep) {
                            for (size_t order = 0; order < methods.size(); ++order) {
                                const size_t index = sweep % 2 == 0 ? order : methods.size() - order - 1;
                                const auto& method = methods[index];
                                RunOut result;
                                if (!TimeMethod(*gp, *seq, inputs, method, result, err)) {
                                    printf("error: timing sweep %d (%s): %s\n", sweep, method.label.c_str(), err.c_str());
                                    ++failures;
                                    continue;
                                }
                                Record row = AdapterFields(gp->Info());
                                row.insert(row.end(), {S("sequence", sn), N("scale", scale, 3), S("input_mode", mode),
                                                       S("method", method.label), N("sharpness", method.sharpness, 3),
                                                       I("sweep", sweep), I("order", static_cast<long long>(order))});
                                const auto average = [&](const std::vector<double>& ms) {
                                    std::vector<double> values;
                                    for (size_t t = static_cast<size_t>(o.warmup); t < ms.size(); ++t)
                                        if (std::isfinite(ms[t])) values.push_back(ms[t]);
                                    return MeanOf(values);
                                };
                                row.push_back(N("gpu_ms_mean", average(result.msMain), 4));
                                row.push_back(N("gpu_motion_ms_mean", average(result.msMotion), 4));
                                for (size_t p = 0; p < kPassNames.size(); ++p)
                                    row.push_back(N(std::string("gpu_pass_") + kPassNames[p] + "_ms_mean",
                                                    average(result.msPass[p]), 4));
                                timingRows.push_back(std::move(row));
                                timing[index].push_back(std::move(result));
                            }
                        }
                    }
                    for (size_t methodIndex = 0; methodIndex < methods.size(); ++methodIndex) {
                        MethodSpec m = methods[methodIndex];
                        m.scoreRefinedFlow = o.scoreRefinedFlow;
                        RunOut r;
                        const bool keep = o.saveImages;
                        if (!RunMethod(*gp, *seq, inputs, gtLuma, m, keep ? o.saveFrame : -1, r, err)) {
                            printf("error: %s %s %gx %s %s: %s\n", Narrow(gp->Info().name).c_str(), sn.c_str(), scale, mode.c_str(), m.label.c_str(), err.c_str());
                            ++failures;
                            continue;
                        }
                        if (o.timingSweeps > 1) {
                            const auto& runs = timing[methodIndex];
                            r.msMain = MedianTiming(runs, inputs.size(), [](const RunOut& v) -> const auto& { return v.msMain; });
                            r.msMotion = MedianTiming(runs, inputs.size(), [](const RunOut& v) -> const auto& { return v.msMotion; });
                            for (size_t p = 0; p < r.msPass.size(); ++p)
                                r.msPass[p] = MedianTiming(runs, inputs.size(),
                                    [p](const RunOut& v) -> const auto& { return v.msPass[p]; });
                        } else if (!TimeMethod(*gp, *seq, inputs, m, r, err)) {
                            printf("error: timing %s %s %gx %s %s: %s\n", Narrow(gp->Info().name).c_str(), sn.c_str(), scale, mode.c_str(), m.label.c_str(), err.c_str());
                            ++failures;
                            r.msMain.assign(inputs.size(), kNaN);
                            r.msMotion.assign(inputs.size(), kNaN);
                            for (auto& pass : r.msPass) pass.assign(inputs.size(), kNaN);
                        }
                        if (keep && !r.keptFrame.Empty()) {
                            const fs::path dir = o.outDir / "images" / "temporal" / sn;
                            std::error_code ec;
                            fs::create_directories(dir, ec);
                            std::string base = G(scale) + "x_" + mode + "_";
                            std::string lab = m.label;
                            for (auto& c : lab) if (!isalnum(static_cast<unsigned char>(c))) c = '_';
                            WritePng((dir / (base + lab + ".png")).wstring(), r.keptFrame, err);
                            if (!r.keptSingle.Empty() &&
                                !WritePng((dir / (base + lab + "_single.png")).wstring(), r.keptSingle, err)) {
                                printf("error: writing adaptive seed snapshot: %s\n", err.c_str());
                                ++failures;
                            }
                            WritePng((dir / (base + "gt.png")).wstring(), gt[static_cast<size_t>(o.saveFrame)], err);
                            WritePng((dir / (base + "input.png")).wstring(), inputs[static_cast<size_t>(o.saveFrame)], err);
                        }
                        Record id = AdapterFields(gp->Info());
                        id.insert(id.end(), {S("sequence", sn), S("content_class", seq->ContentClass()), N("scale", scale, 3),
                                            S("input_mode", mode), S("method", m.label), N("sharpness", m.sharpness, 3)});
                        for (size_t t = 0; t < r.scores.size(); ++t) {
                            const SeqScore& s = r.scores[t];
                            Record fr = {S("kind", "frame")};
                            fr.insert(fr.end(), id.begin(), id.end());
                            fr.push_back(I("frame", static_cast<long long>(t)));
                            if (seq->StopFrame() >= 0) {
                                const int index = static_cast<int>(t);
                                fr.push_back(S("motion_phase", index < seq->StopFrame() ? "initial_pan" :
                                    (index < seq->ResumeFrame() ? "static_hold" : "resumed_pan")));
                            }
                            fr.push_back(I("warmup", t < static_cast<size_t>(o.warmup) ? 1 : 0));
                            fr.push_back(N("psnr_luma_db", s.psnr, 4)); fr.push_back(N("ssim", s.ssim, 5));
                            fr.push_back(N("flicker", s.flicker, 4));
                            if (s.hasHud) { fr.push_back(N("hud_psnr_luma_db", s.hudPsnr, 4)); fr.push_back(N("hud_ssim", s.hudSsim, 5)); fr.push_back(N("hud_flicker", s.hudFlicker, 4)); }
                            if (std::isfinite(s.staticBlend)) fr.push_back(N("static_blend", s.staticBlend, 3));
                            if (s.samplingModel >= 0) {
                                fr.push_back(I("sampling_model", s.samplingModel));
                                fr.push_back(I("sampling_edges", s.samplingEdgeCount));
                                fr.push_back(N("sampling_concentration", s.samplingConcentration, 5));
                                fr.push_back(N("sampling_abrupt_fraction", s.samplingAbruptFraction, 5));
                                fr.push_back(I("sampling_point_candidate", s.samplingPointCandidate));
                                fr.push_back(I("sampling_restart", s.samplingRestart));
                            }
                            if (m.scoreRefinedFlow && m.alsr) {
                                fr.push_back(N("refined_flow_epe_input_px", s.refinedFlowEpe, 4));
                                fr.push_back(N("moving_refined_flow_epe_input_px", s.movingRefinedFlowEpe, 4));
                            }
                            fr.push_back(N("flow_epe_input_px", s.flowEpe, 4));
                            fr.push_back(N("gpu_ms", r.msMain[t], 4));
                            fr.push_back(N("gpu_motion_ms", r.msMotion[t], 4));
                            if (m.alsr) {
                                for (size_t p = 0; p < kPassNames.size(); ++p)
                                    fr.push_back(N(std::string("gpu_pass_") + kPassNames[p] + "_ms", r.msPass[p][t], 4));
                            }
                            frameRows.push_back(std::move(fr));
                        }
                        const Agg a = Aggregate(r, o.warmup, seq->CutFrame());
                        Record sr = {S("kind", "summary")};
                        sr.insert(sr.end(), id.begin(), id.end());
                        sr.push_back(I("in_w", iw)); sr.push_back(I("in_h", ih)); sr.push_back(I("gt_w", seq->W())); sr.push_back(I("gt_h", seq->H()));
                        sr.push_back(I("frames", o.frames)); sr.push_back(I("warmup_frames", o.warmup)); sr.push_back(I("scored_frames", a.n));
                        sr.push_back(S("gt_fnv1a", HashHex(gtHash))); sr.push_back(S("out_fnv1a", HashHex(r.outHash)));
                        sr.push_back(N("psnr_luma_db", a.psnr, 4)); sr.push_back(N("psnr_luma_min_db", a.psnrMin, 4)); sr.push_back(N("ssim", a.ssim, 5));
                        sr.push_back(N("flicker", a.flicker, 4));
                        sr.push_back(N("hud_psnr_luma_db", a.hudPsnr, 4)); sr.push_back(N("hud_ssim", a.hudSsim, 5)); sr.push_back(N("hud_flicker", a.hudFlicker, 4));
                        sr.push_back(N("cut_window_psnr_luma_db", a.cutPsnr, 4)); sr.push_back(N("cut_window_ssim", a.cutSsim, 5));
                        sr.push_back(N("cut_frame_psnr_luma_db", a.cutFramePsnr, 4));
                        if (m.scoreRefinedFlow && m.alsr) {
                            std::vector<double> refined, moving;
                            for (size_t t = static_cast<size_t>(o.warmup); t < r.scores.size(); ++t) {
                                if (std::isfinite(r.scores[t].refinedFlowEpe)) refined.push_back(r.scores[t].refinedFlowEpe);
                                if (std::isfinite(r.scores[t].movingRefinedFlowEpe)) moving.push_back(r.scores[t].movingRefinedFlowEpe);
                            }
                            sr.push_back(N("refined_flow_epe_input_px", MeanOf(refined), 4));
                            sr.push_back(N("moving_refined_flow_epe_input_px", MeanOf(moving), 4));
                        }
                        sr.push_back(N("flow_epe_input_px", a.flowEpe, 4));
                        if (moonsr::UsesSamplingModel(m.accumulation)) {
                            std::array<int, 3> classified{};
                            for (size_t t = static_cast<size_t>(o.warmup); t < r.scores.size(); ++t) {
                                const int model = r.scores[t].samplingModel;
                                if (model >= 0 && model < static_cast<int>(classified.size()))
                                    ++classified[static_cast<size_t>(model)];
                            }
                            sr.push_back(I("sampling_unknown_frames", classified[0]));
                            sr.push_back(I("sampling_area_frames", classified[1]));
                            sr.push_back(I("sampling_point_frames", classified[2]));
                        }
                        sr.push_back(I("edge_step_count", edges.count));
                        const double edgeCount = static_cast<double>(edges.count);
                        sr.push_back(N("edge_step_mean", edges.count ? edges.sum / edgeCount : kNaN, 5));
                        sr.push_back(N("edge_step_abrupt_fraction",
                                       edges.count ? static_cast<double>(edges.abrupt) / edgeCount : kNaN, 5));
                        if (seq->StopFrame() >= 0) {
                            const auto reportTransition = [&sr, &r](int frame, const char* label) {
                                std::vector<double> flicker;
                                for (size_t t = static_cast<size_t>(frame); t < r.scores.size() &&
                                     t < static_cast<size_t>(frame) + 4; ++t) {
                                    if (std::isfinite(r.scores[t].flicker)) flicker.push_back(r.scores[t].flicker);
                                }
                                sr.push_back(I(std::string(label) + "_frame", frame));
                                sr.push_back(N(std::string(label) + "_four_frame_flicker", MeanOf(flicker), 4));
                                sr.push_back(N(std::string(label) + "_peak_flicker",
                                               flicker.empty() ? kNaN : *std::max_element(flicker.begin(), flicker.end()), 4));
                            };
                            reportTransition(seq->StopFrame(), "stop");
                            reportTransition(seq->ResumeFrame(), "resume");
                            double holdPeak = 0.0;
                            for (int index = seq->StopFrame(); index < seq->ResumeFrame(); ++index) {
                                const double flicker = r.scores[static_cast<size_t>(index)].flicker;
                                if (std::isfinite(flicker)) holdPeak = std::max(holdPeak, flicker);
                            }
                            sr.push_back(N("hold_peak_flicker", holdPeak, 4));
                        }
                        sr.push_back(N("gpu_ms_mean", a.msMean, 4)); sr.push_back(N("gpu_ms_median", a.msMed, 4));
                        sr.push_back(N("gpu_ms_p95", a.msP95, 4)); sr.push_back(N("gpu_ms_max", a.msMax, 4));
                        sr.push_back(N("gpu_motion_ms_mean", a.motMean, 4)); sr.push_back(N("gpu_motion_ms_median", a.motMed, 4));
                        if (m.alsr) {
                            for (size_t p = 0; p < kPassNames.size(); ++p) {
                                double sum = 0.0;
                                size_t count = 0;
                                for (size_t t = static_cast<size_t>(o.warmup); t < r.msPass[p].size(); ++t) {
                                    if (!std::isfinite(r.msPass[p][t])) continue;
                                    sum += r.msPass[p][t];
                                    ++count;
                                }
                                sr.push_back(N(std::string("gpu_pass_") + kPassNames[p] + "_ms_mean",
                                               count ? sum / static_cast<double>(count) : kNaN, 4));
                            }
                        }
                        sr.push_back(I("disjoint_frames", a.disjoint));
                        sumRows.push_back(sr);
                        printf("  %-18s %gx %-7s %-16s %-16s PSNR %7.3f dB  SSIM %.4f  flicker %.3f  %.3f ms\n", Narrow(gp->Info().name).c_str(), scale,
                               mode.c_str(), sn.c_str(), m.label.c_str(), a.psnr, a.ssim, a.flicker, a.msMean);
                    }
                }
            }
        }
    }

    // Console summary per (adapter, scale, mode).
    std::map<std::string, std::vector<Record>> groups;
    std::vector<std::string> order;
    auto get = [](const Record& r, const char* k) { for (auto& f : r) if (f.key == k) return f.val; return std::string(); };
    for (const auto& r : sumRows) {
        const std::string key = get(r, "adapter") + " | " + get(r, "scale") + "x | " + get(r, "input_mode");
        if (!groups.count(key)) order.push_back(key);
        groups[key].push_back({S("content", get(r, "content_class")), S("sequence", get(r, "sequence")), S("method", get(r, "method")), S("PSNR", get(r, "psnr_luma_db")), S("SSIM", get(r, "ssim")),
                               S("flicker", get(r, "flicker")), S("HUD PSNR", get(r, "hud_psnr_luma_db")), S("cut8 PSNR", get(r, "cut_window_psnr_luma_db")),
                               S("gpu ms", get(r, "gpu_ms_mean")), S("motion ms", get(r, "gpu_motion_ms_mean")), S("flow EPE", get(r, "flow_epe_input_px"))});
    }
    for (const auto& k : order) {
        printf("\n== Temporal summary: %s (luma PSNR dB / SSIM / error flicker in levels, means over frames >= %d) ==\n", k.c_str(), o.warmup);
        PrintTable(groups[k], {"content", "sequence", "method", "PSNR", "SSIM", "flicker", "HUD PSNR", "cut8 PSNR", "gpu ms", "motion ms", "flow EPE"});
    }

    Record meta = {S("tool", "moonsr_bench"), S("mode", "temporal"), S("os_build", si.osBuild), S("cpu", si.cpu), S("git_commit", si.gitCommit),
                   S("source_hash", si.sourceHash), S("date", si.date), S("command_line", si.commandLine)};
    std::string e2;
    const fs::path base = o.outDir / "results";
    if (o.timingSweeps > 1) {
        const auto path = base / ("temporal_sweeps_" + tag + ".csv");
        if (!WriteCsv(path, timingRows, e2)) { printf("error: %s\n", e2.c_str()); ++failures; }
        else printf("wrote %s\n", path.string().c_str());
    }
    if (WriteCsv(base / ("temporal_summary_" + tag + ".csv"), sumRows, e2)) printf("wrote %s\n", (base / ("temporal_summary_" + tag + ".csv")).string().c_str()); else printf("warning: %s\n", e2.c_str());
    if (WriteCsv(base / ("temporal_frames_" + tag + ".csv"), frameRows, e2)) printf("wrote %s\n", (base / ("temporal_frames_" + tag + ".csv")).string().c_str()); else printf("warning: %s\n", e2.c_str());
    if (!WriteJson(base / ("temporal_summary_" + tag + ".json"), meta, sumRows, e2)) printf("warning: %s\n", e2.c_str());
    return failures ? 1 : 0;
}

int ScoreCaptureSet(const fs::path& captures, const fs::path& results, const std::string& tag) {
    const auto fail = [](const std::string& error) { printf("capture set: %s\n", error.c_str()); return 1; };
    try {
        const auto pngNames = [](const fs::path& dir) {
            std::vector<fs::path> names;
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".png")
                    names.push_back(entry.path().filename());
            }
            std::sort(names.begin(), names.end());
            return names;
        };
        const auto frames = pngNames(captures / "native");
        if (frames.empty()) return fail("native/ must have PNG frames");
        if (pngNames(captures / "input") != frames) return fail("input/ must match the native frame filenames");
        std::vector<std::string> variants{"moonsr"};
        for (const char* vendor : {"dlss", "fsr", "xess"}) {
            if (fs::exists(captures / vendor)) variants.emplace_back(vendor);
        }
        for (const auto& variant : variants) {
            if (pngNames(captures / variant) != frames) return fail(variant + "/ must match the native frame filenames");
        }

        std::vector<std::unique_ptr<SeqScorer>> scorers;
        std::vector<Record> rows;
        Image native, input, output;
        std::string err;
        uint32_t width = 0, height = 0, inputWidth = 0, inputHeight = 0;
        for (const auto& frame : frames) {
            if (!ReadPng((captures / "native" / frame).wstring(), native, err) ||
                !ReadPng((captures / "input" / frame).wstring(), input, err)) return fail(err);
            if (scorers.empty()) {
                width = native.w;
                height = native.h;
                inputWidth = input.w;
                inputHeight = input.h;
                for (size_t i = 0; i < variants.size(); ++i)
                    scorers.push_back(std::make_unique<SeqScorer>(width, height, std::vector<HudRect>{}));
            }
            if (native.w != width || native.h != height || input.w != inputWidth || input.h != inputHeight)
                return fail("frame dimensions changed in native/ or input/");
            const auto luma = Luma(native);
            for (size_t i = 0; i < variants.size(); ++i) {
                const auto& variant = variants[i];
                if (!ReadPng((captures / variant / frame).wstring(), output, err)) return fail(err);
                if (output.w != width || output.h != height)
                    return fail(variant + "/" + frame.string() + " must match native dimensions");
                const auto score = scorers[i]->Add(luma, output);
                rows.push_back({S("frame", frame.string()), S("variant", variant), I("input_width", inputWidth),
                                I("input_height", inputHeight), I("native_width", width), I("native_height", height),
                                N("psnr_luma_db", score.psnr, 4), N("ssim", score.ssim, 5), N("flicker", score.flicker, 4)});
            }
        }
        const auto csv = results / ("capture_set_" + tag + ".csv");
        if (!WriteCsv(csv, rows, err)) return fail(err);
        printf("Scored %zu native frames for %zu variants. Wrote %s\n", frames.size(), variants.size(), csv.string().c_str());
        return 0;
    } catch (const fs::filesystem_error& error) {
        return fail(error.what());
    }
}

// ------------------------------------------------------------------------------------------------------------------
// Self-test

namespace {
int g_fail = 0;
void Check(bool ok, const char* name, const std::string& detail = "") {
    printf("[%s] temporal: %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " : ", detail.c_str());
    if (!ok) ++g_fail;
}
std::string F(const char* fmt, double a, double b = 0, double c = 0) {
    char buf[200];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    return buf;
}
uint64_t HashImg(const Image& im) { return Fnv1a(im.px.data(), im.px.size()); }

Image RandImg(uint32_t w, uint32_t h, uint32_t seed, int lo, int hi) {
    Image img;
    img.Alloc(w, h);
    uint32_t s = seed;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        s = s * 1664525u + 1013904223u;
        const uint8_t v = static_cast<uint8_t>(lo + (s >> 8) % static_cast<uint32_t>(hi - lo + 1));
        img.px[i * 4 + 0] = img.px[i * 4 + 1] = img.px[i * 4 + 2] = v; // grey: luma == value exactly
    }
    return img;
}
Image Offset(const Image& a, int delta, const HudRect* only = nullptr) {
    Image o = a;
    for (uint32_t y = 0; y < o.h; ++y)
        for (uint32_t x = 0; x < o.w; ++x) {
            if (only && !(static_cast<int>(x) >= only->x0 && static_cast<int>(x) < only->x1 && static_cast<int>(y) >= only->y0 && static_cast<int>(y) < only->y1)) continue;
            for (int c = 0; c < 3; ++c) o.Row(y)[x * 4 + c] = static_cast<uint8_t>(std::clamp<int>(o.Row(y)[x * 4 + c] + delta, 0, 255));
        }
    return o;
}
} // namespace

int RunTemporalSelfTest(GpuRunner* gpu) {
    g_fail = 0;
    // Scene generators: determinism, non-trivial content, motion present / absent.
    for (const auto& name : SequenceNames()) {
        auto s = Sequence::Create(name, 192, 108, 40, 2);
        const Image a = s->GroundTruth(5), b = s->GroundTruth(5), c = s->GroundTruth(6);
        double mean = 0, var = 0;
        const auto l = Luma(a);
        for (float v : l) mean += v;
        mean /= static_cast<double>(l.size());
        for (float v : l) var += (v - mean) * (v - mean);
        var /= static_cast<double>(l.size());
        const bool moving = name != "still";
        Check(HashImg(a) == HashImg(b) && var > 25.0 && ((HashImg(a) != HashImg(c)) == moving),
              ("scene '" + name + "' is deterministic, textured and " + (moving ? "moving" : "static")).c_str(), F("luma std %.1f", std::sqrt(var)));
    }
    {
        auto s = Sequence::Create("scene_cut", 192, 108, 40, 2);
        Check(s->CutFrame() == 20, "scene_cut cut frame is frames/2", std::to_string(s->CutFrame()));
        auto h = Sequence::Create("hud_pan", 1920, 1080, 4, 1);
        Check(h->Hud().size() == 4 && h->Hud()[0].x0 >= 0 && h->Hud()[1].x1 <= 1920 && h->Hud()[2].y1 <= 1080, "hud_pan declares four in-bounds overlay rectangles");
        // The overlay must be static: pixels inside the health bar frame are identical across frames while the world beneath pans.
        auto h2 = Sequence::Create("hud_pan", 480, 270, 6, 2);
        const Image f0 = h2->GroundTruth(0), f3 = h2->GroundTruth(3);
        Check(Diff(f0, f3).differing > 0, "hud_pan content moves between frames");
    }
    // Known shift: an integer-velocity pan translates the ground truth exactly.
    {
        auto s = Sequence::CreatePan(256, 144, 4, 2, 4.0, 2.0);
        double dx = 0, dy = 0;
        Check(s->BackwardFlow(16, 8, 1, dx, dy) && dx == -4 && dy == -2 &&
                  !s->BackwardFlow(16, 8, 0, dx, dy) && !s->BackwardFlow(2, 1, 1, dx, dy),
              "backward flow has the correct direction and excludes unmatched samples");
        const Image a = s->GroundTruth(0), b = s->GroundTruth(1);
        uint64_t bad = 0, tot = 0;
        int maxd = 0;
        for (uint32_t y = 0; y + 2 < 144; ++y)
            for (uint32_t x = 0; x + 4 < 256; ++x)
                for (int c = 0; c < 3; ++c) {
                    const int d = std::abs(static_cast<int>(b.Row(y + 2)[(x + 4) * 4 + c]) - static_cast<int>(a.Row(y)[x * 4 + c]));
                    maxd = std::max(maxd, d);
                    bad += d != 0;
                    ++tot;
                }
        Check(maxd <= 1 && bad * 10000 <= tot, "known shift: pan of (4,2) px/frame translates GT(t) to GT(t+1) exactly", F("mismatching samples %.0f of %.0f, max diff %.0f", static_cast<double>(bad), static_cast<double>(tot), maxd));
    }
    // Sampling modes: point sampling at 1x of an ss=1 render is the render itself; 2x area average is the box mean.
    {
        auto s = Sequence::Create("pan_slow", 160, 90, 4, 1);
        const Image gt = s->GroundTruth(2);
        Check(Diff(gt, s->InputPoint(2, 160, 90)).maxAbs == 0, "point sampling at 1x equals the ss=1 ground truth");
        auto s3 = Sequence::Create("pan_slow", 160, 90, 4, 3);
        const Image g3 = s3->GroundTruth(2), area = Sequence::InputArea(g3, 80, 45);
        int worst = 0;
        for (uint32_t y = 0; y < 45; ++y)
            for (uint32_t x = 0; x < 80; ++x)
                for (int c = 0; c < 3; ++c) {
                    const int sum = g3.Row(2 * y)[2 * x * 4 + c] + g3.Row(2 * y)[(2 * x + 1) * 4 + c] + g3.Row(2 * y + 1)[2 * x * 4 + c] + g3.Row(2 * y + 1)[(2 * x + 1) * 4 + c];
                    worst = std::max(worst, std::abs(static_cast<int>(area.Row(y)[x * 4 + c]) - static_cast<int>(std::lround(sum / 4.0))));
                }
        Check(worst == 0, "2x area input equals the 2x2 box mean of the ground truth", F("max diff %.0f", worst));
    }
    // Scorer: identity, constant offset, alternating offset (flicker), HUD-region offset.
    {
        const Image a = RandImg(96, 64, 3, 60, 180);
        const auto gl = Luma(a);
        SeqScorer sc(96, 64, {});
        const SeqScore s0 = sc.Add(gl, a), s1 = sc.Add(gl, a);
        Check(std::isinf(s1.psnr) && s1.ssim > 0.99999 && s1.flicker == 0.0 && std::isnan(s0.flicker), "identity: PSNR inf, SSIM 1, flicker 0 (NaN on the first frame)");
        SeqScorer sc2(96, 64, {});
        sc2.Add(gl, Offset(a, 5));
        const SeqScore c1 = sc2.Add(gl, Offset(a, 5));
        Check(std::abs(c1.psnr - 20.0 * std::log10(255.0 / 5.0)) < 1e-3 && c1.flicker == 0.0, "constant +5 offset: PSNR = 20 log10(255/5) = 34.151 dB, flicker 0", F("PSNR %.4f", c1.psnr));
        SeqScorer sc3(96, 64, {});
        sc3.Add(gl, Offset(a, 5));
        const SeqScore alt = sc3.Add(gl, Offset(a, -5));
        Check(std::abs(alt.flicker - 10.0) < 1e-4, "alternating +5/-5 error: flicker = 10 levels", F("flicker %.5f", alt.flicker));
        const HudRect r{16, 8, 48, 40};
        SeqScorer sc4(96, 64, {r});
        const SeqScore h = sc4.Add(gl, Offset(a, 8, &r));
        const double expectHud = 20.0 * std::log10(255.0 / 8.0), expectAll = expectHud + 10.0 * std::log10(96.0 * 64.0 / (32.0 * 32.0));
        Check(h.hasHud && std::abs(h.hudPsnr - expectHud) < 1e-3 && std::abs(h.psnr - expectAll) < 1e-3, "error confined to the HUD rectangle: HUD and frame PSNR match the analytic values",
              F("hud %.3f (exp %.3f), frame %.3f", h.hudPsnr, expectHud, h.psnr));
    }
    {
        std::vector<Image> hard(25), soft(25);
        for (size_t t = 0; t < hard.size(); ++t) {
            hard[t].Alloc(12, 12);
            soft[t].Alloc(12, 12);
            for (uint32_t y = 0; y < 12; ++y) {
                for (uint32_t x = 0; x < 12; ++x) {
                    for (uint32_t c = 0; c < 3; ++c) {
                        hard[t].Row(y)[x * 4 + c] = x < 6 ? 0 : 200;
                        soft[t].Row(y)[x * 4 + c] = x < 6 ? 0 : (x == 6 ? 100 : 200);
                    }
                }
            }
        }
        const auto sharp = MeasureEdgeSteps(hard, 0);
        const auto spread = MeasureEdgeSteps(soft, 0);
        Check(sharp.count > 0 && sharp.abrupt == sharp.count && spread.count > 0 &&
                  spread.abrupt < spread.count && spread.sum / static_cast<double>(spread.count) < 0.9,
              "edge-step statistics distinguish ideal one-step and two-step transitions");
    }
    {
        const auto startStop = Sequence::Create("start_stop", 160, 90, 24, 1);
        double dx = 0, dy = 0;
        const int stop = startStop->StopFrame(), resume = startStop->ResumeFrame();
        const bool staticFlow = startStop->BackwardFlow(80, 45, stop, dx, dy) && dx == 0.0 && dy == 0.0;
        const bool resumeFlow = startStop->BackwardFlow(80, 45, resume, dx, dy) && dx < 0.0 && dy < 0.0;
        const bool stillFrames = Diff(startStop->GroundTruth(stop - 1), startStop->GroundTruth(resume - 1)).maxAbs == 0;
        Check(staticFlow && resumeFlow && stillFrames,
              "start/stop sequence has an exact static hold and resumes motion without a scene cut");
    }
    // GPU paths.
    if (gpu) {
        auto seq = Sequence::Create("pan_slow", 480, 270, 8, 2);
        auto prep = [&](double scale, bool aliased, std::vector<Image>& in, std::vector<std::vector<float>>& gl, std::vector<Image>& gts) {
            const uint32_t iw = ScaledExtent(480, scale), ih = ScaledExtent(270, scale);
            for (int t = 0; t < seq->Frames(); ++t) {
                gts.push_back(seq->GroundTruth(t));
                gl.push_back(Luma(gts.back()));
                in.push_back(aliased ? seq->InputPoint(t, iw, ih) : Sequence::InputArea(gts.back(), iw, ih));
            }
        };
        std::string err;
        {
            std::vector<Image> in, gts;
            std::vector<std::vector<float>> gl;
            prep(1.0, false, in, gl, gts);
            RunOut r;
            MethodSpec nearest{"Nearest", false, moonsr::ScalerKind::Nearest, 0.0f};
            const bool ok = RunMethod(*gpu, *seq, in, gl, nearest, 3, r, err);
            bool inf = ok;
            for (const auto& s : r.scores) inf = inf && std::isinf(s.psnr) && s.ssim > 0.99999;
            Check(ok && inf && Diff(r.keptFrame, gts[3]).maxAbs == 0, "end to end identity: 1x area input through the GPU runner reproduces the ground truth (PSNR inf)", err);
        }
        {
            std::vector<Image> in, gts;
            std::vector<std::vector<float>> gl;
            prep(2.0, true, in, gl, gts);
            RunOut r;
            MethodSpec lz{"Lanczos-3", false, moonsr::ScalerKind::Lanczos, 0.0f};
            const bool ok = RunMethod(*gpu, *seq, in, gl, lz, 5, r, err);
            Image ref;
            const bool ok2 = ok && gpu->Upscale(in[5], 480, 270, moonsr::ScalerKind::Lanczos, 0.0f, ref, err);
            Check(ok2 && Diff(ref, r.keptFrame).maxAbs == 0, "runner output (persistent texture updated per frame) equals the one-shot ScalePass output", err);
            const auto direct = Compare(gts[5], ref);
            Check(ok2 && std::abs(r.scores[5].psnr - direct.psnrLuma) < 1e-6 && std::abs(r.scores[5].ssim - direct.ssim) < 1e-9,
                  "per-frame luma PSNR / SSIM equal Compare() on the same pair", ok2 ? F("%.4f dB vs %.4f dB", r.scores[5].psnr, direct.psnrLuma) : err);
            RunOut rt;
            const bool okt = ok && TimeMethod(*gpu, *seq, in, lz, rt, err);
            Check(okt && rt.msMain.size() == 8 && std::count_if(rt.msMain.begin(), rt.msMain.end(), [](double v) { return !std::isnan(v) && v > 0.0; }) >= 6,
                  "GPU timestamp queries (timing pass) return positive times", okt ? F("frame 5: %.4f ms", rt.msMain[5]) : err);
        }
        {
            std::vector<Image> in, gts;
            std::vector<std::vector<float>> gl;
            prep(1.5, true, in, gl, gts);
            RunOut r1, r2;
            MethodSpec al{"MoonSR", true, moonsr::ScalerKind::MoonALSR, 0.0f};
            const bool ok = RunMethod(*gpu, *seq, in, gl, al, -1, r1, err) && RunMethod(*gpu, *seq, in, gl, al, -1, r2, err);
            bool fin = ok;
            if (ok) for (const auto& s : r1.scores) fin = fin && std::isfinite(s.psnr) && s.psnr > 10.0;
            Check(ok && fin && r1.outHash == r2.outHash, "MoonSR temporal through FrameGenerator + AlsrUpscaler runs and is bit-deterministic across two runs",
                  ok ? ("FNV " + HashHex(r1.outHash) + " vs " + HashHex(r2.outHash)) : err);
            Check(ok && std::isnan(r1.scores.front().flowEpe) &&
                      std::isfinite(r1.scores.back().flowEpe) && r1.scores.back().flowEpe >= 0,
                  "flow readback scores a valid pair and omits the first frame", err);
            const bool timed = ok && TimeMethod(*gpu, *seq, in, al, r1, err);
            const auto hasPass = [&r1](moonsr::AlsrPass p) {
                const auto& times = r1.msPass[static_cast<size_t>(p)];
                return times.size() == 8 && std::isfinite(times.back());
            };
            Check(timed && hasPass(moonsr::AlsrPass::Prep) && hasPass(moonsr::AlsrPass::ReferenceBlur) &&
                      !hasPass(moonsr::AlsrPass::Final),
                  "ALSR pass timing records both ends of the pipeline and omits skipped sharpening", err);
        }
        {
            std::vector<Image> in, gts;
            std::vector<std::vector<float>> gl;
            prep(1.6, true, in, gl, gts); // 300x169 source includes partial groups on both axes.
            RunOut reference, tiled;
            MethodSpec original{"MoonSR", true, moonsr::ScalerKind::MoonALSR, 0.0f};
            MethodSpec fused = original;
            fused.tiledReference = true;
            const bool ok = RunMethod(*gpu, *seq, in, gl, original, -1, reference, err) &&
                            RunMethod(*gpu, *seq, in, gl, fused, -1, tiled, err);
            Check(ok && std::abs(reference.scores.back().psnr - tiled.scores.back().psnr) < 0.05,
                  "tiled reference matches the two-pass quality with partial work groups",
                  ok ? F("%.4f dB vs %.4f dB", reference.scores.back().psnr, tiled.scores.back().psnr) : err);
            MethodSpec padded = original;
            padded.accumulation = moonsr::AlsrAccumulation::PaddedArea;
            MethodSpec cached = padded;
            cached.accumulation = moonsr::AlsrAccumulation::CachedHull;
            RunOut paddedOutput, cachedOutput;
            const bool cachedOk = RunMethod(*gpu, *seq, in, gl, padded, -1, paddedOutput, err) &&
                                  RunMethod(*gpu, *seq, in, gl, cached, -1, cachedOutput, err);
            Check(cachedOk && paddedOutput.outHash == cachedOutput.outHash,
                  "cached hull matches padded area across partial groups and image edges", err);
        }
        {
            // A high-contrast source with abrupt transitions supplies unambiguous test evidence.
            // Switching to flat input must immediately withdraw the point classification.
            std::vector<Image> inputs(8);
            std::vector<std::vector<float>> gtLuma;
            const Image gt = seq->GroundTruth(0);
            for (size_t t = 0; t < inputs.size(); ++t) {
                auto& frame = inputs[t];
                frame.Alloc(320, 180);
                for (uint32_t y = 0; y < frame.h; ++y) {
                    uint8_t* row = frame.Row(y);
                    for (uint32_t x = 0; x < frame.w; ++x) {
                        const uint8_t value = t >= 4 ? 96 : ((x / 8) % 2 ? 64 : 192);
                        row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = value;
                    }
                }
                gtLuma.push_back(Luma(gt));
            }
            MethodSpec model{"Auto sampling", true, moonsr::ScalerKind::MoonALSR, 0.0f};
            model.accumulation = moonsr::AlsrAccumulation::AutoGuarded;
            RunOut detected;
            const bool ok = RunMethod(*gpu, *seq, inputs, gtLuma, model, -1, detected, err);
            Check(ok && detected.scores[0].samplingModel != 2 && detected.scores[1].samplingModel != 2 &&
                      detected.scores[2].samplingModel == 2 && detected.scores[3].samplingModel == 2 &&
                      detected.scores[4].samplingModel == 0,
                  "GPU sampling detector waits for three point frames and falls back on flat input", err);
            Check(ok && detected.scores[0].samplingRestart && detected.scores[0].samplingPointCandidate &&
                      detected.scores[2].samplingEdgeCount >= 128 && detected.scores[2].samplingConcentration > 0.99 &&
                      !detected.scores[4].samplingPointCandidate && detected.scores[4].samplingEdgeCount == 0,
                  "GPU sampling evidence reports abrupt edges, reset and flat input", err);
            model.accumulation = moonsr::AlsrAccumulation::AdaptiveSeed;
            const auto matchesSeed = [&](int frame, moonsr::AlsrSeed seed, float sharpness, bool point) {
                model.sharpness = sharpness;
                MethodSpec reference{"Seed", false, moonsr::ScalerKind::MoonALSR, sharpness};
                reference.single = true;
                reference.seed = seed;
                RunOut adaptive, expected;
                return RunMethod(*gpu, *seq, inputs, gtLuma, model, frame, adaptive, err) &&
                       RunMethod(*gpu, *seq, inputs, gtLuma, reference, frame, expected, err) &&
                       (adaptive.scores[frame].samplingModel == 2) == point &&
                       !adaptive.keptSingle.Empty() && !expected.keptFrame.Empty() &&
                       Diff(adaptive.keptSingle, expected.keptFrame).maxAbs == 0;
            };
            const bool spatial = matchesSeed(1, moonsr::AlsrSeed::Spatial, 0.0f, false);
            Check(spatial, "adaptive generated-frame seed uses Spatial before point confirmation", err);
            const bool covariance = matchesSeed(2, moonsr::AlsrSeed::Covariance, 0.0f, true);
            Check(covariance, "adaptive generated-frame seed uses covariance after point confirmation", err);
            const bool sharpened = matchesSeed(2, moonsr::AlsrSeed::Covariance, 0.5f, true);
            Check(sharpened, "adaptive generated-frame seed still follows the point decision with sharpening", err);
            model.accumulation = moonsr::AlsrAccumulation::AdaptivePooled;
            const bool pooledPoint = matchesSeed(2, moonsr::AlsrSeed::Covariance, 0.0f, true);
            const bool pooledFlat = matchesSeed(4, moonsr::AlsrSeed::Spatial, 0.0f, false);
            Check(pooledPoint && pooledFlat, "pooled evidence chooses covariance for confirmed edges and drops it on flat input", err);
        }
        {
            std::vector<Image> inputs(8);
            std::vector<std::vector<float>> luma;
            const auto truth = Luma(seq->GroundTruth(0));
            for (size_t t = 0; t < inputs.size(); ++t) {
                inputs[t].Alloc(320, 180);
                const uint8_t shade = t == 0 ? 80 : (t == 1 ? 83 : 120);
                for (size_t i = 0; i < inputs[t].px.size(); i += 4) {
                    inputs[t].px[i] = inputs[t].px[i + 1] = inputs[t].px[i + 2] = shade;
                }
                luma.push_back(truth);
            }
            MethodSpec lite{"MoonSR Lite", false, moonsr::ScalerKind::MoonALSR};
            lite.lite = true;
            RunOut subtle, cut;
            const bool ok = RunMethod(*gpu, *seq, inputs, luma, lite, 1, subtle, err) &&
                            RunMethod(*gpu, *seq, inputs, luma, lite, 2, cut, err);
            Check(ok && subtle.keptFrame.px[0] == 82 && cut.keptFrame.px[0] == 120,
                  "Lite damps one small change and immediately accepts a large change", err);
        }
    } else {
        printf("-- temporal GPU checks skipped (no adapter selected)\n");
    }
    return g_fail;
}

} // namespace bench
