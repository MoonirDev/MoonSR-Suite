// Owner: lead
// Pipeline stages that run on a different GPU from the engine's main device.
//
// The main device (the Upscaling stage's GPU) captures, upscales and presents. When the user puts Frame generation
// or Neural rendering on another GPU, the engine creates the stage through these wrappers. They implement the same
// interfaces as the local stage, run the real stage on the other GPU's device, and move frames across with
// CrossAdapterLink (src/gpu/cross_adapter.h), so the frame loop does not change:
//
//  - RemoteFrameGenerator: PushFrame copies the frame to the remote GPU (one copy per real frame). The real frames it
//    hands back (RealFrame) are kept as local copies on the main device, so they never cross back. Each Interpolate
//    copies the generated frame back to the main device (one copy per generated frame). MotionField() is not
//    available across GPUs (valid = false); Moon ALSR then uses a motion generator on the main device instead.
//  - RemoteNeuralBridge: each processed frame and its optical-flow guide go to the remote GPU, the result comes back.
//
// Threading: pipeline thread only, like the stages they wrap.
#pragma once

#include "framegen/frame_generator.h"
#include "neural/neural_bridge.h"

#include <memory>
#include <string>

namespace moonsr {

class GpuDevice;

// Both devices must outlive the generator.
std::unique_ptr<FrameGenerator> CreateRemoteFrameGenerator(GpuDevice& main, GpuDevice& remote, std::wstring& error);

// `inner` was created on `remote`. Both devices must outlive the returned bridge.
std::unique_ptr<NeuralBridge> CreateRemoteNeuralBridge(GpuDevice& main, GpuDevice& remote,
                                                      std::unique_ptr<NeuralBridge> inner, std::wstring& error);

} // namespace moonsr
