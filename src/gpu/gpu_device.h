// GPU module contract: adapter enumeration, capability detection, D3D11 device ownership.
// Header owned by the lead (interface contract). Implementation: gpu-rendering-engineer.
//
// Threading: EnumerateAdapters is callable from any thread. A GpuDevice instance and its immediate context are
// used by exactly one thread (the engine pipeline thread) unless a method says otherwise. The device is created
// WITHOUT D3D11_CREATE_DEVICE_SINGLETHREADED, and ID3D11Multithread::SetMultithreadProtected(TRUE) is enabled,
// because Windows Graphics Capture uses the device from its own threads.
#pragma once

#include "core/types.h"

#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace moonsr {

class GpuDevice {
public:
    // Enumerates hardware and software adapters. Each entry is fully populated, including the fields that require
    // probing a temporary device (feature level, compute, format support). Software adapters are listed last.
    static std::vector<AdapterInfo> EnumerateAdapters();

    // Creates a device on the adapter with the given LUID. If adapterLuid is null or zero, uses the
    // high-performance adapter (IDXGIFactory6::EnumAdapterByGpuPreference), falling back to adapter 0.
    // Requires feature level 11_0 or higher and BGRA support. Returns null and fills `error` on failure.
    static std::unique_ptr<GpuDevice> Create(const LUID* adapterLuid, std::wstring& error);

    virtual ~GpuDevice() = default;

    virtual ID3D11Device1* Device() const = 0;
    virtual ID3D11DeviceContext1* Context() const = 0;
    virtual IDXGIAdapter1* Adapter() const = 0;
    virtual IDXGIFactory2* Factory() const = 0;
    virtual const AdapterInfo& Info() const = 0;

    // S_OK while the device is healthy; otherwise the GetDeviceRemovedReason() HRESULT.
    virtual HRESULT DeviceStatus() const = 0;
};

// Monitors currently attached to the desktop, with the adapter that drives each one. Callable from any thread.
std::vector<MonitorInfo> EnumerateMonitors();

} // namespace moonsr
