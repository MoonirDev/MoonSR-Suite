// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bidirectional_flow.h"
#include "flow_input.h"
#include "flow_shared.h"
#include <memory>

namespace moonsr {
// Experimental full-resolution motion provider. Produces refinement-like fields, not final confidence.
// All calls and downstream consumption use the supplied D3D11 device's immediate-context thread.
class HardwareFlowAnalysis {
    struct State {
        neural::Gpu gpu;
        BidirectionalFlow flow;
        SharedFlowInputs inputs;
        SharedFlowFields fields;
        std::array<neural::Recording,4> recordings;
        unsigned next=0;
        unsigned currentSlot=1;
        uint64_t sequence=0;
        bool temporalHints=false;
        neural::Recording& Begin() {
            auto& rec=recordings[next++ % recordings.size()];
            if (rec.completion && !gpu.Complete(rec.completion) && !gpu.Wait(rec.completion))
                throw std::runtime_error("Hardware flow recording reuse timed out");
            rec.Begin(gpu.fence.Get());
            return rec;
        }
    };
public:
    HardwareFlowAnalysis(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format,
                         bool temporalHints=false) {
        auto s=std::make_unique<State>();
        neural::ComPtr<IDXGIDevice> dxgi;
        neural::Check(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"Query hardware flow adapter");
        neural::ComPtr<IDXGIAdapter> adapter;
        neural::Check(dxgi->GetAdapter(&adapter),"Get hardware flow adapter");
        DXGI_ADAPTER_DESC desc{};
        neural::Check(adapter->GetDesc(&desc),"Read hardware flow adapter");
        s->gpu.Initialize(&desc.AdapterLuid);
        s->temporalHints=temporalHints;
        s->flow.Initialize(s->gpu,width,height);
        s->inputs.Initialize(s->gpu,device,width,height,format);
        s->fields.Initialize(s->gpu,device,(width+3)/4,(height+3)/4);
        for (auto& rec:s->recordings) rec.Initialize(s->gpu.device.Get());
        state_=std::move(s);
    }
    ~HardwareFlowAnalysis() {
        if (!state_) return;
        // Never free resources or command allocators still used by either API after driver/device failure.
        try {
            if (!state_->gpu.Wait(state_->gpu.Signal()) || !state_->flow.Shutdown()) state_.release();
        } catch (...) { state_.release(); }
    }
    ID3D11ShaderResourceView* Analyse(ID3D11Texture2D* previous, ID3D11Texture2D* current, uint64_t sequence=0) {
        auto& s=*state_;
        // Sequence is the generator's monotonically increasing REAL frame ID, reset with its history.
        // Missing/duplicate/discontinuous IDs rebuild both slots; resource pointer identity is insufficient.
        const bool consecutive=sequence && s.sequence && sequence>s.sequence && sequence-s.sequence==1;
        s.sequence=0; // an exception must never leave reusable history marked valid
        if (consecutive) s.currentSlot^=1;
        else s.currentSlot=1;
        const unsigned previousSlot=s.currentSlot^1;
        if (!consecutive) s.inputs.Copy(s.gpu,previous,previousSlot);
        s.inputs.Copy(s.gpu,current,s.currentSlot);
        auto& packed=s.Begin();
        if (!consecutive) s.flow.Prepare(s.gpu,packed,s.inputs.Image(previousSlot),previousSlot);
        s.flow.Prepare(s.gpu,packed,s.inputs.Image(s.currentSlot),s.currentSlot);
        const auto ready=s.gpu.Submit(packed);
        s.flow.Execute(s.gpu,s.currentSlot,previousSlot,consecutive && s.temporalHints,ready);
        auto& converted=s.Begin();
        s.flow.Convert(s.gpu,converted,s.inputs.Image(previousSlot),s.inputs.Image(s.currentSlot),{&s.fields.Output(0),&s.fields.Output(1)});
        for (unsigned slot=0;slot<2;++slot)
            s.inputs.Image(slot).Transition(converted.list.Get(),D3D12_RESOURCE_STATE_COMMON);
        s.fields.Publish(s.gpu,converted);
        s.inputs.MarkConsumed(s.gpu,s.gpu.LastSubmitted());
        s.sequence=sequence;
        return s.fields.Srv();
    }
private:
    std::unique_ptr<State> state_;
};
} // namespace moonsr
