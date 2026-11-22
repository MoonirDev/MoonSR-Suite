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
//    available across GPUs (valid = false); MoonSR then uses a motion generator on the main device instead.
//  - RemoteNeuralBridge: each processed frame and its optical-flow guide go to the remote GPU, the result comes back.
//  - HostedNeuralBridge (NVIDIA neural rendering on a GPU other than the output monitor's): the real bridge runs in a
//    helper process, MoonSR_NrHost.exe. NVIDIA's NGX creates features only on the adapter of the OLDEST live D3D11 or
//    D3D12 device of a process: with an older device alive on another GPU, CreateFeature returns FAIL_PlatformError
//    (0xBAD00002) in both directions; once that device is released it works again (measured 2026-11-21 with the
//    bridge smoke, RTX 5060 + RTX A400, driver 621.14; MoonSR_NrBridgeSmoke --first-device reproduces it). In
//    MoonSR.exe the oldest device is the Direct2D UI's, on the monitor's GPU, so NR on any other GPU (main GPU or
//    not) runs where its device is the first one: in the helper. On another GPU than the main one, frames cross to
//    it as before (CrossAdapterLink, in this process); then they reach the helper through same-adapter NT-handle
//    textures ordered by shared D3D11 fences, GPU-side waits only. One named-pipe round trip per frame carries the
//    frame parameters; it returns once the helper has submitted its work, not when the GPU finishes it. If the
//    helper dies or hangs, GPU waits for it are released from the CPU and the stage reports an error.
//  - WithHostedFallback: the engine's choice of in-process NVIDIA NR (the monitor's GPU) is a guess about which
//    device is the process's oldest (the UI window may be on another GPU's monitor). If the first feature creation is
//    refused with 0xBAD00002 anyway, the stage moves to the helper once and the frame is processed again there.
//
// Threading: pipeline thread only, like the stages they wrap. RunNeuralHost is the helper process's main thread.
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

// Starts MoonSR_NrHost.exe (beside the running executable), which runs the bridge for `modelPath` on `remote`'s GPU.
// `remote` may be `main` (no cross-adapter copies then). Both devices must outlive the returned bridge.
std::unique_ptr<NeuralBridge> CreateHostedNeuralBridge(GpuDevice& main, GpuDevice& remote, const std::wstring& modelPath,
                                                      const NeuralSettings& settings, std::wstring& error);

// Wraps an in-process NVIDIA bridge (`inProcess`: local on `nr`, or a RemoteNeuralBridge to it) so that a refused
// first feature creation falls back to CreateHostedNeuralBridge(main, nr, ...). Null in, null out.
std::unique_ptr<NeuralBridge> WithHostedFallback(GpuDevice& main, GpuDevice& nr, std::unique_ptr<NeuralBridge> inProcess,
                                                const std::wstring& modelPath, const NeuralSettings& settings);

// Entry point of MoonSR_NrHost.exe: serves one HostedNeuralBridge over the pipe named on the command line.
int RunNeuralHost(int argc, wchar_t** argv);

} // namespace moonsr
