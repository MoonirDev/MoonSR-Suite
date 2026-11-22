#include "neural/neural_bridge.h"

#include "gpu/gpu_device.h"
#include "moonsr_nr_bridge.h"

#include <windows.h>
#include <wrl/client.h>

#include <format>

namespace moonsr {
namespace {
using Microsoft::WRL::ComPtr;

MoonSrNrBridgeConfig BridgeSettings(const NeuralSettings& settings) {
    MoonSrNrBridgeConfig cfg{};
    cfg.size = sizeof(cfg);
    cfg.apiVersion = MOONSR_NR_BRIDGE_API_VERSION;
    cfg.passes = settings.passes;
    cfg.resolutionPercent = settings.resolutionPercent;
    cfg.preset = settings.preset;
    cfg.style = settings.style;
    cfg.intensityPercent = settings.intensityPercent;
    cfg.structurePercent = settings.structurePercent;
    cfg.tonePercent = settings.tonePercent;
    cfg.skinPercent = settings.skinPercent;
    cfg.detailPercent = settings.detailPercent;
    cfg.colourPercent = settings.colourPercent;
    cfg.autoMask = settings.autoMask ? 1u : 0u;
    return cfg;
}

class NeuralBridgeImpl final : public NeuralBridge {
public:
    ~NeuralBridgeImpl() override {
        if (api_ && instance_) api_->Destroy(instance_);
        if (module_) FreeLibrary(module_);
    }

    bool SupportsLiveSettings() const override { return updateSettings_ != nullptr; }

    bool UpdateSettings(const NeuralSettings& settings, std::wstring& error) override {
        if (!updateSettings_) return false;
        const auto cfg = BridgeSettings(settings);
        const wchar_t* bridgeError = nullptr;
        if (updateSettings_(instance_, &cfg, &bridgeError) == MOONSR_NR_BRIDGE_OK) return true;
        error = bridgeError && *bridgeError ? bridgeError : L"The bridge rejected the model settings.";
        return false;
    }

    bool Process(ID3D11Texture2D* texture, const RectI& rect, int64_t captureQpc, uint64_t sequence, bool reset,
                 const PictureSettings& picture,
                 ID3D11Texture2D*& enhanced, RectI& enhancedRect, std::wstring& error) override {
        return ProcessWithGuide(texture, rect, nullptr, captureQpc, sequence, reset, picture, enhanced, enhancedRect, error);
    }

    bool ProcessWithGuide(ID3D11Texture2D* texture, const RectI& rect, ID3D11Texture2D* flowGuide,
                          int64_t captureQpc, uint64_t sequence, bool reset, const PictureSettings& picture,
                          ID3D11Texture2D*& enhanced, RectI& enhancedRect, std::wstring& error) override {
        enhanced = nullptr;
        enhancedRect = {};
        if (!texture || rect.Empty()) {
            error = L"Neural bridge received an invalid capture texture or rectangle.";
            return false;
        }
        MoonSrNrBridgeFrame frame{};
        frame.size = sizeof(frame);
        frame.texture = texture;
        frame.flowGuide = flowGuide;
        frame.left = rect.left;
        frame.top = rect.top;
        frame.right = rect.right;
        frame.bottom = rect.bottom;
        frame.captureQpc = captureQpc;
        frame.sequence = sequence;
        frame.reset = reset ? 1u : 0u;
        frame.pictureEnabled = picture.enabled ? 1u : 0u;
        frame.exposure = picture.exposure; frame.contrast = picture.contrast; frame.midtones = picture.midtones;
        frame.shadows = picture.shadows; frame.highlights = picture.highlights; frame.saturation = picture.saturation;
        frame.vibrance = picture.vibrance; frame.temperature = picture.temperature; frame.tint = picture.tint;
        frame.sharpen = picture.sharpen;
        MoonSrNrBridgeOutput output{};
        output.size = sizeof(output);
        const wchar_t* bridgeError = nullptr;
        const MoonSrNrBridgeResult r = api_->Process(instance_, &frame, &output, &bridgeError);
        if (r != MOONSR_NR_BRIDGE_OK || !output.texture || output.right <= output.left || output.bottom <= output.top) {
            error = bridgeError && *bridgeError ? bridgeError : std::format(L"Neural bridge Process failed ({}).", static_cast<unsigned>(r));
            return false;
        }
        enhanced = static_cast<ID3D11Texture2D*>(output.texture);
        D3D11_TEXTURE2D_DESC desc{};
        enhanced->GetDesc(&desc);
        ComPtr<ID3D11Device> owner;
        enhanced->GetDevice(&owner);
        if (owner.Get() != device_ || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || output.left < 0 || output.top < 0 ||
            static_cast<uint32_t>(output.right) > desc.Width || static_cast<uint32_t>(output.bottom) > desc.Height) {
            enhanced = nullptr;
            error = L"Neural bridge returned an invalid texture, device, or output rectangle.";
            return false;
        }
        enhancedRect = {output.left, output.top, output.right, output.bottom};
        return true;
    }

    HMODULE module_ = nullptr;
    const MoonSrNrBridgeApi* api_ = nullptr;
    MoonSrNrBridgeUpdateSettingsFn updateSettings_ = nullptr;
    void* instance_ = nullptr;
    ID3D11Device* device_ = nullptr; // borrowed; engine destroys bridge before device
};

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring out(path, n);
    const size_t cut = out.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : out.substr(0, cut);
}
} // namespace

std::wstring NeuralBridge::AmdRuntimePath() {
    const auto directory = ExecutableDirectory();
    if (directory.empty()) return {};
    const auto base = directory + L"\\amd-nr\\LmxxfNrRuntime";
    for (const wchar_t* suffix : {L".dll", L".pak"}) {
        const DWORD attrs = GetFileAttributesW((base + suffix).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return {};
    }
    return base + L".dll";
}

std::wstring NeuralBridge::IntelRuntimePath() {
    const auto directory = ExecutableDirectory();
    if (directory.empty()) return {};
    const auto base = directory + L"\\intel-nr\\";
    for (const wchar_t* name : {L"d5c_chain.dll", L"gemm.spv", L"dlssnr-logical.safetensors"}) {
        const DWORD attrs = GetFileAttributesW((base + name).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return {};
    }
    return base + L"d5c_chain.dll";
}

std::unique_ptr<NeuralBridge> NeuralBridge::Create(GpuDevice& device, const std::wstring& modelPath,
                                                  const NeuralSettings& settings, std::wstring& error) {
    const Vendor vendor = device.Info().vendor;
    if (vendor != Vendor::Nvidia && vendor != Vendor::Amd && vendor != Vendor::Intel) {
        error = L"Neural rendering requires an AMD, Intel or NVIDIA processing adapter.";
        return nullptr;
    }
    const std::wstring runtime = vendor == Vendor::Amd     ? AmdRuntimePath()
                                 : vendor == Vendor::Intel ? IntelRuntimePath()
                                                           : modelPath;
    if (vendor == Vendor::Amd && runtime.empty()) {
        error = L"Install LmxxfNrRuntime.dll and LmxxfNrRuntime.pak in the amd-nr folder beside MoonSR.";
        return nullptr;
    }
    if (vendor == Vendor::Intel && runtime.empty()) {
        error = L"Install d5c_chain.dll, its .spv shaders and dlssnr-logical.safetensors in the intel-nr folder beside MoonSR.";
        return nullptr;
    }
    if (runtime.empty() || GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"The selected DLSS Neural Rendering model file is unavailable.";
        return nullptr;
    }
    const std::wstring dir = ExecutableDirectory();
    const std::wstring path = dir.empty() ? L"MoonSR_NrBridge.dll" : dir + L"\\MoonSR_NrBridge.dll";
    auto out = std::make_unique<NeuralBridgeImpl>();
    out->device_ = device.Device();
    out->module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!out->module_) {
        error = L"Could not load MoonSR_NrBridge.dll next to MoonSR: " + std::to_wstring(GetLastError());
        return nullptr;
    }
    const auto getApi = reinterpret_cast<MoonSrNrBridgeGetApiFn>(GetProcAddress(out->module_, "MoonSR_NrBridge_GetApi"));
    if (!getApi || !(out->api_ = getApi(MOONSR_NR_BRIDGE_API_VERSION)) || out->api_->size < sizeof(MoonSrNrBridgeApi) ||
        out->api_->apiVersion != MOONSR_NR_BRIDGE_API_VERSION || !out->api_->Create || !out->api_->Destroy || !out->api_->Process) {
        error = L"MoonSR_NrBridge.dll does not expose the required ABI version 4. Rebuild the bridge.";
        return nullptr;
    }
    out->updateSettings_ = reinterpret_cast<MoonSrNrBridgeUpdateSettingsFn>(
        GetProcAddress(out->module_, "MoonSR_NrBridge_UpdateSettings"));
    auto cfg = BridgeSettings(settings);
    cfg.d3d11Device = device.Device();
    cfg.d3d11Context = device.Context();
    cfg.modelPath = runtime.c_str();
    cfg.adapterVendorId = device.Info().vendorId;
    const wchar_t* bridgeError = nullptr;
    const MoonSrNrBridgeResult r = out->api_->Create(&cfg, &out->instance_, &bridgeError);
    if (r != MOONSR_NR_BRIDGE_OK || !out->instance_) {
        error = bridgeError && *bridgeError ? bridgeError : std::format(L"MoonSR_NrBridge Create failed ({}).", static_cast<unsigned>(r));
        return nullptr;
    }
    return out;
}

} // namespace moonsr
