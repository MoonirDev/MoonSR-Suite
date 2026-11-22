# DLSS on captured frames: Neural Rendering ("DLSS 5") first, Super Resolution second

- Author: graphics-researcher (MoonSR Suite)
- Date: 2026-11-15
- Assignment: find out, from primary sources, what MoonSR can legitimately and technically do to integrate "DLSS 5" Neural Rendering on captured frames. The priority was changed mid-task: Neural Rendering (NR) is the main subject, DLSS Super Resolution (SR) is secondary. Also study how FriedSnowman/ls-addon-manager does it, and list the features of that project worth adopting.
- Method: I read the source files directly from `raw.githubusercontent.com` and the GitHub API. I read NVIDIA's public DLSS repository (LICENSE.txt, include/ headers, commit history) and Streamline (headers, changelog, docs), plus NVIDIA GeForce and developer pages and the NVIDIA developer forum. Nothing was downloaded into the project, and no binaries were fetched.
- Evidence tags:
  - **[V]** verified from the cited source, which I read during this session
  - **[V2]** secondary source (press or a third-party project), read but not confirmed by NVIDIA
  - **[I]** my inference
  - **[U]** unknown or not publicly documented
- Correction to `documentation/research/nvidia.md` §1.7 and §0.1. That file says nothing in the DLSS family "is designed to run on an arbitrary captured window". That is literally true, but it is too strong as a feasibility statement. DLSS SR **does run** on captured frames through the public NGX API when it is fed self-measured motion, flat depth and zero jitter (ls-addon-manager ships this, §A2). The quality is limited, and the reasons are documented below. I have not edited that file, because it belongs to an earlier assignment. The lead should note the correction there.

---

## 0. Decision memo

### 0.1 DLSS 5 Neural Rendering on captures: is there a documented, licensed route today? **No.**

- **Only public integration route: Streamline's `sl.dlss_nr` plugin.**
  - NVIDIA's public Streamline 2.14 changelog announces the plugin [V, S10].
  - `include/sl_core_types.h` defines `kFeatureDLSS_NR = 1004` [V, S11].
  - The repository has **no `sl_dlss_nr.h` header and no NR programming guide**. The docs folder has guides for DLSS, DLSS-G, DLSS-RR, DeepDVC, DirectSR, NIS, PCL and Reflex only [V, S11].
  - A developer posted on 2026-10-30 that the public Streamline 2.14.16 and 2.14.1 packages contain neither the NR plugin, nor an NR header or guide, nor `nvngx_dlssnr`. No NVIDIA reply is visible [V2, S13]. I did not download the release zips to confirm this [U].
  - NVIDIA's DLSS developer page does not mention DLSS 5 at all. It stops at DLSS 4.5 [V, S14].
  - Several forum requests for DLSS 5 SDK access from custom engines, D3D11 apps and tools have 0 replies [V2, S13].
- **The public NGX SDK does not expose NR either.**
  - DLSS SDK 310.9.16 (commit of 2026-10-16) ships `nvngx_dlss`, `nvngx_dlssd` (RR) and `nvngx_dlssg` (FG). It ships **no `nvngx_dlssnr`** [V, S1].
  - In `nvsdk_ngx_defs.h`, feature id 18 is `NVSDK_NGX_Feature_Reserved18` [V, S2]. No public header contains the string "DLSSNR" or "neural" (I checked 4 headers, 0 matches each) [V, S2].
- **What NVIDIA officially says DLSS 5 is** [V, S15, S16, S17]:
  - "3D-Guided Neural Rendering", a final stage of the engine pipeline.
  - It takes the frame's colour and motion vectors.
  - It was trained to recognise albedo, lighting and normals.
  - It runs on RTX 50, RTX Spark and GeForce NOW Ultimate only.
  - Driver 616.64. Integration goes through Streamline or the UE5 plugin.
  - Shipping titles: NBA 2K27 (2026-09-01), Hogwarts Legacy (2026-11-05 game update), and RTX Remix (2026-10-23).
  - A lighter model with about 118 M parameters and half the GPU cost arrived on 2026-10-16.
  - There is no NVIDIA statement that it works on games without an integration, or through an NVIDIA app override.
- **What the undocumented route used by ls-addon-manager DLSSNR02 actually consists of** [V, §A1]:
  1. **The model DLL comes from somewhere else.** It is a user-supplied `nvngx_dlssnr.dll` that "never" ships with the project and whose source the project will not name. Press says the file surfaced in NBA 2K27 PC builds and is described as "leaked" [V2, S20, S21]. Modified, patched and "unsigned" builds also circulate [V2, S21, S22].
  2. **The DLL is called directly.** The NGX entry points are loaded straight out of the snippet DLL (`NVSDK_NGX_D3D12_Init_Ext`, `CreateFeature`, `EvaluateFeature`...). The driver's `_nvngx.dll` is located through the registry key `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath`.
  3. **The application id is not NVIDIA-assigned.** It is a self-chosen id, `0x24480451`, described as "OptiScaler's own generic application id". The public header says the plain Init needs an "ID provided by NVIDIA", and that otherwise `Init_with_ProjectID` with engine type CUSTOM should be used [V, S2].
  4. **Undocumented definitions.** It uses feature id `18` and parameter names `DLSSNR.*` that appear in no NVIDIA header. They come from OptiScaler_DLSSNR (GPL-3.0), whose forwarder code is copied in modified form.
  5. **Reverse-engineered parameter ABI.** It probes the parameter block's vtable to find which slot actually holds the float setter.
  6. **Caller check worked around.** The snippet accepts calls only from a module whose path contains `nvngx.dll`. The addon works around this by putting its calls into a DLL named `NeuralRendering_nvngx.dll`. **[I] This is, on its face, a workaround of a technical restriction in NVIDIA's code.** The NVIDIA RTX SDK licence forbids bypassing "any technical limitation" of the SDK [V, S3 §4d]. It also forbids reverse engineering [V, S3 §4a].
  7. **Placeholder inputs.** Depth is a constant 1.0, motion comes from NVOFA, and exposure, jitter and masks are not supplied.
- **Legal risk of that route.**
  - **[I]** No licence I could find grants anyone the right to use `nvngx_dlssnr.dll` outside the game it shipped with. The RTX SDK licence lets *developers* redistribute SDK files "as incorporated in object code format" in their own application [V, S3 §1c]. A DLL taken from a game is not a file NVIDIA delivered to *us* under any licence. ls-addon-manager itself says the DLL "is not a file NVIDIA offers for redistribution by this project" [V, S7].
  - **[I]** Whatever licence covers the DLL, the vtable probing and the caller-check workaround are hard to reconcile with the anti-reverse-engineering clause (§4a) and the anti-circumvention clause (§4d).
  - **[U]** Whether the end user's EULA for the game or driver binds the user's use of the file. I found no licence text for `nvngx_dlssnr.dll` itself.
- **Stability risk** [V for what the project documents, I for the rest]:
  - The parameter names, feature id, preset meanings, float setter slot and caller check are all undocumented. Any model or driver update can change them.
  - ls-addon-manager pins a single host build and pauses on mismatch. It validated two DLL versions in **synthetic tests only**: "Gameplay image quality and performance across hardware and drivers remain unvalidated" [V, S6].
  - Officially the model supports only RTX 50, RTX Spark and GeForce NOW [V, S16, S17]. Runs on RTX 40 rely on patched DLLs [V2, S22].
- **What would have to become true for a clean integration:**
  1. NVIDIA publishes an `sl.dlss_nr` header, a guide and a runtime, under terms that allow use in a non-game tool.
  2. The guide documents NR's input contract, and that contract can be met without engine depth, G-buffer or masks. Alternatively, NVIDIA blesses captured-frame use explicitly.
  3. The runtime is redistributable or OTA-delivered to apps that identify themselves through `ProjectID` and engine type CUSTOM (or an NVIDIA-assigned id), on the D3D12 API (D3D11 support is [U]).
  4. MoonSR notifies NVIDIA before any commercial release, as the supplement requires [V, S3 suppl. §4].

  Until then, the clean path is to **apply for access** (developer forum or NVIDIA developer relations) and to **wait**.

### 0.2 A "bring your own model file" NR addon in MoonSR: what it would need, and why I advise against building it now

The coordinator asked for a precise description of such an addon. **It is a lead and project-owner decision.** My recommendation is not to ship it: it would only work through items 3 to 6 of the undocumented route, and none of those can be done clean-room without the same reverse-engineered facts. Technically it would need all of the following:

1. **A new D3D12 side device** on the same adapter as our D3D11 device (match by LUID; NVIDIA adapters only).
   - Shared textures: create `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED` textures on our D3D11 device, then `CreateSharedHandle` → `ID3D12Device::OpenSharedHandle`.
   - Two shared fences (D3D11 `CreateFence(D3D11_FENCE_FLAG_SHARED)` → D3D12 `OpenSharedHandle`) for "capture ready" and "capture consumed". One D3D12 queue fence is opened back on D3D11 for "NR done". That is the pattern in `addons/NeuralRendering/src/pipeline.cpp` [V, S8]. It is plain documented D3D11.4 and D3D12 API usage, which we may reimplement.
   - Keep three slots in flight. A D3D12 HIGH-priority queue is optional.
2. **Placement in our pipeline: on real captured frames only, before frame generation and before or instead of upscaling.**
   - Our FG then interpolates the enhanced frames. That mirrors DLSSNR02's fix for the retired DLSS5NR01, which enhanced at Present and carried an older edit onto newer frames [V, S5].
   - Cost: the GPU wait adds latency per real frame. DLSSNR02 says plainly that it "is not a zero-latency path" [V, S5].
3. **Motion vectors.** Our flow is per-cell displacement in frame pixels with a confidence value. It would have to become a per-pixel RG16F texture at the NR working resolution.
   - DLSSNR02 passes current→previous NVOFA vectors in working-resolution pixels with MV scale 1 [V, S6].
   - That NR wants this exact sign and unit is **[U] undocumented**. The project chose it to match OptiScaler, which forwards a game's DLSS vectors.
   - Our confidence has no documented NR input. DLSSNR02 passes no mask [V, S9].
4. **Placeholder depth** (R32F, constant 1.0, DLSSNR02's choice [V, S9]) and a `Reset` on scene cuts.
5. **Process isolation.** Run the D3D12 device, the model and NGX in a **separate helper process** that shares textures and fences across processes (NT handles can be duplicated into another process). A model, driver or ABI change then crashes or hangs only the helper. The main app falls back to unenhanced frames with a visible status.
   - [I] DLSSNR02 is in-process and pauses capture on failure. ls-addon-manager added worker threads with timeouts because "a stuck runtime" could freeze the host [V, S12].
6. **Licence hygiene.**
   - Nothing bundled or downloaded. Off by default. An NVIDIA-only check.
   - The user must explicitly select the file.
   - **No GPL code**: no OptiScaler forwarder, no `dlssnr.hlsl` composition.
   - **[I]** The unavoidable core facts (feature 18, the `DLSSNR.*` keys, the vtable slot, the caller-name workaround) are knowledge obtained by reverse engineering an NVIDIA binary, and the caller-name workaround is a circumvention. A clean-room rewrite does not remove that problem, because the problem is the act, not the copyright of the code. **This is the reason I recommend against it.** If the owner wants it anyway, get a legal opinion first and keep it out of any official MoonSR distribution.

### 0.3 DLSS Super Resolution / DLAA on captures (secondary): **feasible and licensable, with quality limits**

- **API.**
  - The public SDK supports **D3D11** (`NVSDK_NGX_D3D11_Init`, `NGX_D3D11_CREATE_DLSS_EXT` and `NGX_D3D11_EVALUATE_DLSS_EXT` in `nvsdk_ngx_helpers_d3d.h`) and D3D12 [V, S2].
  - Running DLSS on Lossless Scaling's own D3D11 device "crashed it", so ls-addon-manager uses its own D3D12 device [V, S4]. [I] That crash is specific to injecting into the host. In our own process a D3D11 NGX path on our device may work. A D3D12 side device with the bridge in §0.2(1) is the proven fallback.
- **Inputs** (ls-addon-manager's working set [V, S4, S8]):
  - Colour at render size.
  - Per-pixel RG16F motion vectors at render resolution: `MVLowRes`, MV scale = our unit factor, "where each pixel was in the frame before" (current→previous, in pixels).
  - Flat R32F depth of 0.5.
  - Jitter 0,0.
  - `Pre_Exposure` 1, with no AutoExposure flag.
  - `IsHDR` for linear scRGB.
  - `Reset` on cuts and resizes.
  - `Sharpness` 0 (sharpening is done afterwards with CAS).
  - Our confidence fed in, inverted, as `DLSS_Input_Bias_Current_Color_Mask` (which, per ls-addon-manager, transformer presets ignore).
  - Our flow would need upsampling to per-pixel resolution, plus a per-pixel refinement (they pick the best of zero, own-block and three-neighbour vectors per pixel).
- **Caveats.**
  - DLSS rebuilds detail from sub-pixel jitter. "A captured frame has no such shifts (and no depth)". The K and M presets can soften a still picture, while E stays crisper. HUD text comes out softer. Ghosting in fast motion needs a post-pass blend toward a plain upscale [V, S4, S5].
  - Streamline says jitter must be in pixel space and that matrices must not contain jitter [V, S12b]. I could not read NVIDIA's full description of what jitter is for, because it is in the PDF guide [U].
- **Legal.**
  - The RTX SDK licence allows shipping `nvngx_dlss.dll` and the NGX static library inside an application with "material additional functionality" [V, S3 §1c, §2a].
  - It requires NVIDIA marks and attribution on the splash screen, about box and credits [V, S3 suppl.].
  - It requires notifying NVIDIA before a commercial release [V, S3 suppl. §4].
  - It is NVIDIA-GPU-only [V, S3 suppl. §1].
  - The SDK must not be made "subject to an open source software license" [V, S3 §4e]. If MoonSR is ever released under a copyleft licence, the DLSS addon must remain a separate, non-GPL binary. Legal review needed [I].
  - Auto-downloading from NVIDIA's own GitHub after the user accepts the licence is what ls-addon-manager does [V, S4b]. [I] Bundling is the clearer case: the licence speaks of distribution inside the application, and says nothing about a third-party fetcher.
  - Application id: use `NVSDK_NGX_D3D11/D3D12_Init_with_ProjectID` with `NVSDK_NGX_ENGINE_TYPE_CUSTOM` and our own project id. The header says to use this when NVIDIA has not given you an id [V, S2]. **Do not copy ls-addon-manager's self-chosen ids `0x24480451` or `0x24480452`.**

### 0.4 Recommended scope for the first MoonSR "NVIDIA DLSS" addon

- **In scope:**
  - DLSS SR and DLAA via the public NGX SDK. Optional, NVIDIA RTX only, off by default.
  - Fed by MoonSR's own motion estimator, upsampled to per-pixel vectors.
  - Flat depth, zero jitter, `Reset` on cuts.
  - Preset choice: Default, E, K, L, M. The quality ratio is picked from the window/output ratio, with DLAA at 1:1.
  - Our own post-sharpen and "lean" pass.
  - HDR through `IsHDR` with linear scRGB.
  - Runtime path shown in the UI.
  - Bundled `nvngx_dlss.dll` with NVIDIA's licence text, attribution in About, and NVIDIA notified before any commercial release.
- **Explicitly out of scope:**
  - DLSS 5 / NR (until §0.1's conditions are met).
  - DLSS-G and MFG, which need engine buffers and the game's swapchain.
  - Ray Reconstruction.
  - Any code from OptiScaler or ls-addon-manager's GPL addon.
  - Self-chosen NGX application ids.
  - Loading NGX binaries the user took from games.

---

## A. How ls-addon-manager does it

Repository: https://github.com/FriedSnowman/ls-addon-manager (main, read 2026-11-15). File list obtained through the GitHub tree API [V, S4c].

### A1. DLSS Neural Rendering addon `DLSSNR02` (v1.2.2, GPL-3.0-only) — main subject

**Pipeline and placement** [V, S5, S6]
- The data flow is: `capture -> NVIDIA Optical Flow -> NR (1-10 passes) -> OptiScaler composition -> optional Picture -> LSFG -> presentation`.
- It processes only "original real captures before LSFG". It never touches generated frames and never reuses an older edit.
- It hooks a real-frame processing callback that the manager exposes in host API 1.4.
- It requires WGC capture (DXGI duplication is not supported), with LSFG 3 or FG off.
- It pins Lossless Scaling 3.2.2.0 by SHA-256 and pauses on any other build.
- "Every capture waits for NR". There is no fallback to originals under overload.
- It states plainly: "This is not a zero-latency path."

**Model loading and NGX calls** (`src/model.cpp`, `src/model_forwarder.cpp`) [V, S9]
- `_nvngx.dll` is found through `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` `FullPath`. Only `NVSDK_NGX_D3D12_AllocateParameters` and `DestroyParameters` are used from it. The core is deliberately not initialised, so as not to disturb the DLSS SR addon.
- The user's `nvngx_dlssnr.dll` is loaded and five exports are resolved from it: `NVSDK_NGX_D3D12_Init_Ext`, `NVSDK_NGX_D3D12_CreateFeature`, `NVSDK_NGX_D3D12_EvaluateFeature`, `NVSDK_NGX_D3D12_ReleaseFeature` and `NVSDK_NGX_D3D12_Shutdown1`. **D3D12 only.** The forwarder's notes say the Vulkan and D3D11 paths were removed from OptiScaler's version [V, S7].
- The init call is `init(0x24480451, dataPath, device, 0x0000015, capabilityParams)`. `0x15` is the public `NVSDK_NGX_VERSION_API_MACRO` [V, S2]. The comment calls the id "OptiScaler's own generic application id". An earlier version used `0x4350324B` ("CP2K"), which made every game announce itself to the driver as Cyberpunk 2077.
- The feature is created with `create(cmd, 18, params, &handle)`. **Feature 18 is `Reserved18` in the public header** [V, S2].
- Creation parameters:
  - `DLSSNR.Enabled`, `DLSSNR.Width`, `DLSSNR.Height`
  - `CreationNodeMask` and `VisibilityNodeMask` = 1
  - `DLSSNR.Hint.Render.Preset`: 0 default, 1 "Original (A)", 2 "Fast (B)", 3 legacy
  - `DLSSNR.Intensity` (0-2), `DLSSNR.Style` (0-2: Default, Natural, Cinematic)
  - `DLSSNR.LocalStructureStrength`, `DLSSNR.LocalToneStrength`, `DLSSNR.SkinStructureStrength`
  - `DLSSNR.UseAutoMask`, `DLSSNR.UICorrection`
- Evaluation parameters:
  - `DLSSNR.Color`, `DLSSNR.Depth`, `DLSSNR.MVec`, `DLSSNR.Output`
  - `DLSSNR.DepthInverted`, `DLSSNR.Reset`
  - Colour, output and depth sub-rectangles
  - The per-frame model controls again
  - MV scale 1,1
- The preset mapping (2 = the lighter network in 310.9.16.0+) is cited to mauri870/dlssnr-pytorch [V, S6]. That project obtained it by reading the DLL's bytes and extracting its weights [V2, S19].
- **Parameter ABI** [V, S9]:
  - The parameter block is the driver's own capability block, driven through its vtable. The order is assumed to be eight Set overloads followed by eight Get overloads.
  - The comments say the public header puts the float setter at slot 1, but this block does not keep floats there. The host finds the real slot by writing a value and reading it back (`DLSSNR.FloatRoundTrip` = 0.375).
  - Resources are written through slot 0, the 64-bit setter, because the typed D3D12 setter "left them unset".
- **Caller check** [V, S9]: the code comments say the snippet "requires that module's path to contain 'nvngx.dll'" and otherwise returns `FAIL_PlatformError`. All calls are therefore routed through the DLL `NeuralRendering_nvngx.dll`.

**Inputs and how each is produced** [V, S6, S8, S9]

| Input | What DLSSNR02 supplies |
|---|---|
| Colour | The real WGC capture, SDR Rec.709 or linear scRGB (80-nit unit, 203-nit reference white). HDR10/PQ is not supported. It is encoded to RGBA16F at the working resolution (a percentage of the capture size). |
| Motion | **NVIDIA Optical Flow (NVOFA) D3D12**, loaded from the driver's `nvofapi64.dll` through `NvOFAPICreateInstanceD3D12`. Settings: `NV_OF_MODE_OPTICALFLOW`, forward prediction, output cost enabled, grid 1, 2 or 4 (default 4), perf level slow, medium or fast (default medium). It compares "current original to previous accepted original". The S10.5 vectors are divided by 32 and scaled to working-resolution pixels, with MV scale 1. The cost output is diagnostic only and does not weaken the vectors. |
| Depth | Constant **1.0**, R32F, at working resolution: "a compatibility input, not estimated geometry". `DepthInverted` = 0. |
| Exposure, jitter, masks, engine MVs | Not supplied ("unavailable"). |
| Reset | Set on hard cuts (an 8-byte GPU summary of 1,024 samples, compared by histogram and colour change), on sequence gaps, resizes and reconfiguration. |

- **Composition:** OptiScaler_DLSSNR's `dlssnr.hlsl` is imported unchanged (GPL-3.0, with a RenoDX-derived MIT part). It does detail and colour transfer, a luminance-ratio bound, SDR and scRGB handling, and reconstruction at reduced working resolution [V, S7].
- **Multipass (2-10):** each pass has its own model instance and history, followed by one final composition. The project itself notes the setting is for memes [V, S6].
- **D3D11↔D3D12 sharing:** NT-handle shared textures and three shared fences, three slots in flight, and an optional HIGH-priority D3D12 queue [V, S8].
- **Cost and latency:** **no measurements are published for DLSSNR02.** Only the retired addon's 5-9 ms per frame (1912x1080 model input; 8.5 ms at 4K in WoW) is given, with the note "These timings do not describe the replacement" [V, S5].
- **GPUs:** "An NVIDIA GPU with compatible NVOFA and model capabilities" [V, S6]. No list is given.
- **Validated models** [V, S6]:
  - 310.8.0.0 (SHA-256 `e16bcf15...`) and 310.9.16.0 "(unsigned compatibility build)" (`cdfa6c1e...`).
  - The README does not explain the phrase "unsigned compatibility build". **[U]** where that build comes from. [V2] Press and third-party projects describe a 310.8.0.0 DLL found in NBA 2K27 PC files, a 310.9.16.0 extracted after an NBA 2K27 update, and modified builds ("310.8.Lecram", ShortFuse ".SF" repacks, RTX 40-"patched" builds) [S20, S21, S22]. [I] "Unsigned" most likely means a modified copy without NVIDIA's Authenticode signature. Not verified.
- **Stated caveats:** "Gameplay image quality and performance across hardware and drivers remain unvalidated". Only synthetic tests were run, plus a byte-for-byte comparison on an RTX 5080. The cut detector "can miss similar-histogram or gradual cuts" [V, S6].
- **Why DLSS5NR01 was retired** [V, S5]:
  - It evaluated asynchronously and warped the newest result difference onto real and generated frames at Present. As a result:
    - an edit could belong to an older frame (trailing, misplaced detail on newly visible areas and on cuts);
    - LSFG analysed the frame before the edit;
    - capture and output coordinates could disagree;
    - its own HDR compositor needed fixes (neon orange and cyan highlights).
  - Its features included looks per game, user-drawn HUD-exclusion rectangles, split view and auto-quality.
- **DISCLAIMER and NOTICE on the DLL.** NOTICE says the DLL "belongs to NVIDIA and is not a file NVIDIA offers for redistribution by this project … this project does not say where to find it" [V, S7]. The DISCLAIMER says use is at your own risk and that the project is unaffiliated [V, S7b].

### A2. DLSS Upscaler addon `DLSS4DLAA` (MIT) — secondary

- **Runtime:** NVIDIA's `nvngx_dlss.dll` 310.9.1, bundled together with NVIDIA's licence. `tools/fetch_ngx_sdk.ps1 -AcceptNvidiaLicense` fetches the headers, `nvsdk_ngx_s.lib`, the DLL and LICENSE.txt from NVIDIA/DLSS at a pinned commit, each checked against a SHA-256 [V, S4b].
- **NGX calls** (`src/engine/sr_engine.cpp`) [V, S4]:
  - `NVSDK_NGX_D3D12_Init(0x24480452, dataPath, dev, &info)`, with the runtime directory passed in `PathListInfo` and an NGX log callback.
  - Capability check through `SuperSampling_Available` and `NeedsUpdatedDriver`.
  - `CreateFeature(NVSDK_NGX_Feature_SuperSampling)`.
  - Quality value from the ratio: up to 1.01 DLAA, up to 1.55 MaxQuality, up to 1.75 Balanced, up to 2.2 MaxPerf, otherwise UltraPerformance.
  - Flags `MVLowRes | (hdr ? IsHDR : 0)`. No AutoExposure, MVJittered or DepthInverted.
  - All six `Hint_Render_Preset_*` keys are set to the chosen preset.
  - Evaluate: `Color`, `Output`, `Depth` (flat **0.5**), `MotionVectors` (RG16F at input size), `Bias_Current_Color_Mask` = distrust mask, `Jitter_Offset` 0,0, `MV_Scale` = estimator scale, `Reset` on cut or first frame, `Sharpness` 0, `Pre_Exposure` 1, render subrect = full input.
- **Motion** (`flow_estimator.cpp`, the project's own MIT code) [V, S4d]:
  - A luminance pyramid down to about 64 px wide.
  - Coarse to fine: 4x4 blocks with an 8x8 match window, seeded from the level below plus a penalty for straying from the seed.
  - Sub-pixel refinement at half size, then a 3x3 vector median.
  - A per-pixel choice among zero, the pixel's own block and the three nearest blocks, by 3x3 match quality.
  - This also produces a "distrust" mask.
  - Sign: "where each pixel was in the frame before".
  - Cost: about 0.35 ms at 1440p and about 0.7 ms at 4K.
- **Options** [V, S4d]:
  - *Lean from*: from a motion threshold (0.05% of the width per frame when upscaling, 0.1% at 1:1), the result is blended toward a Catmull-Rom upscale of the current frame, weighted by the distrust mask. The reason given is that transformer presets "take no mask".
  - *Stability*: the motion estimator judges trust from a pixel's neighbourhood, so flicker is averaged out. Thin moving lines are kept out of it.
  - *Edge smoothing*: the project's own FXAA-style pass after the upscaler.
  - Sharpening with the CAS formula.
- **Sharing:** the NIS pass in Lossless Scaling's D3D11 device writes into a shared texture and signals a shared fence. The addon's D3D12 device waits on the GPU. One frame of latency [V, S4d].
- **HDR:** scRGB and HDR10 are converted to linear light (1 = SDR white) and run with `IsHDR` [V, S4d].
- **Cost:** about 2.4 ms for 1440p→4K and about 3.2-4.2 ms at 4K 1:1 on an unnamed RTX GPU, as reported by the project [V, S4d; GPU model not stated].
- **Caveats:** no jitter and no depth, so no sub-pixel detail. K and M soften stills while E stays crisp. Thin HUD text is softer than with NIS [V, S4d].

### A3. Licences of everything we might be tempted to reuse

| Component | Licence (quote, source) | May MoonSR copy it? |
|---|---|---|
| Manager, `addons/Upscalers`, `tools/` | MIT, "Copyright (c) 2025 FrankBarretta … 2026 Echo-Storm" [V, S7] | Yes, with the MIT notice kept, after a recorded licence check (CLAUDE.md). Ideas are free in any case. |
| `addons/NeuralRendering` (all of it) | "GPL-3.0-only" [V, S7] | **No** (our licence is undecided, so GPL code is treated as not copyable). Facts and ideas only. |
| OptiScaler and OptiScaler_DLSSNR (`dlssnr.hlsl`, forwarder) | GPL-3.0 (GitHub licence metadata) [V, S18] | **No.** |
| NVOFA headers `nvOpticalFlowCommon.h` and `nvOpticalFlowD3D12.h` | MIT-style notice, "Copyright (c) 2018-2023 NVIDIA Corporation", which "applies to this header file only" [V, S8b] | Yes, with the notice kept. The NVOFA runtime is part of the driver. The NvOF SDK as a whole is under the DesignWorks licence (see nvidia.md §8). |
| video2dlssnr (D3D12 NVOFA reference) | MIT [V, S18] | Yes, with the notice, but it exists to feed the NR DLL, so it is irrelevant to the clean path. |
| dlssnr-pytorch (reimplementation from extracted weights) | MIT [V, S18] | Not relevant. Its method (extracting NVIDIA's weights) is exactly what we must not depend on. |
| NVIDIA NGX headers, static library, `nvngx_dlss.dll` | NVIDIA RTX SDKs License [V, S3] | Use under NVIDIA's terms only: object-code distribution inside the app, no reverse engineering, no open-source licensing of the SDK. |
| `nvngx_dlssnr.dll` | No licence text found [U] | **No.** Not ours to ship. Use is unlicensed as far as I can find. |

---

## B. NVIDIA's official position

### B1. What the public SDKs expose (as of 2026-11-15)
- **NVIDIA/DLSS** [V, S1, S2]:
  - The latest commit is "DLSS 310.9.16 SDK" (2026-10-16).
  - Runtimes: `nvngx_dlss` (SR and DLAA), `nvngx_dlssd` (RR) and `nvngx_dlssg` (FG), each as dev and rel builds.
  - Guides: `DLSS_Programming_Guide_Release.pdf`, `DLSS-RR Integration Guide.pdf` and `DLSS-FG Programming Guide.pdf`.
  - Headers support D3D11, D3D12, Vulkan and CUDA for SR (`NGX_D3D11_CREATE_DLSS_EXT` exists).
  - SR flags: IsHDR, MVLowRes, MVJittered, DepthInverted, AutoExposure, AlphaUpscaling.
  - **No NR runtime, header or feature name.**
- **Streamline** [V, S10, S11]:
  - Version 2.14.16 (2026-10-16). Its changelog says "Added support for DLSS 3D-Guided Neural Rendering with a new plugin: `sl.dlss_nr`".
  - `kFeatureDLSS_NR = 1004` is defined.
  - There is no NR header or guide in the repository.
- **The documented API allows the inputs a capture can supply, for SR** [V, S12b]:
  - Motion vectors in pixel space, scaled through mvecScale.
  - Jitter in pixel space (it can be zero; zero is a value, not a violation).
  - Missing exposure means auto-exposure.
  - [U] Whether constant depth is "permitted" is not addressed in the markdown guide.
- **NR's input contract: [U].** NVIDIA's pages name colour, motion vectors and engine material and lighting understanding [V, S16]. RTX Remix gets "per-object preset selection and content masking" [V, S17].

### B2. Application identification [V, S2]
- `NVSDK_NGX_Init` "requires an application ID provided by NVIDIA". If none is available, use `NVSDK_NGX_Init_with_ProjectID`.
- `ProjectIdDescription` should be used by engine plugins, "unless using NVSDK_NGX_ENGINE_TYPE_CUSTOM". The header adds: "If your NVIDIA contact did not provide you an ID … use ProjectDesc with NVSDK_NGX_ENGINE_TYPE_CUSTOM".
- **MoonSR: `Init_with_ProjectID` with a stable MoonSR GUID and engine type CUSTOM.**

### B3. Licence: NVIDIA RTX SDKs License plus RTX Supplement [V, S3]
- **Grant:** distribute SDK material "as incorporated in object code format into a software application" (§1c). The application "must have material additional functionality" (§2a).
- **Restrictions:**
  - "You may not reverse engineer, decompile or disassemble" (§4a).
  - No stand-alone distribution (§4b).
  - "You may not bypass, disable, or circumvent any technical limitation" (§4d).
  - Must not become "subject to an open source software license" (§4e).
- **Supplement:**
  - "licensed for you to develop applications only for their use in systems with NVIDIA GPUs" (§1).
  - Notify NVIDIA before commercial release, "including a plug-in to a commercial application" (§4).
  - NVIDIA may push OTA updates (§6.1) and may "temporarily disable" a problematic DLSS integration (§6.2).
  - NVIDIA marks are required on the splash screen, the about box and in credits (Exhibit, (b)).
- **Dev and rel DLLs:** both ship in the repository [V, S1]. [U] The watermark and on-screen debug behaviour of the dev DLLs is in the PDF guide, which I could not read. [I] Ship only the `rel` build.
- **No clause found** about tools that process *other* applications' output. [I] Nothing forbids it; MoonSR would be the "application".
- **No clause found** about using binaries taken from other products. [I] The grant covers the SDK "made available to you under this license". A DLL lifted from a game is not that.

### B4. DLSS 5 officially, and `nvngx_dlssnr.dll`
- **Facts** [V, S15, S16, S17]:
  - Announced at GTC on 2026-03-16 and shipped on 2026-09-01 in NBA 2K27.
  - RTX 50, RTX Spark and GeForce NOW Ultimate only. Driver 616.64.
  - Integration through Streamline or the UE5 plugin. The model updates (2026-10-16, about 118 M parameters, half the cost) arrived with the game and driver releases.
  - Hogwarts Legacy got it in a game update on 2026-11-05. RTX Remix got it on 2026-10-23.
  - Players switch it on in-game ("DLSS Neural Rendering").
- **Distribution of `nvngx_dlssnr.dll`:**
  - Not in the public SDK [V, S1].
  - [V2] Press says it was found in NBA 2K27 PC build files, about 158 MB, version 310.8.0.0, and later 310.9.16.0 after a game update [S20, S21]. Guru3D and OC3D returned 403, so I have only the search summaries.
  - [U] Whether the driver or the NVIDIA app delivers it by OTA.
  - [U] Any licence permitting its use in another application. None was found.

---

## C. ls-addon-manager features worth adopting

"MoonSR has it?" reflects my reading of the project brief and CLAUDE.md, not a code audit. The lead should verify.

| Feature (source S5 or S12) | What it does there | MoonSR |
|---|---|---|
| Per-game settings | Saves settings by the game's exe name and restores them when the game takes focus. Only the scaled window counts. | [U] Not known to exist. Worth adopting. |
| Before/after hotkey and paired PNG screenshots | Switches between the addon and the default scaler while playing. Ctrl+Shift+F4 saves two PNGs of the same moment. | Probably partly (the upscaling on/off switch exists, per commit 1409101). Paired screenshots: no. |
| "Apply model result" comparison mode | Keeps processing running but shows the original, so the cost and pacing stay identical in the comparison. | No. Good idea for fair A/B. |
| Runtimes list | Lists every vendor runtime: present or not, loaded or not, version, signed or not. **+** swaps the file live. | No. Useful once optional vendor addons exist. |
| Backup and restore of settings | Saves all settings to one file. Setup also backs up every file it replaces. | [U] |
| Diagnostics zip | Logs plus settings, built locally, uploaded nowhere. | [U] |
| Bug recorder (`.lsrec`) | Keeps the last seconds of input frames in a ring buffer and saves them for offline replay. | No. Very valuable for our own regression tests. |
| FPS counter placement | Corner choice for the counter. | We have an fps overlay (commit db5bd89). Placement: [U]. |
| Live status line and expanded error footer | One line normally, with reason and recovery action first on errors. Details and Copy buttons. | [U] |
| Performance tab | Per-addon GPU cost, GPU load, power, clocks, temperature, VRAM, plus a plain-words limiter ("at power limit"). | [U] |
| HDR handling | Automatic SDR, scRGB or HDR10 detection with a manual override. Works in linear light, 1 = SDR white. | [U] Must be designed. |
| Interface size 75-200% | DPI-aware UI scale. | [U] (D2D UI exists, per commit c85cfe1.) |
| Tray icon and hotkey to show the UI | Hides to the tray. Ctrl+Shift+F12. | [U] |
| Opt-in update check | Asks on first run, checks GitHub's public API once per start, opens the browser to download. | [U] |
| Windowed / virtual display mode | A virtual display sized to the game window, for windowed games and second monitors. | N/A. MoonSR captures windows directly. |
| ReShade input passthrough | Forwards mouse and keyboard to a ReShade overlay. | N/A until we have overlays. |
| Addon install from folder, zip or DLL; Remove moves it aside | Install by drag and drop. Nothing is deleted. | Planned addon framework. Adopt "remove = move aside". |
| Callback fault isolation and atomic settings writes | Addon faults are caught. Settings are written atomically. | Should be a requirement of our addon host. |
| Vendor calls on a worker thread with a timeout | A stuck runtime cannot freeze the host. Falls back to the default scaler. | Must adopt for any vendor addon. Better still: process isolation (§0.2). |
| Scene-cut detection resets history | A GPU histogram summary triggers `Reset`. | Relevant to our temporal SR. [U] whether it exists. |
| Distrust mask and "lean" toward a plain upscale | Hides missing-jitter ghosting. | Concept useful for our own temporal SR (clean-room). |

---

## D. Open questions
1. Does the Streamline 2.14.16 release zip contain `sl.dlss_nr` binaries, a header or a guide? I did not download it, because downloads require permission.
2. What does the DLSS programming guide PDF (310.9.16) say about jitter's role, MV sign conventions, dev-DLL watermarks, and constant or absent depth? The PDF was not read.
3. Is there an NVIDIA access programme for the DLSS 5 SDK, under what terms, and does it allow non-game tools or D3D11? No public answer exists. Forum requests have no replies.
4. Do the driver or the NVIDIA app deliver `nvngx_dlssnr.dll` OTA to integrated games?
5. Does NGX SR on our own D3D11 device work in-process, or does it hit the same crash ls-addon-manager saw inside Lossless Scaling?

## E. Claims I could not verify
- That 310.9.16.0 `nvngx_dlssnr.dll` is "unsigned", where that build comes from, and what "compatibility build" means.
- The NBA 2K27 leak details (Guru3D and OC3D returned 403; search summaries only).
- ls-addon-manager's reported costs (2.4 ms and others, on an unnamed RTX GPU) and its PSNR figures. They are self-reported.
- The forum claim that the public Streamline zips lack the NR plugin.
- Whether NR accepts D3D11 or Vulkan.
- The NR motion-vector sign and unit contract, and whether constant depth is acceptable to the model. The community choices are undocumented by NVIDIA.

---

## Sources
- S1. NVIDIA/DLSS repository tree and commits. https://github.com/NVIDIA/DLSS (API: https://api.github.com/repos/NVIDIA/DLSS/git/trees/main?recursive=1, /commits)
- S2. NGX headers. https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx.h , .../nvsdk_ngx_defs.h , .../nvsdk_ngx_helpers.h , .../nvsdk_ngx_helpers_d3d.h
- S3. NVIDIA RTX SDKs License plus RTX Supplement. https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt
- S4. ls-addon-manager `addons/Upscalers/src/engine/sr_engine.cpp`. https://raw.githubusercontent.com/FriedSnowman/ls-addon-manager/main/addons/Upscalers/src/engine/sr_engine.cpp ; S4b `tools/fetch_ngx_sdk.ps1` and `addons/Upscalers/external/ngx/README.md` ; S4c tree: https://api.github.com/repos/FriedSnowman/ls-addon-manager/git/trees/main?recursive=1 ; S4d `addons/Upscalers/docs/upscalers.md`
- S5. ls-addon-manager README.md. https://raw.githubusercontent.com/FriedSnowman/ls-addon-manager/main/README.md
- S6. `addons/NeuralRendering/README.md` (same base URL)
- S7. `NOTICE.md`, `addons/NeuralRendering/NOTICE.md`, `LICENSE`, `addons/NeuralRendering/LICENSE`, `addons/Upscalers/LICENSE` ; S7b `DISCLAIMER.md`
- S8. `addons/NeuralRendering/src/pipeline.cpp`, `src/gpu.cpp`, `src/optical_flow.cpp` ; S8b `third_party/nvof/nvOpticalFlowCommon.h`
- S9. `addons/NeuralRendering/src/model.cpp`, `src/model_forwarder.cpp`, `shaders/initialize_guides.hlsl`
- S10. Streamline changelog. https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/changelog.txt
- S11. Streamline tree and `include/sl_core_types.h`. https://github.com/NVIDIA-RTX/Streamline ; releases: https://api.github.com/repos/NVIDIA-RTX/Streamline/releases
- S12. ls-addon-manager CHANGELOG.md (same base URL) ; S12b Streamline DLSS guide: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
- S13. NVIDIA developer forum, "Official DLSS 5 Neural Rendering SDK access for a custom D3D12 online game" (2026-10-30) and related topics list. https://forums.developer.nvidia.com/t/official-dlss-5-neural-rendering-sdk-access-for-a-custom-d3d12-online-game/387382
- S14. NVIDIA DLSS developer page. https://developer.nvidia.com/rtx/dlss
- S15. NVIDIA, DLSS 5 announcement (2026-03-16). https://nvidianews.nvidia.com/news/nvidia-dlss-5-delivers-ai-powered-breakthrough-in-visual-fidelity-for-games (via nvidia.md S36)
- S16. NVIDIA, "DLSS 5: Neural Rendering Brings Lifelike Lighting to NBA 2K27" (2026-09-01). https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/
- S17. NVIDIA, "DLSS 5 Comes To Hogwarts Legacy & RTX Spark, Powered By 2X Faster Model" (2026-10-13). https://www.nvidia.com/en-us/geforce/news/dlss-5-faster-ai-model-hogwarts-legacy-rtx-spark-laptops/ ; NVIDIA developer blog on RTX Spark: https://developer.nvidia.com/blog/nvidia-rtx-spark-powers-neural-rendering-and-ai-characters-in-games/
- S18. GitHub repository metadata (licences): https://api.github.com/repos/Dagherbou/OptiScaler_DLSSNR , https://api.github.com/repos/optiscaler/OptiScaler , https://api.github.com/repos/DaniilSokolyuk/video2dlssnr , https://api.github.com/repos/mauri870/dlssnr-pytorch
- S19. mauri870/dlssnr-pytorch README. https://github.com/mauri870/dlssnr-pytorch
- S20. Guru3D, "first DLSS 5 runtime discovered in NBA 2K27 PC build" (403; search summary only). https://guru3d.com/story/first-dlss-5-runtime-discovered-in-nba-2k27-pc-build/ ; OC3D (403): https://overclock3d.net/news/software/nvidia-dlss-5-neural-rendering-dll-files-leak-launch-incoming/
- S21. Noobfeed, "DLSS 5 Mod Unlocks 40% FPS Boost With Leaked NVIDIA DLL" (2026-10-17). https://www.noobfeed.com/hardware/dlss-5-mod-40-fps-boost-leaked-nvidia-dll
- S22. Search-result summaries on modified and RTX 40-patched DLSSNR builds (not opened): https://huggingface.co/Bandukids/DLSS-Runtimes , https://pbxscience.com/?p=22582 , https://newreleases.io/project/github/faisalkindi/DLSS5oneclick/release/v0.10.8
