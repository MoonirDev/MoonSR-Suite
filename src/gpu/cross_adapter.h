// Cross-adapter frame transfer. Header owned by the lead (interface contract). Implementation: gpu-rendering-engineer.
//
// Copies a texture region rendered on one GPU's device so that a device on ANOTHER GPU can read it in its next
// commands. Used by the engine between pipeline stages that run on different GPUs, once per frame and hop
// (generated frames included), so the cost of Transfer is paid in the frame path.
//
// Requirements:
//  - Vendor-neutral: any pair of D3D11 FL 11_0 adapters (NVIDIA, AMD, Intel, integrated or discrete). A GPU-to-GPU
//    path (for example D3D12 cross-adapter heaps with shared fences) is preferred where both adapters support it; a
//    CPU-staging path is the mandatory fallback. Method() says which one is in use.
//  - Ordering: after Transfer returns, commands that the destination device submits on its immediate context see the
//    copied data. Waiting must be done GPU-side where the method allows it; the CPU-staging fallback may wait on the
//    CPU (it is a fallback).
//  - No per-frame heap allocation; resources are created on the first call and recreated only when size or format
//    changes.
//  - RAII for every COM object and handle; every HRESULT checked; failures logged with MSR_LOG_* and returned.
//
// Threading: pipeline thread only. It uses both devices' immediate contexts, which only the pipeline thread uses.
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

struct ID3D11Texture2D;

namespace moonsr {

class CrossAdapterLink {
public:
    // `source` and `destination` must outlive the link and must be on different adapters.
    static std::unique_ptr<CrossAdapterLink> Create(GpuDevice& source, GpuDevice& destination, std::wstring& error);
    virtual ~CrossAdapterLink() = default;

    // Copies `rect` of `texture` (a 2D, single-sample, mip 0 / slice 0 texture of the source device) into a texture
    // owned by the link on the destination device, of size rect.Width() x rect.Height() and the same format, with
    // D3D11_BIND_SHADER_RESOURCE (plus RENDER_TARGET when the format allows it). Returns that texture; it stays valid
    // until the next Transfer or destruction. The copied region starts at (0, 0) of the returned texture.
    // Returns nullptr and fills `error` on failure.
    virtual ID3D11Texture2D* Transfer(ID3D11Texture2D* texture, const RectI& rect, std::wstring& error) = 0;

    // Short description of the transfer method in use, for logs and the status view, e.g. L"D3D12 cross-adapter
    // heap" or L"CPU staging".
    virtual const wchar_t* Method() const = 0;
};

} // namespace moonsr
