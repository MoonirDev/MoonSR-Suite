# MoonSR frame generation: audit of base interpolation quality, survey of transferable techniques, and a plan

Author role: graphics-researcher. Date: 2026-11-21. Scope: the fixed (non-adaptive) frame generator,
`src/framegen/frame_generator.*`, `shaders/framegen/*`, and the shared motion estimator. No engine code is written
here. The per-game adaptive learner (`adaptive_*`) is being removed elsewhere. It is mentioned only where it found
something that matters for base quality (section 1.5).

## Provenance legend (read first)

| Tag | Meaning |
|---|---|
| **[C]** | Verified by reading this repository's code at commit `814bc0d` (file named). |
| **[R]** | **Re-measured in this session** (2026-11-21) with the commands in section 1.1 on the `build/main` binaries (built 2026-11-21 20:08, after the last framegen commit). RTX 5060, driver reported by `nvidia-smi` as 621.14, isolated GPU, Performance tier. Raw CSVs are in the session scratchpad and are **not** in `BENCHMARKS.md`: the lead decides what to record there. |
| **[H]** | Historical measurement recorded in `documentation/BENCHMARKS.md` or `KNOWN_ISSUES.md` (date given). |
| **[V]** | Verified this session from the cited primary source (accessed 2026-11-21). |
| **[M]** | From the researcher's prior knowledge or from an earlier research note (named), **not re-fetched this session**. |
| **[I]** | Inference or engineering reasoning. |
| **[E]** | Cost or quality **estimate**. Not a measurement; must not appear in `BENCHMARKS.md`. |
| **[U]** | Unknown or not publicly documented. |

Related notes this file builds on, without repeating them: `capture_tools_and_algorithms.md` (survey of competitors,
classical and neural flow, Part 2), `ls_reference_motion_depth.md` (the owner's addons and LSFG reflection, ranked
technique list), `nvidia.md` §3-4 and §7-8 (DLSS FG, Smooth Motion, NvOFFRUC), `amd_intel.md`, and `alsr_v2.md`
(the ALSR consumer of the same motion field).

---

## Part 1: Audit of the current generator

### 1.1 What was measured today [R]

Commands (from the repository root; `S` = scratchpad directory):

```
build\main\bin\moonsr_fgbench.exe --quality --adapter 5060 --source build\main\alfg-real-frames\in_0123.png --configs p:0.5,p:1 --out-dir S --tag audit
build\main\bin\moonsr_fgbench.exe --triplet in_0N.png,in_0N+1.png,in_0N+2.png --adapter 5060 --configs p:0.5,p:1      (N = 123..144, 22 triplets)
build\main\bin\moonsr_imgtool.exe diff in_0N.png in_0N+1.png d.png                                                       (repeat-nearest baseline per triplet)
build\main\bin\moonsr_fgbench.exe --time --adapter 5060 --tiers p --flow-scales 0.5,0.75,1 --time-sizes 1920x1080 --iterations 100 --warmup 20 --source ...in_0123.png
build\main\bin\moonsr_fgbench.exe --time ... --flow-scales 1 --precise-motion
```

`build/main/alfg-real-frames/` holds 24 consecutive **real** ATLYSS captures at 1920x1080, spaced 14.2-18.9 ms
(median about 16.7 ms, i.e. a roughly 60 fps game) [C, `index.csv`]. A triplet (N, N+1, N+2) interpolates N and N+2
and compares the result with the held-out real frame N+1. That is a real-game 30 → 60 test at t = 0.5 with a true
ground truth (only the capture-time spacing is approximate).

**Cost, 1920x1080, Performance tier, median ms [R]:**

| Flow scale | PushFrame (per real frame) | Interpolate (per generated frame) | 2x pair |
|---|---|---|---|
| 0.50 | 0.839 | 0.266 | 1.106 |
| 0.75 | 1.482 | 0.280 | 1.763 |
| 1.00 | 2.249 | 0.258 | 2.507 |
| 1.00, precise motion (used at 3x and above) | 2.320 | 0.262 | 2.583 |

**Real-game held-out triplets, 22 ATLYSS triplets [R]:**

| Method | Mean PSNR | Mean SSIM | Worst triplet PSNR |
|---|---|---|---|
| Repeat nearest (frame N shown at t = 0.5) | 22.78 dB | not measured | 18.62 dB |
| MoonSR FG, flow 0.50 | **26.26 dB** | **0.8586** | 20.51 dB |
| MoonSR FG, flow 1.00 | 26.21 dB | 0.8521 | 20.53 dB |

- The linear-blend baseline was not computed for the real triplets (the tools have no command for it, and the
  machine's Python has no numpy). That is a harness gap (section 3.1).
- Flow 0.5 scored equal or higher SSIM than flow 1.0 on **every** one of the 22 triplets, and higher or equal PSNR on
  17 of 22 (100 % was higher by at most 0.08 dB on the other 5) [R]. On this content, the 1.4 ms extra per real frame at 100 % flow buys nothing measurable.
- MoonSR gains +3.5 dB over repeat-nearest on real content, against +10 to +17 dB on most synthetic sequences
  (next table). [I] Most of the synthetic headroom does not transfer: real frames contain non-rigid motion,
  particles, lighting change, motion blur and the HUD, all of which the synthetic suite lacks or under-represents.

**Synthetic suite, PSNR dB at flow 1.0, per phase t (whole frame / edge pixels) [R]:**

| Sequence | t = 1/4 | 1/3 | 1/2 | 2/3 | 3/4 | Repeat-nearest mean | Linear-blend mean |
|---|---|---|---|---|---|---|---|
| pan_subpx (0.4, 0.25 px) | 41.1 / 34.3 | 39.1 / 32.2 | 37.8 / 30.7 | 41.4 / 34.4 | 41.4 / 34.1 | **43.15** | 40.82 |
| pan_5px | 33.0 / 26.0 | 39.2 / 32.7 | 37.1 / 30.2 | 33.5 / 26.6 | 32.4 / 25.4 | 22.18 | 23.44 |
| pan_20px | 33.6 / 29.9 | 33.0 / 28.1 | 37.0 / 34.6 | 34.0 / 27.9 | 35.2 / 29.4 | 17.81 | 18.89 |
| pan_45px | 33.8 / 30.2 | 33.4 / 30.2 | 31.6 / 25.7 | 32.3 / 27.1 | 32.1 / 26.6 | 17.27 | 18.41 |
| hud_pan | 32.7 / 29.1 | 32.1 / 27.7 | 34.3 / 32.1 | 32.4 / 27.3 | 33.4 / 28.7 | 17.98 | 19.06 |
| layers_occlusion | 34.7 / 28.8 | 35.2 / 29.1 | 32.3 / 26.2 | 34.9 / 29.2 | 34.1 / 28.1 | 19.65 | 20.72 |
| small_fast_object | 35.0 / 28.7 | 32.2 / 25.5 | 31.6 / 25.1 | 34.9 / 28.6 | 34.9 / 28.3 | 24.04 | 25.32 |
| zoom_rotate | 28.8 / 26.3 | 29.0 / 25.7 | 29.6 / 25.3 | 30.8 / 25.8 | 31.8 / 26.6 | 17.72 | 18.81 |
| periodic_fence | 30.2 / 31.0 | 32.0 / 34.0 | 30.4 / 26.1 | 29.3 / 25.6 | 27.9 / 24.4 | 12.63 | 14.64 |
| real_turn | 29.3 / 19.4 | 29.7 / 19.5 | 29.8 / 19.3 | 30.0 / 19.5 | 29.9 / 19.6 | 21.40 | 22.54 |
| real_rotate | 29.7 / 19.6 | 29.7 / 19.4 | 29.6 / 19.6 | 29.3 / 19.3 | 29.2 / 19.5 | 20.27 | 21.35 |
| real_layers | 34.0 / 22.6 | 33.7 / 23.0 | 34.0 / 23.0 | 33.7 / 22.6 | 34.9 / 24.2 | 25.63 | 26.82 |

- The `real_*` sequences use real image content with exactly known synthetic motion. They are not real-game motion.
- HUD-region PSNR on hud_pan: **45.7 dB at flow 0.5, 72.3 dB at flow 1.0** (mean over t) [R]. That is the one
  measured place where flow 1.0 is clearly better; on real_layers the difference is 65.2 vs 68.3 dB.
- scene_cut: output at t = 0.25 equals A and at t = 0.75 equals B exactly (max diff 0) at both flow scales [R].

### 1.2 Pipeline as shipped

The application always runs the **Performance** tier (`fg.settings.quality = FrameGenQuality::Performance`,
`src/app/main_window.cpp:1153`) [C]. The default flow scale is 50 % (`settings.h:38`), user-adjustable 25-100 % [C].
"Precise motion" (one sub-pixel step) is switched on automatically when 3x or more is requested
(`engine.cpp:1089`) [C]. Balanced and Quality tiers are reachable only from the benchmarks.

So the shipped configuration is: no census term, search radius 2 (6 at the coarsest level), 4 refinement
candidates, 0 sub-pixel steps (1 with precise motion), 4 interpolation candidates, **bilinear** final fetch
(`cubicWarp = false`), no long-range propagation (`Tier()`, `frame_generator.cpp`) [C].

**Per real frame (`PushFrame` → `Analyse`) [C]:**

| # | Pass (shader) | Resolution | What it decides | Cost [H/R] |
|---|---|---|---|---|
| 1 | Copy into the 2-deep history | frame | – | small |
| 2 | `fg_luma_cs`, `fg_downsample_cs` | flow, then pyramid | Area-averaged BT.709 luma at flow resolution with n = ceil(frame/flow) bilinear taps per axis (an exact box only for integer ratios); [1 3 3 1]/8 pyramid until the coarsest level is ≤ 96 px wide (5 levels at 1080p / 50 %) | small |
| 3 | `fg_histogram_cs` | level 1 | 9 regions × 16-bin luma histograms for the cut test | small |
| 4 | `fg_search_cs` (per level, both directions in one dispatch) | 8×8 blocks per level | 11 prediction candidates (9 parents ×2, zero, previous pair's vector), then exhaustive integer search (2r+1)² around the prediction. Cost = zero-mean RMS (+ census off in Performance) + 0.1·L1 to the prediction. Near-best rule: among offsets within 1.0 + 5 % of the best, take the one closest to the prediction, then descend its valley. V-model sub-pixel (`SubPixel`, `fg_common.hlsli`). HUD-weighted correspondences using the **previous** pair's mask | 0.72 ms (75 %) / 1.27 ms (100 %) at 1080p, profiler [H 2026-11-19] |
| 5 | `fg_regularize_cs` | blocks | 3×3 neighbour consensus scored by the data term + truncated L1 to the neighbours | 0.37 / 0.64 ms [H] |
| 6 | `fg_refine_cs` | 4×4 cells of level 0 | Per cell: best of 4 (or 9) block vectors over a 6×6 window; 0-2 sub-pixel steps | 0.26 ms [H] |
| 7 | `fg_cross_cs` (splat + main) | cells | A vector found in one direction is offered (negated) to the other at its landing cell; accepted only if it lowers RMS by 20 % | 0.01-0.02 ms [H] |
| 8 | `fg_consistency_cs` | cells | confidence = exp(-½(e/τ)²) forward-backward consistency (nearest-cell lookup, τ = 0.75 + 0.1·\|v\| flow px) × match term from rel = rms/(σ+2); pair statistics | small |
| 9 | `fg_motionfrac_cs`, `fg_hudmask_cs` | frame | "Unchanged, and not explained by the surrounding motion", with 3×3 patch test, neighbour count, 7×7 edge dilation and a "surroundings moving" gate | 0.19-0.20 ms at 1080p, near-constant [H] |
| 10 | `fg_scenecut_cs` | 1 thread | cut = badFrac > 0.6, or histogram distance > 0.5 with badFrac > 0.3 (flat images: histogram only) | negligible |

Cell size in frame pixels = 4 / flowScale: **8 px at 50 %, 4 px at 100 %** [C].

**Per generated frame (`Interpolate(t)`) [C]:**

| # | Pass | Resolution | What it decides |
|---|---|---|---|
| 1 | `fg_splat_cs` | cells | Forward-splat both directions' cells to time t (footprint ±0.75 cell). Collisions: `InterlockedMax` on key = confidence (10 bits) ‖ direction ‖ cell index. **The more confident vector wins; there is no occlusion order.** |
| 2 | `fg_resolve_cs` | cells | Decode; holes take the max key within radius 1, then 2 (confidence halved), else zero motion with confidence 0 |
| 3 | `fg_pixsel_cs` | half frame | Per 2×2 pixels: smooth (bilinear) candidate + 4 nearest t-cells. Score = both·patchErr + (1−both)·occCost(10) + 8(1−conf) − 0.5 (smooth bonus). Patch error: 3×3 taps 2 px apart on the **flow-resolution** luma. Visibility per side from the pair's own fields; "explained occlusion" raises confidence for one-sided vectors whose other side is confidently covered |
| 4 | `fg_interpolate_ps` | frame | Smooth region (spread < 1 px): one warp with bilinear-upsampled motion, **bilinear fetch** in Performance. Motion boundary: evidence-weighted blend of the 4 hypotheses. Fallback: lerp(blend or nearest, warp, saturate((conf−0.1)/0.5)). "Keep unchanged" rule for unverifiable warps over unchanged pixels. HUD: lerp toward the plain blend by the mask. Cut: nearer real frame |
| 5 | `fg_aa_ps` | frame | Guided outlier clamp and edge-tangent smoothing where the guide marks uncertainty |

The design already follows the split recommended by every source: heavy analysis once per pair, light synthesis per
phase, every phase from the original pair (A11). That is the right skeleton. The weaknesses are in what each pass
decides.

### 1.3 Weaknesses, with evidence

**F1. Phase-dependent error: sharpness and accuracy pulse across the phases of one interval.**
- [R] Within a single uniform pan, PSNR changes by up to 6.8 dB from phase to phase: pan_5px 33.0 / 39.2 / 37.1 /
  33.5 / 32.4 dB and edge PSNR 26.0 / 32.7 / 30.2 / 26.6 / 25.4 dB at t = 1/4 … 3/4. pan_20px swings 33.0-37.0 dB,
  periodic_fence 27.9-32.0 dB.
- [R] On sub-pixel motion, repeat-nearest beats MoonSR by 10 dB at t = 1/4 (51.2 vs 41.1 dB). This is the
  `KNOWN_ISSUES` item "worse than repeating the nearest frame at t ≤ 1/3" [H 2026-11-13], still present.
- [C] Causes that fit the code: (a) the shipped tier fetches **bilinearly** (`cubicWarp` off in Performance), and
  bilinear blur depends on the fractional part of t·m, which changes with every phase; (b) the two warped samples
  are blended with weight t, so any motion error becomes a phase-dependent double image; (c) the half-resolution
  selection and its bilinear upsampling are re-decided per phase.
- [I] Which of these dominates is **not isolated**, because the harness cannot run the synthesis with the true
  motion (section 3.1, "oracle flow"). This is the first thing to measure.
- [I] Why it matters more than the mean: at 3x-6x the viewer sees the phases in sequence. A sharp real frame
  followed by generated frames of varying blur reads as shimmer at the base frame rate. The user's reports of
  "slightly more visual artifacts" than Lossless Scaling at 30 → 90 [H 2026-11-18/19] are consistent with this but do
  not prove it.

**F2. Occlusion ordering: splat collisions are won by confidence, not by which surface is in front.**
- [C] `fg_splat_cs` resolves collisions with `InterlockedMax` on a confidence key. The forward-backward term makes
  the side that disappears inconsistent, which usually lets the occluder win. But both surfaces are consistent a
  few cells away from the boundary, and then the winner is whichever has the higher match score.
- [R] layers_occlusion is worst at t = 1/2 (32.3 dB, edge 26.2) and best near the ends (34.7-35.2 dB). That is the
  signature of occlusion handling: at t = 1/2 the occluded strip is largest on both sides.
- [V] For comparison, FSR 3's frame interpolation packs a priority into the high 16 bits of each scattered vector:
  a primary/secondary bit, a 10-bit **depth-distance** term and a 5-bit colour-similarity term
  (https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/). Its optical-flow field uses vector
  magnitude for high priority and colour similarity for low priority (same page). MoonSR has no depth, but a
  capture-only occlusion order is available (section 2.3).

**F3. Motion is modelled per 8-pixel cell (at 50 % flow) as translation, and selected on flow-resolution luma.**
- [C] One translation per cell; per-pixel motion is a bilinear blend of half-resolution selections. The selection
  patch error is computed on the flow-resolution pyramid (`gLum0/1` = `pyr_[..][0]`), so detail finer than the flow
  resolution cannot influence which hypothesis wins.
- [R] zoom_rotate 28.8-31.8 dB, real_rotate and real_turn edge PSNR about 19.5 dB, against 26-30 dB edge PSNR on
  pure pans.
- [H] `KNOWN_ISSUES`: "thin moving structures (pillar edge) still have wrong motion"; "Zoom / rotation: edge
  misregistration (per-cell translation model)".

**F4. Small fast objects and thin features lose to the background.**
- [H] `KNOWN_ISSUES`: objects under about 2 coarse blocks moving more than their own size are not tracked and
  appear as two half-transparent copies. The cross-direction pass (pass 7) helps only when one direction finds the
  object.
- [R] small_fast_object drops to 31.6 dB at t = 1/2 against 35.0 at t = 1/4. Whole-frame PSNR hides how bad the
  object itself is. No object-region metric exists (section 3.1).
- [I] An 8×8 block at 50 % flow covers 16×16 frame pixels. A 1-2 px wire or pillar is a small minority of that
  block, so the block takes the background's vector, and the refinement candidates come from the same blocks.

**F5. The low-confidence fallback is a linear blend, which is the ghosting artefact itself.**
- [C] Where confidence is low, output = lerp(lerp(A, B, t), warp, …) (`fallbackMode_ = 0`, blend). A plain blend
  shows two half-transparent copies of anything that moved.
- [I] On real content a much larger share of pixels reaches this branch than on the synthetic suite, which would
  explain part of the gap between +3.5 dB (real) and +13 dB (synthetic) over repeat-nearest. The share of fallback
  pixels is **not measured**: the generator exposes only mean confidence (`FramePairInfo`), not fallback coverage.
- [V] FSR 3 does not blend unrelated frames as a fallback. It keeps holes and fills them from an inpainting pyramid
  of the interpolated colour ("an average of the nearest valid colours"), and blends game-vector and optical-flow
  results by colour similarity (same GPUOpen page).

**F6. HUD protection depends on the flow resolution, keeps no memory, and does not model translucency.**
- [R] HUD-region PSNR on hud_pan is 45.7 dB at 50 % flow and 72.3 dB at 100 %. The mask's "surroundings moving"
  gate and the motion it compares against come from the cell field, so coarser cells blur the overlay boundary.
- [C] The FG mask is recomputed from each pair alone. The search uses the previous pair's mask as weights, so it lags
  one pair. ALSR keeps a persistent overlay-evidence counter (+32/255 per unchanged frame, −8 otherwise,
  `alsr_v2.md` §1.2 pass 0); the generator does not.
- [H] `KNOWN_ISSUES`: semi-transparent overlays and anti-aliased overlay edges tear or ghost; animated HUD elements
  are warped.
- [M, `ls_reference_motion_depth.md` item 7] The owner's Moon FG addon measured on-still-text error 2.47 → 1.49 by
  building a HUD-less frame, interpolating it, and laying the overlay back. No equivalent exists here.

**F7. Scene cuts are correct in the picture but leak into the next pair; duplicate frames are not detected.**
- [R] The cut itself is handled exactly (nearer frame, max diff 0).
- [H] Open review item: the temporal candidate (the previous pair's field, `cbSearchT_`) survives a cut, so the first
  pair after a cut is seeded with unrelated motion. [C] `Analyse` sets `prevFinalValid_ = true` unconditionally,
  and the cut decision is made on the GPU after the search.
- [H/C] Duplicate frames (a game presenting the same image twice) are not detected. The pair then has zero motion,
  and the next pair has double motion. That produces a frozen step followed by a fast step, and the temporal
  candidate is wrong for both.

**F8. Multi-frame (3x-6x) consistency is not designed for, and not measured.**
- [C] Every phase is computed from the original pair, so there is no error accumulation. That is correct.
- [C] But the hypothesis selection (`fg_pixsel_cs`) and the boundary blend are re-decided per phase with
  t-dependent scores. A boundary can therefore move non-monotonically between consecutive generated frames.
- [C] The motion model is linear (constant velocity) between the two real frames. The previous pair's field is
  kept (`final_[prevFinal]`) but used only as a search candidate, not for acceleration.
- [H] `moonsr_imgtool temporal` measures the error of the change between two generated phases against ground truth
  (used on 2026-11-18). There is no metric over a whole 4x or 6x sequence.
- [I] Cost: synthesis is about 0.26 ms per generated frame at 1080p [R]. At 6x that is 1.3 ms per real frame, more
  than PushFrame at 50 % flow. Per-phase work is the cost that grows with the multiplier.

**F9. Large motion is bounded and has no global candidate.**
- [C] At 1080p / 50 % flow, the coarsest level is 60 px wide. Radius 6 there covers about ±192 frame px, plus
  about ±30 px from the finer radius-2 levels, plus the temporal candidate [I, arithmetic from `Tier()` and
  `AllocateFlowResources`]. That is enough for most 60 fps camera turns but not for fast flicks at 30 fps.
- [C] There is no whole-picture shift candidate. [M, `ls_reference_motion_depth.md` item 2] The owner's DLSS-FG
  estimator adds a checkerboard-sampled ±20 px global SAD at about 120×68 that seeds every block, which fixes
  pans that exceed the per-level radius at negligible cost.

**F10. The 75 % flow failure points at a luma resampling problem, not only a flow-scale choice.**
- [H 2026-11-18] periodic_fence at the midpoint: 30.37 dB at 100 % flow, **13.90 dB at 75 %**, 31.25 dB at 70 %.
  It persists with 1-16 frames of history. Census, coarse cutoff and margin changes did not fix it.
- [C] `fg_luma_cs` takes n = ceil(frame/flow) bilinear taps per axis. At 75 % that is n = 2 over a 1.33 px
  footprint, which is not a box filter. The tap positions also beat against the 14 px post period.
- [I] A non-integer, unevenly weighted pre-filter is a plausible cause of moiré at exactly some ratios. That fits
  "75 % fails, 70 % works" better than a search-parameter cause. **Untested.** The fix (an exact area filter for any
  ratio) costs almost nothing if this is the cause.

**F11. Where the cost goes, and the obvious lever.**
- [R] At 1080p, PushFrame is 0.84 ms at 50 % flow and 2.25 ms at 100 %. Interpolate is about 0.26 ms either way.
- [H] Search and regularisation dominate the analysis (1.27 + 0.64 ms of the 100 % profile). The HUD pass is about
  0.19 ms regardless of flow scale.
- [R] On the 22 real triplets, 100 % flow is not better than 50 % (26.21 vs 26.26 dB, lower SSIM on all 22). The
  owner's live comparisons with Lossless Scaling used **100 % flow + precise motion** [H 2026-11-19], where MoonSR
  measured 19.7 % of the RTX 5060 3D engine versus LS 7.0 % [H]. That is not a controlled comparison, but it says
  the owner's working point is the most expensive one, and that the extra cost does not show up in the real-triplet
  score.

### 1.4 What works and should be kept

- [C] The per-pair / per-phase split, synthesis from the original pair only, and no CPU readback in the frame path.
- [R] Large gains over both baselines on every moving synthetic sequence (+10 to +17 dB) and +3.5 dB on real
  triplets. Exact scene-cut output. Exact static-HUD reproduction at 100 % flow.
- [C/H] Measured, documented choices: the [1 3 3 1] pyramid (moiré fix), the near-best shortest-shift rule (fences),
  the V-model sub-pixel step, zero-mean costs (brightness_pan holds at 32.7 dB), the cross-direction candidate, the
  "keep unchanged" rule, patch sigma 32 (+0.9 dB over 12), and the measurement discipline in `BENCHMARKS.md`.

### 1.5 What the adaptive learner found that belongs in the fixed algorithm

The learner itself is being removed. These results should survive it [H, `BENCHMARKS.md` 2026-11-17 to 11-19,
`FRAMEGEN_LEARNING.md`]:

1. **Held-out triplet evaluation on real captures** is the only real-game quality measure the project has. It now
   exists independently as `moonsr_fgbench --triplet`, and the frame recorder (`src/diag/frame_recorder.h`, used for
   `build/main/alfg-real-frames`) is engine diagnostics, not part of the learner [C]. Both must stay. Today's 22-triplet
   run is built on them.
2. **Flow-scale sensitivity is non-monotonic** (the 75 % fence failure, F10). That is a base-quality defect to fix,
   not a setting to learn around.
3. **Tile scores mislead:** all nine 160×160 crops scored higher at 75 % while the full frame lost 16 dB. Quality
   decisions must use full-frame metrics.
4. **Precise motion pays at higher multipliers:** +0.27 dB on real_turn at 1/3 and 2/3, fewer object glitches in the
   user's ATLYSS test, for 0.15-0.3 ms. It is already folded in (automatic at 3x and above, `engine.cpp:1089`).
5. Nothing in the learner changed the algorithm's decisions per pixel, so no other per-pixel logic needs folding in.

---

## Part 2: Techniques that transfer to a capture-only path at this cost

The constraints: two final frames (plus the previous pair's analysis), no depth, no motion vectors, a composited
HUD, D3D11 FL 11_0 / SM 5.0, and a budget equal to today's cost (section 3.0).

### 2.1 Reference implementations, what they do, and what transfers

**AMD FidelityFX Frame Interpolation 1.1.3 and Optical Flow 1.1.2 (FSR 3 frame generation)** [V]
- Sources: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/,
  https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/
- **Optical flow:** colour → luma; full-resolution luma plus 6 downscaled levels; 7 iterations of search / filter /
  upscale. Search: every offset in a 24×24 window (256 candidates), luma SAD over 8×8 blocks, centred on the previous
  estimate. Filter: the 3×3 "middle" vector (smallest total difference to the others). Upscale: each vector becomes
  four, each chosen from four candidates by a 4×4-pixel SAD. Output: one R16G16_SINT vector per 8×8 block. Scene
  change: 9-region histograms; on a change the search returns zero motion. Maximum motion 512 px. Requires SM 6.2
  and wave operations, and relies on `msad4`.
- **Frame interpolation:** scatters game vectors and optical-flow vectors into separate fields with packed
  priorities (F2), builds an SPD inpainting pyramid of the vector field (closest vector in 2D, farthest in depth),
  computes two disocclusion masks (interpolated frame vs previous, vs current), blends the two colours with those
  masks, blends in the optical-flow result by colour similarity, and fills remaining holes from a colour pyramid that
  ignores holes. A HUD-less back buffer can be supplied; then the inpainting pass "cleans up the UI". Requires
  CS 6.2 (6.6 on some 64-wide hardware).
- **Capture applicability [I]:** FSR 3 FI depends on game vectors and depth for its primary field; the optical-flow
  field is the secondary, colour-similarity-weighted source. On a captured window only the secondary path exists.
  The transferable parts are the **ideas**: packed-priority scatter, the hole-ignoring inpainting pyramid, two
  directional disocclusion masks, and "favour the source that matches better". The optical-flow search itself is
  close to what MoonSR already does (MoonSR adds zero-mean costs, consensus regularisation, sub-pixel refinement and
  forward-backward confidence; FidelityFX OF has none of the latter two at its output).
- **Licence [V]:** https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/docs/license.md. The SDK has
  **two** licences. Listed source files, including `ffx_frameinterpolation_*.hlsl` and `ffx_opticalflow_*.hlsl`, are
  under MIT-style terms. All other files (the signed DLLs, for example) fall under a default licence that allows
  redistribution "in binary form only" and forbids reverse engineering. The current SDK is 2.4.0 "Redstone"; Frame
  Generation is listed as 3.1.6 (and ML 4.0.x) [V, repository README]. FSR 4 / ML terms are not stated on that page [U].
- **Recommendation:** read the MIT-licensed shaders for ideas if useful; implement clean-room in SM 5.0. Copying
  would be legal with attribution (`THIRD_PARTY_NOTICES.md`) but would not fit: the shaders need SM 6.2 wave
  operations, which the D3D11 baseline lacks.

**`msad4` on the D3D11 baseline** [V]
- `msad4` is an SM 5 intrinsic but needs a WDDM 1.2 driver and is an **optional** capability
  (`D3D11_FEATURE_D3D11_OPTIONS::SAD4ShaderInstructions`); results above 65535 are undefined
  (https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-msad4).
- [I] It could make a SAD-based Performance search cheaper, but only as a runtime-detected variant with the current
  float path as fallback, and it changes the cost function (SAD on 8-bit luma instead of zero-mean RMS on FP16).
  A brightness-robust variant would need the mean removed before packing.

**DLSS Frame Generation, Smooth Motion, NvOFFRUC** [M, `nvidia.md` §3, §7, §8]
- DLSS FG combines engine vectors, depth and (on Ada) hardware optical flow in a network; DLSS 4 replaced the OFA with
  an AI flow model. Smooth Motion is NVIDIA's driver-level capture-style interpolation with no public API. NvOFFRUC
  is DesignWorks-licensed and NVIDIA-only.
- **Applicability:** none of them can be used on the universal path (vendor-specific, proprietary). They set the
  quality reference only. No public description of their occlusion or HUD logic exists [U].

**Lossless Scaling LSFG 3.x** [V/M]
- The Steam page calls LSFG "a proprietary machine learning model" and mentions "a separate performance model for
  low-power GPUs" [V, https://store.steampowered.com/app/993090/Lossless_Scaling/]. Press reports for LSFG 3 cite
  about 24 % lower latency and about 40 % lower GPU load than LSFG 2, and up to x20 [V as press claims,
  https://www.tomshardware.com/video-games/pc-gaming/lossless-scaling-3-update-touts-greatly-improved-latency-and-performance-universal-frame-gen-tool-boasts-24-percent-reduced-latency].
- [M, `ls_reference_motion_depth.md`] Reflection of the shipped DLL names inputs `InputFrame1/2`, `InputFlow`,
  `InputMask`, `InputUiMask`, a `Timestamp` constant and `ResolutionInvScale`: a heavy once-per-pair flow and mask
  stage with a light per-phase warp, which is MoonSR's structure too.
- Its network, training and internal resolution are [U]. It cannot be studied further under its licence, and it
  cannot be run inside fgbench, so **no quality comparison on identical inputs exists** (section 3.1).

### 2.2 Literature that transfers analytically

| Idea | Source | What transfers to MoonSR, at what cost |
|---|---|---|
| **Visibility-weighted, time-weighted fusion** of two backward warps | Super SloMo, Jiang et al., CVPR 2018, https://arxiv.org/abs/1712.00080 [V abstract]; the formula I_t ∝ (1−t)V₀·g(I₀) + tV₁·g(I₁) is [M] | MoonSR already does this (`beta` in `fg_pixsel_cs`). Keep. |
| **Softmax splatting**: forward-warp with a per-pixel importance metric to resolve many-to-one conflicts | Niklaus & Liu, CVPR 2020, https://arxiv.org/abs/2003.05534 [V abstract]; the brightness-constancy importance metric is [M] | Replace "confidence wins" with an importance that encodes occlusion order (2.3). SM 5.0 has no float atomics, so the D3D11 form is a max-key pass (as now) or fixed-point `InterlockedAdd` accumulation [I]. |
| **Many-to-many splatting (M2M)**: estimate bidirectional flow once, then splat to any number of time steps at minimal extra cost | Hu, Niklaus, Sclaroff, Saenko, CVPR 2022, https://arxiv.org/abs/2204.03513 [V abstract]; code licence not stated on the page [U] | The design principle for 3x-6x: decide as much as possible **per pair in source space**, and keep per-phase work to a splat and a fetch. This also removes per-phase re-decisions (F8). |
| **Splatting-only synthesis**, "much faster … especially for multi-frame interpolation" | Niklaus, Hu, Chen, WACV 2023, https://arxiv.org/abs/2201.10075 [V abstract] | Confirms splatting as the high-multiplier path; the paper's details were not read [U]. |
| **Direct intermediate-flow estimation** (IFNet), arbitrary timestep, 4-27× faster than Super SloMo / DAIN | RIFE, Huang et al., ECCV 2022, https://arxiv.org/abs/2011.06294 [V abstract]; Practical-RIFE code and weights MIT [M, `capture_tools_and_algorithms.md`] | As a network: too heavy for the low tier and the weights inherit Vimeo-90K data rights [M]. Analytically: estimate the **time-t field per pair** with a learned-free coarse-to-fine refinement, instead of re-selecting it per phase. |
| **Scale-agnostic shared-weight feature pyramid** for large motion | FILM, Reda et al., ECCV 2022, Apache-2.0, archived [M, `capture_tools_and_algorithms.md`] | The analytic analogue is what MoonSR has: the same matcher at every pyramid level. No new idea at this cost. |
| **Dense Inverse Search**: inverse-compositional LK on a patch grid, patch aggregation, optional variational refinement; 300-600 Hz on one CPU core | Kroeger et al., https://arxiv.org/abs/1603.03590 [V abstract] | Per-pixel sub-pixel refinement after the block search, cheaper and more accurate than the V-model fit for sub-pixel motion (F1, F3). ALSR v2 proposes the same (`alsr_v2.md`); one shared implementation should serve both. |
| **Quadratic motion** from four frames | QVI, Xu et al., NeurIPS 2019 [M] | Acceleration from the previous pair's field (already kept). Helps rotation and low base fps. Per-cell cost only. |
| **Edge-aware (joint bilateral) upsampling** of a low-resolution flow guided by full-resolution colour | [M, `capture_tools_and_algorithms.md` §2.1] | Snap motion boundaries to image edges at full resolution (F3, F4). |
| **Hole-ignoring inpainting pyramid** | FSR 3 FI [V, above] | Replace the blend fallback for pixels visible in neither frame (F5). One SPD-style downsample chain at half resolution [E ~0.05-0.1 ms at 1080p]. |

### 2.3 A capture-only occlusion order (replacing depth)

[I] Interpolation needs only the **order** of two surfaces at a motion boundary, not depth. Three image-only cues:
1. **Visibility in both frames.** In a forward splat collision, the surface that is in front is visible in both
   frames along its path; the one behind is covered in one of them. `fg_pixsel_cs` already computes per-side
   visibility, but only after the splat has picked a winner. Moving a cheap version of that test into the splat
   key (key = visibility-both ‖ confidence) makes the collision rule an occlusion rule.
2. **Which motion the boundary moves with.** An occluding edge moves with the foreground. At a boundary between
   cells with motions a and b, the boundary's own displacement (measured from the cells' landing points) matches the
   front surface.
3. **Parallax relative to the dominant motion.** Under a translating camera, faster-moving content is nearer. This
   fails for independently moving objects, so it should be a tie-breaker only.

None of these is measured. Cue 1 costs a few texture loads per cell in the splat pass [E < 0.02 ms at 1080p].

### 2.4 HUD and UI detection

- [M, `capture_tools_and_algorithms.md` §2.4] The image-only recipe is a static-pixel test while the surroundings
  move, plus edge and coherence cues. MoonSR implements that per pair (F6).
- [V] Lossless Scaling's 3.2 notes claim "refined UI detection" (`capture_tools_and_algorithms.md`, Steam/press
  sources there); its method is [U]. FSR 3 and DLSS FG rely on a HUD-less buffer or UI buffers from the engine
  [V FSR page; M `nvidia.md` §3], which a capture does not have.
- Transferable, clean-room: (a) **temporal persistence** (ALSR's evidence counter, MoonSR's own code); (b) the
  **HUD-less frame** technique from the owner's Moon FG addon (fill overlay pixels from around, interpolate,
  composite the overlay back), measured there on text [M, `ls_reference_motion_depth.md` item 7, owner's own code];
  (c) a **soft alpha estimate** for translucent panels: a pixel whose change equals (1 − α) times the background's
  change is a translucent overlay of opacity α [I]. (c) is research, not a plan item.

### 2.5 Licence summary for everything named here

| Item | Licence | Use |
|---|---|---|
| FidelityFX SDK listed shader sources (FI, OF, FSR 3 upscaler) | MIT-style [V] | Ideas; copying allowed with attribution but impractical (SM 6.2) |
| FidelityFX SDK other files, signed DLLs | Binary-only, no reverse engineering [V] | Do not use |
| Practical-RIFE code and weights | MIT [M]; training data rights unclear [M] | Not proposed |
| FILM | Apache-2.0, archived [M] | Not proposed |
| M2M | Paper CC BY 4.0 [V]; code licence [U] | Idea only |
| Softmax splatting original code | research licence [M, verify] | Idea only |
| DIS (OpenCV) | Apache-2.0 [M] | Clean-room from the paper |
| Owner's DLSS-FG / Moon FG addon techniques | Owner's own code; the manager it derives from is MIT [M, `ls_reference_motion_depth.md`] | Re-implementation with origin noted (already done once, `fg_search_cs.hlsl` header) |
| LSFG, DLSS FG, Smooth Motion | Proprietary | Reference only |
| NvOFFRUC / NVIDIA OF SDK | DesignWorks [M, `nvidia.md`] | Optional, never mandatory |

**Recommendation:** every plan item below is a clean-room implementation from public descriptions or the project's
own code. None needs borrowed code.

---

## Part 3: Ranked plan

### 3.0 The cost budget

The owner's constraint is "no net increase". Two working points exist and the lead must name the reference
(decision D1):
- **Shipped default** (Performance, 50 % flow): 0.839 + 0.266 ms per 2x pair at 1080p [R].
- **Owner's live setting** (Performance, 100 % flow, precise at ≥ 3x): 2.249-2.320 + 0.26 ms [R].

Savings available to pay for quality, in order of size:
1. **Flow scale.** 100 % → 50 % saves 1.41 ms per real frame at 1080p [R] with no loss on 22 real triplets [R]. The
   only measured loss is HUD-region PSNR (F6). If M3 removes that dependency, 50 % becomes the recommended setting and
   the saving funds everything else. This is the decisive lever, but it changes the owner's setting (D2).
2. **Per-phase work moved to per-pair** (M5). At 6x this removes up to five copies of the selection pass. The
   hand-over measured pixsel at about 0.2 ms per generated frame at 1080p [H 2026-11-16]. [E] 0.5-0.8 ms per real
   frame at 4x-6x.
3. **HUD pass on flagged tiles only.** About 0.19 ms per pair at 1080p regardless of flow scale [H]. Running the full
   pass only on 16×16 tiles that contain unchanged pixels next to motion [E] saves 0.05-0.15 ms in typical scenes.
   Two earlier HUD micro-optimisations did not pay [H], so this must be measured, not assumed.
4. **`msad4` Performance search variant**, runtime-detected (2.1). [E] unknown until measured; it changes the cost
   function, so it needs the full quality suite.

### 3.1 Missing measurements (M0 must add these first)

| Gap | Why it blocks | Proposed tool change (owner: benchmark-specialist, in `tools/fgbench`) |
|---|---|---|
| **Oracle flow** | F1 cannot be split into motion error vs synthesis error | A test hook that feeds the true per-cell (and per-pixel) motion of a synthetic sequence into the interpolation stage. Needs a test-only entry in the generator (contract change, lead) |
| **Flow end-point error** | Motion changes (M2, M4) cannot be judged by PSNR alone | Mean and p95 EPE per sequence against the known synthetic motion, both directions, plus EPE on edge and object masks |
| **Per-phase consistency over a whole interval** | F8 | For M = 2..6, generate all phases and report (a) the spread of PSNR across phases and (b) the RMS error of each phase-to-phase difference against ground truth (extend `imgtool temporal` to sequences) |
| **Object / region PSNR** | small_fast_object, thin features are hidden by whole-frame PSNR | Masks for the moving object, thin structures and occlusion strips |
| **Real-triplet suite as a command** | Today it is a shell loop | `--triplets <dir>` over a recorded folder, with repeat-nearest **and linear-blend** baselines, mean / worst / per-triplet CSV |
| **Fallback coverage** | F5 | Fraction of pixels with fallback weight > 0.5 and > 0.9, as a debug statistic (shader-side counter, no per-frame readback in the app) |
| **New synthetic sequences** | Not covered at all | thin_moving (1-2 px wires and pillars over a moving background), translucent_hud (30-70 % panels and anti-aliased glyphs over a pan), animated_hud (a changing minimap), duplicate_frames (A, A, B pattern), fast_flick_30fps (motion > 200 px), curved_motion (acceleration / rotation over 4 frames), flow-scale sweep including 70/75/80 % |
| **More real recordings** | 22 triplets from one ATLYSS scene | Several games and scenes (fast turn, foliage, particles, UI-heavy), recorded with `frame_recorder` at a high game frame rate so N, N+2 triplets give a 30 → 60 test and N, N+4 a 15 → 60 test |
| **Comparison with Lossless Scaling** | The quality bar | Not possible on identical inputs offline. A controlled live A/B (same scene, recorded output of both, blinded) is the only route [I]. Record as a known limitation |

### 3.2 Milestones

All thresholds are **proposals** for the lead. "Real" = the real-triplet suite; "synthetic" = `fgbench --quality` at
1280×720; costs from `fgbench --time` at 1920×1080 on the RTX 5060, with a correctness run on the RTX A400 and the
Radeon iGPU (correctness only, per the project's note on the 610M). Each milestone reports at **both** 50 % and 100 %
flow.

**M0: baseline and harness (no algorithm change).**
- Implement the gaps in 3.1. Record a reproducible baseline in `BENCHMARKS.md` with commit, driver and settings
  (today's [R] numbers are the starting point).
- *Exit:* baseline tables for synthetic (per phase), real triplets (with both baselines), EPE, per-phase consistency
  and fallback coverage; the 1.1 results reproduced within run-to-run noise.

**M1: correctness fixes (cheap, low risk).**
- Reset the temporal candidate on a scene cut (F7): the cut decision is already on the GPU, so the next pair's
  search can test the stats buffer slot and ignore the temporal candidate. No readback.
- Exact area-weighted luma pre-filter for any flow ratio (F10).
- Duplicate-frame detection: count unchanged pixels in the luma pass and mark the pair. What the scheduler does with
  it (drop the duplicate from the real-frame timeline) is an engine decision (D4).
- *Exit:* periodic_fence at 75 % within 1 dB of 70 % and 100 %; a duplicate_frames sequence produces even motion;
  no other sequence moves by more than ±0.05 dB; cost delta ≤ +0.02 ms.

**M2: sharp, phase-stable synthesis (F1).** Order of work decided by the M0 oracle-flow result.
- If synthesis dominates: a cheaper sharp fetch than the 9-tap Catmull-Rom (for example a 4-5 tap separable cubic
  approximation), used in Performance; and, where the two warped samples agree, weight towards the sample with the
  smaller resampling offset instead of a fixed t-blend [I, experimental].
- If motion dominates: per-pixel inverse-compositional sub-pixel refinement (DIS-style, shared with ALSR v2 M4).
- *Exit:* pan_subpx at t = 1/4 within 1 dB of repeat-nearest (today −10 dB); PSNR spread across phases on pan_5px
  and pan_20px halved (today 6.8 and 4.0 dB); real mean ≥ baseline + 0.2 dB; Interpolate cost ≤ baseline.

**M3: HUD independent of flow resolution, with memory (F6).**
- Persistent per-pixel overlay evidence (ALSR's counter, adapted), full-resolution boundary test that does not depend
  on cell size, use of the current pair's mask in the search where the order allows.
- Then the HUD-less-frame technique for translucent panels and anti-aliased glyphs.
- *Exit:* hud_pan HUD PSNR at 50 % flow within 3 dB of 100 % (today 45.7 vs 72.3); translucent_hud ghost error
  reduced (target set after M0 baseline); HUD pass cost ≤ baseline (pay with the tile-flagging saving, 3.0 item 3).
- Why here: M3 is the precondition for recommending 50 % flow, which funds M4-M6.

**M4: occlusion-ordered splatting and a non-ghosting fallback (F2, F5).**
- Splat key with an occlusion-order term (2.3 cue 1, then cue 2).
- Replace the linear-blend fallback: one-sided warp where one side is visible, hole-ignoring inpainting pyramid where
  neither is, blend only where nothing else is defensible.
- *Exit:* layers_occlusion at t = 1/2 ≥ 34 dB (today 32.3) and edge ≥ 28 dB (today 26.2); small_fast_object object-
  region PSNR improved (target after M0); fallback coverage on real triplets reduced; real mean ≥ M3 + 0.3 dB;
  cost ≤ +0.1 ms, paid by 3.0 savings.

**M5: per-pair decisions for multi-frame consistency and cost (F8).**
- M2M principle: classify each source cell once per pair (motion, confidence, occlusion role), splat per phase,
  and make the per-phase selection a cheap fetch rather than a full re-score. Optional quadratic correction from the
  previous pair's field, gated by its confidence.
- *Exit:* per-phase consistency error at 4x and 6x reduced (target after M0); no 2x regression; Interpolate cost per
  generated frame reduced (target −30 % [E]); curved_motion improved if the quadratic term is enabled, otherwise
  drop it.

**M6: motion detail: thin features, small objects, large motion (F3, F4, F9).**
- Edge-aware upsampling of the time-t field guided by full-resolution luma; selection patch error on full-resolution
  luma at boundary texels only; whole-picture shift candidate at the coarsest level; the thin-line ±1 px two-frame
  warp blend from Moon FG [M, item 8].
- *Exit:* thin_moving and small_fast_object region PSNR improved; EPE on edges improved; zoom_rotate and real_rotate
  edge PSNR ≥ +1 dB; fast_flick_30fps tracked; total cost within budget.

**M7: cost and tiers.**
- `msad4` variant (optional capability), level-0 search skip at 100 % flow (`finestLevel = 1` exists in `TierParams`
  but is unmeasured), and a decision on whether Balanced and Quality tiers are still needed.
- *Exit:* PushFrame + Interpolate at the reference working point ≤ the M0 baseline with all of M1-M6 enabled; A400 and
  iGPU numbers recorded.

### 3.3 Risks

1. **Synthetic-to-real gap [R/I].** +13 dB synthetic versus +3.5 dB real today. No milestone should pass on synthetic
   gains alone; the real-triplet suite is part of every exit.
2. **One game [R].** All real data is one ATLYSS scene. M0 must add recordings before M2 is judged.
3. **PSNR is not perceived quality [I].** Ghosting versus judder trade-offs (M4's fallback) can lower PSNR while
   looking better, or the reverse. The owner's visual checks remain necessary; record them as subjective, as
   `BENCHMARKS.md` already does.
4. **ALSR shares the motion field [C].** M1, M2 (sub-pixel), M6 change `MotionFieldView` contents. Run the ALSR
   temporal suite with every estimator change.
5. **SM 5.0 limits [V/I].** No wave intrinsics, no float atomics, `msad4` optional. Every plan item above is written
   to fit; anything needing SM 6 is out of scope.
6. **Flow-scale recommendation [R/I].** The 50 % recommendation rests on 22 triplets from one scene plus the HUD
   result. It needs the M0 recordings before the default or the owner's habit changes.

---

## Summary of findings

- **The architecture is right; the decisions inside it are the limit.** Once-per-pair analysis, cheap per-phase
  synthesis from the original pair, GPU-only. On 22 real ATLYSS held-out triplets it scores 26.26 dB / SSIM 0.859,
  3.5 dB over repeat-nearest [R]. On synthetic sequences the gain is 10-17 dB, so most synthetic headroom does not
  reach real content.
- **100 % flow is not better than 50 % on real content** (26.21 vs 26.26 dB, lower SSIM on all 22 triplets) and costs
  1.41 ms more per real frame at 1080p [R]. Only HUD-region accuracy depends on it (45.7 vs 72.3 dB) [R]. Fixing that
  dependency makes flow scale the budget that pays for every other improvement.
- **The largest visible defects, by evidence:**
  1. phase-dependent blur and error (up to 6.8 dB across phases of one pan; −10 dB versus repeat at sub-pixel t = 1/4);
     the shipped tier fetches bilinearly;
  2. splat collisions won by confidence rather than occlusion order;
  3. a linear-blend fallback that is itself the ghosting artefact;
  4. HUD protection that depends on cell size and has no memory;
  5. cell-level translation motion selected on flow-resolution luma (thin features, rotation);
  6. scene-cut leakage into the next pair and no duplicate-frame detection;
  7. a likely luma pre-filter cause behind the 75 % fence failure.
- **Transferable without engine data**, all clean-room: packed-priority splatting with an image-only occlusion order
  (FSR 3 FI idea, softmax splatting), a hole-ignoring inpainting pyramid (FSR 3 FI), M2M-style per-pair decisions for
  3x-6x, DIS-style per-pixel refinement (shared with ALSR v2), edge-aware flow upsampling, HUD persistence and a
  HUD-less-frame pass. The FidelityFX FI and OF shader sources are MIT-style [V], but the SDK's binaries are
  binary-only, and the shaders need SM 6.2.
- **Measurement gaps block a judgement on half the plan:** oracle flow, EPE, whole-interval phase consistency,
  region metrics, fallback coverage, a real-triplet command with a blend baseline, and sequences for thin features,
  translucent and animated HUD, duplicates, fast flicks and curved motion.

## Files written

- `documentation/research/fg_quality.md` (this file).
- Measurement outputs (not part of the repository) went to the session scratchpad under `fgq\results\`.

## Decisions needed from the lead

- **D1.** Which working point the "no net increase" budget refers to: the shipped default (50 % flow) or the owner's
  setting (100 % flow, precise motion).
- **D2.** Whether to recommend 50 % flow to the owner once M3 removes the HUD dependency, and whether to record
  today's [R] numbers in `BENCHMARKS.md`.
- **D3.** A test-only oracle-flow entry point in `frame_generator.h` (a contract change) for M0.
- **D4.** What the scheduler does with a duplicate-frame flag (M1); that is engine-owned.
- **D5.** Whether Balanced and Quality tiers stay, given the app only uses Performance.

## Open questions

1. Is F1 (phase-dependent error) mostly bilinear resampling or mostly motion error? Oracle flow answers it.
2. Does the 75 % fence failure come from the luma pre-filter (F10)? A filter-only experiment answers it.
3. How large is the fallback share on real content, and how much of the real-versus-synthetic gap it explains.
4. Do the real-triplet results hold for other games, faster motion and lower base frame rates?
5. How does MoonSR compare with LSFG on identical scenes? Only a controlled, recorded live A/B can tell.

## Claims I could not verify

- The internal method of LSFG 3.x (network, resolution, UI detection) [U]; only "proprietary machine learning model"
  and a separate low-power model are [V].
- FidelityFX FI details beyond the documentation page (how the HUD is composited back in HUD-less mode; exact
  thresholds) [U]. FSR 4 / ML licence terms [U].
- Softmax splatting's importance metric, Super SloMo's exact fusion formula, RIFE's coarse-to-fine and fusion-mask
  details, and the Splatting-based Synthesis architecture: only the abstracts were read [V abstract / M detail].
- M2M and softmax-splatting code licences [U / M].
- The cost estimates marked [E] (inpainting pyramid, tile-flagged HUD pass, per-pair selection savings, `msad4`).
- Whether the 0.2 ms pixsel figure (hand-over, 2026-11-16) still holds after later changes [H, not re-profiled].
- `nvidia-smi` reported driver 621.14 for the RTX 5060; I did not cross-check it with the driver panel.
