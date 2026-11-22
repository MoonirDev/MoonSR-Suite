// Owner: lead
// GPU scheduling priority for MoonSR's own work.
//
// MoonSR's picture is what reaches the screen, so on a GPU that the game keeps fully busy MoonSR's work (capture
// copy, upscaling, frame generation, the neural-rendering bridge's D3D12 queue and its optical flow, presentation)
// must run ahead of the game's next frame instead of being time-sliced with it. At equal priority the GPU is shared
// between the two processes, so MoonSR's frame time grows with the game's load long before the GPU is out of capacity
// (neural rendering, 1280x720 on an RTX 5060: 3.8 ms alone, 16 ms beside a full load).
//
// Measured on Windows 11 26300 with hardware-accelerated GPU scheduling (HAGS) on an RTX 5060, driver 621.14
// (tools/gpuload, documentation/BENCHMARKS.md): a 2.9 ms compute job beside a full competing load takes
//   7.4 ms at process GPU class NORMAL, ABOVE_NORMAL or HIGH, with or without device thread priority +7,
//   3.1 ms at class REALTIME (the same as on an idle GPU).
// So under HAGS only the REALTIME class puts MoonSR into a scheduling band above the game. It was granted to an
// unelevated process on that system. When it is refused, HIGH is used (it still helps on schedulers without HAGS).
//
//  - Process scheduling class (D3DKMTSetProcessSchedulingPriorityClass, gdi32). Applies to every device and queue
//    of the process, D3D11 and D3D12, including the neural bridge's own D3D12 device.
//  - Device thread priority +7 (IDXGIDevice::SetGPUThreadPriority). The neural bridge reads this value and creates
//    its D3D12 queue with D3D12_COMMAND_QUEUE_PRIORITY_HIGH only when it is above zero.
//
// Consequence: MoonSR's work always runs first; the game gets what remains. When MoonSR's own work needs more than
// the whole GPU, the game's frame rate falls instead of MoonSR's. MoonSR's work is bounded by its own pacing (one
// output frame per display refresh at most), so it cannot hold the GPU indefinitely.
//
// Header-only so that test executables outside the engine library can apply the same policy.
// Threading: call from the thread that owns the device, before its first submission.
#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <initializer_list>

namespace moonsr {

// D3DKMT_SCHEDULINGPRIORITYCLASS values (d3dkmthk.h).
enum class GpuSchedulingClass : int { Idle = 0, BelowNormal = 1, Normal = 2, AboveNormal = 3, High = 4, Realtime = 5 };

struct GpuPriorityResult {
    int processClass = -1;          // class read back after the call, -1 if it could not be read
    long processStatus = 0;         // NTSTATUS of the last set call (0 = success), -1 if the export is missing
    int deviceThreadPriority = 0;   // value read back from the device after the call
    HRESULT deviceHr = S_OK;
    bool Realtime() const { return processClass == static_cast<int>(GpuSchedulingClass::Realtime); }
};

inline const wchar_t* GpuSchedulingClassName(int value) {
    switch (value) {
    case 0: return L"idle";
    case 1: return L"below normal";
    case 2: return L"normal";
    case 3: return L"above normal";
    case 4: return L"high";
    case 5: return L"realtime";
    default: return L"unknown";
    }
}

// Raises the process GPU scheduling class to REALTIME, or HIGH if REALTIME is refused. Never lowers it.
inline void RaiseProcessGpuPriority(GpuPriorityResult& r) {
    using SetFn = LONG(APIENTRY*)(HANDLE, int);
    using GetFn = LONG(APIENTRY*)(HANDLE, int*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto set = gdi ? reinterpret_cast<SetFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass")) : nullptr;
    const auto get = gdi ? reinterpret_cast<GetFn>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass")) : nullptr;
    auto current = [&] {
        int value = -1;
        return get && get(GetCurrentProcess(), &value) == 0 ? value : -1;
    };
    r.processClass = current();
    if (!set) { r.processStatus = -1; return; }
    if (r.processClass >= static_cast<int>(GpuSchedulingClass::Realtime)) return;
    for (const auto wanted : {GpuSchedulingClass::Realtime, GpuSchedulingClass::High}) {
        if (r.processClass >= static_cast<int>(wanted)) break;
        r.processStatus = set(GetCurrentProcess(), static_cast<int>(wanted));
        if (r.processStatus == 0) {
            const int now = current();
            r.processClass = now >= 0 ? now : static_cast<int>(wanted);
            if (r.processClass >= static_cast<int>(wanted)) break;
        }
    }
}

inline GpuPriorityResult RaiseGpuPriority(ID3D11Device* device) {
    GpuPriorityResult r;
    RaiseProcessGpuPriority(r);
    if (!device) { r.deviceHr = E_POINTER; return r; }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    r.deviceHr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(r.deviceHr)) r.deviceHr = dxgi->SetGPUThreadPriority(7);
    INT readBack = 0;
    if (dxgi && SUCCEEDED(dxgi->GetGPUThreadPriority(&readBack))) r.deviceThreadPriority = readBack;
    return r;
}

} // namespace moonsr
