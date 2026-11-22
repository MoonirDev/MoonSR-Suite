# Motion and depth estimation from captured frames: study of the owner's Lossless Scaling installation and addons

- Author: graphics-researcher (MoonSR Suite)
- Date: 2026-11-16
- Scope: personal-use project; licence compatibility is not a constraint (CLAUDE.md, 2026-11-15). Origins and licences are still noted. This note supersedes the earlier policy line in `capture_tools_and_algorithms.md` ("do not study Lossless.dll shaders"): the owner has now authorised a structure-only look. Nothing here transcribes shader code.
- Evidence tags: **[V]** verified from a local file or tool output cited here; **[I]** inferred by me; **[U]** unknown / not publicly documented.
- Constraints honoured: nothing under the Lossless Scaling folder or `LS Addons` was modified; Lossless Scaling was not run; no DLL was loaded (Lossless_original.dll was only read as bytes and its embedded DXBC containers parsed for reflection; no disassembly, no shader source).
- Paths: `LS` = `C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling`; `ADD` = the owner's local folder of Lossless Scaling addon builds (`LS Addons`).

---

## 1. Headline findings

1. **No addon estimates depth for frame generation.** DLSS-FG (`DLSSMFG`) hands DLSS-G a constant depth texture (every texel 0.5), identity camera matrices and `cameraMotionIncluded = true` [V, `ADD\DLSS-FG\src\engine\dlssg_engine.cpp` lines ~489 and ~595-602]. Moon FG gives ANR FG a flat depth too, and states that making still overlays "nearer" in depth "measured no better, so the depth stays flat" [V, `ADD\Moon-FG\README.md`]. The DLSS Neural Rendering addon passes "constant depth 1" [V, `LS\addons\DLSSNR02\README.md`]. All the quality in both frame-generation addons comes from the **motion vectors**, not from depth.
2. **Lossless Scaling's own LSFG shows no depth resource either [V/I].** Reflection of its embedded compute shaders names inputs and outputs `InputFrame1/2`, `InputFlow`, `InputMask`, `InputUiMask`, `OutputFlow`, `OutputMask`, `OutputMip0..6`, and constants `Timestamp`, `UiThreshold`, `HdrSupport`, `ResolutionInvScale`. No depth-named resource appears in any of the 202 shaders [V]. That LSFG uses none is inferred from names only [I].
3. **The only depth estimator in the owner's stack is Depth Anything V2 Small (ONNX Runtime + DirectML) in the removed LSRT addon**, used for ray-traced lighting, not for frame generation. Measured: 18 ms for a 378x210 estimate, 37 ms at 518x294, 78 ms at 700x392 on an RTX 5060, run asynchronously and a few frames stale [V, `LS\addons\.removed\LSRT-20261111-174455\README.md`]. This is far too slow for a per-frame stage; see section 5.
4. **The measured motion-estimator cost is small.** DLSS-FG/manager block-matching estimator at 1920x1080: pyramid 0.037-0.040 ms, search 0.41-0.46 ms, median 0.003 ms, per-pixel pass 0.12-0.24 ms, so about 0.6-0.75 ms total [V, `LS\logs\LSAddonManager.log.old`, lines "motion estimator: stages ..." and "motion 0.57 ms of 1.08 ms"]. The log does not print the GPU name for those lines; the machine's NVIDIA GPU is an RTX 5060 per `LS\logs\DLSSMFG.log`, so I assume it ran there [I].
5. **Where the quality gain was found, in the owner's own measurements:** (a) disambiguating repeating textures (stairs, fences, tiles) in the whole-picture and per-block search; (b) wide-reach consensus passes; (c) a "steady" temporal-consistency pass; (d) distrust/invalid markers so the interpolator falls back to its own estimate; (e) HUD / still-overlay protection. Section 3 ranks them.

---

## 2. What each local reference is, and what it does

### 2.1 DLSSMFG (DLSS Multi Frame Generation addon; owner's source `ADD\DLSS-FG`)
- Version 1.0.3, author MoonIR, drives NVIDIA `nvngx_dlssg.dll` 310.9.16 through the NGX D3D12 API; RTX 40/50 only; needs HAGS [V, `LS\addons\DLSSMFG\addon.json`, `README.md`].
- Capture frames carry no motion vectors or depth. The addon (a) measures motion with its own copy of the LS Addon Manager FlowEstimator, (b) feeds depth as constant 0.5, identity matrices, `mvecScale = 1` (vectors already in pixels, this frame to the frame before), (c) passes `motionVectorsInvalidValue = 30000` for ambiguous pixels so that DLSS-FG relies on its own optical flow there [V, `dlssg_engine.cpp`].
- Measured on RTX 5060 at 1920x1080 against a synthetic scene with known in-between frames [V, README "Results"; these are the owner's numbers, not independent]:

| Config | PSNR vs truth | GPU per real frame |
|---|---|---|
| Measured motion + lite blending (default), 2x | 39.5 dB | about 8 ms |
| same, 4x | 40.0 dB | about 10 ms |
| same, 6x | 40.3 dB | about 12 ms |
| DLSS-FG optical flow alone (full blending), 2x / 4x / 6x | 34.3 / 33.0 / 29.8 dB | n/a |
| Repeat last frame / mix of two real frames | 18.9-19.0 / 20.4-20.5 dB | n/a |
| Stairs (checkered treads), measured motion, 2x | 28-29 dB (19 dB before the repeating-texture fix) | n/a |

  The README also records that full blending with measured motion scored the same as lite (within 0.1 dB) but took about 27 ms for 6x, and that lite blending without measured motion scored like a plain mix (20.4 dB) [V]. A live log shows the 2x presenter at 0.47-2.76 ms GPU to generate [V, `LS\logs\DLSSMFG.log.old`]; this includes DLSS-FG itself, so it is not a cost for the motion estimator alone.

### 2.2 MoonFG (owner's source `ADD\Moon-FG`; installed `LS\addons\MoonFG`)
- Frame generation by ANR FG (Apex Compute, a DLSS-G-provider DLL the user supplies), up to 6x, motion measured from frames [V, README]. The installed README is byte-identical to the source README (`diff` clean).
- Three motion sources: basic block matching (the manager's estimator), NVIDIA Optical Flow SDK (hardware OFA, one vector per 4x4 px, `src/nvidia_flow.cpp`), and a vendor-neutral compute **MotionRefiner** (`src/motion_refiner.cpp`) that polishes either [V].
- Measured stairs scene, 1920x1080, RTX 5060 (mean level error; lower is better) [V, README; owner's numbers]:

| Motion | Overall | On edges | Motion GPU time |
|---|---|---|---|
| Basic | 1.39 | 10.98 | 0.5 ms |
| Refined (any GPU) | 1.08 | 8.69 | 2.9 ms |
| NVIDIA OF 4x4, medium + consensus | 0.84 | 8.34 | 4.3 ms (mostly on OFA) |
| NVIDIA OF 1x1, medium + consensus | 0.84 | 8.15 | 10.2 ms |
| two real frames blended | 5.22 | 12.85 | n/a |

  At half the frame rate (`--speed 2`): Refined 2.83 -> 2.54 overall (13.38 -> 10.48 on edges) after adding previous-frame motion as a candidate and prior, keeping unchanged pixels still, and a HUD-less frame. Flat, untextured stairs at 30 fps: Refined "does not follow flat, untextured treads at that speed (18.1 overall)"; NVIDIA OF does [V].
- Live log: GPU 2.3-2.6 ms per real frame at 2x, 3.7-4.1 ms at 4x, 5.1-6.6 ms in a heavier run, "motion" 0-40 px average [V, `LS\logs\LSAddonManager.log.old`, "Moon FG:" lines]. These include ANR FG, so they bound, but do not isolate, the motion cost.
- Direct warp-blend of the two real frames by the motion, laid over the neural frame where both agree (`RecordWarpBlend`): flat stairs on-still-text error 2.39 -> 0.69, overall 8.64 -> 7.06 [V]. Idea is generator-agnostic.

### 2.3 DLSSNR02 (DLSS Neural Rendering addon)
- Runs NVIDIA Optical Flow (hardware) on the original capture before LSFG, then a user-supplied `nvngx_dlssnr.dll`; depth constant 1 [V, README]. Live timing at 1920x1080 capture / 1440x810 model: GPU prepare 0.14, **flow 1.7-3.0 ms**, model 2.95, compose 0.16 ms [V, `LS\logs\LSAddonManager.log` "NR timing" lines]. Source in `ADD\DittoSR\.deps\ls-addon-manager\addons\NeuralRendering` (`src/optical_flow.cpp`, grid 4, quality MEDIUM, cost output, temporal hints from previous frame; `shaders/scene_change.hlsl` is a cheap 32x32-sample histogram plus mean-difference cut detector) [V; header says GPL-3.0-only].

### 2.4 LSRT (removed addon; ray-traced lighting)
- Only depth estimator in the stack; see finding 3. Pipeline: half-size linear frame, Depth Anything V2 Small FP16 ONNX (49.6 MB) on its own thread via DirectML, upsampled and **edge-matched to the current frame by colour** each frame so the stale depth follows moving edges; then normals, SSRT, denoise [V, README]. Licences: Apache-2.0 model, MIT ONNX Runtime, DirectML Microsoft redistribution licence.

### 2.5 LS Addon Manager "Upscalers" addon (`ADD\DittoSR\.deps\ls-addon-manager\addons\Upscalers`, MIT, Echo-Storm and contributors)
- Origin of the FlowEstimator used by DLSSMFG and MoonFG (`src/engine/flow_estimator.cpp`). Not read line by line beyond DLSS-FG's copy, which states it changed it only where marked [V, file header].

### 2.6 Lossless Scaling 3.2.2.0 itself (`Lossless_original.dll`, SHA-256 626b196d...f0eb, 7,521,280 bytes; internal name Lossless.dll, company THS)
Structure only, from PE bytes and DXBC reflection (no execution, no disassembly) [V]:
- RTTI class names `LSFG`, `LSFG2`, `LSFG3` exist [V, strings at 0x4c298-0x4c300]. A string `flow_scale` exists (matches the user-visible "Flow scale" setting; LS 3.1 documented it, see `capture_tools_and_algorithms.md`) [V].
- 202 embedded DXBC containers, all Shader Model 5.0 compute shaders, **all 202 byte-unique**; no duplicates. by file order, indices 0-60 and 142-152 are scaling / sharpening / utility kernels (NIS-style `coef_scaler` / `coef_usm` in 128 or 256-thread groups; a change-detect pair using `UpdateMapTexture` / `PreviousFrameTexture`; `InputTexture` to `OutputTexture` filters), and 61-141 plus 153-201 are the frame-generation families [V for the names and groupings, I for the roles].
- **Frame-generation families.** Two cbuffer layouts: an older one (80-byte `cb`: `InputOffset, InputInvDimension, MipInvDimension, FirstIteration, AdvancedColorKind, HdrSupport, ResolutionInvScale`; 81 shaders, file indices 61-141) and a newer one (32/48-byte `cb`: `InputOffset, FirstIter, FirstIterS, AdvancedColorKind, HdrSupport, ResolutionInvScale, Timestamp[, UiThreshold]`; file indices 153-201) [V]. Which is LSFG2 vs LSFG3 vs the current default is not stated in the file [U].
- **Recurring pass shapes** (resource names are verbatim reflection; the interpretation is mine [I]):

| Pass shape (reflection) | Thread group | Reading [I] |
|---|---|---|
| 1 input frame (or 2), outputs `OutputMip0..5` or `OutputMip0..6`, linear sampler | 32x32x1 | One-dispatch image pyramid of 6-7 levels (appears in at least 7 shaders: idx 69, 79, 141, 154, 178, 201 and the 4-input variant) |
| `Input1..N` to `Output1..N` (N = 2, 3, 4 or 6), sampler, no cbuffer in the newest group; 15 shaders of identical size 15592 B; 5 of 55,292 B; 5 of 119,284 B with 6 in / 6 out | 8x8x1 | Repeated, structurally identical stages with different baked constants (weights or per-level parameters). Consistent with a small neural network or iterated refinement over the pyramid levels; the file alone does not prove which |
| `Input1/2/3` + `InputFlow` + `InputMask` + `InputUiMask` to `OutputFlow` + `OutputMask`, 14-25 KB | 8x8x1 | Flow and mask update stages; **flow and a per-pixel mask are explicit resources**, with the UI mask as a third input |
| `InputFrame1` + `InputFrame2` + `InputFlow` (+ `InputMask`) to `OutputFrame` (or `OutputFrame1/2`), 1.9-2.5 KB | 16x16x1 | Tiny final warp/blend pass, so cheap that producing several output frames costs little. Takes `Timestamp` in the newer layout |
| `InputFrame1..4` to `Output1/2`, 8.7-12 KB | 8x8x1 | Four-frame temporal input (two pairs) |

- Shader sizes by role: pyramid 6-14 KB; mid stages 15-32 KB; heavy stages 55-119 KB; final warp ~2 KB. The heavy stages dominate bytecode, but bytecode size is not a cost measurement [I].
- **What this tells us** [I, from structure only]: LSFG (a) builds a pyramid of at least 6 levels, (b) refines a flow field and a mask across levels with many identical-shape stages, (c) treats the UI as a separate mask with a threshold, (d) synthesises each output frame with a trivial last pass at a fractional `Timestamp`. No depth. Same split as NVIDIA's DLSS 4 FG (heavy per pair, light per output frame, see `nvidia.md` section 3.2).
- **Not determinable here** [U]: resolutions of each pass, texture formats, the real pass order, the number of dispatches per frame, the flow-scale mapping, the timings. Formats and dimensions are runtime-created and not in reflection of these shaders.

---

## 3. Techniques we could adopt, ranked by expected benefit versus cost

Benefit/cost is for MoonSR's current design (pyramidal block matching, no depth). Costs are what the cited code reported or what I derived; I did not run any of it. Every item is a **clean-room re-implementation of a described technique** (per project preference for the owner's own and MIT code, reuse is permitted, with origin noted).

| Rank | Technique | Why | Reported cost | Source file |
|---|---|---|---|---|
| 1 | **Repeating-texture-aware global shift and block search** (of all shifts that match nearly as well as the best, take the shortest; then descend to the bottom of that shift's valley; per block, the one nearest the whole-picture shift among near-equal matches; thresholds `kGlobalNear 2.0 + 0.02`, `kBlockNear 2.0 + 0.02`) | Stairs, fences, grates and tiles are the dominant failure of block matching on captures; DLSS-FG went from 19 dB to 28-29 dB on the stairs test with this alone | Included in the 0.41-0.46 ms search at 1080p; global pass is a +-20 px cost surface at the smallest level | `ADD\DLSS-FG\src\engine\flow_estimator.cpp` (search, global shaders) |
| 2 | **Whole-picture shift candidate at the coarsest level** (checkerboard-sampled SAD over +-20 px at about 120x68, seeds every block; fast camera turns past the per-level search radius) | Cheap; fixes fast pans that exceed the per-level radius | Part of the pyramid cost above | same file, `kGlobalHlsl` |
| 3 | **Per-pixel final selection at full resolution** between "not moving", own block's vector and the three nearest blocks' vectors on a 3x3 patch, with a small bias favouring own block and "not moving" winning ties | Gives object-edge-following motion and keeps a static HUD static; no extra search | 0.12-0.24 ms at 1080p | same file, `kPixelHlsl` |
| 4 | **Distrust / invalid mask**: where even the best vector leaves the neighbourhood unlike the previous frame (kTrusted 0.03 -> kUntrusted 0.10 SAD); output marked as invalid so the interpolator uses a fallback (blend, or neural FG's own flow) | Disocclusions, transparent effects and wrong estimates are what generate visible tearing; this is the control signal for graceful degradation | Computed in the same per-pixel pass | same file; `dlssg_engine.cpp` (`kInvalidMotion = 30000`) |
| 5 | **Consensus refinement passes of shrinking reach** (64, 32, 16, 8, 4 px: each pixel re-tests its own vector, eight neighbours' at that distance, their median and "not moving" on a sparse 3x3; cost for straying from the median 0.01/px) | Carries motion from unmistakable regions across a repeating pattern; measured 1.39 -> 1.08 overall, 10.98 -> 8.69 on edges | **2.9 ms total motion** (vs 0.5 ms basic) at 1080p on RTX 5060; heavy. Use selectively (quality tier, or only on low-confidence tiles) | `ADD\Moon-FG\src\motion_refiner.h/.cpp` |
| 6 | **Previous-frame motion as candidate and prior; steady-motion consistency pass** (robust mean of the measured vectors on 5x5 taps 8 px and 32 px apart, chosen by a matching cost; invalidate when neither measured nor steady wins clearly) | A camera moves smoothly; resolves ambiguity on repeating patterns and at low frame rate (`--speed 2` 13.38 -> 11.68 on edges with history alone) | One extra full-res pass; ~0.1-0.3 ms [I, not logged separately] | `dlssg_engine.cpp` `kSteadyHlsl`; `motion_refiner.h` `history` |
| 7 | **Still-overlay (HUD) detection and HUD-less frame**: a pixel that did not change at all but whose neighbours' motion would move it is held still; the frame with those pixels filled from around is given to the interpolator, then the overlay is laid back over the result | Directly addresses text smearing that block-matching FG shows over moving ground; measured on-still-text error 2.47 -> 1.49 (Refined) and 2.39 -> 0.69 on flat stairs with warp-blend | Small full-res passes | `ADD\Moon-FG\src\motion_refiner.h` (`RecordStillDepth`, `RecordHudless`, `RecordWarpBlend`) |
| 8 | **Direct two-frame warp blend** laid over a base interpolation wherever the two warped real frames agree (a point is read from both real frames where the motion says it was, a small +-1 px search for thin lines) | Cleans thin, fast features that a neural or simple generator breaks; generator-agnostic | One tile-local pass per output frame | `motion_refiner.h` `RecordWarpBlend` |
| 9 | **Scene-cut detector**: 3x16-bin histogram distance plus mean same-position RGB change on 32x32 samples in one workgroup | Almost free; gates `reset` of temporal history | Negligible | `...\NeuralRendering\shaders\scene_change.hlsl` (GPL-3.0-only header) |
| 10 | **Optional NVIDIA OFA motion path** (4x4 grid, MEDIUM, with temporal hints), then bring to every pixel through the consensus refiner | Best result on flat/untextured treads at low fps; runs on separate hardware | 4.3 ms total (most on OFA) incl. consensus; OFA alone 1.7-3.0 ms in the NR addon | `ADD\Moon-FG\src\nvidia_flow.cpp`; `...\NeuralRendering\src\optical_flow.cpp` |
| 11 | **Structure ideas from LSFG's reflection** (heavy once-per-pair flow/mask refinement over a >=6 level pyramid; explicit mask resource; UI threshold; trivial final warp at a `Timestamp`; reduced-resolution flow selected by `ResolutionInvScale`) | Confirms the architecture split we already use; "flow scale" is a cost lever | Unknown (not measurable here) | `LS\Lossless_original.dll` (reflection only) |

Adopt now: 1-4 (these refine the estimator we already have, at under 1 ms), then 7 and 8 for HUD and thin-line quality. Hold 5 as a quality tier. Keep 10 optional and vendor-specific, never mandatory (project rule).

---

## 4. Depth, specifically

- **Is depth needed?** For the owner's two frame generators, evidence says depth gave no measured gain: Moon FG tested near-depth for still overlays and found it "no better" [V]; DLSS-FG uses a constant. What DLSS-G does with depth internally is not public [U]; DLSS-FG's docs list depth as a required input (`nvidia.md` section 3.4), so a better-than-constant depth could in principle help a neural generator's disocclusion handling, but nothing local shows this [U].
- **Depth proxies that are cheap enough** (none measured by me, all [I]): (a) the motion field itself in a translating camera is inversely proportional to depth (parallax), so a relative depth ordering can be derived for free from the flow after global-motion removal; (b) vertical screen position as a ground-plane prior (LSRT's "basic" mode is a rough guess from brightness and screen position) [V for the existence of the basic mode; its quality is not stated]; (c) only the occlusion ordering at motion edges matters for interpolation: which side of a motion boundary is nearer, which the flow divergence already hints at.
- **Monocular network depth** (Depth Anything V2 Small): 18 ms at 378x210 on RTX 5060, asynchronous and stale [V]. Not viable per frame in our budget, and it models the HUD as foreground [V, LSRT limits]. Only plausible as a slow background prior refreshed every N frames and edge-matched to the current frame, which is what LSRT already does. I would not adopt it for frame generation without a measurement showing a gain over flow-only ordering.

---

## 5. Open questions

1. Which pass in LSFG is the flow estimator and at what resolution it runs (the DLL's reflection does not give formats or sizes). A profiling capture (PIX / Nsight) of LS running is the only way; the task forbids running LS, so this stays open.
2. GPU for the "motion estimator: stages" log lines (RTX 5060 assumed).
3. Whether the manager's estimator in `LS\addons` is still the version DLSS-FG copied (DLSS-FG says it is a copy "at the pinned commit").
4. Whether real depth helps neural FG on captures (needs an A/B with ANR FG or DLSS-G, with estimated vs constant depth).
5. Stage-level timings for the steady pass, consensus passes and still-overlay passes (the READMEs give totals only).

## 6. Claims I could not verify

- LSFG "is a neural network" (the repeated identical-shape shaders are consistent with it; not proven).
- Which cbuffer family belongs to LSFG 2, 3 or the current default.
- Any LSFG timing, resolution or format.
- The owner's PSNR/error numbers are from the owner's own synthetic probe scenes, recorded in README files; I did not rerun them.
- GPU name behind the estimator stage timings.
