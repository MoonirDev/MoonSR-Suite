// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "gpu.h"
#include <d3d11_4.h>

namespace moonsr {
// Single buffered output handoff. D3D11 must consume Array() before the next Publish on its immediate context.
// Caller must drain both queues before destruction or device recreation; this helper owns no background work.
class SharedFlowFields {
public:
    void Initialize(neural::Gpu& gpu, ID3D11Device* device, unsigned w, unsigned h) {
        neural::ComPtr<ID3D11Device5> dev;
        neural::Check(device->QueryInterface(IID_PPV_ARGS(&dev)), "Query flow sharing device");
        neural::ComPtr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(&immediate);
        neural::Check(immediate.As(&context_), "Query flow sharing context");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=w; desc.Height=h; desc.MipLevels=1; desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        for (unsigned d=0; d<2; ++d) {
            neural::Check(device->CreateTexture2D(&desc,nullptr,&textures_[d]), "Create shared flow texture");
            neural::ComPtr<IDXGIResource1> shared;
            neural::Check(textures_[d].As(&shared), "Query shared flow handle");
            HANDLE handle=nullptr;
            neural::Check(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle), "Share flow texture");
            const HRESULT opened=gpu.device->OpenSharedHandle(handle,IID_PPV_ARGS(&outputs_[d].object));
            CloseHandle(handle); neural::Check(opened,"Open flow texture in D3D12");
            outputs_[d].width=w; outputs_[d].height=h; outputs_[d].format=desc.Format;
        }
        desc.ArraySize=2; desc.MiscFlags=0;
        neural::Check(device->CreateTexture2D(&desc,nullptr,&array_), "Create flow array");
        neural::Check(device->CreateShaderResourceView(array_.Get(),nullptr,&srv_), "Create flow array SRV");
        HANDLE handle=nullptr;
        neural::Check(gpu.device->CreateSharedHandle(gpu.fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle), "Share flow completion");
        HRESULT opened=dev->OpenSharedFence(handle,IID_PPV_ARGS(&complete_));
        CloseHandle(handle); neural::Check(opened,"Open flow completion in D3D11");
        neural::Check(dev->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&released11_)), "Create flow release fence");
        neural::Check(released11_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle), "Share flow release fence");
        opened=gpu.device->OpenSharedHandle(handle,IID_PPV_ARGS(&released12_));
        CloseHandle(handle); neural::Check(opened,"Open flow release fence in D3D12");
    }
    neural::Resource& Output(unsigned direction) { return outputs_.at(direction); }
    ID3D11Texture2D* Array() const { return array_.Get(); }
    ID3D11ShaderResourceView* Srv() const { return srv_.Get(); }
    void Publish(neural::Gpu& gpu, neural::Recording& rec) {
        for (auto& output:outputs_) output.Transition(rec.list.Get(),D3D12_RESOURCE_STATE_COMMON);
        const uint64_t done=gpu.Submit(rec);
        neural::Check(context_->Wait(complete_.Get(),done), "Wait for flow output in D3D11");
        for (unsigned d=0; d<2; ++d) context_->CopySubresourceRegion(array_.Get(),d,0,0,0,textures_[d].Get(),0,nullptr);
        neural::Check(context_->Signal(released11_.Get(),++releaseValue_), "Release shared flow output");
        context_->Flush();
        neural::Check(gpu.queue->Wait(released12_.Get(),releaseValue_), "Wait before reusing shared flow output");
    }
private:
    std::array<neural::Resource,2> outputs_;
    std::array<neural::ComPtr<ID3D11Texture2D>,2> textures_;
    neural::ComPtr<ID3D11Texture2D> array_;
    neural::ComPtr<ID3D11ShaderResourceView> srv_;
    neural::ComPtr<ID3D11DeviceContext4> context_;
    neural::ComPtr<ID3D11Fence> complete_,released11_;
    neural::ComPtr<ID3D12Fence> released12_;
    uint64_t releaseValue_=0;
};
} // namespace moonsr
