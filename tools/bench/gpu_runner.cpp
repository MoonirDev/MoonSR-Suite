#include "gpu_runner.h"

#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace bench {

using moonsr::ScalerKind;

std::string DriverVersionString(uint64_t v) {
    char b[64];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", static_cast<unsigned>((v >> 48) & 0xFFFF), static_cast<unsigned>((v >> 32) & 0xFFFF),
             static_cast<unsigned>((v >> 16) & 0xFFFF), static_cast<unsigned>(v & 0xFFFF));
    return b;
}

namespace {
std::string HrStr(const char* what, HRESULT hr) {
    char b[128];
    snprintf(b, sizeof(b), "%s failed (0x%08X)", what, static_cast<unsigned>(hr));
    return b;
}

// Waits for a query to produce data. Returns false on error or after 10 s.
bool WaitQuery(ID3D11DeviceContext* ctx, ID3D11Query* q, void* data, UINT size) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const HRESULT hr = ctx->GetData(q, data, size, 0);
        if (hr == S_OK) return true;
        if (FAILED(hr)) return false;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) return false;
        std::this_thread::yield();
    }
}

bool CreateTargets(ID3D11Device* dev, uint32_t w, uint32_t h, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv,
                   std::string& err) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
    if (FAILED(hr)) { err = HrStr("create render target", hr); return false; }
    return true;
}

bool CreateSource(ID3D11Device* dev, const Image& in, ComPtr<ID3D11Texture2D>& tex, std::string& err) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = in.w;
    td.Height = in.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{in.px.data(), in.w * 4, 0};
    const HRESULT hr = dev->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr)) { err = HrStr("create source texture", hr); return false; }
    return true;
}
} // namespace

std::unique_ptr<GpuRunner> GpuRunner::Open(const moonsr::AdapterInfo& info, std::string& err) {
    auto r = std::unique_ptr<GpuRunner>(new GpuRunner());
    r->info_ = info;
    std::wstring werr;
    r->dev_ = moonsr::GpuDevice::Create(&info.luid, werr);
    if (!r->dev_) { err = "GpuDevice::Create: " + Narrow(werr); return nullptr; }
    r->pass_ = moonsr::ScalePass::Create(*r->dev_, werr);
    if (!r->pass_) { err = "ScalePass::Create: " + Narrow(werr); return nullptr; }
    return r;
}

bool GpuRunner::Upscale(const Image& in, uint32_t dw, uint32_t dh, ScalerKind scaler, float sharpness, Image& out,
                        std::string& err) {
    ID3D11Device* dev = dev_->Device();
    ID3D11DeviceContext* ctx = dev_->Context();
    ComPtr<ID3D11Texture2D> src, dst, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (!CreateSource(dev, in, src, err) || !CreateTargets(dev, dw, dh, dst, rtv, err)) return false;
    pass_->ResetSourceCache(); // the cache is keyed by texture pointer; a recycled address must not hit a stale view

    moonsr::ScaleSettings s;
    s.scaler = scaler;
    s.fit = moonsr::FitMode::Stretch;
    s.sharpness = sharpness;
    moonsr::RectI destRect{};
    const moonsr::RectI srcRect{0, 0, static_cast<int32_t>(in.w), static_cast<int32_t>(in.h)};
    if (!pass_->Render(src.Get(), srcRect, rtv.Get(), moonsr::Size{dw, dh}, s, &destRect)) { err = "ScalePass::Render failed"; return false; }
    if (destRect.left != 0 || destRect.top != 0 || destRect.Width() != static_cast<int32_t>(dw) ||
        destRect.Height() != static_cast<int32_t>(dh)) {
        err = "Stretch dest rect is not the full target";
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    dst->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &staging);
    if (FAILED(hr)) { err = HrStr("create staging texture", hr); return false; }
    ctx->CopyResource(staging.Get(), dst.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { err = HrStr("Map staging", hr); return false; }
    out.Alloc(dw, dh);
    for (uint32_t y = 0; y < dh; ++y) memcpy(out.Row(y), static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, dw * 4);
    ctx->Unmap(staging.Get(), 0);
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    pass_->ResetSourceCache();
    return true;
}

bool GpuRunner::Time(const TimingParams& p, const Image& content, TimingResult& out, std::string& err) {
    ID3D11Device* dev = dev_->Device();
    ID3D11DeviceContext* ctx = dev_->Context();
    if (content.w != p.inW || content.h != p.inH) { err = "timing content size mismatch"; return false; }
    ComPtr<ID3D11Texture2D> src, dst;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (!CreateSource(dev, content, src, err) || !CreateTargets(dev, p.outW, p.outH, dst, rtv, err)) return false;
    pass_->ResetSourceCache();

    moonsr::ScaleSettings s;
    s.scaler = p.scaler;
    s.fit = moonsr::FitMode::Stretch;
    s.sharpness = p.sharpness;
    const moonsr::RectI srcRect{0, 0, static_cast<int32_t>(p.inW), static_cast<int32_t>(p.inH)};
    const moonsr::Size dstSize{p.outW, p.outH};
    auto render = [&]() { return pass_->Render(src.Get(), srcRect, rtv.Get(), dstSize, s, nullptr); };

    ComPtr<ID3D11Query> eventQ;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(dev->CreateQuery(&qd, &eventQ))) { err = "CreateQuery(event) failed"; return false; }

    // Warm-up: lets clocks ramp and driver-side lazy work (shader finalisation, intermediate allocation) happen.
    for (uint32_t i = 0; i < p.warmup; ++i)
        if (!render()) { err = "warm-up Render failed"; return false; }
    ctx->End(eventQ.Get());
    ctx->Flush();
    BOOL done = FALSE;
    if (!WaitQuery(ctx, eventQ.Get(), &done, sizeof(done))) { err = "warm-up wait failed"; return false; }

    const uint32_t batch = std::min<uint32_t>(p.idleMs ? 5 : 50, p.iterations);
    ComPtr<ID3D11Query> disjoint;
    qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(dev->CreateQuery(&qd, &disjoint))) { err = "CreateQuery(disjoint) failed"; return false; }
    std::vector<ComPtr<ID3D11Query>> t0(batch), t1(batch);
    qd = {D3D11_QUERY_TIMESTAMP, 0};
    for (uint32_t i = 0; i < batch; ++i)
        if (FAILED(dev->CreateQuery(&qd, &t0[i])) || FAILED(dev->CreateQuery(&qd, &t1[i]))) { err = "CreateQuery(timestamp) failed"; return false; }

    std::vector<double> ms;
    ms.reserve(p.iterations);
    uint32_t discarded = 0, attempts = 0;
    double wallTotalMs = 0;
    while (ms.size() < p.iterations && attempts < (p.iterations / batch + 1) + 10) {
        ++attempts;
        const uint32_t n = std::min<uint32_t>(batch, p.iterations - static_cast<uint32_t>(ms.size()));
        const auto wall0 = std::chrono::steady_clock::now();
        ctx->Begin(disjoint.Get());
        for (uint32_t i = 0; i < n; ++i) {
            ctx->End(t0[i].Get());
            if (!render()) { err = "Render failed during timing"; return false; }
            ctx->End(t1[i].Get());
        }
        ctx->End(disjoint.Get());
        ctx->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (!WaitQuery(ctx, disjoint.Get(), &dj, sizeof(dj))) { err = "disjoint query wait failed"; return false; }
        std::vector<double> batchMs;
        bool ok = !dj.Disjoint;
        for (uint32_t i = 0; i < n && ok; ++i) {
            UINT64 a = 0, b = 0;
            if (!WaitQuery(ctx, t0[i].Get(), &a, sizeof(a)) || !WaitQuery(ctx, t1[i].Get(), &b, sizeof(b))) { err = "timestamp wait failed"; return false; }
            if (b < a) { ok = false; break; }
            batchMs.push_back(static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency));
        }
        const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
        if (p.idleMs) Sleep(p.idleMs);
        if (!ok) { ++discarded; continue; }
        wallTotalMs += wallMs;
        out.timestampFreq = dj.Frequency;
        ms.insert(ms.end(), batchMs.begin(), batchMs.end());
    }
    pass_->ResetSourceCache();
    out.discardedBatches = discarded;
    if (ms.empty()) { err = "no valid (non-disjoint) timing samples"; return false; }
    out.samples = static_cast<uint32_t>(ms.size());
    out.wallPerIterMs = wallTotalMs / static_cast<double>(ms.size());
    std::sort(ms.begin(), ms.end());
    out.meanMs = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
    out.minMs = ms.front();
    out.maxMs = ms.back();
    out.medianMs = ms[ms.size() / 2];
    out.p95Ms = ms[std::min(ms.size() - 1, static_cast<size_t>(std::ceil(0.95 * ms.size())) - 1)];
    return true;
}

} // namespace bench
