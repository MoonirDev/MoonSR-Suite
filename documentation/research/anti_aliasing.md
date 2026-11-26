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
- UI: new "Anti-aliasing" page with its own vector glyph, mode/quality/strength/edge threshold/text protection/debug
  view, live apply, Home details row.
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
protection, HUD protection (disabled until T2), auto-reduce, debug view. Home details row "Anti-aliasing" with the
state and the measured GPU time, like the Smart parallax row.

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

The contract in 7.2 was approved as proposed; the owner chose **after upscaling** as the default position (overriding
the recommendation in 3.1), with the pass still orderable. T1 is the spatial tier only; Directional and Temporal are
accepted by the settings but fall back to Spatial with a status reason.

Measurements replace the estimates in 4.4 and 4.5 (full tables and commands: `documentation/BENCHMARKS.md`):

| Check (1280x720 synthetic, RTX 5060) | Result | Proposed threshold (4.4) |
|---|---:|---|
| Slanted edge, strongest scanline step after / before (20-70 deg) | 0.62-0.70 | <= 0.88 |
| Exactly vertical / horizontal edge, max change | 0 | <= 4/255 |
| 2 px checker, high-pass energy after / before | 0.891 | 0.85-1.15 |
| One-pixel lines, mean luma after / before | 1.000 | >= 0.90 |
| Text strokes, changed pixels / max change | 0 / 0 | <= 2 %, <= 8/255 |
| Smooth gradient, max change | 0 | <= 4/255 |

| Cost, isolated pass | 1080p | 1440p |
|---|---:|---:|
| RTX 5060 | 0.072 ms | 0.119 ms |
| RTX A400 | 0.536 ms | 0.816 ms |
| Radeon 610M iGPU | 2.131 ms | 3.235 ms |

The 4.5 estimates (0.10-0.18 ms at 1080p for one pass) were pessimistic by about 2x on the RTX 5060 and too optimistic
for the A400 and the iGPU, which the estimates did not cover.

What remains unverified: every quality claim against a real game, any comparison with FXAA / SMAA / CMAA2 / TAA / DLAA,
and the "already anti-aliased" thresholds. The detector counts hard steps on a 128x128 grid with a long-edge coherence
test, so text and dense textures no longer read as aliased (0.0 %); what it measures is hard-edge density (0.0-0.1 % on
a single synthetic edge, 1.0 % on a text-heavy screenshot), which is not the same as "the edges are already resolved".
Auto-reduce is therefore off by default and its 0.02 % / 0.1 % thresholds are synthetic-only constants. The stroke
guards were tuned on synthetic text; real glyphs and HUDs may still be softened.

### Real-game check (Helldivers 2, 2026-11-26)

The owner recorded four 120-frame sets (game AA off, TAA, DLAA, MoonSR AA on) in the same ship interior. On the no-AA
1080p frame the isolated filter changes 10.95 % of pixels by a mean of 0.70/255 and lowers high-pass energy from 52.68
to 48.85 (0.927); the game's own TAA and DLAA inputs sit much lower (27.3 and 35.5) and change less under the filter
(mean 0.43 and 0.53). 1:1 crops show cleaner slanted silhouettes without halos and no visible loss of fine detail or
HUD text, but the effect is mild: this tier is not a substitute for the game's temporal anti-aliasing. Details:
BENCHMARKS.md "Anti-aliasing on real game frames". Balanced stays the default; Quality (span 1.2, weight 0.62) removes
more staircase at the cost of thin bright features.
