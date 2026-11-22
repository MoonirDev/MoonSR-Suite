// GPU side of moonsr_fgbench: the real FrameGenerator on a chosen adapter, readback and timestamp timing.
#include "fgbench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace fgb {

namespace {
std::string HrStr(const char* what, HRESULT hr) {
    char b[128];
    snprintf(b, sizeof(b), "%s failed (0x%08X)", what, static_cast<unsigned>(hr));
    return b;
}

bool WaitQuery(ID3D11DeviceContext* ctx, ID3D11Query* q, void* data, UINT size) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const HRESULT hr = ctx->GetData(q, data, size, 0);
        if (hr == S_OK) return true;
        if (FAILED(hr)) return false;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) return false;
        std::this_thread::yield();
    }
}

void Finish(std::vector<double>& ms, TimingResult& r) {
    r.samples = static_cast<uint32_t>(ms.size());
    if (ms.empty()) return;
    std::sort(ms.begin(), ms.end());
    r.meanMs = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
    r.minMs = ms.front();
    r.medianMs = ms[ms.size() / 2];
    r.p95Ms = ms[std::min(ms.size() - 1, static_cast<size_t>(std::ceil(0.95 * ms.size())) - 1)];
}
} // namespace

std::unique_ptr<Runner> Runner::Open(const moonsr::AdapterInfo& info, std::string& err) {
    auto r = std::unique_ptr<Runner>(new Runner());
    r->info_ = info;
    std::wstring werr;
    r->dev_ = moonsr::GpuDevice::Create(&info.luid, werr);
    if (!r->dev_) {
        err = "GpuDevice::Create: " + Narrow(werr);
        return nullptr;
    }
    r->fg_ = moonsr::FrameGenerator::Create(*r->dev_, werr);
    if (!r->fg_) {
        err = "FrameGenerator::Create: " + Narrow(werr);
        return nullptr;
    }
    return r;
}

bool Runner::Upload(const Image& img, ComPtr<ID3D11Texture2D>& tex, std::string& err) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = img.w;
    td.Height = img.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{img.px.data(), img.w * 4, 0};
    const HRESULT hr = dev_->Device()->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr)) {
        err = HrStr("CreateTexture2D(source)", hr);
        return false;
    }
    return true;
}

bool Runner::EnsureTarget(uint32_t w, uint32_t h, std::string& err) {
    if (dst_ && dstW_ == w && dstH_ == h) return true;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev_->Device()->CreateTexture2D(&td, nullptr, &dst_);
    if (SUCCEEDED(hr)) hr = dev_->Device()->CreateRenderTargetView(dst_.Get(), nullptr, &rtv_);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = dev_->Device()->CreateTexture2D(&td, nullptr, &staging_);
    if (FAILED(hr)) {
        err = HrStr("create target", hr);
        dst_.Reset();
        return false;
    }
    dstW_ = w;
    dstH_ = h;
    return true;
}

bool Runner::PushSequence(const std::vector<const Image*>& frames, const moonsr::FrameGenSettings& s, std::string& err) {
    fg_->Reset();
    fg_->SetSettings(s);
    srcs_.resize(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!Upload(*frames[i], srcs_[i], err)) return false;
        const moonsr::RectI rect{0, 0, static_cast<int32_t>(frames[i]->w), static_cast<int32_t>(frames[i]->h)};
        if (!fg_->PushFrame(srcs_[i].Get(), rect, static_cast<int64_t>(i + 1) * 1000)) {
            err = "PushFrame failed";
            return false;
        }
    }
    return true;
}

bool Runner::InterpolateRead(float t, moonsr::FrameGenDebugView view, Image& out, std::string& err) {
    const moonsr::Size fs = fg_->FrameSize();
    if (!EnsureTarget(fs.width, fs.height, err)) return false;
    fg_->SetDebugView(view);
    const bool ok = fg_->Interpolate(t, rtv_.Get());
    fg_->SetDebugView(moonsr::FrameGenDebugView::None);
    if (!ok) {
        err = "Interpolate returned false";
        return false;
    }
    ID3D11DeviceContext* ctx = dev_->Context();
    ctx->CopyResource(staging_.Get(), dst_.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    const HRESULT hr = ctx->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        err = HrStr("Map(staging)", hr);
        return false;
    }
    out.Alloc(fs.width, fs.height);
    for (uint32_t y = 0; y < fs.height; ++y)
        memcpy(out.px.data() + static_cast<size_t>(y) * fs.width * 4, static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, fs.width * 4);
    ctx->Unmap(staging_.Get(), 0);
    for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    return true;
}

bool Runner::WaitIdle() {
    ComPtr<ID3D11Query> q;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(dev_->Device()->CreateQuery(&qd, &q))) return false;
    dev_->Context()->End(q.Get());
    dev_->Context()->Flush();
    BOOL done = FALSE;
    return WaitQuery(dev_->Context(), q.Get(), &done, sizeof(done));
}

std::string Runner::CheckUnbound() {
    ID3D11DeviceContext* ctx = dev_->Context();
    ID3D11ShaderResourceView* srv[16] = {};
    ctx->PSGetShaderResources(0, 16, srv);
    std::string r;
    for (auto*& s : srv)
        if (s) {
            r = "PS SRV left bound";
            s->Release();
        }
    ctx->CSGetShaderResources(0, 16, srv);
    for (auto*& s : srv)
        if (s) {
            r = "CS SRV left bound";
            s->Release();
        }
    ID3D11UnorderedAccessView* uav[8] = {};
    ctx->CSGetUnorderedAccessViews(0, 8, uav);
    for (auto*& u : uav)
        if (u) {
            r = "CS UAV left bound";
            u->Release();
        }
    ID3D11RenderTargetView* rtv[8] = {};
    ctx->OMGetRenderTargets(8, rtv, nullptr);
    for (auto*& v : rtv)
        if (v) {
            r = "RTV left bound";
            v->Release();
        }
    return r;
}

bool Runner::Time(const Image& a, const Image& b, const moonsr::FrameGenSettings& s, uint32_t warmup, uint32_t iterations,
                  TimingResult& push, TimingResult& interp, TimingResult& pair, std::string& err) {
    ID3D11Device* dev = dev_->Device();
    ID3D11DeviceContext* ctx = dev_->Context();
    ComPtr<ID3D11Texture2D> ta, tb;
    if (!Upload(a, ta, err) || !Upload(b, tb, err) || !EnsureTarget(a.w, a.h, err)) return false;
    fg_->Reset();
    fg_->SetSettings(s);
    const moonsr::RectI rect{0, 0, static_cast<int32_t>(a.w), static_cast<int32_t>(a.h)};
    uint64_t counter = 0;
    auto pushNext = [&]() {
        ++counter;
        return fg_->PushFrame((counter & 1) ? ta.Get() : tb.Get(), rect, static_cast<int64_t>(counter));
    };
    if (!pushNext()) { err = "PushFrame failed (first frame)"; return false; }
    for (uint32_t i = 0; i < warmup; ++i) {
        if (!pushNext()) { err = "PushFrame failed (warm-up)"; return false; }
        if (!fg_->Interpolate(0.5f, rtv_.Get())) { err = "Interpolate failed (warm-up)"; return false; }
    }
    if (!WaitIdle()) { err = "warm-up wait failed"; return false; }

    const uint32_t batch = std::min<uint32_t>(25, iterations);
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> disjoint;
    if (FAILED(dev->CreateQuery(&qd, &disjoint))) { err = "CreateQuery(disjoint)"; return false; }
    // Per iteration: ts0, PushFrame, ts1, Interpolate, ts2.
    std::vector<ComPtr<ID3D11Query>> q(static_cast<size_t>(batch) * 3);
    qd = {D3D11_QUERY_TIMESTAMP, 0};
    for (auto& x : q)
        if (FAILED(dev->CreateQuery(&qd, &x))) { err = "CreateQuery(timestamp)"; return false; }

    std::vector<double> pushMs, interpMs, pairMs;
    uint32_t discarded = 0, attempts = 0;
    while (pushMs.size() < iterations && attempts < iterations / batch + 20) {
        ++attempts;
        const uint32_t n = std::min<uint32_t>(batch, iterations - static_cast<uint32_t>(pushMs.size()));
        ctx->Begin(disjoint.Get());
        for (uint32_t i = 0; i < n; ++i) {
            ctx->End(q[i * 3 + 0].Get());
            if (!pushNext()) { err = "PushFrame failed"; return false; }
            ctx->End(q[i * 3 + 1].Get());
            if (!fg_->Interpolate(0.5f, rtv_.Get())) { err = "Interpolate failed"; return false; }
            ctx->End(q[i * 3 + 2].Get());
        }
        ctx->End(disjoint.Get());
        ctx->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (!WaitQuery(ctx, disjoint.Get(), &dj, sizeof(dj))) { err = "disjoint wait failed"; return false; }
        if (dj.Disjoint) {
            ++discarded;
            continue;
        }
        std::vector<double> bp, bi;
        bool ok = true;
        for (uint32_t i = 0; i < n && ok; ++i) {
            UINT64 t0 = 0, t1 = 0, t2 = 0;
            if (!WaitQuery(ctx, q[i * 3].Get(), &t0, 8) || !WaitQuery(ctx, q[i * 3 + 1].Get(), &t1, 8) ||
                !WaitQuery(ctx, q[i * 3 + 2].Get(), &t2, 8)) {
                err = "timestamp wait failed";
                return false;
            }
            if (t1 < t0 || t2 < t1) {
                ok = false;
                break;
            }
            bp.push_back(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency));
            bi.push_back(static_cast<double>(t2 - t1) * 1000.0 / static_cast<double>(dj.Frequency));
        }
        if (!ok) {
            ++discarded;
            continue;
        }
        pushMs.insert(pushMs.end(), bp.begin(), bp.end());
        interpMs.insert(interpMs.end(), bi.begin(), bi.end());
        for (size_t i = 0; i < bp.size(); ++i) pairMs.push_back(bp[i] + bi[i]);
    }
    push.discardedBatches = interp.discardedBatches = pair.discardedBatches = discarded;
    Finish(pushMs, push);
    Finish(interpMs, interp);
    Finish(pairMs, pair);
    if (pushMs.empty()) {
        err = "no valid timing samples";
        return false;
    }
    return true;
}

} // namespace fgb
