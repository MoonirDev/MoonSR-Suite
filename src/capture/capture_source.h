// Capture module contract. Header owned by the lead. Implementation: gpu-rendering-engineer.
//
// A capture source delivers frames of one window as GPU textures on the GpuDevice it was created with.
// No CPU readback happens anywhere in this module.
//
// Threading: Create*, AcquireFrame, ReleaseFrame and the destructor are all called from the same thread (the
// engine pipeline thread). Implementations may receive OS callbacks on other threads internally, but must not
// touch the D3D11 immediate context from them.
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>
#include <vector>

namespace moonsr {

enum class CaptureStatus {
    Ok,         // `out` is valid until ReleaseFrame()
    Timeout,    // no new frame within the timeout (the window content did not change)
    TargetLost, // window closed or can no longer be captured; the source is dead
    DeviceLost, // D3D device removed/reset; the source is dead
    Error,      // unexpected failure, see log; the source is dead
};

struct CaptureOptions {
    bool captureCursor = true;    // draw the cursor into captured frames (if the method supports it)
    bool clientAreaOnly = true;   // crop title bar and borders via CapturedFrame::contentRect
    bool hideCaptureBorder = true; // request no yellow capture border where the OS allows it
    double maxFps = 0.0;          // 0 = as fast as the OS delivers; otherwise a hint for the minimum update interval
};

struct CapturedFrame {
    ID3D11Texture2D* texture = nullptr; // owned by the source; valid until ReleaseFrame(). BGRA8 UNORM for SDR.
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    Size textureSize;       // full texture dimensions
    RectI contentRect;      // region of the texture that holds the game image (after client-area cropping)
    int64_t captureQpc = 0; // QPC time at which the frame became available to the source (e.g. WGC FrameArrived)
    // Raw OS timestamp for the frame converted to QPC ticks, 0 if none. For WGC this is SystemRelativeTime, which
    // was measured to lie about one composition interval AFTER arrival (it behaves like a target display time).
    int64_t osTargetQpc = 0;
    uint64_t sequence = 0;  // increments for every frame the OS delivered, including ones dropped by the source
    uint32_t droppedBefore = 0; // frames that arrived and were discarded since the previous AcquireFrame (latest-wins)
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    // Waits up to timeoutMs for a new frame and returns the most recent one (older queued frames are discarded
    // and counted in droppedBefore). On Ok the caller must call ReleaseFrame() before the next AcquireFrame().
    virtual CaptureStatus AcquireFrame(uint32_t timeoutMs, CapturedFrame& out) = 0;
    virtual void ReleaseFrame() = 0;

    // Short human-readable name of the capture method, e.g. L"Windows Graphics Capture".
    virtual const wchar_t* MethodName() const = 0;
};

// Windows Graphics Capture source for a top-level window. Returns null and fills `error` on failure
// (unsupported OS build, window not capturable, ...).
std::unique_ptr<ICaptureSource> CreateWgcCapture(GpuDevice& device, HWND target, const CaptureOptions& options,
                                                 std::wstring& error);

// Top-level windows that are reasonable capture targets: visible, not minimized, not cloaked, not tool windows,
// non-empty title, not owned by this process. Callable from any thread.
std::vector<WindowInfo> EnumerateCaptureWindows();

} // namespace moonsr
