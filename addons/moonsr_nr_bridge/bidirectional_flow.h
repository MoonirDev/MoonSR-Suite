// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "optical_flow.h"
#include "fg_flow_metrics.h"

namespace moonsr {
// Flow-only analysis: no neural model or compositor dependency. GPU resources remain borrowed
// until the next Execute/Initialize. The caller owns input upload and D3D11/D3D12 sharing.
class BidirectionalFlow {
public:
    void Initialize(neural::Gpu& gpu, uint32_t width, uint32_t height,
                    NV_OF_PERF_LEVEL preset = NV_OF_PERF_LEVEL_MEDIUM) {
        valid_ = false;
        for (auto& flow : flows_) flow.Initialize(gpu, width, height, 4, preset);
        metrics_.Initialize(gpu.device.Get(), g_fg_flow_metrics, sizeof(g_fg_flow_metrics));
        width_ = width; height_ = height;
    }
    void Prepare(neural::Gpu& gpu, neural::Recording& rec, neural::Resource& image, uint32_t slot) {
        flows_[0].Prepare(gpu, rec, image, slot);
        flows_[1].PrepareFrom(gpu, rec, flows_[0], slot);
    }
    // Execute only on real-frame slots. The queue wait inserted by OpticalFlow orders consumers
    // after both outputs; callers must submit subsequent conversion work on this same queue.
    void Execute(neural::Gpu& gpu, uint32_t current, uint32_t previous, bool useTemporalHints,
                 uint64_t inputsReady) {
        valid_ = false;
        if (inputsReady > gpu.LastSubmitted()) throw std::runtime_error("Flow inputs were not submitted");
        for (unsigned d = 0; d < 2; ++d) {
            if (!useTemporalHints) flows_[d].Invalidate();
            flows_[d].Execute(gpu, d ? previous : current, d ? current : previous, false,
                              d ? gpu.LastSubmitted() : inputsReady);
        }
        valid_ = true;
    }
    void Reset() {
        valid_ = false;
        for (auto& flow : flows_) flow.Invalidate();
    }
    // Slice 0 convention matches FrameGenerator: previous -> current. The underlying wrapper's
    // input/current -> reference/previous output is reversed here by selecting the other session.
    neural::Resource& Grid(unsigned direction) {
        if (!valid_ || direction > 1) throw std::runtime_error("No valid bidirectional flow");
        return flows_[1 - direction].Grid();
    }
    bool Shutdown() {
        valid_ = false;
        bool ok = true;
        for (auto& flow : flows_) ok = flow.Shutdown() && ok;
        return ok;
    }
    void Convert(neural::Gpu& gpu, neural::Recording& rec, neural::Resource& previous,
                 neural::Resource& current, std::array<neural::Resource*, 2> outputs) {
        if (!valid_) throw std::runtime_error("Cannot convert invalid flow");
        previous.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        current.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (unsigned d = 0; d < 2; ++d) {
            auto& grid = Grid(d);
            if (!outputs[d] || outputs[d]->width != grid.width || outputs[d]->height != grid.height ||
                outputs[d]->format != DXGI_FORMAT_R32G32B32A32_FLOAT)
                throw std::runtime_error("Invalid converted flow output");
            grid.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            outputs[d]->Transition(rec.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const uint32_t constants[] = {width_, height_, grid.width, grid.height, d, 0, 0, 0};
            metrics_.Dispatch(gpu.device.Get(), rec, constants, sizeof(constants),
                              {&grid, &previous, &current}, {outputs[d]}, (grid.width+7)/8, (grid.height+7)/8);
            grid.Transition(rec.list.Get(), D3D12_RESOURCE_STATE_COMMON);
        }
    }
private:
    std::array<neural::OpticalFlow, 2> flows_;
    bool valid_ = false;
    neural::Kernel metrics_;
    uint32_t width_ = 0, height_ = 0;
};
} // namespace moonsr
