// MoonSR core types shared by every module. Owned by the lead; changes require lead approval.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

namespace moonsr {

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const Size&) const = default;
    bool Empty() const { return width == 0 || height == 0; }
};

// Half-open integer rectangle [left, right) x [top, bottom).
struct RectI {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    bool operator==(const RectI&) const = default;
    int32_t Width() const { return right - left; }
    int32_t Height() const { return bottom - top; }
    bool Empty() const { return right <= left || bottom <= top; }
};

enum class Vendor : uint32_t { Unknown = 0, Nvidia = 0x10DE, Amd = 0x1002, Intel = 0x8086, Microsoft = 0x1414 };

// Capabilities are detected by querying the API, never assumed from the vendor.
struct AdapterInfo {
    LUID luid{};
    std::wstring name;
    Vendor vendor = Vendor::Unknown;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t dedicatedVideoMemory = 0;  // bytes
    uint64_t sharedSystemMemory = 0;    // bytes
    uint64_t driverVersion = 0;         // packed UMD version: 4 x 16 bit (a.b.c.d), 0 if unknown
    bool isSoftware = false;            // WARP / Basic Render Driver
    bool isIntegrated = false;          // heuristic: no meaningful dedicated memory or UMA flag
    uint32_t outputCount = 0;           // monitors attached to this adapter
    // Filled by probing a device on the adapter:
    D3D_FEATURE_LEVEL maxFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
    bool supportsCompute = false;       // FL >= 11_0 compute shaders
    bool supportsTearing = false;       // DXGI_FEATURE_PRESENT_ALLOW_TEARING
    bool supportsR16FRenderTarget = false;
    bool supportsMinPrecision16 = false; // min16float honoured by the driver
};

struct MonitorInfo {
    HMONITOR handle = nullptr;
    std::wstring deviceName;   // e.g. \\.\DISPLAY1
    std::wstring friendlyName; // e.g. monitor model, may be empty
    RECT desktopRect{};        // virtual-desktop coordinates, physical pixels
    double refreshHz = 0.0;
    LUID adapterLuid{};        // adapter that drives this output
    bool primary = false;
    bool hdrEnabled = false;
};

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring exeName; // file name only, e.g. game.exe; empty if the process could not be opened
    uint32_t processId = 0;
    Size clientSize;
};

inline bool LuidEqual(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
inline bool LuidIsZero(const LUID& a) { return a.LowPart == 0 && a.HighPart == 0; }

// QueryPerformanceCounter helpers.
int64_t QpcNow();
double QpcToMs(int64_t ticks);
int64_t QpcFrequency();

} // namespace moonsr
