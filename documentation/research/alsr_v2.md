# Moon ALSR v2: audit of v1, survey of capture-only temporal super-resolution, and a v2 plan

Author role: graphics-researcher. Date: 2026-11-21. Scope: groundwork for a higher-quality Moon ALSR (MoonSR's own
temporal upscaler). No engine code is written here.

## Provenance legend (read first)

| Tag | Meaning |
|---|---|
| **[C]** | Verified by reading this repository's code at commit `814bc0d` (file named). |
| **[H]** | Historical measurement recorded in `benchmarks/reference/alsr_progress.md` or `documentation/BENCHMARKS.md`. These numbers were produced by `moonsr_tbench`, which has since been deleted; **they cannot be reproduced from this repository**, and they come from different harness revisions and frame ranges, so compare numbers only within one table. All are synthetic-sequence results unless marked "real". |
| **[V]** | Verified this session from the cited primary source (accessed 2026-11-21). |
| **[M]** | From the researcher's prior knowledge of the cited primary source, **not re-fetched this session**. Web access was interrupted by a usage limit partway through; after it reset, the most important items were fetched and upgraded to [V]. The rest still carry [M]. The URLs are the canonical locations; check them before relying on a detail. |
| **[I]** | Inference or engineering reasoning. |
| **[E]** | Cost or quality **estimate**. It is not a measurement and must not appear in `BENCHMARKS.md`. |
| **[U]** | Unknown or not publicly documented. |

---

## Part 1: Audit of Moon ALSR v1

### 1.1 Inputs available to ALSR [C]

- **Frames.** `FrameGenerator::RealFrame(0/1)` returns BGRA8 frames at game resolution. ALSR reads only frame 0. The previous frame's working copy is kept from the last `Render` (`alsr_upscaler.cpp`).
- **Motion.** `MotionFieldView` (`src/framegen/frame_generator.h`) is a Texture2DArray RGBA32F field with one texel per motion cell. Slice 0 is prev→curr and slice 1 is curr→prev. Each texel holds xy in frame pixels, z = confidence (forward-backward consistency × match quality) and w = normalised residual.
  - Cell size is `kCell / flowScale` = 4 / 0.5 = **8 frame pixels** at the default flow scale (`frame_generator.cpp`, `kCell = 4`). At 720p that gives 160×90 = 14,400 cells.
  - The estimator does hierarchical 8×8 block matching (zero-mean SSD plus census), a temporal candidate, repeating-texture disambiguation and parabolic sub-pixel (`fg_search_cs.hlsl`).
- **Other signals.** A full-resolution R8 static-overlay (HUD) mask and a GPU-side scene-cut flag.
- **Not available.** There are no engine motion vectors, no depth and no jitter.
- **When FG runs on another GPU,** ALSR runs its own estimator on the main GPU (`KNOWN_ISSUES.md`).

### 1.2 Pipeline, pass by pass

Costs are RTX 5060, 1280×720 → 1920×1080, isolated GPU time [H]. "s2b" is from Stage 2b (2026-11-15). The Stage 3f per-pass breakdown was not recorded after the last cost cuts. Only pass-level numbers quoted in the progress log are given.

| # | Pass (shader) | Res | Inputs | What it decides | Cost [H] |
|---|---|---|---|---|---|
| 0 | `alsr_prep_ps` | src | frame, previous working copy, HUD mask, previous overlay evidence | Linear-light RGBA16F working copy; 5×5 binomial-blurred luma (9 bilinear taps) used for matching; persistent **overlay evidence** counter (+32/255 per unchanged frame with mask, −8 without, reset on change) | 0.045 ms |
| 1 | `alsr_cellrefine_cs` | cells | blurred luma (curr), registration reference (from previous **history**), estimator curr→prev vectors, previous refined cells | Per cell: 2 Gauss-Newton Lucas-Kanade steps over a 16×16 Gaussian-weighted window (two cells wide), brightness offset eliminated (Schur complement), step ≤ 1 px. Then a **velocity filter** blends with the previous refined vector of the same content, using age-based weights and a reset on disagreement | 0.075 ms (was 3.31 as a PS, 0.47 in Stage 2b) |
| 2 | `alsr_motion_ps` | src | refined cells, blurred luma, reference, estimator prev→curr, HUD evidence, scene cut | Per pixel: bilinear cell vector if the 3×3 cells agree within 0.5 px. Otherwise selection among 11 candidates (bilinear, 9 cells, zero) by a 5-tap zero-mean photometric error. Visibility from the forward-backward test with τ = 1.5 + 0.1|v|. Confidence = cell conf × vis × match ramp. Snaps |v| < 0.02 px and unchanged pixels to 0; HUD → zero motion, conf 1. Also writes the overlay layer and boundary ring (MRT1) | 0.16 ms (s2b) |
| 3 | `alsr_reproject_ps` | out | history, per-pixel motion, working copy, layer | History by Catmull-Rom with a 2×2 deringing clamp. **Rectification**: ray-clip in YCoCg onto the 3×3 source min/max hull + 25 % margin, shrunk towards the mean in motion. **Trust** = smoothstep(0.1, 0.5, conf) × inside × no-cut × HUD tests; ring → 0. Writes trust×history and a D16 "needs seed" mask | 0.31 ms (s2b, before seed fusion) |
| 3b | `alsr_seedblend_ps` | out | trust, frame, layer, filter table | Only where trust < 1 (early depth test): adds (1 − trust) × **seed** (MoonSR Spatial + optional learned RAISR-style bank; on the HUD ring, a layer-separated normalised-convolution cubic) | 0.025–0.045 ms |
| 4 | `alsr_residual_ps` | src | Hw, working copy, layer | Residual of each new sample against the prediction from the reprojected history under an **area** sampling model (2×2 box at 2×) | 0.036 ms (s2b) |
| 5 | `alsr_update_ps` | out | Hw, residual, motion, working copy | Recursive **iterative back-projection**: H = clamp(Hw + g·CR(residual), 3×3 source hull ± 25 %). g = 0 on fresh pixels and on zero-motion pixels unless the input changed in place; otherwise max(1/(n+1), gMin), boosted on large residual. Count cap 16. Can write the destination directly when sharpness = 0 | 0.14 ms (s2b) |
| 6 | `alsr_final_ps` | out | gamma copy | Contrast-adaptive unsharp clamped to the 3×3 min/max; debug views | 0.05 ms |
| 7 | `alsr_refy` + `alsr_refluma` | src | history | Registration reference = blurred luma of the new history on the source grid (exact 5×5 binomial) | 0.11 ms (s2b) |
| 8 | Collection (CPU learner) | – | 240×240 tile | Non-blocking staging readback; worker solves ridge-regularised RAISR filters per phase × 72 geometry buckets, with validation and transfer gates | 0.002 ms GPU; worker about 25 ms per tile |

**Totals [H], Stage 3f, RTX 5060:**
- 720p → 1080p: Render **0.67–0.82 ms**, depending on workload and sharpness.
- 1080p → 1440p: 1.36–1.70 ms.
- The motion estimator (`PushFrame`, shared with FG) is **extra**: 0.79 ms at 720p and 1.63 ms at 1080p input.
- RTX A400: about 7× slower per pass (Stage 2b: 11.1 ms at 720p → 1080p). Radeon iGPU (iteration 1, before the cost cuts): 64 ms.
- Memory [C]: 98 MiB for 720p → 1080p.

### 1.3 Weaknesses, with evidence

**W1. A weak single-frame seed sets the quality floor, including on static scenes.**
- [C] A pixel whose motion is exactly zero gets gain 0 in `alsr_update` (no new sample phase). A static scene therefore shows the seed from its first frame forever.
- [H] Iteration 1, aliased 1.5×, static: ALSR 27.97 dB = MoonSR Spatial 27.97, versus Lanczos-3 **29.31** (−1.34 dB).
- [H] Across every sequence of that table, Lanczos-3 beats MoonSR Spatial by 1.3–1.6 dB at 1.5× aliased.
- [I] The same seed is used on disocclusions, after scene cuts, on the first frame, on the HUD ring, and for all **generated** frames (`RenderSingle`). Seed quality therefore affects a large share of displayed pixels whenever FG is on.

**W2. Thin features and 1-pixel detail are lost (rectification is too strict for aliased input).**
- [C] History is clipped to the 3×3 hull of the *current* frame's samples (`alsr_reproject`), and the update is clamped to the same hull (`alsr_update`).
- [I] On point-sampled input a 1-pixel feature can be missing from the current frame's samples entirely, since it falls between sample centres. The hull then excludes it, and accumulated detail is clipped away every frame.
- [H] Recorded defects: "undershoot on thin dark lines, no recovery of 1-px features". thin_features at 1.5× aliased: 16.93 dB vs Lanczos 17.65.
- [I] FSR 2 adds its "lock" mechanism for exactly this problem (see 2.3). v1 has no equivalent.

**W3. Motion is modelled at cell resolution, as translation only.**
- [C] Motion is one translation per 8-pixel cell, refined by LK over a 16×16 window.
- Per-pixel motion is either bilinear interpolation of cells or a choice among 11 cell-level candidates. There is no per-pixel sub-pixel refinement and no rotation or scale model.
- [H] zoom_rotate at 1.5× aliased: −1.51 dB vs Lanczos (iteration 1), the worst moving sequence.
- [H] Matching aliased frames against each other measured the motion of the aliasing pattern: a mean estimate of (0.07, 0.38) against a true (0.31, 0.22) source px. Registering against the history reference was introduced to counter this, and an inaccurate reference costs up to 0.65 dB.
- [I] Sub-pixel accuracy is what turns natural motion into new samples. An error of a few tenths of a source pixel blurs accumulated detail instead of adding it.

**W4. On anti-aliased (prefiltered) input, the temporal path can be worse than single-frame.**
- [H] Stage 3e, pan_medium prefiltered 1.5×: full ALSR **32.60 dB**, versus the generic single-frame path 33.39 and the learned single-frame path 35.37.
- [I] The likely causes are misregistration blur plus a fixed area residual model. The last is measured: the area model is +1.8 dB on prefiltered static content and −0.2 dB on aliased, so no single fixed model suits both.
- [H] The sampling-model detector exists only in the learner. It chose correctly at 1.5× but **wrongly at 2× prefiltered**.

**W5. Accumulation has a short memory and no explicit per-pixel confidence.**
- [C] The default `stability = 0.85` gives a gain floor gMin = clamp(1.4 − 1.25·0.85, 0.12, 1) ≈ **0.34**, with a count cap of 16.
- [I] With back-projection the gain is a residual-correction weight, not a blend weight, so this is not literally a "3-frame history". It does mean at least a third of every frame's aliasing residual is injected again, which bounds how far the aliasing can be averaged out.
- [C] There is no stored per-pixel "how much evidence supports this output pixel" quantity apart from the count. Trust is recomputed from scratch each frame from motion confidence.

**W6. Flicker has no dedicated stabiliser.**
- [H] Flicker is measured in tbench (for example 4.1 → 5.7 levels on pan_slow aliased when overlay false positives were present). [C] No pass targets temporal luma instability.
- [I] Oscillating aliasing on slowly moving content is where users see shimmer.

**W7. HUD handling has a known residual loss.**
- [C] HUD handling has three parts:
  - a per-pair generator mask plus persistent evidence, which flips after 3–5 unchanged frames;
  - an overlay boundary ring rendered single-frame with layer-separated cubic;
  - zero motion on the overlay.
- [H] On prefiltered input, thin anti-aliased glyphs never accumulate evidence because their pixels change with the background: ghost error +1.46 levels (1.5×) and +0.42 (2×) over MoonSR Spatial.
- [H] The log itself proposes a long-term statistic (temporal median or variance). [I] Translucent HUD elements and animated HUD (minimaps, damage numbers) are not modelled at all.

**W8. Learning does not pay for its complexity.**
- [H] Stage 3e: full ALSR gains **+0.03 dB** (aliased) and +0.005 dB (prefiltered) on the trained content type. The learned bank only affects the seed.
- [H] Real point-reduced content: learning made SSIM **worse** (0.8833 → 0.8701 single-frame), and the self-supervised gate did not notice. Some buckets are up to +26.7 % RMSE worse than generic against ground truth.
- [H] Learning is disabled above 1.75×: at 2×, point-model learning reached −9 to −11 dB before the bias fix and is still negative after it.
- [C] As a result, `applyLearned = 0` by default. It also costs about 0.17 ms when applied [H], a worker thread, a 6 MB profile per scale and much code.
- [I] Its single-frame gains (+1.5 dB on trained content) are real but do not transfer reliably. A seed that is better by construction (W1) is a cheaper way to the same benefit.

**W9. Generated frames are treated differently from real frames.**
- [C] Generated frames are upscaled by `RenderSingle`, which uses the seed only and no history.
- [I] With FG at 2×–6×, real (temporal) and generated (single-frame) frames alternate at different sharpness and aliasing. That alternation can read as flicker at the base frame rate. This is not measured.

**W10. Cost at higher tiers and on integrated GPUs.**
- [H] The 1440p ceiling of 1.33 ms (DLSS 4.5 Preset M on an RTX 5070, used as a reference in `nvidia.md` §13) is not met: 1.44–1.70 ms.
- [H] The A400 is about 7× slower. [I] There is no reduced "lite" path for integrated GPUs.

**What v1 does well, and v2 should keep:**
- [H] At 2× aliased it beats the best single-frame filter on every moving sequence (+0.3 to +2.0 dB, flicker roughly halved; iteration 1).
- [H] The HUD layer separation measurably cut HUD ghost error, from 18.1 to 5.97 at 1.5× aliased.
- [C] The pipeline is fully GPU-resident, does no allocation in steady state, never waits, and works on D3D11 FL 11_0 / SM 5.0.
- [C] Each pass is documented, and many choices are backed by measurements.

---

## Part 2: Survey of what raises capture-only temporal SR quality

The setting: only final frames are available (no MVs, depth or jitter); the resolution is fixed and low; frames are often point-sampled and aliased; the HUD is composited; D3D11 / SM 5.0 is the baseline; the budget is about 1–2 ms at 1080p output on an RTX 5060.

### 2.1 Multi-frame SR from natural sub-pixel motion (Google handheld multi-frame SR)

- **Source.** Wronski, Garcia-Dorado, Ernst, Kelly, Krainin, Liang, Levoy, Milanfar, "Handheld Multi-Frame Super-Resolution", SIGGRAPH 2019 (ACM TOG 38(4)). https://arxiv.org/abs/1905.03277
- **Verified [V]:**
  - It reconstructs a full RGB image from a burst of CFA raw frames, using natural hand tremor for sub-pixel offsets, with no explicit demosaicing step.
  - It is described as robust to local motion, occlusion and scene changes.
  - It runs at about **100 ms per 12-megapixel raw frame** on mass-market phones.
- **Method [V]** (full text via https://ar5iv.labs.arxiv.org/html/1905.03277, accessed 2026-11-21):
  - **Alignment:** coarse-to-fine pyramid block matching per tile against one base frame (the HDR+ method; Hasinoff et al., SIGGRAPH Asia 2016). Sub-pixel refinement uses **three Lucas-Kanade iterations**. The authors report that quadratic curve fitting of the matching cost was not accurate enough. That matters for MoonSR, because the FG estimator's sub-pixel step is a parabola fit and ALSR's cellrefine adds LK on top.
  - **Merge:** kernel regression (Takeda, Farsiu, Milanfar, IEEE TIP 2007, https://doi.org/10.1109/TIP.2006.888330).
    - Formula: C = Σ_n Σ_i c·w·R̂ / Σ_n Σ_i w·R̂, over the 3×3 nearest samples of each frame.
    - Kernel: w = exp(−½ dᵀΩ⁻¹d), with Ω built from the structure-tensor eigenvectors. Gradients come from a half-resolution luma image.
    - λ1 sets the kernel's spatial support and (λ1 − λ2)/(λ1 + λ2) its anisotropy.
    - Example parameters: k_detail = 0.05 px, k_stretch = 4, k_shrink = 2, k_denoise 1.0–5.0. The exact k1/k2 mapping and the D_th / D_tr values were in a part of the page I did not read [U].
  - **Robustness model:**
    - R = s·exp(−d²/σ²) − t.
    - σ = max(local spatial std of the guide image, noise-model σ).
    - d = Wiener-shrunk local colour difference.
    - Local motion M = span of the 3×3 neighbouring tile displacements. If M > M_th, a stronger rejection scale is used.
    - Finally, the minimum of R over a 5×5 window.
  - **Without motion** (a tripod), the sensor or OIS is moved deliberately to create sub-pixel shifts. That confirms there is no new information without motion.
  - **Runtime:** GTX 980: 0.83 ms fixed + **0.4 ms per output megapixel per merged frame**. Adreno 630: 15.4 ms + 7.8 ms/MPix. The default burst is 15 frames.
  - [I] One merged frame at 1080p output (about 2 MPix) therefore costs about 0.8 ms on a 2014 desktop GPU. A streaming variant that merges one new frame into a running accumulation is affordable.
- **Applicability [I]:**
  - The burst method aligns everything to one reference frame and runs offline (100 ms). It is not a streaming algorithm. Its **components** carry over directly:
    - **(a)** Keeping weighted sums (Σw·c, Σw) gives an explicit per-pixel evidence weight. v1 lacks this (W5).
    - **(b)** Structure-tensor anisotropic kernels place each new sample's contribution where it belongs on edges.
    - **(c)** A *soft* robustness term, colour difference normalised by local spread, can replace v1's hard hull clip (W2). It degrades gracefully where the hull would wrongly clip true detail.
    - **(d)** The local flow-variation penalty is a ready-made motion-reliability signal (W3).
  - Game frames are 8-bit RGB, not raw Bayer, so the noise model becomes an **aliasing/quantisation model**: the expected sample-to-sample variation of point-sampled content is its local spread.
- **Licence:**
  - The paper describes the method; implementing it from the paper is a clean-room design. [U] Google has published no official code that I know of.
  - Third-party re-implementations exist, but their licences were not checked [U]. Do not reuse any of them without a check.

### 2.2 Video super-resolution (VSR) networks

All of these are [M]; no paper was re-fetched. Each is listed by what it contributes and whether that part can work without a heavy network.

| Work | Core idea | Causal? | Weight / cost | Code licence | Feasible part for v2 |
|---|---|---|---|---|---|
| **FRVSR**: Sajjadi, Vemulapalli, Brown, CVPR 2018, https://arxiv.org/abs/1801.04590 | Recurrent. Flow network on LR frames, upsampled flow warps the **previous HR estimate**, space-to-depth, concatenated with the new LR frame, SR network | yes | small for its time, still a CNN | [U] | **Architecture match**: warp the previous output, then fuse with the new frame. v1 already does this classically. The idea that "the fusion step decides per pixel how much to trust warped history" is what a learned fusion would add |
| **TecoGAN**: Chu, Xie, Mayer, Leal-Taixé, Thuerey, SIGGRAPH 2020 (ACM TOG 39(4)), https://arxiv.org/abs/1811.09393 | FRVSR-style generator + spatio-temporal discriminator + **ping-pong loss** (forward-then-backward sequence; penalises drift) | yes | CNN + GAN training | Apache-2.0 [V, https://github.com/thunil/TecoGAN, accessed 2026-11-21] | **Training-time** trick: if v2 ever trains a tiny learned component, ping-pong and temporal losses stop recurrent drift at no runtime cost. The tLP / tOF temporal metrics are a model for the harness |
| **EDVR**: Wang, Chan, Yu, Dong, Loy, CVPRW 2019, https://arxiv.org/abs/1905.02716 | Sliding window (5–7 frames); pyramid, cascading, deformable alignment; temporal-spatial attention fusion | needs future frames | heavy | Apache-2.0 (BasicSR, which includes EDVR and BasicVSR / IconVSR) [V, https://github.com/XPixelGroup/BasicSR, accessed 2026-11-21] | Only the concept of attention fusion = per-pixel per-frame weights. v2 gets that with the robustness map |
| **BasicVSR**: Chan, Wang, Yu, Dong, Loy, CVPR 2021, https://arxiv.org/abs/2012.02181 | Splits VSR into propagation, alignment, aggregation, upsampling; **bidirectional** recurrent with SpyNet flow alignment | no (bidirectional) | [M] about 6 M params | [M] Apache-2.0 (BasicSR / MMagic) | The decomposition is a useful checklist. Long-range propagation (many frames) beats a sliding window, which supports a long-memory history (W5) |
| **BasicVSR++**: Chan et al., CVPR 2022, https://arxiv.org/abs/2104.13371 | Second-order grid propagation + flow-guided deformable alignment | no | [M] about 7 M params | [M] Apache-2.0 | Flow-guided *residual* offsets = "coarse flow + small local correction". v2's per-pixel refinement on top of cell flow is the classical analogue |
| **RealBasicVSR**: Chan, Zhou, Xu, Loy, CVPR 2022, https://arxiv.org/abs/2111.12704 | Adds a **pre-cleaning** module before propagation, because in long recurrent propagation artefacts get amplified | (uni- or bidirectional variants) | heavy | [M] Apache-2.0 (MMagic) | **Lesson:** do not propagate what you cannot trust. Clean or deweight aliasing *before* it enters history (anti-flicker, robustness). This is consistent with v1's own finding (W5) |
| **Real-time / mobile VSR** (e.g. Mobile AI challenge reports, Ignatov et al. 2021/2022) | Very small recurrent or sliding CNNs at 720p-class LR | mostly yes | tiny | varies [U] | Shows tiny CNNs can run per frame; specific numbers not re-verified [U] |

**Cost reality [E]:**
- BasicVSR-class networks are far above budget: hundreds of GFLOPs per 1080p frame, and they need future frames.
- [I] **One frame of look-ahead is free when FG is on.** Interpolating FG already holds back real frame t while it shows frames between t−1 and t. ALSR could therefore reconstruct frame t−1 using frame t as a "future" frame, adding no latency beyond what FG already adds. This is an option, not part of the core plan; see the open questions.

### 2.3 FSR 2 / 3 ideas that do not need engine motion vectors

- **Source [V]:** FidelityFX FSR 2 README, https://github.com/GPUOpen-Effects/FidelityFX-FSR2 (accessed 2026-11-21). The licence is an MIT-style notice, "Copyright 2023 Advanced Micro Devices, Inc." [V].
- **Pass structure [V]:** luminance pyramid (half-res luma for shading-change detection plus a 1×1 exposure), reconstruct & dilate (3×3 dilation of depth and MVs, depth reprojection), depth clip (disocclusion mask), create locks, reproject & accumulate, RCAS.
- **Locks [V]:**
  - New locks are created by testing a pixel's **3×3 luma neighbourhood against a threshold**, to detect **thin features** (wires, chain-link fences) and keep their detail.
  - Locks are reprojected with the colour. A lock is trusted only if the luminance values in its neighbourhood are close.
  - **Shading-change detection** compares, for a locked pixel, the luminance recorded at lock creation with the current luminance.
  - Lifetime is stored in the red channel, decremented each frame by initial length / jitter sequence length, and expires at zero.
- **Rectification [V]:** history is deweighted on disocclusion; samples far from the current YCoCg bounding box are clamped towards it. The current frame contributes a relatively small lerp factor by default, modified by the reactive mask.
- **Reactive / transparency-composition masks [V]:**
  - The reactive mask (0..1) tells FSR 2 where to reduce reliance on history; AMD recommends a maximum of about 0.9.
  - The T&C mask adjusts lock protection and removes the luminance-instability factor's effect.
  - Auto-reactive generation needs an opaque-only copy of the backbuffer.
- **Luma instability [V]** (`ComputeLumaInstabilityFactor` in https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR2/master/src/ffx-fsr2-api/shaders/ffx_fsr2_accumulate.h, accessed 2026-11-21):
  - **History:** four stored 8-bit luma slots per pixel, reprojected.
  - **Detection:** take the difference of the current luma to the previous frame's. If it is at least 1/255, look at the older slots: a same-signed earlier difference reduces the factor. The test flags luma that oscillates back and forth.
  - **Scaling:** the factor is scaled by (clip-box size / 0.1)⁶ and reduced by the reactive and accumulation masks. It is only active once the history slots are filled.
  - **Use:** `RectifyHistory` takes max(luma instability, lock contribution) × (1 − √reactive). It lerps the history between its clamped and **unclamped** value, and keeps more accumulation weight.
  - **Effect:** oscillating and locked pixels are clamped less and accumulate longer.
  - **Locks [V]:** lock lifetime is decremented by the upsampled sample weight / (jitter length × average Lanczos weight), and a lock is killed when its next-frame UV leaves the screen. `UpdateLockStatus`'s formula was not read [U].
  - [I] Both mechanisms work on reprojected luma only. They do not need depth or engine MVs (the depth-clip input can be replaced by v1's visibility), so they transfer to MoonSR.
- **Performance [V]** (DX12, no sharpening): 1080p Quality (1.5×) costs 0.2 ms on RX 7900 XTX, 0.5 ms on RX 6700 XT and 1.3 ms on RX 590. That covers a 6-pass pipeline at this budget on mid-range hardware, but FSR 2 gets motion vectors and depth for free.
- **What transfers to capture-only [I]:**
  1. **Locks: transfer directly**, and they answer W2. A lock needs only luma and reprojection, which v1 has. Without jitter the lifetime has to be counted in frames of tracked motion instead of a jitter cycle.
  2. **Disocclusion without depth.** v1 already uses forward-backward flow visibility. Depth clip itself is not available.
  3. **Reactive mask without the engine.** It can be derived from v1's own signals: high residual despite confident motion, persistent HUD-ring and translucency evidence, and per-pair photometric error.
  4. **Luma-instability-style anti-flicker** (W6).
  5. **RCAS** (MIT, FSR 1) as the output sharpener. v1's final sharpener is similar but original.
  - Locks, anti-flicker and the derived reactive mask are pixel-local and cheap: a few loads per pixel [E].
- **Licence:** MIT [V], so code reuse with attribution in `THIRD_PARTY_NOTICES.md` is permitted. A clean-room re-implementation is still preferable because MoonSR's inputs differ.

### 2.4 DLSS and XeSS

- [M; see `documentation/research/nvidia.md` and `amd_intel.md`] DLSS SR and XeSS SR need engine motion vectors and jitter, and DLSS also needs depth. They are proprietary learned models.
  - [V in nvidia.md] DLSS 4.5 Preset M costs 0.75 ms at 1080p output on an RTX 5070, which is MoonSR's internal cost ceiling.
  - Neither documents a capture-only mode. Running them on captures is the NR bridge's business, not ALSR's.
- **Transferable ideas [I]:**
  - A temporal upscaler at about 0.75 ms on a 5070 is the bar the owner set.
  - Both vendors' public talks describe the network as replacing *hand-tuned history heuristics* (rejection and rectification), not the warp. That is where learned capacity pays. Their internals are [U].

### 2.5 Optical flow at sub-pixel accuracy

| Method | What it is | Fit for v2 | Licence |
|---|---|---|---|
| **DIS**: Kroeger, Timofte, Dai, Van Gool, ECCV 2016, https://arxiv.org/abs/1603.03590 | [V] Inverse search for patch correspondences (inverse-compositional alignment, Baker & Matthews), densification by aggregating patches across scales, variational refinement; 300–600 Hz on a single CPU core | [I] **Best match for per-pixel refinement.** Per-patch inverse-compositional LK is one Gauss-Newton loop with a *precomputed* Hessian (v1's cellrefine already does this), then weighted densification. A GPU version is embarrassingly parallel | [M] OpenCV `cv::DISOpticalFlow`, Apache-2.0 (OpenCV ≥ 4.5). Implement from the paper anyway |
| **Pyramidal LK**: Bouguet, Intel tech report 2000; Baker & Matthews, IJCV 2004, https://doi.org/10.1023/B:VISI.0000011205.11775.fd | Coarse-to-fine LK | Already used per cell (v1). Remaining gains: smaller windows at full resolution, **affine** warp parameters (6-DOF LK) for zoom and rotation (W3) | Algorithm, no licence issue |
| **NVIDIA Optical Flow SDK / OFA** | [V in nvidia.md §8] Hardware on Turing+ except TU117; grids 4×4 (Turing) or 1/2/4 (Ampere, Ada); DX11/DX12/CUDA/Vulkan. Blackwell support not stated in the application note [U] | [I] Optional accelerator on NVIDIA only. Quarter-pixel output [M] is not finer than v1's LK refinement, so its value is the *search* (large motions), not sub-pixel accuracy. Not a v2 dependency | NVIDIA DesignWorks SDK licence [V in nvidia.md]; runtime from the driver |
| **D3D12 Video Motion Estimator** (`ID3D12VideoMotionEstimator`) | [M] Cross-vendor D3D12 video API, block sizes 8×8 / 16×16, quarter-pel vectors, optional per hardware | [I] Encoder-style ME optimises rate, not true motion, and gives noisy vectors on flat or aliased content. At most a candidate source. Requires D3D12 interop | Windows API [M] |
| **AMD / Intel hardware flow** | [U] No public, general-purpose hardware optical-flow API for D3D11 was found in earlier research (`amd_intel.md`). FidelityFX Optical Flow (FSR 3) is compute, SM 6.2, MIT [V in capture_tools_and_algorithms.md] | Ideas already used in the FG estimator | MIT |

**Key point [I]:** v2 needs **sub-pixel precision on aliased content**, not a wider search. The FG estimator already handles large motion. The documented aliasing-pattern bias [H] says that what is matched matters more than the solver:
- v1 matches against the history reference;
- blurred luma suppresses aliased high frequencies;
- census or zero-mean terms add robustness.

### 2.6 Edge-directed single-frame reconstruction (first frame, disocclusions, static content, generated frames)

| Method | Note | Licence |
|---|---|---|
| Lanczos-3 with anti-ringing clamp | [H] Best measured single-frame filter at 1.5× aliased in this project (beats MoonSR Spatial by 1.3–1.6 dB) | none |
| **FSR 1 EASU** | [M] 12-tap edge-adaptive Lanczos-like kernel, very cheap | MIT [M] |
| **NIS** | [M] 6-tap directional scaler | MIT [V, https://github.com/NVIDIAGameWorks/NVIDIAImageScaling, accessed 2026-11-21] |
| **SGSR v1 / v2** (Qualcomm) | [M] v1: single-pass 12-tap edge-direction-aware. v2 is described as temporal; whether it needs engine MVs was not stated on the repository front page [U] | BSD-3-Clause [V, https://github.com/SnapdragonStudios/snapdragon-gsr, accessed 2026-11-21] |
| **NEDI** (Li & Orchard, IEEE TIP 2001) | [M] Covariance-based edge-directed interpolation; strong on 2× edges, moderate cost | algorithm |
| **RAISR** (Romano, Isidoro, Milanfar, IEEE TCI 2017, https://arxiv.org/abs/1606.01299) | [M] Hashed (angle, strength, coherence) linear filters; what v1's learner implements | algorithm; RAVU port is LGPL [M] |
| **Pixel-art rules** (xBR MIT [M]; MMPX MIT [M]; xBRZ GPL-3.0 [V in earlier doc]) | For genuinely pixel-art games only | xBRZ: do not use in MIT code |

[I] For v2 the seed should be the **best measured** single-frame filter for the detected input class (point-sampled or prefiltered), with offline-fixed parameters, instead of per-game online learning. The harness can rank Lanczos-3, EASU-style and NEDI-style options cheaply.

### 2.7 Small learned components in compute shaders

- **Shipping practice [M]** (mpv and Magpie, see `capture_tools_and_algorithms.md` §3.3): weights are baked into multi-pass shaders, with 4–8 channels per RGBA16F texture and unrolled 3×3 convolutions. No ML runtime is needed.
- **Candidate models and licences:**
  - ArtCNN C4F16 (about 12 K parameters) and C4F32 (about 48 K): MIT [V in earlier doc].
  - ESPCN (Shi et al., CVPR 2016, https://arxiv.org/abs/1609.05158): paper-only, reimplement.
  - SRVGGNetCompact: BSD-3 [V in earlier doc].
  - CuNNy and RAVU: LGPL [V/M], so not in MIT directories without legal review.
- **Kernel prediction networks:**
  - Bako et al., SIGGRAPH 2017 (KPCN); Mildenhall et al., CVPR 2018 (burst KPN, https://arxiv.org/abs/1712.02327). A small network predicts *per-pixel filter weights* applied to the input instead of colours. [M] They generalise better than direct prediction and cannot hallucinate colours outside the input.
  - Thomas et al., "Temporally Stable Real-Time Joint Neural Denoising and Supersampling", HPG 2022: a small U-Net predicts blending weights and filter kernels for a real-time supersampler. It uses engine MVs [M].
  - [I] For v2 a KPN-style head would predict the **history blend / robustness weight** and a few kernel weights, replacing hand-tuned ramps. That is the part DLSS-class methods learn (2.4).
- **Cost per pixel [E]:**
  - A 3×3 convolution with Cin → Cout channels costs 9·Cin·Cout MACs per pixel.
  - Example: a 3-layer net at source resolution, luma in, 16 → 16 → 4 channels, 3×3 kernels: 9·(1·16 + 16·16 + 16·4) ≈ 3.0 k MAC per source pixel. At 720p (0.92 Mpx) that is ≈ 2.8 GMAC ≈ 5.6 GFLOP per frame.
  - [M] An RTX 5060 has on the order of 19 TFLOPS FP32 peak (not verified). [E] At 15–30 % efficiency in SM 5.0 (no wave ops; `min16float` is only a hint) that is **about 1–2 ms**, which by itself is over the remaining budget.
  - A KPN head with 8 channels: 9·(4·8 + 8·8 + 8·6) ≈ 1.3 k MAC, so **about 0.4–0.9 ms** [E].
  - [I] Integrated GPUs (an order of magnitude less throughput) cannot run either at 60 fps or more.
  - **Conclusion:** learned components are a later, optional, measured tier, not part of the v2 core.
- **Training data [I]:** the project cannot bundle or download vendor models (CLAUDE.md). Weights would have to be trained by the project on data with known terms, such as synthetic sequences or licensed captures. Dataset licences (REDS, Vimeo-90K) are [U] here.

### 2.8 Capture-based products

- **Lossless Scaling LS1** [V store page, M details, from `capture_tools_and_algorithms.md` §1.1]: the in-house spatial scaler. Its algorithm is not documented [U]. There is no public evidence that LS1 is temporal [U].
- **Magpie** [V earlier doc]: GPL-3.0; spatial effects only (FSR, NIS, Anime4K, ArtCNN, CuNNy, RAVU, FSRCNNX, NNEDI3, SGSR, xBRZ). [M] No temporal upscaler.
  - It confirms that the market's capture tools are single-frame. A working capture-only *temporal* upscaler would be distinctive, but only if measured to beat the best single-frame filter.
  - Magpie code must not enter MIT directories.

---

## Part 3: Recommendation, the ALSR v2 architecture

### 3.1 Design decisions

1. **Keep the v1 motion front-end** (shared estimator → `alsr_cellrefine` → `alsr_motion` visibility and selection), the HUD layer separation, the registration reference, the scene-cut handling, the D3D11 / SM 5.0 baseline and the "no wait, no allocation" rules. These are measured assets (Part 1).
2. **Replace the seed with the best measured single-frame filter** per input class (W1, W9). This is the cheapest large win and touches every static, disoccluded and generated pixel.
3. **Turn the history into an explicit evidence-weighted accumulation**, (colour, weight W) in the style of kernel regression (2.1), and replace the hard hull clip with a **soft robustness** term plus FSR 2-style **locks** for thin features (W2, W5).
4. **Choose the sample model by input class.** Point-sampled input uses kernel accumulation; prefiltered input uses the existing back-projection update (W4). The class comes from a detector that the harness validates.
5. **Add per-pixel sub-pixel refinement and an affine cell model** (W3).
6. **Add a cheap anti-flicker stabiliser** and a **long-term HUD statistic** (W6, W7).
7. **Online learning is removed** (owner decision 2026-11-21: code, UI and profiles go, in ALSR and in frame generation). v2 has no learner, no learned lookup and no validation gating. The sampling-model detector that lived in the learner is re-implemented standalone inside ALSR (4.3). An offline-trained network is only a separate later option (4.7).

### 3.2 Pass list

Resolutions: src = 1280×720 and out = 1920×1080 for the cost column. Costs are **estimates [E]**, anchored to the Stage 3f measurements in 1.2. "Kept" passes use measured numbers [H].

| # | Pass | Res | Data in → out (formats) | Problem addressed | Cost [E] (reasoning) |
|---|---|---|---|---|---|
| 0 | prep (kept) + **long-term statistics** | src | frame BGRA8 → work RGBA16F (lin RGB + blurred luma); overlay evidence R8; **new**: per-pixel EMA mean + variance of luma, RG16F | W7: thin AA glyphs are recognised by low long-term variance while the surrounding motion field is non-zero | 0.045 [H] + 0.02 (2 extra 4 B/px read+write at 0.9 Mpx) |
| 1 | cellrefine (kept) + **affine fit** | cells | as v1; out RGBA32F → **two** textures: translation + confidence, and affine 2×2 Jacobian (RGBA16F), the latter fitted by least squares over the 3×3 refined neighbours | W3: zoom and rotation inside a cell | 0.075 [H] + 0.01 (14,400 cells × 9 loads) |
| 2 | motion (kept) + candidate "affine" | src | out RGBA16F (v, conf, err) + layer RG8; **new**: local flow variation M (max-min of 3×3 cell vectors) in err's slot or a separate R8 | W3 / robustness prior (2.1 d) | 0.16 [H] + 0.01 |
| 2b | **per-pixel sub-pixel refinement** (new, compute) | src (optionally 2×2 blocks) | blurred luma curr + reference → v refined (RG16F) + residual (R16F) | W3: sub-pixel accuracy at motion boundaries and on non-rigid motion. DIS-style inverse-compositional LK, 5×5 window from a groupshared tile, 2 iterations, only where `alsr_motion` selected (boundary) or the residual is high | 0.10–0.25 (dense: 0.92 M px × 25 taps × 3 evaluations ≈ 70 M tap evaluations, about 6× cellrefine's work, but throughput-bound rather than latency-bound. Restricting to boundaries or 2×2 blocks cuts it 2–4×) |
| 3 | **reproject + robustness + locks** (rewrite of alsr_reproject) | out | history RGBA16F (rgb = colour, a = weight W) + lock RG8 (lifetime, lock luma) → Hw RGBA16F, R R8, seed mask D16 | W2 / W5: R = vis × motion-conf × exp(−d²/(k·σ²_local + ε)) × f(M), where d is the YCoCg distance of history to the current local mean and σ_local the 3×3 spread. Hard clip **only** where R is low and the pixel is not locked. A locked pixel (thin-feature detector on the history, FSR 2-style) widens the clip box; locks break on shading change, low visibility or scene cut | 0.31 [H, s2b, includes the old CR fetch and clip] + 0.05 (lock read and write, exp) |
| 3b | **seed** (rewrite of seedblend) | out, only where W·R < Wseed | frame → added (1 − α) × seed into Hw | W1 / W9: best single-frame filter per input class (candidates: Lanczos-3 + deringing, EASU-style directional, NEDI-style). The HUD-ring LayerCubic is kept | 0.03–0.06 (v1 0.025–0.045 [H]; a directional filter costs about 1.5× the taps) |
| 4 | **accumulate** (point-sampled class) | out | Hw + current frame → history RGBA16F (colour, W), gamma copy or destination | W5: current-frame contribution w_c = anisotropic Gaussian (structure tensor of current luma) of the distance from the output pixel to the nearest current sample centres (fixed pattern per scale); W ← min(R·W_h + w_c, Wmax); colour ← (R·W_h·C_h + w_c·c) / W | 0.10–0.14 (replaces residual 0.036 + update 0.14 [H]; similar loads) |
| 4' | **back-projection** (prefiltered class; v1 residual + update kept) | src + out | as v1 | W4: area sampling model where the input really is area-sampled | as v1, 0.18 [H] |
| 5 | **stabilise** (new; can be fused into 4) | out | history + **luma EMA / variance** RG16F at out res → history | W6: where luma oscillates (high temporal variance, low motion and low residual trend), lower w_c (more history) and pull towards the EMA mean | 0.03–0.05 (one RG16F read+write at 2 Mpx) |
| 6 | final (kept; optional RCAS-style) | out | gamma copy → destination | sharpening | 0.05 [H] |
| 7 | reference (kept) | src | history → R16F | registration | 0.11 [H, s2b] |

**Removed entirely (owner decision):** the learned table lookup (about 0.17 ms when it was applied [H]), collection, the CPU worker and the profiles. 4.3 explains what this frees in the budget.

**Total [E]:**
- Kept passes [H] sum to about 0.75 ms, which is consistent with the measured 0.67–0.82 ms.
- New work adds roughly 0.25–0.50 ms, so **v2 Render ≈ 1.0–1.3 ms at 720p → 1080p on an RTX 5060**, plus the shared estimator (0.79 ms [H]).
- **Superseded by the owner's budget decision.** v2 must stay at v1's cost: Render 0.67–0.82 ms plus the estimator's 0.79 ms. This plan therefore does not fit as written. Section 4.5 says which passes are fused, cut or moved to the low tier so that it does.

**Memory [E]:** v1 uses 98 MiB at 720p → 1080p [C].
- v2 adds lock RG8 (2 B/out px), luma EMA RG16F (4 B/out px), long-term statistics RG16F (4 B/src px), the affine texture and the refined per-pixel flow (4 B/src px).
- It drops the residual on the point-sampled path. The total is about +20 MiB.

**Lower tier (integrated GPUs) [I/E]:**
- **"Lite"** skips 2b and 5 and the affine fit, uses a fixed Lanczos seed and point accumulation, and runs the estimator at flow scale 0.25.
- Iteration-1 data showed the Radeon iGPU about 15× slower than the 5060 [H]. Lite would then still cost on the order of 10 ms there [E], which is more than a 60 fps frame allows.
- [I] On integrated GPUs the honest default is **single-frame seed + stabiliser only** (no motion), with temporal reconstruction offered where a harness run shows it fits. The owner's note that the Radeon 610M is not a performance target applies.

### 3.3 What to keep from v1, and what to drop

- **Keep:**
  - the shared estimator and cell LK refinement with the velocity filter;
  - the history-based registration reference (the measured aliasing-bias fix);
  - per-pixel candidate selection at motion boundaries;
  - visibility from forward-backward flow;
  - zero-motion snapping (static scenes stay exact);
  - linear-light accumulation [H measured better];
  - HUD evidence, boundary ring and LayerCubic;
  - the depth-mask "seed only where needed" trick;
  - the direct-output variant;
  - all the documentation and measurement discipline.
- **Drop entirely:** online learning (W8; owner decision). Keep only its edge-step sampling-model statistic, re-implemented standalone.
- **Rewrite:** the seed, rectification, the update rule on point-sampled input, and HUD detection of thin anti-aliased glyphs.

### 3.4 Milestones

**Superseded by 4.6**, which re-orders these milestones for the XeSS-referenced target at fixed cost. This list is kept for its per-milestone detail.

Each milestone is independently measurable with the new temporal harness (`moonsr_bench`: luma PSNR/SSIM, temporal flicker error, HUD-region score, scene-cut score, GPU time). **Every pass/fail threshold below is a proposal**; the lead sets the final ones.

**Standard test matrix:**
- Sequences: static, pan_slow, pan_medium, pan_fast, zoom_rotate, hud_pan, thin_features, layers, scene_cut, or the harness's equivalents.
- Scales 1.5× and 2×; aliased (point) and prefiltered (area) input.
- RTX 5060, plus a correctness run on the Radeon iGPU.
- Baselines: Lanczos-3, MoonSR Spatial, ALSR v1.

**M0: baseline (no algorithm change).**
- Run v1 and the single-frame baselines through the new harness and record everything in `BENCHMARKS.md` with commit, driver and settings.
- Add one *real* captured sequence (no ground truth; flicker and HUD scores only).
- Ask the benchmark-specialist for a **flow end-point-error metric** on synthetic sequences (true motion is known). Without it, M4 cannot be judged.
- *Exit:* a reproducible baseline table. The historical [H] numbers are replaced by reproducible ones.

**M1: seed upgrade.**
- Implement the 3b candidates: Lanczos-3 + deringing, an EASU-style directional filter, and a NEDI-style 2× filter. Rank them single-frame in the harness per input class and scale, and make the winner the ALSR seed and the `RenderSingle` path.
- *Expected effect [I]:* static and disocclusion-heavy sequences rise to the best single-frame level. v1's static case is 1.34 dB below Lanczos [H]. Generated frames improve by the same amount.
- *Exit criteria (proposal):*
  - static ≥ best single-frame PSNR at 1.5× and 2×, both input classes;
  - no moving sequence regresses by more than 0.05 dB or worsens on flicker;
  - Render cost delta ≤ +0.05 ms.
- *Why first:* the smallest change, a large share of displayed pixels (every pixel of every generated frame), no new state, and it lowers the floor that every later milestone builds on.

**M2: locks + soft robustness instead of hard clipping.** Rewrite pass 3.
- *Measure:* thin_features, pan_slow/medium aliased, hud_pan ghost error, flicker.
- *Exit:*
  - thin_features ≥ best single-frame (v1 was −0.72 dB at 1.5× aliased [H]);
  - ghost error not worse than v1;
  - no flicker regression.

**M3: evidence-weighted accumulation for point-sampled input, plus an input-class detector.**
- Pass 4. Back-projection is kept for prefiltered input.
- The harness must report the detector's decision per sequence. It was wrong at 2× prefiltered in v1 [H].
- *Exit:*
  - pan_medium prefiltered 1.5× ≥ best single-frame. v1 was −0.79 dB versus generic single-frame [H].
  - No aliased-input regression.

**M4: per-pixel sub-pixel refinement + affine cells.** Passes 1, 2, 2b.
- *Measure:* zoom_rotate, flow EPE, and the moving sequences.
- *Exit:* zoom_rotate ≥ best single-frame; EPE improved; cost of 2b ≤ 0.25 ms measured. Drop 2b if it does not pay.

**M5: anti-flicker stabiliser.** Pass 5.
- *Measure:* the temporal flicker metric on all sequences, and PSNR / SSIM (it must not blur).
- *Exit:* flicker reduced on aliased pans; PSNR within −0.05 dB.

**M6: long-term HUD statistic.** Pass 0 extension.
- *Measure:* the HUD-region score on hud_pan, prefiltered input in particular (v1 ghost +1.46 levels over Spatial [H]).
- *Exit:* HUD score ≥ best single-frame on all four hud_pan cases.

**M7: tiers and cost.**
- Measure the full v2 on the RTX 5060, the A400 and the Radeon iGPU at 720p → 1080p and 1080p → 1440p.
- Define Lite and the integrated-GPU default from those numbers.
- An optional research spike follows: a KPN-style robustness head (2.7), measured against M2's hand-tuned robustness.

### 3.5 Risks

1. **Synthetic-to-real gap [H/I].** Every v1 decision was made on synthetic sequences. The learning results are the cautionary tale: they were positive on synthetic data and negative on real point-sampled data. M0 must add real captures, and no milestone ships on synthetic gains alone.
2. **No ground truth for real games [I].** Flicker and HUD scores can be measured without ground truth; PSNR cannot. A capture of a game run at a high internal resolution and downsampled would provide one (the owner's games permitting).
3. **Locks cause ghost trails [I].** Protected thin features can trail when motion is wrong. Lock breaking needs the visibility and residual signals, and hud_pan / zoom_rotate are the stress cases.
4. **Input-class detection errors [H].** It was wrong at 2× prefiltered in v1. A wrong class costs dB, measured at 0.75–2.0 dB for the learner's model choice. A blended or conservative default is needed when the detector is unsure.
5. **Cost overrun [E].** 2b is the largest unknown. It is gated by measurement in M4 and dropped if it does not pay.
6. **Dependency on the FG estimator [C].** ALSR consumes `MotionFieldView`. Changes in `src/framegen` alter ALSR quality, so the harness should pin both.
7. **SM 5.0 limits [M/I].** There are no wave intrinsics and no guaranteed FP16, so reductions and any CNN cost more than on SM 6.x. Optional SM 6 paths would need a D3D12 backend the project does not have.
8. **Licences.**
   - FSR 2: MIT [V], OK with attribution.
   - NIS: MIT [V]. EASU (FSR 1): MIT [M], verify file headers.
   - OpenCV DIS: Apache-2.0 [M].
   - BasicSR: Apache-2.0 [V]. MMagic: Apache-2.0 [M]. TecoGAN: Apache-2.0 [V]. Snapdragon GSR: BSD-3-Clause [V].
   - Magpie and xBRZ: GPL-3.0, never in MIT directories.
   - RAVU and CuNNy: LGPL, review needed.
   - NVIDIA OF SDK: DesignWorks licence, optional only.
   - Recommendation: clean-room implementations from papers throughout. None of the v2 passes needs borrowed code.

---

## Part 4: Target "at least comparable to XeSS 2 SR or FSR 4", at v1's cost

Owner decisions (2026-11-21):
- **Quality target:** at least comparable to XeSS 2 SR or FSR 4.
- **Budget:** **no net cost increase** over v1. Render stays at 0.67–0.82 ms for 720p → 1080p on the RTX 5060 [H], plus the shared motion estimator (0.79 ms [H]).

### 4.1 What XeSS 2 SR and FSR 4 get from the game

**XeSS SR** (Intel XeSS SR Developer Guide 2.0, https://raw.githubusercontent.com/intel/xess/main/doc/xess_sr_developer_guide_english.md and https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html, both accessed 2026-11-21):

| Item | Content | Tag |
|---|---|---|
| Required per frame | **Sub-pixel jitter offsets**, input colour, and either **dilated high-resolution motion vectors** or low-resolution motion vectors **plus depth** (XeSS then dilates internally) | [V] |
| Optional | Responsive-pixel mask (0..1); exposure (value, texture, or built-in auto-exposure at a measurable cost) | [V] |
| Jitter | A quasi-random sequence ("Halton is a fair choice"), length scaled by the upscaling factor; at least 72 phases for Ultra Performance in 1.3 | [V] |
| Pipeline position | At the start of post-processing, **before tone-mapping**. After tone-mapping is "experimental". The UI is therefore not in its input | [V] position; [I] UI not in its input |
| Presets (1.3+) | NativeAA 1.0×, UQ+ 1.3×, UQ 1.5×, Q 1.7×, Balanced 2.0×, Perf 2.3×, UltraPerf 3.0× | [V] |
| APIs and hardware | D3D12: Iris Xe+ or any SM 6.4 GPU (DP4a acceleration recommended). **D3D11: Intel Arc only** ("Only Intel-optimized implementation ... is available"). Vulkan 1.1 | [V] |
| Network | Described as "AI-based temporal super sampling". No layers, sizes or timings are published. XMX and DP4a model differences are not documented in the guide | [V] absence; [U] |
| XeSS 2 vs XeSS 1.3 SR | The repository is labelled "XeSS 3 SDK"; the guide is titled "Developer Guide 2.0". I found no statement on whether the XeSS 2 / 3 SR network differs from 1.3 | [U] |
| Licence | Intel Simplified Software License (Oct 2022): use and redistribute **binary only, unmodified**, with notices; no reverse engineering; no use of Intel's name to endorse or promote derived products; termination on breach. **Benchmarking is not mentioned** | [V] https://raw.githubusercontent.com/intel/xess/main/LICENSE.txt |

**FSR 4 / FSR Upscaling 4.x** (https://gpuopen.com/learn/amd-fsr4-gpuopen-release/, accessed 2026-11-21, plus `amd_intel.md` §1.4):
- **API and packaging [V]:** it keeps the FSR 3.1 API, which is an easy update path, and is therefore fed the FSR 3.1 input set: colour, depth, motion vectors, jitter, exposure, and optional reactive and transparency/composition masks. It is DX12 only, on **Radeon RX 9000 (RDNA 4)**, and ships as prebuilt signed DLLs. The model is trained on "high-quality ground truth game data" on AMD Instinct GPUs.
- **Masks [V]:** AMD says FSR 4 preserves particle detail **without** a reactive or T&C mask.
- **Network [U]:** architecture and timings are not published.
- **Hardware path [V-secondary, amd_intel.md]:** FP8 on RDNA 4; FSR 4.1 is extended to RDNA 3 later per press. The machine has no RDNA 3/4 GPU (RTX 5060, RTX A400, Radeon 610M), so **FSR 4 can only be referenced here, not measured.**

**Shared by both [I]:**
- input rendered *before* the HUD;
- engine-exact per-pixel motion, including on thin and animated objects;
- depth for disocclusion;
- a controlled jitter sequence;
- linear, pre-tone-map colour;
- large learned reconstruction on matrix hardware (XMX, FP8 WMMA) or DP4a.

### 4.2 Gap analysis: where their quality comes from

| Quality component | Source in XeSS / FSR 4 | Capture-only ALSR | Reachable without engine data? |
|---|---|---|---|
| **Static scenes converge to near-native** | **Jitter**: each frame samples a new sub-pixel phase, so after one jitter cycle every output pixel has its own sample [I, from the jitter requirement [V]] | Static content gives **no new samples**. v1 output = seed forever [C]; v1 static was 1.34 dB below even Lanczos [H] | **No.** The ceiling is the best single-frame reconstruction. A learned single-frame net can narrow it (hallucinated plausible detail) but cannot recover information that was never sampled. I expect the largest and permanent gap here [I] |
| Detail accumulation in motion | Jitter + exact MVs | Natural motion: phase diversity is uncontrolled. Pure horizontal pans give no vertical diversity; integer-pixel-per-frame motion gives none. Flow error blurs | **Partly**: on rigid sub-pixel camera motion in both axes, at moderate speed [I]. That is the regime where v1 already gains +0.3 to +2 dB at 2× aliased [H] |
| Motion boundaries, thin and animated objects, particles | Exact per-pixel MVs; depth | Estimated flow: errors at boundaries, thin and non-rigid objects, transparency | Partly. Soft robustness and locks (M2) reduce the visible cost (ghosting) but not the information loss [I] |
| Disocclusion | Depth / MV dilation | Forward-backward flow visibility [C] | Mostly. Quality there is single-frame in both (theirs is learned) [I] |
| HUD | Not in the input at all [I] | Composited: has to be segmented [C] | Partly. Static opaque HUD works [H]; thin anti-aliased glyphs [H], translucent and animated HUD [I] stay weaker |
| Aliasing of the input | Jittered aliasing averages out | Fixed-phase aliasing is "baked in" when static; in motion it is noise | Partly, in motion only |
| Learned rectification and hallucination | Large networks on matrix hardware | SM 5.0 FP32 compute, a few hundred to about 1,500 MAC per pixel at this budget (4.3) | Small fraction only. A tiny net can learn *decisions* (blend weights) well; it cannot match a large net's texture and edge priors [I] |

**Honest verdict [I]:**
- **Plausibly comparable** (to be shown by measurement, 4.4) on moving rigid scenes with sub-pixel camera motion at 1.5–2×, for both input classes. That covers pans and orbit-like motion in 3D games, which is much of active gameplay.
- **Not reachable** on:
  - static or near-static views, which includes menus and slow scenes, and any game that renders at a fixed sample phase;
  - thin geometry while static;
  - particles and transparencies;
  - animated characters close up;
  - pans at whole pixels per frame;
  - content next to the HUD.
- On static content the expected gap equals the jitter-convergence advantage. Its size is **unknown until measured** in the harness (M0 in 4.6). I do not estimate it.
- A claim of "XeSS-comparable" without qualification would therefore be false for a capture-only upscaler. The defensible claim is per content class, with numbers.

### 4.3 Routes to close the gap with the base algorithm, ranked by quality per ms

**Owner decision (2026-11-21):** per-game online learning is removed entirely from ALSR and frame generation (code, UI, profiles). Parts 3 and 4 treat it as gone: no learner, no learned lookup, no validation gating. The main plan is the **base (non-learning) reconstruction**. An offline-trained network appears only as a clearly separated later option (4.7).

#### Budget arithmetic [H/E]

- **Budget = v1's measured range:** Render 0.67–0.82 ms at 720p → 1080p on the RTX 5060 [H], plus the shared estimator (0.79 ms [H]).
- **Time freed by removing learning:**
  - The low end of v1's range is learning off; the high end is "trained + collecting" (Stage 3f: 0.672 ms off vs 0.770 ms on at s0, continuous pan; 0.703–0.723 vs 0.714–0.818 at s0.3) [H].
  - The learned lookup itself measured about 0.17 ms when applied (Stage 3d) [H].
  - [C] In the shipped default (`applyLearned = 0`) the lookup was not running. The real gain is therefore **headroom**: base passes may use up to v1's measured upper bound, i.e. about **0.10–0.15 ms** more than v1 with learning off [E from H]. The lead should confirm that the budget is the upper end of that range.
- **Analytic front end, kept:** prep, cellrefine, motion, reference. Pass costs from 2b/3f [H] sum to ≈ 0.39 ms; later cuts make the current value lower but unrecorded. That leaves ≈ **0.3–0.45 ms**, plus the freed headroom, for reprojection, rejection, accumulation, stabilisation and seed.
- **Precondition:** v1's per-pass profiler was deleted [H]. The harness must time individual passes (M0) before these figures become measurements.

#### Ranked routes [I]

1. **Seed upgrade** (Part 3, pass 3b).
   - Cost ≈ +0.01–0.02 ms [E].
   - The certain gain is the measured Lanczos-vs-Spatial gap (1.3–1.6 dB at 1.5× aliased [H]) on every static, disoccluded, post-cut and **generated** pixel.
   - Highest certain quality per ms.
2. **Budget-neutral restructuring** (4.5): accumulate replaces residual + update; reference luma comes from an MRT of the accumulate pass.
   - Saves ≈ 0.07–0.11 ms [E], which pays for the items below. No quality change on its own (point-sampled path) [I].
3. **Thin-feature locks + soft robustness + fused anti-flicker**.
   - These are the analytic counterparts of what DLSS-class networks learn: Wronski robustness [V], FSR 2 locks and luma instability [V].
   - Cost ≈ +0.05–0.07 ms [E]. Targets W2, W5, W6, which are the largest measured moving-content failures.
4. **Evidence-weighted accumulation with a standalone input-class detector** (point vs area).
   - The detector lived in the learner, so with the learner removed it is re-implemented inside ALSR: the edge-step statistic, chosen correctly at 1.5× and wrongly at 2× prefiltered [H], so it needs harness validation and a conservative default.
   - Cost ≈ 0 net (within item 2). Targets W4 (−0.79 dB vs single-frame on prefiltered medium pans [H]).
5. **Static-mode heavier single-frame reconstruction** (analytic).
   - When the frame is static, the temporal passes do almost nothing (zero motion ⇒ gain 0 [C]). Their unused time can pay for a more expensive edge-directed filter (NEDI-style covariance interpolation or a wider directional kernel), with hysteresis so no switch pops.
   - The only base-algorithm lever on the largest gap to XeSS (4.2). The gain is uncertain [U] and bounded by single-frame quality.
6. **Affine cell motion inline** (≈ +0.01 ms) and **long-term HUD statistic** in prep (≈ +0.02 ms) [E]. Small, targeted (zoom/rotate, thin AA glyphs).
7. **Per-pixel sub-pixel refinement (2b)** only within leftover headroom, capped to motion-boundary pixels. Otherwise high tier only.

**Separate path: the engine-data bridge (route b).**
- Where a game provides MVs, depth and jitter, the root causes disappear (jitter, exact MVs, pre-HUD input).
- Either the user-installed vendor runtime runs through the NR bridge, or ALSR runs as a proper TAAU. By far the best quality per ms where available.
- It applies only to games where the bridge works (CLAUDE.md: no injection by default), so it does not replace the capture path.
- Vendor runtimes are user-installed, never bundled.

**Is the XeSS 2 floor reachable with the base algorithm at v1's cost? [I]**
- **Plausible but unproven** on moving rigid content with sub-pixel camera motion at 1.5–2×. v1 already beats single-frame there by 0.3–2 dB at 2× aliased [H], and items 3–4 attack its measured failures.
- **Not reachable** on static content, thin static geometry, particles and translucency, close animated characters, whole-pixel pans, and HUD-adjacent content (4.2). Item 5 narrows the static gap but cannot close it: the information is not in the frames.
- How big each gap is will only be known after M0.

### 4.4 How "comparable" is measured

- **Reference runs in the harness:**
  - **XeSS SR** through a **user-installed SDK runtime** (`libxess.dll` supplied by the owner, never bundled or downloaded by our tools).
  - **On the RTX 5060 this must be D3D12:** XeSS on D3D11 is Intel-only [V]. The harness needs a D3D12 module (a contract and ownership question for the lead and the benchmark-specialist).
  - [I] On NVIDIA hardware this measures XeSS's **cross-vendor (DP4a) implementation**, not the XMX implementation on Arc. Intel does not document their quality difference [U]. State it as "comparable to XeSS SR on this GPU".
  - **FSR 3.1** (FidelityFX SDK v1.1.x; MIT-style licence [V for the repository's licence file; whether every FSR 3.1 file falls under it is not confirmed [U]]) as an analytic engine-data reference. It is built from source as a tool dependency, not shipped.
  - **FSR 4:** referenced only (no RDNA 3/4 hardware on this machine).
- **Inputs:** per frame, the synthetic renderer produces a jittered low-resolution colour, true MVs and depth for the references, and the **unjittered** low-resolution colour with composited HUD for ALSR. Both use the same camera path, ground truth and output size.
  - The references get the HUD composited at output resolution after upscaling, as in a game.
  - Comparisons reported separately:
    - **(A) product:** XeSS with engine data vs ALSR capture-only, the owner's target;
    - **(B) like-for-like:** both with engine data, once the bridge path exists.
- **Metrics:**
  - luma PSNR / SSIM;
  - temporal flicker error;
  - HUD-region and scene-cut scores;
  - per-pass GPU time.
  - Optionally FLIP (NVIDIA, BSD-3-Clause [M], C++ available [M]) and LPIPS (BSD-2-Clause [M]; needs network weights and Python in a tool, never in the app).
  - Per-content-class reporting is mandatory: static, slow, medium and fast pan, zoom/rotate, thin features, particles/translucency, HUD, scene cut.
- **Licence [V/I]:** the Intel licence permits use and redistribution of the unmodified binary and **does not mention benchmarking** [V]. Local measurement with a user-installed runtime looks permitted [I].
  - Two points need the owner or lead: publishing comparison numbers in a public repository, and the clause against using Intel's name "to endorse or promote products derived from" the Software [V]. ALSR is not derived from XeSS, but marketing wording such as "XeSS-class" should still be checked [I].
  - Reverse engineering is prohibited [V]. We only call the public API.

### 4.5 Fitting Part 3 into v1's cost (budget reconciliation)

Part 3's v2 adds +0.25–0.50 ms [E]. Reconciled:

| Change | Effect [E] |
|---|---|
| Learning removed (lookup, collection, worker) | Budget headroom up to v1's measured upper bound: ≈ +0.10–0.15 ms available (no saving in the old default path, see 4.3) |
| Accumulate (pass 4) replaces residual + update on the point-sampled path | −0.04 to −0.08 ms |
| Reference: luma of the new history as an extra MRT of accumulate; only the 9-tap blur pass remains | about −0.03 ms |
| Dropping the unused `LastOutput` copy (if the lead agrees) | −0.02 to −0.03 ms [H estimate from Stage 3f notes] |
| Stabiliser fused into accumulate (one RG16F read/write) | +0.02 |
| Locks and robustness fused into reproject | +0.03–0.05 |
| Affine model inline in the motion pass from the 3×3 cells | +0.01 |
| HUD long-term statistic fused into prep | +0.02 |
| Seed upgrade (only where needed) | +0.01–0.02 |
| Static-mode heavier seed (static frames only, where temporal work is idle) | ≈ 0 on moving frames; on static frames it uses the idle temporal budget |
| Per-pixel refinement 2b: **not in the default path**; only within remaining headroom, capped to boundary pixels | 0 by default |

- **Net:** about −0.07 to +0.03 ms against v1 with learning off [E], well within v1's measured upper bound.
- **Leftover:** about 0.1–0.2 ms [E], the only budget for 2b or later additions.

### 4.6 Revised milestones (base algorithm)

Order: highest certain gain at zero net cost first, then the measured weak spots by expected size. Thresholds are proposals for the lead. **X** means the measured XeSS SR reference on the RTX 5060, comparison (A). Every milestone must meet **cost ≤ v1's measured upper bound** (same harness run).

| # | Milestone | Exit criteria (proposal) |
|---|---|---|
| **M0** | Harness reference: D3D12 module running the user-installed XeSS SR and FSR 3.1 on synthetic sequences with true MVs, depth and jitter; ALSR v1 (learning removed) and single-frame baselines on the unjittered stream; per-pass GPU timing; per-content-class tables | Reproducible tables in `BENCHMARKS.md`; the gap ALSR v1 − X measured per class (not estimated) |
| **M1** | Seed upgrade + budget-neutral restructuring (4.5 rows 2–4) | Static ≥ best single-frame; generated frames improve by the same amount; no moving class worse than v1 by > 0.05 dB or more flicker |
| **M2** | Locks + soft robustness + fused anti-flicker | Thin features, pans and layers ≥ M1 + measurable gain; flicker ≤ M1; gap to X reported per class |
| **M3** | Evidence-weighted accumulation + standalone input-class detector | Detector correct on every harness sequence, or a safe default where unsure; prefiltered medium pan ≥ best single-frame; on moving rigid classes at 1.5× and 2×: luma PSNR ≥ X − 0.5 dB and flicker ≤ 1.2 × X (placeholder) |
| **M4** | Static-mode heavier single-frame reconstruction, with hysteresis | Static ≥ M1 + measurable gain; no popping on start/stop sequences (flicker metric). **No parity criterion against X on static: not reachable (4.2)** |
| **M5** | Affine cells + long-term HUD statistic | zoom_rotate ≥ M3; HUD-region score ≥ M3 (X has no HUD in its input, so the gap is reported, not a criterion) |
| **M6** | Per-pixel refinement 2b within leftover headroom (else high tier) | Moving classes gain; cost limit held |
| **M7** | Tiers: A400, Radeon iGPU; Lite = M2-level analytic path | Per-tier cost table; the iGPU default decided from data |
| **(B-path)** | Bridge path where a game provides engine data (independent track) | Comparison (B): parity with X |

Thresholds of the form "X − 0.5 dB" are placeholders until M0 shows the real distances.

### 4.7 Later option, separate from the base plan: an offline-trained network

This is clearly labelled as **not** part of the milestone list above. It is not per-game or online; it is trained offline once, and its weights are the project's own (MIT) and baked into HLSL. It would only start after M3, and only if the measured gap to X on moving content remains.

**Largest network that fits [E].** SM 5.0 gives FP32 only: no DP4a, no wave ops, no guaranteed FP16 [M/I]. RTX 5060 peak is ≈ 19 TFLOPS FP32 [M, not verified]; assume 15–30 % efficiency, ≈ 3–6 TFLOPS effective. MAC per pixel that fits:

| Budget | 720p source (0.92 Mpx) | 1080p source (2.07 Mpx, 1440p output) |
|---|---|---|
| 0.3 ms | ≈ 500–1,000 MAC/px | ≈ 220–430 |
| 0.5 ms | ≈ 800–1,600 | ≈ 360–720 |
| 0.8 ms | ≈ 1,300–2,600 | ≈ 580–1,160 |

- At no net cost increase, the network must **replace** analytic logic, not add to it.
- **Candidate 1: weight predictor.** 8 features per source pixel → two 3×3 layers 8 → 8 → 6, ≈ 1,000 MAC/px, ≈ 0.2–0.45 ms at 720p [E]. It predicts robustness, current-sample weight, lock widening and kernel anisotropy, and replaces the M2/M3 decision arithmetic (≈ 0.15–0.3 ms).
  - It predicts how to combine samples, never colours (kernel prediction: Bako et al. 2017, Mildenhall et al. 2018, Thomas et al. HPG 2022 [M]).
- **Candidate 2: static-mode single-frame net** (≈ 2–3 k MAC/px in the idle static budget), replacing M4's analytic filter.
- **Expected quality [I]:** better decisions than hand-tuned ramps (ghosting, flicker, thin features). **Not** large-network detail hallucination. I know of no published data for game TAAU networks under 2 k MAC/px [U]. Only the harness can decide.
- **Training data [I]:**
  - the harness's synthetic renderer at high resolution, plus procedural and CC0 3D scenes (licences recorded per asset);
  - degradations: point, area, TAA-like, 8-bit, synthetic HUD overlays;
  - captured game footage needs an owner decision (copyright).
- **Training [I]:** offline PyTorch tool (BSD-style [M]); losses L1 / SSIM + warped-temporal + ping-pong (idea from TecoGAN, Apache-2.0 [V]; no code reused).

---

## Summary of findings

- **v1 is solid on aliased content in motion but limited elsewhere.** It wins on aliased 2× moving content. Its quality floor is a single-frame seed that measured 1.3–1.6 dB below Lanczos-3 at 1.5× aliased, it loses thin features to a hard 3×3-hull clip, its motion model is cell-resolution translation only, and it loses to single-frame on prefiltered medium pans. Online learning adds about +0.03 dB to full ALSR and can hurt real content.
- **Transferable without engine data:**
  - kernel-regression accumulation with soft robustness (Google handheld multi-frame SR);
  - FSR 2's locks, reactive logic and luma-instability idea (MIT, verified);
  - DIS / inverse-compositional per-pixel refinement;
  - RealBasicVSR's "clean before you propagate" lesson;
  - TecoGAN's temporal losses, if anything is ever trained.
- Heavy VSR networks and DLSS / XeSS do not fit the capture path or the budget.
- **Part 4, XeSS 2 / FSR 4 target at v1's cost, learning removed:**
  - **What they get [V]:** XeSS SR needs jitter, colour and MVs (or low-res MVs + depth), and runs before tone-mapping. FSR 4 keeps the FSR 3.1 input set and is DX12 on RDNA 4 only. Neither publishes its network or timings [U].
  - **Gap [I]:** static content converges to near-native only through jitter. Capture-only ALSR cannot match that on static content, thin static geometry, particles or HUD-adjacent content. It plausibly can on moving rigid content with sub-pixel camera motion, which must be shown by measurement.
  - **Base plan [E]:** about −0.07 to +0.03 ms net against v1 with learning off, inside v1's measured upper bound. Order: M0 reference (XeSS via a user-installed runtime, D3D12, since XeSS on D3D11 is Intel-only [V]); M1 seed; M2 locks, robustness and anti-flicker; M3 accumulation and detector; M4 static mode; M5 affine and HUD; M6 per-pixel refinement; M7 tiers. The bridge is a separate track.
  - **Offline network:** only a separate later option (4.7).

## Open questions

1. Does the 1–2 ms budget include the motion estimator when FG is off (0.79 ms at 720p input [H])?
2. Should ALSR use the one-frame look-ahead that interpolating FG already imposes (reconstruct frame t−1 with t as a future frame)? It adds no latency when FG is on, but needs a contract change (`alsr_upscaler.h`, lead-owned).
3. Should generated frames get a warped ALSR output instead of `RenderSingle` (W9)? That needs FG / ALSR cooperation and a contract change.
4. Which real game sequences can serve as quality references, and can a high-resolution ground truth be captured for any of them?
5. Is the budget v1's measured **upper** bound (0.82 ms, learning on), giving about 0.10–0.15 ms headroom, or its learning-off value (0.67–0.72 ms)?
6. Who owns the harness's D3D12 module for the XeSS / FSR 3.1 references? It is needed because XeSS on D3D11 is Intel-only.
7. Owner / lead check of the Intel licence for publishing XeSS comparison numbers in the public repository and for any "XeSS-class" wording (the licence is silent on benchmarking, and has an endorsement clause).
8. Can the unused `LastOutput` copy be dropped (a contract change)?
9. Should exit thresholds against X be fixed after M0, and what form should they take (per class, dB and flicker ratios)?

## Claims I could not verify

- Wronski et al.: the D_th / D_tr / M_th values and the exact k1/k2 mapping (in an unread part of the supplement) [U]. The method, formulas, LK iterations, 5×5 min filter, OIS detail and runtime are now [V].
- FSR 2: the `UpdateLockStatus` formula [U]. Luma instability is now [V].
- Licences still [M]: MMagic, OpenCV DIS, FSR 1 EASU. Now [V]: FSR 2 MIT, TecoGAN Apache-2.0, BasicSR Apache-2.0, NIS MIT, Snapdragon GSR BSD-3-Clause. Check each repository's LICENSE file before any reuse.
- SGSR 2 input requirements [U].
- RTX 5060 FP32 throughput, used in the CNN cost estimate [M].
- D3D12 Video Motion Estimator block sizes and precision [M].
- NVIDIA OFA support on Blackwell [U].
- Whether Lossless Scaling LS1 has any temporal component [U].
- Specific real-time VSR challenge results [U].
- XeSS: whether the XeSS 2 / 3 SR network differs from 1.3; XMX vs DP4a model differences; network size and timings [U].
- FSR 4: the full input list (inferred from the "keeps the FSR 3.1 API" statement [V]), network and timings [U]; RDNA 3 support for 4.1 is secondary-source only.
- Whether every FSR 3.1 file in FidelityFX SDK v1.1.x is under the MIT-style licence file [U].
- FLIP and LPIPS licences [M].
- The size of the static-scene gap to XeSS: not estimated, to be measured in M0.

### M0 implementation notes (2026-11-25)

Real-game comparisons use the owner's synchronized captures. For each scene and camera path, put
lossless, zero-padded PNG frames with identical filenames in `input/`, `native/`, `moonsr/`, and
`dlss/`, `fsr/` or `xess/` when the game provides these modes. Use the same displayed framing,
output resolution, frame rate and in-game settings for each take, apart from the resolution and
upscaler under test. Record the game, scene, settings, GPU, driver and build in `settings.txt`.
Use native-resolution frames as ground truth and inspect alignment before treating metric
differences as quality differences.

```powershell
build\alsr-m0-verify\bin\moonsr_bench.exe --capture-set benchmarks\output\captures\scene --tag scene
```

The capture scorer writes per-frame luma PSNR, SSIM and error flicker for each output variant.
It validates matching filenames and dimensions. The low-resolution input is retained for
provenance and repeat runs. The synthetic harness reports content classes and analytic backward
flow error in source pixels. Its readback is confined to the benchmark scoring traversal.
Whole-render and shared-estimator GPU times are sampled without pass markers, with a separate
traversal for per-pass times. The synthetic metrics are regression checks. Real-game measurements
are required for the vendor quality comparison.
