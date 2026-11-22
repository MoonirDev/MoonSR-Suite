// SPDX-License-Identifier: GPL-3.0-only
#include "intel_model.h"
#include "gpu.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace neural {
namespace {
std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 1)), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(std::strlen(out.c_str()));
    return out;
}
float Scale(unsigned percent) { return std::clamp(percent / 100.0f, 0.33f, 1.0f); } // runtime range 0.33 .. 1.0
} // namespace

IntelModel::~IntelModel() {
    if (context_ && destroy_) destroy_(context_);
    if (module_) FreeLibrary(module_);
    if (previousDriverSelect_)
        SetEnvironmentVariableW(L"VK_LOADER_DRIVERS_SELECT", hadDriverSelect_ ? previousDriverSelect_->c_str() : nullptr);
}

void IntelModel::Initialize(ID3D11Device* device, const std::wstring& runtimePath, unsigned width, unsigned height,
                            unsigned scalePercent, const ModelSettings& settings, float colourStrength) {
    const size_t cut = runtimePath.find_last_of(L"\\/");
    if (cut == std::wstring::npos) throw std::runtime_error("Intel NR runtime path has no folder");
    const std::wstring folder = runtimePath.substr(0, cut + 1);

    // Restrict the Vulkan loader to Intel drivers before the runtime creates its instance (see the header).
    wchar_t current[512]{};
    const DWORD currentLength = GetEnvironmentVariableW(L"VK_LOADER_DRIVERS_SELECT", current, 512);
    hadDriverSelect_ = currentLength > 0 && currentLength < 512;
    wchar_t wanted[256] = L"*igvk*";
    const DWORD overrideLength = GetEnvironmentVariableW(L"MOONSR_INTEL_NR_DRIVERS", wanted, 256);
    if (overrideLength == 0 || overrideLength >= 256) wcscpy_s(wanted, L"*igvk*");
    previousDriverSelect_ = hadDriverSelect_ ? std::wstring(current, currentLength) : std::wstring();
    SetEnvironmentVariableW(L"VK_LOADER_DRIVERS_SELECT", wanted);

    module_ = LoadLibraryExW(runtimePath.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_)
        throw std::runtime_error("Could not load d5c_chain.dll from intel-nr (Win32 error " +
                                 std::to_string(GetLastError()) + ")");
    auto resolve = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module_, name));
        if (!fn) throw std::runtime_error(std::string("d5c_chain.dll does not export ") + name);
    };
    resolve(create_, "dlss5_create");
    resolve(destroy_, "dlss5_destroy");
    resolve(process_, "dlss5_process_frame");
    resolve(setScale_, "dlss5_set_scale");
    resolve(setPhase30_, "dlss5_set_phase30");
    resolve(resetHistory_, "dlss5_reset_history");

    const std::string weights = Narrow(folder + L"dlssnr-logical.safetensors");
    const std::string shaders = Narrow(folder);
    context_ = create_(weights.c_str(), shaders.c_str(), Scale(scalePercent), 1.0f, 1.0f);
    if (!context_) throw std::runtime_error("dlss5_create failed (check the weights file and shaders in intel-nr)");
    SetAppearance(settings, colourStrength);

    width_ = width;
    height_ = height;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&d, nullptr, &staging_), "Create Intel NR readback");
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Check(device->CreateTexture2D(&d, nullptr, &output_), "Create Intel NR output");
    Check(device->CreateShaderResourceView(output_.Get(), nullptr, &outputSrv_), "Create Intel NR output view");
    input_.assign(size_t(width) * height * 4, 0);
    result_.assign(size_t(width) * height * 4, 0);
}

void IntelModel::SetAppearance(const ModelSettings& s, float colourStrength) {
    if (!context_) return;
    // Same Phase 30 controls as the NVIDIA runtime. Skin structure -1 disables it, as in the runtime's baseline.
    setPhase30_(context_, s.intensity, s.localTone, s.localStructure, s.autoMask ? s.skinStructure : -1.0f,
                s.autoMask ? 1 : 0, static_cast<int>(s.style), std::clamp(colourStrength, 0.0f, 2.0f));
}

void IntelModel::SetScale(unsigned scalePercent) {
    if (context_) setScale_(context_, Scale(scalePercent));
}

void IntelModel::Invalidate() {
    if (context_) resetHistory_(context_);
}

void IntelModel::Process(ID3D11DeviceContext* ctx, ID3D11Texture2D* original, uint64_t frameIndex) {
    ctx->CopyResource(staging_.Get(), original);
    D3D11_MAPPED_SUBRESOURCE map{};
    Check(ctx->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map), "Read back frame for Intel NR");
    const size_t row = size_t(width_) * 4;
    for (unsigned y = 0; y < height_; ++y)
        std::memcpy(input_.data() + y * row, static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, row);
    ctx->Unmap(staging_.Get(), 0);
    const int r = process_(context_, width_, height_, input_.data(), result_.data(), static_cast<long>(frameIndex));
    if (r != 0) throw std::runtime_error("dlss5_process_frame failed (" + std::to_string(r) + "); see the runtime's console log");
    ctx->UpdateSubresource(output_.Get(), 0, nullptr, result_.data(), static_cast<UINT>(row), 0);
}
} // namespace neural
