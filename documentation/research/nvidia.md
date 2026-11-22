# NVIDIA upscaling, frame generation and latency technology: research notes

- Author: graphics-researcher (MoonSR Suite)
- Date: 2026-11-13
- Method: live web search and fetches of primary sources (NVIDIA GeForce news, NVIDIA ADLR research pages, NVIDIA developer docs, GitHub repos and license files), plus secondary technical press where NVIDIA is silent. Web tools were available for this assignment.
- Evidence tags used throughout:
  - **[V]** verified from the cited source (fetched during this session)
  - **[V2]** reported by a secondary source (press or review). The source was read, but the claim was not confirmed by NVIDIA
  - **[I]** inferred by me from verified facts. Not a sourced claim
  - **[U]** unknown or not publicly documented
- Source numbers such as [S3] refer to the source list at the end.
- Limitation: I could not read the DLSS SR programming guide PDF (`doc/DLSS_Programming_Guide_Release.pdf`, 84 pages, dated 2026-03-10). It is compressed, and no PDF text extractor was available. I used the Streamline per-feature guides instead, which describe the same inputs. HotHardware and Guru3D returned HTTP 403, and I could not reach a Digital Foundry primary article. Claims that depend on those sources are marked [V2] or [U].

---

## 0. Executive answer for MoonSR

1. **Nothing in the DLSS family (SR, DLAA, RR, FG, MFG, Dynamic MFG, DLSS 5) is designed to run on an arbitrary captured window.** Every one of them needs engine data: motion vectors and depth at minimum, plus jitter for SR, a HUD-less color buffer, a UI buffer and camera matrices for FG, and G-buffer channels for RR. They are also licensed only for applications running on NVIDIA GPUs [V, S5, S7, S8, S9]. They cannot be MoonSR's core path, which must be cross-vendor and work from the final image only.
2. **NVIDIA Image Scaling (NIS) is the only NVIDIA component that is both capture-compatible and permissively licensed.** It is MIT-licensed (copyright 2022 NVIDIA), works on a single frame, and ships as HLSL/GLSL compute shaders that are not tied to a vendor [V, S10]. It can be used as a baseline or fallback, with the MIT notice retained, or simply as a quality reference to beat.
3. **NVIDIA has two products that prove the capture-style approach works: Smooth Motion and the Optical Flow SDK's NvOFFRUC.** Smooth Motion is driver-level interpolation without game data, on RTX 40 and newer [V, S14, S15]. NvOFFRUC interpolates from two frames with no application-supplied motion vectors, on Turing and newer, using the DX11 or CUDA API [V, S17]. Both are NVIDIA-only. Smooth Motion has no public API. NvOFFRUC is redistributable only inside an application, under the DesignWorks SDK License [V, S19]. At most, NvOFFRUC could be an optional NVIDIA backend for motion estimation.
4. **The most transferable lessons are architectural, and all of them can be applied clean-room:**
   - Split frame generation into a heavy per-pair stage and a light per-output-frame stage.
   - Generate every intermediate frame directly from the original pair at its own timestep.
   - Always present real frames. Generated frames may be dropped.
   - Run dedicated, high-precision pacing.
   - Choose the multiplier from the gap between base fps and the display refresh rate.
   - Make UI and HUD handling an explicit, first-class problem.

   See section 10.

---

## 1. DLSS Super Resolution and DLAA

### 1.1 DLSS 1.0 (2018-2019)
- **Architecture:**
  - [V] NVIDIA states that the original DLSS "required training the AI network for each new game" and that it "tended to struggle in areas with objects in motion" [S1].
  - [V2] It was mostly a spatial upscaler, trained against 64x-supersampled ground-truth images, and it ran on Tensor Cores [S2, S3].
- **Inputs:** [V2] Mostly the current low-resolution frame. The secondary sources disagree on how much motion data it used [S2, S3]. [U] NVIDIA never fully documented the inputs.
- **"DLSS 1.9" (Control, 2019):** [V] NVIDIA says the earlier Control implementation "used an image processing algorithm to boost frame rates" [S1]. [V2] Wiki-style sources call it a shader-core variant and label it "1.9". That name is unofficial [S2].
- **Hardware:** RTX 20 series (Turing Tensor Cores). [V] DLSS 2.0 "eliminates previous limitations on which GPUs, settings, and resolutions could be enabled" [S1].
- **Quality:** [V2] It was widely criticised as blurry, for example in Battlefield V [S2].
- **Licensing:** Proprietary and integrated per game.
- **Applicability:** None directly. **Lesson:** a single-frame network trained per game did not generalise well and looked soft. NVIDIA's own fix was temporal accumulation driven by motion vectors.

### 1.2 DLSS 2.x (CNN, March 2020 onward; versions 2.0 to 3.x SR)
- **Architecture:** [V] NVIDIA describes it as "a convolutional autoencoder". It takes the current low-resolution frame plus the previous high-resolution output reprojected with motion vectors ("temporal feedback"). It is trained against "16K reference" images on content that is not specific to any game, so one network works across games [S1].
- **Inputs:**
  - Required, from the Streamline DLSS guide [V, S6]: depth (`kBufferTypeDepth`), motion vectors (`kBufferTypeMvec`), render-resolution color, and output color.
  - Jitter offsets are passed separately in pixel space, through constants. They must not be baked into the matrices.
  - Exposure (`kBufferTypeExposure`) is optional. Without it, DLSS falls back to auto-exposure.
- **Modes:** [V] Quality, Balanced and Performance. Performance mode is "up to 4X super resolution (i.e. 1080p to 4K)". Overall, NVIDIA claims quality "comparable to native resolution while rendering only one quarter to one half of the pixels" [S1]. Ultra Performance and DLAA came later [S11].
- **Performance:** [V] All figures are NVIDIA's own:
  - The network is "2X faster than the original".
  - Control in Quality mode was "up to 76%" faster.
  - At 4K in Performance mode the gain was "2-3X" [S1].
  - [U] Independent per-GPU millisecond costs were in the PDF guide I could not parse.
- **Presets:** [V] In the current Streamline guide, presets A to D are "no longer available", E and F are legacy, and G to I are reserved. The examples show K, L and M [S6].
- **Quality:** [V2] Widely regarded as the reference temporal upscaler of the CNN era. Its typical weaknesses were ghosting on objects without motion vectors (particles, transparencies) and softness in motion. This is general press consensus. I did not fetch a specific review.

### 1.3 DLSS 4 Super Resolution and DLAA: transformer, "Model/Preset K" (January 2025)
- **Architecture:**
  - [V] A vision transformer. NVIDIA calls it "an industry first, real-time vision transformer model".
  - For the RR variant it has "four times the computations and twice the number of parameters" of the CNN, "in a similar frame budget". The SR model "uses double the parameters of the CNN model".
  - NVIDIA co-designed it with custom CUDA kernels, fused layers, and FP8 training and inference on Ada and Blackwell [S4, S11].
- **Inputs:** Same as the CNN SR model: color, depth, motion vectors, jitter, and optional exposure [V, S6].
- **Hardware:** [V] All GeForce RTX GPUs, through the NVIDIA app override or games integrated with the newer DLL [S11]. [I] Turing and Ampere run it without FP8, at higher cost (consistent with S12).
- **Quality:** [V] NVIDIA reports better detail preservation, less ghosting and better temporal stability [S4]. [V2] Reviewers broadly confirmed better motion clarity than the CNN models.
- **Licensing:** NVIDIA RTX SDKs License (see 1.6).

### 1.4 DLSS 4.5 Super Resolution: second-generation transformer, Models/Presets M and L (January 2026)
- **Architecture:** [V] It has "five times the compute power of the original transformer model". It was trained on an expanded dataset. It trains and infers in **linear** color space instead of logarithmic space, which NVIDIA says avoids muted lighting and crushed shadows. FP8 Tensor Cores on RTX 40 and 50 double inference throughput [S12].
- **Presets:** [V] Preset M is optimised for Performance mode. Preset L is optimised for 4K Ultra Performance. Preset K (the DLSS 4.0 transformer) remains the default for the other modes. "Recommended" replaced "Latest" in the NVIDIA app override [S12].
- **Hardware:** [V] All RTX GPUs. RTX 20 and 30 lack native FP8, so "Models M and L carry a heavier performance cost", and NVIDIA suggests Model K for those cards [S12, S13]. [V2] RTX 40 can lose "as much as 15%" render frame rate going from K to L [S24].
- **Quality claim:** [V] NVIDIA says Performance mode is "comparable to, and can even beat, native" [S12]. This is a vendor claim.
- **Availability:** [V] Through the NVIDIA app since January 2026, in "over 400" games and apps [S12].

### 1.5 DLAA
[V] DLAA is the same SR network run with the input at output resolution. It has its own preset slot (`dlaaPreset`) [S6], and the NVIDIA app can force it [S11]. The inputs are the same as SR.

### 1.6 Licensing (DLSS SDK, applies to SR, DLAA, RR and the NGX runtime)
[V] The DLSS repository license file is the **"NVIDIA RTX SDKs LICENSE"** plus the **"NVIDIA RTX Supplement"** [S5]. Key terms:
- Binaries may be redistributed in object code form inside an application that adds "material functionality". Stand-alone distribution of the SDK is not allowed.
- Reverse engineering, decompiling and circumvention of protections are forbidden. The SDK may not be used in a way that subjects it to an open-source license.
- **The DLSS, NGX and RTX Dynamic Vibrance SDKs "are licensed for you to develop applications only for their use in systems with NVIDIA GPUs".**
- The developer must notify NVIDIA before commercial release. NVIDIA may disable a problematic integration and may push over-the-air updates.
- NVIDIA marks are required on the splash screen, about box and credits.

### 1.7 Applicability to MoonSR (SR and DLAA)
- **Legal:**
  - [I] An NVIDIA-only optional backend in a commercial MoonSR build appears permissible, subject to attribution, notifying NVIDIA, and not being open-sourced alongside the SDK.
  - It cannot serve AMD or Intel GPUs, and it is not "original".
  - [I] If MoonSR is distributed as open source, the clause against open-source licensing needs legal review before any DLSS binary is bundled.
- **Technical:**
  - [I] A captured frame has no jitter, no depth and no engine motion vectors. Its HUD and post-processing are already applied, and it is usually already anti-aliased (often by the game's own TAA or DLSS).
  - DLSS relies on sub-pixel jitter to accumulate detail over time. With no jitter, the temporal accumulation has nothing to resolve. Motion vectors estimated by optical flow on a final, tone-mapped image would mislabel the HUD and transparencies.
  - Depth would have to be faked as a constant.
  - The expected result is close to a temporal-filtered spatial upscale with ghosting risk around the HUD. **Not recommended.** [U] No public test of DLSS SR on captured frames exists that I could find.

---

## 2. DLSS Ray Reconstruction

- **Versions:**
  - [V2] DLSS 3.5 (2023) was the CNN version.
  - [V] DLSS 4 (January 2025) is the transformer version, with 4x the compute and 2x the parameters of the CNN, FP8, and targeting Ada and Blackwell Tensor Cores [S4].
  - [V2] A "DLSS 4.5 Ray Reconstruction" for all RTX GPUs was reported through a driver update [S25]. The page returned 403, so the details are [U].
- **Architecture:** [V] One network that replaces the hand-tuned denoisers and the upscaler for ray-traced effects. It is a transformer since DLSS 4 [S4].
- **Required inputs:** [V, S7]
  - Diffuse albedo
  - Specular albedo
  - Normals
  - Linear roughness
  - Noisy HDR ray-traced color
  - Motion vectors
  - Depth
  - Either specular motion vectors, or specular hit distance plus camera matrices
  - Optional: transparency layer, color before transparency, SSS guide and DoF guide
  - HDR input is mandatory.
- **Hardware:** All RTX GPUs [V, S11].
- **Quality:** [V] NVIDIA reports better surface detail and fewer ghosting and disocclusion artifacts [S4]. [V2] Reviewers reported smearing in some CNN-era titles. I did not fetch those reviews.
- **License:** NVIDIA RTX SDKs License [S5].
- **Applicability:** **None.** RR needs a full G-buffer and noisy pre-denoise lighting, which a capture never has. The game-integration bridge could supply these only if it hooked deep into the renderer. That is out of scope.

---

## 3. DLSS Frame Generation (DLSS 3 FG, then the DLSS 4 FG model)

### 3.1 DLSS 3 FG (October 2022, RTX 40)
- **Architecture:** [V] A network that takes four inputs: "current and prior game frames, an optical flow field generated by Ada's Optical Flow Accelerator, and game engine data such as motion vectors and depth" [S9]. [V] Optical flow captures things engine motion vectors miss ("particles, reflections, shadows, and lighting"). The network combines both sources per pixel [S9].
- **Temporal handling:** Interpolation. One generated frame between two rendered frames, which requires holding back the newer frame [V, S9]. [V] DLSS 3 pacing ran on the CPU [S11].
- **Cost:** [V] About 3.25 ms per 4K frame on an RTX 4090 [S4].
- **Latency:** [V] NVIDIA claims DLSS 3 "can reduce latency by up to 2X" versus native. That compares against native without SR or Reflex, so it is a marketing framing [S9]. [V2] TechSpot measured about 10 ms added by FG in Cyberpunk 2077 [S27].
- **Hardware:** [V] RTX 40, because it depends on the Ada OFA and 4th-generation Tensor Cores [S9].

### 3.2 DLSS 4 FG model (January 2025)
- [V] The hardware OFA was replaced by "a very efficient AI model". The new model is "40% faster, uses 30% less VRAM, and only needs to run once per rendered frame" [S11].
- [V] **The network is split in half. One half runs once per input frame pair, and its output is reused. The smaller half runs once per generated frame** [S4]. This is the single most transferable architectural fact in this document.
- [V] Per-frame cost is "around 1ms on average on a GeForce RTX 5090" at 4K, compared with about 3.25 ms for DLSS 3 on a 4090 [S4].
- [V] Inputs still include motion vectors, depth and masks. NVIDIA notes these "can be inaccurate for specular highlights, reflections, UI", so the network "learns when to trust them". NVIDIA built its own training set because public interpolation datasets lacked gaming content and UI [S4].
- [V] The DLSS 4 FG model runs on RTX 40 and 50 [S11].

### 3.3 DLSS 4.5 "enhanced" FG model (Preset B, March 2026)
- [V] It uses "additional UI buffers in selected game engines" to handle static UI such as minimaps. It runs on RTX 40 and 50 and is selected as Frame Generation "Preset B" in the NVIDIA app [S13].
- [V2] PC Gamer could not see any improvement [S24].

### 3.4 Required inputs (Streamline DLSS-G guide) [V, S8]
- Backbuffer: intercepted by the Streamline swapchain proxy
- Depth
- Dense motion vectors (camera and object motion)
- **HUD-less color**
- **UI buffer** (`kBufferTypeUIAlpha` preferred, or `UIColorAndAlpha`)
- Camera matrices without jitter
- Optional distortion field
- Frame-indexed Reflex markers
- **Streamline Reflex integration is mandatory**
- Windows 10 20H1 or newer
- **HAGS must be enabled**
- VSync with FG requires independent flip and is D3D12-only
- [V] VRAM estimate: 272 MB at 1080p, 489 MB at 1440p, 725 MB at 4K. It is roughly the same for 2x and multi-frame [S8].

### 3.5 Licensing
- [V] The Streamline source carries an MIT-style permissive notice. Two NvPerf files fall under the Nsight Perf SDK License [S20].
- [I] The DLSS-G plugin binary and the NGX models fall under the NVIDIA RTX SDKs License (NVIDIA GPUs only) [S5]. [U] I did not find a DLSS-G-specific license file stating this explicitly.

### 3.6 Applicability
- **Not usable on a capture.** It needs depth, HUD-less color, a UI mask, camera matrices and Reflex integration inside the game. It is NVIDIA-only.
- [I] The optional bridge could in principle supply motion vectors and depth. Even then, DLSS-G must own the game's swapchain, not MoonSR's presenter, so it does not fit an external-window architecture.

---

## 4. DLSS Multi Frame Generation and Dynamic MFG

### 4.1 MFG (DLSS 4, January 2025, RTX 50 only)
- [V] Up to three generated frames per rendered frame (4x). "Five AI models" run per rendered frame across SR, RR and MFG [S11].
- [V] **Hardware Flip Metering:** frame pacing moved from the CPU (DLSS 3) to the Blackwell display engine, which has 2x the pixel processing. NVIDIA explains the move by noting that CPU pacing became less consistent as the frame count increased [S11, S4].
- [V] Upgrading from FG to MFG gives "up to 1.7X". NVIDIA reports multipliers over native of 4.7x to 8.2x across several titles (these include SR) [S4, S11].
- **Latency:** [V2] Digital Foundry's early RTX 5080 test, via TechSpot: the second generated frame added about 4 ms and the third about 2 ms. "The majority of the extra latency still comes from buffering that extra frame" [S26]. [V] NVIDIA's Avowed figures: 35 to 215 fps, with latency from 114 ms to 43 ms. That comparison is against native without Reflex [S4].

### 4.2 DLSS 4.5: 6X MFG and Dynamic MFG (announced CES 2026-01-06, released 2026-03-31, driver 595.97+)
- [V] **6X:** up to five generated frames per rendered frame. NVIDIA claims "up to 35%" higher frame rates over 4X at 4K path-traced. It is aimed at 240 Hz and 360 Hz displays and works only in titles with the newer FG DLL [S12, S13].
- [V] **Dynamic MFG** works like an "automatic transmission". It "continuously monitors the gap between GPU performance and the display's maximum refresh rate". It raises the multiplier in heavy scenes and lowers it as the load eases. The target is either "Max refresh rate" or a custom maximum fps [S13].
- [V] In Streamline:
  - `DLSSGMode::eDynamic` ignores `numFramesToGenerate`.
  - `dynamicTargetFrameRate = 0` auto-detects the monitor refresh rate.
  - It is designed to work with the Reflex frame limiter (`frameLimitUs`).
  - It requires D3D12 (Vulkan is not supported) and driver 595.41 or newer.
  - The runtime reports `numFramesToGenerateMax` [S8].
- [V] The consumer override says Dynamic mode "is not compatible with frame rate limiters or V-Sync" [S13]. [V2] Noobfeed reported a VSync/G-Sync interaction problem at 240 fps [S28].
- [V2] PC Gamer and DSOGaming describe multiplier switching as "practically instantaneous", with no major latency issues reported [S13 quoting press].
- [U] **NVIDIA has not published the controller logic.** Unknown items: hysteresis, how often the multiplier changes, whether it changes per frame or by averaging, the minimum base fps, and what happens if the base fps exceeds the refresh rate.
- [V] Hardware: RTX 50 only [S12, S13].

### 4.3 Pacing and present rules that are documented [V, S8]
- An asynchronous "SL pacer" schedules both real and generated frames.
- **Real frames are always presented. Interpolated frames can be dropped when presents fall out of sync.**
- The app must not use its own frame-latency waitable or `SetMaximumFrameLatency` unless it asked for them. It uses `slReflexSleep` instead.
- FG must be turned off around resizes and fullscreen transitions.
- NVIDIA recommends FrameView `MsBetweenDisplayChange` as the pacing metric.

### 4.4 Applicability
- The implementation is not usable: it is RTX 50 only and needs engine inputs.
- The **behavioural design is the most useful public reference for MoonSR's multi-frame and dynamic modes.** See section 10.

---

## 5. NVIDIA Image Scaling (NIS)

- **Version:** [V] v1.0.3 [S10].
- **Algorithm:** [V]
  - NVScaler combines "a 6-tap scaling filter with four directional scaling and adaptive sharpening filters" in one pass.
  - NVSharpen is adaptive-directional sharpening only. It must not be stacked on top of NVScaler.
  - Both run as compute shaders after tone mapping [S10].
- **Inputs:** One frame, after tone mapping. No motion vectors, depth or history. **Fully capture-compatible.**
- **APIs:** [V] DX11 and DX12 (HLSL), Vulkan (GLSL, or HLSL via DXC), and a Streamline plug-in. There are tuning targets for NVIDIA, AMD, Intel generic and NVIDIA fp16 [S10].
- **Hardware:** [V] Not stated. [I] It is a plain compute shader, so it should run on any DX11-class GPU, including GTX 10, RX 400 and Intel UHD. Verify on real hardware.
- **Driver variant:** [V2] The NVIDIA control panel exposes NIS as a driver-level scaler (GeForce GPUs). I did not fetch its documentation.
- **Quality:** [I] It is a spatial scaler and cannot add information. It competes with FSR 1 and Lanczos plus CAS. Expect sharpening halos at high strength and aliasing carried over from the input.
- **License:** [V] **MIT License, "Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES"** [S10].
- **Applicability:** **High.**
  - It may legally be shipped (with the MIT notice) as a baseline scaler on all vendors.
  - It is also a useful public quality and performance reference for MoonSR's own spatial stage.
  - [I] If the project requires every shipped algorithm to be original, use NIS only as an A/B benchmark and not as shipped code.

---

## 6. Reflex, Reflex 2 Frame Warp and related low-latency technology

- **Reflex Low Latency (2020):** [V] It aligns CPU work so that it finishes "just in time" for the GPU, "eliminating the GPU render queue and reducing CPU back pressure in GPU-bound scenarios". It uses markers to measure PC latency [S21]. [V] It is integrated through Streamline or the standalone Reflex SDK (Windows 10, NVAPI, C++) [S21]. [U] No license text was on that page, and supported GPUs were not listed.
- **Reflex 2 Frame Warp (announced January 2025):**
  - [V] Mechanism: the CPU computes the next camera pose from the latest input, and the finished frame is warped to that pose "as late as possible, just before the rendered frame is sent to the display". Disocclusion holes are inpainted using "camera, color and depth data from prior frames" [S22]. ADLR adds that "predictive rendering" reduces holes and AI inpainting fills the rest [S4].
  - [V] Latency claims: THE FINALS 56 ms to 27 ms (Reflex) to 14 ms (Frame Warp). VALORANT under 3 ms on an RTX 5090 [S22, S4].
  - **Status:** [V] NVIDIA's developer page still says "Sign up to be notified when Frame Warp is available for general integration" [S21]. [V2] As of late July 2026 there was no public release and no shipping game [S23].
- **Applicability:**
  - Reflex needs in-game markers, so it **cannot be applied from outside the game.**
  - [I] MoonSR can borrow two principles. First, keep its own capture-to-present queue at depth 1 and never let frames pile up. Second, do any final warping as late as possible.
  - Frame Warp needs a camera pose and depth from the engine. [I] Only the optional bridge could ever enable a frame-warp-style mode.
  - [I] A capture-only "late warp" using extrapolated optical flow is possible in principle, but it would predict motion rather than respond to input. It therefore reduces perceived judder, not input latency.

---

## 7. NVIDIA Smooth Motion (driver-level frame generation)

- **What it is:** [V] A "driver-based AI model that delivers smoother gameplay by inferring an additional frame between two rendered frames", for games "without DLSS Frame Generation support". It works with DX11, DX12 and Vulkan games. It can stack with native rendering, DLSS SR or other scalers [S14].
- **Hardware:** [V] It launched on RTX 50 and was extended to all RTX 40 GPUs (NVIDIA app plus driver; [V2] August 2025, driver 581.08) [S14, S29]. [V] On Linux it is available for RTX 40 and newer, Vulkan only [S15].
- **Implementation clue:** [V] On Linux it is an implicit Vulkan layer, `VK_LAYER_NV_present`, which "overrides the application's presentation to inject additional frames". It **presents from an asynchronous compute queue**, which can conflict with overlays. An option presents from the graphics queue instead "at some performance cost" [S15].
- **Multiplier:** [V] One generated frame (2x) per the official description [S14].
- **Inputs:** [V] It gets no game-supplied engine data: it works at the driver level, without integration. [U] The internal motion estimation (OFA, AI flow, or a hybrid) is not documented.
- **Quality and latency:** [V2] DSOGaming found it better than Lossless Scaling and AFMF in its tests. It reported about +10 ms latency and up to about 70-75% fps gains, worked only in profiled games, and crashed RTSS/Afterburner [S30]. One forum user reported heavy flicker when it was combined with DLSS FG. I treat that as anecdotal.
- **License:** Driver feature with no SDK or API. **Not available to third-party applications.**
- **Applicability:** It is the closest NVIDIA analogue to MoonSR's FG. It proves that two-frame AI interpolation without engine data is commercially viable at about 2x. It also shows a sensible present architecture: a present-layer injection running on async compute. MoonSR differs because it presents to its own window from captured frames, which adds capture latency that a driver layer avoids [I].

---

## 8. NVIDIA Optical Flow SDK and NvOFFRUC

- **Version:** [V] Optical Flow SDK 5.0 is the current docs version. SDK 3.0 added the DX12 interface, forward and backward flow, and global flow [S16, S17].
- **Hardware OFA:**
  - [V] Supported on "all Turing GPUs (except TU117) and above". Grid sizes: Turing 4x4 only, Ampere and Ada 1x1, 2x2 and 4x4. APIs: CUDA, DX11, DX12, Vulkan (with Vulkan limits on Turing) [S18].
  - [V] 1080p throughput at 4x4 grid ranges from 225 fps on Turing (SLOW preset) to 1296 fps on Ada (FAST preset). Ada has the lowest error rates [S18].
  - [U] **Blackwell OFA support is not mentioned in the application note.** [I] DLSS 4 FG dropped the OFA dependency, so do not assume the OFA will keep improving or even survive.
- **NvOFFRUC:**
  - [V] The application passes the current frame and a cached previous frame. **No motion vectors are supplied.** The library computes forward and backward flow internally.
  - It runs a forward-backward consistency check, rejects unreliable vectors, infills them, and fills the remaining holes in the image domain.
  - It interpolates at any timestamp the application chooses (for example 1.5).
  - **If the quality check fails, it returns the previous frame and sets `bHasFrameRepetitionOccurred`.**
  - Formats: ARGB and NV12. APIs: DX11 shared textures or CUDA. Windows driver 511.65 or newer [S17].
- **License:** [V] **NVIDIA DesignWorks SDK License (2022-05-10).** Distribution is allowed only as object code inside an application with "material additional functionality". Stand-alone distribution and reverse engineering are not allowed [S19]. [V] The download page says the SDK is free for commercial use and requires Developer Program membership [S19 search]. [U] No explicit NVIDIA-GPU-only clause was found, but the library only works on NVIDIA OFA hardware anyway. [V] NVIDIA recommends `SecureLoadLibrary()` so that only the NVIDIA-signed NvFRUC.dll is loaded [S19 search].
- **Applicability:**
  - **Medium, as an optional NVIDIA-only accelerator** for MoonSR's motion estimation on Turing to Ada. It could run alongside, or validate, MoonSR's portable compute-shader optical flow.
  - It does not cover GTX 10, AMD or Intel, so it can never be the only path.
  - [I] Using NvOFFRUC as a shipped interpolator would make MoonSR's FG not original. Using the raw NVOFA flow API as a motion-estimation input is a cleaner fit.

**Design lessons from FRUC (public description, clean-room OK):**
- Bidirectional flow with a consistency check.
- Fill rejected vectors before warping.
- Fill image-space holes after warping.
- **Fall back to repeating a frame when confidence is low.** A repeated frame looks better than a broken one.

---

## 9. RTX Video (VSR / HDR) and DLSS 5

### 9.1 RTX Video Super Resolution and RTX Video SDK
- **What it does:** [V2] AI upscaling of browser video (Chrome, Edge, later Firefox) from pixels only, with no engine data. It also removes compression artifacts. [V2] One description says it bicubic-upscales first and then adds AI enhancement [S31].
- **Hardware:** [V] The RTX Video SDK exposes VSR and SDR-to-HDR "RTX Video HDR" to applications on RTX GPUs with Tensor Cores [S32].
- **Unknowns:** [U] The SDK page lists no API (DX11, DX12, CUDA), no license and no version. [V2] The NGC catalog points to a license bundled in the SDK v1.1.0 package [S33]. I could not read it.
- **Applicability:**
  - [I] Technically, it is the one NVIDIA upscaler built for "final image only" input, which matches MoonSR's capture.
  - It is tuned for compressed natural video, not aliased game rendering with a HUD, and it is NVIDIA RTX-only.
  - **Possible optional NVIDIA backend only after its license is read.** Mark the licensing as unverified.

### 9.2 DLSS 5 (announced GTC 2026-03-16, shipped 2026-09-01 in NBA 2K27)
- **Architecture:** [V] A "one-step pixel-space diffusion model", "causal and deterministic". It is conditioned on the current frame, engine motion vectors, carried temporal state and artistic-direction values. It also uses albedo, lighting and normals. It produces the final appearance rather than reconstructing a reference. It runs at up to 4K on RTX 50 [S34, S35].
- **Requirements:** [V] Driver 616.64 or newer [S35].
- **Reception:** [V2] Controversial because it alters faces and materials [S36].
- **Applicability:** None. It is not an upscaler or a frame generator, and it needs engine data and integration. It is listed here only so the "anything newer" question is answered. **There is no "DLSS 5 Super Resolution" or "DLSS 5 Frame Generation"** as of this date [V for the product pages read; U for anything announced after them].

---

## 10. Design lessons for MoonSR (clean-room, capture-based)

Every item below is derived from public descriptions. None requires NVIDIA code.

### Frame generation

1. **Split the work into a per-pair stage and a per-output stage** (NVIDIA's DLSS 4 FG split network [S4]).
   - Run motion estimation, occlusion and confidence estimation **once per real-frame pair**.
   - Run a cheap per-timestep synthesis (warp, blend, inpaint) **once per generated frame**.
   - Cost then scales like `C_pair + N * C_gen`, with `C_gen` much smaller than `C_pair`. That is what makes 3x to 6x affordable.
   - This should be the MoonSR FG architecture.
2. **Compute every intermediate frame directly from the original pair** at t = k/N. Do not chain generated frames from earlier generated frames.
   - [I] NVIDIA's split network and FRUC's arbitrary-timestamp API both imply direct per-t synthesis [S4, S17].
   - Chaining compounds errors.
3. **Use bidirectional flow, a consistency check, vector infill, image-space hole fill, and a confidence-gated fallback to frame repeat** (FRUC [S17]). Add a per-pixel "trust" blend between flow sources: NVIDIA's network "learns when to trust" motion vectors [S4]. MoonSR has only flow, but it can blend multi-scale flow candidates in the same spirit.
4. **UI and HUD is the dominant artifact source, even for NVIDIA.** NVIDIA needed explicit UI-alpha and HUD-less buffers in DLSS 3/4 and additional UI buffers in 4.5 [S8, S13]. A capture has none of these.
   - MoonSR must detect static or near-static overlay regions itself (temporal variance plus flow magnitude near zero, plus edge persistence).
   - It should also expose user-drawn exclusion rectangles.
   - It should hold detected UI regions to the nearest real frame instead of interpolating them.
   - Treat this as a first-class feature, not a polish item.

### Pacing, presentation and latency

5. **Pacing is a separate system, and precision matters more as N grows.**
   - NVIDIA moved pacing from the CPU to display-engine hardware (flip metering) on Blackwell because CPU pacing degraded at higher multipliers [S11].
   - [I] MoonSR has no flip metering on GTX 10, RX 400 or UHD. It needs:
     - a dedicated high-priority present thread with waitable swapchains;
     - a high-resolution timer combined with busy-wait at the tail;
     - DXGI frame statistics or Present statistics to measure actual flip times;
     - output slots spaced evenly at `T_base / N`, based on a smoothed estimate of the real-frame interval.
   - [I] Presenting from async compute (as Smooth Motion does [S15]) is worth evaluating.
6. **Always present real frames. Drop generated frames when late** (SL pacer rule [S8]). Never let the generated-frame queue grow. A dropped generated frame costs smoothness. A delayed real frame costs latency and judder.
7. **Interpolation latency is dominated by holding back one real frame.** The extra generated frames add only a little (DF: +4 ms, then +2 ms [S26]).
   - [I] For a capture pipeline, add capture latency (WGC or DXGI) on top.
   - Budget one base-frame interval plus capture plus synthesis, and keep MoonSR's internal queue depth at 1.
   - Reflex cannot be injected, but MoonSR can avoid adding its own queueing.

### Dynamic multiplier

8. **Dynamic multiplier logic** (behaviour from NVIDIA [S8, S13]; the controller itself is [U], so this is a proposal):
   - Target = display refresh rate (auto-detected) or a user cap. Measure base fps with an exponential moving average over about 0.5 to 1 s.
   - N = clamp(floor(target / base_fps), 1, N_max). Use **hysteresis**, for example require a 10% margin before stepping, and enforce a minimum dwell time, to avoid oscillation.
   - Consider integer-only N, so that output spacing stays even relative to real frames.
   - Below a minimum base fps (for example 30 to 40), warn the user or cap N, because artifacts scale with motion per base frame.
   - NVIDIA's dynamic mode is incompatible with VSync and frame limiters [S13]. MoonSR should define and test its behaviour with VRR, VSync and limiters explicitly rather than inherit that limitation.

### Upscaling

9. **Choose the network type for the hardware floor.**
   - NVIDIA's transformer models depend on FP8 Tensor Cores for affordable cost. Even RTX 20 and 30 pay a heavy price for the 4.5 models [S12].
   - [I] GTX 10, RX 400 and UHD have no matrix units, and some lack fast FP16.
   - A transformer is **not viable** as MoonSR's baseline. Use hand-designed shaders or a very small CNN (int8 or fp16 where available) for the floor.
   - Optionally add higher tiers using DirectML or cooperative vectors on capable GPUs.
   - NVIDIA's own history shows bigger models buy stability and less ghosting, not miracles: 2x parameters for DLSS 4, then 5x compute for 4.5.
10. **Process in linear light where possible.** DLSS 4.5 moved training and inference from log to linear space to fix crushed shadows and muted lighting [S12]. [I] Captured SDR frames are gamma-encoded. Linearise them before resampling or blending, and handle HDR (scRGB or PQ) captures explicitly.
11. **A temporal upscaler without jitter is limited.** [I] DLSS's quality comes from jittered sub-pixel samples [S6]. A capture gives MoonSR the same pixel grid every frame. Temporal accumulation can still denoise and stabilise, but it cannot recover detail the way DLSS does.
    - Plan MoonSR SR as strong spatial reconstruction plus light flow-guided temporal stabilisation.
    - Reserve "true" temporal super-resolution for when the optional bridge supplies jitter.
12. **Repeat on low confidence; never ship garbage.** This applies to both SR history rejection and FG: FRUC's repeat fallback [S17], and the SL pacer dropping late generated frames [S8].

### Engineering and quality assurance

13. **Budgets.**
    - NVIDIA spends about 1 ms per generated 4K frame on an RTX 5090, and 272 to 725 MB of VRAM for FG [S4, S8].
    - [I] On the MoonSR hardware floor, aim for synthesis at 1080p rather than 4K, with flow computed at quarter resolution.
    - Measure on real hardware. Do not extrapolate from NVIDIA numbers.
14. **Measurement.** Use display-change intervals (PresentMon/FrameView `MsBetweenDisplayChange`), not app fps, as the pacing metric [S8]. Use an end-to-end latency tool rather than self-reported timers.

---

## 11. Unverified / unknown

- [U] Exact DLSS SR per-GPU execution-time tables, the scaling ratios for all quality modes, and the complete preset table, including whether Preset J exists. These are in the 84-page programming guide PDF, which I could not parse.
- [U] Internal algorithm of Smooth Motion (OFA, AI flow or hybrid), its pacing method, and its exact latency. Only secondary reviews exist.
- [U] The Dynamic MFG controller: hysteresis, update rate, minimum base fps, and behaviour above refresh. Only the "automatic transmission" description and Streamline parameters are public.
- [U] How flip metering works internally, and whether any public API exposes similar functionality to non-DLSS applications. I found no evidence that it does.
- [U] Whether the Blackwell (RTX 50) Optical Flow Accelerator is still exposed through the OF SDK. The application note lists only Turing, Ampere and Ada.
- [U] The RTX Video SDK license and API surface.
- [U] An explicit license file for the DLSS-G (sl.dlss_g) plugin binary. I infer it falls under the RTX SDKs License.
- [U] Details of "DLSS 4.5 Ray Reconstruction". The source returned 403.
- [V2 only] Digital Foundry's MFG latency numbers (seen only through TechSpot); Smooth Motion latency and quality (DSOGaming); the DLSS 1.0 64xSS training detail and the unofficial "1.9" label; RTX 40 losing up to 15% on Preset L; HotHardware's DLSS 4.5 SDK and test articles (403, not read).
- [U] Reflex SDK license terms and the supported-GPU list. The developer page did not state them.
- [U] Independent image-quality comparisons of the 2nd-gen transformer against native. NVIDIA's "beats native" claim is vendor marketing.
- Nothing found for "DLSS 5 Super Resolution" or "DLSS 5 FG". DLSS 5 is a neural-rendering appearance model only.

---

## 12. Sources

- S1. NVIDIA, "NVIDIA DLSS 2.0: A Big Leap In AI Rendering" (2020-03-23). https://www.nvidia.com/en-us/geforce/news/nvidia-dlss-2-0-a-big-leap-in-ai-rendering/
- S2. Wikipedia, "Deep learning super sampling" (secondary). https://en.wikipedia.org/wiki/Deep_learning_super_sampling
- S3. AnandTech, "NVIDIA Intros DLSS 2.0: Ditches Per-Game Training, Adds Motion Vectors" (redirects; not read). https://www.anandtech.com/show/15648
- S4. NVIDIA ADLR, "DLSS 4" research page. https://research.nvidia.com/labs/adlr/DLSS4/
- S5. NVIDIA DLSS repo license, "NVIDIA RTX SDKs LICENSE" plus RTX Supplement. https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt (raw: https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt)
- S6. Streamline, DLSS SR programming guide. https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
- S7. Streamline, DLSS-RR programming guide. https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md
- S8. Streamline, DLSS-G programming guide. https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
- S9. NVIDIA, "DLSS 3: AI-Powered Neural Graphics Innovations" (2022). https://www.nvidia.com/en-us/geforce/news/dlss3-ai-powered-neural-graphics-innovations/
- S10. NVIDIA Image Scaling SDK (MIT, v1.0.3). https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- S11. NVIDIA, "DLSS 4: Multi Frame Generation and AI innovations" (2025-01). https://www.nvidia.com/en-us/geforce/news/dlss4-multi-frame-generation-ai-innovations/
- S12. NVIDIA, "DLSS 4.5: Dynamic Multi Frame Gen, 6X, 2nd Gen Transformer Super Res" (2026-01-06). https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/
- S13. NVIDIA, "DLSS 4.5 Dynamic Multi Frame Generation and 6X mode released" (2026-03-31). https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-generation-6x-mode-released/
- S14. NVIDIA, "NVIDIA app update adds global DLSS overrides, Smooth Motion for RTX 40". https://www.nvidia.com/en-us/geforce/news/nvidia-app-global-dlss-overrides-rtx-40-series-smooth-motion/
- S15. NVIDIA Linux driver 590.48.01 README, "NVIDIA Smooth Motion". https://download.nvidia.com/XFree86/Linux-x86_64/590.48.01/README/nvpresent.html
- S16. NVIDIA Optical Flow SDK product page. https://developer.nvidia.com/optical-flow-sdk
- S17. NVIDIA, NvOFFRUC programming guide (OF SDK 5.0). https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html
- S18. NVIDIA, NVOFA application note (OF SDK 5.0). https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html
- S19. NVIDIA Optical Flow SDK license (DesignWorks SDK License, 2022-05-10). https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html ; download terms: https://developer.nvidia.com/opticalflow/download
- S20. Streamline repository and license. https://github.com/NVIDIA-RTX/Streamline ; https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/license.txt
- S21. NVIDIA Reflex developer page. https://developer.nvidia.com/performance-rendering-tools/reflex
- S22. NVIDIA, "Reflex 2 with Frame Warp" (2025-01). https://www.nvidia.com/en-us/geforce/news/reflex-2-even-lower-latency-gameplay-with-frame-warp/
- S23. HWBusters, "NVIDIA Reflex 2 is 18 months old and still hasn't shipped" (2026-07, secondary). https://hwbusters.com/news/nvidia-reflex-2-is-18-months-old-and-still-hasnt-shipped/
- S24. PC Gamer, DLSS 4.5 FG UI model test, and DLSS 4.5 testing (secondary). https://www.pcgamer.com/hardware/graphics-cards/nvidias-big-frame-gen-update-to-dlss-includes-a-new-model-for-improved-ui-elements-but-i-cant-see-any-improvements-whatsoever ; https://www.pcgamer.com/hardware/graphics-cards/ive-put-nvidias-new-dlss-4-5-to-the-test-heres-how-to-enable-it-yourself-and-whether-you-should ; HotHardware benchmarks (403): https://hothardware.com/news/nvidia-dlss-45-benchmarked
- S25. Guru3D, "NVIDIA driver brings DLSS 4.5 Ray Reconstruction to all RTX GPUs" (403, not read). https://www.guru3d.com/story/nvidia-driver-brings-dlss-45-ray-reconstruction-to-all-rtx-gpus/
- S26. TechSpot summary of Digital Foundry's early DLSS 4 test (secondary). https://www.techspot.com/news/106265-early-dlss-4-test-showcases-cleaner-images-multiplied.html
- S27. TechSpot DLSS 3 latency testing (secondary; cited via search summary). https://www.techspot.com/
- S28. Noobfeed, DLSS 4.5 Dynamic vs traditional FG (secondary). https://www.noobfeed.com/hardware/dlss-4-5-dynamic-traditional-frame-generation
- S29. Tom's Hardware, Smooth Motion on RTX 40 (secondary). https://www.tomshardware.com/pc-components/gpu-drivers/nvidias-new-driver-update-finally-brings-smooth-motion-to-rtx-40-series-gpus-works-like-amds-fluid-motion-frames-and-claims-to-double-your-fps-with-a-single-click-in-any-game
- S30. DSOGaming Smooth Motion review (secondary). https://www.dsogaming.com/?p=185805
- S31. NVIDIA blog, "RTX Video Super Resolution" (2023). https://blogs.nvidia.com/blog/rtx-video-super-resolution ; HWCooling explainer: https://www.hwcooling.net/en/nvidia-rtx-video-super-resolution-out-what-you-need-to-know/
- S32. NVIDIA RTX Video SDK page. https://developer.nvidia.com/rtx-video-sdk
- S33. NVIDIA NGC catalog, RTX Video model (DLPP). https://catalog.ngc.nvidia.com/orgs/nvidia/teams/multimedia/models/dlpp
- S34. NVIDIA ADLR, "DLSS 5: Generative Neural Rendering". https://research.nvidia.com/labs/adlr/DLSS5/
- S35. NVIDIA, "DLSS 5: Neural Rendering Brings Lifelike Lighting to NBA 2K27" (2026-09-01). https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/
- S36. NVIDIA newsroom, DLSS 5 announcement (2026-03-16). https://nvidianews.nvidia.com/news/nvidia-dlss-5-delivers-ai-powered-breakthrough-in-visual-fidelity-for-games ; reception (secondary): https://www.neowin.net/amp/nvidia-unveils-dlss-5-calls-it-the-gpt-moment-for-graphics/
- S37. NVIDIA GTC 2020 talk S22698, "DLSS 2.0: Image Reconstruction for Real-time Rendering with Deep Learning" (Edward Liu; not watched). https://developer.nvidia.com/gtc/2020/video/s22698-vid

---

## 13. DLSS 4.5 cost ceilings (published NVIDIA GPU execution times)

Added 2026-11-16 (graphics-researcher). This closes the gap noted in section 11 ("exact DLSS SR per-GPU execution-time tables ... in the programming guide PDF, which I could not parse"). I downloaded the two PDFs from the NVIDIA/DLSS repository into a scratch folder and extracted their text with `pdftotext -table` (xpdf); the tables below are copied from that text. Evidence tags as in the header.

### 13.1 Sources and versions
- **S38.** NVIDIA, "NVIDIA DLSS Super Resolution (version 310.6.0) Programming Guide", document revision 310.6.0, "Released: March 2026", footer "NVIDIA DLSS | 31 March 2026", section "DLSS Execution Times & GPU RAM Usage", printed pages 5-7. URL: https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf (raw: https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf). SHA-256 of the file I read: `dca4d2e68ccedc04a431803595867a893474b1cce033e65a41e9eab9f6b436f8`.
- **S39.** NVIDIA, "DLSS Frame Generation SDK, Version 310.7.0", dated June 2026, section "Performance", printed pages 3-4. URL: https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-FG%20Programming%20Guide.pdf (raw: https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS-FG%20Programming%20Guide.pdf). SHA-256 of the file I read: `acc65d72d88bc2f226bfef1030ea5a82adc0f318fd1143202fa9e74a7a2d95aa`.

### 13.2 Super Resolution execution time (S38) [V]
Test conditions quoted from S38: NVIDIA ran the DLSS library "using a command line utility (i.e. without a 3D renderer)" across GeForce RTX GPUs; the exact time "varies from engine to engine and is dependent on the integration"; the figures are "a rough estimate". The algorithm "is executed in 16-bit 'Performance Mode' where the input is one quarter the number of pixels as the output": 960x540 -> 1920x1080, 1280x720 -> 2560x1440, 1920x1080 -> 3840x2160.

Preset meanings from S38 section 3.12 [V]: K is the default for DLAA/Balanced/Quality; J is similar to K; **L** is the default for Ultra Performance, "more expensive performance wise" and "peak performant on RTX 40 series GPUs and above"; **M** is the default for Performance mode, "closer in speed to Presets J, K". The revision history says L and M were added in 310.5.0 (11/20/2025) and E, F were re-added with a deprecation note in 310.6.0 (2/17/2026). Mapping J,K = DLSS 4 first-generation transformer and L,M = DLSS 4.5 second-generation transformer comes from NVIDIA's DLSS 4.5 announcement [V, S12] plus that revision history. **That E,F are the CNN models is my inference [I]: the guide text I extracted never labels them CNN, only "deprecated" and (for F) "intended for Ultra Perf/DLAA modes".**

Selected rows, exact values from S38 (ms; columns are output resolution 1920x1080 / 2560x1440 / 3840x2160):

| GPU | Preset | 1080p | 1440p | 4K |
|---|---|---|---|---|
| RTX 5070 | E, F | 0.27 | 0.43 | 0.93 |
| RTX 5070 | J, K | 0.64 | 1.02 | 2.11 |
| RTX 5070 | L | 1.17 | 1.87 | 3.87 |
| RTX 5070 | M | 0.75 | 1.33 | 2.98 |
| RTX 5080 | E, F | 0.20 | 0.30 | 0.62 |
| RTX 5080 | J, K | 0.46 | 0.68 | 1.31 |
| RTX 5080 | L | 0.78 | 1.16 | 2.24 |
| RTX 5080 | M | 0.49 | 0.80 | 1.74 |
| RTX 5090 | E, F | 0.15 | 0.22 | 0.40 |
| RTX 5090 | J, K | 0.36 | 0.49 | 0.87 |
| RTX 5090 | L | 0.56 | 0.77 | 1.37 |
| RTX 5090 | M | 0.35 | 0.51 | 1.05 |
| RTX 4070 Ti | E, F | 0.26 | 0.45 | 1.02 |
| RTX 4070 Ti | J, K | 0.56 | 0.91 | 1.97 |
| RTX 4070 Ti | L | 0.90 | 1.46 | 3.18 |
| RTX 4070 Ti | M | 0.68 | 1.18 | 2.74 |
| RTX 4090 | E, F | 0.17 | 0.25 | 0.53 |
| RTX 4090 | J, K | 0.38 | 0.54 | 1.06 |
| RTX 4090 | L | 0.59 | 0.85 | 1.67 |
| RTX 4090 | M | 0.41 | 0.64 | 1.39 |
| RTX 3060 | J, K | 1.45 | 2.39 | 5.05 |
| RTX 3060 | L | 3.96 | 6.53 | 13.81 |
| RTX 3060 | M | 2.81 | 5.05 | 11.10 |

The guide also lists RTX 2080 Ti, 3070, 3080 Ti and 4080 (omitted here). **No RTX 5060 row exists in the table [V by absence].** The nearest listed Blackwell GPU is the RTX 5070; an RTX 5060 is a smaller GPU, but no number for it is published and **I do not estimate one.** Caveat: the RTX 3060 and 3070 rows straddle a page break and plain text extraction scrambles the row labels (the 3060 values above come from the `-table` extraction, whose labelling is internally consistent); I did not check them against a rendered image of the page [U, minor]. The 5070, 5080, 5090, 4070 Ti and 4090 rows are unambiguous.

Reading for MoonSR [I]: the second-generation transformer at 1080p output on an RTX 5070 costs 0.75 ms (M) to 1.17 ms (L) in Performance mode with one-quarter-pixel input. Quality and Balanced mode inputs are larger and not in the table. These ceilings apply to an engine integration with motion vectors, depth and jitter; DLSS SR is not usable on a bare capture (section 1.7).

### 13.3 Frame Generation execution time (S39) [V]
Quoted from S39 "Performance": NVIDIA "benchmarked DLSS-FG across multiple games using different NVIDIA GeForce RTX graphics cards"; performance depends "on the specific game engine and integration details". Columns are "2x Cost (ms)" and "4x Cost (ms)". The guide does not define whether 4x is a total per rendered frame; the numbers (4x cost less than twice the 2x cost) and its remark that later generated frames are "much faster to generate" fit "total per rendered frame" [I].

| GPU | Resolution | 2x cost (ms) | 4x cost (ms) |
|---|---|---|---|
| RTX 4090 | 1080p | 1.35 | - (not listed) |
| RTX 4090 | 1440p | 1.78 | - |
| RTX 4090 | 4k | 2.77 | - |
| RTX 5080 | 1080p | 1.45 | 2.43 |
| RTX 5080 | 1440p | 2.04 | 3.61 |
| RTX 5080 | 4k | 2.84 | 5.25 |
| RTX 5090 | 1080p | 1.07 | 1.78 |
| RTX 5090 | 1440p | 1.46 | 2.48 |
| RTX 5090 | 4k | 1.72 | 3.32 |

VRAM estimate (S39): 218 MB at 1080p, 384 MB at 1440p, 598 MB at 4k, "does not change significantly between GPUs or between single-frame and multi-frame generation". (Section 3.4's 272/489/725 MB comes from the Streamline guide, a different document.)

Not published in S39 [V by absence]: any RTX 5060, 5070 or 5070 Ti row; any **6x** cost; Dynamic MFG cost; lite-blending cost; which model preset was used. Arithmetic on published rows, not a published number [derived]: RTX 5080 at 1080p, 2x to 4x adds 2.43 - 1.45 = 0.98 ms for two more generated frames, about 0.49 ms each; RTX 5090 at 1080p adds 0.71 ms, about 0.36 ms each.

### 13.4 Local measurement for comparison (the owner's, not NVIDIA's)
On the owner's RTX 5060 at 1920x1080 the DLSS-FG addon reports GPU time per real frame of about 8 ms (2x), 10 ms (4x), 12 ms (6x) with lite candidate blending and measured motion, and about 27 ms at 6x with full blending [V, `LS Addons\DLSS-FG\README.md` in the owner's local addon folder; the addon author's sustained numbers]. That README also quotes "2.4 ms for 4x at 1080p on an RTX 5080", consistent with the S39 row. A live 2x log line shows 0.47-2.76 ms to generate (`Lossless Scaling\logs\DLSSMFG.log.old`) [V]. The gap between the S39 ceilings and the addon's 8-12 ms is large and unexplained here; contributors such as the measured-motion pass, GPU sharing with the game, or a smaller GPU than the 5080 are conjecture [I].

### 13.5 Usable ceilings for MoonSR budgeting [I]
- An original capture-based frame generator costing under about 1.5 ms per rendered pair at 1080p is level with NVIDIA's published 2x cost on an RTX 5080 (1.45 ms) and above it on an RTX 5090 (1.07 ms). Each extra generated frame should cost well under 0.5 ms; NVIDIA's own derived marginal cost is 0.36-0.49 ms.
- The motion estimator alone (0.5-0.75 ms at 1080p in the owner's addons, see `ls_reference_motion_depth.md`) is about half of NVIDIA's 2x ceiling on an RTX 5080.
- For upscaling, the second-generation transformer at 1080p output costs 0.49 ms (M) to 0.78 ms (L) on RTX 5080 and 0.75 to 1.17 ms on RTX 5070, in Performance mode.

### 13.6 Sources added
- S38. https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf (rev 310.6.0, March 2026)
- S39. https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-FG%20Programming%20Guide.pdf (v310.7.0, June 2026)
