// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include <d3d11_4.h>

namespace moonsr {
// Full-frame D3D11 -> D3D12 capture slots. Queue lifetimes must be drained by the owner.
class SharedFlowInputs {
public:
    void Initialize(neural::Gpu& gpu, ID3D11Device* device, unsigned w, unsigned h,
                    DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM) {
        neural::ComPtr<ID3D11Device5> dev;
        neural::Check(device->QueryInterface(IID_PPV_ARGS(&dev)),"Query flow input device");
        neural::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        neural::Check(context.As(&context_),"Query flow input context");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=w; desc.Height=h; desc.MipLevels=1; desc.ArraySize=1; desc.SampleDesc.Count=1;
        if (format!=DXGI_FORMAT_R8G8B8A8_UNORM && format!=DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Unsupported shared flow input format");
        desc.Format=format; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        for (unsigned i=0;i<2;++i) {
            neural::Check(device->CreateTexture2D(&desc,nullptr,&textures_[i]),"Create shared flow input");
            neural::ComPtr<IDXGIResource1> resource;
            neural::Check(textures_[i].As(&resource),"Query flow input handle");
            HANDLE handle=nullptr;
            neural::Check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"Share flow input");
            const auto hr=gpu.device->OpenSharedHandle(handle,IID_PPV_ARGS(&images_[i].object));
            CloseHandle(handle); neural::Check(hr,"Open flow input in D3D12");
            images_[i].width=w; images_[i].height=h; images_[i].format=desc.Format;
        }
        neural::Check(dev->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&ready11_)),"Create input ready fence");
        HANDLE handle=nullptr;
        neural::Check(ready11_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle),"Share input ready fence");
        auto hr=gpu.device->OpenSharedHandle(handle,IID_PPV_ARGS(&ready12_));
        CloseHandle(handle); neural::Check(hr,"Open input ready fence");
        neural::Check(gpu.device->CreateSharedHandle(gpu.fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"Share input release fence");
        hr=dev->OpenSharedFence(handle,IID_PPV_ARGS(&consumed11_));
        CloseHandle(handle); neural::Check(hr,"Open input release fence");
    }
    void Copy(neural::Gpu& gpu, ID3D11Texture2D* source, unsigned slot) {
        auto& image=images_.at(slot);
        if (image.state!=D3D12_RESOURCE_STATE_COMMON) throw std::runtime_error("Flow input not released to D3D11");
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        if (desc.Width!=image.width || desc.Height!=image.height || desc.Format!=image.format ||
            desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1)
            throw std::runtime_error("Unsupported flow input texture layout");
        if (consumed_[slot]) neural::Check(context_->Wait(consumed11_.Get(),consumed_[slot]),"Wait for input reuse");
        context_->CopyResource(textures_[slot].Get(),source);
        neural::Check(context_->Signal(ready11_.Get(),++readyValue_),"Signal flow input ready");
        context_->Flush();
        neural::Check(gpu.queue->Wait(ready12_.Get(),readyValue_),"Wait for capture input");
    }
    neural::Resource& Image(unsigned slot) { return images_.at(slot); }
    // Call after the final submitted use of BOTH real frames, with COMMON transitions in that recording.
    void MarkConsumed(neural::Gpu& gpu, uint64_t value) {
        if (value>gpu.LastSubmitted() || images_[0].state!=D3D12_RESOURCE_STATE_COMMON ||
            images_[1].state!=D3D12_RESOURCE_STATE_COMMON) throw std::runtime_error("Invalid input release");
        consumed_.fill(value);
    }
private:
    std::array<neural::Resource,2> images_;
    std::array<neural::ComPtr<ID3D11Texture2D>,2> textures_;
    neural::ComPtr<ID3D11DeviceContext4> context_;
    neural::ComPtr<ID3D11Fence> ready11_,consumed11_;
    neural::ComPtr<ID3D12Fence> ready12_;
    std::array<uint64_t,2> consumed_{};
    uint64_t readyValue_=0;
};
} // namespace moonsr
