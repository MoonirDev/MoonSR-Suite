// Host for the optional, separately built neural-rendering bridge. Pipeline thread only.
#pragma once

#include "core/types.h"
#include "neural/neural_settings.h"

#include <memory>
#include <string>

namespace moonsr {

class GpuDevice;

class NeuralBridge {
public:
    // The folder the bridge loads a vendor runtime from: amd-nr or intel-nr beside MoonSR.exe (empty for other vendors).
    static std::wstring RuntimeDirectory(Vendor vendor);
    static std::wstring AmdRuntimePath();
    // intel-nr\d5c_chain.dll beside MoonSR.exe when that folder also holds gemm.spv and dlssnr-logical.safetensors.
    static std::wstring IntelRuntimePath();
    static std::unique_ptr<NeuralBridge> Create(GpuDevice& device, const std::wstring& modelPath,
                                               const NeuralSettings& settings, std::wstring& error);
    virtual ~NeuralBridge() = default;
    virtual bool SupportsLiveSettings() const = 0;
    virtual bool UpdateSettings(const NeuralSettings& settings, std::wstring& error) = 0;

    // Processes an original capture. The returned texture is borrowed and remains valid until the next call.
    virtual bool Process(ID3D11Texture2D* texture, const RectI& rect, int64_t captureQpc, uint64_t sequence,
                         bool reset, const PictureSettings& picture, ID3D11Texture2D*& enhanced,
                         RectI& enhancedRect, std::wstring& error) = 0;
    virtual bool ProcessWithGuide(ID3D11Texture2D* texture, const RectI& rect, ID3D11Texture2D* flowGuide,
                                  int64_t captureQpc, uint64_t sequence, bool reset, const PictureSettings& picture,
                                  ID3D11Texture2D*& enhanced, RectI& enhancedRect, std::wstring& error) = 0;
};

} // namespace moonsr
