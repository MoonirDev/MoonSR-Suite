# Anti-aliasing on captured frames: edge reconstruction without engine data

Author role: graphics-researcher (lead review). Date: 2026-11-23. Repository state read: commit `3610f95`.
Scope: the anti-aliasing (AA) section of MoonSR. The universal capture path receives a finished, resolved picture: no
depth, no motion vectors, no jitter, no MSAA samples. This note surveys what can be done in that constraint, proposes
an implementation and a contract, and records the decisions the owner has to take. It contains no engine code; the
interface sketch in Part 7 is a proposal for the lead, who owns the contract headers.

## Provenance legend (read first)

| Tag | Meaning |
|---|---|
| **[C]** | Verified by reading this repository at commit `3610f95` (file named). |
| **[H]** | Historical MoonSR measurement recorded in another MoonSR document (named). Not re-run for this note. |
| **[V]** | Verified this session from the cited source. |
| **[M]** | From prior knowledge of the cited work, **not re-fetched this session** (this note was written without network access, per the project rules). Check before relying on a detail. |
| **[I]** | Inference or engineering reasoning by the researcher. |
| **[E]** | Cost or quality **estimate**. Not a measurement. Must not appear in `BENCHMARKS.md`. |
| **[U]** | Unknown, or not publicly documented. |

Implementation update (2026-11-26): the spatial tier was rebuilt as a three-pass pattern-based morphological filter
(MLAA family). The FXAA-style build whose measurements appear under "Measured after T1" is superseded; the rebuild and
its measurements are at the end of this note. The contract, the settings section, the page and the load-order stage are
unchanged. Automated evaluation and the settings sweep are described in `aa_benchmark.md` and run with
`moonsr_bench --aa-suite` / `--aa-sweep`.

---

## Part 0: Summary and recommendation

1. **Only spatial post-process AA is available.** MSAA cannot be used: the game already resolved its samples before
   Windows Graphics Capture receives the frame. Temporal AA needs jitter, motion vectors and a depth buffer, none of
   which exist on the universal path [C]. DLAA is a DLSS runtime mode that requires NVIDIA Tensor Cores [M], which the
   project rules forbid as a requirement. What remains is the morphological / edge-directed family: MLAA, FXAA, SMAA
   and CMAA2 [M].
2. **The first tier is a clean-room edge-directed luma filter** ("Spatial" mode). It is the cheapest useful thing that
   can be built on one frame: detect a luminance step, find the edge direction and its two ends, and blend a
   low-passed colour weighted by the estimated coverage. It is the FXAA-class idea [M] implemented from published
   descriptions, not copied shader code (Part 5).
3. **A second tier adds pattern-based reconstruction** ("Directional" mode, SMAA-class): precomputed coverage areas
   for the eight edge shapes, neighbourhood blending for diagonals. It is better on near-horizontal and diagonal
   edges and blurs text less, at roughly twice the taps.
4. **Placement: a stage in the user's load order, default before frame generation and before upscaling.** At game
   resolution it costs the least, it runs once per *real* frame (generated frames inherit it), and the upscaler then
   enlarges clean edges. The load order lets the owner move it after upscaling for A/B comparison; the pass then runs
   at output resolution and costs proportionally more.
5. **It cannot fix what the game broke.** If the game renders at 720p with no AA and MoonSR upscales to 1440p, a
   spatial filter on the 720p input removes the staircase but cannot invent the detail the game never rendered. If the
   game already runs TAA, the picture is already smooth and a second spatial pass mostly softens it; the "already
   anti-aliased" detector (2.7) is meant to reduce strength there. Neither behaviour is a claim until measured.
6. **Cost target.** ≤ 0.3 ms per 1080p pass and ≤ 0.6 ms at 1440p on the owner's RTX 5060 [E]. A single-pass
   edge-directed filter at 1080p is bandwidth-bound (one read and one write of an 8 MB image) and should land well
   under that [I]; the estimates are in 4.5 and must be replaced by measurements.
7. **Text and HUD protection is part of the first tier, not an optimisation.** AA on 1-pixel text strokes is the most
   visible failure of the whole family [M]. The first tier uses a thin-stroke test; static-pixel (HUD) protection
   needs history and comes with the second tier.

---

## Part 1: What the capture path gives us

### 1.1 Facts from this repository [C]

- The capture source delivers `CapturedFrame` with a `texture` and a `contentRect`; the engine treats it as the
  original picture (`src/engine/engine.cpp`). Windows Graphics Capture resolves the game's back buffer, so any MSAA
  the game used is already gone.
- `MotionFieldView` (`src/framegen/frame_generator.h`) exists only while the frame generator runs. It is a
  Texture2DArray of RGBA32F, one texel per motion cell (8 px cells at 720p) [H]. It is a block field, not a per-pixel
  velocity buffer, so it is not good enough for temporal AA on thin features (3.3).
- Captured pixels are gamma-encoded SDR BGRA8 and are **not** linearised (`shaders/README.md`). All filters work on
  encoded values. Luma is the perceptual weighting `dot(c, (0.299, 0.587, 0.114))`
  (`shaders/common/scale_common.hlsli`).
- Smart parallax already runs a compute pass that computes a static-pixel (HUD) counter and several gates
  (`shaders/relief/relief_gate_cs.hlsl`, `src/relief/relief_pass.cpp`). The same idea — static pixels are HUD — can
  serve AA in a later tier, but the AA pass itself is single-frame in T1.
- The engine can already route a stage through its own GPU with a cross-adapter copy
  (`CrossAdapterLink`, `StartRelief`/`RenderRelief` in `src/engine/engine.cpp`). AA can follow the same pattern.
- GPU timings and statistics reach the CPU through a ring of staging buffers mapped with `DO_NOT_WAIT` and timestamp
  queries (`src/relief/relief_pass.cpp`), so no new mechanism is needed for AA's cost display.

### 1.2 What is missing, and why it matters

| Missing input | What it would enable | Why we do not have it |
|---|---|---|
| MSAA samples | True geometric coverage | The game resolved them before capture |
| Jitter / sub-pixel offsets | Temporal super-sampling (TAA/TAAU) | The game owns its projection matrix |
| Per-pixel motion vectors | Ghost-free history reprojection | Only available through the optional game integration bridge, and even there it is a game-specific contract |
| Depth buffer | Edge-aware upsampling and AA in 3D-aware space | Same |
| HDR / linear light | Correct blending on HDR games | The capture path is SDR BGRA8 today [C] |

Consequence: every AA tier in this note is a **2D image filter**. It estimates coverage from luminance and colour
transitions. It cannot know whether a step is a geometric silhouette, a texture boundary, a shadow edge or text.

### 1.3 The two families that remain

**Morphological / edge-directed (spatial).** MLAA (Reshetov, 2009) finds horizontal and vertical edges, measures the
run length of the pattern, and reconstructs the coverage of the implied line [M]. FXAA (Lottes, 2009–2011) does the
same with a cheaper structure: luminance edge test, gradient-derived orientation, a bounded search along the edge,
and a blend toward a local average [M]. SMAA (Jimenez et al., 2011) replaces the run-length search with a precomputed
area lookup indexed by the 8×8 neighbourhood pattern, which is more accurate on diagonals and thin shapes [M].
CMAA2 (Strugar, 2018) is a compute-shader variant with local-contrast adaptation and an explicit line-processing
queue [M]. All four are single-frame; all four blur where they cannot tell an edge from detail.

**Temporal.** TAA and its variants accumulate sub-pixel-jittered frames, reprojected by motion vectors [M]. Without
jitter and vectors the history smears moving content (ghosting). MoonSR's block motion field could reproject history
coarsely, but 8-pixel cells cannot follow a 1-pixel text stroke or a railing; the result would be visible ghosting on
exactly the content AA is supposed to fix [I]. This is a research tier, not a product tier.

**Neural.** DLAA-class networks need a runtime and weights; the project forbids bundling or downloading either, and
requires cross-vendor operation. The neural rendering bridge could host a user-supplied model later, but it is not an
AA design and not on the critical path [I].

---

## Part 2: The proposed filter

### 2.1 Signal model

A spatially aliased edge is a transition that happens inside one pixel: the sampled value jumps from A to B with no
intermediate coverage. A correctly filtered edge spreads that transition over the pixel footprint. A spatial
post-filter cannot recover the true coverage, but it can estimate it from the local profile: a step that is
**monotonic** across the transition is an edge; a step that returns (a 1-pixel line or a dot) is detail and must be
kept.

The filter therefore has four jobs, in order:

1. **Detect** a luminance step above a contrast threshold.
2. **Orient** the edge and find where it ends, so the blend follows the edge rather than crossing it.
3. **Estimate coverage** per pixel along the edge.
4. **Blend** toward a low-passed colour by that coverage, scaled by the user's strength and the protection masks.

### 2.2 Detection

Luma is `dot(rgb, (0.299, 0.587, 0.114))` on the encoded values, matching the rest of the shader set [C]. For each
pixel, take the four direct neighbours and compute `range = max(luma) - min(luma)`. Skip the pixel when

```
range < max(edgeThresholdMin, maxLuma * edgeThreshold)
```

`edgeThresholdMin` is the absolute floor that keeps dark scenes from being over-filtered; `edgeThreshold` is the
relative threshold. Both are exposed (Part 7), because the right values depend on the game's contrast and on whether
the owner wants "invisible" or "crisp".

The early exit is also the texture-preservation mechanism: a high-frequency texture has a high local range but its
gradients point in many directions, and the edge search that follows fails to find a consistent edge, so the blend
falls back to zero (2.4).

### 2.3 Orientation and edge search

The gradient across the 3×3 neighbourhood gives a direction. The filter walks along the perpendicular (i.e. along the
edge) up to N steps, stopping when the edge ends or the luminance turns. The two ends define the edge segment; the
blend weight varies along it, which is what makes a staircase into a straight line instead of a uniform blur.

Quality levels change N and the number of taps:

| Quality | Search steps | Taps (approx.) | Intended use |
|---|---|---|---|
| Performance | 4 | 9–13 | 4K output, integrated GPUs |
| Balanced (default) | 8 | 13–21 | 1080p–1440p |
| Quality | 12 | 21–29 | A/B comparison, stills |

The step counts are a design choice, not a standard; they are bounded by SM 5.0 instruction limits and by the cost
target. They are the first thing to tune in M1.

### 2.4 Blend and sub-pixel weight

Two weights are combined:

- **Edge weight**: how far the pixel sits from the edge centre, derived from the search; 0 at the ends, 1 in the
  middle of a long edge.
- **Sub-pixel weight**: a local 3×3 low-pass compared with the centre pixel. Where the neighbourhood average differs
  from the centre, the pixel is likely on the thin side of a nearly-horizontal or nearly-vertical edge and gets more
  blend; where it matches, the pixel is interior and gets less.

Final blend = `strength * edgeWeight * subPixelWeight * protection`, clamped to a maximum (a hard cap such as 0.85
prevents the filter from turning a hard edge into mush at strength 1.0). The blended colour is a bilinear or
4-tap average along the edge direction, not a box blur, so texture on both sides is preserved.

### 2.5 Text and thin-stroke protection

Text is the failure case. A 1-pixel stem is a high-contrast step on both sides; a filter that treats it as an edge
blurs it into the background. The first tier uses a **thin-stroke test**:

- The pixel is a local extremum (brighter or darker than all four direct neighbours), and
- the 3×3 range is large (a strong feature), and
- the feature is at most 2 pixels wide in both axes (the neighbourhood returns to the background value within one
  pixel on each side).

When all three hold, the protection weight is reduced (not necessarily to zero: a *little* smoothing of jagged text is
usually preferable to leaving it untouched, but the UI exposes it as a toggle so the owner can decide). This is a
heuristic; it will sometimes protect a genuinely aliased thin edge and sometimes miss a thick letter. It is measured
in 4.4 with a text-over-texture scene.

The second tier adds **static-pixel protection**: pixels that did not change over the last few frames are likely HUD,
and the relief gate already computes exactly this statistic [C]. That requires history and a reset rule (scene cut,
resize), so it is not in T1.

### 2.6 Diagonals

Near-45° edges are the weakest case of the edge-search family: the search runs out of steps, or the two ends are
ambiguous. The second tier's pattern lookup handles them directly; the first tier mitigates with a longer search at
Quality and with the sub-pixel weight. This is an honest limitation of T1, measured in 4.2.

### 2.7 "Already anti-aliased" detection

A second spatial pass on an already smooth image is pure softening. The proposal is a per-frame statistic computed
inside the AA pass and read back with a lag (the same mechanism as the relief statistics [C]):

- **Aliased coverage**: share of pixels where a strong step exists *and* the transition is confined to one pixel
  (the neighbours on both sides are flat).
- **Smooth coverage**: share of pixels where a strong gradient exists but is spread over two or more pixels.

When `aliased / (aliased + smooth)` is low, the image is already resolved and the effective strength is scaled down
toward a floor (for example 0.25), with a plain-language note in the UI ("the game already looks anti-aliased; MoonSR
AA is reduced"). The detector cannot distinguish TAA blur from a soft game; it should therefore only *reduce*, never
turn the effect off silently, and the user can disable it. This is a heuristic and must be validated on the owner's
clips before it is presented as working.

### 2.8 Temporal stability

T1 has no history, so its output is a deterministic function of one frame. On static content it is perfectly stable.
On moving content the edge search can pick different ends in consecutive frames, which reads as a faint shimmer on
high-contrast thin geometry. Mitigations that do not need vectors:

- bias the search toward the previous frame's orientation (one extra tap from a small history texture) — this is
  history, so it belongs to T2;
- keep the blend cap moderate, so a flip changes the image by a small amount;
- prefer the longer search at Quality, which is less likely to flip.

The measured criterion (4.4) is: frame-to-frame difference on a static scene must be zero, and on the moving test
scene must not exceed the input's own frame-to-frame difference by more than a small margin.

---

## Part 3: Interaction with the rest of MoonSR

### 3.1 Where AA can run

| Position | Resolution | Runs per | Notes |
|---|---|---|---|
| A: before frame generation and before upscaling | game | real frame | Cheapest. Generated frames inherit the AA. Improves the generator's input. Default. |
| B: after upscaling | output | displayed frame | Cleans upscaler ringing and residual aliasing. Costs scale with output pixels and with the frame-gen multiplier. |
| C: after frame generation, before upscaling | game | displayed frame | Pointless: same resolution as A for more calls. Excluded by the load order's meaning. |
| D: before neural rendering | game | real frame | Only sensible when neural rendering runs at game resolution; otherwise the model sees filtered input it was not trained for. |

Position A is the recommendation. It is the only position where the cost is paid once per *game* frame regardless of
frame generation, and it is where the upscaler benefits most: enlarging an already-smooth edge is what every
conventional scaler is good at.

### 3.2 Interaction with MoonSR temporal upscaling (ALSR)

ALSR accumulates history and reconstructs the image itself; it expects aliased input and resolves it. Pre-filtering
with a spatial AA removes high-frequency information the reconstruction could have used, and the two can disagree on
what an edge is [I]. Recommendation: when the scaler is MoonSR, the UI notes that MoonSR upscaling already resolves
aliasing and AA is usually unnecessary; the default strength is reduced (the auto detector will usually do this
anyway). The owner decides whether the combination should be allowed at all (D8).

### 3.3 Interaction with frame generation

With AA before frame generation (A), the generator's motion field is computed on the filtered image. Filtering does
not move edges, so the field is unaffected in practice. The interpolation blends two filtered images, which is what
we want: no per-generated-frame AA cost, and no difference in sharpness between real and generated frames.

With AA after frame generation (B), every generated frame gets the pass; at 4× that is four times the cost. The
engine must account for it in the statistics (`aaGpuMs` per displayed frame, not per real frame).

### 3.4 Interaction with Smart parallax

Both are spatial filters. Relief derives its height from the captured detail, so AA before relief removes some of the
signal relief uses; relief before AA means AA smooths the relief shading, which is low-amplitude and mostly
unaffected. The default order puts AA early (before frame generation), which is before relief in practice; this is
acceptable because relief's band-pass centre (about 2 px) is above the scale AA touches (about 1 px). The owner can
reorder; the UI description says so.

### 3.5 Interaction with A/B compare and the debug view

The existing A/B compare replaces the left half of the output with the raw capture. With AA on, that becomes "raw
aliased" versus "AA" — a useful comparison and exactly what the owner asked for. The AA edge-mask debug view is
separate: it replaces the output with the detected edges (white on black) so the owner can see what the filter
thinks is an edge. It must be excluded from A/B compare's left half, like the rest of the final image.

### 3.6 Load order and per-stage GPU

Following the Smart parallax pattern [C]:

- New `PipelineStage::AntiAliasing` appended to the enum.
- `PipelineOrder` default: `NeuralRendering, AntiAliasing, FrameGeneration, Upscaling, Relief`.
- `NormalizeOrder` **appends** missing stages instead of resetting: a 4-stage order (the 1.1.0 set) gains
  `AntiAliasing` at the front of the non-neural stages, and a 3-stage order gains both `Relief` and `AntiAliasing`.
  The relative order of the stages the file already has is preserved.
- `StageAdapters` gains an explicit `antiAliasing` LUID with a `For()` case and `SetAll`.
- `AppSettings::stageGpu` grows from 4 to 5 entries; old files load with Auto for the new entry (the reader already
  tolerates missing keys).
- `pipelineOrder` strings gain the token `aa`; the reader maps unknown tokens to the default (existing behaviour).

The migration rule is important: the owner has a saved order, and resetting it would be a visible regression. The
validator appends; it never discards.

### 3.7 Cost and memory

One full-resolution intermediate at the pass position (BGRA8: 8 MB at 1080p, 14 MB at 1440p, 33 MB at 4K). The pass
reads one texture and writes another; there is no history in T1, so nothing else is retained. At the default position
the intermediate is game resolution, not output resolution.

---

## Part 4: Quality and measurement

### 4.1 What "better" means here

AA is judged by four things that pull against each other:

1. **Edge staircase removed**: the step along a hard edge is smaller.
2. **Detail preserved**: high-frequency texture energy does not drop.
3. **Text and thin lines intact**: pixels on 1-pixel strokes barely change.
4. **Stable**: no shimmer on static content, no new crawling on moving content.

A filter can win 1 and lose 2; that is what FXAA at high strength does. The measurement must report all four.

### 4.2 Synthetic scenes (`moonsr_bench --aa`)

Rendered procedurally at 1280×720 and 1920×1080:

| Scene | Content | Metric |
|---|---|---|
| `edges` | Hard black/white steps at 0°, 15°, 30°, 45°, 60°, 75°, 90° | Mean absolute step across the edge before/after; reduction % |
| `lines` | 1-pixel and 2-pixel lines on flat backgrounds | Peak luminance retained; width change |
| `text` | Small text (10–14 px) over a mid-grey texture | Max and mean absolute difference inside text pixels |
| `texture` | High-frequency checkerboard, noise and fine detail | High-frequency energy ratio before/after (must stay within ±5 %) |
| `gradient` | Smooth gradients with a few hard bands | Banding introduced (new steps) |
| `hud` | Static HUD-like overlay over a moving background | Difference inside static pixels (T2) |

The pass runs on the GPU through the real `AaPass`, and the metrics are computed on the read-back image, the same way
`--relief` works [C].

### 4.3 Real material (`moonsr_bench --aa-image <png>`)

Screenshots supplied by the owner, processed at their own resolution:

- game AA off;
- game AA or TAA on;
- MoonSR AA on in the same scene.

The tool prints the measured statistics (edge coverage, strength in effect, GPU time) and saves before/after images
for visual comparison. Quality is *not* scored automatically on real material; the owner's eyes decide. The numbers
are reported, the judgement is the owner's.

### 4.4 Proposed acceptance thresholds (not measured)

| Check | Threshold | Rationale |
|---|---|---|
| Edge step reduction at strength 1.0, 45° and 90° | ≥ 40 % | Visible improvement without destroying the edge |
| Edge step reduction at 15° and 75° | ≥ 25 % | Near-axis edges are the hard case |
| Texture energy ratio | 0.95–1.05 | AA must not act as a blur |
| Text pixel max difference | ≤ 8 levels of 255 | Text must stay readable |
| Text pixel mean difference | ≤ 2 levels | No visible softening of the glyph body |
| Static scene frame-to-frame difference | 0 | No shimmer without motion |
| New overshoot on a step | ≤ 2 % of the step | No ringing |

These are targets for M1; if a threshold cannot be met, it is recorded as a known limitation, not quietly relaxed.

### 4.5 Cost estimates (must be replaced by measurements) [E]

| Mode | 1280×720 | 1920×1080 | 2560×1440 |
|---|---|---|---|
| Spatial, Performance | 0.04–0.07 ms | 0.07–0.12 ms | 0.13–0.22 ms |
| Spatial, Balanced | 0.06–0.10 ms | 0.10–0.18 ms | 0.18–0.30 ms |
| Spatial, Quality | 0.09–0.14 ms | 0.15–0.25 ms | 0.28–0.45 ms |
| Directional (T2) | 0.10–0.18 ms | 0.18–0.30 ms | 0.32–0.55 ms |

The estimates assume a bandwidth-bound single pass plus a bounded search; they are consistent with published costs of
the family [M] but are **not** MoonSR measurements. `BENCHMARKS.md` gets only numbers produced by the tool.

---

## Part 5: Prior art and licences

| Technique | Origin | Licence | Use in MoonSR |
|---|---|---|---|
| MLAA | Reshetov, Intel, 2009 [M] | Paper describes the method; sample code licence not confirmed [U] | Method reference only |
| FXAA 3.11 | Lottes, NVIDIA, 2009–2011 [M] | NVIDIA released it under a BSD-3-Clause licence in the GameWorks repository [M]; the original blog release was effectively public domain [M] | Method reference; clean-room implementation |
| SMAA 1x / T2x | Jimenez et al., 2011; Unity [M] | MIT [M] | Method reference for T2 |
| CMAA2 | Strugar, Intel, 2018 [M] | MIT [M] | Alternative if T2 misses its cost target |
| DLAA | NVIDIA, DLSS 2.3+ [M] | Proprietary runtime [M] | Not used (vendor lock, Tensor Cores) |
| TAA / TAAU | Common industry practice [M] | n/a | Not usable without jitter and vectors |

**Implementation rule for this repository.** MoonSR's own code is MIT. The T1 shader is written from the published
*descriptions* of the technique (detect, orient, search, blend), with no third-party shader source read or copied.
The header comment cites the technique. If a later tier adopts a licensed implementation, the origin URL, author and
licence go into the shader header **and** `THIRD_PARTY_NOTICES.md`, and the licence must be compatible with MIT
(BSD-3 and MIT are). No runtime, model or weight is bundled or downloaded.

---

## Part 6: Risks and honest limits

### 6.1 What will look wrong

- **Text and UI**: the thin-stroke test is a heuristic. Some glyphs will soften; some aliased thin geometry will be
  protected and stay jagged.
- **Pixel art and stylised games**: a deliberate 1-pixel step *is* the art. AA will either do nothing (protected) or
  damage the style. A detector is not planned for T1; the owner can turn AA off per game.
- **Already anti-aliased games**: softening. Mitigated by 2.7, not solved by it.
- **Near-diagonal edges**: T1's search is weakest here. T2 exists for this.
- **Coloured edges**: blending all channels by the same luma-derived weight desaturates high-contrast colour
  boundaries slightly. Accepted; measured as a colour difference on the edge scene.
- **Sharpening after AA**: the sharpen pass runs inside `ScalePass` and will re-sharpen AA'd edges, partly undoing the
  smoothing and adding halos. AA before upscaling means the sharpen sees the AA'd image at output resolution; the
  interaction must be measured (D6).
- **HDR**: not applicable today (SDR BGRA8) [C]. If HDR capture is added, the thresholds must become relative to the
  signal range, or the filter will over-blur.

### 6.2 When AA should not run

- The image is already smooth (2.7) — reduce, do not silently disable.
- The pass cannot allocate its intermediate — log once, skip AA for the frame, keep the pipeline alive (the relief
  stage's failure handling is the model [C]).
- A game that renders 1:1 with no scaling and no AA: the user asked for AA, so run it; the strength is theirs.

### 6.3 Measurement plan

1. `moonsr_bench --aa` on the RTX 5060: every scene, every quality, strength 0.5 and 1.0, at 720p, 1080p and 1440p.
   Record the metric table and the GPU timings.
2. `moonsr_bench --aa-image` on the owner's screenshots.
3. Owner A/B in a real game at 1080p output: game AA off / on / MoonSR AA on, same scene, same camera.
4. Only then does `BENCHMARKS.md` get a row, with hardware, driver, commit and settings.

---

## Part 7: Milestones and the contract

### 7.1 Milestones

**M0: research (this note).** Decisions D1–D12 below. *Exit:* the owner accepts or amends the contract.

**M1: Spatial tier.**
- New module `src/aa/` (`aa_pass.h`, `aa_pass.cpp`), shaders `shaders/aa/`.
- `AaPass::Apply` runs one pixel-shader pass with the quality's tap count; settings clamped; statistics with lagged
  readback; GPU timings.
- Engine wiring: stage creation/release on settings change, load-order position, per-stage GPU, cross-GPU copies,
  reset on resize/scene change, statistics in `EngineStats`.
- UI: new "Anti-aliasing" page with its own vector glyph, mode/quality/strength/edge threshold/text protection, live
  apply, Home details row. The diagnostic views are CLI-only (`moonsr_cli --aa-view`).
- Settings persistence with an old-file default of off.
- `moonsr_bench --aa` and `--aa-image`.
- *Exit:* all synthetic checks of 4.4 pass or are recorded as limitations; measured cost ≤ 0.3 ms at 1080p on the
  RTX 5060; `--selftest` and `moonsr_fgbench --selftest` pass; four themes render the page correctly.

**M2: Directional tier and history.**
- Pattern/area reconstruction; static-pixel protection; already-AA detector validated on real clips.
- *Exit:* measurable improvement over T1 on diagonals and text at no more than 2× the cost; owner's A/B agrees.

**M3: Temporal (research, gated).**
- Only if the frame generator's flow proves stable enough on thin features; otherwise do not build.
- *Exit:* ghosting on thin features below a stated bound, or the tier is dropped.

### 7.2 Contract proposal (the lead owns the headers)

New module `src/aa/` with contract header `aa_pass.h`, owned by the lead; implementation by the upscaling-engineer
(algorithms) with the shader-engineer (HLSL), following the existing ownership table.

```cpp
// Threading: pipeline thread only (owner of the GpuDevice immediate context). No per-frame heap allocation;
// resources are created on first use and recreated only when the size or format changes.
enum class AaMode : uint32_t {
    Off = 0,
    Spatial = 1,      // T1: edge-directed luma blend (cheapest)
    Directional = 2,  // T2: pattern / area reconstruction (better diagonals and text)
    Temporal = 3,     // T3: experimental, needs the frame generator's flow (deferred)
};
enum class AaQuality : uint32_t { Performance = 0, Balanced = 1, Quality = 2 };
enum class AaDebugView : uint32_t { None = 0, EdgeMask, BlendWeight, Split };

struct AaSettings {
    AaMode mode = AaMode::Off;
    AaQuality quality = AaQuality::Balanced;
    float strength = 1.0f;           // 0..1 master blend
    float edgeThreshold = 0.10f;     // relative luma contrast, 0.02..0.5
    float edgeThresholdMin = 0.03f;  // absolute floor for dark scenes, 0..0.2
    bool protectText = true;         // thin-stroke protection
    bool protectHud = true;          // static pixels (T2+; T1 uses the stroke test only)
    bool autoReduce = true;          // already-anti-aliased detection
    AaDebugView debugView = AaDebugView::None; // EdgeMask/BlendWeight/Split are diagnostic
    bool operator==(const AaSettings&) const = default;
};

struct AaStats {                     // read with a lag; never stalls the CPU
    bool active = false;
    AaMode modeInEffect = AaMode::Off;
    float edgeCoverage = 0.0f;       // share of pixels classified as an aliased edge, 0..1
    float strengthInEffect = 1.0f;   // after autoReduce
    std::wstring reason;             // plain-language state when reduced or not active
    double gpuMs = 0.0;              // timestamp median over recent Apply calls
};

class AaPass {
public:
    static std::unique_ptr<AaPass> Create(GpuDevice& device, std::wstring& error);
    virtual ~AaPass() = default;
    virtual void SetSettings(const AaSettings& settings) = 0;
    // Once per frame that reaches this stage, at the position given by the load order. Reads `src` (the finished
    // image of the previous stage, `srcSize`) and writes `dst` (same size, different texture). `contentInDst`
    // excludes letterbox. `outputPixelScale` is the total scale from the captured pixels to the output, so the
    // filter can keep its edge width in output pixels when it runs before upscaling.
    virtual bool Apply(ID3D11ShaderResourceView* src, Size srcSize, ID3D11RenderTargetView* dst, Size dstSize,
                       const RectI& contentInDst, float outputPixelScale = 1.0f) = 0;
    virtual void Reset() = 0;        // scene cut, resize, device recovery
    virtual AaStats Stats() const = 0;
};
```

**Engine-level changes (proposal):**

- `PipelineStage::AntiAliasing = 4` (appended), default order
  `NeuralRendering, AntiAliasing, FrameGeneration, Upscaling, Relief`; `NormalizeOrder` appends missing stages
  (3.6); `StageAdapters::antiAliasing` with `For()` and `SetAll`.
- `EngineConfig::aa` (`AaSettings`) and `Engine::SetAaSettings`.
- `EngineStats::aaActive`, `aaMode`, `aaEdgeCoverage`, `aaStrength`, `aaNote`, `aaGpuMs`.
- `StageName` = "Anti-aliasing"; `StageDescription` = one line about the position's cost.
- `Engine::SetPipelineOrder` already normalises; no signature change.

**Settings persistence (proposal):** a new `[AntiAliasing]` section — `Enabled`, `Mode`, `Quality`,
`StrengthPercent`, `EdgeThresholdMilli`, `EdgeThresholdMinMilli`, `ProtectText`, `ProtectHud`, `AutoReduce`.
Files without the section load off/balanced. `pipelineOrder` gains the token `aa`; unknown tokens keep the current
fallback (default order).

**UI (proposal):** sidebar page "Anti-aliasing" between Frame generation and Neural rendering, with its own vector
glyph (a stepped line becoming a straight one). Rows: enable, mode, quality, strength, edge threshold, text
protection, HUD protection (disabled until T2), auto-reduce. Home details row "Anti-aliasing" with the
state and the measured GPU time, like the Smart parallax row. *(Updated 2026-11-26: the diagnostic-view row was removed
from the page; the views are CLI-only and A/B compare on Home covers the split.)*

---

## Decisions needed (lead / owner)

- **D1: Implementation approach for T1.** Recommended: clean-room edge-directed filter from the published description
  (Part 2), no third-party shader code. Alternative: port FXAA 3.11 verbatim (BSD-3, would go into
  `THIRD_PARTY_NOTICES.md`). The clean-room route keeps the licence surface at zero and lets us tune for upscaling.
- **D2: Default pipeline position.** Recommended: before frame generation and before upscaling (position A), as a
  user-orderable stage. Alternative: after upscaling (B) as the default.
- **D3: Load-order migration.** Recommended: append missing stages, never reset; `StageAdapters` gains an explicit
  `antiAliasing` entry (3.6). This is required for old saved orders to survive.
- **D4: Default state.** Recommended: off globally, opt-in per session/game profile, like Smart parallax.
- **D5: Defaults for mode, quality and thresholds.** Recommended: Spatial, Balanced, strength 1.0,
  `edgeThreshold` 0.10, `edgeThresholdMin` 0.03. The owner's A/B decides the final defaults.
- **D6: Sharpening interaction.** Recommended: measure AA before `ScalePass` sharpening first; if halos appear, offer
  "reduce sharpening while AA is on" as a UI note, not an automatic change.
- **D7: Text protection default.** Recommended: on, with the thin-stroke test; expose the toggle.
- **D8: AA together with MoonSR temporal upscaling.** Recommended: allowed but noted, with the auto-reduce default on.
  Alternative: force AA off (or after upscaling) when the scaler is MoonSR.
- **D9: Temporal tier.** Recommended: do not build now; revisit after M2 with real clips.
- **D10: Ownership.** Recommended: `src/aa/` to the upscaling-engineer (algorithms) with the shader-engineer (HLSL);
  the lead owns `aa_pass.h` and the engine wiring.
- **D11: Naming.** Recommended: page "Anti-aliasing", modes "Spatial (fast)" and "Directional (sharper)", labelled
  experimental until the owner's A/B at M1.
- **D12: Cost guard.** Recommended: no automatic quality drop; show the measured GPU time and the note. A hard
  threshold that silently changes quality is worse than a visible number.

## Claims not verified

- Exact FXAA 3.11 constants (`FxaaQualitySubpix` 0.75, `FxaaQualityEdgeThreshold` 0.166,
  `FxaaQualityEdgeThresholdMin` 0.0833) and search-step counts (8/12/24) are from prior knowledge [M].
- FXAA's licence history (original blog release vs the BSD-3-Clause GameWorks release) is from prior knowledge [M].
- SMAA's area-texture dimensions and pattern tables, and CMAA2's line-queue design, are from prior knowledge [M].
- The cost figures in 4.5 are estimates [E], not measurements, and are not copied from any vendor claim.
- DLAA's exact runtime requirements are summarised from prior knowledge [M].
- No external source was fetched for this note; the repository facts are the only [C] claims.

## Measured after T1 (2026-11-25)

**Historical: this is the FXAA-style build, superseded by the rebuild below.** Kept as the record of what that build
measured and why the owner asked for a rebuild.

The contract in 7.2 was approved as proposed; the owner chose **after upscaling** as the default position (overriding
the recommendation in 3.1), with the pass still orderable. T1 is the spatial tier only; Directional and Temporal are
accepted by the settings but fall back to Spatial with a status reason.

Measurements replace the estimates in 4.4 and 4.5 (full tables and commands: `documentation/BENCHMARKS.md`):

| Check (1280x720 synthetic, RTX 5060) | Result | Proposed threshold (4.4) |
|---|---:|---|
| Slanted edge, strongest scanline step after / before (20-70 deg) | 0.42-0.65 | <= 0.88 |
| Exactly vertical / horizontal edge, max change | 0 | <= 4/255 |
| 4 px checker, luma contrast after / before | 0.781 | >= 0.75 |
| One-pixel lines, mean luma after / before | 1.000 | >= 0.90 |
| Text strokes, mean luma after / before, changed pixels | 0.960, 13.0 % | >= 0.95, <= 25 % |
| Smooth gradient, max change | 0 | <= 4/255 |

| Cost, isolated pass | 1080p | 1440p |
|---|---:|---:|
| RTX 5060 | 0.054 ms | 0.082 ms |
| RTX A400 | 0.350 ms | 0.574 ms |
| Radeon 610M iGPU | 1.430 ms | 2.101 ms |

The 4.5 estimates (0.10-0.18 ms at 1080p for one pass) were pessimistic by about 2x on the RTX 5060 and too optimistic
for the A400 and the iGPU, which the estimates did not cover.

What remains unverified: an objective quality score against the game's own anti-aliasing, any comparison with FXAA /
SMAA / CMAA2 / TAA / DLAA, and the "already anti-aliased" thresholds. The detector counts long, slanted, hard steps on a
128x128 grid, so text and dense textures do not read as aliased; what it measures is hard-edge density (0.0-0.1 % on a
single synthetic edge, 1.0 % on a text-heavy screenshot), which is not the same as "the edges are already resolved".
Auto-reduce is therefore off by default and its 0.02 % / 0.1 % thresholds are synthetic-only constants.

### Real-game check (Helldivers 2, 2026-11-26)

The owner recorded 120-frame sets of Helldivers 2 (AA off, TAA, DLAA, two with MoonSR AA on) and Black Mesa (AA off, two
with MoonSR AA on), then reported three times that the effect was not visible. The final rework abandons the gradient-normal
sampling: it picks the axis the edge steps on from the N/S vs E/W luma variation, samples the two sides along that axis and
blends the pixel towards their average; the straight part of an edge is skipped (the along-edge luma variation is zero
there), which keeps exactly axis-aligned edges and text stems crisp. A stroke-shaped feature is skipped while text
protection is on, and a checker-like pattern (both along-edge neighbours differ by the full contrast) is rejected.

Measured on the no-AA frames: Black Mesa changes 17.7 % of pixels by a mean of 2.95/255 and removes 23.5 % of the
high-pass energy; Helldivers 2 changes 15.7 % by 2.94/255 and removes 21.2 %. The spatial tier was then simplified to a
plain FXAA on the owner's request (see the 2026-11-26 log entry), which is what these numbers describe. The 1:1 crops show the roofline and crowbar
staircase gone with the sand and grass texture intact, and HUD text unchanged. The model became cheaper at the same time
(0.055 ms at 1080p, five point loads and two colour samples). Presets share the sample distance and differ in the weight:
0.70 / 0.85 / 1.00. The edge threshold and dark-scene floor are internal constants and the "reduce when already smooth"
option was removed: the coverage figure is a density, not a measure of how well edges are resolved, so it is displayed on
Home and never changes the strength.

---

## T1 rebuild: pattern-based morphological filter (2026-11-26)

This is the current spatial tier. It replaces the FXAA-style build above. The contract, settings, page, per-stage GPU,
load-order token and statistics fields were kept; only the filter and its benchmark checks changed.

### Why the rebuild

The FXAA-style build reduced the strongest scanline step to 0.42-0.65 of its input on synthetic edges and changed
15.7-17.7 % of pixels on the owner's real frames, but the owner reported three times that it was not visible in Black
Mesa and Helldivers 2. A flat two-side average changes a wide band of every edge, which both softens texture and still
leaves the shallow staircase read as a step. The rebuild returns to the morphological family (MLAA and SMAA) and makes
the correction geometric: a straight edge run is not touched, and only the staircase end pixels receive coverage.

### Algorithm

1. **Edge mask** (`shaders/aa/aa_edge_ps.hlsl`, RGBA8). Luma is `dot(rgb, (0.299, 0.587, 0.114))` on the gamma-encoded
   capture. A vertical edge is written when `|L(p) - L(p-W)|` exceeds `max(floor, threshold * localMax)`; a horizontal
   edge uses `p-N`. The edge survives only when its delta is at least half the largest delta in the 3x3 neighbourhood
   (the SMAA local-contrast adaptation). This is what keeps weak texture grain next to a silhouette out of the mask.
   The B channel holds a sign-based one-texel-stroke flag: both neighbours on an axis must be on the same side of the
   centre. It survives a textured background, unlike an equality test, and does not fire on a straight half-plane edge,
   where one neighbour is on the centre's side.
2. **Coverage weights** (`aa_weights_ps.hlsl`, RGBA8: north, south, west, east). For each of the pixel's four edges,
   walk the run along the edge until it ends or a crossing edge turns the staircase, up to 8 / 12 / 16 texels for
   Performance / Balanced / Quality. A pixel in the middle of a run gets zero. A pixel at an end gets
   `0.5 / sqrt(runLength)`. The exact area of the triangle cut by the approximating line is `1 / (2 * runLength)`; the
   sqrt falloff is a deliberate perceptual tuning (D14) because the exact area was measured as too subtle on real
   frames. The thin-stroke flag, dilated by one texel, drops all four weights in the protected region, so glyph caps and
   arm/stem junctions are protected as well as the straight sides. Two per-edge far-side tests additionally protect a
   one-texel feature when the pixel being blended is the background beside it.
3. **Blend** (`aa_blend_ps.hlsl`). Read the four weights, pick the largest, and blend the centre towards that neighbour
   with that weight. No 3x3 average and no along-edge taps: the texture on both sides of the edge is never mixed into
   the result. Letterbox pixels are copied bit for bit.
4. **Statistics** (`aa_stats_cs.hlsl`). A 128x128 sample grid counts long, slanted, hard one-pixel steps with a
   two-/four-texel coherence test and a centre-versus-two-side deviation of at least 0.25. The counters are copied to a
   staging ring and read a few frames later without waiting. The figure is a hard-edge density, not a quality score; it
   is shown on Home and never changes the strength.

The filter is original HLSL written from the published MLAA and SMAA descriptions. No SMAA demo code, area texture or
search texture is bundled. `tools/aa_lab/_reference/` holds a local SMAA demo for visual comparison only and is
git-ignored.

### Placement and interactions

The stage remains user-orderable and runs on its own GPU. The default position is still **after upscaling** (the
owner's 2026-11-25 choice), before Smart parallax. At that position it runs once per displayed frame at output
resolution. Before upscaling it runs once per real frame at game resolution and generated frames inherit it; that is
cheaper but filters at the lower resolution. `outputPixelScale` is reserved and unused in this tier.

- **Frame generation:** with the default order, generated frames receive the pass like real frames. Before upscaling,
  the generator interpolates already-filtered frames and no per-generated-frame cost is paid.
- **MoonSR temporal upscaling (ALSR):** after upscaling, the pass cleans the reconstruction's output; before it, the
  pass removes high-frequency input the reconstruction could have used. The user chooses by the load order. There is no
  automatic reduction when ALSR is selected.
- **Smart parallax:** `ComposeAndPresent` applies AA first, so relief shading is not filtered. A custom order that puts
  relief before AA is not honoured; this is the same documented limitation as before.
- **A/B compare:** the left half shows the raw capture, the right half the final image. The edge-mask and blend-weight
  debug views are developer tools reached with `moonsr_cli --aa-view edges|weights`.

### Measurements

Hardware: Ryzen 9 7900X, 31 GB, NVIDIA GeForce RTX 5060 (driver 621.14, UMD 32.0.16.2114), Windows 11 build
26300.9974. Synthetic scenes are 1280x720; timings are the isolated pass plus statistics dispatch, median of eight
timestamp pairs on an otherwise idle GPU. Commands and the full table are in `documentation/BENCHMARKS.md`.

Synthetic checks (Balanced, strength 1.0, text/HUD protection on):

| Check | Result | Bound |
|---|---:|---|
| Slanted edge, strongest scanline step after / before (20 / 30 / 45 / 60 / 70 deg) | 0.579 / 0.437 / 0.501 / 0.500 / 0.584 | <= 0.80 |
| Exactly vertical / horizontal edge, step ratio | 1.000 | >= 0.98 |
| One-pixel line grid, mean luma after / before | 1.000 | >= 0.90 |
| Small text over a 2 px checker, stroke pixels changed / mean luma / max change | 0.00 % / 1.000 / 0 | <= 1 %, >= 0.99, <= 64 |
| 4 px checker, luma stddev after / before | 1.000 | >= 0.95 |
| 4 px checker, high-pass energy after / before | 1.000 | >= 0.90 |
| 2 px checker (informational), stddev / high-pass | 0.998 / 1.001 | not judged |
| Smooth gradient, maximum change | 0 | <= 4/255 |

Cost:

| Resolution | RTX 5060 | RTX A400 | Radeon 610M iGPU | Budget (RTX 5060) |
|---|---:|---:|---:|---:|
| 1920x1080 | 0.152 ms | 1.144 ms | 3.943 ms | 0.3 ms |
| 2560x1440 | 0.280 ms | 1.989 ms | 6.489 ms | 0.6 ms |

The Radeon is a correctness check, not a performance target. All checks pass with the same reported metrics on the
three adapters; no vendor-specific path is used. On a real HD2 frame the isolated pass measures 0.190-0.195 ms; in the
live engine on a 1280x720 -> 1080p test pattern it measures 0.117-0.124 ms with 485 frames, no drops and a clean stop.

Real recordings (120 frames each, `%LOCALAPPDATA%\MoonSR\recordings\AAtests`; isolated pass on the recorded inputs):

| Recorded input | Hard-edge density | Pixels changed | Mean \|d\| | High-pass energy | Ratio | GPU |
|---|---:|---:|---:|---|---:|---:|
| Helldivers 2, AA off, frame 2180 | 1.4 % | 1.52 % | 0.21 | 52.67 -> 52.09 | 0.989 | 0.19 ms |
| Helldivers 2, AA off, frame 2200 | 1.5 % | 1.52 % | 0.21 | 52.62 -> 52.04 | 0.989 | 0.19 ms |
| Black Mesa, AA off, frame 1033 | 1.2 % | 2.89 % | 0.53 | 51.55 -> 49.47 | 0.960 | 0.19 ms |
| Black Mesa, AA off, frame 1050 | 1.1 % | 2.90 % | 0.53 | 51.61 -> 49.54 | 0.960 | 0.19 ms |

The previous FXAA-style build changed 15.7 % (HD2) and 17.7 % (Black Mesa) of pixels by a mean of 2.94 / 2.95 and
removed 21-24 % of the high-pass energy. The new filter touches far fewer pixels but moves each one further where it
acts; the energy column shows it is not a blur. This is not a like-for-like quality comparison: there is no ground
truth and the recordings are not the same camera pose across AA states. The owner's eye decides.

Presets on the same HD2 frame produce identical output because every run is shorter than 8 texels; only the cost
differs: Performance about 0.17 ms, Balanced 0.19 ms, Quality about 0.21 ms. Quality is the setting for shallow (near-horizontal
or near-vertical) staircases whose run exceeds 12 texels.

### Honest limits

- **Still spatial.** It cannot recover detail the game never rendered, and it cannot remove sub-pixel shimmer on moving
  content. DLAA-class results need temporal accumulation with jitter and motion vectors; that is not this tier.
- **Static HUD masking is deferred.** Both protection toggles drive the same geometric thin-stroke guard in T1. A HUD
  stroke that is thicker than one texel, or a translucent/animated overlay, can still be softened. A real static-overlay
  mask needs frame history (or the frame generator's `staticMask`) and belongs to T2.
- **One-texel game features are protected, not anti-aliased.** A thin railing or wire stays crisp even when it is
  jagged. That is the price of leaving text intact without a history mask.
- **Shallow staircases get a small weight.** `0.5 / sqrt(L)` is stronger than the exact area but still modest at
  `L = 16` (0.125). If the owner sees shallow roof lines and curbs still stepping, the next tool is the SMAA-style area
  lookup, not more blend weight.
- **No ground truth and no head-to-head.** No PSNR/SSIM against the game's TAA/DLAA, and the recording pairs are not
  aligned. The synthetic checks are regression guards, not a quality score.
- **Low-end GPUs pay for the fixed passes.** The A400 and the Radeon are 7-23x the RTX 5060 cost. The filter is off by
  default and the Performance preset does not remove the fixed edge/weight/blend passes.

### Decisions recorded by this rebuild

- **D13: Rebuild the spatial tier as a pattern-based morphological filter.** Run-length coverage replaces the flat
  two-side average. The contract, page, settings and load-order stage stay. Accepted: this is the owner's "clean
  rebuild" request, and the result passes every synthetic check the FXAA-style build passed plus the strict text one.
- **D14: Endpoint falloff `0.5 / sqrt(L)`, not the exact `1 / (2L)`.** The exact area changed 1.5 % of HD2 pixels by a
  mean of 0.18 and left shallow steps; the sqrt falloff costs no measured texture or text contrast and corrects the
  shallow case more. Revisit only with the owner's real-game A/B on near-horizontal edges.
- **D15: Text/HUD protection is a sign-based one-texel stroke mask, dilated by one texel, shared by both toggles.**
  Static-overlay masking is a T2 item because it needs history; do not claim it works before it exists.
- **D16: Default position stays after upscaling, before relief.** The owner chose it on 2026-11-25; the stage remains
  orderable and works before upscaling at game resolution.
- **D17: Defaults stay Balanced, strength 100 %, both protections on.** Quality only lengthens the run search. It does
  not change the blend weight, so a review that compares presets on a 45-degree edge will see no difference.
- **D18: Temporal remains deferred.** The frame generator's field is 8-pixel cells. A temporal AA tier needs a
  per-pixel refinement, a ghosting bound on thin features, and a static-HUD mask before it can be offered. Shipping a
  temporal mode on the block field alone would ghost exactly the text and railings this tier protects.

---

## Appendix: spatial options compared for the capture-only path (2026-11-26)

This appendix closes the survey for the current tier. It compares the options that can run on a finished capture, with
no depth, motion vectors, jitter or MSAA samples, on the D3D11 FL 11_0 / SM 5.0 baseline. Costs marked [C] were measured
this session; [E] are estimates and must not be copied into BENCHMARKS.md; [M] is prior knowledge not re-fetched.

### A.1 Options

| Option | Inputs it needs | Structure | Strength | Weakness on captured frames | Licence / use |
|---|---|---|---|---|---|
| **FXAA 3.11** | Luma (one frame) | One pass: luma edge test, gradient orientation, bounded search, blend | Cheapest visible option; simple | Blends the whole edge band; softens 1-px text and fine texture; weak on near-horizontal staircases | NVIDIA GameWorks BSD-3-Clause [M]; original blog release effectively public domain [M]. We did not copy it; the earlier FXAA-style build was clean-room |
| **SMAA 1x** | Luma or colour (one frame) | Three passes: edge mask, blending weights using a precomputed area/search LUT, neighbourhood blend | Better diagonals and thin-shape handling than FXAA; corner detection | Needs the area/search textures (or a generated equivalent); text still softens without a history mask; 2-3x the passes | MIT (Jimenez et al. 2013 demo, including its data) [C: local reference LICENSE]; not bundled |
| **SMAA T2x** | Luma/colour + **velocity + sub-pixel jitter** | SMAA 1x plus temporal resolve | Near-2x supersampling on edges | The capture path has neither jitter nor per-pixel velocity; block flow ghosts thin features | MIT [C]; not applicable to the universal path |
| **CMAA2** | Luma/colour (one frame) | Compute: edge/line queue, local-contrast adaptation, shape processing | Good quality/cost on Intel; handles thin lines | Compute queue and more complex than the 3-pass PS path; cross-vendor cost unverified | MIT (Intel) [M]; not built |
| **MLAA / run-length** | Luma (one frame) | Edge mask, run search along the edge, coverage from the run shape | Straight runs untouched; good text/texture behaviour; no lookup data | Shallow staircases get small coverage; diagonal corners need care | Paper (Reshetov 2009) [M]; **this is the shipped T1**, original HLSL |
| **Edge-directed single pass** (the earlier MoonSR build) | Luma | One pass, N/S vs E/W axis, two-side average | 0.054 ms at 1080p [C] | The owner reported it was not visible in three games; it softens texture and text | Original, superseded |
| **TAA** | Jitter + per-pixel motion vectors (+ depth) | Reproject history, clamp, accumulate | DLAA-like edge stability when the data exists | No jitter or vectors on the capture path; 8-px block flow cannot follow text or railings; ghosting | Common industry practice [M]; research tier only |
| **DLAA** | NVIDIA DLSS runtime + Tensor Cores | Neural temporal resolve | Best-in-class edge stability | Vendor-specific hardware and runtime, forbidden as a requirement; weights/runtime not bundled | Proprietary [M]; not used |

### A.2 Quality vs cost on our budget

The budget is <= 0.3 ms at 1080p and <= 0.6 ms at 1440p on the RTX 5060, one isolated pass at output resolution. The
shipped T1 measures 0.152 / 0.280 ms [C]. The earlier one-pass build measured 0.054 / 0.082 ms [C] but failed the
owner's visibility test. SMAA 1x is estimated at 0.18-0.30 / 0.32-0.55 ms [E] for three passes plus LUT sampling; CMAA2
is unknown [U] and likely similar or higher. Both fit the budget on the RTX 5060 but not on the A400/iGPU without a
quality tier. The T1 result passes every synthetic check at 0.152 ms, so there is headroom for a Directional (SMAA-style)
tier later; it is not justified until the owner's real-game A/B says the current diagonals are insufficient.

### A.3 Text, HUD and fine texture

- FXAA blends every detected edge, including one-texel strokes; the earlier build changed 13 % of the synthetic text
  stroke pixels [C].
- SMAA's corner detection and area lookup reduce corner bleeding, but without a static-overlay mask a glyph corner is
  still a small staircase; measured evidence is not available in-tree.
- The shipped T1 computes a sign-based one-texel-stroke flag in the edge pass, dilates it by one texel in the weights
  pass and drops every coverage weight there. On the text-over-checker stress scene 0 % of stroke pixels change [C].
  One-texel game features are protected for the same reason.
- A 4 px checker keeps 100 % of its luma contrast and high-pass energy; a 2 px checker is effectively preserved
  (0.998 / 1.001) [C]. Fine texture is not used as an AA target.
- Static HUD masking is not in this tier. It needs history (or the frame generator's `staticMask`) and is deferred; the
  two page toggles currently drive the same geometric stroke guard.

### A.4 "Already anti-aliased" detection

- The shipped statistics pass samples a 128x128 grid and counts long, slanted, hard one-pixel steps with a 2/4-texel
  coherence test and a centre-versus-two-side deviation of at least 0.25. On the real recordings it reads 1.1-1.5 %;
  on synthetic slanted edges 0.0-0.1 % [C].
- That is a **hard-edge density**, not a quality score. A frame with one aliased silhouette reads lower than a
  text-heavy frame with smooth text. It is shown on Home and never changes the strength.
- A useful detector would compare one-pixel and two-pixel transition shares, but it needs ground truth to calibrate and
  must never silently disable the filter. The decision remains "display, do not act" (D12, D15).

### A.5 Where the stage belongs

| Position | Runs per | Resolution | Pros | Cons |
|---|---|---|---|---|
| Before upscaling | real frame | game | Cheapest; the upscaler sees clean edges; generated frames inherit it | Cannot fix upscaler ringing; edge width is in game pixels; modifies the generator's input |
| Before frame generation | real frame | game | Generated frames inherit AA; one cost per real frame | Same input concern for the motion estimator; after the current default order it is the same position as "before upscaling" |
| **After upscaling (default)** | displayed frame | output | Cleans the scaler output and residual aliasing at the pixels the viewer sees | Cost multiplies with the frame-generation multiplier; needs an output-resolution pass |
| After frame generation, before upscaling | displayed frame | game | None over "before upscaling" | More calls for the same resolution; excluded by the load-order meaning |

The owner chose **after upscaling** on 2026-11-25. The stage remains orderable; the engine honours the position for
AA and relief except that `ComposeAndPresent` always applies AA before relief. A custom order with relief before AA is
not honoured (documented).

### A.6 Interactions

- **Neural rendering:** if AA runs before NR, the model sees filtered input; if after, AA cleans the model's output.
  The default order is NR, FG, upscaling, AA, relief, so NR sees the original capture.
- **MoonSR temporal upscaling (ALSR):** after upscaling, AA cleans the reconstruction; before, it removes high-frequency
  input the reconstruction could use. There is no automatic reduction when ALSR is selected; the user chooses by order.
- **Smart parallax:** AA runs first, so relief shading is not filtered. Relief derives height from the image; AA before
  relief removes a little of that signal, but relief's band-pass is at a larger scale than the AA correction.
- **A/B compare:** left half is the raw capture, right half the final image. Edge-mask/blend-weight views are CLI tools
  (`moonsr_cli --aa-view edges|weights`); the split view is also A/B compare.

### A.7 Contract as shipped (for owner confirmation)

The contract was already in the tree from the 2026-11-25 approval; this rebuild kept it unchanged. It is repeated here
so the owner can confirm it without reading headers.

```cpp
enum class AaMode : uint32_t { Off = 0, Spatial = 1, Directional = 2, Temporal = 3 };
enum class AaQuality : uint32_t { Performance = 0, Balanced = 1, Quality = 2 };
enum class AaDebugView : uint32_t { None = 0, EdgeMask, BlendWeight, Split };

struct AaSettings {
    AaMode mode = AaMode::Off;          // enable + tier
    AaQuality quality = AaQuality::Balanced;
    float strength = 1.0f;              // 0..1
    float edgeThreshold = 0.10f;        // relative luma contrast, 0.02..0.5
    float edgeThresholdMin = 0.03f;     // absolute floor for dark scenes, 0..0.2
    bool protectText = true;
    bool protectHud = true;
    AaDebugView debugView = AaDebugView::None;
    bool operator==(const AaSettings&) const = default;
};
```

- **Engine:** `EngineConfig::aa`; `Engine::SetAaSettings`; `EngineStats::{aaActive, aaMode, aaEdgeCoverage,
  aaStrength, aaNote, aaGpuMs}`; `PipelineStage::AntiAliasing`; `StageAdapters::antiAliasing`; token `aa` in
  `pipelineOrder`.
- **Persistence:** `[AntiAliasing]` `Enabled`, `Mode`, `Quality`, `StrengthPercent`, `ProtectText`, `ProtectHud`.
  Files without the section load off / Balanced / 100 % / protection on. Old 3- and 4-stage orders load and gain the
  missing stage before relief; `NormalizeOrder` appends, never resets.
- **Placement:** default after upscaling, before relief; user-orderable; per-stage GPU.
- **UI:** enable, method, quality, strength, protect text, protect HUD. The threshold and floor are internal constants
  (the owner removed those sliders on 2026-11-26 because they invited misconfiguration). The debug views are developer
  tools, CLI-only, as requested in the same change.
- **Headless:** `moonsr_cli --aa off|spatial|directional|temporal`, `--aa-strength`, `--aa-quality`,
  `--aa-edge-threshold`, `--aa-edge-floor`, `--aa-no-protect`, `--aa-view`.
- **Off:** the engine does not create `AaPass`; no textures, buffers, queries or shaders for the stage are allocated,
  so the disabled cost is zero.

### A.8 Decisions

| ID | Decision | Status |
|---|---|---|
| D1 | Clean-room spatial filter from published descriptions; no third-party shader code | Shipped |
| D2 | Default position after upscaling, before relief; orderable | Shipped (owner's 2026-11-25 choice) |
| D3 | Load-order migration appends missing stages; explicit `StageAdapters` entry; `aa` token | Shipped |
| D4 | Off by default; opt-in like Smart parallax | Shipped |
| D5 | Spatial, Balanced, strength 1.0, threshold 0.10, floor 0.03 | Shipped; thresholds internal in UI |
| D6 | Measure sharpening interaction; do not silently change sharpness | Open (default order gives AA the scaler's sharpened output) |
| D7 | Text protection on by default, toggle exposed | Shipped |
| D8 | AA together with MoonSR temporal upscaling allowed and noted; no auto-off | Shipped; auto-reduce removed |
| D9 | Temporal tier deferred | Deferred |
| D10 | `src/aa/` owned by upscaling-engineer (algorithms) + shader-engineer (HLSL) | Shipped |
| D11 | Page named "Anti-aliasing"; modes labelled experimental | Shipped |
| D12 | No automatic quality drop; show the measured time | Shipped |
| D13 | Rebuild T1 as pattern-based morphological (run-length coverage) | Shipped |
| D14 | Endpoint falloff `0.5 / sqrt(L)` (exact area is `1/(2L)`) | Shipped; revisit on owner A/B |
| D15 | Text/HUD protection is a sign-based 1-texel stroke mask, dilated 1 texel | Shipped; static HUD mask deferred |
| D16 | Default position stays after upscaling | Shipped |
| D17 | Defaults Balanced, 100 %, protections on; Quality only lengthens the run search | Shipped |
| D18 | Temporal remains deferred until per-pixel motion refinement, a ghosting bound and a static-HUD mask exist | Deferred |

The open decisions are the owner's: D6 (sharpening interaction), D14 (shallow-edge falloff), D18 (temporal), and the
release version bump.
