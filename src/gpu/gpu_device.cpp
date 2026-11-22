// GpuDevice implementation: DXGI adapter enumeration with API-queried capabilities, D3D11 device creation.
//
// Threading: EnumerateAdapters() is stateless and callable from any thread (it creates its own factory and
// temporary devices). GpuDeviceImpl is immutable after Create(); its accessors are thread-safe, but the immediate
// context it hands out must only be used by the owning (pipeline) thread. Multithread protection is enabled because
// Windows Graphics Capture uses the device from its own worker threads.
#include "gpu/gpu_device.h"

#include "core/log.h"

#include <d3d11_4.h> // ID3D11Multithread

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

bool Failed(HRESULT hr, std::wstring& error, const wchar_t* what) {
    if (SUCCEEDED(hr)) return false;
    error = std::format(L"{} failed: {}", what, HrToString(hr));
    MSR_LOG_ERROR(L"GpuDevice: {}", error);
    return true;
}

Vendor VendorFromId(uint32_t id) {
    switch (id) {
    case 0x10DE: return Vendor::Nvidia;
    case 0x1002: return Vendor::Amd;
    case 0x8086: return Vendor::Intel;
    case 0x1414: return Vendor::Microsoft;
    default: return Vendor::Unknown;
    }
}

// Creates a device on `adapter` with the highest feature level it supports. Tries the 12_x levels first; runtimes
// that do not know them reject the array with E_INVALIDARG, in which case the 11_1 / 11_0 / 10_x list is retried.
HRESULT CreateDeviceOnAdapter(IDXGIAdapter1* adapter, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                              ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context,
                              D3D_FEATURE_LEVEL& obtained) {
    return D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, levelCount, D3D11_SDK_VERSION,
                             device.ReleaseAndGetAddressOf(), &obtained, context.ReleaseAndGetAddressOf());
}

HRESULT CreateProbeDevice(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& device, D3D_FEATURE_LEVEL& obtained) {
    static constexpr D3D_FEATURE_LEVEL kAll[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                                 D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = CreateDeviceOnAdapter(adapter, 0, kAll, static_cast<UINT>(std::size(kAll)), device, ctx, obtained);
    if (hr == E_INVALIDARG)
        hr = CreateDeviceOnAdapter(adapter, 0, kAll + 2, static_cast<UINT>(std::size(kAll) - 2), device, ctx, obtained);
    return hr;
}

// Fills the device-independent fields (DXGI description, driver version, outputs) of `info`.
void FillAdapterBasics(IDXGIAdapter1* adapter, AdapterInfo& info) {
    DXGI_ADAPTER_DESC1 desc{};
    const HRESULT hr = adapter->GetDesc1(&desc);
    if (FAILED(hr)) {
        MSR_LOG_WARN(L"IDXGIAdapter1::GetDesc1 failed: {}", HrToString(hr));
        return;
    }
    info.luid = desc.AdapterLuid;
    info.name = desc.Description;
    info.vendorId = desc.VendorId;
    info.deviceId = desc.DeviceId;
    info.vendor = VendorFromId(desc.VendorId);
    info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
    info.sharedSystemMemory = desc.SharedSystemMemory;
    info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

    LARGE_INTEGER umd{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        info.driverVersion = static_cast<uint64_t>(umd.QuadPart);

    info.outputCount = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, &output) != S_OK) break;
        info.outputCount++;
    }
}

// Fills the capability fields of `info` by querying `device`.
void FillDeviceCaps(ID3D11Device* device, AdapterInfo& info) {
    info.maxFeatureLevel = device->GetFeatureLevel();

    if (info.maxFeatureLevel >= D3D_FEATURE_LEVEL_11_0) {
        info.supportsCompute = true;
    } else {
        // Not part of the 11_0 baseline, but report it honestly for 10_x hardware.
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS opts{};
        info.supportsCompute =
            SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &opts, sizeof(opts))) &&
            opts.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
    }

    UINT fmt = 0;
    info.supportsR16FRenderTarget = SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &fmt)) &&
                                    (fmt & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;

    // min16float is "honoured" only if the driver reports 16-bit min precision for the pixel stage AND the other
    // stages (compute is in "all other stages"). Otherwise min16float silently runs at 32 bit, which is still correct.
    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT mp{};
    info.supportsMinPrecision16 =
        SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &mp, sizeof(mp))) &&
        (mp.PixelShaderMinPrecision & D3D11_SHADER_MIN_PRECISION_16_BIT) &&
        (mp.AllOtherShaderStagesMinPrecision & D3D11_SHADER_MIN_PRECISION_16_BIT);

    // Integrated GPU: prefer the API's unified-memory flag; fall back to a dedicated-memory heuristic.
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))))
        info.isIntegrated = o2.UnifiedMemoryArchitecture != FALSE;
    else
        info.isIntegrated = info.dedicatedVideoMemory < (512ull << 20);
    if (info.isSoftware) info.isIntegrated = false;
}

bool QueryTearing(IDXGIFactory1* factory) {
    ComPtr<IDXGIFactory5> f5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) return false;
    BOOL allow = FALSE;
    if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) return false;
    return allow != FALSE;
}

class GpuDeviceImpl final : public GpuDevice {
public:
    ComPtr<ID3D11Device1> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory2> factory;
    AdapterInfo info;

    ID3D11Device1* Device() const override { return device.Get(); }
    ID3D11DeviceContext1* Context() const override { return context.Get(); }
    IDXGIAdapter1* Adapter() const override { return adapter.Get(); }
    IDXGIFactory2* Factory() const override { return factory.Get(); }
    const AdapterInfo& Info() const override { return info; }
    HRESULT DeviceStatus() const override { return device->GetDeviceRemovedReason(); }
};

} // namespace

std::vector<AdapterInfo> GpuDevice::EnumerateAdapters() {
    std::vector<AdapterInfo> result;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        MSR_LOG_ERROR(L"EnumerateAdapters: CreateDXGIFactory2 failed: {}", HrToString(hr));
        return result;
    }
    const bool tearing = QueryTearing(factory.Get());

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            MSR_LOG_WARN(L"EnumerateAdapters: EnumAdapters1({}) failed: {}", i, HrToString(hr));
            break;
        }
        AdapterInfo info;
        FillAdapterBasics(adapter.Get(), info);
        info.supportsTearing = tearing;

        ComPtr<ID3D11Device> probe;
        D3D_FEATURE_LEVEL fl{};
        hr = CreateProbeDevice(adapter.Get(), probe, fl);
        if (SUCCEEDED(hr)) {
            FillDeviceCaps(probe.Get(), info);
        } else {
            MSR_LOG_WARN(L"EnumerateAdapters: no D3D11 device on '{}': {}", info.name, HrToString(hr));
            info.isIntegrated = !info.isSoftware && info.dedicatedVideoMemory < (512ull << 20);
        }
        result.push_back(std::move(info));
    }

    std::stable_partition(result.begin(), result.end(), [](const AdapterInfo& a) { return !a.isSoftware; });
    return result;
}

std::unique_ptr<GpuDevice> GpuDevice::Create(const LUID* adapterLuid, std::wstring& error) {
    error.clear();
    ComPtr<IDXGIFactory4> factory;
    if (Failed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), error, L"CreateDXGIFactory2")) return nullptr;

    ComPtr<IDXGIAdapter1> adapter;
    if (adapterLuid && !LuidIsZero(*adapterLuid)) {
        const HRESULT hr = factory->EnumAdapterByLuid(*adapterLuid, IID_PPV_ARGS(&adapter));
        if (FAILED(hr)) {
            error = std::format(L"No adapter with LUID {:08X}:{:08X} ({})", static_cast<uint32_t>(adapterLuid->HighPart),
                                adapterLuid->LowPart, HrToString(hr));
            MSR_LOG_ERROR(L"GpuDevice: {}", error);
            return nullptr;
        }
    } else {
        ComPtr<IDXGIFactory6> f6;
        if (SUCCEEDED(factory.As(&f6)))
            f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (!adapter && Failed(factory->EnumAdapters1(0, &adapter), error, L"EnumAdapters1(0)")) return nullptr;
    }

    auto impl = std::make_unique<GpuDeviceImpl>();
    impl->adapter = adapter;
    FillAdapterBasics(adapter.Get(), impl->info);
    impl->info.supportsTearing = QueryTearing(factory.Get());

    static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = CreateDeviceOnAdapter(adapter.Get(), flags, kLevels, 2, device, context, fl);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        // Debug layer (Graphics Tools optional feature) not installed.
        MSR_LOG_WARN(L"GpuDevice: debug layer unavailable ({}), creating without it", HrToString(hr));
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = CreateDeviceOnAdapter(adapter.Get(), flags, kLevels, 2, device, context, fl);
    }
    if (hr == E_INVALIDARG) // pre-11.1 runtime does not accept 11_1 in the array
        hr = CreateDeviceOnAdapter(adapter.Get(), flags, kLevels + 1, 1, device, context, fl);
    if (FAILED(hr)) {
        error = std::format(L"D3D11CreateDevice on '{}' (FL 11_0+, BGRA) failed: {}", impl->info.name, HrToString(hr));
        MSR_LOG_ERROR(L"GpuDevice: {}", error);
        return nullptr;
    }

    if (Failed(device.As(&impl->device), error, L"QueryInterface(ID3D11Device1)")) return nullptr;
    if (Failed(context.As(&impl->context), error, L"QueryInterface(ID3D11DeviceContext1)")) return nullptr;

    ComPtr<ID3D11Multithread> mt;
    if (Failed(context.As(&mt), error, L"QueryInterface(ID3D11Multithread)")) return nullptr;
    mt->SetMultithreadProtected(TRUE);

    // The factory must be the adapter's parent so swapchains are created on the matching factory.
    if (Failed(adapter->GetParent(IID_PPV_ARGS(&impl->factory)), error, L"IDXGIAdapter::GetParent(IDXGIFactory2)"))
        return nullptr;

    FillDeviceCaps(impl->device.Get(), impl->info);

    MSR_LOG_INFO(L"GpuDevice: created on '{}' (LUID {:08X}:{:08X}), FL {:X}, debug layer {}", impl->info.name,
                 static_cast<uint32_t>(impl->info.luid.HighPart), impl->info.luid.LowPart,
                 static_cast<uint32_t>(fl), (flags & D3D11_CREATE_DEVICE_DEBUG) ? L"on" : L"off");
    return impl;
}

} // namespace moonsr
