// SPDX-License-Identifier: GPL-3.0-only
// Intel backend: drives the DLSS5_INTEL runtime (d5c_chain.dll, Apache-2.0,
// https://github.com/geladons/DLSS5_INTEL) through its C ABI (dlss5/chain/chain_c_api.h, declared locally below).
// The runtime is user-built and user-installed with the user's own dlssnr-logical.safetensors; nothing is bundled.
//
// Data path. That ABI takes CPU-side BGRA8 buffers, so each frame is copied from D3D11 into a staging texture,
// read back, processed by the runtime on its own Vulkan device, and uploaded again. This is the one CPU readback in
// MoonSR's frame path, accepted because the runtime offers no GPU-resource interface. The call blocks the pipeline
// thread for the runtime's whole processing time (about 0.2 s per frame at 960x576 on an Arc Pro B50, per that
// project's own BENCHMARKS.md), which applies backpressure to capture like the other backends.
//
// Adapter. dlss5_create cannot be given an adapter: the runtime scores Vulkan devices itself and prefers any
// discrete GPU. On a machine with another vendor's discrete GPU it would leave the Intel GPU unused, so before the
// runtime creates its Vulkan instance VK_LOADER_DRIVERS_SELECT restricts the Vulkan loader to Intel's drivers
// ("*igvk*", overridable with MOONSR_INTEL_NR_DRIVERS). The previous value is restored on destruction. MoonSR uses
// Vulkan nowhere else.
//
// Threading: pipeline thread only (the same thread as the D3D11 immediate context it is given).
#pragma once

#include "model.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neural {
class IntelModel {
  public:
    IntelModel() = default;
    IntelModel(const IntelModel&) = delete;
    IntelModel& operator=(const IntelModel&) = delete;
    ~IntelModel();

    // runtimePath: d5c_chain.dll. Its folder must hold the .spv shaders and dlssnr-logical.safetensors.
    void Initialize(ID3D11Device*, const std::wstring& runtimePath, unsigned width, unsigned height,
                    unsigned scalePercent, const ModelSettings&, float colourStrength);
    void SetAppearance(const ModelSettings&, float colourStrength);
    void SetScale(unsigned scalePercent);
    void Invalidate();
    // Processes `original` (BGRA8, exactly width x height) into Output(). Blocks until the runtime returns.
    void Process(ID3D11DeviceContext*, ID3D11Texture2D* original, uint64_t frameIndex);
    ID3D11ShaderResourceView* Output() const { return outputSrv_.Get(); }

  private:
    using Create = void* (*)(const char*, const char*, float, float, float);
    using Destroy = void (*)(void*);
    using ProcessFrame = int (*)(void*, uint32_t, uint32_t, const void*, void*, long);
    using SetScaleFn = void (*)(void*, float);
    using SetPhase30 = void (*)(void*, float, float, float, float, int, int, float);
    using ResetHistory = void (*)(void*);

    HMODULE module_ = nullptr;
    void* context_ = nullptr;
    Create create_ = nullptr;
    Destroy destroy_ = nullptr;
    ProcessFrame process_ = nullptr;
    SetScaleFn setScale_ = nullptr;
    SetPhase30 setPhase30_ = nullptr;
    ResetHistory resetHistory_ = nullptr;
    std::optional<std::wstring> previousDriverSelect_; // value before we set it; nullopt = we did not set it
    bool hadDriverSelect_ = false;
    unsigned width_ = 0, height_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_, output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSrv_;
    std::vector<uint8_t> input_, result_; // width * height * 4, allocated once
};
} // namespace neural
