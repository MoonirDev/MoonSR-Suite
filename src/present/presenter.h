// Presentation module contract. Header owned by the lead. Implementation: gpu-rendering-engineer.
//
// A Presenter owns the borderless output window covering one monitor and its flip-model swapchain.
//
// Threading: the Presenter is created, used and destroyed on one thread (the engine pipeline thread). The output
// window belongs to that thread, so that thread must call PumpMessages() regularly (at least once per frame).
// Z-order tracking (keepAbove) runs inside PumpMessages() on that same thread: out-of-context WinEvent hooks
// installed by Create() are delivered through that thread's message queue, plus a low-rate poll as a safety net.
//
// Z-order: by default the output window is an ordinary (non-topmost) borderless popup that keeps itself directly
// above `keepAbove` (the captured game window). Other applications' windows can be placed over it, alt-tab works,
// and clicking through the output activates the game, after which the output restacks itself above the game again.
// While `keepAbove` is minimised, hidden or cloaked (another virtual desktop), the output window is hidden.
// The output window never takes activation.
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

namespace moonsr {

struct PresentOptions {
    HMONITOR monitor = nullptr;     // output monitor; the window covers its full desktop rect
    bool vsync = false;             // true: sync interval 1. false: interval 0, with tearing if supported
    bool clickThrough = true;       // mouse input passes through to the windows underneath
    bool excludeFromCapture = false; // screenshots can capture final output; input WGC targets the game HWND
    // Top-level window the output stays directly above in z-order (normally the captured game window). Child
    // windows are resolved to their root. Null: the output is placed once at the top of the non-topmost band and
    // not tracked.
    HWND keepAbove = nullptr;
    // true: legacy behaviour, a WS_EX_TOPMOST window above everything; keepAbove is then ignored.
    bool topmost = false;
};

enum class PresentStatus {
    Ok,
    Occluded,   // presented nothing because the window is occluded; keep running
    DeviceLost, // device removed/reset; the presenter is dead
    Error,      // unexpected failure, see log; the presenter is dead
};

class Presenter {
public:
    static std::unique_ptr<Presenter> Create(GpuDevice& device, const PresentOptions& options, std::wstring& error);
    virtual ~Presenter() = default;

    // Size of the back buffer == physical pixel size of the monitor.
    virtual Size BackBufferSize() const = 0;
    virtual DXGI_FORMAT BackBufferFormat() const = 0;

    // Render target view of the current back buffer. Valid until Present().
    virtual ID3D11RenderTargetView* BackBufferRtv() = 0;

    // Presents the back buffer.
    virtual PresentStatus Present() = 0;
    // DXGI-reported display timing, when available. May be unavailable for some swapchain modes.
    virtual bool FrameStatistics(DXGI_FRAME_STATISTICS& statistics) const = 0;

    // Waitable handle signalled when the swapchain can accept a new frame (frame latency waitable object), or
    // null if unavailable. The pipeline may wait on it to avoid queueing frames.
    virtual HANDLE FrameLatencyWaitable() const = 0;

    // Dispatches pending window messages for the output window. Returns false once the window was closed or the
    // monitor configuration changed in a way that requires the presenter to be recreated.
    virtual bool PumpMessages() = 0;

    virtual HWND Window() const = 0;
    virtual void SetVsync(bool vsync) = 0;
};

} // namespace moonsr
