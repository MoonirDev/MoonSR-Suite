# Screen-space relief: estimated height, relief shading and motion parallax on captured frames

Author role: graphics-researcher. Date: 2026-11-22. Repository state read: commit `d25cd7a`.
Scope: groundwork for the owner's "smart parallax filter for game textures" feature. The owner chose the
**screen-space relief** interpretation: estimate surface height or depth from the captured picture (plus the camera and
scene motion MoonSR already measures), then add subtle parallax and relief shading to textured surfaces such as bricks,
rock, ground and walls. It must work on any game, approximately, without game changes. This note contains no engine
code. The interface sketch in Part 7 is a proposal for the lead, who owns contract headers.

## Provenance legend (read first)

| Tag | Meaning |
|---|---|
| **[C]** | Verified by reading this repository at commit `d25cd7a` (file named). |
| **[H]** | Historical MoonSR measurement recorded in another MoonSR document (named). Not re-run for this note. |
| **[V]** | Verified this session from the cited source (accessed 2026-11-22). |
| **[M]** | From the researcher's prior knowledge of the cited work, **not re-fetched this session**. Check before relying on a detail. |
| **[I]** | Inference or engineering reasoning by the researcher. |
| **[E]** | Cost or quality **estimate**. Not a measurement. Must not appear in `BENCHMARKS.md`. |
| **[U]** | Unknown, or not publicly documented. |

---

## Part 0: Summary and recommendation

1. **Nobody ships this as a capture-based feature.** Lossless Scaling lists no depth, relief or lighting filter [V]. NVIDIA's
   depth-based Freestyle filters (SSAO, Dynamic DOF, SSRTGI) use the game's depth through the driver [V], which MoonSR
   cannot do without injection. The closest depth-free precedent is a ReShade experiment: its author (Pascal "Marty
   McFly" Gilcher) used the greyscale image as relief height and reported edge and screen-border bugs that he said
   could not be worked around [V]. A paid ReShade shader (ReSurface) claims screen-space normals and POM at
   0.5–2.0 ms, but whether it uses the depth buffer is not stated [V/U]. This feature would be new. The best-qualified
   prior attempt reported failure.
2. **What can honestly be computed per frame at under 0.5 ms** is a contrast-normalised, edge-aware **band-pass of
   log-luminance** used as a texture height ("dark is deep"). From it you can derive relief normals, directional relief
   shading, cavity darkening and a short screen-space parallax offset. This is a well-founded heuristic: Khan et al.
   (SIGGRAPH 2006) derived approximate depth from image intensities with bilateral filters, and their material edits
   were perceptually accepted [V]. It is **not** shape recovery. Shape-from-shading is ill-posed (the bas-relief
   ambiguity) [M], and the polarity is wrong wherever dark means "dark material" rather than "deep".
3. **Camera motion gives real geometric information, but only under translation.** After the camera-rotation part of
   MoonSR's flow is removed with a robust homography fit (plane + parallax), the residual is proportional to relative
   inverse depth [M]. That gives coarse relative depth at motion-cell resolution (8 px cells at 720p [C]). It also
   gives two very valuable masks: geometry boundaries, and independently moving objects such as characters. It gives
   nothing during pure mouse-look rotation.
4. **Monocular depth networks are too slow for the base tier.** Depth Anything V2 Small is 41.3 GMAC at 518² [V]. It
   was measured at 18 ms for a 378×210 estimate on the owner's RTX 5060 through DirectML [H]. On SM 5.0 without matrix
   hardware I estimate 10–30 ms per inference [E]. The project rules also forbid bundling or downloading weights
   (CLAUDE.md), so this could only be a later, user-supplied, asynchronous research tier.
5. **Parallax must be position-driven, not velocity-driven.** The physically correct screen-space relief offset is a
   function of screen position, height and depth. It becomes visible because content moves across the screen. An
   offset proportional to the current flow is wrong: it would lag and snap back when motion stops ("rubber texture")
   [I, derivation in 2.3].
6. **Recommended tiering** (Part 1.6): T1 relief shading (base), T2 + parallax, T3 + motion-derived depth and masks
   (experimental), T4 neural depth (deferred research). The **default position is a two-rate stage**: analysis once
   per real frame at game resolution from the original capture, and a cheap apply pass per displayed frame at output
   resolution after frame generation and upscaling. Generated frames then get relief that is consistent with real
   frames, and no frame-generator contract change is needed.
7. **The main risks are perceptual, not technical.** Embossed halos on silhouettes, wrong polarity, doubled lighting,
   shimmer, and the effect reading as "oversharpened" rather than "deeper". Every milestone after M1 is gated on the
   owner's real-game A/B comparisons as well as synthetic metrics.

---

## Part 1: Height and depth estimation from captured frames

### 1.1 What MoonSR already provides [C]

- `MotionFieldView` (`src/framegen/frame_generator.h`) is a Texture2DArray, RGBA32F, one texel per motion cell.
  - Slice 0 is prev→curr and slice 1 is curr→prev.
  - xy is the displacement in frame pixels, z is confidence (forward-backward consistency × match quality), and w is
    the normalised residual.
  - Cell size is `cellSizePx`: 8 frame pixels at the default flow scale 0.5 (see `alsr_v2.md` §1.1 [C]). At 720p that
    is 160×90 cells.
- `staticMask` is a full-resolution R8 HUD mask (null when HUD protection is off). `stats[sceneCutIndex]` is a GPU-side
  scene-cut flag, readable by shaders in the same frame.
- `FramePairInfo::meanMotionPx` and `meanConfidence` are frame-level summaries, possibly one pair late.
- The motion analysis **always uses original captures** in the current engine (`src/engine/engine.h`, PipelineOrder
  comment; `main_window.cpp` info text) [C].
- When frame generation runs on another GPU, or is off while ALSR runs, the engine creates a separate local estimator
  (`alsrMotion`, `engine.cpp`) [C]. It costs 0.79 ms at 720p and 1.63 ms at 1080p input on an RTX 5060 [H, `alsr_v2.md`
  §1.2].
- Known HUD weakness: semi-transparent overlays, anti-aliased overlay edges and animated HUD elements are not handled
  (`KNOWN_ISSUES.md` line 13) [C]. Relief protection inherits this.

### 1.2 Single-frame, luminance-based height (the "fake parallax" / emboss family)

**Prior art.**
- The classic is "brightness = height". The ReShade forum thread by Pascal Gilcher ("Marty McFly") describes exactly
  this: greyscale of the game texture as relief height, in a POM shader, with "numerous bugs on object edges and screen
  borders" that cannot be worked around, because without geometry data the shader cannot tell where one object or
  texture ends [V, https://reshade.me/forum/shader-troubleshooting/2950-pom-custom-effect]. No licence is stated for
  that code [V], so it must not be used.
- A related ReShade request asks for a "fake normal mapping" full-screen effect for games without a depth buffer [V,
  search listing of https://www.reshade.me/forum/shader-suggestions/6462-direct-3d-fake-normal-mapping; thread not read].
- **Khan, Reinhard, Fleming, Bülthoff, "Image-Based Material Editing", ACM TOG 25(3), SIGGRAPH 2006**,
  https://doi.org/10.1145/1141911.1141937. [V, abstract via https://history.siggraph.org/?p=113552 and the UCF
  dissertation record https://stars.library.ucf.edu/etd/1091]:
  - an approximate depth map is derived from image intensities using bilateral filtering;
  - surface normals from that depth are used to re-texture objects;
  - the method relies on human vision being tolerant of some large physical errors and sensitive to others.
  - [M] The paper motivates "dark is deep" with Langer & Bülthoff's work on depth from shading under diffuse
    lighting (Perception, 2000). I did not re-read the paper to confirm the exact wording.
  - [I] This is the strongest evidence that the effect can be perceptually acceptable. Their use was offline,
    single-image and per object (a mask was given). Ours is real-time, whole-frame and unmasked, which is harder.

**Recommended estimator (clean-room, all [I]; costs [E]):**

1. **Luma.** Linearise the gamma-encoded capture (A9 in `ARCHITECTURE.md` says processing is gamma SDR) and take
   Rec.709 luma. Use **log-luminance** l = log(Y + ε): in an intrinsic-image view, shading is multiplicative, so a log
   turns it into an additive layer that filtering can separate. (Retinex: Land & McCann, JOSA 1971 [M].)
   - Prefilter with the same 5×5 binomial ALSR uses for matching [C, `alsr_v2.md` §1.2]. That suppresses aliasing,
     which would otherwise turn into crawling relief.
2. **Frequency separation, edge-aware.** Base B = self-guided **guided filter** of l, radius about 4–8 source px.
   (He, Sun, Tang, ECCV 2010 oral / TPAMI 2013: edge-preserving like the bilateral filter, "better behaviors near
   edges", and an exact linear-time algorithm independent of kernel size [V,
   https://people.csail.mit.edu/kaiming/eccv10/index.html]. It is built from box filters [M], so on the GPU it is a
   few separable passes.) Detail D = l − B.
   - Strong steps (silhouettes, geometry edges, shadow boundaries) stay in B. Fine texture (mortar, cracks, grain) goes
     to D.
   - A plain Gaussian base would put geometry edges into D, which is the "emboss halo" failure Gilcher describes.
   - Alternatives: bilateral (Khan et al.'s choice, costlier at a large radius), domain transform (Gastal & Oliveira,
     SIGGRAPH 2011 [M]), or a 2–3 level edge-aware pyramid.
3. **Contrast normalisation.** H = tanh(D / (k·σ_local(D) + σ_floor)). σ_local is the local RMS of D, so relief
   strength does not depend on texture contrast or scene brightness. σ_floor stops dark or noisy regions, film grain
   and compression noise from being amplified.
4. **Polarity: dark = deep.** This matches crevices under diffuse light (mortar between light bricks, cracks, pores).
   It is **wrong** for dark albedo on a flat surface: dark bricks with light mortar invert, and moss, decals, painted
   lines and dark-on-light text are also wrong. No single-image fix exists. The parallax tier (1.3) cannot recover
   albedo either. Accept it and keep amplitudes small.
5. **Texture-energy gate.** w_tex = smoothstep(e0, e1, σ_local) × (1 − smoothstep(e2, e3, σ_local)). Relief applies
   only to mid-energy texture. Flat regions (sky, smooth skin, cel-shaded fills) and extreme contrast (text, UI,
   specular glints) get none.
6. **Edge/texture classification.**
   - Geometry-edge suppression: P_edge = 1 − smoothstep(a, b, |∇B|), dilated by 2–3 px.
   - Optionally, structure-tensor coherence of D: a long, coherent, strong edge is a boundary; isotropic or repeated
     small-scale variation is texture.

*Cost [E]:* about 0.06–0.12 ms at 720p for luma, prefilter, guided filter (two separable box passes on l and l²), D,
σ_local and the gates. Anchor: ALSR prep (9 bilinear taps per pixel at 720p) measured 0.045 ms on the RTX 5060 [H,
`alsr_v2.md` §1.2].

**Shape-from-shading proper** (Zhang, Tsai, Cryer, Shah, "Shape from shading: a survey", IEEE TPAMI 1999 [M]):
- Recovering shape from shading needs known lighting, known albedo and smoothness priors, and is iterative.
- Belhumeur, Kriegman and Yuille (IJCV 1999) showed the **generalised bas-relief ambiguity**: under unknown lighting
  and albedo, shading cannot distinguish a surface from a family of flattened or sheared versions of it [M].
- The convex/concave ambiguity (the crater illusion) is resolved perceptually by a "light from above" prior
  (Ramachandran, Nature 1988 [M]).
- [I] Shape-from-shading is not usable per frame on arbitrary game images. Its one practical lesson is to use a
  **fixed light from above-left** for relief shading (2.1), because it agrees with the viewer's prior and needs no
  estimate.

**Small learned height-from-colour network (option, not recommended now).**
- A tiny CNN could map colour to height, trained on synthetic renders of CC0 PBR materials that have true height
  maps. Poly Haven and ambientCG are CC0 [V, https://polyhaven.com/license, https://docs.ambientcg.com/license/].
- Cost by the formula in `alsr_v2.md` §2.7: a 3-layer 3×3 CNN, 16 → 16 → 1 channels, at 720p costs about 1–2 ms on SM
  5.0 [E]. That is over budget, and its generalisation to game imagery is unknown [U]. Revisit only if M1 shows the
  heuristic's polarity errors dominate.

### 1.3 Depth from motion parallax using MoonSR's flow

**Geometry** [M: Longuet-Higgins & Prazdny, Proc. R. Soc. B 208 (1980), https://doi.org/10.1098/rspb.1980.0057; the
rotational flow component is independent of depth, confirmed by secondary citations V, search 2026-11-22. Plane +
parallax: Sawhney, CVPR 1994; Irani & Anandan, ECCV 1996. Pure-rotation homography H = K R K⁻¹: Hartley & Zisserman,
*Multiple View Geometry*, 2nd ed., 2004.]
- Image motion of a static scene = a rotational part (depends on camera rotation and intrinsics, **not on depth**) +
  a translational part (∝ T / Z, pointing away from or towards the epipole, the focus of expansion).
- A pure camera rotation (mouse look) maps the image by a homography and **carries no depth information**.
- Fitting a homography to the flow and subtracting it leaves a **residual parallax field**. Each residual vector lies
  on the line through the epipole, and its magnitude is proportional to the point's projective depth relative to the
  plane the homography locked onto.

**Proposed GPU method (clean-room, [I]; costs [E]):**

1. **Input.** Slice 1 (curr→prev) cell vectors and their confidence z, about 14,400 cells at 720p [C].
2. **Robust homography fit.** 8 DOF, 3 IRLS iterations with Tukey or Huber weights.
   - Each iteration is one 1,024-thread group that accumulates the 8×8 normal equations from all cells in groupshared
     memory, then solves them in one thread. No CPU readback.
   - Pinhole projection, which games normally use [I], makes the homography model exact for rotation and zoom.
   - Cost: about 0.02–0.05 ms [E].
3. **Translation strength** τ = robust mean |residual| over confident inlier cells.
   - If τ < about 0.3 px, the frame is rotation-only or static: do **no depth update** and keep the history.
4. **Epipole.** Least-squares intersection of the residual-vector lines (a 2×2 solve).
5. **Per cell.** γ = signed |r| / |p − e|, the relative inverse depth with respect to the reference plane.
   - The angle between r and (p − e) measures **epipolar inconsistency**. A large value means the point moves on its
     own: a character, a vehicle, foliage, an effect. That gives an **independent-motion mask** for free.
6. **Temporal fusion.**
   - Reproject the γ history with the curr→prev flow.
   - Blend weighted by τ × z, with a Kalman-style variance per cell (Matthies, Kanade, Szeliski, IJCV 1989 [M]).
   - Reset on a scene cut.
7. **Outputs** (cell resolution): relative depth (sign and scale ambiguous), its confidence, |∇γ| as a geometry
   boundary map, and the independent-motion mask.

**What this is good for [I]:**
- **Protection** (Part 3): geometry boundaries and characters, which are exactly where luminance-height fails.
- **Amplitude scaling:** a far surface should get proportionally less parallax.
- **Parallax direction:** surface orientation from the γ gradient (2.3).

**What it cannot do [I]:**
- It is coarse: 8 px cells, so no texture-scale relief.
- It is silent during rotation-only play, which is common in first-person games. Information persists only through the
  reprojected history.
- It depends on flow quality. Flat, untextured surfaces and repeating patterns give unreliable vectors (a documented
  estimator failure, `ls_reference_motion_depth.md` §3 [H]).
- If the homography locks onto the ground plane instead of the far background, "depth" becomes "height above the
  ground plane". The sign and scale need a per-scene normalisation, for example robust percentiles.

*Cost [E]:* about 0.05–0.1 ms total at 720p, **if the estimator already runs** (frame generation or ALSR on). If neither
runs, the estimator adds 0.79 ms at 720p [H]. That would exceed the base budget, so T3 should be offered only when
motion is already available.

### 1.4 Monocular depth networks

| Model | Params | Compute | Licence | Speed evidence |
|---|---|---|---|---|
| **Depth Anything V2 Small** | 24.8 M [V] | 41.30 GMAC at 518×518 [V, DepthART README comparison table, https://github.com/xuefeng-cvr/DepthART] | **Apache-2.0** (Small only; Base and Large are CC-BY-NC-4.0) [V, https://github.com/DepthAnything/Depth-Anything-V2] | 18 ms at 378×210, 37 ms at 518×294, 78 ms at 700×392 on the **owner's RTX 5060** via ONNX Runtime + DirectML, asynchronous [H, `ls_reference_motion_depth.md` finding 3, from the LSRT README]. A community TensorRT FP16 port reports 3 ms at 518×518 on an RTX 4090, including pre- and post-processing [V, https://github.com/spacewalk01/depth-anything-tensorrt, MIT] |
| **Video Depth Anything Small** | 28.4 M [V] | [U] | **Apache-2.0** (Small; Base and Large CC-BY-NC-4.0) [V, https://github.com/DepthAnything/Video-Depth-Anything] | A100: 9.1 ms FP32, 7.5 ms FP16 for input 1×32×518×518; per frame or per clip is not stated [V]. An experimental **streaming** mode exists, with an accuracy drop (ScanNet δ1 0.926 → 0.836) [V] |
| **MiDaS v2.1 small 256** | 21 M [V] | [U] | **MIT**. The repository was archived on 2025-08-25 [V, https://github.com/isl-org/MiDaS] | 90 FPS on an RTX 3090 at training resolution [V]; 22 FPS on an i7-1185G7 CPU via OpenVINO [V] |
| DepthART (ACM MM 2026) | 6.0–8.6 M | 3.8–8.6 GMAC at 448² [V] | Code CC BY 4.0; **checkpoints not covered** because they were trained from CC-BY-NC Depth Anything V2-L labels [V, repo README] | 0.9–2.9 ms model-only on an RTX A6000 (TensorRT FP16 / PyTorch) [V]. **Do not use** (weights licence) |
| ZipDepth | 6.14 M | 4.50 GMAC at 448² [V, same table] | [U] | [U] |

**Cost on MoonSR's baseline [E]:**
- An RTX 5060 has about 19 TFLOPS FP32 peak [M, as in `alsr_v2.md`]. Hand-written SM 5.0 compute reaches perhaps 15–30 %
  of that on convolutions and matrix multiplies (no wave intrinsics, no guaranteed FP16, no tensor cores).
- Depth Anything V2 Small at 518²: 82.6 GFLOP, so **about 14–28 ms**. At a quarter of the area: about 4–8 ms, plus
  attention and DPT-head overheads.
- A 4.5-GMAC model such as ZipDepth: about 1.5–3 ms.
- The measured 18 ms at 378×210 [H] (through DirectML, which may itself use matrix hardware) is consistent with this
  range.
- A ViT plus DPT head written in SM 5.0 HLSL is also a large engineering job.

**Other constraints:**
- [C, CLAUDE.md] Vendor runtimes, models and weights are never bundled or downloaded. Apache-2.0 Small weights would
  have to be **user-supplied** (as neural-rendering models already are). ONNX Runtime + DirectML would be a D3D12
  optional path, not the FL 11_0 baseline.
- [H, `ls_reference_motion_depth.md` §4] Depth Anything treats the HUD as foreground, and LSRT had to run it
  asynchronously, a few frames stale, and edge-match the stale depth to the current frame by colour.
- [I] A network supplies what the heuristic lacks most: **object-level depth ordering and silhouettes**, which drive
  protection, and per-surface depth scale for parallax amplitude. It adds little texture-scale relief: its output is
  smooth at that scale, so the luminance band-pass would still provide the texture relief.

**Verdict:** a research tier only (T4), asynchronous at 2–10 Hz, user-supplied weights, adopted only if M5 shows a
visible gain over T3's motion-derived masks.

### 1.5 Related single-image 2.5D work (context, not adoptable as is)

- **3D Ken Burns** (Niklaus, Mai, Yang, Liu, ACM TOG 2019, https://arxiv.org/abs/1909.05483) and **3D Photography with
  context-aware layered depth inpainting** (Shih et al., CVPR 2020, https://arxiv.org/abs/2004.04727) [M]: monocular
  depth plus inpainting of disoccluded regions for novel views.
  - Offline, with seconds per image [M].
  - The second is reported as MIT [V-secondary, a Replicate copy of the README]. The first is reported as
    non-commercial [secondary source only, U].
  - The relevant lesson [I]: large view changes need inpainting. Keeping MoonSR's offsets to 1–3 px avoids
    disocclusion entirely, and that is a design rule (2.5).
- **Relief and shape enhancement for display:** Luft, Colditz, Deussen, "Image enhancement by unsharp masking the depth
  buffer", SIGGRAPH 2006; Rusinkiewicz, Burns, DeCarlo, "Exaggerated shading", SIGGRAPH 2006; Vergne et al., "Radiance
  scaling", 2010 [M]. All need real depth or normals. They show that **small, local, shape-aligned contrast changes**
  read as "more 3D" without looking processed. That is the right target for T1.

### 1.6 Recommended tiers

| Tier | Height source | Adds | Needs motion? | Budget proposal (RTX 5060, 720p game → 1080p output) |
|---|---|---|---|---|
| **T0** Off | – | – | – | 0 |
| **T1 Relief shading** (base) | Luminance band-pass (1.2) | Relief normals, directional shading, cavity darkening, protection, temporal stabilisation when motion exists | Optional: stabilisation and the HUD mask use it; a cheap fallback exists without it (3.1) | Analyse ≤ 0.15 ms per real frame + apply ≤ 0.10 ms per displayed frame [E-target] |
| **T2 Relief + parallax** | As T1 | Screen-position parallax with a short POM march; optional height self-shadow | Same as T1 | Analyse ≤ 0.15 ms + apply ≤ 0.20 ms per displayed frame. **Total ≤ 0.5 ms at 1080p output without FG** (the owner's figure) |
| **T3 Motion depth** (experimental) | T1 + plane+parallax depth (1.3) | Geometry-boundary and independent-motion masks, depth-scaled amplitude, orientation-aware parallax direction | **Yes**, offered only when the estimator already runs | + ≤ 0.1 ms per real frame |
| **T4 Neural depth** (research, deferred) | T1 + a user-supplied monocular model, asynchronous | Object-level depth ordering | No (but it needs to edge-match stale depth with motion) | 10–30 ms per inference [E], amortised over 6–30 frames; an optional D3D12 / DirectML path |

---

## Part 2: Rendering the relief

### 2.1 Relief normals and shading (T1)

- **Normal.** N = normalize(−s·∂H/∂x, −s·∂H/∂y, 1), from a 3×3 Sobel or Scharr kernel on H, where s is the
  "bumpiness".
- **Light.** The default is a **fixed artistic light** from upper-left, about 45° elevation. This matches the
  light-from-above prior [M, 1.2], avoids estimation noise and is temporally constant.
  - "Follow scene light" is an experimental option. Estimate the dominant illuminant tilt from shading-gradient
    statistics of the **base** layer B (Pentland, JOSA A 1982, "Finding the illuminant direction" [M]), temporally
    smoothed, and blend towards the fixed light when the estimate is weak.
  - Contradicting the game's baked lighting is the risk the option addresses. Inconsistent relief lighting is a known
    perceptual cue that something is fake [I].
- **Shading factor.** S = 1 + a·(N·L − N₀·L), with N₀ = (0, 0, 1). Flat areas are unchanged and only slopes brighten or
  darken.
  - [I] With a fixed L, to first order this is a **directional derivative of H**: an edge-aware, contrast-normalised
    **emboss**. Being honest about this matters. T1 is a principled emboss, not 3D reconstruction.
- **Cavity / micro-AO.** C = 1 − b·max(0, (G_r * H) − H). Pixels lower than their neighbourhood are darkened. One blur,
  or a mip of H.
  - A horizon-style variant (a few directions over the height field, in the spirit of HBAO and GTAO: Bavoil & Sainz
    2008; Jimenez et al., Activision 2016 [M]) costs 4–8× more taps.
  - Expectation [I]: cavity darkening contributes the largest share of perceived depth in mortar and rock, and is the
    most robust to the polarity problem. A wrong-polarity cavity merely darkens a dark spot.
- **Composite.** Apply in linear light: out = in × S × C, luminance only (chroma kept). Limit the change to ±δ, with a
  proposed δ of 6 % of the pixel's luminance.

### 2.2 Static parallax is invisible; motion makes it visible

- A static camera and static content with any fixed offset field is indistinguishable from slightly distorted texture.
  There is no reference to compare against [I].
- Depth is perceived through **change of offset as the view changes**, which is motion parallax. The offset itself
  must still be a **function of view geometry** (screen position, depth, surface orientation), not of velocity (2.3).

### 2.3 The correct screen-space parallax offset

**Derivation [I, pinhole camera, small heights].** Take a texel raised by Δz towards the camera (relative to its
surface at depth Z), at screen position p relative to the principal point c (normally the screen centre), with focal
length f in pixels.

- **Fronto-parallel wall** (normal facing the camera): the raised texel projects at f·X / (Z − Δz), so it shifts by
  approximately **(p − c) · Δz / Z**. The shift is radial from the screen centre: zero at the crosshair and largest at
  the screen edges.
- **Ground plane** (normal up), camera height h_c, level camera: a point at depth Z appears (y − y_h) = f·h_c / Z below
  the horizon line y_h. A bump of height Δh shifts **up** by f·Δh / Z = **(y − y_h) · Δh / h_c**. The shift is
  vertical only and grows linearly with distance below the horizon.
- **In general:** the relief offset of a texel points along the image of its surface normal, away from the normal's
  vanishing point, and its size scales with height / depth.

**Consequences:**
- Both models depend only on **position**, height and depth. When the camera dollies forward, ground texture moves down
  the screen, (y − y_h) grows, and bumps appear to rise as they approach. When the camera strafes past a wall, texels
  cross the screen and their radial offset changes sign. Both are the expected parallax. Pure rotation moves content
  across the screen too, so the offset still changes with position. Real parallax from rotation is zero, so this is a
  model error. It stays small for wall-like surfaces near the screen centre [I]. T3's rotation/translation split could
  freeze the offset field under rotation-only motion. This is a tuning item for M3.
- **Velocity-driven offsets are wrong.** The per-frame differential parallax is flow × Δz / Z. That is the time
  derivative of the position form. Using it directly (offset ∝ current flow) gives a texture that leads or lags while
  moving and snaps back when motion stops. Integrating it drifts without bound. Reject both [I].
- **Magnitude check [I].** Take 1 cm mortar depth at 3 m, a 1080p screen with a 60° vertical FOV (f ≈ 935 px), and a
  texel 900 px from the centre on a facing wall: about 3 px. A ground bump of 1 cm at 3 m depth: about 3 px. Real POM
  in games works at a few pixels. **A cap of 1–3 output px is physically plausible** and stays below the scale where
  disocclusion appears.

**Model choice without depth (T2), [I]:**
- Default: the vertical "ground" model in the lower part of the screen and the radial "wall" model elsewhere,
  cross-faded around the centre line. The horizon is assumed to pass through the screen centre (level camera).
- T3 replaces the guess: the γ gradient orientation per region selects wall or ground, the homography gives the horizon
  and principal-point estimate, and γ scales the amplitude.
- This is the least-founded part of the design. It must be tuned by eye (M3).

### 2.4 Screen-space POM ("steep parallax" in screen space)

- **Prior art [V]:** parallax occlusion mapping ray-marches a height field per pixel in tangent space. Tatarchuk, "Dynamic
  Parallax Occlusion Mapping with Approximate Soft Shadows", I3D 2006
  (https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Tatarchuk-POM-SI3D06.pdf). Steep parallax
  mapping: McGuire & McGuire, I3D 2005 poster. Tatarchuk reports that earlier single-step parallax flattens and aliases
  at steep angles [V, search excerpt].
- **In screen space [I]:**
  - Along −offset direction, take N = 4–8 linear steps over H (bilinear) and find the first step where the "ray height"
    falls below H. Refine with one secant step, then sample colour at the hit.
  - At 1–3 px offsets, a single-tap "parallax offset" (offset = k·H(p)·dir) is often enough. The march matters only at
    the cap.
  - **Edge stop:** the march stops wherever protection P along the path drops below a threshold. The offset shrinks
    there, so sampling never crosses a silhouette or HUD edge. This answers Gilcher's unfixable-edge problem by refusing
    to warp near edges, not by solving it.
- **Height self-shadow (optional, T2):** march 4 steps towards L over H and darken where the path is occluded
  (Tatarchuk's approximate soft shadows, applied to the estimated height [I]). Bend Studio's screen-space shadows are
  related prior art [V, https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/], but the code licence is not
  named on that page [U] and the technique needs real depth. Clean-room only.
- *Cost [E]:* 2.07 Mpx at 1080p × (8 march taps + 1 colour tap + 4 normal taps + 1 cavity tap) ≈ 29 M taps. Expect
  about 0.10–0.20 ms on an RTX 5060 (anchor: ALSR's final 3×3 pass at 1080p, 0.05 ms [H]).

### 2.5 Keeping it subtle (design rules) [I]

1. **Offset cap:** ≤ 2 output px by default and ≤ 3 px maximum. That leaves no disocclusion and no inpainting.
2. **Luminance change:** ≤ ±6 % by default, applied to luminance only.
3. **Strength:** one master "Strength" control, defaulting to a low value. Internal parameters are tied to it so that
   there are no runaway combinations.
4. **Hierarchy of effect:** cavity, then shading, then parallax. Each layer can be turned off.
5. **Sharpening:** relief adds high-frequency contrast, so stacking it with ALSR's final sharpen or the spatial sharpen
   gives halos. Either apply relief **before** the final sharpen, or reduce sharpening while relief is on (a lead
   decision, D6).
6. **A/B toggle and split-screen debug view** so the owner can always compare.

---

## Part 3: Protection and stability

Protection is one weight P ∈ [0, 1] per pixel, the product of the factors below. The final effect is scaled by P
(shading) and by P along the march path (parallax). P is computed at game resolution in Analyse and sampled bilinearly
at output resolution.

### 3.1 HUD, UI and text

- **Reuse [C]:** `MotionFieldView::staticMask` (full-res R8), dilated by 2 px. ALSR's persistent overlay evidence is a
  second source when ALSR runs.
- **Without any estimator [I]:** a cheap per-pixel counter of "unchanged while the frame changed", like ALSR's overlay
  evidence (+ when unchanged, − when changed) [C, `alsr_v2.md` §1.2 pass 0]. About 0.02 ms [E].
- **Known gaps** [C, KNOWN_ISSUES.md]: translucent and animated HUD. Additional cheap cues [I]:
  - **saturated flat colour with sharp edges:** low σ_local inside a region, high |∇| at its border, which the
    texture-energy gate already excludes;
  - **thin high-contrast strokes (text):** excluded by the e2–e3 upper gate.
  - Text in the game world (signs, posters) will get some relief. It is tolerable at low amplitude.
- **Menus and fully static screens:** if the static-mask coverage is above about 60 %, fade relief out (3.6).

### 3.2 Sky

- The sky usually has low σ_local, so the energy gate makes H ≈ 0 there anyway [I].
- Add a soft "smooth-gradient region near the top of the frame" factor only if M4 tests show clouds getting relief.
  Clouds are mid-energy texture and will be embossed otherwise [I].

### 3.3 Characters, silhouettes and object boundaries

- Geometry-edge suppression from |∇B| (1.2 step 6).
- **Motion discontinuities:** the max − min of the 3×3 neighbouring cell vectors above about 1 px, plus low confidence z,
  suppresses relief [I]. This is the same local-flow-variation signal proposed for ALSR v2 [C, `alsr_v2.md` §3.2
  pass 2].
- **T3:** the independent-motion mask (1.3 step 5) and |∇γ| boundaries.
- **Faces.**
  - Faces are smooth, so the energy gate keeps relief low. Pores and stubble at close range would get relief, which
    tends to look "craggy" [I].
  - Skin-tone chroma detectors (for example YCbCr ellipse models [M]) are unreliable under game colour grading.
    Prefer: independent motion (T3) plus a lower amplitude in regions whose chroma is near the skin locus. Make it
    tunable, not a hard mask [I].

### 3.4 Flat-shaded, stylised and pixel-art games

- **Frame-level texture coverage:** the share of pixels with w_tex > 0.5, computed in a GPU reduction and smoothed over
  seconds. Below about 5 %, the game is cel-shaded, flat or minimalist: fade T1/T2 out and report the reason in stats
  [I].
- **Pixel art and point-sampled content:** relief on pixel art destroys its intent. ALSR v2 plans a standalone
  sampling-model detector [C, `alsr_v2.md` §3.1 item 7]. If its class is "point-sampled at integer scale", disable.
  Otherwise use the per-profile setting [I].

### 3.5 Transparent effects (smoke, fire, particles, glass, water)

- Low flow confidence z, a high normalised residual w, and high temporal variance of luma together reduce P [I].
- Water and glass with refraction or reflection change appearance with view. Relief on them shimmers. They are caught
  mostly by the residual and variance factors.

### 3.6 Temporal stability and scene cuts

- **Height history.** H_t = lerp(reproject(H_{t−1}, curr→prev flow), H_raw, α). α ≈ 0.3 by default, and α = 1 on
  disocclusion (low z) and on a scene cut. P is stabilised the same way, so masks do not flicker [I]. Without motion,
  run no history (α = 1). Relief then flickers exactly as much as the image does.
- **Anti-aliasing input.** Compute H from the prefiltered luma (1.2 step 1). Aliasing crawl otherwise becomes relief
  crawl.
- **Swimming.** Parallax offsets vary smoothly with position by construction (2.3). Clamp |∇offset| so that neighbouring
  pixels never differ by more than about 0.5 px of offset [I].
- **Scene cuts.** Read the GPU flag `stats[sceneCutIndex]` [C]. Reset histories and fade relief in over about 5 real
  frames. That avoids a one-frame flash of unstabilised relief [I].
- **Auto-disable summary** (all reported in stats with a reason):
  - texture coverage too low (stylised);
  - static-mask coverage too high (menus);
  - global flow confidence too low for N frames (fast chaos, heavy effects; T3 only);
  - very dark frame (mean luma below a threshold; noise would dominate);
  - scene cut (temporary fade);
  - estimator unavailable (T3 falls back to T2).

---

## Part 4: Where relief fits in MoonSR's pipeline

### 4.1 Positions considered

| Position | Pros | Cons |
|---|---|---|
| **(A) Before frame generation, on real frames at game resolution** | Cheapest: once per real frame at 0.92 Mpx. Generated frames inherit relief through interpolation | `FrameGenerator::PushFrame` takes **one** texture for both analysis and as the interpolation source [C]. Relief before it would perturb flow analysis (position-dependent offsets are non-rigid), unless the contract gains a separate "display" input (**contract change**, D1). ALSR also reads `RealFrame(0)` [C, alsr_v2 §1.1], so relief would enter ALSR's history |
| **(B) After FG, before upscaling, at game resolution, every displayed frame** | Half the pixels of (C). Upscaler sharpening comes after relief | The upscaler sees modified input. Relief detail is magnified by the upscale. With ALSR, real and generated frames differ in treatment (`RenderSingle`) [C, alsr_v2 W9] |
| **(C) Last stage, at output resolution, every displayed frame** (recommended default) | Real and generated frames are treated identically. Analysis can still use the **original capture**, like neural motion analysis [C]. No FG or ALSR contract change. Sub-pixel offsets at output resolution | The apply cost is paid per displayed frame (× the FG multiplier). HUD mask and H must be upsampled. It interacts with output sharpening (2.5 rule 5) |

### 4.2 Recommended: two-rate stage at the end of the pipeline [I]

1. **Analyse** runs once per real frame, at game resolution, on the **original capture**, the same image the motion
   estimator analyses.
   - It computes the luma band-pass H, the protection P, the T3 depth if enabled, and frame statistics.
   - It writes H, P and a blurred H (for cavity) into a 2-deep history (current and previous real frame).
   - Cost: ≤ 0.15 ms at 720p (T1/T2), + ≤ 0.1 ms (T3) [E-target].
2. **Apply** runs once per displayed frame, at output resolution, as the last image operation before the cursor
   overlay and present.
   - **Real frame:** sample H and P of that frame.
   - **Generated frame at phase t:** sample H and P from both real analyses, displaced along the cell flow scaled by t
     and (1 − t), and blend by t. This approximates how FG warps colour.
   - Where cell confidence z is low, scale P down, so FG artefacts are not embossed.
   - On a scene-cut pair, use the nearer real frame's H, as FG does [C, `Interpolate` contract].
   - This needs nothing beyond the existing `MotionFieldView` [C].
3. **Alternative for generated frames:** recompute a cheap H directly from the generated image (band-pass only, no
   history). It is simpler but flickers more on interpolation artefacts. Measure both in M4 [I].
4. **Upscaling placed before FG** (the load-order option that interpolates at output resolution [C, engine.h]): nothing
   changes. Apply still runs last, and analysis still uses the original capture.

### 4.3 Load-order and per-stage GPU (proposal for the lead)

- **Enum.** Add `PipelineStage::Relief = 3`, appended so existing settings indices (`settings.h` says the values match
  `PipelineStage` [C]) stay valid.
- **Migration hazards in the current code [C, `src/engine/engine.h`, `src/app/settings.h`]:**
  - `PipelineOrder` is documented as "must contain each PipelineStage exactly once; anything else is replaced by the
    default". Every saved order today has three entries, so once a fourth stage exists, a strict check would reset
    every user's custom order. The validator should **append missing stages** (Relief at the end) instead.
  - `StageAdapters::For` is a ternary chain whose final branch returns `upscaling` for any other value. A new stage
    would silently map to the upscaling GPU. That happens to match "follow the previous stage" only in the default
    order, so make it explicit.
  - `settings.h` indexes `AppSettings::stageGpu` by `PipelineStage` value and stores the order as a text list such as
    "neural,framegen,upscaling". Both need a fourth entry and a token (for example `relief`).
- **Positions.** Relief's position in the load order selects **where Apply runs**: last (default, C), or before
  upscaling (B). Position A needs D1 first. Analysis always uses the original capture. The UI note should say so, as it
  does for neural motion analysis [C].
- **Per-stage GPU:** I recommend **none** at first. Relief runs on the GPU of the stage it follows (the presentation GPU
  when last).
  - A separate relief GPU would add one cross-GPU copy per displayed frame for a 0.1–0.2 ms stage. That costs far more
    than it saves [I].
  - If the lead wants uniformity with `StageAdapters`, an "Auto (follow previous stage)" default is the safe choice.
- **Motion availability:**
  - **FG on the same GPU:** use its `MotionField()`.
  - **FG on another GPU, or FG off with ALSR on:** use `alsrMotion`, the local estimator [C].
  - **Neither:** T1/T2 run without motion (no history; HUD via the cheap counter, 3.1). T3 is unavailable. The engine
    should **not** start the estimator just for relief by default, because 0.79 ms at 720p [H] exceeds the whole relief
    budget (D7).

### 4.4 What relief needs from the frame generator's motion field

| Need | Available now [C] | Request (optional) |
|---|---|---|
| curr→prev cell flow for history reprojection | slice 1 | – |
| Flow for warping H to phase t | slices 0 and 1 | – |
| Confidence and residual | z, w | – |
| HUD mask | `staticMask` (null if HUD protection is off) | Expose ALSR's persistent overlay evidence as a shared resource |
| Scene cut | `stats[sceneCutIndex]` | – |
| Per-generated-frame "fallback used" mask (where FG blended instead of warping) | No | Lets relief switch itself off exactly where the generated frame is a blend (nice to have) |
| Per-pixel occlusion / visibility at phase t | No | Same purpose |

### 4.5 Cost and memory budget (proposal) [E]

RTX 5060, 1280×720 game → 1920×1080 output. **These are targets and estimates, not measurements.**

| Pass | Rate | Res | T1 | T2 | T3 adds |
|---|---|---|---|---|---|
| Analyse: luma, prefilter, guided base, band-pass, normalise, gates | per real frame | game | 0.06–0.12 | same | – |
| Analyse: history, protection, frame statistics reduction | per real frame | game | 0.02–0.04 | same | – |
| Analyse: plane + parallax (fit, epipole, γ, fusion) | per real frame | cells | – | – | 0.05–0.10 |
| Apply: H/P fetch (warped for generated frames), normal, shading, cavity | per displayed frame | output | 0.05–0.08 | same | – |
| Apply: parallax march + colour fetch (+ optional self-shadow) | per displayed frame | output | – | +0.06–0.12 | – |
| **Total without FG** | | | **≈ 0.13–0.24** | **≈ 0.19–0.36** | +0.05–0.10 |
| Total with FG 4× (per real frame) | | | ≈ 0.28–0.48 | ≈ 0.52–1.08 | |

**Budget proposal:**
- T2 ≤ 0.5 ms at 1080p output without FG, and T1 ≤ 0.25 ms.
- With FG, the apply cost multiplies. The per-displayed-frame apply must stay ≤ 0.10 ms (T1) or ≤ 0.20 ms (T2). Above
  4× FG, consider T1 only on generated frames (D5).
- At 1440p output, the apply cost scales by 1.78× in pixels.

**Memory [E]** at 720p game → 1080p output:
- H history: 2 real frames × 2 slots, R16F, about 7.4 MB.
- P: R8, 2 frames, about 1.8 MB.
- Blurred-H mip: about 0.6 MB.
- Guided-filter intermediates: RG16F, about 3.7 MB.
- Optional output intermediate: RGBA8 at 1080p, 8.3 MB (zero if Apply writes into the next stage's target).
- T3: cell-resolution textures under 0.5 MB.
- **Total about 14–22 MiB.** No per-frame allocation; all resources are created on start or resize, per project rules.

---

## Part 5: Prior art and licences

| Item | What it does | Depth source | Licence / terms | Use for MoonSR |
|---|---|---|---|---|
| ReShade POM experiment (P. Gilcher / Marty McFly) | POM with greyscale as height | **None (colour)** | None stated [V] | Lesson only: edge and border failures [V] |
| ReSurface (pilzprinz, itch.io) | "PBR" re-lighting: screen-space normals (Sobel/Scharr mentioned), AO, SSR, SSS, POM; "0.5–2.0 ms"; SM 5.0; ReShade 6.1+ [V, https://pilzprinz.itch.io/resurface] | Not stated [U] | Paid; redistribution and reselling prohibited [V] | Market signal only. Never use its code |
| ReShade shader collection | Many post effects | Depth buffer through ReShade's hook (depth effects); colour-only effects exist | No repository-wide licence on the README [V, https://github.com/crosire/reshade-shaders]; each shader has its own [U] | Do not copy. Clean-room |
| NVIDIA Freestyle depth filters: SSAO, Dynamic DOF, SSRTGI (driver 511.23, 14 Jan 2022, made with Pascal Gilcher) [V, https://www.nvidia.com/en-us/geforce/news/god-of-war-game-ready-driver/] | AO, DOF, screen-space ray-traced GI | Game depth through the driver [V "depth-based"; mechanism U] | Proprietary, NVIDIA only | Shows that depth-based relief-like effects are valued. Not reproducible on captures without depth |
| NVIDIA "Details" filter (sharpen, clarity, HDR toning, bloom) [secondary sources only, M] | Local contrast | None | Proprietary | Closest depth-free analogue: "clarity" is a local-contrast boost. T1 must look better than a clarity filter to justify itself (M2 exit) |
| NVIDIA RTX HDR / RTX Dynamic Vibrance | AI tone filters, Tensor Cores [V, https://www.nvidia.com/en-us/geforce/news/nvidia-app-download-and-features/, 2024-11-12] | – | Proprietary | Not relief. "Sharpen+" could not be confirmed in any source [U] |
| AMD Adrenalin (RIS 2, RSR, AFMF 2/2.1, Boost, Chill, Anti-Lag, Integer Scaling) [V, AMD release notes via search, 2026-11-22] | Sharpening, upscaling, FG | – | Proprietary | No relief or depth filter found [I from absence; U for completeness] |
| Intel Graphics Software (Arc) | Image Sharpening (a compute-shader adaptive sharpen, D3D11/12 only), Adaptive Tessellation and Anisotropic Filtering were **discontinued** from IGS 25.22.1502.2 / driver 32.0.101.6913, per an Intel support technician, July 2025 [V, https://community.intel.com/t5/Intel-ARC-Graphics/Latest-driver-32-0-101-6913-removes-options-to-customize-image/td-p/1700342; sharpen description from search excerpt of https://www.intel.com/content/www/us/en/support/articles/000088517.html, M] | – | Proprietary | No relief, depth or lighting filter found. The current Arc Control page lists no game filters [V-search excerpt; U for completeness] |
| Lossless Scaling (Steam page) | LSFG 3, LS1, FSR, NIS, Integer, XBR, Anime4K, dual GPU, profiles | – | Proprietary | **No** depth, relief or lighting feature [V, https://store.steampowered.com/app/993090/Lossless_Scaling/] |
| Owner's removed LSRT addon (Depth Anything V2 Small, DirectML) | Screen-space RT lighting from estimated depth | Monocular network, asynchronous | Model Apache-2.0, ONNX Runtime MIT [H, `ls_reference_motion_depth.md` §2.4] | Measured cost reference; the edge-matching-of-stale-depth idea |
| Khan et al. 2006; Langer & Bülthoff 2000 | Depth from intensity ("dark is deep") | Image | Papers [V/M] | The basis of T1 (clean-room) |
| Tatarchuk 2006; McGuire & McGuire 2005 | POM / steep parallax | Height map | Papers [V] | Basis of the T2 march |
| Longuet-Higgins & Prazdny 1980; Sawhney 1994; Irani & Anandan 1996; Matthies et al. 1989 | Motion parallax, plane + parallax, depth-from-video fusion | Motion | Papers [M] | Basis of T3 |
| Depth Anything V2 Small / Video Depth Anything Small / MiDaS v2.1 small | Monocular depth | Network | Apache-2.0 / Apache-2.0 / MIT [V] | T4 only; user-supplied weights |
| DepthART | Tiny monocular depth | Network | Weights not covered by the CC BY 4.0 grant (CC-BY-NC teacher) [V] | **Do not use** |
| Poly Haven, ambientCG | PBR materials with height maps | – | CC0 [V] | **Synthetic test scenes** (M0), and training data if a learned T1 is ever tried |

**Licence policy for this feature [I]:**
- Everything T1–T3 needs is described in papers. Implement clean-room in MIT directories.
- Do not read or port ReShade shader code, ReSurface or NVIDIA filters.
- T4 weights must be user-supplied. GPL code is irrelevant here. If any appears in references, it stays out of MIT
  directories (CLAUDE.md).

---

## Part 6: Risks, honest limits and measurement

### 6.1 What will look wrong

| # | Failure | Cause | Mitigation | Residual risk |
|---|---|---|---|---|
| R1 | **Embossed outlines** on silhouettes and geometry edges | The band-pass reacts to steps | Guided base, |∇B| gate, motion-discontinuity gate, edge-stopped march | **High.** This is the failure Gilcher called unfixable [V]. Measure the halo metric (6.3) |
| R2 | **Inverted relief** (dark albedo raised or lowered wrongly) | "Dark is deep" is false for albedo patterns | Low amplitude; cavity-heavy mix | **Permanent** for some textures (dark bricks with light mortar) [I] |
| R3 | **Double or contradictory lighting** | The game already shades with normal maps, POM and baked light | Fixed upper-left light; optional "follow scene light"; low amplitude | Medium. Games with strong POM get "double relief" [I] |
| R4 | **Shimmer / crawl** | Aliasing, specular glints, foliage, film grain | Prefiltered luma, σ_floor, history, energy gate | Medium on foliage and aliased games |
| R5 | **Swimming** texture | Parallax-model error, especially under rotation | Offset cap, gradient clamp, T3 rotation split | Medium. Tune by eye (M3) |
| R6 | **HUD / text embossed** | Translucent and animated HUD missed (`KNOWN_ISSUES`) | staticMask, counter, upper energy gate | Medium for translucent HUD |
| R7 | **"Craggy" faces** | Skin micro-detail gets relief | Energy gate, independent motion, skin-locus attenuation | Low–medium |
| R8 | **Generated-frame artefacts emphasised** | FG blends and ghosts get embossed | Scale P by cell confidence; optional FG fallback mask | Low–medium |
| R9 | **Oversharpened look** | Relief ≈ directional high-pass, stacked with sharpening | Apply before sharpen, or reduce sharpen (D6) | Medium. **The effect may simply read as "sharper" or "grittier", not "deeper"** [I]. This is the core product risk |
| R10 | **Dark scenes become noisy** | Normalisation amplifies noise | σ_floor; dark-frame auto-off | Low |
| R11 | **Cost × FG multiplier** | Apply runs per displayed frame | Two-rate design; T1-only on generated frames (D5) | Low |
| R12 | **HDR** | Processing is SDR gamma (A9) | Out of scope until an HDR path exists | – |

### 6.2 When relief should switch itself off

See 3.6. Additionally, stay off by default per game until the owner enables it in a profile (D4).

### 6.3 Measurement plan

**Synthetic scenes with known height** (benchmark-specialist; CC0 materials from Poly Haven or ambientCG [V]):

- **Scene set:**
  - a facing brick wall;
  - a ground plane of rock and cobblestone;
  - a wall-and-ground corner;
  - an occluding object (silhouette test);
  - a sky gradient;
  - a HUD overlay with opaque, translucent and text elements;
  - a flat-shaded (cel) variant;
  - a pixel-art variant;
  - a particle/smoke layer.
- **Two renders per frame:** (a) **flat** texture mapping, which is the input MoonSR sees; (b) the same scene with
  **true POM or a displaced mesh**, as reference. Same camera paths: static, strafe, dolly forward, pure yaw rotation,
  orbit, and a scene cut.
- **Metrics:**
  1. **Height accuracy (M1):** Spearman rank correlation and sign agreement between estimated H and the true height map,
     inside textured surfaces (the ground-truth surface mask), per material. Report the albedo-polarity failures
     separately.
  2. **Relief fidelity (M2/M3):** error to the POM reference (FLIP, SSIM) for flat vs flat + relief. Relief must move
     the image **towards** the reference.
  3. **Edge halo energy:** mean |output − flat input| in a 4-px band around known silhouettes and object boundaries. It
     must stay below a threshold relative to the textured interior.
  4. **HUD integrity:** max |output − input| over HUD pixels. Target 0 for opaque HUD; report translucent HUD separately.
  5. **Temporal:** flicker on static paths (per-pixel temporal std vs input); **warp error** on moving paths (output
     warped by the true motion vs the next output, relative to the same for the input), which captures swimming.
  6. **Cost:** GPU timestamps per pass, at 720p→1080p and 1080p→1440p, without FG and at FG 2× / 4×.
  7. **Auto-disable correctness:** cel and pixel-art scenes must trigger it; textured scenes must not.

**Real games (the owner's eye tests, the owner's preferred evidence):**
- Use `Engine::RecordFrames` [C] to save input and output PNGs with relief off and on (identical seeds, back-to-back
  runs, or the split-screen debug view in one run).
- Per game, cover these situations: brick or stone walls, rocky ground, foliage, faces in cutscenes, dense HUD and text,
  a dark scene, smoke or water, fast camera, a scene cut.
- **Comparison protocol:**
  - flip comparisons (toggle on the same frame), side by side, and short clips;
  - optionally blind A/B with randomised order, recorded as preference plus defect notes;
  - a scoring sheet per situation: "deeper?" and "artefact?" on 1–5 scales.
- **Genre spread:** one first-person game with mostly rotation-only camera, one third-person or orbit camera, one
  open-world ground-heavy game, and one stylised game (it must auto-disable).
- Record hardware, driver, commit and settings with each set (CLAUDE.md). Real-game results are qualitative unless a
  ground truth exists.

**How to judge quality [I]:** relief is worth shipping only if, in the owner's A/B, it is preferred on textured surfaces
**and** produces no defect rated ≥ 3 on silhouettes, HUD or faces in normal play. Synthetic metrics are necessary but
not sufficient: they cannot measure "looks deeper".

---

## Part 7: Milestones and the groundwork contract

Every threshold is a **proposal**. The lead sets the final values.

### 7.1 Milestones

**M0: measurement and test scenes (benchmark-specialist).**
- Build the synthetic scenes of 6.3 (flat vs POM reference renders, camera paths, HUD, silhouettes, sky, cel, pixel
  art, smoke). Implement metrics 1–7.
- Add a "relief off / on / split" capture mode to the owner's real-game routine.
- *Exit:* the scenes and metrics run reproducibly from a command. Baselines are recorded for a "clarity" (local
  contrast) filter and a naive greyscale emboss. These are the bars T1 must beat.

**M1: height estimation (Analyse, T1 source).**
- Implement 1.2 (luma, guided base, band-pass, normalisation, gates) and the height debug view.
- *Exit:*
  - Spearman correlation with true height ≥ 0.5 on at least 3 of 5 CC0 materials;
  - polarity failures documented per material;
  - halo-band energy of H ≤ 25 % of the interior energy;
  - Analyse ≤ 0.15 ms at 720p (measured).

**M2: relief shading (normals, fixed light, cavity, composite).**
- *Exit:*
  - FLIP to the POM reference improves over flat input on brick and rock scenes, and beats the naive-emboss and clarity
    baselines;
  - halo metric within the threshold;
  - HUD integrity 0 on opaque HUD;
  - T1 total ≤ 0.25 ms (measured);
  - **owner A/B preference on at least 2 real games with no defect ≥ 3.**
- **Go/no-go gate for the whole feature.** If the owner does not see "deeper" here, stop before parallax.

**M3: motion-visible parallax (T2).**
- Position-driven offset models (2.3), screen-space march with edge stop, offset cap and gradient clamp.
- *Exit:*
  - warp-error (swimming) increase vs flat input ≤ an agreed small margin on the strafe, dolly and yaw paths;
  - FLIP to the POM reference improves over T1 on the dolly and strafe paths;
  - no new defect ≥ 3 in the owner's tests;
  - T2 ≤ 0.5 ms at 1080p output without FG (measured).

**M4: protection and stability.**
- Complete Part 3: history, motion-discontinuity and confidence factors, generated-frame handling (4.2 step 2 vs step
  3, measured), scene-cut fade, auto-disable with reasons.
- *Exit:*
  - flicker on static paths ≤ the input's flicker + a small margin;
  - cel and pixel-art scenes auto-disable and textured scenes do not;
  - generated frames show no relief flicker at the base frame rate in the owner's FG 2× / 4× clips;
  - scene cuts show no flash.

**M5: tiers.**
- T3 (plane + parallax) as an experimental toggle when motion exists. Measure the gain from its masks on silhouettes
  and characters, and its rotation-only behaviour.
- Decide on T4 from M2–M4 results. Default: do not build.
- Record final costs on the RTX 5060, the RTX A400 and the Radeon 610M (the 610M for correctness only, per the owner's
  note).
- *Exit:* tier table in `BENCHMARKS.md` with measured costs; T3 kept only if it measurably reduces halo or character
  defects.

### 7.2 Minimal contract for the groundwork (proposal; the lead owns the headers)

A proposed new module `src/relief/` with contract header `relief_pass.h` (lead-owned), an implementation owner (likely
the upscaling-engineer or shader-engineer, D8), and shaders under `shaders/relief/`.

```
// Threading: pipeline thread only (owner of the GpuDevice immediate context). No per-frame allocation.
enum class ReliefTier : uint32_t { Off = 0, Shading = 1, Parallax = 2, MotionDepth = 3 };
enum class ReliefLight : uint32_t { Fixed = 0, FollowScene = 1 };
enum class ReliefDebugView : uint32_t { None = 0, Height, Normals, Protection, Offset, Depth, Split };

struct ReliefSettings {
    ReliefTier tier = ReliefTier::Off;
    float strength = 0.35f;        // master 0..1; scales the three below
    float shading = 1.0f;          // 0..1 share of directional relief shading
    float cavity = 1.0f;           // 0..1 share of cavity darkening
    float parallaxMaxPx = 2.0f;    // output pixels, clamped to [0, 3]
    float detailScalePx = 1.5f;    // band-pass centre in game pixels (guided-filter radius derives from it)
    ReliefLight light = ReliefLight::Fixed;
    float lightAzimuthDeg = 135.0f, lightElevationDeg = 45.0f;
    bool protectHud = true, protectEdges = true, protectCharacters = true;  // characters: T3 only
    bool autoDisable = true;       // stylised / menu / dark-frame detection
    ReliefDebugView debugView = ReliefDebugView::None;
    bool operator==(const ReliefSettings&) const = default;
};

struct ReliefStats {               // read with a lag; no CPU stall
    bool active = false;
    ReliefTier tierInEffect = ReliefTier::Off;   // may be lower than requested (e.g. no motion for T3)
    float texturedCoverage = 0;    // share of pixels that received relief, 0..1
    float meanOffsetPx = 0;        // T2+
    bool autoDisabled = false;
    std::wstring reason;           // plain-language reason when not active or reduced
    double analyseGpuMs = 0, applyGpuMs = 0;     // timestamp queries, averaged
};

class ReliefPass {
public:
    static std::unique_ptr<ReliefPass> Create(GpuDevice& device, std::wstring& error);
    virtual void SetSettings(const ReliefSettings& s) = 0;
    // Once per REAL frame, on the ORIGINAL capture (game resolution). `motion` may be invalid or null:
    // T1/T2 then run without history and use the built-in static-pixel counter for HUD protection.
    virtual bool Analyse(ID3D11Texture2D* capture, const RectI& rect, const MotionFieldView* motion, int64_t qpc) = 0;
    // Once per DISPLAYED frame. `src` is the image produced by the previous stage at `dst` size.
    // phase: 1 = the newest real frame; (0,1) = a generated frame between the previous and newest real frames.
    virtual bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
                       const RectI& contentInDst, float phase) = 0;
    virtual void Reset() = 0;      // scene cut, resize, recovery
    virtual ReliefStats Stats() const = 0;
};
```

**Settings ranges and defaults (proposal; the implementation clamps every field) [I]:**

| Field | Range | Default | Notes |
|---|---|---|---|
| `tier` | Off, Shading, Parallax, MotionDepth | Off | MotionDepth falls back to Parallax without motion (`tierInEffect`) |
| `strength` | 0.0–1.0 | 0.35 | Master. Maps to ≤ ±6 % luminance change and to `parallaxMaxPx` at 1.0 |
| `shading` | 0.0–1.0 | 1.0 | Share of directional shading within `strength` |
| `cavity` | 0.0–1.0 | 1.0 | Share of cavity darkening within `strength` |
| `parallaxMaxPx` | 0.0–3.0 output px | 2.0 | Hard cap (2.5 rule 1). Ignored below the Parallax tier |
| `detailScalePx` | 0.75–4.0 game px | 1.5 | Band-pass centre. Larger values pick up bigger bricks and stones and more halo risk |
| `light` | Fixed, FollowScene | Fixed | FollowScene is experimental |
| `lightAzimuthDeg` | 0–360 | 135 | Screen convention: 0 = right, 90 = up, so 135 = upper-left |
| `lightElevationDeg` | 15–80 | 45 | Lower values give stronger shading on slopes |
| `protectHud`, `protectEdges` | bool | true | Debug only when false; the UI should not expose them by default |
| `protectCharacters` | bool | true | Has an effect only in MotionDepth |
| `autoDisable` | bool | true | Stylised, menu, dark-frame and low-confidence rules (3.6) |
| `debugView` | None…Split | None | Split = left half off, right half on, for the owner's A/B |

The derived internal constants (energy gates e0–e3, σ_floor, α, halo dilation) stay internal until M4 has tuned them.
Exposing them would invite runaway combinations (2.5 rule 3).

**Engine-level changes:**
- `PipelineStage::Relief = 3` (appended), with the migration rules of 4.3.
- `EngineConfig::relief` (`ReliefSettings`) and `Engine::SetReliefSettings`.
- `EngineStats::reliefActive`, `reliefNote`, `reliefGpuMs`.
- `StageName` and `StageDescription` entries.
- Profiles store `ReliefSettings` per game.
- **No change** to `FrameGenerator` or `MotionFieldView` is required for the recommended design (4.2). The optional
  requests are in 4.4.

---

## Decisions needed (lead / owner)

- **D1:** Pipeline position default.
  - Recommended: the two-rate end-of-pipeline stage (4.2, position C).
  - Position A (before FG, cheapest) needs a `FrameGenerator::PushFrame` contract change: a separate analysis input and
    interpolation source.
- **D2:** Whether relief gets its own per-stage GPU. Recommended: no; it follows the previous stage.
- **D3:** Scope gate. Recommended: build M0–M2 first. Proceed to parallax (M3) only if the owner's A/B at M2 says it
  looks deeper, not just sharper.
- **D4:** Default state. Recommended: off globally and enabled per game profile, with a low default strength.
- **D5:** With FG above 4×, whether generated frames get T1 only (cost control).
- **D6:** Interaction with sharpening. Apply relief before the final sharpen, or auto-reduce sharpening while relief is
  on.
- **D7:** Whether the engine may start the motion estimator just for relief (0.79 ms at 720p [H]). Recommended: no; T3
  is offered only when motion already exists.
- **D8:** Owner of `src/relief/`: upscaling-engineer (algorithms) with shader-engineer (HLSL), or a new assignment.
- **D9:** T4 (neural depth). Recommended: do not build unless M5 shows a need. If built, user-supplied Apache-2.0
  weights only, with an optional D3D12 / DirectML path.
- **D11:** Load-order migration. Recommended: the `PipelineOrder` validator appends missing stages instead of
  resetting to the default, and `StageAdapters` gains an explicit `relief` entry (4.3).
- **D10:** Product name ("Relief", "Surface depth", "Smart parallax") and whether the UI calls it experimental.

## Claims not verified

- The exact wording of Khan et al.'s "dark is deep" rationale and its citation of Langer & Bülthoff (paper not read; the
  bilateral-filter depth from intensity is verified from the abstract and dissertation record).
- Full texts of Tatarchuk 2006 and McGuire 2005 (existence and summary verified via search excerpts only).
- Longuet-Higgins & Prazdny 1980 details (verified only through secondary citations). Plane + parallax (Sawhney;
  Irani & Anandan), Matthies et al., He et al. guided filter, Belhumeur et al. bas-relief ambiguity, Pentland 1982,
  Ramachandran 1988, Luft et al., Rusinkiewicz et al., Vergne et al., GTAO and HBAO: all from prior knowledge [M].
- The ReSurface depth-buffer use and its normal-generation method (the page does not say).
- The NVIDIA "Details" filter contents (secondary sources only). The existence of a filter named "Sharpen+" could not be
  confirmed anywhere.
- Intel: the July 2025 removal of Image Sharpening is verified from an Intel support technician's forum post, not from
  official release notes. The description of the former sharpen filter comes from a search excerpt. AMD: only
  release-note-level absence of a relief filter.
- The guided filter's box-filter construction (the project page confirms linear time independent of kernel size, not
  the construction).
- The Bend Studio screen-space-shadows code licence (not named on the page).
- The 3D Ken Burns code licence (secondary source only); the 3D Photo Inpainting licence (README copy, not the LICENSE
  file).
- Video Depth Anything's A100 timings: whether per frame or per 32-frame clip.
- RTX 5060 FP32 peak (about 19 TFLOPS, prior knowledge). Every cost in this note is an estimate [E] except the [H]
  anchors.
- The 18 ms Depth Anything V2 Small figure is the owner's LSRT README number recorded in `ls_reference_motion_depth.md`.
  I did not re-run it.
