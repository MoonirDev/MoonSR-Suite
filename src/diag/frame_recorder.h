// Diagnostic frame recorder. Header owned by the lead.
//
// Saves the exact images the engine presents (and the captured input frames they came from) as lossless PNG files,
// so that output quality and frame generation can be inspected offline. The output window is excluded from screen
// capture, so this is the only way to see what was really shown.
//
// Recording uses a blocking GPU readback per frame. That disturbs frame pacing while it runs: recorded IMAGES are
// exact, recorded TIMING is not representative. Never active unless explicitly requested.
//
// Threading: Begin / Capture / Active / Poll are called on the engine pipeline thread (owner of the immediate
// context). PNG encoding and file writes happen on an internal worker thread.
#pragma once

#include "core/types.h"
#include "gpu/gpu_device.h"

#include <memory>
#include <string>

namespace moonsr {

enum class RecordedKind : uint32_t {
    Input = 0,  // captured game frame (source resolution), before any processing
    Real,       // presented output derived from a real frame
    Generated,  // presented output that is an interpolated frame
};

struct RecordedFrameInfo {
    RecordedKind kind = RecordedKind::Real;
    float phase = 1.0f;       // interpolation phase t for Generated frames (0..1); 1 for Real
    int64_t dueQpc = 0;       // scheduled present time (0 if not scheduled)
    int64_t actualQpc = 0;    // time the frame was handed to the recorder (just before Present)
    uint64_t realIndex = 0;   // index of the real frame this output belongs to (generated frames lie between realIndex-1 and realIndex)
};

class FrameRecorder {
public:
    static std::unique_ptr<FrameRecorder> Create(GpuDevice& device, std::wstring& error);
    // Blocks until queued frames are written, then stops the worker thread.
    virtual ~FrameRecorder() = default;

    // Starts a recording of `outputFrames` presented frames (clamped to 1..240) into `directory` (created if
    // missing). Returns false if a recording is already active or the directory cannot be created.
    virtual bool Begin(const std::wstring& directory, uint32_t outputFrames, const std::wstring& description) = 0;

    // True from Begin until the requested number of output frames has been captured.
    virtual bool Active() const = 0;

    // Copies `region` of `texture` out of the GPU (blocking) and queues it for PNG encoding. Input frames do not
    // count toward the requested number of output frames. No-op when not Active(). `region` empty = whole texture.
    // Accepts BGRA8 UNORM (typed or typeless / sRGB) single-sample textures.
    // File names: out_0001_real.png, out_0002_gen_t0.50.png, in_0001.png (in_ numbered by realIndex).
    virtual void Capture(ID3D11Texture2D* texture, const RectI& region, const RecordedFrameInfo& info) = 0;

    // Returns true exactly once, after a recording has finished and every file (including index.csv, one row per
    // saved image: file, kind, phase, realIndex, dueQpc, actualQpc, ms since recording start, width, height) has
    // been written. `directory` receives the folder.
    virtual bool PollFinished(std::wstring& directory) = 0;
};

} // namespace moonsr
