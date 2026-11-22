// Internal helper shared by the gpu, capture and present modules (not a lead contract).
//
// Switches the calling thread to per-monitor-v2 DPI awareness for the lifetime of the object so that window and
// monitor coordinates are physical pixels regardless of the process manifest. Restores the previous context.
// Threading: affects only the calling thread; construct and destroy on the same thread.
#pragma once

#include <windows.h>

namespace moonsr {

class ScopedPerMonitorDpi {
public:
    ScopedPerMonitorDpi() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~ScopedPerMonitorDpi() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    ScopedPerMonitorDpi(const ScopedPerMonitorDpi&) = delete;
    ScopedPerMonitorDpi& operator=(const ScopedPerMonitorDpi&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

} // namespace moonsr
