# AMD and Intel upscaling / frame generation research

Date: 2026-11-13. Author: graphics-researcher.

**Method note.** Web search and page fetch were available and used. Claims are tagged:
- **[V]** verified from a fetched source (URL given),
- **[I]** inferred,
- **[M]** from model memory, not re-verified this session (treat as unverified),
- **[U]** not publicly documented.

Several pages were fetched through a summarizing tool, not read raw; quotes and numbers are as relayed by that tool. Some 2026 news items (FSR 4.1, SDK 2.2/2.3, XeSS MFG) come only from press search snippets and carry conflicting dates. They are marked as such. The FSR 1 EASU/RCAS internals could not be fetched (the SDK header mirror returned 404) and are section [M].

Legal notes are a technical reading, not legal advice.

---

## 1. AMD

### 1.1 FSR 1 (EASU + RCAS)
- **Version:** FSR 1.0 (2021); the SDK still carries FSR1 as `ffx_fsr1.h` (host API plus GPU header under `include/FidelityFX/gpu/fsr1/`). [V] https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-spatial/
- **Architecture:** purely spatial, two compute passes: EASU (Edge-Adaptive Spatial Upsampling) then RCAS (Robust Contrast-Adaptive Sharpening). No history, no motion vectors, no depth. [V] same page.
- **Inputs:** one color image, anti-aliased, normalized [0,1], in a perceptual (gamma/sRGB) space, noise free; integrate after AA/tone mapping and before grain, CA and UI. Negative values into RCAS produce NaN. [V]
- **EASU, what the official manual says:** it detects "gradient reversals" over a set of input pixels, and their strength sets the weights used to reconstruct display-resolution pixels. The manual does not give tap layout or math. [V]
- **EASU, detailed algorithm [M]:**
  - For each output pixel, map to input space, take the nearest input pixel and read a 12-tap footprint (a 4x4 neighbourhood minus corners, plus the center cross).
  - Compute a luma proxy per tap (approx `0.5*B + 0.5*R + G`, i.e. cheap, no true luma).
  - For the four 2x2 bilinear cells around the sample, estimate local gradient direction from luma differences (horizontal and vertical pairs), weight each cell by its bilinear weight at the sub-pixel position, and accumulate a direction vector and a length/strength term.
  - The direction vector is normalized (with a guard against zero length). Length is derived from how consistent the horizontal/vertical gradients are (edge-ness), then shaped (squared, clamped) to a stretch factor.
  - The sample offset vector is rotated into the edge frame and scaled: along the edge the kernel is lengthened, across the edge it is kept tight, so edges are reconstructed as lines instead of stairs.
  - The kernel is a windowed polynomial approximation of Lanczos-2: `w(x^2)` built from nested squared terms, cheaper than sin/pow. A negative lobe is used and its strength is reduced as the edge-length measure drops (flat areas get a near-bilinear result, edges get a sharper lobe).
  - Output is clamped to the min/max of the 4 center taps (the bilinear cell) to prevent ringing/overshoot.
  - Exact constants are not reproduced here because they are unverified. Verify against the header before implementing anything derived from it.
- **RCAS [M]:**
  - 5 taps: center plus N/S/E/W cross.
  - Per-channel (R, G, B) it computes the largest negative lobe that does not push the result outside the min/max of the cross taps ("robust" = clip-avoiding), then scales by the user sharpness (given in stops, exponent base 2) and a hard cap of about -1/5 at maximum.
  - Result is `(w*sum(neighbours) + center) / (4w + 1)` with normalization.
  - Optional noise handling (`FSR_RCAS_DENOISE`) reduces the lobe where the center differs from the neighbour average in a noise-like pattern (luma-based weight), so film grain is not amplified. The official manual says nothing about internal denoising and tells the integrator to supply noise-free input. [V for the manual statement, M for the internals]
- **CAS** is the older single-pass contrast-adaptive sharpener; RCAS is its derivative. [M] Its place in the SDK is `FidelityFX CAS` (own header `ffx_cas.h`). Not fetched this session. [U for licence-file placement, see 3.]
- **Hardware/API:** any GPU with compute; D3D11 HLSL SM 5.0 port is trivial and widely done. The header ships GLSL/HLSL/C++ shared code, so it targets D3D11-level hardware. [I]
- **Performance:** the manual gives no figures. [V] Cost is a small fraction of a ms at 4K on modern GPUs [M].
- **Quality:** strong on edges and text at 1.3-1.5x; falls off sharply at 2x+ because it cannot invent detail; no anti-aliasing; sensitive to noise and to un-antialiased input (amplifies aliasing). [M/I]
- **Licence:** see section 3.
- **Applicability:** directly usable on a captured window image. Fully fits the project baseline (D3D11 CS 5.0). Capture is usually already tone-mapped, gamma-encoded and with UI baked in, which is the exact assumption violation the manual warns about (UI after upscaling), but spatial upscaling of UI is acceptable.

### 1.2 FSR 2.x (temporal)
- **Version:** component version in the 2.4.0 SDK is "FSR Temporal 2.3.4". [V] https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- **Stages (names only, from manual outline):** compute luminance pyramid; reconstruct and dilate; depth clip; create locks; reproject and accumulate; RCAS. [V] https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/ (page text was truncated; stage descriptions not read)
- **Required inputs:** render-res color, depth, motion vectors, optional reactive mask (R8, marks alpha/particles with no depth footprint), optional transparency-and-composition mask, exposure, **sub-pixel camera jitter** (Halton via `ffxFsr2GetJitterOffset`), frame time delta. [V]
- **Presets:** Quality 1.5x, Balanced 1.7x, Performance 2.0x, Ultra Performance 3.0x. Memory about 323-448 MB at 4K. HLSL CS 6.2 (6.6 for wave64). [V]
- **Mechanics (conceptual) [M]:** reprojection of accumulated history using dilated motion vectors; depth-clip detects disocclusion by comparing reprojected depth; "locks" mark thin-feature pixels that keep their history for several frames; reactive mask reduces history weight; accumulation uses Lanczos-weighted new samples with luma-stability and color clamp rectification.
- **Weaknesses [M]:** ghosting on disocclusion and transparency, shimmering fine detail at Performance and below, and loss of thin features.
- **Applicability:** needs jitter and MVs, which the capture pipeline lacks. Not usable in the default pipeline. Only relevant for the optional game-integration bridge, or as a concept source (history rectification, locks).

### 1.3 FSR 3 / 3.1 frame generation
- **Versions:** FSR 3.0 (Sep 2023, initial), 3.1 (Jul 2024: upscaler decoupled from frame generation, Vulkan, FidelityFX API), 3.1.1 (Anti-Lag 2 in the swapchain), 3.1.2 (velocity factor to reduce bright flicker, ellipsoid clamp, frame distortion texture), 3.1.3 (opt-in hybrid spin-lock pacing), 3.1.4 (camera parameters for ML FG, less disocclusion ghosting), 3.1.5 (Aug 2025). [V] https://gpuopen.com/fidelityfx-super-resolution-3/ . SDK 2.4.0 lists FG 3.1.6 and swapchain 3.1.7 / 4.0.0. [V] GitHub README.
- **Requirements:** DX12, Vulkan, UE 5.1-5.6. Works from 60 fps base best; avoid below 30. Needs motion vectors and depth at render resolution; third-party upscalers are supported if they provide those. [V] same page.
- **Overhead (4K Quality estimates, async off):** 7900 XTX 1.0 ms upscale plus up to 1.7 ms FG; 6800 XT 1.8 plus 2.4; 5700 XT 4.4 plus 5.0. [V]
- **Optical flow algorithm (FidelityFX Optical Flow 1.1.2) [V]** https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/
  - Input: one scene color buffer; motion estimated on **luminance**. Output: `R16G16_SINT` vectors, **one per 8x8 block** (size `(w+7)/8`), fixed block size 8; plus a 3x1 `R32_UINT` scene-change result.
  - Luma pyramid: full-res luma plus 6 downsampled levels (7 total) built by SPD; two pyramids ping-pong for history.
  - Scene-change detection: luma split into 9 sections, per-section histograms compared to the previous frame; threshold exceeded sets a flag; search then returns zero motion. (Bin count and threshold not documented.) [U]
  - 7 iterations coarse to fine, each = search, filter, upscale (upscale skipped last).
  - Search: each non-overlapping 8x8 block tested at 16x16 = 256 offsets in a 24x24 window (radius 8) around the predicted vector, cost = SAD over 64 pixels via `msad4`; result is a correction added to the incoming estimate.
  - Filter: 3x3 vector-median (the vector with smallest summed difference to the others).
  - Upscale: vectors x2; each coarse block becomes four 4x4-pixel sub-blocks at the finer level; 4 candidates (own upscaled vector plus 3 neighbours) are scored by SAD on 4x4 blocks and the best wins.
  - Max trackable motion about 512 px; requires SM 6.2 (wave ops, `msad4`); about 26-28 MB at 4K.
  - No inpainting in the optical flow stage itself. [V]
- **Frame interpolation algorithm (1.1.3) [V]** https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/
  1. Clear counters/fields (atomics are used later).
  2. Estimate interpolated frame depth: reproject and dilate depth/MVs by half the vector.
  3. Build a "game motion vector field" in the interpolated frame's frame of reference, scattered with atomics; vector in low 16 bits of two uint32, high bits priority (1 bit primary/secondary, 10 bits depth distance, 5 bits colour similarity). Field may contain holes.
  4. Inpaint game-MV holes via an SPD mip pyramid (vectors in xy, depth priority z, colour priority w; picks closest in 2D, farthest in depth).
  5. Same scatter for the optical-flow vector field; priority from vector magnitude and colour similarity of optical-flow-reprojected colours.
  6. Disocclusion masks (interpolated vs previous, interpolated vs current), derived from the temporal upscaler's disocclusion logic.
  7. Reconstruction: blend the two source frames by disocclusion mask; compute an optical-flow-based colour and blend with the game-vector result by colour-similarity match; holes may remain.
  8. Inpainting pyramid of the interpolated colour ignoring holes.
  9. Hole filling and UI composition (HUD-less mode cleans up UI).
  - Output needs an alpha channel (used for intermediate data). Requires CS 6.2/6.6. [V]
  - The fact that game motion vectors are a primary input means FSR 3 FG is **not** a pure image-only technique. The optical-flow path alone is the fallback/secondary source. [I] (This is the mechanism by which the original AFMF-derived optical flow is used.)
- **UI handling modes:** (1) UI callback rendered over every presented frame; (2) separate UI render target alpha-blended over real and generated frames; (3) HUD-less back buffer, detect UI by comparing with/without UI. [V] https://gpuopen.com/fidelityfx-super-resolution-3/ and the swapchain manual.
- **Frame pacing swapchain [V]** https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation-swap-chain/
  - Replaces DXGI/Vulkan swapchain; one extra copy when FG off.
  - Two extra threads: a high-priority pacing thread tracking a moving average of frame time (including UI composition) and computing the target present interval, waiting for GPU work; a present thread that composes the generated frame, waits until the interval elapsed since the last present, presents it, then repeats for the real frame.
  - Goal: equal on-screen time for generated and real frames, although generated frames take much less GPU time.
  - Busy-wait (spin) for the final part because Windows is not real-time and VRR is timing sensitive; 3.1.3 added opt-in "hybrid spin lock".
  - Asynchronous present queue (second graphics queue) and optional async compute for interpolation.
  - Guidance: render rate slightly below half of target; with VSync on effective render cap = half the refresh rate; use frame-latency waitable object or limiter; keep command lists short; VSync adds up to one refresh of latency, off gives tearing.
  - Requires unobstructed swapchain access; overlays that hook DXGI cause pacing problems. [V] (relevant: this is the opposite of our overlay model.)
- **Quality:** artifacts in UI and thin structures, disocclusion holes, and the damage that upscaler artifacts propagate into generated frames (AMD advises a good upscale-only path first). [V for AMD advice; I for class list]
- **Licence:** see section 3.

### 1.4 FSR 4 / FSR Upscaling 4.x (ML)
- **Versions:** FSR 4 launched with RDNA 4 (Mar 2025); SDK 2.4.0 "Redstone" lists "FSR ML-Upscaler 4.1.2". [V] GitHub README. Press snippets (unverified, conflicting dates): FSR 4.1 reaching RX 7000 (about July 2026) and RX 6000 / Ryzen AI via Adrenalin 26.10.1 (about Oct 2026). [U-secondary] https://www.invenglobal.com/articles/27423/amd-brings-fsr-41-to-radeon-rx-6000-cards-apus-and-xbox ; https://pcgamer.com/hardware/graphics-cards/amd-bringing-fsr-4-1-ml-powered-upscaling-to-rx-7000-series-in-july-rx-6000-series-in-2027/
- **Architecture:** ML temporal upscaler using engine inputs (colour, depth, motion vectors, jitter) like FSR 2/3. Network topology not publicly documented. [U]
- **Hardware:** original FP8 path needs RDNA 4 WMMA FP8; an INT8 variant was accidentally published in the SDK (about Aug 2025) and modders ran it on RDNA 2/3 and some NVIDIA cards with worse perf. [V-secondary] https://www.tomshardware.com/pc-components/gpus/amd-accidentally-marks-fsr-4-open-source-source-code-reveals-potential-support-for-older-radeon-gpus ; https://www.windowscentral.com/hardware/amd/fsr-4-rdna-2-3-nvidia-amd-unsupported
- **Licence:** the licence text does not mention FSR 4 or ML separately; DLLs are binary-only. See section 3. The accidental source exposure does not grant rights. [V/I]
- **Applicability:** needs engine inputs; GPU-gated; closed. Out of scope for the default pipeline. Possibly interesting only for the bridge.

### 1.5 FSR "Redstone"
- Redstone = ML upscaling (FSR 4), ML frame generation, ML ray regeneration (denoiser), neural radiance caching. Initially announced as RDNA 4-only. [V-secondary] https://www.notebookcheck.net/AMD-FSR-Redstone-promises-faster-RDNA-4-ray-tracing-with-ML-Ray-Regeneration-and-more.1021290.0.html
- SDK 2.4.0 component list: ML-Upscaler 4.1.2, FG 3.1.6, FG swapchain 3.1.7 / 4.0.0, FG (ML) 4.0.2, Ray Regeneration (ML denoiser) 1.2.1, Radiance Caching (Preview, no version). [V] GitHub README.
- ML FG takes camera parameters (added in 3.1.4 notes). Spatial and temporal frame data used. Network architecture: [U].
- Ray regeneration and radiance caching need path-traced engine data (G-buffer, ray hit data). Not applicable to capture. [I]
- **Multi-frame generation:** one report speculates 4-6x capability in an SDK update; no confirmed shipping MFG found. [U] (https://tech-insider.org/amd-fsr-4-1-rx-6000-gpus-400-games-2026/ is unreliable.)

### 1.6 AMD Fluid Motion Frames (AFMF 1 / 2 / 2.1+)
- **What it is:** driver-level frame generation applied to any DX11/DX12 game with no game integration; AFMF 1 preview Dec 2023; AFMF 2 preview Jul 2024, final in Adrenalin 24.9.1; 2.1 later (about 25.9.x with "Search Mode" and "Performance Mode"). [V-secondary] https://www.jonpeddie.com/news/amd-announces-a-major-advancement-in-frame-generation-technology-for-amd-hypr-rx/ ; https://www.pcgamer.com/hardware/graphics-cards/amd-fluid-motion-frames-2-is-out-now-and-hands-radeon-gamers-a-new-ai-optimised-frame-generation-option-in-most-games
- **Method:** AMD says AI was used to develop the algorithm; no AI inference at runtime for AFMF 2 (secondary report). [V-secondary] https://www.digitaltrends.com/computing/amd-afmf-2-tested/ . The motion estimation is not documented in detail. [U] FSR 3's block-based optical flow is described by AMD as "enhanced from AMD Fluid Motion Frames" [V] (gpuopen FSR3 page), so it is reasonable to infer AFMF is a similar block-based, luma-driven pyramid search on the presented frame without game MVs. [I]
- **Inputs:** only the final presented frame(s), no depth, no MVs. HUD-less input is unavailable, so UI is handled heuristically; 2.1 improved this per press. [V-secondary] https://canaltech.com.br/hardware/3-melhorias-do-amd-fluid-motion-frames-21/ , exact method [U].
- **Fallback:** automatically disables generation during fast motion, producing jitter ("Search Mode" improves fallback smoothness). [V-secondary] PCGamer/Digital Trends links above.
- **Hardware:** officially RX 6000 / 7000 series plus select Ryzen; later RDNA 4 support. [V-secondary] Compatibility beyond that: [U].
- **Latency:** AMD claims lower than AFMF 1; works best with Anti-Lag; Radeon Chill sets cap to half of FreeSync max. [V-secondary]
- **Known artifact classes (reviews, not controlled studies):** HUD/subtitle/crosshair distortion, ghosting on fast swings and camera turns, particles, foliage, thin geometry, UI transitions. [V-secondary] https://gamersnexus.net/index%2Ephp/gpus/amd-fake-frame-image-quality-afmf-fsr-4-vs-fsr-31-comparison ; https://www.digitaltrends.com/computing/amd-afmf-2-tested/
- **Applicability:** this is the closest analogue to what MoonSR must do. Lessons in section 5.

### 1.7 Radeon Anti-Lag / Anti-Lag 2
- Anti-Lag (driver, DX11/12) limits CPU run-ahead. Anti-Lag+ hooked game code, triggered VAC bans in CS2 and was removed. Anti-Lag 2 is an in-engine SDK integration (RX 5000+), AMD CS2 example 11 ms vs 16 ms (Anti-Lag) vs 19 ms (none) on 7900 GRE 4K. [V-secondary] https://www.tomshardware.com/pc-components/gpu-drivers/amd-introduces-radeon-anti-lag-2-with-full-integration-for-counter-strike-2-the-nvidia-reflex-alternative-shouldnt-trigger-cheating-bans-this-time
- Applicability: none to a capture app (cannot instrument the game's frame loop). Lesson: injecting into games risks anti-cheat bans.

---

## 2. Intel

### 2.1 XeSS Super Resolution (1.x / 2.x / 3.x)
- **Versions:** repo at https://github.com/intel/xess is labelled XeSS 3 SDK; contains `bin`, `doc`, `inc`, `lib`, `samples`, LICENSE.txt, third-party-programs.txt. [V]
- **Architecture:** ML temporal upscaler (colour, depth, motion vectors, jitter, exposure), the same input class as FSR 2. Intel states XeSS-SR runs on SM 6.4 DP4a GPUs and uses XMX on Arc. The network architecture is not published. [V for requirements, U for network] https://github.com/intel/xess ; https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html
- **Paths:** XMX (Arc), DP4a (cross-vendor, simpler model, recommended NVIDIA 30xx+ / AMD 6000+), and the dispatcher can pick the game-shipped runtime, driver runtime, or the cross-vendor implementation. [V]
- **API:** SR on DX11, DX12, Vulkan; FG and XeLL on DX12. [V]
- **Quality:** secondary sources say the DP4a model is less robust than XMX; no controlled data obtained. [V-secondary / U]
- **Applicability:** needs jitter, MVs, depth. Not usable on captured images. DX11 SR exists, but needs engine data.

### 2.2 XeSS Frame Generation (XeSS 2) and Multi Frame Generation (XeSS 3)
- **FG inputs:** MVs (jitter-free), depth, HUD-less colour, UI-only texture with alpha, frame constants (view/projection matrices etc.). DX12, Win10/11 x64, Intel Arc with XMX, driver 32.0.101.6252+, XeLL 1.1.0+ mandatory. Proxy swapchain; presentation on a separate thread; not usable in exclusive fullscreen or with other FG libraries; recommended 40 fps min, 60 fps for good latency. [V] https://intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html
- **Cross-vendor status conflict:** the GitHub README says XeSS-FG runs on non-Intel SM 6.4 GPUs; the FG developer guide fetched says Intel Arc with XMX; the Intel overview page says XeLL is required for FG on all hardware and XeLL alone is Intel-only. [V, conflicting] Treat cross-vendor FG as unconfirmed. Jon Peddie Research claims DP4a FG, unverified. https://www.jonpeddie.com/news/intels-xess-2-supports-ai-driven-frame-generation-on-all-gpus/
- **MFG (XeSS 3):** up to 3 generated frames, Intel hardware only per Intel's footnote; first on Panther Lake, later Arc A/B discrete via driver 32.0.101.8509 (Feb 2026) per press. [V (Intel footnote), V-secondary (rollout)] https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html ; https://windowsreport.com/xess-3-multi-frame-generation-expands-to-all-intel-arc-graphics/
- **Algorithm internals / performance:** not documented in the sources read. [U]
- **Applicability:** needs engine inputs plus HUD-less frames; cannot run on arbitrary capture. The useful content is design: proxy swapchain, separate present thread, UI composition modes, partial-area interpolation (letterbox), reset flags.

### 2.3 Xe Low Latency (XeLL)
- Latency reduction library (Reflex-like), DX12, required by XeSS-FG, must remain enabled; same frame counter shared with FG. Standalone only on Intel Arc. [V] Intel pages above.
- Applicability: none (needs the game's frame markers).

---

## 3. Licensing

| Item | Licence | What is allowed | Source |
|---|---|---|---|
| FidelityFX SDK, files listed in the second licence block (e.g. `Samples`, `Kits\Cauldron2`, `3rdpartynotice.md`) | MIT-style, (c) Advanced Micro Devices, Inc. | use, modify, merge, distribute, sublicense, sell, with notice retained | docs/license.md (fetched via summarizer) |
| Everything else in the SDK (including the signed DLLs under `Kits\FidelityFX\signedbin`) | AMD proprietary "binary only" licence | free use and distribution **in binary form only**; no reverse engineering, decompilation, disassembly; copyright and permission notices must be reproduced in docs/materials | same |
| FSR 4 / ML | not separately addressed; falls under the above by default | binaries only [I] | same |
| Intel XeSS SDK | Intel Simplified Software License (Oct 2022), XeSS SDK (2025) | use and redistribute the **binary-only** software **unmodified**; keep notices; no reverse engineering; no use of Intel names for endorsement; feedback usable by Intel; Delaware law; termination on breach | https://raw.githubusercontent.com/intel/xess/main/LICENSE.txt (via summarizer) |

Important caveats:
- The "SDK is MIT" premise is only **partly** true. The GitHub README says "AMD FSR Samples are open source ... under the MIT license", but the licence file as relayed has a **proprietary binary-only default** and an MIT carve-out for a file list. **I could not confirm into which licence `ffx_fsr1.h`, `ffx_cas.h`, the optical flow and frame interpolation shader sources fall**, because the file list was only summarized. Before shipping any derived code, a human must read `docs/license.md` and match each file path against the MIT file list. [U]
- Historically, the FSR 1 source (2021 standalone release on GPUOpen) was MIT. [M]
- The FSR 1 and CAS algorithms are described in public GDC/GPUOpen material; a clean-room reimplementation avoids the licence question, though patents are not analysed here. [U for patents]
- XeSS cannot be modified or merged into our code; it may only be redistributed as the unmodified binary with notices. It also needs engine inputs, so it is moot for the default path.

---

## 4. Answers to the key questions

**1. Which components can run on a plain captured image?**
- FSR 1 EASU and RCAS: yes (spatial only). [V]
- CAS: yes. [M]
- AFMF-style frame generation (image-only optical flow + interpolation): the technique class exists in the driver, but no SDK exposes it. [V-secondary]
- FSR 3 Optical Flow module: its only input is a colour buffer, so it can produce block vectors from captured frames. [V] The Frame Interpolation module, however, is designed around depth plus game MVs and can take optical flow only as a secondary vector field; running it without game vectors is not a documented configuration. [I/U]
- FSR 2/3/4 upscalers, XeSS-SR, XeSS-FG, XeLL, Anti-Lag 2, Ray Regeneration, Radiance Caching: no (need jitter, MVs, depth, HUD-less frames or engine hooks).

**2. May we ship FSR 1 / CAS / FSR 3 optical flow code?**
- If a file falls in the MIT file list: yes, including modification, with the AMD copyright and permission notice kept in the app's third-party notices. If it falls in the proprietary default: binary-only redistribution with no modification or reverse engineering, and notices retained. [V as relayed]
- Which block each file belongs to is **unverified** (see section 3 caveat). Action: read `docs/license.md` and the per-file headers in the downloaded SDK; keep AMD's file headers intact.
- Practical note: Optical Flow 1.1.2 needs SM 6.2 (`msad4`, wave ops), above our SM 5.0 baseline. Shipping its HLSL as-is does not work on D3D11 SM 5.0. We would have to port or reimplement (SAD loops can be written without `msad4`). [V for requirement, I for port]
- Redistributing the signed AMD DLLs (FSR 3/4 binaries) is permitted only as unmodified binaries and they need DX12 plus engine inputs, so they are not useful.

**3. How does FSR 3 optical flow work, and how do AFMF versions cope without motion vectors / HUD-less input?**
- FSR 3: see 1.3 (8x8 blocks, 7-level luma pyramid, 256-offset SAD search per level in a +-8 window, vector-median filter, 4-candidate refinement, SCD with 9-region histograms). [V]
- AFMF: no public algorithm. It estimates motion from presented frames only. It cannot use depth to arbitrate occlusions or to separate UI. Observable coping behaviours: a fallback that turns generation off in fast motion, a Search Mode for smoother fallback, a Performance Mode that cheapens estimation, and better HUD heuristics in 2.1. [V-secondary] Inner workings: [U].
- Known artifact classes (AFMF and any image-only interpolation): HUD/text/crosshair warping, ghosting on fast motion, disocclusion holes, repeated textures (aliasing in block matching), particles, foliage, thin objects, scene cuts and UI transitions. [V-secondary + I]

**4. What does the FSR 3 pacing swapchain do and what applies to a separate overlay window?**
- See 1.3. In short: it owns presentation, measures frame time (including UI), computes a target interval, and presents generated and real frames at equal spacing from a dedicated high-priority thread with a final busy-wait. [V]
- Lessons for MoonSR (separate overlay window, not our swapchain for the game): we own our own output swapchain, so we can do the same, but the real-frame arrival times come from the capture API, not the game's Present. See section 5. AMD itself says overlays that hook the game's DXGI swapchain disturb FSR 3 pacing, which supports presenting from our own window instead of hooking. [V]

---

## 5. Design lessons for MoonSR

1. **Default upscaler:** implement a clean-room EASU-style edge-adaptive upscaler plus RCAS-style robust sharpener in D3D11 CS 5.0. Make the sharpening limit and noise handling tunable, and pre-clamp inputs to non-negative. Keep capture-specific handling: input is already gamma-encoded, with baked-in UI (draw text-aware rather than treat as noise).
2. **Do not depend on AMD shader files** until each file's licence block is confirmed. If used, keep the AMD notice and retain third-party-notice text in the installer.
3. **Frame generation without engine data:** the image-only recipe that is publicly documented is FSR 3's optical flow (luma pyramid, hierarchical 8x8 SAD search, median filter, per-level candidate refinement) followed by FSR 3-style forward warp with priority-based scatter, mip-pyramid hole inpainting, and a disocclusion blend fallback to blending of the two source frames. Without game MVs the second vector field is absent, so the disocclusion/holes quality will rely on a good flow field and good fallback.
4. **Port issue:** avoid `msad4` and wave ops (SM 6.2). Provide a plain SM 5.0 SAD path; optionally SM 6.x fast paths.
5. **Fallback behaviour is a feature:** AFMF disables generation in fast motion and on scene change. We should have a confidence metric (SAD residual, flow inconsistency) and a scene-cut detector (FSR3-style per-region histogram compare), and fade smoothly to repeat/blend.
6. **UI/HUD problem:** no HUD-less input, so artifacts on HUD are the largest known weak point of AFMF. Mitigate by: static-region detection (pixels unchanged for N frames are copied, not warped), edge-sharp text detection, user-defined exclusion rectangles, and optionally per-region interpolation (the FSR 3/XeSS "interpolation rect" idea). Optional bridge could provide HUD-less frames later.
7. **Pacing:** copy the architecture: dedicated high-priority present thread; moving average of capture inter-arrival interval; present interval = average real interval divided by (1+N); generate the intermediate frame into a pre-composed texture ahead of time so its presentation time is deadline-driven; busy-wait for the last fraction of the interval (VRR/Windows timer limits). Desired render rate is below half the output rate. Present both generated and real frames at equal spacing.
8. **Overlay-specific:** latency is capture latency + one held real frame. We cannot influence the game's frame loop (no Anti-Lag 2 / XeLL possible); expose a clear "latency added" estimate and encourage the user to cap the game fps. Real frame N must be held until frame N+1 is available, so base latency increases by about one source frame. [I]
9. **VSync/VRR:** FSR 3 recommends VRR on, VSync off for variable frame times; with VSync on, output is limited and judder appears below max. Offer the same modes. [V]
10. **Do not hook the game's swapchain or process** (AMD says it harms pacing; Anti-Lag+ shows ban risk). Stick with Windows Graphics Capture / DXGI duplication.
11. **Cross-vendor ML:** XeSS/FSR4 cannot be reused. A small custom ML model (INT8/DP4a style) remains a possible later research item, but is outside this evidence.

---

## 6. Unverified / unknown

- Exact EASU/RCAS tap math and constants (section 1.1 details are from memory).
- Licence membership of `ffx_fsr1.h`, `ffx_cas.h`, optical flow and frame interpolation shader files (MIT vs proprietary default). Need a manual read of `docs/license.md`.
- CAS repository location and licence in current SDK.
- Whether Optical Flow can feed Frame Interpolation without game MVs/depth; the SCD histogram bin count and threshold.
- AFMF internals (motion estimation method, block size, fallback criteria, UI handling) and any changes in AFMF versions after 2.1. Which GPUs are supported at present.
- Whether FSR multi-frame generation shipped (only speculative reports).
- FSR 4 network topology; ML FG internals; Ray Regeneration and Radiance Caching details.
- Dates for FSR 4.1 on RDNA 2/3 and SDK 2.2/2.3 (press dates conflict).
- XeSS network types; XeSS-FG cross-vendor status (conflicting Intel pages); XeSS MFG internals and published performance numbers; XeSS 1.x/2.x/3.x release-note details.
- Patent coverage of any of these algorithms.
- Published performance for FSR 1, XeSS, AFMF (not found from primary sources).

---

## 7. Sources

- https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (README, versions, MIT sample statement)
- https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/main/docs/license.md
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-spatial/
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation-swap-chain/
- https://gpuopen.com/fidelityfx-super-resolution-3/
- https://gpuopen.com/gdc-presentations/2024/GDC2024_High_Performance_Rendering_in_Snowdrop_Using_AMD_FidelityFX_Super_Resolution_3.pdf (listed, not read)
- https://github.com/intel/xess
- https://raw.githubusercontent.com/intel/xess/main/LICENSE.txt
- https://intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html
- https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html
- https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html (listed, not read)
- https://www.jonpeddie.com/news/amd-announces-a-major-advancement-in-frame-generation-technology-for-amd-hypr-rx/
- https://www.pcgamer.com/hardware/graphics-cards/amd-fluid-motion-frames-2-is-out-now-and-hands-radeon-gamers-a-new-ai-optimised-frame-generation-option-in-most-games
- https://www.digitaltrends.com/computing/amd-afmf-2-tested/
- https://gamersnexus.net/index%2Ephp/gpus/amd-fake-frame-image-quality-afmf-fsr-4-vs-fsr-31-comparison
- https://canaltech.com.br/hardware/3-melhorias-do-amd-fluid-motion-frames-21/
- https://www.notebookcheck.net/AMD-FSR-Redstone-promises-faster-RDNA-4-ray-tracing-with-ML-Ray-Regeneration-and-more.1021290.0.html
- https://www.tomshardware.com/pc-components/gpus/amd-accidentally-marks-fsr-4-open-source-source-code-reveals-potential-support-for-older-radeon-gpus
- https://www.windowscentral.com/hardware/amd/fsr-4-rdna-2-3-nvidia-amd-unsupported
- https://www.invenglobal.com/articles/27423/amd-brings-fsr-41-to-radeon-rx-6000-cards-apus-and-xbox
- https://pcgamer.com/hardware/graphics-cards/amd-bringing-fsr-4-1-ml-powered-upscaling-to-rx-7000-series-in-july-rx-6000-series-in-2027/
- https://www.tomshardware.com/pc-components/gpu-drivers/amd-introduces-radeon-anti-lag-2-with-full-integration-for-counter-strike-2-the-nvidia-reflex-alternative-shouldnt-trigger-cheating-bans-this-time
- https://windowsreport.com/xess-3-multi-frame-generation-expands-to-all-intel-arc-graphics/
- https://www.jonpeddie.com/news/intels-xess-2-supports-ai-driven-frame-generation-on-all-gpus/
