// SPDX-License-Identifier: GPL-3.0-only
#include "pipeline.h"
#include "amd_colour.h"
#include "bridge.h"
#include "initialize_guides.h"
#include "scene_change.h"
#include <algorithm>

namespace neural {
namespace {
// Save every CS resource slot: D3D11 hazard handling can unbind aliases implicitly.
struct StateGuard {
    ID3D11DeviceContext *context;
    ID3D11ComputeShader *shader = nullptr;
    ID3D11ClassInstance *classes[256]{};
    UINT count = 256;
    ID3D11ShaderResourceView *srvs[128]{};
    ID3D11UnorderedAccessView *uavs[8]{};
    explicit StateGuard(ID3D11DeviceContext *c) : context(c) {
        c->CSGetShader(&shader, classes, &count);
        c->CSGetShaderResources(0, 128, srvs);
        c->CSGetUnorderedAccessViews(0, 8, uavs);
        ID3D11UnorderedAccessView *none[8]{};
        c->CSSetUnorderedAccessViews(0, 8, none, nullptr);
        ID3D11ShaderResourceView *empty[128]{};
        c->CSSetShaderResources(0, 128, empty);
    }
    ~StateGuard() {
        ID3D11UnorderedAccessView *none[8]{};
        context->CSSetUnorderedAccessViews(0, 8, none, nullptr);
        ID3D11ShaderResourceView *empty[128]{};
        context->CSSetShaderResources(0, 128, empty);
        context->CSSetShader(shader, classes, count);
        context->CSSetShaderResources(0, 128, srvs);
        context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
        if (shader)
            shader->Release();
        for (UINT i = 0; i < count; i++)
            if (classes[i])
                classes[i]->Release();
        for (auto *p : srvs)
            if (p)
                p->Release();
        for (auto *p : uavs)
            if (p)
                p->Release();
    }
};
bool WaitFence(ID3D12Fence *fence, uint64_t value, DWORD timeout) {
    if (!fence || value == 0)
        return true;
    auto complete = [&] {
        auto n = fence->GetCompletedValue();
        return n != UINT64_MAX && n >= value;
    };
    if (complete())
        return true;
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!e)
        return false;
    bool ok = false;
    if (SUCCEEDED(fence->SetEventOnCompletion(value, e))) {
        const auto start = GetTickCount64();
        do {
            const auto elapsed = GetTickCount64() - start;
            const DWORD interval = timeout == INFINITE ? 100 : DWORD(std::min<uint64_t>(100, timeout - std::min<uint64_t>(elapsed, timeout)));
            const auto result = WaitForSingleObject(e, interval);
            if (result == WAIT_OBJECT_0) { ok = complete(); break; }
            if (result != WAIT_TIMEOUT || fence->GetCompletedValue() == UINT64_MAX)
                break;
        } while (timeout == INFINITE || GetTickCount64() - start < timeout);
    }
    CloseHandle(e);
    return ok;
}
} // namespace
Pipeline::Shared Pipeline::Share(unsigned w, unsigned h) {
    Shared s;
    s.texture.width = w;
    s.texture.height = h;
    s.texture.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = s.texture.format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    Check(device11_->CreateTexture2D(&d, nullptr, &s.texture11), "Create shared capture image");
    ComPtr<IDXGIResource1> resource;
    Check(s.texture11.As(&resource), "Query shared image");
    HANDLE handle = nullptr;
    Check(
        resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle),
        "Share image handle");
    const HRESULT result = gpu_.device->OpenSharedHandle(handle, IID_PPV_ARGS(&s.texture.object));
    CloseHandle(handle);
    Check(result, "Open image in D3D12");
    Check(device11_->CreateShaderResourceView(s.texture11.Get(), nullptr, &s.srv), "Create shared SRV");
    Check(device11_->CreateUnorderedAccessView(s.texture11.Get(), nullptr, &s.uav), "Create shared UAV");
    return s;
}
void Pipeline::SharedFence(ComPtr<ID3D11Fence> &f11, ComPtr<ID3D12Fence> &f12) {
    Check(device11_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f11)), "Create capture fence");
    HANDLE handle = nullptr;
    Check(f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "Share capture fence");
    const HRESULT result = gpu_.device->OpenSharedHandle(handle, IID_PPV_ARGS(&f12));
    CloseHandle(handle);
    Check(result, "Open capture fence");
}
void Pipeline::Initialize(ID3D11Device *device, unsigned w, unsigned h, Encoding encoding, const PipelineSettings &s) {
    settings_ = s;
    encoding_ = encoding;
    if (s.scalePercent < 25 || s.scalePercent > 100 || s.flowQuality < 1 || s.flowQuality > 3 ||
        s.passes < 1 || s.passes > PipelineSettings::MaxPasses)
        throw std::runtime_error("Invalid pipeline settings");
    Check(device->QueryInterface(IID_PPV_ARGS(&device11_)), "D3D11 shared fences require Windows 10");
    ComPtr<IDXGIDevice> dxgi;
    Check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Query capture adapter");
    ComPtr<IDXGIAdapter> adapter;
    Check(dxgi->GetAdapter(&adapter), "Get capture adapter");
    DXGI_ADAPTER_DESC description{};
    Check(adapter->GetDesc(&description), "Read capture adapter");
    dxgi->GetGPUThreadPriority(&metrics_.captureGpuPriority);
    gpu_.Initialize(&description.AdapterLuid, metrics_.captureGpuPriority > 0, s.amd ? 0x1002 : 0x10de);
    metrics_.nrQueuePriority = gpu_.queue->GetDesc().Priority;
    SharedFence(capture11_, capture12_);
    SharedFence(consumed11_, consumed12_);
    HANDLE handle = nullptr;
    Check(gpu_.device->CreateSharedHandle(gpu_.fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle),
          "Share completion fence");
    const HRESULT opened = device11_->OpenSharedFence(handle, IID_PPV_ARGS(&complete11_));
    CloseHandle(handle);
    Check(opened, "Open completion fence");
    Check(device->CreateComputeShader(g_bridge, sizeof(g_bridge), nullptr, &bridge_), "Create capture bridge");
    metrics_.width = w;
    metrics_.height = h;
    metrics_.workingWidth = std::max(1u, w * s.scalePercent / 100);
    metrics_.workingHeight = std::max(1u, h * s.scalePercent / 100);
    const unsigned ww = metrics_.workingWidth, wh = metrics_.workingHeight;
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 15;
    Check(gpu_.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries_)), "Create timings");
    Check(gpu_.queue->GetTimestampFrequency(&frequency_), "Read timestamp frequency");
    for (auto &slot : slots_) {
        slot.input = Share(w, h);
        slot.output = Share(w, h);
        slot.proxy = gpu_.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        slot.keep = gpu_.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (ww != w || wh != h)
            slot.working = gpu_.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (!s.amd) {
            slot.flowInput = Share(w, h);
            slot.flowProxy = gpu_.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            slot.flowKeep = gpu_.Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            slot.model = gpu_.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (s.passes > 1)
                slot.modelScratch = gpu_.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
            slot.motion = gpu_.Texture(ww, wh, DXGI_FORMAT_R16G16_FLOAT);
            slot.summary = gpu_.Buffer(8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            slot.readback = gpu_.Buffer(8, D3D12_HEAP_TYPE_READBACK);
        }
        slot.timings = gpu_.Buffer(40, D3D12_HEAP_TYPE_READBACK);
        slot.prepare.Initialize(gpu_.device.Get());
        slot.finish.Initialize(gpu_.device.Get());
    }
    composition_.Initialize(gpu_.device.Get());
    if (s.amd) {
        // Use a stable input texture so the runtime can keep its descriptors across frames.
        amdLinear_ = gpu_.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
        amdDisplay_ = gpu_.Texture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT);
        amdColour_.Initialize(gpu_.device.Get(), g_amd_colour, sizeof(g_amd_colour));
        amd_ = std::make_unique<AmdModel>();
        amd_->Initialize(gpu_, s.modelPath);
        initialized_ = true;
        return;
    }
    depth_ = gpu_.Texture(ww, wh, DXGI_FORMAT_R32_FLOAT);
    scene_.Initialize(gpu_.device.Get(), g_scene_change, sizeof(g_scene_change));
    guides_.Initialize(gpu_.device.Get(), g_initialize_guides, sizeof(g_initialize_guides));
    const NV_OF_PERF_LEVEL levels[] = {NV_OF_PERF_LEVEL_SLOW, NV_OF_PERF_LEVEL_MEDIUM, NV_OF_PERF_LEVEL_FAST};
    flow_.Initialize(gpu_, w, h, s.flowGrid, levels[s.flowQuality - 1]);
    auto &rec = slots_[0].prepare;
    rec.Begin(gpu_.fence.Get());
    depth_.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slots_[0].motion.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const unsigned constants[] = {ww, wh, 0, 0};
    guides_.Dispatch(gpu_.device.Get(), rec, constants, sizeof(constants), {}, {&depth_, &slots_[0].motion},
                     (ww + 7) / 8, (wh + 7) / 8);
    depth_.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Submit initialization even if feature creation throws after recording partial GPU work.
    try {
        model_.Initialize(gpu_, rec, s.forwarderPath, s.modelPath, s.cachePath, ww, wh, s.model);
    } catch (...) {
        gpu_.Submit(rec);
        throw;
    }
    gpu_.Submit(rec);
    if (!gpu_.Wait(rec.completion, INFINITE))
        throw std::runtime_error("Model initialization did not retire");
    // OptiScaler's independent-history chain: one persistent feature per pass.
    // The original one-pass creation/submission above is unchanged. Each extra
    // creation retires separately, before any feature is evaluated.
    for (unsigned pass = 1; pass < s.passes; ++pass) {
        extraModels_.push_back(std::make_unique<Model>());
        rec.Begin(gpu_.fence.Get());
        auto profile = s.model;
        profile.localTone = 0; // PreSR-Multipass default: avoid compounding local tone.
        try {
            extraModels_.back()->Initialize(gpu_, rec, s.forwarderPath, s.modelPath, s.cachePath, ww, wh, profile);
        } catch (const std::exception &e) {
            gpu_.Submit(rec);
            throw std::runtime_error("NR pass " + std::to_string(pass + 1) + " initialization: " + e.what());
        } catch (...) {
            gpu_.Submit(rec);
            throw;
        }
        gpu_.Submit(rec);
        if (!gpu_.Wait(rec.completion, INFINITE))
            throw std::runtime_error("Additional model initialization did not retire");
    }
    metrics_.passes = s.passes;
    initialized_ = true;
}
void Pipeline::Copy(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *src, ID3D11UnorderedAccessView *dst) {
    ctx->CSSetShader(bridge_.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &dst, nullptr);
    ctx->Dispatch((metrics_.width + 7) / 8, (metrics_.height + 7) / 8, 1);
    ID3D11ShaderResourceView *none = nullptr;
    ID3D11UnorderedAccessView *empty = nullptr;
    ctx->CSSetShaderResources(0, 1, &none);
    ctx->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
}
void Pipeline::ReadTimings(Slot &s) {
    if (!s.hasTimings)
        return;
    void *pointer = nullptr;
    D3D12_RANGE read{0, 40};
    Check(s.timings.object->Map(0, &read, &pointer), "Read completed GPU timestamps");
    const auto *t = static_cast<const uint64_t *>(pointer);
    double factor = 1000.0 / frequency_;
    metrics_.prepareMs = (t[1] - t[0]) * factor;
    metrics_.flowMs = (t[2] - t[1]) * factor;
    metrics_.modelMs = (t[3] - t[2]) * factor;
    metrics_.compositionMs = (t[4] - t[3]) * factor;
    D3D12_RANGE none{0, 0};
    s.timings.object->Unmap(0, &none);
    s.hasTimings = false;
}
void Pipeline::SetAppearance(const ModelSettings &settings) {
    settings_.model = settings;
    model_.SetAppearance(settings);
    auto extra = settings;
    extra.localTone = 0;
    for (auto &model : extraModels_) model->SetAppearance(extra);
}

void Pipeline::ConvertAmdColour(Recording& rec, Resource& source, Resource& destination, bool encodeSrgb) {
    const uint32_t direction = encodeSrgb ? 1 : 0;
    source.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    destination.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    amdColour_.Dispatch(gpu_.device.Get(), rec, &direction, sizeof(direction), {&source}, {&destination},
                        (destination.width + 7) / 8, (destination.height + 7) / 8);
    destination.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Pipeline::Process(const EamRealFrame &frame, const CompositionSettings &settings, const PictureSettings &picture,
                       ID3D11Texture2D *flowGuide) {
    if (!initialized_)
        throw std::runtime_error("NR pipeline is not initialized");
    auto *ctx = static_cast<ID3D11DeviceContext *>(frame.context);
    ComPtr<ID3D11DeviceContext4> context;
    Check(ctx->QueryInterface(IID_PPV_ARGS(&context)), "Capture context shared fences");
    int index = -1;
    for (unsigned i = 0; i < 3; i++) {
        unsigned n = (cursor_ + i) % 3;
        auto &slot = slots_[n];
        auto consumed = consumed12_->GetCompletedValue();
        if (int(n) != previous_ && gpu_.Complete(slot.prepare.completion) && gpu_.Complete(slot.finish.completion) &&
            consumed != UINT64_MAX && consumed >= slot.consumed) {
            index = int(n);
            break;
        }
    }
    metrics_.slotWaitMs = 0;
    if (index < 0) {
        // Apply backpressure to capture. Never replace an overloaded frame with its original.
        index = int(cursor_);
        if (index == previous_)
            index = (index + 1) % 3;
        auto &pending = slots_[index];
        const auto start = std::chrono::steady_clock::now();
        ctx->Flush();
        if (!gpu_.Wait(pending.prepare.completion, INFINITE) ||
            !gpu_.Wait(pending.finish.completion, INFINITE) ||
            !WaitFence(consumed12_.Get(), pending.consumed, INFINITE))
            throw std::runtime_error("GPU failed while waiting for an NR frame slot");
        metrics_.slotWaitMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    auto &slot = slots_[index];
    cursor_ = (index + 1) % 3;
    ReadTimings(slot);
    const uint64_t now = GetTickCount64();
    bool reset = reset_ || previous_ < 0 || frame.sequence != lastSequence_ + 1 || now - lastFrameTick_ > 250;
    // Consume completed scene comparisons without making command submission wait on the GPU.
    // A detected cut resets the next available frame; explicit discontinuities still reset immediately.
    const bool discontinuity = reset;
    for (auto& completed : slots_) {
        if (!completed.hasSceneSummary) continue;
        if (discontinuity) {
            completed.hasSceneSummary = false;
            continue;
        }
        if (!gpu_.Complete(completed.prepare.completion)) continue;
        completed.hasSceneSummary = false;
        void* pointer = nullptr;
        D3D12_RANGE range{0, 8};
        Check(completed.readback.object->Map(0, &range, &pointer), "Read completed scene summary");
        const auto* values = static_cast<const unsigned*>(pointer);
        const float histogram = values[0] / 1000000.f;
        const float delta = values[1] / 1000000.f;
        D3D12_RANGE none{0, 0};
        completed.readback.object->Unmap(0, &none);
        metrics_.sceneDistance = histogram;
        if ((histogram > .32f && delta > .18f) || delta > .5f) {
            ++metrics_.cuts;
            reset = true;
        }
    }
    ComPtr<ID3D11ShaderResourceView> original;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    D3D11_TEXTURE2D_DESC td{};
    static_cast<ID3D11Texture2D *>(frame.original)->GetDesc(&td);
    sd.Format = td.Format;
    Check(device11_->CreateShaderResourceView(static_cast<ID3D11Texture2D *>(frame.original), &sd, &original),
          "View current original");
    ComPtr<ID3D11ShaderResourceView> guide;
    if (flowGuide && flowGuide != frame.original && !amd_) {
        D3D11_TEXTURE2D_DESC gd{};
        flowGuide->GetDesc(&gd);
        if (gd.Width != td.Width || gd.Height != td.Height || gd.Format != td.Format)
            throw std::runtime_error("NR flow guide must match input dimensions and format");
        Check(device11_->CreateShaderResourceView(flowGuide, &sd, &guide), "View original flow guide");
    }
    ComPtr<ID3D11UnorderedAccessView> target;
    Check(device11_->CreateUnorderedAccessView(static_cast<ID3D11Texture2D *>(frame.output), nullptr, &target),
          "View current handoff");
    StateGuard guard(ctx);
    Copy(ctx, original.Get(), slot.input.uav.Get());
    slot.separateFlowGuide = guide != nullptr;
    if (guide) Copy(ctx, guide.Get(), slot.flowInput.uav.Get());
    const uint64_t transfer = ++transferValue_;
    Check(context->Signal(capture11_.Get(), transfer), "Signal original capture");
    ctx->Flush();
    Check(gpu_.queue->Wait(capture12_.Get(), transfer), "Wait for original capture");
    auto &prep = slot.prepare;
    prep.Begin(gpu_.fence.Get());
    const unsigned query = index * 5;
    prep.list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
    composition_.Encode(gpu_, prep, slot.input.texture, slot.proxy, slot.keep, encoding_, settings);
    slot.input.texture.Transition(prep.list.Get(), D3D12_RESOURCE_STATE_COMMON);
    // At native resolution the downsample shader would only copy the encoded texture.
    Resource& working = slot.working.object ? slot.working : slot.proxy;
    if (slot.working.object) composition_.Downsample(gpu_, prep, slot.proxy, working);
    if (amd_) {
        ConvertAmdColour(prep, working, amdLinear_, false);
        amd_->RecordInput(prep, amdLinear_, settings_.model, settings_.passes);
    } else {
        if (slot.separateFlowGuide) {
            composition_.Encode(gpu_, prep, slot.flowInput.texture, slot.flowProxy, slot.flowKeep, encoding_, settings);
            slot.flowInput.texture.Transition(prep.list.Get(), D3D12_RESOURCE_STATE_COMMON);
        }
        flow_.Prepare(gpu_, prep, slot.FlowProxy(), index);
    }
    slot.hasSceneSummary = !amd_ && !discontinuity;
    if (slot.hasSceneSummary) {
        slot.summary.Transition(prep.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto& previousGuide = slots_[previous_].FlowProxy();
        previousGuide.Transition(prep.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        scene_.Dispatch(gpu_.device.Get(), prep, nullptr, 0, {&slot.FlowProxy(), &previousGuide}, {&slot.summary},
                        1, 1);
        slot.summary.Transition(prep.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        prep.list->CopyBufferRegion(slot.readback.object.Get(), 0, slot.summary.object.Get(), 0, 8);
    }
    prep.list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
    const uint64_t ready = gpu_.Submit(prep);
    if (amd_) {
        amd_->Enqueue(gpu_);
    } else {
        flow_.Execute(gpu_, index, previous_ < 0 ? unsigned(index) : unsigned(previous_), reset, ready);
    }
    auto &finish = slot.finish;
    finish.Begin(gpu_.fence.Get());
    if (!amd_) flow_.Convert(gpu_, finish, slot.motion, reset);
    finish.list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2);
    Resource amdOutput;
    Resource* result = &slot.model;
    auto resolve = settings;
    if (amd_) {
        amdOutput = amd_->RecordOutput(finish);
        if (amdOutput.width != working.width || amdOutput.height != working.height)
            throw std::runtime_error("AMDNR output size does not match its input");
        ConvertAmdColour(finish, amdOutput, amdDisplay_, true);
        result = &amdDisplay_;
        resolve.detail *= settings_.model.intensity;
        resolve.colour *= settings_.model.intensity;
    } else {
        model_.Evaluate(finish, working, depth_, slot.motion, slot.model, reset);
        for (size_t pass = 0; pass < extraModels_.size(); ++pass) {
            Resource *next = result == &slot.model ? &slot.modelScratch : &slot.model;
            try {
                extraModels_[pass]->Evaluate(finish, *result, depth_, slot.motion, *next, reset);
            } catch (const std::exception &e) {
                throw std::runtime_error("NR pass " + std::to_string(pass + 2) + " evaluation: " + e.what());
            }
            result = next;
        }
    }
    finish.list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 3);
    composition_.Resolve(gpu_, finish, working, *result, slot.keep, slot.output.texture, encoding_, resolve);
    slot.output.texture.Transition(finish.list.Get(), D3D12_RESOURCE_STATE_COMMON);
    finish.list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 4);
    finish.list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 5, slot.timings.object.Get(), 0);
    const uint64_t done = gpu_.Submit(finish);
    // The runtime retires CPU job state after submission. GPU resources stay ordered on this queue.
    if (amd_) amd_->RetireSubmitted();
    slot.hasTimings = true;
    // Only publish the D3D11 wait after the exact current output and its signal exist.
    Check(context->Wait(complete11_.Get(), done), "Wait for enhanced current image");
    // Comparison keeps the manager's exact original copy, including negative scRGB
    // components that upstream composition deliberately clamps at its output.
    if (settings.apply) {
        // Off/neutral uses the original copy shader, with no picture resources or extra GPU work.
        if (!picture_.Apply(ctx, slot.output.srv.Get(), target.Get(), metrics_.width, metrics_.height,
                            encoding_, settings.whiteNits, picture))
            Copy(ctx, slot.output.srv.Get(), target.Get());
    }
    Check(context->Signal(consumed11_.Get(), transfer), "Signal enhanced image consumed");
    slot.consumed = transfer;
    lastConsumed_ = transfer;
    ctx->Flush();
    previous_ = index;
    lastSequence_ = frame.sequence;
    lastFrameTick_ = now;
    reset_ = false;
    ++metrics_.accepted;
    if (reset)
        ++metrics_.resets;
}
bool Pipeline::Retire() {
    if (!gpu_.device || !gpu_.fence)
        return true;
    if (!gpu_.Wait(gpu_.LastSubmitted()) || !WaitFence(consumed12_.Get(), lastConsumed_, 3000))
        return false;
    amd_.reset();
    if (!extraModels_.empty()) {
        // Shared snippet shutdown refuses while ANY pass feature remains live.
        // Include a partially initialized final pass when recovering from creation failure.
        for (auto &model : extraModels_)
            if (!model->ReleaseFeature())
                return false;
        if (!model_.ReleaseFeature())
            return false;
        for (auto &model : extraModels_)
            if (!model->Shutdown())
                return false;
        extraModels_.clear();
    }
    if (!model_.Shutdown() || !flow_.Shutdown())
        return false;
    initialized_ = false;
    return true;
}
} // namespace neural
