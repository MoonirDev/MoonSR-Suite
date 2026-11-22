# Moon ALSR - progress log (upscaling-engineer)

Purpose: hand-over notes so that a successor can continue if a run is cut off. Newest entry LAST. Every number
below comes from a command that was run on the stated tree; synthetic results say nothing about real games.

Note (2026-11-21): `tools/tbench`, its `alsr_dev.h` hooks and the `MOONSR_ALSR_TUNE profile=1` profiler were removed
before the repository was published. The measurements below are a historical record and cannot be reproduced from
this repository.

Hardware used: NVIDIA GeForce RTX 5060 (driver 32.0.16.2114), NVIDIA RTX A400, AMD Radeon iGPU (Ryzen 9 7900X).
Build: `powershell -ExecutionPolicy Bypass -File tools\build.ps1 -BuildDir build\alsr -Target moonsr_tbench`
Binary: `build\alsr\bin\moonsr_tbench.exe`.

---

## 2026-11-15 - Stage 1: rename and port (DONE)

What was done
- `src/upscale/alsr_upscaler.cpp` (new): the MoonSR Temporal implementation moved over unchanged as
  `AlsrUpscalerImpl : AlsrUpscaler`. Learning methods are STUBS at this stage: `SetProfile` records
  `<dir>\<gameKey>.alsr` (no load), `SaveProfile` returns true, `ResetLearning` does nothing, `LearningStatus`
  returns zeros + the path.
- `temporal_upscaler.cpp/.h` deleted (nothing referenced them except tbench).
- Shaders renamed `shaders/upscale/tu_*_ps.hlsl` -> `alsr_*_ps.hlsl`, `temporal_common.hlsli` -> `alsr_common.hlsli`,
  cbuffer `TemporalConstants` -> `AlsrConstants`; variables `g_ps_tu_*` -> `g_ps_alsr_*` in the upscaling-engineer
  section of `shaders/CMakeLists.txt`. Tuning env var is now `MOONSR_ALSR_TUNE` (tbench `--tune` sets it).
- `tools/tbench`: uses AlsrUpscaler / AlsrSettings / AlsrDebugView; the method label is now "ALSR s<sharpness>".

Measured (RTX 5060, tree = efc2289 + these changes)
- `moonsr_tbench.exe --adapter 5060 --selftest` -> 38 checks, 0 failures.
- `moonsr_tbench.exe --adapter 5060 --quality --scales 1.5 --inputs aliased --baselines none --tsharp 0,0.3
  --seq pan_slow,hud_pan --no-curves --tag s1_after`, identical to before the port (tag s1_before) and to
  `temporal_quality_r2_a15.csv`:
  pan_slow s0 29.152 dB / SSIM 0.95216; s0.3 28.388. hud_pan s0 28.500 (ghost 18.114); s0.3 27.932 (ghost 19.050).
- `-Target moonsr_cli` and `-Target moonsr_fgbench` build; `moonsr_fgbench --selftest` OK (0 failures).

Next: Stage 2a (HUD overlay layer), then 2b (per-pass GPU profiling and cost).

---

## 2026-11-15 - Stage 2a: HUD / overlay layer (DONE, one residual loss documented)

State found at the start (commit 380b3fe, uncommitted-at-the-time Stage 2 work of the previous engineer): per-pass
profiler (`MOONSR_ALSR_TUNE profile=1`, tbench `--time --passes`) complete; overlay layer partly done (persistent
overlay mask in alsr_prep, layer/ring texture from alsr_motion, ring = no history + LayerCubic, no residual in the
ring, "touch" test on the history fetch). Measured at that state (RTX 5060, s0, `--baselines all`): pan_slow aliased
1.5x 28.931 dB (Stage 1: 29.152 - REGRESSION), hud_pan aliased 1.5x 28.685 / ghost 5.668, prefiltered 1.5x ghost
13.301 (best single-frame 11.346), aliased 2x ghost 11.925, prefiltered 2x ghost 13.368 (best 13.269).

Root cause of the pan_slow regression (and of a much older loss): the generator's per-pair static mask has false
positives inside slowly moving ALIASED texture (a point-sampled pixel stays unchanged for a frame while the scene
moves sub-pixel). Those pixels were forced to zero motion / confidence 1 and became overlay rings.

Changes
- alsr_prep MRT1 is now an overlay EVIDENCE counter (R8): +32/255 per frame with mask >= 0.5 and pixel unchanged
  in place (< 1.5 levels), -8/255 per unchanged frame without mask, 0 on any change. `OvlStrength()`
  (alsr_common.hlsli) = smoothstep(96/255, 160/255): overlay after 3..5 consistent frames. Used everywhere the mask
  was used (motion: zero motion + layer; reproject: hThere / hHere).
- alsr_motion Layer(): r = 1 overlay, 0.5 = background with an overlay pixel among its 8 neighbours, 0 clean bg.
- alsr_reproject LayerCubic: the background side weights "next to overlay" pixels by an alpha estimate (projection of
  the colour onto the segment mean-overlay -> mean-clean-background colour of the 4x4 window, smoothstep(0.55,0.85)):
  hard-edged input keeps them, anti-aliased (mixed) pixels are dropped. Flags 32768 = keep always, 65536 = drop always.
- tbench --save-images: `errdiff_alsr_vs_spatial.png` (red = ALSR worse than MoonSR Spatial, green = better, blue =
  ghost region), `mosaic_gt_alsr_errdiff.png`, and a printed per-frame ghost-region MAE.

Measured (RTX 5060, s0; command: `moonsr_tbench --adapter 5060 --quality --scales 1.5,2 --inputs aliased,prefiltered
--baselines none --tsharp 0 --seq pan_slow,hud_pan --no-curves`):
| case | PSNR before | PSNR after | ghost before | ghost after | best single-frame ghost |
|---|---|---|---|---|---|
| pan_slow aliased 1.5x | 28.931 | 29.669 | - | - | - |
| pan_slow prefiltered 1.5x | 34.174 | 34.263 | - | - | - |
| pan_slow aliased 2x | 27.628 | 29.798 | - | - | - |
| pan_slow prefiltered 2x | 32.124 | 32.193 | - | - | - |
| hud_pan aliased 1.5x | 28.685 | 28.717 | 5.668 | 5.971 | 6.516 (Bicubic) |
| hud_pan prefiltered 1.5x | 31.792 | 31.898 | 13.301 | 12.811 | 11.346 (Spatial) |
| hud_pan aliased 2x | 27.922 | 28.064 | 11.925 | 11.579 | 13.605 (Spatial) |
| hud_pan prefiltered 2x | 30.106 | 30.056 | 13.368 | 13.686 | 13.269 (Spatial) |
(Stage 1 hud_pan aliased 1.5x ghost was 18.114.) Variants tried: ring = generic spatial (flag 4096): a1.5 7.47,
p1.5 12.47, a2 13.96, p2 14.81; drop all "next to overlay" pixels: 7.62 / 12.39 / 13.93 / 15.28; keep all
(old): 6.35 / 14.04 / 12.66 / 14.67. The alpha-estimate version is the best compromise and the only one that beats the
best single-frame ghost on both aliased cases.
Self-test 38/38 pass (aliased slow pan gain over Spatial now +2.767 dB, was +0.646).

Looked at (4x): `benchmarks/output/temporal/s2a_final/.../hud_pan/glyphs_a15.png` (GT | ALSR | Spatial, aliased
1.5x): no dark outlines around glyphs. Prefiltered 1.5x `f31_mosaic_gt_alsr_errdiff.png`: no outlines, ALSR better
than Spatial along glyph strokes; it loses in background revealed BELOW the text block (mortar lines) and on the 1-px
glyph row.
Remaining loss (not fixed): prefiltered input, ghost +1.46 (1.5x) / +0.42 (2x) levels over MoonSR Spatial. Cause: thin
(1 output px) glyphs are partially covered in EVERY input pixel with anti-aliased input, so they change with the
background, never accumulate overlay evidence and are treated as moving background. No per-pair test can separate
them; a long-term statistic (temporal median / variance) would be needed.

Next: Stage 2b (pass timing breakdown, then cost cuts).

---

## 2026-11-15 - Stage 2b: cost (DONE for the RTX 5060 target)

Command: `moonsr_tbench --adapter 5060 [--adapter A400] --time --passes --tag <tag>` (pan_medium aliased frames,
s0.3, 200 iterations, 30 warm-up; per-pass = mean over 64 profiled frames). Isolated GPU time, idle GPU.

Breakdown found (tag s2b_before, RTX 5060, 1280x720 -> 1920x1080): Render 4.463 ms; cellrefine 3.313, reproject
0.305, spatial 0.231, motion 0.160, update 0.143, refluma 0.111, final 0.109, prep 0.048, residual 0.036.

Changes
- Cell refinement moved to a compute shader `shaders/upscale/alsr_cellrefine_cs.hlsl` (the PS version is deleted):
  one 16x16 group per cell, one thread per Lucas-Kanade window sample, group reductions; the v-independent terms
  (weights, gradients, 2x2 matrix) are computed and reduced once, each Gauss-Newton step fetches one reference
  sample per thread. The pixel shader ran one thread per cell (3600 threads) and left the GPU idle.
  Result identical to the PS version (all quality numbers equal to 3 decimals).
- Gauss-Newton steps 4 -> 2 (`lkIters`, tune key). Quality deltas 4 -> 2 steps (RTX 5060, s0, aliased / prefiltered):
  pan_slow 1.5x +0.032 / +0.002, 2x +0.071 / +0.001; hud_pan 1.5x +0.085 / -0.008 (ghost +0.008 / +0.030),
  2x +0.085 / -0.007; pan_medium 1.5x +0.111 / -0.008; layers 1.5x +0.090 / -0.018; zoom_rotate 1.5x +0.009 /
  -0.002; thin_features 1.5x -0.004 / +0.061 dB. All within +-0.11 dB, no systematic loss.
- Compute pass helper `Dispatch()` (binds b0, s0/s1, SRVs, u0; unbinds everything).

After (tag s2b_after):
| GPU | size | Render before | Render after | RenderSingle | PushFrame |
|---|---|---|---|---|---|
| RTX 5060 | 720p->1080p | 4.463 | 1.626 | 0.343 | 0.787 |
| RTX 5060 | 1080p->1440p | - | 3.397 | 0.618 | 1.628 |
| RTX 5060 | 1080p->2160p | - | 5.055 | 1.367 | 1.620 |
| RTX A400 | 720p->1080p | - | 11.092 | 2.428 | 4.034 |
| RTX A400 | 1080p->1440p | - | 23.018 | 4.344 | 8.924 |
RTX 5060 720p->1080p passes after: cellrefine 0.467, reproject 0.307, spatial 0.233, motion 0.159, update 0.142,
refluma 0.112, final 0.111, prep 0.048, residual 0.036.
A400 720p->1080p passes: cellrefine 3.106, reproject 2.057, spatial 1.694, motion 1.352, update 0.828, refluma
0.774, final 0.739, prep 0.285, residual 0.272 - the A400 is ~7x slower than the 5060 on every pass; 11 ms is too
much for it at 60 fps+ with frame generation; not addressed further here.
Target "<= 2.0 ms on the RTX 5060 at 720p->1080p": met (1.626 ms), no measurable quality loss.
Self-test 38/38.
Ideas not done: cellrefine at 0.47 is still the largest pass (reduction barrier count; wave intrinsics are SM6 so not
available); reproject + residual + update could share loads; motion candidate scoring already only at boundaries.

Next: Stage 3 (learning).

---

## 2026-11-15 - Stage 3a: CPU learner (first version, builds, offline results)

State found: `src/upscale/alsr_learn.h` was a header only (no .cpp, not in the build). Its design idea (RAISR-style
filters on the low-res pixels, phase x orientation x strength x coherence buckets, worker thread) was kept; the header
was rewritten and implemented.

Done (new: `src/upscale/alsr_learn.cpp`, `tools/tbench/learn.cpp`; changed: `alsr_learn.h`, `src/upscale/CMakeLists.txt`,
`tools/tbench/CMakeLists.txt`, `tbench.h`, `main.cpp` option `--learn-offline [real|synthetic]`)
- Filter: out = w0 * G + sum w_k * S_k, G = generic (MoonSR Spatial) output, S = 5x5 source pixels around the source
  pixel nearest to the sample (26 features, per RGB channel). Prior = (1, 0, ...) = generic path exactly; ridge towards it.
- Buckets: phase (exact period: 1.5x -> 3x3, 2x -> 2x2; otherwise 4 quantised bins) x 8 angles x 3 strengths
  (sqrt l1 < 0.015 / < 0.06) x 3 coherences (< 0.3 / < 0.6). 648 buckets at 1.5x, 288 at 2x.
- Pairs: 240x240 tile of F = truth; F_low = point or area (linear light) reduction, 8-bit; G via `SpatialCpu` (C++ port
  of moonsr_spatial_ps.hlsl, not yet compared with the GPU); training / held-out split 7:1 by hash.
- Normal equations per bucket for training AND held-out samples (double) -> exact held-out SSE of any filter.
  Solve: Cholesky, mu = (32 + 1e-3 n) * trace / (26 n). Gate: n_val >= 24 and held-out SSE >= 1 % below generic;
  alpha = n_val/(n_val+64) * ramp(improvement 1..3 %). Published = alpha * w + (1 - alpha) * prior.
- Sampling model estimate: edge-step sharpness statistic of F vs of its point / area reductions (first 8 tiles).
- Profile serialisation (magic MSRALSR1, version 1, sections per scale, both equation sets, filters, FNV-1a checksum,
  temp file + MoveFileEx) - written, NOT yet tested.

Measured (`build\alsr\bin\moonsr_tbench.exe --learn-offline real`, CPU, 5 s):
- ATLYSS, train in_1466, validate in_1467 (nearly the same image - almost no camera motion, so this is close to a
  training-set number): model estimated = point. Self-supervised MAE generic 3.260 -> learned 3.045 (RMSE 10.61 ->
  9.94); forced point 3.260 -> 3.076 (one pass over the frame); forced area 3.206 -> 3.146. Coverage 0.83, 0 gating
  violations. Bucket occupancy: none empty; strength bins 44 / 39 / 17 %, coherence 30 / 47 / 23 %.
(`--learn-offline synthetic`, 107 s): train pan_medium frames (n*7), evaluate PSNR vs TRUE GT (CPU apply reference) on
pan_medium frame 900 (new texture) and thin_features frame 16 (unseen content). After 30 full frames:
| case | model | pan f900 generic -> learned | thin generic -> learned | self-sup MAE gen -> learned |
|---|---|---|---|---|
| 1.5x aliased | point (estimated) | 27.909 -> 28.309 | 17.388 -> 16.771 | 6.726 -> 5.755 |
| 1.5x aliased | area | 27.909 -> 27.139 | 17.388 -> 16.178 | 6.409 -> 5.838 |
| 1.5x prefiltered | point | 32.368 -> 30.096 | 23.098 -> 22.681 | 4.941 -> 3.840 |
| 1.5x prefiltered | area (estimated) | 32.368 -> 33.909 | 23.098 -> 23.591 | 4.890 -> 4.432 |
| 2x aliased | point (estimated) | 34.628 -> 25.260 | 16.941 -> 16.171 | 12.788 -> 9.247 |
| 2x prefiltered | area | 34.628(36.439) -> 35.595 | 21.269 -> 21.220 | 9.661 -> 9.626 |
| 2x prefiltered | point (estimated - WRONG) | 36.439 -> 25.760 | 21.269 -> 20.091 | 11.257 -> 7.727 |
FINDING: the self-supervised gate cannot see whether a gain transfers from (F/s -> F) to (F -> F*s). Prefiltered 1.5x
transfers (+1.5 dB same content, +0.5 dB unseen); aliased 1.5x helps on the trained content (+0.4) but hurts unseen
content (-0.6); 2x point-model learning is catastrophic (-9 to -11 dB) while self-supervised error drops 28 %.
The contract rule "never worse than generic" is therefore NOT met by self-supervised gating alone.

Next: 3a.2 - a cross-scale transfer gate (train one pyramid level lower, F/s^2 -> F/s, validate that filter on the
level-0 equations; a bucket is used only if the lower-level filter also beats generic one level up), then re-run.

---

## 2026-11-15 - Stage 3a.2 / 3a.3 / 3b: transfer gate, point-model bias fix, sampling-model choice, scale limit

1. Cross-scale transfer gate (in `Learner::Solve`): per bucket a third equation set from pairs one pyramid level lower
   (F reduced twice -> F reduced); its filter must beat generic on ALL level-0 samples; alpha *= ramp(improvement).
   Profile version 2 (3 equation sets per bucket). Effect measured: small (see table); kept as a safety measure.
2. ROOT CAUSE of the 2x catastrophe: the point model picked the NEAREST F pixel. At 2x every low centre lies between
   four F pixels -> the whole low grid shifted by half an F pixel; at 1.5x alternating -/+0.25 F px by phase. The
   learner learns that shift. Fix: stochastic rounding per column / row (hash of frame and index) - unbiased, keeps
   hard single-value steps (`PointIndices` in alsr_learn.cpp).
3. Remaining 2x loss, LOOKED AT (`benchmarks/output/alsr_learn/syn_2x_aliased_point_pan.png`, panels input | GT |
   generic | learned): learned output breaks 1-input-px mortar lines into dotted fragments and fades them. With point
   self-supervision a 1-F-px feature survives the 2x reduction only about half the time, so the learner learns
   "thin lines are unreliable". Not detectable by any self-supervised test (targets are point-sampled frames).
4. 1.5x images LOOKED AT (`syn_1.5x_aliased_point_pan.png`, `_thin.png`): learned mortar / 1-px lines are smoother and
   closer to GT than generic; no dotted artefacts.

Measured (`moonsr_tbench --learn-offline synthetic[:scale[:input]]`, CPU; 30 training frames of pan_medium; PSNR vs
TRUE GT on pan_medium f900 / thin_features f16; transfer gate ON unless noted):
| case | model | pan generic -> learned | thin generic -> learned |
|---|---|---|---|
| 1.5x aliased | point (estimated) | 27.909 -> 29.181 (gate off 29.422) | 17.388 -> 17.896 (off 17.883) |
| 1.5x aliased | area | 27.909 -> 27.157 | 17.388 -> 16.476 (off 16.178) |
| 1.5x prefiltered | area (estimated) | 32.368 -> 33.903 | 23.098 -> 23.590 |
| 1.5x prefiltered | point | 32.368 -> 30.336 | 23.098 -> 22.702 |
| 2x aliased | point (estimated) | 34.628 -> 27.890 | 16.941 -> 17.324 |
| 2x aliased | area | 34.628 -> 33.403 (off 32.723) | 16.941 -> 16.583 (off 16.269) |
| 2x prefiltered | area | 36.439 -> 36.422 | 21.269 -> 21.232 |
| 2x prefiltered | point (estimated - wrong) | 36.439 -> 28.731 | 21.269 -> 20.768 |
3b (sampling model): the edge-step statistic chose correctly at 1.5x for both inputs and chose point for ATLYSS; it
chose WRONG at 2x prefiltered. A wrong model at 1.5x costs -2.0 dB (point on prefiltered) / -0.75 dB (area on aliased).
The self-supervised MAE does NOT track true-GT quality (1.5x aliased point: self-sup MAE 8.33 -> 8.56 worse while true
GT +1.5 dB), so "which model validates better" is not usable as the selector; the statistic is.
DECISION: `kMaxLearnScale = 1.75` (alsr_learn.h): learning only for ratios in (1.01, 1.75] per axis.

Next: 3c GPU apply pass (`shaders/upscale/alsr_learned_ps.hlsl` written, not yet registered / built).

---

## 2026-11-15 - Stage 3c / 3d: GPU apply pass, online collection, profile API (DONE, self-tested)

Done
- `shaders/upscale/alsr_learned_ps.hlsl` (registered `g_ps_alsr_learned`): bank applied in one output-res pass;
  inputs source frame, generic (spatial_) output, filter table (Buffer<float4>, 7 float4 / bucket, DEFAULT buffer,
  UpdateSubresource only when the worker published a new table). Seed for reproject = learned output when
  useLearned && scale in (1.01, 1.75] && any bucket active; otherwise the generic texture is used unchanged.
  RenderSingle: spatial -> learned -> final (same rule; table only used for the scale of the last Render).
- Collection (`Collect`): 3-slot staging ring of 240x240 tiles at a hashed, time-varying position, event query per
  slot; GetData(DONOTFLUSH) + Map(DO_NOT_WAIT) -> memcpy into the learner's preallocated job -> Submit. No copy while
  the worker is busy. Worker: THREAD_PRIORITY_LOWEST, solves every 8 jobs, autosave every 180 s and on destruction.
- SetProfile (creates the directory; loads `<dir>\<key>.alsr`), SaveProfile, ResetLearning (deletes the file),
  LearningStatus (samples, solves, coverage, held-out MAE generic / learned of recent jobs, learningActive).
- Debug view LearnedBlend: alpha in red over grey generic luma.
- alsr_dev: `LearnerOf(upscaler)`, `LearnSync(upscaler)` (tools only, blocking).
- Profiler pass names: + "learned", "collect".

Self-test (`build\alsr\bin\moonsr_tbench.exe --adapter 5060 --selftest`): 62 checks, 0 failures (was 38). New:
untrained useLearned == generic bit-exact; model estimated point on point-sampled input; gating 0 violations (held-out
SSE generic 570.58 -> published 514.11, 206/648 buckets active); useLearned=false == generic bit-exact (RenderSingle,
and Render over 6 frames on a trained upscaler vs a fresh one); SpatialCpu vs GPU spatial mean 0.032, max 1 level;
GPU learned vs CPU reference mean 0.045, max 12, 790 of 6.2M values > 1 level (bucket flips at thresholds); true GT
on an unseen frame 27.965 -> 29.070 dB; save / load round trip identical; corrupt and truncated files ignored;
ResetLearning deletes the file and restores generic output; online collection over 24 Renders: 853776 samples,
6 solves; nothing bound after Render with learning.
Profile size: 5,966,880 bytes (one 1.5x section: 648 buckets x 3 double equation sets).
Known: online held-out MAE learned 8.124 vs generic 8.092 in that run - the gate minimises SQUARED error, so the
contract's MAE fields can show the learned path slightly worse while its RMSE is better.

Next: 3e evaluation in tbench (learning curves vs true GT through the real Render path), costs (--time with
learning on/off, worker ms).

---

## 2026-11-15 - Stage 3d cost (RTX 5060)

Command: `build\alsr\bin\moonsr_tbench.exe --adapter 5060 --time --passes --tag s3_time` (pan_medium aliased frames,
s0.3, 200 iterations; mode "trained + collecting" = bank pre-trained on the CPU from frame a, learning on).
| size | Render off | Render trained+collect | learned pass | collect | RenderSingle off / on | PushFrame |
|---|---|---|---|---|---|---|
| 720p -> 1080p | 1.614 | 1.790 | 0.170 | 0.002 | 0.343 / 0.514 | 0.785 |
| 1080p -> 1440p | 3.394 | 3.716 | 0.318 | 0.002 | 0.617 / 0.935 | 1.617 |
| 1080p -> 2160p (2x: learning disabled) | 5.060 | 5.064 | - | 0.000 | 1.371 / 1.371 | 1.629 |
Collection target (<= 0.3 ms) met: 0.002 ms GPU. Worker 24.8 ms (720p) / 27.6 ms (1080p) wall per 240x240 tile job
(level 0 + level -1 pairs), THREAD_PRIORITY_LOWEST. VRAM: + learned target 4 B / output px, + 0.66 MB (staging ring,
table). RenderSingle now creates the table itself if the instance has not rendered yet.

Next: 3e (`--learn-eval`: learning curves through Render, cross-sequence matrix, ATLYSS mosaics).

---

## 2026-11-15 - Stage 3e (part): real content through the GPU path

Command: `build\alsr\bin\moonsr_tbench.exe --adapter 5060 --learn-eval real` (new `tools/tbench/learn_eval.cpp`).
Train on in_1466 (CPU ProcessSync, 2 passes), evaluate on in_1467 (almost the same view - near training data).
- model point; self-supervised on in_1467: MAE generic 3.967 -> learned 3.975 (no gain), RMSE 12.939 -> 12.424
  (-4 %); 312/648 buckets active; 0 gating violations. (Numbers differ from the 3a entry because the point model is
  now the unbiased stochastic one, which adds irreducible noise to the self-supervised targets.)
- GPU output learned vs generic at 1920x1080: mean |diff| 0.635 levels, max 89.
- LOOKED AT `benchmarks/output/alsr_learn/real_mosaic_*_input_generic_learned.png` (input nearest | generic | learned,
  4x of output): edges (pillar / ledge diagonal) - learned and generic look the same, edges NOT visibly smoother;
  stairs texture - learned slightly crisper texture blocks, stair-step jaggies on the shallow edges remain in both;
  HUD quest text - learned strokes very slightly crisper / more contrast, no halos visible; hotbar not yet inspected.
  Honest summary: on this content the learned filters make a small, subtle difference; they do not remove jaggies.
  Debug image `real_in_1467_learned_blend_debug.png` (alpha in red).

Next: `--learn-eval curves` / `cross` (synthetic, true GT, through Render).

---

## 2026-11-15 - Stage 3e: learning curves through the real upscaler (RTX 5060)

Command: `build\alsr\bin\moonsr_tbench.exe --adapter 5060 --learn-eval curves` (4 min). Training: one 240x240 tile per
Render from a pool of 60 frames of pan_medium (frames 500 + 7k, never evaluated), LearnSync after each Render, solve +
save at each checkpoint; evaluation by a FRESH upscaler that loads the saved profile (learning off): mean over frames
12..31 of pan_medium (A, same content type, new frames) and thin_features (B, unseen content). PSNR dB / SSIM vs TRUE GT.
| case | frames | A single | A ALSR | B single | B ALSR |
|---|---|---|---|---|---|
| 1.5x aliased | 0 | 28.051 / 0.93002 | 29.818 / 0.95227 | 16.873 / 0.72712 | 17.080 / 0.74191 |
| | 30 | 29.458 / 0.94122 | 29.849 / 0.95235 | 17.306 / 0.73211 | 17.351 / 0.74438 |
| | 100 | 29.530 / 0.94075 | 29.850 / 0.95235 | 17.427 / 0.73120 | 17.500 / 0.74788 |
| | 300 | 29.558 / 0.94011 | 29.850 / 0.95236 | 17.379 / 0.72385 | 17.520 / 0.74919 |
| | 1000 | 29.569 / 0.94001 | 29.850 / 0.95237 | 17.293 / 0.71589 | 17.500 / 0.74940 |
| 1.5x prefiltered | 0 | 33.394 / 0.96179 | 32.600 / 0.96002 | 20.930 / 0.82063 | 21.103 / 0.83863 |
| | 30 | 35.121 / 0.97199 | 32.605 / 0.96002 | 21.029 / 0.82748 | 21.107 / 0.83880 |
| | 100 | 35.285 / 0.97426 | 32.605 / 0.96003 | 21.095 / 0.82893 | 21.122 / 0.83919 |
| | 300 | 35.333 / 0.97557 | 32.605 / 0.96003 | 21.160 / 0.82785 | 21.149 / 0.83967 |
| | 1000 | 35.370 / 0.97624 | 32.605 / 0.96002 | 21.252 / 0.82745 | 21.170 / 0.84003 |
| 2x aliased | 0 and 100 | 27.181 / 0.88179 | 30.433 / 0.94943 | 15.791 / 0.62532 | 16.249 / 0.64476 | (identical: learning disabled above 1.75x)
| 2x prefiltered | 0 and 100 | 30.969 / 0.91146 | 29.934 / 0.92041 | 19.425 / 0.73264 | 19.658 / 0.73911 | (identical)
Findings
- Single-frame path (RenderSingle - what generated frames get): +1.5 dB (aliased) / +2.0 dB (prefiltered) on the
  trained content type; on unseen content +0.4..0.55 dB (aliased) / +0.1..0.3 dB (prefiltered). Most of the gain is
  reached after 30 frames (30 tiles); plateau after ~100.
- Aliased, unseen content (B single): PSNR peaks at 100 frames (+0.55) then drops (+0.42 at 1000) and SSIM falls below
  the untrained value from 300 frames (0.72712 -> 0.71589): specialisation to A hurts B's structure. Mild, but a
  real "learning can hurt unseen content" case (SSIM).
- FULL ALSR barely benefits: +0.03 dB (A aliased), +0.005 dB (A prefiltered), +0.42 / +0.07 dB on B. The learned
  reconstruction is only the seed / fallback; accumulated history dominates wherever motion is tracked.
- On pan_medium prefiltered 1.5x the LEARNED SINGLE-FRAME path (35.37) beats full ALSR (32.61) by 2.8 dB, and even the
  generic single-frame path (33.39) beats ALSR (pre-existing Stage 2 weakness on medium-speed prefiltered pans).
- The probe status shows "model unknown" because a learner reports per-section status only after the first Render
  (Configure); LearningStatus before the first Render has profileLoaded only.

Next: cross-sequence matrix (`--learn-eval cross`), Stage 4 table (`--quality --profile-train 300`).

---

## 2026-11-16 - Stage 3f: cost cuts towards the DLSS 4.5 SR ceiling (new engineer; IN PROGRESS)

Ceiling from the lead (NVIDIA DLSS Programming Guide 310.6.0, documentation/research/nvidia.md section 13): DLSS 4.5
SR Preset M, 1080p output, RTX 5070 = 0.75 ms; applied to our RTX 5060 as the ceiling for Render + collection at 1080p
output; 1.33 ms at 1440p. No time-based fallbacks of any kind (none exist in src/upscale; the engine's frame
generation multiplier back-off in src/engine/engine.cpp is outside ALSR and not touched).
Build dir of this engineer: `build\upscale` (`tools\build.ps1 -BuildDir build\upscale -Target moonsr_tbench`).
Baseline at hand-over (tag s3_handover, `--time --passes`, alternating frames 0/1 of pan_medium, s0.3): 720p -> 1080p
Render 1.652 (learning off) / 1.857 ms (trained + collecting); 1080p -> 1440p 3.478 / 3.793.

Changes (all measured quality-neutral against tag s3c_base = the hand-over tree, `--quality --scales 1.5,2 --inputs
aliased,prefiltered --baselines none --tsharp 0 --no-curves`; see the Stage 3f quality table below):
1. alsr_cellrefine_cs: 16 threads per cell (4 cells per 64-thread group), the 6 v-independent sums in one reduction,
   26x26 groupshared luma tile when cellPx == 8 (default flow scale). The pass was latency-bound (serial
   fetch/reduce chain per cell; fewer threads per cell = more cells in flight). 0.48 -> 0.075 ms (720p input).
   8 threads per cell (8 cells per group) measured slower (0.085).
2. History gamma copy: alsr_update writes MRT1 = RGBA8 through an _SRGB RTV (hardware sRGB encode); alsr_final reads
   it (no pow per tap). final 0.11 -> 0.05 ms.
3. Seed = MoonSR Spatial + learned bank fused (shaders/upscale/alsr_seed.hlsli; the spatial algorithm moved into
   moonsr_spatial.hlsli, used by both ScalePass and ALSR). Render no longer runs the spatial and learned passes for every
   pixel: alsr_reproject writes trust x history and a D16 "needs seed" mask (SV_Depth), alsr_seedblend (new) runs only
   where trust < 1 (early depth test) and adds (1 - trust) x seed with additive blending. Fusing the seed into
   alsr_reproject itself was tried first: it cost 0.19 ms through register pressure even where unused. A discard-based
   seedblend cost 0.04 ms for the discards alone; the depth mask brought the pass to 0.025-0.045 ms incl. the seeds.
   RenderSingle: alsr_seed_ps (one pass) + final. alsr_learned_ps.hlsl is gone (its logic is in alsr_seed.hlsli; the
   old file is not in the tree any more).
4. Motion field RGBA32F -> RGBA16F (halves its traffic; quality-neutral).
5. alsr_refluma split into Y (R32F, one fetch per source px) + 5x5 binomial as 9 bilinear taps of Y: exact
   equivalent of the 25-tap version. MEASURED TRAPS: interpolating the gamma copy instead of the linear history, or a
   9-tap approximation of the blur on the history, cost up to -0.65 dB (pan_slow 2x aliased) / -0.48 (zoom_rotate 2x)
   through worse registration.
6. alsr_prep: 5x5 binomial luma as 9 bilinear fetches (exact up to sampler weight precision): 0.048 -> 0.045.
7. Tried and reverted: Gather-based overlay Layer() in alsr_motion (slower than 26 R8 Loads on the 5060, twice);
   lkIters 1 (-0.3..-0.5 dB on aliased pans).
New tbench option `--time-seq`: 16 consecutive pan_medium frames played ping-pong (continuous pan) instead of the
alternating frames 0/1 (motion reverses every frame - a worst case for the velocity filter and the seed coverage).

### Stage 3f state at hand-over (2026-11-16, run stopped at the lead's request)
Full quality check after changes 1-6 (tag s3c_q2 vs s3c_base, all sequences, 1.5x/2x, aliased/prefiltered): worst
-0.027 dB (thin_features 2x aliased); 0 regressions beyond that. Self-test 62/62 (with learning applied).
Also done: alsr_update variant ALSR_UPDATE_DIRECT (sharpness 0, no debug view: update writes dst + LastOutput, final
skipped). tbench `--time-sharp v`.
Cost, RTX 5060, isolated GPU time (other desktop apps incl. Lossless Scaling were running on this GPU: run-to-run
spread of about +-0.05 ms; numbers are means of 200 iterations):
| workload | 720p->1080p Render (learning off / trained+collecting) | 1080p->1440p |
|---|---|---|
| hand-over, alternating frames | 1.652 / 1.857 | 3.478 / 3.793 |
| now, alternating frames, s0.3 | 0.773 / 0.776 | 1.683 / 1.683 |
| now, --time-seq, s0.3 | 0.703-0.723 / 0.714-0.818 | 1.436-1.487 / 1.56-1.60 |
| now, --time-seq, s0 | 0.672 / 0.770 | 1.355 / 1.49 |
Ceiling 0.75 (1080p): met on the continuous pan, about 0.03 ms over on the alternating worst case. Ceiling 1.33
(1440p): NOT met (about 1.44-1.70). PushFrame (motion estimator, shared with frame generation) is extra: 0.79 ms /
1.63 ms.

LEARNING VS TRUE GT (new `--learn-eval truth[:case]`, tools/tbench/learn_truth.cpp; run tag t1, real cases only):
- real_area (ATLYSS, area-reduced 1.5x): single 25.962 -> 26.048 -> 26.170 -> 26.389 -> 26.488 dB (0/30/100/300/1000
  frames), ALSR 26.700 -> 26.951; improves steadily, BUT 53 of 628 active buckets are worse than generic vs GT,
  8 significantly (> 1 %, n >= 1000; worst +26.7 % RMSE).
- real_point (nearest-reduced): single 24.841 -> 24.973 (30) -> 24.839 (1000), SSIM 0.8833 -> 0.8701; ALSR SSIM
  0.8941 -> 0.8895. LEARNING MAKES IT WORSE; 83 significant bucket regressions. Self-supervised gate: 0 violations,
  i.e. it cannot see this (same failure as the synthetic 2x finding).
- Synthetic truth cases (syn_aliased / syn_prefiltered) not yet run in this harness.
DECISION: learned filters are NOT applied by default (Tune applyLearned = 0; MOONSR_ALSR_TUNE=applyLearned=1 or
tbench --tune applyLearned=1 to apply). Collection and the profile still run, so profiles keep accumulating. With the
default, two self-test checks that need the bank applied fail by design (learned pass vs CPU reference, learned beats
generic); with applyLearned=1 the self-test is 62/62.
NEXT (planned, not started in code): actual-scale validation gate. With each collected tile, also read back the
history (RGBA16F, output footprint) and the motion tile; accumulate per bucket a 4th equation set (features from the
real input at the real scale, target = converged history luma where count >= 8, confidence >= 0.5, |v| >= 0.05);
in Solve cap alpha at the minimiser of the history SSE on [0, alpha] and require >= 1 % gain (profile version 3).
Then rerun `--learn-eval truth` (all cases) and only enable applyLearned if no significant bucket regression remains.
Cost ideas left: 1440p source-res passes (prep, cellrefine, motion, residual, refluma = about 0.77 ms); LastOutput copy
is unused by the engine (proposal to the lead: drop or make it opt-in, saves 0.02-0.03 ms).
