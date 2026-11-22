// Monitor enumeration: GDI monitor handles + QueryDisplayConfig (friendly name, exact refresh) + DXGI outputs
// (driving adapter, HDR colour space).
//
// Threading: EnumerateMonitors() is stateless and callable from any thread. It temporarily switches the calling
// thread to per-monitor-v2 DPI awareness so rectangles are physical pixels.
#include "gpu/gpu_device.h"

#include "core/log.h"
#include "gpu/dpi_scope.h"

#include <vector>

using Microsoft::WRL::ComPtr;

namespace moonsr {

namespace {

struct DisplayConfigEntry {
    std::wstring gdiName;      // \\.\DISPLAYn
    std::wstring friendlyName; // monitor model
    double refreshHz = 0.0;
    LUID adapterLuid{};
};

double Rational(const DISPLAYCONFIG_RATIONAL& r) {
    return r.Denominator ? static_cast<double>(r.Numerator) / static_cast<double>(r.Denominator) : 0.0;
}

std::vector<DisplayConfigEntry> QueryActivePaths() {
    std::vector<DisplayConfigEntry> out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (rc != ERROR_SUCCESS) break;
        paths.resize(pathCount);
        modes.resize(modeCount);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(pathCount);
            modes.resize(modeCount);
        }
    }
    if (rc != ERROR_SUCCESS) {
        MSR_LOG_WARN(L"EnumerateMonitors: QueryDisplayConfig failed: {}", HrToString(HRESULT_FROM_WIN32(rc)));
        return out;
    }

    for (const auto& p : paths) {
        DisplayConfigEntry e;
        e.adapterLuid = p.sourceInfo.adapterId;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        e.gdiName = src.viewGdiDeviceName;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) e.friendlyName = tgt.monitorFriendlyDeviceName;

        // Path refresh rate is the exact rational desktop rate. Fall back to the target mode's vsync frequency.
        e.refreshHz = Rational(p.targetInfo.refreshRate);
        if (e.refreshHz <= 0.0 && p.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            p.targetInfo.modeInfoIdx < modes.size()) {
            const auto& m = modes[p.targetInfo.modeInfoIdx];
            if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
                e.refreshHz = Rational(m.targetMode.targetVideoSignalInfo.vSyncFreq);
        }
        out.push_back(std::move(e));
    }
    return out;
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor);
    return TRUE;
}

} // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    ScopedPerMonitorDpi dpi;
    std::vector<MonitorInfo> result;

    std::vector<HMONITOR> handles;
    if (!EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&handles))) {
        MSR_LOG_ERROR(L"EnumerateMonitors: EnumDisplayMonitors failed: {}", HrToString(HRESULT_FROM_WIN32(GetLastError())));
        return result;
    }
    const std::vector<DisplayConfigEntry> paths = QueryActivePaths();

    // Map each DXGI output's HMONITOR to its adapter and HDR state.
    struct OutputEntry {
        HMONITOR monitor;
        LUID luid;
        bool hdr;
    };
    std::vector<OutputEntry> outputs;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) == S_OK; ++a) {
            DXGI_ADAPTER_DESC1 ad{};
            if (FAILED(adapter->GetDesc1(&ad))) continue;
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) == S_OK; ++o) {
                OutputEntry e{nullptr, ad.AdapterLuid, false};
                ComPtr<IDXGIOutput6> o6;
                DXGI_OUTPUT_DESC1 d1{};
                if (SUCCEEDED(output.As(&o6)) && SUCCEEDED(o6->GetDesc1(&d1))) {
                    e.monitor = d1.Monitor;
                    e.hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                } else {
                    DXGI_OUTPUT_DESC d{};
                    if (FAILED(output->GetDesc(&d))) continue;
                    e.monitor = d.Monitor;
                }
                outputs.push_back(e);
            }
        }
    } else {
        MSR_LOG_WARN(L"EnumerateMonitors: CreateDXGIFactory2 failed: {}", HrToString(hr));
    }

    for (HMONITOR h : handles) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(h, &mi)) continue;
        MonitorInfo info;
        info.handle = h;
        info.deviceName = mi.szDevice;
        info.desktopRect = mi.rcMonitor;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        bool haveLuid = false;
        for (const auto& o : outputs) {
            if (o.monitor == h) {
                info.adapterLuid = o.luid;
                info.hdrEnabled = o.hdr;
                haveLuid = true;
                break;
            }
        }
        for (const auto& p : paths) {
            if (p.gdiName != info.deviceName) continue;
            info.friendlyName = p.friendlyName;
            info.refreshHz = p.refreshHz;
            if (!haveLuid) info.adapterLuid = p.adapterLuid; // may be a display-only adapter LUID
            break;
        }
        if (info.refreshHz <= 0.0) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(info.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                info.refreshHz = static_cast<double>(dm.dmDisplayFrequency);
        }
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace moonsr
