// On-screen frame counter. Header owned by the lead.
//
// Draws a short text such as "30/60" (frames rendered by the game per second / frames displayed per second) in the
// top-left corner of the output. Only the characters 0-9, '/', '.', ' ' and '-' are needed.
//
// Threading: one thread only (the thread that owns the GpuDevice immediate context).
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

namespace moonsr {

class StatsOverlay {
public:
    static std::unique_ptr<StatsOverlay> Create(GpuDevice& device, std::wstring& error);
    virtual ~StatsOverlay() = default;

    // Draws "<rendered>/<total>" with its top-left corner at a small margin from the top-left of the render target
    // (the output monitor). Text height scales with the target height (about 1/40 of it, at least 16 px), white
    // with a dark outline or backing so it is readable on any content. Called once per presented frame, so it must
    // be cheap: no allocation, no GDI work, no texture updates when the numbers have not changed.
    // Returns false only on a D3D failure (see log). Sets all pipeline state it relies on and leaves no render
    // target or shader resource bound.
    virtual bool Draw(ID3D11RenderTargetView* rtv, Size targetSize, uint32_t rendered, uint32_t total) = 0;
};

} // namespace moonsr
