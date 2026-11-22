// Owner: lead
#include "engine/remote_stages.h"

#include "core/log.h"
#include "gpu/cross_adapter.h"
#include "gpu/gpu_device.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>

namespace moonsr {
namespace {
using Microsoft::WRL::ComPtr;

class RemoteFrameGenerator final : public FrameGenerator {
public:
    RemoteFrameGenerator(GpuDevice& main, GpuDevice& remote) : main_(main), remote_(remote) {}

    bool Init(std::wstring& error) {
        inner_ = FrameGenerator::Create(remote_, error);
        if (!inner_) return false;
        up_ = CrossAdapterLink::Create(main_, remote_, error);
        if (!up_) return false;
        down_ = CrossAdapterLink::Create(remote_, main_, error);
        if (!down_) return false;
        MSR_LOG_INFO(L"Frame generation runs on '{}' (main GPU '{}'); frames cross by {}", remote_.Info().name,
                     main_.Info().name, up_->Method());
        return true;
    }

    void SetSettings(const FrameGenSettings& settings) override { inner_->SetSettings(settings); }

    bool PushFrame(ID3D11Texture2D* src, const RectI& srcRect, int64_t qpc) override {
        if (!src || srcRect.Empty()) return false;
        const Size size{static_cast<uint32_t>(srcRect.Width()), static_cast<uint32_t>(srcRect.Height())};
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return Fail(L"frame generation input is not BGRA8");
        if (size != size_ && !EnsureLocal(size)) return false;
        std::wstring error;
        ID3D11Texture2D* remoteFrame = up_->Transfer(src, srcRect, error);
        if (!remoteFrame) return Fail(error);
        if (!inner_->PushFrame(remoteFrame, RectI{0, 0, srcRect.Width(), srcRect.Height()}, qpc)) return false;
        // Keep the real frame on the main device too: RealFrame() must not cross back.
        newest_ ^= 1u;
        const D3D11_BOX box{static_cast<UINT>(srcRect.left), static_cast<UINT>(srcRect.top), 0,
                            static_cast<UINT>(srcRect.right), static_cast<UINT>(srcRect.bottom), 1};
        main_.Context()->CopySubresourceRegion(local_[newest_].Get(), 0, 0, 0, 0, src, 0, &box);
        count_ = std::min(count_ + 1u, 2u);
        return true;
    }

    void Reset() override {
        inner_->Reset();
        count_ = 0;
    }

    FramePairInfo PairInfo() const override { return inner_->PairInfo(); }

    ID3D11Texture2D* RealFrame(uint32_t index) const override {
        if (index >= count_) return nullptr;
        return local_[index == 0 ? newest_ : newest_ ^ 1u].Get();
    }

    Size FrameSize() const override { return size_; }

    bool Interpolate(float t, ID3D11RenderTargetView* dst) override {
        if (!dst || size_.Empty()) return false;
        if ((!gen_ || genSize_ != size_) && !EnsureGenerated()) return false;
        if (!inner_->Interpolate(t, genRtv_.Get())) return false;
        std::wstring error;
        ID3D11Texture2D* back = down_->Transfer(gen_.Get(), RectI{0, 0, static_cast<int32_t>(size_.width),
                                                                   static_cast<int32_t>(size_.height)}, error);
        if (!back) return Fail(error);
        ComPtr<ID3D11Resource> target;
        dst->GetResource(&target);
        const D3D11_BOX box{0, 0, 0, size_.width, size_.height, 1};
        main_.Context()->CopySubresourceRegion(target.Get(), 0, 0, 0, 0, back, 0, &box);
        return true;
    }

    MotionFieldView MotionField() const override { return {}; } // not shared across GPUs (see header)

    void SetDebugView(FrameGenDebugView view) override { inner_->SetDebugView(view); }

private:
    bool Fail(const std::wstring& what) {
        if (!failureLogged_) {
            failureLogged_ = true;
            MSR_LOG_ERROR(L"Frame generation on '{}': {}", remote_.Info().name, what);
        }
        return false;
    }

    bool EnsureLocal(Size size) {
        for (auto& t : local_) t.Reset();
        count_ = 0;
        size_ = {};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.width;
        td.Height = size.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (auto& t : local_) {
            const HRESULT hr = main_.Device()->CreateTexture2D(&td, nullptr, &t);
            if (FAILED(hr)) return Fail(std::format(L"local real-frame copy {}x{}: {}", size.width, size.height, HrToString(hr)));
        }
        size_ = size;
        return true;
    }

    bool EnsureGenerated() {
        genRtv_.Reset();
        gen_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size_.width;
        td.Height = size_.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = remote_.Device()->CreateTexture2D(&td, nullptr, &gen_);
        if (SUCCEEDED(hr)) hr = remote_.Device()->CreateRenderTargetView(gen_.Get(), nullptr, &genRtv_);
        if (FAILED(hr)) {
            gen_.Reset();
            return Fail(std::format(L"generated-frame target {}x{}: {}", size_.width, size_.height, HrToString(hr)));
        }
        genSize_ = size_;
        return true;
    }

    GpuDevice& main_;
    GpuDevice& remote_;
    std::unique_ptr<FrameGenerator> inner_;
    std::unique_ptr<CrossAdapterLink> up_, down_;
    ComPtr<ID3D11Texture2D> local_[2];
    uint32_t newest_ = 0, count_ = 0;
    Size size_;
    ComPtr<ID3D11Texture2D> gen_;
    ComPtr<ID3D11RenderTargetView> genRtv_;
    Size genSize_;
    bool failureLogged_ = false;
};

class RemoteNeuralBridge final : public NeuralBridge {
public:
    RemoteNeuralBridge(GpuDevice& main, GpuDevice& remote, std::unique_ptr<NeuralBridge> inner)
        : main_(main), remote_(remote), inner_(std::move(inner)) {}

    bool Init(std::wstring& error) {
        up_ = CrossAdapterLink::Create(main_, remote_, error);
        if (!up_) return false;
        guideUp_ = CrossAdapterLink::Create(main_, remote_, error);
        if (!guideUp_) return false;
        down_ = CrossAdapterLink::Create(remote_, main_, error);
        if (!down_) return false;
        MSR_LOG_INFO(L"Neural rendering runs on '{}' (main GPU '{}'); frames cross by {}", remote_.Info().name,
                     main_.Info().name, up_->Method());
        return true;
    }

    bool SupportsLiveSettings() const override { return inner_->SupportsLiveSettings(); }
    bool UpdateSettings(const NeuralSettings& settings, std::wstring& error) override {
        return inner_->UpdateSettings(settings, error);
    }

    bool Process(ID3D11Texture2D* texture, const RectI& rect, int64_t captureQpc, uint64_t sequence, bool reset,
                 const PictureSettings& picture, ID3D11Texture2D*& enhanced, RectI& enhancedRect,
                 std::wstring& error) override {
        return ProcessWithGuide(texture, rect, nullptr, captureQpc, sequence, reset, picture, enhanced, enhancedRect, error);
    }

    bool ProcessWithGuide(ID3D11Texture2D* texture, const RectI& rect, ID3D11Texture2D* flowGuide, int64_t captureQpc,
                          uint64_t sequence, bool reset, const PictureSettings& picture, ID3D11Texture2D*& enhanced,
                          RectI& enhancedRect, std::wstring& error) override {
        enhanced = nullptr;
        enhancedRect = {};
        ID3D11Texture2D* remoteInput = up_->Transfer(texture, rect, error);
        if (!remoteInput) return false;
        ID3D11Texture2D* remoteGuide = nullptr;
        if (flowGuide) {
            D3D11_TEXTURE2D_DESC gd{};
            flowGuide->GetDesc(&gd);
            remoteGuide = guideUp_->Transfer(flowGuide, RectI{0, 0, static_cast<int32_t>(gd.Width),
                                                             static_cast<int32_t>(gd.Height)}, error);
            if (!remoteGuide) return false;
        }
        ID3D11Texture2D* remoteOut = nullptr;
        RectI remoteRect;
        if (!inner_->ProcessWithGuide(remoteInput, RectI{0, 0, rect.Width(), rect.Height()}, remoteGuide, captureQpc,
                                      sequence, reset, picture, remoteOut, remoteRect, error)) return false;
        ID3D11Texture2D* back = down_->Transfer(remoteOut, remoteRect, error);
        if (!back) return false;
        enhanced = back;
        enhancedRect = {0, 0, remoteRect.Width(), remoteRect.Height()};
        return true;
    }

private:
    GpuDevice& main_;
    GpuDevice& remote_;
    std::unique_ptr<NeuralBridge> inner_;
    std::unique_ptr<CrossAdapterLink> up_, guideUp_, down_;
};
} // namespace

std::unique_ptr<FrameGenerator> CreateRemoteFrameGenerator(GpuDevice& main, GpuDevice& remote, std::wstring& error) {
    auto g = std::make_unique<RemoteFrameGenerator>(main, remote);
    if (!g->Init(error)) return nullptr;
    return g;
}

std::unique_ptr<NeuralBridge> CreateRemoteNeuralBridge(GpuDevice& main, GpuDevice& remote,
                                                      std::unique_ptr<NeuralBridge> inner, std::wstring& error) {
    if (!inner) {
        error = L"Neural rendering bridge was not created.";
        return nullptr;
    }
    auto b = std::make_unique<RemoteNeuralBridge>(main, remote, std::move(inner));
    if (!b->Init(error)) return nullptr;
    return b;
}

} // namespace moonsr
