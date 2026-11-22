// Cursor module contract. Header owned by the lead. Implementation: gpu-rendering-engineer.
//
// The game window is small; its image is shown scaled on the output monitor. The real system cursor stays over
// the game window (so clicks land correctly), which is the wrong place visually. CursorOverlay draws a software
// copy of the current cursor at the mapped position on the output and hides the system cursor while it is over
// the game's client area.
//
// Threading: created, used and destroyed on the engine pipeline thread (per-monitor-v2 DPI aware, COM MTA).
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

namespace moonsr {

struct CursorState {
    bool visible = false;   // the cursor is showing AND its hotspot is inside the target's client area
    POINT screenPos{};      // hotspot position in physical screen pixels
    HCURSOR shape = nullptr;
    bool operator==(const CursorState& o) const {
        return visible == o.visible && screenPos.x == o.screenPos.x && screenPos.y == o.screenPos.y && shape == o.shape;
    }
};

class CursorOverlay {
public:
    static std::unique_ptr<CursorOverlay> Create(GpuDevice& device, std::wstring& error);
    // The destructor must restore the system cursor if this object hid it.
    virtual ~CursorOverlay() = default;

    // Cheap poll (no GPU work, no allocation): current cursor state relative to `target`'s client area.
    // The engine calls this every loop iteration to decide whether the output must be redrawn.
    virtual CursorState Poll(HWND target) = 0;

    // Draws the cursor described by `state` into `rtv`. The target's client area maps linearly onto `destRect`
    // (in render-target pixels); the cursor image is scaled by the same factor (destRect size / client size),
    // with the hotspot landing on the mapped position. Does nothing when !state.visible.
    // Handles colour (alpha), masked-colour and monochrome cursors, including XOR/invert pixels.
    // Returns false only on a D3D failure (see log). Leaves no render target or shader resource bound.
    virtual bool Draw(const CursorState& state, HWND target, ID3D11RenderTargetView* rtv, Size targetSize,
                      const RectI& destRect) = 0;

    // Hides or shows the SYSTEM cursor (process-independent, e.g. via the Magnification API). Idempotent.
    // The engine hides it while state.visible is true and shows it otherwise.
    virtual void SetSystemCursorHidden(bool hidden) = 0;
};

} // namespace moonsr
