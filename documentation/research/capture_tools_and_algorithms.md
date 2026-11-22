# Capture-based Tools and Image-only Algorithms: Competitors, Frame Interpolation, Upscaling

Author role: graphics-researcher. Date: 2026-11-13.
Scope: competitor survey (Part 1), image-only frame interpolation and motion estimation (Part 2), image-only upscaling (Part 3), followed by recommendations.

## Provenance legend

Web search and page fetching were available and used for this document. Every claim carries one of these tags:

- **[V]** Verified this session from the cited source (the source list at the end gives URLs). Where the source is a secondary press article, not the vendor, that is noted.
- **[M]** From model memory (training data up to mid-2026) and not re-checked this session. Usually correct for well-known facts, but check before relying on it for a legal or shipping decision.
- **[I]** Inferred: an engineering estimate or reasoning, not a published figure.
- **[U]** Unknown or not publicly documented.

Every performance number with no [V] tag is an estimate. None of the numbers here are benchmark results produced by this project.

---

## PART 1 - Direct competitors and related tools

### 1.1 Lossless Scaling (THS, Steam app 993090)

LSFG is proprietary. Everything below describes public behaviour only.

**Product facts**
- $6.99 on Steam. Minimum: Windows 10 2004, "modern integrated graphics", DirectX 11. Recommended: Windows 11 24H2 with an RTX 30, RX 6000 or Arc GPU. [V, Steam page]
- "Only windowed and borderless fullscreen modes are supported." Exclusive fullscreen "may work only if the Lossless Scaling output is configured for a second display." [V, Steam page]
- The baseline is D3D11, the same as MoonSR's target. [V, Steam page lists DirectX 11]

**Scalers**
- The store page lists LS1, FSR and NIS "for modern games", Integer and XBR for pixel art, and Anime4K for cartoons/anime. [V, Steam page]
- From memory, the app also offers Nearest Neighbor, Bicubic CAS, SGSR and a sharpness slider. [M]
- LS1 is the in-house spatial scaler. Its algorithm is not documented: [U]. Community descriptions call it an edge-adaptive spatial scaler with sharpening. [M, unverified]

**Frame generation history (LSFG)**

| Version | Public behaviour | Tag |
|---|---|---|
| LSFG 1.x | Initial 2X interpolation. | [M] |
| LSFG 2.0 | Fewer artifacts. The changelog says GPU load rose 1.5-2x by resolution, and a Performance mode was added to compensate. | [V, Wccftech/press summary] |
| 2024 beta | 4X mode added; Digital Trends found it artifact-heavy. | [V, overclock3d, Digital Trends] |
| LSFG 3.0 | New architecture. Press reported about 24% lower latency, better performance and a multiplier unlocked up to x20. | [V, Tom's Hardware, IgorsLab] |
| LS app 3.1 (beta Mar 2025) | **Adaptive Frame Generation (AFG)**: fractional multipliers to hit a target output FPS. "Resolution Scale" renamed **"Flow Scale"**. | [V, TechEnclave, Tom's Hardware] |
| LSFG 3.1, shipped with LS app 3.2 (~Jun 2025) | **Performance mode**: up to about 2x lower GPU load for "slight" quality loss. Developers also claim less ghosting and flicker, better borders, refined **UI detection**, and better quality at low flow scale. | [V, Reddit official 3.2 thread mirror, Notebookcheck, Tom's Hardware] |
| Later | No LSFG 4 announcement found as of Nov 2026. A Notebookcheck guide updated May 2026 still lists LSFG 3.1 as current. | [V, negative search result] |

Note that the app version and the LSFG model version differ: LS 3.1 introduced AFG, and LS 3.2 shipped LSFG 3.1. Press articles conflate the two.

**Flow scale**
- Flow scale is the resolution at which motion is estimated, relative to the output. lsfg-vk exposes it as 1.0 down to 0.25. [V, GamingOnLinux]
- This strongly suggests LSFG estimates flow at reduced resolution and warps at full resolution. [I]

**Adaptive mode**
- The user sets a target FPS that need not be a multiple of the base rate. [V, Steam page]
- Generating frames at arbitrary timesteps implies a model that interpolates at continuous t. [I]

**Low-end GPUs**
- "A separate performance model for low-power GPUs." [V, Steam page]

**Dual GPU**
- The game renders on GPU A, and LSFG runs on GPU B, which drives the display. [V, Steam page; official dual-GPU guide via Reddit mirror]
- The display must connect to the secondary GPU, or there is "a large performance hit". [V]
- Community PCIe guidance: below PCIe 3.0 x4 is not recommended. 3.0 x4 handles about 1080p240, 1440p180 or 4K60. 4.0 x4 handles about 1080p540, 1440p240 or 4K165. These figures come from community testing, not THS. [V as community claim]
- The community says Intel and AMD secondary GPUs are "generally better" than NVIDIA for LSFG. A GTX 1050 is cited as enough for 1080p120 framegen. [V as community claim]

**Capture**
- The DXGI capture API (Desktop Duplication) arrived in v2.6.0. It requires Windows 10 2004 and also "allows LS to be tied to the game's framerate". [V, Steam discussion quoting release notes]
- WGC is also offered. Community guides say WGC is unavailable before Windows 11 24H2 and LS falls back to DXGI. That claim is unusual, since WGC exists from Windows 10 1903, so it probably refers to a specific WGC feature, such as borderless/no-yellow-border or dirty-region support. Treat it as [I/U].
- A GDI option exists. [M]

**Presentation**
- LS draws an overlay window over the game. [M]
- The UI exposes "Sync mode" (default / off-allow tearing / VSync variants), "Max frame latency" and a "Queue target" (frames buffered before generation; lower means less latency and more stutter risk). [M]
- I found no primary documentation for these options: [U] for exact semantics.
- A community guide says Enhanced Sync, Fast Sync and Adaptive Sync do not work with LS. [V]

**HDR**
- An HDR toggle exists in LS. [M]
- I could not verify HDR handling in the 3.x notes: [U]

**Known artifact classes**
These are community-reported and partly acknowledged in the changelogs above. [V, partly]
- Ghosting on moving objects.
- Flicker.
- HUD and UI warping or shimmer during fast camera motion; UI detection was added to address it.
- Screen-border tearing ("border handling").
- Worse quality at high multipliers, low base FPS and low flow scale.
- Dark scenes at 4X (Digital Trends).
- Added input latency, a frequent complaint even with adaptive mode (3.1 beta testers).
- Performance-mode quality loss.

**Implications for MoonSR**
- The bar is set: D3D11, works on integrated graphics, a flow-scale knob, fixed and adaptive multipliers, a performance mode, dual GPU, and UI detection.
- MoonSR must match all of these to be competitive. [I]

### 1.2 lsfg-vk (community Linux port of LSFG)

- **What it is:** A Vulkan layer that runs LSFG on Linux. [V, GamingOnLinux; LinuxLinks]
- **Dependency:** It relies on an installed, purchased Lossless Scaling. "You also need to own a copy of Lossless Scaling on Steam." [V]
- **Mechanism:** A commit titled "extract shaders from Lossless.dll" (2025-07-04) indicates it loads THS's compiled shaders from the user's own DLL at runtime. [V as commit title; mechanism details U]
- **v2.0 (Sep 2026):** Bindless pipeline, FP16 (claimed 2-3x faster on 2:1 FP16 hardware), about 30% of the previous memory, Vulkan 1.2, and a `lsfg-vk-cli` benchmark tool. [V]
- **License history:** MIT, then GPLv3 planned, then **CC BY-NC-ND 4.0 (Aug 2026)**. The project moved off GitHub to self-hosted git.lsfg-vk.dev. Codeberg reportedly dropped the mirror because it does not host NC-ND code. [V, GamingOnLinux]

**Legal status**
- lsfg-vk does not redistribute LSFG shaders; the user supplies the DLL. [V/I]
- It still depends on THS's proprietary compiled code. I could not find whether THS has formally approved or objected: [U].

**For MoonSR**
- **Do not** read, extract, decompile or study Lossless.dll shaders, and do not use lsfg-vk code. Its current license is NC-ND, and its core is THS's proprietary work. [I, policy]

### 1.3 "Apex Compute ANR" / "ANR FG" (ANR Frame Generation)

**Result: no credible public source found.** I cannot describe this product.

Searches tried, all on 2026-11-13:
1. `"ANR Frame Generation" Apex Compute`
2. `"Apex Compute" ANR upscaling frame generation`
3. `"ANR FG" frame generation`
4. `"ANR" "Apex Compute" GPU software`
5. `"Apex Compute" frame generation tool Windows`
6. `ANR frame generation app Steam capture-based "ANR"`

What these searches did surface:
- **Apex Compute** is a real company. Khronos announced it as an associate member; it builds low-power edge AI chips and a GPU architecture targeting Vulkan conformance, prototyped on FPGA. [V, khronos.org]
- No page ties Apex Compute to anything called "ANR", or to any frame generation or upscaling product. [V, negative result]
- "ANR" in search results means Android "Application Not Responding". Nearby acronyms are Arm **NFRU** (Neural Frame Rate Upscaling) and Qualcomm **Adreno Neural Fusion (ANF)**, an SR+FG SDK announced Sep 2026. Neither is evidence that "ANR FG" is the same product. [V that these exist; any link is U]

**Recommendation:** Ask the brief's author for a URL, store page or vendor name. Until then, treat "ANR FG" as **unverified / possibly nonexistent**, and do not use it as a benchmark target.

### 1.4 New capture-based competitors (found during the ANR search)

**Game Frame** (Stone Pit Sons, Steam app 4913010, Early Access from Sep 25, 2026) [V, Steam page]
- Fixed 2x-8x or target-FPS frame generation, "Flow Scale", HUD protection and automatic pacing.
- Upscaling: FSR1, NIS or Bicubic.
- **Requires SDR** (Windows HDR off).
- Has a game-side module for FPS limiting and latency, with an anti-cheat warning.
- Minimum: a "modern integrated GPU". Search snippets also say it "runs on ordinary Direct3D 11 compute shaders" and does not inject. [V, snippet]
- This is the most direct LS clone, and it validates MoonSR's architecture choice.

**DraFG** (Draske, Steam app 5557000, planned for Dec 2026, playtest available) [V, Steam page]
- Captures through OS APIs and presents in its own window or overlay.
- Offers **interpolation and extrapolation** modes, a neural "Surge" model family, multi-frame generation and adjustable interpolation buffering.
- Written in Rust; uses CUDA/Vulkan on Windows and runs on macOS and Linux.
- Minimum GPU is a GTX 1660, RTX 2060 or RX 5600 XT, which suggests a neural approach heavier than LSFG. [I]

**FluidFrames / Flowframes / SVP**
- FluidFrames and Flowframes are offline video tools (RIFE-based), not real-time game tools. [V/M]
- SVP (SmoothVideo Project) is proprietary real-time video interpolation. It uses its own block-based motion estimation, plus optional RIFE through TensorRT/ncnn on capable GPUs. It is relevant as proof that block-matching interpolation runs in real time on old hardware, and of its artifact profile (halo / "soap opera" artifacts at object edges). [M]

### 1.5 Driver-level frame generation

**AMD Fluid Motion Frames (AFMF 2 / 2.1)**
- Driver-level interpolation for DX11/DX12/Vulkan/OpenGL games, in borderless or fullscreen. [V, AMD page and press]
- Requires RX 6000 (RDNA2) or newer, and Ryzen AI 300 APUs. [V, press]
- No dedicated AI hardware. AFMF 2.1 added "Fast Motion Response". [V, IgorsLab]
- Also interpolates the HUD (no UI separation). [V, press, secondary]
- **Key public detail:** AMD says FidelityFX Optical Flow "is based on AMD Fluid Motion Frames". [V, GPUOpen] FidelityFX Optical Flow is therefore the best public window into AFMF-class image-only motion estimation; see Part 2.
- Not available to MoonSR as a component, because it is vendor-locked and driver-internal. [I]

**NVIDIA Smooth Motion**
- Driver-level frame generation, introduced for RTX 50 and extended to RTX 40 (preview driver 590.26, later the NVIDIA App). [V, press]
- Works without game integration. Reported to be less sharp and higher latency than DLSS FG. [V, press]
- Algorithm undocumented; presumably a neural model on tensor cores plus the hardware optical flow accelerator. [I/U]
- RTX 40+ only; not usable by MoonSR. [I]

### 1.6 Magpie (open-source window scaler)

- **License: GPLv3.** [V, GitHub] MoonSR cannot copy Magpie code unless MoonSR itself is GPLv3. Ideas and the public structure of its pipeline are fine to learn from. [I]
- **Requirements:** Windows 10 1903+ or 11, D3D feature level 11. [V]
- **Capture methods** [V, Magpie wiki "Comparison of capture methods"]:

| Method | Pros | Cons |
|---|---|---|
| Graphics Capture (WGC) | Recommended; handles DirectComposition/UWP. | — |
| Desktop Duplication | Handles DirectComposition. | No DPI-virtualization bypass; needs Win10 2004. |
| GDI | Recording and streaming OK. | No DirectComposition; "unstable performance". |
| DwmSharedSurface | Recording, multi-screen and DPI OK. | No DirectComposition; relies on an undocumented DWM API [M]. |

- **Overlay and presentation:** A fullscreen topmost "scaling window" over the source window. Input is mapped back by cursor position transformation. [M]
- **Effect format:** Single-file HLSL compute effects with a custom header (`//!MAGPIE EFFECT`, `//!TEXTURE`, `//!PASS`, `//!PARAMETER` directives), compiled and cached at runtime. Multi-pass, with named intermediate textures. [M]
- **Bundled effects** [V, Built-in effects doc]:
  - ACNet (port of ACNetGLSL)
  - Anime4K family
  - ArtCNN (Ani4Kv2_ArtCNN_C4F32_i)
  - CAS, FSR EASU/RCAS and NIS/NVSharpen
  - CuNNy and CuNNy2
  - FSRCNNX and FSRCNNX_LineArt
  - RAVU and RAVU_Zoom (from bjin/mpv-prescalers)
  - NNEDI3, SGSR and xBRZ 2x-6x
  - Jinc, Lanczos, MMPX, CRT shaders, FXAA/SMAA, deband
- **Frame generation:** None in Magpie. [V, README lists none]

### 1.7 Video interpolators and other open-source capture framegen

- **SVP:** see 1.4.
- **mpv** has no real-time motion interpolation beyond `interpolation=yes` frame blending (tscale). [M]
- I found no mature open-source, capture-based, real-time game frame generator on Windows comparable to LSFG. [V, negative search; DraFG is closed]
- The open references for building one are:
  - FidelityFX Optical Flow + Frame Interpolation (FSR3, MIT) [M on license]
  - RIFE ports (rife-ncnn-vulkan, MIT) [M]

---

## PART 2 - Image-only frame interpolation and motion estimation

### 2.0 Constraints that decide everything

Budget per generated frame at 1080p on a GTX 1060 or RX 580-class GPU (about 4-6 TFLOPS FP32, 192-256 GB/s):
- Target: **≤ 2-3 ms total** for motion estimation and synthesis, so a 2X mode leaves headroom for capture, the upscaler and the game itself. [I]
- At 4K the budget is about the same, which means motion estimation **must** run at reduced resolution. That is exactly what LS "flow scale" does. [I]

**Implications** [I]
- Full-resolution dense neural flow is out of reach on the low tier.
- Pyramidal block matching with refinement is the workhorse.
- Neural models fit at the high tier, or at heavily reduced flow resolution.
- Single full-resolution RGBA8 read or write at 1080p is 8.3 MB; at 4K it is 33 MB. On a 192 GB/s card, each full-res pass costs about 0.05 ms (1080p) or 0.2 ms (4K) in bandwidth alone. Keep full-res passes to roughly 3-5. [I]

### 2.1 Classical motion estimation

| Method | Idea | Real-time feasibility on low/mid GPU | Notes / tag |
|---|---|---|---|
| **Hierarchical block matching (HBM)** | Luma pyramid. At each level, an exhaustive or diamond search per 8x8 block minimizing SAD/census. Upsample vectors, then refine. | **Excellent.** Flow at 1/2-1/4 res gives about 0.2-0.6 ms at 1080p on a GTX 1060-class GPU. [I] | This is FidelityFX Optical Flow's design: 8x8 blocks, 7 pyramid levels, 24x24 window / 256 candidates SAD, 3x3 "middle-vector" filter, 4-candidate upscale refinement, max motion 512 px, scene-change detection by 9-region histograms. Requires SM 6.2, wave ops and `msad4`. [V, GPUOpen] `msad4` exists in SM 5.0, so an SM5 port is feasible without wave ops at some cost. [I/M] |
| **Phase correlation** | FFT cross-power spectrum, peak = global/tile translation. | Good for global or tile-level motion (camera pans); poor for dense flow. | Useful as a **global-motion prior** and pan detector per 64x64 tile. FFT in compute is fine. [M/I] |
| **Lucas-Kanade (pyramidal)** | Local least-squares on gradients in a window. | Good: cheap, sparse or dense. Fails at large motion without a pyramid, and in textureless areas. | Use as the sub-pixel refinement step after block matching. [M] |
| **Horn-Schunck** | Global variational smoothness, iterative. | Many iterations; acceptable at low res only. Oversmooths at boundaries. | Not recommended as primary. [M] |
| **Farneback** | Polynomial expansion per neighbourhood, dense. | Moderate (OpenCV CUDA real-time at 720p-ish). Blurry boundaries. | Superseded by DIS. [M] |
| **DIS (Dense Inverse Search, Kroeger et al. ECCV 2016)** | Coarse-to-fine; sparse grid of patches; each patch aligned by inverse-compositional LK; patch flows densified by weighted averaging; optional variational refinement. | **Very good.** The paper reports hundreds of Hz on a single CPU core at about 1024x436 for the fastest operating point. [M] A GPU port is embarrassingly parallel. | Probably the best classical dense flow for its cost. Apache/BSD-style OpenCV implementation (`cv::DISOpticalFlow`); MoonSR can write a clean HLSL implementation from the paper. [M] |
| **PatchMatch-style flow** | Random init + propagation + random search over nearest-neighbour fields. | Propagation is sequential-ish; GPU variants use jump-flooding or checkerboard propagation. Good at large displacements. | Useful to seed candidates (e.g. jump-flood propagation of good vectors). [M/I] |

**Cost functions**
- **SAD on luma:** Cheapest; `msad4` packs 4 SADs per instruction. [V for FidelityFX use; M for HW]
- **Census / ternary census:** Robust to brightness changes (flashes, fades, HDR tone-mapping changes); about 2-3x SAD cost. [M]
- **Gradient-domain SAD / ZNCC:** Robust to exposure, but more expensive. [M]
- **Recommended:** SAD at coarse levels, census or gradient+SAD at fine levels. [I]

**Regularization and cleanup**
- **Forward-backward consistency:** Compute F(0→1) and F(1→0); pixels where |F01(x) + F10(x + F01(x))| > τ are occluded or unreliable. Costs a 2x motion estimation pass. Standard and essential for occlusion masks. [M]
- **Median / vector-median filtering:** Removes outlier vectors. FidelityFX uses a 3x3 neighbour-consensus pick. [V]
- **Joint bilateral / edge-aware upsampling of flow:** Guided by the full-res luma, so motion boundaries snap to image edges. Critical when flow is estimated at 1/4 res. [M]
- **Temporal flow prior:** Seed the search with last frame's flow, warped. Cuts search cost and improves stability. [I]

**Occlusion reasoning and synthesis**
- **Backward warping:** I_t(x) = blend(I_0(x - t·F_t→0...)). Needs flow *at time t*, which you do not have. The common approximation F_t→0 ≈ -t·F_0→1 sampled at x fails at motion boundaries. [M]
- **Forward splatting:** Push each source pixel to x + t·F, then resolve collisions.
  - Gives correct flow at t, but leaves holes and conflicts.
  - On D3D11, splatting needs atomics on a 32-bit depth-key buffer (e.g. pack depth proxy and pixel index into `InterlockedMax`/`InterlockedMin`), followed by hole filling. [M/I]
- **Softmax splatting (Niklaus & Liu, CVPR 2020):**
  - Splat with weights exp(Z), where Z is an importance metric (e.g. negative brightness-constancy error, or a learned or heuristic depth proxy). Normalized by the summed weights.
  - Differentiable; resolves conflicts softly.
  - The real-time version uses `InterlockedAdd` on fixed-point accumulators (D3D11 has no float atomics), or a two-pass approach: max-key first, then gather. [M/I]
  - Original code is under a research/academic license; the method itself is implementable from the paper. [M — verify]
- **Recommended hybrid (FSR3-style, also inferred for LSFG):**
  1. Forward-splat the block flow to time t to get a "flow at t" field.
  2. Fill holes from neighbours or the opposite direction.
  3. Backward-warp both frames with it.
  4. Blend by a per-pixel confidence from consistency and photometric error.
  5. Prefer the frame that is *not* occluded.

  [I, consistent with public FSR3 FI description, M]

**Pixel-level quality tricks that matter more than the flow method** [I]
- Estimate flow at 1/2-1/4 res, warp at full res.
- Edge-aware flow upsampling.
- Photometric error-based fallback: where warped I0 and warped I1 disagree strongly, blend toward the nearest real frame (bias, not blur). This hides artifacts at the cost of judder.
- Treat the HUD separately (2.6).

### 2.2 Neural interpolation and flow

Cost figures without [V] are estimates; verify with in-project measurement before use.

| Model | Idea | Approx cost (1080p, mid GPU) | Quality | Code / weights license | Training data | Tag |
|---|---|---|---|---|---|---|
| **RIFE 4.x (Practical-RIFE)**, latest full 4.26 (Sep 2024), lite 4.25.lite (Oct 2024) | IFNet: coarse-to-fine *direct* intermediate-flow estimation (to t) plus fusion mask, no pretrained flow net; arbitrary timestep. | About 3-8 GFLOP per frame at 720p scale for lite (est.). Real-time 1080p 2x in SVP/TensorRT on RTX 3060-class reported by users. Likely **10-25 ms on a GTX 1060**, too slow for low tier at full res. [M/I] | Best speed/quality of the classic VFI nets; anime improved in 4.25. | **MIT, weights included under MIT** [V] | Vimeo-90K + undisclosed extra data (training packages "for reference only") [V partial] | [V/M] |
| **IFRNet** (CVPR 2022) | Single encoder-decoder producing intermediate flows and features jointly; S/L variants. | Paper: small variant about 3 M params, fast. [M] Timing in repo measured at 1280x720 [V]. | Good. | **MIT** [V] | Vimeo-90K, GoPro [V] | [V/M] |
| **FILM** (Google, ECCV 2022) | Scale-agnostic feature pyramid, large motion. | Heavy (tens of ms at 1080p on high-end GPU). [M] | Excellent large motion. | **Apache-2.0**; **repo archived Oct 2025** [V] | Vimeo-90K [V] | [V] |
| **AMT** (CVPR 2023) | All-pairs multi-field transforms (RAFT-like correlation), multiple flow fields. | Moderate-heavy. [M] | Very good. | **CC BY-NC 4.0, non-commercial** — **unusable for commercial MoonSR** [V] | Vimeo-90K, GoPro [V] | [V] |
| **EMA-VFI** (CVPR 2023) | Inter-frame attention extracts motion and appearance jointly; hybrid CNN/Transformer. | Heavy (attention); small variant exists. [M] | SOTA-tier in 2023. | **Apache-2.0** (plus inherited licenses of RIFE/IFRNet/PvT/Swin code) [V] | Vimeo-90K [V] | [V] |
| **GMFSS / GMFSS_Fortuna** | GMFlow + softmax splatting + synthesis net, anime-focused. | Heavy (transformer flow), not real-time on mid GPUs. [M/I] | Excellent on anime. | **MIT** [V] | Undisclosed [U] | [V/U] |
| **M2M (Many-to-Many splatting, CVPR 2022)** | Estimates bidirectional flow once, then splats multiple motion vectors per pixel for *arbitrary many* timesteps cheaply. | Flow cost amortized across N outputs: the very property needed for 3X-6X. [M] | Good. | [U — check repo] | Vimeo-90K [M] | [M] |
| **PWC-Net** | Pyramid, warping, cost volume. | About 9 M params; real-time on high-end. [M] | Older. | **NVIDIA source code license, non-commercial** (original). [M — verify] | FlyingChairs/Things, Sintel | [M] |
| **RAFT / RAFT-small** | All-pairs correlation + recurrent GRU updates. | Small variant about 1 M params but many iterations; tens of ms at 1080p. [M] | High. | **BSD-3** [M] | Chairs/Things/Sintel/KITTI | [M] |
| **SEA-RAFT** (ECCV 2024) | Simplified RAFT, mixture-of-Laplace loss, rigid-flow pretraining; "at least 2.3x faster" than prior methods. | Still too heavy for low tier. [I] | SOTA accuracy 2024. | **BSD-3-Clause** [V] | TartanAir, Things, Spring, etc. [M] | [V/M] |
| **NeuFlow v1/v2** | Very efficient flow (global matching at 1/16 + local refinement). v2: ">20 FPS at 512x384 on Jetson Orin Nano", "10x-70x" faster than SOTA at similar accuracy. | On a desktop mid GPU, likely **~2-5 ms at 960x540** flow res (est.) — the most plausible neural-flow candidate for the mid tier. [I] | Near RAFT-class on benchmarks (claimed). | **Apache-2.0** [V] | FlyingThings, Sintel, mixed [V] | [V/I] |

**Game-specific, mostly needs engine data (not directly applicable)**

| Method | Inputs | Applicability | Tag |
|---|---|---|---|
| **ExtraNet** (SIGGRAPH Asia 2021) | G-buffers (depth, normals, motion vectors), extrapolation. | Requires engine data; ideas (occlusion-motion, history) only. | [M] |
| **ExtraSS** (SIGGRAPH Asia 2023) | G-buffers + jitter; joint extrapolation + SR. | Not applicable. | [M] |
| **GFFE** (Intel/UCSB, TOG 2024) | "G-buffer free" extrapolation from previous frames; heuristic extrapolation block (dynamic-fragment motion, disocclusion types) + lightweight shading-correction net. Figure caption: GFFE 6.62 ms vs DMVFN 20.57 ms vs UPR 43.04 ms. | Abstract does not say whether engine motion vectors/depth are used. Paper describes "dynamic fragments", which suggests it still uses renderer motion vectors [I]. **Must read full PDF before assuming capture-compatibility** [U]. | [V partial] |
| **Mob-FGSR** (SIGGRAPH 2024) | Mobile frame gen + SR with engine motion vectors + depth. | Not applicable without engine data. | [M] |
| **STSSNet / spatial-temporal super-sampling** (2024) | G-buffers. | Not applicable. | [M] |
| **DMVFN** (CVPR 2023) | Image-only *video prediction* (extrapolation), dynamic multi-scale voxel flow. | Applicable in principle (image-only), but ~20 ms per GFFE figure. | [V via GFFE caption / M] |
| **Arm NFRU** | Neural frame-rate upscaling: optical flow, then frame generation, then pacing; Unreal plugin; Model Gym training. | Engine-integrated on Arm; architecture reference only; check license. | [V existence / U license] |
| **Recent VFI (2025-2026)** | SG-RIFE (Dec 2025) adds semantic guidance to RIFE, near-real-time. RDVFI (ICLR 2026) uses one-step diffusion, 17 FPS at 1024x576. ANVIL (Mar 2026) is mobile-NPU VFI using codec motion vectors. | None is a drop-in real-time option for mid-tier GPUs at 1080p+. | [V, search summaries] |

**Dataset caveat**
- Almost every VFI model is trained on **Vimeo-90K**: clips scraped from Vimeo with per-video creator licenses and no blanket commercial grant. [M]
- Code may be MIT or Apache, but the **weights inherit unclear data rights**. This is a legal grey area; commercial products (e.g. SVP, Flowframes) ship RIFE weights anyway.
- Recommendation: if shipping neural weights, prefer training our own on licensed or synthetic data. Game-engine rendered sequences are ideal and also match the domain. Otherwise, record the risk and get legal sign-off. [I]

### 2.3 Multi-frame generation (2X-6X, adaptive)

**Approach**
- **Arbitrary-timestep from one pair:** Estimate flow F01 (and F10) **once** per real-frame pair. For each t in {1/N, ..., (N-1)/N}, run only the cheap splat/warp/blend at time t. M2M-style. [M/I]
- Flow cost is amortized, so 4X costs about flow + 3x synthesis, not 3x flow. Adaptive (fractional) mode just picks t from the presentation timestamp. [I]

**Why not recursive interpolation**
- Interpolating at 0.5 and then 0.25 between the original and the *generated* frame feeds artifacts back as inputs.
- Errors compound: halos double, and occlusion holes get filled with hallucinated content that is then treated as truth.
- It also wastes flow passes and forces power-of-two multipliers. [M/I]

**Motion model**
- **Linear (constant velocity between two frames):** Standard; fine at ≥ 40-60 base FPS.
- **Quadratic / acceleration (QVI, Xu et al. NeurIPS 2019; uses 4 frames):** Better on curved paths, rotations and acceleration at low base FPS.
- **Cost:** One extra flow pass (or reuse of the previous pair's flow from history, which is free). It adds no latency beyond the pair already needed, since history frames are past frames. [M/I]
- **Recommendation:** Linear by default. Quadratic correction from cached F(-1→0), and F(0→1) as an optional quality mode, gated by flow-consistency confidence. [I]

**Extrapolation vs interpolation**
- Extrapolation (DraFG offers it; GFFE is extrapolation) avoids the one-frame hold latency, but disocclusions have no source data. [V for DraFG offering / I]
- Recommend interpolation first, with extrapolation as an experimental low-latency mode. [I]

**Pacing**
- Generated frames must be presented at exact fractions of the real-frame interval.
- Needs the capture timestamps (WGC `SystemRelativeTime`, DXGI `LastPresentTime`) and an estimate of the real frame interval (median filter of deltas). [M/I]

### 2.4 HUD / static overlay detection (image-only)

All of the following is engineering inference [I] unless noted.

**Static-pixel mask**
- Per pixel, a temporal counter of "unchanged" frames: |I_t - I_{t-1}| < ε, over K frames, plus low flow magnitude in the surrounding area while the background moves.
- HUD pixels are static in *screen space* while the surrounding scene moves.

**Edge/alpha cues**
- HUD elements have hard edges, saturated or flat colours and text-like gradients.
- A cheap classifier: high local contrast + static + spatially coherent rectangles. Use connected components on a coarse 16x16 tile grid.

**Treatment**
- Where the mask is set, take the nearest real frame (or a blend of I0/I1 by t) **without warping**.
- Force zero flow for HUD blocks during motion estimation, so their vectors don't contaminate the background.

**Risks**
- Semi-transparent HUD over moving background: no image-only solution is perfect. Use soft masks and favour non-warped content there.
- Crosshairs: static and small, so the counter must work at pixel or 4x4 granularity.

**Evidence the competition works this way**
- LS 3.2's notes mention "refined UI detection", and Game Frame advertises "HUD protection". Their methods are undocumented. [V that they exist / U how]

### 2.5 Scene-cut detection

- FidelityFX OF: luma histograms over 9 screen regions compared to the previous frame against a threshold. On a cut, return zero motion. [V]
- Add mean-SAD of the coarsest pyramid level and the fraction of blocks with high best-match cost. [I]
- On a cut, repeat the nearest real frame (or a short crossfade at most). Never warp. [I]
- Also handle "same frame captured twice" (game FPS < capture FPS). Detect exact duplicates by hash or zero-SAD, and drop them from the real-frame timeline. Otherwise flow is zero and pacing breaks. [I]

---

## PART 3 - Image-only upscaling

### 3.1 Spatial (non-learned)

| Method | Notes | License | Tag |
|---|---|---|---|
| Bilinear, bicubic (Mitchell-Netravali B=C=1/3; Catmull-Rom B=0,C=0.5), Lanczos-2/3, Jinc/EWA | Separable or 2D kernels. Lanczos/Catmull-Rom ring; anti-ringing clamps to the 2x2 neighbourhood min/max. Cost < 0.1-0.2 ms at 4K. | Public-domain math | [M] |
| **FSR 1 (EASU + RCAS)** | Edge-adaptive Lanczos-like 12-tap + robust contrast-adaptive sharpening. Very cheap. | **MIT** (FidelityFX) | [M] |
| **NIS** | 6-tap directional scaler + sharpening. | **MIT** (NVIDIA Image Scaling SDK) | [M] |
| **SGSR v1** (Snapdragon Game Super Resolution) | Single-pass, 12-tap, edge-direction-aware; mobile-cheap. | BSD-3-Clause (Qualcomm) | [M — verify] |
| **NEDI / EDI** | Covariance-based edge-directed interpolation; quality good for 2x, cost moderate (local 4x4 least squares per pixel). | Algorithm (Li & Orchard 2001) unencumbered by license | [M] |
| **Integer scaling** | Nearest-neighbour by integer factor with centering; essential for pixel art. | — | [M] |
| **xBR** (Hyllian) | Edge-detection rule-based pixel-art scaler. | **MIT** (Hyllian's shaders, libretro) | [M — verify] |
| **xBRZ** (Zenju) | Improved xBR, 2-6x. | **GPLv3** (with an author exception clause in some headers) | [V] |
| **hqx** | Lookup-table pattern scaler. | **LGPL-2.1** | [V] |
| **ScaleFX** (Sp00kyFox) | Scale3x derivative with edge-level detection, 9x. | Not found [U]; libretro shaders vary | [U] |
| **MMPX** | Pixel-art scaler (Morgan McGuire, 2021). | MIT [M] | [M] |
| **Sharpening** | CAS (MIT), RCAS (MIT), NVSharpen (MIT), unsharp-mask, adaptive sharpen. Apply after upscaling in a linear or perceptual space. Clamp to avoid halos. | | [M] |

### 3.2 Temporal SR without engine data

**Is jitterless temporal SR from a captured stream feasible?** [I]
- **Partially.** Temporal SR recovers detail only when consecutive frames sample the scene at *different sub-pixel offsets*.
  - Game cameras move continuously, so sub-pixel motion exists naturally. Static scenes, though, give no new information.
  - Most games already run TAA. Their output is temporally resolved and spatially soft, so there is little aliasing left to exploit.
- What *is* feasible and valuable is **temporal stabilization** of a spatial or neural upscaler: reproject the previous *output* using our own flow and blend with clamping, as TAA does.
  - This removes shimmer from sharpening and neural SR and gives modest detail accumulation.
  - It reuses the frame generation flow at no extra estimation cost.
  - Risk: ghosting where flow is wrong. Mitigate with neighbourhood clamping and flow confidence.

**Video SR literature**
- **BasicVSR / BasicVSR++ / IconVSR:** Bidirectional recurrent propagation with flow alignment (SpyNet) and, in BasicVSR++, deformable alignment. [M]
  - Cost: hundreds of GFLOPs per frame at 1080p output; far beyond 2 ms. [M/I]
  - Bidirectional propagation also needs future frames, so it is not causal.
  - License: Apache-2.0 for BasicSR/MMagic code. [M]
- **Real-time VSR** (e.g. RealBasicVSR, compact recurrent nets, mobile VSR challenge entries): Unidirectional, tiny; some reach real-time at 720p on high-end GPUs. Still above budget on low-end. [M]
- **RTX Video Super Resolution / AMD Video Upscale / Intel VSR:** Vendor-locked neural spatial (or lightly temporal) upscalers in the video pipeline. Algorithm undocumented: [U]. They show that a small CNN at 1080p→4K runs in real time on tensor hardware. [M]

**Realistic in < 2 ms on a midrange GPU** (RX 6600 / RTX 3060-class, about 10-13 TFLOPS FP32) [I]
- FSR1/NIS/SGSR-class spatial: < 0.3 ms at 4K output.
- A tiny CNN on luma at the *input* resolution, plus pixel shuffle (ESPCN-like or ArtCNN C4F16/C4F32, CuNNy "fast"): roughly 0.5-2 ms for 1080p→4K, depending on channel count and FP16 or packed int8.
- A temporal-stabilization pass: about 0.2-0.4 ms.

### 3.3 Lightweight real-time neural SR

| Model | Idea / size | Shipped as shader by | License | Tag |
|---|---|---|---|---|
| **ESPCN** (Shi et al. 2016) | 3 conv layers at LR resolution + sub-pixel (pixel-shuffle) output. About 20-60 K params. | Common in research | Paper-only; reimplement freely | [M] |
| **FSRCNN / FSRCNNX** | Feature extraction, shrinking, mapping, expanding, deconv. FSRCNNX_x2_8-0-4-1 (igv) is the mpv classic. | mpv (GLSL), Magpie (HLSL port) [V port] | FSRCNNX: MIT [M — verify] | [V/M] |
| **SRVGGNetCompact** (Real-ESRGAN compact / realesr-animevideov3) | Plain VGG-style conv stack + pixel shuffle; about 0.6 M params for 4x anime-video. | ncnn-vulkan portable builds (Windows/Linux/macOS, any vendor) [V] | **BSD-3-Clause** [V] | [V/M] |
| **ArtCNN** | C4F16 about 12 K params, C4F32 about 48 K (real-time); R8F64 about 926 K, R16F96 about 4 M (offline). Luma doubler, anime-focused. | GLSL (mpv) and ONNX [V]; Magpie ships Ani4Kv2_ArtCNN_C4F32_i [V] | **MIT** [V] | [V] |
| **CuNNy** | Tiny CNNs, NxD naming (8x32 down to veryfast). Variants: neutral / SOFT / DS / NVL. 2x only. Optional dp4a int8 path. | mpv GLSL, Magpie effects [V] | **LGPL-3.0** [V]; trained on visual-novel screenshots (data provenance U) | [V] |
| **RAVU** (bjin/mpv-prescalers) | RAISR-style: per-pixel gradient-hash (angle/strength/coherence) selects a learned linear filter from a LUT. Extremely cheap, no CNN. | mpv GLSL, Magpie [V port] | **LGPL-3.0** [M] | [V/M] |
| **NNEDI3** | Small neural predictor (originally deinterlacing) for the missing lines; 2x per axis pass. | mpv prescalers, Magpie [V] | LGPL-3.0 (mpv-prescalers port) [M] | [M] |
| **Anime4K v4** | Line-reconstruction CNN "Restore" + "Upscale" GLSL shader chains for mpv; corrects "distribution shift" in 1080p anime. Variants S/M/L/VL/UL. | mpv GLSL; Magpie [V] | **MIT** [V] | [V] |
| **ACNet** | Anime4KCPP CNN. | Magpie port of ACNetGLSL [V] | MIT [M] | [M] |

**How mpv and Magpie ship neural SR** [M]
- Weights are **baked as constants** into multi-pass GLSL (mpv user shaders: `//!HOOK`, `//!BIND`, `//!SAVE`, `//!COMPUTE`) or HLSL (Magpie `//!PASS`).
- Each pass computes 4-8 channels into RGBA16F textures. Convolutions are unrolled 3x3 loops with FP16 packing where available.
- No ML runtime is needed. This is the right pattern for MoonSR's SM 5.0 baseline: a weights-to-HLSL generator script.
- DirectML/ONNX is an optional fast path for larger models on DX12-capable hardware.

**Licensing summary for shipping in MoonSR (closed source assumed)** [I]
- **Safe to reimplement or ship:** FSR1/CAS/RCAS (MIT), NIS (MIT), Anime4K (MIT), ArtCNN (MIT), Real-ESRGAN compact (BSD-3), RIFE (MIT), IFRNet (MIT), EMA-VFI (Apache-2.0), FILM (Apache-2.0), NeuFlow v2 (Apache-2.0), SEA-RAFT (BSD-3).
- **LGPL** (CuNNy, RAVU, hqx, NNEDI3 port): Shipping baked weights inside our own shaders creates derivative-work questions. Get a legal review or retrain.
- **GPL** (Magpie, xBRZ): Do not incorporate.
- **Non-commercial** (AMT, PWC-Net original, lsfg-vk current): Do not use.

---

## Recommendations

### (a) Frame generation algorithm stack, ranked by tier

All tiers share the following [I]:
- Capture timestamps and duplicate-frame rejection.
- Scene-cut detection (histogram + coarse SAD).
- A HUD/static mask.
- Single flow estimate per real-frame pair, reused for all N-1 intermediate timesteps (no recursion).
- Forward-splat-to-t, then backward-warp both frames, then confidence blend, with fallback to the nearest real frame.
- Interpolation by default; adaptive mode chooses t from presentation time.

**Low-end** (GTX 10 / RX 400-500 / Intel UHD-Iris Xe; SM 5.0, no wave ops)

| Rank | Option | Details |
|---|---|---|
| 1 | Pyramidal block matching at flow scale 0.25-0.5 | 8x8 blocks, SAD via `msad4`, 3-5 levels; candidates from the temporal prior, spatial neighbours and a global phase-correlation vector. 3x3 vector-median, edge-aware flow upsampling, one-directional flow with photometric confidence only (skip the backward pass). Est. 0.5-1.5 ms at 1080p output on GTX 1060. [I] |
| 2 | Same, plus forward-backward consistency | Better occlusion handling at about 1.6x the motion estimation cost. |
| 3 | Fallback | Frame blending is the minimum for adaptive mode on very weak iGPUs. LS ships a separate low-power model for the same reason. [V that LS does] |

**Midrange** (GTX 16/RTX 20-30/RX 5000-6000/Arc A)

| Rank | Option | Details |
|---|---|---|
| 1 | Block matching + DIS-style refinement | Bidirectional block matching with census cost at fine levels, plus DIS-style inverse-search sub-pixel refinement at flow scale 0.5. Full forward-backward occlusion masks. Softmax-splat synthesis (fixed-point atomics). Optional quadratic motion from history. [I] |
| 2 | Neural flow | NeuFlow v2 (Apache-2.0) at about 960x540 flow resolution via DirectML/ONNX, paired with the same classical synthesis. Only after in-project measurement shows < 3 ms. [I] |
| 3 | Small neural refinement net | A net (IFRNet-S-like, our own weights) that cleans warped and blended outputs at reduced resolution. [I] |

**High-end** (RTX 40-50 / RX 7000-9000 / Arc B)

| Rank | Option | Details |
|---|---|---|
| 1 | RIFE 4.25-lite-class model or our own retrained IFNet-style model | Via DirectML (FP16) at flow scale 0.5-1.0, arbitrary timestep. MIT code. Prefer our own training data to remove the Vimeo-90K weight risk. [V license / I perf] |
| 2 | Midrange stack at flow scale 1.0 with bidirectional quadratic motion | |
| 3 | Experimental extrapolation mode | Low latency; GFFE-inspired heuristics, image-only. Read the full GFFE paper first. [U applicability] |

### (b) Upscaler stack, ranked

| Rank | Option | Details |
|---|---|---|
| 1 | **Own edge-adaptive spatial scaler** (the default "MoonSR1") | FSR1/SGSR-class directional kernel with anti-ringing clamp + RCAS-like adaptive sharpening. All tiers, < 0.3 ms at 4K. FSR1/NIS/CAS are MIT and can be shipped as alternatives with attribution. [M license / I perf] |
| 2 | **Tiny CNN luma doubler as baked HLSL** | ArtCNN-C4F16/C4F32-class (MIT) or our own retrained ESPCN/SRVGG-compact on game content. Bilinear chroma; pixel-shuffle; FP16. Mid/high tier. Target ≤ 1.5 ms at 1080p→4K on an RTX 3060/RX 6600. [I] |
| 3 | **Temporal stabilization pass** | Reuses the frame generation flow on the upscaled output; neighbourhood clamping. Mid/high. [I] |
| 4 | **Content modes** | Integer and nearest; xBR (MIT, verify) / MMPX for pixel art; Anime4K (MIT) or ArtCNN for anime. [V/M] |
| 5 | **Optional DirectML path** | Larger models such as SRVGGNetCompact (BSD-3) on high-end GPUs. [V license] |
| — | **Do not ship** | xBRZ (GPL), Magpie code (GPL), or LGPL CuNNy/RAVU weights without legal review. |

### (c) Unverified / unknown

- **"Apex Compute ANR" / "ANR FG":** No source found (6 searches listed in 1.3). Apex Compute exists as a GPU/AI-chip startup, but no link to any ANR frame generation product was found. Treat it as unverified.
- **LSFG internals:** Algorithm, network or not, flow method, LS1 scaler algorithm: not public. Exact semantics of LS "Queue target", "Max frame latency" and sync modes: not found in primary docs. HDR behaviour in LS 3.x: not verified. The WGC-requires-24H2 claim is from a community guide and is suspicious.
- **lsfg-vk:** Whether THS endorses it; exact shader-extraction mechanism.
- **AFMF / Smooth Motion algorithms:** Undocumented beyond AMD's statement that FidelityFX OF is based on AFMF.
- **GFFE inputs:** Whether engine motion vectors are required. The full paper was not read.
- **Licenses not re-verified this session:** FidelityFX SDK (believed MIT), NIS (MIT), SGSR (BSD-3), xBR shaders (MIT), ScaleFX (unknown), FSRCNNX (MIT?), RAVU and NNEDI3 ports (LGPL-3.0), M2M, softmax-splatting reference code, PWC-Net (NVIDIA non-commercial), BasicVSR/MMagic (Apache-2.0), Arm NFRU model license.
- **Performance figures:** All per-GPU millisecond estimates in this file are **inferred**, not measured. NeuFlow v2's only published speed is on a Jetson Orin Nano at 512x384. RIFE real-time claims are from user reports. Every candidate needs an in-project benchmark before tier assignment.
- **Vimeo-90K commercial usability of derived weights:** A legal question; not answered here.
- **Game Frame:** The "D3D11 compute shaders" claim is from a search snippet; the capture method is not stated on the store page. **DraFG:** Model architecture undocumented.

### (d) Sources

**Lossless Scaling and LSFG**
- Lossless Scaling Steam page: https://store.steampowered.com/app/993090/Lossless_Scaling/
- LS 3.1 AFG / Flow Scale (TechEnclave): https://techenclave.com/t/lossless-scaling-3-1-adds-adaptive-frame-generation/273906
- LS 3.1 / performance mode (Tom's Hardware): https://www.tomshardware.com/pc-components/gpus/new-lossless-scaling-update-can-reduce-gpu-load-by-2x-version-3-1-could-be-the-most-potent-fsr-dlss-alternative-yet
- Notebookcheck on LSFG perf mode: https://www.notebookcheck.net/New-Lossless-Scaling-update-promises-2x-performance-boost-and-visual-enhancements.1038120.0.html
- LSFG 3.0 latency (Tom's Hardware): https://www.tomshardware.com/video-games/pc-gaming/lossless-scaling-3-update-touts-greatly-improved-latency-and-performance-universal-frame-gen-tool-boasts-24-percent-reduced-latency
- LSFG 2.0 (Wccftech): https://wccftech.com/lossless-scaling-universal-frame-generation-2-0-out-now-improves-efficiency-noticeably-reduces-artifacts/
- LS 3.2 official discussion (Reddit mirror): https://lr.in.psf.lt/r/losslessscaling/comments/1l8w2uj/official_discussion_lossless_scaling_32_release
- Dual-GPU official guide (Reddit mirror): https://reddit.birdcat.cafe/r/losslessscaling/comments/1jtaoau/official_dual_gpu_overview_guide/mqm8jmy/?context=3
- 4X mode first look (Digital Trends): https://digitaltrends.com/computing/lossless-scaling-4x-frame-generation-first-look
- IgorsLab on LS architecture: https://www.igorslab.de/en/lossless-scaling-scaling-without-blur-frame-generation-without-hardware-binding/
- Steam discussion on DXGI capture: https://steamcommunity.com/app/993090/discussions/0/4209246056405471570/?ctp=2
- Notebookcheck modding guide (May 2026): https://www.notebookcheck.net/You-Should-Be-Modding-Lossless-Scaling-Guide.1295503.0.html

**lsfg-vk**
- License change and move: https://www.gamingonlinux.com/2026/08/lossless-scaling-frame-generation-on-linux-lsfg-vk-moves-away-from-github-changes-license-due-to-ai-slop-forks/
- v2.0 release: https://www.gamingonlinux.com/2026/09/lossless-scalings-frame-generation-for-linux-lsfg-vk-2-0-is-out-now/
- v3.1 support: https://www.gamingonlinux.com/2025/07/lossless-scalings-frame-generation-for-linux-gets-upgraded-to-the-latest-v3-1
- LinuxLinks overview: https://www.linuxlinks.com/lsfg-vk-lossless-scaling-frame-generation/

**ANR search and other competitors**
- Apex Compute (Khronos): https://www.khronos.org/news/permalink/khronos-group-welcomes-apex-compute-as-associate-member
- Qualcomm Adreno Neural Fusion: https://www.qualcomm.com/developer/blog/2026/09/introducing-adreno-neural-fusion-sdk-for-snapdragon-mobile-platforms
- Arm NFRU: https://huggingface.co/Arm/neural-frame-rate-upscaling and https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/introducing-neural-frame-rate-upscaling-in-arm-neural-graphics-sdk-for-game-engines-and-ue-plugins
- Game Frame: https://store.steampowered.com/app/4913010/Game_Frame/
- DraFG: https://store.steampowered.com/app/5557000
- FluidFrames: https://store.steampowered.com/app/3228250

**Driver-level frame generation**
- AMD AFMF: https://amd.com/en/products/software/adrenalin/afmf.html
- AFMF 2.1 (IgorsLab): https://www.igorslab.de/en/amd-expands-afmf-2-1-with-fast-motion-response-finally-an-answer-to-fast-game-scenes/
- NVIDIA Smooth Motion on RTX 40 (Tom's Hardware): https://www.tomshardware.com/pc-components/gpu-drivers/nvidias-new-driver-update-finally-brings-smooth-motion-to-rtx-40-series-gpus-works-like-amds-fluid-motion-frames-and-claims-to-double-your-fps-with-a-single-click-in-any-game
- Smooth Motion (Wccftech): https://wccftech.com/nvidia-adds-smooth-motion-a-driver-based-frame-generation-ai-model-for-rtx-50-gpus/

**Magpie**
- Repository: https://github.com/Blinue/Magpie
- Capture methods: https://github.com/Blinue/Magpie/wiki/Comparison%20of%20capture%20methods
- Built-in effects: https://github.com/Blinue/Magpie/blob/main/docs/Built-in%20effects.md

**Motion estimation**
- FidelityFX Optical Flow: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/
- D3D12 motion estimator: https://learn.microsoft.com/en-us/windows/desktop/api/d3d12video/ns-d3d12video-d3d12_feature_data_video_motion_estimator_size

**Neural interpolation and flow**
- RIFE: https://github.com/hzwer/Practical-RIFE
- IFRNet: https://github.com/ltkong218/IFRNet
- FILM: https://github.com/google-research/frame-interpolation
- AMT: https://github.com/MCG-NKU/AMT
- EMA-VFI: https://github.com/MCG-NJU/EMA-VFI
- GMFSS_Fortuna: https://github.com/98mxr/GMFSS_Fortuna
- SEA-RAFT: https://github.com/princeton-vl/SEA-RAFT
- NeuFlow v2: https://github.com/neufieldrobotics/NeuFlow_v2 and https://arxiv.org/abs/2408.10161

**Game frame generation research**
- GFFE: https://arxiv.org/abs/2406.18551 and https://www.intel.com/content/www/us/en/developer/articles/technical/gffe-g-buffer-free-frame-extrapolation.html
- Mob-FGSR paper: https://sites.cs.ucsb.edu/~lingqi/publications/paper_sig24mobfgsr.pdf
- Recent VFI: https://arxiv.org/html/2603.26835v2 (ANVIL), https://arxiv.org/pdf/2512.18241v1 (SG-RIFE), https://mlanthology.org/iclr/2026/ma2026iclr-realtime/ (RDVFI)

**Upscaling**
- Anime4K: https://github.com/bloc97/Anime4K
- Real-ESRGAN: https://github.com/xinntao/Real-ESRGAN
- CuNNy: https://github.com/funnyplanter/CuNNy
- ArtCNN: https://github.com/Artoriuz/ArtCNN
- xBRZ: https://sourceforge.net/projects/xbrz/
- hqx: https://wikipedia.com/wiki/Hqx

**From model memory, not fetched this session** (for follow-up verification)
- DIS optical flow paper (Kroeger et al. 2016): https://arxiv.org/abs/1603.03590
- Softmax splatting: https://arxiv.org/abs/2003.05534
- M2M: https://arxiv.org/abs/2204.03513
- QVI: https://arxiv.org/abs/1911.00627
- FidelityFX SDK: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- NIS SDK: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- mpv-prescalers: https://github.com/bjin/mpv-prescalers
