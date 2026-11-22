# MoonSR Neural Rendering bridge

MoonSR loads `MoonSR_NrBridge.dll` from the same folder as `MoonSR.exe` when Neural Rendering is enabled.
The bridge owns vendor runtime loading, motion guides where supported, shared-resource synchronization, model evaluation,
and the OptiScaler composition pass. MoonSR sends the image at the selected position in the load order, plus
an original Windows Graphics Capture image as the optical-flow guide.

The C ABI is [moonsr_nr_bridge.h](../sdk/include/moonsr_nr_bridge.h), version 4. The `MoonSR_NrBridge` target builds
the bridge and `NeuralRendering_nvngx.dll` from the supplied `ls-addon-manager` source; the MoonSR target
builds both beside the executable. The separate `MoonSR_NrBridgeTemplate` is an ABI example only.

The Neural rendering page selects the imported runtime file independently of its network hint (default,
Original A, or Fast B). It also sets 1–20 passes, 25–100% working resolution, style, model intensity, local
structure and tone, automatic skin detection and skin strength, and detail and colour transfer. Settings can
be edited during scaling. MoonSR applies them between frame schedules. Changing the runtime, network, number
of passes, or working resolution rebuilds model resources. Fast is a hint and older runtimes may ignore it.
Later passes have independent model histories and use zero local tone, matching the reference multipass behavior.

Picture adjustments apply after neural composition at the selected neural stage. They use the reference Picture
pass: exposure, contrast, midtones, shadows, highlights, saturation, vibrance, temperature, tint and sharpening.
The controls update on the next captured frame without rebuilding the model. Disabling Picture keeps its values;
Reset picture restores neutral values without changing the enable switch or model settings.

The bridge uses the reference pipeline's NVIDIA Optical Flow implementation, model calls, shared D3D11/D3D12
fences, and pinned OptiScaler composition shader. It is an experimental community implementation, not an
NVIDIA-supported integration. Its first supported capture format is SDR BGRA8. When neural rendering follows frame
generation, it processes both generated and real displayed frames; the guide remains a resized original capture.
The same guide can be used for more than one output frame, so optical flow between those repeated guides can be zero;
motion quality in this configuration needs further validation with the user's runtime. The source and license
notices are in `ls-addon-manager/addons/NeuralRendering/NOTICE.md` and that directory's `licenses/` folder.

## Required bridge behavior

- Export `MoonSR_NrBridge_GetApi` for ABI version 4.
- Optionally export `MoonSR_NrBridge_UpdateSettings` to accept live settings on the processing thread. MoonSR
  recreates bridges that only expose the base ABI when their settings change.
- Accept MoonSR's D3D11 device and immediate context at creation.
- Process the supplied stage input, which may be upscaled or generated, and use `flowGuide` for motion.
- Return an enhanced texture from the supplied D3D11 device. If using motion history, use the original-capture
  guide and reset history when the `reset` field is nonzero.
- Keep a returned texture alive until the next `Process` call or `Destroy`.
- Return a specific error string on initialization or processing failure. If Neural Rendering fails after a
  live change, MoonSR reports the error, bypasses the neural stage, and continues scaling.

MoonSR uses a model supplied by the user. For RTX 20, 30, and 40 series, select the patched FP16
`nvngx_dlssnr.dll`. If the file is beside `MoonSR.exe`, the Neural rendering page offers it under
**Local model file** for review and import. The bridge uses the imported model and the installed NVIDIA NGX core.

## AMDNR

The AMD path uses the lmxxf C ABI and character-mask mapping from the supplied AMDNR OptiScaler source.
MoonSR calls the runtime on its own Direct3D 12 queue on the selected processing GPU. It converts the SDR
capture to linear colour, queues HIP inference, and composes the result for the same captured frame.
The bridge uses AMDNR's colour-only frame path on Radeon RX 7000, RX 9000, and the RDNA 3-class APUs
supported by that runtime. RX 6000 support requires a different runtime.
The model supports up to three passes. Original selects the full network. The style and local-tone controls
are available on NVIDIA. The remaining supported controls and picture adjustments work during scaling.

Install the matching `LmxxfNrRuntime.dll` and `LmxxfNrRuntime.pak` from
[`AMDNR-v0.3.5.6.zip`](https://github.com/3zwr1/AMD-NR---OptiScaler/releases/tag/Alpha0.3.5.6) in `amd-nr` beside
`MoonSR.exe`. Keep any runtime DLL dependencies from that release beside the runtime and follow its GPU
and HIP driver requirements. Choose the Radeon processing GPU in **Output**, then enable **Neural rendering**.
The AMD backend takes its model from the AMDNR pack. The MoonSR **Active model file** picker serves NVIDIA.

With a supported Radeon GPU, run `MoonSR_NrBridgeSmoke.exe --amd "path\to\LmxxfNrRuntime.dll"`
to exercise frame processing, live resolution changes, and picture adjustment. The pack must be beside that DLL.

## Intel (DLSS5_INTEL)

On an Intel processing GPU the bridge drives the DLSS5_INTEL runtime, `d5c_chain.dll` from
[github.com/geladons/DLSS5_INTEL](https://github.com/geladons/DLSS5_INTEL) (Apache-2.0, reference commit `7e8e039`).
That runtime runs the DLSS NR network on Vulkan cooperative matrices (`VK_KHR_cooperative_matrix`, XMX on Arc).
MoonSR bundles none of it: build `dlss5/chain` yourself (it needs the Vulkan SDK) or take `d5c_chain.dll` and its
`.spv` shaders from that project's release bundle, and supply your own weights.

Install into an `intel-nr` folder beside `MoonSR.exe`:

- `d5c_chain.dll`
- its compute shaders (`gemm.spv` and the rest of the `.spv` files)
- `dlssnr-logical.safetensors`: the weights in that project's logical layout, extracted from your own
  `nvngx_dlssnr.dll` (see its `docs/weights-format.md`; NVIDIA's data, never redistributed)

Then choose the Intel GPU as the processing GPU in **Output** and enable **Neural rendering**. The bridge
(`addons/moonsr_nr_bridge/intel_model.cpp`) works like this:

- **One pass, one network.** Working resolution maps to the runtime's scale (33-100 %) and changes live.
- **Settings.** Intensity, local tone, local structure, skin, auto mask, style and colour strength go to the
  runtime's Phase 30 controls. Picture adjustments run afterwards in the bridge's D3D11 picture pass.
- **CPU data path.** The runtime's C ABI takes CPU buffers, so each frame is read back, processed and uploaded
  again. This is the only CPU readback in MoonSR's frame path. The call blocks the pipeline thread for the whole
  processing time.
- **Adapter selection.** `dlss5_create` cannot be given an adapter and prefers any discrete GPU. While the backend
  is active the bridge therefore sets `VK_LOADER_DRIVERS_SELECT=*igvk*`, so the Vulkan loader only offers Intel
  drivers. `MOONSR_INTEL_NR_DRIVERS` overrides the pattern; the previous value is restored afterwards.
- **Speed.** DLSS5_INTEL's own measurements: about 210 ms per frame at 960x576 and 550-770 ms at 1920x1088 on an
  Arc Pro B50. Use a low working resolution. MoonSR has not measured it (no Intel GPU on the development machine).

`MoonSR_IntelNrSmoke.exe` tests the bridge path with a stand-in runtime (`MoonSR_IntelNrFixture.dll`) on any GPU.
