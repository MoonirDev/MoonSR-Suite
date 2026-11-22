// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "flow_shared.h"
#include "fg_flow_consistency.h"
#include <cmath>
#include <cstdio>
#include <vector>

inline void CheckFlowConfidence(ID3D11Device* device, ID3D11DeviceContext* context,
                                moonsr::SharedFlowFields& shared, unsigned width, unsigned height) {
    using neural::Check;
    using neural::ComPtr;
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(g_fg_flow_consistency,sizeof(g_fg_flow_consistency),nullptr,&shader),"Create consistency shader");
    D3D11_TEXTURE2D_DESC desc{}; shared.Array()->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> output,staging;
    Check(device->CreateTexture2D(&desc,nullptr,&output),"Create confidence field");
    ComPtr<ID3D11UnorderedAccessView> outputUav;
    Check(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputUav),"Create confidence UAV");
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc,nullptr,&staging),"Create confidence readback");
    struct Constants { int sizes[8]; float p0[4],p1[4]; unsigned u0[4],u1[4]; } constants{};
    static_assert(sizeof(constants)==96);
    constants.sizes[0]=width; constants.sizes[1]=height; constants.sizes[2]=desc.Width; constants.sizes[3]=desc.Height;
    constants.p0[0]=.75f; constants.p0[1]=.1f; constants.p0[2]=1; constants.p0[3]=4;
    constants.p1[0]=.35f; constants.p1[1]=.8f; constants.p1[2]=.6f;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=96; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{&constants};
    ComPtr<ID3D11Buffer> cb,stats;
    Check(device->CreateBuffer(&bd,&initial,&cb),"Create confidence constants");
    bd.ByteWidth=64; bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    Check(device->CreateBuffer(&bd,nullptr,&stats),"Create confidence statistics");
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format=DXGI_FORMAT_R32_UINT; ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements=16;
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    Check(device->CreateUnorderedAccessView(stats.Get(),&ud,&statsUav),"Create confidence statistics UAV");
    for (unsigned test=0; test<2; ++test) {
        if (test) {
            // Contradict direction 0 without changing its measured photometric residual.
            std::vector<float> zero(size_t(desc.Width)*desc.Height*4,0);
            context->UpdateSubresource(shared.Array(),1,nullptr,zero.data(),desc.Width*16,0);
        }
        const UINT zeros[4]{};
        context->ClearUnorderedAccessViewUint(statsUav.Get(),zeros);
        ID3D11ShaderResourceView* input=shared.Srv();
        ID3D11UnorderedAccessView* outputs[]={outputUav.Get(),statsUav.Get()};
        ID3D11Buffer* constant=cb.Get();
        context->CSSetShader(shader.Get(),nullptr,0); context->CSSetConstantBuffers(0,1,&constant);
        context->CSSetShaderResources(0,1,&input); context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
        context->Dispatch((desc.Width+7)/8,(desc.Height+7)/8,2);
        input=nullptr; outputs[0]=outputs[1]=nullptr;
        context->CSSetShaderResources(0,1,&input); context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
        context->CSSetShader(nullptr,nullptr,0);
        context->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read confidence");
        double sum=0; unsigned count=0; bool valid=true;
        for (unsigned y=8;y+8<desc.Height;++y) {
            auto row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch);
            for (unsigned x=8;x+8<desc.Width;++x) {
                const float confidence=row[x*4+2],rel=row[x*4+3];
                valid=valid&&std::isfinite(confidence)&&confidence>=0&&confidence<=1&&std::isfinite(rel)&&rel>=0;
                sum+=confidence; ++count;
            }
        }
        context->Unmap(staging.Get(),0);
        const double mean=sum/count;
        printf("consistency %s: mean interior confidence %.5f\n",test?"contradicted":"matched",mean);
        if (!valid || (!test && mean<.9) || (test && mean>.1)) throw std::runtime_error("Hardware flow confidence gate failed");
    }
}
